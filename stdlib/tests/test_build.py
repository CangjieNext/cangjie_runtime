#!/usr/bin/env python3
# -*- coding: utf-8 -*-
#
# Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
# This source file is part of the Cangjie project, licensed under Apache-2.0
# with Runtime Library Exception.
#
# See https://cangjie-lang.cn/pages/LICENSE for license information.

import importlib.util
import os
import types
import unittest


BUILD_PATH = os.path.join(os.path.dirname(os.path.dirname(__file__)), "build.py")
SPEC = importlib.util.spec_from_file_location("stdlib_build", BUILD_PATH)
STDLIB_BUILD = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(STDLIB_BUILD)


def generate_android_defs(target):
    args = types.SimpleNamespace(
        target=STDLIB_BUILD.TARGET_DICTIONARY[target],
        build_type=STDLIB_BUILD.BuildType.release,
        target_lib=[],
        target_toolchain="/ndk/toolchains/llvm/prebuilt/linux-x86_64/bin",
        hwasan=False,
        target_sysroot=None,
        build_args=[],
        include=[],
        stdlib_coverage=False,
        asancov=False,
        gcc_toolchain=None,
        sanitizer_support=None,
    )
    return STDLIB_BUILD.generate_cmake_defs(args)


class GenerateCmakeDefsTest(unittest.TestCase):
    def test_x86_64_android_api_levels(self):
        expected_api_levels = {
            "android26-x86_64": "26",
            "android31-x86_64": "31",
            "android-x86_64": "",
        }

        for target, api_level in expected_api_levels.items():
            with self.subTest(target=target):
                definitions = generate_android_defs(target)
                self.assertIn("-DCMAKE_ANDROID_API=" + api_level, definitions)
                self.assertTrue(any(definition.startswith("-DCMAKE_ANDROID_NDK=")
                                    for definition in definitions))


if __name__ == "__main__":
    unittest.main()
