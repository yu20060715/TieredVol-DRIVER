#!/bin/bash
# install_boot.sh — 開機持久：自動載入 tieredvol module + 自動重建卷。
#
# 交付內容：
#   1. /etc/modules-load.d/tieredvol.conf   （開機載入 tieredvol.ko）
#   2. /etc/systemd/system/tieredvol.service（oneshot：開機 create、關機 remove）
#   3. /usr/local/sbin/tieredvol-boot       （helper：loop /etc/tieredvol/*.conf）
#   4. /etc/tieredvol/*.conf                （active configs，from repo configs/）
#
# 限制（務必了解）：
#   - 碟位綁定靠 /dev/disk/by-id（serial），重啟/換槽不變。
#   - 借出表 .borrow 只在「正常 dmsetup remove / 關機 ExecStop」時寫回磁碟；
#     直接斷電時已借出資料仍在、但映射表未存，開機後該區退回未借出狀態。
#   - 本機測試卷，未掛載於 root；若把 root/資料卷放在 tieredvol 上請自行斟酌順序。
#
# 用法：
#   sudo scripts/install_boot.sh          # 安裝 + enable
#   sudo scripts/install_boot.sh --uninstall

set -euo pipefail

CONF_DIR=/etc/tieredvol
UNIT=/etc/systemd/system/tieredvol.service
HELPER=/usr/local/sbin/tieredvol-boot
MODCONF=/etc/modules-load.d/tieredvol.conf
SRC_CONF="$(cd "$(dirname "$0")/../configs" && pwd)"   # repo configs/

do_uninstall=0
for a in "$@"; do
    [ "$a" = "--uninstall" ] && do_uninstall=1
done

if [ $do_uninstall -eq 1 ]; then
    systemctl disable --now tieredvol.service 2>/dev/null || true
    rm -f "$UNIT" "$MODCONF" "$HELPER"
    systemctl daemon-reload
    echo "uninstalled: $UNIT $MODCONF $HELPER（$CONF_DIR 保留）"
    exit 0
fi

[ -d "$SRC_CONF" ] || { echo "找不到 repo configs/：$SRC_CONF"; exit 1; }

# 1) modules-load.d
mkdir -p /etc/modules-load.d
printf 'tieredvol\n' > "$MODCONF"
echo "[1/4] $MODCONF <- tieredvol"

# 2) helper
cat > "$HELPER" <<'HELP'
#!/bin/sh
# tieredvol-boot create|remove — 依 /etc/tieredvol/*.conf 建/刪卷（開機持久用）。
CONF_DIR=/etc/tieredvol
WAIT_TRIES=30

_vol_name() { basename "$1" .conf; }

_vol_size() {
    python3 - "$1" <<'EOF'
import sys, configparser
c = configparser.ConfigParser()
c.read(sys.argv[1])
sec = c['weighted_striping']
end = max(int(sec[k]) for k in sec if k.startswith('seg') and k.endswith('_end'))
print(end // 512)
EOF
}

_wait_disks() {
    i=0
    while [ $i -lt $WAIT_TRIES ]; do
        missing=0
        for cfg in "$CONF_DIR"/*.conf; do
            [ -e "$cfg" ] || continue
            for n in $(sed -n 's/^disk[0-9]*_name=//p' "$cfg"); do
                [ -e "$n" ] || missing=1
            done
        done
        [ $missing -eq 0 ] && return 0
        i=$((i + 1))
        sleep 1
    done
    echo "tieredvol-boot: by-id 碟 30s 內未就緒（仍嘗試建卷）" >&2
    return 1
}

case "$1" in
create)
    modprobe tieredvol 2>/dev/null || true
    command -v udevadm >/dev/null && udevadm settle --timeout=30 2>/dev/null
    _wait_disks || true
    for cfg in "$CONF_DIR"/*.conf; do
        [ -e "$cfg" ] || continue
        name=$(_vol_name "$cfg")
        [ -e "/dev/mapper/$name" ] && { echo "tieredvol-boot: $name 已存在，略過"; continue; }
        S=$(_vol_size "$cfg")
        if echo "0 $S tieredvol $cfg" | dmsetup create "$name" 2>/dev/null; then
            echo "tieredvol-boot: created $name ($S sectors)"
        else
            echo "tieredvol-boot: FAILED $name" >&2
        fi
    done
    ;;
remove)
    for cfg in "$CONF_DIR"/*.conf; do
        [ -e "$cfg" ] || continue
        name=$(_vol_name "$cfg")
        dmsetup remove "$name" 2>/dev/null \
            && echo "tieredvol-boot: removed $name"
    done
    ;;
*)
    echo "usage: $0 create|remove" >&2
    exit 2
    ;;
esac
HELP
chmod 0755 "$HELPER"
echo "[2/4] $HELPER"

# 3) systemd unit（oneshot；開機 create、關機 remove → 觸發 .borrow save）
cat > "$UNIT" <<UNIT
[Unit]
Description=TieredVol volume auto-create (boot persistence)
Documentation=file://$HELPER
After=systemd-modules-load.service blockdev.target
Wants=systemd-modules-load.service

[Service]
Type=oneshot
RemainAfterExit=yes
ExecStart=$HELPER create
ExecStop=$HELPER remove

[Install]
WantedBy=multi-user.target
UNIT
systemctl daemon-reload
systemctl enable tieredvol.service >/dev/null 2>&1
echo "[3/4] $UNIT (enabled)"

# 4) active configs（保留既有 *.borrow，砍掉非 active 舊 *.conf）
mkdir -p "$CONF_DIR"
find "$CONF_DIR" -maxdepth 1 -name '*.conf' -delete
cp "$SRC_CONF"/*.conf "$CONF_DIR"/
echo "[4/4] $CONF_DIR/ <- $SRC_CONF/*.conf"
ls "$CONF_DIR"/*.conf

echo ""
echo "安裝完成。測試：systemctl start tieredvol.service  （重啟後自動生效）"
echo "查看：journalctl -u tieredvol.service -b"
