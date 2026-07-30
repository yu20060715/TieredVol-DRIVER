# TieredVol-DRIVER Roadmap

## Short-term（thesis submission）

- [ ] **[BUG] Mirror capacity check**
      `tieredvol_ctr` 沒檢查 mirror 碟容量 ≥ segment 總容量。若 mirror 碟太小，
      寫超過會靜默失敗。要在 `tieredvol_core.c` 加上驗證：`disk_sectors[mirror_disk] * SECTOR_SIZE >= seg_size`。

- [ ] **Bad block bitmap（formula + bitmap hybrid）**
      Per-disk bitmap (1 bit / chunk), formula stays O(1), filter bad chunks at dispatch time.
      Read → return zero; Write → redirect to next healthy disk in same stripe.

- [ ] **Write error recovery**
      On write failure: retry once, then mark chunk bad, fall back to mirror or zero-fill.
      No data corruption from partial stripe writes.

- [ ] **Parallel submit timeout watchdog**
      Timer-based watchdog for tv_parallel_block. If a sub-bio never completes,
      force-complete with error and mark disks degraded.

## Medium-term

- [ ] **Background rebuild for bad blocks**
      Kernel thread that scans bitmap, rewrites bad chunks from mirror or recomputes
      from remaining disks in the segment.

- [ ] **Per-segment policy switching**
      Dynamically switch static ↔ adaptive per segment. Safe because policy only
      changes disk selection strategy, not stripe geometry — mapping stays valid.

- [ ] **Runtime benchmark display (read-only)**
      Periodic speed test exposed via sysfs / dmsetup message. User reads it,
      decides whether to edit .conf and reload. No automatic weight changes.
