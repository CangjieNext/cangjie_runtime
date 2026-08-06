#!/usr/bin/env python3
from pathlib import Path


CASE_DIR = Path(__file__).resolve().parent.parent
RUNNER = CASE_DIR / "run_compare.sh"
JACKSON_CORE_JAR = CASE_DIR / "java_benchmark" / "lib" / "jackson-core-2.17.2.jar"


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def main() -> None:
    require(RUNNER.is_file(), "case25 run_compare.sh is missing")
    text = RUNNER.read_text(encoding="utf-8")
    for required in (
        '"$CASE_DIR/payloads/${PAYLOAD}.json"',
        '"$CASE_DIR/go_benchmark"',
        '"$CASE_DIR/java_benchmark/Case27HttpComplexJsonBenchmark.java"',
        '"$SCRIPT_DIR/compare_results.py"',
        '"BENCH_SRC=$CASE_DIR/benchmark.cj"',
        '"$CASE_DIR/stream_transform.cj"',
        '"$CASE_DIR/persistent_baseline.cj"',
        'CASE25_PERSISTENT_BYTES',
        'CASE27_PAYLOAD=$PAYLOAD',
    ):
        require(required in text, f"runner is missing local/self-contained dependency: {required}")
    require("case26_http_complex_json" not in text, "runner must not reference case26")
    require("../case27_http_complex_json" not in text, "runner must not reference case27")
    require(JACKSON_CORE_JAR.is_file(), "vendored jackson-core jar is missing")
    require(
        'JACKSON_CORE_JAR="$CASE_DIR/java_benchmark/lib/jackson-core-2.17.2.jar"' in text,
        "runner must define its local Jackson classpath",
    )
    require(
        '"$JAVAC_BIN" "${module_opts[@]}" -cp "$JACKSON_CORE_JAR"' in text,
        "runner must compile Java against Jackson core",
    )
    require(
        '-cp "$OUT_DIR/classes:$JACKSON_CORE_JAR" Case27HttpComplexJsonBenchmarkTest' in text,
        "runner must run Java tests against Jackson core",
    )


if __name__ == "__main__":
    main()
