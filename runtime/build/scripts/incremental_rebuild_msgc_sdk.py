#!/usr/bin/env python3

"""Incrementally rebuild CJC and fully rebuild the remaining MSGC SDK parts.

The compiler build and ``cangjie_compiler_region_mark/output/msgc`` are
preserved unless the compiler CMake cache belongs to another checkout. Runtime,
stdlib, and stdx are each cleaned, rebuilt, installed, and synchronized into
the MSGC SDK before the next component starts.
"""

from __future__ import annotations

import pathlib
import shutil
import subprocess
import sys
from typing import Sequence

import full_sdk_builder as full_builder
import rebuild_sdk_and_stdx as sdk_helpers
import rebuild_msgc_runtime_output as runtime_output


BuildStage = full_builder.BuildStage
FullBuildConfig = full_builder.FullBuildConfig
GCMode = full_builder.GCMode
HostLayout = full_builder.HostLayout


_INCREMENTAL_STAGE_ORDER = [
    ("compiler", "build"),
    ("compiler-llvm", "build"),
    ("compiler", "install"),
    ("runtime", "clean"),
    ("runtime", "build"),
    ("runtime", "install"),
    ("stdlib", "clean"),
    ("stdlib", "build"),
    ("stdlib", "install"),
    ("stdx", "clean"),
    ("stdx", "build"),
    ("stdx", "install"),
]


def _cmake_cache_value(cache_path: pathlib.Path, key: str) -> pathlib.Path | None:
    if not cache_path.is_file():
        return None

    prefix = f"{key}:"
    for line in cache_path.read_text(errors="replace").splitlines():
        if line.startswith(prefix):
            _, separator, value = line.partition("=")
            if separator and value:
                return pathlib.Path(value)
    return None


def _same_path(left: pathlib.Path, right: pathlib.Path) -> bool:
    return left.expanduser().resolve() == right.expanduser().resolve()


def reset_relocated_compiler_build(config: FullBuildConfig) -> bool:
    """Discard compiler build metadata configured for a different checkout."""

    compiler_root = config.compiler_root.resolve()
    build_root = compiler_root / "build"
    cache_expectations = (
        (
            build_root / "build" / "CMakeCache.txt",
            compiler_root,
        ),
        (
            sdk_helpers.compiler_cjnative_build_dir(config) / "CMakeCache.txt",
            compiler_root / "third_party" / "llvm-project" / "llvm",
        ),
    )

    mismatches: list[str] = []
    for cache_path, expected_source_dir in cache_expectations:
        cached_source_dir = _cmake_cache_value(cache_path, "CMAKE_HOME_DIRECTORY")
        cached_build_dir = _cmake_cache_value(cache_path, "CMAKE_CACHEFILE_DIR")
        if cached_source_dir is not None and not _same_path(
            cached_source_dir, expected_source_dir
        ):
            mismatches.append(
                f"{cache_path}: source {cached_source_dir} != {expected_source_dir}"
            )
        if cached_build_dir is not None and not _same_path(
            cached_build_dir, cache_path.parent
        ):
            mismatches.append(
                f"{cache_path}: build {cached_build_dir} != {cache_path.parent}"
            )

    if not mismatches:
        return False

    print("\n== reset relocated compiler build ==", flush=True)
    for mismatch in mismatches:
        print(f"[stale-cache] {mismatch}", flush=True)
    print(f"[reset] {build_root}", flush=True)
    if not config.dry_run:
        shutil.rmtree(build_root)
    return True


def prepare_compiler_release(config: FullBuildConfig) -> None:
    """Force relinking the release compiler artifact copied by install."""

    release_compiler = (
        config.compiler_root / "build" / "build" / "bin" / "cjc-release"
    )
    print(f"[remove stale compiler release] {release_compiler}", flush=True)
    if not config.dry_run:
        release_compiler.unlink(missing_ok=True)


def build_incremental_stage_plan(
    config: FullBuildConfig,
    host: HostLayout,
) -> list[BuildStage]:
    """Return the MSGC stages in build-then-install component order."""

    if config.gc_mode is not GCMode.MSGC:
        raise ValueError("incremental MSGC builder requires GCMode.MSGC")

    full_stages = full_builder.build_stage_plan(config, host)
    stage_by_key: dict[tuple[str, str], BuildStage] = {}
    for stage in full_stages:
        key = (stage.component, stage.action)
        if key in stage_by_key:
            raise ValueError(f"duplicate build stage: {key}")
        stage_by_key[key] = stage

    missing = [key for key in _INCREMENTAL_STAGE_ORDER if key not in stage_by_key]
    if missing:
        raise ValueError(f"missing build stages: {missing}")

    stages: list[BuildStage] = []
    for key in _INCREMENTAL_STAGE_ORDER:
        stages.append(stage_by_key[key])
        if key == ("compiler", "build"):
            # Release SDK installation renames cjc-release to bin/cjc. The
            # cjc-release target only has an order-only dependency on cjc, so
            # rebuilding cjc alone does not relink the artifact that install
            # actually copies.
            stages.append(
                BuildStage(
                    component="compiler-release",
                    action="build",
                    cwd=config.compiler_root / "build" / "build",
                    command=[
                        "cmake",
                        "--build",
                        ".",
                        "--parallel",
                        str(config.jobs),
                        "--target",
                        "cjc-release",
                    ],
                )
            )
    return stages


def _run_stage(
    config: FullBuildConfig,
    stage: BuildStage,
    environment: dict[str, str] | None = None,
) -> None:
    print(f"\n== {stage.component} {stage.action} ==", flush=True)
    sdk_helpers.run_command(
        stage.command,
        cwd=stage.cwd,
        dry_run=config.dry_run,
        envsetup=stage.envsetup,
        environment=environment,
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
        full_builder.verify_sdk_compiler_gc_mode(config)
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
    elif stage.component == "stdx":
        print("\n== sync stdx into SDK ==", flush=True)
        sdk_helpers.copy_tree(
            config.stdx_root / "target" / host.sdk_platform_name,
            config.sdk_root / host.sdk_platform_name,
            config.dry_run,
        )
        if host.os_name == "darwin":
            sdk_helpers.codesign_tree(
                config.sdk_root / host.sdk_platform_name,
                config.dry_run,
            )


def execute_incremental_build(
    config: FullBuildConfig,
    host: HostLayout,
) -> None:
    """Run the incremental compiler and sequential full component rebuilds."""

    full_builder.ensure_paths_exist(config)
    reset_relocated_compiler_build(config)
    runtime_environment = runtime_output.build_subprocess_environment(
        host,
        validate_tools=not config.dry_run,
    )
    for stage in build_incremental_stage_plan(config, host):
        if stage.component == "compiler-release":
            prepare_compiler_release(config)
        _run_stage(
            config,
            stage,
            environment=(
                runtime_environment if stage.component == "runtime" else None
            ),
        )
        _after_install(config, host, stage)


def parse_args(argv: Sequence[str] | None = None) -> FullBuildConfig:
    return full_builder.parse_args_for_mode(GCMode.MSGC, argv)


def main(argv: Sequence[str] | None = None) -> int:
    try:
        config = parse_args(argv)
        host = sdk_helpers.detect_host_layout()
        execute_incremental_build(config, host)
    except subprocess.CalledProcessError as exc:
        print(
            f"[error] command failed with exit code {exc.returncode}: {exc.cmd}",
            file=sys.stderr,
        )
        return exc.returncode
    except Exception as exc:
        print(f"[error] {exc}", file=sys.stderr)
        return 1

    print("\nIncremental MSGC SDK build completed.", flush=True)
    print(f"SDK root : {config.sdk_root}", flush=True)
    print(f"Source   : {config.sdk_root / 'envsetup.sh'}", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
