# Write Coalescing — 寫入合併

## 概述

Write Coalescing（WC）是 TieredVol 的寫入緩衝機制。將小寫入合併成較大的 chunk 再 flush，減少 I/O 次數並提高 throughput。

啟用方式：`wc_enabled=Y`（module param，預設開啟）。

## 架構

```
tieredvol_map() 收到 WRITE bio
    │
    ▼
tv_wc_try_buffer(ctx, bio, logical, cur)
    │
    ├─ [條件不符] → return -EAGAIN → 走直接路徑
    │
    └─ [緩衝成功] → return DM_MAPIO_SUBMITTED
         │
         ▼
    存入 ctx->wc.entries (linked list)
         │
         ▼
    accumulated += bio_size
         │
         ├─ accumulated ≥ stripe_size → 同步 flush
         └─ accumulated < stripe_size → schedule_delayed_work(1 jiffy)
```

## 緩衝條件

WC 只在以下條件成立時緩衝：

1. `wc_enabled = true`
2. `bio_data_dir() == WRITE`
3. segment 有效（`seg_idx` 在範圍內）
4. `seg->disk_count > 1`（單碟 segment 不走 WC）

## Stripe Boundary 切割

寫入若跨越 stripe 邊界，DM target 會先接收前半段（`dm_accept_partial_bio`），後半段由 DM core 重新 submit 處理。

```
bio: [===========] (跨 stripe)
        dm_accept_partial_bio
                     ↓
[====前半段====] + [====後半段====] (各自 tieredvol_map)
```

WC 只緩衝切割後的 bio，確保每筆 entry 都在同一 stripe 內。

## Flush 時機

| 條件 | 行為 |
|------|------|
| `accumulated ≥ stripe_size` | 立即同步 flush |
| 跨 striping buffer（新的 stripe_no） | 先 flush 舊 stripe 再 buffer 新的 |
| 1 jiffy 逾時 | 非同步 flush（`system_wq`） |
| 收到 READ bio | 先 flush 確保 read 看到最新資料 |
| device destroy | 強制 flush |

## Flush 流程

```
tv_wc_flush()
    │
    ▼
splice entries → batch list
    │
    ▼
for each entry:
    ├─ [cross-disk] → tv_mirror_handle() + tv_parallel_submit()
    └─ [single-disk] → tv_mirror_handle() + bio_set_dev + submit_bio()
```

Flush 時 mirror 會在 submit 前被 call，確保 mirror 資料與 primary 一致。

## 與 Mirror 的互動

- WC flush 同時處理 mirror（在 `tv_wc_flush()` 中 call `tv_mirror_handle()`）
- Mirror write 使用 `bio_alloc_clone`，與原始 bio 共享 page
- Parallel submit 路徑也在 submit 前補了 `tv_mirror_handle()`

## 參數

| module_param | 預設值 | 說明 |
|-------------|--------|------|
| `wc_enabled` | `Y` | 啟用 Write Coalescing |

執行時變更：

```bash
echo N | sudo tee /sys/module/tieredvol/parameters/wc_enabled
```
