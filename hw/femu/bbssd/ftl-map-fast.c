/*
 * FAST (Fully-Associative Sector Translation, Lee et al. TECS'07) log-block mapping,
 * mapping=fast, on the FEMU_MAP_CLASS_LOG allocator hook. Like the faithful BAST hybrid
 * (ftl-map-fast mirrors ftl-map-hybrid), but with FAST's fully-associative log organization:
 *
 *  - A single SEQUENTIAL-WRITE (SW) log block: a sequential run of overwrites to one data
 *    block lands here; if it fills the block in order it can switch-merge (cheap).
 *  - A SHARED, fully-associative RANDOM-WRITE (RW) log pool: overwrites from ANY data block
 *    land in a common pool of RW log capacity. Because logs are not pinned 1:1 to a data
 *    block (as in BAST), a single data block churning does not force an immediate merge --
 *    full merges are DELAYED until the RW pool fills, at which point a random merge must
 *    reconcile EVERY distinct data block (LBN) that has pages in the pool. That is FAST's
 *    trade: fewer but larger (multi-victim) merges vs BAST's frequent 1:1 merges.
 *
 * Faithfulness: writes physically land in LOG-class blocks (get_new_page_class), and merges
 * PHYSICALLY relocate valid pages into fresh DATA-class blocks (read old, program new,
 * invalidate old, validate new, update L2P/rmap, advance the data write pointer,
 * gc_write_pages++) -- so write amplification is genuine, like the faithful BAST. translate
 * uses the flat L2P (always holds the real log-or-data ppa). State lives behind ssd->map_priv.
 *
 * Simplifications (documented, like BAST): the SW log detects only a strict in-order run; the
 * RW pool is a fixed page budget; the random-merge victim is "all LBNs currently in the RW
 * pool" drained in one bounded pass; no FAST second-chance/partial-merge heuristics. reclaim
 * is budget=1 (one merge pass per drain) to bound per-request latency.
 */
/*
 * [한글 요약] FAST log-block mapping (선택: device property mapping=fast)
 * - hybrid(BAST)처럼 모든 host write(첫 write 포함)가 LOG class에 기록되고,
 *   데이터 정합성은 flat maptbl이 담당한다. 다른 점은 log 구성이다.
 * - SW log 1개: 한 LBN을 offset 0부터 순서대로 쓰는 run만 받는다.
 *   꽉 차면 switch merge(erase 1회, 복사 없음).
 * - RW log pool: 모든 LBN이 공유하는 page 예산(16 block 분량). 다 차면
 *   random merge로 dirty LBN들의 valid page를 DATA class로 복사한다.
 * - BAST 대비: merge는 덜 자주, 대신 한 번에 여러 LBN(multi-victim)을 처리.
 * - reclaim은 write 요청/destage batch 끝에 1번(reclaim_per_page=false),
 *   한 번에 최대 2개 LBN만 merge(FAST_MERGE_LBNS_PER_PASS)해 지연을 제한.
 * - partial merge·second chance 같은 FAST 논문의 최적화는 없다.
 * - 주요 진입: femu_mapping_fast_ops (ssd->mapping->*)
 * - 자세한 설명: 참고자료/code_study/14_mapping_schemes.md
 */
#include "qemu/osdep.h"
#include "ftl.h"
#include "ftl-internal.h"

/* RW pool 크기 / dirty LBN 목록 최대 길이 / reclaim 1회당 merge할 LBN 수 */
#define FAST_RW_LOG_BLOCKS 16   /* shared RW log pool size, in blocks */
#define FAST_MAX_DIRTY_LBNS 4096
#define FAST_MERGE_LBNS_PER_PASS 2  /* victim data blocks merged per reclaim drain (bound) */

/*
 * fast 전용 상태(ssd->map_priv). 아래 merge 카운터는 ssd_hybrid_stats()가
 * hybrid만 집계하므로 현재는 debug_ftl 로그(ftl_log)로만 보인다.
 */
struct femu_map_fast {
    int pgs_per_blk;
    /* shared RW log pool: a page budget + the set of data blocks (LBNs) with pages in it */
    int rw_capacity_pages;   /* total RW log pages before a random merge is forced */
    int rw_used_pages;       /* RW log pages currently in use */
    uint8_t *lbn_dirty;      /* lbn_dirty[lbn] = has pages in the RW pool */
    int64_t *dirty_list;     /* compact list of dirty LBNs (for the merge sweep) */
    int dirty_cnt;
    int64_t nr_lbns;
    /* SW (sequential-write) log: tracks one in-order run */
    int64_t sw_lbn;          /* data block the SW log currently serves; -1 = none */
    int sw_used;             /* pages written into the SW log in order */
    /* stats (exposed via SMART vendor area, same accessors as hybrid) */
    uint64_t switch_merges;
    uint64_t full_merges;
    uint64_t merge_read_pages;
    uint64_t merge_write_pages;
};

static inline struct femu_map_fast *fa(struct ssd *ssd)
{
    return (struct femu_map_fast *)ssd->map_priv;
}

/*
 * fast 상태 할당: RW 예산 = 16 × pgs_per_blk page, dirty 표, SW log 비움.
 * 호출: ssd->mapping->init (ssd_init(), ftl.c)
 */
static void femu_map_fast_init(struct ssd *ssd)
{
    struct ssdparams *spp = &ssd->sp;
    struct femu_map_fast *f = g_malloc0(sizeof(*f));

    f->pgs_per_blk = spp->pgs_per_blk;
    f->rw_capacity_pages = FAST_RW_LOG_BLOCKS * spp->pgs_per_blk;
    f->rw_used_pages = 0;
    f->nr_lbns = spp->tt_pgs / spp->pgs_per_blk + 1;
    f->lbn_dirty = g_malloc0(sizeof(uint8_t) * f->nr_lbns);
    f->dirty_list = g_malloc0(sizeof(int64_t) * FAST_MAX_DIRTY_LBNS);
    f->dirty_cnt = 0;
    f->sw_lbn = -1;
    f->sw_used = 0;
    ssd->map_priv = f;
}

/* translate: flat L2P holds the real ppa (log or data) -- always correct. */
/* 읽기는 page scheme과 같은 maptbl 조회. 호출: ssd->mapping->translate */
static struct ppa femu_map_fast_translate(struct ssd *ssd, uint64_t lpn)
{
    return get_maptbl_ent(ssd, lpn);
}

/*
 * prepare_write: all overwrites go to the LOG class. Signal may_need_reclaim when the
 * shared RW pool is (about to be) full -- the datapath then drains one random merge.
 */
/*
 * 배치 결정: 첫 write든 덮어쓰기든 항상 FEMU_MAP_CLASS_LOG.
 * may_need_reclaim은 세우지만 현재 datapath는 읽지 않는다(merge 여부는
 * 요청 끝의 needs_reclaim()이 결정).
 * 호출: ssd->mapping->prepare_write (ssd_program_lpn())
 */
static struct map_write_plan femu_map_fast_prepare_write(struct ssd *ssd,
                                                         uint64_t lpn, int io_type)
{
    struct femu_map_fast *f = fa(ssd);
    struct map_write_plan plan = { .target_class = FEMU_MAP_CLASS_LOG,
                                   .may_need_reclaim = false };
    (void)lpn; (void)io_type;

    if (f->rw_used_pages >= f->rw_capacity_pages ||
        f->dirty_cnt >= FAST_MAX_DIRTY_LBNS - 1) {
        plan.may_need_reclaim = true;
    }
    return plan;
}

/*
 * commit_write: flat L2P update (data-correct), then account the overwrite into the FAST
 * log model. A strict in-order run to a single data block uses the SW log; anything else
 * goes to the shared RW pool, marking that LBN dirty.
 */
/*
 * flat L2P 갱신 + 이 write를 SW log 또는 RW pool에 기록한다.
 * 호출: ssd->mapping->commit_write (ssd_program_lpn())
 * 흐름: ① 옛 PPA invalid, maptbl/rmap을 new_ppa로 (page scheme과 동일)
 *       ② SW log가 잡은 LBN의 다음 offset이면 SW run 연장
 *       ③ SW log가 비어 있고 offset 0이면 이 LBN으로 새 SW run 시작
 *       ④ 그 밖에는 RW pool: LBN을 dirty 목록에 넣고 rw_used_pages++
 * 참고: SW log는 꽉 차서 switch merge될 때만 비워진다. 그 전에는 다른
 *       LBN의 순차 write도 RW pool로 간다.
 */
static void femu_map_fast_commit_write(struct ssd *ssd, uint64_t lpn,
                                       struct ppa *new_ppa)
{
    struct femu_map_fast *f = fa(ssd);
    int64_t lbn = lpn / f->pgs_per_blk;
    int off = lpn % f->pgs_per_blk;
    struct ppa old = get_maptbl_ent(ssd, lpn);

    /* ① 데이터 정합성: page scheme과 똑같은 flat 갱신 */
    if (mapped_ppa(&old)) {
        mark_page_invalid(ssd, &old);
        set_rmap_ent(ssd, INVALID_LPN, &old);
    }
    set_maptbl_ent(ssd, lpn, new_ppa);
    set_rmap_ent(ssd, lpn, new_ppa);

    /* SW log: continue an in-order run to the same data block */
    /* ② SW run 연장 */
    if (lbn == f->sw_lbn && off == f->sw_used) {
        f->sw_used++;
        return;
    }
    /* ③ 새 SW run 시작 (offset 0부터여야 switch merge가 가능) */
    if (f->sw_lbn < 0 && off == 0) {
        f->sw_lbn = lbn;
        f->sw_used = 1;
        return;
    }

    /* otherwise -> shared RW pool */
    /* ④ RW pool: 같은 LPN을 여러 번 써도 쓸 때마다 예산 1 page 소모 */
    if (lbn >= 0 && lbn < f->nr_lbns && !f->lbn_dirty[lbn] &&
        f->dirty_cnt < FAST_MAX_DIRTY_LBNS) {
        f->lbn_dirty[lbn] = 1;
        f->dirty_list[f->dirty_cnt++] = lbn;
    }
    f->rw_used_pages++;
}

/*
 * merge가 필요한가: ① RW pool 예산 소진 ② SW log 꽉 참
 * ③ dirty LBN 목록이 거의 꽉 참 — 하나라도 해당하면 true.
 * 호출: ssd->mapping->needs_reclaim (ssd_write() 요청 끝,
 *       ssd_buffer_destage() batch 끝)
 */
static bool femu_map_fast_needs_reclaim(struct ssd *ssd)
{
    struct femu_map_fast *f = fa(ssd);

    return (f->rw_used_pages >= f->rw_capacity_pages) ||
           (f->sw_lbn >= 0 && f->sw_used >= f->pgs_per_blk) ||
           (f->dirty_cnt >= FAST_MAX_DIRTY_LBNS - 1);
}

/* merge NAND 명령 1건을 GC_IO로 timeline에 반영. stime=0 → 현재 시각 기준 */
static uint64_t fast_charge(struct ssd *ssd, struct ppa *ppa, int cmd)
{
    struct nand_cmd c = { .type = GC_IO, .cmd = cmd, .stime = 0 };
    return ssd_advance_status(ssd, ppa, &c);
}

/*
 * Physically relocate one data block's valid pages into fresh DATA-class pages.
 * *relocated (if non-NULL) receives the number of valid pages actually moved, so
 * the caller can decrement the RW-pool page counter by the real count rather than
 * a fixed block's worth (sparse LBNs hold fewer than pgs_per_blk pages).
 */
/*
 * LBN 하나 merge: valid page마다 read → DATA class에 program, maptbl/rmap
 * 갱신, gc_write_pages++ (WAF에 반영). 옛 page는 invalid로 남아 line GC가
 * 회수한다. 호출: femu_map_fast_reclaim()
 */
static uint64_t fast_merge_one_lbn(struct ssd *ssd, int64_t lbn, int *relocated)
{
    struct femu_map_fast *f = fa(ssd);
    uint64_t base_lpn = (uint64_t)lbn * f->pgs_per_blk;
    uint64_t lat = 0;
    int moved = 0;

    for (int off = 0; off < f->pgs_per_blk; off++) {
        uint64_t lpn = base_lpn + off;
        struct ppa old = get_maptbl_ent(ssd, lpn);
        if (!mapped_ppa(&old) || !valid_ppa(ssd, &old)) {
            continue;
        }
        /*
         * Allocate before invalidating the old copy: with nowhere to put the
         * merged page, invalidating first would drop the only copy of the
         * data on the floor.
         */
        struct ppa new = get_new_page_class(ssd, FEMU_MAP_CLASS_DATA);
        if (!mapped_ppa(&new)) {
            break;
        }
        lat += fast_charge(ssd, &old, NAND_READ);
        mark_page_invalid(ssd, &old);
        set_rmap_ent(ssd, INVALID_LPN, &old);

        set_maptbl_ent(ssd, lpn, &new);
        set_rmap_ent(ssd, lpn, &new);
        mark_page_valid(ssd, &new);
        lat += fast_charge(ssd, &new, NAND_WRITE);
        ssd_advance_write_pointer_class(ssd, FEMU_MAP_CLASS_DATA);
        ssd->gc_write_pages++;
        f->merge_read_pages++;
        f->merge_write_pages++;
        moved++;
    }
    if (relocated) {
        *relocated = moved;
    }
    return lat;
}

/*
 * reclaim (one bounded pass per drain): a FAST random merge reconciles EVERY data block
 * that currently has pages in the shared RW pool (the multi-victim cost), physically
 * relocating their valid pages into data blocks; then the RW pool is empty. The SW log, if
 * it filled in order, switch-merges (cheap). This is FAST's "fewer but larger merges".
 */
/*
 * merge 1 pass. budget은 쓰지 않는다.
 * 호출: ssd->mapping->reclaim (ssd_write(), ssd_buffer_destage())
 * 흐름: ① SW log가 꽉 찼으면 switch merge: erase 1회 charge, 복사 없음
 *       ② RW random merge: dirty 목록 뒤에서부터 최대 2개 LBN을
 *          fast_merge_one_lbn()으로 옮기고 dirty 해제
 *       ③ rw_used_pages -= 옮긴 page 수. dirty LBN이 하나도 없으면 0으로
 * 반환: merge NAND 명령 지연의 합(ns)
 */
static uint64_t femu_map_fast_reclaim(struct ssd *ssd, int budget)
{
    struct femu_map_fast *f = fa(ssd);
    uint64_t lat = 0;
    (void)budget; /* one full random-merge pass; inherently bounded by dirty_cnt */

    /* SW switch merge: a fully in-order SW log promotes in place (no copy) */
    /* ① SW switch merge */
    if (f->sw_lbn >= 0 && f->sw_used >= f->pgs_per_blk) {
        /*
         * hybrid와 달리 별도 data block anchor가 없어, erase는 LBN offset 0이
         * 지금 가리키는 PPA(보통 SW run의 첫 page) 위치에 charge된다.
         */
        struct ppa anchor = get_maptbl_ent(ssd, (uint64_t)f->sw_lbn * f->pgs_per_blk);
        if (mapped_ppa(&anchor)) {
            lat += fast_charge(ssd, &anchor, NAND_ERASE);
        }
        f->switch_merges++;
        f->sw_lbn = -1;
        f->sw_used = 0;
    }

    /*
     * RW random merge, bounded: relocate up to FAST_MERGE_LBNS_PER_PASS dirty data blocks
     * per drain (pop from the tail of the dirty list), leaving the rest dirty for the next
     * drain. FAST's random merge is multi-victim, but relocating ALL dirty LBNs in one call
     * can be thousands of page copies -> a latency spike that starves the guest; bounding the
     * per-pass victim count keeps each reclaim's cost in check (a faithfulness/latency
     * tradeoff -- the merge is still real and multi-victim, just throttled). Each freed LBN
     * returns ~one block of RW capacity.
     */
    /* ② RW random merge (pass당 최대 FAST_MERGE_LBNS_PER_PASS개 LBN) */
    int merged = 0;
    while (f->dirty_cnt > 0 && merged < FAST_MERGE_LBNS_PER_PASS) {
        int relocated = 0;
        int64_t lbn = f->dirty_list[--f->dirty_cnt];
        lat += fast_merge_one_lbn(ssd, lbn, &relocated);
        if (lbn >= 0 && lbn < f->nr_lbns) {
            f->lbn_dirty[lbn] = 0;
        }
        /* decrement by the pages actually relocated; a sparse LBN holds fewer
         * than a full block, and subtracting a fixed pgs_per_blk would underflow
         * the unsigned counter and wedge reclaim/pool-capacity tracking. */
        /* ③ RW 예산 반환 */
        if (f->rw_used_pages >= (uint64_t)relocated) {
            f->rw_used_pages -= relocated;
        } else {
            f->rw_used_pages = 0;
        }
        merged++;
    }
    /* 몇 개 LBN을 옮겼든 random merge pass 1회 = full_merges 1 증가 */
    if (merged > 0) {
        f->full_merges++;          /* one (bounded) random-merge pass */
    }
    /* if the pool is drained of dirty LBNs, the counter must be zero -- guards
     * against a stale count leaving needs_reclaim permanently true. */
    if (f->dirty_cnt == 0) {
        f->rw_used_pages = 0;
    }

    if (ssd->debug_ftl) {
        ftl_log("FAST merges: switch=%lu random=%lu merge_wr_pages=%lu gc_wr=%lu\n",
                (unsigned long)f->switch_merges, (unsigned long)f->full_merges,
                (unsigned long)f->merge_write_pages,
                (unsigned long)ssd->gc_write_pages);
    }
    return lat;
}

/*
 * page scheme과 같은 maptbl/rmap 갱신. SW/RW 모델은 건드리지 않는다.
 * 호출: ssd->mapping->gc_relocate_commit (gc_write_page(), ftl-line-gc.c)
 */
static void femu_map_fast_gc_relocate_commit(struct ssd *ssd, uint64_t lpn,
                                             struct ppa *old_ppa, struct ppa *new_ppa)
{
    (void)old_ppa;
    set_maptbl_ent(ssd, lpn, new_ppa);
    set_rmap_ent(ssd, lpn, new_ppa);
}

/*
 * flat unmap만 한다. RW 예산·dirty 표는 줄이지 않는다.
 * 호출: ssd->mapping->trim (ssd_deallocate_lpns())
 */
static void femu_map_fast_trim(struct ssd *ssd, uint64_t lpn)
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
static void femu_map_fast_exit(struct ssd *ssd)
{
    struct femu_map_fast *f = ssd->map_priv;

    if (!f) {
        return;
    }
    g_free(f->lbn_dirty);
    g_free(f->dirty_list);
    g_free(f);
    ssd->map_priv = NULL;
}

/*
 * fast vtable. uses_log_class=true(log_wp용 line 예약), reclaim_per_page는
 * 기본 false → merge는 요청/batch 끝에서만 검사된다.
 * 호출: femu_mapping_scheme_lookup()이 femu_mapping_extra[]에서 찾는다.
 */
const struct femu_mapping_ops femu_mapping_fast_ops = {
    .exit           = femu_map_fast_exit,
    .uses_log_class = true,
    .name               = "fast",
    .uses_cmt           = false,
    .init               = femu_map_fast_init,
    .translate          = femu_map_fast_translate,
    .prepare_write      = femu_map_fast_prepare_write,
    .commit_write       = femu_map_fast_commit_write,
    .needs_reclaim      = femu_map_fast_needs_reclaim,
    .reclaim            = femu_map_fast_reclaim,
    .gc_relocate_commit = femu_map_fast_gc_relocate_commit,
    .trim               = femu_map_fast_trim,
};
