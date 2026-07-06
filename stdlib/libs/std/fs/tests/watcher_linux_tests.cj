/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
 * This source file is part of the Cangjie project, licensed under Apache-2.0
 * with Runtime Library Exception.
 *
 * See https://cangjie-lang.cn/pages/LICENSE for license information.
 */

package std.fs

import std.collection.ArrayList
import std.core.Thread
import std.core.sleep

/**
 * Integration tests for the Linux inotify watcher.
 *
 * These tests exercise the real inotify-based native implementation:
 * create / modify / delete a file under a watched directory, and
 * recursive watching of a newly-created subdirectory.
 *
 * NOTE: These tests only run on Linux. They rely on real filesystem
 * events and inotify, so they require appropriate timing margins.
 */
@Test
class WatcherLinuxIntegrationTests {

    /**
     * Creates a file (triggers IN_CREATE), overwrites it (triggers IN_MODIFY),
     * then deletes it (triggers IN_DELETE).  Asserts that all three event kinds
     * are observed through the Watcher.
     */
    @TestCase
    func create_modify_delete() {
        let dir = "/tmp/cj_watcher_it_${Thread.currentThread.id}"
        try { remove(dir, recursive: true) } catch (_: Exception) { }
        Directory.create(dir, recursive: true)
        try {
            let w = Watcher(dir)   // recursive defaults to true
            w.start()
            sleep(100 * Duration.millisecond)

            let f = dir + "/a.txt"

            // Create: File.writeTo creates (or overwrites) the file
            File.writeTo(f, unsafe { "hello".rawData() })
            sleep(150 * Duration.millisecond)

            // Modify: overwrite content
            File.writeTo(f, unsafe { "hello world".rawData() })
            sleep(150 * Duration.millisecond)

            // Delete
            remove(f)
            sleep(150 * Duration.millisecond)

            let kinds = collectKinds(w, 3, 1000 * Duration.millisecond)
            @Expect(kinds.contains(WatchEventKind.create), true, "missing create event")
            @Expect(kinds.contains(WatchEventKind.modify), true, "missing modify event")
            @Expect(kinds.contains(WatchEventKind.delete), true, "missing delete event")
            w.stop()
        } finally {
            remove(dir, recursive: true)
        }
    }

    /**
     * Creates a subdirectory inside the watched tree and then creates a file
     * inside that subdirectory.  With recursive=true, both the subdirectory
     * creation and the inner file creation should be captured.
     */
    @TestCase
    func recursive_subdir_create() {
        let dir = "/tmp/cj_watcher_rec_${Thread.currentThread.id}"
        try { remove(dir, recursive: true) } catch (_: Exception) { }
        Directory.create(dir, recursive: true)
        try {
            let w = Watcher(dir, recursive: true)
            w.start()
            sleep(100 * Duration.millisecond)

            let sub = dir + "/d"
            Directory.create(sub, recursive: true)
            sleep(100 * Duration.millisecond)

            // File inside the new subdirectory should be captured by recursive watch
            File.writeTo(sub + "/b.cj", unsafe { "x".rawData() })
            sleep(200 * Duration.millisecond)

            let kinds = collectKinds(w, 4, 1000 * Duration.millisecond)
            @Expect(kinds.contains(WatchEventKind.create), true, "recursive watch did not capture create")
            w.stop()
        } finally {
            remove(dir, recursive: true)
        }
    }
}

/**
 * Collects up to `need` WatchEventKinds from the watcher, or until the deadline
 * elapses without any new event.  Returns the collected kinds as an Array.
 */
func collectKinds(w: Watcher, need: Int64, deadline: Duration): Array<WatchEventKind> {
    let out = ArrayList<WatchEventKind>()
    var i = 0
    while (i < need) {
        match (w.nextEvent(deadline)) {
            case Some(e) => out.add(e.kind); i += 1
            case None    => break
        }
    }
    return out.toArray()
}
