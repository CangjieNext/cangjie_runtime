package std.concurrent

import std.collection.ArrayList
import std.collection.concurrent.LinkedBlockingQueue
import std.sync.AtomicUInt64

class ThreadGroupIterator<T> <: Iterator<T> {
    public ThreadGroupIterator(let group: ThreadGroup<T>) {}

    public override func next(): Option<T> {
        return group.nextResult()
    }
}

enum Result<T> {
    | Ok(T)
    | Err(Exception)
}

public class ThreadGroup<T> <: Iterable<T> {
    private let futures = ArrayList<Future<Unit>>()
    private let completed = LinkedBlockingQueue<Result<T>>()
    private var unconsumed: AtomicUInt64 = AtomicUInt64(0)

    public func add(task: () -> T): Unit {
        unconsumed.fetchAdd(1)
        let future = spawn {
            try {
                completed.add(Ok(task()))
            } catch (e: Exception) {
                completed.add(Err(e))
            }
        }
        futures.add(future)
    }

    public func cancelAll() {
        for (future in futures) {
            future.cancel()
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
    }
}
