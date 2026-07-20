#!/bin/bash
# bench_fio.sh — fio io_uring benchmark on LVM for fair comparison
# Usage: sudo bash bench_fio.sh
#
# Prerequisites:
#   - LVM volume created via tiered_setup
#   - fio installed: apt install fio
#
# Tests LVM with io_uring engine + iodepth=64, matching our scheduler's
# I/O pattern for a fair three-way comparison.

set -e

SIZES="512m 5g 10g"
BS="256k"
IODEPTH=64
DIRECT=1

echo "=== fio io_uring LVM benchmark ==="
echo "bs=$BS  iodepth=$IODEPTH  direct=$DIRECT"

for sz in $SIZES; do
    echo ""
    echo "=== LVM ext4 — $sz ==="
    sudo fio --name=lvm_ext4_$sz \
        --directory=/mnt/test \
        --ioengine=io_uring \
        --iodepth=$IODEPTH \
        --direct=$DIRECT \
        --rw=write \
        --bs=$BS \
        --size=$sz \
        --numjobs=1 \
        --group_reporting \
        --fallocate=0 2>&1
done

RAW_DEV="/dev/mapper/tv_vg_testpool2-tv_lv_testpool2"
if [ -b "$RAW_DEV" ]; then
    for sz in $SIZES; do
        echo ""
        echo "=== LVM raw — $sz ==="
        sudo fio --name=lvm_raw_$sz \
            --filename=$RAW_DEV \
            --ioengine=io_uring \
            --iodepth=$IODEPTH \
            --direct=$DIRECT \
            --rw=write \
            --bs=$BS \
            --size=$sz \
            --numjobs=1 \
            --group_reporting \
            --fallocate=0 2>&1
    done
fi

echo ""
echo "=== Done ==="
