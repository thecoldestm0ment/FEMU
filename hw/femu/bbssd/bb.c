#include "../nvme.h"
#include "./ftl.h"
/*
 * [bb.c] FEMU의 BlackBox-SSD(bbssd) 모드를 NVMe controller에 연결하는 glue.
 * 역할: ① nvme_register_bbssd()로 모드별 ops table(FemuExtCtrlOps) 등록
 *       ② namespace 생성 시 geometry·용량·FDP 설정을 검증하고 ssd_init()
 *       ③ FEMU 전용 admin 명령 flip(GC 지연·NAND 지연 on/off 등) 처리
 *       ④ READ/WRITE I/O 명령을 공통 nvme_rw()로 넘김(데이터 이동)
 * 의존: femu.c가 nvme_register_extensions()에서 등록하고 ops로 호출한다.
 *       FTL 본체는 ftl.c(ssd_init/ssd_free/bb_ftl_process_req).
 * 자세한 설명: 참고자료/code_study/11_core_structures_and_dispatch.md
 */

/*
 * controller model/serial 문자열 설정.
 * 호출: ops.init_ctrl_name(femu.c femu_realize()), bb_init()
 */
static void bb_init_ctrl_str(FemuCtrl *n, NvmeNamespace *ns)
{
    static int fsid_vbb = 0;
    const char *vbbssd_mn = "FEMU BlackBox-SSD Controller";
    const char *vbbssd_sn = "vSSD";

    nvme_set_ctrl_name(n, ns, vbbssd_mn, vbbssd_sn, &fsid_vbb);
}

/*
 * A line is only reclaimable by relocating its valid pages somewhere else, so
 * part of the NAND has to stay unexposed. Expose all of it and a host that
 * fills the namespace leaves garbage collection nothing to free: the write
 * path then runs out of lines and aborts mid-run. Refuse the geometry instead,
 * and say what would fit.
 */
/*
 * namespace 크기가 GC 예비 공간(reserve)을 남기는지 검사한다.
 * 호출: bb_init(), csd_init()(csd/csd.c), nvme_ns_mgmt()(nvme-admin.c,
 *       Namespace Management create 때 후보 크기 검사)
 * 흐름: ① 예비 line 수 = 강제 GC 임계 line 수 + 열려 있을 write pointer 수
 *       → ② FDP면 RU 단위로 따로 계산(RUH·RG 수, fdp.runs 크기 검사)
 *       → ③ 비FDP: (tt_lines − reserve) × pgs_per_line ≥ 노출 page 수인지
 * 반환: 0 = OK, -1 = 거부(errp에 이유와 가능한 최대 크기)
 */
int bb_check_capacity(FemuCtrl *n, NvmeNamespace *ns, Error **errp)
{
    BbCtrlParams *p = &n->bb_params;
    uint64_t page_bytes = (uint64_t)p->secs_per_pg * p->secsz;
    /* a line is one block on every plane of every LUN, as the FTL builds it */
    uint64_t pgs_per_line = (uint64_t)p->nchs * p->luns_per_ch *
                            p->pls_per_lun * p->pgs_per_blk;
    uint64_t tt_lines = (uint64_t)p->blks_per_pl;
    uint64_t reserve_lines, usable_pgs, exposed_pgs;

    /* ① GC가 쓸 예비 line 수 계산 */
    /*
     * The free lines GC insists on, plus one for every write pointer this
     * configuration can hold open at once. Counting only the data pointer
     * leaves a device that hot/cold separation or a log-block scheme can run
     * out of lines on, which used to be fatal and is now a refused write.
     */
    reserve_lines = bb_gc_forced_lines(n);
    reserve_lines += 1;                      /* the data write pointer */
    if (n->streams) {
        reserve_lines += n->streams_max + 1;
        if (pgs_per_line > UINT16_MAX) {
            error_setg(errp, "Streams SGS exceeds the 16-bit field");
            return -1;
        }
        if (femu_mapping_name_uses_log_class(p->mapping_scheme)) {
            error_setg(errp, "Streams requires page or dftl mapping");
            return -1;
        }
    }
    if (p->hot_cold_sep) {
        reserve_lines += 1;                  /* the hot write pointer */
    }
    if (femu_mapping_name_uses_log_class(p->mapping_scheme)) {
        reserve_lines += 1;                  /* the log write pointer */
    }

    /* 예) 기본값: 강제 GC 12 line + data wp 1 = 13 line 예약 */
    if (tt_lines <= reserve_lines) {
        error_setg(errp, "FEMU bbssd: the geometry has only %" PRIu64 " lines, "
                   "fewer than the %" PRIu64 " garbage collection needs free",
                   tt_lines, reserve_lines);
        return -1;
    }

    /* ② FDP: line 대신 RU 단위로 예약량과 노출 용량을 따진다 */
    /*
     * Under placement the reclaim units are what the write path draws from, and
     * one is held open per handle with another for the unit being collected
     * into. Fewer than that and the very first setup asks for a unit that is
     * not there, which the handle initialisation dereferences without looking.
     */
    if (n->subsys && n->subsys->endgrp.fdp.enabled) {
        NvmeEnduranceGroup *endgrp = &n->subsys->endgrp;
        uint64_t pool = endgrp->fdp.nru ? MIN(tt_lines, endgrp->fdp.nru) :
                                          tt_lines;
        uint64_t needed = (uint64_t)endgrp->fdp.nruh * endgrp->fdp.nrg +
                          endgrp->fdp.nruh + 1;
        uint64_t reserve;

        /*
         * The FTL's reclaim unit is one superblock, and the host sizes its
         * placement by the unit it is told about.
         */
        if (n->subsys->params.fdp.runs &&
            n->subsys->params.fdp.runs != pgs_per_line * page_bytes) {
            error_setg(errp, "FEMU bbssd: fdp.runs must be %" PRIu64 ", the "
                       "size of one superblock of this geometry, or unset",
                       pgs_per_line * page_bytes);
            return -1;
        }
        if (pool < needed) {
            error_setg(errp, "FEMU bbssd: placement needs %" PRIu64 " reclaim "
                       "units for %u handles across %u groups and this geometry "
                       "gives %" PRIu64 "; raise blks_per_pl or fdp.nru, or "
                       "lower fdp.nruh", needed, endgrp->fdp.nruh,
                       endgrp->fdp.nrg, pool);
            return -1;
        }

        /*
         * Placement draws from the units alone, so they are what bounds the
         * namespace: one held open per handle and group, one more per
         * persistently isolated handle to collect into (the initially
         * isolated handle collects into its open one), and the free units
         * forced collection keeps. The data pointer line counted above is
         * not used under placement.
         */
        pool = endgrp->fdp.nrg * (pool / endgrp->fdp.nrg);
        reserve = (uint64_t)endgrp->fdp.nruh * endgrp->fdp.nrg;
        reserve += endgrp->fdp.nruh -
                   (n->subsys->params.fdp.isolation_mode ? 1 : 0);
        reserve += endgrp->fdp.nrg *
                   bb_fdp_forced_units(n, pool / endgrp->fdp.nrg);
        usable_pgs = pool > reserve ? (pool - reserve) * pgs_per_line : 0;
        exposed_pgs = DIV_ROUND_UP(ns->size, page_bytes);
        if (exposed_pgs > usable_pgs) {
            error_setg(errp, "FEMU bbssd: namespace %u exposes %" PRIu64
                       " KiB, but placement keeps %" PRIu64 " of the %" PRIu64
                       " reclaim units for %u handles and collection, leaving "
                       "%" PRIu64 " KiB; lower devsz_mb or fdp.nruh, raise "
                       "blks_per_pl, or raise op_pcent", ns->id,
                       ns->size >> 10, reserve, pool, endgrp->fdp.nruh,
                       (usable_pgs * page_bytes) >> 10);
            return -1;
        }
        return 0;
    }

    /*
     * ③ 비FDP: 예약 line을 뺀 나머지가 노출 page 수 이상이어야 한다.
     *    예) 기본값: (256 − 13) line × 16384 page × 4KiB = 15552MiB까지
     */
    usable_pgs = (tt_lines - reserve_lines) * pgs_per_line;
    exposed_pgs = DIV_ROUND_UP(ns->size, page_bytes);

    if (exposed_pgs > usable_pgs) {
        error_setg(errp, "FEMU bbssd: namespace %u exposes %" PRIu64 " MiB of "
                   "the %" PRIu64 " MiB this geometry has, leaving garbage "
                   "collection no room; expose at most %" PRIu64 " MiB per "
                   "namespace (lower devsz_mb, or set op_pcent)",
                   ns->id, ns->size >> 20,
                   (tt_lines * pgs_per_line * page_bytes) >> 20,
                   (usable_pgs * page_bytes) >> 20);
        return -1;
    }

    return 0;
}

/*
 * FDP keeps its own write and reclaim path, which none of the knobs below
 * reach; refuse them rather than accept them silently. Every mode that runs
 * this FTL under FDP needs the refusal and the reclaim unit size, not only
 * bbssd. A no-op without FDP.
 */
/*
 * FDP가 켜진 경우: FDP 경로에 효과 없는 옵션(buffer_size, hot_cold_sep,
 * mapping, gc_policy 등)을 거부하고, RU 크기 fdp.runs를 정한다.
 * 호출: bb_init(), csd_init()(csd/csd.c)
 * 반환: 0 = OK(FDP가 아니면 바로 0), -1 = 거부
 */
int bb_init_fdp(FemuCtrl *n, NvmeNamespace *ns, const char *mode,
                Error **errp)
{
    const BbCtrlParams *p = &n->bb_params;
    const char *knob = NULL;

    if (!n->subsys || !n->subsys->endgrp.fdp.enabled) {
        return 0;
    }

    if (p->buffer_size) {
        knob = "buffer_size";
    } else if (p->hot_cold_sep) {
        knob = "hot_cold_sep";
    } else if (p->read_reclaim_limit) {
        knob = "read_reclaim_limit";
    } else if (p->retention_limit_sec) {
        knob = "retention_limit_sec";
    } else if (p->ecc_retention_sec) {
        knob = "ecc_retention_sec";
    } else if (p->trim_lat_ns) {
        knob = "trim_lat_ns";
    } else if (p->mapping_scheme && strcmp(p->mapping_scheme, "page")) {
        knob = "mapping";
    } else if (p->gc_policy && strcmp(p->gc_policy, "greedy")) {
        knob = "gc_policy";
    }
    if (knob) {
        error_setg(errp, "FEMU %s: %s has no effect under FDP", mode, knob);
        return -1;
    }

    /* RU 하나 = line 하나(lines_per_ru = 1)이므로 RU 크기 = line의 byte 크기 */
    n->subsys->endgrp.fdp.runs = (uint64_t)p->nchs * p->luns_per_ch *
                                 p->pls_per_lun * p->pgs_per_blk *
                                 p->secs_per_pg * p->secsz;
    nvme_ns_refresh_fdp(ns);
    return 0;
}

/* bb <=> black-box */
/*
 * bbssd namespace 하나를 초기화하는 ops.init 콜백.
 * 호출: nvme_register_bbssd()의 ops 등록 → femu.c가 namespace마다
 *       ns->ext_ops.init()으로 호출(femu_realize(), nvme_ns_create())
 * 흐름: ① CXL medium이 이미 만든 FTL이면 빌려 쓰고 끝
 *       → ② geometry·용량·FDP 설정 검증 → ③ struct ssd 할당·이름 설정
 *       → ④ ssd_init()으로 FTL 구성
 */
static void bb_init(FemuCtrl *n, NvmeNamespace *ns, Error **errp)
{
    struct ssd *ssd;

    /* ① CXL medium이 FTL을 이미 만들었으면 빌려 쓴다(ssd_borrowed) */
    /* A linked CXL medium already built the FTL from its own geometry. */
    if (n->cxl_ssd) {
        ns->ssd = n->cxl_ssd;
        ns->ssd_borrowed = true;
        n->ssd = ns->ssd;
        bb_init_ctrl_str(n, ns);
        return;
    }

    /* ② geometry → 용량(GC 예약) → FDP 옵션 순으로 검증 */
    if (bb_check_geometry(n, errp)) {
        return;
    }

    if (bb_check_capacity(n, ns, errp)) {
        return;
    }

    if (bb_init_fdp(n, ns, "bbssd", errp)) {
        return;
    }

    /* ③ namespace 전용 struct ssd 할당(0으로 채움) */
    ssd = ns->ssd = g_malloc0(sizeof(struct ssd));

    if (!n->ns_mgmt) {
        bb_init_ctrl_str(n, ns);
    }

    /*
     * Each bbssd namespace carries its own FTL, so the controller can mix it
     * with namespaces of other modes. The first one also answers the
     * controller-wide queries that predate per-namespace state.
     */
    if (!n->ssd) {
        n->ssd = ssd;
    }

    ssd->dataplane_started_ptr = &n->dataplane_started;
    ssd->ssdname = (char *)n->devname;
    femu_debug("Starting FEMU in Blackbox-SSD mode ...\n");
    /* ④ FTL 구성(ftl.c) */
    ssd_init(n, ns);
}

/*
 * Apply a timing or GC toggle to every namespace carrying an FTL. Flip arrives
 * as an admin command and names no namespace, so it is device-wide; n->ssd is
 * only whichever namespace brought its mode up first, and using it would leave
 * every other FTL-backed namespace on the previous setting.
 */
/*
 * 호출: bb_flip()
 * 흐름: ① poller 정지 → ② CXL medium이면 그쪽 flip에 위임
 *       → ③ namespace마다 sp 값 변경 + media timing 갱신 → ④ poller 재개
 */
static void bb_flip_apply(FemuCtrl *n, int64_t cdw10)
{
    bool resume;
    int i;

    /* ① poller 정지: FTL thread가 매 요청 읽는 sp 값을 바꾸기 전에 멈춘다 */
    /*
     * These are the timings the FTL thread reads on every request, and the
     * media layer is rebuilt from them below. Stop the dataplane first: this
     * runs on the thread that took the admin command, so nothing else keeps
     * the fields still.
     */
    resume = nvme_pause_pollers(n);

    /* ② CXL medium이면 그쪽 worker가 같은 timing을 쓰므로 위임 */
    /* The medium's worker shares these timings; it applies them locked. */
    if (n->cxl_media) {
        femu_cxl_nvme_ops->flip(n, cdw10);
        nvme_resume_pollers(n, resume);
        return;
    }

    /* ③ FTL을 가진 모든 namespace에 같은 설정을 적용 */
    for (i = 0; i < n->namespace_limit; i++) {
        struct ssd *ssd = n->namespaces[i].ssd;

        if (!n->namespaces[i].allocated || !ssd) {
            continue;
        }

        switch (cdw10) {
        case FEMU_ENABLE_GC_DELAY:
            ssd->sp.enable_gc_delay = true;
            break;
        case FEMU_DISABLE_GC_DELAY:
            ssd->sp.enable_gc_delay = false;
            break;
        case FEMU_ENABLE_DELAY_EMU:
            ssd->sp.pg_rd_lat = NAND_READ_LATENCY;
            ssd->sp.pg_wr_lat = NAND_PROG_LATENCY;
            ssd->sp.blk_er_lat = NAND_ERASE_LATENCY;
            ssd->sp.ch_xfer_lat = 0;
            /* refresh는 rd/wr/er 3개만 media로 복사(ch_xfer_lat는 미반영) */
            /* refresh the media-layer timing snapshot from the updated params */
            bb_nand_media_refresh_timing(ssd);
            break;
        case FEMU_DISABLE_DELAY_EMU:
            ssd->sp.pg_rd_lat = 0;
            ssd->sp.pg_wr_lat = 0;
            ssd->sp.blk_er_lat = 0;
            ssd->sp.ch_xfer_lat = 0;
            /* refresh the media-layer timing snapshot from the updated params */
            bb_nand_media_refresh_timing(ssd);
            break;
        default:
            break;
        }
    }

    /* ④ poller 재개 */
    nvme_resume_pollers(n, resume);
}

/*
 * FEMU 전용 admin 명령 flip(NVME_ADM_CMD_FEMU_FLIP = 0xef) 처리.
 * cdw10: 1/2 GC 지연 on/off, 3/4 NAND 지연 에뮬레이션 on/off,
 *        5 지연 통계 리셋, 6/7 로그 출력 on/off
 * 호출: bb_admin_cmd()
 */
static void bb_flip(FemuCtrl *n, NvmeCmd *cmd)
{
    int64_t cdw10 = le64_to_cpu(cmd->cdw10);

    switch (cdw10) {
    case FEMU_ENABLE_GC_DELAY:
        bb_flip_apply(n, cdw10);
        femu_log("%s,FEMU GC Delay Emulation [Enabled]!\n", n->devname);
        break;
    case FEMU_DISABLE_GC_DELAY:
        bb_flip_apply(n, cdw10);
        femu_log("%s,FEMU GC Delay Emulation [Disabled]!\n", n->devname);
        break;
    case FEMU_ENABLE_DELAY_EMU:
        bb_flip_apply(n, cdw10);
        femu_log("%s,FEMU Delay Emulation [Enabled]!\n", n->devname);
        break;
    case FEMU_DISABLE_DELAY_EMU:
        bb_flip_apply(n, cdw10);
        femu_log("%s,FEMU Delay Emulation [Disabled]!\n", n->devname);
        break;
    case FEMU_RESET_ACCT: {
        /* counters are sharded per poller (see FemuPollerCtr); sum then reset */
        int64_t tt = 0, late = 0;
        if (n->poller_ctr) {
            for (uint32_t p = 1; p <= n->nr_pollers; p++) {
                tt += n->poller_ctr[p].nr_tt_ios;
                late += n->poller_ctr[p].nr_tt_late_ios;
                n->poller_ctr[p].nr_tt_ios = 0;
                n->poller_ctr[p].nr_tt_late_ios = 0;
            }
        }
        femu_log("%s,Reset tt_late_ios/tt_ios,%ld/%ld\n", n->devname, late, tt);
        break;
    }
    case FEMU_ENABLE_LOG:
        n->print_log = true;
        femu_log("%s,Log print [Enabled]!\n", n->devname);
        break;
    case FEMU_DISABLE_LOG:
        n->print_log = false;
        femu_log("%s,Log print [Disabled]!\n", n->devname);
        break;
    default:
        printf("FEMU:%s,Not implemented flip cmd (%lu)\n", n->devname, cdw10);
    }
}

/*
 * Release what the namespace's FTL still holds. Reached for the mode the
 * controller itself runs; a namespace running bbssd underneath a controller of
 * another mode is not dispatched an exit at all, which is a gap in the generic
 * teardown rather than one here.
 */
/*
 * Release the FTL this mode built. Each mode allocates its own ns->ssd and
 * frees the ones its namespaces own, so a controller mixing modes tears each
 * down exactly once. n->ssd aliases the first namespace's, so clear it before
 * the memory goes.
 */
/*
 * ops.ns_exit: namespace 하나의 FTL 해제. CXL에서 빌린 ssd는 해제 안 함.
 * 호출: nvme_ns_release()(femu.c)의 ns->ext_ops.ns_exit, bb_exit()
 */
static void bb_ns_exit(FemuCtrl *n, NvmeNamespace *ns)
{
    if (!ns->ssd) {
        return;
    }
    if (n->ssd == ns->ssd) {
        n->ssd = NULL;
    }
    if (ns->ssd_borrowed) {
        ns->ssd = NULL;
        return;
    }
    ssd_free(ns->ssd);
    g_free(ns->ssd);
    ns->ssd = NULL;
}

/* ops.exit: 모든 bbssd namespace 해제. 호출: femu_exit_extensions()(femu.c) */
static void bb_exit(FemuCtrl *n)
{
    int i;

    for (i = 0; i < n->namespace_limit; i++) {
        NvmeNamespace *ns = &n->namespaces[i];

        if (NS_BBSSD(ns)) {
            bb_ns_exit(n, ns);
        }
    }
}

/*
 * READ/WRITE 공통 처리 nvme_rw()(nvme-io.c)로 위임: 요청 검증, guest 메모리
 * ↔ backend 메모리 DMA(power_loss 모델의 write는 req->write_data에 보관),
 * req->slba/nlb 설정. 지연 계산은 그 뒤 to_ftl ring을 거쳐 FTL thread의
 * bb_ftl_process_req()에서 한다.
 * 호출: bb_io_cmd()
 */
static uint16_t bb_nvme_rw(FemuCtrl *n, NvmeNamespace *ns, NvmeCmd *cmd,
                           NvmeRequest *req)
{
    return nvme_rw(n, ns, cmd, req);
}

/*
 * ops.io_cmd. 호출: nvme_io_cmd()(nvme-io.c)의 default 분기.
 * FLUSH/DSM/WRITE_ZEROES/COPY 등은 nvme_io_cmd()가 먼저 처리하고,
 * 나머지가 여기로 온다. bbssd는 그중 READ/WRITE만 받는다.
 */
static uint16_t bb_io_cmd(FemuCtrl *n, NvmeNamespace *ns, NvmeCmd *cmd,
                          NvmeRequest *req)
{
    switch (cmd->opcode) {
    case NVME_CMD_READ:
    case NVME_CMD_WRITE:
        return bb_nvme_rw(n, ns, cmd, req);
    default:
        return NVME_INVALID_OPCODE | NVME_DNR;
    }
}

/* ops.admin_cmd. 호출: nvme_admin_cmd()(nvme-admin.c)의 default 분기 */
static uint16_t bb_admin_cmd(FemuCtrl *n, NvmeCmd *cmd)
{
    switch (cmd->opcode) {
    case NVME_ADM_CMD_FEMU_FLIP:
        bb_flip(n, cmd);
        return NVME_SUCCESS;
    default:
        return NVME_INVALID_OPCODE | NVME_DNR;
    }
}

/*
 * bbssd 모드의 ops table(FemuExtCtrlOps)을 채운다.
 * 호출: nvme_register_extensions()(femu.c). namespace별 table은
 *       nvme_register_extensions_ns()가 이 결과를 복사해 만든다.
 */
int nvme_register_bbssd(FemuCtrl *n)
{
    n->ext_ops = (FemuExtCtrlOps) {
        .state            = NULL,
        .init_ctrl_name   = bb_init_ctrl_str,
        .ns_exit          = bb_ns_exit,
        .init             = bb_init,
        .exit             = bb_exit,
        .rw_check_req     = NULL,
        .admin_cmd        = bb_admin_cmd,
        .io_cmd           = bb_io_cmd,
        .get_log          = NULL,
    };

    return 0;
}

