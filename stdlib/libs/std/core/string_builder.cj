/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
 * This source file is part of the Cangjie project, licensed under Apache-2.0
 * with Runtime Library Exception.
 *
 * See https://cangjie-lang.cn/pages/LICENSE for license information.
 */

/**
 * @file
 *
 * This file defines StringBuilder.
 */
package std.core

let digits100: Array<Byte> = [
    b'0',b'0',b'0',b'1',b'0',b'2',b'0',b'3',b'0',b'4',b'0',b'5',b'0',b'6',b'0',b'7',b'0',b'8',b'0',b'9', //00 ~ 09
    b'1',b'0',b'1',b'1',b'1',b'2',b'1',b'3',b'1',b'4',b'1',b'5',b'1',b'6',b'1',b'7',b'1',b'8',b'1',b'9', //10 ~ 19
    b'2',b'0',b'2',b'1',b'2',b'2',b'2',b'3',b'2',b'4',b'2',b'5',b'2',b'6',b'2',b'7',b'2',b'8',b'2',b'9', //20 ~ 29
    b'3',b'0',b'3',b'1',b'3',b'2',b'3',b'3',b'3',b'4',b'3',b'5',b'3',b'6',b'3',b'7',b'3',b'8',b'3',b'9', //30 ~ 39
    b'4',b'0',b'4',b'1',b'4',b'2',b'4',b'3',b'4',b'4',b'4',b'5',b'4',b'6',b'4',b'7',b'4',b'8',b'4',b'9', //40 ~ 49
    b'5',b'0',b'5',b'1',b'5',b'2',b'5',b'3',b'5',b'4',b'5',b'5',b'5',b'6',b'5',b'7',b'5',b'8',b'5',b'9', //50 ~ 59
    b'6',b'0',b'6',b'1',b'6',b'2',b'6',b'3',b'6',b'4',b'6',b'5',b'6',b'6',b'6',b'7',b'6',b'8',b'6',b'9', //60 ~ 69
    b'7',b'0',b'7',b'1',b'7',b'2',b'7',b'3',b'7',b'4',b'7',b'5',b'7',b'6',b'7',b'7',b'7',b'8',b'7',b'9', //70 ~ 79
    b'8',b'0',b'8',b'1',b'8',b'2',b'8',b'3',b'8',b'4',b'8',b'5',b'8',b'6',b'8',b'7',b'8',b'8',b'8',b'9', //80 ~ 89
    b'9',b'0',b'9',b'1',b'9',b'2',b'9',b'3',b'9',b'4',b'9',b'5',b'9',b'6',b'9',b'7',b'9',b'8',b'9',b'9'] //90 ~ 99

/**
 * A mutable sequence of characters.
 * StringBuilder provides an efficient way to build strings by appending various types of data.
 */
public class StringBuilder <: ToString {
    private var data: Array<UInt8>
    private var endIndex: Int64

    private static const defaultCapacity: Int64 = 32

    /**
     * Constructs an empty StringBuilder with default capacity.
     */
    @Frozen
    public init() {
        this.data = Array<UInt8>(defaultCapacity, repeat: 0)
        this.endIndex = 0
    }

    /**
     * Constructs a StringBuilder initialized with the given string.
     *
     * @param str the initial string content
     */
    @Frozen
    public init(str: String) {
        let capacity = str.size + defaultCapacity
        this.data = Array<UInt8>(capacity, repeat: 0)
        unsafe { str.rawData().copyTo(this.data, 0, 0, str.size) }
        this.endIndex = str.size
    }

    /**
     * Constructs a StringBuilder with n repetitions of the given Rune.
     *
     * @param r the Rune to repeat
     * @param n the number of times to repeat the Rune
     * @throws IllegalArgumentException if n is negative
     */
    @Frozen
    public init(r: Rune, n: Int64) {
        if (n < 0) {
            throw IllegalArgumentException("The number of rune should not be negative.")
        }
        var bytesSize = Rune.utf8Size(r)
        let capacity = bytesSize * n + defaultCapacity
        this.data = Array<UInt8>(capacity, repeat: 0)
        var itemLen = 0
        for (_ in 0..n) {
            itemLen += Rune.intoUtf8Array(r, this.data, itemLen)
        }
        this.endIndex = itemLen
    }

    /**
     * Constructs a StringBuilder from an array of Runes.
     *
     * @param value the array of Runes to initialize with
     */
    @Frozen
    public init(value: Array<Rune>) {
        if (value.size == 0) {
            this.data = Array<UInt8>(defaultCapacity, repeat: 0)
            this.endIndex = 0
        } else {
            var temp = RunesToUtf8Bytes.getBytes(value)
            let capacity = temp.size + defaultCapacity
            this.data = Array<UInt8>(capacity, repeat: 0)
            unsafe { temp.copyTo(this.data, 0, 0, temp.size) }
            this.endIndex = temp.size
        }
    }

    /**
     * Constructs an empty StringBuilder with the specified capacity.
     *
     * @param capacity the initial capacity
     * @throws IllegalArgumentException if capacity is not positive
     */
    @Frozen
    public init(capacity: Int64) {
        if (capacity <= 0) {
            throw IllegalArgumentException("The capacity should be positive.")
        }
        this.data = Array<UInt8>(capacity, repeat: 0)
        this.endIndex = 0
    }

    /**
     * Returns the UTF-8 code unit size of this StringBuilder.
     *
     * @type Int64
     */
    @Frozen
    public prop size: Int64 {
        get() {
            return this.endIndex
        }
    }

    /**
     * Returns the capacity of this StringBuilder.
     *
     * @type Int64
     */
    @Frozen
    public prop capacity: Int64 {
        get() {
            return this.data.size
        }
    }

    /**
     * Returns the string representation of the contents of this StringBuilder.
     *
     * @returns String the string built by this StringBuilder
     */
    @Frozen
    public func toString(): String {
        return String(this.data[0..this.endIndex])
    }

    /**
     * Resets this StringBuilder, optionally with a new capacity.
     *
     * @param capacity the optional new capacity; if None, uses the default capacity
     * @throws IllegalArgumentException if the provided capacity is not positive
     * @throws IllegalMemoryException if there are some system errors
     */
    @Frozen
    public func reset(capacity!: Option<Int64> = None): Unit {
        let newCapacity = if (let Some(v) <- capacity) {
            if (v <= 0) {
                throw IllegalArgumentException("The capacity should be positive.")
            }
            v
        } else {
            defaultCapacity
        }
        this.data = Array<UInt8>(newCapacity, repeat: 0)
        this.endIndex = 0
    }

    /**
     * Appends a Rune to this StringBuilder.
     *
     * @param r the Rune to append
     */
    @OverflowWrapping
    @Frozen
    public func append(r: Rune): Unit {
        let byte = UInt32(r)
        if (byte <= UTF8_1_MAX) {
            this.grow(1)
            data[endIndex] = UInt8(byte)
            this.endIndex++
            return
        }
        var itemLen = Rune.utf8Size(r)
        this.grow(itemLen)
        var _ = Rune.intoUtf8Array(r, this.data, this.endIndex)
        this.endIndex += itemLen
    }

    /**
     * Appends a String to this StringBuilder.
     *
     * @param str the String to append
     */
    @OverflowWrapping
    @Frozen
    public func append(str: String): Unit {
        if (str.isEmpty()) {
            return
        }
        var strData = unsafe { str.rawData() }
        var itemLen = strData.size
        this.grow(itemLen)
        strData.copyTo(this.data, 0, this.endIndex, itemLen)
        this.endIndex += itemLen
    }

    /**
     * Appends the contents of another StringBuilder to this StringBuilder.
     *
     * @param sb the StringBuilder to append
     */
    @OverflowWrapping
    @Frozen
    public func append(sb: StringBuilder): Unit {
        let itemLen = sb.endIndex
        if (itemLen == 0) {
            return
        }
        this.grow(itemLen)
        sb.data.copyTo(this.data, 0, this.endIndex, itemLen)
        this.endIndex += itemLen
    }

    /**
     * Appends UTF-8 bytes from the given array after validating the encoding.
     *
     * @param arr the byte array containing UTF-8 data
     * @throws IllegalArgumentException if there is an invalid UTF-8 leading code in the array
     */
    @OverflowWrapping
    @Frozen
    public func appendFromUtf8(arr: Array<Byte>): Unit {
        if (arr.size == 0) {
            return
        }
        checkInvalid(arr.rawptr, arr.start, arr.start + arr.size)
        var arrSize = arr.size
        this.grow(arrSize)
        arr.copyTo(this.data, 0, this.endIndex, arrSize)
        this.endIndex += arrSize
    }

    /**
     * Appends UTF-8 bytes from the given array without validating the encoding.
     *
     * @param arr the byte array containing UTF-8 data
     */
    @OverflowWrapping
    @Frozen
    public unsafe func appendFromUtf8Unchecked(arr: Array<Byte>): Unit {
        if (arr.size == 0) {
            return
        }
        var itemLen = arr.size
        this.grow(itemLen)
        arr.copyTo(this.data, 0, this.endIndex, itemLen)
        this.endIndex += itemLen
        return
    }

    /**
     * Appends an array of Runes to this StringBuilder.
     *
     * @param runeArr the array of Runes to append
     */
    @Frozen
    public func append(runeArr: Array<Rune>): Unit {
        for (c in runeArr) {
            this.append(c)
        }
        return
    }

    /**
     * Appends a C-style string to this StringBuilder.
     *
     * @param cstr the CString to append
     */
    @Frozen
    public func append(cstr: CString): Unit {
        this.append(cstr.toString())
    }

    /**
     * Appends the string representation of a value to this StringBuilder.
     *
     * @param v the value to append
     */
    @Frozen
    public func append<T>(v: T): Unit where T <: ToString {
        match (v) {
            case i64: Int64 => this.append(i64)
            case i32: Int32 => this.append(i32)
            case i16: Int16 => this.append(i16)
            case i8: Int8 => this.append(i8)
            case u64: UInt64 => this.append(u64)
            case u32: UInt32 => this.append(u32)
            case u16: UInt16 => this.append(u16)
            case u8: UInt8 => this.append(u8)
            case r: Rune => this.append(r)
            case b: Bool => this.append(b)
            case _ => this.append(v.toString())
        }
    }

    /**
     * Appends the string representation of each element in the array to this StringBuilder.
     *
     * @param val the array of values to append
     */
    @Frozen
    public func append<T>(val: Array<T>): Unit where T <: ToString {
        for (v in val) {
            this.append(v)
        }
    }

    /**
     * Appends "true" or "false" to this StringBuilder.
     *
     * @param b the Bool to append
     */
    @Frozen
    public func append(b: Bool): Unit {
        if (b) {
            this.append("true")
        } else {
            this.append("false")
        }
    }

    /**
     * Appends the string representation of an Int64 to this StringBuilder.
     *
     * @param n the Int64 to append
     */
    @Frozen
    @OverflowWrapping
    public func append(n: Int64): Unit {
        if (n == Int64.Min) {
            this.append("-9223372036854775808") // the Int64.Min cannot be negated, so append the string directly
            return
        }
        var numLen: Int64 = 0
        var unum: UInt64 = if (n < 0) {
            UInt64(-n)
        } else {
            UInt64(n)
        }
        numLen += calcNumLen(unum)

        if (n < 0) {
            this.grow(numLen + 1)
            this.data[this.endIndex] = b'-'
            this.endIndex++
        } else {
            this.grow(numLen)
        }

        numberToString(unum, numLen, Array<Byte>(this.data.rawptr, this.endIndex, numLen))
        this.endIndex += numLen
    }

    /**
     * Appends the string representation of an Int32 to this StringBuilder.
     *
     * @param n the Int32 to append
     */
    @Frozen
    public func append(n: Int32): Unit {
        return this.append(Int64(n))
    }

    /**
     * Appends the string representation of an Int16 to this StringBuilder.
     *
     * @param n the Int16 to append
     */
    @Frozen
    public func append(n: Int16): Unit {
        return this.append(Int64(n))
    }

    /**
     * Appends the string representation of an Int8 to this StringBuilder.
     *
     * @param n the Int8 to append
     */
    @Frozen
    public func append(n: Int8): Unit {
        return this.append(Int64(n))
    }

    /**
     * Appends the string representation of a UInt64 to this StringBuilder.
     *
     * @param n the UInt64 to append
     */
    @OverflowWrapping
    @Frozen
    public func append(n: UInt64): Unit {
        var numLen = calcNumLen(n)
        this.grow(numLen)
        numberToString(n, numLen, Array<Byte>(this.data.rawptr, this.endIndex, numLen))
        this.endIndex += numLen
    }

    /**
     * Appends the string representation of a UInt32 to this StringBuilder.
     *
     * @param n the UInt32 to append
     */
    @Frozen
    public func append(n: UInt32): Unit {
        return this.append(UInt64(n))
    }

    /**
     * Appends the string representation of a UInt16 to this StringBuilder.
     *
     * @param n the UInt16 to append
     */
    @Frozen
    public func append(n: UInt16): Unit {
        return this.append(UInt64(n))
    }

    /**
     * Appends the string representation of a UInt8 to this StringBuilder.
     *
     * @param n the UInt8 to append
     */
    @Frozen
    public func append(n: UInt8): Unit {
        return this.append(UInt64(n))
    }

    /**
     * Appends the string representation of a Float64 to this StringBuilder.
     *
     * @param n the Float64 to append
     */
    @Frozen
    public func append(n: Float64): Unit {
        unsafe {
            let p: CPointer<UInt8> = CJ_CORE_Float64ToCPointer(n)
            if (p.isNull()) {
                return
            }
            append(p)
            LibC.free(p)
        }
    }

    /**
     * Appends the string representation of a Float32 to this StringBuilder.
     *
     * @param n the Float32 to append
     */
    @Frozen
    public func append(n: Float32): Unit {
        return this.append(Float64(n))
    }

    /**
     * Appends the string representation of a Float16 to this StringBuilder.
     *
     * @param n the Float16 to append
     */
    @Frozen
    public func append(n: Float16): Unit {
        return this.append(Float64(n))
    }

    /**
     * Ensures that the capacity is at least the current capacity plus the specified additional amount.
     *
     * @param additional the additional capacity required
     */
    @Frozen
    @OverflowThrowing
    public func reserve(additional: Int64): Unit {
        if (this.data.size - this.endIndex >= additional) {
            return
        }
        let oldCapacity = this.data.size
        var newCapacity = oldCapacity + (oldCapacity >> 1)
        if (newCapacity < this.endIndex + additional) {
            newCapacity = this.endIndex + additional
        }
        var itemData = Array<UInt8>(newCapacity, repeat: 0)
        this.data.copyTo(itemData, 0, 0, this.endIndex)
        this.data = itemData
    }

    @OverflowWrapping
    @Frozen
    private func grow(minCapacity: Int64): Unit {
        if (this.data.size - this.endIndex >= minCapacity) {
            return
        }
        let oldCapacity = this.data.size
        var newCapacity = oldCapacity + (oldCapacity >> 1)
        if (newCapacity < this.endIndex + minCapacity) {
            newCapacity = this.endIndex + minCapacity + (oldCapacity >> 1)
        }
        var itemData = Array<UInt8>(newCapacity, repeat: 0)
        this.data.copyTo(itemData, 0, 0, this.endIndex)
        this.data = itemData
    }

    /**
     * To use this method, ensure that the input complies with the UTF-8 code.
     * Otherwise, an error occurs.
     */
    @Frozen
    private unsafe func append(cp: CPointer<UInt8>): Unit {
        let cpSize = Int64(strlen(cp))
        if (cpSize <= 0) {
            return
        }
        this.grow(cpSize)
        var index = 0
        while (index < cpSize) {
            let c = UInt8(cp.read(index))
            this.data[index + this.endIndex] = c
            index++
        }
        this.endIndex += cpSize
    }
}

@OverflowWrapping
func calcNumLen(num: UInt64): Int64 {
    var p: UInt64 = 10
    for (i in 1..19) {
        if (num < p) {
            return i
        }
        p = p * UInt64(10)
    }
    if (num >= 10000000000000000000) {
        return 20
    }
    return 19
}

@OverflowWrapping
func numberToString(number: UInt64, endIndex: Int64, buffer: Array<Byte>): Unit {
    //make sure endindex is smaller then buffer.size
    var next = endIndex - 1
    var value = number
    var i = 0
    while (value >= 100) {
        let remain = value / 100
        i = Int64((value - 100 * remain) * 2)
        value = remain
        buffer[next] = digits100[i + 1]
        buffer[next - 1] = digits100[i]
        next = next - 2
    }
    if (value < 10) {
        buffer[next] = b'0' + UInt8(value)
    } else {
        i = Int64(value * 2)
        buffer[next] = digits100[i + 1]
        buffer[next - 1] = digits100[i]
    }
}
