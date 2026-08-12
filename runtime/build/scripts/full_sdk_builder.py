#!/usr/bin/env python3

"""Shared full-build orchestration for isolated CopyGC and MSGC SDKs."""

from __future__ import annotations

import argparse
import contextlib
import os
import pathlib
import shutil
import subprocess
import sys
from dataclasses import dataclass
from enum import Enum
from typing import Iterator, Sequence

import rebuild_sdk_and_stdx as sdk_helpers


RepoLayout = sdk_helpers.RepoLayout
HostLayout = sdk_helpers.HostLayout

DEFAULT_MSGC_COMPILER_ROOT_DARWIN = pathlib.Path(
    "/Users/lipeisheng/code/cangjie/cangjie/"
    "cangjie_compiler_region_mark"
)

DEFAULT_MSGC_COMPILER_ROOT_LINUX = pathlib.Path(
    "/home/lipeisheng/cangjie/cangjie_compiler_CMSGC"
)

DEFAULT_MSGC_STDX_ROOT_DARWIN = pathlib.Path(
    "/Users/lipeisheng/code/cangjie/cangjie/cangjie_stdx_region_mark"
)

DEFAULT_MSGC_STDX_ROOT_LINUX = pathlib.Path(
    "/home/lipeisheng/cangjie/cangjie_stdx_CMSGC"
)


class GCMode(Enum):
    MSGC = ("msgc", "marksweep", "marksweep")
    COPYGC = ("copygc", "copygc", "copygc")

    def __init__(self, output_name: str, runtime_name: str, cjc_name: str) -> None:
        self.output_name = output_name
        self.runtime_name = runtime_name
        self.cjc_name = cjc_name

    @property
    def sibling(self) -> "GCMode":
        return GCMode.COPYGC if self is GCMode.MSGC else GCMode.MSGC


@dataclass(frozen=True)
class FullBuildConfig:
    compiler_root: pathlib.Path
    runtime_root: pathlib.Path
    stdlib_root: pathlib.Path
    stdx_root: pathlib.Path
    sdk_root: pathlib.Path
    build_type: str
    version: str
    jobs: int
    openssl_lib: pathlib.Path
    gc_mode: GCMode
    dry_run: bool


@dataclass(frozen=True)
class BuildStage:
    component: str
    action: str
    cwd: pathlib.Path
    command: list[str]
    envsetup: pathlib.Path | None = None


def default_msgc_compiler_root(host: HostLayout) -> pathlib.Path:
    if host.os_name == "darwin":
        return DEFAULT_MSGC_COMPILER_ROOT_DARWIN
    if host.os_name == "linux":
        return DEFAULT_MSGC_COMPILER_ROOT_LINUX
    raise ValueError(f"unsupported host system for MSGC SDK: {host.os_name}")


def default_msgc_stdx_root(host: HostLayout) -> pathlib.Path:
    if host.os_name == "darwin":
        return DEFAULT_MSGC_STDX_ROOT_DARWIN
    if host.os_name == "linux":
        return DEFAULT_MSGC_STDX_ROOT_LINUX
    raise ValueError(f"unsupported host system for MSGC SDK: {host.os_name}")


def default_config_for_mode(
    gc_mode: GCMode,
    layout: RepoLayout,
    openssl_lib: pathlib.Path,
    *,
    build_type: str = "release",
    version: str = "0.0.1",
    jobs: int | None = None,
    dry_run: bool = False,
) -> FullBuildConfig:
    return FullBuildConfig(
        compiler_root=layout.compiler_root,
        runtime_root=layout.runtime_root,
        stdlib_root=layout.stdlib_root,
        stdx_root=layout.stdx_root,
        sdk_root=layout.compiler_root / "output" / gc_mode.output_name,
        build_type=build_type,
        version=version,
        jobs=jobs if jobs is not None else max(os.cpu_count() or 1, 1),
        openssl_lib=openssl_lib,
        gc_mode=gc_mode,
        dry_run=dry_run,
    )


def build_stage_plan(config: FullBuildConfig, host: HostLayout) -> list[BuildStage]:
    del host  # Reserved for future platform-specific stage arguments.
    envsetup = config.sdk_root / "envsetup.sh"
    cjc_gc_arg = f"--build-args=--gc-mode={config.gc_mode.cjc_name}"

    clean_stages = [
        BuildStage(
            component="compiler",
            action="clean",
            cwd=config.compiler_root,
            command=["python3", "build.py", "clean"],
        ),
        BuildStage(
            component="runtime",
            action="clean",
            cwd=config.runtime_root,
            command=["python3", "build.py", "clean"],
        ),
        BuildStage(
            component="stdlib",
            action="clean",
            cwd=config.stdlib_root,
            command=["python3", "build.py", "clean"],
        ),
        BuildStage(
            component="stdx",
            action="clean",
            cwd=config.stdx_root,
            command=["python3", "build.py", "clean"],
        ),
    ]

    build_stages = [
        BuildStage(
            component="compiler",
            action="build",
            cwd=config.compiler_root,
            command=[
                "python3",
                "build.py",
                "build",
                "-t",
                config.build_type,
                "--product",
                "cjc",
                "--no-tests",
                "-j",
                str(config.jobs),
                "-v",
                config.version,
            ],
        ),
        BuildStage(
            component="compiler-llvm",
            action="build",
            cwd=sdk_helpers.compiler_cjnative_build_dir(config),
            command=[
                "cmake",
                "--build",
                ".",
                "--parallel",
                str(config.jobs),
                "--target",
                "LLVM",
                "llc",
                "LTO",
            ],
        ),
        BuildStage(
            component="compiler",
            action="install",
            cwd=config.compiler_root,
            command=[
                "python3",
                "build.py",
                "install",
                "--prefix",
                str(config.sdk_root),
            ],
        ),
        BuildStage(
            component="runtime",
            action="build",
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
                config.gc_mode.runtime_name,
                "-v",
                config.version,
            ],
        ),
        BuildStage(
            component="runtime",
            action="install",
            cwd=config.runtime_root,
            command=[
                "python3",
                "build.py",
                "install",
                "--prefix",
                str(config.runtime_root / "output" / "common"),
            ],
        ),
        BuildStage(
            component="stdlib",
            action="build",
            cwd=config.stdlib_root,
            command=[
                "python3",
                "build.py",
                "build",
                "-t",
                config.build_type,
                "--target-lib",
                str(config.runtime_root / "output"),
                "--target-lib",
                str(config.openssl_lib),
                cjc_gc_arg,
                "-j",
                str(config.jobs),
            ],
            envsetup=envsetup,
        ),
        BuildStage(
            component="stdlib",
            action="install",
            cwd=config.stdlib_root,
            command=[
                "python3",
                "build.py",
                "install",
                "--prefix",
                str(config.stdlib_root / "output"),
            ],
            envsetup=envsetup,
        ),
        BuildStage(
            component="stdx",
            action="build",
            cwd=config.stdx_root,
            command=[
                "python3",
                "build.py",
                "build",
                "-t",
                config.build_type,
                "--target-lib",
                str(config.openssl_lib),
                "--include",
                str(config.compiler_root / "include"),
                cjc_gc_arg,
                "--jobs",
                str(config.jobs),
            ],
            envsetup=envsetup,
        ),
        BuildStage(
            component="stdx",
            action="install",
            cwd=config.stdx_root,
            command=[
                "python3",
                "build.py",
                "install",
                "--prefix",
                str(config.stdx_root / "target"),
            ],
            envsetup=envsetup,
        ),
    ]
    return clean_stages + build_stages


def expected_sdk_root(config: FullBuildConfig) -> pathlib.Path:
    return (
        config.compiler_root.resolve()
        / "output"
        / config.gc_mode.output_name
    )


def validate_sdk_output(config: FullBuildConfig) -> None:
    actual = config.sdk_root.resolve()
    expected = expected_sdk_root(config)
    forbidden = {
        pathlib.Path("/").resolve(),
        pathlib.Path.home().resolve(),
        config.compiler_root.resolve(),
        (config.compiler_root / "output").resolve(),
    }
    if actual != expected or actual in forbidden:
        raise ValueError(
            "refusing to reset SDK output: "
            f"expected {expected} for {config.gc_mode.output_name}, got {actual}"
        )


def reset_sdk_output(config: FullBuildConfig) -> None:
    validate_sdk_output(config)
    print(f"[reset] {config.sdk_root}", flush=True)
    if config.dry_run:
        return
    sdk_helpers.make_path_writable(config.sdk_root)
    if config.sdk_root.exists():
        shutil.rmtree(config.sdk_root)
    config.sdk_root.mkdir(parents=True, exist_ok=True)


@contextlib.contextmanager
def preserve_sibling_sdk(config: FullBuildConfig) -> Iterator[None]:
    validate_sdk_output(config)
    sibling = config.compiler_root / "output" / config.gc_mode.sibling.output_name
    protected = (
        config.compiler_root
        / f".full-sdk-builder-preserve-{config.gc_mode.sibling.output_name}"
    )
    print(f"[preserve] {sibling} -> {protected}", flush=True)
    if protected.exists():
        raise FileExistsError(
            f"refusing to overwrite preserved sibling SDK: {protected}"
        )
    if config.dry_run or not sibling.exists():
        yield
        return

    sibling.rename(protected)
    try:
        yield
    finally:
        sibling.parent.mkdir(parents=True, exist_ok=True)
        if sibling.exists():
            raise FileExistsError(
                f"cannot restore preserved sibling SDK; target exists: {sibling}"
            )
        protected.rename(sibling)
        print(f"[restore] {protected} -> {sibling}", flush=True)


def ensure_paths_exist(config: FullBuildConfig) -> None:
    required = {
        "compiler_root": config.compiler_root,
        "runtime_root": config.runtime_root,
        "stdlib_root": config.stdlib_root,
        "stdx_root": config.stdx_root,
        "openssl_lib": config.openssl_lib,
    }
    for name, path in required.items():
        if not path.exists():
            raise FileNotFoundError(f"{name} does not exist: {path}")
    if config.jobs < 1:
        raise ValueError(f"jobs must be positive, got {config.jobs}")
    validate_sdk_output(config)


def _run_stage(config: FullBuildConfig, stage: BuildStage) -> None:
    print(f"\n== {stage.component} {stage.action} ==", flush=True)
    sdk_helpers.run_command(
        stage.command,
        cwd=stage.cwd,
        dry_run=config.dry_run,
        envsetup=stage.envsetup,
    )


def verify_sdk_compiler_gc_mode(config: FullBuildConfig) -> None:
    compiler = config.sdk_root / "bin" / "cjc"
    gc_mode_arg = f"--gc-mode={config.gc_mode.cjc_name}"
    command = [str(compiler), gc_mode_arg, "--version"]
    print(f"[verify] {' '.join(command)}", flush=True)
    if config.dry_run:
        return
    completed = subprocess.run(command, capture_output=True, text=True)
    if completed.returncode != 0:
        details = (completed.stderr or completed.stdout).strip()
        raise RuntimeError(
            f"SDK compiler rejected {gc_mode_arg}: {compiler}"
            + (f"\n{details}" if details else "")
        )


def _after_install(
    config: FullBuildConfig,
    host: HostLayout,
    stage: BuildStage,
) -> None:
    if stage.action != "install":
        return

    if stage.component == "compiler":
        print("\n== verify SDK compiler GC mode ==", flush=True)
        verify_sdk_compiler_gc_mode(config)
        print("\n== sync compiler llvm into SDK ==", flush=True)
        sdk_helpers.sync_compiler_llvm_into_sdk(config, host, config.dry_run)
        if host.os_name == "darwin":
            sdk_helpers.codesign_tree(
                config.sdk_root / "third_party" / "llvm" / "lib",
                config.dry_run,
            )
    elif stage.component == "runtime":
        print("\n== repair and sync runtime into SDK ==", flush=True)
        sdk_helpers.repair_runtime_output_layout(config, host, config.dry_run)
        sdk_helpers.sync_runtime_into_sdk(config, host, config.dry_run)
        if host.os_name == "darwin":
            sdk_helpers.codesign_tree(
                config.sdk_root / "runtime" / "lib" / host.sdk_platform_name,
                config.dry_run,
            )
    elif stage.component == "stdlib":
        print("\n== sync stdlib into SDK ==", flush=True)
        sdk_helpers.sync_stdlib_into_sdk(config, host, config.dry_run)
        if host.os_name == "darwin":
            sdk_helpers.codesign_tree(
                config.sdk_root / "runtime" / "lib" / host.sdk_platform_name,
                config.dry_run,
            )


def execute_full_build(config: FullBuildConfig, host: HostLayout) -> None:
    ensure_paths_exist(config)
    stages = build_stage_plan(config, host)
    clean_stages = [stage for stage in stages if stage.action == "clean"]
    build_stages = [stage for stage in stages if stage.action != "clean"]

    for stage in clean_stages:
        if stage.component == "compiler":
            with preserve_sibling_sdk(config):
                _run_stage(config, stage)
        else:
            _run_stage(config, stage)

    reset_sdk_output(config)

    for stage in build_stages:
        _run_stage(config, stage)
        _after_install(config, host, stage)

    print("\n== overlay outputs into SDK ==", flush=True)
    for src, dst in sdk_helpers.build_copy_plan(config, host):
        sdk_helpers.copy_tree(src, dst, config.dry_run)

    if host.os_name == "darwin":
        print("\n== codesign packaged dylibs ==", flush=True)
        for root in sdk_helpers.build_codesign_targets(config, host):
            sdk_helpers.codesign_tree(root, config.dry_run)


def parse_args_for_mode(
    gc_mode: GCMode,
    argv: Sequence[str] | None = None,
) -> FullBuildConfig:
    layout = sdk_helpers.resolve_repo_layout(pathlib.Path(__file__))
    host = sdk_helpers.detect_host_layout()
    default_compiler_root = (
        default_msgc_compiler_root(host)
        if gc_mode is GCMode.MSGC
        else layout.compiler_root
    )
    default_stdx_root = (
        default_msgc_stdx_root(host)
        if gc_mode is GCMode.MSGC
        else layout.stdx_root
    )
    parser = argparse.ArgumentParser(
        description=(
            f"Fully rebuild the {gc_mode.output_name} SDK without running "
            "post-build validation."
        )
    )
    parser.add_argument("--compiler-root", default=str(default_compiler_root))
    parser.add_argument("--runtime-root", default=str(layout.runtime_root))
    parser.add_argument("--stdlib-root", default=str(layout.stdlib_root))
    parser.add_argument("--stdx-root", default=str(default_stdx_root))
    parser.add_argument(
        "--build-type",
        choices=["release", "debug", "relwithdebinfo"],
        default="release",
    )
    parser.add_argument(
        "--version",
        default=os.environ.get("CANGJIE_VERSION", "0.0.1"),
    )
    parser.add_argument("--jobs", type=int, default=max(os.cpu_count() or 1, 1))
    parser.add_argument(
        "--openssl-lib",
        help="OpenSSL library directory; otherwise use OPENSSL_PATH/Homebrew discovery.",
    )
    parser.add_argument(
        "--dry-run",
        action="store_true",
        help="Print clean/build/copy commands without changing files.",
    )
    args = parser.parse_args(argv)

    openssl_lib = (
        pathlib.Path(args.openssl_lib).expanduser().resolve()
        if args.openssl_lib
        else sdk_helpers.resolve_default_openssl_lib().resolve()
    )
    compiler_root = pathlib.Path(args.compiler_root).expanduser().resolve()
    resolved_layout = RepoLayout(
        workspace_root=compiler_root.parent,
        compiler_root=compiler_root,
        runtime_root=pathlib.Path(args.runtime_root).expanduser().resolve(),
        stdlib_root=pathlib.Path(args.stdlib_root).expanduser().resolve(),
        stdx_root=pathlib.Path(args.stdx_root).expanduser().resolve(),
    )
    return default_config_for_mode(
        gc_mode,
        resolved_layout,
        openssl_lib,
        build_type=args.build_type,
        version=args.version,
        jobs=args.jobs,
        dry_run=args.dry_run,
    )


def main_for_mode(gc_mode: GCMode, argv: Sequence[str] | None = None) -> int:
    try:
        config = parse_args_for_mode(gc_mode, argv)
        host = sdk_helpers.detect_host_layout()
        execute_full_build(config, host)
    except subprocess.CalledProcessError as exc:
        print(
            f"[error] command failed with exit code {exc.returncode}: {exc.cmd}",
            file=sys.stderr,
        )
        return exc.returncode
    except Exception as exc:
        print(f"[error] {exc}", file=sys.stderr)
        return 1

    print("\nBuild completed (no post-build validation was run).", flush=True)
    print(f"GC mode  : {config.gc_mode.output_name}", flush=True)
    print(f"SDK root : {config.sdk_root}", flush=True)
    print(f"Source   : {config.sdk_root / 'envsetup.sh'}", flush=True)
    return 0
