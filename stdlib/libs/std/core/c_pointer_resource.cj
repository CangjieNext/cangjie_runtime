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

    @OverflowWrapping
    public unsafe operator func -(offset: Int64): CPointer<T> {
        if (offset == Int64.Min) {
            throw OverflowException("Pointer offset overflow: cannot negate 'Int64.Min'.")
        }
        return addPointer<T>(this, (-1) * offset)
    }

    public func asResource(): CPointerResource<T> {
        return CPointerResource(this)
    }
}

public struct CPointerResource<T> <: Resource where T <: CType {
    public let value: CPointer<T>
    // 0 = not freed, 1 = freed. Atomic so that close() on copies shared
    // across threads frees the native pointer exactly once.
    private let isFree: AtomicBool = AtomicBool(false)

    init(p: CPointer<T>) {
        this.value = p
    }

    public func isClosed(): Bool {
        return isFree.load()
    }

    public func close(): Unit {
        // Atomic check-and-set: exactly one caller (the CAS winner) frees the
        // native pointer. Concurrent close() calls on copies of the same
        // resource previously raced between the isFree check and the store,
        // double-freeing the pointer (see issue #977).
        if (isFree.compareAndSwap(false, true)) {
            unsafe { LibC.free<T>(value) }
        }
    }
}
