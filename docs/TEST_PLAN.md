# Test Plan — TieredVol-DRIVER

## 硬碟對照

```
代號    型號            容量   介面              預估速度
A    = SN750           512G   PCIe 3.0 x4       3000 MB/s
B    = P3 Plus         1T     PCIe 2.0 x4       1000 MB/s
C    = MX500           512G   SATA               500 MB/s
D    = WD Blue         250G   SATA               250 MB/s
```

---

## Phase 1 — Raw 基準

每顆碟 `fio --ioengine=io_uring --direct=1 --bs=1M --size=1G --iodepth=128`。

### 排程

| ID | 碟 | RW | 預期速度 |
|----|----|----|----------|
| R1 | A  | write | ~3000 MB/s |
| R2 | A  | read  | ~3000 MB/s |
| R3 | B  | write | ~1000 MB/s |
| R4 | B  | read  | ~1000 MB/s |
| R5 | C  | write | ~500 MB/s |
| R6 | C  | read  | ~500 MB/s |
| R7 | D  | write | ~250 MB/s |
| R8 | D  | read  | ~250 MB/s |

**驗收標準**：每碟速度穩定，跟預估差距 < 20%。

---

## Phase 2 — TieredVol 功能測試

通用 fio 參數：`--ioengine=io_uring --direct=1 --bs=1M --size=1G --iodepth=128`。

每組測試記錄：
- Write throughput（MB/s）
- Read throughput（MB/s）
- `dmsetup status` per-disk write bytes vs weight 比例
- `show_mirror`（若有開 mirror）確認 mirror_err=0

### 2-disk：A + B（weight ≈ 3:1）

| ID | WC | Mirror | NVMe | SATA | 備註 |
|----|----|--------|------|------|------|
| T1 | on | off | A+B | - | 主場景 |
| T2 | off | off | A+B | - | WC 效能影響 |
| T3 | on | on → C | A+B | C(mir) | Mirror 驗證 |
| T4 | off | on → C | A+B | C(mir) | Mirror 無 WC |

### WC 專項：小寫入合併效益

`fio --bs=4K --size=1G --iodepth=1`（單 depth 避免平行蓋掉 WC 效果）

| ID | WC | 描述 |
|----|----|------|
| W1 | on | 4K 隨機寫入，WC 應合併成 1MB chunk |
| W2 | off | 同上，關 WC，每筆 4K 直接送 disk |
| W3 | on | 4K 循序寫入 |
| W4 | off | 4K 循序寫入，關 WC |

**驗收標準**：W1/W3 的 IOPS 應顯著高於 W2/W4（WC 減少 I/O 次數）。

### 3-disk：A + B + C（weight ≈ 6:2:1）

| ID | WC | Policy | Mirror | 備註 |
|----|----|--------|--------|------|
| T5 | on | static | off | 主場景 |
| T6 | off | static | off | WC 影響 |
| T7 | on | static | on → D | Mirror 驗證 |
| T8 | off | static | on → D | Mirror 無 WC |
| T9 | on | adaptive | off | vs T5 |
| T10 | off | adaptive | off | vs T6 |

### 4-disk：A + B + C + D（weight ≈ 12:4:2:1）

| ID | WC | Policy | Mirror | 備註 |
|----|----|--------|--------|------|
| T11 | on | static | off | 最大吞吐量 |
| T12 | off | static | off | WC 影響 |
| T13 | on | adaptive | off | vs T11 |

---

## Phase 3 — LVM 對照組

使用 `--lvm` 建立 LVM striped volume（stripesize 自動判斷）。fio 參數同 Phase 2。

| ID | Disk 組合 | 對照對象 |
|----|-----------|----------|
| L1 | A+B | T1 |
| L2 | A+B+C | T5 |
| L3 | A+B+C+D | T11 |
| L4 | A+C | 慢碟拖快碟最糟案例 |
| L5 | B+C+D | 混合 NVMe + SATA |

**驗收標準**：TieredVol throughput > LVM throughput（快碟不應被慢碟拖垮）。

---

## Phase 4 — Mirror 專項測試

| ID | 測試 | 步驟 | 預期 |
|----|------|------|------|
| M1 | Mirror write + 拔碟 | 1. 建立 A+B(mirror→D)<br>2. 寫入 256M<br>3. 拔 A（或 offline）<br>4. 讀取 256M | 讀取成功（從 D 回補） |
| M2 | Mirror rebuild | 1. M1 完成後<br>2. 加回 A<br>3. 觸發 rebuild | mirror_err=0<br>資料完整 |
| M3 | Cross-disk mirror | 4K iodepth=1 寫入，確認跨 disk boundary 的 bio 也正確 mirror | mirror_err=0<br>per-disk ratio 正確 |

---

## Phase 5 — 功能驗證

| ID | 測試 | 步驟 | 預期 |
|----|------|------|------|
| F1 | Carve `:GB` | `--disks A:100,B:50` → status 確認容量 | Size 正確 |
| F2 | Virtual device reject | `--disks /dev/loop0` | 拒絕，提示 virtual device |
| F3 | Lifecycle | create → bench → message show_stats → remove → recreate | 全部成功 |
| F4 | DM message | show_mirror / show_stats / show_config / set_policy / reset_stats | 正確輸出 |
| F5 | WC runtime toggle | `echo N > /sys/module/tieredvol/parameters/wc_enabled` → rerun | WC 生效/關閉 |
| F6 | Data integrity | `fio --verify=md5 --verify_interval=4K --bs=4K --size=10G --rw=write` → `--rw=read` | verify 0 mismatch，資料正確 |

---

## 執行彙總

| Phase | Runs | 估計時間 |
|-------|------|----------|
| Phase 1 Raw | 8 | 5 min |
| Phase 2 TieredVol | ~28 (×2 for write+read) | 22 min |
| Phase 3 LVM 對照 | ~10 (×2 for write+read) | 10 min |
| Phase 4 Mirror | 3 | 5 min |
| Phase 5 功能 | 6 | 10 min |
| **總計** | **~51 runs** | **~50 min** |

建議分兩輪執行：

- **Round 1**：Phase 1 + Phase 2（2-disk, 3-disk）+ Phase 3（L1, L2）
- **Round 2**：Phase 2（4-disk）+ Phase 3（L3-L5）+ Phase 4 + Phase 5
