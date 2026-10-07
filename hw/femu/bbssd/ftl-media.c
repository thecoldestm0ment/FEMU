/*
 * bbssd FTL <-> uniform NAND media-layer bridge.
 *
 * Decodes the bbssd ppa into the media's normalized NandLoc, builds the media
 * config from ssdparams, and provides the ssd_advance_status wrapper the datapath
 * and GC call. The timing math lives in hw/femu/nand/nand-media.c; this file only
 * adapts bbssd's geometry and per-resource state to that API.
 */
/*
 * 역할: bbssd의 PPA를 NAND media 계층(hw/femu/nand/nand-media.c)의
 *   NandLoc으로 바꾸고, ssdparams로 NandMediaConfig를 만들어 NAND
 *   read/program/erase 지연을 그 계층에 계산시킨다.
 * 주요 진입: bb_nand_media_init(), bb_nand_media_refresh_timing(),
 *   ssd_advance_status(), ssd_advance_status_multiplane()
 * 의존: datapath/GC/mapping/FDP/kvssd가 ssd_advance_status*()를 부르고,
 *   이 파일은 nand_media_op()/nand_media_multiplane()을 부른다.
 *   "LUN이 언제 비는가"(next_lun_avail_time)는 bbssd struct에 있고,
 *   media 계층은 timeline callback으로 그 값을 읽고 갱신한다.
 * 자세한 설명: 참고자료/code_study/13_line_gc_geometry_media.md
 */
#include "qemu/osdep.h"
#include "ftl.h"
#include "ftl-internal.h"

/* Decode a bbssd ppa into the media's normalized NandLoc. */
/*
 * 호출: ssd_advance_status(), ssd_advance_status_multiplane()
 * ppa의 ch/lun/pl/blk/pg는 그대로 복사하고, 나머지는 지연 모델 입력이다.
 *   - flash_type/page_type: nand_cell_type을 쓰면 table timing의 index
 *   - pe_cycles(block erase_cnt), age_sec(line close 뒤 경과 초): ECC tier
 */
static NandLoc bb_decode_loc(struct ssd *ssd, struct ppa *ppa, uint64_t stime)
{
    int ct = (ssd->n) ? ssd->n->nand_cell_type : 0;
    NandLoc loc = {
        .ch = ppa->g.ch, .lun = ppa->g.lun, .pl = ppa->g.pl,
        .blk = ppa->g.blk, .pg = ppa->g.pg,
        /*
         * With a cell type set, the media layer uses table timing, so report the
         * real flash type and the paired page type. Otherwise flash_type is 0
         * (flat timing) and page_type is the pg-modulo-bits-per-cell value, which
         * only the optional pgtype_lat program-latency model consumes.
         */
        .flash_type = (uint8_t)ct,
        .page_type = ct ? get_page_type(ct, ppa->g.pg) :
                     (uint8_t)(ppa->g.pg % (ssd->sp.cell_pages ?
                                            ssd->sp.cell_pages : 3)),
        /*
         * pe_cycles feeds only the wear-dependent ECC-on-read latency tier, which
         * is active when ecc_step_ns is set. Read the block erase count lazily so
         * the common path does not chase block metadata on every op.
         */
        .pe_cycles = ssd->sp.ecc_step_ns ?
                     (uint32_t)get_blk(ssd, ppa)->erase_cnt : 0,
        .age_sec = 0,
    };

    /*
     * Charge leaks with time as well as with wear, so how long the data has sat
     * programmed feeds the same correction model. close_time is when the line
     * filled; a line still being written has none yet.
     */
    if (ssd->sp.ecc_step_ns && ssd->sp.ecc_retention_sec) {
        uint64_t closed = get_line(ssd, ppa)->close_time;

        if (closed && stime > closed) {
            loc.age_sec = (uint32_t)((stime - closed) / NANOSECONDS_PER_SECOND);
        }
    }
    return loc;
}

/* timeline-vtable thunks: return pointers into bbssd's own per-resource state */
/*
 * media 계층이 "이 channel/LUN이 언제 비는가"를 읽고 쓸 때 부르는
 * callback. 실제 값은 ssd->ch[].next_ch_avail_time과
 * ssd->ch[].lun[].next_lun_avail_time이다(ssd_init_ch()에서 0으로 시작).
 */
static uint64_t *bb_ch_avail(void *opaque, uint32_t ch)
{
    struct ssd *ssd = opaque;
    return &ssd->ch[ch].next_ch_avail_time;
}
static uint64_t *bb_lun_avail(void *opaque, const NandLoc *loc)
{
    struct ssd *ssd = opaque;
    return &ssd->ch[loc->ch].lun[loc->lun].next_lun_avail_time;
}

/*
 * plane_avail / page_reg_ready / lock_lun / unlock_lun are left unset: bbssd runs
 * the flat per-LUN gate under a single FTL thread, so the media never reaches for
 * plane-level state, a page register, or a lock.
 */
static const NandTimelineOps bb_timeline_ops = {
    .ch_avail = bb_ch_avail,
    .lun_avail = bb_lun_avail,
};

/*
 * Build the media config from bbssd ssdparams: flat per-LUN read/program/erase
 * timing gated on the LUN only, with no channel-bus accounting. This reproduces
 * the historical ssd_advance_status latency exactly.
 */
/*
 * 호출: ssd_init() (ftl.c)
 * 흐름: ① geometry와 flat 지연 → ② 선택 timing knob
 *       → ③ nand_cell_type이면 flash table timing으로 전환
 *       → ④ gate/channel 모델 선택 → ⑤ timeline 연결 후 nand_media_init()
 */
void bb_nand_media_init(struct ssd *ssd)
{
    struct ssdparams *spp = &ssd->sp;
    NandMediaConfig cfg = {0};

    /* ① 기본 flat 지연: read 40us / program 200us / erase 2ms (femu.c) */
    cfg.nchs = spp->nchs;
    cfg.luns_per_ch = spp->luns_per_ch;
    cfg.planes_per_lun = spp->pls_per_lun;
    cfg.timing.rd_ns = spp->pg_rd_lat;
    cfg.timing.wr_ns = spp->pg_wr_lat;
    cfg.timing.er_ns = spp->blk_er_lat;

    /* ② bus phase, multi-plane busy, suspend, ECC, page-type 배수 */
    /* optional finer NAND timing; every knob defaults to 0, leaving these zero
     * and the behavior bit-identical to the flat model. */
    cfg.timing.cmd_addr_ns = spp->cmd_addr_lat;
    /* ch_xfer_lat is the older name for the data transfer phase */
    cfg.timing.page_xfer_ns = spp->pg_xfer_lat ? spp->pg_xfer_lat :
                                                 spp->ch_xfer_lat;
    cfg.timing.status_ns = spp->status_lat;
    cfg.timing.tplpbsy_ns = spp->tplpbsy;
    cfg.timing.tplrbsy_ns = spp->tplrbsy;
    cfg.timing.tplebsy_ns = spp->tplebsy;
    cfg.timing.trcbsy_ns = spp->trcbsy;
    cfg.policy.pe_suspend = (spp->pe_suspend != 0);
    cfg.timing.tsusp_ns = spp->tsusp_ns;
    cfg.timing.ecc_step_ns = spp->ecc_step_ns;
    cfg.timing.ecc_pe_per_tier = FEMU_ECC_PE_PER_TIER;
    cfg.timing.ecc_max_tiers = FEMU_ECC_MAX_TIERS;
    cfg.timing.ecc_retention_per_tier_sec = spp->ecc_retention_sec;
    /*
     * The media layer gates the ECC read adder on this as well as on
     * ecc_step_ns. Nothing ever set it, so ecc_step_ns has been inert; the
     * adder itself is still off unless ecc_step_ns is given a value.
     */
    cfg.policy.ecc_on_read = true;
    cfg.timing.pgtype_lat = (spp->pgtype_lat != 0);
    /* page-type program multipliers (x1000): SLC..PLC rows, picked by cell_pages.
     * Only consulted when pgtype_lat is set, so this is inert by default. */
    {
        static const int32_t mult[5][NAND_MEDIA_MAX_PGTYPE] = {
            {1000,    0,    0,    0,    0, 0},
            { 600, 1400,    0,    0,    0, 0},
            { 600, 1000, 1400,    0,    0, 0},
            { 500,  850, 1150, 1500,    0, 0},
            { 400,  750, 1000, 1250, 1600, 0},
        };
        int cp = spp->cell_pages < 1 ? 1 : (spp->cell_pages > 5 ? 5 : spp->cell_pages);
        for (int i = 0; i < NAND_MEDIA_MAX_PGTYPE; i++) {
            cfg.timing.pgtype_mult[i] = mult[cp - 1][i];
        }
    }

    /* ③ flat = op 종류별 고정 지연. cell type이 있으면 아래에서 table로 */
    cfg.policy.use_flat_timing = true;
    /*
     * Cell type: when nand_cell_type is set (1 SLC .. 4 QLC), drive true per-type
     * NAND timing from the shared flash-type table (per-type read/program/erase
     * plus page pairing) instead of the flat latencies. Default (0) keeps the flat
     * path bit-identical.
     */
    if (ssd->n && ssd->n->nand_cell_type) {
        int ft = ssd->n->nand_cell_type;
        init_nand_flash(ssd->n); /* ensure the shared flash tables are populated */
        for (int p = 0; p < NAND_MEDIA_MAX_PGTYPE; p++) {
            cfg.timing.rd_table_ns[ft][p] = get_page_read_latency(ft, p);
            cfg.timing.wr_table_ns[ft][p] = get_page_write_latency(ft, p);
        }
        cfg.timing.er_table_ns[ft] = get_blk_erase_latency(ft);
        cfg.policy.use_flat_timing = false;
        cfg.timing.pgtype_lat = false; /* the table path already carries pairing */
    }

    /* ④ LUN 단위 gate: 한 LUN은 한 번에 array 동작 하나(plane gate 없음) */
    cfg.policy.array_gate = NAND_GATE_LUN_ONLY;
    /*
     * The channel bus phases only exist in the staged model. Select it when
     * a phase has a duration, so the unset default keeps the plain array gate
     * and its exact timing.
     */
    cfg.policy.channel_mode = (cfg.timing.cmd_addr_ns || cfg.timing.page_xfer_ns ||
                               cfg.timing.status_ns) ? NAND_CH_STAGED : NAND_CH_OFF;
    /* ⑤ media가 bbssd의 ch/LUN avail time을 직접 읽고 갱신하도록 연결 */
    cfg.timeline = &bb_timeline_ops;
    cfg.timeline_opaque = ssd;
    nand_media_init(&ssd->media, &cfg);
}

/*
 * Refresh only the flat per-op latencies after a runtime change to the device
 * parameters (the delay-emulation toggle rewrites pg_rd_lat/pg_wr_lat/blk_er_lat).
 * Three aligned scalar stores, matching the concurrency profile of the previous
 * inline code, which read those same ssd->sp scalars live per op. This avoids
 * rewriting the whole media config under the FTL thread, which would be a wide
 * non-atomic copy racing the datapath's per-op config reads.
 */
/* 호출: bb_flip_apply() (bb.c), femu_cxl_nvme_flip() (cxlssd.c) */
void bb_nand_media_refresh_timing(struct ssd *ssd)
{
    ssd->media.cfg.timing.rd_ns = ssd->sp.pg_rd_lat;
    ssd->media.cfg.timing.wr_ns = ssd->sp.pg_wr_lat;
    ssd->media.cfg.timing.er_ns = ssd->sp.blk_er_lat;
}

/*
 * NAND op timing lives in the uniform media layer (hw/femu/nand/nand-media.c).
 * This wrapper maps a bbssd read/program/erase onto nand_media_op; the timing math
 * and per-LUN accumulator update are identical to the historical
 * ssd_advance_status (same max() ordering). The signature is unchanged so every
 * caller (read, write, GC) is untouched.
 */
/*
 * NAND 명령 하나의 완료 시각을 계산하고 LUN timeline을 전진시킨다.
 * 호출: ssd_program_lpn()·ssd_read() (ftl-datapath.c), gc_read_page(),
 *       gc_write_page(), gc_write_page_fdp_style()·ssd_stream_write_lpns()
 *       (ftl-fdp.c), hybrid_charge(), fast_charge(), cmt_touch(),
 *       kv_program_ppas() 등 (kvssd-ftl.c)
 * 흐름: ① stime 결정 → ② op 변환 → ③ read면 block read_cnt++
 *       → ④ ppa → NandLoc 후 nand_media_op()
 * 반환: stime부터 완료까지 지연(ns). 기본(flat, bus phase 없음)에서는
 *       done = max(stime, next_lun_avail_time) + op 지연,
 *       next_lun_avail_time = done, 반환 = done − stime
 */
uint64_t ssd_advance_status(struct ssd *ssd, struct ppa *ppa,
                            struct nand_cmd *ncmd)
{
    /* ① stime = 0이면 "지금"(QEMU_CLOCK_REALTIME). GC 경로가 0을 넘긴다 */
    uint64_t stime = (ncmd->stime == 0) ?
        qemu_clock_get_ns(QEMU_CLOCK_REALTIME) : ncmd->stime;
    NandLoc loc;
    NandMediaOp op;

    /* ② bbssd NAND_READ/WRITE/ERASE → media 계층 op */
    switch (ncmd->cmd) {
    case NAND_READ:  op = NAND_MEDIA_READ;    break;
    case NAND_WRITE: op = NAND_MEDIA_PROGRAM; break;
    case NAND_ERASE: op = NAND_MEDIA_ERASE;   break;
    default:
        ftl_err("Unsupported NAND command: 0x%x\n", ncmd->cmd);
        return 0;
    }
    /*
     * Count the read against its block. Every bbssd NAND read passes through
     * here, host or collection, and a read stresses the other pages in the
     * block -- so this is the pressure a device watches to decide when data
     * must be rewritten. Reads answered from the write buffer or the read cache
     * never reach the media and correctly do not count, and a translation-page
     * read is only charged for time: it addresses a placeholder block, whose
     * data pages it does not disturb.
     */
    /* ③ read_cnt는 ssd_read_check_wear()가 read reclaim 판단에 쓴다 */
    if (op == NAND_MEDIA_READ && ncmd->type != MAP_IO) {
        get_blk(ssd, ppa)->read_cnt++;
    }

    /* ④ 지연 계산 본체는 nand_media_op() (hw/femu/nand/nand-media.c) */
    ssd->media_ops[op]++;
    loc = bb_decode_loc(ssd, ppa, stime);
    return nand_media_op(&ssd->media, &loc, op, stime).latency_ns;
}

/*
 * Multi-plane variant: one command sequence addressing the same block on every
 * plane of a LUN, which real NAND runs as a single array operation rather than
 * one erase after another. GC erase reclaims the same block index on every
 * plane, exactly the shape the die can batch; with mp_program and mp_read the
 * host datapath sends programs and reads of one page on several planes here.
 *
 * With one plane there is nothing to batch, so this is the ordinary single-op
 * path and the default configuration is untouched.
 */
/*
 * 호출: ssd_erase_lun_block() (GC erase), ssd_mp_charge() (ftl-datapath.c)
 * 흐름: ① plane 1개면 ssd_advance_status()로 위임 → ② op 변환
 *       → ③ plane마다 read_cnt 집계·NandLoc 생성
 *       → ④ nand_media_multiplane()
 * 반환: 모든 plane을 한 번의 array 동작으로 처리한 지연(ns)
 */
uint64_t ssd_advance_status_multiplane(struct ssd *ssd, struct ppa *ppas,
                                       int nppas, struct nand_cmd *ncmd)
{
    NandLoc locs[1 << PL_BITS];
    uint64_t stime;
    NandMediaOp op;
    int i;

    /* ① 묶을 plane이 없으면 단일 op 경로와 완전히 같다 */
    if (nppas <= 1) {
        return ssd_advance_status(ssd, &ppas[0], ncmd);
    }
    ftl_assert(nppas <= (1 << PL_BITS));

    /* ② ssd_advance_status()와 같은 op 변환 */
    switch (ncmd->cmd) {
    case NAND_READ:  op = NAND_MEDIA_READ;    break;
    case NAND_WRITE: op = NAND_MEDIA_PROGRAM; break;
    case NAND_ERASE: op = NAND_MEDIA_ERASE;   break;
    default:
        ftl_err("Unsupported NAND command: 0x%x\n", ncmd->cmd);
        return 0;
    }

    stime = (ncmd->stime == 0) ?
        qemu_clock_get_ns(QEMU_CLOCK_REALTIME) : ncmd->stime;

    /* ③ 단일 경로와 달리 MAP_IO 구분 없이 read면 plane마다 read_cnt++ */
    for (i = 0; i < nppas; i++) {
        if (op == NAND_MEDIA_READ) {
            get_blk(ssd, &ppas[i])->read_cnt++;
        }
        locs[i] = bb_decode_loc(ssd, &ppas[i], stime);
    }

    /* ④ media_ops는 plane 수와 무관하게 명령 1회로 센다 */
    ssd->media_ops[op]++;
    return nand_media_multiplane(&ssd->media, locs, nppas, op, stime).latency_ns;
}
