import pathlib
import unittest


CASE_DIR = pathlib.Path(__file__).resolve().parent.parent
BENCHMARK = CASE_DIR / "benchmark.cj"
RUN_COMPARE = CASE_DIR / "run_compare.sh"
RUN_COMPARE_LINUX = CASE_DIR / "run_compare_linux.sh"
RUN_COMPARE_PORTSAFE = CASE_DIR / "run_compare_portsafe.sh"
GO_BENCHMARK = CASE_DIR / "go_benchmark" / "main.go"
JAVA_BENCHMARK = CASE_DIR / "java_benchmark" / "Case27HttpComplexJsonBenchmark.java"
DEFAULT_MSGC_SDK = (
    "/Users/lipeisheng/code/cangjie/cangjie/"
    "cangjie_compiler_region_mark/output/msgc"
)
DEFAULT_MSGC_STDX = f"{DEFAULT_MSGC_SDK}/darwin_aarch64_cjnative"
DEFAULT_COPYGC_SDK = (
    "/Users/lipeisheng/code/cangjie/cangjie-pkg/dev/20260507/cangjie"
)
DEFAULT_COPYGC_STDX = (
    "/Users/lipeisheng/code/cangjie/cangjie-pkg/dev/20260507/"
    "darwin_aarch64_cjnative"
)


class Case27BenchmarkTuningTests(unittest.TestCase):
    def test_request_level_logs_removed(self) -> None:
        text = BENCHMARK.read_text()
        self.assertNotIn("[CASE27-REQ][client-send]", text)
        self.assertNotIn("[CASE27-REQ][client-ok]", text)
        self.assertNotIn("[CASE27-REQ][client-fail]", text)
        self.assertNotIn("[CASE27-REQ][server-start]", text)
        self.assertNotIn("[CASE27-REQ][server-done]", text)

    def test_sync_client_pool_size_is_one(self) -> None:
        text = BENCHMARK.read_text()
        self.assertIn("func buildClient(): Client {", text)
        self.assertIn(".poolSize(1)", text)
        self.assertNotIn(".poolSize(8)", text)

    def test_payload_clone_removed(self) -> None:
        text = BENCHMARK.read_text()
        self.assertNotIn("payload.clone()", text)

    def test_cangjie_processor_cap_not_forced(self) -> None:
        text = RUN_COMPARE.read_text()
        self.assertNotIn('PROCS="${PROCS:-6}"', text)
        self.assertIn('if [ -n "${PROCS:-}" ]; then', text)
        self.assertIn('export cjProcessorNum="$PROCS"', text)

    def test_copygc_compile_uses_default_gc_mode(self) -> None:
        text = RUN_COMPARE.read_text()
        self.assertIn('cjc_args=("-O2" "$BENCH_SRC")', text)
        self.assertIn('if [ -n "$CJC_GC_MODE" ]; then', text)
        self.assertIn('cjc_args+=("--gc-mode=$CJC_GC_MODE")', text)
        self.assertIn('cjc "${cjc_args[@]}"', text)
        self.assertIn(
            'run_side "CopyGC" "$COPYGC_DIR" "$COPYGC_SDK" "$COPYGC_STDX" '
            '18226 "" "" "$COPYGC_GC_INTERVAL" "" ""',
            text,
        )
        self.assertNotIn(
            'run_side "CopyGC" "$COPYGC_DIR" "$COPYGC_SDK" "$COPYGC_STDX" '
            '18226 "" "" "$COPYGC_GC_INTERVAL" "" "copygc"',
            text,
        )
        self.assertIn('"$MSGC_MIN_GC_THRESHOLD" "marksweep"', text)

    def test_all_cangjie_launchers_compile_at_o2(self) -> None:
        for script in (RUN_COMPARE, RUN_COMPARE_LINUX, RUN_COMPARE_PORTSAFE):
            with self.subTest(script=script.name):
                text = script.read_text()
                self.assertTrue(
                    'cjc_args=("-O2" "$BENCH_SRC")' in text
                    or 'cjc -O2 "$BENCH_SRC"' in text
                )
    def test_result_directory_uses_case26_style_configuration_name(self) -> None:
        for runner in (RUN_COMPARE, RUN_COMPARE_LINUX, RUN_COMPARE_PORTSAFE):
            with self.subTest(runner=runner.name):
                text = runner.read_text()
                self.assertIn('TS="$(date +%Y%m%d)"', text)
                self.assertIn("PAYLOAD_TAG=$(printf '%s' \"$PAYLOAD\" | tr '[:upper:]' '[:lower:]')", text)
                self.assertIn(
                    'COMPARE_DIR_BASE="$CASE_DIR/result/${TS}_c${CONC}_${PAYLOAD_TAG}_'
                    'HeapGrow${MSGC_HEAP_GROWTH}_Scavenge${MSGC_SCAVENGE_GROWTH}_compare"',
                    text,
                )
                self.assertIn('while [[ -e "$COMPARE_DIR" ]]; do', text)
                self.assertIn('COMPARE_DIR="${COMPARE_DIR_BASE}_${COMPARE_DIR_SUFFIX}"', text)

    def test_throughput_counts_only_successful_requests(self) -> None:
        self.assertIn('let rps = if (totalMs > 0) { okReq * 1000 / totalMs } else { 0 }', BENCHMARK.read_text())
        self.assertIn('throughput = stats.OK * 1000 / totalMs', GO_BENCHMARK.read_text())
        self.assertIn('long throughput = ok * 1000L / totalMs;', JAVA_BENCHMARK.read_text())

    def test_java_heap_defaults_to_heap_size(self) -> None:
        for runner in (RUN_COMPARE, RUN_COMPARE_LINUX):
            with self.subTest(runner=runner.name):
                text = runner.read_text()
                self.assertIn('JAVA_HEAP_SIZE=$(printf \'%s\' "$HEAP_SIZE" | tr \'[:upper:]\' \'[:lower:]\')', text)
                self.assertIn('JAVA_HEAP_SIZE="${JAVA_HEAP_SIZE%b}"', text)
                self.assertIn('JAVA_OPTS="${JAVA_OPTS:--Xmx${JAVA_HEAP_SIZE}}"', text)

    def test_macos_defaults_use_region_scan_msgc_and_20260507_copygc(self) -> None:
        text = RUN_COMPARE.read_text()

        self.assertIn(
            f'MSGC_SDK_ROOT="${{MSGC_SDK_ROOT:-{DEFAULT_MSGC_SDK}}}"',
            text,
        )
        self.assertIn(
            f'MSGC_STDX_ROOT="${{MSGC_STDX_ROOT:-{DEFAULT_MSGC_STDX}}}"',
            text,
        )
        self.assertIn(
            f'COPYGC_SDK_ROOT="${{COPYGC_SDK_ROOT:-{DEFAULT_COPYGC_SDK}}}"',
            text,
        )
        self.assertIn(
            f'COPYGC_STDX_ROOT="${{COPYGC_STDX_ROOT:-{DEFAULT_COPYGC_STDX}}}"',
            text,
        )

    def test_msgc_heap_size_is_soft_limit_not_virtual_heap_reservation(self) -> None:
        for runner in (RUN_COMPARE, RUN_COMPARE_LINUX):
            with self.subTest(runner=runner.name):
                text = runner.read_text()
                self.assertIn('cjHeapSize="$HEAP_SIZE"', text)
                self.assertNotIn('export cjMSMaxVirtualHeapSize="$HEAP_SIZE"', text)
                self.assertNotIn('cjMSMaxVirtualHeapSize=$HEAP_SIZE', text)

    def test_macos_runner_supports_opt_in_native_stack_sampling(self) -> None:
        text = RUN_COMPARE.read_text()
        self.assertIn('CASE27_SAMPLE_DURATION_SECONDS="${CASE27_SAMPLE_DURATION_SECONDS:-0}"', text)
        self.assertIn('CASE27_SAMPLE_DELAY_SECONDS="${CASE27_SAMPLE_DELAY_SECONDS:-8}"', text)
        self.assertIn('"SAMPLE_DURATION_SECONDS=$CASE27_SAMPLE_DURATION_SECONDS"', text)
        self.assertIn('/usr/bin/sample "$BENCH_PID" "$SAMPLE_DURATION_SECONDS" 1', text)
        self.assertIn('"$OUT_DIR/sample_${SAMPLE_DURATION_SECONDS}s.txt"', text)


if __name__ == "__main__":
    unittest.main()
