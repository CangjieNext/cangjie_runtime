#!/usr/bin/env python3

import unittest
from pathlib import Path


CASE_DIR = Path(__file__).resolve().parent.parent
SCRIPTS = (
    CASE_DIR / "run_compare.sh",
    CASE_DIR / "run_compare_linux.sh",
)


class JavaCompareIntegrationTests(unittest.TestCase):
    def test_both_compare_scripts_run_and_report_java(self) -> None:
        for script in SCRIPTS:
            with self.subTest(script=script.name):
                text = script.read_text()
                self.assertIn('JAVA_ENABLE="${JAVA_ENABLE:-1}"', text)
                self.assertIn('JAVA_DIR="$COMPARE_DIR/java"', text)
                self.assertIn("run_java_side()", text)
                self.assertIn('command -v "$JAVA_BIN"', text)
                self.assertIn('command -v "$JAVAC_BIN"', text)
                self.assertIn('"$OUT_DIR/compile.exitcode"', text)
                self.assertIn('"$OUT_DIR/test.exitcode"', text)
                self.assertIn('"$OUT_DIR/run.exitcode"', text)
                self.assertIn('compare_args+=(--run "Java=$JAVA_DIR")', text)
                self.assertIn('if [[ "$JAVA_RC" -ne 0 ]]; then', text)


if __name__ == "__main__":
    unittest.main()
