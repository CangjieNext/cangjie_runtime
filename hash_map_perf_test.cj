/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
 * This source file is part of the Cangjie project, licensed under Apache-2.0
 * with Runtime Library Exception.
 *
 * See https://cangjie-lang.cn/pages/LICENSE for license information.
 */

// HashMap.addIfAbsent 性能对比测试
// 编译: cjc hash_map_perf_test.cj -o hash_map_perf_test
// 运行: ./hash_map_perf_test
// 社区学习赛 - 任务#7

import std.collection.*
import std.random.*
import std.time.*
import std.io.*

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
            text[i] = Rune(RND.nextInt64(26) + 97) // a-z
        }
        String(text)
    }

    println("数据量: ${DATA_SIZE} 条随机字符串")
    println("========================================")

    // 测试 1: addIfAbsent
    let start1 = DateTime.now()
    var count1 = 0
    let data1 = HashMap<String, Int64>()
    for (str in randomStrings) {
        data1.addIfAbsent(str, Int64(count1))
        count1++
    }
    let end1 = DateTime.now()
    let duration1 = end1 - start1
    println("addIfAbsent:       ${duration1.toMilliseconds()} ms (map size: ${data1.size})")

    // 测试 2: contains + add
    let start2 = DateTime.now()
    var count2 = 0
    let data2 = HashMap<String, Int64>()
    for (str in randomStrings) {
        if (!data2.contains(str)) {
            data2.add(str, Int64(count2))
            count2++
        }
    }
    let end2 = DateTime.now()
    let duration2 = end2 - start2
    println("contains + add:    ${duration2.toMilliseconds()} ms (map size: ${data2.size})")

    println("========================================")
    let ratio = Float64(duration1.toMilliseconds()) / Float64(duration2.toMilliseconds())
    println("性能差距: addIfAbsent 是 contains+add 的 ${ratio} 倍")

    if (ratio > 1.2) {
        println(">>> 确认性能问题存在: addIfAbsent 显著慢于 contains+add")
    } else {
        println(">>> 性能正常: addIfAbsent 与 contains+add 性能接近")
    }

    return 0
}
