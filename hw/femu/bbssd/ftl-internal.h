#ifndef __FEMU_BBSSD_FTL_INTERNAL_H
#define __FEMU_BBSSD_FTL_INTERNAL_H

#include "ftl.h"
/*
 * [ftl-internal.h] bbssd FTL 내부 전용 헤더 (bbssd의 .c들과
 * kvssd/kvssd-ftl.c가 포함한다). 내용:
 *   ① PPA → channel/LUN/plane/block/page/line 포인터 변환 inline
 *   ② write pointer 이동 규칙 ssd_wp_reset()/ssd_wp_step()
 *   ③ maptbl/rmap 접근 inline   ④ GC trigger 판정 should_gc*()
 *   ⑤ NVMe LBA → LPN 변환 ssd_lpn_range()
 *   ⑥ 여러 .c로 나뉜 FTL 함수 선언(geom/map/line-gc/datapath/fdp 등)
 * 자세한 설명: 참고자료/code_study/11_core_structures_and_dispatch.md
 */

/* geometry / validity / resource accessors -- hot-path inlines */
/* check_addr: 범위 검사. ftl_assert 기반이라 일반 빌드에서는 아무 일도 안 함 */
static inline void check_addr(int a, int max)
{
    ftl_assert(a >= 0 && a < max);
}

/* point a write pointer at the first page of @line */
/*
 * write pointer를 line의 첫 page(ch/lun/pl/pg = 0, blk = line->id)로 둔다.
 * 호출: ssd_init_write_pointer()·ssd_init_class_write_pointer()
 *       (ftl-line-gc.c), fdp_set_ru_write_pointer()(ftl-fdp.c),
 *       kv_ensure_write_pointer()(kvssd/kvssd-ftl.c)
 */
static inline void ssd_wp_reset(struct write_pointer *wpp, struct line *line)
{
    wpp->curline = line;
    wpp->ch = 0;
    wpp->lun = 0;
    wpp->pl = 0;
    wpp->pg = 0;
    wpp->blk = line->id;
}

/*
 * Move a write pointer to the next page of its line: channel first, then LUN,
 * then plane, then page. Returns true when the step wrapped past the line's
 * last page, leaving the pointer at page 0 for the caller to retire the line.
 */
/*
 * write pointer를 line 안의 다음 page로 한 칸 옮긴다.
 * 호출: ssd_advance_write_pointer_common()(ftl-line-gc.c),
 *       fdp_advance_ru_pointer()(ftl-fdp.c),
 *       kv_advance_write_pointer()(kvssd/kvssd-ftl.c)
 * 흐름: ① ch++ → 넘치면 ② lun++ → 넘치면 ③ pl++ → 넘치면 ④ pg++
 *       예) 8ch × 8LUN × 1pl: line의 page 0~63은 모두 pg=0이고
 *       64개 LUN에 하나씩 흩어진다.
 * 반환: true = line의 마지막 page를 지나 한 바퀴 돎(호출자가 line을 닫음)
 */
static inline bool ssd_wp_step(struct ssdparams *spp, struct write_pointer *wpp)
{
    /* ① channel 먼저: 연속 쓰기가 channel 병렬성을 최대로 쓰게 한다 */
    check_addr(wpp->ch, spp->nchs);
    if (++wpp->ch < spp->nchs) {
        return false;
    }
    wpp->ch = 0;
    /* ② 모든 channel을 돌았으면 다음 LUN */
    check_addr(wpp->lun, spp->luns_per_ch);
    if (++wpp->lun < spp->luns_per_ch) {
        return false;
    }
    wpp->lun = 0;
    /* ③ 모든 LUN을 돌았으면 다음 plane */
    check_addr(wpp->pl, spp->pls_per_lun);
    if (++wpp->pl < spp->pls_per_lun) {
        return false;
    }
    wpp->pl = 0;
    /* ④ 그다음 page 번호. 이것까지 넘치면 line 전체를 다 쓴 것 */
    check_addr(wpp->pg, spp->pgs_per_blk);
    if (++wpp->pg < spp->pgs_per_blk) {
        return false;
    }
    wpp->pg = 0;
    return true;
}

/*
 * PPA의 각 좌표가 geometry 범위 안인지 검사한다.
 * ssd_read()는 mapped_ppa()와 함께 써서 읽을 page가 있는지 판단한다.
 */
static inline bool valid_ppa(struct ssd *ssd, struct ppa *ppa)
{
    struct ssdparams *spp = &ssd->sp;
    int ch = ppa->g.ch;
    int lun = ppa->g.lun;
    int pl = ppa->g.pl;
    int blk = ppa->g.blk;
    int pg = ppa->g.pg;
    int sec = ppa->g.sec;

    if (ch >= 0 && ch < spp->nchs && lun >= 0 && lun < spp->luns_per_ch && pl >=
        0 && pl < spp->pls_per_lun && blk >= 0 && blk < spp->blks_per_pl && pg
        >= 0 && pg < spp->pgs_per_blk && sec >= 0 && sec < spp->secs_per_pg)
        return true;

    return false;
}

/* valid_lpn: LPN < tt_pgs인지. mapped_ppa: UNMAPPED_PPA(전부 1)가 아닌지 */
static inline bool valid_lpn(struct ssd *ssd, uint64_t lpn)
{
    return (lpn < ssd->sp.tt_pgs);
}

static inline bool mapped_ppa(struct ppa *ppa)
{
    return !(ppa->ppa == UNMAPPED_PPA);
}

/*
 * PPA → 물리 객체 포인터: ssd->ch[ch].lun[lun].pl[pl].blk[blk].pg[pg].
 * get_line()은 blk 번호만 본다(line id == block 번호, 모든 ch/LUN 공통).
 */
static inline struct ssd_channel *get_ch(struct ssd *ssd, struct ppa *ppa)
{
    return &(ssd->ch[ppa->g.ch]);
}

static inline struct nand_lun *get_lun(struct ssd *ssd, struct ppa *ppa)
{
    struct ssd_channel *ch = get_ch(ssd, ppa);
    return &(ch->lun[ppa->g.lun]);
}

static inline struct nand_plane *get_pl(struct ssd *ssd, struct ppa *ppa)
{
    struct nand_lun *lun = get_lun(ssd, ppa);
    return &(lun->pl[ppa->g.pl]);
}

static inline struct nand_block *get_blk(struct ssd *ssd, struct ppa *ppa)
{
    struct nand_plane *pl = get_pl(ssd, ppa);
    return &(pl->blk[ppa->g.blk]);
}

static inline struct line *get_line(struct ssd *ssd, struct ppa *ppa)
{
    return &(ssd->lm.lines[ppa->g.blk]);
}

static inline struct nand_page *get_pg(struct ssd *ssd, struct ppa *ppa)
{
    struct nand_block *blk = get_blk(ssd, ppa);
    return &(blk->pg[ppa->g.pg]);
}

/* mapping accessors (maptbl / rmap) -- hot-path inlines */
/* maptbl[lpn]: L2P(논리 page → 물리 page) 조회/설정 */
static inline struct ppa get_maptbl_ent(struct ssd *ssd, uint64_t lpn)
{
    return ssd->maptbl[lpn];
}

static inline void set_maptbl_ent(struct ssd *ssd, uint64_t lpn, struct ppa *ppa)
{
    ftl_assert(lpn < ssd->sp.tt_pgs);
    ssd->maptbl[lpn] = *ppa;
}

/*
 * PPA → 0..tt_pgs-1 평탄 page 번호(rmap의 index).
 * ch → lun → pl → blk → pg 순의 가중합이다(struct ppa 정수값과는 다름).
 */
static inline uint64_t ppa2pgidx(struct ssd *ssd, struct ppa *ppa)
{
    struct ssdparams *spp = &ssd->sp;
    uint64_t pgidx;

    pgidx = ppa->g.ch  * spp->pgs_per_ch  + \
            ppa->g.lun * spp->pgs_per_lun + \
            ppa->g.pl  * spp->pgs_per_pl  + \
            ppa->g.blk * spp->pgs_per_blk + \
            ppa->g.pg;

    ftl_assert(pgidx < spp->tt_pgs);

    return pgidx;
}

/* rmap: 물리 page → LPN 역매핑. GC가 옮길 page의 주인 LPN을 찾을 때 쓴다 */
static inline uint64_t get_rmap_ent(struct ssd *ssd, struct ppa *ppa)
{
    uint64_t pgidx = ppa2pgidx(ssd, ppa);

    return ssd->rmap[pgidx];
}

/* set rmap[page_no(ppa)] -> lpn */
static inline void set_rmap_ent(struct ssd *ssd, uint64_t lpn, struct ppa *ppa)
{
    uint64_t pgidx = ppa2pgidx(ssd, ppa);

    ssd->rmap[pgidx] = lpn;
}

/* geometry setup (hw/femu/bbssd/ftl-geom.c) */
void ssd_init_params(struct ssdparams *spp, FemuCtrl *n);
void ssd_init_ch(struct ssd_channel *ch, struct ssdparams *spp);

/* page mapping table init + mapping-scheme registry (hw/femu/bbssd/ftl-map.c) */
void ssd_init_maptbl(struct ssd *ssd);
void ssd_init_rmap(struct ssd *ssd);
const struct femu_mapping_ops *femu_mapping_scheme_lookup(const char *name);
bool femu_mapping_scheme_uses_cmt(const struct femu_mapping_ops *ops);

/* DFTL cached mapping table cost model (hw/femu/bbssd/ftl-map-cmt.c) */
void cmt_init(struct ssd *ssd, uint32_t cache_mb);
uint64_t cmt_touch(struct ssd *ssd, uint64_t lpn, uint64_t stime, bool is_write);

/*
 * Data-remanence experiment logging (debug; off unless FEMU_EXP_LOG / FEMU_DUMP_LPN
 * are set). Shared by the datapath, GC, and init; the code and state live in
 * hw/femu/bbssd/ftl-exp.c.
 */
extern bool exp_log_enabled;
extern uint8_t exp_watch_blk[];   /* block (line) id -> carries the marker? */
extern uint64_t exp_dump_lpn;
extern bool exp_dump_lpn_set;
void exp_load_cfg(void);
bool exp_lpn_watched(uint64_t lpn);
void exp_watch_lpn_add(uint64_t lpn);
void femu_dbg_dump_lpn(struct ssd *ssd, uint64_t lpn);
void femu_dbg_scan_secret(struct ssd *ssd, const char *tag);
bool femu_dbg_lpn_has_secret(struct ssd *ssd, uint64_t lpn);
#define EXP_LOG(fmt, ...) do { \
    if (exp_log_enabled) \
        fprintf(stderr, "[EXP] " fmt, ## __VA_ARGS__); \
} while (0)
#define PPA_FMT "ch=%u lun=%u pl=%u blk=%u pg=%u"
#define PPA_ARG(p) (unsigned)(p)->g.ch, (unsigned)(p)->g.lun, \
                   (unsigned)(p)->g.pl, (unsigned)(p)->g.blk, (unsigned)(p)->g.pg

/*
 * 정의: ftl-line-gc.c. wp에 line이 없으면 free line을 다시 잡아 보고,
 * 그래도 없으면 true → 호출자(ssd_write 등)가 쓰기를 CAP_EXCEEDED로 거부.
 */
bool ssd_out_of_lines(struct ssd *ssd);

/* GC trigger predicates (used by the datapath and GC) */
/*
 * 비FDP GC 판정 (free line 수 기준. 기본 256 line이면 각각 64개/12개 이하).
 * should_gc():      호출 bb_ftl_process_req() → 요청 후 background GC 1회
 * should_gc_high(): 호출 ssd_write()/ssd_write_zeroes()/ssd_gc_until_clear()
 *                   (ftl-datapath.c), do_read_reclaim()(ftl-line-gc.c)
 *                   → 쓰기 전에 강제 GC를 반복할지 판정
 */
static inline bool should_gc(struct ssd *ssd)
{
    return (ssd->lm.free_line_cnt <= ssd->sp.gc_thres_lines);
}

static inline bool should_gc_high(struct ssd *ssd)
{
    return (ssd->lm.free_line_cnt <= ssd->sp.gc_thres_lines_high);
}

/* FDP GC decision: returns rg index if GC needed, -1 otherwise */
/* 호출: bb_ftl_process_req(). RG를 0번부터 보며 처음 걸린 RG 번호를 반환 */
static inline int16_t should_gc_fdp_style(struct ssd *ssd)
{
    for (int i = 0; i < (int)ssd->nrg; i++) {
        if (ssd->rg[i].ru_mgmt->free_ru_cnt <=
            ssd->rg[i].ru_mgmt->gc_thres_rus) {
            return i;
        }
    }
    return -1;
}

/*
 * FDP 강제 GC 판정(gc_thres_rus_high 기준).
 * 호출: fdp_gc_until_clear(), ssd_stream_write_lpns()(ftl-fdp.c)
 */
static inline int should_gc_high_fdp_style(struct ssd *ssd)
{
    for (int i = 0; i < (int)ssd->nrg; i++) {
        if (ssd->rg[i].ru_mgmt->free_ru_cnt <=
            ssd->rg[i].ru_mgmt->gc_thres_rus_high) {
            return i;
        }
    }
    return -1;
}

/* line management + garbage collection (hw/femu/bbssd/ftl-line-gc.c) */
struct line *get_next_free_line(struct ssd *ssd);
void ssd_init_lines(struct ssd *ssd);
void ssd_init_write_pointer(struct ssd *ssd);
struct ppa ssd_stream_page(struct ssd *ssd, unsigned slot, uint64_t tag);
void ssd_stream_advance(struct ssd *ssd, unsigned slot);
void ssd_advance_write_pointer(struct ssd *ssd);
struct ppa get_new_page(struct ssd *ssd);
struct ppa get_new_page_class(struct ssd *ssd, int klass);
void ssd_advance_write_pointer_class(struct ssd *ssd, int klass);
void mark_page_invalid(struct ssd *ssd, struct ppa *ppa);
void mark_page_valid(struct ssd *ssd, struct ppa *ppa);
void mark_block_free(struct ssd *ssd, struct ppa *ppa);
uint64_t ssd_erase_lun_block(struct ssd *ssd, int ch, int lun, int blk,
                             bool charge, int64_t stime);
void mark_line_free(struct ssd *ssd, struct ppa *ppa);
void gc_read_page(struct ssd *ssd, struct ppa *ppa);
int do_gc(struct ssd *ssd, bool force);
int do_read_reclaim(struct ssd *ssd);
const struct femu_ftl_policy_ops *femu_ftl_policy_lookup(const char *name);
uint64_t ftl_gc_rand(struct ssd *ssd);

/* log-block mapping schemes (hw/femu/bbssd/ftl-map-hybrid.c) */
extern const struct femu_mapping_ops femu_mapping_hybrid_ops;
extern const struct femu_mapping_ops femu_mapping_fast_ops;

/*
 * Translate blocks [slba, slba + nlb) using the namespace's current format.
 * Fixed namespaces retain device-relative numbering; managed namespaces use
 * their private FTL address spaces.
 */
/*
 * NVMe LBA 범위 → FTL LPN 범위(end_lpn 포함). byte offset을 page 크기
 * (secsz × secs_per_pg, 기본 4KiB)로 나눈다. 예) 512B LBA: LBA 0~7 → LPN 0
 * 호출: ssd_read()/ssd_write()/ssd_write_zeroes()/ssd_trim()
 *       (ftl-datapath.c), ssd_stream_write()/ssd_trim_fdp_range()/
 *       ssd_write_zeroes_fdp_style()(ftl-fdp.c)
 */
static inline void ssd_lpn_range(struct ssd *ssd, NvmeRequest *req,
                                 uint64_t slba, uint64_t nlb,
                                 uint64_t *start_lpn, uint64_t *end_lpn)
{
    uint64_t pg = (uint64_t)ssd->sp.secsz * ssd->sp.secs_per_pg;
    uint8_t lbads = req->ns ? req->ns->lbaf.lbads : BDRV_SECTOR_BITS;
    uint64_t off = req->ns ? req->ns->backend_offset : 0;

    /* Managed namespaces index their private FTL independently of placement. */
    if (ssd->n->ns_mgmt &&
        (le16_to_cpu(ssd->n->id_ctrl.oacs) & NVME_OACS_NS_MGMT)) {
        off = 0;
    }
    off += slba << lbads;
    *start_lpn = off / pg;
    *end_lpn = (off + (nlb << lbads) - 1) / pg;
}

/* DRAM write buffer ordering (hw/femu/bbssd/ftl-datapath.c) */
int comp_buffer(const void *a, const void *b);

/* non-FDP host datapath (hw/femu/bbssd/ftl-datapath.c) */
/* bb_ftl_process_req()가 opcode별로 부르는 본체. 반환은 지연(ns) */
uint64_t ssd_read(struct ssd *ssd, NvmeRequest *req);
uint64_t ssd_write(struct ssd *ssd, NvmeRequest *req);
uint64_t ssd_trim(struct ssd *ssd, NvmeRequest *req);

/* optional DRAM read cache (hw/femu/bbssd/ftl-cache.c) */
void rcache_init(struct ssd *ssd, uint32_t read_cache_mb, uint32_t evict_policy);
uint64_t rcache_touch(struct ssd *ssd, uint64_t lpn);
void rcache_invalidate(struct ssd *ssd, uint64_t lpn);

/* Flexible Data Placement (hw/femu/bbssd/ftl-fdp.c) */
void ssd_init_fdp_params(struct ssdparams *spp, FemuCtrl *n);
void femu_fdp_ssd_init_reclaim_group(FemuCtrl *n, struct ssd *ssd);
void femu_fdp_ssd_init_ru_handles(FemuCtrl *n, struct ssd *ssd);
void femu_fdp_ssd_free(struct ssd *ssd);
void rcache_destroy(struct ssd *ssd);
void cmt_destroy(struct ssd *ssd);
void ssd_free_lines(struct ssd *ssd);
void ssd_free_ch(struct ssd_channel *ch, struct ssdparams *spp);
/* nvme_do_write_fdp() is declared in nvme.h (included via ftl.h) */
int do_gc_fdp_style(struct ssd *ssd, uint16_t rgid, uint16_t ruhid, bool force);
uint64_t ssd_write_zeroes_fdp_style(FemuCtrl *n, NvmeRequest *req);
void ssd_fdp_update_ruhs(FemuCtrl *n, NvmeRequest *req);
void ssd_trim_fdp_style(FemuCtrl *n, NvmeRequest *req, uint64_t slba,
                        uint32_t nlb);

#endif /* __FEMU_BBSSD_FTL_INTERNAL_H */
