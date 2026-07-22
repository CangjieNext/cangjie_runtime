#!/usr/bin/env python3

"""Fully rebuild the MSGC SDK into cangjie_compiler/output/msgc."""

from __future__ import annotations

import sys

from full_sdk_builder import GCMode, main_for_mode


GC_MODE = GCMode.MSGC


if __name__ == "__main__":
    sys.exit(main_for_mode(GC_MODE))
