# TieredVol Scheduler

An experimental user-space weighted striping scheduler for heterogeneous storage devices.

TieredVol Scheduler evaluates whether weighted striping — assigning I/O chunk sizes proportional to each disk's sequential bandwidth — can improve aggregate throughput when storage devices with different speeds (e.g., NVMe + SATA SSD) are combined.

**This is not a filesystem, RAID implementation, Linux block driver, or Device Mapper target.** It does not intercept standard POSIX `write()` calls. Applications interact with the scheduler through a dedicated user-space API (`tv_write()` / `tv_read()`), which performs weighted striping directly on raw storage devices.

```
Application
      │
      ▼
  tv_write() / tv_read()    ← Application must use this API
      │
      ▼
  TieredVol Scheduler       ← weighted chunk分配 + offset mapping
      │
      ▼
  io_uring                  ← async I/O dispatch
      │
      ▼
  Raw Device (/dev/mapper/*)
```

### What This Prototype Validates

- Weighted chunk scheduling (proportional to disk speed)
- Static weight generation via benchmark at initialization
- Segment-based mapping for unequal disk capacities
- Logical ↔ Physical offset mapping
- Parallel asynchronous dispatch using io_uring

### What Is Intentionally Excluded

- Filesystem implementation
- Kernel block driver / Device Mapper target
- Data redundancy (no mirror, no parity)
- Crash consistency / journaling
- Metadata recovery
- Dynamic online rebalancing
- POSIX `write()` interception

The current implementation assumes static striping weights generated during initialization. Changing weights invalidates all logical-to-physical mappings unless data migration is performed.

> Under large sequential workloads with sufficient queue depth, aggregate throughput **may approach** the sum of individual sequential bandwidths. Actual results depend on CPU, PCIe bandwidth, filesystem overhead, and I/O pattern.

---

## Weighted Striping vs Fixed-Size Striping

Traditional RAID0 and LVM striping use a fixed stripe size for all disks. When storage devices exhibit significantly different sequential bandwidth, fixed striping underutilizes faster devices because all disks must finish their chunk before the next stripe begins.

```
Fixed striping (LVM):          Weighted striping (TieredVol):

NVMe  3100 MB/s → 1 chunk     NVMe  3100 MB/s → 7 chunks = 448KB
SATA  1700 MB/s → 1 chunk     SATA  1700 MB/s → 4 chunks = 256KB
SATA   800 MB/s → 1 chunk     SATA   800 MB/s → 2 chunks = 128KB
SATA   450 MB/s → 1 chunk     SATA   450 MB/s → 1 chunk  =  64KB

NVMe idle waiting for SATA     All disks finish at approximately
→ throughput ≈ slowest disk    the same time → higher aggregate
```

**Weight is generated at initialization** via a benchmark that measures sequential write speed with SLC cache warm-up (2GB pre-write). This ensures weights reflect sustained disk speed, not peak SLC cache speed.

```bash
# Create a weighted striping session
sudo tiered_setup --create --name fastpool \
    --disks nvme0n1:1000,sda:500,sdb:500 \
    --scheduler

# Compare peak vs sustained speed
sudo tiered_io --name fastpool --bench --size 128MB           # Peak (SLC cache)
sudo tiered_io --name fastpool --bench --size 128MB --warmup  # Sustained
```

For implementation details, see:
- [PARTITION_SPLITTING.md](docs/PARTITION_SPLITTING.md) — Weight calculation, capacity segmentation, offset mapping
- [WEIGHTED_IO_SCHEDULER.md](docs/WEIGHTED_IO_SCHEDULER.md) — Three-layer architecture, io_uring dispatch, stripe buffer
- [AGENTS.md](AGENTS.md) — Full implementation guide with code for every module

---

## Legacy: dm-linear + LVM Striping Tool

The project also includes a TUI management tool for dm-linear carving + LVM striping (Phase 0/1). This tool predates the weighted striping scheduler and provides a user-friendly way to combine disks using Linux's native dm-linear and LVM.

```
Disk A (NVMe, 2000 MB/s) ──dm-linear──┐
Disk B (SATA, 1000 MB/s) ──dm-linear──┤── LVM VG ── striped LV ── filesystem ── mount
                                       │
                                       ▼
                                  ~2820 MB/s (same-speed disks)
```

> **Note:** LVM striping uses the same stripe size for all disks. When combining disks of different speeds, throughput is limited by the slowest disk. This is why the weighted striping scheduler was developed.

### Features

- **Carve Custom Capacity** — Slice a specified amount from each disk via dm-linear
- **ncurses TUI** — Interactive 3-phase create wizard, disk list, benchmark, RAM cache tuning
- **Input Validation** — Name/FS/mount whitelists, double-confirmation safety prompts
- **Error Rollback** — Any step failure automatically cleans up all created devices
- **Auto Benchmark** — Background parallel speed testing of all disks
- **RAM Cache Tuning** — Adjust `vm.dirty_ratio` to borrow RAM as write cache

### Quick Start (LVM Striping)

```bash
git clone https://github.com/yu20060715/TieredVol.git
cd TieredVol
make
sudo ./tiered_ui
```

### CLI Usage (LVM Striping)

```bash
# List all disks
sudo tiered_setup --list

# Benchmark 3 disks
sudo tiered_setup --bench --disks sdb,sdc,nvme0n1

# Create striped volume (carve 300G from sdb, 200G from sdc)
sudo tiered_setup --create --name fastpool --disks sdb:300,sdc:200 --fs ext4 --mount /mnt/fast

# Create volume (full sda + 500G from sdb)
sudo tiered_setup --create --name pool --disks sda,sdb:500 --fs xfs --mount /mnt/pool

# View status
sudo tiered_setup --status

# Destroy volume
sudo tiered_setup --destroy --name fastpool
```

### Carve Syntax

| Example | Description |
|---------|-------------|
| `sda:1000,sdb:500` | Carve 1000GB from sda, 500GB from sdb |
| `sda,sdb` | Take full disk from both (minus 1GB each) |
| `nvme0n1:2000,sda:1000,sdb:1000` | Three disks: 2T + 1T + 1T |

---

## Requirements

- Linux (kernel 5.1+ for io_uring support)
- `lvm2` `dmsetup` `libncurses-dev` `gcc` `make`
- `liburing-dev` (for Weighted Striping Scheduler)
- Root privileges (sudo)

### Install Dependencies

```bash
# Debian / Ubuntu
sudo apt install lvm2 libncurses-dev gcc make liburing-dev

# Fedora / RHEL
sudo dnf install lvm2 ncurses-devel gcc make liburing-devel

# Arch
sudo pacman -S lvm2 ncurses gcc make liburing
```

## Build

```bash
make              # Build tiered_setup + tiered_ui + tiered_io
make test         # Run all tests (56 test cases)
make clean        # Remove all build artifacts
sudo make install # Install to /usr/local/bin/
```

### Enable Auto-Restore on Boot (Optional)

```bash
sudo systemctl daemon-reload
sudo systemctl enable tieredvol-restore
```

---

## TUI Interface

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

| Screen | Key | Action |
|--------|-----|--------|
| Main Menu | ↑↓ Enter Q/ESC | Select / Confirm / Exit |
| Disk List | B | Re-benchmark |
| Create Phase 1 | Space Enter | Toggle disk / Next |
| Create Phase 2 | ←→ ↑↓ | Adjust carve size |
| RAM Cache | ←→ ↑↓ Enter | Adjust / Apply / Reset |
| Destroy | Y | Confirm destruction |

---

## Project Structure

```
TieredVol/
├── README.md
├── README_CN.md
├── AGENTS.md                   # Implementation guide
├── LICENSE                     # MIT License
├── docs/
│   ├── USAGE.md                # Detailed usage guide
│   ├── PLAN.md                 # Improvement roadmap
│   ├── PARTITION_SPLITTING.md  # Weighted striping algorithm
│   └── WEIGHTED_IO_SCHEDULER.md # I/O dispatch implementation
├── scripts/
│   ├── install_deps.sh         # One-click dependency installer
│   ├── test_scheduler.sh       # End-to-end scheduler test
│   ├── tieredvol-restore.sh    # Boot-time volume restore
│   └── tieredvol-restore.service
├── Makefile
├── src/
│   ├── tiered_setup.c          # CLI backend
│   ├── tiered_ui.c             # ncurses TUI frontend
│   ├── tiered_common.h         # Shared validation
│   ├── tiered_ui_helpers.h     # TUI helpers
│   ├── version.h
│   ├── tiered_sched.h          # Scheduler structs + API
│   ├── tiered_sched.c          # Scheduler core
│   ├── tiered_mapper.c         # Offset mapping
│   ├── tiered_io_uring.c       # io_uring wrapper
│   ├── tiered_benchmark.c      # Initialization benchmark
│   ├── tiered_partition.c      # Weight + segment calculation
│   ├── tiered_metadata.c       # Metadata save/load
│   └── tiered_io.c             # CLI I/O tool (read/write/bench)
└── tests/
    ├── test_common.c
    └── test_tui.c
```

### Code Architecture

| Module | Responsibility |
|--------|---------------|
| `tiered_setup.c` | CLI core: disk discovery, benchmark, dm-linear/LVM/scheduler create, rollback |
| `tiered_ui.c` | TUI frontend: 7 screens, create wizard, benchmarking, RAM cache tuning |
| `tiered_sched.c` | Scheduler core: init, write (buffer + flush), read (mapping + io_uring), destroy |
| `tiered_mapper.c` | Logical ↔ Physical offset mapping (prefix sum + linear scan) |
| `tiered_io_uring.c` | io_uring wrapper (SQE/CQE, submit, wait) |
| `tiered_benchmark.c` | Initialization benchmark (O_DIRECT, 3 runs average) — **not a storage benchmark** |
| `tiered_partition.c` | Weight calculation, capacity segmentation, segment building |
| `tiered_metadata.c` | Metadata save/load (static weights only) |

---

## Limitations

- **I/O path integration** — `tv_write()` / `tv_read()` are implemented and called by `tiered_io` CLI tool. End-to-end verified via `tiered_io --bench`, `tiered_io --write`, and `tiered_io --read`. FUSE/libtiered integration not yet available.
- **Static weights only** — Weights are computed at initialization and fixed. Changing weights invalidates all mappings.
- **No fault tolerance** — If any disk fails, the entire stripe set is lost. No degraded mode, no rebuild, no mirror/parity.
- **No POSIX write() interception** — Applications must use `tv_write()` / `tv_read()`. Standard `write()` goes to the filesystem, not the scheduler.
- **No partial stripe tracking** — Close/fsync behavior for partial stripes is not fully implemented.
- **Benchmark is for initialization only** — The built-in benchmark measures initial sequential write speed with SLC cache warm-up (2GB pre-write). It is not a comprehensive storage benchmark (no latency, no queue depth sweep).
- **Not persistent across reboot** — dm-linear targets and LVM volumes require the systemd service for auto-restore.
- **System disk cannot be used** — dm-linear returns EBUSY on mounted root partition.

## License

MIT
