#!/bin/bash
# multi_vol_suite.sh — P4: 真併發多卷（兩卷同時 fio）vs 各自隔離，記錄多卷代價
# 前置：sudo、config 檔（預設 /home/yu/tv_s4.conf、tv_s2.conf）、module 已 build。
# 可用環境變數覆寫：CFG4=... CFG2=...
set -u
SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
REPO=$(dirname "$SCRIPT_DIR")
KO=$REPO/driver/tieredvol.ko
CFG4=${CFG4:-/home/yu/tv_s4.conf}
CFG2=${CFG2:-/home/yu/tv_s2.conf}
OUT=/home/yu/multi_vol_$(date +%m%d_%H%M).log
exec > >(tee "$OUT") 2>&1

PASS=0; FAIL=0
pass(){ echo "  [PASS] $1"; PASS=$((PASS+1)); }
fail(){ echo "  [FAIL] $1"; FAIL=$((FAIL+1)); }

SECT(){ python3 -c "import configparser;c=configparser.ConfigParser();c.read('$1');print(int(c['weighted_striping']['seg0_end'])//512)"; }
fio_bw(){ python3 -c "
import json,re
raw=open('$1','rb').read().decode('utf-8','replace')
i=raw.find('{')
d=json.loads(raw[i:])['jobs'][0]
w=d.get('write',{}).get('bw_bytes',0)
r=d.get('read',{}).get('bw_bytes',0)
e=d.get('error',0)
print(round((w or r)/1e6,1),e)"; }
dmesg_bad(){ dmesg | grep -ciE "warning|oops|bug|pw_remove MISS|give-up|pending-full|hung_task"; }

dmsetup remove_all 2>/dev/null; true
rmmod tieredvol 2>/dev/null; true
insmod "$KO"
echo "  module loaded"
dmesg -c >/dev/null 2>&1; true

S4=$(SECT "$CFG4")
S2=$(SECT "$CFG2")
echo "0 $S4 tieredvol /home/yu/tv_s4.conf" | dmsetup create tv_s4
echo "0 $S2 tieredvol /home/yu/tv_s2.conf" | dmsetup create tv_s2
[ -e /dev/mapper/tv_s4 ] && [ -e /dev/mapper/tv_s2 ] && pass "建立 tv_s4 + tv_s2" || fail "建立卷"

echo "==== A. 寫入 1M 順序（隔離 vs 真併發） ===="
echo "-- A1 隔離: tv_s4 8G --"
fio --name=w4 --filename=/dev/mapper/tv_s4 --rw=write --bs=1M --size=8G \
  --direct=1 --ioengine=libaio --iodepth=32 --numjobs=1 --end_fsync=1 \
  --output-format=json --output=/tmp/mv_a1.json >/dev/null 2>&1
A1=$(fio_bw /tmp/mv_a1.json); echo "  tv_s4 alone: ${A1} MB/s"
echo "-- A2 隔離: tv_s2 8G --"
fio --name=w2 --filename=/dev/mapper/tv_s2 --rw=write --bs=1M --size=8G \
  --direct=1 --ioengine=libaio --iodepth=32 --numjobs=1 --end_fsync=1 \
  --output-format=json --output=/tmp/mv_a2.json >/dev/null 2>&1
A2=$(fio_bw /tmp/mv_a2.json); echo "  tv_s2 alone: ${A2} MB/s"

echo "-- A3 真併發: tv_s4 + tv_s2 同時寫 --"
fio --name=w4c --filename=/dev/mapper/tv_s4 --rw=write --bs=1M --size=8G \
  --direct=1 --ioengine=libaio --iodepth=32 --numjobs=1 --end_fsync=1 \
  --output-format=json --output=/tmp/mv_a3_4.json >/dev/null 2>&1 &
PID4=$!
fio --name=w2c --filename=/dev/mapper/tv_s2 --rw=write --bs=1M --size=8G \
  --direct=1 --ioengine=libaio --iodepth=32 --numjobs=1 --end_fsync=1 \
  --output-format=json --output=/tmp/mv_a3_2.json >/dev/null 2>&1 &
PID2=$!
wait $PID4; wait $PID2
A3_4=$(fio_bw /tmp/mv_a3_4.json); A3_2=$(fio_bw /tmp/mv_a3_2.json)
echo "  concurrent: tv_s4=${A3_4%% *} MB/s + tv_s2=${A3_2%% *} MB/s"
SUM_A=$(python3 -c "a1=float('${A1%% *}');a2=float('${A2%% *}');a3=float('${A3_4%% *}')+float('${A3_2%% *}');print(f'孤立和={a1+a2:.1f} 併發和={a3:.1f} 代價={(a1+a2-a3)/(a1+a2)*100:.1f}%')")
echo "  $SUM_A"

echo "==== B. 讀取 1M 順序（隔離 vs 真併發，讀已寫 8G） ===="
echo "-- B1 隔離: tv_s4 讀 --"
fio --name=r4 --filename=/dev/mapper/tv_s4 --rw=read --bs=1M --size=8G \
  --direct=1 --ioengine=libaio --iodepth=32 --numjobs=1 \
  --output-format=json --output=/tmp/mv_b1.json >/dev/null 2>&1
B1=$(fio_bw /tmp/mv_b1.json); echo "  tv_s4 alone: ${B1} MB/s"
echo "-- B2 隔離: tv_s2 讀 --"
fio --name=r2 --filename=/dev/mapper/tv_s2 --rw=read --bs=1M --size=8G \
  --direct=1 --ioengine=libaio --iodepth=32 --numjobs=1 \
  --output-format=json --output=/tmp/mv_b2.json >/dev/null 2>&1
B2=$(fio_bw /tmp/mv_b2.json); echo "  tv_s2 alone: ${B2} MB/s"
echo "-- B3 真併發: tv_s4 + tv_s2 同時讀 --"
fio --name=r4c --filename=/dev/mapper/tv_s4 --rw=read --bs=1M --size=8G \
  --direct=1 --ioengine=libaio --iodepth=32 --numjobs=1 \
  --output-format=json --output=/tmp/mv_b3_4.json >/dev/null 2>&1 &
PID4=$!
fio --name=r2c --filename=/dev/mapper/tv_s2 --rw=read --bs=1M --size=8G \
  --direct=1 --ioengine=libaio --iodepth=32 --numjobs=1 \
  --output-format=json --output=/tmp/mv_b3_2.json >/dev/null 2>&1 &
PID2=$!
wait $PID4; wait $PID2
B3_4=$(fio_bw /tmp/mv_b3_4.json); B3_2=$(fio_bw /tmp/mv_b3_2.json)
echo "  concurrent: tv_s4=${B3_4%% *} MB/s + tv_s2=${B3_2%% *} MB/s"
SUM_B=$(python3 -c "b1=float('${B1%% *}');b2=float('${B2%% *}');b3=float('${B3_4%% *}')+float('${B3_2%% *}');print(f'孤立和={b1+b2:.1f} 併發和={b3:.1f} 代價={(b1+b2-b3)/(b1+b2)*100:.1f}%')")
echo "  $SUM_B"

echo "==== 檢查 ===="
A1E=${A1##* }; A2E=${A2##* }; A3_4E=${A3_4##* }; A3_2E=${A3_2##* }
B1E=${B1##* }; B2E=${B2##* }; B3_4E=${B3_4##* }; B3_2E=${B3_2##* }
BAD=$(dmesg_bad)
[ "$A1E$A2E$A3_4E$A3_2E$B1E$B2E$B3_4E$B3_2E" = "00000000" ] && pass "全部 fio err=0" || fail "fio err 非零: $A1E/$A2E/$A3_4E/$A3_2E/$B1E/$B2E/$B3_4E/$B3_2E"
[ "$BAD" = "0" ] && pass "dmesg 無 bad (warning/oops/miss/give-up)" || fail "dmesg bad=$BAD"
echo "  dmesg bad=$BAD"

echo "---- dmsetup status ----"
dmsetup status tv_s4
dmsetup status tv_s2

dmsetup remove tv_s4 && pass "remove tv_s4" || fail "remove tv_s4"
dmsetup remove tv_s2 && pass "remove tv_s2" || fail "remove tv_s2"
rmmod tieredvol && pass "rmmod" || fail "rmmod"
echo "==== RESULT: PASS=$PASS FAIL=$FAIL ===="
[ "$FAIL" = "0" ]
