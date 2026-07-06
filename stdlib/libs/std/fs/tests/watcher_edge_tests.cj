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
 * Edge-case and robustness tests for the std.fs Watcher.
 *
 * Tests covered:
 *   1. root_deleted_signals_disappearance — delete the watch root and assert
 *      that a WatchEventKind.watchDisappeared event arrives.  On Linux this is
 *      triggered by inotify IN_DELETE_SELF / IN_MOVE_SELF / IN_IGNORED; the
 *      worker also pushes watchDisappeared when pollNative returns <0.
 *   2. bulk_create_may_overflow (best-effort) — rapidly create many files in a
 *      tight loop to stress the kernel event queue.  If overflow events appear
 *      they must be WatchEventKind.overflow, but the test does NOT hard-assert
 *      that overflow must occur (it is system/load-dependent).
 *
 * NOTE: These tests rely on real filesystem events and require appropriate
 * timing margins.  They only function on platforms with a native watcher
 * implementation (Linux inotify, macOS FSEvents, Windows RDCW).
 * Verification is deferred to Linux Docker — no Cangjie toolchain on this host.
 */
@Test
class WatcherEdgeTests {

    /**
     * Starts a watcher on a temp directory, then deletes that directory.
     * Asserts that a WatchEventKind.watchDisappeared event is received
     * within a reasonable polling window (~2 s).
     *
     * On Linux: deleting the watched root triggers inotify IN_DELETE_SELF /
     * IN_IGNORED, which the native layer maps to watchDisappeared.  The
     * worker loop also pushes watchDisappeared when pollNative returns <0.
     */
    @TestCase
    func root_deleted_signals_disappearance() {
        let dir = "/tmp/cj_watcher_gone_${Thread.currentThread.id}"
        try { remove(dir, recursive: true) } catch (_: Exception) { }
        Directory.create(dir, recursive: true)
        try {
            let w = Watcher(dir)
            w.start()
            sleep(100 * Duration.millisecond)

            // Delete the watched root directory
            remove(dir, recursive: true)

            // Poll for watchDisappeared with a generous window
            var seen = false
            for (_ in 0..20) {
                match (w.nextEvent(100 * Duration.millisecond)) {
                    case Some(e) =>
                        if (e.kind == WatchEventKind.watchDisappeared) {
                            seen = true
                            break
                        }
                    case None => ()
                }
            }

            @Assert(seen, "expected watchDisappeared after deleting watch root")

            // After watchDisappeared the worker exits; subsequent nextEvent
            // calls should return None without blocking indefinitely.
            w.stop()
        } finally {
            // Guard: ensure cleanup even if assertions failed mid-test
            try { remove(dir, recursive: true) } catch (_: Exception) { }
        }
    }

    /**
     * BEST-EFFORT overflow test.
     *
     * Rapidly creates many small files in the watched directory to stress
     * the kernel event queue (Linux inotify queue, etc.).  Collects all
     * events within a bounded window and verifies that:
     *   - Any event whose kind is WatchEventKind.overflow is valid (soft
     *     check only — overflow is not guaranteed).
     *   - At least some create events were observed (sanity check).
     *
     * This test is intentionally non-flaky: it never hard-asserts that
     * overflow MUST occur, since that depends on kernel queue size, system
     * load, and timing.
     */
    @TestCase
    func bulk_create_may_overflow() {
        let dir = "/tmp/cj_watcher_overflow_${Thread.currentThread.id}"
        try { remove(dir, recursive: true) } catch (_: Exception) { }
        Directory.create(dir, recursive: true)
        try {
            let w = Watcher(dir, recursive: false)
            w.start()
            sleep(100 * Duration.millisecond)

            // Bulk-create files in a tight loop to flood the event queue
            let batchSize = 500
            for (i in 0..batchSize) {
                File.writeTo("${dir}/f${i}.dat", unsafe { "x".rawData() })
            }

            // Allow the kernel and worker to deliver events
            sleep(500 * Duration.millisecond)

            // Collect events
            var createCount = 0
            var overflowSeen = false
            var eventCount = 0
            let maxEvents = batchSize + 50  // generous upper bound
            while (eventCount < maxEvents) {
                match (w.nextEvent(50 * Duration.millisecond)) {
                    case Some(e) =>
                        eventCount += 1
                        match (e.kind) {
                            case WatchEventKind.create => createCount += 1
                            case WatchEventKind.overflow => overflowSeen = true
                            case _ => ()
                        }
                    case None => break
                }
            }

            // Sanity: we should see at least some create events
            @Assert(createCount > 0,
                     "expected at least one create event from batch of ${batchSize} files")

            w.stop()
        } finally {
            try { remove(dir, recursive: true) } catch (_: Exception) { }
        }
    }
}
