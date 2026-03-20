#!/usr/bin/env python3
"""Parse MS-GC WATERFALL / VOLUME / BackgroundSweep logs and produce summary."""

import re, sys, statistics

log_file = sys.argv[1] if len(sys.argv) > 1 else "gc_phase_analysis.log"

# ── helpers ──
def strip_commas(s):
    return int(s.replace(",", ""))

def parse_waterfall(line):
    """Parse: MarkSetup=644(STW=514) ConcMark=5,363 MarkTerm=14(STW=32) PostMark=28 ConcSweep=87(STW=33) | totalSTW=579(8.9%)"""
    m = re.search(
        r"MarkSetup=([\d,]+)\(STW=([\d,]+)\)\s+"
        r"ConcMark=([\d,]+)\s+"
        r"MarkTerm=([\d,]+)\(STW=([\d,]+)\)\s+"
        r"PostMark=([\d,]+)\s+"
        r"ConcSweep=([\d,]+)\(STW=([\d,]+)\)\s+\|\s+"
        r"totalSTW=([\d,]+)\(([\d.]+)%\)",
        line,
    )
    if not m:
        return None
    g = [strip_commas(m.group(i)) for i in range(1, 10)]
    return {
        "markSetup": g[0], "markSetupStw": g[1],
        "concMark": g[2],
        "markTerm": g[3], "markTermStw": g[4],
        "postMark": g[5],
        "concSweep": g[6], "concSweepStw": g[7],
        "totalStw": g[8],
        "stwPct": float(m.group(10)),
    }

def parse_volume(line):
    m = re.search(
        r"marked (\d+) objects, trueLive ([\d,]+) B, roots (\d+), "
        r"wbBufEntries (\d+), assistCalls (\d+), assistMarked (\d+), "
        r"losFreed (\d+)\(([\d,]+) B\), finalizerEnqueued (\d+)",
        line,
    )
    if not m:
        return None
    return {
        "markedObj": int(m.group(1)),
        "trueLive": strip_commas(m.group(2)),
        "roots": int(m.group(3)),
        "wbBuf": int(m.group(4)),
        "assistCalls": int(m.group(5)),
        "assistMarked": int(m.group(6)),
        "losFreed": int(m.group(7)),
        "losFreedBytes": strip_commas(m.group(8)),
        "finEnqueued": int(m.group(9)),
    }

def parse_bgsweep(line):
    m = re.search(r"(\d+) regions swept, (\d+) released, ([\d,]+)us", line)
    if not m:
        return None
    return {
        "regions": int(m.group(1)),
        "released": int(m.group(2)),
        "timeUs": strip_commas(m.group(3)),
    }

# ── read log ──
waterfalls, volumes, bgsweeps = [], [], []
with open(log_file) as f:
    for line in f:
        if "WATERFALL" in line:
            r = parse_waterfall(line)
            if r: waterfalls.append(r)
        elif "VOLUME" in line:
            r = parse_volume(line)
            if r: volumes.append(r)
        elif "BackgroundSweep done" in line:
            r = parse_bgsweep(line)
            if r: bgsweeps.append(r)

N = len(waterfalls)
print(f"=== MS-GC Phase Analysis ({N} cycles) ===\n")

# ── timing summary ──
def avg(lst): return sum(lst) / len(lst) if lst else 0
def med(lst): return statistics.median(lst) if lst else 0
def p95(lst):
    if not lst: return 0
    s = sorted(lst)
    idx = int(len(s) * 0.95)
    return s[min(idx, len(s)-1)]
def mx(lst): return max(lst) if lst else 0

keys = [
    ("markSetup",    "MarkSetup(total)"),
    ("markSetupStw", "  └─ STW"),
    ("concMark",     "ConcMark"),
    ("markTerm",     "MarkTerm(total)"),
    ("markTermStw",  "  └─ STW"),
    ("postMark",     "PostMark"),
    ("concSweep",    "ConcSweep(total)"),
    ("concSweepStw", "  └─ STW"),
    ("totalStw",     "totalSTW"),
]

print(f"{'Phase':<22} {'Avg(us)':>10} {'Med(us)':>10} {'P95(us)':>10} {'Max(us)':>10}")
print("-" * 66)
for k, label in keys:
    vals = [w[k] for w in waterfalls]
    print(f"{label:<22} {avg(vals):>10.0f} {med(vals):>10.0f} {p95(vals):>10.0f} {mx(vals):>10.0f}")

stw_pcts = [w["stwPct"] for w in waterfalls]
print(f"\nSTW%:  avg={avg(stw_pcts):.1f}%  med={med(stw_pcts):.1f}%  p95={p95(stw_pcts):.1f}%  max={mx(stw_pcts):.1f}%")

# ── total wall per cycle ──
wall = [w["markSetup"] + w["concMark"] + w["markTerm"] + w["postMark"] + w["concSweep"] for w in waterfalls]
print(f"\nTotal GC wall(us): avg={avg(wall):.0f}  med={med(wall):.0f}  p95={p95(wall):.0f}  max={mx(wall):.0f}")

# ── STW breakdown ──
setup_stw = [w["markSetupStw"] for w in waterfalls]
term_stw  = [w["markTermStw"] for w in waterfalls]
sweep_stw = [w["concSweepStw"] for w in waterfalls]
total_stw_avg = avg(setup_stw) + avg(term_stw) + avg(sweep_stw)
print(f"\nSTW Breakdown (avg us):")
print(f"  MarkSetup STW:  {avg(setup_stw):>8.0f}  ({avg(setup_stw)/total_stw_avg*100:>5.1f}%)")
print(f"  MarkTerm  STW:  {avg(term_stw):>8.0f}  ({avg(term_stw)/total_stw_avg*100:>5.1f}%)")
print(f"  SweepPrep STW:  {avg(sweep_stw):>8.0f}  ({avg(sweep_stw)/total_stw_avg*100:>5.1f}%)")
print(f"  ──────────────  ────────")
print(f"  Total STW:      {total_stw_avg:>8.0f}")

# ── BackgroundSweep ──
if bgsweeps:
    bg_us = [b["timeUs"] for b in bgsweeps]
    bg_reg = [b["regions"] for b in bgsweeps]
    bg_rel = [b["released"] for b in bgsweeps]
    print(f"\nBackgroundSweep ({len(bgsweeps)} invocations):")
    print(f"  Time(us):     avg={avg(bg_us):.0f}  med={med(bg_us):.0f}  p95={p95(bg_us):.0f}  max={mx(bg_us):.0f}")
    print(f"  Regions swept: avg={avg(bg_reg):.0f}  total={sum(bg_reg)}")
    print(f"  Regions released: avg={avg(bg_rel):.0f}  total={sum(bg_rel)}")

# ── Volume summary ──
if volumes:
    marked = [v["markedObj"] for v in volumes]
    live   = [v["trueLive"] for v in volumes]
    los_b  = [v["losFreedBytes"] for v in volumes]
    print(f"\nMarking Volume ({len(volumes)} cycles):")
    print(f"  Objects marked: avg={avg(marked):.0f}  med={med(marked):.0f}  max={mx(marked)}")
    print(f"  TrueLive(B):    avg={avg(live):,.0f}  med={med(live):,.0f}  max={mx(live):,}")
    print(f"  LOS freed(B):   avg={avg(los_b):,.0f}  total={sum(los_b):,}")
    wb = [v["wbBuf"] for v in volumes]
    ac = [v["assistCalls"] for v in volumes]
    am = [v["assistMarked"] for v in volumes]
    print(f"  WB buf entries: avg={avg(wb):.1f}  max={mx(wb)}")
    print(f"  Assist calls:   avg={avg(ac):.1f}  marked={sum(am)}")

# ── Top-5 longest STW cycles ──
indexed = [(i, w["totalStw"]) for i, w in enumerate(waterfalls)]
indexed.sort(key=lambda x: x[1], reverse=True)
print(f"\nTop-5 longest STW cycles:")
for rank, (i, stw) in enumerate(indexed[:5], 1):
    w = waterfalls[i]
    print(f"  #{rank} cycle {i+1}: totalSTW={stw}us  "
          f"MarkSetupSTW={w['markSetupStw']}  ConcMark={w['concMark']}  "
          f"marked={volumes[i]['markedObj'] if i < len(volumes) else '?'}")

print("\n=== END ===")
