#!/usr/bin/env python3
"""Create the case29 local comparison report and charts from run artifacts."""
from __future__ import annotations

import csv
import re
import sys
from pathlib import Path

import matplotlib.pyplot as plt


RESULT = re.compile(r"total=(\d+) ok=(\d+) fail=(\d+).*throughput=(\d+)req/s")


def result_values(path: Path) -> tuple[int, int, int, int]:
    text = path.read_text(encoding="utf-8", errors="replace")
    match = RESULT.search(text)
    if not match:
        return (0, 0, 0, 0)
    return tuple(map(int, match.groups()))


def samples(path: Path) -> list[tuple[float, float]]:
    if not path.is_file():
        return []
    values: list[tuple[float, float]] = []
    with path.open(newline="", encoding="utf-8") as stream:
        for row in csv.reader(stream):
            if len(row) != 3:
                continue
            try:
                values.append((float(row[1]) / 1024.0, float(row[2])))
            except ValueError:
                continue
    return values


def main() -> int:
    root = Path(sys.argv[1]).resolve()
    labels = [("java", "Java"), ("msgc", "MSGC"), ("copygc", "CopyGC")]
    rows = []
    for directory, label in labels:
        total, ok, fail, throughput = result_values(root / directory / "run.stdout.txt")
        data = samples(root / directory / "rss.csv")
        rss_avg = sum(x[0] for x in data) / len(data) if data else 0.0
        rss_peak = max((x[0] for x in data), default=0.0)
        cpu_avg = sum(x[1] for x in data) / len(data) if data else 0.0
        cpu_peak = max((x[1] for x in data), default=0.0)
        rows.append((label, total, ok, fail, throughput, rss_avg, rss_peak, cpu_avg, cpu_peak, data))

    plt.style.use("seaborn-v0_8-whitegrid")
    fig, axes = plt.subplots(1, 3, figsize=(13, 4))
    names = [row[0] for row in rows]
    axes[0].bar(names, [row[4] for row in rows], color=["#4c78a8", "#59a14f", "#f28e2b"])
    axes[0].set_title("Throughput")
    axes[0].set_ylabel("req/s")
    axes[1].bar(names, [row[6] for row in rows], color=["#4c78a8", "#59a14f", "#f28e2b"])
    axes[1].set_title("Peak RSS")
    axes[1].set_ylabel("MiB")
    axes[2].bar(names, [row[7] for row in rows], color=["#4c78a8", "#59a14f", "#f28e2b"])
    axes[2].set_title("Average sampled CPU")
    axes[2].set_ylabel("%")
    fig.tight_layout()
    fig.savefig(root / "compare_summary.png", dpi=160)
    plt.close(fig)

    fig, axes = plt.subplots(2, 1, figsize=(11, 7), sharex=True)
    for label, *_, data in rows:
        if data:
            x = [index * 0.2 for index in range(len(data))]
            axes[0].plot(x, [item[0] for item in data], label=label)
            axes[1].plot(x, [item[1] for item in data], label=label)
    axes[0].set_title("RSS / CPU samples")
    axes[0].set_ylabel("RSS (MiB)")
    axes[1].set_ylabel("CPU (%)")
    axes[1].set_xlabel("sample time (s, 200ms interval)")
    axes[0].legend()
    fig.tight_layout()
    fig.savefig(root / "rss_cpu_timeline.png", dpi=160)
    plt.close(fig)

    metadata = (root / "run_metadata.env").read_text(encoding="utf-8")
    table = "\n".join(
        f"| {label} | {total:,} | {ok:,} | {fail:,} | {throughput:,} | {rss_avg:.2f} | {rss_peak:.2f} | {cpu_avg:.2f}% | {cpu_peak:.2f}% |"
        for label, total, ok, fail, throughput, rss_avg, rss_peak, cpu_avg, cpu_peak, _ in rows
    )
    report = f"""# case29 HTTP bank comparison\n\n## Configuration\n\n```text\n{metadata.strip()}\n```\n\nCPU is not quota-limited. `HEAP_SIZE` maps to Java `-Xmx` and Cangjie `cjHeapSize`; RSS is observed rather than OS hard-capped.\n\n## Results\n\n| Side | Total | OK | Fail | Throughput (req/s) | Avg RSS (MiB) | Peak RSS (MiB) | Avg CPU | Peak CPU |\n| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |\n{table}\n\n![Comparison summary](compare_summary.png)\n\n![RSS and CPU timeline](rss_cpu_timeline.png)\n\n## Artifacts\n\nEach side directory contains source snapshots, compile/run logs, `gc.log`, and `rss.csv` sampled every 200ms.\n"""
    (root / "compare_report.md").write_text(report, encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
