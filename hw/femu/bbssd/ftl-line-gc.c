/*
 * bbssd FTL line management and garbage collection.
 *
 * Owns the free/victim/full line lists, the write pointer, page/block/line
 * valid-invalid bookkeeping, and the greedy GC (victim selection, valid-page
 * copyback, block erase). Split out of the ftl.c monolith; the FDP-specific
 * reclaim-unit machinery lives separately.
 */
/*
 * 역할: non-FDP bbssd의 line(= superblock) 관리와 GC를 담당한다.
 *   - line 상태 3종: free list / victim pq(min-heap) / full list
 *     (쓰는 중인 line과 GC 중인 victim은 어느 list에도 없다)
 *   - write pointer(데이터 wp, HOT/LOG class, stream) 전진과 새 PPA 발급
 *   - page/block/line의 vpc·ipc 집계(mark_page_valid/invalid)
 *   - GC: victim 정책 5종 → valid page 이동 → erase → free list
 *   - read reclaim / retention refresh(do_read_reclaim)
 * 주요 진입: ssd_init_lines(), ssd_init_write_pointer(), get_new_page*(),
 *   ssd_advance_write_pointer*(), mark_page_valid/invalid(), do_gc(),
 *   do_read_reclaim()
 * 의존: ftl.c(ssd_init, bb_ftl_process_req), ftl-datapath.c(쓰기 경로),
 *   ftl-map*.c(mapping scheme)가 부른다. 일부 helper는 ftl-fdp.c와
 *   kvssd-ftl.c도 함께 쓴다. 이 파일은 ftl-media.c의
 *   ssd_advance_status*()로 NAND 지연을, lib/pqueue.c로 victim heap을 쓴다.
 * 자세한 설명: 참고자료/code_study/13_line_gc_geometry_media.md
 */
#include "qemu/osdep.h"
#include "ftl.h"
#include "ftl-internal.h"

/* victim-line priority-queue callbacks */
/*
 * victim pq callback 묶음(lib/pqueue.c가 호출).
 * pqueue는 cmp_pri(부모, 자식)이 참이면 둘을 바꾼다. 여기서는 '>'이므로
 * 작은 key가 위로 올라가는 min-heap이다(heap top = key가 가장 작은 line).
 *   - get/set_pri       : key = vpc (greedy 등, valid page 적을수록 top)
 *   - get/set_close_seq : key = close_seq (fifo, 먼저 닫힌 line이 top)
 *   - get/set_pos       : heap index를 line->pos에 기록(0 = pq 밖)
 */
static inline int victim_line_cmp_pri(pqueue_pri_t next, pqueue_pri_t curr)
{
    return (next > curr);
}

static inline pqueue_pri_t victim_line_get_pri(void *a)
{
    return ((struct line *)a)->vpc;
}

static inline void victim_line_set_pri(void *a, pqueue_pri_t pri)
{
    ((struct line *)a)->vpc = pri;
}

static inline pqueue_pri_t victim_line_get_close_seq(void *a)
{
    return ((struct line *)a)->close_seq;
}

static inline void victim_line_set_close_seq(void *a, pqueue_pri_t pri)
{
    ((struct line *)a)->close_seq = pri;
}

static inline size_t victim_line_get_pos(void *a)
{
    return ((struct line *)a)->pos;
}

static inline void victim_line_set_pos(void *a, size_t pos)
{
    ((struct line *)a)->pos = pos;
}

/*
 * line 배열을 만들고 모든 line을 free list에 넣는다.
 * 호출: ssd_init() (ftl.c, ssd->policy를 정한 직후)
 * 흐름: ① tt_lines(= blks_per_pl)개 line 할당
 *       → ② gc_policy에 맞는 key로 victim pq 생성 → ③ 전부 free list로
 */
void ssd_init_lines(struct ssd *ssd)
{
    struct ssdparams *spp = &ssd->sp;
    struct line_mgmt *lm = &ssd->lm;
    struct line *line;

    /* ① line i = 모든 ch/LUN/plane의 block i 묶음 → 개수 = blks_per_pl */
    lm->tt_lines = spp->blks_per_pl;
    ftl_assert(lm->tt_lines == spp->tt_lines);
    lm->lines = g_malloc0(sizeof(struct line) * lm->tt_lines);

    QTAILQ_INIT(&lm->free_line_list);
    /* ② fifo만 close_seq를 key로, 나머지 정책은 vpc를 key로 쓴다 */
    if (ssd->policy->by_close_order) {
        lm->victim_line_pq = pqueue_init(spp->tt_lines, victim_line_cmp_pri,
                victim_line_get_close_seq, victim_line_set_close_seq,
                victim_line_get_pos, victim_line_set_pos);
    } else {
        lm->victim_line_pq = pqueue_init(spp->tt_lines, victim_line_cmp_pri,
                victim_line_get_pri, victim_line_set_pri,
                victim_line_get_pos, victim_line_set_pos);
    }
    lm->next_close_seq = 0;
    QTAILQ_INIT(&lm->full_line_list);

    /* ③ 초기 상태: 모든 line이 free, victim pq와 full list는 비어 있음 */
    lm->free_line_cnt = 0;
    for (int i = 0; i < lm->tt_lines; i++) {
        line = &lm->lines[i];
        line->id = i;
        line->ipc = 0;
        line->vpc = 0;
        line->pos = 0;
        line->close_time = 0;
        line->close_seq = 0;
        /* initialize all the lines as free lines */
        QTAILQ_INSERT_TAIL(&lm->free_line_list, line, entry);
        lm->free_line_cnt++;
    }

    ftl_assert(lm->free_line_cnt == lm->tt_lines);
    lm->victim_line_cnt = 0;
    lm->full_line_cnt = 0;
}

/*
 * 데이터 write pointer(ssd->wp)를 첫 free line의 page 0에 놓고,
 * DRAM write buffer(LRU queue + 조회용 tree)도 초기화한다.
 * 호출: ssd_init() (ftl.c, FDP가 꺼져 있을 때만)
 */
void ssd_init_write_pointer(struct ssd *ssd)
{
    struct write_pointer *wpp = &ssd->wp;
    struct line_mgmt *lm = &ssd->lm;
    struct line *curline = NULL;

    /* 열린(쓰는 중인) line은 free/victim/full 어느 list에도 없다 */
    curline = QTAILQ_FIRST(&lm->free_line_list);
    QTAILQ_REMOVE(&lm->free_line_list, curline, entry);
    lm->free_line_cnt--;

    /* wpp->curline is always our next-to-write super-block */
    ssd_wp_reset(wpp, curline);

    /* DRAM write buffer: LRU queue plus a tree for lookup by page number */
    QTAILQ_INIT(&ssd->write_buffer);
    ssd->write_buffer_cnt = 0;
    ssd->wb_tree = ssd->sp.buffer_size > 0 ? g_tree_new(comp_buffer) : NULL;
}


/*
 * free list 맨 앞 line을 꺼낸다(free된 순서대로 재사용, FIFO).
 * 호출: ssd_advance_write_pointer_common(), ssd_stream_pointer_page(),
 *       ssd_init_class_write_pointer(), femu_fdp_init_ssd_reclaim_unit()
 *       (ftl-fdp.c), kv_ensure_write_pointer() (kvssd-ftl.c)
 * 반환: 꺼낸 line(이제 어느 list에도 없음), free line이 없으면 NULL
 */
struct line *get_next_free_line(struct ssd *ssd)
{
    struct line_mgmt *lm = &ssd->lm;
    struct line *curline = NULL;

    curline = QTAILQ_FIRST(&lm->free_line_list);
    if (!curline) {
        ftl_err("No free lines left in [%s] !!!!\n", ssd->ssdname);
        return NULL;
    }

    QTAILQ_REMOVE(&lm->free_line_list, curline, entry);
    lm->free_line_cnt--;
    return curline;
}

/*
 * write pointer를 다음 page로 옮기고, line이 다 차면 닫고 새 line을 받는다.
 * 호출: ssd_advance_write_pointer(), ssd_advance_write_pointer_class(),
 *       ssd_stream_advance(), gc_write_page()
 * 흐름: ① ssd_wp_step(): ch → lun → pl → pg 순으로 증가(line 미완이면 끝)
 *       → ② close_time/close_seq 기록
 *       → ③ full list 또는 victim pq로 이동
 *       → ④ free list에서 다음 line 수령(없으면 curline = NULL)
 */
static void ssd_advance_write_pointer_common(struct ssd *ssd,
                                             struct write_pointer *wpp)
{
    struct ssdparams *spp = &ssd->sp;
    struct line_mgmt *lm = &ssd->lm;

    /* ① ch가 가장 빨리 돈다 → 연속 page가 서로 다른 channel/LUN에 분산 */
    if (!ssd_wp_step(spp, wpp)) {
        return;
    }
    /* ② close_time은 cost-benefit의 age, close_seq는 fifo의 key */
    /* record when the line filled, for age-based GC policies */
    wpp->curline->close_time = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
    wpp->curline->close_seq = lm->next_close_seq++;
    /* ③ 닫힌 line: invalid가 없으면 full list, 하나라도 있으면 victim pq */
    /* move current line to {victim,full} line list */
    if (wpp->curline->vpc == spp->pgs_per_line) {
        /* all pgs are still valid, move to full line list */
        ftl_assert(wpp->curline->ipc == 0);
        QTAILQ_INSERT_TAIL(&lm->full_line_list, wpp->curline, entry);
        lm->full_line_cnt++;
    } else {
        ftl_assert(wpp->curline->vpc >= 0 &&
                   wpp->curline->vpc < spp->pgs_per_line);
        /* there must be some invalid pages in this line */
        ftl_assert(wpp->curline->ipc > 0);
        pqueue_insert(lm->victim_line_pq, wpp->curline);
        lm->victim_line_cnt++;
    }
    /* ④ 새 line의 block index = line id (line i는 모든 LUN의 block i) */
    /* current line is used up, pick another empty line */
    check_addr(wpp->blk, spp->blks_per_pl);
    wpp->curline = get_next_free_line(ssd);
    if (!wpp->curline) {
        /*
         * Nothing left to program into, and get_next_free_line() has said so.
         * Leave the pointer without a line rather than taking the process down
         * under a running guest: the write paths test for that and refuse the
         * command.
         */
        return;
    }
    wpp->blk = wpp->curline->id;
    check_addr(wpp->blk, spp->blks_per_pl);
    /* make sure we are starting from page 0 in the super block */
    ftl_assert(wpp->pg == 0 && wpp->lun == 0 && wpp->ch == 0 && wpp->pl == 0);
}

/*
 * 주어진 write pointer의 현재 위치를 PPA로 만든다(전진은 하지 않음).
 * line이 없으면 free line을 새로 받아 page 0부터 시작하고, line에
 * stream_tag를 붙인다(GC가 이 tag로 같은 stream의 pointer를 찾음).
 * 호출: ssd_stream_page(), ssd_out_of_lines(), gc_write_page()
 *       (뒤 둘은 tag 0으로 ssd->wp에 line을 다시 붙일 때도 쓴다)
 * 반환: 새 PPA, free line이 없으면 INVALID_PPA
 */
static struct ppa ssd_stream_pointer_page(struct ssd *ssd,
                                          struct write_pointer *wp,
                                          uint64_t tag)
{
    struct ppa ppa = { .ppa = INVALID_PPA };

    if (!wp->curline) {
        wp->curline = get_next_free_line(ssd);
        if (!wp->curline) {
            return ppa;
        }
        wp->ch = wp->lun = wp->pl = wp->pg = 0;
        wp->blk = wp->curline->id;
    }
    wp->curline->stream_tag = tag;
    ppa.ppa = 0;
    ppa.g.ch = wp->ch;
    ppa.g.lun = wp->lun;
    ppa.g.pl = wp->pl;
    ppa.g.pg = wp->pg;
    ppa.g.blk = wp->blk;
    return ppa;
}

/*
 * 데이터 write pointer에 쓸 line이 없는지 확인한다. line이 끊긴 사이
 * format/trim/GC로 free line이 생겼으면 여기서 다시 붙인다.
 * 호출: ssd_buffer_destage(), ssd_write(), ssd_write_zeroes()
 *       (ftl-datapath.c)
 * 반환: true = 쓸 line 없음(호출자가 쓰기를 멈추거나 거절)
 */
bool ssd_out_of_lines(struct ssd *ssd)
{
    /* Format, trim or GC may have freed space since the frontier filled. */
    if (!ssd->wp.curline && ssd->lm.free_line_cnt) {
        ssd_stream_pointer_page(ssd, &ssd->wp, 0);
    }
    return ssd->wp.curline == NULL;
}

/*
 * stream용 write pointer가 쥔 line을 내려놓는다.
 * 호출: ssd_release_stream(), ssd_stream_gc_pointer()
 * 흐름: ① 한 page도 안 쓴 line → free list로 반납
 *       ② 일부만 쓴 line → 남은 FREE page를 INVALID로 봉인(ipc 증가)하고
 *          닫힌 line처럼 victim pq에 넣어 GC가 회수하게 함
 *       → ③ pointer를 0으로 초기화(curline = NULL)
 */
static void ssd_stream_close_pointer(struct ssd *ssd, struct write_pointer *wp)
{
    struct line *line = wp->curline;
    struct ssdparams *sp = &ssd->sp;
    struct ppa ppa = { .ppa = 0 };
    int ch;
    int lun;
    int pl;
    int pg;

    if (!line) {
        return;
    }
    /* ① vpc = ipc = 0: 한 page도 program되지 않은 line */
    if (!line->vpc && !line->ipc) {
        line->stream_tag = 0;
        QTAILQ_INSERT_TAIL(&ssd->lm.free_line_list, line, entry);
        ssd->lm.free_line_cnt++;
    } else {
        /* ② clean_one_block()은 victim에 FREE page가 없다고 가정한다 */
        /* Seal unused pages so GC can reclaim a partially written line. */
        ppa.g.blk = line->id;
        for (ch = 0; ch < sp->nchs; ch++) {
            ppa.g.ch = ch;
            for (lun = 0; lun < sp->luns_per_ch; lun++) {
                ppa.g.lun = lun;
                for (pl = 0; pl < sp->pls_per_lun; pl++) {
                    ppa.g.pl = pl;
                    for (pg = 0; pg < sp->pgs_per_blk; pg++) {
                        ppa.g.pg = pg;
                        if (get_pg(ssd, &ppa)->status == PG_FREE) {
                            get_pg(ssd, &ppa)->status = PG_INVALID;
                            get_blk(ssd, &ppa)->ipc++;
                            line->ipc++;
                        }
                    }
                }
            }
        }
        line->close_time = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
        line->close_seq = ssd->lm.next_close_seq++;
        pqueue_insert(ssd->lm.victim_line_pq, line);
        ssd->lm.victim_line_cnt++;
    }
    /* ③ curline = NULL → 다음 ssd_stream_pointer_page()가 새 line을 받음 */
    memset(wp, 0, sizeof(*wp));
}

/*
 * stream slot을 닫는다: pointer의 line을 내려놓고 tag를 지운다.
 * 호출: nvme_stream_close() (nvme-streams.c)
 */
void ssd_release_stream(struct ssd *ssd, unsigned slot)
{
    ssd_stream_close_pointer(ssd, &ssd->stream_wp[slot]);
    ssd->stream_tags[slot] = 0;
}

/*
 * stream slot의 다음 PPA를 돌려준다(slot의 tag도 갱신).
 * 호출: ssd_program_lpn(), ssd_write() (ftl-datapath.c)
 */
struct ppa ssd_stream_page(struct ssd *ssd, unsigned slot, uint64_t tag)
{
    ssd->stream_tags[slot] = tag;
    return ssd_stream_pointer_page(ssd, &ssd->stream_wp[slot], tag);
}

/*
 * stream slot의 pointer를 한 page 전진. line이 바뀌었으면 새 line에도
 * slot의 tag를 붙인다.
 * 호출: ssd_program_lpn() (ftl-datapath.c)
 */
void ssd_stream_advance(struct ssd *ssd, unsigned slot)
{
    ssd_advance_write_pointer_common(ssd, &ssd->stream_wp[slot]);
    if (ssd->stream_wp[slot].curline) {
        ssd->stream_wp[slot].curline->stream_tag = ssd->stream_tags[slot];
    }
}

/*
 * stream tag가 붙은 page를 GC가 옮길 목적지 pointer를 고른다.
 * 열린 slot 중 같은 tag가 있으면 그 slot의 pointer를, 없으면(release된
 * stream) 공용 stream_gc_wp를 쓴다. 공용 pointer의 tag가 바뀌면 이전
 * line을 먼저 닫아 서로 다른 stream이 한 line에 섞이지 않게 한다.
 * 호출: gc_write_page()
 */
static struct write_pointer *ssd_stream_gc_pointer(struct ssd *ssd,
                                                   uint64_t tag)
{
    unsigned i;

    for (i = 0; i < ssd->n->streams_max; i++) {
        if (ssd->stream_tags[i] == tag) {
            return &ssd->stream_wp[i];
        }
    }
    /* Released streams retain their identity while their data remains live. */
    if (ssd->stream_gc_tag != tag) {
        ssd_stream_close_pointer(ssd, &ssd->stream_gc_wp);
        ssd->stream_gc_tag = tag;
    }
    return &ssd->stream_gc_wp;
}

/* take a line for a class that allocates one lazily (LOG, HOT) */
/* 호출: ssd_write_pointer_for_class() (그 pointer에 line이 없을 때) */
static void ssd_init_class_write_pointer(struct ssd *ssd,
                                         struct write_pointer *wpp,
                                         const char *what)
{
    struct line *curline = get_next_free_line(ssd);

    if (!curline) {
        ftl_err("no free line for the %s class in [%s]\n", what, ssd->ssdname);
        return;
    }

    ssd_wp_reset(wpp, curline);
}

/* pick the write pointer an allocation class writes through */
/*
 * class → pointer: LOG → log_wp, HOT → hot_wp(첫 사용 때 line 배정).
 * DATA이거나 line을 못 받으면 데이터 pointer ssd->wp로 fallback.
 * 호출: ssd_advance_write_pointer_class(), get_new_page_class()
 */
static struct write_pointer *ssd_write_pointer_for_class(struct ssd *ssd,
                                                         int klass)
{
    struct write_pointer *wpp = NULL;
    const char *what = NULL;

    if (klass == FEMU_MAP_CLASS_LOG) {
        wpp = &ssd->log_wp;
        what = "log";
    } else if (klass == FEMU_MAP_CLASS_HOT) {
        wpp = &ssd->hot_wp;
        what = "hot";
    }

    if (wpp) {
        if (!wpp->curline) {
            ssd_init_class_write_pointer(ssd, wpp, what);
        }
        if (wpp->curline) {
            return wpp;
        }
    }

    /* no line to be had for the class; fall back to the data pointer */
    return &ssd->wp;
}

/*
 * 데이터 write pointer(ssd->wp)를 한 page 전진.
 * 호출: gc_write_page() (stream tag가 없는 page를 옮긴 뒤)
 */
void ssd_advance_write_pointer(struct ssd *ssd)
{
    ssd_advance_write_pointer_common(ssd, &ssd->wp);
}

/*
 * mapping scheme이 고른 class의 pointer를 한 page 전진.
 * 호출: ssd_program_lpn() (ftl-datapath.c), femu_map_hybrid_reclaim()
 *       (ftl-map-hybrid.c), fast_merge_one_lbn() (ftl-map-fast.c)
 */
void ssd_advance_write_pointer_class(struct ssd *ssd, int klass)
{
    ssd_advance_write_pointer_common(ssd, ssd_write_pointer_for_class(ssd, klass));
}

/*
 * class의 write pointer 위치를 PPA로 돌려준다(전진은 별도 호출).
 * 호출: ssd_program_lpn(), femu_map_hybrid_reclaim(), fast_merge_one_lbn()
 * 반환: 새 PPA, pointer에 line이 없으면 INVALID_PPA
 */
struct ppa get_new_page_class(struct ssd *ssd, int klass)
{
    struct write_pointer *wpp = ssd_write_pointer_for_class(ssd, klass);
    struct ppa ppa;

    /* no line on this pointer, no page -- see get_new_page() */
    if (!wpp->curline) {
        ppa.ppa = INVALID_PPA;
        return ppa;
    }

    ppa.ppa = 0;
    ppa.g.ch = wpp->ch;
    ppa.g.lun = wpp->lun;
    ppa.g.pg = wpp->pg;
    ppa.g.blk = wpp->blk;
    ppa.g.pl = wpp->pl;

    return ppa;
}

/*
 * 데이터 write pointer(ssd->wp)의 현재 위치를 PPA로 돌려준다.
 * "할당"은 위치를 읽기만 하므로, 쓴 뒤 반드시 pointer를 전진시켜야 한다.
 * 호출: gc_write_page(), kv_program_ppas() (kvssd-ftl.c)
 * 반환: 새 PPA, line이 없으면 INVALID_PPA(UNMAPPED_PPA와 같은 ~0 값)
 */
struct ppa get_new_page(struct ssd *ssd)
{
    struct write_pointer *wpp = &ssd->wp;
    struct ppa ppa;

    /*
     * The pointer is left without a line when nothing is free, and its block
     * field still names the line that has just closed -- so building an
     * address from it here hands back page zero of a line that is already
     * fully programmed. Say there is no page instead, and let the caller
     * decide; every allocation goes through here, so no path can miss it.
     */
    if (!wpp->curline) {
        ppa.ppa = INVALID_PPA;
        return ppa;
    }

    ppa.ppa = 0;
    ppa.g.ch = wpp->ch;
    ppa.g.lun = wpp->lun;
    ppa.g.pg = wpp->pg;
    ppa.g.blk = wpp->blk;
    ppa.g.pl = wpp->pl;

    return ppa;
}

/* update SSD status about one page from PG_VALID -> PG_INVALID */
/*
 * page를 VALID → INVALID로 바꾸고 block/line의 vpc·ipc를 갱신한다.
 * 호출: gc_write_page(), ssd_deallocate_lpns() (ftl-datapath.c),
 *       mapping scheme의 덮어쓰기/trim/merge: femu_map_page_commit_write()
 *       (ftl-map.c), femu_map_hybrid_*(), femu_map_fast_*(),
 *       fast_merge_one_lbn(), kv_invalidate_ppa_array() (kvssd-ftl.c)
 * 흐름: ① page 상태 → ② block·line 집계 → ③ victim pq 안이면 heap 위치
 *       갱신 → ④ full line이었다면 victim pq로 이동
 */
void mark_page_invalid(struct ssd *ssd, struct ppa *ppa)
{
    struct line_mgmt *lm = &ssd->lm;
    struct ssdparams *spp = &ssd->sp;
    struct nand_block *blk = NULL;
    struct nand_page *pg = NULL;
    bool was_full_line = false;
    struct line *line;

    /* ① VALID → INVALID (FREE로는 erase 때 mark_block_free()만 되돌림) */
    /* update corresponding page status */
    pg = get_pg(ssd, ppa);
    if (unlikely(ssd->debug_ftl) && pg->status != PG_VALID) {
        ftl_err("invalidating a page that is not valid: status=%d " PPA_FMT "\n",
                pg->status, PPA_ARG(ppa));
    }
    ftl_assert(pg->status == PG_VALID);
    pg->status = PG_INVALID;

    /* ② block과 그 block이 속한 line(= 같은 blk index)을 함께 갱신 */
    /* update corresponding block status */
    blk = get_blk(ssd, ppa);
    ftl_assert(blk->ipc >= 0 && blk->ipc < spp->pgs_per_blk);
    blk->ipc++;
    ftl_assert(blk->vpc > 0 && blk->vpc <= spp->pgs_per_blk);
    blk->vpc--;

    /* update corresponding line status */
    line = get_line(ssd, ppa);
    ftl_assert(line->ipc >= 0 && line->ipc < spp->pgs_per_line);
    if (line->vpc == spp->pgs_per_line) {
        ftl_assert(line->ipc == 0);
        was_full_line = true;
    }
    line->ipc++;
    ftl_assert(line->vpc > 0 && line->vpc <= spp->pgs_per_line);
    /* ③ pos ≠ 0 → victim pq 안: vpc가 key이면 heap을 재정렬해야 한다 */
    /*    fifo는 key(close_seq)가 그대로라 vpc만 줄인다 */
    /* Adjust the position of the victime line in the pq under over-writes */
    if (line->pos && !ssd->policy->by_close_order) {
        /* Note that line->vpc will be updated by this call */
        pqueue_change_priority(lm->victim_line_pq, line->vpc - 1, line);
    } else {
        line->vpc--;
    }

    /* ④ full → victim. reclaiming(read reclaim 중) line은 list 밖에 둔다 */
    if (was_full_line && !line->reclaiming) {
        /* move line: "full" -> "victim" */
        QTAILQ_REMOVE(&lm->full_line_list, line, entry);
        lm->full_line_cnt--;
        pqueue_insert(lm->victim_line_pq, line);
        lm->victim_line_cnt++;
    }
}

/*
 * 새로 program한 page를 FREE → VALID로 바꾸고 block/line vpc를 올린다.
 * 호출: ssd_program_lpn() (ftl-datapath.c), gc_write_page(),
 *       femu_map_hybrid_reclaim(), fast_merge_one_lbn(), kv_program_ppas()
 */
void mark_page_valid(struct ssd *ssd, struct ppa *ppa)
{
    struct nand_block *blk = NULL;
    struct nand_page *pg = NULL;
    struct line *line;

    /* update page status */
    pg = get_pg(ssd, ppa);
    if (unlikely(ssd->debug_ftl) && pg->status != PG_FREE) {
        ftl_err("programming a page that is not free: status=%d " PPA_FMT "\n",
                pg->status, PPA_ARG(ppa));
    }
    ftl_assert(pg->status == PG_FREE);
    pg->status = PG_VALID;

    /* update corresponding block status */
    blk = get_blk(ssd, ppa);
    ftl_assert(blk->vpc >= 0 && blk->vpc < ssd->sp.pgs_per_blk);
    blk->vpc++;

    /* 새 page는 열린 line(어느 list에도 없음)에 쓰이므로 pq 갱신이 없다 */
    /* update corresponding line status */
    line = get_line(ssd, ppa);
    ftl_assert(line->vpc >= 0 && line->vpc < ssd->sp.pgs_per_line);
    line->vpc++;
}

/*
 * Free block @blk on every plane of LUN @ch/@lun and, with @charge, time them
 * as one multi-plane erase starting at @stime (0: now): a line holds the same
 * block index on every plane, so the die erases them together. The LUN's GC end
 * then follows its busy-until time. Returns the erase latency, 0 if untimed.
 */
/*
 * 한 LUN에서 line에 속한 block(모든 plane)을 erase한다.
 * 호출: reclaim_line(), do_gc_fdp_style()·ssd_trim_fdp_reset_all()
 *       (ftl-fdp.c), kv_reclaim_empty_lines() (kvssd-ftl.c)
 * 흐름: ① plane마다 mark_block_free() → ② charge면 multi-plane erase
 *       지연 계산 → ③ LUN의 gc_endtime 갱신
 * 반환: erase 지연(ns), charge = false면 0
 */
uint64_t ssd_erase_lun_block(struct ssd *ssd, int ch, int lun, int blk,
                             bool charge, int64_t stime)
{
    struct ssdparams *spp = &ssd->sp;
    struct ppa ppas[1 << PL_BITS];
    struct ppa ppa = { .ppa = 0 };
    struct nand_lun *lunp;
    uint64_t lat = 0;

    ppa.g.ch = ch;
    ppa.g.lun = lun;
    ppa.g.blk = blk;
    lunp = get_lun(ssd, &ppa);
    /* ① 모든 plane의 같은 block index를 free로 되돌리고 ppas[]에 모은다 */
    for (int pl = 0; pl < spp->pls_per_lun; pl++) {
        ppa.g.pl = pl;
        mark_block_free(ssd, &ppa);
        ppas[pl] = ppa;
    }
    /* ② plane이 1개면 multiplane 함수가 ssd_advance_status()로 위임 */
    if (charge) {
        struct nand_cmd gce = {
            .type = GC_IO,
            .cmd = NAND_ERASE,
            .stime = stime,
        };

        lat = ssd_advance_status_multiplane(ssd, ppas, spp->pls_per_lun, &gce);
    }
    /* ③ gc_endtime은 기록만 되고 hw/femu 안에서 읽는 곳은 없다 */
    lunp->gc_endtime = lunp->next_lun_avail_time;
    return lat;
}

/*
 * block 하나를 erase 직후 상태로: 모든 page FREE, vpc/ipc 0, erase_cnt와
 * total_erases 증가, read_cnt 0(erase가 read disturb 누적을 지움).
 * 지연은 여기서 계산하지 않는다(ssd_erase_lun_block()이 부과).
 * 호출: ssd_erase_lun_block()
 */
void mark_block_free(struct ssd *ssd, struct ppa *ppa)
{
    struct ssdparams *spp = &ssd->sp;
    struct nand_block *blk = get_blk(ssd, ppa);
    struct nand_page *pg = NULL;

    for (int i = 0; i < spp->pgs_per_blk; i++) {
        /* reset page status */
        pg = &blk->pg[i];
        pg->status = PG_FREE;
    }

    /* reset block status */
    ftl_assert(blk->npgs == spp->pgs_per_blk);
    blk->ipc = 0;
    blk->vpc = 0;
    blk->erase_cnt++;
    ssd->total_erases++;
    blk->read_cnt = 0; /* the stress an erase clears */
    if (exp_watch_blk[ppa->g.blk])
        EXP_LOG("[ERASE] " PPA_FMT " erase_cnt=%d (vpc/ipc reset)\n",
                PPA_ARG(ppa), blk->erase_cnt);
}

/*
 * GC가 valid page를 읽는 NAND read 지연을 LUN timeline에 부과한다.
 * 데이터는 이미 DRAM에 있어(gc_write_page() 위 주석) 실제 복사는 없다.
 * stime = 0(지금)이고 반환 지연은 버린다: host 요청 지연에 직접 더하지
 * 않고, 같은 LUN의 다음 I/O가 next_lun_avail_time까지 기다리게 된다.
 * 호출: clean_one_block(), clean_one_block_fdp_style() (ftl-fdp.c)
 */
void gc_read_page(struct ssd *ssd, struct ppa *ppa)
{
    /* advance ssd status, we don't care about how long it takes */
    if (ssd->sp.enable_gc_delay) {
        struct nand_cmd gcr;
        gcr.type = GC_IO;
        gcr.cmd = NAND_READ;
        gcr.stime = 0;
        ssd_advance_status(ssd, ppa, &gcr);
    }
}

/* move valid page data (already in DRAM) from victim line to a new page */
/* true when the page was relocated; false when there is nowhere to put it */
/*
 * victim의 valid page 하나를 새 위치로 옮긴다(GC relocation).
 * 호출: clean_one_block()
 * 흐름: ① 목적지 pointer 결정 → ② 새 PPA가 없으면 false(page 그대로)
 *       → ③ gc_relocate_commit()으로 maptbl/rmap 갱신
 *       → ④ old page invalid + rmap 비움 → ⑤ new page valid, pointer 전진
 *       → ⑥ NAND program 지연 부과(enable_gc_delay일 때)
 * 반환: true = 옮김, false = 옮길 곳 없음
 */
static bool gc_write_page(struct ssd *ssd, struct ppa *old_ppa)
{
    struct ppa new_ppa;
    struct nand_lun *new_lun;
    uint64_t lpn = get_rmap_ent(ssd, old_ppa);
    uint64_t tag = get_line(ssd, old_ppa)->stream_tag;
    struct write_pointer *stream_wp = NULL;

    ftl_assert(valid_lpn(ssd, lpn));
    /* ① stream tag가 있으면 그 stream의 pointer로(분리 유지), 아니면 */
    /*    데이터 pointer ssd->wp(line이 없으면 free line을 다시 붙여 봄) */
    if (tag) {
        stream_wp = ssd_stream_gc_pointer(ssd, tag);
        new_ppa = ssd_stream_pointer_page(ssd, stream_wp, tag);
    } else if (!ssd->wp.curline) {
        new_ppa = ssd_stream_pointer_page(ssd, &ssd->wp, 0);
    } else {
        new_ppa = get_new_page(ssd);
    }
    /* ② INVALID_PPA == UNMAPPED_PPA라 mapped_ppa()로 "자리 없음"을 판별 */
    if (!mapped_ppa(&new_ppa)) {
        /*
         * Relocating with nowhere to relocate to used to mark a page valid a
         * second time, which carried the line's valid count past its capacity
         * -- and the test that returns a line to collection is an equality,
         * so that line was never collected again. Leave the page where it is.
         */
        return false;
    }
    /* ③ page/dftl scheme: maptbl[lpn] = new, rmap[new] = lpn */
    /* commit the relocated mapping through the active scheme (maptbl + rmap) */
    ssd->mapping->gc_relocate_commit(ssd, lpn, old_ppa, &new_ppa);
    /* ④ victim line의 vpc가 여기서 하나씩 줄어든다 */
    /*
     * Retire the old copy now rather than at the erase: if the line cannot be
     * emptied it goes back to the victim queue, and its counts must then say
     * what it still holds.
     */
    mark_page_invalid(ssd, old_ppa);
    set_rmap_ent(ssd, INVALID_LPN, old_ppa);
    if (exp_lpn_watched(lpn)) {
        exp_watch_blk[new_ppa.g.blk] = 1; /* track the new block too */
        EXP_LOG("[GC_MOVE] lpn=%lu " PPA_FMT " -> " PPA_FMT "\n",
                lpn, PPA_ARG(old_ppa), PPA_ARG(&new_ppa));
    }

    /* ⑤ GC write도 host write와 같은 순서(ch → lun → pl → pg)로 채운다 */
    mark_page_valid(ssd, &new_ppa);
    ssd->gc_write_pages++; /* write amplification: a page the device relocated */

    /* need to advance the write pointer here */
    if (stream_wp) {
        ssd_advance_write_pointer_common(ssd, stream_wp);
        if (stream_wp->curline) {
            stream_wp->curline->stream_tag = tag;
        }
    } else {
        ssd_advance_write_pointer(ssd);
    }

    /* ⑥ stime = 0(지금)으로 program 지연을 LUN에 부과, 반환값은 버림 */
    if (ssd->sp.enable_gc_delay) {
        struct nand_cmd gcw;
        gcw.type = GC_IO;
        gcw.cmd = NAND_WRITE;
        gcw.stime = 0;
        ssd_advance_status(ssd, &new_ppa, &gcw);
    }

    /* advance per-ch gc_endtime as well */
#if 0
    new_ch = get_ch(ssd, &new_ppa);
    new_ch->gc_endtime = new_ch->next_ch_avail_time;
#endif

    new_lun = get_lun(ssd, &new_ppa);
    new_lun->gc_endtime = new_lun->next_lun_avail_time;

    return true;
}

/*
 * [greedy] valid page가 가장 적은 line = vpc min-heap의 top을 고른다.
 * 닫힌 line은 vpc + ipc = pgs_per_line이라 "vpc 최소 = ipc 최대"이다.
 * 호출: do_gc() (ssd->policy, gc_policy=greedy 또는 미지정)
 * 흐름: ① top peek → ② 비강제면 ipc 기준 확인 → ③ pop
 * 반환: victim line(어느 list에도 없음), 없거나 보류면 NULL
 */
static struct line *select_victim_line(struct ssd *ssd, bool force)
{
    struct line_mgmt *lm = &ssd->lm;
    struct line *victim_line = NULL;

    victim_line = pqueue_peek(lm->victim_line_pq);
    if (!victim_line) {
        return NULL;
    }

    /* ② background GC: invalid가 line의 1/8 미만이면 보류(회수량이 적음) */
    if (!force && victim_line->ipc < ssd->sp.pgs_per_line / 8) {
        return NULL;
    }

    /* ③ heap에서 빼고 pos = 0 → victim은 어느 list에도 없는 상태 */
    pqueue_pop(lm->victim_line_pq);
    victim_line->pos = 0;
    lm->victim_line_cnt--;

    /* victim_line is a danggling node now */
    return victim_line;
}

/* Alternative victim policy: pick a random victim (selectable via
 * gc_policy=random). Demonstrates the policy vtable; greedy stays the default. */
/*
 * [random] 점수 없이 victim pq에서 무작위로 하나를 뽑는다(ftl_gc_rand).
 * 비강제 GC에서 ipc 기준(1/8) 미달이면 뽑은 line을 pq에 되돌린다.
 * 호출: do_gc() (gc_policy=random)
 */
static struct line *select_victim_line_random(struct ssd *ssd, bool force)
{
    struct line_mgmt *lm = &ssd->lm;
    struct line *victim_line = pqueue_randpop(lm->victim_line_pq,
                                              ftl_gc_rand(ssd));

    if (!victim_line) {
        return NULL;
    }
    if (!force && victim_line->ipc < ssd->sp.pgs_per_line / 8) {
        pqueue_insert(lm->victim_line_pq, victim_line);
        return NULL;
    }
    victim_line->pos = 0;
    lm->victim_line_cnt--;
    return victim_line;
}

/*
 * Cost-benefit victim policy: pick the line maximizing age * (1 - u) / (2u),
 * where u = vpc/pgs_per_line. P cancels, so the rank is age * ipc / (2 * vpc);
 * lines are compared by the cross-multiplied integer form to avoid division and
 * float on the GC path. A line with vpc == 0 is the ideal victim. The victim
 * pqueue is keyed by vpc (greedy), so CB scans its bounded backing heap.
 */
/*
 * [cost-benefit] 점수 = age × ipc / vpc (상수 2 생략)가 최대인 line
 *   age = now − close_time (ns, QEMU_CLOCK_REALTIME)
 *   vpc = 0인 line은 복사 비용이 0이라 무조건 최우선
 * 호출: do_gc() (gc_policy=cost-benefit)
 * 흐름: ① heap 배열 전체 비교(O(n)) → ② 비강제면 ipc 기준 확인
 *       → ③ pqueue_remove()로 heap 중간에서 제거
 */
static struct line *select_victim_line_cb(struct ssd *ssd, bool force)
{
    struct line_mgmt *lm = &ssd->lm;
    pqueue_t *pq = lm->victim_line_pq;
    uint64_t now = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
    struct line *best = NULL;
    uint64_t best_age = 0, best_inv = 0, best_val = 1;

    /* heap is 1-indexed and size is (count + 1), so valid slots are [1, size) */
    /* ① pq 순서(vpc key)는 무시하고 후보 전체를 훑는다 */
    for (size_t i = 1; i < pq->size; i++) {
        struct line *ln = pq->d[i];
        uint64_t age = now - ln->close_time;
        uint64_t inv = ln->ipc;
        uint64_t val = ln->vpc;
        bool better;

        if (val == 0) {
            /* fully invalid: ideal victim (free reclaim), always wins */
            better = true;
        } else if (best && best_val == 0) {
            /* incumbent is already a free-reclaim line; keep it */
            better = false;
        } else {
            /* compare age*(1-u)/(2u) ~ age*inv/val via cross-multiply */
            /* a·i/v > ba·bi/bv ⇔ a·i·bv > ba·bi·v (v > 0), 128-bit 곱 */
            better = !best || (__uint128_t)age * inv * best_val >
                              (__uint128_t)best_age * best_inv * val;
        }
        if (better) {
            best = ln;
            best_age = age;
            best_inv = inv;
            best_val = val;
        }
    }

    if (!best) {
        return NULL;
    }
    /* ② 비강제면 ipc 1/8 기준 → ③ heap 중간에서 제거 */
    if (!force && best->ipc < ssd->sp.pgs_per_line / 8) {
        return NULL;
    }
    pqueue_remove(pq, best);
    best->pos = 0;
    lm->victim_line_cnt--;
    return best;
}

/*
 * FIFO victim selection: reclaim the line that was closed earliest, regardless
 * of validity -- the simplest age-based policy. Under this policy the victim
 * queue is ordered by close_seq, which never changes while a line is queued,
 * so the oldest line is at the top.
 */
/*
 * [fifo] 가장 먼저 닫힌 line(close_seq 최소 = heap top)을 고른다.
 * vpc를 보지 않으므로 valid page가 많은 line도 victim이 될 수 있다.
 * 호출: do_gc() (gc_policy=fifo, by_close_order=true)
 */
static struct line *select_victim_line_fifo(struct ssd *ssd, bool force)
{
    struct line_mgmt *lm = &ssd->lm;
    struct line *best = pqueue_peek(lm->victim_line_pq);

    if (!best) {
        return NULL;
    }
    /* top 하나만 보므로 가장 오래된 line이 기준 미달이면 이번 GC는 쉰다 */
    if (!force && best->ipc < ssd->sp.pgs_per_line / 8) {
        return NULL;
    }
    pqueue_pop(lm->victim_line_pq);
    best->pos = 0;
    lm->victim_line_cnt--;
    return best;
}

/*
 * D-Choice victim selection (random d-sample then greedy): sample DCHOICE_D
 * random candidates from the victim heap and pick the one with the fewest valid
 * pages. A cheap O(d) approximation of greedy that avoids full-heap scans; d=4 is
 * the common choice in the literature.
 */
#define FEMU_DCHOICE_D 4
/*
 * [d-choice] heap에서 무작위 d(= FEMU_DCHOICE_D = 4)개를 뽑아(중복 허용)
 * 그중 vpc 최소를 고른다. greedy의 O(d) 근사.
 * 호출: do_gc() (gc_policy=d-choice)
 * 흐름: ① d개 표본 → ② 비강제면 ipc 기준 확인 → ③ pqueue_remove()
 */
static struct line *select_victim_line_dchoice(struct ssd *ssd, bool force)
{
    struct line_mgmt *lm = &ssd->lm;
    pqueue_t *pq = lm->victim_line_pq;
    struct line *best = NULL;
    size_t n = (pq->size > 1) ? pq->size - 1 : 0; /* heap slots [1, size) */
    int d = FEMU_DCHOICE_D;

    if (n == 0) {
        return NULL;
    }
    /* ① heap index 1..n에서 균일 표본, vpc가 더 작으면 교체 */
    for (int s = 0; s < d; s++) {
        size_t idx = 1 + ftl_gc_rand(ssd) % n;
        struct line *ln = pq->d[idx];
        if (!best || ln->vpc < best->vpc) {
            best = ln;
        }
    }
    if (!best) {
        return NULL;
    }
    /* ② 비강제면 ipc 1/8 기준 → ③ heap 중간에서 제거 */
    if (!force && best->ipc < ssd->sp.pgs_per_line / 8) {
        return NULL;
    }
    pqueue_remove(pq, best);
    best->pos = 0;
    lm->victim_line_cnt--;
    return best;
}

/*
 * gc_policy 이름 → victim 선택 함수 표.
 * ssd_init()이 femu_ftl_policy_lookup()으로 하나를 골라 ssd->policy에
 * 두고, do_gc()가 ssd->policy->select_victim_line()으로 부른다.
 * fifo만 by_close_order = true → victim pq key가 close_seq가 된다.
 */
static const struct femu_ftl_policy_ops femu_ftl_policies[] = {
    { .name = "greedy", .select_victim_line = select_victim_line },
    { .name = "random", .select_victim_line = select_victim_line_random },
    { .name = "cost-benefit", .select_victim_line = select_victim_line_cb },
    { .name = "fifo", .select_victim_line = select_victim_line_fifo,
      .by_close_order = true },
    { .name = "d-choice", .select_victim_line = select_victim_line_dchoice },
};

/*
 * Next number for the policies that sample victims (splitmix64). It is seeded
 * from gc_seed, not the clock, so the same workload picks the same victims.
 */
/*
 * 상태: ssd->gc_rng (ssd_init()이 gc_seed 속성으로 초기화, 기본 0)
 * 호출: select_victim_line_random(), select_victim_line_dchoice(),
 *       select_victim_ru() (ftl-fdp.c)
 */
uint64_t ftl_gc_rand(struct ssd *ssd)
{
    uint64_t z = (ssd->gc_rng += 0x9e3779b97f4a7c15ULL);

    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
}

/* Resolve a gc_policy name to its ops; default to greedy for NULL/empty/unknown. */
/*
 * 호출: ssd_init() (ftl.c). bb/kvssd/csd 경로에서는 모르는 이름을
 * bb_check_geometry()가 femu_ftl_policy_known()으로 먼저 거부한다.
 */
const struct femu_ftl_policy_ops *femu_ftl_policy_lookup(const char *name)
{
    if (name && name[0]) {
        for (size_t i = 0; i < ARRAY_SIZE(femu_ftl_policies); i++) {
            if (strcmp(name, femu_ftl_policies[i].name) == 0) {
                return &femu_ftl_policies[i];
            }
        }
    }
    return &femu_ftl_policies[0]; /* greedy */
}

/* Is this a collection policy the device has? See femu_mapping_scheme_known. */
/* 호출: bb_check_geometry() (ftl-geom.c). NULL/빈 이름은 기본값 → true */
bool femu_ftl_policy_known(const char *name)
{
    if (!name || !name[0]) {
        return true;
    }
    for (size_t i = 0; i < ARRAY_SIZE(femu_ftl_policies); i++) {
        if (!strcmp(name, femu_ftl_policies[i].name)) {
            return true;
        }
    }

    return false;
}

/* move a block's valid pages out; false when one had nowhere to go */
/*
 * victim line에 속한 block 하나(ch/lun/pl 고정)의 page를 차례로 보며
 * VALID page만 gc_read_page() → gc_write_page()로 옮긴다.
 * 호출: reclaim_line()
 * 반환: true = 이 block의 valid page를 모두 옮김, false = 옮길 곳 없음
 */
static bool clean_one_block(struct ssd *ssd, struct ppa *ppa)
{
    struct ssdparams *spp = &ssd->sp;
    struct nand_page *pg_iter = NULL;

    for (int pg = 0; pg < spp->pgs_per_blk; pg++) {
        ppa->g.pg = pg;
        pg_iter = get_pg(ssd, ppa);
        /* 닫힌 line은 모든 page가 program됐거나 봉인(INVALID)되어 있다 */
        /* there shouldn't be any free page in victim blocks */
        ftl_assert(pg_iter->status != PG_FREE);
        if (pg_iter->status == PG_VALID) {
            gc_read_page(ssd, ppa);
            if (!gc_write_page(ssd, ppa)) {
                return false;
            }
        }
    }

    ftl_assert(get_blk(ssd, ppa)->vpc == 0);
    return true;
}

/*
 * erase가 끝난 line의 집계를 0으로 돌리고 free list 끝에 넣는다.
 * 호출: reclaim_line(), kv_reclaim_empty_lines() (kvssd-ftl.c)
 */
void mark_line_free(struct ssd *ssd, struct ppa *ppa)
{
    struct line_mgmt *lm = &ssd->lm;
    struct line *line = get_line(ssd, ppa);
    /*
     * If the read path had queued this line for a refresh, that request dies
     * with the line: it is about to be erased and recycled, which is exactly
     * what the refresh would have achieved.
     */
    if (ssd->read_reclaim_line == line) {
        ssd->read_reclaim_line = NULL;
        ssd->reclaim_by_age = false;
    }

    line->ipc = 0;
    line->vpc = 0;
    line->close_time = 0;
    line->stream_tag = 0;
    /* move this line to free line list */
    QTAILQ_INSERT_TAIL(&lm->free_line_list, line, entry);
    lm->free_line_cnt++;
}

/* return a line that could not be emptied to the list its counts call for */
/*
 * 호출: reclaim_line() (relocation 실패 시)
 * 일부 page가 이미 옮겨졌다면 그만큼 ipc가 늘어 victim pq로 간다.
 */
static void requeue_line(struct ssd *ssd, struct line *line)
{
    struct line_mgmt *lm = &ssd->lm;

    if (line->vpc == ssd->sp.pgs_per_line) {
        QTAILQ_INSERT_TAIL(&lm->full_line_list, line, entry);
        lm->full_line_cnt++;
    } else {
        pqueue_insert(lm->victim_line_pq, line);
        lm->victim_line_cnt++;
    }
}

/*
 * Relocate a line's valid pages, erase it, and return it to the free list.
 * Every page is moved before any block is erased: with nowhere left to put
 * one, the line is requeued as it stands and false returned. Erasing then
 * would destroy the pages still in it while their mappings point there.
 */
/*
 * 호출: do_gc(), do_read_reclaim()
 * 흐름: ① ch → lun → pl 순으로 block마다 clean_one_block()
 *       (실패하면 requeue_line() 후 false) → ② 모든 ch/LUN에서 같은
 *       block index erase(ssd_erase_lun_block) → ③ mark_line_free()
 * 반환: true = line 하나 회수, false = 옮길 곳이 없어 중단
 */
static bool reclaim_line(struct ssd *ssd, struct line *victim_line)
{
    struct ssdparams *spp = &ssd->sp;
    struct ppa ppa;
    int ch, lun, pl;

    /*
     * Only some fields are filled in below, so start from zero rather than
     * from the stack: the sector and reserved fields are copied into the
     * batch this builds and passed on to mark_line_free(). Nothing reads
     * them today, but valid_ppa() does check the sector, so the day anything
     * calls it on one of these the answer would come from whatever the stack
     * happened to hold.
     */
    ppa.ppa = 0;
    ppa.g.blk = victim_line->id;

    /* ① vpc가 0이 되면 남은 block은 볼 필요 없다(조건에 vpc 포함) */
    for (ch = 0; ch < spp->nchs && victim_line->vpc; ch++) {
        for (lun = 0; lun < spp->luns_per_ch; lun++) {
            ppa.g.ch = ch;
            ppa.g.lun = lun;
            for (pl = 0; pl < spp->pls_per_lun; pl++) {
                ppa.g.pl = pl;
                if (!clean_one_block(ssd, &ppa)) {
                    requeue_line(ssd, victim_line);
                    return false;
                }
            }
        }
    }

    /* ② 전부 옮긴 뒤에만 erase. 지연은 enable_gc_delay일 때만 부과 */
    for (ch = 0; ch < spp->nchs; ch++) {
        for (lun = 0; lun < spp->luns_per_ch; lun++) {
            ssd_erase_lun_block(ssd, ch, lun, ppa.g.blk, spp->enable_gc_delay,
                                0);
        }
    }

    /* ③ ppa.g.blk = victim id → mark_line_free()의 get_line()이 이 line */
    /* update line status */
    mark_line_free(ssd, &ppa);
    return true;
}

/*
 * non-FDP GC 1회: victim line 하나를 골라 비우고 free list로 돌린다.
 * 호출: bb_ftl_process_req() (ftl.c, 요청 처리 뒤 should_gc()면
 *       force = false), ssd_write()·ssd_write_zeroes()·
 *       ssd_gc_until_clear() (ftl-datapath.c, should_gc_high()인 동안
 *       force = true로 반복)
 * 흐름: ① 정책 함수로 victim 선택 → ② streams 모드 보류 조건
 *       → ③ reclaim_line() → ④ streams 모드에서 free line 증가 확인
 * 반환: 0 = line 하나 회수, -1 = victim 없음/보류/실패(호출자 루프 중단)
 */
int do_gc(struct ssd *ssd, bool force)
{
    int free_lines = ssd->lm.free_line_cnt;
    struct line *victim_line = ssd->policy->select_victim_line(ssd, force);

    /* ① victim 선택은 위 선언의 ssd->policy->select_victim_line() */
    if (!victim_line) {
        return -1;
    }

    /* ② streams: 옮길 page가 있는데 free line이 0이면 pq로 되돌려 보류 */
    if (ssd->n->streams && victim_line->vpc && !ssd->lm.free_line_cnt) {
        pqueue_insert(ssd->lm.victim_line_pq, victim_line);
        ssd->lm.victim_line_cnt++;
        return -1;
    }

    ftl_debug("GC-ing line:%d,ipc=%d,victim=%d,full=%d,free=%d\n",
              victim_line->id, victim_line->ipc, ssd->lm.victim_line_cnt,
              ssd->lm.full_line_cnt, ssd->lm.free_line_cnt);

    /* ③ 실패하면 reclaim_line()이 line을 이미 list에 되돌려 두었다 */
    if (!reclaim_line(ssd, victim_line)) {
        return -1;
    }

    /* ④ free line이 늘지 않았으면 -1 → 호출자의 GC 반복 루프가 멈춘다 */
    /* Distinct retired streams may occupy lines that cannot be combined. */
    if (ssd->n->streams && ssd->lm.free_line_cnt <= free_lines) {
        return -1;
    }
    return 0;
}

/*
 * Rewrite a line whose data has become worth refreshing, for either reason.
 *
 * Reading a page stresses the others in its block, so a block read many times
 * without being rewritten drifts towards errors. Charge also leaks out of a
 * cell on its own, so data that has simply sat programmed long enough drifts
 * the same way whether or not anything reads it. Real devices watch for both
 * and rewrite the data before it decays; the cost is relocation, which is why
 * this shows up as write amplification rather than as anything the host sees
 * directly. The two are counted apart -- read_reclaims and retention_refreshes
 * -- so a workload can tell which pressure it is paying for.
 *
 * A line nothing ever reads is never refreshed here: both triggers sit on the
 * read path. Modelling the background media scan a real device runs would need
 * a timer of its own, which this deliberately does not add.
 *
 * The line is chosen by the read path and rewritten here, on a write, because
 * this is where collection already happens and where the cost belongs. Doing it
 * inside the read would stall a read behind a whole line of relocation.
 *
 * Relocation goes through the ordinary data write pointer. An earlier attempt
 * at static wear levelling gave itself a dedicated pointer, which pinned a line
 * out of circulation and drove the device into a collection death spiral; there
 * is no reason to repeat that here.
 */
/*
 * read 경로(ssd_read_check_wear)가 예약한 line 하나를 GC처럼 재기록한다.
 * 호출: ssd_write() (ftl-datapath.c, 쓰기 명령마다 최대 1 line)
 * 흐름: ① 예약 확인 → ② free line 여유 확인 → ③ 예약 해제
 *       → ④ full list / victim pq에서 꺼냄 → ⑤ reclaiming 표시 후
 *       reclaim_line() → ⑥ 원인별 카운터 증가
 * 반환: 0 = 재기록 완료, -1 = 대상 없음/보류/실패
 */
int do_read_reclaim(struct ssd *ssd)
{
    struct line *line = ssd->read_reclaim_line;
    bool by_age;

    /* ① ssd_read_check_wear()가 read_cnt 또는 retention 기준으로 예약 */
    if (!line) {
        return -1;
    }

    /* ② 여기서 보류(-1)하면 예약이 남아 다음 쓰기에서 다시 시도한다 */
    /*
     * Only with lines to spare. The test is the high watermark, not
     * should_gc(): a busy device sits below should_gc() permanently, so testing
     * that would disable this outright.
     */
    if (should_gc_high(ssd) || ssd->lm.free_line_cnt < 2) {
        return -1;
    }

    /* ③ 예약은 한 번에 하나: 비워야 read 경로가 다음 line을 예약한다 */
    ssd->read_reclaim_line = NULL;
    by_age = ssd->reclaim_by_age;
    ssd->reclaim_by_age = false;

    /* ④ full list / victim pq(pos ≠ 0)에서 꺼낸다. 그 외 = 쓰는 중인 line */
    /*
     * A heavily read line is usually full, and invalidating the first page of a
     * full line moves it from full_line_list into the victim queue half way
     * through the rewrite. Take it out and mark it so that does not happen;
     * a line from select_victim_line() already arrives in no list.
     */
    if (line->vpc == ssd->sp.pgs_per_line) {
        QTAILQ_REMOVE(&ssd->lm.full_line_list, line, entry);
        ssd->lm.full_line_cnt--;
    } else if (line->pos) {
        pqueue_remove(ssd->lm.victim_line_pq, line);
        line->pos = 0;
        ssd->lm.victim_line_cnt--;
    } else {
        /* being written to right now: leave it alone */
        return -1;
    }

    /* ⑤ reclaiming 동안 mark_page_invalid()가 full → victim 이동을 안 함 */
    line->reclaiming = true;
    if (!reclaim_line(ssd, line)) {
        line->reclaiming = false;
        return -1;
    }
    line->reclaiming = false;
    /* ⑥ by_age: retention refresh, 아니면 read disturb에 의한 read reclaim */
    if (by_age) {
        ssd->retention_refreshes++;
    } else {
        ssd->read_reclaims++;
    }

    return 0;
}

/* release what ssd_init_lines() took */
/* 호출: ssd_free() (ftl.c) */
void ssd_free_lines(struct ssd *ssd)
{
    struct line_mgmt *lm = &ssd->lm;

    pqueue_free(lm->victim_line_pq);
    lm->victim_line_pq = NULL;
    g_free(lm->lines);
    lm->lines = NULL;
    lm->tt_lines = 0;
    lm->free_line_cnt = 0;
    lm->victim_line_cnt = 0;
    lm->full_line_cnt = 0;
}
