# TieredVol — Code Fix AGENTS

> 執行方式：在 TieredVol 目錄下啟動 opencode，然後把 batch 內容貼進去
> 每個 batch 做完跑 `make test` 確認全 PASS 再往下

---

## Batch 1：Easy（4 項）

把以下 prompt 完整貼進 opencode：

```
幫我做以下 4 個小修，每改完一個檔案就存檔，最後跑 make test。

1. tests/test_common.c — 刪除 lines 5-16 的 static int tests_run / tests_passed / check() 定義，
   改為 #include "test_common.h"。確保該檔案的 main() 和 print_summary() 仍正常運作。

2. .gitignore — 在最後補五行（每行一個）：
   test_mapper
   test_partition
   test_metadata
   test_sched
   test_integrity

3. src/io_bench.c — 找到 line 110 附近的 malloc呼叫（uint8_t *buf = malloc(...)），
   改為 posix_memalign(&buf, 4096, size)。errno 判斷回傳值。
   如果 malloc 原本沒有 NULL check，也要補上。

4. Makefile — 找到依賴 tiered_sched.h 的 .o target rules（tiered_partition.o、
   tiered_mapper.o、tiered_io_uring.o、tiered_metadata.o、tiered_benchmark.o），
   把依賴改為tiered_types.h（tiered_io_uring.o 改為 tiered_io_uring.h + tiered_types.h）。
   tiered_benchmark.o 如果還 include warmup.h 也要補上。

做完跑 make clean && make test，全部 PASS 才算完成。
```

---

## Batch 2：Medium（6 項）

```
幫我做以下 6 個修改。每改完一個檔案就存檔，全部完成後跑 make test。

5. src/cmd_create.c + src/cmd_remove.c — 找到所有 tv_exec_sudo() 呼叫。
   函式簽名是 int tv_exec_sudo(char *const argv[], int quiet)。
   如果呼叫端只傳了一個參數（缺 quiet），在最後補 , 0。
   共 12 處需要修：cmd_create.c 約 4 處、cmd_remove.c 約 8 處。

6. _GNU_SOURCE 統一到 Makefile — 從以下 9 個 .c 檔案的第一行刪除 #define _GNU_SOURCE：
   tiered_io.c, tiered_setup.c, cmd_create.c, cmd_remove.c, io_bench.c,
   setup_bench.c, setup_discover.c, tiered_benchmark.c, test_integrity.c
   然後在 Makefile 的 CFLAGS 行加上 -D_GNU_SOURCE（加在 -Wall 前面即可）。

7. 把 src/tiered_setup.c rename 為 src/main.c：
   在檔案系統中執行 rename（git mv 或 PowerShell Move-Item）。
   然後在 Makefile 中把 OBJS 裡的 tiered_setup.o 改為 main.o。

8. tests/test_sched.c — 在檔案末尾的 main() 裡（print_summary 之前）新增兩個 test：

   void test_double_free(void) {
       TV_METADATA meta = {0};
       meta.segment_count = 1;
       meta.segments[0].disk_count = 1;
       meta.segments[0].disk_index[0] = 0;
       meta.segments[0].weight[0] = 1;
       meta.segments[0].stripe_size = 1048576;
       meta.segments[0].logical_begin = 0;
       meta.segments[0].logical_end = 1048576;
       strcpy(meta.disk_names[0], "/dev/null");
       TV_SCHED *sched = tv_sched_init(&meta);
       if (sched) {
           tv_sched_destroy(sched);
           tv_sched_destroy(sched);  // 應安全忽略，不 crash
           check(1, "double_free_no_crash");
       } else {
           check(1, "double_free_skip");
       }
   }

   void test_write_after_destroy(void) {
       TV_METADATA meta = {0};
       meta.segment_count = 1;
       meta.segments[0].disk_count = 1;
       meta.segments[0].disk_index[0] = 0;
       meta.segments[0].weight[0] = 1;
       meta.segments[0].stripe_size = 1048576;
       meta.segments[0].logical_begin = 0;
       meta.segments[0].logical_end = 1048576;
       strcpy(meta.disk_names[0], "/dev/null");
       TV_SCHED *sched = tv_sched_init(&meta);
       if (sched) {
           tv_sched_destroy(sched);
           uint8_t buf[4] = {0};
           int ret = tv_write(sched, buf, 0, 4);
           check(ret != 0 || sched == NULL, "write_after_destroy_returns_error");
       } else {
           check(1, "write_after_destroy_skip");
       }
   }

   在 main() 裡 test_seek_rejects_unaligned 之後、print_summary 之前呼叫：
   test_double_free();
   test_write_after_destroy();

9. src/io_bench.c — 找到 cmd_bench_path() 函式裡的 inline warmup loop
   （大約在 line 410-431，有 while (warmup_written < warmup_size) 的那個 loop）。
   把整個 while loop 替換為一行呼叫：
       tv_warmup_device(path, warmup_size);
   確認 warmup.h 已被 include（io_bench.c 頂部）。

10. src/tiered_benchmark.c — 找到 tv_benchmark() 函式裡的 inline warmup loop
    （大約在 line 51-68，有 while (written < target) 的那個 loop）。
    把整個 while loop 替換為一行呼叫：
        tv_warmup_device(path, target);
    確認 warmup.h 已被 include。

做完跑 make clean && make test，全部 PASS 才算完成。
```

---

## Batch 3：Hard（2 項）

```
幫我做以下修改：

11. Magic numbers 命名 — 在 src/tiered_types.h 裡新增以下 #define（加在現有 constants 之後）：
    #define TV_URING_QUEUE_DEPTH    256
    #define TV_ALLOC_ALIGNMENT      512
    #define TV_DIRECT_ALIGNMENT     4096
    #define TV_CONFIG_DIR           "/etc/tieredvol/"
    #define TV_PROGRESS_INTERVAL    (64 * 1024 * 1024)

    然後在對應的 .c 檔案中把 literal 值替換為常數名：
    - tiered_sched.c: aligned_alloc(512, ...) → aligned_alloc(TV_ALLOC_ALIGNMENT, ...)
    - tiered_sched.c: tv_uring_init(&sched->ring, 256) → tv_uring_init(&sched->ring, TV_URING_QUEUE_DEPTH)
    - tiered_io.c: posix_memalign(&buf, 4096, ...) → posix_memalign(&buf, TV_DIRECT_ALIGNMENT, ...)
    - tiered_io.c: posix_memalign(&buf, 512, ...) → posix_memalign(&buf, TV_ALLOC_ALIGNMENT, ...)
    - tiered_setup.c 或 cmd_create.c: "/etc/tieredvol/" → TV_CONFIG_DIR
    - io_bench.c: if (written % (64 * 1024 * 1024) == 0) → if (written % TV_PROGRESS_INTERVAL == 0)

    注意：不要改動 /proc 或 /sys 的路徑字串，那些是 kernel interface 不是 magic number。
    不要改動 test files 裡的 magic numbers（test 的 literal 值是故意的）。

12. Makefile test target — 找到 test: target 的 recipe（跑 test_common, test_mapper 等的那段），
    在最後一行（test_sched 之後）加上：
        @echo "=== test_integrity ===" && ./test_integrity
    這個 test 不需要 block device，init/destroy 測試不需要 device 參數。

做完跑 make clean && make test && make test-full
（test-full 需要 losetup，如果沒有 root 權限就只跑 make test）。
```

---

## 完成後檢查清單

全部做完後跑這個 prompt 來驗收：

```
幫我驗收以下 12 項，逐項回覆 PASS 或 FAIL：

C1. tests/test_common.c 是否 #include "test_common.h" 而非自己定義 check()？
C2. .gitignore 是否包含 test_mapper, test_partition, test_metadata, test_sched, test_integrity？
C3. src/io_bench.c line ~110 是否使用 posix_memalign 而非 malloc？
C4. Makefile 的 .o target 依賴是否引用 tiered_types.h（而非 tiered_sched.h）？
C5. cmd_create.c + cmd_remove.c 所有 tv_exec_sudo 呼叫是否有 2 個參數？
C6. 9 個 .c 檔案第一行是否無 #define _GNU_SOURCE？Makefile CFLAGS 是否有 -D_GNU_SOURCE？
C7. src/tiered_setup.c 是否已 rename 為 src/main.c？Makefile OBJS 是否對應？
C8. tests/test_sched.c 是否有 test_double_free 和 test_write_after_destroy？
C9. src/io_bench.c cmd_bench_path() 是否呼叫 tv_warmup_device() 而非 inline loop？
C10. src/tiered_benchmark.c tv_benchmark() 是否呼叫 tv_warmup_device() 而非 inline loop？
C11. src/tiered_types.h 是否有 TV_URING_QUEUE_DEPTH, TV_ALLOC_ALIGNMENT 等常數？
C12. Makefile test target 是否包含 ./test_integrity？

回覆格式：每項一行 "C1: PASS ✅" 或 "C1: FAIL ❌ — 具體原因"
```

---

## 注意事項

- C7（rename）做完後，如果 git 有 tracking，用 `git mv src/tiered_setup.c src/main.c` 而非 PowerShell Move-Item，這樣 git 歷史才不會斷
- C9 的 io_bench.c warmup 替換前，先確認 `tv_warmup_device` 的 signature 是 `int tv_warmup_device(const char *path, uint64_t target_bytes)`，參數順序對得上
- C10 的 tiered_benchmark.c warmup 替換前，確認呼叫端的 `path` 和 `target` 變數名跟 `tv_warmup_device` 參數對得上
- C5 的 12 處呼叫，quiet 參數一律傳 0（非 quiet 模式），因為原本這些呼叫的 stderr 都沒有被 redirect
