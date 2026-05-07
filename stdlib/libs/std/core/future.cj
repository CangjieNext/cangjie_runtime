/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
 * This source file is part of the Cangjie project, licensed under Apache-2.0
 * with Runtime Library Exception.
 *
 * See https://cangjie-lang.cn/pages/LICENSE for license information.
 */

package std.core

@Intrinsic
func futureInit<T>(future: Future<T>): Unit

@Intrinsic
func futureIsComplete<T>(future: Future<T>): Bool

@Intrinsic
func futureWait<T>(future: Future<T>, timeout: Int64): Unit

@Intrinsic
func futureNotifyAll<T>(future: Future<T>): Unit

@FastNative
foreign func CJ_CORE_PrintOomHint(): Unit

let INT64_MAX: Int64 = 0x7fff_ffff_ffff_ffffi64

private open class FutureResult {}

private class FutureResInit <: FutureResult {}

private class FutureOk<T> <: FutureResult {
    var value: T
    init(v: T) {
        value = v
    }
}

private class FutureErr <: FutureResult {
    static let cachedResult = FutureErr(globalOutOfMemoryError)
    var err: Error
    init(e: Error) {
        err = e
    }
}

private class FutureExcept <: FutureResult {
    var except: Exception
    init(e: Exception) {
        except = e
    }
}

/**
 * Represents the result of an asynchronous computation.
 * A Future can be used to wait for and retrieve the result of a spawned thread.
 */
public class Future<T> {
    // `_thread` must be the first field,
    // it will be accessed when spawning a runtime cj thread.
    private let _thread: Thread = Thread()
    private var result: FutureResult = FutureResInit()
    private var executeFn: () -> T

    private init(fn: () -> T) {
        this.executeFn = fn
        unsafe { futureInit(this) }
    }

    /**
     * Returns the thread associated with this Future.
     *
     * @type Thread
     */
    public prop thread: Thread {
        get() {
            _thread
        }
    }

    /**
     * Blocks the current thread, waiting for the result of the thread corresponding
     * to this Future object.
     *
     * @returns T the computed result
     * @throws IllegalStateException if the Future result is in an unexpected state
     * @throws Exception if an exception occurs in the corresponding thread
     * @throws Error if an error occurs in the corresponding thread
     */
    public func get(): T {
        let isComplete = unsafe { futureIsComplete(this) }
        if (!isComplete) {
            unsafe { futureWait(this, Int64.Max) }
        }
        match (result) {
            case v: FutureOk<T> => return v.value
            case e: FutureExcept => throw e.except
            case e: FutureErr => throw e.err
            case _ => throw IllegalStateException("Future result is in an unexpected state.")
        }
    }

    /**
     * Non-blocking method that immediately returns None if the thread has not finished execution.
     * Returns the computed result otherwise.
     *
     * @returns Option<T> Some containing the result if completed, None otherwise
     * @throws IllegalStateException if the Future result is in an unexpected state
     * @throws Exception if an exception occurs in the corresponding thread
     * @throws Error if an error occurs in the corresponding thread
     */
    public func tryGet(): Option<T> {
        let isComplete = unsafe { futureIsComplete(this) }
        if (!isComplete) {
            return Option<T>.None
        }
        match (result) {
            case v: FutureOk<T> => return v.value
            case e: FutureExcept => throw e.except
            case e: FutureErr => throw e.err
            case _ => throw IllegalStateException("Future result is in an unexpected state.")
        }
    }

    /**
     * Blocks the current thread, waiting for the result of the thread corresponding
     * to this Future object. If the thread has not completed execution within the
     * specified timeout, throws TimeoutException. If timeout is less than or equal
     * to zero, behaves the same as `get()`.
     *
     * @param timeout the maximum duration to wait
     * @returns T the computed result
     * @throws TimeoutException if the thread does not complete within the specified timeout
     * @throws IllegalStateException if the Future result is in an unexpected state
     * @throws Exception if an exception occurs in the corresponding thread
     * @throws Error if an error occurs in the corresponding thread
     */
    public func get(timeout: Duration): T {
        if (timeout <= Duration.Zero) {
            return get()
        }
        let isComplete = unsafe { futureIsComplete(this) }
        if (!isComplete) {
            // Wait for `ns` nanoseconds
            unsafe { futureWait(this, timeout.toNanoseconds()) }
            // Check whether the future is complete
            let isCompleteAfterWait = unsafe { futureIsComplete(this) }
            if (!isCompleteAfterWait) {
                throw TimeoutException()
            }
        }
        match (result) {
            case v: FutureOk<T> => return v.value
            case e: FutureExcept => throw e.except
            case e: FutureErr => throw e.err
            case _ => throw IllegalStateException("Future result is in an unexpected state.")
        }
    }

    /**
     * Marks the current future as cancelled and sends a termination request
     * to its executing thread.
     */
    public func cancel(): Unit {
        this._thread.setCancellationRequest()
    }

    func execute(): Unit {
        // Wait until `_thread` object is assigned with its CJThread handle,
        // then, bind the object.
        this._thread.waitForCJThreadHandle()
        setThreadObject(this._thread)
        try {
            this.result = FutureOk<T>(executeFn())
        } catch (ex: Exception) {
            this.result = FutureExcept(ex)
            handleException(ex)
        } catch (er: OutOfMemoryError) {
            this.result = FutureErr.cachedResult
            unsafe { CJ_CORE_PrintOomHint() }
            handleError(er)
        } catch (er: Error) {
            this.result = FutureErr(er)
            er.printStackTrace()
            handleError(er)
        } finally {
            unsafe { futureNotifyAll(this) }
        }
        this._thread.clearRuntimeCJThreadHandle()
        return
    }

    // ** BE AWARE OF: **
    // This method is tied to the runtime implementation details,
    // and if you want to make any changes, you should consider
    // whether you need to update the runtime implementation as well.
    static func executeClosure(fn: () -> T): Unit {
        try {
            fn()
        } catch (ex: Exception) {
            handleException(ex)
        } catch (_: OutOfMemoryError) {
            eprintln("An exception has occurred:")
            eprintln("    Out of memory")
        } catch (er: Error) {
            er.printStackTrace()
        }
        return
    }
}

/**
 * The optional argument type of the `spawn` expression.
 * Specific derived types of `ThreadContext` could affect the behavior of the thread at runtime.
 */
public interface ThreadContext {
    /**
     * Called when the thread context ends.
     */
    func end(): Unit

    /**
     * Checks whether the thread context has ended.
     *
     * @returns Bool true if the context has ended, false otherwise
     */
    func hasEnded(): Bool
}
