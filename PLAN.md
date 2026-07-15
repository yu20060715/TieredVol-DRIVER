# TieredVol V4 優化計畫

> 目標：不新增功能，純優化現有功能的穩定性、效能、UX

## 功能優化總表

| 類別 | 編號 | 改動名稱 | 檔案 | 優先級 | 預估行數 |
|------|------|----------|------|--------|----------|
| **A. Benchmark** | A1 | 平行 Benchmark（fork 並行測速） | tiered_setup.c | 🔴高 | +80 |
| | A2 | TUI 進度百分比（pass X/5） | tiered_ui.c | 🔴高 | +30 |
| | A3 | Signal Handler 清理殘留檔 | tiered_setup.c | 🟡中 | +25 |
| | A4 | Create 流程鎖定測試中磁碟 | tiered_ui.c | 🟡中 | +20 |
| **B. RAM Cache** | B1 | 顯示剩餘安全記憶體 | tiered_ui.c | 🟢低 | +10 |
| | B2 | dirty_bg_ratio 上限 40% | tiered_ui.c | 🟢低 | +3 |
| **C. Volume 防護** | C1 | cmd_create rollback on failure | tiered_setup.c | 🔴高 | +60 |
| | C2 | 偵測已掛載磁碟並禁用 | tiered_ui.c | 🔴高 | +20 |
| | C3 | 選碟數量不足提示 | tiered_ui.c | 🟡中 | +5 |
| **D. TUI 體驗** | D1 | Re-benchmark 按鍵（B） | tiered_ui.c | 🟡中 | +15 |
| | D2 | Status 結構化表格顯示 | tiered_ui.c | 🟡中 | +40 |
| | D3 | input_str 方向鍵游標移動 | tiered_ui.c | 🟡中 | +30 |
| | D4 | Tab 切換輸入欄位 | tiered_ui.c | 🟢低 | +10 |
| | D5 | KEY_RESIZE 確認（已隱式處理） | tiered_ui.c | ⚪無 | 0 |
| **E. 自動偵測** | E1 | 啟動掃描已有 Volume | tiered_ui.c | 🔴高 | +25 |
| **F. 代碼品質** | F1 | 名稱白名單（防注入） | tiered_setup.c | 🟡中 | +15 |
| | F2 | Makefile 加 install + flags | Makefile | 🟢低 | +10 |
| | | **合計** | | | **~398** |

## Phase 規劃

| Phase | 內容 | 依賴 |
|-------|------|------|
| 1 | tiered_setup.c 改動 | F1→A3→A1→C1→F2 |
| 2 | tiered_ui.c 改動 | E1→C2→C3→A4→A2→D1→D3→D4→D2→B1→B2 |
| 3 | 編譯測試 | Phase 1+2 完成後 |

## 詳細設計

### A1: 平行 Benchmark

目前 `cmd_bench()` 依序測每顆碟（5 iteration x 512MB = 30-45s/3碟）。
改為：`bench_disk()` 改為可獨立呼叫的函式，`cmd_bench()` 傳 n 個 fork，
每個子程序跑一顆碟，父程序 `waitpid(WNOHANG)` 輪詢，全部完成後收集結果。

- tiered_setup.c 新增 `bench_disk_parallel()` 函式
- 每個 fork 的子程序寫速度到 pipe，父程序讀取
- tiered_ui.c 的 `auto_bench_start()` 改為呼叫 parallel 版本

### A3: Signal Handler

`bench_disk()` 沒有 signal handler，Ctrl+C 會留下 `/tmp/db_*` 殘留。
改為安裝 `SIGINT`/`SIGTERM` handler，清理 temp mount + test file。

### C1: Rollback on Failure

`cmd_create()` 步驟 3-7 都沒檢查回傳值，失敗時殘留 LV/VG/PV/dm。
改為每步檢查回傳值，失敗時執行反向清理（lvremove → vgremove → pvremove → dmsetup remove）。

### E1: 自動偵測已有 Volume

啟動時掃描 `dmsetup ls | grep tv_`，解析 VG/LV 名稱，
再 `mount | grep tv_` 取得掛載點，填入 `vol_name` / `mount_point` 全域變數。
