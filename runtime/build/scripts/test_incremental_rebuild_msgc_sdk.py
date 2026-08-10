#!/usr/bin/env python3

from __future__ import annotations

import contextlib
import io
import pathlib
import tempfile
import unittest
from unittest import mock

import incremental_rebuild_msgc_sdk as target


def make_config(root: pathlib.Path, *, dry_run: bool = False) -> target.FullBuildConfig:
    return target.FullBuildConfig(
        compiler_root=root / "cangjie_compiler",
        runtime_root=root / "cangjie_runtime_MSGC" / "runtime",
        stdlib_root=root / "cangjie_runtime_MSGC" / "stdlib",
        stdx_root=root / "cangjie_stdx",
        sdk_root=root / "cangjie_compiler" / "output" / "msgc",
        build_type="release",
        version="0.0.1",
        jobs=8,
        openssl_lib=root / "openssl" / "lib",
        gc_mode=target.GCMode.MSGC,
        dry_run=dry_run,
    )


def make_host() -> target.HostLayout:
    return target.HostLayout(
        os_name="darwin",
        arch="aarch64",
        sdk_platform_name="darwin_aarch64_cjnative",
    )


class IncrementalStagePlanTests(unittest.TestCase):
    def test_compiler_is_incremental_and_other_components_install_sequentially(self) -> None:
        config = make_config(pathlib.Path("/tmp/cangjie-incremental-msgc-sdk-test"))

        stages = target.build_incremental_stage_plan(config, make_host())

        self.assertEqual(
            [(stage.component, stage.action) for stage in stages],
            [
                ("compiler", "build"),
                ("compiler-release", "build"),
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
            ],
        )
        self.assertNotIn(
            ("compiler", "clean"),
            [(stage.component, stage.action) for stage in stages],
        )
        release_stage = next(
            stage for stage in stages if stage.component == "compiler-release"
        )
        self.assertEqual(release_stage.cwd, config.compiler_root / "build" / "build")
        self.assertEqual(release_stage.command[-2:], ["--target", "cjc-release"])

    def test_msgc_mode_is_passed_to_runtime_stdlib_and_stdx(self) -> None:
        config = make_config(pathlib.Path("/tmp/cangjie-incremental-msgc-sdk-test"))

        stages = target.build_incremental_stage_plan(config, make_host())
        commands = {(stage.component, stage.action): stage.command for stage in stages}

        self.assertIn("marksweep", commands[("runtime", "build")])
        self.assertIn(
            "--build-args=--gc-mode=marksweep",
            commands[("stdlib", "build")],
        )
        self.assertIn(
            "--build-args=--gc-mode=marksweep",
            commands[("stdx", "build")],
        )


class IncrementalPathDefaultsTests(unittest.TestCase):
    def parse_for_host(self, host: target.HostLayout) -> target.FullBuildConfig:
        with mock.patch.object(
            target.full_builder.sdk_helpers,
            "resolve_default_openssl_lib",
            return_value=pathlib.Path("/tmp/openssl/lib"),
        ), mock.patch.object(
            target.full_builder.sdk_helpers,
            "detect_host_layout",
            return_value=host,
        ):
            return target.parse_args([])

    def test_darwin_keeps_existing_msgc_stdx_default(self) -> None:
        config = self.parse_for_host(make_host())

        self.assertEqual(
            config.stdx_root,
            pathlib.Path(
                "/Users/lipeisheng/code/cangjie/cangjie/cangjie_stdx"
            ).resolve(),
        )

    def test_linux_uses_cmsgc_stdx_default(self) -> None:
        config = self.parse_for_host(
            target.HostLayout(
                os_name="linux",
                arch="aarch64",
                sdk_platform_name="linux_aarch64_cjnative",
            )
        )

        self.assertEqual(
            config.stdx_root,
            pathlib.Path(
                "/home/lipeisheng/cangjie/cangjie_stdx_CMSGC"
            ).resolve(),
        )


class CompilerBuildCacheTests(unittest.TestCase):
    def test_relocated_cache_resets_compiler_build_and_keeps_sdk(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = pathlib.Path(temp_dir)
            config = make_config(root)
            cache = config.compiler_root / "build" / "build" / "CMakeCache.txt"
            cache.parent.mkdir(parents=True)
            cache.write_text(
                "CMAKE_CACHEFILE_DIR:INTERNAL=/old/compiler/build/build\n"
                "CMAKE_HOME_DIRECTORY:INTERNAL=/old/compiler\n"
            )
            config.sdk_root.mkdir(parents=True)
            sdk_marker = config.sdk_root / "keep.txt"
            sdk_marker.write_text("keep\n")

            target.reset_relocated_compiler_build(config)

            self.assertFalse((config.compiler_root / "build").exists())
            self.assertEqual(sdk_marker.read_text(), "keep\n")

    def test_cache_for_current_compiler_keeps_incremental_build(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = pathlib.Path(temp_dir)
            config = make_config(root)
            build_root = config.compiler_root / "build"
            cache = build_root / "build" / "CMakeCache.txt"
            cache.parent.mkdir(parents=True)
            cache.write_text(
                f"CMAKE_CACHEFILE_DIR:INTERNAL={cache.parent}\n"
                f"CMAKE_HOME_DIRECTORY:INTERNAL={config.compiler_root}\n"
            )
            marker = build_root / "keep.txt"
            marker.write_text("keep\n")

            target.reset_relocated_compiler_build(config)

            self.assertEqual(marker.read_text(), "keep\n")

    def test_compiler_release_stage_removes_stale_install_artifact(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = pathlib.Path(temp_dir)
            config = make_config(root)
            release_compiler = (
                config.compiler_root / "build" / "build" / "bin" / "cjc-release"
            )
            release_compiler.parent.mkdir(parents=True)
            release_compiler.write_text("stale\n")

            target.prepare_compiler_release(config)

            self.assertFalse(release_compiler.exists())


class IncrementalDryRunTests(unittest.TestCase):
    def test_dry_run_keeps_existing_sdk_and_reports_each_install_before_next_clean(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = pathlib.Path(temp_dir)
            config = make_config(root, dry_run=True)
            for path in (
                config.compiler_root,
                config.runtime_root,
                config.stdlib_root,
                config.stdx_root,
                config.openssl_lib,
            ):
                path.mkdir(parents=True, exist_ok=True)
            config.sdk_root.mkdir(parents=True)
            marker = config.sdk_root / "keep.txt"
            marker.write_text("keep\n")

            output = io.StringIO()
            with contextlib.redirect_stdout(output):
                target.execute_incremental_build(config, make_host())

            text = output.getvalue()
            self.assertEqual(marker.read_text(), "keep\n")
            self.assertNotIn("[reset]", text)
            self.assertLess(text.index("== compiler install =="), text.index("== runtime clean =="))
            self.assertLess(text.index("== runtime install =="), text.index("== stdlib clean =="))
            self.assertLess(text.index("== stdlib install =="), text.index("== stdx clean =="))
            self.assertIn("== sync stdx into SDK ==", text)

    def test_runtime_stages_use_validated_darwin_build_tools(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = pathlib.Path(temp_dir)
            config = make_config(root, dry_run=True)
            for path in (
                config.compiler_root,
                config.runtime_root,
                config.stdlib_root,
                config.stdx_root,
                config.openssl_lib,
            ):
                path.mkdir(parents=True, exist_ok=True)
            runtime_environment = {"PATH": "/llvm16/bin:/usr/bin"}

            with mock.patch.object(
                target.runtime_output,
                "build_subprocess_environment",
                return_value=runtime_environment,
            ), mock.patch.object(target.sdk_helpers, "run_command") as run_command:
                target.execute_incremental_build(config, make_host())

            runtime_calls = [
                call
                for call in run_command.call_args_list
                if call.kwargs["cwd"] == config.runtime_root
            ]
            self.assertEqual(len(runtime_calls), 3)
            self.assertTrue(
                all(
                    call.kwargs["environment"] is runtime_environment
                    for call in runtime_calls
                )
            )


if __name__ == "__main__":
    unittest.main()
