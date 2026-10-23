# std.math.numeric

## Feature Overview

The math.numeric package provides extended capabilities beyond the expressible range of basic types.

For example:

1. Supports big integers (BigInt);
2. Supports high-precision decimal numbers (Decimal type);
3. Provides common mathematical operations including high-precision arithmetic rules.

## API List

### Functions

|              Function Name          |            Functionality           |
| ----------------------------------- | ----------------------------------- |
| [abs(BigInt)](./math_numeric_package_api/math_numeric_package_funcs.md#func-absbigint)| Computes the absolute value of a `BigInt`. |
| [abs(Decimal)](./math_numeric_package_api/math_numeric_package_funcs.md#func-absdecimal)| Computes the absolute value of a `Decimal`. |
| [countOne(BigInt) <sup>(deprecated)</sup>](./math_numeric_package_api/math_numeric_package_funcs.md#func-countonebigint-deprecated) | Calculates and returns the number of 1's in the two's complement binary representation of the input `BigInt`. |
| [countOnes(BigInt)](./math_numeric_package_api/math_numeric_package_funcs.md#func-countonesbigint) | Calculates and returns the number of 1's in the two's complement binary representation of the input `BigInt`. |
| [gcd(BigInt, BigInt)](./math_numeric_package_api/math_numeric_package_funcs.md#func-gcdbigint-bigint)| Computes the greatest common divisor of two `BigInt`s. Always returns a non-negative number (equivalent to the GCD of absolute values). |
| [lcm(BigInt, BigInt)](./math_numeric_package_api/math_numeric_package_funcs.md#func-lcmbigint-bigint) | Computes the least common multiple of two `BigInt`s. Returns 0 when either input is 0; otherwise always returns a positive number (equivalent to the LCM of absolute values). |
| [round(Decimal, RoundingMode)](./math_numeric_package_api/math_numeric_package_funcs.md#func-rounddecimal-roundingmode) | Rounds a `Decimal` value to the nearest integer according to the specified rounding mode. |
| [sqrt(BigInt)](./math_numeric_package_api/math_numeric_package_funcs.md#func-sqrtbigint)| Computes the integer square root of a `BigInt`, rounded down. |
| [sqrt(Decimal)](./math_numeric_package_api/math_numeric_package_funcs.md#func-sqrtdecimal)| Computes the square root of a `Decimal`. For infinite decimal results, the default uses IEEE 754-2019 decimal128 rounding. |
| [trailingZeros(BigInt)](./math_numeric_package_api/math_numeric_package_funcs.md#func-trailingzerosbigint) | Counts the number of consecutive least significant 0 bits in the binary representation of a `BigInt`. Returns 0 if the least significant bit is not 0. |

### Enums

|                 Enum              |                Functionality                |
| --------------------------------- | ------------------------------------------ |
| [OverflowStrategy](./math_numeric_package_api/math_numeric_package_enums.md#enum-overflowstrategy) | Overflow strategy enum class containing 3 overflow strategies. When converting `BigInt` or `Decimal` types to integer types, different overflow handling strategies can be specified. |

### Structs

|                Struct              |                Functionality                |
| ---------------------------------- | ------------------------------------------ |
| [BigInt](./math_numeric_package_api/math_numeric_package_structs.md#struct-bigint) | BigInt is defined as a signed integer of arbitrary precision (binary). The Cangjie struct BigInt is used for arbitrary-precision signed integer calculations and type conversions. |
| [Decimal](./math_numeric_package_api/math_numeric_package_structs.md#struct-decimal) | Decimal represents a signed decimal number of arbitrary precision. Allows specifying context during operations, result precision, and rounding rules. Provides conversion capabilities between basic types (Int, UInt, String, Float, etc.) and BigInt type, supports querying basic properties of Decimal objects, basic mathematical operations, and provides fundamental capabilities like object comparison, hashing, and string printing. |

### Extensions

| Extension Name | Functionality |
| --- | --- |
| [extend BigInt <: Formattable](math_numeric_package_api/math_numeric_package_extends.md#extend-bigint--formattable) | Extends the [Formattable](../convert/convert_package_api/convert_package_interfaces.md#interface-formattable) interface for [BigInt](math_numeric_package_api/math_numeric_package_structs.md#struct-bigint) to enable formatting of [BigInt](math_numeric_package_api/math_numeric_package_structs.md#struct-bigint) instances into strings. |
| [extend BigInt <: Integer\<BigInt>](math_numeric_package_api/math_numeric_package_extends.md#extend-bigint--integerbigint) | Extends the [Integer\<T>](../math/math_package_api/math_package_interfaces.md#interface-integert) interface for the [BigInt](math_numeric_package_api/math_numeric_package_structs.md#struct-bigint) type. |
| [extend BigInt <: Number\<BigInt>](math_numeric_package_api/math_numeric_package_extends.md#extend-bigint--numberbigint) | Extends the [Number\<T>](../math/math_package_api/math_package_interfaces.md#interface-numbert) interface for the [BigInt](math_numeric_package_api/math_numeric_package_structs.md#struct-bigint) type. |
| [extend BigInt <: Parsable\<BigInt>](math_numeric_package_api/math_numeric_package_extends.md#extend-bigint--parsablebigint) | This extension primarily implements operations for converting string literals of the [BigInt](math_numeric_package_api/math_numeric_package_structs.md#struct-bigint) type into [BigInt](math_numeric_package_api/math_numeric_package_structs.md#struct-bigint) structs. |
| [extend BigInt <: RadixConvertible\<BigInt>](math_numeric_package_api/math_numeric_package_extends.md#extend-bigint--radixconvertiblebigint) | This extension primarily implements operations for converting string literals of the [BigInt](math_numeric_package_api/math_numeric_package_structs.md#struct-bigint) type into [BigInt](math_numeric_package_api/math_numeric_package_structs.md#struct-bigint) structs. |
| [extend Decimal <: Formattable](math_numeric_package_api/math_numeric_package_extends.md#extend-decimal--formattable) | Extends the [Decimal](math_numeric_package_api/math_numeric_package_structs.md#struct-decimal) type with the [Formattable](../convert/convert_package_api/convert_package_interfaces.md#interface-formattable) interface to enable formatting [Decimal](math_numeric_package_api/math_numeric_package_structs.md#struct-decimal) instances into strings. |
| [extend Decimal <: Number\<Decimal>](math_numeric_package_api/math_numeric_package_extends.md#extend-decimal--numberdecimal) | Extends the [Decimal](math_numeric_package_api/math_numeric_package_structs.md#struct-decimal) type with the [Number\<T>](../math/math_package_api/math_package_interfaces.md#interface-numbert) interface. |
| [extend Decimal <: Parsable\<Decimal>](math_numeric_package_api/math_numeric_package_extends.md#extend-decimal--parsabledecimal) | This extension primarily implements operations for converting string literals of the [Decimal](math_numeric_package_api/math_numeric_package_structs.md#struct-decimal) type into [Decimal](math_numeric_package_api/math_numeric_package_structs.md#struct-decimal) struct instances. |
