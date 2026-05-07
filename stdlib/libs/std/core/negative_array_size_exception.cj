/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
 * This source file is part of the Cangjie project, licensed under Apache-2.0
 * with Runtime Library Exception.
 *
 * See https://cangjie-lang.cn/pages/LICENSE for license information.
 */

package std.core

/**
 * Thrown when an application attempts to create an array with a negative size.
 */
public class NegativeArraySizeException <: Exception {
    /**
     * Constructs a NegativeArraySizeException with no detail message.
     */
    public init() {
        super()
    }

    /**
     * Constructs a NegativeArraySizeException with the specified detail message.
     *
     * @param message the detail message
     */
    public init(message: String) {
        super(message)
    }

    protected override func getClassName(): String {
        return "NegativeArraySizeException"
    }
}
