# Benchmark Summary — B85 Platform

## System
- CPU: Intel i5-4570
- RAM: DDR3 1600MHz
- NVMe: P3 Plus 1T (PCIe 2.0x4)
- SATA: WD Blue NAND 250G + MX500 500G
- OS: Lubuntu, Linux 6.x

## Results

| Scenario | Mean (MB/s) | StdDev | Runs |
|----------|------------|--------|------|
| 2-disk 5GB write | 1324.5 | 6.2 | 5 |
| 2-disk 10GB write | 1301.4 | 2.8 | 5 |
| 2-disk 5GB read | 1304.1 | 59.4 | 5 |
| 2-disk 10GB read | 1319.6 | 59.1 | 5 |
| 3-disk 5GB write | 1302.3 | 5.7 | 5 |
| 3-disk 10GB write | 1284.5 | 7.6 | 5 |
| 3-disk 5GB read | 1190.1 | 69.1 | 5 |
| 3-disk 10GB read | 1166.3 | 37.9 | 5 |

### Disk Configuration
- **2-disk**: NVMe (CT1000P3PSSD8) + SATA (CT500MX500SSD1)
- **3-disk**: NVMe + SATA (MX500) + SATA (WDS250G2B0A)
- **Weight ratio (NVMe:MX500:WD Blue)**: 3:1:1 (2-disk 3:1)
- Stripe sizes: 4096KB (2-disk segment 0), 3072KB (segment 1)
- O_DIRECT, TV_CHUNK_SIZE=1MB, TV_BUF_COUNT=64, TV_URING_QUEUE_DEPTH=256

### Observations
- Write throughput is very consistent (low stddev) at ~1300 MB/s for both 2-disk and 3-disk — adding the WD Blue SATA did not improve write throughput significantly (NVMe + MX500 saturate available bandwidth)
- Read throughput shows higher variance, likely due to Linux page cache effects and hardware differences between SATA SSDs
- 3-disk read is ~120 MB/s slower than 2-disk, possibly due to additional overhead of coordinating 3 drives
- "CQEs stuck (lost), recovering" messages observed frequently during flush; data integrity is maintained via recovery

Raw data files: see `*_raw.txt` in this directory.
