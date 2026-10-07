# 11. 핵심 자료구조와 요청 dispatch — `ftl.h` · `ftl-internal.h` · `ftl.c` · `bb.c`

기준: 브랜치 `study`, 2026-10-07 소스. 링크의 줄 번호는 한글 주석을 넣은 뒤의 줄 번호다.
다른 파일(datapath, line-gc, fdp 등)은 줄 번호가 바뀔 수 있어 파일만 링크한다.

## 1. 개요

이 네 파일은 bbssd FTL의 **뼈대**다.

- `ftl.h`는 모든 자료구조를 정의한다. NAND 계층, 물리 주소 `struct ppa`, `line`, write pointer, `struct ssd`, FDP 구조체가 여기에 있다.
- `ftl-internal.h`는 FTL 내부에서만 쓰는 inline helper와 함수 선언을 모은다.
- `ftl.c`는 `struct ssd`를 만들고(`ssd_init`), 요청 하나를 opcode별 datapath로 보낸다(`bb_ftl_process_req`).
- `bb.c`는 bbssd 모드를 NVMe controller에 등록하는 glue다.

핵심 원리: **데이터 자체는 backend 메모리에 있다.** 기본 모드에서 `nvme_rw()`가 데이터를 그쪽으로 이미 복사했다. FTL은 mapping과 NAND 상태를 갱신하면서 **"이 요청이 언제 끝나는가"(지연, ns)** 만 계산한다.

## 2. 파일별 역할

| 파일 | 역할 | 주요 함수/내용 |
|---|---|---|
| [`ftl.h`](../../ftl.h) | 공용 자료구조와 외부 공개 API | [`struct ppa`](../../ftl.h#L107), [`struct ssdparams`](../../ftl.h#L203), [`struct line`](../../ftl.h#L311), [`struct write_pointer`](../../ftl.h#L338), [`struct ssd`](../../ftl.h#L611) |
| [`ftl-internal.h`](../../ftl-internal.h) | 내부 inline과 함수 선언 | [`ssd_wp_step()`](../../ftl-internal.h#L55), [`get_pg()`](../../ftl-internal.h#L149) 등 접근자, [`ppa2pgidx()`](../../ftl-internal.h#L172), [`should_gc()`](../../ftl-internal.h#L255), [`ssd_lpn_range()`](../../ftl-internal.h#L331) |
| [`ftl.c`](../../ftl.c) | FTL 생성·해제, 요청 dispatch, 통계 접근자 | [`ssd_init()`](../../ftl.c#L33), [`bb_ftl_process_req()`](../../ftl.c#L547), [`ssd_copy()`](../../ftl.c#L327), [`ssd_free()`](../../ftl.c#L713) |
| [`bb.c`](../../bb.c) | 모드 등록, 용량 검증, flip admin 명령 | [`nvme_register_bbssd()`](../../bb.c#L506), [`bb_init()`](../../bb.c#L234), [`bb_check_capacity()`](../../bb.c#L43), [`bb_flip()`](../../bb.c#L364) |

datapath 본체는 다른 파일에 있다. host 읽기·쓰기는 [`ftl-datapath.c`](../../ftl-datapath.c), line과 GC는 [`ftl-line-gc.c`](../../ftl-line-gc.c), FDP는 [`ftl-fdp.c`](../../ftl-fdp.c), NAND timing은 [`ftl-media.c`](../../ftl-media.c), 파라미터 계산은 [`ftl-geom.c`](../../ftl-geom.c)를 본다.

## 3. 핵심 자료구조

| 구조체 | 역할 | 주요 필드 |
|---|---|---|
| [`ssdparams`](../../ftl.h#L203) (`ssd->sp`) | geometry, 지연, GC 임계값 | `nchs`, `luns_per_ch`, `pls_per_lun`, `blks_per_pl`, `pgs_per_blk`, `pg_rd_lat`, `gc_thres_lines(_high)`, `tt_pgs`, `pgs_per_line`, `tt_lines` |
| [`ssd_channel`](../../ftl.h#L188) | channel 하나 | `lun[]`, `next_ch_avail_time`(staged bus 모델에서만 사용) |
| [`nand_lun`](../../ftl.h#L173) | LUN(die). NAND 명령이 직렬화되는 단위 | `pl[]`, **`next_lun_avail_time`**, `mp_prog`/`mp_rd`(multi-plane 대기열) |
| `nand_plane` / [`nand_block`](../../ftl.h#L131) / `nand_page` | 물리 계층의 아래쪽 | block: `pg[]`, `vpc`, `ipc`, `erase_cnt`, `read_cnt` / page: `status` |
| [`ppa`](../../ftl.h#L107) | 64bit 물리 page 주소 | `g.ch/lun/pl/blk/pg`, `ppa`(정수) |
| [`line`](../../ftl.h#L311) | 모든 ch·LUN·plane의 같은 block 번호 묶음(superblock). 할당·GC 단위 | `id`(=block 번호), `vpc`, `ipc`, `pos`, `close_seq`, `my_ru` |
| [`write_pointer`](../../ftl.h#L338) | 다음에 program할 위치 | `curline`, `ch`, `lun`, `pl`, `pg`, `blk` |
| [`line_mgmt`](../../ftl.h#L351) (`ssd->lm`) | line 목록 | `free_line_list`, `victim_line_pq`, `full_line_list`, `free_line_cnt` |
| [`nand_cmd`](../../ftl.h#L381) | `ssd_advance_status()`에 넘기는 명령 | `type`(USER/GC/MAP_IO), `cmd`(READ/WRITE/ERASE), `stime` |
| [`femu_mapping_ops`](../../ftl.h#L547) | L2P scheme vtable | `translate`, `prepare_write`, `commit_write`, `gc_relocate_commit`, `reclaim` |
| [`femu_ftl_policy_ops`](../../ftl.h#L514) | GC victim 선택 vtable | `select_victim_line`, `by_close_order` |
| [`ssd`](../../ftl.h#L611) | namespace 하나의 FTL 전체 상태 | `sp`, `ch`, `maptbl`, `rmap`, `wp`, `lm`, `mapping`, `policy`, FDP용 `rg`/`ruhs` |
| [`ru_mgmt`](../../ftl.h#L413), [`FemuReclaimGroup`](../../ftl.h#L497), [`FemuReclaimUnit`](../../ftl.h#L447), [`FemuRuHandle`](../../ftl.h#L478) | FDP용 RG/RU/RUH | [01번](01_FDP_objects_and_pointers.md)~08번 FDP 문서 참고 |

`struct ssd` 안의 연결 관계:

```
struct ssd
 ├─ sp                 : ssdparams
 ├─ ch[nchs] ─ lun[luns_per_ch] ─ pl[pls_per_lun] ─ blk[blks_per_pl] ─ pg[pgs_per_blk]
 ├─ maptbl[tt_pgs]     : LPN → struct ppa        (초기값 UNMAPPED_PPA)
 ├─ rmap[tt_pgs]       : ppa2pgidx(ppa) → LPN    (초기값 INVALID_LPN)
 ├─ lm                 : lines[tt_lines] + free list / victim pqueue / full list
 ├─ wp, hot_wp, log_wp, stream_wp[32]  → 각각 curline(line *) 하나를 채우는 중
 ├─ mapping, policy    : vtable 포인터 (기본 "page", "greedy")
 └─ (FDP) rg[nrg] → RU 배열,  ruhs[nruhs] → RG별 현재 RU
```

## 4. `struct ppa` 비트 구성

GCC x86(little-endian)에서 첫 bit-field가 LSB에 온다([`ftl.h` BITS 정의](../../ftl.h#L93)).

| 필드 | bit 위치 | 폭 | 축당 최대 개수 | 비고 |
|---|---|---|---|---|
| `blk` | 0–15 | 16 | 65536 | plane 안의 block 번호. **line id와 같다** |
| `pg` | 16–31 | 16 | 65536 | block 안의 page 번호 |
| `sec` | 32–39 | 8 | 256 | 할당 시 항상 0. `valid_ppa()`만 검사한다 |
| `pl` | 40–43 | 4 | 16 | plane |
| `lun` | 44–50 | 7 | 128 | channel 안의 LUN |
| `ch` | 51–62 | 12 | 4096 | channel |
| `rsv` | 63 | 1 | – | 사용하지 않는다 |

- 축당 최대 개수는 `bb_check_geometry()`([`ftl-geom.c`](../../ftl-geom.c))가 property 값과 비교해 검사한다.
- `INVALID_PPA`와 `UNMAPPED_PPA`는 모두 `~0ULL`(비트 전부 1)이다. `mapped_ppa()`는 이 값인지만 본다.
- **정수값 `ppa.ppa`와 [`ppa2pgidx()`](../../ftl-internal.h#L172)는 다르다.** 기본 geometry에서 ch=2, lun=3, pl=0, blk=10, pg=5인 경우:
  - `ppa.ppa` = `0x1030000005000a`. bit를 이어 붙인 값이며, 대입과 동일성 비교(`==`)에 쓴다.
  - `ppa2pgidx()` = 2·524288 + 3·65536 + 10·256 + 5 = **1247749**. `rmap[]`의 index로 쓰는 0..tt_pgs-1 평탄 번호다.

## 5. write pointer와 line 수명

[`ssd_wp_step()`](../../ftl-internal.h#L55)은 **ch → lun → pl → pg** 순으로 전진한다. 기본값(8ch × 8LUN × 1pl)에서 line의 page 0~63은 모두 `pg=0`이고 64개 LUN에 하나씩 흩어진다. 그래서 연속 쓰기가 channel·LUN 병렬성을 그대로 얻는다. 예를 들어 빈 장치에 32KiB(8 page)를 쓰면 8개 channel에 하나씩 가므로 약 200µs(program 1회)가 걸린다.

| line 상태 | 어디에 있나 | 다음 상태로 가는 시점 (모두 `ftl-line-gc.c`) |
|---|---|---|
| free | `lm.free_line_list` | write pointer가 `get_next_free_line()`으로 가져갈 때 |
| 쓰는 중 | 어떤 `write_pointer.curline` | `ssd_wp_step()`이 true를 반환할 때(line 끝) |
| full | `lm.full_line_list` (vpc == pgs_per_line) | page 하나가 invalid 되면 victim으로 이동 |
| victim | `lm.victim_line_pq` (기본: vpc 최소가 top) | GC가 고르면 valid page를 옮기고 erase |
| (회수 완료) | 다시 `free_line_list` | `mark_line_free()` |

## 6. 요청 처리 흐름 (NVMe 명령 → 완료)

```
guest가 SQ doorbell을 울림
 │ nvme_poller() → nvme_process_sq_io()                  [nvme-io.c, poller thread]
 │   req->stime = req->expire_time = 현재 시각
 │   nvme_io_cmd(): FLUSH/DSM/COPY/WRITE_ZEROES 등은 공통 처리
 │     └ 나머지 → ns->ext_ops.io_cmd = bb_io_cmd() → nvme_rw()       [bb.c]
 │                (검증 + guest ↔ backend 메모리 DMA, req->slba/nlb 설정)
 ▼ femu_ring_enqueue(n->to_ftl[poller])      ※ 검증에 실패한 요청도 ring으로 간다
femu_ftl_thread() → femu_ftl_process_req()               [femu.c, FTL thread 1개]
 └ bb_ftl_process_req(n, ns, req)                        [ftl.c]
     ① req->status 실패 → 0 반환      ② Streams면 streams_lock
     ③ (A | B = 비FDP | FDP)
        READ  → ssd_read()             WRITE → ssd_write() | nvme_do_write_fdp()
        FLUSH → ssd_buffer_destage()   DSM   → ssd_trim()  | ssd_trim_fdp_style()
        COPY  → ssd_copy()             WRITE_ZEROES → ssd_write_zeroes(_fdp_style)()
     ④ 오류 주입(err_*_ppm)    ⑤ should_gc() → do_gc(ssd, false) 1회 (FDP: do_gc_fdp_style)
     → lat 반환. 이어서 req->expire_time += lat
 ▼ femu_ring_enqueue(n->to_poller[poller])
nvme_process_cq_cpl()                                    [nvme-io.c, poller]
 └ expire_time 순 pqueue → 현재 시각 ≥ expire_time이면 CQE 기록 + 인터럽트
```

지연은 이렇게 계산된다(기본 flat timing, `ftl-media.c` → `nand/nand-media.c`).

1. `ssd_read()`는 LPN마다 `mapping->translate()`로 PPA를 얻고 `ssd_advance_status()`를 부른다. write buffer나 read cache에서 hit하면 NAND를 건너뛴다.
2. 각 NAND 명령의 완료 시각은 `max(req->stime, lun->next_lun_avail_time) + pg_rd_lat`이다. 이 값이 그 LUN의 새 `next_lun_avail_time`이 된다.
3. 요청의 지연은 LPN별 `(완료 시각 − stime)` 중 **최댓값**이다. 예를 들어 4 page read가 서로 다른 LUN에 있으면 40µs가 걸린다. 같은 LUN에 두 page가 몰리면 두 번째가 기다려서 80µs가 걸린다.
4. GC가 쓴 NAND 시간은 `lat`에 더해지지 않는다. 대신 `enable_gc_delay`일 때 LUN timeline에 쌓여 **다음 요청들을 늦춘다**.

## 7. 초기화·종료 흐름

| 단계 | 함수 (파일) | 하는 일 |
|---|---|---|
| 등록 | `nvme_register_extensions()` (femu.c) → [`nvme_register_bbssd()`](../../bb.c#L506) | controller의 `ext_ops`에 bb_* 콜백을 채운다 |
| namespace별 표 | `nvme_register_extensions_ns()` (femu.c) | 위 표를 복사해 `ns->ext_ops`를 만든다 |
| 생성 | `femu_realize()` / `nvme_ns_create()` → [`bb_init()`](../../bb.c#L234) | geometry·용량·FDP 검증 → `g_malloc0(struct ssd)` → `ssd_init()` |
| FTL 구성 | [`ssd_init()`](../../ftl.c#L33) | ① 파라미터 ② ch~page 배열 ③ 카운터 ④ media ⑤ maptbl/rmap ⑥ [policy](../../ftl.c#L138) 다음 [line](../../ftl.c#L142) ⑦ mapping/CMT/rcache ⑧ FDP 또는 [wp](../../ftl.c#L200) |
| namespace 삭제 | `nvme_ns_release()` (femu.c) → [`bb_ns_exit()`](../../bb.c#L428) | `ssd_free()` 다음 `g_free()`. CXL에서 빌린 ssd는 해제하지 않는다 |
| 장치 종료 | `femu_exit_extensions()` (femu.c) → [`bb_exit()`](../../bb.c#L446) | 모든 bbssd namespace에 `bb_ns_exit()`를 부른다 |

`ssd_init()`과 `ssd_free()`는 csd(`csd_init`), kvssd(`kvssd_init_timing_ssd`), cxlssd(`femu_cxl_start`)도 함께 쓴다. 이 모드들은 같은 FTL을 재사용한다.

## 8. 주요 설정 파라미터

device property 기본값은 `femu.c`의 `DEFINE_PROP_*` 목록에서 온다. `ssd->sp`로 옮기는 일은 `ssd_init_params()`([`ftl-geom.c`](../../ftl-geom.c))가 한다.

| 이름 | 의미 | 기본값 | 비고 / 설정 위치 |
|---|---|---|---|
| `femu_mode` | 모드 선택 | 2 (NoSSD) | bbssd는 **1**로 지정해야 한다 |
| `devsz_mb` | 노출 용량(MB) | 1024 | `bb_check_capacity()`가 GC 예약과 비교한다 |
| `secsz` × `secs_per_pg` | sector, page 크기 | 512 × 8 | page = 4KiB = LPN 하나 |
| `pgs_per_blk` / `blks_per_pl` | block당 page / plane당 block | 256 / 256 | `tt_lines = blks_per_pl = 256` |
| `pls_per_lun` / `luns_per_ch` / `nchs` | plane / LUN / channel | 1 / 8 / 8 | NAND 전체 16GiB, line = 16384 page = 64MiB |
| `pg_rd_lat` / `pg_wr_lat` / `blk_er_lat` | NAND 지연(ns) | 40000 / 200000 / 2000000 | flip 3번이 같은 값으로 되돌린다 |
| `ch_xfer_lat` | page 전송 지연(ns) | 0 | 0이 아니면 staged channel 모델이 켜진다(`ftl-media.c`) |
| `gc_thres_pcent` | background GC 기준 | 75 | `gc_thres_lines = (1−0.75)×256 = 64` |
| `gc_thres_pcent_high` | 강제 GC 기준 | 95 | `bb_gc_forced_lines()` = 12 line |
| `gc_policy` | 비FDP victim 정책 | (없음) → greedy | `femu_ftl_policy_lookup()` |
| `mapping` | L2P scheme | (없음) → page | `femu_mapping_scheme_lookup()` |
| `mapping_cache_mb` | DFTL CMT 크기 | 0 | mapping=dftl이고 0이면 4MB(`ssd_init`) |
| `read_cache_mb` / `cache_evict` | DRAM read cache | 0 / (없음) → clock | `rcache_init()` |
| `buffer_size` / `buffer_thres_pcent` | DRAM write buffer(page) | 0 / 90 | 0이면 끔 |
| `hot_cold_sep` | 덮어쓰기를 HOT class로 | false | `hot_wp` 사용 |
| `gc_strategy` | FDP RU GC 전략 | 0 (greedy) | 0/1/2/4만 허용 |
| `err_read_unc_ppm` / `err_write_fail_ppm` | 오류 주입 비율 | 0 / 0 | ppm → `1000000/ppm`번째마다 실패 |
| (property 아님) `enable_gc_delay` | GC의 NAND 시간을 반영할지 | true | `ssd_init_params()`, flip 1/2 |

## 9. FEMU flip admin 명령 (opcode `0xef`)

[`bb_flip()`](../../bb.c#L364)이 cdw10 값에 따라 처리한다. 지연 관련 값은 [`bb_flip_apply()`](../../bb.c#L295)가 poller를 멈춘 뒤 모든 namespace에 적용한다.

| cdw10 | 상수 | 효과 |
|---|---|---|
| 1 / 2 | `FEMU_ENABLE/DISABLE_GC_DELAY` | `sp.enable_gc_delay` on/off |
| 3 / 4 | `FEMU_ENABLE/DISABLE_DELAY_EMU` | NAND 지연을 40/200/2000µs로 설정하거나 0으로 설정하고 media timing을 갱신한다 |
| 5 | `FEMU_RESET_ACCT` | poller별 `nr_tt_ios`, `nr_tt_late_ios`를 합산해 출력하고 리셋한다 |
| 6 / 7 | `FEMU_ENABLE/DISABLE_LOG` | `n->print_log` on/off |

## 10. 코드를 읽다 확인한 참고 사항

- `struct ssd`의 `to_ftl`, `to_poller`, `ftl_thread`는 쓰이지 않는다. ring과 thread는 `FemuCtrl` 쪽에 있다. `dataplane_started_ptr`는 설정만 되고 읽는 곳이 없다.
- `nand_lun`/`ssd_channel`의 `busy`, `gc_endtime`, 그리고 `nand_block.wp`는 실제로 쓰이지 않는다.
- `ssd_waf_x1000()`은 `nvme.h`에 선언되어 있지만 호출하는 곳이 없다.
- `ftl.c`에는 위치가 어긋난 영어 주석이 몇 개 있다. `ssd_init()` 안의 bad-block 주석, `ssd_copy()` 위의 "Lines rewritten…" 한 줄, `ssd_percentage_used()` 위의 "SMART available_spare" 주석이다. 한글 주석에 표시해 두었다.
- flip 3/4는 `sp.ch_xfer_lat`을 0으로 바꾼다. 하지만 `bb_nand_media_refresh_timing()`은 rd/wr/er 3개만 media로 복사한다.

## 11. 공부 포인트 · 확인 질문

1. 기본 geometry에서 line 하나는 몇 page, 몇 MiB인가? 이때 `tt_lines`는?
2. `ssd_wp_step()`이 page 번호보다 channel을 먼저 바꾸는 이유는?
3. `ppa.ppa`와 `ppa2pgidx(ppa)`는 각각 어디에 쓰이나?
4. 기본 설정에서 `should_gc()`와 `should_gc_high()`가 true가 되는 free line 수는?
5. `bb_ftl_process_req()`의 반환값에는 GC 시간이 없다. 그런데도 GC가 host 지연을 늘리는 이유는?
6. 검증에 실패한 요청도 FTL ring을 거친다. 그런데도 FTL 상태를 건드리지 않는 이유는?
7. READ/WRITE와 달리 FLUSH·DSM은 `bb_io_cmd()`를 거치지 않는다. 그러면 FTL에는 어떻게 도달하나?

<details><summary>답 보기</summary>

1. 64 LUN × 256 page = 16384 page = 64MiB이고, `tt_lines`는 `blks_per_pl`과 같은 256이다.
2. 연속된 page를 서로 다른 channel/LUN에 두어 NAND 명령을 병렬로 겹치게 하기 위해서다(`next_lun_avail_time`이 LUN마다 따로 있다).
3. `ppa.ppa`는 비트를 이어 붙인 값으로, 비교·대입과 `UNMAPPED_PPA` 판정에 쓴다. `ppa2pgidx()`는 0..tt_pgs-1 평탄 번호로, `rmap[]` index에 쓴다.
4. free line이 64개 이하이면 `should_gc()`, 12개 이하이면 `should_gc_high()`가 true다.
5. GC의 NAND read/program/erase가 LUN의 `next_lun_avail_time`을 뒤로 밀기 때문이다. 다음 요청의 `max(stime, next_lun_avail_time)`이 커진다.
6. 첫머리에서 `req->status != NVME_SUCCESS`이면 0을 반환하기 때문이다. `slba`/`nlb`가 stale일 수 있다.
7. `nvme_io_cmd()`가 공통 처리(검증, DSM range 복사 등)를 마친다. 그 뒤 요청은 status와 상관없이 `to_ftl` ring으로 간다(csd의 `req->defer` 경우는 제외). FTL thread에서는 `bb_ftl_process_req()`의 opcode switch가 처리한다.

</details>
