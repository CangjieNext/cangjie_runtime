/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
 * This source file is part of the Cangjie project, licensed under Apache-2.0
 * with Runtime Library Exception.
 *
 * See https://cangjie-lang.cn/pages/LICENSE for license information.
 */

package std.core

// Intended to be thrown if an attempt is made to access or update a non-existing member
public class ExternMemberAccessException <: Exception {
    public ExternMemberAccessException(message: String) {
        super(message)
    }
}

// Intended to be thrown if an attempt is made to access a non-existing function
public class ExternFunctionAccessException <: Exception {
    public ExternFunctionAccessException(message: String) {
        super(message)
    }
}

// Intended to be thrown if an attempt is made to call a function with the wrong arguments
public class ExternFunctionCallException <: Exception {
    public ExternFunctionCallException(message: String) {
        super(message)
    }
}

// Intended to be thrown if an attempt is made to access or update with the wrong index, including out-of-bounds errors
public class ExternIndexedAccessException <: Exception {
    public ExternIndexedAccessException(message: String) {
        super(message)
    }
}

// Intended to be thrown if a function in the foreign runtime throws a runtime exception
public class ForeignRuntimeException <: Exception {
    public ForeignRuntimeException(message: String) {
        super(message)
    }
}

// Intended to be thrown if data cannot be converted to or from an Extern type as required.
public class ExternConversionException <: Exception {
    public ExternConversionException(message: String) {
        super(message)
    }
}

public interface ForeignRuntime<T> where T <: ForeignRuntime<T> {
    static func memberAccess(e: Extern<T>, field: String): Extern<T>
    static func indexedAccess(e: Extern<T>, arg: Any): Extern<T>

    static func memberUpdate(e: Extern<T>, field: String, value: Any) : Unit
    static func indexedUpdate(e: Extern<T>, field: Any, value: Any): Unit

    static func functionCall(e: Extern<T>, args: Array<Any>): Extern<T>

    static func fromExtern<R>(h: Extern<T>): R
    static func toExtern<R>(v: R): Extern<T>
}

// Reads the `payload` field of an `Extern<T>` value. Lowered by the compiler to a direct field
// read, bypassing the dynamic `Extern` member-access desugaring.
@Intrinsic
func getExtern<T>(e: Extern<T>): Any where T <: ForeignRuntime<T>

// Writes the `payload` field of an `Extern<T>` value. Lowered by the compiler to a direct field
// write, bypassing the dynamic `Extern` member-update desugaring.
@Intrinsic
func setExtern<T>(e: Extern<T>, payload: Any): Unit where T <: ForeignRuntime<T>

public struct Extern<T> where T <: ForeignRuntime<T> {
    // `payload` is given a default so that `this` is considered fully initialised before the
    // constructor body runs; `setExtern` then writes the real payload through the intrinsic field
    // write (not a dynamic `Extern` member update).
    private var payload: Any = ()

    // the property `Extern<T>.getPayload(Extern<T>(v)) = v` holds for any value `v: Any`, and `T <: ForeignRuntime<T>`
    public Extern(payload: Any) {
        setExtern(this, payload)
    }
    public static func getPayload(e: Extern<T>): Any {
        getExtern(e)
    }
}