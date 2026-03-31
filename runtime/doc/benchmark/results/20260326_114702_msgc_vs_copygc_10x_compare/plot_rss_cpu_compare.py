#!/usr/bin/env python3
"""
Plot CopyGC vs MSGC — RSS memory trend + CPU trend on one figure.
Reads two CSVs (copygc_monitor.csv, msgc_monitor.csv) with columns:
  elapsed_ms, rss_kb, rss_mb, cpu_pct
"""
import sys, os, csv
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
import numpy as np


def load_csv(path):
    elapsed, rss_mb, cpu_pct = [], [], []
    with open(path) as f:
        reader = csv.DictReader(f)
        for row in reader:
            elapsed.append(float(row['elapsed_ms']) / 1000.0)  # seconds
            rss_mb.append(float(row['rss_mb']))
            cpu_pct.append(float(row['cpu_pct']))
    return np.array(elapsed), np.array(rss_mb), np.array(cpu_pct)


def smooth(data, window=5):
    """Simple moving average."""
    if len(data) < window:
        return data
    kernel = np.ones(window) / window
    return np.convolve(data, kernel, mode='same')


def main():
    base = os.path.dirname(os.path.abspath(__file__))
    copy_csv = os.path.join(base, 'copygc_monitor.csv')
    ms_csv = os.path.join(base, 'msgc_monitor.csv')

    if not os.path.exists(copy_csv):
        print(f"Missing: {copy_csv}")
        sys.exit(1)
    if not os.path.exists(ms_csv):
        print(f"Missing: {ms_csv}")
        sys.exit(1)

    t_copy, rss_copy, cpu_copy = load_csv(copy_csv)
    t_ms, rss_ms, cpu_ms = load_csv(ms_csv)

    # Smooth CPU for readability
    cpu_copy_s = smooth(cpu_copy, 7)
    cpu_ms_s = smooth(cpu_ms, 7)

    BLUE = '#1976D2'
    RED = '#E53935'

    fig, (ax1, ax2) = plt.subplots(2, 1, figsize=(14, 9), sharex=False)
    fig.suptitle('CopyGC vs MSGC — 10x Pressure, 16GB Heap', fontsize=16, fontweight='bold', y=0.97)

    # --- Panel 1: RSS Memory ---
    ax1.plot(t_copy, rss_copy, linewidth=1.2, color=BLUE, alpha=0.85, label=f'CopyGC (peak {rss_copy.max():.0f} MB)')
    ax1.plot(t_ms, rss_ms, linewidth=1.2, color=RED, alpha=0.85, label=f'MSGC (peak {rss_ms.max():.0f} MB)')
    ax1.fill_between(t_copy, 0, rss_copy, alpha=0.08, color=BLUE)
    ax1.fill_between(t_ms, 0, rss_ms, alpha=0.08, color=RED)
    ax1.set_ylabel('RSS Memory (MB)', fontsize=13)
    ax1.set_xlabel('Time (s)', fontsize=12)
    ax1.legend(fontsize=11, loc='upper right')
    ax1.grid(True, alpha=0.3)
    ax1.set_title('Memory Usage Trend', fontsize=13)

    # --- Panel 2: CPU Usage ---
    ax2.plot(t_copy, cpu_copy_s, linewidth=1.0, color=BLUE, alpha=0.85, label=f'CopyGC (avg {cpu_copy.mean():.1f}%)')
    ax2.plot(t_ms, cpu_ms_s, linewidth=1.0, color=RED, alpha=0.85, label=f'MSGC (avg {cpu_ms.mean():.1f}%)')
    ax2.fill_between(t_copy, 0, cpu_copy_s, alpha=0.08, color=BLUE)
    ax2.fill_between(t_ms, 0, cpu_ms_s, alpha=0.08, color=RED)
    ax2.set_ylabel('CPU Usage (%)', fontsize=13)
    ax2.set_xlabel('Time (s)', fontsize=12)
    ax2.legend(fontsize=11, loc='upper right')
    ax2.grid(True, alpha=0.3)
    ax2.set_title('CPU Usage Trend', fontsize=13)

    plt.tight_layout(rect=[0, 0, 1, 0.95])
    out = os.path.join(base, 'copygc_vs_msgc_16g_10x.png')
    fig.savefig(out, dpi=150, bbox_inches='tight')
    print(f"Saved: {out}")
    plt.close()


if __name__ == '__main__':
    main()
