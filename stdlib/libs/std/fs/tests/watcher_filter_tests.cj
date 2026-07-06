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
 * Integration tests for single-file watching.
 *
 * These tests verify that:
 *   A Watcher pointed at a single file automatically filters events so
 *   that only changes to that specific file are reported.
 *
 * NOTE: These tests rely on real filesystem events and require appropriate
 * timing margins.  They only function on platforms with a native watcher
 * implementation (Linux inotify, macOS FSEvents, Windows RDCW).
 */
@Test
class WatcherFilterTests {

    /**
     * Watches a single file (not a directory).  Writes a sibling file that
     * should be filtered out, then modifies the target file.  Verifies
     * that the received event's path matches the watched file's basename.
     */
    @TestCase
    func single_file_watch() {
        let dir = "/tmp/cj_watcher_file_${Thread.currentThread.id}"
        try { remove(dir, recursive: true) } catch (_: Exception) { }
        Directory.create(dir, recursive: true)
        let f = dir + "/only.txt"
        File.writeTo(f, unsafe { "a".rawData() })
        try {
            let w = Watcher(f, recursive: false)
            w.start()
            sleep(100 * Duration.millisecond)

            // Write a sibling file — should be filtered out by basename check
            File.writeTo(dir + "/other.log", unsafe { "noise".rawData() })
            // Modify the target file — should produce an event
            File.writeTo(f, unsafe { "bb".rawData() })
            sleep(300 * Duration.millisecond)

            var gotTarget = false
            var gotNoise = false
            var count = 0
            let maxEvents = 8
            while (count < maxEvents) {
                match (w.nextEvent(200 * Duration.millisecond)) {
                    case Some(e) =>
                        if (e.path.endsWith("only.txt")) {
                            gotTarget = true
                        } else if (e.path.endsWith("other.log")) {
                            gotNoise = true
                        }
                        count += 1
                    case None => break
                }
            }

            @Assert(gotTarget, "expected to receive an event for only.txt")
            @Assert(!gotNoise, "single-file watch should have suppressed other.log event")

            w.stop()
        } finally {
            try { remove(dir, recursive: true) } catch (_: Exception) { }
        }
    }
}
