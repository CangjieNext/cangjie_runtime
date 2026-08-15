#!/usr/bin/env python3
"""
compare_results.py — 对比两次或多次运行结果。

兼容两种 CLI 形式：

1) 旧形式（两侧）：
    python3 compare_results.py \
        --left  <copygc_result_dir> \
        --right <msgc_result_dir> \
        --left-label CopyGC \
        --right-label MSGC \
        --output compare_report.md

2) 新形式（多侧）：
    python3 compare_results.py \
        --run CopyGC=/path/to/copygc \
        --run MSGC=/path/to/msgc \
        --run Go=/path/to/go \
        --run Java=/path/to/java \
        --output compare_report.md
"""

from __future__ import annotations

import argparse
import csv
import math
import re
from datetime import datetime, timedelta
from pathlib import Path
from typing import Optional

# === Regex: MSGC ===
MSGC_START_RE = re.compile(
    r"^(?P<ts>\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}\.\d{6}).*?\[MS-GC\] === GC (?:START|begin:)\s+cycle=(?P<cycle>\d+)\b"
)
MSGC_TOTAL_RE = re.compile(r"(?:total gc time:\s*|GC total=)([\d,]+)\s*us\b")

# === Regex: CopyGC ===
COPYGC_START_RE = re.compile(
    r"^(?P<ts>\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}\.\d{6}).*?\[GC\] Start (?P<collector>.+?)\s+(?P<reason_name>\S+)\s+gcIndex=\s*(?P<cycle>\d+)"
)
COPYGC_TOTAL_RE = re.compile(r"total gc time:\s*([\d,]+)\s*us")

# === Regex: Go ===
GO_GC_RE = re.compile(r"^gc\s+(?P<cycle>\d+)\s+@.*?:\s+(?P<clock>[0-9.+]+)\s+ms clock")

# === Regex: Java ===
JAVA8_GC_RE = re.compile(
    r",\s*(?P<seconds>[0-9]+(?:\.[0-9]+)?)\s+secs\]\s*(?:\[Times:|$)"
)
JAVA_UNIFIED_GC_RE = re.compile(
    r"\bPause\b.*?\s(?P<duration>[0-9]+(?:\.[0-9]+)?)(?P<unit>ms|s)\s*$",
    re.MULTILINE,
)

# === Regex: 通用 ===
BENCH_TOTAL_RE = re.compile(r"^\[BENCH\]\s+Total wall time:\s+(\d+) ms$", re.MULTILINE)
MONITOR_TOTAL_RE = re.compile(r"^Total time:\s+(\d+)ms$", re.MULTILINE)

# === Regex: 吞吐 (case25 等 HTTP benchmark 输出 [RESULT] 行) ===
RESULT_DUR_RE = re.compile(r"^\[RESULT\]\s+payload=\S+\s+concurrency=\d+\s+duration=(\d+)ms", re.MULTILINE)
RESULT_TOTAL_RE = re.compile(
    r"^\[RESULT\]\s+total=(\d+)\s+ok=(\d+)\s+fail=(\d+)(?:\s+inflight=\d+)?\s+throughput=(\d+)req/s",
    re.MULTILINE,
)
RESULT_BYTES_RE = re.compile(r"^\[RESULT\]\s+bytes_sent=(\d+)MB\s+bytes_recv=(\d+)MB", re.MULTILINE)


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
    if GO_GC_RE.search(text):
        return "go"
    if (
        "Java HotSpot" in text
        or JAVA8_GC_RE.search(text)
        or JAVA_UNIFIED_GC_RE.search(text)
    ):
        return "java"
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

    elif gc_mode == "go":
        times_ms: list[float] = []
        for line in gc_log_path.read_text(errors="replace").splitlines():
            match = GO_GC_RE.match(line.strip())
            if not match:
                continue
            clock_parts = [part for part in match.group("clock").split("+") if part]
            try:
                cycle_ms = sum(float(part) for part in clock_parts)
            except ValueError:
                continue
            times_ms.append(cycle_ms)
        return times_ms

    elif gc_mode == "java":
        times_ms: list[float] = []
        for line in gc_log_path.read_text(errors="replace").splitlines():
            java8_match = JAVA8_GC_RE.search(line)
            if java8_match:
                times_ms.append(round(float(java8_match.group("seconds")) * 1000.0, 7))
                continue

            unified_match = JAVA_UNIFIED_GC_RE.search(line)
            if not unified_match:
                continue
            duration = float(unified_match.group("duration"))
            if unified_match.group("unit") == "s":
                duration *= 1000.0
            times_ms.append(duration)
        return times_ms

    return [t / 1000.0 for t in times_us]


def normalize_absolute_samples(
    samples: list[tuple[datetime, float, float]],
    duration_ms: int | None,
) -> list[tuple[float, float, float]]:
    if not samples:
        return []

    if duration_ms is None or duration_ms <= 0:
        start = samples[0][0]
    else:
        start = samples[-1][0] - timedelta(milliseconds=duration_ms)

    normalized: list[tuple[float, float, float]] = []
    for ts, rss_mb, cpu_pct in samples:
        elapsed_s = (ts - start).total_seconds()
        if elapsed_s < 0:
            continue
        normalized.append((elapsed_s, rss_mb, cpu_pct))
    return normalized


def parse_rss_csv(path: Path, duration_ms: int | None = None) -> dict:
    elapsed_s: list[float] = []
    rss_mb: list[float] = []
    cpu_pct: list[float] = []

    if not path.exists():
        return {}

    with path.open() as f:
        first = f.readline()
        f.seek(0)
        # Schema A: header "elapsed_ms,rss_mb,cpu_pct" (rss_monitor.py)
        # Schema B: headerless "ts_iso,rss_kb,cpu_pct" (bare-run sampler)
        if "elapsed_ms" in first:
            reader = csv.DictReader(f)
            for row in reader:
                elapsed_s.append(float(row["elapsed_ms"]) / 1000.0)
                rss_mb.append(float(row["rss_mb"]))
                cpu_pct.append(float(row["cpu_pct"]))
        else:
            reader = csv.reader(f)
            absolute_samples: list[tuple[datetime, float, float]] = []
            for row in reader:
                if len(row) < 3:
                    continue
                try:
                    t = datetime.fromisoformat(row[0])
                    rss_kb = float(row[1])
                    cpu = float(row[2])
                except (ValueError, IndexError):
                    continue
                absolute_samples.append((t, rss_kb / 1024.0, cpu))
            normalized = normalize_absolute_samples(absolute_samples, duration_ms)
            for elapsed, rss, cpu in normalized:
                elapsed_s.append(elapsed)
                rss_mb.append(rss)
                cpu_pct.append(cpu)

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


def parse_bench_result(results_dir: Path) -> dict:
    """解析 HTTP benchmark 的 [RESULT] 行（throughput, total, ok, fail, duration, bytes）"""
    out: dict = {}
    for name in ("stdout.txt", "benchmark.stdout.txt"):
        path = results_dir / name
        if not path.exists():
            continue
        text = path.read_text(errors="replace")
        m = RESULT_DUR_RE.search(text)
        if m:
            out["duration_ms"] = int(m.group(1))
        m = RESULT_TOTAL_RE.search(text)
        if m:
            out["total"] = int(m.group(1))
            out["ok"] = int(m.group(2))
            out["fail"] = int(m.group(3))
            out["throughput"] = int(m.group(4))
        m = RESULT_BYTES_RE.search(text)
        if m:
            out["bytes_sent_mb"] = int(m.group(1))
            out["bytes_recv_mb"] = int(m.group(2))
        if out:
            break
    return out


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
        self.bench = parse_bench_result(results_dir)
        self.wall_ms = parse_wall_time(results_dir)
        if self.wall_ms is None:
            self.wall_ms = self.bench.get("duration_ms")
        self.rss = parse_rss_csv(results_dir / "rss.csv", duration_ms=self.wall_ms)

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

def report_title(runs: list["RunData"]) -> str:
    return " vs ".join(run.label for run in runs)


def chart_color_palette(count: int) -> list[str]:
    base = [
        "#1f77b4",
        "#d62728",
        "#2ca02c",
        "#ff7f0e",
        "#9467bd",
        "#8c564b",
    ]
    if count <= len(base):
        return base[:count]
    colors: list[str] = []
    for i in range(count):
        colors.append(base[i % len(base)])
    return colors


def annotate_bars(ax, bars, integer: bool) -> None:
    for bar in bars:
        height = bar.get_height()
        if height <= 0:
            continue
        label = f"{int(height)}" if integer else f"{height:.1f}"
        ax.text(
            bar.get_x() + bar.get_width() / 2,
            height,
            label,
            ha="center",
            va="bottom",
            fontsize=7,
        )


def grouped_bars(ax, runs: list["RunData"], categories: list[str], values_by_run: list[list[float]],
                 colors: list[str], ylabel: str, title: str, integer: bool = False) -> None:
    bar_span = 0.8
    run_count = max(1, len(runs))
    bar_width = bar_span / run_count
    x_positions = list(range(len(categories)))

    for idx, run in enumerate(runs):
        offset = (idx - (run_count - 1) / 2) * bar_width
        values = values_by_run[idx]
        bars = ax.bar(
            [x + offset for x in x_positions],
            values,
            bar_width,
            label=run.label,
            color=colors[idx],
            alpha=0.85,
        )
        annotate_bars(ax, bars, integer)

    ax.set_xticks(x_positions)
    ax.set_xticklabels(categories)
    ax.set_ylabel(ylabel)
    ax.set_title(title)
    ax.legend(fontsize=8)
    ax.grid(True, axis="y", alpha=0.25)


def generate_compare_plots(runs: list["RunData"], output_dir: Path) -> list[str]:
    """生成对比图表，返回生成的文件名列表"""
    try:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.font_manager as fm
        import matplotlib.pyplot as plt
    except ImportError:
        print("警告: matplotlib 不可用，跳过图表生成")
        return []

    zh_fonts = ["PingFang SC", "STHeiti", "Arial Unicode MS", "Heiti SC", "SimHei"]
    found_font = None
    for name in zh_fonts:
        for font in fm.fontManager.ttflist:
            if name in font.name:
                found_font = name
                break
        if found_font:
            break
    if found_font:
        plt.rcParams["font.sans-serif"] = [found_font, "DejaVu Sans"]
    else:
        plt.rcParams["font.sans-serif"] = ["DejaVu Sans"]
    plt.rcParams["axes.unicode_minus"] = False

    title = report_title(runs)
    colors = chart_color_palette(len(runs))
    generated: list[str] = []

    if any(run.rss.get("elapsed_s") and run.rss.get("rss_mb") for run in runs):
        fig, (ax_rss, ax_cpu) = plt.subplots(2, 1, figsize=(14, 8), sharex=False)
        for idx, run in enumerate(runs):
            elapsed = run.rss.get("elapsed_s")
            rss_mb = run.rss.get("rss_mb")
            cpu_pct = run.rss.get("cpu_pct")
            if not elapsed or not rss_mb or not cpu_pct:
                continue
            ax_rss.plot(elapsed, rss_mb, color=colors[idx], linewidth=1.4, label=run.label, alpha=0.85)
            ax_rss.fill_between(elapsed, 0, rss_mb, color=colors[idx], alpha=0.10)
            ax_cpu.plot(elapsed, cpu_pct, color=colors[idx], linewidth=1.2, label=run.label, alpha=0.85)

        ax_rss.set_ylabel("RSS (MB)")
        ax_rss.set_title(f"{title} — RSS & CPU 时间序列")
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

    fig, axes = plt.subplots(2, 2, figsize=(12, 8))
    grouped_bars(
        axes[0][0],
        runs,
        ["Avg", "Peak", "P95"],
        [[run.rss.get("rss_avg_mb", 0), run.rss.get("rss_peak_mb", 0), run.rss.get("rss_p95_mb", 0)] for run in runs],
        colors,
        "MB",
        "RSS 内存",
    )
    grouped_bars(
        axes[0][1],
        runs,
        ["Avg", "Peak", "P95"],
        [[run.rss.get("cpu_avg_pct", 0), run.rss.get("cpu_peak_pct", 0), run.rss.get("cpu_p95_pct", 0)] for run in runs],
        colors,
        "%",
        "CPU 使用率",
    )
    grouped_bars(
        axes[1][0],
        runs,
        ["GC Count", "Total (ms)", "Avg (ms)"],
        [[run.gc_count, run.gc_total_ms, run.gc_avg_ms] for run in runs],
        colors,
        "",
        "GC 次数与时间",
    )
    grouped_bars(
        axes[1][1],
        runs,
        ["Max (ms)", "P95 (ms)", "Wall (s)"],
        [[run.gc_max_ms, run.gc_p95_ms, (run.wall_ms or 0) / 1000.0] for run in runs],
        colors,
        "",
        "GC 延迟 & 总运行时间",
    )
    fig.suptitle(f"{title} — 关键指标对比", fontsize=13, y=1.01)
    plt.tight_layout()
    fname = "compare_summary.png"
    plt.savefig(output_dir / fname, dpi=160, bbox_inches="tight")
    plt.close(fig)
    generated.append(fname)

    if any(run.bench for run in runs):
        fig, axes = plt.subplots(1, 3, figsize=(14, 4.5))
        throughput_vals = [run.bench.get("throughput", 0) for run in runs]
        bars = axes[0].bar([run.label for run in runs], throughput_vals, color=colors, alpha=0.85)
        axes[0].set_ylabel("req/s")
        axes[0].set_title("吞吐率")
        axes[0].grid(True, axis="y", alpha=0.25)
        annotate_bars(axes[0], bars, integer=True)

        grouped_bars(
            axes[1],
            runs,
            ["Total", "OK", "Fail"],
            [[run.bench.get("total", 0), run.bench.get("ok", 0), run.bench.get("fail", 0)] for run in runs],
            colors,
            "",
            "请求总数",
            integer=True,
        )
        grouped_bars(
            axes[2],
            runs,
            ["Sent", "Recv"],
            [[run.bench.get("bytes_sent_mb", 0), run.bench.get("bytes_recv_mb", 0)] for run in runs],
            colors,
            "MB",
            "传输字节数",
            integer=True,
        )

        fig.suptitle(f"{title} — 吞吐对比", fontsize=13, y=1.02)
        plt.tight_layout()
        fname = "compare_throughput.png"
        plt.savefig(output_dir / fname, dpi=160, bbox_inches="tight")
        plt.close(fig)
        generated.append(fname)

    if any(run.gc_times_ms for run in runs):
        fig, ax = plt.subplots(figsize=(12, 4.5))
        for idx, run in enumerate(runs):
            if not run.gc_times_ms:
                continue
            x_values = list(range(1, len(run.gc_times_ms) + 1))
            ax.plot(
                x_values,
                run.gc_times_ms,
                color=colors[idx],
                marker="o",
                markersize=2.5,
                linewidth=1.2,
                alpha=0.85,
                label=f"{run.label} ({run.gc_count} cycles)",
            )
            ax.axhline(
                run.gc_p95_ms,
                color=colors[idx],
                linestyle="--",
                linewidth=1,
                alpha=0.6,
                label=f"{run.label} P95={run.gc_p95_ms:.1f}ms",
            )
        ax.set_ylabel("GC Time (ms)")
        ax.set_xlabel("GC Cycle (ordinal)")
        ax.set_title(f"{title} — 每次 GC 耗时")
        ax.legend(loc="upper right", fontsize=8)
        ax.grid(True, axis="y", alpha=0.25)
        plt.tight_layout()
        fname = "compare_gc_per_cycle.png"
        plt.savefig(output_dir / fname, dpi=160)
        plt.close(fig)
        generated.append(fname)

    return generated


# ─────────── 报告生成 ───────────

def format_table_value(value: float | int, integer: bool) -> str:
    if integer:
        return str(int(value))
    return fmt(float(value))


def append_metric_table(lines: list[str], title: str, runs: list["RunData"],
                        rows: list[tuple[str, list[float | int], bool]]) -> None:
    lines.extend([f"## {title}", ""])
    headers = ["指标"] + [run.label for run in runs]
    if len(runs) == 2:
        headers.append("变化")
    lines.append("| " + " | ".join(headers) + " |")
    lines.append("| " + " | ".join(["---"] + ["---:" for _ in headers[1:]]) + " |")
    for label_text, values, integer in rows:
        cells = [label_text] + [format_table_value(value, integer) for value in values]
        if len(runs) == 2:
            cells.append(delta_str(float(values[0]), float(values[1])))
        lines.append("| " + " | ".join(cells) + " |")
    lines.append("")


def build_compare_report(runs: list["RunData"], plot_files: list[str] | None = None) -> str:
    title = report_title(runs)
    lines = [f"# {title} 对比报告", ""]
    for index, run in enumerate(runs, start=1):
        lines.append(f"- 侧 {index} ({run.label}): `{run.dir.name}` (gc_mode={run.gc_mode})")
    lines.extend([
        f"- 用例: {runs[0].metadata.get('case', 'unknown')}",
        f"- 堆大小: {runs[0].metadata.get('heap_size', 'unknown')}",
        "",
    ])

    if plot_files:
        lines.extend(["## 对比图表", ""])
        chart_titles = {
            "compare_rss_cpu_timeline.png": "RSS & CPU 时间序列对比",
            "compare_summary.png": "关键指标汇总对比",
            "compare_throughput.png": "吞吐对比",
            "compare_gc_per_cycle.png": "GC 单次耗时对比",
        }
        for filename in plot_files:
            lines.append(f"### {chart_titles.get(filename, filename)}")
            lines.append("")
            lines.append(f"![{chart_titles.get(filename, filename)}]({filename})")
            lines.append("")

    append_metric_table(
        lines,
        "运行时间",
        runs,
        [("总运行时间 (ms)", [run.wall_ms or 0 for run in runs], True)],
    )

    if any(run.bench for run in runs):
        append_metric_table(
            lines,
            "吞吐与请求统计",
            runs,
            [
                ("吞吐 (req/s)", [run.bench.get("throughput", 0) for run in runs], True),
                ("总请求数", [run.bench.get("total", 0) for run in runs], True),
                ("成功数", [run.bench.get("ok", 0) for run in runs], True),
                ("失败数", [run.bench.get("fail", 0) for run in runs], True),
                ("实际运行 (ms)", [run.bench.get("duration_ms", 0) for run in runs], True),
                ("发送字节 (MB)", [run.bench.get("bytes_sent_mb", 0) for run in runs], True),
                ("接收字节 (MB)", [run.bench.get("bytes_recv_mb", 0) for run in runs], True),
            ],
        )

    append_metric_table(
        lines,
        "RSS 内存",
        runs,
        [
            ("RSS 平均 (MB)", [run.rss.get("rss_avg_mb", 0) for run in runs], False),
            ("RSS 峰值 (MB)", [run.rss.get("rss_peak_mb", 0) for run in runs], False),
            ("RSS P95 (MB)", [run.rss.get("rss_p95_mb", 0) for run in runs], False),
        ],
    )

    append_metric_table(
        lines,
        "CPU 使用率",
        runs,
        [
            ("CPU 平均 (%)", [run.rss.get("cpu_avg_pct", 0) for run in runs], False),
            ("CPU 峰值 (%)", [run.rss.get("cpu_peak_pct", 0) for run in runs], False),
            ("CPU P95 (%)", [run.rss.get("cpu_p95_pct", 0) for run in runs], False),
        ],
    )

    append_metric_table(
        lines,
        "GC 性能",
        runs,
        [
            ("GC 次数", [run.gc_count for run in runs], True),
            ("GC 总时间 (ms)", [run.gc_total_ms for run in runs], False),
            ("GC 平均时间 (ms)", [run.gc_avg_ms for run in runs], False),
            ("GC 最大时间 (ms)", [run.gc_max_ms for run in runs], False),
            ("GC P95 时间 (ms)", [run.gc_p95_ms for run in runs], False),
        ],
    )

    lines.extend(["## GC 时间分布", ""])
    for run in runs:
        if not run.gc_times_ms:
            continue
        lines.append(f"### {run.label}")
        lines.append("")
        buckets = [0, 1, 2, 5, 10, 20, 50, 100, 200, 500, 1000, float("inf")]
        bucket_labels = [
            "< 1ms",
            "1-2ms",
            "2-5ms",
            "5-10ms",
            "10-20ms",
            "20-50ms",
            "50-100ms",
            "100-200ms",
            "200-500ms",
            "500ms-1s",
            "> 1s",
        ]
        counts = [0] * (len(buckets) - 1)
        for gc_time in run.gc_times_ms:
            for idx in range(len(buckets) - 1):
                if gc_time < buckets[idx + 1]:
                    counts[idx] += 1
                    break

        lines.append("| 区间 | 次数 | 占比 |")
        lines.append("| --- | ---: | ---: |")
        for label_text, count in zip(bucket_labels, counts):
            if count <= 0:
                continue
            pct = count / len(run.gc_times_ms) * 100
            lines.append(f"| {label_text} | {count} | {pct:.1f}% |")
        lines.append("")

    lines.append("")
    return "\n".join(lines) + "\n"


# ─────────── main ───────────

def parse_run_specs(args: argparse.Namespace) -> list[tuple[Path, str]]:
    specs: list[tuple[Path, str]] = []
    if args.run:
        for item in args.run:
            if "=" not in item:
                raise SystemExit(f"--run 参数必须形如 Label=/path/to/dir: {item}")
            label, raw_path = item.split("=", 1)
            specs.append((Path(raw_path).resolve(), label))
    else:
        if not args.left or not args.right:
            raise SystemExit("旧 CLI 形式要求同时提供 --left 和 --right，或改用 --run")
        specs.append((Path(args.left).resolve(), args.left_label))
        specs.append((Path(args.right).resolve(), args.right_label))

    if len(specs) < 2:
        raise SystemExit("至少需要两组运行结果做对比")
    for path, _ in specs:
        if not path.exists():
            raise SystemExit(f"结果目录不存在: {path}")
    return specs


def main() -> None:
    parser = argparse.ArgumentParser(description="对比两次或多次运行结果")
    parser.add_argument("--run", action="append", default=[], help="对比项，格式为 Label=/path/to/result，可重复")
    parser.add_argument("--left", default=None, help="左侧结果目录路径（兼容旧 CLI）")
    parser.add_argument("--right", default=None, help="右侧结果目录路径（兼容旧 CLI）")
    parser.add_argument("--left-label", default="Left", help="左侧标签 (默认: Left)")
    parser.add_argument("--right-label", default="Right", help="右侧标签 (默认: Right)")
    parser.add_argument("--output", default=None, help="输出报告路径 (默认: 第一组结果目录的上级/compare_report.md)")
    args = parser.parse_args()

    run_specs = parse_run_specs(args)
    runs = [RunData(results_dir, label) for results_dir, label in run_specs]

    for run in runs:
        print(f"{run.label}: {run.gc_mode}, {run.gc_count} cycles")

    if args.output:
        output_path = Path(args.output).resolve()
    else:
        output_path = run_specs[0][0].parent / "compare_report.md"

    output_dir = output_path.parent
    output_dir.mkdir(parents=True, exist_ok=True)

    plot_files = generate_compare_plots(runs, output_dir)
    print(f"生成了 {len(plot_files)} 张对比图表")

    report = build_compare_report(runs, plot_files)
    output_path.write_text(report)
    print(f"对比报告已生成: {output_path}")


if __name__ == "__main__":
    main()
