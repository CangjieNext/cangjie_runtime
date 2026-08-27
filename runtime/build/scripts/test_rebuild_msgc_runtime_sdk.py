#!/usr/bin/env python3

from __future__ import annotations

import pathlib
import unittest

import rebuild_msgc_runtime_output as runtime_builder
import rebuild_msgc_runtime_sdk as target


class RebuildMSGCRuntimeSDKTests(unittest.TestCase):
    def test_default_sdk_root_is_resolved_by_runtime_builder(self) -> None:
        script_path = pathlib.Path(
            "/workspace/cangjie_runtime/runtime/build/scripts/rebuild_msgc_runtime_sdk.py"
        )

        forwarded = target.build_forwarded_args(script_path, ["--dry-run"])

        self.assertEqual(forwarded, ["--dry-run"])

    def test_explicit_sdk_root_overrides_the_default(self) -> None:
        script_path = pathlib.Path(
            "/workspace/cangjie_runtime/runtime/build/scripts/rebuild_msgc_runtime_sdk.py"
        )

        forwarded = target.build_forwarded_args(
            script_path,
            ["--sdk-root", "/tmp/custom-msgc-sdk", "--dry-run"],
        )
        args = runtime_builder.parse_args(forwarded)

        self.assertEqual(args.sdk_root, pathlib.Path("/tmp/custom-msgc-sdk"))
        self.assertTrue(args.dry_run)


if __name__ == "__main__":
    unittest.main()
