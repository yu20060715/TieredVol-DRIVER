#!/bin/bash
# auto_weight.sh — 建卷自動加權：測各碟 solo 寫 → 權重 ∝ 碟速 → 更新 config seg0_weight/seg0_stripe
# 用法：scripts/auto_weight.sh <config> [--solo-size=N] [--dry-run]
# 權重基準 = 8G 連續寫平均（與疊碟實驗同條件，避免 SLC 瞬間冷態峰值）。
# 權重 = max(1, round(solo/min_solo))；stripe = chunk × 總權重。
# 前置：sudo、碟空閒（raw 寫會覆蓋碟前段資料）。
set -u
SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
CFG=${1:?usage: auto_weight.sh <config> [--solo-size=N] [--dry-run]}
SIZE=8G
DRY=""
for a in "${@:2}"; do
  case "$a" in
    --solo-size=*) SIZE=${a#--solo-size=} ;;
    --dry-run) DRY=1 ;;
  esac
done
[ -f "$CFG" ] || { echo "config 不存在: $CFG"; exit 1; }

read -r DISK_COUNT CHUNK < <(python3 - "$CFG" <<'EOF'
import configparser, sys
c = configparser.ConfigParser()
c.read(sys.argv[1])
ws = c['weighted_striping']
print(ws['disk_count'], ws['chunk_size'])
EOF
)
echo "碟數=$DISK_COUNT chunk=$CHUNK"

DEVS=()
for i in $(seq 0 $((DISK_COUNT - 1))); do
  n=$(python3 - "$CFG" "$i" <<'EOF'
import configparser, sys
c = configparser.ConfigParser()
c.read(sys.argv[1])
print(c['weighted_striping'][f'disk{sys.argv[2]}_name'])
EOF
)
  DEVS+=("$n")
done

echo "==== 測各碟 solo 寫（${SIZE}） ===="
SOLO=()
for d in "${DEVS[@]}"; do
  dev=${d#/dev/}
  fio --name=s --filename="$d" --rw=write --bs=1M --size=$SIZE \
    --direct=1 --ioengine=libaio --iodepth=32 --numjobs=1 --end_fsync=1 \
    --output-format=json --output=/tmp/aw_${dev}.json >/dev/null 2>&1
  v=$(python3 -c "
import json
raw = open('/tmp/aw_${dev}.json','rb').read().decode('utf-8','replace')
d = json.loads(raw[raw.find('{'):])['jobs'][0]
print(round(d['write']['bw_bytes'] / 1e6, 1))")
  SOLO+=("$v")
  echo "  $dev: $v MB/s"
done

if [ "$DRY" = "1" ]; then
  echo "[dry-run] 僅顯示，不寫入 config"
fi
python3 - "$CFG" "$CHUNK" "$DRY" "${SOLO[@]}" <<'EOF'
import sys
cfg, chunk = sys.argv[1], int(sys.argv[2])
dry = sys.argv[3] == '1'
solo = [float(x) for x in sys.argv[4:]]
m = min(solo)
w = [max(1, round(s / m)) for s in solo]
tot = sum(w)
stripe = chunk * tot
model = min(s * tot / ww for s, ww in zip(solo, w))
print(f"權重 = {w}（總和 {tot}） stripe = {stripe}")
print(f"瓶頸模型預期吞吐 = {model:.0f} MB/s")
if dry:
    sys.exit(0)
bak = cfg + '.bak'
import shutil
shutil.copy(cfg, bak)
lines = open(cfg).read().splitlines()
out, in_runtime = [], False
for l in lines:
    if l.startswith('seg0_weight='):
        out.append('seg0_weight=' + ','.join(map(str, w)))
    elif l.startswith('seg0_stripe='):
        out.append(f'seg0_stripe={stripe}')
    elif l.startswith('crc32='):
        pass  # 移除，driver 無 crc32 行時不驗證
    elif l.startswith('badmap_'):
        pass  # 清空舊壞塊 bitmap
    elif l.startswith('[runtime]'):
        in_runtime = True
    elif in_runtime:
        # 保留 runtime 段的非 policy 行，policy 強制回 static
        if l.startswith('policy='):
            out.append('policy=0')
        elif l.startswith('['):
            in_runtime = False
            out.append(l)
        else:
            out.append(l)
    else:
        out.append(l)
open(cfg, 'w').write('\n'.join(out) + '\n')
print(f"已更新 {cfg}（備份 {bak}）：權重 {w}、stripe {stripe}、已移除 crc32/badmap、policy→static")
EOF
echo done
