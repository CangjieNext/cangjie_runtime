#!/usr/bin/env python3

"""Compare case27 throughput with isolated CopyGC and MSGC SDKs."""

from __future__ import annotations

import argparse
import dataclasses
import datetime as dt
import os
import pathlib
import re
import statistics
import subprocess
import sys
from typing import Sequence

import run_case27_throughput as throughput


DEFAULT_COPYGC_COMPILER_OUTPUT = pathlib.Path(
    "/Users/lipeisheng/code/cangjie/cangjie/cangjie_compiler/output"
)
DEFAULT_MSGC_SDK_ROOT = pathlib.Path(
    "/Users/lipeisheng/code/cangjie/cangjie/"
    "cangjie_compiler_region_mark/output/msgc"
)
DEFAULT_PLATFORM_DIR = "darwin_aarch64_cjnative"
RUNTIME_PATH_RE = re.compile(r"(/\S*libcangjie-runtime\.dylib)")


@dataclasses.dataclass(frozen=True)
class SDKSpec:
    label: str
    root: pathlib.Path
    cjc_gc_mode: str
    binary_name: str
    platform_dir: str

    @property
    def compiler(self) -> pathlib.Path:
        return self.root / "bin" / "cjc"

    @property
    def envsetup(self) -> pathlib.Path:
        return self.root / "envsetup.sh"

    @property
    def stdlib_modules(self) -> pathlib.Path:
        return self.root / "modules" / self.platform_dir

    @property
    def stdx_dir(self) -> pathlib.Path:
        return self.root / self.platform_dir / "dynamic" / "stdx"

    @property
    def runtime_dylib(self) -> pathlib.Path:
        return (
            self.root
            / "runtime"
            / "lib"
            / self.platform_dir
            / "libcangjie-runtime.dylib"
        )


def parse_args(argv: Sequence[str] | None = None) -> argparse.Namespace:
    case_dir = throughput.default_case_dir(pathlib.Path(__file__))
    parser = argparse.ArgumentParser(
        description=(
            "Compile and compare case27 with isolated CopyGC/MSGC SDKs. "
            "Each side is rebuilt from benchmark.cj with an explicit --gc-mode."
        )
    )
    parser.add_argument(
        "--copygc-sdk-root",
        default=str(DEFAULT_COPYGC_COMPILER_OUTPUT / "copygc"),
    )
    parser.add_argument(
        "--msgc-sdk-root",
        default=str(DEFAULT_MSGC_SDK_ROOT),
    )
    parser.add_argument("--platform-dir", default=DEFAULT_PLATFORM_DIR)
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
    parser.add_argument("--tag", default="case27_msgc_vs_copygc")
    parser.add_argument(
        "--run-timeout-sec",
        type=int,
        default=None,
        help=(
            "Max seconds for one SDK compile+run child. The default is derived "
            "from --duration-ms with benchmark startup headroom."
        ),
    )
    parser.add_argument("--dry-run", action="store_true")
    return parser.parse_args(argv)


def build_sdk_specs(args: argparse.Namespace) -> tuple[SDKSpec, SDKSpec]:
    return (
        SDKSpec(
            label="copygc",
            root=pathlib.Path(args.copygc_sdk_root).expanduser(),
            cjc_gc_mode="copygc",
            binary_name="benchmark_copygc",
            platform_dir=args.platform_dir,
        ),
        SDKSpec(
            label="msgc",
            root=pathlib.Path(args.msgc_sdk_root).expanduser(),
            cjc_gc_mode="marksweep",
            binary_name="benchmark_msgc",
            platform_dir=args.platform_dir,
        ),
    )


def ensure_input_paths(args: argparse.Namespace, specs: Sequence[SDKSpec]) -> None:
    if args.dry_run:
        return
    case_dir = pathlib.Path(args.case_dir).expanduser()
    required_paths = [
        case_dir / "benchmark.cj",
        case_dir / "payloads" / f"{args.payload}.json",
    ]
    for spec in specs:
        required_paths.extend(
            [
                spec.root,
                spec.compiler,
                spec.envsetup,
                spec.stdlib_modules,
                spec.stdx_dir,
                spec.runtime_dylib,
            ]
        )
    missing = [str(path) for path in required_paths if not path.exists()]
    if missing:
        raise FileNotFoundError("required input path missing:\n" + "\n".join(missing))


def build_run_environment(
    args: argparse.Namespace,
    spec: SDKSpec,
    out_dir: pathlib.Path,
    port: int,
) -> dict[str, str]:
    case_dir = pathlib.Path(args.case_dir).expanduser()
    env = {
        "HOME": os.environ.get("HOME", ""),
        "TERM": os.environ.get("TERM", "xterm-256color"),
        "PATH": "/usr/bin:/bin:/usr/sbin:/sbin:/opt/homebrew/bin",
        "SDK_LABEL": spec.label,
        "SDK_ROOT": str(spec.root),
        "ENVSETUP": str(spec.envsetup),
        "STDX_DIR": str(spec.stdx_dir),
        "EXPECTED_RUNTIME": str(spec.runtime_dylib),
        "CJC_GC_MODE": spec.cjc_gc_mode,
        "BENCH_BINARY": spec.binary_name,
        "OUT_DIR": str(out_dir.expanduser()),
        "BENCH_SRC": str(case_dir / "benchmark.cj"),
        "CASE27_PAYLOAD": args.payload,
        "CASE27_CONCURRENCY": str(args.concurrency),
        "CASE27_DURATION_MS": str(args.duration_ms),
        "CASE27_SERVER_PORT": str(port),
        "CASE27_PAYLOAD_DIR": str(case_dir / "payloads"),
        "CASE27_CLIENT_MODE": args.client_mode,
        "HEAP_SIZE": args.heap_size,
        "RUNTIME_HEAP_GROWTH": args.heap_growth,
        "RUNTIME_SCAVENGE_GROWTH": args.scavenge_growth,
    }
    if spec.label == "msgc":
        env["cjMSSweepAssist"] = os.environ.get("cjMSSweepAssist", "1")
        env["cjMSEnumAssist"] = os.environ.get("cjMSEnumAssist", "1")
    return env


def render_child_script() -> str:
    return """\
set -euo pipefail
export DYLD_LIBRARY_PATH=""
unset DYLD_FALLBACK_LIBRARY_PATH || true
source "$ENVSETUP" >/dev/null 2>&1
export PATH=/opt/homebrew/bin:/opt/homebrew/opt/llvm@16/bin:/opt/homebrew/opt/m4/bin:$PATH
export DYLD_LIBRARY_PATH="$STDX_DIR:${DYLD_LIBRARY_PATH:-}"
cd "$OUT_DIR"
rm -f "$BENCH_BINARY" \\
  compile.stdout.txt compile.stderr.txt \\
  probe.stdout.txt probe.stderr.txt probe_gc.log \\
  run.stdout.txt run.stderr.txt gc.log link.txt loaded_runtime.txt
"$SDK_ROOT/bin/cjc" "$BENCH_SRC" \\
  --gc-mode="$CJC_GC_MODE" \\
  --import-path "$STDX_DIR" \\
  -L "$STDX_DIR" \\
  --link-option "$STDX_DIR/libstdx.net.http.dylib" \\
  --link-option "$STDX_DIR/libstdx.encoding.json.dylib" \\
  -o "$BENCH_BINARY" > compile.stdout.txt 2> compile.stderr.txt
codesign -f -s - "$BENCH_BINARY" >/dev/null 2>&1 || true
otool -L "$BENCH_BINARY" > link.txt 2>/dev/null || true
set +e
DYLD_PRINT_LIBRARIES=1 \\
MRT_REPORT="$OUT_DIR/probe_gc.log" \\
MRT_LOG_FILE_SIZE="10mb" \\
cjHeapSize="$HEAP_SIZE" \\
cjHeapGrowth="$RUNTIME_HEAP_GROWTH" \\
cjScavengeGrowth="$RUNTIME_SCAVENGE_GROWTH" \\
CASE27_PAYLOAD="$CASE27_PAYLOAD" \\
CASE27_CONCURRENCY="1" \\
CASE27_DURATION_MS="1" \\
CASE27_PAYLOAD_DIR="$CASE27_PAYLOAD_DIR" \\
CASE27_SERVER_PORT="$CASE27_SERVER_PORT" \\
CASE27_CLIENT_MODE="$CASE27_CLIENT_MODE" \\
./"$BENCH_BINARY" > probe.stdout.txt 2> probe.stderr.txt
probe_rc=$?
set -e
grep "libcangjie-runtime.dylib" probe.stderr.txt > loaded_runtime.txt || true
if ! grep -F "$EXPECTED_RUNTIME" probe.stderr.txt >/dev/null; then
  echo "runtime probe mismatch: expected $EXPECTED_RUNTIME (probe_rc=$probe_rc)" >&2
  exit 86
fi
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
./"$BENCH_BINARY" > run.stdout.txt 2> run.stderr.txt
rc=$?
set -e
exit "$rc"
"""


def validate_loaded_runtime(stderr_path: pathlib.Path, spec: SDKSpec) -> pathlib.Path:
    if not stderr_path.exists():
        raise RuntimeError(f"missing dyld trace: {stderr_path}")
    matches = RUNTIME_PATH_RE.findall(stderr_path.read_text(errors="replace"))
    if not matches:
        raise RuntimeError("DYLD_PRINT_LIBRARIES did not report libcangjie-runtime.dylib")
    expected = spec.runtime_dylib.resolve()
    loaded_paths = [pathlib.Path(match).resolve() for match in matches]
    if expected not in loaded_paths:
        rendered = ", ".join(str(path) for path in loaded_paths)
        raise RuntimeError(
            f"loaded runtime is outside selected SDK: expected={expected}; loaded={rendered}"
        )
    return expected


def write_failure_diagnostics(out_dir: pathlib.Path, headline: str) -> pathlib.Path:
    diagnostics_path = throughput.write_failure_diagnostics(out_dir, headline)
    with diagnostics_path.open("a") as diagnostics:
        for filename in ("probe.stderr.txt", "probe.stdout.txt", "loaded_runtime.txt"):
            diagnostics.write(f"\n[{filename}]\n")
            diagnostics.write(throughput.tail_text_file(out_dir / filename))
            diagnostics.write("\n")
    return diagnostics_path


def resolve_run_timeout_sec(args: argparse.Namespace) -> int:
    return throughput.resolve_run_timeout_sec(args)


def make_result_dir(args: argparse.Namespace) -> pathlib.Path:
    case_dir = pathlib.Path(args.case_dir).resolve()
    result_root = (
        pathlib.Path(args.result_root).resolve()
        if args.result_root
        else case_dir / "result"
    )
    run_count_label = f"{args.runs}run" if args.runs == 1 else f"{args.runs}runs"
    dirname = (
        f"{throughput.timestamp_now()}_{args.tag}_c{args.concurrency}_p{args.payload}_"
        f"{args.duration_ms // 1000}s_heap{args.heap_size}_{run_count_label}"
    )
    result_dir = result_root / dirname
    result_dir.mkdir(parents=True, exist_ok=False)
    return result_dir


def write_run_summary(
    summary_path: pathlib.Path,
    run_index: int,
    start_time: dt.datetime,
    end_time: dt.datetime,
    args: argparse.Namespace,
    spec: SDKSpec,
    port: int,
    rc: int,
    result: throughput.BenchmarkResult | None,
    loaded_runtime: pathlib.Path | None,
    error_message: str | None,
) -> None:
    lines = [
        f"gc_label={spec.label}",
        f"cjc_gc_mode={spec.cjc_gc_mode}",
        f"binary={spec.binary_name}",
        f"run_index={run_index}",
        f"start={start_time.strftime('%Y-%m-%d %H:%M:%S')}",
        f"end={end_time.strftime('%Y-%m-%d %H:%M:%S')}",
        f"sdk_root={spec.root.resolve()}",
        f"compiler={spec.compiler.resolve()}",
        f"stdlib_modules={spec.stdlib_modules.resolve()}",
        f"stdx_dir={spec.stdx_dir.resolve()}",
        f"runtime_expected={spec.runtime_dylib.resolve()}",
        f"runtime_loaded={loaded_runtime if loaded_runtime else '<not-validated>'}",
        f"case_dir={pathlib.Path(args.case_dir).resolve()}",
        f"port={port}",
        f"payload={args.payload}",
        f"concurrency={args.concurrency}",
        f"duration_ms={args.duration_ms}",
        f"heap={args.heap_size}",
        f"run_timeout_sec={resolve_run_timeout_sec(args)}",
        f"compile_and_run_rc={rc}",
    ]
    if error_message:
        lines.append(f"error_message={error_message.replace(chr(10), ' | ')}")
    if result is not None:
        lines.extend(
            [
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


def _aggregate_values(
    results: Sequence[throughput.BenchmarkResult],
) -> tuple[float, int, int, int]:
    throughputs = [result.throughput for result in results]
    return (
        statistics.mean(throughputs),
        min(throughputs),
        max(throughputs),
        sum(result.fail for result in results),
    )


def write_comparison_summary(
    summary_path: pathlib.Path,
    copygc_results: Sequence[throughput.BenchmarkResult],
    msgc_results: Sequence[throughput.BenchmarkResult],
) -> None:
    if not copygc_results or not msgc_results:
        raise ValueError("both CopyGC and MSGC results are required")
    copy_avg, copy_min, copy_max, copy_fail = _aggregate_values(copygc_results)
    msgc_avg, msgc_min, msgc_max, msgc_fail = _aggregate_values(msgc_results)
    delta = msgc_avg - copy_avg
    percent = delta / copy_avg * 100 if copy_avg else float("inf")
    lines = [
        f"runs_per_sdk={len(copygc_results)}",
        f"copygc_throughput_avg={copy_avg:.2f}",
        f"copygc_throughput_min={copy_min}",
        f"copygc_throughput_max={copy_max}",
        f"copygc_fail_total={copy_fail}",
        f"msgc_throughput_avg={msgc_avg:.2f}",
        f"msgc_throughput_min={msgc_min}",
        f"msgc_throughput_max={msgc_max}",
        f"msgc_fail_total={msgc_fail}",
        f"msgc_vs_copygc_delta={delta:.2f}",
        f"msgc_vs_copygc_percent={percent:.2f}",
    ]
    summary_path.write_text("\n".join(lines) + "\n")


def run_single_benchmark(
    args: argparse.Namespace,
    spec: SDKSpec,
    run_index: int,
    out_dir: pathlib.Path,
    port: int,
) -> tuple[
    int,
    throughput.BenchmarkResult | None,
    pathlib.Path | None,
    str | None,
]:
    env = build_run_environment(args, spec, out_dir, port)
    if args.dry_run:
        print(
            f"[dry-run] sdk={spec.label} cjc_gc_mode={spec.cjc_gc_mode} "
            f"binary={spec.binary_name} run={run_index} port={port} out_dir={out_dir}"
        )
        for key in sorted(env):
            print(f"[dry-run] {spec.label}.{key}={env[key]}")
        return 0, None, None, None

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
        child_stdout.write_text(throughput.coerce_subprocess_output(exc.output))
        child_stderr.write_text(throughput.coerce_subprocess_output(exc.stderr))
        diagnostics_path = write_failure_diagnostics(
            out_dir,
            f"{spec.label} benchmark timed out after {timeout_sec}s",
        )
        error = throughput.build_failure_message(
            f"{spec.label} benchmark timed out after {timeout_sec}s",
            diagnostics_path,
            out_dir,
        )
        return throughput.TIMEOUT_EXIT_CODE, None, None, error

    child_stdout.write_text(completed.stdout)
    child_stderr.write_text(completed.stderr)
    result = None
    loaded_runtime = None
    errors: list[str] = []
    stdout_path = out_dir / "run.stdout.txt"
    if stdout_path.exists():
        try:
            result = throughput.parse_benchmark_result(
                stdout_path.read_text(errors="replace")
            )
        except ValueError as exc:
            errors.append(str(exc))
    else:
        errors.append(f"missing benchmark stdout: {stdout_path}")

    try:
        loaded_runtime = validate_loaded_runtime(out_dir / "probe.stderr.txt", spec)
    except RuntimeError as exc:
        errors.append(str(exc))

    if completed.returncode != 0:
        errors.insert(0, f"benchmark exited with rc={completed.returncode}")
    if completed.returncode == 0 and not errors and result is not None:
        return 0, result, loaded_runtime, None

    headline = f"{spec.label} benchmark validation failed: " + "; ".join(errors)
    diagnostics_path = write_failure_diagnostics(out_dir, headline)
    error = throughput.build_failure_message(
        headline,
        diagnostics_path,
        out_dir,
    )
    return completed.returncode or 1, result, loaded_runtime, error


def print_run_result(
    spec: SDKSpec,
    run_index: int,
    result: throughput.BenchmarkResult,
) -> None:
    print(
        f"[{spec.label} run {run_index}] throughput={result.throughput} req/s "
        f"ok={result.ok} fail={result.fail} total={result.total} "
        f"duration={result.duration_ms}ms"
    )


def main(argv: Sequence[str] | None = None) -> int:
    args = parse_args(argv)
    if args.runs < 1:
        raise ValueError("--runs must be >= 1")
    if args.concurrency < 1:
        raise ValueError("--concurrency must be >= 1")
    if args.duration_ms < 1:
        raise ValueError("--duration-ms must be >= 1")
    resolve_run_timeout_sec(args)
    specs = build_sdk_specs(args)
    ensure_input_paths(args, specs)
    result_dir = make_result_dir(args)
    print(f"[info] result_dir={result_dir}")
    for spec in specs:
        print(
            f"[info] {spec.label}: compiler={spec.compiler} "
            f"stdlib={spec.stdlib_modules} runtime={spec.runtime_dylib} "
            f"cjc_gc_mode={spec.cjc_gc_mode}"
        )

    all_results: dict[str, list[throughput.BenchmarkResult]] = {
        spec.label: [] for spec in specs
    }
    for run_index in range(1, args.runs + 1):
        for spec_index, spec in enumerate(specs):
            out_dir = result_dir / spec.label / f"run{run_index}"
            out_dir.mkdir(parents=True, exist_ok=False)
            requested_port = args.port_base + (run_index - 1) * len(specs) + spec_index
            port = throughput.choose_port(requested_port)
            start_time = dt.datetime.now()
            rc, result, loaded_runtime, error_message = run_single_benchmark(
                args,
                spec,
                run_index,
                out_dir,
                port,
            )
            end_time = dt.datetime.now()
            write_run_summary(
                out_dir / "summary.txt",
                run_index,
                start_time,
                end_time,
                args,
                spec,
                port,
                rc,
                result,
                loaded_runtime,
                error_message,
            )
            if args.dry_run:
                continue
            if rc != 0 or result is None:
                print(
                    f"[error] {spec.label} run {run_index} failed:\n"
                    f"{error_message or f'see {out_dir}'}",
                    file=sys.stderr,
                )
                return rc or 1
            all_results[spec.label].append(result)
            print_run_result(spec, run_index, result)

    if args.dry_run:
        return 0

    for spec in specs:
        mode_summary = result_dir / spec.label / "summary.txt"
        throughput.write_aggregate_summary(mode_summary, all_results[spec.label])
    write_comparison_summary(
        result_dir / "comparison_summary.txt",
        all_results["copygc"],
        all_results["msgc"],
    )
    copy_avg = statistics.mean(
        result.throughput for result in all_results["copygc"]
    )
    msgc_avg = statistics.mean(result.throughput for result in all_results["msgc"])
    delta_percent = (msgc_avg - copy_avg) / copy_avg * 100 if copy_avg else float("inf")
    print(
        f"[summary] CopyGC={copy_avg:.2f} req/s MSGC={msgc_avg:.2f} req/s "
        f"MSGC-vs-CopyGC={delta_percent:.2f}%"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
