/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
 * This source file is part of the Cangjie project, licensed under Apache-2.0
 * with Runtime Library Exception.
 *
 * See https://cangjie-lang.cn/pages/LICENSE for license information.
 */

package std.fs

import std.core.Thread
import std.core.sleep

/**
 * Lifecycle tests for the std.fs Watcher.
 *
 * Tests Watcher start/stop behaviour, double-stop safety, and
 * that constructing a Watcher on a non-existent path throws WatcherException.
 */
@Test
class WatcherLifecycleTests {
    @TestCase
    func start_stop_returns_none() {
        let dir = "/tmp/cj_watcher_test_${Thread.currentThread.id}"
        if (!exists(dir)) { Directory.create(dir, recursive: true) }
        try {
            let w = Watcher(dir)
            w.start()
            // Stub native returns 0 events, so nextEvent should return None
            // after stop drains the queue.
            let fut = spawn {
                => while (let Some(ev) <- w.nextEvent(50 * Duration.millisecond)) {
                    @Assert(false)  // stub should never produce events
                }
            }
            sleep(200 * Duration.millisecond)
            w.stop()
            fut.get()  // worker should exit cleanly
        } finally {
            if (exists(dir)) { remove(dir, recursive: true) }
        }
    }

    @TestCase
    func double_stop_safe() {
        let dir = "/tmp/cj_watcher_test2_${Thread.currentThread.id}"
        if (!exists(dir)) { Directory.create(dir, recursive: true) }
        try {
            let w = Watcher(dir)
            w.start()
            w.stop()
            w.stop()  // must not throw
            @Expect(true, true)
        } finally {
            if (exists(dir)) { remove(dir, recursive: true) }
        }
    }

    @TestCase
    func non_existent_path_throws() {
        var caught = false
        try {
            let w = Watcher("/tmp/cj_nonexistent_path_12345")
        } catch (_: WatcherException) {
            caught = true
        }
        @Expect(caught, true)
    }
}
