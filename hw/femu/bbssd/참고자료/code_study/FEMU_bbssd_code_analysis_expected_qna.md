# FEMU bbssd 코드 분석 예상 꼬리 질문과 답변

대상 발표: `FEMU_bbssd_code_analysis_final.html`  
청중: SSD / FTL / 시스템 소프트웨어 연구자  
사용법: 발표 후 질문에는 먼저 "짧은 답변"을 말하고, 필요하면 "보충"을 이어서 설명한다.

## 1. 왜 FTL 동작의 시작점을 `bb_io_cmd()`가 아니라 `ftl_thread()`로 보았나?

**짧은 답변:**  
`bb_io_cmd()`는 bbssd 모드의 NVMe I/O entry이지만, 실제 mapping, read/write, GC, latency 계산은 queue를 지나 `ftl_thread()`에서 수행되기 때문이다.

**보충:**  
`bb_io_cmd()`는 read/write opcode를 받아 `nvme_rw()` 쪽으로 넘긴다. 이후 request는 poller와 `to_ftl` queue를 거쳐 FTL worker thread로 전달된다. 따라서 SSD 내부 동작을 분석하려면 `to_ftl` dequeue 이후 `ftl_thread()`의 opcode dispatch를 보는 것이 더 직접적이다.

## 2. `to_ftl` queue는 코드 구조상 어떤 의미인가?

**짧은 답변:**  
Host protocol 처리 path와 SSD 내부 model path를 분리하는 경계로 볼 수 있다.

**보충:**  
앞단 poller는 NVMe submission/completion 흐름을 처리하고, FTL thread는 SSD 내부 상태를 갱신한다. `to_ftl` queue는 이 둘을 연결한다. 이 구조 덕분에 host-facing NVMe path와 FTL simulation path가 분리되어 있고, request latency는 FTL 처리 후 다시 `to_poller`를 통해 completion path로 전달된다.

## 3. `maptbl`과 `rmap`이 둘 다 필요한 이유는?

**짧은 답변:**  
Host I/O는 LPN에서 시작하지만, GC는 physical page에서 시작하기 때문이다.

**보충:**  
Read/write는 Host LBA를 LPN으로 바꾼 뒤 `maptbl[lpn]`로 최신 PPA를 찾는다. 반면 GC는 victim line 안의 valid physical page를 보면서 시작한다. 이 page가 어떤 LPN의 최신 데이터인지 알아야 `maptbl`을 새 PPA로 갱신할 수 있으므로 `rmap[ppa_index] = lpn`이 필요하다.

## 4. 이 코드는 page-level mapping FTL인가?

**짧은 답변:**  
기본 read/write 경로 기준으로는 page-level mapping으로 볼 수 있다.

**보충:**  
`ssd_read()`와 `ssd_write()` 모두 LBA를 `secs_per_pg`로 나누어 LPN 범위를 계산하고, 각 LPN마다 `maptbl` entry를 조회하거나 갱신한다. 따라서 host-visible logical page 단위로 PPA가 관리된다. 다만 erase와 GC는 NAND 특성상 block/line 단위로 수행된다.

## 5. 여기서 line은 일반적인 NAND block과 같은가?

**짧은 답변:**  
완전히 같은 개념은 아니고, 여러 LUN/channel에 걸친 같은 block index 묶음인 superblock에 가까운 단위로 보면 된다.

**보충:**  
코드에서 `line.id`는 block id와 대응된다. GC의 `do_gc()`는 victim line의 block id를 기준으로 channel과 LUN을 순회하며 block들을 정리한다. 즉 하나의 line은 여러 physical block을 묶어 관리하는 단위이고, free/victim/full 상태도 line 단위로 관리된다.

## 6. 왜 GC victim을 line 단위로 선택하나?

**짧은 답변:**  
write pointer가 line 단위로 공간을 소비하고, free space도 line 단위로 회수하기 때문이다.

**보충:**  
write는 channel과 LUN을 돌며 현재 line을 채운다. line이 다 차면 full list 또는 victim priority queue로 이동한다. GC도 victim line을 고른 뒤 해당 line에 속한 block들을 erase하고 line을 free list로 되돌린다. 그래서 공간 관리의 기본 단위가 line이다.

## 7. victim line priority는 무엇을 기준으로 하나?

**짧은 답변:**  
valid page count, 즉 `vpc`를 기준으로 한다. valid page가 적은 line이 GC 비용이 낮다.

**보충:**  
`victim_line_pq`는 line의 `vpc`를 priority로 사용한다. GC 때 valid page는 새 위치로 migration해야 하지만 invalid page는 버릴 수 있다. 따라서 valid page가 적은 line을 고르면 복사해야 할 page 수가 줄어 cleaning 비용이 낮아진다.

## 8. foreground GC와 background GC의 차이는?

**짧은 답변:**  
background GC는 request 처리 후 free line이 낮을 때 수행되고, foreground GC는 write path에서 free line이 더 부족할 때 강제로 수행된다.

**보충:**  
`should_gc()`는 일반 threshold를 보고, `should_gc_high()`는 더 높은 압박 상태를 본다. `ssd_write()` 앞부분에는 `should_gc_high()`가 true인 동안 `do_gc(..., true)`를 호출하는 루프가 있다. 반면 `ftl_thread()` 끝부분의 background GC는 request 처리 후 `should_gc()`를 보고 한 번 수행된다.

## 9. GC가 write amplification을 만드는 위치는 어디인가?

**짧은 답변:**  
`clean_one_block()`에서 valid page를 읽고 `gc_write_page()`로 새 위치에 다시 쓰는 부분이다.

**보충:**  
Host가 직접 요청한 write가 아닌데도, GC가 valid page 보존을 위해 내부 write를 발생시킨다. invalid page는 버려지지만 valid page는 migration해야 한다. 이 내부 migration write가 host write 대비 media write를 증가시키므로 write amplification으로 이어진다.

## 10. `ssd_advance_status()`는 실제 데이터를 이동시키는 함수인가?

**짧은 답변:**  
아니다. 이 함수는 NAND operation의 timing state를 갱신하는 latency model에 가깝다.

**보충:**  
read/write/erase command에 따라 해당 LUN의 `next_lun_avail_time`을 갱신하고, request 시작 시간 대비 latency를 계산한다. FEMU bbssd는 실제 NAND data movement를 상세히 재현한다기보다, FTL 상태 변화와 NAND timing을 emulation한다.

## 11. 왜 request latency는 여러 page latency의 합이 아니라 max로 계산되나?

**짧은 답변:**  
여러 page operation이 서로 다른 LUN에서 병렬적으로 진행될 수 있다는 모델을 반영하기 위해서다.

**보충:**  
`ssd_read()`와 `ssd_write()`는 각 LPN에 대한 sub-latency를 계산하고 `maxlat`을 반환한다. 단순히 모든 latency를 더하면 완전 직렬 모델이 된다. max를 쓰는 방식은 여러 NAND operation 중 가장 늦게 끝나는 operation이 request completion time을 결정한다는 관점에 가깝다.

## 12. channel transfer latency는 코드에서 어떻게 반영되나?

**짧은 답변:**  
현재 기본 경로에서는 LUN latency 중심으로 계산되고, channel transfer 부분은 비활성화된 코드 블록으로 남아 있다.

**보충:**  
`ssd_advance_status()` 안에 channel transfer timing을 고려하는 코드가 `#if 0` 블록으로 존재한다. 현재 활성 경로에서는 read/write/erase 모두 LUN의 `next_lun_avail_time`을 중심으로 latency를 계산한다. 따라서 발표 범위에서는 channel bandwidth 모델보다 LUN-level timing model로 설명하는 것이 맞다.

## 13. write pointer는 어떤 순서로 전진하나?

**짧은 답변:**  
channel을 먼저 돌고, channel이 끝나면 LUN, LUN이 끝나면 page를 증가시키며 현재 line을 채운다.

**보충:**  
`ssd_advance_write_pointer()`는 `ch`를 증가시키고, 모든 channel을 돌면 `lun`을 증가시킨다. 모든 LUN을 돌면 page를 증가시킨다. block 안의 모든 page를 다 쓰면 현재 line을 full 또는 victim으로 분류하고, free line list에서 다음 line을 가져온다.

## 14. full line과 victim line의 차이는?

**짧은 답변:**  
full line은 모든 page가 아직 valid한 line이고, victim line은 invalid page가 생겨 GC 후보가 된 line이다.

**보충:**  
line을 다 썼을 때 `vpc == pgs_per_line`이면 invalid page가 없으므로 full list로 간다. 반대로 overwrite나 trim 때문에 invalid page가 생겼다면 `vpc < pgs_per_line`이고 victim priority queue로 들어간다. full line도 이후 overwrite가 발생하면 victim queue로 이동할 수 있다.

## 15. `mark_page_invalid()`에서 왜 page뿐 아니라 block과 line count도 바꾸나?

**짧은 답변:**  
GC victim 선택과 free space 관리가 block/line 단위 count에 의존하기 때문이다.

**보충:**  
page 하나가 invalid가 되면 해당 block의 `ipc`는 증가하고 `vpc`는 감소한다. 같은 변화가 line에도 반영된다. 이 count가 누적되어 victim line의 우선순위가 바뀌고, line이 full list에서 victim queue로 이동할 수 있다.

## 16. read path에서 unmapped LPN을 skip하는 이유는?

**짧은 답변:**  
아직 write된 적이 없거나 trim된 논리 주소는 유효한 physical page가 없기 때문이다.

**보충:**  
`ssd_read()`는 `maptbl[lpn]`을 확인한 뒤 unmapped PPA이거나 valid PPA가 아니면 NAND read latency 계산을 하지 않고 넘어간다. 이 코드는 데이터 내용 자체보다 FTL mapping과 timing 모델에 집중하므로, unmapped read에 대한 별도 data return 처리는 이 함수의 핵심이 아니다.

## 17. trim은 발표에서 깊게 다루지 않았는데, write/GC와 어떤 관계인가?

**짧은 답변:**  
trim은 mapping을 해제하고 기존 PPA를 invalid로 만들어 GC가 회수할 수 있는 공간을 늘린다.

**보충:**  
`ssd_trim()`은 DSM range를 LPN 범위로 바꾼 뒤 mapped PPA가 있으면 `mark_page_invalid()`를 호출하고, `rmap`과 `maptbl`을 지운다. Host가 더 이상 필요 없는 데이터를 알려주는 경로라서, 이후 GC 효율에 영향을 준다.

## 18. 실제 SSD와 FEMU bbssd 모델의 차이는 무엇이라고 봐야 하나?

**짧은 답변:**  
FEMU bbssd는 실제 SSD firmware 전체라기보다 FTL state transition과 NAND timing을 단순화해 emulation하는 모델이다.

**보충:**  
실제 SSD에는 DRAM cache, wear leveling, bad block management, read disturb, program interference, multi-plane command, scheduler 등 더 많은 요소가 있다. bbssd 기본 경로는 page-level mapping, line-level GC, LUN-level latency를 중심으로 한 교육적이고 실험 가능한 모델로 보는 것이 적절하다.

## 19. 이 분석에서 FDP 경로를 제외한 이유는?

**짧은 답변:**  
기본 read/write/GC 흐름을 먼저 이해하는 것이 목적이었고, FDP는 placement와 reclaim unit 관리가 추가된 별도 확장 경로이기 때문이다.

**보충:**  
코드에는 `ssd->fdp_enabled` 분기가 있고, FDP가 켜지면 stream write, reclaim group, reclaim unit, RU handle 기반의 다른 allocation/GC 경로를 탄다. 이번 발표는 bbssd 기본 FTL 흐름을 설명하는 것이므로 non-FDP path에 집중했다.

## 20. 박사님이 “그래서 이 코드에서 성능 실험을 하면 무엇을 조심해야 하나?”라고 물으면?

**짧은 답변:**  
latency 모델이 LUN available time 중심이고, 일부 실제 SSD 요소는 단순화되어 있으므로 절대 성능보다 경향 분석에 더 적합하다고 답할 수 있다.

**보충:**  
예를 들어 GC threshold, line utilization, overwrite 비율, read/write mix 변화에 따른 latency/WAF 경향을 보는 데는 유용하다. 하지만 실제 장치 수준의 channel scheduling, firmware cache, parallelism, media variation까지 정밀하게 반영한다고 해석하면 과도하다. 실험 결과를 해석할 때 어떤 timing path가 코드에서 활성화되어 있는지 먼저 확인해야 한다.

