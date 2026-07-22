#!/usr/bin/env python3

import sys
import tempfile
import unittest
from datetime import datetime
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import compare_results


class CompareResultsTimelineTests(unittest.TestCase):
    def test_normalize_absolute_samples_trims_leading_prewarm_window(self) -> None:
        samples = [
            (datetime.fromisoformat("2026-06-17T22:09:50.000"), 10.0, 0.1),
            (datetime.fromisoformat("2026-06-17T22:09:51.000"), 11.0, 0.2),
            (datetime.fromisoformat("2026-06-17T22:09:52.000"), 100.0, 50.0),
            (datetime.fromisoformat("2026-06-17T22:09:53.000"), 101.0, 55.0),
        ]

        normalized = compare_results.normalize_absolute_samples(samples, duration_ms=1500)

        self.assertEqual(len(normalized), 2)
        self.assertAlmostEqual(normalized[0][0], 0.5)
        self.assertAlmostEqual(normalized[1][0], 1.5)
        self.assertEqual(normalized[0][1:], (100.0, 50.0))
        self.assertEqual(normalized[1][1:], (101.0, 55.0))

    def test_normalize_absolute_samples_falls_back_to_first_sample_without_duration(self) -> None:
        samples = [
            (datetime.fromisoformat("2026-06-17T22:09:50.000"), 10.0, 0.1),
            (datetime.fromisoformat("2026-06-17T22:09:51.000"), 11.0, 0.2),
        ]

        normalized = compare_results.normalize_absolute_samples(samples, duration_ms=None)

        self.assertEqual(len(normalized), 2)
        self.assertAlmostEqual(normalized[0][0], 0.0)
        self.assertAlmostEqual(normalized[1][0], 1.0)

    def test_parse_rss_csv_trims_absolute_timestamp_samples_to_benchmark_window(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            rss_path = Path(tmp) / "rss.csv"
            rss_path.write_text(
                "\n".join(
                    [
                        "2026-06-17T22:09:50.000,10240,0.1",
                        "2026-06-17T22:09:51.000,11264,0.2",
                        "2026-06-17T22:09:52.000,102400,50.0",
                        "2026-06-17T22:09:53.000,103424,55.0",
                    ]
                )
                + "\n"
            )

            parsed = compare_results.parse_rss_csv(rss_path, duration_ms=1500)

        self.assertEqual(parsed["sample_count"], 2)
        self.assertEqual(parsed["rss_mb"], [100.0, 101.0])
        self.assertEqual(parsed["cpu_pct"], [50.0, 55.0])
        self.assertAlmostEqual(parsed["elapsed_s"][0], 0.5)
        self.assertAlmostEqual(parsed["elapsed_s"][1], 1.5)


class CompareResultsBenchParsingTests(unittest.TestCase):
    def test_parse_bench_result_accepts_inflight_field_in_result_line(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            results_dir = Path(tmp)
            (results_dir / "stdout.txt").write_text(
                "\n".join(
                    [
                        "[RESULT] payload=100K concurrency=8 duration=60153ms client_mode=sync",
                        "[RESULT] total=83978 ok=83978 fail=0 inflight=0 throughput=1396req/s",
                        "[RESULT] bytes_sent=7974MB bytes_recv=8041MB",
                    ]
                )
                + "\n"
            )

            parsed = compare_results.parse_bench_result(results_dir)

        self.assertEqual(parsed["duration_ms"], 60153)
        self.assertEqual(parsed["total"], 83978)
        self.assertEqual(parsed["ok"], 83978)
        self.assertEqual(parsed["fail"], 0)
        self.assertEqual(parsed["throughput"], 1396)
        self.assertEqual(parsed["bytes_sent_mb"], 7974)
        self.assertEqual(parsed["bytes_recv_mb"], 8041)

    def test_run_data_uses_benchmark_duration_when_wall_time_is_missing(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            results_dir = Path(tmp)
            (results_dir / "stdout.txt").write_text(
                "[RESULT] payload=100K concurrency=8 duration=60153ms client_mode=sync\n"
            )

            run = compare_results.RunData(results_dir, "MSGC")

        self.assertEqual(run.wall_ms, 60153)


if __name__ == "__main__":
    unittest.main()


class CompareResultsCliShapeTests(unittest.TestCase):
    def test_main_accepts_three_run_form(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            base = Path(tmp)
            for name in ("copygc", "msgc", "go"):
                run_dir = base / name
                run_dir.mkdir()
                (run_dir / "stdout.txt").write_text("[RESULT] payload=10K concurrency=8 duration=1000ms client_mode=sync\n[RESULT] total=10 ok=10 fail=0 throughput=10req/s\n")
                (run_dir / "rss.csv").write_text("elapsed_ms,rss_mb,cpu_pct\n0,1,1\n")
            out = base / "report.md"
            argv = [
                "compare_results.py",
                "--run", f"CopyGC={base / 'copygc'}",
                "--run", f"MSGC={base / 'msgc'}",
                "--run", f"Go={base / 'go'}",
                "--output", str(out),
            ]
            old_argv = sys.argv
            try:
                sys.argv = argv
                compare_results.main()
            finally:
                sys.argv = old_argv
            self.assertTrue(out.exists())
