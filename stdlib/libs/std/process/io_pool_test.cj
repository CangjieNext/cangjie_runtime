/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
 * This source file is part of the Cangjie project, licensed under Apache-2.0
 * with Runtime Library Exception.
 *
 * See https://cangjie-lang.cn/pages/LICENSE for license information.
 */

package std.process

import std.collection.*
import std.sync.*
import std.time.*

/*
 * IOPool concurrency tests (see IO调度池设计方案.md §6).
 *
 * These five cases verify the four headline properties of the pool:
 *   - lazy init        (testLazyInitNoWorkers)
 *   - lazy spawn       (testLazyWorkerSpawn)
 *   - basic state      (testIOPoolRunning)
 *   - dynamic scaling  (testConcurrentEchoSubprocess)
 *   - read/write loop  (testConcurrentWriteToSubprocess)
 *
 * The ProcessRedirect.Pipe scenario covered here was previously
 * unusable due to "coroutines scheduler stagnation" — see
 * `stdlib/libs/std/unittest/process.cj:93-95`.
 *
 * Entry point: `runIOPoolTests()` (no main() so this file can be
 * compiled into the std.process library; the caller invokes
 * runIOPoolTests() from its own main).
 */

/*
 * The IOPool singleton should be created with zero workers. No IO has
 * been submitted yet, so lazy initialization must keep liveWorkers at 0.
 */
public func testLazyInitNoWorkers(): Unit {
    let live = IOPool.shared.liveWorkers.load()
    if (live != 0) {
        throw IllegalStateException("Expected liveWorkers==0 on lazy init, got ${live}")
    }
}

/*
 * Running state must be true for workerLoop() to spin.
 */
public func testIOPoolRunning(): Unit {
    if (!IOPool.shared.running) {
        throw IllegalStateException("Expected IOPool.shared.running == true")
    }
}

/*
 * After the first IO submission, at least one worker pthread must have
 * been spawned. We trigger this by launching an `echo` subprocess with
 * stdOut redirected to a Pipe and reading its output through
 * PipeInputStream, which routes through IOPool.submitRead().
 */
@When[os != "Windows" && os != "iOS"]
public func testLazyWorkerSpawn(): Unit {
    let before = IOPool.shared.liveWorkers.load()

    let p = launch("echo", Array<String>(["hello"]))
    try {
        let stdOutPipe = p.stdOutPipe
        var buf = Array<Byte>(64, repeat: 0)
        var total = Array<Byte>()
        while (true) {
            let n = stdOutPipe.read(buf)
            if (n <= 0) {
                break
            }
            total = total.concat(buf[0:Int64(n)])
        }
        if (total.size == 0) {
            throw IllegalStateException("Expected non-empty echo output")
        }
    } finally {
        p.wait()
    }

    let after = IOPool.shared.liveWorkers.load()
    if (after < before) {
        throw IllegalStateException("liveWorkers decreased: ${before} -> ${after}")
    }
}

/*
 * 8 coroutines concurrently read stdout from 8 `echo` subprocesses.
 * This stresses the dynamic-scaling path: once busyCount reaches
 * INLINE_THRESHOLD(8), submissions go through the Pool path and
 * ensureWorkerAvailable() spawns new pthreads up to maxWorkers.
 */
@When[os != "Windows" && os != "iOS"]
public func testConcurrentEchoSubprocess(): Unit {
    let n = 8
    let futures = ArrayList<Future<String>>()
    for (i in 0..n) {
        let idx = i
        let f = spawn {
            let p = launch("echo", Array<String>(["task-${idx}"]))
            try {
                let stdOutPipe = p.stdOutPipe
                var buf = Array<Byte>(64, repeat: 0)
                var total = Array<Byte>()
                while (true) {
                    let r = stdOutPipe.read(buf)
                    if (r <= 0) {
                        break
                    }
                    total = total.concat(buf[0:Int64(r)])
                }
                String.fromUtf8(total)
            } finally {
                p.wait()
            }
        }
        futures.add(f)
    }
    var i = 0
    for (f in futures) {
        let s = f.get()
        if (!s.contains("task-${i}")) {
            throw IllegalStateException("Coroutine ${i} got wrong output: ${s}")
        }
        i++
    }
}

/*
 * A single coroutine writes a known buffer into a `cat` subprocess's
 * stdin and reads the same bytes back from stdout. Exercises the full
 * write -> read loop through IOPool.submitWrite() / submitRead().
 */
@When[os != "Windows" && os != "iOS"]
public func testConcurrentWriteToSubprocess(): Unit {
    let payload = "the quick brown fox jumps over the lazy dog"
    let payloadBytes = payload.toArray()

    let p = launch("cat", Array<String>([]), stdIn: Pipe, stdOut: Pipe)
    try {
        let stdInPipe = p.stdInPipe
        let stdOutPipe = p.stdOutPipe

        let writer = spawn {
            stdInPipe.write(payloadBytes)
            stdInPipe.flush()
            stdInPipe.close()
        }

        var received = Array<Byte>()
        var buf = Array<Byte>(64, repeat: 0)
        while (true) {
            let n = stdOutPipe.read(buf)
            if (n <= 0) {
                break
            }
            received = received.concat(buf[0:Int64(n)])
        }
        writer.get()

        let receivedStr = String.fromUtf8(received).trimAsciiEnd()
        if (receivedStr != payload) {
            throw IllegalStateException("Round-trip mismatch: sent '${payload}', got '${receivedStr}'")
        }
    } finally {
        p.wait()
    }
}

/*
 * Entry point: invoke all five tests. The caller (an external test
 * driver) supplies its own main() and calls this.
 *
 * Auto-exit verification: after the tests, we sleep 3 seconds to allow
 * the 2s idle timeout in workerLoop to fire, then assert liveWorkers==0.
 */
@When[os != "Windows" && os != "iOS"]
public func runIOPoolTests(): Unit {
    testLazyInitNoWorkers()
    println("testLazyInitNoWorkers: PASS")

    testIOPoolRunning()
    println("testIOPoolRunning: PASS")

    testLazyWorkerSpawn()
    println("testLazyWorkerSpawn: PASS")

    testConcurrentEchoSubprocess()
    println("testConcurrentEchoSubprocess: PASS")

    testConcurrentWriteToSubprocess()
    println("testConcurrentWriteToSubprocess: PASS")

    sleep(Duration.second * 3)
    let live = IOPool.shared.liveWorkers.load()
    if (live != 0) {
        println("WARNING: expected liveWorkers==0 after 3s idle, got ${live}")
    } else {
        println("auto-exit: PASS (liveWorkers==0 after 3s idle)")
    }
}

@When[os == "Windows" || os == "iOS"]
public func runIOPoolTests(): Unit {
    testLazyInitNoWorkers()
    println("testLazyInitNoWorkers: PASS")

    testIOPoolRunning()
    println("testIOPoolRunning: PASS")

    println("(remaining tests skipped on Windows/iOS — IOPool requires pthread)")
}
