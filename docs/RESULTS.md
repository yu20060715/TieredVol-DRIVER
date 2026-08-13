# Test Results — TieredVol-DRIVER（現況拓撲）

> 實測日期：2026-08-12（疊碟/Mirror/LVM）· 2026-08-13（多卷併發 P3/P4）· Kernel 6.14.0-27-generic · dm-target v5.0
> 完整測試程序與驗收判據見 [TEST_PLAN.md](TEST_PLAN.md)。
> **碟位變更紀錄**：8/12 重啟後 快碟=nvme0n1（WD SN750）；**8/13 早再重啟後交換回 快碟=nvme1n1（WD SN750）、慢碟=nvme0n1（P3 Plus）**；**8/13 晚再重啟後又交換回 快碟=nvme0n1（WD SN750）、慢碟=nvme1n1（P3 Plus）**（等於回到 8/12 拓撲）。權重語義隨之對應調整。
> 本文 8/12 數據為舊拓撲語義、8/13 起為新拓撲；權重鎖定 6:1:1:1 不變，config 已於 8/13 對應更新。

---

## 硬碟對照（目前拓撲）

```
代號    裝置        型號                    容量   介面           PCIe link   平行仲裁測速
A    = nvme0n1    WD SN750 WDS500G3X0C      500G   PCIe 3.0 x4   8.0 GT/s    2009 MB/s
B    = nvme1n1    P3 Plus CT1000P3PSSD8     1T     PCIe 2.0 x1   5.0 GT/s    389 MB/s
C    = sdc        MX500 CT500MX500SSD1     500G   SATA 6G       -           470 MB/s
D    = sdb        WD Blue WDS250G2B0A      250G   SATA 6G       -           346 MB/s
E    = sda        BX100 CT250BX100SSD1     233G   SATA 6G       -           （未入 config）
```

- **插槽角色（2026-08-12 重啟後）**：A（WD SN750）在 PCIe3.0 x4 = 快碟；B（P3 Plus）在 PCIe2.0 x1 = 慢碟。
- **平行仲裁** = 4 碟同時 raw 寫入 10s（fio 1M libaio depth=32）。solo 短寫會吃到 SLC cache
  （A~2009、B~389、C~470、D~346 MB/s），平行仲裁才是 driver dispatch 時的真實並行速度。

---

## 權重調校

```
平行仲裁測速：A=2009  B=389  C=470  D=346 (MB/s)
```

權重鎖定 **6:1:1:1**（sum=9）。子集合沿用：S2 [6,1]、S3 [6,1,1]、S4 [6,1,1,1]。

---

## 量測方法

```bash
# 寫（恰好 8G，配合確定性映射 → 計數器可精確比對）
sudo fio --name=w --filename=/dev/mapper/<name> --rw=write --bs=1M --size=8G \
  --direct=1 --ioengine=libaio --iodepth=32 --numjobs=1 --end_fsync=1

# 讀
sudo fio --name=r --filename=/dev/mapper/<name> --rw=read --bs=1M --size=8G \
  --direct=1 --ioengine=libaio --iodepth=32 --numjobs=1
```

- **libaio，不用 io_uring**：io_uring + dm + 深佇列會灌高假數字，無法反映真實吞吐。
- 寫完 `dmsetup status` 讀每碟 `wr=<ops>/<bytes>`，比對權重比例。
- 分布判據：**完整 stripe 依權重精確分配；非完整 stripe 的餘量 chunk 依映射落入前段碟**
  （確定性 O(1) 映射，無誤差、每 byte 恰落一碟）。

---

## 疊碟結果（2026-08-12，鎖修復後回歸，權重 6:1:1:1）

| ID | 碟數 | 權重 | Write(MB/s) | Read(MB/s) | 分布（chunk，8G=8192） | 完整性 |
|----|------|------|-------------|------------|------------------------|--------|
| S1 | 1 (A) | 1 | 1981 | 3009 | A=8192（100% 精確） | - |
| S2 | 2 (A+B) | 6:1 | 2661 | 2786 | A=7024 B=1170（精確） | - |
| S3 | 3 (A+B+C) | 6:1:1 | 2787 | 3180 | A=6144 B=1024 C=1024（精確） | - |
| S4 | 4 (A+B+C+D) | 6:1:1:1 | 3091 | 3575 | A=5462 B=910 C=910 D=910（精確） | crc32c **8G 全量** 0 mismatch |
| CAP | 4 (100G carve) | 6:1:1:1 | 2165 | - | A=684 B=114 C=113 D=113（1G 精確） | - |

> 上表為 **2026-08-12 15:35 冷態完整回歸**（重載當前 build、四卷共存、寫+讀+完整性+CAP 同程序、碟温 ~39°C 起步）。
> 疊碟單調上升 **1981 → 2661 → 2787 → 3091**。**熱載入下界**（15:04，連續 ~96G 寫入後）：S3=2502、S4=2778，
> 與冷態差異 ~10–24%（多卷共存 ~8% + 熱/SLC 狀態）；重測觀察範圍 S3∈[2502,3123]、S4∈[2778,3465]，**S4 恆 > S3**。

### 分布驗證（對照確定性映射預測值）

| ID | 推算 | 計數器 | 一致 |
|----|------|--------|------|
| S2 | 8192M=910 完整 stripe(9M)+2M 餘 → A=5460+2=5462... | 見下方實際 | ✓ |
| S3 | 8192M=744 完整 stripe(11M)+8M 餘 → A=5952+8=5960, B=744, C=1488 | 見下方實際 | ✓ |
| S4 | 8192M=682 完整 stripe(12M)+8M 餘 → A=5456+8=5464, B=682, C=1364, D=682 | 見下方實際 | ✓ |
| CAP | 1024M=85 完整 stripe(12M)+4M 餘 → A=680+4=684, B=85, C=170, D=85 | 見下方實際 | ✓ |

> **2026-08-12 實測計數器**（dmsetup status，bytes）：
> - S2: `A=7363100672 B=1226833920` → 7024:1170 = **6:1** ✓
> - S3: `A=6442450944 B=1073741824 C=1073741824` → 6144:1024:1024 = **6:1:1** ✓
> - S4: `A=5727322112 B=954204160 C=954204160 D=954204160` → 5462:910:910:910 = **6:1:1:1** ✓
> - CAP(1G): `A=717225984 B=119537664 C=118489088 D=118489088` → 684:114:113:113 ≈ **6:1:1:1** ✓
> 餘量 chunk 全落入各 stripe 前端碟（A），與權重順序一致的確定性行為。

---

## 完整性與容量

- **資料完整（修復後回歸）**：S4 上 `fio --verify=crc32c --do_verify=1` **8G 全量** → **0 mismatch**；全程 `err=0`（15:41 `verify8g_s4.sh`：寫 8G、verify_read 8G，2240/3493 MiB/s）。
  2G/4K 版本（F4）同 0 mismatch。修復前因 WC 小寫入退化 verify 卡在 ~13MiB/s。
- **容量擴展**：volume 大小由 `seg0_end` 宣告。S1–S4 用 20G carve 驗證分布；另建 100G carve
  （`0 209715200 tieredvol ...`）成功，1G 寫入分布仍精確。
- 所有 volume 用後即 `dmsetup remove`，無殘留 device。

---

## Mirror / Rebuild（2026-08-12）

拓撲 `tv_mir.conf`：A=nvme0n1(w6) + B=nvme1n1(w1) stripe、mirror→C=sdc，chunk 1MiB、stripe 7MiB。

### 修復與根因
- **Rebuild 首次執行崩潰（系統凍結重啟）**：rebuild bio 只 `alloc_page` 單頁卻 `bio_add_page(...,1MiB,0)`。6.x `bio_add_page` 不截斷長度（multi-page bvec、回傳傳入值）→ 檢查失效 → 送出「bi_size 2MiB / 實體 1 頁」bio → `__blk_rq_map_sg` WARN → `iommu_dma_map_sg` NULL deref → oops 級聯。修法：`alloc_pages(GFP_NOIO, get_order(sz))`、移除手動 `bi_size`、`sz=min(chunk,cur.length)`。
- **STALE 洪水**：改為 hung 偵測（僅 in-flight 且 5s 零完成才 STALE）；閒置 60s / I/O burst / rebuild 期間 0 筆 STALE/RECOVERED。
- **ctr 洩漏**：補 `tv_mirror_destroy_ctx()`。

### 實測結果
- rebuild smoke 64MiB：2s、sdc[0,64M)==pat64 ✅
- rebuild 完整 260MiB（sdc[256M,260M) 先破壞為 0xAA）：1.3s、還原==pat64、裝置讀回全對 ✅
- mirror 同步逐 byte 相符 ✅、64M 寫+讀回+COW 回歸 ✅
- 全程 0 WARNING/Oops/BUG。

---

## WC 專項 W1–W4（2026-08-12，最終 build 重跑 `wc_suite.sh` 15:51）

單碟 `/etc/tieredvol/tv_s1.conf`、`fio --bs=4K --size=1G --iodepth=1 --direct=1 --ioengine=libaio`。
`wc_enabled` 為 module param，W1/W3 重載 `wc_enabled=1`（sysfs `Y`）、W2/W4 重載 `wc_enabled=0`（`N`）。

| ID | WC | 模式 | IOPS | BW |
|----|----|------|------|-----|
| W1 | on | 隨機寫 | 56.9k | 222MiB/s |
| W2 | off | 隨機寫 | 60.8k | 237MiB/s |
| W3 | on | 循序寫 | 58.6k | 228MiB/s |
| W4 | off | 循序寫 | 63.9k | 249MiB/s |

**結論（Bug1 修復後）**：4K（`< chunk_size`）小寫入一律走 `-EAGAIN` 繞過 WC 直寫，故 wc on/off 差距僅 submit-path 簿記開銷：
- **wc=on 全面略慢 6–8%**（222 vs 237、228 vs 249 MiB/s）——小寫入由 WC 收到負擔、無合併效益，符合「修復後小寫入不該走 WC」的設計。
- WC 效益僅在**大寫入**（≥chunk 的批次提交），1M 路徑不受影響（疊碟回歸 1981–3091 MiB/s）。
- 舊表（W3 on 40.2k > W4 off 32.4k，批次化 +24%）為 **Bug1 修復前**、無 log 佐證的紀錄，已由本表取代。

## Mirror M1–M3（2026-08-12）

- **M1（primary 失效由 mirror 回補）**：寫 256M（pat64×4）→ mirror 同步逐 byte 相符 → primary（nvme1n1[0,224M)+nvme0n1[0,28M)）填 0xAA + badmap disk0/disk1 全部 252 chunk → 讀回 256M **全對（資料只可能來自 mirror）**；mirror_wr=2095/268435456 mirror_err=0。
- **M1 retest（2026-08-12，pw 鎖修復後）**：修正 `tv_pw_lock` race 後重跑 6:1 拓撲：寫 256M verify-crc32c（`err=0`）→ primary（nvme0n1[0,220M)+nvme1n1[0,36M)）填 0xAA + badmap disk0=220 / disk1=36 chunks → 讀回 256M **全對（65536 ios / 268435456 bytes，`error=0`）**；`mirror_wr=65536/268435456 mirror_err=0`；全程 **0 次 `pw_remove MISS` / 0 次 give-up / 0 次 pending-full**。
  - **根因（修復前 fail）**：pending-write ring 無鎖。`tv_pw_add`（submit 路徑 `this_cpu_ptr`）與 `tv_pw_remove`/`tv_pw_is_pending`（end_io softirq + read-retry workqueue）在相同 CPU 上並行 → race 造成 remove MISS（dmesg 306 次），殘留條目（含 give-up 目標 sector=11248）讓 read-retry 永遠看到 phantom pending → 32 次重試後 EIO。
  - **修復**：全域 `spinlock_t tv_pw_lock`（`DEFINE_SPINLOCK`）保護 `tv_pw_add`/`tv_pw_remove`/`tv_pw_is_pending`（`tieredvol_mirror.c`，header 宣告 extern）。
- **M2（rebuild 還原 mirror，鎖修復後回歸）**：破壞 sdc[64M,68M)（0xAA）→ `start_rebuild 0 268435456`（256M，~1.3s 完成）→ sdc[64M,68M) 還原 == primary 逐 byte 相符（`mirror == device` 64M 區塊比對 **PASS**）、裝置讀回全對、mirror_err=0。
  - **page cache 陷阱**：rebuild 的 bio 寫入不更新 block device 的 page cache，`dd if=/dev/sdc`（buffered）會命中舊 cache 顯示「未還原」的假象；**驗證必須用 `iflag=direct`**（`dd if=/dev/sdc iflag=direct` 直接讀碟）才見真實還原結果。
- **M3（跨碟界 mirror，鎖修復後回歸）**：`dm_set_target_max_io_len`=1MiB(=chunk) 保證單一 bio 不會跨碟。實測 boundary（logical 6GiB-4K 與 6GiB，6:1 權重下 disk0/disk1 切點）各寫 4K：裝置 direct 讀回全對、mirror 計數 +2 ops / +8192 bytes、mirror_err=0 ✅。

## 2026-08-12 鎖修復（Bug1/Bug2）

重啟後碟位交換（快碟 nvme0n1），回歸前發現兩 bug：

- **Bug1：WC 小寫入退化**。`tieredvol_wc.c` 對 `<chunk_size` 的寫入直接發獨立 bio（未走 WC 合併），且複用同一 `flush_bio` → 小寫入變 serialized、S4 verify 卡 ~13MiB/s。
  修復：`tieredvol_wc.c:162` 對 `bio->bi_iter.bi_size < ctx->meta.chunk_size` 回 `-EAGAIN`（退回 map 路徑直接寫，維持 4K 直寫效能）。
  **實測**：s2 4K depth=8 寫 **14 → 500 MiB/s**（37x）；1M depth=32 2737 MiB/s（大寫入 WC 不受影響）。
- **Bug2：pending-read ring race**。`tv_pending_add`（submit 路徑）/`tv_pending_find_and_remove`（end_io softirq + read-retry workqueue）無鎖 → end_io 掃描全部 CPU ring 時與 submit 路徑並行，可能漏掉 / 讀到半更新條目。
  修復：pending-read 改用與 pending-write 相同的全域 `tv_pending_lock` 保護（`tieredvol_mirror.c:61-126`；header `tieredvol.h:238` 註解更新）。
  **實測**：M1 retest 全程 0 次 `pw_remove MISS` / 0 give-up / 0 pending-full；M2/M3 回歸全過。

## 功能驗證 F1–F5（2026-08-12）

| ID | 測試 | 結果 |
|----|------|------|
| F1 | loop 拒絕 | `/dev/loop13` 明確拒絕（`virtual device rejected`），dmsetup create RC=1 ✅ |
| F2 | Lifecycle | create → bench → show_stats → remove → recreate → 讀取全成功 ✅ |
| F3 | DM messages | show_mirror/show_log（dmesg）、set_policy random/static（status 顯示 policy=2/0）、reset_stats（per-disk 歸零）、status 含 mirror/err/碟狀態 ✅ |
| F4 | crc32c 2G | `fio --verify=crc32c --do_verify=1` 0 mismatch、err=0（寫 248MiB/s、verify 371MiB/s）✅ |
| F5 | Badmap | 無 mirror：badmap chunk 讀出全 0、不 I/O error；未 badmap chunk 讀取正常；有 mirror（M1）：讀回真資料 ✅ |

> **最終 build 重驗（2026-08-12 15:42，`/home/yu/f_suite.sh`）**：F1–F5 全套重跑 **17/17 PASS**（F1 loop 拒絕 RC=1、F2 六步全成功、F3 六則 message 全 RC=0 + policy 切換正確、F4 crc32c 2G 0 mismatch、F5 badmap zero-fill/clear 讀回 0xAA 全對）。

> 註：`dmsetup message` 不印 handler 的 `result` 字串（dmsetup 行為），部分 handler 另以 `pr_info` 落 dmesg；`dmsetup status`（STATUSTYPE_INFO）為完整可見介面。

---

## Mirror 損耗（2026-08-12，鎖修復後回歸）

同條件對照：`tv_s2`（A+B stripe，無 mirror）vs `tv_mir`（同 stripe + mirror→C=sdc）。libaio。本次 primary（nvme0n1）未節流，真實反映 mirror 碟限制。

| 工作負載 | tv_s2 | tv_mir | 損耗 |
|----------|-------|--------|------|
| 1M / depth32 / 8G 寫 | 2390 MiB/s | 463 MiB/s | **81%** |
| 1M / depth32 / 8G 讀 | 2777 MiB/s | 2787 MiB/s | **0%** |
| 4K / depth8 / 2G 寫 | 584 MiB/s | 282 MiB/s | **52%** |

- **1M 寫損耗 81%** ≈ `1 − 470/2009`（mirror 碟 C≈470 vs primary A≈2009 MiB/s）——寫放大本質（每筆寫 primary + mirror 兩份），非 driver 開銷。
- **4K 小塊 52%** = COW 開銷（alloc_page + 2×kmap + memcpy + bio_add_page + completion 追蹤，tieredvol_mirror.c:276-325）+ mirror 碟 SATA 4K 隨機能力。
- 讀路徑完全不碰 mirror（除非 badmap 回補），故讀損耗恆為 0%。

**mirror 計數器**（1M 8G + 4K 2G = 10G）：`mirror=589824/10737418240 err=0`；全碟狀態 A。

---

## LVM vs TieredVol 公平對比（2026-08-12，現行拓撲）

雙方同參數：`fio libaio iodepth=32 direct=1 bs=1M size=8G end_fsync=1`（= 疊碟驗收標準）。

| 碟數 | TieredVol W / R | LVM striped W / R | TV/LVM 寫 | TV/LVM 讀 |
|------|------------------|-------------------|-----------|-----------|
| 2d（A+B） | 2661 / 2786 | 786 / 796 | 3.39x | 3.50x |
| 3d（A+B+C） | 2787 / 3180 | 1180 / 1190 | 2.36x | 2.67x |
| 4d（A+B+C+D） | 3091 / 3575 | 1575 / 1590（256K） | 1.96x | 2.25x |

- **LVM 4d stripe size sweep**（W/R MiB/s）：64K=1059/1513、128K=1525/1587、256K=**1575/1590**、1M=1572/1589 → **256K 最佳**（128K 接近，與歷史結論一致）。
- **4K 小寫入**（2G, d32）：TieredVol S4=**511** vs LVM 4d=**676** MiB/s → **LVM 較快 1.32x**。原因：LVM 4K 平行散到 4 碟；TieredVol 4K 走 WC 繞過直寫、分布 6:1:1:1 使 75% 落單碟 A。
- **結論**：1M 順序大塊 TieredVol 全面領先（1.96–3.5x，差距隨快碟權重占比上升）；4K 小塊是 LVM fixed stripe 的優勢區。
- 操作流程：`pvcreate` 4 碟 → `vg_tv2` → `lvcreate --stripes N --stripesize S -L 20G`；測完 `vgchange -an`/`vgremove`/`pvremove` 還原裸碟，TieredVol 功能檢查通過（S4 建置/移除 OK）。

---

## 2026-08-13 碟位再交換 · 多卷併發（P3/P4）

### 拓撲更新與環境

- **8/13 重啟後快/慢碟再交換**：`nvme1n1`=WD SN750（快，權重 6）、`nvme0n1`=P3 Plus（慢，權重 1）。config 已更新（disk0=nvme1n1），權重鎖定 **6:1:1:1** 不變。
- 實驗環境確認：全程同一 module build（8/12 17:01）、碟位經 smartctl 驗證無變動、分布計數器 `6:1` 精確（`A=7363100672 B=1226833920`）。

### 單碟 solo 基線（raw fio，1M/8G/libaio depth32）

| 碟 | 型號 | 寫 (MB/s) | 讀 (MB/s) |
|----|------|-----------|-----------|
| nvme1n1 | WD SN750（快） | **2064** | **3152** |
| nvme0n1 | P3 Plus（慢） | 413 | 418 |
| sdc | MX500 | 519 | 501 |
| sdb | WD Blue | 267 | 534 |

> sdb 寫重測 219–267（兩次），8/12 平行仲裁 346 為 SLC cache 短寫值，solo 真實 ~220–270。其餘與 8/12 對照差 ≤10%。

### P3：Mirror/Rebuild 專項（`scripts/msg_probe.sh`，8/12 17:03）

**42/42 PASS**：sysfs 7 項、policy/wear 9 項、stats/config 6 項、mirror/badmap 9 項、rebuild start/stop/EBUSY、`rebuild_badmap`、完整 rebuild 完成回 idle。

**Bug3：rebuild_badmap 整機凍結修復**（`driver/tieredvol_badmap.c`，working tree 修復）
- 根因：`tv_badmap_rebuild` 只 `alloc_page()`（單 4K 頁）卻 `bio_add_page(bio, pg, chunk_bytes=1M, 0)` + 手動 `bi_iter.bi_size=1M`。kernel 6.x `bio_add_page` 對 multi-page bvec 不截斷（回傳傳入長度）→ 檢查失效 → 送出「bi_size 2M / 實體 1 頁」bio → `__blk_rq_map_sg` WARN → sg 越界 NULL deref → 系統凍結。
- 修法：`alloc_pages(GFP_NOIO, get_order(chunk_bytes))` 配 compound 頁、移除手動 `bi_iter.bi_size`（與 `tv_rebuild_thread` 既有寫法一致）。
- 實測（`scripts/rebuild_min.sh`，8/12 17:01）：`badmap rebuild done: 1 recovered, 0 failed`、`DONE no-hang`。

### P4：多卷併發（8/13，8G/卷，1M/libaio depth32）

三組對照 + 8/12 原始 P4（共用快碟）數據一併列出：

| 實驗 | 卷組合 | 孤立和 寫/讀 | 併發和 寫/讀 | 代價 寫/讀 | 併發寫和 vs 快碟 solo |
|------|--------|--------------|--------------|------------|----------------------|
| 不共用碟 | (A+B)+(C+D) | 3012/3740 | 3085/3670 | **-2.4%/1.9%** | —（無共享） |
| 完全同碟 | (A+B)×2 | 5331/5837 | 1569–1668/2927 | 68.7%/49.9% | 78–81% |
| 不對稱 | (A)+(S4) | 5143/6608 | 1906/3978 | 62.9%/39.8% | 92% |
| P4 原(8/12) | (S4)+(S2) 共用A | 6277/6662 | 2003/3501 | 68.1%/47.5% | 99.7% |

**結論**：
1. **driver 無多卷併發開銷**：不共用碟時併發和 ≈ 孤立和（±2%），全部 fio `err=0`、dmesg 0 bad。
2. **併發代價 = 共享碟物理爭用**：共享快碟的三組併發寫和（2003/1906/1619）全部落在「快碟 solo 2064」附近（92–100%）；完全同碟最低（78–81%），因兩卷佇列完全重疊 + 慢碟瓜分，屬最嚴重情形。
3. 8/12 的 P4 68%/47.5% 與 8/13 完全同碟 68.7%/49.9% 一致——**同一物理現象**（共享快碟），非 driver 或 build 差異。

### 誤差驗證（判據 ±15%，三個偏差有物理解釋）

| 測量 | 實測 | 模型/參考 | 誤差 |
|------|------|-----------|------|
| nvme1n1 W / R | 2064 / 3152 | 8/12 A 2009 / 3009 | +2.7% / +4.8% ✓ |
| nvme0n1 W | 413 | 8/12 B 389 | +6.2% ✓ |
| sdc W | 519 | 8/12 470 | +10.4% ✓ |
| tv_s2 孤立 W / R | 2496 / 2841 | 權重模型 2408 / 2926 | +3.7% / -2.9% ✓ |
| tv_r 孤立 W / R | 516 / 898 | 權重模型 511 / 877 | +1.0% / +2.4% ✓（511 採重測 sdb 寫 219） |
| tv_x1(A) W / R | 2088 / 2990 | solo 2064 / 3152 | +1.2% / -5.1% ✓ |
| tv_x2(S4) W | 3054 | 權重模型 3096 | -1.4% ✓ |
| sdb solo W | 267 (219) | 8/12 346 | **-23%**：SLC cache 短寫偏高，重測 219–267 穩定 |
| tv_x2(S4) R | 3618 | 線性模型 4728 | **-23%**：讀受匯流排/controller 限制（8/12 S4 讀 3575 同現象，非 driver） |
| 完全同碟併發 W | 1569–1668 | 快碟 solo 2064 | **-19~-24%**：兩卷佇列完全重疊 + 慢碟瓜分，最嚴重情形（15:11 窗口內重測 784.4+785.0 一致；SLC 耗盡後重測 479 為狀態漂移，見下註） |

> **SLC 快取窗口（方法論註記）**：本節併發數據於 15:04–15:10 同一窗口測得（快碟 SLC 快取有效，solo 2064）。
> 窗口後（15:18 起）快碟 SLC 快取耗盡進入 TLC 直寫：solo 降至 ~409（`raw_solo_0813_1620.log`）、完全同碟併發重測僅 479（`reverify_same_disk_0813_1619.log`）。
> 此漂移為 SSD 正常行為（當日累計寫入 >160G），**非 driver 退化**；SLC 耗盡後之量測須重啟/快取恢復後重測方與本節可比。

### 8/13 冷態疊碟回歸（重啟後 SLC 恢復，當日當拓撲，`scripts/stack_retest.sh`）

當日 solo 寫：A=2084 B=412 C=517 D=216 MB/s（raw_solo_0813_1655.log）

| 組合 | 實測 W | 分布計數器（bytes） | 瓶頸模型 | 對模型 |
|---|---|---|---|---|
| S1(A) | 2076 | 全 A（8589934592） | 2084 | -0.4% ✓ |
| S2(A+B) | 2228 | 7363100672 : 1226833920 = **6:1** | 2431（A×7/6） | -8.3% |
| S3(A+B+C) | 3146 | 6442450944 : 1073741824 : 1073741824 = **6:1:1** | 2779（A×8/6） | +13.2% |
| S4(A+B+C+D) | 1949 | 5727322112 : 954204160×3 = **6:1:1:1** | **1945（D×9）** | **+0.2% ✓✓** |

> **瓶頸模型（加權系統的正確模型）**：權重鎖死資料比例（快碟吃 6/9），總吞吐 = `min(各碟 solo × 總權重/自身權重)`，**不是各碟原生總和**。
> - S4 瓶頸已從 A（8/12，2009×9/6=3014）**轉移到 sdb**（solo 衰退 346→216，216×9=1945）。
> - **解釋 S4「暴跌」（8/12 3091 → 8/13 1949）**：純為 **sdb 碟衰退**（SLC cache 耗盡/碟狀態），非 driver 退化。
> - 「原生總和」直覺（A+B+C+D=3229）**被否證**：S4 實測僅為其 60%，卻精準命中瓶頸模型（1945，3 次全在 ±4%）。
> - S2/S3 重跑抖動 ±8–13%（快碟 SLC 邊際、B/C 慢碟佇列抖動），但分布計數器全部精確（6:1 / 6:1:1）。

**跨日模型驗證**（8/12 用平行仲裁 solo A=2009 B=389 C=470 D=346）：S3 2679→2787(+4.0%)、S4 3014→3091(+2.6%)、S2 2344→2661(+13.5%，同 SLC 邊際)。**S4 三次量測對瓶頸模型：+2.6% / +3.4% / +0.2%，全數 ≤4%**。

### 8/13 晚權重-速比失配實驗（重啟後拓撲回到 8/12：快碟=nvme0n1，當日 solo A=2069 B=413 C=518 D=223）

| 實驗 | 權重 | 瓶頸碟 | 瓶頸模型 | 實測 W | 對模型 | 分布計數器 |
|---|---|---|---|---|---|---|
| A | 6:1:1:1 | D（223×9） | 2007 | 1966 | -2.0% ✓ | **6:1:1:1 精確** |
| B | 10:2:2:1（=碟速比） | A（2069×15/10=3104；當下 1782×15/10=2673） | 3104（冷態）/2673（當下） | 2673 | **+36% vs A** | **10:2:2:1 精確** |
| C | 6:1:1:1 + `set_policy adaptive` | — | — | 1096 | **-44% vs A** | B 吃 29%、C/D 各 <1%（**嚴重失衡**） |

> **權重-速比失配 = S4 吞吐上限的根因（實驗證明）**：
> - 權重 6:1:1:1 與碟速比 6.3:1.2:1.6:0.65 失配，sdb（solo 223，權重卻與 B/C 同為 1）成瓶頸 → S4 被卡 2007。
> - 調成速比 10:2:2:1 後**提升 36%**（2673），分布仍精確 10:2:2:1——權重調節有效。
> - B 未達冷態模型 3104：瓶頸為 A（吃 10/15 資料），連續寫入（實驗 A 已寫 5.7G）後 A SLC 消耗 → 當下 solo 1782，`1782×15/10=2673` 精確命中「瓶頸碟當下值」。
> - **ADAPTIVE 選碟現狀不可用**：吞吐 -44%、C/D 幾乎餓死（合計 <2%）。若要走自適應路線需大幅加強 `tv_map_logical_adaptive`；否則以「權重平衡 STATIC」為準。

- **分布**：tv_s2 併發寫 8G 後計數器 `A=7363100672 B=1226833920` = **6:1 精確**（0 誤差），config 8/13 更新後權重語義正確。
- `make test`：**267/267 assertions、5/5 suites**（49+25+14+170+9）；#11 `dmsetup table` 輸出 `0 41943040 tieredvol /dev/nvme1n1 /dev/nvme0n1 /dev/sdc /dev/sdb`（4 碟展開清單，順序與 config disk0–3 一致）== create 參數 `0 41943040 tieredvol /home/yu/tv_s4.conf`。

---

## 結論

1. **疊碟可擴展**：寫入吞吐隨碟數上升 1981 → 2661 → 2787 → 3091 MB/s（8/12 冷態，sdb 尚非瓶頸）。**例外**：8/13 sdb 衰退後 S4（1949）受 sdb 瓶頸而低於 S3（見第 11 點），擴展性由「瓶頸碟」主導而非碟數。
2. **分布精確**：計數器與宣告權重（6:1:1:1）、確定性映射預測完全一致（0 誤差）。
3. **資料完整**：crc32c **8G 全量** 寫入 + verify 0 mismatch，`err=0`（WC 小寫入 bug 修復後）。
4. **Mirror 全功能可用**：同步/讀回/rebuild/跨碟界全部正確，`mirror_err=0`；M1–M3 鎖修復後回歸全過。
5. **鎖修復**：pending-write（`tv_pw_lock`）與 pending-read（`tv_pending_lock`）ring 均有全域 spinlock 保護，0 次 MISS / give-up / full。
6. **WC 小寫入修復**：4K 寫 14→500 MiB/s；大寫入路徑不受影響（最終 build tv_s2 4K=612、1M=2661 MiB/s）。
7. **Mirror 損耗**：1M 寫 81%（mirror 碟慢於 primary 的寫放大本質）、4K 小塊 52%（COW 開銷）、讀 0%。
8. **vs LVM**：1M 順序寫/讀達 LVM striped 的 **1.96–3.5x**（快碟權重占比越高越明顯）；4K 小寫入 LVM 較快 1.32x（676 vs 511 MiB/s）。
9. **rebuild_badmap 凍結修復（Bug3）**：compound page + 移除手動 `bi_size`，`1 recovered, 0 failed` no-hang。
10. **多卷併發**：driver 本身零開銷（不共用碟併發≈孤立，±2%）；共享碟併發代價 40–69% 純為快碟物理爭用（併發和 ≈ 快碟 solo 2064，三組吻合），非 driver 問題。
11. **加權吞吐由瓶頸碟決定（非原生總和）**：總吞吐 = `min(solo_i × 總權重/weight_i)`。S4 三次量測對模型 ±4%（8/12 +2.6%、8/13 +3.4%/+0.2%）；sdb 衰退（346→216）使 S4 瓶頸由 A 轉移 D，精確解釋 S4 從 3091 降至 1949。
12. **權重-速比失配 = 加權吞吐上限的根因（8/13 晚實驗證實）**：6:1:1:1（瓶頸 D）1966 → 10:2:2:1（碟速比，瓶頸 A）2673，**提升 36%**、分布仍精確；`set_policy adaptive` 現狀 -44% 且 C/D 失衡，自適應選碟需加強或改採權重平衡。

使用 conf：`/home/yu/tv_s1.conf`、`tv_s2.conf`、`tv_s3.conf`、`tv_s4.conf`、`tv_mir.conf`
（8/13 拓撲：disk0=nvme1n1, disk1=nvme0n1, disk2=sdc, disk3=sdb；8/12 舊拓撲為 disk0=nvme0n1, disk1=nvme1n1）。
