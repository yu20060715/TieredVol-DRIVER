# 8/14 晚補測：USB 計畫檔 A–E（B@x4 + D 回池拓撲）

來源：計畫檔（USB `E862-F783/新增 文字文件.txt`），本次為逐項執行記錄與復現指引。
拓撲：A=SN750（8.0 GT/s x4、CPU 直連）、B=P3 Plus（5.0 GT/s **x4**、PCH）、C=MX500（SATA）、D=WD Blue（SATA，回池）、E=BX100 開機碟。
所有 config 用 `/dev/disk/by-id/`。測試卷 config 收在 `configs/test/`。

## 量測協定

- 排水態：先寫入 ≥64G（丟棄）等 SLC 回充後再量，或寫入前空檔 60s。
- 完整協定細節見 `docs/RESULTS.md`「8/14 晚補測」節。
- `dmsetup message` 的 result buffer 不印出 → 一律用 `/sys/kernel/tieredvol/status` 讀計數/錯誤。
  **限制**：sysfs 只反映 `tv_active_ctx`（最後 ctr 的卷）；多卷共存時 per-disk 計數用 `/proc/diskstats` 的 write-sector 增量驗證。
- **fio 陷阱**：`/dev/mapper/<名>` 不存在時 fio 會建**普通檔**（假讀 5.5 GB/s、假 ENOSPC）。裝置存在（`dmsetup ls`）且 chmod 666 後才能信賴 fio。
- **命令陷阱**：多行 table 的 `dmsetup create` 不能用 `echo pw | sudo -S` 前綴（stdin 被吃），須先 `echo pw | sudo -S -v` 快取後直接 `sudo`；`dd` 從 pipe 讀只傳 64 KB（pipe buffer），用檔輸入。

## A1 — 主表：S4 [64,27,9:4] 排水態×2（PASS）

config `configs/test/m_s4b.conf`（無 borrow 則用 `tv_s4.conf` 同權重）。

| 輪 | Write | Read | 分布（16G 寫） |
|----|-------|------|----------------|
| 1 | 2530 | 4290 | A=10104M B=4239M C=1413M D=628M |
| 2 | 2503 | 4293 | 同上（零誤差） |
| 中位 | **2517** | **4292** | 與確定性映射逐 chunk 吻合 |

排水態下 B+C+D 寫份額 ≈970 MB/s < DMI 1300 → A 為瓶頸、DMI 未飽和。

## A2 — Mirror 讀（PASS）

`configs/test/m_mir2.conf`（A:B=4:3、mirror→C）讀 16G = **3799 MB/s**；分布 A:B=74978:56247≈4:3 精確、C=0。

## A3 — vs LVM @ B@x4（方向不變）

| 方案 | Write | Read |
|------|-------|------|
| LVM 4 碟 1M stripe 20G | 1616 | 1791 |
| TV S4 fresh | 3091 | 3575（排水態 4292） |
| 倍率 | **1.9x** | **2.0–2.4x** |

## B4 — weight-borrowing 效益（結論：64:27:9:4 下中性）

config `configs/test/m_s4b.conf`（`borrow_area_mb=2048,0,0,0` → 2G 於 A 尾部、`borrow_watermark_kb=256`）。交替順序（off/on/off）控制 SLC 漂移。

| 情境 | off | on | off |
|------|-----|----|----|
| 無背景 | 2348 | 2197 | 2154 |
| D 飽和（背景 1M/d32 fio → ~112 MiB/s） | 1851 / 1865（均值 1858） | 1949 / 1742（均值 1846） | — |

**結論**：on≈off（雜訊內）。D 份額僅 3.85%（~96 MB/s ≪ solo 229），**D 天然非瓶頸、無功可借**。與 8/13 的 +3%（6:1:1:1、D 真瓶頸）互補：借調只在「慢碟真為瓶頸」的拓撲有正效益。
**設計限制**：borrow 表大小 = 20G carve/128KB = 163840 entries × 16B ≈ 2.6 MB（檔頭 16B：magic `TVBR`、version 2、n_blocks + entries 陣列）；100G carve（819200 entries ≈ 13 MB）超 kmalloc 上限 → ctr ENOMEM。現役全用 20G carve。

## C5 — Mirror 讀重試（fail injection，PASS）

前置：建合成裝置 `dAerr` = A[0,20G) 中於 A[4G,4G+1M) 掛 1M dm-error 窗（41943040 sectors）：

```
0 8388608 linear  /dev/disk/by-id/nvme-WDS500G3X0C-00SJG0_200705800588 0
8388608 2048 error
8390656 33552384 linear /dev/disk/by-id/nvme-WDS500G3X0C-00SJG0_200705800588 8390656
```

`configs/test/m_mir2.conf`（disk0=dAerr、disk1=B、mirror=C）。在 C 的 mirror 區
（`mirror_sec = (logical − seg0 logical_begin) >> 9`，此例 14680064 = 7G）埋 0xD5 → 讀 volume [7168M,7169M)（對應 A[4G,4G+1M)）：
- A 讀回 err（sysfs `err=1`）→ 自動 fallback mirror → **成功**，讀回 **byte 精確 0xD5**（資料必來自 C）。
- 驗證用 `dd if=/dev/mapper/m_mir2 iflag=direct skip=... count=... | cmp - d5.bin`。
- **統計小洞**：mirror retry 讀不計入 per-disk `total_read_ops`（main path only）。

## C6 — write 錯誤 → 自動 badmap + zero-fill（PASS）

`configs/test/m_bad.conf`（單碟 dAerr）。16G 寫 → 在窗處 EIO（badmap chunk 4096 被標記）：
- 讀 bad chunk → **全 0**（zero-fill）。
- 重寫 bad chunk → **成功**（badmap skip、資料保持 0、無錯誤）。
- sysfs `err=1`（dAerr 的窗）。

## D7 — WC read-after-write（PASS）

`configs/test/m_wc.conf`（A:B=64:36、chunk 1M、stripe 100M）。WC 只緩衝 **多碟 + ≥chunk_size** 的寫。
- 4M 0xD5 寫入 → 立即讀回全 0xD5；[4M,8M) 覆寫 0x3C → 讀回全 0x3C；[0,4M) 仍 0xD5。
- ramp 全 stripe 一致；fio 6G `--verify=pattern --do_verify=1` **0 mismatch**；persistence 重讀 6G 0 mismatch。

## D8 — 確定性重現（PASS）

`configs/test/m_d.conf`（A:B:C=4:3:2、stripe 9M）。
- vol1 寫 ramp（0x00–0xFF/1M）至 [0,18M)；`dmsetup remove`+重 create → 讀回 **byte 精確**。
- vol2 覆寫 0x3C → remove+重 create → 讀回全 0x3C。
- 同權重建雙向逐 byte 相同 → 映射純函數（無狀態/無隨機）。

## E9 — sparse + lockdep（抓到 1 個真 bug，已修）

```
make -C /lib/modules/$(uname -r)/build M=<repo>/driver C=2 CHECK="sparse"
```

**Bug**：`driver/tieredvol_borrow.c` `tv_borrow_redirect` 在 `borrow_off` 後（entries 保留、enabled=false）
遇未借區小寫入 `need>0` 分支**帶 spinlock（irqs off）直接 `return false`** → 下一個碰 `borrow.lock` 的 I/O 死鎖。
可達路徑：`msg_borrow_off` → <1M 小寫入（WC 不緩衝）。B4 全用 1M 寫（被 WC 緩衝）而未觸發。
**修復**：該分支加 `spin_unlock_irqrestore(&ctx->borrow.lock, flags);`。
**驗證**：`configs/test/m_fix.conf`（同 m_s4b 的 borrow 卷）→ `msg_borrow_off` → 512K 寫入 offset 0/4/8（WC bypass）→ 不再掛、讀回正確；`borrow_on` 正常。
**lockdep**：本機 kernel 6.14.0-27-generic `CONFIG_PROVE_LOCKING` 未開 → runtime lockdep 不可用；以 sparse lock-context 檢查替代（即此 bug 來源）。

## 當機調查（附帶）

boot -2 於 08:41:07 `pkill -9 -f busyd`（SIGKILL 兩個對 /dev/sdb offset 110G 的 1M/d32 libaio fio）後 2 秒內硬當；boot -1 殘留態 35s 再當。
無 panic/oops（硬當）、SMART 全乾淨 → **AHCI/SATA 控制器被深佇列 direct I/O 的 SIGKILL wedged**（B85 老晶片組），非 tieredvol driver（當下閒置）。
**防範**：不 `pkill -9`，一律固定 `--runtime` 讓 fio 自然結束。
