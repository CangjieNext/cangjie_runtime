#!/usr/bin/env python3
"""
compare_results.py — 对比两次运行结果（通常为 CopyGC vs MSGC）。

用法：
    python3 compare_results.py \
        --left  <copygc_result_dir> \
        --right <msgc_result_dir> \
        --left-label CopyGC \
        --right-label MSGC \
        --output compare_report.md
"""

from __future__ import annotations

import argparse
import csv
import math
import re
from datetime import datetime
from pathlib import Path
from typing import Optional

# === Regex: MSGC ===
MSGC_START_RE = re.compile(
    r"^(?P<ts>\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}\.\d{6}).*?\[MS-GC\] === GC START cycle=(?P<cycle>\d+) === .*?reason=(?P<reason_code>\d+)\((?P<reason_name>[^)]+)\)"
)
MSGC_TOTAL_RE = re.compile(r"total gc time:\s*([\d,]+)\s*us")

# === Regex: CopyGC ===
COPYGC_START_RE = re.compile(
    r"^(?P<ts>\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}\.\d{6}).*?\[GC\] Start (?P<collector>.+?)\s+(?P<reason_name>\S+)\s+gcIndex=\s*(?P<cycle>\d+)"
)
COPYGC_TOTAL_RE = re.compile(r"total gc time:\s*([\d,]+)\s*us")

# === Regex: 通用 ===
BENCH_TOTAL_RE = re.compile(r"^\[BENCH\]\s+Total wall time:\s+(\d+) ms$", re.MULTILINE)
MONITOR_TOTAL_RE = re.compile(r"^Total time:\s+(\d+)ms$", re.MULTILINE)


def cint(s: str) -> int:
    return int(s.replace(",", ""))


def percentile(values: list[float], pct: float) -> float:
    if not values:
        return 0.0
    if len(values) == 1:
        return values[0]
    rank = (len(values) - 1) * pct
    low = math.floor(rank)
    high = math.ceil(rank)
    if low == high:
        return values[low]
    weight = rank - low
    return values[low] * (1.0 - weight) + values[high] * weight


def fmt(value: float) -> str:
    return f"{value:.2f}"


def delta_str(left: float, right: float) -> str:
    """返回右侧相对于左侧的变化百分比字符串"""
    if left == 0:
        return "N/A"
    d = (right - left) / left * 100
    sign = "+" if d >= 0 else ""
    return f"{sign}{d:.1f}%"


# ─────────── 解析函数 ───────────

def detect_gc_mode(gc_log_path: Path) -> str:
    text = gc_log_path.read_text(errors="replace")
    if "[MS-GC]" in text:
        return "msgc"
    if "[GC] Start" in text:
        return "copygc"
    return "unknown"


def parse_gc_times(gc_log_path: Path) -> list[float]:
    """解析所有 GC 周期的时间 (ms)"""
    gc_mode = detect_gc_mode(gc_log_path)
    times_us: list[int] = []

    if gc_mode == "msgc":
        current_us = 0
        in_cycle = False
        for line in gc_log_path.read_text(errors="replace").splitlines():
            if MSGC_START_RE.match(line):
                if in_cycle and current_us > 0:
                    times_us.append(current_us)
                current_us = 0
                in_cycle = True
            if in_cycle:
                m = MSGC_TOTAL_RE.search(line)
                if m:
                    current_us = cint(m.group(1))
        if in_cycle and current_us > 0:
            times_us.append(current_us)

    elif gc_mode == "copygc":
        current_us = 0
        in_cycle = False
        for line in gc_log_path.read_text(errors="replace").splitlines():
            if COPYGC_START_RE.match(line):
                if in_cycle and current_us > 0:
                    times_us.append(current_us)
                current_us = 0
                in_cycle = True
            if in_cycle:
                m = COPYGC_TOTAL_RE.search(line)
                if m:
                    current_us = cint(m.group(1))
        if in_cycle and current_us > 0:
            times_us.append(current_us)

    return [t / 1000.0 for t in times_us]


def parse_rss_csv(path: Path) -> dict:
    elapsed_s: list[float] = []
    rss_mb: list[float] = []
    cpu_pct: list[float] = []

    if not path.exists():
        return {}

    with path.open() as f:
        reader = csv.DictReader(f)
        for row in reader:
            elapsed_s.append(float(row["elapsed_ms"]) / 1000.0)
            rss_mb.append(float(row["rss_mb"]))
            cpu_pct.append(float(row["cpu_pct"]))

    if not rss_mb:
        return {}

    sorted_rss = sorted(rss_mb)
    sorted_cpu = sorted(cpu_pct)
    return {
        "elapsed_s": elapsed_s,
        "rss_mb": rss_mb,
        "cpu_pct": cpu_pct,
        "rss_avg_mb": sum(rss_mb) / len(rss_mb),
        "rss_peak_mb": max(rss_mb),
        "rss_p95_mb": percentile(sorted_rss, 0.95),
        "cpu_avg_pct": sum(cpu_pct) / len(cpu_pct),
        "cpu_peak_pct": max(cpu_pct),
        "cpu_p95_pct": percentile(sorted_cpu, 0.95),
        "sample_count": len(rss_mb),
    }


def parse_wall_time(results_dir: Path) -> int | None:
    for name in ("stdout.txt", "monitor.txt"):
        path = results_dir / name
        if path.exists():
            text = path.read_text(errors="replace")
            m = BENCH_TOTAL_RE.search(text)
            if m:
                return int(m.group(1))
            m = MONITOR_TOTAL_RE.search(text)
            if m:
                return int(m.group(1))
    return None


def read_metadata(results_dir: Path) -> dict[str, str]:
    metadata: dict[str, str] = {}
    path = results_dir / "run_metadata.env"
    if not path.exists():
        return metadata
    for line in path.read_text().splitlines():
        if not line or "=" not in line:
            continue
        key, value = line.split("=", 1)
        metadata[key.strip()] = value.strip()
    return metadata


# ─────────── 数据收集 ───────────

class RunData:
    def __init__(self, results_dir: Path, label: str):
        self.dir = results_dir
        self.label = label
        self.metadata = read_metadata(results_dir)
        gc_log = results_dir / "gc.log"
        self.gc_mode = detect_gc_mode(gc_log) if gc_log.exists() else "unknown"
        self.gc_times_ms = parse_gc_times(gc_log) if gc_log.exists() else []
        self.rss = parse_rss_csv(results_dir / "rss.csv")
        self.wall_ms = parse_wall_time(results_dir)

    @property
    def gc_count(self) -> int:
        return len(self.gc_times_ms)

    @property
    def gc_total_ms(self) -> float:
        return sum(self.gc_times_ms)

    @property
    def gc_avg_ms(self) -> float:
        return self.gc_total_ms / self.gc_count if self.gc_count else 0

    @property
    def gc_max_ms(self) -> float:
        return max(self.gc_times_ms) if self.gc_times_ms else 0

    @property
    def gc_p95_ms(self) -> float:
        return percentile(sorted(self.gc_times_ms), 0.95) if self.gc_times_ms else 0


# ─────────── 图表生成 ───────────

def generate_compare_plots(left: "RunData", right: "RunData", output_dir: Path) -> list[str]:
    """生成对比图表，返回生成的文件名列表"""
    try:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
        import matplotlib.font_manager as fm
    except ImportError:
        print("警告: matplotlib 不可用，跳过图表生成")
        return []

    # 设置中文字体（macOS 优先使用 PingFang SC，回退到 STHeiti/Arial Unicode MS）
    zh_fonts = ["PingFang SC", "STHeiti", "Arial Unicode MS", "Heiti SC", "SimHei"]
    found_font = None
    for name in zh_fonts:
        matches = fm.findSystemFonts(fontpaths=None)
        for m in fm.fontManager.ttflist:
            if name in m.name:
                found_font = name
                break
        if found_font:
            break
    if found_font:
        plt.rcParams["font.sans-serif"] = [found_font, "DejaVu Sans"]
    else:
        plt.rcParams["font.sans-serif"] = ["DejaVu Sans"]
    plt.rcParams["axes.unicode_minus"] = False

    generated: list[str] = []
    left_color = "#1f77b4"   # 蓝色
    right_color = "#d62728"  # 红色

    # ── 图1: RSS + CPU 时间序列对比 ──
    has_left_ts = left.rss.get("elapsed_s") and left.rss.get("rss_mb")
    has_right_ts = right.rss.get("elapsed_s") and right.rss.get("rss_mb")

    if has_left_ts or has_right_ts:
        fig, (ax_rss, ax_cpu) = plt.subplots(2, 1, figsize=(14, 8), sharex=False)

        if has_left_ts:
            ax_rss.plot(left.rss["elapsed_s"], left.rss["rss_mb"],
                        color=left_color, linewidth=1.4, label=left.label, alpha=0.85)
            ax_rss.fill_between(left.rss["elapsed_s"], 0, left.rss["rss_mb"],
                                color=left_color, alpha=0.10)
            ax_cpu.plot(left.rss["elapsed_s"], left.rss["cpu_pct"],
                        color=left_color, linewidth=1.2, label=left.label, alpha=0.85)
        if has_right_ts:
            ax_rss.plot(right.rss["elapsed_s"], right.rss["rss_mb"],
                        color=right_color, linewidth=1.4, label=right.label, alpha=0.85)
            ax_rss.fill_between(right.rss["elapsed_s"], 0, right.rss["rss_mb"],
                                color=right_color, alpha=0.10)
            ax_cpu.plot(right.rss["elapsed_s"], right.rss["cpu_pct"],
                        color=right_color, linewidth=1.2, label=right.label, alpha=0.85)

        ax_rss.set_ylabel("RSS (MB)")
        ax_rss.set_title(f"{left.label} vs {right.label} — RSS & CPU 时间序列")
        ax_rss.legend(loc="upper right")
        ax_rss.grid(True, alpha=0.25)
        ax_rss.set_xlabel("Elapsed (s)")

        ax_cpu.set_ylabel("CPU (%)")
        ax_cpu.set_xlabel("Elapsed (s)")
        ax_cpu.legend(loc="upper right")
        ax_cpu.grid(True, alpha=0.25)

        plt.tight_layout()
        fname = "compare_rss_cpu_timeline.png"
        plt.savefig(output_dir / fname, dpi=160)
        plt.close(fig)
        generated.append(fname)

    # ── 图2: 汇总指标柱状图 (2x2) ──
    fig, axes = plt.subplots(2, 2, figsize=(12, 8))
    bar_width = 0.35
    x = [0, 1]
    labels_pair = [left.label, right.label]
    colors_pair = [left_color, right_color]

    # (0,0) RSS: avg / peak / p95
    ax = axes[0][0]
    rss_metrics = ["rss_avg_mb", "rss_peak_mb", "rss_p95_mb"]
    rss_labels_short = ["Avg", "Peak", "P95"]
    x_rss = list(range(len(rss_metrics)))
    left_vals = [left.rss.get(k, 0) for k in rss_metrics]
    right_vals = [right.rss.get(k, 0) for k in rss_metrics]
    bars_l = ax.bar([i - bar_width / 2 for i in x_rss], left_vals, bar_width, label=left.label, color=left_color, alpha=0.85)
    bars_r = ax.bar([i + bar_width / 2 for i in x_rss], right_vals, bar_width, label=right.label, color=right_color, alpha=0.85)
    ax.set_xticks(x_rss)
    ax.set_xticklabels(rss_labels_short)
    ax.set_ylabel("MB")
    ax.set_title("RSS 内存")
    ax.legend(fontsize=8)
    ax.grid(True, axis="y", alpha=0.25)
    for bar_group in [bars_l, bars_r]:
        for bar in bar_group:
            h = bar.get_height()
            if h > 0:
                ax.text(bar.get_x() + bar.get_width() / 2, h, f"{h:.1f}",
                        ha="center", va="bottom", fontsize=7)

    # (0,1) CPU: avg / peak / p95
    ax = axes[0][1]
    cpu_metrics = ["cpu_avg_pct", "cpu_peak_pct", "cpu_p95_pct"]
    cpu_labels_short = ["Avg", "Peak", "P95"]
    x_cpu = list(range(len(cpu_metrics)))
    left_vals = [left.rss.get(k, 0) for k in cpu_metrics]
    right_vals = [right.rss.get(k, 0) for k in cpu_metrics]
    bars_l = ax.bar([i - bar_width / 2 for i in x_cpu], left_vals, bar_width, label=left.label, color=left_color, alpha=0.85)
    bars_r = ax.bar([i + bar_width / 2 for i in x_cpu], right_vals, bar_width, label=right.label, color=right_color, alpha=0.85)
    ax.set_xticks(x_cpu)
    ax.set_xticklabels(cpu_labels_short)
    ax.set_ylabel("%")
    ax.set_title("CPU 使用率")
    ax.legend(fontsize=8)
    ax.grid(True, axis="y", alpha=0.25)
    for bar_group in [bars_l, bars_r]:
        for bar in bar_group:
            h = bar.get_height()
            if h > 0:
                ax.text(bar.get_x() + bar.get_width() / 2, h, f"{h:.1f}",
                        ha="center", va="bottom", fontsize=7)

    # (1,0) GC 次数 & 总时间
    ax = axes[1][0]
    gc_bar_labels = ["GC Count", "Total (ms)", "Avg (ms)"]
    x_gc = list(range(len(gc_bar_labels)))
    left_vals = [left.gc_count, left.gc_total_ms, left.gc_avg_ms]
    right_vals = [right.gc_count, right.gc_total_ms, right.gc_avg_ms]
    bars_l = ax.bar([i - bar_width / 2 for i in x_gc], left_vals, bar_width, label=left.label, color=left_color, alpha=0.85)
    bars_r = ax.bar([i + bar_width / 2 for i in x_gc], right_vals, bar_width, label=right.label, color=right_color, alpha=0.85)
    ax.set_xticks(x_gc)
    ax.set_xticklabels(gc_bar_labels)
    ax.set_title("GC 次数与时间")
    ax.legend(fontsize=8)
    ax.grid(True, axis="y", alpha=0.25)
    for bar_group in [bars_l, bars_r]:
        for bar in bar_group:
            h = bar.get_height()
            if h > 0:
                ax.text(bar.get_x() + bar.get_width() / 2, h, f"{h:.1f}",
                        ha="center", va="bottom", fontsize=7)

    # (1,1) GC 延迟: max / p95 / wall time
    ax = axes[1][1]
    lat_labels = ["Max (ms)", "P95 (ms)", "Wall (s)"]
    x_lat = list(range(len(lat_labels)))
    left_vals = [left.gc_max_ms, left.gc_p95_ms, (left.wall_ms or 0) / 1000.0]
    right_vals = [right.gc_max_ms, right.gc_p95_ms, (right.wall_ms or 0) / 1000.0]
    bars_l = ax.bar([i - bar_width / 2 for i in x_lat], left_vals, bar_width, label=left.label, color=left_color, alpha=0.85)
    bars_r = ax.bar([i + bar_width / 2 for i in x_lat], right_vals, bar_width, label=right.label, color=right_color, alpha=0.85)
    ax.set_xticks(x_lat)
    ax.set_xticklabels(lat_labels)
    ax.set_title("GC 延迟 & 总运行时间")
    ax.legend(fontsize=8)
    ax.grid(True, axis="y", alpha=0.25)
    for bar_group in [bars_l, bars_r]:
        for bar in bar_group:
            h = bar.get_height()
            if h > 0:
                ax.text(bar.get_x() + bar.get_width() / 2, h, f"{h:.1f}",
                        ha="center", va="bottom", fontsize=7)

    fig.suptitle(f"{left.label} vs {right.label} — 关键指标对比", fontsize=13, y=1.01)
    plt.tight_layout()
    fname = "compare_summary.png"
    plt.savefig(output_dir / fname, dpi=160, bbox_inches="tight")
    plt.close(fig)
    generated.append(fname)

    # ── 图3: GC 单次耗时对比 (散点/柱状) ──
    if left.gc_times_ms or right.gc_times_ms:
        fig, ax = plt.subplots(figsize=(12, 4.5))
        if left.gc_times_ms:
            ax.bar(range(1, len(left.gc_times_ms) + 1), left.gc_times_ms,
                   color=left_color, alpha=0.7, width=0.8, label=f"{left.label} ({left.gc_count} cycles)")
        if right.gc_times_ms:
            offset = len(left.gc_times_ms) + 3 if left.gc_times_ms else 0
            ax.bar(range(offset + 1, offset + len(right.gc_times_ms) + 1), right.gc_times_ms,
                   color=right_color, alpha=0.7, width=0.8, label=f"{right.label} ({right.gc_count} cycles)")
        if left.gc_times_ms:
            ax.axhline(left.gc_p95_ms, color=left_color, linestyle="--", linewidth=1, alpha=0.6,
                       label=f"{left.label} P95={left.gc_p95_ms:.1f}ms")
        if right.gc_times_ms:
            ax.axhline(right.gc_p95_ms, color=right_color, linestyle="--", linewidth=1, alpha=0.6,
                       label=f"{right.label} P95={right.gc_p95_ms:.1f}ms")
        ax.set_ylabel("GC Time (ms)")
        ax.set_xlabel("GC Cycle (ordinal)")
        ax.set_title(f"{left.label} vs {right.label} — 每次 GC 耗时")
        ax.legend(loc="upper right", fontsize=8)
        ax.grid(True, axis="y", alpha=0.25)
        plt.tight_layout()
        fname = "compare_gc_per_cycle.png"
        plt.savefig(output_dir / fname, dpi=160)
        plt.close(fig)
        generated.append(fname)

    return generated


# ─────────── 报告生成 ───────────

def build_compare_report(left: RunData, right: RunData, plot_files: list[str] | None = None) -> str:
    lines = [
        f"# {left.label} vs {right.label} 对比报告",
        "",
        f"- 左侧 ({left.label}): `{left.dir.name}` (gc_mode={left.gc_mode})",
        f"- 右侧 ({right.label}): `{right.dir.name}` (gc_mode={right.gc_mode})",
        f"- 用例: {left.metadata.get('case', 'unknown')}",
        f"- 堆大小: {left.metadata.get('heap_size', 'unknown')}",
        "",
    ]

    # 插入图表引用
    if plot_files:
        lines.extend([
            "## 对比图表",
            "",
        ])
        chart_titles = {
            "compare_rss_cpu_timeline.png": "RSS & CPU 时间序列对比",
            "compare_summary.png": "关键指标汇总对比",
            "compare_gc_per_cycle.png": "GC 单次耗时对比",
        }
        for f in plot_files:
            title = chart_titles.get(f, f)
            lines.append(f"### {title}")
            lines.append("")
            lines.append(f"![{title}]({f})")
            lines.append("")

    lines.extend([
        "## 运行时间",
        "",
        f"| 指标 | {left.label} | {right.label} | 变化 |",
        "| --- | ---: | ---: | ---: |",
    ])

    lw = left.wall_ms or 0
    rw = right.wall_ms or 0
    lines.append(f"| 总运行时间 (ms) | {lw} | {rw} | {delta_str(lw, rw)} |")

    lines.extend([
        "",
        "## RSS 内存",
        "",
        f"| 指标 | {left.label} | {right.label} | 变化 |",
        "| --- | ---: | ---: | ---: |",
    ])

    for key, label_text in [
        ("rss_avg_mb", "RSS 平均 (MB)"),
        ("rss_peak_mb", "RSS 峰值 (MB)"),
        ("rss_p95_mb", "RSS P95 (MB)"),
    ]:
        lv = left.rss.get(key, 0)
        rv = right.rss.get(key, 0)
        lines.append(f"| {label_text} | {fmt(lv)} | {fmt(rv)} | {delta_str(lv, rv)} |")

    lines.extend([
        "",
        "## CPU 使用率",
        "",
        f"| 指标 | {left.label} | {right.label} | 变化 |",
        "| --- | ---: | ---: | ---: |",
    ])

    for key, label_text in [
        ("cpu_avg_pct", "CPU 平均 (%)"),
        ("cpu_peak_pct", "CPU 峰值 (%)"),
        ("cpu_p95_pct", "CPU P95 (%)"),
    ]:
        lv = left.rss.get(key, 0)
        rv = right.rss.get(key, 0)
        lines.append(f"| {label_text} | {fmt(lv)} | {fmt(rv)} | {delta_str(lv, rv)} |")

    lines.extend([
        "",
        "## GC 性能",
        "",
        f"| 指标 | {left.label} | {right.label} | 变化 |",
        "| --- | ---: | ---: | ---: |",
        f"| GC 次数 | {left.gc_count} | {right.gc_count} | {delta_str(left.gc_count, right.gc_count)} |",
        f"| GC 总时间 (ms) | {fmt(left.gc_total_ms)} | {fmt(right.gc_total_ms)} | {delta_str(left.gc_total_ms, right.gc_total_ms)} |",
        f"| GC 平均时间 (ms) | {fmt(left.gc_avg_ms)} | {fmt(right.gc_avg_ms)} | {delta_str(left.gc_avg_ms, right.gc_avg_ms)} |",
        f"| GC 最大时间 (ms) | {fmt(left.gc_max_ms)} | {fmt(right.gc_max_ms)} | {delta_str(left.gc_max_ms, right.gc_max_ms)} |",
        f"| GC P95 时间 (ms) | {fmt(left.gc_p95_ms)} | {fmt(right.gc_p95_ms)} | {delta_str(left.gc_p95_ms, right.gc_p95_ms)} |",
    ])

    # GC 时间分布直方图（文本版）
    lines.extend([
        "",
        "## GC 时间分布",
        "",
    ])
    for run in [left, right]:
        if not run.gc_times_ms:
            continue
        lines.append(f"### {run.label}")
        lines.append("")
        buckets = [0, 1, 2, 5, 10, 20, 50, 100, 200, 500, 1000, float("inf")]
        bucket_labels = [
            "< 1ms", "1-2ms", "2-5ms", "5-10ms", "10-20ms",
            "20-50ms", "50-100ms", "100-200ms", "200-500ms", "500ms-1s", "> 1s"
        ]
        counts = [0] * (len(buckets) - 1)
        for t in run.gc_times_ms:
            for i in range(len(buckets) - 1):
                if t < buckets[i + 1]:
                    counts[i] += 1
                    break

        lines.append("| 区间 | 次数 | 占比 |")
        lines.append("| --- | ---: | ---: |")
        for label_text, count in zip(bucket_labels, counts):
            if count > 0:
                pct = count / len(run.gc_times_ms) * 100
                lines.append(f"| {label_text} | {count} | {pct:.1f}% |")
        lines.append("")

    lines.append("")
    return "\n".join(lines) + "\n"


# ─────────── main ───────────

def main() -> None:
    parser = argparse.ArgumentParser(description="对比两次运行结果")
    parser.add_argument("--left", required=True, help="左侧结果目录路径")
    parser.add_argument("--right", required=True, help="右侧结果目录路径")
    parser.add_argument("--left-label", default="Left", help="左侧标签 (默认: Left)")
    parser.add_argument("--right-label", default="Right", help="右侧标签 (默认: Right)")
    parser.add_argument("--output", default=None, help="输出报告路径 (默认: 左侧目录的上级/compare_report.md)")
    args = parser.parse_args()

    left_dir = Path(args.left).resolve()
    right_dir = Path(args.right).resolve()

    if not left_dir.exists():
        raise SystemExit(f"左侧目录不存在: {left_dir}")
    if not right_dir.exists():
        raise SystemExit(f"右侧目录不存在: {right_dir}")

    left = RunData(left_dir, args.left_label)
    right = RunData(right_dir, args.right_label)

    print(f"左侧 ({left.label}): {left.gc_mode}, {left.gc_count} cycles")
    print(f"右侧 ({right.label}): {right.gc_mode}, {right.gc_count} cycles")

    if args.output:
        output_path = Path(args.output).resolve()
    else:
        output_path = left_dir.parent / "compare_report.md"

    output_dir = output_path.parent
    output_dir.mkdir(parents=True, exist_ok=True)

    plot_files = generate_compare_plots(left, right, output_dir)
    print(f"生成了 {len(plot_files)} 张对比图表")

    report = build_compare_report(left, right, plot_files)
    output_path.write_text(report)
    print(f"对比报告已生成: {output_path}")


if __name__ == "__main__":
    main()
