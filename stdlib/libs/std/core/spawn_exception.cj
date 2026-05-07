/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
 * This source file is part of the Cangjie project, licensed under Apache-2.0
 * with Runtime Library Exception.
 *
 * See https://cangjie-lang.cn/pages/LICENSE for license information.
 */

package std.core

/**
 * Thrown when a spawn operation fails.
 */
public class SpawnException <: Exception {
    /**
     * Constructs a SpawnException with no detail message.
     */
    public init() {
        super()
    }

    /**
     * Constructs a SpawnException with the specified detail message.
     *
     * @param message the detail message
     */
    public init(message: String) {
        super(message)
    }

    protected override func getClassName(): String {
        return "SpawnException"
    }
}
