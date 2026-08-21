#!/usr/bin/env python3

from __future__ import annotations

import importlib.util
import io
import pathlib
import subprocess
import sys
import tempfile
import unittest
from unittest import mock


def load_module():
    script_path = pathlib.Path(__file__).with_name("run_case27_throughput.py")
    spec = importlib.util.spec_from_file_location("run_case27_throughput", script_path)
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


class RunCase27ThroughputTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.module = load_module()

    def test_parse_result_extracts_core_metrics(self):
        stdout = "\n".join(
            [
                "[CONFIG] payload_dir=/tmp/payloads server_port=19127",
                "[RESULT] payload=100K concurrency=1000 duration=61057ms client_mode=sync",
                "[RESULT] total=31868 ok=31817 fail=51 inflight=0 throughput=521req/s",
                "[RESULT] bytes_sent=3021MB bytes_recv=3046MB",
            ]
        )

        parsed = self.module.parse_benchmark_result(stdout)

        self.assertEqual(parsed.payload, "100K")
        self.assertEqual(parsed.concurrency, 1000)
        self.assertEqual(parsed.duration_ms, 61057)
        self.assertEqual(parsed.client_mode, "sync")
        self.assertEqual(parsed.total, 31868)
        self.assertEqual(parsed.ok, 31817)
        self.assertEqual(parsed.fail, 51)
        self.assertEqual(parsed.inflight, 0)
        self.assertEqual(parsed.throughput, 521)
        self.assertEqual(parsed.bytes_sent_mb, 3021)
        self.assertEqual(parsed.bytes_recv_mb, 3046)

    def test_parse_result_requires_result_lines(self):
        with self.assertRaisesRegex(ValueError, "missing"):
            self.module.parse_benchmark_result("[CONFIG] only config here\n")

    def test_write_aggregate_summary_includes_avg_min_max(self):
        result_one = self.module.BenchmarkResult(
            payload="100K",
            concurrency=1000,
            duration_ms=61057,
            client_mode="sync",
            total=31868,
            ok=31817,
            fail=51,
            inflight=0,
            throughput=521,
            bytes_sent_mb=3021,
            bytes_recv_mb=3046,
        )
        result_two = self.module.BenchmarkResult(
            payload="100K",
            concurrency=1000,
            duration_ms=61279,
            client_mode="sync",
            total=32432,
            ok=32432,
            fail=0,
            inflight=0,
            throughput=529,
            bytes_sent_mb=3079,
            bytes_recv_mb=3105,
        )

        with tempfile.TemporaryDirectory() as tmpdir:
            summary_path = pathlib.Path(tmpdir) / "summary.txt"

            self.module.write_aggregate_summary(
                summary_path=summary_path,
                run_results=[result_one, result_two],
            )

            content = summary_path.read_text()

        self.assertIn("runs=2", content)
        self.assertIn("throughput_avg=525.00", content)
        self.assertIn("throughput_min=521", content)
        self.assertIn("throughput_max=529", content)
        self.assertIn("fail_total=51", content)

    def test_parse_args_uses_case27_validation_defaults(self):
        args = self.module.parse_args([])

        self.assertEqual(
            pathlib.Path(args.sdk_root),
            pathlib.Path(
                "/Users/lipeisheng/code/cangjie/cangjie/"
                "cangjie_compiler_region_mark/output/msgc"
            ),
        )
        self.assertEqual(
            pathlib.Path(args.stdx_dir),
            pathlib.Path(
                "/Users/lipeisheng/code/cangjie/cangjie/"
                "cangjie_compiler_region_mark/output/msgc/"
                "darwin_aarch64_cjnative/dynamic/stdx"
            ),
        )
        self.assertEqual(args.payload, "100K")
        self.assertEqual(args.concurrency, 1000)
        self.assertEqual(args.duration_ms, 60000)
        self.assertEqual(args.heap_size, "4gb")
        self.assertEqual(args.runs, 1)

    def test_build_run_environment_contains_expected_case27_values(self):
        args = self.module.parse_args(
            [
                "--payload",
                "100K",
                "--concurrency",
                "1000",
                "--duration-ms",
                "60000",
                "--heap-size",
                "4gb",
                "--sdk-root",
                "/tmp/sdk",
                "--stdx-dir",
                "/tmp/stdx",
                "--case-dir",
                "/tmp/case27",
            ]
        )

        env = self.module.build_run_environment(
            args=args,
            out_dir=pathlib.Path("/tmp/out/run1"),
            port=19127,
        )

        self.assertEqual(env["SDK_ROOT"], "/tmp/sdk")
        self.assertEqual(env["STDX_DIR"], "/tmp/stdx")
        self.assertEqual(env["CASE27_PAYLOAD"], "100K")
        self.assertEqual(env["CASE27_CONCURRENCY"], "1000")
        self.assertEqual(env["CASE27_DURATION_MS"], "60000")
        self.assertEqual(env["CASE27_SERVER_PORT"], "19127")
        self.assertEqual(env["HEAP_SIZE"], "4gb")
        self.assertEqual(env["RUNTIME_HEAP_GROWTH"], "1")
        self.assertEqual(env["RUNTIME_SCAVENGE_GROWTH"], "10")
        self.assertEqual(env["cjMSEnumAssist"], "1")

    def test_build_run_environment_allows_external_enum_assist_override(self):
        args = self.module.parse_args(
            [
                "--payload", "100K", "--concurrency", "1000",
                "--duration-ms", "60000", "--heap-size", "4gb",
                "--sdk-root", "/tmp/sdk", "--stdx-dir", "/tmp/stdx",
                "--case-dir", "/tmp/case27",
            ]
        )

        with mock.patch.dict("os.environ", {"cjMSEnumAssist": "0"}, clear=False):
            env = self.module.build_run_environment(
                args=args, out_dir=pathlib.Path("/tmp/out/run1"), port=19127)

        self.assertEqual(env["cjMSEnumAssist"], "0")

    def test_main_dry_run_returns_zero_without_throughput(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            result_dir = pathlib.Path(tmpdir) / "result"
            with mock.patch.object(self.module, "ensure_input_paths") as ensure_paths:
                with mock.patch.object(self.module, "make_result_dir", return_value=result_dir):
                    with mock.patch.object(self.module, "choose_port", return_value=19127):
                        rc = self.module.main(
                            [
                                "--dry-run",
                                "--sdk-root",
                                "/tmp/sdk",
                                "--stdx-dir",
                                "/tmp/stdx",
                                "--envsetup",
                                "/tmp/envsetup.sh",
                                "--case-dir",
                                "/tmp/case27",
                            ]
                        )

        ensure_paths.assert_called_once()
        self.assertEqual(rc, 0)

    def test_child_script_initializes_dyld_before_sourcing_envsetup(self):
        child_script = self.module.render_child_script()

        self.assertIn('export DYLD_LIBRARY_PATH=""', child_script)
        self.assertIn('--gc-mode "$CJC_GC_MODE"', child_script)
        self.assertLess(
            child_script.index('export DYLD_LIBRARY_PATH=""'),
            child_script.index('source "$ENVSETUP"'),
        )

    def test_build_run_environment_selects_marksweep_compilation(self):
        args = self.module.parse_args(
            [
                "--sdk-root",
                "/tmp/msgc",
                "--stdx-dir",
                "/tmp/stdx",
                "--case-dir",
                "/tmp/case27",
            ]
        )

        env = self.module.build_run_environment(
            args=args,
            out_dir=pathlib.Path("/tmp/out/run1"),
            port=19127,
        )

        self.assertEqual(env["CJC_GC_MODE"], "marksweep")

    def test_main_exits_on_child_timeout_and_reports_diagnostics(self):
        args = self.module.parse_args(
            [
                "--sdk-root",
                "/tmp/sdk",
                "--stdx-dir",
                "/tmp/stdx",
                "--envsetup",
                "/tmp/envsetup.sh",
                "--case-dir",
                "/tmp/case27",
                "--run-timeout-sec",
                "1",
            ]
        )

        with tempfile.TemporaryDirectory() as tmpdir:
            result_dir = pathlib.Path(tmpdir) / "result"
            run_dir = result_dir / "run1"
            stderr = io.StringIO()
            with mock.patch.object(self.module, "ensure_input_paths"):
                with mock.patch.object(self.module, "make_result_dir", return_value=result_dir):
                    with mock.patch.object(self.module, "choose_port", return_value=19127):
                        with mock.patch.object(
                            self.module,
                            "run_single_benchmark",
                            return_value=(
                                124,
                                None,
                                "benchmark timed out after 1s; diagnostics: /tmp/fake.txt",
                            ),
                        ):
                            with mock.patch("sys.stderr", stderr):
                                rc = self.module.main(
                                    [
                                        "--sdk-root",
                                        "/tmp/sdk",
                                        "--stdx-dir",
                                        "/tmp/stdx",
                                        "--envsetup",
                                        "/tmp/envsetup.sh",
                                        "--case-dir",
                                        "/tmp/case27",
                                        "--run-timeout-sec",
                                        "1",
                                    ]
                                )

            self.assertEqual(args.run_timeout_sec, 1)
            self.assertEqual(rc, 124)
            self.assertTrue(run_dir.exists())
            summary = (run_dir / "summary.txt").read_text()
            self.assertIn("compile_and_run_rc=124", summary)
            self.assertIn("timed out after 1s", stderr.getvalue())
            self.assertIn("/tmp/fake.txt", stderr.getvalue())

    def test_run_single_benchmark_timeout_returns_failure_and_writes_diagnostics(self):
        args = self.module.parse_args(
            [
                "--sdk-root",
                "/tmp/sdk",
                "--stdx-dir",
                "/tmp/stdx",
                "--envsetup",
                "/tmp/envsetup.sh",
                "--case-dir",
                "/tmp/case27",
                "--run-timeout-sec",
                "1",
            ]
        )

        timeout_error = subprocess.TimeoutExpired(
            cmd=["/bin/bash", "-c", "echo test"],
            timeout=1,
            output="partial child stdout",
            stderr="partial child stderr",
        )

        with tempfile.TemporaryDirectory() as tmpdir:
            out_dir = pathlib.Path(tmpdir)

            def fake_subprocess_run(cmd, **kwargs):
                if cmd[:2] == ["/bin/bash", "-c"]:
                    raise timeout_error
                return subprocess.CompletedProcess(cmd, 0, stdout="ps snapshot\n", stderr="")

            with mock.patch.object(self.module.subprocess, "run", side_effect=fake_subprocess_run):
                rc, result, error_message = self.module.run_single_benchmark(
                    args=args,
                    run_index=1,
                    out_dir=out_dir,
                    port=19127,
                )

            self.assertEqual(rc, 124)
            self.assertIsNone(result)
            self.assertIsNotNone(error_message)
            self.assertIn("timed out after 1s", error_message)
            self.assertTrue((out_dir / "child.stdout.txt").exists())
            self.assertTrue((out_dir / "child.stderr.txt").exists())
            diagnostics = out_dir / "failure_diagnostics.txt"
            self.assertTrue(diagnostics.exists())
            self.assertIn("partial child stderr", diagnostics.read_text())


if __name__ == "__main__":
    unittest.main()
