/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
 * This source file is part of the Cangjie project, licensed under Apache-2.0
 * with Runtime Library Exception.
 *
 * See https://cangjie-lang.cn/pages/LICENSE for license information.
 */

package blocking_queue_bench

import std.sync.*
import std.time.*

// 把无锁核心（tryAddCore / tryRemoveCore）异常路径需要补发的跨 Monitor 唤醒标志带出到外层：
// 异常路径不能持锁发跨锁信号（ABBA），故核心只置位、由外层在 finally 解锁后统一补发。
// 每个调用线程各自 new 一个实例，不存在跨线程竞争。
private class WakeFlags {
    var needWakeConsumer = false
    var needWakeProducer = false
}


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
 * Signal is CONDITIONAL (only when the queue transitions from empty/full),
 * matching upstream's JDK-style signal() semantics: this avoids the extra
 * Monitor round-trip on the SPSC hot path that an unconditional signal would
 * incur, while staying provably free of lost-wakeup (the signal is taken under
 * the same takeMonitor/putMonitor the consumer/producer waits on, so a notify
 * can only land before the waiter acquires that monitor or after it is already
 * parked — never in the check-and-await gap).
 *
 * Chained wake-up (notify of another waiter on the *same* monitor) is also preserved
 * from upstream to avoid MPMC liveness starvation: a successful add with prev+1 < capacity
 * wakes another blocked producer, and a successful remove with prev > 1 wakes another
 * blocked consumer. This complements the conditional cross-monitor signal above.
 *
 * IMPORTANT (producer wake-up predicate must match the reservation-based blocking
 * predicate): a producer blocks when the reservation counter `putQsize` reaches
 * `capacity` (see tryAddCore), so the producer-wakeup signal MUST be derived from the
 * same counter. In tryRemoveCore the slot-release is `let prevPut = putQsize.fetchSub(1)`,
 * and signalNotFull() is triggered only when `prevPut == capacity` (the release took the
 * reservation from full to non-full). Deriving it from the committed `count` (== capacity)
 * instead would mismatch the blocking predicate and could leave a producer parked forever
 * while a free slot exists. The consumer-wakeup signal (signalNotEmpty) stays count-based,
 * matching the consumer's count-based blocking predicate, so it needs no such adjustment.
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
public class LinkedBlockingQueue<E> <: BQueue<E> {
    public let capacity: Int64 // the capacity of the queue
    private let takeMonitor = Monitor() // Monitor for blocking consumers when the queue is empty
    private let putMonitor = Monitor() // Monitor for blocking producers when the queue is full
    // putQsize 作为槽位预约计数器：0 <= putQsize <= capacity，用于在无锁快路径上
    // 对"已预约但尚未提交"的槽位进行 bounded 控制，从而把生产者的预约动作和消费者的
    // 认领动作与 Monitor 解耦，只在队列满/空时才落到 Monitor 上阻塞。
    private let putQsize = AtomicInt64(0) // Number of reserved slots (0 <= putQsize <= capacity).
    private let takeQsize = AtomicInt64(0) // Number of elements that are being dequeued (kept for ABI compatibility).
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
        // 快路径：公开 tryAdd 内部已包含“按需唤醒消费者”语义（仅 prev==0）及 OOM 异常路径补发生产者唤醒，
        // 这里无需重复发信号
        if (tryAdd(element)) {
            return
        }
        putMonitor.lock()
        let wake = WakeFlags()
        var needWakeConsumer = false
        try {
            // 等待循环里必须复用无锁核心 tryAddCore（用 CAS 预约槽位），不能再用“锁内直接 fetchAdd”的版本：
            // putMonitor 只串行化“走阻塞路径的生产者”，无法与同时走无锁快路径的生产者互斥，
            // 二者并发修改 putQsize 会把它加到超过 capacity，导致队列排空后生产者仍误判为“满”而永久阻塞。
            // 这里也不能改用公开 tryAdd：公开 tryAdd 在成功入队后会自行发信号，会与此处重复；
            // 我们用 tryAddCore（纯入队、不发声信号）并在下方解锁后统一发信号，避免重复唤醒与 ABBA。
            while (true) {
                let (ok, prev) = tryAddCore(element, wake)
                if (ok) {
                    // 仅当本次入队前队列为空（prev==0）才唤醒消费者，避免 SPSC 下多余 signal
                    if (prev == 0) {
                        needWakeConsumer = true
                    }
                    // 与上游对齐的链式唤醒：本次入队后队列仍未满（prev+1 < capacity），
                    // 唤醒另一个阻塞在 putMonitor 上的生产者，避免 MPMC 下满队列多生产者永久挂起。
                    if (prev + 1 < capacity) {
                        putMonitor.notify()
                    }
                    break
                }
                let _ = putMonitor.wait()
            }
        } catch (e: Error) {
            // OOM 等异常路径从 tryAddCore 经等待循环抛出：回补预约恰跨越 capacity 边界（prevPut==capacity）
            // 时必须补发 signalNotFull，否则纯排水流量下其余生产者会在空队列上永久挂起（VUL-301）。
            // 此时 finally 已释放 putMonitor，跨锁发信号不会形成 ABBA。
            if (wake.needWakeProducer) {
                signalNotFull()
            }
            throw e
        } catch (e: Exception) {
            if (wake.needWakeProducer) {
                signalNotFull()
            }
            throw e
        } finally {
            putMonitor.unlock()
        }
        // 跨 Monitor 的唤醒（signalNotEmpty 会获取 takeMonitor）必须在释放 putMonitor 之后进行，
        // 否则与 remove 侧（持 takeMonitor 取 putMonitor）形成 putMonitor↔takeMonitor 的 ABBA 死锁。
        // 该唤醒落在消费者等待所用的 takeMonitor 上，不会丢失唤醒（见文件头注释）。
        if (needWakeConsumer) {
            signalNotEmpty()
        }
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
        // 快路径：公开 tryAdd 内部已包含“按需唤醒消费者”语义（仅 prev==0）及 OOM 异常路径补发生产者唤醒，
        // 这里无需重复发信号
        if (tryAdd(element)) {
            return true
        }
        putMonitor.lock()
        let wake = WakeFlags()
        var needWakeConsumer = false
        try {
            var remaining = timeout
            while (true) {
                // 同 add(element)：等待循环内只能用无锁核心 tryAddCore，保证 putQsize 始终由 CAS 预约
                let (ok, prev) = tryAddCore(element, wake)
                if (ok) {
                    if (prev == 0) {
                        needWakeConsumer = true
                    }
                    if (prev + 1 < capacity) {
                        putMonitor.notify()
                    }
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
        } catch (e: Error) {
            // OOM 异常路径：回补预约跨越 capacity 边界时补发 signalNotFull（finally 已释放 putMonitor）。
            if (wake.needWakeProducer) {
                signalNotFull()
            }
            throw e
        } catch (e: Exception) {
            if (wake.needWakeProducer) {
                signalNotFull()
            }
            throw e
        } finally {
            putMonitor.unlock()
        }
        if (needWakeConsumer) {
            signalNotEmpty()
        }
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
        // 快路径：公开 tryRemove 内部已包含“按需唤醒生产者”语义（仅 prev==capacity）及异常路径补发消费者唤醒，
        // 这里无需重复发信号
        if (let Some(e) <- tryRemove()) {
            return e
        }
        takeMonitor.lock()
        let wake = WakeFlags()
        var needWakeProducer = false
        var result: Option<E> = None<E>
        try {
            while (true) {
                // 等待循环里必须复用无锁核心 tryRemoveCore（用 CAS 认领元素），不能再用“锁内直接 fetchSub”的版本：
                // takeMonitor 只串行化“走阻塞路径的消费者”，无法与同时走无锁快路径的消费者互斥，
                // 二者并发修改 count 会把它减成负数，导致从空队列出队并抛出 "Unreachable code"。
                // 这里也不能改用公开 tryRemove：公开 tryRemove 在成功出队后会自行发信号，会与此处重复；
                // 我们用 tryRemoveCore（纯出队、不发声信号）并在下方解锁后统一发信号，避免重复唤醒与 ABBA。
                let (opt, prevCount, prevPut) = tryRemoveCore(wake)
                if (let Some(e) <- opt) {
                    // 仅当本次释放槽位前槽位预约恰为满（prevPut==capacity）才唤醒生产者：
                    // 与生产者阻塞所依据的预约口径（putQsize>=capacity）一致，避免“队列有空位但生产者永久挂起”。
                    if (prevPut == capacity) {
                        needWakeProducer = true
                    }
                    // 与上游对齐的链式唤醒：本次出队前队列元素数 >1（prevCount > 1 表示出队后仍有剩余元素），
                    // 唤醒另一个阻塞在 takeMonitor 上的消费者，避免 MPMC 下消费者永久挂起。
                    if (prevCount > 1) {
                        takeMonitor.notify()
                    }
                    result = Some(e)
                    break
                }
                let _ = takeMonitor.wait()
            }
        } catch (e: Error) {
            // 出队核心异常路径回补认领跨越 0→1（prevCount==0）时，须补发 signalNotEmpty 唤醒其余阻塞消费者。
            // 此时 finally 已释放 takeMonitor，跨锁发信号不会形成 ABBA。
            if (wake.needWakeConsumer) {
                signalNotEmpty()
            }
            throw e
        } catch (e: Exception) {
            if (wake.needWakeConsumer) {
                signalNotEmpty()
            }
            throw e
        } finally {
            takeMonitor.unlock()
        }
        // 跨 Monitor 的唤醒（signalNotFull 会获取 putMonitor）必须在释放 takeMonitor 之后进行，避免 ABBA。
        if (needWakeProducer) {
            signalNotFull()
        }
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
        // 快路径：公开 tryRemove 内部已包含“按需唤醒生产者”语义（仅 prev==capacity）及异常路径补发消费者唤醒，
        // 这里无需重复发信号
        if (let Some(e) <- tryRemove()) {
            return Some(e)
        }
        takeMonitor.lock()
        let wake = WakeFlags()
        var needWakeProducer = false
        var result: Option<E> = None<E>
        try {
            var remaining = timeout
            while (true) {
                // 同 remove()：等待循环内只能用无锁核心 tryRemoveCore，保证 count 始终由 CAS 认领
                let (opt, prevCount, prevPut) = tryRemoveCore(wake)
                if (let Some(e) <- opt) {
                    if (prevPut == capacity) {
                        needWakeProducer = true
                    }
                    if (prevCount > 1) {
                        takeMonitor.notify()
                    }
                    result = Some(e)
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
        } catch (e: Error) {
            // 出队核心异常路径回补认领跨越 0→1（prevCount==0）时，须补发 signalNotEmpty 唤醒其余阻塞消费者。
            if (wake.needWakeConsumer) {
                signalNotEmpty()
            }
            throw e
        } catch (e: Exception) {
            if (wake.needWakeConsumer) {
                signalNotEmpty()
            }
            throw e
        } finally {
            takeMonitor.unlock()
        }
        if (needWakeProducer) {
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
    // 返回 (是否成功, 本次入队前的 count 值 prev)：prev 用于外层判断是否需要唤醒消费者
    // （prev==0，即入队前队列为空，才可能有消费者阻塞在 remove() 上）以及是否需要链式唤醒
    // 其他生产者（prev+1 < capacity 表示入队后仍未满）。
    private func tryAddCore(element: E, wake: WakeFlags): (Bool, Int64) {
        // 用 break 而非 return 退出"满"分支：Cangjie 中 while(true) 作为函数尾表达式时
        // 类型被判定为 Unit，必须让循环存在可达出口并在循环后统一 return，才能满足 (Bool, Int64) 返回类型
        while (true) {
            let r: Int64 = putQsize.load()
            if (r >= capacity) {
                break
            }
            let next: Int64 = r + 1
            if (putQsize.compareAndSwap(r, next)) {
                // enqueueUnsafe 内部可能因 newBlock 堆分配抛异常（如 OOM）；
                // 一旦失败必须把已预约的 putQsize 回补，否则队列容量被永久 -1。
                try {
                    enqueueUnsafe(element)
                    let prev = count.fetchAdd(1)
                    return (true, prev)
                } catch (e: Error) {
                    // 覆盖 newBlock 等堆分配失败抛出的 OutOfMemoryError（<: Error，非 Exception）：
                    // 预约已成功（putQsize +1），入队失败必须把预约回补，否则队列真实容量永久 -1。
                    // 回补恰好使 putQsize 从 capacity 降回 capacity-1（prevPut==capacity）时，等价于一次
                    // “从满释放槽位”的事件，必须补发 signalNotFull（否则纯排水流量下其余生产者会永久挂起，
                    // VUL-301）。跨锁信号由外层在解锁后统一补发，核心内不持 Monitor 发信号（避免 ABBA）。
                    let prevPut = putQsize.fetchSub(1)
                    if (prevPut == capacity) {
                        wake.needWakeProducer = true
                    }
                    throw e
                } catch (e: Exception) {
                    let prevPut = putQsize.fetchSub(1)
                    if (prevPut == capacity) {
                        wake.needWakeProducer = true
                    }
                    throw e
                }
            }
        }
        return (false, 0)
    }

    public func tryAdd(element: E): Bool {
        // 与上游语义对齐：非阻塞入队成功后必须唤醒阻塞在 remove() 上的消费者，
        // 但仅在“入队前队列为空（prev==0）”这一真正可能有消费者等待的情形下才发信号，
        // 避免 SPSC 下每次成功都抢占 takeMonitor 把省下的锁往返又赔回去（即上游的
        // 条件 signal 语义）。signal 在 takeMonitor 下进行，不会丢失唤醒。
        // OOM 异常路径：快路径不持任何 Monitor，回补预约跨越 capacity 边界时直接补发 signalNotFull。
        let wake = WakeFlags()
        try {
            let (ok, prev) = tryAddCore(element, wake)
            if (ok) {
                if (prev == 0) {
                    signalNotEmpty()
                }
                return true
            }
            return false
        } catch (e: Error) {
            if (wake.needWakeProducer) {
                signalNotFull()
            }
            throw e
        } catch (e: Exception) {
            if (wake.needWakeProducer) {
                signalNotFull()
            }
            throw e
        }
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

    // 无锁核心：只做出队与计数，绝不获取任何 Monitor（原因同 tryAddCore）。
    // 返回 (出队结果, 出队前的 count 值 prevCount, 释放槽位前的 putQsize 值 prevPut)：
    //  - prevCount 用于外层判断是否需要链式唤醒其他消费者（prevCount > 1 表示出队后仍有剩余元素）；
    //  - prevPut   用于外层判断是否需要唤醒生产者（prevPut == capacity，即本次释放前槽位预约恰为满，
    //              才可能有生产者阻塞在 add() 上）。生产者是按预约口径 putQsize>=capacity 阻塞的，
    //              故唤醒其的谓词必须同样基于 putQsize（而非已提交的 count），否则会出现
    //              “队列有空位但生产者永久挂起”的活性缺陷。
    private func tryRemoveCore(wake: WakeFlags): (Option<E>, Int64, Int64) {
        // 同 tryAdd：用 break 让 while(true) 具备可达出口，循环后统一 return (Option<E>, Int64, Int64)
        while (true) {
            let c: Int64 = count.load()
            if (c == 0) {
                break
            }
            let next: Int64 = c - 1
            if (count.compareAndSwap(c, next)) {
                // dequeueUnsafe 的 "Unreachable code." 异常路径会抛异常；
                // 一旦失败必须把已认领的 count 回补，否则元素丢失且 putQsize 泄漏。
                try {
                    let value = dequeueUnsafe()
                    let prevPut = putQsize.fetchSub(1)
                    return (Some(value), c, prevPut)
                } catch (e: Error) {
                    // 与 tryAddCore 对称：兜底捕获任何错误/异常，回补已认领的 count，避免元素丢失与 putQsize 泄漏。
                    // 回补恰使 count 从 0 升回 1（prevCount==0）时，等价于一次“从空释放元素”的事件，
                    // 必须补发 signalNotEmpty 唤醒其余阻塞消费者（防御性：当前唯一抛出源在认领协议成立时不可达，
                    // 但与 VUL-301 对称，统一守住异常路径不丢唤醒）。跨锁信号由外层解锁后补发。
                    let prevCount = count.fetchAdd(1)
                    if (prevCount == 0) {
                        wake.needWakeConsumer = true
                    }
                    throw e
                } catch (e: Exception) {
                    let prevCount = count.fetchAdd(1)
                    if (prevCount == 0) {
                        wake.needWakeConsumer = true
                    }
                    throw e
                }
            }
        }
        return (None<E>, 0, 0)
    }

    public func tryRemove(): Option<E> {
        // 与上游语义对齐：非阻塞出队成功后必须唤醒阻塞在 add() 上的生产者，
        // 但仅在“释放槽位前槽位预约恰为满（prevPut==capacity）”这一真正可能有生产者等待的情形下才发信号，
        // 避免 SPSC 下每次成功都抢占 putMonitor 把省下的锁往返又赔回去（即上游的
        // 条件 signal 语义）。signal 在 putMonitor 下进行，不会丢失唤醒。
        // OOM 异常路径：快路径不持任何 Monitor，回补跨越 0→1 时直接补发 signalNotEmpty。
        let wake = WakeFlags()
        try {
            let (opt, _, prevPut) = tryRemoveCore(wake)
            if (let Some(e) <- opt) {
                if (prevPut == capacity) {
                    signalNotFull()
                }
                return Some(e)
            }
            return None<E>
        } catch (e: Error) {
            if (wake.needWakeConsumer) {
                signalNotEmpty()
            }
            throw e
        } catch (e: Exception) {
            if (wake.needWakeConsumer) {
                signalNotEmpty()
            }
            throw e
        }
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
