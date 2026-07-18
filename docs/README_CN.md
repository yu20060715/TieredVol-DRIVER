# TieredVol — 分層儲存卷管理器

Linux 分層儲存解決方案。將多顆硬碟合併成高效能條紋化磁碟區（striped volume），支援 **自選容量 carve** + dm-linear 分割 + LVM striped + RAM Cache 即時調優。

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

### 為什麼速度接近總和？

LVM striped volume 在寫入時會將資料分散到所有底層磁碟（parallel I/O），所以：

- **理論速度** = 各碟速度相加 = 2000 + 1000 = **3000 MB/s**
- **實際速度** ≈ 理論 × 94% ≈ **2820 MB/s**（扣除 LVM overhead、kernel 調度開銷）
- **大檔案循序讀寫**最接近理論值；隨機 I/O 會有差距

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
2. **設容量** — 為每顆碟設定要切割多少 GB 給 volume（用 `← →` 調整，步進 50GB）
3. **設定** — 輸入 volume 名稱、掛載點、檔案系統（ext4/xfs/btrfs）

建立過程中自動執行：dm-linear 分割 → pvcreate → vgcreate → lvcreate → mkfs → mount。任何步驟失敗自動回滾，清理所有殘留。

### Volume 管理
- **查看狀態** — 顯示 volume 名稱、大小、掛載點、使用率、各硬碟讀寫速度
- **刪除 Volume** — 一鍵拆除 striped LV → VG → PV → dm-linear 裝置，確認後才執行

### RAM Cache 調優
透過調整 Linux kernel 的 `vm.dirty_ratio` 參數，將部分系統 RAM 用作磁碟寫入快取。128MB 步進調整，Apply 套用 / Reset 還原原始值。退出程式時自動還原，不影響系統。

適用場景：有多顆 HDD 組成 striped volume 時，借用 RAM 作為寫入緩衝，加速小檔案寫入。

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

## 建置指令

```bash
make              # 編譯 tiered_setup + tiered_ui
make test         # 編譯並執行所有測試（53 個 test case）
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

## RAM Cache 調優

透過調整 kernel 的 `vm.dirty_ratio` 將部分 RAM 用作寫入快取：

- **← →**：調整借用量（128MB 步進）
- **↑ ↓**：選擇 Apply / Reset / Back
- **Apply**：套用新設定
- **Reset**：恢復原始值

例：16GB RAM 借用 2GB → dirty_ratio 從 20% 提升到 33%。

## 專案結構

```
TieredVol/
├── README.md                   # 說明文件（英文，根目錄）
├── docs/
│   ├── README_CN.md            # 說明文件（中文，本檔案）
│   ├── USAGE.md                # 詳細使用教學
│   └── PLAN.md                 # 改善計畫
├── Makefile                    # 建置系統
├── .gitignore
├── src/
│   ├── tiered_setup.c          # CLI 後端（1326 行）
│   ├── tiered_ui.c             # ncurses TUI 前端（1379 行）
│   ├── tiered_common.h         # 共用驗證函式（名稱/FS/掛載點白名單）
│   ├── tiered_ui_helpers.h     # TUI 共用輔助（ui_disk_t、解析函式）
│   └── version.h               # 版本常數
└── tests/
    ├── test_common.c           # 驗證函式測試（31 cases）
    └── test_tui.c              # TUI 解析測試（22 cases）
```

### 程式碼架構

| 模組 | 職責 |
|------|------|
| `tiered_setup.c` | CLI 核心：磁碟發現、parallel benchmark、dm-linear/LVM 建立刪除、rollback、config 持久化 |
| `tiered_ui.c` | TUI 前端：7 個畫面、3 階段建立精靈、背景測速、RAM cache 調整、終端防禦 |
| `tiered_common.h` | 輸入驗證：`tiered_is_valid_name()`、`tiered_is_valid_fs()`、`tiered_is_valid_mount()` |
| `tiered_ui_helpers.h` | `ui_disk_t` 結構、`parse_bench_output()`、`bench_disk_done()` |

## 注意事項

- **系統碟無法使用** — dm-linear 在已掛載的根分区上會回傳 EBUSY
- 選擇的硬碟資料會被**完全清除**
- 需要 root 權限執行所有操作
- RAM Cache 設定在退出時自動還原
- Carve 大小不能超過碟本身容量（例如 1T 碟最多 carve 999G）

## License

MIT
