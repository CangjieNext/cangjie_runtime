/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
 * This source file is part of the Cangjie project, licensed under Apache-2.0
 * with Runtime Library Exception.
 *
 * See https://cangjie-lang.cn/pages/LICENSE for license information.
 */

package std.core

/**
 * Prints a string to standard output.
 *
 * @param str the string to print
 * @param flush whether to flush the output stream
 * @throws OutOfMemoryError if memory allocation fails during printing
 */
@Frozen
public func print(str: String, flush!: Bool = false): Unit {
    unsafe {
        var cp = acquireArrayRawData(str.rawData())
        let ret = CJ_CORE_PrintUTF8(cp.pointer, str.size, false, flush)
        releaseArrayRawData(cp)
        if (ret == -1) {
            throw OutOfMemoryError()
        }
    }
}

/**
 * Prints a newline to standard output.
 */
@Frozen
public func println(): Unit {
    unsafe {
        CJ_CORE_PrintChar(UInt32(r'\n'), false, false)
    }
}

/**
 * Prints a string followed by a newline to standard output.
 *
 * @param str the string to print
 * @throws OutOfMemoryError if memory allocation fails during printing
 */
@Frozen
public func println(str: String): Unit {
    unsafe {
        var cp = acquireArrayRawData(str.rawData())
        let ret = CJ_CORE_PrintUTF8(cp.pointer, str.size, true, false)
        releaseArrayRawData(cp)
        if (ret == -1) {
            throw OutOfMemoryError()
        }
    }
}

// The `eprint` is used to print error message, such as exception throwing.
// The message will be printed to standard error text stream rather than standard output.

/**
 * Prints a string to standard error.
 *
 * @param str the string to print
 * @param flush whether to flush the error stream
 * @throws OutOfMemoryError if memory allocation fails during printing
 */
@Frozen
public func eprint(str: String, flush!: Bool = true): Unit {
    unsafe {
        var cp = acquireArrayRawData(str.rawData())
        let ret = CJ_CORE_ErrorPrintUTF8(cp.pointer, str.size, false, flush)
        releaseArrayRawData(cp)
        if (ret == -1) {
            throw OutOfMemoryError()
        }
    }
}

// The `eprintln` is used to print error message, such as exception throwing.
// The message will be printed to standard error text stream rather than standard output.

/**
 * Prints a string followed by a newline to standard error.
 *
 * @param str the string to print
 * @throws OutOfMemoryError if memory allocation fails during printing
 */
@Frozen
public func eprintln(str: String): Unit {
    unsafe {
        var cp = acquireArrayRawData(str.rawData())
        let ret = CJ_CORE_ErrorPrintUTF8(cp.pointer, str.size, true, true)
        releaseArrayRawData(cp)
        if (ret == -1) {
            throw OutOfMemoryError()
        }
    }
}

/**
 * Prints the string representation of a value to standard error.
 *
 * @param arg the value to print
 * @param flush whether to flush the error stream
 */
@Frozen
public func eprint<T>(arg: T, flush!: Bool = false): Unit where T <: ToString {
    eprint(arg.toString(), flush: flush)
}

/**
 * Prints the string representation of a value followed by a newline to standard error.
 *
 * @param arg the value to print
 */
@Frozen
public func eprintln<T>(arg: T): Unit where T <: ToString {
    eprintln(arg.toString())
}

/**
 * Prints the string representation of a value to standard output.
 *
 * @param arg the value to print
 * @param flush whether to flush the output stream
 */
@Frozen
public func print<T>(arg: T, flush!: Bool = false): Unit where T <: ToString {
    print(arg.toString(), flush: flush)
}

/**
 * Prints the string representation of a value followed by a newline to standard output.
 *
 * @param arg the value to print
 */
@Frozen
public func println<T>(arg: T): Unit where T <: ToString {
    println(arg.toString())
}

/**
 * Reads a line from standard input.
 *
 * @returns String the line read from standard input, without trailing newline
 * @throws IllegalStateException if an I/O error or EOF occurs while reading
 */
public func readln(): String {
    let ln = readFromConsole()
    if (ln.endsWith("\r\n")) {
        return ln[..ln.size - 2]
    } else if (ln.endsWith('\n')) {
        return ln[..ln.size - 1]
    }
    return ln
}

private func readFromConsole(): String {
    let build: StringBuilder = StringBuilder()
    let line: CPointer<StructureString> = unsafe { CJ_CONSOLE_Readline() }
    if (line.isNull()) {
        unsafe { CJ_CONSOLE_ClearstdInerr() }
        throw IllegalStateException("Failed to read from console: I/O error or EOF.")
    }
    let str = unsafe { line.read() }
    var itemLen = Int64(str.length)
    try {
        var buffer: Array<Byte> = Array<Byte>(itemLen, repeat: 0)
        var i: Int64 = 0
        while (i < itemLen) {
            var c = unsafe { str.buffer.read(i) }
            buffer[i] = c
            i++
        }
        build.appendFromUtf8(buffer)
    } finally {
        unsafe {
            LibC.free(str.buffer)
            LibC.free(line)
        }
    }
    return build.toString()
}

/**
 * Overloaded print functions for primitive types for performance optimization.
 * For example, when using `Bool` type as the argument of `print` function, there are two steps:
 * 1. call `toString()` method to create a String
 * 2. call `print(String)` function to do the printing
 * A few temporary objects like `Array<UInt8>` and `String` are created in above process.
 * If we provide a few overloaded `print` function below, these temporary objects can be avoided.
 */

/**
 * Prints a Bool to standard output.
 *
 * @param b the Bool to print
 * @param flush whether to flush the output stream
 */
@Frozen
public func print(b: Bool, flush!: Bool = false): Unit {
    unsafe { CJ_CORE_PrintBool(b, false, flush) }
}

/**
 * Prints a Bool followed by a newline to standard output.
 *
 * @param b the Bool to print
 */
@Frozen
public func println(b: Bool): Unit {
    unsafe { CJ_CORE_PrintBool(b, true, false) }
}

/**
 * Prints a Rune to standard output.
 *
 * @param c the Rune to print
 * @param flush whether to flush the output stream
 */
@Frozen
public func print(c: Rune, flush!: Bool = false): Unit {
    unsafe { CJ_CORE_PrintChar(UInt32(c), false, flush) }
}

/**
 * Prints a Rune followed by a newline to standard output.
 *
 * @param c the Rune to print
 */
@Frozen
public func println(c: Rune): Unit {
    unsafe { CJ_CORE_PrintChar(UInt32(c), true, false) }
}

/**
 * Prints a Float16 to standard output.
 *
 * @param f the Float16 to print
 * @param flush whether to flush the output stream
 */
@Frozen
public func print(f: Float16, flush!: Bool = false): Unit {
    unsafe { CJ_CORE_PrintFloat(Float64(f), false, flush) }
}

/**
 * Prints a Float16 followed by a newline to standard output.
 *
 * @param f the Float16 to print
 */
@Frozen
public func println(f: Float16): Unit {
    unsafe { CJ_CORE_PrintFloat(Float64(f), true, false) }
}

/**
 * Prints a Float32 to standard output.
 *
 * @param f the Float32 to print
 * @param flush whether to flush the output stream
 */
@Frozen
public func print(f: Float32, flush!: Bool = false): Unit {
    unsafe { CJ_CORE_PrintFloat(Float64(f), false, flush) }
}

/**
 * Prints a Float32 followed by a newline to standard output.
 *
 * @param f the Float32 to print
 */
@Frozen
public func println(f: Float32): Unit {
    unsafe { CJ_CORE_PrintFloat(Float64(f), true, false) }
}

/**
 * Prints a Float64 to standard output.
 *
 * @param f the Float64 to print
 * @param flush whether to flush the output stream
 */
@Frozen
public func print(f: Float64, flush!: Bool = false): Unit {
    unsafe { CJ_CORE_PrintFloat(f, false, flush) }
}

/**
 * Prints a Float64 followed by a newline to standard output.
 *
 * @param f the Float64 to print
 */
@Frozen
public func println(f: Float64): Unit {
    unsafe { CJ_CORE_PrintFloat(f, true, false) }
}

/**
 * Prints an Int8 to standard output.
 *
 * @param i the Int8 to print
 * @param flush whether to flush the output stream
 */
@Frozen
public func print(i: Int8, flush!: Bool = false): Unit {
    unsafe { CJ_CORE_PrintSigned(Int64(i), false, flush) }
}

/**
 * Prints an Int16 to standard output.
 *
 * @param i the Int16 to print
 * @param flush whether to flush the output stream
 */
@Frozen
public func print(i: Int16, flush!: Bool = false): Unit {
    unsafe { CJ_CORE_PrintSigned(Int64(i), false, flush) }
}

/**
 * Prints an Int32 to standard output.
 *
 * @param i the Int32 to print
 * @param flush whether to flush the output stream
 */
@Frozen
public func print(i: Int32, flush!: Bool = false): Unit {
    unsafe { CJ_CORE_PrintSigned(Int64(i), false, flush) }
}

/**
 * Prints an Int64 to standard output.
 *
 * @param i the Int64 to print
 * @param flush whether to flush the output stream
 */
@Frozen
public func print(i: Int64, flush!: Bool = false): Unit {
    unsafe { CJ_CORE_PrintSigned(i, false, flush) }
}

/**
 * Prints a UInt8 to standard output.
 *
 * @param i the UInt8 to print
 * @param flush whether to flush the output stream
 */
@Frozen
public func print(i: UInt8, flush!: Bool = false): Unit {
    unsafe { CJ_CORE_PrintUnsigned(UInt64(i), false, flush) }
}

/**
 * Prints a UInt16 to standard output.
 *
 * @param i the UInt16 to print
 * @param flush whether to flush the output stream
 */
@Frozen
public func print(i: UInt16, flush!: Bool = false): Unit {
    unsafe { CJ_CORE_PrintUnsigned(UInt64(i), false, flush) }
}

/**
 * Prints a UInt32 to standard output.
 *
 * @param i the UInt32 to print
 * @param flush whether to flush the output stream
 */
@Frozen
public func print(i: UInt32, flush!: Bool = false): Unit {
    unsafe { CJ_CORE_PrintUnsigned(UInt64(i), false, flush) }
}

/**
 * Prints a UInt64 to standard output.
 *
 * @param i the UInt64 to print
 * @param flush whether to flush the output stream
 */
@Frozen
public func print(i: UInt64, flush!: Bool = false): Unit {
    unsafe { CJ_CORE_PrintUnsigned(i, false, flush) }
}

/**
 * Prints an Int8 followed by a newline to standard output.
 *
 * @param i the Int8 to print
 */
@Frozen
public func println(i: Int8): Unit {
    unsafe { CJ_CORE_PrintSigned(Int64(i), true, false) }
}

/**
 * Prints an Int16 followed by a newline to standard output.
 *
 * @param i the Int16 to print
 */
@Frozen
public func println(i: Int16): Unit {
    unsafe { CJ_CORE_PrintSigned(Int64(i), true, false) }
}

/**
 * Prints an Int32 followed by a newline to standard output.
 *
 * @param i the Int32 to print
 */
@Frozen
public func println(i: Int32): Unit {
    unsafe { CJ_CORE_PrintSigned(Int64(i), true, false) }
}

/**
 * Prints an Int64 followed by a newline to standard output.
 *
 * @param i the Int64 to print
 */
@Frozen
public func println(i: Int64): Unit {
    unsafe { CJ_CORE_PrintSigned(i, true, false) }
}

/**
 * Prints a UInt8 followed by a newline to standard output.
 *
 * @param i the UInt8 to print
 */
@Frozen
public func println(i: UInt8): Unit {
    unsafe { CJ_CORE_PrintUnsigned(UInt64(i), true, false) }
}

/**
 * Prints a UInt16 followed by a newline to standard output.
 *
 * @param i the UInt16 to print
 */
@Frozen
public func println(i: UInt16): Unit {
    unsafe { CJ_CORE_PrintUnsigned(UInt64(i), true, false) }
}

/**
 * Prints a UInt32 followed by a newline to standard output.
 *
 * @param i the UInt32 to print
 */
@Frozen
public func println(i: UInt32): Unit {
    unsafe { CJ_CORE_PrintUnsigned(UInt64(i), true, false) }
}

/**
 * Prints a UInt64 followed by a newline to standard output.
 *
 * @param i the UInt64 to print
 */
@Frozen
public func println(i: UInt64): Unit {
    unsafe { CJ_CORE_PrintUnsigned(i, true, false) }
}
