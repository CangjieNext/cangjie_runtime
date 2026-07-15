import pathlib
import unittest


CASE_DIR = pathlib.Path(__file__).resolve().parent.parent
BENCHMARK = CASE_DIR / "benchmark.cj"
RUN_COMPARE = CASE_DIR / "run_compare.sh"


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


if __name__ == "__main__":
    unittest.main()
