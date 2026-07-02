/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
 * This source file is part of the Cangjie project, licensed under Apache-2.0
 * with Runtime Library Exception.
 *
 * See https://cangjie-lang.cn/pages/LICENSE for license information.
 */
macro package std.unittest.testmacro

import std.ast.*

/**
 *
 * The macro only add inner macro @Attribute to outer macro Test.
 * The macro can be only defined at func.
 *
 * @since TODO
 * @see ConcurrentTest
 *
 * @throws MacroException if input is not FuncDecl.
 */
public macro StressTest(input: Tokens): Tokens {
    return topLevelMacroDriver(STRESS_TEST_MACRO, input, Tokens(), { it => insideParentContext(it.macroIdentifier) })
}

public macro StressTest(args: Tokens, input: Tokens): Tokens {
    return topLevelMacroDriver(STRESS_TEST_MACRO, input, args, { it => insideParentContext(it.macroIdentifier) })
}
