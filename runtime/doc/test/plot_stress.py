#!/usr/bin/env python3
"""
GC Stress Test — 可视化脚本
读取 CopyGC 和 MS GC 的监控 CSV，生成对比图表。

用法：
  python3 plot_stress.py --copygc copygc_metrics.csv --ms ms_metrics.csv --out stress_comparison.png
"""

import csv
import sys
import os


def read_csv(path):
    times, rss, cpu = [], [], []
    with open(path, "r") as f:
        reader = csv.DictReader(f)
        for row in reader:
            times.append(float(row["time_s"]))
            rss.append(float(row["rss_mb"]))
            cpu.append(float(row["cpu_pct"]))
    return times, rss, cpu


def main():
    args = sys.argv[1:]
    copygc_csv = None
    ms_csv = None
    out_prefix = "stress_comparison"

    i = 0
    while i < len(args):
        if args[i] == "--copygc":
            copygc_csv = args[i + 1]; i += 2
        elif args[i] == "--ms":
            ms_csv = args[i + 1]; i += 2
        elif args[i] == "--out":
            out_prefix = args[i + 1].replace(".png", ""); i += 2
        else:
            i += 1

    if not copygc_csv or not ms_csv:
        print("Usage: python3 plot_stress.py --copygc <csv> --ms <csv> [--out prefix]")
        sys.exit(1)

    try:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
    except ImportError:
        print("matplotlib not installed, trying to install ...")
        import subprocess
        subprocess.check_call([sys.executable, "-m", "pip", "install", "matplotlib"])
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt

    cg_t, cg_rss, cg_cpu = read_csv(copygc_csv)
    ms_t, ms_rss, ms_cpu = read_csv(ms_csv)

    # Detect MS crash: if no data or very short, note it
    ms_crashed = len(ms_t) < 20 or (len(ms_t) > 0 and max(ms_t) < 10)
    crash_label = " [CRASHED @ {:.1f}s]".format(max(ms_t)) if ms_crashed and ms_t else ""

    fig, axes = plt.subplots(2, 1, figsize=(14, 8), sharex=True)
    fig.suptitle("GC Stress Test: CopyGC vs MarkSweep\n(Pinned Regions + Large Alloc, 60s target)" + 
                 ("\n⚠️ MarkSweep crashed" + crash_label if ms_crashed else ""), fontsize=14)

    # --- RSS ---
    ax1 = axes[0]
    ax1.plot(cg_t, cg_rss, label="CopyGC RSS", color="#2196F3", alpha=0.8, linewidth=1.2)
    ax1.plot(ms_t, ms_rss, label="MarkSweep RSS", color="#F44336", alpha=0.8, linewidth=1.2)
    ax1.set_ylabel("RSS (MB)")
    ax1.legend(loc="upper left")
    ax1.grid(True, alpha=0.3)
    # Mark crash point if MS crashed
    if ms_crashed and ms_t:
        ax1.axvline(x=max(ms_t), color="#F44336", linestyle=":", alpha=0.7, label="MS crash")
        ax1.annotate("CRASH", xy=(max(ms_t), max(ms_rss) if ms_rss else 0),
                     fontsize=10, color="#F44336", fontweight="bold",
                     xytext=(max(ms_t) + 2, (max(ms_rss) if ms_rss else 0) * 0.9),
                     arrowprops=dict(arrowstyle="->", color="#F44336"))
    # 标注 min/max/avg
    for label, t, rss, color in [("CopyGC", cg_t, cg_rss, "#2196F3"), ("MarkSweep", ms_t, ms_rss, "#F44336")]:
        if rss:
            avg_rss = sum(rss) / len(rss)
            ax1.axhline(y=avg_rss, color=color, linestyle="--", alpha=0.4)
            ax1.text(max(t) * 0.7, avg_rss, f"  {label} avg={avg_rss:.0f}MB", color=color, fontsize=8, va="bottom")

    # --- CPU ---
    ax2 = axes[1]
    # 用滑动平均平滑 CPU 数据
    def smooth(data, window=5):
        if len(data) < window:
            return data
        result = []
        for i in range(len(data)):
            start = max(0, i - window // 2)
            end = min(len(data), i + window // 2 + 1)
            result.append(sum(data[start:end]) / (end - start))
        return result

    ax2.plot(cg_t, smooth(cg_cpu), label="CopyGC CPU%", color="#2196F3", alpha=0.8, linewidth=1.2)
    ax2.plot(ms_t, smooth(ms_cpu), label="MarkSweep CPU%", color="#F44336", alpha=0.8, linewidth=1.2)
    ax2.set_ylabel("CPU (%)")
    ax2.set_xlabel("Time (seconds)")
    ax2.legend(loc="upper left")
    ax2.grid(True, alpha=0.3)

    plt.tight_layout()
    out_path = f"{out_prefix}.png"
    plt.savefig(out_path, dpi=150)
    print(f"[plot] Saved: {out_path}")

    # --- 额外生成一个 RSS 差值图 ---
    fig2, ax3 = plt.subplots(figsize=(14, 4))
    # 取两者共同时间范围
    min_len = min(len(cg_rss), len(ms_rss))
    if min_len > 0:
        diff = [ms_rss[i] - cg_rss[i] for i in range(min_len)]
        t_common = cg_t[:min_len]
        ax3.fill_between(t_common, diff, 0, where=[d >= 0 for d in diff],
                         color="#F44336", alpha=0.3, label="MS > CopyGC")
        ax3.fill_between(t_common, diff, 0, where=[d < 0 for d in diff],
                         color="#2196F3", alpha=0.3, label="CopyGC > MS")
        ax3.plot(t_common, diff, color="black", linewidth=0.8, alpha=0.6)
        ax3.axhline(y=0, color="gray", linestyle="-", alpha=0.5)
        ax3.set_ylabel("RSS Difference (MB)\n(MS - CopyGC)")
        ax3.set_xlabel("Time (seconds)")
        ax3.set_title("RSS Difference: MarkSweep - CopyGC")
        ax3.legend(loc="upper left")
        ax3.grid(True, alpha=0.3)
        plt.tight_layout()
        diff_path = f"{out_prefix}_diff.png"
        plt.savefig(diff_path, dpi=150)
        print(f"[plot] Saved: {diff_path}")

    # 打印统计
    print("\n--- Statistics ---")
    for label, rss, cpu in [("CopyGC", cg_rss, cg_cpu), ("MarkSweep", ms_rss, ms_cpu)]:
        if rss:
            print(f"{label}:")
            print(f"  RSS: min={min(rss):.1f} MB, max={max(rss):.1f} MB, avg={sum(rss)/len(rss):.1f} MB")
            print(f"  CPU: min={min(cpu):.1f}%, max={max(cpu):.1f}%, avg={sum(cpu)/len(cpu):.1f}%")


if __name__ == "__main__":
    main()
