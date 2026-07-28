#!/bin/bash
# TieredVol-DRIVER — End-to-end comparison: TV vs DRIVER-static vs DRIVER-adaptive vs LVM
#
# Usage: sudo ./scripts/test_scheduler.sh [--disks nvme0n1,sdb] [--size 10GB] [--runs 3]
#
# What it does:
#   1. Detect eligible disks (or use --disks)
#   2. For each implementation:
#      a. TV: tiered_setup --create --scheduler, tiered_io --bench
#      b. DRIVER-static: tiered_setup --create --scheduler, dmsetup message set_policy static, fio
#      c. DRIVER-adaptive: same + set_policy adaptive, fio
#      d. LVM: pvcreate + vgcreate + lvcreate -i <n> -I 2m, fio
#   3. Print comparison table
#   4. Append results to experiments/raw_results.csv

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
SETUP="$PROJECT_DIR/tiered_setup"
IO="$PROJECT_DIR/tiered_io"
CSV="$PROJECT_DIR/experiments/raw_results.csv"

# Defaults
DISKS=""
SIZE="10GB"
RUNS=3
BS="2m"
IODEPTH=256
TEST_NAME="tv_test_$$"
MOUNT_POINT="/mnt/tv_test_$$"

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
    "$SETUP" --remove --name "$TEST_NAME" 2>/dev/null || true
    sudo umount "$MOUNT_POINT" 2>/dev/null || true
    sudo rmdir "$MOUNT_POINT" 2>/dev/null || true
    # Clean LVM
    sudo lvremove -f "tv_vg_${TEST_NAME}/tv_lv_${TEST_NAME}" 2>/dev/null || true
    sudo vgremove -f "tv_vg_${TEST_NAME}" 2>/dev/null || true
    for dev in "${DISK_LIST[@]:-}"; do
        sudo pvremove -ff -y "/dev/mapper/tv_${dev}_carve" 2>/dev/null || true
        sudo dmsetup remove "tv_${dev}_carve" 2>/dev/null || true
    done
}
trap cleanup EXIT

# --- Parse args ---
while [[ $# -gt 0 ]]; do
    case "$1" in
        --disks) DISKS="$2"; shift 2 ;;
        --size)  SIZE="$2"; shift 2 ;;
        --runs)  RUNS="$2"; shift 2 ;;
        --help|-h)
            echo "Usage: $0 [--disks nvme0n1,sdb] [--size 10GB] [--runs 3]"
            exit 0
            ;;
        *) fail "Unknown arg: $1"; exit 1 ;;
    esac
done

# --- Pre-flight ---
if [[ $EUID -ne 0 ]]; then
    fail "Must run as root (sudo)"
    exit 1
fi

for bin in dmsetup fio; do
    if ! command -v "$bin" &>/dev/null; then
        fail "'$bin' not found. Run: sudo apt install $bin"
        exit 1
    fi
done

if [[ ! -x "$SETUP" ]]; then
    fail "tiered_setup not found. Run: make"
    exit 1
fi

# --- Resolve disk list ---
if [[ -n "$DISKS" ]]; then
    IFS=',' read -ra DISK_LIST <<< "$DISKS"
else
    log "Scanning disks..."
    DISK_LIST=()
    while IFS= read -r disk; do
        [[ -z "$disk" ]] && continue
        root_dev=$(findmnt -n -o SOURCE / 2>/dev/null | sed 's|^/dev/||' | sed 's/[0-9]*$//' | sed 's/p[0-9]*$//')
        [[ "$disk" == "$root_dev" ]] && continue
        lsblk -o MOUNTPOINT "/dev/$disk" 2>/dev/null | grep -q '/' && continue
        DISK_LIST+=("$disk")
    done < <("$SETUP" --list 2>/dev/null | grep -E "^[a-z]" | grep -v "loop" | awk '{print $1}')
fi

if [[ ${#DISK_LIST[@]} -lt 2 ]]; then
    fail "Need >= 2 disks. Found: ${DISK_LIST[*]:-none}"
    exit 1
fi

DISK_COUNT=${#DISK_LIST[@]}
DISK_STR=$(IFS=+; echo "${DISK_LIST[*]}")
DISK_ARGS=$(printf "%s:10," "${DISK_LIST[@]}")
DISK_ARGS="${DISK_ARGS%,}"  # trim trailing comma

echo ""
echo "=========================================="
echo "  TieredVol DRIVER Comparison Test"
echo "=========================================="
echo ""
echo "  Disks:    /dev/${DISK_STR}"
echo "  Count:    ${DISK_COUNT}"
echo "  Size:     ${SIZE}"
echo "  BS:       ${BS}"
echo "  Runs:     ${RUNS}"
echo "  CSV:      ${CSV}"
echo ""
echo "=========================================="
echo ""

# --- Ensure CSV header exists ---
if [[ ! -f "$CSV" ]]; then
    echo "exp_id,phase,impl,disks,disk_count,size,run,write_mb_s,read_mb_s,bs,ioengine,notes" > "$CSV"
fi

# --- Helper: run fio and extract results ---
run_fio() {
    local target="$1"
    local label="$2"
    local result
    result=$(fio --name="$label" --filename="$target" --rw=write --bs="$BS" --size="$SIZE" \
        --direct=1 --ioengine=io_uring --iodepth="$IODEPTH" --numjobs=1 \
        --output-format=json 2>&1)
    echo "$result" | python3 -c "
import sys, json
d = json.load(sys.stdin)
jobs = d.get('jobs', [{}])
w = jobs[0].get('write', {}).get('bw', 0) / 1024  # KiB/s → MiB/s
r = jobs[0].get('read', {}).get('bw', 0) / 1024
print(f'{w:.1f},{r:.1f}')
" 2>/dev/null || echo "0.0,0.0"
}

# --- Helper: run fio read (write first then read) ---
run_fio_read() {
    local target="$1"
    local label="$2"
    # Write first
    fio --name="${label}_w" --filename="$target" --rw=write --bs="$BS" --size="$SIZE" \
        --direct=1 --ioengine=io_uring --iodepth="$IODEPTH" --numjobs=1 --output-format=json > /dev/null 2>&1
    # Then read
    local result
    result=$(fio --name="${label}_r" --filename="$target" --rw=read --bs="$BS" --size="$SIZE" \
        --direct=1 --ioengine=io_uring --iodepth="$IODEPTH" --numjobs=1 \
        --output-format=json 2>&1)
    echo "$result" | python3 -c "
import sys, json
d = json.load(sys.stdin)
jobs = d.get('jobs', [{}])
w = jobs[0].get('write', {}).get('bw', 0) / 1024
r = jobs[0].get('read', {}).get('bw', 0) / 1024
print(f'{w:.1f},{r:.1f}')
" 2>/dev/null || echo "0.0,0.0"
}

# --- Test 1: TV (tiered_io --bench) ---
log "=== Test 1: TV (tiered_io --bench) ==="
"$SETUP" --create --name "$TEST_NAME" --disks "$DISK_ARGS" --scheduler 2>&1 | sed 's/^/  /'
ok "TV volume created"

TV_W_SUM=0; TV_R_SUM=0
for i in $(seq 1 "$RUNS"); do
    log "  Run $i/$RUNS..."
    result=$("$IO" --name "$TEST_NAME" --bench --size "$SIZE" --raw 2>&1)
    w=$(echo "$result" | grep -oP '[\d.]+(?= MB/s)' | head -1)
    r=$(echo "$result" | grep -oP '[\d.]+(?= MB/s)' | tail -1)
    w=${w:-0}; r=${r:-0}
    echo "    write=${w} MB/s read=${r} MB/s"
    echo "P2,tv,${DISK_STR},${DISK_COUNT},${SIZE},${i},${w},${r},${BS},pwrite,TV tiered_io --bench" >> "$CSV"
    TV_W_SUM=$(echo "$TV_W_SUM + $w" | bc)
    TV_R_SUM=$(echo "$TV_R_SUM + $r" | bc)
    sleep 5
done
TV_W_MEAN=$(echo "scale=1; $TV_W_SUM / $RUNS" | bc)
TV_R_MEAN=$(echo "scale=1; $TV_R_SUM / $RUNS" | bc)
ok "TV: write=${TV_W_MEAN} read=${TV_R_MEAN} MB/s (mean of $RUNS)"

"$SETUP" --remove --name "$TEST_NAME" 2>&1 | sed 's/^/  /'
sleep 5

# --- Test 2: DRIVER-static ---
log "=== Test 2: DRIVER-static ==="
"$SETUP" --create --name "$TEST_NAME" --disks "$DISK_ARGS" --scheduler 2>&1 | sed 's/^/  /'
sudo dmsetup message "$TEST_NAME" 0 set_policy static 2>&1 | sed 's/^/  /'
ok "DRIVER-static volume created (policy=static)"

DS_W_SUM=0; DS_R_SUM=0
for i in $(seq 1 "$RUNS"); do
    log "  Run $i/$RUNS..."
    wr=$(run_fio "/dev/mapper/$TEST_NAME" "ds_w_${i}")
    w=$(echo "$wr" | cut -d',' -f1)
    r=$(echo "$wr" | cut -d',' -f2)
    echo "    write=${w} read=${r} MB/s"
    echo "P2,driver-static,${DISK_STR},${DISK_COUNT},${SIZE},${i},${w},${r},${BS},io_uring,dmsetup set_policy static" >> "$CSV"
    DS_W_SUM=$(echo "$DS_W_SUM + $w" | bc)
    DS_R_SUM=$(echo "$DS_R_SUM + $r" | bc)
    sleep 5
done
DS_W_MEAN=$(echo "scale=1; $DS_W_SUM / $RUNS" | bc)
DS_R_MEAN=$(echo "scale=1; $DS_R_SUM / $RUNS" | bc)
ok "DRIVER-static: write=${DS_W_MEAN} read=${DS_R_MEAN} MB/s"

"$SETUP" --remove --name "$TEST_NAME" 2>&1 | sed 's/^/  /'
sleep 5

# --- Test 3: DRIVER-adaptive ---
log "=== Test 3: DRIVER-adaptive ==="
"$SETUP" --create --name "$TEST_NAME" --disks "$DISK_ARGS" --scheduler 2>&1 | sed 's/^/  /'
sudo dmsetup message "$TEST_NAME" 0 set_policy adaptive 2>&1 | sed 's/^/  /'
ok "DRIVER-adaptive volume created (policy=adaptive)"

DA_W_SUM=0; DA_R_SUM=0
for i in $(seq 1 "$RUNS"); do
    log "  Run $i/$RUNS..."
    wr=$(run_fio "/dev/mapper/$TEST_NAME" "da_w_${i}")
    w=$(echo "$wr" | cut -d',' -f1)
    r=$(echo "$wr" | cut -d',' -f2)
    echo "    write=${w} read=${r} MB/s"
    echo "P2,driver-adaptive,${DISK_STR},${DISK_COUNT},${SIZE},${i},${w},${r},${BS},io_uring,dmsetup set_policy adaptive" >> "$CSV"
    DA_W_SUM=$(echo "$DA_W_SUM + $w" | bc)
    DA_R_SUM=$(echo "$DA_R_SUM + $r" | bc)
    sleep 5
done
DA_W_MEAN=$(echo "scale=1; $DA_W_SUM / $RUNS" | bc)
DA_R_MEAN=$(echo "scale=1; $DA_R_SUM / $RUNS" | bc)
ok "DRIVER-adaptive: write=${DA_W_MEAN} read=${DA_R_MEAN} MB/s"

"$SETUP" --remove --name "$TEST_NAME" 2>&1 | sed 's/^/  /'
sleep 5

# --- Test 4: LVM striping ---
log "=== Test 4: LVM striping ==="
VG_NAME="tv_vg_${TEST_NAME}"
LV_NAME="tv_lv_${TEST_NAME}"

# Create dm-linear carve targets
for dev in "${DISK_LIST[@]}"; do
    target="tv_${dev}_carve"
    sectors=$(( 10 * 1024 * 1024 * 1024 / 512 ))
    echo "0 ${sectors} linear /dev/${dev} 0" | sudo dmsetup create "$target"
done

# Create PV + VG + striped LV
pv_args=()
for dev in "${DISK_LIST[@]}"; do
    pv_args+=("/dev/mapper/tv_${dev}_carve")
done
sudo pvcreate -f "${pv_args[@]}" 2>&1 | sed 's/^/  /'
sudo vgcreate -f "$VG_NAME" "${pv_args[@]}" 2>&1 | sed 's/^/  /'
sudo lvcreate -l 100%FREE -i "$DISK_COUNT" -I 2m -n "$LV_NAME" "$VG_NAME" 2>&1 | sed 's/^/  /'
ok "LVM volume created (/dev/mapper/${VG_NAME}-${LV_NAME})"

LVM_TARGET="/dev/mapper/${VG_NAME}-${LV_NAME}"
LM_W_SUM=0; LM_R_SUM=0
for i in $(seq 1 "$RUNS"); do
    log "  Run $i/$RUNS..."
    wr=$(run_fio "$LVM_TARGET" "lvm_w_${i}")
    w=$(echo "$wr" | cut -d',' -f1)
    r=$(echo "$wr" | cut -d',' -f2)
    echo "    write=${w} read=${r} MB/s"
    echo "P2,lvm,${DISK_STR},${DISK_COUNT},${SIZE},${i},${w},${r},${BS},io_uring,LVM stripe -i ${DISK_COUNT} -I 2m" >> "$CSV"
    LM_W_SUM=$(echo "$LM_W_SUM + $w" | bc)
    LM_R_SUM=$(echo "$LM_R_SUM + $r" | bc)
    sleep 5
done
LM_W_MEAN=$(echo "scale=1; $LM_W_SUM / $RUNS" | bc)
LM_R_MEAN=$(echo "scale=1; $LM_R_SUM / $RUNS" | bc)
ok "LVM: write=${LM_W_MEAN} read=${LM_R_MEAN} MB/s"

# Cleanup LVM
sudo lvremove -f "${VG_NAME}/${LV_NAME}" 2>&1 | sed 's/^/  /'
sudo vgremove -f "$VG_NAME" 2>&1 | sed 's/^/  /'
for dev in "${DISK_LIST[@]}"; do
    sudo pvremove -ff -y "/dev/mapper/tv_${dev}_carve" 2>/dev/null || true
    sudo dmsetup remove "tv_${dev}_carve" 2>/dev/null || true
done
ok "LVM volume destroyed"

# --- Summary ---
echo ""
echo "=========================================="
echo "  Comparison Results (${DISK_STR}, ${SIZE}, ${RUNS} runs)"
echo "=========================================="
echo ""
printf "  %-25s %10s %10s\n" "Method" "Write" "Read"
printf "  %-25s %10s %10s\n" "------" "-----" "----"
printf "  %-25s %9s%s %9s%s\n" "TV (tiered_io)" "${TV_W_MEAN}" " MB/s" "${TV_R_MEAN}" " MB/s"
printf "  %-25s %9s%s %9s%s\n" "DRIVER-static" "${DS_W_MEAN}" " MB/s" "${DS_R_MEAN}" " MB/s"
printf "  %-25s %9s%s %9s%s\n" "DRIVER-adaptive" "${DA_W_MEAN}" " MB/s" "${DA_R_MEAN}" " MB/s"
printf "  %-25s %9s%s %9s%s\n" "LVM" "${LM_W_MEAN}" " MB/s" "${LM_R_MEAN}" " MB/s"
echo ""
echo "  Results appended to: $CSV"
echo "=========================================="
