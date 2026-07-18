# TieredVol 使用教學

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

### 編譯

```bash
cd TieredVol
make
```

---

## TUI 互動模式

### 啟動

```bash
sudo ./tiered_ui
```

啟動後自動進入主選單，同時背景開始自動測速。

### 主選單操作

| 按鍵 | 動作 |
|------|------|
| `↑` `↓` | 上下移動游標 |
| `Enter` | 確認選擇 |
| `Q` / `ESC` | 退出 |

主選單有 7 個選項：

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

---

### 1. Disk List — 硬碟列表

顯示系統所有硬碟的型號、介面類型、容量、讀寫速度。

| 按鍵 | 動作 |
|------|------|
| `B` | 重新測速 |
| `Q` | 返回主選單 |

畫面標記說明：
- `[ROOT]` — 系統碟（掛載 `/`），無法使用
- `[MOUNTED]` — 已掛載的硬碟，無法使用
- 測速中顯示 `Testing...`，完成後顯示 `Write XXX Read XXX MB/s`

> 硬碟列表只在進入畫面時自動刷新一次。按 `B` 可以手動觸發重新測速。

---

### 2. Benchmark — 測速結果

查看所有硬碟的讀寫速度。進入畫面後顯示目前的測速進度，完成後顯示最終結果。

| 按鍵 | 動作 |
|------|------|
| `Q` | 返回主選單（測速繼續背景進行） |

測速過程中會每 200ms 自動更新畫面，顯示每顆碟的狀態：
- `Waiting...` — 尚未開始
- `Testing... Xms Y/Z` — 測試中（已用時間 / 進度）
- `Write XXX Read XXX MB/s` — 完成

---

### 3. Create Volume — 建立 Volume（3 步驟精靈）

互動式引導建立 striped LVM volume。

#### Step 1：選碟

| 按鍵 | 動作 |
|------|------|
| `↑` `↓` | 移動游標 |
| `Space` | 勾選 / 取消勾選 |
| `Enter` | 確認，進入下一步 |
| `ESC` | 取消，返回主選單 |

- 至少勾選 **2 顆**硬碟
- `[ROOT]` 和 `[MOUNTED]` 的硬碟無法勾選
- 測速未完成的硬碟顯示 `[BENCH...]`，需等測速完成才能勾選

#### Step 2：設定切割容量

| 按鍵 | 動作 |
|------|------|
| `←` `→` | 調整容量（每次 50GB） |
| `↑` `↓` | 切換硬碟 |
| `Enter` | 確認，進入下一步 |
| `ESC` | 返回 Step 1 |

為每顆勾選的硬碟設定要切割多少 GB 給 volume。例如 500GB 的碟可以切 300GB。

#### Step 3：輸入設定

| 欄位 | 說明 | 預設值 |
|------|------|--------|
| Volume name | 英數 + `._-`（如 `fastpool`） | 無，必填 |
| Mount point | 掛載路徑（如 `/mnt/fast`） | `/mnt/fast` |
| Filesystem | `ext4` / `xfs` / `btrfs` / `none` | `ext4` |

| 按鍵 | 動作 |
|------|------|
| `Tab` | 切換欄位 |
| `Enter` | 確認建立 |
| `ESC` | 取消 |

確認後程式自動執行：
1. 清理舊的 dm 目標
2. 建立 dm-linear 分割
3. 建立 LVM PV
4. 建立 VG
5. 建立 striped LV
6. 格式化檔案系統
7. 掛載

**任何步驟失敗會自動回滾**，清理所有已完成的裝置。

---

### 4. Volume Status — Volume 狀態

顯示已建立 volume 的資訊：
- Volume 名稱
- LVM 詳情（大小、stripes、stripesize）
- 掛載點
- 磁碟使用率（df）

按任意鍵返回主選單。

---

### 5. RAM Cache — 寫入快取調整

透過調整 Linux `vm.dirty_ratio` 參數，借用系統 RAM 作為磁碟寫入緩衝。

| 按鍵 | 動作 |
|------|------|
| `←` `→` | 調整借用量（128MB 步進） |
| `↑` `↓` | 選擇按鈕 |
| `Enter` | 執行選中的按鈕 |
| `Q` | 返回主選單 |

三個按鈕：
- **Apply** — 套用新的 dirty_ratio
- **Reset** — 還原為原始值
- **Back** — 返回主選單

適用場景：多顆 HDD 組成 striped volume 時，借用 RAM 加速小檔案寫入。

> 例：16GB RAM 借用 2GB → dirty_ratio 從 20% 提升到 33%。
> 退出程式時**自動還原**，不影響系統。

---

### 6. Destroy Volume — 刪除 Volume

確認後一鍵拆除 striped LV → VG → PV → dm-linear 裝置。

按 `Y` 確認刪除，其他任意鍵取消。

---

### 7. Exit — 退出

自動還原 RAM Cache 設定，清理背景測速程序。

---

## CLI 模式

所有 CLI 操作需要 root 權限。

### 列出硬碟

```bash
sudo tiered_setup --list
```

輸出範例：

```
DEVICE       TYPE     MODEL                        TRAN         SIZE       SPEED
------------ -------- ---------------------------- ------------ -------- --------
sda          disk     Samsung SSD 870              sata         500.1G     -
sdb          disk     WDC WD10EZEX                sata         931.5G     -
nvme0n1      disk     WD Black SN770              nvme         1.0T       -

[ROOT] = System disk, cannot be carved with dm-linear
[MOUNTED] = Mounted partition, cannot be carved with dm-linear
```

### 測速

```bash
# 依序測試
sudo tiered_setup --bench --disks sdb,sdc,nvme0n1

# 依序測試（加 --sequential）
sudo tiered_setup --bench --disks sdb,sdc --sequential
```

預設 parallel 模式，多顆碟同時跑。輸出每顆碟的 Write / Read 速度（MB/s）。

### 建立 Volume

```bash
sudo tiered_setup --create --name fastpool --disks sdb:300,sdc:200 --fs ext4 --mount /mnt/fast
```

參數說明：
| 參數 | 說明 | 必填 |
|------|------|------|
| `--name` | Volume 名稱 | 是 |
| `--disks` | 硬碟列表（格式：`碟名:GB,碟名:GB`） | 是 |
| `--fs` | 檔案系統（ext4/xfs/btrfs/none） | 否，預設 ext4 |
| `--mount` | 掛載點 | 否 |
| `--stripesize` | stripe 大小（KB） | 否，自動判斷 |

不指定容量時，使用硬碟全部空間（扣 1GB）。

#### 建立流程

程式會依序執行以下步驟：

1. **碟驗證** — 檢查每顆碟的狀態：
   - 系統碟（`[ROOT]`）→ 跳過
   - 已掛載碟（`[MOUNTED]`）→ 跳過
   - **已切過的碟** → 報錯退出，提示先執行 `--remove` 解除（並警告資料會消失）
   - 碟容量不足 → 報錯退出
2. **Configuration 顯示** — 列出每顆碟的名稱、carve 大小、剩餘空間、速度、比例
3. **雙重確認** — 顯示警告訊息，要求輸入 `YES` 才能繼續（其他輸入直接退出）
4. **清理舊 target** — 自動清除上次的 dm-linear、VG、LV
5. **建立 dm-linear** — 從每顆碟的 sector 0 切出指定大小
6. **建立 LVM** — pvcreate → vgcreate → lvcreate striped
7. **格式化** — mkfs.ext4/xfs/btrfs
8. **掛載** — mount 到指定路徑

**任何步驟失敗會自動回滾**，清理所有已完成的裝置。

#### 剩餘空間顯示

Configuration 表格會顯示每顆碟 carve 後的剩餘空間：

```
  DEVICE       CARVE    REMAIN   SPEED      TIER       RATIO
  ------------ -------- -------- ---------- ---------- ----------
  sdb          1000GB   0GB      2000       FAST       66.7%
  sdc          500GB    500GB    1000       SLOW       33.3%
```

注意：每顆碟只能 carve 一次（dm-linear 從 sector 0 開始）。剩餘空間不能再次 carve。

### 查看狀態

```bash
sudo tiered_setup --status
```

顯示 DM targets 和已儲存的 config 檔案。

### 刪除 Volume

```bash
sudo tiered_setup --destroy --name fastpool
# 或
sudo tiered_setup --remove --name fastpool
```

---

## 快速鍵速查

| 畫面 | 按鍵 | 動作 |
|------|------|------|
| 主選單 | `↑↓` `Enter` | 選擇 / 確認 |
| 主選單 | `Q` | 退出 |
| Disk List | `B` | 重新測速 |
| Disk List | `Q` | 返回 |
| Benchmark | `Q` | 返回（測速繼續） |
| Create Step 1 | `Space` `Enter` | 選碟 / 下一步 |
| Create Step 2 | `←→` `↑↓` | 調整容量 / 換碟 |
| Create Step 2 | `Enter` | 下一步 |
| Create Step 3 | `Tab` `Enter` | 切換欄位 / 建立 |
| RAM Cache | `←→` | 調整借用量 |
| RAM Cache | `↑↓` `Enter` | 選擇 Apply/Reset/Back |
| Destroy | `Y` | 確認刪除 |

---

## 注意事項

- **系統碟無法使用** — 掛載 `/` 的硬碟會標記 `[ROOT]` 並鎖定
- **已掛載硬碟無法使用** — 標記 `[MOUNTED]` 的硬碟會鎖定
- **已切過的碟** — 如果硬碟已經被 carve 過（存在 `tv_*_carve` dm target），程式會報錯並提示先解除
- **carve 部分會被覆蓋** — dm-linear 從 sector 0 開始，carve 部分的資料將永久消失。操作前請備份
- **每顆碟只能 carve 一次** — dm-linear 從 sector 0 開始，第二次 carve 會覆蓋第一次
- 需要 root 權限執行所有操作
- RAM Cache 設定在退出程式時**自動還原**
- 建立 volume 時任何步驟失敗會**自動回滾**

---

## 重開機保留

TieredVol 的 volume 預設**不會在重開機後自動恢復**。要實現開機自動重建，需要安裝 systemd service。

### 安裝

```bash
cd TieredVol
make
sudo make install
sudo systemctl daemon-reload
sudo systemctl enable tieredvol-restore
```

安裝後，每次建立 volume 時，systemd service 會在開機時自動讀取 `/etc/tieredvol/*.conf` 並重建。

### 手動測試

```bash
# 模擬還原（不真的動手）
sudo tieredvol-restore.sh --dry-run

# 正式還原
sudo tieredvol-restore.sh

# 查看日誌
journalctl -u tieredvol-restore
```

### 管理

```bash
sudo systemctl status tieredvol-restore   # 查看狀態
sudo systemctl disable tieredvol-restore  # 停用開機自動還原
sudo systemctl enable tieredvol-restore   # 重新啟用
```

### 運作原理

```
開機 → systemd 啟動 tieredvol-restore.service
     → 讀取 /etc/tieredvol/*.conf
     → 依序重建：dm-linear → pvcreate → vgcreate → lvcreate → mount
```

每個 volume 的 config 檔包含所有必要參數（碟名、容量、檔案系統、掛載點、stripe 大小），restore script 會逐一讀取並重建。
