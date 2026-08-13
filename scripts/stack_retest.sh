#!/bin/bash
# stack_retest.sh — 疊碟冷態回歸：raw solo 基線 + S1→S4 加權疊碟 + 權重受限模型驗算
# 前置：sudo、config（預設 /home/yu/tv_s{1,2,3,4}.conf）、module 已 build。
# 權重受限模型 = 當日 nvme1n1 solo 寫 × 總權重/6（快碟權重 6，鎖死資料比例 → 快碟瓶頸）。
# 環境變數覆寫：SOLO_DEVS SOLO_RW SOLO_SIZE CFG1..CFG4。
set -u
SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
REPO=$(dirname "$SCRIPT_DIR")
KO=$REPO/driver/tieredvol.ko
CFG1=${CFG1:-/home/yu/tv_s1.conf}
CFG2=${CFG2:-/home/yu/tv_s2.conf}
CFG3=${CFG3:-/home/yu/tv_s3.conf}
CFG4=${CFG4:-/home/yu/tv_s4.conf}
OUT=/home/yu/stack_retest_$(date +%m%d_%H%M).log
exec > >(tee "$OUT") 2>&1

SECT(){ python3 -c "import configparser;c=configparser.ConfigParser();c.read('$1');print(int(c['weighted_striping']['seg0_end'])//512)"; }
fio_bw(){ python3 -c "
import json
raw=open('$1','rb').read().decode('utf-8','replace')
i=raw.find('{')
d=json.loads(raw[i:])['jobs'][0]
w=d.get('write',{}).get('bw_bytes',0)
r=d.get('read',{}).get('bw_bytes',0)
e=d.get('error',0)
print(round((w or r)/1e6,1),e)"; }

echo "==== Step 1: raw solo 基線（當日） ===="
SOLO_DEVS=${SOLO_DEVS:-nvme1n1:nvme0n1:sdc:sdb} SOLO_RW=${SOLO_RW:-write:read} SOLO_SIZE=${SOLO_SIZE:-8G} bash "$SCRIPT_DIR/raw_solo.sh"
ASOLO=$(grep -E "nvme1n1 write:" "$OUT" | tail -1 | awk '{print $3}')
echo "  當日 nvme1n1 solo 寫 = ${ASOLO} MB/s（模型基準）"

run_vol(){ # $1=卷名 $2=conf $3=標籤
  local vol=$1 cfg=$2 tag=$3 S W R
  S=$(SECT "$cfg")
  dmsetup remove_all 2>/dev/null; true
  rmmod tieredvol 2>/dev/null; true
  insmod "$KO" || { echo "  [FAIL] insmod"; return 1; }
  echo "0 $S tieredvol $cfg" | dmsetup create "$vol" || { echo "  [FAIL] create $vol"; return 1; }
  echo "---- $tag 寫 8G 1M d32 ----"
  fio --name=w --filename=/dev/mapper/$vol --rw=write --bs=1M --size=8G \
    --direct=1 --ioengine=libaio --iodepth=32 --numjobs=1 --end_fsync=1 \
    --output-format=json --output=/tmp/st_w.json >/dev/null 2>&1
  W=$(fio_bw /tmp/st_w.json); echo "  $tag 寫: ${W} MB/s"
  echo "  -- dmsetup status（寫分布 bytes）--"
  dmsetup status $vol
  echo "---- $tag 讀 8G ----"
  fio --name=r --filename=/dev/mapper/$vol --rw=read --bs=1M --size=8G \
    --direct=1 --ioengine=libaio --iodepth=32 --numjobs=1 \
    --output-format=json --output=/tmp/st_r.json >/dev/null 2>&1
  R=$(fio_bw /tmp/st_r.json); echo "  $tag 讀: ${R} MB/s"
  dmsetup remove "$vol" 2>/dev/null; true
  rmmod tieredvol 2>/dev/null; true
  echo "  $tag 結果 W=${W%% *} R=${R%% *}  err=${W##* }/${R##* }"
}

echo "==== Step 2: 疊碟 S1→S4 ===="
run_vol tv_s1 "$CFG1" "S1(A)"
run_vol tv_s2 "$CFG2" "S2(A+B)"
run_vol tv_s3 "$CFG3" "S3(A+B+C)"
run_vol tv_s4 "$CFG4" "S4(A+B+C+D)"

echo "==== Step 3: 權重受限模型驗算（瓶頸 = min(solo_i × 總權重/weight_i)） ===="
python3 - "$OUT" <<'EOF'
import re, sys
out = sys.argv[1]
lines = open(out).read()
def solo(tag):
    m = re.search(rf"  {tag} write: ([\d.]+)", lines)
    return float(m.group(1)) if m else float('nan')
S = {d: solo(d) for d in ('nvme1n1','nvme0n1','sdc','sdb')}
def wb(tag):
    m = re.search(rf"^  {tag}\(.*\) 結果 W=([\d.]+)", lines, re.M)
    return float(m.group(1)) if m else float('nan')
print("當日 solo 寫: " + " ".join(f"{d}={v:.0f}" for d, v in S.items()))
s1 = wb('S1')
print(f"S1(A): 實測 {s1:.0f} vs A solo {S['nvme1n1']:.0f}  差 {(s1-S['nvme1n1'])/S['nvme1n1']*100:+.1f}%")
combos = [('S2', [('nvme1n1',6),('nvme0n1',1)]),
          ('S3', [('nvme1n1',6),('nvme0n1',1),('sdc',1)]),
          ('S4', [('nvme1n1',6),('nvme0n1',1),('sdc',1),('sdb',1)])]
for tag, pairs in combos:
    act = wb(tag)
    tot = sum(w for _, w in pairs)
    limits = {d: S[d] * tot / w for d, w in pairs}
    bn = min(limits, key=limits.get)
    model = limits[bn]
    print(f"{tag}: 瓶頸模型 {model:.0f}（受 {bn} 限制 {S[bn]:.0f}×{tot}/{dict(pairs)[bn]}) | 實測 {act:.0f} | 差 {(act-model)/model*100:+.1f}%")
EOF
echo "done: $OUT"
