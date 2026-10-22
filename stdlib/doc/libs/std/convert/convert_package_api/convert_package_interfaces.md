# 接口

## interface Formattable

```cangjie
public interface Formattable
```

功能：该接口定义了格式化函数，即根据格式化参数将指定类型实例转换为对应格式的字符串。

格式化参数相关的说明请参见 convert 包中的[功能介绍](./../convert_package_overview.md#功能介绍)。

其他类型可通过实现该接口提供格式化功能。

扩展：

- [extend Float16 <: Formattable](convert_package_extends.md#extend-float16--formattable)
- [extend Float32 <: Formattable](convert_package_extends.md#extend-float32--formattable)
- [extend Float64 <: Formattable](convert_package_extends.md#extend-float64--formattable)
- [extend Int16 <: Formattable](convert_package_extends.md#extend-int16--formattable)
- [extend Int32 <: Formattable](convert_package_extends.md#extend-int32--formattable)
- [extend Int64 <: Formattable](convert_package_extends.md#extend-int64--formattable)
- [extend Int8 <: Formattable](convert_package_extends.md#extend-int8--formattable)
- [extend Rune <: Formattable](convert_package_extends.md#extend-rune--formattable)
- [extend UInt16 <: Formattable](convert_package_extends.md#extend-uint16--formattable)
- [extend UInt32 <: Formattable](convert_package_extends.md#extend-uint32--formattable)
- [extend UInt64 <: Formattable](convert_package_extends.md#extend-uint64--formattable)
- [extend UInt8 <: Formattable](convert_package_extends.md#extend-uint8--formattable)
- [extend BigInt <: Formattable](../../math_numeric/math_numeric_package_api/math_numeric_package_extends.md#extend-bigint--formattable)
- [extend Decimal <: Formattable](../../math_numeric/math_numeric_package_api/math_numeric_package_extends.md#extend-decimal--formattable)

### func format(String)

```cangjie
func format(fmt: String): String
```

功能：根据格式化参数将当前实例格式化为对应格式的字符串。

参数：

- fmt: [String](../../core/core_package_api/core_package_structs.md#struct-string) - 格式化参数。

返回值：

- [String](../../core/core_package_api/core_package_structs.md#struct-string) - 将当前实例格式化后得到的字符串。

## interface Parsable\<T>

```cangjie
public interface Parsable<T>
```

功能：本接口提供了统一的方法，以支持将字符串解析为特定类型。

本接口提供了 parse 和 tryParse 两套方法，parse 方法将在解析失败时抛出异常，tryParse 方法将返回值用 [Option](../../core/core_package_api/core_package_enums.md#enum-optiont) 包裹，解析失败时将返回 None。
本包内已经为 [Bool](../../core/core_package_api/core_package_intrinsics.md#bool)，[Rune](../../core/core_package_api/core_package_intrinsics.md#rune)，[Float16](../../core/core_package_api/core_package_intrinsics.md#float16)，[Int64](../../core/core_package_api/core_package_intrinsics.md#int64) 等基础类型实现该接口，可用于将字符串转换为这些类型。

扩展：

- [extend Bool <: Parsable\<Bool>](convert_package_extends.md#extend-bool--parsablebool)
- [extend Float16 <: Parsable\<Float16>](convert_package_extends.md#extend-float16--parsablefloat16)
- [extend Float32 <: Parsable\<Float32>](convert_package_extends.md#extend-float32--parsablefloat32)
- [extend Float64 <: Parsable\<Float64>](convert_package_extends.md#extend-float64--parsablefloat64)
- [extend Int16 <: Parsable\<Int16>](convert_package_extends.md#extend-int16--parsableint16)
- [extend Int32 <: Parsable\<Int32>](convert_package_extends.md#extend-int32--parsableint32)
- [extend Int64 <: Parsable\<Int64>](convert_package_extends.md#extend-int64--parsableint64)
- [extend Int8 <: Parsable\<Int8>](convert_package_extends.md#extend-int8--parsableint8)
- [extend Rune <: Parsable\<Rune>](convert_package_extends.md#extend-rune--parsablerune)
- [extend UInt16 <: Parsable\<UInt16>](convert_package_extends.md#extend-uint16--parsableuint16)
- [extend UInt32 <: Parsable\<UInt32>](convert_package_extends.md#extend-uint32--parsableuint32)
- [extend UInt64 <: Parsable\<UInt64>](convert_package_extends.md#extend-uint64--parsableuint64)
- [extend UInt8 <: Parsable\<UInt8>](convert_package_extends.md#extend-uint8--parsableuint8)
- [extend BigInt <: Parsable\<BigInt>](../../math_numeric/math_numeric_package_api/math_numeric_package_extends.md#extend-bigint--parsablebigint)
- [extend Decimal <: Parsable\<Decimal>](../../math_numeric/math_numeric_package_api/math_numeric_package_extends.md#extend-decimal--parsabledecimal)

### static func parse(String)

```cangjie
static func parse(value: String): T
```

功能：从字符串中解析特定类型。

参数：

- value: [String](../../core/core_package_api/core_package_structs.md#struct-string) - 待解析的字符串。

返回值：

- T - 转换后的值。

### static func tryParse(String)

```cangjie
static func tryParse(value: String): Option<T>
```

功能：从字符串中解析特定类型。

参数：

- value: [String](../../core/core_package_api/core_package_structs.md#struct-string) - 待解析的字符串。

返回值：

- [Option](../../core/core_package_api/core_package_enums.md#enum-optiont)\<T> - 转换后值，转换失败返回 [Option](../../core/core_package_api/core_package_enums.md#enum-optiont)\<T>.None。

## interface RadixConvertible\<T>

```cangjie
public interface RadixConvertible<T>
```

功能：本接口提供了统一的方法，以支持将指定进制的字符串解析为特定类型。

进制将以参数指定，不按字符串前缀指定，进制表示范围必须在 2-36 之间，否则会抛异常，超过十进制的表示按大或者小写英文字母序表示，即 10 个数字 + 26 个英文字母 = 36 进制，Int 系列的字符串可以以 `+`、`-` 开头，如果不以这两个符号开头则处理成 `+`，UInt 系列的字符串可以以 `+` 开头，如果以 `-` 开头会抛出异常。返回指定进制形式字符串，超过十进制的表示按小写英文字母序表示，即 10 个数字 + 26 个小写英文字母 = 36 进制。

本接口提供了 parse 和 tryParse 两套方法，parse 方法将在解析失败时抛出异常，tryParse 方法将返回值用 [Option](../../core/core_package_api/core_package_enums.md#enum-optiont) 包裹，解析失败时将返回 None。
本包内已经为[Int64](../../core/core_package_api/core_package_intrinsics.md#int64)，[UInt64](../../core/core_package_api/core_package_intrinsics.md#uint64) 等基础类型实现该接口，可用于将字符串转换为这些类型。

扩展：

- [extend Int16 <: RadixConvertible\<Int16>](convert_package_extends.md#extend-int16--radixconvertibleint16)
- [extend Int32 <: RadixConvertible\<Int32>](convert_package_extends.md#extend-int32--radixconvertibleint32)
- [extend Int64 <: RadixConvertible\<Int64>](convert_package_extends.md#extend-int64--radixconvertibleint64)
- [extend Int8 <: RadixConvertible\<Int8>](convert_package_extends.md#extend-int8--radixconvertibleint8)
- [extend UInt16 <: RadixConvertible\<UInt16>](convert_package_extends.md#extend-uint16--radixconvertibleuint16)
- [extend UInt32 <: RadixConvertible\<UInt32>](convert_package_extends.md#extend-uint32--radixconvertibleuint32)
- [extend UInt64 <: RadixConvertible\<UInt64>](convert_package_extends.md#extend-uint64--radixconvertibleuint64)
- [extend UInt8 <: RadixConvertible\<UInt8>](convert_package_extends.md#extend-uint8--radixconvertibleuint8)
- [extend BigInt <: RadixConvertible\<BigInt>](../../math_numeric/math_numeric_package_api/math_numeric_package_extends.md#extend-bigint--radixconvertiblebigint)

### static func parse(String, Int64)

```cangjie
static func parse(value: String, radix!: Int64): T
```

功能：从指定进制字符串中解析特定类型。

参数：

- value: [String](../../core/core_package_api/core_package_structs.md#struct-string) - 待解析的字符串。
- radix!: [Int64](../../core/core_package_api/core_package_intrinsics.md#int64) - 指定的进制。

返回值：

- T - 转换后的值。

### static func tryParse(String, Int64)

```cangjie
static func tryParse(value: String, radix!: Int64): Option<T>
```

功能：从指定进制字符串中解析特定类型。

参数：

- value: [String](../../core/core_package_api/core_package_structs.md#struct-string) - 待解析的字符串。
- radix!: [Int64](../../core/core_package_api/core_package_intrinsics.md#int64) - 指定的进制。

返回值：

- [Option](../../core/core_package_api/core_package_enums.md#enum-optiont)\<T> - 转换后值，转换失败返回 [Option](../../core/core_package_api/core_package_enums.md#enum-optiont)\<T>.None。

### func toString(Int64)

```cangjie
func toString(radix!: Int64): String
```

功能：返回指定进制形式字符串。

参数：

- radix!: [Int64](../../core/core_package_api/core_package_intrinsics.md#int64) - 指定的进制。

返回值：

- [String](../../core/core_package_api/core_package_structs.md#struct-string) - 指定进制形式字符串。

