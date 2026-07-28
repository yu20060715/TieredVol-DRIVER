# TieredVol 使用教學

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

支援刪除 LVM striped volume 和 weighted I/O scheduler volume。程式會自動偵測 `.scheduler` 檔案，如果是 scheduler volume 則只清理 dm-linear targets。

### 建立 Weighted I/O Scheduler Volume

```bash
# 方式 1：使用 tiered_setup（推薦）
sudo tiered_setup --create --name fastpool \
    --disks nvme0n1:1000,sda:500 \
    --scheduler

# 方式 2：手動建立 config + dmsetup
cat > /etc/tieredvol/v2bench.conf << 'EOF'
crc32=49359161
v2disk_start[0]=0
v2disk_start[1]=1073741824
v2disk_end[0]=1073741824
v2disk_end[1]=2147483648
v2weight[0]=7
v2weight[1]=1
EOF
sudo dmsetup create v2bench --table '0 4194304 v2 /etc/tieredvol/v2bench.conf'
```

> **手動建立時**：確認 `/dev/mapper/v2bench` 為 symlink 指向 `../dm-X`。若為普通檔案需先 `rm -f`。

加上 `--scheduler` 參數後，不會建立 LVM volume，而是：
1. 建立 dm-linear targets
2. 依磁碟速度計算 weight
3. 建立 segment 資料
4. 儲存 metadata 到 `/etc/tieredvol/fastpool.scheduler`

之後用 `fio` 工具操作 `/dev/mapper/fastpool`，進行 I/O 測試。

---

## I/O 工具 (fio)

> **tiered_io 已移除（v5.0）：** kernel dm-target 已接手所有 I/O dispatch，標準 `write()`/`read()` 即可操作 `/dev/mapper/<name>`。Benchmark 使用 `fio`。

### 建立 DM Volume

```bash
# 1. 建立 config（例如 2-disk 7:1 weight）
cat > /etc/tieredvol/v2bench.conf << 'EOF'
crc32=49359161
v2disk_start[0]=0
v2disk_start[1]=1073741824
v2disk_end[0]=1073741824
v2disk_end[1]=2147483648
v2weight[0]=7
v2weight[1]=1
EOF

# 2. 建立 dm device（需先確認 symlink 是否存在）
sudo dmsetup create v2bench --table '0 4194304 v2 /etc/tieredvol/v2bench.conf'

# 3. 確認 symlink 指向 block device
ls -la /dev/mapper/v2bench
```

> **重要：** 建立 DM device 後，`/dev/mapper/v2bench` 必須是 symlink 指向 `../dm-X`。若為普通檔案，表示 symlink 遺失，需先 `rm -f /dev/mapper/v2bench` 再重建。

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
- **已切過的碟** — 如果硬碟已經被 carve 過（存在 `tv_*_carve` dm target），程式會報錯並提示先解除
- **carve 部分會被覆蓋** — dm-linear 從 sector 0 開始，carve 部分的資料將永久消失。操作前請備份
- **每顆碟只能 carve 一次** — dm-linear 從 sector 0 開始，第二次 carve 會覆蓋第一次
- 需要 root 權限執行所有操作
- 建立 volume 時任何步驟失敗會**自動回滾**
