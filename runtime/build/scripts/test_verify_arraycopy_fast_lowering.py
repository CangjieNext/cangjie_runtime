#!/usr/bin/env python3

from __future__ import annotations

import importlib.util
import pathlib
import sys
import tempfile
import unittest
from unittest import mock


def load_module():
    script_path = pathlib.Path(__file__).with_name("verify_arraycopy_fast_lowering.py")
    spec = importlib.util.spec_from_file_location(
        "verify_arraycopy_fast_lowering",
        script_path,
    )
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


class VerifyArrayCopyFastLoweringTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.module = load_module()

    def test_parse_wrapper_hits_counts_ref_and_struct_wrappers(self):
        asm = "\n".join(
            [
                "bl _CJ_MCC_ArrayCopyRefFast_MSGC",
                "_CJ_MCC_ArrayCopyRefFast_MSGC:",
                "bl _CJ_MCC_ArrayCopyStructFast_MSGC",
            ]
        )

        hits = self.module.parse_wrapper_hits(asm)

        self.assertEqual(hits.ref_count, 2)
        self.assertEqual(hits.struct_count, 1)
        self.assertEqual(hits.total_count, 3)
        self.assertTrue(hits.has_fast_wrappers)

    def test_parse_wrapper_hits_returns_zero_when_no_fast_wrapper_exists(self):
        hits = self.module.parse_wrapper_hits("bl _CJ_MCC_ArrayCopyRef\n")

        self.assertEqual(hits.ref_count, 0)
        self.assertEqual(hits.struct_count, 0)
        self.assertEqual(hits.total_count, 0)
        self.assertFalse(hits.has_fast_wrappers)

    def test_count_arraycopy_signal_tokens_detects_slow_and_intrinsic_names(self):
        text = "\n".join(
            [
                "bl _CJ_MCC_ArrayCopyRef",
                "call void @llvm.cj.array.copy.struct",
                "cj_array_copy_ref",
            ]
        )

        signal_count = self.module.count_arraycopy_signal_tokens(text)

        self.assertEqual(signal_count, 3)

    def test_overall_exit_code_prefers_error_over_detection_fail(self):
        pass_result = self.module.ProbeResult(
            channel="llc",
            command=["llc"],
            rc=0,
            artifact_path=pathlib.Path("/tmp/llc.s"),
            hits=self.module.WrapperHits(ref_count=0, struct_count=0),
        )
        fail_result = self.module.ProbeResult(
            channel="cjc",
            command=["cjc"],
            rc=0,
            artifact_path=pathlib.Path("/tmp/benchmark.s"),
            hits=self.module.WrapperHits(ref_count=1, struct_count=0),
        )
        error_result = self.module.ProbeResult(
            channel="cjc",
            command=["cjc"],
            rc=1,
            artifact_path=pathlib.Path("/tmp/benchmark.s"),
            hits=None,
            error="compile failed",
        )

        self.assertEqual(self.module.compute_exit_code([pass_result]), 0)
        self.assertEqual(self.module.compute_exit_code([fail_result]), 1)
        self.assertEqual(self.module.compute_exit_code([pass_result, error_result]), 2)

    def test_render_cjc_child_script_resets_dyld_before_envsetup(self):
        child_script = self.module.render_cjc_child_script()

        self.assertIn('export DYLD_LIBRARY_PATH=""', child_script)
        self.assertLess(
            child_script.index('export DYLD_LIBRARY_PATH=""'),
            child_script.index('source "$ENVSETUP"'),
        )
        self.assertIn('cjc "$CJ_SOURCE"', child_script)
        self.assertIn('--save-temps "$OUT_DIR" -j1 -o benchmark', child_script)

    def test_build_default_link_options_uses_stdx_runtime_libs(self):
        stdx_dir = pathlib.Path("/tmp/stdx")

        link_options = self.module.build_default_link_options(stdx_dir)

        self.assertEqual(
            link_options,
            [
                "/tmp/stdx/libstdx.net.http.dylib",
                "/tmp/stdx/libstdx.encoding.json.dylib",
            ],
        )

    def test_main_dry_run_skips_real_probe_execution(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            result_root = pathlib.Path(tmpdir) / "result"
            with mock.patch.object(self.module, "ensure_input_paths") as ensure_paths:
                with mock.patch.object(self.module, "make_result_dir", return_value=result_root):
                    with mock.patch.object(self.module, "run_llc_probe") as run_llc_probe:
                        with mock.patch.object(self.module, "run_cjc_probe") as run_cjc_probe:
                            rc = self.module.main(
                                [
                                    "--dry-run",
                                    "--result-root",
                                    str(result_root),
                                ]
                            )

        ensure_paths.assert_called_once()
        run_llc_probe.assert_not_called()
        run_cjc_probe.assert_not_called()
        self.assertEqual(rc, 0)


if __name__ == "__main__":
    unittest.main()
