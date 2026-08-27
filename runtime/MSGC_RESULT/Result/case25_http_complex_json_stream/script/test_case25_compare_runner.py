#!/usr/bin/env python3
from pathlib import Path


CASE_DIR = Path(__file__).resolve().parent.parent
RUNNERS = (
    CASE_DIR / "run_compare.sh",
    CASE_DIR / "run_compare_linux.sh",
)
WINDOWS_RUNNER = CASE_DIR / "run_compare_windows.ps1"
JACKSON_CORE_JAR = CASE_DIR / "java_benchmark" / "lib" / "jackson-core-2.17.2.jar"


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def main() -> None:
    for runner in RUNNERS:
        require(runner.is_file(), f"case25 {runner.name} is missing")
        text = runner.read_text(encoding="utf-8")
        for required in (
            '"$CASE_DIR/payloads/${PAYLOAD}.json"',
            '"$CASE_DIR/go_benchmark"',
            '"$CASE_DIR/java_benchmark/Case27HttpComplexJsonBenchmark.java"',
            '"$SCRIPT_DIR/compare_results.py"',
            '"$CASE_DIR/persistent_baseline.cj"',
            'CASE25_PERSISTENT_BYTES',
            'CASE27_PAYLOAD=$PAYLOAD',
        ):
            require(required in text, f"{runner.name} is missing dependency: {required}")
        require("case26_http_complex_json" not in text, f"{runner.name} must not reference case26")
        require("../case27_http_complex_json" not in text, f"{runner.name} must not reference case27")
        require('-cp "$JACKSON_CORE_JAR"' in text, f"{runner.name} must compile Java against Jackson core")
        require(
            "Case27HttpComplexJsonBenchmarkTest" in text,
            f"{runner.name} must run the Java source tests",
        )
    require(JACKSON_CORE_JAR.is_file(), "vendored jackson-core jar is missing")
    require(
        'JACKSON_CORE_JAR="$CASE_DIR/java_benchmark/lib/jackson-core-2.17.2.jar"' in text,
        "runner must define its local Jackson classpath",
    )
    require(WINDOWS_RUNNER.is_file(), "case25 Windows compare runner is missing")
    windows_text = WINDOWS_RUNNER.read_text(encoding="utf-8")
    for required in (
        '"C:\\cangjie-pkg\\main\\20260827"',
        '"--dy-std"',
        '".dll.a"',
        '".dll"',
        'CASE25_PERSISTENT_BYTES',
        'CASE27_PAYLOAD',
        'Start-MonitoredProcess',
        'compare_results.py',
        'java_benchmark\\lib\\jackson-core-2.17.2.jar',
        'go_benchmark',
    ):
        require(required in windows_text, f"Windows runner is missing dependency: {required}")
    require("case26_http_complex_json" not in windows_text, "Windows runner must not reference case26")
    require("../case27_http_complex_json" not in windows_text, "Windows runner must not reference case27")


if __name__ == "__main__":
    main()
