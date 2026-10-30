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
 * This is a class for consoleReader.
 */

package std.env

import std.io.InputStream
import std.sync.*

const defaultCapacity: Int64 = 32

/**
 * This class Provides the ability to read data from console
 */
public class ConsoleReader <: InputStream {
    private static let mutex: Mutex = Mutex()
    private var buffer: Array<Byte> = Array<Byte>(defaultCapacity, repeat: 0) /* stdin read buffer */
    private var endIndex: Int64 = 0
    private var startIndex: Int64 = 0
    init() {}

    /**
     * Read one char
     * @throws IllegalMemoryException if there are some system errors.
     */
    @OverflowWrapping
    public func read(): ?Rune {
        synchronized(mutex) {
            if (endIndex == startIndex && !readFromConsole()) {
                return None
            }

            let (rune, size): (Rune, Int64) = Rune.fromUtf8(buffer, startIndex)
            startIndex += size
            return rune
        }
    }

    /** Read one line */
    public func readln(): ?String {
        if (let Some(line) <- readUntil(r'\n')) {
            // not including CR character
            let size = line.size
            if (line.endsWith("\r\n")) {
                return line[..size - 2]
            } else if (line.endsWith('\n')) {
                return line[..size - 1]
            }
            return line
        }

        return None
    }

    /** Read all line */
    public func readToEnd(): ?String {
        return readUntil({_ => false})
    }

    /**
     * Reads until some contents is encountered, or the end of the stream is reached.
     * ch is included in the result
     */
    public func readUntil(ch: Rune): ?String {
        return readUntil({it => it == ch})
    }

    /**
     * Reads until predicate is encountered, or the end of the stream is reached.
     * ch is included in the result
     */
    public func readUntil(predicate: (Rune) -> Bool): ?String {
        let result: StringBuilder = StringBuilder()
        synchronized(mutex) {
            while (true) {
                if (startIndex >= endIndex && !readFromConsole()) {
                    break
                }

                if (let Some(byteOffset) <- findFirstOf(predicate)) {
                    unsafe { result.appendFromUtf8Unchecked(buffer[startIndex..byteOffset]) }
                    startIndex = byteOffset
                    break
                }

                unsafe { result.appendFromUtf8Unchecked(buffer[startIndex..endIndex]) }
                startIndex = this.endIndex
            }
        }

        return if (result.size == 0) {
            None
        } else {
            result.toString()
        }
    }

    /**
     * Reads input through the line-reading path until arr is full or the line
     * reader reports end of input or a read failure.
     * Use readSome for redirected input that must not wait for a complete line.
     */
    public func read(arr: Array<Byte>): Int64 {
        let arraySize = arr.size
        synchronized(mutex) {
            while (arraySize > endIndex - startIndex) {
                if (!readFromConsole()) {
                    buffer.copyTo(arr, startIndex, 0, endIndex - startIndex)
                    let readSize = endIndex - startIndex
                    startIndex = 0
                    endIndex = 0
                    return readSize
                }
            }
            buffer.copyTo(arr, startIndex, 0, arraySize)
            startIndex += arraySize
        }
        return arraySize
    }

    /**
     * Reads bytes from standard input without additional reads to fill arr.
     * The operation may block and is subject to the terminal input mode.
     * Returns 0 for an empty array; read failures may also produce a return value of 0.
     */
    public func readSome(arr: Array<Byte>): Int64 {
        let arraySize = arr.size
        if (arraySize == 0) {
            return 0
        }

        var readSize: Int64 = 0
        synchronized(mutex) {
            if (endIndex > startIndex) {
                readSize = copyBufferedBytes(arr)
            } else {
                readSize = readAvailableBytes(arr)
            }
        }
        return readSize
    }

    @When[os == "Windows"]
    private func readAvailableBytes(arr: Array<Byte>): Int64 {
        if (unsafe { CJ_CONSOLE_IsStdinCharDevice() }) {
            if (!readFromConsole()) {
                return 0
            }
            return copyBufferedBytes(arr)
        }
        return directRead(arr)
    }

    @When[os != "Windows"]
    private func readAvailableBytes(arr: Array<Byte>): Int64 {
        directRead(arr)
    }

    private func copyBufferedBytes(arr: Array<Byte>): Int64 {
        let readSize = min(arr.size, endIndex - startIndex)
        buffer.copyTo(arr, startIndex, 0, readSize)
        startIndex += readSize
        if (startIndex == endIndex) {
            startIndex = 0
            endIndex = 0
        }
        return readSize
    }

    private func directRead(arr: Array<Byte>): Int64 {
        let readSize = unsafe {
            let data: CPointerHandle<Byte> = acquireArrayRawData(arr)
            let result = CJ_CONSOLE_ReadBytes(data.pointer, UIntNative(arr.size))
            releaseArrayRawData(data)
            result
        }
        return if (readSize < 0) {
            0
        } else {
            readSize
        }
    }

    private func readFromConsole(): Bool {
        let line: CPointer<StructureString> = unsafe { CJ_CONSOLE_Readline() }
        if (line.isNull()) {
            unsafe { CJ_CONSOLE_ClearstdInerr() }
            return false
        }

        unsafe {
            let ss = line.read()
            try {
                this.append(ss)
            } finally {
                LibC.free(ss.buffer)
                LibC.free(line)
            }
        }

        return true
    }

    /*
     * @returns utf8 position
     */
    private func findFirstOf(predicate: (Rune) -> Bool): ?Int64 {
        var byteOffset = startIndex
        while (byteOffset < endIndex) {
            let (char, len): (Rune, Int64) = Rune.fromUtf8(buffer, byteOffset)
            byteOffset += len
            if (predicate(char)) {
                return byteOffset
            }
        }
        return None
    }

    @OverflowWrapping
    unsafe func append(str: StructureString): Unit {
        let itemLen = Int64(str.length)
        this.reserve(itemLen)

        if (itemLen < 32) {
            for (i in 0..itemLen) {
                this.buffer[i + endIndex] = str.buffer.read(i)
            }
            this.endIndex += itemLen
            return
        }

        let data: CPointerHandle<UInt8> = acquireArrayRawData(this.buffer)
        let rc = memcpy_s(data.pointer + endIndex, UIntNative(buffer.size - endIndex), str.buffer,
            UIntNative(itemLen))
        releaseArrayRawData(data)
        if (rc != 0) {
            // never reaches here
            throw IllegalMemoryException("Failed to copy memory in console reader: error code '${rc}'.")
        }
        this.endIndex += itemLen
    }

    @OverflowThrowing
    private func reserve(addition: Int64): Unit {
        // |---------|------|---------|
        // 0       start   end     size-1
        let rightRemain = this.buffer.size - this.endIndex

        // if have enough space, do nothing
        if (rightRemain >= addition) {
            return
        }

        // if move data to head can make space, move data to head
        if (this.startIndex >= addition - rightRemain) {
            this.buffer.copyTo(this.buffer, startIndex, 0, this.endIndex - startIndex)
            this.endIndex -= startIndex
            this.startIndex = 0
            return
        }

        this.grow(this.buffer.size + addition)
    }

    @OverflowWrapping
    private func grow(minCapacity: Int64): Unit {
        let oldCapacity = this.buffer.size
        var newCapacity = oldCapacity + (oldCapacity >> 1)
        if (newCapacity < minCapacity) {
            newCapacity = minCapacity
        }

        var itemData = Array<UInt8>(newCapacity, repeat: 0)
        this.buffer.copyTo(itemData, this.startIndex, 0, this.endIndex - this.startIndex)
        this.buffer = itemData
        this.endIndex = this.endIndex - this.startIndex
        this.startIndex = 0
    }
}
