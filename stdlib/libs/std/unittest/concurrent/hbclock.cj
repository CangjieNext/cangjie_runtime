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
import std.time.DateTime

/**
 * A standalone happens-before clock utility for enforcing and verifying ordering
 * between points in code that run on different threads.
 *
 * `HBClock` works outside the scenario DSL — it can be used in regular `@TestCase`s,
 * in `spawn`ed threads, or inside scenario actor bodies. The clock is backed by a
 * process-global `AtomicInt64` that is `reset()` to 0 automatically before each test
 * case, so independent tests do not interfere with each other.
 *
 * Typical uses: enforce that a server starts before a client connects, or create a
 * contention scenario where two threads reach a lock simultaneously.
 */
public class HBClock {
    static var globalClock: HBClockCounter = HBClockCounter()

    /**
     * Reset the global clock to 0.
     *
     * Called automatically before each test case to ensure a clean state. Users
     * normally do not need to call this manually.
     */
    public static func reset(): Unit {
        globalClock.counter.store(0)
    }

    /**
     * Block the calling thread until the global clock reaches at least `id`, then
     * advance the clock past `id`.
     *
     * The wait is performed with short sleeps (10 ms) so that the calling thread
     * does not busy-spin. Use this for initialization ordering or for creating
     * contention scenarios where a brief sleep is acceptable.
     *
     * @param id clock value to wait for; the clock is advanced from `id` to `id + 1`
     *           only by the thread that observes `counter == id`
     * @param timeout maximum wait duration; defaults to one second
     * @throws TimeoutException if `id` is not reached within `timeout`
     */
    public static func wait(id: Int64, timeout!: Duration = Duration.second): Unit {
        globalClock.wait(id, timeout: timeout)
    }

    /**
     * Block the calling thread until the global clock reaches at least `id`, then
     * advance the clock past `id`, using a busy-spin loop instead of sleeping.
     *
     * Compared to `wait`, this minimises the release latency: all threads reaching
     * the spin point continue execution as simultaneously as the hardware allows.
     * Suitable for benchmarking-grade synchronous release or for creating
     * contention scenarios where minimal scheduling delay is required.
     *
     * This method does not time out — it busy-waits forever if `id` is never
     * reached. Use `wait` if a timeout is needed.
     *
     * @param id clock value to wait for; the clock is advanced from `id` to `id + 1`
     *           only by the thread that observes `counter == id`
     */
    public static func waitSpin(id: Int64): Unit {
        globalClock.waitSpin(id)
    }

    /**
     * Verify that the global clock has already reached at least `id`.
     *
     * Non-blocking: returns immediately if the clock is at least `id`, otherwise
     * throws. Use this to assert that a happens-before edge established by an
     * earlier `wait` / `waitSpin` actually took place — i.e. to detect
     * happens-before violations.
     *
     * @param id clock value that must already have been reached
     * @throws Exception if the clock is strictly less than `id`
     */
    public static func verify(id: Int64): Unit {
        globalClock.verify(id)
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
