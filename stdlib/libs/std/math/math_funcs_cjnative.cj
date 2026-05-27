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
 * This is a library for math class.
 */

package std.math

/********************* utility functions *********************/
/**
 * Returns the absolute value of a Float64 value.
 *
 * @param x the value whose absolute value is calculated.
 * @return @p x without a negative sign. NaN is returned as NaN.
 */
@Frozen
public func abs(x: Float64): Float64 {
    intrinsicFabs<Float64>(x)
}

/**
 * Returns the absolute value of a Float32 value.
 *
 * @param x the value whose absolute value is calculated.
 * @return @p x without a negative sign. NaN is returned as NaN.
 */
@Frozen
public func abs(x: Float32): Float32 {
    intrinsicFabs<Float32>(x)
}

/**
 * Returns the absolute value of a Float16 value.
 *
 * @param x the value whose absolute value is calculated.
 * @return @p x without a negative sign. NaN is returned as NaN.
 */
@Frozen
public func abs(x: Float16): Float16 {
    intrinsicFabs<Float16>(x)
}

/**
 * Returns the absolute value of an Int64 value.
 *
 * @param x the integer value to convert.
 * @return @p x when it is non-negative, otherwise `-x`.
 * @throws OverflowException if @p x is Int64.Min.
 */
@Frozen
public func abs(x: Int64): Int64 {
    if (x == Int64.Min) {
        throw OverflowException("The absolute value of the minimum Int64 value is out of range.")
    }
    intrinsicAbs<Int64>(x)
}

/**
 * Returns the absolute value of an Int32 value.
 *
 * @param x the integer value to convert.
 * @return @p x when it is non-negative, otherwise `-x`.
 * @throws OverflowException if @p x is Int32.Min.
 */
@Frozen
public func abs(x: Int32): Int32 {
    if (x == Int32.Min) {
        throw OverflowException("The absolute value of the minimum Int32 value is out of range.")
    }
    intrinsicAbs<Int32>(x)
}

/**
 * Returns the absolute value of an Int16 value.
 *
 * @param x the integer value to convert.
 * @return @p x when it is non-negative, otherwise `-x`.
 * @throws OverflowException if @p x is Int16.Min.
 */
@Frozen
public func abs(x: Int16): Int16 {
    if (x == Int16.Min) {
        throw OverflowException("The absolute value of the minimum Int16 value is out of range.")
    }
    intrinsicAbs<Int16>(x)
}

/**
 * Returns the absolute value of an Int8 value.
 *
 * @param x the integer value to convert.
 * @return @p x when it is non-negative, otherwise `-x`.
 * @throws OverflowException if @p x is Int8.Min.
 */
@Frozen
public func abs(x: Int8): Int8 {
    if (x == Int8.Min) {
        throw OverflowException("The absolute value of the minimum Int8 value is out of range.")
    }
    intrinsicAbs<Int8>(x)
}

/**
 * Returns the square root of a Float16 value.
 *
 * @param x the value whose square root is calculated.
 * @return the non-negative square root of @p x.
 * @throws IllegalArgumentException if @p x is less than 0.0.
 */
@Frozen
public func sqrt(x: Float16): Float16 {
    if (x < 0.0) {
        throw IllegalArgumentException(
            "The input parameter must be greater than or equal to 0.0. Please enter a correct parameter.")
    }
    return intrinsicSqrt<Float16>(x)
}

/**
 * Returns the square root of a Float32 value.
 *
 * @param x the value whose square root is calculated.
 * @return the non-negative square root of @p x.
 * @throws IllegalArgumentException if @p x is less than 0.0.
 */
@Frozen
public func sqrt(x: Float32): Float32 {
    if (x < 0.0) {
        throw IllegalArgumentException(
            "The input parameter must be greater than or equal to 0.0. Please enter a correct parameter.")
    }
    return intrinsicSqrt<Float32>(x)
}

/**
 * Returns the square root of a Float64 value.
 *
 * @param x the value whose square root is calculated.
 * @return the non-negative square root of @p x.
 * @throws IllegalArgumentException if @p x is less than 0.0.
 */
@Frozen
public func sqrt(x: Float64): Float64 {
    if (x < 0.0) {
        throw IllegalArgumentException(
            "The input parameter must be greater than or equal to 0.0. Please enter a correct parameter.")
    }
    return intrinsicSqrt<Float64>(x)
}

/**
 * Returns e raised to a Float64 power.
 *
 * @param x the exponent.
 * @return the Float64 value \f$ e^x \f$.
 */
@Frozen
public func exp(x: Float64): Float64 {
    return intrinsicExp<Float64>(x)
}

/**
 * Returns e raised to a Float32 power.
 *
 * @param x the exponent.
 * @return the Float32 value \f$ e^x \f$.
 */
@Frozen
public func exp(x: Float32): Float32 {
    return intrinsicExp<Float32>(x)
}

/**
 * Returns e raised to a Float16 power.
 *
 * @param x the exponent.
 * @return the Float16 value \f$ e^x \f$.
 */
@Frozen
public func exp(x: Float16): Float16 {
    return intrinsicExp<Float16>(x)
}

/**
 * Returns 2 raised to a Float64 power.
 *
 * @param x the exponent.
 * @return the Float64 value \f$ 2^x \f$.
 */
@Frozen
public func exp2(x: Float64): Float64 {
    return intrinsicExp2<Float64>(x)
}

/**
 * Returns 2 raised to a Float32 power.
 *
 * @param x the exponent.
 * @return the Float32 value \f$ 2^x \f$.
 */
@Frozen
public func exp2(x: Float32): Float32 {
    return intrinsicExp2<Float32>(x)
}

/**
 * Returns 2 raised to a Float16 power.
 *
 * @param x the exponent.
 * @return the Float16 value \f$ 2^x \f$.
 */
@Frozen
public func exp2(x: Float16): Float16 {
    return intrinsicExp2<Float16>(x)
}

/**
 * Returns the base 10 logarithm of a Float64 value.
 *
 * @param x the value whose logarithm is calculated.
 * @return the Float64 logarithm of @p x with base 10, following floating-point special-value rules.
 */
@Frozen
public func log10(x: Float64): Float64 {
    return intrinsicLog10<Float64>(x)
}

/**
 * Returns the base 10 logarithm of a Float32 value.
 *
 * @param x the value whose logarithm is calculated.
 * @return the Float32 logarithm of @p x with base 10, following floating-point special-value rules.
 */
@Frozen
public func log10(x: Float32): Float32 {
    return intrinsicLog10<Float32>(x)
}

/**
 * Returns the base 10 logarithm of a Float16 value.
 *
 * @param x the value whose logarithm is calculated.
 * @return the Float16 logarithm of @p x with base 10, following floating-point special-value rules.
 */
@Frozen
public func log10(x: Float16): Float16 {
    return intrinsicLog10<Float16>(x)
}

/**
 * Returns the base 2 logarithm of a Float64 value.
 *
 * @param x the value whose logarithm is calculated.
 * @return the Float64 logarithm of @p x with base 2, following floating-point special-value rules.
 */
@Frozen
public func log2(x: Float64): Float64 {
    return intrinsicLog2<Float64>(x)
}

/**
 * Returns the base 2 logarithm of a Float32 value.
 *
 * @param x the value whose logarithm is calculated.
 * @return the Float32 logarithm of @p x with base 2, following floating-point special-value rules.
 */
@Frozen
public func log2(x: Float32): Float32 {
    return intrinsicLog2<Float32>(x)
}

/**
 * Returns the base 2 logarithm of a Float16 value.
 *
 * @param x the value whose logarithm is calculated.
 * @return the Float16 logarithm of @p x with base 2, following floating-point special-value rules.
 */
@Frozen
public func log2(x: Float16): Float16 {
    return intrinsicLog2<Float16>(x)
}

/**
 * Returns the natural logarithm of a Float64 value.
 *
 * @param x the value whose logarithm is calculated.
 * @return the Float64 logarithm of @p x with base e, following floating-point special-value rules.
 */
@Frozen
public func log(x: Float64): Float64 {
    return intrinsicLog<Float64>(x)
}

/**
 * Returns the natural logarithm of a Float32 value.
 *
 * @param x the value whose logarithm is calculated.
 * @return the Float32 logarithm of @p x with base e, following floating-point special-value rules.
 */
@Frozen
public func log(x: Float32): Float32 {
    return intrinsicLog<Float32>(x)
}

/**
 * Returns the natural logarithm of a Float16 value.
 *
 * @param x the value whose logarithm is calculated.
 * @return the Float16 logarithm of @p x with base e, following floating-point special-value rules.
 */
@Frozen
public func log(x: Float16): Float16 {
    return intrinsicLog<Float16>(x)
}

/**
 * Returns the largest Float64 integer value not greater than @p x.
 *
 * @param x the value to round downward.
 * @return the floor of @p x.
 */
@Frozen
public func floor(x: Float64): Float64 {
    return intrinsicFloor<Float64>(x)
}

/**
 * Returns the largest Float32 integer value not greater than @p x.
 *
 * @param x the value to round downward.
 * @return the floor of @p x.
 */
@Frozen
public func floor(x: Float32): Float32 {
    return intrinsicFloor<Float32>(x)
}

/**
 * Returns the largest Float16 integer value not greater than @p x.
 *
 * @param x the value to round downward.
 * @return the floor of @p x.
 */
@Frozen
public func floor(x: Float16): Float16 {
    intrinsicFloor(x)
}

/**
 * Returns the smallest Float64 integer value not less than @p x.
 *
 * @param x the value to round upward.
 * @return the ceiling of @p x.
 */
@Frozen
public func ceil(x: Float64): Float64 {
    return intrinsicCeil(x)
}

/**
 * Returns the smallest Float32 integer value not less than @p x.
 *
 * @param x the value to round upward.
 * @return the ceiling of @p x.
 */
@Frozen
public func ceil(x: Float32): Float32 {
    return intrinsicCeil(x)
}

/**
 * Returns the smallest Float16 integer value not less than @p x.
 *
 * @param x the value to round upward.
 * @return the ceiling of @p x.
 */
@Frozen
public func ceil(x: Float16): Float16 {
    return intrinsicCeil(x)
}

/**
 * Truncates a Float64 value toward zero.
 *
 * @param x the value to truncate.
 * @return the integral Float64 part of @p x, rounded toward zero.
 */
@Frozen
public func trunc(x: Float64): Float64 {
    return intrinsicTrunc(x)
}

/**
 * Truncates a Float32 value toward zero.
 *
 * @param x the value to truncate.
 * @return the integral Float32 part of @p x, rounded toward zero.
 */
@Frozen
public func trunc(x: Float32): Float32 {
    return intrinsicTrunc(x)
}

/**
 * Truncates a Float16 value toward zero.
 *
 * @param x the value to truncate.
 * @return the integral Float16 part of @p x, rounded toward zero.
 */
@Frozen
public func trunc(x: Float16): Float16 {
    return intrinsicTrunc(x)
}

/********************* Trigonometric functions *********************/

/**
 * Returns the trigonometric sine of a Float64 angle.
 *
 * @param x an angle in radians.
 * @return the sine of @p x.
 */
@Frozen
public func sin(x: Float64): Float64 {
    return intrinsicSin<Float64>(x)
}

/**
 * Returns the trigonometric sine of a Float32 angle.
 *
 * @param x an angle in radians.
 * @return the sine of @p x.
 */
@Frozen
public func sin(x: Float32): Float32 {
    return intrinsicSin<Float32>(x)
}

/**
 * Returns the trigonometric sine of a Float16 angle.
 *
 * @param x an angle in radians.
 * @return the sine of @p x.
 */
@Frozen
public func sin(x: Float16): Float16 {
    return intrinsicSin<Float16>(x)
}

/**
 * Returns the trigonometric cosine of a Float64 angle.
 *
 * @param x an angle in radians.
 * @return the cosine of @p x.
 */
@Frozen
public func cos(x: Float64): Float64 {
    return intrinsicCos<Float64>(x)
}

/**
 * Returns the trigonometric cosine of a Float32 angle.
 *
 * @param x an angle in radians.
 * @return the cosine of @p x.
 */
@Frozen
public func cos(x: Float32): Float32 {
    return intrinsicCos<Float32>(x)
}

/**
 * Returns the trigonometric cosine of a Float16 angle.
 *
 * @param x an angle in radians.
 * @return the cosine of @p x.
 */
@Frozen
public func cos(x: Float16): Float16 {
    return intrinsicCos<Float16>(x)
}

/**
 * Returns a Float64 base raised to a Float64 exponent.
 *
 * @param base the base value.
 * @param exponent the exponent value.
 * @return \f$ {base}^{exponent} \f$ as Float64.
 */
@Frozen
public func pow(base: Float64, exponent: Float64): Float64 {
    return intrinsicPow<Float64>(base, exponent)
}

/**
 * Returns a Float32 base raised to a Float32 exponent.
 *
 * @param base the base value.
 * @param exponent the exponent value.
 * @return \f$ {base}^{exponent} \f$ as Float32.
 */
@Frozen
public func pow(base: Float32, exponent: Float32): Float32 {
    return intrinsicPow<Float32>(base, exponent)
}

/**
 * Returns the value of the first argument raised to the power of the second argument.
 * Note: Due to the lack of support from LLVM, Int64 calls cffi while Int32 can use intrinsicPowi.
 * @param base the base.
 * @param exponent the exponent.
 * @return \f$ {base}^{exponent} \f$.
 *
 * @since 0.17.4
 */
@Frozen
public func pow(base: Float64, exponent: Int64): Float64 {
    unsafe { return CJ_CORE_FastPowerDoubleInt64(base, exponent) }
}

/**
 * Returns a Float32 base raised to an Int32 exponent.
 *
 * @param base the base value.
 * @param exponent the integer exponent.
 * @return \f$ {base}^{exponent} \f$ as Float32.
 */
@Frozen
public func pow(base: Float32, exponent: Int32): Float32 {
    return intrinsicPowi(base, exponent)
}
