# Kernel 源碼編譯進 Userspace 測試 — 可行性研究

日期：2026-08-13
狀態：prototype 已完成，27/27 測試通過

## 1. 目標

評估「把 driver/ 的 kernel 原始碼直接編譯成 userspace 測試」是否可行、
成本多少、能測到什麼、有什麼限制，並據此決定測試架構的長線方向。

## 2. 方法

- 建立最小 mock kernel 標頭於 `tests/mock/linux/`（23 個檔案，約 300 行），
  只提供 driver 需要**型別形狀**與少量 API 語意。
- 測試檔 `tests/test_stripe_kernel.c` 直接 `#include "../driver/tieredvol_stripe.c"`
  原始碼（**driver 未做任何修改**），再以 gcc 編譯執行。
- `make test` 已納入 `test_stripe_kernel`，共 2 個 suites（`test_map` + `test_stripe_kernel`）。

### Mock 清單與簡化程度

| Header | 簡化 | 風險 |
|---|---|---|
| atomic.h | GCC `__sync` 內建（單執行緒） | 低（語意一致） |
| kref.h | 完整行為 | 低 |
| timer.h | 只記錄 armed 狀態，不自動觸發 | 中（需測試手動呼叫 callback） |
| bio.h | `bio_endio` 同步呼叫 `bi_end_io`；`submit_bio` 只記錄最後 clone | 中（非同步語意需手動驅動） |
| spinlock.h | 無鎖語意（單執行緒） | 低 |
| 其餘 (dm/timer/workqueue/…) | 只有型別形狀 | 低 |

## 3. 結果

### 3.1 可行性：成立

`tv_stripe_calc_boundaries`、`tv_stripe_compute_ranges`（純數學）與
`tv_parallel_submit/end_io/timeout`（含 kref/timer/bio 交接）都能在
userspace 直接執行並受測。

### 3.2 已抓到的語意誤解（對照式測試測不到）

編寫測試的預期值與 kernel 實作有 5 處不符，全部是「replica 重實作」看不見的細節：

1. `s_off == disk_end[i]` 邊界為 exclusive → 落在下一碟（非上一碟）。
2. 跨碟範圍只在範圍真的穿越邊界時才 split（`ps < pe`）。
3. `b_end <= s_sz` 時用「含 prev 比較」、跨 stripe 時用「首個命中即 break」，
   兩條路徑的 `fi/li` 語意不同。
4. `d_start` 是 **disk 層相對位址**（`stripe_no * dchunk + (ps - c_beg)`），
   不是 logical 偏移。
5. `compute_ranges` 依賴先前 `calc_boundaries` 的 `sc` 狀態（兩階段 API），
   且範圍跨 stripe 時只用 fi..li 內的一段。

這證明本路線能對「replica 看不見的行為」提供真值錨點。

### 3.3 成本量化

- 初次 mock：約 300 行 header + 1 個測試檔（210 行）。
- 每多編譯一個 driver .c 檔：mock 缺失 API 增量，粗估 30–100 行/檔。
- 建議優先擴充：`tieredvol_borrow.c`（redirect/lookup/pick_dst 的鎖下語意）、
  `tieredvol_core.c`（meta save/load 的 binary/crc32 格式，可測 v2 格式）。

## 4. 限制

- **非同步路徑需手動驅動**：bio 完成與 timer 觸發由測試自行呼叫，無法驗證
  IRQ/softirq 交錯。真實並發只能用 kernel（實機回歸腳本）驗證。
- **spinlock 無語意**：單執行緒測試測不到鎖的 race；但可測「持鎖下欄位操作
  是否正確」。
- **記憶體模型**：`__sync` 在單執行緒下不驗證記憶體序。

## 5. 結論與建議

- **採用此路線**：作為「replica 重實作 + 實機回歸」之外的第三層
  （真源碼單元層）。三層分工：
  - `tests/` replica：高覆蓋率、快速回歸，測「設計意圖」。
  - `test_stripe_kernel`（mock kernel）：測「真實實作」的純邏輯與交接。
  - scripts/ 實機回歸：測「真實 kernel」行為與效能。
- **後續擴充**（記錄於 ROADMAP）：把 borrow/core 的 save/load 格式測試
  遷移或擴充到本層，讓 .borrow v2 與 meta crc32 有「真實實作」的格式驗證。
