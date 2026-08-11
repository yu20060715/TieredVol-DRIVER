# Test Plan — TieredVol-DRIVER

> 主目標：**證明「疊加硬碟」可行**（1→2→3→4 disk，分布精確、資料完整、容量擴展）。
> 速度為參考指標，非驗收條件。

---

## 硬碟對照（目前拓撲）

```
代號    裝置        型號                   容量   介面            PCIe link   測速
A    = nvme1n1    WD SN550 WDS500G3X0C    500G   PCIe 3.0 x4     8.0 GT/s    ~2563 MB/s
B    = nvme0n1    P3 Plus CT1000P3PSSD8   1T     PCIe 2.0 x1     5.0 GT/s    ~413 MB/s
C    = sdc        MX500 CT500MX500SSD1    500G   SATA 6G        -            ~517 MB/s
D    = sdb        WD Blue WDS250G2B0A     250G   SATA 6G        -            ~227 MB/s
E    = sda        BX100 CT250BX100SSD1    233G   SATA 6G        -            （未入 config）
```

> **插槽角色已交換**：A（WD SN550）在 PCIe3.0 x4 = 快碟；B（P3 Plus）在 PCIe2.0 x1 = 慢碟。
> 早先實驗（tv3x/tv4x）是在舊拓撲（nvme0=WD 快、nvme1=P3 慢）下調的權重，**已不適用於目前拓撲**，
> 需依「現況拓撲」重新調權重後再跑疊碟測試。

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

- **用 `libaio`，不要用 `io_uring`**：io_uring + dm + 深佇列會產生誇張的假數字（大量 CQE 完成計數，
  bi_size 被重複累計），無法反映真實吞吐。
- `iodepth` 用 32 層級即可；`--size=8G` 固定，配合確定性映射，計數器才能**精確**比對。

### 驗收判據（疊碟測試）

1. **分布精確**：寫入**恰好 8G** 後，`dmsetup status` 的每碟 wr bytes
   = 8G × weight_i/Σweight，**無誤差**（映射是確定性純算術，每 byte 只落在一個碟）。
2. **資料完整**：`fio --verify=crc32c --do_verify=1`（或 write 後 read 對 hash）0 mismatch。
3. **容量擴展**：新增一碟 → volume 大小 = Σ 參與碟容量（目前 conf 用 20G carve）。
4. **速度**（參考）：快碟不被慢碟拖垮；但**不作為通過/失敗條件**。

---

## 主測試：疊碟擴展（Scale-out）

依目前拓撲由 1 碟往上疊。每個步驟建立獨立 conf（`/home/yu/tv_add<k>.conf`）並 dmsetup create。

| ID | 碟數 | 碟組合 | 權重（現況拓撲調校） | 驗證 |
|----|------|--------|----------------------|------|
| S1 | 1 | A | [1] | 基準：單碟滿速、計數器 = 100% |
| S2 | 2 | A+B | 待調（A≫B） | 分布精確 + 完整性 + 容量 = A+B |
| S3 | 3 | A+B+C | 待調 | 分布精確 + 完整性 + 容量 = Σ |
| S4 | 4 | A+B+C+D | 待調 | 分布精確 + 完整性 + 容量 = Σ |

**權重調校方法**：以**平行仲裁**（各碟同時打 raw、看實際並行吞吐）為準，
不是單碟 solo 測速（solo 會因 DMI 池/插槽互搶失真）。調完鎖定權重再驗分布精確。

**執行步驟（每個 ID）**：
1. 建 conf → `dmsetup create`
2. `dmsetup message <name> 0 reset_io_stats`
3. fio write 恰好 8G（libaio）
4. `dmsetup status`：比對每碟 wr bytes / 8G == weight / Σweight（±0，精確）
5. fio read + verify：完整性 0 mismatch
6. `dmsetup table`：確認 size = Σ 碟 carve
7. 記錄結果到下方「執行結果」表，`dmsetup remove`

---

## 次要測試

### WC 專項：小寫入合併效益

`fio --bs=4K --size=1G --iodepth=1`（單 depth，避免平行蓋掉 WC 效果）

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
| M1 | Mirror write + 拔碟 | 1. 建 A+B（mirror→C）<br>2. 寫 256M<br>3. offline A<br>4. 讀 256M | 讀取成功（從 mirror 回補） |
| M2 | Mirror rebuild | M1 後加回 A，觸發 rebuild | mirror_err=0、資料完整 |
| M3 | Cross-disk mirror | 4K iodepth=1 寫入，跨 disk boundary 也正確 mirror | mirror_err=0、ratio 正確 |

### 功能驗證

| ID | 測試 | 步驟 | 預期 |
|----|------|------|------|
| F1 | Virtual device reject | `--disks /dev/loop0` | 拒絕，提示 virtual device |
| F2 | Lifecycle | create → bench → show_stats → remove → recreate | 全部成功 |
| F3 | DM message | show_mirror / show_stats / show_log / set_policy / reset_stats | 正確輸出 |
| F4 | Data integrity | `fio --verify=crc32c --do_verify=1 --bs=4K --size=2G` | 0 mismatch |
| F5 | Badmap | `dmsetup message` set_badmap → 讀該 chunk | 讀作 0，不 I/O error |

---

## 執行結果

> 執行後回填。

| ID | 碟數 | 權重 | Write(MB/s) | Read(MB/s) | 分布誤差 | 完整性 |
|----|------|------|-------------|------------|----------|--------|
| S1 | 1 |  |  |  | 0% |  |
| S2 | 2 |  |  |  | 0% |  |
| S3 | 3 |  |  |  | 0% |  |
| S4 | 4 |  |  |  | 0% |  |

---

## 執行彙總

| 測試 | 預估時間 |
|------|----------|
| S1–S4 疊碟 | ~20 min |
| WC 專項 | ~5 min |
| Mirror 專項 | ~5 min |
| 功能驗證 | ~5 min |

建議一輪跑完 S1→S4；鏡像/功能測試視需要另開 session。
