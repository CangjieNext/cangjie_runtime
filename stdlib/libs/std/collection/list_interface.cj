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
    // The @local!/@local? forms of first/last are declared abstract rather than given throwing default
    // bodies: a default body would make an @local? member a "concrete" candidate that outranks the plain
    // prop even on a plain receiver; the cost is that non-ArrayList ReadOnlyList/List conformers must implement them themselves.
    prop first: ?T @local!
    prop first: ?T @local?
    prop last: ?T @local!
    prop last: ?T @local?
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
    func remove(this @local!, range: Range<Int64>): Unit
    func removeIf(this @local!, predicate: ((T @local!) -> Bool) @local?): Unit
    func clear(this @local!): Unit
    operator func [](this @local!, index: Int64, value!: T @local!): Unit
}

