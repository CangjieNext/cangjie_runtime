// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

import std.collection.ArrayDeque
import std.collection.ArrayList
import std.collection.HashSet
import std.time.MonoTime

let SAMPLES: Int64 = 9
let WARMUPS: Int64 = 3
let OPS: Int64 = 1000000
let ISSUE_OPS: Int64 = 500000

func nanosSince(start: MonoTime): Int64 {
    return (MonoTime.now() - start).toNanoseconds()
}

func sortSamples(samples: Array<Int64>): Unit {
    for (i in 1..samples.size) {
        let key = samples[i]
        var j = i - 1
        while (j >= 0 && samples[j] > key) {
            samples[j + 1] = samples[j]
            j--
        }
        samples[j + 1] = key
    }
}

func meanNs(samples: Array<Int64>): Int64 {
    var sum: Int64 = 0
    for (value in samples) {
        sum += value
    }
    return sum / samples.size
}

func benchListAppendNoGrow(ops: Int64): Int64 {
    var list = ArrayList<Int64>(ops)
    var i: Int64 = 0
    while (i < ops) {
        list.add(i)
        i++
    }
    return list.size
}

func benchDequeAddLastNoGrow(ops: Int64): Int64 {
    // ArrayDeque keeps one empty slot to distinguish empty from full.
    var deque = ArrayDeque<Int64>(ops + 1)
    var i: Int64 = 0
    while (i < ops) {
        deque.addLast(i)
        i++
    }
    return deque.size
}

func benchListAppendGrow(ops: Int64): Int64 {
    var list = ArrayList<Int64>()
    var i: Int64 = 0
    while (i < ops) {
        list.add(i)
        i++
    }
    return list.size
}

func benchDequeAddLastGrow(ops: Int64): Int64 {
    var deque = ArrayDeque<Int64>()
    var i: Int64 = 0
    while (i < ops) {
        deque.addLast(i)
        i++
    }
    return deque.size
}

func checkUniqueList(ops: Int64): Int64 {
    var seen = HashSet<Int64>(ops)
    var list = ArrayList<Int64>(ops)
    var i: Int64 = 0
    while (i < ops) {
        if (!seen.contains(i)) {
            seen.add(i)
            list.add(i)
        }
        i++
    }
    return list.size + seen.size
}

func checkUniqueDeque(ops: Int64): Int64 {
    var seen = HashSet<Int64>(ops)
    var deque = ArrayDeque<Int64>(ops + 1)
    var i: Int64 = 0
    while (i < ops) {
        if (!seen.contains(i)) {
            seen.add(i)
            deque.addLast(i)
        }
        i++
    }
    return deque.size + seen.size
}

func runCase(name: String, ops: Int64, body: (Int64) -> Int64): Unit {
    var checksum: Int64 = 0
    for (_ in 0..WARMUPS) {
        checksum += body(ops)
    }

    let samples = Array<Int64>(SAMPLES, repeat: 0)
    for (i in 0..SAMPLES) {
        let start = MonoTime.now()
        checksum += body(ops)
        samples[i] = nanosSince(start)
    }

    sortSamples(samples)
    let median = samples[SAMPLES / 2]
    let mean = meanNs(samples)
    println("${name}, ops=${ops}, median_ns=${median}, mean_ns=${mean}, checksum=${checksum}")
}

main(): Int64 {
    println("Issue585 ArrayDeque.addLast benchmark")
    println("samples=${SAMPLES}, warmups=${WARMUPS}")
    runCase("ArrayList.add/no-grow", OPS, benchListAppendNoGrow)
    runCase("ArrayDeque.addLast/no-grow", OPS, benchDequeAddLastNoGrow)
    runCase("ArrayList.add/grow", OPS, benchListAppendGrow)
    runCase("ArrayDeque.addLast/grow", OPS, benchDequeAddLastGrow)
    runCase("checkUniqueList", ISSUE_OPS, checkUniqueList)
    runCase("checkUniqueDeque", ISSUE_OPS, checkUniqueDeque)
    return 0
}
