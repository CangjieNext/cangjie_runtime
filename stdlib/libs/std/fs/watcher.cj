/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
 * This source file is part of the Cangjie project, licensed under Apache-2.0
 * with Runtime Library Exception.
 *
 * See https://cangjie-lang.cn/pages/LICENSE for license information.
 */

/**
 * @file
 *
 * Watcher: high-level file-system watcher that wraps the native
 * CJ_FS_Watcher* C API.  Events are delivered to a LinkedBlockingQueue
 * consumed via nextEvent() / nextEvent(timeout).
 *
 * Lifecycle: init -> start() -> [nextEvent loop] -> stop().
 * The worker thread polls the native watcher at a fixed interval, converts
 * native events, filters by file-basename, and enqueues them.
 */

package std.fs

import std.collection.concurrent.LinkedBlockingQueue
import std.core.LibC
import std.core.Thread
import std.core.Future
import std.sync.AtomicBool

/** Default poll interval (milliseconds) for the worker loop. */
const WATCHER_POLL_TIMEOUT_MS: Int64 = 200

/** Capacity of the native event buffer passed to CJ_FS_WatcherPoll. */
const WATCHER_BUFFER_CAP: Int64 = 256

/**
 * Watches a file or directory tree for changes and delivers
 * [WatchEvent]s through a bounded queue.
 *
 * ```
 * let w = Watcher("/tmp/project")
 * w.start()
 * while (let Some(ev) <- w.nextEvent(500 * Duration.millisecond)) {
 *     println("event: ${ev.kind} ${ev.path}")
 * }
 * w.stop()
 * ```
 *
 * @throws WatcherException if the path does not exist or the native
 *         watcher cannot be created.
 */
public class Watcher {
    private let queue: LinkedBlockingQueue<WatchEvent>
    private let recursive: Bool
    private let dirPath: String          // native receives this directory
    private let watchIsFile: Bool        // true when init(path) targets a single file
    private let fileBasename: String     // basename filter for single-file watches

    private var handle: UIntNative = UIntNative(0)  // 0 = not created / already destroyed
    private let stopped = AtomicBool(false)
    private var workerFuture: Option<Future<Unit>> = None

    /**
     * Creates a Watcher for the given path.
     *
     * @param path       File or directory to watch.
     * @param recursive  Watch subdirectories recursively (default true).
     * @param capacity   Queue capacity for buffered events (default 1024).
     * @throws WatcherException if the path does not exist or native init fails.
     */
    public init(path: String, recursive!: Bool = true,
                capacity!: Int64 = 1024) {
        this.recursive = recursive
        this.queue = LinkedBlockingQueue<WatchEvent>(capacity)

        // Resolve file vs directory: single file -> watch parent dir + basename filter
        if (!exists(path)) {
            throw WatcherException("path does not exist: ${path}")
        } else if (!FileInfo(path).isDirectory()) {
            this.watchIsFile = true
            this.dirPath = parentOf(path)
            this.fileBasename = basenameOf(path)
        } else {
            this.watchIsFile = false
            this.dirPath = path
            this.fileBasename = String()
        }

        this.handle = createNative(this.dirPath, recursive)
    }

    /** Starts the background worker thread. No-op if already started. */
    public func start(): Unit {
        if (workerFuture.isSome()) { return }
        stopped.store(false)
        workerFuture = Some(spawn {
            => workerLoop()
        })
    }

    /** Stops the watcher. Safe to call multiple times. */
    public func stop(): Unit {
        if (stopped.compareAndSwap(false, true)) {
            match (workerFuture) {
                case Some(f) => f.cancel()
                case None => ()
            }
        }
    }

    /** Exposes the underlying event queue for advanced consumers. */
    public prop events: LinkedBlockingQueue<WatchEvent> {
        get() { queue }
    }

    /**
     * Blocks until an event is available. Returns None once the watcher
     * is stopped AND all queued events have been drained.
     */
    public func nextEvent(): Option<WatchEvent> {
        while (true) {
            match (queue.remove(WATCHER_POLL_TIMEOUT_MS * Duration.millisecond)) {
                case Some(e) => return Some(e)
                case None =>
                    if (stopped.load() && queue.size == 0) { return None }
            }
        }
        return None // unreachable; satisfies return-type on all paths
    }

    /**
     * Blocks for at most `timeout` waiting for a single event.
     * Returns None if no event arrives within the timeout.
     */
    public func nextEvent(timeout: Duration): Option<WatchEvent> {
        return queue.remove(timeout)
    }

    // ---------------------------------------------------------------------------
    // Worker loop
    // ---------------------------------------------------------------------------

    private func workerLoop(): Unit {
        let buf = unsafe { Array<WatchEventNative>(WATCHER_BUFFER_CAP,
                                          repeat: zeroValue<WatchEventNative>()) }
        while (!stopped.load()) {
            if (Thread.currentThread.hasPendingCancellation) { break }
            if (handle == UIntNative(0)) { break }
            let count = pollNative(handle, WATCHER_POLL_TIMEOUT_MS, buf,
                                   WATCHER_BUFFER_CAP)
            if (count < 0) {
                // Fatal error or OS watch lost: push watchDisappeared and exit
                queue.add(WatchEvent(WatchEventKind.watchDisappeared, String(), false))
                stopped.store(true)
                break
            }
            // Convert ready events, filter, enqueue (copy paths before next poll)
            for (i in 0..count) {
                let ne = buf[i]
                let pathStr = cstringToString(ne.path)
                if (!accept(pathStr)) { continue }
                queue.add(WatchEvent(mapKind(ne.kind), pathStr, ne.isDir != 0))
            }
        }
        destroyNative(handle)
        handle = UIntNative(0)
    }

    // ---------------------------------------------------------------------------
    // Path predicate: single-file basename
    // ---------------------------------------------------------------------------

    private func accept(relPath: String): Bool {
        // FSEvents is inherently recursive; reject subdirectory events when
        // non-recursive (unless watching a single file, where basename filter applies).
        if (!recursive && !watchIsFile && (relPath.contains("/") || relPath.contains("\\"))) {
            return false
        }
        if (watchIsFile && basenameOf(relPath) != fileBasename) { return false }
        return true
    }

    // ---------------------------------------------------------------------------
    // Native wrappers (private)
    // ---------------------------------------------------------------------------

    private func createNative(dir: String, recursive: Bool): UIntNative {
        unsafe {
            let cDir = LibC.mallocCString(dir)
            // Simplified: pass null CPointer<CString> for errOut.
            // The detailed OS error string will be retrieved in Task 7.
            let errOut: CPointer<CString> = CPointer<CString>()
            let h = CJ_FS_WatcherCreate(cDir, if (recursive) { 1 } else { 0 }, errOut)
            LibC.free(cDir)
            if (h == UIntNative(0)) {
                throw WatcherException("CJ_FS_WatcherCreate failed for: ${dir} (check: path exists and is a directory; read permission; on Linux, fs.inotify.max_user_watches limit)")
            }
            return h
        }
    }

    private func pollNative(h: UIntNative, timeoutMs: Int64,
                            buf: Array<WatchEventNative>,
                            cap: Int64): Int64 {
        unsafe {
            let bufHandle = acquireArrayRawData(buf)
            try {
                // Simplified: pass null CPointer<CString> for errOut.
                let errOut: CPointer<CString> = CPointer<CString>()
                return CJ_FS_WatcherPoll(h, timeoutMs,
                                          bufHandle.pointer, cap, errOut)
            } finally {
                releaseArrayRawData(bufHandle)
            }
        }
    }

    private func destroyNative(h: UIntNative): Unit {
        if (h != UIntNative(0)) { unsafe { CJ_FS_WatcherDestroy(h) } }
    }
}

// ---------------------------------------------------------------------------
// Path utilities
// ---------------------------------------------------------------------------

/** Returns the parent directory of a path string, or "." if none. */
func parentOf(p: String): String {
    let fwd = p.lastIndexOf("/").getOrDefault { -1 }
    let bwd = p.lastIndexOf("\\").getOrDefault { -1 }
    let idx = if (fwd > bwd) { fwd } else { bwd }
    if (idx <= 0) { return "." }
    return p[0..idx]
}

/** Returns the last path component (file name) of a path string. */
func basenameOf(p: String): String {
    let fwd = p.lastIndexOf("/").getOrDefault { -1 }
    let bwd = p.lastIndexOf("\\").getOrDefault { -1 }
    let idx = if (fwd > bwd) { fwd } else { bwd }
    if (idx < 0) { return p }
    return p[(idx + 1)..p.size]
}

