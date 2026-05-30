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
 */
public class LinkedBlockingQueue<E> {
    public let capacity: Int64 // the capacity of the queue
    private let sendMonitor = Monitor() // Monitor for blocking the fail receivers
    private let receiveMonitor = Monitor() // Monitor for blocking the fail senders
    private let sendQSize = AtomicInt64(0) // Number of elements that are enqueuing.
    private let receiveQSize = AtomicInt64(0) // Number of elements that are being dequeued.
    private let front: AtomicReference<Block<E>> // head of BlockingQueue.
    private let rear: AtomicReference<Block<E>> // tail of BlockingQueue.

    public init(capacity: Int64) {
        if (capacity <= 0) {
            throw IllegalArgumentException("Invalid size of BlockingQueue: ${capacity}.")
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
        this(capacity)
        if (capacity < elements.size) {
            throw IllegalArgumentException("Size of BlockingQueue should be equal or greater than ${elements.size}.")
        }

        for (e in elements) {
            tryEnqueue(e)
        }
    }

    @Deprecated[message: "Use the other version of `init` instead."]
    public init(capacity: Int64, elements: Collection<E>) {
        this(capacity)
        if (capacity < elements.size) {
            throw IllegalArgumentException("Size of BlockingQueue should be equal or greater than ${elements.size}.")
        }

        for (e in elements) {
            tryEnqueue(e)
        }
    }

    /**
     * Returns a sequential snapshot of the number of elements in the linked queue.
     *
     * The value is not atomic with concurrent producers or consumers, so it may be stale as soon as it is observed.
     */
    public prop size: Int64 {
        get() {
            let headBlock = front.load()
            let tailBlock = rear.load()
            let (headBlockVersion, headBlockReserved) = headBlock.getReserved()
            let (tailBlockVersion, tailBlockAllocated) = tailBlock.getAllocated()
            return (tailBlockAllocated - headBlockReserved) + (Int64(tailBlockVersion - headBlockVersion) <<
                BUF_SIZE_BITS)
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
        while (true) {
            if (tryAdd(element)) {
                return
            }

            waitForWritableElement()
        }
    }

    private func waitForWritableElement(): Unit {
        receiveMonitor.lock()
        sendQSize.fetchAdd(1)
        if (isFull()) {
            receiveMonitor.wait()
        }
        sendQSize.fetchSub(1)
        receiveMonitor.unlock()
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
        add(element, timeout)
    }

    public func add(element: E, timeout: Duration): Bool {
        var total = timeout
        while (true) {
            if (tryAdd(element)) {
                return true
            }
            if (total <= Duration.Zero) {
                break
            }
            total = waitForWritableElement(total)
        }
        return false
    }

    private func waitForWritableElement(timeout: Duration): Duration {
        receiveMonitor.lock()
        let startTime = MonoTime.now()
        sendQSize.fetchAdd(1)
        receiveMonitor.wait(timeout: timeout)
        sendQSize.fetchSub(1)
        let endTime = MonoTime.now()
        receiveMonitor.unlock()
        timeout - (endTime - startTime)
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
        while (true) {
            if (let Some(e) <- tryRemove()) {
                return e
            }

            waitForReadableElement()
        }
        throw Exception("Unreachable code.")
    }

    private func waitForReadableElement(): Unit {
        sendMonitor.lock()
        receiveQSize.fetchAdd(1)
        if (size == 0) {
            sendMonitor.wait()
        }
        receiveQSize.fetchSub(1)
        sendMonitor.unlock()
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
        remove(timeout)
    }

    public func remove(timeout: Duration): Option<E> {
        var total = timeout
        while (true) {
            if (let Some(e) <- tryRemove()) {
                return Some(e)
            }
            if (total <= Duration.Zero) {
                break
            }
            total = waitForReadableElement(total)
        }
        return None<E>
    }

    private func waitForReadableElement(timeout: Duration): Duration {
        sendMonitor.lock()
        let startTime = MonoTime.now()
        receiveQSize.fetchAdd(1)
        sendMonitor.wait(timeout: timeout)
        receiveQSize.fetchSub(1)
        let endTime = MonoTime.now()
        sendMonitor.unlock()
        timeout - (endTime - startTime)
    }

    /**
     * Returns the current head element without removing it.
     *
     * The operation is non-blocking and observes the linked queue at the time of the call.
     *
     * @return Some(element) when a head element is available, otherwise None.
     */
    @Deprecated[message: "Use member function `public func peek(): Option<E>` instead."]
    public func head(): Option<E> {
        peek()
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
        tryAdd(element)
    }

    public func tryAdd(element: E): Bool {
        while (true) {
            let tailBlock = rear.load()
            let headBlock = front.load()
            let (tailBlockVersion, tailBlockAllocated) = tailBlock.getAllocated()
            let (headBlockVersion, headBlockReserved) = headBlock.getReserved()

            if (tailBlockVersion < headBlockVersion) {
                continue
            }

            // Justifying whether the queue is full.
            if (isFull(tailBlockVersion, tailBlockAllocated, headBlockVersion, headBlockReserved)) {
                return false
            }

            // Try to push @p element into the block at index 'allocatedIndex'.
            match (tailBlock.pushAt(element, tailBlockAllocated)) {
                case SUCCEED => break
                case BlockDone => newBlock<E>(tailBlock, rear)
                case CONFLICT => ()
            }
        }

        // await the receivers that wait for successful sender.
        if (receiveQSize.load() > 0) {
            sendMonitor.lock()
            sendMonitor.notify()
            sendMonitor.unlock()
        }
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
        tryRemove()
    }

    public func tryRemove(): Option<E> {
        while (true) {
            let headBlock = front.load()
            match (headBlock.pop()) {
                case BlockDone =>
                    if (!nextBlock<E>(headBlock, front, rear)) {
                        return None<E>
                    }
                case NoEntry => break
                case ELEM(e) =>
                    if (sendQSize.load() > 0) {
                        receiveMonitor.lock()
                        receiveMonitor.notify()
                        receiveMonitor.unlock()
                    }
                    return Some(e)
            }
        }
        return None<E>
    }

    private func isFull(
        tailVersion: UInt64,
        tailAllocated: Int64,
        headVersion: UInt64,
        headReserved: Int64
    ): Bool {
        return (tailAllocated - headReserved) + Int64((tailVersion - headVersion) << BUF_SIZE_BITS) >= capacity
    }

    private func isFull(): Bool {
        let tailBlock = rear.load()
        let headBlock = front.load()
        let (tailBlockVersion, tailBlockAllocated) = tailBlock.getAllocated()
        let (headBlockVersion, headBlockReserved) = headBlock.getReserved()
        return isFull(tailBlockVersion, tailBlockAllocated, headBlockVersion, headBlockReserved)
    }
}
