/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
 * This source file is part of the Cangjie project, licensed under Apache-2.0
 * with Runtime Library Exception.
 *
 * See https://cangjie-lang.cn/pages/LICENSE for license information.
 */

package std.core

/**
 * Interface for resources that can be closed.
 */
public interface Resource {
    /**
     * Checks if this resource has been closed.
     *
     * @returns Bool true if the resource is closed, false otherwise
     */
    func isClosed(): Bool

    /**
     * Closes this resource and releases any associated resources.
     */
    func close(): Unit
}
