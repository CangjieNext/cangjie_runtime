/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
 * This source file is part of the Cangjie project, licensed under Apache-2.0
 * with Runtime Library Exception.
 *
 * See https://cangjie-lang.cn/pages/LICENSE for license information.
 */

package std.core

/**
 * The root interface in the collection hierarchy.
 * A collection represents a group of objects, known as its elements.
 * Some collections allow duplicate elements and others do not.
 * Some are ordered and others unordered.
 */
public interface Collection<T> <: Iterable<T> {
    /**
     * The number of elements in this collection.
     *
     * @type Int64
     */
    prop size: Int64

    /**
     * Checks if this collection contains no elements.
     *
     * @returns Bool true if this collection contains no elements, false otherwise
     */
    func isEmpty(): Bool

    /**
     * Returns an array containing all elements in this collection.
     *
     * @returns Array<T> an array of all elements
     */
    func toArray(): Array<T> {
        return Array<T>(this)
    }
}
