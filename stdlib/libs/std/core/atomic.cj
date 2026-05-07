/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
 * This source file is part of the Cangjie project, licensed under Apache-2.0
 * with Runtime Library Exception.
 *
 * See https://cangjie-lang.cn/pages/LICENSE for license information.
 */

package std.core

enum MemoryOrder {
    SeqCst
}

@Intrinsic
func load<ATOMIC_TYPE, TYPE>(atomicType: ATOMIC_TYPE, memoryOrder: MemoryOrder): TYPE

@Intrinsic
func store<ATOMIC_TYPE, TYPE>(atomicType: ATOMIC_TYPE, value: TYPE, memoryOrder: MemoryOrder): Unit

@Intrinsic
func fetchAdd<ATOMIC_TYPE, TYPE>(atomicType: ATOMIC_TYPE, value: TYPE, memoryOrder: MemoryOrder): TYPE

@Intrinsic
func compareAndSwap<ATOMIC_TYPE, TYPE>(
    atomicType: ATOMIC_TYPE,
    oldValue: TYPE,
    newValue: TYPE,
    successOrder: MemoryOrder,
    failureOrder: MemoryOrder
): Bool

/**
 * A boolean value that supports atomic operations.
 */
class AtomicBool {
    private var value: Int8 = 0

    /**
     * Constructs an AtomicBool with the given initial value.
     *
     * @param val the initial value
     */
    init(val: Bool) {
        var valInt8: Int8 = if (val) {
            1
        } else {
            0
        }
        store<AtomicBool, Int8>(this, valInt8, MemoryOrder.SeqCst)
    }

    /**
     * Atomically loads and returns the current value.
     *
     * @returns Bool the current value
     */
    func load(): Bool {
        var v = load<AtomicBool, Int8>(this, MemoryOrder.SeqCst)
        return v != 0
    }

    /**
     * Atomically stores the given value.
     *
     * @param val the value to store
     */
    func store(val: Bool): Unit {
        var valInt8: Int8 = if (val) {
            1
        } else {
            0
        }
        store<AtomicBool, Int8>(this, valInt8, MemoryOrder.SeqCst)
    }
}

/**
 * A 64-bit integer that supports atomic operations.
 */
class AtomicInt64 {
    private var value: Int64

    /**
     * Constructs an AtomicInt64 with the given initial value.
     *
     * @param val the initial value
     */
    init(val: Int64) {
        value = val
        store<AtomicInt64, Int64>(this, val, MemoryOrder.SeqCst)
    }

    /**
     * Atomically adds the given value and returns the previous value.
     *
     * @param val the value to add
     * @returns Int64 the previous value before the addition
     */
    func fetchAdd(val: Int64): Int64 {
        fetchAdd<AtomicInt64, Int64>(this, val, MemoryOrder.SeqCst)
    }

    /**
     * Atomically loads and returns the current value.
     *
     * @returns Int64 the current value
     */
    func load(): Int64 {
        load<AtomicInt64, Int64>(this, MemoryOrder.SeqCst)
    }

    /**
     * Atomically stores the given value.
     *
     * @param val the value to store
     */
    func store(val: Int64): Unit {
        store<AtomicInt64, Int64>(this, val, MemoryOrder.SeqCst)
    }

    /**
     * Atomically compares the current value with the expected value,
     * and if equal, sets it to the new value.
     *
     * @param old the expected current value
     * @param new the new value to set if the comparison succeeds
     * @returns Bool true if the comparison succeeded and the value was updated
     */
    func compareAndSwap(old: Int64, new: Int64): Bool {
        return compareAndSwap<AtomicInt64, Int64>(this, old, new, MemoryOrder.SeqCst, MemoryOrder.SeqCst)
    }
}

/**
 * A reference that supports atomic operations on object references.
 */
class AtomicReference<T> where T <: Object {
    private var value: T

    /**
     * Constructs an AtomicReference with the given initial value.
     *
     * @param val the initial value
     */
    init(val: T) {
        value = val
        store<AtomicReference<T>, T>(this, val, MemoryOrder.SeqCst)
    }

    /**
     * Atomically loads and returns the current value.
     *
     * @returns T the current value
     */
    func load(): T {
        load<AtomicReference<T>, T>(this, MemoryOrder.SeqCst)
    }

    /**
     * Atomically stores the given value.
     *
     * @param val the value to store
     */
    func store(val: T): Unit {
        store<AtomicReference<T>, T>(this, val, MemoryOrder.SeqCst)
    }
}

/**
 * A wrapper class to atomically load/store values of any types.
 */
class AtomicBox<T> {
    private let boxRef: AtomicReference<Box<T>>

    /**
     * Constructs an AtomicBox with the given initial value.
     *
     * @param val the initial value
     */
    init(val: T) {
        boxRef = AtomicReference((Box(val)))
    }

    /**
     * Atomically loads and returns the current value.
     *
     * @returns T the current value
     */
    func load(): T {
        boxRef.load().value
    }

    /**
     * Atomically stores the given value.
     *
     * @param val the value to store
     */
    func store(val: T): Unit {
        boxRef.store(Box<T>(val))
    }
}
