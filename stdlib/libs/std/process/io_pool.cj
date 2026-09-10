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
 * Non-generic interface for queue storage. The worker loop dispatches
 * through this interface without knowing the concrete result type.
 */
interface IBlockingTask {
    func execute(): Unit
}

/*
 * A generic blocking task submitted to BlockingCallPool.
 *
 * The submitting coroutine constructs the task, enqueues it, then calls
 * waitResult() which parks the coroutine on `cond` until the worker
 * pthread fills in the result.
 */
class BlockingTask<T> <: IBlockingTask {
    let action: () -> T
    let mtx = Mutex()
    let cond: Condition
    var result: ?T = None
    var completed: Bool = false

    init(action: () -> T) {
        this.action = action
        synchronized (this.mtx) {
            this.cond = this.mtx.condition()
        }
    }

    /*
     * Executed by a worker pthread: performs the blocking call (outside
     * the lock), then writes the result under `mtx` and notifies the
     * waiting coroutine. The mutex unlock -> lock pair provides
     * happens-before, so the coroutine observes the final field values.
     */
    public func execute(): Unit {
        let r = this.action()
        synchronized (this.mtx) {
            this.result = Some(r)
            this.completed = true
            this.cond.notifyAll()
        }
    }

    /*
     * Called by the submitting coroutine: parks until `completed` is set
     * by the worker, then returns the result.
     */
    func waitResult(): T {
        synchronized (this.mtx) {
            while (!this.completed) {
                this.cond.wait()
            }
            return this.result.getOrThrow()
        }
    }
}

/*
 * Blocking Call Pool singleton.
 *
 * Offloads blocking syscalls from the M:N scheduler's OS threads to
 * dedicated pthreads. The submitting coroutine parks (releasing its
 * OS thread to the scheduler) while the worker pthread blocks in the
 * syscall. When the syscall completes, the worker writes the result
 * and notifies the coroutine.
 *
 * Dispatch strategy:
 *   - busyCount < INLINE_THRESHOLD(0): inline fast path, the submitting
 *     coroutine runs the blocking call directly on its current OS
 *     thread. The runtime's syscall handoff mechanism (schmon) will
 *     spawn a replacement thread if this one blocks.
 *   - busyCount >= INLINE_THRESHOLD: Pool path. Construct a BlockingTask,
 *     enqueue, ensure a worker is available (lazy spawn / dynamic
 *     scaling), then park the coroutine.
 *
 * Workers are pthreads created via CJ_IOPool_CreateWorker(); they enter
 * CJ_IOPool_WorkerLoop() -> BlockingCallPool.workerLoop(). Each worker
 * dequeues a task with a 2s timeout; on timeout and empty queue it
 * decrements liveWorkers and exits the pthread.
 */
@When[os != "Windows"]
public class BlockingCallPool {
    public static let shared = BlockingCallPool()

    let taskQueue = LinkedBlockingQueue<IBlockingTask>()

    let busyCount = AtomicInt32(0)
    static const INLINE_THRESHOLD: Int32 = 0

    let liveWorkers = AtomicInt32(0)
    let idleCount = AtomicInt32(0)
    let spawnMutex = Mutex()
    static const maxWorkers: Int32 = 128
    var running: Bool = true
    static const IDLE_TIMEOUT_SEC: Int64 = 2

    private init() {}

    /*
     * Shutdown the pool: stop accepting new tasks, drain idle workers,
     * and wait for all workers to exit.
     *
     * Must be called before runtime shutdown (FiniCJRuntime) to ensure
     * workers exit cleanly via the normal waitqueue wake path, not via
     * ScheduleNonDefaultThreadExit's direct SemaphorePost (which bypasses
     * waitqueue node/timer cleanup and causes heap corruption).
     */
    public func shutdown(): Unit {
        this.running = false
        // Wake all idle workers by adding dummy tasks so they exit via
        // the normal remove() path (WaitqueueWakeOne cleans up node+timer).
        while (this.idleCount.load() > 0) {
            let dummy = BlockingTask<Unit>({ => })
            this.taskQueue.add(dummy)
            sleep(Duration.millisecond * 1)
        }
        // Wait for all workers to exit
        while (this.liveWorkers.load() > 0) {
            sleep(Duration.millisecond * 10)
        }
    }

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
            if (current >= BlockingCallPool.maxWorkers) {
                return
            }

            this.liveWorkers.fetchAdd(1)
            let ret = unsafe { CJ_IOPool_CreateWorker() }
            if (ret != 0) {
                this.liveWorkers.fetchSub(1)
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
        let pool = BlockingCallPool.shared
        while (pool.running) {
            pool.idleCount.fetchAdd(1)

            let taskOpt = pool.taskQueue.remove(Duration.second * BlockingCallPool.IDLE_TIMEOUT_SEC)

            pool.idleCount.fetchSub(1)

            if (taskOpt.isNone()) {
                if (pool.taskQueue.peek().isNone()) {
                    pool.liveWorkers.fetchSub(1)
                    break
                }
                if (!pool.running) {
                    pool.liveWorkers.fetchSub(1)
                    break
                }
                continue
            }

            if (!pool.running) {
                pool.liveWorkers.fetchSub(1)
                break
            }

            taskOpt.getOrThrow().execute()

            pool.busyCount.fetchSub(1)
        }
    }

    /*
     * Submit a generic blocking call. Inline fast path when
     * busyCount < INLINE_THRESHOLD; otherwise enqueue a BlockingTask
     * and park the coroutine.
     */
    public func submit<T>(task: () -> T): T {
        let cur = this.busyCount.fetchAdd(1)
        if (cur < BlockingCallPool.INLINE_THRESHOLD) {
            let r = task()
            this.busyCount.fetchSub(1)
            return r
        }
        let bt = BlockingTask<T>(task)
        this.taskQueue.add(bt)
        this.ensureWorkerAvailable()
        return bt.waitResult()
    }

    /*
     * Convenience: submit a read. Thin wrapper over submit<Int64>.
     */
    public func submitRead(fd: IntNative, buffer: Array<Byte>): Int64 {
        return this.submit<Int64>({ =>
            unsafe {
                let bufPtr = acquireArrayRawData(buffer)
                let readSize = CJ_OS_FileRead(fd, bufPtr.pointer, UIntNative(buffer.size))
                releaseArrayRawData(bufPtr)
                readSize
            }
        })
    }

    /*
     * Convenience: submit a write. Thin wrapper over submit<Bool>.
     */
    public func submitWrite(fd: IntNative, buffer: Array<Byte>): Bool {
        return this.submit<Bool>({ =>
            unsafe {
                let bufPtr = acquireArrayRawData(buffer)
                let success = CJ_OS_FileWrite(fd, bufPtr.pointer, UIntNative(buffer.size))
                releaseArrayRawData(bufPtr)
                success
            }
        })
    }
}

/*
 * C-callable entry point invoked by io_pool_ffi.c's IoWorkerEntry.
 * Control transfers back to the Cangjie-side workerLoop().
 */
@When[os != "Windows"]
@C
func CJ_IOPool_WorkerLoop(): Unit {
    BlockingCallPool.workerLoop()
}
