#!/bin/bash
# shared_ctl.sh — P4: 共享碟對照組（驗「共享碟即瓶頸」模型）
#   部分 C：完全同碟 (A+B)+(A+B)        → 預期併發和 ≈ A+B solo ≈2398
#   部分 X：不對稱 (A solo)+(A+B+C+D)   → 預期併發和 ≈ A solo ≈2009
# 前置：sudo、config /home/yu/tv_s1/tv_s2/tv_s4.conf、module 已 build。
# 可用環境變數覆寫：CFG1=... CFG2=... CFG4=...
set -u
SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
REPO=$(dirname "$SCRIPT_DIR")
KO=$REPO/driver/tieredvol.ko
CFG1=${CFG1:-/home/yu/tv_s1.conf}
CFG2=${CFG2:-/home/yu/tv_s2.conf}
CFG4=${CFG4:-/home/yu/tv_s4.conf}
OUT=/home/yu/shared_ctl_$(date +%m%d_%H%M).log
exec > >(tee "$OUT") 2>&1

PASS=0; FAIL=0
pass(){ echo "  [PASS] $1"; PASS=$((PASS+1)); }
fail(){ echo "  [FAIL] $1"; FAIL=$((FAIL+1)); }

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
dmesg_bad(){ dmesg | grep -ciE "warning|oops|bug|pw_remove MISS|give-up|pending-full|hung_task"; }
run(){ local n=$1 v=$2 rw=$3 out=$4; local fsync=""; [ "$rw" = write ] && fsync="--end_fsync=1"
  fio --name=$n --filename=/dev/mapper/$v --rw=$rw --bs=1M --size=8G \
    --direct=1 --ioengine=libaio --iodepth=32 --numjobs=1 $fsync \
    --output-format=json --output=$out >/dev/null 2>&1; }

pair(){ local tag=$1 v1=$2 v2=$3 label=$4
  echo "==== $tag: $label ===="
  echo "-- 孤立 $v1 寫 --"; run ${tag}1 $v1 write /tmp/sh_${tag}_1w.json; A1=$(fio_bw /tmp/sh_${tag}_1w.json); echo "  $v1 alone: ${A1}"
  echo "-- 孤立 $v2 寫 --"; run ${tag}2 $v2 write /tmp/sh_${tag}_2w.json; A2=$(fio_bw /tmp/sh_${tag}_2w.json); echo "  $v2 alone: ${A2}"
  echo "-- 併發寫 --"
  run ${tag}1c $v1 write /tmp/sh_${tag}_c1w.json & P1=$!
  run ${tag}2c $v2 write /tmp/sh_${tag}_c2w.json & P2=$!
  wait $P1; wait $P2
  C1=$(fio_bw /tmp/sh_${tag}_c1w.json); C2=$(fio_bw /tmp/sh_${tag}_c2w.json)
  echo "  concurrent: $v1=${C1%% *} + $v2=${C2%% *}"
  echo "  $(python3 -c "a1=float('${A1%% *}');a2=float('${A2%% *}');c=float('${C1%% *}')+float('${C2%% *}');print(f'寫: 孤立和={a1+a2:.1f} 併發和={c:.1f} 代價={(a1+a2-c)/(a1+a2)*100:.1f}%')")"
  echo "-- 孤立 $v1 讀 --"; run ${tag}1 $v1 read /tmp/sh_${tag}_1r.json; B1=$(fio_bw /tmp/sh_${tag}_1r.json); echo "  $v1 alone: ${B1}"
  echo "-- 孤立 $v2 讀 --"; run ${tag}2 $v2 read /tmp/sh_${tag}_2r.json; B2=$(fio_bw /tmp/sh_${tag}_2r.json); echo "  $v2 alone: ${B2}"
  echo "-- 併發讀 --"
  run ${tag}1c $v1 read /tmp/sh_${tag}_c1r.json & P1=$!
  run ${tag}2c $v2 read /tmp/sh_${tag}_c2r.json & P2=$!
  wait $P1; wait $P2
  D1=$(fio_bw /tmp/sh_${tag}_c1r.json); D2=$(fio_bw /tmp/sh_${tag}_c2r.json)
  echo "  concurrent: $v1=${D1%% *} + $v2=${D2%% *}"
  echo "  $(python3 -c "b1=float('${B1%% *}');b2=float('${B2%% *}');d=float('${D1%% *}')+float('${D2%% *}');print(f'讀: 孤立和={b1+b2:.1f} 併發和={d:.1f} 代價={(b1+b2-d)/(b1+b2)*100:.1f}%')")"
}

dmsetup remove_all 2>/dev/null; true
rmmod tieredvol 2>/dev/null; true
insmod "$KO"
echo "  module loaded"
dmesg -c >/dev/null 2>&1; true

if [ "${SKIP_C:-0}" != "1" ]; then
echo "==== 建卷（完全同碟對照） ===="
SC=$(SECT "$CFG2")
echo "0 $SC tieredvol $CFG2" | dmsetup create tv_c1
echo "0 $SC tieredvol $CFG2" | dmsetup create tv_c2
[ -e /dev/mapper/tv_c1 ] && [ -e /dev/mapper/tv_c2 ] && pass "tv_c1+tv_c2 (A+B) 建立" || fail "完全同碟建卷"
pair C tv_c1 tv_c2 "完全同碟 (A+B)+(A+B)"
dmsetup remove tv_c1; dmsetup remove tv_c2
fi

echo "==== 建卷（不對稱共用對照） ===="
S1=$(SECT "$CFG1"); S4=$(SECT "$CFG4")
echo "0 $S1 tieredvol $CFG1" | dmsetup create tv_x1
echo "0 $S4 tieredvol $CFG4" | dmsetup create tv_x2
[ -e /dev/mapper/tv_x1 ] && [ -e /dev/mapper/tv_x2 ] && pass "tv_x1(A)+tv_x2(S4) 建立" || fail "不對稱建卷"
pair X tv_x1 tv_x2 "不對稱 (A solo)+(A+B+C+D)"
dmsetup remove tv_x1; dmsetup remove tv_x2

echo "==== 檢查 ===="
BAD=$(dmesg_bad)
[ "$BAD" = "0" ] && pass "dmesg 無 bad" || fail "dmesg bad=$BAD"
echo "  dmesg bad=$BAD"
rmmod tieredvol && pass "rmmod" || fail "rmmod"
echo "==== RESULT: PASS=$PASS FAIL=$FAIL ===="
[ "$FAIL" = "0" ]
