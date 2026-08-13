#!/usr/bin/env python3
"""Convert tieredvol config diskN_name=/dev/sdX|nvmeXnX to stable /dev/disk/by-id/ paths.

The kernel binds disks by block-device path, but /dev names (sda, nvme0n1, ...)
change when hardware is re-enumerated or drives are physically moved between
slots.  /dev/disk/by-id/ is bound to the drive's serial/WWN, so it follows the
physical disk.  by-path is bound to the slot and does NOT follow the drive.

Config files may carry a crc32= integrity line.  The kernel validates it on load
(CRC mismatch => -EIO) using crc32c(0, bytes-before-the-crc32=-line) — the raw
Castagnoli reflected CRC with init 0 and no final XOR.  This tool recomputes that
line after rewriting disk names so the config still passes kernel validation.

Usage:
  python3 scripts/conf_to_byid.py FILE...       # convert in place (+ .bak-byid)
  python3 scripts/conf_to_byid.py --check FILE...  # verify stored crc, no write
"""

import os
import shutil
import sys

# Stable by-id paths for this machine (ls -l /dev/disk/by-id/).  These follow
# the physical drive regardless of which port/slot it is currently plugged into.
MAP = {
    "/dev/nvme0n1": "/dev/disk/by-id/nvme-WDS500G3X0C-00SJG0_200705800588",
    "/dev/nvme1n1": "/dev/disk/by-id/nvme-CT1000P3PSSD8_242849E47E24",
    "/dev/sdc":     "/dev/disk/by-id/ata-CT500MX500SSD1_2129E5B858A9",
    "/dev/sdb":     "/dev/disk/by-id/ata-WDC_WDS250G2B0A_201965800292",
}

_POLY = 0x82F63B78  # reflected CRC-32C (Castagnoli)

_TABLE = []
for _i in range(256):
    _c = _i
    for _ in range(8):
        _c = (_c >> 1) ^ _POLY if _c & 1 else _c >> 1
    _TABLE.append(_c & 0xFFFFFFFF)


def crc32c(data: bytes, crc: int = 0) -> int:
    """Raw Castagnoli reflected CRC, init 0, no final XOR — matches kernel crc32c(0,...)."""
    for b in data:
        crc = (crc >> 8) ^ _TABLE[(crc ^ b) & 0xFF]
    return crc & 0xFFFFFFFF


def _split_crc(data: bytes):
    """Return (prefix, stored_crc_or_None). prefix = bytes before the crc32= line."""
    idx = data.find(b"crc32=")
    if idx < 0:
        return data, None
    line_start = data.rfind(b"\n", 0, idx) + 1
    prefix = data[:line_start]
    rest = data[line_start:]
    fields = rest.split(b"\n", 1)[0].split(b"=", 1)
    stored = int(fields[1].strip()) if len(fields) == 2 else None
    return prefix, stored


def verify(data: bytes) -> bool:
    prefix, stored = _split_crc(data)
    if stored is None:
        return True
    return crc32c(prefix) == stored


def convert(data: bytes):
    lines = data.split(b"\n")
    out = []
    for ln in lines:
        if ln.startswith(b"disk") and b"_name=" in ln:
            key, _, val = ln.partition(b"=")
            name = val.decode()
            if name in MAP:
                ln = key + b"=" + MAP[name].encode()
        out.append(ln)
    new = b"\n".join(out)
    if b"crc32=" in new:
        prefix, _ = _split_crc(new)
        new = prefix + b"crc32=%d\n" % crc32c(prefix)
    return new


def main():
    check_only = False
    files = []
    for a in sys.argv[1:]:
        if a == "--check":
            check_only = True
        else:
            files.append(a)
    if not files:
        print(__doc__)
        sys.exit(1)

    rc = 0
    for path in files:
        with open(path, "rb") as f:
            data = f.read()
        if not verify(data):
            print(f"[CRC-BAD] {path}: stored crc does not match content (refusing)")
            rc = 1
            continue
        new = convert(data)
        if new == data:
            print(f"[SKIP] {path}: no diskN_name to convert")
            continue
        if check_only:
            ok = verify(new)
            print(f"[CHECK] {path}: converted {'OK' if ok else 'BAD crc'}")
            if not ok:
                rc = 1
            continue
        shutil.copy2(path, path + ".bak-byid")
        with open(path, "wb") as f:
            f.write(new)
        print(f"[CONV] {path}: -> by-id"
              + (f" (crc32 updated)" if b"crc32=" in data else " (no crc line)"))
    sys.exit(rc)


if __name__ == "__main__":
    main()
