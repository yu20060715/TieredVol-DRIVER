#!/bin/bash
# final_s4.sh — 定案 S4（B@x4+D，tv_s4.conf 20G）之 P1 排水態量測 + F12 衰減曲線
# 功能：
#   1) F12：排水階段逐 1 GB 記錄瞬時吞吐（CSV: 累計GB, MB/s），橫軸累計寫入量、
#      縱軸瞬時吞吐，直到 TLC 穩態（每 GB 記錄點即為圖 F12 的資料源）。
#   2) P1 主表量測：dmsetup remove+重建歸零計數 → 寫 16G（數據源）→ status 驗分布 → 讀 16G。
# 前置：sudo；module tieredvol 已建置；tv_s4.conf 以 by-id 指定碟（B@x4 定案拓撲）。
# 可用環境變數覆寫：CFG=/path/tv_s4.conf  VOL=tv_s4  DRAIN_GB=64  CHUNK_GB=1
# 量測陷阱（附錄 A.7）：不 pkill -9（AHCI wedge）；建卷後先確認 /dev/mapper/$VOL 存在
#   （fio 對不存在裝置會建普通檔 → 假讀/假 ENOSPC）。
set -u
SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
REPO=$(dirname "$SCRIPT_DIR")
KO=$REPO/driver/tieredvol.ko
CFG=${CFG:-/home/yu/tv_s4.conf}
VOL=${VOL:-tv_s4}
DRAIN_GB=${DRAIN_GB:-64}
CHUNK_GB=${CHUNK_GB:-1}
OUT=/home/yu/final_s4_$(date +%m%d_%H%M).log
CSV=/home/yu/f12_decay_$(date +%m%d_%H%M).csv
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

S=$(SECT "$CFG")
CHUNK_B=$((CHUNK_GB*1024*1024*1024))
VOL_B=$((S*512))

echo "==== final_s4: CFG=$CFG VOL=$VOL (${VOL_B} bytes) DRAIN=${DRAIN_GB}G ----"
dmsetup remove_all 2>/dev/null; true
rmmod tieredvol 2>/dev/null; true
insmod "$KO" || { echo "[FAIL] insmod"; exit 1; }
echo "0 $S tieredvol $CFG" | dmsetup create "$VOL" || { echo "[FAIL] create $VOL"; exit 1; }
[ -e /dev/mapper/$VOL ] || { echo "[FAIL] /dev/mapper/$VOL 不存在（fio 會建普通檔，停止）"; exit 1; }

echo "==== 1) F12 排水衰減（每 ${CHUNK_GB}G 一記錄點，累計 ${DRAIN_GB}G）===="
echo "# GB, MB/s（累計寫入量, 瞬時吞吐；排水階段）" > "$CSV"
cum=0 pass=0 stable=0 prev=0
while [ "$cum" -lt "$DRAIN_GB" ]; do
  off=$(( (pass * CHUNK_B) % VOL_B ))
  fio --name=drain --filename=/dev/mapper/$VOL --rw=write --bs=1M --size=${CHUNK_GB}G \
      --offset=$off --direct=1 --ioengine=libaio --iodepth=32 --numjobs=1 --end_fsync=1 \
      --output-format=json --output=/tmp/f12.json >/dev/null 2>&1
  MB=$(fio_bw /tmp/f12.json)
  mbps=${MB%% *}; err=${MB##* }
  [ "$err" != "0" ] && { echo "[FAIL] drain chunk $cum 寫入 err=$err"; exit 1; }
  cum=$((cum+CHUNK_GB))
  echo "$cum, $mbps" >> "$CSV"
  echo "  累計 ${cum}G: ${mbps} MB/s"
  # 早停：連續 6 點皆在 ±5% 帶內視為 TLC 穩態
  d=$(echo "$mbps $prev" | awk '{d=$1-$2; if(d<0)d=-d; if($2>0)printf "%.4f", d/$2; else print 9}')
  if [ "$prev" != "0" ] && [ "$(echo "$d < 0.05" | bc -l)" = "1" ]; then stable=$((stable+1)); else stable=0; fi
  prev=$mbps
  [ "$stable" -ge 6 ] && { echo "  已達 TLC 穩態（連續 6 點 ±5%），提前結束排水"; break; }
  pass=$((pass+1))
done

echo "==== 2) P1 主表量測（remove+重建歸零計數 → 寫 16G）===="
dmsetup remove "$VOL"
echo "0 $S tieredvol $CFG" | dmsetup create "$VOL"
[ -e /dev/mapper/$VOL ] || { echo "[FAIL] 重建 $VOL 失敗"; exit 1; }
fio --name=meas --filename=/dev/mapper/$VOL --rw=write --bs=1M --size=16G \
    --direct=1 --ioengine=libaio --iodepth=32 --numjobs=1 --end_fsync=1 \
    --output-format=json --output=/tmp/final_w.json >/dev/null 2>&1
echo "  寫 16G: $(fio_bw /tmp/final_w.json) MB/s"
echo "  -- dmsetup status（驗分布）--"
dmsetup status "$VOL"
echo "==== 3) 讀 16G ===="
fio --name=read --filename=/dev/mapper/$VOL --rw=read --bs=1M --size=16G \
    --direct=1 --ioengine=libaio --iodepth=32 --numjobs=1 \
    --output-format=json --output=/tmp/final_r.json >/dev/null 2>&1
echo "  讀 16G: $(fio_bw /tmp/final_r.json) MB/s"
dmsetup remove "$VOL" 2>/dev/null; true
echo "done: $OUT"
echo "F12 CSV: $CSV"
