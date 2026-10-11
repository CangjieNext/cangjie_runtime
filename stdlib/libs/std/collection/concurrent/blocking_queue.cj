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
 *   - Producers reserve a slot with a CAS on `putQsize` (bounded by `capacity`),
 *     commit the element, then publish it with a `count.fetchAdd`.
 *   - Consumers claim an element with a CAS on `count`, then dequeue it and free
 *     the slot with `putQsize.fetchSub`.
 *
 * `putQsize` is reused as the reservation counter on purpose: it was already
 * declared (but unused) upstream with exactly this meaning, so reusing it keeps
 * the field order and types of this class unchanged and therefore keeps the
 * ABI of std.collection.concurrent intact.
 * The two `Monitor`s are now used *only* to park threads when the queue is full
 * (producers) or empty (consumers), and to deliver the wake-up signal. This
 * decouples producers from consumers on the hot path and removes the per-op
 * monitor round-trips.
 *
 * IMPORTANT: signalNotEmpty / signalNotFull are called AFTER releasing the
 * putMonitor / takeMonitor to avoid ABBA deadlock (add holds putMonitor and
 * would acquire takeMonitor via signalNotEmpty, while remove holds takeMonitor
 * and would acquire putMonitor via signalNotFull).
 *
 * IMPORTANT (capacity/element counters): every mutation of `putQsize` / `count`
 * must go through the SAME CAS-based atomic protocol used by the lock-free fast
 * path, no matter whether the caller is on the fast path or on the blocking
 * path. The putMonitor / takeMonitor only serialize the *blocking* producers or
 * consumers among themselves; they do NOT exclude a thread that is concurrently
 * running the lock-free fast path. Using a plain fetchAdd/fetchSub inside the
 * monitors would therefore race with the CAS loop and corrupt the counters
 * (putQsize could exceed capacity, or count could go negative).
 */
public class LinkedBlockingQueue<E> {
    public let capacity: Int64 // the capacity of the queue
    private let takeMonitor = Monitor() // Monitor for blocking consumers when the queue is empty
    private let putMonitor = Monitor() // Monitor for blocking producers when the queue is full
    // putQsize 作为槽位预约计数器：0 <= putQsize <= capacity，用于在无锁快路径上
    // 对"已预约但尚未提交"的槽位进行 bounded 控制，从而把生产者的预约动作和消费者的
    // 认领动作与 Monitor 解耦，只在队列满/空时才落到 Monitor 上阻塞。
    private let putQsize = AtomicInt64(0) // Number of reserved slots (0 <= putQsize <= capacity).
    private let front: AtomicReference<Block<E>> // head of BlockingQueue.
    private let rear: AtomicReference<Block<E>> // tail of BlockingQueue.
    private let count = AtomicInt64(0) // Number of committed elements in the queue.

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
        // 快路径：公开 tryAdd 内部已包含“唤醒消费者”语义，这里不能再重复发信号
        if (tryAdd(element)) {
            return
        }
        putMonitor.lock()
        try {
            // 等待循环里必须复用无锁核心 tryAddCore（用 CAS 预约槽位），不能再用“锁内直接 fetchAdd”的版本：
            // putMonitor 只串行化“走阻塞路径的生产者”，无法与同时走无锁快路径的生产者互斥，
            // 二者并发修改 putQsize 会把它加到超过 capacity，导致队列排空后生产者仍误判为“满”而永久阻塞。
            // 这里也不能改用公开 tryAdd：它会在持有 putMonitor 期间去抢 takeMonitor，
            // 与 remove 侧形成 putMonitor↔takeMonitor 的 ABBA 死锁。
            while (!tryAddCore(element)) {
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
        // 快路径：公开 tryAdd 已负责唤醒消费者
        if (tryAdd(element)) {
            return true
        }
        putMonitor.lock()
        try {
            var remaining = timeout
            while (true) {
                // 同 add(element)：等待循环内只能用无锁核心 tryAddCore，
                // 既保证 putQsize 始终由 CAS 预约，又避免在持有 putMonitor 时去抢 takeMonitor
                if (tryAddCore(element)) {
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
        // 快路径：公开 tryRemove 已负责唤醒生产者
        if (let Some(e) <- tryRemove()) {
            return e
        }
        takeMonitor.lock()
        var result: Option<E> = None<E>
        try {
            while (true) {
                // 等待循环里必须复用无锁核心 tryRemoveCore（用 CAS 认领元素），不能再用“锁内直接 fetchSub”的版本：
                // takeMonitor 只串行化“走阻塞路径的消费者”，无法与同时走无锁快路径的消费者互斥，
                // 二者并发修改 count 会把它减成负数，导致从空队列出队并抛出 "Unreachable code"。
                // 这里也不能改用公开 tryRemove：它会在持有 takeMonitor 期间去抢 putMonitor，形成 ABBA 死锁。
                if (let Some(e) <- tryRemoveCore()) {
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
        return result.getOrThrow()
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
        // 快路径：公开 tryRemove 已负责唤醒生产者
        if (let Some(e) <- tryRemove()) {
            return Some(e)
        }
        var success = false
        var result: Option<E> = None<E>
        takeMonitor.lock()
        try {
            var remaining = timeout
            while (true) {
                // 同 remove()：等待循环内只能用无锁核心 tryRemoveCore，
                // 保证 count 始终由 CAS 认领，且避免在持有 takeMonitor 时去抢 putMonitor
                if (let Some(e) <- tryRemoveCore()) {
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

    // 无锁核心：只做入队与计数，绝不获取任何 Monitor。
    // 之所以把“发信号”拆到外层：发信号需要获取对端 Monitor，而本核心会在 add 的
    // putMonitor 等待循环内被调用，若在持锁时去抢 takeMonitor 就会形成 ABBA 死锁。
    private func tryAddCore(element: E): Bool {
        // 用 break 而非 return 退出"满"分支：Cangjie 中 while(true) 作为函数尾表达式时
        // 类型被判定为 Unit，必须让循环存在可达出口并在循环后统一 return，才能满足 Bool 返回类型
        while (true) {
            let r: Int64 = putQsize.load()
            if (r >= capacity) {
                break
            }
            let next: Int64 = r + 1
            if (putQsize.compareAndSwap(r, next)) {
                enqueueUnsafe(element)
                count.fetchAdd(1)
                return true
            }
        }
        return false
    }

    public func tryAdd(element: E): Bool {
        // 与上游语义对齐：非阻塞入队成功后必须唤醒阻塞在 remove() 上的消费者。
        // 上游 tryAdd 在持有 putMonitor 的临界区内完成这件事；我们改为在无锁核心之外发信号，
        // 否则“只用 tryAdd 生产、用 remove() 消费”的用法会让消费者永久挂起（丢失唤醒）。
        if (tryAddCore(element)) {
            signalNotEmpty()
            return true
        }
        return false
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

    // 无锁核心：只做出队与计数，绝不获取任何 Monitor（原因同 tryAddCore）
    private func tryRemoveCore(): Option<E> {
        // 同 tryAdd：用 break 让 while(true) 具备可达出口，循环后统一 return Option<E>
        while (true) {
            let c: Int64 = count.load()
            if (c == 0) {
                break
            }
            let next: Int64 = c - 1
            if (count.compareAndSwap(c, next)) {
                let value = dequeueUnsafe()
                putQsize.fetchSub(1)
                return Some(value)
            }
        }
        return None<E>
    }

    public func tryRemove(): Option<E> {
        // 与上游语义对齐：非阻塞出队成功后必须唤醒阻塞在 add() 上的生产者，
        // 否则“只用 tryRemove 消费、用 add 生产”的用法（如 ResourcePool）会让生产者永久挂起。
        if (let Some(e) <- tryRemoveCore()) {
            signalNotFull()
            return Some(e)
        }
        return None<E>
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
