#!/usr/bin/env python3

import pathlib
import unittest


CASE_DIR = pathlib.Path(__file__).resolve().parent.parent
RUN_SCRIPTS = (
    CASE_DIR / "run_compare.sh",
    CASE_DIR / "run_compare_linux.sh",
)


class Case27JavaCompareScriptTests(unittest.TestCase):
    def test_java_side_is_enabled_and_reported_by_default(self) -> None:
        for script in RUN_SCRIPTS:
            text = script.read_text()
            with self.subTest(script=script.name):
                self.assertIn('JAVA_ENABLE="${JAVA_ENABLE:-1}"', text)
                self.assertIn('JAVA_DIR="$COMPARE_DIR/java"', text)
                self.assertIn("run_java_side()", text)
                self.assertIn('run_java_side "Java" "$JAVA_DIR" 18238', text)
                self.assertIn('compare_args+=(--run "Java=$JAVA_DIR")', text)

    def test_java_preflight_and_failures_are_not_silent(self) -> None:
        for script in RUN_SCRIPTS:
            text = script.read_text()
            with self.subTest(script=script.name):
                self.assertIn("Case27HttpComplexJsonBenchmarkTest", text)
                self.assertIn("JAVA_RC", text)
                self.assertIn('if [[ "$JAVA_RC" -ne 0 ]]; then', text)
                self.assertIn("run.exitcode", text)
                self.assertIn("compile.exitcode", text)
                self.assertIn("test.exitcode", text)


if __name__ == "__main__":
    unittest.main()
