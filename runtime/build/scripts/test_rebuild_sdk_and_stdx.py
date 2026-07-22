#!/usr/bin/env python3

from __future__ import annotations

import pathlib
import stat
import tempfile
import unittest

import rebuild_sdk_and_stdx as target


def make_config() -> target.BuildConfig:
    root = pathlib.Path("/tmp/cangjie-rebuild-sdk-test")
    return target.BuildConfig(
        compiler_root=root / "cangjie_compiler",
        runtime_root=root / "runtime",
        stdlib_root=root / "stdlib",
        stdx_root=root / "stdx",
        sdk_root=root / "sdk-output",
        build_type="release",
        version="0.0.1",
        jobs=8,
        openssl_lib=root / "openssl" / "lib",
        run_verify=True,
        dry_run=False,
    )


def make_host(os_name: str = "darwin", arch: str = "aarch64") -> target.HostLayout:
    return target.HostLayout(
        os_name=os_name,
        arch=arch,
        sdk_platform_name=f"{os_name}_{arch}_cjnative",
    )


class CompilerLLVMSyncPlanTests(unittest.TestCase):
    def test_build_stage_plan_includes_incremental_cjnative_llvm_build_before_install(self) -> None:
        config = make_config()
        host = make_host()

        stages = target.build_stage_plan(config, host)

        compiler_build = stages[0]
        compiler_llvm = stages[1]
        compiler_install = stages[2]
        cjnative_build = (
            config.compiler_root
            / "build"
            / "build"
            / "third_party"
            / "cjnative-prefix"
            / "src"
            / "cjnative-build"
        )

        self.assertEqual(compiler_build.name, "compiler")
        self.assertEqual(compiler_build.command[0:3], ["python3", "build.py", "build"])
        self.assertEqual(compiler_llvm.name, "compiler-llvm")
        self.assertEqual(compiler_llvm.cwd, cjnative_build)
        self.assertEqual(
            compiler_llvm.command,
            [
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
        )
        self.assertEqual(compiler_install.name, "compiler")
        self.assertEqual(compiler_install.command[0:3], ["python3", "build.py", "install"])

    def test_build_compiler_llvm_sync_plan_copies_bin_and_core_dylibs(self) -> None:
        config = make_config()
        host = make_host()

        plan = target.build_compiler_llvm_sync_plan(config, host)

        cjnative_build = (
            config.compiler_root
            / "build"
            / "build"
            / "third_party"
            / "cjnative-prefix"
            / "src"
            / "cjnative-build"
        )
        self.assertEqual(
            plan,
            [
                (
                    cjnative_build / "bin",
                    config.sdk_root / "third_party" / "llvm" / "bin",
                ),
                (
                    cjnative_build / "lib" / "libLLVM.dylib",
                    config.sdk_root / "third_party" / "llvm" / "lib" / "libLLVM.dylib",
                ),
                (
                    cjnative_build / "lib" / "libLTO.dylib",
                    config.sdk_root / "third_party" / "llvm" / "lib" / "libLTO.dylib",
                ),
            ],
        )

    def test_build_codesign_targets_include_sdk_llvm_lib_on_darwin(self) -> None:
        config = make_config()
        host = make_host()

        targets = target.build_codesign_targets(config, host)

        self.assertIn(config.sdk_root / "third_party" / "llvm" / "lib", targets)


class CopyPermissionTests(unittest.TestCase):
    def test_copy_file_overwrites_readonly_destination(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = pathlib.Path(temp_dir)
            src = root / "src.txt"
            dst = root / "dst.txt"
            src.write_text("new\n")
            dst.write_text("old\n")
            dst.chmod(stat.S_IRUSR | stat.S_IRGRP | stat.S_IROTH)

            target.copy_file(src, dst, dry_run=False)

            self.assertEqual(dst.read_text(), "new\n")

    def test_copy_tree_overwrites_readonly_existing_files(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = pathlib.Path(temp_dir)
            src = root / "src"
            dst = root / "dst"
            (src / "bin").mkdir(parents=True)
            (dst / "bin").mkdir(parents=True)
            (src / "bin" / "llc").write_text("new-binary\n")
            stale = dst / "bin" / "llc"
            stale.write_text("old-binary\n")
            stale.chmod(stat.S_IRUSR | stat.S_IRGRP | stat.S_IROTH)

            target.copy_tree(src, dst, dry_run=False)

            self.assertEqual((dst / "bin" / "llc").read_text(), "new-binary\n")


if __name__ == "__main__":
    unittest.main()
