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
 * Returns the magnitude of a Float64 value without changing its numeric type.
 * The sign bit is removed from finite inputs, so -0.0 becomes +0.0.
 * For NaN, the result is NaN. For either infinity, the result is positive infinity.
 *
 * @param x some value.
 * @return the absolute value of @p x.
 *
 * @since 0.17.4
 */
@Frozen
public func abs(x: Float64): Float64 {
    intrinsicFabs<Float64>(x)
}

/**
 * Computes the absolute value for a Float32 argument.
 * The sign bit is cleared for finite inputs, including negative zero.
 * NaN stays NaN, and infinite input produces positive infinity.
 *
 * @param x some value.
 * @return the absolute value of @p x.
 *
 * @since 0.17.4
 */
@Frozen
public func abs(x: Float32): Float32 {
    intrinsicFabs<Float32>(x)
}

/**
 * Gets the absolute value of a Float16 argument.
 * Finite inputs are returned with a positive sign, including -0.0.
 * NaN remains NaN, and infinity is normalized to positive infinity.
 *
 * @param x some value.
 * @return the absolute value of @p x.
 *
 * @since 0.17.4
 */
@Frozen
public func abs(x: Float16): Float16 {
    intrinsicFabs<Float16>(x)
}

/**
 * Returns the absolute value of an Int64 number.
 * Non-negative input is returned directly, and negative input is negated.
 * Int64.Min cannot be represented as a positive Int64 and causes an exception.
 *
 * @param x some value.
 * @return the absolute value of @p x.
 *
 * @since 0.17.4
 *
 * @throws OverflowException if the argument `x` is the minimum value of Int64
 */
@Frozen
public func abs(x: Int64): Int64 {
    if (x == Int64.Min) {
        throw OverflowException("The absolute value of the minimum Int64 value is out of range.")
    }
    intrinsicAbs<Int64>(x)
}

/**
 * Computes the absolute value of an Int32 number.
 * Values greater than or equal to zero are preserved, while negative values are
 * converted to their positive counterpart.
 * Int32.Min overflows the result range.
 *
 * @param x some value.
 * @return the absolute value of @p x.
 *
 * @since 0.17.4
 *
 * @throws OverflowException if the argument `x` is the minimum value of Int32
 */
@Frozen
public func abs(x: Int32): Int32 {
    if (x == Int32.Min) {
        throw OverflowException("The absolute value of the minimum Int32 value is out of range.")
    }
    intrinsicAbs<Int32>(x)
}

/**
 * Gets the absolute value of an Int16 number.
 * A non-negative argument is returned as is. A negative argument is negated unless
 * it is Int16.Min, whose positive value is out of range.
 *
 * @param x some value.
 * @return the absolute value of @p x.
 *
 * @since 0.17.4
 *
 * @throws OverflowException if the argument `x` is the min value of Int16
 */
@Frozen
public func abs(x: Int16): Int16 {
    if (x == Int16.Min) {
        throw OverflowException("The absolute value of the minimum Int16 value is out of range.")
    }
    intrinsicAbs<Int16>(x)
}

/**
 * Returns the magnitude of an Int8 value.
 * The minimum Int8 value has no representable positive counterpart and therefore
 * throws OverflowException.
 *
 * @param x some value.
 * @return the absolute value of @p x.
 *
 * @since 0.17.4
 *
 * @throws OverflowException if the argument `x` is the minimum value of Int8
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
 * A negative argument is rejected with IllegalArgumentException.
 * NaN remains NaN, and positive infinity is returned as positive infinity.
 *
 * @param x some value.
 * @return the square root of @p x.
 *
 * @since 0.17.4
 *
 * @throws IllegalArgumentException if the argument `x` is negative
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
 * Computes the Float32 square root of the argument.
 * Negative input is invalid and raises IllegalArgumentException.
 * The special values NaN and positive infinity are propagated.
 *
 * @param x some value.
 * @return the square root of @p x.
 *
 * @since 0.17.4
 *
 * @throws IllegalArgumentException if the argument `x` is negative
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
 * Computes the principal square root of a Float64 value.
 * Values below zero are rejected with IllegalArgumentException.
 * NaN is returned as NaN, and positive infinity is returned unchanged.
 *
 * @param x some value.
 * @return the square root of @p x.
 *
 * @since 0.17.4
 *
 * @throws IllegalArgumentException if the argument `x` is negative
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
 * Raises Euler's number to a Float64 exponent.
 * NaN is returned as NaN; positive infinity returns positive infinity; negative
 * infinity returns zero.
 *
 * @param x some value.
 * @return \f$ e^{x} \f$.
 *
 * @since 0.17.4
 */
@Frozen
public func exp(x: Float64): Float64 {
    return intrinsicExp<Float64>(x)
}

/**
 * Evaluates the natural exponential function for a Float32 argument.
 * NaN propagates as NaN, and the two infinite inputs return positive infinity
 * and zero respectively.
 *
 * @param x some value.
 * @return \f$ e^{x} \f$.
 *
 * @since 0.17.4
 */
@Frozen
public func exp(x: Float32): Float32 {
    return intrinsicExp<Float32>(x)
}

/**
 * Computes e raised to the Float16 argument.
 * The result is NaN for NaN, positive infinity for positive infinity, and zero
 * for negative infinity.
 *
 * @param x some value.
 * @return \f$ e^{x} \f$.
 *
 * @since 0.17.4
 */
@Frozen
public func exp(x: Float16): Float16 {
    return intrinsicExp<Float16>(x)
}

/**
 * Raises 2 to a Float64 exponent.
 * NaN remains NaN, positive infinity produces positive infinity, and negative
 * infinity produces zero.
 *
 * @param x some value.
 * @return \f$ 2^{x} \f$.
 *
 * @since 0.17.4
 */
@Frozen
public func exp2(x: Float64): Float64 {
    return intrinsicExp2<Float64>(x)
}

/**
 * Evaluates 2 to the power of a Float32 value.
 * NaN is propagated. Positive infinity and negative infinity return positive
 * infinity and zero respectively.
 *
 * @param x some value.
 * @return \f$ 2^{x} \f$.
 *
 * @since 0.17.4
 */
@Frozen
public func exp2(x: Float32): Float32 {
    return intrinsicExp2<Float32>(x)
}

/**
 * Computes 2 raised to the Float16 argument.
 * NaN, positive infinity, and negative infinity map to NaN, positive infinity,
 * and zero respectively.
 *
 * @param x some value.
 * @return \f$ 2^{x} \f$.
 *
 * @since 0.17.4
 */
@Frozen
public func exp2(x: Float16): Float16 {
    return intrinsicExp2<Float16>(x)
}

/**
 * Returns the base 10 logarithm for a Float64 value.
 * NaN and negative values return NaN, positive infinity returns positive infinity,
 * and either signed zero returns negative infinity.
 *
 * @param x some value.
 * @return base 10 logarithm of @p x.
 *
 * @since 0.17.4
 */
@Frozen
public func log10(x: Float64): Float64 {
    return intrinsicLog10<Float64>(x)
}

/**
 * Computes the decimal logarithm of a Float32 value.
 * The result is NaN for NaN or negative input, positive infinity for positive
 * infinity, and negative infinity for positive or negative zero.
 *
 * @param x some value.
 * @return base 10 logarithm of @p x.
 *
 * @since 0.17.4
 */
@Frozen
public func log10(x: Float32): Float32 {
    return intrinsicLog10<Float32>(x)
}

/**
 * Gets the base 10 logarithm of a Float16 value.
 * NaN and values below zero produce NaN. Positive infinity is preserved, and
 * both signed zeros produce negative infinity.
 *
 * @param x some value.
 * @return base 10 logarithm of @p x.
 *
 * @since 0.17.4
 */
@Frozen
public func log10(x: Float16): Float16 {
    return intrinsicLog10<Float16>(x)
}

/**
 * Returns the base 2 logarithm for a Float64 value.
 * NaN or negative input returns NaN. Positive infinity returns positive infinity,
 * and positive or negative zero returns negative infinity.
 *
 * @param x some value.
 * @return base 2 logarithm of @p x.
 *
 * @since 0.17.4
 */
@Frozen
public func log2(x: Float64): Float64 {
    return intrinsicLog2<Float64>(x)
}

/**
 * Computes the binary logarithm of a Float32 value.
 * NaN and negative arguments produce NaN; positive infinity and signed zero
 * produce positive infinity and negative infinity respectively.
 *
 * @param x some value.
 * @return base 2 logarithm of @p x.
 *
 * @since 0.17.4
 */
@Frozen
public func log2(x: Float32): Float32 {
    return intrinsicLog2<Float32>(x)
}

/**
 * Gets the base 2 logarithm of a Float16 argument.
 * NaN or a value less than zero returns NaN, positive infinity is preserved, and
 * both signed zeros return negative infinity.
 *
 * @param x some value.
 * @return base 2 logarithm of @p x.
 *
 * @since 0.17.4
 */
@Frozen
public func log2(x: Float16): Float16 {
    return intrinsicLog2<Float16>(x)
}

/**
 * Returns the natural logarithm of a Float64 value.
 * NaN and negative values return NaN, positive infinity returns positive infinity,
 * and either signed zero returns negative infinity.
 *
 * @param x some value.
 * @return \f$ \ln{x} \f$.
 *
 * @since 0.17.4
 */
@Frozen
public func log(x: Float64): Float64 {
    return intrinsicLog<Float64>(x)
}

/**
 * Computes the natural logarithm of a Float32 value.
 * The result is NaN for NaN or negative input, positive infinity for positive
 * infinity, and negative infinity for positive or negative zero.
 *
 * @param x some value.
 * @return \f$ \ln{x} \f$.
 *
 * @since 0.17.4
 */
@Frozen
public func log(x: Float32): Float32 {
    return intrinsicLog<Float32>(x)
}

/**
 * Gets the natural logarithm of a Float16 value.
 * NaN and values below zero produce NaN. Positive infinity is preserved, and
 * both signed zeros produce negative infinity.
 *
 * @param x some value.
 * @return \f$ \ln{x} \f$.
 *
 * @since 0.17.4
 */
@Frozen
public func log(x: Float16): Float16 {
    return intrinsicLog<Float16>(x)
}

/**
 * Rounds a Float64 value downward to the nearest mathematical integer value.
 * The returned floating-point value is less than or equal to @p x.
 *
 * @param x some value.
 * @return the rounded-down Float64 result.
 *
 * @since 0.17.4
 */
@Frozen
public func floor(x: Float64): Float64 {
    return intrinsicFloor<Float64>(x)
}

/**
 * Returns the greatest Float32 integer value that is not greater than @p x.
 * The result keeps the Float32 type.
 *
 * @param x some value.
 * @return the floor of @p x as Float32.
 *
 * @since 0.17.4
 */
@Frozen
public func floor(x: Float32): Float32 {
    return intrinsicFloor<Float32>(x)
}

/**
 * Computes the floor of a Float16 value.
 * The result is the nearest mathematical integer value toward negative infinity.
 *
 * @param x some value.
 * @return the floor of @p x as Float16.
 *
 * @since 0.17.4
 */
@Frozen
public func floor(x: Float16): Float16 {
    intrinsicFloor(x)
}

/**
 * Rounds a Float64 value upward to the nearest mathematical integer value.
 * The returned floating-point value is greater than or equal to @p x.
 *
 * @param x some value.
 * @return the rounded-up Float64 result.
 *
 * @since 0.17.4
 */
@Frozen
public func ceil(x: Float64): Float64 {
    return intrinsicCeil(x)
}

/**
 * Returns the least Float32 integer value that is not less than @p x.
 * The result keeps the Float32 type.
 *
 * @param x some value.
 * @return the ceiling of @p x as Float32.
 *
 * @since 0.17.4
 */
@Frozen
public func ceil(x: Float32): Float32 {
    return intrinsicCeil(x)
}

/**
 * Computes the ceiling of a Float16 value.
 * The result is the nearest mathematical integer value toward positive infinity.
 *
 * @param x some value.
 * @return the ceiling of @p x as Float16.
 *
 * @since 0.17.4
 */
@Frozen
public func ceil(x: Float16): Float16 {
    return intrinsicCeil(x)
}

/**
 * Truncates a Float64 value toward zero.
 *
 * @param x some value.
 * @return the Float64 value with the fractional part removed.
 *
 * @since 0.17.4
 */
@Frozen
public func trunc(x: Float64): Float64 {
    return intrinsicTrunc(x)
}

/**
 * Removes the fractional part of a Float32 value by rounding toward zero.
 *
 * @param x some value.
 * @return the truncated Float32 result.
 *
 * @since 0.17.4
 */
@Frozen
public func trunc(x: Float32): Float32 {
    return intrinsicTrunc(x)
}

/**
 * Computes the Float16 value obtained by discarding the fractional part.
 *
 * @param x some value.
 * @return the truncated Float16 result.
 *
 * @since 0.17.4
 */
@Frozen
public func trunc(x: Float16): Float16 {
    return intrinsicTrunc(x)
}

/********************* Trigonometric functions *********************/

/**
 * Returns the sine of a Float64 angle measured in radians.
 *
 * @param x an angle, in radians.
 * @return the Float64 sine of @p x.
 *
 * @since 0.17.4
 */
@Frozen
public func sin(x: Float64): Float64 {
    return intrinsicSin<Float64>(x)
}

/**
 * Computes the trigonometric sine for a Float32 radian value.
 *
 * @param x an angle, in radians.
 * @return the Float32 sine of @p x.
 *
 * @since 0.17.4
 */
@Frozen
public func sin(x: Float32): Float32 {
    return intrinsicSin<Float32>(x)
}

/**
 * Gets the sine of a Float16 angle in radians.
 *
 * @param x an angle, in radians.
 * @return the Float16 sine of @p x.
 *
 * @since 0.17.4
 */
@Frozen
public func sin(x: Float16): Float16 {
    return intrinsicSin<Float16>(x)
}

/**
 * Returns the cosine of a Float64 angle measured in radians.
 *
 * @param x an angle, in radians.
 * @return the Float64 cosine of @p x.
 *
 * @since 0.17.4
 */
@Frozen
public func cos(x: Float64): Float64 {
    return intrinsicCos<Float64>(x)
}

/**
 * Computes the trigonometric cosine for a Float32 radian value.
 *
 * @param x an angle, in radians.
 * @return the Float32 cosine of @p x.
 *
 * @since 0.17.4
 */
@Frozen
public func cos(x: Float32): Float32 {
    return intrinsicCos<Float32>(x)
}

/**
 * Gets the cosine of a Float16 angle in radians.
 *
 * @param x an angle, in radians.
 * @return the Float16 cosine of @p x.
 *
 * @since 0.17.4
 */
@Frozen
public func cos(x: Float16): Float16 {
    return intrinsicCos<Float16>(x)
}

/**
 * Raises a Float64 base to a Float64 exponent.
 * @param base the Float64 base.
 * @param exponent the Float64 exponent.
 * @return \f$ {base}^{exponent} \f$ as Float64.
 *
 * @since 0.17.4
 */
@Frozen
public func pow(base: Float64, exponent: Float64): Float64 {
    return intrinsicPow<Float64>(base, exponent)
}

/**
 * Computes a Float32 base raised to a Float32 exponent.
 * @param base the Float32 base.
 * @param exponent the Float32 exponent.
 * @return \f$ {base}^{exponent} \f$ as Float32.
 *
 * @since 0.17.4
 */
@Frozen
public func pow(base: Float32, exponent: Float32): Float32 {
    return intrinsicPow<Float32>(base, exponent)
}

/**
 * Raises a Float64 base to an Int64 exponent.
 * Note: Due to the lack of support from LLVM, Int64 calls cffi while Int32 can use intrinsicPowi.
 * @param base the Float64 base.
 * @param exponent the Int64 exponent.
 * @return \f$ {base}^{exponent} \f$ as Float64.
 *
 * @since 0.17.4
 */
@Frozen
public func pow(base: Float64, exponent: Int64): Float64 {
    unsafe { return CJ_CORE_FastPowerDoubleInt64(base, exponent) }
}

/**
 * Computes a Float32 base raised to an Int32 exponent.
 * @param base the Float32 base.
 * @param exponent the Int32 exponent.
 * @return \f$ {base}^{exponent} \f$ as Float32.
 *
 * @since 0.17.4
 */
@Frozen
public func pow(base: Float32, exponent: Int32): Float32 {
    return intrinsicPowi(base, exponent)
}
