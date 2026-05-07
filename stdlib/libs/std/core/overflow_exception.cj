/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
 * This source file is part of the Cangjie project, licensed under Apache-2.0
 * with Runtime Library Exception.
 *
 * See https://cangjie-lang.cn/pages/LICENSE for license information.
 */

package std.core

let MAX_INT_NATIVE: IntNative = IntNative(IntNative.Max)
let MIN_INT_NATIVE: IntNative = IntNative(IntNative.Min)
let MAX_UINT_NATIVE: UIntNative = UIntNative(UIntNative.Max)

/**
 * Thrown when an arithmetic operation results in an overflow.
 */
public class OverflowException <: ArithmeticException {
    /**
     * Constructs an OverflowException with no detail message.
     */
    public init() {
        super()
    }

    /**
     * Constructs an OverflowException with the specified detail message.
     *
     * @param message the detail message
     */
    public init(message: String) {
        super(message)
    }

    protected override func getClassName(): String {
        return "OverflowException"
    }
}
