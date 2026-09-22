/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
 * This source file is part of the Cangjie project, licensed under Apache-2.0
 * with Runtime Library Exception.
 *
 * See https://cangjie-lang.cn/pages/LICENSE for license information.
 */

/**
 * @file
 *
 * This file defines ReadOnlyList and List interface.
 */
package std.collection

public interface ReadOnlyList<T> <: Collection<T> {
    prop first: ?T
    prop last: ?T
    func get(index: Int64): ?T
    func get(this @local!, index: Int64): ?T @local!
    func get(this @local?, index: Int64): ?T @local?
    operator func [](index: Int64): T
    operator func [](this @local!, index: Int64): T @local!
    operator func [](this @local?, index: Int64): T @local?
}

public interface List<T> <: ReadOnlyList<T> {
    func add(element: T): Unit
    func add(all!: Collection<T>): Unit
    func add(element: T, at!: Int64): Unit
    func add(all!: Collection<T>, at!: Int64): Unit
    func remove(at!: Int64): T
    func remove(range: Range<Int64>): Unit
    func removeIf(predicate: (T) -> Bool): Unit
    func clear(): Unit
    operator func [](index: Int64, value!: T): Unit
    func add(this @local!, element: T @local!): Unit
    func add(this @local!, all!: Collection<T> @local!): Unit
    func add(this @local!, element: T @local!, at!: Int64): Unit
    func add(this @local!, all!: Collection<T> @local!, at!: Int64): Unit
    func remove(this @local!, at!: Int64): T @local!
    func remove(this @local!, range: Range<Int64> @local?): Unit
    func removeIf(this @local!, predicate: ((T @local!) -> Bool) @local?): Unit
    func clear(this @local!): Unit
    operator func [](this @local!, index: Int64, value!: T @local!): Unit
}

/**
 * List interface for @local? instances where T <: Copyable (提案 6.2)。
 * 只包含当前编译器允许在 @local? class 实例上实现的操作：下标读写（改内容而不重绑
 * 字段）、标量成员调整、读取方法。
 *
 * `add` 家族在本接口中声明，但 `@local?` 实现只能覆盖“容量足够”的分支：扩容需要把
 * 新的 `Array<T>` 赋回 `myData`，而 `@local?` 实例禁止重绑 non-Copyable 成员
 * （"cannot assign to member of @local? value of non copy type outside its constructor
 * and finalizer"）。因此需要扩容时按提案 7 节抛 `UnImplementedException`；`reserve`
 * 同样只在容量已足够时为空操作。要真正扩容的调用方需先把实例转成 `@local!`。
 */
public interface ListOfCopyable<T> <: ReadOnlyList<T> where T <: Copyable {
    func add(this @local?, element: T): Unit
    func add(this @local?, all!: Collection<T> @local?): Unit
    func add(this @local?, element: T, at!: Int64): Unit
    func add(this @local?, all!: Collection<T> @local?, at!: Int64): Unit
    func remove(this @local?, at!: Int64): T
    func remove(this @local?, range: Range<Int64> @local?): Unit
    func removeIf(this @local?, predicate: ((T) -> Bool) @local?): Unit
    func clear(this @local?): Unit
    operator func [](this @local?, index: Int64, value!: T): Unit
}