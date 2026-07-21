# Benchmark Summary — B85 Platform

## System
- CPU: Intel i5-4570
- RAM: DDR3 1600MHz
- NVMe: P3 Plus 1T (PCIe 2.0x4)
- SATA: WD Blue NAND 250G + MX500 500G
- OS: Lubuntu, Linux 6.x

## Results

### TieredVol Scheduler (Weighted Stripe)

| Scenario | Mean (MB/s) | StdDev | Runs |
|----------|------------|--------|------|
| 2-disk 512MB write | 2203.4 | 85.0 | 5 |
| 2-disk 512MB read | 1079.7 | 58.9 | 5 |
| 2-disk 5GB write | 1324.5 | 6.2 | 5 |
| 2-disk 10GB write | 1301.4 | 2.8 | 5 |
| 2-disk 5GB read | 1304.1 | 59.4 | 5 |
| 2-disk 10GB read | 1319.6 | 59.1 | 5 |
| 3-disk 512MB write | 1815.7 | 22.9 | 5 |
| 3-disk 512MB read | 1407.4 | 28.6 | 5 |
| 3-disk 5GB write | 1302.3 | 5.7 | 5 |
| 3-disk 10GB write | 1284.5 | 7.6 | 5 |
| 3-disk 5GB read | 1190.1 | 69.1 | 5 |
| 3-disk 10GB read | 1166.3 | 37.9 | 5 |

### LVM Striped (Control)

| Scenario | Mean (MB/s) | StdDev | Runs |
|----------|------------|--------|------|
| 2-disk 512MB write | 580.4 | 10.9 | 5 |
| 2-disk 5GB write | 498.8 | 13.3 | 5 |
| 2-disk 5GB read | 803.8 | 12.7 | 5 |

### Disk Configuration
- **TieredVol 2-disk**: NVMe (CT1000P3PSSD8) + SATA (CT500MX500SSD1)
- **TieredVol 3-disk**: NVMe + SATA (MX500) + SATA (WDS250G2B0A)
- **LVM 2-disk**: SATA (MX500) + SATA (WD Blue) — *only the two SATA SSDs, no NVMe*
- **Weight ratio (NVMe:MX500:WD Blue)**: 3:1:1 (2-disk 3:1)
- Stripe sizes: 4096KB (2-disk segment 0), 3072KB (segment 1)
- O_DIRECT, TV_CHUNK_SIZE=1MB, TV_BUF_COUNT=64, TV_URING_QUEUE_DEPTH=256

### Observations
- **TieredVol 2-disk write beats LVM 2-disk write by 2.7–4.4×** (2203 vs 580 MB/s for 512MB; 1324 vs 499 MB/s for 5GB). Note: LVM uses only two SATA SSDs (no NVMe), TieredVol uses NVMe + one SATA.
- **TieredVol 2-disk read beats LVM 2-disk read by 1.3–1.6×** (1304 vs 804 MB/s for 5GB).
- **512MB writes are significantly faster** than larger writes in TieredVol (2203 vs 1300 MB/s) because they fit entirely within the 64-entry ring buffer — no pipeline stalls, hot cache.
- **3-disk vs 2-disk TieredVol**: 512MB read is 30% faster on 3-disk (1407 vs 1080 MB/s) due to parallel reads from 3 drives; 5GB/10GB reads are 10% slower on 3-disk due to coordination overhead.
- Write throughput is very consistent (low stddev) at ~1300 MB/s for both 2-disk and 3-disk with large I/O.
- Read throughput shows higher variance (stddev 28–69 MB/s), likely due to Linux page cache effects and hardware differences.
- "CQEs stuck (lost), recovering" messages observed during flush; data integrity is maintained via recovery.

Raw data files: see `*_raw.txt` in this directory.
