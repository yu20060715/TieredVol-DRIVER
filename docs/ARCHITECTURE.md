# Architecture — 資料流與模組職責

## 整體資料流

```
Application
    │
    ▼
write() / read()         ← Standard POSIX I/O
    │
    ▼
VFS → bio                ← Kernel bio 產生
    │
    ▼
tieredvol_map()          ← DM target map function
    │
    ├─ [WRITE] ─→ tv_wc_try_buffer()
    │                ├─ 緩衝成功 → DM_MAPIO_SUBMITTED
    │                │       │
    │                │       ▼ (async)
    │                │   tv_wc_flush()
    │                │       ├─ cross-disk? → tv_parallel_submit()
    │                │       │                   └─ tv_mirror_handle()
    │                │       └─ single-disk → tv_mirror_handle()
    │                │                          └─ submit_bio()
    │                │
    │                └─ 緩衝失敗 → 進入 B path
    │                                ├─ cross-disk? → tv_parallel_submit()
    │                                │                   └─ tv_mirror_handle()
    │                                └─ single-disk → tv_mirror_handle()
    │                                                   └─ DM_MAPIO_REMAPPED
    │
    ├─ [READ] ──→ tv_wc_flush() (flush buffer for ordering)
    │               └─ tv_map_logical_*()
    │                   └─ submit_bio()
    │
    ▼
tieredvol_end_io()       ← DM 收到 bio 完成
    ├─ in_flight_bytes 遞減
    ├─ latency 記錄
    ├─ [error] → [mirror retry] → schedule_delayed_work()
    └─ [mirror success] → pending 清理
```

## I/O Path 詳解

### Write Path

1. **tieredvol_map()** 收到 WRITE bio
2. **tv_wc_try_buffer()** 嘗試緩衝（Write Coalescing）
   - 成功：bio 存入 WC list，回傳 `DM_MAPIO_SUBMITTED`
   - 失敗（kmalloc fail 或非 multi-disk segment）：fall through
3. **B path**（cross-disk boundary check）
   - 若 bio 橫跨多顆 disk → `tv_parallel_submit()` 拆分 clone 後分別 submit
   - 若在單顆 disk 內 → 設定 `bio_set_dev` + sector 後回傳 `DM_MAPIO_REMAPPED`
4. **Mirror**：在 submit 之前 call `tv_mirror_handle()`，clone bio 到 mirror disk
5. DM core 將 remapped bio 送到底層 device

### Read Path

1. **tieredvol_map()** 收到 READ bio
2. 先 flush WC buffer（確保 read 看到之前 buffered 的 write data）
3. **tv_map_logical_** 決定讀哪顆 disk
4. submit bio → DM core 送到目標 device
5. **tieredvol_end_io()** 收到完成通知
   - 讀取失敗 → 檢查是否有 mirror pending read → 有的話從 mirror disk retry
   - 讀取成功 → 清理 pending read 記錄

### Mirror Write Path

1. `tv_mirror_handle()` 收到原始 WRITE bio
2. `bio_alloc_clone()` clone bio 到 mirror disk
3. clone bio 的 sector = `(logical - seg->logical_begin) >> 9`
4. submit clone → 放入 pending-write 追蹤
5. 原始 bio 繼續走正常路徑
6. `tv_mirror_end_io()` 收到 clone 完成 → 清理 pending + 更新 mirror stats

## 模組職責

| 模組 | 職責 |
|------|------|
| `tieredvol_core.c` | DM lifecycle（ctr/dtr/map/status/init/exit），I/O 進入點 |
| `tieredvol_map.c` | Logical→Physical 映射（static / adaptive / random） |
| `tieredvol_stripe.c` | Stripe 邊界計算、範圍拆分、parallel submit |
| `tieredvol_mirror.c` | Mirror I/O、pending read/write 追蹤、retry、rebuild |
| `tieredvol_wc.c` | Write coalescing 緩衝與 flush |
| `tieredvol_meta.c` | Config file 讀寫、CRC32 驗證 |
| `tieredvol_log.c` | Log ring buffer、EMA decay timer |
| `tieredvol_message.c` | DM message dispatch（show/set 指令） |
| `tieredvol_sysfs.c` | sysfs 屬性介面 |

## 關鍵設計決策

- **WC buffer 在 stripe boundary 切割**：避免跨 stripe 的複雜度
- **Mirror 使用 bio_alloc_clone + fire-and-forget**：不 block 原始 bio
- **Pending tracking 使用 per-CPU ring buffer**：lockless write path
- **Mirror retry 使用 mempool + delayed work**：避免 OOM
- **Parallel submit 使用 tv_parallel_block**：所有 clone 完成後才 endio 原始 bio
