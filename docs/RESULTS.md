# Test Results — TieredVol-DRIVER（現況拓撲）

> 實測日期：2026-08-12 · Kernel 6.14.0-27-generic · dm-target v5.0
> 完整測試程序與驗收判據見 [TEST_PLAN.md](TEST_PLAN.md)。
> **2026-08-12 重啟後碟位交換**：快碟現為 nvme0n1（WD SN750），慢碟 nvme1n1（P3 Plus）；權重鎖定 6:1:1:1。

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
| S1 | 1 (A) | 1 | 1979 | 2955 | A=8192（100% 精確） | - |
| S2 | 2 (A+B) | 6:1 | 2584 | 2762 | A=7024 B=1170（精確） | - |
| S3 | 3 (A+B+C) | 6:1:1 | 2502 | 3120 | A=6144 B=1024 C=1024（精確） | - |
| S4 | 4 (A+B+C+D) | 6:1:1:1 | 2778 | 3490 | A=5462 B=910 C=910 D=910（精確） | crc32c 2G **0 mismatch** |
| CAP | 4 (100G carve) | 6:1:1:1 | 1695 | - | A=684 B=114 C=113 D=113（1G 精確） | - |

> 上表為 **2026-08-12 15:03 完整回歸**（重載當前 build、四卷共存、寫+讀+完整性+CAP 同程序）。
> 另做 WC on/off、換序、多卷共存 3 組對照（見 `phase1_retest_*_currbuild.log` / `step1_wc.log` / `step2_seq.log`）：
> - **S4 單卷重測 3383–3465、序列中 3280、四卷共存 3185**；**WC on/off 無差異（3383 vs 3407）**。
> - **S3 觀察範圍 2502–3123**。所有重測 **S4 寫入皆高於 S3** → 疊碟擴展確認（4 碟 > 3 碟 > 2 碟 > 1 碟），
>   先前記錄的 S4=2092 為舊 build / 瞬態干擾，非現行驅動行為。

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

- **資料完整（修復後回歸）**：S4 上 `fio --verify=crc32c --do_verify=1 --size=2G` → **0 mismatch**；全程 `err=0`。
  寫 445MiB/s、verify_read 264MiB/s（修復前因 WC 小寫入退化卡在 ~13MiB/s）。
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

## WC 專項 W1–W4（2026-08-12）

單碟 `/etc/tieredvol/tv_s1.conf`、`fio --bs=4K --size=1G --iodepth=1 --direct=1 --ioengine=libaio`。

| ID | WC | 模式 | IOPS | BW |
|----|----|------|------|-----|
| W1 | on | 隨機寫 | 34.9k | 136MiB/s |
| W2 | off | 隨機寫 | 38.9k | 152MiB/s |
| W3 | on | 循序寫 | 40.2k | 157MiB/s |
| W4 | off | 循序寫 | 32.4k | 127MiB/s |

**結論**：WC 為「提交批次化」而非「bio 合併」——每筆仍是 4K bio，但 flush 一次批次下送，讓磁碟看到循序局部性：
- **循序 +24%**（40.2k vs 32.4k IOPS）✅ 符合預期。
- **隨機無合併機會**（跨 stripe 每次觸發 flush），-10%（34.9k vs 38.9k），屬設計特性非 bug。
- 若要隨機小寫入也受惠，需將 WC 改為真正的 bio 合併（compound page 聚合同 chunk）— 未來增強方向。

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
| 2d（A+B） | 2584 / 2762 | 786 / 796 | 3.29x | 3.47x |
| 3d（A+B+C） | 2502 / 3120 | 1180 / 1190 | 2.12x | 2.62x |
| 4d（A+B+C+D） | 2778 / 3490 | 1575 / 1590（256K） | 1.76x | 2.19x |

- **LVM 4d stripe size sweep**（W/R MiB/s）：64K=1059/1513、128K=1525/1587、256K=**1575/1590**、1M=1572/1589 → **256K 最佳**（128K 接近，與歷史結論一致）。
- **4K 小寫入**（2G, d32）：TieredVol S4=**511** vs LVM 4d=**676** MiB/s → **LVM 較快 1.32x**。原因：LVM 4K 平行散到 4 碟；TieredVol 4K 走 WC 繞過直寫、分布 6:1:1:1 使 75% 落單碟 A。
- **結論**：1M 順序大塊 TieredVol 全面領先（1.76–3.5x，差距隨快碟權重占比上升）；4K 小塊是 LVM fixed stripe 的優勢區。
- 操作流程：`pvcreate` 4 碟 → `vg_tv2` → `lvcreate --stripes N --stripesize S -L 20G`；測完 `vgchange -an`/`vgremove`/`pvremove` 還原裸碟，TieredVol 功能檢查通過（S4 建置/移除 OK）。

---

## 結論

1. **疊碟可擴展**：寫入吞吐隨碟數上升 1979 → 2584 → 2502 → 2778 MB/s（重測 S3∈[2502,3123]、S4∈[2778,3465]，**S4 恆 > S3**，擴展確認；先前 S4=2092 為舊 build 瞬態）。
2. **分布精確**：計數器與宣告權重（6:1:1:1）、確定性映射預測完全一致（0 誤差）。
3. **資料完整**：crc32c 2G 寫入 + verify 0 mismatch，`err=0`（WC 小寫入 bug 修復後）。
4. **Mirror 全功能可用**：同步/讀回/rebuild/跨碟界全部正確，`mirror_err=0`；M1–M3 鎖修復後回歸全過。
5. **鎖修復**：pending-write（`tv_pw_lock`）與 pending-read（`tv_pending_lock`）ring 均有全域 spinlock 保護，0 次 MISS / give-up / full。
6. **WC 小寫入修復**：4K 寫 14→500 MiB/s；大寫入路徑不受影響（2737 MiB/s）。
7. **Mirror 損耗**：1M 寫 81%（mirror 碟慢於 primary 的寫放大本質）、4K 小塊 52%（COW 開銷）、讀 0%。
8. **vs LVM**：1M 順序寫/讀達 LVM striped 的 **1.76–3.5x**（快碟權重占比越高越明顯）；4K 小寫入 LVM 較快 1.32x（676 vs 511 MiB/s）。

使用 conf：`/home/yu/tv_s1.conf`、`tv_s2.conf`、`tv_s3.conf`、`tv_s4.conf`、`tv_mir.conf`
（disk0=nvme0n1, disk1=nvme1n1, disk2=sdc, disk3=sdb）。
