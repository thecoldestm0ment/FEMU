# 21. FDP 그림 학습 계획

[20_FDP_study_plan_fs.md](20_FDP_study_plan_fs.md)의 0~6단계를 그림으로 공부하기 위한 계획.
그림마다 **이 그림이 답하는 질문 / 그릴 것 / 근거 코드 / 다 그린 뒤 확인**을 적었다.
★ = 꼭 그릴 것(10장), 나머지는 시간 날 때.

## 공통 규칙

| 항목 | 규칙 |
|---|---|
| 한 장에 하나 | 그림 한 장은 질문 하나에만 답한다 |
| 예시 설정 고정 | [fdp.conf](../../../scripts/configs/fdp.conf) 기준: `nruh=4`, `nrg=1`, RU 256개, RU 1개 = 64 MiB |
| RUH 색 | RUH0 파랑, RUH1 주황, RUH2 초록, RUH3 보라 — 모든 그림에서 같게 |
| 층 구분 | 위: host / NVMe 층(`Nvme*`), 아래: FTL 층(`Femu*`, `ssd->`). 점선으로 나눈다 |
| thread 구분 | poller thread = 회색 lane, FTL thread = 노란 lane |
| 코드 이름 | 박스 아래 작은 글씨로 함수/구조체 이름 → 그림에서 코드로 바로 찾아갈 수 있게 |
| 도구 | draw.io, Excalidraw 추천. AI 이미지 생성기는 글자·화살표가 틀리기 쉽다 |

## 한눈에 보기

| 단계 | 그림 | 답하는 질문 |
|---|---|---|
| 0 | ★0-1 FDP 객체 계층 | RG·RU·RUH·PH는 어떤 관계인가? |
| 0 | 0-2 PID 비트 구성 | PID 16비트 안에 무엇이 들어 있나? |
| 1 | 1-1 명령줄 → 객체 | QEMU 옵션이 어느 구조체를 채우나? |
| 1 | ★1-2 RU 크기 계산 | RU 하나는 왜 64 MiB인가? |
| 1 | 1-3 RU 예산 | RU 256개는 어디에 쓰이나? |
| 1 | ★1-4 두 층의 RUH | `NvmeRuHandle`과 `FemuRuHandle`은 무엇이 다른가? |
| 2 | 2-1 write 명령 비트 | DTYPE·DSPEC은 명령 어디에 있나? |
| 2 | ★2-2 PID → RU 경로 | write 하나가 어느 함수를 거쳐 RU에 닿나? |
| 2 | ★2-3 예외 경로 | placement 없음 / 틀린 PID는 어디로 가나? |
| 2 | 2-4 host가 PID를 알아내는 법 | host는 쓸 수 있는 PID를 어떻게 아나? |
| 3 | ★3-1 명령 지도 | 어떤 FDP 명령이 어느 thread에서 끝나나? |
| 3 | 3-2 RUH Update 순서 | RUH Update 하면 RU가 어떻게 바뀌나? |
| 4 | ★4-1 RU 상태도 | RU는 어떤 상태를 돌아다니나? |
| 4 | 4-2 GC 타이밍 | background·foreground GC는 언제 도나? |
| 5 | ★5-1 victim 정책 비교 | 같은 RU들에서 정책마다 누구를 고르나? |
| 5 | ★5-2 PI vs II 격리 | GC로 옮긴 데이터는 어디로 가나? |
| 5 | 5-3 GC 한 번의 순서 | GC pass 하나는 어떤 순서로 진행되나? |
| 6 | ★6-1 전체 스택 | 파일시스템 hint가 RU까지 가는 전체 경로는? |

---

## 0단계

### ★0-1 FDP 객체 계층
- 그릴 것: Endurance Group 1개 → RG 1개 → RU 256개(작은 칸 줄).
  RUH 0~3이 각각 화살표(`curr_ru`)로 RU 하나씩 가리킴. 위쪽 namespace에 PH 0~3 → RUH 0~3 화살표.
- 핵심 메시지: RUH는 "지금 쓰는 RU를 가리키는 포인터", PH는 "namespace가 host에 보여 주는 RUH 번호".
- 근거: [15 §1](15_fdp_code_map.md), [`nvme_subsys_setup_fdp()`](../../../femu.c#L98)
- 확인: RU 하나가 가득 차면 RUH 화살표는 어떻게 바뀌나?

### 0-2 PID 비트 구성
- 그릴 것: 16비트 막대 = [RG 비트(상위 `rgif`개) | PH]. `nrg=1`이면 `rgif=0` → 막대 전체가 PH.
- 근거: [`nvme_parse_pid()`](../../../nvme-io.c#L1684) 위의 `nvme_pid2rg()`·`nvme_pid2ph()`, `nvme_calc_rgif()`([femu.c](../../../femu.c#L72))

## 1단계

### 1-1 명령줄 → 객체
- 그릴 것: 왼쪽에 `-device femu-subsys,fdp=on,fdp.nruh=4,fdp.nrg=1,fdp.nru=256`과
  `-device femu,...,gc_strategy=0,subsys=...`. 각 옵션에서 화살표로
  `endgrp->fdp`(NVMe 층), `ns->fdp.phs`(namespace), `ssd->rg`/`ssd->ruhs`(FTL 층)로.
- 근거: [20 1단계 표](20_FDP_study_plan_fs.md)

### ★1-2 RU 크기 계산
- 그릴 것: 8 channel × 8 LUN 격자. 모든 칸에서 같은 block 번호 i를 칠해 "line i = RU i".
  옆에 식: `8 × 8 × 1 plane × 256 page × 8 sector × 512 B = 64 MiB`.
- 핵심 메시지: RU = superblock(line) 1개라서 `fdp.runs`는 0 또는 정확히 이 값만 허용.
- 근거: [`bb_check_capacity()`](../../bb.c#L43), [13 §2](13_line_gc_geometry_media.md)
- 확인: `luns_per_ch`를 4로 줄이면 RU 크기는?

### 1-3 RU 예산
- 그릴 것: RU 256칸 막대를 구역으로 나눔: RUH별 열린 RU 4 + PI RUH별 GC용 RU 4 +
  forced GC 여유 + 나머지(namespace 용량). 최소 조건 `2 × nruh + 1` 표시.
- 근거: [features/fdp.md](../../../docs/features/fdp.md) "Configuration", [`bb_check_capacity()`](../../bb.c#L43)

### ★1-4 두 층의 RUH
- 그릴 것: 위 박스 `NvmeRuHandle`(host에 보고): `ruht`(PI/II), `ruha`, `ruamw`, `event_filter`.
  아래 박스 `FemuRuHandle`(실제 배치): `curr_ru`, `gc_ru`, `rus[]`, `ru_mgmt`, `hbmw/mbmw/mbe`.
  `FemuRuHandle.ruh` → `NvmeRuHandle`, `FemuReclaimUnit.nvme_ru` → `NvmeReclaimUnit` 화살표.
- 핵심 메시지: host가 보는 숫자(NVMe 층)와 실제 page 배치(FTL 층)는 따로 있고 포인터로 연결된다.
- 근거: [ftl.h](../../ftl.h) `struct FemuRuHandle`, [`femu_fdp_ssd_init_ru_handles()`](../../ftl-fdp.c#L1903)
- 확인: RUAMW는 어느 층 값인가?

## 2단계 (가장 중요)

### 2-1 write 명령 비트
- 그릴 것: NVMe write의 CDW12·CDW13 막대. CDW12 [23:20] = DTYPE(2), CDW13 [31:16] = DSPEC(PID).
  아래에 `nvme write ... -T 2 -S 1`의 `-T`, `-S`가 각각 어디로 가는지.
- 근거: [`nvme_rw()`](../../../nvme-io.c#L965) 주석

### ★2-2 PID → RU 경로
```text
 host        poller lane               FTL lane
 write ──▶ nvme_rw() ──(ring)──▶ bb_ftl_process_req() ─▶ nvme_do_write_fdp()
 DSPEC=1   req->fdp_dspec=1           ─▶ ssd_stream_write_lpns()
                                         ① nvme_parse_pid(1) → ph 1, rg 0
                                         ② ruhid = phs[1] = 1
                                         ③ ssd->ruhs[1].curr_ru 에 page 할당
                                         ④ RUAMW 감소, RU 차면 교체
 완료 ◀──────────────────────(ring)──── 지연 계산
```
- 그릴 것: 위 스케치를 swimlane으로. 값 `1`이 단계마다 어떤 이름(PID → PH → RUHID)으로 바뀌는지 강조.
- 근거: [`ssd_stream_write_lpns()`](../../ftl-fdp.c#L1359) ①②, [`phs[i] = i`](../../../femu.c#L359), [11 §6](11_core_structures_and_dispatch.md)
- 확인: FEMU에서 PH와 RUHID가 같은 이유는? 실제 SSD에서도 같을까?

### ★2-3 예외 경로
- 그릴 것: 판단 다이아몬드 2개.
  `DTYPE == 2?` 아니오 → PH 0. 예 → `PID 유효?` 아니오 → PH 0 + INVALID_PID event, 예 → 정상 경로.
- 근거: [`ssd_stream_write_lpns()`](../../ftl-fdp.c#L1359) ①
- 확인: 커널이 DSPEC을 안 넣으면 모든 데이터는 어느 RUH로 가나?

### 2-4 host가 PID를 알아내는 법
- 그릴 것: host → IO Management Receive(RUH Status) → 응답 표(PID, RUHID, RUAMW) →
  host가 "stream 번호 → 표의 index → PID"로 사용. fio `fdp_pli=0,1`이 이 index.
- 근거: [`nvme_io_mgmt_recv_ruhs()`](../../../nvme-io.c#L1771)

## 3단계

### ★3-1 명령 지도
- 그릴 것: 왼쪽 admin queue / I/O queue, 가운데 명령, 오른쪽 처리 함수.
  admin 경로, poller, FTL thread 세 줄로 구분.

| admin 경로에서 끝남 | poller가 응답, 완료만 FTL 경유 | FTL thread가 실제 처리 |
|---|---|---|
| Get Log Page 20h/21h/22h/23h, Get/Set Features 1Dh/1Eh | IO Mgmt Receive(RUH Status) | Write/Write Zeroes/Copy(DTYPE=2), IO Mgmt Send(RUH Update), DSM Deallocate |

- 근거: [20 3단계 표](20_FDP_study_plan_fs.md)
- 확인: RUH Status는 FTL에서 할 일이 없는데 왜 FTL ring을 거치나? RUH Update는 왜 FTL이 직접 처리해야 하나?

### 3-2 RUH Update 순서
- 그릴 것: ① PID → RUH → ② 지금 RU가 비어 있으면 그대로(event만) →
  ③ 새 RU 할당(없으면 GC 1회 후 재시도, 실패 시 CAP_EXCEEDED) → ④ 옛 RU는 full 또는 victim으로 →
  ⑤ NVMe 층·FTL 층 포인터 둘 다 새 RU로.
- 근거: [`ssd_fdp_update_ruhs()`](../../ftl-fdp.c#L1605)

## 4단계

### ★4-1 RU 상태도
```text
 free ──(RUH가 RU 필요)──▶ active ──(가득 참 / RUH Update)──▶ 전부 valid? ─ yes ▶ full list
   ▲                                                         └ no ▶ victim queue
   └──── GC: valid page 이동 → erase ◀───────────────────────────────┘
          (full list RU에 invalid page가 생기면 victim queue로 이동)
```
- 근거: [design/fdp.md](../../../docs/design/fdp.md) "Garbage collection", [`fdp_get_new_ru()`](../../ftl-fdp.c#L174), [`fdp_advance_ru_pointer()`](../../ftl-fdp.c#L326)
- 확인: 덮어쓰기 하나가 RU를 full list에서 victim queue로 옮기는 과정은?

### 4-2 GC 타이밍
- 그릴 것: 가로 시간축에 요청 하나. foreground GC = write 시작 전과 **매 page 전**(free RU ≤ 5%),
  background GC = 요청 끝(free RU ≤ 25%). 옆에 free RU 게이지와 75% / 95% 선.
- 근거: [`fdp_gc_until_clear()`](../../ftl-fdp.c#L1323), [`bb_ftl_process_req()` 끝](../../ftl.c#L674)

## 5단계

### ★5-1 victim 정책 비교
- 그릴 것: 같은 victim 후보 3개를 4칸에 반복해 그리고, 칸마다 고른 RU에 동그라미.
  예시(RU 하나 = 100 page로 단순화):

| RU | RUH | valid | age | cost-benefit 점수 (1−u)·age/u |
|---|---|---|---|---|
| A | 0 | 20 | 1 | 0.8 × 1 / 0.2 = 4 |
| B | 1 | 50 | 10 | 0.5 × 10 / 0.5 = 10 |
| C | 0 | 30 | 2 | 0.7 × 2 / 0.3 ≈ 4.7 |

  greedy(0) → A(valid 최소) / cost-benefit(1) → B(점수 최대) / random(2) → 아무거나(`gc_seed`로 재현) /
  per-handle(4) → RUH별 heap의 맨 위끼리 비교해 valid 최소 → 지금 코드에선 A(greedy와 거의 같음)
- 핵심 메시지: cost-benefit은 오래된(cold) RU를 valid가 많아도 먼저 고를 수 있다.
  per-handle은 RUH별 기준을 넣을 수 있는 구조지만 현재 threshold가 0이다.
- 근거: [`select_victim_ru()`](../../ftl-fdp.c#L678), [design/fdp.md](../../../docs/design/fdp.md) "Victim selection"
- 확인: hot/cold가 섞이면 greedy가 왜 불리할 수 있나?

### ★5-2 PI vs II 격리
- 그릴 것: 좌우 두 칸.
  왼쪽 PI: RUH1 victim의 valid page → RUH1의 `gc_ru`(host가 쓰는 `curr_ru`와 별도). 색이 안 섞임.
  오른쪽 II: victim의 valid page → 마지막 RUH(RUH3)의 `curr_ru`. 다른 색과 섞임.
- 근거: [`fdp_gc_frontier()`](../../ftl-fdp.c#L601), [design/fdp.md](../../../docs/design/fdp.md) "Where relocated data goes"
- 확인: 파일시스템 hotness 분리에는 PI와 II 중 어느 쪽이 맞나?

### 5-3 GC 한 번의 순서
- 그릴 것: victim 선택 → valid page를 하나씩 옮기며 옛 page invalid → 다 옮긴 뒤 erase → free list.
  옆 가지: 도중에 옮길 곳이 없으면 erase 없이 중단 → victim은 queue로 돌아감.
- 근거: [`do_gc_fdp_style()`](../../ftl-fdp.c#L1162), [`gc_write_page_fdp_style()`](../../ftl-fdp.c#L893)

## 6단계

### ★6-1 전체 스택
```text
 파일시스템   파일 hotness → class (hot / warm / cold)          ← 우리가 수정
 block layer  class → write stream 번호                        ← 우리가 수정
 NVMe 드라이버 RUH Status로 PID 목록 → stream → PID, DTYPE=2     ← 우리가 수정
 ─────────────────────────────── NVMe ───────────────────────────────
 FEMU         PID → PH → RUHID → RU (2-2)  →  GC(5-1, 5-2)
 측정         nvme fdp status / stats, WAF(log C0h)
```
- 그릴 것: 위 스케치를 세로 스택으로. 수정할 계층은 강조, 각 층 옆에 "이 층이 알아야 할 정보".
- 근거: [20 6단계](20_FDP_study_plan_fs.md)
- 확인: class 수가 RUH 4개보다 많으면 어느 층에서 합쳐야 하나?

---

## 순서 추천

1. ★만 단계 순서대로: 0-1 → 1-2 → 1-4 → 2-2 → 2-3 → 3-1 → 4-1 → 5-1 → 5-2 → 6-1
2. 그림 하나 그릴 때마다 근거 코드를 열어 박스 아래 함수 이름이 맞는지 확인
3. 각 그림의 "확인" 질문에 답할 수 있으면 다음 그림으로
