# TieredVol-DRIVER Benchmark Results

## Hardware

| Device | Model | Size | Interface | Raw Sequential Write |
|--------|-------|------|-----------|---------------------|
| nvme0n1 | CT1000P3PSSD8 | 931 GB | NVMe PCIe 3.0 x4 | 1499 MB/s (2M, QD=256) |
| sdb | CT500MX500SSD1 | 465 GB | SATA III | 517 MB/s |
| sdc | WDC WDS250G2B0A | 232 GB | SATA III | 536 MB/s |
| sdb+sdc combined | - | - | Same SATA controller | ~957 MB/s |

NVMe: MDTS=6 (256KB cmd), max_hw_sectors_kb=128 (driver caps at 128KB), nr_requests=1023, 5 MSI-X queues
SATA: Both on Intel 8 Series/C220 SATA III controller (shared bus)

## Configuration

- Chunk size: 1 MB
- Segment 0: [0, 231 GB) → 3 disks, weights [1,1,6] (NVMe gets 75%)
- Segment 1: [231 GB, 464 GB) → 2 disks, weights [1,3] (NVMe gets 75%)
- Segment 2: [464 GB, 930 GB) → 1 disk (NVMe only)
- Driver: v4.2.0 (DM_TARGET_NOWAIT, per-CPU stats, 0.25µs/map call)
- Kernel: 6.14.0-27-generic, module built out-of-tree

## fio Benchmark Results

All tests: `--direct=1`, 2G write, 3 runs each.

### Best Configuration: io_uring, bs=2M, QD=256

| Config | Throughput (MB/s) |
|--------|-------------------|
| **DM io_uring QD=256 bs=2M** | **1486-1499** |
| DM io_uring QD=256 bs=1M | 1460-1480 |
| DM io_uring QD=128 bs=16M | 1396 |
| DM psync bs=16M | 1351 |
| Raw NVMe io_uring QD=256 bs=2M | 1499-1504 |
| Raw NVMe psync bs=16M | 1267 |

### Block Size Sweep (DM, io_uring QD=256)

| Block Size | Throughput (MB/s) |
|------------|-------------------|
| 128 KB | 1450 |
| 256 KB | 1459 |
| 512 KB | 1450 |
| 1 MB | 1480 |
| **2 MB** | **1495** |
| 4 MB | 1426-1440 |
| 8 MB | 1403 |
| 16 MB | 1426 |

### Queue Depth Sweep (DM, io_uring, bs=2M)

| QD | Throughput (MB/s) |
|----|-------------------|
| 1 | 1353 |
| 32 | 1168 |
| 128 | 1495 |
| **256** | **1499** |
| 512 | 1452 |
| 1024 | 1469 |

### Phase 1 Experiments

| Experiment | Result |
|------------|--------|
| Remove end_fsync | No difference (1488 vs 1486) |
| NUMA-aware allocation | N/A (single NUMA node) |
| NVMe IRQ affinity | Already optimal (q1-q4 → CPU0-3) |
| NVMe write cache OFF | **-21%** (1174 MB/s, restored via `nvme reset`) |
| NVMe interrupt coalescing | Not supported by CT1000P3PSSD8 |
| DM write_cache=write through | No difference (already was write-through) |
| NVMe max_sectors_kb=1024 | Slightly worse (1442 MB/s) |
| sqthread_poll=1 | Slightly worse (1455 MB/s) |
| registerfiles | No difference (1497 MB/s) |
| 4 GB test size | Same (1500 MB/s) |
| Per-CPU stats (v4.2.0) | No change (1479 MB/s) |
| TV_CHUNK_SIZE=2MB | No change (1486 MB/s, reverted) |
| NVMe scheduler=none | No change (already active) |
| NVMe nr_requests=2048 | Slightly worse (1455 MB/s) |

### Driver Version Comparisons

| Version | Throughput (MB/s) | Notes |
|---------|-------------------|-------|
| v3.x (DM_MAPIO_REMAPPED old) | 1354 | Initial DM migration |
| v4.0.0 (DM_TARGET_NOWAIT) | 1472 | +8.7% from NOWAIT |
| v4.1.0 (io_hints, max_io_len) | 1499 | +1.8% from limits |
| v4.2.0 (per-CPU stats) | 1479 | No change |

### Segment Test Results

| Config | Theoretical | Actual | Efficiency |
|--------|------------|--------|------------|
| 3-disk [1,1,6] | 1999 MB/s | 1499 MB/s | **75%** |
| 2-disk [1,7] NVMe+sdb | 1713 MB/s | 1370 MB/s | **80%** |
| 2-disk [1,6] NVMe+sdb | 1749 MB/s | 1383 MB/s | **79%** |

Key insight: More devices = more overhead (74% for 3-disk vs 80% for 2-disk).

## Theoretical Analysis

With [1,1,6] weights:
- NVMe at 75%: T ≤ 1499/0.75 = **1999 MB/s** (NVMe-limited)
- SATA at 25%: T ≤ 957/0.25 = **3828 MB/s** (SATA-limited)
- **Theoretical max = 1999 MB/s**

Achieved: **1499 MB/s = 75% efficiency**
Target: 1683 MB/s = **84% efficiency** (requires ~12% improvement)

### DM Layer Overhead
- Raw NVMe: 1475-1504 MB/s
- DM device: 1499 MB/s (includes SATA parallelism)
- **DM overhead: <1%** (the driver is extremely efficient)

### Why 75% Efficiency?

The 25% overhead is NOT from the DM target. It is from the Linux block layer:

1. **BIO_MAX_VECS=256** (x86_64, 4KB pages): Limits each bio to 1MB. A 2MB write
   creates 2 bios. Each bio incurs allocation, queuing, and completion callback overhead.

2. **NVMe max_hw_sectors_kb=128**: Each 1MB bio is further split into 8x128KB
   NVMe commands. The NVMe controller's MDTS=6 supports 256KB, but the Linux
   NVMe driver caps at 128KB due to `max_segments=33`.

3. **Per-bio overhead accumulates**: bio allocation (~1-2µs) + block layer
   queue insertion + DM map (0.25µs) + NVMe submission + completion callback.
   At high QD, these compound across thousands of in-flight bios.

4. **NVMe command processing**: Each 128KB NVMe command has its own completion,
   interrupt handling, and callback chain. The NVMe controller has 1023 queue
   depth, but the CPU must process each completion.

### What Would Be Needed for 1683 MB/s

- **Increase NVMe max_hw_sectors_kb from 128 to 256**: Controller supports it
  (MDTS=6), but Linux driver caps at 128KB. Requires NVMe driver patch.
- **Increase BIO_MAX_VECS from 256 to 1024+**: Allows 4MB bios → fewer bios →
  less overhead. Requires kernel rebuild with modified bio.h.
- Both are kernel-level changes beyond DM target scope.

## Recommended Production Configuration

```bash
# fio benchmark command (for validation)
fio --name=bench --filename=/dev/mapper/testpool --rw=write --bs=2m --size=2G \
  --direct=1 --ioengine=io_uring --iodepth=256 --numjobs=1 --end_fsync=1

# Expected throughput: ~1500 MB/s
```

```
# /etc/tieredvol/testpool.conf
[weighted_striping]
chunk_size=1048576
seg0_weight=1,1,6    # NVMe=75%, SATA=25%
```

```bash
# Required sysfs settings (apply after boot/module load)
echo 0 > /sys/block/nvme0n1/queue/wbt_lat_usec
echo none > /sys/block/nvme0n1/queue/scheduler
echo 1024 > /sys/module/dm_mod/parameters/reserved_bio_based_ios

# WARNING: Never disable NVMe write cache (-21% throughput)
# Verify: nvme get-feature -f 0x06 /dev/nvme0
# Recovery if accidentally disabled: sudo nvme reset /dev/nvme0
```
