# 7단계 — FDP 통계, 이벤트, RUH Update, TRIM

기준: `master`, 커밋 `e2d5413ff`. 현재 코드의 계측 의미를 설명한다. 문서의 수치 예시는 가상 계산이며 장치 측정값이 아니다.

이전: [6단계 — GC와 격리](06_FDP_GC_and_isolation.md) · 다음: [8단계 — 통합 실행 추적과 연구 검증](08_FDP_integrated_walkthrough.md)

## 1. 이번 단원의 질문

**어떤 카운터와 이벤트가 FDP 동작을 보여 주며, TRIM 이후 무엇이 실제로 초기 상태가 되는가?**

계측 필드의 이름만 읽지 않고 **갱신 위치, 단위, 귀속 대상, 갱신 시점, 초기화 범위**를 함께 본다. 이 다섯 가지가 맞아야 WAF나 격리 효과를 해석할 수 있다.

| 관측값 | 현재 구현에서 확인할 의미 |
|---|---|
| `hbmw` | 호스트 요청의 바이트 수 누적 |
| `mbmw` | 호스트 요청 바이트 + GC로 옮긴 페이지의 바이트 누적 |
| `mbe` | GC에서 센 block 수를 block 용량으로 변환한 바이트 누적 |
| `ruamw` | NVMe RU의 가용 쓰기량 필드. FTL 물리 잔여 페이지 수와의 단위·갱신 차이를 확인 |
| `ru_in_use_cnt`, `ruh_live_pages_cnt` | RUH의 배정 RU 수와 유효 페이지 집계 |
| FDP event | 특정 코드 분기가 filter 조건을 만족해 남긴 레코드 |
| `FDP_TRACE` | 현재 활성화된 trace 호출의 텍스트 출력 |

## 2. 호스트 쓰기 통계는 실제 배치보다 앞서 증가한다

출처: [bbssd/ftl.c:2149–2152](/root/workspace/FEMU/hw/femu/bbssd/ftl.c:2149)

```c
    /* update FDP host bytes written stats */
    data_bytes = (uint64_t)nlb * spp->secsz;
    nvme_fdp_stat_inc(&ns->endgrp->fdp.hbmw, data_bytes);
    nvme_fdp_stat_inc(&ns->endgrp->fdp.mbmw, data_bytes);
```

`data_bytes = nlb × secsz`를 endurance group의 hbmw와 mbmw 양쪽에 더한다. 이어서 PH가 선택한 RUH의 FTL/NVMe 카운터도 양쪽 모두 증가시킨다.

출처: [bbssd/ftl.c:2164–2170](/root/workspace/FEMU/hw/femu/bbssd/ftl.c:2164)

```c
    ruhid = ns->fdp.phs[ph];
    nvme_fdp_stat_inc(&ssd->ruhs[ruhid].hbmw, data_bytes);
    nvme_fdp_stat_inc(&ssd->ruhs[ruhid].ruh->hbmw, data_bytes);
    nvme_fdp_stat_inc(&ssd->ruhs[ruhid].mbmw, data_bytes);
    nvme_fdp_stat_inc(&ssd->ruhs[ruhid].ruh->mbmw, data_bytes);

    return ssd_stream_write(n, ssd, req);
```

이 증가는 `ssd_stream_write()` 호출 **전**이다. 따라서 RU 부족으로 실제 페이지 배치가 완성되지 않은 경로에서도 이미 요청 전체가 계상될 수 있다. 성공 완료와 실제 배치 확인 없이 이 값을 완료된 NAND 프로그램 바이트라고 단정하면 안 된다.

또한 호스트 경로의 mbmw는 실제로 진행한 페이지 수를 따로 세는 것이 아니라 요청 바이트를 바로 더한다. namespace LBA 크기와 `secsz`, 요청의 페이지 정렬 조건이 맞는지 확인해야 한다.

## 3. GC 통계는 이동 페이지 수와 순회 block 수에서 나온다

출처: [bbssd/ftl.c:1937–1950](/root/workspace/FEMU/hw/femu/bbssd/ftl.c:1937)

```c
    /* update FDP statistics: media bytes written (GC writes) */
    uint64_t gc_bytes = (uint64_t)vpc_cnt * spp->secsz * spp->secs_per_pg;
    uint64_t erase_bytes = (uint64_t)blk_cnt * spp->secsz * spp->secs_per_pg
                           * spp->pgs_per_blk;

    FDP_TRACE(ssd, "GC_DONE victim_ru=%u pages_migrated=%d "
              "blocks_erased=%d mbmw_delta=%lu mbe_delta=%lu\n",
              victim_ru->ruidx, vpc_cnt, blk_cnt, gc_bytes, erase_bytes);
    nvme_fdp_stat_inc(&ssd->n->subsys->endgrp.fdp.mbmw, gc_bytes);
    nvme_fdp_stat_inc(&victim_ru->ruh->mbmw, gc_bytes);
    nvme_fdp_stat_inc(&victim_ru->ruh->ruh->mbmw, gc_bytes);
    nvme_fdp_stat_inc(&ssd->n->subsys->endgrp.fdp.mbe, erase_bytes);
    nvme_fdp_stat_inc(&victim_ru->ruh->mbe, erase_bytes);
    nvme_fdp_stat_inc(&victim_ru->ruh->ruh->mbe, erase_bytes);
```

`vpc_cnt`는 GC가 실제로 valid 페이지 이동 함수를 호출한 수이고, `blk_cnt`는 victim을 순회한 block 수다. 한 FTL 페이지 바이트를 P라고 두면:

```text
P = secsz × secs_per_pg
GC 쓰기 바이트 = vpc_cnt × P
GC erase 바이트 = blk_cnt × pgs_per_blk × P
```

집계는 `enable_gc_delay` 조건문 밖에 있다. 지연 모델을 껐더라도 이 코드 경로를 거치면 GC 바이트 수는 증가한다.

### 3.1 RUH별 mbmw/mbe는 source에 귀속된다

대입 대상은 `victim_ru->ruh`다. PI에서는 source와 destination RUH가 같지만, 서로 다른 RUH로 이동하는 II 경로를 구성했다면 GC 쓰기 바이트는 **목적 RUH가 아니라 source RUH**에 귀속된다.

따라서 RUH별 mbmw를 그 RUH의 물리 목적 RU에 실제로 도착한 모든 바이트라고 해석하면 안 된다. 이 구현에서는 “그 RUH의 데이터를 정리하면서 발생한 GC 비용”에 가까운 집계다. 반면 목적 RUH의 live page 수는 valid 처리에서 증가한다. 비용 귀속과 데이터 소속 집계가 서로 다르다.

### 3.2 saturation과 로그 표현

출처: [nvme.h:302–306](/root/workspace/FEMU/hw/femu/nvme.h:302)

```c
static inline void nvme_fdp_stat_inc(uint64_t *a, uint64_t b)
{
    uint64_t ret = *a + b;
    *a = ret < *a ? UINT64_MAX : ret;
}
```

덧셈 overflow가 발생하면 `UINT64_MAX`로 포화시킨다. 포화 후의 차분은 실제 쓰기량을 나타내지 못한다.

출처: [nvme-admin.c:1119–1127](/root/workspace/FEMU/hw/femu/nvme-admin.c:1119)

```c
    endgrp = &n->subsys->endgrp;
    trans_len = MIN(sizeof(log) - off, buf_len);

    /* spec value is 128 bit, we only use low 64 bit */
    log.hbmw[0] = cpu_to_le64(endgrp->fdp.hbmw);
    log.mbmw[0] = cpu_to_le64(endgrp->fdp.mbmw);
    log.mbe[0] = cpu_to_le64(endgrp->fdp.mbe);

    return dma_read_prp(n, (uint8_t *)&log + off, trans_len, prp1, prp2);
```

로그 구조는 두 개의 64비트 원소로 각 값을 표현하지만 현재 출력 코드는 하위 원소만 채운다. 상위 원소는 zero 초기화된 상태다. 내부 카운터가 128비트 정밀도로 증가하는 구현이라고 해석하지 않는다.

RUH 사용량 로그는 NVMe RUH의 hbmw/mbmw를 읽는다(`nvme-admin.c:1086–1093`). FTL RUH와 NVMe RUH에 중복 보관된 통계가 다른 경로에서도 일치하는지 확인할 이유가 여기에 있다.

## 4. 이 구현의 WAF를 어떻게 계산하고 해석할 것인가?

정상 완료, 카운터 미포화, 측정 중 reset 없음, 같은 측정 구간을 전제로 다음과 같이 계산할 수 있다.

```text
ΔH = 종료 hbmw - 시작 hbmw
ΔM = 종료 mbmw - 시작 mbmw
카운터 기반 WAF = ΔM / ΔH    (ΔH > 0)
GC 바이트 = ΔM - ΔH
```

**가상 계산:** 호스트 요청 1,024 KiB와 GC 이동 256 KiB가 계상되면 ΔH=1,024 KiB, ΔM=1,280 KiB, WAF=1.25다. 실제 workload 측정 결과가 아니다.

해석의 조건을 구분한다.

- 이 값은 현재 코드가 계상한 바이트의 비율이다. 호스트 mbmw 증가가 실제 NAND 페이지 프로그램 수의 직접 계측은 아니다.
- 부분 페이지 쓰기나 단위 불일치가 있으면 요청 바이트와 FTL 페이지 배치량이 달라질 수 있다.
- 공간 소진·요청 실패·부분 처리 경로가 있으면 선증가된 통계가 포함될 수 있다.
- GC delay 옵션은 지연 비용을 바꾸며 통계 증가 자체를 끄는 옵션이 아니다.
- 초기화·preconditioning 이후의 측정 구간을 구분해야 한다. 누적값 전체의 비율과 구간 차분 비율은 다르다.

`ru_mgmt`의 `waf_score_global`, `waf_score_transitory`, `is_gc_triggered` 같은 필드는 이 checkout에서 초기값 설정 외에 유효한 갱신 경로가 확인되지 않는다(`ftl.c:2191–2195`). 이름에 WAF가 있다는 이유로 실제 runtime WAF 결과로 사용하면 안 된다.

## 5. erase를 세는 세 가지 값은 같지 않다

공통 block 초기화 함수의 FDP 관련 효과만 확인한다.

출처: [bbssd/ftl.c:748–752](/root/workspace/FEMU/hw/femu/bbssd/ftl.c:748)

```c
    /* reset block status */
    ftl_assert(blk->npgs == spp->pgs_per_blk);
    blk->ipc = 0;
    blk->vpc = 0;
    blk->erase_cnt++;
```

`mark_block_free()` 호출마다 block의 erase 카운터가 증가한다. 그런데 FDP GC 본문은 block당 이 함수를 호출하고(`ftl.c:1924`), 마지막 `mark_ru_free()`도 같은 RU의 block들을 다시 순회하며 호출한다(`ftl.c:1791–1802`).

| 값 | 한 번의 정상 victim 회수에서의 동작 |
|---|---|
| `blk->erase_cnt` | 위 두 경로로 해당 block당 2회 증가 |
| `mbe` | 본문에서 센 `blk_cnt × block 용량`을 한 번 더함 |
| `ru->erase_cnt` | `mark_ru_free()`에서 1 증가하나 새 RU 배정 시 0으로 재설정 |

NAND erase 지연 호출은 GC 본문의 옵션 분기에 있으며 `mark_ru_free()`에는 추가 NAND erase 지연 호출이 없다. 따라서 **block erase counter 증가 횟수, 모델에 넣은 erase 명령 횟수, mbe 바이트**를 동일시하면 안 된다.

RU의 `erase_cnt`는 `fdp_get_new_ru()`에서 0으로 설정된다(`ftl.c:1139`). 물리 RU의 생애 누적 마모 횟수라고 해석하기 어렵다. TRIM도 block 초기화 함수를 사용하므로 erase 집계와 함께 보아야 한다.

## 6. RUAMW: 초기 단위와 감소 단위의 차이

출처: [femu.c:220–230](/root/workspace/FEMU/hw/femu/femu.c:220)

```c
        if (ruh->ruha == NVME_RUHA_UNUSED) {
            ruh->ruha = NVME_RUHA_HOST;
            ruh->lbafi = lbafi;
            ruh->ruamw = endgrp->fdp.runs >> ns->lbaf.lbads;
            ruh->hbmw = 0;
            ruh->mbmw = 0;
            ruh->mbe = 0;

            for (uint16_t rg = 0; rg < endgrp->fdp.nrg; rg++) {
                for (uint64_t j = 0; j < endgrp->fdp.nru; j++) {
                    endgrp->fdp.rus[rg][j].ruamw = ruh->ruamw;
```

초기값은 `runs >> lbads`다. 코드식은 runs를 namespace의 LBA 바이트 크기로 나눈 개수로 읽힌다. 그러나 FTL 호스트 쓰기의 감소는 다음과 같다.

출처: [bbssd/ftl.c:2107–2110](/root/workspace/FEMU/hw/femu/bbssd/ftl.c:2107)

```c
        /* decrement ruamw for this RU */
        if (ru->nvme_ru && ru->nvme_ru->ruamw > 0) {
            ru->nvme_ru->ruamw--; //TODO ? Does this really decrements page KiB?
        }
```

페이지 루프 한 번에 1 줄인다. namespace LBA가 512 B이고 FTL 페이지가 4 KiB인 가상 설정에서는 한 페이지 쓰기가 8 LBA에 해당하지만 이 감소는 1이다. 따라서 RUAMW가 실제 잔여 LBA 수 또는 잔여 FTL 페이지 수와 항상 일치한다고 가정하면 안 된다. `runs`와 실제 RU geometry의 연결도 2단계에서 본 것처럼 자동 계산되지 않는다.

GC 페이지 쓰기 함수에는 동일한 ruamw 감소가 없으며, RU 반환에서는 초기값으로 복원한다(`ftl.c:1818–1822`). 이 쓰기 경로에서 ruamw=0은 물리 RU 교체를 직접 트리거하지도 않는다.

### 6.1 RUH Status는 어느 포인터를 읽는가?

출처: [nvme-io.c:926–937](/root/workspace/FEMU/hw/femu/nvme-io.c:926)

```c
    ruhid = ns->fdp.phs;
    for (ph = 0; ph < ns->fdp.nphs; ph++, ruhid++) {
        NvmeRuHandle *ruh = &endgrp->fdp.ruhs[*ruhid];

        for (rg = 0; rg < endgrp->fdp.nrg; rg++, ruhsd++) {
            uint16_t pid = nvme_make_pid(ns, rg, ph);
            NvmeReclaimUnit *ru = ruh->rus[rg];
            ruhsd->pid = cpu_to_le16(pid);
            ruhsd->ruhid = *ruhid;
            ruhsd->earutr = 0;
            ruhsd->ruamw = cpu_to_le64(ru->ruamw);
        }
```

Status는 NVMe RUH의 `rus[rg]`가 가리키는 RU의 ruamw를 읽는다. FTL의 `curr_ru`를 직접 읽는 함수가 아니다. PI GC RU 할당도 NVMe 측 참조를 갱신할 수 있다는 6단계 내용과 연결된다. 호스트 Status와 FTL의 활성 RU를 비교할 때는 이 포인터 차이까지 확인해야 한다.

### 6.2 RUH Update가 물리 RU 교체를 수행하는가?

명령 경계는 `nvme_io_mgmt_send_ruh_update()` → `nvme_update_ruh()`다(`nvme-io.c:990–993`). 핵심 실행문은 다음과 같다.

출처: [nvme-io.c:864–882](/root/workspace/FEMU/hw/femu/nvme-io.c:864)

```c
    ruhid = ns->fdp.phs[ph];
    ruh = &endgrp->fdp.ruhs[ruhid];
    ru = ruh->rus[rg];

    if (ru->ruamw) {
        if (log_event(ruh, FDP_EVT_RU_NOT_FULLY_WRITTEN)) {
            e = nvme_fdp_alloc_event(n, &endgrp->fdp.host_events);
            e->type = FDP_EVT_RU_NOT_FULLY_WRITTEN;
            e->flags = FDPEF_PIV | FDPEF_NSIDV | FDPEF_LV;
            e->pid = cpu_to_le16(pid);
            e->nsid = cpu_to_le32(ns->id);
            e->rgid = cpu_to_le16(rg);
            e->ruhid = cpu_to_le16(ruhid);
        }
    }

    ru->ruamw = ruh->ruamw;

    return true;
```

남은 ruamw가 있으면 filter에 따라 이벤트를 만들고, ruamw를 RUH의 초기값으로 복원한다. 이 함수에는 FTL의 `fdp_get_new_ru()`나 `fdp_advance_ru_pointer()` 호출이 없다. 따라서 이 명령이 현재 bbssd의 물리 배치를 새 RU로 회전시킨다고 설명하면 안 된다. 규격상 의미와의 비교는 별도의 규격 검토 대상이며, 여기서는 구현된 동작을 설명한다.

## 7. 이벤트: 어떤 분기가 남기는 레코드인가?

| 발생 지점 | 이벤트 | 버퍼·조건 |
|---|---|---|
| `ssd_stream_write`, `ftl.c:2005–2017` | `INVALID_PID` | 배치 DTYPE으로 시도한 PID가 무효이고 기본 PH의 RUH filter가 허용할 때 host 버퍼 |
| `do_gc_fdp_style` | `RUH_IMPLICIT_RU_CHANGE` | victim RUH filter가 허용할 때 controller 버퍼 |
| `nvme_update_ruh` | `RU_NOT_FULLY_WRITTEN` | ruamw가 남아 있고 filter가 허용할 때 host 버퍼 |

GC 이벤트의 실제 내용은 다음과 같다.

출처: [bbssd/ftl.c:1957–1970](/root/workspace/FEMU/hw/femu/bbssd/ftl.c:1957)

```c
    /* generate controller event for RU change due to GC */
    if (ssd->n->subsys) {
        NvmeEnduranceGroup *endgrp = &ssd->n->subsys->endgrp;
        NvmeRuHandle *nvme_ruh = victim_ru->ruh->ruh;
        if (nvme_ruh &&
            (nvme_ruh->event_filter >>
             nvme_fdp_evf_shifts[FDP_EVT_RUH_IMPLICIT_RU_CHANGE]) & 0x1) {
            NvmeFdpEvent *e = ftl_fdp_alloc_event(ssd,
                                    &endgrp->fdp.ctrl_events);
            e->type = FDP_EVT_RUH_IMPLICIT_RU_CHANGE;
            e->flags = FDPEF_LV;
            e->rgid = cpu_to_le16(rgid);
            e->ruhid = victim_ru->ruh->ruhid;
        }
```

GC 이벤트는 source RUH 정보와 호출 RG를 기록한다. 모든 host RU 교체가 이 이벤트와 일대일로 대응하는 것은 아니다. 일반적인 host RU 소진 시의 `fdp_advance_ru_pointer()`에는 이 이벤트 생성이 없다. enum에 존재하는 다른 이벤트도 실제 생성 호출을 찾은 뒤 설명해야 한다.

### 7.1 FTL 이벤트 버퍼는 원형이며 timestamp를 자동 설정하지 않는다

출처: [bbssd/ftl.c:27–41](/root/workspace/FEMU/hw/femu/bbssd/ftl.c:27)

```c
    NvmeFdpEvent *ret;
    bool is_full = ebuf->next == ebuf->start && ebuf->nelems;

    ret = &ebuf->events[ebuf->next++];
    if (unlikely(ebuf->next == NVME_FDP_MAX_EVENTS)) {
        ebuf->next = 0;
    }
    if (is_full) {
        ebuf->start = ebuf->next;
    } else {
        ebuf->nelems++;
    }

    memset(ret, 0, sizeof(NvmeFdpEvent));
    return ret;
```

`next`가 끝에 도달하면 0으로 돌아가며, 가득 찼을 때는 가장 오래된 위치인 `start`도 이동한다. 따라서 버퍼의 현재 이벤트 수는 장치 실행 전체의 누적 발생 횟수와 다르다.

마지막 `memset` 뒤 FTL allocator는 timestamp를 채우지 않는다. 위 GC/INVALID_PID 생성부도 timestamp를 설정하지 않으므로 이 경로의 이벤트 timestamp는 0으로 남는다. 반면 NVMe 측 allocator는 timestamp를 기록한다(`nvme-io.c:846–847`). 두 경로의 이벤트 시간을 같은 의미로 사용하면 안 된다.

이벤트 로그 읽기는 host/controller 버퍼를 선택해 내용을 복사한다(`nvme-admin.c:1152–1175`). 해당 읽기 함수에는 버퍼를 비우는 동작이 없다. 같은 로그를 반복해서 읽은 결과를 단순 합산하면 중복을 세게 된다.

## 8. trace와 카운터의 차이

출처: [bbssd/ftl.h:355–359](/root/workspace/FEMU/hw/femu/bbssd/ftl.h:355)

```c
/* FDP conditional trace: only emits when ssd->fdp_debug is set */
#define FDP_TRACE(ssd, fmt, ...) do { \
    if ((ssd)->fdp_debug) \
        fprintf(stderr, "[FEMU] FDP-Trace: " fmt, ## __VA_ARGS__); \
} while (0)
```

`FEMU_FDP_DEBUG` 환경변수의 존재 여부가 `ssd->fdp_debug`를 설정한다(`ftl.c:512`). 실제로 살아 있는 trace에는 RU_ROTATE, GC_START, GC_DONE, background 보류 등이 있다. WRITE/INVAL/GC_MIGRATE 등 일부 trace 호출은 주석 처리되어 있으므로 환경변수만으로 나오지 않는다.

GC_DONE trace는 통계 대입 및 `mark_ru_free()`보다 앞에서 출력된다(`ftl.c:1942–1945`). 따라서 그 줄이 보였다는 사실만으로 마지막 free 목록 복구까지 모두 성공했다고 단정하지 않는다. 이벤트 buffer, trace 텍스트, 최종 카운터는 서로 보완하는 관측값이다.

## 9. FDP TRIM의 입력 범위와 실제 처리 범위

NVMe DSM 수신부는 range들을 `req->dsm_ranges`에 보관한다(`nvme-io.c:641–645`). 하지만 FTL의 FDP DSM 분기는 다음 함수를 호출한다.

출처: [bbssd/ftl.c:2573–2576](/root/workspace/FEMU/hw/femu/bbssd/ftl.c:2573)

```c
            case NVME_CMD_DSM:
                if (ssd->fdp_enabled) {
                    ssd_trim_fdp_style(n, req, req->slba, req->nlb);
                    lat = 0;
```

`ssd_trim_fdp_style()`는 `slba`, `nlb`를 인자로 받지만 함수 본문에서 그 범위나 `req->dsm_ranges`를 사용해 제한하지 않는다. 모든 channel/LUN/block을 순회하며 상태를 free로 만든다.

출처: [bbssd/ftl.c:2444–2463](/root/workspace/FEMU/hw/femu/bbssd/ftl.c:2444)

```c
    /* erase all blocks */
    for (int ch = 0; ch < spp->nchs; ch++) {
        for (int lun = 0; lun < spp->luns_per_ch; lun++) {
            for (int blk = 0; blk < spp->blks_per_pl; blk++) {
                ppa.g.ch = ch;
                ppa.g.lun = lun;
                ppa.g.pl = 0;
                ppa.g.blk = blk;
                lunp = get_lun(ssd, &ppa);
                mark_block_free(ssd, &ppa);
                if (spp->enable_gc_delay) {
                    struct nand_cmd gce;
                    gce.type = GC_IO;
                    gce.cmd = NAND_ERASE;
                    gce.stime = 0;
                    ssd_advance_status(ssd, &ppa, &gce);
                }
                lunp->gc_endtime = lunp->next_lun_avail_time;
            }
        }
```

따라서 이 함수에 도달한 FDP DSM은 **FTL 전체 매핑과 물리 상태를 초기화하는 동작**으로 읽어야 한다. non-FDP의 범위 무효화 동작을 여기에도 그대로 적용하면 안 된다. 이 문서에서는 DSM 실행을 요청하거나 장치를 초기화하지 않았다.

NAND erase 지연을 내부 자원 일정에 반영할 수 있지만 caller는 `lat=0`으로 설정한다. 즉 내부 erase 일정과 이 요청에 반환되는 직접 지연을 구분해야 한다.

## 10. TRIM의 RU 큐와 활성 RU 처리

출처: [bbssd/ftl.c:2466–2479](/root/workspace/FEMU/hw/femu/bbssd/ftl.c:2466)

```c
    /* drain victim and full RU queues for all reclaim groups */
    for (rg_idx = 0; rg_idx < (int)ssd->nrg; rg_idx++) {
        struct ru_mgmt *rm = ssd->rg[rg_idx].ru_mgmt;
        while ((v_ru = pqueue_peek(rm->victim_ru_pq)) != NULL) {
            pqueue_remove(rm->victim_ru_pq, v_ru);
            rm->victim_ru_cnt--;
            mark_ru_free(ssd, v_ru->rgidx, v_ru);
        }
        while ((v_ru = QTAILQ_FIRST(&rm->full_ru_list)) != NULL) {
            QTAILQ_REMOVE(&rm->full_ru_list, v_ru, entry);
            rm->full_ru_cnt--;
            mark_ru_free(ssd, v_ru->rgidx, v_ru);
        }
    }
```

RG의 `victim_ru_pq`와 full 목록을 비우며 각 RU를 free로 만든다. RUH별 vpc heap도 비우고 `ruh_pos`와 카운터를 정리한다(`ftl.c:2491–2501`). 여기의 “free”는 객체 메모리 해제가 아니라 재사용 목록으로 반환한다는 의미다.

이어 RUH 통계를 0으로 만들고 현재 host RU를 반환한 뒤 RG마다 새 host RU를 할당한다.

출처: [bbssd/ftl.c:2503–2521](/root/workspace/FEMU/hw/femu/bbssd/ftl.c:2503)

```c
        ruh->hbmw = 0;
        ruh->mbmw = 0;
        ruh->mbe = 0;
        ssd->ruhs[i].hbmw = 0;
        ssd->ruhs[i].mbmw = 0;
        ssd->ruhs[i].mbe = 0;
        if (ssd->ruhs[i].curr_ru) {
            mark_ru_free(ssd, ssd->ruhs[i].curr_ru->rgidx,
                         ssd->ruhs[i].curr_ru);
        }
        ssd->ruhs[i].curr_ru = NULL;
        for (rg_idx = 0; rg_idx < (int)ssd->nrg; rg_idx++) {
            ssd->ruhs[i].rus[rg_idx] =
                fdp_get_new_ru(ssd, rg_idx, ssd->ruhs[i].ruhid);
            ssd->ruhs[i].ruh->rus[rg_idx] =
                ssd->ruhs[i].rus[rg_idx]->nvme_ru;
        }
        /* primary RG (index 0) is the active one */
        ssd->ruhs[i].curr_ru = ssd->ruhs[i].rus[0];
```

마지막으로 전체 maptbl/rmap과 endurance group 통계를 초기화한다.

출처: [bbssd/ftl.c:2524–2530](/root/workspace/FEMU/hw/femu/bbssd/ftl.c:2524)

```c
    ssd_reset_maptbl(ssd);

    endgrp->fdp.hbmw = 0;
    endgrp->fdp.mbmw = 0;
    endgrp->fdp.mbe = 0;

    ftl_log("FDP TRIM: all RUs reset\n");
```

`ssd_reset_maptbl()`은 모든 LPN을 unmapped, rmap을 INVALID_LPN으로 만든다(`ftl.c:2419–2426`). 부분 범위 reset이 아니다.

### 10.1 “all RUs reset” 로그와 실제 초기화 범위는 구분한다

| 항목 | 현재 실행문에서 확인한 상태 |
|---|---|
| 모든 block의 page status | 전체 순회로 free 처리 |
| maptbl/rmap | 전체 초기화 |
| EG 및 FTL/NVMe RUH의 hbmw/mbmw/mbe | 0으로 설정 |
| RG의 vpc victim heap 및 full 목록 | 비우고 free 반환 |
| RG의 `victim_ru_cb` | 이를 비우는 실행문이 없음 |
| RUH의 활성 `gc_ru` | 반환·NULL 설정·write pointer reset을 명시적으로 수행하지 않음 |
| `ru_in_use_cnt`, `ruh_live_pages_cnt` | 0으로 재설정하는 실행문이 없음; 새 할당은 RU 수를 다시 증가시킴 |
| host `rus[]` / `curr_ru` | 각 RG에서 새 RU를 배정하고 `curr_ru=rus[0]` 설정 |
| 다중 RG의 이전 host RU | 단일 `curr_ru`만 명시적으로 반환하므로 다른 RG 참조의 이전 RU 정리 확인 필요 |
| 이벤트 버퍼 | 비우는 실행문이 없음 |
| block erase count와 NAND 시간 상태 | fresh 장치의 0 상태로 되돌리는 구현이 아님 |

CB 후보가 남거나 활성 GC RU 포인터와 카운터가 남는 경우, TRIM 뒤 상태를 새 프로세스 초기화와 동일하다고 볼 수 없다. 페이지 상태만 전체 free가 된 것과 RU 관리 메타데이터가 완전히 초기화된 것은 다르다. 이런 비대칭이 실제 후속 동작에 주는 영향은 별도의 검증 대상이다.

## 11. 측정 자료를 해석하는 순서

1. checkout·geometry·RG/RUH 수·유형·GC 전략을 고정한다.
2. 측정값이 EG인지 RUH인지, 누적인지 구간 차분인지 명시한다.
3. 요청 성공, 물리 배치, 카운터 증가의 시점 차이를 확인한다.
4. GC source 비용과 destination 데이터 소속을 구분한다.
5. 측정 사이에 TRIM이 있었다면 위 reset 범위를 확인한다.
6. trace와 이벤트만으로 단정하지 않고 최종 mapping/큐/카운터 관계를 함께 본다.

## 12. 확인 질문과 답

1. **mbmw는 매 NAND 페이지 쓰기를 직접 센 값인가?**  
   호스트 쪽은 요청 바이트를 선증가하고 GC 쪽은 이동 페이지 수에서 바이트를 계산한다.
2. **GC 목적 RUH가 바뀌면 GC 비용도 목적 RUH에 더하는가?**  
   현재 코드는 victim/source RUH에 더한다.
3. **block erase count가 2 늘면 mbe도 두 배로 증가하는가?**  
   이 FDP GC에서는 block reset 중복과 mbe 집계가 다른 경로이므로 그렇지 않다.
4. **RUH Update가 새 FTL RU를 할당하는가?**  
   확인한 함수는 이벤트와 ruamw reset을 수행하며 물리 RU 교체 호출은 없다.
5. **FTL 이벤트 timestamp로 GC 순서를 시간 분석해도 되는가?**  
   해당 경로는 timestamp를 0으로 남기므로 그대로 시간값으로 사용할 수 없다.
6. **FDP TRIM은 요청 범위만 지우는가?**  
   이 FTL 함수는 범위를 사용하지 않고 전체 상태와 매핑을 초기화한다.
7. **TRIM 직후는 fresh QEMU와 같은가?**  
   아니다. CB heap, gc_ru, RUH 상태 카운터, 이벤트, erase/time 상태 등의 reset 차이를 확인해야 한다.
