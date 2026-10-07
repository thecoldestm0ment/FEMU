/*
 * bbssd FTL datapath: the non-FDP host read / write / trim handlers run by the
 * FTL thread. They translate an LBA request into page mapping lookups and
 * updates, drive the NAND media timing, and trigger GC when the device runs low
 * on free lines. Split out of ftl.c; mapping, line/GC, timing, and the debug
 * hooks come from ftl-internal.h and the media layer.
 */
/*
 * [이 파일의 역할]
 *   FTL thread가 실행하는 bbssd host datapath. NVMe Read / Write /
 *   Write Zeroes / DSM(trim) 요청을 LPN(논리 page) 단위로 쪼개서
 *   maptbl 조회·갱신, write buffer, NAND 지연 계산, foreground GC를
 *   처리한다. (ssd_read()와 Flush는 FDP 모드에서도 이 파일을 쓰고,
 *   write / write zeroes / trim은 non-FDP 전용이다)
 * [주요 진입 함수]
 *   ssd_read(), ssd_write(), ssd_write_zeroes(), ssd_trim()
 *   ssd_buffer_destage()   : write buffer → NAND (Flush 등)
 *   bbssd_drain_namespace(), bbssd_flush_all(), bbssd_power_loss()
 *   bbssd_deallocate_all() : Format/Sanitize 때 전체 unmap
 * [의존 관계]
 *   부르는 쪽: bb_ftl_process_req()·ssd_copy() (ftl.c), nvme-io.c,
 *             nvme-admin.c, femu.c
 *   부르는 것: mapping scheme ops (ftl-map*.c), line/GC (ftl-line-gc.c),
 *             NAND 지연 (ftl-media.c), DFTL CMT (ftl-map-cmt.c),
 *             read cache (ftl-cache.c)
 * 자세한 설명: 참고자료/code_study/12_datapath_read_write_trim.md
 */
#include "qemu/osdep.h"
#include "ftl.h"
#include "ftl-internal.h"

/*
 * Order buffer entries by logical page number. Compared rather than
 * subtracted: a page number is 64-bit and the difference between two of them
 * does not fit the int this has to return, so subtracting misorders the tree
 * for pages far enough apart and loses entries that are still in it.
 */
/*
 * wb_tree(GTree)의 key 비교 함수: LPN 오름차순.
 * 호출: GLib 내부 (ssd_init_write_pointer()의 g_tree_new(comp_buffer))
 */
int comp_buffer(const void *a, const void *b)
{
    uint64_t la = ((const struct buffer_entry *)a)->lpn;
    uint64_t lb = ((const struct buffer_entry *)b)->lpn;

    if (la < lb) {
        return -1;
    }
    return la > lb;
}

/*
 * The write buffer models a DRAM staging area in front of the NAND: a host
 * write is accepted into it and only programmed later, so repeated writes to
 * the same page cost one program rather than several, and the program cost is
 * charged to whichever later event forces the page out.
 *
 * Normally it holds only page numbers. With power_loss enabled, payload
 * access also runs on the FTL thread. Each dirty entry saves the previous
 * durable bytes and per-LBA validity before the backend changes. Removing an
 * entry after programming commits those bytes; a power cut restores them.
 * This undo log consumes at most buffer_size pages plus their LBA state,
 * instead of duplicating the entire backend. FUA programs a full NAND page,
 * so dirty neighboring LBAs in that page become durable too.
 *
 * Everything below runs on the FTL thread, which is the only owner of the
 * buffer, the mapping, the line management and the NAND status. None of it is
 * serialised against the poller thread and none of it may be called from there.
 */
/*
 * [write buffer 구조]
 *   write_buffer(QTAILQ): 오래전에 쓴 LPN이 앞에 오는 LRU 순서 list
 *   wb_tree(GTree): LPN으로 entry를 찾는 index (같은 buffer_entry 공유)
 *   write_buffer_cnt: 점유 page 수. entry의 undo/lba_state는 power_loss용
 *
 * buffer가 지금 새 write를 받아도 되는지 판단한다.
 * 호출: ssd_write() (buffer 경로 진입 조건, direct 경로의 drain 여부)
 * 반환: false = buffer_size <= 0 또는 wb_tree 없음, power_loss인데 vwc=0,
 *       host가 Set Features(Volatile Write Cache)로 cache를 끈 경우
 */
static bool buffer_enabled(struct ssd *ssd)
{
    if ((ssd->n->power_loss && !ssd->n->vwc) ||
        ssd->sp.buffer_size <= 0 || !ssd->wb_tree) {
        return false;
    }

    /*
     * A controller that advertises a volatile write cache lets the host turn
     * it off (Set Features, Volatile Write Cache). A device whose cache is
     * disabled may not hold writes back, so the buffer stops accepting them
     * and ssd_write() drains what it still holds before going direct. A
     * controller that advertises no cache has nothing for the host to disable
     * and keeps buffering, which is what buffer_size alone has always meant.
     */
    if (ssd->n && ssd->n->vwc && !ssd->n->features.volatile_wc) {
        return false;
    }

    return true;
}

/* fill level at which the buffer starts writing pages back */
/* watermark(page 수) = buffer_size × buffer_thres_pcent(기본 0.9), 최소 1 */
static int buffer_watermark(struct ssd *ssd)
{
    int wm = (int)(ssd->sp.buffer_size * ssd->sp.buffer_thres_pcent);

    /*
     * A threshold that rounds down to zero would write every page back as soon
     * as it arrived; a buffer of any size holds at least one page first.
     */
    return wm > 0 ? wm : 1;
}

/* 점유 page 수가 watermark 이상인가. 호출: ssd_write() */
static bool buffer_at_watermark(struct ssd *ssd)
{
    return ssd->write_buffer_cnt >= buffer_watermark(ssd);
}

/*
 * How many pages to write back once the watermark is reached. The batch is the
 * slack above it, so a buffer set to start writing back at 90% full moves 10%
 * of its pages each time it gets there, and the fill level oscillates in that
 * band instead of tracking the watermark exactly. Writing back in batches is
 * most of what makes a buffer worth modelling: pages that leave together can be
 * spread over channels and LUNs, which one-in-one-out eviction cannot do.
 */
/*
 * destage 1회 분량 = buffer_size - watermark (예: 100 page, 90% → 10 page)
 * 호출: ssd_write()
 */
static int buffer_destage_batch(struct ssd *ssd)
{
    int batch = ssd->sp.buffer_size - buffer_watermark(ssd);

    return batch > 0 ? batch : 1;
}

/* take the least recently written page out of the buffer */
/*
 * QTAILQ 맨 앞(가장 오래전에 쓴 LPN)을 꺼내고 entry·undo를 해제한다.
 * 호출: ssd_buffer_destage() — 꺼낸 LPN은 곧바로 ssd_program_lpn()으로 간다
 */
static bool buffer_select_victim(struct ssd *ssd, uint64_t *lpn)
{
    struct buffer_entry *victim = QTAILQ_FIRST(&ssd->write_buffer);

    if (!victim) {
        return false;
    }

    *lpn = victim->lpn;
    QTAILQ_REMOVE(&ssd->write_buffer, victim, b_entry);
    g_tree_remove(ssd->wb_tree, victim);
    g_free(victim->undo);
    g_free(victim->lba_state);
    g_free(victim);
    ssd->write_buffer_cnt--;

    return true;
}

/*
 * Accept a page into the buffer. Returns true when the page was already held,
 * which is the case the buffer exists for: the earlier write is superseded and
 * no extra program is owed. A page that is already held keeps its identity and
 * is only moved to the tail, so occupancy cannot grow on a repeat write.
 */
/*
 * LPN을 buffer에 넣는다. 이미 있으면 QTAILQ tail로만 옮긴다.
 * 호출: ssd_write() (buffer 경로)
 * 반환: true = write hit (이미 있던 LPN이라 추가 program이 없음)
 */
static bool buffer_insert(struct ssd *ssd, uint64_t lpn)
{
    struct buffer_entry target = { .lpn = lpn };
    struct buffer_entry *held = g_tree_lookup(ssd->wb_tree, &target);
    struct buffer_entry *entry;

    if (held) {
        QTAILQ_REMOVE(&ssd->write_buffer, held, b_entry);
        QTAILQ_INSERT_TAIL(&ssd->write_buffer, held, b_entry);
        return true;
    }

    entry = g_new0(struct buffer_entry, 1);
    entry->lpn = lpn;
    g_tree_insert(ssd->wb_tree, entry, entry);
    QTAILQ_INSERT_TAIL(&ssd->write_buffer, entry, b_entry);
    ssd->write_buffer_cnt++;

    return false;
}

/*
 * Drop a page from the buffer without programming it. A page the host has
 * deallocated must be removed: otherwise the pending program runs later and
 * data the host discarded reappears.
 */
/*
 * LPN의 entry를 program 없이 버린다 (undo도 함께 해제).
 * 호출: ssd_write(), ssd_write_zeroes(), ssd_deallocate_lpns(),
 *       bbssd_power_loss()
 */
static void buffer_discard(struct ssd *ssd, uint64_t lpn)
{
    struct buffer_entry target = { .lpn = lpn };
    struct buffer_entry *held;

    if (!ssd->wb_tree) {
        return;
    }

    held = g_tree_lookup(ssd->wb_tree, &target);
    if (!held) {
        return;
    }

    QTAILQ_REMOVE(&ssd->write_buffer, held, b_entry);
    g_tree_remove(ssd->wb_tree, held);
    g_free(held->undo);
    g_free(held->lba_state);
    g_free(held);
    ssd->write_buffer_cnt--;
}

/* a read served from the buffer does not reach the media */
/* wb_tree 조회만 한다. 호출: ssd_read(), ssd_write() */
static bool buffer_hit(struct ssd *ssd, uint64_t lpn)
{
    struct buffer_entry target = { .lpn = lpn };

    return g_tree_lookup(ssd->wb_tree, &target) != NULL;
}

/*
 * Cost of touching the buffer rather than the media, for both a write it
 * accepts and a read it answers. Derived from the page read time the same way
 * the read cache derives its own hit latency, so the two agree on what DRAM
 * costs relative to NAND on this device.
 *
 * This is a duration, not a completion time: every latency the datapath returns
 * is added to the request's expire_time by the FTL thread.
 */
/*
 * buffer(DRAM) 접근 지연 = pg_rd_lat / 16 (기본 40000ns → 2500ns),
 * pg_rd_lat가 0이면 1000ns. 호출: ssd_read() (hit), ssd_write() (수락)
 */
static uint64_t buffer_hit_lat(struct ssd *ssd)
{
    uint64_t lat = ssd->sp.pg_rd_lat ? ssd->sp.pg_rd_lat / 16 : 1000;

    /* a configured page read time under 16ns would otherwise round to free */
    return lat > 0 ? lat : 1;
}

/*
 * Release the buffer. Safe only because femu_exit() joins the FTL thread first;
 * the pages it still owes are meaningless once the volatile backing store goes.
 */
/* 모든 entry와 wb_tree를 해제한다. 호출: ssd_free() (ftl.c) */
void ssd_free_write_buffer(struct ssd *ssd)
{
    struct buffer_entry *entry;

    while ((entry = QTAILQ_FIRST(&ssd->write_buffer)) != NULL) {
        QTAILQ_REMOVE(&ssd->write_buffer, entry, b_entry);
        g_free(entry->undo);
        g_free(entry->lba_state);
        g_free(entry);
    }
    ssd->write_buffer_cnt = 0;
    if (ssd->wb_tree) {
        g_tree_destroy(ssd->wb_tree);
        ssd->wb_tree = NULL;
    }
}

/* Save the last durable bytes once, then apply only this write's LBAs. */
/*
 * power_loss 모드에서 host data(req->write_data)를 backend에 반영한다.
 * (power_loss가 아니면 write_data가 NULL이라 바로 반환)
 * 호출: ssd_write() (buffered=true: buffer 경로, false: direct 경로)
 * 흐름: ① 이 LPN page와 요청 LBA 범위의 겹침 [first, end) 계산
 *       → ② buffered면 첫 write 때만 undo(옛 page 내용 + LBA 상태) 저장
 *       → ③ memcpy + nvme_mark_written()
 */
static void buffer_write_data(struct ssd *ssd, NvmeRequest *req, uint64_t lpn,
                              bool buffered)
{
    NvmeNamespace *ns = req->ns;
    uint32_t page_size = ssd_page_size(ssd);
    uint8_t idx = NVME_ID_NS_FLBAS_INDEX(ns->id_ns.flbas);
    uint8_t shift = ns->id_ns.lbaf[idx].lbads;
    /* ① page_start: namespace 기준 byte offset, [first, end)만 복사한다 */
    uint64_t page_start = lpn * page_size - ns->backend_offset;
    uint64_t first = MAX(page_start, req->slba << shift);
    uint64_t end = MIN(page_start + page_size,
                       (req->slba + req->nlb) << shift);
    uint8_t *base = (uint8_t *)ssd->n->mbe->logical_space + ns->backend_offset;

    if (!req->write_data) {
        return;
    }
    /* ② undo는 entry당 1번: power cut 때 되돌릴 "마지막 durable 내용" */
    if (buffered) {
        struct buffer_entry target = { .lpn = lpn };
        struct buffer_entry *entry = g_tree_lookup(ssd->wb_tree, &target);
        uint32_t count = page_size >> shift;
        uint64_t slba = page_start >> shift;

        if (!entry->undo) {
            entry->undo = g_memdup2(base + page_start, page_size);
            entry->lba_state = g_malloc(count);
            for (uint32_t i = 0; i < count; i++) {
                entry->lba_state[i] = test_bit(slba + i, ns->util) |
                    (test_bit(slba + i, ns->uncorrectable) << 1);
            }
        }
    }
    /* ③ backend 갱신 + LBA를 written(util)으로, uncorrectable은 해제 */
    memcpy(base + first, req->write_data + first - (req->slba << shift),
           end - first);
    nvme_mark_written(ns, first >> shift, (end - first) >> shift);
}

/* The caller is the FTL thread or an admin handler with processing paused. */
/*
 * power_loss 모드에서 이 namespace의 write buffer를 모두 destage한다.
 * 호출: nvme_dsm(), nvme_copy(), nvme_write_zeros(), nvme_write_uncor()
 *       (nvme-io.c), nvme_format_namespace() (nvme-admin.c)
 *       — I/O layer가 backend를 직접 바꾸기 전에 부른다
 * 반환: 다 못 비우면(line 부족) NVME_CAP_EXCEEDED, *lat에는 지연을 max로
 */
uint16_t bbssd_drain_namespace(NvmeNamespace *ns, uint64_t stime, int64_t *lat)
{
    int64_t elapsed;

    if (!ns->ctrl->power_loss || !ns->ssd) {
        return NVME_SUCCESS;
    }
    elapsed = ssd_buffer_destage(ns->ssd, 0, stime);
    if (lat) {
        *lat = MAX(*lat, elapsed);
    }
    return ns->ssd->write_buffer_cnt ? NVME_CAP_EXCEEDED | NVME_DNR :
                                      NVME_SUCCESS;
}

/* Admin callers hold both pollers and the FTL thread paused. */
/*
 * 모든 namespace의 write buffer를 destage한다.
 * 호출: nvme_set_feature() (VWC off), nvme_sanitize() (nvme-admin.c),
 *       nvme_write_bar() (CC.SHN shutdown, femu.c) — 모두 power_loss일 때만
 */
uint16_t bbssd_flush_all(FemuCtrl *n)
{
    uint64_t now = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);

    for (uint32_t i = 0; i < n->namespace_limit; i++) {
        NvmeNamespace *ns = &n->namespaces[i];

        if (ns->allocated && ns->ssd) {
            ssd_buffer_destage(ns->ssd, 0, now);
            if (ns->ssd->write_buffer_cnt) {
                return NVME_CAP_EXCEEDED | NVME_DNR;
            }
        }
    }
    return NVME_SUCCESS;
}

/* Called with the controller stopped; old mappings still name durable pages. */
/*
 * 전원 차단 시뮬레이션: buffer에 남은(아직 program 안 된) write를 되돌린다.
 * 호출: femu_simulate_power_loss() ("simulate-power-loss" QOM property)
 * 흐름: ① entry마다 undo를 backend에 복원 → ② LBA 상태 복원 + read cache
 *       무효화 → ③ 모든 entry를 program 없이 폐기
 */
void bbssd_power_loss(NvmeNamespace *ns)
{
    struct ssd *ssd = ns->ssd;
    struct buffer_entry *entry;
    uint32_t page_size = ssd_page_size(ssd);
    uint8_t idx = NVME_ID_NS_FLBAS_INDEX(ns->id_ns.flbas);
    uint8_t shift = ns->id_ns.lbaf[idx].lbads;
    uint8_t *base = (uint8_t *)ssd->n->mbe->logical_space + ns->backend_offset;

    /* ① entry마다 옛 page 내용(undo)을 backend에 되돌린다 */
    QTAILQ_FOREACH(entry, &ssd->write_buffer, b_entry) {
        uint64_t offset = entry->lpn * page_size - ns->backend_offset;
        uint64_t slba = offset >> shift;

        if (!entry->undo) {
            continue;
        }
        memcpy(base + offset, entry->undo, page_size);
        /* ② LBA별 bit0 = util(written), bit1 = uncorrectable */
        for (uint32_t i = 0; i < page_size >> shift; i++) {
            if (entry->lba_state[i] & 1) {
                set_bit(slba + i, ns->util);
            } else {
                clear_bit(slba + i, ns->util);
            }
            if (entry->lba_state[i] & 2) {
                set_bit(slba + i, ns->uncorrectable);
            } else {
                clear_bit(slba + i, ns->uncorrectable);
            }
        }
        rcache_invalidate(ssd, entry->lpn);
    }
    /* ③ program 없이 폐기 (maptbl은 원래부터 옛 page를 가리키고 있다) */
    while ((entry = QTAILQ_FIRST(&ssd->write_buffer))) {
        buffer_discard(ssd, entry->lpn);
    }
}

/*
 * Queue the line holding a page that has just been read for a rewrite if
 * read stress or retention age calls for one.
 */
/*
 * 방금 읽은 page의 line을 rewrite(read reclaim) 후보로 등록할지 본다.
 * 호출: ssd_read() (single-plane), ssd_mp_charge() (multi-plane read)
 * 흐름: ① block read_cnt ≥ read_reclaim_limit → read disturb로 등록
 *       → ② line이 닫힌 지 retention_limit_sec 이상 → retention으로 등록
 *       후보는 한 번에 1개(read_reclaim_line)이고, 실제 rewrite는
 *       ssd_write()가 부르는 do_read_reclaim()이 한다
 */
static void ssd_read_check_wear(struct ssd *ssd, struct ppa *ppa,
                                uint64_t stime)
{
    struct ssdparams *spp = &ssd->sp;

    /*
     * Enough reads against this block to be worth refreshing the line it
     * belongs to. Only note it here -- the rewrite happens on a write,
     * where relocation already costs something, rather than stalling this
     * read behind a whole line of it. One line is queued at a time, which
     * is the rate limit.
     */
    /* ① block read_cnt: NAND read마다 +1 (ftl-media.c), erase 때 0 */
    if (spp->read_reclaim_limit && !ssd->read_reclaim_line &&
        get_blk(ssd, ppa)->read_cnt >= (uint64_t)spp->read_reclaim_limit) {
        ssd->read_reclaim_line = get_line(ssd, ppa);
        ssd->reclaim_by_age = false;
    }

    /*
     * Charge leaks out of a cell whether or not anything reads it, so data
     * that has merely sat programmed long enough is refreshed as well. A
     * line's close_time is when it was filled; one still being written has
     * none yet and cannot be old. Queued here and rewritten on a write for
     * the same reason as read stress: that is where relocation belongs.
     */
    /* ② close_time과 stime은 ns 시각, limit은 초 → ns로 바꿔 비교 */
    if (spp->retention_limit_sec && !ssd->read_reclaim_line) {
        struct line *aged = get_line(ssd, ppa);
        uint64_t limit = (uint64_t)spp->retention_limit_sec *
                         NANOSECONDS_PER_SECOND;

        if (aged->close_time && stime > aged->close_time &&
            stime - aged->close_time >= limit) {
            ssd->read_reclaim_line = aged;
            ssd->reclaim_by_age = true;
        }
    }
}

/*
 * Multi-plane commands. NAND can program or read the same page of one block
 * on several planes of a LUN with one command: the per-plane command,
 * address and data phases still take turns on the channel, but the array
 * work runs once on all the planes. Placement is unchanged; pages are held
 * per LUN until they stop forming such a group, then charged together.
 */
/*
 * [multi-plane batching] mp_program / mp_read 옵션 (pls_per_lun > 1일 때만)
 *   LUN마다 batch 2개(mp_prog, mp_rd)를 두고, 같은 stime·같은
 *   ch/LUN/blk/pg·서로 다른 plane인 page를 모아 명령 1개로 지연을 계산한다.
 *   charge 시점: plane 수만큼 찼을 때, 안 맞는 page가 왔을 때,
 *   요청 끝이나 GC 직전(ssd_mp_flush()).
 *   write pointer가 ch → LUN → plane → page 순으로 전진하므로, 연속
 *   write는 nchs × luns_per_ch page마다 같은 LUN의 다음 plane에 놓인다.
 */
/* FDP shares this read path but keeps its own timing, so it is left alone */
/* multi-plane read 사용 여부. 호출: ssd_read(), ssd_mp_flush() */
static inline bool ssd_mp_reads(struct ssd *ssd)
{
    return ssd->sp.mp_read && !ssd->fdp_enabled;
}

/*
 * ppa가 batch b에 합쳐질 수 있는가: 같은 stime, 같은 ch/LUN/blk/pg,
 * batch에 아직 없는 plane이어야 한다. 호출: ssd_mp_add()
 */
static bool ssd_mp_fits(struct ssd_mp_batch *b, struct ppa *ppa,
                        uint64_t stime)
{
    struct ppa *first = &b->ppa[0];
    int i;

    if (b->stime != stime || first->g.ch != ppa->g.ch ||
        first->g.lun != ppa->g.lun || first->g.blk != ppa->g.blk ||
        first->g.pg != ppa->g.pg) {
        return false;
    }
    for (i = 0; i < b->n; i++) {
        if (b->ppa[i].g.pl == ppa->g.pl) {
            return false;
        }
    }
    return true;
}

/* charge a LUN's held pages as one command and empty the batch */
/*
 * batch를 multi-plane 명령 1개로 charge하고 비운다.
 * 호출: ssd_mp_add(), ssd_mp_flush()
 * 반환: b->stime 기준 지연(ns). read면 page마다 ssd_read_check_wear()도 함
 */
static uint64_t ssd_mp_charge(struct ssd *ssd, struct ssd_mp_batch *b,
                              int cmd)
{
    struct nand_cmd ncmd;
    uint64_t lat;
    int i;

    if (!b->n) {
        return 0;
    }
    ncmd.type = USER_IO;
    ncmd.cmd = cmd;
    ncmd.stime = b->stime;
    lat = ssd_advance_status_multiplane(ssd, b->ppa, b->n, &ncmd);
    if (cmd == NAND_READ) {
        for (i = 0; i < b->n; i++) {
            ssd_read_check_wear(ssd, &b->ppa[i], b->stime);
        }
    }
    b->n = 0;
    return lat;
}

/* hold one page for a multi-plane command; returns what any charge cost */
/*
 * page 하나를 그 LUN의 batch에 보류한다.
 * 호출: ssd_program_lpn() (NAND_WRITE), ssd_read() (NAND_READ)
 * 흐름: ① 기존 batch와 안 맞으면 먼저 charge → ② batch에 추가
 *       → ③ plane 수만큼 차면 바로 charge
 * 반환: 이번 호출에서 charge된 지연의 max (보류만 했으면 0)
 */
static uint64_t ssd_mp_add(struct ssd *ssd, struct ppa *ppa, uint64_t stime,
                           int cmd)
{
    struct nand_lun *lun = get_lun(ssd, ppa);
    struct ssd_mp_batch *b = cmd == NAND_READ ? &lun->mp_rd : &lun->mp_prog;
    uint64_t lat = 0;

    /* ① 다른 page/stime의 batch가 남아 있으면 그것부터 charge */
    if (b->n && !ssd_mp_fits(b, ppa, stime)) {
        lat = ssd_mp_charge(ssd, b, cmd);
    }
    /* ② 빈 batch면 이 page의 stime이 batch의 기준 시각이 된다 */
    if (!b->n) {
        b->stime = stime;
    }
    b->ppa[b->n++] = *ppa;
    /* ③ 모든 plane이 찼으면 더 기다릴 이유가 없다 */
    if (b->n == ssd->sp.pls_per_lun) {
        lat = MAX(lat, ssd_mp_charge(ssd, b, cmd));
    }
    return lat;
}

/* charge every held page of one kind; returns the longest latency */
/*
 * 모든 LUN의 보류 batch(read 또는 program 중 한 종류)를 charge한다.
 * 호출: ssd_read(), ssd_write(), ssd_write_zeroes(), ssd_buffer_destage(),
 *       ssd_program_lpn(), ssd_gc_until_clear()
 * 반환: 가장 긴 지연(ns). 해당 multi-plane 옵션이 꺼져 있으면 0
 */
static uint64_t ssd_mp_flush(struct ssd *ssd, int cmd)
{
    struct ssdparams *spp = &ssd->sp;
    uint64_t lat = 0;
    int ch, lun;

    if (!(cmd == NAND_READ ? ssd_mp_reads(ssd) : spp->mp_program)) {
        return 0;
    }
    for (ch = 0; ch < spp->nchs; ch++) {
        for (lun = 0; lun < spp->luns_per_ch; lun++) {
            struct nand_lun *l = &ssd->ch[ch].lun[lun];

            lat = MAX(lat, ssd_mp_charge(ssd, cmd == NAND_READ ?
                                         &l->mp_rd : &l->mp_prog, cmd));
        }
    }
    return lat;
}

/*
 * Program one logical page: choose placement through the mapping scheme,
 * commit it, and charge the media. Shared by the ordinary write path and by
 * the eviction of a buffered page, so both cost the same.
 */
/*
 * LPN 하나를 새 물리 page에 기록한다(out-of-place write).
 * 호출: ssd_write(), ssd_buffer_destage(), ssd_write_zeroes()
 * 흐름: ① mapping scheme이 배치 class 결정 → ② 새 PPA 할당
 *       → ③ maptbl/rmap 갱신(이전 PPA는 여기서 invalid) → ④ write pointer 전진
 *       → ⑤ NAND program 지연 계산
 * 반환: 이 program이 끝나는 시각까지의 지연(ns)
 *       (mp_program이면 이 page는 batch에 보류되고, 지연은 batch charge 때)
 */
static uint64_t ssd_program_lpn(struct ssd *ssd, uint64_t lpn, uint64_t stime,
                               int stream)
{
    struct map_write_plan plan;
    struct nand_cmd swr;
    struct ppa ppa;
    uint64_t lat;

    /* EXP_LOG 계열: 데이터 잔존 실험용 디버그 로그 (env로 켬, 기본 꺼짐) */
    /* log the overwrite before commit_write invalidates the old mapping */
    if (exp_lpn_watched(lpn)) {
        struct ppa old = ssd->mapping->translate(ssd, lpn);
        if (mapped_ppa(&old))
            EXP_LOG("[INVALIDATE:overwrite] lpn=%lu old " PPA_FMT "\n",
                    lpn, PPA_ARG(&old));
    }

    /* mapping scheme picks placement (page/dftl: data class, no reclaim) */
    /* ① page 매핑: 기본은 DATA, hot_cold_sep=on이면 덮어쓰기 LPN은 HOT class */
    /*    (dftl도 같은 규칙, hybrid/fast scheme은 항상 LOG class) */
    plan = ssd->mapping->prepare_write(ssd, lpn, USER_IO);

    /* the page content changes: drop any stale read-cache entry for it */
    rcache_invalidate(ssd, lpn);

    /* allocate from the class the scheme asked for and commit the mapping */
    /* ② 새 PPA: stream write면 stream 전용 line, 아니면 class별 line */
    if (stream >= 0) {
        /* Sub-SWS writes share their FTL page and line with its other LBAs. */
        ppa = ssd_stream_page(ssd, stream,
                              ssd->n->stream_slots[stream].tag);
    } else {
        ppa = get_new_page_class(ssd, plan.target_class);
    }
    /* ③ commit_write: 이전 PPA invalid + maptbl/rmap 갱신 (모든 scheme 공통) */
    ssd->mapping->commit_write(ssd, lpn, &ppa);

    mark_page_valid(ssd, &ppa);
    ssd->nand_write_pages++; /* write amplification: a user page programmed */
    /* only log + track pages that carry the marker string */
    if (femu_dbg_lpn_has_secret(ssd, lpn)) {
        exp_watch_lpn_add(lpn);
        exp_watch_blk[ppa.g.blk] = 1;
        EXP_LOG("[WRITE] lpn=%lu -> " PPA_FMT " (secret)\n",
                lpn, PPA_ARG(&ppa));
    }

    /* need to advance the write pointer here */
    /* ④ line을 다 쓰면 그 line은 full/victim 쪽으로, 다음 free line을 연다 */
    if (stream >= 0) {
        ssd_stream_advance(ssd, stream);
    } else {
        ssd_advance_write_pointer_class(ssd, plan.target_class);
    }

    /* ⑤ mp_program이면 LUN batch에 보류, 아니면 바로 지연 계산 */
    if (ssd->sp.mp_program) {
        lat = ssd_mp_add(ssd, &ppa, stime, NAND_WRITE);
    } else {
        swr.type = USER_IO;
        swr.cmd = NAND_WRITE;
        swr.stime = stime;
        lat = ssd_advance_status(ssd, &ppa, &swr);
    }
    /* A full logical log cannot accept another program in this command. */
    /* hybrid(reclaim_per_page=true)만 해당: log가 차면 page마다 즉시 merge */
    if (ssd->mapping->reclaim_per_page &&
        ssd->mapping->needs_reclaim(ssd)) {
        uint64_t merge_lat;

        /* the merge reads pages this command may still hold */
        lat = MAX(lat, ssd_mp_flush(ssd, NAND_WRITE));
        merge_lat = ssd->mapping->reclaim(ssd, 1);
        lat = MAX(lat, merge_lat);
    }
    return lat;
}

/*
 * Force collection while free lines sit at the watermark, or until it stalls.
 * Held programs are charged first, since they were issued before the
 * collection. Returns what that charge cost.
 */
/*
 * free line 수가 gc_thres_lines_high 이하인 동안 foreground GC를 반복한다.
 * 호출: ssd_buffer_destage(), ssd_write(), ssd_write_zeroes()
 * 반환: GC 전에 보류 program batch를 charge한 지연(ns). GC 자체 비용은
 *       반환값에 없고, (enable_gc_delay면) LUN busy 시간으로 이후 I/O에 반영
 */
static uint64_t ssd_gc_until_clear(struct ssd *ssd)
{
    uint64_t lat = 0;

    if (should_gc_high(ssd)) {
        lat = ssd_mp_flush(ssd, NAND_WRITE);
    }
    while (should_gc_high(ssd)) {
        if (do_gc(ssd, true) == -1) {
            break;
        }
    }
    return lat;
}

/*
 * Write buffered pages back to the media. This is the one place a buffered page
 * becomes a programmed one, and it owns everything that owning a host write
 * entails: GC backpressure, the mapping translation cost, the program itself,
 * and the mapping scheme's own reclaim. The direct write path charges the same
 * things, so a page costs the same whether it was buffered or not -- only when
 * it is charged differs.
 *
 * budget caps how many pages one call may program, or is 0 for "all of them".
 * The returned latency is charged to whichever event forced the progress: a
 * host write that found the buffer full, a flush, or teardown of the cache.
 */
/*
 * write buffer의 page를 LRU 순서로 꺼내 NAND에 program한다.
 * 호출: ssd_write() (watermark 도달, cache off), bb_ftl_process_req()
 *       (NVMe Flush), bbssd_drain_namespace(), bbssd_flush_all()
 * 흐름: ① free line 확인 → ② foreground GC → ③ 재확인 후 victim LPN 꺼냄
 *       → ④ (dftl) cmt_touch + ssd_program_lpn()  (①~④를 budget만큼 반복)
 *       → ⑤ multi-plane flush → ⑥ (fast) batch당 reclaim 1회
 * 반환: stime 기준 지연의 max(ns). budget=0이면 buffer를 끝까지 비운다
 */
uint64_t ssd_buffer_destage(struct ssd *ssd, int budget, uint64_t stime)
{
    uint64_t maxlat = 0, curlat;
    uint64_t lpn;
    int done = 0;

    while (budget <= 0 || done < budget) {
        /* ① 공간이 없으면 victim을 꺼내지 않고 멈춘다 */
        /*
         * Tested before a victim is chosen: selecting one removes it from the
         * buffer, and a page taken out but not programmed would be lost from
         * the occupancy count for nothing.
         */
        if (ssd_out_of_lines(ssd)) {
            break;
        }

        /* ② program할수록 free line이 줄어 page마다 GC 필요 여부를 본다 */
        /*
         * Free space is consumed as pages are programmed, not as they are
         * accepted, so a long write-back has to keep checking rather than rely
         * on the single check the host write already made.
         */
        curlat = ssd_gc_until_clear(ssd);
        maxlat = (curlat > maxlat) ? curlat : maxlat;
        /* ③ GC 후 다시 확인하고, 그다음에야 victim을 buffer에서 꺼낸다 */
        /*
         * The collection above can consume the last line, so ask again, and
         * only then take a page out of the buffer: one taken out with nowhere
         * to program it would never reach the media or the mapping.
         */
        if (ssd_out_of_lines(ssd)) {
            break;
        }

        if (!buffer_select_victim(ssd, &lpn)) {
            break;
        }

        /* ④ victim LPN 하나를 program (dftl이면 매핑 캐시 비용 포함) */
        /* dftl: demand-cache translation cost (no-op when disabled) */
        if (ssd->cmt.capacity) {
            curlat = cmt_touch(ssd, lpn, stime, true);
            maxlat = (curlat > maxlat) ? curlat : maxlat;
        }

        curlat = ssd_program_lpn(ssd, lpn, stime, -1);
        maxlat = (curlat > maxlat) ? curlat : maxlat;
        done++;
    }
    /* ⑤ 보류된 multi-plane program을 모두 charge */
    curlat = ssd_mp_flush(ssd, NAND_WRITE);
    maxlat = (curlat > maxlat) ? curlat : maxlat;

    /* ⑥ fast처럼 reclaim_per_page가 아닌 scheme만 (page/dftl은 hook 없음) */
    /*
     * Schemes without per-page reclaim get one merge per batch: for a
     * log-block scheme that is a merge, and its NAND cost is charged inside
     * reclaim(). page and dftl have no reclaim and skip this.
     */
    if (done && !ssd->mapping->reclaim_per_page &&
        ssd->mapping->needs_reclaim && ssd->mapping->reclaim &&
        ssd->mapping->needs_reclaim(ssd)) {
        curlat = ssd->mapping->reclaim(ssd, 1);
        maxlat = (curlat > maxlat) ? curlat : maxlat;
    }

    return maxlat;
}

/*
 * NVMe Read(와 Copy의 source read)를 LPN 단위로 처리해 지연을 구한다.
 * 호출: bb_ftl_process_req() (NVME_CMD_READ, FDP 모드 포함), ssd_copy()
 * 흐름: ① LBA → LPN 범위 변환·검사 → ② write buffer hit 확인
 *       → ③ (dftl) cmt_touch → ④ maptbl 조회, 미매핑이면 skip
 *       → ⑤ read cache hit 확인 → ⑥ NAND read (multi-plane이면 batch)
 *       → ⑦ 남은 multi-plane read batch flush      (②~⑥은 LPN마다)
 * 반환: LPN별 지연(모두 req->stime 기준) 중 최댓값(ns)
 */
uint64_t ssd_read(struct ssd *ssd, NvmeRequest *req)
{
    struct ssdparams *spp = &ssd->sp;
    struct ppa ppa;
    uint64_t start_lpn, end_lpn;
    uint64_t lpn;
    uint64_t sublat, maxlat = 0;

    /* ① LBA 범위 → LPN 범위. 장치 page 수를 넘으면 LBA Range 에러 */
    ssd_lpn_range(ssd, req, req->slba, req->nlb, &start_lpn, &end_lpn);

    if (end_lpn >= spp->tt_pgs) {
        ftl_err("read past device geometry: end_lpn=%"PRIu64" tt_pgs=%d\n",
                end_lpn, ssd->sp.tt_pgs);
        req->status = NVME_LBA_RANGE | NVME_DNR;
        return 0;
    }

    /* normal IO read path */
    /* sp.read_cnt: host가 읽은 page 수(buffer 통계), block의 read_cnt와 별개 */
    ssd->sp.read_cnt += end_lpn - start_lpn + 1;
    for (lpn = start_lpn; lpn <= end_lpn; lpn++) {
        /* ② buffer에 있으면 DRAM 지연만 내고 NAND는 건너뛴다 */
        /*
         * A page the write buffer is still holding is served from there: it
         * costs a DRAM access and does not reach the media. The buffer is
         * checked even when it has stopped accepting writes, since it can still
         * be holding pages that have not been written back yet.
         */
        if (ssd->write_buffer_cnt && buffer_hit(ssd, lpn)) {
            sublat = buffer_hit_lat(ssd);
            maxlat = (sublat > maxlat) ? sublat : maxlat;
            ssd->sp.read_hit_cnt++;
            continue;
        }

        /* dump the FEMU_DUMP_LPN page on every read (config read once at init) */
        if (exp_dump_lpn_set && lpn == exp_dump_lpn)
            femu_dbg_dump_lpn(ssd, lpn);

        /* ③ CMT 비용은 data read와 합산이 아니라 max (둘 다 stime 기준) */
        /* dftl: charge the demand-cache translation cost (no-op when disabled) */
        if (ssd->cmt.capacity) {
            uint64_t clat = cmt_touch(ssd, lpn, req->stime, false);
            maxlat = (clat > maxlat) ? clat : maxlat;
        }

        /* ④ 미매핑(쓴 적 없거나 trim된) LPN은 NAND 비용 없이 skip */
        ppa = ssd->mapping->translate(ssd, lpn);
        if (!mapped_ppa(&ppa) || !valid_ppa(ssd, &ppa)) {
            //printf("%s,lpn(%" PRId64 ") not mapped to valid ppa\n", ssd->ssdname, lpn);
            //printf("Invalid ppa,ch:%d,lun:%d,blk:%d,pl:%d,pg:%d,sec:%d\n",
            //ppa.g.ch, ppa.g.lun, ppa.g.blk, ppa.g.pl, ppa.g.pg, ppa.g.sec);
            continue;
        }

        /* ⑤ read cache: hit면 hit_lat, miss면 0을 받고 NAND로 진행 */
        /*
         * Optional DRAM read cache: a hit returns at DRAM latency and skips the
         * NAND read (and its channel/LUN occupancy); a miss inserts the LPN and
         * falls through to the media. No-op when disabled (bit-identical default).
         */
        sublat = rcache_touch(ssd, lpn);
        if (sublat) {
            maxlat = (sublat > maxlat) ? sublat : maxlat;
            continue;
        }

        /* ⑥ NAND read: mp_read면 LUN batch에 보류 */
        if (ssd_mp_reads(ssd)) {
            sublat = ssd_mp_add(ssd, &ppa, req->stime, NAND_READ);
            maxlat = (sublat > maxlat) ? sublat : maxlat;
            continue;
        }

        /* 다른 LUN은 병렬, 같은 LUN은 LUN timeline에서 직렬화된다 */
        struct nand_cmd srd;
        srd.type = USER_IO;
        srd.cmd = NAND_READ;
        srd.stime = req->stime;
        sublat = ssd_advance_status(ssd, &ppa, &srd);
        maxlat = (sublat > maxlat) ? sublat : maxlat;
        ssd_read_check_wear(ssd, &ppa, req->stime);
    }
    /* ⑦ 보류된 multi-plane read batch 정리 */
    sublat = ssd_mp_flush(ssd, NAND_READ);
    maxlat = (sublat > maxlat) ? sublat : maxlat;

    return maxlat;
}

/*
 * NVMe Write(와 Copy의 destination write)를 처리한다.
 * 호출: bb_ftl_process_req() (NVME_CMD_WRITE, non-FDP), ssd_copy()
 * 흐름: ① LBA → LPN 범위 변환·검사 → ② free line 부족하면 foreground GC
 *       → ③ read reclaim (line 1개) → ④ 통계 카운터
 *       → ⑤-A buffer 경로: (watermark면 destage 후) LPN을 buffer에 넣고 반환
 *       → ⑤-B direct 경로: 겹치는 buffer entry 정리 → LPN마다 GC 확인 +
 *         ssd_program_lpn() → ⑥ multi-plane flush → ⑦ (fast) reclaim 1회
 * 반환: 지연(ns). buffer 경로는 max(destage 지연, buffer_hit_lat)
 */
uint64_t ssd_write(struct ssd *ssd, NvmeRequest *req)
{
    struct ssdparams *spp = &ssd->sp;
    uint64_t start_lpn, end_lpn;
    uint64_t lpn;
    uint64_t curlat = 0, maxlat = 0;
    const NvmeRwCmd *rw = (const NvmeRwCmd *)&req->cmd;
    bool fua = le16_to_cpu(rw->control) & NVME_RW_FUA;
    /* Streams directive write면 열린 stream slot 번호, 아니면 -1 */
    int stream = ssd->n->streams && req->cmd.opcode == NVME_CMD_WRITE ?
                 nvme_streams_open(req->ns, &req->cmd) : -1;
    int r;

    /* ① LBA 범위 → LPN 범위 + 장치 범위 검사 */
    ssd_lpn_range(ssd, req, req->slba, req->nlb, &start_lpn, &end_lpn);

    if (end_lpn >= spp->tt_pgs) {
        ftl_err("write past device geometry: end_lpn=%"PRIu64" tt_pgs=%d\n",
                end_lpn, ssd->sp.tt_pgs);
        req->status = NVME_LBA_RANGE | NVME_DNR;
        return 0;
    }

    /* ② free line ≤ gc_thres_lines_high 인 동안 foreground GC */
    while (should_gc_high(ssd)) {
        /* perform GC here until !should_gc(ssd) */
        r = do_gc(ssd, true);
        if (r == -1)
            break;
    }

    /* refresh at most one read-stressed line per write */
    /* ③ read path가 예약해 둔 read_reclaim_line을 여기서 rewrite */
    do_read_reclaim(ssd);

    /* ④ host_write_pages = WAF 분모, sp.write_cnt = buffer 통계용 */
    /* pages the host wrote, whether or not the buffer absorbs them */
    ssd->host_write_pages += end_lpn - start_lpn + 1;
    ssd->sp.write_cnt += end_lpn - start_lpn + 1;

    /*
     * ⑤-A buffer 경로 조건: cache 켜짐 + FUA 아님 + stream 아님
     *     + (power_loss면 data가 있는 요청만 — Copy 등은 direct로 간다)
     */
    /*
     * With a write buffer configured the pages are accepted into it rather than
     * programmed, and what gets programmed is whatever they push past the
     * watermark. Without one the loop below programs each page directly,
     * exactly as it always has.
     *
     * Force Unit Access is the host asking for this write to be on the media
     * before it completes, so an FUA write takes the direct path even when the
     * buffer is on. Any copy the buffer is holding is dropped on the way past,
     * below: leaving it there would program the superseded version after this
     * one.
     */
    /* Program stream writes directly even when a cache is enabled. */
    if (buffer_enabled(ssd) && !fua && stream < 0 &&
        (!ssd->n->power_loss || req->write_data)) {
        int batch = buffer_destage_batch(ssd);

        for (lpn = start_lpn; lpn <= end_lpn; lpn++) {
            /* 새 LPN인데 watermark에 닿았으면 batch만큼 먼저 destage */
            /*
             * Admission control, per page rather than once per request: a
             * request larger than the buffer must not be able to push
             * occupancy past the configured size. Writing back a bounded batch
             * also bounds the latency a single command can absorb.
             */
            if (!buffer_hit(ssd, lpn) && buffer_at_watermark(ssd)) {
                curlat = ssd_buffer_destage(ssd, batch, req->stime);
                maxlat = (curlat > maxlat) ? curlat : maxlat;
                /*
                 * The write-back could not free anything, so the buffer is at
                 * its limit and the media is full. Accepting more here would
                 * grow it past its configured size and report success for
                 * pages that can never be programmed; the direct path tells
                 * the host instead, and so does this one.
                 */
                if (ssd_out_of_lines(ssd) && buffer_at_watermark(ssd)) {
                    req->status = NVME_CAP_EXCEEDED | NVME_DNR;
                    break;
                }
            }

            /* 이미 있던 LPN = write hit: 이전 write가 덮여 program 1회 절약 */
            if (buffer_insert(ssd, lpn)) {
                ssd->sp.write_hit_cnt++;
            }
            buffer_write_data(ssd, req, lpn, true);
        }

        /* accepting into DRAM is not free, even though it is not a program */
        curlat = buffer_hit_lat(ssd);
        maxlat = (curlat > maxlat) ? curlat : maxlat;

        return maxlat;
    }

    /* ⑤-B direct 경로 (cache off / FUA / stream / data 없는 power_loss 요청) */
    /*
     * Going direct while pages are still buffered: either the host disabled
     * the write cache, or this is an FUA write. Anything the buffer holds for
     * a page this request also writes has to go first -- programming it
     * afterwards would put the older version on the media. Dropping the entry
     * is enough, since the program below writes the newer version anyway.
     */
    if (ssd->write_buffer_cnt) {
        for (lpn = start_lpn; lpn <= end_lpn; lpn++) {
            /* power_loss면 아래 루프에서 program 직전에 버린다 */
            if (!ssd->n->power_loss) {
                buffer_discard(ssd, lpn);
            }
        }
        /*
         * A disabled write cache must not keep pages back at all, so drain the
         * rest too. An FUA write only owes its own pages and leaves them.
         */
        if (!buffer_enabled(ssd)) {
            curlat = ssd_buffer_destage(ssd, 0, req->stime);
            maxlat = (curlat > maxlat) ? curlat : maxlat;
        }
    }

    /* LPN마다: GC 확인 → 공간 확인 → (dftl) CMT 비용 → program */
    for (lpn = start_lpn; lpn <= end_lpn; lpn++) {
        struct ppa target = { .ppa = INVALID_PPA };

        /*
         * Collect per page, as write-back does: one command can span more
         * lines than the watermark keeps free, and a collection that starts
         * after the last line is gone has nowhere to move pages to.
         */
        curlat = ssd_gc_until_clear(ssd);
        maxlat = (curlat > maxlat) ? curlat : maxlat;

        /*
         * Garbage collection could not free a line, so there is nowhere to
         * put this page. Tell the host instead of programming through a write
         * pointer that no longer addresses anything.
         */
        /* stream write는 자기 stream line이, 일반 write는 data line이 필요 */
        if (stream >= 0) {
            target = ssd_stream_page(ssd, stream,
                                     ssd->n->stream_slots[stream].tag);
        }
        if (stream >= 0 ? !mapped_ppa(&target) : ssd_out_of_lines(ssd)) {
            req->status = NVME_CAP_EXCEEDED | NVME_DNR;
            break;
        }

        /* dftl: charge the demand-cache translation cost (no-op when disabled) */
        if (ssd->cmt.capacity) {
            uint64_t clat = cmt_touch(ssd, lpn, req->stime, true);
            maxlat = (clat > maxlat) ? clat : maxlat;
        }

        /* power_loss: program 직전에 entry(undo)를 버려 이 page를 durable로 */
        if (ssd->n->power_loss) {
            buffer_discard(ssd, lpn);
        }
        buffer_write_data(ssd, req, lpn, false);
        curlat = ssd_program_lpn(ssd, lpn, req->stime, stream);
        maxlat = (curlat > maxlat) ? curlat : maxlat;
    }
    /* ⑥ 보류된 multi-plane program batch 정리 */
    curlat = ssd_mp_flush(ssd, NAND_WRITE);
    maxlat = (curlat > maxlat) ? curlat : maxlat;

    /* ⑦ fast처럼 reclaim_per_page가 아닌 scheme만 요청당 reclaim 1회 */
    /*
     * Let schemes without per-page reclaim collect once the writes have
     * committed -- for a log-block scheme that is a merge. The NAND cost is
     * charged inside reclaim(). One reclaim per request bounds the latency a
     * single command can absorb; page and dftl have no reclaim and skip this.
     */
    if (!ssd->mapping->reclaim_per_page &&
        ssd->mapping->needs_reclaim && ssd->mapping->reclaim &&
        ssd->mapping->needs_reclaim(ssd)) {
        curlat = ssd->mapping->reclaim(ssd, 1);
        maxlat = (curlat > maxlat) ? curlat : maxlat;
    }

    return maxlat;
}

/*
 * Deallocate the logical pages in [start_lpn, end_lpn]: drop whatever the write
 * buffer still owes for them, unmap them, and invalidate the media pages they
 * held. Shared by DSM Deallocate and by Write Zeroes with the Deallocate bit,
 * which owe the device exactly the same thing.
 *
 * Returns the number of pages that had a mapping to invalidate and counts the
 * rest through already_invalid.
 */
/*
 * LPN 범위를 unmap한다. NAND 접근이 없어서 지연도 없다.
 * 호출: ssd_trim(), ssd_write_zeroes() (DEAC=1), bbssd_deallocate_all()
 * 흐름: LPN마다 ① buffer entry 폐기 → ② maptbl/rmap 해제 + 옛 page invalid
 *       → ③ read cache 무효화
 * 반환: invalid 처리한 page 수 (매핑 없던 LPN은 *already_invalid에 누적)
 */
static int ssd_deallocate_lpns(struct ssd *ssd, uint64_t start_lpn,
                               uint64_t end_lpn, int *already_invalid)
{
    int deallocated = 0;
    uint64_t lpn;

    for (lpn = start_lpn; lpn <= end_lpn; lpn++) {
        struct ppa ppa = get_maptbl_ent(ssd, lpn);

        /* ① buffer 먼저: 아직 program 안 된 page는 maptbl에 매핑이 없다 */
        /*
         * Drop any copy the write buffer is holding first, before the
         * mapped-page test below can skip past it. A page that was accepted
         * into the buffer and not yet written back has no mapping at all, so
         * testing the mapping first would leave the pending program in place
         * and the data the host just discarded would reach the media after it
         * had been deallocated.
         */
        buffer_discard(ssd, lpn);

        /* nothing mapped to invalidate; the buffer entry is already gone */
        if (!mapped_ppa(&ppa) || !valid_ppa(ssd, &ppa)) {
            (*already_invalid)++;
            continue;
        }

        if (exp_lpn_watched(lpn))
            EXP_LOG("[INVALIDATE:trim] lpn=%lu old " PPA_FMT "\n",
                    lpn, PPA_ARG(&ppa));

        /* ② hybrid/fast는 mapping->trim hook, page/dftl은 아래 flat unmap */
        /*
         * Hand the unmap to the mapping scheme when it keeps state of its own,
         * so a log-block scheme drops the page from its logs as well. The hook
         * does the same flat invalidation; without one, do it here.
         */
        if (ssd->mapping->trim) {
            ssd->mapping->trim(ssd, lpn);
        } else {
            mark_page_invalid(ssd, &ppa);
            set_rmap_ent(ssd, INVALID_LPN, &ppa);
            ppa.ppa = UNMAPPED_PPA;
            set_maptbl_ent(ssd, lpn, &ppa);
        }

        /* ③ read cache 무효화 */
        /* drop any stale read-cache entry for the deallocated page */
        rcache_invalidate(ssd, lpn);

        deallocated++;
    }

    return deallocated;
}

/*
 * Write Zeroes with the Deallocate bit: the blocks become deallocated, so the
 * FTL owes the same unmap DSM Deallocate does. The I/O layer has already zeroed
 * the backing store and cleared the allocation bits; without this the mapping
 * and the write buffer would still be holding the old pages.
 */
/*
 * NVMe Write Zeroes의 FTL 처리 (backend를 0으로 채우는 건 I/O layer가 함).
 * 호출: bb_ftl_process_req() (NVME_CMD_WRITE_ZEROES, non-FDP)
 * 흐름: DEAC=0 → ① 범위 검사 → ② foreground GC → ③ 통계 카운터
 *         → ④ 겹치는 buffer entry 폐기 → ⑤ LPN마다 ssd_program_lpn()
 *         → ⑥ multi-plane flush, 지연 반환
 *       DEAC=1 → ssd_deallocate_lpns()로 unmap, 지연 0
 */
uint64_t ssd_write_zeroes(struct ssd *ssd, NvmeRequest *req)
{
    struct ssdparams *spp = &ssd->sp;
    const NvmeRwCmd *rw = (const NvmeRwCmd *)&req->cmd;
    uint64_t start_lpn, end_lpn;
    int already_invalid = 0;

    /* Write Zeroes의 NLB 필드는 0-based라 +1 해서 개수로 만든다 */
    ssd_lpn_range(ssd, req, le64_to_cpu(rw->slba), le16_to_cpu(rw->nlb) + 1,
                  &start_lpn, &end_lpn);

    /* DEAC 비트가 없으면 zero를 "기록"하므로 write처럼 program 비용을 낸다 */
    if (!(le16_to_cpu(rw->control) & NVME_WZ_DEAC)) {
        uint64_t lpn, curlat, maxlat = 0;

        /*
         * Without the bit the blocks hold written zeros rather than becoming
         * deallocated: the host reads zeros back, so the device has to put
         * them somewhere. Program the range as a write would. The I/O layer
         * has already zeroed the backing store; what is owed here is the media
         * cost and the page counts, which the command used to escape entirely.
         *
         * It goes straight to the media rather than through the write buffer,
         * since the host handed over no data to hold.
         */
        /* ① 장치 범위 밖이면 아무것도 안 한다 (status는 그대로) */
        if (end_lpn >= spp->tt_pgs) {
            return 0;
        }

        /* ② foreground GC */
        while (should_gc_high(ssd)) {
            if (do_gc(ssd, true) == -1) {
                break;
            }
        }

        /* ③ ssd_write()와 같은 통계 카운터 */
        ssd->host_write_pages += end_lpn - start_lpn + 1;
        /* the pair an ordinary write bumps; these pages are programmed too */
        ssd->sp.write_cnt += end_lpn - start_lpn + 1;

        /* ④ 같은 LPN의 buffer entry 폐기 (옛 버전이 나중에 program되지 않게) */
        /*
         * This goes straight to the media, so anything the buffer is holding
         * for these pages has to go first, exactly as the direct write path
         * does: leaving it there programs the superseded version afterwards,
         * which is one more program per page than the media owes and shows up
         * as amplification the workload did not cause.
         */
        if (ssd->write_buffer_cnt) {
            for (lpn = start_lpn; lpn <= end_lpn; lpn++) {
                buffer_discard(ssd, lpn);
            }
        }

        /* ⑤ buffer를 거치지 않고 LPN마다 바로 program */
        for (lpn = start_lpn; lpn <= end_lpn; lpn++) {
            /* per page, for the reason ssd_write() gives */
            curlat = ssd_gc_until_clear(ssd);
            maxlat = (curlat > maxlat) ? curlat : maxlat;
            if (ssd_out_of_lines(ssd)) {
                req->status = NVME_CAP_EXCEEDED | NVME_DNR;
                break;
            }
            curlat = ssd_program_lpn(ssd, lpn, req->stime, -1);
            maxlat = (curlat > maxlat) ? curlat : maxlat;
        }
        /* ⑥ 보류된 multi-plane program batch 정리 */
        curlat = ssd_mp_flush(ssd, NAND_WRITE);
        maxlat = (curlat > maxlat) ? curlat : maxlat;

        return maxlat;
    }

    /* DEAC=1: unmap만 하고 NAND 비용은 0 */
    if (end_lpn >= spp->tt_pgs) {
        return 0;
    }

    ssd_deallocate_lpns(ssd, start_lpn, end_lpn, &already_invalid);

    return 0;
}

/*
 * Drop every mapping the namespace's FTL holds, as Format erases it. The host's
 * data is gone, so the pages it occupied are invalid: left mapped, garbage
 * collection would keep relocating data nobody can read any more. The caller
 * has the dataplane and the FTL thread paused.
 */
/*
 * namespace의 모든 LPN을 unmap한다 (Format/Sanitize용).
 * 호출: nvme_sanitize(), nvme_format_namespace() (nvme-admin.c)
 * FDP면 ssd_deallocate_fdp_all(), 아니면 ssd_deallocate_lpns(0..tt_pgs-1)
 */
void bbssd_deallocate_all(NvmeNamespace *ns)
{
    struct ssd *ssd = ns->ssd;
    int already_invalid = 0;

    if (!ssd || !ssd->sp.tt_pgs) {
        return;
    }
    if (ssd->fdp_enabled) {
        ssd_deallocate_fdp_all(ssd);
    } else {
        ssd_deallocate_lpns(ssd, 0, ssd->sp.tt_pgs - 1, &already_invalid);
    }
}

/*
 * NVMe DSM Deallocate(trim): range마다 LPN 범위를 unmap한다.
 * 호출: bb_ftl_process_req() (NVME_CMD_DSM, non-FDP, range가 있을 때)
 * 흐름: ① range마다 LPN 범위 변환, 장치 범위 밖이면 그 range만 skip
 *       → ② ssd_deallocate_lpns() → ③ range 배열 해제
 *       → ④ 지연 = trim_lat_ns × range 수 (기본 0 → TRIM은 즉시 완료)
 */
uint64_t ssd_trim(struct ssd *ssd, NvmeRequest *req)
{
    struct ssdparams *spp = &ssd->sp;
    NvmeDsmRange *ranges = req->dsm_ranges;
    int nr_ranges = req->dsm_nr_ranges;
    // uint32_t attributes = req->dsm_attributes;
    
    int total_trimmed_pages = 0;
    int total_already_invalid = 0;
    int total_out_of_bounds = 0;
    
    if (!ranges || nr_ranges <= 0) {
        printf("TRIM: Invalid ranges or count\n");
        return 0;
    }
    
    // printf("TRIM: Processing %d ranges (attributes=0x%x)\n", nr_ranges, attributes);
    
    /* ① range마다 처리. 범위 밖 range는 에러 없이 건너뛴다 */
    for (int range_idx = 0; range_idx < nr_ranges; range_idx++) {
        uint64_t start_lpn, end_lpn;
        int trimmed_pages;
        int already_invalid = 0;

        ssd_lpn_range(ssd, req, le64_to_cpu(ranges[range_idx].slba),
                      le32_to_cpu(ranges[range_idx].nlb), &start_lpn, &end_lpn);

        // Boundary check
        if (end_lpn >= spp->tt_pgs) {
            ftl_err("TRIM: Range %d exceeds FTL capacity - end_lpn=%lu, tt_pgs=%d\n", 
                   range_idx, end_lpn, spp->tt_pgs);
            total_out_of_bounds++;
            continue;  // Skip this range, continue with others
        }

        /* ② buffer 폐기 + unmap + 옛 page invalid (NAND 접근 없음) */
        // Process each LPN in this range
        trimmed_pages = ssd_deallocate_lpns(ssd, start_lpn, end_lpn,
                                            &already_invalid);
        
        total_trimmed_pages += trimmed_pages;
        total_already_invalid += already_invalid;
        
        // ftl_debug("TRIM Range %d: %d pages trimmed, %d already invalid\n", 
        //        range_idx, trimmed_pages, already_invalid);
    }

    // ftl_debug("TRIM: Completed - %d pages trimmed, %d already invalid, %d out of bounds across %d ranges\n", 
    //        total_trimmed_pages, total_already_invalid, total_out_of_bounds, nr_ranges);

    /* ③ nvme_dsm()(I/O layer)이 넘긴 range 배열은 FTL이 해제한다 */
    // Free the ranges array
    g_free(ranges);
    req->dsm_ranges = NULL;
    req->dsm_nr_ranges = 0;
    req->dsm_attributes = 0;

    /* right after a TRIM, check whether the marker still remains in the backend */
    if (exp_log_enabled)
        femu_dbg_scan_secret(ssd, "after_trim");

    /* ④ range 수는 건너뛴 range까지 포함한 nr_ranges */
    /*
     * Optional modeled TRIM cost: trim_lat_ns per processed DSM range, for the
     * metadata map updates a real SSD incurs on deallocate. Default 0 keeps TRIM
     * instant, bit-identical to the prior behavior.
     */
    if (spp->trim_lat_ns > 0) {
        return (uint64_t)spp->trim_lat_ns * nr_ranges;
    }
    return 0;
}
