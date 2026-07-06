/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
 * This source file is part of the Cangjie project, licensed under Apache-2.0
 * with Runtime Library Exception.
 *
 * See https://cangjie-lang.cn/pages/LICENSE for license information.
 */

package std.unittest.concurrent

public interface EquatableObj <: ToString {

    func eq(other: Any): Bool
}

public func execResultOf<T>(f: () -> T): EquatableObj where T <: Equatable<T> {
    try {
        let result = f()
        EqProxy(result)
    } catch (e: Exception) {
        EqProxy(e)
    }
}


class EqProxy<T> <: EquatableObj {

    EqProxy(let value: T) {}

    public func eq(other: Any): Bool {
        if (let Some(other) <- (other as EqProxy<T>)) {
            return match ((value, other.value)) {
                case (e1: Exception, e2: Exception) => e1.message == e2.message
                case (_, o: Equatable<T>) => o == value
                case _ => false
            }
        }
        return false
    }

    public func toString(): String {
        match (value) {
            case v: ToString => v.toString()
            case _ => "<non-stringifiable>"
        }
    }

}