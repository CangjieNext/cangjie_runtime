/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
 * This source file is part of the Cangjie project, licensed under Apache-2.0
 * with Runtime Library Exception.
 *
 * See https://cangjie-lang.cn/pages/LICENSE for license information.
 */

package std.core

/**
 * A wrapper class that boxes a value of any type for use with AtomicReference.
 */
public class Box<T> {
    /**
     * The boxed value.
     *
     * @type T
     */
    public var value: T

    /**
     * Constructs a Box with the given value.
     *
     * @param v the value to box
     */
    @Frozen
    public init(v: T) {
        value = v
    }
}

extend<T> Box<T> <: Hashable where T <: Hashable {
    public func hashCode(): Int64 {
        return this.value.hashCode()
    }
}

extend<T> Box<T> <: Comparable<Box<T>> where T <: Comparable<T> {
    public operator func ==(other: Box<T>): Bool {
        return this.value == other.value
    }
    public operator func !=(other: Box<T>): Bool {
        return this.value != other.value
    }
    public operator func >(other: Box<T>): Bool {
        return this.value > other.value
    }
    public operator func <(other: Box<T>): Bool {
        return this.value < other.value
    }
    public operator func >=(other: Box<T>): Bool {
        return this.value >= other.value
    }
    public operator func <=(other: Box<T>): Bool {
        return this.value <= other.value
    }

    /**
     * Compares the relationship between two Box<T> instances.
     *
     * @param other the Box<T> to compare with this
     * @returns Ordering the ordering relationship between the two Box<T> instances
     *
     * @since 0.27.3
     */
    public func compare(other: Box<T>): Ordering {
        match {
            case this.value < other.value => Ordering.LT
            case this.value > other.value => Ordering.GT
            case _ => Ordering.EQ
        }
    }
}

extend<T> Box<T> <: ToString where T <: ToString {
    public func toString(): String {
        return this.value.toString()
    }
}
