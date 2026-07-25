#!/usr/bin/env python3

from __future__ import annotations

import contextlib
import io
import pathlib
import stat
import tempfile
import unittest
from unittest import mock

import rebuild_sdk_and_stdx as target


def make_config() -> target.BuildConfig:
    root = pathlib.Path("/tmp/cangjie-rebuild-sdk-test")
    return make_config_at(root)


def make_config_at(root: pathlib.Path) -> target.BuildConfig:
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

    def test_linux_sync_plan_discovers_versioned_llvm_shared_libraries(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            config = make_config_at(pathlib.Path(temp_dir))
            host = make_host(os_name="linux", arch="aarch64")
            cjnative_build = target.compiler_cjnative_build_dir(config)
            lib_dir = cjnative_build / "lib"
            lib_dir.mkdir(parents=True)
            (lib_dir / "libLLVM-15.so").write_text("llvm\n")
            (lib_dir / "libLTO.so.15").write_text("lto\n")
            (lib_dir / "libLTO.so").symlink_to("libLTO.so.15")

            plan = target.build_compiler_llvm_sync_plan(config, host)

            self.assertEqual(
                [src.name for src, _ in plan],
                ["bin", "libLLVM-15.so", "libLTO.so.15", "libLTO.so"],
            )
            self.assertEqual(
                [dst.name for _, dst in plan],
                ["bin", "libLLVM-15.so", "libLTO.so.15", "libLTO.so"],
            )

    def test_linux_sync_preserves_shared_library_symlinks(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            config = make_config_at(pathlib.Path(temp_dir))
            host = make_host(os_name="linux", arch="aarch64")
            cjnative_build = target.compiler_cjnative_build_dir(config)
            (cjnative_build / "bin").mkdir(parents=True)
            lib_dir = cjnative_build / "lib"
            lib_dir.mkdir()
            (lib_dir / "libLLVM-15.so").write_text("llvm\n")
            (lib_dir / "libLTO.so.15").write_text("lto\n")
            (lib_dir / "libLTO.so").symlink_to("libLTO.so.15")
            sdk_lib_dir = config.sdk_root / "third_party" / "llvm" / "lib"
            sdk_lib_dir.mkdir(parents=True)
            (sdk_lib_dir / "libLTO.so").write_text("stale regular file\n")

            target.sync_compiler_llvm_into_sdk(config, host, dry_run=False)

            self.assertEqual((sdk_lib_dir / "libLLVM-15.so").read_text(), "llvm\n")
            self.assertEqual((sdk_lib_dir / "libLTO.so.15").read_text(), "lto\n")
            self.assertTrue((sdk_lib_dir / "libLTO.so").is_symlink())
            self.assertEqual(
                (sdk_lib_dir / "libLTO.so").readlink(),
                pathlib.Path("libLTO.so.15"),
            )

    def test_linux_sync_dry_run_does_not_require_built_libraries(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            config = make_config_at(pathlib.Path(temp_dir))
            host = make_host(os_name="linux", arch="aarch64")
            output = io.StringIO()

            with contextlib.redirect_stdout(output):
                target.sync_compiler_llvm_into_sdk(config, host, dry_run=True)

            self.assertIn("libLLVM*.so*", output.getvalue())
            self.assertIn("libLTO*.so*", output.getvalue())

    def test_build_codesign_targets_include_sdk_llvm_lib_on_darwin(self) -> None:
        config = make_config()
        host = make_host()

        targets = target.build_codesign_targets(config, host)

        self.assertIn(config.sdk_root / "third_party" / "llvm" / "lib", targets)


class RuntimeRelayoutTests(unittest.TestCase):
    def test_linux_relayout_and_sdk_sync_work_without_section_object(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            config = make_config_at(pathlib.Path(temp_dir))
            host = make_host(os_name="linux", arch="aarch64")
            runtime_common_root = config.runtime_root / "output" / "common"
            platform = host.sdk_platform_name
            installed_lib = runtime_common_root / "lib" / platform
            installed_runtime = runtime_common_root / "runtime" / "lib" / platform
            installed_lib.mkdir(parents=True)
            installed_runtime.mkdir(parents=True)

            (installed_lib / "cjstart.o").write_text("cjstart\n")
            (installed_lib / "libcangjie-runtime.a").write_text("static\n")
            (installed_lib / "cjld.lds").write_text("linker-script\n")
            (installed_runtime / "libcangjie-runtime.so").write_text("runtime\n")
            (installed_runtime / "libcangjie-trace.so").write_text("trace\n")

            target.repair_runtime_output_layout(config, host, dry_run=False)
            target.sync_runtime_into_sdk(config, host, dry_run=False)

            repaired_root = (
                runtime_common_root / host.runtime_common_name(config.build_type)
            )
            repaired_lib = repaired_root / "lib" / platform
            repaired_runtime = repaired_root / "runtime" / "lib" / platform
            self.assertEqual((repaired_lib / "cjstart.o").read_text(), "cjstart\n")
            self.assertEqual(
                (repaired_lib / "libcangjie-runtime.a").read_text(),
                "static\n",
            )
            self.assertEqual(
                (repaired_lib / "cjld.lds").read_text(),
                "linker-script\n",
            )
            self.assertFalse((repaired_lib / "section.o").exists())
            self.assertEqual(
                (repaired_runtime / "libcangjie-runtime.so").read_text(),
                "runtime\n",
            )
            self.assertEqual(
                (repaired_runtime / "libcangjie-trace.so").read_text(),
                "trace\n",
            )
            self.assertEqual(
                (
                    config.sdk_root
                    / "runtime"
                    / "lib"
                    / platform
                    / "libcangjie-runtime.so"
                ).read_text(),
                "runtime\n",
            )
            self.assertEqual(
                (config.sdk_root / "lib" / platform / "cjld.lds").read_text(),
                "linker-script\n",
            )


class OpenSSLLibraryDiscoveryTests(unittest.TestCase):
    def test_linux_aarch64_discovers_debian_multiarch_lib_dir(self) -> None:
        expected = pathlib.Path("/usr/lib/aarch64-linux-gnu")

        with mock.patch.object(
            pathlib.Path,
            "is_dir",
            autospec=True,
            side_effect=lambda path: path == expected,
        ):
            actual = target.resolve_default_openssl_lib(
                system_name="linux",
                machine_name="aarch64",
                multiarch_name="aarch64-linux-gnu",
                env={},
            )

        self.assertEqual(actual, expected)

    def test_linux_x86_64_discovers_debian_multiarch_lib_dir(self) -> None:
        expected = pathlib.Path("/usr/lib/x86_64-linux-gnu")

        with mock.patch.object(
            pathlib.Path,
            "is_dir",
            autospec=True,
            side_effect=lambda path: path == expected,
        ):
            actual = target.resolve_default_openssl_lib(
                system_name="linux",
                machine_name="x86_64",
                multiarch_name="x86_64-linux-gnu",
                env={},
            )

        self.assertEqual(actual, expected)


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
