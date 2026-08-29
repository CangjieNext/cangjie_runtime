/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
 * This source file is part of the Cangjie project, licensed under Apache-2.0
 * with Runtime Library Exception.
 * 
 * See https://cangjie-lang.cn/pages/LICENSE for license information.
 */

// LEVEL: 2
// EXEC: %compiler %cmp_opt %f -o %output
// RUN-EXEC: %export cjHeapSize=256MB && %run %run_opt %output %run_args 2>&1 | compare %f
// (not iOS) ASSERT: scan Out of memory
// (iOS) ASSERT: scan OutOfMemoryError

import std.collection.*

class A {}

main() {
    var list = ArrayList<A>()
    try {
        while (true) {
            list.add(A())
        }
    } catch (e: Exception) {
        e.printStackTrace()
    }
    return 0
}
