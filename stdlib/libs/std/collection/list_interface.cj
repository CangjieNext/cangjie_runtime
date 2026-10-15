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
    // gap audit 2026-09-24 #2–5：first/last 的 @local!/@local? 接口声明（doc §10），
    // 抽象声明。ArrayList 已有四形态实现（array_list.cj first/last prop 组），静态
    // ReadOnlyList 接收者经此分派到具体覆写（NEW-C1 已修复，2026-09-24 探针验证）。
    // 注：不能用带 throw 默认体的形态——默认体使 @local? 成员成为"具体"候选，
    // 重载决议在任意接收者模式（含 plain）下都压过 plain prop（探针 probe_f 实测：
    // plain 接收者 first 绑到 @local? 版，显式 Option<T> 标注也无法选回 plain，
    // test_readonlylist_local.cj 的 plain 契约被破坏）；抽象声明无此问题（probe_e 实测）。
    // 代价：非 ArrayList 的 ReadOnlyList/List 泛型 conformer 需补实现义务
    // （本仓库内仅 std.sort 测试的 MyArrayList <: List 两处，已同步补齐）。
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

