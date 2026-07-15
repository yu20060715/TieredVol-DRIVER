# TieredVol V5 重構計畫 — 冗餘消除 + 演算法加強 + 安全加固

> 目標：消除重複代碼、統一共享邏輯、加強效能演算法、修復 UX bug、加固安全性
> 版本：V5（基於 V4 1.1.0）

---

## 改動總表（19 項）

| 類別 | 編號 | 改動名稱 | 檔案 | 優先級 | 預估行數 |
|------|------|----------|------|--------|----------|
| **A. 冗餘消除** | A1 | 刪除未使用 LVM_CFG 宏 | tiered_setup.c | 🔴高 | -2 |
| | A2 | `make_target()` helper 統一 target 命名 | tiered_setup.c | 🔴高 | +10 |
| | A3 | `LVM_CONF` 常數統一 LVM config 字串 | tiered_setup.c | 🔴高 | +5 |
| | A4 | `save/restore_speeds()` helper | tiered_ui.c | 🟡中 | +15 |
| | A5 | `drain_pipe_to_buf()` helper | tiered_ui.c | 🟡中 | +10 |
| | A6 | `tiered_common.h` 共享驗證邏輯 | 新建 .h | 🟡中 | +40 |
| | A7 | 刪除 `MAX_NAME` / `MAX_PATH` | tiered_setup.c | 🟢低 | -2 |
| **B. 演算法** | B1 | 合併 lsblk 呼叫（tran + root 一次讀） | tiered_setup.c | 🔴高 | +30 |
| | B2 | `cmd_create()` 並行 bench | tiered_setup.c | 🟡中 | +40 |
| | B3 | 修復 "Testing..." 狀態永不顯示 | tiered_ui.c | 🔴高 | +5 |
| | B4 | orphan fallback 加安全警告 | tiered_setup.c | 🔴高 | +15 |
| | B5 | `cmd_status()` 用 `opendir()` 替代 `popen("ls")` | tiered_setup.c | 🟢低 | +15 |
| | B6 | `bench_disk()` drop_caches 直接寫檔 | tiered_setup.c | 🟢低 | +8 |
| **C. 品質** | C1 | 拆分 `screen_create_select_disks()` 三階段 | tiered_ui.c | 🟡中 | +20 |
| | C2 | `#pragma GCC` 改精準範圍 | tiered_setup.c | 🟡中 | +5 |
| **D. 安全加固** | D1 | 終端最小尺寸防禦 | tiered_ui.c | 🔴高 | +15 |
| | D2 | LVM 關鍵命令改 `safe_execvp()` | tiered_setup.c | 🟡中 | +50 |
| | D3 | `snprintf` 截斷檢查 | tiered_setup.c | 🟢低 | +20 |
| | D4 | `run_cmd()` 防禦 stdout 溢位 | tiered_ui.c | 🟢低 | +5 |

---

## Phase 規劃

| Phase | 內容 | 預估 |
|-------|------|------|
| 0 | 建立共享頭文件 + Makefile 更新 | +50 行 |
| 1 | tiered_setup.c 重構（A1-A3, A7, B1-B2, B4-B6, C2, D2-D3） | +160 行 |
| 2 | tiered_ui.c 重構（A4-A5, B3, C1, D1, D4） | +55 行 |
| 3 | 單元測試擴充 | +80 行 |
| 4 | 編譯 + 手動測試 + REVIEW_V5.md | 文檔 |

---

## 詳細設計

### Phase 0: 共享頭文件

#### A6: 建立 `src/tiered_common.h`

```c
#ifndef TIERED_COMMON_H
#define TIERED_COMMON_H

#include <string.h>

static inline int tiered_is_valid_name(const char *name) {
    if (!name || !*name) return 0;
    for (const char *p = name; *p; p++) {
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
              (*p >= '0' && *p <= '9') || *p == '.' || *p == '_' || *p == '-'))
            return 0;
    }
    return 1;
}

static inline int tiered_is_valid_fs(const char *fs) {
    const char *ok[] = {"ext4","ext3","xfs","btrfs","none",NULL};
    for (int i = 0; ok[i]; i++) if (strcmp(fs, ok[i]) == 0) return 1;
    return 0;
}

static inline int tiered_is_valid_mount(const char *mp) {
    return mp && mp[0] == '/';
}

#endif
```

- 兩個 .c 檔都 `#include "tiered_common.h"`
- 刪除各自內聯的驗證邏輯

---

### Phase 1: tiered_setup.c 重構

#### A1 + A3: LVM_CFG 宏 + LVM config 字串統一

**問題：** line 66 定義了 `LVM_CFG` 宏但從未使用。6 處 LVM 呼叫手寫 config 字串。

**改動：**
- 刪 `#define LVM_CFG ...`（line 66）
- 新增 `static const char *LVM_CONF = " --config 'devices{scan=[\"/dev/mapper\"] obtain_device_list_from_udev=0}'";`
- 替換 6 處手寫 config 為 `LVM_CONF`

**影響：** `cleanup_create()`、`cmd_create()`、`cmd_remove()` 各有 2-3 處

#### A2: `make_target()` helper

```c
static void make_target(char *out, size_t sz, const char *disk) {
    snprintf(out, sz, "tv_%s_carve", disk);
}
```

**影響：** 8 處 `snprintf(target, ..., "tv_%s_carve", disk)` 替換

#### A7: 刪除未使用的宏

```c
// 刪除:
#define MAX_NAME 64
#define MAX_PATH 512
```

#### B1: 合併 lsblk 呼叫

**問題：** `sysfs_tran()` 每碟 fork 一次 lsblk。`is_root_disk()` 每碟 fork 一次 lsblk。3 颗碟 = 6 次 fork。

**改動：** 新增 `load_all_disk_info()` 一次性讀取所有碟的 tran + root 狀態。

```c
typedef struct {
    char name[32];
    char tran[16];
    int is_root;
} disk_info_t;

static int load_all_disk_info(disk_info_t *out, int max);
```

- 刪 `sysfs_tran()` 和 `is_root_disk()`
- `cmd_list()` 改用 `load_all_disk_info()` 查 tran
- `cmd_create()` 改用 `load_all_disk_info()` 查 root 狀態

#### B2: `cmd_create()` 並行 bench

**問題：** 依序 bench 每碟 45s，3 颗碟 = 135s。

**改動：** 提取 `bench_child_t` 結構為全域定義，`cmd_create()` 用 fork 並行。

#### B4: orphan fallback 安全化

**問題：** config 文件缺失時，掃描刪除**所有** `tv_*_carve` target。

**改動：** 加 3 秒警告等待 + orphan 數量提示。

#### B5: `cmd_status()` 用 `opendir()`

**改動：** 替代 `popen("ls /dev/mapper/tv_*")`。

#### B6: drop_caches 直接寫檔

**改動：** `open("/proc/sys/vm/drop_caches", O_WRONLY)` + `write()`，替代 `system("sudo sh -c 'echo 3 > ...'")`。

#### C2: `#pragma` 改精準範圍

**改動：** 從 line 13-14 的全域 ignore 改為 push/pop 只包住 snprintf truncation 的函式。

#### D2: `safe_execvp()` 封裝

**問題：** `system()` 經過 shell 解析，即使有白名單仍有風險。

**改動：** 新增 `safe_execvp()` 用 `fork()+execvp()` 直接執行，參數以 `char*[]` 傳遞。

```c
static int safe_execvp(const char *path, char *const argv[]) {
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) { execvp(path, argv); _exit(127); }
    int status;
    waitpid(pid, &status, 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}
```

**影響：** LVM 關鍵路徑（pvcreate/vgcreate/lvcreate/lvremove/vgremove/pvremove）改用此函式。`mkfs.*` 和 `mount` 保留 `system()`（參數複雜度低，且需 root）。

#### D3: `snprintf` 截斷檢查

**改動：** 在所有路徑拼接（`sysfs_*`、`cmd_create`、`cmd_remove`）的 `snprintf` 後檢查回傳值，截斷時 return error。

---

### Phase 2: tiered_ui.c 重構

#### A4: `save/restore_speeds()` helper

```c
static double saved_speed_w[MAX_DISKS], saved_speed_r[MAX_DISKS];
static void save_speeds(void);
static void restore_speeds(void);
```

**影響：** `screen_disk_list()`、`screen_create_select_disks()` 兩處替換。

#### A5: `drain_pipe_to_buf()` helper

**影響：** `auto_bench_poll()` 兩處 read loop 替換。

#### B3: 修復 "Testing..." 狀態

**問題：** `screen_bench()` line 651 搜 `"Testing /dev/"` 但並行模式輸出 `"Started benchmark for"`。

**改動：** 加第二個匹配 `"Started benchmark for /dev/%s"`。

#### C1: 拆分 `screen_create_select_disks()`

**改動：** 拆成 3 個子函式 + 1 個 coordinator。

#### D1: 終端最小尺寸防禦

**問題：** 終端 < 80x24 時畫面重疊、閃退。

**改動：** `main()` 啟動時檢查 `getmaxyx()`，小於 80x24 則印提示並 exit。每次畫面重繪前也檢查，過小則顯示 "請放大視窗"。

```c
// main() initscr() 之後：
int maxy, maxx;
getmaxyx(stdscr, maxy, maxx);
if (maxy < 20 || maxx < 60) {
    endwin();
    fprintf(stderr, "Error: terminal too small (%dx%d). Minimum: 80x24\n", maxx, maxy);
    return 1;
}

// 每個 screen_* 開頭加：
static int check_terminal_size(void) {
    int maxy, maxx;
    getmaxyx(stdscr, maxy, maxx);
    if (maxy < 20 || maxx < 60) {
        clear();
        mvprintw(maxy/2 - 1, (maxx - 36) / 2, "Terminal too small (%dx%d)", maxx, maxy);
        mvprintw(maxy/2 + 1, (maxx - 36) / 2, "Minimum: 80x24 — please resize");
        draw_status_bar("Resize terminal or press Q to exit", NULL);
        refresh();
        return 0;
    }
    return 1;
}
```

#### D4: `run_cmd()` 防禦 stdout 溢位

**改動：** 用 `fread` 的回傳值 `n` 確保不超出 `outsize-1`。目前已有此邏輯（line 111），確認無問題。

---

### Phase 3: 測試

#### 新增 `tests/test_common.c`

- `tiered_is_valid_name()`：正常名、空名、特殊字元、注入字串
- `tiered_is_valid_fs()`：合法/非法格式、注入字串
- `tiered_is_valid_mount()`：合法/相對路徑

#### Makefile 新增 targets

```makefile
test_common: tests/test_common.c src/tiered_common.h
	$(CC) -Wall -Wextra -std=gnu11 -O2 -o $@ $<

test_tui: tests/test_tui.c
	$(CC) -Wall -Wextra -std=gnu11 -O2 -o $@ $< -lm

test: test_tui test_common
	./test_tui && ./test_common
```

---

### Phase 4: 編譯 + 測試 + 文檔

- `make clean && make` 零 warning
- `make test` 全部 pass
- CLI 手動：--list → --bench → --create → --status → --destroy
- TUI 手動：B 鍵重跑、Testing 狀態、方向鍵、Tab、小視窗防禦
- 版本升至 `1.2.0`

---

## 預估最終行數

| 檔案 | V4 | V5 | 變化 |
|------|-----|-----|------|
| tiered_setup.c | 878 | ~1030 | +152 |
| tiered_ui.c | 1249 | ~1295 | +46 |
| tiered_common.h | 0 | ~30 | +30 |
| test_tui.c | 207 | ~210 | +3 |
| test_common.c | 0 | ~60 | +60 |
| **合計** | **2334** | **~2625** | **+291** |
