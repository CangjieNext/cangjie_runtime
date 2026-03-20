#!/usr/bin/env python3
"""Detailed MarkSetup STW breakdown analysis."""
import re, statistics, sys

log_file = sys.argv[1] if len(sys.argv) > 1 else "gc_phase_analysis.log"

def sc(s): return int(s.replace(',',''))

lines_ffs, lines_cb, lines_stw, lines_total = [], [], [], []

with open(log_file) as f:
    for line in f:
        if 'ForceFinishSweep:' in line and 'stats:' not in line:
            m = re.search(r'ForceFinishSweep: ([\d,]+)us', line)
            if m: lines_ffs.append(sc(m.group(1)))
        elif 'ClearMarkBitmaps:' in line:
            m = re.search(r'ClearMarkBitmaps: (\d+) regions, ([\d,]+)us', line)
            if m: lines_cb.append((int(m.group(1)), sc(m.group(2))))
        elif 'WATERFALL' in line:
            m = re.search(r'MarkSetup=([\d,]+)\(STW=([\d,]+)\)', line)
            if m: lines_stw.append((sc(m.group(1)), sc(m.group(2))))
        elif 'MarkSetup total:' in line:
            m = re.search(r'MarkSetup total: ([\d,]+)us', line)
            if m: lines_total.append(sc(m.group(1)))

N = min(len(lines_ffs), len(lines_cb), len(lines_stw), len(lines_total))
print(f'=== MarkSetup STW Breakdown ({N} cycles) ===')
print()
print(f"{'#':>3} {'Total':>7} {'STW':>7} {'FFS':>7} {'ClearBM':>8} {'Rgns':>6} {'STW-CB':>8} {'CB/STW':>7}")
print('-' * 65)

stw_vals, cb_vals, ffs_vals, other_vals, rgn_vals = [], [], [], [], []

for i in range(N):
    total = lines_total[i]
    stw = lines_stw[i][1]
    ffs = lines_ffs[i]
    cb_rgn, cb_us = lines_cb[i]
    other_stw = stw - cb_us
    pct = cb_us / stw * 100 if stw > 0 else 0
    print(f'{i+1:>3} {total:>7} {stw:>7} {ffs:>7} {cb_us:>8} {cb_rgn:>6} {other_stw:>8} {pct:>6.1f}%')
    stw_vals.append(stw); cb_vals.append(cb_us); ffs_vals.append(ffs)
    other_vals.append(other_stw); rgn_vals.append(cb_rgn)

def p95(lst):
    s = sorted(lst)
    return s[min(int(len(s)*0.95), len(s)-1)]

print('-' * 65)
print(f"{'AVG':>3} {sum(lines_total[:N])//N:>7} {sum(stw_vals)//N:>7} {sum(ffs_vals)//N:>7} {sum(cb_vals)//N:>8} {sum(rgn_vals)//N:>6} {sum(other_vals)//N:>8} {sum(cb_vals)/sum(stw_vals)*100:>6.1f}%")
print(f"{'MED':>3} {'':>7} {statistics.median(stw_vals):>7.0f} {statistics.median(ffs_vals):>7.0f} {statistics.median(cb_vals):>8.0f} {statistics.median(rgn_vals):>6.0f} {statistics.median(other_vals):>8.0f}")
print(f"{'P95':>3} {'':>7} {p95(stw_vals):>7} {p95(ffs_vals):>7} {p95(cb_vals):>8} {p95(rgn_vals):>6} {p95(other_vals):>8}")
print(f"{'MAX':>3} {'':>7} {max(stw_vals):>7} {max(ffs_vals):>7} {max(cb_vals):>8} {max(rgn_vals):>6} {max(other_vals):>8}")

print()
print("Column key:")
print("  Total   = MarkSetup total (incl non-STW ForceFinishSweep + EnumRoots)")
print("  STW     = STW wall clock (ScopedStopTheWorld duration)")
print("  FFS     = ForceFinishSweep (BEFORE STW, concurrent)")
print("  ClearBM = ClearMarkBitmaps inside STW (memset per region)")
print("  Rgns    = regions cleared")
print("  STW-CB  = STW minus ClearBitmaps = FlushTL + ClearLOS + TransitionPhase + stack scanning")
print("  CB/STW  = ClearBitmaps as % of STW")

# Correlation: regions vs ClearBM time
print()
print("=== Correlation: region count vs ClearBitmap time ===")
for bucket_lo, bucket_hi in [(0, 1000), (1000, 2500), (2500, 4000), (4000, 6000)]:
    subset_us = [cb_vals[i] for i in range(N) if bucket_lo <= rgn_vals[i] < bucket_hi]
    subset_rg = [rgn_vals[i] for i in range(N) if bucket_lo <= rgn_vals[i] < bucket_hi]
    if subset_us:
        avg_rg = sum(subset_rg) / len(subset_rg)
        avg_us = sum(subset_us) / len(subset_us)
        us_per_rgn = avg_us / avg_rg if avg_rg > 0 else 0
        print(f"  {bucket_lo:>5}-{bucket_hi:>5} rgns: n={len(subset_us):>3}, avg_rgns={avg_rg:>6.0f}, avg_us={avg_us:>6.0f}, us/rgn={us_per_rgn:.2f}")
