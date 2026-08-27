import pathlib
import unittest


CASE_DIR = pathlib.Path(__file__).resolve().parent.parent
BENCHMARK = CASE_DIR / "benchmark.cj"
GO_BENCHMARK = CASE_DIR / "go_benchmark" / "main.go"
GO_STREAM_BENCHMARK = CASE_DIR / "go_benchmark_stream" / "main.go"
JAVA_BENCHMARK = CASE_DIR / "java_benchmark" / "Case27HttpComplexJsonBenchmark.java"
RUN_COMPARE = CASE_DIR / "run_compare.sh"
RUN_MSGC = CASE_DIR / "run_msgc.sh"


class Case25BenchmarkTuningTests(unittest.TestCase):
    def test_cangjie_uses_the_streaming_json_path(self) -> None:
        text = BENCHMARK.read_text(encoding="utf-8")

        self.assertIn("CASE25_PAYLOAD", text)
        self.assertIn("transformRequest", text)
        self.assertIn("responseHasServerOk", text)
        self.assertIn('"payloads"', text)
        self.assertNotIn("../case27_http_complex_json", text)

    def test_all_sides_count_only_successful_requests(self) -> None:
        self.assertIn(
            "let throughput = if (elapsed > 0) { ok * 1_000 / elapsed } else { 0 }",
            BENCHMARK.read_text(encoding="utf-8"),
        )
        self.assertIn(
            "throughput = stats.OK * 1000 / totalMs",
            GO_BENCHMARK.read_text(encoding="utf-8"),
        )
        self.assertIn(
            "long throughput = ok * 1000L / totalMs;",
            JAVA_BENCHMARK.read_text(encoding="utf-8"),
        )

    def test_go_uses_dom_active_module_and_preserves_stream_variant(self) -> None:
        self.assertTrue(GO_STREAM_BENCHMARK.is_file())
        go = GO_BENCHMARK.read_text(encoding="utf-8")
        go_stream = GO_STREAM_BENCHMARK.read_text(encoding="utf-8")

        self.assertIn("func decodeJSONTree", go)
        self.assertIn("return json.Marshal(root)", go)
        self.assertIn("transformRequestJSONStream", go_stream)
        self.assertTrue((CASE_DIR / "go_benchmark_stream" / "stream_json.go").is_file())
        self.assertIn(
            "var raw json.RawMessage",
            (CASE_DIR / "go_benchmark_stream" / "stream_json.go").read_text(encoding="utf-8"),
        )

    def test_persistent_baseline_is_disabled_by_default_everywhere(self) -> None:
        compare = RUN_COMPARE.read_text(encoding="utf-8")
        go = GO_BENCHMARK.read_text(encoding="utf-8")
        java = JAVA_BENCHMARK.read_text(encoding="utf-8")

        self.assertIn('PERSISTENT_BYTES_INPUT="${CASE25_PERSISTENT_BYTES:-0}"', compare)
        self.assertIn("defaultPersistentBytes   int64 = 0", go)
        self.assertIn("DEFAULT_PERSISTENT_BYTES = 0L", java)
        self.assertIn("Persistent heap baseline disabled", go)
        self.assertIn("Persistent heap baseline disabled", java)

    def test_runners_are_self_contained_and_compile_at_o2(self) -> None:
        for runner in (RUN_COMPARE, RUN_MSGC):
            with self.subTest(runner=runner.name):
                text = runner.read_text(encoding="utf-8")
                self.assertIn("$CASE_DIR/payloads", text)
                self.assertNotIn("../case27_http_complex_json", text)
                self.assertIn("-O2", text)

        compare = RUN_COMPARE.read_text(encoding="utf-8")
        self.assertIn('"$CASE_DIR/copygc/stream_transform.cj"', compare)
        self.assertIn('"$CASE_DIR/msgc/stream_transform.cj"', compare)
        self.assertIn('"$CASE_DIR/persistent_baseline.cj"', compare)
        self.assertIn("libstdx.encoding.json.stream.dylib", compare)
        self.assertIn('"CASE_DIR=$CASE_DIR"', compare)
        self.assertIn('"CASE27_PAYLOAD=$PAYLOAD"', compare)

    def test_msgc_compile_mode_is_explicit(self) -> None:
        text = RUN_MSGC.read_text(encoding="utf-8")

        self.assertIn("-O2 --gc-mode=marksweep", text)


if __name__ == "__main__":
    unittest.main()
