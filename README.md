# TieredVol-DRIVER

A kernel-level Device Mapper target for weighted striping across heterogeneous storage devices.

TieredVol-DRIVER replaces the userspace I/O path with a kernel dm-target module (`tieredvol.ko`), achieving near-zero overhead. Applications interact with tiered storage through standard `write()`/`read()` syscalls on `/dev/mapper/<name>`.

```
Application
      │
      ▼
  write() / read()         ← Standard POSIX I/O
      │
      ▼
  VFS → bio                ← Kernel bio path
      │
      ▼
  tieredvol_submit()       ← Kernel module: weighted dispatch per-bio
      │
      ▼
  Disk A / Disk B / ...    ← Direct to underlying block devices
```

### What This Prototype Validates

- Kernel-level weighted bio dispatch (proportional to disk speed)
- Bio splitting for bios spanning multiple disk regions
- Segment-based mapping for unequal disk capacities
- Logical ↔ Physical offset mapping (zero-copy)
- Zero overhead from syscall/copy/CQE
- Mirror/RAID1 redundancy (bio_alloc_clone + fire-and-forget)
- Weight-borrowing: 慢碟塞車時，其 chunk 暫時寫入快碟的 over-provisioned 借用區，
  並以持久化 per-block 表記錄（讀寫皆可正確解析、重載後仍一致）
- Per-disk I/O statistics (in-flight tracking, completion counters)
- Integrity (CRC32C), atomic (O_DIRECT), crypto (AES-256-XTS) passthrough
- Error detection with per-disk error_count and degraded mode

### Key Results (kernel dm-target v5.0, fio + io_uring + QD=256)

| Config | Write | Read | vs Raw | vs LVM |
|--------|-------|------|--------|--------|
| 4-disk [nvme,sdb,sdc,sdd] | **3325 MB/s** | **4063 MB/s** | 81%/99% | +136%/+122% |
| 3-disk [nvme,sdb,sdc] | 2893 MB/s | 3938 MB/s | 83%/100% | +111%/+174% |
| 2-disk [nvme,sdb] | 2179 MB/s | 3261 MB/s | 82%/97% | +129%/+238% |

> **⚠ 舊拓撲 + io_uring 數字（參考用）**：上表是舊拓撲（nvme0=WD @ PCIe3.0 x4、nvme1=P3）
> 以 io_uring QD=256 測得；io_uring + dm 深佇列會把數字灌高，**不能作為疊碟驗收依據**。
> 疊碟（1→2→3→4）驗收以 **libaio + 計數器精確比對** 為準，最新實測見 `docs/RESULTS.md`。

Hardware（現況拓撲，2026-08-13 晚重啟後又交換回 8/12 配置）: A=WD SN750 500G `nvme0n1` (PCIe 3.0 x4, solo 寫/讀 2069/3132 MB/s), B=P3 Plus 1T `nvme1n1` (PCIe 2.0 x1, 413/418 MB/s), C=MX500 500G `sdc` (518/493 MB/s), D=WD Blue 250G `sdb` (223/535 MB/s), E=BX100 233G `sda` (未入 config)。權重 6:1:1:1。（8/13 午曾交換為 A=nvme1n1；碟位變更紀錄見 docs/RESULTS.md）

vs LVM（現行拓撲，2026-08-12，同 fio libaio d32，見 `docs/RESULTS.md`）：LVM striped 4-disk W 1575 / R 1590（256K 最佳）、3-disk W 1180 / R 1190、2-disk W 786 / R 796。TieredVol weighted striping 於 1M 順序寫/讀達 **LVM 的 1.96–3.5x**；僅 4K 小寫入 LVM 較快（676 vs 511 MiB/s）。舊拓撲 io_uring 數字（W 1407 / R 1829）僅參考。

### What Is Intentionally Excluded

- Filesystem implementation
- Data redundancy via parity (RAID5/6)
- Crash consistency / journaling
- Metadata recovery
- Dynamic online rebalancing

---

## Quick Start

```bash
git clone https://github.com/yu20060715/TieredVol-DRIVER.git
cd TieredVol-DRIVER

# Build kernel module + run unit tests
make module

# Load kernel module
sudo modprobe tieredvol

# Create a weighted volume from a config file (格式見 docs/CONFIG.md)
# /etc/tieredvol/fastpool.conf 範例（diskN_name 用 stable by-id 路徑，見 docs/CONFIG.md）:
#   [weighted_striping]
#   version=1
#   chunk_size=1048576
#   segment_count=1
#   disk_count=3
#   disk0_name=/dev/disk/by-id/nvme-WDS500G3X0C-00SJG0_200705800588
#   disk1_name=/dev/disk/by-id/ata-WDC_WDS250G2B0A_201965800292
#   disk2_name=/dev/disk/by-id/ata-CT500MX500SSD1_2129E5B858A9
#   seg0_begin=0
#   seg0_end=21474836480
#   seg0_count=3
#   seg0_disks=0,1,2
#   seg0_weight=6,2,1
S=$(python3 -c "import configparser;c=configparser.ConfigParser();c.read('/etc/tieredvol/fastpool.conf');print(int(c['weighted_striping']['seg0_end'])//512)")
echo "0 $S tieredvol /etc/tieredvol/fastpool.conf" | sudo dmsetup create fastpool

# Benchmark (libaio — 別用 io_uring QD=256，會灌高假數字):
sudo fio --name=bench --filename=/dev/mapper/fastpool --rw=write --bs=1M \
  --size=8G --direct=1 --ioengine=libaio --iodepth=32 --numjobs=1 --end_fsync=1

# Remove
sudo dmsetup remove fastpool
```

---

## Boot Persistence（開機持久）

重啟後自動載入 `tieredvol.ko` 並重建卷（免手動）：

```bash
sudo scripts/install_boot.sh        # 安裝 + enable；--uninstall 可移除
```

交付內容：

1. `/etc/modules-load.d/tieredvol.conf` — 開機載入 module
2. `/etc/systemd/system/tieredvol.service` — oneshot：開機 `create`（`After=systemd-modules-load.service blockdev.target` + by-id 碟就緒重試）、關機 `remove`
3. `/usr/local/sbin/tieredvol-boot` — helper，遍歷 `/etc/tieredvol/*.conf` 建卷
4. `/etc/tieredvol/*.conf` — 從 repo `configs/` 安裝的 active config（`tv_s1/s2/s3/s4` + `tv_mir`，**現今拓撲：WD=disk0**）

限制：

- **碟位綁定靠 by-id**（serial）— 重啟/實體換槽不變；`/dev/sdX`/`nvmeXnY` 代號會變，故 config 一律用 `/dev/disk/by-id/...`
- **斷電不保證 `.borrow` 借出表**：只在正常 `dmsetup remove`/關機 ExecStop 時寫回 `<config>.borrow`；直接斷電時已借出資料仍在、但映射表未存
- 測試卷，未掛載於 root

測試（無需重啟）：`sudo systemctl restart tieredvol.service && ls /dev/mapper/tv_*`

---

## Weighted Striping vs Fixed-Size Striping

Traditional RAID0 and LVM striping use a fixed stripe size for all disks. When storage devices exhibit significantly different sequential bandwidth, fixed striping underutilizes faster devices.

```
Fixed striping (LVM):          Weighted striping (TieredVol):

NVMe  3100 MB/s → 1 chunk     NVMe  3100 MB/s → 7 chunks = 1792KB
SATA  1700 MB/s → 1 chunk     SATA  1700 MB/s → 4 chunks = 1024KB
SATA   800 MB/s → 1 chunk     SATA   800 MB/s → 2 chunks = 512KB
SATA   450 MB/s → 1 chunk     SATA   450 MB/s → 1 chunk  =  256KB

NVMe idle waiting for SATA     All disks finish at approximately
→ throughput ≈ slowest disk    the same time → higher aggregate
```

**Weight is generated at initialization** via a benchmark that measures sequential write speed with SLC cache warm-up (2GB pre-write).

---

## CLI Usage

### Tiered Storage (Kernel dm target)

```bash
# Create weighted volume from config (見 docs/CONFIG.md)
S=$(python3 -c "import configparser;c=configparser.ConfigParser();c.read('/etc/tieredvol/fastpool.conf');print(int(c['weighted_striping']['seg0_end'])//512)")
echo "0 $S tieredvol /etc/tieredvol/fastpool.conf" | sudo dmsetup create fastpool

# Benchmark the volume (libaio，見 docs/TEST_PLAN.md)
sudo fio --filename=/dev/mapper/fastpool --rw=write --bs=1M --size=8G \
  --direct=1 --ioengine=libaio --iodepth=32 --numjobs=1
sudo fio --filename=/dev/mapper/fastpool --rw=read --bs=1M --size=8G \
  --direct=1 --ioengine=libaio --iodepth=32 --numjobs=1

# Show volume metadata
sudo dmsetup table fastpool
sudo dmsetup status fastpool

# 控制/查詢 (dm message)，完整清單見 docs/MAPPING.md
sudo dmsetup message fastpool 0 "status"
sudo dmsetup message fastpool 0 "show_stats"
sudo dmsetup message fastpool 0 "show_log"
sudo dmsetup message fastpool 0 "borrow_on"

# Remove volume
sudo dmsetup remove fastpool
```

### Disk Management

權重由 `scripts/auto_weight.sh`（python3 + fio，見 docs/CONFIG.md「weight 計算」）產生後填入 config。

---

## Requirements

- Linux kernel 6.x (tested on 6.14.0-27)
- `lvm2` `dmsetup` `gcc` `make`
- `linux-headers-$(uname -r)` (for kernel module build)
- Root privileges (sudo)

### Install Dependencies

```bash
# Debian / Ubuntu
sudo apt install lvm2 gcc make linux-headers-$(uname -r)
```

## Build

```bash
make module             # Build kernel module (主要產物)
make test               # Unit tests (test_map + test_stripe_kernel，免 sudo)
make lint               # Syntax check tests
sudo make module_install # Install kernel module
sudo depmod -a          # Update module dependencies
sudo make test-full     # Unit + integration (需 /dev/mapper/tv_* 存在)
make clean              # Remove all build artifacts
sudo make install       # 等同 module_install（需 root）
```

---

## Project Structure

```
TieredVol-DRIVER/
├── README.md
├── ARCHITECTURE.md
├── Makefile
├── common/
│   └── tieredvol_meta_format.h     # Shared constants (kernel + tests)
├── configs/                        # Active configs（現今拓撲，by-id 碟名）
│   └── tv_s1.conf / tv_s2.conf / tv_s3.conf / tv_s4.conf / tv_mir.conf
├── driver/                         # Kernel dm-target module
│   ├── tieredvol.h                 # Central header: all structs + exports
│   ├── tieredvol_core.c            # DM lifecycle: ctr/dtr/map/status/init/exit
│   ├── tieredvol_stripe.c          # Stripe-split helpers (B path)
│   ├── tieredvol_map.c             # Logical→Physical: static/random + borrow
│   ├── tieredvol_borrow.c          # Weight-borrowing redirect table (block index)
│   ├── tieredvol_badmap.c          # Bad block map (bitmap per disk)
│   ├── tieredvol_mirror.c          # Mirror I/O + pending tracking + end_io
│   ├── tieredvol_wc.c              # Write coalescing buffer + flush
│   ├── tieredvol_log.c             # Log ring buffer
│   ├── tieredvol_meta.c            # Metadata read/write (config file)
│   ├── tieredvol_sysfs.c           # sysfs interface
│   ├── tieredvol_message.c         # DM message dispatch
│   ├── tieredvol_msg_stats.c       # Stats message handlers
│   ├── tieredvol_msg_policy.c      # Policy/borrow handlers
│   ├── tieredvol_msg_mirror.c      # Mirror/rebuild handlers
│   ├── tieredvol_msg_config.c      # Config/log handlers
│   └── Kbuild
├── tests/
│   ├── test_common.h               # Assertion macro (shared)
│   ├── test_map.c                  # Logical→Physical mapping tests (301 assertions)
│   ├── test_stripe_kernel.c        # Kernel stripe source vs mock headers (27 assertions)
│   └── mock/linux/                 # Minimal mock kernel headers for userspace tests
├── docs/
│   ├── ARCHITECTURE.md            # Kernel 架構總覽
│   ├── DESIGN.md                  # 設計本質 + 不要亂改清單（先讀）
│   ├── RESULTS.md                 # 疊碟測試結果（現況拓撲）
│   ├── TEST_PLAN.md               # 疊碟驗收計畫（主目標）
│   ├── CONFIG.md                  # 設定檔參考
│   ├── MAPPING.md                 # 映射/權重實驗紀錄
│   ├── MIRROR.md                  # Mirror/RAID1 設計
│   ├── WC.md                      # Write coalescing 設計
│   ├── ROADMAP.md                 # Roadmap（已實作標記）
│   └── PARTITION_SPLITTING.md     # Weighted striping algorithm（歷史 prototype）
└── scripts/
    ├── auto_weight.sh             # fio 測速 + 產生 seg weight（docs/CONFIG.md）
    ├── conf_to_byid.py            # 批次把 diskN_name 轉 stable by-id（含 crc32 重算）
    ├── install_boot.sh            # 開機持久安裝（modules-load.d + systemd unit）
    ├── msg_probe.sh               # dm message 全 handler 回歸探測
    ├── borrow_verify.sh           # borrow 借出/持久化/重載驗證
    └── install_deps.sh            # Install dependencies + build
```

### Kernel Module Architecture

| File | Responsibility |
|------|---------------|
| `tieredvol_core.c` | DM lifecycle, I/O entry (`tieredvol_map`), module init/exit |
| `tieredvol_stripe.c` | Stripe-split helpers: boundaries, ranges, parallel submit |
| `tieredvol_map.c` | Logical→Physical mapping (static/random) |
| `tieredvol_borrow.c` | Weight-borrowing: per-block redirect table, lookup, persistence |
| `tieredvol_badmap.c` | Bad block map: bitmap per disk, set/clear/query, IO error marking |
| `tieredvol_mirror.c` | Mirror I/O, pending tracking (per-CPU), mirror/destroy ctx, parallel end_io |
| `tieredvol_wc.c` | Write-coalescing buffer + flush, init/destroy ctx (Phase 1 C) |
| `tieredvol_log.c` | Log ring buffer |
| `tieredvol_meta.c` | Config file parse/save, CRC32 validation |
| `tieredvol_message.c` | DM message commands (show/set/modify at runtime) |
| `tieredvol_sysfs.c` | sysfs attributes |

### I/O Flow

```
User → write()/read() → VFS → bio → tieredvol_map()
  → static stripe mapping (weight-based)        # deterministic placement
  → [borrow?] → tv_borrow_redirect()            # slow-disk chunk → borrow area
   → [mirror?] → bio_alloc_clone() → submit_bio(clone)
   → submit_bio(bio)               # Submit to physical disk directly
  ...
  → tieredvol_end_io()            # Completion: in_flight--, retry if needed
```

---

## Kernel Module (v5.0)

The `tieredvol` dm target processes bios in-kernel:

1. **bio arrives** at the dm target (from VFS `write()`/`read()`)
2. **Map**: deterministic weight-based stripe placement → (disk, physical_offset)
3. **Borrow**: if the source disk is backlogged, `tv_borrow_redirect()` moves
   whole 128 KB blocks to the least-loaded disk's borrow area and records them
4. **Mirror**: if enabled, `bio_alloc_clone()` + fire-and-forget to redundant disk
5. **Redirect**: `bio_set_dev()` + sector update → DM core submits to underlying device

Key features:
- `DM_TARGET_NOWAIT`: Non-blocking bio dispatch
- `flush_bypasses_map`: Flush FUA bios bypass the map function
- `dm_set_target_max_io_len()`: Bio splitting at chunk boundaries
- **Weight-borrowing**: slow-disk chunks redirected to the least-loaded
  disk's over-provisioned borrow area when the source is backlogged; every
  redirect is recorded in a persistent per-block table (`<config>.borrow`)
  so reads and later writes resolve the same destination
- **Per-CPU pending arrays**: lockless write path for mirror tracking
- **Mempool**: zero OOM for mirror/retry contexts
- Mirror/RAID1: per-segment `mirror_enabled` + `mirror_disk` config
- CRC32 config validation (two-pass parse)
- Error detection with per-disk error_count and degraded mode

Key constants:
- `TV_MAX_DISKS` = 16
- `TV_MAX_SEGS` = 16

### Metadata Format

Config files are stored in `/etc/tieredvol/<name>.conf` (INI format):

```ini
[weighted_striping]
version=1
chunk_size=1048576
segment_count=1
disk_count=2
disk0_name=/dev/disk/by-id/nvme-WDS500G3X0C-00SJG0_200705800588
disk1_name=/dev/disk/by-id/ata-WDC_WDS250G2B0A_201965800292
seg0_begin=0
seg0_end=931520000000
seg0_count=2
seg0_disks=0,1
seg0_weight=2,1
seg0_stripe=3145728
seg0_mirror=1          # optional: mirror to disk index 1
seg0_policy=static     # optional: static (default), random
```

```ini
[runtime]                          # optional overrides (persisted on message)
borrow_enable=1                    # weight-borrowing on/off
borrow_watermark_kb=256            # 觸發借出的 src in-flight 門檻
borrow_area_mb=2048,0,0,0          # 每碟借用區大小（MB）
```

---

## Limitations

- **Static weights are the placement authority** — `random` policy exists for
  layout stress only; weight-borrowing offloads *within* the static layout.
  The former dynamic (adaptive) policy was removed (address determinism broken).
- **No parity-based redundancy** — Mirror/RAID1 supported, but no RAID5/6.
- **No crash consistency** — No journaling or metadata recovery.
- **System disk cannot be used** — dm returns EBUSY on mounted root partition.
- **Module instability risk** — A kernel module bug can oops the system.
- **NVMe write cache should stay ON** — Disabling it causes -21% throughput loss on some hardware.
- **Virtual devices rejected** — Loop, ram, and zram devices are blocked by both the userspace tool and the kernel module. Only physical block devices (NVMe, SATA, SAS) are allowed.

## License

MIT
