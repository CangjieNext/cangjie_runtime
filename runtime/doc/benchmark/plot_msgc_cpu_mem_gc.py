#!/usr/bin/env python3

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


TS_RE = re.compile(r"^(\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}\.\d+)")
GC_START_RE = re.compile(r"=== GC START cycle=(\d+)")
GC_TOTAL_RE = re.compile(r"total gc time: ([\d,]+) us")


def load_monitor(csv_path: Path):
    elapsed_s = []
    rss_mb = []
    cpu_pct = []
    with csv_path.open() as handle:
        reader = csv.DictReader(handle)
        for row in reader:
            elapsed_s.append(float(row["elapsed_ms"]) / 1000.0)
            rss_mb.append(float(row["rss_mb"]))
            cpu_pct.append(float(row["cpu_pct"]))
    return elapsed_s, rss_mb, cpu_pct


def moving_average(values, window=9):
    if not values:
        return values
    half = window // 2
    result = []
    for index in range(len(values)):
        left = max(0, index - half)
        right = min(len(values), index + half + 1)
        result.append(sum(values[left:right]) / (right - left))
    return result


def parse_timestamp(line: str):
    match = TS_RE.match(line)
    if match is None:
        return None
    return datetime.strptime(match.group(1), "%Y-%m-%d %H:%M:%S.%f")


def parse_gc_log(log_path: Path):
    gc_starts = []
    gc_durations = []
    start_ts = None
    current_cycle = None

    with log_path.open(errors="ignore") as handle:
        for line in handle:
            ts = parse_timestamp(line)
            if ts is None:
                continue
            if start_ts is None:
                start_ts = ts
            rel_s = (ts - start_ts).total_seconds()

            match = GC_START_RE.search(line)
            if match is not None:
                current_cycle = int(match.group(1))
                gc_starts.append((rel_s, current_cycle))
                continue

            match = GC_TOTAL_RE.search(line)
            if match is not None and current_cycle is not None:
                duration_ms = int(match.group(1).replace(",", "")) / 1000.0
                gc_durations.append((rel_s, current_cycle, duration_ms))

    return gc_starts, gc_durations


def bucket_counts(events, width_s=1.0):
    buckets = defaultdict(int)
    for when_s, _cycle in events:
        buckets[int(when_s // width_s)] += 1
    xs = sorted(buckets)
    ys = [buckets[x] for x in xs]
    return xs, ys


def plot_dashboard(monitor_csv: Path, gc_log: Path, output: Path, title: str):
    elapsed_s, rss_mb, cpu_pct = load_monitor(monitor_csv)
    gc_starts, gc_durations = parse_gc_log(gc_log)
    gc_x, gc_y = bucket_counts(gc_starts)

    fig, axes = plt.subplots(3, 1, figsize=(15, 11), sharex=False)
    fig.suptitle(title, fontsize=16, fontweight="bold")

    ax = axes[0]
    ax.plot(elapsed_s, rss_mb, color="#c62828", linewidth=1.2)
    ax.fill_between(elapsed_s, 0, rss_mb, color="#c62828", alpha=0.12)
    ax.set_title("Memory Usage Trend")
    ax.set_ylabel("RSS (MB)")
    ax.grid(True, alpha=0.25)

    ax = axes[1]
    cpu_smooth = moving_average(cpu_pct, 11)
    ax.plot(elapsed_s, cpu_pct, color="#42a5f5", linewidth=0.8, alpha=0.35, label="raw")
    ax.plot(elapsed_s, cpu_smooth, color="#1565c0", linewidth=1.4, label="smoothed")
    ax.set_title("CPU Usage Trend")
    ax.set_ylabel("CPU %")
    ax.grid(True, alpha=0.25)
    ax.legend(loc="upper right", fontsize=9)

    ax = axes[2]
    ax.bar(gc_x, gc_y, width=0.9, color="#6a1b9a", alpha=0.85)
    ax.set_title("GC Starts Per Second")
    ax.set_xlabel("Time (s)")
    ax.set_ylabel("GC starts")
    ax.grid(True, axis="y", alpha=0.25)

    plt.tight_layout()
    fig.subplots_adjust(top=0.93)
    plt.savefig(output, dpi=160)


def plot_gc_time_trend(gc_log: Path, output: Path, title: str):
    _gc_starts, gc_durations = parse_gc_log(gc_log)
    elapsed_s = [entry[0] for entry in gc_durations]
    cycles = [entry[1] for entry in gc_durations]
    durations_ms = [entry[2] for entry in gc_durations]
    duration_smooth = moving_average(durations_ms, 7)

    fig, ax = plt.subplots(figsize=(15, 5.5))
    ax.plot(elapsed_s, durations_ms, color="#ef6c00", linewidth=0.9, alpha=0.45, label="raw")
    ax.plot(elapsed_s, duration_smooth, color="#e65100", linewidth=1.6, label="smoothed")
    ax.scatter(elapsed_s, durations_ms, s=9, color="#ef6c00", alpha=0.35)

    if durations_ms:
        peak_index = max(range(len(durations_ms)), key=durations_ms.__getitem__)
        ax.annotate(
            f"cycle {cycles[peak_index]} peak {durations_ms[peak_index]:.1f} ms",
            xy=(elapsed_s[peak_index], durations_ms[peak_index]),
            xytext=(elapsed_s[peak_index], durations_ms[peak_index] * 1.08),
            fontsize=9,
        )

    ax.set_title(title)
    ax.set_xlabel("Time (s)")
    ax.set_ylabel("GC duration (ms)")
    ax.grid(True, alpha=0.25)
    ax.legend(loc="upper right", fontsize=9)
    plt.tight_layout()
    plt.savefig(output, dpi=160)


def main():
    parser = argparse.ArgumentParser(description="Plot MSGC CPU/RSS/GC trends and GC duration trend")
    parser.add_argument("--csv", required=True, help="Monitor CSV from rss_cpu_monitor.py")
    parser.add_argument("--log", required=True, help="MRT_REPORT GC log")
    parser.add_argument("--dashboard", default="msgc_cpu_mem_gc_dashboard.png", help="Output dashboard PNG")
    parser.add_argument("--gc-time", default="msgc_gc_time_trend.png", help="Output GC duration trend PNG")
    parser.add_argument("--title", default="MSGC Benchmark Trends", help="Base title")
    args = parser.parse_args()

    csv_path = Path(args.csv)
    log_path = Path(args.log)
    plot_dashboard(csv_path, log_path, Path(args.dashboard), f"{args.title} - CPU / Memory / GC Calls")
    plot_gc_time_trend(log_path, Path(args.gc_time), f"{args.title} - GC Duration Trend")
    print(Path(args.dashboard))
    print(Path(args.gc_time))


if __name__ == "__main__":
    main()