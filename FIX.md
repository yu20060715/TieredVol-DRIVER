# TieredVol — Bug Fix Plan (11 issues)

Fix all 11 issues found during code audit. Each fix includes exact location, change, and verification.

---

## Fix #1: Buffer overflow in `drain_pipe_to_buf` 🔴

**File:** `src/tiered_ui.c:184-198`
**Problem:** After trimming to the last newline, the remaining tail + new chunk can exceed `bench_buf[16384]`.
**Fix:** After trim, re-check capacity and truncate if needed.

```c
// BEFORE (line ~196)
bench_buf_len = tail_len;
// Falls through to memcpy without re-checking

// AFTER
bench_buf_len = tail_len;
// Re-check: if still no room, force truncate
if (bench_buf_len + (int)n >= (int)sizeof(bench_buf) - 1) {
    bench_buf_len = 0;  // Buffer full of junk, start fresh
}
```

**Verification:** Code review + manual test with large fio output.

---

## Fix #2: Pointer underflow in `tiered_is_valid_mount` 🔴

**File:** `src/tiered_common.h:32`
**Problem:** `strncmp(mp + strlen(mp) - 3, "/..", 3)` is UB when `strlen(mp) < 3`.
**Fix:** Add length guard.

```c
// BEFORE
if (strncmp(mp + strlen(mp) - 3, "/..", 3) == 0)

// AFTER
if (strlen(mp) >= 3 && strncmp(mp + strlen(mp) - 3, "/..", 3) == 0)
```

**Verification:** Add test case: `check(tiered_is_valid_mount("/a") == 1, "/a short path");`

---

## Fix #3: Inconsistent shell quoting 🟡

**File:** `src/tiered_ui.c:1274`
**Problem:** `vol_name` not quoted in snprintf command string.
**Fix:** Add quotes around `%s`.

```c
// BEFORE
snprintf(cmd, sizeof(cmd), "%s --destroy --name %s 2>&1", tool_path, vol_name);

// AFTER
snprintf(cmd, sizeof(cmd), "%s --destroy --name \"%s\" 2>&1", tool_path, vol_name);
```

**Verification:** Code review.

---

## Fix #4: Anonymous struct pipe protocol mismatch 🟡

**File:** `src/tiered_setup.c:801-811`
**Problem:** Child writes with anonymous struct, parent reads with `bench_result_t`.
**Fix:** Use `bench_result_t` in child process.

```c
// BEFORE
struct { int ret; double w; double r; } result = { ret, w, r };

// AFTER
bench_result_t result = { ret, w, r };
```

**Verification:** Compile check, benchmark still works.

---

## Fix #5: Silent disk_spec truncation 🟡

**File:** `src/tiered_ui.c:1053-1064`
**Problem:** If disk_spec overflows 1024 bytes, silently truncated.
**Fix:** Add error handling when buffer full.

```c
// BEFORE
if (offset + n + 1 < (int)sizeof(disk_spec)) {
    memcpy(disk_spec + offset, disk, n);
    offset += n;
    disk_spec[offset] = 0;
}

// AFTER
if (offset + n + 1 >= (int)sizeof(disk_spec)) {
    // disk_spec full, stop appending
    break;
}
memcpy(disk_spec + offset, disk, n);
offset += n;
disk_spec[offset] = 0;
```

**Verification:** Code review.

---

## Fix #6: Replace 3 `system()` with `/proc/sys/` writes 🟢

**File:** `src/tiered_ui.c:484, 496, 1366`
**Problem:** Uses `system("sysctl ...")` which invokes shell.
**Fix:** Write directly to `/proc/sys/vm/` files.

```c
// BEFORE (line 484)
snprintf(cmd, sizeof(cmd), "sysctl -w vm.dirty_ratio=%d", new_dirty);
system(cmd);

// AFTER
static void write_sysctl(const char *path, int value) {
    FILE *f = fopen(path, "w");
    if (f) { fprintf(f, "%d\n", value); fclose(f); }
}
// Usage:
write_sysctl("/proc/sys/vm/dirty_ratio", new_dirty);
```

Also replace:
- Line 496: `dirty_background_ratio` → `write_sysctl("/proc/sys/vm/dirty_background_ratio", new_bg)`
- Line 1366: restore `dirty_ratio` → `write_sysctl("/proc/sys/vm/dirty_ratio", ...)`
- Line 1367: restore `dirty_background_ratio` → `write_sysctl("/proc/sys/vm/dirty_background_ratio", ...)`

**Verification:** Compile, run TUI RAM Cache screen, verify sysctl values change.

---

## Fix #7: Empty `()` → `(void)` 🟢

**File:** `src/tiered_ui.c` (15 locations)
**Problem:** `()` means unspecified params in C, should be `(void)`.
**Fix:** Global replace of function declarations.

Affected functions (all in tiered_ui.c):
- `detect_tool_path()` → `detect_tool_path(void)`
- `screen_main()` → `screen_main(void)`
- `screen_list_disks()` → `screen_list_disks(void)`
- `screen_bench()` → `screen_bench(void)`
- `screen_create()` → `screen_create(void)`
- `screen_create_select_disks()` → `screen_create_select_disks(void)`
- `screen_status()` → `screen_status(void)`
- `screen_destroy()` → `screen_destroy(void)`
- `screen_ram_cache()` → `screen_ram_cache(void)`
- `auto_bench_start()` → `auto_bench_start(void)`
- `auto_bench_stop()` → `auto_bench_stop(void)`
- `drain_pipe_to_buf()` → `drain_pipe_to_buf(void)`
- And any others found by grep

**Verification:** Compile with `-Wpedantic`, no warnings.

---

## Fix #8: Remove `#pragma` warning suppression 🟢

**File:** `src/tiered_setup.c:19-21`, `src/tiered_ui.c:16-18`
**Problem:** Hides real warnings (like the buffer overflow in Fix #1).
**Fix:** Delete the 3 `#pragma` lines in each file (6 lines total).

**Verification:** Compile with `-Wall -Wextra -Wpedantic`, no warnings.

---

## Fix #9: TOCTOU in config save 🟢

**File:** `src/tiered_setup.c:1101-1124`
**Problem:** `mkstemp` → `fclose` → `sudo cp` has race window.
**Fix:** Use `rename()` after writing to temp file.

```c
// BEFORE
FILE *f = fopen(tmp_path, "w");
// ... write ...
fclose(f);
snprintf(cmd, sizeof(cmd), "cp %s %s", tmp_path, dest_path);
system(cmd);

// AFTER
FILE *f = fopen(tmp_path, "w");
// ... write ...
fflush(f);
fsync(fileno(f));
fclose(f);
// Use sudo rename (atomic)
snprintf(cmd, sizeof(cmd), "mv %s %s", tmp_path, dest_path);
safe_execvp("/usr/bin/sudo", ...);
```

**Verification:** Code review, test config save.

---

## Fix #10: Pre-flight dependency check 🟢

**File:** `src/tiered_setup.c:main()`, `src/tiered_ui.c:main()`
**Problem:** No check for required tools before operation.
**Fix:** Add check after root detection.

```c
// Add to both files after root check
static int check_deps(void) {
    const char *deps[] = {"dmsetup", "vgcreate", "pvcreate", NULL};
    for (int i = 0; deps[i]; i++) {
        char cmd[64];
        snprintf(cmd, sizeof(cmd), "which %s > /dev/null 2>&1", deps[i]);
        if (system(cmd) != 0) {
            fprintf(stderr, "Error: '%s' not found. Install lvm2: apt install lvm2\n", deps[i]);
            return -1;
        }
    }
    return 0;
}
// In main(), after root check:
if (check_deps() != 0) return 1;
```

**Verification:** Uninstall lvm2, run program, verify error message.

---

## Fix #11: Add LICENSE file 🟢

**File:** `LICENSE` (new file at root)
**Problem:** README says MIT but no LICENSE file exists.
**Fix:** Create MIT LICENSE file with `yu20060715` copyright.

**Verification:** GitHub repo shows "MIT License" badge.

---

## Execution Order

1. Fix #1 + #2 (critical bugs)
2. Fix #3 + #4 + #5 (medium bugs)
3. Fix #6 + #7 + #8 (code quality)
4. Fix #9 + #10 + #11 (infrastructure)
5. Run `make clean && make` — zero warnings
6. Run `make test` — all tests pass

## Estimated Time: ~45 minutes total
