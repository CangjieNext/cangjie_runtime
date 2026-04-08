#!/usr/bin/env python3
"""
analyze_case.py — 分析单次运行的 GC 日志 + RSS/CPU 采样数据。

用法：
    python3 analyze_case.py --results-dir <path>

输入文件（在 results-dir 中）：
    gc.log          GC 运行日志（MRT_REPORT 输出）
    rss.csv         RSS/CPU 采样数据（rss_monitor.py 输出）
    stdout.txt      benchmark 标准输出（可选）
    monitor.txt     rss_monitor 日志（可选）
    run_metadata.env 运行元数据（可选）

输出文件：
    gc_cycles.csv   GC 周期表
    report.md       分析报告
"""

from __future__ import annotations

import argparse
import csv
import math
import re
from collections import Counter
from datetime import datetime
from pathlib import Path

# === Regex: MSGC ===
MSGC_START_RE = re.compile(
    r"^(?P<ts>\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}\.\d{6}).*?\[MS-GC\] === GC START cycle=(?P<cycle>\d+) === .*?reason=(?P<reason_code>\d+)\((?P<reason_name>[^)]+)\)"
)
MSGC_TOTAL_RE = re.compile(r"total gc time:\s*([\d,]+)\s*us\s*(?:\(([^)]*)\))?")
MSGC_TRUE_LIVE_RE = re.compile(r"active-region bytes:\s*([\d,]+)\s*B,\s*true live:\s*([\d,]+)\s*B")
MSGC_RUNTIME_RSS_RE = re.compile(
    r"runtime-rss:\s*empty-cache bytes:\s*([\d,]+)\s*B\s*\(([\d,]+)\s*regions\),\s*"
    r"region-info count:\s*([\d,]+),\s*region-info bytes:\s*([\d,]+)\s*B,\s*"
    r"region-bitmap bytes:\s*([\d,]+)\s*B,\s*releasedTree meta bytes:\s*([\d,]+)\s*B,\s*"
    r"collector queue bytes:\s*([\d,]+)\s*B,\s*wb buffer bytes:\s*([\d,]+)\s*B,\s*"
    r"runtime meta bytes:\s*([\d,]+)\s*B"
)
MSGC_REGION_ACCOUNTING_RE = re.compile(
    r"region-accounting:\s*active=([\d,]+)\s*regions,\s*releasedTree=([\d,]+)\s*regions,\s*"
    r"virgin=([\d,]+)\s*regions,\s*committed=([\d,]+)\s*regions"
)

# === Regex: CopyGC ===
COPYGC_START_RE = re.compile(
    r"^(?P<ts>\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}\.\d{6}).*?\[GC\] Start (?P<collector>.+?)\s+(?P<reason_name>\S+)\s+gcIndex=\s*(?P<cycle>\d+)"
)
COPYGC_TOTAL_RE = re.compile(r"total gc time:\s*([\d,]+)\s*us")

# === Regex: 通用 ===
LOG_TS_RE = re.compile(r"^(\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}\.\d{6})")
BENCH_TOTAL_RE = re.compile(r"^\[BENCH\]\s+Total wall time:\s+(\d+) ms$", re.MULTILINE)
BENCH_ROW_RE = re.compile(r"^\[BENCH\]\s+([A-Za-z0-9_]+)\s+(\d+) ms(?:\s|$)", re.MULTILINE)
MONITOR_TOTAL_RE = re.compile(r"^Total time:\s+(\d+)ms$", re.MULTILINE)

STAGE_ORDER = ["PreEnum", "EnumRoots", "ConcMark", "MarkTerm", "PostMark", "ConcSweep", "PostGC", "Other"]


# ─────────── 工具函数 ───────────

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


def cint(s: str) -> int:
    return int(s.replace(",", ""))


# ─────────── 元数据解析 ───────────

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


# ─────────── GC 模式检测 ───────────

def detect_gc_mode(gc_log_path: Path) -> str:
    """自动检测 GC 模式：msgc 或 copygc"""
    text = gc_log_path.read_text(errors="replace")
    if "[MS-GC]" in text:
        return "msgc"
    if "[GC] Start" in text:
        return "copygc"
    return "unknown"


# ─────────── MSGC 解析 ───────────

def parse_stage_breakdown(text: str) -> dict[str, int]:
    breakdown: dict[str, int] = {stage: 0 for stage in STAGE_ORDER}
    if not text:
        return breakdown
    for piece in text.split("+"):
        piece = piece.strip()
        if not piece:
            continue
        parts = piece.rsplit(" ", 1)
        if len(parts) == 2:
            breakdown[parts[0].strip()] = cint(parts[1])
    return breakdown


def parse_msgc_cycles(path: Path) -> list[dict]:
    cycles: list[dict] = []
    current: dict | None = None

    for line in path.read_text(errors="replace").splitlines():
        start_match = MSGC_START_RE.match(line)
        if start_match:
            if current is not None:
                cycles.append(current)
            current = {
                "cycle": int(start_match.group("cycle")),
                "start_ts": datetime.strptime(start_match.group("ts"), "%Y-%m-%d %H:%M:%S.%f"),
                "reason_code": int(start_match.group("reason_code")),
                "reason_name": start_match.group("reason_name"),
                "total_gc_us": 0,
                "active_region_bytes": None,
                "true_live_bytes": None,
                "empty_cache_bytes": None,
                "region_info_count": None,
                "active_region_count": None,
                "released_tree_region_count": None,
                "virgin_region_count": None,
                "committed_region_count": None,
                "region_info_bytes": None,
                "region_bitmap_bytes": None,
                "released_tree_meta_bytes": None,
                "collector_queue_bytes": None,
                "wb_buffer_bytes": None,
                "runtime_meta_bytes": None,
                **{stage: 0 for stage in STAGE_ORDER},
            }
            continue
        if current is None:
            continue

        m = MSGC_TRUE_LIVE_RE.search(line)
        if m:
            current["active_region_bytes"] = cint(m.group(1))
            current["true_live_bytes"] = cint(m.group(2))

        m = MSGC_RUNTIME_RSS_RE.search(line)
        if m:
            current["empty_cache_bytes"] = cint(m.group(1))
            current["region_info_count"] = cint(m.group(3))
            current["region_info_bytes"] = cint(m.group(4))
            current["region_bitmap_bytes"] = cint(m.group(5))
            current["released_tree_meta_bytes"] = cint(m.group(6))
            current["collector_queue_bytes"] = cint(m.group(7))
            current["wb_buffer_bytes"] = cint(m.group(8))
            current["runtime_meta_bytes"] = cint(m.group(9))

        m = MSGC_REGION_ACCOUNTING_RE.search(line)
        if m:
            current["active_region_count"] = cint(m.group(1))
            current["released_tree_region_count"] = cint(m.group(2))
            current["virgin_region_count"] = cint(m.group(3))
            current["committed_region_count"] = cint(m.group(4))

        m = MSGC_TOTAL_RE.search(line)
        if m:
            current["total_gc_us"] = cint(m.group(1))
            if m.group(2):
                current.update(parse_stage_breakdown(m.group(2)))

    if current is not None:
        cycles.append(current)
    return sorted(
        [c for c in cycles if c.get("total_gc_us", 0)],
        key=lambda c: c["start_ts"],
    )


# ─────────── CopyGC 解析 ───────────

def parse_copygc_cycles(path: Path) -> list[dict]:
    cycles: list[dict] = []
    current: dict | None = None

    for line in path.read_text(errors="replace").splitlines():
        start_match = COPYGC_START_RE.match(line)
        if start_match:
            if current is not None:
                cycles.append(current)
            current = {
                "cycle": int(start_match.group("cycle")),
                "start_ts": datetime.strptime(start_match.group("ts"), "%Y-%m-%d %H:%M:%S.%f"),
                "reason_code": 0,
                "reason_name": start_match.group("reason_name"),
                "total_gc_us": 0,
            }
            continue
        if current is None:
            continue
        m = COPYGC_TOTAL_RE.search(line)
        if m:
            current["total_gc_us"] = cint(m.group(1))

    if current is not None:
        cycles.append(current)
    return sorted(
        [c for c in cycles if c.get("total_gc_us", 0)],
        key=lambda c: c["start_ts"],
    )


# ─────────── RSS CSV 解析 ───────────

def parse_rss_csv(path: Path) -> dict:
    rss_mb: list[float] = []
    cpu_pct: list[float] = []

    with path.open() as f:
        reader = csv.DictReader(f)
        for row in reader:
            rss_mb.append(float(row["rss_mb"]))
            cpu_pct.append(float(row["cpu_pct"]))

    if not rss_mb:
        return {"rss_mb": [], "cpu_pct": [], "sample_count": 0}

    sorted_rss = sorted(rss_mb)
    sorted_cpu = sorted(cpu_pct)
    return {
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


# ─────────── 标准输出 / Monitor 解析 ───────────

def parse_wall_time(results_dir: Path) -> dict:
    result: dict = {"benchmark_wall_ms": None, "monitor_wall_ms": None, "phases": {}}

    stdout_path = results_dir / "stdout.txt"
    if stdout_path.exists():
        text = stdout_path.read_text(errors="replace")
        m = BENCH_TOTAL_RE.search(text)
        if m:
            result["benchmark_wall_ms"] = int(m.group(1))
        for name, value in BENCH_ROW_RE.findall(text):
            if name != "Total":
                result["phases"][name] = int(value)

    monitor_path = results_dir / "monitor.txt"
    if monitor_path.exists():
        text = monitor_path.read_text(errors="replace")
        m = MONITOR_TOTAL_RE.search(text)
        if m:
            result["monitor_wall_ms"] = int(m.group(1))
        if result["benchmark_wall_ms"] is None:
            m2 = BENCH_TOTAL_RE.search(text)
            if m2:
                result["benchmark_wall_ms"] = int(m2.group(1))

    return result


# ─────────── CSV 写出 ───────────

def write_msgc_cycles_csv(path: Path, cycles: list[dict]) -> None:
    fieldnames = [
        "cycle", "start_ts", "reason_code", "reason_name", "total_gc_ms",
        "active_region_mb", "true_live_mb", "empty_cache_mb",
        "active_region_count", "released_tree_region_count",
        "virgin_region_count", "committed_region_count",
        *[f"{s}_ms" for s in STAGE_ORDER],
    ]
    with path.open("w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=fieldnames)
        writer.writeheader()
        for c in cycles:
            row = {
                "cycle": c["cycle"],
                "start_ts": c["start_ts"].isoformat(sep=" ", timespec="microseconds"),
                "reason_code": c["reason_code"],
                "reason_name": c["reason_name"],
                "total_gc_ms": f"{c['total_gc_us'] / 1000.0:.3f}",
                "active_region_mb": f"{(c.get('active_region_bytes') or 0) / (1024 * 1024):.3f}",
                "true_live_mb": f"{(c.get('true_live_bytes') or 0) / (1024 * 1024):.3f}",
                "empty_cache_mb": f"{(c.get('empty_cache_bytes') or 0) / (1024 * 1024):.3f}",
                "active_region_count": c.get("active_region_count") or 0,
                "released_tree_region_count": c.get("released_tree_region_count") or 0,
                "virgin_region_count": c.get("virgin_region_count") or 0,
                "committed_region_count": c.get("committed_region_count") or 0,
            }
            for s in STAGE_ORDER:
                row[f"{s}_ms"] = f"{c.get(s, 0) / 1000.0:.3f}"
            writer.writerow(row)


def write_copygc_cycles_csv(path: Path, cycles: list[dict]) -> None:
    fieldnames = ["cycle", "start_ts", "reason_name", "total_gc_ms"]
    with path.open("w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=fieldnames)
        writer.writeheader()
        for c in cycles:
            writer.writerow({
                "cycle": c["cycle"],
                "start_ts": c["start_ts"].isoformat(sep=" ", timespec="microseconds"),
                "reason_name": c["reason_name"],
                "total_gc_ms": f"{c['total_gc_us'] / 1000.0:.3f}",
            })


# ─────────── 报告生成 ───────────

def build_report(
    results_dir: Path,
    metadata: dict[str, str],
    gc_mode: str,
    cycles: list[dict],
    rss_data: dict,
    wall: dict,
) -> str:
    total_ms_list = [c["total_gc_us"] / 1000.0 for c in cycles]
    total_gc_ms = sum(total_ms_list)
    reason_counter = Counter(c["reason_name"] for c in cycles)

    avg_interval_ms = 0.0
    if len(cycles) > 1:
        intervals = [
            (cycles[i]["start_ts"] - cycles[i - 1]["start_ts"]).total_seconds() * 1000.0
            for i in range(1, len(cycles))
        ]
        avg_interval_ms = sum(intervals) / len(intervals)

    mode_label = "MSGC" if gc_mode == "msgc" else "CopyGC"

    lines = [
        f"# {mode_label} Benchmark Report",
        "",
        f"- 用例: {metadata.get('case', 'unknown')}",
        f"- GC 模式: {metadata.get('gc_mode', gc_mode)}",
        f"- 堆大小: {metadata.get('heap_size', 'unknown')}",
        f"- 结果目录: `{results_dir.name}`",
        f"- 时间: {metadata.get('timestamp', 'unknown')}",
        "",
        "## 总览",
        "",
        "| 指标 | 值 |",
        "| --- | ---: |",
    ]

    if wall.get("benchmark_wall_ms") is not None:
        lines.append(f"| Benchmark 总时间 (ms) | {wall['benchmark_wall_ms']} |")
    if wall.get("monitor_wall_ms") is not None:
        lines.append(f"| Monitor 总时间 (ms) | {wall['monitor_wall_ms']} |")

    if rss_data.get("sample_count", 0) > 0:
        lines.extend([
            f"| RSS 平均 (MB) | {fmt(rss_data['rss_avg_mb'])} |",
            f"| RSS 峰值 (MB) | {fmt(rss_data['rss_peak_mb'])} |",
            f"| RSS P95 (MB) | {fmt(rss_data['rss_p95_mb'])} |",
            f"| CPU 平均 (%) | {fmt(rss_data['cpu_avg_pct'])} |",
            f"| CPU 峰值 (%) | {fmt(rss_data['cpu_peak_pct'])} |",
            f"| CPU P95 (%) | {fmt(rss_data['cpu_p95_pct'])} |",
            f"| 采样点数 | {rss_data['sample_count']} |",
        ])

    lines.extend([
        f"| GC 次数 | {len(cycles)} |",
        f"| GC 总时间 (ms) | {fmt(total_gc_ms)} |",
        f"| GC 平均时间 (ms) | {fmt(total_gc_ms / len(cycles) if cycles else 0)} |",
        f"| GC 最大时间 (ms) | {fmt(max(total_ms_list) if total_ms_list else 0)} |",
        f"| GC P95 时间 (ms) | {fmt(percentile(sorted(total_ms_list), 0.95))} |",
        f"| GC 平均间隔 (ms) | {fmt(avg_interval_ms)} |",
    ])

    # MSGC 特有指标
    if gc_mode == "msgc":
        true_live = [(c.get("true_live_bytes") or 0) / (1024 * 1024) for c in cycles if c.get("true_live_bytes") is not None]
        if true_live:
            lines.extend([
                f"| True live 平均 (MB) | {fmt(sum(true_live) / len(true_live))} |",
                f"| True live 峰值 (MB) | {fmt(max(true_live))} |",
            ])

    lines.extend([
        "",
        "## GC 原因分布",
        "",
        "| 原因 | 次数 |",
        "| --- | ---: |",
    ])
    for reason, count in sorted(reason_counter.items()):
        lines.append(f"| {reason} | {count} |")

    # Benchmark 阶段
    if wall.get("phases"):
        lines.extend([
            "",
            "## Benchmark 阶段",
            "",
            "| 阶段 | 时间 (ms) |",
            "| --- | ---: |",
        ])
        for name, value in sorted(wall["phases"].items()):
            lines.append(f"| {name} | {value} |")

    # MSGC 阶段分解
    if gc_mode == "msgc" and cycles and any(c.get("ConcMark", 0) for c in cycles):
        stage_totals = {s: sum(c.get(s, 0) for c in cycles) / 1000.0 for s in STAGE_ORDER}
        lines.extend([
            "",
            "## GC 阶段分解",
            "",
            "| 阶段 | 总时间 (ms) | 平均 (ms) | 占比 |",
            "| --- | ---: | ---: | ---: |",
        ])
        for s in STAGE_ORDER:
            avg = stage_totals[s] / len(cycles) if cycles else 0
            share = (stage_totals[s] / total_gc_ms * 100) if total_gc_ms else 0
            lines.append(f"| {s} | {fmt(stage_totals[s])} | {fmt(avg)} | {share:.1f}% |")

    # 最慢 GC 周期
    slowest = sorted(cycles, key=lambda c: c["total_gc_us"], reverse=True)[:10]
    if slowest:
        lines.extend([
            "",
            "## 最慢 GC 周期 (Top 10)",
            "",
            "| Cycle | 原因 | 总时间 (ms) |",
            "| --- | --- | ---: |",
        ])
        for c in slowest:
            lines.append(f"| {c['cycle']} | {c['reason_name']} | {c['total_gc_us'] / 1000.0:.3f} |")

    lines.append("")
    return "\n".join(lines) + "\n"


# ─────────── main ───────────

def main() -> None:
    parser = argparse.ArgumentParser(description="分析单次用例运行结果")
    parser.add_argument("--results-dir", required=True, help="包含 gc.log / rss.csv 的结果目录")
    args = parser.parse_args()

    results_dir = Path(args.results_dir).resolve()
    gc_log = results_dir / "gc.log"
    rss_csv = results_dir / "rss.csv"

    if not gc_log.exists():
        raise SystemExit(f"gc.log 不存在: {gc_log}")

    metadata = read_metadata(results_dir)
    gc_mode = detect_gc_mode(gc_log)
    print(f"检测到 GC 模式: {gc_mode}")

    # 解析 GC cycles
    if gc_mode == "msgc":
        cycles = parse_msgc_cycles(gc_log)
        write_msgc_cycles_csv(results_dir / "gc_cycles.csv", cycles)
    elif gc_mode == "copygc":
        cycles = parse_copygc_cycles(gc_log)
        write_copygc_cycles_csv(results_dir / "gc_cycles.csv", cycles)
    else:
        print("警告: 无法识别 GC 日志格式，跳过 GC 解析")
        cycles = []

    print(f"解析到 {len(cycles)} 个 GC 周期")

    # 解析 RSS
    rss_data: dict = {}
    if rss_csv.exists():
        rss_data = parse_rss_csv(rss_csv)
        print(f"RSS 采样点: {rss_data.get('sample_count', 0)}")

    # 解析 wall time
    wall = parse_wall_time(results_dir)

    # 生成报告
    report = build_report(results_dir, metadata, gc_mode, cycles, rss_data, wall)
    report_path = results_dir / "report.md"
    report_path.write_text(report)
    print(f"报告已生成: {report_path}")


if __name__ == "__main__":
    main()
