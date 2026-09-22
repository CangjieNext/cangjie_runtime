/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
 * This source file is part of the Cangjie project, licensed under Apache-2.0
 * with Runtime Library Exception.
 *
 * See https://cangjie-lang.cn/pages/LICENSE for license information.
 */

package std.core

public class Box<T> {
    public var value: T
    @Frozen
    public init(v: T) {
        value = v
    }
}

extend<T> Box<T> <: Hashable where T <: Hashable {
    public func hashCode(): Int64 {
        return this.value.hashCode()
    }

    // local-mode @local? 覆写(2026-09-18 补齐 G8)。
    public func hashCode(this @local?): Int64 {
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
     * Compare the relationship between two instance of Box<T>.
     *
     * @param other Instance of Box<T> compared with this.
     * @return Value indicating the relationship between two instance of Box<T>.
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
    // local-mode @local? toString【暂缓登记 2026-09-17,实测复现】:
    // 同 Option——append(this.value) 触发 call-prelude 装箱(NEW-R1 同族),
    // 泛型 T @local? 的 toString 分派落错 vtable(NEW-C1 残余),运行即
    // StackOverflowError。恢复前提同 Option 登记的三项编译器修复。
}
