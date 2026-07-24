import importlib.util
import pathlib
import sys
import unittest


SCRIPT_PATH = (
    pathlib.Path(__file__).resolve().parents[1]
    / "build"
    / "scripts"
    / "rebuild_sdk_and_stdx.py"
)


def load_script_module():
    spec = importlib.util.spec_from_file_location("rebuild_sdk_and_stdx", SCRIPT_PATH)
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


class RebuildSdkAndStdxScriptTest(unittest.TestCase):
    def test_resolve_repo_layout_uses_current_workspace_defaults(self):
        module = load_script_module()

        layout = module.resolve_repo_layout(SCRIPT_PATH)

        self.assertEqual(
            layout.workspace_root,
            pathlib.Path("/Users/lipeisheng/code/cangjie/cangjie"),
        )
        self.assertEqual(
            layout.compiler_root,
            pathlib.Path("/Users/lipeisheng/code/cangjie/cangjie/cangjie_compiler"),
        )
        self.assertEqual(
            layout.runtime_root,
            pathlib.Path(
                "/Users/lipeisheng/code/cangjie/cangjie/cangjie_runtime_MSGC/runtime"
            ),
        )
        self.assertEqual(
            layout.stdlib_root,
            pathlib.Path(
                "/Users/lipeisheng/code/cangjie/cangjie/cangjie_runtime_MSGC/stdlib"
            ),
        )
        self.assertEqual(
            layout.stdx_root,
            pathlib.Path("/Users/lipeisheng/code/cangjie/cangjie/cangjie_stdx"),
        )

    def test_build_plan_uses_incremental_cjc_and_full_msgc_runtime_rebuild(self):
        module = load_script_module()

        layout = module.resolve_repo_layout(SCRIPT_PATH)
        host = module.detect_host_layout(system_name="Darwin", machine_name="arm64")
        config = module.BuildConfig(
            compiler_root=layout.compiler_root,
            runtime_root=layout.runtime_root,
            stdlib_root=layout.stdlib_root,
            stdx_root=layout.stdx_root,
            sdk_root=layout.compiler_root / "output",
            build_type="release",
            version="0.0.1",
            jobs=16,
            openssl_lib=pathlib.Path("/opt/homebrew/opt/openssl@3/lib"),
            run_verify=False,
            dry_run=False,
        )

        stages = module.build_stage_plan(config, host)
        commands = [stage.command for stage in stages]
        envsetups = [stage.envsetup for stage in stages]

        self.assertEqual(
            commands[0],
            [
                "python3",
                "build.py",
                "build",
                "-t",
                "release",
                "--product",
                "cjc",
                "--no-tests",
                "-j",
                "16",
            ],
        )
        self.assertEqual(
            commands[1],
            [
                "python3",
                "build.py",
                "install",
                "--prefix",
                str(layout.compiler_root / "output"),
            ],
        )
        self.assertEqual(commands[2], ["python3", "build.py", "clean"])
        self.assertEqual(
            commands[3],
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
        )
        self.assertEqual(
            commands[4],
            [
                "python3",
                "build.py",
                "install",
                "--prefix",
                str(layout.runtime_root / "output" / "common"),
            ],
        )
        self.assertEqual(commands[5], ["python3", "build.py", "clean"])
        self.assertEqual(
            commands[6],
            [
                "python3",
                "build.py",
                "build",
                "-t",
                "release",
                "--target-lib",
                str(layout.runtime_root / "output"),
                "--target-lib",
                "/opt/homebrew/opt/openssl@3/lib",
                "-j",
                "16",
            ],
        )
        self.assertEqual(
            commands[7],
            [
                "python3",
                "build.py",
                "install",
                "--prefix",
                str(layout.stdlib_root / "output"),
            ],
        )
        self.assertEqual(commands[8], ["python3", "build.py", "clean"])
        self.assertEqual(
            commands[9],
            [
                "python3",
                "build.py",
                "build",
                "-t",
                "release",
                "--target-lib",
                "/opt/homebrew/opt/openssl@3/lib",
                "--include",
                str(layout.compiler_root / "include"),
                "--jobs",
                "16",
            ],
        )
        self.assertEqual(
            commands[10],
            [
                "python3",
                "build.py",
                "install",
                "--prefix",
                str(layout.stdx_root / "target"),
            ],
        )
        expected_envsetup = layout.compiler_root / "output" / "envsetup.sh"
        self.assertEqual(envsetups[0], None)
        self.assertEqual(envsetups[1], None)
        self.assertEqual(envsetups[2], None)
        self.assertEqual(envsetups[3], None)
        self.assertEqual(envsetups[4], None)
        self.assertEqual(envsetups[5], None)
        self.assertEqual(envsetups[6], expected_envsetup)
        self.assertEqual(envsetups[7], expected_envsetup)
        self.assertEqual(envsetups[8], None)
        self.assertEqual(envsetups[9], expected_envsetup)
        self.assertEqual(envsetups[10], expected_envsetup)

    def test_copy_plan_overlays_runtime_stdlib_and_stdx_into_sdk_output(self):
        module = load_script_module()

        layout = module.resolve_repo_layout(SCRIPT_PATH)
        host = module.detect_host_layout(system_name="Darwin", machine_name="arm64")
        config = module.BuildConfig(
            compiler_root=layout.compiler_root,
            runtime_root=layout.runtime_root,
            stdlib_root=layout.stdlib_root,
            stdx_root=layout.stdx_root,
            sdk_root=layout.compiler_root / "output",
            build_type="release",
            version="0.0.1",
            jobs=16,
            openssl_lib=pathlib.Path("/opt/homebrew/opt/openssl@3/lib"),
            run_verify=False,
            dry_run=False,
        )

        copy_plan = module.build_copy_plan(config, host)

        self.assertEqual(
            copy_plan,
            [
                (
                    layout.runtime_root
                    / "output"
                    / "common"
                    / "darwin_release_aarch64"
                    / "lib",
                    layout.compiler_root / "output" / "lib",
                ),
                (
                    layout.runtime_root
                    / "output"
                    / "common"
                    / "darwin_release_aarch64"
                    / "runtime",
                    layout.compiler_root / "output" / "runtime",
                ),
                (
                    layout.stdlib_root / "output" / "lib",
                    layout.compiler_root / "output" / "lib",
                ),
                (
                    layout.stdlib_root / "output" / "modules",
                    layout.compiler_root / "output" / "modules",
                ),
                (
                    layout.stdlib_root / "output" / "runtime",
                    layout.compiler_root / "output" / "runtime",
                ),
                (
                    layout.stdx_root / "target" / "darwin_aarch64_cjnative",
                    layout.compiler_root / "output" / "darwin_aarch64_cjnative",
                ),
            ],
        )

    def test_runtime_relayout_plan_bridges_runtime_install_layout_for_stdlib(self):
        module = load_script_module()

        layout = module.resolve_repo_layout(SCRIPT_PATH)
        host = module.detect_host_layout(system_name="Darwin", machine_name="arm64")
        config = module.BuildConfig(
            compiler_root=layout.compiler_root,
            runtime_root=layout.runtime_root,
            stdlib_root=layout.stdlib_root,
            stdx_root=layout.stdx_root,
            sdk_root=layout.compiler_root / "output",
            build_type="release",
            version="0.0.1",
            jobs=16,
            openssl_lib=pathlib.Path("/opt/homebrew/opt/openssl@3/lib"),
            run_verify=False,
            dry_run=False,
        )

        relayout_plan = module.build_runtime_relayout_plan(config, host)

        runtime_common_root = layout.runtime_root / "output" / "common"
        runtime_common_dir = runtime_common_root / "darwin_release_aarch64"
        runtime_platform_dir = pathlib.Path("darwin_aarch64_cjnative")
        self.assertEqual(
            relayout_plan,
            [
                (
                    runtime_common_root / "lib" / runtime_platform_dir,
                    runtime_common_dir / "lib" / runtime_platform_dir,
                ),
                (
                    runtime_common_root / "runtime" / "lib" / runtime_platform_dir,
                    runtime_common_dir / "runtime" / "lib" / runtime_platform_dir,
                ),
            ],
        )

    def test_wrap_with_envsetup_sources_compiler_sdk_environment(self):
        module = load_script_module()

        envsetup = pathlib.Path("/tmp/fake-sdk/envsetup.sh")
        wrapped = module.wrap_with_envsetup(
            ["python3", "build.py", "build", "-t", "release"],
            envsetup,
        )

        self.assertEqual(wrapped[0:2], ["bash", "-lc"])
        self.assertIn(f'source "{envsetup}" >/dev/null 2>&1', wrapped[2])
        self.assertIn("python3 build.py build -t release", wrapped[2])

    def test_codesign_targets_cover_sdk_runtime_and_stdx_dylibs_on_macos(self):
        module = load_script_module()

        layout = module.resolve_repo_layout(SCRIPT_PATH)
        host = module.detect_host_layout(system_name="Darwin", machine_name="arm64")
        config = module.BuildConfig(
            compiler_root=layout.compiler_root,
            runtime_root=layout.runtime_root,
            stdlib_root=layout.stdlib_root,
            stdx_root=layout.stdx_root,
            sdk_root=layout.compiler_root / "output",
            build_type="release",
            version="0.0.1",
            jobs=16,
            openssl_lib=pathlib.Path("/opt/homebrew/opt/openssl@3/lib"),
            run_verify=False,
            dry_run=False,
        )

        targets = module.build_codesign_targets(config, host)

        self.assertEqual(
            targets,
            [
                layout.compiler_root
                / "output"
                / "runtime"
                / "lib"
                / "darwin_aarch64_cjnative",
                layout.compiler_root
                / "output"
                / "darwin_aarch64_cjnative",
                layout.stdx_root / "target" / "darwin_aarch64_cjnative",
            ],
        )


if __name__ == "__main__":
    unittest.main()
