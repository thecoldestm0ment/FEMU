# 20. FDP 학습 계획 — 파일시스템 placement 관점

목표: 파일시스템이 정한 write stream이 FEMU 안에서 어느 RU로 가는지,
어떤 명령으로 주고받는지, GC가 그걸 어떻게 다루는지 설명할 수 있다.
FDP 전체를 완벽히 이해하는 것이 목표가 아니다.

| 박사님 과제 | 단계 |
|---|---|
| ① PH (application이 지정) ↔ RUHID (FEMU가 배정) | **2단계** |
| ② FEMU FDP의 GC 정책 | 4~5단계 |
| ③ 명령을 주고받고 처리하는 부분 | 3단계 |
| ④ FDP 파라미터 설정 (RUH 개수, RU 크기 등) | 1단계 |

## 진행 방식

- 한 번에 한 단계씩: **읽기 → 해보기 → 체크 질문**. 체크 질문에 답할 수
  있으면 다음 단계로 넘어간다.
- 코드는 `study` 브랜치의 한국어 주석(`호출:` / `흐름:`)을 따라 읽는다.
- 01~08 노트는 각 단계의 "더 보기"로만 쓴다(예전 코드 기준,
  `⚠ 최신 코드:` 표시 주의). 지도는 [15_fdp_code_map.md](15_fdp_code_map.md).
- 공식 문서 두 개가 최신이고 짧다:
  [features/fdp.md](../../../docs/features/fdp.md)(사용법),
  [design/fdp.md](../../../docs/design/fdp.md)(설계).

그림으로 공부할 때는 [21_FDP_image_plan.md](21_FDP_image_plan.md)의 단계별 그림 목록을 같이 쓴다.

| 단계 | 주제 | 끝나면 할 수 있는 것 |
|---|---|---|
| 0 | 용어와 큰 그림 | PID 하나가 어디로 가는지 말로 설명 |
| 1 | 파라미터 | FDP 장치를 원하는 RUH 개수·RU 크기로 띄움 |
| 2 | PH ↔ RUHID | write 하나의 DSPEC이 RU에 닿기까지 함수 순서대로 설명 |
| 3 | 명령 | `nvme fdp ...` 출력이 어느 함수에서 나오는지 앎 |
| 4 | RU 교체와 GC 시점 | RU가 언제 차고 GC가 언제 도는지 앎 |
| 5 | GC 정책 | `gc_strategy` 0/1/2/4의 차이와 격리(PI/II) 설명 |
| 6 | 파일시스템과 연결 | 커널 수정 시 무엇을 맞춰야 하는지 정리 |

---

## 0단계 — 용어와 큰 그림

| 용어 | 뜻 | FEMU에서 |
|---|---|---|
| RU (Reclaim Unit) | GC가 한 번에 지우는 단위 | superblock(line) 1개 |
| RG (Reclaim Group) | RU 묶음 | `fdp.nrg`는 1만 허용 |
| RUH (RU Handle) | 지금 쓰는 RU를 가리키는 포인터 | 개수 = `fdp.nruh` |
| PH (Placement Handle) | namespace가 host에게 보여 주는 handle 번호 | `phs[ph] = ruhid`, FEMU는 `phs[i] = i` |
| PID (Placement ID) | write에 실어 보내는 값 = (RG 비트 \| PH) | `nrg=1`이면 PID = PH |
| DTYPE / DSPEC | write의 directive type / 값 | DTYPE=2(data placement), DSPEC=PID |
| RUAMW | 현재 RU에 더 쓸 수 있는 양 | LBA 단위로 줄어듦 |
| PI / II | GC 뒤에도 격리 유지 / 처음만 격리 | 기본은 전부 PI |

- 읽기: [features/fdp.md](../../../docs/features/fdp.md) 처음 ~ "Configuration",
  [15 §1](15_fdp_code_map.md)
- 체크: `nvme write ... -T 2 -S 1`의 `-T 2`와 `-S 1`은 각각 무엇인가?

## 1단계 — FDP 파라미터 (과제 ④)

| 속성 | 위치 | 기본값 | 의미·제약 |
|---|---|---|---|
| `fdp=on` | `femu-subsys` | off | FDP 켜기. 속성은 `femu`가 아니라 subsys에 있다 |
| `fdp.nruh` | subsys | 0(필수) | RUH 개수 = host가 고를 수 있는 PH 개수 |
| `fdp.nru` | subsys | 128 | RU 개수. 실제 = min(`fdp.nru`, `blks_per_pl`), 최소 `2*nruh+1` |
| `fdp.runs` | subsys | 0 | RU 크기. 0이면 superblock 크기, 다른 값이면 그 크기와 같아야 함 |
| `fdp.nrg` | subsys | 1 | 1만 허용 |
| `fdp.isolation_mode` | subsys | 0 | 0 = 전부 PI, 그 외 = 마지막 RUH만 II |
| `gc_strategy` | `femu` | 0 | 0 greedy, 1 cost-benefit, 2 random, 4 per-handle |
| `gc_thres_pcent` / `_high` | `femu` | 75 / 95 | background / foreground GC 기준 |

- 코드 (NVMe가 보는 쪽 → FTL 내부 쪽 순서):
  1. [`nvme_subsys_setup_fdp()`](../../../femu.c#L98): `endgrp->fdp`에 RUH·RU 배열과 PI/II 설정
  2. [`nvme_ns_init_fdp()`](../../../femu.c#L343): namespace의 PH 표 `phs[]` 생성
  3. [`ssd_init_fdp_params()`](../../ftl-fdp.c#L1995) → [`femu_fdp_ssd_init_reclaim_group()`](../../ftl-fdp.c#L1824) → [`femu_fdp_ssd_init_ru_handles()`](../../ftl-fdp.c#L1903): FTL 쪽 RG/RU/RUH
  4. 잘못된 설정 거부: [`bb_check_capacity()`](../../bb.c#L43)(RU 크기·개수),
     [`bb_init_fdp()`](../../bb.c#L185)(FDP와 안 맞는 옵션), [`bb_check_geometry()`](../../ftl-geom.c#L90)(`gc_strategy`)
- 핵심: 같은 RUH가 **두 층**에 있다. NVMe 층(`NvmeRuHandle`, host에 보고)과
  FTL 층(`FemuRuHandle`, 실제 page 배치).
- 해보기: [fdp.conf](../../../scripts/configs/fdp.conf)로 띄우고 `nvme fdp configs /dev/nvme0 -e 1`.
  `fdp.nruh`를 4 → 8로 바꿔 다시 확인. `fdp.runs`를 일부러 틀려서 거부 메시지 확인.
- 더 보기: [02](02_FDP_initialization.md)
- 체크: 기본 geometry에서 RU 크기가 64 MiB인 계산식은? RU가 `2*nruh+1`개 이상 필요한 이유는?

## 2단계 — PH ↔ RUHID (과제 ①, 가장 중요)

```text
host  nvme write -T 2 -S <pid>      CDW12[23:20]=DTYPE, CDW13[31:16]=DSPEC
 │
 ├ nvme_rw()                          nvme-io.c   req->fdp_dtype / fdp_dspec 저장
 ├ (FTL thread) bb_ftl_process_req()  ftl.c       FDP면 nvme_do_write_fdp()
 ├ ssd_stream_write_lpns()            ftl-fdp.c
 │   ① nvme_parse_pid(pid) → (ph, rg)   rg = pid 상위 rgif 비트, ph = 나머지
 │      DTYPE≠2 또는 잘못된 PID → ph = 0 (+ INVALID_PID event)
 │   ② ruhid = ns->fdp.phs[ph]
 │   ③ ruh->curr_ru에 page 할당, RUAMW 감소
 └ 완료
```

- 코드: [`nvme_rw()`](../../../nvme-io.c#L965)의 DTYPE/DSPEC 파싱,
  [`nvme_parse_pid()`](../../../nvme-io.c#L1684)와 위의 `nvme_pid2ph()`·`nvme_pid2rg()`,
  [`phs[i] = i`](../../../femu.c#L359), [`ssd_stream_write_lpns()`](../../ftl-fdp.c#L1359)의 ①②
- host는 PID 목록을 **RUH Status**(IO Management Receive)로 알아낸다:
  [`nvme_io_mgmt_recv_ruhs()`](../../../nvme-io.c#L1771). fio의 `fdp_pli=0,1`은 이 목록의 index다.
- 해보기:
  1. `nvme write ... -T 2 -S 1` 후 `nvme fdp status /dev/nvme0n1`에서 PID 1의 RUAMW 감소 확인
  2. `-S 99`로 쓰고 `nvme fdp events /dev/nvme0 -e 1 -E`에서 INVALID_PID 확인
  3. QEMU 환경변수 `FEMU_FDP_DEBUG=1`로 trace 보기
- 더 보기: [03](03_FDP_placement_resolution.md)
- 체크: PID, PH, RUHID는 각각 누가 정하나? FEMU에서는 왜 PH = RUHID인가(실제 SSD는 다를 수 있음)?

## 3단계 — 명령 주고받기 (과제 ③)

| 명령 | 하는 일 | FEMU 처리 함수 | 처리 위치 |
|---|---|---|---|
| Identify Controller | CTRATT bit 19 = FDP 지원 | (`NVME_CTRATT_FDPS`) | admin 경로에서 바로 응답 |
| Get Log Page 20h | FDP 설정(RUH 수, RU 크기) | [`nvme_fdp_confs()`](../../../nvme-admin.c#L2346) | admin 경로에서 바로 응답 |
| Get Log Page 21h | RUH 사용 현황 | [`nvme_fdp_ruh_usage()`](../../../nvme-admin.c#L2415) | admin 경로에서 바로 응답 |
| Get Log Page 22h | 통계 HBMW/MBMW/MBE | [`nvme_fdp_stats()`](../../../nvme-admin.c#L2461) | admin 경로에서 바로 응답 |
| Get Log Page 23h | FDP event | [`nvme_fdp_events()`](../../../nvme-admin.c#L2493) | admin 경로에서 바로 응답 |
| Get/Set Features 1Dh, 1Eh | FDP 모드 / event 필터 | [`nvme_get_feature_fdp()`](../../../nvme-admin.c#L1188), [`nvme_set_feature()`](../../../nvme-admin.c#L1501) | admin 경로에서 바로 응답 |
| IO Mgmt Receive (12h) | RUH Status: PID 목록·RUAMW | [`nvme_io_mgmt_recv_ruhs()`](../../../nvme-io.c#L1771) | poller가 응답 작성, 완료만 FTL 경유 |
| IO Mgmt Send (1Dh) | RUH Update: 새 RU로 교체 | [`nvme_io_mgmt_send_ruh_update()`](../../../nvme-io.c#L1849) → [`ssd_fdp_update_ruhs()`](../../ftl-fdp.c#L1605) | FTL thread까지 |
| Write / Write Zeroes / Copy | DTYPE=2면 placement | `nvme_rw()` → `nvme_do_write_fdp()` | FTL thread |
| DSM Deallocate | trim | `ssd_trim_fdp_style()` | FTL thread |

- admin 명령은 host가 admin doorbell을 쓸 때 바로 처리된다(`nvme_process_sq_admin()`). I/O 명령은 실패한 것까지 전부 FTL ring을 거쳐 완료된다.
- poller와 FTL thread의 역할은 [11 §6](11_core_structures_and_dispatch.md)과
  [design/fdp.md](../../../docs/design/fdp.md) "Threads, locks and where latency is charged"
- 해보기: `nvme fdp configs / usage / stats / events / status`를 하나씩 실행하고
  출력 필드를 위 함수의 구조체 필드와 대응시키기
- 더 보기: [07](07_FDP_statistics_events_and_trim.md) (⚠ 많음)
- 체크: RUH Status와 RUH Update는 각각 어느 thread에서 끝나나? HBMW와 MBMW 차이로 무엇을 계산하나?

## 4단계 — RU 교체와 GC 시점 (과제 ② 준비)

```text
free ──(RUH가 RU 필요)──▶ active(curr_ru) ──(가득 참 / RUH Update)──▶ 전부 valid? ─ yes ▶ full list
  ▲                                                                   └ no ▶ victim queue
  └──────────── GC: valid page 이동 → erase ◀──────────────────────────────────┘
```

| GC 종류 | 언제 | 코드 |
|---|---|---|
| background | 요청 하나 끝날 때마다, free RU ≤ (1 − `gc_thres_pcent`/100) × 전체 | [`bb_ftl_process_req()` 끝](../../ftl.c#L674), `should_gc_fdp_style()` |
| foreground | placed write 전과 **매 page 전**, free RU ≤ (1 − `_high`/100) × 전체 | [`fdp_gc_until_clear()`](../../ftl-fdp.c#L1323) |

- 코드: [`fdp_get_new_ru()`](../../ftl-fdp.c#L174), [`fdp_advance_ru_pointer()`](../../ftl-fdp.c#L326), [`mark_page_invalid_fdp()`](../../ftl-fdp.c#L442)
- 더 보기: [04](04_FDP_write_and_RU_rotation.md), [05](05_FDP_invalidation_and_queues.md) + [15 §4](15_fdp_code_map.md)
- 체크: 덮어쓰기 하나가 RU를 full list에서 victim queue로 옮기는 과정은?

## 5단계 — GC 정책 (과제 ②)

| `gc_strategy` | 정책 | victim 고르는 기준 |
|---|---|---|
| 0 (기본) | greedy | valid page가 가장 적은 RU |
| 1 | cost-benefit | 빈 RU 먼저, 아니면 (1 − u) × age / u 최대 (u = valid 비율, 선택할 때 계산) |
| 2 | random | victim 중 무작위 (`gc_seed`로 재현 가능) |
| 4 | per-handle | PI handle별 queue에서 valid 최소, 없으면 greedy |

| 격리 | victim의 valid page가 가는 곳 |
|---|---|
| PI | 같은 RUH의 GC 전용 RU(`gc_ru`) → GC 후에도 다른 handle과 섞이지 않음 |
| II | 마지막 RUH의 현재 RU → GC 후 섞일 수 있음 |

- 코드: [`select_victim_ru()`](../../ftl-fdp.c#L678) → [`do_gc_fdp_style()`](../../ftl-fdp.c#L1162) →
  [`fdp_gc_frontier()`](../../ftl-fdp.c#L601) / [`gc_write_page_fdp_style()`](../../ftl-fdp.c#L893)
- [ftl.h](../../ftl.h#L395)의 enum에는 10~13 등 다른 정책 이름도 있지만 설정으로는 0/1/2/4만 허용된다.
- 배경: FEMU FDP의 출처인 WARP 논문 ([FAST '26](https://www.usenix.org/conference/fast26/presentation/song))의 GC 부분
- 해보기: hot/cold가 섞인 fio를 `--fdp_pli=0,1`로 두 handle에 나눠 쓰고,
  `gc_strategy` 0/1/2/4마다 WAF(vendor log C0h)와 `nvme fdp stats`의 MBMW/HBMW 비교
- 더 보기: [06](06_FDP_GC_and_isolation.md) (⚠ 13곳 → 반드시 15 §4와 같이)
- 체크: greedy가 hot/cold 분리에 불리한 경우는? PI와 II 중 파일시스템 hotness 분리에 맞는 것은?

## 6단계 — 파일시스템과 연결 (정리)

| 계층 | 할 일 | FEMU에서 확인 |
|---|---|---|
| 파일시스템 | 파일/데이터 hotness → class | — |
| block layer | class → write stream 번호 | — |
| NVMe 드라이버 | RUH Status로 PID 목록 획득 → stream 번호를 PID로 → write에 DTYPE=2, DSPEC=PID | `nvme fdp status`, `FEMU_FDP_DEBUG` |
| FEMU | PID → PH → RUHID → RU (2단계) | RUAMW 감소, MBMW/HBMW, WAF |

- 일반 block device write에는 placement 정보가 없다. 게스트 커널이 넣어 줘야 한다.
  Linux 최근 버전(6.16 무렵)에 block layer write stream과 NVMe FDP 연결이 들어갔으니,
  쓸 게스트 커널 버전에서 확인해 볼 것.
- fio의 `io_uring_cmd` + `fdp=1` 구현이 host 쪽 PID 처리의 참고 예시다.
- 체크: class 수가 `fdp.nruh`보다 많으면 어떻게 할까? II/PI 선택은 어느 계층이 알아야 할까?
