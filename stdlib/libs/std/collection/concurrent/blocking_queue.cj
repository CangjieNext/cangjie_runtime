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

// Per-call holder passed into the lock-free cores. The cores must not signal while holding a
// monitor (ABBA), so they only set flags/prev-counters here; the outer function re-issues the
// matching signal after `finally { unlock }` (or directly on the lock-free fast path, where no
// monitor is held). prev/prevCount/prevPut carry the counters observed by the core out to the
// caller so the caller can decide wake-up without re-destructuring a tuple.
private class WakeFlags {
    var needWakeConsumer = false
    var needWakeProducer = false
    var prev: Int64 = 0       // count before this enqueue (add/tryAdd consumer-wake decision)
    var prevCount: Int64 = 0  // count before this dequeue (remove/tryRemove chained-notify decision)
    var prevPut: Int64 = 0    // putQsize before this slot release (producer-wake decision)
}

/**
 * BlockingQueue is a concurrent, capacity-bounded queue that blocks producers when
 * full and consumers when empty.
 *
 * Slot reservation protocol:
 *   - Producers reserve a slot with a CAS on `putQsize` (bounded by `capacity`), commit the
 *     element, then publish it with `count.fetchAdd`.
 *   - Consumers claim an element with a CAS on `count`, then dequeue it and free the slot
 *     with `putQsize.fetchSub`.
 *   Every mutation of `putQsize` / `count` (fast path or blocking path) uses this same CAS-based
 *   atomic protocol. The monitors only serialize blocking producers/consumers among themselves;
 *   they do NOT exclude a thread concurrently running the lock-free fast path, so a plain
 *   fetchAdd/fetchSub inside a monitor would race with the CAS loop.
 *
 * Wake-up protocol (deadlock- and lost-wakeup-free):
 *   - Cross-monitor signals (signalNotEmpty / signalNotFull) are issued AFTER releasing the
 *     monitor, to avoid an ABBA deadlock (add holds putMonitor and would take takeMonitor via
 *     signalNotEmpty, while remove holds takeMonitor and would take putMonitor via signalNotFull).
 *   - A producer blocks when `putQsize >= capacity`, so signalNotFull fires only when the slot
 *     release takes `putQsize` from `capacity` to `capacity-1` (prevPut == capacity) — derived from
 *     the same reservation counter, not the committed `count`. Deriving it from `count` would
 *     mismatch the blocking predicate and could leave a producer parked forever with a free slot.
 *   - A consumer blocks on `count == 0`, so signalNotEmpty is count-based (prev == 0).
 *   - Chained notify on the SAME monitor avoids MPMC starvation: a successful add with
 *     prev+1 < capacity wakes another blocked producer; a successful remove with prevCount > 1
 *     wakes another blocked consumer.
 *   - Exception path: an OOM rollback is itself a release event that can unblock a parked peer
 *     (putQsize.fetchSub crossing capacity->capacity-1, or count.fetchAdd crossing 0->1). The cores
 *     set the flag on the per-call WakeFlags holder; the outer function re-issues the matching signal
 *     after `finally { unlock }` (or directly on the fast path). This keeps the "core never signals
 *     while holding a monitor" invariant. See VUL-301.
 */
public class LinkedBlockingQueue<E> {
    public let capacity: Int64 // the capacity of the queue
    private let takeMonitor = Monitor() // Monitor for blocking consumers when the queue is empty
    private let putMonitor = Monitor() // Monitor for blocking producers when the queue is full
    // Slot reservation counter: 0 <= putQsize <= capacity. Bounds the number of in-flight
    // producers (reserved-but-not-committed) so the queue never exceeds capacity.
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
        // Fast path: tryAdd wakes a consumer on prev==0 and re-issues the producer wake-up on OOM.
        if (tryAdd(element)) {
            return
        }
        putMonitor.lock()
        let wake = WakeFlags()
        var needWakeConsumer = false
        try {
            // Wait loop reuses lock-free tryAddCore (CAS reservation): putMonitor does not exclude a
            // concurrent fast-path producer, so a lock-held fetchAdd would let putQsize exceed capacity.
            while (true) {
                if (tryAddCore(element, wake)) {
                    if (wake.prev == 0) {
                        needWakeConsumer = true
                    }
                    if (wake.prev + 1 < capacity) {
                        putMonitor.notify()
                    }
                    break
                }
                let _ = putMonitor.wait()
            }
        } catch (e: Error) {
            // OOM path: re-issue signalNotFull if rollback crossed capacity (finally released putMonitor). VUL-301.
            reissueProducerWake(wake)
            throw e
        } catch (e: Exception) {
            reissueProducerWake(wake)
            throw e
        } finally {
            putMonitor.unlock()
        }
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
        // Fast path: tryAdd handles consumer wake-up (prev==0) and its OOM-path producer wake-up.
        if (tryAdd(element)) {
            return true
        }
        putMonitor.lock()
        let wake = WakeFlags()
        var needWakeConsumer = false
        try {
            var remaining = timeout
            while (true) {
                if (tryAddCore(element, wake)) {
                    if (wake.prev == 0) {
                        needWakeConsumer = true
                    }
                    if (wake.prev + 1 < capacity) {
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
            reissueProducerWake(wake)
            throw e
        } catch (e: Exception) {
            reissueProducerWake(wake)
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
        // Fast path: tryRemove wakes a producer on prevPut==capacity and re-issues consumer wake-up on OOM.
        if (let Some(e) <- tryRemove()) {
            return e
        }
        takeMonitor.lock()
        let wake = WakeFlags()
        var needWakeProducer = false
        var result: Option<E> = None<E>
        try {
            // Wait loop reuses lock-free tryRemoveCore (CAS claim); a lock-held fetchSub would drive count negative.
            while (true) {
                if (let Some(e) <- tryRemoveCore(wake)) {
                    if (wake.prevPut == capacity) {
                        needWakeProducer = true
                    }
                    if (wake.prevCount > 1) {
                        takeMonitor.notify()
                    }
                    result = Some(e)
                    break
                }
                let _ = takeMonitor.wait()
            }
        } catch (e: Error) {
            // OOM path: re-issue signalNotEmpty if rollback crossed empty (finally released takeMonitor).
            reissueConsumerWake(wake)
            throw e
        } catch (e: Exception) {
            reissueConsumerWake(wake)
            throw e
        } finally {
            takeMonitor.unlock()
        }
        if (needWakeProducer) {
            signalNotFull()
        }
        return match(result) {
            case Some(v) => v
            case None => throw Exception("unreachable")
        }
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
        // Fast path: tryRemove handles producer wake-up (prevPut==capacity) and its OOM-path consumer wake-up.
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
                if (let Some(e) <- tryRemoveCore(wake)) {
                    if (wake.prevPut == capacity) {
                        needWakeProducer = true
                    }
                    if (wake.prevCount > 1) {
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
            reissueConsumerWake(wake)
            throw e
        } catch (e: Exception) {
            reissueConsumerWake(wake)
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

    // Lock-free core: enqueues and updates counters only; never acquires any monitor.
    // Signalling is split out because it needs the peer monitor, and this core runs inside add's
    // putMonitor wait loop — signalling while held would form an ABBA. Returns whether the enqueue
    // succeeded; the count before this enqueue is written into wake.prev so the caller can decide
    // whether to wake a consumer (prev==0) and whether to chained-notify another producer (prev+1 < capacity).
    private func tryAddCore(element: E, wake: WakeFlags): Bool {
        while (true) {
            let r: Int64 = putQsize.load()
            if (r >= capacity) {
                break
            }
            if (putQsize.compareAndSwap(r, r + 1)) {
                // enqueueUnsafe may throw on heap allocation (e.g. OOM in newBlock); on failure the
                // reserved putQsize must be rolled back, or the queue's real capacity is permanently -1.
                try {
                    enqueueUnsafe(element)
                    wake.prev = count.fetchAdd(1)
                    return true
                } catch (e: Error) {
                    // Covers OutOfMemoryError (<: Error, not Exception) from newBlock heap allocation:
                    // the slot was reserved (+1) but enqueue failed, so roll it back or capacity is poisoned.
                    // If the rollback took putQsize from capacity to capacity-1 (prevPut==capacity), it is a
                    // "release from full" event that must re-issue signalNotFull (VUL-301); otherwise a parked
                    // producer can hang forever on a drained/empty queue. The cross-lock signal is re-issued by
                    // the outer function after unlock; the core never signals while holding a monitor.
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
        return false
    }

    public func tryAdd(element: E): Bool {
        // Non-blocking enqueue: wake a consumer on prev==0 (conditional signal avoids a needless
        // monitor round-trip on the SPSC hot path); on the OOM path re-issue signalNotFull directly.
        let wake = WakeFlags()
        try {
            if (tryAddCore(element, wake)) {
                if (wake.prev == 0) {
                    signalNotEmpty()
                }
                return true
            }
            return false
        } catch (e: Error) {
            reissueProducerWake(wake)
            throw e
        } catch (e: Exception) {
            reissueProducerWake(wake)
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

    // Lock-free core: dequeues and updates counters only; never acquires any monitor (same reason
    // as tryAddCore). Returns the dequeued element (Some) or None when empty; the count before this
    // dequeue is written into wake.prevCount (chained-notify another consumer when > 1) and the
    // putQsize before the slot release into wake.prevPut (wake a producer when == capacity). A producer
    // blocks on the reservation predicate putQsize>=capacity, so its wake-up MUST be derived from putQsize,
    // not the committed count — otherwise a free slot could leave a producer parked forever (lost-wakeup bug).
    private func tryRemoveCore(wake: WakeFlags): Option<E> {
        while (true) {
            let c: Int64 = count.load()
            if (c == 0) {
                break
            }
            if (count.compareAndSwap(c, c - 1)) {
                // dequeueUnsafe can throw ("Unreachable code."); on failure the claimed count must be
                // rolled back, or the element is lost and putQsize leaks.
                try {
                    let value = dequeueUnsafe()
                    wake.prevPut = putQsize.fetchSub(1)
                    wake.prevCount = c
                    return Some(value)
                } catch (e: Error) {
                    // Symmetric to tryAddCore: roll back the claimed count on any error/exception.
                    // If the rollback took count from 0 to 1 (prevCount==0), re-issue signalNotEmpty to
                    // wake other blocked consumers (defensive: the only throwing source is unreachable under
                    // the claim protocol, but kept symmetric with VUL-301 so the exception path never loses a
                    // wake-up). The cross-lock signal is re-issued by the outer function after unlock.
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
        return None<E>
    }

    public func tryRemove(): Option<E> {
        // Non-blocking dequeue: wake a producer on prevPut==capacity; on the OOM path re-issue
        // signalNotEmpty directly if the rollback crossed the empty boundary.
        let wake = WakeFlags()
        try {
            if (let Some(e) <- tryRemoveCore(wake)) {
                if (wake.prevPut == capacity) {
                    signalNotFull()
                }
                return Some(e)
            }
            return None<E>
        } catch (e: Error) {
            reissueConsumerWake(wake)
            throw e
        } catch (e: Exception) {
            reissueConsumerWake(wake)
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

    private func reissueProducerWake(wake: WakeFlags): Unit {
        if (wake.needWakeProducer) {
            signalNotFull()
        }
    }

    private func reissueConsumerWake(wake: WakeFlags): Unit {
        if (wake.needWakeConsumer) {
            signalNotEmpty()
        }
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
