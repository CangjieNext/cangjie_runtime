#!/usr/bin/env python3
"""Compare MarkSetup STW before/after ClearMarkBitmaps optimization."""
import re, sys, statistics

def sc(s):
    return int(s.replace(',', ''))

def parse_log(path):
    """Extract per-cycle ClearBitmaps us, MarkSetup STW us, totalSTW us."""
    cycles = []
    cb_times = []
    waterfall = []
    
    with open(path) as f:
        for line in f:
            # ClearMarkBitmaps line
            m = re.search(r'ClearMarkBitmaps\S*:\s*(\d[\d,]*)\s*regions?,\s*([\d,]+)us', line)
            if m:
                cb_times.append((sc(m.group(1)), sc(m.group(2))))
            
            # WATERFALL line
            m = re.search(r'MarkSetup=([\d,]+)\(STW=([\d,]+)\).*totalSTW=([\d,]+)', line)
            if m:
                waterfall.append({
                    'markSetup': sc(m.group(1)),
                    'markSetupSTW': sc(m.group(2)),
                    'totalSTW': sc(m.group(3)),
                })
    
    # Zip together
    n = min(len(cb_times), len(waterfall))
    for i in range(n):
        cycles.append({
            'cbRegions': cb_times[i][0],
            'cbUs': cb_times[i][1],
            'markSetup': waterfall[i]['markSetup'],
            'markSetupSTW': waterfall[i]['markSetupSTW'],
            'totalSTW': waterfall[i]['totalSTW'],
        })
    return cycles

def stats(values):
    if not values:
        return {'avg': 0, 'med': 0, 'p95': 0, 'min': 0, 'max': 0}
    s = sorted(values)
    return {
        'avg': int(statistics.mean(s)),
        'med': int(statistics.median(s)),
        'p95': s[int(len(s) * 0.95)] if len(s) >= 5 else s[-1],
        'min': s[0],
        'max': s[-1],
    }

def main():
    if len(sys.argv) < 2:
        print(f"Usage: {sys.argv[0]} <optimized.log> [baseline.log]")
        sys.exit(1)
    
    opt = parse_log(sys.argv[1])
    base = parse_log(sys.argv[2]) if len(sys.argv) > 2 else None
    
    print(f"=== ClearMarkBitmaps Optimization Analysis ===\n")
    
    # Optimized data table
    print(f"Optimized ({len(opt)} cycles):")
    print(f"{'#':>4}  {'CB(us)':>8}  {'Rgns':>6}  {'SetupSTW':>9}  {'TotalSTW':>9}")
    print("-" * 45)
    for i, c in enumerate(opt, 1):
        print(f"{i:4d}  {c['cbUs']:>8,}  {c['cbRegions']:>6,}  {c['markSetupSTW']:>9,}  {c['totalSTW']:>9,}")
    
    cb_vals = [c['cbUs'] for c in opt]
    stw_vals = [c['markSetupSTW'] for c in opt]
    total_stw_vals = [c['totalSTW'] for c in opt]
    
    cb_s = stats(cb_vals)
    stw_s = stats(stw_vals)
    total_s = stats(total_stw_vals)
    
    print(f"\nOptimized Summary (ClearBitmap us): avg={cb_s['avg']}, med={cb_s['med']}, "
          f"P95={cb_s['p95']}, min={cb_s['min']}, max={cb_s['max']}")
    print(f"Optimized Summary (MarkSetup STW): avg={stw_s['avg']}, med={stw_s['med']}, "
          f"P95={stw_s['p95']}, min={stw_s['min']}, max={stw_s['max']}")
    print(f"Optimized Summary (Total STW):     avg={total_s['avg']}, med={total_s['med']}, "
          f"P95={total_s['p95']}, min={total_s['min']}, max={total_s['max']}")
    
    if base:
        base_cb = [c['cbUs'] for c in base]
        base_stw = [c['markSetupSTW'] for c in base]
        base_total = [c['totalSTW'] for c in base]
        
        bcb = stats(base_cb)
        bstw = stats(base_stw)
        btotal = stats(base_total)
        
        print(f"\n--- Comparison ---")
        print(f"  ClearBitmap: {bcb['avg']}us → {cb_s['avg']}us  "
              f"({cb_s['avg']/max(1,bcb['avg'])*100:.1f}% of baseline)")
        print(f"  MarkSetup STW: {bstw['avg']}us → {stw_s['avg']}us  "
              f"({stw_s['avg']/max(1,bstw['avg'])*100:.1f}% of baseline, "
              f"saved {bstw['avg']-stw_s['avg']}us avg)")
        print(f"  Total STW: {btotal['avg']}us → {total_s['avg']}us  "
              f"({total_s['avg']/max(1,btotal['avg'])*100:.1f}% of baseline, "
              f"saved {btotal['avg']-total_s['avg']}us avg)")

if __name__ == '__main__':
    main()
