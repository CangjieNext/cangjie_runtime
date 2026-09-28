/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
 * This source file is part of the Cangjie project, licensed under Apache-2.0
 * with Runtime Library Exception.
 *
 * See https://cangjie-lang.cn/pages/LICENSE for license information.
 */

package std.core

// @local! 默认体(==/!=)是 @local! 操作数相等比较的编译/分派面(HashMap @local! 泛型 == 依赖),刻意保留;审计 §5.2。
public interface Equal<T> {
    operator func ==(other: T): Bool
    operator func ==(this @local!, other: T @local!): Bool {
        throw UnimplementedException("Current Type does not implement '==' method for its @local! instance")
    }
    operator func ==(this @local?, other: T @local?): Bool {
        throw UnimplementedException("Current Type does not implement '==' method for its @local? instance")
    }
}

public interface NotEqual<T> {
    operator func !=(other: T): Bool
    operator func !=(this @local!, other: T @local!): Bool {
        throw UnimplementedException("Current Type does not implement '!=' method for its @local! instance")
    }
    operator func !=(this @local?, other: T @local?): Bool {
        throw UnimplementedException("Current Type does not implement '!=' method for its @local? instance")
    }
}

public interface Equatable<T> <: Equal<T> & NotEqual<T> {
    operator func !=(other: T): Bool {
        !(this == other)
    }
    operator func !=(this @local!, other: T @local!): Bool {
        // exclave { !(this == other) }
        throw UnimplementedException("Current Type does not implement '!=' method for its @local! instance")
    }
    // URL 文档 §8：Equatable 的 != @local? 默认实现——委托 ==（== @local? 覆写或其
    // 默认链），不再抛异常。类型只实现 == 时 != 经此默认体推导；两者皆未适配时
    // 经 == @local? 默认体抛 UnimplementedException。
    operator func !=(this @local?, other: T @local?): Bool {
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
