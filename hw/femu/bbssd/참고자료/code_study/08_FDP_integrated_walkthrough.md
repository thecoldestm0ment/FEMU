# 8단계 — 통합 실행 추적과 연구 검증

기준: `master`, 커밋 `e2d5413ff`. 이번 단원은 **코드를 따라 계산한 가상 실행**이다. QEMU/fio를 실행한 실험 결과나 성능 예측이 아니다.

이전: [7단계 — 통계·이벤트·TRIM](07_FDP_statistics_events_and_trim.md)

## 1. 이번 단원의 질문

**PH 선택, RU 교체, 덮어쓰기, GC 이동, 통계 증가를 한 workload의 상태 변화로 설명할 수 있는가?**

각 함수의 설명을 하나의 상태 추적으로 연결한다. 마지막에는 연구 변경 지점과 검증해야 할 관계를 정리한다. 구현 패치는 제공하지 않는다.

## 2. 예시의 전제와 표기

다음은 설명을 단순하게 만든 가상 설정이다.

| 항목 | 가정 |
|---|---|
| RG | 1개, `rgif=0` |
| RU | 총 8개, `R0`부터 `R7` |
| RUH | 2개, `H0`와 `H1`, 모두 PI |
| PH 매핑 | `phs=[0,1]` |
| RU geometry | line 1개, channel 2개, channel당 LUN 2개, block당 2페이지 → RU당 8페이지 |
| 페이지 단위 | `secsz=512`, `secs_per_pg=8`, 한 페이지 4 KiB |
| 쓰기 요청 | 한 번에 정렬된 FTL 페이지 1개, 유효한 DTYPE=2 |
| GC 정책 | global greedy, heap 후보 순서가 정상인 상태 |
| 카운터 | 처음 0, overflow·오류·TRIM 없음 |

`A`~`P`는 서로 다른 LPN을 뜻한다. `[A,B,...]`는 RU write pointer가 배치하는 순서이며 NAND 한 block의 단순 배열을 뜻하지 않는다. `A×`는 A의 이전 페이지가 invalid가 되었음을 뜻한다.

**GC 호출 시점의 가정:** 쓰기들을 먼저 추적한 뒤 지정한 시점에 GC 함수를 한 번 호출하는 학습 예시다. 그 전에는 자동 GC가 개입하지 않는다고 둔다. 예를 들어 두 free 임계값이 0인 조건에서 아래 free 수는 0보다 크므로 자동 GC가 필요하다는 판단을 만들지 않는다. 지정 시점의 GC 호출은 별도의 가정이며 이 sequence가 기본 설정의 자동 실행 순서라는 뜻은 아니다.

## 3. S0 — 초기화 직후

출처: [bbssd/ftl.c:2344–2352](/root/workspace/FEMU/hw/femu/bbssd/ftl.c:2344)

```c
        /* allocate per-RG RU pointer array */
        ssd->ruhs[i].rus = g_malloc0(sizeof(FemuReclaimUnit *) *
                                     endgrp->fdp.nrg);
        for (int j = 0; j < (int)endgrp->fdp.nrg; j++) {
            ssd->ruhs[i].rus[j] = fdp_get_new_ru(ssd, j, i);
            ssd->ruhs[i].rus[j]->ruh = &ssd->ruhs[i];
            ssd->ruhs[i].ruh->rus[j] = ssd->ruhs[i].rus[j]->nvme_ru;
            ssd->ruhs[i].curr_ru = ssd->ruhs[i].rus[j];
        }
```

각 RUH가 RG에서 첫 RU를 하나 받는다. free 목록이 인덱스 순으로 초기화되었다고 두면 H0는 R0, H1은 R1을 받는다.

```text
H0: curr_ru=R0, gc_ru=NULL
H1: curr_ru=R1, gc_ru=NULL
free: R2, R3, R4, R5, R6, R7
full: 없음
victim: 없음
```

host active RU는 2개, free는 6개다. RUH별 `ru_in_use_cnt`는 각각 1이고 live pages는 0이다.

## 4. S1 — H0에 A~H를 쓴다

PID=0으로 A~H 8개를 쓰면 PH=0, RUH=H0가 선택된다. 모두 처음 쓰는 LPN이므로 R0는 8개 valid 페이지로 채워진다.

출처: [bbssd/ftl.c:1222–1224](/root/workspace/FEMU/hw/femu/bbssd/ftl.c:1222)

```c
                if (is_full) {
                    QTAILQ_INSERT_TAIL(&rm->full_ru_list, ru, entry);
                    rm->full_ru_cnt++;
```

R0가 full 목록에 들어가고, 공통 진행 함수가 새 RU를 즉시 확보한다. free 목록 앞의 R2를 H0의 새 `curr_ru`로 둔다.

```text
R0: [A,B,C,D,E,F,G,H]  vpc=8, ipc=0, full, 소속 H0
H0: curr_ru=R2(비어 있음), gc_ru=NULL
H1: curr_ru=R1(비어 있음), gc_ru=NULL
free: R3, R4, R5, R6, R7
```

호스트 쓰기량은 8페이지이고 H0 live pages는 8이다. H0는 R0와 R2 두 RU를 배정받은 상태다. 호스트 쓰기가 잠시 멈춰도 R2는 free 목록에 돌아가지 않는다.

## 5. S2 — A~D를 H1로 덮어쓴다

이번에는 PID=1로 A,B,C,D를 다시 쓴다. 새 페이지는 H1의 R1로 가고, 이전 페이지의 무효화는 R0를 따라간다.

출처: [bbssd/ftl.c:2094–2105](/root/workspace/FEMU/hw/femu/bbssd/ftl.c:2094)

```c
        ru = ruh->curr_ru;

        ppa = get_maptbl_ent(ssd, lpn);
        if (mapped_ppa(&ppa)) {
            mark_page_invalid_fdp(ssd, &ppa);
            set_rmap_ent(ssd, INVALID_LPN, &ppa);
        }
        /* new write */
        ppa = fdp_get_new_page(ssd, ru);
        set_maptbl_ent(ssd, lpn, &ppa);
        set_rmap_ent(ssd, lpn, &ppa);
        mark_page_valid_fdp(ssd, &ppa, ru);
```

A의 첫 덮어쓰기에서 R0는 full→victim으로 이동한다. B,C,D의 덮어쓰기에서는 이미 victim인 R0의 vpc가 추가로 감소한다. 이 예시에는 global victim 후보가 R0 하나라서 후보 간 순서 재배치 문제를 도입하지 않는다.

```text
R0: [A×,B×,C×,D×,E,F,G,H]  vpc=4, ipc=4, victim, 소속 H0
R1: [A,B,C,D,빈칸,빈칸,빈칸,빈칸]  vpc=4, ipc=0, host active H1
R2: 비어 있는 host active H0
```

| 집계 | H0 | H1 |
|---|---:|---:|
| 유효 페이지 | 4 | 4 |
| 누적 호스트 페이지 쓰기 | 8 | 4 |
| 배정 RU 수 | 2 | 1 |

기존 8개 LPN 중 A~D의 최신 위치만 H1로 바뀌었다. 전체 유효 LPN 수는 여전히 8개이고 누적 호스트 쓰기는 12페이지다.

## 6. S3 — H0에 I~L을 쓴다

H0의 현재 RU R2에 새로운 LPN I,J,K,L을 쓴다. R2는 절반만 사용된 활성 RU로 남는다.

```text
R0: E,F,G,H가 valid인 victim
R1: A,B,C,D가 valid인 H1 host RU
R2: I,J,K,L이 valid인 H0 host RU
free: R3, R4, R5, R6, R7
```

누적 host write는 16페이지, 유효 LPN은 A~L의 12개다. H0 live pages는 R0의 4개와 R2의 4개를 합친 8, H1은 4다.

## 7. S4 — R0를 PI GC로 회수한다

이 시점에 `do_gc_fdp_style(ssd, 0, 0, true)`가 호출되었다고 가정한다. 이것은 코드 추적을 위한 호출 표기이며 실제 명령 실행이나 소스 수정을 뜻하지 않는다.

### 7.1 victim 선택과 목적지 확보

후보는 R0 하나다. `force=true`이므로 background의 ipc 보류 조건을 건너뛴다. R0의 RUH는 PI인 H0다.

출처: [bbssd/ftl.c:1876–1882](/root/workspace/FEMU/hw/femu/bbssd/ftl.c:1876)

```c
    if (victim_ruh->ruh_type == NVME_RUHT_PERSISTENTLY_ISOLATED) {
        dest_ruh = victim_ruh;
        /* PI RUH: GC writes go to a dedicated gc_ru, distinct from curr_ru. */
        if ((ret = check_gc_ruh_available(ssd, dest_ruh)) < 0 ){
            ftl_err("No free space left in device. \n");
            ftl_assert(false && __LINE__ );
        }
```

H0의 `gc_ru`가 없으므로 R3를 새로 할당한다. free 수는 일시적으로 5→4가 되고 H0의 배정 RU 수는 2→3이 된다. source R0는 아직 회수 중이며 free가 아니다.

### 7.2 E~H의 매핑 이동

출처: [bbssd/ftl.c:1699–1702](/root/workspace/FEMU/hw/femu/bbssd/ftl.c:1699)

```c
    new_ppa = fdp_get_new_page(ssd, dest_ru);
    set_maptbl_ent(ssd, lpn, &new_ppa);
    set_rmap_ent(ssd, lpn, &new_ppa);
    mark_page_valid_fdp(ssd, &new_ppa, dest_ru);
```

R0의 valid 페이지 E,F,G,H를 R3에 옮긴다. A~D의 이전 invalid 페이지는 복사하지 않는다. maptbl은 E~H의 새 PPA를 가리키고 destination RUH의 live pages는 페이지마다 증가한다.

R3는 8페이지 중 4페이지만 사용하므로 GC RU 교체는 일어나지 않는다. H0의 host `curr_ru=R2`도 그대로다.

### 7.3 source 집계 차감과 free 반환

출처: [bbssd/ftl.c:1952–1955](/root/workspace/FEMU/hw/femu/bbssd/ftl.c:1952)

```c
    if (ssd->ruhs[victim_ru->ruh->ruhid].ru_in_use_cnt > 0) {
        ssd->ruhs[victim_ru->ruh->ruhid].ru_in_use_cnt--;
    }
    ssd->ruhs[victim_ru->ruh->ruhid].ruh_live_pages_cnt -= vpc_cnt;
```

H0 live pages는 복사 중 8→12가 되었다가 source 4개를 일괄 차감해 8로 돌아간다. RU 수는 3→2가 되고, R0를 free 목록 끝에 반환하면 free 수는 4→5가 된다.

```text
H0: curr_ru=R2[I,J,K,L,...], gc_ru=R3[E,F,G,H,...]
H1: curr_ru=R1[A,B,C,D,...], gc_ru=NULL
free: R4, R5, R6, R7, R0
full: 없음
victim: 없음
```

R0는 재사용 가능하지만 `ru->ruh`에 이전 H0 참조가 남을 수 있다. 그 포인터만으로 사용 중이라고 판정하지 않는다. R0의 이전 rmap에도 과거 값이 남을 수 있으므로 page status 및 최신 maptbl과 함께 읽는다.

### 7.4 공간 수지

| 단계 | free | host active | GC active | full | victim | 회수 중 |
|---|---:|---:|---:|---:|---:|---:|
| S0 초기화 | 6 | 2 | 0 | 0 | 0 | 0 |
| S1 H0 첫 RU 소진 | 5 | 2 | 0 | 1 | 0 | 0 |
| S2 덮어쓰기 | 5 | 2 | 0 | 0 | 1 | 0 |
| S3 I~L 쓰기 | 5 | 2 | 0 | 0 | 1 | 0 |
| S4 victim 제거·GC RU 배정 후 | 4 | 2 | 1 | 0 | 0 | 1 |
| S4 회수 완료 | 5 | 2 | 1 | 0 | 0 | 0 |

각 행의 합은 8이다. victim 하나를 반환했지만 새 GC RU를 하나 소비했으므로 free 순증가는 0이다. heap을 두 개에 등록하는 경우에도 같은 물리 RU는 한 번만 세어야 한다.

## 8. S4의 통계를 독립적으로 계산한다

출처: [bbssd/ftl.c:1937–1940](/root/workspace/FEMU/hw/femu/bbssd/ftl.c:1937)

```c
    /* update FDP statistics: media bytes written (GC writes) */
    uint64_t gc_bytes = (uint64_t)vpc_cnt * spp->secsz * spp->secs_per_pg;
    uint64_t erase_bytes = (uint64_t)blk_cnt * spp->secsz * spp->secs_per_pg
                           * spp->pgs_per_blk;
```

여기까지 host write는 16페이지, GC 이동은 4페이지다. 한 페이지를 4 KiB로 가정했으므로:

| 값 | 페이지 단위 계산 | 바이트 환산 |
|---|---:|---:|
| hbmw | 16페이지 상당 | 64 KiB |
| GC 쓰기 증가량 | 4페이지 | 16 KiB |
| mbmw | 16+4=20페이지 상당 | 80 KiB |
| mbe | block 4개 × block당 2페이지 | 32 KiB |
| 이 시점의 누적 카운터 WAF | 20/16 | 1.25 |

RUH별로 H0는 host 12페이지 상당에 GC 4페이지를 더하므로 mbmw가 16페이지 상당이다. H1은 host 4페이지 상당이며 GC 비용이 없다. 합하면 EG의 20페이지 상당과 같다.

이 수치는 페이지 단위·정렬·정상 처리 조건을 맞춘 가상 계산이다. 실제 장치에서 WAF 1.25가 나온다는 예측이나 성능 개선 주장으로 사용하지 않는다. block erase count의 중복 증가는 별도이며 mbe를 두 배로 만들지 않는다.

## 9. S5 — H1에 M~P를 써서 또 RU를 교체한다

R1의 뒤쪽 네 슬롯에 M,N,O,P를 쓰면 R1은 A~D와 M~P의 8개 valid 페이지를 가진 full RU가 된다. 후속 host RU로 free 목록 첫 R4를 받는다.

```text
H0: host R2[I,J,K,L,...], GC R3[E,F,G,H,...]
H1: host R4(비어 있음)
full: R1[A,B,C,D,M,N,O,P]
free: R5, R6, R7, R0
```

host 누적 쓰기는 20페이지, GC 누적은 4페이지이므로 mbmw는 24페이지 상당, 누적 WAF는 1.20이다. WAF가 1.25→1.20으로 변한 것은 새 GC 없이 host write가 추가되었기 때문이며, GC 알고리즘이 중간에 개선되었다는 뜻은 아니다.

## 10. S6 — GC로 옮겼던 E~H를 다시 H0에 쓴다

E,F,G,H를 H0로 다시 쓰면 현재 host RU인 R2의 뒤쪽에 기록한다. 기존 위치는 GC RU R3다. 이 경우에도 기존 PPA의 `line->my_ru`를 따라 무효화하므로, 별도의 “GC 출신 페이지” 예외 없이 기존 R3 통계를 줄인다.

```text
R2: [I,J,K,L,E,F,G,H]  vpc=8, ipc=0 → full
R3: [E×,F×,G×,H×,빈칸,빈칸,빈칸,빈칸]  vpc=0, ipc=4 → GC active 유지
H0: curr_ru=R5(새 RU), gc_ru=R3
H1: curr_ru=R4
full: R1, R2
free: R6, R7, R0
victim: 없음
```

R3는 유효 페이지가 0이지만 물리적으로 아직 다 쓰지 않은 활성 GC RU다. `was_full_ru` 조건에도 해당하지 않고 heap에도 없으므로 이 무효화만으로 free나 victim으로 이동하지 않는다. **vpc=0인 모든 RU가 즉시 회수 후보라는 해석은 틀리다.**

host 누적은 24페이지, GC는 4페이지, mbmw는 28페이지 상당이다. 누적 WAF는 `28/24 = 7/6 ≈ 1.1667`이다. 유효 LPN은 A~P의 16개이며 H0와 H1이 각각 8개씩 가진다.

## 11. 최종 상태에서 확인할 관계

S6의 종료 시점에 다음을 독립적으로 확인한다.

| 검증 대상 | 예상 관계 |
|---|---|
| 공간 소속 | free 3 + host active 2 + GC active 1 + full 2 = 총 8 |
| RUH의 배정 수 | H0: R2,R3,R5의 3개 / H1: R1,R4의 2개 |
| 최신 데이터 | H0는 E~L의 8개 / H1은 A~D와 M~P의 8개 |
| RU별 valid 합 | R1 8 + R2 8 = 16 |
| 매핑 | A~D,M~P→R1 / E~L→R2 |
| GC active | R3는 vpc=0, ipc=4이며 아직 write frontier 보유 |
| 바이트 계측 | hbmw 96 KiB, GC 16 KiB, mbmw 112 KiB |
| erase 바이트 | 이번 추적에는 GC가 한 번이므로 mbe 32 KiB |

GC 이동 때의 임시 live page 증가와 같은 함수 중간 상태를 최종 검증에 혼합하면 안 된다. free RU의 남아 있는 owner 포인터나 stale rmap만으로 데이터 소유 수를 세어서도 안 된다.

## 12. II로 바꾸면 무엇을 다시 추적해야 하는가?

이 예시의 PI 가정을 II로 바꾸면 단순히 점수만 바뀌는 것이 아니다.

- GC destination RUH가 마지막 RUH로 바뀌는지 확인한다.
- destination은 `gc_ru`가 아니라 마지막 RUH의 `curr_ru`이므로 host 쓰기와 공간을 공유한다.
- source/destination RUH가 다르면 live pages의 분포가 source -v, destination +v로 변한다.
- RUH별 GC 바이트 비용은 여전히 source RUH에 귀속된다.
- 목적지의 기존 잔여 공간과 RU 교체 횟수가 달라져 free RU 수지도 바뀔 수 있다.

현재 초기화는 isolation mode를 켜도 마지막 RUH만 II로 만든다. 여러 II stream이 하나의 GC RUH로 합류하는 실험을 설명하려면 실제 RUH 유형 설정이 그렇게 구성되는지 먼저 확인해야 한다.

## 13. 연구 변경 지점과 먼저 지킬 불변조건

다음 표는 학습한 코드를 이용한 설계 검토 안내다. 이 자료를 작성하면서 해당 코드를 수정하지 않았다.

| 연구 질문 | 읽고 검토할 위치 | 유지·검증할 관계 |
|---|---|---|
| 호스트 배치 힌트가 RUH 선택을 바꾸는가? | `ssd_stream_write`, `nvme_parse_pid`, `ns->fdp.phs` | 통계용·배치용 PH 해석 및 기본값 일치 |
| RU 소진 시 다른 공간 할당 정책을 넣는가? | `fdp_get_new_ru`, `fdp_advance_ru_pointer` | RG 소속, old RU의 상태 등록, host/GC 포인터 구분 |
| victim 기준을 바꾸는가? | `select_victim_ru`, heap callback, valid/invalid 갱신 | key 변경 규약, heap 순서, pos/ruh_pos, 후보 등록 완전성 |
| PI/II 이후 배치를 바꾸는가? | `do_gc_fdp_style`, `gc_write_page_fdp_style` | source/destination RUH, live pages, 비용 귀속, destination 잔여 공간 |
| RU 크기를 바꾸는가? | `ssd_init_fdp_params`, RU 구성, 포인터 진행 | 다중 line 전환, npages, NVMe RUAMW와 실제 geometry |
| reset을 측정 경계로 쓰는가? | `ssd_trim_fdp_style` | CB heap, GC RU, 카운터, 이벤트, 시간·erase 상태의 초기화 범위 |

5단계의 우선순위 선변경 문제, 6단계의 목적지 할당 실패, 7단계의 reset 비대칭은 **현재 구현을 해석할 때의 확인 지점**이다. 해당 현상이 모든 실험 결과에 영향을 주었다고 소스만으로 단정하지 않는다. 발생 조건과 실행 증거를 따로 확보해야 한다.

## 14. 검증을 설계할 때의 작은 사례들

대형 workload 전에 다음처럼 조건이 분명한 사례로 기대 상태를 적어 두면 원인을 좁힐 수 있다. 아래는 실행한 테스트 목록이 아니라 연구 검증 항목이다.

| 사례 | 관측할 결과 |
|---|---|
| 서로 다른 PH로 새로운 LPN 쓰기 | 서로 다른 RUH의 현재 RU로 배치되는지 |
| full RU의 첫 덮어쓰기 | full 수 -1, victim 수 +1, 기존 RUH live -1 |
| 활성 RU 안에서 같은 LPN 반복 | 활성 중 미등록, RU 소진 때 victim 직접 등록 |
| victim의 vpc가 부모 key보다 작아지도록 한 페이지 무효화 | 실제 최소 key가 heap head로 올라오는지 |
| PI GC 1회 | 같은 RUH의 gc_ru 사용, GC 끝 live page 총량 유지 |
| GC 목적 RU의 마지막 페이지까지 복사 | 후속 RU 선할당과 free 수지, NULL 경로 확인 |
| GC active RU의 valid 페이지를 전부 덮어쓰기 | vpc=0이어도 활성·미소진 상태인지 |
| CB 또는 GC 실행 이후 TRIM | free/full/victim 및 gc_ru와 카운터가 모두 일관적인지 |
| 다중 RG 요청 교대 | PID→RG와 `curr_ru == rus[rgid]`가 계속 일치하는지 |

결과를 기록할 때는 `(rgid, ruhid, ruidx)`, host/GC 구분, PPA, vpc/ipc, free/full/victim 수, mbmw/hbmw를 연결한다. 일부 trace 호출이 주석 처리되어 있으므로 현재 로그에 어떤 필드가 실제로 나오는지 먼저 확인한다.

## 15. 비교 실험의 인과 해석

배치와 GC를 함께 바꾼 두 실행의 WAF 차이는 두 변경을 합친 관찰이다. 배치만의 효과를 보려면 GC 전략과 geometry, RUH 수·유형, 초기 상태, workload를 맞춘 비교가 필요하다. GC 전략을 비교할 때도 PID 분포와 RUH 구성을 맞춘다.

고정할 항목에는 checkout·빌드, RG/RU/RUH 수, 물리·노출 용량, 여유 공간, 임계값, 지연 옵션, namespace LBA와 FTL 페이지 크기, preconditioning, seed, 쓰기량 또는 실행 시간, 성공한 요청 범위가 포함된다. TRIM이 fresh 초기화와 동일하지 않다는 7단계 사실도 초기 상태 선택에 반영한다.

WAF가 낮아졌다는 관찰만으로 tail latency·공정성·SSD 수명까지 개선되었다고 말하지 않는다. RUH별 지연과 throughput, GC 복사량, 공간 압력은 별도의 지표다. 단일 seed 결과는 그 조건의 관찰로 제시하고 일반적인 우월성이나 통계적 유의성으로 확대하지 않는다.

## 16. 최종 확인 질문과 답

1. **R0의 데이터를 H1로 덮어썼는데 왜 R0의 H0 live pages가 줄어드는가?**  
   기존 PPA의 line→RU→RUH 연결로 이전 소속을 찾기 때문이다.
2. **S4에서 victim 하나를 회수했는데 free는 왜 그대로 5인가?**  
   새 PI GC RU를 하나 소비하고 source RU 하나를 반환했기 때문이다.
3. **S4 GC 중 H0 live pages가 잠시 12가 되는 것은 항상 오류인가?**  
   아니다. destination 증가 후 source 일괄 차감이 아직 실행되지 않은 중간 상태다.
4. **S6의 R3는 vpc가 0인데 왜 victim이 아닌가?**  
   미소진 활성 GC RU이며 full→victim 또는 RU 소진 등록 경로를 거치지 않았다.
5. **WAF가 1.25에서 1.20으로 줄면 정책이 개선된 것인가?**  
   이 예시에서는 GC 없이 host write를 추가해 누적 비율이 달라진 것이다.
6. **RUH Update/Status만 보고 실제 FTL RU 교체를 증명할 수 있는가?**  
   아니다. NVMe 객체의 ruamw·참조와 FTL의 curr_ru/gc_ru 경로를 함께 확인해야 한다.
7. **이 표의 WAF를 실험 결과로 보고해도 되는가?**  
   아니다. 명시한 호출 시점과 작은 geometry를 가정한 코드 추적 계산이다.

## 17. 전체 학습자료 찾아보기

| 단계 | 자료 |
|---|---|
| 1 | [객체 관계와 쓰기 포인터](01_FDP_objects_and_pointers.md) |
| 2 | [초기화와 공간 구성](02_FDP_initialization.md) |
| 3 | [배치 정보 해석](03_FDP_placement_resolution.md) |
| 4 | [페이지 쓰기와 RU 교체](04_FDP_write_and_RU_rotation.md) |
| 5 | [무효화와 victim queue](05_FDP_invalidation_and_queues.md) |
| 6 | [GC와 II·PI 격리](06_FDP_GC_and_isolation.md) |
| 7 | [통계·이벤트·TRIM](07_FDP_statistics_events_and_trim.md) |
| 8 | 현재 자료: 통합 실행 추적과 연구 검증 |
