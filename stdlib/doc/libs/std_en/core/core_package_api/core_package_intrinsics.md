# Built-in Types

## Bool

Function: Represents the boolean type with two possible values: `true` and `false`.

Extensions:

- [extend Bool <: BigEndianOrder\<Bool> & LittleEndianOrder\<Bool>](../../binary/binary_package_api/binary_package_extends.md#extend-bool--bigendianorderbool--littleendianorderbool)

- [extend Bool <: Parsable\<Bool>](../../convert/convert_package_api/convert_package_extends.md#extend-bool--parsablebool) (cross-package extension)
- [extend Bool <: Equatable\<Bool>](core_package_extends.md#extend-bool--equatablebool)
- [extend Bool <: Hashable](core_package_extends.md#extend-bool--hashable)
- [extend Bool <: ToString](core_package_extends.md#extend-bool--tostring)

## CPointer\<T>

Function: Represents a pointer to an instance of type `T`, used for interoperability with C language, corresponding to C's `T*`.

Extensions:

- [extend\<T> CPointer\<T>](core_package_extends.md#extendt-cpointert)

The type `T` must satisfy the [CType](core_package_interfaces.md#interface-ctype) constraint.

The [CPointer](core_package_intrinsics.md#cpointert) type must satisfy:

- Size and alignment are platform-dependent.
- Pointer arithmetic operations and memory read/write require an unsafe context.
- [CPointer](core_package_intrinsics.md#cpointert)\<T1> can be type-cast to [CPointer](core_package_intrinsics.md#cpointert)\<T2> in an unsafe context.

## CString

Function: Represents C-style strings, used in scenarios requiring interoperability with C language.

Extensions:

- [extend CString <: ToString](core_package_extends.md#extend-cstring--tostring)

C-style strings can be created through the constructor of [CString](core_package_intrinsics.md#cstring) or the `mallocCString` method of [LibC](core_package_structs.md#struct-libc). To release memory on the Cangjie side, call the free method of [LibC](core_package_structs.md#struct-libc).

## Float16

Function: Represents a 16-bit floating-point number conforming to the half-precision format (`binary16`) in `IEEE 754`.

Extensions:

- [extend Float16 <: BigEndianOrder\<Float16> & LittleEndianOrder\<Float16>](../../binary/binary_package_api/binary_package_extends.md#extend-float16--bigendianorderfloat16--littleendianorderfloat16)

- [extend Float16 <: Formattable](../../convert/convert_package_api/convert_package_extends.md#extend-float16--formattable) (cross-package extension)
- [extend Float16 <: Parsable\<Float16>](../../convert/convert_package_api/convert_package_extends.md#extend-float16--parsablefloat16) (cross-package extension)
- [extend Float16 <: Comparable\<Float16>](core_package_extends.md#extend-float16--comparablefloat16)
- [extend Float16 <: Hashable](core_package_extends.md#extend-float16--hashable)
- [extend Float16 <: ToString](core_package_extends.md#extend-float16--tostring)
- [extend Float16](core_package_extends.md#extend-float16)
- [extend Float16 <: FloatingPoint\<Float16>](../../math/math_package_api/math_package_extends.md#extend-float16--floatingpointfloat16) (cross-package extension)
- [extend Float16 <: MathExtension\<Float16>](../../math/math_package_api/math_package_extends.md#extend-float16--mathextensionfloat16) (cross-package extension)
- [extend Float16 <: MaxMinValue\<Float16>](../../math/math_package_api/math_package_extends.md#extend-float16--maxminvaluefloat16) (cross-package extension)
- [extend Float16 <: Number\<Float16>](../../math/math_package_api/math_package_extends.md#extend-float16--numberfloat16) (cross-package extension)

## Float32

Function: Represents a 32-bit floating-point number, conforming to the single-precision format (`binary32`) in `IEEE 754`.

Extensions:

- [extend Float32 <: BigEndianOrder\<Float32> & LittleEndianOrder\<Float32>](../../binary/binary_package_api/binary_package_extends.md#extend-float32--bigendianorderfloat32--littleendianorderfloat32)

- [extend Float32 <: Formattable](../../convert/convert_package_api/convert_package_extends.md#extend-float32--formattable) (cross-package extension)
- [extend Float32 <: Parsable\<Float32>](../../convert/convert_package_api/convert_package_extends.md#extend-float32--parsablefloat32) (cross-package extension)
- [extend Float32 <: Comparable\<Float32>](core_package_extends.md#extend-float32--comparablefloat32)
- [extend Float32 <: Hashable](core_package_extends.md#extend-float32--hashable)
- [extend Float32 <: ToString](core_package_extends.md#extend-float32--tostring)
- [extend Float32](core_package_extends.md#extend-float32)
- [extend Float32 <: FloatingPoint\<Float32>](../../math/math_package_api/math_package_extends.md#extend-float32--floatingpointfloat32) (cross-package extension)
- [extend Float32 <: MathExtension\<Float32>](../../math/math_package_api/math_package_extends.md#extend-float32--mathextensionfloat32) (cross-package extension)
- [extend Float32 <: MaxMinValue\<Float32>](../../math/math_package_api/math_package_extends.md#extend-float32--maxminvaluefloat32) (cross-package extension)
- [extend Float32 <: Number\<Float32>](../../math/math_package_api/math_package_extends.md#extend-float32--numberfloat32) (cross-package extension)

## Float64

Function: Represents a 64-bit floating-point number, conforming to the double-precision format (`binary64`) in `IEEE 754`.

Extensions:

- [extend Float64 <: BigEndianOrder\<Float64> & LittleEndianOrder\<Float64>](../../binary/binary_package_api/binary_package_extends.md#extend-float64--bigendianorderfloat64--littleendianorderfloat64)

- [extend Float64 <: Formattable](../../convert/convert_package_api/convert_package_extends.md#extend-float64--formattable) (cross-package extension)
- [extend Float64 <: Parsable\<Float64>](../../convert/convert_package_api/convert_package_extends.md#extend-float64--parsablefloat64) (cross-package extension)
- [extend Float64 <: Comparable\<Float64>](core_package_extends.md#extend-float64--comparablefloat64)
- [extend Float64 <: Hashable](core_package_extends.md#extend-float64--hashable)
- [extend Float64 <: ToString](core_package_extends.md#extend-float64--tostring)
- [extend Float64](core_package_extends.md#extend-float64)
- [extend Float64](core_package_extends.md#extend-float64-1)
- [extend Float64 <: FloatingPoint\<Float64>](../../math/math_package_api/math_package_extends.md#extend-float64--floatingpointfloat64) (cross-package extension)
- [extend Float64 <: MathExtension\<Float64>](../../math/math_package_api/math_package_extends.md#extend-float64--mathextensionfloat64) (cross-package extension)
- [extend Float64 <: MaxMinValue\<Float64>](../../math/math_package_api/math_package_extends.md#extend-float64--maxminvaluefloat64) (cross-package extension)
- [extend Float64 <: Number\<Float64>](../../math/math_package_api/math_package_extends.md#extend-float64--numberfloat64) (cross-package extension)

## Int16

Function: Represents a 16-bit signed integer with a range of [-2^{15}, 2^{15} - 1].

Extensions:

- [extend Int16 <: BigEndianOrder\<Int16> & LittleEndianOrder\<Int16> & SwapEndianOrder\<Int16>](../../binary/binary_package_api/binary_package_extends.md#extend-int16--bigendianorderint16--littleendianorderint16--swapendianorderint16)

- [extend Int16 <: Formattable](../../convert/convert_package_api/convert_package_extends.md#extend-int16--formattable) (cross-package extension)
- [extend Int16 <: Parsable\<Int16>](../../convert/convert_package_api/convert_package_extends.md#extend-int16--parsableint16) (cross-package extension)
- [extend Int16 <: RadixConvertible\<Int16>](../../convert/convert_package_api/convert_package_extends.md#extend-int16--radixconvertibleint16) (cross-package extension)
- [extend Int16 <: Comparable\<Int16>](core_package_extends.md#extend-int16--comparableint16)
- [extend Int16 <: Countable\<Int16>](core_package_extends.md#extend-int16--countableint16)
- [extend Int16 <: Hashable](core_package_extends.md#extend-int16--hashable)
- [extend Int16 <: ToString](core_package_extends.md#extend-int16--tostring)
- [extend Int16 <: Integer\<Int16>](../../math/math_package_api/math_package_extends.md#extend-int16--integerint16) (cross-package extension)
- [extend Int16 <: MaxMinValue\<Int16>](../../math/math_package_api/math_package_extends.md#extend-int16--maxminvalueint16) (cross-package extension)
- [extend Int16 <: Number\<Int16>](../../math/math_package_api/math_package_extends.md#extend-int16--numberint16) (cross-package extension)
- [extend Int16 <: CarryingOp\<Int16>](../../overflow/overflow_package_api/overflow_package_extends.md#extend-int16--carryingopint16) (cross-package extension)
- [extend Int16 <: CheckedOp\<Int16>](../../overflow/overflow_package_api/overflow_package_extends.md#extend-int16--checkedopint16) (cross-package extension)
- [extend Int16 <: SaturatingOp\<Int16>](../../overflow/overflow_package_api/overflow_package_extends.md#extend-int16--saturatingopint16) (cross-package extension)
- [extend Int16 <: ThrowingOp\<Int16>](../../overflow/overflow_package_api/overflow_package_extends.md#extend-int16--throwingopint16) (cross-package extension)
- [extend Int16 <: WrappingOp\<Int16>](../../overflow/overflow_package_api/overflow_package_extends.md#extend-int16--wrappingopint16) (cross-package extension)

## Int32

Function: Represents a 32-bit signed integer with a range of [-2^{31}, 2^{31} - 1].

Extensions:

- [extend Int32 <: BigEndianOrder\<Int32> & LittleEndianOrder\<Int32> & SwapEndianOrder\<Int32>](../../binary/binary_package_api/binary_package_extends.md#extend-int32--bigendianorderint32--littleendianorderint32--swapendianorderint32) (cross-package extension)

- [extend Int32 <: Formattable](../../convert/convert_package_api/convert_package_extends.md#extend-int32--formattable) (cross-package extension)
- [extend Int32 <: Parsable\<Int32>](../../convert/convert_package_api/convert_package_extends.md#extend-int32--parsableint32) (cross-package extension)
- [extend Int32 <: RadixConvertible\<Int32>](../../convert/convert_package_api/convert_package_extends.md#extend-int32--radixconvertibleint32) (cross-package extension)
- [extend Int32 <: Comparable\<Int32>](core_package_extends.md#extend-int32--comparableint32)
- [extend Int32 <: Countable\<Int32>](core_package_extends.md#extend-int32--countableint32)
- [extend Int32 <: Hashable](core_package_extends.md#extend-int32--hashable)
- [extend Int32 <: ToString](core_package_extends.md#extend-int32--tostring)
- [extend Int32 <: Integer\<Int32>](../../math/math_package_api/math_package_extends.md#extend-int32--integerint32) (cross-package extension)
- [extend Int32 <: MaxMinValue\<Int32>](../../math/math_package_api/math_package_extends.md#extend-int32--maxminvalueint32) (cross-package extension)
- [extend Int32 <: Number\<Int32>](../../math/math_package_api/math_package_extends.md#extend-int32--numberint32) (cross-package extension)
- [extend Int32 <: CarryingOp\<Int32>](../../overflow/overflow_package_api/overflow_package_extends.md#extend-int32--carryingopint32) (cross-package extension)
- [extend Int32 <: CheckedOp\<Int32>](../../overflow/overflow_package_api/overflow_package_extends.md#extend-int32--checkedopint32) (cross-package extension)
- [extend Int32 <: SaturatingOp\<Int32>](../../overflow/overflow_package_api/overflow_package_extends.md#extend-int32--saturatingopint32) (cross-package extension)
- [extend Int32 <: ThrowingOp\<Int32>](../../overflow/overflow_package_api/overflow_package_extends.md#extend-int32--throwingopint32) (cross-package extension)
- [extend Int32 <: WrappingOp\<Int32>](../../overflow/overflow_package_api/overflow_package_extends.md#extend-int32--wrappingopint32) (cross-package extension)

## Int64

Function: Represents a 64-bit signed integer with a range of [-2^{63}, 2^{63} - 1].

Extensions:

- [extend Int64 <: BigEndianOrder\<Int64> & LittleEndianOrder\<Int64> & SwapEndianOrder\<Int64>](../../binary/binary_package_api/binary_package_extends.md#extend-int64--bigendianorderint64--littleendianorderint64--swapendianorderint64) (cross-package extension)

- [extend Int64 <: Formattable](../../convert/convert_package_api/convert_package_extends.md#extend-int64--formattable) (cross-package extension)
- [extend Int64 <: Parsable\<Int64>](../../convert/convert_package_api/convert_package_extends.md#extend-int64--parsableint64) (cross-package extension)
- [extend Int64 <: RadixConvertible\<Int64>](../../convert/convert_package_api/convert_package_extends.md#extend-int64--radixconvertibleint64) (cross-package extension)
- [extend Int64 <: Comparable\<Int64>](core_package_extends.md#extend-int64--comparableint64)
- [extend Int64 <: Countable\<Int64>](core_package_extends.md#extend-int64--countableint64)
- [extend Int64 <: Hashable](core_package_extends.md#extend-int64--hashable)
- [extend Int64 <: ToString](core_package_extends.md#extend-int64--tostring)
- [extend Int64](core_package_extends.md#extend-int64)
- [extend Int64 <: Integer\<Int64>](../../math/math_package_api/math_package_extends.md#extend-int64--integerint64) (cross-package extension)
- [extend Int64 <: MaxMinValue\<Int64>](../../math/math_package_api/math_package_extends.md#extend-int64--maxminvalueint64) (cross-package extension)
- [extend Int64 <: Number\<Int64>](../../math/math_package_api/math_package_extends.md#extend-int64--numberint64) (cross-package extension)
- [extend Int64 <: CarryingOp\<Int64> & CarryingPow](../../overflow/overflow_package_api/overflow_package_extends.md#extend-int64--carryingopint64--carryingpow) (cross-package extension)
- [extend Int64 <: CheckedOp\<Int64> & CheckedPow](../../overflow/overflow_package_api/overflow_package_extends.md#extend-int64--checkedopint64--checkedpow) (cross-package extension)
- [extend Int64 <: SaturatingOp\<Int64> & SaturatingPow](../../overflow/overflow_package_api/overflow_package_extends.md#extend-int64--saturatingopint64--saturatingpow) (cross-package extension)
- [extend Int64 <: ThrowingOp\<Int64> & ThrowingPow](../../overflow/overflow_package_api/overflow_package_extends.md#extend-int64--throwingopint64--throwingpow) (cross-package extension)
- [extend Int64 <: WrappingOp\<Int64> & WrappingPow](../../overflow/overflow_package_api/overflow_package_extends.md#extend-int64--wrappingopint64--wrappingpow) (cross-package extension)

## Int8

Function: Represents an 8-bit signed integer with a range of [-2^7, 2^7 - 1].

Extensions:

- [extend Int8 <: BigEndianOrder\<Int8> & LittleEndianOrder\<Int8> & SwapEndianOrder\<Int8>](../../binary/binary_package_api/binary_package_extends.md#extend-int8--bigendianorderint8--littleendianorderint8--swapendianorderint8) (cross-package extension)

- [extend Int8 <: Formattable](../../convert/convert_package_api/convert_package_extends.md#extend-int8--formattable) (cross-package extension)
- [extend Int8 <: Parsable\<Int8>](../../convert/convert_package_api/convert_package_extends.md#extend-int8--parsableint8) (cross-package extension)
- [extend Int8 <: RadixConvertible\<Int8>](../../convert/convert_package_api/convert_package_extends.md#extend-int8--radixconvertibleint8) (cross-package extension)
- [extend Int8 <: Comparable\<Int8>](core_package_extends.md#extend-int8--comparableint8)
- [extend Int8 <: Countable\<Int8>](core_package_extends.md#extend-int8--countableint8)
- [extend Int8 <: Hashable](core_package_extends.md#extend-int8--hashable)
- [extend Int8 <: ToString](core_package_extends.md#extend-int8--tostring)
- [extend Int8 <: Integer\<Int8>](../../math/math_package_api/math_package_extends.md#extend-int8--integerint8) (cross-package extension)
- [extend Int8 <: MaxMinValue\<Int8>](../../math/math_package_api/math_package_extends.md#extend-int8--maxminvalueint8) (cross-package extension)
- [extend Int8 <: Number\<Int8>](../../math/math_package_api/math_package_extends.md#extend-int8--numberint8) (cross-package extension)
- [extend Int8 <: CarryingOp\<Int8>](../../overflow/overflow_package_api/overflow_package_extends.md#extend-int8--carryingopint8) (cross-package extension)
- [extend Int8 <: CheckedOp\<Int8>](../../overflow/overflow_package_api/overflow_package_extends.md#extend-int8--checkedopint8) (cross-package extension)
- [extend Int8 <: SaturatingOp\<Int8>](../../overflow/overflow_package_api/overflow_package_extends.md#extend-int8--saturatingopint8) (cross-package extension)
- [extend Int8 <: ThrowingOp\<Int8>](../../overflow/overflow_package_api/overflow_package_extends.md#extend-int8--throwingopint8) (cross-package extension)
- [extend Int8 <: WrappingOp\<Int8>](../../overflow/overflow_package_api/overflow_package_extends.md#extend-int8--wrappingopint8) (cross-package extension)

## IntNative

Function: Represents a platform-dependent signed integer type whose length matches the bit width of the current system.

Extensions:

- [extend IntNative <: Comparable\<IntNative>](core_package_extends.md#extend-intnative--comparableintnative)
- [extend IntNative <: Countable\<IntNative>](core_package_extends.md#extend-intnative--countableintnative)
- [extend IntNative <: Hashable](core_package_extends.md#extend-intnative--hashable)
- [extend IntNative <: ToString](core_package_extends.md#extend-intnative--tostring)

- [extend IntNative <: Integer\<IntNative>](../../math/math_package_api/math_package_extends.md#extend-intnative--integerintnative) (cross-package extension)
- [extend IntNative <: MaxMinValue\<IntNative>](../../math/math_package_api/math_package_extends.md#extend-intnative--maxminvalueintnative) (cross-package extension)
- [extend IntNative <: Number\<IntNative>](../../math/math_package_api/math_package_extends.md#extend-intnative--numberintnative) (cross-package extension)
- [extend IntNative <: CarryingOp\<IntNative>](../../overflow/overflow_package_api/overflow_package_extends.md#extend-intnative--carryingopintnative) (cross-package extension)
- [extend IntNative <: CheckedOp\<IntNative>](../../overflow/overflow_package_api/overflow_package_extends.md#extend-intnative--checkedopintnative) (cross-package extension)
- [extend IntNative <: SaturatingOp\<IntNative>](../../overflow/overflow_package_api/overflow_package_extends.md#extend-intnative--saturatingopintnative) (cross-package extension)
- [extend IntNative <: ThrowingOp\<IntNative>](../../overflow/overflow_package_api/overflow_package_extends.md#extend-intnative--throwingopintnative) (cross-package extension)
- [extend IntNative <: WrappingOp\<IntNative>](../../overflow/overflow_package_api/overflow_package_extends.md#extend-intnative--wrappingopintnative) (cross-package extension)

## Rune

Function: Represents a character in the Unicode character set.

The range is `Unicode scalar value`, i.e., characters from `\u{0000}` to `\u{D7FF}`, and from `\u{E000}` to `\u{10FFF}`.

Extensions:

- [extend Rune <: Formattable](../../convert/convert_package_api/convert_package_extends.md#extend-rune--formattable) (cross-package extension)
- [extend Rune <: Parsable\<Rune>](../../convert/convert_package_api/convert_package_extends.md#extend-rune--parsablerune) (cross-package extension)

- [extend Rune](core_package_extends.md#extend-rune)
- [extend Rune <: Comparable\<Rune>](core_package_extends.md#extend-rune--comparablerune)
- [extend Rune <: Countable\<Rune>](core_package_extends.md#extend-rune--countablerune)
- [extend Rune <: Hashable](core_package_extends.md#extend-rune--hashable)
- [extend Rune <: ToString](core_package_extends.md#extend-rune--tostring)
- [extend Rune <: UnicodeRuneExtension](../../unicode/unicode_package_api/unicode_package_extends.md#extend-rune--unicoderuneextension) (cross-package extension)

## UInt16

Function: Represents a 16-bit unsigned integer with a range of [0, 2^{16} - 1].

Extensions:

- [extend UInt16 <: BigEndianOrder\<UInt16> & LittleEndianOrder\<UInt16> & SwapEndianOrder\<UInt16>](../../binary/binary_package_api/binary_package_extends.md#extend-uint16--bigendianorderuint16--littleendianorderuint16--swapendianorderuint16) (cross-package extension)

- [extend UInt16 <: Formattable](../../convert/convert_package_api/convert_package_extends.md#extend-uint16--formattable) (cross-package extension)
- [extend UInt16 <: Parsable\<UInt16>](../../convert/convert_package_api/convert_package_extends.md#extend-uint16--parsableuint16) (cross-package extension)
- [extend UInt16 <: RadixConvertible\<UInt16>](../../convert/convert_package_api/convert_package_extends.md#extend-uint16--radixconvertibleuint16) (cross-package extension)
- [extend UInt16 <: Comparable\<UInt16>](core_package_extends.md#extend-uint16--comparableuint16)
- [extend UInt16 <: Countable\<UInt16>](core_package_extends.md#extend-uint16--countableuint16)
- [extend UInt16 <: Hashable](core_package_extends.md#extend-uint16--hashable)
- [extend UInt16 <: ToString](core_package_extends.md#extend-uint16--tostring)
- [extend UInt16 <: Integer\<UInt16>](../../math/math_package_api/math_package_extends.md#extend-uint16--integeruint16) (cross-package extension)
- [extend UInt16 <: MaxMinValue\<UInt16>](../../math/math_package_api/math_package_extends.md#extend-uint16--maxminvalueuint16) (cross-package extension)
- [extend UInt16 <: Number\<UInt16>](../../math/math_package_api/math_package_extends.md#extend-uint16--numberuint16) (cross-package extension)
- [extend UInt16 <: CarryingOp\<UInt16>](../../overflow/overflow_package_api/overflow_package_extends.md#extend-uint16--carryingopuint16) (cross-package extension)
- [extend UInt16 <: CheckedOp\<UInt16>](../../overflow/overflow_package_api/overflow_package_extends.md#extend-uint16--checkedopuint16) (cross-package extension)
- [extend UInt16 <: ThrowingOp\<UInt16>](../../overflow/overflow_package_api/overflow_package_extends.md#extend-uint16--throwingopuint16) (cross-package extension)
- [extend UInt16 <: WrappingOp\<UInt16>](../../overflow/overflow_package_api/overflow_package_extends.md#extend-uint16--wrappingopuint16) (cross-package extension)

## UInt32

Function: Represents a 32-bit unsigned integer with a range of [0, 2^32 - 1].

Extensions:

- [extend UInt32 <: Formattable](../../convert/convert_package_api/convert_package_extends.md#extend-uint32--formattable) (cross-package extension)
- [extend UInt32 <: Parsable\<UInt32>](../../convert/convert_package_api/convert_package_extends.md#extend-uint32--parsableuint32) (cross-package extension)
- [extend UInt32 <: RadixConvertible\<UInt32>](../../convert/convert_package_api/convert_package_extends.md#extend-uint32--radixconvertibleuint32) (cross-package extension)

- [extend UInt32 <: Comparable\<UInt32>](core_package_extends.md#extend-uint32--comparableuint32)
- [extend UInt32 <: Countable\<UInt32>](core_package_extends.md#extend-uint32--countableuint32)
- [extend UInt32 <: Hashable](core_package_extends.md#extend-uint32--hashable)
- [extend UInt32 <: ToString](core_package_extends.md#extend-uint32--tostring)
- [extend UInt32 <: Integer\<UInt32>](../../math/math_package_api/math_package_extends.md#extend-uint32--integeruint32) (cross-package extension)
- [extend UInt32 <: MaxMinValue\<UInt32>](../../math/math_package_api/math_package_extends.md#extend-uint32--maxminvalueuint32) (cross-package extension)
- [extend UInt32 <: Number\<UInt32>](../../math/math_package_api/math_package_extends.md#extend-uint32--numberuint32) (cross-package extension)
- [extend UInt32 <: CarryingOp\<UInt32>](../../overflow/overflow_package_api/overflow_package_extends.md#extend-uint32--carryingopuint32) (cross-package extension)
- [extend UInt32 <: CheckedOp\<UInt32>](../../overflow/overflow_package_api/overflow_package_extends.md#extend-uint32--checkedopuint32) (cross-package extension)
- [extend UInt32 <: SaturatingOp\<UInt32>](../../overflow/overflow_package_api/overflow_package_extends.md#extend-uint32--saturatingopuint32) (cross-package extension)
- [extend UInt32 <: ThrowingOp\<UInt32>](../../overflow/overflow_package_api/overflow_package_extends.md#extend-uint32--throwingopuint32) (cross-package extension)
- [extend UInt32 <: WrappingOp\<UInt32>](../../overflow/overflow_package_api/overflow_package_extends.md#extend-uint32--wrappingopuint32) (cross-package extension)

## UInt64

Function: Represents a 64-bit unsigned integer with a range of [0, 2^64 - 1].

Extensions:

- [extend UInt64 <: SwapEndianOrder\<UInt64>](../../binary/binary_package_api/binary_package_extends.md#extend-uint64--swapendianorderuint64)

- [extend UInt64 <: Formattable](../../convert/convert_package_api/convert_package_extends.md#extend-uint64--formattable) (cross-package extension)
- [extend UInt64 <: Parsable\<UInt64>](../../convert/convert_package_api/convert_package_extends.md#extend-uint64--parsableuint64) (cross-package extension)
- [extend UInt64 <: RadixConvertible\<UInt64>](../../convert/convert_package_api/convert_package_extends.md#extend-uint64--radixconvertibleuint64) (cross-package extension)
- [extend UInt64 <: Comparable\<UInt64>](core_package_extends.md#extend-uint64--comparableuint64)
- [extend UInt64 <: Countable\<UInt64>](core_package_extends.md#extend-uint64--countableuint64)
- [extend UInt64 <: Hashable](core_package_extends.md#extend-uint64--hashable)
- [extend UInt64 <: ToString](core_package_extends.md#extend-uint64--tostring)
- [extend UInt64 <: Integer\<UInt64>](../../math/math_package_api/math_package_extends.md#extend-uint64--integeruint64) (cross-package extension)
- [extend UInt64 <: MaxMinValue\<UInt64>](../../math/math_package_api/math_package_extends.md#extend-uint64--maxminvalueuint64) (cross-package extension)
- [extend UInt64 <: Number\<UInt64>](../../math/math_package_api/math_package_extends.md#extend-uint64--numberuint64) (cross-package extension)
- [extend UInt64 <: CarryingOp\<UInt64>](../../overflow/overflow_package_api/overflow_package_extends.md#extend-uint64--carryingopuint64) (cross-package extension)
- [extend UInt64 <: CheckedOp\<UInt64>](../../overflow/overflow_package_api/overflow_package_extends.md#extend-uint64--checkedopuint64) (cross-package extension)
- [extend UInt64 <: SaturatingOp\<UInt64>](../../overflow/overflow_package_api/overflow_package_extends.md#extend-uint64--saturatingopuint64) (cross-package extension)
- [extend UInt64 <: ThrowingOp\<UInt64>](../../overflow/overflow_package_api/overflow_package_extends.md#extend-uint64--throwingopuint64) (cross-package extension)
- [extend UInt64 <: WrappingOp\<UInt64>](../../overflow/overflow_package_api/overflow_package_extends.md#extend-uint64--wrappingopuint64) (cross-package extension)

## UInt8

Function: Represents an 8-bit unsigned integer with a range of [0, 2^8 - 1].

Extensions:

- [extend UInt8 <: SwapEndianOrder\<UInt8>](../../binary/binary_package_api/binary_package_extends.md#extend-uint8--swapendianorderuint8)

- [extend UInt8 <: Formattable](../../convert/convert_package_api/convert_package_extends.md#extend-uint8--formattable) (cross-package extension)
- [extend UInt8 <: Parsable\<UInt8>](../../convert/convert_package_api/convert_package_extends.md#extend-uint8--parsableuint8) (cross-package extension)
- [extend UInt8 <: RadixConvertible\<UInt8>](../../convert/convert_package_api/convert_package_extends.md#extend-uint8--radixconvertibleuint8) (cross-package extension)
- [extend UInt8 <: Comparable\<UInt8>](core_package_extends.md#extend-uint8--comparableuint8)
- [extend UInt8 <: Countable\<UInt8>](core_package_extends.md#extend-uint8--countableuint8)
- [extend UInt8 <: Hashable](core_package_extends.md#extend-uint8--hashable)
- [extend UInt8 <: ToString](core_package_extends.md#extend-uint8--tostring)
- [extend UInt8 <: Integer\<UInt8>](../../math/math_package_api/math_package_extends.md#extend-uint8--integeruint8) (cross-package extension)
- [extend UInt8 <: MaxMinValue\<UInt8>](../../math/math_package_api/math_package_extends.md#extend-uint8--maxminvalueuint8) (cross-package extension)
- [extend UInt8 <: Number\<UInt8>](../../math/math_package_api/math_package_extends.md#extend-uint8--numberuint8) (cross-package extension)
- [extend UInt8 <: CarryingOp\<UInt8>](../../overflow/overflow_package_api/overflow_package_extends.md#extend-uint8--carryingopuint8) (cross-package extension)
- [extend UInt8 <: SaturatingOp\<UInt8>](../../overflow/overflow_package_api/overflow_package_extends.md#extend-uint8--saturatingopuint8) (cross-package extension)
- [extend UInt8 <: ThrowingOp\<UInt8>](../../overflow/overflow_package_api/overflow_package_extends.md#extend-uint8--throwingopuint8) (cross-package extension)
- [extend UInt8 <: WrappingOp\<UInt8>](../../overflow/overflow_package_api/overflow_package_extends.md#extend-uint8--wrappingopuint8) (cross-package extension)

## UIntNative

Function: Represents a platform-dependent unsigned integer whose length matches the current system's bit width.

Extensions:

- [extend UIntNative <: Comparable\<UIntNative>](core_package_extends.md#extend-uintnative--comparableuintnative)
- [extend UIntNative <: Countable](core_package_extends.md#extend-uintnative--countable)
- [extend UIntNative <: Hashable](core_package_extends.md#extend-uintnative--hashable)
- [extend UIntNative <: ToString](core_package_extends.md#extend-uintnative--tostring)

- [extend UIntNative <: Integer\<UIntNative>](../../math/math_package_api/math_package_extends.md#extend-uintnative--integeruintnative) (cross-package extension)
- [extend UIntNative <: MaxMinValue\<UIntNative>](../../math/math_package_api/math_package_extends.md#extend-uintnative--maxminvalueuintnative) (cross-package extension)
- [extend UIntNative <: Number\<UIntNative>](../../math/math_package_api/math_package_extends.md#extend-uintnative--numberuintnative) (cross-package extension)
- [extend UIntNative <: CarryingOp\<UIntNative>](../../overflow/overflow_package_api/overflow_package_extends.md#extend-uintnative--carryingopuintnative) (cross-package extension)
- [extend UIntNative <: CheckedOp\<UIntNative>](../../overflow/overflow_package_api/overflow_package_extends.md#extend-uintnative--checkedopuintnative) (cross-package extension)
- [extend UIntNative <: SaturatingOp\<UIntNative>](../../overflow/overflow_package_api/overflow_package_extends.md#extend-uintnative--saturatingopuintnative) (cross-package extension)
- [extend UIntNative <: ThrowingOp\<UIntNative>](../../overflow/overflow_package_api/overflow_package_extends.md#extend-uintnative--throwingopuintnative) (cross-package extension)
- [extend UIntNative <: WrappingOp\<UIntNative>](../../overflow/overflow_package_api/overflow_package_extends.md#extend-uintnative--wrappingopuintnative) (cross-package extension)

## Unit

Function: Represents the type of expressions in Cangjie language that only care about side effects and not about values.

Extensions:

- [extend Unit <: Equatable\<Unit>](core_package_extends.md#extend-unit--equatableunit)
- [extend Unit <: Hashable](core_package_extends.md#extend-unit--hashable)
- [extend Unit <: ToString](core_package_extends.md#extend-unit--tostring)

For example, the `print` function, assignment expressions, compound assignment expressions, increment and decrement expressions, and loop expressions all have the type [Unit](core_package_intrinsics.md#unit).

The [Unit](core_package_intrinsics.md#unit) type has only one value, which is also its literal: `()`. Apart from assignment, equality, and inequality comparisons, the [Unit](core_package_intrinsics.md#unit) type does not support other operations.

