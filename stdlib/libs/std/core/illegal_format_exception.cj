/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
 * This source file is part of the Cangjie project, licensed under Apache-2.0
 * with Runtime Library Exception.
 *
 * See https://cangjie-lang.cn/pages/LICENSE for license information.
 */

package std.core

/**
 * Thrown to indicate that a format string contains illegal syntax or an incompatible format argument.
 */
public open class IllegalFormatException <: IllegalArgumentException {
    /**
     * Constructs an IllegalFormatException with no detail message.
     */
    public init() {
        super()
    }

    /**
     * Constructs an IllegalFormatException with the specified detail message.
     *
     * @param message the detail message
     */
    public init(message: String) {
        super(message)
    }

    protected override func getClassName(): String {
        return "IllegalFormatException"
    }
}
