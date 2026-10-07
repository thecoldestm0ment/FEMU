/*
 * Hybrid log-block mapping with BAST's one-log-per-logical-block association.
 * Each page reaching NAND consumes a fresh log slot, even when its logical
 * offset was already written. NAND cannot overwrite a programmed page; neither
 * overwrite invalidation nor trim restores a slot. Only merging resets it.
 * See Lee et al., "A Log Buffer-Based Flash Translation Layer Using
 * Fully-Associative Sector Translation", sections 2.2 and 2.4, for BAST's
 * append and merge rules; section 3 describes FAST's shared-log alternative.
 *
 * FEMU routes initial writes as well as overwrites through sixteen logical
 * logs. After each program it merges the fullest log if a log fills or the
 * pool is occupied, breaking ties by slot order. An in-order full log switches
 * without copying; a full merge copies each currently mapped page of the
 * logical block once, including surviving data from earlier merges.
 *
 * This is a merge-cost model, not physical BAST block placement: LOG and DATA
 * have separate line write pointers, but logical logs share physical lines.
 * Full merges really relocate pages and charge gc_write_pages; switch merges
 * charge an erase without physically exchanging blocks. Retain the data
 * block's physical erase anchor separately from L2P, replacing it only on a
 * merge; overwrites and trim must not remove the old block's erase cost.
 * Initial logs also incur one switch erase. Physical line GC is
 * separate and can add copies. Reads use the flat L2P.
 */
/*
 * [한글 요약] BAST식 hybrid log-block mapping의 merge 비용 모델
 * - 선택: device property mapping=hybrid
 * - 모든 host write(첫 write 포함)는 LOG class(ssd->log_wp)에 기록된다.
 * - 논리 block(LBN = lpn / pgs_per_blk)마다 log block을 최대 1개(1:1) 붙이며,
 *   log pool은 16개(HYBRID_DEFAULT_LOG_BLOCKS)로 고정이다.
 * - program마다(reclaim_per_page) 검사해 log가 꽉 차거나 pool이 다 차면
 *   가장 많이 찬 log를 merge한다:
 *   switch merge(offset 순서대로 꽉 참 → erase 1회만, 복사 없음)
 *   full merge(그 LBN의 valid page를 전부 DATA class로 복사 → WAF 증가)
 * - partial merge는 없다: 순서대로 쓰다 덜 찬 log도 full merge로 처리된다.
 * - 데이터 정합성은 flat maptbl이 담당하고 log 구조는 merge 판정·비용용이다.
 * - 주요 진입: femu_mapping_hybrid_ops (ssd->mapping->*), ssd_hybrid_stats()
 * - 자세한 설명: 참고자료/code_study/14_mapping_schemes.md
 */
#include "qemu/osdep.h"
#include "ftl.h"
#include "ftl-internal.h"

/* one log block per data block (BAST 1:1); a small fixed pool of active log blocks. */
#define HYBRID_DEFAULT_LOG_BLOCKS 16

/* log block 1개의 모델 상태(실제 물리 block이 아니라 회계용 slot) */
struct hybrid_log {
    int64_t lbn;          /* data block this log serves; -1 = free */
    struct ppa anchor;    /* first program, retained even if trimmed */
    int used;             /* programs, including invalidated pages */
    int seq;              /* pages still in sequential order (offset i at slot i) */
};

/* hybrid 전용 상태. ssd->map_priv에 들어간다 */
struct femu_map_hybrid {
    int pgs_per_blk;      /* pages per (data/log) block = BAST merge unit */
    int nr_logs;          /* size of the log-block pool */
    struct hybrid_log *logs;
    /* per-data-block -> index into logs[], or -1 if no open log block */
    int *lbn_to_log;
    int64_t nr_lbns;
    struct ppa *data_blocks; /* erase identity independent of live L2P */
    /* counters (exposed via ftl_log / debug) */
    uint64_t switch_merges;
    uint64_t full_merges;
    uint64_t merge_erases;
    uint64_t merge_read_pages;
    uint64_t merge_write_pages;
};

static inline struct femu_map_hybrid *hy(struct ssd *ssd)
{
    return (struct femu_map_hybrid *)ssd->map_priv;
}

/*
 * hybrid 상태 할당: log pool(16개, 모두 free=-1), LBN→log 색인,
 * LBN별 data block erase anchor(data_blocks[], 처음엔 UNMAPPED).
 * 호출: ssd->mapping->init (ssd_init(), ftl.c)
 */
static void femu_map_hybrid_init(struct ssd *ssd)
{
    struct ssdparams *spp = &ssd->sp;
    struct femu_map_hybrid *h = g_malloc0(sizeof(*h));

    h->pgs_per_blk = spp->pgs_per_blk;
    h->nr_logs = HYBRID_DEFAULT_LOG_BLOCKS;
    h->logs = g_malloc0(sizeof(struct hybrid_log) * h->nr_logs);
    for (int i = 0; i < h->nr_logs; i++) {
        h->logs[i].lbn = -1;
    }
    h->nr_lbns = spp->tt_pgs / spp->pgs_per_blk + 1;
    h->lbn_to_log = g_malloc0(sizeof(int) * h->nr_lbns);
    h->data_blocks = g_new(struct ppa, h->nr_lbns);
    for (int64_t i = 0; i < h->nr_lbns; i++) {
        h->lbn_to_log[i] = -1;
        h->data_blocks[i].ppa = UNMAPPED_PPA;
    }
    ssd->map_priv = h;
}

/*
 * translate: data correctness comes from the flat L2P (maptbl), exactly like the page
 * scheme -- the log/data distinction in this overlay is for merge accounting, not for
 * where the latest data physically lives. So a read is the standard maptbl lookup.
 */
/* 읽기는 page scheme과 같은 maptbl 조회. 호출: ssd->mapping->translate */
static struct ppa femu_map_hybrid_translate(struct ssd *ssd, uint64_t lpn)
{
    return get_maptbl_ent(ssd, lpn);
}

/* find an existing log for lbn, or claim a free one; -1 if the pool is full. */
/*
 * LBN에 이미 붙은 log 번호를 주거나, 없으면 free slot을 새로 붙인다.
 * 호출: femu_map_hybrid_commit_write()
 */
static int hybrid_get_log(struct femu_map_hybrid *h, int64_t lbn)
{
    /* defensive: lbn derives from lpn; out-of-range would index lbn_to_log[] OOB.
     * The datapath rejects out-of-range LPNs, but guard the mapping too. */
    if (lbn < 0 || lbn >= h->nr_lbns) {
        return -1;
    }
    if (h->lbn_to_log[lbn] >= 0) {
        return h->lbn_to_log[lbn];
    }
    for (int i = 0; i < h->nr_logs; i++) {
        if (h->logs[i].lbn < 0) {
            h->logs[i].lbn = lbn;
            h->logs[i].used = 0;
            h->logs[i].seq = 0;
            h->lbn_to_log[lbn] = i;
            return i;
        }
    }
    return -1; /* pool full -> caller must reclaim first */
}

/*
 * prepare_write: route an overwrite into this LBN's log block. If the LBN has no open log
 * and the pool is full, signal may_need_reclaim so the datapath drains a merge first.
 */
/*
 * 배치 결정: 첫 write든 덮어쓰기든 항상 FEMU_MAP_CLASS_LOG.
 * 이 LBN에 log가 없고 pool도 꽉 찼으면 may_need_reclaim=true를 세우지만,
 * 현재 datapath는 이 필드를 읽지 않는다(merge는 program 직후
 * needs_reclaim()으로 결정된다).
 * 호출: ssd->mapping->prepare_write (ssd_program_lpn())
 */
static struct map_write_plan femu_map_hybrid_prepare_write(struct ssd *ssd,
                                                           uint64_t lpn, int io_type)
{
    struct femu_map_hybrid *h = hy(ssd);
    int64_t lbn = lpn / h->pgs_per_blk;
    struct map_write_plan plan = { .target_class = FEMU_MAP_CLASS_LOG,
                                   .may_need_reclaim = false };
    (void)io_type;

    if (h->lbn_to_log[lbn] < 0) {
        /* would need a fresh log block; if none free, ask for reclaim */
        bool have_free = false;
        for (int i = 0; i < h->nr_logs; i++) {
            if (h->logs[i].lbn < 0) { have_free = true; break; }
        }
        if (!have_free) {
            plan.may_need_reclaim = true;
        }
    }
    return plan;
}

/*
 * commit_write: update the flat L2P exactly as the page scheme (keeps data correct), and
 * record the overwrite into the LBN's log model to drive merge classification. An offset
 * written out of sequence breaks the switch-merge eligibility for this log block.
 */
/*
 * flat L2P 갱신(page scheme과 동일) + 이 LBN의 log 모델에 program 1회 기록.
 * 호출: ssd->mapping->commit_write (ssd_program_lpn())
 * 흐름: ① 옛 PPA invalid, maptbl/rmap을 new_ppa로
 *       ② LBN의 log 확보(hybrid_get_log). 첫 program이면 log anchor 기록
 *       ③ data_blocks[lbn]이 비어 있으면 new_ppa를 erase anchor로
 *       ④ offset == used(다음 순서 slot)면 seq++ → switch merge 자격 유지
 *       ⑤ used++ : 같은 offset을 다시 써도 slot은 계속 소모된다
 */
static void femu_map_hybrid_commit_write(struct ssd *ssd, uint64_t lpn,
                                         struct ppa *new_ppa)
{
    struct femu_map_hybrid *h = hy(ssd);
    int64_t lbn = lpn / h->pgs_per_blk;
    int off = lpn % h->pgs_per_blk;
    struct ppa old = get_maptbl_ent(ssd, lpn);

    /* ① 데이터 정합성: page scheme과 똑같은 flat 갱신 */
    if (mapped_ppa(&old)) {
        mark_page_invalid(ssd, &old);
        set_rmap_ent(ssd, INVALID_LPN, &old);
    }
    set_maptbl_ent(ssd, lpn, new_ppa);
    set_rmap_ent(ssd, lpn, new_ppa);

    /* ② pool이 꽉 차 log를 못 받으면(li<0) 모델 기록만 건너뛴다 */
    int li = hybrid_get_log(h, lbn);
    if (li >= 0) {
        struct hybrid_log *lg = &h->logs[li];
        if (!lg->used) {
            lg->anchor = *new_ppa;
        }
        /* Initial logs also charge one switch erase in this cost model. */
        /* ③ 옛 data block이 없는 첫 log도 switch 때 erase 1회를 내게 된다 */
        if (!mapped_ppa(&h->data_blocks[lbn])) {
            h->data_blocks[lbn] = *new_ppa;
        }
        /* sequential iff this write lands at the next in-order slot */
        /* ④ offset i가 i번째 slot에 와야 순차. 한 번 어긋나면 switch 불가 */
        if (lg->used == off) {
            lg->seq++;
        }
        /* ⑤ NAND는 덮어쓸 수 없으므로 매 program이 새 slot 하나 */
        lg->used++;
    }
}

/*
 * merge가 필요한가: ① used >= pgs_per_blk인(꽉 찬) log가 있거나
 * ② pool의 모든 slot이 사용 중이면 true.
 * 호출: ssd->mapping->needs_reclaim (ssd_program_lpn()이 page마다 검사)
 */
static bool femu_map_hybrid_needs_reclaim(struct ssd *ssd)
{
    struct femu_map_hybrid *h = hy(ssd);

    for (int i = 0; i < h->nr_logs; i++) {
        if (h->logs[i].lbn >= 0 && h->logs[i].used >= h->pgs_per_blk) {
            return true; /* a full log block needs merging */
        }
    }
    /* also reclaim when the pool is exhausted (every slot in use) */
    for (int i = 0; i < h->nr_logs; i++) {
        if (h->logs[i].lbn < 0) {
            return false;
        }
    }
    return true;
}

/* charge one NAND op of `cmd` on `ppa` to the timeline (GC-class, like real merge I/O). */
/* merge NAND 명령 1건을 GC_IO로 timeline에 반영. stime=0 → 현재 시각 기준 */
static uint64_t hybrid_charge(struct ssd *ssd, struct ppa *ppa, int cmd)
{
    struct nand_cmd c = { .type = GC_IO, .cmd = cmd, .stime = 0 };
    return ssd_advance_status(ssd, ppa, &c);
}

/*
 * reclaim: merge one log block (BAST). Switch merge if the log filled sequentially
 * (in-order overwrite of the whole block) -> ~free, just an erase of the old data block.
 * Otherwise a full merge -> read every valid page of the merge unit and rewrite it,
 * charging reads+writes (the WAF cost) plus the erases. Picks the fullest log block.
 */
/*
 * log block을 budget개 merge한다(datapath는 budget=1로 부른다).
 * 호출: ssd->mapping->reclaim (ssd_program_lpn(), needs_reclaim()이 true일 때)
 * 흐름: ① victim = used가 가장 큰 log (동률이면 앞 slot)
 *       ② seq == pgs_per_blk(전부 순서대로) → switch merge:
 *          옛 data block anchor에 erase 1회만 charge, page 복사 없음
 *       ③ 아니면 full merge: LBN의 valid page마다 read → DATA class에
 *          program, maptbl/rmap 갱신, gc_write_pages++ (WAF에 반영)
 *       ④ log slot 해제 (lbn=-1, used/seq=0)
 * 반환: merge NAND 명령 지연의 합(ns)
 */
static uint64_t femu_map_hybrid_reclaim(struct ssd *ssd, int budget)
{
    struct femu_map_hybrid *h = hy(ssd);
    uint64_t lat = 0;

    for (int n = 0; n < budget; n++) {
        /* victim = fullest in-use log block */
        /* ① victim 선택 */
        int vi = -1, vmax = -1;
        for (int i = 0; i < h->nr_logs; i++) {
            if (h->logs[i].lbn >= 0 && h->logs[i].used > vmax) {
                vmax = h->logs[i].used;
                vi = i;
            }
        }
        if (vi < 0) {
            break;
        }
        struct hybrid_log *lg = &h->logs[vi];
        int64_t lbn = lg->lbn;
        uint64_t base_lpn = (uint64_t)lbn * h->pgs_per_blk;

        /* ② switch merge 조건: 모든 program이 offset 순서대로 들어왔다 */
        bool switch_ok = (lg->seq >= h->pgs_per_blk);
        if (switch_ok) {
            /* switch merge: log block becomes the data block; old data block erased.
             * cost ~ one block erase, no page copies. */
            struct ppa anchor = h->data_blocks[lbn];
            lat += hybrid_charge(ssd, &anchor, NAND_ERASE);
            h->merge_erases++;
            h->data_blocks[lbn] = lg->anchor;
            h->switch_merges++;
        } else {
            /*
             * ③ full merge: 비용 = valid page 수 × (read + program).
             * erase는 따로 charge하지 않는다. 옛 page는 invalid로 남고
             * 나중에 line GC가 erase한다(merge_erases는 switch에서만 증가).
             */
            bool have_data = false;

            /*
             * Copy each live logical page to DATA-class space. Invalidating
             * its previous mapping leaves physical reclamation to line GC;
             * the modeled log slot is released below.
             */
            for (int off = 0; off < h->pgs_per_blk; off++) {
                uint64_t lpn = base_lpn + off;
                struct ppa old = get_maptbl_ent(ssd, lpn);
                if (!mapped_ppa(&old) || !valid_ppa(ssd, &old)) {
                    continue;
                }
                /*
                 * Allocate before invalidating the old copy: with nowhere to
                 * put the merged page, invalidating first would drop the only
                 * copy of the data on the floor.
                 */
                struct ppa new = get_new_page_class(ssd, FEMU_MAP_CLASS_DATA);
                if (!mapped_ppa(&new)) {
                    break;
                }
                /* 다음 switch merge가 erase할 anchor = 이번 첫 복사 page */
                if (!have_data) {
                    h->data_blocks[lbn] = new;
                    have_data = true;
                }
                lat += hybrid_charge(ssd, &old, NAND_READ);   /* read valid page */
                mark_page_invalid(ssd, &old);
                set_rmap_ent(ssd, INVALID_LPN, &old);

                set_maptbl_ent(ssd, lpn, &new);
                set_rmap_ent(ssd, lpn, &new);
                mark_page_valid(ssd, &new);
                lat += hybrid_charge(ssd, &new, NAND_WRITE);  /* program merged page */
                ssd_advance_write_pointer_class(ssd, FEMU_MAP_CLASS_DATA);
                ssd->gc_write_pages++;       /* WAF: a merge-relocated page */
                h->merge_read_pages++;
                h->merge_write_pages++;
            }
            h->full_merges++;
        }

        /* ④ slot 반납: 이 LBN의 다음 write는 새 log를 받는다 */
        /* free the log block */
        h->lbn_to_log[lbn] = -1;
        lg->lbn = -1;
        lg->used = 0;
        lg->seq = 0;
    }

    /* debug_ftl=on이면 merge 누적 64회 단위로 카운터를 로그로 남긴다 */
    if (ssd->debug_ftl && (h->switch_merges + h->full_merges) % 64 == 0) {
        ftl_log("HYBRID merges: switch=%lu full=%lu merge_wr_pages=%lu gc_wr=%lu\n",
                (unsigned long)h->switch_merges, (unsigned long)h->full_merges,
                (unsigned long)h->merge_write_pages,
                (unsigned long)ssd->gc_write_pages);
    }

    return lat;
}

/* gc_relocate_commit: identical to the page scheme (line GC relocates a valid page; the
 * overlay's log model is unaffected -- merges are driven by host overwrites, not GC). */
/*
 * page scheme과 같은 maptbl/rmap 갱신. log 모델(used/seq)은 건드리지 않는다.
 * 호출: ssd->mapping->gc_relocate_commit (gc_write_page(), ftl-line-gc.c)
 */
static void femu_map_hybrid_gc_relocate_commit(struct ssd *ssd, uint64_t lpn,
                                               struct ppa *old_ppa,
                                               struct ppa *new_ppa)
{
    (void)old_ppa;
    set_maptbl_ent(ssd, lpn, new_ppa);
    set_rmap_ent(ssd, lpn, new_ppa);
}

/* Trim changes logical validity, not the number of programmed log slots. */
/*
 * flat unmap만 하고 log의 used는 줄이지 않는다(slot은 이미 program됨).
 * 호출: ssd->mapping->trim (ssd_deallocate_lpns(): DSM deallocate,
 *       Write Zeroes+DEAC, Format 등)
 */
static void femu_map_hybrid_trim(struct ssd *ssd, uint64_t lpn)
{
    struct ppa old = get_maptbl_ent(ssd, lpn);

    if (mapped_ppa(&old)) {
        mark_page_invalid(ssd, &old);
        set_rmap_ent(ssd, INVALID_LPN, &old);
        old.ppa = UNMAPPED_PPA;
        set_maptbl_ent(ssd, lpn, &old);
    }
}

/* map_priv 해제. 호출: ssd->mapping->exit (ssd_free(), ftl.c) */
static void femu_map_hybrid_exit(struct ssd *ssd)
{
    struct femu_map_hybrid *h = ssd->map_priv;

    if (!h) {
        return;
    }
    g_free(h->logs);
    g_free(h->lbn_to_log);
    g_free(h->data_blocks);
    g_free(h);
    ssd->map_priv = NULL;
}

/*
 * hybrid vtable. uses_log_class=true → log_wp용 line 예약,
 * reclaim_per_page=true → ssd_program_lpn()이 page마다 needs_reclaim() 검사.
 * 호출: femu_mapping_scheme_lookup()이 femu_mapping_extra[]에서 찾는다.
 */
const struct femu_mapping_ops femu_mapping_hybrid_ops = {
    .exit           = femu_map_hybrid_exit,
    .uses_log_class = true,
    .reclaim_per_page = true,
    .name               = "hybrid",
    .uses_cmt           = false,
    .init               = femu_map_hybrid_init,
    .translate          = femu_map_hybrid_translate,
    .prepare_write      = femu_map_hybrid_prepare_write,
    .commit_write       = femu_map_hybrid_commit_write,
    .needs_reclaim      = femu_map_hybrid_needs_reclaim,
    .reclaim            = femu_map_hybrid_reclaim,
    .gc_relocate_commit = femu_map_hybrid_gc_relocate_commit,
    .trim               = femu_map_hybrid_trim,
};

/* Add only hybrid costs; physical line GC is accounted separately. */
/*
 * hybrid merge 카운터를 FEMU stats vendor log(0xC0) 집계에 더한다.
 * hybrid가 아니면(fast 포함) 아무것도 더하지 않는다.
 * 호출: nvme_collect_media_stats() (nvme-admin.c)
 */
void ssd_hybrid_stats(struct ssd *ssd, uint64_t *switches,
                      uint64_t *full, uint64_t *erases)
{
    struct femu_map_hybrid *h;

    if (ssd->mapping != &femu_mapping_hybrid_ops || !ssd->map_priv) {
        return;
    }
    h = hy(ssd);
    *switches += h->switch_merges;
    *full += h->full_merges;
    *erases += h->merge_erases;
}
