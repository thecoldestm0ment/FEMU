#include "ftl.h"
#include "ftl-internal.h"
/*
 * [ftl.c] bbssd FTL의 생성·해제와 요청 dispatch를 맡는 "본체" 진입점.
 * 주요 진입 함수:
 *   ssd_init()           — namespace 하나의 struct ssd 전체를 구성
 *   bb_ftl_process_req() — FTL thread가 요청마다 호출. opcode별로 datapath
 *                          (ssd_read/ssd_write/...)로 보내고 끝에 GC 판정
 *   ssd_free()           — ssd_init()의 역순 해제
 *   ssd_*() 통계 접근자  — SMART/log page용 카운터(nvme-admin.c가 읽음)
 * 의존: 위에서 femu.c(FTL thread)·bb.c·csd/·kvssd/·cxlssd/가 부르고,
 *       아래로 ftl-geom/map/line-gc/datapath/fdp/media/cache.c를 부른다.
 * 자세한 설명: 참고자료/code_study/11_core_structures_and_dispatch.md
 */

//#define FEMU_DEBUG_FTL






/*
 * namespace 하나의 FTL 상태(struct ssd)를 처음부터 구성한다.
 * 호출: bb_init()(bb.c), csd_init()(csd/csd.c),
 *       kvssd_init_timing_ssd()(kvssd/kvssd-ftl.c),
 *       femu_cxl_start()(cxlssd/cxlssd.c)
 * 흐름: ① property → ssd->sp 파라미터 → ② channel~page 배열 할당
 *       → ③ cell type 검증·오류 주입·SMART 카운터 → ④ NAND timing 엔진
 *       → ⑤ maptbl/rmap → ⑥ GC policy 선택 후 line 생성
 *       → ⑦ mapping scheme·CMT·read cache → ⑧ FDP 구조 또는 write pointer
 */
void ssd_init(FemuCtrl *n, NvmeNamespace *ns)
{
    struct ssd *ssd = ns->ssd;
    struct ssdparams *spp = &ssd->sp;

    ftl_assert(ssd);
    ssd->n = n;

    /* read the data-remanence experiment env vars once (debug only, off by default) */
    exp_load_cfg();

    /* ① n->bb_params(property) → ssd->sp: geometry·지연·GC 임계값 계산 */
    ssd_init_params(spp, n);

    /* ② ch[nchs] → lun[] → pl[] → blk[] → pg[] 할당(모든 page PG_FREE) */
    /* initialize ssd internal layout architecture */
    ssd->ch = g_malloc0(sizeof(struct ssd_channel) * spp->nchs);
    for (int i = 0; i < spp->nchs; i++) {
        ssd_init_ch(&ssd->ch[i], spp);
    }

    /* ③ cell type·page-type 모델 검증, 오류 주입·WAF·SMART 카운터 초기화 */
    /*
     * Validate the optional NAND cell type before the media layer or the datapath
     * indexes the per-type tables with it. Only SLC..QLC have populated tables and
     * page-type pairing; anything else (including PLC) falls back to flat timing.
     */
    if (n->nand_cell_type &&
        (n->nand_cell_type < SLC || n->nand_cell_type > QLC)) {
        ftl_err("nand_cell_type %u out of range (1 SLC..4 QLC); using flat timing\n",
                n->nand_cell_type);
        n->nand_cell_type = 0;
    }
    /*
     * The pgtype_lat program model needs a bits-per-cell count to pick a
     * multiplier row and to fold the page number into a page type; default it to
     * TLC when the model is on but cell_pages was left unset, so the page type
     * and the multiplier row stay consistent.
     */
    if (spp->pgtype_lat && spp->cell_pages < 1) {
        spp->cell_pages = 3; /* TLC */
    }

    /* factory bad-block count, reported via SMART available_spare; clamp to the
     * total block count so a misconfigured value cannot exceed 100% depletion */
    /* (위 영어 주석은 아래쪽 bad_blocks 설정 설명. 이 줄은 debug_ftl 복사) */
    ssd->debug_ftl = n->debug_ftl;

    /*
     * Turn a parts-per-million rate into a 1-in-N period, clamped so a non-zero
     * rate always injects something. Zero leaves injection off.
     */
    /* 예) err_read_unc_ppm=100 → period 10000 → read 10000번마다 1번 실패 */
    ssd->err_read_unc_period = 0;
    ssd->err_read_counter = 0;
    ssd->err_read_injected = 0;
    if (n->err_read_unc_ppm) {
        ssd->err_read_unc_period = 1000000u / n->err_read_unc_ppm;
        if (ssd->err_read_unc_period < 1) {
            ssd->err_read_unc_period = 1;
        }
    }
    ssd->err_write_fail_period = 0;
    ssd->err_write_counter = 0;
    ssd->err_write_injected = 0;
    if (n->err_write_fail_ppm) {
        ssd->err_write_fail_period = 1000000u / n->err_write_fail_ppm;
        if (ssd->err_write_fail_period < 1) {
            ssd->err_write_fail_period = 1;
        }
    }
    ssd->host_write_pages = 0;
    ssd->nand_write_pages = 0;
    ssd->gc_write_pages = 0;

    ssd->bad_blocks = n->nand_bad_blocks;
    if (spp->tt_blks > 0 && ssd->bad_blocks > (uint32_t)spp->tt_blks) {
        ssd->bad_blocks = (uint32_t)spp->tt_blks;
    }

    /*
     * An explicit rating wins; otherwise take the one the cell type implies.
     * A device configured with neither keeps zero and reports no life estimate.
     */
    ssd->total_erases = 0;
    ssd->rated_pe_cycles = n->pe_cycles_rated ? n->pe_cycles_rated :
                           get_rated_pe_cycles(n->nand_cell_type);

    /* ④ NAND timing 엔진(nand/nand-media.c) 설정: sp 지연값·cell type 반영 */
    /* configure the NAND media-layer timing (reads spp, points at ssd->ch) */
    bb_nand_media_init(ssd);

    /* ⑤ L2P 표: maptbl은 전부 UNMAPPED_PPA, rmap은 전부 INVALID_LPN */
    /* initialize maptbl */
    ssd_init_maptbl(ssd);

    /* initialize rmap */
    ssd_init_rmap(ssd);

    /*
     * ⑥ GC policy(gc_policy, 기본 greedy)를 먼저 고른다: policy가 victim
     *    pqueue의 정렬 키(vpc 또는 close_seq)를 정하기 때문. 그 뒤 모든
     *    line을 free_line_list에 넣는다.
     */
    /* the policy orders the victim queue, so resolve it before the lines */
    ssd->policy = femu_ftl_policy_lookup(n->bb_params.gc_policy);
    ssd->gc_rng = n->gc_seed;

    /* initialize all the lines */
    ssd_init_lines(ssd);

    /* ⑦ L2P mapping scheme(page/dftl/hybrid/fast) + CMT + DRAM read cache */
    /*
     * L2P mapping scheme: "page" (default, full DRAM L2P, bit-identical) vs "dftl"
     * (demand-cached L2P via the CMT). The CMT turns on when dftl is named or
     * mapping_cache_mb is set; a named dftl with no explicit size gets 4 MB.
     */
    ssd->mapping = femu_mapping_scheme_lookup(n->bb_params.mapping_scheme);
    /* schemes that keep private state allocate it here; page/dftl keep it NULL */
    ssd->map_priv = NULL;
    if (ssd->mapping->init) {
        ssd->mapping->init(ssd);
    }
    uint32_t map_cache_mb = n->mapping_cache_mb;
    if (femu_mapping_scheme_uses_cmt(ssd->mapping) && map_cache_mb == 0) {
        map_cache_mb = 4; /* default dftl cache when no explicit size given */
    }
    /* the CMT is a dftl-only cost model: never charge it under the page scheme,
     * even if mapping_cache_mb was set */
    cmt_init(ssd, femu_mapping_scheme_uses_cmt(ssd->mapping) ? map_cache_mb : 0);

    /*
     * Optional DRAM read cache (opt-in via read_cache_mb; 0 = off, bit-identical
     * default). cache_evict selects the eviction policy.
     */
    uint32_t rc_evict = 0; /* 0 = CLOCK (default) */
    if (n->bb_params.cache_evict) {
        if (!strcmp(n->bb_params.cache_evict, "random")) {
            rc_evict = 1;
        } else if (!strcmp(n->bb_params.cache_evict, "lru")) {
            rc_evict = 2;
        } else if (!strcmp(n->bb_params.cache_evict, "arc")) {
            rc_evict = 3; /* scan-resistant 2Q-style */
        }
    }
    rcache_init(ssd, n->read_cache_mb, rc_evict);

    /*
     * ⑧ FDP: RG/RU/RUH 구성(RU마다 free line을 미리 배정)
     *    비FDP: 첫 free line을 wp에 물리고 DRAM write buffer 초기화
     */
    /* FDP vs non-FDP init path */
    ssd->fdp_enabled = (n->subsys != NULL &&
                        n->subsys->params.fdp.enabled);
    ssd->fdp_debug = (getenv("FEMU_FDP_DEBUG") != NULL);

    if (ssd->fdp_enabled) {
        ssd_init_fdp_params(spp, n);

        ftl_log("FDP: initializing reclaim groups\n");
        femu_fdp_ssd_init_reclaim_group(n, ssd);
        ftl_log("FDP: initializing RU handles\n");
        femu_fdp_ssd_init_ru_handles(n, ssd);
        ftl_log("FDP: init complete (nrg=%lu, nruhs=%lu)\n",
                ssd->nrg, ssd->nruhs);
    } else {
        /* non-FDP: use single write pointer */
        ssd_init_write_pointer(ssd);
    }
}

/*
 * Write-amplification factor scaled by 1000, so it needs no floating point on
 * the read path: pages written to NAND over pages the host wrote. Reads 1000
 * (a factor of 1.0) before the host has written anything.
 *
 * The denominator is what the host wrote, not what reached the media, so a
 * write buffer that absorbs repeated writes to the same page shows up as the
 * reduction in amplification that it is. With no buffer configured the two are
 * equal and this is the same ratio it has always been.
 */
/* 참고: 현재 hw/femu 안에서 호출하는 곳이 없다(nvme.h에 선언만 있음). */
uint32_t ssd_waf_x1000(struct ssd *ssd)
{
    uint64_t host = ssd->host_write_pages;
    uint64_t total = ssd->nand_write_pages + ssd->gc_write_pages;

    if (host == 0) {
        return 1000;
    }

    return (uint32_t)((total * 1000ull) / host);
}

/* struct ssd is opaque outside the FTL, so the raw counters need accessors */
/*
 * WAF 원자료 접근자. 호출: nvme_collect_media_stats()(nvme-admin.c);
 * ssd_nand_write_pages()는 cxlssd/cxlssd.c에서도 쓴다.
 */
uint64_t ssd_host_write_pages(struct ssd *ssd)
{
    return ssd->host_write_pages;
}

uint64_t ssd_gc_write_pages(struct ssd *ssd)
{
    return ssd->gc_write_pages;
}

uint64_t ssd_nand_write_pages(struct ssd *ssd)
{
    return ssd->nand_write_pages;
}

/*
 * Check the mapping against the media. @lost counts logical pages whose
 * mapping names a page that does not hold them: one not valid, or one the
 * reverse map gives to another page. @orphans counts valid pages that no
 * mapping names. Collection that erases a block it has not emptied leaves the
 * first; a relocated copy left marked valid leaves the second.
 */
/*
 * maptbl·rmap·page 상태가 서로 맞는지 전수 검사한다(디버그·테스트용).
 * 호출: femu_test_ftl_check()(femu.c, qtest 전용 "x-ftl-check" property)
 * 흐름: ① LPN마다 maptbl이 가리키는 page가 VALID이고 rmap이 같은 LPN인지
 *          → 아니면 lost
 *       ② 물리 page마다 VALID page의 rmap 주인 LPN이 다시 이 page를
 *          가리키는지 → 아니면 orphans
 */
void ssd_check_mapping(struct ssd *ssd, uint64_t *mapped, uint64_t *lost,
                       uint64_t *orphans)
{
    struct ssdparams *spp = &ssd->sp;
    struct ppa owner;
    uint64_t lpn;
    struct ppa ppa;

    *mapped = *lost = *orphans = 0;
    /* ① 정방향: LPN → PPA → (page 상태, rmap) */
    for (lpn = 0; lpn < spp->tt_pgs; lpn++) {
        ppa = get_maptbl_ent(ssd, lpn);
        if (!mapped_ppa(&ppa)) {
            continue;
        }
        (*mapped)++;
        if (get_pg(ssd, &ppa)->status != PG_VALID ||
            get_rmap_ent(ssd, &ppa) != lpn) {
            (*lost)++;
        }
    }

    /* ② 역방향: 모든 물리 page를 돌며 VALID page의 주인 확인 */
    ppa.ppa = 0;
    for (int ch = 0; ch < spp->nchs; ch++) {
        ppa.g.ch = ch;
        for (int lun = 0; lun < spp->luns_per_ch; lun++) {
            ppa.g.lun = lun;
            for (int pl = 0; pl < spp->pls_per_lun; pl++) {
                ppa.g.pl = pl;
                for (int blk = 0; blk < spp->blks_per_pl; blk++) {
                    ppa.g.blk = blk;
                    for (int pg = 0; pg < spp->pgs_per_blk; pg++) {
                        ppa.g.pg = pg;
                        if (get_pg(ssd, &ppa)->status != PG_VALID) {
                            continue;
                        }
                        lpn = get_rmap_ent(ssd, &ppa);
                        owner.ppa = UNMAPPED_PPA;
                        if (lpn < spp->tt_pgs) {
                            owner = get_maptbl_ent(ssd, lpn);
                        }
                        *orphans += owner.ppa != ppa.ppa;
                    }
                }
            }
        }
    }
}

/* Lines rewritten because a block of theirs passed the read stress limit. */
/*
 * Copy: read every source range, then program the destination as one write.
 * The write cannot start before its data has been read, so the latency is the
 * slowest read plus the write.
 */
/*
 * (맨 위 "Lines rewritten ..." 한 줄은 아래쪽 ssd_read_reclaims()의 설명이
 *  잘못 놓인 것이다.)
 * NVMe Copy의 FTL 비용 계산. 데이터 복사는 nvme_copy()(nvme-io.c)가 이미
 * 끝냈고, 원본 namespace id를 dsm_ranges[i].cattr에 담아 넘긴다.
 * 호출: bb_ftl_process_req()의 NVME_CMD_COPY
 * 흐름: ① range마다 원본 ssd에서 ssd_read() → ② 목적지 범위로 한 번 write
 * 반환: 가장 느린 read 지연 + write 지연(ns)
 */
static uint64_t ssd_copy(FemuCtrl *n, struct ssd *ssd, NvmeRequest *req)
{
    uint64_t dslba = req->slba;
    uint32_t dnlb = req->nlb;
    uint64_t rlat = 0;
    uint64_t wlat;

    NvmeNamespace *dns = req->ns;

    /* ① 원본 range별 read. 다른 namespace면 그 ns의 ssd·offset으로 계산 */
    for (int i = 0; i < req->dsm_nr_ranges; i++) {
        uint32_t snsid = le32_to_cpu(req->dsm_ranges[i].cattr);
        struct ssd *from = ssd;
        uint64_t lat;

        /*
         * A range from another namespace is read from that one's media, at
         * that one's place in the backing store, which is what the request's
         * namespace stands for while the read is charged.
         */
        if (snsid && snsid != dns->id) {
            NvmeNamespace *source = nvme_ns(n, snsid);

            /* a namespace without media (no-SSD) costs nothing to read */
            if (!source || !source->ssd) {
                continue;
            }
            from = source->ssd;
            req->ns = source;
        }
        req->slba = le64_to_cpu(req->dsm_ranges[i].slba);
        req->nlb = le32_to_cpu(req->dsm_ranges[i].nlb);
        lat = ssd_read(from, req);
        req->ns = dns;
        rlat = MAX(rlat, lat);
    }
    /* ② slba/nlb를 목적지로 되돌리고 write (FDP면 RUH 배치 경로) */
    req->slba = dslba;
    req->nlb = dnlb;
    if (ssd->fdp_enabled) {
        wlat = nvme_do_write_fdp(n, req, dslba, dnlb);
    } else {
        wlat = ssd_write(ssd, req);
    }

    return rlat + wlat;
}

/* read disturb로 다시 쓴 line 수. 호출: nvme_collect_media_stats() */
uint64_t ssd_read_reclaims(struct ssd *ssd)
{
    return ssd->read_reclaims;
}

/* Bytes in one NAND page, for counters the host wants in bytes. */
/*
 * 호출: nvme_collect_media_stats(), ssd_query_collect()(ftl-query.c),
 *       buffer_write_data()·bbssd_power_loss()(ftl-datapath.c)
 */
uint32_t ssd_page_size(struct ssd *ssd)
{
    return (uint32_t)ssd->sp.secs_per_pg * (uint32_t)ssd->sp.secsz;
}

/*
 * Media and data integrity errors, as SMART counts them: the reads and writes
 * the device failed and reported to the host. Only fault insertion produces
 * them, so a device with none configured reports none.
 */
/* 오류 주입으로 실패시킨 read+write 수. 호출: nvme_collect_media_stats() */
uint64_t ssd_media_errors(struct ssd *ssd)
{
    return ssd->err_read_injected + ssd->err_write_injected;
}

/* Lines rewritten because their data had sat programmed past the retention limit. */
/* 호출: nvme_collect_media_stats() */
uint64_t ssd_retention_refreshes(struct ssd *ssd)
{
    return ssd->retention_refreshes;
}

/*
 * Write buffer effectiveness. The hit counts are the pages the buffer answered
 * without touching the media: a read it still held, and a write that superseded
 * a page it already held and so owed no extra program. Reported against the
 * host totals so the ratio is computable, since the buffer is only configured
 * on some devices and a zero denominator means it was never exercised.
 */
/* 아래 4개는 ssd->sp의 buffer 카운터 반환. 호출: nvme_collect_media_stats() */
uint64_t ssd_buffer_reads(struct ssd *ssd)
{
    return ssd->sp.read_cnt;
}

uint64_t ssd_buffer_read_hits(struct ssd *ssd)
{
    return ssd->sp.read_hit_cnt;
}

uint64_t ssd_buffer_writes(struct ssd *ssd)
{
    return ssd->sp.write_cnt;
}

uint64_t ssd_buffer_write_hits(struct ssd *ssd)
{
    return ssd->sp.write_hit_cnt;
}

/*
 * The most-read block since its last erase. A device rewrites data before read
 * stress accumulates far enough to cost it; this is the number that decision
 * would be made on.
 */
/* 전체 block의 read_cnt 최댓값(전수 순회). 호출: nvme_collect_media_stats() */
uint64_t ssd_max_block_reads(struct ssd *ssd)
{
    struct ssdparams *spp = &ssd->sp;
    uint64_t most = 0;
    int ch, lun, pl, blk;

    for (ch = 0; ch < spp->nchs; ch++) {
        for (lun = 0; lun < spp->luns_per_ch; lun++) {
            for (pl = 0; pl < spp->pls_per_lun; pl++) {
                struct nand_plane *plane = &ssd->ch[ch].lun[lun].pl[pl];

                for (blk = 0; blk < spp->blks_per_pl; blk++) {
                    if (plane->blk[blk].read_cnt > most) {
                        most = plane->blk[blk].read_cnt;
                    }
                }
            }
        }
    }

    return most;
}

/*
 * SMART available_spare: 100% on a healthy device, reduced by the factory
 * bad-block fraction (bad_blocks / tt_blks) as bad blocks consume the
 * over-provisioned reserve. A reported value only -- placement is unaffected.
 */
/*
 * SMART percentage_used: the average program/erase cycles a block has taken,
 * as a percentage of what the media is rated for. The specification allows
 * values above 100 and caps the reported figure at 255. Zero when the device
 * was given no endurance rating, which means no estimate rather than a new
 * device -- a new device also reports zero, and the two are indistinguishable
 * in this field by design.
 */
/*
 * (첫 번째 영어 주석 "SMART available_spare"는 아래 ssd_available_spare()
 *  설명이다.) percentage_used = total_erases × 100 / (tt_blks × 정격 P/E)
 * 호출: nvme_collect_media_stats()
 */
uint8_t ssd_percentage_used(struct ssd *ssd)
{
    struct ssdparams *spp = &ssd->sp;
    uint64_t denom, pct;

    if (!ssd->rated_pe_cycles || spp->tt_blks <= 0) {
        return 0;
    }
    /*
     * Divide once. Taking the average first threw the fraction away, so a
     * device half way through its first cycle across every block -- a third
     * of a ten-cycle rating, say -- still reported nothing used.
     */
    denom = (uint64_t)spp->tt_blks * ssd->rated_pe_cycles;
    pct = (ssd->total_erases * 100ull) / denom;

    return pct > 255 ? 255 : (uint8_t)pct;
}

/*
 * available_spare(%) = 100 − bad_blocks × 100 / tt_blks
 * 호출: nvme_collect_media_stats()
 */
uint8_t ssd_available_spare(struct ssd *ssd)
{
    struct ssdparams *spp = &ssd->sp;
    uint64_t bad_pct;

    if (ssd->bad_blocks == 0 || spp->tt_blks <= 0) {
        return 100;
    }
    bad_pct = ((uint64_t)ssd->bad_blocks * 100ull) / (uint64_t)spp->tt_blks;
    return bad_pct >= 100 ? 0 : (uint8_t)(100 - bad_pct);
}



/*
 * ssd_advance_status now lives in the NAND media-layer bridge
 * (hw/femu/bbssd/ftl-media.c), which runs the same flat per-LUN read/program/
 * erase timing through the uniform nand_media API.
 */




/*
 * Run one request against this namespace's bbssd FTL and return the latency to
 * charge it. The controller's FTL thread calls this once it has picked out the
 * namespace, so one controller can serve namespaces of different modes.
 */
/*
 * FTL thread가 요청 하나를 처리하는 bbssd FTL의 단일 진입점.
 * 호출: femu_ftl_process_req()(femu.c, femu_ftl_thread()가 ring에서 꺼낸
 *       요청마다), femu_cxl_nvme_ftl()·cxl_worker()(cxlssd/cxlssd.c)
 * 흐름: ① 이미 실패한 요청은 그대로 통과 → ② Streams면 잠금
 *       → ③ opcode별 datapath 호출로 지연 계산 → ④ 오류 주입
 *       → ⑤ background GC 판정·1회 수행 → ⑥ 잠금 해제
 * 반환: req->stime 기준 완료까지의 지연(ns). femu_ftl_thread()가
 *       req->expire_time에 더하고, poller가 그 시각에 completion을 올린다.
 *       데이터 자체는 (기본 모드에서) nvme_rw()가 이미 backend 메모리로
 *       옮겼고, 여기서는 "시간"만 계산한다.
 */
uint64_t bb_ftl_process_req(FemuCtrl *n, NvmeNamespace *ns, NvmeRequest *req)
{
    struct ssd *ssd = ns->ssd;
    uint64_t lat = 0;

    if (!ssd) {
        return 0;
    }

    /* ① 검증 실패 요청: 처리하지 않고 지연 0 → poller가 오류 상태로 완료 */
    /*
     * A request that already failed validation in the I/O layer is only routed
     * through the FTL ring so the poller completes it with its carried error
     * status. It must not run any opcode handler: req->slba and req->nlb may be
     * stale (the I/O layer returns before setting them) and ssd_read/ssd_write
     * would then touch unrelated mapping state and overwrite the error. Leave
     * latency at zero and let the poller post the carried status.
     */
    if (req->status != NVME_SUCCESS) {
        return 0;
    }

    /* ② Streams 상태는 admin 경로(nvme-streams.c)와 공유 → 처리 중 잠금 */
    if (n->streams) {
        qemu_mutex_lock(&n->streams_lock);
    }
    /* ③ opcode별 datapath 호출. 각 함수는 req->stime 기준 지연(ns)을 반환 */
    switch (req->cmd.opcode) {
    case NVME_CMD_FLUSH:
        /* FLUSH: DRAM write buffer 전체를 destage(budget 0 = 전부) */
        /*
         * Commit what the write buffer is holding. The buffer is the only state
         * a bbssd namespace keeps back from the media, so a flush is a full
         * write-back of it, and the cost of programming those pages is charged
         * here -- which is where a real device charges it too.
         *
         * Handled here rather than in the I/O layer because the buffer, the
         * mapping and the line management belong to this thread; draining them
         * from the poller would race everything else the FTL is doing. The
         * request reaches the FTL ring like any other, so no extra plumbing is
         * needed to get here.
         *
         * What this guarantees: a completion is only posted after the FTL has
         * processed the request, so every write the host has seen complete is
         * already in the buffer by the time a later flush is dequeued, and the
         * write-back covers it. That is what the NVM Command Set asks of Flush.
         * It says nothing about writes still in flight on another poller ring,
         * which the host has not been told are complete and which the FTL
         * thread may not have reached yet -- those are unordered against this
         * flush, as they are against each other.
         *
         * Drained whether or not the cache is currently advertised or enabled:
         * if pages are being held back, a flush has to write them out. Doing
         * this only for an enabled cache would let the host disable the cache
         * and then be told a flush succeeded with pages still buffered.
         */
        lat = ssd_buffer_destage(ssd, 0, req->stime);
        if (n->power_loss && ssd->write_buffer_cnt) {
            req->status = NVME_CAP_EXCEEDED | NVME_DNR;
        }
        break;
    case NVME_CMD_WRITE_ZEROES:
        /* FDP keeps its own reclaim-unit accounting and needs its own path */
        if (ssd->fdp_enabled) {
            lat = ssd_write_zeroes_fdp_style(n, req);
        } else {
            lat = ssd_write_zeroes(ssd, req);
        }
        break;
    case NVME_CMD_WRITE:
        /* WRITE: FDP면 RUH 배치 경로(ftl-fdp.c), 아니면 ssd_write() */
        if (ssd->fdp_enabled) {
            lat = nvme_do_write_fdp(n, req, req->slba, req->nlb);
        } else {
            lat = ssd_write(ssd, req);
        }
        /* ④ 오류 주입: err_write_fail_ppm에서 구한 period마다 WRITE_FAULT */
        /* optional write fault on every Nth write, on the same principle */
        if (ssd->err_write_fail_period &&
            (++ssd->err_write_counter % ssd->err_write_fail_period) == 0) {
            req->status = NVME_WRITE_FAULT;
            ssd->err_write_injected++;
        }
        break;
    case NVME_CMD_COPY:
        lat = ssd_copy(n, ssd, req);
        break;
    case NVME_CMD_READ:
        /* READ: LPN별 NAND read 지연 중 최댓값(channel/LUN 병렬) */
        lat = ssd_read(ssd, req);
        /* ④ 오류 주입(read): period마다 UNRECOVERED_READ */
        /*
         * Optional media error on every Nth read. The counter makes a run
         * reproducible, unlike a random draw.
         */
        if (ssd->err_read_unc_period &&
            (++ssd->err_read_counter % ssd->err_read_unc_period) == 0) {
            req->status = NVME_UNRECOVERED_READ | NVME_DNR;
            ssd->err_read_injected++;
        }
        break;
    case NVME_CMD_IO_MGMT_SEND:
        /* I/O Management Send(FDP RUH Update): 지정 RUH를 새 RU로 교체 */
        if (ssd->fdp_enabled && req->fdp_pids) {
            ssd_fdp_update_ruhs(n, req);
        }
        break;
    case NVME_CMD_DSM:
        /* DSM deallocate(TRIM): 매핑 해제·page invalid. FDP 경로는 지연 0 */
        if (ssd->fdp_enabled) {
            ssd_trim_fdp_style(n, req, req->slba, req->nlb);
            lat = 0;
        } else if (req->dsm_ranges && req->dsm_nr_ranges > 0) {
            lat = ssd_trim(ssd, req);
        }
        break;
    default:
        /* 그 밖의 opcode는 FTL 비용 없음(lat = 0) */
        ;
    }

    /*
     * ⑤ 임계값을 넘었으면 victim 하나만 GC(force=false). GC 시간은 lat에
     *    더하지 않는다. 대신 enable_gc_delay면 GC의 NAND 시간이 LUN의
     *    next_lun_avail_time에 쌓여 이후 요청들이 늦어진다.
     */
    /* background GC */
    if (ssd->fdp_enabled) {
        int16_t rgidx;
        /*
         * Trigger a single GC pass on a reclaim group over threshold; the GC
         * policy itself lives in do_gc_fdp_style(), not here.
         */
        if (!((rgidx = should_gc_fdp_style(ssd)) < 0)) {
            if (ssd->nrg == 1) {
                do_gc_fdp_style(ssd, 0, 0, false);
            } else {
                do_gc_fdp_style(ssd, rgidx, 0, false);
            }
        }
    } else if (should_gc(ssd)) {
        do_gc(ssd, false);
    }

    /* ⑥ 잠금 해제 후 지연 반환 */
    if (n->streams) {
        qemu_mutex_unlock(&n->streams_lock);
    }
    return lat;
}

/*
 * ssd_free - release everything ssd_init() built, in the reverse order
 *
 * Every mode that allocates an ns->ssd runs it through ssd_init(), so one
 * teardown serves bbssd, CSD and KV. The struct itself belongs to whichever
 * mode allocated it, and that mode frees it after calling this.
 */
/*
 * 호출: bb_ns_exit()(bb.c), csd_exit()(csd/csd.c),
 *       kvssd_free_ssd()(kvssd/kvssd-ftl.c), femu_cxl_stop()
 *       (cxlssd/cxlssd.c), cxl_nvme_detach()(cxlssd/qemu-adapter.c)
 * 흐름: ① FDP 구조 → ② mapping scheme 상태 → ③ read cache/CMT
 *       → ④ line·write buffer → ⑤ maptbl/rmap → ⑥ media
 *       → ⑦ channel 트리
 */
void ssd_free(struct ssd *ssd)
{
    struct ssdparams *spp;
    int i;

    if (!ssd) {
        return;
    }
    spp = &ssd->sp;

    /*
     * Guarded on the pointers rather than fdp_enabled: KV builds the same
     * structures without setting the flag, and freeing nothing is cheap.
     */
    femu_fdp_ssd_free(ssd);

    if (ssd->mapping && ssd->mapping->exit) {
        ssd->mapping->exit(ssd);
    }

    rcache_destroy(ssd);
    cmt_destroy(ssd);
    ssd_free_lines(ssd);
    ssd_free_write_buffer(ssd);

    g_free(ssd->rmap);
    ssd->rmap = NULL;
    g_free(ssd->maptbl);
    ssd->maptbl = NULL;

    nand_media_destroy(&ssd->media);

    if (ssd->ch) {
        for (i = 0; i < spp->nchs; i++) {
            ssd_free_ch(&ssd->ch[i], spp);
        }
        g_free(ssd->ch);
        ssd->ch = NULL;
    }
}
