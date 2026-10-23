# Interfaces

## interface Formattable

```cangjie
public interface Formattable
```

Function: This interface defines a formatting function that converts an instance of the specified type into a string of corresponding format based on formatting parameters.

For detailed descriptions of formatting parameters, please refer to the [Function Introduction](./../convert_package_overview.md#功能介绍) in the convert package.

Other types can provide formatting functionality by implementing this interface.

Extensions:

- [extend Float16 <: Formattable](convert_package_extends.md#extend-float16--formattable) (cross-package extension)
- [extend Float32 <: Formattable](convert_package_extends.md#extend-float32--formattable) (cross-package extension)
- [extend Float64 <: Formattable](convert_package_extends.md#extend-float64--formattable) (cross-package extension)
- [extend Int16 <: Formattable](convert_package_extends.md#extend-int16--formattable) (cross-package extension)
- [extend Int32 <: Formattable](convert_package_extends.md#extend-int32--formattable) (cross-package extension)
- [extend Int64 <: Formattable](convert_package_extends.md#extend-int64--formattable) (cross-package extension)
- [extend Int8 <: Formattable](convert_package_extends.md#extend-int8--formattable) (cross-package extension)
- [extend Rune <: Formattable](convert_package_extends.md#extend-rune--formattable) (cross-package extension)
- [extend UInt16 <: Formattable](convert_package_extends.md#extend-uint16--formattable) (cross-package extension)
- [extend UInt32 <: Formattable](convert_package_extends.md#extend-uint32--formattable) (cross-package extension)
- [extend UInt64 <: Formattable](convert_package_extends.md#extend-uint64--formattable) (cross-package extension)
- [extend UInt8 <: Formattable](convert_package_extends.md#extend-uint8--formattable) (cross-package extension)

### func format(String)

```cangjie
func format(fmt: String): String
```

Function: Formats the current instance into a string of corresponding format based on formatting parameters.

Parameters:

- fmt: [String](../../core/core_package_api/core_package_structs.md#struct-string) - Formatting parameters.

Returns:

- [String](../../core/core_package_api/core_package_structs.md#struct-string) - The formatted string obtained from the current instance.

## interface Parsable\<T>

```cangjie
public interface Parsable<T>
```

Function: This interface provides unified methods to support parsing strings into specific types.

The interface provides two sets of methods: parse and tryParse. The parse method throws an exception on parsing failure, while tryParse wraps the return value in [Option](../../core/core_package_api/core_package_enums.md#enum-optiont) and returns None on failure.
This package already implements this interface for basic types such as [Bool](../../core/core_package_api/core_package_intrinsics.md#bool), [Rune](../../core/core_package_api/core_package_intrinsics.md#rune), [Float16](../../core/core_package_api/core_package_intrinsics.md#float16), and [Int64](../../core/core_package_api/core_package_intrinsics.md#int64), enabling string conversion to these types.

Extensions:

- [extend Bool <: Parsable\<Bool>](convert_package_extends.md#extend-bool--parsablebool) (cross-package extension)
- [extend Float16 <: Parsable\<Float16>](convert_package_extends.md#extend-float16--parsablefloat16) (cross-package extension)
- [extend Float32 <: Parsable\<Float32>](convert_package_extends.md#extend-float32--parsablefloat32) (cross-package extension)
- [extend Float64 <: Parsable\<Float64>](convert_package_extends.md#extend-float64--parsablefloat64) (cross-package extension)
- [extend Int16 <: Parsable\<Int16>](convert_package_extends.md#extend-int16--parsableint16) (cross-package extension)
- [extend Int32 <: Parsable\<Int32>](convert_package_extends.md#extend-int32--parsableint32) (cross-package extension)
- [extend Int64 <: Parsable\<Int64>](convert_package_extends.md#extend-int64--parsableint64) (cross-package extension)
- [extend Int8 <: Parsable\<Int8>](convert_package_extends.md#extend-int8--parsableint8) (cross-package extension)
- [extend Rune <: Parsable\<Rune>](convert_package_extends.md#extend-rune--parsablerune) (cross-package extension)
- [extend UInt16 <: Parsable\<UInt16>](convert_package_extends.md#extend-uint16--parsableuint16) (cross-package extension)
- [extend UInt32 <: Parsable\<UInt32>](convert_package_extends.md#extend-uint32--parsableuint32) (cross-package extension)
- [extend UInt64 <: Parsable\<UInt64>](convert_package_extends.md#extend-uint64--parsableuint64) (cross-package extension)
- [extend UInt8 <: Parsable\<UInt8>](convert_package_extends.md#extend-uint8--parsableuint8) (cross-package extension)

### static func parse(String)

```cangjie
static func parse(value: String): T
```

Function: Parses a specific type from a string.

Parameters:

- value: [String](../../core/core_package_api/core_package_structs.md#struct-string) - The string to be parsed.

Returns:

- T - The converted value.

### static func tryParse(String)

```cangjie
static func tryParse(value: String): Option<T>
```

Function: Parses a specific type from a string.

Parameters:

- value: [String](../../core/core_package_api/core_package_structs.md#struct-string) - The string to be parsed.

Returns:

- [Option](../../core/core_package_api/core_package_enums.md#enum-optiont)\<T> - The converted value, or [Option](../../core/core_package_api/core_package_enums.md#enum-optiont)\<T>.None if conversion fails.

## interface RadixConvertible\<T>

```cangjie
public interface RadixConvertible<T>
```

Function: This interface provides unified methods to support parsing strings of specified radix into specific types.

The radix is specified via parameters rather than string prefixes, with a valid range of 2-36. Values beyond decimal are represented using uppercase or lowercase English letters in alphabetical order (10 digits + 26 letters = base-36). For Int series strings, they may start with `+` or `-`; if neither is present, they are treated as `+`. UInt series strings may start with `+`, but will throw an exception if starting with `-`. Returns the string representation in specified radix, with values beyond decimal represented using lowercase English letters (10 digits + 26 lowercase letters = base-36).

This interface provides two sets of methods: parse and tryParse. The parse method throws exceptions on parsing failures, while tryParse wraps the return value in [Option](../../core/core_package_api/core_package_enums.md#enum-optiont), returning None on failure.
The package has already implemented this interface for basic types like [Int64](../../core/core_package_api/core_package_intrinsics.md#int64) and [UInt64](../../core/core_package_api/core_package_intrinsics.md#uint64), enabling string conversion to these types.

Extensions:

- [extend Int16 <: RadixConvertible\<Int16>](convert_package_extends.md#extend-int16--radixconvertibleint16) (cross-package extension)
- [extend Int32 <: RadixConvertible\<Int32>](convert_package_extends.md#extend-int32--radixconvertibleint32) (cross-package extension)
- [extend Int64 <: RadixConvertible\<Int64>](convert_package_extends.md#extend-int64--radixconvertibleint64) (cross-package extension)
- [extend Int8 <: RadixConvertible\<Int8>](convert_package_extends.md#extend-int8--radixconvertibleint8) (cross-package extension)
- [extend UInt16 <: RadixConvertible\<UInt16>](convert_package_extends.md#extend-uint16--radixconvertibleuint16) (cross-package extension)
- [extend UInt32 <: RadixConvertible\<UInt32>](convert_package_extends.md#extend-uint32--radixconvertibleuint32) (cross-package extension)
- [extend UInt64 <: RadixConvertible\<UInt64>](convert_package_extends.md#extend-uint64--radixconvertibleuint64) (cross-package extension)
- [extend UInt8 <: RadixConvertible\<UInt8>](convert_package_extends.md#extend-uint8--radixconvertibleuint8) (cross-package extension)

### static func parse(String, Int64)

```cangjie
static func parse(value: String, radix!: Int64): T
```

Function: Parses a specific type from a string of specified radix.

Parameters:

- value: [String](../../core/core_package_api/core_package_structs.md#struct-string) - The string to be parsed.
- radix!: [Int64](../../core/core_package_api/core_package_intrinsics.md#int64) - The specified radix.

Returns:

- T - The converted value.

### static func tryParse(String, Int64)

```cangjie
static func tryParse(value: String, radix!: Int64): Option<T>
```

Function: Parses a specific type from a string of specified radix.

Parameters:

- value: [String](../../core/core_package_api/core_package_structs.md#struct-string) - The string to be parsed.
- radix!: [Int64](../../core/core_package_api/core_package_intrinsics.md#int64) - The specified radix.

Returns:

- [Option](../../core/core_package_api/core_package_enums.md#enum-optiont)\<T> - The converted value, returns [Option](../../core/core_package_api/core_package_enums.md#enum-optiont)\<T>.None on failure.

### func toString(Int64)

```cangjie
func toString(radix!: Int64): String
```

Function: Returns the string representation in specified radix.

Parameters:

- radix!: [Int64](../../core/core_package_api/core_package_intrinsics.md#int64) - The specified radix.

Returns:

- [String](../../core/core_package_api/core_package_structs.md#struct-string) - The string representation in specified radix.

