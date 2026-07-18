# TieredVol — 開源專案改善計畫

本文檔記錄 TieredVol 達到「勉強合格開源」所需的所有改善項目。
優先級：🔴 必要 > 🟡 建議 > 🟢 錦上添花

---

## 🔴 P0：必要（沒有不能上線）

### 1. 加入 LICENSE 檔案
- **問題**：README 寫 MIT 但沒有 LICENSE 實體檔案，GitHub 不顯示授權標誌
- **做法**：在根目錄建立 `LICENSE`，內容為 MIT License 全文，copyright 寫 `yu20060715`
- **驗證**：GitHub repo 首頁出現 "MIT License" 標籤

### 2. Pre-flight Check（啟動時檢查依賴）
- **問題**：程式直接呼叫 dmsetup/lvm2，沒裝的話會在操作中途爆炸，錯誤訊息不明確
- **做法**：在 `tiered_setup.c` 的 `main()` 和 `tiered_ui.c` 的 `main()` 開頭，檢查以下指令是否存在：
  - `dmsetup`
  - `vgcreate`（代表 lvm2）
  - `pvcreate`
  - `lvremove`
- **實作方式**：用 `which <cmd>` 或直接 `execvp` 測試，找不到就 `fprintf(stderr, "Error: %s not found. Install lvm2: apt install lvm2\n")` 然後 exit
- **位置**：`tiered_setup.c:1300` 和 `tiered_ui.c:1305`，在 root check 之後加

### 3. Carve 大小驗證
- **問題**：使用者輸入 `--disks sda:5000`（碟只有 1TB），程式不會擋，等到 LVM 建 dm-linear 時才失敗，rollback 雖然會清但使用者體驗差
- **做法**：在 `cmd_create()` 解析完 carve 大小後，馬上檢查每個 carve 是否超過碟容量
- **位置**：`tiered_setup.c:772-779`，在現有的 `carve_gb > size_gb - 1` 檢查**之前**，加一個更早的驗證
- **注意**：目前已經有檢查了（775 行），但錯誤訊息可以更清楚，例如提示「最大可 carve %lldGB」

---

## 🟡 P1：建議（Code Review 不會被挑）

### 4. 替換 3 個 system() 為 fork+execvp
- **問題**：`tiered_ui.c` 有 3 處使用 `system()`，雖然輸入是整數（%d）所以安全，但 code review 會被點
- **位置與做法**：

| 位置 | 現在 | 改成 |
|------|------|------|
| `tiered_ui.c:484` | `system(cmd)` | `run_cmd()` 或新增 `run_sysctl()` 函式 |
| `tiered_ui.c:496` | `system(cmd)` | 同上 |
| `tiered_ui.c:1366` | `system(cmd)` | 同上 |

- **實作方式**：新增一個 `static int run_sysctl_int(const char *key, int value)` 函式，用 fork+execvp 呼叫 `/usr/sbin/sysctl`，或直接寫 `/proc/sys/` 檔案（更簡單、更安全）

### 5. Signal Handler 安全性
- **問題**：`tiered_ui.c:1369` 用 `kill(-bench_pid, SIGTERM)` 殺 process group，如果 `bench_pid` 不是 group leader 會殺錯
- **做法**：
  - 方案 A：在 `auto_bench_start()` 的 child process 裡加 `setpgid(0, 0)` 確保 child 是 group leader（目前 `tiered_ui.c:227` 已經有做了 ✅）
  - 方案 B：改用 `killpg(bench_pid, SIGTERM)` 明確殺 process group
- **結論**：目前 227 行已經有 `setpgid(0, 0)`，所以 `-bench_pid` 是對的。但建議改成 `killpg()` 更語意明確

### 6. TOCTOU 修復（暫存檔安全）
- **問題**：`tiered_setup.c:1101-1124` 用 `mkstemp` 建暫存檔 → `fclose` → `sudo cp` 到 destination，中間有 race window
- **做法**：改用 `rename()` 原子操作（但需要 root 權限才能 rename 到 `/etc/tieredvol/`），或者改用 `fchmod` + `write` 直接寫到 destination（先建暫存檔，fchmod 設權限，再 rename）
- **位置**：`tiered_setup.c:1101-1124`

---

## 🟢 P2：錦上添花

### 7. USB 斷線處理
- **問題**：外接碟拔掉後 dm-linear 會壞掉，程式沒有偵測
- **做法**：在 `cmd_status()` 和 TUI `screen_status()` 加入 checking，定期（或使用者觸發時）檢查所有 dm target 的底層碟是否還在 `/sys/block/` 裡
- **難度**：中等，需要額外的 polling 機制

### 8. 支援 NVMe namespace 格式
- **問題**：`sysfs_model()` 用 `/sys/block/<name>/device/model`，NVMe 的 model 路徑不同
- **做法**：偵測到 `nvme*` 時改用 `/sys/block/<name>/device/device/model` 或 `/sys/class/nvme/nvmeX/model`
- **位置**：`tiered_setup.c:150-167`

### 9. Config 路徑可配置
- **問題**：`/etc/tieredvol` 硬編碼，有些系統不允許寫入
- **做法**：改用 `$HOME/.config/tieredvol/` 或加 `--config-dir` 參數
- **位置**：`tiered_setup.c:1114-1118` 和 `tiered_setup.c:1163`

---

## 實作順序建議

```
Phase 1（必要，1-2 小時）：
  1. LICENSE 檔案
  2. Pre-flight check
  3. Carve 驗證改善

Phase 2（建議，2-3 小時）：
  4. 3 個 system() 替換
  5. killpg() 替換
  6. TOCTOU 修復

Phase 3（錦上添花，看需要）：
  7. USB 斷線處理
  8. NVMe namespace 支援
  9. Config 路徑可配置
```

---

## 清單

- [ ] P0-1: 建立 LICENSE 檔案（MIT）
- [ ] P0-2: Pre-flight check（dmsetup, lvm2）
- [ ] P0-3: Carve 驗證改善
- [ ] P1-4: 3 個 system() → fork+execvp 或 /proc/sys 寫入
- [ ] P1-5: kill() → killpg()
- [ ] P1-6: TOCTOU 修復
- [ ] P2-7: USB 斷線處理
- [ ] P2-8: NVMe namespace 支援
- [ ] P2-9: Config 路徑可配置
