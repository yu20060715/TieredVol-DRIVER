#!/bin/bash
# TieredVol — End-to-end scheduler test: weighted striping vs LVM striping
#
# Usage: sudo ./scripts/test_scheduler.sh
#
# What it does:
#   1. Detect available non-root, non-mounted disks
#   2. Create a weighted I/O scheduler volume
#   3. Benchmark with tiered_io --bench
#   4. Destroy scheduler volume
#   5. Create an LVM striped volume (same disks, same capacity)
#   6. Benchmark with dd
#   7. Print comparison table

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
SETUP="$PROJECT_DIR/tiered_setup"
IO="$PROJECT_DIR/tiered_io"
TEST_NAME="tv_test_$$"
MOUNT_POINT="/mnt/tv_test_$$"
BENCH_SIZE="128MB"

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
NC='\033[0m'

log()  { echo -e "${CYAN}[TEST]${NC} $*"; }
ok()   { echo -e "${GREEN}[OK]${NC} $*"; }
warn() { echo -e "${YELLOW}[WARN]${NC} $*"; }
fail() { echo -e "${RED}[FAIL]${NC} $*"; }

cleanup() {
    log "Cleaning up..."
    "$SETUP" --destroy --name "$TEST_NAME" 2>/dev/null || true
    sudo umount "$MOUNT_POINT" 2>/dev/null || true
    sudo rmdir "$MOUNT_POINT" 2>/dev/null || true
}
trap cleanup EXIT

# --- Pre-flight checks ---
if [[ $EUID -ne 0 ]]; then
    fail "This script must be run as root (sudo)"
    exit 1
fi

for bin in dmsetup vgcreate; do
    if ! command -v "$bin" &>/dev/null; then
        fail "'$bin' not found. Run: sudo apt install lvm2 (or dnf/pacman equivalent)"
        exit 1
    fi
done

if [[ ! -x "$SETUP" ]]; then
    fail "tiered_setup not found. Run: make clean && make"
    exit 1
fi
if [[ ! -x "$IO" ]]; then
    fail "tiered_io not found. Run: make clean && make"
    exit 1
fi

# --- Find eligible disks ---
log "Scanning disks..."
DISK_LIST=$("$SETUP" --list 2>/dev/null | grep -E "^[a-z]" | grep -v "loop" | awk '{print $1}')
ELIGIBLE=()
while IFS= read -r disk; do
    [[ -z "$disk" ]] && continue
    # Skip root disk
    root_dev=$(findmnt -n -o SOURCE / 2>/dev/null | sed 's|^/dev/||' | sed 's/[0-9]*$//' | sed 's/p[0-9]*$//')
    if [[ "$disk" == "$root_dev" ]]; then
        continue
    fi
    # Skip mounted
    if lsblk -o MOUNTPOINT "/dev/$disk" 2>/dev/null | grep -q '/'; then
        continue
    fi
    ELIGIBLE+=("$disk")
done <<< "$DISK_LIST"

if [[ ${#ELIGIBLE[@]} -lt 2 ]]; then
    fail "Need at least 2 non-root, non-mounted disks. Found: ${#ELIGIBLE[@]}"
    fail "Eligible disks: ${ELIGIBLE[*]:-none}"
    exit 1
fi

# Use first 2 eligible disks
DISK_A="${ELIGIBLE[0]}"
DISK_B="${ELIGIBLE[1]}"
# Use 10GB from each for testing
TEST_SIZE_GB=10

echo ""
echo "=========================================="
echo "  TieredVol Scheduler Test"
echo "=========================================="
echo ""
echo "  Disks:    /dev/$DISK_A + /dev/$DISK_B"
echo "  Size:     ${TEST_SIZE_GB}GB each"
echo "  Bench:    $BENCH_SIZE sequential write"
echo ""
echo "=========================================="
echo ""

# --- Test 1: Weighted I/O Scheduler ---
log "=== Test 1: Weighted I/O Scheduler ==="
log "Creating scheduler volume..."
"$SETUP" --create --name "$TEST_NAME" \
    --disks "${DISK_A}:${TEST_SIZE_GB},${DISK_B}:${TEST_SIZE_GB}" \
    --scheduler 2>&1 | sed 's/^/  /'

if [[ ! -f "/etc/tieredvol/${TEST_NAME}.scheduler" ]]; then
    fail "Scheduler metadata not created"
    exit 1
fi
ok "Scheduler volume created"

log "Running tiered_io benchmark..."
SCHED_RESULT=$("$IO" --name "$TEST_NAME" --bench --size "$BENCH_SIZE" 2>&1)
SCHED_MBS=$(echo "$SCHED_RESULT" | grep "Throughput:" | awk '{print $2}')
echo "$SCHED_RESULT" | sed 's/^/  /'
ok "Scheduler benchmark: ${SCHED_MBS:-?} MB/s"

log "Destroying scheduler volume..."
"$SETUP" --destroy --name "$TEST_NAME" 2>&1 | sed 's/^/  /'
ok "Scheduler volume destroyed"
echo ""

# --- Test 2: LVM Striping ---
log "=== Test 2: LVM Striping ==="
log "Creating LVM striped volume..."
"$SETUP" --create --name "$TEST_NAME" \
    --disks "${DISK_A}:${TEST_SIZE_GB},${DISK_B}:${TEST_SIZE_GB}" \
    --fs ext4 --mount "$MOUNT_POINT" 2>&1 | sed 's/^/  /'
ok "LVM volume created"

log "Running dd benchmark..."
sync
echo 3 | sudo tee /proc/sys/vm/drop_caches > /dev/null
DD_RESULT=$(dd if=/dev/zero of="${MOUNT_POINT}/benchfile" bs=1M count=128 oflag=direct 2>&1)
DD_MBS=$(echo "$DD_RESULT" | grep -oP '[\d.]+(?= MB/s)')
echo "  $DD_RESULT"
ok "LVM benchmark: ${DD_MBS:-?} MB/s"

log "Destroying LVM volume..."
"$SETUP" --destroy --name "$TEST_NAME" 2>&1 | sed 's/^/  /'
ok "LVM volume destroyed"
echo ""

# --- Summary ---
echo ""
echo "=========================================="
echo "  Comparison Results"
echo "=========================================="
echo ""
printf "  %-30s %10s\n" "Method" "Throughput"
printf "  %-30s %10s\n" "------" "----------"
printf "  %-30s %10s\n" "Weighted I/O Scheduler" "${SCHED_MBS:-?} MB/s"
printf "  %-30s %10s\n" "LVM Striping (fixed)" "${DD_MBS:-?} MB/s"
echo ""

if [[ -n "$SCHED_MBS" && -n "$DD_MBS" ]]; then
    RATIO=$(echo "scale=2; $SCHED_MBS / $DD_MBS" | bc 2>/dev/null || echo "?")
    echo "  Ratio (weighted / LVM): ${RATIO}x"
    echo ""
fi

echo "=========================================="
echo "  Test complete"
echo "=========================================="
