/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
 * This source file is part of the Cangjie project, licensed under Apache-2.0
 * with Runtime Library Exception.
 *
 * See https://cangjie-lang.cn/pages/LICENSE for license information.
 */

package std.core

/**
 * Represents an element in a stack trace, containing class, method, file, and line information.
 */
public open class StackTraceElement <: ToString {
    /**
     * The declaring class name of the stack trace element.
     *
     * @type String
     */
    public let declaringClass: String

    /**
     * The method name of the stack trace element.
     *
     * @type String
     */
    public let methodName: String

    /**
     * The source file name of the stack trace element.
     *
     * @type String
     */
    public let fileName: String

    /**
     * The line number of the stack trace element.
     *
     * @type Int64
     */
    public let lineNumber: Int64

    /**
     * Constructs a StackTraceElement with the specified class, method, file, and line number.
     *
     * @param declaringClass the declaring class name
     * @param methodName the method name
     * @param fileName the source file name
     * @param lineNumber the line number
     */
    public init(declaringClass: String, methodName: String, fileName: String, lineNumber: Int64) {
        this.declaringClass = declaringClass
        this.methodName = methodName
        this.fileName = fileName
        this.lineNumber = lineNumber
    }

    /**
     * Converts the call point information to a string representation.
     *
     * @returns String a formatted string containing class name, method name, file name and line number information
     */
    public func toString(): String {
        if (declaringClass.isEmpty() && methodName.isEmpty()) {
                return("${fileName}:${lineNumber}")
        } else {
            let packageDelimiter: String = if (declaringClass.isEmpty()) { "" } else { "." }
            return(
                "${declaringClass}${packageDelimiter}${methodName}(${fileName}:${lineNumber})")
        }
    }
}

// Workaround to avoid llvm back-end crash while compiling several `decodeStackTrace` instrinsic invocations
let decodeStackTraceWrapper = { pcArray: RawArray<UInt64>, j: Int64 =>
    return decodeStackTrace(
        arrayGet(pcArray, j * 3), arrayGet(pcArray, j * 3 + 1), arrayGet(pcArray, j * 3 + 2))
}

/**
 * Returns the raw stack trace of the current thread.
 *
 * @returns Array<UInt64> the raw stack trace data
 */
protected func getRawStackTrace(): Array<UInt64> {
    let stackTrace = fillInStackTrace(RawArray<UInt64>(1, repeat: 0), String.empty.toArray().rawptr)
    return Array(stackTrace, 0, arraySize(stackTrace))
}

/**
 * Decodes a raw stack trace element at the given frame index.
 *
 * @param rawStackTrace the raw stack trace data
 * @param frameIndex the frame index to decode
 * @returns Option<StackTraceElement> the decoded StackTraceElement, or None if the frame is invalid
 */
protected func decodeRawStackTrace(rawStackTrace: Array<UInt64>, frameIndex: Int64): Option<StackTraceElement> {
    let data = decodeStackTraceWrapper(rawStackTrace.rawptr, frameIndex)

    if ((data.lineNumber == 0 && arraySize(data.fileName) == 0) || data.lineNumber == -1) {
        return Option<StackTraceElement>.None
    }
    return StackTraceElement(
        String(data.className), String(data.methodName), String(data.fileName), data.lineNumber)
}
