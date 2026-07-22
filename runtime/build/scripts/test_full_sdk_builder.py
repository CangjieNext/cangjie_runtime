#!/usr/bin/env python3

from __future__ import annotations

import pathlib
import shutil
import tempfile
import unittest

import full_sdk_builder as target


def make_layout(root: pathlib.Path) -> target.RepoLayout:
    return target.RepoLayout(
        workspace_root=root,
        compiler_root=root / "cangjie_compiler",
        runtime_root=root / "cangjie_runtime_MSGC" / "runtime",
        stdlib_root=root / "cangjie_runtime_MSGC" / "stdlib",
        stdx_root=root / "cangjie_stdx",
    )


def make_config(root: pathlib.Path, mode: target.GCMode) -> target.FullBuildConfig:
    layout = make_layout(root)
    return target.FullBuildConfig(
        compiler_root=layout.compiler_root,
        runtime_root=layout.runtime_root,
        stdlib_root=layout.stdlib_root,
        stdx_root=layout.stdx_root,
        sdk_root=layout.compiler_root / "output" / mode.output_name,
        build_type="release",
        version="0.0.1",
        jobs=8,
        openssl_lib=root / "openssl" / "lib",
        gc_mode=mode,
        dry_run=False,
    )


def make_host() -> target.HostLayout:
    return target.HostLayout(
        os_name="darwin",
        arch="aarch64",
        sdk_platform_name="darwin_aarch64_cjnative",
    )


class ModeBindingTests(unittest.TestCase):
    def test_msgc_defaults_to_marksweep_and_msgc_output(self) -> None:
        root = pathlib.Path("/tmp/cangjie-full-sdk-test")
        config = target.default_config_for_mode(
            target.GCMode.MSGC,
            make_layout(root),
            openssl_lib=root / "openssl" / "lib",
        )

        self.assertEqual(config.gc_mode.runtime_name, "marksweep")
        self.assertEqual(config.gc_mode.cjc_name, "marksweep")
        self.assertEqual(config.sdk_root, root / "cangjie_compiler" / "output" / "msgc")

    def test_copygc_defaults_to_copygc_and_copygc_output(self) -> None:
        root = pathlib.Path("/tmp/cangjie-full-sdk-test")
        config = target.default_config_for_mode(
            target.GCMode.COPYGC,
            make_layout(root),
            openssl_lib=root / "openssl" / "lib",
        )

        self.assertEqual(config.gc_mode.runtime_name, "copygc")
        self.assertEqual(config.gc_mode.cjc_name, "copygc")
        self.assertEqual(config.sdk_root, root / "cangjie_compiler" / "output" / "copygc")


class StagePlanTests(unittest.TestCase):
    def test_full_build_starts_with_all_component_clean_commands(self) -> None:
        root = pathlib.Path("/tmp/cangjie-full-sdk-test")
        config = make_config(root, target.GCMode.MSGC)

        stages = target.build_stage_plan(config, make_host())
        clean_stages = [stage for stage in stages if stage.action == "clean"]

        self.assertEqual(
            [(stage.component, stage.command) for stage in clean_stages],
            [
                ("compiler", ["python3", "build.py", "clean"]),
                ("runtime", ["python3", "build.py", "clean"]),
                ("stdlib", ["python3", "build.py", "clean"]),
                ("stdx", ["python3", "build.py", "clean"]),
            ],
        )
        self.assertEqual([stage.action for stage in stages[:4]], ["clean"] * 4)

    def test_msgc_mode_reaches_runtime_stdlib_and_stdx(self) -> None:
        root = pathlib.Path("/tmp/cangjie-full-sdk-test")
        config = make_config(root, target.GCMode.MSGC)

        stages = target.build_stage_plan(config, make_host())
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

    def test_copygc_mode_reaches_runtime_stdlib_and_stdx(self) -> None:
        root = pathlib.Path("/tmp/cangjie-full-sdk-test")
        config = make_config(root, target.GCMode.COPYGC)

        stages = target.build_stage_plan(config, make_host())
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


class CompilerProbeTests(unittest.TestCase):
    def test_probe_accepts_selected_gc_mode(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = pathlib.Path(temp_dir)
            config = make_config(root, target.GCMode.MSGC)
            compiler = config.sdk_root / "bin" / "cjc"
            compiler.parent.mkdir(parents=True)
            compiler.write_text(
                "#!/bin/sh\n"
                "test \"$1\" = \"--gc-mode=marksweep\" && "
                "test \"$2\" = \"--version\"\n"
            )
            compiler.chmod(0o755)

            target.verify_sdk_compiler_gc_mode(config)

    def test_probe_reports_release_compiler_rejection(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = pathlib.Path(temp_dir)
            config = make_config(root, target.GCMode.MSGC)
            compiler = config.sdk_root / "bin" / "cjc"
            compiler.parent.mkdir(parents=True)
            compiler.write_text(
                "#!/bin/sh\n"
                "echo \"error: invalid option: '--gc-mode'\" >&2\n"
                "exit 1\n"
            )
            compiler.chmod(0o755)

            with self.assertRaisesRegex(
                RuntimeError,
                "SDK compiler rejected --gc-mode=marksweep",
            ):
                target.verify_sdk_compiler_gc_mode(config)


class OutputResetTests(unittest.TestCase):
    def test_preserve_rejects_leftover_protection_directory(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = pathlib.Path(temp_dir)
            config = make_config(root, target.GCMode.MSGC)
            protected = (
                config.compiler_root / ".full-sdk-builder-preserve-copygc"
            )
            protected.mkdir(parents=True)

            with self.assertRaisesRegex(
                FileExistsError,
                "refusing to overwrite preserved sibling SDK",
            ):
                with target.preserve_sibling_sdk(config):
                    pass

    def test_sibling_sdk_survives_compiler_output_clean(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = pathlib.Path(temp_dir)
            config = make_config(root, target.GCMode.MSGC)
            output_root = config.compiler_root / "output"
            sibling = output_root / "copygc"
            sibling.mkdir(parents=True)
            (sibling / "keep.txt").write_text("keep\n")

            with target.preserve_sibling_sdk(config):
                self.assertFalse(sibling.exists())
                shutil.rmtree(output_root, ignore_errors=True)

            self.assertEqual((sibling / "keep.txt").read_text(), "keep\n")

    def test_reset_removes_only_selected_mode_sdk(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = pathlib.Path(temp_dir)
            config = make_config(root, target.GCMode.MSGC)
            sibling = config.compiler_root / "output" / "copygc"
            config.sdk_root.mkdir(parents=True)
            sibling.mkdir(parents=True)
            (config.sdk_root / "stale.txt").write_text("stale\n")
            (sibling / "keep.txt").write_text("keep\n")

            target.reset_sdk_output(config)

            self.assertTrue(config.sdk_root.is_dir())
            self.assertFalse((config.sdk_root / "stale.txt").exists())
            self.assertEqual((sibling / "keep.txt").read_text(), "keep\n")

    def test_reset_rejects_output_parent(self) -> None:
        root = pathlib.Path("/tmp/cangjie-full-sdk-test")
        config = make_config(root, target.GCMode.MSGC)
        unsafe = target.FullBuildConfig(
            **{
                **config.__dict__,
                "sdk_root": config.compiler_root / "output",
            }
        )

        with self.assertRaisesRegex(ValueError, "refusing to reset SDK output"):
            target.reset_sdk_output(unsafe)

    def test_reset_rejects_other_mode_directory(self) -> None:
        root = pathlib.Path("/tmp/cangjie-full-sdk-test")
        config = make_config(root, target.GCMode.MSGC)
        mismatched = target.FullBuildConfig(
            **{
                **config.__dict__,
                "sdk_root": config.compiler_root / "output" / "copygc",
            }
        )

        with self.assertRaisesRegex(ValueError, "refusing to reset SDK output"):
            target.reset_sdk_output(mismatched)

    def test_dry_run_does_not_mutate_selected_sdk(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = pathlib.Path(temp_dir)
            config = make_config(root, target.GCMode.COPYGC)
            config = target.FullBuildConfig(**{**config.__dict__, "dry_run": True})
            config.sdk_root.mkdir(parents=True)
            marker = config.sdk_root / "keep.txt"
            marker.write_text("keep\n")

            target.reset_sdk_output(config)

            self.assertEqual(marker.read_text(), "keep\n")


class EntryPointTests(unittest.TestCase):
    def test_msgc_entry_point_binds_msgc_mode(self) -> None:
        import rebuild_msgc_sdk

        self.assertIs(rebuild_msgc_sdk.GC_MODE, target.GCMode.MSGC)

    def test_copygc_entry_point_binds_copygc_mode(self) -> None:
        import rebuild_copygc_sdk

        self.assertIs(rebuild_copygc_sdk.GC_MODE, target.GCMode.COPYGC)


if __name__ == "__main__":
    unittest.main()
