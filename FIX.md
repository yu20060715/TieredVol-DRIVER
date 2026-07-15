# TUI↔CLI Connection Bug Fixes

## Bug 1 (HIGH): `parse_bench_output()` wrong speed in parallel mode
**File:** `src/tiered_ui.c:185-202`
**Problem:** `strstr(out, d->disk)` matches the first occurrence of the disk name, which may be a "Started benchmark for /dev/XXX" line or another disk's result line. In parallel mode with 4+ disks, results may arrive out of order — the parser could read disk B's speed into disk A.
**Fix:** For each disk, search for `"  /dev/<disk>:"` (the result line format) instead of bare `d->disk`. This ensures we read from the correct line.

## Bug 2 (HIGH): `bench_disk_done()` broken in parallel mode
**File:** `src/tiered_ui.c:277-284`
**Problem:** Searches for `"Testing /dev/<disk>"` and `"<disk> ... Write:"` which only exist in sequential mode output. In parallel mode, output is `"  /dev/<disk>: Write X MB/s  Read Y MB/s"` — disk never shows as "done" during bench progress.
**Fix:** Search for `"  /dev/<disk>: Write"` to detect completion in parallel mode.

## Bug 3 (HIGH): `screen_status()` lvs command malformed
**File:** `src/tiered_ui.c:1032`
**Problem:** String literal concatenation `"tv_vg_" "2>/dev/null"` produces `"tv_vg_2>/dev/null"` — no VG name is interpolated. The lvs command always fails.
**Fix:** Use snprintf to inject `vol_name` into the command.

## Bug 4 (HIGH): UI doesn't validate input names before passing to CLI
**File:** `src/tiered_ui.c:965-967`
**Problem:** `vol_name`, `input_mount`, `input_fs` are passed unquoted to the shell command. Spaces or shell metacharacters in these fields break the command or could cause injection.
**Fix:** Add validation: vol_name must pass `is_valid_name()`, input_fs must be from allowed set, input_mount must be a valid absolute path.

## Bug 5 (MEDIUM): `parse_disk_list()` doesn't parse "T" suffix
**File:** `src/tiered_ui.c:164-165`
**Problem:** CLI outputs "1.0T" for ≥1024GB disks, but `atoll(p)` stops at the decimal point, returning 1 instead of 1024.
**Fix:** After atoll, check for 'T' suffix and multiply by 1024. Handle "1.0T" style by also parsing the decimal part.
