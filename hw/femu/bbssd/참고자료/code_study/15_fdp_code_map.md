# 15. FDP 코드 지도 — `ftl-fdp.c` ↔ 01~08 노트

기준: 브랜치 `study`(코드는 `master`와 동일, 한국어 주석만 추가), 2026-10-07 확인. 01~08 노트는 단일 `ftl.c`(커밋 `e2d5413ff`)를 기준으로 썼으므로 줄 번호와 일부 동작이 지금과 다르다. 아래 링크의 줄 번호는 한국어 주석을 넣은 뒤의 [`ftl-fdp.c`](../../ftl-fdp.c) 기준이다.

## 1. 개요

- `ftl-fdp.c`는 FDP 모드(`ssd->fdp_enabled`)의 FTL 전부를 담는다: RG/RU/RUH 관리, placement-aware 쓰기, RU 단위 GC, trim과 Write Zeroes, RUH Update, 초기화와 해제.
- 파일 머리 주석의 "verbatim"은 파일 분리 커밋(`8dd16648f`)에만 해당한다. `e2d5413ff` 이후 분리 전후로 FDP 수정 커밋이 많아서 동작이 꽤 바뀌었다(§4).
- 다른 파일과의 경계:

| 파일 | FDP와의 관계 |
|---|---|
| [`ftl.c`](../../ftl.c) | `ssd_init()`이 초기화 3단계 호출, `bb_ftl_process_req()`가 opcode별 FDP 함수 호출과 background GC, `ssd_copy()`, `ssd_free()` |
| [`ftl-internal.h`](../../ftl-internal.h) | `ssd_wp_step()`/`ssd_wp_reset()`, `should_gc_fdp_style()`/`should_gc_high_fdp_style()`, FDP 함수 선언 |
| [`ftl.h`](../../ftl.h) | `FemuReclaimGroup`, `FemuReclaimUnit`, `FemuRuHandle`, `ru_mgmt`, GC 전략 enum |
| [`ftl-line-gc.c`](../../ftl-line-gc.c) | `get_next_free_line()`, `gc_read_page()`, `ssd_erase_lun_block()`, `ftl_gc_rand()` |
| [`ftl-geom.c`](../../ftl-geom.c) | `bb_fdp_forced_units()`, `gc_strategy` 허용값(0/1/2/4) 검사 |
| [`ftl-datapath.c`](../../ftl-datapath.c) | `bbssd_deallocate_all()` → `ssd_deallocate_fdp_all()` |
| [`nvme-io.c`](../../../nvme-io.c) | DTYPE/DSPEC 추출, `nvme_parse_pid()`, `nvme_fdp_record_event()`, `nvme_fdp_note_ru_left()`, RUH Update 명령 검사 |
| [`femu.c`](../../../femu.c) | RUH 유형(PI/II) 설정, namespace의 PH 배정, `femu_ftl_thread()` |

## 2. 함수 표

### 2.1 RU 할당과 교체

| 함수 | 역할 | 호출하는 곳 | 관련 노트 |
|---|---|---|---|
| victim pqueue 콜백 ([`victim_ru_cmp_pri()`](../../ftl-fdp.c#L54) 등 10개) | heap 우선순위(vpc, my_cb)와 위치(pos, ruh_pos) | `pqueue_init()`에 전달(init 함수들) | 05 §6~§8 |
| [`get_next_free_ru()`](../../ftl-fdp.c#L129) | RG free list 맨 앞 RU를 꺼냄 | `fdp_get_new_ru()` | 01 §3, 04 §6 |
| [`fdp_set_ru_write_pointer()`](../../ftl-fdp.c#L153) | RU write pointer를 첫 line 처음으로 | `fdp_get_new_ru()`, `mark_ru_free()` | 04 §6 |
| [`fdp_get_new_ru()`](../../ftl-fdp.c#L174) | free RU를 RUH에 배정·초기화 | `fdp_advance_ru_pointer()`, `fdp_gc_frontier()`, `ssd_stream_write_lpns()`, `ssd_fdp_update_ruhs()`, `femu_fdp_ssd_init_ru_handles()`, `ssd_trim_fdp_reset_all()` | 01 §6, 04 §6 |
| [`fdp_drop_active_ru()`](../../ftl-fdp.c#L216) | 활성 RU를 비우고 NVMe 쪽은 `no_ru`로 | `gc_write_page_fdp_style()`, `ssd_stream_write_lpns()` | (새 함수) 04 §9 |
| [`fdp_get_new_page()`](../../ftl-fdp.c#L230) | write pointer 좌표 → PPA | `gc_write_page_fdp_style()`, `ssd_stream_write_lpns()` | 04 §2 |
| [`fdp_retire_ru()`](../../ftl-fdp.c#L262) | 다 쓴 RU를 full 또는 victim으로 분류 | `fdp_advance_ru_pointer()`, `ssd_fdp_update_ruhs()` | (새 함수) 04 §5 |
| [`fdp_advance_ru_pointer()`](../../ftl-fdp.c#L326) | pointer 전진, RU 끝이면 교체 | `ssd_stream_write_lpns()`, `gc_write_page_fdp_style()` | 04 §4~§9 |

### 2.2 valid / invalid

| 함수 | 역할 | 호출하는 곳 | 관련 노트 |
|---|---|---|---|
| [`mark_page_valid_fdp()`](../../ftl-fdp.c#L394) | page valid, line/RU vpc와 RUH live page 증가 | `ssd_stream_write_lpns()`, `gc_write_page_fdp_style()` | 05 §2 |
| [`mark_page_invalid_fdp()`](../../ftl-fdp.c#L442) | 옛 page invalid, RU 통계와 victim heap 갱신 | `ssd_stream_write_lpns()`, `gc_write_page_fdp_style()`, `ssd_deallocate_fdp_lpns()` | 05 §3~§9 |

### 2.3 GC

| 함수 | 역할 | 호출하는 곳 | 관련 노트 |
|---|---|---|---|
| [`fdp_gc_frontier()`](../../ftl-fdp.c#L601) | GC 목적 RU(PI: `gc_ru`, II: `curr_ru`) 확보 | `do_gc_fdp_style()`, `gc_write_page_fdp_style()` | 06 §5~§6 |
| [`select_victim_ru_from_ruh()`](../../ftl-fdp.c#L635) | per-RUH heap에서 pop | `select_victim_ru()` | 06 §3.2 |
| [`select_victim_ru()`](../../ftl-fdp.c#L678) | 전략별 victim 선택, force 보류 | `do_gc_fdp_style()` | 06 §3~§4, 05 §8 |
| [`gc_write_page_fdp_style()`](../../ftl-fdp.c#L893) | valid page 하나 relocation | `clean_one_block_fdp_style()` | 06 §5, §7~§8 |
| [`clean_one_block_fdp_style()`](../../ftl-fdp.c#L1019) | block 하나의 valid page 전부 이동 | `do_gc_fdp_style()` | 06 §7 |
| [`fdp_count_gc_writes()`](../../ftl-fdp.c#L1053) | GC bytes → mbmw | `do_gc_fdp_style()` | 07 §3 |
| [`mark_ru_free()`](../../ftl-fdp.c#L1073) | erase된 RU를 초기화해 free list로 | `do_gc_fdp_style()`, `ssd_trim_fdp_reset_all()` | 06 §9, 07 §5 |
| [`reinsert_victim_ru()`](../../ftl-fdp.c#L1126) | 실패한 victim을 heap에 되돌림 | `do_gc_fdp_style()` | (새 함수) 06 §6 |
| [`do_gc_fdp_style()`](../../ftl-fdp.c#L1162) | GC 1회(RU 하나 회수) | `bb_ftl_process_req()`, `fdp_gc_until_clear()`, `ssd_fdp_update_ruhs()` | 06 전체, 07 §3, §7 |
| [`fdp_gc_until_clear()`](../../ftl-fdp.c#L1323) | foreground GC 반복 | `ssd_stream_write_lpns()` | (새 함수) 06 §2.1 |

### 2.4 쓰기와 RUH Update

| 함수 | 역할 | 호출하는 곳 | 관련 노트 |
|---|---|---|---|
| [`ssd_stream_write_lpns()`](../../ftl-fdp.c#L1359) | PID → RG/RUH, LPN마다 `curr_ru`에 기록 | `ssd_stream_write()`, `ssd_write_zeroes_fdp_style()` | 03 전체, 04 §3, §7 |
| [`fdp_ru_unwritten()`](../../ftl-fdp.c#L1579) | RU가 비어 있는지 | `ssd_fdp_update_ruhs()` | (새 함수) |
| [`ssd_fdp_update_ruhs()`](../../ftl-fdp.c#L1605) | RUH Update: RUH를 새 RU로 이동 | `bb_ftl_process_req()` | (새 함수) 07 §6.2 |
| [`ssd_stream_write()`](../../ftl-fdp.c#L1652) | slba/nlb → LPN 범위 | `nvme_do_write_fdp()` | 03 §9 |
| [`fdp_count_write()`](../../ftl-fdp.c#L1672) | 실제 쓴 bytes → hbmw/mbmw | `nvme_do_write_fdp()`, `ssd_write_zeroes_fdp_style()` | 07 §2 |
| [`nvme_do_write_fdp()`](../../ftl-fdp.c#L1703) | FDP 쓰기 진입점 | `bb_ftl_process_req()`(Write), `ssd_copy()`(Copy) | 03 §4, 07 §2 |

### 2.5 초기화와 해제

| 함수 | 역할 | 호출하는 곳 | 관련 노트 |
|---|---|---|---|
| [`femu_fdp_init_ru_mgmt()`](../../ftl-fdp.c#L1735) | RG의 ru_mgmt 기본값, heap 2개 | `femu_fdp_ssd_init_reclaim_group()` | 02 §4.1 |
| [`femu_fdp_init_ssd_reclaim_unit()`](../../ftl-fdp.c#L1777) | RU 하나 생성, line 연결 | `femu_fdp_ssd_init_reclaim_group()` | 02 §5 |
| [`femu_fdp_ssd_init_reclaim_group()`](../../ftl-fdp.c#L1824) | RG·RU pool·GC 임계값 | `ssd_init()` | 02 §4~§6 |
| [`femu_fdp_ssd_init_ru_handles()`](../../ftl-fdp.c#L1903) | PH마다 RUH, 첫 RU 배정 | `ssd_init()` | 02 §7, 01 §5 |
| [`ssd_init_fdp_params()`](../../ftl-fdp.c#L1995) | `lines_per_ru = 1`, RU 총수 | `ssd_init()` | 02 §3 |
| [`femu_fdp_ssd_free()`](../../ftl-fdp.c#L2355) | FDP 메모리 해제 | `ssd_free()` | (새 함수) |

### 2.6 trim, deallocate, Write Zeroes

| 함수 | 역할 | 호출하는 곳 | 관련 노트 |
|---|---|---|---|
| [`ssd_reset_maptbl()`](../../ftl-fdp.c#L2031) | maptbl/rmap 전체 초기화 | `ssd_trim_fdp_reset_all()` | 07 §10 |
| [`ssd_deallocate_fdp_lpns()`](../../ftl-fdp.c#L2053) | LPN 범위 unmap(FDP 통계 유지) | `ssd_trim_fdp_range()`, `ssd_write_zeroes_fdp_style()`, `ssd_deallocate_fdp_all()` | (새 함수) |
| [`ssd_deallocate_fdp_all()`](../../ftl-fdp.c#L2082) | 전체 unmap | `bbssd_deallocate_all()`(Sanitize, Format) | (새 함수) |
| [`ssd_trim_fdp_range()`](../../ftl-fdp.c#L2105) | DSM range만 unmap(기본) | `ssd_trim_fdp_style()` | (새 함수) 07 §9 |
| [`ssd_write_zeroes_fdp_style()`](../../ftl-fdp.c#L2149) | DEAC면 unmap, 아니면 0을 program | `bb_ftl_process_req()` | (새 함수) |
| [`ssd_trim_fdp_reset_all()`](../../ftl-fdp.c#L2199) | 장치 전체 리셋(opt-in) | `ssd_trim_fdp_style()` | 07 §9~§10 |
| [`ssd_trim_fdp_style()`](../../ftl-fdp.c#L2329) | DSM 진입점, range 목록 해제 | `bb_ftl_process_req()` | 07 §9 |

## 3. 주요 경로 요약

**쓰기 (Write, Copy, DEAC 없는 Write Zeroes)** — 03, 04, 05 노트
1. `femu_ftl_thread()` → `bb_ftl_process_req()` → [`nvme_do_write_fdp()`](../../ftl-fdp.c#L1703) → `ssd_stream_write()` → [`ssd_stream_write_lpns()`](../../ftl-fdp.c#L1359). Copy는 `ssd_copy()`가 `nvme_do_write_fdp()`를, Write Zeroes는 `ssd_write_zeroes_fdp_style()`가 `ssd_stream_write_lpns()`를 부른다.
2. DTYPE/DSPEC → `nvme_parse_pid()` → (ph, rgid). DTYPE이 placement가 아니거나 PID가 틀리면 ph = 0, rgid = 0(틀린 PID면 filter에 따라 INVALID_PID event). `ruhid = ns->fdp.phs[ph]`.
3. `curr_ru`가 없으면 free RU를 바로 시도(없으면 `NVME_CAP_EXCEEDED`), LBA 범위 초과면 `NVME_LBA_RANGE`.
4. `fdp_gc_until_clear()`: 명령 시작과 매 page 전에 foreground GC.
5. LPN마다: 옛 PPA `mark_page_invalid_fdp()` → `fdp_get_new_page()` → maptbl/rmap → `mark_page_valid_fdp()` → RUAMW 감소 → `fdp_advance_ru_pointer()`(RU 끝이면 `fdp_retire_ru()` + 새 RU) → NAND 지연.
6. `fdp_count_write()`: 실제로 쓴 `req->xfer_bytes`만 hbmw/mbmw에 더한다.

**GC** — 06 노트
1. background: `bb_ftl_process_req()` 끝에서 `should_gc_fdp_style()`이 RG를 고르면 `do_gc_fdp_style(rg, 0, false)` 1회.
2. foreground: `fdp_gc_until_clear()`가 어느 RG든 free RU 수가 `gc_thres_rus_high` 이하인 동안 `do_gc_fdp_style(rgid, ruhid, true)` 반복.
3. [`do_gc_fdp_style()`](../../ftl-fdp.c#L1162): `select_victim_ru()` → 목적 RUH(PI: 자기, II: 마지막 RUH) → `fdp_gc_frontier()` → 모든 block의 valid page 이동(`clean_one_block_fdp_style()` → `gc_write_page_fdp_style()`, 실패 시 `reinsert_victim_ru()`) → `ssd_erase_lun_block()` → mbmw/mbe, IMPLICIT_RU_CHANGE event → `mark_ru_free()`.

**trim (DSM Deallocate)** — 07 노트
1. `bb_ftl_process_req()` → [`ssd_trim_fdp_style()`](../../ftl-fdp.c#L2329).
2. 기본: `ssd_trim_fdp_range()` → range마다 `ssd_deallocate_fdp_lpns()`(invalid → rmap/maptbl/read cache 정리). erase는 GC가 나중에.
3. `fdp_trim_erase_all=1`: `ssd_trim_fdp_reset_all()`(전체 erase, 모든 RU free, RUH마다 새 RU, 매핑·통계 0).
4. 끝으로 `req->dsm_ranges` 해제.

**초기화** — 02 노트
1. `ssd_init()` → [`ssd_init_fdp_params()`](../../ftl-fdp.c#L1995): RU = line 1개, RU 총수 = line 수(endgrp `nru`로 clamp).
2. [`femu_fdp_ssd_init_reclaim_group()`](../../ftl-fdp.c#L1824): RG마다 RU 1/nrg, `femu_fdp_init_ru_mgmt()`, RU마다 line 배정 → free list, 임계값, `gc_strategy`.
3. [`femu_fdp_ssd_init_ru_handles()`](../../ftl-fdp.c#L1903): PH마다 RUH, RG마다 첫 RU, `curr_ru = rus[0]`, PI면 per-RUH heap.

**RUH Update (I/O Management Send)** — 07 노트 §6.2
1. `nvme-io.c`가 PID를 모두 검사한 뒤 bbssd면 `req->fdp_pids`에 담아 FTL로 넘긴다.
2. [`ssd_fdp_update_ruhs()`](../../ftl-fdp.c#L1605): 빈 RU면 유지, 아니면 새 RU(없으면 force GC 1회) → `nvme_fdp_note_ru_left()` → `fdp_retire_ru(old)` → 새 RU를 활성으로.

## 4. 예전 노트 이후 바뀐 점

| 주제 | 예전(`e2d5413ff`, 노트 내용) | 지금 | 노트 |
|---|---|---|---|
| 파일 구조 | 모두 `ftl.c`, `ftl_thread()`가 opcode 분기 | `ftl-fdp.c`로 분리, 분기는 `ftl.c`의 `bb_ftl_process_req()`, thread는 `femu.c`의 `femu_ftl_thread()` | 전체 |
| background GC 위치 | 완료를 `to_poller`에 넣은 **뒤** | `bb_ftl_process_req()` 끝, 즉 완료를 넣기 **전** | 06 §2.2 |
| foreground GC | 명령당 최대 `max(nrg,1)`회 | `fdp_gc_until_clear()`: 압력이 풀릴 때까지, 매 page 전에도 | 06 §2.1 |
| LPN 계산 | `slba / secs_per_pg` | `ssd_lpn_range()`(LBA 크기, backend offset 반영). 범위 초과는 `NVME_LBA_RANGE` | 03, 04 §3 |
| 장치 full | assert, NULL 역참조 위험, `curr_ru`가 없으면 지연 0으로 성공 처리 | `NULL` 반환 → `NVME_CAP_EXCEEDED`, `fdp_drop_active_ru()`로 호스트에 공간 0 표시 | 04 §9 |
| `fdp_get_new_ru()` | NVMe 쪽 `ruh->rus[rg]`도 바꿈 | 바꾸지 않음(활성 RU가 될 때 호출자가 갱신). PI `gc_ru` 할당이 호스트 view를 바꾸지 않음 | 06 §7.1, 07 §6.1 |
| pointer 진행 | ch → lun → pg (pl = 0 고정) | `ssd_wp_step()`: ch → lun → pl → pg, multi-plane 지원 | 04 §4 |
| RU 분류 | advance 안에 직접, per-RUH 복제 없음(TODO) | `fdp_retire_ru()`로 분리, NOISY면 per-RUH heap에도 넣음 | 04 §5, 05 §6 |
| RUAMW | page마다 1 감소 | page가 덮는 LBA 개수만큼 감소 | 07 §6 |
| hbmw/mbmw | 쓰기 **전**에 `nlb * secsz` | 쓰기 **후** `req->xfer_bytes`(실제 쓴 만큼) | 07 §2 |
| WAF | hbmw/mbmw로만 해석 | `host_write_pages`, `nand_write_pages`, `gc_write_pages` 카운터 추가(`ssd_waf_x1000()`) | 07 §4 |
| GC 목적지 확보 | `check_gc_ruh_available()`: `curr_ru->rgidx` 참조, 실패 시 assert | `fdp_gc_frontier()`: victim의 RG 사용, 실패하면 victim을 되돌리고 -1 | 06 §6 |
| GC source 처리 | source를 invalid 처리 안 함, 끝에 live page 일괄 차감 | page마다 `mark_page_invalid_fdp()` + rmap 정리 | 06 §8, 08 §7.3 |
| GC 순서 | block마다 이동 직후 erase | 전부 이동한 뒤 `ssd_erase_lun_block()`(multi-plane erase). 중간 실패 시 erase 없음 | 06 §7, §9 |
| erase 횟수 | `mark_ru_free()`도 block free → 2번 셈 | `mark_ru_free()`는 erase 안 함 → 1번 | 07 §5 |
| CB victim | `victim_ru_cb` heap pop(삽입 때 점수) | 선택 때 heap 전체를 훑어 `(1-u)*age/u`, vpc 0 우선 | 05 §8, 06 §3 |
| RAND victim | `rand()` | `ftl_gc_rand()`(`gc_seed`로 재현 가능) | 06 §3 |
| GC 전략 값 | enum 전부 설정 가능 | 0/1/2/4만 허용(`ftl-geom.c`) | 06 §3, §3.2 |
| heap 갱신 | `pqueue_change_priority()`가 옛 우선순위로 방향 결정 | 부모와 비교해 방향 결정(`lib/pqueue.c`) | 05 §7 |
| 다중 RG | `curr_ru` = 마지막 RG의 RU, NOISY 교차 RG 계산 오류 | `curr_ru = rus[0]`, NOISY/put-back/free가 victim 자신의 RG 사용 | 01 §5.1, 02 §7.1, 06 §3.1 |
| FDP event | FTL `ftl_fdp_alloc_event()`, timestamp 0 | `nvme_fdp_record_event()`(lock + timestamp), GC event의 rgid = victim RG | 07 §7, §7.1 |
| RUH Update | NVMe 쪽 RUAMW 복원만 | bbssd는 FTL이 실제로 새 RU로 이동 | 07 §6.2 |
| trim | 항상 장치 전체 erase/리셋 | 기본은 range만 unmap, 전체 리셋은 `fdp_trim_erase_all=1` | 07 §9 |
| 전체 리셋 | cb heap, `gc_ru`, 카운터 미정리 | cb drain, `gc_ru` 반납, `ru_in_use_cnt`/live page 0 | 07 §10.1 |
| 새 명령 | Write, Read, DSM만 | FDP용 Write Zeroes, Copy, I/O Mgmt Send(RUH Update) 처리 | — |

## 5. 공부 포인트

1. **두 view**: FTL의 `ruh->curr_ru`/`rus[]`와 NVMe의 `ruh->ruh->rus[]`는 따로 갱신된다. 둘을 함께 바꾸는 곳(`ssd_stream_write_lpns()`, `gc_write_page_fdp_style()`의 II 경로, `fdp_gc_frontier()`의 II 경로, `ssd_fdp_update_ruhs()`, `fdp_drop_active_ru()`)을 비교해 보자.
2. **PI와 II의 GC 목적지**: [`fdp_gc_frontier()`](../../ftl-fdp.c#L601)와 [`gc_write_page_fdp_style()`](../../ftl-fdp.c#L893)의 ④를 읽고, PI에서 할당 실패 시 왜 호스트 `curr_ru`를 되살리는지 설명해 보자.
3. **옮긴 뒤 erase**: `do_gc_fdp_style()`이 이동과 erase를 두 루프로 나눈 이유(중간 실패 시 남은 page 보존, erase 중복 집계 방지)를 08 노트 S4 예시에 적용해 보자.
4. **utilization 분모 차이**(그대로 남음): `fdp_retire_ru()`는 `vpc / npages`, `mark_page_invalid_fdp()`는 `vpc / (vpc + ipc)`. CB 점수에 쓰이므로 RUH Update로 일찍 닫힌 RU에서 값이 어떻게 다른지 확인해 보자.
5. **다중 RG 한계**(그대로 남음): `curr_ru`는 RUH당 하나라 `rgid != 0` 쓰기에서는 `curr_ru == rus[rgid]` assert 조건이 성립하지 않고 데이터는 `curr_ru`로 간다. 또 `fdp_gc_until_clear()`는 압력이 있는 RG가 아니라 호출자 RG에서 GC한다. `gc_ru`도 RUH당 하나라 RG를 구분하지 않는다.
6. **확인 필요(전체 리셋, nrg > 1)**: `ssd_trim_fdp_reset_all()`은 `curr_ru`와 `gc_ru`만 반납한 뒤 `rus[rg]`를 새 RU로 덮어쓴다. RG 0이 아닌 쪽의 이전 RU가 어느 목록에도 없으면 free pool로 돌아오지 않을 수 있다.
7. **주석과 코드 차이**: `ssd_stream_write_lpns()`의 "Run foreground GC first" 주석과 달리 그 자리의 GC 루프는 주석 처리돼 있다. 실제 foreground GC는 그 뒤 `fdp_gc_until_clear()`에서 돈다.
8. **SOCIAL_WELFARE 경로**: `select_victim_ru_from_ruh()`는 남아 있지만 전략 13은 설정 검사에서 거부된다. NOISY(4)만 per-RUH heap을 쓴다.
9. **WAF 계산**: hbmw/mbmw(바이트, NVMe log)와 page 카운터(`ssd_waf_x1000()`)는 단위와 갱신 위치가 다르다. GC 한 번 뒤 두 값을 직접 계산해 맞춰 보자.
