#!/usr/bin/env python3

from __future__ import annotations

import pathlib
import sys
from typing import Sequence


SCRIPT_DIR = pathlib.Path(__file__).resolve().parent
if str(SCRIPT_DIR) not in sys.path:
    sys.path.insert(0, str(SCRIPT_DIR))

import run_case27_throughput as runner


def build_default_args(extra_args: Sequence[str]) -> list[str]:
    return [
        "--payload",
        "10K",
        "--concurrency",
        "10",
        "--tag",
        "case27_c10_p10k_validate",
        *extra_args,
    ]


def main(argv: Sequence[str] | None = None) -> int:
    forwarded_args = build_default_args(list(argv or []))
    return runner.main(forwarded_args)


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
