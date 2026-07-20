# TieredVol：使用者空間加權條帶化排程器實作與驗證

## 問題

當 NVMe SSD（~3000 MB/s）與 SATA SSD（~500 MB/s）組合使用時，傳統 RAID0 / LVM striping 使用固定 stripe size，所有磁碟分配到相同的 chunk 大小。這導致快碟完成 I/O 後等待慢碟，整體吞吐量被最慢的磁碟限制，無法疊加頻寬。

## 解決方案：加權條帶化

核心想法很簡單：**讓快碟拿更多資料，慢碟拿更少，使大家同時完成。**

TieredVol 在使用者空間實作了一個加權條帶化排程器，透過 io_uring 進行非同步 I/O 派發。每次 stripe flush 時，每顆磁碟分配到 `weight[i] × TV_CHUNK_SIZE` 的資料量，其中 weight 由初始化 benchmark 決定：

```
weight[i] = round(speed[i] / slowest_speed)
```

例如 NVMe 1000 MB/s + SATA 500 MB/s → weight [2:1]，stripe_size = 3MB。

## 三層架構

```
第一層：Offset Map（純數學）
  輸入 logical offset → 輸出 (disk, physical offset, length)
  不碰 I/O

第二層：Stripe Buffer（資料暫存）
  接收使用者寫入 → 累積到 stripe_size → 觸發 flush
  池大小 = TV_BUF_COUNT（64 個 stripe buffer）

第三層：Dispatcher（實際 I/O）
  將 buffer 按 weight 切割 → 建立 io_uring SQE
  submit + wait CQE
```

Metadata 儲存為文字設定檔 `/etc/tieredvol/<name>.scheduler`，包含 disk 名稱、weight、segment 資訊。

## 驗證結果

### 硬體環境

B85 測試機，Linux 核心 6.x，三顆磁碟：

| 磁碟 | 型態 | 持續寫入 |
|------|------|---------|
| nvme0n1 | NVMe (M.2 PCIe) | ~1100 MB/s |
| sdb | SATA SSD | ~450 MB/s |
| sdc | SATA SSD | ~450 MB/s |

配置：`TV_CHUNK_SIZE=1MB`，`TV_BUF_COUNT=64`。

### 2 碟測試（nvme0n1 + sdb）

權重 [2:1]，stripe=3072KB。

| 大小 | Scheduler (MB/s) | LVM ext4 (MB/s) | 倍數 |
|------|-----------------|----------------|------|
| 512MB | 1472 (93.6%理論) | 640 | **2.3x** |
| 5GB | 1125 | — | — |
| 10GB | 1122 | — | — |

### 3 碟測試（nvme0n1 + sdb + sdc）

權重 [2:1:1]，stripe=4096KB。

| 大小 | Scheduler (MB/s) |
|------|-----------------|
| 512MB | 1792 (97.9%理論) |

### 關鍵發現

1. **加權條帶化有效疊加頻寬**：2 碟達 1472 MB/s（理論 1573），3 碟達 1792 MB/s（理論 1832），利用率 93-98%
2. **LVM 固定條帶化僅 640 MB/s**（2 碟），瓶頸在 dm-linear 層，非 ext4 非 I/O 提交方式
3. **1MB chunk 優於 256KB**（+35%），因 per-stripe 開銷更低
4. **大容量降速因 NVMe SLC cache 耗盡**，非架構問題

### 已知限制

- 靜態 weight，初始化後不可變更（資料遷移可改）
- 無容錯機制（單碟故障整組損毀）
- 應用程式須使用 `tv_write()`/`tv_read()`，不攔截 POSIX write
- 無 FUSE 整合
- dm-linear + O_DIRECT + io_uring 存在 CQE 丟失 bug，以 timeout + drain 處理
- Partial stripe 僅寫入第一顆碟（不影響正確性，影響平衡性）

## 程式碼架構

```
src/
├── tiered_setup.c    CLI 後端（建立、刪除、磁碟掃描）
├── tiered_sched.h    struct 定義 + API 宣告
├── tiered_sched.c    Scheduler 核心（write/read/flush/seek/destroy）
├── tiered_mapper.c   Logical ↔ Physical offset mapping
├── tiered_io_uring.c io_uring wrapper
├── tiered_benchmark.c 初始化 benchmark
├── tiered_partition.c Weight + segment 計算
├── tiered_metadata.c  Metadata 讀寫
├── tiered_io.c       CLI I/O 工具（read/write/bench）
├── tiered_common.h   共用驗證函式
└── version.h         版本 1.4.0
```

## 結語

TieredVol 證明了使用者空間加權條帶化可以有效解決異質磁碟組合的頻寬疊加問題。3 碟組合達到 1792 MB/s（97.9% 理論值），2 碟組合以 2.3x 勝出 LVM striping。實作採用純使用者空間 + io_uring，無需 kernel module 修改，是探索儲存系統設計的有效原型。

Phase 3（動態 tiering）和 Phase 4（FUSE/POSIX 整合）留待未來。

原始碼：https://github.com/yu20060715/TieredVol
授權：MIT
