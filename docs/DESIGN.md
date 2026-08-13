# DESIGN — 設計本質

> 本文回答「這個 driver 為什麼這樣寫」，以及「改什麼會踩到承重牆」。
> 修改原始碼前，先讀完本篇。亂改 code 造成的 bug 幾乎都來自違反下列不變式。

## 1. 核心哲學：靜態、確定性、O(1) 純算術映射

整個 driver 的地基是一張「地圖」：logical → (disk, physical offset)。

`tv_map_logical()`（`driver/tieredvol_map.c`）用純指標運算計算，沒有 mapping table：

```
stripe_no  = (L - seg_begin) / stripe_size
offset_in  = (L - seg_begin) % stripe_size
boundary[i] = Σ_{j<i} weight[j] * chunk_size        ← 累積邊界
選碟：boundary[i] <= offset_in < boundary[i+1]
offset = stripe_no * weight[i] * chunk_size + (offset_in - boundary[i])
```

**為什麼這樣寫：**

- 映射**無狀態**：只依賴 `ctx->meta`（一次 ctr() 載入，之後只會被 dmsetup message 改動）。
- **無鎖、任何 context 可呼叫**：hardirq、io_uring worker、kthread 都能安全算。
- 沒有 rebuild mapping、沒有 on-disk 索引、IO 熱路徑上零共享 spinlock。

**不變式（動它 = 出事）：**

- segment 必須按 `logical_begin` 排序（ctr 驗證）。
- 每個 segment 的 `stripe_size` 必須 = `Σ weight[i] * chunk_size`。
- `map.length` 與 `map.offset` 的計算公式不能單獨改一邊。

## 2. DM contract 主導一切

map() 只有兩種回傳：

| 回傳值 | 意義 |
|--------|------|
| `DM_MAPIO_REMAPPED` | 已改好 `bi_bdev` + `bi_sector`，交給 DM 提交 |
| `DM_MAPIO_SUBMITTED` | driver 已自己處理（clone 提交 / 直接完成） |

跨碟區的 bio 靠 `dm_accept_partial_bio(bio, len)` 把「塞得進目前碟區的部分」還給 DM，
DM 會再對剩餘部分重進 map()。**跨碟拆分基本由 DM 做，driver 不自己切讀 bio。**

配套：

- `dm_set_target_max_io_len(stripe_sectors)` → 進 map() 的 bio 最多一個 stripe 長。
- `io_hints`：`chunk_sectors = stripe`、`io_min = min(weight*chunk)`、`io_opt = stripe`。
- `num_flush_bios = ndisks`、`flush_bypasses_map = true`。

**為什麼讀路徑沒有 parallel：** 讀拆分便宜（DM 重進 map 即可），平行拆分只為寫吞吐而存在。

## 3. 平行寫路徑（B）— 全 driver 最微妙的地方

當 **WRITE** bio 在一個 stripe 內跨多碟區（`tv_stripe_calc_boundaries` 算出 `li - fi + 1 > 1`）時，
`tv_parallel_submit()` 把 bio clone 成 per-disk sub-bio 平行提交：

- `bio_alloc_clone()` + `bio_advance()` 定位、`submit_bio()` 平行發。
- 完成語義：`atomic pending` + `kref` + `cmpxchg(completed, 0, 1)` → **orig_bio 恰好完成一次**。
- 最後一個 sub-bio 完成（hardirq 內）：用 `del_timer()`，**絕不能用 `del_timer_sync()`**
  （會在 hardirq 自旋等 timeout callback → 死鎖）。
- timer timeout：`kref_get_unless_zero()` 保活 block，仍 pending 則把碟標 DEGRADED，`cmpxchg` 完成 orig_bio。

**重要特性：平行路徑是 driver 全權持有的路徑，繞過 DM 的 end_io。**
因此平行寫的 sub-bio 錯誤**不會**走 `tieredvol_end_io` 的 degradation/error_count 統計，
只有 timeout 會標記碟 DEGRADED。這是設計特性，不是 bug——改它之前先想清楚。

**不變式：**

- `completed` 只允許 0→1 一次；`pending` 遞減到 0 的那一方負責 endio + del_timer + kref_put。
- sub-bio 的 `bi_end_io` 必須是 `tv_parallel_end_io`，不能掛其他 end_io。

## 4. 兩種 policy + weight-borrowing

| policy | 說明 |
|--------|------|
| `static` (0) | 第 1 節的確定性地圖。**生產/基準測試用它**（tv3x/tv4x 都是）。 |
| `random` (2) | 純測試用，隨機選碟。 |

> **`adaptive` (1) 已移除**（2026-08-13，commit a48bba5 前之 dead code 清理時刪除）：
> EMA 三因子計分會破壞位址確定性且實測失衡（RESULTS.md 結論 #12）。
> 慢碟 offload 改由 **weight-borrowing** 承擔。

**不變式：** 平行拆分的 `tv_stripe_calc_boundaries` 以靜態佈局為前提，
`random` 與 static 混用會讓權重計數與佈局不一致（僅測試用）。

### Weight-borrowing（`driver/tieredvol_borrow.c`）
- 慢碟 in-flight ≥ watermark 且整 block 對齊 → 重導至最少負載碟（排除
  degraded/無空間）的 over-provisioned 借用區，per-block 表記錄。
- **一致性不變式**：`borrow_off` 只停新借；lookup 與 need==0 的重寫
  永遠解析至借用區（不檢查 enabled），故已借 block 的讀寫不受開關影響。
- 借用區配置是 all-or-none（整 block），避免部分配置造成表不一致。
- 表在 remove 時存 `<config>.borrow`、重載時載入，跨 reload 位址一致。
- 選碟計數沿用 `tv_io_stats.in_flight_bytes`（無 EMA/decay timer）。

## 5. WC / Mirror / Badmap / Config 模式

### WC（`driver/tieredvol_wc.c`）
- 以 `(segment, stripe_no)` 群組緩衝 WRITE bio，累積 ≥ stripe_size、換 stripe、或**讀**時才 flush。
- **讀必先 flush**（`bio_data_dir != WRITE` 時 `tv_wc_flush()`）——這是讀序保證的承重牆。
- flush 時重跑 B 邏輯（跨碟 → parallel，否則直接 submit）。
- 啟用由 `wc_enabled` module param 控制。

### Mirror（`driver/tieredvol_mirror.c`）
- 寫：fire-and-forget clone 到 mirror 碟（`mirror_sec = logical - seg_begin`），wa=1。
- 讀：per-CPU lockless pending ring 記錄「正在讀哪個 range」；讀錯誤時等 mirror 寫排空
  （`tv_pw_is_pending`，最多 32 次退讓）後從 mirror 重試。
- rebuild：kthread 依 static map 逐 chunk 讀資料碟 → 寫 mirror。
- 限制：mirror 碟不能是該 segment 的 stripe 參與者；一個 segment 只能一個 mirror。
- 熱路徑零共享 lock（per-CPU ring + atomic）。

### Badmap（`driver/tieredvol_badmap.c`）
- per-disk chunk bitmap（`n_chunks = disk_sectors / chunk`）。
- 壞塊讀 → `zero_fill_bio` 直接完成（讀作零）；壞塊寫 → 直接 `bio_endio` 跳過。
- WRITE 錯誤在 `tieredvol_end_io` 自動標壞 chunk。
- 以 range 字串持久化進 config（`badmap_<disk>=a-b,c`），kernel save 時壓縮寫回。

### Config（`driver/tieredvol_meta.c` + `common/tieredvol_meta_format.h`）
- **單一真相來源**：`/etc/tieredvol/<name>.conf` INI 檔。
- kernel 載入驗 **CRC32C**；kernel save 先寫 `.bak`（userspace parser 已隨舊 prototype `src/` 移除）。
- dm table 本身極簡：`0 <sectors> tieredvol <config_path>`——佈局全部在 config 裡。
- **不變式：改 config 格式 = 改 kernel parser + CRC 演算法，兩處同步。**

## 6. 為什麼亂改會出 bug（承重牆清單）

| 承重牆 | 違反後果 |
|--------|----------|
| parallel 完成語義（kref/timer/`cmpxchg(completed,0,1)`、hardirq 內 `del_timer`） | 雙完成 / 漏完成 / 死鎖 / UAF |
| boundary 算術與不變式（segment 排序、stripe=Σweight、max_io_len ≤ stripe） | 映射錯位、資料寫錯碟 |
| WC 讀序（read 必先 flush） | 讀到舊資料 |
| mirror 寫 fire-and-forget + pending ring 的生命週期 | mirror 資料不一致 / ring 溢出 |
| config kernel parser + CRC | 載入失敗、靜默損壞 |
| borrow 表一致性（off 仍解析、all-or-none 配置、.borrow 存/載） | 資料讀寫錯碟 / 跨 reload 位址漂移 |

## 修改規則

1. 先講清楚「改哪個不變式、為什麼可以改」，再動 code。
2. 動平行路徑或完成語義，必須解釋與 kref/timer 的互動。
3. 改 config 格式：kernel parser、CRC 兩處同步。
4. 加測試：dmsetup 計數器（rd=/wr= ops/bytes）能驗證分布是否精確。
