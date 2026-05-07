/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
 * This source file is part of the Cangjie project, licensed under Apache-2.0
 * with Runtime Library Exception.
 *
 * See https://cangjie-lang.cn/pages/LICENSE for license information.
 */

package std.core

/**
 * The base class for all errors in Cangjie.
 * Errors represent serious unrecoverable problems, as opposed to Exceptions which represent recoverable conditions.
 */
public open class Error <: ToString {
    private var detailMessage: String = String.empty
    var pcArray: RawArray<UInt64> = RawArray<UInt64>(1, repeat: 0)
    var traceElementArray: Array<StackTraceElement> = []

    init() {
        pcArray = fillInStackTrace(RawArray<UInt64>(1, repeat: 0), detailMessage.toArray().rawptr)
    }

    /**
     * @param message the detail message for this error
     */
    init(message: String) {
        detailMessage = message
        pcArray = fillInStackTrace(RawArray<UInt64>(1, repeat: 0), detailMessage.toArray().rawptr)
    }

    protected open func getClassName(): String {
        return "Error"
    }

    /**
     * Returns the detail message of this error.
     *
     * @type String
     */
    public open prop message: String {
        get() {
            return detailMessage
        }
    }

    /**
     * Returns the string representation of this error.
     *
     * @returns String the string representation
     */
    public open func toString(): String {
        let className: String = getClassName()
        let message: String = this.message
        if (message.isEmpty()) {
            return className
        }
        return "${className}: ${message}"
    }

    /**
     * Prints the stack trace of this error to standard error.
     */
    public open func printStackTrace(): Unit {
        eprintln("An exception has occurred: ${getStackTraceMessage()}")
    }

    /**
     * Returns the stack trace message of this error as a string.
     *
     * @returns String the formatted stack trace message
     */
    public open func getStackTraceMessage(): String {
        let message = StringBuilder()
        message.append(toString() + "\n")

        traceElementArray = getStackTrace()
        for (element in traceElementArray) {
            var packageDelimiter: String = if (element.declaringClass.isEmpty()) { "" } else { "." }
            message.append(
                "\t at ${element.declaringClass}${packageDelimiter}${element.methodName}(${element.fileName}:${element.lineNumber})\n")
        }

        return message.toString()
    }

    /**
     * Returns the stack trace elements of this error.
     *
     * @returns Array<StackTraceElement> the stack trace elements
     */
    public func getStackTrace(): Array<StackTraceElement> {
        getStackTraceImpl(pcArray)
    }
}
