# TieredVol-DRIVER Architecture

## Overview

TieredVol-DRIVER is a **kernel device-mapper target** that implements weighted I/O striping directly in the Linux block layer. Unlike the userspace scheduler, it operates at the bio level with zero-copy I/O redirection, plus mirror support, adaptive scheduling, and degradation detection.

## Two-Layer Architecture

```
┌─────────────────────────────────────────────────────────────┐
│  用戶態：tiered_setup CLI (src/)                              │
│                                                              │
│  main.c (82行)          CLI 入口                              │
│  cmd_create.c (473行)   建立卷（含 modprobe + dmsetup）       │
│  cmd_remove.c (255行)   刪除卷 + 狀態查詢                     │
│  setup_discover.c (117行) 硬體發現                             │
│  setup_bench.c (420行)   並行測速                             │
│  tiered_metadata.c (156行) 寫 .conf 檔案                      │
│  tiered_partition.c (95行)  計算權重 + segments               │
│  exec_helper.c (115行)  fork/exec 外部指令                    │
└───────────────────────────┬─────────────────────────────────┘
                            │
        寫入 .conf 檔案 + dmsetup create
                            │
                            ▼
┌─────────────────────────────────────────────────────────────┐
│  內核態：tieredvol.ko (driver/)                              │
│                                                              │
│  tieredvol_core.c      ★ dm-target 核心                      │
│    ctr()  建構器：載入 .conf, 取得 dm_dev, 初始化             │
│    dtr()  解構器：釋放所有資源                                │
│    map()  I/O 分派熱路徑                                     │
│                                                              │
│  tieredvol_map.c       ★ 條紋計算引擎                        │
│    tv_map_logical()          靜態權重條紋                     │
│    tv_map_logical_adaptive() 自適應調度                      │
│    tv_map_logical_random()   隨機調度                        │
│                                                              │
│  tieredvol_meta.c      內核態 metadata 讀寫 + CRC32C         │
│  tieredvol_mirror.c    鏡像 I/O、讀取重試、資料重建           │
│  tieredvol_log.c       日誌 ring buffer + EMA 定時器         │
│  tieredvol_sysfs.c     sysfs 接口 (/sys/kernel/tieredvol/)  │
│  tieredvol_message.c   dmsetup message 分派                   │
│    ├─ msg_stats.c      統計/重置                              │
│    ├─ msg_policy.c     policy/adaptive 設定                  │
│    ├─ msg_mirror.c     鏡像/rebuild 控制                     │
│    └─ msg_config.c     log 控制                              │
└─────────────────────────────────────────────────────────────┘
```

## Data Flow: Write

```
用戶應用 write() → /dev/mapper/<name>
    │
    ▼
Device-Mapper 核心
    │
    ▼
tieredvol_map()  [tieredvol_core.c]
    │
    ├─ sector << 9 = logical byte offset
    ├─ 選擇 policy: STATIC / ADAPTIVE / RANDOM
    │
    ▼
tv_map_logical()  [tieredvol_map.c]
    │
    ├─ 二元搜尋找 segment
    ├─ stripe_no = (logical - seg_begin) / stripe_size
    ├─ offset_in = (logical - seg_begin) % stripe_size
    ├─ boundary array 找到目標碟
    └─ 回傳 { disk_index, disk_offset, length }
    │
    ▼
bio_set_dev(bio, target_bdev)
bio->bi_iter.bi_sector = disk_offset >> 9
    │
    ├─ [鏡像寫入] bio_alloc_clone → mirror disk
    │
    ▼
DM_MAPIO_REMAPPED → 區塊層
    │
    ▼
物理碟 I/O 完成
    │
    ▼
tieredvol_end_io()  [tieredvol_mirror.c]
    │
    ├─ 減 in_flight_bytes[disk]
    ├─ 計算延遲 (timestamp ring)
    ├─ 錯誤 → error_count++ → DEGRADED
    └─ 讀失敗 + mirror → tv_read_retry_work
```

## Data Flow: Read

```
用戶應用 read() → /dev/mapper/<name>
    │
    ▼
Device-Mapper 核心 → tieredvol_map()
    │
    ▼
tv_map_logical() → 回傳 { disk, offset, length }
    │
    ▼
bio_set_dev(bio, target_bdev)
    │
    ├─ [有 mirror] 記錄 pending-read → 錯誤時重試
    │
    ▼
DM_MAPIO_REMAPPED → 物理碟
    │
    ▼
完成 → tieredvol_end_io()
    ├─ 成功 → 清理 pending-read
    └─ 失敗 → tv_read_retry_work → 重試到 mirror
```

## Key Data Structures

```
tieredvol_ctx (每個 dm-setup 實例的核心上下文)
  │
  ├─ meta.tieredvol_metadata     從 .conf 載入
  │    ├─ disk_count, disk_names[16][64]
  │    ├─ segments[16]
  │    │    ├─ logical_begin/end   邏輯地址範圍
  │    │    ├─ disk_index[16]      參與的碟
  │    │    ├─ weight[16]          權重
  │    │    ├─ stripe_size         條紋大小
  │    │    └─ mirror_enabled/mirror_disk  鏡像設定
  │    └─ runtime_policy, stale_ms, wear_bias
  │
  ├─ devs[16]              內核 dm_dev 引用
  │
  ├─ io.tv_io_stats        每碟 I/O 計數器 (atomic)
  │    ├─ in_flight_bytes[16]
  │    ├─ total_write_bytes/ops[16]
  │    └─ total_latency_ns[16]
  │
  ├─ adaptive              自適應狀態
  │    ├─ policy            STATIC/ADAPTIVE/RANDOM
  │    ├─ ema_load[16]      負載 EMA
  │    ├─ ema_latency_ns[16] 延遲 EMA
  │    ├─ ema_iops[16]      IOPS EMA
  │    ├─ stale[16]         是否 stale
  │    └─ decay_timer       EMA 定時器 (100ms/1s)
  │
  ├─ deg.tv_degradation    降級追蹤
  │    ├─ error_count[16]   每碟錯誤計數
  │    └─ degraded[16]      是否降級
  │
  └─ mirror/rebuild        鏡像/重建狀態
       ├─ pending-read/write per-CPU rings
       ├─ timestamp rings (延遲測量)
       └─ rebuild_thread   主碟→mirror 重建
```

## Three Mapping Policies

### STATIC (預設)
Traditional weighted striping. Each disk gets `weight[i] × chunk_size` bytes per stripe. Deterministic, predictable.

### ADAPTIVE
Multi-factor scoring: `score = ema_load + ema_latency/1M + wear_bias × (disk_writes/total_writes)`. Selects disk with lowest score. Excludes stale and degraded disks.

### RANDOM
`disk = get_random_u32() % disk_count`. Useful for testing or when disk speeds are equal.

## Mirror System

```
Primary Disk ──write──→ bio
    │
    └─ bio_alloc_clone → Mirror Disk
         │
         └─ tv_mirror_end_io() → 完成追蹤

Primary Disk ──read fail──→ tv_read_retry_work
    │
    └─ clone bio → Mirror Disk → 重試 (最多32次, 1ms間隔)

Rebuild: tv_rebuild_thread
    └─ 逐 chunk 複製 primary → mirror, 帶退避
```

## Adaptive EMA Timer

```
tv_decay_timer_fn (100ms busy / 1s idle)
    │
    ├─ 原子交換 in_flight_bytes[disk] → 0
    ├─ 原子交換 interval_completions[disk] → 0
    │
    ├─ ema_load[disk] = ema_load × (1-α) + in_flight × α
    ├─ ema_latency[disk] = ema_latency × (1-α) + avg_latency × α
    ├─ ema_iops[disk] = ema_iops × (1-α) + completions × α
    │
    └─ 偵測 stale 碟 (無 I/O > 5s) → 標記/恢復
```

## Userspace ↔ Kernel Communication

```
┌─────────────────┐     ┌─────────────────┐
│  tiered_setup    │     │  tieredvol.ko   │
│  (用戶態)        │     │  (內核態)        │
└────────┬────────┘     └────────┬────────┘
         │                       │
         │  1. 寫 .conf 檔案      │
         │──────────────────────→│ (ctr 時讀取)
         │                       │
         │  2. dmsetup create     │
         │──────────────────────→│ 觸發 ctr()
         │                       │
         │  3. dmsetup message    │
         │──────────────────────→│ 分派到 msg_*.c
         │                       │
         │  4. sysfs 讀寫         │
         │<──────────────────────│ /sys/kernel/tieredvol/
         │                       │
         │  5. dmsetup remove     │
         │──────────────────────→│ 觸發 dtr()
         │                       │
         └───────────────────────┘
```

## Module Responsibilities

| Module | Lines | Purpose |
|--------|------:|---------|
| `tieredvol_core.c` | 582 | DM target lifecycle: ctr, dtr, map, end_io, status |
| `tieredvol_map.c` | 235 | Stripe calculation: static, adaptive, random |
| `tieredvol_meta.c` | 534 | Kernel metadata read/write + CRC32C |
| `tieredvol_mirror.c` | 577 | Mirror I/O, read retry, rebuild thread |
| `tieredvol_log.c` | 146 | Log ring buffer + EMA decay timer |
| `tieredvol_sysfs.c` | 273 | sysfs interface |
| `tieredvol_message.c` | 60 | dmsetup message dispatch |
| `msg_stats.c` | 146 | Statistics handlers |
| `msg_policy.c` | 170 | Policy/adaptive handlers |
| `msg_mirror.c` | 255 | Mirror/rebuild handlers |
| `msg_config.c` | 86 | Config/log handlers |
| `tiered_setup.c` | — | Userspace CLI (src/) |

## vs TieredVol (Userspace)

| | TieredVol (用戶態) | DRIVER (內核態) |
|---|---|---|
| **I/O 路徑** | 用戶態 `pwrite/pread` + pthread | 內核 `submit_bio()` 零拷貝 |
| **Stripe 計算** | 純數學，用戶態計算 | 內核二元搜尋 |
| **metadata** | INI 文字檔 | 同格式 + CRC32C |
| **額外功能** | 無 | 鏡像、自適應、EMA、降級偵測、sysfs |
| **適合場景** | 研究/比較基準 | 生產環境 |
