#!/usr/bin/env python3

from __future__ import annotations

import argparse
import dataclasses
import datetime as dt
import os
import pathlib
import re
import socket
import statistics
import subprocess
import sys
from typing import Sequence


TIMEOUT_EXIT_CODE = 124
FAILURE_DIAGNOSTICS_NAME = "failure_diagnostics.txt"
TAIL_LINE_COUNT = 40

RESULT_HEADER_RE = re.compile(
    r"^\[RESULT\]\s+payload=(?P<payload>\S+)\s+concurrency=(?P<concurrency>\d+)\s+duration=(?P<duration>\d+)ms\s+client_mode=(?P<client_mode>\S+)$",
    re.MULTILINE,
)
RESULT_TOTAL_RE = re.compile(
    r"^\[RESULT\]\s+total=(?P<total>\d+)\s+ok=(?P<ok>\d+)\s+fail=(?P<fail>\d+)\s+inflight=(?P<inflight>\d+)\s+throughput=(?P<throughput>\d+)req/s$",
    re.MULTILINE,
)
RESULT_BYTES_RE = re.compile(
    r"^\[RESULT\]\s+bytes_sent=(?P<bytes_sent>\d+)MB\s+bytes_recv=(?P<bytes_recv>\d+)MB$",
    re.MULTILINE,
)


@dataclasses.dataclass(frozen=True)
class BenchmarkResult:
    payload: str
    concurrency: int
    duration_ms: int
    client_mode: str
    total: int
    ok: int
    fail: int
    inflight: int
    throughput: int
    bytes_sent_mb: int
    bytes_recv_mb: int


def repo_root_for_script(script_path: pathlib.Path) -> pathlib.Path:
    return script_path.resolve().parents[2]


def default_case_dir(script_path: pathlib.Path) -> pathlib.Path:
    return repo_root_for_script(script_path) / "MSGC_RESULT" / "Result" / "case27_http_complex_json"


def default_sdk_root() -> pathlib.Path:
    return pathlib.Path("/Users/lipeisheng/code/cangjie/cangjie/cangjie_compiler/output")


def default_stdx_dir() -> pathlib.Path:
    return default_sdk_root() / "darwin_aarch64_cjnative" / "dynamic" / "stdx"


def default_envsetup() -> pathlib.Path:
    return default_sdk_root() / "envsetup.sh"


def parse_args(argv: Sequence[str] | None = None) -> argparse.Namespace:
    script_path = pathlib.Path(__file__)
    case_dir = default_case_dir(script_path)
    parser = argparse.ArgumentParser(
        description="One-click case27 throughput validator for c1000 + p100K style runs."
    )
    parser.add_argument("--sdk-root", default=str(default_sdk_root()))
    parser.add_argument("--stdx-dir", default=str(default_stdx_dir()))
    parser.add_argument("--envsetup", default=str(default_envsetup()))
    parser.add_argument("--case-dir", default=str(case_dir))
    parser.add_argument("--payload", default="100K")
    parser.add_argument("--concurrency", type=int, default=1000)
    parser.add_argument("--duration-ms", type=int, default=60000)
    parser.add_argument("--heap-size", default="4gb")
    parser.add_argument("--heap-growth", default="1")
    parser.add_argument("--scavenge-growth", default="10")
    parser.add_argument("--client-mode", default="sync")
    parser.add_argument("--runs", type=int, default=1)
    parser.add_argument("--port-base", type=int, default=19127)
    parser.add_argument("--result-root", default=None)
    parser.add_argument("--tag", default="case27_validate")
    parser.add_argument(
        "--run-timeout-sec",
        type=int,
        default=None,
        help="Max seconds for one compile+run child process before force-failing. "
        "Default resolves from duration with extra headroom.",
    )
    parser.add_argument("--dry-run", action="store_true")
    return parser.parse_args(argv)


def parse_benchmark_result(stdout: str) -> BenchmarkResult:
    header_match = RESULT_HEADER_RE.search(stdout)
    total_match = RESULT_TOTAL_RE.search(stdout)
    bytes_match = RESULT_BYTES_RE.search(stdout)
    if header_match is None or total_match is None or bytes_match is None:
        raise ValueError("missing [RESULT] lines in benchmark stdout")
    return BenchmarkResult(
        payload=header_match.group("payload"),
        concurrency=int(header_match.group("concurrency")),
        duration_ms=int(header_match.group("duration")),
        client_mode=header_match.group("client_mode"),
        total=int(total_match.group("total")),
        ok=int(total_match.group("ok")),
        fail=int(total_match.group("fail")),
        inflight=int(total_match.group("inflight")),
        throughput=int(total_match.group("throughput")),
        bytes_sent_mb=int(bytes_match.group("bytes_sent")),
        bytes_recv_mb=int(bytes_match.group("bytes_recv")),
    )


def build_run_environment(
    args: argparse.Namespace,
    out_dir: pathlib.Path,
    port: int,
) -> dict[str, str]:
    case_dir = pathlib.Path(args.case_dir).expanduser()
    return {
        "HOME": os.environ.get("HOME", ""),
        "TERM": os.environ.get("TERM", "xterm-256color"),
        "PATH": "/usr/bin:/bin:/usr/sbin:/sbin:/opt/homebrew/bin",
        "SDK_ROOT": str(pathlib.Path(args.sdk_root).expanduser()),
        "STDX_DIR": str(pathlib.Path(args.stdx_dir).expanduser()),
        "ENVSETUP": str(pathlib.Path(args.envsetup).expanduser()),
        "OUT_DIR": str(out_dir.expanduser()),
        "BENCH_SRC": str(case_dir / "benchmark.cj"),
        "PAYLOAD_DIR": str(case_dir / "payloads"),
        "CASE27_PAYLOAD": args.payload,
        "CASE27_CONCURRENCY": str(args.concurrency),
        "CASE27_DURATION_MS": str(args.duration_ms),
        "CASE27_SERVER_PORT": str(port),
        "CASE27_PAYLOAD_DIR": str(case_dir / "payloads"),
        "CASE27_CLIENT_MODE": args.client_mode,
        "HEAP_SIZE": args.heap_size,
        "RUNTIME_HEAP_GROWTH": args.heap_growth,
        "RUNTIME_SCAVENGE_GROWTH": args.scavenge_growth,
        "cjMSSweepAssist": "1",
        "cjMSEnumAssist": os.environ.get("cjMSEnumAssist", "1"),
    }


def choose_port(base_port: int, attempts: int = 20) -> int:
    for candidate in range(base_port, base_port + attempts):
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
            sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            try:
                sock.bind(("127.0.0.1", candidate))
            except OSError:
                continue
            return candidate
    raise RuntimeError(f"failed to find free port from {base_port} within {attempts} attempts")


def ensure_input_paths(args: argparse.Namespace) -> None:
    required_paths = [
        pathlib.Path(args.sdk_root),
        pathlib.Path(args.stdx_dir),
        pathlib.Path(args.envsetup),
        pathlib.Path(args.case_dir) / "benchmark.cj",
        pathlib.Path(args.case_dir) / "payloads" / f"{args.payload}.json",
    ]
    missing = [str(path) for path in required_paths if not path.exists()]
    if missing:
        raise FileNotFoundError("required input path missing:\n" + "\n".join(missing))


def render_child_script() -> str:
    return """\
set -euo pipefail
export DYLD_LIBRARY_PATH=""
source "$ENVSETUP" >/dev/null 2>&1
export PATH=/opt/homebrew/bin:/opt/homebrew/opt/llvm@16/bin:/opt/homebrew/opt/m4/bin:$PATH
export DYLD_LIBRARY_PATH="$STDX_DIR:${DYLD_LIBRARY_PATH:-}"
cd "$OUT_DIR"
rm -f benchmark compile.stdout.txt compile.stderr.txt run.stdout.txt run.stderr.txt gc.log link.txt
cjc "$BENCH_SRC" \\
  --import-path "$STDX_DIR" \\
  -L "$STDX_DIR" \\
  --link-option "$STDX_DIR/libstdx.net.http.dylib" \\
  --link-option "$STDX_DIR/libstdx.encoding.json.dylib" \\
  -o benchmark > compile.stdout.txt 2> compile.stderr.txt
codesign -f -s - benchmark >/dev/null 2>&1 || true
otool -L benchmark > link.txt 2>/dev/null || true
set +e
MRT_REPORT="$OUT_DIR/gc.log" \\
MRT_LOG_FILE_SIZE="200mb" \\
cjHeapSize="$HEAP_SIZE" \\
cjHeapGrowth="$RUNTIME_HEAP_GROWTH" \\
cjScavengeGrowth="$RUNTIME_SCAVENGE_GROWTH" \\
CASE27_PAYLOAD="$CASE27_PAYLOAD" \\
CASE27_CONCURRENCY="$CASE27_CONCURRENCY" \\
CASE27_DURATION_MS="$CASE27_DURATION_MS" \\
CASE27_PAYLOAD_DIR="$CASE27_PAYLOAD_DIR" \\
CASE27_SERVER_PORT="$CASE27_SERVER_PORT" \\
CASE27_CLIENT_MODE="$CASE27_CLIENT_MODE" \\
./benchmark > run.stdout.txt 2> run.stderr.txt
rc=$?
set -e
exit "$rc"
"""


def resolve_run_timeout_sec(args: argparse.Namespace) -> int:
    if args.run_timeout_sec is not None:
        if args.run_timeout_sec < 1:
            raise ValueError("--run-timeout-sec must be >= 1")
        return args.run_timeout_sec
    duration_sec = max(args.duration_ms // 1000, 0)
    return max(duration_sec + 90, 180)


def coerce_subprocess_output(value: str | bytes | None) -> str:
    if value is None:
        return ""
    if isinstance(value, bytes):
        return value.decode(errors="replace")
    return value


def tail_text_file(path: pathlib.Path, line_count: int = TAIL_LINE_COUNT) -> str:
    if not path.exists():
        return "<missing>"
    content = path.read_text(errors="replace").splitlines()
    if not content:
        return "<empty>"
    return "\n".join(content[-line_count:])


def collect_process_snapshot() -> str:
    completed = subprocess.run(
        ["ps", "-axo", "pid,ppid,stat,etime,command"],
        capture_output=True,
        text=True,
    )
    if completed.returncode != 0:
        return f"<ps failed rc={completed.returncode}>\n{completed.stderr.strip()}"
    markers = (
        "run_case27_throughput.py",
        "/bin/bash -c set -euo pipefail",
        "./benchmark",
    )
    matched = [
        line
        for line in completed.stdout.splitlines()
        if any(marker in line for marker in markers)
    ]
    if not matched:
        return "<no matching benchmark-related process found>"
    return "\n".join(matched)


def build_failure_message(
    headline: str,
    diagnostics_path: pathlib.Path,
    out_dir: pathlib.Path,
) -> str:
    sections = [
        headline,
        f"out_dir={out_dir}",
        f"diagnostics={diagnostics_path}",
    ]
    for filename in ("run.stderr.txt", "gc.log"):
        file_path = out_dir / filename
        tail = tail_text_file(file_path, line_count=12)
        if tail not in {"<missing>", "<empty>"}:
            sections.append(f"{filename} tail:\n{tail}")
    return "\n".join(sections)


def write_failure_diagnostics(
    out_dir: pathlib.Path,
    headline: str,
) -> pathlib.Path:
    sections = [
        headline,
        f"out_dir={out_dir}",
        "",
        "[process_snapshot]",
        collect_process_snapshot(),
    ]
    for filename in (
        "compile.stderr.txt",
        "child.stderr.txt",
        "run.stderr.txt",
        "run.stdout.txt",
        "gc.log",
    ):
        sections.extend(
            [
                "",
                f"[{filename}]",
                tail_text_file(out_dir / filename),
            ]
        )
    diagnostics_path = out_dir / FAILURE_DIAGNOSTICS_NAME
    diagnostics_path.write_text("\n".join(sections) + "\n")
    return diagnostics_path


def write_run_summary(
    summary_path: pathlib.Path,
    run_index: int,
    start_time: dt.datetime,
    end_time: dt.datetime,
    args: argparse.Namespace,
    port: int,
    rc: int,
    result: BenchmarkResult | None,
    error_message: str | None = None,
) -> None:
    lines = [
        f"run_index={run_index}",
        f"start={start_time.strftime('%Y-%m-%d %H:%M:%S')}",
        f"end={end_time.strftime('%Y-%m-%d %H:%M:%S')}",
        f"sdk_root={pathlib.Path(args.sdk_root).resolve()}",
        f"stdx_dir={pathlib.Path(args.stdx_dir).resolve()}",
        f"case_dir={pathlib.Path(args.case_dir).resolve()}",
        f"port={port}",
        f"payload={args.payload}",
        f"concurrency={args.concurrency}",
        f"duration_ms={args.duration_ms}",
        f"heap={args.heap_size}",
        f"run_timeout_sec={resolve_run_timeout_sec(args)}",
        f"compile_and_run_rc={rc}",
    ]
    if error_message is not None:
        lines.append(f"error_message={error_message.replace(chr(10), ' | ')}")
    if result is not None:
        lines.extend(
            [
                f"payload_result={result.payload}",
                f"concurrency_result={result.concurrency}",
                f"duration_result_ms={result.duration_ms}",
                f"client_mode={result.client_mode}",
                f"total={result.total}",
                f"ok={result.ok}",
                f"fail={result.fail}",
                f"inflight={result.inflight}",
                f"throughput={result.throughput}",
                f"bytes_sent_mb={result.bytes_sent_mb}",
                f"bytes_recv_mb={result.bytes_recv_mb}",
            ]
        )
    summary_path.write_text("\n".join(lines) + "\n")


def write_aggregate_summary(
    summary_path: pathlib.Path,
    run_results: Sequence[BenchmarkResult],
) -> None:
    throughputs = [result.throughput for result in run_results]
    fail_total = sum(result.fail for result in run_results)
    content = [
        f"runs={len(run_results)}",
        f"throughput_avg={statistics.mean(throughputs):.2f}",
        f"throughput_min={min(throughputs)}",
        f"throughput_max={max(throughputs)}",
        f"fail_total={fail_total}",
    ]
    summary_path.write_text("\n".join(content) + "\n")


def timestamp_now() -> str:
    return dt.datetime.now().strftime("%Y%m%d_%H%M%S")


def make_result_dir(args: argparse.Namespace) -> pathlib.Path:
    case_dir = pathlib.Path(args.case_dir).resolve()
    result_root = pathlib.Path(args.result_root).resolve() if args.result_root else case_dir / "result"
    run_count_label = f"{args.runs}run" if args.runs == 1 else f"{args.runs}runs"
    dirname = (
        f"{timestamp_now()}_{args.tag}_c{args.concurrency}_p{args.payload}_"
        f"{args.duration_ms // 1000}s_heap{args.heap_size}_{run_count_label}"
    )
    result_dir = result_root / dirname
    result_dir.mkdir(parents=True, exist_ok=False)
    return result_dir


def run_single_benchmark(
    args: argparse.Namespace,
    run_index: int,
    out_dir: pathlib.Path,
    port: int,
) -> tuple[int, BenchmarkResult | None, str | None]:
    env = build_run_environment(args=args, out_dir=out_dir, port=port)
    if args.dry_run:
        print(f"[dry-run] run={run_index} port={port} out_dir={out_dir}")
        for key in sorted(env):
            print(f"[dry-run] {key}={env[key]}")
        return 0, None, None
    child_stdout = out_dir / "child.stdout.txt"
    child_stderr = out_dir / "child.stderr.txt"
    timeout_sec = resolve_run_timeout_sec(args)
    try:
        completed = subprocess.run(
            ["/bin/bash", "-c", render_child_script()],
            env=env,
            cwd=out_dir,
            text=True,
            capture_output=True,
            timeout=timeout_sec,
        )
    except subprocess.TimeoutExpired as exc:
        child_stdout.write_text(coerce_subprocess_output(exc.output))
        child_stderr.write_text(coerce_subprocess_output(exc.stderr))
        diagnostics_path = write_failure_diagnostics(
            out_dir=out_dir,
            headline=f"benchmark timed out after {timeout_sec}s",
        )
        error_message = build_failure_message(
            headline=f"benchmark timed out after {timeout_sec}s",
            diagnostics_path=diagnostics_path,
            out_dir=out_dir,
        )
        return TIMEOUT_EXIT_CODE, None, error_message
    child_stdout.write_text(completed.stdout)
    child_stderr.write_text(completed.stderr)
    result = None
    error_message = None
    stdout_path = out_dir / "run.stdout.txt"
    if stdout_path.exists():
        stdout = stdout_path.read_text(errors="replace")
        try:
            result = parse_benchmark_result(stdout)
        except ValueError:
            result = None
    if completed.returncode != 0:
        diagnostics_path = write_failure_diagnostics(
            out_dir=out_dir,
            headline=f"benchmark exited with rc={completed.returncode}",
        )
        error_message = build_failure_message(
            headline=f"benchmark exited with rc={completed.returncode}",
            diagnostics_path=diagnostics_path,
            out_dir=out_dir,
        )
    elif result is None:
        diagnostics_path = write_failure_diagnostics(
            out_dir=out_dir,
            headline="benchmark finished without parsable throughput result",
        )
        error_message = build_failure_message(
            headline="benchmark finished without parsable throughput result",
            diagnostics_path=diagnostics_path,
            out_dir=out_dir,
        )
    return completed.returncode, result, error_message


def print_run_result(run_index: int, result: BenchmarkResult) -> None:
    print(
        f"[run {run_index}] throughput={result.throughput} req/s "
        f"ok={result.ok} fail={result.fail} total={result.total} duration={result.duration_ms}ms"
    )


def main(argv: Sequence[str] | None = None) -> int:
    args = parse_args(argv)
    if args.runs < 1:
        raise ValueError("--runs must be >= 1")
    resolve_run_timeout_sec(args)
    ensure_input_paths(args)
    result_dir = make_result_dir(args)
    print(f"[info] result_dir={result_dir}")
    all_results: list[BenchmarkResult] = []
    for run_index in range(1, args.runs + 1):
        out_dir = result_dir / f"run{run_index}"
        out_dir.mkdir(parents=True, exist_ok=False)
        port = choose_port(args.port_base + run_index - 1)
        start_time = dt.datetime.now()
        rc, result, error_message = run_single_benchmark(
            args=args,
            run_index=run_index,
            out_dir=out_dir,
            port=port,
        )
        end_time = dt.datetime.now()
        write_run_summary(
            summary_path=out_dir / "summary.txt",
            run_index=run_index,
            start_time=start_time,
            end_time=end_time,
            args=args,
            port=port,
            rc=rc,
            result=result,
            error_message=error_message,
        )
        if args.dry_run:
            continue
        if rc != 0:
            if error_message is not None:
                print(f"[error] run {run_index} failed:\n{error_message}", file=sys.stderr)
            else:
                print(f"[error] run {run_index} failed with rc={rc}; see {out_dir}", file=sys.stderr)
            return rc
        if result is None:
            if error_message is not None:
                print(
                    f"[error] run {run_index} finished without throughput data:\n{error_message}",
                    file=sys.stderr,
                )
            else:
                print(
                    f"[error] run {run_index} finished but no throughput data found; see {out_dir}",
                    file=sys.stderr,
                )
            return 1
        all_results.append(result)
        print_run_result(run_index, result)
    if not args.dry_run:
        write_aggregate_summary(result_dir / "summary.txt", all_results)
        throughputs = [result.throughput for result in all_results]
        print(
            "[summary] "
            f"runs={len(all_results)} avg={statistics.mean(throughputs):.2f} "
            f"min={min(throughputs)} max={max(throughputs)} req/s"
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
