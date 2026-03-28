/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
 * This source file is part of the Cangjie project, licensed under Apache-2.0
 * with Runtime Library Exception.
 *
 * See https://cangjie-lang.cn/pages/LICENSE for license information.
 */

package std.core

@Intrinsic
func getPointerAddress<T>(c: CPointer<T>): UIntNative where T <: CType

@Intrinsic
func readPointer<T>(c: CPointer<T>, idx: Int64): T where T <: CType

@Intrinsic
func writePointer<T>(c: CPointer<T>, idx: Int64, value: T): Unit where T <: CType

@Intrinsic
func addPointer<T>(c: CPointer<T>, offset: Int64): CPointer<T> where T <: CType

extend<T> CPointer<T> {
    public func isNull(): Bool {
        return getPointerAddress<T>(this) == 0
    }

    public func isNotNull(): Bool {
        return !isNull()
    }

    public func toUIntNative(): UIntNative {
        return getPointerAddress<T>(this)
    }

    @Frozen
    public unsafe func read(): T {
        return read(0)
    }

    @Frozen
    public unsafe func write(value: T): Unit {
        write(0, value)
    }

    /**
     * This interface does not check the correctness of the pointer.
     * If the pointer is invalid, an undefined error occurs.
     */
    @Frozen
    public unsafe func read(idx: Int64): T {
        return readPointer<T>(this, idx)
    }

    /**
     * This interface does not check the correctness of the pointer.
     * If the pointer is invalid, an undefined error occurs.
     */
    @Frozen
    public unsafe func write(idx: Int64, value: T): Unit {
        writePointer<T>(this, idx, value)
    }

    public unsafe operator func +(offset: Int64): CPointer<T> {
        return addPointer<T>(this, offset)
    }

    public unsafe operator func -(offset: Int64): CPointer<T> {
        return addPointer<T>(this, -offset)
    }

    public func asResource(): CPointerResource<T> {
        return CPointerResource(this)
    }
}

public struct CPointerResource<T> <: Resource where T <: CType {
    private let value: CPointer<T>
    private let isFree: AtomicInt64 = AtomicInt64(0)

    init(p: CPointer<T>) {
        this.value = p
    }

    public func isClosed(): Bool {
        return isFree.load() != 0
    }

    public func getValue(): CPointer<T> {
        if (isFree.load() != 0) {
            throw IllegalStateException("Resource has been closed")
        }
        return value
    }

    public func close(): Unit {
        if (isFree.compareAndSwap(0, 1)) {
            unsafe { LibC.free<T>(value) }
        }
    }
}
