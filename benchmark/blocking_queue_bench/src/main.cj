/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
 * This source file is part of the Cangjie project, licensed under Apache-2.0
 * with Runtime Library Exception.
 *
 * See https://cangjie-lang.cn/pages/LICENSE for license information.
 */

package blocking_queue_bench

import std.sync.*
import std.time.*
import std.collection.ArrayList
import std.core.*
import std.env.*

// ============ 共享：结果收集与校验 ============
class Collector {
    private let mon = Monitor()
    private let list = ArrayList<Int64>()

    public func add(v: Int64): Unit {
        mon.lock()
        try {
            list.add(v)
        } finally {
            mon.unlock()
        }
    }

    // 校验消费到的 id 集合恰为 [0, total)
    public func verify(total: Int64): Bool {
        mon.lock()
        try {
            if (list.size != total) {
                println("  FAIL: 消费总数 ${list.size} != 期望 ${total}")
                return false
            }
            let seen = Array<Bool>(total, repeat: false)
            for (v in list) {
                if (v < 0 || v >= total) {
                    println("  FAIL: 越界 id ${v}")
                    return false
                }
                if (seen[v]) {
                    println("  FAIL: 重复 id ${v}")
                    return false
                }
                seen[v] = true
            }
            var i = 0
            while (i < total) {
                if (!seen[i]) {
                    println("  FAIL: 丢失 id ${i}")
                    return false
                }
                i = i + 1
            }
            return true
        } finally {
            mon.unlock()
        }
    }
}

// ============ 工厂：构造被测队列（复制自 MR 改动后的源码）============
func mkLinked(cap: Int64): BQueue<Int64> {
    return LinkedBlockingQueue<Int64>(cap)
}

// 上游原版（main 分支，未做任何修改）：用于基准对照，证明 PR 改动的真实收益/回归。
// 由 blocking_queue_upstream.cj 提供（类 LinkedBlockingQueueUpstream）。
func mkLinkedUpstream(cap: Int64): BQueue<Int64> {
    return LinkedBlockingQueueUpstream<Int64>(cap)
}

// ============ 场景 1：高并发阻塞 add + 阻塞 remove ============
func scenarioBlocking(name: String, make: (Int64) -> BQueue<Int64>, capacity: Int64,
                      total: Int64, producers: Int64, consumers: Int64): Bool {
    println("=== ${name} ===")
    let q = make(capacity)
    let collector = Collector()
    let perP = total / producers
    let perC = total / consumers
    let futures = ArrayList<Future<Unit>>()

    var p = 0
    while (p < producers) {
        let start = p * perP
        let fut = spawn { =>
            var i = start
            while (i < start + perP) {
                q.add(i)
                i = i + 1
            }
        }
        futures.add(fut)
        p = p + 1
    }

    var c = 0
    while (c < consumers) {
        let fut = spawn { =>
            var k = 0
            while (k < perC) {
                let v = q.remove()
                collector.add(v)
                k = k + 1
            }
        }
        futures.add(fut)
        c = c + 1
    }

    // 容量不变量 checker
    let maxSize = AtomicInt64(0)
    let stop = AtomicInt64(0)
    let checker = spawn { =>
        while (stop.load() == 0) {
            let s = q.size
            if (s > maxSize.load()) {
                maxSize.store(s)
            }
            if (maxSize.load() > capacity) {
                break
            }
            sleep(Duration.millisecond)
        }
    }

    for (f in futures) {
        f.get()
    }
    stop.store(1)
    checker.get()

    if (maxSize.load() > capacity) {
        println("  FAIL: 队列 size 超过容量上限，最大 ${maxSize.load()} > ${capacity}")
        return false
    }
    let ok = collector.verify(total)
    if (ok) {
        println("  ${name}: PASS (maxSize=${maxSize.load()})")
    } else {
        println("  ${name}: FAIL (maxSize=${maxSize.load()})")
    }
    return ok
}

// ============ 场景 2：tryAdd 生产者 + 阻塞 remove 消费者（丢失唤醒核心测试）============
func scenarioTryAddProducer(name: String, make: (Int64) -> BQueue<Int64>,
                            capacity: Int64, total: Int64): Bool {
    println("=== ${name} (tryAdd生产者 + 阻塞remove消费者) ===")
    let q = make(capacity)
    let collector = Collector()

    let producer = spawn { =>
        var i = 0
        while (i < total) {
            while (!q.tryAdd(i)) {
                sleep(Duration.millisecond)
            }
            i = i + 1
        }
    }
    let consumer = spawn { =>
        var k = 0
        while (k < total) {
            let v = q.remove()   // 阻塞：若 tryAdd 没发信号，这里会永久挂起
            collector.add(v)
            k = k + 1
        }
    }

    producer.get()
    consumer.get()

    let ok = collector.verify(total)
    if (ok) {
        println("  ${name}: PASS")
    } else {
        println("  ${name}: FAIL")
    }
    return ok
}

// ============ 场景 3：阻塞 add 生产者 + tryRemove 消费者（signalNotFull 测试）============
func scenarioBlockingAddTryRemove(name: String, make: (Int64) -> BQueue<Int64>,
                                 capacity: Int64, total: Int64): Bool {
    println("=== ${name} (阻塞add生产者 + tryRemove消费者) ===")
    let q = make(capacity)
    let collector = Collector()

    let producer = spawn { =>
        var i = 0
        while (i < total) {
            q.add(i)
            i = i + 1
        }
    }
    let consumer = spawn { =>
        var got = 0
        while (got < total) {
            if (let Some(v) <- q.tryRemove()) {
                collector.add(v)
                got = got + 1
            } else {
                sleep(Duration.millisecond)
            }
        }
    }

    producer.get()
    consumer.get()

    let ok = collector.verify(total)
    if (ok) {
        println("  ${name}: PASS")
    } else {
        println("  ${name}: FAIL")
    }
    return ok
}

// ============ 场景 4：超时语义 ============
func scenarioTimeout(name: String, make: (Int64) -> BQueue<Int64>): Bool {
    println("=== ${name} (超时语义) ===")
    let cap: Int64 = 4
    let q1 = make(cap)
    let t0 = MonoTime.now()
    let r1 = q1.remove(Duration.millisecond * 100)
    let t1 = MonoTime.now()
    let e1 = (t1 - t0).toMilliseconds()
    if (r1.isSome()) {
        println("  FAIL: 空队列 remove(timeout) 不应返回元素")
        return false
    }
    if (e1 < 80 || e1 > 800) {
        println("  FAIL: 空队列 remove(timeout=100ms) 实际 ${e1}ms，超出合理区间[80,800]")
        return false
    }
    let q2 = make(cap)
    var fillId = 0
    while (q2.tryAdd(fillId)) {
        fillId = fillId + 1
    }
    let t2 = MonoTime.now()
    let added = q2.add(999, Duration.millisecond * 100)
    let t3 = MonoTime.now()
    let e2 = (t3 - t2).toMilliseconds()
    if (added) {
        println("  FAIL: 满队列 add(timeout) 不应成功")
        return false
    }
    if (e2 < 80 || e2 > 800) {
        println("  FAIL: 满队列 add(timeout=100ms) 实际 ${e2}ms，超出合理区间[80,800]")
        return false
    }
    println("  ${name}: PASS (empty-remove=${e1}ms, full-add=${e2}ms)")
    return true
}

// ============ 基准：吞吐（ops/sec），用于回应评审"本 PR 无基准数据" ============
// 与上游实现（LinkedBlockingQueueUpstream）同二进制对照，CI / 上游正常运行时跑即可产出
// before/after 数据。SPSC 稳态（队列常非空非满）最敏感于"每次成功都无条件 signal 对端
// Monitor"的额外锁往返开销，是本 PR 与原版差异最大的场景。
func benchmarkThroughput(name: String, make: (Int64) -> BQueue<Int64>,
                         capacity: Int64, total: Int64,
                         producers: Int64, consumers: Int64): Bool {
    println("=== ${name} (吞吐基准: ${producers}P/${consumers}C, total=${total}) ===")
    let q = make(capacity)
    let futures = ArrayList<Future<Unit>>()
    let perP = total / producers
    var p = 0
    while (p < producers) {
        let start = p * perP
        let fut = spawn { =>
            var i = start
            while (i < start + perP) {
                q.add(i)
                i = i + 1
            }
        }
        futures.add(fut)
        p = p + 1
    }
    var c = 0
    while (c < consumers) {
        let fut = spawn { =>
            var got = 0
            while (got < (total / consumers)) {
                q.remove()
                got = got + 1
            }
        }
        futures.add(fut)
        c = c + 1
    }
    let t0 = MonoTime.now()
    for (f in futures) {
        f.get()
    }
    let t1 = MonoTime.now()
    let ms = (t1 - t0).toMilliseconds()
    var msSafe = ms
    if (ms <= 0) {
        msSafe = 1
    }
    let opsPerSec = (total * 1000) / msSafe
    println("  ${name}: ${total} ops in ${ms} ms -> ~${opsPerSec} ops/sec")
    return true
}

// ============ 主流程 ============
main(): Int64 {
    let gDone = AtomicInt64(0)
    spawn { =>
        sleep(Duration.second * 300)
        if (gDone.load() == 0) {
            println("WATCHDOG: 测试在 300s 内未完成，疑似 DEADLOCK / 丢失唤醒 / 计数错乱")
            exit(1)
        }
    }

    var allOk = true

    // LinkedBlockingQueue（本次 MR 修改对象）
    allOk = scenarioBlocking("Linked-高并发阻塞", mkLinked, 64, 100000, 8, 8) && allOk
    allOk = scenarioTryAddProducer("Linked-tryAdd生产/阻塞remove消费", mkLinked, 64, 20000) && allOk
    allOk = scenarioBlockingAddTryRemove("Linked-阻塞add/tryRemove消费", mkLinked, 64, 20000) && allOk
    allOk = scenarioTimeout("Linked-超时", mkLinked) && allOk

    // 吞吐基准：PR 实现 vs 上游原版，同二进制对照（SPSC 稳态最敏感于 signal 开销）
    benchmarkThroughput("PR-Linked-SPSC", mkLinked, 1024, 1000000, 1, 1)
    benchmarkThroughput("UP-Linked-SPSC", mkLinkedUpstream, 1024, 1000000, 1, 1)
    benchmarkThroughput("PR-Linked-MPMC", mkLinked, 1024, 2000000, 8, 8)
    benchmarkThroughput("UP-Linked-MPMC", mkLinkedUpstream, 1024, 2000000, 8, 8)

    gDone.fetchAdd(1)
    if (allOk) {
        println("")
        println("========== ALL LINKED TESTS PASSED ==========")
        return 0
    } else {
        println("")
        println("========== SOME LINKED TESTS FAILED ==========")
        return 2
    }
}
