# Test Plan — TieredVol-DRIVER

> 主目標：**證明「疊加硬碟」可行**（1→2→3→4 disk，分布精確、資料完整、容量擴展）。
> 速度為參考指標，非驗收條件。
> 實測結果已紀錄於 [RESULTS.md](RESULTS.md)（2026-08-11，現況拓撲）。

---

## 硬碟代號（目前拓撲，2026-08-12 重啟後）

```
A = nvme0n1  WD SN750 (PCIe 3.0 x4, 快)    B = nvme1n1  P3 Plus (PCIe 2.0 x1, 慢)
C = sdc      MX500 (SATA 6G)               D = sdb      WD Blue (SATA 6G)
E = sda      BX100（未入 config）
```

完整型號/容量/link 速度/平行仲裁測速見 [RESULTS.md](RESULTS.md)「硬碟對照」。

> **2026-08-12 重啟後碟位交換**：快碟現為 nvme0n1（WD SN750）、慢碟 nvme1n1（P3 Plus）；現況權重 **6:1:1:1**。

---

## 量測方法論

### fio 參數

```bash
# 寫
sudo fio --name=w --filename=/dev/mapper/<name> --rw=write --bs=1M --size=8G \
  --direct=1 --ioengine=libaio --iodepth=32 --numjobs=1 --end_fsync=1

# 讀
sudo fio --name=r --filename=/dev/mapper/<name> --rw=read --bs=1M --size=8G \
  --direct=1 --ioengine=libaio --iodepth=32 --numjobs=1
```

- **用 `libaio`，不要用 `io_uring`**：io_uring + dm + 深佇列會灌高假數字，無法反映真實吞吐。
- `iodepth=32`；`--size=8G` 固定，配合確定性映射才能精確比對計數器。

### 驗收判據（疊碟測試）

1. **分布精確**：寫入恰好 8G 後，`dmsetup status` 每碟 wr bytes 對應 chunk 數：
   **完整 stripe 依權重精確分配；非完整 stripe 的餘量 chunk 依映射落入前段碟**
   （確定性 O(1) 映射，0 誤差、每 byte 恰落一碟）。可先以 256M 快速 dry-run 驗計數器。
2. **資料完整**：`fio --verify=crc32c --do_verify=1` 0 mismatch。
3. **容量擴展**：volume 大小由 `seg0_end` 宣告；加大 carve 需各碟容量足夠承載。
4. **速度**（參考）：快碟不被慢碟拖垮；**不作為通過/失敗條件**。

---

## 主測試：疊碟擴展（Scale-out）— **已完成**

依目前拓撲由 1 碟往上疊，每步獨立 conf + dmsetup create。權重以**平行仲裁**
（各碟同時打 raw、取實際並行吞吐，非 solo 測速）調校，現況為 **6:1:1:1**。

| ID | 碟數 | 碟組合 | 權重 | 驗證 | 狀態 |
|----|------|--------|------|------|------|
| S1 | 1 | A | [1] | 單碟基線、計數 100% | ✅ 完成 |
| S2 | 2 | A+B | 6:1 | 分布精確 | ✅ 完成 |
| S3 | 3 | A+B+C | 6:1:1 | 分布精確 | ✅ 完成 |
| S4 | 4 | A+B+C+D | 6:1:1:1 | 分布精確 + 完整性（crc32c 8G 全量） | ✅ 完成 |
| CAP | 4 | 100G carve | 6:1:1:1 | 容量擴展 + 分布精確 | ✅ 完成 |

結果與逐項對照（預測值 vs 計數器）見 [RESULTS.md](RESULTS.md)。

**執行步驟（供複測/擴充）**：
1. 建 conf → `dmsetup create <name>`
2. `dmsetup message <name> 0 reset_io_stats`
3. fio write 8G（libaio）
4. `dmsetup status`：比對每碟 wr bytes vs 權重
5. fio read + `--verify=crc32c`：完整性 0 mismatch
6. `dmsetup table`：確認 size = seg0_end
7. `dmsetup remove`

---

## Mirror / Rebuild 專項（2026-08-12）— **已完成**

拓撲：`/home/yu/tv_mir.conf`（A=nvme1n1 權重8 + B=nvme0n1 權重1 的 8:1 stripe、mirror→C=sdc，chunk=1MiB、stripe=9MiB）。

### 本日根因修正
1. **Rebuild 崩潰（⑤，首次執行即爆）**：`tv_rebuild_thread` 只 `alloc_page` 單頁卻把整 chunk(1MiB) 塞進 `bio_add_page`。6.x `bio_add_page` **不截斷長度**（multi-page bvec 語意、回傳傳入值）→ 長度檢查形同虛設，送出「bi_size=1MiB(手動)+1MiB(bio_add_page)=2MiB、實體僅 1 頁」的 bio → `__blk_rq_map_sg` WARN + `iommu_dma_map_sg` NULL deref → oops 級聯 → 系統凍結重啟。修法：`alloc_pages(GFP_NOIO, get_order(sz))` compound page 涵蓋整個 chunk、**移除手動 `bi_iter.bi_size`**、`sz = min(chunk_bytes, cur.length)` 並以 `sz` 推進（一併修跨 stripe 邊界越界讀隱患）。
2. **STALE 洪水**：原「無 in-flight I/O 滿 5s 即 STALE」把閒置碟標 STALE（與 MAPPING.md 設計相反）且無窮刷 log。改為 **hung 偵測**：僅「有 in-flight 卻連續 5s 零完成」才標 STALE，閒置碟永不標；排空/恢復完成立即 RECOVERED。
3. **ctr 錯誤路徑**：`tv_mirror_init_ctx` 成功後若後續 ctr 失敗（chunk 幾何、`dm_set_target_max_io_len`），補 `tv_mirror_destroy_ctx()` 防 mempool/percpu 洩漏。
4. Mirror 寫入（copy 式 COW，本日修復前已完成）已過 5GB soak 驗證。

### 測試結果（本日實測）
- **rebuild smoke 64MiB**：2s 完成，sdc[0,64M)==pat64、裝置讀回全對。
- **rebuild 完整 260MiB**（先破壞 sdc[256M,260M) 為 0xAA）：1.3s 完成，sdc[256M,264M) 還原==pat64、裝置讀回全對。
- **mirror 同步 + 修復回歸**：寫入後 sdc 逐 byte 相符；64M 寫+讀回+COW 全對。
- **STALE 驗證**：閒置 60s、I/O burst、rebuild 期間 0 筆 STALE/RECOVERED（原每 5s 一筆）。
- dmesg：全程 0 WARNING/Oops/BUG。

---

## 次要測試（**已完成**）

### WC 專項：小寫入合併效益

`fio --bs=4K --size=1G --iodepth=1`（單 depth，避免平行蓋掉 WC 效果）

> **注意**：`wc_enabled` 是 **module param**。W1/W3 需 `dmsetup remove` + `rmmod` 後 `insmod tieredvol.ko wc_enabled=1`、W2/W4 用 `wc_enabled=0`。用單碟 conf（`/etc/tieredvol/tv_s1.conf`）隔離 WC 效應。

| ID | WC | 描述 |
|----|----|------|
| W1 | on | 4K 隨機寫入，WC 應合併成 1MB chunk |
| W2 | off | 同上，關 WC，每筆 4K 直接送 disk |
| W3 | on | 4K 循序寫入 |
| W4 | off | 4K 循序寫入，關 WC |

**驗收**：W1/W3 的 IOPS 應顯著高於 W2/W4。

### Mirror 專項

| ID | 測試 | 步驟 | 預期 |
|----|------|------|------|
| M1 | Mirror write + primary 失效 | 1. 建 A+B（mirror→C）<br>2. 寫 256M 同步 mirror<br>3. 破壞 primary + set_badmap<br>4. 讀 256M | 讀取由 mirror 回補 | ✅ 完成（badmap 模擬；pw 鎖修復後 retest 全過） |
| M2 | Mirror rebuild | 破壞 mirror → `start_rebuild` 還原 | mirror_err=0、資料完整 | ✅ 完成（rebuild 崩潰已修；鎖修復後回歸 mirror==primary PASS） |
| M3 | Cross-disk mirror | 4K iodepth=1 寫入，跨 disk boundary 也正確 mirror | mirror_err=0、ratio 正確 | ✅ 完成（鎖修復後回歸 +2 ops/+8K 精確） |

> 測試陷阱：rebuild 的 bio 寫入不更新 block device page cache，驗證 mirror 碟必須用 `dd iflag=direct`，否則 buffered read 命中舊 cache 誤判「未還原」。

### 功能驗證

| ID | 測試 | 步驟 | 預期 |
|----|------|------|------|
| F1 | Virtual device reject | `--disks /dev/loop0` | 拒絕，提示 virtual device |
| F2 | Lifecycle | create → bench → show_stats → remove → recreate | 全部成功 |
| F3 | DM message | show_mirror / show_stats / show_log / set_policy / reset_stats | 正確輸出 |
| F4 | Data integrity | `fio --verify=crc32c --do_verify=1 --bs=4K --size=2G` | 0 mismatch |
| F5 | Badmap | `dmsetup message` set_badmap → 讀該 chunk | 有 mirror：讀回 mirror 資料；無 mirror：zero-fill。不 I/O error |

---

## LVM 對比（現行拓撲，2026-08-12）— **已完成**

與 LVM fixed striped 公平對比，**雙方同用 fio libaio iodepth=32 direct=1 bs=1M size=8G end_fsync=1**（= 疊碟驗收標準），非舊拓撲 io_uring 灌水數字。

| 碟數 | TieredVol W / R | LVM striped W / R | TV/LVM 寫 | TV/LVM 讀 |
|------|------------------|-------------------|-----------|-----------|
| 2d（A+B） | 2661 / 2786 | 786 / 796 | 3.39x | 3.50x |
| 3d（A+B+C） | 2787 / 3180 | 1180 / 1190 | 2.36x | 2.67x |
| 4d（A+B+C+D） | 3091 / 3575 | 1575 / 1590（256K 最佳） | 1.96x | 2.25x |

- **LVM 4d stripe sweep**：64K=1059/1513、128K=1525/1587、256K=**1575/1590**、1M=1572/1589（256K 最佳，128K 接近）。
- **4K 小寫入（2G d32）**：TieredVol S4=511 vs LVM 4d=676 MiB/s → **LVM 較快 1.32x**（LVM 4K 平行散至 4 碟；TieredVol 4K 走 WC 繞過直寫，75% 落在 A 單碟）。
- 結論：1M 順序大塊 TieredVol 全面領先（1.96–3.5x，快碟占比高）；4K 小塊 LVM 略勝。
- 操作：`pvcreate 4 碟 → vg_tv2 → lvcreate --stripes N --stripesize S`；測完 `vgchange -an/vgremove/pvremove` 全數還原裸碟（TieredVol 功能檢查通過）。

---

## 執行狀態

| 測試 | 狀態 | 結果位置 |
|------|------|----------|
| S1–S4 疊碟 + CAP | **完成**（2026-08-11；08-12 鎖修復後 6:1:1:1 重跑，15:35 冷態） | `docs/RESULTS.md` |
| S4 8G 全量完整性 | **完成**（2026-08-12，crc32c 8G write+verify 0 mismatch、計數 6:1:1:1 恰合） | `/home/yu/verify8g_s4_*.log` |
| Mirror / Rebuild 專項（含 5 項修復） | **完成**（2026-08-12） | 本文件 + `docs/RESULTS.md` |
| WC 專項 W1–W4 | **完成**（2026-08-12） | `docs/RESULTS.md` |
| Mirror 專項 M1–M3 | **完成**（2026-08-12，pw/pr 鎖修復後回歸全過） | `docs/RESULTS.md` |
| LVM vs TieredVol 對比（2/3/4d + sweep） | **完成**（2026-08-12） | 本文件 + `docs/RESULTS.md` |
| 功能驗證 F1–F5 | **完成**（2026-08-12 最終 build 重驗，`/home/yu/f_suite.sh` **17/17 PASS**） | `docs/RESULTS.md` |
