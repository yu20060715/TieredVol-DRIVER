#!/bin/bash
# TieredVol — Restore volumes from saved config after reboot
# Reads /etc/tieredvol/*.conf and rebuilds dm-linear + LVM striped volumes.
#
# Usage: tieredvol-restore.sh [--dry-run]
#   --dry-run   Show what would be done without making changes

set -euo pipefail

CONF_DIR="/etc/tieredvol"
LOG_TAG="tieredvol-restore"
DRY_RUN=0

if [[ "${1:-}" == "--dry-run" ]]; then
    DRY_RUN=1
fi

log() { echo "[$LOG_TAG] $*"; }
err() { echo "[$LOG_TAG] ERROR: $*" >&2; }

run() {
    if [[ $DRY_RUN -eq 1 ]]; then
        log "DRY-RUN: $*"
        return 0
    fi
    "$@"
}

parse_ini() {
    local file="$1" section="" key="" value=""
    while IFS='=' read -r key value || [[ -n "$key" ]]; do
        key=$(echo "$key" | tr -d '[:space:]')
        value=$(echo "$value" | tr -d '[:space:]')
        [[ -z "$key" || "$key" == \#* || "$key" == ";"* ]] && continue
        if [[ "$key" == "["*"]" ]]; then
            section="${key:1:${#key}-2}"
            continue
        fi
        case "$section/$key" in
            general/name)      TV_NAME="$value" ;;
            general/count)     TV_COUNT="$value" ;;
            general/fs)        TV_FS="$value" ;;
            general/stripesize) TV_STRIPE="$value" ;;
            general/mount)     TV_MOUNT="$value" ;;
            general/total_gb)  TV_TOTAL="$value" ;;
            disk.*/*)          eval "TV_DISK_${section#disk.}_${key}=$value" ;;
        esac
    done < "$file"
}

restore_volume() {
    local conf_file="$1"
    local TV_NAME="" TV_COUNT="" TV_FS="ext4" TV_STRIPE="512" TV_MOUNT="" TV_TOTAL=""
    local -a DISK_DEVICE=() DISK_SIZE=()

    parse_ini "$conf_file"

    if [[ -z "$TV_NAME" ]]; then
        err "No volume name in $conf_file, skipping"
        return 1
    fi

    log "Restoring volume: $TV_NAME"

    # Collect disk info
    local i=0
    while [[ $i -lt ${TV_COUNT:-0} ]]; do
        local dev_var="TV_DISK_${i}_device"
        local sz_var="TV_DISK_${i}_size_gb"
        DISK_DEVICE+=("${!dev_var:-}")
        DISK_SIZE+=("${!sz_var:-0}")
        i=$((i + 1))
    done

    if [[ ${#DISK_DEVICE[@]} -lt 2 ]]; then
        err "Volume $TV_NAME needs at least 2 disks, found ${#DISK_DEVICE[@]}, skipping"
        return 1
    fi

    # Check all disks exist
    for dev in "${DISK_DEVICE[@]}"; do
        if [[ ! -b "/dev/$dev" ]]; then
            err "Disk /dev/$dev not found, skipping volume $TV_NAME"
            return 1
        fi
    done

    # Step 1: Clean up old targets
    log "  Step 1: Cleaning up old targets..."
    for dev in "${DISK_DEVICE[@]}"; do
        local target="tv_${dev}_carve"
        run sudo dmsetup remove "$target" 2>/dev/null || true
    done
    local vg_name="tv_vg_${TV_NAME}"
    local lv_name="tv_lv_${TV_NAME}"
    run sudo lvremove -f "${vg_name}/${lv_name}" 2>/dev/null || true
    run sudo vgremove -f "$vg_name" 2>/dev/null || true
    for dev in "${DISK_DEVICE[@]}"; do
        local target="tv_${dev}_carve"
        run sudo pvremove -ff -y "/dev/mapper/${target}" 2>/dev/null || true
    done

    # Step 2: Create dm-linear targets
    log "  Step 2: Creating dm-linear targets..."
    i=0
    while [[ $i -lt ${#DISK_DEVICE[@]} ]]; do
        local dev="${DISK_DEVICE[$i]}"
        local size_gb="${DISK_SIZE[$i]}"
        local target="tv_${dev}_carve"
        local sectors=$(( size_gb * 1024 * 1024 * 1024 / 512 ))

        local table="0 ${sectors} linear /dev/${dev} 0"
        if [[ $DRY_RUN -eq 1 ]]; then
            log "  DRY-RUN: dmsetup create $target (table: $table)"
        else
            echo "$table" | sudo dmsetup create "$target"
        fi
        if [[ $? -ne 0 ]]; then
            err "Failed to create dm target for /dev/$dev"
            return 1
        fi
        log "  Created $target (${size_gb}GB)"
        i=$((i + 1))
    done

    # Step 3: Create PV
    log "  Step 3: Creating LVM physical volumes..."
    for dev in "${DISK_DEVICE[@]}"; do
        local target="tv_${dev}_carve"
        run sudo pvcreate -f "/dev/mapper/${target}"
    done

    # Step 4: Create VG
    log "  Step 4: Creating volume group..."
    local pv_args=()
    for dev in "${DISK_DEVICE[@]}"; do
        pv_args+=("/dev/mapper/tv_${dev}_carve")
    done
    run sudo vgcreate "$vg_name" "${pv_args[@]}"

    # Step 5: Create striped LV
    log "  Step 5: Creating striped logical volume..."
    local stripes="${#DISK_DEVICE[@]}"
    run sudo lvcreate -l 100%FREE -i "$stripes" -I "${TV_STRIPE}k" -n "$lv_name" "$vg_name"

    local lv_path="/dev/mapper/${vg_name}-${lv_name}"

    # Step 6: Format
    if [[ "$TV_FS" != "none" ]]; then
        log "  Step 6: Formatting as $TV_FS..."
        run sudo "mkfs.${TV_FS}" "$lv_path"
    else
        log "  Step 6: Skipped formatting (raw)"
    fi

    # Step 7: Mount
    if [[ -n "$TV_MOUNT" && "$TV_FS" != "none" ]]; then
        log "  Step 7: Mounting at $TV_MOUNT..."
        run sudo mkdir -p "$TV_MOUNT"
        run sudo mount "$lv_path" "$TV_MOUNT"
    else
        log "  Step 7: Skipped mounting"
    fi

    log "  Volume $TV_NAME restored successfully!"
    return 0
}

# Main
if [[ ! -d "$CONF_DIR" ]]; then
    log "No config directory found at $CONF_DIR, nothing to restore"
    exit 0
fi

configs=("$CONF_DIR"/*.conf)
if [[ ! -e "${configs[0]}" ]]; then
    log "No config files found in $CONF_DIR, nothing to restore"
    exit 0
fi

log "Found ${#configs[@]} config(s) to restore"

restored=0
failed=0
for conf in "${configs[@]}"; do
    if restore_volume "$conf"; then
        restored=$((restored + 1))
    else
        failed=$((failed + 1))
    fi
done

log "Restore complete: $restored succeeded, $failed failed"
exit $failed
