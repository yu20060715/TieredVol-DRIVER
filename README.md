# TieredVol — Tiered Storage Volume Manager

Linux tiered storage solution. Merge multiple disks into a high-performance striped volume with **custom carve capacity** + dm-linear + LVM striped + real-time RAM Cache tuning.

```
Disk A (NVMe, 2000 MB/s) ──dm-linear──┐
Disk B (SATA, 1000 MB/s) ──dm-linear──┤── LVM VG ── striped LV ── filesystem ── mount
                                       │
                                       ▼
                                  ~3000 MB/s
```

## Core Concept: Carve Custom Capacity

TieredVol's core feature is **carve** — slice a specified amount from each disk and combine them into one striped virtual volume. You choose exactly how much capacity each disk contributes, and the resulting speed is approximately the **sum of all contributing disks' speeds**.

### Example: Two Disks into One Fast Volume

Suppose you have two disks:

| Disk | Model | Capacity | Sequential R/W |
|------|-------|----------|----------------|
| sda | NVMe SSD | 1 TB | 2000 MB/s |
| sdb | SATA SSD | 1 TB | 1000 MB/s |

Carve 1000G from sda and 500G from sdb:

```bash
sudo tiered_setup --create --name fastpool --disks sda:1000,sdb:500 --fs ext4 --mount /mnt/fast
```

Result:

```
┌─────────────────────────────────────────────────────────┐
│  striped volume: 1500 GB                                │
│  Sequential write: ~2820 MB/s (theoretical 3000 × 94%)  │
│  Layout: 1000G @ 2000 MB/s + 500G @ 1000 MB/s          │
└─────────────────────────────────────────────────────────┘
```

### How Striping Works (Implementation)

LVM striped volumes write to all disks **simultaneously** at the block level, not sequentially. Data is split into chunks (stripe units, default 64KB) and distributed across disks in round-robin fashion:

```
Write stream: [chunk0][chunk1][chunk2][chunk3][chunk4][chunk5]...
              ↓       ↓       ↓       ↓       ↓       ↓
sda (fast):  [chunk0]         [chunk2]         [chunk4]...
sdb (slow):          [chunk1]         [chunk3]         [chunk5]...
```

Both disks are active at the same time. The kernel sends I/O requests to all disks in parallel, so total throughput approaches the **sum** of individual disk speeds.

**Performance characteristics:**

| Scenario | Result |
|----------|--------|
| Theoretical speed | sum of all disks = 2000 + 1000 = **3000 MB/s** |
| Actual speed | theoretical × 94% ≈ **2820 MB/s** (LVM overhead + kernel scheduling) |
| Large sequential I/O (≥1GB) | Closest to theoretical |
| Small random I/O | Much lower — striping doesn't help much |
| Single thread vs multi-thread | Multi-thread (fio --numjobs=4) saturates better |

**Why 94% and not 100%?**

- LVM metadata read/write overhead
- Kernel I/O scheduler scheduling delay
- CPU and memory bandwidth limits
- Stripe alignment between disks

**How to get closest to theoretical speed:**

```bash
# Using fio to saturate the striped volume
sudo fio --name=test --filename=/mnt/fast/test --rw=write --bs=1M --size=2G --numjobs=4 --iodepth=32 --direct=1
```

### Example: Three-Disk Combo

```bash
# Carve 500G from each of 3 disks → 1.5T volume
sudo tiered_setup --create --name tripool --disks nvme0n1:500,sdb:500,sdc:500 --fs ext4 --mount /mnt/tri

# Result: ~4500 MB/s (theoretical 5000 × 90%)
# nvme0n1: 2000 MB/s + sdb: 1500 MB/s + sdc: 1500 MB/s
```

### When No Carve Size Is Specified

If you omit the capacity, it defaults to the full disk (minus 1GB reserved):

```bash
# sda takes full 1000G, sdb takes full 500G
sudo tiered_setup --create --name pool --disks sda,sdb --fs ext4 --mount /mnt/pool
```

## Features

### Disk Detection
Automatically scans all system disks, displays model, interface (SATA/NVMe/USB), and capacity. System disk is auto-tagged `[ROOT]` and locked to prevent accidental selection. Mounted disks are tagged `[MOUNTED]` and cannot be modified.

### Auto Benchmark
Starts a parallel benchmark in the background on launch, simultaneously testing sequential read/write speeds of all disks. Non-blocking — you can browse other screens while testing. Shows `TESTING...` status during the test, updates speeds when complete. Press `B` to re-benchmark at any time.

### Create Volume
Interactive 3-phase wizard for creating striped LVM volumes:

1. **Select Disks** — Pick disks to combine (minimum 2). Use `↑↓` to move cursor, `Space` to toggle
2. **Set Carve Sizes** — Choose how many GB to carve from each disk (`← →` to adjust, 50GB steps)
3. **Configure** — Enter volume name, mount point, filesystem (ext4/xfs/btrfs)

Auto-executes: dm-linear carve → pvcreate → vgcreate → lvcreate → mkfs → mount. Any failure triggers automatic rollback, cleaning up all residues.

### Volume Management
- **View Status** — Shows volume name, size, mount point, usage, per-disk read/write speeds
- **Destroy Volume** — One-click teardown of striped LV → VG → PV → dm-linear devices, with confirmation

### RAM Cache Tuning
Adjust Linux kernel's `vm.dirty_ratio` to borrow system RAM as write cache. 128MB step increments, Apply to use / Reset to restore originals. Auto-restores on exit, no system impact.

Use case: Multiple HDDs in a striped volume — borrow RAM as write buffer to accelerate small file writes. TUI lets you adjust borrow amount, preview new dirty_ratio, and apply/reset in real time.

### Security
- **Name validation** — Whitelist `[a-zA-Z0-9._-]`, blocks semicolons, pipes, $, backticks
- **Filesystem validation** — Only ext4, ext3, xfs, btrfs, none allowed
- **Mount point validation** — Must be absolute path, rejects `..` path traversal
- **LVM command safety** — Uses `fork+execvp` instead of `system()`, preventing shell injection
- **Temp file safety** — `fchmod` sets permissions immediately, eliminating TOCTOU race windows

### Error Rollback
Any step failure during volume creation (dmsetup / pvcreate / vgcreate / lvcreate / mkfs / mount) automatically cleans up all created devices and LVM config. Benchmark interruption auto-kills all child processes.

## Requirements

- Linux (tested on Ubuntu 24.04, kernel 6.14)
- `lvm2` `dmsetup` `libncurses-dev` `gcc` `make`
- Root privileges (sudo)

### Install Dependencies

```bash
# Debian / Ubuntu
sudo apt install lvm2 libncurses-dev gcc make

# Fedora / RHEL
sudo dnf install lvm2 ncurses-devel gcc make

# Arch
sudo pacman -S lvm2 ncurses gcc make
```

## Quick Start

```bash
git clone https://github.com/yu20060715/TieredVol.git
cd TieredVol
make
sudo ./tiered_ui
```

### Install System-Wide

```bash
sudo make install
sudo tiered_ui
```

## Build Commands

```bash
make              # Build tiered_setup + tiered_ui
make test         # Build and run all tests (53 test cases)
make clean        # Remove all build artifacts
sudo make install # Install to /usr/local/bin/
sudo make uninstall
```

## CLI Usage

```bash
# List all disks
sudo tiered_setup --list

# Benchmark (3 disks)
sudo tiered_setup --bench --disks sdb,sdc,nvme0n1

# Create striped volume (carve 300G from sdb, 200G from sdc)
sudo tiered_setup --create --name fastpool --disks sdb:300,sdc:200 --fs ext4 --mount /mnt/fast

# Create striped volume (full sda + 500G from sdb)
sudo tiered_setup --create --name pool --disks sda,sdb:500 --fs xfs --mnt /mnt/pool

# View status
sudo tiered_setup --status

# Destroy volume
sudo tiered_setup --destroy --name fastpool
```

### Carve Syntax

The `--disks` parameter supports `diskname:sizeG` format:

| Example | Description |
|---------|-------------|
| `sda:1000,sdb:500` | Carve 1000GB from sda, 500GB from sdb |
| `sda,sdb` | Take full disk from both (minus 1GB each) |
| `nvme0n1:2000,sda:1000,sdb:1000` | Three disks: 2T + 1T + 1T = 4T volume |
| `sda:500,sdb:500,sdc:500` | 500G each from 3 disks → 1.5T volume |

## TUI Interface

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

### Keyboard Shortcuts

| Screen | Key | Action |
|--------|-----|--------|
| Main Menu | ↑↓ Enter Q/ESC | Select / Confirm / Exit |
| Disk List | B | Re-benchmark |
| Disk List | Q/ESC | Back |
| Benchmark | Q/ESC | Back (benchmark continues in background) |
| Create Phase 1 | Space Enter | Toggle disk / Next |
| Create Phase 2 | ←→ ↑↓ | Adjust carve size / Select disk |
| RAM Cache | ←→ ↑↓ Enter | Adjust / Select Apply/Reset |
| Destroy | Y | Confirm destruction |

## Project Structure

```
TieredVol/
├── README.md                   # This file (English)
├── docs/
│   ├── README_CN.md            # Documentation (Chinese)
│   ├── USAGE.md                # Detailed usage guide
│   └── PLAN.md                 # Improvement roadmap
├── Makefile                    # Build system
├── .gitignore
├── src/
│   ├── tiered_setup.c          # CLI backend (1326 lines)
│   ├── tiered_ui.c             # ncurses TUI frontend (1379 lines)
│   ├── tiered_common.h         # Shared validation (name/FS/mount whitelist)
│   ├── tiered_ui_helpers.h     # TUI helpers (ui_disk_t, parse functions)
│   └── version.h               # Version constant
└── tests/
    ├── test_common.c           # Validation tests (31 cases)
    └── test_tui.c              # TUI parsing tests (22 cases)
```

### Code Architecture

| Module | Responsibility |
|--------|---------------|
| `tiered_setup.c` | CLI core: disk discovery, parallel benchmark, dm-linear/LVM create/destroy, rollback, config persistence |
| `tiered_ui.c` | TUI frontend: 7 screens, 3-phase create wizard, background benchmarking, RAM cache tuning, terminal defense |
| `tiered_common.h` | Input validation: `tiered_is_valid_name()`, `tiered_is_valid_fs()`, `tiered_is_valid_mount()` |
| `tiered_ui_helpers.h` | `ui_disk_t` struct, `parse_bench_output()`, `bench_disk_done()` |

## Notes

- **System disk cannot be used** — dm-linear returns EBUSY on mounted root partition
- Selected disks will be **completely wiped**
- Requires root privileges for all operations
- RAM Cache settings auto-restore on exit
- Carve size cannot exceed disk capacity (e.g., 1TB disk max carve = 999GB)

## License

MIT
