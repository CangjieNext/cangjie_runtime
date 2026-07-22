#!/usr/bin/env python3

from __future__ import annotations

import contextlib
import io
import pathlib
import tempfile
import unittest

import incremental_rebuild_copygc_sdk as target


def make_config(root: pathlib.Path, *, dry_run: bool = False) -> target.FullBuildConfig:
    return target.FullBuildConfig(
        compiler_root=root / "cangjie_compiler",
        runtime_root=root / "cangjie_runtime_MSGC" / "runtime",
        stdlib_root=root / "cangjie_runtime_MSGC" / "stdlib",
        stdx_root=root / "cangjie_stdx",
        sdk_root=root / "cangjie_compiler" / "output" / "copygc",
        build_type="release",
        version="0.0.1",
        jobs=8,
        openssl_lib=root / "openssl" / "lib",
        gc_mode=target.GCMode.COPYGC,
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
        config = make_config(pathlib.Path("/tmp/cangjie-incremental-copygc-sdk-test"))

        stages = target.build_incremental_stage_plan(config, make_host())

        self.assertEqual(
            [(stage.component, stage.action) for stage in stages],
            [
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
            ],
        )
        self.assertNotIn(
            ("compiler", "clean"),
            [(stage.component, stage.action) for stage in stages],
        )

    def test_copygc_mode_is_passed_to_runtime_stdlib_and_stdx(self) -> None:
        config = make_config(pathlib.Path("/tmp/cangjie-incremental-copygc-sdk-test"))

        stages = target.build_incremental_stage_plan(config, make_host())
        commands = {(stage.component, stage.action): stage.command for stage in stages}

        self.assertIn("copygc", commands[("runtime", "build")])
        self.assertIn(
            "--build-args=--gc-mode=copygc",
            commands[("stdlib", "build")],
        )
        self.assertIn(
            "--build-args=--gc-mode=copygc",
            commands[("stdx", "build")],
        )


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


if __name__ == "__main__":
    unittest.main()
