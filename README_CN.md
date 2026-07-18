# TieredVol — 分層儲存卷管理器

Linux 分層儲存解決方案。將多顆硬碟合併成高效能條紋化磁碟區（striped volume），支援 **自選容量 carve** + dm-linear 分割 + LVM striped + RAM Cache 即時調優。

## TieredVol 是什麼？

TieredVol 是一個 Linux dm-linear + LVM striping 的 **TUI 管理工具**。為了同時最大化讀取速度與儲存空間利用率，本專案使用 Linux 原生的 dm-linear 進行區塊層級的硬碟切割，並透過 LVM 條帶化（striping）將多顆硬碟併行讀寫，達成接近所有貢獻碟速度總和的效能。

**本專案並非發明新的儲存演算法。** TieredVol 的價值在於：
- 友善的 ncurses TUI 介面，含 3 階段建立精靈
- 輸入驗證、雙重確認安全提示、失敗自動回滾
- 背景平行測速、即時 RAM Cache 調整
- 啟動前依賴檢查與 config 持久化

底層技術（dm-linear + LVM striping）是 Linux kernel 的成熟功能，自 kernel 2.6（2004 年）起即支援。TieredVol 將其封裝成易用、安全的工具。

```
Disk A (NVMe, 2000 MB/s) ──dm-linear──┐
Disk B (SATA, 1000 MB/s) ──dm-linear──┤── LVM VG ── striped LV ── filesystem ── mount
                                       │
                                       ▼
                                  接近 3000 MB/s
```

## 核心概念：Carve 自選容量

TieredVol 的核心功能是 **carve**——從每顆硬碟中切出指定大小的空間，組合成一顆虛擬的 striped volume。你可以自由決定每顆碟貢獻多少容量，而速度接近所有貢獻碟的速度總和。

### 範例：從兩顆碟組出高速 volume

假設你有兩顆硬碟：

| 碟 | 型號 | 容量 | 循序讀寫 |
|----|------|------|----------|
| sda | NVMe SSD | 1 TB | 2000 MB/s |
| sdb | SATA SSD | 1 TB | 1000 MB/s |

使用 carve 從 sda 取 1000G、從 sdb 取 500G：

```bash
sudo tiered_setup --create --name fastpool --disks sda:1000,sdb:500 --fs ext4 --mount /mnt/fast
```

結果：

```
┌─────────────────────────────────────────────────────────┐
│  striped volume: 1500 GB                                │
│  循序寫入：~2820 MB/s（理論 3000 × 94%）                  │
│  結構：1000G @ 2000 MB/s + 500G @ 1000 MB/s             │
└─────────────────────────────────────────────────────────┘
```

### Striping 實作原理

LVM striped volume 在 **block level** 同時對所有硬碟進行讀寫，不是依序寫。資料被切成 chunks（stripe unit，預設 64KB），以 round-robin 方式分配到各碟：

```
寫入資料流：[chunk0][chunk1][chunk2][chunk3][chunk4][chunk5]...
              ↓       ↓       ↓       ↓       ↓       ↓
sda (快):   [chunk0]         [chunk2]         [chunk4]...
sdb (慢):           [chunk1]         [chunk3]         [chunk5]...
```

兩顆碟同時在動。kernel 同時對所有碟下 I/O 請求，所以總吞吐量接近各碟速度的**總和**。

**效能特性：**

| 場景 | 結果 |
|------|------|
| 理論速度 | 各碟速度相加 = 2000 + 1000 = **3000 MB/s** |
| 實際速度 | 理論 × 94% ≈ **2820 MB/s**（LVM overhead + kernel 調度） |
| 大檔案循序 I/O（≥1GB） | 最接近理論值 |
| 小檔案隨機 I/O | 差很多 — striping 對隨機 I/O 幫助不大 |
| 單執行緒 vs 多執行緒 | 多執行緒（fio --numjobs=4）更容易打滿 |

**為什麼是 94% 而不是 100%？**

- LVM metadata 讀寫開銷
- Kernel I/O scheduler 調度延遲
- CPU 和記憶體頻寬限制
- Stripe 對齊（各碟之間的 stripe 位置對齊）

**重要：混合速度碟（NVMe + SATA）**

當組合不同速度的硬碟時，實際吞吐量受限於最慢的碟。LVM 對所有碟使用相同的 stripe size，所以較快的碟會空等較慢的碟。例如 NVMe（2000 MB/s）+ SATA（500 MB/s）實際只有 ~1000 MB/s，不是 2500 MB/s。詳見 [PARTITION_SPLITTING.md](docs/PARTITION_SPLITTING.md) 了解加權切塊演算法，[WEIGHTED_IO_SCHEDULER.md](docs/WEIGHTED_IO_SCHEDULER.md) 了解 I/O dispatch 實作。

**如何接近理論速度：**

```bash
# 用 fio 打滿 striped volume
sudo fio --name=test --filename=/mnt/fast/test --rw=write --bs=1M --size=2G --numjobs=4 --iodepth=32 --direct=1
```

### 範例：三碟組合

```bash
# 從三顆碟各取 500G，組成 1.5T volume
sudo tiered_setup --create --name tripool --disks nvme0n1:500,sdb:500,sdc:500 --fs ext4 --mount /mnt/tri

# 結果：~4500 MB/s（理論 5000 × 90%）
# nvme0n1: 2000 MB/s + sdb: 1500 MB/s + sdc: 1500 MB/s
```

### 不指定 carve 時

如果沒寫容量，預設取整顆碟的全部空間（扣 1GB 留給系統）：

```bash
# sda 取全碟 1000G，sdb 取全碟 500G
sudo tiered_setup --create --name pool --disks sda,sdb --fs ext4 --mount /mnt/pool
```

## 功能

### 硬碟偵測
自動掃描系統所有硬碟，顯示型號、傳輸介面（SATA/NVMe/USB）、容量。系統碟自動標記為 `[ROOT]` 並鎖定，防止誤選。已掛載的硬碟標記 `[MOUNTED]`，不可操作。

### 自動測速
啟動時背景自動跑 parallel benchmark，同時測多顆硬碟的循序讀寫速度。測速過程不阻塞 UI，可同時瀏覽其他畫面。測速中顯示 `TESTING...` 狀態，完成後立即更新速度。按 `B` 隨時重新測速。

### 建立 Volume
互動式 3 階段精靈，一步步引導建立 striped LVM volume：

1. **選碟** — 勾選要合併的硬碟，至少 2 顆。用 `↑↓` 移動游標、`Space` 勾選
2. **設容量** — 為每顆碟設定要切割多少 GB 給 volume（用 `← →` 調整，步進 1GB）
3. **設定** — 輸入 volume 名稱、掛載點、檔案系統（ext4/xfs/btrfs）

建立過程中自動執行：dm-linear 分割 → pvcreate → vgcreate → lvcreate → mkfs → mount。任何步驟失敗自動回滾，清理所有殘留。

### Volume 管理
- **查看狀態** — 顯示 volume 名稱、大小、掛載點、使用率、各硬碟讀寫速度
- **刪除 Volume** — 一鍵拆除 striped LV → VG → PV → dm-linear 裝置，確認後才執行

### RAM Cache 調優
透過調整 Linux kernel 的 `vm.dirty_ratio` 參數，將部分系統 RAM 用作磁碟寫入快取。128MB 步進調整，Apply 套用 / Reset 還原原始值。退出程式時自動還原，不影響系統。

適用場景：多顆 HDD 組成 striped volume 時，借用 RAM 作為寫入緩衝，加速小檔案寫入。TUI 可即時調整借用量、預覽新的 dirty_ratio、套用/還原。

### 安全防護
- **名稱驗證** — 白名單 `[a-zA-Z0-9._-]`，阻擋分號、 pipe、$、反引號等注入字元
- **檔案系統驗證** — 只允許 ext4、ext3、xfs、btrfs、none
- **掛載點驗證** — 必須是絕對路徑，拒絕 `..` 路徑穿越
- **LVM 命令安全** — 使用 `fork+execvp` 取代 `system()`，避免 shell 注入
- **暫存檔安全** — `fchmod` 即時設定權限，消除 TOCTOU 競爭窗口

### 錯誤回滾
建立 volume 時任何步驟失敗（dmsetup / pvcreate / vgcreate / lvcreate / mkfs / mount），自動清理所有已完成的裝置和 LVM 設定，不留殘留。基準測試中斷時自動 kill 所有子程序。

## 系統需求

- Linux（已測試 Ubuntu 24.04, kernel 6.14）
- `lvm2` `dmsetup` `libncurses-dev` `gcc` `make`
- Root 權限（sudo）

### 安裝依賴

```bash
# Debian / Ubuntu
sudo apt install lvm2 libncurses-dev gcc make

# Fedora / RHEL
sudo dnf install lvm2 ncurses-devel gcc make

# Arch
sudo pacman -S lvm2 ncurses gcc make
```

## 快速開始

```bash
git clone https://github.com/yu20060715/TieredVol.git
cd TieredVol
make
sudo ./tiered_ui
```

### 安裝到系統

```bash
sudo make install
sudo tiered_ui
```

### 啟用開機自動還原（可選）

```bash
sudo systemctl daemon-reload
sudo systemctl enable tieredvol-restore
```

啟用後，TieredVol volume 會在開機時自動從 saved config 重建。詳見 [USAGE.md](docs/USAGE.md#重開機保留)。

## 建置指令

```bash
make              # 編譯 tiered_setup + tiered_ui
make test         # 編譯並執行所有測試（56 個 test case）
make clean        # 刪除所有編譯产物
sudo make install # 安裝到 /usr/local/bin/
sudo make uninstall
```

## CLI 使用

```bash
# 列出所有硬碟
sudo tiered_setup --list

# 測速（3 顆硬碟）
sudo tiered_setup --bench --disks sdb,sdc,nvme0n1

# 建立 striped volume（從 sdb 取 300G、sdc 取 200G）
sudo tiered_setup --create --name fastpool --disks sdb:300,sdc:200 --fs ext4 --mount /mnt/fast

# 建立 striped volume（從 sda 取全碟、sdb 取 500G）
sudo tiered_setup --create --name pool --disks sda,sdb:500 --fs xfs --mnt /mnt/pool

# 查看狀態
sudo tiered_setup --status

# 刪除 volume
sudo tiered_setup --destroy --name fastpool
```

### Carve 語法

`--disks` 參數支援 `碟名:大小G` 格式：

| 範例 | 說明 |
|------|------|
| `sda:1000,sdb:500` | sda 取 1000GB、sdb 取 500GB |
| `sda,sdb` | 兩顆都取全碟（各扣 1GB） |
| `nvme0n1:2000,sda:1000,sdb:1000` | 三碟：2T + 1T + 1T = 4T volume |
| `sda:500,sdb:500,sdc:500` | 三碟各 500G，組成 1.5T volume |

## TUI 介面

```bash
sudo tiered_ui
```

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

### 快速鍵

| 畫面 | 按鍵 | 動作 |
|------|------|------|
| 主選單 | ↑↓ Enter Q/ESC | 選擇/確認/離開 |
| Disk List | B | 重新測速 |
| Disk List | Q/ESC | 返回 |
| Benchmark | Q/ESC | 返回（測速繼續背景跑）|
| Create Phase 1 | Space Enter | 選碟 / 下一步 |
| Create Phase 2 | ←→ ↑↓ | 調整 carve 容量 / 選碟 |
| RAM Cache | ←→ ↑↓ Enter | 調整 / 選擇 Apply/Reset |
| Destroy | Y | 確認刪除 |

## 專案結構

```
TieredVol/
├── README.md                   # 說明文件（英文，根目錄）
├── README_CN.md                # 說明文件（中文，本檔案）
├── LICENSE                     # MIT 授權條款
├── docs/
│   ├── USAGE.md                # 詳細使用教學
│   ├── PLAN.md                 # 改善計畫
│   ├── PARTITION_SPLITTING.md  # 加權切塊演算法
│   └── WEIGHTED_IO_SCHEDULER.md # I/O dispatch 實作
├── scripts/
│   ├── tieredvol-restore.sh    # 開機還原腳本
│   └── tieredvol-restore.service  # systemd 單元檔
├── Makefile                    # 建置系統
├── .gitignore
├── src/
│   ├── tiered_setup.c          # CLI 後端
│   ├── tiered_ui.c             # ncurses TUI 前端
│   ├── tiered_common.h         # 共用驗證函式（名稱/FS/掛載點白名單）
│   ├── tiered_ui_helpers.h     # TUI 共用輔助（ui_disk_t、解析函式）
│   └── version.h               # 版本常數
└── tests/
    ├── test_common.c           # 驗證函式測試
    └── test_tui.c              # TUI 解析測試
```

### 程式碼架構

| 模組 | 職責 |
|------|------|
| `tiered_setup.c` | CLI 核心：磁碟發現、parallel benchmark、dm-linear/LVM 建立刪除、rollback、config 持久化 |
| `tiered_ui.c` | TUI 前端：7 個畫面、3 階段建立精靈、背景測速、RAM cache 調整、終端防禦 |
| `tiered_common.h` | 輸入驗證：`tiered_is_valid_name()`、`tiered_is_valid_fs()`、`tiered_is_valid_mount()` |
| `tiered_ui_helpers.h` | `ui_disk_t` 結構、`parse_bench_output()`、`bench_disk_done()` |
| `tieredvol-restore.sh` | 開機還原腳本：讀取 `/etc/tieredvol/*.conf` 重建 dm-linear + LVM volumes |
| `tieredvol-restore.service` | systemd 單元：在本地檔案系統掛載後觸發還原腳本 |

## 注意事項

- **系統碟無法使用** — dm-linear 在已掛載的根分区上會回傳 EBUSY
- **已切過的碟會被鎖定** — 如果硬碟已有 `tv_*_carve` dm target，程式會報錯並提示先解除現有 volume。解除 volume 會銷毀其上所有資料，請先備份。
- **carve 部分會被覆蓋** — dm-linear 在區塊層級運作，從 sector 0 開始。它無法偵測哪些區塊有資料。carve 部分的資料將永久消失。請在操作前備份。
- **dm-linear 限制** — dm-linear 是區塊層級工具，不認檔案系統。它無法「跳過有資料的區塊」或「只切空白空間」。這是 Linux 儲存堆疊的根本限制，非本專案的缺陷。
- **每顆碟只能 carve 一次** — dm-linear 從 sector 0 開始，第二次 carve 同一顆碟會覆蓋第一次。如需重新 carve，請先用 `--remove` 解除現有 volume。
- **重開機後需重建** — dm-linear targets 和 LVM volumes 在重開機後不會自動恢復。啟用 systemd service 以實現自動還原：`sudo systemctl enable tieredvol-restore`。
- 需要 root 權限執行所有操作
- RAM Cache 設定在退出時自動還原
- Carve 大小不能超過碟本身容量（例如 1T 碟最多 carve 999G）

## License

MIT
