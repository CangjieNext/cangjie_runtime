#!/usr/bin/env python3
"""Aggregate per-stage MSGC test summaries without loading large raw JSON files."""

from collections import Counter
import json
from pathlib import Path
import sys


STAGES = (
    "01-runtime-llt",
    "02-runtime-hlt",
    "03-stdlib-llt",
    "04-stdlib-hlt",
    "05-compiler-llt",
    "06-compiler-hlt",
)


def parse_summary(path: Path):
    values = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        key, separator, value = line.partition(": ")
        if separator and key in {"raw_results", "results"}:
            values[key] = json.loads(value)
    missing = {"raw_results", "results"} - values.keys()
    if missing:
        raise ValueError(f"missing {sorted(missing)} in {path}")
    return values


def copy_cases(stage_dir: Path, filename: str, output, stage: str):
    path = stage_dir / filename
    if not path.exists():
        return 0
    count = 0
    for line in path.read_text(encoding="utf-8").splitlines():
        if line.strip():
            output.write(f"[{stage}] {line}\n")
            count += 1
    return count


def main():
    if len(sys.argv) != 2:
        raise SystemExit(f"usage: {sys.argv[0]} RUN_ROOT")

    run_root = Path(sys.argv[1]).resolve()
    if not run_root.is_dir():
        raise SystemExit(f"run root does not exist: {run_root}")

    raw_total = Counter()
    result_total = Counter()
    stage_data = []
    for stage in STAGES:
        stage_dir = run_root / stage
        summary = parse_summary(stage_dir / "summary.txt")
        raw = {key: int(value) for key, value in summary["raw_results"].items()}
        results = {key: int(value) for key, value in summary["results"].items()}
        raw_total.update(raw)
        result_total.update(results)
        stage_data.append(
            {
                "stage": stage,
                "raw_results": raw,
                "results": results,
            }
        )

    failed_path = run_root / "all_failed_cases.txt"
    unsupported_path = run_root / "all_unsupported_cases.txt"
    environment_path = run_root / "all_environment_error_cases.txt"
    case_counts = {}
    with failed_path.open("w", encoding="utf-8") as output:
        case_counts["FAIL"] = sum(
            copy_cases(run_root / stage, "failed_cases.txt", output, stage)
            for stage in STAGES
        )
    with unsupported_path.open("w", encoding="utf-8") as output:
        case_counts["UNSUPPORTED"] = sum(
            copy_cases(run_root / stage, "unsupported_cases.txt", output, stage)
            for stage in STAGES
        )
    with environment_path.open("w", encoding="utf-8") as output:
        case_counts["ENV-ERROR"] = sum(
            copy_cases(run_root / stage, "environment_error_cases.txt", output, stage)
            for stage in STAGES
        )

    aggregate = {
        "run_root": str(run_root),
        "stages": stage_data,
        "raw_results": dict(sorted(raw_total.items())),
        "results": dict(sorted(result_total.items())),
        "case_list_counts": case_counts,
    }
    (run_root / "final_summary.json").write_text(
        json.dumps(aggregate, indent=2, ensure_ascii=True) + "\n", encoding="utf-8"
    )

    lines = [
        "MSGC Linux aarch64 test run summary",
        f"run_root: {run_root}",
        "",
    ]
    for item in stage_data:
        lines.append(f"[{item['stage']}]")
        lines.append(f"raw_results: {json.dumps(item['raw_results'], sort_keys=True)}")
        lines.append(f"results: {json.dumps(item['results'], sort_keys=True)}")
        lines.append("")
    lines.append("[TOTAL]")
    lines.append(f"raw_results: {json.dumps(dict(sorted(raw_total.items())))}")
    lines.append(f"results: {json.dumps(dict(sorted(result_total.items())))}")
    lines.append(f"case_list_counts: {json.dumps(case_counts, sort_keys=True)}")
    (run_root / "final_summary.txt").write_text("\n".join(lines) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
