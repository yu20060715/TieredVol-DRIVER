# TieredVol V5 實作步驟

## Phase 0: 共享頭文件

### Step 0.1: A6 — 建立 tiered_common.h
- 建 `src/tiered_common.h`，含 `tiered_is_valid_name()`、`tiered_is_valid_fs()`、`tiered_is_valid_mount()`
- 兩個 .c 檔都加 `#include "tiered_common.h"`

### Step 0.2: Makefile 更新
- 新增 `test_common` target
- 新增 `test` 聚合 target

---

## Phase 1: tiered_setup.c

### Step 1.1: A1+A3 — LVM_CFG + LVM config 統一
- 刪 `#define LVM_CFG ...`（line 66）
- 新增 `static const char *LVM_CONF = " --config '...'";`
- 替換 `cleanup_create()` 3 處、`cmd_create()` 3 處、`cmd_remove()` 3 處

### Step 1.2: A2 — make_target() helper
- 新增 `make_target()` 函式
- 替換 8 處 `"tv_%s_carve"` 拼接

### Step 1.3: A7 — 刪 MAX_NAME / MAX_PATH
- 刪 line 18-19

### Step 1.4: A6 — 引入 tiered_common.h
- 頂部加 `#include "tiered_common.h"`
- 刪 `is_valid_name()` 函式（line 28-36）
- `cmd_create()` 改用 `tiered_is_valid_name()` 和 `tiered_is_valid_fs()`
- `cmd_bench()` 改用 `tiered_is_valid_name()`

### Step 1.5: B1 — 合併 lsblk
- 新增 `disk_info_t` 結構 + `load_all_disk_info()` 函式
- 刪 `sysfs_tran()`（line 107-116）
- 刪 `is_root_disk()`（line 118-123）
- `cmd_list()` 改用 `load_all_disk_info()`
- `cmd_create()` 改用 `load_all_disk_info()` 檢查 is_root

### Step 1.6: B2 — cmd_create 並行 bench
- 提取 `bench_child_t` 結構為 static（line 368 附近）
- `cmd_create()` 的碟迴圈改為 fork 並行 + pipe 收集結果

### Step 1.7: B4 — orphan fallback 安全化
- `cmd_remove()` line 769-783：加 orphan 數量計算 + 3 秒警告

### Step 1.8: B5 — cmd_status 用 opendir
- `cmd_status()` line 813：`popen("ls ...")` 改為 `opendir("/dev/mapper")`

### Step 1.9: B6 — drop_caches 直接寫
- `bench_disk()` line 226：`system(...)` 改為 `open()+write()`

### Step 1.10: C2 — pragma 精準化
- line 13-14：從全域 ignore 改為 push/pop

### Step 1.11: D2 — safe_execvp()
- 新增 `safe_execvp()` 函式（fork + execvp）
- 替換 `cmd_create()` 的 pvcreate/vgcreate/lvcreate
- 替換 `cmd_remove()` 的 lvremove/vgremove/pvremove

### Step 1.12: D3 — snprintf 截斷檢查
- `sysfs_size_gb()`、`sysfs_model()`、`cmd_create()` 路徑拼接加截斷檢查

### Step 1.13: 升級 VERSION
- `#define VERSION "1.2.0"`

---

## Phase 2: tiered_ui.c

### Step 2.1: A6 — 引入 tiered_common.h
- 頂部加 `#include "tiered_common.h"`
- 刪 inline validation（line 869-882）改用 `tiered_is_valid_name()`、`tiered_is_valid_fs()`、`tiered_is_valid_mount()`

### Step 2.2: A4 — save/restore_speeds
- 新增 `saved_speed_w[MAX_DISKS]`、`saved_speed_r[MAX_DISKS]` + `save_speeds()` + `restore_speeds()`
- `screen_disk_list()` line 547-550 改用
- `screen_create_select_disks()` line 766-769 改用

### Step 2.3: A5 — drain_pipe_to_buf
- 新增 `drain_pipe_to_buf()` 函式
- `auto_bench_poll()` 兩處 read loop（line 256-262, 267-273）改用

### Step 2.4: B3 — Testing 狀態修復
- `screen_bench()` line 650-652：加 `"Started benchmark for /dev/%s"` 匹配

### Step 2.5: C1 — 拆分 screen_create_select_disks
- 拆成 `create_phase_select_disks()`、`create_phase_set_carves()`、`create_phase_input_settings()`
- 主函式變 coordinator（~30 行）

### Step 2.6: D1 — 終端最小尺寸防禦
- `main()` initscr() 後加 80x24 檢查
- 新增 `check_terminal_size()` 輔助函式
- 每個 `screen_*` 開頭呼叫

### Step 2.7: 升級 VERSION
- `#define VERSION "1.2.0"`

---

## Phase 3: 測試

### Step 3.1: 新增 test_common.c
- 測試 `tiered_is_valid_name()`（6 cases）
- 測試 `tiered_is_valid_fs()`（5 cases）
- 測試 `tiered_is_valid_mount()`（3 cases）

### Step 3.2: Makefile 更新
- 新增 `test_common` target
- 新增 `test` 聚合 target

---

## Phase 4: 編譯 + 測試

### Step 4.1: 編譯
```bash
make clean && make 2>&1
```

### Step 4.2: 單元測試
```bash
make test
```

### Step 4.3: CLI 測試
```bash
sudo ./tiered_setup --list
sudo ./tiered_setup --bench --disks sdb,sdc,nvme0n1
sudo ./tiered_setup --create --name testvol --disks sdb:100,sdc:100 --fs ext4 --mount /mnt/test
sudo ./tiered_setup --status
sudo ./tiered_setup --destroy --name testvol
```

### Step 4.4: TUI 測試
```bash
sudo ./tiered_ui
```
走訪：Disk List → Benchmark → Create → Status → RAM Cache → Destroy
驗證：B 鍵重跑、Testing 狀態、方向鍵、Tab、小視窗防禦

### Step 4.5: 推送
```bash
git add -A && git commit -m "v5: refactor redundancy, parallel create bench, terminal defense, safe_execvp"
git push origin main
```

### Step 4.6: REVIEW_V5.md
- 寫測試確認報告
