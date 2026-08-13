#!/bin/bash
# borrow_verify.sh — 實機回歸：weight-borrowing 端到端驗證（2026-08-13）
# 流程：建卷 → write+verify → 檢查借出 → read verify → borrow_off 後仍一致 →
#       移除存 .borrow → 重建載入 → verify_only 重讀 → dmesg 潔淨檢查
# 前置：sudo、config（預設 /home/yu/tv_s4_borrow.conf，需 borrow_enable=1）。
# 覆寫：CFG=... SIZE=... (byte)
set -u
SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
REPO=$(dirname "$SCRIPT_DIR")
KO=$REPO/driver/tieredvol.ko
CFG=${CFG:-/home/yu/tv_s4_borrow.conf}
SIZE=${SIZE:-268435456}  # 256 MB 預設（64M 順序寫不足以觸發借出）
DEV=tv_bv
LOG=/home/yu/borrow_verify_$(date +%m%d_%H%M).log
exec > >(tee "$LOG") 2>&1

PASS=0; FAIL=0
pass(){ echo "  [PASS] $1"; PASS=$((PASS+1)); }
fail(){ echo "  [FAIL] $1"; FAIL=$((FAIL+1)); }
SECT(){ python3 -c "import configparser;c=configparser.ConfigParser();c.read('$1');print(int(c['weighted_striping']['seg0_end'])//512)"; }
dmesg_bad(){ dmesg | grep -ciE "warning|oops|bug|hung_task|pw_remove MISS|pending-full|borrow.*fail"; }

echo "==== borrow_verify: CFG=$CFG SIZE=$SIZE ===="
dmsetup remove $DEV 2>/dev/null; true
rmmod tieredvol 2>/dev/null; true
rm -f "$CFG.borrow"
insmod "$KO" || { echo "insmod failed"; exit 1; }
dmesg -c >/dev/null 2>&1; true
S=$(SECT "$CFG")

echo "== 1. 建立卷 =="
echo "0 $S tieredvol $CFG" | dmsetup create $DEV
[ -e /dev/mapper/$DEV ] && pass "create $DEV" || fail "create $DEV"

echo "== 2. write + verify =="
fio --name=w --filename=/dev/mapper/$DEV --rw=write --bs=1M --size=$SIZE \
    --direct=1 --ioengine=libaio --iodepth=32 --verify=crc32c \
    --output-format=json --output=/tmp/bv_write.json >/dev/null 2>&1
[ $? -eq 0 ] && pass "write rc=0" || fail "write 失敗"
STAT=$(dmsetup status $DEV)
echo "  status: $STAT"
N_BORROWED=$(echo "$STAT" | grep -oP 'borrow=[01]/\K[0-9]+' | head -1)
N_BORROWED=${N_BORROWED:-0}
if [ -n "$N_BORROWED" ] && [ "$N_BORROWED" -gt 0 ]; then
	pass "borrow 已觸發 (n_borrowed=$N_BORROWED)"
else
	fail "borrow 未觸發 (status=$STAT)"
fi

echo "== 3. read verify（借出資料正確） =="
fio --name=r --filename=/dev/mapper/$DEV --rw=read --bs=1M --size=$SIZE \
    --direct=1 --ioengine=libaio --iodepth=32 --verify=crc32c --verify_only \
    --output-format=json --output=/tmp/bv_read.json >/dev/null 2>&1
[ $? -eq 0 ] && pass "read verify rc=0" || fail "read verify 失敗"

echo "== 4. borrow_off 後已借 block 仍一致 =="
dmsetup message $DEV 0 borrow_off >/dev/null
dmesg | grep -qi "borrow disabled" && pass "borrow_off 生效" || fail "borrow_off 未生效"
fio --name=ro --filename=/dev/mapper/$DEV --rw=read --bs=1M --size=$SIZE \
    --direct=1 --ioengine=libaio --iodepth=32 --verify=crc32c --verify_only \
    --output-format=json --output=/tmp/bv_readoff.json >/dev/null 2>&1
[ $? -eq 0 ] && pass "read after borrow_off rc=0" || fail "read after borrow_off 失敗"
BAD=$(dmesg | grep -ciE "warning|oops|bug|hung_task|pw_remove MISS|pending-full|borrow.*fail")
[ "$BAD" -eq 0 ] && pass "write/borrow 期間 dmesg 潔淨" || fail "write/borrow 期間 dmesg 有 bad (bad=$BAD)"
dmesg -c >/dev/null 2>&1
dmsetup message $DEV 0 borrow_on >/dev/null
dmesg | grep -qi "borrow enabled" && pass "borrow_on 生效" || fail "borrow_on 未生效"

echo "== 5. 移除 → 存 .borrow =="
dmesg -c >/dev/null 2>&1
dmsetup remove $DEV
dmesg | grep -qi "borrow table saved" && pass ".borrow 存檔訊息" || fail ".borrow 存檔訊息缺"
[ -f "$CFG.borrow" ] && pass ".borrow 已存 ($(stat -c%s $CFG.borrow) B)" || fail ".borrow 未存"
if [ -f "$CFG.borrow" ]; then
	BORROW_ENTRIES=$(( $(stat -c%s "$CFG.borrow") / 16 ))
	[ "$BORROW_ENTRIES" -gt 0 ] && pass ".borrow 含 entries (full-table $BORROW_ENTRIES entries)" \
		|| fail ".borrow entries 檢查失敗"
else
	fail ".borrow entries 檢查失敗（檔不存在）"
fi

echo "== 6. 重建 → load → verify_only =="
echo "0 $S tieredvol $CFG" | dmsetup create $DEV
[ -e /dev/mapper/$DEV ] && pass "recreate $DEV" || fail "recreate $DEV"
fio --name=rl --filename=/dev/mapper/$DEV --rw=read --bs=1M --size=$SIZE \
    --direct=1 --ioengine=libaio --iodepth=32 --verify=crc32c --verify_only \
    --output-format=json --output=/tmp/bv_reload.json >/dev/null 2>&1
[ $? -eq 0 ] && pass "reload verify rc=0" || fail "reload verify 失敗"

dmesg | grep -qi "borrow table loaded" && pass "borrow table loaded 訊息" || \
	( [ "$SIZE" -lt 1048576 ] && pass "小卷可能無 borrow（略）" || fail "borrow table loaded 訊息缺")
BAD=$(dmesg_bad)
[ "$BAD" -eq 0 ] && pass "dmesg 潔淨 (bad=0)" || fail "dmesg 有 bad 記號 (bad=$BAD)"

dmsetup remove $DEV 2>/dev/null; true
rmmod tieredvol 2>/dev/null; true
echo "==== RESULT: PASS=$PASS FAIL=$FAIL ===="
[ "$FAIL" = "0" ]
