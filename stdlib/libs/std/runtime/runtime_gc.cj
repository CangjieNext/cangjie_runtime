/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
 * This source file is part of the Cangjie project, licensed under Apache-2.0
 * with Runtime Library Exception.
 *
 * See https://cangjie-lang.cn/pages/LICENSE for license information.
 */

package std.runtime

let MIN_GC_THRESHOLD: UInt64 = 1024 * 1024
let MAX_GC_THRESHOLD: UInt64 = 1024 * 1024 * 1024 * 1024

@Intrinsic
func invokeGC(heavy: Bool): Unit

@When[backend == "cjnative"]
@Intrinsic
func getCjGCCount(): UIntNative

@When[backend == "cjnative"]
@Intrinsic
func getCjGCTime(): Int64

@When[backend == "cjnative"]
@Intrinsic
func getCjGCFreedSize(): UIntNative

@When[backend == "cjnative"]
@Intrinsic
func setCjGCThreshold(value: UInt64): Unit

@When[backend == "cjnative"]
foreign func CJ_MCC_IsGCRunning(): Bool

@Deprecated[message: "Use 'public func gc(heavy!: Bool = false): Unit' instead."]
public func GC(heavy!: Bool = false): Unit {
    return gc(heavy: heavy)
}

/*
 * Set GCThreshold that user expected for GC as reference threshold.
 */
@When[backend == "cjnative"]
@Deprecated[message: "Use 'public func setGCThreshold(value: UInt64): Unit' instead."]
public func SetGCThreshold(value: UInt64): Unit {
    return setGCThreshold(value)
}

public func gc(heavy!: Bool = false): Unit {
    unsafe { return invokeGC(heavy) }
}

@When[backend == "cjnative"]
public func setGCThreshold(value: UInt64): Unit {
    let clampedValue = if (value < MIN_GC_THRESHOLD) {
        MIN_GC_THRESHOLD
    } else if (value > MAX_GC_THRESHOLD) {
        MAX_GC_THRESHOLD
    } else {
        value
    }
    unsafe { setCjGCThreshold(clampedValue) }
}

@When[backend == "cjnative"]
public func getGCCount(): Int64 {
    return Int64(getCjGCCount())
}

@When[backend == "cjnative"]
public func getGCTime(): Int64 {
    return getCjGCTime()
}

@When[backend == "cjnative"]
public func getGCFreedSize(): Int64 {
    return Int64(getCjGCFreedSize())
}

@When[backend == "cjnative"]
public func isGCRunning(): Bool {
    unsafe { CJ_MCC_IsGCRunning() }
}
