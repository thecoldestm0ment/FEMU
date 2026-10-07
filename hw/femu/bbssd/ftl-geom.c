/*
 * bbssd FTL geometry setup: derive the ssdparams from the device parameters and
 * allocate the channel / LUN / plane / block / page array that backs the FTL.
 * Split out of the ftl.c monolith; the timing and datapath live elsewhere.
 */
/*
 * 역할: QEMU -device 속성(FemuCtrl의 bb_params)을 검증하고, 그 값으로
 *   ssdparams(기본 축, 파생 geometry, GC 수위)를 채운 뒤
 *   channel → LUN → plane → block → page 배열을 할당/해제한다.
 * 주요 진입: bb_check_geometry(), ssd_init_params(), ssd_init_ch(),
 *   ssd_free_ch(), bb_gc_forced_lines(), bb_fdp_forced_units()
 * 의존: bb_init()(bb.c), kvssd_init(), csd_init()이 검증을, ssd_init()과
 *   ssd_free()(ftl.c)가 params/배열 생성·해제를 부른다.
 *   속성 기본값은 femu.c의 DEFINE_PROP_* 표에 있다.
 * 자세한 설명: 참고자료/code_study/13_line_gc_geometry_media.md
 */
#include "qemu/osdep.h"
#include "ftl.h"
#include "ftl-internal.h"

/*
 * Free lines at or below which collection is forced. A second frontier (hot
 * data, a stream, or the log of a log-block mapping) takes lines by itself, so
 * it can take the last one while the data frontier, which collection writes
 * into, is nearly full. Keep a line back then, so collection always has one.
 */
/*
 * forced GC 수위(free line 수) = (1 − gc_thres_pcent_high/100) × blks_per_pl
 * 기본값(95%, blks_per_pl 256)이면 (int)(0.05 × 256) = 12 line.
 * 호출: ssd_init_params() (gc_thres_lines_high), bb_check_capacity() (bb.c)
 * 반환: free line이 이 값 이하이면 should_gc_high()가 참
 */
int bb_gc_forced_lines(FemuCtrl *n)
{
    const BbCtrlParams *p = &n->bb_params;
    int lines = (int)((1 - p->gc_thres_pcent_high / 100.0) * p->blks_per_pl);

    if (!lines && (p->hot_cold_sep || n->streams ||
                   femu_mapping_name_uses_log_class(p->mapping_scheme))) {
        lines = 1;
    }
    return lines;
}

/*
 * Free reclaim units, out of @units, at or below which placement collection is
 * forced. Collection moves a victim into a frontier that can fill part way
 * through, and with no free unit to follow it the victim stays where it is:
 * so it has to start while one is left, and a percentage that rounds to none
 * keeps one back. A threshold of 100 forces it only once no unit is free.
 */
/*
 * FDP용: reclaim unit @units개 기준 forced GC 수위. 식은 위와 같고,
 * 0으로 내림되면(high < 100) 최소 1개를 남긴다.
 * 호출: bb_check_capacity() (bb.c), femu_fdp_ssd_init_reclaim_group()
 *       (ftl-fdp.c)
 */
uint64_t bb_fdp_forced_units(FemuCtrl *n, uint64_t units)
{
    int high = n->bb_params.gc_thres_pcent_high;
    uint64_t kept = (uint64_t)((1 - high / 100.0) * units);

    if (!kept && high < 100) {
        kept = 1;
    }
    return kept;
}

/*
 * Validate the configured geometry before any of it is used to size an
 * allocation or drive the datapath.
 *
 * Two independent failure modes are reachable from the device parameters. An
 * axis of zero or less divides by zero on the datapath -- an LBA becomes a page
 * by dividing by secs_per_pg -- or allocates an empty array the FTL still
 * indexes. Separately, an axis that fits its own PPA field can still overflow
 * the derived totals: those are held in int, and the sector count is the
 * product of every axis, so per-axis values that are each legal can push the
 * aggregate past INT_MAX and wrap negative. Compute the aggregate in 64-bit
 * here and reject it, rather than letting ssd_init() allocate from a wrapped
 * count.
 */
/*
 * 호출: bb_init() (bb.c), kvssd_init() (kvssd.c), csd_init() (csd.c)
 * 흐름: ① 각 축 > 0 이고 PPA bit field 안 → ② GC 수위 범위
 *       → ③ timing/wear/FDP 옵션 범위 → ④ 이름 옵션(mapping 등)
 *       → ⑤ write buffer 설정 → ⑥ 전체 sector 수 ≤ INT_MAX
 * 반환: 0 = 통과, -1 = 실패(이유는 errp)
 */
int bb_check_geometry(FemuCtrl *n, Error **errp)
{
    const BbCtrlParams *p = &n->bb_params;
    const struct {
        const char *name;
        int value;
        int max;              /* 0 = no PPA field bound, only positivity */
    } axes[] = {
        { "secsz",       p->secsz,       0 },
        { "secs_per_pg", p->secs_per_pg, 1 << SEC_BITS },
        { "pgs_per_blk", p->pgs_per_blk, 1 << PG_BITS  },
        { "blks_per_pl", p->blks_per_pl, 1 << BLK_BITS },
        { "pls_per_lun", p->pls_per_lun, 1 << PL_BITS  },
        { "luns_per_ch", p->luns_per_ch, 1 << LUN_BITS },
        { "nchs",        p->nchs,        1 << CH_BITS  },
    };
    uint64_t tt_secs;
    int i;

    /* ① PPA bit 폭(ftl.h): ch 12, lun 7, pl 4, blk 16, pg 16, sec 8 */
    for (i = 0; i < (int)ARRAY_SIZE(axes); i++) {
        if (axes[i].value <= 0) {
            error_setg(errp, "FEMU bbssd: %s must be greater than 0, got %d",
                       axes[i].name, axes[i].value);
            return -1;
        }
        /*
         * An index runs 0..value-1, so a count equal to the field's capacity
         * still addresses within the field; only a larger count aliases.
         */
        if (axes[i].max && axes[i].value > axes[i].max) {
            error_setg(errp, "FEMU bbssd: %s value %d exceeds the %d addressable "
                       "by its PPA field", axes[i].name, axes[i].value,
                       axes[i].max);
            return -1;
        }
    }

    /* ② gc_thres_pcent(background) ≤ gc_thres_pcent_high(forced) */
    /*
     * The collection watermarks are percentages of the line count, and both
     * are turned into a line count by (1 - pcent/100) * tt_lines. Outside
     * [1, 100] that expression is negative, and converting a negative double
     * to the unsigned the reserve calculation uses is undefined; on this
     * compiler it produced a reserve of nearly 2^64 and a message about
     * needing 18446744073709551614 free lines. A high watermark below the low
     * one also means forced collection starts before background collection,
     * which is not what either is for.
     */
    if (p->gc_thres_pcent < 1 || p->gc_thres_pcent > 100 ||
        p->gc_thres_pcent_high < 1 || p->gc_thres_pcent_high > 100) {
        error_setg(errp, "FEMU bbssd: gc_thres_pcent and gc_thres_pcent_high "
                   "must be in [1, 100], got %d and %d", p->gc_thres_pcent,
                   p->gc_thres_pcent_high);
        return -1;
    }
    if (p->gc_thres_pcent_high < p->gc_thres_pcent) {
        error_setg(errp, "FEMU bbssd: gc_thres_pcent_high (%d) must not be "
                   "below gc_thres_pcent (%d); the forced watermark is reached "
                   "after the background one, not before",
                   p->gc_thres_pcent_high, p->gc_thres_pcent);
        return -1;
    }

    /* ③ NAND timing / FDP gc_strategy / wear(read reclaim 등) 옵션 */
    /* the page-type model has multiplier rows for one to five bits per cell */
    if (p->cell_pages < 0 || p->cell_pages > NAND_MEDIA_MAX_PGTYPE - 1) {
        error_setg(errp, "FEMU bbssd: cell_pages must be in [0, %d]",
                   NAND_MEDIA_MAX_PGTYPE - 1);
        return -1;
    }

    /* the per-cell-type page tables index by page number within the block */
    if (n->nand_cell_type && p->pgs_per_blk > MAX_SUPPORTED_PAGES_PER_BLOCK) {
        error_setg(errp, "FEMU bbssd: nand_cell_type supports at most %d "
                   "pgs_per_blk", MAX_SUPPORTED_PAGES_PER_BLOCK);
        return -1;
    }

    /* the other FDP strategy numbers fall back to greedy or never collect */
    if (p->gc_strategy != GC_GLOBAL_GREEDY && p->gc_strategy != GC_GLOBAL_CB &&
        p->gc_strategy != GC_GLOBAL_RAND &&
        p->gc_strategy != GC_NOISY_RUH_CUSTOM) {
        error_setg(errp, "FEMU bbssd: gc_strategy must be %d (greedy), %d "
                   "(cost-benefit), %d (random) or %d (per-handle)",
                   GC_GLOBAL_GREEDY, GC_GLOBAL_CB, GC_GLOBAL_RAND,
                   GC_NOISY_RUH_CUSTOM);
        return -1;
    }

    if (p->read_reclaim_limit < 0) {
        error_setg(errp, "FEMU bbssd: read_reclaim_limit must not be negative");
        return -1;
    }

    /* a negative overhead would start a preempting read before it arrived */
    if (p->tsusp_ns < 0) {
        error_setg(errp, "FEMU bbssd: tsusp_ns must not be negative");
        return -1;
    }

    if (p->retention_limit_sec < 0) {
        error_setg(errp, "FEMU bbssd: retention_limit_sec must not be negative");
        return -1;
    }
    /* ④ 이름으로 고르는 옵션: mapping, gc_policy, cache_evict */
    /*
     * A name the device does not have used to fall back to the default without
     * a word, so a misspelt scheme or policy ran a different one and a
     * measurement taken through it described something else entirely.
     */
    if (!femu_mapping_scheme_known(p->mapping_scheme)) {
        error_setg(errp, "FEMU bbssd: unknown mapping \"%s\"",
                   p->mapping_scheme);
        return -1;
    }
    if (!femu_ftl_policy_known(p->gc_policy)) {
        error_setg(errp, "FEMU bbssd: unknown gc_policy \"%s\"",
                   p->gc_policy);
        return -1;
    }
    if (p->cache_evict && p->cache_evict[0] &&
        strcmp(p->cache_evict, "clock") && strcmp(p->cache_evict, "random") &&
        strcmp(p->cache_evict, "lru") && strcmp(p->cache_evict, "arc")) {
        error_setg(errp, "FEMU bbssd: unknown cache_evict \"%s\"; use clock, "
                   "random, lru or arc", p->cache_evict);
        return -1;
    }

    /* ⑤ DRAM write buffer */
    if (p->buffer_size < 0) {
        error_setg(errp, "FEMU bbssd: buffer_size must not be negative");
        return -1;
    }
    /*
     * The write buffer starts writing pages back once it reaches
     * buffer_thres_pcent of its size, so a level above 100 would never be
     * reached and the buffer would grow without bound.
     */
    if (p->buffer_size > 0 &&
        (p->buffer_thres_pcent <= 0 || p->buffer_thres_pcent > 100)) {
        error_setg(errp, "FEMU bbssd: buffer_thres_pcent must be between 1 and "
                   "100, got %d", p->buffer_thres_pcent);
        return -1;
    }

    /* ⑥ ssdparams의 tt_secs 등은 int → 64-bit로 곱해 미리 넘침을 거부 */
    tt_secs = (uint64_t)p->secs_per_pg * p->pgs_per_blk * p->blks_per_pl *
              p->pls_per_lun * p->luns_per_ch * p->nchs;
    if (tt_secs > INT_MAX) {
        error_setg(errp, "FEMU bbssd: the geometry describes %" PRIu64 " sectors, "
                   "which exceeds the %d the FTL can address; reduce one of "
                   "secs_per_pg, pgs_per_blk, blks_per_pl, pls_per_lun, "
                   "luns_per_ch or nchs", tt_secs, INT_MAX);
        return -1;
    }

    return 0;
}

/* 현재는 검사 없음(assert가 주석 처리됨). 호출: ssd_init_params() */
static void check_params(struct ssdparams *spp)
{
    /*
     * we are using a general write pointer increment method now, no need to
     * force luns_per_ch and nchs to be power of 2
     */

    //ftl_assert(is_power_of_2(spp->luns_per_ch));
    //ftl_assert(is_power_of_2(spp->nchs));
}

/*
 * bb_params를 ssdparams로 옮기고 파생 geometry와 GC 수위를 계산한다.
 * 호출: ssd_init() (ftl.c)
 * 흐름: ① 기본 축(sector..channel) → ② NAND 지연과 선택 timing
 *       → ③ write buffer / wear 옵션 → ④ 파생값(secs_*, pgs_*, blks_*,
 *       pls_*, tt_luns) → ⑤ line geometry → ⑥ GC 수위(line 수)
 */
void ssd_init_params(struct ssdparams *spp, FemuCtrl *n)
{
    /* ① 기본값(femu.c): 512B × 8 = 4KiB page, 256 pg/blk, 256 blk/pl, */
    /*    1 pl/LUN, 8 LUN/ch, 8 ch → 전체 16GiB */
    spp->secsz = n->bb_params.secsz; // 512
    spp->secs_per_pg = n->bb_params.secs_per_pg; // 8
    spp->pgs_per_blk = n->bb_params.pgs_per_blk; //256
    spp->blks_per_pl = n->bb_params.blks_per_pl; /* 256 16GB */
    spp->pls_per_lun = n->bb_params.pls_per_lun; // 1
    spp->luns_per_ch = n->bb_params.luns_per_ch; // 8
    spp->nchs = n->bb_params.nchs; // 8

    /* ② 단위 ns. 기본 read 40us, program 200us, erase 2ms, ch_xfer 0 */
    spp->pg_rd_lat = n->bb_params.pg_rd_lat;
    spp->pg_wr_lat = n->bb_params.pg_wr_lat;
    spp->blk_er_lat = n->bb_params.blk_er_lat;
    spp->ch_xfer_lat = n->bb_params.ch_xfer_lat;

    /* optional richer NAND timing model (all 0 = off, flat per-page latency) */
    spp->cell_pages = n->bb_params.cell_pages;
    spp->pgtype_lat = n->bb_params.pgtype_lat;
    spp->ecc_step_ns = n->bb_params.ecc_step_ns;
    spp->ecc_retention_sec = n->bb_params.ecc_retention_sec;
    spp->cmd_addr_lat = n->bb_params.cmd_addr_lat;
    spp->pg_xfer_lat = n->bb_params.pg_xfer_lat;
    spp->status_lat = n->bb_params.status_lat;
    spp->tplpbsy = n->bb_params.tplpbsy;
    spp->tplrbsy = n->bb_params.tplrbsy;
    spp->tplebsy = n->bb_params.tplebsy;
    spp->trcbsy = n->bb_params.trcbsy;
    spp->pe_suspend = n->bb_params.pe_suspend;
    spp->tsusp_ns = n->bb_params.tsusp_ns;
    spp->trim_lat_ns = n->bb_params.trim_lat_ns;
    /* with one plane there is nothing to combine, so the flag stays clear */
    spp->mp_program = n->bb_params.mp_program && spp->pls_per_lun > 1;
    spp->mp_read = n->bb_params.mp_read && spp->pls_per_lun > 1;

    /* ③ buffer_thres_pcent는 여기서 0~1 비율(double)로 바뀐다 */
    /* DRAM write buffer */
    spp->buffer_size = n->bb_params.buffer_size;
    spp->buffer_thres_pcent = n->bb_params.buffer_thres_pcent / 100.0;
    spp->hot_cold_sep = n->bb_params.hot_cold_sep;
    spp->read_reclaim_limit = n->bb_params.read_reclaim_limit;
    spp->retention_limit_sec = n->bb_params.retention_limit_sec;
    spp->read_hit_cnt = 0;
    spp->read_cnt = 0;
    spp->write_hit_cnt = 0;
    spp->write_cnt = 0;

    /* ④ 하위 단위부터 곱해 올라간다. 기본값이면 secs_per_blk 2048, */
    /*    tt_secs 33,554,432, tt_pgs 4,194,304, tt_blks 16,384, tt_luns 64 */
    /* calculated values */
    spp->secs_per_blk = spp->secs_per_pg * spp->pgs_per_blk;
    spp->secs_per_pl = spp->secs_per_blk * spp->blks_per_pl;
    spp->secs_per_lun = spp->secs_per_pl * spp->pls_per_lun;
    spp->secs_per_ch = spp->secs_per_lun * spp->luns_per_ch;
    spp->tt_secs = spp->secs_per_ch * spp->nchs;

    spp->pgs_per_pl = spp->pgs_per_blk * spp->blks_per_pl;
    spp->pgs_per_lun = spp->pgs_per_pl * spp->pls_per_lun;
    spp->pgs_per_ch = spp->pgs_per_lun * spp->luns_per_ch;
    spp->tt_pgs = spp->pgs_per_ch * spp->nchs;

    spp->blks_per_lun = spp->blks_per_pl * spp->pls_per_lun;
    spp->blks_per_ch = spp->blks_per_lun * spp->luns_per_ch;
    spp->tt_blks = spp->blks_per_ch * spp->nchs;

    spp->pls_per_ch =  spp->pls_per_lun * spp->luns_per_ch;
    spp->tt_pls = spp->pls_per_ch * spp->nchs;

    spp->tt_luns = spp->luns_per_ch * spp->nchs;

    /* ⑤ line(superblock) = 모든 LUN × plane에서 같은 index의 block */
    /*    기본값: blks_per_line 64, pgs_per_line 16,384, tt_lines 256 */
    /*
     * A line is one block index taken across every channel, LUN and plane, so
     * there are as many lines as a plane has blocks. With one plane per LUN
     * these are the same numbers as before: blks_per_lun == blks_per_pl.
     */
    spp->blks_per_line = spp->tt_luns * spp->pls_per_lun;
    spp->pgs_per_line = spp->blks_per_line * spp->pgs_per_blk;
    spp->secs_per_line = spp->pgs_per_line * spp->secs_per_pg;
    spp->tt_lines = spp->blks_per_pl;

    /* ⑥ 비율 → free line 수. 기본: background ≤ 64 line(75%), */
    /*    forced ≤ 12 line(95%). should_gc()/should_gc_high()가 비교 */
    spp->gc_thres_pcent = n->bb_params.gc_thres_pcent/100.0;
    spp->gc_thres_lines = (int)((1 - spp->gc_thres_pcent) * spp->tt_lines);
    spp->gc_thres_pcent_high = n->bb_params.gc_thres_pcent_high/100.0;
    spp->gc_thres_lines_high = bb_gc_forced_lines(n);
    /* GC의 NAND 지연도 LUN에 부과(FEMU_DISABLE_GC_DELAY flip으로 끔) */
    spp->enable_gc_delay = true;


    check_params(spp);
}

/*
 * NAND 배열 생성 helper 묶음(ssd_init_ch() → lun → plane → block → page).
 * 각 단계에서 g_malloc0하고 page는 PG_FREE, block의 vpc/ipc/erase_cnt는
 * 0, LUN·channel의 next_*_avail_time(지연 timeline)은 0에서 시작한다.
 */
static void ssd_init_nand_page(struct nand_page *pg, struct ssdparams *spp)
{
    pg->status = PG_FREE;
}

static void ssd_init_nand_blk(struct nand_block *blk, struct ssdparams *spp)
{
    blk->npgs = spp->pgs_per_blk;
    blk->pg = g_malloc0(sizeof(struct nand_page) * blk->npgs);
    for (int i = 0; i < blk->npgs; i++) {
        ssd_init_nand_page(&blk->pg[i], spp);
    }
    blk->ipc = 0;
    blk->vpc = 0;
    blk->erase_cnt = 0;
    blk->wp = 0;
}

static void ssd_init_nand_plane(struct nand_plane *pl, struct ssdparams *spp)
{
    pl->nblks = spp->blks_per_pl;
    pl->blk = g_malloc0(sizeof(struct nand_block) * pl->nblks);
    for (int i = 0; i < pl->nblks; i++) {
        ssd_init_nand_blk(&pl->blk[i], spp);
    }
}

static void ssd_init_nand_lun(struct nand_lun *lun, struct ssdparams *spp)
{
    lun->npls = spp->pls_per_lun;
    lun->pl = g_malloc0(sizeof(struct nand_plane) * lun->npls);
    for (int i = 0; i < lun->npls; i++) {
        ssd_init_nand_plane(&lun->pl[i], spp);
    }
    lun->next_lun_avail_time = 0;
    lun->busy = false;
}

/*
 * channel 하나 아래의 LUN/plane/block/page 배열 전체를 만든다.
 * 호출: ssd_init() (ftl.c, channel 수만큼 반복)
 */
void ssd_init_ch(struct ssd_channel *ch, struct ssdparams *spp)
{
    ch->nluns = spp->luns_per_ch;
    ch->lun = g_malloc0(sizeof(struct nand_lun) * ch->nluns);
    for (int i = 0; i < ch->nluns; i++) {
        ssd_init_nand_lun(&ch->lun[i], spp);
    }
    ch->next_ch_avail_time = 0;
    ch->busy = 0;
}

/* release what ssd_init_ch() built, in the reverse order */
/* 호출: ssd_free() (ftl.c). 일부만 할당된 배열도 NULL 검사로 건너뛴다 */
void ssd_free_ch(struct ssd_channel *ch, struct ssdparams *spp)
{
    int lun, pl;

    if (!ch->lun) {
        return;
    }
    for (lun = 0; lun < spp->luns_per_ch; lun++) {
        struct nand_lun *l = &ch->lun[lun];

        if (!l->pl) {
            continue;
        }
        for (pl = 0; pl < spp->pls_per_lun; pl++) {
            struct nand_plane *p = &l->pl[pl];
            int blk;

            if (!p->blk) {
                continue;
            }
            for (blk = 0; blk < spp->blks_per_pl; blk++) {
                g_free(p->blk[blk].pg);
            }
            g_free(p->blk);
        }
        g_free(l->pl);
    }
    g_free(ch->lun);
    ch->lun = NULL;
}
