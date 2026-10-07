/*
 * DFTL / cached mapping table (CMT) cost model. A bounded cache of translation
 * pages over the flat maptbl; a CMT miss charges a translation-page NAND read
 * (and a write-back if a dirty TP is evicted). Timing-only: the flat maptbl in
 * ftl-map.c stays the source of truth. cmt_touch is the datapath entry point.
 *
 * Scope: the cost is charged for the USER read/write datapath (ssd_read /
 * ssd_write). Bulk mapping updates from base GC relocation, TRIM, and the FDP
 * write path update the flat maptbl directly and are not separately charged,
 * so the CMT models the host-facing translation cost rather than every internal
 * table mutation.
 */
/*
 * [한글 요약] DFTL의 CMT(Cached Mapping Table) 비용 모델 (mapping=dftl)
 * - DFTL은 L2P 전체를 NAND의 translation page(TP)에 두고 일부만 DRAM에
 *   캐시한다. 여기서는 "어느 TP가 캐시에 있나"만 추적해 miss 지연을 더한다.
 * - 왜 timing-only인가: 실제 L2P 값은 언제나 flat maptbl에서 읽으므로
 *   dftl은 page scheme과 데이터 동작이 똑같고 지연만 달라진다.
 * - TP 하나 = page 크기 / 8B(PPA 1개) 개의 LPN (lpn_per_tp)
 * - 교체: CLOCK(second chance), 조회: open-address hash(linear probing)
 * - 주요 진입 함수: cmt_touch() (datapath), cmt_init()/cmt_destroy() (ftl.c)
 * - 의존: ssd_advance_status() (ftl-media.c)로 MAP_IO NAND 지연 계산
 * - 자세한 설명: 참고자료/code_study/14_mapping_schemes.md
 */
#include "qemu/osdep.h"
#include "ftl.h"
#include "ftl-internal.h"

/* Open-address hash over the CMT slot array: find the slot holding tp_id, or -1. */
/* 호출: cmt_touch(). 상수 2654435761은 곱셈 hash(Knuth)용 상수 */
static int32_t cmt_lookup(struct ssd *ssd, uint64_t tp_id)
{
    uint32_t h = (tp_id * 2654435761u) % ssd->cmt.hash_sz;
    for (uint32_t probe = 0; probe < ssd->cmt.hash_sz; probe++) {
        int32_t slot = ssd->cmt.hash[h];
        if (slot < 0) {
            return -1; /* empty bucket -> not present */
        }
        if (ssd->cmt.slots[slot].valid &&
            ssd->cmt.slots[slot].tp_id == tp_id) {
            return slot;
        }
        h = (h + 1) % ssd->cmt.hash_sz;
    }
    return -1;
}

/* Rebuild the open-address hash from the current slot contents (after eviction). */
/*
 * 왜: linear probing 표에서 항목 하나만 지우면 probe 체인이 끊길 수 있어,
 * miss(교체)마다 hash 표 전체를 valid slot으로 다시 만든다. 호출: cmt_touch()
 */
static void cmt_rehash(struct ssd *ssd)
{
    for (uint32_t i = 0; i < ssd->cmt.hash_sz; i++) {
        ssd->cmt.hash[i] = -1;
    }
    for (uint32_t s = 0; s < ssd->cmt.capacity; s++) {
        if (!ssd->cmt.slots[s].valid) {
            continue;
        }
        uint32_t h = (ssd->cmt.slots[s].tp_id * 2654435761u) % ssd->cmt.hash_sz;
        while (ssd->cmt.hash[h] >= 0) {
            h = (h + 1) % ssd->cmt.hash_sz;
        }
        ssd->cmt.hash[h] = s;
    }
}

/* Pick a victim slot by CLOCK (second chance) and return its index. */
/*
 * hand를 돌며 빈 slot 또는 ref=false slot을 victim으로 고른다.
 * ref=true면 false로 내리고 한 바퀴 기회를 더 준다. 호출: cmt_touch()
 */
static uint32_t cmt_clock_evict(struct ssd *ssd)
{
    for (;;) {
        uint32_t s = ssd->cmt.hand;
        ssd->cmt.hand = (ssd->cmt.hand + 1) % ssd->cmt.capacity;
        if (!ssd->cmt.slots[s].valid) {
            return s;
        }
        if (!ssd->cmt.slots[s].ref) {
            return s;
        }
        ssd->cmt.slots[s].ref = false;
    }
}

/*
 * Charge the cached-mapping-table cost for one USER request's translation-page
 * access. Hit: O(1), set the reference bit, no added latency. Miss: charge a
 * translation-page read on the TP's home LUN (and a program if a dirty TP is
 * evicted), then install the TP. Timing-only: maptbl is unaffected. Returns the
 * added latency in ns.
 */
/*
 * LPN 하나의 L2P 접근에 드는 CMT 비용(ns)을 계산하고 캐시 상태를 갱신한다.
 * 호출: ssd_read() (is_write=false), ssd_write()·ssd_buffer_destage()
 *       (is_write=true). 모두 ssd->cmt.capacity != 0일 때만 부른다.
 * 흐름: ① tp_id = lpn / lpn_per_tp → hit면 ref(+write면 dirty) 설정, 0 반환
 *       ② miss: CLOCK으로 victim slot 선택
 *       ③ victim이 dirty TP면 그 TP의 home LUN에 program(write-back)
 *       ④ 새 TP를 자기 home LUN에서 read (write-back이 끝난 뒤 시작)
 *       ⑤ slot에 새 TP 설치(write miss면 dirty) 후 hash 재구성
 * 반환: 추가 지연(ns). hit은 0, miss는 [write-back +] TP read 지연
 */
uint64_t cmt_touch(struct ssd *ssd, uint64_t lpn, uint64_t stime, bool is_write)
{
    struct ssdparams *spp = &ssd->sp;
    uint64_t tp_id = lpn / ssd->cmt.lpn_per_tp;
    int32_t slot = cmt_lookup(ssd, tp_id);
    uint64_t lat = 0;

    /* ① hit: DRAM 안에서 끝나므로 추가 지연 없음 */
    if (slot >= 0) {
        ssd->cmt.slots[slot].ref = true;
        if (is_write) {
            ssd->cmt.slots[slot].dirty = true;
        }
        ssd->cmt.hits++;
        return 0;
    }

    /* miss: model the translation-page access */
    /* ② miss: CLOCK hand가 처음 만나는 빈 slot 또는 ref=false slot이 victim */
    ssd->cmt.misses++;
    uint32_t victim = cmt_clock_evict(ssd);

    /*
     * If a dirty translation page is evicted, write it back on ITS OWN home LUN
     * (not the incoming TP's), then serialize the new TP read after it so the two
     * accesses are charged once, in order, rather than both from stime.
     */
    /* ③ dirty victim만 write-back 비용이 든다. clean victim은 그냥 버린다 */
    uint64_t wb_lat = 0;
    if (ssd->cmt.slots[victim].valid) {
        if (ssd->cmt.slots[victim].dirty) {
            uint32_t vhome = ssd->cmt.slots[victim].tp_id % spp->tt_luns;
            struct ppa vppa = {.ppa = 0};
            vppa.g.ch = vhome / spp->luns_per_ch;
            vppa.g.lun = vhome % spp->luns_per_ch;
            struct nand_cmd twr = {.type = MAP_IO, .cmd = NAND_WRITE,
                                   .stime = stime};
            wb_lat = ssd_advance_status(ssd, &vppa, &twr);
        }
        ssd->cmt.used--;
    }

    /*
     * ④ TP의 home LUN = tp_id % tt_luns. blk/pg는 0인 자리표시 주소라
     * 시간(LUN 점유)만 계산되고 실제 page 상태·read 횟수는 바뀌지 않는다.
     */
    uint32_t home = tp_id % spp->tt_luns;
    struct ppa tppa = {.ppa = 0};
    tppa.g.ch = home / spp->luns_per_ch;
    tppa.g.lun = home % spp->luns_per_ch;
    struct nand_cmd trd = {.type = MAP_IO, .cmd = NAND_READ,
                           .stime = stime + wb_lat};
    lat = wb_lat + ssd_advance_status(ssd, &tppa, &trd);

    /* ⑤ 설치. write miss면 곧바로 dirty → 나중에 evict될 때 write-back */
    ssd->cmt.slots[victim].tp_id = tp_id;
    ssd->cmt.slots[victim].valid = true;
    ssd->cmt.slots[victim].ref = true;
    ssd->cmt.slots[victim].dirty = is_write;
    ssd->cmt.used++;
    cmt_rehash(ssd);
    return lat;
}

/*
 * Allocate the CMT from cache_mb (0 = disabled), one slot per translation page.
 * The sizing is held in 64 bits and bounded so capacity and hash_sz stay within
 * uint32_t. lpn_per_tp is the number of PPAs a translation page holds.
 */
/*
 * CMT 할당: capacity = cache_mb / page 크기 (= 캐시할 TP 수), hash_sz = 2배+1.
 * cache_mb=0이거나 LUN이 없으면 capacity=0 → CMT 꺼짐(datapath가 건너뜀).
 * 호출: ssd_init() (ftl.c). dftl이면 mapping_cache_mb(0이면 4MB), 아니면 0
 */
void cmt_init(struct ssd *ssd, uint32_t cache_mb)
{
    struct ssdparams *spp = &ssd->sp;

    ssd->cmt.capacity = 0;
    if (!cache_mb) {
        return;
    }
    /* the CMT models translation-page reads on a home LUN; without LUNs there is
     * nothing to model (and tp_id % tt_luns would divide by zero) */
    if (spp->tt_luns == 0) {
        return;
    }
    uint64_t pgsz = (uint64_t)spp->secsz * spp->secs_per_pg;
    if (pgsz == 0) {
        pgsz = 4096;
    }
    ssd->cmt.lpn_per_tp = pgsz / sizeof(uint64_t); /* PPAs per translation page */
    if (ssd->cmt.lpn_per_tp < 1) {
        ssd->cmt.lpn_per_tp = 1;
    }
    /* 예: 4KB page → TP당 512 LPN, 4MB CMT → 1024 TP → 2GB분 L2P 캐시 */
    uint64_t cap = ((uint64_t)cache_mb * 1024 * 1024) / pgsz;
    /* bound so capacity and hash_sz (= capacity * 2 + 1) never overflow uint32_t */
    if (cap > (UINT32_MAX - 1) / 2) {
        cap = (UINT32_MAX - 1) / 2;
    }
    if (cap < 1) {
        cap = 1;
    }
    ssd->cmt.capacity = (uint32_t)cap;
    ssd->cmt.hash_sz = ssd->cmt.capacity * 2 + 1;
    ssd->cmt.slots = g_malloc0(sizeof(*ssd->cmt.slots) * ssd->cmt.capacity);
    ssd->cmt.hash = g_malloc(sizeof(*ssd->cmt.hash) * ssd->cmt.hash_sz);
    for (uint32_t i = 0; i < ssd->cmt.hash_sz; i++) {
        ssd->cmt.hash[i] = -1;
    }
    ssd->cmt.used = ssd->cmt.hand = 0;
    ssd->cmt.hits = ssd->cmt.misses = 0;
}

/* release what cmt_init() took */
/* 호출: ssd_free() (ftl.c) */
void cmt_destroy(struct ssd *ssd)
{
    g_free(ssd->cmt.slots);
    g_free(ssd->cmt.hash);
    ssd->cmt.slots = NULL;
    ssd->cmt.hash = NULL;
    ssd->cmt.capacity = 0;
}
