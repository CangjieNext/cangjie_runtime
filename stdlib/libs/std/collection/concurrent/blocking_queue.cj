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
    private let putMonitor = Monitor()
    private let takeMonitor = Monitor()
    private let count = AtomicInt64(0)
    private let putWaiters = AtomicInt64(0)
    private let takeWaiters = AtomicInt64(0)
    private let front: AtomicReference<Block<E>> // head block
    private let rear: AtomicReference<Block<E>> // tail block

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
        if (capacity <= 0) {
            throw IllegalArgumentException("Invalid size of BlockingQueue: ${capacity}.")
        }

        if (capacity < elements.size) {
            throw IllegalArgumentException("Size of BlockingQueue should be equal or greater than ${elements.size}.")
        }

        this.capacity = capacity
        let block = Block<E>(0)
        front = AtomicReference<Block<E>>(block)
        rear = AtomicReference<Block<E>>(block)

        for (e in elements) {
            let _ = tryEnqueue(e)
        }
    }

    @Deprecated[message: "Use the other version of `init` instead."]
    public init(capacity: Int64, elements: Collection<E>) {
        if (capacity <= 0) {
            throw IllegalArgumentException("Invalid size of BlockingQueue: ${capacity}.")
        }

        if (capacity < elements.size) {
            throw IllegalArgumentException("Size of BlockingQueue should be equal or greater than ${elements.size}.")
        }

        this.capacity = capacity
        let block = Block<E>(0)
        front = AtomicReference<Block<E>>(block)
        rear = AtomicReference<Block<E>>(block)

        for (e in elements) {
            let _ = tryEnqueue(e)
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
        putMonitor.lock()
        var c: Int64 = -1
        try {
            while (true) {
                if (count.load() != capacity) {
                    break
                }
                putWaiters.fetchAdd(1)
                try {
                    if (count.load() == capacity) {
                        putMonitor.wait()
                    }
                } finally {
                    putWaiters.fetchSub(1)
                }
            }
            enqueueUnsafe(element)
            c = count.fetchAdd(1)
            if (c + 1 < capacity) {
                putMonitor.notify()
            }
        } finally {
            putMonitor.unlock()
        }
        if (c == 0 && takeWaiters.load() > 0) {
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
        var total = timeout
        putMonitor.lock()
        var c: Int64 = -1
        try {
            while (true) {
                if (count.load() != capacity) {
                    break
                }
                if (total <= Duration.Zero) {
                    return false
                }
                putWaiters.fetchAdd(1)
                try {
                    if (count.load() == capacity) {
                        let startTime = MonoTime.now()
                        let _ = putMonitor.wait(timeout: total)
                        let endTime = MonoTime.now()
                        total -= endTime - startTime
                    }
                } finally {
                    putWaiters.fetchSub(1)
                }
            }
            enqueueUnsafe(element)
            c = count.fetchAdd(1)
            if (c + 1 < capacity) {
                putMonitor.notify()
            }
        } finally {
            putMonitor.unlock()
        }
        if (c == 0 && takeWaiters.load() > 0) {
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
        takeMonitor.lock()
        var c: Int64 = -1
        try {
            while (true) {
                if (count.load() != 0) {
                    break
                }
                takeWaiters.fetchAdd(1)
                try {
                    if (count.load() == 0) {
                        takeMonitor.wait()
                    }
                } finally {
                    takeWaiters.fetchSub(1)
                }
            }
            let value = dequeueUnsafe()
            c = count.fetchSub(1)
            if (c > 1) {
                takeMonitor.notify()
            }
            return value
        } finally {
            takeMonitor.unlock()
            if (c == capacity && putWaiters.load() > 0) {
                signalNotFull()
            }
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
        var total = timeout
        takeMonitor.lock()
        var c: Int64 = -1
        try {
            while (true) {
                if (count.load() != 0) {
                    break
                }
                if (total <= Duration.Zero) {
                    return None<E>
                }
                takeWaiters.fetchAdd(1)
                try {
                    if (count.load() == 0) {
                        let startTime = MonoTime.now()
                        let _ = takeMonitor.wait(timeout: total)
                        let endTime = MonoTime.now()
                        total -= endTime - startTime
                    }
                } finally {
                    takeWaiters.fetchSub(1)
                }
            }
            let value = dequeueUnsafe()
            c = count.fetchSub(1)
            if (c > 1) {
                takeMonitor.notify()
            }
            return Some(value)
        } finally {
            takeMonitor.unlock()
            if (c == capacity && putWaiters.load() > 0) {
                signalNotFull()
            }
        }
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
        if (count.load() == 0) {
            return None<E>
        }
        var result: Option<E> = None<E>
        takeMonitor.lock()
        try {
            if (count.load() == 0) {
                result = None<E>
            } else {
                while (true) {
                    let headBlock = front.load()
                    match (headBlock.front()) {
                        case ELEM(e) =>
                            result = Some(e)
                            break
                        case NoEntry =>
                            result = None<E>
                            break
                        case BlockDone =>
                            if (!nextBlock<E>(headBlock, front, rear)) {
                                result = None<E>
                                break
                            }
                    }
                }
            }
        } finally {
            takeMonitor.unlock()
        }
        return result
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

    public func tryAdd(element: E): Bool {
        if (count.load() == capacity) {
            return false
        }
        putMonitor.lock()
        var c: Int64 = -1
        try {
            if (count.load() == capacity) {
                return false
            }
            enqueueUnsafe(element)
            c = count.fetchAdd(1)
            if (c + 1 < capacity) {
                putMonitor.notify()
            }
            return true
        } finally {
            putMonitor.unlock()
            if (c == 0 && takeWaiters.load() > 0) {
                signalNotEmpty()
            }
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

    public func tryRemove(): Option<E> {
        if (count.load() == 0) {
            return None<E>
        }
        takeMonitor.lock()
        var c: Int64 = -1
        try {
            if (count.load() == 0) {
                return None<E>
            }
            let value = dequeueUnsafe()
            c = count.fetchSub(1)
            if (c > 1) {
                takeMonitor.notify()
            }
            return Some(value)
        } finally {
            takeMonitor.unlock()
            if (c == capacity && putWaiters.load() > 0) {
                signalNotFull()
            }
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
