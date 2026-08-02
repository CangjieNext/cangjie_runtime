/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
 * This source file is part of the Cangjie project, licensed under Apache-2.0
 * with Runtime Library Exception.
 *
 * See https://cangjie-lang.cn/pages/LICENSE for license information.
 */

// Full large-input natural-sort matrix for the fourteen primitive types.

import std.random.*
import std.sort.*
import std.time.*

private const WARMUPS: Int64 = 3
private const ROUNDS: Int64 = 10
private const LARGE_SIZE: Int64 = 10_000_000
private const SEED: UInt64 = 2651

private func verifyAscending<T>(data: Array<T>): Unit where T <: Comparable<T> {
    for (i in 1..data.size) {
        if (data[i - 1].compare(data[i]) == Ordering.GT) {
            throw Exception("sort result is not ordered")
        }
    }
}

private func benchmarkNatural<T>(
    typeName: String,
    stable: Bool,
    factory: () -> Array<T>
): Unit where T <: Comparable<T> {
    let mode = if (stable) { "stable" } else { "unstable" }
    for (round in 0..(WARMUPS + ROUNDS)) {
        let data = factory()
        let started = MonoTime.now()
        sort(data, stable: stable)
        let elapsed = MonoTime.now() - started
        verifyAscending(data)
        let kind = if (round < WARMUPS) { "warmup" } else { "measurement" }
        let index = if (round < WARMUPS) { round } else { round - WARMUPS }
        println("${kind},${typeName},${mode},${index},${LARGE_SIZE},${elapsed.toNanoseconds()}")
    }
}

private func benchmarkType<T>(typeName: String, factory: () -> Array<T>): Unit where T <: Comparable<T> {
    benchmarkNatural<T>(typeName, false, factory)
    benchmarkNatural<T>(typeName, true, factory)
}

main(): Int64 {
    println("kind,type,mode,index,size,elapsed_ns")
    benchmarkType<Int8>("Int8", { => let r = Random(SEED); Array<Int8>(LARGE_SIZE, {_ => r.nextInt8()}) })
    benchmarkType<Int16>("Int16", { => let r = Random(SEED); Array<Int16>(LARGE_SIZE, {_ => r.nextInt16()}) })
    benchmarkType<Int32>("Int32", { => let r = Random(SEED); Array<Int32>(LARGE_SIZE, {_ => r.nextInt32()}) })
    benchmarkType<Int64>("Int64", { => let r = Random(SEED); Array<Int64>(LARGE_SIZE, {_ => r.nextInt64()}) })
    benchmarkType<IntNative>("IntNative", { =>
        let r = Random(SEED)
        Array<IntNative>(LARGE_SIZE, {_ => IntNative(r.nextInt64())})
    })
    benchmarkType<UInt8>("UInt8", { => let r = Random(SEED); Array<UInt8>(LARGE_SIZE, {_ => r.nextUInt8()}) })
    benchmarkType<UInt16>("UInt16", { => let r = Random(SEED); Array<UInt16>(LARGE_SIZE, {_ => r.nextUInt16()}) })
    benchmarkType<UInt32>("UInt32", { => let r = Random(SEED); Array<UInt32>(LARGE_SIZE, {_ => r.nextUInt32()}) })
    benchmarkType<UInt64>("UInt64", { => let r = Random(SEED); Array<UInt64>(LARGE_SIZE, {_ => r.nextUInt64()}) })
    benchmarkType<UIntNative>("UIntNative", { =>
        let r = Random(SEED)
        Array<UIntNative>(LARGE_SIZE, {_ => UIntNative(r.nextUInt64())})
    })
    benchmarkType<Float16>("Float16", { => let r = Random(SEED); Array<Float16>(LARGE_SIZE, {_ => r.nextFloat16()}) })
    benchmarkType<Float32>("Float32", { => let r = Random(SEED); Array<Float32>(LARGE_SIZE, {_ => r.nextFloat32()}) })
    benchmarkType<Float64>("Float64", { => let r = Random(SEED); Array<Float64>(LARGE_SIZE, {_ => r.nextFloat64()}) })
    benchmarkType<Rune>("Rune", { =>
        let r = Random(SEED)
        Array<Rune>(LARGE_SIZE, {_ => Rune(UInt32(32) + r.nextUInt32(UInt32(95)))})
    })
    return 0
}
