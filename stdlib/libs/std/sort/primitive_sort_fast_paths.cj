/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
 * This source file is part of the Cangjie project, licensed under Apache-2.0
 * with Runtime Library Exception.
 *
 * See https://cangjie-lang.cn/pages/LICENSE for license information.
 */

package std.sort

// Float must be dispatched BEFORE the isAscending/isDescending short-circuit
// checks in `unstableSort`: NaN compares false against everything, so those
// checks may wrongly treat a NaN-carrying array as already sorted and skip
// the sort entirely.
func tryPrimitiveFloatUnstableSort<T>(data: Array<T>): Bool where T <: Comparable<T> {
    if (let Some(concreteData) <- (data as Array<Float16>)) {
        primitiveUnstableSortFloat16(concreteData)
        return true
    }
    if (let Some(concreteData) <- (data as Array<Float32>)) {
        primitiveUnstableSortFloat32(concreteData)
        return true
    }
    if (let Some(concreteData) <- (data as Array<Float64>)) {
        primitiveUnstableSortFloat64(concreteData)
        return true
    }
    return false
}

func tryPrimitiveUnstableSort<T>(data: Array<T>): Bool where T <: Comparable<T> {
    if (let Some(concreteData) <- (data as Array<Int8>)) {
        primitiveUnstableSortInt8(concreteData)
        return true
    }
    if (let Some(concreteData) <- (data as Array<Int16>)) {
        primitiveUnstableSortInt16(concreteData)
        return true
    }
    if (let Some(concreteData) <- (data as Array<Int32>)) {
        primitiveUnstableSortInt32(concreteData)
        return true
    }
    if (let Some(concreteData) <- (data as Array<Int64>)) {
        primitiveUnstableSortInt64(concreteData)
        return true
    }
    if (let Some(concreteData) <- (data as Array<IntNative>)) {
        primitiveUnstableSortIntNative(concreteData)
        return true
    }
    if (let Some(concreteData) <- (data as Array<UInt8>)) {
        primitiveUnstableSortUInt8(concreteData)
        return true
    }
    if (let Some(concreteData) <- (data as Array<UInt16>)) {
        primitiveUnstableSortUInt16(concreteData)
        return true
    }
    if (let Some(concreteData) <- (data as Array<UInt32>)) {
        primitiveUnstableSortUInt32(concreteData)
        return true
    }
    if (let Some(concreteData) <- (data as Array<UInt64>)) {
        primitiveUnstableSortUInt64(concreteData)
        return true
    }
    if (let Some(concreteData) <- (data as Array<UIntNative>)) {
        primitiveUnstableSortUIntNative(concreteData)
        return true
    }
    if (let Some(concreteData) <- (data as Array<Rune>)) {
        primitiveUnstableSortRune(concreteData)
        return true
    }
    return false
}

func tryPrimitiveStableSort<T>(data: Array<T>): Bool where T <: Comparable<T> {
    if (let Some(concreteData) <- (data as Array<Int8>)) {
        primitiveStableSortInt8(concreteData)
        return true
    }
    if (let Some(concreteData) <- (data as Array<Int16>)) {
        primitiveStableSortInt16(concreteData)
        return true
    }
    if (let Some(concreteData) <- (data as Array<Int32>)) {
        primitiveStableSortInt32(concreteData)
        return true
    }
    if (let Some(concreteData) <- (data as Array<Int64>)) {
        primitiveStableSortInt64(concreteData)
        return true
    }
    if (let Some(concreteData) <- (data as Array<IntNative>)) {
        primitiveStableSortIntNative(concreteData)
        return true
    }
    if (let Some(concreteData) <- (data as Array<UInt8>)) {
        primitiveStableSortUInt8(concreteData)
        return true
    }
    if (let Some(concreteData) <- (data as Array<UInt16>)) {
        primitiveStableSortUInt16(concreteData)
        return true
    }
    if (let Some(concreteData) <- (data as Array<UInt32>)) {
        primitiveStableSortUInt32(concreteData)
        return true
    }
    if (let Some(concreteData) <- (data as Array<UInt64>)) {
        primitiveStableSortUInt64(concreteData)
        return true
    }
    if (let Some(concreteData) <- (data as Array<UIntNative>)) {
        primitiveStableSortUIntNative(concreteData)
        return true
    }
    if (let Some(concreteData) <- (data as Array<Float16>)) {
        primitiveStableSortFloat16(concreteData)
        return true
    }
    if (let Some(concreteData) <- (data as Array<Float32>)) {
        primitiveStableSortFloat32(concreteData)
        return true
    }
    if (let Some(concreteData) <- (data as Array<Float64>)) {
        primitiveStableSortFloat64(concreteData)
        return true
    }
    if (let Some(concreteData) <- (data as Array<Rune>)) {
        primitiveStableSortRune(concreteData)
        return true
    }
    return false
}

private func primitiveUnstableSortInt8(data: Array<Int8>): Unit {
    data.introSort(0, data.size - 1, data.depth())
}

private func primitiveStableSortInt8(data: Array<Int8>): Unit {
    data.timSort()
}

private func primitiveUnstableSortInt16(data: Array<Int16>): Unit {
    data.introSort(0, data.size - 1, data.depth())
}

private func primitiveStableSortInt16(data: Array<Int16>): Unit {
    data.timSort()
}

private func primitiveUnstableSortInt32(data: Array<Int32>): Unit {
    data.introSort(0, data.size - 1, data.depth())
}

private func primitiveStableSortInt32(data: Array<Int32>): Unit {
    data.timSort()
}

private func primitiveUnstableSortInt64(data: Array<Int64>): Unit {
    data.introSort(0, data.size - 1, data.depth())
}

private func primitiveStableSortInt64(data: Array<Int64>): Unit {
    data.timSort()
}

private func primitiveUnstableSortIntNative(data: Array<IntNative>): Unit {
    data.introSort(0, data.size - 1, data.depth())
}

private func primitiveStableSortIntNative(data: Array<IntNative>): Unit {
    data.timSort()
}

private func primitiveUnstableSortUInt8(data: Array<UInt8>): Unit {
    data.introSort(0, data.size - 1, data.depth())
}

private func primitiveStableSortUInt8(data: Array<UInt8>): Unit {
    data.timSort()
}

private func primitiveUnstableSortUInt16(data: Array<UInt16>): Unit {
    data.introSort(0, data.size - 1, data.depth())
}

private func primitiveStableSortUInt16(data: Array<UInt16>): Unit {
    data.timSort()
}

private func primitiveUnstableSortUInt32(data: Array<UInt32>): Unit {
    data.introSort(0, data.size - 1, data.depth())
}

private func primitiveStableSortUInt32(data: Array<UInt32>): Unit {
    data.timSort()
}

private func primitiveUnstableSortUInt64(data: Array<UInt64>): Unit {
    data.introSort(0, data.size - 1, data.depth())
}

private func primitiveStableSortUInt64(data: Array<UInt64>): Unit {
    data.timSort()
}

private func primitiveUnstableSortUIntNative(data: Array<UIntNative>): Unit {
    data.introSort(0, data.size - 1, data.depth())
}

private func primitiveStableSortUIntNative(data: Array<UIntNative>): Unit {
    data.timSort()
}

// NaN is incomparable with any value under IEEE-754 (every comparison returns
// false), so letting NaN take part in the partition/merge loops may stall or
// crash the sort. For Float arrays the fast paths first move all NaN to the
// tail and sort only the non-NaN prefix, which makes the result deterministic:
// non-NaN values sorted ascending, all NaN at the end (like Java's
// Arrays.sort). The unstable variants partition in place; the stable variants
// preserve the relative order of both the non-NaN values and the NaN values.

private func primitiveUnstableSortFloat16(data: Array<Float16>): Unit {
    var left = 0
    var right = data.size
    while (left < right) {
        if (data[left] != data[left]) {
            right--
            let t = data[left]
            data[left] = data[right]
            data[right] = t
        } else {
            left++
        }
    }
    if (left <= 1) {
        return
    }
    // Short-circuit on the non-NaN prefix (it is NaN-free, so comparisons are exact)
    var i = 0
    while (i < left - 1 && !(data[i] > data[i + 1])) {
        i++
    }
    if (i == left - 1) {
        return
    }
    i = 0
    while (i < left - 1 && !(data[i] < data[i + 1])) {
        i++
    }
    if (i == left - 1) {
        data.reverseRange(0, left - 1)
        return
    }
    data.introSort(0, left - 1, data.depth())
}

private func primitiveStableSortFloat16(data: Array<Float16>): Unit {
    var nonNaNCount = 0
    for (i in 0..data.size) {
        if (!(data[i] != data[i])) {
            nonNaNCount++
        }
    }
    if (nonNaNCount == data.size) {
        data.timSort()
        return
    }
    let nanCount = data.size - nonNaNCount
    let sorted = Array<Float16>(nonNaNCount, repeat: 0.0)
    let nanTail = Array<Float16>(nanCount, repeat: 0.0)
    var i = 0
    var j = 0
    for (k in 0..data.size) {
        let x = data[k]
        if (x != x) {
            nanTail[j] = x
            j++
        } else {
            sorted[i] = x
            i++
        }
    }
    sorted.timSort()
    for (k in 0..nonNaNCount) {
        data[k] = sorted[k]
    }
    for (k in 0..nanCount) {
        data[nonNaNCount + k] = nanTail[k]
    }
}

private func primitiveUnstableSortFloat32(data: Array<Float32>): Unit {
    var left = 0
    var right = data.size
    while (left < right) {
        if (data[left] != data[left]) {
            right--
            let t = data[left]
            data[left] = data[right]
            data[right] = t
        } else {
            left++
        }
    }
    if (left <= 1) {
        return
    }
    // Short-circuit on the non-NaN prefix (it is NaN-free, so comparisons are exact)
    var i = 0
    while (i < left - 1 && !(data[i] > data[i + 1])) {
        i++
    }
    if (i == left - 1) {
        return
    }
    i = 0
    while (i < left - 1 && !(data[i] < data[i + 1])) {
        i++
    }
    if (i == left - 1) {
        data.reverseRange(0, left - 1)
        return
    }
    data.introSort(0, left - 1, data.depth())
}

private func primitiveStableSortFloat32(data: Array<Float32>): Unit {
    var nonNaNCount = 0
    for (i in 0..data.size) {
        if (!(data[i] != data[i])) {
            nonNaNCount++
        }
    }
    if (nonNaNCount == data.size) {
        data.timSort()
        return
    }
    let nanCount = data.size - nonNaNCount
    let sorted = Array<Float32>(nonNaNCount, repeat: 0.0)
    let nanTail = Array<Float32>(nanCount, repeat: 0.0)
    var i = 0
    var j = 0
    for (k in 0..data.size) {
        let x = data[k]
        if (x != x) {
            nanTail[j] = x
            j++
        } else {
            sorted[i] = x
            i++
        }
    }
    sorted.timSort()
    for (k in 0..nonNaNCount) {
        data[k] = sorted[k]
    }
    for (k in 0..nanCount) {
        data[nonNaNCount + k] = nanTail[k]
    }
}

private func primitiveUnstableSortFloat64(data: Array<Float64>): Unit {
    var left = 0
    var right = data.size
    while (left < right) {
        if (data[left] != data[left]) {
            right--
            let t = data[left]
            data[left] = data[right]
            data[right] = t
        } else {
            left++
        }
    }
    if (left <= 1) {
        return
    }
    // Short-circuit on the non-NaN prefix (it is NaN-free, so comparisons are exact)
    var i = 0
    while (i < left - 1 && !(data[i] > data[i + 1])) {
        i++
    }
    if (i == left - 1) {
        return
    }
    i = 0
    while (i < left - 1 && !(data[i] < data[i + 1])) {
        i++
    }
    if (i == left - 1) {
        data.reverseRange(0, left - 1)
        return
    }
    data.introSort(0, left - 1, data.depth())
}

private func primitiveStableSortFloat64(data: Array<Float64>): Unit {
    var nonNaNCount = 0
    for (i in 0..data.size) {
        if (!(data[i] != data[i])) {
            nonNaNCount++
        }
    }
    if (nonNaNCount == data.size) {
        data.timSort()
        return
    }
    let nanCount = data.size - nonNaNCount
    let sorted = Array<Float64>(nonNaNCount, repeat: 0.0)
    let nanTail = Array<Float64>(nanCount, repeat: 0.0)
    var i = 0
    var j = 0
    for (k in 0..data.size) {
        let x = data[k]
        if (x != x) {
            nanTail[j] = x
            j++
        } else {
            sorted[i] = x
            i++
        }
    }
    sorted.timSort()
    for (k in 0..nonNaNCount) {
        data[k] = sorted[k]
    }
    for (k in 0..nanCount) {
        data[nonNaNCount + k] = nanTail[k]
    }
}

// The descending dispatchers mirror the ascending ones above: the descending
// Comparable entry (`sort(..., descending: true)`) builds `{l, r => r.compare(l)}`
// internally and routes through the comparator overloads, whose
// isAscending/isDescending short-circuit checks are fooled by NaN in the same
// way as the ascending ones, so Float arrays must also be intercepted before
// those checks. Under descending order NaN is still treated as the largest
// value, hence all NaN are moved to the head of the array and only the
// non-NaN suffix is sorted.

func tryPrimitiveFloatUnstableSortDescending<T>(data: Array<T>): Bool where T <: Comparable<T> {
    if (data.size < 2) {
        return true
    }
    if (let Some(concreteData) <- (data as Array<Float16>)) {
        primitiveUnstableSortFloat16Descending(concreteData)
        return true
    }
    if (let Some(concreteData) <- (data as Array<Float32>)) {
        primitiveUnstableSortFloat32Descending(concreteData)
        return true
    }
    if (let Some(concreteData) <- (data as Array<Float64>)) {
        primitiveUnstableSortFloat64Descending(concreteData)
        return true
    }
    return false
}

func tryPrimitiveFloatStableSortDescending<T>(data: Array<T>): Bool where T <: Comparable<T> {
    if (data.size < 2) {
        return true
    }
    if (let Some(concreteData) <- (data as Array<Float16>)) {
        primitiveStableSortFloat16Descending(concreteData)
        return true
    }
    if (let Some(concreteData) <- (data as Array<Float32>)) {
        primitiveStableSortFloat32Descending(concreteData)
        return true
    }
    if (let Some(concreteData) <- (data as Array<Float64>)) {
        primitiveStableSortFloat64Descending(concreteData)
        return true
    }
    return false
}

// The unstable variants reuse the ascending NaN-separating sort and reverse
// the whole array afterwards: NaN moves from the tail to the head and the
// non-NaN prefix turns descending. Unstable sort keeps no relative-order
// promise, so the extra reversal is invisible.

private func primitiveUnstableSortFloat16Descending(data: Array<Float16>): Unit {
    primitiveUnstableSortFloat16(data)
    data.reverseRange(0, data.size - 1)
}

private func primitiveUnstableSortFloat32Descending(data: Array<Float32>): Unit {
    primitiveUnstableSortFloat32(data)
    data.reverseRange(0, data.size - 1)
}

private func primitiveUnstableSortFloat64Descending(data: Array<Float64>): Unit {
    primitiveUnstableSortFloat64(data)
    data.reverseRange(0, data.size - 1)
}

// The stable variants cannot reverse a stably sorted array (that would
// reverse the relative order of equal elements), so they mirror the ascending
// stable paths: non-NaN values are sorted descending with the reversed
// comparator (still stable for equal keys) and placed after all NaN values,
// which keep their original relative order.

private func primitiveStableSortFloat16Descending(data: Array<Float16>): Unit {
    var nonNaNCount = 0
    for (i in 0..data.size) {
        if (!(data[i] != data[i])) {
            nonNaNCount++
        }
    }
    if (nonNaNCount == data.size) {
        data.timSort({l: Float16, r: Float16 => r.compare(l)})
        return
    }
    let nanCount = data.size - nonNaNCount
    let sorted = Array<Float16>(nonNaNCount, repeat: 0.0)
    let nanHead = Array<Float16>(nanCount, repeat: 0.0)
    var i = 0
    var j = 0
    for (k in 0..data.size) {
        let x = data[k]
        if (x != x) {
            nanHead[j] = x
            j++
        } else {
            sorted[i] = x
            i++
        }
    }
    sorted.timSort({l: Float16, r: Float16 => r.compare(l)})
    for (k in 0..nanCount) {
        data[k] = nanHead[k]
    }
    for (k in 0..nonNaNCount) {
        data[nanCount + k] = sorted[k]
    }
}

private func primitiveStableSortFloat32Descending(data: Array<Float32>): Unit {
    var nonNaNCount = 0
    for (i in 0..data.size) {
        if (!(data[i] != data[i])) {
            nonNaNCount++
        }
    }
    if (nonNaNCount == data.size) {
        data.timSort({l: Float32, r: Float32 => r.compare(l)})
        return
    }
    let nanCount = data.size - nonNaNCount
    let sorted = Array<Float32>(nonNaNCount, repeat: 0.0)
    let nanHead = Array<Float32>(nanCount, repeat: 0.0)
    var i = 0
    var j = 0
    for (k in 0..data.size) {
        let x = data[k]
        if (x != x) {
            nanHead[j] = x
            j++
        } else {
            sorted[i] = x
            i++
        }
    }
    sorted.timSort({l: Float32, r: Float32 => r.compare(l)})
    for (k in 0..nanCount) {
        data[k] = nanHead[k]
    }
    for (k in 0..nonNaNCount) {
        data[nanCount + k] = sorted[k]
    }
}

private func primitiveStableSortFloat64Descending(data: Array<Float64>): Unit {
    var nonNaNCount = 0
    for (i in 0..data.size) {
        if (!(data[i] != data[i])) {
            nonNaNCount++
        }
    }
    if (nonNaNCount == data.size) {
        data.timSort({l: Float64, r: Float64 => r.compare(l)})
        return
    }
    let nanCount = data.size - nonNaNCount
    let sorted = Array<Float64>(nonNaNCount, repeat: 0.0)
    let nanHead = Array<Float64>(nanCount, repeat: 0.0)
    var i = 0
    var j = 0
    for (k in 0..data.size) {
        let x = data[k]
        if (x != x) {
            nanHead[j] = x
            j++
        } else {
            sorted[i] = x
            i++
        }
    }
    sorted.timSort({l: Float64, r: Float64 => r.compare(l)})
    for (k in 0..nanCount) {
        data[k] = nanHead[k]
    }
    for (k in 0..nonNaNCount) {
        data[nanCount + k] = sorted[k]
    }
}

private func primitiveUnstableSortRune(data: Array<Rune>): Unit {
    data.introSort(0, data.size - 1, data.depth())
}

private func primitiveStableSortRune(data: Array<Rune>): Unit {
    data.timSort()
}
