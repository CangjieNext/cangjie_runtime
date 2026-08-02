/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
 * This source file is part of the Cangjie project, licensed under Apache-2.0
 * with Runtime Library Exception.
 *
 * See https://cangjie-lang.cn/pages/LICENSE for license information.
 */

package std.sort

func tryPrimitiveUnstableSort<T>(data: Array<T>): Bool where T <: Comparable<T> {
    if (data is Array<Int8>) {
        let concreteData = (data as Array<Int8>) ?? return false
        primitiveUnstableSortInt8(concreteData)
        return true
    }
    if (data is Array<Int16>) {
        let concreteData = (data as Array<Int16>) ?? return false
        primitiveUnstableSortInt16(concreteData)
        return true
    }
    if (data is Array<Int32>) {
        let concreteData = (data as Array<Int32>) ?? return false
        primitiveUnstableSortInt32(concreteData)
        return true
    }
    if (data is Array<Int64>) {
        let concreteData = (data as Array<Int64>) ?? return false
        primitiveUnstableSortInt64(concreteData)
        return true
    }
    if (data is Array<IntNative>) {
        let concreteData = (data as Array<IntNative>) ?? return false
        primitiveUnstableSortIntNative(concreteData)
        return true
    }
    if (data is Array<UInt8>) {
        let concreteData = (data as Array<UInt8>) ?? return false
        primitiveUnstableSortUInt8(concreteData)
        return true
    }
    if (data is Array<UInt16>) {
        let concreteData = (data as Array<UInt16>) ?? return false
        primitiveUnstableSortUInt16(concreteData)
        return true
    }
    if (data is Array<UInt32>) {
        let concreteData = (data as Array<UInt32>) ?? return false
        primitiveUnstableSortUInt32(concreteData)
        return true
    }
    if (data is Array<UInt64>) {
        let concreteData = (data as Array<UInt64>) ?? return false
        primitiveUnstableSortUInt64(concreteData)
        return true
    }
    if (data is Array<UIntNative>) {
        let concreteData = (data as Array<UIntNative>) ?? return false
        primitiveUnstableSortUIntNative(concreteData)
        return true
    }
    if (data is Array<Float16>) {
        let concreteData = (data as Array<Float16>) ?? return false
        primitiveUnstableSortFloat16(concreteData)
        return true
    }
    if (data is Array<Float32>) {
        let concreteData = (data as Array<Float32>) ?? return false
        primitiveUnstableSortFloat32(concreteData)
        return true
    }
    if (data is Array<Float64>) {
        let concreteData = (data as Array<Float64>) ?? return false
        primitiveUnstableSortFloat64(concreteData)
        return true
    }
    if (data is Array<Rune>) {
        let concreteData = (data as Array<Rune>) ?? return false
        primitiveUnstableSortRune(concreteData)
        return true
    }
    return false
}

func tryPrimitiveStableSort<T>(data: Array<T>): Bool where T <: Comparable<T> {
    if (data is Array<Int8>) {
        let concreteData = (data as Array<Int8>) ?? return false
        primitiveStableSortInt8(concreteData)
        return true
    }
    if (data is Array<Int16>) {
        let concreteData = (data as Array<Int16>) ?? return false
        primitiveStableSortInt16(concreteData)
        return true
    }
    if (data is Array<Int32>) {
        let concreteData = (data as Array<Int32>) ?? return false
        primitiveStableSortInt32(concreteData)
        return true
    }
    if (data is Array<Int64>) {
        let concreteData = (data as Array<Int64>) ?? return false
        primitiveStableSortInt64(concreteData)
        return true
    }
    if (data is Array<IntNative>) {
        let concreteData = (data as Array<IntNative>) ?? return false
        primitiveStableSortIntNative(concreteData)
        return true
    }
    if (data is Array<UInt8>) {
        let concreteData = (data as Array<UInt8>) ?? return false
        primitiveStableSortUInt8(concreteData)
        return true
    }
    if (data is Array<UInt16>) {
        let concreteData = (data as Array<UInt16>) ?? return false
        primitiveStableSortUInt16(concreteData)
        return true
    }
    if (data is Array<UInt32>) {
        let concreteData = (data as Array<UInt32>) ?? return false
        primitiveStableSortUInt32(concreteData)
        return true
    }
    if (data is Array<UInt64>) {
        let concreteData = (data as Array<UInt64>) ?? return false
        primitiveStableSortUInt64(concreteData)
        return true
    }
    if (data is Array<UIntNative>) {
        let concreteData = (data as Array<UIntNative>) ?? return false
        primitiveStableSortUIntNative(concreteData)
        return true
    }
    if (data is Array<Float16>) {
        let concreteData = (data as Array<Float16>) ?? return false
        primitiveStableSortFloat16(concreteData)
        return true
    }
    if (data is Array<Float32>) {
        let concreteData = (data as Array<Float32>) ?? return false
        primitiveStableSortFloat32(concreteData)
        return true
    }
    if (data is Array<Float64>) {
        let concreteData = (data as Array<Float64>) ?? return false
        primitiveStableSortFloat64(concreteData)
        return true
    }
    if (data is Array<Rune>) {
        let concreteData = (data as Array<Rune>) ?? return false
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

private func primitiveUnstableSortFloat16(data: Array<Float16>): Unit {
    data.introSort(0, data.size - 1, data.depth())
}

private func primitiveStableSortFloat16(data: Array<Float16>): Unit {
    data.timSort()
}

private func primitiveUnstableSortFloat32(data: Array<Float32>): Unit {
    data.introSort(0, data.size - 1, data.depth())
}

private func primitiveStableSortFloat32(data: Array<Float32>): Unit {
    data.timSort()
}

private func primitiveUnstableSortFloat64(data: Array<Float64>): Unit {
    data.introSort(0, data.size - 1, data.depth())
}

private func primitiveStableSortFloat64(data: Array<Float64>): Unit {
    data.timSort()
}

private func primitiveUnstableSortRune(data: Array<Rune>): Unit {
    data.introSort(0, data.size - 1, data.depth())
}

private func primitiveStableSortRune(data: Array<Rune>): Unit {
    data.timSort()
}
