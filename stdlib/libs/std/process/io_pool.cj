/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
 * This source file is part of the Cangjie project, licensed under Apache-2.0
 * with Runtime Library Exception.
 *
 * See https://cangjie-lang.cn/pages/LICENSE for license information.
 */

package std.process

import std.collection.concurrent.*
import std.sync.*
import std.time.*

/*
 * IO Task kind: Read or Write.
 */
enum IOTaskKind {
    | Read
    | Write
}

/*
 * A single blocking IO operation submitted to IOPool.
 *
 * The submitting coroutine constructs the task, enqueues it, then calls
 * waitAndGetReadResult() / waitAndCheckWriteResult() which park the
 * coroutine on `cond` until the worker pthread fills in the result.
 */
class IOTask {
    let kind: IOTaskKind
    let fd: IntNative
    let buffer: Array<Byte>

    let mtx = Mutex()
    let cond: Condition

    var readResult: Int64 = -1
    var writeSuccess: Bool = false
    var completed: Bool = false

    init(kind!: IOTaskKind, fd!: IntNative, buffer!: Array<Byte>) {
        this.kind = kind
        this.fd = fd
        this.buffer = buffer
        synchronized (this.mtx) {
            this.cond = this.mtx.condition()
        }
    }

    /*
     * Executed by a worker pthread: performs the blocking C call (outside
     * the lock), then writes the result under `mtx` and notifies the
     * waiting coroutine. The mutex unlock -> lock pair provides
     * happens-before, so the coroutine observes the final field values.
     */
    func execute(): Unit {
        var r: Int64 = -1
        var ok: Bool = false
        match (this.kind) {
            case Read =>
                unsafe {
                    var bufPtr = acquireArrayRawData(this.buffer)
                    r = CJ_OS_FileRead(this.fd, bufPtr.pointer, UIntNative(this.buffer.size))
                    releaseArrayRawData(bufPtr)
                }
            case Write =>
                unsafe {
                    var bufPtr = acquireArrayRawData(this.buffer)
                    ok = CJ_OS_FileWrite(this.fd, bufPtr.pointer, UIntNative(this.buffer.size))
                    releaseArrayRawData(bufPtr)
                }
        }
        synchronized (this.mtx) {
            this.readResult = r
            this.writeSuccess = ok
            this.completed = true
            this.cond.notifyAll()
        }
    }

    /*
     * Called by the submitting coroutine: parks until `completed` is set
     * by the worker, then returns the read result.
     */
    func waitAndGetReadResult(): Int64 {
        synchronized (this.mtx) {
            while (!this.completed) {
                this.cond.wait()
            }
            return this.readResult
        }
    }

    /*
     * Called by the submitting coroutine: parks until `completed` is set
     * by the worker, then returns whether the write fully succeeded.
     */
    func waitAndCheckWriteResult(): Bool {
        synchronized (this.mtx) {
            while (!this.completed) {
                this.cond.wait()
            }
            return this.writeSuccess
        }
    }
}

/*
 * IO Pool singleton.
 *
 * Dispatch strategy (see IO调度池设计方案.md §4.2.3):
 *   - busyCount < INLINE_THRESHOLD(8): inline fast path, the submitting
 *     coroutine runs the blocking C call directly on its current OS
 *     thread. Zero allocation, zero thread hop.
 *   - busyCount >= INLINE_THRESHOLD: Pool path. Construct an IOTask,
 *     enqueue, ensure a worker is available (lazy spawn / dynamic
 *     scaling), then park the coroutine.
 *
 * Workers are pthreads created via CJ_IOPool_CreateWorker(); they enter
 * CJ_IOPool_WorkerLoop() -> IOPool.workerLoop(). Each worker dequeues a
 * task with a 2s timeout; on timeout and empty queue it decrements
 * liveWorkers and exits the pthread.
 */
@When[os != "Windows"]
public class IOPool {
    public static let shared = IOPool()

    let taskQueue = LinkedBlockingQueue<IOTask>()

    let busyCount = AtomicInt32(0)
    static const INLINE_THRESHOLD: Int32 = 8

    let liveWorkers = AtomicInt32(0)
    let idleCount = AtomicInt32(0)
    let spawnMutex = Mutex()
    static const maxWorkers: Int32 = 128
    var running: Bool = true
    static const IDLE_TIMEOUT_SEC: Int64 = 2

    private init() {}

    /*
     * Ensure at least one idle worker is available before the submitting
     * coroutine parks itself. Double-checked under spawnMutex to avoid
     * thundering-herd: N concurrent submissions would otherwise create N
     * redundant pthreads.
     */
    private func ensureWorkerAvailable(): Unit {
        if (this.idleCount.load() > 0) {
            return
        }

        synchronized (this.spawnMutex) {
            if (this.idleCount.load() > 0) {
                return
            }

            let current = this.liveWorkers.load()
            if (current >= IOPool.maxWorkers) { // TODO：just return without any info？
                return
            }

            this.liveWorkers.fetchAdd(1)
            let ret = unsafe { CJ_IOPool_CreateWorker() }
            if (ret != 0) {
                this.liveWorkers.fetchSub(1) // TODO：just return without any info？
            }
        }
    }

    /*
     * Worker main loop, running on a pthread.
     *
     * 1. idleCount++  (mark self available)
     * 2. remove(timeout=2s)
     * 3. idleCount--
     * 4. timeout + empty queue -> liveWorkers-- -> break (pthread exits)
     * 5. got task -> execute() -> busyCount-- -> loop
     */
    public static func workerLoop(): Unit {
        let pool = IOPool.shared
        while (pool.running) {
            pool.idleCount.fetchAdd(1)

            let taskOpt = pool.taskQueue.remove(Duration.second * IOPool.IDLE_TIMEOUT_SEC)

            pool.idleCount.fetchSub(1)

            match (taskOpt) {
                case Some(task) =>
                    task.execute()
                case None =>
                    if (pool.taskQueue.peek().isNone()) {
                        pool.liveWorkers.fetchSub(1)
                        break
                    }
                    continue
            }
            pool.busyCount.fetchSub(1)
        }
    }

    /*
     * Submit a read. Inline fast path when busyCount < INLINE_THRESHOLD;
     * otherwise enqueue an IOTask and park the coroutine.
     */
    public func submitRead(fd: IntNative, buffer: Array<Byte>): Int64 {
        let cur = this.busyCount.fetchAdd(1)
        if (cur < IOPool.INLINE_THRESHOLD) {
            let r = unsafe {
                let bufPtr = acquireArrayRawData(buffer)
                let readSize = CJ_OS_FileRead(fd, bufPtr.pointer, UIntNative(buffer.size))
                releaseArrayRawData(bufPtr)
                readSize
            }
            this.busyCount.fetchSub(1)
            return r
        }
        let task = IOTask(kind: Read, fd: fd, buffer: buffer)
        this.taskQueue.add(task)
        this.ensureWorkerAvailable()
        return task.waitAndGetReadResult()
    }

    /*
     * Submit a write. Inline fast path when busyCount < INLINE_THRESHOLD;
     * otherwise enqueue an IOTask and park the coroutine.
     */
    public func submitWrite(fd: IntNative, buffer: Array<Byte>): Bool {
        let cur = this.busyCount.fetchAdd(1)
        if (cur < IOPool.INLINE_THRESHOLD) {
            let ok = unsafe {
                let bufPtr = acquireArrayRawData(buffer)
                let success = CJ_OS_FileWrite(fd, bufPtr.pointer, UIntNative(buffer.size))
                releaseArrayRawData(bufPtr)
                success
            }
            this.busyCount.fetchSub(1)
            return ok
        }
        let task = IOTask(kind: Write, fd: fd, buffer: buffer)
        this.taskQueue.add(task)
        this.ensureWorkerAvailable()
        return task.waitAndCheckWriteResult()
    }

    public func shutdown(): Unit {
        this.running = false
        while (this.liveWorkers.load() > 0) {
            let dummy = IOTask(kind: Read, fd: (-1), buffer: Array<Byte>(1, repeat: 0))
            this.taskQueue.add(dummy)
        }
        while (this.liveWorkers.load() > 0) {
            sleep(Duration.millisecond * 10)
        }
    }
}

/*
 * C-callable entry point invoked by io_pool_ffi.c's IoWorkerEntry.
 * Control transfers back to the Cangjie-side workerLoop().
 */
@When[os != "Windows"]
@C
func CJ_IOPool_WorkerLoop(): Unit {
    IOPool.workerLoop()
}
