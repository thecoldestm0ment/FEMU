# 14. L2P mapping scheme — page · dftl · hybrid(BAST) · fast(FAST)

기준: `study` 브랜치, 한글 주석을 넣은 뒤의 줄 번호. 링크의 `#L` 번호는 이 문서를 쓴 시점 기준이다.

| 파일 | 내용 |
|---|---|
| [`ftl-map.c`](../../ftl-map.c) | maptbl/rmap 초기화, scheme registry, page·dftl 공용 콜백 |
| [`ftl-map-cmt.c`](../../ftl-map-cmt.c) | dftl의 CMT(Cached Mapping Table) 비용 모델 |
| [`ftl-map-hybrid.c`](../../ftl-map-hybrid.c) | BAST식 log-block mapping (merge 비용 모델) |
| [`ftl-map-fast.c`](../../ftl-map-fast.c) | FAST식 log-block mapping (SW log + 공유 RW log) |
| [`ftl-exp.c`](../../ftl-exp.c), [`ftl-query.c`](../../ftl-query.c) | 보조 기능: 잔존 데이터 실험 로그, query-femu 상태 수집 |

## 1. 개요

- scheme은 vtable [`struct femu_mapping_ops`](../../ftl.h) 하나다. datapath는 L2P 결정이 필요한 곳에서 `ssd->mapping->콜백(...)`을 부르므로 scheme을 바꿔도 datapath 코드는 그대로다.
- **핵심 설계: 어떤 scheme이든 데이터의 정답은 flat `maptbl`(LPN→PPA)과 `rmap`(PPA→LPN)이다.** scheme이 정하는 것은 두 가지뿐이다.
  1. 새 page를 어느 allocation class(= write pointer)에 쓸지: `DATA`(`ssd->wp`), `HOT`(`ssd->hot_wp`), `LOG`(`ssd->log_wp`)
  2. 비용을 얼마나 더 charge할지: CMT miss(dftl), log merge(hybrid/fast)
- 선택 방법: device property `mapping` → `bb_params.mapping_scheme` ([`femu.c`](../../../femu.c)의 `DEFINE_PROP_STRING("mapping", ...)`).
  예: `-device femu,...,femu_mode=1,mapping=hybrid`
- 이름 → ops: [`femu_mapping_scheme_lookup()`](../../ftl-map.c#L207). 모르는 이름은 [`femu_mapping_scheme_known()`](../../ftl-map.c#L235)이 false를 돌려 `bb_check_geometry()`가 `unknown mapping` 오류로 realize를 거부한다.
- 제약: FDP를 켜면 `page`만 허용([`bb_init_fdp()`](../../bb.c)). streams는 page/dftl만 허용([`bb_check_capacity()`](../../bb.c)).

## 2. `femu_mapping_ops` 콜백 표

| 콜백 / 플래그 | 언제·누가 부르나 | page | dftl | hybrid(BAST) | fast(FAST) |
|---|---|---|---|---|---|
| `uses_cmt` | `ssd_init()` → `cmt_init()` | false | **true** | false | false |
| `uses_log_class` | `bb_gc_forced_lines()`, `bb_check_capacity()` | false | false | **true** | **true** |
| `reclaim_per_page` | merge 검사 시점 결정 | false | false | **true** | false |
| `init` / `exit` | `ssd_init()` / `ssd_free()` | NULL | NULL | log pool 할당/해제 | SW·RW 상태 할당/해제 |
| `translate` | [`ssd_read()`](../../ftl-datapath.c), `ssd_program_lpn()`의 EXP 로그, `nvme_streams_test()` | maptbl 조회 | 같음 (CMT 비용은 호출자가 따로) | 같음 | 같음 |
| `prepare_write` | [`ssd_program_lpn()`](../../ftl-datapath.c) | DATA (hot_cold_sep=on이면 덮어쓰기→HOT) | page와 같음 | 항상 LOG | 항상 LOG |
| `commit_write` | `ssd_program_lpn()` | 옛 PPA invalid + maptbl/rmap 갱신 | 같음 | 같음 + log의 `used`/`seq` 기록 | 같음 + SW run 또는 RW pool 기록 |
| `needs_reclaim` | hybrid: `ssd_program_lpn()`이 page마다 / fast: `ssd_write()`·`ssd_buffer_destage()` 끝 | NULL | NULL | 꽉 찬 log 있음 or pool 16개 소진 | RW 예산 소진 or SW 꽉 참 or dirty 목록 거의 꽉 참 |
| `reclaim(budget)` | 위와 같은 곳, budget=1 | NULL | NULL | log 1개 merge | SW switch + RW random merge(≤2 LBN) |
| `gc_relocate_commit` | [`gc_write_page()`](../../ftl-line-gc.c) | maptbl/rmap만 새 PPA로 | 같음 | 같음 (log 모델 불변) | 같음 |
| `trim` | [`ssd_deallocate_lpns()`](../../ftl-datapath.c) | NULL → datapath가 flat 무효화 | NULL | flat unmap (log `used`는 그대로) | flat unmap |

구현 위치: page/dftl 표 [`femu_mapping_schemes[]`](../../ftl-map.c#L173), [`femu_mapping_hybrid_ops`](../../ftl-map-hybrid.c#L427), [`femu_mapping_fast_ops`](../../ftl-map-fast.c#L392).

> `map_write_plan.may_need_reclaim`은 hybrid/fast가 세우지만 현재 datapath는 `target_class`만 읽는다. merge 여부는 `needs_reclaim()`이 정한다.

## 3. scheme 비교

| 항목 | page | dftl | hybrid (BAST) | fast (FAST) |
|---|---|---|---|---|
| 모델이 흉내 내는 매핑 | page 단위, L2P 전체가 DRAM에 | page 단위, L2P는 NAND의 TP에, 일부만 DRAM 캐시 | block 매핑 + LBN당 log block 1개 | block 매핑 + SW log 1개 + 공유 RW log |
| 실제 L2P | flat maptbl | flat maptbl | flat maptbl | flat maptbl |
| 추가 메타데이터 | 없음 | `ssd->cmt` (slot, hash, CLOCK hand) | `map_priv`: `logs[16]`, `lbn_to_log[]`, `data_blocks[]` | `map_priv`: `sw_lbn/sw_used`, `rw_used_pages`, dirty LBN 목록 |
| host write 위치 | `ssd->wp` (+`hot_wp`) | page와 같음 | `ssd->log_wp` | `ssd->log_wp` |
| 추가 비용 | 없음 | CMT miss: TP read (+dirty TP write-back) | merge (switch / full) | merge (switch / random) |
| 장점 | 단순, 기준선 | 작은 DRAM에서의 L2P 지연 관찰 | 블록 매핑 계열의 merge 비용 관찰 | log를 공유해 merge 빈도를 줄임 |
| 단점(모델에서 보이는 것) | — | miss마다 NAND read | random write에서 full merge가 잦음 | merge 한 번이 여러 LBN이라 큼 |
| 관련 옵션 | `mapping=page`(기본), `hot_cold_sep` | `mapping=dftl`, `mapping_cache_mb`(0이면 4MB) | `mapping=hybrid`, `debug_ftl` | `mapping=fast`, `debug_ftl` |
| 통계 노출 | WAF 카운터 | `cmt.hits/misses`는 내부 카운터 | FEMU stats log(0xC0)의 `hybrid_*` ([`ssd_hybrid_stats()`](../../ftl-map-hybrid.c#L449)) | `debug_ftl` 로그만 |

## 4. page 매핑과 `hot_cold_sep`

page와 dftl은 같은 콜백을 쓴다. 쓰기 한 번은 이렇게 흐른다.

```
ssd_write() / ssd_buffer_destage()
  ├─ (dftl만) cmt_touch(lpn, is_write=true)        ← CMT 비용
  └─ ssd_program_lpn()
       ├─ prepare_write()      → target_class (DATA / HOT / LOG)
       ├─ get_new_page_class() → 그 class의 write pointer에서 새 PPA
       ├─ commit_write()       → 옛 PPA invalid, maptbl/rmap = 새 PPA
       ├─ mark_page_valid(), write pointer 전진, NAND program 지연
       └─ (hybrid만) needs_reclaim()? → reclaim(1)
  └─ (fast만) 요청/batch 끝에 needs_reclaim()? → reclaim(1)
```

[`femu_map_page_prepare_write()`](../../ftl-map.c#L97)의 class 결정:

| 상황 | `hot_cold_sep=off` (기본) | `hot_cold_sep=on` |
|---|---|---|
| 처음 쓰는 LPN | DATA (`ssd->wp`) | DATA |
| 이미 mapped인 LPN을 덮어쓰기 | DATA | **HOT** (`ssd->hot_wp`) |
| TRIM으로 unmap된 뒤 다시 쓰기 | DATA | DATA (mapping이 없으니 첫 write 취급) |
| GC가 옮기는 valid page | `ssd->wp` | `ssd->wp` (`gc_write_page()`는 `get_new_page()` 사용) |
| write buffer 경유 | destage 시점에 판정 | 같음 |

- 왜 분리하나: 덮어쓴 LPN은 또 덮어쓸 가능성이 높다. 같이 invalid될 page를 한 line에 모으면 GC victim의 valid page가 줄어 복사량(WAF)이 준다.
- `hot_wp`의 line은 처음 쓸 때 잡히고, free line이 없으면 `ssd->wp`로 fallback한다([`ftl-line-gc.c`](../../ftl-line-gc.c)의 `ssd_write_pointer_for_class()`).
- `bb_check_capacity()`는 hot write pointer용 line 1개를 더 예약한다. FDP와 함께 쓰면 거부된다.
- hybrid/fast의 `prepare_write`는 `hot_cold_sep`를 읽지 않는다. 항상 LOG다.
- commit: [`femu_map_page_commit_write()`](../../ftl-map.c#L133)는 옛 PPA를 invalid로만 표시한다. 실제 erase는 line GC가 한다. GC 쪽 갱신은 [`femu_map_page_gc_relocate_commit()`](../../ftl-map.c#L158)이 맡고, 옛 page 정리는 `gc_write_page()`가 직접 한다.

## 5. DFTL CMT 비용 모델 ([`cmt_touch()`](../../ftl-map-cmt.c#L108))

- 실제 L2P 값은 flat maptbl에서 읽는다. CMT는 "어느 translation page(TP)가 DRAM에 있나"만 추적하는 **timing-only** 모델이다. 그래서 dftl은 page와 데이터 동작이 같고 지연만 다르다.
- 크기([`cmt_init()`](../../ftl-map-cmt.c#L182)): `lpn_per_tp = page 크기 / 8B`, `capacity = cache_mb × 1MiB / page 크기`.
  예: 4KB page → TP당 512 LPN, 4MB CMT → 1024 TP → L2P 2GB 분량.
- `mapping=dftl`이고 `mapping_cache_mb=0`이면 4MB. page에서는 `mapping_cache_mb`를 줘도 CMT가 꺼진다(`ssd_init()`).
- TP의 home LUN = `tp_id % tt_luns`. 주소의 blk/pg는 0인 자리표시라 LUN 점유 시간만 계산되고, `MAP_IO` read는 block의 read 횟수를 올리지 않는다.

| 상황 | 동작 | 추가 지연 |
|---|---|---|
| hit (read) | `ref=1` | 0 |
| hit (write) | `ref=1`, `dirty=1` | 0 |
| miss, victim이 빈 slot 또는 clean | victim 버림 → 새 TP를 home LUN에서 read | TP read |
| miss, victim이 dirty | victim TP를 **자기** home LUN에 program → 끝난 뒤 새 TP read | write-back + TP read (직렬) |
| write miss | 위와 같고, 설치한 TP가 곧바로 dirty | 위와 같음 |

- victim 선택: [`cmt_clock_evict()`](../../ftl-map-cmt.c#L75)의 CLOCK(second chance). 조회는 linear probing hash이고, miss마다 [`cmt_rehash()`](../../ftl-map-cmt.c#L53)로 표 전체를 다시 만든다.
- charge하는 곳: `ssd_read()`(LPN마다, write buffer hit은 제외), `ssd_write()`의 직접 경로, `ssd_buffer_destage()`. GC relocation, TRIM, FDP 경로, Write Zeroes에서는 charge하지 않는다.

## 6. BAST(hybrid)와 FAST(fast)의 merge

공통: 모든 host write(첫 write 포함)가 LOG class로 간다. merge가 page를 복사할 때는 DATA class(`ssd->wp`)로 옮기고 `gc_write_pages++`(WAF)를 센다. 데이터 읽기는 언제나 flat maptbl이다.

| merge 종류 | 일반 개념 | hybrid 조건 | fast 조건 | 이 코드의 비용 |
|---|---|---|---|---|
| switch | log가 offset 순서대로 꽉 참 → log가 곧 새 data block | `seq == pgs_per_blk` (모든 program이 순서대로) | SW log가 `sw_used >= pgs_per_blk` | erase 1회 charge, 복사 0 |
| partial | 순서대로 덜 찬 log에 나머지 page를 채워 넣음 | **미구현** → full merge로 처리 | **미구현** (SW log는 꽉 찰 때까지 유지) | — |
| full / random | data block + log의 valid page를 새 block으로 모음 | switch 불가인 victim | RW 예산 소진 등 → dirty LBN을 뒤에서 최대 2개 | valid page 수 × (read + program). erase는 따로 charge 안 함 |

| 항목 | hybrid ([`femu_map_hybrid_reclaim()`](../../ftl-map-hybrid.c#L274)) | fast ([`femu_map_fast_reclaim()`](../../ftl-map-fast.c#L275)) |
|---|---|---|
| log 구성 | LBN당 log 1개(1:1), pool 16개 | SW log 1개 + 모든 LBN 공유 RW pool(16 block 분량 page) |
| 기록 | [`commit_write`](../../ftl-map-hybrid.c#L188): `used++`, 순서가 맞으면 `seq++` | [`commit_write`](../../ftl-map-fast.c#L146): SW run 연장/시작, 아니면 RW pool(`rw_used_pages++`, LBN dirty) |
| 검사 시점 | program마다 (`reclaim_per_page=true`) | write 요청/destage batch 끝 1번 |
| victim | `used`가 가장 큰 log 1개 (동률이면 앞 slot) | SW(꽉 찼으면) + dirty 목록 뒤 2개 LBN |
| switch erase 위치 | `data_blocks[lbn]` (따로 보관한 옛 data block anchor) | LBN offset 0이 지금 가리키는 PPA |
| 카운터 | `switch_merges`, `full_merges`, `merge_erases` → stats log | `switch_merges`, `full_merges`(pass 단위) → `debug_ftl` 로그만 |

- hybrid에서 trim·덮어쓰기는 `used`를 줄이지 않는다. NAND는 덮어쓸 수 없으므로 program 1회 = slot 1개다.
- full merge 경로는 erase를 직접 charge하지 않는다. 옛 page는 invalid로 남고, 나중에 line GC가 그 line을 회수할 때 erase된다.
- 복사할 곳이 없으면(`get_new_page_class()`가 실패하면) 옛 copy를 invalid로 만들기 전에 멈춘다. 데이터를 잃지 않는다.

## 7. 보조 기능 한 줄 요약

| 파일 | 한 줄 요약 | 진입점 |
|---|---|---|
| [`ftl-exp.c`](../../ftl-exp.c) | 환경변수(`FEMU_EXP_LOG`, `FEMU_SECRET`, `FEMU_DUMP_LPN`)가 있을 때만, marker를 담은 LPN/block의 write·overwrite·GC 이동·erase·TRIM을 stderr에 `[EXP]`로 기록. FTL 동작은 바꾸지 않음 | [`exp_load_cfg()`](../../ftl-exp.c#L40), [`femu_dbg_lpn_has_secret()`](../../ftl-exp.c#L165) |
| [`ftl-query.c`](../../ftl-query.c) | QMP `query-femu`용. FTL thread에서 geometry, WAF 카운터, line 수, line별 vpc/ipc/상태/erase 범위를 복사 | [`ssd_query_collect()`](../../ftl-query.c#L96) |

line 상태는 [`ssd_query_line_states()`](../../ftl-query.c#L36)가 정한다. 뒤에 칠한 것이 우선이다: UNLISTED → FREE/FULL/VICTIM(목록) → OPEN(write pointer) → RECLAIMING.

## 8. 코드를 읽다 보인 것 (관찰)

- `struct femu_map_fast`의 영어 주석은 통계가 "SMART vendor area"로 노출된다고 하지만, `ssd_hybrid_stats()`는 hybrid일 때만 더한다. fast 카운터는 `debug_ftl` 로그로만 보인다.
- `femu_map_hybrid_reclaim()` 위 영어 주석은 full merge가 "plus the erases"를 charge한다고 하지만, 코드에서 erase를 charge하는 곳은 switch 경로뿐이다.
- fast의 SW log는 꽉 차서 switch merge될 때만 비워지고, 이 검사는 요청 끝에 한 번 한다. 그래서 한 write 요청이 여러 LBN을 처음부터 순서대로 쓰면 SW log가 첫 LBN에 잡힌 채라 두 번째 LBN부터는 RW pool로 간다. 끝나지 않은 SW run도 SW log를 계속 붙잡는다.
- dftl의 `cmt_touch()`는 `ssd_read()`에서 `translate`와 read cache 조회보다 먼저 불린다. unmapped LPN이나 read cache hit도 CMT 비용을 낸다.

## 9. 공부 포인트 · 확인 질문

1. 네 scheme 모두 `translate`가 maptbl 조회인데, 왜 hybrid/fast에서도 읽기가 정확한가?
2. dftl을 켜면 데이터 배치(WAF)는 page와 같고 지연만 달라진다. 코드의 어떤 구조 때문인가?
3. `hot_cold_sep=on`에서 TRIM한 LPN을 다시 쓰면 어느 class로 가는가? GC가 옮긴 hot page는?
4. hybrid, `pgs_per_blk=64`, 빈 장치에서 LPN 0..63을 순서대로 한 번씩 쓰면 어떤 merge가 일어나는가? 0을 두 번 쓴 뒤 1..63을 쓰면?
5. hybrid의 `reclaim_per_page=true`가 없고 fast처럼 요청 끝에서만 검사한다면 어떤 문제가 생기는가?
6. fast의 random merge 한 pass가 최대 2개 LBN만 옮기는 이유와 대가는?
7. CMT miss에서 dirty victim의 write-back을 새 TP read보다 먼저 직렬로 계산하는 이유는?

<details><summary>짧은 답</summary>

1. 모든 scheme의 `commit_write`/`gc_relocate_commit`/merge가 flat maptbl/rmap을 최신 PPA로 갱신한다. log 구조는 비용 회계용이다.
2. 콜백이 page와 같고, CMT는 [`cmt_touch()`](../../ftl-map-cmt.c#L108)에서 지연만 더하기 때문이다(`uses_cmt`만 다름).
3. TRIM 후엔 unmapped라 DATA. GC 복사는 `get_new_page()` → `ssd->wp`(DATA)다.
4. 첫 경우: `seq == 64` → switch merge(erase 1회). 둘째: 두 번째 write의 offset(0)이 `used`(1)와 달라 그 뒤로는 seq가 늘지 않는다. 64번째 program(LPN 62)에서 log가 꽉 차 full merge(valid인 LPN 0..62, 63 page 복사)가 일어나고, LPN 63은 새 log에 들어간다.
5. log가 꽉 찬 뒤에도 같은 요청의 다음 page가 그 log에 들어가 `used`가 `pgs_per_blk`를 넘는다(물리 block 하나보다 큰 log). 영어 주석의 "merge before another page can consume log space"가 이 이유다.
6. 모든 dirty LBN을 한 번에 옮기면 page 복사 수천 번으로 지연이 튄다. 대신 merge가 여러 요청에 나뉘어 일어난다.
7. 같은 slot을 비워야 새 TP를 넣을 수 있다. 둘 다 `stime`부터 세면 비용이 겹쳐 과소 계산된다(코드 주석 참고).

</details>
