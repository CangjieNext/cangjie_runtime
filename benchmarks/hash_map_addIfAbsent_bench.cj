/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
 * This source file is part of the Cangjie project, licensed under Apache-2.0
 * with Runtime Library Exception.
 *
 * See https://cangjie-lang.cn/pages/LICENSE for license information.
 */

// Benchmark for HashMap.addIfAbsent performance
// Issue: https://gitcode.com/Cangjie/cangjie_runtime/issues/583
//
// 编译: cjc benchmarks/hash_map_addIfAbsent_bench.cj -o benchmarks/hash_map_addIfAbsent_bench
// 运行: ./benchmarks/hash_map_addIfAbsent_bench
//
// 社区学习赛 - 任务#7

import std.collection.*
import std.random.*
import std.time.*
import std.console.*

main(): Int64 {
    let DATA_SIZE = 256 * 1024
    let RND = Random(42)
    let STR_MIN_SIZE = 2
    let STR_MAX_SIZE = 20

    // 生成随机字符串数组
    let randomStrings = Array<String>(DATA_SIZE) {_ =>
        let size = RND.nextInt64(STR_MAX_SIZE - STR_MIN_SIZE)
        var text = Array<Rune>(STR_MIN_SIZE + size, repeat: Rune(0))
        for (i in 0..(STR_MIN_SIZE + size)) {
            text[i] = Rune(RND.nextInt64(26) + 97)
        }
        String(text)
    }

    println("========================================")
    println("HashMap.addIfAbsent 性能基准测试")
    println("数据量: ${DATA_SIZE} 条随机字符串")
    println("========================================")
    println()

    let RUN_COUNT = 5

    // 测试 1: addIfAbsent (优化后)
    var totalAddIfAbsent = Int64(0)
    for (run in 0..RUN_COUNT) {
        let start = DateTime.now()
        var count = 0
        let data = HashMap<String, Int64>()
        for (str in randomStrings) {
            data.addIfAbsent(str, Int64(count))
            count++
        }
        let duration = DateTime.now() - start
        totalAddIfAbsent += duration.toMilliseconds()
        println("[addIfAbsent] 第${run + 1}次: ${duration.toMilliseconds()} ms (size: ${data.size})")
    }
    let avgAddIfAbsent = totalAddIfAbsent / RUN_COUNT

    println()

    // 测试 2: contains + add (参照实现)
    var totalContainsAdd = Int64(0)
    for (run in 0..RUN_COUNT) {
        let start = DateTime.now()
        var count = 0
        let data = HashMap<String, Int64>()
        for (str in randomStrings) {
            if (!data.contains(str)) {
                data.add(str, Int64(count))
                count++
            }
        }
        let duration = DateTime.now() - start
        totalContainsAdd += duration.toMilliseconds()
        println("[contains+add] 第${run + 1}次: ${duration.toMilliseconds()} ms (size: ${data.size})")
    }
    let avgContainsAdd = totalContainsAdd / RUN_COUNT

    println()
    println("========================================")
    println("结果汇总 (${RUN_COUNT} 次平均)")
    println("========================================")
    println("addIfAbsent (优化后):  ${avgAddIfAbsent} ms")
    println("contains + add (参照): ${avgContainsAdd} ms")

    if (avgContainsAdd > 0) {
        let ratio = Float64(avgAddIfAbsent) / Float64(avgContainsAdd)
        println("比值: addIfAbsent / contains+add = ${ratio}")
        println()
        if (ratio <= 1.0) {
            println(">>> 优化成功: addIfAbsent 性能与 contains+add 持平或更优")
        } else if (ratio <= 1.2) {
            println(">>> 优化基本成功: addIfAbsent 性能接近 contains+add")
        } else {
            println(">>> 仍有性能差距: addIfAbsent 比 contains+add 慢")
        }
    }

    println()
    println("Issue #583 优化前的基准数据 (供参考):")
    println("  addIfAbsent (优化前): 392.4 ms")
    println("  contains + add:         87.03 ms")
    println("  优化前比值: 4.5x")
    println()
    if (avgContainsAdd > 0) {
        let oldMs = Float64(392)
        let newMs = Float64(avgAddIfAbsent)
        let improvement = (oldMs - newMs) / oldMs * 100.0
        println("性能提升: ${(oldMs - newMs)} ms -> ${improvement}%")
        if (improvement >= 20.0) {
            println(">>> 满足验收标准: 性能提升 >= 20%")
        }
    }

    return 0
}
