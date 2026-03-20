#!/usr/bin/env python3
"""Compare GC WATERFALL STW timing between two logs."""
import re, sys, statistics

def sc(s):
    return int(s.replace(',', ''))

def parse_waterfall(path):
    cycles = []
    with open(path) as f:
        for line in f:
            m = re.search(
                r'WATERFALL\(us\):\s*MarkSetup=([\d,]+)\(STW=([\d,]+)\)\s*'
                r'ConcMark=([\d,]+)\s*'
                r'MarkTerm=([\d,]+)\(STW=([\d,]+)\)\s*'
                r'PostMark=([\d,]+)\s*'
                r'ConcSweep=([\d,]+)\(STW=([\d,]+)\)\s*'
                r'\|\s*totalSTW=([\d,]+)',
                line)
            if m:
                cycles.append({
                    'markSetup': sc(m.group(1)),
                    'markSetupSTW': sc(m.group(2)),
                    'concMark': sc(m.group(3)),
                    'markTerm': sc(m.group(4)),
                    'markTermSTW': sc(m.group(5)),
                    'postMark': sc(m.group(6)),
                    'concSweep': sc(m.group(7)),
                    'concSweepSTW': sc(m.group(8)),
                    'totalSTW': sc(m.group(9)),
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

def print_stats(label, values):
    s = stats(values)
    print(f"  {label}: avg={s['avg']}, med={s['med']}, P95={s['p95']}, min={s['min']}, max={s['max']}")
    return s

def main():
    if len(sys.argv) < 3:
        print(f"Usage: {sys.argv[0]} <optimized.log> <baseline.log>")
        sys.exit(1)

    opt = parse_waterfall(sys.argv[1])
    base = parse_waterfall(sys.argv[2])

    print(f"=== GC STW Comparison: Optimized ({len(opt)} cycles) vs Baseline ({len(base)} cycles) ===\n")

    for label, data in [("Baseline", base), ("Optimized", opt)]:
        print(f"--- {label} ({len(data)} cycles) ---")
        ms_stw = print_stats("MarkSetup STW(us)", [c['markSetupSTW'] for c in data])
        mt_stw = print_stats("MarkTerm  STW(us)", [c['markTermSTW'] for c in data])
        cs_stw = print_stats("ConcSweep STW(us)", [c['concSweepSTW'] for c in data])
        total  = print_stats("Total STW    (us)", [c['totalSTW'] for c in data])
        print()

    # Direct comparison
    def cmp(name, base_vals, opt_vals):
        b = stats(base_vals)
        o = stats(opt_vals)
        saved_avg = b['avg'] - o['avg']
        saved_med = b['med'] - o['med']
        pct = o['med'] / max(1, b['med']) * 100
        print(f"  {name}:")
        print(f"    avg: {b['avg']:>6}us → {o['avg']:>6}us  (saved {saved_avg}us, {o['avg']/max(1,b['avg'])*100:.1f}%)")
        print(f"    med: {b['med']:>6}us → {o['med']:>6}us  (saved {saved_med}us, {pct:.1f}%)")

    print("=== Improvement Summary ===")
    cmp("MarkSetup STW", [c['markSetupSTW'] for c in base], [c['markSetupSTW'] for c in opt])
    cmp("Total STW",     [c['totalSTW'] for c in base],     [c['totalSTW'] for c in opt])

if __name__ == '__main__':
    main()
