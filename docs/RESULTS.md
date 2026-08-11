# Test Results — TieredVol-DRIVER（現況拓撲）

> 實測日期：2026-08-11 · Kernel 6.14.0-27-generic · dm-target v5.0
> 完整測試程序與驗收判據見 [TEST_PLAN.md](TEST_PLAN.md)。

---

## 硬碟對照（目前拓撲）

```
代號    裝置        型號                    容量   介面           PCIe link   平行仲裁測速
A    = nvme1n1    WD SN550 WDS500G3X0C     500G   PCIe 3.0 x4   8.0 GT/s    2720 MB/s
B    = nvme0n1    P3 Plus CT1000P3PSSD8    1T     PCIe 2.0 x1   5.0 GT/s    407 MB/s
C    = sdc        MX500 CT500MX500SSD1     500G   SATA 6G       -           489 MB/s
D    = sdb        WD Blue WDS250G2B0A      250G   SATA 6G       -           324 MB/s
E    = sda        BX100 CT250BX100SSD1     233G   SATA 6G       -           （未入 config）
```

- **插槽角色已交換**：A（WD SN550）在 PCIe3.0 x4 = 快碟；B（P3 Plus）在 PCIe2.0 x1 = 慢碟。
- **平行仲裁** = 4 碟同時 raw 寫入 10s（fio 1M libaio depth=32）。solo 短寫會吃到 SLC cache
  （A~2563、B~413、C~517、D~227 MB/s），平行仲裁才是 driver dispatch 時的真實並行速度。

---

## 權重調校

```
平行仲裁測速：A=2720  B=407  C=489  D=324 (MB/s)
除以最慢碟：   8.40   1.26   1.51   1.00
取整：         8      1      2      1
```

權重鎖定 **8:1:2:1**（sum=12）。子集合沿用：S2 [8,1]、S3 [8,1,2]、S4 [8,1,2,1]。

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

## 疊碟結果

| ID | 碟數 | 權重 | Write(MB/s) | Read(MB/s) | 分布（chunk，8G=8192） | 完整性 |
|----|------|------|-------------|------------|------------------------|--------|
| S1 | 1 (A) | 1 | 2083 | 3149 | A=8192（100% 精確） | - |
| S2 | 2 (A+B) | 8:1 | 2587 | 3529 | A=7282 B=910（精確） | - |
| S3 | 3 (A+B+C) | 8:1:2 | 2788 | 2730 | A=5960 B=744 C=1488（精確） | - |
| S4 | 4 (A+B+C+D) | 8:1:2:1 | 2972 | 3006 | A=5464 B=682 C=1364 D=682（精確） | crc32c **0 mismatch** |
| CAP | 4 (100G carve) | 8:1:2:1 | 3016 | - | A=684 B=85 C=170 D=85（1G 精確） | - |

### 分布驗證（對照確定性映射預測值）

| ID | 推算 | 計數器 | 一致 |
|----|------|--------|------|
| S2 | 8192M=910 完整 stripe(9M)+2M 餘 → A=7280+2=7282, B=910 | A=7282 B=910 | ✓ |
| S3 | 8192M=744 完整 stripe(11M)+8M 餘 → A=5952+8=5960, B=744, C=1488 | A=5960 B=744 C=1488 | ✓ |
| S4 | 8192M=682 完整 stripe(12M)+8M 餘 → A=5456+8=5464, B=682, C=1364, D=682 | A=5464 B=682 C=1364 D=682 | ✓ |
| CAP | 1024M=85 完整 stripe(12M)+4M 餘 → A=680+4=684, B=85, C=170, D=85 | A=684 B=85 C=170 D=85 | ✓ |

> 餘量 chunk 全落入各 stripe 前端碟（A），與權重順序一致的確定性行為。

---

## 完整性與容量

- **資料完整**：S4 上 `fio --verify=crc32c --do_verify=1 --size=2G` → **0 mismatch**；全程 `err=0`。
- **容量擴展**：volume 大小由 `seg0_end` 宣告。S1–S4 用 20G carve 驗證分布；另建 100G carve
  （`0 209715200 tieredvol ...`）成功，1G 寫入分布仍精確。
- 所有 volume 用後即 `dmsetup remove`，無殘留 device。

---

## Mirror / Rebuild（2026-08-12）

拓撲 `tv_mir.conf`：A=nvme1n1(w8) + B=nvme0n1(w1) stripe、mirror→C=sdc，chunk 1MiB、stripe 9MiB。

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
- **M2（rebuild 還原 mirror）**：還原 primary → 破壞 sdc[64M,68M) → `start_rebuild 0 268435456`（256M，1.25s）→ sdc[64M,68M) 還原正確、整碟 mirror==pat64×4、裝置讀回全對、mirror_err=0。
- **M3（跨碟界 mirror）**：`dm_set_target_max_io_len`=1MiB(=chunk) 保證單一 bio 不會跨碟；跨碟正確性由 chunk 切分 + 整 bio COW 保證。實測 boundary 兩側（logical 8MiB-4K 與 8MiB）各寫 4K：裝置讀回全對、mirror 逐 byte 相符、nvme1n1/nvme0n1 各存正確一半 ✅。

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

## Mirror 損耗（2026-08-12）

同條件對照：`tv_s2`（A+B stripe，無 mirror）vs `tv_mir`（同 stripe + mirror→C=sdc）。libaio。

| 工作負載 | tv_s2 | tv_mir | 損耗 |
|----------|-------|--------|------|
| 1M / depth32 / 8G 寫 | 442 MiB/s | 442 MiB/s | **0%** |
| 1M / depth32 / 8G 讀 | 446 MiB/s | 446 MiB/s | **0%** |
| 1M / depth32 / 2G 寫（SLC 溫期） | 441 MiB/s | 441 MiB/s | **0%** |
| 4K / depth8 / 2G 寫 | 396 MiB/s | 280 MiB/s | **29%** |

**重要 caveat**：本次實測時 nvme1n1 已節流（raw 現測僅 393 MiB/s，早先仲裁 2720）→ 1M 寫的瓶頸在 primary（~442）而非 mirror 碟 C（run 期間 sdc 吃到 464 MiB/s，完全跟上）。因此：
- **大塊寫/讀損耗 0%** 是「primary 慢於 mirror 碟」時的結果。若 primary 恢復全速（2720）而 C 只有 ~489，mirror 會卡在 C，理論損耗 ≈ 1 − 489/2720 ≈ **82%**（寫放大本質，非 driver 開銷）。
- **4K 小塊 29%** 才是 driver COW 開銷的直接量度：每筆 4K COW 需 `alloc_page` + 2×kmap + `memcpy` + `bio_add_page` + completion 追蹤（tieredvol_mirror.c:276-325），與 primary 無關。
- 讀路徑完全不碰 mirror（除非 badmap 回補），故讀損耗恆為 0%。

**session 累計 mirror 計數器**：`mirror=606208 ops / 12 GiB，err=0`；全碟狀態 A。

---

## 結論

1. **疊碟可擴展**：寫入吞吐隨碟數上升 2083 → 2587 → 2788 → 2972 MB/s（疊碟後超過單碟基線）。
2. **分布精確**：計數器與宣告權重、確定性映射預測完全一致（0 誤差）。
3. **資料完整**：crc32c 寫入 + verify 0 mismatch，`err=0`。
4. **Mirror 全功能可用**：同步/讀回/rebuild/跨碟界全部正確，`mirror_err=0`；rebuild 崩潰已修復。
5. **STALE 修正**：改 hung 偵測後，閒置/I/O/rebuild 期間 0 誤報。
6. **WC 循序有效**：+24% IOPS；隨機無合併機會（設計特性，bio 合併為未來增強）。
7. **Mirror 損耗**：大塊寫/讀 0%（primary 為瓶頸時）、4K 小塊 29%（COW 開銷）；mirror 碟比 primary 慢時損耗由寫放大本質決定。

使用 conf：`/home/yu/tv_s1.conf`、`tv_s2.conf`、`tv_s3.conf`、`tv_s4.conf`
（disk0=nvme1n1, disk1=nvme0n1, disk2=sdc, disk3=sdb）。
