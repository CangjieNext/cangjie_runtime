import pathlib
import unittest


CASE_DIR = pathlib.Path(__file__).resolve().parent.parent
BENCHMARK = CASE_DIR / "benchmark.cj"
GO_BENCHMARK = CASE_DIR / "go_benchmark" / "main.go"
JAVA_BENCHMARK = CASE_DIR / "java_benchmark" / "Case27HttpComplexJsonBenchmark.java"
RUN_COMPARE = CASE_DIR / "run_compare.sh"
RUN_COMPARE_LINUX = CASE_DIR / "run_compare_linux.sh"
RUN_COMPARE_PORTSAFE = CASE_DIR / "run_compare_portsafe.sh"
RUN_COMPARE_ALTPORTS = CASE_DIR / ".run_compare_altports.sh"
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

    def test_throughput_counts_only_successful_requests(self) -> None:
        self.assertIn(
            'let rps = if (totalMs > 0) { okReq * 1000 / totalMs } else { 0 }',
            BENCHMARK.read_text(),
        )
        self.assertIn(
            'throughput = stats.OK * 1000 / totalMs',
            GO_BENCHMARK.read_text(),
        )
        self.assertIn(
            'long throughput = ok * 1000L / totalMs;',
            JAVA_BENCHMARK.read_text(),
        )

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
        for script in (
            RUN_COMPARE,
            RUN_COMPARE_LINUX,
            RUN_COMPARE_PORTSAFE,
            RUN_COMPARE_ALTPORTS,
        ):
            with self.subTest(script=script.name):
                text = script.read_text()
                self.assertTrue(
                    'cjc_args=("-O2" "$BENCH_SRC")' in text
                    or 'cjc -O2 "$BENCH_SRC"' in text
                )
    def test_cangjie_has_concurrent_static_and_stack_persistent_roots(self) -> None:
        text = BENCHMARK.read_text()
        self.assertIn('CASE26_PERSISTENT_BYTES', text)
        self.assertIn('CASE26_PERSISTENT_WORKERS', text)
        self.assertIn('let PERSISTENT_STATIC_ROOT_SHARDS', text)
        self.assertIn('let stackRoots = ArrayList<Array<Byte>>(', text)
        self.assertIn('spawn { persistentAllocationWorker(', text)
        self.assertIn('128, 256, 512, 1024, 2048, 4096, 8192, 16384', text)
        self.assertIn('65536, 262144, 1048576', text)
        self.assertNotIn('Array<Byte>(remainder', text)
        self.assertIn('catch (e: Error)', text)
        self.assertIn('persistentCompleted.fetchAdd(1)', text)
        self.assertIn('if (persistentFailed.load())', text)

    def test_launchers_forward_five_gib_baseline_and_keep_sixty_second_load(self) -> None:
        scripts = (
            RUN_COMPARE,
            RUN_COMPARE_LINUX,
            RUN_COMPARE_PORTSAFE,
            RUN_COMPARE_ALTPORTS,
        )
        for script in scripts:
            text = script.read_text()
            self.assertIn('DUR="${CASE26_DURATION_MS:-60000}"', text)
            self.assertIn('HEAP_SIZE="${HEAP_SIZE:-16gb}"', text)
            if script in (RUN_COMPARE, RUN_COMPARE_LINUX):
                self.assertIn(
                    'PERSISTENT_BYTES_INPUT="${CASE26_PERSISTENT_BYTES:-5368709120}"',
                    text,
                )
                self.assertIn(
                    'bash "$SCRIPT_DIR/normalize_byte_size.sh" "$PERSISTENT_BYTES_INPUT"',
                    text,
                )
            else:
                self.assertIn(
                    'PERSISTENT_BYTES="${CASE26_PERSISTENT_BYTES:-5368709120}"',
                    text,
                )
            self.assertIn(
                'PERSISTENT_WORKERS="${CASE26_PERSISTENT_WORKERS:-8}"',
                text,
            )
            self.assertIn('"CASE26_PERSISTENT_BYTES=$PERSISTENT_BYTES"', text)
            self.assertIn('"CASE26_PERSISTENT_WORKERS=$PERSISTENT_WORKERS"', text)
            self.assertIn(
                'GO_GOMEMLIMIT="${GO_GOMEMLIMIT:-16GiB}"',
                text,
            )
            self.assertIn('"GOMEMLIMIT=$GO_GOMEMLIMIT"', text)

    def test_macos_launchers_default_to_20260507_copygc_sdk(self) -> None:
        for script in (RUN_COMPARE, RUN_COMPARE_PORTSAFE, RUN_COMPARE_ALTPORTS):
            text = script.read_text()
            self.assertIn(
                f'COPYGC_SDK_ROOT="${{COPYGC_SDK_ROOT:-{DEFAULT_COPYGC_SDK}}}"',
                text,
            )
            self.assertIn(
                f'COPYGC_STDX_ROOT="${{COPYGC_STDX_ROOT:-{DEFAULT_COPYGC_STDX}}}"',
                text,
            )

    def test_run_compare_uses_heap_size_for_default_java_heap_limit(self) -> None:
        text = RUN_COMPARE.read_text()

        self.assertIn(
            "JAVA_HEAP_SIZE=$(printf '%s' \"$HEAP_SIZE\" | tr '[:upper:]' '[:lower:]')",
            text,
        )
        self.assertIn('JAVA_HEAP_SIZE="${JAVA_HEAP_SIZE%b}"', text)
        self.assertIn('JAVA_OPTS="${JAVA_OPTS:--Xmx${JAVA_HEAP_SIZE}}"', text)

    def test_msgc_heap_size_is_soft_limit_not_virtual_heap_reservation(self) -> None:
        for runner in (RUN_COMPARE, RUN_COMPARE_LINUX):
            with self.subTest(runner=runner.name):
                text = runner.read_text()
                self.assertIn('cjHeapSize="$HEAP_SIZE"', text)
                self.assertNotIn('export cjMSMaxVirtualHeapSize="$HEAP_SIZE"', text)
                self.assertNotIn('cjMSMaxVirtualHeapSize=$HEAP_SIZE', text)

    def test_run_compare_can_disable_each_side(self) -> None:
        text = RUN_COMPARE.read_text()

        for name in (
            "COPYGC_ENABLE",
            "MSGC_ENABLE",
            "GO_ENABLE",
            "JAVA_ENABLE",
        ):
            self.assertIn(f'{name}="${{{name}:-1}}"', text)
            self.assertIn(f'if [[ "${name}" != "0" ]]; then', text)

        self.assertIn(
            'if [[ "$ENABLED_SIDE_COUNT" -eq 0 ]]; then',
            text,
        )
        self.assertIn(
            'compare_args+=(--run "CopyGC=$COPYGC_DIR")',
            text,
        )
        self.assertIn(
            'compare_args+=(--run "MSGC=$MSGC_DIR")',
            text,
        )
        self.assertIn(
            'if [[ "$ENABLED_SIDE_COUNT" -ge 2 ]]; then',
            text,
        )

    def test_run_compare_names_result_from_benchmark_configuration(self) -> None:
        text = RUN_COMPARE.read_text()

        self.assertIn('TS="$(date +%Y%m%d)"', text)
        self.assertIn(
            'PERSISTENT_BYTES_MIB=$((PERSISTENT_BYTES / 1024 / 1024))',
            text,
        )
        self.assertIn(
            "PAYLOAD_TAG=$(printf '%s' \"$PAYLOAD\" | tr '[:upper:]' '[:lower:]')",
            text,
        )
        self.assertNotIn('${PAYLOAD,,}', text)
        self.assertIn(
            'COMPARE_DIR_BASE="$CASE_DIR/result/${TS}_${PERSISTENT_BYTES_MIB}M_c${CONC}_${PAYLOAD_TAG}_HeapGrow${MSGC_HEAP_GROWTH}_Scavenge${MSGC_SCAVENGE_GROWTH}_compare"',
            text,
        )
        self.assertIn('COMPARE_DIR="$COMPARE_DIR_BASE"', text)
        self.assertIn('while [[ -e "$COMPARE_DIR" ]]; do', text)
        self.assertIn(
            'COMPARE_DIR="${COMPARE_DIR_BASE}_${COMPARE_DIR_SUFFIX}"',
            text,
        )
        self.assertNotIn('COMPARE_DIR="$CASE_DIR/result/${TS}_${TAG}_c${CONC}_p${PAYLOAD}_compare"', text)

    def test_linux_run_compare_names_result_with_procs(self) -> None:
        text = RUN_COMPARE_LINUX.read_text()

        self.assertIn('RUN_DATE="$(date +%Y%m%d)"', text)
        self.assertIn(
            'PERSISTENT_BYTES_MIB=$((PERSISTENT_BYTES / 1024 / 1024))',
            text,
        )
        self.assertIn(
            "PAYLOAD_TAG=$(printf '%s' \"$PAYLOAD\" | tr '[:upper:]' '[:lower:]')",
            text,
        )
        self.assertIn('PROCS_TAG="${PROCS:-Auto}"', text)
        self.assertIn(
            'COMPARE_DIR_BASE="$CASE_DIR/result/${RUN_DATE}_${PERSISTENT_BYTES_MIB}M_c${CONC}_${PAYLOAD_TAG}_HeapGrow${MSGC_HEAP_GROWTH}_Scavenge${MSGC_SCAVENGE_GROWTH}_Procs${PROCS_TAG}_compare"',
            text,
        )
        self.assertIn('COMPARE_DIR="$COMPARE_DIR_BASE"', text)
        self.assertIn('while [[ -e "$COMPARE_DIR" ]]; do', text)
        self.assertIn(
            'COMPARE_DIR="${COMPARE_DIR_BASE}_${COMPARE_DIR_SUFFIX}"',
            text,
        )


if __name__ == "__main__":
    unittest.main()
