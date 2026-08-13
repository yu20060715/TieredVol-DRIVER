# TieredVol-DRIVER Architecture

## Overview

TieredVol-DRIVER is a **kernel device-mapper target** that implements weighted I/O striping directly in the Linux block layer. Unlike the userspace scheduler, it operates at the bio level with zero-copy I/O redirection, plus mirror support, weight-borrowing offload, and degradation detection.

## Two-Layer Architecture

```
┌─────────────────────────────────────────────────────────────┐
│  用戶態：tiered_setup CLI (src/)                              │
│                                                              │
│  main.c (82行)          CLI 入口                              │
│  cmd_create.c (479行)   建立卷（含 modprobe + dmsetup）       │
│  cmd_remove.c (190行)   刪除卷 + 狀態查詢                     │
│  setup_discover.c (117行) 硬體發現                             │
│  setup_bench.c (422行)   並行測速                             │
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
│    map()  I/O 分派熱路徑 (B 路徑: 跨磁碟 clone, C 路徑: WC)  │
│                                                              │
│  tieredvol_stripe.c    ★ 跨條紋 split helper + parallel 完成    │
│    tv_stripe_calc_boundaries() / tv_stripe_compute_ranges()   │
│    tv_parallel_submit()    平行 clone/submit helper           │
│    tv_parallel_end_io()    平行 write 完成收集 (stripe.c)      │
│                                                              │
│  tieredvol_map.c       ★ 條紋計算引擎                        │
│    tv_map_logical()          靜態權重條紋                     │
│    tv_map_logical_random()   隨機調度（測試用）               │
│  tieredvol_borrow.c    ★ weight-borrowing：慢碟借用快碟       │
│    tv_borrow_redirect()  塞車時整 block 重導至最少負載碟      │
│    tv_borrow_lookup()    讀回解析（與 runtime 開關無關）      │
│    tv_borrow_save/load() 持久化 overlay（<config>.borrow）   │
│                                                              │
│  tieredvol_meta.c      內核態 metadata 讀寫 + CRC32C         │
│  tieredvol_mirror.c    鏡像 I/O、讀取重試、資料重建、         │
│                        tieredvol_end_io (完成/統計)           │
│  tieredvol_badmap.c    per-disk bad block bitmap + rebuild    │
│  tieredvol_wc.c        ★ Write-coalescing 緩衝 + flush       │
│    tv_wc_init_ctx() / tv_wc_destroy_ctx() 生命週期            │
│    tv_wc_try_buffer()       嘗試緩衝 bio (C 路徑入口)         │
│    tv_wc_flush()            清空緩衝佇列 (含 B 路徑 clone)    │
│  tieredvol_log.c       日誌 ring buffer                      │
│  tieredvol_sysfs.c     sysfs 接口 (/sys/kernel/tieredvol/)  │
│  tieredvol_message.c   dmsetup message 分派                   │
│    ├─ msg_stats.c      統計/重置                              │
│    ├─ msg_policy.c     policy/borrow 設定                    │
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
    ├─ 選擇 policy: STATIC / RANDOM（RANDOM 僅測試用）
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
    ├─── [borrow] ─── tv_borrow_redirect() [tieredvol_borrow.c]
    │       ├─ src in_flight ≥ watermark 且整 block 對齊
    │       ├─ 選最少負載碟（排除 src/degraded/無空間）
    │       ├─ 記錄 per-block 表 → 重導至借用區
    │       └─ 讀/重寫經 tv_borrow_lookup() 解析同一目的地
    │
    ├─── [C 路徑: WC 啟用] ─── tv_wc_try_buffer() [tieredvol_wc.c]
    │       └─ 成功 → DM_MAPIO_SUBMITTED (bio 已緩衝)
    │
    ├─── [B 路徑: 跨條紋] ─── tv_stripe_calc_boundaries()
    │       tv_stripe_compute_ranges() → tv_parallel_submit()
    │       ├─ bio_alloc_clone() × n_sub
    │       ├─ submit_bio() × n_sub (tv_parallel_end_io 收集)
    │       └─ DM_MAPIO_SUBMITTED
    │
            └─── [直接提交] ─── bio_set_dev() → submit_bio(bio)
            ├─ [鏡像寫入] bio_alloc_clone → submit_bio(clone)
            └─ DM_MAPIO_SUBMITTED
    │
    ▼
物理碟 I/O 完成
    │
    ▼
tieredvol_end_io()  [tieredvol_mirror.c]
    │
    ├─ 減 in_flight_bytes[disk]
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
submit_bio(bio) → 物理碟
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
  │    └─ runtime_policy / borrow 運行期覆寫（[runtime] 節）
  │
  ├─ devs[16]              內核 dm_dev 引用
  │
  ├─ io.tv_io_stats        每碟 I/O 計數器 (atomic)
  │    ├─ in_flight_bytes[16]
  │    └─ total_write_bytes/ops[16]
  │
  ├─ borrow.tv_borrow_state   weight-borrowing 狀態
  │    ├─ enabled            運行期開關（borrow_on/off）
  │    ├─ block_size         borrow 粒度 = chunk_size/8（128KB）
  │    ├─ watermark_bytes    src in_flight 觸發門檻
  │    ├─ area_blocks/used_blocks[16]  每碟借用區/已用
  │    ├─ entries[]          per-block 記錄表（valid/dst_disk/dst_sector）
  │    └─ n_borrowed         已借 block 數
  │
  ├─ deg.tv_degradation    降級追蹤
  │    ├─ error_count[16]   每碟錯誤計數
  │    └─ degraded[16]      是否降級
  │
  └─ mirror/rebuild        鏡像/重建狀態
       ├─ pending-read/write per-CPU rings
       └─ rebuild_thread   主碟→mirror 重建
```

## Two Mapping Policies

### STATIC (預設)
Traditional weighted striping. Each disk gets `weight[i] × chunk_size` bytes per stripe. Deterministic, predictable. **Static 是位址權威**，weight-borrowing 在其上做暫時 offload。

### RANDOM
`disk = get_random_u32() % disk_count`. 僅測試/佈局壓力用，會破壞位址確定性。

### Weight-borrowing（在 STATIC 之上）
當慢碟（src）in-flight ≥ watermark 且寫入整 block 對齊時，把該 block 重導至
**最少負載**碟（排除 src / degraded / 借用區無空間）的 over-provisioned 借用區，
並記錄於 per-block 表。讀取與重寫經 `tv_borrow_lookup` 解析**同一目的地**（不因
runtime 開關而變），因此資料一致。`borrow_off` 只停新借、已借 block 仍正確解析。
表在 volume 移除時存成 `<config>.borrow`、重建時載入，跨 reload 一致。

## Borrow 借用區佈局

```
A 碟（快碟，權重高）邏輯空間：
  0 ──────────────── static 條紋區 ──────────────► borrow area ──►
     正常 chunk（權重比例）                    │  over-provisioned
                                               │  (borrow_area_mb)
                                               ▼
   借用區 sector = area_base_sector[dst] + used_blocks[dst] × block_sectors
   （逐 block 依序配置，entry 記錄 dst_disk + dst_sector）
```

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

## Borrow 觸發與選碟

```
tv_borrow_redirect()（WRITE 熱路徑）
    │
    ├─ enabled && entries 存在？
    ├─ 範圍全已借 → 重導（need==0，不受開關影響）
    ├─ src in_flight_bytes[src] ≥ watermark_bytes？
    ├─ logical/length 均 block 對齊？
    │
    ├─ pick_dst：min(in_flight) among
    │    排除 src / degraded / 借用區餘量 < need
    │
    └─ 逐 block 配置 slot（used_blocks[dst] 遞增）
        不足 → all-or-none 拒絕，不部分配置

tv_borrow_lookup()（READ 熱路徑）
    └─ entries[block].valid → dst_sector + (logical % block_size)>>9
        （與 borrow_on/off 無關，已借 block 永遠解析至借用區）
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
| `tieredvol_core.c` | 701 | DM lifecycle, map, module init/exit |
| `tieredvol_stripe.c` | 210 | Stripe-split helpers + parallel submit/end_io |
| `tieredvol_map.c` | 236 | Stripe calculation: static, random |
| `tieredvol_borrow.c` | 423 | Weight-borrowing: redirect/lookup/持久化 |
| `tieredvol_meta.c` | 590 | Kernel metadata read/write + CRC32C |
| `tieredvol_mirror.c` | 634 | Mirror I/O, read retry, rebuild, tieredvol_end_io |
| `tieredvol_badmap.c` | 149 | Per-disk bad block bitmap + rebuild |
| `tieredvol_wc.c` | 202 | Write-coalescing buffer + flush, init/destroy ctx (Phase 1 C) |
| `tieredvol_log.c` | 146 | Log ring buffer |
| `tieredvol_sysfs.c` | 273 | sysfs interface |
| `tieredvol_message.c` | 60 | dmsetup message dispatch |
| `msg_stats.c` | 174 | Statistics handlers |
| `msg_policy.c` | 121 | Policy/borrow handlers |
| `msg_mirror.c` | 331 | Mirror/rebuild handlers |
| `msg_config.c` | 86 | Config/log handlers |

## vs TieredVol (Userspace)

| | TieredVol (用戶態) | DRIVER (內核態) |
|---|---|---|
| **I/O 路徑** | 用戶態 `pwrite/pread` + pthread | 內核 `submit_bio()` 零拷貝 |
| **Stripe 計算** | 純數學，用戶態計算 | 內核二元搜尋 |
| **metadata** | INI 文字檔 | 同格式 + CRC32C |
| **額外功能** | 無 | 鏡像、weight-borrowing、降級偵測、sysfs |
| **適合場景** | 研究/比較基準 | 生產環境 |
