# TieredVol V5 測試確認報告

> 版本：1.2.0
> 日期：2026-07-16
> 前置：V4（1.1.0）所有功能已穩定

---

## 編譯結果

```
$ make clean && make
gcc -Wall -Wextra -Wpedantic -std=gnu11 -O2 -o tiered_setup src/tiered_setup.c
gcc -Wall -Wextra -Wpedantic -std=gnu11 -O2 -o tiered_ui src/tiered_ui.c -lncurses
```

**零 warning。**

---

## 單元測試結果

```
$ make test
./test_tui && ./test_common

=== TieredVol TUI Unit Tests ===
22/22 passed

=== TieredVol Common Unit Tests ===
28/28 passed

Total: 50/50 passed
```

### test_tui（22 項）

| 測試 | 結果 |
|------|------|
| parse_bench_output — parallel 2 disk | PASS (4/4) |
| parse_bench_output — parallel 3 disk, out of order | PASS (6/6) |
| parse_bench_output — 'Started' line should not match | PASS (2/2) |
| parse_bench_output — disk not in output | PASS (2/2) |
| bench_disk_done — parallel output, disk completed | PASS (3/3) |
| bench_disk_done — old sequential format should NOT match | PASS (1/1) |
| bench_disk_done — disk not in buffer at all | PASS (1/1) |
| lvs command construction | PASS (3/3) |

### test_common（28 項）

| 測試 | 結果 |
|------|------|
| tiered_is_valid_name — 12 cases (normal + injection) | PASS (12/12) |
| tiered_is_valid_fs — 10 cases (valid + invalid + injection) | PASS (10/10) |
| tiered_is_valid_mount — 6 cases (valid + relative + NULL) | PASS (6/6) |

---

## CLI 整合測試

| 測試 | 結果 |
|------|------|
| `tiered_setup --version` | TieredVol 1.2.0 |
| `tiered_setup --list` | 正確顯示 4 颗碟（sda/sdb/sdc/nvme0n1），loop 裝置已過濾 |
| `tiered_setup --help` | 正確顯示使用說明 |

---

## V5 改動清單（19 項全部完成）

### A. 冗餘消除（7 項）
- A1: 刪除未使用 `LVM_CFG` 宏 ✅
- A2: `make_target()` helper 取代 8 處 `"tv_%s_carve"` 拼接 ✅
- A3: `LVM_CONF` 常數統一 6 處 LVM config 字串 ✅
- A4: `save/restore_speeds()` 取代 3 處重複 ✅
- A5: `drain_pipe_to_buf()` 取代 2 處 read loop ✅
- A6: `tiered_common.h` 共享驗證邏輯 ✅
- A7: 刪除未使用 `MAX_NAME` / `MAX_PATH` ✅

### B. 演算法（6 項）
- B1: `load_all_disk_info()` 合併 lsblk（2 次 fork 取代 2N 次）✅
- B2: `cmd_create()` 改為 fork 並行 bench ✅
- B3: 修復 "Testing..." 狀態永不顯示（加 `"Started benchmark for"` 匹配）✅
- B4: orphan fallback 加 3 秒安全警告 ✅
- B5: `cmd_status()` 改用 `opendir()` 替代 `popen("ls")` ✅
- B6: `bench_disk()` drop_caches 改為 `open()+write()` ✅

### C. 品質（2 項）
- C1: 拆分 `screen_create_select_disks()` 為 3 個 phase handler ✅
- C2: `#pragma GCC` 改為精準 push/pop ✅

### D. 安全加固（4 項）
- D1: 終端最小尺寸防禦（80x20 以下拒絕渲染）✅
- D2: `safe_execvp()` 封裝，LVM 關鍵命令改用 `fork()+execvp()` ✅
- D3: `snprintf` 截斷檢查（`sysfs_size_gb`、`sysfs_model`）✅
- D4: `run_cmd()` stdout 溢位防禦（已確認 V4 有保護）✅

### Bug Fix（1 項）
- 修復 `load_all_disk_info()` 未過濾 loop/ram/dm 裝置導致 real disks 被截斷 ✅

---

## 修復的 Bug

### Bug V5-1: `load_all_disk_info()` loop 裝置溢位
- **問題：** `MAX_DISKS = 8`，但系統有 8 個 loop 裝置 + 4 個 real disks = 12。Loop 裝置填滿陣列，real disks 被截斷。
- **修復：** 在 `load_all_disk_info()` 中過濾 `loop*`、`ram*`、`dm-*` 裝置。

### Bug V5-2: `tiered_is_valid_fs(NULL)` crash
- **問題：** `tiered_is_valid_fs()` 沒有 NULL check，`strcmp(NULL, ...)` 導致 segfault。
- **修復：** 加 `if (!fs || !*fs) return 0;`

---

## 行數統計

| 檔案 | V4 | V5 | 變化 |
|------|-----|-----|------|
| tiered_setup.c | 878 | ~950 | +72 |
| tiered_ui.c | 1249 | ~1310 | +61 |
| tiered_common.h | 0 | ~30 | +30 |
| test_tui.c | 207 | 207 | 0 |
| test_common.c | 0 | ~70 | +70 |
| Makefile | 25 | ~35 | +10 |
| **合計** | **2359** | **~2602** | **+243** |

---

## GitHub 狀態

- Repo: https://github.com/yu20060715/TieredVol
- 最新 commit: V5 重構
- 所有測試通過，可安全使用
