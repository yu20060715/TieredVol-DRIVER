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

## 三種 Policy

| Policy | 說明 | 演算法 |
|--------|------|--------|
| **static**（預設） | 依 config weight 固定分配 | 上述四步驟 |
| **adaptive** | 即時依負載/延遲/磨損動態分配 | EMA load + latency + wear 評分 |
| **random** | 隨機分配（測試用） | get_random_u32() % disk_count |

### Adaptive 評分

每顆 disk 維護三個指標：
- **EMA load**：`in_flight_bytes` 的指數移動平均
- **EMA latency**：最近 I/O 完成時間的 EMA
- **Wear**：累積寫入量 × wear_bias

評分 = load + latency + wear，選分數最低的 disk。

Hung-disk 偵測（STALE）：僅當某 disk 有 in-flight I/O 卻連續 `stale_ms` 零完成（卡死）時標記 STALE，adaptive 路由會避開該 disk；排空或恢復完成立即 RECOVERED。閒置 disk 永不標 STALE。

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
