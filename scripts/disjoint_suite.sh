#!/bin/bash
# disjoint_suite.sh — P4: 不共用碟併發判據（tv_l=A+B vs tv_r=C+D，無共享碟）
# 預期：併發和 ≈ 孤立和（±10%）→ 證明 driver 本身無多卷併發開銷，先前 68% 純為碟 A 爭用。
# 前置：sudo、config /home/yu/tv_s2.conf + /home/yu/tv_r.conf、module 已 build。
# 可用環境變數覆寫：CFGL=... CFGR=...
set -u
SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
REPO=$(dirname "$SCRIPT_DIR")
KO=$REPO/driver/tieredvol.ko
CFGL=${CFGL:-/home/yu/tv_s2.conf}
CFGR=${CFGR:-/home/yu/tv_r.conf}
OUT=/home/yu/disjoint_$(date +%m%d_%H%M).log
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

dmsetup remove_all 2>/dev/null; true
rmmod tieredvol 2>/dev/null; true
insmod "$KO"
echo "  module loaded"
dmesg -c >/dev/null 2>&1; true

SL=$(SECT "$CFGL"); SR=$(SECT "$CFGR")
echo "0 $SL tieredvol $CFGL" | dmsetup create tv_l
echo "0 $SR tieredvol $CFGR" | dmsetup create tv_r
[ -e /dev/mapper/tv_l ] && [ -e /dev/mapper/tv_r ] && pass "建立 tv_l(A+B)+tv_r(C+D)" || fail "建立卷"

echo "==== A. 寫入（孤立 vs 真併發） ===="
echo "-- A1 孤立 tv_l(A+B) --"; run wl tv_l write /tmp/dj_a1.json; A1=$(fio_bw /tmp/dj_a1.json); echo "  tv_l alone: ${A1}"
echo "-- A2 孤立 tv_r(C+D) --"; run wr tv_r write /tmp/dj_a2.json; A2=$(fio_bw /tmp/dj_a2.json); echo "  tv_r alone: ${A2}"
echo "-- A3 併發寫 --"
run wlc tv_l write /tmp/dj_a3l.json & PIDL=$!
run wrc tv_r write /tmp/dj_a3r.json & PIDR=$!
wait $PIDL; wait $PIDR
A3L=$(fio_bw /tmp/dj_a3l.json); A3R=$(fio_bw /tmp/dj_a3r.json)
echo "  concurrent: tv_l=${A3L%% *} + tv_r=${A3R%% *}"
echo "  $(python3 -c "a1=float('${A1%% *}');a2=float('${A2%% *}');a3=float('${A3L%% *}')+float('${A3R%% *}');print(f'孤立和={a1+a2:.1f} 併發和={a3:.1f} 代價={(a1+a2-a3)/(a1+a2)*100:.1f}%')")"

echo "==== B. 讀取（孤立 vs 真併發） ===="
echo "-- B1 孤立 tv_l 讀 --"; run rl tv_l read /tmp/dj_b1.json; B1=$(fio_bw /tmp/dj_b1.json); echo "  tv_l alone: ${B1}"
echo "-- B2 孤立 tv_r 讀 --"; run rr tv_r read /tmp/dj_b2.json; B2=$(fio_bw /tmp/dj_b2.json); echo "  tv_r alone: ${B2}"
echo "-- B3 併發讀 --"
run rlc tv_l read /tmp/dj_b3l.json & PIDL=$!
run rrc tv_r read /tmp/dj_b3r.json & PIDR=$!
wait $PIDL; wait $PIDR
B3L=$(fio_bw /tmp/dj_b3l.json); B3R=$(fio_bw /tmp/dj_b3r.json)
echo "  concurrent: tv_l=${B3L%% *} + tv_r=${B3R%% *}"
echo "  $(python3 -c "b1=float('${B1%% *}');b2=float('${B2%% *}');b3=float('${B3L%% *}')+float('${B3R%% *}');print(f'孤立和={b1+b2:.1f} 併發和={b3:.1f} 代價={(b1+b2-b3)/(b1+b2)*100:.1f}%')")"

echo "==== 檢查 ===="
E=${A1##* }${A2##* }${A3L##* }${A3R##* }${B1##* }${B2##* }${B3L##* }${B3R##* }
BAD=$(dmesg_bad)
[ "$E" = "00000000" ] && pass "全部 fio err=0" || fail "fio err 非零: $E"
[ "$BAD" = "0" ] && pass "dmesg 無 bad" || fail "dmesg bad=$BAD"
echo "  dmesg bad=$BAD"

dmsetup remove tv_l && pass "remove tv_l" || fail "remove tv_l"
dmsetup remove tv_r && pass "remove tv_r" || fail "remove tv_r"
rmmod tieredvol && pass "rmmod" || fail "rmmod"
echo "==== RESULT: PASS=$PASS FAIL=$FAIL ===="
[ "$FAIL" = "0" ]
