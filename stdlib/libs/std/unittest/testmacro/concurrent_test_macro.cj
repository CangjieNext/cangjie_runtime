/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
 * This source file is part of the Cangjie project, licensed under Apache-2.0
 * with Runtime Library Exception.
 *
 * See https://cangjie-lang.cn/pages/LICENSE for license information.
 */

/**
 * @file The file declares the macro ConcurrentTest.
 */
macro package std.unittest.testmacro

import std.ast.*

/**
 * @ConcurrentTest macro declares test cases for a specific set of concurrent
 * data structure operations.
 */
public macro ConcurrentTest(input: Tokens): Tokens {
    return topLevelMacroDriver(CONCURRENT_TEST_MACRO, input, Tokens(), { it => insideParentContext(it.macroIdentifier) })
}

public macro ConcurrentTest(dsl: Tokens, input: Tokens): Tokens {
    return topLevelMacroDriver(CONCURRENT_TEST_MACRO, input, dsl, { it => insideParentContext(it.macroIdentifier) })
}
