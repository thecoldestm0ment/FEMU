/*
 * bbssd FTL: Flexible Data Placement (FDP) implementation. Owns the reclaim
 * group / reclaim unit / RU handle machinery, the FDP-aware write, GC, and trim
 * paths, and their initialization. Split out of ftl.c last so the public FDP
 * behavior is preserved verbatim; the FTL thread dispatch, mapping, line/GC, and
 * media interfaces come from ftl-internal.h and the media layer.
 */
/*
 * [한국어 학습 주석] FDP(Flexible Data Placement) FTL
 * 역할: 호스트가 준 placement 정보(PID → RG, RUH)에 따라 page를 RU에
 *       배치하고, RU 단위로 GC / trim / 통계 / FDP event를 처리한다.
 *       line 단위 write pointer를 쓰는 non-FDP 경로(ftl-datapath.c,
 *       ftl-line-gc.c)와 나란히 있는 별도 경로다(ssd->fdp_enabled).
 * 주요 진입 함수(괄호 = 호출하는 곳):
 *   - nvme_do_write_fdp()          : Write, Copy의 쓰기 (ftl.c)
 *   - ssd_write_zeroes_fdp_style() : Write Zeroes (ftl.c)
 *   - ssd_trim_fdp_style()         : DSM Deallocate = trim (ftl.c)
 *   - ssd_fdp_update_ruhs()        : I/O Mgmt Send의 RUH Update (ftl.c)
 *   - do_gc_fdp_style()            : background GC (ftl.c)
 *   - ssd_init_fdp_params() → femu_fdp_ssd_init_reclaim_group()
 *     → femu_fdp_ssd_init_ru_handles() : 초기화 3단계 (ssd_init)
 *   - ssd_deallocate_fdp_all()     : 전체 unmap (ftl-datapath.c)
 *   - femu_fdp_ssd_free()          : 해제 (ssd_free)
 * 의존: ftl-internal.h(ssd_wp_step 등 write pointer, 매핑 accessor,
 *       should_gc_*_fdp_style), ftl-line-gc.c(get_next_free_line,
 *       gc_read_page, ssd_erase_lun_block), nvme-io.c(nvme_parse_pid,
 *       nvme_fdp_record_event, nvme_fdp_note_ru_left)
 * 객체 관계: RG가 RU 배열과 free/full/victim 목록(ru_mgmt)을 갖고,
 *       RUH는 그중 지금 쓰는 RU(curr_ru, PI는 GC용 gc_ru도)를 가리킨다.
 *       line->my_ru로 PPA → line → RU → RUH 역추적이 가능하다.
 * 관련 노트(예전 단일 ftl.c 기준이라 줄 번호는 맞지 않음):
 *   01 객체·포인터, 02 초기화, 03 PID 해석, 04 쓰기·RU 교체,
 *   05 무효화·victim queue, 06 GC·격리, 07 통계·event·trim,
 *   08 통합 추적
 * 자세한 설명: 참고자료/code_study/15_fdp_code_map.md
 */
#include "qemu/osdep.h"
#include "ftl.h"
#include "ftl-internal.h"

/* internal FDP forward declarations (the exported ones live in ftl-internal.h) */
static void mark_page_valid_fdp(struct ssd *ssd, struct ppa *ppa,
                                FemuReclaimUnit *ru);
static void mark_page_invalid_fdp(struct ssd *ssd, struct ppa *ppa);
static void ssd_reset_maptbl(struct ssd *ssd);

/* FDP: victim RU priority queue callbacks (greedy by vpc) */
/*
 * per-RG victim heap(victim_ru_pq)용 콜백: 우선순위 = vpc.
 * cmp가 (next > curr)이라 부모가 더 크면 올라가므로 vpc가 가장 작은
 * RU가 heap 맨 앞(greedy victim)이 된다. 위치 필드는 pos.
 * 자세한 설명: code_study/05_FDP_invalidation_and_queues.md §6~§7
 */
static inline int victim_ru_cmp_pri(pqueue_pri_t next, pqueue_pri_t curr)
{
    return (next > curr);
}

static inline pqueue_pri_t victim_ru_get_pri(void *a)
{
    return ((FemuReclaimUnit *)a)->vpc;
}

static inline void victim_ru_set_pri(void *a, pqueue_pri_t pri)
{
    ((FemuReclaimUnit *)a)->vpc = pri;
}

static inline size_t victim_ru_get_pos(void *a)
{
    return ((FemuReclaimUnit *)a)->pos;
}

static inline void victim_ru_set_pos(void *a, size_t pos)
{
    ((FemuReclaimUnit *)a)->pos = pos;
}

/*
 * A PI-type RUH keeps its full RUs in BOTH the per-RG (global) victim pqueue
 * and its own per-RUH victim pqueue simultaneously. The two heaps must track
 * each RU's index independently, so the per-RUH queues use ruh_pos via these
 * callbacks. Sharing the single `pos` field across both heaps (issue #189)
 * corrupted whichever heap was touched second and crashed in
 * victim_ru_get_pri(NULL).
 */
/* per-RUH heap(PI RUH의 ru_mgmt) 전용: pos 대신 ruh_pos 사용 */
static inline size_t victim_ru_get_pos_ruh(void *a)
{
    return ((FemuReclaimUnit *)a)->ruh_pos;
}

static inline void victim_ru_set_pos_ruh(void *a, size_t pos)
{
    ((FemuReclaimUnit *)a)->ruh_pos = pos;
}

/* FDP: victim RU priority queue callbacks (cost-benefit by my_cb) */
/*
 * CB heap(victim_ru_cb)용 콜백: 우선순위 = my_cb. 다만 지금의 CB victim
 * 선택은 select_victim_ru()가 heap 전체를 훑으며 점수를 새로 계산한다.
 */
static inline int victim_ru_cmp_pri_by_cb(pqueue_pri_t next, pqueue_pri_t curr)
{
    return (next > curr);
}

static inline pqueue_pri_t victim_ru_get_pri_by_cb(void *a)
{
    /* cast float to pqueue_pri_t for ordering */
    return (pqueue_pri_t)((FemuReclaimUnit *)a)->my_cb;
}

static inline void victim_ru_set_pri_by_cb(void *a, pqueue_pri_t pri)
{
    ((FemuReclaimUnit *)a)->my_cb = (float)pri;
}

/* ========== FDP FTL Implementation ========== */

/*
 * get_next_free_ru - dequeue a free RU from a reclaim group
 */
/*
 * RG의 free_ru_list 맨 앞 RU를 꺼낸다(RUH와 무관한 RG 공용 pool).
 * 호출: fdp_get_new_ru()
 * 반환: 꺼낸 RU, free RU가 없으면 NULL
 */
static FemuReclaimUnit *get_next_free_ru(struct ssd *ssd,
                                         FemuReclaimGroup *rg)
{
    struct ru_mgmt *rm = rg->ru_mgmt;
    FemuReclaimUnit *ru;

    ru = QTAILQ_FIRST(&rm->free_ru_list);
    if (!ru) {
        ftl_err("No free RUs left in rg[%d]\n", rg->rgidx);
        return NULL;
    }

    QTAILQ_REMOVE(&rm->free_ru_list, ru, entry);
    rm->free_ru_cnt--;
    return ru;
}

/*
 * fdp_set_ru_write_pointer - reset RU write pointer to first line
 */
/*
 * RU write pointer를 첫 line의 page 0으로 되돌린다.
 * 호출: fdp_get_new_ru(), mark_ru_free()
 */
static void fdp_set_ru_write_pointer(struct ssd *ssd, FemuReclaimUnit *ru)
{
    struct write_pointer *wptr = ru->ssd_wptr;

    ftl_assert(wptr != NULL);
    ssd_wp_reset(wptr, ru->lines[0]);
}

/*
 * fdp_get_new_ru - allocate a fresh free RU for a given RUH
 */
/*
 * RG의 free RU 하나를 RUH에 배정하고 RU 메타데이터를 새로 초기화한다.
 * 호출: fdp_advance_ru_pointer(), fdp_gc_frontier(),
 *       ssd_stream_write_lpns(), ssd_fdp_update_ruhs(),
 *       femu_fdp_ssd_init_ru_handles(), ssd_trim_fdp_reset_all()
 * 주의: RUH의 curr_ru / rus[] / NVMe 쪽 rus[]는 바꾸지 않는다(호출자
 *       책임). 예전 버전은 여기서 NVMe 쪽 rus[]를 바꿨다.
 * 반환: 새 RU, free RU가 없으면 NULL
 * 자세한 설명: code_study/04_FDP_write_and_RU_rotation.md §6
 */
static FemuReclaimUnit *fdp_get_new_ru(struct ssd *ssd, uint16_t rgidx,
                                       uint16_t ruhid)
{
    FemuRuHandle *eruh = &ssd->ruhs[ruhid];
    FemuReclaimGroup *rg = &ssd->rg[rgidx];
    FemuReclaimUnit *new_ru;

    new_ru = get_next_free_ru(ssd, rg);
    if (!new_ru) {
        ftl_err("No reclaim unit available for ruh %d\n", ruhid);
        return NULL;
    }
    new_ru->rgidx = rgidx;
    new_ru->ruh = eruh;
    new_ru->last_init_time = qemu_clock_get_us(QEMU_CLOCK_REALTIME);
    new_ru->last_invalidated_time = 0;
    new_ru->erase_cnt = 0;
    new_ru->my_cb = 0.0f;
    new_ru->chance_token = 0;

    fdp_set_ru_write_pointer(ssd, new_ru);
    eruh->ru_in_use_cnt++;

    /*
     * The host-visible active RU (NvmeRuHandle.rus) is published by the
     * caller that makes this the handle's current RU; a collection
     * destination allocated here must not replace it.
     */
    ftl_assert(new_ru->ruh == eruh);
    return new_ru;
}

/*
 * The handle's active unit filled with nothing free to follow it. Point both
 * views away from the retired unit: collection can free it and hand it to
 * another handle, whose remaining room the host would then be shown here.
 */
/*
 * RUH의 활성 RU를 비운다: FTL 쪽 curr_ru, rus[]는 NULL로, NVMe 쪽은
 * no_ru(남은 쓰기량 0인 dummy)를 가리키게 해 호스트에 "공간 없음"을 보인다.
 * 호출: gc_write_page_fdp_style()(II 경로), ssd_stream_write_lpns()
 */
static void fdp_drop_active_ru(FemuRuHandle *ruh, uint16_t rgidx)
{
    ruh->curr_ru = NULL;
    ruh->rus[rgidx] = NULL;
    ruh->ruh->rus[rgidx] = &ruh->no_ru;
}

/*
 * fdp_get_new_page - get next PPA from an RU's write pointer
 */
/*
 * RU write pointer의 현재 좌표를 PPA로 복사만 한다(전진은 하지 않음).
 * 호출: gc_write_page_fdp_style(), ssd_stream_write_lpns()
 */
static struct ppa fdp_get_new_page(struct ssd *ssd, FemuReclaimUnit *ru)
{
    struct write_pointer *wpp = ru->ssd_wptr;
    struct ppa ppa;

    ftl_assert(ru != NULL);
    ftl_assert(wpp != NULL);

    ppa.ppa = 0;
    ppa.g.ch = wpp->ch;
    ppa.g.lun = wpp->lun;
    ppa.g.pg = wpp->pg;
    ppa.g.blk = wpp->blk;
    ppa.g.pl = wpp->pl;

    return ppa;
}

/*
 * fdp_retire_ru - file a unit that takes no more writes: on the full list if
 * every page it holds is valid, otherwise as a collection victim. Returns
 * whether it went on the full list.
 */
/*
 * 더 쓰지 않을 RU(다 찼거나 RUH Update로 닫힘)를 분류한다.
 * 호출: fdp_advance_ru_pointer(), ssd_fdp_update_ruhs()
 * 흐름: ① 소속 line vpc 합으로 RU vpc 재계산
 *       → ② 모든 page가 valid면 full_ru_list(아직 GC 후보 아님)
 *       → ③ 아니면 CB 전략은 victim_ru_cb, 그 외는 victim_ru_pq에 넣음
 *       → ④ NOISY 전략이면 per-RUH heap에도 같이 넣음
 * 자세한 설명: code_study/04_FDP_write_and_RU_rotation.md §5
 */
static bool fdp_retire_ru(struct ssd *ssd, FemuReclaimUnit *ru)
{
    struct ssdparams *spp = &ssd->sp;
    struct ru_mgmt *rm = ssd->rg[ru->rgidx].ru_mgmt;
    bool is_full = true;

    /* ① RU vpc = 소속 line vpc 합 (지금 설정은 RU 1개 = line 1개) */
    ru->vpc = 0;
    for (int i = 0; i < ru->n_lines; i++) {
        if (ru->lines[i]->vpc != spp->pgs_per_line) {
            is_full = false;
        }
        ru->vpc += ru->lines[i]->vpc;
    }

    /*
     * ② 전부 valid면 full list(GC 후보 아님). 나중에 invalid가 생기면
     *    mark_page_invalid_fdp()가 victim heap으로 옮긴다
     */
    if (is_full) {
        QTAILQ_INSERT_TAIL(&rm->full_ru_list, ru, entry);
        rm->full_ru_cnt++;
        return true;
    }

    /* ③ 무효 page가 있으면 바로 victim 후보 */
    ru->utilization = (float)ru->vpc / ru->npages;
    if (rm->mgmt_type == GC_GLOBAL_CB) {
        if (ru->utilization < 1.0f && ru->last_invalidated_time > 0) {
            ru->my_cb = (uint64_t)(100000.0f * ru->utilization /
                ((1.0f - ru->utilization + 0.001f) *
                (float)ru->last_invalidated_time));
        }
        pqueue_insert(rm->victim_ru_cb, ru);
    } else {
        pqueue_insert(rm->victim_ru_pq, ru);
    }
    rm->victim_ru_cnt++;
    /* ④ 예전엔 없던 복제(05 노트 §6 "두 등록 경로의 비대칭" 해소) */
    /* the per-handle policy picks from the handle's own queue first */
    if (rm->mgmt_type == GC_NOISY_RUH_CUSTOM && ru->ruh && ru->ruh->ru_mgmt) {
        pqueue_insert(ru->ruh->ru_mgmt->victim_ru_pq, ru);
        ru->ruh->ru_mgmt->victim_ru_cnt++;
    }
    return false;
}

/*
 * fdp_advance_ru_pointer - advance RU write pointer. When RU fills up,
 * move it to victim/full list and allocate a new RU for the RUH.
 * Returns the (possibly new) current RU.
 */
/*
 * page 하나를 쓴 뒤 RU write pointer를 한 칸 전진시키고, RU 끝이면
 * 같은 RG에서 같은 RUH용 새 RU로 교체한다.
 * 호출: ssd_stream_write_lpns(), gc_write_page_fdp_style()
 * 흐름: ① ssd_wp_step(): ch → lun → pl → pg 순 전진(RU 안이면 끝)
 *       → ② RU 끝: fdp_retire_ru()로 full/victim 분류
 *       → ③ fdp_get_new_ru()로 새 RU 할당
 * 반환: RU 안쪽이면 입력 ru, 교체되면 새 RU, ruh가 NULL이거나 free RU가
 *       없으면 NULL(후자는 ruh->curr_ru도 NULL로 비움)
 * 주의: curr_ru/gc_ru 갱신은 호출자가 RUH 유형에 맞게 한다
 * 자세한 설명: code_study/04_FDP_write_and_RU_rotation.md §4~§9
 */
static FemuReclaimUnit *fdp_advance_ru_pointer(struct ssd *ssd,
                                               FemuReclaimGroup *rg,
                                               FemuRuHandle *ruh,
                                               FemuReclaimUnit *ru)
{
    struct ssdparams *spp = &ssd->sp;
    struct ru_mgmt *rm = rg->ru_mgmt;
    struct write_pointer *wpp = ru->ssd_wptr;
    FemuReclaimUnit *new_ru = NULL;
    bool is_full;

    /* ① 한 RU 안에서는 channel → LUN → plane → page 순으로 퍼뜨린다 */
    /* mid-RU: the same RU keeps taking pages */
    if (!ssd_wp_step(spp, wpp)) {
        return ru;
    }
    /* ② RU를 다 씀: 이 RU는 이제 full 또는 victim 목록에 있다 */
    is_full = fdp_retire_ru(ssd, ru);

    /* ③ 새 RU는 RUH 전용 pool이 아니라 RG의 free list에서 온다 */
    /* allocate a new RU for this RUH cuase ruh->curr_ru is full */
    if (ruh != NULL) {
        check_addr(wpp->blk, spp->blks_per_pl);
        new_ru = fdp_get_new_ru(ssd, ru->rgidx, ruh->ruhid);
        if (!new_ru) {
            ftl_err("No free RU for ruh %d: device full - point %s L:%d\n",
                    ruh->ruhid, __FILE__, __LINE__);
            /*
             * Signal device pressure: clear curr_ru so
             * callers know no active write frontier exists.
             * A full device is an ordinary outcome the callers
             * turn into a capacity error, not a bug -- the
             * assertion that used to stand here aborted the
             * process in the build that arms assertions.
             */
            /* 장치 full: assert 대신 NULL을 돌려 호출자가 용량 오류로 처리 */
            ruh->curr_ru = NULL;
            return NULL;
        }
        FDP_TRACE(ssd, "RU_ROTATE ruhid=%u old_ru=%u "
                  "new_ru=%u reason=%s victim_ru_cnt %d\n",
                  ruh->ruhid, ru->ruidx, new_ru->ruidx,
                  is_full ? "full_valid" : "full_victim",
                  rm->victim_ru_cnt);
        wpp = new_ru->ssd_wptr;
        wpp->blk = wpp->curline->id;
        check_addr(wpp->blk, spp->blks_per_pl);
        ftl_assert(wpp->pg == 0);
        ftl_assert(wpp->lun == 0);
        ftl_assert(wpp->ch == 0);
        ftl_assert(wpp->pl == 0);
    }

    /*
     * The RU is retired (in full_ru_list or the victim queue): return the RU
     * that replaces it, or NULL without a handle or when the device is full.
     */
    return new_ru;
}

/*
 * mark_page_valid_fdp - mark page valid and update RU/line statistics
 */
/*
 * 새로 쓴 page를 valid로: block/line/RU vpc와 RUH live page 수 증가.
 * 호출: ssd_stream_write_lpns(), gc_write_page_fdp_style()
 * 자세한 설명: code_study/05_FDP_invalidation_and_queues.md §2
 */
static void mark_page_valid_fdp(struct ssd *ssd, struct ppa *ppa,
                                FemuReclaimUnit *ru)
{
    struct nand_block *blk = NULL;
    struct nand_page *pg = NULL;
    struct line *line;

    pg = get_pg(ssd, ppa);
    ftl_assert(pg->status == PG_FREE);
    pg->status = PG_VALID;

    blk = get_blk(ssd, ppa);
    ftl_assert(blk->vpc >= 0 && blk->vpc < ssd->sp.pgs_per_blk);
    blk->vpc++;

    line = get_line(ssd, ppa);
    ftl_assert(line->vpc >= 0 && line->vpc < ssd->sp.pgs_per_line);
    line->vpc++;

    /* update RU vpc from its line (single-line RU fast path) */
    ftl_assert(line->my_ru == ru);
    if (ru->n_lines == 1) {
        ru->vpc = line->vpc;
    } else {
        ru->vpc = 0;
        for (int i = 0; i < ru->n_lines; i++) {
            ru->vpc += ru->lines[i]->vpc;
        }
    }

    ru->ruh->ruh_live_pages_cnt++;
}

/*
 * mark_page_invalid_fdp - invalidate a page and update RU/line/victim state
 */
/*
 * 옛 page를 invalid로 만들고, 그 page가 속한 RU(line->my_ru)의 통계와
 * victim queue 상태를 갱신한다. 새 쓰기의 RUH와 다른 RU일 수 있다.
 * 호출: ssd_stream_write_lpns()(덮어쓰기), gc_write_page_fdp_style()
 *       (GC 이동의 source), ssd_deallocate_fdp_lpns()(trim)
 * 흐름: ① page → block → line 카운터 → ② line->my_ru로 RU 찾기
 *       → ③ RU ipc/vpc/utilization/무효화 시각 갱신
 *       → ④ 전략별 queue: 이미 victim이면 우선순위 갱신,
 *          full이었으면 full_ru_list → victim heap 이동
 *       → ⑤ 옛 RU의 RUH live page 수 감소
 * 자세한 설명: code_study/05_FDP_invalidation_and_queues.md §3~§9
 */
static void mark_page_invalid_fdp(struct ssd *ssd, struct ppa *ppa)
{
    struct ssdparams *spp = &ssd->sp;
    struct nand_block *blk = NULL;
    struct nand_page *pg = NULL;
    struct line *line;
    FemuReclaimUnit *ru;
    struct ru_mgmt *rm;
    bool was_full_ru = false;

    /* ① 이미 invalid면 아무것도 하지 않는다(중복 호출 허용) */
    pg = get_pg(ssd, ppa);
    if (pg->status == PG_INVALID) {
        return;  /* already invalidated */
    }
    ftl_assert(pg->status == PG_VALID);
    pg->status = PG_INVALID;

    blk = get_blk(ssd, ppa);
    ftl_assert(blk->ipc >= 0 && blk->ipc < spp->pgs_per_blk);
    blk->ipc++;
    ftl_assert(blk->vpc > 0 && blk->vpc <= spp->pgs_per_blk);
    blk->vpc--;

    line = get_line(ssd, ppa);
    ftl_assert(line->ipc >= 0 && line->ipc < spp->pgs_per_line);
    if (line->vpc == spp->pgs_per_line) {
        ftl_assert(line->ipc == 0);
    }
    line->ipc++;
    ftl_assert(line->vpc > 0 && line->vpc <= spp->pgs_per_line);
    line->vpc--;

    /* ② PPA → line → my_ru: 이 page를 담은 RU를 역추적 */
    /* update RU state */
    ru = line->my_ru;
    ftl_assert(ru != NULL);
    rm = ssd->rg[ru->rgidx].ru_mgmt;
    /* aggregate ipc across all lines in this RU (n_lines=1 in typical config) */
    ru->ipc = 0;
    for (int li = 0; li < ru->n_lines; li++) {
        ru->ipc += ru->lines[li]->ipc;
    }

    // FDP_TRACE(ssd, "INVAL ppa(ch=%u/lun=%u/blk=%u/pg=%u) "
    //           "ru=%u vpc=%d->%d pos %d was_full=%d victim_ru_cnt=%d\n",
    //           (unsigned)ppa->g.ch, (unsigned)ppa->g.lun,
    //           (unsigned)ppa->g.blk, (unsigned)ppa->g.pg,
    //           ru->ruidx, ru->vpc, ru->vpc - 1, ru->pos,
    //           (ru->vpc == spp->pgs_per_line * ru->n_lines),
    //           rm->victim_ru_cnt);

    /* ③ 감소 "전" vpc로 full RU였는지 먼저 판단(05 노트 §4) */
    /* check if RU was full and needs to move to victim */
    if (ru->vpc == spp->pgs_per_line * ru->n_lines) {
        was_full_ru = true;
    }

    /* update RU vpc and victim queue priority based on GC strategy */
    ru->vpc--;
    ru->utilization = (ru->vpc + ru->ipc > 0) ?
        (float)ru->vpc / (ru->vpc + ru->ipc) : 0.0f;
    ru->last_invalidated_time = qemu_clock_get_us(QEMU_CLOCK_REALTIME);

    /* ④ pos != 0 이면 이미 per-RG victim heap 안에 있다는 뜻 */
    switch (rm->mgmt_type) {
    case GC_GLOBAL_GREEDY:
    case GC_GLOBAL_RAND:
    case GC_NOISY_RUH_CUSTOM:
        if (ru->pos) {
            pqueue_change_priority(rm->victim_ru_pq, ru->vpc, ru);
        }
        /*
         * Only GC_NOISY_RUH_CUSTOM consults the per-RUH victim queue; keep its
         * ordering in sync there. The per-RUH heap indexes via ruh_pos, so it
         * no longer aliases the global queue's pos (issue #189). ruh_pos != 0
         * is a valid "in per-RUH queue" marker here because, for this strategy,
         * every per-RUH pop/remove is paired with a ruh_pos reset.
         */
        if (rm->mgmt_type == GC_NOISY_RUH_CUSTOM && ru->ruh_pos &&
            ru->ruh && ru->ruh->ru_mgmt) {
            pqueue_change_priority(ru->ruh->ru_mgmt->victim_ru_pq, ru->vpc, ru);
        }
        if (was_full_ru) {
            QTAILQ_REMOVE(&rm->full_ru_list, ru, entry);
            rm->full_ru_cnt--;
            pqueue_insert(rm->victim_ru_pq, ru);
            rm->victim_ru_cnt++;
            /*
             * Mirror into the per-RUH queue ONLY for the strategy that reads it
             * (GC_NOISY_RUH_CUSTOM). GREEDY/RAND never pop the per-RUH queue, so
             * a second membership there would just go stale on the global pop
             * and later corrupt that heap (issue #189).
             */
            if (rm->mgmt_type == GC_NOISY_RUH_CUSTOM &&
                ru->ruh && ru->ruh->ru_mgmt) {
               pqueue_insert(ru->ruh->ru_mgmt->victim_ru_pq, ru);
               ru->ruh->ru_mgmt->victim_ru_cnt++;
            }
        }
        break;

    case GC_GLOBAL_CB:
        if (ru->utilization < 1.0f && ru->last_invalidated_time > 0) {
            ru->my_cb = (uint64_t)(100000.0f * ru->utilization /
                ((1.0f - ru->utilization + 0.001f) *
                 (float)ru->last_invalidated_time));
        }
        if (ru->pos) {
            pqueue_change_priority(rm->victim_ru_cb, (pqueue_pri_t)ru->my_cb,
                                   ru);
        }
        if (was_full_ru) {
            QTAILQ_REMOVE(&rm->full_ru_list, ru, entry);
            rm->full_ru_cnt--;
            pqueue_insert(rm->victim_ru_cb, ru);
            rm->victim_ru_cnt++;
        }
        break;

    default:
        //Greedy
        ftl_err( "Undefined rg mgmt type (rm->mgmt_type %d). Fallback to Greedy.\n",
              rm->mgmt_type);
        if (ru->pos) {
            pqueue_change_priority(rm->victim_ru_pq, ru->vpc, ru);
        }
        if (was_full_ru) {
            QTAILQ_REMOVE(&rm->full_ru_list, ru, entry);
            rm->full_ru_cnt--;
            pqueue_insert(rm->victim_ru_pq, ru);
            rm->victim_ru_cnt++;
        }
        break;
    }
    /* ⑤ live page는 옛 page가 속했던 RUH에서 뺀다 */
    if (ru->ruh->ruh_live_pages_cnt > 0)
        ru->ruh->ruh_live_pages_cnt -=1 ;
    
}

/*
 * The unit collection relocates into for @ruh, taking a free one from @rgidx
 * when the frontier has none. A persistently isolated handle collects into a
 * unit of its own; the initially isolated one shares its active unit with the
 * host. Either is dropped when it fills with no free unit to follow it, and is
 * picked up again here once collection has freed one, without looking at the
 * handle's active unit: that may be the one dropped.
 */
/*
 * GC가 옮길 page를 받을 RU(GC frontier)를 돌려주고, 없으면 새로 받는다.
 * 예전 check_gc_ruh_available()를 대신한다.
 * 호출: do_gc_fdp_style(), gc_write_page_fdp_style()
 * 분기: PI → 같은 RUH의 gc_ru(호스트 curr_ru와 분리 = 격리 유지)
 *       II → 그 RUH의 curr_ru(호스트 쓰기와 공유). do_gc_fdp_style()은
 *            II victim이면 마지막 RUH를 넘긴다
 * 반환: frontier RU, free RU가 없으면 NULL
 * 자세한 설명: code_study/06_FDP_GC_and_isolation.md §5~§6
 */
static FemuReclaimUnit *fdp_gc_frontier(struct ssd *ssd, FemuRuHandle *ruh,
                                        uint16_t rgidx)
{
    FemuReclaimUnit *ru;

    if (ruh->ruh_type == NVME_RUHT_PERSISTENTLY_ISOLATED) {
        if (!ruh->gc_ru) {
            ruh->gc_ru = fdp_get_new_ru(ssd, rgidx, ruh->ruhid);
        }
        return ruh->gc_ru;
    }

    ftl_assert(ruh->ruh_type == NVME_RUHT_INITIALLY_ISOLATED);
    if (!ruh->curr_ru) {
        ru = fdp_get_new_ru(ssd, rgidx, ruh->ruhid);
        if (!ru) {
            return NULL;
        }
        ruh->curr_ru = ru;
        ruh->rus[rgidx] = ru;
        ruh->ruh->rus[rgidx] = ru->nvme_ru;
    }
    return ruh->curr_ru;
}

/*
 * select_victim_ru_from_ruh - pick victim from a specific RUH's queue
 */
/*
 * 특정 RUH(PI)의 per-RUH victim heap에서 하나 꺼낸다.
 * 호출: select_victim_ru()(GC_SELECTIVE_RUH_SOCIAL_WELFARE 분기)
 * 참고: per-RUH heap은 NOISY 전략일 때만 채워지고, SOCIAL_WELFARE(13)는
 *       ftl-geom.c 검사에서 거부되므로 지금은 사실상 쓰이지 않는다.
 */
static FemuReclaimUnit *select_victim_ru_from_ruh(struct ssd *ssd,
                                                   uint16_t rgid,
                                                   uint16_t ruhid)
{
    FemuReclaimUnit *victim_ru = NULL;
    struct ru_mgmt *ru_mgmt = ssd->ruhs[ruhid].ru_mgmt;

    if (!ru_mgmt) {
        return NULL;
    }

    victim_ru = pqueue_pop(ru_mgmt->victim_ru_pq);
    if (victim_ru) {
        /*
         * pqueue_pop does not clear the popped element's stored index, so reset
         * ruh_pos to keep it a truthful "not in per-RUH queue" marker (same
         * discipline as the NOISY path). Without this, a later change_priority
         * or insert could act on a stale per-RUH index -- the issue #189 bug
         * class. Dormant today (no strategy fills the per-RUH queue on this
         * path) but hardened for when one does.
         */
        victim_ru->ruh_pos = 0;
        ru_mgmt->victim_ru_cnt--;
    }
    return victim_ru;
}

/*
 * select_victim_ru - pick best victim RU based on configured GC strategy
 */
/*
 * 설정된 GC 전략(rm->mgmt_type)으로 victim RU 하나를 골라 heap에서 뺀다.
 * 호출: do_gc_fdp_style()
 * 흐름: ① 전략별 선택
 *          GREEDY: vpc 최소 pop / CB: heap을 훑어 (1-u)*age/u 최대
 *          RAND: 무작위 / NOISY: RUH별 heap 맨 앞끼리 비교
 *       → ② force=false이고 ipc < npages/8 이면 되돌려 넣고 NULL
 *       → ③ pos/ruh_pos = 0, victim 자신의 RG victim_ru_cnt 감소
 * 참고: gc_strategy는 0/1/2/4만 허용(ftl-geom.c). 나머지 case는 설정으로
 *       도달하지 않는 fallback이다.
 * 반환: victim RU, 후보가 없거나 보류되면 NULL
 * 자세한 설명: code_study/06_FDP_GC_and_isolation.md §3~§4
 */
static FemuReclaimUnit *select_victim_ru(struct ssd *ssd, uint16_t rgid,
                                         uint16_t ruhid, bool force)
{
    struct ru_mgmt *rm = ssd->rg[rgid].ru_mgmt;
    FemuReclaimUnit *victim_ru = NULL;

    switch (rm->mgmt_type) {
    case GC_GLOBAL_GREEDY:
        victim_ru = pqueue_pop(rm->victim_ru_pq);
        break;

    case GC_GLOBAL_CB: {
        /*
         * Cost-benefit weighs the space a reclaim frees against the copying
         * it costs, by how long the data has sat: (1 - u) * age / u. The age
         * only means something at selection time, so scan the heap here as
         * the line GC does rather than trust a priority fixed at insertion.
         */
        pqueue_t *pq = rm->victim_ru_cb;
        uint64_t now = qemu_clock_get_us(QEMU_CLOCK_REALTIME);
        double best = -1.0;
        bool best_free = false;

        /* CB: vpc == 0(전부 invalid)인 RU가 있으면 그것, 없으면 점수 최대 */
        for (size_t i = 1; i < pq->size; i++) {
            FemuReclaimUnit *ru = pq->d[i];
            double u = ru->utilization;
            double age = (double)(now - ru->last_invalidated_time) + 1.0;
            double score = (1.0 - u) * age / (u + 1e-6);

            if (ru->vpc == 0) {
                if (!best_free) {
                    best_free = true;
                    victim_ru = ru;
                }
            } else if (!best_free && score > best) {
                best = score;
                victim_ru = ru;
            }
        }
        if (victim_ru) {
            pqueue_remove(pq, victim_ru);
        }
        break;
    }

    case GC_GLOBAL_RAND:
        victim_ru = pqueue_randpop(rm->victim_ru_pq, ftl_gc_rand(ssd));
        break;

    case GC_NOISY_RUH_CUSTOM: {
        /*
         * Cross-RUH selection: find lowest vpc across all RUHs
         * that exceed their custom GC threshold.
         */
        /*
         * NOISY: ru_in_use_cnt > custom_gc_threshold(항상 0)인 PI RUH들의
         * heap 맨 앞 중 vpc 최소를 고르고, 없으면 RG greedy로 fallback
         */
        FemuReclaimUnit *ru = NULL;
        int best_ruh = -1;
        int i;
        for (i = 0; i < (int)ssd->nruhs; i++) {
            if (!ssd->ruhs[i].ru_mgmt) {
                continue;
            }
            if (ssd->ruhs[i].ru_in_use_cnt <=
                ssd->ruhs[i].ru_mgmt->custom_gc_threshold) {
                continue;
            }
            ru = pqueue_peek(ssd->ruhs[i].ru_mgmt->victim_ru_pq);
            if (!ru) {
                continue;
            }
            if (!victim_ru || ru->vpc < victim_ru->vpc) {
                best_ruh = i;
                victim_ru = ru;
            }
        }
        if (best_ruh >= 0) {
            victim_ru = pqueue_pop(
                ssd->ruhs[best_ruh].ru_mgmt->victim_ru_pq);
            if (victim_ru) {
                /*
                 * pqueue_pop does NOT clear the element's stored index, so reset
                 * ruh_pos explicitly to keep it a truthful "in per-RUH queue"
                 * marker (the change_priority guard relies on it).
                 */
                victim_ru->ruh_pos = 0;
                ssd->ruhs[best_ruh].ru_mgmt->victim_ru_cnt--;
                /*
                 * Also remove from the global queue (uses the still-valid pos);
                 * the global victim_ru_cnt-- and pos=0 happen at the shared
                 * cleanup below, so do not touch them here. With nrg>1 the
                 * per-RUH queue can hold RUs from any RG, so the global twin
                 * lives in the victim's OWN RG queue, not the caller's rgid.
                 */
                if (victim_ru->pos) {
                    pqueue_remove(ssd->rg[victim_ru->rgidx].ru_mgmt->victim_ru_pq,
                                  victim_ru);
                }
            }
        } else {
            /* fallback to global greedy */
            victim_ru = pqueue_pop(rm->victim_ru_pq);
            /*
             * The global-popped RU may still have a per-RUH twin (NOISY mirrors
             * full RUs into both queues). Drop it from the per-RUH queue here,
             * using the still-valid ruh_pos, so it does not linger as a stale
             * entry (later change_priority / duplicate insert on put-back).
             */
            if (victim_ru && victim_ru->ruh_pos &&
                victim_ru->ruh && victim_ru->ruh->ru_mgmt) {
                pqueue_remove(victim_ru->ruh->ru_mgmt->victim_ru_pq, victim_ru);
                victim_ru->ruh_pos = 0;
                victim_ru->ruh->ru_mgmt->victim_ru_cnt--;
            }
        }
        break;
    }

    case GC_SELECTIVE_RUH:
    case GC_EXPLOIT_SEQUENTIAL:
        victim_ru = pqueue_pop(rm->victim_ru_pq);
        break;

    case GC_SELECTIVE_RUH_SOCIAL_WELFARE:
        victim_ru = select_victim_ru_from_ruh(ssd, rgid, ruhid);
        break;

    case GC_BIT_POPULATION:
    case GC_GLOBAL_WARM:
    case GC_SELECTIVE_RUH_ADV:
    case GC_SELECTIVE_MIDAS_OP:
    default:
        /* fallback to greedy */
        victim_ru = pqueue_pop(rm->victim_ru_pq);
        break;
    }

    if (!victim_ru) {
        /*
         * victim_ru_pq is empty: all in-use RUs are still fully written
         * with no invalidations yet (e.g., during sequential fill). There is
         * no victim to reclaim, so returning NULL is the correct behavior.
         */
        return NULL;
    }

    /* ② background(force=false)는 무효 page가 1/8 미만이면 미룬다 */
    if (!force && victim_ru->vpc > 0) {
        int threshold = victim_ru->npages / 8;
        if (victim_ru->ipc < threshold) {
            /*
             * Delay GC and put the victim back. Cross-RG NOISY selection can
             * return an RU that belongs to a different reclaim group than the
             * caller's rgid, so re-insert it into the victim's OWN RG queue --
             * matching the global removal above and the shared count decrement
             * below. Using the caller's rm here would charge the entry to the
             * wrong heap, and its stored pos would then index that wrong array
             * on a later remove/change_priority, corrupting the queue.
             */
            struct ru_mgmt *victim_rm = ssd->rg[victim_ru->rgidx].ru_mgmt;
            /* put it back */
            FDP_TRACE(ssd, "GC_BACK_RESERT triggered but delay GC (ru %d ipc %d threshold %d full %d)\n",victim_ru->ruidx, victim_ru->ipc, threshold, victim_ru->npages);
            if (victim_rm->mgmt_type == GC_GLOBAL_CB){
                pqueue_insert(victim_rm->victim_ru_cb, victim_ru);
            }else{
                pqueue_insert(victim_rm->victim_ru_pq, victim_ru);
                /*
                 * GC_NOISY_RUH_CUSTOM selection popped this RU from BOTH the
                 * per-RUH and global queues (and zeroed ruh_pos + decremented
                 * the per-RUH count). Restore the per-RUH membership and count
                 * too, or they fall out of sync. pqueue_insert re-sets ruh_pos.
                 */
                if (victim_rm->mgmt_type == GC_NOISY_RUH_CUSTOM &&
                    victim_ru->ruh && victim_ru->ruh->ru_mgmt) {
                    pqueue_insert(victim_ru->ruh->ru_mgmt->victim_ru_pq,
                                  victim_ru);
                    victim_ru->ruh->ru_mgmt->victim_ru_cnt++;
                }
            }
            return NULL;
        }
    }

    /* ③ 선택 확정: 두 heap 위치를 지워 "queue 밖" 상태로 만든다 */
    victim_ru->pos = 0;
    victim_ru->ruh_pos = 0;
    /*
     * Decrement the count on the victim's OWN reclaim group. For every path
     * except cross-RG NOISY selection this is the caller's rgid; NOISY can pull
     * a victim from another RG, whose global count must be the one adjusted.
     */
    ssd->rg[victim_ru->rgidx].ru_mgmt->victim_ru_cnt--;

    return victim_ru;
}

/*
 * gc_write_page_fdp_style - relocate a valid page to a GC destination RU.
 * Returns false when there is nowhere to put it, which leaves the page where it
 * is: the caller must then not erase the block it came from.
 */
/*
 * victim의 valid page 하나를 GC frontier RU로 옮긴다.
 * 호출: clean_one_block_fdp_style()
 * 흐름: ① fdp_gc_frontier()로 목적 RU 확보(없으면 false)
 *       → ② 새 PPA에 maptbl/rmap 연결 + valid 처리
 *       → ③ 옛 PPA는 mark_page_invalid_fdp()로 즉시 invalid
 *       → ④ fdp_advance_ru_pointer() 후 RUH 유형별로 gc_ru / curr_ru 갱신
 *       → ⑤ enable_gc_delay면 NAND write 지연 반영
 * 반환: 옮겼으면 true, 둘 곳이 없으면 false(이 block은 erase 금지)
 * 자세한 설명: code_study/06_FDP_GC_and_isolation.md §5, §7~§8
 */
static bool gc_write_page_fdp_style(struct ssd *ssd, struct ppa *old_ppa,
                                    FemuRuHandle *dest_ruh, uint16_t rgidx)
{
    struct ppa new_ppa;
    struct nand_lun *new_lun;
    uint64_t lpn = get_rmap_ent(ssd, old_ppa);
    FemuReclaimUnit *dest_ru=NULL;
    FemuReclaimUnit *ret_ru=NULL;
    ftl_assert(valid_lpn(ssd, lpn));
    ftl_assert(dest_ruh!=NULL);

    /* ① 목적 RU: PI면 gc_ru, II면 (마지막) RUH의 curr_ru */
    /* the frontier may have been dropped by this or an earlier pass */
    dest_ru = fdp_gc_frontier(ssd, dest_ruh, rgidx);
    if (!dest_ru) {
        return false;
    }

    /* ② 매핑을 새 PPA로 옮긴다 */
    new_ppa = fdp_get_new_page(ssd, dest_ru);
    set_maptbl_ent(ssd, lpn, &new_ppa);
    set_rmap_ent(ssd, lpn, &new_ppa);
    mark_page_valid_fdp(ssd, &new_ppa, dest_ru);
    /* ③ 예전엔 source를 여기서 invalid 처리하지 않았다(06 노트 §8) */
    /*
     * Retire the old copy now: a victim that cannot be emptied is requeued,
     * and a copy still marked valid would be moved again over newer data.
     */
    mark_page_invalid_fdp(ssd, old_ppa);
    set_rmap_ent(ssd, INVALID_LPN, old_ppa);
    ssd->gc_write_pages++; /* a page the device relocated itself */

    // FDP_TRACE(ssd, "GC_MIGRATE lpn=%lu src(ch=%u/lun=%u/blk=%u/pg=%u) "
    //           "dst(ch=%u/lun=%u/blk=%u/pg=%u) dest_ruhid=%u\n",
    //           lpn, (unsigned)old_ppa->g.ch, (unsigned)old_ppa->g.lun,
    //           (unsigned)old_ppa->g.blk, (unsigned)old_ppa->g.pg,
    //           (unsigned)new_ppa.g.ch, (unsigned)new_ppa.g.lun,
    //           (unsigned)new_ppa.g.blk, (unsigned)new_ppa.g.pg,
    //           dest_ruh->ruhid);

    /*
     * fdp_advance_ru_pointer() can be called from both the foreground write
     * path and GC, and only advances the write pointer of the given RU. The
     * caller is therefore responsible for updating curr_ru / gc_ru by RUH type
     * right after the advance.
     */

    /*
     * Advancing hands back NULL when there is no free reclaim unit left, which
     * is what a full device looks like from here. The initially-isolated arm
     * dereferenced that straight away, and the only thing standing between it
     * and a null pointer was an ftl_assert, which is compiled out. Neither arm
     * can do anything useful without a unit, so leave the write frontier where
     * it is and let the caller run out of room.
     */
    /* ④ PI: 새 RU는 gc_ru로. 실패하면 호스트 curr_ru는 되살린다 */
    if (dest_ruh->ruh_type == NVME_RUHT_PERSISTENTLY_ISOLATED) {
        FemuReclaimUnit *host_ru = dest_ru->ruh->curr_ru;

        ret_ru = fdp_advance_ru_pointer(ssd, &ssd->rg[dest_ru->rgidx],
                                        dest_ru->ruh, dest_ru);
        if (ret_ru && ret_ru != dest_ru) {
            dest_ruh->gc_ru = ret_ru;
        } else if (!ret_ru) {
            /*
             * Advancing clears the handle's host write frontier when it cannot
             * allocate, but what ran out here is the collection frontier. Put
             * the host's back: cleared, the next host write to this handle
             * would take a null unit into the page allocator. The collection
             * unit is retired and its pointer wrapped to page 0, so drop it:
             * kept, the next relocation would program a written page.
             */
            dest_ru->ruh->curr_ru = host_ru;
            dest_ruh->gc_ru = NULL;
        }
    } else if (dest_ruh->ruh_type == NVME_RUHT_INITIALLY_ISOLATED) {
        /* ④ II: frontier가 호스트 curr_ru이므로 FTL/NVMe view를 함께 갱신 */
        int gcruh_id = ssd->nruhs - 1;

        ftl_assert(dest_ruh->ruhid == gcruh_id);
        ret_ru = fdp_advance_ru_pointer(ssd, &ssd->rg[dest_ru->rgidx],
                                        dest_ruh, dest_ru);
        if (ret_ru && ret_ru != dest_ru) {
            ssd->ruhs[gcruh_id].rus[dest_ru->rgidx] = ret_ru;
            ssd->ruhs[gcruh_id].curr_ru = ret_ru;
            ssd->ruhs[gcruh_id].ruh->rus[dest_ru->rgidx] = ret_ru->nvme_ru;
        } else if (!ret_ru) {
            fdp_drop_active_ru(dest_ruh, dest_ru->rgidx);
        }
    }

    if (!ret_ru) {
        ftl_err("FDP: no free reclaim unit while relocating; device is full\n");
    }

    /* ⑤ enable_gc_delay=on일 때만 GC write를 NAND 시간에 반영 */
    if (ssd->sp.enable_gc_delay) {
        struct nand_cmd gcw;
        gcw.type = GC_IO;
        gcw.cmd = NAND_WRITE;
        gcw.stime = 0;
        ssd_advance_status(ssd, &new_ppa, &gcw);
    }

    new_lun = get_lun(ssd, &new_ppa);
    new_lun->gc_endtime = new_lun->next_lun_avail_time;

    /* this page is relocated; whether the next one can be is the next call */
    return true;
}

/*
 * clean_one_block_fdp_style - GC one block: read valid pages and write to new_ru
 * @params 
 *  ppa : identifies the block we want to clean
 *  dest_ruh : destination ruh.
 *          ru based mechanism can cause pointer error when gc write page allocates new ru to ruh->gc. 
 *          Replace ru based parameter which makes ruh->curr_ru or ruh->gc_ru dangling
 *          
 */
/*
 * victim block 하나의 valid page를 모두 옮긴다(invalid/free page는 건너뜀).
 * 호출: do_gc_fdp_style()
 * 반환: 다 옮기면 true, 중간에 목적 RU가 없으면 false.
 *       옮긴 page 수는 *moved에 누적
 */
static bool clean_one_block_fdp_style(struct ssd *ssd, struct ppa *ppa,
                                      FemuRuHandle *dest_ruh, uint16_t rgidx,
                                      int *moved)
{
    struct ssdparams *spp = &ssd->sp;
    struct nand_page *pg_iter;

    for (int pg = 0; pg < spp->pgs_per_blk; pg++) {
        ppa->g.pg = pg;
        pg_iter = get_pg(ssd, ppa);
        /* a unit a handle update closed early has pages never written */
        if (pg_iter->status == PG_VALID) {
            gc_read_page(ssd, ppa);
            /*
             * Nowhere to put this one. Stop here and say so: the pages already
             * moved keep their new home, and the ones still in this block have
             * to keep theirs, so the block must not be erased.
             */
            if (!gc_write_page_fdp_style(ssd, ppa, dest_ruh, rgidx)) {
                return false;
            }
            (*moved)++;
        }
    }

    ftl_assert(get_blk(ssd, ppa)->vpc == 0);
    return true;
}

/* charge relocated pages to MBMW, also when a pass stops part way */
/*
 * GC로 옮긴 bytes를 EG, FTL RUH, NVMe RUH의 mbmw에 더한다(victim RUH 기준).
 * 호출: do_gc_fdp_style()(완료 경로와 중간 실패 경로 모두)
 */
static void fdp_count_gc_writes(struct ssd *ssd, FemuReclaimUnit *victim_ru,
                                int pages)
{
    struct ssdparams *spp = &ssd->sp;
    uint64_t gc_bytes = (uint64_t)pages * spp->secsz * spp->secs_per_pg;

    nvme_fdp_stat_inc(&ssd->n->subsys->endgrp.fdp.mbmw, gc_bytes);
    nvme_fdp_stat_inc(&victim_ru->ruh->mbmw, gc_bytes);
    nvme_fdp_stat_inc(&victim_ru->ruh->ruh->mbmw, gc_bytes);
}

/*
 * mark_ru_free - reset a victim RU to free state after GC
 */
/*
 * 이미 erase된 victim RU를 초기 상태로 되돌려 RG free list에 넣는다.
 * 호출: do_gc_fdp_style(), ssd_trim_fdp_reset_all()
 * 주의: erase는 하지 않는다. 예전엔 여기서도 block을 free 처리해
 *       erase가 두 번 세어졌다(07 노트 §5). RUAMW는 초기값으로 복원.
 */
static void mark_ru_free(struct ssd *ssd, uint16_t rgid,
                         FemuReclaimUnit *ru)
{
    struct ru_mgmt *rm = ssd->rg[rgid].ru_mgmt;

    ftl_assert(ru != NULL);

    /*
     * The blocks are already erased. Every caller walks them itself first,
     * with the erase timing the die owes, and this ran the same walk again:
     * resetting page state a second time is harmless, but each block was
     * counted as erased twice, which is what wears the media in the model.
     */
    for (int i = 0; i < ru->n_lines; i++) {
        ru->lines[i]->ipc = 0;
        ru->lines[i]->vpc = 0;
        ru->lines[i]->pos = 0;
    }

    ru->vpc = 0;
    ru->ipc = 0;
    ru->pos = 0;
    ru->ruh_pos = 0;
    ru->next_line_index = 1;
    ru->utilization = 0.0f;
    ru->my_cb = 0.0f;
    ru->erase_cnt++;
    ru->chance_token = 0;

    fdp_set_ru_write_pointer(ssd, ru);

    /* restore ruamw to initial value */
    ftl_assert(ru->nvme_ru != NULL);
    ftl_assert(ru->ruh != NULL);
    ftl_assert(ru->ruh->ruh != NULL);
    ru->nvme_ru->ruamw = ru->ruh->ruh->ruamw;

    QTAILQ_INSERT_TAIL(&rm->free_ru_list, ru, entry);
    rm->free_ru_cnt++;
}

/*
 * reinsert_victim_ru - return a fully-popped victim RU to its own reclaim
 * group's victim queue. Used when GC cannot proceed (no free destination RU),
 * so the victim is not orphaned. Reverses the bookkeeping select_victim_ru()
 * performed when it returned this victim, always keying off the victim's own
 * reclaim group so a cross-RG NOISY victim goes back to the right heap.
 */
/*
 * GC를 끝내지 못한 victim을 자기 RG의 victim heap(NOISY면 per-RUH heap
 * 도)에 되돌린다.
 * 호출: do_gc_fdp_style()
 */
static void reinsert_victim_ru(struct ssd *ssd, FemuReclaimUnit *victim_ru)
{
    struct ru_mgmt *victim_rm = ssd->rg[victim_ru->rgidx].ru_mgmt;

    if (victim_rm->mgmt_type == GC_GLOBAL_CB) {
        pqueue_insert(victim_rm->victim_ru_cb, victim_ru);
    } else {
        pqueue_insert(victim_rm->victim_ru_pq, victim_ru);
        if (victim_rm->mgmt_type == GC_NOISY_RUH_CUSTOM &&
            victim_ru->ruh && victim_ru->ruh->ru_mgmt) {
            pqueue_insert(victim_ru->ruh->ru_mgmt->victim_ru_pq, victim_ru);
            victim_ru->ruh->ru_mgmt->victim_ru_cnt++;
        }
    }
    victim_rm->victim_ru_cnt++;
}

/*
 * do_gc_fdp_style - FDP garbage collection: select victim RU, migrate valid
 * pages to GC RU, then free the victim
 *  gaurantees one RU to be reclaimed, if victim is valid.
 */
/*
 * FDP GC 1회: victim RU 하나를 골라 valid page를 옮기고 erase 후 free로.
 * 호출: bb_ftl_process_req()(background, force=false),
 *       fdp_gc_until_clear()(foreground, force=true),
 *       ssd_fdp_update_ruhs()(free RU가 없을 때, force=true)
 * 흐름: ① select_victim_ru()
 *       → ② victim RUH 유형으로 목적 RUH 결정(PI: 자기 자신, II: 마지막 RUH)
 *       → ③ 옮길 page가 있으면 frontier 확보, 실패 시 victim 되돌림
 *       → ④ 모든 block의 valid page 이동(중간 실패 시 되돌리고 -1)
 *       → ⑤ 이동이 다 끝난 뒤에야 block erase
 *       → ⑥ mbmw/mbe 통계, IMPLICIT_RU_CHANGE event → ⑦ mark_ru_free()
 * 반환: 0 = RU 하나 회수, -1 = victim 없음 / 보류 / 공간 부족
 * 자세한 설명: code_study/06_FDP_GC_and_isolation.md
 */
int do_gc_fdp_style(struct ssd *ssd, uint16_t rgid, uint16_t ruhid,
                           bool force)
{
    struct ssdparams *spp = &ssd->sp;
    FemuReclaimUnit *victim_ru;
    //FemuReclaimUnit *new_ru;
    struct ppa ppa;
    int vpc_cnt = 0;
    int blk_cnt = 0;
    /* ① victim 선택(없거나 보류되면 -1) */
    victim_ru = select_victim_ru(ssd, rgid, ruhid, force);
    if (!victim_ru) {
        //FDP_TRACE(ssd,"GC_SKIP Unable to find victim RU, gc skip\n");
        return -1;
    }

    /* ② 목적지는 victim RUH의 격리 유형(PI/II)이 정한다 */
    /*
     * Where the victim's pages go depends on its handle's isolation: a
     * persistently isolated handle relocates into a unit of its own (gc_ru),
     * so collected data never mixes with another handle's; the initially
     * isolated one, always the last handle, relocates into its active unit.
     */
    FemuRuHandle *victim_ruh = victim_ru->ruh;
    FemuRuHandle *dest_ruh = NULL;

    if (victim_ruh->ruh_type == NVME_RUHT_PERSISTENTLY_ISOLATED) {
        dest_ruh = victim_ruh;
    } else if (victim_ruh->ruh_type == NVME_RUHT_INITIALLY_ISOLATED) {
        dest_ruh = &ssd->ruhs[ssd->nruhs - 1];
    } else {
        ftl_err("Undefined RUHT.");
        ftl_assert(false && __LINE__);
    }

    /* ③ 옮길 page가 있을 때만 frontier 필요(vpc 0이면 바로 회수) */
    /*
     * A victim with pages to move needs somewhere to put them; with no free
     * unit for the frontier, put it back and fail the pass, and the caller
     * degrades to a device-full write. One with none needs nowhere, and must
     * be collected even then: it is what gives the frontier a unit again.
     */
    if (victim_ru->vpc && !fdp_gc_frontier(ssd, dest_ruh, victim_ru->rgidx)) {
        reinsert_victim_ru(ssd, victim_ru);
        return -1;
    }
    ftl_assert(dest_ruh!=NULL);
    /* sanity: don't GC an active RU */
    if (victim_ru == victim_ru->ruh->curr_ru) {
        ftl_err("Victim RU %d is active, skipping GC\n", victim_ru->ruidx);
        //This is a bug.
        ftl_assert(false && __LINE__);
        return -1;
    }

    FDP_TRACE(ssd, "GC_START rgid=%u ruhid=%u victim_ru=%u "
              "victim_vpc=%d isolation=%s gc_type=%s\n",
              rgid, ruhid, victim_ru->ruidx, victim_ru->vpc,
              (victim_ruh->ruh_type == NVME_RUHT_PERSISTENTLY_ISOLATED) ?
              "PI" : "II", (force) ? "FORCE" : "BACK" );

    /* ④ 먼저 전부 옮긴다: 실패하면 victim을 되돌리고 erase는 안 한다 */
    /*
     * Migrate every valid page before erasing any block. If the destination
     * runs out part way, the victim goes back on the queue holding what was
     * not moved; a block erased before that point would be counted erased
     * again when the unit is next collected.
     */
    for (int i = 0; i < spp->lines_per_ru; i++) {
        ppa.g.blk = victim_ru->lines[i]->id;
        for (int ch = 0; ch < spp->nchs; ch++) {
            for (int lun = 0; lun < spp->luns_per_ch; lun++) {
                ppa.g.ch = ch;
                ppa.g.lun = lun;
                for (int pl = 0; pl < spp->pls_per_lun; pl++) {
                    ppa.g.pl = pl;
                    if (!clean_one_block_fdp_style(ssd, &ppa, dest_ruh,
                                                   victim_ru->rgidx,
                                                   &vpc_cnt)) {
                        fdp_count_gc_writes(ssd, victim_ru, vpc_cnt);
                        reinsert_victim_ru(ssd, victim_ru);
                        return -1;
                    }
                }
            }
        }
    }

    /* ⑤ LUN마다 같은 block 번호를 (multi-plane) erase */
    for (int i = 0; i < spp->lines_per_ru; i++) {
        struct line *victim_line = victim_ru->lines[i];
        ppa.g.blk = victim_line->id;
        for (int ch = 0; ch < spp->nchs; ch++) {
            for (int lun = 0; lun < spp->luns_per_ch; lun++) {
                ssd_erase_lun_block(ssd, ch, lun, ppa.g.blk,
                                    spp->enable_gc_delay, 0);
                blk_cnt += spp->pls_per_lun;
            }
        }
    }

    /* ⑥ 통계: GC write → mbmw, erase → mbe (모두 victim RUH에 귀속) */
    /* update FDP statistics: media bytes written (GC writes) */
    uint64_t gc_bytes = (uint64_t)vpc_cnt * spp->secsz * spp->secs_per_pg;
    uint64_t erase_bytes = (uint64_t)blk_cnt * spp->secsz * spp->secs_per_pg
                           * spp->pgs_per_blk;

    FDP_TRACE(ssd, "GC_DONE victim_ru=%u pages_migrated=%d "
              "blocks_erased=%d mbmw_delta=%lu mbe_delta=%lu\n",
              victim_ru->ruidx, vpc_cnt, blk_cnt, gc_bytes, erase_bytes);
    fdp_count_gc_writes(ssd, victim_ru, vpc_cnt);
    nvme_fdp_stat_inc(&ssd->n->subsys->endgrp.fdp.mbe, erase_bytes);
    nvme_fdp_stat_inc(&victim_ru->ruh->mbe, erase_bytes);
    nvme_fdp_stat_inc(&victim_ru->ruh->ruh->mbe, erase_bytes);

    /*
     * RUH의 RU 수만 줄인다. live page는 page별 invalid에서 이미 뺐다
     * (예전엔 여기서 live page를 vpc_cnt만큼 일괄 차감했다)
     */
    if (ssd->ruhs[victim_ru->ruh->ruhid].ru_in_use_cnt > 0) {
        ssd->ruhs[victim_ru->ruh->ruhid].ru_in_use_cnt--;
    }

    /* ⑥ event_filter가 켜져 있으면 ctrl_events에 IMPLICIT_RU_CHANGE 기록 */
    /* generate controller event for RU change due to GC */
    if (ssd->n->subsys) {
        NvmeEnduranceGroup *endgrp = &ssd->n->subsys->endgrp;
        NvmeRuHandle *nvme_ruh = victim_ru->ruh->ruh;
        if (nvme_ruh &&
            (nvme_ruh->event_filter >>
             nvme_fdp_evf_shifts[FDP_EVT_RUH_IMPLICIT_RU_CHANGE]) & 0x1) {
            NvmeFdpEvent e = {
                .type = FDP_EVT_RUH_IMPLICIT_RU_CHANGE,
                .flags = FDPEF_LV,
                .rgid = cpu_to_le16(victim_ru->rgidx),
                .ruhid = victim_ru->ruh->ruhid,
            };

            nvme_fdp_record_event(ssd->n, endgrp, false, &e);
        }
    }

    /* ⑦ victim을 자기 RG의 free list로 */
    /*
     * Free the victim into its OWN reclaim group. A cross-RG NOISY victim can
     * differ from the caller's rgid; freeing it into rgid would corrupt the
     * per-RG free list and later hand a foreign RU out from the wrong group.
     * For every non-NOISY path victim_ru->rgidx == rgid, so this is a no-op.
     */
    mark_ru_free(ssd, victim_ru->rgidx, victim_ru);
    return 0;
}

/*
 * foreground GC: 어느 RG든 free RU가 gc_thres_rus_high 이하인 동안
 * force GC를 반복한다(쓰기가 GC를 기다리는 backpressure).
 * 호출: ssd_stream_write_lpns()(명령 시작 시 + page마다)
 * 종료: 압력 해소, victim 없음(-1), 또는 반복 상한
 * 주의: 압력이 있는 RG와 상관없이 GC는 호출자의 rgid에서 한다
 *       (06 노트 §2.1에서 지적한 점이 그대로 남아 있다)
 */
static void fdp_gc_until_clear(struct ssd *ssd, uint16_t rgid, uint16_t ruhid)
{
    uint64_t fg_gc_iters = 0;
    uint64_t max_fg_gc = (uint64_t)ssd->nrg * ssd->rg[0].ru_mgmt->tt_rus +
                         ssd->nrg;

    while (should_gc_high_fdp_style(ssd) >= 0 && fg_gc_iters < max_fg_gc) {
        if (do_gc_fdp_style(ssd, rgid, ruhid, true) == -1) {
            break;
        }
        fg_gc_iters++;
    }
}

/*
 * ssd_stream_write - FDP write path: placement-aware page allocation
 */
/*
 * Program [start_lpn, end_lpn] into the reclaim unit the request places into.
 * Write and Write Zeroes both land here: the second hands over no data, but
 * the pages it leaves holding zeros are programmed and counted the same way.
 */
/*
 * FDP 쓰기 본체: PID로 RG/RUH를 고르고 LPN마다 그 RUH의 curr_ru에 쓴다.
 * 호출: ssd_stream_write()(Write, Copy), ssd_write_zeroes_fdp_style()
 * 흐름: ① DTYPE/DSPEC → nvme_parse_pid() → (ph, rgid)
 *          (실패하면 ph = 0, rgid = 0, INVALID_PID event)
 *       → ② ruhid = ns->fdp.phs[ph]
 *       → ③ curr_ru가 없으면 새 RU 시도(없으면 CAP_EXCEEDED)
 *       → ④ foreground GC(fdp_gc_until_clear)
 *       → ⑤ LPN마다: 옛 PPA invalid → 새 PPA 할당·매핑·valid
 *          → RUAMW 감소 → RU pointer 전진(필요 시 RU 교체) → NAND 지연
 * 반환: 가장 늦게 끝나는 page program 지연(ns). 쓴 bytes는 xfer_bytes
 * 자세한 설명: code_study/03_FDP_placement_resolution.md,
 *              code_study/04_FDP_write_and_RU_rotation.md
 */
static uint64_t ssd_stream_write_lpns(FemuCtrl *n, struct ssd *ssd,
                                      NvmeRequest *req, uint64_t start_lpn,
                                      uint64_t end_lpn)
{
    NvmeNamespace *ns = req->ns;
    struct ssdparams *spp = &ssd->sp;
    FemuReclaimGroup *rg;
    FemuRuHandle *ruh;
    FemuReclaimUnit *ru;

    uint64_t pg = (uint64_t)spp->secsz * spp->secs_per_pg;
    uint64_t media = 0;
    struct ppa ppa;
    uint64_t lpn;
    uint64_t curlat = 0, maxlat = 0;
    uint64_t written = 0;

    /* ① PID = (RG 비트 | PH). placement가 아니거나 틀리면 PH 0, RG 0 */
    /* parse placement info from request */
    uint16_t pid = req->fdp_dspec;
    uint8_t dtype = req->fdp_dtype;
    uint16_t ph, rgid, ruhid;

    if (dtype != NVME_DIRECTIVE_DATA_PLACEMENT ||
        !nvme_parse_pid(ns, pid, &ph, &rgid)) {
        /* generate INVALID_PID event if placement was attempted */
        if (dtype == NVME_DIRECTIVE_DATA_PLACEMENT && ssd->n->subsys) {
            NvmeEnduranceGroup *endgrp = &ssd->n->subsys->endgrp;
            NvmeRuHandle *def_ruh = &endgrp->fdp.ruhs[ns->fdp.phs[0]];
            if ((def_ruh->event_filter >>
                 nvme_fdp_evf_shifts[FDP_EVT_INVALID_PID]) & 0x1) {
                NvmeFdpEvent e = {
                    .type = FDP_EVT_INVALID_PID,
                    .flags = FDPEF_PIV | FDPEF_NSIDV,
                    .pid = cpu_to_le16(pid),
                    .nsid = cpu_to_le32(ns->id),
                };

                nvme_fdp_record_event(ssd->n, endgrp, true, &e);
            }
        }
        ph = 0;
        rgid = 0;
    }

    /* ② PH는 namespace 안의 번호, 실제 RUH 번호는 phs[]로 찾는다 */
    ruhid = ns->fdp.phs[ph];
    /* safety: ruhid must be within bounds (nvme_parse_pid ensures ph is valid) */
    if (unlikely(ruhid >= (uint16_t)ssd->nruhs)) {
        ftl_err("ssd_stream_write: ruhid %u >= nruhs %lu, clamping to 0\n",
                (unsigned)ruhid, (unsigned long)ssd->nruhs);
        ruhid = 0;
    }
    rg = &ssd->rg[rgid];
    ruh = &ssd->ruhs[ruhid];

    // FDP_TRACE(ssd, "WRITE lpn=%lu-%lu dtype=%u dspec=0x%x ph=%u "
    //           "ruhid=%u rgid=%u\n", start_lpn, end_lpn, dtype, pid,
    //           ph, ruhid, rgid);

    /*
     * Ensure this RUH has an active RU.  After a sequential fill,
     * curr_ru may be NULL (cleared by fdp_advance_ru_pointer when it
     * enqueued the last RU into full_ru_list and could not allocate a
     * fresh one).  Run foreground GC first so we have a free RU.
     */
    /*
     * ③ 활성 RU 없음(이전 쓰기에서 소진). 위 주석과 달리 GC 루프는
     * 주석 처리되어 있어 free RU 할당만 시도한다.
     */
    if (unlikely(!ruh->curr_ru)) {
        /* try to free space via GC before allocating */
        //Erase experimental gc behavior
        //int max_fg_gc = (int)(ssd->nrg > 0 ?
        //    ssd->rg[0].ru_mgmt->tt_rus : 64);
        //for (int gi = 0; gi < max_fg_gc && !ruh->curr_ru; gi++) {
        //    r = do_gc_fdp_style(ssd, rgid, ruhid, true);
        //    if (r == -1) break;
            /* GC may have freed a RU; try to grab it */
            FemuReclaimUnit *fresh = fdp_get_new_ru(ssd, rgid, ruhid);
            if (fresh) {
                ruh->rus[rgid] = fresh;
                ruh->ruh->rus[rgid] = fresh->nvme_ru;
                ruh->curr_ru = fresh;
            }else{
                ftl_err("NO reclaim Unit. Device is full error\n");
                //Fallout
            }
        //}
        if (!ruh->curr_ru) {
            /*
             * Genuinely out of reclaimable space: the foreground backpressure
             * GC below could not free a reclaim unit. Fail the command with a
             * capacity error instead of completing it as success with no data
             * written.
             */
            ftl_err("ssd_stream_write: device full, no RU for ruh %d\n", ruhid);
            req->status = NVME_CAP_EXCEEDED | NVME_DNR;
            return 0;
        }
    }
    /* 실제로 쓰는 RU는 curr_ru 하나(rus[rgid]와 같다고 가정; 01 노트 §5.1) */
    ru = ruh->rus[rgid];
    ru = ruh->curr_ru;
    ftl_assert(ruh->curr_ru == ruh->rus[rgid]);
    
    if (end_lpn >= spp->tt_pgs) {
        ftl_err("write past device geometry: end_lpn=%" PRIu64 " tt_pgs=%d\n",
                end_lpn, spp->tt_pgs);
        req->status = NVME_LBA_RANGE | NVME_DNR;
        return 0;
    }

    /* ④ foreground GC */
    /*
     * Foreground GC backpressure: reclaim until write pressure clears rather
     * than for a fixed number of passes. Each pass frees at most one reclaim
     * unit, so the old fixed cap (nrg) let the free-RU pool drain to zero under
     * sustained overwrites and the write then failed with a spurious device-full.
     * Running until should_gc_high_fdp_style() reports no pressure holds the
     * pool above the threshold -- the host write effectively waits on GC, which
     * is the intended backpressure. do_gc_fdp_style() returns -1 when no victim
     * remains (nothing left to reclaim); that is the normal exit. The counter is
     * only a guard against an unexpected non-terminating condition, bounded by
     * the total reclaim-unit population so it never trips during real progress.
     */
    fdp_gc_until_clear(ssd, rgid, ruhid);

    /* ⑤ LPN 단위 out-of-place write */
    for (lpn = start_lpn; lpn <= end_lpn; lpn++) {
        /*
         * And per page: one command can take more units than the watermark
         * keeps free, and once the handle's last unit is gone collection has
         * nowhere to move pages to.
         */
        fdp_gc_until_clear(ssd, rgid, ruhid);

        /*
         * Updating curr_ru should be handled by fdp_advance_ru_pointer() naturally.
         */
        ru = ruh->curr_ru;
        /*
         * A previous iteration may have exhausted the last free reclaim unit:
         * fdp_advance_ru_pointer() cleared curr_ru with nothing left to allocate.
         * Writing into a NULL RU would dereference it in fdp_get_new_page(); stop
         * and fail the command with a capacity error rather than crashing or
         * reporting a silent success for pages that were never written.
         */
        if (unlikely(!ru)) {
            req->status = NVME_CAP_EXCEEDED | NVME_DNR;
            break;
        }

        /* ⑤-a 덮어쓰기면 옛 page invalid(옛 RU/RUH 통계가 줄어든다) */
        ppa = get_maptbl_ent(ssd, lpn);
        if (mapped_ppa(&ppa)) {
            mark_page_invalid_fdp(ssd, &ppa);
            set_rmap_ent(ssd, INVALID_LPN, &ppa);
        }
        /* the page content changes: drop any stale read-cache entry for it */
        rcache_invalidate(ssd, lpn);
        /* ⑤-b curr_ru의 write pointer 위치에 기록 */
        /* new write */
        ppa = fdp_get_new_page(ssd, ru);
        set_maptbl_ent(ssd, lpn, &ppa);
        set_rmap_ent(ssd, lpn, &ppa);
        mark_page_valid_fdp(ssd, &ppa, ru);
        ssd->nand_write_pages++; /* a user page programmed into NAND */
        /*
         * Counted here rather than for the whole command before the loop: the
         * loop stops when the device runs out of room, so a command that wrote
         * part of itself used to add all of its pages to the denominator the
         * amplification figure divides by.
         */
        ssd->host_write_pages++;
        written++;

        /* ⑤-c RUAMW(호스트에 보이는 남은 쓰기량)는 LBA 개수로 줄인다 */
        /* RUAMW counts blocks, and a page may hold several or half of one */
        if (ru->nvme_ru) {
            uint64_t blks = ((media + pg) >> ns->lbaf.lbads) -
                            (media >> ns->lbaf.lbads);

            ru->nvme_ru->ruamw -= MIN(ru->nvme_ru->ruamw, blks);
        }
        media += pg;

        /* ⑤-d RU가 차서 교체되면 FTL/NVMe 양쪽 view를 새 RU로 */
        /* advance RU write pointer; may allocate new RU */
        FemuReclaimUnit *ret = fdp_advance_ru_pointer(ssd, rg, ruh, ru);
        if (ret && ret != ruh->curr_ru) {
            ruh->rus[rgid] = ret;
            ruh->curr_ru = ret;
            ruh->ruh->rus[rgid] = ret->nvme_ru;
            ru = ret;
        } else if (!ret) {
            /* no free unit; the next page or command fails for it */
            fdp_drop_active_ru(ruh, rgid);
            ru = NULL;
        }

        /* ⑤-e 방금 쓴 PPA의 NAND program 지연 */
        struct nand_cmd swr;
        swr.type = USER_IO;
        swr.cmd = NAND_WRITE;
        swr.stime = req->stime;
        curlat = ssd_advance_status(ssd, &ppa, &swr);
        maxlat = (curlat > maxlat) ? curlat : maxlat;
    }

    /* what the caller charges its byte counters with */
    req->xfer_bytes = written * (uint64_t)spp->secs_per_pg * spp->secsz;

    return maxlat;
}

/*
 * RU에 아직 한 page도 쓰지 않았는지(write pointer가 첫 위치인지).
 * 호출: ssd_fdp_update_ruhs()
 */
static bool fdp_ru_unwritten(FemuReclaimUnit *ru)
{
    struct write_pointer *wpp = ru->ssd_wptr;

    return wpp->curline == ru->lines[0] && !wpp->ch && !wpp->lun &&
           !wpp->pl && !wpp->pg;
}

/*
 * ssd_fdp_update_ruhs - Reclaim Unit Handle Update: each named handle moves to
 * a fresh unit and the one it leaves is collected like any other. A unit that
 * was never written is as good as a fresh one and is kept. If collection
 * cannot free a unit either, the command fails rather than claim a move.
 */
/*
 * I/O Management Send의 RUH Update: 지정 PID마다 RUH를 새 RU로 옮긴다.
 * 호출: bb_ftl_process_req()(NVME_CMD_IO_MGMT_SEND, req->fdp_pids 있음)
 * 흐름: ① PID → (ph, rgid) → RUH
 *       → ② 아직 빈 RU면 그대로 둔다(RU_NOT_FULLY_WRITTEN event만)
 *       → ③ 새 RU 할당(없으면 force GC 1회 후 재시도, 그래도 없으면
 *          CAP_EXCEEDED)
 *       → ④ event 기록, 옛 RU는 fdp_retire_ru()로 full/victim
 *       → ⑤ FTL/NVMe view를 새 RU로
 * 자세한 설명: code_study/07_FDP_statistics_events_and_trim.md §6.2
 *       (예전엔 RUAMW만 복원했고 물리 RU 교체가 없었다)
 */
void ssd_fdp_update_ruhs(FemuCtrl *n, NvmeRequest *req)
{
    struct ssd *ssd = n->ssd;
    NvmeNamespace *ns = req->ns;

    for (uint32_t i = 0; i < req->nr_fdp_pids; i++) {
        uint16_t pid = req->fdp_pids[i];
        uint16_t ph, rgid;
        FemuRuHandle *ruh;
        FemuReclaimUnit *old, *fresh;

        /* ① PID → (ph, rgid) → RUH */
        /* the poller refused the command if any of these was invalid */
        nvme_parse_pid(ns, pid, &ph, &rgid);
        ruh = &ssd->ruhs[ns->fdp.phs[ph]];
        old = ruh->curr_ru;
        if (!old) {
            continue;
        }
        /* ② 한 page도 안 쓴 RU는 새 RU와 다를 게 없으니 유지 */
        if (fdp_ru_unwritten(old)) {
            nvme_fdp_note_ru_left(n, ns, pid, old->nvme_ru);
            continue;
        }

        /* ③ 새 RU: 없으면 force GC 한 번으로 만들어 본다 */
        fresh = fdp_get_new_ru(ssd, rgid, ruh->ruhid);
        if (!fresh && do_gc_fdp_style(ssd, rgid, ruh->ruhid, true) != -1) {
            fresh = fdp_get_new_ru(ssd, rgid, ruh->ruhid);
        }
        if (!fresh) {
            req->status = NVME_CAP_EXCEEDED | NVME_DNR;
            return;
        }
        /* ④ 옛 RU 이탈 기록 + victim 후보로 → ⑤ 새 RU를 활성으로 */
        nvme_fdp_note_ru_left(n, ns, pid, old->nvme_ru);
        fdp_retire_ru(ssd, old);
        ruh->rus[rgid] = fresh;
        ruh->curr_ru = fresh;
        ruh->ruh->rus[rgid] = fresh->nvme_ru;
    }
}

/*
 * 요청의 slba/nlb를 LPN 범위로 바꿔 ssd_stream_write_lpns()에 넘긴다.
 * 호출: nvme_do_write_fdp()
 */
static uint64_t ssd_stream_write(FemuCtrl *n, struct ssd *ssd,
                                NvmeRequest *req)
{
    uint64_t start_lpn, end_lpn;

    ssd_lpn_range(ssd, req, req->slba, req->nlb, &start_lpn, &end_lpn);

    return ssd_stream_write_lpns(n, ssd, req, start_lpn, end_lpn);
}

/*
 * Charge what a placed write actually programmed to the endurance group and
 * to the handle its placement identifier selects.
 */
/*
 * 실제로 쓴 bytes(req->xfer_bytes)를 hbmw/mbmw에 더한다(EG + 해당 RUH).
 * 호출: nvme_do_write_fdp(), ssd_write_zeroes_fdp_style()
 * 자세한 설명: code_study/07_FDP_statistics_events_and_trim.md §2
 *       (예전엔 쓰기 전에 nlb * secsz를 미리 더했다)
 */
static void fdp_count_write(NvmeNamespace *ns, struct ssd *ssd,
                            NvmeRequest *req)
{
    uint64_t data_bytes = req->xfer_bytes;
    uint16_t ph, rg, ruhid;

    if (!data_bytes) {
        return;
    }
    if (req->fdp_dtype != NVME_DIRECTIVE_DATA_PLACEMENT ||
        !nvme_parse_pid(ns, req->fdp_dspec, &ph, &rg)) {
        ph = 0;
    }
    ruhid = ns->fdp.phs[ph];

    nvme_fdp_stat_inc(&ns->endgrp->fdp.hbmw, data_bytes);
    nvme_fdp_stat_inc(&ns->endgrp->fdp.mbmw, data_bytes);
    nvme_fdp_stat_inc(&ssd->ruhs[ruhid].hbmw, data_bytes);
    nvme_fdp_stat_inc(&ssd->ruhs[ruhid].ruh->hbmw, data_bytes);
    nvme_fdp_stat_inc(&ssd->ruhs[ruhid].mbmw, data_bytes);
    nvme_fdp_stat_inc(&ssd->ruhs[ruhid].ruh->mbmw, data_bytes);
}

/*
 * nvme_do_write_fdp - top-level FDP write: stats + stream write
 */
/*
 * FDP 쓰기 진입점: stream write 후 실제로 쓴 만큼만 통계에 반영한다.
 * 호출: bb_ftl_process_req()(NVME_CMD_WRITE), ssd_copy()(Copy의 쓰기)
 * 반환: 쓰기 지연(ns). slba/nlb 인자는 쓰지 않고 req 안의 값을 쓴다
 */
uint64_t nvme_do_write_fdp(FemuCtrl *n, NvmeRequest *req, uint64_t slba,
                           uint32_t nlb)
{
    NvmeNamespace *ns = req->ns;
    struct ssd *ssd = n->ssd;
    uint64_t lat;

    (void)slba;
    (void)nlb;

    lat = ssd_stream_write(n, ssd, req);

    /*
     * Charged from what the write actually programmed, after the fact: a
     * write the device had no room for must not add its length to either
     * total, or the reported amplification falls below one.
     */
    fdp_count_write(ns, ssd, req);

    return lat;
}

/* ========== FDP Init Functions ========== */

/*
 * femu_fdp_init_ru_mgmt - initialize RU management for a reclaim group
 */
/*
 * RG 하나의 ru_mgmt 기본값: free/full list와 victim heap 2개
 * (vpc 기준 victim_ru_pq, CB용 victim_ru_cb).
 * 호출: femu_fdp_ssd_init_reclaim_group()
 */
static void femu_fdp_init_ru_mgmt(struct ssd *ssd, FemuReclaimGroup *rg)
{
    struct ru_mgmt *rm = rg->ru_mgmt;

    rm->tt_rus = rg->tt_nru;
    rm->free_ru_cnt = rg->tt_nru;
    rm->victim_ru_cnt = 0;
    rm->full_ru_cnt = 0;
    rm->custom_gc_threshold = 0;

    /* 실제 전략은 femu_fdp_ssd_init_reclaim_group()에서 gc_strategy로 바꾼다 */
    /* default GC strategy */
    rm->mgmt_type = GC_GLOBAL_GREEDY;

    rm->is_gc_triggered = false;
    rm->is_force_gc_triggered = false;
    rm->waf_score_global = 0.0f;
    rm->waf_score_transitory = 0.0f;
    rm->utilization_overall = 0.0f;

    QTAILQ_INIT(&rm->free_ru_list);
    QTAILQ_INIT(&rm->full_ru_list);

    rm->victim_ru_pq = pqueue_init(rm->tt_rus, victim_ru_cmp_pri,
                                   victim_ru_get_pri, victim_ru_set_pri,
                                   victim_ru_get_pos, victim_ru_set_pos);

    rm->victim_ru_cb = pqueue_init(rm->tt_rus, victim_ru_cmp_pri_by_cb,
                                   victim_ru_get_pri_by_cb,
                                   victim_ru_set_pri_by_cb,
                                   victim_ru_get_pos, victim_ru_set_pos);
}

/*
 * femu_fdp_init_ssd_reclaim_unit - initialize one RU with lines and wptr
 */
/*
 * RU 하나를 만든다: line을 lines_per_ru개 받아 연결(line->my_ru = RU)
 * 하고, RU 전용 write pointer를 첫 line의 처음에 둔다.
 * 호출: femu_fdp_ssd_init_reclaim_group()
 * 자세한 설명: code_study/02_FDP_initialization.md §5
 */
static void femu_fdp_init_ssd_reclaim_unit(struct ssd *ssd,
                                           FemuReclaimUnit *femu_ru,
                                           int rgidx, int index)
{
    struct ssdparams *spp = &ssd->sp;
    struct write_pointer *wpp;

    femu_ru->n_lines = spp->lines_per_ru;
    femu_ru->next_line_index = 1;
    femu_ru->vpc = 0;
    femu_ru->ipc = 0;
    femu_ru->pos = 0;
    femu_ru->ruh_pos = 0;     /* not yet in any victim pqueue */
    femu_ru->ssd_wptr = g_malloc0(sizeof(struct write_pointer));
    femu_ru->npages = spp->lines_per_ru * spp->pgs_per_line;

    wpp = femu_ru->ssd_wptr;
    femu_ru->lines = g_malloc0(femu_ru->n_lines * sizeof(struct line *));
    for (int i = 0; i < femu_ru->n_lines; i++) {
        femu_ru->lines[i] = get_next_free_line(ssd);
        if (!femu_ru->lines[i]) {
            ftl_err("FDP: no free line for RU %d (rg %d, line %d/%d)\n",
                    index, rgidx, i, femu_ru->n_lines);
            abort();
        }
        femu_ru->lines[i]->my_ru = femu_ru;
    }
    wpp->curline = femu_ru->lines[0];
    wpp->ch = 0;
    wpp->lun = 0;
    wpp->pl = 0;
    wpp->blk = wpp->curline->id;
    wpp->pg = 0;
}

/*
 * femu_fdp_ssd_init_reclaim_group - init all RGs and their RU pools
 */
/*
 * 초기화 ②: RG 배열, RG별 RU pool과 ru_mgmt를 만들고 GC 임계값을 정한다.
 * 호출: ssd_init()(fdp_enabled일 때)
 * 흐름: ① RG마다 RU 배열 + ru_mgmt
 *       → ② RU마다 NvmeReclaimUnit과 짝짓고 line 배정 → free_ru_list
 *       → ③ gc_thres_rus(background), gc_thres_rus_high(foreground)
 *       → ④ gc_strategy 적용
 * 자세한 설명: code_study/02_FDP_initialization.md §4~§6
 */
void femu_fdp_ssd_init_reclaim_group(FemuCtrl *n, struct ssd *ssd)
{
    NvmeSubsystem *subsys = n->subsys;
    uint64_t rgs = subsys->params.fdp.nrg;
    FemuReclaimGroup *rg;
    uint64_t tt_nru = ssd->sp.total_ru_cnt;

    ftl_assert(tt_nru > 0);

    ssd->rg = g_malloc0(rgs * sizeof(FemuReclaimGroup));
    ssd->nrg = rgs;
    ssd->rus = g_malloc0(rgs * sizeof(FemuReclaimUnit *));

    for (int i = 0; i < (int)rgs; i++) {
        /* ① RG마다 전체 RU의 1/nrg씩 갖는다 */
        rg = &ssd->rg[i];
        rg->rgidx = i;
        rg->tt_nru = tt_nru / rgs;
        ssd->rus[i] = g_malloc0(tt_nru * sizeof(FemuReclaimUnit));
        rg->rus = ssd->rus[i];
        rg->ru_mgmt = g_malloc0(sizeof(struct ru_mgmt));
        femu_fdp_init_ru_mgmt(ssd, rg);
        fdp_log("Allocated %lu RUs to rg[%d]\n", tt_nru, i);
    }

    /* ② RU마다 NVMe 쪽 RU와 짝짓고 line 배정 → 처음엔 모두 free */
    /* link NvmeReclaimUnit pointers and init each SSD-level RU */
    NvmeReclaimUnit **russ = subsys->endgrp.fdp.rus;
    if (russ) {
        for (int i = 0; i < (int)rgs; i++) {
            rg = &ssd->rg[i];
            rg->ru_mgmt->free_ru_cnt = 0;
            for (int j = 0; j < rg->tt_nru; j++) {
                rg->rus[j].rgidx = i;
                rg->rus[j].nvme_ru = &russ[i][j];
                rg->rus[j].ruidx = j;
                femu_fdp_init_ssd_reclaim_unit(ssd, &rg->rus[j], i, j);
                QTAILQ_INSERT_TAIL(&rg->ru_mgmt->free_ru_list,
                                   &rg->rus[j], entry);
                rg->ru_mgmt->free_ru_cnt++;
            }
            /* ③ free RU <= gc_thres_rus면 background, <= _high면 foreground */
            rg->ru_mgmt->gc_thres_pcent =
                n->bb_params.gc_thres_pcent / 100.0;
            rg->ru_mgmt->gc_thres_pcent_high =
                n->bb_params.gc_thres_pcent_high / 100.0;
            rg->ru_mgmt->gc_thres_rus =
                (uint64_t)((1 - rg->ru_mgmt->gc_thres_pcent) *
                           rg->tt_nru);
            /* high는 bb_fdp_forced_units(): 0이 되면 최소 1개를 남긴다 */
            rg->ru_mgmt->gc_thres_rus_high =
                bb_fdp_forced_units(n, rg->tt_nru);
            ftl_log("rg[%d] gc threshold (%d%%) %lu/%d RU\n",
                    i, n->bb_params.gc_thres_pcent,
                    rg->ru_mgmt->gc_thres_rus, rg->tt_nru);
            ftl_log("rg[%d] gc threshold_high (%d%%) %lu/%d RU\n",
                    i, n->bb_params.gc_thres_pcent_high,
                    rg->ru_mgmt->gc_thres_rus_high, rg->tt_nru);

            /* ④ 전략 번호는 ftl-geom.c에서 0/1/2/4만 허용된다 */
            /* apply configured GC strategy */
            rg->ru_mgmt->mgmt_type = n->bb_params.gc_strategy;
            ftl_log("rg[%d] gc strategy=%d\n", i,
                    rg->ru_mgmt->mgmt_type);
        }
    }
}

/*
 * femu_fdp_ssd_init_ru_handles - init FemuRuHandle for each namespace PH
 */
/*
 * 초기화 ③: namespace의 PH마다 FTL 쪽 RUH(FemuRuHandle)를 만든다.
 * 호출: ssd_init()(fdp_enabled일 때)
 * 흐름: ① NVMe RUH와 연결, 유형(PI/II)·카운터 초기화
 *       → ② RG마다 첫 RU를 미리 배정(rus[rg]), curr_ru = rus[0]
 *       → ③ PI RUH만 per-RUH ru_mgmt(victim heap, ruh_pos 사용) 생성
 * 자세한 설명: code_study/02_FDP_initialization.md §7
 */
void femu_fdp_ssd_init_ru_handles(FemuCtrl *n, struct ssd *ssd)
{
    NvmeNamespace *ns = &n->namespaces[0];
    NvmeSubsystem *subsys = n->subsys;
    NvmeEnduranceGroup *endgrp = &subsys->endgrp;
    uint16_t nruh = subsys->params.fdp.nruh;
    uint16_t ph, *ruhid;

    ssd->ruhs = g_malloc0(nruh * sizeof(FemuRuHandle));
    ssd->nruhs = nruh;
    ruhid = ns->fdp.phs;

    for (ph = 0; ph < ns->fdp.nphs; ph++, ruhid++) {
        uint16_t i = *ruhid;
        NvmeRuHandle *nvme_ruh = &endgrp->fdp.ruhs[i];

        ssd->ruhs[i].ruh = nvme_ruh;
        ssd->ruhs[i].ruh_type = nvme_ruh->ruht;
        ssd->ruhs[i].ruhid = i;
        ssd->ruhs[i].ruh_live_pages_cnt = 0;
        ssd->ruhs[i].ru_in_use_cnt = 0;
        ssd->ruhs[i].curr_rg = 0;
        ssd->ruhs[i].hbmw = 0;
        ssd->ruhs[i].mbmw = 0;
        ssd->ruhs[i].mbe = 0;

        /* ② RUH × RG 개수만큼 free RU가 초기화 때 바로 쓰인다 */
        /* allocate per-RG RU pointer array */
        ssd->ruhs[i].rus = g_malloc0(sizeof(FemuReclaimUnit *) *
                                     endgrp->fdp.nrg);
        for (int j = 0; j < (int)endgrp->fdp.nrg; j++) {
            ssd->ruhs[i].rus[j] = fdp_get_new_ru(ssd, j, i);
            ssd->ruhs[i].rus[j]->ruh = &ssd->ruhs[i];
            ssd->ruhs[i].ruh->rus[j] = ssd->ruhs[i].rus[j]->nvme_ru;
        }
        /* curr_ru = RG 0의 RU(예전엔 마지막 RG의 RU; 01 노트 §5.1) */
        /*
         * The active RU must match the default reclaim group (rgid 0, curr_rg
         * 0), not the last one allocated by the loop above. A non-placement
         * write uses rgid 0, and the write path advances ruh->curr_ru through
         * ssd->rg[rgid]'s management object. Leaving curr_ru in the last group
         * (nrg-1) made a default write advance an rg[nrg-1] RU through rg[0]'s
         * bookkeeping: the RU was enqueued in one group's victim queue but later
         * looked up via its own group's queue with a stale heap position, a NULL
         * dereference crash under nrg>1. Cross-group placement writes still need
         * a per-(RUH,RG) active-RU model; this repairs the default path.
         */
        ssd->ruhs[i].curr_ru = ssd->ruhs[i].rus[0];

        /*
         * ③ PI RUH만 per-RUH heap을 만든다. gc_ru는 여기서 만들지 않고
         * 첫 PI GC 때 fdp_gc_frontier()가 할당한다
         */
        /* PI type RUHs get their own ru_mgmt for per-RUH victim queues */
        if (nvme_ruh->ruht == NVME_RUHT_PERSISTENTLY_ISOLATED) {
            ssd->ruhs[i].ru_mgmt = g_malloc0(sizeof(struct ru_mgmt));
            ssd->ruhs[i].ru_mgmt->mgmt_type = n->bb_params.gc_strategy;
            ssd->ruhs[i].ru_mgmt->victim_ru_cnt = 0;
            ssd->ruhs[i].ru_mgmt->full_ru_cnt = 0;
            ssd->ruhs[i].ru_mgmt->custom_gc_threshold = 0;
            QTAILQ_INIT(&ssd->ruhs[i].ru_mgmt->free_ru_list);
            QTAILQ_INIT(&ssd->ruhs[i].ru_mgmt->full_ru_list);
            /*
             * Per-RUH queues index via ruh_pos (see victim_ru_*_pos_ruh) so
             * they do not alias the per-RG queue's pos (issue #189).
             */
            ssd->ruhs[i].ru_mgmt->victim_ru_pq =
                pqueue_init(ssd->rg[0].tt_nru, victim_ru_cmp_pri,
                            victim_ru_get_pri, victim_ru_set_pri,
                            victim_ru_get_pos_ruh, victim_ru_set_pos_ruh);
            ssd->ruhs[i].ru_mgmt->victim_ru_cb =
                pqueue_init(ssd->rg[0].tt_nru, victim_ru_cmp_pri_by_cb,
                            victim_ru_get_pri_by_cb,
                            victim_ru_set_pri_by_cb,
                            victim_ru_get_pos_ruh, victim_ru_set_pos_ruh);
        }

        ftl_log("FDP: ruh[%d] type=%d, curr_ru=%d (line=%d)\n",
                i, ssd->ruhs[i].ruh_type, ssd->ruhs[i].curr_ru->ruidx,
                ssd->ruhs[i].curr_ru->lines[0]->id);
    }
}

/*
 * ssd_init_fdp_params - compute FDP-specific SSD parameters
 */
/*
 * 초기화 ①: RU geometry. RU 1개 = line 1개, RU 총수 = line 수
 * (endgrp의 nru보다 많으면 그 값으로 줄인다).
 * 호출: ssd_init()(fdp_enabled일 때)
 * 자세한 설명: code_study/02_FDP_initialization.md §3
 */
void ssd_init_fdp_params(struct ssdparams *spp, FemuCtrl *n)
{
    NvmeSubsystem *subsys = n->subsys;
    NvmeEnduranceGroup *endgrp = &subsys->endgrp;
    uint64_t runs = endgrp->fdp.runs;

    /* lines_per_ru: how many lines (superblocks) per reclaim unit */
    spp->lines_per_ru = 1; /* M1: 1 line per RU for simplicity */

    /*
     * Compute total RU count from device geometry:
     * total_ru = tt_lines / lines_per_ru
     * Clamp to endgrp->fdp.nru to avoid overflowing NvmeReclaimUnit array
     * allocated in nvme_subsys_setup_fdp().
     */
    spp->total_ru_cnt = spp->tt_lines / spp->lines_per_ru;

    if (endgrp->fdp.nru == 0) {
        endgrp->fdp.nru = spp->total_ru_cnt;
    } else if (spp->total_ru_cnt > (int)endgrp->fdp.nru) {
        ftl_log("FDP: clamping total_ru from %d to %lu (endgrp.nru)\n",
                spp->total_ru_cnt, (unsigned long)endgrp->fdp.nru);
        spp->total_ru_cnt = endgrp->fdp.nru;
    }

    ftl_log("FDP params: lines_per_ru=%d, total_ru=%d, runs=%lu\n",
            spp->lines_per_ru, spp->total_ru_cnt, (unsigned long)runs);
}

/*
 * ssd_reset_maptbl - clear entire mapping table (used by FDP trim)
 */
/*
 * 전체 maptbl/rmap을 unmapped로 지운다.
 * 호출: ssd_trim_fdp_reset_all()
 */
static void ssd_reset_maptbl(struct ssd *ssd)
{
    struct ssdparams *spp = &ssd->sp;

    for (int i = 0; i < spp->tt_pgs; i++) {
        ssd->maptbl[i].ppa = UNMAPPED_PPA;
        ssd->rmap[i] = INVALID_LPN;
    }
}

/*
 * Deallocate [start_lpn, end_lpn] under FDP: invalidate through the FDP path so
 * the reclaim unit's valid/invalid counts and victim state stay right, then
 * unmap. Shared by DSM Deallocate and Write Zeroes with the Deallocate bit.
 */
/*
 * LPN 범위를 unmap: mapped page는 mark_page_invalid_fdp()로 RU 통계와
 * victim 상태를 맞추고 maptbl/rmap/read cache를 지운다. erase는 GC 몫.
 * 호출: ssd_trim_fdp_range(), ssd_write_zeroes_fdp_style(),
 *       ssd_deallocate_fdp_all()
 * 반환: 실제로 unmap한 page 수(원래 없던 것은 *already_invalid에 누적)
 */
static int ssd_deallocate_fdp_lpns(struct ssd *ssd, uint64_t start_lpn,
                                   uint64_t end_lpn, int *already_invalid)
{
    int deallocated = 0;
    uint64_t lpn;

    for (lpn = start_lpn; lpn <= end_lpn; lpn++) {
        struct ppa ppa = get_maptbl_ent(ssd, lpn);

        if (!mapped_ppa(&ppa) || !valid_ppa(ssd, &ppa)) {
            (*already_invalid)++;
            continue;
        }
        mark_page_invalid_fdp(ssd, &ppa);
        set_rmap_ent(ssd, INVALID_LPN, &ppa);
        ppa.ppa = UNMAPPED_PPA;
        set_maptbl_ent(ssd, lpn, &ppa);
        rcache_invalidate(ssd, lpn);
        deallocated++;
    }

    return deallocated;
}

/* Deallocate every page under FDP; see bbssd_deallocate_all(). */
/*
 * 장치 전체 LPN을 FDP 방식으로 unmap한다.
 * 호출: bbssd_deallocate_all()(ftl-datapath.c; Sanitize, Format에서 사용)
 */
void ssd_deallocate_fdp_all(struct ssd *ssd)
{
    int already_invalid = 0;

    if (ssd->sp.tt_pgs) {
        ssd_deallocate_fdp_lpns(ssd, 0, ssd->sp.tt_pgs - 1, &already_invalid);
    }
}

/*
 * ssd_trim_fdp_range - FDP DSM deallocate (default). Invalidate only the logical
 * pages covered by the requested LBA ranges: mark each mapped page invalid via
 * the FDP path (which decrements RU/line vpc and moves the RU onto the victim
 * queue so GC reclaims it), clear the reverse map, and unmap the L2P entry.
 * Erase is left to GC. This matches a normal SSD's deallocate: a host TRIM of a
 * few LBAs must not disturb any other logical data.
 */
/*
 * DSM Deallocate 기본 경로: 요청의 range마다 LPN 범위를 unmap한다.
 * 호출: ssd_trim_fdp_style()(fdp_trim_erase_all = 0, 기본값)
 * 자세한 설명: code_study/07_FDP_statistics_events_and_trim.md §9
 *       (예전엔 이 경로가 없어 항상 장치 전체를 지웠다)
 */
static void ssd_trim_fdp_range(FemuCtrl *n, NvmeRequest *req)
{
    struct ssd *ssd = n->ssd;
    struct ssdparams *spp = &ssd->sp;
    NvmeDsmRange *ranges = req->dsm_ranges;
    int nr_ranges = req->dsm_nr_ranges;
    int total_trimmed_pages = 0;
    int total_already_invalid = 0;

    if (!ranges || nr_ranges <= 0) {
        return;
    }

    for (int range_idx = 0; range_idx < nr_ranges; range_idx++) {
        uint64_t start_lpn, end_lpn;

        ssd_lpn_range(ssd, req, le64_to_cpu(ranges[range_idx].slba),
                      le32_to_cpu(ranges[range_idx].nlb), &start_lpn, &end_lpn);

        if (end_lpn >= spp->tt_pgs) {
            ftl_err("FDP TRIM: range %d exceeds capacity (end_lpn=%lu "
                    "tt_pgs=%d)\n", range_idx, end_lpn, spp->tt_pgs);
            continue;
        }

        total_trimmed_pages += ssd_deallocate_fdp_lpns(ssd, start_lpn, end_lpn,
                                                      &total_already_invalid);
    }

    ftl_debug("FDP TRIM: %d pages trimmed, %d already invalid across %d ranges\n",
              total_trimmed_pages, total_already_invalid, nr_ranges);
}

/*
 * FDP Write Zeroes with the Deallocate bit: the blocks become deallocated, so
 * the FTL owes the same unmap DSM Deallocate does. Without the bit they hold
 * written zeros instead and the mapping is left alone.
 */
/*
 * FDP Write Zeroes: DEAC 비트가 없으면 0을 실제로 program(stream write),
 * 있으면 deallocate(unmap)만 한다.
 * 호출: bb_ftl_process_req()(NVME_CMD_WRITE_ZEROES, fdp_enabled)
 * 반환: program했으면 그 지연, deallocate나 범위 초과면 0
 */
uint64_t ssd_write_zeroes_fdp_style(FemuCtrl *n, NvmeRequest *req)
{
    struct ssd *ssd = (req->ns && req->ns->ssd) ? req->ns->ssd : n->ssd;
    struct ssdparams *spp = &ssd->sp;
    const NvmeRwCmd *rw = (const NvmeRwCmd *)&req->cmd;
    uint64_t start_lpn, end_lpn;
    int already_invalid = 0;

    ssd_lpn_range(ssd, req, le64_to_cpu(rw->slba), le16_to_cpu(rw->nlb) + 1,
                  &start_lpn, &end_lpn);
    if (end_lpn >= spp->tt_pgs) {
        return 0;
    }

    /*
     * Without the deallocate bit the blocks hold written zeros, so the device
     * has to put them somewhere: program the range into the handle's reclaim
     * unit, as the non-placement path does, rather than return having neither
     * charged the media nor counted the pages.
     */
    if (!(le16_to_cpu(rw->control) & NVME_WZ_DEAC)) {
        uint64_t lat = ssd_stream_write_lpns(n, ssd, req, start_lpn, end_lpn);

        fdp_count_write(req->ns, ssd, req);
        return lat;
    }

    ssd_deallocate_fdp_lpns(ssd, start_lpn, end_lpn, &already_invalid);

    return 0;
}

/*
 * ssd_trim_fdp_reset_all - FDP DSM whole-device reset (opt-in via the
 * fdp_trim_erase_all device property, test only). Erases every block, drains all
 * reclaim units, resets all RUH state, and wipes the mapping table. This was the
 * original prototype behavior for a customized non-filesystem fio+trim sweep; it
 * is NOT how DSM-deallocate behaves on a real SSD and ignores the requested LBA
 * range, so it is gated off by default.
 */
/*
 * 장치 전체 리셋(fdp_trim_erase_all = 1일 때만, 테스트용). LBA 범위 무시.
 * 호출: ssd_trim_fdp_style()
 * 흐름: ① 모든 block erase
 *       → ② RG마다 victim(pq, cb)·full RU를 free로
 *       → ③ RUH마다 per-RUH heap 비움, 통계 0, gc_ru/curr_ru 반납 후
 *          RG마다 새 RU 배정
 *       → ④ maptbl/rmap, EG 통계 초기화
 * 자세한 설명: code_study/07_FDP_statistics_events_and_trim.md §9~§10
 */
static void ssd_trim_fdp_reset_all(FemuCtrl *n, NvmeRequest *req, uint64_t slba,
                                   uint32_t nlb)
{
    struct ssd *ssd = n->ssd;
    struct ssdparams *spp = &ssd->sp;
    NvmeEnduranceGroup *endgrp = &n->subsys->endgrp;
    FemuReclaimUnit *v_ru;
    NvmeRuHandle *ruh;
    int rg_idx;

    /* ① 매핑과 무관하게 모든 block erase(enable_gc_delay면 시간 반영) */
    /* erase all blocks */
    for (int ch = 0; ch < spp->nchs; ch++) {
        for (int lun = 0; lun < spp->luns_per_ch; lun++) {
            for (int blk = 0; blk < spp->blks_per_pl; blk++) {
                ssd_erase_lun_block(ssd, ch, lun, blk, spp->enable_gc_delay,
                                    0);
            }
        }
    }

    /* ② victim(pq, cb)·full RU를 free로. cb drain은 예전 노트 이후 추가 */
    /* drain victim and full RU queues for all reclaim groups */
    for (rg_idx = 0; rg_idx < (int)ssd->nrg; rg_idx++) {
        struct ru_mgmt *rm = ssd->rg[rg_idx].ru_mgmt;
        while ((v_ru = pqueue_peek(rm->victim_ru_pq)) != NULL) {
            pqueue_remove(rm->victim_ru_pq, v_ru);
            rm->victim_ru_cnt--;
            mark_ru_free(ssd, v_ru->rgidx, v_ru);
        }
        /*
         * GC_GLOBAL_CB keeps its full victims in victim_ru_cb, not
         * victim_ru_pq, so drain it too or those RUs leak with a stale
         * victim_ru_cnt on trim/format. The two queues are mutually exclusive
         * per RG mode (the inactive one is empty here) and victim_ru_cb indexes
         * via the same pos field, so this is safe; guard the shared count
         * against underflow in case it was already inconsistent.
         */
        while ((v_ru = pqueue_peek(rm->victim_ru_cb)) != NULL) {
            pqueue_remove(rm->victim_ru_cb, v_ru);
            if (rm->victim_ru_cnt > 0) {
                rm->victim_ru_cnt--;
            }
            mark_ru_free(ssd, v_ru->rgidx, v_ru);
        }
        while ((v_ru = QTAILQ_FIRST(&rm->full_ru_list)) != NULL) {
            QTAILQ_REMOVE(&rm->full_ru_list, v_ru, entry);
            rm->full_ru_cnt--;
            mark_ru_free(ssd, v_ru->rgidx, v_ru);
        }
    }

    /* ③ curr_ru/gc_ru는 위 drain에 안 걸리므로 RUH마다 따로 반납 */
    /* reset active RUs and stats for each RUH across all RGs */
    ruh = endgrp->fdp.ruhs;
    for (int i = 0; i < (int)endgrp->fdp.nruh; i++, ruh++) {
        /*
         * Empty this RUH's victim queue too. The per-RG drain above freed the
         * RUs (and zeroed their ruh_pos via mark_ru_free), but the per-RUH heap
         * still physically references them; drop those stale entries so trim
         * leaves the per-RUH queue consistent (PI RUHs only). The RU objects
         * are already freed, so just clear the heap and its counter.
         */
        if (ssd->ruhs[i].ru_mgmt && ssd->ruhs[i].ru_mgmt->victim_ru_pq) {
            FemuReclaimUnit *pru;
            /*
             * pqueue_pop reassigns ruh_pos during percolate_down and never
             * clears the popped element's own index, so zero it explicitly or
             * a drained RU keeps a stale ruh_pos and later looks "in queue".
             */
            while ((pru = pqueue_pop(ssd->ruhs[i].ru_mgmt->victim_ru_pq))) {
                pru->ruh_pos = 0;
            }
            ssd->ruhs[i].ru_mgmt->victim_ru_cnt = 0;
        }
        ruh->hbmw = 0;
        ruh->mbmw = 0;
        ruh->mbe = 0;
        ssd->ruhs[i].hbmw = 0;
        ssd->ruhs[i].mbmw = 0;
        ssd->ruhs[i].mbe = 0;
        /*
         * The collection destination is a frontier like curr_ru, on no queue
         * the drain above walks. Left as it was, it points at a unit this
         * reset has handed back, which the next allocation gives to another
         * handle to write into at the same time.
         */
        if (ssd->ruhs[i].gc_ru && ssd->ruhs[i].gc_ru != ssd->ruhs[i].curr_ru) {
            mark_ru_free(ssd, ssd->ruhs[i].gc_ru->rgidx, ssd->ruhs[i].gc_ru);
        }
        ssd->ruhs[i].gc_ru = NULL;
        if (ssd->ruhs[i].curr_ru) {
            mark_ru_free(ssd, ssd->ruhs[i].curr_ru->rgidx,
                         ssd->ruhs[i].curr_ru);
        }
        ssd->ruhs[i].curr_ru = NULL;
        /* every unit was handed back above; count only the ones taken below */
        ssd->ruhs[i].ru_in_use_cnt = 0;
        ssd->ruhs[i].ruh_live_pages_cnt = 0;
        for (rg_idx = 0; rg_idx < (int)ssd->nrg; rg_idx++) {
            ssd->ruhs[i].rus[rg_idx] =
                fdp_get_new_ru(ssd, rg_idx, ssd->ruhs[i].ruhid);
            ssd->ruhs[i].ruh->rus[rg_idx] =
                ssd->ruhs[i].rus[rg_idx]->nvme_ru;
        }
        /* primary RG (index 0) is the active one */
        ssd->ruhs[i].curr_ru = ssd->ruhs[i].rus[0];
    }

    /* ④ 모든 LPN unmap + EG 통계 0 */
    ssd_reset_maptbl(ssd);

    endgrp->fdp.hbmw = 0;
    endgrp->fdp.mbmw = 0;
    endgrp->fdp.mbe = 0;

    ftl_log("FDP TRIM: all RUs reset\n");
}

/*
 * ssd_trim_fdp_style - dispatch FDP DSM deallocate. Range-honoring by default;
 * whole-device reset only when the fdp_trim_erase_all knob is set. Frees the
 * per-command DSM range list either way (nvme_dsm() allocates it per command and
 * leaves it for the FTL to release, as ssd_trim() does on the non-FDP path).
 */
/*
 * FDP DSM(trim) 진입점: fdp_trim_erase_all이면 전체 리셋, 아니면 range trim.
 * 끝나면 nvme_dsm()이 할당한 range 목록을 여기서 해제한다.
 * 호출: bb_ftl_process_req()(NVME_CMD_DSM, fdp_enabled)
 */
void ssd_trim_fdp_style(FemuCtrl *n, NvmeRequest *req, uint64_t slba,
                               uint32_t nlb)
{
    if (n->bb_params.fdp_trim_erase_all) {
        ssd_trim_fdp_reset_all(n, req, slba, nlb);
    } else {
        ssd_trim_fdp_range(n, req);
    }

    g_free(req->dsm_ranges);
    req->dsm_ranges = NULL;
    req->dsm_nr_ranges = 0;
    req->dsm_attributes = 0;
}

/*
 * femu_fdp_ssd_free - release what the two FDP init functions took
 *
 * ssd->rus[i] and rg->rus are the same allocation, and a handle's rus[] holds
 * pointers into those arrays rather than reclaim units of its own, so each is
 * freed exactly once from the side that allocated it.
 */
/*
 * FDP 초기화가 만든 RUH / RG / RU 메모리를 해제한다.
 * 호출: ssd_free()(ftl.c)
 */
void femu_fdp_ssd_free(struct ssd *ssd)
{
    uint64_t i;

    if (ssd->ruhs) {
        for (i = 0; i < ssd->nruhs; i++) {
            struct ru_mgmt *rm = ssd->ruhs[i].ru_mgmt;

            if (rm) {
                pqueue_free(rm->victim_ru_pq);
                pqueue_free(rm->victim_ru_cb);
                g_free(rm);
            }
            g_free(ssd->ruhs[i].rus);
        }
        g_free(ssd->ruhs);
        ssd->ruhs = NULL;
        ssd->nruhs = 0;
    }

    if (ssd->rg) {
        for (i = 0; i < ssd->nrg; i++) {
            FemuReclaimGroup *rg = &ssd->rg[i];
            struct ru_mgmt *rm = rg->ru_mgmt;
            uint64_t r;

            /*
             * The array holds total_ru_cnt entries even though a group hands
             * out only its share, and an untouched entry is zeroed, so walk
             * the allocation rather than the group's count.
             */
            for (r = 0; rg->rus && r < ssd->sp.total_ru_cnt; r++) {
                g_free(rg->rus[r].ssd_wptr);
                g_free(rg->rus[r].lines);
            }
            if (rm) {
                pqueue_free(rm->victim_ru_pq);
                pqueue_free(rm->victim_ru_cb);
                g_free(rm);
            }
        }
        g_free(ssd->rg);
        ssd->rg = NULL;
    }

    if (ssd->rus) {
        for (i = 0; i < ssd->nrg; i++) {
            g_free(ssd->rus[i]);
        }
        g_free(ssd->rus);
        ssd->rus = NULL;
    }
    ssd->nrg = 0;
}
