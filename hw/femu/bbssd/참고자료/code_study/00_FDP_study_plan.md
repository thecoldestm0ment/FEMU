# FEMU bbssd FDP 코드 학습 계획

- 기준: `master`, 커밋 `e2d5413ff`, 2026-09-17 확인.
- 대상: `hw/femu/bbssd/ftl.h`, `ftl.c`의 FDP 경로.
- 전제: non-FDP의 매핑, 페이지 상태, 기본 GC, NAND 타이밍은 이미 학습했다.
- 목표: 호스트 배치 정보가 RU 선택과 GC 후 데이터 배치에 어떤 영향을 주는지 실제 코드로 설명할 수 있다.

## 진행 방식

각 단원은 **핵심 질문 → 짧은 코드 인용 → 호출 조건과 상태 변화 → 가상 실행 예시 → 연구 관점과 확인 질문** 순서로 진행한다. 긴 함수는 의미 단위로 나눈다. 코드의 실행 동작, 주석의 의도, 해석을 구분한다.

구조체를 파일 순서대로 외우기보다, 쓰기 요청이 RU를 선택하고 채운 뒤 GC로 회수되는 흐름에 필요한 순서로 배운다. `bbssd` 밖은 PID 해석과 명령 전달 등 경계 확인에 필요한 부분만 다룬다. FDP 규격 일반론이 필요한 경우에는 별도로 원문을 확인하며, 이 자료의 구현 관찰을 규격 전체의 보장으로 확대하지 않는다.

## 단원 구성

| 순서 | 주제와 답할 질문 | 주요 코드 | 학습 결과 |
|---|---|---|---|
| 1 | 객체 관계: RG·RU·RUH·line은 어떻게 연결되는가? | `FemuReclaimGroup`, `FemuReclaimUnit`, `FemuRuHandle`, `ru_mgmt`, `line->my_ru` | 소유 관계와 쓰기 대상 포인터를 그림으로 설명 |
| 2 | 초기화: line과 RU를 구성하고 RUH에 어떻게 배정하는가? | `ssd_init_fdp_params`, `femu_fdp_init_ssd_reclaim_unit`, `femu_fdp_ssd_init_reclaim_group`, `femu_fdp_ssd_init_ru_handles` | 설정값에서 초기 객체 상태까지 추적 |
| 3 | 배치 정보: PID에서 RG와 RUH를 어떻게 선택하는가? | `ftl_thread`, `nvme_do_write_fdp`, `ssd_stream_write`, `nvme_parse_pid`, `ns->fdp.phs` | PH와 RUH ID를 구분하고 기본 배치·잘못된 PID 경로 설명 |
| 4 | 쓰기와 RU 교체: 다음 PPA와 새 RU를 어떻게 얻는가? | `fdp_get_new_ru`, `fdp_get_new_page`, `fdp_advance_ru_pointer` | RU 소진 전후 포인터·큐 변화 추적 |
| 5 | 무효화와 후보 관리: 덮어쓰기가 RU 상태에 어떻게 전파되는가? | `mark_page_valid_fdp`, `mark_page_invalid_fdp`, victim queue 콜백 | line·RU·RUH 카운터와 `pos`·`ruh_pos` 관계 설명 |
| 6 | GC와 격리: 무엇을 회수하고 어디로 옮기는가? | `should_gc_fdp_style`, `should_gc_high_fdp_style`, `select_victim_ru`, `do_gc_fdp_style`, `gc_write_page_fdp_style`, `mark_ru_free` | GC 발동·victim 선택·이동 목적지를 구분하고 II/PI 비교 |
| 7 | 관측과 초기화: 통계·이벤트·TRIM은 무엇을 기록하고 지우는가? | `nvme_do_write_fdp`, GC 통계 갱신, `ftl_fdp_alloc_event`, `ssd_trim_fdp_style` | 카운터 갱신 위치와 단위, 이벤트 조건, 초기화 범위 설명 |
| 8 | 통합 추적: 여러 배치의 쓰기·덮어쓰기·GC가 어떻게 연결되는가? | 앞선 호출 흐름 전체 | 작은 가상 workload를 추적하고 연구 변경 지점·검증 항목 정리 |

## 설명 시 지킬 기준

- 코드 인용에 파일과 줄 번호를 붙인다. 줄 번호는 위 커밋 기준이다.
- 가상 RU 번호·페이지 수·workload는 설명용으로 표시한다. 실험 결과처럼 제시하지 않는다.
- GC 전략은 enum 선언과 실제 구현된 분기를 구분한다.
- WAF 변화는 배치, victim 선택, GC 목적지, 공간 여유가 함께 작용할 수 있으므로 하나의 요인으로 단정하지 않는다.
- 읽기나 매핑의 공통 구현은 반복하지 않고 FDP 연결 지점만 설명한다.
- 한 단원 끝에 확인 질문과 짧은 답을 두어 독립적으로 복습할 수 있게 한다.

## 작성된 자료

- [1단계: FDP 객체 관계와 쓰기 포인터](01_FDP_objects_and_pointers.md)

2~8단계는 위 순서로 이어서 작성할 학습 범위다. 이 문서는 계획과 1단계의 안내이며, 전체 강의가 작성되었다는 의미는 아니다.
