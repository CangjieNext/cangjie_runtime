/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
 * This source file is part of the Cangjie project, licensed under Apache-2.0
 * with Runtime Library Exception.
 *
 * See https://cangjie-lang.cn/pages/LICENSE for license information.
 */

/**
 * @file The file declares the macro Group.
 */
macro package std.unittest.testmacro

import std.ast.*

/**
 * @Group["group"] macro marks the @Operation function, assigning this
 * operation to group "group". Operations in the same group are not executed in
 * parallel.
 */
public macro Group(attr: Tokens, input: Tokens): Tokens {
    topLevelMacroDriver(GROUP_MACRO, input, attr, { it => insideParentContext(it.macroIdentifier) })
}
