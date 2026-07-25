#!/usr/bin/env python3

"""Rebuild only the MSGC Runtime and install it into output/msgc."""

from __future__ import annotations

import pathlib
import sys
from collections.abc import Sequence

import rebuild_msgc_runtime_output as runtime_builder


def build_forwarded_args(script_path: pathlib.Path, argv: Sequence[str]) -> list[str]:
    repo = runtime_builder.resolve_repo_layout(script_path)
    default_sdk_root = repo.compiler_root / "output" / "msgc"
    return ["--sdk-root", str(default_sdk_root), *argv]


def main(argv: Sequence[str] | None = None) -> int:
    caller_args = list(sys.argv[1:] if argv is None else argv)
    forwarded_args = build_forwarded_args(pathlib.Path(__file__), caller_args)
    return runtime_builder.main(forwarded_args)


if __name__ == "__main__":
    raise SystemExit(main())
