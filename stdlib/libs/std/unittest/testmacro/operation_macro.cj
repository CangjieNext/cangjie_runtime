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
 * @Operation macro marks the function that implements concurrent operation on
 * data structure. Value sets for arguments cab be specified in macro arguments
 * similar to @TestCase: `@Operation[arg1 in values1, arg2 in values2, ...]`.
 */
public macro Operation(input: Tokens): Tokens {
    topLevelMacroDriver(OPERATION_MACRO, input, Tokens(), { it => insideParentContext(it.macroIdentifier) })
}

public macro Operation(attr: Tokens, input: Tokens): Tokens {
    topLevelMacroDriver(OPERATION_MACRO, input, attr, { it => insideParentContext(it.macroIdentifier) })
}
