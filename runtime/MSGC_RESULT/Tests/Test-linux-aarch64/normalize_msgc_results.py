#!/usr/bin/env python3

import json
import re
import sys
from pathlib import Path


def output_text(test):
    output = test.get("output", "")
    if isinstance(output, str):
        return output
    if isinstance(output, list):
        chunks = []
        for item in output:
            if isinstance(item, dict):
                for key in ("cmd", "stdout", "stderr", "output"):
                    value = item.get(key)
                    if value is not None:
                        chunks.append(str(value))
            else:
                chunks.append(str(item))
        return "\n".join(chunks)
    return str(output)


def classify(test):
    raw = test.get("result", "UNRESOLVED")
    text = output_text(test)
    lower = text.lower()
    if raw == "UNRESOLVED" or "no valid command statement was found" in lower:
        return "UNSUPPORTED", "no_valid_command"
    if raw != "FAIL":
        return raw, ""

    missing_package = (
        "can not find package" in lower
        or "cannot find package" in lower
        or "package_search_error" in lower
        or "no such package" in lower
    )
    missing_stdx = bool(re.search(r"stdx\.(chir|syntax|syntaxffi|aspect_cj)", lower))
    missing_tool = bool(
        re.search(
            r"(cjtrace-recover|cjfmt|cjcpl|cjpm|%link-python|dos2unix).*"
            r"(not found|no such file|invalid file|command not found)",
            lower,
        )
    )
    missing_library = (
        "library not found" in lower
        or "cannot find -lstdx" in lower
        or "cannot find -lcangjie" in lower
    )
    if missing_package or (missing_stdx and (missing_package or missing_library)):
        return "UNSUPPORTED", "sdk_package_or_stdx_missing"
    if missing_tool:
        return "ENV-ERROR", "host_tool_or_macro_missing"
    if missing_library:
        return "ENV-ERROR", "link_dependency_missing"
    return "FAIL", ""


def main():
    if len(sys.argv) != 4:
        raise SystemExit("usage: normalize_msgc_results.py RESULTS_JSON STAGE_ROOT STAGE")
    result_path = Path(sys.argv[1])
    stage_root = Path(sys.argv[2])
    stage = sys.argv[3]
    raw = json.loads(result_path.read_text(encoding="utf-8"))
    tests = []
    raw_counts = {}
    normalized_counts = {}

    for suite in raw if isinstance(raw, list) else []:
        for key, value in suite.items():
            if key not in ("name", "tests") and isinstance(value, int):
                raw_counts[key] = raw_counts.get(key, 0) + value
        for test in suite.get("tests", []):
            normalized, reason = classify(test)
            item = dict(test)
            item["raw_result"] = test.get("result")
            item["result"] = normalized
            if reason:
                item["classification"] = reason
            tests.append(item)
            normalized_counts[normalized] = normalized_counts.get(normalized, 0) + 1

    summary = {
        "stage": stage,
        "raw_results": raw_counts,
        "results": normalized_counts,
        "tests": tests,
    }
    (stage_root / "normalized_results.json").write_text(
        json.dumps(summary, ensure_ascii=False, indent=2), encoding="utf-8"
    )

    for result_name, file_name in (
        ("FAIL", "failed_cases.txt"),
        ("UNSUPPORTED", "unsupported_cases.txt"),
        ("ENV-ERROR", "environment_error_cases.txt"),
    ):
        lines = []
        for test in tests:
            if test.get("result") == result_name:
                lines.append(
                    "{}\t{}\t{}\t{}".format(
                        test.get("name", ""),
                        test.get("classification", ""),
                        test.get("cfg", ""),
                        test.get("log_file", ""),
                    )
                )
        (stage_root / file_name).write_text(
            "\n".join(lines) + ("\n" if lines else ""), encoding="utf-8"
        )

    summary_lines = [
        "stage: {}".format(stage),
        "raw_results: {}".format(json.dumps(raw_counts, sort_keys=True)),
        "results: {}".format(json.dumps(normalized_counts, sort_keys=True)),
    ]
    (stage_root / "summary.txt").write_text(
        "\n".join(summary_lines) + "\n", encoding="utf-8"
    )


if __name__ == "__main__":
    main()
