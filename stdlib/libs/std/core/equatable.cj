/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
 * This source file is part of the Cangjie project, licensed under Apache-2.0
 * with Runtime Library Exception.
 *
 * See https://cangjie-lang.cn/pages/LICENSE for license information.
 */

package std.core

/**
 * Interface for types that support the equality operator.
 */
public interface Equal<T> {
    /**
     * Checks if this value is equal to the other.
     *
     * @param other the value to compare with
     * @returns Bool true if this is equal to other
     */
    operator func ==(other: T): Bool
}

/**
 * Interface for types that support the inequality operator.
 */
public interface NotEqual<T> {
    /**
     * Checks if this value is not equal to the other.
     *
     * @param other the value to compare with
     * @returns Bool true if this is not equal to other
     */
    operator func !=(other: T): Bool
}

/**
 * Interface for types that support both equality and inequality comparisons.
 */
public interface Equatable<T> <: Equal<T> & NotEqual<T> {
    operator func !=(other: T): Bool {
        !(this == other)
    }
}

extend Bool <: Equatable<Bool> {}

extend Unit <: Equatable<Unit> {}

extend<T> Range<T> <: Equatable<Range<T>> where T <: Countable<T> & Comparable<T> & Equatable<T> {
    public operator func ==(other: Range<T>): Bool {
        this.start == other.start && this.end == other.end && this.step == other.step && this.isClosed == other.isClosed
    }
}
