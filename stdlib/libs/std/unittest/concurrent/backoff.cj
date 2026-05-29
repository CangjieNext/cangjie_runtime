/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
 * This source file is part of the Cangjie project, licensed under Apache-2.0
 * with Runtime Library Exception.
 *
 * See https://cangjie-lang.cn/pages/LICENSE for license information.
 */

package std.unittest.concurrent

import std.random.Random
import std.math.abs

public class BackoffGenerator {
    public BackoffGenerator(
        public var random!: Random = Random(),
        public var sigma!: Float64 = 1.0,
        public let maxBackoff!: Int64 = 10000
    ) {}
    
    public func next(): Int64 {
        let u = random.nextGaussianFloat64(sigma: sigma)
        let value = Int64(abs(u))
        if (value > maxBackoff) {
            return maxBackoff
        }
        value
    }
    
    public func nextBounded(min: Int64, max: Int64): Int64 {
        let raw = next()
        min + (raw % (max - min + 1))
    }
    
    public func reset(seed: Int64): Unit {
        random = Random(UInt64(seed))
    }
}