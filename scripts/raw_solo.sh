#!/bin/bash
# raw_solo.sh — 單碟 raw solo 基線（1M 順序寫+讀，fio 參數同疊碟標準）
# 前置：sudo；A/B/C/D 為專用測試碟（系統碟 sda 勿測）。raw 寫 8G 會覆蓋碟前段資料。
# 可用環境變數覆寫：SOLO_DEVS=..(冒號分隔) SOLO_RW=write:read SOLO_SIZE=..
set -u
OUT=/home/yu/raw_solo_$(date +%m%d_%H%M).log
exec > >(tee "$OUT") 2>&1
DEVS=${SOLO_DEVS:-nvme1n1:nvme0n1:sdc:sdb}
RW=${SOLO_RW:-write:read}
SZ=${SOLO_SIZE:-8G}

fio_bw(){ python3 -c "
import json
raw=open('$1','rb').read().decode('utf-8','replace')
i=raw.find('{')
d=json.loads(raw[i:])['jobs'][0]
w=d.get('write',{}).get('bw_bytes',0)
r=d.get('read',{}).get('bw_bytes',0)
e=d.get('error',0)
print(round((w or r)/1e6,1),e)"; }

echo "raw solo fio 1M $SZ (libaio depth=32 direct)"
for dev in ${DEVS//:/ }; do
  for rw in ${RW//:/ }; do
    FSYNC=""; [ "$rw" = write ] && FSYNC="--end_fsync=1"
    fio --name=solo --filename=/dev/$dev --rw=$rw --bs=1M --size=$SZ \
      --direct=1 --ioengine=libaio --iodepth=32 --numjobs=1 $FSYNC \
      --output-format=json --output=/tmp/solo_${dev}_${rw}.json >/dev/null 2>&1
    echo "  $dev $rw: $(fio_bw /tmp/solo_${dev}_${rw}.json) MB/s"
  done
done
echo "done: $OUT"
