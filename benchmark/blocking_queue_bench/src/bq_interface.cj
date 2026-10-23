/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
 * This source file is part of the Cangjie project, licensed under Apache-2.0
 * with Runtime Library Exception.
 *
 * See https://cangjie-lang.cn/pages/LICENSE for license information.
 */

package blocking_queue_bench

import std.time.*

// 统一接口，让 LinkedBlockingQueue 与 ArrayBlockingQueue 的测试副本都实现，
// 以便用同一套泛型压力测试覆盖两种队列。
public interface BQueue<E> {
    public func add(element: E): Unit
    public func add(element: E, timeout: Duration): Bool
    public func remove(): E
    public func remove(timeout: Duration): Option<E>
    public func tryAdd(element: E): Bool
    public func tryRemove(): Option<E>
    public prop size: Int64
}
