/*
 * bbssd device DRAM read cache (opt-in; capacity 0 = disabled). A bounded set of
 * recently-read LPNs with a fixed slot array and an open-address hash index --
 * the same structure as a cached mapping table. Timing-only: it does not hold
 * data (NAND remains the source of truth), it only models the latency benefit of
 * a controller read cache. Runs on the single FTL thread. The eviction policy is
 * selectable: CLOCK (default), random, true LRU, or a scan-resistant 2Q variant.
 */
/*
 * [이 파일의 역할]
 *   선택적 DRAM read cache (read_cache_mb > 0일 때만 동작). data는 담지
 *   않고 "최근 읽은 LPN 집합"만 기억해서, hit이면 NAND read 대신 DRAM
 *   지연을 돌려주는 timing 모델이다.
 * [주요 진입 함수]
 *   rcache_init(), rcache_touch(), rcache_invalidate(), rcache_destroy()
 * [의존 관계]
 *   부르는 쪽: ssd_init()·ssd_free() (ftl.c), ssd_read()·ssd_program_lpn()·
 *             ssd_deallocate_lpns()·bbssd_power_loss() (ftl-datapath.c),
 *             ssd_stream_write_lpns()·ssd_deallocate_fdp_lpns() (ftl-fdp.c)
 *   부르는 것: 없음 (slot 배열 + open-address hash만 사용)
 * 자세한 설명: 참고자료/code_study/12_datapath_read_write_trim.md
 */
#include "qemu/osdep.h"
#include "ftl.h"
#include "ftl-internal.h"

/* Open-address hash over the rcache slot array: find the slot holding lpn, or -1. */
/* 호출: rcache_touch(). 빈 bucket(-1)을 만나면 없는 것으로 판정 */
static int32_t rcache_lookup(struct ssd *ssd, uint64_t lpn)
{
    uint32_t h = (lpn * 2654435761u) % ssd->rcache.hash_sz;
    for (uint32_t probe = 0; probe < ssd->rcache.hash_sz; probe++) {
        int32_t slot = ssd->rcache.hash[h];
        if (slot < 0) {
            return -1; /* empty bucket -> not present */
        }
        if (ssd->rcache.slots[slot].valid &&
            ssd->rcache.slots[slot].lpn == lpn) {
            return slot;
        }
        h = (h + 1) % ssd->rcache.hash_sz;
    }
    return -1;
}

/* Rebuild the open-address hash from current slot contents (after an eviction). */
/* 호출: rcache_touch() — eviction으로 slot의 LPN이 바뀐 뒤 hash 전체 재구성 */
static void rcache_rehash(struct ssd *ssd)
{
    for (uint32_t i = 0; i < ssd->rcache.hash_sz; i++) {
        ssd->rcache.hash[i] = -1;
    }
    for (uint32_t s = 0; s < ssd->rcache.capacity; s++) {
        if (!ssd->rcache.slots[s].valid) {
            continue;
        }
        uint32_t h = (ssd->rcache.slots[s].lpn * 2654435761u) % ssd->rcache.hash_sz;
        while (ssd->rcache.hash[h] >= 0) {
            h = (h + 1) % ssd->rcache.hash_sz;
        }
        ssd->rcache.hash[h] = s;
    }
}

/*
 * [eviction policy 4종] rcache_evict()가 evict_policy 값으로 고른다.
 *   0 CLOCK  : hand가 돌며 ref=false인 slot 선택 (지나간 slot은 ref 해제)
 *   1 random : 고정 seed LCG → 같은 workload면 같은 결과 (재현성)
 *   2 LRU    : last_used가 가장 작은 slot (capacity 전체 스캔)
 *   3 2Q     : ref=false(한 번만 접근) slot 중 LRU 우선, 없으면 전체 LRU
 *   cache_evict 속성: "random"→1, "lru"→2, "arc"→3, 그 외/미지정→0 (ftl.c)
 */
/* Pick a victim slot by CLOCK (second chance) and return its index. */
static uint32_t rcache_clock_evict(struct ssd *ssd)
{
    for (;;) {
        uint32_t s = ssd->rcache.hand;
        ssd->rcache.hand = (ssd->rcache.hand + 1) % ssd->rcache.capacity;
        if (!ssd->rcache.slots[s].valid) {
            return s;
        }
        if (!ssd->rcache.slots[s].ref) {
            return s;
        }
        ssd->rcache.slots[s].ref = false;
    }
}

/*
 * Random eviction: pick a victim by a deterministic LCG over an internal counter
 * so a run is reproducible. A baseline for comparing against CLOCK's recency
 * approximation.
 */
static uint32_t rcache_random_evict(struct ssd *ssd)
{
    ssd->rcache.rand_state = ssd->rcache.rand_state * 6364136223846793005ull
                             + 1442695040888963407ull;
    return (uint32_t)((ssd->rcache.rand_state >> 33) % ssd->rcache.capacity);
}

/*
 * True LRU: evict the slot with the smallest last_used tick (least recently
 * accessed). O(capacity) scan -- exact recency, the reference point CLOCK
 * approximates cheaply.
 */
static uint32_t rcache_lru_evict(struct ssd *ssd)
{
    uint32_t victim = 0;
    uint64_t oldest = (uint64_t)-1;

    for (uint32_t s = 0; s < ssd->rcache.capacity; s++) {
        if (!ssd->rcache.slots[s].valid) {
            return s;
        }
        if (ssd->rcache.slots[s].last_used < oldest) {
            oldest = ssd->rcache.slots[s].last_used;
            victim = s;
        }
    }
    return victim;
}

/*
 * Scan-resistant 2Q-style eviction. The `ref` bit acts as a "protected" flag: a
 * page is inserted unprotected (ref=false, seen once) and only promoted to
 * protected (ref=true) on a second access (a real hit). Eviction prefers the LRU
 * among unprotected slots -- so scan traffic (touched once, never re-referenced)
 * is reclaimed before re-referenced pages, defeating the looping-scan pathology
 * that sinks LRU/CLOCK. Falls back to LRU among protected slots if every slot is
 * protected.
 */
static uint32_t rcache_2q_evict(struct ssd *ssd)
{
    uint32_t victim = 0;
    uint64_t oldest_unprot = (uint64_t)-1, oldest_any = (uint64_t)-1;
    int unprot_victim = -1;

    for (uint32_t s = 0; s < ssd->rcache.capacity; s++) {
        if (!ssd->rcache.slots[s].valid) {
            return s;
        }
        if (ssd->rcache.slots[s].last_used < oldest_any) {
            oldest_any = ssd->rcache.slots[s].last_used;
            victim = s;
        }
        if (!ssd->rcache.slots[s].ref &&
            ssd->rcache.slots[s].last_used < oldest_unprot) {
            oldest_unprot = ssd->rcache.slots[s].last_used;
            unprot_victim = (int)s;
        }
    }
    return (unprot_victim >= 0) ? (uint32_t)unprot_victim : victim;
}

/* Eviction-policy dispatch: 0 = CLOCK (default), 1 = random, 2 = LRU, 3 = 2Q. */
/* 호출: rcache_touch() (cache가 가득 찼을 때 victim slot 선택) */
static uint32_t rcache_evict(struct ssd *ssd)
{
    if (ssd->rcache.evict_policy == 1) {
        return rcache_random_evict(ssd);
    }
    if (ssd->rcache.evict_policy == 2) {
        return rcache_lru_evict(ssd);
    }
    if (ssd->rcache.evict_policy == 3) {
        return rcache_2q_evict(ssd);
    }
    return rcache_clock_evict(ssd);
}

/*
 * Consult the read cache for a USER read of `lpn`. On a hit, set the reference
 * bit and return the DRAM hit latency (the caller substitutes it for the NAND
 * read). On a miss, insert the LPN (evicting via the selected policy if full) and
 * return 0 so the caller pays the NAND read. No-op (returns 0) when disabled.
 */
/*
 * read 1회에 대해 cache를 조회하고, miss면 그 LPN을 cache에 넣는다.
 * 호출: ssd_read() (매핑된 LPN의 NAND read 직전)
 * 흐름: ① hash 조회, hit면 ref/last_used 갱신 후 hit_lat 반환
 *       → ② miss이고 빈 slot이 있으면 뒤에 추가 + hash 삽입
 *       → ③ 가득 찼으면 policy로 victim을 골라 덮어쓰고 rcache_rehash()
 * 반환: hit_lat(ns) 또는 0(miss/비활성). 0이 miss 신호라 hit_lat ≥ 1
 */
uint64_t rcache_touch(struct ssd *ssd, uint64_t lpn)
{
    if (!ssd->rcache.capacity) {
        return 0;
    }

    /* ① hash 조회. hit면 ref=true (2Q에서는 이것이 protected 승격) */
    int32_t slot = rcache_lookup(ssd, lpn);
    if (slot >= 0) {
        ssd->rcache.slots[slot].ref = true;
        ssd->rcache.slots[slot].last_used = ++ssd->rcache.tick;
        ssd->rcache.hits++;
        return ssd->rcache.hit_lat;
    }

    ssd->rcache.misses++;
    uint32_t victim;
    /* ② 아직 안 쓴 slot(used < capacity)이 있으면 그 자리에 추가 */
    if (ssd->rcache.used < ssd->rcache.capacity) {
        victim = ssd->rcache.used++;
        ssd->rcache.slots[victim].valid = true;
        ssd->rcache.slots[victim].lpn = lpn;
        /*
         * 2Q (policy 3): new pages enter unprotected (probation), promoted on a
         * second hit. Other policies keep ref=true on insert (unchanged behavior).
         */
        ssd->rcache.slots[victim].ref = (ssd->rcache.evict_policy != 3);
        ssd->rcache.slots[victim].last_used = ++ssd->rcache.tick;
        uint32_t h = (lpn * 2654435761u) % ssd->rcache.hash_sz;
        while (ssd->rcache.hash[h] >= 0) {
            h = (h + 1) % ssd->rcache.hash_sz;
        }
        ssd->rcache.hash[h] = victim;
    } else {
        /* ③ 가득 참: policy가 고른 slot을 새 LPN으로 덮어쓴다 */
        victim = rcache_evict(ssd);
        /*
         * The victim may be a slot that a prior rcache_invalidate() marked dead
         * (valid=false, left in the used prefix); revive it so the insert stays
         * visible to rcache_rehash() and later lookups.
         */
        ssd->rcache.slots[victim].valid = true;
        ssd->rcache.slots[victim].lpn = lpn;
        ssd->rcache.slots[victim].ref = (ssd->rcache.evict_policy != 3);
        ssd->rcache.slots[victim].last_used = ++ssd->rcache.tick;
        /* rehash는 O(capacity): 에뮬레이터 CPU 비용일 뿐 모델 지연과 무관 */
        rcache_rehash(ssd); /* slot's key changed -> rebuild index */
    }
    return 0;
}

/*
 * Drop any cached entry for lpn so a subsequent read does not return a stale
 * DRAM-hit after the page is overwritten or trimmed. No-op when disabled or when
 * the lpn is not cached.
 */
/*
 * LPN의 cache entry를 지운다 (overwrite/trim/power loss 뒤 stale hit 방지).
 * 호출: ssd_program_lpn(), ssd_deallocate_lpns(), bbssd_power_loss()
 *       (ftl-datapath.c), ssd_stream_write_lpns(), ssd_deallocate_fdp_lpns()
 *       (ftl-fdp.c)
 * 흐름: ① hash에서 slot을 찾아 valid=false → ② backward-shift로 hash 정리
 */
void rcache_invalidate(struct ssd *ssd, uint64_t lpn)
{
    if (!ssd->rcache.capacity) {
        return;
    }

    /* ① slot은 dead로만 표시, used는 줄이지 않고 eviction 때 재사용 */
    /* find the hash bucket holding this lpn */
    uint32_t hs = ssd->rcache.hash_sz;
    uint32_t i = (lpn * 2654435761u) % hs;
    uint32_t probe;
    for (probe = 0; probe < hs; probe++) {
        int32_t s = ssd->rcache.hash[i];
        if (s < 0) {
            return; /* not present */
        }
        if (ssd->rcache.slots[s].valid && ssd->rcache.slots[s].lpn == lpn) {
            /*
             * Mark the slot dead but leave `used`: the slot index stays within
             * the allocated prefix and is reclaimed by normal eviction; a dead
             * slot is simply never a lookup hit (valid=false).
             */
            ssd->rcache.slots[s].valid = false;
            ssd->rcache.slots[s].ref = false;
            break;
        }
        i = (i + 1) % hs;
    }
    if (probe == hs) {
        return;
    }

    /* ② 지운 bucket 뒤의 연속 구간을 재삽입해 probe chain을 유지 */
    /*
     * Backward-shift deletion: removing one open-addressed bucket means clearing
     * it and re-inserting the following contiguous run so no lookup chain breaks.
     * O(cluster length), not O(capacity) -- this runs on the write hot path.
     */
    ssd->rcache.hash[i] = -1;
    uint32_t j = (i + 1) % hs;
    while (ssd->rcache.hash[j] >= 0) {
        int32_t moved = ssd->rcache.hash[j];
        ssd->rcache.hash[j] = -1;
        uint32_t k = (ssd->rcache.slots[moved].lpn * 2654435761u) % hs;
        while (ssd->rcache.hash[k] >= 0) {
            k = (k + 1) % hs;
        }
        ssd->rcache.hash[k] = moved;
        j = (j + 1) % hs;
    }
}

/*
 * Allocate the read cache from read_cache_mb (0 = disabled), one slot per NAND
 * page. The sizing is held in 64 bits and bounded so capacity and hash_sz stay
 * within uint32_t even for an absurdly large request. hit_lat defaults to ~1/16
 * of a page read (a DRAM-ish hit) when pg_rd_lat is known.
 */
/*
 * read cache를 할당한다 (read_cache_mb = 0이면 capacity = 0, 즉 꺼짐).
 * 호출: ssd_init() (ftl.c) — cache_evict 문자열을 policy 번호로 바꿔 넘김
 * capacity = read_cache_mb MiB / page 크기, hash_sz = capacity × 2 + 1
 * hit_lat = pg_rd_lat / 16 (기본 40000ns → 2500ns), init 때 한 번 고정
 */
void rcache_init(struct ssd *ssd, uint32_t read_cache_mb, uint32_t evict_policy)
{
    ssd->rcache.capacity = 0;
    if (read_cache_mb == 0) {
        return;
    }
    uint32_t page_sz = (uint32_t)ssd->sp.secsz * ssd->sp.secs_per_pg;
    if (page_sz == 0) {
        page_sz = 4096;
    }
    uint64_t cap = ((uint64_t)read_cache_mb * 1024 * 1024) / page_sz;
    /* bound so capacity and hash_sz (= capacity * 2 + 1) never overflow uint32_t */
    if (cap > (UINT32_MAX - 1) / 2) {
        cap = (UINT32_MAX - 1) / 2;
    }
    if (cap < 1) {
        cap = 1;
    }
    ssd->rcache.capacity = (uint32_t)cap;
    ssd->rcache.used = 0;
    ssd->rcache.hand = 0;
    ssd->rcache.evict_policy = evict_policy; /* 0 = CLOCK, 1 = random, 2 = LRU, 3 = 2Q */
    ssd->rcache.rand_state = 0x9e3779b97f4a7c15ull; /* fixed seed -> reproducible */
    ssd->rcache.tick = 0; /* LRU recency counter */
    ssd->rcache.hits = 0;
    ssd->rcache.misses = 0;
    /* a hit must return a nonzero latency: rcache_touch() signals a miss with 0 */
    ssd->rcache.hit_lat = ssd->sp.pg_rd_lat ? ssd->sp.pg_rd_lat / 16 : 1000;
    if (ssd->rcache.hit_lat < 1) {
        ssd->rcache.hit_lat = 1;
    }
    ssd->rcache.hash_sz = ssd->rcache.capacity * 2 + 1;
    ssd->rcache.slots = g_malloc0(sizeof(*ssd->rcache.slots) * ssd->rcache.capacity);
    ssd->rcache.hash = g_malloc(sizeof(*ssd->rcache.hash) * ssd->rcache.hash_sz);
    for (uint32_t i = 0; i < ssd->rcache.hash_sz; i++) {
        ssd->rcache.hash[i] = -1;
    }
}

/* release what rcache_init() took */
/* 호출: ssd_free() (ftl.c) */
void rcache_destroy(struct ssd *ssd)
{
    g_free(ssd->rcache.slots);
    g_free(ssd->rcache.hash);
    ssd->rcache.slots = NULL;
    ssd->rcache.hash = NULL;
    ssd->rcache.capacity = 0;
}
