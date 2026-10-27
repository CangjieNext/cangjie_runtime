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

// Per-call holder for cross-monitor wake-up flags raised on the OOM exception path.
// The cores must not signal while holding a monitor (ABBA), so they only set a flag here
// and the outer function re-issues the matching signal after `finally { unlock }`
// (or directly on the lock-free fast path, where no monitor is held).
private class WakeFlags {
    var needWakeConsumer = false
    var needWakeProducer = false
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
        // Fast path: tryAdd already wakes a blocked consumer when prev==0 and re-issues the
        // producer wake-up on its OOM path, so no signalling is needed here.
        if (tryAdd(element)) {
            return
        }
        putMonitor.lock()
        let wake = WakeFlags()
        var needWakeConsumer = false
        try {
            // The wait loop must reuse the lock-free core tryAddCore (CAS-based reservation):
            // putMonitor only serializes blocking producers and does NOT exclude a concurrent
            // fast-path producer, so a lock-held fetchAdd would let putQsize exceed capacity (a
            // drained queue would still look "full"). We also cannot call the public tryAdd here
            // (it signals on success), which would duplicate the signal; tryAddCore signals nothing,
            // so we signal once after unlock below.
            while (true) {
                let (ok, prev) = tryAddCore(element, wake)
                if (ok) {
                    // Wake a consumer only when the queue was empty before this enqueue (prev==0),
                    // avoiding a needless signal on the SPSC hot path.
                    if (prev == 0) {
                        needWakeConsumer = true
                    }
                    // Chained notify (same monitor, no cross-lock): if the queue is still not full
                    // after this enqueue, wake another blocked producer to avoid MPMC starvation.
                    if (prev + 1 < capacity) {
                        putMonitor.notify()
                    }
                    break
                }
                let _ = putMonitor.wait()
            }
        } catch (e: Error) {
            // OOM (or other) exception propagated from tryAddCore through the wait loop: re-issue
            // signalNotFull if the rollback crossed the capacity boundary (prevPut==capacity), otherwise
            // a parked producer can be left hanging on an empty queue (VUL-301). finally has released
            // putMonitor by now, so the cross-lock signal cannot form an ABBA.
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
        // Cross-monitor signal (takes takeMonitor) must come AFTER releasing putMonitor, otherwise it
        // forms an ABBA with the remove side (holds takeMonitor, takes putMonitor). It lands on the
        // monitor consumers wait on, so no wake-up is lost.
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
        // Fast path: tryAdd already handles consumer wake-up (prev==0) and its OOM-path producer
        // wake-up, so nothing to signal here.
        if (tryAdd(element)) {
            return true
        }
        putMonitor.lock()
        let wake = WakeFlags()
        var needWakeConsumer = false
        try {
            var remaining = timeout
            while (true) {
                // Same as add(element): the wait loop uses only the lock-free core tryAddCore so
                // putQsize stays CAS-reserved.
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
            // OOM exception path: re-issue signalNotFull if the rollback crossed the capacity boundary
            // (finally has released putMonitor, so no ABBA).
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
        // Fast path: tryRemove already wakes a blocked producer when prevPut==capacity and re-issues
        // the consumer wake-up on its OOM path, so nothing to signal here.
        if (let Some(e) <- tryRemove()) {
            return e
        }
        takeMonitor.lock()
        let wake = WakeFlags()
        var needWakeProducer = false
        var result: Option<E> = None<E>
        try {
            while (true) {
                // The wait loop must reuse the lock-free core tryRemoveCore (CAS-based claim), not a
                // lock-held fetchSub: takeMonitor only serializes blocking consumers and does NOT exclude a
                // concurrent fast-path consumer, so a plain fetchSub would drive count negative (empty-queue
                // pop throwing "Unreachable code"). We also cannot call public tryRemove (it signals on
                // success); tryRemoveCore signals nothing, so we signal once after unlock below.
                let (opt, prevCount, prevPut) = tryRemoveCore(wake)
                if (let Some(e) <- opt) {
                    // Wake a producer only when the slot release took the reservation from full to
                    // non-full (prevPut==capacity) — matching the producer's reservation-based blocking
                    // predicate, so a free slot never leaves a producer parked.
                    if (prevPut == capacity) {
                        needWakeProducer = true
                    }
                    // Chained notify (same monitor): if elements remain after this dequeue
                    // (prevCount > 1), wake another blocked consumer to avoid MPMC starvation.
                    if (prevCount > 1) {
                        takeMonitor.notify()
                    }
                    result = Some(e)
                    break
                }
                let _ = takeMonitor.wait()
            }
        } catch (e: Error) {
            // OOM (or other) exception path: re-issue signalNotEmpty if the rollback crossed the
            // empty boundary (prevCount==0); finally has released takeMonitor, so no ABBA.
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
        // Cross-monitor signal (takes putMonitor) must come AFTER releasing takeMonitor, otherwise ABBA.
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
        // Fast path: tryRemove already handles producer wake-up (prevPut==capacity) and its OOM-path
        // consumer wake-up, so nothing to signal here.
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
                // Same as remove(): the wait loop uses only the lock-free core tryRemoveCore so count
                // stays CAS-claimed.
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
            // OOM exception path: re-issue signalNotEmpty if the rollback crossed the empty boundary
            // (prevCount==0); finally has released takeMonitor, so no ABBA.
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

    // Lock-free core: enqueues and updates counters only; never acquires any monitor.
    // Signalling is split out because it needs the peer monitor, and this core runs inside add's
    // putMonitor wait loop — signalling while held would form an ABBA. Returns (success, prev):
    // prev (count before this enqueue) tells the caller whether to wake a consumer (prev==0) and
    // whether to chained-notify another producer (prev+1 < capacity).
    private func tryAddCore(element: E, wake: WakeFlags): (Bool, Int64) {
        // Use break (not return) to leave the "full" branch: a while(true) used as the function's
        // tail expression is typed Unit, so the loop needs a reachable exit and a single return after
        // it to satisfy the (Bool, Int64) return type.
        while (true) {
            let r: Int64 = putQsize.load()
            if (r >= capacity) {
                break
            }
            let next: Int64 = r + 1
            if (putQsize.compareAndSwap(r, next)) {
                // enqueueUnsafe may throw on heap allocation (e.g. OOM in newBlock); on failure the
                // reserved putQsize must be rolled back, or the queue's real capacity is permanently -1.
                try {
                    enqueueUnsafe(element)
                    let prev = count.fetchAdd(1)
                    return (true, prev)
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
        return (false, 0)
    }

    public func tryAdd(element: E): Bool {
        // Non-blocking enqueue: on success, wake a blocked consumer only when the queue was empty
        // before (prev==0), matching the conditional signal semantics and avoiding a needless
        // monitor round-trip on the SPSC hot path. On the OOM path (no monitor held) re-issue
        // signalNotFull directly if the rollback crossed the capacity boundary.
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

    // Lock-free core: dequeues and updates counters only; never acquires any monitor (same reason
    // as tryAddCore). Returns (result, prevCount, prevPut):
    //   - prevCount (count before this dequeue) tells the caller whether to chained-notify another
    //     consumer (prevCount > 1).
    //   - prevPut (putQsize before the slot release) tells the caller whether to wake a producer
    //     (prevPut == capacity). A producer blocks on the reservation predicate putQsize>=capacity,
    //     so its wake-up MUST be derived from putQsize, not the committed count — otherwise a free slot
    //     could leave a producer parked forever (lost-wakeup liveness bug).
    private func tryRemoveCore(wake: WakeFlags): (Option<E>, Int64, Int64) {
        // Same as tryAddCore: use break so the while(true) has a reachable exit and a single return after it.
        while (true) {
            let c: Int64 = count.load()
            if (c == 0) {
                break
            }
            let next: Int64 = c - 1
            if (count.compareAndSwap(c, next)) {
                // dequeueUnsafe can throw ("Unreachable code."); on failure the claimed count must be
                // rolled back, or the element is lost and putQsize leaks.
                try {
                    let value = dequeueUnsafe()
                    let prevPut = putQsize.fetchSub(1)
                    return (Some(value), c, prevPut)
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
        return (None<E>, 0, 0)
    }

    public func tryRemove(): Option<E> {
        // Non-blocking dequeue: on success, wake a blocked producer only when the slot release took the
        // reservation from full to non-full (prevPut==capacity), matching the conditional signal semantics.
        // On the OOM path (no monitor held) re-issue signalNotEmpty directly if the rollback crossed the
        // empty boundary.
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
