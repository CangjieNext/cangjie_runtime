/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
 * This source file is part of the Cangjie project, licensed under Apache-2.0
 * with Runtime Library Exception.
 *
 * See https://cangjie-lang.cn/pages/LICENSE for license information.
 */

package std.core

/**
 * Thrown when a package compatibility check fails at runtime.
 */
public class IncompatiblePackageException <: Exception {
    /**
     * Constructs an IncompatiblePackageException with no detail message.
     */
    public init() {
        super()
    }

    /**
     * Constructs an IncompatiblePackageException with the specified detail message.
     *
     * @param message the detail message
     */
    public init(message: String) {
        super(message)
    }

    protected override func getClassName(): String {
        return "IncompatiblePackageException"
    }
}
