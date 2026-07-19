# TieredVol Scheduler

一個實驗性的使用者空間加權條帶化排程器，用於異質儲存裝置。

TieredVol Scheduler 驗證一個假設：當不同速度的儲存裝置（如 NVMe + SATA SSD）組合時，透過加權條帶化（按磁碟順序頻寬比例分配 chunk 大小）能否提高整體順序吞吐量。

**這不是檔案系統、RAID 實作、Linux 區塊驅動裝置或 Device Mapper target。** 它不會攔截標準 POSIX `write()` 呼叫。應用程式透過專用的使用者空間 API（`tv_write()` / `tv_read()`）與排程器互動，直接對原始儲存裝置執行加權條帶化。

```
Application
      │
      ▼
  tv_write() / tv_read()    ← 應用程式必須使用此 API
      │
      ▼
  TieredVol Scheduler       ← 加權 chunk 分配 + offset mapping
      │
      ▼
  io_uring                  ← 非同步 I/O dispatch
      │
      ▼
  Raw Device (/dev/mapper/*)
```

### 此原型驗證的內容

- 加權 chunk 調度（按磁碟速度比例分配）
- 初始化時透過 benchmark 產生靜態 weight
- 基於 segment 的 mapping，處理不等容量磁碟
- Logical ↔ Physical offset mapping
- 使用 io_uring 的非同步平行 dispatch

### 有意排除的內容

- 檔案系統實作
- Linux 區塊驅動裝置 / Device Mapper target
- 資料備援（無 mirror、無 parity）
- 崩潰一致性 / journaling
- Metadata 恢復
- 動態線上重新平衡
- POSIX `write()` 攔截

目前實作假設 weight 在初始化時產生且固定不變。更改 weight 會使所有 logical-to-physical mapping 失效，除非執行資料遷移。

> 在大量循序工作負載且佇列深度足夠的情況下，整體吞吐量**可能接近**各磁碟順序頻寬的總和。實際結果取決於 CPU、PCIe 頻寬、檔案系統開銷和 I/O 模式。

---

## 加權條帶化 vs 固定大小條帶化

傳統 RAID0 和 LVM striping 對所有磁碟使用相同的 stripe size。當儲存裝置的順序頻寬差異顯著時，固定條帶化無法充分利用較快的裝置，因為所有磁碟必須在下一個 stripe 開始前完成各自的 chunk。

```
固定條帶化（LVM）：             加權條帶化（TieredVol）：

NVMe  3100 MB/s → 1 chunk     NVMe  3100 MB/s → 7 chunks = 448KB
SATA  1700 MB/s → 1 chunk     SATA  1700 MB/s → 4 chunks = 256KB
SATA   800 MB/s → 1 chunk     SATA   800 MB/s → 2 chunks = 128KB
SATA   450 MB/s → 1 chunk     SATA   450 MB/s → 1 chunk  =  64KB

NVMe 空等 SATA 完成            所有磁碟大約同時完成
→ 吞吐量 ≈ 最慢的碟            → 更高的整體吞吐量
```

**Weight 在初始化時產生**，透過 benchmark 測量循序寫入速度，並在 SLC cache 預熱（先寫2GB）後測速。這確保 weight 反映的是持久速度，而非 SLC cache 衝刺速度。

```bash
# 使用加權條帶化建立 session
sudo tiered_setup --create --name fastpool \
    --disks nvme0n1:1000,sda:500,sdb:500 \
    --scheduler

# 比較峰值 vs 持久速度
sudo tiered_io --name fastpool --bench --size 128MB           # 峰值（SLC cache）
sudo tiered_io --name fastpool --bench --size 128MB --warmup  # 持久
```

詳細實作見：
- [PARTITION_SPLITTING.md](docs/PARTITION_SPLITTING.md) — Weight 計算、容量分段、Offset Mapping
- [WEIGHTED_IO_SCHEDULER.md](docs/WEIGHTED_IO_SCHEDULER.md) — 三層架構、io_uring dispatch、stripe buffer
- [AGENTS.md](AGENTS.md) — 完整實作指南，包含每個模組的程式碼

---

## 舊版：dm-linear + LVM Striping 工具

本專案也包含一個 dm-linear carving + LVM striping 的 TUI 管理工具（Phase 0/1）。此工具早於加權條帶化排程器，提供友善的介面使用 Linux 原生 dm-linear 和 LVM 組合磁碟。

```
Disk A (NVMe, 2000 MB/s) ──dm-linear──┐
Disk B (SATA, 1000 MB/s) ──dm-linear──┤── LVM VG ── striped LV ── filesystem ── mount
                                       │
                                       ▼
                                  ~2820 MB/s（同速碟）
```

> **注意：** LVM striping 對所有磁碟使用相同的 stripe size。當組合不同速度的磁碟時，吞吐量受限於最慢的碟。這就是開發加權條帶化排程器的原因。

### 功能

- **自選容量 carve** — 透過 dm-linear 從每顆碟切出指定大小
- **ncurses TUI** — 互動式 3 階段建立精靈、硬碟清單、測速、RAM Cache 調整
- **輸入驗證** — 名稱/FS/掛載點白名單、雙重確認安全提示
- **錯誤回滾** — 任何步驟失敗自動清理所有已建立的裝置
- **自動測速** — 背景平行測試所有磁碟的讀寫速度
- **RAM Cache 調整** — 調整 `vm.dirty_ratio` 借用 RAM 作為寫入快取

### 快速開始（LVM Striping）

```bash
git clone https://github.com/yu20060715/TieredVol.git
cd TieredVol
make
sudo ./tiered_ui
```

### CLI 使用（LVM Striping）

```bash
# 列出所有硬碟
sudo tiered_setup --list

# 測速（3 顆硬碟）
sudo tiered_setup --bench --disks sdb,sdc,nvme0n1

# 建立 striped volume（從 sdb 取 300G、sdc 取 200G）
sudo tiered_setup --create --name fastpool --disks sdb:300,sdc:200 --fs ext4 --mount /mnt/fast

# 建立 volume（sda 取全碟、sdb 取 500G）
sudo tiered_setup --create --name pool --disks sda,sdb:500 --fs xfs --mount /mnt/pool

# 查看狀態
sudo tiered_setup --status

# 刪除 volume
sudo tiered_setup --destroy --name fastpool
```

### Carve 語法

| 範例 | 說明 |
|------|------|
| `sda:1000,sdb:500` | sda 取 1000GB、sdb 取 500GB |
| `sda,sdb` | 兩顆都取全碟（各扣 1GB） |
| `nvme0n1:2000,sda:1000,sdb:1000` | 三碟：2T + 1T + 1T |

---

## 系統需求

- Linux（kernel 5.1+ 以支援 io_uring）
- `lvm2` `dmsetup` `libncurses-dev` `gcc` `make`
- `liburing-dev`（Weighted Striping Scheduler 需要）
- Root 權限（sudo）

### 安裝依賴

```bash
# Debian / Ubuntu
sudo apt install lvm2 libncurses-dev gcc make liburing-dev

# Fedora / RHEL
sudo dnf install lvm2 ncurses-devel gcc make liburing-devel

# Arch
sudo pacman -S lvm2 ncurses gcc make liburing
```

## 建置

```bash
make              # 編譯 tiered_setup + tiered_ui + tiered_io
make test         # 執行所有測試（56 個 test case）
make clean        # 刪除所有編譯产物
sudo make install # 安裝到 /usr/local/bin/
```

### 啟用開機自動還原（可選）

```bash
sudo systemctl daemon-reload
sudo systemctl enable tieredvol-restore
```

---

## TUI 介面

```
┌─ Main Menu ─────────────────────┐
│   > Disk List                   │
│     Benchmark                   │
│     Create Volume               │
│     Volume Status               │
│     RAM Cache                   │
│     Destroy Volume              │
│     Exit                        │
└─────────────────────────────────┘
```

| 畫面 | 按鍵 | 動作 |
|------|------|------|
| 主選單 | ↑↓ Enter Q/ESC | 選擇/確認/離開 |
| Disk List | B | 重新測速 |
| Create Phase 1 | Space Enter | 選碟 / 下一步 |
| Create Phase 2 | ←→ ↑↓ | 調整 carve 容量 |
| RAM Cache | ←→ ↑↓ Enter | 調整 / Apply / Reset |
| Destroy | Y | 確認刪除 |

---

## 專案結構

```
TieredVol/
├── README.md
├── README_CN.md
├── AGENTS.md                   # 完整實作指南
├── LICENSE                     # MIT 授權條款
├── docs/
│   ├── USAGE.md                # 詳細使用教學
│   ├── PLAN.md                 # 改善計畫
│   ├── PARTITION_SPLITTING.md  # 加權條帶化演算法
│   └── WEIGHTED_IO_SCHEDULER.md # I/O dispatch 實作
├── scripts/
│   ├── install_deps.sh         # 一鍵安裝依賴
│   ├── test_scheduler.sh       # End-to-end scheduler 測試
│   ├── tieredvol-restore.sh
│   └── tieredvol-restore.service
├── Makefile
├── src/
│   ├── tiered_setup.c          # CLI 後端
│   ├── tiered_ui.c             # ncurses TUI 前端
│   ├── tiered_common.h         # 共用驗證函式
│   ├── tiered_ui_helpers.h     # TUI 輔助函式
│   ├── version.h
│   ├── tiered_sched.h          # Scheduler struct + API
│   ├── tiered_sched.c          # Scheduler 核心
│   ├── tiered_mapper.c         # Offset mapping
│   ├── tiered_stripe_buf.c     # Stripe buffer
│   ├── tiered_io_uring.c       # io_uring wrapper
│   ├── tiered_benchmark.c      # 初始化 benchmark
│   ├── tiered_partition.c      # Weight + segment 計算
│   ├── tiered_metadata.c       # Metadata 讀寫
│   └── tiered_io.c             # CLI I/O 工具（read/write/bench）
└── tests/
    ├── test_common.c
    └── test_tui.c
```

### 程式碼架構

| 模組 | 職責 |
|------|------|
| `tiered_setup.c` | CLI 核心：磁碟發現、benchmark、dm-linear/LVM/scheduler 建立、rollback |
| `tiered_ui.c` | TUI 前端：7 個畫面、建立精靈、測速、RAM cache 調整 |
| `tiered_sched.c` | Scheduler 核心：init、write（buffer + flush）、read（mapping + io_uring）、destroy |
| `tiered_mapper.c` | Logical ↔ Physical offset mapping（prefix sum + linear scan） |
| `tiered_stripe_buf.c` | Stripe buffer 管理（ring buffer，滿了就 flush） |
| `tiered_io_uring.c` | io_uring wrapper（SQE/CQE、submit、wait） |
| `tiered_benchmark.c` | 初始化 benchmark（O_DIRECT，3 次取平均）— **不是儲存 benchmark** |
| `tiered_partition.c` | Weight 計算、容量分段、segment 建立 |
| `tiered_metadata.c` | Metadata 讀寫（僅支援靜態 weight） |

---

## 限制

- **I/O 路徑已整合** — `tv_write()` / `tv_read()` 已實作，並由 `tiered_io` CLI 工具呼叫。透過 `tiered_io --bench`、`tiered_io --write`、`tiered_io --read` 驗證。FUSE/libtiered 整合尚未完成。
- **僅支援靜態 weight** — Weight 在初始化時計算並固定。更改 weight 會使所有 mapping 失效。
- **無容錯機制** — 任何磁碟故障即導致整組 stripe set 損毀。無 degraded mode、無 rebuild、無 mirror/parity。
- **無法攔截 POSIX write()** — 應用程式必須使用 `tv_write()` / `tv_read()`。標準 `write()` 走檔案系統，不經過 scheduler。
- **未完整實作 partial stripe tracking** — close/fsync 對部分 stripe 的處理尚未完成。
- **Benchmark 僅用於初始化** — 內建 benchmark 測量初始循序寫入速度（含 SLC cache 預熱），不是完整的儲存 benchmark（無 latency、無 queue depth sweep）。
- **重開機後不保留** — dm-linear targets 和 LVM volumes 需要 systemd service 才能自動還原。
- **系統碟無法使用** — dm-linear 在已掛載的根分区上回傳 EBUSY。

## License

MIT
