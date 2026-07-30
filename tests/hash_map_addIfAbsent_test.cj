/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
 * This source file is part of the Cangjie project, licensed under Apache-2.0
 * with Runtime Library Exception.
 *
 * See https://cangjie-lang.cn/pages/LICENSE for license information.
 */

// Unit tests for HashMap.addIfAbsent
// 社区学习赛 - 任务#7
//
// 编译: cjc tests/hash_map_addIfAbsent_test.cj -o tests/hash_map_addIfAbsent_test
// 运行: ./tests/hash_map_addIfAbsent_test

import std.collection.*
import std.console.*

main(): Int64 {
    var passed = 0
    var failed = 0

    func assert(condition: Bool, msg: String) {
        if (condition) {
            passed++
            println("[PASS] ${msg}")
        } else {
            failed++
            println("[FAIL] ${msg}")
        }
    }

    // 测试 1: key 不存在时插入成功，返回 None
    {
        let map = HashMap<String, Int64>()
        let result = map.addIfAbsent("key1", 100)
        assert(result is None, "key不存在时返回None")
        assert(map.size == 1, "key不存在时size为1")
        assert(map.get("key1") == Some(100), "key不存在时插入正确的值")
    }

    // 测试 2: key 已存在时不覆盖，返回已有值
    {
        let map = HashMap<String, Int64>()
        map.add("key1", 100)
        let result = map.addIfAbsent("key1", 200)
        assert(result == Some(100), "key已存在时返回已有值")
        assert(map.get("key1") == Some(100), "key已存在时不覆盖原值")
        assert(map.size == 1, "key已存在时size不变")
    }

    // 测试 3: 多次 addIfAbsent 同一 key
    {
        let map = HashMap<String, Int64>()
        let r1 = map.addIfAbsent("key1", 10)
        let r2 = map.addIfAbsent("key1", 20)
        let r3 = map.addIfAbsent("key1", 30)
        assert(r1 is None, "第一次addIfAbsent返回None")
        assert(r2 == Some(10), "第二次addIfAbsent返回原值")
        assert(r3 == Some(10), "第三次addIfAbsent返回原值")
        assert(map.get("key1") == Some(10), "多次addIfAbsent后值不变")
    }

    // 测试 4: 不同 key 都能正确插入
    {
        let map = HashMap<String, Int64>()
        for (i in 0..100) {
            let key = "key${i}"
            let result = map.addIfAbsent(key, Int64(i))
            assert(result is None, "插入key${i}返回None")
        }
        assert(map.size == 100, "插入100个不同key后size为100")
        for (i in 0..100) {
            let key = "key${i}"
            assert(map.get(key) == Some(Int64(i)), "key${i}的值正确")
        }
    }

    // 测试 5: 触发扩容（大量数据）
    {
        let map = HashMap<String, Int64>()
        let DATA_SIZE = 10000
        for (i in 0..DATA_SIZE) {
            map.addIfAbsent("key_${i}", Int64(i))
        }
        assert(map.size == Int64(DATA_SIZE), "扩容后size正确")
        // 验证数据完整性
        var allCorrect = true
        for (i in 0..DATA_SIZE) {
            if (map.get("key_${i}") != Some(Int64(i))) {
                allCorrect = false
                break
            }
        }
        assert(allCorrect, "扩容后数据完整性验证通过")
    }

    // 测试 6: 扩容后再次 addIfAbsent 已有 key
    {
        let map = HashMap<String, Int64>()
        let DATA_SIZE = 10000
        for (i in 0..DATA_SIZE) {
            map.addIfAbsent("key_${i}", Int64(i))
        }
        // 扩容后再对已有 key 调用 addIfAbsent
        for (i in 0..100) {
            let result = map.addIfAbsent("key_${i}", Int64(i + 10000))
            assert(result == Some(Int64(i)), "扩容后key_${i}返回原值")
        }
        assert(map.size == Int64(DATA_SIZE), "扩容后addIfAbsent不增加size")
    }

    // 测试 7: 空字符串 key
    {
        let map = HashMap<String, Int64>()
        let r1 = map.addIfAbsent("", 1)
        let r2 = map.addIfAbsent("", 2)
        assert(r1 is None, "空字符串key首次插入返回None")
        assert(r2 == Some(1), "空字符串key第二次返回原值")
        assert(map.get("") == Some(1), "空字符串key值正确")
    }

    // 测试 8: addIfAbsent 与 contains 结果一致
    {
        let map = HashMap<String, Int64>()
        map.addIfAbsent("key1", 1)
        assert(map.contains("key1"), "addIfAbsent后contains返回true")
        assert(!map.contains("key2"), "未插入的key contains返回false")
    }

    // 测试 9: 重复 key 的 addIfAbsent 与 add 行为对比
    {
        let map1 = HashMap<String, Int64>()
        let map2 = HashMap<String, Int64>()
        map1.add("key1", 100)
        map2.addIfAbsent("key1", 100)

        // add 会覆盖，addIfAbsent 不会覆盖
        let r1 = map1.add("key1", 200)  // 返回旧值
        let r2 = map2.addIfAbsent("key1", 200)  // 也返回旧值，但不覆盖
        assert(r1 == Some(100), "add返回旧值")
        assert(r2 == Some(100), "addIfAbsent返回旧值")
        assert(map1.get("key1") == Some(200), "add覆盖了值")
        assert(map2.get("key1") == Some(100), "addIfAbsent没有覆盖值")
    }

    // 测试 10: 大量重复 key addIfAbsent
    {
        let map = HashMap<String, Int64>()
        map.addIfAbsent("dup", 42)
        for (i in 0..1000) {
            let result = map.addIfAbsent("dup", Int64(i))
            assert(result == Some(42), "第${i}次重复addIfAbsent返回原值")
        }
        assert(map.size == 1, "大量重复addIfAbsent后size仍为1")
        assert(map.get("dup") == Some(42), "大量重复addIfAbsent后值不变")
    }

    println()
    println("========================================")
    println("测试结果: ${passed} 通过, ${failed} 失败")
    println("========================================")

    return if (failed > 0) { 1 } else { 0 }
}
