/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
 * This source file is part of the Cangjie project, licensed under Apache-2.0
 * with Runtime Library Exception.
 *
 * See https://cangjie-lang.cn/pages/LICENSE for license information.
 */

package std.core

// Provide the function that bitcast between two kinds of primitive types.
// It's implemented by Cangjie compiler and is backend-independent.
@Intrinsic
func bitCast<From, To>(val: From): To where From <: BitCastRange, To <: BitCastRange

extend Float64 {
    /**
     * Converts from an IEEE 754 binary representation to a Float64 value.
     *
     * @param bits the IEEE 754 binary representation of the value
     * @returns Float64 the Float64 value
     */
    public static func fromBits(bits: UInt64): Float64 {
        bitCast(bits)
    }

    /**
     * Converts this Float64 value to its IEEE 754 binary representation.
     *
     * @returns UInt64 the IEEE 754 binary representation
     */
    public func toBits(): UInt64 {
        bitCast(this)
    }
}

extend Float32 {
    /**
     * Converts from an IEEE 754 binary representation to a Float32 value.
     *
     * @param bits the IEEE 754 binary representation of the value
     * @returns Float32 the Float32 value
     */
    public static func fromBits(bits: UInt32): Float32 {
        bitCast(bits)
    }

    /**
     * Converts this Float32 value to its IEEE 754 binary representation.
     *
     * @returns UInt32 the IEEE 754 binary representation
     */
    public func toBits(): UInt32 {
        bitCast(this)
    }
}

extend Float16 {
    /**
     * Converts from an IEEE 754 binary representation to a Float16 value.
     *
     * @param bits the IEEE 754 binary representation of the value
     * @returns Float16 the Float16 value
     */
    public static func fromBits(bits: UInt16): Float16 {
        bitCast(bits)
    }

    /**
     * Converts this Float16 value to its IEEE 754 binary representation.
     *
     * @returns UInt16 the IEEE 754 binary representation
     */
    public func toBits(): UInt16 {
        bitCast(this)
    }
}

// Indicates the types can be used in function bitCast.
interface BitCastRange {}

extend UInt64 <: BitCastRange {}

extend Float64 <: BitCastRange {}

extend UInt32 <: BitCastRange {}

extend Float32 <: BitCastRange {}

extend UInt16 <: BitCastRange {}

extend Float16 <: BitCastRange {}
