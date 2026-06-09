/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
 * This source file is part of the Cangjie project, licensed under Apache-2.0
 * with Runtime Library Exception.
 *
 * See https://cangjie-lang.cn/pages/LICENSE for license information.
 */

package std.concurrent

import std.collection.ArrayList
import std.collection.concurrent.LinkedBlockingQueue
import std.sync.{AtomicUInt64, Mutex}

class ThreadGroupIterator<T> <: Iterator<T> {
    public ThreadGroupIterator(let group: ThreadGroup<T>) {}

    public override func next(): Option<T> {
        return group.nextResult()
    }
}

enum Result<T> {
    | Ok(T)
    | Ex(Exception)
    | Err(Error)
}

public class ThreadGroup<T> <: Iterable<T> {
    private let futures = ArrayList<Future<Unit>>()
    private let completed = LinkedBlockingQueue<Result<T>>()
    private var unconsumed: AtomicUInt64 = AtomicUInt64(0)
    private let mtx = Mutex()
    private var cancelled = false
    
    init() {}

    public func launch(task: () -> T): Unit {
        synchronized(mtx) {
            unconsumed.fetchAdd(1)
            let future = spawn {
                try {
                    completed.add(Ok(task()))
                } catch (e: Exception) {
                    completed.add(Ex(e))
                } catch (e: Error) {
                    completed.add(Err(e))
                }
            }
            futures.add(future)
            if (cancelled) {
                future.cancel()
            }
        }
    }

    public func cancelAll() {
        synchronized(mtx) {
            cancelled = true
            for (future in futures) {
                future.cancel()
            }
        }
    }

    public func iterator(): Iterator<T> {
        ThreadGroupIterator<T>(this)
    }

    func waitForAll() {
        for (_ in this) {}
    }

    func silentWaitForAll() {
        while (unconsumed.load() != 0) {
            completed.remove()
            unconsumed.fetchSub(1)
        }
    }

    func nextResult(): Option<T> {
        if (unconsumed.load() == 0) {
            return None
        }

        let result = completed.remove()
        unconsumed.fetchSub(1)

        match (result) {
            case Ok(v) => return v
            case Ex(e) => throw e
            case Err(e) => throw e
        }
    }
}

public func ThreadScope<T, R>(fn: (ThreadGroup<T>) -> R): R {
    let group = ThreadGroup<T>()
    try {
        let result = fn(group)
        group.waitForAll()
        return result
    } catch (e: Exception) {
        group.cancelAll()
        group.silentWaitForAll()
        throw e
    } catch (e: Error) {
        group.cancelAll()
        group.silentWaitForAll()
        throw e
    }
}
