#!/usr/bin/env python3
"""
gc_bench_analyze.py — 分析 gc_bench_compare.sh 的输出，生成 Markdown 对比报告

用法:
    python3 gc_bench_analyze.py <results_dir>

示例:
    python3 gc_bench_analyze.py doc/benchmark/results/20260319_170000
"""

import os
import re
import sys
import statistics
from collections import defaultdict
from datetime import datetime
import numpy as np


def parse_stdout(filepath):
    """解析 benchmark 标准输出，提取功能测试和性能数据。"""
    result = {"functional": {}, "performance": {}, "total": None}
    with open(filepath, "r") as f:
        for line in f:
            line = line.strip()
            # [BENCH] F1_TreeIntegrity         PASS  157 ms
            m = re.match(r"\[BENCH\]\s+(F\d+_\w+)\s+(PASS|FAIL)\s+(\d+)\s+ms", line)
            if m:
                result["functional"][m.group(1)] = {
                    "status": m.group(2),
                    "time_ms": int(m.group(3)),
                }
                continue
            # [BENCH] P1_SmallAlloc            316 ms  ...
            m = re.match(r"\[BENCH\]\s+(P\d+_\w+)\s+(\d+)\s+ms", line)
            if m:
                result["performance"][m.group(1)] = int(m.group(2))
                continue
            # [BENCH] Total wall time:    6570 ms
            m = re.match(r"\[BENCH\]\s+Total wall time:\s+(\d+)\s+ms", line)
            if m:
                result["total"] = int(m.group(1))
    return result


def parse_resource(filepath):
    """解析资源采样 CSV，返回 CPU% 和 RSS(KB) 列表。"""
    cpu_list = []
    rss_list = []
    with open(filepath, "r") as f:
        for line in f:
            line = line.strip()
            if line.startswith("timestamp") or not line:
                continue
            parts = line.split(",")
            if len(parts) >= 3:
                try:
                    cpu_list.append(float(parts[1]))
                    rss_list.append(int(parts[2]))
                except (ValueError, IndexError):
                    continue
    return cpu_list, rss_list


def parse_resource_timeseries(filepath):
    """解析资源采样 CSV，返回 (elapsed_seconds[], cpu_pct[], rss_mb[])。"""
    timestamps, cpus, rsss = [], [], []
    with open(filepath, "r") as f:
        for line in f:
            line = line.strip()
            if line.startswith("timestamp") or not line:
                continue
            parts = line.split(",")
            if len(parts) >= 3:
                try:
                    timestamps.append(float(parts[0]))
                    cpus.append(float(parts[1]))
                    rsss.append(int(parts[2]) / 1024.0)  # KB → MB
                except (ValueError, IndexError):
                    continue
    if timestamps:
        t0 = timestamps[0]
        elapsed = [t - t0 for t in timestamps]
    else:
        elapsed = []
    return elapsed, cpus, rsss


def generate_charts(groups, results_dir, fig_dir, round_num):
    """为每个堆大小生成 CPU% 和 RSS 趋势图，返回 {(heap, metric): relative_path}。"""
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    # macOS 中文字体支持
    plt.rcParams["font.sans-serif"] = ["PingFang SC", "STHeiti", "Arial Unicode MS", "DejaVu Sans"]
    plt.rcParams["axes.unicode_minus"] = False

    os.makedirs(fig_dir, exist_ok=True)
    heap_list = sorted({h for _, h in groups.keys()})
    chart_paths = {}  # (heap, "cpu"|"rss") → relative path

    for heap in heap_list:
        # 收集每个 GC 模式的时间序列
        gc_series = {}  # gc_label → list of (elapsed, values)
        for gc_label in ["copy", "ms"]:
            key = (gc_label, heap)
            runs = groups.get(key, [])
            gc_series[gc_label] = []
            for run in runs:
                ts_data = run.get("timeseries")
                if ts_data:
                    gc_series[gc_label].append(ts_data)

        # 对每个指标 (cpu, rss) 分别画图
        for metric, ylabel, title_suffix in [
            ("cpu", "CPU %", "CPU 使用率趋势"),
            ("rss", "RSS (MB)", "内存 (RSS) 趋势"),
        ]:
            fig, ax = plt.subplots(figsize=(10, 5))

            colors = {"copy": "#2196F3", "ms": "#FF5722"}
            labels = {"copy": "CopyGC", "ms": "MS-GC"}

            for gc_label in ["copy", "ms"]:
                series_list = gc_series.get(gc_label, [])
                for i, ts in enumerate(series_list):
                    elapsed = ts["elapsed"]
                    values = ts[metric]
                    if not elapsed:
                        continue
                    alpha = 0.25
                    ax.plot(elapsed, values, color=colors[gc_label],
                            alpha=alpha, linewidth=0.8)

                # 计算均值线：对齐到公共时间轴（插值）
                if series_list:
                    # 取最长运行时间的 80% 作为公共轴上限，避免尾部偏差
                    max_times = [s["elapsed"][-1] for s in series_list if s["elapsed"]]
                    if max_times:
                        t_end = min(max_times)  # 截止到最短那轮的结束
                        common_t = np.linspace(0, t_end, 100)
                        interped = []
                        for ts in series_list:
                            if len(ts["elapsed"]) < 2:
                                continue
                            interped.append(np.interp(common_t, ts["elapsed"], ts[metric]))
                        if interped:
                            avg_vals = np.mean(interped, axis=0)
                            ax.plot(common_t, avg_vals, color=colors[gc_label],
                                    linewidth=2.5, label=f"{labels[gc_label]} (avg)")

            ax.set_xlabel("运行时间 (s)", fontsize=12)
            ax.set_ylabel(ylabel, fontsize=12)
            ax.set_title(f"Round {round_num} — {heap}MB 堆 {title_suffix}", fontsize=14)
            ax.legend(fontsize=11)
            ax.grid(True, alpha=0.3)
            ax.set_xlim(left=0)
            ax.set_ylim(bottom=0)
            plt.tight_layout()

            fname = f"round{round_num}_{heap}m_{metric}.png"
            fpath = os.path.join(fig_dir, fname)
            fig.savefig(fpath, dpi=150)
            plt.close(fig)
            # 返回相对于报告目录的路径
            chart_paths[(heap, metric)] = f"figures/{fname}"

    return chart_paths


def collect_runs(results_dir):
    """收集所有运行数据，按 (gc, heap) 分组。"""
    # key: (gc_label, heap_mb) → list of {stdout_data, cpu_list, rss_list}
    groups = defaultdict(list)

    for fname in sorted(os.listdir(results_dir)):
        if not fname.endswith("_stdout.txt"):
            continue
        # copy_256m_r1_stdout.txt  or  ms_512m_r3_stdout.txt
        m = re.match(r"(copy|ms)_(\d+)m_r(\d+)_stdout\.txt", fname)
        if not m:
            continue
        gc_label = m.group(1)
        heap_mb = int(m.group(2))
        run_id = m.group(3)

        stdout_path = os.path.join(results_dir, fname)
        resource_path = os.path.join(
            results_dir, f"{gc_label}_{heap_mb}m_r{run_id}_resource.csv"
        )

        stdout_data = parse_stdout(stdout_path)
        cpu_list, rss_list = [], []
        if os.path.exists(resource_path):
            cpu_list, rss_list = parse_resource(resource_path)

        # 时间序列数据（用于趋势图）
        ts_elapsed, ts_cpu, ts_rss = [], [], []
        if os.path.exists(resource_path):
            ts_elapsed, ts_cpu, ts_rss = parse_resource_timeseries(resource_path)

        groups[(gc_label, heap_mb)].append(
            {
                "stdout": stdout_data,
                "cpu": cpu_list,
                "rss": rss_list,
                "timeseries": {
                    "elapsed": ts_elapsed,
                    "cpu": ts_cpu,
                    "rss": ts_rss,
                },
            }
        )

    return groups


def avg(lst):
    return statistics.mean(lst) if lst else 0


def safe_max(lst):
    return max(lst) if lst else 0


def generate_report(groups, round_num, chart_paths=None):
    """生成 Round6 风格的 Markdown 对比报告。"""
    lines = []
    heap_list = sorted({h for _, h in groups.keys()})
    rounds_per_group = max(len(v) for v in groups.values()) if groups else 3

    # ── 头部信息 ──
    lines.append(f"# Round {round_num} — MS-GC vs CopyGC Benchmark 对比")
    lines.append("")
    lines.append(f"- **日期**: {datetime.now().strftime('%Y-%m-%d')}")
    lines.append("- **平台**: macOS aarch64 (Apple Silicon)")
    lines.append("- **构建类型**: Release (`-O2`), MS-GC 使用 `--gc-mode marksweep`")
    lines.append("- **测试集**: `gc_benchmark.cj`")
    lines.append(f"- **堆大小**: {' / '.join(str(h) + 'MB' for h in heap_list)}")
    lines.append(f"- **每组重复**: {rounds_per_group} 轮取平均")
    lines.append("- **资源采样**: `ps -p PID -o rss=,%cpu=` 每 0.2s 采样")
    lines.append("")
    lines.append("---")
    lines.append("")

    # ── 辅助：per-heap 章节 ──
    p_tests = ["P1_SmallAlloc", "P2_LargeAlloc", "P3_MixedWorkload",
               "P4_LongLivedChurn", "P5_Fragmentation"]
    f_tests = ["F1_TreeIntegrity", "F2_Generational", "F3_LargeObject",
               "F4_LinkedList", "F5_HashMap"]

    section_num = 1
    for heap in heap_list:
        copy_key = ("copy", heap)
        ms_key = ("ms", heap)
        copy_runs = groups.get(copy_key, [])
        ms_runs = groups.get(ms_key, [])

        lines.append(f"## {section_num}. {heap}MB 堆对比")
        lines.append("")

        # 功能完整性
        lines.append("### 功能完整性")
        lines.append("")
        lines.append("| 测试项 | CopyGC | MS-GC |")
        lines.append("|--------|--------|-------|")
        for ft in f_tests:
            cs = "N/A"
            ms = "N/A"
            if copy_runs:
                cs = copy_runs[-1]["stdout"]["functional"].get(ft, {}).get("status", "N/A")
            if ms_runs:
                ms = ms_runs[-1]["stdout"]["functional"].get(ft, {}).get("status", "N/A")
            lines.append(f"| {ft} | {cs} | {ms} |")

        copy_pass = sum(1 for ft in f_tests
                        if copy_runs and copy_runs[-1]["stdout"]["functional"]
                        .get(ft, {}).get("status") == "PASS") if copy_runs else 0
        ms_pass = sum(1 for ft in f_tests
                      if ms_runs and ms_runs[-1]["stdout"]["functional"]
                      .get(ft, {}).get("status") == "PASS") if ms_runs else 0
        lines.append(f"| **通过率** | **{copy_pass}/{len(f_tests)}** | **{ms_pass}/{len(f_tests)}** |")
        lines.append("")

        # 性能对比
        lines.append("### 性能对比")
        lines.append("")
        lines.append("| 测试项 | CopyGC | MS-GC | MS/Copy |")
        lines.append("|--------|--------|-------|---------|")
        for pt in p_tests:
            cv = avg([r["stdout"]["performance"].get(pt, 0) for r in copy_runs]) if copy_runs else 0
            mv = avg([r["stdout"]["performance"].get(pt, 0) for r in ms_runs]) if ms_runs else 0
            ratio = f"{mv / cv:.2f}×" if cv > 0 else "—"
            lines.append(f"| {pt} | {cv:.0f} ms | {mv:.0f} ms | {ratio} |")

        ct = avg([r["stdout"]["total"] or 0 for r in copy_runs]) if copy_runs else 0
        mt = avg([r["stdout"]["total"] or 0 for r in ms_runs]) if ms_runs else 0
        tr = f"{mt / ct:.2f}×" if ct > 0 else "—"
        lines.append(f"| **Total** | **{ct:.0f} ms** | **{mt:.0f} ms** | **{tr}** |")
        lines.append("")

        # 资源使用
        lines.append("### 资源使用")
        lines.append("")
        lines.append("| 指标 | CopyGC | MS-GC | 差距 |")
        lines.append("|------|--------|-------|------|")

        def res_metrics(runs):
            if not runs:
                return {}
            cpu_avgs = [avg(r["cpu"]) for r in runs if r["cpu"]]
            cpu_maxs = [safe_max(r["cpu"]) for r in runs if r["cpu"]]
            rss_avgs = [avg(r["rss"]) / 1024 for r in runs if r["rss"]]
            rss_maxs = [safe_max(r["rss"]) / 1024 for r in runs if r["rss"]]
            return {
                "avg_cpu": avg(cpu_avgs) if cpu_avgs else 0,
                "max_cpu": avg(cpu_maxs) if cpu_maxs else 0,
                "avg_rss": avg(rss_avgs) if rss_avgs else 0,
                "max_rss": avg(rss_maxs) if rss_maxs else 0,
            }

        cm = res_metrics(copy_runs)
        mm = res_metrics(ms_runs)

        def diff_str(cv, mv, unit="%", higher_bad=True):
            if cv == 0:
                return "—"
            r = mv / cv
            if higher_bad:
                return f"MS {'高' if r > 1 else '低'} {abs(r - 1) * 100:.0f}%"
            else:
                return f"MS {'低' if r < 1 else '高'} {abs(r - 1) * 100:.0f}%"

        lines.append(f"| 平均 CPU% | {cm.get('avg_cpu', 0):.1f}% | {mm.get('avg_cpu', 0):.1f}% | {diff_str(cm.get('avg_cpu', 0), mm.get('avg_cpu', 0))} |")
        lines.append(f"| 峰值 CPU% | {cm.get('max_cpu', 0):.1f}% | {mm.get('max_cpu', 0):.1f}% | {diff_str(cm.get('max_cpu', 0), mm.get('max_cpu', 0))} |")
        lines.append(f"| 平均 RSS | {cm.get('avg_rss', 0):.1f} MB | {mm.get('avg_rss', 0):.1f} MB | {diff_str(cm.get('avg_rss', 0), mm.get('avg_rss', 0))} |")
        lines.append(f"| 峰值 RSS | {cm.get('max_rss', 0):.1f} MB | {mm.get('max_rss', 0):.1f} MB | {diff_str(cm.get('max_rss', 0), mm.get('max_rss', 0))} |")
        lines.append("")

        # 趋势图
        if chart_paths:
            cpu_chart = chart_paths.get((heap, "cpu"))
            rss_chart = chart_paths.get((heap, "rss"))
            if cpu_chart or rss_chart:
                lines.append("### 资源趋势图")
                lines.append("")
                if cpu_chart:
                    lines.append(f"![CPU 趋势 — {heap}MB]({cpu_chart})")
                    lines.append("")
                if rss_chart:
                    lines.append(f"![RSS 趋势 — {heap}MB]({rss_chart})")
                    lines.append("")

        section_num += 1

    # ── 横向对比汇总 ──
    lines.append(f"## {section_num}. 多堆尺寸横向对比")
    lines.append("")

    # 总耗时
    lines.append("### 总耗时")
    lines.append("")
    lines.append("| 堆大小 | CopyGC | MS-GC | MS/Copy |")
    lines.append("|--------|--------|-------|---------|")
    for heap in heap_list:
        copy_runs = groups.get(("copy", heap), [])
        ms_runs = groups.get(("ms", heap), [])
        ct = avg([r["stdout"]["total"] or 0 for r in copy_runs]) if copy_runs else 0
        mt = avg([r["stdout"]["total"] or 0 for r in ms_runs]) if ms_runs else 0
        tr = f"{mt / ct:.2f}×" if ct > 0 else "—"
        lines.append(f"| {heap} MB | {ct:.0f} ms | {mt:.0f} ms | {tr} |")
    lines.append("")

    # Peak RSS
    lines.append("### Peak RSS")
    lines.append("")
    lines.append("| 堆大小 | CopyGC Peak | MS-GC Peak | 比值 |")
    lines.append("|--------|-------------|------------|------|")
    for heap in heap_list:
        copy_runs = groups.get(("copy", heap), [])
        ms_runs = groups.get(("ms", heap), [])
        cr = avg([safe_max(r["rss"]) / 1024 for r in copy_runs if r["rss"]]) if copy_runs else 0
        mr = avg([safe_max(r["rss"]) / 1024 for r in ms_runs if r["rss"]]) if ms_runs else 0
        ratio = f"{mr / cr:.2f}×" if cr > 0 else "—"
        lines.append(f"| {heap} MB | {cr:.0f} MB | {mr:.0f} MB | {ratio} |")
    lines.append("")

    # CPU
    lines.append("### CPU 使用率")
    lines.append("")
    lines.append("| 堆大小 | CopyGC Avg | MS-GC Avg | CopyGC Max | MS-GC Max |")
    lines.append("|--------|-----------|-----------|-----------|-----------|")
    for heap in heap_list:
        copy_runs = groups.get(("copy", heap), [])
        ms_runs = groups.get(("ms", heap), [])
        ca = avg([avg(r["cpu"]) for r in copy_runs if r["cpu"]]) if copy_runs else 0
        ma = avg([avg(r["cpu"]) for r in ms_runs if r["cpu"]]) if ms_runs else 0
        cm_max = avg([safe_max(r["cpu"]) for r in copy_runs if r["cpu"]]) if copy_runs else 0
        mm_max = avg([safe_max(r["cpu"]) for r in ms_runs if r["cpu"]]) if ms_runs else 0
        lines.append(f"| {heap} MB | {ca:.1f}% | {ma:.1f}% | {cm_max:.1f}% | {mm_max:.1f}% |")
    lines.append("")
    section_num += 1

    # ── 对比倍率汇总 ──
    lines.append(f"## {section_num}. MS-GC vs CopyGC 对比倍率汇总")
    lines.append("")
    heap_headers = " | ".join(f"{h}MB" for h in heap_list)
    heap_seps = " | ".join("------" for _ in heap_list)
    lines.append(f"| 指标 | {heap_headers} |")
    lines.append(f"|------|{heap_seps}|")

    def ratio_row(label, copy_fn, ms_fn):
        row = f"| {label} |"
        for heap in heap_list:
            copy_key = ("copy", heap)
            ms_key = ("ms", heap)
            if copy_key in groups and ms_key in groups:
                cv = copy_fn(groups[copy_key])
                mv = ms_fn(groups[ms_key])
                if cv > 0:
                    row += f" {mv / cv:.2f}× |"
                else:
                    row += " — |"
            else:
                row += " — |"
        return row

    lines.append(ratio_row(
        "Total 性能",
        lambda runs: avg([r["stdout"]["total"] or 0 for r in runs]),
        lambda runs: avg([r["stdout"]["total"] or 0 for r in runs]),
    ))
    lines.append(ratio_row(
        "峰值 RSS",
        lambda runs: avg([safe_max(r["rss"]) / 1024 for r in runs if r["rss"]]),
        lambda runs: avg([safe_max(r["rss"]) / 1024 for r in runs if r["rss"]]),
    ))
    lines.append(ratio_row(
        "平均 CPU%",
        lambda runs: avg([avg(r["cpu"]) for r in runs if r["cpu"]]),
        lambda runs: avg([avg(r["cpu"]) for r in runs if r["cpu"]]),
    ))
    lines.append("")
    section_num += 1

    # ── 结论 ──
    lines.append(f"## {section_num}. 结论")
    lines.append("")

    # 自动判定
    all_func_pass = True
    for key, runs in groups.items():
        for ft in f_tests:
            if runs[-1]["stdout"]["functional"].get(ft, {}).get("status") != "PASS":
                all_func_pass = False

    lines.append("### 判定结果")
    lines.append("")
    lines.append("| 维度 | 判定 |")
    lines.append("|------|------|")
    lines.append(f"| 功能完整性 | {'**全部 PASS**' if all_func_pass else '存在 FAIL'} |")

    for heap in heap_list:
        copy_runs = groups.get(("copy", heap), [])
        ms_runs = groups.get(("ms", heap), [])
        if copy_runs and ms_runs:
            ct = avg([r["stdout"]["total"] or 0 for r in copy_runs])
            mt = avg([r["stdout"]["total"] or 0 for r in ms_runs])
            ratio = mt / ct if ct > 0 else 0
            status = "**达标** (< 1.5×)" if ratio < 1.5 else "未达标 (>= 1.5×)"
            lines.append(f"| Total 性能 ({heap}MB) | {ratio:.2f}× → {status} |")

            cr = avg([safe_max(r["rss"]) / 1024 for r in copy_runs if r["rss"]])
            mr = avg([safe_max(r["rss"]) / 1024 for r in ms_runs if r["rss"]])
            rss_ratio = mr / cr if cr > 0 else 0
            rss_status = "**优秀** (MS 更省)" if rss_ratio < 1.0 else ("**达标** (< 1.3×)" if rss_ratio < 1.3 else "未达标 (>= 1.3×)")
            lines.append(f"| 峰值 RSS ({heap}MB) | {rss_ratio:.2f}× → {rss_status} |")

            ca = avg([avg(r["cpu"]) for r in copy_runs if r["cpu"]])
            ma = avg([avg(r["cpu"]) for r in ms_runs if r["cpu"]])
            cpu_ratio = ma / ca if ca > 0 else 0
            cpu_status = "**达标** (< 1.5×)" if cpu_ratio < 1.5 else "未达标 (>= 1.5×)"
            lines.append(f"| 平均 CPU ({heap}MB) | {cpu_ratio:.2f}× → {cpu_status} |")

    lines.append("")

    return "\n".join(lines)


def main():
    import argparse
    parser = argparse.ArgumentParser(description="Analyze GC benchmark results")
    parser.add_argument("results_dir", help="Path to results directory")
    parser.add_argument("--round", type=int, default=0,
                        help="Round number for report title (e.g. 7)")
    args = parser.parse_args()

    results_dir = args.results_dir
    if not os.path.isdir(results_dir):
        print(f"Error: {results_dir} is not a directory")
        sys.exit(1)

    groups = collect_runs(results_dir)
    if not groups:
        print(f"No benchmark results found in {results_dir}")
        sys.exit(1)

    round_num = args.round
    if round_num == 0:
        # Auto-detect next round number from existing files
        benchmark_dir = os.path.join(os.path.dirname(os.path.abspath(__file__)))
        existing = [f for f in os.listdir(benchmark_dir)
                    if re.match(r"Round(\d+)_", f)]
        if existing:
            nums = [int(re.match(r"Round(\d+)_", f).group(1)) for f in existing]
            round_num = max(nums) + 1
        else:
            round_num = 1

    # 生成趋势图
    benchmark_dir = os.path.dirname(os.path.abspath(__file__))
    fig_dir = os.path.join(benchmark_dir, "figures")
    chart_paths = generate_charts(groups, results_dir, fig_dir, round_num)
    print(f"Generated {len(chart_paths)} charts in {fig_dir}")

    report = generate_report(groups, round_num, chart_paths)

    # 输出到终端
    print(report)

    # 写入 benchmark 目录
    report_name = f"Round{round_num}_MS-GC_vs_CopyGC_Benchmark.md"
    report_path = os.path.join(benchmark_dir, report_name)
    with open(report_path, "w") as f:
        f.write(report)

    print(f"\nReport saved to: {report_path}")


if __name__ == "__main__":
    main()
