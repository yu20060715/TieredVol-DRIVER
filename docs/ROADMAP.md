# TieredVol-DRIVER Roadmap

> 狀態以 driver 原始碼為準（2026-08 實地對照）。

## Short-term（thesis submission）

- [x] **[BUG] Mirror capacity check**
      `tieredvol_ctr` 檢查 mirror 碟容量 ≥ segment 總容量（`tieredvol_core.c`
      `"Validate mirror disk capacity"`）。已修，mirror 碟不足會拒絕 load。

- [x] **Bad block bitmap（formula + bitmap hybrid）**
      Per-disk bitmap (1 bit / chunk)，formula 保持 O(1)，dispatch 時過濾 bad chunk
      （`tieredvol_core.c` Phase D）：Read → `zero_fill_bio`；Write → skip。
      實作於 `tieredvol_badmap.c`（init/test/set/clear）。

- [x] **Write error recovery**
      寫失敗：**無 retry**，直接 `tv_badmap_set` 標記 chunk bad（`tieredvol_mirror.c`
      `"Mark bad block on WRITE error"`），並依 `error_threshold` 標記 disk degraded。
      後續讀該 chunk 回 zero、寫則 skip。

- [x] **Parallel submit timeout watchdog**
      `tv_parallel_timeout`（`tieredvol_stripe.c`）：timer + `TV_PARALLEL_TIMEOUT`，
      sub-bio 未完成即強制完成並標記 disk degraded。

## Medium-term

- [x] **Background rebuild for bad blocks**
      `tv_badmap_rebuild`（`tieredvol_badmap.c`）：掃描 bitmap，重新 read bad chunk，
      成功即 clear bit。注意：現為「重讀清標」，非從 mirror 重算/重寫。

- [x] **Per-segment policy switching**
      `set_seg_policy`（`tieredvol_msg_policy.c`）。policy 只改 disk 選擇策略、
      不改 stripe 幾何，映射仍有效。

- [x] **Runtime benchmark display (read-only)**
      `show_bench`（`tieredvol_msg_stats.c`）。使用者讀取後自行決定是否改 .conf 重載，
      無自動調權重。
