#!/bin/bash
# rebuild_min.sh — 最小重現 rebuild_badmap 當機（分步 marker）
# 前置：sudo、config 檔、module 已 build（make）。tieredvol.ko 取自已 build 的 driver/。
# 可用環境變數覆寫：CFG=... （預設 /home/yu/tv_mirs.conf）
set -u
SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
REPO=$(dirname "$SCRIPT_DIR")
KO=$REPO/driver/tieredvol.ko
CFG=${CFG:-/home/yu/tv_mirs.conf}
OUT=/home/yu/rebuild_min_$(date +%m%d_%H%M).log
MARKER=/home/yu/rebuild_min.marker
exec > >(tee "$OUT") 2>&1
step(){ echo "$1" | tee "$MARKER" >/dev/null; sync; }

S=$(python3 -c "import configparser;c=configparser.ConfigParser();c.read('$CFG');print(int(c['weighted_striping']['seg0_end'])//512)")

step "rmmod+insmod"
rmmod tieredvol 2>/dev/null; true
insmod "$KO"
dmesg -c >/dev/null 2>&1; true

step "create tv_m"
echo "0 $S tieredvol $CFG" | dmsetup create tv_m

step "set_badmap 1 2"
dmsetup message tv_m 0 "set_badmap 1 2"; echo "  rc=$?"
dmesg | grep tieredvol | tail -3

step "rebuild_badmap"
dmsetup message tv_m 0 "rebuild_badmap"; echo "  rc=$?"
dmesg | grep tieredvol | tail -5

step "clear_badmap"
dmsetup message tv_m 0 "clear_badmap 1 2"; echo "  rc=$?"

step "remove"
dmsetup remove tv_m
rmmod tieredvol
echo "DONE no-hang"
