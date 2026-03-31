#!/usr/bin/env python3
"""
Plot a single MSGC benchmark run from:
  - msgc_monitor.csv
  - msgc_gc.log

Outputs a 2x2 dashboard:
  1. RSS trend
  2. CPU trend
  3. GC starts per second
  4. HEU trigger reason mix per second (threshold vs pressure)
"""

from __future__ import annotations

import argparse
import csv
import re
from collections import defaultdict
from datetime import datetime
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt


GC_START_PATTERN = re.compile(r"\[MS-GC\] === GC START")
HEU_REASON_PATTERN = re.compile(r"\[MS-GC\] HEU trigger: .* reason=(threshold|pressure)")


def load_monitor(csv_path: Path):
    elapsed_s = []
    rss_mb = []
    cpu_pct = []
    with csv_path.open() as f:
        reader = csv.DictReader(f)
        for row in reader:
            elapsed_s.append(float(row["elapsed_ms"]) / 1000.0)
            rss_mb.append(float(row["rss_mb"]))
            cpu_pct.append(float(row["cpu_pct"]))
    return elapsed_s, rss_mb, cpu_pct


def parse_gc_log(log_path: Path):
    with log_path.open(errors="ignore") as f:
        lines = f.readlines()

    if not lines:
        return [], [], []

    start_ts = None
    gc_start_s = []
    threshold_s = []
    pressure_s = []

    for line in lines:
        if len(line) < 26:
            continue
        try:
            ts = datetime.strptime(line[:26], "%Y-%m-%d %H:%M:%S.%f")
        except ValueError:
            continue
        if start_ts is None:
            start_ts = ts
        rel_s = (ts - start_ts).total_seconds()
        if GC_START_PATTERN.search(line):
            gc_start_s.append(rel_s)
        m = HEU_REASON_PATTERN.search(line)
        if m:
            if m.group(1) == "threshold":
                threshold_s.append(rel_s)
            else:
                pressure_s.append(rel_s)

    return gc_start_s, threshold_s, pressure_s


def bucket_counts(samples_s, bucket_width_s=1.0):
    counts = defaultdict(int)
    for t in samples_s:
        bucket = int(t // bucket_width_s)
        counts[bucket] += 1
    if not counts:
        return [], []
    xs = sorted(counts)
    ys = [counts[x] for x in xs]
    return xs, ys


def moving_average(values, window=9):
    if not values:
        return values
    out = []
    half = window // 2
    n = len(values)
    for i in range(n):
        left = max(0, i - half)
        right = min(n, i + half + 1)
        out.append(sum(values[left:right]) / (right - left))
    return out


def main():
    parser = argparse.ArgumentParser(description="Plot a single MSGC run dashboard")
    parser.add_argument("--results-dir", required=True, help="Directory containing msgc_monitor.csv and msgc_gc.log")
    parser.add_argument("--output", default="msgc_trends_dashboard.png", help="Output PNG filename")
    args = parser.parse_args()

    results_dir = Path(args.results_dir)
    monitor_csv = results_dir / "msgc_monitor.csv"
    gc_log = results_dir / "msgc_gc.log"
    output_path = results_dir / args.output

    elapsed_s, rss_mb, cpu_pct = load_monitor(monitor_csv)
    gc_start_s, threshold_s, pressure_s = parse_gc_log(gc_log)

    gc_x, gc_y = bucket_counts(gc_start_s, 1.0)
    th_x, th_y = bucket_counts(threshold_s, 1.0)
    pr_x, pr_y = bucket_counts(pressure_s, 1.0)

    fig, axes = plt.subplots(2, 2, figsize=(15, 10))
    fig.suptitle("MSGC 10x Run Dashboard", fontsize=16, fontweight="bold")

    ax = axes[0][0]
    ax.plot(elapsed_s, rss_mb, color="#d32f2f", linewidth=1.2)
    ax.fill_between(elapsed_s, 0, rss_mb, color="#d32f2f", alpha=0.12)
    ax.set_title("RSS Trend")
    ax.set_xlabel("Time (s)")
    ax.set_ylabel("RSS (MB)")
    ax.grid(True, alpha=0.25)
    if rss_mb:
        peak_idx = max(range(len(rss_mb)), key=rss_mb.__getitem__)
        ax.scatter([elapsed_s[peak_idx]], [rss_mb[peak_idx]], color="#b71c1c", s=24)
        ax.annotate(f"Peak {rss_mb[peak_idx]:.1f} MB",
                    xy=(elapsed_s[peak_idx], rss_mb[peak_idx]),
                    xytext=(elapsed_s[peak_idx], rss_mb[peak_idx] + 20),
                    fontsize=9)

    ax = axes[0][1]
    cpu_smooth = moving_average(cpu_pct, 11)
    ax.plot(elapsed_s, cpu_pct, color="#1976d2", linewidth=0.8, alpha=0.35, label="raw")
    ax.plot(elapsed_s, cpu_smooth, color="#0d47a1", linewidth=1.4, label="smoothed")
    ax.set_title("CPU Trend")
    ax.set_xlabel("Time (s)")
    ax.set_ylabel("CPU %")
    ax.grid(True, alpha=0.25)
    ax.legend(loc="upper right", fontsize=9)

    ax = axes[1][0]
    ax.bar(gc_x, gc_y, width=0.9, color="#6a1b9a", alpha=0.85)
    ax.set_title("GC Starts Per Second")
    ax.set_xlabel("Time bucket (s)")
    ax.set_ylabel("GC starts")
    ax.grid(True, axis="y", alpha=0.25)

    ax = axes[1][1]
    if th_x or pr_x:
        max_bucket = max((th_x or [0]) + (pr_x or [0]))
        xs = list(range(max_bucket + 1))
        th_map = {x: y for x, y in zip(th_x, th_y)}
        pr_map = {x: y for x, y in zip(pr_x, pr_y)}
        th_vals = [th_map.get(x, 0) for x in xs]
        pr_vals = [pr_map.get(x, 0) for x in xs]
        ax.bar(xs, pr_vals, width=0.9, color="#ef6c00", alpha=0.8, label="pressure")
        ax.bar(xs, th_vals, width=0.9, bottom=pr_vals, color="#2e7d32", alpha=0.8, label="threshold")
    ax.set_title("HEU Trigger Mix Per Second")
    ax.set_xlabel("Time bucket (s)")
    ax.set_ylabel("trigger count")
    ax.grid(True, axis="y", alpha=0.25)
    ax.legend(loc="upper right", fontsize=9)

    plt.tight_layout()
    fig.subplots_adjust(top=0.92)
    plt.savefig(output_path, dpi=160)
    print(output_path)


if __name__ == "__main__":
    main()
