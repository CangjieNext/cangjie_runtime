/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
 * This source file is part of the Cangjie project, licensed under Apache-2.0
 * with Runtime Library Exception.
 *
 * See https://cangjie-lang.cn/pages/LICENSE for license information.
 */

package std.core

/**
 * Thrown when the runtime has run out of memory and the garbage collector cannot free sufficient memory.
 */
public class OutOfMemoryError <: Error {
    /**
     * Constructs an OutOfMemoryError with no detail message.
     */
    init() {
        super()
    }

    /**
     * Constructs an OutOfMemoryError with the specified detail message.
     *
     * @param message the detail message
     */
    init(message: String) {
        super(message)
    }

    protected override func getClassName(): String {
        return "OutOfMemoryError"
    }
}
