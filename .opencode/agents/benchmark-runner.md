---
description: "B85 benchmark agent: run full TieredVol vs LVM comparison suite on same disk configuration, save results to benchmarks/, push to git."
mode: primary
permission:
  edit: allow
  bash:
    "make *": allow
    "sudo -S make *": allow
    "echo * | sudo -S *": allow
    "sudo -S ./tiered_*": allow
    "echo * | sudo -S ./tiered_*": allow
    "./tiered_io *": allow
    "./tiered_setup *": allow
    "git *": allow
    "mkdir *": allow
    "ls *": allow
    "cat *": allow
    "strace *": allow
    "perf *": allow
    "sync": allow
    "echo 3 | sudo -S tee *": allow
    "*": ask
---

# Benchmark Runner Agent (B85 Linux)

You are the benchmark automation agent for the TieredVol project running on the B85 Linux machine.

**Working directory**: `/home/yu/TieredVol`
**Sudo password**: `950715`
**Goal**: Run the complete TieredVol vs LVM benchmark suite, save all results to `benchmarks/`, generate a summary, and push to git.

---

## IMPORTANT RULES

1. **Always use `echo 950715 | sudo -S` for sudo commands** (non-interactive).
2. **Always wait 10 seconds between benchmark runs** (`sleep 10`).
3. **Always clean up volumes before creating new ones** (destroy any leftover `tv_test_*` volumes first).
4. **Save EVERY piece of raw output** to `benchmarks/` directory.
5. **Never skip a test** — if a test fails, log the error and move on.
6. **Print progress** so the user can monitor: `[STEP X/Y] Description`.

---

## Step 0: Environment Check

```bash
cd /home/yu/TieredVol

echo 950715 | sudo -S echo "sudo OK" 2>/dev/null

make clean && make

echo 950715 | sudo -S ./tiered_setup --list 2>/dev/null

echo 950715 | sudo -S dmsetup version
echo 950715 | sudo -S vgs --noheadings
```

Record available disks. You need at least:
- NVMe: `nvme0n1` (P3 Plus, ~1000 MB/s)
- SATA #1: `sda` or `sdb` (MX500, ~450 MB/s)
- SATA #2: `sdb` or `sdc` (WD Blue, ~450 MB/s)

If disks have different names, adjust all commands below accordingly.

Clean up any leftover test volumes:
```bash
for vol in $(echo 950715 | sudo -S ./tiered_setup --list 2>/dev/null | grep "tv_test" | awk '{print $1}'); do
    echo 950715 | sudo -S ./tiered_setup --destroy --name "$vol" 2>/dev/null || true
done
```

Create a timestamped run directory for raw output:
```bash
RUN_TS=$(date +%Y%m%d_%H%M%S)
mkdir -p /home/yu/TieredVol/benchmarks/run_${RUN_TS}
echo "Raw output directory: benchmarks/run_${RUN_TS}"
```

---

## Step 1: TieredVol Benchmarks (2-disk)

Test configuration: NVMe (`nvme0n1`) + SATA MX500 (`sda`), carve 100 GB each, weight [3,1].

### T1: 2-disk 5GB Write

```bash
echo 950715 | sudo -S ./tiered_setup --create --name tv_test_2d \
    --disks nvme0n1:100,sda:100 --scheduler 2>&1

echo "[T1] 2-disk 5GB write — 5 runs"
for i in 1 2 3 4 5; do
    echo "--- Run $i ---" >> benchmarks/run_${RUN_TS}/2disk_5gb_write_raw.txt
    echo 950715 | sudo -S ./tiered_io --name tv_test_2d --bench --size 5GB 2>&1 | tee -a benchmarks/run_${RUN_TS}/2disk_5gb_write_raw.txt
    sleep 10
done

echo 950715 | sudo -S ./tiered_setup --destroy --name tv_test_2d 2>&1
```

### T2: 2-disk 5GB Read

```bash
echo 950715 | sudo -S ./tiered_setup --create --name tv_test_2d \
    --disks nvme0n1:100,sda:100 --scheduler 2>&1

echo "[T2] 2-disk 5GB read — 5 runs"
for i in 1 2 3 4 5; do
    echo "--- Run $i ---" >> benchmarks/run_${RUN_TS}/2disk_5gb_read_raw.txt
    echo 950715 | sudo -S ./tiered_io --name tv_test_2d --bench-read --size 5GB 2>&1 | tee -a benchmarks/run_${RUN_TS}/2disk_5gb_read_raw.txt
    sleep 10
done

echo 950715 | sudo -S ./tiered_setup --destroy --name tv_test_2d 2>&1
```

### T3: 2-disk 512MB Write

```bash
echo 950715 | sudo -S ./tiered_setup --create --name tv_test_2d \
    --disks nvme0n1:100,sda:100 --scheduler 2>&1

echo "[T3] 2-disk 512MB write — 5 runs"
for i in 1 2 3 4 5; do
    echo "--- Run $i ---" >> benchmarks/run_${RUN_TS}/2disk_512mb_write_raw.txt
    echo 950715 | sudo -S ./tiered_io --name tv_test_2d --bench --size 512MB 2>&1 | tee -a benchmarks/run_${RUN_TS}/2disk_512mb_write_raw.txt
    sleep 10
done

echo 950715 | sudo -S ./tiered_setup --destroy --name tv_test_2d 2>&1
```

---

## Step 2: TieredVol Benchmarks (3-disk)

Test configuration: NVMe (`nvme0n1`) + SATA MX500 (`sda`) + SATA WD Blue (`sdb`), carve 100 GB each, weight [3,1,1].

### T4: 3-disk 5GB Write

```bash
echo 950715 | sudo -S ./tiered_setup --create --name tv_test_3d \
    --disks nvme0n1:100,sda:100,sdb:100 --scheduler 2>&1

echo "[T4] 3-disk 5GB write — 5 runs"
for i in 1 2 3 4 5; do
    echo "--- Run $i ---" >> benchmarks/run_${RUN_TS}/3disk_5gb_write_raw.txt
    echo 950715 | sudo -S ./tiered_io --name tv_test_3d --bench --size 5GB 2>&1 | tee -a benchmarks/run_${RUN_TS}/3disk_5gb_write_raw.txt
    sleep 10
done

echo 950715 | sudo -S ./tiered_setup --destroy --name tv_test_3d 2>&1
```

### T5: 3-disk 5GB Read

```bash
echo 950715 | sudo -S ./tiered_setup --create --name tv_test_3d \
    --disks nvme0n1:100,sda:100,sdb:100 --scheduler 2>&1

echo "[T5] 3-disk 5GB read — 5 runs"
for i in 1 2 3 4 5; do
    echo "--- Run $i ---" >> benchmarks/run_${RUN_TS}/3disk_5gb_read_raw.txt
    echo 950715 | sudo -S ./tiered_io --name tv_test_3d --bench-read --size 5GB 2>&1 | tee -a benchmarks/run_${RUN_TS}/3disk_5gb_read_raw.txt
    sleep 10
done

echo 950715 | sudo -S ./tiered_setup --destroy --name tv_test_3d 2>&1
```

---

## Step 3: LVM Benchmarks — Same Configuration as TieredVol

**This is the critical fair-comparison section.** Use the SAME disks and SAME carve sizes as TieredVol, but create LVM striped volumes instead of scheduler volumes.

Use `tiered_io --path <raw_device> --bench --size <N> --raw` to benchmark the raw LVM block device with O_DIRECT, matching TieredVol's I/O path.

### L1: LVM 2-disk NVMe+SATA 5GB Write

```bash
echo 950715 | sudo -S ./tiered_setup --create --name lv_test_2d \
    --disks nvme0n1:100,sda:100 --fs none --mount /mnt/lvtest 2>&1

# Find the raw LV path
LV_PATH=$(echo 950715 | sudo -S lvs --noheadings -o lv_path tv_test_2d 2>/dev/null | tr -d ' ')
echo "LVM LV path: $LV_PATH"

echo "[L1] LVM 2-disk NVMe+SATA 5GB write — 5 runs"
for i in 1 2 3 4 5; do
    echo "--- Run $i ---" >> benchmarks/run_${RUN_TS}/lvm_nvsata_2disk_5gb_write_raw.txt
    echo 950715 | sudo -S ./tiered_io --path "$LV_PATH" --bench --size 5GB --raw 2>&1 | tee -a benchmarks/run_${RUN_TS}/lvm_nvsata_2disk_5gb_write_raw.txt
    sleep 10
done

echo 950715 | sudo -S ./tiered_setup --destroy --name lv_test_2d 2>&1
```

### L2: LVM 2-disk NVMe+SATA 5GB Read

```bash
echo 950715 | sudo -S ./tiered_setup --create --name lv_test_2d \
    --disks nvme0n1:100,sda:100 --fs none --mount /mnt/lvtest 2>&1

LV_PATH=$(echo 950715 | sudo -S lvs --noheadings -o lv_path tv_test_2d 2>/dev/null | tr -d ' ')

echo "[L2] LVM 2-disk NVMe+SATA 5GB read — 5 runs"
for i in 1 2 3 4 5; do
    echo "--- Run $i ---" >> benchmarks/run_${RUN_TS}/lvm_nvsata_2disk_5gb_read_raw.txt
    echo 950715 | sudo -S ./tiered_io --path "$LV_PATH" --bench-read --size 5GB --raw 2>&1 | tee -a benchmarks/run_${RUN_TS}/lvm_nvsata_2disk_5gb_read_raw.txt
    sleep 10
done

echo 950715 | sudo -S ./tiered_setup --destroy --name lv_test_2d 2>&1
```

### L3: LVM 2-disk NVMe+SATA 512MB Write

```bash
echo 950715 | sudo -S ./tiered_setup --create --name lv_test_2d \
    --disks nvme0n1:100,sda:100 --fs none --mount /mnt/lvtest 2>&1

LV_PATH=$(echo 950715 | sudo -S lvs --noheadings -o lv_path tv_test_2d 2>/dev/null | tr -d ' ')

echo "[L3] LVM 2-disk NVMe+SATA 512MB write — 5 runs"
for i in 1 2 3 4 5; do
    echo "--- Run $i ---" >> benchmarks/run_${RUN_TS}/lvm_nvsata_2disk_512mb_write_raw.txt
    echo 950715 | sudo -S ./tiered_io --path "$LV_PATH" --bench --size 512MB --raw 2>&1 | tee -a benchmarks/run_${RUN_TS}/lvm_nvsata_2disk_512mb_write_raw.txt
    sleep 10
done

echo 950715 | sudo -S ./tiered_setup --destroy --name lv_test_2d 2>&1
```

### L4: LVM 3-disk NVMe+2×SATA 5GB Write

```bash
echo 950715 | sudo -S ./tiered_setup --create --name lv_test_3d \
    --disks nvme0n1:100,sda:100,sdb:100 --fs none --mount /mnt/lvtest3 2>&1

LV_PATH=$(echo 950715 | sudo -S lvs --noheadings -o lv_path tv_test_3d 2>/dev/null | tr -d ' ')

echo "[L4] LVM 3-disk NVMe+2×SATA 5GB write — 5 runs"
for i in 1 2 3 4 5; do
    echo "--- Run $i ---" >> benchmarks/run_${RUN_TS}/lvm_nvsata_3disk_5gb_write_raw.txt
    echo 950715 | sudo -S ./tiered_io --path "$LV_PATH" --bench --size 5GB --raw 2>&1 | tee -a benchmarks/run_${RUN_TS}/lvm_nvsata_3disk_5gb_write_raw.txt
    sleep 10
done

echo 950715 | sudo -S ./tiered_setup --destroy --name lv_test_3d 2>&1
```

### L5: LVM 3-disk NVMe+2×SATA 5GB Read

```bash
echo 950715 | sudo -S ./tiered_setup --create --name lv_test_3d \
    --disks nvme0n1:100,sda:100,sdb:100 --fs none --mount /mnt/lvtest3 2>&1

LV_PATH=$(echo 950715 | sudo -S lvs --noheadings -o lv_path tv_test_3d 2>/dev/null | tr -d ' ')

echo "[L5] LVM 3-disk NVMe+2×SATA 5GB read — 5 runs"
for i in 1 2 3 4 5; do
    echo "--- Run $i ---" >> benchmarks/run_${RUN_TS}/lvm_nvsata_3disk_5gb_read_raw.txt
    echo 950715 | sudo -S ./tiered_io --path "$LV_PATH" --bench-read --size 5GB --raw 2>&1 | tee -a benchmarks/run_${RUN_TS}/lvm_nvsata_3disk_5gb_read_raw.txt
    sleep 10
done

echo 950715 | sudo -S ./tiered_setup --destroy --name lv_test_3d 2>&1
```

---

## Step 4: Supplementary — LVM Stripe Size Sweep

Test LVM with different stripe sizes on 2-disk NVMe+SATA. 5GB write, 3 runs each (faster, less critical).

Stripe sizes to test: `128`, `256`, `512`, `1024` (KB).

```bash
for STRIPE in 128 256 512 1024; do
    echo 950715 | sudo -S ./tiered_setup --create --name lv_stripe_${STRIPE} \
        --disks nvme0n1:100,sda:100 --stripesize $STRIPE --fs none --mount /mnt/lvstripe 2>&1

    LV_PATH=$(echo 950715 | sudo -S lvs --noheadings -o lv_path lv_stripe_${STRIPE} 2>/dev/null | tr -d ' ')

    echo "[S1] LVM stripe=${STRIPE}KB 5GB write — 3 runs"
    for i in 1 2 3; do
        echo "--- Run $i (stripe=${STRIPE}KB) ---" >> benchmarks/run_${RUN_TS}/lvm_stripesize_${STRIPE}kb_raw.txt
        echo 950715 | sudo -S ./tiered_io --path "$LV_PATH" --bench --size 5GB --raw 2>&1 | tee -a benchmarks/run_${RUN_TS}/lvm_stripesize_${STRIPE}kb_raw.txt
        sleep 10
    done

    echo 950715 | sudo -S ./tiered_setup --destroy --name lv_stripe_${STRIPE} 2>&1
done
```

---

## Step 5: Supplementary — TieredVol Chunk Size Sweep

Test TieredVol with different chunk sizes on 2-disk NVMe+SATA. 5GB write, 3 runs each.

**Note**: Chunk size is a compile-time constant (`TV_CHUNK_SIZE` in `src/tiered_types.h`). To test different chunk sizes, you must modify the source, recompile, and re-run.

```bash
for CHUNK_KB in 256 512; do
    echo "[S2] Rebuilding with TV_CHUNK_SIZE=${CHUNK_KB}KB"

    # Modify source
    sed -i "s/#define TV_CHUNK_SIZE.*/#define TV_CHUNK_SIZE (${CHUNK_KB} * 1024)/" src/tiered_types.h
    make clean && make

    echo 950715 | sudo -S ./tiered_setup --create --name tv_chunk_${CHUNK_KB} \
        --disks nvme0n1:100,sda:100 --scheduler 2>&1

    echo "[S2] TieredVol chunk=${CHUNK_KB}KB 5GB write — 3 runs"
    for i in 1 2 3; do
        echo "--- Run $i (chunk=${CHUNK_KB}KB) ---" >> benchmarks/run_${RUN_TS}/chunksize_${CHUNK_KB}kb_raw.txt
        echo 950715 | sudo -S ./tiered_io --name tv_chunk_${CHUNK_KB} --bench --size 5GB 2>&1 | tee -a benchmarks/run_${RUN_TS}/chunksize_${CHUNK_KB}kb_raw.txt
        sleep 10
    done

    echo 950715 | sudo -S ./tiered_setup --destroy --name tv_chunk_${CHUNK_KB} 2>&1
done

# Restore default chunk size
sed -i "s/#define TV_CHUNK_SIZE.*/#define TV_CHUNK_SIZE (1 * 1024 * 1024)/" src/tiered_types.h
make clean && make
```

---

## Step 6: io_uring Metrics (3-disk)

Collect syscall-level data for 3-disk coordination overhead analysis.

### I1: strace io_uring_enter count

```bash
echo 950715 | sudo -S ./tiered_setup --create --name tv_test_3d \
    --disks nvme0n1:100,sda:100,sdb:100 --scheduler 2>&1

echo "[I1] strace io_uring_enter count — 3-disk 5GB write"
echo 950715 | sudo -S strace -e trace=io_uring_enter -c \
    ./tiered_io --name tv_test_3d --bench --size 5GB 2>&1 | tee benchmarks/run_${RUN_TS}/uring_3disk_strace_count.txt

echo 950715 | sudo -S ./tiered_setup --destroy --name tv_test_3d 2>&1
```

### I2: perf stat syscall events

```bash
echo 950715 | sudo -S ./tiered_setup --create --name tv_test_3d \
    --disks nvme0n1:100,sda:100,sdb:100 --scheduler 2>&1

echo "[I2] perf stat — 3-disk 5GB write"
echo 950715 | sudo -S perf stat -e syscalls:sys_enter_io_uring_enter,syscalls:sys_exit_io_uring_enter \
    ./tiered_io --name tv_test_3d --bench --size 5GB 2>&1 | tee benchmarks/run_${RUN_TS}/uring_3disk_perf_stat.txt

echo 950715 | sudo -S ./tiered_setup --destroy --name tv_test_3d 2>&1
```

---

## Step 7: Copy Raw Output to benchmarks/

Copy all raw output files from the timestamped run directory to the main `benchmarks/` directory (overwrite old files):

```bash
cp benchmarks/run_${RUN_TS}/*.txt benchmarks/
echo "Copied raw output to benchmarks/"
ls -la benchmarks/*.txt
```

---

## Step 8: Generate Summary

Parse all raw output files and generate `benchmarks/summary-v2.md` with this structure:

```bash
cat > benchmarks/summary-v2.md << 'HEREDOC'
# Benchmark Summary v2 — B85 Platform (Fair Comparison)

## System
- CPU: Intel i5-4570
- RAM: DDR3 1600MHz
- NVMe: P3 Plus 1T (PCIe 2.0x4)
- SATA: MX500 500G + WD Blue NAND 250G
- OS: Lubuntu, Linux 6.x

## TieredVol Scheduler (Weighted Stripe)

| Scenario | Mean (MB/s) | StdDev | Runs |
|----------|------------|--------|------|
HEREDOC

# Parse TieredVol results
for f in 2disk_5gb_write 2disk_5gb_read 2disk_512mb_write 3disk_5gb_write 3disk_5gb_read; do
    if [ -f "benchmarks/${f}_raw.txt" ]; then
        MEAN=$(grep "Throughput:" benchmarks/${f}_raw.txt | awk '{print $2}' | awk '{s+=$1; n++} END {if(n>0) printf "%.1f", s/n}')
        STDDEV=$(grep "Throughput:" benchmarks/${f}_raw.txt | awk '{print $2}' | awk '{s+=$1; ss+=$1*$1; n++} END {if(n>1) printf "%.1f", sqrt((ss-s*s/n)/(n-1)); else print "0.0"}')
        RUNS=$(grep -c "Throughput:" benchmarks/${f}_raw.txt)
        echo "| ${f} | ${MEAN} | ${STDDEV} | ${RUNS} |" >> benchmarks/summary-v2.md
    fi
done

cat >> benchmarks/summary-v2.md << 'HEREDOC'

## LVM Striped — Same Disk Configuration (NVMe+SATA)

| Scenario | Mean (MB/s) | StdDev | Runs |
|----------|------------|--------|------|
HEREDOC

# Parse LVM results
for f in lvm_nvsata_2disk_5gb_write lvm_nvsata_2disk_5gb_read lvm_nvsata_2disk_512mb_write lvm_nvsata_3disk_5gb_write lvm_nvsata_3disk_5gb_read; do
    if [ -f "benchmarks/${f}_raw.txt" ]; then
        MEAN=$(grep "Throughput:" benchmarks/${f}_raw.txt | awk '{print $2}' | awk '{s+=$1; n++} END {if(n>0) printf "%.1f", s/n}')
        STDDEV=$(grep "Throughput:" benchmarks/${f}_raw.txt | awk '{print $2}' | awk '{s+=$1; ss+=$1*$1; n++} END {if(n>1) printf "%.1f", sqrt((ss-s*s/n)/(n-1)); else print "0.0"}')
        RUNS=$(grep -c "Throughput:" benchmarks/${f}_raw.txt)
        echo "| ${f} | ${MEAN} | ${STDDEV} | ${RUNS} |" >> benchmarks/summary-v2.md
    fi
done

cat >> benchmarks/summary-v2.md << 'HEREDOC'

## LVM Stripe Size Sweep (2-disk, 5GB write)

| Stripe Size | Mean (MB/s) | Runs |
|-------------|------------|------|
HEREDOC

for STRIPE in 128 256 512 1024; do
    if [ -f "benchmarks/lvm_stripesize_${STRIPE}kb_raw.txt" ]; then
        MEAN=$(grep "Throughput:" benchmarks/lvm_stripesize_${STRIPE}kb_raw.txt | awk '{print $2}' | awk '{s+=$1; n++} END {if(n>0) printf "%.1f", s/n}')
        RUNS=$(grep -c "Throughput:" benchmarks/lvm_stripesize_${STRIPE}kb_raw.txt)
        echo "| ${STRIPE} KB | ${MEAN} | ${RUNS} |" >> benchmarks/summary-v2.md
    fi
done

cat >> benchmarks/summary-v2.md << 'HEREDOC'

## TieredVol Chunk Size Sweep (2-disk, 5GB write)

| Chunk Size | Mean (MB/s) | Runs |
|------------|------------|------|
HEREDOC

for CHUNK in 256 512 1024; do
    if [ -f "benchmarks/chunksize_${CHUNK}kb_raw.txt" ]; then
        MEAN=$(grep "Throughput:" benchmarks/chunksize_${CHUNK}kb_raw.txt | awk '{print $2}' | awk '{s+=$1; n++} END {if(n>0) printf "%.1f", s/n}')
        RUNS=$(grep -c "Throughput:" benchmarks/chunksize_${CHUNK}kb_raw.txt)
        echo "| ${CHUNK} KB | ${MEAN} | ${RUNS} |" >> benchmarks/summary-v2.md
    fi
done

echo "" >> benchmarks/summary-v2.md
echo "Run directory: benchmarks/run_${RUN_TS}" >> benchmarks/summary-v2.md
echo "Generated: $(date)" >> benchmarks/summary-v2.md
```

---

## Step 9: Print Comparison Table

After generating summary-v2.md, print a comparison table to the terminal:

```bash
echo ""
echo "=============================================="
echo "  FAIR COMPARISON: TieredVol vs LVM"
echo "  (Same disks, same carve sizes, O_DIRECT)"
echo "=============================================="
echo ""
printf "  %-35s %12s %12s\n" "Scenario" "TieredVol" "LVM"
printf "  %-35s %12s %12s\n" "---" "---" "---"
# Add parsed values from summary-v2.md
echo ""
echo "=============================================="
```

---

## Step 10: Git Commit and Push

```bash
cd /home/yu/TieredVol

git add benchmarks/
git status
git diff --staged --stat

git commit -m "bench: full retest with NVMe+SATA LVM baseline (fair comparison)

- TieredVol: 2-disk and 3-disk write/read (5 runs each)
- LVM: same-disk NVMe+SATA comparison (5 runs each)
- Supplementary: LVM stripe size sweep, chunk size sweep
- io_uring metrics: strace + perf stat for 3-disk analysis
- Raw output in benchmarks/run_${RUN_TS}/

$(git diff --staged --stat)"

git push origin main
echo "[DONE] All benchmarks completed and pushed."
```

---

## Error Handling

### If `tiered_setup --create` fails:
- Check if disks are already in use: `echo 950715 | sudo -S dmsetup ls | grep tv_`
- Clean up: `echo 950715 | sudo -S ./tiered_setup --destroy --name <vol>`
- Retry once, then skip and log error

### If `tiered_io --bench` fails:
- Check disk space: `df -h /dev/mapper/tv_*`
- Check dmesg for errors: `echo 950715 | sudo -S dmesg | tail -20`
- Retry once, then skip and log error

### If `tiered_io --path --bench --raw` fails on LVM LV:
- The LV may not support O_DIRECT. Fall back to filesystem benchmark:
  ```bash
  echo 950715 | sudo -S mkfs.ext4 "$LV_PATH"
  echo 950715 | sudo -S mkdir -p /mnt/lvtest
  echo 950715 | sudo -S mount "$LV_PATH" /mnt/lvtest
  echo 950715 | sudo -S ./tiered_io --path /mnt/lvtest --bench --size 5GB
  echo 950715 | sudo -S umount /mnt/lvtest
  ```
- Log that raw benchmark was not possible

### If strace/perf fails:
- Check if installed: `which strace perf`
- If not installed: `echo 950715 | sudo -S apt install strace linux-tools-common linux-tools-$(uname -r)`
- If still fails, skip and log

---

## Disk Name Detection

If the actual disk names differ from `nvme0n1`, `sda`, `sdb`, detect them:

```bash
echo 950715 | sudo -S ./tiered_setup --list 2>/dev/null | grep -v "loop" | grep -v "NAME"
```

Look for:
- NVMe device (name contains `nvme`)
- Two SATA devices (name starts with `s`)

Adjust all `--disks` arguments accordingly.

---

## Final Checklist

After all benchmarks complete, verify:

- [ ] All 17 raw output files exist in `benchmarks/`
- [ ] `benchmarks/summary-v2.md` is generated
- [ ] All TieredVol volumes are destroyed (`echo 950715 | sudo -S ./tiered_setup --list` shows no `tv_test_*`)
- [ ] All LVM volumes are destroyed
- [ ] Git commit is pushed to origin/main
- [ ] `git log --oneline -1` shows the benchmark commit
