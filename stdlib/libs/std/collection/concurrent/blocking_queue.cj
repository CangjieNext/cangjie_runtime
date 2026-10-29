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
 *   fetchAdd/fetchSub inside a monitor would race with the CAS loop and corrupt the counters.
 *
 * Wake-up protocol (deadlock- and lost-wakeup-free):
 *   - Cross-monitor signals (signalNotEmpty / signalNotFull) are issued AFTER releasing the
 *     monitor, to avoid an ABBA deadlock (add holds putMonitor and would take takeMonitor via
 *     signalNotEmpty, while remove holds takeMonitor and would take putMonitor via signalNotFull).
 *   - Chained notify on the SAME monitor avoids MPMC starvation: a successful add with
 *     prev+1 < capacity wakes another blocked producer; a successful remove with prevCount > 1
 *     wakes another blocked consumer. This uses only the monitor the caller already holds,
 *     so no cross-lock acquisition ever occurs and there is zero hot-path overhead.
 *   - Exception path: tryAddCore's OOM rollback is itself a slot-release event that can unblock
 *     a parked producer. Because Monitor is reentrant (same thread can re-lock the monitor it
 *     already holds), the core can safely call signalNotFull from its OOM catch — whether the
 *     caller currently holds putMonitor (add/add(timeout)) or not (tryAdd). After the rollback
 *     we propagate the error. This keeps every counter mutation + signal decision inside a single
 *     stack frame, and the core returns a small value-type (AddResult / RemoveResult<E>) carrying
 *     the counts the caller needs for the post-finally cross-monitor signal, so no heap-allocated
 *     holder is required on the hot path.
 */

// Value-type result carriers for the lock-free cores. They hold the counters the caller needs
// to decide the post-finally cross-monitor signal. Being plain structs (not classes) they are
// stack-allocated, so the hot path pays zero heap allocation - no per-call holder object.
private struct AddResult {
    var ok: Bool
    var prev: Int64
    init(ok: Bool, prev: Int64) {
        this.ok = ok
        this.prev = prev
    }
}

private struct RemoveResult<E> {
    var opt: Option<E>
    var prevCount: Int64
    var prevPut: Int64
    init(opt: Option<E>, prevCount: Int64, prevPut: Int64) {
        this.opt = opt
        this.prevCount = prevCount
        this.prevPut = prevPut
    }
}

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
        // Fast path succeeds on every non-full enqueue and handles conditional consumer wake-up there.
        if (tryAdd(element)) {
            return
        }
        putMonitor.lock()
        var needWakeConsumer = false
        try {
            // Wait loop reuses the lock-free CAS reservation core. A lock-held fetchAdd would race
            // with a concurrent fast-path producer and could drive putQsize past capacity.
            while (true) {
                let r = tryAddCore(element)
                if (r.ok) {
                    let prev = r.prev
                    // Edge signal: prev==0 means this enqueue transitioned the queue from empty,
                    // which is the only state where a consumer could have been parked in remove().
                    if (prev == 0) {
                        needWakeConsumer = true
                    }
                    // Chained notify on putMonitor (upstream semantics): prev+1<capacity means
                    // the queue is still not full after this enqueue, so wake another blocked
                    // producer. We already hold putMonitor — no cross-lock, zero overhead.
                    if (prev + 1 < capacity) {
                        putMonitor.notify()
                    }
                    break
                }
                let _ = putMonitor.wait()
            }
        } finally {
            putMonitor.unlock()
        }
        // signalNotEmpty takes takeMonitor — emitting it while holding putMonitor would form
        // an ABBA window with remove's signalNotFull taking putMonitor while holding takeMonitor.
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
     *
     * @throws IllegalArgumentException if timeout <= Duration.Zero (thrown by Monitor.wait(timeout))
     */
    @Deprecated[message: "Use member function `public func add(element: E, timeout: Duration): Bool` instead."]
    public func enqueue(element: E, timeout: Duration): Bool {
        return add(element, timeout)
    }

    public func add(element: E, timeout: Duration): Bool {
        if (tryAdd(element)) {
            return true
        }
        putMonitor.lock()
        var needWakeConsumer = false
        try {
            var remaining = timeout
            while (true) {
                let r = tryAddCore(element)
                if (r.ok) {
                    let prev = r.prev
                    if (prev == 0) {
                        needWakeConsumer = true
                    }
                    if (prev + 1 < capacity) {
                        putMonitor.notify()
                    }
                    break
                }
                if (remaining <= Duration.Zero) {
                    // Timeout: finally releases putMonitor, then we return false and skip the
                    // post-finally signal (nothing was enqueued, no consumer could be woken).
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
        if (let Some(e) <- tryRemove()) {
            return e
        }
        takeMonitor.lock()
        var needWakeProducer = false
        var result: Option<E> = None<E>
        try {
            while (true) {
                let r = tryRemoveCore()
                if (let Some(e) <- r.opt) {
                    let prevPut = r.prevPut
                    let prevCount = r.prevCount
                    // Edge signal for producers is based on the RESERVATION counter, not count.
                    // A producer blocks on putQsize >= capacity, so we need prevPut==capacity
                    // (the slot we just freed was the one that made the queue appear full to
                    // a producer) — not prevCount==capacity which would miss the in-flight
                    // reservation window that distinguishes this implementation from upstream.
                    if (prevPut == capacity) {
                        needWakeProducer = true
                    }
                    // Chained notify on takeMonitor (upstream semantics): prevCount>1 means
                    // the queue still has elements after this dequeue, so wake another blocked
                    // consumer. We already hold takeMonitor — no cross-lock, zero overhead.
                    if (prevCount > 1) {
                        takeMonitor.notify()
                    }
                    result = Some(e)
                    break
                }
                let _ = takeMonitor.wait()
            }
        } finally {
            takeMonitor.unlock()
        }
        // signalNotFull takes putMonitor — must not hold takeMonitor when emitting.
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
     *
     * @throws IllegalArgumentException if timeout <= Duration.Zero (thrown by Monitor.wait(timeout))
     */
    @Deprecated[message: "Use member function `public func remove(timeout: Duration): Option<E>` instead."]
    public func dequeue(timeout: Duration): Option<E> {
        return remove(timeout)
    }

    public func remove(timeout: Duration): Option<E> {
        if (let Some(e) <- tryRemove()) {
            return Some(e)
        }
        takeMonitor.lock()
        var needWakeProducer = false
        var result: Option<E> = None<E>
        try {
            var remaining = timeout
            while (true) {
                let r = tryRemoveCore()
                if (let Some(e) <- r.opt) {
                    let prevPut = r.prevPut
                    let prevCount = r.prevCount
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
                    // Timeout: finally releases takeMonitor, then we return None and skip signal.
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

    // Lock-free enqueue core: reserves a slot via putQsize CAS, then commits the element.
    // Never acquires any monitor (runs inside add's putMonitor wait loop, where taking any
    // monitor would form an ABBA window). Returns (ok, prev) — prev is count before this
    // enqueue, so the caller can decide whether to signal a consumer (prev==0 is the only
    // state where a consumer could be parked in remove()) and whether to chained-notify
    // another blocked producer (prev+1 < capacity, queue still has room).
    //
    // Exception path: enqueueUnsafe may throw on heap allocation (OOM in newBlock). The core
    // rolls back the reserved putQsize; if the rollback crossed capacity->capacity-1 it calls
    // signalNotFull directly — safe whether the caller holds putMonitor (add path, reentrant)
    // or not (tryAdd fast path, normal cross-thread acquire). Then the error propagates.
    // The result is returned as a value-type AddResult (stack-allocated, zero heap allocation
    // on the hot path); the caller reads r.prev only when r.ok to decide the cross-monitor signal.
    private func tryAddCore(element: E): AddResult {
        while (true) {
            let r: Int64 = putQsize.load()
            if (r >= capacity) {
                break
            }
            if (putQsize.compareAndSwap(r, r + 1)) {
                try {
                    enqueueUnsafe(element)
                    return AddResult(true, count.fetchAdd(1))
                } catch (e: Error) {
                    // Covers OutOfMemoryError (<: Error, not Exception) from newBlock heap allocation:
                    // the slot was reserved (+1) but enqueue failed, so roll it back or capacity is poisoned.
                    // Rollback released the last slot (capacity->capacity-1)? If a producer was parked on
                    // putQsize>=capacity this is its release event — reissue signalNotFull here.
                    // Monitor is reentrant, safe whether caller holds putMonitor or not.
                    if (putQsize.fetchSub(1) == capacity) {
                        signalNotFull()
                    }
                    throw e
                }
            }
        }
        return AddResult(false, 0)
    }

    public func tryAdd(element: E): Bool {
        // Lock-free fast path: conditional consumer wake-up (prev==0 only, avoids a needless
        // takeMonitor round-trip on SPSC) and OOM-path producer re-issue are both handled inside
        // tryAddCore — no holder object, zero heap allocation.
        let r = tryAddCore(element)
        if (r.ok) {
            if (r.prev == 0) {
                signalNotEmpty()
            }
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

    // Lock-free dequeue core: claims an element via count CAS, then dequeues it and frees the
    // slot via putQsize.fetchSub. Never acquires any monitor (runs inside remove's takeMonitor
    // wait loop, where taking any monitor would form an ABBA window). Returns (opt, prevCount, prevPut):
    //   - opt    : Some(actual) or None when queue is empty
    //   - prevCount: count before this dequeue (caller uses for chained-notify: prevCount>1 means
    //     queue still has elements after this dequeue, wake another consumer)
    //   - prevPut  : putQsize before the slot release (caller uses for the producer wake-up edge:
    //     prevPut==capacity means the slot we just freed was the one that made the queue appear
    //     full to a producer — the reservation predicate putQsize>=capacity is what producers
    //     block on, so we MUST derive the producer wake-up from putQsize, not from count).
    //
    // Exception path: dequeueUnsafe throws only its "Unreachable code." guard, so we keep the
    // Exception catch (defensive rollback of the claimed count). No reentrant-signal needed here
    // because this throw is under the claim protocol and does not represent a cross-boundary event.
    // The result is returned as a value-type RemoveResult<E> (stack-allocated, zero heap allocation).
    private func tryRemoveCore(): RemoveResult<E> {
        while (true) {
            let c: Int64 = count.load()
            if (c == 0) {
                break
            }
            if (count.compareAndSwap(c, c - 1)) {
                try {
                    let value = dequeueUnsafe()
                    return RemoveResult<E>(Some(value), c, putQsize.fetchSub(1))
                } catch (e: Exception) {
                    // Defensive: dequeueUnsafe's "Unreachable code." guard. Roll back the claimed
                    // count and let the error propagate; a new iteration will re-CAS if needed.
                    count.fetchAdd(1)
                    throw e
                }
            }
        }
        return RemoveResult<E>(None<E>, 0, 0)
    }

    public func tryRemove(): Option<E> {
        // Lock-free fast path: conditional producer wake-up (prevPut==capacity, reservation-edge
        // not count-edge) and any exception-path consumer re-issue — both handled inside tryRemoveCore.
        let r = tryRemoveCore()
        if (let Some(e) <- r.opt) {
            if (r.prevPut == capacity) {
                signalNotFull()
            }
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
