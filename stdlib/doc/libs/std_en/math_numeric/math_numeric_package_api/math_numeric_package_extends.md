# Extensions

## extend BigInt <: Formattable

```cangjie
extend BigInt <: Formattable
```

Function: Extends the [Formattable](../../convert/convert_package_api/convert_package_interfaces.md#interface-formattable) interface for [BigInt](math_numeric_package_structs.md#struct-bigint) to enable formatting of [BigInt](math_numeric_package_structs.md#struct-bigint) instances into strings.

Extended type:

- [BigInt](math_numeric_package_structs.md#struct-bigint)

Extended interfaces:

- Formattable

### func format(String)

```cangjie
public func format(fmt: String): String
```

Function: Formats the current [BigInt](math_numeric_package_structs.md#struct-bigint) instance into a string according to the specified format parameters.

Parameters:

- fmt: [String](../../core/core_package_api/core_package_structs.md#struct-string) - Formatting parameters.

Returns:

- [String](../../core/core_package_api/core_package_structs.md#struct-string) - The formatted string representation of the current [BigInt](math_numeric_package_structs.md#struct-bigint) instance.

Exceptions:

- [IllegalArgumentException](../../core/core_package_api/core_package_exceptions.md#class-illegalargumentexception) - Thrown when the fmt parameter is invalid.

## extend BigInt <: Integer\<BigInt>

```cangjie
extend BigInt <: Integer<BigInt>
```

Function: Extends the [Integer\<T>](../../math/math_package_api/math_package_interfaces.md#interface-integert) interface for the [BigInt](math_numeric_package_structs.md#struct-bigint) type.

Extended type:

- [BigInt](math_numeric_package_structs.md#struct-bigint)

Extended interfaces:

- Integer\<BigInt>

### static func isSigned()

```cangjie
public static func isSigned(): Bool
```

Function: Determines whether the [BigInt](math_numeric_package_structs.md#struct-bigint) type is signed.

Returns:

- [Bool](../../core/core_package_api/core_package_intrinsics.md#bool) - Always returns `true`.

## extend BigInt <: Number\<BigInt>

```cangjie
extend BigInt <: Number<BigInt>
```

Function: Extends the [Number\<T>](../../math/math_package_api/math_package_interfaces.md#interface-numbert) interface for the [BigInt](math_numeric_package_structs.md#struct-bigint) type.

Extended type:

- [BigInt](math_numeric_package_structs.md#struct-bigint)

Extended interfaces:

- Number\<BigInt>

## extend BigInt <: Parsable\<BigInt>

```cangjie
extend BigInt <: Parsable<BigInt>
```

Function: This extension primarily implements operations for converting string literals of the [BigInt](math_numeric_package_structs.md#struct-bigint) type into [BigInt](math_numeric_package_structs.md#struct-bigint) structs.

Extended type:

- [BigInt](math_numeric_package_structs.md#struct-bigint)

Extended interfaces:

- Parsable\<BigInt>

### static func parse(String)

```cangjie
public static func parse(value: String): BigInt
```

Function: Parses a string into a [BigInt](math_numeric_package_structs.md#struct-bigint) struct.

The string follows these rules: an optional sign (positive or negative) at the beginning, followed by a base prefix, and then a sequence of digits:

IntegerString : SignString? BaseString? ValueString

- SignString : + | -

- BaseString : "0b" | "0B" | "0o" | "0O" | "0x" | "0X" | ""

- ValueString : Digits

  - Digits: Digit | Digit Digits

    - Digit : '0' ~ '9' | 'A' ~ 'Z' | 'a' ~ 'z'

      - If the base prefix is "0b" or "0B", Digit must be in the range '0' ~ '1';

      - If the base prefix is "0o" or "0O", Digit must be in the range '0' ~ '7';

      - If the base prefix is "0x" or "0X", Digit must be in the range '0' ~ '9', 'a' ~ 'z', or 'A' ~ 'Z';

      - If the base prefix is empty, Digit must be in the range '0' ~ '9'.

Parameters:

- value: [String](../../core/core_package_api/core_package_structs.md#struct-string) - The string used to construct the [BigInt](math_numeric_package_structs.md#struct-bigint) struct. The string rules are: an optional positive (+) or negative (-) sign at the beginning, followed by an optional base prefix (default is decimal). Use "0b" or "0B" for binary, "0o" or "0O" for octal, and "0x" or "0X" for hexadecimal. This is followed by a mandatory non-empty sequence of Arabic numerals or Latin letters (case-insensitive, where 'a' and 'A' equal decimal 10, 'b' and 'B' equal decimal 11, etc.). The characters in the sequence must comply with the character set requirements of the corresponding base.

Returns:

- [BigInt](math_numeric_package_structs.md#struct-bigint) - The parsed [BigInt](math_numeric_package_structs.md#struct-bigint) struct.

Exceptions:

- [IllegalArgumentException](../../core/core_package_api/core_package_exceptions.md#class-illegalargumentexception) - Thrown if the string `value` does not conform to the above rules.

### static func tryParse(String)

```cangjie
public static func tryParse(value: String): ?BigInt
```

Function: Attempts to parse a string into a [BigInt](math_numeric_package_structs.md#struct-bigint) struct.

The string follows these rules: an optional sign (positive or negative) at the beginning, followed by a base prefix, and then a sequence of digits:

IntegerString : SignString? BaseString? ValueString

- SignString : + | -

- BaseString : "0b" | "0B" | "0o" | "0O" | "0x" | "0X" | ""

- ValueString : Digits

  - Digits: Digit | Digit Digits

    - Digit : '0' ~ '9' | 'A' ~ 'Z' | 'a' ~ 'z'

      - If the base prefix is "0b" or "0B", Digit must be in the range '0' ~ '1';

      - If the base prefix is "0o" or "0O", Digit must be in the range '0' ~ '7';

      - If the base prefix is "0x" or "0X", Digit must be in the range '0' ~ '9', 'a' ~ 'z', or 'A' ~ 'Z';

      - If the base prefix is empty, Digit must be in the range '0' ~ '9'.

Parameters:

- value: [String](../../core/core_package_api/core_package_structs.md#struct-string) - The string used to construct the [BigInt](math_numeric_package_structs.md#struct-bigint) struct. The string rules are: an optional positive (+) or negative (-) sign at the beginning, followed by an optional base prefix (default is decimal). Use "0b" or "0B" for binary, "0o" or "0O" for octal, and "0x" or "0X" for hexadecimal. This is followed by a mandatory non-empty sequence of Arabic numerals or Latin letters (case-insensitive, where 'a' and 'A' equal decimal 10, 'b' and 'B' equal decimal 11, etc.). The characters in the sequence must comply with the character set requirements of the corresponding base.

Returns:

- ?[BigInt](math_numeric_package_structs.md#struct-bigint) - The parsed [BigInt](math_numeric_package_structs.md#struct-bigint) struct, or `None` if parsing fails.

## extend BigInt <: RadixConvertible\<BigInt>

```cangjie
extend BigInt <: RadixConvertible<BigInt>
```

Function: This extension primarily implements operations for converting string literals of the [BigInt](math_numeric_package_structs.md#struct-bigint) type into [BigInt](math_numeric_package_structs.md#struct-bigint) structs.

Extended type:

- [BigInt](math_numeric_package_structs.md#struct-bigint)

Extended interfaces:

- RadixConvertible\<BigInt>

### static func parse(String, Int64)

```cangjie
public static func parse(value: String, radix!: Int64): BigInt
```

Function: Parses a string into a [BigInt](math_numeric_package_structs.md#struct-bigint) struct based on the specified radix, supporting bases from 2 to 36.

The string follows these rules: an optional sign (positive or negative) at the beginning, followed by a sequence of digits:

IntegerString : SignString? ValueString

- SignString : + | -

- ValueString : Digits

  - Digits: Digit | Digit Digits

    - Digit : '0' ~ '9' | 'A' ~ 'Z' | 'a' ~ 'z'

      - If Digit is in '0' ~ '9', it must satisfy (Digit - '0') < radix;

      - If Digit is in 'A' ~ 'Z', it must satisfy (Digit - 'A') + 10 < radix;

      - If Digit is in 'a' ~ 'z', it must satisfy (Digit - 'A') + 10 < radix.

Parameters:

- value: [String](../../core/core_package_api/core_package_structs.md#struct-string) - The string used to construct the [BigInt](math_numeric_package_structs.md#struct-bigint) struct. The string rules are: an optional positive (+) or negative (-) sign at the beginning, followed by a mandatory non-empty sequence of Arabic numerals or Latin letters (case-insensitive, where 'a' and 'A' equal decimal 10, 'b' and 'B' equal decimal 11, etc.). The value of each character must not exceed the radix.
- radix!: [Int64](../../core/core_package_api/core_package_intrinsics.md#int64) - The radix. The base of the string representation, ranging from [2, 36].

Returns:

- [BigInt](math_numeric_package_structs.md#struct-bigint) - The parsed [BigInt](math_numeric_package_structs.md#struct-bigint) struct.Exceptions:

- [IllegalArgumentException](../../core/core_package_api/core_package_exceptions.md#class-illegalargumentexception) - Thrown if the string `value` does not conform to the specified rules, or if the `radix` is not within the range [2, 36].

### static func tryParse(String, Int64)

```cangjie
public static func tryParse(value: String, radix!: Int64): ?BigInt
```

Function: Attempts to parse a string into a [BigInt](math_numeric_package_structs.md#struct-bigint) structure based on the specified radix, supporting bases from 2 to 36.

The string must adhere to the following rules, starting with an optional sign (positive or negative) followed by a sequence of digits:

IntegerString : SignString? ValueString

- SignString : + | -

- ValueString : Digits

  - Digits: Digit | Digit Digits

    - Digit : '0' ~ '9' | 'A' ~ 'Z' | 'a' ~ 'z'

      - If Digit is within '0' ~ '9', it must satisfy (Digit - '0') < radix;

      - If Digit is within 'A' ~ 'Z', it must satisfy (Digit - 'A') + 10 < radix;

      - If Digit is within 'a' ~ 'z', it must satisfy (Digit - 'A') + 10 < radix.

Parameters:

- value: [String](../../core/core_package_api/core_package_structs.md#struct-string) - The string used to construct the [BigInt](math_numeric_package_structs.md#struct-bigint) structure. The string must start with an optional positive (+) or negative (-) sign, followed by a non-empty sequence of Arabic numerals or Latin letters (case-insensitive). The letters 'a' and 'A' represent the decimal value 10, 'b' and 'B' represent 11, and so on. The value of each character in the sequence must be less than the radix.
- radix!: [Int64](../../core/core_package_api/core_package_intrinsics.md#int64) - The radix (base) of the string representation, which must be within the range [2, 36].

Returns:

- ?[BigInt](math_numeric_package_structs.md#struct-bigint) - The parsed [BigInt](math_numeric_package_structs.md#struct-bigint) structure, or `None` if parsing fails.

### func toString(Int64)

```cangjie
public func toString(radix!: Int64): String
```

Function: Computes and returns the string representation of this [BigInt](math_numeric_package_structs.md#struct-bigint) in the specified radix.

Parameters:

- radix!: [Int64](../../core/core_package_api/core_package_intrinsics.md#int64) - The radix (base) for the string representation, which must be within the range [2, 36].

Returns:

- [String](../../core/core_package_api/core_package_structs.md#struct-string) - The string representation of this [BigInt](math_numeric_package_structs.md#struct-bigint) in the specified `radix`.

Exceptions:

- [IllegalArgumentException](../../core/core_package_api/core_package_exceptions.md#class-illegalargumentexception) - Thrown if the input `radix` is not within the range [2, 36].

Example:
<!-- verify -->
```cangjie
import std.math.numeric.BigInt

main() {
    let bigInt = BigInt(0x400)
    let toString = bigInt.toString(radix: 2)
    println(toString)
}
```

Output:

```text
10000000000
```

## extend Decimal <: Formattable

```cangjie
extend Decimal <: Formattable
```

Function: Extends the [Decimal](math_numeric_package_structs.md#struct-decimal) type with the [Formattable](../../convert/convert_package_api/convert_package_interfaces.md#interface-formattable) interface to enable formatting [Decimal](math_numeric_package_structs.md#struct-decimal) instances into strings.

Extended type:

- [Decimal](math_numeric_package_structs.md#struct-decimal)

Extended interfaces:

- Formattable

### func format(String)

```cangjie
public func format(fmt: String): String
```

Function: Formats the current [Decimal](math_numeric_package_structs.md#struct-decimal) instance into a string according to the specified format parameters.

Parameters:

- fmt: [String](../../core/core_package_api/core_package_structs.md#struct-string) - Formatting parameters.

Returns:

- [String](../../core/core_package_api/core_package_structs.md#struct-string) - The formatted string representation of the current [Decimal](math_numeric_package_structs.md#struct-decimal) instance.

Exceptions:

- [IllegalArgumentException](../../core/core_package_api/core_package_exceptions.md#class-illegalargumentexception) - Thrown when the format parameter `fmt` is invalid.

## extend Decimal <: Number\<Decimal>

```cangjie
extend Decimal <: Number<Decimal>
```

Function: Extends the [Decimal](math_numeric_package_structs.md#struct-decimal) type with the [Number\<T>](../../math/math_package_api/math_package_interfaces.md#interface-numbert) interface.

Extended type:

- [Decimal](math_numeric_package_structs.md#struct-decimal)

Extended interfaces:

- Number\<Decimal>

## extend Decimal <: Parsable\<Decimal>

```cangjie
extend Decimal <: Parsable<Decimal>
```

Function: This extension primarily implements operations for converting string literals of the [Decimal](math_numeric_package_structs.md#struct-decimal) type into [Decimal](math_numeric_package_structs.md#struct-decimal) struct instances.

Extended type:

- [Decimal](math_numeric_package_structs.md#struct-decimal)

Extended interfaces:

- Parsable\<Decimal>

### static func parse(String)

```cangjie
public static func parse(value: String): Decimal
```

Function: Constructs a [Decimal](math_numeric_package_structs.md#struct-decimal) struct from a formatted string. By default, it uses a precision value of 0 (infinite precision). The string must adhere to the following format: an optional sign (positive or negative) at the beginning, followed by a ValueString, and an optional ExponentString:

[Decimal](math_numeric_package_structs.md#struct-decimal) String: SignString? ValueString ExponentString?

- SignString: + | -

- ValueString: IntegerPart.(FractionPart)? | .FractionPart | IntegerPart

  - IntegerPart: Digits

  - FractionPart: Digits

  - Digits: Digit | Digit Digits

    - Digit: '0' ~ '9'

- ExponentString: ExponentIndicator (SignString)? IntegerPart

  - ExponentIndicator: e | E

Parameters:

- value: [String](../../core/core_package_api/core_package_structs.md#struct-string) - The formatted string.

Returns:

- [Decimal](math_numeric_package_structs.md#struct-decimal) - The parsed [Decimal](math_numeric_package_structs.md#struct-decimal) struct.

Exceptions:

- [IllegalArgumentException](../../core/core_package_api/core_package_exceptions.md#class-illegalargumentexception) - Thrown when the input string does not conform to the specified format.
- [OverflowException](../../core/core_package_api/core_package_exceptions.md#class-overflowexception) - Thrown when the scale of the constructed value overflows.

### static func tryParse(String)

```cangjie
public static func tryParse(value: String): ?Decimal
```

Function: Attempts to construct a [Decimal](math_numeric_package_structs.md#struct-decimal) struct from a formatted string. By default, it uses a precision value of 0 (infinite precision). The string must adhere to the following format: an optional sign (positive or negative) at the beginning, followed by a ValueString, and an optional ExponentString:

[Decimal](math_numeric_package_structs.md#struct-decimal) String: SignString? ValueString ExponentString?

- SignString: + | -

- ValueString: IntegerPart.(FractionPart)? | .FractionPart | IntegerPart

  - IntegerPart: Digits

  - FractionPart: Digits

  - Digits: Digit | Digit Digits

    - Digit: '0' ~ '9'

- ExponentString: ExponentIndicator (SignString)? IntegerPart

  - ExponentIndicator: e | E

Parameters:

- value: [String](../../core/core_package_api/core_package_structs.md#struct-string) - The formatted string.

Returns:

- ?[Decimal](math_numeric_package_structs.md#struct-decimal) - The parsed [Decimal](math_numeric_package_structs.md#struct-decimal) struct, or `None` if parsing fails.

