#!/usr/bin/env python3
"""
TieredVol Cross-Reference Verification Script

Parses fio JSON output, compares with thesis/README numbers,
and generates a discrepancy report.

Usage:
    python3 verify_numbers.py <raw_dir>
    python3 verify_numbers.py raw_20260725_093018
"""

import json, os, sys, glob
from collections import defaultdict

# ============================================================
# Thesis reference numbers (Ch8 Table 8.4.2)
# ============================================================
THESIS_CH8 = {
    'static_128k_read':  1574, 'static_128k_write':  1415,
    'static_4k_read':    1483, 'static_4k_write':     845,
    'adaptive_128k_read': 349, 'adaptive_128k_write': 492,
    'adaptive_4k_read':   319, 'adaptive_4k_write':   231,
    'random_128k_read':   516, 'random_128k_write':  1039,
    'random_4k_read':     598, 'random_4k_write':     386,
}

# BENCHMARK.md BS sweep (MB/s)
BENCHMARK_BS = {
    '128k': 1450, '256k': 1459, '512k': 1450, '1m': 1480,
    '2m': 1495, '4m': 1426, '8m': 1403, '16m': 1426,
}

# BENCHMARK.md QD sweep (MB/s)
BENCHMARK_QD = {
    1: 1353, 32: 1168, 128: 1495, 256: 1499, 512: 1452, 1024: 1469,
}

# Ch8 efficiency
THESIS_EFF = {
    '2disk_1_7': (1713, 1370, 0.80),
    '3disk_1_1_6': (1999, 1499, 0.75),
}

# Ch5 userspace
THESIS_CH5 = {
    '2disk_weighted': (1117, 87.1, 1.27),
    '3disk_weighted': (1369, 79.0, 1.16),
}

# Disk speeds (from setup)
DISK_SPEEDS = {
    'nvme0n1': {'write': 1407, 'read': 1407},  # MiB/s measured
    'sdb': {'write': 447, 'read': 447},
    'sdc': {'write': 442, 'read': 442},
}


def parse_fio_json(filepath):
    """Parse fio JSON and return total bandwidth in MiB/s."""
    with open(filepath) as f:
        d = json.load(f)
    total = 0
    for job in d['jobs']:
        for key in ['read', 'write']:
            if key in job and job[key]['bw'] > 0:
                total += job[key]['bw'] / 1024  # KiB/s -> MiB/s
    return total


def parse_fio_params(filepath):
    """Extract fio parameters from JSON."""
    with open(filepath) as f:
        d = json.load(f)
    j0 = d['jobs'][0]
    opts = j0.get('job options', {})
    return {
        'bs': opts.get('bs', '?'),
        'rw': opts.get('rw', '?'),
        'iodepth': opts.get('iodepth', '?'),
        'numjobs': opts.get('numjobs', '?'),
        'size': opts.get('size', '?'),
        'runtime': opts.get('runtime', 'none'),
        'jobs': len(d['jobs']),
    }


def find_tests(raw_dir, prefix):
    """Find all run files for a given test prefix."""
    files = sorted(glob.glob(os.path.join(raw_dir, f'{prefix}_run*.json')))
    return files


def classify_test(filename):
    """Classify a test file into (policy, workload)."""
    base = os.path.basename(filename).replace('.json', '')
    # dm_static_128k_read_run1 -> static, 128k_read
    parts = base.split('_')
    if parts[0] == 'dm':
        policy = parts[1]
        workload = '_'.join(parts[2:-1])  # remove 'runN'
    elif parts[0] in ('bs', 'qd'):
        return parts[0], '_'.join(parts[1:-1])
    elif parts[0] == 'raw':
        return 'raw', '_'.join(parts[1:-1])
    else:
        return 'unknown', base
    return policy, workload


def main():
    if len(sys.argv) < 2:
        print("Usage: python3 verify_numbers.py <raw_dir>")
        sys.exit(1)

    raw_dir = sys.argv[1]
    if not os.path.isdir(raw_dir):
        print(f"Error: {raw_dir} not found")
        sys.exit(1)

    report = []
    report.append("# TieredVol Verification Report")
    report.append(f"**Directory:** {raw_dir}")
    report.append("")

    # ============================================================
    # Section 1: Policy Comparison (Ch8 Table 8.4.2)
    # ============================================================
    report.append("## 1. Policy Comparison (vs Ch8 Table 8.4.2)")
    report.append("")
    report.append("| Test | Run1 | Run2 | Run3 | Avg | Thesis | Delta | Status |")
    report.append("|------|------|------|------|-----|--------|-------|--------|")

    for prefix, label in [
        ('dm_static_128k_read',  'Static 128K Read'),
        ('dm_static_128k_write', 'Static 128K Write'),
        ('dm_static_4k_read',    'Static 4K Read'),
        ('dm_static_4k_write',   'Static 4K Write'),
        ('dm_adaptive_128k_read',  'Adaptive 128K Read'),
        ('dm_adaptive_128k_write', 'Adaptive 128K Write'),
        ('dm_adaptive_4k_read',    'Adaptive 4K Read'),
        ('dm_adaptive_4k_write',   'Adaptive 4K Write'),
        ('dm_random_128k_read',  'Random 128K Read'),
        ('dm_random_128k_write', 'Random 128K Write'),
        ('dm_random_4k_read',    'Random 4K Read'),
        ('dm_random_4k_write',   'Random 4K Write'),
    ]:
        files = find_tests(raw_dir, prefix)
        bws = [parse_fio_json(f) for f in files]
        while len(bws) < 3:
            bws.append(0)

        key = prefix.replace('dm_', '')
        thesis_val = THESIS_CH8.get(key, 0)

        if thesis_val > 0:
            avg = sum(bws[:3]) / max(len(bws[:3]), 1)
            delta = (avg - thesis_val) / thesis_val * 100
            if abs(delta) < 5:
                status = 'OK'
            elif abs(delta) < 15:
                status = 'WARN'
            else:
                status = 'FLAG'
            report.append(
                f"| {label} | {bws[0]:.0f} | {bws[1]:.0f} | {bws[2]:.0f} "
                f"| {avg:.0f} | {thesis_val} | {delta:+.1f}% | {status} |"
            )
        else:
            avg = sum(bws[:3]) / max(len(bws[:3]), 1)
            report.append(
                f"| {label} | {bws[0]:.0f} | {bws[1]:.0f} | {bws[2]:.0f} "
                f"| {avg:.0f} | — | — | — |"
            )

    report.append("")

    # ============================================================
    # Section 2: Block Size Sweep
    # ============================================================
    report.append("## 2. Block Size Sweep (vs BENCHMARK.md)")
    report.append("")
    report.append("| BS | Measured (MiB/s) | Measured (MB/s) | Thesis (MB/s) | Delta |")
    report.append("|----|-------------------|-----------------|---------------|-------|")

    for bs in ['128k', '256k', '512k', '1m', '2m', '4m', '8m', '16m']:
        f = os.path.join(raw_dir, f'bs_sweep_{bs}.json')
        if os.path.exists(f):
            bw = parse_fio_json(f)
            bw_mb = bw * 1.048576
            thesis_mb = BENCHMARK_BS.get(bs, 0)
            delta = (bw_mb - thesis_mb) / thesis_mb * 100 if thesis_mb else 0
            status = 'OK' if abs(delta) < 10 else 'FLAG'
            report.append(f"| {bs} | {bw:.0f} | {bw_mb:.0f} | {thesis_mb} | {delta:+.1f}% |")

    report.append("")

    # ============================================================
    # Section 3: Queue Depth Sweep
    # ============================================================
    report.append("## 3. Queue Depth Sweep (vs BENCHMARK.md)")
    report.append("")
    report.append("| QD | Measured (MiB/s) | Measured (MB/s) | Thesis (MB/s) | Delta |")
    report.append("|----|-------------------|-----------------|---------------|-------|")

    for qd in [1, 32, 128, 256, 512, 1024]:
        f = os.path.join(raw_dir, f'qd_sweep_{qd}.json')
        if os.path.exists(f):
            bw = parse_fio_json(f)
            bw_mb = bw * 1.048576
            thesis_mb = BENCHMARK_QD.get(qd, 0)
            delta = (bw_mb - thesis_mb) / thesis_mb * 100 if thesis_mb else 0
            report.append(f"| {qd} | {bw:.0f} | {bw_mb:.0f} | {thesis_mb} | {delta:+.1f}% |")

    report.append("")

    # ============================================================
    # Section 4: Raw NVMe Baseline
    # ============================================================
    report.append("## 4. Raw NVMe Baseline")
    report.append("")
    report.append("| Run | Write (MiB/s) | Write (MB/s) |")
    report.append("|-----|---------------|--------------|")

    raw_bws = []
    for i in range(1, 4):
        f = os.path.join(raw_dir, f'raw_nvme_2m_run{i}.json')
        if os.path.exists(f):
            bw = parse_fio_json(f)
            raw_bws.append(bw)
            report.append(f"| {i} | {bw:.0f} | {bw*1.048576:.0f} |")

    if raw_bws:
        avg_raw = sum(raw_bws) / len(raw_bws)
        report.append(f"| **Avg** | **{avg_raw:.0f}** | **{avg_raw*1.048576:.0f}** |")
        report.append(f"")
        report.append(f"Thesis raw NVMe: 1475 MB/s, Measured: {avg_raw*1.048576:.0f} MB/s, "
                      f"Delta: {(avg_raw*1.048576 - 1475)/1475*100:+.1f}%")

    report.append("")

    # ============================================================
    # Section 5: Self-Consistency Checks
    # ============================================================
    report.append("## 5. Self-Consistency Checks")
    report.append("")

    # T_ideal calculation
    report.append("### T_ideal Verification")
    report.append("")
    report.append("Formula: $T_\\text{{ideal}} = W / \\max_i(w_i / v_i)$")
    report.append("")

    # 2-disk [1,7] with measured NVMe speed
    if raw_bws:
        nvme_speed = avg_raw  # MiB/s
        sdb_speed = 447
        sdc_speed = 442

        # 3-disk [1,1,6]
        W3 = 8
        t_ideal_3d = W3 / max(1/sdb_speed, 1/sdc_speed, 6/nvme_speed)
        report.append(f"- NVMe measured: {avg_raw:.0f} MiB/s ({avg_raw*1.048576:.0f} MB/s)")
        report.append(f"- 3-disk [1,1,6] T_ideal: {t_ideal_3d:.0f} MiB/s ({t_ideal_3d*1.048576:.0f} MB/s)")
        report.append(f"- Thesis T_ideal: 1999 MB/s")

        # Efficiency
        if raw_bws:
            # Get static 3-disk write
            static_write_files = find_tests(raw_dir, 'dm_static_128k_write')
            if static_write_files:
                static_write_bw = parse_fio_json(static_write_files[0])
                static_write_mb = static_write_bw * 1.048576
                eta = static_write_mb / (t_ideal_3d * 1.048576) * 100
                report.append(f"- Static 128K write measured: {static_write_bw:.0f} MiB/s ({static_write_mb:.0f} MB/s)")
                report.append(f"- Efficiency (η): {eta:.1f}% (thesis: 75%)")

    report.append("")

    # ============================================================
    # Section 6: Summary & Flags
    # ============================================================
    report.append("## 6. Flags & Notes")
    report.append("")

    # Check for anomalies
    for prefix, label in [
        ('dm_static_128k_read',  'Static 128K Read'),
        ('dm_static_128k_write', 'Static 128K Write'),
        ('dm_static_4k_read',    'Static 4K Read'),
        ('dm_static_4k_write',   'Static 4K Write'),
    ]:
        files = find_tests(raw_dir, prefix)
        bws = [parse_fio_json(f) for f in files]
        key = prefix.replace('dm_', '')
        thesis_val = THESIS_CH8.get(key, 0)
        if bws and thesis_val:
            avg = sum(bws) / len(bws)
            delta = (avg - thesis_val) / thesis_val * 100
            if abs(delta) > 10:
                report.append(f"- **{label}**: delta {delta:+.1f}% — "
                              f"possible causes: SLC cache state, NVMe GC after format, "
                              f"different segment weights than thesis")

    report.append("")
    report.append("---")
    report.append(f"*Generated by verify_numbers.py on {raw_dir}*")

    # Write report
    report_path = os.path.join(os.path.dirname(raw_dir), 'verification_report.md')
    with open(report_path, 'w') as f:
        f.write('\n'.join(report))

    print(f"Report written to: {report_path}")
    print()
    print('\n'.join(report))


if __name__ == '__main__':
    main()
