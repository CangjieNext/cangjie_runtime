#!/usr/bin/env python3

from __future__ import annotations

import argparse
import contextlib
import io
import os
import pathlib
import tempfile
import unittest
from unittest import mock

import rebuild_msgc_runtime_output as target


def make_config() -> target.BuildConfig:
    root = pathlib.Path("/tmp/cangjie-rebuild-runtime-test")
    return target.BuildConfig(
        runtime_root=root / "runtime",
        sdk_root=root / "sdk-output",
        build_type="release",
        version="0.0.1",
        skip_clean=False,
        verify_output=True,
        dry_run=False,
    )


def make_host(os_name: str = "darwin", arch: str = "aarch64") -> target.HostLayout:
    return target.HostLayout(
        os_name=os_name,
        arch=arch,
        sdk_platform_name=f"{os_name}_{arch}_cjnative",
    )


class RebuildRuntimePlanTests(unittest.TestCase):
    def test_main_defaults_sdk_root_to_compiler_output_msgc(self) -> None:
        workspace_root = pathlib.Path("/workspace")
        repo = target.RepoLayout(
            workspace_root=workspace_root,
            compiler_root=workspace_root / "cangjie_compiler",
            runtime_root=workspace_root / "cangjie_runtime" / "runtime",
        )
        args = argparse.Namespace(
            runtime_root=None,
            sdk_root=None,
            build_type="release",
            version="0.0.1",
            skip_clean=False,
            skip_verify_output=False,
            dry_run=True,
        )
        output = io.StringIO()

        with mock.patch.object(target, "parse_args", return_value=args), \
             mock.patch.object(target, "resolve_repo_layout", return_value=repo), \
             mock.patch.object(target, "detect_host_layout", return_value=make_host("darwin", "aarch64")), \
             contextlib.redirect_stdout(output):
            self.assertEqual(target.main(), 0)

        self.assertIn(
            (
                "[info] sdk_root=/Users/lipeisheng/code/cangjie/cangjie/"
                "cangjie_compiler_region_mark/output/msgc"
            ),
            output.getvalue(),
        )

    def test_linux_default_sdk_root_uses_cmsgc_output(self) -> None:
        workspace_root = pathlib.Path("/workspace")
        repo = target.RepoLayout(
            workspace_root=workspace_root,
            compiler_root=workspace_root / "cangjie_compiler",
            runtime_root=workspace_root / "cangjie_runtime" / "runtime",
        )
        args = argparse.Namespace(
            runtime_root=None,
            sdk_root=None,
            build_type="release",
            version="0.0.1",
            skip_clean=False,
            skip_verify_output=False,
            dry_run=True,
        )
        output = io.StringIO()
        with mock.patch.object(target, "parse_args", return_value=args), \
             mock.patch.object(target, "resolve_repo_layout", return_value=repo), \
             mock.patch.object(target, "detect_host_layout", return_value=make_host("linux", "aarch64")), \
             contextlib.redirect_stdout(output):
            self.assertEqual(target.main(), 0)
        self.assertIn(
            "[info] sdk_root=/home/lipeisheng/cangjie/cangjie_compiler_CMSGC/output/msgc",
            output.getvalue(),
        )

    def test_build_subprocess_path_prepends_existing_darwin_tool_dirs(self) -> None:
        host = make_host()
        with tempfile.TemporaryDirectory() as temp_dir:
            temp_root = pathlib.Path(temp_dir)
            llvm_bin = temp_root / "llvm@16" / "bin"
            m4_bin = temp_root / "m4" / "bin"
            homebrew_bin = temp_root / "homebrew" / "bin"
            for path in (llvm_bin, m4_bin, homebrew_bin):
                path.mkdir(parents=True)

            result = target.build_subprocess_path(
                host,
                os.pathsep.join(["/usr/bin", "/bin"]),
                darwin_tool_dirs=[llvm_bin, m4_bin, homebrew_bin],
            )

        self.assertEqual(
            result.split(os.pathsep),
            [str(llvm_bin), str(m4_bin), str(homebrew_bin), "/usr/bin", "/bin"],
        )

    def test_build_stage_plan_includes_clean_build_install(self) -> None:
        config = make_config()

        stages = target.build_stage_plan(config)

        self.assertEqual(
            [stage.command for stage in stages],
            [
                ["python3", "build.py", "clean"],
                [
                    "python3",
                    "build.py",
                    "build",
                    "--target",
                    "native",
                    "--build-type",
                    "release",
                    "--gc-mode",
                    "marksweep",
                    "-v",
                    "0.0.1",
                ],
                [
                    "python3",
                    "build.py",
                    "install",
                    "--prefix",
                    str(config.runtime_root / "output" / "common"),
                ],
            ],
        )

    def test_build_runtime_relayout_plan_copies_expected_runtime_artifacts(self) -> None:
        config = make_config()
        host = make_host()

        plan = target.build_runtime_relayout_plan(config, host)
        runtime_common_root = config.runtime_root / "output" / "common"
        runtime_common_dir = runtime_common_root / "darwin_release_aarch64"

        self.assertEqual(
            plan,
            [
                (
                    runtime_common_root / "lib" / "darwin_aarch64_cjnative" / "cjstart.o",
                    runtime_common_dir / "lib" / "darwin_aarch64_cjnative" / "cjstart.o",
                ),
                (
                    runtime_common_root / "lib" / "darwin_aarch64_cjnative" / "section.o",
                    runtime_common_dir / "lib" / "darwin_aarch64_cjnative" / "section.o",
                ),
                (
                    runtime_common_root / "lib" / "darwin_aarch64_cjnative" / "libcangjie-runtime.a",
                    runtime_common_dir / "lib" / "darwin_aarch64_cjnative" / "libcangjie-runtime.a",
                ),
                (
                    runtime_common_root
                    / "runtime"
                    / "lib"
                    / "darwin_aarch64_cjnative"
                    / "libcangjie-runtime.dylib",
                    runtime_common_dir
                    / "runtime"
                    / "lib"
                    / "darwin_aarch64_cjnative"
                    / "libcangjie-runtime.dylib",
                ),
                (
                    runtime_common_root
                    / "runtime"
                    / "lib"
                    / "darwin_aarch64_cjnative"
                    / "libcangjie-trace.dylib",
                    runtime_common_dir
                    / "runtime"
                    / "lib"
                    / "darwin_aarch64_cjnative"
                    / "libcangjie-trace.dylib",
                ),
            ],
        )


    def test_linux_relayout_uses_so_and_skips_macos_only_section_object(self) -> None:
        config = make_config()
        host = make_host(os_name="linux", arch="aarch64")
        root = config.runtime_root / "output" / "common"
        output = root / "linux_release_aarch64"
        platform_dir = "linux_aarch64_cjnative"

        self.assertEqual(
            target.build_runtime_relayout_plan(config, host),
            [
                (root / "lib" / platform_dir / "cjstart.o",
                 output / "lib" / platform_dir / "cjstart.o"),
                (root / "lib" / platform_dir / "libcangjie-runtime.a",
                 output / "lib" / platform_dir / "libcangjie-runtime.a"),
                (root / "runtime" / "lib" / platform_dir / "libcangjie-runtime.so",
                 output / "runtime" / "lib" / platform_dir / "libcangjie-runtime.so"),
                (root / "runtime" / "lib" / platform_dir / "libcangjie-trace.so",
                 output / "runtime" / "lib" / platform_dir / "libcangjie-trace.so"),
            ],
        )

    def test_linux_verification_plan_uses_so_and_omits_section_object(self) -> None:
        plan = target.build_verification_plan(make_config(), make_host("linux", "aarch64"))
        self.assertEqual(
            [item.label for item in plan],
            ["cjstart.o", "libcangjie-runtime.a", "libcangjie-runtime.so", "libcangjie-trace.so"],
        )
    def test_build_copy_plan_copies_runtime_lib_and_runtime_tree_into_sdk(self) -> None:
        config = make_config()
        host = make_host()

        plan = target.build_copy_plan(config, host)

        self.assertEqual(
            plan,
            [
                (
                    config.runtime_root / "output" / "common" / "darwin_release_aarch64" / "lib",
                    config.sdk_root / "lib",
                ),
                (
                    config.runtime_root / "output" / "common" / "darwin_release_aarch64" / "runtime",
                    config.sdk_root / "runtime",
                ),
            ],
        )

    def test_build_codesign_targets_only_runtime_sdk_tree_on_darwin(self) -> None:
        config = make_config()
        host = make_host()

        targets = target.build_codesign_targets(config, host)

        self.assertEqual(
            targets,
            [config.sdk_root / "runtime" / "lib" / "darwin_aarch64_cjnative"],
        )

    def test_build_verification_plan_uses_exact_match_for_static_outputs_and_symbol_match_for_dylibs(self) -> None:
        config = make_config()
        host = make_host()

        plan = target.build_verification_plan(config, host)
        items = [
            (item.label, item.compare_mode, item.source, item.installed)
            for item in plan
        ]
        runtime_common_dir = config.runtime_root / "output" / "common" / "darwin_release_aarch64"

        self.assertEqual(
            items,
            [
                (
                    "cjstart.o",
                    "sha256",
                    runtime_common_dir / "lib" / "darwin_aarch64_cjnative" / "cjstart.o",
                    config.sdk_root / "lib" / "darwin_aarch64_cjnative" / "cjstart.o",
                ),
                (
                    "section.o",
                    "sha256",
                    runtime_common_dir / "lib" / "darwin_aarch64_cjnative" / "section.o",
                    config.sdk_root / "lib" / "darwin_aarch64_cjnative" / "section.o",
                ),
                (
                    "libcangjie-runtime.a",
                    "sha256",
                    runtime_common_dir / "lib" / "darwin_aarch64_cjnative" / "libcangjie-runtime.a",
                    config.sdk_root / "lib" / "darwin_aarch64_cjnative" / "libcangjie-runtime.a",
                ),
                (
                    "libcangjie-runtime.dylib",
                    "symbols",
                    runtime_common_dir
                    / "runtime"
                    / "lib"
                    / "darwin_aarch64_cjnative"
                    / "libcangjie-runtime.dylib",
                    config.sdk_root
                    / "runtime"
                    / "lib"
                    / "darwin_aarch64_cjnative"
                    / "libcangjie-runtime.dylib",
                ),
                (
                    "libcangjie-trace.dylib",
                    "symbols",
                    runtime_common_dir
                    / "runtime"
                    / "lib"
                    / "darwin_aarch64_cjnative"
                    / "libcangjie-trace.dylib",
                    config.sdk_root
                    / "runtime"
                    / "lib"
                    / "darwin_aarch64_cjnative"
                    / "libcangjie-trace.dylib",
                ),
            ],
        )


if __name__ == "__main__":
    unittest.main()
