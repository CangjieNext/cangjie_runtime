#!/usr/bin/env python3

from __future__ import annotations

import pathlib
import unittest


RUNTIME_ROOT = pathlib.Path(__file__).resolve().parents[2]
MSGC_SDK = (
    "/Users/lipeisheng/code/cangjie/cangjie/"
    "cangjie_compiler_region_mark/output/msgc"
)
MSGC_STDX_ROOT = f"{MSGC_SDK}/darwin_aarch64_cjnative"

SHELL_CONTRACTS = {
    "MSGC_RESULT/Result/case25_http_complex_json/run.sh": (
        f'SDK_ROOT="${{SDK_ROOT:-{MSGC_SDK}}}"',
        f'STDX_DIR="${{STDX_DIR:-{MSGC_STDX_ROOT}/dynamic/stdx}}"',
    ),
    "MSGC_RESULT/Result/case25_http_complex_json/run_compare_d602_vs_d507.sh": (
        f'MSGC_SDK="{MSGC_SDK}"',
        f'MSGC_STDX="{MSGC_STDX_ROOT}/dynamic/stdx"',
    ),
    "MSGC_RESULT/Result/case26_http_complex_json/run_compare.sh": (
        f'MSGC_SDK_ROOT="${{MSGC_SDK_ROOT:-{MSGC_SDK}}}"',
        f'MSGC_STDX_ROOT="${{MSGC_STDX_ROOT:-{MSGC_STDX_ROOT}}}"',
    ),
    "MSGC_RESULT/Result/case26_http_complex_json/run_compare_portsafe.sh": (
        f'MSGC_SDK_ROOT="${{MSGC_SDK_ROOT:-{MSGC_SDK}}}"',
        f'MSGC_STDX_ROOT="${{MSGC_STDX_ROOT:-{MSGC_STDX_ROOT}}}"',
    ),
    "MSGC_RESULT/Result/case26_http_complex_json/.run_compare_altports.sh": (
        f'MSGC_SDK_ROOT="${{MSGC_SDK_ROOT:-{MSGC_SDK}}}"',
        f'MSGC_STDX_ROOT="${{MSGC_STDX_ROOT:-{MSGC_STDX_ROOT}}}"',
    ),
    "MSGC_RESULT/Result/case27_http_complex_json/run_compare.sh": (
        f'MSGC_SDK_ROOT="${{MSGC_SDK_ROOT:-{MSGC_SDK}}}"',
        f'MSGC_STDX_ROOT="${{MSGC_STDX_ROOT:-{MSGC_STDX_ROOT}}}"',
    ),
    "MSGC_RESULT/Result/case27_http_complex_json/run_compare_portsafe.sh": (
        f'MSGC_SDK_ROOT="${{MSGC_SDK_ROOT:-{MSGC_SDK}}}"',
        f'MSGC_STDX_ROOT="${{MSGC_STDX_ROOT:-{MSGC_STDX_ROOT}}}"',
    ),
    "MSGC_RESULT/Result/case27_http_complex_json/.run_compare_altports.sh": (
        f'MSGC_SDK_ROOT="${{MSGC_SDK_ROOT:-{MSGC_SDK}}}"',
        f'MSGC_STDX_ROOT="${{MSGC_STDX_ROOT:-{MSGC_STDX_ROOT}}}"',
    ),
    "MSGC_RESULT/Result/case27_http_complex_json_msgc_high_p/run_compare_all_modes.sh": (
        f'MSGC_SDK_ROOT="${{MSGC_SDK_ROOT:-{MSGC_SDK}}}"',
        f'MSGC_STDX_ROOT="${{MSGC_STDX_ROOT:-{MSGC_STDX_ROOT}}}"',
    ),
    "MSGC_RESULT/Result/case27_http_complex_json_msgc_high_p/run_compare_portsafe.sh": (
        f'MSGC_SDK_ROOT="${{MSGC_SDK_ROOT:-{MSGC_SDK}}}"',
        f'MSGC_STDX_ROOT="${{MSGC_STDX_ROOT:-{MSGC_STDX_ROOT}}}"',
    ),
    "MSGC_RESULT/Result/case27_http_complex_json_msgc_high_p/.run_compare_altports.sh": (
        f'MSGC_SDK_ROOT="${{MSGC_SDK_ROOT:-{MSGC_SDK}}}"',
        f'MSGC_STDX_ROOT="${{MSGC_STDX_ROOT:-{MSGC_STDX_ROOT}}}"',
    ),
    "MSGC_RESULT/Result/case27_http_complex_json_msgc_high_p/run_msgc_high_p.sh": (
        f'MSGC_SDK_ROOT="${{MSGC_SDK_ROOT:-{MSGC_SDK}}}"',
        f'MSGC_STDX_ROOT="${{MSGC_STDX_ROOT:-{MSGC_STDX_ROOT}}}"',
    ),
    "MSGC_RESULT/Result/case28_http_complex_json/run_compare.sh": (
        f'MSGC_SDK_ROOT="${{MSGC_SDK_ROOT:-{MSGC_SDK}}}"',
        f'MSGC_STDX_ROOT="${{MSGC_STDX_ROOT:-{MSGC_STDX_ROOT}}}"',
    ),
    "MSGC_RESULT/Result/case29_http_complex_json/run_case29.sh": (
        f'MSGC_SDK_ROOT="${{MSGC_SDK_ROOT:-{MSGC_SDK}}}"',
        f'MSGC_STDX_ROOT="${{MSGC_STDX_ROOT:-{MSGC_STDX_ROOT}}}"',
    ),
    "MSGC_RESULT/Result/case30_31_http_external_client/run_external_case.sh": (
        f'MSGC_SDK_ROOT="${{MSGC_SDK_ROOT:-{MSGC_SDK}}}"',
        f'MSGC_STDX_ROOT="${{MSGC_STDX_ROOT:-{MSGC_STDX_ROOT}}}"',
    ),
}


class MSGCSDKPathDefaultsTests(unittest.TestCase):
    def test_macos_case_scripts_use_the_region_mark_msgc_sdk_contract(self) -> None:
        for relative_path, expected_lines in SHELL_CONTRACTS.items():
            with self.subTest(script=relative_path):
                text = (RUNTIME_ROOT / relative_path).read_text(encoding="utf-8")
                for expected in expected_lines:
                    self.assertIn(expected, text)


if __name__ == "__main__":
    unittest.main()
