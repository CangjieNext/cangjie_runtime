/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
 * This source file is part of the Cangjie project, licensed under Apache-2.0
 * with Runtime Library Exception.
 *
 * See https://cangjie-lang.cn/pages/LICENSE for license information.
 */

package std.collection.concurrent

import std.sync.*
import std.time.*

@Deprecated[message: "Use LinkedBlockingQueue instead."]
public type BlockingQueue<E> = LinkedBlockingQueue<E>

/**
 * BlockingQueue is a concurrent queue with a blocking mechanism and supports a user-specified upper bound of capacity.
 *
 * Performance optimization note:
 * The previous implementation acquired a `Monitor` for *every* `tryAdd` /
 * `tryRemove` call, even when the queue was neither full nor empty. That forced
 * all producers to serialize on one lock and all consumers on another, which
 * dominated the cost of high-throughput producer/consumer workloads.
 *
 * This version keeps the same capacity-bounded, blocked-on-full/empty semantics,
 * but makes the common (non-full / non-empty) path fully lock-free:
 *   - Producers reserve a slot with a CAS on `reserved` (bounded by `capacity`),
 *     commit the element, then publish it with a `count.fetchAdd`.
 *   - Consumers claim an element with a CAS on `count`, then dequeue it and free
 *     the slot with `reserved.fetchSub`.
 * The two `Monitor`s are now used *only* to park threads when the queue is full
 * (producers) or empty (consumers), and to deliver the wake-up signal. This
 * decouples producers from consumers on the hot path and removes the per-op
 * monitor round-trips.
 *
 * IMPORTANT: signalNotEmpty / signalNotFull are called AFTER releasing the
 * putMonitor / takeMonitor to avoid ABBA deadlock (add holds putMonitor and
 * would acquire takeMonitor via signalNotEmpty, while remove holds takeMonitor
 * and would acquire putMonitor via signalNotFull).
 */
public class LinkedBlockingQueue<E> {
    public let capacity: Int64 // the capacity of the queue
    private let takeMonitor = Monitor() // Monitor for blocking consumers when the queue is empty
    private let putMonitor = Monitor() // Monitor for blocking producers when the queue is full
    private let front: AtomicReference<Block<E>> // head of BlockingQueue.
    private let rear: AtomicReference<Block<E>> // tail of BlockingQueue.
    private let count = AtomicInt64(0) // Number of committed elements in the queue.
    private let reserved = AtomicInt64(0) // Number of reserved slots (<= capacity).

    public init(capacity: Int64) {
        if (capacity <= 0) {
            throw IllegalArgumentException("Invalid capacity '${capacity}': expected a positive value.")
        }

        this.capacity = capacity
        let block = Block<E>(0)
        front = AtomicReference<Block<E>>(block)
        rear = AtomicReference<Block<E>>(block)
    }

    public init() {
        this(0x7FFF_FFFF_FFFF_FFFF) // Int64.Max
    }

    @Deprecated[message: "Use the other version of `init` instead."]
    public init(capacity: Int64, elements: Array<E>) {
        if (capacity <= 0) {
            throw IllegalArgumentException("Invalid capacity '${capacity}': expected a positive value.")
        }

        if (capacity < elements.size) {
            throw IllegalArgumentException("Invalid capacity '${capacity}': expected at least '${elements.size}'.")
        }

        this.capacity = capacity
        let block = Block<E>(0)
        front = AtomicReference<Block<E>>(block)
        rear = AtomicReference<Block<E>>(block)

        for (e in elements) {
            tryEnqueue(e)
        }
    }

    @Deprecated[message: "Use the other version of `init` instead."]
    public init(capacity: Int64, elements: Collection<E>) {
        if (capacity <= 0) {
            throw IllegalArgumentException("Invalid capacity '${capacity}': expected a positive value.")
        }

        if (capacity < elements.size) {
            throw IllegalArgumentException("Invalid capacity '${capacity}': expected at least '${elements.size}'.")
        }

        this.capacity = capacity
        let block = Block<E>(0)
        front = AtomicReference<Block<E>>(block)
        rear = AtomicReference<Block<E>>(block)

        for (e in elements) {
            tryEnqueue(e)
        }
    }

    /**
     * Returns the number of elements in the BlockingQueue.
     *
     * It ensures to return the accurate number of elements in the BlockingQueue in sequential.
     * Note that getting 'size' prop is not atomic!
     * So, if elements are added or removed during execution of getting 'size',
     * the 'size' may be inaccurate.
     */
    public prop size: Int64 {
        get() {
            return count.load()
        }
    }

    /**
     * Blocking method to send the element @p element into the tail of the queue.
     * If there is no free space in the queue, the thread calling such method will be blocked,
     * and waiting for another thread calling 'dequeue' method to await.
     *
     * @param element: the element to send into the queue.
     *
     * A successful 'enqueue' will await the threads blocked by calling 'dequeue'.
     */
    @Deprecated[message: "Use member function `public func add(element: E): Unit` instead."]
    public func enqueue(element: E): Unit {
        add(element)
    }

    public func add(element: E): Unit {
        // 无锁快路径：直接尝试入队
        if (tryAdd(element)) {
            // 入队成功后在锁外通知消费者，避免 putMonitor→takeMonitor 交叉加锁死锁
            signalNotEmpty()
            return
        }
        putMonitor.lock()
        try {
            while (!tryAddLocked(element)) {
                let _ = putMonitor.wait()
            }
        } finally {
            putMonitor.unlock()
        }
        // 锁已释放，安全通知消费者
        signalNotEmpty()
    }

    /**
     * Blocking method to send the element @p element to the tail of the queue,
     * waiting up to @p timeout if the BlockingQueue is full.
     *
     * @param element: the element to send into the queue.
     * @param timeout: the waiting time.
     *
     * @return:
     * - true: if @p element is successfully pushed at the tail of the queue.
     * - false: if the waiting time exceeds @p timeout.
     */
    @Deprecated[message: "Use member function `public func add(element: E, timeout: Duration): Bool` instead."]
    public func enqueue(element: E, timeout: Duration): Bool {
        return add(element, timeout)
    }

    public func add(element: E, timeout: Duration): Bool {
        // 无锁快路径
        if (tryAdd(element)) {
            signalNotEmpty()
            return true
        }
        putMonitor.lock()
        try {
            var remaining = timeout
            while (true) {
                if (tryAddLocked(element)) {
                    // 在 finally 释放锁后通知
                    break
                }
                if (remaining <= Duration.Zero) {
                    return false
                }
                let startTime = MonoTime.now()
                let _ = putMonitor.wait(timeout: remaining)
                let endTime = MonoTime.now()
                remaining -= endTime - startTime
            }
        } finally {
            putMonitor.unlock()
        }
        signalNotEmpty()
        return true
    }

    /**
     * Blocking method to get and remove the element at the head of the queue.
     * If the queue is empty, the thread calling such method will be blocked,
     * and waiting for another thread calling 'enqueue' method to await.
     *
     * A successful 'dequeue' will await blocked threads calling 'enqueue'.
     *
     * @return:
     * - e: e is the element at the head of the queue.
     */
    @Deprecated[message: "Use member function `public func remove(): E` instead."]
    public func dequeue(): E {
        return remove()
    }

    public func remove(): E {
        // 无锁快路径
        if (let Some(e) <- tryRemove()) {
            signalNotFull()
            return e
        }
        takeMonitor.lock()
        var result: Option<E> = None<E>
        try {
            while (true) {
                if (let Some(e) <- tryRemoveLocked()) {
                    result = Some(e)
                    break
                }
                let _ = takeMonitor.wait()
            }
        } finally {
            takeMonitor.unlock()
        }
        // 锁已释放，安全通知生产者
        signalNotFull()
        return result.unwrap()
    }

    /**
     * Blocking method to get and remove the head of the queue,
     * waiting up to @p timeout if the BlockingQueue is empty.
     *
     * @param timeout: the waiting time.
     *
     * @return:
     * - Some(v): if gets and removes the element at the head of the queue successfully.
     * - None: if the waiting time exceeds @p timeout.
     */
    @Deprecated[message: "Use member function `public func remove(timeout: Duration): Option<E>` instead."]
    public func dequeue(timeout: Duration): Option<E> {
        return remove(timeout)
    }

    public func remove(timeout: Duration): Option<E> {
        // 无锁快路径
        if (let Some(e) <- tryRemove()) {
            signalNotFull()
            return Some(e)
        }
        var success = false
        var result: Option<E> = None<E>
        takeMonitor.lock()
        try {
            var remaining = timeout
            while (true) {
                if (let Some(e) <- tryRemoveLocked()) {
                    result = Some(e)
                    success = true
                    break
                }
                if (remaining <= Duration.Zero) {
                    return None<E>
                }
                let startTime = MonoTime.now()
                let _ = takeMonitor.wait(timeout: remaining)
                let endTime = MonoTime.now()
                remaining -= endTime - startTime
            }
        } finally {
            takeMonitor.unlock()
        }
        if (success) {
            signalNotFull()
        }
        return result
    }

    /**
     * Gets the head of the queue, but does not remove it.
     * Such method is non-blocking.
     *
     * @return:
     * - Some(v): if we successfully gets the element at the head of the queue.
     * - None: if the queue is empty, and we fails to get the head of the queue.
     */
    @Deprecated[message: "Use member function `public func peek(): Option<E>` instead."]
    public func head(): Option<E> {
        return peek()
    }

    public func peek(): Option<E> {
        while (true) {
            let headBlock = front.load()
            match (headBlock.front()) {
                case ELEM(e) => return Some(e)
                case NoEntry => break
                case BlockDone =>
                    if (!nextBlock<E>(headBlock, front, rear)) {
                        break
                    }
            }
        }
        return None<E>
    }

    /**
     * Non-blocking method to send the element @p element to the tail of the queue.
     * A successful 'tryEnqueue' will await blocked threads calling 'dequeue'.
     *
     * @param element: the element to send into the queue.
     *
     * @return:
     * - true: if @p element is successfully pushed at the tail of the queue.
     * - false: if the queue is full.
     */
    @Deprecated[message: "Use member function `public func tryAdd(element: E): Bool` instead."]
    public func tryEnqueue(element: E): Bool {
        return tryAdd(element)
    }

    // 无锁快路径版 tryAdd：不在任何 Monitor 锁内调用
    public func tryAdd(element: E): Bool {
        while (true) {
            let r = reserved.load()
            if (r >= capacity) {
                return false
            }
            if (reserved.compareAndSwap(r, r + 1)) {
                enqueueUnsafe(element)
                count.fetchAdd(1)
                return true
            }
        }
    }

    // 锁内版 tryAdd：已在 putMonitor 保护下，只做 CAS + 入队，不发送信号
    private func tryAddLocked(element: E): Bool {
        let r = reserved.load()
        if (r >= capacity) {
            return false
        }
        // 锁内无需 CAS 循环，putMonitor 已保证串行
        reserved.fetchAdd(1)
        enqueueUnsafe(element)
        count.fetchAdd(1)
        return true
    }

    /**
     * Non-blocking method to get and remove the head of the queue.
     * A successful 'tryDequeue' will await blocked threads calling 'enqueue'.
     *
     * @return:
     * - Some(v): if gets and removes the element at the head of the queue successfully.
     * - None: if the queue is empty.
     */
    @Deprecated[message: "Use member function `public func tryRemove(): Option<E>` instead."]
    public func tryDequeue(): Option<E> {
        return tryRemove()
    }

    // 无锁快路径版 tryRemove：不在任何 Monitor 锁内调用
    public func tryRemove(): Option<E> {
        while (true) {
            let c = count.load()
            if (c == 0) {
                return None<E>
            }
            if (count.compareAndSwap(c, c - 1)) {
                let value = dequeueUnsafe()
                reserved.fetchSub(1)
                return Some(value)
            }
        }
    }

    // 锁内版 tryRemove：已在 takeMonitor 保护下，只做 CAS + 出队，不发送信号
    private func tryRemoveLocked(): Option<E> {
        let c = count.load()
        if (c == 0) {
            return None<E>
        }
        // 锁内无需 CAS 循环，takeMonitor 已保证串行
        count.fetchSub(1)
        let value = dequeueUnsafe()
        reserved.fetchSub(1)
        return Some(value)
    }

    private func enqueueUnsafe(element: E): Unit {
        while (true) {
            let tailBlock = rear.load()
            let (_, tailAllocated) = tailBlock.getAllocated()
            match (tailBlock.pushAt(element, tailAllocated)) {
                case SUCCEED => return
                case BlockDone => newBlock<E>(tailBlock, rear)
                case CONFLICT => ()
            }
        }
    }

    private func dequeueUnsafe(): E {
        while (true) {
            let headBlock = front.load()
            match (headBlock.pop()) {
                case ELEM(e) => return e
                case NoEntry => ()
                case BlockDone =>
                    if (!nextBlock<E>(headBlock, front, rear)) {
                        break
                    }
            }
        }
        throw Exception("Unreachable code.")
    }

    private func signalNotEmpty(): Unit {
        takeMonitor.lock()
        try {
            takeMonitor.notify()
        } finally {
            takeMonitor.unlock()
        }
    }

    private func signalNotFull(): Unit {
        putMonitor.lock()
        try {
            putMonitor.notify()
        } finally {
            putMonitor.unlock()
        }
    }
}
