/*
 * bbssd FTL page-mapping: forward (maptbl) and reverse (rmap) table setup, plus
 * the pluggable L2P mapping-scheme registry. The flat maptbl/rmap here stay the
 * source of truth; "page" (default) translates directly and "dftl" adds the
 * demand-cache cost model in ftl-map-cmt.c. Split out of ftl.c.
 */
/*
 * [한글 요약] L2P 표와 mapping scheme registry
 * - 역할: maptbl(LPN→PPA)과 rmap(물리 page→LPN)을 할당·초기화하고,
 *   device property "mapping" 이름으로 L2P scheme(vtable)을 고른다.
 * - scheme: "page"(기본)·"dftl"은 이 파일의 표에, "hybrid"·"fast"는
 *   각자 파일(ftl-map-hybrid.c, ftl-map-fast.c)의 ops를 끼워 넣는다.
 * - 어떤 scheme이든 데이터의 정답은 flat maptbl/rmap 하나뿐이다.
 * - 주요 진입 함수: ssd_init_maptbl(), ssd_init_rmap(),
 *   femu_mapping_scheme_lookup(), femu_mapping_scheme_known()
 * - 의존: ftl-internal.h의 get/set_maptbl_ent()·set_rmap_ent(),
 *   ftl-line-gc.c의 mark_page_invalid()
 * - 자세한 설명: 참고자료/code_study/14_mapping_schemes.md
 */
#include "qemu/osdep.h"
#include "ftl.h"
#include "ftl-internal.h"

/*
 * maptbl(LPN→PPA)을 tt_pgs칸 할당하고 전부 UNMAPPED_PPA로 채운다.
 * 호출: ssd_init() (ftl.c)
 */
void ssd_init_maptbl(struct ssd *ssd)
{
    struct ssdparams *spp = &ssd->sp;

    ssd->maptbl = g_malloc0(sizeof(struct ppa) * spp->tt_pgs);
    for (int i = 0; i < spp->tt_pgs; i++) {
        ssd->maptbl[i].ppa = UNMAPPED_PPA;
    }
}

/*
 * rmap(물리 page 번호→LPN)을 tt_pgs칸 할당하고 전부 INVALID_LPN으로 채운다.
 * GC는 victim의 물리 page에서 출발하므로 rmap으로 LPN을 되찾아 maptbl을 고친다.
 * 호출: ssd_init() (ftl.c)
 */
void ssd_init_rmap(struct ssd *ssd)
{
    struct ssdparams *spp = &ssd->sp;

    ssd->rmap = g_malloc0(sizeof(uint64_t) * spp->tt_pgs);
    for (int i = 0; i < spp->tt_pgs; i++) {
        ssd->rmap[i] = INVALID_LPN;
    }
}

/*
 * Pluggable L2P mapping schemes. A scheme is a vtable (femu_mapping_ops),
 * name-selected by the mapping device property like the gc_policy registry. The
 * interface is a translate (read) plus a prepare/commit write split and a
 * GC-relocation commit. "page" is the original full-DRAM mapping (bit-identical);
 * "dftl" reuses the same flat updates but sets uses_cmt so the datapath charges
 * the demand-cache cost from ftl-map-cmt.c.
 */
/*
 * page와 dftl은 아래 4개 콜백을 그대로 공유하고 uses_cmt 플래그만 다르다.
 * dftl의 CMT 비용은 콜백 안이 아니라 datapath(ssd_read(), ssd_write(),
 * ssd_buffer_destage())가 cmt_touch()로 따로 더한다(ftl-map-cmt.c).
 */

/*
 * page/dftl translate: the flat L2P lookup. Any dftl cache cost is charged at the
 * call site via cmt_touch, which keeps this path bit-identical.
 */
/*
 * 읽기용 L2P 조회: maptbl[lpn]을 그대로 돌려준다(없으면 UNMAPPED_PPA).
 * 호출: ssd->mapping->translate (ssd_read(), ssd_program_lpn()의 EXP 로그,
 *       nvme_streams_test())
 */
static struct ppa femu_map_page_translate(struct ssd *ssd, uint64_t lpn)
{
    return get_maptbl_ent(ssd, lpn);
}

/*
 * page/dftl prepare_write: the data class, or hot/cold separated when asked.
 * An already-mapped page is an overwrite, the cheapest signal that it will be
 * overwritten again; keeping those in their own lines means a line fills with
 * pages that die together, so GC victims are mostly invalid rather than mixed.
 */
/*
 * 새 page를 어느 allocation class(= 어느 write pointer)에 쓸지 정한다.
 * 호출: ssd->mapping->prepare_write (ssd_program_lpn())
 * 흐름: ① hot_cold_sep=off → 항상 FEMU_MAP_CLASS_DATA (ssd->wp)
 *       ② on → 이미 mapped(덮어쓰기)면 FEMU_MAP_CLASS_HOT (ssd->hot_wp),
 *          처음 쓰는 LPN이면 DATA
 * 왜: 한 번 덮어쓴 LPN은 또 덮어쓸 가능성이 높다. 같이 invalid될 page끼리
 *     한 line에 모으면 GC victim의 valid page(복사량)가 줄어든다.
 * 반환: target_class만 채운 plan (may_need_reclaim=false)
 */
static struct map_write_plan femu_map_page_prepare_write(struct ssd *ssd,
                                                         uint64_t lpn, int io_type)
{
    struct ppa old;

    (void)io_type;

    /* ① 분리 off: page scheme 원래 동작 그대로 DATA 하나 */
    if (!ssd->sp.hot_cold_sep) {
        return (struct map_write_plan){ .target_class = FEMU_MAP_CLASS_DATA };
    }

    /*
     * ② 덮어쓰기 판정 = 현재 mapping 존재 여부. TRIM으로 unmap된 LPN은 다시
     * DATA로 간다. GC가 옮기는 page는 hot_wp가 아니라 ssd->wp로 간다.
     */
    old = get_maptbl_ent(ssd, lpn);

    return (struct map_write_plan){
        .target_class = mapped_ppa(&old) ? FEMU_MAP_CLASS_HOT
                                         : FEMU_MAP_CLASS_DATA,
    };
}

/*
 * page/dftl commit_write: invalidate the old mapping, then point lpn at new_ppa.
 * This is the verbatim flat-L2P update from the original ssd_write.
 */
/*
 * 새 PPA를 L2P에 확정하고 이전 PPA를 무효화한다(out-of-place update).
 * 호출: ssd->mapping->commit_write (ssd_program_lpn())
 * 흐름: ① 이전 PPA가 있으면 page invalid(block/line의 vpc·ipc 갱신)
 *          + rmap[old] = INVALID_LPN
 *       ② maptbl[lpn] = new_ppa, rmap[new_ppa] = lpn
 * 참고: new_ppa를 valid로 표시(mark_page_valid())하는 것은 호출자 몫이다.
 */
static void femu_map_page_commit_write(struct ssd *ssd, uint64_t lpn,
                                       struct ppa *new_ppa)
{
    struct ppa old = get_maptbl_ent(ssd, lpn);

    /* ① 옛 copy는 invalid로만 표시한다. 실제 회수(erase)는 line GC가 한다 */
    if (mapped_ppa(&old)) {
        mark_page_invalid(ssd, &old);
        set_rmap_ent(ssd, INVALID_LPN, &old);
    }
    /* ② 정방향·역방향 표를 모두 새 PPA로 */
    set_maptbl_ent(ssd, lpn, new_ppa);
    set_rmap_ent(ssd, lpn, new_ppa);
}

/*
 * page/dftl gc_relocate_commit: point lpn at the GC-relocated ppa. This is the
 * verbatim maptbl/rmap update from the original gc_write_page.
 */
/*
 * GC가 valid page를 옮긴 뒤 maptbl/rmap을 새 PPA로 바꾼다.
 * 호출: ssd->mapping->gc_relocate_commit (gc_write_page(), ftl-line-gc.c)
 * 참고: old_ppa의 invalid 처리와 rmap 정리는 gc_write_page()가 이 호출
 *       직후에 직접 하므로 여기서는 old_ppa를 쓰지 않는다.
 */
static void femu_map_page_gc_relocate_commit(struct ssd *ssd, uint64_t lpn,
                                             struct ppa *old_ppa,
                                             struct ppa *new_ppa)
{
    (void)old_ppa;
    set_maptbl_ent(ssd, lpn, new_ppa);
    set_rmap_ent(ssd, lpn, new_ppa);
}

/*
 * 내장 scheme 표. [0]번 "page"가 기본값(lookup 실패 시 fallback)이다.
 * init/exit/needs_reclaim/reclaim/trim이 NULL이라 private 상태와 merge가 없고,
 * trim은 datapath(ssd_deallocate_lpns())의 flat 무효화로 처리된다.
 * dftl은 uses_cmt=true라 ssd_init()이 cmt_init()으로 CMT를 켠다.
 */
static const struct femu_mapping_ops femu_mapping_schemes[] = {
    {
        .name          = "page",
        .uses_cmt      = false,
        .translate     = femu_map_page_translate,
        .prepare_write = femu_map_page_prepare_write,
        .commit_write  = femu_map_page_commit_write,
        .gc_relocate_commit = femu_map_page_gc_relocate_commit,
    },
    {
        .name          = "dftl",
        .uses_cmt      = true,
        .translate     = femu_map_page_translate,
        .prepare_write = femu_map_page_prepare_write,
        .commit_write  = femu_map_page_commit_write,
        .gc_relocate_commit = femu_map_page_gc_relocate_commit,
    },
};

/*
 * Schemes that keep their own state live in their own files and are spliced in
 * by name here, so the table above stays the plain page-mapping description.
 */
/* 자체 상태(ssd->map_priv)를 가진 log-block scheme: hybrid(BAST), fast(FAST) */
static const struct femu_mapping_ops * const femu_mapping_extra[] = {
    &femu_mapping_hybrid_ops,
    &femu_mapping_fast_ops,
};

/* Resolve a mapping name to its ops; default to page for NULL/unknown. */
/*
 * 이름 → ops 포인터. 내장 표 → extra 표 순으로 찾고 없으면 page를 준다.
 * 호출: ssd_init() (ftl.c), femu_mapping_name_uses_log_class()
 */
const struct femu_mapping_ops *femu_mapping_scheme_lookup(const char *name)
{
    if (name) {
        for (size_t i = 0; i < ARRAY_SIZE(femu_mapping_schemes); i++) {
            if (!strcmp(name, femu_mapping_schemes[i].name)) {
                return &femu_mapping_schemes[i];
            }
        }
        for (size_t i = 0; i < ARRAY_SIZE(femu_mapping_extra); i++) {
            if (!strcmp(name, femu_mapping_extra[i]->name)) {
                return femu_mapping_extra[i];
            }
        }
    }
    return &femu_mapping_schemes[0]; /* default: page-level */
}

/*
 * Is this a scheme the device has? The lookup falls back to page mapping for a
 * name it does not know, which is the right behaviour once realize has accepted
 * the configuration and the wrong behaviour before: a misspelt scheme ran a
 * different one without a word.
 */
/*
 * realize 단계 검증: 모르는 이름이면 false → "unknown mapping" 오류로 거부.
 * (NULL/빈 문자열은 기본값 page라 true)
 * 호출: bb_check_geometry() (ftl-geom.c)
 */
bool femu_mapping_scheme_known(const char *name)
{
    if (!name || !name[0]) {
        return true;                    /* unset means the default */
    }
    for (size_t i = 0; i < ARRAY_SIZE(femu_mapping_schemes); i++) {
        if (!strcmp(name, femu_mapping_schemes[i].name)) {
            return true;
        }
    }
    for (size_t i = 0; i < ARRAY_SIZE(femu_mapping_extra); i++) {
        if (!strcmp(name, femu_mapping_extra[i]->name)) {
            return true;
        }
    }

    return false;
}

/*
 * 이 scheme이 LOG class write pointer(ssd->log_wp)를 쓰는가(hybrid/fast).
 * true면 GC 강제 기준(최소 1 line)과 용량 예약(log_wp용 1 line)에 반영되고,
 * streams와 함께 쓰면 bb_check_capacity()가 거부한다.
 * 호출: bb_gc_forced_lines() (ftl-geom.c), bb_check_capacity() (bb.c)
 */
bool femu_mapping_name_uses_log_class(const char *name)
{
    return femu_mapping_scheme_lookup(name)->uses_log_class;
}

/* dftl처럼 CMT 비용 모델을 켜야 하는 scheme인가. 호출: ssd_init() (ftl.c) */
bool femu_mapping_scheme_uses_cmt(const struct femu_mapping_ops *ops)
{
    return ops && ops->uses_cmt;
}
