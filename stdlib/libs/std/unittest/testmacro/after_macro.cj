/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
 * This source file is part of the Cangjie project, licensed under Apache-2.0
 * with Runtime Library Exception.
 *
 * See https://cangjie-lang.cn/pages/LICENSE for license information.
 */

/**
 * @file The file declares the macro Operation.
 */
macro package std.unittest.testmacro

import std.ast.*

/**
 * @After macro marks the function that will be executed after each scenario
 * iteration. The function can be marked with @After only inside @ConcurrentTest
 * class. There can be not more than one function marked with @After.
 */
public macro After(input: Tokens): Tokens {
    topLevelMacroDriver(AFTER_MACRO, input, Tokens(), { it => insideParentContext(it.macroIdentifier) })
}
