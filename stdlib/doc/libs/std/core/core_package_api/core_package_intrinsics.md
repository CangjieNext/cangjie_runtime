# 内置类型

## Bool

功能：表示布尔类型，有 `true` 和 `false` 两种取值。

扩展：

- [extend Bool <: BigEndianOrder\<Bool> & LittleEndianOrder\<Bool>](../../binary/binary_package_api/binary_package_extends.md#extend-bool--bigendianorderbool--littleendianorderbool)
- [extend Bool <: Parsable\<Bool>](../../convert/convert_package_api/convert_package_extends.md#extend-bool--parsablebool)
- [extend Bool <: Equatable\<Bool>](core_package_extends.md#extend-bool--equatablebool)
- [extend Bool <: Hashable](core_package_extends.md#extend-bool--hashable)
- [extend Bool <: ToString](core_package_extends.md#extend-bool--tostring)

## CPointer\<T>

功能：表示 `T` 类型实例的指针，在与 C 语言互操作的场景下使用，对应 C 语言的 `T*`。

扩展：

- [extend\<T> CPointer\<T>](core_package_extends.md#extendt-cpointert)

其中 `T` 必须满足 [CType](core_package_interfaces.md#interface-ctype) 约束。

[CPointer](core_package_intrinsics.md#cpointert) 类型必须满足：

- 大小和对齐与平台相关。
- 对它做加减法算术运算、读写内存，是需要在 unsafe 上下文操作的。
- [CPointer](core_package_intrinsics.md#cpointert)\<T1> 可以在 unsafe 上下文中使用类型强制转换，变成 [CPointer](core_package_intrinsics.md#cpointert)\<T2> 类型。

## CString

功能：表示 C 风格字符串，在与 C 语言互操作的场景下使用。

扩展：

- [extend CString <: ToString](core_package_extends.md#extend-cstring--tostring)

可以通过 [CString](core_package_intrinsics.md#cstring) 的构造函数或 [LibC](core_package_structs.md#struct-libc) 的 `mallocCString` 创建 C 风格字符串，如需在仓颉端释放，则调用 [LibC](core_package_structs.md#struct-libc) 的 free 方法。

## Float16

功能：表示 16 位浮点数，符合 `IEEE 754` 中的半精度格式（`binary16`）。

扩展：

- [extend Float16 <: BigEndianOrder\<Float16> & LittleEndianOrder\<Float16>](../../binary/binary_package_api/binary_package_extends.md#extend-float16--bigendianorderfloat16--littleendianorderfloat16)
- [extend Float16 <: Formattable](../../convert/convert_package_api/convert_package_extends.md#extend-float16--formattable)
- [extend Float16 <: Parsable\<Float16>](../../convert/convert_package_api/convert_package_extends.md#extend-float16--parsablefloat16)
- [extend Float16 <: Comparable\<Float16>](core_package_extends.md#extend-float16--comparablefloat16)
- [extend Float16 <: Hashable](core_package_extends.md#extend-float16--hashable)
- [extend Float16 <: ToString](core_package_extends.md#extend-float16--tostring)
- [extend Float16 <: FloatingPoint\<Float16>](../../math/math_package_api/math_package_extends.md#extend-float16--floatingpointfloat16)
- [extend Float16 <: MathExtension\<Float16>](../../math/math_package_api/math_package_extends.md#extend-float16--mathextensionfloat16)
- [extend Float16 <: MaxMinValue\<Float16>](../../math/math_package_api/math_package_extends.md#extend-float16--maxminvaluefloat16)
- [extend Float16 <: Number\<Float16>](../../math/math_package_api/math_package_extends.md#extend-float16--numberfloat16)
- [extend Float16](core_package_extends.md#extend-float16)

## Float32

功能：表示 32 位浮点数，符合 `IEEE 754` 中的单精度格式（`binary32`）。

扩展：

- [extend Float32 <: BigEndianOrder\<Float32> & LittleEndianOrder\<Float32>](../../binary/binary_package_api/binary_package_extends.md#extend-float32--bigendianorderfloat32--littleendianorderfloat32)
- [extend Float32 <: Formattable](../../convert/convert_package_api/convert_package_extends.md#extend-float32--formattable)
- [extend Float32 <: Parsable\<Float32>](../../convert/convert_package_api/convert_package_extends.md#extend-float32--parsablefloat32)
- [extend Float32 <: Comparable\<Float32>](core_package_extends.md#extend-float32--comparablefloat32)
- [extend Float32 <: Hashable](core_package_extends.md#extend-float32--hashable)
- [extend Float32 <: ToString](core_package_extends.md#extend-float32--tostring)
- [extend Float32 <: FloatingPoint\<Float32>](../../math/math_package_api/math_package_extends.md#extend-float32--floatingpointfloat32)
- [extend Float32 <: MathExtension\<Float32>](../../math/math_package_api/math_package_extends.md#extend-float32--mathextensionfloat32)
- [extend Float32 <: MaxMinValue\<Float32>](../../math/math_package_api/math_package_extends.md#extend-float32--maxminvaluefloat32)
- [extend Float32 <: Number\<Float32>](../../math/math_package_api/math_package_extends.md#extend-float32--numberfloat32)
- [extend Float32](core_package_extends.md#extend-float32)

## Float64

功能：表示 64 位浮点数，符合 `IEEE 754` 中的双精度格式（`binary64`）。

扩展：

- [extend Float64 <: BigEndianOrder\<Float64> & LittleEndianOrder\<Float64>](../../binary/binary_package_api/binary_package_extends.md#extend-float64--bigendianorderfloat64--littleendianorderfloat64)
- [extend Float64 <: Formattable](../../convert/convert_package_api/convert_package_extends.md#extend-float64--formattable)
- [extend Float64 <: Parsable\<Float64>](../../convert/convert_package_api/convert_package_extends.md#extend-float64--parsablefloat64)
- [extend Float64 <: Comparable\<Float64>](core_package_extends.md#extend-float64--comparablefloat64)
- [extend Float64 <: Hashable](core_package_extends.md#extend-float64--hashable)
- [extend Float64 <: ToString](core_package_extends.md#extend-float64--tostring)
- [extend Float64 <: FloatingPoint\<Float64>](../../math/math_package_api/math_package_extends.md#extend-float64--floatingpointfloat64)
- [extend Float64 <: MathExtension\<Float64>](../../math/math_package_api/math_package_extends.md#extend-float64--mathextensionfloat64)
- [extend Float64 <: MaxMinValue\<Float64>](../../math/math_package_api/math_package_extends.md#extend-float64--maxminvaluefloat64)
- [extend Float64 <: Number\<Float64>](../../math/math_package_api/math_package_extends.md#extend-float64--numberfloat64)
- [extend Float64](core_package_extends.md#extend-float64)
- [extend Float64](core_package_extends.md#extend-float64-1)

## Int16

功能：表示 16 位有符号整型，表示范围为 [-2^{15}, 2^{15} - 1]。

扩展：

- [extend Int16 <: BigEndianOrder\<Int16> & LittleEndianOrder\<Int16> & SwapEndianOrder\<Int16>](../../binary/binary_package_api/binary_package_extends.md#extend-int16--bigendianorderint16--littleendianorderint16--swapendianorderint16)
- [extend Int16 <: Formattable](../../convert/convert_package_api/convert_package_extends.md#extend-int16--formattable)
- [extend Int16 <: Parsable\<Int16>](../../convert/convert_package_api/convert_package_extends.md#extend-int16--parsableint16)
- [extend Int16 <: RadixConvertible\<Int16>](../../convert/convert_package_api/convert_package_extends.md#extend-int16--radixconvertibleint16)
- [extend Int16 <: Comparable\<Int16>](core_package_extends.md#extend-int16--comparableint16)
- [extend Int16 <: Countable\<Int16>](core_package_extends.md#extend-int16--countableint16)
- [extend Int16 <: Hashable](core_package_extends.md#extend-int16--hashable)
- [extend Int16 <: ToString](core_package_extends.md#extend-int16--tostring)
- [extend Int16 <: Integer\<Int16>](../../math/math_package_api/math_package_extends.md#extend-int16--integerint16)
- [extend Int16 <: MaxMinValue\<Int16>](../../math/math_package_api/math_package_extends.md#extend-int16--maxminvalueint16)
- [extend Int16 <: Number\<Int16>](../../math/math_package_api/math_package_extends.md#extend-int16--numberint16)
- [extend Int16 <: CarryingOp\<Int16>](../../overflow/overflow_package_api/overflow_package_extends.md#extend-int16--carryingopint16)
- [extend Int16 <: CheckedOp\<Int16>](../../overflow/overflow_package_api/overflow_package_extends.md#extend-int16--checkedopint16)
- [extend Int16 <: SaturatingOp\<Int16>](../../overflow/overflow_package_api/overflow_package_extends.md#extend-int16--saturatingopint16)
- [extend Int16 <: ThrowingOp\<Int16>](../../overflow/overflow_package_api/overflow_package_extends.md#extend-int16--throwingopint16)
- [extend Int16 <: WrappingOp\<Int16>](../../overflow/overflow_package_api/overflow_package_extends.md#extend-int16--wrappingopint16)

## Int32

功能：表示 32 位有符号整型，表示范围为 [-2^{31}, 2^{31} - 1]。

扩展：

- [extend Int32 <: BigEndianOrder\<Int32> & LittleEndianOrder\<Int32> & SwapEndianOrder\<Int32>](../../binary/binary_package_api/binary_package_extends.md#extend-int32--bigendianorderint32--littleendianorderint32--swapendianorderint32)
- [extend Int32 <: Formattable](../../convert/convert_package_api/convert_package_extends.md#extend-int32--formattable)
- [extend Int32 <: Parsable\<Int32>](../../convert/convert_package_api/convert_package_extends.md#extend-int32--parsableint32)
- [extend Int32 <: RadixConvertible\<Int32>](../../convert/convert_package_api/convert_package_extends.md#extend-int32--radixconvertibleint32)
- [extend Int32 <: Comparable\<Int32>](core_package_extends.md#extend-int32--comparableint32)
- [extend Int32 <: Countable\<Int32>](core_package_extends.md#extend-int32--countableint32)
- [extend Int32 <: Hashable](core_package_extends.md#extend-int32--hashable)
- [extend Int32 <: ToString](core_package_extends.md#extend-int32--tostring)
- [extend Int32 <: Integer\<Int32>](../../math/math_package_api/math_package_extends.md#extend-int32--integerint32)
- [extend Int32 <: MaxMinValue\<Int32>](../../math/math_package_api/math_package_extends.md#extend-int32--maxminvalueint32)
- [extend Int32 <: Number\<Int32>](../../math/math_package_api/math_package_extends.md#extend-int32--numberint32)
- [extend Int32 <: CarryingOp\<Int32>](../../overflow/overflow_package_api/overflow_package_extends.md#extend-int32--carryingopint32)
- [extend Int32 <: CheckedOp\<Int32>](../../overflow/overflow_package_api/overflow_package_extends.md#extend-int32--checkedopint32)
- [extend Int32 <: SaturatingOp\<Int32>](../../overflow/overflow_package_api/overflow_package_extends.md#extend-int32--saturatingopint32)
- [extend Int32 <: ThrowingOp\<Int32>](../../overflow/overflow_package_api/overflow_package_extends.md#extend-int32--throwingopint32)
- [extend Int32 <: WrappingOp\<Int32>](../../overflow/overflow_package_api/overflow_package_extends.md#extend-int32--wrappingopint32)

## Int64

功能：表示 64 位有符号整型，表示范围为 [-2^{63}, 2^{63} - 1]。

扩展：

- [extend Int64 <: BigEndianOrder\<Int64> & LittleEndianOrder\<Int64> & SwapEndianOrder\<Int64>](../../binary/binary_package_api/binary_package_extends.md#extend-int64--bigendianorderint64--littleendianorderint64--swapendianorderint64)
- [extend Int64 <: Formattable](../../convert/convert_package_api/convert_package_extends.md#extend-int64--formattable)
- [extend Int64 <: Parsable\<Int64>](../../convert/convert_package_api/convert_package_extends.md#extend-int64--parsableint64)
- [extend Int64 <: RadixConvertible\<Int64>](../../convert/convert_package_api/convert_package_extends.md#extend-int64--radixconvertibleint64)
- [extend Int64 <: Comparable\<Int64>](core_package_extends.md#extend-int64--comparableint64)
- [extend Int64 <: Countable\<Int64>](core_package_extends.md#extend-int64--countableint64)
- [extend Int64 <: Hashable](core_package_extends.md#extend-int64--hashable)
- [extend Int64 <: ToString](core_package_extends.md#extend-int64--tostring)
- [extend Int64 <: Integer\<Int64>](../../math/math_package_api/math_package_extends.md#extend-int64--integerint64)
- [extend Int64 <: MaxMinValue\<Int64>](../../math/math_package_api/math_package_extends.md#extend-int64--maxminvalueint64)
- [extend Int64 <: Number\<Int64>](../../math/math_package_api/math_package_extends.md#extend-int64--numberint64)
- [extend Int64](core_package_extends.md#extend-int64)
- [extend Int64 <: CarryingOp\<Int64> & CarryingPow](../../overflow/overflow_package_api/overflow_package_extends.md#extend-int64--carryingopint64--carryingpow)
- [extend Int64 <: CheckedOp\<Int64> & CheckedPow](../../overflow/overflow_package_api/overflow_package_extends.md#extend-int64--checkedopint64--checkedpow)
- [extend Int64 <: SaturatingOp\<Int64> & SaturatingPow](../../overflow/overflow_package_api/overflow_package_extends.md#extend-int64--saturatingopint64--saturatingpow)
- [extend Int64 <: ThrowingOp\<Int64> & ThrowingPow](../../overflow/overflow_package_api/overflow_package_extends.md#extend-int64--throwingopint64--throwingpow)
- [extend Int64 <: WrappingOp\<Int64> & WrappingPow](../../overflow/overflow_package_api/overflow_package_extends.md#extend-int64--wrappingopint64--wrappingpow)

## Int8

功能：表示 8 位有符号整型，表示范围为 [-2^7, 2^7 - 1]。

扩展：

- [extend Int8 <: BigEndianOrder\<Int8> & LittleEndianOrder\<Int8> & SwapEndianOrder\<Int8>](../../binary/binary_package_api/binary_package_extends.md#extend-int8--bigendianorderint8--littleendianorderint8--swapendianorderint8)
- [extend Int8 <: Formattable](../../convert/convert_package_api/convert_package_extends.md#extend-int8--formattable)
- [extend Int8 <: Parsable\<Int8>](../../convert/convert_package_api/convert_package_extends.md#extend-int8--parsableint8)
- [extend Int8 <: RadixConvertible\<Int8>](../../convert/convert_package_api/convert_package_extends.md#extend-int8--radixconvertibleint8)
- [extend Int8 <: Comparable\<Int8>](core_package_extends.md#extend-int8--comparableint8)
- [extend Int8 <: Countable\<Int8>](core_package_extends.md#extend-int8--countableint8)
- [extend Int8 <: Hashable](core_package_extends.md#extend-int8--hashable)
- [extend Int8 <: ToString](core_package_extends.md#extend-int8--tostring)
- [extend Int8 <: Integer\<Int8>](../../math/math_package_api/math_package_extends.md#extend-int8--integerint8)
- [extend Int8 <: MaxMinValue\<Int8>](../../math/math_package_api/math_package_extends.md#extend-int8--maxminvalueint8)
- [extend Int8 <: Number\<Int8>](../../math/math_package_api/math_package_extends.md#extend-int8--numberint8)
- [extend Int8 <: CarryingOp\<Int8>](../../overflow/overflow_package_api/overflow_package_extends.md#extend-int8--carryingopint8)
- [extend Int8 <: CheckedOp\<Int8>](../../overflow/overflow_package_api/overflow_package_extends.md#extend-int8--checkedopint8)
- [extend Int8 <: SaturatingOp\<Int8>](../../overflow/overflow_package_api/overflow_package_extends.md#extend-int8--saturatingopint8)
- [extend Int8 <: ThrowingOp\<Int8>](../../overflow/overflow_package_api/overflow_package_extends.md#extend-int8--throwingopint8)
- [extend Int8 <: WrappingOp\<Int8>](../../overflow/overflow_package_api/overflow_package_extends.md#extend-int8--wrappingopint8)

## IntNative

功能：表示平台相关的有符号整型，其长度与当前系统的位宽一致。

扩展：

- [extend IntNative <: Comparable\<IntNative>](core_package_extends.md#extend-intnative--comparableintnative)
- [extend IntNative <: Countable\<IntNative>](core_package_extends.md#extend-intnative--countableintnative)
- [extend IntNative <: Hashable](core_package_extends.md#extend-intnative--hashable)
- [extend IntNative <: ToString](core_package_extends.md#extend-intnative--tostring)
- [extend IntNative <: Integer\<IntNative>](../../math/math_package_api/math_package_extends.md#extend-intnative--integerintnative)
- [extend IntNative <: MaxMinValue\<IntNative>](../../math/math_package_api/math_package_extends.md#extend-intnative--maxminvalueintnative)
- [extend IntNative <: Number\<IntNative>](../../math/math_package_api/math_package_extends.md#extend-intnative--numberintnative)
- [extend IntNative <: CarryingOp\<IntNative>](../../overflow/overflow_package_api/overflow_package_extends.md#extend-intnative--carryingopintnative)
- [extend IntNative <: CheckedOp\<IntNative>](../../overflow/overflow_package_api/overflow_package_extends.md#extend-intnative--checkedopintnative)
- [extend IntNative <: SaturatingOp\<IntNative>](../../overflow/overflow_package_api/overflow_package_extends.md#extend-intnative--saturatingopintnative)
- [extend IntNative <: ThrowingOp\<IntNative>](../../overflow/overflow_package_api/overflow_package_extends.md#extend-intnative--throwingopintnative)
- [extend IntNative <: WrappingOp\<IntNative>](../../overflow/overflow_package_api/overflow_package_extends.md#extend-intnative--wrappingopintnative)

## Rune

功能：表示 Unicode 字符集中的字符。

表示范围为 `Unicode scalar value`，即从 `\u{0000}` 到 `\u{D7FF}`，以及从 `\u{E000}` 到 `\u{10FFFF}` 的字符。

扩展：

- [extend Rune <: Formattable](../../convert/convert_package_api/convert_package_extends.md#extend-rune--formattable)
- [extend Rune <: Parsable\<Rune>](../../convert/convert_package_api/convert_package_extends.md#extend-rune--parsablerune)
- [extend Rune](core_package_extends.md#extend-rune)
- [extend Rune <: Comparable\<Rune>](core_package_extends.md#extend-rune--comparablerune)
- [extend Rune <: Countable\<Rune>](core_package_extends.md#extend-rune--countablerune)
- [extend Rune <: Hashable](core_package_extends.md#extend-rune--hashable)
- [extend Rune <: ToString](core_package_extends.md#extend-rune--tostring)
- [extend Rune <: UnicodeRuneExtension](../../unicode/unicode_package_api/unicode_package_extends.md#extend-rune--unicoderuneextension)

## UInt16

功能：表示 16 位无符号整型，表示范围为 [0, 2^{16} - 1]。

扩展：

- [extend UInt16 <: BigEndianOrder\<UInt16> & LittleEndianOrder\<UInt16> & SwapEndianOrder\<UInt16>](../../binary/binary_package_api/binary_package_extends.md#extend-uint16--bigendianorderuint16--littleendianorderuint16--swapendianorderuint16)
- [extend UInt16 <: Formattable](../../convert/convert_package_api/convert_package_extends.md#extend-uint16--formattable)
- [extend UInt16 <: Parsable\<UInt16>](../../convert/convert_package_api/convert_package_extends.md#extend-uint16--parsableuint16)
- [extend UInt16 <: RadixConvertible\<UInt16>](../../convert/convert_package_api/convert_package_extends.md#extend-uint16--radixconvertibleuint16)
- [extend UInt16 <: Comparable\<UInt16>](core_package_extends.md#extend-uint16--comparableuint16)
- [extend UInt16 <: Countable\<UInt16>](core_package_extends.md#extend-uint16--countableuint16)
- [extend UInt16 <: Hashable](core_package_extends.md#extend-uint16--hashable)
- [extend UInt16 <: ToString](core_package_extends.md#extend-uint16--tostring)
- [extend UInt16 <: Integer\<UInt16>](../../math/math_package_api/math_package_extends.md#extend-uint16--integeruint16)
- [extend UInt16 <: MaxMinValue\<UInt16>](../../math/math_package_api/math_package_extends.md#extend-uint16--maxminvalueuint16)
- [extend UInt16 <: Number\<UInt16>](../../math/math_package_api/math_package_extends.md#extend-uint16--numberuint16)
- [extend UInt16 <: CarryingOp\<UInt16>](../../overflow/overflow_package_api/overflow_package_extends.md#extend-uint16--carryingopuint16)
- [extend UInt16 <: CheckedOp\<UInt16>](../../overflow/overflow_package_api/overflow_package_extends.md#extend-uint16--checkedopuint16)
- [extend UInt16 <: SaturatingOp\<UInt16>](../../overflow/overflow_package_api/overflow_package_extends.md#extend-uint16--saturatingopuint16)
- [extend UInt16 <: ThrowingOp\<UInt16>](../../overflow/overflow_package_api/overflow_package_extends.md#extend-uint16--throwingopuint16)
- [extend UInt16 <: WrappingOp\<UInt16>](../../overflow/overflow_package_api/overflow_package_extends.md#extend-uint16--wrappingopuint16)

## UInt32

功能：表示 32 位无符号整型，表示范围为 [0, 2^{32} - 1]。

扩展：

- [extend UInt32 <: BigEndianOrder\<UInt32> & LittleEndianOrder\<UInt32> & SwapEndianOrder\<UInt32>](../../binary/binary_package_api/binary_package_extends.md#extend-uint32--bigendianorderuint32--littleendianorderuint32--swapendianorderuint32)
- [extend UInt32 <: Formattable](../../convert/convert_package_api/convert_package_extends.md#extend-uint32--formattable)
- [extend UInt32 <: Parsable\<UInt32>](../../convert/convert_package_api/convert_package_extends.md#extend-uint32--parsableuint32)
- [extend UInt32 <: RadixConvertible\<UInt32>](../../convert/convert_package_api/convert_package_extends.md#extend-uint32--radixconvertibleuint32)
- [extend UInt32 <: Comparable\<UInt32>](core_package_extends.md#extend-uint32--comparableuint32)
- [extend UInt32 <: Countable\<UInt32>](core_package_extends.md#extend-uint32--countableuint32)
- [extend UInt32 <: Hashable](core_package_extends.md#extend-uint32--hashable)
- [extend UInt32 <: ToString](core_package_extends.md#extend-uint32--tostring)
- [extend UInt32 <: Integer\<UInt32>](../../math/math_package_api/math_package_extends.md#extend-uint32--integeruint32)
- [extend UInt32 <: MaxMinValue\<UInt32>](../../math/math_package_api/math_package_extends.md#extend-uint32--maxminvalueuint32)
- [extend UInt32 <: Number\<UInt32>](../../math/math_package_api/math_package_extends.md#extend-uint32--numberuint32)
- [extend UInt32 <: CarryingOp\<UInt32>](../../overflow/overflow_package_api/overflow_package_extends.md#extend-uint32--carryingopuint32)
- [extend UInt32 <: CheckedOp\<UInt32>](../../overflow/overflow_package_api/overflow_package_extends.md#extend-uint32--checkedopuint32)
- [extend UInt32 <: SaturatingOp\<UInt32>](../../overflow/overflow_package_api/overflow_package_extends.md#extend-uint32--saturatingopuint32)
- [extend UInt32 <: ThrowingOp\<UInt32>](../../overflow/overflow_package_api/overflow_package_extends.md#extend-uint32--throwingopuint32)
- [extend UInt32 <: WrappingOp\<UInt32>](../../overflow/overflow_package_api/overflow_package_extends.md#extend-uint32--wrappingopuint32)

## UInt64

功能：表示 64 位无符号整型，表示范围为 [0, 2^{64} - 1]。

扩展：

- [extend UInt64 <: BigEndianOrder\<UInt64> & LittleEndianOrder\<UInt64> & SwapEndianOrder\<UInt64>](../../binary/binary_package_api/binary_package_extends.md#extend-uint64--bigendianorderuint64--littleendianorderuint64--swapendianorderuint64)
- [extend UInt64 <: Formattable](../../convert/convert_package_api/convert_package_extends.md#extend-uint64--formattable)
- [extend UInt64 <: Parsable\<UInt64>](../../convert/convert_package_api/convert_package_extends.md#extend-uint64--parsableuint64)
- [extend UInt64 <: RadixConvertible\<UInt64>](../../convert/convert_package_api/convert_package_extends.md#extend-uint64--radixconvertibleuint64)
- [extend UInt64 <: Comparable\<UInt64>](core_package_extends.md#extend-uint64--comparableuint64)
- [extend UInt64 <: Countable\<UInt64>](core_package_extends.md#extend-uint64--countableuint64)
- [extend UInt64 <: Hashable](core_package_extends.md#extend-uint64--hashable)
- [extend UInt64 <: ToString](core_package_extends.md#extend-uint64--tostring)
- [extend UInt64 <: Integer\<UInt64>](../../math/math_package_api/math_package_extends.md#extend-uint64--integeruint64)
- [extend UInt64 <: MaxMinValue\<UInt64>](../../math/math_package_api/math_package_extends.md#extend-uint64--maxminvalueuint64)
- [extend UInt64 <: Number\<UInt64>](../../math/math_package_api/math_package_extends.md#extend-uint64--numberuint64)
- [extend UInt64 <: CarryingOp\<UInt64>](../../overflow/overflow_package_api/overflow_package_extends.md#extend-uint64--carryingopuint64)
- [extend UInt64 <: CheckedOp\<UInt64>](../../overflow/overflow_package_api/overflow_package_extends.md#extend-uint64--checkedopuint64)
- [extend UInt64 <: SaturatingOp\<UInt64>](../../overflow/overflow_package_api/overflow_package_extends.md#extend-uint64--saturatingopuint64)
- [extend UInt64 <: ThrowingOp\<UInt64>](../../overflow/overflow_package_api/overflow_package_extends.md#extend-uint64--throwingopuint64)
- [extend UInt64 <: WrappingOp\<UInt64>](../../overflow/overflow_package_api/overflow_package_extends.md#extend-uint64--wrappingopuint64)

## UInt8

功能：表示 8 位无符号整型，表示范围为 [0, 2^8 - 1]。

扩展：

- [extend UInt8 <: BigEndianOrder\<UInt8> & LittleEndianOrder\<UInt8> & SwapEndianOrder\<UInt8>](../../binary/binary_package_api/binary_package_extends.md#extend-uint8--bigendianorderuint8--littleendianorderuint8--swapendianorderuint8)
- [extend UInt8 <: Formattable](../../convert/convert_package_api/convert_package_extends.md#extend-uint8--formattable)
- [extend UInt8 <: Parsable\<UInt8>](../../convert/convert_package_api/convert_package_extends.md#extend-uint8--parsableuint8)
- [extend UInt8 <: RadixConvertible\<UInt8>](../../convert/convert_package_api/convert_package_extends.md#extend-uint8--radixconvertibleuint8)
- [extend UInt8 <: Comparable\<UInt8>](core_package_extends.md#extend-uint8--comparableuint8)
- [extend UInt8 <: Countable\<UInt8>](core_package_extends.md#extend-uint8--countableuint8)
- [extend UInt8 <: Hashable](core_package_extends.md#extend-uint8--hashable)
- [extend UInt8 <: ToString](core_package_extends.md#extend-uint8--tostring)
- [extend UInt8 <: Integer\<UInt8>](../../math/math_package_api/math_package_extends.md#extend-uint8--integeruint8)
- [extend UInt8 <: MaxMinValue\<UInt8>](../../math/math_package_api/math_package_extends.md#extend-uint8--maxminvalueuint8)
- [extend UInt8 <: Number\<UInt8>](../../math/math_package_api/math_package_extends.md#extend-uint8--numberuint8)
- [extend UInt8 <: CarryingOp\<UInt8>](../../overflow/overflow_package_api/overflow_package_extends.md#extend-uint8--carryingopuint8)
- [extend UInt8 <: CheckedOp\<UInt8>](../../overflow/overflow_package_api/overflow_package_extends.md#extend-uint8--checkedopuint8)
- [extend UInt8 <: SaturatingOp\<UInt8>](../../overflow/overflow_package_api/overflow_package_extends.md#extend-uint8--saturatingopuint8)
- [extend UInt8 <: ThrowingOp\<UInt8>](../../overflow/overflow_package_api/overflow_package_extends.md#extend-uint8--throwingopuint8)
- [extend UInt8 <: WrappingOp\<UInt8>](../../overflow/overflow_package_api/overflow_package_extends.md#extend-uint8--wrappingopuint8)

## UIntNative

功能：表示平台相关的无符号整型，其长度与当前系统的位宽一致。

扩展：

- [extend UIntNative <: Comparable\<UIntNative>](core_package_extends.md#extend-uintnative--comparableuintnative)
- [extend UIntNative <: Countable\<UIntNative>](core_package_extends.md#extend-uintnative--countableuintnative)
- [extend UIntNative <: Hashable](core_package_extends.md#extend-uintnative--hashable)
- [extend UIntNative <: ToString](core_package_extends.md#extend-uintnative--tostring)
- [extend UIntNative <: Integer\<UIntNative>](../../math/math_package_api/math_package_extends.md#extend-uintnative--integeruintnative)
- [extend UIntNative <: MaxMinValue\<UIntNative>](../../math/math_package_api/math_package_extends.md#extend-uintnative--maxminvalueuintnative)
- [extend UIntNative <: Number\<UIntNative>](../../math/math_package_api/math_package_extends.md#extend-uintnative--numberuintnative)
- [extend UIntNative <: CarryingOp\<UIntNative>](../../overflow/overflow_package_api/overflow_package_extends.md#extend-uintnative--carryingopuintnative)
- [extend UIntNative <: CheckedOp\<UIntNative>](../../overflow/overflow_package_api/overflow_package_extends.md#extend-uintnative--checkedopuintnative)
- [extend UIntNative <: SaturatingOp\<UIntNative>](../../overflow/overflow_package_api/overflow_package_extends.md#extend-uintnative--saturatingopuintnative)
- [extend UIntNative <: ThrowingOp\<UIntNative>](../../overflow/overflow_package_api/overflow_package_extends.md#extend-uintnative--throwingopuintnative)
- [extend UIntNative <: WrappingOp\<UIntNative>](../../overflow/overflow_package_api/overflow_package_extends.md#extend-uintnative--wrappingopuintnative)

## Unit

功能：表示仓颉语言中只关心副作用而不关心值的表达式的类型。

扩展：

- [extend Unit <: Equatable\<Unit>](core_package_extends.md#extend-unit--equatableunit)
- [extend Unit <: Hashable](core_package_extends.md#extend-unit--hashable)
- [extend Unit <: ToString](core_package_extends.md#extend-unit--tostring)

例如，print 函数、赋值表达式、复合赋值表达式、自增和自减表达式、循环表达式，它们的类型都是 [Unit](core_package_intrinsics.md#unit)。

[Unit](core_package_intrinsics.md#unit) 类型只有一个值，也是它的字面量：()。除了赋值、判等和判不等外，[Unit](core_package_intrinsics.md#unit) 类型不支持其他操作。

