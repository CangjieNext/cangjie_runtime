/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
 * This source file is part of the Cangjie project, licensed under Apache-2.0
 * with Runtime Library Exception.
 *
 * See https://cangjie-lang.cn/pages/LICENSE for license information.
 */

package std.core

/**
 * The root interface of the Cangjie type hierarchy.
 * All types implicitly implement Any.
 */
public interface Any {}

/**
 * The base class for all reference types in Cangjie.
 */
public open class Object <: Any {
    public const init() {}
}

/**
 * Checks whether two Objects reference the same memory block.
 *
 * @param a the first object to compare
 * @param b the second object to compare
 * @returns Bool true if both references point to the same object
 *
 * @since 0.21.4
 */
public func refEq(a: Object, b: Object): Bool {
    return intrinsicRefEq(a, b)
}

// This function is only used by ArrayList type, and it is not intended to be documented in users' guide
@Intrinsic
public unsafe func zeroValue<T>(): T

@Intrinsic
func intrinsicRefEq(a: Object, b: Object): Bool

@When[backend == "cjnative"]
@Intrinsic
func intrinsicRefEqRawArray<T>(a: RawArray<T>, b: RawArray<T>): Bool
