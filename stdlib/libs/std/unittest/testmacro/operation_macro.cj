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

public macro Operation(input: Tokens): Tokens {
    topLevelMacroDriver(OPERATION_MACRO, input, Tokens(), { it => insideParentContext(it.macroIdentifier) })
}

/**
 * TODO
 */
public macro Operation(attr: Tokens, input: Tokens): Tokens {
    topLevelMacroDriver(OPERATION_MACRO, input, attr, { it => insideParentContext(it.macroIdentifier) })
}
