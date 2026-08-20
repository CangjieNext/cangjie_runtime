from pathlib import Path


CASE_DIR = Path(__file__).resolve().parent.parent
RUNNER = CASE_DIR / "run_compare.sh"
PYTHON_RUNNER = CASE_DIR / "script" / "run_case29_compare.py"
CANGJIE_DIR = CASE_DIR / "cangjie_benchmark"


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def main() -> None:
    require(RUNNER.is_file(), "case29 run_compare.sh is missing")
    require(PYTHON_RUNNER.is_file(), "case29 Python runner is missing")
    for name in ("member_accounts_benchmark.cj", "member_accounts_common.cj", "test_member_accounts_api.cj"):
        require((CANGJIE_DIR / name).is_file(), f"cangjie_benchmark/{name} is missing")
        require(not (CASE_DIR / name).exists(), f"{name} must not remain in the case root")
    text = RUNNER.read_text(encoding="utf-8") + PYTHON_RUNNER.read_text(encoding="utf-8")
    for name in (
        "CASE29_SEED",
        "CASE29_ACCOUNT_COUNT",
        "CASE29_SUBJECT_ID",
        "CASE29_LIMIT",
        "CASE29_OFFSET",
        "CASE29_CONCURRENCY",
        "CASE29_DURATION_MS",
        "PROCS",
        "HEAP_SIZE",
        "cjProcessorNum",
        "-XX:ActiveProcessorCount",
        "Java",
        "MSGC",
        "CopyGC",
        "cangjie_benchmark",
    ):
        require(name in text, f"runner is missing {name}")
    for retired in ("CPU_QUOTA_CORES", "cpu_quota_cores", "proc_pid_rusage", "SIGSTOP"):
        require(retired not in text, f"runner still contains retired CPU quota control: {retired}")


if __name__ == "__main__":
    main()
