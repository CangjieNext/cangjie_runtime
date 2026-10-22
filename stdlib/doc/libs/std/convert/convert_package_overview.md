# std.convert

## 功能介绍

convert 包提供从字符串转到特定类型的 Convert 系列函数。

例如字符串 "true" 到布尔类型 `true` 的转换以及字符串 "123" 到整数类型 `123` 的转换等。

convert 包提供格式化能力，主要为将仓颉类型实例转换为格式化字符串。

定义了接口 Formattable，用于规定统一的格式化方法，并为 Int8、Int16 等一系列仓颉类型实现了该接口，用户也可以自行为其他类型实现该接口以获取格式化功能。

将仓颉类型转换为字符串时，可根据格式化参数规定字符串格式，如宽度、对齐方式等。（在 Formattable 接口定义的方法中，格式化参数将作为函数入参。）

格式化参数的详细语法说明如下：

```text
format_spec := [flags][width][.precision][specifier]
flags := '-' | '+' | '#' | '0'
width := integer
precision := integer
specifier := 'b' | 'B' | 'o' | 'O' | 'x' | 'X' | 'e' | 'E' | 'g' | 'G'
```

参数 flags：

- '-' 适用于 Int，UInt，Rune 和 Float，表示左对齐。见[参数示例 flag1](./convert_samples/convert_samples.md#convert-参数-flag-的语法-1)。
- '+' 适用于 Int，UInt 和 Float，如果数值为正数则打出 '+' 符号，如果数值为负数则忽略。见[参数示例 flag2](./convert_samples/convert_samples.md#convert-参数-flag-的语法-2)。
- '#' 是针对进制打印的，对于二进制打印会补充一个 '0b' 或者 '0B'，对于八进制打印会补充一个 '0o' 或者 '0O'，对于十六进制会补充 '0x' 或者 '0X'。见[参数示例 flag3](./convert_samples/convert_samples.md#convert-参数-flag-的语法-3)。
- '0' 适用于 Int，UInt 和 Float，在空位补充 0。见[参数示例 flag4](./convert_samples/convert_samples.md#convert-参数-flag-的语法-4)。

参数 width 宽度：

- 宽度为正整数，适用于 Int，UInt，Rune 和 Float。见[参数示例 width](./convert_samples/convert_samples.md#convert-参数-width-的语法)。
- 宽度前有负号则表示左对齐，没有负号则是右对齐，如果宽度小于数值本身的长度，不会发生截断。
- 如果前缀有 `+` 或 `-` 符号会占用一个字符位，如果前缀有 `0x` 或 `0o` 等会占用两个字符位。

参数 precision 精度:

- 精度为正整数，适用于 Int，UInt 和 Float。见[参数示例 precision](./convert_samples/convert_samples.md#convert-参数-precision-的语法)。
- 对于浮点数表示小数点后的有效数字位数，如果不指定，那么则打印六位小数，如果小于数值本身有效数字的长度，那就四舍五入，如果大于就补全，补全的不一定是 0。
- 对于整数类型，不指定或者指定的数字小于数值本身的长度，则无效果，如果大于数值本身的长度，则在前面补全'0'。

参数 specifier:

- 'b' | 'B' | 'o' | 'O' | 'x' | 'X' 适用于 Int 和 UInt 类型。见[参数示例 specifier1](./convert_samples/convert_samples.md#convert-参数-specifier-的语法-1)。

  'b' | 'B' : 表示二进制格式打印

  'o' | 'O' : 表示八进制格式打印

  'x' | 'X' : 表示十六进制格式打印

- 'e' | 'E' | 'g' | 'G' 适用于 Float 类型。见[参数示例 specifier2](./convert_samples/convert_samples.md#convert-参数-specifier-的语法-2)。

  'e' | 'E' : 科学计数法，小写和大写

  'g' | 'G' : general，用十进制或者科学计数法表示，会选择精简的表示方式进行打印

## API 列表

### 接口

|              接口名          |           功能           |
| --------------------------- | ------------------------ |
| [Formattable](./convert_package_api/convert_package_interfaces.md#interface-formattable) | 该接口定义了格式化函数，即根据格式化参数将指定类型实例转换为对应格式的字符串。 |
| [Parsable\<T>](./convert_package_api/convert_package_interfaces.md#interface-parsablet) | 将字符串解析为特定类型的接口。 |
| [RadixConvertible\<T>](./convert_package_api/convert_package_interfaces.md#interface-radixconvertiblet) | 将指定字符串解析为特定类型的接口。 |

### 扩展

| 扩展名 | 功能 |
| --- | --- |
| [extend Bool <: Parsable\<Bool>](convert_package_api/convert_package_extends.md#extend-bool--parsablebool) | 此扩展主要用于实现将 [Bool](../core/core_package_api/core_package_intrinsics.md#bool) 类型字面量的字符串转换为 [Bool](../core/core_package_api/core_package_intrinsics.md#bool) 值的相关操作函数。 |
| [extend Float16 <: Formattable](convert_package_api/convert_package_extends.md#extend-float16--formattable) | 为 [Float16](../core/core_package_api/core_package_intrinsics.md#float16) 扩展 [Formattable](convert_package_api/convert_package_interfaces.md#interface-formattable) 接口，以实现将 [Float16](../core/core_package_api/core_package_intrinsics.md#float16) 实例转换为格式化字符串。 |
| [extend Float16 <: Parsable\<Float16>](convert_package_api/convert_package_extends.md#extend-float16--parsablefloat16) | 此扩展主要用于实现将 [Float16](../core/core_package_api/core_package_intrinsics.md#float16) 类型字面量的字符串转换为 [Float16](../core/core_package_api/core_package_intrinsics.md#float16) 值的相关操作函数。 |
| [extend Float32 <: Formattable](convert_package_api/convert_package_extends.md#extend-float32--formattable) | 为 [Float32](../core/core_package_api/core_package_intrinsics.md#float32) 扩展 [Formattable](convert_package_api/convert_package_interfaces.md#interface-formattable) 接口，以实现将 [Float32](../core/core_package_api/core_package_intrinsics.md#float32) 实例转换为格式化字符串。 |
| [extend Float32 <: Parsable\<Float32>](convert_package_api/convert_package_extends.md#extend-float32--parsablefloat32) | 此扩展主要用于实现将 [Float32](../core/core_package_api/core_package_intrinsics.md#float32) 类型字面量的字符串转换为 [Float32](../core/core_package_api/core_package_intrinsics.md#float32) 值的相关操作函数。 |
| [extend Float64 <: Formattable](convert_package_api/convert_package_extends.md#extend-float64--formattable) | 为 [Float64](../core/core_package_api/core_package_intrinsics.md#float64) 扩展 [Formattable](convert_package_api/convert_package_interfaces.md#interface-formattable) 接口，以实现将 [Float64](../core/core_package_api/core_package_intrinsics.md#float64) 实例转换为格式化字符串。 |
| [extend Float64 <: Parsable\<Float64>](convert_package_api/convert_package_extends.md#extend-float64--parsablefloat64) | 此扩展主要用于实现将 [Float64](../core/core_package_api/core_package_intrinsics.md#float64) 类型字面量的字符串转换为 [Float64](../core/core_package_api/core_package_intrinsics.md#float64) 值的相关操作函数。 |
| [extend Int16 <: Formattable](convert_package_api/convert_package_extends.md#extend-int16--formattable) | 为 [Int16](../core/core_package_api/core_package_intrinsics.md#int16) 扩展 [Formattable](convert_package_api/convert_package_interfaces.md#interface-formattable) 接口，以实现将 [Int16](../core/core_package_api/core_package_intrinsics.md#int16) 实例转换为格式化字符串。 |
| [extend Int16 <: Parsable\<Int16>](convert_package_api/convert_package_extends.md#extend-int16--parsableint16) | 此扩展主要用于实现将 [Int16](../core/core_package_api/core_package_intrinsics.md#int16) 类型字面量的字符串转换为 [Int16](../core/core_package_api/core_package_intrinsics.md#int16) 值的相关操作函数。 |
| [extend Int16 <: RadixConvertible\<Int16>](convert_package_api/convert_package_extends.md#extend-int16--radixconvertibleint16) | 此扩展主要用于实现将 [Int16](../core/core_package_api/core_package_intrinsics.md#int16) 类型字面量的字符串转换为 [Int16](../core/core_package_api/core_package_intrinsics.md#int16) 值的相关操作函数。 |
| [extend Int32 <: Formattable](convert_package_api/convert_package_extends.md#extend-int32--formattable) | 为 [Int32](../core/core_package_api/core_package_intrinsics.md#int32) 扩展 [Formattable](convert_package_api/convert_package_interfaces.md#interface-formattable) 接口，以实现将 [Int32](../core/core_package_api/core_package_intrinsics.md#int32) 实例转换为格式化字符串。 |
| [extend Int32 <: Parsable\<Int32>](convert_package_api/convert_package_extends.md#extend-int32--parsableint32) | 此扩展主要用于实现将 [Int32](../core/core_package_api/core_package_intrinsics.md#int32) 类型字面量的字符串转换为 [Int32](../core/core_package_api/core_package_intrinsics.md#int32) 值的相关操作函数。 |
| [extend Int32 <: RadixConvertible\<Int32>](convert_package_api/convert_package_extends.md#extend-int32--radixconvertibleint32) | 此扩展主要用于实现将 [Int32](../core/core_package_api/core_package_intrinsics.md#int32) 类型字面量的字符串转换为 [Int32](../core/core_package_api/core_package_intrinsics.md#int32) 值的相关操作函数。 |
| [extend Int64 <: Formattable](convert_package_api/convert_package_extends.md#extend-int64--formattable) | 为 [Int64](../core/core_package_api/core_package_intrinsics.md#int64) 扩展 [Formattable](convert_package_api/convert_package_interfaces.md#interface-formattable) 接口，以实现将 [Int64](../core/core_package_api/core_package_intrinsics.md#int64) 实例转换为格式化字符串。 |
| [extend Int64 <: Parsable\<Int64>](convert_package_api/convert_package_extends.md#extend-int64--parsableint64) | 此扩展主要用于实现将 [Int64](../core/core_package_api/core_package_intrinsics.md#int64) 类型字面量的字符串转换为 [Int64](../core/core_package_api/core_package_intrinsics.md#int64) 值的相关操作函数。 |
| [extend Int64 <: RadixConvertible\<Int64>](convert_package_api/convert_package_extends.md#extend-int64--radixconvertibleint64) | 此扩展主要用于实现将 [Int64](../core/core_package_api/core_package_intrinsics.md#int64) 类型字面量的字符串转换为 [Int64](../core/core_package_api/core_package_intrinsics.md#int64) 值的相关操作函数。 |
| [extend Int8 <: Formattable](convert_package_api/convert_package_extends.md#extend-int8--formattable) | 为 [Int8](../core/core_package_api/core_package_intrinsics.md#int8) 扩展 [Formattable](convert_package_api/convert_package_interfaces.md#interface-formattable) 接口，以实现将 [Int8](../core/core_package_api/core_package_intrinsics.md#int8) 实例转换为格式化字符串。 |
| [extend Int8 <: Parsable\<Int8>](convert_package_api/convert_package_extends.md#extend-int8--parsableint8) | 此扩展主要用于实现将 [Int8](../core/core_package_api/core_package_intrinsics.md#int8) 类型字面量的字符串转换为 [Int8](../core/core_package_api/core_package_intrinsics.md#int8) 值的相关操作函数。 |
| [extend Int8 <: RadixConvertible\<Int8>](convert_package_api/convert_package_extends.md#extend-int8--radixconvertibleint8) | 此扩展主要用于实现将 [Int8](../core/core_package_api/core_package_intrinsics.md#int8) 类型字面量的字符串转换为 [Int8](../core/core_package_api/core_package_intrinsics.md#int8) 值的相关操作函数。 |
| [extend Rune <: Formattable](convert_package_api/convert_package_extends.md#extend-rune--formattable) | 为 [Rune](../core/core_package_api/core_package_intrinsics.md#rune) 扩展 [Formattable](convert_package_api/convert_package_interfaces.md#interface-formattable) 接口，以实现将 [Rune](../core/core_package_api/core_package_intrinsics.md#rune) 实例转换为格式化字符串。 |
| [extend Rune <: Parsable\<Rune>](convert_package_api/convert_package_extends.md#extend-rune--parsablerune) | 此扩展主要用于实现将 [Rune](../core/core_package_api/core_package_intrinsics.md#rune) 类型字面量的字符串转换为 [Rune](../core/core_package_api/core_package_intrinsics.md#rune) 值的相关操作函数。 |
| [extend UInt16 <: Formattable](convert_package_api/convert_package_extends.md#extend-uint16--formattable) | 为 [UInt16](../core/core_package_api/core_package_intrinsics.md#uint16) 扩展 [Formattable](convert_package_api/convert_package_interfaces.md#interface-formattable) 接口，以实现将 [UInt16](../core/core_package_api/core_package_intrinsics.md#uint16) 实例转换为格式化字符串。 |
| [extend UInt16 <: Parsable\<UInt16>](convert_package_api/convert_package_extends.md#extend-uint16--parsableuint16) | 此扩展主要用于实现将 [UInt16](../core/core_package_api/core_package_intrinsics.md#uint16) 类型字面量的字符串转换为 [UInt16](../core/core_package_api/core_package_intrinsics.md#uint16) 值的相关操作函数。 |
| [extend UInt16 <: RadixConvertible\<UInt16>](convert_package_api/convert_package_extends.md#extend-uint16--radixconvertibleuint16) | 此扩展主要用于实现将 [UInt16](../core/core_package_api/core_package_intrinsics.md#uint16) 类型字面量的字符串转换为 [UInt16](../core/core_package_api/core_package_intrinsics.md#uint16) 值的相关操作函数。 |
| [extend UInt32 <: Formattable](convert_package_api/convert_package_extends.md#extend-uint32--formattable) | 为 [UInt32](../core/core_package_api/core_package_intrinsics.md#uint32) 扩展 [Formattable](convert_package_api/convert_package_interfaces.md#interface-formattable) 接口，以实现将 [UInt32](../core/core_package_api/core_package_intrinsics.md#uint32) 实例转换为格式化字符串。 |
| [extend UInt32 <: Parsable\<UInt32>](convert_package_api/convert_package_extends.md#extend-uint32--parsableuint32) | 此扩展主要用于实现将 [UInt32](../core/core_package_api/core_package_intrinsics.md#uint32) 类型字面量的字符串转换为 [UInt32](../core/core_package_api/core_package_intrinsics.md#uint32) 值的相关操作函数。 |
| [extend UInt32 <: RadixConvertible\<UInt32>](convert_package_api/convert_package_extends.md#extend-uint32--radixconvertibleuint32) | 此扩展主要用于实现将 [UInt32](../core/core_package_api/core_package_intrinsics.md#uint32) 类型字面量的字符串转换为 [UInt32](../core/core_package_api/core_package_intrinsics.md#uint32) 值的相关操作函数。 |
| [extend UInt64 <: Formattable](convert_package_api/convert_package_extends.md#extend-uint64--formattable) | 为 [UInt64](../core/core_package_api/core_package_intrinsics.md#uint64) 扩展 [Formattable](convert_package_api/convert_package_interfaces.md#interface-formattable) 接口，以实现将 [UInt64](../core/core_package_api/core_package_intrinsics.md#uint64) 实例转换为格式化字符串。 |
| [extend UInt64 <: Parsable\<UInt64>](convert_package_api/convert_package_extends.md#extend-uint64--parsableuint64) | 此扩展主要用于实现将 [UInt64](../core/core_package_api/core_package_intrinsics.md#uint64) 类型字面量的字符串转换为 [UInt64](../core/core_package_api/core_package_intrinsics.md#uint64) 值的相关操作函数。 |
| [extend UInt64 <: RadixConvertible\<UInt64>](convert_package_api/convert_package_extends.md#extend-uint64--radixconvertibleuint64) | 此扩展主要用于实现将 [UInt64](../core/core_package_api/core_package_intrinsics.md#uint64) 类型字面量的字符串转换为 [UInt64](../core/core_package_api/core_package_intrinsics.md#uint64) 值的相关操作函数。 |
| [extend UInt8 <: Formattable](convert_package_api/convert_package_extends.md#extend-uint8--formattable) | 为 [UInt8](../core/core_package_api/core_package_intrinsics.md#uint8) 扩展 [Formattable](convert_package_api/convert_package_interfaces.md#interface-formattable) 接口，以实现将 [UInt8](../core/core_package_api/core_package_intrinsics.md#uint8) 实例转换为格式化字符串。 |
| [extend UInt8 <: Parsable\<UInt8>](convert_package_api/convert_package_extends.md#extend-uint8--parsableuint8) | 此扩展主要用于实现将 [UInt8](../core/core_package_api/core_package_intrinsics.md#uint8) 类型字面量的字符串转换为 [UInt8](../core/core_package_api/core_package_intrinsics.md#uint8) 值的相关操作函数。 |
| [extend UInt8 <: RadixConvertible\<UInt8>](convert_package_api/convert_package_extends.md#extend-uint8--radixconvertibleuint8) | 此扩展主要用于实现将 [UInt8](../core/core_package_api/core_package_intrinsics.md#uint8) 类型字面量的字符串转换为 [UInt8](../core/core_package_api/core_package_intrinsics.md#uint8) 值的相关操作函数。 |
