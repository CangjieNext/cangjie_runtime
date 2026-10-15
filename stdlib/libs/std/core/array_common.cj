/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
 * This source file is part of the Cangjie project, licensed under Apache-2.0
 * with Runtime Library Exception.
 *
 * See https://cangjie-lang.cn/pages/LICENSE for license information.
 */

package std.core

@Frozen
@OverflowWrapping
private func arrayInitByCollection<T>(array: RawArray<T>, elements: Collection<T>): RawArray<T> {
    var i: Int64 = 0
    let it: Iterator<T> = elements.iterator()
    while (true) {
        match (it.next()) {
            case Some(v) => arraySet(array, i, v)
            case None => break
        }
        i = i + 1
    }
    return array
}

/**
 * Constructs a RawArray with the specified initial size and specified initial function.
 */
@Frozen
@OverflowWrapping
private func arrayInitByFunction<T>(array: RawArray<T>, initElement: (Int64) -> T): RawArray<T> {
    var i: Int64 = 0
    while (i < arraySize(array)) {
        arraySet(array, i, initElement(i))
        i++
    }
    return array
}

@Frozen
func toArray<T>(raw: RawArray<T>): Array<T> {
    if (arraySize(raw) == 0) {
        return Array<T>()
    }
    return Array<T>(raw, 0, arraySize(raw))
}
@Frozen
exclave func toArray<T>(raw: RawArray<T> @local!): Array<T> @local! {
    if (arraySize(raw) == 0) {
        return Array<T>()
    }
    return Array<T>(raw, 0, arraySize(raw))
}
@Frozen
exclave func toArray<T>(raw: RawArray<T> @local?): Array<T> @local? {
    if (arraySize(raw) == 0) {
        return Array<T>()
    }
    return Array<T>(raw, 0, arraySize(raw))
}

extend<T> Array<T> <: Collection<T> {
    /** Returns an iterator over the elements in this Array in proper sequence. */
    @Frozen
    public func iterator(): Iterator<T> {
        return ArrayIterator(this)
    }

    @Frozen
    public exclave func iterator(this @local!): Iterator<T> @local! {
        ArrayIterator<T>(this) @local!
    }

    @Frozen
    public exclave func iterator(this @local?): Iterator<T> @local? {
        ArrayIterator<T>(this) @local?
    }

    /**
     * Call 'size' on array type variable will be processed by compiler.
     * Only call based on 'Collection' type will use this function.
     */
    @Frozen
    public prop size: Int64 {
        get() {
            return this.len
        }
    }

    @Frozen
    public prop size: Int64 @local? {
        get() {
            this.len
        }
    }

    /** Returns true if this Array contains no elements. */
    @Frozen
    public func isEmpty(): Bool {
        return this.len == 0
    }
    @Frozen
    public func isEmpty(this @local?): Bool {
        return this.size == 0
    }

    @Frozen
    public func toArray(): Array<T> {
        return this.clone()
    }

    // 适配方案 Collection 接口适配（:1287）：新增 toArray(this @local!)。
    // 语义与 non-local 版一致（独立拷贝），委托 clone(this @local!) 完成
    // 新分配（rawArrayAllocate2）+ 逐份拷贝（intrinsicBuiltInCopyTo2）。
    @Frozen
    public exclave func toArray(this @local!): Array<T> @local! {
        this.clone()
    }

}


extend<T> Array<T> <: ToString where T <: ToString {
    @Frozen
    public func toString(): String {
        if (size == 0) {
            return "[]"
        }
        let sb = StringBuilder()
        sb.append(r'[')
        sb.append(this[0])
        for (i in 1..size) {
            sb.append(", ")
            sb.append(this[i])
        }
        sb.append(r']')
        return sb.toString()
    }

    @Frozen
    public exclave func toString(this @local?): String @local! {
        let sb: StringBuilder @local! = StringBuilder()
        sb.appendRune(r'[')
        if (size > 0) {
            sb.appendValue(this[0])
            for (i in 1..size) {
                sb.appendString(", ")
                sb.appendValue(this[i])
            }
        }
        sb.appendRune(r']')
        sb.toStringLocal()
    }
}

@ConstSafe
extend<T> Array<T> <: Equatable<Array<T>> where T <: Equatable<T> {
    @Frozen
    public const operator func ==(other: Array<T>): Bool {
        if (this.size != other.size) {
            return false
        }
        /* Back-to-front traversal can reduce boundary checks and improve performance */
        for (i in this.size - 1..=0 : -1) {
            if (this[i] != other[i]) {
                return false
            }
        }
        return true
    }

    @Frozen
    public const operator func !=(other: Array<T>): Bool {
        return !(this == other)
    }

    @Frozen
    public func contains(element: T): Bool {
        if (this.size == 0) {
            return false
        }
        /* Back-to-front traversal can reduce boundary checks and improve performance */
        for (i in this.size - 1..=0 : -1) {
            if (this[i] == element) {
                return true
            }
        }
        return false
    }

    /**
     * Returns the index of the first occurrence of the element in the src Array.
     *
     * @params element - The target element.
     *
     * @return Index of the first occurrence of the element in the Array, or None if the the element does not occur.
     * @throws IllegalArgumentException - If the start less than 0.
     */
    @Frozen
    public func indexOf(element: T): Option<Int64> {
        if (size == 0) {
            return None
        }
        for (i in 0..size) {
            if (this[i] == element) {
                return Some(i)
            }
        }
        return None
    }

    /**
     * Returns the index of the first occurrence of the element after fromIndex in the src Array.
     *
     * @params element - The target element.
     * @params fromIndex - The start index, default 0.
     *
     * @return Index of the first occurrence of the element in the Array, or None if the the element does not occur.
     * @throws IllegalArgumentException - If the start less than 0.
     */
    @Frozen
    public func indexOf(element: T, fromIndex: Int64): Option<Int64> {
        if (size == 0 || fromIndex >= size) {
            return None
        }
        let start = if (fromIndex >= 0) {
            fromIndex
        } else {
            0
        }
        for (i in start..size) {
            if (this[i] == element) {
                return Some(i)
            }
        }
        return None
    }

    /**
     * Returns the index of the first occurrence of a subarray in this Array.
     *
     * @params elements - The target subarray.
     *
     * @return Index of the first occurrence of @p elements, or None if the subarray does not occur.
     */
    @Frozen
    public func indexOf(elements: Array<T>): Option<Int64> {
        if (this.size < elements.size) {
            return None
        }
        for (i in 0..(size - elements.size + 1)) {
            if (this[i..i + elements.size] == elements) {
                return Some(i)
            }
        }
        return None
    }

    /**
     * Returns the index of the first occurrence of a subarray at or after @p fromIndex.
     *
     * @params elements - The target subarray.
     *
     * @return Index of the first occurrence of @p elements from the starting position, or None if it does not occur.
     */
    @Frozen
    public func indexOf(elements: Array<T>, fromIndex: Int64): Option<Int64> {
        if (this.size < elements.size || fromIndex >= this.size) {
            return None
        }
        let start = if (fromIndex >= 0) {
            fromIndex
        } else {
            0
        }
        for (i in start..(size - elements.size + 1)) {
            if (this[i..i + elements.size] == elements) {
                return Some(i)
            }
        }
        return None
    }

    @Frozen
    public func lastIndexOf(element: T): Option<Int64> {
        if (size == 0) {
            return None
        }
        for (i in size - 1..=0 : -1) {
            if (this[i] == element) {
                return Some(i)
            }
        }
        return None
    }

    @Frozen
    public func lastIndexOf(element: T, fromIndex: Int64): Option<Int64> {
        if (size == 0 || fromIndex >= size) {
            return None
        }
        let start = if (fromIndex >= 0) {
            fromIndex
        } else {
            0
        }
        for (i in size - 1..=start : -1) {
            if (this[i] == element) {
                return Some(i)
            }
        }
        return None
    }

    @Frozen
    public func lastIndexOf(elements: Array<T>): Option<Int64> {
        if (this.size < elements.size) {
            return None
        }
        for (i in (size - elements.size)..=0 : -1) {
            if (this[i..i + elements.size] == elements) {
                return Some(i)
            }
        }
        return None
    }

    @Frozen
    public func lastIndexOf(elements: Array<T>, fromIndex: Int64): Option<Int64> {
        if (this.size < elements.size || fromIndex >= this.size) {
            return None
        }
        let start = if (fromIndex >= 0) {
            fromIndex
        } else {
            0
        }
        for (i in (size - elements.size)..=start : -1) {
            if (this[i..i + elements.size] == elements) {
                return Some(i)
            }
        }
        return None
    }

    /**
     * Returns a slice that dones not contain elements from the start.
     *
     * @param set - The target elements to trim.
     *
     * @return The slice of the src.
     */
    @Frozen
    @OverflowWrapping
    public func trimStart(set: Array<T>): Array<T> {
        var trim = false
        for (i in 0..this.len) {
            let elem = this[i]
            for (j in (set.len - 1)..-1 : -1) {
                if (set[j] == elem) {
                    trim = true
                    break
                }
                trim = false
            }
            if (!trim) {
                return this[i..]
            }
        }
        return Array<T>()
    }

    /**
     * Returns a slice that dones not contain elements from the end.
     *
     * @param set - The target elements to trim.
     *
     * @return The slice of the src.
     */
    @Frozen
    public func trimEnd(set: Array<T>): Array<T> {
        var trim = false
        for (i in (this.len - 1)..-1 : -1) {
            let elem = this[i]
            for (j in (set.len - 1)..-1 : -1) {
                if (set[j] == elem) {
                    trim = true
                    break
                }
                trim = false
            }
            if (!trim) {
                return this[..(i + 1)]
            }
        }
        return Array<T>()
    }

    /**
     * Returns a slice that dones not contain elements from the start.
     *
     * @param predicate - the elements satisfy the predicate should be trimmed.
     *
     * @return The slice of the src.
     */
    @Frozen
    public func trimStart(predicate: (T) -> Bool): Array<T> {
        for (i in 0..this.len) {
            if (!predicate(this[i])) {
                return this[i..]
            }
        }
        return Array<T>()
    }

    /**
     * Returns a slice that dones not contain elements from the end.
     *
     * @param predicate - the elements satisfy the predicate should be trimmed.
     *
     * @return The slice of the src.
     */
    @Frozen
    public func trimEnd(predicate: (T) -> Bool): Array<T> {
        for (i in (this.len - 1)..-1 : -1) {
            if (!predicate(this[i])) {
                return this[..(i + 1)]
            }
        }
        return Array<T>()
    }

    @Frozen
    public func removePrefix(prefix: Array<T>): Array<T> {
        if (this.size < prefix.size || prefix.size == 0) {
            return this
        }
        if (this[..prefix.size] == prefix) {
            return this[prefix.size..]
        }
        return this
    }

    @Frozen
    public func removeSuffix(suffix: Array<T>): Array<T> {
        if (this.size < suffix.size || suffix.size == 0) {
            return this
        }
        if (this[(this.size - suffix.size)..] == suffix) {
            return this[..(this.size - suffix.size)]
        }
        return this
    }
}

/**
 * Array-based Iterator.
 */
public class ArrayIterator<T> <: Iterator<T> {
    private var myData: Array<T>
    private let end: Int64
    private var myPosition: Int64

    @Frozen
    public init(data: Array<T>) {
        this.myData = data
        this.end = data.size
        this.myPosition = 0
    }

    @Frozen
    internal exclave init(this @local!, data: Array<T> @local!) {
        this.myData = data
        this.end = data.size
        this.myPosition = 0
    }

    @Frozen
    internal exclave init(this @local?, data: Array<T> @local?) {
        this.myData = data
        this.end = data.size
        this.myPosition = 0
    }

    /** Returns the next element in the iteration. */
    @Frozen
    @OverflowWrapping
    public func next(): Option<T> {
        if (this.myPosition < this.end) {
            var v: Option<T> = Some(this.myData[this.myPosition])
            this.myPosition++
            return v
        } else {
            return None
        }
    }

    /** Returns the next element in the iteration. */
    @Frozen
    @OverflowWrapping
    public exclave func next(this @local!): Option<T> @local! {
        if (this.myPosition < this.end) {
            let v: T @local! = arrayGetUnchecked2<T>(this.myData.rawptr, this.myData.start + this.myPosition)
            this.myPosition++
            return Some<T>(v)
        }
        None<T>
    }

    /** Returns the next element in the iteration. */
    @Frozen
    @OverflowWrapping
    public exclave func next(this @local?): Option<T> @local? {
        if (this.myPosition < this.end) {
            let v: T @local? = arrayGetUnchecked3<T>(this.myData.rawptr, this.myData.start + this.myPosition)
            this.myPosition++
            return Some<T>(v)
        }
        None<T>
    }
}

public struct CPointerHandle<T> where T <: CType {
    public let pointer: CPointer<T>
    public let array: Array<T>

    @Deprecated[message: "Use global function `public unsafe func acquireArrayRawData<T>(arr: Array<T>): CPointerHandle<T> where T <: CType` instead."]
    public init() {
        pointer = CPointer<T>()
        array = Array<T>()
    }

    @Deprecated[message: "Use global function `public unsafe func acquireArrayRawData<T>(arr: Array<T>): CPointerHandle<T> where T <: CType` instead."]
    public init(ptr: CPointer<T>, arr: Array<T>) {
        this.pointer = ptr
        this.array = arr
    }

    // gap audit 2026-09-24 D1b/8c：本构造降为 internal——唯一构造点是同包
    // acquireArrayRawData @local?（:577）。
    internal exclave init(this @local?, ptr: CPointer<T> @local?, arr: Array<T> @local?) {
        pointer = ptr
        array = arr
    }
}

/**
 * Caution: any behavior that may allocate memory in cangjie heap or
 * trigger garbage collection between func acquireArrayRawData<T>(arr: Array<T>)
 * and func releaseArrayRawData<T>(handle: CPointerHandle<T>)
 * is permitted in case of dead lock
 */
@Frozen
public unsafe func acquireArrayRawData<T>(arr: Array<T>): CPointerHandle<T> where T <: CType {
    CPointerHandle<T>(acquireRawData<T>(arr.rawptr) + arr.start, arr)
}

@Frozen
// 支撑面(刻意保留 public,2026-09-28 修正 9-24 的 D1b/8e 降级决策):cangjie_stdx 的
// @local! 上下文(package internal handle / encoding.json write_buffer 等)以 @local!
// 数组实参调用本重载——plain 版收不了 @local! 实参,@local? 版一旦降为 internal,
// stdx 立即编译失败(2026-09-28 Server 构建实测:stdx.encoding.json 6 处
// "unable to infer generic argument ... impossible to match parameter type
// 'Struct-Array<Generics-T>'" / "expected 'Struct-Array<UInt8>', found '... @local!'")。
// stdlib 内部仅 string_common.cj 的 String @local? 搜索路径使用。
public exclave unsafe func acquireArrayRawData<T>(arr: Array<T> @local?): CPointerHandle<T> @local? where T <: CType {
    CPointerHandle<T>(acquireRawData2<T>(arr.rawptr) + arr.start, arr)
}

@Frozen
public unsafe func releaseArrayRawData<T>(handle: CPointerHandle<T>): Unit where T <: CType {
    releaseRawData(handle.array.rawptr, handle.pointer - handle.array.start)
}

@Frozen
// 支撑面(刻意保留 public,同 acquireArrayRawData @local?)。
public unsafe func releaseArrayRawData<T>(handle: CPointerHandle<T> @local?): Unit where T <: CType {
    releaseRawData2(handle.array.rawptr, handle.pointer - handle.array.start)
}


/**
 * 提案 8 节 D 类（判定/计数：indexOf、lastIndexOf）在 `@local?` 实例上的实现。
 * 交付清单 = 策略问题闭环 §3.4.1 判定类列出的 7 个形态。
 *
 * 实现方式：**就地循环**——用 arrayGetUnchecked3 逐个取出 `T @local?` 元素与目标比较，
 * 不做 non-local 副本，故只需 `T <: Equatable<T>`（旧版走 cloneToNotLocal + deLocalizeElement
 * 需额外 `T <: Copyable`；2026-09-28 重探确认不再必要——旧版两条依据：(1) `@local?` 元素
 * 写入 non-local 容器受 Array Copyable 扩展的约束（旧版误记为「arraySet3 的 Copyable 约束」；
 * arraySet3 本身无约束，带约束的是公开写入器与 arraySetUnchecked3），(2) `T @local?` 传给 plain 形参被拒，
 * 在就地写法下都不触发）。实测：非 Copyable 的 class 元素（自适配 == @local?）命中/未命中/
 * fromIndex 全部正确；未适配 == @local? 的类型经 Equatable 默认体抛 UnimplementedException
 * （与 §4.2.5/修改建议 #4 的统一策略一致）。
 *
 * 闭环 §3.4.1 未列、本文件也不提供的形态：contains、无 fromIndex 的 indexOf(element)、
 * `==` / `!=`（==/!= 另有 extend 遮蔽限制：error: extend member '==' is not allowed to
 * shadow members of 'extend Array'）。
 */
extend<T> Array<T> where T <: Equatable<T> {
    public func indexOf(this @local?, element: T @local?, fromIndex: Int64): Option<Int64> {
        if (this.len == 0 || fromIndex >= this.len) {
            return None
        }
        let start = if (fromIndex >= 0) {
            fromIndex
        } else {
            0
        }
        for (i in start..this.len) {
            if (arrayGetUnchecked3<T>(this.rawptr, this.start + i) == element) {
                return Some(i)
            }
        }
        return None
    }

    public func indexOf(this @local?, elements: Array<T> @local?): Option<Int64> {
        if (this.len < elements.len) {
            return None
        }
        for (i in 0..(this.len - elements.len + 1)) {
            var matched: Bool = true
            for (j in 0..elements.len) {
                if (arrayGetUnchecked3<T>(this.rawptr, this.start + i + j) !=
                    arrayGetUnchecked3<T>(elements.rawptr, elements.start + j)) {
                    matched = false
                    break
                }
            }
            if (matched) {
                return Some(i)
            }
        }
        return None
    }

    public func indexOf(this @local?, elements: Array<T> @local?, fromIndex: Int64): Option<Int64> {
        if (this.len < elements.len || fromIndex >= this.len) {
            return None
        }
        let start = if (fromIndex >= 0) {
            fromIndex
        } else {
            0
        }
        if (start > this.len - elements.len) {
            return None
        }
        for (i in start..(this.len - elements.len + 1)) {
            var matched: Bool = true
            for (j in 0..elements.len) {
                if (arrayGetUnchecked3<T>(this.rawptr, this.start + i + j) !=
                    arrayGetUnchecked3<T>(elements.rawptr, elements.start + j)) {
                    matched = false
                    break
                }
            }
            if (matched) {
                return Some(i)
            }
        }
        return None
    }

    public func lastIndexOf(this @local?, element: T @local?): Option<Int64> {
        if (this.len == 0) {
            return None
        }
        for (i in this.len - 1..=0 : -1) {
            if (arrayGetUnchecked3<T>(this.rawptr, this.start + i) == element) {
                return Some(i)
            }
        }
        return None
    }

    public func lastIndexOf(this @local?, element: T @local?, fromIndex: Int64): Option<Int64> {
        if (this.len == 0 || fromIndex >= this.len) {
            return None
        }
        let start = if (fromIndex >= 0) {
            fromIndex
        } else {
            0
        }
        for (i in this.len - 1..=start : -1) {
            if (arrayGetUnchecked3<T>(this.rawptr, this.start + i) == element) {
                return Some(i)
            }
        }
        return None
    }

    public func lastIndexOf(this @local?, elements: Array<T> @local?): Option<Int64> {
        if (this.len < elements.len) {
            return None
        }
        for (i in (this.len - elements.len)..=0 : -1) {
            var matched: Bool = true
            for (j in 0..elements.len) {
                if (arrayGetUnchecked3<T>(this.rawptr, this.start + i + j) !=
                    arrayGetUnchecked3<T>(elements.rawptr, elements.start + j)) {
                    matched = false
                    break
                }
            }
            if (matched) {
                return Some(i)
            }
        }
        return None
    }

    public func lastIndexOf(this @local?, elements: Array<T> @local?, fromIndex: Int64): Option<Int64> {
        if (this.len < elements.len || fromIndex >= this.len) {
            return None
        }
        let start = if (fromIndex >= 0) {
            fromIndex
        } else {
            0
        }
        for (i in (this.len - elements.len)..=start : -1) {
            var matched: Bool = true
            for (j in 0..elements.len) {
                if (arrayGetUnchecked3<T>(this.rawptr, this.start + i + j) !=
                    arrayGetUnchecked3<T>(elements.rawptr, elements.start + j)) {
                    matched = false
                    break
                }
            }
            if (matched) {
                return Some(i)
            }
        }
        return None
    }
}
