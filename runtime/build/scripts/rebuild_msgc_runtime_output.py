#!/usr/bin/env python3

from __future__ import annotations

import argparse
import datetime
import hashlib
import os
import pathlib
import platform
import shlex
import shutil
import stat
import subprocess
from collections.abc import Sequence
from dataclasses import dataclass


DEFAULT_MSGC_SDK_ROOT = pathlib.Path(
    "/Users/lipeisheng/code/cangjie/cangjie/"
    "cangjie_compiler_region_mark/output/msgc"
)


@dataclass(frozen=True)
class RepoLayout:
    workspace_root: pathlib.Path
    compiler_root: pathlib.Path
    runtime_root: pathlib.Path


@dataclass(frozen=True)
class HostLayout:
    os_name: str
    arch: str
    sdk_platform_name: str

    def runtime_common_name(self, build_type: str) -> str:
        return f"{self.os_name}_{build_type}_{self.arch}"


@dataclass(frozen=True)
class BuildConfig:
    runtime_root: pathlib.Path
    sdk_root: pathlib.Path
    build_type: str
    version: str
    skip_clean: bool
    verify_output: bool
    dry_run: bool


@dataclass(frozen=True)
class BuildStage:
    cwd: pathlib.Path
    command: list[str]


@dataclass(frozen=True)
class VerificationItem:
    label: str
    compare_mode: str
    source: pathlib.Path
    installed: pathlib.Path


DARWIN_BUILD_TOOL_DIRS = (
    pathlib.Path("/opt/homebrew/opt/llvm@16/bin"),
    pathlib.Path("/opt/homebrew/opt/m4/bin"),
    pathlib.Path("/opt/homebrew/bin"),
    pathlib.Path("/usr/local/bin"),
)


def build_subprocess_path(
    host: HostLayout,
    current_path: str,
    darwin_tool_dirs: list[pathlib.Path] | tuple[pathlib.Path, ...] | None = None,
) -> str:
    tool_dirs = darwin_tool_dirs if darwin_tool_dirs is not None else DARWIN_BUILD_TOOL_DIRS
    path_entries: list[str] = []
    if host.os_name == "darwin":
        path_entries.extend(str(path) for path in tool_dirs if path.is_dir())
    path_entries.extend(entry for entry in current_path.split(os.pathsep) if entry)
    return os.pathsep.join(dict.fromkeys(path_entries))


def build_subprocess_environment(host: HostLayout, validate_tools: bool = True) -> dict[str, str]:
    environment = os.environ.copy()
    environment["PATH"] = build_subprocess_path(host, environment.get("PATH", ""))
    if not validate_tools:
        return environment

    required_tools = ("cmake", "make", "clang", "clang++")
    missing_tools = [
        tool for tool in required_tools if shutil.which(tool, path=environment["PATH"]) is None
    ]
    if missing_tools:
        raise FileNotFoundError(
            "required runtime build tools are not available in PATH: " + ", ".join(missing_tools)
        )
    for tool in required_tools:
        print(f"[info] {tool}={shutil.which(tool, path=environment['PATH'])}", flush=True)
    return environment


def resolve_repo_layout(script_path: pathlib.Path) -> RepoLayout:
    runtime_root = script_path.resolve().parents[2]
    workspace_root = runtime_root.parents[1]
    return RepoLayout(
        workspace_root=workspace_root,
        compiler_root=workspace_root / "cangjie_compiler",
        runtime_root=runtime_root,
    )


def detect_host_layout(system_name: str | None = None, machine_name: str | None = None) -> HostLayout:
    system_name = (system_name or platform.system()).lower()
    machine_name = (machine_name or platform.machine()).lower()

    if system_name == "darwin":
        os_name = "darwin"
    elif system_name == "linux":
        os_name = "linux"
    else:
        raise ValueError(f"unsupported host system: {system_name}")

    arch_alias = {
        "arm64": "aarch64",
        "aarch64": "aarch64",
        "x86_64": "x86_64",
        "amd64": "x86_64",
        "arm": "arm",
        "arm32": "arm",
    }
    arch = arch_alias.get(machine_name)
    if arch is None:
        raise ValueError(f"unsupported host architecture: {machine_name}")

    return HostLayout(
        os_name=os_name,
        arch=arch,
        sdk_platform_name=f"{os_name}_{arch}_cjnative",
    )


def build_stage_plan(config: BuildConfig) -> list[BuildStage]:
    stages: list[BuildStage] = []
    if not config.skip_clean:
        stages.append(
            BuildStage(
                cwd=config.runtime_root,
                command=["python3", "build.py", "clean"],
            )
        )
    stages.extend(
        [
            BuildStage(
                cwd=config.runtime_root,
                command=[
                    "python3",
                    "build.py",
                    "build",
                    "--target",
                    "native",
                    "--build-type",
                    config.build_type,
                    "--gc-mode",
                    "marksweep",
                    "-v",
                    config.version,
                ],
            ),
            BuildStage(
                cwd=config.runtime_root,
                command=[
                    "python3",
                    "build.py",
                    "install",
                    "--prefix",
                    str(config.runtime_root / "output" / "common"),
                ],
            ),
        ]
    )
    return stages


def build_runtime_relayout_plan(
    config: BuildConfig,
    host: HostLayout,
) -> list[tuple[pathlib.Path, pathlib.Path]]:
    runtime_common_root = config.runtime_root / "output" / "common"
    runtime_common_dir = runtime_common_root / host.runtime_common_name(config.build_type)
    runtime_platform_dir = pathlib.Path(host.sdk_platform_name)

    return [
        (
            runtime_common_root / "lib" / runtime_platform_dir / "cjstart.o",
            runtime_common_dir / "lib" / runtime_platform_dir / "cjstart.o",
        ),
        (
            runtime_common_root / "lib" / runtime_platform_dir / "section.o",
            runtime_common_dir / "lib" / runtime_platform_dir / "section.o",
        ),
        (
            runtime_common_root / "lib" / runtime_platform_dir / "libcangjie-runtime.a",
            runtime_common_dir / "lib" / runtime_platform_dir / "libcangjie-runtime.a",
        ),
        (
            runtime_common_root / "runtime" / "lib" / runtime_platform_dir / "libcangjie-runtime.dylib",
            runtime_common_dir / "runtime" / "lib" / runtime_platform_dir / "libcangjie-runtime.dylib",
        ),
        (
            runtime_common_root / "runtime" / "lib" / runtime_platform_dir / "libcangjie-trace.dylib",
            runtime_common_dir / "runtime" / "lib" / runtime_platform_dir / "libcangjie-trace.dylib",
        ),
    ]


def build_copy_plan(
    config: BuildConfig,
    host: HostLayout,
) -> list[tuple[pathlib.Path, pathlib.Path]]:
    runtime_common_dir = (
        config.runtime_root
        / "output"
        / "common"
        / host.runtime_common_name(config.build_type)
    )
    return [
        (runtime_common_dir / "lib", config.sdk_root / "lib"),
        (runtime_common_dir / "runtime", config.sdk_root / "runtime"),
    ]


def build_codesign_targets(config: BuildConfig, host: HostLayout) -> list[pathlib.Path]:
    if host.os_name != "darwin":
        return []
    return [config.sdk_root / "runtime" / "lib" / host.sdk_platform_name]


def build_verification_plan(config: BuildConfig, host: HostLayout) -> list[VerificationItem]:
    runtime_common_dir = (
        config.runtime_root
        / "output"
        / "common"
        / host.runtime_common_name(config.build_type)
    )
    return [
        VerificationItem(
            label="cjstart.o",
            compare_mode="sha256",
            source=runtime_common_dir / "lib" / host.sdk_platform_name / "cjstart.o",
            installed=config.sdk_root / "lib" / host.sdk_platform_name / "cjstart.o",
        ),
        VerificationItem(
            label="section.o",
            compare_mode="sha256",
            source=runtime_common_dir / "lib" / host.sdk_platform_name / "section.o",
            installed=config.sdk_root / "lib" / host.sdk_platform_name / "section.o",
        ),
        VerificationItem(
            label="libcangjie-runtime.a",
            compare_mode="sha256",
            source=runtime_common_dir / "lib" / host.sdk_platform_name / "libcangjie-runtime.a",
            installed=config.sdk_root / "lib" / host.sdk_platform_name / "libcangjie-runtime.a",
        ),
        VerificationItem(
            label="libcangjie-runtime.dylib",
            compare_mode="symbols",
            source=runtime_common_dir / "runtime" / "lib" / host.sdk_platform_name / "libcangjie-runtime.dylib",
            installed=config.sdk_root / "runtime" / "lib" / host.sdk_platform_name / "libcangjie-runtime.dylib",
        ),
        VerificationItem(
            label="libcangjie-trace.dylib",
            compare_mode="symbols",
            source=runtime_common_dir / "runtime" / "lib" / host.sdk_platform_name / "libcangjie-trace.dylib",
            installed=config.sdk_root / "runtime" / "lib" / host.sdk_platform_name / "libcangjie-trace.dylib",
        ),
    ]


def run_command(
    cmd: list[str],
    cwd: pathlib.Path,
    dry_run: bool,
    environment: dict[str, str] | None = None,
) -> None:
    print(f"[run] cwd={cwd} cmd={shlex.join(cmd)}", flush=True)
    if dry_run:
        return
    subprocess.run(cmd, cwd=cwd, check=True, env=environment)


def make_path_writable(path: pathlib.Path) -> None:
    if not path.exists() or path.is_symlink():
        return

    def add_user_write(target: pathlib.Path) -> None:
        mode = target.stat().st_mode
        extra_bits = stat.S_IWUSR
        if target.is_dir():
            extra_bits |= stat.S_IXUSR
        target.chmod(mode | extra_bits)

    add_user_write(path)
    if path.is_dir():
        for child in path.rglob("*"):
            if child.is_symlink():
                continue
            add_user_write(child)


def copy_file(src: pathlib.Path, dst: pathlib.Path, dry_run: bool) -> None:
    print(f"[copy] {src} -> {dst}", flush=True)
    if dry_run:
        return
    if not src.is_file():
        raise FileNotFoundError(f"copy source file does not exist: {src}")
    make_path_writable(dst)
    dst.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(src, dst)


def copy_tree(src: pathlib.Path, dst: pathlib.Path, dry_run: bool) -> None:
    print(f"[copy] {src} -> {dst}", flush=True)
    if dry_run:
        return
    if not src.exists():
        raise FileNotFoundError(f"copy source does not exist: {src}")
    make_path_writable(dst)
    dst.parent.mkdir(parents=True, exist_ok=True)
    shutil.copytree(src, dst, dirs_exist_ok=True)


def iter_dylibs(root: pathlib.Path) -> list[pathlib.Path]:
    if not root.exists():
        return []
    return sorted(path for path in root.rglob("*.dylib") if path.is_file())


def codesign_tree(root: pathlib.Path, dry_run: bool) -> None:
    dylibs = iter_dylibs(root)
    if not dylibs:
        print(f"[codesign] skip empty tree: {root}", flush=True)
        return
    for dylib in dylibs:
        cmd = ["codesign", "-f", "-s", "-", str(dylib)]
        print(f"[codesign] {shlex.join(cmd)}", flush=True)
        if not dry_run:
            subprocess.run(cmd, check=True)


def repair_runtime_output_layout(config: BuildConfig, host: HostLayout, dry_run: bool) -> None:
    for src, dst in build_runtime_relayout_plan(config, host):
        copy_file(src, dst, dry_run=dry_run)


def verify_outputs(config: BuildConfig, host: HostLayout) -> None:
    required_paths = [
        config.sdk_root / "lib" / host.sdk_platform_name / "cjstart.o",
        config.sdk_root / "lib" / host.sdk_platform_name / "section.o",
        config.sdk_root / "lib" / host.sdk_platform_name / "libcangjie-runtime.a",
        config.sdk_root / "runtime" / "lib" / host.sdk_platform_name / "libcangjie-runtime.dylib",
        config.sdk_root / "runtime" / "lib" / host.sdk_platform_name / "libcangjie-trace.dylib",
    ]
    missing = [str(path) for path in required_paths if not path.exists()]
    if missing:
        raise FileNotFoundError("missing installed runtime outputs:\n" + "\n".join(missing))


def sha256_file(path: pathlib.Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as file:
        for chunk in iter(lambda: file.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def collect_symbol_names(path: pathlib.Path) -> list[str]:
    completed = subprocess.run(
        ["nm", "-P", str(path)],
        check=True,
        capture_output=True,
        text=True,
    )
    symbols: set[str] = set()
    for line in completed.stdout.splitlines():
        stripped = line.strip()
        if not stripped:
            continue
        symbols.add(stripped.split()[0])
    return sorted(symbols)


def verify_codesign(path: pathlib.Path) -> str:
    completed = subprocess.run(
        ["codesign", "-vv", str(path)],
        check=True,
        capture_output=True,
        text=True,
    )
    stderr = completed.stderr.strip()
    return stderr if stderr else "codesign -vv passed"


def format_file_stat(path: pathlib.Path) -> str:
    stat_result = path.stat()
    timestamp = datetime.datetime.fromtimestamp(stat_result.st_mtime).isoformat(timespec="seconds")
    return f"size={stat_result.st_size} mtime={timestamp}"


def verify_installed_runtime_contents(config: BuildConfig, host: HostLayout) -> None:
    for item in build_verification_plan(config, host):
        source_sha = sha256_file(item.source)
        installed_sha = sha256_file(item.installed)
        exact_match = source_sha == installed_sha

        if item.compare_mode == "sha256":
            if not exact_match:
                raise RuntimeError(
                    f"{item.label} sha256 mismatch:\n"
                    f"source={item.source} {source_sha}\n"
                    f"installed={item.installed} {installed_sha}"
                )
            print(
                f"[verify] {item.label}: sha256 match {source_sha[:16]} "
                f"source({format_file_stat(item.source)}) installed({format_file_stat(item.installed)})",
                flush=True,
            )
            continue

        if exact_match:
            print(
                f"[verify] {item.label}: exact bytes match {source_sha[:16]} "
                f"source({format_file_stat(item.source)}) installed({format_file_stat(item.installed)})",
                flush=True,
            )
        else:
            if host.os_name != "darwin":
                raise RuntimeError(
                    f"{item.label} unexpected binary mismatch on {host.os_name}:\n"
                    f"source={item.source} {source_sha}\n"
                    f"installed={item.installed} {installed_sha}"
                )
            if collect_symbol_names(item.source) != collect_symbol_names(item.installed):
                raise RuntimeError(
                    f"{item.label} symbol mismatch after install:\n"
                    f"source={item.source}\n"
                    f"installed={item.installed}"
                )
            print(
                f"[verify] {item.label}: bytes changed after codesign "
                f"(source {source_sha[:16]} != installed {installed_sha[:16]}), "
                f"but symbol set matches and install is accepted; "
                f"source({format_file_stat(item.source)}) installed({format_file_stat(item.installed)})",
                flush=True,
            )

        if host.os_name == "darwin":
            codesign_status = verify_codesign(item.installed)
            print(f"[verify] {item.label}: {codesign_status}", flush=True)


def parse_args(argv: Sequence[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Rebuild MSGC runtime and install it into "
            "cangjie_compiler_region_mark/output/msgc"
        ),
    )
    parser.add_argument(
        "--runtime-root",
        type=pathlib.Path,
        default=None,
        help="path to runtime repo root; defaults to this script's repo",
    )
    parser.add_argument(
        "--sdk-root",
        type=pathlib.Path,
        default=None,
        help=f"target SDK root; defaults to {DEFAULT_MSGC_SDK_ROOT}",
    )
    parser.add_argument(
        "--build-type",
        default="release",
        choices=["release", "debug", "relwithdebinfo"],
        help="runtime build type",
    )
    parser.add_argument(
        "--version",
        default="0.0.1",
        help="runtime SDK version string passed to build.py",
    )
    parser.add_argument(
        "--skip-clean",
        action="store_true",
        help="skip 'python3 build.py clean' before rebuilding runtime",
    )
    parser.add_argument(
        "--skip-verify-output",
        action="store_true",
        help="skip installed-file existence checks after copy",
    )
    parser.add_argument(
        "--dry-run",
        action="store_true",
        help="print planned commands without executing them",
    )
    return parser.parse_args(argv)


def main(argv: Sequence[str] | None = None) -> int:
    args = parse_args(argv)
    script_path = pathlib.Path(__file__)
    repo = resolve_repo_layout(script_path)
    runtime_root = args.runtime_root.resolve() if args.runtime_root else repo.runtime_root
    sdk_root = (
        args.sdk_root.resolve()
        if args.sdk_root
        else DEFAULT_MSGC_SDK_ROOT
    )
    host = detect_host_layout()
    config = BuildConfig(
        runtime_root=runtime_root,
        sdk_root=sdk_root,
        build_type=args.build_type,
        version=args.version,
        skip_clean=args.skip_clean,
        verify_output=not args.skip_verify_output,
        dry_run=args.dry_run,
    )

    print(f"[info] runtime_root={config.runtime_root}", flush=True)
    print(f"[info] sdk_root={config.sdk_root}", flush=True)
    print(f"[info] host={host.os_name}/{host.arch}", flush=True)
    build_environment = build_subprocess_environment(host, validate_tools=not config.dry_run)

    for stage in build_stage_plan(config):
        run_command(
            stage.command,
            cwd=stage.cwd,
            dry_run=config.dry_run,
            environment=build_environment,
        )

    print("\n== repair runtime output layout ==", flush=True)
    repair_runtime_output_layout(config, host, dry_run=config.dry_run)

    print("\n== copy runtime into sdk ==", flush=True)
    for src, dst in build_copy_plan(config, host):
        copy_tree(src, dst, dry_run=config.dry_run)

    if host.os_name == "darwin":
        print("\n== codesign runtime dylibs ==", flush=True)
        for root in build_codesign_targets(config, host):
            codesign_tree(root, dry_run=config.dry_run)

    if config.verify_output and not config.dry_run:
        print("\n== verify installed runtime outputs ==", flush=True)
        verify_outputs(config, host)
        print("\n== verify installed runtime contents ==", flush=True)
        verify_installed_runtime_contents(config, host)

    print("\n[done] MSGC runtime rebuilt and installed into output SDK.", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
