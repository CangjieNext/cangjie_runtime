#!/usr/bin/env python3

from __future__ import annotations

import importlib.util
import io
import os
import pathlib
import sys
import tempfile
import unittest
from unittest import mock


def load_module():
    script_path = pathlib.Path(__file__).with_name("run_case27_sdk_compare.py")
    spec = importlib.util.spec_from_file_location("run_case27_sdk_compare", script_path)
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


class RunCase27SdkCompareTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.module = load_module()

    def test_compare_script_is_importable(self):
        self.assertTrue(callable(self.module.main))

    def test_defaults_select_isolated_output_sdks_and_gc_modes(self):
        args = self.module.parse_args([])
        specs = self.module.build_sdk_specs(args)

        self.assertEqual(
            pathlib.Path(args.msgc_sdk_root),
            pathlib.Path(
                "/Users/lipeisheng/code/cangjie/cangjie/"
                "cangjie_compiler_region_mark/output/msgc"
            ),
        )
        self.assertEqual(
            pathlib.Path(args.copygc_sdk_root),
            pathlib.Path(
                "/Users/lipeisheng/code/cangjie/cangjie/"
                "cangjie_compiler/output/copygc"
            ),
        )
        self.assertEqual([spec.label for spec in specs], ["copygc", "msgc"])
        self.assertEqual([spec.cjc_gc_mode for spec in specs], ["copygc", "marksweep"])
        self.assertEqual(
            [spec.binary_name for spec in specs],
            ["benchmark_copygc", "benchmark_msgc"],
        )

    def test_sdk_spec_derives_all_artifacts_from_each_sdk_root(self):
        args = self.module.parse_args(
            [
                "--copygc-sdk-root",
                "/tmp/copygc",
                "--msgc-sdk-root",
                "/tmp/msgc",
            ]
        )
        copygc, msgc = self.module.build_sdk_specs(args)

        self.assertEqual(copygc.envsetup, pathlib.Path("/tmp/copygc/envsetup.sh"))
        self.assertEqual(
            copygc.stdx_dir,
            pathlib.Path(
                "/tmp/copygc/darwin_aarch64_cjnative/dynamic/stdx"
            ),
        )
        self.assertEqual(
            msgc.runtime_dylib,
            pathlib.Path(
                "/tmp/msgc/runtime/lib/darwin_aarch64_cjnative/"
                "libcangjie-runtime.dylib"
            ),
        )

    def test_child_script_uses_explicit_gc_mode_and_separate_binary(self):
        child_script = self.module.render_child_script()

        self.assertIn('--gc-mode="$CJC_GC_MODE"', child_script)
        self.assertIn('"$SDK_ROOT/bin/cjc" "$BENCH_SRC"', child_script)
        self.assertIn('-o "$BENCH_BINARY"', child_script)
        self.assertIn('./"$BENCH_BINARY"', child_script)
        self.assertIn("DYLD_PRINT_LIBRARIES=1", child_script)
        self.assertIn("probe.stderr.txt", child_script)
        self.assertIn('grep -F "$EXPECTED_RUNTIME" probe.stderr.txt', child_script)
        self.assertLess(
            child_script.index("probe.stderr.txt"),
            child_script.index("> run.stdout.txt"),
        )
        self.assertLess(
            child_script.index('export DYLD_LIBRARY_PATH=""'),
            child_script.index('source "$ENVSETUP"'),
        )

    def test_run_environment_is_mode_specific(self):
        args = self.module.parse_args(
            [
                "--case-dir",
                "/tmp/case27",
                "--copygc-sdk-root",
                "/tmp/copygc",
                "--msgc-sdk-root",
                "/tmp/msgc",
            ]
        )
        copygc, msgc = self.module.build_sdk_specs(args)

        with mock.patch.dict(os.environ, {"cjMSEnumAssist": "0"}, clear=False):
            copy_env = self.module.build_run_environment(
                args, copygc, pathlib.Path("/tmp/out/copygc/run1"), 19127
            )
            msgc_env = self.module.build_run_environment(
                args, msgc, pathlib.Path("/tmp/out/msgc/run1"), 19128
            )

        self.assertEqual(copy_env["CJC_GC_MODE"], "copygc")
        self.assertEqual(copy_env["BENCH_BINARY"], "benchmark_copygc")
        self.assertNotIn("cjMSEnumAssist", copy_env)
        self.assertEqual(msgc_env["CJC_GC_MODE"], "marksweep")
        self.assertEqual(msgc_env["BENCH_BINARY"], "benchmark_msgc")
        self.assertEqual(msgc_env["cjMSEnumAssist"], "0")

    def test_comparison_summary_reports_msgc_relative_to_copygc(self):
        copy_results = [self._result(500), self._result(600, fail=2)]
        msgc_results = [self._result(600), self._result(720, fail=1)]

        with tempfile.TemporaryDirectory() as tmpdir:
            summary_path = pathlib.Path(tmpdir) / "comparison_summary.txt"
            self.module.write_comparison_summary(
                summary_path, copy_results, msgc_results
            )
            content = summary_path.read_text()

        self.assertIn("copygc_throughput_avg=550.00", content)
        self.assertIn("msgc_throughput_avg=660.00", content)
        self.assertIn("msgc_vs_copygc_delta=110.00", content)
        self.assertIn("msgc_vs_copygc_percent=20.00", content)
        self.assertIn("copygc_fail_total=2", content)
        self.assertIn("msgc_fail_total=1", content)

    def test_loaded_runtime_must_come_from_selected_sdk(self):
        args = self.module.parse_args(
            ["--copygc-sdk-root", "/tmp/copygc", "--msgc-sdk-root", "/tmp/msgc"]
        )
        copygc, _ = self.module.build_sdk_specs(args)

        with tempfile.TemporaryDirectory() as tmpdir:
            stderr_path = pathlib.Path(tmpdir) / "run.stderr.txt"
            stderr_path.write_text(
                "dyld: loaded: /tmp/copygc/runtime/lib/"
                "darwin_aarch64_cjnative/libcangjie-runtime.dylib\n"
            )
            loaded = self.module.validate_loaded_runtime(stderr_path, copygc)
            self.assertEqual(loaded, copygc.runtime_dylib.resolve())

            stderr_path.write_text(
                "dyld: loaded: /tmp/msgc/runtime/lib/"
                "darwin_aarch64_cjnative/libcangjie-runtime.dylib\n"
            )
            with self.assertRaisesRegex(RuntimeError, "outside selected SDK"):
                self.module.validate_loaded_runtime(stderr_path, copygc)

    def test_missing_sdk_artifacts_are_reported_before_execution(self):
        args = self.module.parse_args(
            [
                "--case-dir",
                "/tmp/not-built-case27",
                "--copygc-sdk-root",
                "/tmp/not-built-copygc",
                "--msgc-sdk-root",
                "/tmp/not-built-msgc",
            ]
        )

        with self.assertRaises(FileNotFoundError) as raised:
            self.module.ensure_input_paths(args, self.module.build_sdk_specs(args))

        message = str(raised.exception)
        self.assertIn("/tmp/not-built-copygc/bin/cjc", message)
        self.assertIn("/tmp/not-built-msgc/bin/cjc", message)
        self.assertIn("benchmark.cj", message)

    def test_main_dry_run_describes_both_modes_without_built_sdks(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            stdout = io.StringIO()
            with mock.patch("sys.stdout", stdout):
                rc = self.module.main(
                    [
                        "--dry-run",
                        "--case-dir",
                        "/tmp/not-built-case27",
                        "--copygc-sdk-root",
                        "/tmp/not-built-copygc",
                        "--msgc-sdk-root",
                        "/tmp/not-built-msgc",
                        "--result-root",
                        tmpdir,
                        "--runs",
                        "1",
                    ]
                )

            result_dirs = list(pathlib.Path(tmpdir).iterdir())

        self.assertEqual(rc, 0)
        self.assertEqual(len(result_dirs), 1)
        self.assertIn("sdk=copygc cjc_gc_mode=copygc", stdout.getvalue())
        self.assertIn("sdk=msgc cjc_gc_mode=marksweep", stdout.getvalue())

    def _result(self, throughput_value: int, fail: int = 0):
        return self.module.throughput.BenchmarkResult(
            payload="100K",
            concurrency=1000,
            duration_ms=60000,
            client_mode="sync",
            total=throughput_value * 60,
            ok=throughput_value * 60 - fail,
            fail=fail,
            inflight=0,
            throughput=throughput_value,
            bytes_sent_mb=100,
            bytes_recv_mb=100,
        )


if __name__ == "__main__":
    unittest.main()
