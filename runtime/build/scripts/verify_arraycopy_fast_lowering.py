#!/usr/bin/env python3

from __future__ import annotations

import argparse
import dataclasses
import datetime as dt
import os
import pathlib
import re
import shlex
import stat
import subprocess
import sys
from typing import Sequence


FAST_REF_RE = re.compile(r"CJ_MCC_ArrayCopyRefFast_MSGC\b")
FAST_STRUCT_RE = re.compile(r"CJ_MCC_ArrayCopyStructFast_MSGC\b")
ARRAYCOPY_SIGNAL_PATTERNS = (
    re.compile(r"CJ_MCC_ArrayCopyRefFast_MSGC\b"),
    re.compile(r"CJ_MCC_ArrayCopyStructFast_MSGC\b"),
    re.compile(r"CJ_MCC_ArrayCopyRef\b"),
    re.compile(r"CJ_MCC_ArrayCopyStruct\b"),
    re.compile(r"cj_array_copy_ref\b"),
    re.compile(r"cj_array_copy_struct\b"),
    re.compile(r"llvm\.cj\.array\.copy\.ref\b"),
    re.compile(r"llvm\.cj\.array\.copy\.struct\b"),
)


@dataclasses.dataclass(frozen=True)
class WrapperHits:
    ref_count: int
    struct_count: int

    @property
    def total_count(self) -> int:
        return self.ref_count + self.struct_count

    @property
    def has_fast_wrappers(self) -> bool:
        return self.total_count > 0


@dataclasses.dataclass(frozen=True)
class ProbeResult:
    channel: str
    command: Sequence[str]
    rc: int
    artifact_path: pathlib.Path
    hits: WrapperHits | None
    error: str | None = None
    stdout_path: pathlib.Path | None = None
    stderr_path: pathlib.Path | None = None
    signal_count: int | None = None

    @property
    def status(self) -> str:
        if self.error is not None or self.rc != 0 or self.hits is None:
            return "error"
        if self.hits.has_fast_wrappers:
            return "fail"
        return "pass"


def repo_root_for_script(script_path: pathlib.Path) -> pathlib.Path:
    return script_path.resolve().parents[2]


def default_sdk_root() -> pathlib.Path:
    return pathlib.Path(
        "/Users/lipeisheng/code/cangjie/cangjie/"
        "cangjie_compiler_region_mark/output/msgc"
    )


def default_compiler_root(sdk_root: pathlib.Path) -> pathlib.Path:
    if sdk_root.name in {"msgc", "copygc"} and sdk_root.parent.name == "output":
        return sdk_root.resolve().parents[1]
    return sdk_root.resolve().parent


def default_envsetup(sdk_root: pathlib.Path) -> pathlib.Path:
    return sdk_root / "envsetup.sh"


def default_stdx_dir(sdk_root: pathlib.Path) -> pathlib.Path:
    return sdk_root / "darwin_aarch64_cjnative" / "dynamic" / "stdx"


def default_llc_path(sdk_root: pathlib.Path) -> pathlib.Path:
    return sdk_root / "third_party" / "llvm" / "bin" / "llc"


def default_llvm_test(compiler_root: pathlib.Path) -> pathlib.Path:
    return (
        compiler_root
        / "third_party"
        / "llvm-project"
        / "llvm"
        / "test"
        / "CodeGen"
        / "AArch64"
        / "CangjieGC"
        / "msgc-writebarrier-lowering.ll"
    )


def default_case27_source(script_path: pathlib.Path) -> pathlib.Path:
    return (
        repo_root_for_script(script_path)
        / "MSGC_RESULT"
        / "Result"
        / "case27_http_complex_json"
        / "benchmark.cj"
    )


def default_result_root(script_path: pathlib.Path) -> pathlib.Path:
    return repo_root_for_script(script_path) / "output" / "verify_arraycopy_fast_lowering"


def build_default_link_options(stdx_dir: pathlib.Path) -> list[str]:
    return [
        str(stdx_dir / "libstdx.net.http.dylib"),
        str(stdx_dir / "libstdx.encoding.json.dylib"),
    ]


def parse_args(argv: Sequence[str] | None = None) -> argparse.Namespace:
    script_path = pathlib.Path(__file__)
    sdk_root = default_sdk_root()
    compiler_root = default_compiler_root(sdk_root)
    parser = argparse.ArgumentParser(
        description=(
            "Verify whether the current LLVM/cjc build still lowers array copy "
            "to CJ_MCC_ArrayCopyRefFast_MSGC or CJ_MCC_ArrayCopyStructFast_MSGC."
        )
    )
    parser.add_argument("--sdk-root", default=str(sdk_root))
    parser.add_argument("--compiler-root", default=str(compiler_root))
    parser.add_argument("--envsetup", default=str(default_envsetup(sdk_root)))
    parser.add_argument("--stdx-dir", default=str(default_stdx_dir(sdk_root)))
    parser.add_argument("--llc", default=str(default_llc_path(sdk_root)))
    parser.add_argument("--llvm-test", default=str(default_llvm_test(compiler_root)))
    parser.add_argument("--cjc-source", default=str(default_case27_source(script_path)))
    parser.add_argument("--import-path", action="append", default=None)
    parser.add_argument("--library-path", action="append", default=None)
    parser.add_argument("--link-option", action="append", default=None)
    parser.add_argument("--result-root", default=str(default_result_root(script_path)))
    parser.add_argument("--tag", default="arraycopy_fast_probe")
    parser.add_argument("--triple", default="arm64-apple-macosx12.0.0")
    parser.add_argument("--skip-llc", action="store_true")
    parser.add_argument("--skip-cjc", action="store_true")
    parser.add_argument("--dry-run", action="store_true")
    return parser.parse_args(argv)


def normalize_args(args: argparse.Namespace) -> argparse.Namespace:
    args.sdk_root = pathlib.Path(args.sdk_root).expanduser().resolve()
    args.compiler_root = pathlib.Path(args.compiler_root).expanduser().resolve()
    args.envsetup = pathlib.Path(args.envsetup).expanduser().resolve()
    args.stdx_dir = pathlib.Path(args.stdx_dir).expanduser().resolve()
    args.llc = pathlib.Path(args.llc).expanduser().resolve()
    args.llvm_test = pathlib.Path(args.llvm_test).expanduser().resolve()
    args.cjc_source = pathlib.Path(args.cjc_source).expanduser().resolve()
    args.result_root = pathlib.Path(args.result_root).expanduser().resolve()
    if args.import_path is None:
        args.import_path = [str(args.stdx_dir)]
    if args.library_path is None:
        args.library_path = [str(args.stdx_dir)]
    if args.link_option is None:
        args.link_option = build_default_link_options(args.stdx_dir)
    if args.skip_llc and args.skip_cjc:
        raise ValueError("at least one probe must remain enabled")
    return args


def parse_wrapper_hits(text: str) -> WrapperHits:
    return WrapperHits(
        ref_count=len(FAST_REF_RE.findall(text)),
        struct_count=len(FAST_STRUCT_RE.findall(text)),
    )


def count_arraycopy_signal_tokens(text: str) -> int:
    return sum(len(pattern.findall(text)) for pattern in ARRAYCOPY_SIGNAL_PATTERNS)


def format_wrapper_hits(hits: WrapperHits) -> str:
    return (
        f"ref={hits.ref_count} "
        f"struct={hits.struct_count} "
        f"total={hits.total_count}"
    )


def compute_exit_code(results: Sequence[ProbeResult]) -> int:
    if any(result.status == "error" for result in results):
        return 2
    if any(result.status == "fail" for result in results):
        return 1
    return 0


def ensure_input_paths(args: argparse.Namespace) -> None:
    required: list[pathlib.Path] = []
    if not args.skip_llc:
        required.extend([args.llc, args.llvm_test])
    if not args.skip_cjc:
        required.extend([args.envsetup, args.stdx_dir, args.cjc_source])
        required.extend(pathlib.Path(path) for path in args.import_path)
        required.extend(pathlib.Path(path) for path in args.library_path)
        required.extend(pathlib.Path(path) for path in args.link_option)
    missing = [str(path) for path in required if not path.exists()]
    if missing:
        raise FileNotFoundError("required input path missing:\n" + "\n".join(missing))


def make_result_dir(args: argparse.Namespace) -> pathlib.Path:
    stamp = dt.datetime.now().strftime("%Y%m%d_%H%M%S")
    result_dir = args.result_root / f"{stamp}_{args.tag}"
    result_dir.mkdir(parents=True, exist_ok=True)
    return result_dir


def build_cjc_frontend_args(args: argparse.Namespace) -> list[str]:
    frontend_args: list[str] = []
    for import_path in args.import_path:
        frontend_args.extend(["--import-path", import_path])
    for library_path in args.library_path:
        frontend_args.extend(["-L", library_path])
    for link_option in args.link_option:
        frontend_args.extend(["--link-option", link_option])
    return frontend_args


def render_cjc_child_script(extra_args: Sequence[str] | None = None) -> str:
    rendered_args = ""
    for arg in extra_args or []:
        rendered_args += " \\\n  " + shlex.quote(arg)
    return f"""\
set -euo pipefail
export DYLD_LIBRARY_PATH=""
source "$ENVSETUP" >/dev/null 2>&1
export PATH=/opt/homebrew/bin:/opt/homebrew/opt/llvm@16/bin:/opt/homebrew/opt/m4/bin:$PATH
export DYLD_LIBRARY_PATH="$STDX_DIR:${{DYLD_LIBRARY_PATH:-}}"
cd "$OUT_DIR"
rm -f benchmark benchmark.bc benchmark.o benchmark.opt.bc benchmark.s compile.stdout.txt compile.stderr.txt
cjc "$CJ_SOURCE"{rendered_args} \\
  --save-temps "$OUT_DIR" -j1 -o benchmark > compile.stdout.txt 2> compile.stderr.txt
"""


def build_cjc_environment(args: argparse.Namespace, out_dir: pathlib.Path) -> dict[str, str]:
    return {
        "HOME": os.environ.get("HOME", ""),
        "TERM": os.environ.get("TERM", "xterm-256color"),
        "PATH": "/usr/bin:/bin:/usr/sbin:/sbin:/opt/homebrew/bin",
        "ENVSETUP": str(args.envsetup),
        "STDX_DIR": str(args.stdx_dir),
        "OUT_DIR": str(out_dir),
        "CJ_SOURCE": str(args.cjc_source),
    }


def write_command_file(path: pathlib.Path, command: Sequence[str]) -> None:
    path.write_text(shlex.join(str(part) for part in command) + "\n", encoding="utf-8")


def read_text_if_exists(path: pathlib.Path) -> str:
    if not path.exists():
        return ""
    return path.read_text(encoding="utf-8", errors="replace")


def error_excerpt(path: pathlib.Path) -> str:
    content = read_text_if_exists(path).strip()
    if not content:
        return ""
    lines = content.splitlines()
    return "\n".join(lines[-10:])


def collect_strings_output(path: pathlib.Path) -> str:
    if not path.exists():
        return ""
    completed = subprocess.run(
        ["/usr/bin/strings", str(path)],
        text=True,
        capture_output=True,
        check=False,
    )
    if completed.returncode != 0:
        return ""
    return completed.stdout


def run_llc_probe(args: argparse.Namespace, out_dir: pathlib.Path) -> ProbeResult:
    artifact_path = out_dir / "llc_probe.s"
    stderr_path = out_dir / "llc_probe.stderr.txt"
    command = [
        str(args.llc),
        "--cangjie-pipeline",
        f"-mtriple={args.triple}",
        "-o",
        "-",
    ]
    write_command_file(out_dir / "llc_probe.command.txt", command)
    ll_input = args.llvm_test.read_text(encoding="utf-8")
    completed = subprocess.run(
        command,
        input=ll_input,
        text=True,
        capture_output=True,
        check=False,
    )
    artifact_path.write_text(completed.stdout, encoding="utf-8")
    stderr_path.write_text(completed.stderr, encoding="utf-8")
    if completed.returncode != 0:
        return ProbeResult(
            channel="llc",
            command=command,
            rc=completed.returncode,
            artifact_path=artifact_path,
            hits=None,
            error=error_excerpt(stderr_path) or "llc command failed",
            stderr_path=stderr_path,
        )
    return ProbeResult(
        channel="llc",
        command=command,
        rc=completed.returncode,
        artifact_path=artifact_path,
        hits=parse_wrapper_hits(completed.stdout),
        stderr_path=stderr_path,
    )


def run_cjc_probe(args: argparse.Namespace, out_dir: pathlib.Path) -> ProbeResult:
    compile_stdout_path = out_dir / "compile.stdout.txt"
    compile_stderr_path = out_dir / "compile.stderr.txt"
    shell_stdout_path = out_dir / "cjc_probe.stdout.txt"
    shell_stderr_path = out_dir / "cjc_probe.stderr.txt"
    artifact_path = out_dir / "benchmark.s"
    child_script_path = out_dir / "run_cjc_probe.sh"
    child_script = render_cjc_child_script(build_cjc_frontend_args(args))
    child_script_path.write_text(child_script, encoding="utf-8")
    child_script_path.chmod(child_script_path.stat().st_mode | stat.S_IXUSR)
    command = ["bash", str(child_script_path)]
    write_command_file(out_dir / "cjc_probe.command.txt", command)
    completed = subprocess.run(
        command,
        text=True,
        capture_output=True,
        env=build_cjc_environment(args, out_dir),
        check=False,
    )
    shell_stdout_path.write_text(completed.stdout, encoding="utf-8")
    shell_stderr_path.write_text(completed.stderr, encoding="utf-8")
    if completed.returncode != 0:
        return ProbeResult(
            channel="cjc",
            command=command,
            rc=completed.returncode,
            artifact_path=artifact_path,
            hits=None,
            error=error_excerpt(compile_stderr_path) or error_excerpt(shell_stderr_path) or "cjc compile failed",
            stdout_path=compile_stdout_path,
            stderr_path=compile_stderr_path,
        )
    if not artifact_path.exists():
        return ProbeResult(
            channel="cjc",
            command=command,
            rc=completed.returncode,
            artifact_path=artifact_path,
            hits=None,
            error="cjc probe finished without producing benchmark.s",
            stdout_path=compile_stdout_path,
            stderr_path=compile_stderr_path,
        )
    signal_text_parts = [read_text_if_exists(artifact_path)]
    for bitcode_path in (out_dir / "benchmark.bc", out_dir / "benchmark.opt.bc"):
        signal_text_parts.append(collect_strings_output(bitcode_path))
    signal_text = "\n".join(part for part in signal_text_parts if part)
    signal_count = count_arraycopy_signal_tokens(signal_text)
    if signal_count == 0:
        return ProbeResult(
            channel="cjc",
            command=command,
            rc=completed.returncode,
            artifact_path=artifact_path,
            hits=parse_wrapper_hits(read_text_if_exists(artifact_path)),
            error=(
                "cjc source did not expose any array-copy symbol or intrinsic; "
                "use --cjc-source with a stronger reproducer"
            ),
            stdout_path=compile_stdout_path,
            stderr_path=compile_stderr_path,
            signal_count=0,
        )
    return ProbeResult(
        channel="cjc",
        command=command,
        rc=completed.returncode,
        artifact_path=artifact_path,
        hits=parse_wrapper_hits(read_text_if_exists(artifact_path)),
        stdout_path=compile_stdout_path,
        stderr_path=compile_stderr_path,
        signal_count=signal_count,
    )


def summarize_result(result: ProbeResult) -> list[str]:
    prefix = f"[{result.channel.upper()}]"
    lines = [
        f"{prefix} status={result.status}",
        f"{prefix} command={shlex.join(str(part) for part in result.command)}",
        f"{prefix} artifact={result.artifact_path}",
        f"{prefix} rc={result.rc}",
    ]
    if result.hits is not None:
        lines.append(f"{prefix} wrappers={format_wrapper_hits(result.hits)}")
    if result.signal_count is not None:
        lines.append(f"{prefix} arraycopy_signal_count={result.signal_count}")
    if result.stdout_path is not None:
        lines.append(f"{prefix} stdout={result.stdout_path}")
    if result.stderr_path is not None:
        lines.append(f"{prefix} stderr={result.stderr_path}")
    if result.error is not None:
        lines.append(f"{prefix} error={result.error}")
    return lines


def write_summary(path: pathlib.Path, args: argparse.Namespace, results: Sequence[ProbeResult]) -> None:
    lines = [
        f"time={dt.datetime.now().strftime('%Y-%m-%d %H:%M:%S')}",
        f"sdk_root={args.sdk_root}",
        f"compiler_root={args.compiler_root}",
        f"envsetup={args.envsetup}",
        f"stdx_dir={args.stdx_dir}",
        f"llvm_test={args.llvm_test}",
        f"cjc_source={args.cjc_source}",
        f"triple={args.triple}",
        f"exit_code={compute_exit_code(results)}",
    ]
    for result in results:
        lines.extend(summarize_result(result))
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def print_dry_run(result_dir: pathlib.Path, args: argparse.Namespace) -> None:
    print(f"result_dir={result_dir}")
    if not args.skip_llc:
        command = [
            str(args.llc),
            "--cangjie-pipeline",
            f"-mtriple={args.triple}",
            "-o",
            "-",
        ]
        print(f"[LLC] command={shlex.join(command)}")
        print(f"[LLC] llvm_test={args.llvm_test}")
    if not args.skip_cjc:
        compile_args = [
            "cjc",
            str(args.cjc_source),
            *build_cjc_frontend_args(args),
            "--save-temps",
            "<RESULT_DIR>",
            "-j1",
            "-o",
            "benchmark",
        ]
        print(f"[CJC] envsetup={args.envsetup}")
        print(f"[CJC] stdx_dir={args.stdx_dir}")
        print(f"[CJC] command={shlex.join(compile_args)}")


def main(argv: Sequence[str] | None = None) -> int:
    try:
        args = normalize_args(parse_args(argv))
        ensure_input_paths(args)
        result_dir = make_result_dir(args)
        if args.dry_run:
            print_dry_run(result_dir, args)
            return 0
        results: list[ProbeResult] = []
        if not args.skip_llc:
            results.append(run_llc_probe(args, result_dir))
        if not args.skip_cjc:
            results.append(run_cjc_probe(args, result_dir))
        write_summary(result_dir / "summary.txt", args, results)
        for result in results:
            for line in summarize_result(result):
                print(line)
        print(f"[OVERALL] exit_code={compute_exit_code(results)}")
        print(f"[OVERALL] summary={result_dir / 'summary.txt'}")
        return compute_exit_code(results)
    except Exception as exc:
        print(f"[ERROR] {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
