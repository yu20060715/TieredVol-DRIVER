# TieredVol — Windows 報告 Agent（v2）

> 在 Windows 的 tieredvol-thesis 目錄啟動 opencode，把 prompt 貼進去
> 前提：B85 已跑完 benchmark，BENCHMARK-RESULTS.md 已在 TieredVol 目錄

---

## 重要：2-disk outlier 處理

2-disk 的 Run3 數據異常高（write 2139、read 1421），是 SLC cache burst 造成的 outlier。
**論文裡不要用 mean，改用 median（中位數）。**
5 筆排序後取第 3 筆，天然忽略極端值。

| 場景 | Sorted | Median |
|------|--------|--------|
| 2-disk write | 1477.8, 1484.3, 1485.7, 1490.9, 2139.0* | **1485.7** |
| 2-disk read | 993.6, 1071.1, 1181.4, 1216.1, 1421.9* | **1181.4** |

---

## Prompt — 完整貼入 opencode

```
幫我完成以下 4 項任務。所有 benchmark 數據從 C:\Users\yu\Desktop\TieredVol\BENCHMARK-RESULTS.md 讀取。

=== #6：Pandoc HTML Build 測試 ===

1. 確認 Pandoc 是否安裝：pandoc --version
2. 如果沒安裝，跳過此項並報告 "Pandoc not installed"
3. 如果有安裝，執行：
   pandoc thesis.md -o test_output.html --toc --toc-depth 2 --standalone
4. 檢查 test_output.html：
   - 是否只有一份 TOC（不該有兩份）
   - Chapter 5 是否有 §5.5 Generality 和 §5.7 Lessons Learned
   - Appendix C 和 D 是否在 TOC 裡
   - 有無 broken links
5. 刪除 test_output.html，回報結果

=== #3：SPA Content 同步 ===

1. diff 比對以下 6 組檔案：
   - chapters/05-discussion.md vs spa/src/content/en/chapters/05-discussion.md
   - zh/chapters/05-discussion.md vs spa/src/content/cn/chapters/05-discussion.md
   - appendices/C-glossary.md vs spa/src/content/en/appendices/C-glossary.md
   - appendices/D-references.md vs spa/src/content/en/appendices/D-references.md
   - zh/appendices/C-glossary.md vs spa/src/content/cn/appendices/C-glossary.md
   - zh/appendices/D-references.md vs spa/src/content/cn/appendices/D-references.md
2. 有差異的用 chapters/ 或 zh/ 的最新版覆蓋到 spa/src/content/
3. 回報哪些已更新

=== #7：Quantization Error Sensitivity Analysis ===

1. 先讀取 chapters/05-discussion.md 的 §5.1 Quantization Error 段落

2. 在 §5.1 末尾（§5.2 之前）新增：

   ### Chunk Size Sensitivity

   | Chunk Size | Stripe Size (2-disk) | Error (2-disk) | Stripe Size (3-disk) | Error (3-disk) |
   |-----------|---------------------|----------------|---------------------|----------------|
   | 256 KB | ... | ... | ... | ... |
   | 512 KB | ... | ... | ... | ... |
   | 1 MB | ... | ... | ... | ... |
   | 2 MB | ... | ... | ... | ... |

   計算規則：
   - 從 BENCHMARK-RESULTS.md 的 Integration Test 段落找碟的單碟速度（如果有）
   - 如果沒有單碟速度，用 write throughput 反推：
     2-disk weights=[2,1] → true_ratio = NVMe/SATA ≈ 1000/450 ≈ 2.22
     3-disk weights=[2,1,1] → true_ratio 同上
   - assigned_weight = round(true_ratio)
   - error = |true_ratio - assigned_weight| / true_ratio
   - Stripe size = chunk_size × sum(weights)
   - **用實際計算值填入，不要用 placeholder**

   表格後加：
   "Quantization error is bounded by integer weight assignment and remains
   constant across chunk sizes. A 1 MB chunk balances I/O granularity with
   memory overhead (TV_BUF_COUNT × stripe_size)."

3. 同步更新 thesis_zh.md 的 §5.1 對應段落（中文翻譯）

=== #8：Benchmark 數據更新到論文 ===

讀取 TieredVol/BENCHMARK-RESULTS.md 的數據，更新以下內容：

1. chapters/04-experiments.md — 更新 §4.3 和 §4.4 的表格：
   - 用 **median** 而非 mean（2-disk 有 SLC burst outlier）
   - 3-disk 數據用原始 mean（無 outlier）

   數據來源：

   2-disk（用 median，排除 Run3 outlier）：
   - Write: 1485.7 MB/s（sorted: 1477.8, 1484.3, 1485.7, 1490.9, 2139.0*）
   - Read: 1181.4 MB/s（sorted: 993.6, 1071.1, 1181.4, 1216.1, 1421.9*）

   3-disk（用 mean）：
   - Write: 1788.8 ± 73.6 MB/s
   - Read: 1280.9 ± 50.9 MB/s

   3-disk large transfer：
   - Write 5GB: 1285.6, 10GB: 1228.6
   - Read 5GB: 1240.5, 10GB: 1318.0

   LVM control（sdb+sdc, stripe=1M）：
   - Write: 398.3 MB/s, Read: 432.0 MB/s

2. thesis.md 和 thesis_zh.md — 同步更新 Chapter 4 的數據

3. 注意事項：
   - 不要改動 headline 數字（1467, 1861 等）如果它們跟之前的 commit 一致
   - 如果新數據跟舊數據不同，在表格旁加腳註說明 "measured on B85 platform with PCIe 2.0x4 NVMe"
   - 3-disk 10GB read (1318.0) 比 5GB read (1240.5) 還高，這是正常的（NVMe steady-state），不需額外解釋

=== 完成後 ===

回報：
#6 Pandoc: PASS/FAIL
#3 SPA: X 個檔案已更新
#7 Quantization: 表格已加入
#8 Benchmark: 數據已更新到 Chapter 4
```

---

## 注意事項

- #7 和 #8 的數值必須從 BENCHMARK-RESULTS.md 讀取，不要猜
- 2-disk 數據有 outlier，一律用 median
- 3-disk 數據正常，用 mean ± stddev
- 所有改動完成後在 tieredvol-thesis 目錄 git commit + push
