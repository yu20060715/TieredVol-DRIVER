#!/bin/bash
# msg_probe.sh — P3: dmsetup message / sysfs 專項 probe（重建版 2026-08-13）
# 每個 message 以 timeout 包裹，每步前寫 marker 檔，當機可從 marker 定位。
# 前置：sudo、config 檔（預設 /home/yu/tv_mirs.conf）、module 已 build。
# 可用環境變數覆寫：CFG=...
set -u
SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
REPO=$(dirname "$SCRIPT_DIR")
KO=$REPO/driver/tieredvol.ko
CFG=${CFG:-/home/yu/tv_mirs.conf}
OUT=/home/yu/msg_probe_$(date +%m%d_%H%M).log
MARKER=/home/yu/msg_probe.marker
exec > >(tee "$OUT") 2>&1

PASS=0; FAIL=0
pass(){ echo "  [PASS] $1"; PASS=$((PASS+1)); }
fail(){ echo "  [FAIL] $1"; FAIL=$((FAIL+1)); }

SECT(){ python3 -c "import configparser;c=configparser.ConfigParser();c.read('$1');print(int(c['weighted_striping']['seg0_end'])//512)"; }
dmesg_bad(){ dmesg | grep -ciE "warning|oops|bug|pw_remove MISS|give-up|pending-full|hung_task|rebuild read failed|rebuild write failed"; }
step(){ echo "$1" | tee "$MARKER" >/dev/null; sync; }

MSG(){ timeout 10 dmsetup message tv_m "$1" "$2"; }

dmsetup remove_all 2>/dev/null; true
rmmod tieredvol 2>/dev/null; true
insmod "$KO"
echo "  module loaded"
dmesg -c >/dev/null 2>&1; true
S=$(SECT $CFG)

step "create volume"
echo "0 $S tieredvol $CFG" | dmsetup create tv_m
[ -e /dev/mapper/tv_m ] && pass "建立 tv_m" || fail "建立 tv_m"

echo "==== P3-1: sysfs 讀取（建 volume 後） ===="
step "sysfs: policy"
v=$(cat /sys/kernel/tieredvol/policy 2>&1) && [ -n "$v" ] && pass "policy=$v" || fail "policy=$v"
step "sysfs: status"
v=$(cat /sys/kernel/tieredvol/status 2>&1) && [ -n "$v" ] && pass "status read OK" || fail "status=$v"
step "sysfs: disk_count"
v=$(cat /sys/kernel/tieredvol/disk_count 2>&1) && pass "disk_count=$v" || fail "disk_count=$v"
step "sysfs: loglevel"
v=$(cat /sys/kernel/tieredvol/loglevel 2>&1) && pass "loglevel=$v" || fail "loglevel=$v"

echo "==== P3-2: policy / borrow message ===="
step "msg set_policy static"
MSG 0 "set_policy static" && pass "set_policy" || fail "set_policy"
step "msg set_seg_policy"
MSG 0 "set_seg_policy 0 static" && pass "set_seg_policy" || fail "set_seg_policy"
step "msg show_stats"
MSG 0 "show_stats" && pass "show_stats" || fail "show_stats"
step "msg status"
MSG 0 "status" && pass "status" || fail "status"
step "msg show_borrow"
MSG 0 "show_borrow" && pass "show_borrow" || fail "show_borrow"
step "msg borrow_off"
MSG 0 "borrow_off" && pass "borrow_off" || fail "borrow_off"
step "msg borrow_on"
MSG 0 "borrow_on" && pass "borrow_on" || fail "borrow_on"

echo "==== P3-3: stats / config message ===="
step "msg show_io_stats"
MSG 0 "show_io_stats" && pass "show_io_stats" || fail "show_io_stats"
step "msg show_inflight"
MSG 0 "show_inflight" && pass "show_inflight" || fail "show_inflight"
step "msg reset_io_stats"
MSG 0 "reset_io_stats" && pass "reset_io_stats" || fail "reset_io_stats"
step "msg reset_stats"
MSG 0 "reset_stats" && pass "reset_stats" || fail "reset_stats"
step "msg show_bench"
MSG 0 "show_bench" && pass "show_bench" || fail "show_bench"
step "msg show_log"
MSG 0 "show_log" && pass "show_log" || fail "show_log"
step "msg clear_log"
MSG 0 "clear_log" && pass "clear_log" || fail "clear_log"
step "msg set_loglevel"
MSG 0 "set_loglevel 1" && pass "set_loglevel" || fail "set_loglevel"

echo "==== P3-4: mirror / degraded / badmap message ===="
step "msg show_mirror"
MSG 0 "show_mirror" && pass "show_mirror" || fail "show_mirror"
step "msg set_mirror"
MSG 0 "set_mirror 0 1" && pass "set_mirror" || fail "set_mirror"
step "msg set_error_threshold"
MSG 0 "set_error_threshold 100" && pass "set_error_threshold" || fail "set_error_threshold"
step "msg show_errors"
MSG 0 "show_errors" && pass "show_errors" || fail "show_errors"
step "msg reset_errors"
MSG 0 "reset_errors" && pass "reset_errors" || fail "reset_errors"
step "msg show_degraded"
MSG 0 "show_degraded" && pass "show_degraded" || fail "show_degraded"
step "msg clear_degraded"
MSG 0 "clear_degraded" && pass "clear_degraded" || fail "clear_degraded"
step "msg set_badmap"
MSG 0 "set_badmap 1 0" && pass "set_badmap 1 0" || fail "set_badmap"
step "msg show_badmap"
MSG 0 "show_badmap" && pass "show_badmap" || fail "show_badmap"
step "msg clear_badmap"
MSG 0 "clear_badmap 1 0" && pass "clear_badmap" || fail "clear_badmap"

echo "==== P3-5: rebuild（小範圍 start + stop 中止） ===="
step "msg start_rebuild seg0 2G (running ~4.5s)"
MSG 0 "start_rebuild 0 2147483648" && pass "start_rebuild" || fail "start_rebuild"
sleep 2
step "msg show_rebuild"
MSG 0 "show_rebuild" && pass "show_rebuild" || fail "show_rebuild"
step "msg start_rebuild (running 中應 EBUSY)"
MSG 0 "start_rebuild 0 8388608" && fail "running 中 start_rebuild 應被拒" || pass "running 中 start_rebuild 被拒"
step "msg stop_rebuild"
MSG 0 "stop_rebuild" && pass "stop_rebuild" || fail "stop_rebuild"
sleep 1
step "msg show_rebuild (idle)"
MSG 0 "show_rebuild" && pass "show_rebuild idle" || fail "show_rebuild 未 idle"
step "msg rebuild_badmap"
MSG 0 "set_badmap 1 2" && MSG 0 "rebuild_badmap" && pass "rebuild_badmap" || fail "rebuild_badmap"
MSG 0 "clear_badmap 1 2" >/dev/null 2>&1

echo "==== P3-6: 完整小範圍 rebuild 完成 ===="
step "msg start_rebuild seg0 32M full"
MSG 0 "start_rebuild 0 33554432" && pass "start_rebuild full" || fail "start_rebuild full"
for i in $(seq 1 60); do
	sleep 1
	dmesg | grep -q "rebuild seg0 complete" && break
done
dmesg | grep -q "rebuild seg0 complete" && pass "rebuild 完成回到 idle" || fail "rebuild 未完成"
echo "  dmesg bad=$(dmesg_bad)"

dmsetup remove tv_m && pass "remove tv_m OK" || fail "remove tv_m"
rmmod tieredvol && pass "rmmod OK" || fail "rmmod"
echo "==== RESULT: PASS=$PASS FAIL=$FAIL ===="
[ "$FAIL" = "0" ]
