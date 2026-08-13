# Mapping — Logical→Physical 映射

## 核心問題

給定一個 logical byte offset，決定這筆資料要放在哪顆 disk 的哪個 sector。

```
Input:  logical (byte offset from volume start)
Output: disk_id, physical_offset (byte offset on disk)
```

## 資料結構

### Stripe

一次循環內，所有 disk 依照 weight 分配 chunk 的總和。

```
stripe_size = Σ weight[i] × chunk_size
```

### Segment

一段連續 logical space，disk 集合與 weight 固定。當 disk 容量不同時，用多個 segment 分段處理。

```
Segment 0: logical 0 ~ 1TB   → disks [0,1,2,3]  weight [6,3,2,1]
Segment 1: logical 1TB ~ 2TB  → disks [0,1,2]    weight [6,3,2]
Segment 2: logical 2TB ~ 4TB  → disks [0]        weight [1]
```

## 映射演算法

### Step 1：找 Segment

binary search，比對 `logical ∈ [seg->logical_begin, seg->logical_end)`。

### Step 2：計算 Stripe 編號與 Offset

```
stripe_no  = (logical - seg->logical_begin) / seg->stripe_size
stripe_off = (logical - seg->logical_begin) % seg->stripe_size
```

### Step 3：找 Disk

stripe 內每顆 disk 的邊界 = prefix sum of `weight[i] × chunk_size`。

```
boundary[0] = 0
boundary[1] = weight[0] × chunk_size
boundary[2] = boundary[1] + weight[1] × chunk_size
...
```

disk_idx = 滿足 `stripe_off ∈ [boundary[i], boundary[i+1])` 的 i。

### Step 4：計算 Physical Offset

```
physical_offset = stripe_no × weight[disk_idx] × chunk_size
                + (stripe_off - boundary[disk_idx])
```

sector = `physical_offset >> 9`。

## 兩種 Policy + Weight-borrowing

| Policy | 說明 | 演算法 |
|--------|------|--------|
| **static**（預設） | 依 config weight 固定分配 | 上述四步驟 |
| **random** | 隨機分配（測試用） | get_random_u32() % disk_count |

> **ADAPTIVE 已移除**（2026-08-13）：動態選碟會破壞位址確定性且實測失衡
> （見 docs/RESULTS.md 結論 #13）。改以 **weight-borrowing** 在 static 佈局上
> 做暫時 offload。

### Weight-borrowing（static 之上）

慢碟（src）in-flight ≥ `borrow_watermark_kb` 且寫入整 128KB block 對齊時，
該 block 重導至**最少負載**碟（排除 src / degraded / 借用區無空間）的
over-provisioned 借用區。per-block 表記錄 `(dst_disk, dst_sector)`，讀取與
重寫經 lookup 解析同一目的地；`borrow_off` 只停新借、已借 block 仍正確解析。
表在 remove 時存 `<config>.borrow`、重建時載入，跨 reload 一致（詳見
ARCHITECTURE.md「Borrow 觸發與選碟」）。

## Static Mapping 範例

```
volume: 5GB, disks: [nvme0n1(6), sdb(1)]
chunk_size = 1MB
stripe_size = 7MB

Logical 0:
  seg=0, stripe_no=0, stripe_off=0
  boundary[0]=0, boundary[1]=6MB
  0 ∈ [0, 6MB) → disk=0, offset=0 + 0 = 0

Logical 6MB:
  stripe_off=6MB
  6MB ∈ [6MB, 7MB) → disk=1, offset=0 + (6MB - 6MB) = 0

Logical 7MB:
  stripe_no=1, stripe_off=0
  → disk=0, offset=1×6MB + 0 = 6MB
```
