#!/usr/bin/env python3
from pathlib import Path


CASE_DIR = Path(__file__).resolve().parent.parent
RUNNERS = (
    CASE_DIR / "run_compare.sh",
    CASE_DIR / "run_compare_linux.sh",
)
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
