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

## 結論

1. **疊碟可擴展**：寫入吞吐隨碟數上升 2083 → 2587 → 2788 → 2972 MB/s（疊碟後超過單碟基線）。
2. **分布精確**：計數器與宣告權重、確定性映射預測完全一致（0 誤差）。
3. **資料完整**：crc32c 寫入 + verify 0 mismatch，`err=0`。

使用 conf：`/home/yu/tv_s1.conf`、`tv_s2.conf`、`tv_s3.conf`、`tv_s4.conf`
（disk0=nvme1n1, disk1=nvme0n1, disk2=sdc, disk3=sdb）。
