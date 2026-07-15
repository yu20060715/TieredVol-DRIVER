# TieredVol V4 實作步驟

## Phase 1: tiered_setup.c

### Step 1.1: F1 — 名稱白名單
- 新增 `is_valid_name()` 函式，校驗 `[a-zA-Z0-9._-]`
- `cmd_create()` 的 `--name` 參數、`cmd_bench()` 的 `--disks` 參數都過濾
- 失敗時 fprintf(stderr) 告知合法字元

### Step 1.2: A3 — Signal Handler
- 新增 `bench_cleanup()` 函式：刪除 `mp/.bench_*`、rmdir `mp`
- 新增 `bench_signal_handler()`：設 flag，主循環退出前呼叫 cleanup
- `bench_disk()` 入口處安裝 `SIGINT`/`SIGTERM` handler
- 結束時還原原始 handler

### Step 1.3: A1 — 平行 Benchmark
- `bench_disk()` 簽名不變，但新增 static `bench_pipe_fds` 供子程序回傳結果
- `cmd_bench()` 改為：fork n 個子程序，每個子程序呼叫 `bench_disk()`，結果寫 pipe
- 父程序用 `waitpid(WNOHANG)` 輪詢，pipe 非阻塞讀取子程序回傳的速度
- 當全部子程序完成後，print 結果表
- 保留原本 sequential 模式作為 fallback（加 `--sequential` flag）

### Step 1.4: C1 — Rollback on Failure
- `cmd_create()` 每個 step 都檢查回傳值
- 新增 `cleanup_create()` 函式：接受 valid_disks array，依序執行
  - `lvremove -f tv_vg_<name>/tv_lv_<name>`
  - `vgremove -f tv_vg_<name>`
  - loop: `pvremove /dev/mapper/tv_<disk>_carve` + `dmsetup remove tv_<disk>_carve`
- 步驟失敗時呼叫 `cleanup_create()` 後 return 1

### Step 1.5: F2 — Makefile 改進
- CFLAGS 加 `-Wextra -Wpedantic -std=gnu11`
- 新增 `install` target: `cp tiered_setup $(PREFIX)/bin/ && cp tiered_ui $(PREFIX)/bin/`
- 新增 `uninstall` target: `rm $(PREFIX)/bin/tiered_setup $(PREFIX)/bin/tiered_ui`

---

## Phase 2: tiered_ui.c

### Step 2.1: E1 — 自動偵測已有 Volume
- 新增 `detect_existing_volume()` 函式
- 執行 `dmsetup ls 2>/dev/null`，grep `tv_` 找 vg 名稱
- 執行 `mount | grep /dev/mapper/tv_` 取掛載點
- 填入 `vol_name[]` / `mount_point[]`
- 在 `main()` 的 `parse_disk_list()` 之後呼叫

### Step 2.2: C2 — 偵測已掛載磁碟
- `ui_disk_t` 新增 `int is_mounted` 欄位
- `parse_disk_list()` 後掃描 `/proc/mounts`，標記已掛載的磁碟
- Create 流程 Phase 0：已掛載磁碟顯示 `[MOUNTED]` 且不可 toggle
- disk_list 顯示已掛載磁碟帶 `[MOUNTED]` 標記

### Step 2.3: C3 — 選碟數量提示
- `screen_create_select_disks()` Phase 0：Enter 但 `sel_count < 2` 時
- 在底部 draw_status_bar 用 COLOR_PAIR(3) 紅色顯示 "至少選擇 2 顆碟"

### Step 2.4: A4 — Create 鎖定測試中磁碟
- Phase 0：若 `bench_finished==0` 且 `bench_disk_done(disk)==0`
- 該碟 Space 不允許 toggle，顯示 "Benchmarking..."

### Step 2.5: A2 — TUI 進度百分比
- `bench_disk_done()` 改為回傳 int：0=未完成, 1=pass X, 2=完成
- 新增 `bench_disk_pass()` 回傳目前第幾輪 pass
- screen_bench() 顯示 "Testing sdb: pass 3/5 Write xxx MB/s"

### Step 2.6: D1 — Re-benchmark 按鍵
- `screen_disk_list()` 的 getch() 加 'b'/'B' case
- 按 B → 重置 bench 相關全域變數 → `auto_bench_start()` 重跑

### Step 2.7: D3 — input_str 方向鍵
- 加 KEY_LEFT/KEY_RIGHT case：移動 pos 和 cx
- 支援中間插入字元：memmove 後插入
- 支援 Home/End（可選）

### Step 2.8: D4 — Tab 切換欄位
- Phase 2 的 input_str 循環加 '\t' case → return 前存值到當前欄位
- 在 screen_create_select_disks() Phase 2 用 loop 管理 3 個欄位的 Tab 切換

### Step 2.9: D2 — Status 結構化
- 重寫 `screen_status()`
- 解析 dmsetup ls → 提取 VG 名稱
- 查 `lvs -o lv_name,lv_size,stripes vg_name` 取結構化資料
- 查 `df -h mount_point` 取使用率
- 畫成整齊的表格：Volume | Size | Stripes | Mount | Usage

### Step 2.10: B1 — 顯示剩餘安全記憶體
- screen_ram_cache() 在 Borrow 旁邊顯示 "Safe: X GB remaining"
- 計算：`avail_mb - borrow_mb` 再顯示

### Step 2.11: B2 — dirty_bg_ratio 上限
- `mb_to_dirty_ratio()` 已有 clamp 到 80
- 改 `new_bg = min(new_dirty/2, 40)` 避免 background 過高

---

## Phase 3: 編譯測試

### Step 3.1: 編譯
```bash
make clean && make 2>&1
```
確認零 warning。

### Step 3.2: CLI 測試
```bash
sudo ./tiered_setup --list
sudo ./tiered_setup --bench --disks sdb
sudo ./tiered_setup --create --name testvol --disks sdb:100,sdc:100 --fs ext4 --mount /mnt/test
sudo ./tiered_setup --status
sudo ./tiered_setup --destroy --name testvol
```

### Step 3.3: TUI 測試
```bash
sudo ./tiered_ui
```
走訪每個功能：Disk List → Benchmark → Create → Status → RAM Cache → Destroy
測試：B 鍵重跑 bench、Tab 切換、方向鍵游標、resize terminal

### Step 3.4: 推送
```bash
git add -A && git commit -m "v4: parallel bench, rollback, auto-detect, TUI improvements"
git push origin main
```
