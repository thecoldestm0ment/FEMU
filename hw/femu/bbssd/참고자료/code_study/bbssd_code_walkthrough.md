# FEMU bbssd 코드 워크스루

이 문서는 `hw/femu/bbssd` 안의 `bb.c`, `ftl.h`, `ftl.c`를 처음 읽는 학부생을 위한 코드 설명 자료입니다. FTL의 기본 개념, 예를 들어 LBA, LPN, PPA, mapping ta+ble, garbage collection은 이미 배웠다고 가정합니다. 목표는 개념을 코드와 연결해서 "요청 하나가 들어오면 어떤 함수와 자료구조를 지나가는지" 스스로 따라갈 수 있게 만드는 것입니다.

이번 문서에서는 기본 non-placement FTL 흐름만 다룹니다. `ssd->fdp_enabled` 같은 분기는 고급 placement 기능으로 보고, 지금은 "다른 경로로 빠진다" 정도만 이해하면 됩니다.

## 1. 전체 그림

`bbssd` 디렉터리의 세 파일은 역할이 분명히 나뉩니다.

| 파일 | 역할 | 처음 읽을 때의 관점 |
| --- | --- | --- |
| [`bb.c`](./bb.c) | FEMU BlackBox SSD 모드의 진입점 | NVMe 요청이 bbssd 모드로 들어오는 문 |
| [`ftl.h`](./ftl.h) | SSD/FTL 자료구조 정의 | 코드 전체에서 쓰는 용어 사전 |
| [`ftl.c`](./ftl.c) | 실제 FTL 동작 구현 | 초기화, read/write, mapping, GC, TRIM 처리 |

가장 큰 요청 흐름은 다음과 같습니다.

```text
NVMe request
  -> bb_io_cmd()
  -> nvme_rw()
  -> ftl_thread()
  -> ssd_read() / ssd_write() / ssd_trim()
  -> latency 계산
  -> poller로 request 반환
```

여기서 중요한 점은 `bb.c`가 직접 NAND를 읽고 쓰지 않는다는 것입니다. `bb.c`는 bbssd 모드를 FEMU에 등록하고, 요청을 FEMU/NVMe 공통 경로로 넘기는 입구 역할을 합니다. 실제 FTL 판단, 예를 들어 LPN을 어떤 PPA에 쓸지, 기존 page를 invalid로 만들지, GC를 할지는 대부분 `ftl.c`에서 일어납니다.

## 2. `bb.c`: BlackBox SSD 모드의 입구

`bb.c`는 짧지만 중요합니다. 이 파일은 "FEMU에서 bbssd 모드가 어떤 callback을 쓰는가"를 정합니다.

### `bb_init_ctrl_str()`

역할: 컨트롤러 모델명과 시리얼 문자열을 설정합니다.

```c
static void bb_init_ctrl_str(FemuCtrl *n)
```

이 함수는 `nvme_set_ctrl_name()`을 호출해서 컨트롤러 이름을 `"FEMU BlackBox-SSD Controller"`로 설정합니다. 실제 FTL 동작과 직접 관련은 없지만, QEMU 안에서 이 장치가 어떤 NVMe controller로 보이는지 정하는 초기 설정입니다.

바뀌는 자료구조:

- `FemuCtrl *n` 내부의 controller name 관련 필드

처음 읽을 때는 "장치 이름 붙이는 함수" 정도로 보면 충분합니다.

### `bb_init()`

역할: bbssd 모드에 필요한 `struct ssd`를 만들고 FTL 초기화를 시작합니다.

```c
static void bb_init(FemuCtrl *n, Error **errp)
```

핵심 코드는 다음 흐름입니다.

```text
struct ssd 할당
  -> controller string 설정
  -> dataplane flag 주소 저장
  -> SSD 이름 저장
  -> ssd_init(n) 호출
```

`n->ssd = g_malloc0(sizeof(struct ssd))`에서 FEMU controller인 `FemuCtrl` 안에 bbssd 전용 SSD 상태를 붙입니다. 이후 FTL 코드는 `n->ssd`를 통해 `struct ssd`에 접근합니다.

왜 필요한가:

- FEMU의 controller 객체는 NVMe 장치 전체를 나타냅니다.
- `struct ssd`는 bbssd FTL이 관리할 내부 상태입니다.
- 이 둘을 연결해야 request 처리 중에 FTL 상태에 접근할 수 있습니다.

바뀌는 자료구조:

- `n->ssd`
- `ssd->dataplane_started_ptr`
- `ssd->ssdname`
- `ssd->sp`, `ssd->ch`, `ssd->maptbl`, `ssd->rmap`, `ssd->lm`, `ssd->wp` 등은 뒤에서 `ssd_init()`이 채웁니다.

### `bb_flip()`

역할: 실험 중에 delay, GC delay, log 같은 기능을 켜고 끄는 admin command를 처리합니다.

```c
static void bb_flip(FemuCtrl *n, NvmeCmd *cmd)
```

`cmd->cdw10` 값을 보고 switch 문으로 동작을 나눕니다.

대표 동작:

| command | 의미 | 코드에서 바뀌는 값 |
| --- | --- | --- |
| `FEMU_ENABLE_GC_DELAY` | GC latency 반영 | `ssd->sp.enable_gc_delay = true` |
| `FEMU_DISABLE_GC_DELAY` | GC latency 무시 | `ssd->sp.enable_gc_delay = false` |
| `FEMU_ENABLE_DELAY_EMU` | NAND read/write/erase latency 활성화 | `pg_rd_lat`, `pg_wr_lat`, `blk_er_lat` 설정 |
| `FEMU_DISABLE_DELAY_EMU` | latency를 0으로 설정 | latency 값 0 |
| `FEMU_ENABLE_LOG` | log 출력 활성화 | `n->print_log = true` |
| `FEMU_DISABLE_LOG` | log 출력 비활성화 | `n->print_log = false` |

처음 읽을 때는 "FTL 정책 자체를 바꾸는 함수라기보다, 실험용 스위치를 다루는 함수"로 이해하면 됩니다.

### `bb_nvme_rw()`와 `bb_io_cmd()`

역할: NVMe read/write command를 공통 NVMe read/write 처리 함수로 넘깁니다.

```c
static uint16_t bb_nvme_rw(FemuCtrl *n, NvmeNamespace *ns, NvmeCmd *cmd,
                           NvmeRequest *req)
{
    return nvme_rw(n, ns, cmd, req);
}
```

`bb_io_cmd()`는 opcode를 확인합니다.

```text
NVME_CMD_READ 또는 NVME_CMD_WRITE
  -> bb_nvme_rw()
그 외
  -> invalid opcode
```

중요한 점은 여기서 바로 `ssd_read()`나 `ssd_write()`를 호출하지 않는다는 것입니다. `nvme_rw()`가 NVMe request를 만들고, 이후 dataplane과 FTL thread 사이의 ring queue를 통해 FTL 쪽으로 전달됩니다.

### `bb_admin_cmd()`

역할: bbssd 모드가 처리할 admin command를 고릅니다.

현재 이 파일에서 직접 처리하는 admin command는 `NVME_ADM_CMD_FEMU_FLIP`입니다. 이 command가 들어오면 `bb_flip()`을 호출합니다.

### `nvme_register_bbssd()`

역할: bbssd 모드의 callback table을 FEMU controller에 등록합니다.

```c
int nvme_register_bbssd(FemuCtrl *n)
```

핵심은 `n->ext_ops`에 함수 포인터들을 넣는 부분입니다.

```text
init      -> bb_init
admin_cmd -> bb_admin_cmd
io_cmd    -> bb_io_cmd
```

이 등록이 끝나야 FEMU가 bbssd 모드에서 어떤 초기화 함수와 command 처리 함수를 호출해야 하는지 알 수 있습니다.

정리하면 `bb.c`는 다음 한 문장으로 요약할 수 있습니다.

> `bb.c`는 bbssd 모드를 FEMU에 등록하고, 초기화와 NVMe command 진입점을 연결하는 파일이다.

## 3. `ftl.h`: FTL 코드의 자료구조 지도

`ftl.h`는 코드를 읽기 전에 반드시 봐야 하는 지도입니다. `ftl.c`의 함수들은 대부분 여기 정의된 구조체의 값을 바꿉니다.

### 주소 표현: `struct ppa`

```c
struct ppa {
    union {
        struct {
            uint64_t blk : BLK_BITS;
            uint64_t pg  : PG_BITS;
            uint64_t sec : SEC_BITS;
            uint64_t pl  : PL_BITS;
            uint64_t lun : LUN_BITS;
            uint64_t ch  : CH_BITS;
            uint64_t rsv : 1;
        } g;

        uint64_t ppa;
    };
};
```

PPA는 Physical Page Address입니다. 즉 "실제 NAND의 어느 위치인가"를 나타냅니다.

이 코드에서는 하나의 64-bit 값 안에 다음 정보를 bit field로 나눠 담습니다.

```text
channel -> LUN -> plane -> block -> page -> sector
```

예를 들어 `ppa.g.ch`는 channel 번호, `ppa.g.lun`은 LUN 번호, `ppa.g.blk`는 block 번호입니다. 동시에 전체 주소를 `ppa.ppa`라는 64-bit 정수처럼 다룰 수도 있습니다.

왜 필요한가:

- FTL은 LPN을 PPA로 바꾸어야 합니다.
- NAND 내부 계층별로 latency와 상태를 관리해야 합니다.
- GC 때 특정 block이나 page를 찾아가야 합니다.

### NAND 계층 구조

이 코드는 SSD 내부를 다음 구조체들로 표현합니다.

```text
ssd
  -> channel
    -> LUN
      -> plane
        -> block
          -> page
            -> sector status
```

관련 구조체는 다음과 같습니다.

| 구조체 | 의미 | 중요한 필드 |
| --- | --- | --- |
| `struct nand_page` | NAND page | `sec`, `nsecs`, `status` |
| `struct nand_block` | block | `pg`, `ipc`, `vpc`, `erase_cnt`, `wp` |
| `struct nand_plane` | plane | `blk`, `nblks` |
| `struct nand_lun` | LUN/die | `pl`, `next_lun_avail_time` |
| `struct ssd_channel` | channel | `lun`, `next_ch_avail_time` |

`ipc`와 `vpc`는 GC를 이해하는 핵심입니다.

| 이름 | 의미 |
| --- | --- |
| `vpc` | valid page count |
| `ipc` | invalid page count |

overwrite가 일어나면 기존 page는 invalid가 됩니다. 그러면 `vpc`는 줄고 `ipc`는 늘어납니다. GC는 보통 invalid page가 많은 block 또는 line을 골라 정리하려고 합니다.

### 상태 값: free, valid, invalid

page와 sector는 상태를 가집니다.

```c
SEC_FREE
SEC_INVALID
SEC_VALID

PG_FREE
PG_INVALID
PG_VALID
```

기본 FTL 흐름에서는 page 상태가 특히 중요합니다.

```text
처음 상태: PG_FREE
write 후: PG_VALID
overwrite 또는 trim 후: PG_INVALID
erase 후: PG_FREE
```

NAND flash의 중요한 특징은 in-place overwrite가 안 된다는 점입니다. 이미 쓴 page를 바로 덮어쓰지 않고, 새 page에 쓰고 기존 page를 invalid 처리합니다. 이 코드의 `mark_page_valid()`와 `mark_page_invalid()`가 그 상태 전이를 구현합니다.

### `struct ssdparams`

`struct ssdparams`는 SSD geometry와 timing 정보를 담습니다.

대표 필드:

| 필드 | 의미 |
| --- | --- |
| `secsz` | sector 크기 |
| `secs_per_pg` | page 하나에 들어가는 sector 수 |
| `pgs_per_blk` | block 하나의 page 수 |
| `blks_per_pl` | plane 하나의 block 수 |
| `luns_per_ch` | channel 하나의 LUN 수 |
| `nchs` | channel 수 |
| `pg_rd_lat` | NAND page read latency |
| `pg_wr_lat` | NAND program latency |
| `blk_er_lat` | block erase latency |
| `tt_pgs` | 전체 page 수 |
| `tt_lines` | 전체 line 수 |
| `gc_thres_lines` | background GC threshold |
| `gc_thres_lines_high` | foreground GC threshold |

초기화 때 `ssd_init_params()`가 FEMU 설정값을 읽어서 이 값들을 채웁니다.

예를 들어 전체 page 수는 대략 다음 관계로 계산됩니다.

```text
pages per plane
  = pages per block * blocks per plane

pages per LUN
  = pages per plane * planes per LUN

pages per channel
  = pages per LUN * LUNs per channel

total pages
  = pages per channel * number of channels
```

### Mapping table과 reverse mapping table

`struct ssd` 안에는 두 가지 mapping table이 있습니다.

```c
struct ppa *maptbl;
uint64_t *rmap;
```

`maptbl`은 LPN에서 PPA를 찾습니다.

```text
maptbl[lpn] = ppa
```

host가 LBA에 read 요청을 보내면 FTL은 먼저 LBA를 LPN으로 바꾼 뒤 `maptbl[lpn]`을 확인합니다. 값이 있으면 해당 PPA에서 NAND read latency를 계산합니다.

`rmap`은 반대로 PPA에서 LPN을 찾습니다.

```text
rmap[physical_page_index] = lpn
```

GC 때 필요합니다. GC는 victim block 또는 victim line 안의 valid physical page를 보고 "이 page가 어떤 LPN의 최신 데이터였지?"를 알아야 합니다. 그때 `rmap`을 사용합니다.

### Line, write pointer, line management

이 코드는 block 하나만 따로 관리하지 않고 line이라는 단위를 씁니다.

```c
typedef struct line {
    int id;
    int ipc;
    int vpc;
    QTAILQ_ENTRY(line) entry;
    size_t pos;
    ...
} line;
```

여기서 line은 여러 channel/LUN에 걸쳐 같은 block 번호를 가진 block들의 묶음, 즉 superblock에 가까운 개념입니다. 예를 들어 line id가 10이면 여러 LUN의 block 10들을 함께 하나의 write/GC 단위처럼 봅니다.

`struct write_pointer`는 다음에 쓸 physical 위치를 가리킵니다.

```c
struct write_pointer {
    struct line *curline;
    int ch;
    int lun;
    int pg;
    int blk;
    int pl;
};
```

쓰기 순서는 크게 다음처럼 진행됩니다.

```text
현재 line의 block 번호 사용
  -> channel을 하나씩 증가
  -> channel을 다 돌면 LUN 증가
  -> LUN을 다 돌면 page 증가
  -> block 안의 page를 다 쓰면 다음 free line 선택
```

`struct line_mgmt`는 line들의 상태를 관리합니다.

```c
struct line_mgmt {
    struct line *lines;
    free_line_list;
    victim_line_pq;
    full_line_list;
    int free_line_cnt;
    int victim_line_cnt;
    int full_line_cnt;
};
```

line은 크게 세 상태 중 하나에 있다고 보면 됩니다.

| 목록 | 의미 |
| --- | --- |
| `free_line_list` | 아직 쓸 수 있는 빈 line |
| `victim_line_pq` | invalid page가 있어서 GC 후보가 되는 line |
| `full_line_list` | 모든 page가 valid라 당장 GC 효율이 낮은 line |

## 4. `ftl.c`: 초기화 흐름

`ftl.c`에서 가장 먼저 이해해야 할 흐름은 `ssd_init()`입니다. `bb_init()`이 `ssd_init(n)`을 호출하면서 FTL 내부 상태가 만들어집니다.

### 전체 초기화 순서

기본 경로만 보면 `ssd_init()`의 흐름은 다음과 같습니다.

```text
ssd_init_params()
  -> channel/LUN/plane/block/page 구조 생성
  -> ssd_init_maptbl()
  -> ssd_init_rmap()
  -> ssd_init_lines()
  -> ssd_init_write_pointer()
  -> ftl_thread 생성
```

고급 placement 기능이 켜진 경우에는 별도 초기화 경로로 들어가지만, 이번 문서에서는 기본 FTL 경로만 봅니다.

### `ssd_init_params()`

역할: FEMU 설정값을 읽어서 SSD geometry와 GC threshold를 계산합니다.

```c
static void ssd_init_params(struct ssdparams *spp, FemuCtrl *n)
```

처음에는 `n->bb_params`에서 직접 설정값을 가져옵니다.

```text
sector size
sectors per page
pages per block
blocks per plane
planes per LUN
LUNs per channel
number of channels
latency 값들
```

그 뒤 계산되는 값들이 중요합니다.

```text
secs_per_blk = secs_per_pg * pgs_per_blk
pgs_per_pl   = pgs_per_blk * blks_per_pl
pgs_per_ch   = pgs_per_lun * luns_per_ch
tt_pgs       = pgs_per_ch * nchs
```

line 관련 값도 여기서 계산됩니다.

```text
blks_per_line = total LUN count
pgs_per_line  = blks_per_line * pages per block
tt_lines      = blocks per LUN
```

왜 필요한가:

- read/write에서 LBA를 LPN으로 바꾸려면 page 크기를 알아야 합니다.
- PPA를 linear page index로 바꾸려면 전체 geometry가 필요합니다.
- GC threshold를 line 개수 기준으로 계산해야 합니다.

바뀌는 자료구조:

- `ssd->sp`

### NAND 구조 생성 함수들

초기화 함수들은 계층적으로 호출됩니다.

```text
ssd_init_ch()
  -> ssd_init_nand_lun()
    -> ssd_init_nand_plane()
      -> ssd_init_nand_blk()
        -> ssd_init_nand_page()
```

각 함수는 자기 아래 계층 배열을 `g_malloc0()`로 할당하고 초기화합니다.

예를 들어 `ssd_init_nand_page()`는 page 안의 sector 상태를 모두 `SEC_FREE`로 두고, page 상태를 `PG_FREE`로 둡니다.

```text
처음 만들어진 NAND page
  -> sector들은 free
  -> page도 free
```

`ssd_init_nand_blk()`는 block 안의 page들을 만들고 다음 값을 0으로 초기화합니다.

```text
ipc = 0
vpc = 0
erase_cnt = 0
wp = 0
```

이 시점의 SSD는 아직 아무 데이터도 쓰지 않았으므로 모든 page가 free입니다.

### `ssd_init_maptbl()`

역할: LPN -> PPA mapping table을 초기화합니다.

```c
static void ssd_init_maptbl(struct ssd *ssd)
```

전체 page 수만큼 `struct ppa` 배열을 만들고, 모든 entry를 `UNMAPPED_PPA`로 설정합니다.

```text
maptbl[0] = UNMAPPED_PPA
maptbl[1] = UNMAPPED_PPA
...
```

의미는 "아직 어떤 LPN도 physical page에 매핑되지 않았다"입니다.

### `ssd_init_rmap()`

역할: PPA -> LPN reverse mapping table을 초기화합니다.

```c
static void ssd_init_rmap(struct ssd *ssd)
```

전체 physical page 수만큼 `uint64_t` 배열을 만들고, 모든 entry를 `INVALID_LPN`으로 설정합니다.

```text
rmap[physical page 0] = INVALID_LPN
rmap[physical page 1] = INVALID_LPN
...
```

아직 어떤 physical page도 유효한 LPN 데이터를 담고 있지 않다는 뜻입니다.

### `ssd_init_lines()`

역할: line 관리 자료구조를 초기화합니다.

```c
static void ssd_init_lines(struct ssd *ssd)
```

초기에는 모든 line이 비어 있으므로 `free_line_list`에 들어갑니다.

```text
line 0 -> free
line 1 -> free
line 2 -> free
...
```

또한 victim line을 고르기 위한 priority queue도 만듭니다.

```c
lm->victim_line_pq = pqueue_init(...)
```

이 priority queue는 line의 `vpc`를 우선순위로 사용합니다. valid page 수가 적은 line일수록 GC 때 옮겨야 할 데이터가 적어서 좋은 victim이 됩니다.

바뀌는 자료구조:

- `ssd->lm.lines`
- `ssd->lm.free_line_list`
- `ssd->lm.victim_line_pq`
- `ssd->lm.free_line_cnt`
- `ssd->lm.victim_line_cnt`
- `ssd->lm.full_line_cnt`

### `ssd_init_write_pointer()`

역할: 첫 write가 시작될 위치를 정합니다.

```c
static void ssd_init_write_pointer(struct ssd *ssd)
```

초기화 흐름:

```text
free_line_list의 첫 line을 꺼냄
  -> free_line_cnt 감소
  -> write pointer의 curline으로 설정
  -> ch/lun/pg/pl을 0으로 설정
  -> blk는 현재 line id로 설정
```

이제 첫 host write가 오면 `get_new_page()`가 이 write pointer를 보고 첫 PPA를 반환할 수 있습니다.

## 5. Read/Write 경로

이 장에서는 실제 host I/O가 들어왔을 때 어떤 일이 생기는지 봅니다.

### 공통: LBA에서 LPN으로

read와 write 모두 처음에는 LBA를 LPN 범위로 바꿉니다.

```c
uint64_t start_lpn = lba / spp->secs_per_pg;
uint64_t end_lpn = (lba + len - 1) / spp->secs_per_pg;
```

여기서 `secs_per_pg`는 page 하나가 몇 sector인지 나타냅니다. 즉 sector 단위 LBA 요청을 page 단위 LPN으로 바꾸는 것입니다.

예를 들어 page 하나가 8 sectors라면:

```text
LBA 0~7   -> LPN 0
LBA 8~15  -> LPN 1
LBA 16~23 -> LPN 2
```

이 FTL은 page-level mapping을 사용하므로 최종적으로 LPN 단위로 mapping table을 봅니다.

### `ssd_read()`

역할: 요청된 LPN들의 PPA를 찾아 read latency를 계산합니다.

```c
static uint64_t ssd_read(struct ssd *ssd, NvmeRequest *req)
```

흐름은 다음과 같습니다.

```text
LBA와 nlb로 start_lpn/end_lpn 계산
  -> 각 LPN에 대해 maptbl 확인
  -> mapping이 없거나 invalid PPA면 skip
  -> valid PPA면 NAND_READ latency 계산
  -> 가장 큰 latency를 반환
```

왜 가장 큰 latency를 반환할까요? 여러 page를 읽는 요청에서 각 page read가 서로 다른 LUN에서 병렬처럼 진행될 수 있습니다. 요청 전체 완료 시간은 각 page latency의 합이 아니라 가장 늦게 끝나는 sub-request에 가까워집니다. 그래서 `maxlat`을 사용합니다.

중요한 함수:

```c
ppa = get_maptbl_ent(ssd, lpn);
```

`maptbl[lpn]`을 읽어서 physical 위치를 찾습니다.

```c
sublat = ssd_advance_status(ssd, &ppa, &srd);
maxlat = (sublat > maxlat) ? sublat : maxlat;
```

`ssd_advance_status()`는 NAND command가 LUN의 available time을 얼마나 뒤로 미는지 계산합니다.

바뀌는 자료구조:

- read는 mapping table이나 page 상태를 바꾸지 않습니다.
- 대신 `nand_lun.next_lun_avail_time`이 바뀌어 timing model에 반영됩니다.

### `ssd_write()`

역할: host write를 page-level mapping 방식으로 처리합니다.

```c
static uint64_t ssd_write(struct ssd *ssd, NvmeRequest *req)
```

큰 흐름은 다음과 같습니다.

```text
LBA와 nlb로 start_lpn/end_lpn 계산
  -> free line이 너무 적으면 foreground GC 수행
  -> 각 LPN에 대해 반복
    -> 기존 mapping이 있으면 old PPA를 invalid 처리
    -> 새 PPA를 write pointer에서 얻음
    -> maptbl[lpn] = new PPA
    -> rmap[new PPA] = lpn
    -> new PPA를 valid page로 표시
    -> write pointer 전진
    -> NAND_WRITE latency 계산
  -> max latency 반환
```

이 함수는 FTL의 핵심입니다. NAND는 같은 page에 overwrite할 수 없으므로, 기존 데이터가 있으면 다음처럼 처리합니다.

```text
old PPA: valid -> invalid
new PPA: free -> valid
maptbl[lpn]: old PPA -> new PPA
```

즉 host 입장에서는 같은 LPN에 다시 쓴 것처럼 보이지만, 내부 physical 위치는 바뀝니다.

### 기존 mapping invalid 처리

```c
ppa = get_maptbl_ent(ssd, lpn);
if (mapped_ppa(&ppa)) {
    mark_page_invalid(ssd, &ppa);
    set_rmap_ent(ssd, INVALID_LPN, &ppa);
}
```

기존 PPA가 있으면 그 page를 invalid로 바꿉니다. 그리고 reverse mapping도 `INVALID_LPN`으로 지웁니다.

왜 필요한가:

- 최신 데이터는 새 PPA에 쓸 예정입니다.
- old PPA는 더 이상 host가 읽으면 안 됩니다.
- 하지만 NAND block erase 전까지 physical page 자체는 남아 있으므로 GC 대상이 됩니다.

바뀌는 자료구조:

- old page의 `status`
- old block의 `ipc`, `vpc`
- old line의 `ipc`, `vpc`
- `rmap[old physical page index]`

### 새 page 할당

```c
ppa = get_new_page(ssd);
set_maptbl_ent(ssd, lpn, &ppa);
set_rmap_ent(ssd, lpn, &ppa);
mark_page_valid(ssd, &ppa);
ssd_advance_write_pointer(ssd);
```

`get_new_page()`는 현재 write pointer가 가리키는 위치를 PPA로 만들어 반환합니다. 실제로 free list에서 page 하나를 pop하는 방식이 아니라, 현재 line과 `ch/lun/page` 좌표를 조합해서 PPA를 만듭니다.

그 뒤 mapping table과 reverse mapping table을 갱신합니다.

```text
maptbl[lpn] = new ppa
rmap[new physical page index] = lpn
```

마지막으로 `mark_page_valid()`로 page/block/line의 valid count를 늘립니다.

### `mark_page_valid()`

역할: 새로 쓴 physical page를 valid 상태로 바꿉니다.

```text
page.status = PG_VALID
block.vpc++
line.vpc++
```

처음 free였던 page에 데이터가 쓰였으므로 valid page 수가 늘어납니다.

### `mark_page_invalid()`

역할: overwrite나 trim으로 더 이상 최신 데이터가 아닌 page를 invalid 상태로 바꿉니다.

```text
page.status = PG_INVALID
block.ipc++
block.vpc--
line.ipc++
line.vpc--
```

만약 line이 원래 full list에 있었는데 invalid page가 생기면, 이제 GC 후보가 될 수 있습니다. 그래서 full list에서 victim priority queue로 이동합니다.

```text
full line
  -> invalid page 발생
  -> victim line priority queue로 이동
```

이 부분이 GC와 write path가 연결되는 지점입니다.

### `ssd_advance_write_pointer()`

역할: 다음 write가 사용할 physical 위치로 write pointer를 전진시킵니다.

전진 순서는 다음과 같습니다.

```text
ch 증가
  -> ch가 끝까지 가면 ch=0, lun 증가
  -> lun이 끝까지 가면 lun=0, page 증가
  -> page가 block 끝까지 가면 현재 line 사용 완료
  -> line을 full 또는 victim으로 분류
  -> 새 free line을 가져와 curline으로 설정
```

이 코드에서 line 하나는 여러 channel/LUN의 같은 block id를 묶은 단위입니다. 그래서 `ch`와 `lun`을 돌면서 여러 LUN에 write를 분산하고, page가 끝까지 차면 line 하나가 다 찬 것으로 봅니다.

line이 다 찼을 때 분류 기준:

```text
line.vpc == pages_per_line
  -> 모든 page가 아직 valid
  -> full_line_list로 이동

line.vpc < pages_per_line
  -> 쓰는 도중 overwrite 등으로 invalid page가 있음
  -> victim_line_pq로 이동
```

## 6. GC와 TRIM

FTL에서 GC는 "invalid page가 많은 공간을 골라 valid page만 새 곳으로 옮기고, block을 erase해서 다시 free 공간으로 만드는 과정"입니다.

### GC trigger: `should_gc()`와 `should_gc_high()`

```c
static inline bool should_gc(struct ssd *ssd)
{
    return (ssd->lm.free_line_cnt <= ssd->sp.gc_thres_lines);
}
```

`free_line_cnt`가 threshold 이하로 떨어지면 GC가 필요하다고 판단합니다.

두 threshold의 의미:

| 함수 | 사용 위치 | 의미 |
| --- | --- | --- |
| `should_gc()` | background GC | 여유가 줄었으니 뒤에서 GC 수행 |
| `should_gc_high()` | foreground write path | write를 계속하기 위험하니 즉시 GC 수행 |

`ssd_write()`의 앞부분에는 다음 흐름이 있습니다.

```text
while (should_gc_high(ssd))
  -> do_gc(ssd, true)
```

즉 free line이 너무 적으면 host write 처리 중에도 강제로 GC를 합니다.

### victim 선택: `select_victim_line()`

역할: GC할 line을 고릅니다.

```c
static struct line *select_victim_line(struct ssd *ssd, bool force)
```

victim line은 `victim_line_pq`에서 가져옵니다. 이 priority queue는 valid page count인 `vpc`를 우선순위로 씁니다. valid page가 적을수록 GC 때 복사해야 할 page가 적으므로 좋은 victim입니다.

`force`가 false일 때는 invalid page가 충분히 많지 않으면 GC를 미룹니다.

```text
victim_line->ipc < pages_per_line / 8
  -> 아직 invalid page가 너무 적음
  -> GC 효율이 낮으므로 skip
```

### `do_gc()`: 기본 GC 전체 흐름

역할: victim line 하나를 정리해서 free line으로 되돌립니다.

```c
static int do_gc(struct ssd *ssd, bool force)
```

전체 흐름:

```text
select_victim_line()
  -> victim line의 block id 확인
  -> 모든 channel과 LUN을 순회
    -> 해당 block 안의 valid page를 새 위치로 복사
    -> block erase
  -> line 상태를 초기화하고 free_line_list로 반환
```

여기서 victim line 하나는 여러 LUN의 같은 block 번호들을 묶은 것이므로, `do_gc()`는 channel과 LUN을 모두 돌면서 같은 block id를 가진 block들을 정리합니다.

### `clean_one_block()`

역할: victim block 하나 안에서 valid page만 골라 새 위치로 옮깁니다.

```c
static void clean_one_block(struct ssd *ssd, struct ppa *ppa)
```

흐름:

```text
block 안의 모든 page 순회
  -> PG_VALID page 발견
    -> gc_read_page()
    -> gc_write_page()
  -> invalid page는 복사하지 않음
```

GC가 공간을 회수할 수 있는 이유가 여기에 있습니다. invalid page는 최신 데이터가 아니므로 복사하지 않습니다. valid page만 새 위치로 옮긴 뒤 block 전체를 erase하면, invalid page가 차지하던 공간이 사라집니다.

### `gc_write_page()`

역할: GC 중 valid page를 새 PPA로 옮기고 mapping을 갱신합니다.

```c
static uint64_t gc_write_page(struct ssd *ssd, struct ppa *old_ppa)
```

흐름:

```text
old_ppa로 rmap 조회
  -> 이 physical page가 어떤 LPN인지 찾음
  -> write pointer에서 new_ppa 획득
  -> maptbl[lpn] = new_ppa
  -> rmap[new_ppa] = lpn
  -> new_ppa를 valid로 표시
  -> write pointer 전진
```

여기서 `rmap`이 꼭 필요합니다. GC는 physical page를 보고 시작하기 때문에, 그 page가 어떤 LPN의 데이터인지 알아야 mapping table을 새 PPA로 고칠 수 있습니다.

```text
GC 전:
  maptbl[LPN 7] = old PPA
  rmap[old PPA] = LPN 7

GC migration 후:
  maptbl[LPN 7] = new PPA
  rmap[new PPA] = LPN 7
```

### `mark_block_free()`와 `mark_line_free()`

`mark_block_free()`는 erase 후 block 내부 page 상태를 free로 되돌립니다.

```text
모든 page.status = PG_FREE
block.ipc = 0
block.vpc = 0
block.erase_cnt++
```

`mark_line_free()`는 line 단위 카운터를 초기화하고 free list에 넣습니다.

```text
line.ipc = 0
line.vpc = 0
free_line_list에 추가
free_line_cnt++
```

GC가 끝나면 이 line은 다시 write pointer가 선택할 수 있는 빈 line이 됩니다.

### `ssd_trim()`

역할: DSM/TRIM 요청을 처리해서 특정 LPN 범위의 mapping을 제거합니다.

```c
static uint64_t ssd_trim(struct ssd *ssd, NvmeRequest *req)
```

흐름:

```text
DSM range 목록 확인
  -> 각 range의 slba/nlb 읽기
  -> LPN 범위 계산
  -> 각 LPN에 대해 mapping 확인
    -> mapped PPA가 있으면 mark_page_invalid()
    -> rmap을 INVALID_LPN으로 설정
    -> maptbl[lpn]을 UNMAPPED_PPA로 설정
  -> range 메모리 해제
```

TRIM은 host가 "이 논리 주소의 데이터는 더 이상 필요 없다"고 알려주는 명령입니다. FTL 입장에서는 해당 page를 invalid로 만들 수 있으므로, 나중에 GC가 더 쉽게 공간을 회수할 수 있습니다.

## 7. `ftl_thread()`: request가 FTL로 들어오는 곳

`ftl_thread()`는 FTL의 메인 루프입니다.

```c
static void *ftl_thread(void *arg)
```

### 시작 대기

처음에는 dataplane이 시작될 때까지 기다립니다.

```text
while (!dataplane_started)
  -> sleep
```

그 뒤 FEMU controller의 ring queue 포인터를 SSD 구조체에 연결합니다.

```text
ssd->to_ftl = n->to_ftl
ssd->to_poller = n->to_poller
```

### request dequeue

FTL thread는 poller별 queue를 계속 확인합니다.

```text
for each poller
  -> to_ftl ring에 request가 있으면 dequeue
```

여기서 `to_ftl`은 poller에서 FTL로 request를 넘기는 queue입니다.

### opcode별 처리

dequeue한 request의 opcode를 보고 FTL 함수를 호출합니다.

```text
NVME_CMD_WRITE
  -> ssd_write()

NVME_CMD_READ
  -> ssd_read()

NVME_CMD_DSM
  -> ssd_trim()
```

고급 placement 기능이 켜진 경우에는 write와 trim이 별도 경로로 갈 수 있지만, 기본 FTL을 공부하는 지금은 `ssd_write()`, `ssd_read()`, `ssd_trim()` 흐름을 중심으로 보면 됩니다.

### latency 반영 후 반환

각 함수는 latency를 반환합니다. FTL thread는 이 값을 request에 반영합니다.

```c
req->reqlat = lat;
req->expire_time += lat;
```

그 뒤 처리된 request를 `to_poller` ring으로 돌려보냅니다.

```text
FTL 처리 완료
  -> latency 기록
  -> to_poller ring enqueue
  -> poller가 완료 처리
```

### background GC

request 하나를 처리한 뒤, free line이 threshold 이하인지 확인합니다.

```text
if (should_gc(ssd))
  -> do_gc(ssd, false)
```

이것이 background GC입니다. foreground GC는 write 중 free line이 너무 부족할 때 강제로 실행되고, background GC는 일반 request 처리 뒤 여유가 줄었을 때 한 번씩 실행됩니다.

## 8. 한 번에 따라가는 예시

### 첫 write

상황: LPN 0에 처음 write가 들어옵니다.

```text
ssd_write()
  -> maptbl[0] 확인
  -> UNMAPPED_PPA이므로 invalid 처리 없음
  -> get_new_page()로 새 PPA 획득
  -> maptbl[0] = new PPA
  -> rmap[new PPA] = 0
  -> mark_page_valid()
  -> ssd_advance_write_pointer()
```

결과:

```text
LPN 0은 새 physical page에 매핑됨
new page는 PG_VALID
block.vpc와 line.vpc 증가
```

### overwrite

상황: 이미 LPN 0이 쓰인 상태에서 LPN 0에 다시 write가 들어옵니다.

```text
ssd_write()
  -> maptbl[0]에서 old PPA 발견
  -> mark_page_invalid(old PPA)
  -> rmap[old PPA] = INVALID_LPN
  -> get_new_page()로 new PPA 획득
  -> maptbl[0] = new PPA
  -> rmap[new PPA] = 0
  -> mark_page_valid(new PPA)
  -> write pointer 전진
```

결과:

```text
old PPA는 PG_INVALID
new PPA는 PG_VALID
host가 LPN 0을 읽으면 new PPA를 읽음
old PPA는 나중에 GC가 회수할 공간이 됨
```

### GC

상황: free line이 부족해서 GC가 실행됩니다.

```text
do_gc()
  -> select_victim_line()
  -> victim line의 각 block 순회
  -> valid page만 gc_write_page()로 새 위치에 복사
  -> block erase
  -> mark_line_free()
```

결과:

```text
valid data는 새 위치로 이동
invalid data는 버려짐
victim line은 다시 free line이 됨
free_line_cnt 증가
```

## 9. 처음 읽는 학생을 위한 체크리스트

코드를 읽으면서 다음 질문에 답할 수 있으면 기본 흐름을 이해한 것입니다.

1. `bb_init()`에서 왜 `n->ssd`를 할당하는가?
2. LBA는 어디서 LPN으로 바뀌는가?
3. `maptbl`과 `rmap`은 각각 어느 방향의 mapping인가?
4. overwrite가 발생하면 old PPA는 어떻게 되는가?
5. `mark_page_invalid()`는 page뿐 아니라 block과 line의 어떤 값을 바꾸는가?
6. write pointer는 어떤 순서로 전진하는가?
7. line이 full list와 victim queue 중 어디로 가는 기준은 무엇인가?
8. GC는 왜 valid page만 복사하는가?
9. `gc_write_page()`에서 `rmap`이 필요한 이유는 무엇인가?
10. `ftl_thread()`는 request를 어디서 받고 어디로 돌려보내는가?

## 10. PPT로 바꿀 때의 슬라이드 초안

이 Markdown은 나중에 PPT와 발표대본으로 확장할 수 있도록 장을 나누어 작성했습니다. 첫 번째 PPT 초안은 다음 순서가 자연스럽습니다.

| 슬라이드 | 제목 | 핵심 그림 |
| --- | --- | --- |
| 1 | bbssd 코드 전체 구조 | `bb.c` / `ftl.h` / `ftl.c` 역할 분리 |
| 2 | 요청 처리 큰 흐름 | NVMe request -> FTL thread -> read/write/trim |
| 3 | PPA와 NAND 계층 | channel/LUN/plane/block/page |
| 4 | mapping table과 reverse mapping | LPN -> PPA, PPA -> LPN |
| 5 | 초기화 흐름 | `ssd_init()` call graph |
| 6 | write path | overwrite, invalidation, new page allocation |
| 7 | read path | LPN lookup과 latency 계산 |
| 8 | write pointer와 line | channel/LUN/page 순서 전진 |
| 9 | GC flow | victim 선택, valid page migration, erase |
| 10 | ftl_thread | ring queue와 opcode dispatch |

발표대본을 만들 때는 각 슬라이드마다 "이 함수가 왜 필요한가"를 먼저 말하고, 그다음 "어떤 구조체 필드가 바뀌는가"를 설명하면 코드가 훨씬 덜 복잡하게 들립니다.
