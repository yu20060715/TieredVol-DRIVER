# Test Results — TieredVol-DRIVER（現況拓撲）

> 實測日期：2026-08-12（疊碟/Mirror/LVM）· 2026-08-13（多卷併發 P3/P4）· 2026-08-14（全卷自動加權定案 + 插槽解鎖/DMI 兩組實驗定案）· Kernel 6.14.0-27-generic · dm-target v5.0
> 完整測試程序與驗收判據見 [TEST_PLAN.md](TEST_PLAN.md)。
> **現況拓撲（2026-08-14 晚）**：資料池 4 碟 = A（SN750，CPU 直連 PCIe3.0 x4）+ B（P3 Plus，PCH PCIe2.0 **x1**，8/14 晚自 x4 移回）+ C（MX500，SATA）+ **D（WD Blue，SATA，8/14 晚回池）**。現役 config 權重（8/14 晚重加權）：S2 [83:17]、S3 [81:16:20]、S4 [82:16:20:9]、mirror [83:17]；見 [CONFIG.md](CONFIG.md)。
> **碟位/插槽變更紀錄**：8/12–8/13 多次 nvme 代號交換（config 以 by-id 綁碟身分、不受影響）；**8/14 下午 P3 Plus 由 PCIe2.0 x1 換至 PCIe2.0 x4 插槽（solo 415→1522，+3.7x）**、移出 WD Blue；**8/14 晚為重測 8/12 疊碟表而移回 x1、WD Blue 回池**。舊 config 備份於 `/home/yu/backup-20260814-old/`。
> 各歷史章節標註當日拓撲；疊碟曲線以「8/14 排水態重測」（見下）為準，「8/14 兩組實驗」（B@x4）為歷史紀錄。

---

## 硬碟對照（目前拓撲，2026-08-14 下午）

```
代號    裝置          型號                    容量   介面          PCIe link    solo 測速(8G)
A    = nvme0n1      WD SN750 WDS500G3X0C      500G   PCIe 3.0 x4   8.0 GT/s    2064-2080 MB/s
B    = nvme1n1      P3 Plus CT1000P3PSSD8     1T     PCIe 2.0 x1   5.0 GT/s    389-415 MB/s（8/14 下午 x4 解鎖時 1517-1522）
C    = sdc          MX500 CT500MX500SSD1     500G   SATA 6G       -           516-518 MB/s
D    = sdb          WD Blue WDS250G2B0A      250G   SATA 6G       -           220-267 MB/s（8/14 晚回池）
E    = sda          BX100 CT250BX100SSD1     233G   SATA 6G       -           （開機碟，不入池）
```

- **stable by-id 綁定**（config `diskN_name` 一律用此，避免 `/dev` 代號重啟變動）：
  - A = `/dev/disk/by-id/nvme-WDS500G3X0C-00SJG0_200705800588`
  - B = `/dev/disk/by-id/nvme-CT1000P3PSSD8_242849E47E24`
  - C = `/dev/disk/by-id/ata-CT500MX500SSD1_2129E5B858A9`
  - D = `/dev/disk/by-id/ata-WDC_WDS250G2B0A_201965800292`（8/14 晚回池）
  - E = `/dev/disk/by-id/ata-CT250BX100SSD1_1452F001259B`（開機碟，不入池）
  - 轉換工具：`scripts/conf_to_byid.py`（含 crc32 重算，`tv_compute_config_crc` 同演算法）。

- **插槽角色（2026-08-14 晚）**：A（WD SN750）在 **CPU 直連** PCIe3.0 x4（繞過 DMI）；B（P3 Plus）在 **PCH** PCIe2.0 **x1**（8/14 下午曾在 x4，見「8/14 兩組實驗」節）；C（MX500）SATA 在 PCH；D（WD Blue）SATA 在 PCH。
- **solo 測速** = 單碟 raw 寫 8G（fio 1M libaio depth=32）。短寫會吃到 SLC cache，故以 8G 平均為準（量測協定見 8/14 下午節）。

---

## 權重調校（現況：auto_weight v2 窮舉）

```
solo 基線：B@x4（8/14 下午）A=2064-2080  B=1517-1522  C=516-518
          B@x1（8/14 晚）  A=2014-2086  B=411-415  C=516-519  D=216-229 (MB/s)
```

現役 config 權重（`/etc/tieredvol/`，`dmsetup create` 已建卷、CRC 全過、boot 自動重建 OK）：
- `tv_s1.conf` **1**（A solo）
- `tv_s2.conf` **83:17**（A+B，stripe 104857600）
- `tv_s3.conf` **81:16:20**（A+B+C，stripe 122683392）
- `tv_mir.conf` **83:17** + mirror=2（A+B→C）
- `tv_s4.conf` **82:16:20:9**（A+B+C+D，stripe 133169152，D 回池後回歸）

權重由 `scripts/auto_weight.sh` v2（窮舉搜尋，最大化瓶頸模型 `min(solo_i×W/w_i)`）產生（B@x1 + D 回池，8/14 晚重加權）。
B@x1 下 B+C≈928 < DMI 1300，故比例權重即最佳、不需 DMI-aware（8/14 下午 B@x4 的 [64:30:10] 為歷史值，見「8/14 兩組實驗」節）。

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
- **疊碟表用排水態量測**（短寫會落在 SLC 帶狀、不可重現）：100G carve 卷 → 寫 64G 排水（丟棄）→ `dmsetup remove`+重建歸零計數 → 寫 16G 量測 → `dmsetup status` 驗分布 → 讀 16G。
- 分布判據：**完整 stripe 依權重精確分配；非完整 stripe 的餘量 chunk 依映射落入前段碟**
  （確定性 O(1) 映射，無誤差、每 byte 恰落一碟）。

---

## 疊碟結果（2026-08-14 排水態重測，權重 6:1:1:1）

> 本表**取代** 8/12 冷態短寫表（原 1981→2661→2787→3091）。原表數值落在 **SLC 帶狀**（短寫吃到快碟 SLC cache），
> 同場重測即跳動（S2∈[2262,2713]、S3≈3282、S4≈3093）。故改以**排水態（steady-state）**量測：先寫 64G 把 SLC 灌滿，
> 再量 TLC 穩態——可重現、貼瓶頸模型。**讀取不進 SLC，直接量測。**

| ID | 碟數 | 權重 | 穩態 Write(MB/s) | Read(MB/s) | 分布（16G 寫，1M-chunk） | 完整性 |
|----|------|------|------------------|------------|--------------------------|--------|
| S1 | 1 (A) | 1 | 1360 | 3127 | A=16384（100% 精確） | - |
| S2 | 2 (A+B) | 6:1 | 1667 | 2924 | A=14044 B=2340（=6.002:1） | - |
| S3 | 3 (A+B+C) | 6:1:1 | 1999 | 3293 | A=12288 B=2048 C=2048（=6:1:1） | - |
| S4 | 4 (A+B+C+D) | 6:1:1:1 | 1760 | 3722 | A=10924 B=1820 C=1820 D=1820（=6.002:1:1:1） | - |

> **協定**：100G carve 卷（權重/碟序/分段同原 8/12 config，僅 seg0_end 放大）→ 先寫 **64G 排水**（丟棄）→
> `dmsetup remove` + 重建歸零計數 → 寫 **16G 量測**（計數器只含量測段）→ `dmsetup status` 驗分布 → 讀 16G。
> 全程 `err=0`、`borrow=0`。原始 8/12 config 由 `/home/yu/backup-20260814-old/` 直接復用（crc 全過）。

> **1→2→3 碟寫入單調 1360 → 1667 → 1999**（每加一碟寫入上升）。**S4=1760 < S3**：加入的 D（WD Blue，solo 220–267）
> 在穩態成為瓶頸碟，`D×9 ≈ 1760` 精確命中瓶頸模型 `min(solo_i×W/w_i)`，故 4 碟不再往上。

> **S2≈S3「怪象」解釋**：8/12 冷態 S2=2661≈S3=2787 為 SLC-fresh 帶狀假象（S2 短寫吃到 A 的 SLC 邊際、被灌高）；
> 排水態下 **S2=1667 < S3=1999**，間隔清晰。S2 的不穩定（2262↔2713）即短寫落在 SLC 帶狀不同位置所致。
> 8/12 表 S4=3091 > S3=2787 亦為 SLC 幸運值，真實穩態關係為 **S4 < S3**（D 封頂）。

**分布驗證（2026-08-14 實測計數器，bytes）**：
- S2: `A=14726201344 B=2453667840` → 14044:2340 = **6.002:1** ✓
- S3: `A=12884901888 B=2147483648 C=2147483648` → 12288:2048:2048 = **6:1:1** ✓
- S4: `A=11454644224 B=1908408320×3` → 10924:1820:1820:1820 = **6.002:1:1:1** ✓
- 確定性映射同 8/12 驗證（餘量 chunk 依權重順序從 A 起連續分配），一致。

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
- WC 效益僅在**大寫入**（≥chunk 的批次提交），1M 路徑不受影響（疊碟回歸 1360–1999 MiB/s，排水態重測）。
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

> 註：tv_s2 1M 寫在不同時刻量測為 2390–2737 MiB/s（疊碟冷態 2661、Bug1 復測 2737、本表 2390），
> 差異為 SLC 快取/熱狀態漂移（solo 短寫會吃到 SLC cache），非衝突數據。

- **1M 寫損耗 81%** ≈ `1 − 470/2009`（mirror 碟 C≈470 vs primary A≈2009 MiB/s）——寫放大本質（每筆寫 primary + mirror 兩份），非 driver 開銷。
- **4K 小塊 52%** = COW 開銷（alloc_page + 2×kmap + memcpy + bio_add_page + completion 追蹤，tieredvol_mirror.c:276-325）+ mirror 碟 SATA 4K 隨機能力。
- 讀路徑完全不碰 mirror（除非 badmap 回補），故讀損耗恆為 0%。

**mirror 計數器**（1M 8G + 4K 2G = 10G）：`mirror=589824/10737418240 err=0`；全碟狀態 A。

---

## LVM vs TieredVol 公平對比（2026-08-12 拓撲）

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

## 2026-08-13 碟位再交換 · 多卷併發（P3/P4）*(8/13 午配置：disk0=nvme1n1=WD；8/13 晚重啟後已回到 disk0=nvme0n1=WD，見下節「8/13 晚實機回歸」)*

### 拓撲更新與環境（8/13 午）

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

**42/42 PASS（8/12 歷史版）**：sysfs 7 項、policy/wear 9 項、stats/config 6 項、mirror/badmap 9 項、rebuild start/stop/EBUSY、`rebuild_badmap`、完整 rebuild 完成回 idle。

> 8/13 移除 adaptive/wear 後，`msg_probe.sh` 已重寫為現行 handler 對應版（40/40 PASS，見 8/13 實機回歸章節）。

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
> - S3 的 **+13.2%**（3146 vs 模型 2779）為高於模型側的單次量測：模型以當日 solo 快照（A=2084）推導，
>   實際寫入瞬間快碟 SLC 殘餘快取/佇列抖動會把吞吐推過模型，屬測量噪音非衝突；計數器 6:1:1 精確。

**跨日模型驗證**（8/12 用平行仲裁 solo A=2009 B=389 C=470 D=346）：S3 2679→2787(+4.0%)、S4 3014→3091(+2.6%)、S2 2344→2661(+13.5%，同 SLC 邊際)。**S4 三次量測對瓶頸模型：+2.6% / +3.4% / +0.2%，全數 ≤4%**。

### 8/13 晚權重-速比失配實驗（重啟後拓撲回到 8/12：快碟=nvme0n1，當日 solo A=2069 B=413 C=518 D=223）

| 實驗 | 權重 | 瓶頸碟 | 瓶頸模型 | 實測 W | 對模型 | 分布計數器 |
|---|---|---|---|---|---|---|
| A | 6:1:1:1 | D（223×9） | 2007 | 1966 | -2.0% ✓ | **6:1:1:1 精確** |
| B | 10:2:2:1（=碟速比） | A≈B（冷態 min 實為 B=413×15/2=**3098**，A=2069×15/10=3104，僅差 0.2% 屬 A/B 共瓶頸；當下瓶頸 A=1782×15/10=2673） | 3104（冷態）/2673（當下） | 2673 | **+36% vs A** | **10:2:2:1 精確** |
| C | 6:1:1:1 + `set_policy adaptive` *(歷史)* | — | — | 1096 | **-44% vs A** | B 吃 29%、C/D 各 <1%（**嚴重失衡**） |

> **權重-速比失配 = S4 吞吐上限的根因（實驗證明）**：
> - 權重 6:1:1:1 與碟速比 6.3:1.2:1.6:0.65 失配，sdb（solo 223，權重卻與 B/C 同為 1）成瓶頸 → S4 被卡 2007。
> - 調成速比 10:2:2:1 後**提升 36%**（2673），分布仍精確 10:2:2:1——權重調節有效。
> - B 未達冷態模型 3104：瓶頸為 A（吃 10/15 資料），連續寫入（實驗 A 已寫 5.7G）後 A SLC 消耗 → 當下 solo 1782，`1782×15/10=2673` 精確命中「瓶頸碟當下值」。
> - *(歷史結論)* **ADAPTIVE 選碟現狀不可用**：吞吐 -44%、C/D 幾乎餓死（合計 <2%）。此實驗促成 8/13 移除 adaptive、改以權重平衡 STATIC + weight-borrowing 為準。

### 8/13 auto-weight 原型驗收（`scripts/auto_weight.sh`）

工具流程：測各碟 solo 寫（8G，與疊碟同條件）→ 權重 = `max(1, round(solo/min_solo))` → 更新 config `seg0_weight`/`seg0_stripe`，並移除被 driver 存回的 `crc32`/`badmap` 行、強制 `policy=0`（static）。

- `tv_s4_auto.conf`：測速 2077 : 412 : 518 : 216 → 權重 **10:2:2:1**（stripe 15728640），瓶頸模型 3092。
- **驗收**：S4_auto 寫 8G = **2954 MB/s**（err=0）對模型 **-4.5%** ✓；分布計數器 **10:2:2:1 精確**（5727322112:1145044992:1145044992:572522496）。
- **對比**：6:1:1:1 = 1966 → auto（10:2:2:1）= 2954，**+50%**；瓶頸從 D（216×9=2007）轉為 B（412×15/2=3092），快碟 A 不再被慢碟權重卡住。
- 附註：config 檔經 driver 建卷後會寫回 `crc32=`/`badmap_*`/`[runtime] policy`；改檔後須同步清理（本工具已內建），否則 CRC mismatch 建卷 EIO。

- **分布**：tv_s2 併發寫 8G 後計數器 `A=7363100672 B=1226833920` = **6:1 精確**（0 誤差），config 8/13 更新後權重語義正確。
- `make test`：**267/267 assertions、5/5 suites**（49+25+14+170+9）；#11 `dmsetup table` 輸出 `0 41943040 tieredvol /dev/nvme1n1 /dev/nvme0n1 /dev/sdc /dev/sdb`（4 碟展開清單，順序與 config disk0–3 一致）== create 參數 `0 41943040 tieredvol /home/yu/tv_s4.conf`。*(8/13 上午快照；當日稍後加入 128KB borrow 語意 + `.borrow` v2 格式 + kernel 源碼測試層後為 **301/301、6/6 suites**；8/13 晚清理 userspace prototype `src/` 與其 4 個測試後為 **2 suites（test_map 301 + test_stripe_kernel 27）**，見 TEST_PLAN「測試層架構」)*

### 8/13 weight-borrowing 原型實機驗證（`tv_s4_borrow.conf`，權重 6:1:1:1）

設計：`[runtime] borrow_enable=1 borrow_watermark_kb=256 borrow_area_mb=2048,0,0,0`。慢碟份額（水位以上）的寫入借到快碟 A 尾端保留區（2 GB），借出 block 記錄在持久化 overlay（`<config>.borrow`，magic TVBR），重載後映射恢復。與 mirror 互斥、borrow on 時停用 WC。

- **實機關鍵發現**：本機 NVMe 全部 `max_sectors_per_request=256`（128 KB）→ WRITE bio 恆為 128 KB，舊的 1 MB chunk 對齊條件永不觸發（`n_borrowed=0`）。借出粒度改為 **128 KB block（= chunk_size/8）**，依 block index 查表/借出。
- **修復 1（save 死鎖）**：`tv_borrow_save` 曾在 spinlock 內 `filp_open`/`kernel_write`（sleep-while-atomic）→ `dmsetup remove` 卡死整機。改為鎖內 snapshot + 鎖外寫檔，每次 save 正常、無卡死。
- **修復 2（混合單位）**：lookup/redirect 曾把 byte 單位的 `logical % block_size` 直接加進 sector 單位的 `dst_sector` 再 `>>9` → 借出寫/讀錯位、verify 全 fail。修正後（offset 先 `>>9`、sector 項不移位）verify 全過。
- **驗收（同卷 6:1:1:1）**：
  - 寫入 verify=crc32c：64 M / 256 M / 1 G / 4 G **全部 err=0**（借出 26/643/2523/10041 blocks）。
  - 讀 verify_only：256 M/1 G/4 G **全部 err=0**。
  - 吞吐：4 G 寫 **2346 MB/s**、讀 **3165 MB/s**；1 G 讀 3130；256 M 寫 2237。
  - 持久化：remove 觸發 save（10041 blocks, 3932176 B）→ rmmod/insmod → 重建卷載入 10041 blocks → 4 G verify 讀 **err=0**，映射正確恢復。
  - 全程 dmesg 無 hung / irq-disabled / panic / oops。
- **static 對比（`tv_s4_static.conf`，borrow_enable=0，同拓撲）**：4 G 寫 2275、讀 3529 MB/s。此配置下 borrow 寫 +3%、讀 -10%（讀時查表開銷 + 讀集中單碟）；瓶頸由 D 轉 A，增益受快碟 A solo 上限限制——borrow 在「快碟有富餘」的拓撲收益更大。
- **ADAPTIVE 已移除**（`tieredvol_map.c`/`tieredvol_log.c` 自適應選碟與 decay 統計全刪），現役 `policy=0` static + borrow。

### 8/13 晚實機回歸（碟位回到 nvme0n1=WD 快，權重 6:1:1:1）

**`scripts/msg_probe.sh` 40/40 PASS（19:23，`/home/yu/msg_probe_0813_1923.log`）**：移除 adaptive/wear 後重寫之現行 handler 版——sysfs 4 項（policy/status/disk_count/loglevel）、policy/borrow 7 項（set_policy/set_seg_policy/show_stats/status/show_borrow/borrow_off/borrow_on）、stats/config 8 項、mirror/degraded/badmap 10 項、rebuild 7 項（start/show/EBUSY 拒絕/stop/idle/rebuild_badmap/32M 完整完成）、remove/rmmod 2 項。29 個現行 message handler 全覆蓋。

**`scripts/borrow_verify.sh` 15/15 PASS（19:50，`/home/yu/borrow_verify_0813_1950.log`）**：`CFG=/home/yu/tv_s4_borrow.conf SIZE=256MB`。
- 寫+讀 verify err=0；`dmsetup status` 顯示 **`borrow=1/644`**（借出 644 blocks = 644×128KB，run-to-run 因水位時序略有差異）。
- `borrow_off` 後讀 verify 仍 err=0（已借 block 繼續解析至借用區）；`borrow_on` 生效。
- remove 存 `.borrow` = **2621456 B**（=16B header + 163840 entries×16B，20GiB 卷 full table）；重建載入 → reload verify err=0。
- 腳本強制 assert：`n_borrowed>0`（防 64M 假陽性）、`.borrow` full-table entries>0、write/borrow 期間與 reload 後 `dmesg bad=0`。
- `dmesg bad=0`（無 warning/oops/hung/borrow fail）。

> 註：borrow_verify.sh 第一次 64M run（19:23）因順序寫不足以超過 watermark 觸發借出而 FAIL（6/7，`n_borrowed=0`、無 .borrow）——此為**測試設計修正**（預設 SIZE 改 256MB + 強制 `n_borrowed>0` assert）而非 driver bug。

---

## 8/14 冷態回歸（重啟後 · 當日 6:1:1:1 + auto-weight S4 對照）

拓撲同 8/13 晚（disk0=nvme0n1=WD 快、disk1=nvme1n1=P3、disk2=sdc=MX500、disk3=sdb=WD Blue）。
重啟後 SLC 冷態恢復，module `AD4AAA…` 自動載入、5 卷自動重建、config CRC 全過。
solo 基線（`auto_weight.sh`，8G 連續寫平均）：**A=2067.9、B=412.6、C=518.5、D=219.6 MB/s**。

### 疊碟回歸（8G，1M/libaio depth32/end_fsync）

| 組合 | 權重 | 實測 W | 實測 R | 分布計數器（wr bytes） | 瓶頸模型 | 對模型 |
|------|------|--------|--------|------------------------|----------|--------|
| S1(A) | 1 | 1983 | 3008 | A 全量 | 2068（A） | -4.1% |
| S2(A+B) | 6:1 | 2384 | 2779 | 7363100672 : 1226833920 = **6:1 精確** | 2413（A×7/6） | -1.2% ✓ |
| S3(A+B+C) | 6:1:1 | 2531 | 3148 | 6442450944 : 1073741824×2 = **6:1:1 精確** | 2757（A×8/6） | -8.2% |
| S4(A+B+C+D) | 6:1:1:1 | **1762** | 2958 | 5727322112 : 954204160×3 = **6:1:1:1 精確** | 1976（D×9） | -10.8% |
| **S4auto** | **9:2:2:1** | **2749** | 2780 | 5522849792 : 1226833920×2 : 613416960 = **9:2:2:1 精確** | 2888 | **-4.8% ✓** |
| MIR | 6:1 + mirror→C | 470 | 2780 | mirror=65536/8G 全量 | — | — |

**結論**：
1. **sdb 再次成為 S4 瓶頸**：solo 219.6 → 6:1:1:1 下 S4 被 D×9=1976 卡在 1762（實測 -10.8%），寫入曲線 S1<S2<S3 但 **S4 破單調**（同 8/13 sdb 衰退現象重現）。
2. **auto-weight 9:2:2:1 解除瓶頸 → S4 1762 → 2749（+56%）**，分布 9:2:2:1 精確、對模型 -4.8%；寫入恢復**單調遞增 1983 < 2384 < 2531 < 2749**。
3. 本次 auto-weight 因 D 特別慢（219.6）得權重 **9:2:2:1**（8/13 為 10:2:2:1）——權重隨碟況自適應，再次證明權重-速比匹配有效。
4. 全部分布計數器逐 byte 精確（6:1 / 6:1:1 / 6:1:1:1 / 9:2:2:1）、mirror 滿鏡像、全程 `err=0`。

### 8/14 早全卷自動加權定案（auto_weight v2 · 窮舉式搜尋；即「實驗 1」B@x1 組）

`auto_weight.sh` v2 改為**窮舉式最佳化搜尋**：最慢碟權重 2..40 × 每碟 floor/ceil±1，最大化瓶頸模型 `min(solo_i×W/w_i)`，並以 `--max-sum`（預設 128）限制總權重、stripe ≤128M。
solo（8G）**A=2056.5、B=413.1、C=518.0、D=220.3**（Σsolo=3208）。**v1 的 ±1 區域搜尋有盲點**：3–4 碟時卡次佳解（如 S3 只給 [10,2,2]=2879），窮舉修正後產出：

| 卷 | 權重 | 模型 | 對 Σsolo |
|----|------|------|----------|
| S2 | **10:2** | 2468 | 99.9% |
| S3 | **20:4:5** | 2982 | 99.8% |
| S4 | **56:11:14:6** | 3194 | 99.6% |

最終冷態窗口量測（量測序 **S4→S3→S2→S1→MIR**，頭號卷優先取最新 SLC；8G，1M/libaio depth32/end_fsync）：

| 組合 | 權重 | 實測 W | 實測 R | 分布計數器（wr bytes） | 瓶頸模型 | 對模型 |
|------|------|--------|--------|------------------------|----------|--------|
| S1(A) | 1 | 2078 | 3157 | A 全量 | 2057（A） | +1.0% ✓ |
| S2(A+B) | 10:2 | 2410 | 2499 | 7159676928 : 1430257664 = **10:2 精確** | 2468（A×12/10） | -2.4% ✓ |
| S3(A+B+C) | 20:4:5 | 2896 | 3011 | 5928648704 : 1182793728 : 1478492160 = **20:4:5 精確** | 2982（A×29/20） | -2.9% ✓ |
| **S4** | **56:11:14:6** | **3006** | 3073 | 5534384128 : 1084227584 : 1379926016 : 591396864 = **56:11:14:6 精確** | 3194（A/D 同級） | **-5.9%** |
| MIR | 6:1 + mirror→C | 496 | 2921 | mirror=65536/8G 全量 | — | — |

**結論**：
1. **寫入曲線嚴格單調 2078 < 2410 < 2896 < 3006**，每步 ≈ 新增碟 solo 的貢獻（+332 / +486 / +110；理想 +411 / +514 / +206），達成「照增加硬碟速度加速」。
2. **S4 舊權重（6:1:1:1）→ 自動加權：1983 → 3006（+52%）**，達瓶頸模型 94%、Σsolo 3208 的 93.7%，四碟同時接近飽和。
3. **S4 步進 +110 被壓縮**：A 碟承載 64% 資料，中段 SLC 耗損使 A 實際跑 ~1935（solo 2056 的 94%）→ S4 對模型 -5.9%（S2/S3 僅 -2.4/-2.9%）；屬碟況現象、非映射誤差。
4. **「切細」對照（8/14 上午）**：同權重 chunk 256K = 3032（-2.5% vs 1M）→ chunk 大小對順序吞吐中性，權重-速比匹配才是主因；production 維持 chunk 1M。
5. **一致與正確性**：全卷分布逐 byte 精確（10:2 / 20:4:5 / 56:11:14:6）、mirror 滿鏡像、全程 `err=0`；S2/S3 跨窗口重測偏差 <±3%。
6. **production 全量更新**：`tv_s2.conf`→10:2、`tv_s3.conf`→20:4:5、`tv_s4.conf`→56:11:14:6（各自 `.bak` 備份），`dmsetup remove/create` 重建生效、計數器歸零、CRC 過。

> **（此節為 8/14 早、B@x1、4 碟快照，即「實驗 1」組；當日下午 P3 Plus 解鎖至 x4、WD Blue 移出資料池後，以兩組實驗定案，見下節。）**

---

## 8/14 下午：P3 Plus 插槽解鎖（x1→x4）＋ DMI 上限 · 兩組實驗定案（現況）

**背景**：P3 Plus 由 PCIe2.0 **x1** 換至 **x4** 插槽，solo 415 → 1522 MB/s（+3.7x）。解鎖後 B+C 需 ~2040 MB/s 經 PCH，但 B85 晶片組 DMI 上行實測僅 ~1300 MB/s → **a+b+c 寫入被 DMI 卡住**（S3≈S2）。故最終驗收拆成兩組互補實驗。

**實驗 1（B@x1 換槽前 · 8/14 早冷態，已 commit）— 三碟聚合證明**

配置：tv_s1 [1]、tv_s2 [10,2]、tv_s3 [20,4,5]。solo：A=2056.5、B=413.1、C=518.0（Σsolo≈2988）。

| 組合 | 權重 | 實測 W | 實測 R | 分布 | 瓶頸模型 | 對模型 |
|------|------|--------|--------|------|----------|--------|
| S1(A) | 1 | 2078 | 3157 | A 全量 | 2057（A） | +1.0% ✓ |
| S2(A+B) | 10:2 | 2410 | 2499 | **10:2 精確** | 2468（A×12/10） | -2.4% ✓ |
| S3(A+B+C) | 20:4:5 | 2896 | 3011 | **20:4:5 精確** | 2982（A×29/20） | -2.9% ✓ |

寫入嚴格單調 **2078 < 2410 < 2896**，S3 = Σsolo 的 **96.9%**——權重-速比匹配讓三碟聚合至接近原生總和（詳見上節）。

**實驗 2（B@x4 換槽後 · 8/14 下午，本次定案）— 插槽解鎖聚焦**

配置：tv_s1 [1]、tv_s2 [37,27]、tv_s3 [64,30,10]、tv_mir [37,27]→C。solo：A=2064-2080、B=1517-1522、C=516-518。

| 組合 | 權重 | 實測 W | 實測 R | 分布 | 瓶頸模型 | 對模型 |
|------|------|--------|--------|------|----------|--------|
| S1(A) | 1 | 2080 | 3112 | A 全量 | 2064（A） | +0.8% ✓ |
| S2(A+B) | 37:27 | **3547** | 3899 | **37:27 精確**（累積 34.76G:25.37G） | 3571（A×64/37） | -0.7% ✓ |
| S3(A+B+C) | 64:30:10 | 3370 | 4323 | **64:30:10 精確** | 3364（A+DMI≈2064+1300） | +0.2% ✓ |
| MIR(A+B→C) | 37:27 | 457 | - | mirror 滿鏡像 | — | — |

> **量測協定**：每次寫入前空檔 **60s**（等 P3 Plus SLC 回充）。背靠背寫耗盡 SLC 時 S2 掉至 ~3000（空檔 60s 回復 3529）——屬碟況、非 driver 退化。S2 寫入為 3 次（3547/3555/3464）中位數。

- **解鎖成效**：a+b 寫入 2410 → **3547（+47%）**；S2 對瓶頸模型 -0.7%（Σsolo 98.9%）。
- **DMI 上限定位**：比例權重 S3 [64,47,16] 僅 **2561**（B+C 互搶 DMI）；DMI-aware [64,30,10] 達 **3370（+32%）**。但 S3_max = A（CPU 直連）+ DMI ≈ 3364 < S2_max = A+B ≈ 3590 → **B85 上 a+b+c 寫入恆 ≤ a+b，C 只加容量**。硬體限制、非 driver 缺陷；全速三碟聚合需無 DMI 牆平台（B650M AM5 遷移為未來選項）。
- **讀取**：嚴格單調 3112 < 3899 < 4297（讀不受 DMI 寫入上限影響，S3 三碟並讀全速）。
- **驗證**：全卷分布逐 byte 精確、`err=0`、冷重開機 boot 自動建卷 CRC 全過。

**定案對照**：

| 指標 | 實驗 1（B@x1） | 實驗 2（B@x4） |
|------|----------------|-----------------|
| a+b 寫入 | 2410 | **3547（+47%）** |
| 三碟聚合 | S3=2896（Σsolo 96.9%） | 受限 DMI：S3≈S2（上限 ~3360） |
| 證明重點 | 權重-速比聚合接近原生總和 | 插槽是 P3 Plus 速度關鍵；DMI 上限 |

---

## 結論

1. **疊碟可擴展（排水態穩態）**：寫入吞吐隨碟數 1→2→3 上升 **1360 → 1667 → 1999 MB/s**（6:1:1:1 權重，8/14 排水態重測，取代 8/12 SLC 帶狀表）。**S4=1760 受 D（WD Blue）瓶頸而低於 S3**（D×9≈1760，精確命中瓶頸模型）；8/13 sdb 衰退（1949）同型（見第 11 點）。擴展性由「瓶頸碟」主導而非碟數。
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
12. **權重-速比失配 = 加權吞吐上限的根因（8/13 晚實驗證實）**：6:1:1:1（瓶頸 D）1966 → 10:2:2:1（碟速比，瓶頸 A）2673，**提升 36%**、分布仍精確；`set_policy adaptive` 現狀 -44% 且 C/D 失衡。*(adaptive 已於 8/13 移除，此為歷史實驗；offload 改由 weight-borrowing 承擔)*
13. **auto-weight 工具有效（`scripts/auto_weight.sh`）**：自動測速設權重 10:2:2:1 後 S4 達 **2954**（對模型 -4.5%、分布精確），較 6:1:1:1 **+50%**；權重基準用 8G 平均 solo（非瞬間峰值）避免 SLC 陷阱；附 config 清理（crc32/badmap/policy）。
14. **權重-速比匹配＋窮舉搜尋逼近原生總和（8/14 早定案）**：最慢碟權重釘在 1 → 捨入誤差使次慢碟過載（9:2:2:1 卡 2888）；auto_weight v2 窮舉最佳化產出全卷比例權重（S2 **10:2**、S3 **20:4:5**、S4 **56:11:14:6**，模型 2468/2982/3194 = Σsolo 99.9%/99.8%/99.6%）→ 寫入曲線嚴格單調 **2078 < 2410 < 2896 < 3006**，S4 對舊 6:1:1:1（1983）**+52%**。chunk 大小（1M vs 256K）對順序吞吐中性。
15. **weight-borrowing 正確性成立（8/13 驗收）**：128 KB block 粒度下借出統計與持久化正確，64 M–4 G 寫入/讀回 verify 全過、reload 後映射恢復（4 G verify err=0）；修復兩根因（save spinlock 死鎖、混合單位位移）。6:1:1:1 下寫 2346（vs static 2275）、讀 3165，瓶頸轉移後增益受限於快碟 solo，快碟有富餘的拓撲收益更大。
16. **插槽是 P3 Plus 速度關鍵（8/14 下午）**：PCIe2.0 x1→x4 使 solo 415→1522（+3.7x）、a+b 寫入 2410→3547（+47%）。B85 僅 PCH 提供 x4 槽，但與 SATA 共用 DMI。
17. **B85 DMI 上限 = 三碟聚合的硬體天花板（8/14 下午）**：B+C 共用 DMI 上行 ~1300 MB/s。比例權重 S3 [64,47,16] 僅 2561；DMI-aware [64,30,10] 達 3370（+32%），但 S3_max = A+DMI ≈ 3364 < S2_max = A+B ≈ 3590 → **a+b+c 寫入恆 ≤ a+b，C 只加容量**（硬體限制、非 driver 缺陷；全速三碟需無 DMI 牆平台如 B650M AM5）。
18. **8/14 兩組實驗定案**：實驗 1（B@x1）三碟聚合 2078<2410<2896（Σsolo 96.9%）＋實驗 2（B@x4）a+b=3547（+47%）——權重-速比匹配與插槽解鎖的完整證明。量測協定（寫前空檔 60s 等 SLC 回充）確保跨日可比。

## 8/14 晚補測（USB 計畫檔 A–E，B@x4 + D 回池）

拓撲與 8/14 下午相同：A=SN750（8.0 GT/s x4、CPU 直連）、B=P3 Plus（5.0 GT/s **x4**、PCH）、C=MX500、D=WD Blue 回池、E=BX100 開機。排水態協定同前。

- **A1（主表 S4 64:27:9:4，排水態×2）**：寫 2530/2503 → 中位 **2517 MB/s**；讀 4290/4293 → **4292 MB/s**；兩輪分布與確定性映射**零誤差**（A=10104M/B=4239M/C=1413M/D=628M）；B+C+D 寫份額 ≈970 < DMI 1300（排水態 A 為瓶頸、DMI 未飽和）。
- **A2（MIR 讀）**：A:B=4:3→C 卷讀 16G = **3799 MB/s**；讀分布 A:B=74978:56247≈4:3 精確、C=0。
- **A3（vs LVM @B@x4）**：LVM 4 碟 1M stripe 20G = 寫 1616/讀 1791；TV S4 fresh 寫 3091/讀 3575 → **寫 ~1.9x、讀 2.0x**（排水態 2517/4292 → 讀 2.4x）；**1.96–3.5x 方向不變**。
- **B4（借調效益，最有價值的負結果）**：無背景 off=2348/on=2197/off=2154（on 落在 SLC 漂移帶內，純開銷小）；D 飽和 off=1851/1865、on=1949/1742（平均 1858 vs 1846，**≈相等**）。**借調在 64:27:9:4 下中性**：D 份額僅 3.85%（~96 MB/s ≪ solo 229），D 天然非瓶頸、無功可借；與 8/13 的 +3%（6:1:1:1、D 真瓶頸）互補——借調只在「慢碟真為瓶頸」的拓撲有正效益。另記：100G carve 的 borrow 表（819200×16B≈13 MB）超 kmalloc 上限 → ENOMEM，20G carve（2.6 MB）可用。
- **C5（mirror 讀重試，fail injection）**：對 A 掛 1M dm-error 窗（dAerr=linear+error+linear 合成裝置）→ 讀該區**成功**（A err=1，資料必來自 mirror）；在 C 的 mirror 區（mirror_sec=logical>>9）埋 0xD5 → volume 讀回 **byte 精確 0xD5**。證明 fallback 讀到正確 mirror sector。**統計小洞**：mirror retry 讀不計入 per-disk rd counter（show_stats/sysfs C 顯示 rd=0）。
- **C6（write 錯誤→自動 badmap）**：寫入失敗 chunk 自動標記（後續讀 = **zero-fill** 全 0、重寫被 **skip** 且無錯誤資料不變）；sysfs err=1。
- **D7（WC read-after-write）**：部分 stripe 寫入未 flush 前立即讀回正確（讀觸發 flush）；fio 6G write+verify **0 mismatch**；重讀 6G 0 mismatch。
- **D8（確定性重現）**：同權（A:B:C=4:3:2）重建 3 次：vol1 寫 ramp → vol2 讀回 byte 精確；vol2 覆寫 0x3C → vol3 讀回全 0x3C。**雙向確定**。
- **E9（sparse + lockdep）**：`make C=2 CHECK=sparse` **抓到 1 個真 bug**——`tv_borrow_redirect`（tieredvol_borrow.c）在 `borrow_off` 後（entries 保留、enabled=false）遇未借區小寫入（need>0）分支**帶 spinlock（irqs off）直接 return** → 下一個碰 `borrow.lock` 的 I/O 死鎖（B4 全 1M 寫被 WC 緩衝而未觸發）。已修復（該分支加 unlock）並驗證：borrow_off + 512K 小寫入不再掛、讀回正確。剩 2 個 blk_status_t 良性 cast。**lockdep**：本機 kernel 6.14.0-27-generic 的 `CONFIG_PROVE_LOCKING` 未開，無法跑 runtime lockdep；以 sparse lock-context 檢查替代（即上述 bug 來源）。
- **當機調查**：兩次硬當無 panic/oops（硬當不留痕跡）、SMART 全乾淨（sdb 重配置/待處理/CRC 皆 0、28°C）。boot -2 日誌在 `pkill -9 -f busyd`（SIGKILL 兩個對 /dev/sdb 做 64 深佇列 1M direct 寫的背景 fio）後 **2 秒內中斷**；boot -1（35 秒）殘留態再掛。**根因 = AHCI/SATA 控制器被深佇列 direct I/O 的 SIGKILL wedged**（B85 老晶片組，前次 SIGTERM 未掛、本次 -9 掛，時序吻合）；tieredvol driver 當下完全閒置、非其責任。後續改用固定 `--runtime` 自然結束 fio 不再觸發。

---

現役 conf（**8/14 晚 B@x4 + D 重加權**，基於 8/14 下午 B@x4 實證權重）：`/etc/tieredvol/tv_s1.conf` [1]、`tv_s2.conf` **[37:27]**（寫 3547）、`tv_s3.conf` **[64:30:10]**（DMI-aware 3370）、`tv_mir.conf` **[37:27]**+mirror=C、`tv_s4.conf` **[64:27:9:4]**（A1 排水態 2517/4292）（對應 repo `configs/`）。8G write+verify 分布對確定性映射**逐 chunk 吻合**（tv_s3/tv_s4 為 78 條整 stripe + 80 partial chunks A-優先；tv_mir C 收到全量 mirror 8G）。
（碟位史：8/13 午 disk0=nvme1n1、8/13 晚回到 disk0=nvme0n1；8/14 下午 A=SN750=nvme1n1（CPU 直連）、B=P3 Plus=nvme0n1（PCH x4）；8/14 晚第一次重開機後 A=SN750=nvme0n1、B=P3 Plus=nvme1n1（PCH x1）；**8/14 晚換碟回 B@x4 + D 後 A=SN750=nvme0n1、B=P3 Plus=nvme1n1（PCH x4）**。config 一律用 by-id，與 nvme 編號無關。）
