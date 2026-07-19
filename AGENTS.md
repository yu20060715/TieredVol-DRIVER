# AGENTS.md — TieredVol 專案指南

本文件供 AI agent 和開發者快速理解 TieredVol 專案的現狀、架構、待辦事項。

---

## 專案定位

TieredVol Scheduler 是一個實驗性的使用者空間加權條帶化排程器，用於異質儲存裝置，以 C 語言開發。

**核心功能**：驗證加權條帶化（按磁碟速度比例分配 chunk）能否提高異質磁碟組合的順序吞吐量。

**這不是檔案系統、RAID 實作、Linux 區塊驅動裝置或 Device Manager target。** 應用程式透過 `tv_write()` / `tv_read()` 與排程器互動，不會攔截標準 POSIX `write()`。

**目前進度**：Phase 0 + Phase 1（LVM striping TUI 工具）已完成，Phase 2（Weighted I/O Scheduler）已實作原型。

### 已知限制

- 僅支援靜態 weight（初始化時計算，不可變更）
- 無容錯機制（任何磁碟故障即導致整組 stripe set 損毀）
- 應用程式必須使用 `tv_write()` / `tv_read()`，標準 `write()` 走檔案系統不經過 scheduler
- 未完整實作 partial stripe tracking
- Benchmark 僅用於初始化，不是完整的儲存 benchmark
- 重開機後不保留（需 systemd service）
- 系統碟無法使用

---

## 目錄結構

```
TieredVol/
├── src/
│   ├── tiered_setup.c          # CLI 後端（建立/刪除/還原 volume）
│   ├── tiered_ui.c             # ncurses TUI（互動式介面）
│   ├── tiered_common.h         # 共用驗證函式
│   ├── tiered_ui_helpers.h     # UI 輔助函式
│   ├── version.h               # 版本 1.2.0
│   ├── tiered_sched.h          # Scheduler struct + API
│   ├── tiered_sched.c          # Scheduler 核心
│   ├── tiered_mapper.c         # Offset mapping
│   ├── tiered_stripe_buf.c     # Stripe buffer
│   ├── tiered_io_uring.c       # io_uring wrapper
│   ├── tiered_benchmark.c      # 初始化 benchmark
│   ├── tiered_partition.c      # Weight + segment 計算
│   ├── tiered_metadata.c       # Metadata 讀寫
│   └── tiered_io.c             # CLI I/O 工具（read/write/bench）
├── tests/
│   ├── test_common.c           # 驗證函式測試
│   └── test_tui.c              # TUI 解析測試
├── scripts/
│   ├── install_deps.sh         # 一鍵安裝依賴 + 建置
│   ├── test_scheduler.sh       # End-to-end scheduler 測試
│   ├── tieredvol-restore.sh
│   └── tieredvol-restore.service
├── docs/
│   ├── USAGE.md                # 詳細使用指南
│   ├── PLAN.md                 # 改進路線圖
│   ├── PARTITION_SPLITTING.md  # 切塊演算法（Weight 計算、容量分段、Offset Mapping）
│   └── WEIGHTED_IO_SCHEDULER.md # I/O Scheduler 實作（io_uring、stripe buffer、三層架構）
├── README.md
├── README_CN.md
├── AGENTS.md
├── Makefile
└── LICENSE
```

---

## Phase 2：Weighted I/O Scheduler（已實作原型）

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

## 已實作的檔案（Phase 2）

| 檔案 | 職責 | 行數 |
|------|------|------|
| `src/tiered_sched.h` | 所有 struct（TV_DISK, TV_SEGMENT, TV_METADATA, TV_MAP, TV_BUFFER, TV_SCHED）+ API 宣告 | 84 |
| `src/tiered_sched.c` | Scheduler 核心：init、write（buffer + flush）、read（mapping + io_uring）、destroy | 143 |
| `src/tiered_mapper.c` | Logical ↔ Physical offset mapping（prefix sum + linear scan） | 85 |
| `src/tiered_stripe_buf.c` | Stripe buffer 管理（aligned_alloc ring buffer，滿了就 flush） | 41 |
| `src/tiered_io_uring.c` | io_uring wrapper（SQE/CQE、submit、wait） | 56 |
| `src/tiered_benchmark.c` | 初始化 benchmark（O_DIRECT pwrite，32MB×3，3 次取平均） | 74 |
| `src/tiered_partition.c` | Weight 計算、容量分段、segment 建立 | 82 |
| `src/tiered_metadata.c` | Metadata 讀寫（文字設定檔格式） | 142 |
| `src/tiered_io.c` | CLI I/O 工具：info/read/write/bench，呼叫 tv_write/tv_read 驗證 scheduler | ~250 |

API 定義見 `src/tiered_sched.h`。完整 struct 說明見 [PARTITION_SPLITTING.md](docs/PARTITION_SPLITTING.md)。

---

### ⚠️ 已知缺口：I/O 路徑整合狀態

`tiered_io` CLI 工具已實作，提供 `--read` / `--write` / `--bench` / `--info` 四種模式，可驗證 scheduler 的 I/O 路徑。

使用方式：
```bash
sudo ./tiered_io --name fastpool --info
sudo ./tiered_io --name fastpool --bench --size 128MB
```

**仍缺少**：
- FUSE 檔案系統介面（讓 cp/dd/fio 直接用）
- libtiered.so 共用函式庫

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
# Step 1: 建立 scheduler volume
sudo ./tiered_setup --create --name testpool \
    --disks nvme0n1:100,sda:100 \
    --scheduler

# Step 2: 用 tiered_io 測試
sudo ./tiered_io --name testpool --info
sudo ./tiered_io --name testpool --bench --size 128MB

# Step 3: 對比 LVM striping
sudo ./tiered_setup --create --name testpool2 \
    --disks nvme0n1:100,sda:100 \
    --fs ext4 --mount /mnt/test2

dd if=/dev/zero of=/mnt/test2/testfile bs=1M count=128 conv=fdatasync
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
3. **`tiered_io` 是目前唯一的 I/O 入口**，用來驗證 scheduler 的 read/write/bench
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
