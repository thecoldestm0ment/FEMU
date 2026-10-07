# 13. line · GC · geometry · media 계층

> 대상: [`ftl-line-gc.c`](../../ftl-line-gc.c) · [`ftl-geom.c`](../../ftl-geom.c) · [`ftl-media.c`](../../ftl-media.c)
> 기준: `study` 브랜치(코드는 `master`와 같고 한국어 주석만 추가). 링크의 줄 번호는 주석을 넣은 뒤의 번호다.
> 이 문서의 기본값은 모두 [`femu.c`](../../../femu.c)의 `DEFINE_PROP_*` 표에서 확인했다.

## 1. 개요

| 파일 | 하는 일 | 주요 진입 함수 |
|---|---|---|
| ftl-geom.c | 속성 검증 → `ssdparams` 계산 → NAND 배열 할당 | [`bb_check_geometry()`](../../ftl-geom.c#L90), [`ssd_init_params()`](../../ftl-geom.c#L269), [`ssd_init_ch()`](../../ftl-geom.c#L413) |
| ftl-line-gc.c | line 리스트, write pointer, valid 집계, GC, read reclaim | [`get_new_page()`](../../ftl-line-gc.c#L493), [`mark_page_invalid()`](../../ftl-line-gc.c#L530), [`do_gc()`](../../ftl-line-gc.c#L1212), [`do_read_reclaim()`](../../ftl-line-gc.c#L1279) |
| ftl-media.c | PPA → `NandLoc` 변환, 지연 계산을 media 계층에 위임 | [`bb_nand_media_init()`](../../ftl-media.c#L111), [`ssd_advance_status()`](../../ftl-media.c#L235) |

초기화 순서([`ssd_init()`](../../ftl.c)). `bb_check_geometry()`는 그보다 먼저 [`bb.c`](../../bb.c)의 `bb_init()`에서 불린다.

```
ssd_init()                              (ftl.c)
 ├─ ssd_init_params()                   ftl-geom.c    ssdparams 계산
 ├─ ssd_init_ch() × nchs                ftl-geom.c    ch→LUN→plane→block→page 배열
 ├─ bb_nand_media_init()                ftl-media.c   NAND 지연 모델 설정
 ├─ ssd_init_maptbl() / ssd_init_rmap() ftl-map.c
 ├─ femu_ftl_policy_lookup()            ftl-line-gc.c gc_policy → ssd->policy
 ├─ ssd_init_lines()                    ftl-line-gc.c 모든 line → free list
 └─ ssd_init_write_pointer()            ftl-line-gc.c FDP가 꺼져 있을 때만
```

## 2. SSD geometry (`struct ssdparams`)

[`ssd_init_params()`](../../ftl-geom.c#L269)는 속성 7개를 복사한 뒤 [파생값](../../ftl-geom.c#L320)을 아래 단위에서 위 단위 순서로 곱해서 만든다.

| 필드 | 의미 | 계산식 | 기본값 |
|---|---|---|---|
| `secsz` | sector 크기(B) | 속성 | 512 |
| `secs_per_pg` | page당 sector | 속성 | 8 (page = 4 KiB) |
| `pgs_per_blk` | block당 page | 속성 | 256 (block = 1 MiB) |
| `blks_per_pl` | plane당 block | 속성 | 256 |
| `pls_per_lun` | LUN당 plane | 속성 | 1 |
| `luns_per_ch` | channel당 LUN | 속성 | 8 |
| `nchs` | channel 수 | 속성 | 8 |
| `secs_per_blk` / `secs_per_pl` | sector 수 | `secs_per_pg×pgs_per_blk` / `×blks_per_pl` | 2,048 / 524,288 |
| `secs_per_lun` / `secs_per_ch` | sector 수 | `×pls_per_lun` / `×luns_per_ch` | 524,288 / 4,194,304 |
| `tt_secs` | 전체 sector | `secs_per_ch×nchs` | 33,554,432 (16 GiB) |
| `pgs_per_pl` / `pgs_per_lun` | page 수 | `pgs_per_blk×blks_per_pl` / `×pls_per_lun` | 65,536 / 65,536 |
| `pgs_per_ch` / `tt_pgs` | page 수 | `×luns_per_ch` / `×nchs` | 524,288 / 4,194,304 |
| `blks_per_lun` / `blks_per_ch` / `tt_blks` | block 수 | `blks_per_pl×pls_per_lun` / `×luns_per_ch` / `×nchs` | 256 / 2,048 / 16,384 |
| `pls_per_ch` / `tt_pls` | plane 수 | `pls_per_lun×luns_per_ch` / `×nchs` | 8 / 64 |
| `tt_luns` | 전체 LUN | `luns_per_ch×nchs` | 64 |
| `blks_per_line` | line 하나의 block 수 | `tt_luns×pls_per_lun` ([L348](../../ftl-geom.c#L348)) | 64 |
| `pgs_per_line` / `secs_per_line` | line 크기 | `blks_per_line×pgs_per_blk` / `×secs_per_pg` | 16,384 (64 MiB) / 131,072 |
| `tt_lines` | line 수 | `blks_per_pl` | 256 |
| `gc_thres_lines` | background GC 수위 | `(int)((1 − gc_thres_pcent/100)×tt_lines)` ([L355](../../ftl-geom.c#L355)) | 64 (`gc_thres_pcent`=75) |
| `gc_thres_lines_high` | forced GC 수위 | [`bb_gc_forced_lines()`](../../ftl-geom.c#L33) | 12 (`gc_thres_pcent_high`=95) |

- 지연 기본값: `pg_rd_lat` 40,000 ns, `pg_wr_lat` 200,000 ns, `blk_er_lat` 2,000,000 ns, `ch_xfer_lat` 0.
- `gc_thres_pcent`와 `buffer_thres_pcent`는 `ssdparams` 안에서 0~1 비율(double)로 저장된다.
- [`bb_check_geometry()`](../../ftl-geom.c#L90)가 막는 것: 축 값 ≤ 0, PPA bit 폭 초과(ch 12, lun 7, pl 4, blk 16, pg 16, sec 8 bit), GC 수위가 [1,100] 밖이거나 high < low, 모르는 `mapping`/`gc_policy`/`cache_evict` 이름, `tt_secs > INT_MAX`(파생값이 `int`라서).
- 배열은 [`ssd_init_ch()`](../../ftl-geom.c#L413)가 `g_malloc0`로 만든다. page는 `PG_FREE`, LUN과 channel의 `next_*_avail_time`은 0에서 시작한다.

## 3. line 상태와 리스트

**line i = 모든 channel·LUN·plane에 있는 block i**(superblock)다. [`get_line()`](../../ftl-internal.h)은 `ppa->g.blk`만 보고 `lm.lines[blk]`을 돌려준다.

| 상태 | 자료구조 | 들어오는 때 | 나가는 때 |
|---|---|---|---|
| free | `free_line_list` (QTAILQ, FIFO) | 초기화, [`mark_line_free()`](../../ftl-line-gc.c#L1102), 한 page도 쓰지 않은 stream line 반납 | [`get_next_free_line()`](../../ftl-line-gc.c#L158) |
| open(쓰는 중) | 어느 list에도 없음 (`wp->curline`) | free에서 꺼낼 때 | 마지막 page를 쓴 뒤 [`ssd_advance_write_pointer_common()`](../../ftl-line-gc.c#L183) |
| full | `full_line_list` (QTAILQ) | 닫힐 때 `vpc == pgs_per_line` (ipc 0) | 첫 무효화 때 victim으로 이동, read reclaim이 꺼낼 때 |
| victim | `victim_line_pq` (min-heap) | 닫힐 때 ipc > 0, full에서 첫 무효화, stream line 봉인, GC 실패 후 requeue | 정책 함수의 pop/remove, read reclaim의 remove |
| GC 중 | 어느 list에도 없음 (`pos = 0`) | victim으로 뽑힌 직후 | 성공하면 free, 실패하면 [`requeue_line()`](../../ftl-line-gc.c#L1130) |

```
           get_next_free_line()           line을 다 씀
  [free] ─────────────────────▶ [open] ───────────────┬─▶ [full]   (ipc == 0)
     ▲                                                └─▶ [victim] (ipc  > 0)
     │                     첫 mark_page_invalid():  [full] ──▶ [victim]
     │  mark_line_free()                                         │ select_victim_*()
     └──────────────────────────────────── [GC 중] ◀─────────────┘
                                  (실패하면 requeue_line() → full 또는 victim)
```

꼭 기억할 불변식:
- `line->pos ≠ 0`이면 victim pq 안에 있다. heap은 1부터 번호를 매기고, 꺼낸 쪽에서 `pos = 0`으로 되돌린다.
- 닫힌 line은 `vpc + ipc = pgs_per_line`이다. 그래서 vpc가 최소인 line은 ipc가 최대인 line이다. 다 쓰지 못한 stream line은 남은 FREE page를 INVALID로 봉인해 같은 성질을 맞춘다([`ssd_stream_close_pointer()`](../../ftl-line-gc.c#L287)).
- victim pq의 key는 vpc다(fifo만 `close_seq`). heap 안의 line이 무효화되면 [`mark_page_invalid()`](../../ftl-line-gc.c#L530)가 `pqueue_change_priority()`로 heap을 다시 정렬한다. fifo는 key가 바뀌지 않으므로 vpc만 줄인다.
- [`victim_line_cmp_pri()`](../../ftl-line-gc.c#L39)는 `next > curr`이다. [`pqueue.c`](../../../lib/pqueue.c)는 부모의 key가 더 크면 둘을 바꾸므로 **min-heap**이 된다.

## 4. write pointer 전진 순서

`ssd_wp_step()`([`ftl-internal.h`](../../ftl-internal.h))은 **ch → lun → pl → pg** 순서로 올린다. 기본 geometry(8 ch, 8 LUN, 1 plane)에서는 다음과 같다.

```
쓰기 순서 (line = blk b 고정)
  #0      ch0 lun0 pl0 pg0
  #1      ch1 lun0 pl0 pg0        ← ch가 가장 빨리 돈다
  ...
  #8      ch0 lun1 pl0 pg0        ← ch가 한 바퀴 돌면 lun + 1
  #64     ch0 lun0 pl0 pg1        ← lun(그리고 pl)이 한 바퀴 돌면 pg + 1
  #16383  ch7 lun7 pl0 pg255      → 다음 step에서 wrap → line close, 새 line
```

연속으로 쓰는 page가 서로 다른 channel과 LUN에 흩어진다. 그래서 LUN마다 따로 계산되는 program 지연(§8)이 서로 겹쳐 병렬로 진행된다. line을 닫을 때는 `close_time`(cost-benefit의 age)과 `close_seq`(fifo의 key)를 기록한다.

| pointer | 필드 | 쓰는 곳 |
|---|---|---|
| 데이터 | `ssd->wp` | 기본 host write(DATA class), stream tag가 없는 GC relocation(read reclaim 포함) |
| HOT | `ssd->hot_wp` | `hot_cold_sep=on`일 때 덮어쓰기 LPN([`ftl-map.c`](../../ftl-map.c)의 prepare_write) |
| LOG | `ssd->log_wp` | log-block mapping(hybrid, fast) |
| stream | `ssd->stream_wp[slot]` | NVMe Streams write, 같은 tag를 가진 page의 GC |
| stream GC | `ssd->stream_gc_wp` | 이미 release된 stream의 GC ([`ssd_stream_gc_pointer()`](../../ftl-line-gc.c#L375)) |

HOT/LOG pointer는 처음 쓰일 때 line을 받는다. line을 받지 못하면 `ssd->wp`로 대신 쓴다([`ssd_write_pointer_for_class()`](../../ftl-line-gc.c#L415)). [`get_new_page()`](../../ftl-line-gc.c#L493)는 위치만 읽고 pointer를 움직이지 않는다. 쓸 line이 없으면 `INVALID_PPA`(= `UNMAPPED_PPA`)를 돌려준다.

## 5. GC 흐름

| 호출 위치 | 조건 | force | 반복 |
|---|---|---|---|
| [`bb_ftl_process_req()`](../../ftl.c), 요청을 처리한 뒤 | `should_gc()`: free ≤ `gc_thres_lines` (기본 64) | false | 1회 |
| [`ssd_write()`](../../ftl-datapath.c) 시작, `ssd_write_zeroes()`, `ssd_gc_until_clear()` | `should_gc_high()`: free ≤ `gc_thres_lines_high` (기본 12) | true | -1이 나올 때까지 |

```
do_gc(ssd, force)                                            ftl-line-gc.c
 ① victim = ssd->policy->select_victim_line(ssd, force)
      force=false이고 victim->ipc < pgs_per_line/8 이면 NULL(보류)
 ② streams 모드에서 vpc > 0인데 free line이 0개 → pq로 되돌리고 -1
 ③ reclaim_line(victim)
      ├ ch → lun → pl마다 clean_one_block()        (vpc가 0이 되면 중단)
      │    └ VALID page마다 gc_read_page() → gc_write_page()
      │        목적지 pointer 선택 → 새 PPA → gc_relocate_commit(maptbl/rmap)
      │        → old invalid, rmap 비움 → new valid → pointer 전진 → program 지연
      │    옮길 곳이 없으면 requeue_line() 후 false (erase하지 않음)
      ├ 모든 ch, lun: ssd_erase_lun_block()  (plane들을 묶어 multi-plane erase)
      └ mark_line_free() → free list 맨 뒤
 ④ streams 모드에서 free line 수가 늘지 않았으면 -1
```

- 링크: [`reclaim_line()`](../../ftl-line-gc.c#L1156), [`clean_one_block()`](../../ftl-line-gc.c#L1075), [`gc_write_page()`](../../ftl-line-gc.c#L726), [`ssd_erase_lun_block()`](../../ftl-line-gc.c#L632), [`mark_block_free()`](../../ftl-line-gc.c#L672).
- **GC 지연은 host 지연에 바로 더해지지 않는다.** GC read/program/erase는 `stime = 0`(지금)으로 `ssd_advance_status*()`를 부르고 반환값을 버린다. 그 결과는 해당 LUN의 `next_lun_avail_time`이 뒤로 밀리는 것뿐이고, 같은 LUN을 쓰는 다음 host I/O가 그만큼 기다린다. `enable_gc_delay=false`(FEMU flip 명령)이면 이 지연도 붙지 않는다.
- old page는 erase 전에 이미 invalid가 된다. relocation이 중간에 실패해 line을 되돌려 놓아도 vpc/ipc가 실제 상태와 맞게 하기 위해서다.
- GC로 옮긴 page는 `ssd->gc_write_pages`에 누적된다(WAF 계산용).

## 6. victim 정책 비교 (`gc_policy=` 속성)

| 정책 | 함수 | 고르는 기준 | 비용 | pq key | 특징 |
|---|---|---|---|---|---|
| `greedy` (기본, 빈 값) | [`select_victim_line`](../../ftl-line-gc.c#L814) | vpc 최소 = heap top | pop O(log n) | vpc | 복사량이 가장 적다. 표준 기준선 |
| `random` | [`select_victim_line_random`](../../ftl-line-gc.c#L845) | 무작위 하나 | O(log n) | vpc | ipc가 기준에 못 미치면 pq에 되돌린다 |
| `cost-benefit` | [`select_victim_line_cb`](../../ftl-line-gc.c#L878) | `age × ipc / vpc` 최대, vpc=0이면 무조건 선택 | 전체 heap 훑기 O(n) | vpc | 오래 닫혀 있던(cold) line을 우대. `age = now − close_time` |
| `fifo` | [`select_victim_line_fifo`](../../ftl-line-gc.c#L939) | `close_seq` 최소(가장 먼저 닫힌 line) | O(log n) | close_seq | vpc를 보지 않는다 |
| `d-choice` | [`select_victim_line_dchoice`](../../ftl-line-gc.c#L970) | 무작위 4개(중복 허용) 중 vpc 최소 | O(d) + remove | vpc | greedy의 근사 |

- 공통 보류 규칙: `force == false`이고 victim의 `ipc < pgs_per_line / 8`(기본 2,048 page)이면 NULL을 돌려준다.
- cost-benefit 식은 코드 주석의 `age·(1−u)/(2u)`(u = vpc/pgs_per_line)에서 상수를 지운 것이다. 나눗셈과 float을 피하려고 `age·ipc·best_vpc > best_age·best_ipc·vpc`처럼 128-bit 곱셈으로 비교한다.
- random과 d-choice는 [`ftl_gc_rand()`](../../ftl-line-gc.c#L1026)(splitmix64)를 쓴다. 이 함수는 `gc_seed` 속성(기본 0)으로 시작하므로 같은 workload면 같은 victim이 뽑힌다.
- 이름과 함수의 대응은 [`femu_ftl_policies[]`](../../ftl-line-gc.c#L1008) 표에 있다. fifo만 `by_close_order = true`이고, 그래서 [`ssd_init_lines()`](../../ftl-line-gc.c#L80)가 pq key를 `close_seq`로 정한다.

## 7. read reclaim · wear

| 트리거 | 예약 조건 (`ssd_read_check_wear()`, [`ftl-datapath.c`](../../ftl-datapath.c)) | 카운터 |
|---|---|---|
| read disturb | `read_reclaim_limit > 0` && block `read_cnt ≥ limit` | `read_reclaims` |
| retention | `retention_limit_sec > 0` && line이 닫힌 뒤 limit초 이상 지남 | `retention_refreshes` |

- read 경로는 line을 **예약만** 한다(`ssd->read_reclaim_line`, 한 번에 1개). 실제 재기록은 다음 `ssd_write()`의 [`do_read_reclaim()`](../../ftl-line-gc.c#L1279)이 한다. read가 line 하나 분량의 relocation 뒤에서 멈춰 기다리지 않게 하려는 것이다.
- 실행 조건은 `!should_gc_high()`이고 free line이 2개 이상일 때다. 조건이 안 맞아 보류해도 예약은 남아 다음 쓰기에서 다시 시도한다.
- line을 full list 또는 victim pq에서 꺼내고 `reclaiming = true`로 표시한 뒤 GC와 같은 [`reclaim_line()`](../../ftl-line-gc.c#L1156)을 탄다. 표시해 두는 이유는, 그러지 않으면 첫 무효화 때 `mark_page_invalid()`가 이 line을 victim pq에 넣어 버리기 때문이다.
- `read_cnt`: [`ssd_advance_status()`](../../ftl-media.c#L235)에서 NAND read마다 1 증가(MAP_IO 제외), `mark_block_free()`에서 0으로 초기화.
- `erase_cnt`: `mark_block_free()`에서 1 증가(`total_erases`도 함께). ECC 지연 모델의 `pe_cycles` 입력이 된다(§8).

## 8. media 지연 모델

[`ssd_advance_status()`](../../ftl-media.c#L235) → [`bb_decode_loc()`](../../ftl-media.c#L32) → `nand_media_op()`([`nand-media.c`](../../../nand/nand-media.c)) 순서로 흐른다. 지연 계산은 모두 media 계층에서 하고, bbssd는 상태를 넘기고 결과를 받는다.

| NandLoc 필드 | 출처 |
|---|---|
| `ch, lun, pl, blk, pg` | `ppa->g.*` 그대로 |
| `flash_type` | `nand_cell_type` (0 = flat timing) |
| `page_type` | cell type이 있으면 `get_page_type(ct, pg)`, 없으면 `pg % (cell_pages ?: 3)` |
| `pe_cycles` | `ecc_step_ns ≠ 0`일 때 block의 `erase_cnt` |
| `age_sec` | `ecc_step_ns`와 `ecc_retention_sec`가 둘 다 있을 때 `(stime − close_time) / 1e9` |

[`bb_nand_media_init()`](../../ftl-media.c#L111)이 만드는 설정:

| ssdparams | NandMediaConfig | 기본 |
|---|---|---|
| `pg_rd_lat` / `pg_wr_lat` / `blk_er_lat` | `timing.rd_ns` / `wr_ns` / `er_ns` | 40 us / 200 us / 2 ms |
| `pg_xfer_lat` (0이면 `ch_xfer_lat`) | `timing.page_xfer_ns` | 0 |
| `cmd_addr_lat`, `status_lat` | `cmd_addr_ns`, `status_ns` | 0 |
| `tplpbsy` / `tplrbsy` / `tplebsy` | multi-plane busy | 0 |
| `ecc_step_ns`, `ecc_retention_sec` | ECC tier (750 P/E당 1 tier, 최대 4) | 0 (끔) |
| `pe_suspend`, `tsusp_ns` | program/erase suspend | 0 (끔) |
| `pgtype_lat` + `cell_pages` | page-type별 program 배수 | 0 (끔) |
| `nand_cell_type` 1~4 | flash table timing (`use_flat_timing = false`) | 0 (flat) |

- **gate**: `NAND_GATE_LUN_ONLY`([L186](../../ftl-media.c#L186)). LUN 하나는 한 번에 array 동작 하나만 한다.
- **channel**: `cmd_addr`, `page_xfer`, `status` 중 하나라도 0이 아니면 `NAND_CH_STAGED`(channel bus 시간을 `next_ch_avail_time`에 누적)이고, 모두 0이면 `NAND_CH_OFF`다. 따라서 기본 설정에서는 channel 경합이 없다.
- **기본(flat) 계산식**: `done = max(stime, lun.next_lun_avail_time) + op 지연`, `next_lun_avail_time = done`, 반환값은 `done − stime`. read에는 `ecc_step_ns × min(pe/750 + age/ecc_retention_sec, 4)`가 더해진다(기본 0).
- **multi-plane**([`ssd_advance_status_multiplane()`](../../ftl-media.c#L290)): plane마다 cmd/addr는 bus에서 하나씩 차례로 보내고, array 동작은 모든 plane이 한 번에 한다. GC erase와 `mp_program`/`mp_read`가 이 경로를 쓴다.
- **런타임 변경**: FEMU flip 명령(`bb_flip_apply()`)이 `pg_*_lat`를 바꾸면 [`bb_nand_media_refresh_timing()`](../../ftl-media.c#L209)이 rd/wr/er 세 값만 다시 복사한다.

## 9. 공부 포인트 · 확인 질문

1. 기본 geometry에서 line 하나는 몇 MiB이고, line은 모두 몇 개인가? 그 수가 `blks_per_pl`과 같은 이유는?
2. `victim_line_cmp_pri()`가 `>`인데 왜 vpc가 **가장 작은** line이 heap top에 오는가?
3. full line의 page 하나가 덮어쓰기로 invalid가 되면 그 line은 어디로 가는가? read reclaim 중이면?
4. fifo 정책일 때 `mark_page_invalid()`는 왜 `pqueue_change_priority()`를 부르지 않는가?
5. GC가 host I/O를 느리게 만드는 실제 경로는? (힌트: `stime = 0`, `next_lun_avail_time`)
6. `gc_write_page()`는 왜 erase 전에 old page를 invalid로 만드는가?
7. read reclaim은 왜 read 경로가 아니라 write 경로에서 실행되는가?
8. `ch_xfer_lat`만 0이 아닌 값으로 주면 media 모델의 무엇이 바뀌는가?

### 관찰 메모 (코드는 고치지 않음)

- `nand_lun.gc_endtime`은 `ssd_erase_lun_block()`과 `gc_write_page()`(그리고 FDP GC)에서 값을 쓰기만 한다. hw/femu 전체에서 읽는 곳이 없다.
- `ssd_advance_status_multiplane()`은 read일 때 MAP_IO인지 확인하지 않고 `read_cnt`를 올린다(단일 경로는 MAP_IO를 뺀다). 다만 지금 호출자는 USER_IO(`ssd_mp_charge()`)와 GC erase뿐이라 실제 차이는 없다. `media_ops`는 multi-plane 명령을 1회로 센다.
- fifo에서 background GC는 heap top 하나만 본다. 가장 오래된 line의 ipc가 1/8에 못 미치면, 다른 line이 기준을 넘어도 이번 GC를 건너뛴다.
