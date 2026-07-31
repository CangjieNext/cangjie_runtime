/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
 * This source file is part of the Cangjie project, licensed under Apache-2.0
 * with Runtime Library Exception.
 *
 * See https://cangjie-lang.cn/pages/LICENSE for license information.
 */

package string_isblank_empty

import std.unicode.*
import std.unittest.*
import std.unittest.testmacro.*

@Test
@Configure[warmup: Duration.second * 2, minDuration: Duration.second * 10]
class StringIsBlankEmptyBenchmark {
    private static const DATA_SIZE = 256 * 1024
    private static let EMPTY_STRINGS = Array<String>(DATA_SIZE) { _ => "" }

    private func isBlankBeforeOptimization(str: String): Bool {
        for (rune in str.runes()) {
            if (!rune.isWhiteSpace()) {
                return false
            }
        }
        return true
    }

    @Bench
    func checkBaselineIsBlank(): Int64 {
        var count: Int64 = 0
        for (str in EMPTY_STRINGS) {
            if (isBlankBeforeOptimization(str)) {
                count++
            }
        }
        return count
    }

    @Bench
    func checkOptimizedIsBlank(): Int64 {
        var count: Int64 = 0
        for (str in EMPTY_STRINGS) {
            if (str.isBlank()) {
                count++
            }
        }
        return count
    }

    @Bench
    func checkReferenceEmptyOrBlank(): Int64 {
        var count: Int64 = 0
        for (str in EMPTY_STRINGS) {
            if (str.isEmpty() || str.isBlank()) {
                count++
            }
        }
        return count
    }
}

main() {}
