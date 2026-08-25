/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
 * This source file is part of the Cangjie project, licensed under Apache-2.0
 * with Runtime Library Exception.
 *
 * See https://cangjie-lang.cn/pages/LICENSE for license information.
 */
/**
 * @file The file declares the macro StressTest.
 */
macro package std.unittest.testmacro

import std.ast.*

/**
 * @StressTest macro marks function as a concurrent test inside @ConcurrentTest
 * class C. Function must declare first parameter (e.g. named "scenario") of type
 * ScenarioBuilder<C> and generator must be specified to this parameter:
 * @StressTest[scenario in scenarios()].
 */
public macro StressTest(input: Tokens): Tokens {
    return topLevelMacroDriver(STRESS_TEST_MACRO, input, Tokens(), { it => insideParentContext(it.macroIdentifier) })
}

public macro StressTest(args: Tokens, input: Tokens): Tokens {
    return topLevelMacroDriver(STRESS_TEST_MACRO, input, args, { it => insideParentContext(it.macroIdentifier) })
}
