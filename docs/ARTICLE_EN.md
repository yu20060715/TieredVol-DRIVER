# TieredVol: A Userspace Weighted Striping Scheduler for Heterogeneous Storage

## The Problem

When combining NVMe SSDs (~3000 MB/s) with SATA SSDs (~500 MB/s), traditional RAID0 and LVM striping assign equal chunk sizes to every disk. Fast disks finish their I/O and wait idly for the slowest disk, capping aggregate throughput near the slowest member. Bandwidth does not sum.

## The Solution: Weighted Striping

The idea is straightforward: **assign more data to faster disks and less to slower ones, so all disks finish at roughly the same time.**

TieredVol implements a weighted striping scheduler entirely in userspace, dispatching I/O asynchronously through io_uring. On each stripe flush, every disk receives `weight[i] × TV_CHUNK_SIZE` bytes of data, where weights are derived from an initialization benchmark:

```
weight[i] = round(speed[i] / slowest_speed)
```

Example: NVMe 1000 MB/s + SATA 500 MB/s → weight [2:1], stripe_size = 3 MB.

## Three-Layer Architecture

```
Layer 1: Offset Map (pure math)
  Input: logical offset → Output: (disk, physical offset, length)
  No I/O touching

Layer 2: Stripe Buffer (data staging)
  Accumulates user writes → flushes at stripe_size
  Pool size = TV_BUF_COUNT (64 stripe buffers)

Layer 3: Dispatcher (actual I/O)
  Splits buffer by weight table → builds io_uring SQEs
  submit + wait CQE
```

Metadata persists as a plain-text configuration file at `/etc/tieredvol/<name>.scheduler`, containing disk names, weights, and segment definitions.

## Benchmark Results

### Hardware

B85 test bench, Linux kernel 6.x, three disks:

| Disk | Type | Sustained Write |
|------|------|----------------|
| nvme0n1 | NVMe (M.2 PCIe) | ~1100 MB/s |
| sdb | SATA SSD | ~450 MB/s |
| sdc | SATA SSD | ~450 MB/s |

Configuration: `TV_CHUNK_SIZE=1 MB`, `TV_BUF_COUNT=64`.

### 2-Disk (nvme0n1 + sdb)

Weights [2:1], stripe = 3072 KB.

| Size | Scheduler (MB/s) | LVM ext4 (MB/s) | Ratio |
|------|-----------------|----------------|-------|
| 512 MB | 1472 (93.6% of theoretical) | 640 | **2.3x** |
| 5 GB | 1125 | — | — |
| 10 GB | 1122 | — | — |

### 3-Disk (nvme0n1 + sdb + sdc)

Weights [2:1:1], stripe = 4096 KB.

| Size | Scheduler (MB/s) |
|------|-----------------|
| 512 MB | 1792 (97.9% of theoretical) |

### Key Findings

1. **Weighted striping effectively aggregates bandwidth**: 2-disk achieved 1472 MB/s (93.6%), 3-disk achieved 1792 MB/s (97.9%)
2. **LVM fixed striping capped at 640 MB/s** — the bottleneck is the kernel dm-linear layer, not ext4 or the I/O submission method
3. **1 MB chunks outperform 256 KB by +35%** due to lower per-stripe overhead
4. **Throughput drop at larger sizes is caused by NVMe SLC cache exhaustion**, not an architectural limitation

### Known Limitations

- Static weights — cannot be changed after initialization without full data migration
- No fault tolerance — any disk failure destroys the entire stripe set
- Applications must use `tv_write()`/`tv_read()` directly — no POSIX intercept
- No FUSE integration
- dm-linear + O_DIRECT + io_uring has a CQE loss bug (handled via timeout + drain)
- Partial stripes only write to the first weight's disk (correct but unbalanced)

## Code Architecture

```
src/
├── tiered_setup.c    CLI backend (create, destroy, disk scan)
├── tiered_sched.h    Struct definitions + API declarations
├── tiered_sched.c    Scheduler core (write/read/flush/seek/destroy)
├── tiered_mapper.c   Logical ↔ Physical offset mapping
├── tiered_io_uring.c io_uring wrapper
├── tiered_benchmark.c  Initialization benchmark
├── tiered_partition.c Weight + segment computation
├── tiered_metadata.c  Metadata save/load
├── tiered_io.c       CLI I/O tool (read/write/bench)
├── tiered_common.h   Shared validation functions
└── version.h         Version 1.4.0
```

## Conclusion

TieredVol demonstrates that userspace weighted striping can effectively aggregate bandwidth across heterogeneous disks. A 3-disk combination reached 1792 MB/s (97.9% of theoretical maximum), and a 2-disk configuration outperformed LVM striping by 2.3x. The implementation is pure userspace with io_uring — no kernel module modifications required — making it a viable prototype for exploring storage system design.

Phase 3 (dynamic tiering) and Phase 4 (FUSE/POSIX integration) are left for future work.

Source: https://github.com/yu20060715/TieredVol
License: MIT
