#!/bin/bash
# TieredVol-DRIVER — Full benchmark suite (3 phases, 216+ tests)
#
# Usage: sudo ./scripts/run_benchmark.sh [--runs 3] [--phases 1,2,3]
#
# Phase 1: Raw baseline (each disk individually)
# Phase 2: Main comparison (TV / DRIVER-static / DRIVER-adaptive / LVM)
# Phase 3: Background load (4-disk, DRIVER only)
#
# Results → experiments/raw_results.csv
# Summary → experiments/summary.csv (auto-generated at end)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
SETUP="$PROJECT_DIR/tiered_setup"
IO="$PROJECT_DIR/tiered_io"
RAW_CSV="$PROJECT_DIR/experiments/raw_results.csv"
SUMMARY_CSV="$PROJECT_DIR/experiments/summary.csv"

# Defaults
RUNS=3
PHASES="1,2,3"
BS="2m"
SIZES="512MB 5GB 10GB"
IODEPTH=256

CYAN='\033[0;36m'
GREEN='\033[0;32m'
RED='\033[0;31m'
NC='\033[0m'

log()  { echo -e "${CYAN}[$(date +%H:%M:%S)]${NC} $*"; }
ok()   { echo -e "${GREEN}[OK]${NC} $*"; }
fail() { echo -e "${RED}[FAIL]${NC} $*"; }

# --- Parse args ---
while [[ $# -gt 0 ]]; do
    case "$1" in
        --runs)   RUNS="$2"; shift 2 ;;
        --phases) PHASES="$2"; shift 2 ;;
        --help|-h)
            echo "Usage: $0 [--runs 3] [--phases 1,2,3]"
            echo "  --runs N      Runs per test (default: 3)"
            echo "  --phases X,Y  Comma-separated phases to run (default: 1,2,3)"
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

for bin in dmsetup fio python3 bc; do
    command -v "$bin" &>/dev/null || { fail "'$bin' not found"; exit 1; }
done

[[ -x "$SETUP" ]] || { fail "tiered_setup not found. Run: make"; exit 1; }
[[ -x "$IO" ]]     || { fail "tiered_io not found. Run: make"; exit 1; }

# --- Discover disks ---
log "Discovering disks..."
ALL_DISKS=()
while IFS= read -r disk; do
    [[ -z "$disk" ]] && continue
    root_dev=$(findmnt -n -o SOURCE / 2>/dev/null | sed 's|^/dev/||' | sed 's/[0-9]*$//' | sed 's/p[0-9]*$//')
    [[ "$disk" == "$root_dev" ]] && continue
    lsblk -o MOUNTPOINT "/dev/$disk" 2>/dev/null | grep -q '/' && continue
    ALL_DISKS+=("$disk")
done < <("$SETUP" --list 2>/dev/null | grep -E "^[a-z]" | grep -v "loop" | awk '{print $1}')

log "Found ${#ALL_DISKS[@]} eligible disks: ${ALL_DISKS[*]}"

if [[ ${#ALL_DISKS[@]} -lt 2 ]]; then
    fail "Need >= 2 eligible disks"
    exit 1
fi

# --- Disk configurations (from plan) ---
# C1: nvme0+nvme1, C2: nvme0+sdb, C3: sdb+sdc
# C4: nvme0+nvme1+sdb, C5: nvme0+sdb+sdc, C6: all 4
declare -A CONFIGS
get_disk() {
    # Map logical names to actual disks
    local idx=$1
    if [[ $idx -lt ${#ALL_DISKS[@]} ]]; then
        echo "${ALL_DISKS[$idx]}"
    fi
}

# Build configs from available disks
CONFIG_NAMES=()
CONFIG_LIST=()

build_configs() {
    local n=${#ALL_DISKS[@]}
    # 2-disk configs
    for ((i=0; i<n; i++)); do
        for ((j=i+1; j<n; j++)); do
            CONFIG_NAMES+=("C${#CONFIG_NAMES[@]}")
            CONFIG_LIST+=("$(get_disk $i),$(get_disk $j)")
        done
    done
    # 3-disk configs
    for ((i=0; i<n; i++)); do
        for ((j=i+1; j<n; j++)); do
            for ((k=j+1; k<n; k++)); do
                CONFIG_NAMES+=("C${#CONFIG_NAMES[@]}")
                CONFIG_LIST+=("$(get_disk $i),$(get_disk $j),$(get_disk $k)")
            done
        done
    done
    # 4-disk config (if 4 disks)
    if [[ $n -ge 4 ]]; then
        CONFIG_NAMES+=("C${#CONFIG_NAMES[@]}")
        CONFIG_LIST+=("$(IFS=,; echo "${ALL_DISKS[*]}")")
    fi
}

build_configs

log "Disk configs: ${#CONFIG_LIST[@]}"

# --- Helper functions ---
cleanup_volume() {
    local name="$1"
    "$SETUP" --remove --name "$name" 2>/dev/null || true
    sudo lvremove -f "tv_vg_${name}/tv_lv_${name}" 2>/dev/null || true
    sudo vgremove -f "tv_vg_${name}" 2>/dev/null || true
    for dev in $(echo "$2" | tr ',' ' '); do
        sudo pvremove -ff -y "/dev/mapper/tv_${dev}_carve" 2>/dev/null || true
        sudo dmsetup remove "tv_${dev}_carve" 2>/dev/null || true
    done
}

run_fio_write() {
    local target="$1" label="$2" size="$3"
    fio --name="$label" --filename="$target" --rw=write --bs="$BS" --size="$size" \
        --direct=1 --ioengine=io_uring --iodepth="$IODEPTH" --numjobs=1 \
        --output-format=json 2>&1 | python3 -c "
import sys, json
d = json.load(sys.stdin)
bw = d['jobs'][0]['write']['bw'] / 1024
print(f'{bw:.1f}')
" 2>/dev/null || echo "0"
}

run_fio_read() {
    local target="$1" label="$2" size="$3"
    # Write first to fill
    fio --name="${label}_fill" --filename="$target" --rw=write --bs="$BS" --size="$size" \
        --direct=1 --ioengine=io_uring --iodepth="$IODEPTH" --numjobs=1 --output-format=json > /dev/null 2>&1
    # Then read
    fio --name="$label" --filename="$target" --rw=read --bs="$BS" --size="$size" \
        --direct=1 --ioengine=io_uring --iodepth="$IODEPTH" --numjobs=1 \
        --output-format=json 2>&1 | python3 -c "
import sys, json
d = json.load(sys.stdin)
bw = d['jobs'][0]['read']['bw'] / 1024
print(f'{bw:.1f}')
" 2>/dev/null || echo "0"
}

# --- Initialize CSV ---
echo "exp_id,phase,impl,disks,disk_count,size,run,write_mb_s,read_mb_s,bs,ioengine,notes" > "$RAW_CSV"

TOTAL_TESTS=0
PASS_TESTS=0

run_test() {
    local exp_id="$1" phase="$2" impl="$3" disks="$4" disk_count="$5" size="$6" target="$7"
    local notes="$8"
    local w r

    for run in $(seq 1 "$RUNS"); do
        TOTAL_TESTS=$((TOTAL_TESTS + 1))
        log "  [$exp_id] $impl $size run $run/$RUNS"

        if [[ "$impl" == "tv" ]]; then
            result=$("$IO" --name "$exp_id" --bench --size "$size" --raw 2>&1)
            w=$(echo "$result" | grep -oP '[\d.]+(?= MB/s)' | head -1)
            r=$(echo "$result" | grep -oP '[\d.]+(?= MB/s)' | tail -1)
            w=${w:-0}; r=${r:-0}
        else
            w=$(run_fio_write "$target" "${exp_id}_w_${run}" "$size")
            r=$(run_fio_read "$target" "${exp_id}_r_${run}" "$size")
        fi

        echo "${exp_id},${phase},${impl},${disks},${disk_count},${size},${run},${w},${r},${BS},$([ "$impl" = "tv" ] && echo "pwrite" || echo "io_uring"),${notes}" >> "$RAW_CSV"
        PASS_TESTS=$((PASS_TESTS + 1))
        echo "    write=${w} read=${r} MB/s"
        sleep 5
    done
}

# =============================================
#  PHASE 1: Raw baseline
# =============================================
has_phase() { echo "$PHASES" | grep -q "$1"; }

if has_phase 1; then
    log "=========================================="
    log "  PHASE 1: Raw Baseline (${#ALL_DISKS[@]} disks × ${#SIZES[@]} sizes × ${RUNS} runs)"
    log "=========================================="

    for disk in "${ALL_DISKS[@]}"; do
        for size in $SIZES; do
            exp_id="P1_raw_${disk}_${size}"
            log "[$exp_id] Raw write+read /dev/$disk"
            for run in $(seq 1 "$RUNS"); do
                TOTAL_TESTS=$((TOTAL_TESTS + 1))
                w=$(run_fio_write "/dev/$disk" "${exp_id}_w_${run}" "$size")
                r=$(run_fio_read "/dev/$disk" "${exp_id}_r_${run}" "$size")
                echo "${exp_id},P1,raw,${disk},1,${size},${run},${w},${r},${BS},io_uring,raw baseline" >> "$RAW_CSV"
                PASS_TESTS=$((PASS_TESTS + 1))
                echo "    write=${w} read=${r} MB/s"
                sleep 5
            done
        done
    done
fi

# =============================================
#  PHASE 2: Main comparison
# =============================================
if has_phase 2; then
    log "=========================================="
    log "  PHASE 2: Main Comparison (${#CONFIG_LIST[@]} configs × 4 impls × ${#SIZES[@]} sizes × ${RUNS} runs)"
    log "=========================================="

    for ci in "${!CONFIG_LIST[@]}"; do
        disks="${CONFIG_LIST[$ci]}"
        cname="${CONFIG_NAMES[$ci]}"
        disk_count=$(echo "$disks" | tr ',' '\n' | wc -l)
        disk_str=$(echo "$disks" | tr '+' '+')
        vol_name="tv_bench_${cname}"

        for size in $SIZES; do
            # --- TV ---
            log "[$cname] TV create (${disks})"
            "$SETUP" --create --name "$vol_name" --disks "$(echo "$disks" | sed 's/,/:10,/g;s/$/:10/')" --scheduler 2>&1 | sed 's/^/    /'
            run_test "${cname}-tv" "P2" "tv" "$disk_str" "$disk_count" "$size" "/dev/mapper/$vol_name" "tiered_io --bench"
            cleanup_volume "$vol_name" "$disks"
            sleep 5

            # --- DRIVER-static ---
            log "[$cname] DRIVER-static create"
            "$SETUP" --create --name "$vol_name" --disks "$(echo "$disks" | sed 's/,/:10,/g;s/$/:10/')" --scheduler 2>&1 | sed 's/^/    /'
            sudo dmsetup message "$vol_name" 0 set_policy static 2>&1
            run_test "${cname}-driver-static" "P2" "driver-static" "$disk_str" "$disk_count" "$size" "/dev/mapper/$vol_name" "dmsetup set_policy static"
            cleanup_volume "$vol_name" "$disks"
            sleep 5

            # --- DRIVER-adaptive ---
            log "[$cname] DRIVER-adaptive create"
            "$SETUP" --create --name "$vol_name" --disks "$(echo "$disks" | sed 's/,/:10,/g;s/$/:10/')" --scheduler 2>&1 | sed 's/^/    /'
            sudo dmsetup message "$vol_name" 0 set_policy adaptive 2>&1
            run_test "${cname}-driver-adaptive" "P2" "driver-adaptive" "$disk_str" "$disk_count" "$size" "/dev/mapper/$vol_name" "dmsetup set_policy adaptive"
            cleanup_volume "$vol_name" "$disks"
            sleep 5

            # --- LVM ---
            log "[$cname] LVM create"
            IFS=',' read -ra dev_arr <<< "$disks"
            vg_name="tv_vg_${vol_name}"
            lv_name="tv_lv_${vol_name}"
            for dev in "${dev_arr[@]}"; do
                sectors=$(( 10 * 1024 * 1024 * 1024 / 512 ))
                echo "0 ${sectors} linear /dev/${dev} 0" | sudo dmsetup create "tv_${dev}_carve"
            done
            pv_args=()
            for dev in "${dev_arr[@]}"; do pv_args+=("/dev/mapper/tv_${dev}_carve"); done
            sudo pvcreate -f "${pv_args[@]}" 2>&1 | sed 's/^/    /'
            sudo vgcreate -f "$vg_name" "${pv_args[@]}" 2>&1 | sed 's/^/    /'
            sudo lvcreate -l 100%FREE -i "$disk_count" -I 2m -n "$lv_name" "$vg_name" 2>&1 | sed 's/^/    /'
            run_test "${cname}-lvm" "P2" "lvm" "$disk_str" "$disk_count" "$size" "/dev/mapper/${vg_name}-${lv_name}" "LVM stripe -i ${disk_count} -I 2m"
            sudo lvremove -f "${vg_name}/${lv_name}" 2>/dev/null
            sudo vgremove -f "$vg_name" 2>/dev/null
            for dev in "${dev_arr[@]}"; do
                sudo pvremove -ff -y "/dev/mapper/tv_${dev}_carve" 2>/dev/null || true
                sudo dmsetup remove "tv_${dev}_carve" 2>/dev/null || true
            done
            sleep 5
        done
    done
fi

# =============================================
#  PHASE 3: Background load
# =============================================
if has_phase 3; then
    log "=========================================="
    log "  PHASE 3: Background Load (4-disk, DRIVER only)"
    log "=========================================="

    if [[ ${#ALL_DISKS[@]} -lt 4 ]]; then
        log "  SKIP  Need 4 disks for Phase 3 (found ${#ALL_DISKS[@]})"
    else
        bg_disks="$(IFS=,; echo "${ALL_DISKS[*]}")"
        vol_name="tv_bench_bg"
        bg_disk="${ALL_DISKS[1]}"  # second disk for background load

        for scenario in baseline bg_sdb bg_nvme1; do
            log "[$scenario] Create volume"
            "$SETUP" --create --name "$vol_name" --disks "$(echo "$bg_disks" | sed 's/,/:10,/g;s/$/:10/')" --scheduler 2>&1 | sed 's/^/    /'
            sudo dmsetup message "$vol_name" 0 set_policy static 2>&1

            bg_pid=""
            if [[ "$scenario" == "bg_sdb" ]]; then
                log "  Starting bg load on /dev/$bg_disk..."
                sudo dd if=/dev/zero of="/dev/$bg_disk" bs=1M oflag=direct &
                bg_pid=$!
            elif [[ "$scenario" == "bg_nvme1" ]]; then
                log "  Starting bg load on /dev/${ALL_DISKS[1]}..."
                sudo dd if=/dev/zero of="/dev/${ALL_DISKS[1]}" bs=1M oflag=direct &
                bg_pid=$!
            fi

            sleep 2

            for size in 5GB; do
                run_test "bg-${scenario}" "P3" "driver-static" "$bg_disks" 4 "$size" "/dev/mapper/$vol_name" "bg_load=${scenario}"
            done

            if [[ -n "$bg_pid" ]]; then
                sudo kill $bg_pid 2>/dev/null || true
                wait $bg_pid 2>/dev/null || true
            fi

            cleanup_volume "$vol_name" "$bg_disks"
            sleep 5
        done
    fi
fi

# =============================================
#  Generate summary
# =============================================
log "Generating summary..."
python3 - "$RAW_CSV" "$SUMMARY_CSV" <<'PYEOF'
import sys, csv
from collections import defaultdict

raw_path = sys.argv[1]
summary_path = sys.argv[2]

groups = defaultdict(lambda: {"w": [], "r": []})

with open(raw_path) as f:
    reader = csv.DictReader(f)
    for row in reader:
        key = (row["exp_id"], row["phase"], row["impl"], row["disks"], row["disk_count"], row["size"])
        groups[key]["w"].append(float(row["write_mb_s"]))
        groups[key]["r"].append(float(row["read_mb_s"]))

with open(summary_path, "w", newline="") as f:
    writer = csv.writer(f)
    writer.writerow(["exp_id", "phase", "impl", "disks", "disk_count", "size", "write_mean", "write_stddev", "read_mean", "read_stddev"])
    for key in sorted(groups.keys()):
        exp_id, phase, impl, disks, disk_count, size = key
        w_vals = groups[key]["w"]
        r_vals = groups[key]["r"]
        w_mean = sum(w_vals) / len(w_vals)
        r_mean = sum(r_vals) / len(r_vals)
        w_std = (sum((x - w_mean)**2 for x in w_vals) / max(len(w_vals)-1, 1)) ** 0.5
        r_std = (sum((x - r_mean)**2 for x in r_vals) / max(len(r_vals)-1, 1)) ** 0.5
        writer.writerow([exp_id, phase, impl, disks, disk_count, size,
                        f"{w_mean:.1f}", f"{w_std:.1f}", f"{r_mean:.1f}", f"{r_std:.1f}"])

print(f"  Summary: {len(groups)} groups → {summary_path}")
PYEOF

# --- Final summary ---
echo ""
echo "=========================================="
echo "  Benchmark Complete"
echo "=========================================="
echo ""
echo "  Total tests run: ${TOTAL_TESTS}"
echo "  Raw results:     ${RAW_CSV}"
echo "  Summary:         ${SUMMARY_CSV}"
echo ""
echo "  Quick view (last config):"
column -t -s',' "$SUMMARY_CSV" 2>/dev/null | tail -20
echo ""
echo "=========================================="
