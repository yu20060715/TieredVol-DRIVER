# AGENTS.md — TieredVol 專案指南

本文件供 AI agent 和開發者快速理解 TieredVol 專案的現狀、架構、待辦事項。

---

## 專案定位

TieredVol 是一個 Linux 分層儲存 Volume 管理器，用 C 語言開發。

**核心功能**：將多顆不同速度的磁碟（NVMe + SATA）組合成一個高速 Volume。

**目前進度**：Phase 0 + Phase 1 已完成，Phase 2（Weighted I/O Scheduler）待實作。

---

## 目錄結構

```
TieredVol/
├── src/
│   ├── tiered_setup.c      # CLI 後端（建立/刪除/還原 volume）
│   ├── tiered_ui.c         # ncurses TUI（互動式介面）
│   ├── tiered_common.h     # 共用驗證函式
│   ├── tiered_ui_helpers.h # UI 輔助函式
│   └── version.h           # 版本 1.2.0
├── tests/
│   ├── test_common.c       # 驗證函式測試
│   └── test_tui.c          # TUI 解析測試
├── scripts/
│   ├── tieredvol-restore.sh      # 開機還原腳本
│   └── tieredvol-restore.service # systemd unit
├── docs/
│   ├── USAGE.md                 # 詳細使用指南
│   ├── PLAN.md                  # 改進路線圖
│   ├── PARTITION_SPLITTING.md   # 切塊演算法（Weight 計算、容量分段、Offset Mapping）
│   └── WEIGHTED_IO_SCHEDULER.md # I/O Scheduler 實作（io_uring、stripe buffer、三層架構）
├── README.md
├── README_CN.md             # 中文說明
├── AGENTS.md
├── Makefile
└── LICENSE
```

---

## Phase 2：Weighted I/O Scheduler（待實作）

### 為什麼要做

LVM striping 無法做到 1000+500+500=1800。快碟被迫等慢碟，整體速度 ≈ 最慢碟。Weighted Striping 讓快碟拿更多 chunk，慢碟拿較少，使大家同時完成。

### 架構

```
舊架構：dm-linear carving → LVM striping（無法加權）
新架構：dm-linear carving → Weighted I/O Scheduler（可以加權）
```

`--scheduler` 參數不加 → 走現有 LVM striping（向下相容）
`--scheduler` 加了 → 走新的加權排程

### 三層架構

```
┌─────────────────────────────────────────┐
│  第一層：Offset Map                      │
│  輸入 logical offset                     │
│  輸出 disk index + physical offset       │
│  只做數學，不碰 I/O                      │
├─────────────────────────────────────────┤
│  第二層：Stripe Buffer                   │
│  收使用者寫入                            │
│  累積到 stripe_size                      │
│  滿了就 flush                            │
│  只管資料暫存                            │
├─────────────────────────────────────────┤
│  第三層：Dispatcher                      │
│  把 buffer 切給各碟                      │
│  建 io_uring SQE                        │
│  submit + wait                           │
│  這一層才真正碰磁碟                      │
└─────────────────────────────────────────┘
```

### 資料流

```
1. 掃描磁碟 → 取得容量 + 速度
         ↓
2. benchmark → 每顆碟測速
         ↓
3. weight = round(speed / slowest_speed)
         ↓
4. 依容量排序 → 建立 segments（每個 segment 有自己的 disk list + weight）
         ↓
5. 儲存 metadata → /etc/tieredvol/*.scheduler
         ↓
6. 使用者寫入 → stripe buffer → 滿了 → flush
         ↓
7. flush → 依 weight 切分 buffer → 建 SQE → io_uring submit → 等完成
         ↓
8. 讀取 → map_logical_offset → io_uring read → 等完成 → 組回 buffer
```

---

## 需要新增的檔案

### Header File（最重要）

**`src/tiered_sched.h`** — 所有 struct 和 API 的定義

```c
#ifndef TIERED_SCHED_H
#define TIERED_SCHED_H

#include <stdint.h>
#include <liburing.h>

#define TV_MAX_DISKS    16
#define TV_MAX_SEGS     16
#define TV_CHUNK_SIZE   (64 * 1024)    // 64KB

// ─── Partition Splitting 用的 struct ───

typedef struct {
    int      id;
    int      fd;

    uint64_t total_size;    // 碟總容量（bytes）
    uint64_t free_size;     // 碟可用容量（bytes）

    uint64_t speed;         // 測速結果（MB/s）
    uint32_t weight;        // 加權值

    uint64_t physical_offset;  // 在 partition 內的物理起始位置
    char     name[64];         // 碟名（nvme0n1, sda, etc.）
} TV_DISK;

typedef struct {
    uint64_t logical_begin;
    uint64_t logical_end;

    uint32_t disk_count;
    uint32_t disk_index[TV_MAX_DISKS];
    uint32_t weight[TV_MAX_DISKS];

    uint64_t stripe_size;
} TV_SEGMENT;

typedef struct {
    uint32_t version;
    uint32_t chunk_size;
    uint32_t segment_count;

    // Disk info（needed for metadata save/load）
    uint32_t disk_count;
    char     disk_names[TV_MAX_DISKS][64];

    TV_SEGMENT segments[TV_MAX_SEGS];
} TV_METADATA;

typedef struct {
    int      disk;        // 目標碟的 index
    uint64_t offset;      // 在該碟上的物理 offset
    uint64_t length;      // 要讀寫的長度
} TV_MAP;

// ─── Scheduler 用的 struct ───

typedef struct {
    uint8_t *data;
    uint64_t used;
    uint64_t capacity;
    uint64_t logical_begin;
} TV_BUFFER;

typedef struct {
    io_uring ring;
    TV_METADATA *meta;
    TV_DISK     *disks;
    int          ndisks;
    TV_BUFFER    buf;
} TV_SCHED;

// ─── API ───

// Partition Splitting
uint32_t tv_compute_weight(uint64_t speed, uint64_t slowest);
int      tv_build_segments(TV_DISK *disks, int ndisks, TV_SEGMENT *segs, int *nsegs);
uint64_t tv_compute_stripe_size(uint32_t *weights, int nweights, uint32_t chunk_size);

// Offset Mapping
TV_MAP   tv_map_logical(uint64_t logical, TV_METADATA *meta);
uint64_t tv_map_reverse(int disk_index, uint64_t disk_offset, TV_METADATA *meta);

// Scheduler
TV_SCHED *tv_sched_init(TV_DISK *disks, int ndisks, TV_METADATA *meta);
int       tv_write(TV_SCHED *sched, const void *buf, uint64_t len);
int       tv_read(TV_SCHED *sched, void *buf, uint64_t len, uint64_t offset);
int       tv_flush(TV_SCHED *sched);
void      tv_sched_destroy(TV_SCHED *sched);

// Metadata
int  tv_metadata_save(TV_METADATA *meta, const char *path);
int  tv_metadata_load(TV_METADATA *meta, const char *path);

// Benchmark
int  tv_benchmark(const char *disk_path, uint64_t *speed_out);

#endif
```

### Source Files

| File | 職責 | 用到的 struct | 用到的 API |
|------|------|--------------|-----------|
| `tiered_benchmark.c` | 測速 | TV_DISK | `tv_benchmark()` |
| `tiered_partition.c` | 計算 weight + segments | TV_DISK, TV_SEGMENT, TV_METADATA | `tv_compute_weight()`, `tv_build_segments()`, `tv_compute_stripe_size()` |
| `tiered_mapper.c` | Offset mapping | TV_MAP, TV_METADATA | `tv_map_logical()`, `tv_map_reverse()` |
| `tiered_stripe_buf.c` | Stripe buffer 管理 | TV_BUFFER | buffer init/write/flush/reset |
| `tiered_sched.c` | Scheduler 核心 | TV_SCHED, TV_DISK, TV_METADATA | `tv_sched_init()`, `tv_write()`, `tv_read()`, `tv_flush()`, `tv_sched_destroy()` |
| `tiered_io_uring.c` | io_uring wrapper | io_uring | submit/write/read/wait |
| `tiered_metadata.c` | Metadata 讀寫 | TV_METADATA | `tv_metadata_save()`, `tv_metadata_load()` |

### 每個檔案的實作細節

#### tiered_benchmark.c

```c
// 測速方法：寫入 1MB 到碟上，計算 MB/s
// 使用 O_DIRECT 繞過 cache
// 測 3 次取平均
int tv_benchmark(const char *disk_path, uint64_t *speed_out) {
    int fd = open(disk_path, O_RDWR | O_DIRECT);
    // 分配 1MB buffer（對齊到 512 bytes）
    // 寫入 1MB
    // 計算時間差
    // speed_out = 1MB / time
    close(fd);
    return 0;
}
```

#### tiered_partition.c

```c
// 計算 weight：除以最慢碟，限制 ≤16
uint32_t tv_compute_weight(uint64_t speed, uint64_t slowest) {
    double w = (double)speed / (double)slowest;
    uint32_t result = (uint32_t)(w + 0.5);  // 四捨五入
    if (result > 16) result = 16;
    if (result < 1) result = 1;
    return result;
}

// 建立 segments：依容量排序，找出邊界
// 例如：Disk0 4TB, Disk1 2TB, Disk2 2TB, Disk3 1TB
// Segment0: 0~1TB → disk [0,1,2,3]
// Segment1: 1~2TB → disk [0,1,2]
// Segment2: 2~4TB → disk [0]
int tv_build_segments(TV_DISK *disks, int ndisks, TV_SEGMENT *segs, int *nsegs) {
    // 1. 依容量排序（小到大）
    // 2. 找出每個容量邊界
    // 3. 對每個 segment，計算 weight
    // 4. 計算 stripe_size = sum(weight) × chunk_size
    return 0;
}

// stripe_size = sum(weight) × chunk_size
// 例如 weight [7,4,2,1], chunk=64KB
// stripe_size = (7+4+2+1) × 64KB = 896KB
uint64_t tv_compute_stripe_size(uint32_t *weights, int nweights, uint32_t chunk_size) {
    uint32_t sum = 0;
    for (int i = 0; i < nweights; i++) sum += weights[i];
    return (uint64_t)sum * chunk_size;
}
```

#### tiered_mapper.c

```c
// Logical → Physical mapping
// 輸入：logical offset
// 輸出：TV_MAP（disk index, physical offset, length）
TV_MAP tv_map_logical(uint64_t logical, TV_METADATA *meta) {
    TV_MAP map = {0};

    // 1. 找到 segment（binary search by logical offset）
    int seg_idx = -1;
    for (int i = 0; i < meta->segment_count; i++) {
        if (logical >= meta->segments[i].logical_begin &&
            logical <  meta->segments[i].logical_end) {
            seg_idx = i;
            break;
        }
    }
    TV_SEGMENT *seg = &meta->segments[seg_idx];

    // 2. 計算 stripe_no 和 offset_in_stripe
    uint64_t stripe_no = logical / seg->stripe_size;
    uint64_t offset_in = logical % seg->stripe_size;

    // 3. 建立 prefix sum boundary
    // boundary[0] = 0
    // boundary[1] = weight[0] × chunk_size
    // boundary[2] = boundary[1] + weight[1] × chunk_size
    // ...
    uint64_t boundary[TV_MAX_DISKS + 1];
    boundary[0] = 0;
    for (int i = 0; i < seg->disk_count; i++) {
        boundary[i+1] = boundary[i] + seg->weight[i] * TV_CHUNK_SIZE;
    }

    // 4. binary search 找 disk
    int disk_idx = 0;
    for (int i = 0; i < seg->disk_count; i++) {
        if (offset_in >= boundary[i] && offset_in < boundary[i+1]) {
            disk_idx = i;
            break;
        }
    }

    // 5. 計算 physical offset
    map.disk   = seg->disk_index[disk_idx];
    map.offset = stripe_no * seg->weight[disk_idx] * TV_CHUNK_SIZE
               + (offset_in - boundary[disk_idx]);
    map.length = TV_CHUNK_SIZE;  // 簡化，實際可能更小

    return map;
}
```

#### tiered_stripe_buf.c

```c
// Buffer 管理（使用 TV_BUFFER from tiered_sched.h）
// 固定大小 = stripe_size

// 初始化
int tv_buf_init(TV_BUFFER *buf, uint64_t stripe_size) {
    buf->data = aligned_alloc(512, stripe_size);
    buf->used = 0;
    buf->capacity = stripe_size;
    buf->logical_begin = 0;
    return 0;
}

// 寫入（copy to buffer）
// 回傳：0 = 正常, 1 = buffer 滿了需要 flush
int tv_buf_write(TV_BUFFER *buf, const void *data, uint64_t len) {
    uint64_t space = buf->capacity - buf->used;
    if (len > space) len = space;
    memcpy(buf->data + buf->used, data, len);
    buf->used += len;
    return (buf->used == buf->capacity) ? 1 : 0;
}

// 重置
void tv_buf_reset(TV_BUFFER *buf) {
    buf->used = 0;
    buf->logical_begin += buf->capacity;
}
```

#### tiered_io_uring.c

```c
// io_uring wrapper

// 初始化
int tv_uring_init(io_uring *ring, int queue_depth) {
    io_uring_queue_init(queue_depth, ring, 0);
    return 0;
}

// 送一個 write request
int tv_uring_write(io_uring *ring, int fd, void *buf, size_t len, off_t offset) {
    struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
    io_uring_prep_write(sqe, fd, buf, len, offset);
    io_uring_sqe_set_data(sqe, NULL);
    return 0;
}

// 送一個 read request
int tv_uring_read(io_uring *ring, int fd, void *buf, size_t len, off_t offset) {
    struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
    io_uring_prep_read(sqe, fd, buf, len, offset);
    io_uring_sqe_set_data(sqe, NULL);
    return 0;
}

// Submit 所有 requests
int tv_uring_submit(io_uring *ring) {
    return io_uring_submit(ring);
}

// Wait for one completion
int tv_uring_wait(io_uring *ring) {
    struct io_uring_cqe *cqe;
    io_uring_wait_cqe(ring, &cqe);
    int res = cqe->res;
    io_uring_cqe_seen(ring, cqe);
    return res;
}

// 清理
void tv_uring_destroy(io_uring *ring) {
    io_uring_queue_exit(ring);
}
```

#### tiered_sched.c

```c
// Scheduler 核心 — 整合所有模組

TV_SCHED *tv_sched_init(TV_DISK *disks, int ndisks, TV_METADATA *meta) {
    TV_SCHED *sched = calloc(1, sizeof(TV_SCHED));
    sched->disks = disks;
    sched->ndisks = ndisks;
    sched->meta = meta;

    // 初始化 io_uring
    tv_uring_init(&sched->ring, 256);

    // 計算 stripe_size（從第一個 segment）
    uint64_t stripe_size = meta->segments[0].stripe_size;

    // 初始化 buffer
    tv_buf_init(&sched->buf, stripe_size);

    return sched;
}

int tv_write(TV_SCHED *sched, const void *buf, uint64_t len) {
    const uint8_t *src = buf;
    uint64_t pos = 0;

    while (pos < len) {
        uint64_t space = sched->buf.capacity - sched->buf.used;
        uint64_t chunk = (len - pos < space) ? (len - pos) : space;

        memcpy(sched->buf.data + sched->buf.used, src + pos, chunk);
        sched->buf.used += chunk;
        pos += chunk;

        // Buffer 滿了，flush
        if (sched->buf.used == sched->buf.capacity) {
            tv_flush(sched);
        }
    }
    return 0;
}

int tv_flush(TV_SCHED *sched) {
    // 找到目前 logical offset 對應的 segment
    uint64_t logical = sched->buf.logical_begin;
    TV_MAP map = tv_map_logical(logical, sched->meta);
    TV_SEGMENT *seg = &sched->meta->segments[0];  // 簡化，只用第一個 segment

    size_t buf_pos = 0;

    for (int i = 0; i < seg->disk_count; i++) {
        uint64_t disk_bytes = seg->weight[i] * TV_CHUNK_SIZE;
        if (disk_bytes == 0) continue;

        // 計算每顆碟的 physical offset
        // 使用 offset mapping 避免每次呼叫 tv_map_logical
        uint64_t stripe_no = logical / seg->stripe_size;

        // prefix sum boundary
        uint64_t boundary[TV_MAX_DISKS + 1];
        boundary[0] = 0;
        for (int j = 0; j < seg->disk_count; j++) {
            boundary[j+1] = boundary[j] + seg->weight[j] * TV_CHUNK_SIZE;
        }

        // disk i 的 physical offset
        uint64_t disk_off = stripe_no * seg->weight[i] * TV_CHUNK_SIZE;

        // 送 io_uring write
        tv_uring_write(&sched->ring,
                       sched->disks[seg->disk_index[i]].fd,
                       sched->buf.data + buf_pos,
                       disk_bytes,
                       disk_off);

        buf_pos += disk_bytes;
    }

    // Submit 所有 requests
    tv_uring_submit(&sched->ring);

    // 等全部完成
    for (int i = 0; i < seg->disk_count; i++) {
        int res = tv_uring_wait(&sched->ring);
        if (res < 0) {
            // I/O 失敗處理
        }
    }

    // 重置 buffer
    tv_buf_reset(&sched->buf);
    return 0;
}

int tv_read(TV_SCHED *sched, void *buf, uint64_t len, uint64_t offset) {
    uint8_t *dst = buf;
    uint64_t pos = 0;
    int pending = 0;

    while (pos < len) {
        TV_MAP map = tv_map_logical(offset + pos, sched->meta);
        uint64_t chunk = len - pos;
        if (chunk > map.length) chunk = map.length;

        tv_uring_read(&sched->ring,
                      sched->disks[map.disk].fd,
                      dst + pos,
                      chunk,
                      map.offset);
        pos += chunk;
        pending++;
    }

    // Submit 所有 requests
    tv_uring_submit(&sched->ring);

    // 等全部完成
    for (int i = 0; i < pending; i++) {
        int res = tv_uring_wait(&sched->ring);
        if (res < 0) {
            // I/O 失敗處理
        }
    }

    return 0;
}

void tv_sched_destroy(TV_SCHED *sched) {
    tv_uring_destroy(&sched->ring);
    free(sched->buf.data);
    free(sched);
}
```

#### tiered_metadata.c

```c
// Metadata 讀寫 — 文字檔格式

// 儲存格式：
// [weighted_striping]
// version=1
// chunk_size=65536
// segment_count=3
// seg0_disks=0,1,2,3
// seg0_weight=7,4,2,1
// seg0_stripe_size=896KB
// ...

int tv_metadata_save(TV_METADATA *meta, const char *path) {
    FILE *f = fopen(path, "w");
    if (!f) return -1;

    fprintf(f, "[weighted_striping]\n");
    fprintf(f, "version=%u\n", meta->version);
    fprintf(f, "chunk_size=%u\n", meta->chunk_size);
    fprintf(f, "segment_count=%u\n", meta->segment_count);
    fprintf(f, "disk_count=%u\n", meta->disk_count);

    for (int i = 0; i < meta->disk_count; i++) {
        fprintf(f, "disk%d_name=%s\n", i, meta->disk_names[i]);
    }

    for (int i = 0; i < meta->segment_count; i++) {
        TV_SEGMENT *seg = &meta->segments[i];
        fprintf(f, "seg%d_begin=%lu\n", i, seg->logical_begin);
        fprintf(f, "seg%d_end=%lu\n", i, seg->logical_end);
        fprintf(f, "seg%d_count=%u\n", i, seg->disk_count);
        fprintf(f, "seg%d_disks=", i);
        for (int j = 0; j < seg->disk_count; j++) {
            fprintf(f, "%s%u", j ? "," : "", seg->disk_index[j]);
        }
        fprintf(f, "\n");
        fprintf(f, "seg%d_weight=", i);
        for (int j = 0; j < seg->disk_count; j++) {
            fprintf(f, "%s%u", j ? "," : "", seg->weight[j]);
        }
        fprintf(f, "\n");
        fprintf(f, "seg%d_stripe=%lu\n", i, seg->stripe_size);
    }

    fclose(f);
    return 0;
}

int tv_metadata_load(TV_METADATA *meta, const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return -1;

    // 讀取並解析
    // ...

    fclose(f);
    return 0;
}
```

---

## 需要修改的檔案

| File | 改動 | 要加的東西 |
|------|------|-----------|
| `tiered_setup.c` | 加 `--scheduler` 參數 | `--scheduler` flag, 呼叫 benchmark → partition → metadata |
| `tiered_ui.c` | 加 Scheduler 選項、Status 畫面 | screen_scheduler_status(), create wizard 加 scheduler 選項 |
| `Makefile` | 新增 .c 編譯規則 | `tiered_sched.o`, `tiered_partition.o`, etc. + `-luring` |
| `tieredvol-restore.sh` | 讀取 .scheduler config | 讀取 metadata 重建 scheduler state |

### tiered_setup.c 改動

在 `main()` 的 argument parsing 加：
```c
} else if (strcmp(argv[i], "--scheduler") == 0) {
    use_scheduler = 1;
}
```

在 create 流程加：
```c
if (use_scheduler) {
    // 1. Benchmark 每顆碟
    for (int i = 0; i < ndisks; i++) {
        tv_benchmark(disks[i].name, &disks[i].speed);
    }
    // 2. 計算 weight
    // 3. 建立 segments
    // 4. 儲存 metadata
    tv_metadata_save(&meta, "/etc/tieredvol/fastpool.scheduler");
} else {
    // 現有 LVM striping 流程
}
```

### tiered_ui.c 改動

在 Create Volume 精靈加：
```c
// 選擇 I/O 模式
mvprintw(row, col, "I/O Mode:");
mvprintw(row+1, col, "  1. LVM Striping (default)");
mvprintw(row+2, col, "  2. Weighted Striping (new, faster)");
```

在 Status 畫面加 Scheduler Info：
```c
if (has_scheduler) {
    printw("Weight Table: [%s]\n", weight_str);
    printw("Stripe Size: %lu KB\n", meta.segments[0].stripe_size / 1024);
}
```

### Makefile 改動

```makefile
SCHED_OBJS = tiered_sched.o tiered_partition.o tiered_mapper.o \
             tiered_stripe_buf.o tiered_io_uring.o tiered_metadata.o \
             tiered_benchmark.o

tiered_setup: tiered_setup.o $(COMMON_OBJS) $(SCHED_OBJS)
    $(CC) -o $@ $^ -luring -lncurses -lm

tiered_ui: tiered_ui.o $(COMMON_OBJS) $(SCHED_OBJS)
    $(CC) -o $@ $^ -luring -lncurses -lm
```

---

## 實作順序

```
Step 1: 建立 src/tiered_sched.h（所有 struct + API 定義）
    ↓
Step 2: 實作 tiered_benchmark.c（測速）
    ↓
Step 3: 實作 tiered_partition.c（weight 計算 + segment 建立）
    ↓
Step 4: 實作 tiered_mapper.c（offset mapping）
    ↓
Step 5: 實作 tiered_stripe_buf.c（buffer 管理）
    ↓
Step 6: 實作 tiered_io_uring.c（io_uring wrapper）
    ↓
Step 7: 實作 tiered_metadata.c（metadata 讀寫）
    ↓
Step 8: 實作 tiered_sched.c（整合以上所有）
    ↓
Step 9: 修改 tiered_setup.c（加 --scheduler 參數）
    ↓
Step 10: 修改 tiered_ui.c（加 Scheduler 選項 + Status）
    ↓
Step 11: 修改 Makefile（加新 .c 檔案 + -luring）
    ↓
Step 12: 編譯測試 make clean && make
    ↓
Step 13: B85 測試（NVMe + SATA 測加權效果）
```

---

## 測試方法

### 單元測試（不需要真實碟）

```bash
# 測 weight 計算
# 測 offset mapping
# 測 metadata 讀寫
# 測 buffer 管理
```

### 整合測試（需要真實碟）

```bash
# B85: NVMe (via M.2 PCIe) + SATA
sudo ./tiered_setup --create --name testpool \
    --disks nvme0n1:100,sda:100 \
    --scheduler \
    --fs ext4 --mount /mnt/test

# 測速度
fio --name=test --filename=/mnt/test/testfile \
    --rw=write --bs=4k --size=1G \
    --numjobs=4 --iodepth=32 --direct=1

# 對比 LVM striping
sudo ./tiered_setup --create --name testpool2 \
    --disks nvme0n1:100,sda:100 \
    --fs ext4 --mount /mnt/test2

fio --name=test --filename=/mnt/test2/testfile \
    --rw=write --bs=4k --size=1G \
    --numjobs=4 --iodepth=32 --direct=1
```

---

## 編譯與測試

```bash
# 需要依賴
apt install liburing-dev   # Phase 2 需要
apt install libncurses-dev # TUI 需要
apt install lvm2           # LVM 需要

# 編譯
make clean && make

# 測試
make test
```

---

## 注意事項

1. **不要在 Windows 上編譯**，只能在 Linux（B85）上編譯測試
2. **不要動 dm-linear carving**，那是已完成的 Phase 0
3. **Weighted Scheduler 是獨立的 I/O 路徑**，不影響現有 LVM striping
4. **測試前先 `apt install liburing-dev`**，Phase 2 需要
5. **commit 前確認 `make clean && make` 零警告**
6. **所有 struct 統一放在 `tiered_sched.h`**，不要分散到其他 header

---

## 參考文件

| 文件 | 說明 | 什麼時候讀 |
|------|------|-----------|
| `docs/PARTITION_SPLITTING.md` | 演算法 + struct 定義 | 要實作 weight 計算、segment 建立、offset mapping 時 |
| `docs/WEIGHTED_IO_SCHEDULER.md` | I/O 實作 + 三層架構 + 踩坑 | 要實作 stripe buffer、io_uring dispatch、scheduler 核心時 |
| `docs/PLAN.md` | 改進路線圖 | 要了解整體進度和未來方向 |

**閱讀順序**：PARTITION_SPLITTING.md → WEIGHTED_IO_SCHEDULER.md → AGENTS.md
