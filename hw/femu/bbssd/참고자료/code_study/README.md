# bbssd 코드 공부 노트 — 목차

FEMU `hw/femu/bbssd/`(BlackBox SSD FTL)를 공부하며 정리한 노트다.
`study` 브랜치 = upstream `master` + 한국어 학습 주석 + 이 폴더.

## 읽는 순서

| 순서 | 문서 | 다루는 코드 | 한 줄 요약 |
|---|---|---|---|
| 1 | [11_core_structures_and_dispatch.md](11_core_structures_and_dispatch.md) | `ftl.h`, `ftl-internal.h`, `ftl.c`, `bb.c` | 자료구조, `ppa` 비트 구성, NVMe 명령 → FTL thread → 완료 흐름 |
| 2 | [12_datapath_read_write_trim.md](12_datapath_read_write_trim.md) | `ftl-datapath.c`, `ftl-cache.c` | host read / write / trim, write buffer, read cache, 지연 계산 |
| 3 | [13_line_gc_geometry_media.md](13_line_gc_geometry_media.md) | `ftl-line-gc.c`, `ftl-geom.c`, `ftl-media.c` | geometry, line 리스트, write pointer, GC와 victim 정책, NAND 지연 모델 |
| 4 | [14_mapping_schemes.md](14_mapping_schemes.md) | `ftl-map*.c`, `ftl-exp.c`, `ftl-query.c` | page / dftl / hybrid(BAST) / fast(FAST), `hot_cold_sep` |
| 5 | [15_fdp_code_map.md](15_fdp_code_map.md) | `ftl-fdp.c` | FDP 함수 지도, 예전 FDP 노트(01~08)와 연결, 바뀐 점 |

## FDP 공부 (파일시스템 placement 과제)

[20_FDP_study_plan_fs.md](20_FDP_study_plan_fs.md)부터 시작한다. 0~6단계로 나눠 파라미터 → PH ↔ RUHID → 명령 → GC 정책 순서로 본다. 01~08은 각 단계의 "더 보기"로 쓴다. 그림으로 공부할 계획은 [21_FDP_image_plan.md](21_FDP_image_plan.md).

그림 + 대화식 쉬운 설명 (단계별):
- [30 · 0단계 큰 그림](30_FDP_0단계_큰그림.md)
- [32 · 2단계 write 하나가 RU에 닿기까지](32_FDP_2단계_PID에서_RU까지.md)

## 예전 노트 (커밋 `e2d5413ff`, 단일 `ftl.c` 기준)

코드 참조는 최신 위치로 갱신했고, 바뀐 동작은 각 노트 안에 `⚠ 최신 코드:`로 표시했다.

| 문서 | 내용 |
|---|---|
| [00_FDP_study_plan.md](00_FDP_study_plan.md) | FDP 학습 계획 |
| [01](01_FDP_objects_and_pointers.md) · [02](02_FDP_initialization.md) · [03](03_FDP_placement_resolution.md) · [04](04_FDP_write_and_RU_rotation.md) | FDP 객체, 초기화, placement 해석, write와 RU 교체 |
| [05](05_FDP_invalidation_and_queues.md) · [06](06_FDP_GC_and_isolation.md) · [07](07_FDP_statistics_events_and_trim.md) · [08](08_FDP_integrated_walkthrough.md) | invalidation과 queue, GC와 격리, 통계·event·trim, 통합 워크스루 |
| [bbssd_code_walkthrough.md](bbssd_code_walkthrough.md) | 예전 bbssd 전체 워크스루 (지금은 11~14가 최신) |
| [FEMU_bbssd_code_analysis_expected_qna.md](FEMU_bbssd_code_analysis_expected_qna.md) | 발표 예상 질문과 답변 |

## 코드 안 주석 읽는 법

- 파일 맨 위 한국어 블록: 파일 역할, 주요 진입 함수, 의존 관계
- 함수 바로 위 블록: `호출:` (실제 호출하는 곳), `흐름:` ①②③, `반환:`
- 함수 안 `/* ② ... */`: 위 `흐름:`의 각 단계가 시작되는 지점
- 영어 주석은 upstream 원문 그대로다. 한국어 주석은 모두 새 줄로만 추가했다.

## upstream 최신화

```bash
git fetch origin
git switch master && git merge --ff-only origin/master && git push target master
git switch study && git rebase master   # 충돌은 대부분 주석 줄 주변에서만 난다
```
