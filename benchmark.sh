#!/bin/bash
# TieredVol-DRIVER benchmark script
# Usage: sudo ./benchmark.sh [block_size] [queue_depth] [test_size]
# Default: bs=2m, qd=256, size=2G

set -euo pipefail

BS="${1:-2m}"
QD="${2:-256}"
SIZE="${3:-2G}"
RUNS=3
DM_DEVICE="/dev/mapper/testpool"
NVME_DEVICE="/dev/nvme0n1"
CONF="/etc/tieredvol/testpool.conf"

if [[ $EUID -ne 0 ]]; then
    echo "Error: must run as root" >&2
    exit 1
fi

echo "=== TieredVol Benchmark ==="
echo "Config: bs=$BS qd=$QD size=$SIZE runs=$RUNS"
echo ""

# Ensure required sysfs settings
echo 0 > /sys/block/nvme0n1/queue/wbt_lat_usec 2>/dev/null || true
echo none > /sys/block/nvme0n1/queue/scheduler 2>/dev/null || true
echo 1024 > /sys/module/dm_mod/parameters/reserved_bio_based_ios 2>/dev/null || true

# Verify NVMe write cache is ON
CACHE=$(nvme get-feature -f 0x06 /dev/nvme0 2>/dev/null | grep -o 'enabled\|disabled' || echo "unknown")
if [[ "$CACHE" == "disabled" ]]; then
    echo "WARNING: NVMe write cache is disabled! Restoring..."
    nvme set-feature -f 0x06 -v 1 /dev/nvme0
    nvme reset /dev/nvme0
    sleep 2
fi

echo "--- NVMe Write Cache: $CACHE ---"
echo ""

# Raw NVMe baseline
echo "=== Raw NVMe Sequential Write ==="
for i in $(seq 1 $RUNS); do
    fio --name=raw_nvme --filename=$NVME_DEVICE --rw=write --bs=$BS \
        --size=$SIZE --direct=1 --ioengine=io_uring --iodepth=$QD \
        --numjobs=1 --end_fsync=1 2>&1 | grep "WRITE:"
done
echo ""

# DM device test
echo "=== DM Device Sequential Write (testpool) ==="
for i in $(seq 1 $RUNS); do
    fio --name=dm_test --filename=$DM_DEVICE --rw=write --bs=$BS \
        --size=$SIZE --direct=1 --ioengine=io_uring --iodepth=$QD \
        --numjobs=1 --end_fsync=1 2>&1 | grep "WRITE:"
done
echo ""

# DM device metadata commands
echo "=== DM Stats ==="
dmsetup message testpool reset_stats 2>/dev/null || true
echo "Commands: reset_stats, show_stats, status"
echo ""

echo "=== Done ==="
