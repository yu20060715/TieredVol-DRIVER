# TieredVol — 開源專案改善計畫

本文檔記錄 TieredVol 的所有改善項目與狀態。
優先級：🔴 必要 > 🟡 建議 > 🟢 錦上添花 > 🔵 進階功能

---

## ✅ 已完成

| # | 項目 | Commit |
|---|------|--------|
| P0-1 | LICENSE 檔案（MIT） | `1badcba` |
| P0-2 | Pre-flight check（dmsetup, lvm2） | `1badcba` |
| P0-3 | Carve 大小驗證改善 | `1badcba` + `e60bb36` |
| P1-4 | 3 個 system() → write_sysctl() | `1badcba` |
| P1-5 | kill() → killpg() | `e89a2f1` |
| P1-6 | TOCTOU 修復（sudo cp → sudo mv） | `1badcba` |
| P0-extra | 步進 50GB → 1GB | `e89a2f1` |
| P3-12 | README 速度限制說明 | `ac2f013` |

---

## 🔴 P0：必要（已全部完成）

### 1. 加入 LICENSE 檔案 ✅
- **問題**：README 寫 MIT 但沒有 LICENSE 實體檔案，GitHub 不顯示授權標誌
- **做法**：在根目錄建立 `LICENSE`，內容為 MIT License 全文，copyright 寫 `yu20060715`
- **驗證**：GitHub repo 首頁出現 "MIT License" 標籤

### 2. Pre-flight Check（啟動時檢查依賴）✅
- **問題**：程式直接呼叫 dmsetup/lvm2，沒裝的話會在操作中途爆炸
- **做法**：在 `main()` 開頭檢查 dmsetup、vgcreate、pvcreate、lvremove 是否存在
- **位置**：`tiered_setup.c` 和 `tiered_ui.c`

### 3. Carve 大小驗證 ✅
- **問題**：使用者輸入超過碟容量的 carve 大小，等到 LVM 建 dm-linear 時才失敗
- **做法**：解析完 carve 大小後馬上檢查，加已切碟偵測、雙重確認、剩餘空間顯示

---

## 🟡 P1：建議（已全部完成）

### 4. 替換 3 個 system() 為 write_sysctl() ✅
- **問題**：`tiered_ui.c` 有 3 處使用 `system()`
- **做法**：直接寫 `/proc/sys/` 檔案（比 fork+execvp 更簡單更安全）

### 5. Signal Handler 安全性 ✅
- **問題**：`kill(-bench_pid, SIGTERM)` 語意不夠明確
- **做法**：改用 `killpg(bench_pid, SIGTERM/SIGKILL)`

### 6. TOCTOU 修復 ✅
- **問題**：`mkstemp` → `fclose` → `sudo cp` 有 race window
- **做法**：改用 `sudo mv -f` 原子操作

---

## 🟢 P2：錦上添花（未來）

### 7. USB 斷線處理
- **問題**：外接碟拔掉後 dm-linear 會壞掉，程式沒有偵測
- **做法**：在 `cmd_status()` 和 TUI `screen_status()` 加入 polling 檢查所有 dm target 的底層碟是否還在 `/sys/block/` 裡
- **難度**：中等，需要額外的 polling 機制

### 8. 支援 NVMe namespace 格式
- **問題**：`sysfs_model()` 用 `/sys/block/<name>/device/model`，NVMe 的 model 路徑不同
- **做法**：偵測到 `nvme*` 時改用 `/sys/block/<name>/device/device/model` 或 `/sys/class/nvme/nvmeX/model`
- **位置**：`tiered_setup.c:150-167`
- **難度**：簡單

### 9. Config 路徑可配置
- **問題**：`/etc/tieredvol` 硬編碼，有些系統不允許寫入
- **做法**：改用 `$HOME/.config/tieredvol/` 或加 `--config-dir` 參數
- **位置**：`tiered_setup.c:1114-1118` 和 `tiered_setup.c:1163`
- **難度**：簡單

---

## 🔵 P3：進階功能（未來）

### 10. Weighted Striping Scheduler — 按碟速度比例分配 I/O
- **問題**：LVM striping 用同一個 stripe size，快碟被迫等慢碟，`1000+500+500≠1800`
- **做法**：TieredVol 自己做 I/O Scheduler（io_uring），按速度比例直接 dispatch
- **演算法**：詳見 [PARTITION_SPLITTING.md](PARTITION_SPLITTING.md)（切塊計算）
- **實作**：詳見 [WEIGHTED_IO_SCHEDULER.md](WEIGHTED_IO_SCHEDULER.md)（I/O dispatch）
- **實作指南**：詳見 [AGENTS.md](../AGENTS.md)（完整程式碼 + 實作順序）
- **難度**：高（~20 小時，需要 io_uring dispatch、stripe buffer、offset 映射、7 個新 .c 檔案）

### 11. Stripe Size 智慧選擇
- **問題**：現在用 `has_sata ? 512 : 64` 的簡單判斷，沒有根據實際速度
- **做法**：根據碟速度差距更智慧地選擇 stripe size（例如全 NVMe 但速度差距大也用 512）
- **難度**：簡單

### 12. README 說明 LVM striping 速度限制 ✅
- **問題**：README 暗示 `2000+1000=3000`，但實際會被慢碟拖累
- **做法**：在 README 加入速度限制說明，引導使用者閱讀 PARTITION_SPLITTING.md
- **難度**：簡單（文件）

---

## 清單

- [x] P0-1: 建立 LICENSE 檔案（MIT）
- [x] P0-2: Pre-flight check（dmsetup, lvm2）
- [x] P0-3: Carve 驗證改善
- [x] P1-4: 3 個 system() → write_sysctl()
- [x] P1-5: kill() → killpg()
- [x] P1-6: TOCTOU 修復
- [x] P0-extra: 步進 50GB → 1GB
- [ ] P2-7: USB 斷線處理
- [ ] P2-8: NVMe namespace 支援
- [ ] P2-9: Config 路徑可配置
- [x] P3-10: Partition splitting 按速度比例（Phase 2）
- [ ] P3-11: Stripe size 智慧選擇
- [x] P3-12: README 速度限制說明
