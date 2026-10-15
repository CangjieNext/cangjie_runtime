/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
 * This source file is part of the Cangjie project, licensed under Apache-2.0
 * with Runtime Library Exception.
 *
 * See https://cangjie-lang.cn/pages/LICENSE for license information.
 */

package std.core

public enum Option<T> {
    Some(T)
    | None

    /*
     * @throws NoneValueException if the value of Option is None
     */
    @Frozen
    public func getOrThrow(): T {
        match (this) {
            case Some(v) => v
            case None => throw NoneValueException()
        }
    }

    /*
     * @throws NoneValueException if the value of Option is None
     */
    @Frozen
    public exclave func getOrThrow(this @local!): T @local! {
        match (this) {
            case Some(v) => v
            case None => throw NoneValueException()
        }
    }

    /*
     * @throws input Exception if the value of Option is None
     */
    @Frozen
    public func getOrThrow(exception: () -> Exception): T {
        match (this) {
            case Some(v) => v
            case None => throw exception()
        }
    }

    /*
     * @throws input Exception if the value of Option is None
     */
    @Frozen
    public exclave func getOrThrow(this @local!, exception: () -> Exception): T @local! {
        match (this) {
            case Some(v) => v
            case None => throw exception()
        }
    }

    @Frozen
    public func getOrDefault(other: () -> T): T {
        match (this) {
            case Some(v) => v
            case None => other()
        }
    }

    @Frozen
    public func isNone(): Bool {
        match (this) {
            case None => true
            case Some(_) => false
        }
    }

    @Frozen
    public func isNone(this @local?): Bool {
        match (this) {
            case None => true
            case Some(_) => false
        }
    }

    @Frozen
    public func isSome(): Bool {
        match (this) {
            case Some(_) => true
            case None => false
        }
    }

    @Frozen
    public func isSome(this @local?): Bool {
        match (this) {
            case Some(_) => true
            case None => false
        }
    }

    /*
     * @throws NoneValueException if the value of Option is None
     */
    /*
     * @throws input Exception if the value of Option is None
     */
    public func map<R>(transform: (T) -> R): Option<R> {
        match (this) {
            case Some(v) => Some(transform(v))
            case None => None
        }
    }

    public func filter(predicate: (T) -> Bool): Option<T> {
        let v = this ?? return None
        if (predicate(v)) {
            return v
        }
        return None
    }

    public func flatMap<R>(transform: (T) -> Option<R>): Option<R> {
        match (this) {
            case Some(v) => transform(v)
            case None => None
        }
    }
}

public func ifSome<T>(o: Option<T>, action: (T) -> Unit): Unit {
    match (o) {
        case Some(v) => action(v)
        case None => ()
    }
}

public func ifNone<T>(o: Option<T>, action: () -> Unit): Unit {
    match (o) {
        case Some(v) => ()
        case None => action()
    }
}

extend<T> Option<T> <: Equatable<Option<T>> where T <: Equatable<T> {
    @Frozen
    public operator func ==(other: Option<T>): Bool {
        return match ((this, other)) {
            case (Some(v1), Some(v2)) => v1 == v2
            case (None, None) => true
            case _ => false
        }
    }

    @Frozen
    public operator func !=(other: Option<T>): Bool {
        return !(this == other)
    }
}

extend<T> Option<T> <: ToString where T <: ToString {
    public func toString(): String {
        match (this) {
            case Some(s) => "Some(${s})"
            case None => "None"
        }
    }
    // local-mode @local? toString【暂缓登记 2026-09-17,实测复现】:
    // 实现"exclave + StringBuilder @local! + append(v)"后运行即 StackOverflowError:
    //   (a) sb.append(v)/append(字面量) 的参数传递触发编译器 call-prelude 装箱
    //       (NEW-R1 同族),装箱的 MCC_NewLocalObject 分配失败重试耗尽 CJ 栈;
    //   (b) String 字面量→String @local! 的隐式转换函数生成为无界自递归
    //       (gdb 实测 0x60a44→0x60bc0→0x60a44 无限深帧);
    //   (c) 泛型 T @local? 上的 v.toString() 分派落错 vtable(NEW-C1 残余)。
    // 对照:Array.toString(this @local?) 同模板当前编译器亦崩(2026-09-17 实测,
    // 0914 SDK 同)——该缺陷先于本登记存在。恢复前提 = (a)(b) 修复;
    // 参考 Ordering/CString 的成功路径(直接 return 字面量 / 纯字节拷贝,零 append)。
}

extend<T> Option<T> <: Hashable where T <: Hashable {
    public func hashCode(): Int64 {
        return match (this) {
            case Some(v) => v.hashCode()
            case None => 0
        }
    }

    // local-mode @local? 覆写(2026-09-18 补齐 G8):NEW-R1 修复后 local 分配可用。
    public func hashCode(this @local?): Int64 {
        return match (this) {
            case Some(v) => v.hashCode()
            case None => 0
        }
    }
}

extend<T> Option<Option<T>> {
    public func flatten(): Option<T> {
        match (this) {
            case Some(o) => o
            case None => None
        }
    }
}
