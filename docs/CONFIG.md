# Config — 設定檔參考

## 格式

INI 格式，儲存在 `/etc/tieredvol/<name>.conf`。

## 完整範例

```ini
[weighted_striping]
version=1
chunk_size=1048576
segment_count=2
disk_count=4
disk0_name=/dev/nvme0n1
disk1_name=/dev/sdb
disk2_name=/dev/sdc
disk3_name=/dev/sdd
seg0_begin=0
seg0_end=1073741824
seg0_count=4
seg0_disks=0,1,2,3
seg0_weight=6,3,2,1
seg0_stripe=12582912
seg1_begin=1073741824
seg1_end=2147483648
seg1_count=2
seg1_disks=0,1
seg1_weight=3,1
seg1_stripe=4194304
```

## 參數說明

### 全域

| 參數 | 型態 | 必填 | 說明 |
|------|------|------|------|
| `version` | int | 是 | 格式版本（目前 = 1） |
| `chunk_size` | int | 是 | chunk 大小（bytes），建議 1048576（1MB） |
| `segment_count` | int | 是 | segment 數量（上限 16） |
| `disk_count` | int | 是 | disk 數量（上限 16） |
| `disk{X}_name` | string | 是 | 第 X 顆 disk 的裝置路徑 |

### Per-Segment

| 參數 | 型態 | 必填 | 說明 |
|------|------|------|------|
| `seg{X}_begin` | uint64 | 是 | segment 起始 logical byte |
| `seg{X}_end` | uint64 | 是 | segment 結束 logical byte |
| `seg{X}_count` | int | 是 | 此 segment 使用的 disk 數量 |
| `seg{X}_disks` | csv | 是 | disk index 列表（逗號分隔） |
| `seg{X}_weight` | csv | 是 | weight 列表（逗號分隔，與 disks 對應） |
| `seg{X}_stripe` | int | 是 | stripe 大小（bytes） |
| `seg{X}_mirror` | int | 否 | mirror disk index（不指定則無 mirror） |
| `seg{X}_policy` | string | 否 | dispatch policy（static / random） |

## 參數計算

### stripe_size

```
stripe_size = Σ weight[i] × chunk_size
```

chunk_size 建議與 filesystem block size 或常用 I/O size 一致（1MB = 1048576）。

### weight

從 benchmark 速度計算：`weight = speed / slowest_speed`。

不指定則 `tiered_setup --create` 會自動測速計算。

> **權重推導方式**：以**平行仲裁**（所有碟同時打 raw，取實際並行吞吐）為準，
> 不要用單碟 solo 測速（solo 會因 DMI 池 / PCIe 插槽互搶而失真）。
> 現有 `/home/yu/tv3x.conf`、`tv4x.conf` 是舊拓撲（nvme0=WD 快、nvme1=P3 慢）調的，
> **不適用於目前拓撲**（nvme0=P3 @ PCIe2.0 x1、nvme1=WD @ PCIe3.0 x4），需重新調校。

### segment 範圍

當 disk 容量不同時，依容量遞增排序，分段建立 segment：

```
Disk 0: 4TB → 參與所有 segment
Disk 1: 2TB → 參與前兩個 segment
Disk 2: 1TB → 只參與第一個 segment

Segment 0: 0 ~ 1TB     → disks [0,1,2]
Segment 1: 1TB ~ 2TB   → disks [0,1]
Segment 2: 2TB ~ 4TB   → disks [0]
```

## Carve 語法

建立時用 `碟名:GB` 指定 carve 容量：

```bash
sudo tiered_setup --create --name pool --disks nvme0n1:100,sdb:50
```

- DM path（預設）和 LVM path 都支援
- 不指定 `:GB` 時使用整碟（扣 1GB）
- dm-linear 從 sector 0 開始 carve，剩餘空間不能再次使用

## Borrow 運行期設定（`[runtime]` 節，可選）

weight-borrowing 透過 `.conf` 的 `[runtime]` 節或 `dmsetup message` 覆寫：

```ini
[runtime]
borrow_enable=1          # 1/0：啟用/停用（預設 0）
borrow_watermark_kb=256  # 觸發借出的 src in-flight 門檻（KB）
borrow_area_mb=2048,0,0,0 # 每碟借用區大小（MB，按 disk index 順序）
```

- borrow block = `chunk_size / 8`（NVMe `max_sectors_per_request=256` 下為 128KB）
- 借用區位在每碟 static 條紋區之後，需在 carve 容量內保留
- 覆寫以 `dmsetup message` 為準，remove 時連同 `.borrow` 表一起持久化

## DM Message 命令

| 命令 | 參數 | 說明 |
|------|------|------|
| `status` | - | 顯示 volume 狀態與每碟 I/O bytes（驗收用） |
| `show_stats` | - | 顯示 I/O stats |
| `show_io_stats` | - | 顯示每碟 I/O bytes |
| `show_inflight` | - | 顯示 inflight bio 統計 |
| `show_mirror` | - | 顯示 mirror stats |
| `show_degraded` | - | 顯示 degraded 狀態 |
| `show_rebuild` | - | 顯示 rebuild 進度 |
| `show_badmap` | - | 顯示 per-disk badmap chunk 統計 |
| `show_errors` | - | 顯示錯誤計數 |
| `show_bench` | - | 顯示內建 benchmark 結果 |
| `show_log` | - | 顯示 log buffer |
| `borrow_on` / `borrow_off` | - | 啟用 / 停用 weight-borrowing（停用後已借 block 仍解析） |
| `show_borrow` | - | 顯示 borrow 狀態（enabled、借用區、n_borrowed） |
| `set_policy` | static/random | 切換全域 dispatch policy |
| `set_seg_policy` | static/random | 切換單一 segment policy |
| `set_mirror` | disk_index | 設定 mirror disk（不可為參與碟） |
| `start_rebuild` / `stop_rebuild` | - | 手動觸發 / 停止 rebuild |
| `set_badmap` | disk:chunk | 將 chunk 標記為 bad |
| `set_loglevel` | int | 設定 log 等級 |
| `reset_stats` / `reset_io_stats` | - | 重置 I/O stats |
| `reset_errors` | - | 重置錯誤計數 |

> `show_wear` / `reset_wear` / `show_adaptive` 已移除（adaptive/wear 系統刪除）。

```bash
sudo dmsetup message <name> 0 show_stats
sudo dmsetup message <name> 0 show_borrow
sudo dmsetup message <name> 0 borrow_on
```

## 注意

- Segment 必須依 `begin` 遞增排序，不可重疊
- 所有 disk index 必須小於 `disk_count`
- mirror disk 不可與 segment 的任一 primary disk 相同；若要 mirror，
  需有**不參與該 segment** 的第 N+1 顆碟（例如 `disk_count=5`、`seg0_disks=0,1,2,3`、`seg0_mirror=4`）
- Weight 上限 16，最少 1
