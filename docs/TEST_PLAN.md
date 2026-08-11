# Test Plan — TieredVol-DRIVER

> 主目標：**證明「疊加硬碟」可行**（1→2→3→4 disk，分布精確、資料完整、容量擴展）。
> 速度為參考指標，非驗收條件。
> 實測結果已紀錄於 [RESULTS.md](RESULTS.md)（2026-08-11，現況拓撲）。

---

## 硬碟代號（目前拓撲）

```
A = nvme1n1  WD SN550 (PCIe 3.0 x4, 快)    B = nvme0n1  P3 Plus (PCIe 2.0 x1, 慢)
C = sdc      MX500 (SATA 6G)               D = sdb      WD Blue (SATA 6G)
E = sda      BX100（未入 config）
```

完整型號/容量/link 速度/平行仲裁測速見 [RESULTS.md](RESULTS.md)「硬碟對照」。

> **插槽角色已交換**：現況 A 在 x4 = 快碟、B 在 x1 = 慢碟。早先實驗（tv3x/tv4x）是舊拓撲
> （nvme0=WD 快、nvme1=P3 慢）調的權重，**已不適用**；現況權重以平行仲裁重新調校（8:1:2:1）。

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
（各碟同時打 raw、取實際並行吞吐，非 solo 測速）調校，現況為 **8:1:2:1**。

| ID | 碟數 | 碟組合 | 權重 | 驗證 | 狀態 |
|----|------|--------|------|------|------|
| S1 | 1 | A | [1] | 單碟基線、計數 100% | ✅ 完成 |
| S2 | 2 | A+B | 8:1 | 分布精確 | ✅ 完成 |
| S3 | 3 | A+B+C | 8:1:2 | 分布精確 | ✅ 完成 |
| S4 | 4 | A+B+C+D | 8:1:2:1 | 分布精確 + 完整性 | ✅ 完成 |
| CAP | 4 | 100G carve | 8:1:2:1 | 容量擴展 + 分布精確 | ✅ 完成 |

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

## 次要測試（**待辦**）

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

## 執行狀態

| 測試 | 狀態 | 結果位置 |
|------|------|----------|
| S1–S4 疊碟 + CAP | **完成**（2026-08-11） | `docs/RESULTS.md` |
| WC 專項 W1–W4 | 待辦 | - |
| Mirror 專項 M1–M3 | 待辦 | - |
| 功能驗證 F1–F5 | 待辦 | - |
