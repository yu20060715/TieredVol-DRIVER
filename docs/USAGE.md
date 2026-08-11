# TieredVol 使用教學

## 文件索引

| 文件 | 說明 |
|------|------|
| `DESIGN.md` | 設計本質 + 承重牆（**動手改 code 前必讀**） |
| `ARCHITECTURE.md` | 整體資料流與模組職責 |
| `RESULTS.md` | 疊碟測試結果（現況拓撲，2026-08-11） |
| `TEST_PLAN.md` | 疊碟驗收計畫（主目標：1→2→3→4 碟） |
| `MAPPING.md` | Logical→Physical 映射演算法 |
| `MIRROR.md` | Mirror RAID1 實作 |
| `WC.md` | Write Coalescing 寫入合併 |
| `CONFIG.md` | 設定檔完整參考 |
| `ROADMAP.md` | Roadmap（已實作標記） |
| `PARTITION_SPLITTING.md` | Segment 切分原理 |

## 系統需求

- Linux（已測試 Ubuntu 24.04, kernel 6.14）
- `lvm2` `dmsetup` `gcc` `make`
- Root 權限（sudo）

### 安裝依賴

```bash
# Debian / Ubuntu
sudo apt install lvm2 gcc make

# Fedora / RHEL
sudo dnf install lvm2 gcc make

# Arch
sudo pacman -S lvm2 gcc make
```

### 編譯

```bash
cd TieredVol-DRIVER
make              # 編譯 tiered_setup
sudo make install # 安裝到 /usr/local/bin

# 編譯 kernel module（需要 kernel header）
cd driver
make -C /lib/modules/$(uname -r)/build M=$(pwd) modules
sudo insmod tieredvol.ko
```

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
# 依序測試（峰值速度）
sudo tiered_setup --bench --disks sdb,sdc,nvme0n1

# 依序測試（含 SLC cache 預熱，持久速度）
sudo tiered_setup --bench --disks sdb,sdc,nvme0n1 --warmup

# 依序測試
sudo tiered_setup --bench --disks sdb,sdc --sequential
```

預設 parallel 模式，多顆碟同時跑。輸出每顆碟的 Write / Read 速度（MB/s）。
加 `--warmup` 會先寫 2GB 填滿 SLC cache 再測速，得到持久速度。

### 建立 Volume

```bash
# 預設使用 kernel dm-target（weighted striping）
sudo tiered_setup --create --name fastpool --disks nvme0n1,sdb

# 指定 carve 容量（DM path 和 LVM path 都支援）
sudo tiered_setup --create --name fastpool --disks nvme0n1:300,sdb:200

# LVM 模式（需加 --lvm）
sudo tiered_setup --create --name fastpool --lvm --disks sdb:300,sdc:200 --fs ext4 --mount /mnt/fast
```

參數說明：
| 參數 | 說明 | 必填 |
|------|------|------|
| `--name` | Volume 名稱 | 是 |
| `--disks` | 硬碟列表（格式：`碟名` 或 `碟名:GB`） | 是 |
| `--fs` | 檔案系統（ext4/xfs/btrfs/none） | 否，預設 ext4 |
| `--mount` | 掛載點 | 否 |
| `--lvm` | 使用 LVM striped（預設為 kernel dm-target） | 否 |
| `--stripesize` | stripe 大小（KB，僅 LVM） | 否，自動判斷 |

不指定 `:GB` 時，使用硬碟全部空間（扣 1GB）。切碟語法在 DM path（預設）和 LVM path（`--lvm`）都支援。

#### 建立流程

程式會依序執行以下步驟：

1. **碟驗證** — 先檢查碟名是否為實體裝置（loop/ram/zram 直接拒絕），接著檢查每顆碟的狀態：
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

支援刪除 LVM striped volume 和 kernel dm-target volume。程式會自動偵測 volume 類型並執行對應的清理流程。

### 建立 Kernel dm-target Volume（預設）

```bash
# 使用 tiered_setup（自動測速、計算權重、載入模組、建立 dm target）
sudo tiered_setup --create --name fastpool --disks nvme0n1,sda

# 指定 carve 容量（不指定即用整碟）
sudo tiered_setup --create --name fastpool --disks nvme0n1:100,sda:50

# 或手動建立 config + dmsetup
cat > /etc/tieredvol/fastpool.conf << 'EOF'
[weighted_striping]
version=1
chunk_size=1048576
segment_count=1
disk_count=2
disk0_name=/dev/nvme0n1
disk1_name=/dev/sda
seg0_begin=0
seg0_end=931520000000
seg0_count=2
seg0_disks=0,1
seg0_weight=2,1
seg0_stripe=3145728
EOF
sudo dmsetup create fastpool --table '0 2097152 tieredvol /etc/tieredvol/fastpool.conf'
```

預設建立 kernel dm-target volume，流程如下：
1. **碟驗證** — 檢查虛擬裝置（loop/ram/zram 拒絕）、系統碟、已掛載碟
2. **Configuration 顯示** — 列出每顆碟的 carve 大小、剩餘空間、速度
3. 依磁碟速度計算 weight（benchmark 自動測速）
4. 建立 segment 資料（加權條紋）
5. 儲存 metadata 到 `/etc/tieredvol/<name>.conf`
6. `dmsetup create` → 建立 tieredvol dm target
7. 之後用 `fio` 工具操作 `/dev/mapper/<name>`，進行 I/O 測試

支援 `碟名:GB` 語法指定 carve 容量，不指定即用整碟（扣 1GB）。

---

## I/O 工具 (fio)

> **tiered_io 已移除（v5.0）：** kernel dm-target 已接手所有 I/O dispatch，標準 `write()`/`read()` 即可操作 `/dev/mapper/<name>`。Benchmark 使用 `fio`。

### 建立 tieredvol Volume

使用 `tiered_setup` CLI 工具即可：

```bash
# 建立 weighted volume（自動測速、計算權重、載入模組、建立 dm target）
sudo tiered_setup --create --name fastpool --disks nvme0n1,sdb,sdc
```

或手動 config 格式（INI）：

```ini
[weighted_striping]
version=1
chunk_size=1048576
segment_count=1
disk_count=2
disk0_name=/dev/nvme0n1
disk1_name=/dev/sdb
seg0_begin=0
seg0_end=931520000000
seg0_count=2
seg0_disks=0,1
seg0_weight=2,1
seg0_stripe=3145728
```

使用 `dmsetup create` 載入：

```bash
sudo dmsetup create fastpool --table '0 2097152 tieredvol /etc/tieredvol/fastpool.conf'
```

### Benchmark（fio）

```bash
# Write benchmark — O_DIRECT（真實磁碟速度）
sudo fio --filename=/dev/mapper/v2bench \
    --ioengine=io_uring --direct=1 \
    --rw=write --bs=1M --size=1G --iodepth=256

# Read benchmark — O_DIRECT
sudo fio --filename=/dev/mapper/v2bench \
    --ioengine=io_uring --direct=1 \
    --rw=read --bs=1M --size=1G --iodepth=256

# Random read — O_DIRECT
sudo fio --filename=/dev/mapper/v2bench \
    --ioengine=io_uring --direct=1 \
    --rw=randread --bs=4k --size=256M --iodepth=256
```

參數說明：
| 參數 | 說明 |
|------|------|
| `--filename` | DM target 裝置路徑 |
| `--ioengine=io_uring` | 使用 io_uring 非同步 I/O |
| `--direct=1` | O_DIRECT，繞過 page cache |
| `--rw` | 讀寫模式：`write`、`read`、`randread` |
| `--bs` | Block size（1M = 順序，4k = 隨機） |
| `--size` | 測試資料量 |
| `--iodepth` | I/O queue depth（建議 256） |

### 驗證 Block Stats

```bash
# 測試前先 reset stats
sudo sh -c 'for f in /sys/block/dm-*/stat; do echo 0 0 0 0 0 0 0 0 0 0 0 > $f; done'

# 跑 benchmark
sudo fio --filename=/dev/mapper/v2bench \
    --ioengine=io_uring --direct=1 \
    --rw=write --bs=1M --size=1G --iodepth=256

# 測試後查看 stats（確認哪些 disk 真的被寫入）
cat /sys/block/nvme0n1/stat
cat /sys/block/sdb/stat
```

> Write 領域的 sectors 計算方式：`(sectors / 2) / 1024 / 1024` = MiB。每個 disk 的寫入量應符合 weight 比例。

## 注意事項

- **建立 DM device 前**，確認 `/dev/mapper/<name>` 沒有殘留的普通檔案（fio `O_CREAT` 會建立假檔案，導致 O_DIRECT 數據膨脹）
- **O_DIRECT 測試**，`--filename` 必須是 block device 或 regular file，不可 pipe
- **驗證**：benchmark 跑完後檢查 block stats，確認每個 disk 的寫入量符合 weight 比例
- **系統碟無法使用** — 掛載 `/` 的硬碟會標記 `[ROOT]` 並鎖定
- **已掛載硬碟無法使用** — 標記 `[MOUNTED]` 的硬碟會鎖定
- **虛擬裝置無法使用** — Loop、ram、zram 等虛擬裝置會被 userspace 工具和 kernel module 共同阻擋。僅允許實體區塊裝置（NVMe、SATA、SAS）
- **已切過的碟** — 如果硬碟已經被 carve 過（存在 `tv_*_carve` dm target），程式會報錯並提示先解除
- **carve 部分會被覆蓋** — dm-linear 從 sector 0 開始，carve 部分的資料將永久消失。操作前請備份
- **每顆碟只能 carve 一次** — dm-linear 從 sector 0 開始，第二次 carve 會覆蓋第一次
- 需要 root 權限執行所有操作
- 建立 volume 時任何步驟失敗會**自動回滾**
