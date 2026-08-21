#!/usr/bin/env python3

from __future__ import annotations

import importlib.util
import pathlib
import sys
import unittest
from unittest import mock


def load_module():
    script_path = pathlib.Path(__file__).with_name("run_case27_c10_p10k.py")
    spec = importlib.util.spec_from_file_location("run_case27_c10_p10k", script_path)
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


class RunCase27C10P10KTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.module = load_module()

    def test_build_default_args_sets_case27_c10_p10k_defaults(self):
        args = self.module.build_default_args([])

        self.assertEqual(
            args[:6],
            [
                "--payload",
                "10K",
                "--concurrency",
                "10",
                "--tag",
                "case27_c10_p10k_validate",
            ],
        )

    def test_main_delegates_to_shared_runner_with_passthrough_args(self):
        with mock.patch.object(self.module.runner, "main", return_value=0) as runner_main:
            rc = self.module.main(["--runs", "3", "--duration-ms", "1000"])

        runner_main.assert_called_once()
        delegated_args = runner_main.call_args.args[0]
        self.assertEqual(
            delegated_args[:6],
            [
                "--payload",
                "10K",
                "--concurrency",
                "10",
                "--tag",
                "case27_c10_p10k_validate",
            ],
        )
        self.assertEqual(delegated_args[6:], ["--runs", "3", "--duration-ms", "1000"])
        self.assertEqual(rc, 0)


if __name__ == "__main__":
    unittest.main()
