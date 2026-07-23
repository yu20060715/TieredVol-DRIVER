# TieredVol-DRIVER

A kernel-level Device Mapper target for weighted striping across heterogeneous storage devices.

TieredVol-DRIVER replaces the userspace io_uring I/O path with a kernel dm-target module (`tieredvol.ko`), achieving near-zero overhead. Applications interact with tiered storage through standard `write()`/`read()` syscalls on `/dev/mapper/<name>`.

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

### Key Results (sustained 5 GB, fio + io_uring + iodepth=64, userspace v1)

| Config | Write | vs LVM | Read | HW Sum Efficiency |
|--------|-------|--------|------|-------------------|
| 2-disk [2,1] | 1063 MB/s | 1.56× | 1101 MB/s | 76.3% |
| 3-disk [2,1,1] | 1168 MB/s | 2.01× | 1253 MB/s | 62.7% |

Hardware: i5-4570, NVMe CT1000P3PSSD8 (~967 MB/s), SATA CT500MX500SSD1 (~427 MB/s), SATA WDC WDS250G2B0A (~432 MB/s). Hardware sum: 2-disk=1394 MB/s, 3-disk=1864 MB/s. Weights: [2,1] / [2,1,1].

Target with kernel module: η > 90% (overhead < 10%).

### What Is Intentionally Excluded

- Filesystem implementation
- Data redundancy (no mirror, no parity)
- Crash consistency / journaling
- Metadata recovery
- Dynamic online rebalancing

---

## Quick Start

```bash
git clone https://github.com/yu20060715/TieredVol-DRIVER.git
cd TieredVol-DRIVER

# Build userspace tools
make

# Build kernel module
make module
sudo make module_install
sudo depmod -a

# Create a weighted volume
sudo ./tiered_setup --create --name fastpool --disks nvme0n1,sdb,sdc --scheduler

# Benchmark
sudo ./tiered_io --path /dev/mapper/fastpool --bench-all

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
sudo tiered_setup --create --name fastpool --disks nvme0n1,sdb --scheduler

# Benchmark the volume
sudo tiered_io --path /dev/mapper/fastpool --bench --size 5GB
sudo tiered_io --path /dev/mapper/fastpool --bench-all

# Show volume metadata
sudo tiered_io --name fastpool --info

# Remove volume
sudo tiered_setup --remove --name fastpool
```

### LVM Striping (Legacy)

```bash
# Create LVM striped volume
sudo tiered_setup --create --name pool --disks sdb:300,sdc:200 --fs ext4 --mount /mnt/pool

# Benchmark
sudo tiered_io --path /dev/mapper/tv_vg_pool-tv_lv_pool --bench --size 5GB

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
make                    # Build tiered_setup + tiered_io
make test               # Unit tests (81 assertions, 4 suites, no sudo)
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
├── MIGRATION.md                  # Migration plan from userspace to kernel
├── Makefile
├── driver/                       # Kernel dm-target module
│   ├── Kbuild                    # Kernel build configuration
│   ├── tieredvol.h               # Shared kernel types
│   ├── tieredvol_core.c          # dm_target ops, bio submission, completion
│   ├── tieredvol_map.c           # Logical → Physical offset mapping
│   └── tieredvol_meta.c          # Metadata loading from config file
├── src/
│   ├── main.c                    # CLI entry point (tiered_setup)
│   ├── tiered_common.h           # Input validation (name, fs, mount)
│   ├── tiered_types.h            # Shared type definitions
│   ├── version.h                 # Version string
│   ├── tiered_mapper.c           # Offset mapping (userspace)
│   ├── tiered_partition.c        # Weight + segment calculation
│   ├── tiered_metadata.c         # Metadata save/load (INI format)
│   ├── tiered_benchmark.c        # Initialization benchmark
│   ├── tiered_io.c               # I/O benchmark tool (pwrite/pread)
│   ├── warmup.c / warmup.h       # SLC cache warm-up
│   ├── exec_helper.c / .h        # External command execution
│   ├── setup_discover.c / .h     # Disk discovery (lsblk, sysfs)
│   ├── setup_bench.c / .h        # Setup benchmark logic
│   ├── cmd_create.c / .h         # Volume creation (kernel dm + LVM)
│   └── cmd_remove.c / .h         # Volume removal
├── tests/
│   ├── test_common.c             # Input validation tests
│   ├── test_mapper.c             # Mapping tests
│   ├── test_partition.c          # Weight/segment tests
│   └── test_metadata.c           # Metadata round-trip tests
├── benchmarks/                   # Raw benchmark data and summary
├── docs/
│   ├── USAGE.md                  # Detailed usage guide
│   ├── PARTITION_SPLITTING.md    # Weighted striping algorithm
│   ├── WEIGHTED_IO_SCHEDULER.md  # I/O dispatch implementation
│   └── BENCHMARK-RESULTS.md      # Benchmark results on B85 platform
└── scripts/
    ├── install_deps.sh           # Dependency installer
    ├── test_scheduler.sh         # End-to-end test
    ├── tieredvol-restore.sh      # Boot-time volume restore
    └── tieredvol-restore.service # Systemd service
```

### Code Architecture

| Module | Responsibility |
|--------|---------------|
| **Kernel module** | |
| `driver/tieredvol_core.c` | dm_target ctr/dtr/map, bio splitting, weighted dispatch |
| `driver/tieredvol_map.c` | Logical → Physical offset mapping (kernel) |
| `driver/tieredvol_meta.c` | Metadata loading from config file (kernel) |
| **Userspace tools** | |
| `main.c` | CLI entry point: argument dispatch, dependency checks |
| `cmd_create.c` | Volume creation: kernel dm target + legacy LVM |
| `cmd_remove.c` | Volume removal: dmsetup remove + legacy teardown |
| `tiered_io.c` | I/O benchmark: pwrite/pread on block devices |
| `tiered_mapper.c` | Logical ↔ Physical offset mapping (userspace) |
| `tiered_partition.c` | Weight calculation, capacity segmentation |
| `tiered_metadata.c` | Metadata save/load (INI format) |
| `tiered_benchmark.c` | Initialization benchmark |
| `warmup.c` | SLC cache warm-up |
| `exec_helper.c` | External command execution for dmsetup/lvm |
| `setup_discover.c` | Disk discovery: list, filter, detect partitions |
| `setup_bench.c` | Setup benchmark: parallel speed testing |

---

## Kernel Module

The `tieredvol` dm target processes bios in-kernel:

1. **bio arrives** at the dm target (from VFS `write()`/`read()`)
2. **Map**: `tv_map_logical()` translates logical byte offset → (disk, physical_offset, remaining)
3. **Split**: If bio crosses a stripe boundary, `bio_split()` fragments it
4. **Redirect**: `bio_set_dev()` + sector update → submit to underlying device

Key constants:
- `TV_CHUNK_SIZE` = 1 MB (weight unit)
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
```

---

## Limitations

- **Static weights only** — Weights are computed at initialization and fixed.
- **No fault tolerance** — If any disk fails, the entire stripe set is lost.
- **No POSIX write() interception** — Applications use standard `write()`/`read()` on the dm target device.
- **No crash consistency** — No journaling or metadata recovery.
- **System disk cannot be used** — dm returns EBUSY on mounted root partition.
- **Module instability risk** — A kernel module bug can oops the system.

## License

MIT
