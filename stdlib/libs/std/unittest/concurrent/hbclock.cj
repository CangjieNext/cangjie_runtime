/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
 * This source file is part of the Cangjie project, licensed under Apache-2.0
 * with Runtime Library Exception.
 *
 * See https://cangjie-lang.cn/pages/LICENSE for license information.
 */

package std.unittest.concurrent


import std.collection.HashMap
import std.sync.AtomicInt64
// import std.time.Duration
import std.time.DateTime
// import std.unittest.common.AssertException


var registry: HashMap<String, Int64> = HashMap()
// happens before clock
public class HBClock {
    static var globalClock: HBClockCounter = HBClockCounter()

    func reset(): Unit {
        globalClock.counter.store(0)
        registry.clear()
    }

    // Enforce happens-before order between particular points in code using absolute ordering.
    // Can be used for initialization or to create specific contention scenarios. 
    public static func wait(id: Int64, timeout!: Duration = Duration.second): Unit {
        globalClock.wait(id, timeout: timeout)
    }

    // Same as `wait` but use spinning for waiting to ensure that all threads reaching this point will continue execution at as same time as possible.
    // Used for benchmarking or creating specific contention scenarios.
    public static func waitSpin(id: Int64): Unit {
        globalClock.waitSpin(id)
    }

    // Verify that happens-before order was not violated relative to all points of code with different id.
    public static func verify(id: Int64): Unit {
        globalClock.verify(id)
    }

    public static func verify(before!: String): Unit {
        let id = registry.get(before) ?? throw IllegalArgumentException("No counter found for identifier: ${before}")
        globalClock.verify(id + 1)
    }

    public static func register(ident: String): Unit {
        if (let Some(existingId) <- registry.get(ident)) {

        }
        var id = globalClock.counter.fetchAdd(1)
        registry[ident] = id
    }

}

class HBClockCounter {
    var counter: AtomicInt64 = AtomicInt64(0)

    // HBClockCounter(let name: String) {}

    public func wait(id: Int64, timeout!: Duration = Duration.second): Unit {
        let startTime = DateTime.now()

        while (counter.load() < id) {
            if (DateTime.now() - startTime > timeout) {
                throw TimeoutException("Timeout while waiting for ${id} to be reached. Current value: ${counter.load()}")
            }
            sleep(Duration.millisecond * 10) // Sleep briefly to avoid busy waiting
        }
        counter.compareAndSwap(id, id + 1)
    }

    public func waitSpin(id: Int64): Unit {
        while (counter.load() < id) {
            // Busy wait without sleeping to ensure minimal delay and synchronization
        }
        counter.compareAndSwap(id, id + 1)
    }

    public func verify(id: Int64): Unit {
        if (counter.load() < id) {
            throw Exception("Happens-before order violated: expected at least $id, but got ${counter.load()}")
        }
    }
}
