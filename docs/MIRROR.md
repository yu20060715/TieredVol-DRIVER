# Mirror — RAID1 實作

## 概述

TieredVol 支援 per-segment mirror（RAID1）。每個 segment 可指定一顆 mirror disk，寫入時 clone bio 同時寫到 mirror disk，讀取時若 primary disk 錯誤則從 mirror retry。

## Config

```
seg0_mirror=2   # disk index 2 是 mirror
```

可對不同 segment 指定不同 mirror disk。mirror disk 可以是未被 primary 使用的備用碟。

## Write Mirror

### 流程

```
原始 WRITE bio 到達 tieredvol_map() / tv_wc_flush()
    │
    ▼
tv_mirror_handle(ctx, bio, cur, logical)
    ├─ mirror_enabled? → yes
    ├─ mirror_disk == cur.disk? → no（不 mirror 到同一顆）
    │
    ▼
bio_alloc_clone(mirror_bdev, bio, GFP_NOIO, &fs_bio_set)
    │
    ▼
clone->bi_iter.bi_sector = (logical - seg->logical_begin) >> 9
pwc = mempool_alloc(mirror_pw_pool, GFP_NOIO)
pwc->ctx = ctx, pwc->bdev = mirror_bdev, pwc->sector, pwc->size
clone->bi_private = pwc
clone->bi_end_io = tv_mirror_end_io
    │
    ▼
tv_pw_add()    ← 記錄 pending write（給 read retry 判斷用）
submit_bio(clone)
```

### Completion

`tv_mirror_end_io()`：
- 成功 → `mirror_write_ops++`
- 失敗 → `mirror_errors++`
- 清理 pending write → free mempool → bio_put

### 注意

- Mirror 使用 `bio_alloc_clone`，與原始 bio 共享 page reference
- 使用 `mempool_alloc` 確保不 OOM（pool size = 128）
- Mirror write 不 block 原始 bio，fire-and-forget
- 原始 bio 仍走原本的 weighted striping 路徑

## Read Retry

當 primary disk 讀取失敗時，若該位置有 mirror write pending（尚未完成），則等待最多 32 次 retry（每 1ms），然後從 mirror disk 重新讀取。

```
tieredvol_end_io()
    ├─ bio->bi_status != OK
    ├─ bio_data_dir() == READ
    │
    ▼
tv_pending_find_and_remove()    ← 檢查是否有 pending read
    │
    ▼
mempool_alloc(retry_ctx_pool)
    │
    ▼
schedule_delayed_work(retry_dwork, 0)
    │
    ▼
tv_read_retry_work()
    ├─ tv_pw_is_pending()? → yes: 等 1ms，retry--
    │    until retries==0 → give up
    └─ no: bio_alloc_clone(mirror_bdev) → submit_bio
```

### Pending Read 追蹤

使用 per-CPU ring buffer（lockless）：
- `tv_pending_add()`：每次 READ bio submit 時記錄
- `tv_pending_find_and_remove()`：錯誤發生時查詢並移除

## Rebuild

當 mirror disk 被新加入或修復後，需要將 primary 的資料完整複製到 mirror：

```
tv_rebuild_thread()
    for offset in 0..seg_size, step=chunk_size:
        bio_read(primary, offset) → wait
        bio_write(mirror, offset) → wait
```

- 使用同步 I/O（bio_alloc + submit + wait_for_completion）
- backoff 機制：失敗時 10ms → 20ms → 40ms → ... → 1000ms
- 每 10MB 回報進度百分比
- rebuild 可透過 message 命令觸發

## Stats

| Counter | 欄位 | 說明 |
|---------|------|------|
| mirror_write_ops | mirror_wr ops | Mirror write 成功次數 |
| mirror_write_bytes | mirror_wr bytes | Mirror write 成功位元組 |
| mirror_read_ops | mirror_rd ops | Mirror read retry 成功次數 |
| mirror_errors | mirror_err | Mirror write/read 失敗次數 |

查看方式：

```bash
sudo dmsetup message <name> 0 show_mirror
sudo dmsetup status <name>
```

## 限制

- Per-segment mirror（每 segment 最多一個 mirror disk）
- Mirror disk 不可與 segment 的任一 primary disk 相同
- 不支援 multi-mirror（RAID1 三份以上）
- Mirror write 不經過 WC buffer（直接 submit）
