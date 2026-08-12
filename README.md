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
- Adaptive striping with EMA-based load balancing
- Staleness detection with grace period (timer-based monitoring)
- Wear leveling (write-count-aware weight adjustment)
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

Hardware（現況拓撲，2026-08-12 重啟後碟位交換）: A=WD SN750 500G `nvme0n1` (PCIe 3.0 x4, 平行仲裁 ~2009 MB/s), B=P3 Plus 1T `nvme1n1` (PCIe 2.0 x1, ~389 MB/s), C=MX500 500G `sdc` (~470 MB/s), D=WD Blue 250G `sdb` (~346 MB/s), E=BX100 233G `sda` (未入 config)。權重 6:1:1:1。

vs LVM（現行拓撲，2026-08-12，同 fio libaio d32，見 `docs/RESULTS.md`）：LVM striped 4-disk W 1575 / R 1590（256K 最佳）、3-disk W 1180 / R 1190、2-disk W 786 / R 796。TieredVol weighted striping 於 1M 順序寫/讀達 **LVM 的 1.96–3.5x**；僅 4K 小寫入 LVM 較快（676 vs 511 MiB/s）。舊拓撲 io_uring 數字（W 1407 / R 1829）僅參考。

### What Is Intentionally Excluded

- Filesystem implementation
- Data redundancy via parity (RAID5/6)
- Crash consistency / journaling
- Metadata recovery
- Dynamic online rebalancing
- Write cache (Phase 1 C, enabled via `wc_enabled=1` module_param; enabled by default)

---

## Quick Start

```bash
git clone https://github.com/yu20060715/TieredVol-DRIVER.git
cd TieredVol-DRIVER

# Build userspace tools + kernel module
make
make module

# Create a weighted volume (loads kernel module automatically)
sudo ./tiered_setup --create --name fastpool --disks nvme0n1,sdb,sdc

# Or manual fio (libaio — 別用 io_uring QD=256，會灌高假數字):
sudo fio --name=bench --filename=/dev/mapper/fastpool --rw=write --bs=1M \
  --size=8G --direct=1 --ioengine=libaio --iodepth=32 --numjobs=1 --end_fsync=1

# Remove
sudo ./tiered_setup --remove --name fastpool
```

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
# Create weighted volume (kernel module)
sudo tiered_setup --create --name fastpool --disks nvme0n1,sdb

# Benchmark the volume (libaio，見 docs/TEST_PLAN.md)
sudo fio --filename=/dev/mapper/fastpool --rw=write --bs=1M --size=8G \
  --direct=1 --ioengine=libaio --iodepth=32 --numjobs=1
sudo fio --filename=/dev/mapper/fastpool --rw=read --bs=1M --size=8G \
  --direct=1 --ioengine=libaio --iodepth=32 --numjobs=1

# Show volume metadata
sudo dmsetup table fastpool
sudo dmsetup status fastpool

# Remove volume
sudo tiered_setup --remove --name fastpool
```

### LVM Striping (Legacy)

```bash
# Create LVM striped volume (--lvm flag required)
sudo tiered_setup --create --name pool --disks sdb:300,sdc:200 --lvm --fs ext4 --mount /mnt/pool

# Benchmark
sudo fio --filename=/dev/mapper/tv_vg_pool-tv_lv_pool --rw=write --bs=128k --size=5G \
  --direct=1 --ioengine=io_uring --iodepth=256 --numjobs=1

# Remove
sudo tiered_setup --remove --name pool
```

### Disk Management

```bash
sudo tiered_setup --list         # List all disks
sudo tiered_setup --bench --disks sdb,sdc,nvme0n1  # Benchmark disks
sudo tiered_setup --status       # Show status
```

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
make                    # Build tiered_setup
make test               # Unit tests (235 assertions, 4 suites, no sudo)
make module             # Build kernel module
sudo make module_install # Install kernel module
sudo depmod -a          # Update module dependencies
sudo make test-full     # Unit + integration tests
make clean              # Remove all build artifacts
sudo make install       # Install to /usr/local/bin/
```

---

## Project Structure

```
TieredVol-DRIVER/
├── README.md
├── ARCHITECTURE.md
├── Makefile
├── common/
│   └── tieredvol_meta_format.h     # Shared constants (kernel + userspace)
├── driver/                         # Kernel dm-target module
│   ├── tieredvol.h                 # Central header: all structs + exports
│   ├── tieredvol_core.c            # DM lifecycle: ctr/dtr/map/status/init/exit
│   ├── tieredvol_stripe.c          # Stripe-split helpers (B path)
│   ├── tieredvol_map.c             # Logical→Physical: static/adaptive/random
│   ├── tieredvol_mirror.c          # Mirror I/O + pending tracking + end_io
│   ├── tieredvol_log.c             # Log ring buffer + EMA decay timer
│   ├── tieredvol_meta.c            # Metadata read/write (config file)
│   ├── tieredvol_sysfs.c           # sysfs interface
│   ├── tieredvol_message.c         # DM message dispatch
│   ├── tieredvol_msg_stats.c       # Stats message handlers
│   ├── tieredvol_msg_policy.c      # Policy/adaptive handlers
│   ├── tieredvol_msg_mirror.c      # Mirror/rebuild handlers
│   ├── tieredvol_msg_config.c      # Config/log handlers
│   └── Kbuild
├── src/                            # Userspace tools
│   ├── main.c                      # CLI entry point
│   ├── tiered_types.h              # Core data structures
│   ├── tiered_common.h             # Input validation
│   ├── tiered_metadata.c           # Metadata save/load
│   ├── tiered_partition.c          # Weight computation + segments
│   ├── tiered_benchmark.c          # Raw device benchmark
│   ├── warmup.c                    # SLC cache warm-up
│   ├── cmd_create.c                # Volume creation (kernel dm-target + LVM)
│   ├── cmd_remove.c                # Volume removal + status
│   ├── exec_helper.c               # fork/exec wrappers
│   ├── setup_discover.c            # Block device discovery
│   └── setup_bench.c               # Parallel disk benchmarking
├── tests/
│   ├── test_common.c               # Input validation tests
│   ├── test_partition.c            # Weight/segment tests
│   └── test_metadata.c             # Metadata round-trip tests
├── docs/
│   ├── ARCHITECTURE.md            # Kernel 架構總覽
│   ├── DESIGN.md                  # 設計本質 + 不要亂改清單（先讀）
│   ├── RESULTS.md                 # 疊碟測試結果（現況拓撲）
│   ├── TEST_PLAN.md               # 疊碟驗收計畫（主目標）
│   ├── CONFIG.md                  # 設定檔參考
│   ├── USAGE.md                   # Usage tutorial
│   ├── MAPPING.md                 # 映射/權重實驗紀錄
│   ├── MIRROR.md                  # Mirror/RAID1 設計
│   ├── WC.md                      # Write coalescing 設計
│   ├── ROADMAP.md                 # Roadmap（已實作標記）
│   └── PARTITION_SPLITTING.md     # Weighted striping algorithm
└── scripts/
    └── install_deps.sh             # Install dependencies + build
```

### Kernel Module Architecture

| File | Responsibility |
|------|---------------|
| `tieredvol_core.c` | DM lifecycle, I/O entry (`tieredvol_map`), module init/exit |
| `tieredvol_stripe.c` | Stripe-split helpers: boundaries, ranges, parallel submit |
| `tieredvol_map.c` | Logical→Physical mapping (static/adaptive/random dispatch) |
| `tieredvol_mirror.c` | Mirror I/O, pending tracking (per-CPU), timestamp ring, mirror/destroy ctx, parallel end_io |
| `tieredvol_wc.c` | Write-coalescing buffer + flush, init/destroy ctx (Phase 1 C) |
| `tieredvol_log.c` | Log ring buffer, EMA decay timer (load/latency/IOPS tracking) |
| `tieredvol_meta.c` | Config file parse/save, CRC32 validation |
| `tieredvol_message.c` | DM message commands (show/set/modify at runtime) |
| `tieredvol_sysfs.c` | sysfs attributes |

### I/O Flow

```
User → write()/read() → VFS → bio → tieredvol_map()
  → tv_map_logical_adaptive()     # Multi-factor scoring: load + latency + wear
  → tv_ts_submit()                # Record submit timestamp
   → [mirror?] → bio_alloc_clone() → submit_bio(clone)
   → submit_bio(bio)               # Submit to physical disk directly
  ...
  → tieredvol_end_io()            # Completion: latency delta, in_flight--
    → tv_ts_complete()            # Calculate latency
    → [mirror retry?] → schedule_delayed_work()
```

---

## Kernel Module (v5.0)

The `tieredvol` dm target processes bios in-kernel:

1. **bio arrives** at the dm target (from VFS `write()`/`read()`)
2. **Map**: `tv_map_logical_adaptive()` translates logical byte offset → (disk, physical_offset)
3. **Scoring**: multi-factor score = EMA load + EMA latency + wear penalty (lower = better)
4. **Mirror**: if enabled, `bio_alloc_clone()` + fire-and-forget to redundant disk
5. **Redirect**: `bio_set_dev()` + sector update → DM core submits to underlying device

Key features:
- `DM_TARGET_NOWAIT`: Non-blocking bio dispatch
- `flush_bypasses_map`: Flush FUA bios bypass the map function
- `dm_set_target_max_io_len()`: Bio splitting at chunk boundaries
- **Adaptive EMA dispatch**: load + latency + wear scoring per bio
- **Per-CPU pending arrays**: lockless write path for mirror tracking
- **Per-disk timestamp ring**: latency tracking for adaptive scoring
- **Mempool**: zero OOM for mirror/retry contexts
- Mirror/RAID1: per-segment `mirror_enabled` + `mirror_disk` config
- Staleness detection: adaptive timer (100ms busy / 1s idle), grace period
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
disk0_name=/dev/nvme0n1
disk1_name=/dev/sdb
seg0_begin=0
seg0_end=931520000000
seg0_count=2
seg0_disks=0,1
seg0_weight=2,1
seg0_stripe=3145728
seg0_mirror=1          # optional: mirror to disk index 1
seg0_policy=adaptive   # optional: static (default), adaptive, random
```

---

## Limitations

- **Static weights only by default** — Adaptive striping available via `set_policy` message.
- **No parity-based redundancy** — Mirror/RAID1 supported, but no RAID5/6.
- **No crash consistency** — No journaling or metadata recovery.
- **System disk cannot be used** — dm returns EBUSY on mounted root partition.
- **Module instability risk** — A kernel module bug can oops the system.
- **NVMe write cache should stay ON** — Disabling it causes -21% throughput loss on some hardware.
- **Virtual devices rejected** — Loop, ram, and zram devices are blocked by both the userspace tool and the kernel module. Only physical block devices (NVMe, SATA, SAS) are allowed.

## License

MIT
