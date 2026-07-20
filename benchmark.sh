#!/bin/bash
set -e

# ===== Step A: 全部清理 =====
sudo dmsetup remove tv_nvme0n1_carve 2>/dev/null
sudo dmsetup remove tv_sdb_carve 2>/dev/null
sudo dmsetup remove tv_sdc_carve 2>/dev/null
sudo rm -f /etc/tieredvol/testpool.scheduler
sudo rm -f /etc/tieredvol/testpool2.scheduler
sudo umount /mnt/test 2>/dev/null
sudo lvremove -f /dev/mapper/testpool2* 2>/dev/null
sudo vgremove -f testpool2 2>/dev/null
sudo pvremove /dev/mapper/tv_nvme0n1_carve /dev/mapper/tv_sdb_carve /dev/mapper/tv_sdc_carve 2>/dev/null

echo "=== Step A: Cleanup ==="
sudo dmsetup ls | grep tv_ || echo "  (clean)"

# ===== Step B: 拉最新代碼 + 編譯 =====
echo ""
echo "=== Step B: Build ==="
cd ~/TieredVol
git pull
make clean && make

# ===== Step C: 單元測試 =====
echo ""
echo "=== Step C: Unit tests ==="
make test

# ===== Step D: 建立 Scheduler volume =====
echo ""
echo "=== Step D: Create scheduler volume ==="
sudo ./tiered_setup --create --name testpool \
    --disks sdb:200,sdc:100,nvme0n1:200 \
    --scheduler

# ===== Step E: Scheduler bench-all =====
echo ""
echo "=== Step E: Scheduler bench-all ==="
sudo ./tiered_io --name testpool --bench-all 2>&1 | tee ~/Desktop/scheduler_result.txt

# ===== Step F: Destroy scheduler =====
echo ""
echo "=== Step F: Destroy scheduler ==="
sudo ./tiered_setup --destroy --name testpool
sudo dmsetup ls | grep tv_ || echo "  (clean)"

# ===== Step G: 建立 LVM striped volume =====
echo ""
echo "=== Step G: Create LVM volume ==="
sudo ./tiered_setup --create --name testpool2 \
    --disks sdb:200,sdc:100,nvme0n1:200 \
    --fs ext4 --mount /mnt/test

# ===== Step H: 確認 LVM =====
echo ""
echo "=== Step H: Verify LVM ==="
lsblk | grep test

# ===== Step I: LVM ext4 bench-all =====
echo ""
echo "=== Step I: LVM ext4 bench-all ==="
sudo ./tiered_io --path /mnt/test --bench-all 2>&1 | tee ~/Desktop/path_fs_result.txt

# ===== Step J: LVM raw bench-all (bypass filesystem) =====
echo ""
echo "=== Step J: LVM raw bench-all (bypass ext4) ==="
LVM_DEV="/dev/mapper/tv_vg_testpool2-tv_lv_testpool2"
echo "  Raw device: $LVM_DEV"
sudo ./tiered_io --path "$LVM_DEV" --bench-all --raw 2>&1 | tee ~/Desktop/path_raw_result.txt

# ===== Step K: 看結果 =====
echo ""
echo "============================================"
echo "========== Scheduler（加權） =========="
echo "============================================"
cat ~/Desktop/scheduler_result.txt
echo ""
echo "============================================"
echo "========== Path ext4（LVM striping）========="
echo "============================================"
cat ~/Desktop/path_fs_result.txt
echo ""
echo "============================================"
echo "========== Path raw（LVM 直寫）=========="
echo "============================================"
cat ~/Desktop/path_raw_result.txt

# ===== Step L: 清理 =====
echo ""
echo "=== Step L: Cleanup ==="
sudo ./tiered_setup --destroy --name testpool2
