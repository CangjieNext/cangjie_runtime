# Interfaces

## interface BigEndianOrder\<T>

```cangjie
public interface BigEndianOrder<T>
```

Function: Big-endian byte sequence conversion interface.

Extensions:

- [extend Bool <: BigEndianOrder\<Bool> & LittleEndianOrder\<Bool>](binary_package_extends.md#extend-bool--bigendianorderbool--littleendianorderbool) (cross-package extension)
- [extend Float16 <: BigEndianOrder\<Float16> & LittleEndianOrder\<Float16>](binary_package_extends.md#extend-float16--bigendianorderfloat16--littleendianorderfloat16) (cross-package extension)
- [extend Float32 <: BigEndianOrder\<Float32> & LittleEndianOrder\<Float32>](binary_package_extends.md#extend-float32--bigendianorderfloat32--littleendianorderfloat32) (cross-package extension)
- [extend Float64 <: BigEndianOrder\<Float64> & LittleEndianOrder\<Float64>](binary_package_extends.md#extend-float64--bigendianorderfloat64--littleendianorderfloat64) (cross-package extension)
- [extend Int32 <: BigEndianOrder\<Int32> & LittleEndianOrder\<Int32> & SwapEndianOrder\<Int32>](binary_package_extends.md#extend-int32--bigendianorderint32--littleendianorderint32--swapendianorderint32) (cross-package extension)
- [extend Int64 <: BigEndianOrder\<Int64> & LittleEndianOrder\<Int64> & SwapEndianOrder\<Int64>](binary_package_extends.md#extend-int64--bigendianorderint64--littleendianorderint64--swapendianorderint64) (cross-package extension)
- [extend Int8 <: BigEndianOrder\<Int8> & LittleEndianOrder\<Int8> & SwapEndianOrder\<Int8>](binary_package_extends.md#extend-int8--bigendianorderint8--littleendianorderint8--swapendianorderint8) (cross-package extension)
- [extend UInt16 <: BigEndianOrder\<UInt16> & LittleEndianOrder\<UInt16> & SwapEndianOrder\<UInt16>](binary_package_extends.md#extend-uint16--bigendianorderuint16--littleendianorderuint16--swapendianorderuint16) (cross-package extension)
- [extend UInt32 <: BigEndianOrder\<UInt32> & LittleEndianOrder\<UInt32> & SwapEndianOrder\<UInt32>](binary_package_extends.md#extend-uint32--bigendianorderuint32--littleendianorderuint32--swapendianorderuint32) (cross-package extension)

### static func readBigEndian(Array\<UInt8>)

```cangjie
static func readBigEndian(buffer: Array<UInt8>): T
```

Function: Reads a value of type T from a byte array in big-endian order.

Parameters:

- buffer: [Array](../../core/core_package_api/core_package_structs.md#struct-arrayt)\<[UInt8](../../core/core_package_api/core_package_intrinsics.md#uint8)> - Buffer containing the data to be read.

Returns:

- T - The value of type T.

### func writeBigEndian(Array\<UInt8>)

```cangjie
func writeBigEndian(buffer: Array<UInt8>): Int64
```

Function: Writes a value of type T to a byte array in big-endian order.

Parameters:

- buffer: [Array](../../core/core_package_api/core_package_structs.md#struct-arrayt)\<[UInt8](../../core/core_package_api/core_package_intrinsics.md#uint8)> - Buffer to store the data to be written.

Returns:

- [Int64](../../core/core_package_api/core_package_intrinsics.md#int64) - Number of bytes written.

## interface LittleEndianOrder\<T>

```cangjie
public interface LittleEndianOrder<T>
```

Function: Interface for little-endian byte sequence conversion.

Extensions:

- [extend Bool <: BigEndianOrder\<Bool> & LittleEndianOrder\<Bool>](binary_package_extends.md#extend-bool--bigendianorderbool--littleendianorderbool) (cross-package extension)
- [extend Float16 <: BigEndianOrder\<Float16> & LittleEndianOrder\<Float16>](binary_package_extends.md#extend-float16--bigendianorderfloat16--littleendianorderfloat16) (cross-package extension)
- [extend Float32 <: BigEndianOrder\<Float32> & LittleEndianOrder\<Float32>](binary_package_extends.md#extend-float32--bigendianorderfloat32--littleendianorderfloat32) (cross-package extension)
- [extend Float64 <: BigEndianOrder\<Float64> & LittleEndianOrder\<Float64>](binary_package_extends.md#extend-float64--bigendianorderfloat64--littleendianorderfloat64) (cross-package extension)
- [extend Int16 <: BigEndianOrder\<Int16> & LittleEndianOrder\<Int16> & SwapEndianOrder\<Int16>](binary_package_extends.md#extend-int16--bigendianorderint16--littleendianorderint16--swapendianorderint16) (cross-package extension)
- [extend Int64 <: BigEndianOrder\<Int64> & LittleEndianOrder\<Int64> & SwapEndianOrder\<Int64>](binary_package_extends.md#extend-int64--bigendianorderint64--littleendianorderint64--swapendianorderint64) (cross-package extension)

### static func readLittleEndian(Array\<UInt8>)

```cangjie
static func readLittleEndian(buffer: Array<UInt8>): T
```

Function: Reads a value of type T from a byte array in little-endian order.

Parameters:

- buffer: [Array](../../core/core_package_api/core_package_structs.md#struct-arrayt)\<[UInt8](../../core/core_package_api/core_package_intrinsics.md#uint8)> - Buffer containing the data to be read.

Returns:

- T - The value of type T.

### func writeLittleEndian(Array\<UInt8>)

```cangjie
func writeLittleEndian(buffer: Array<UInt8>): Int64
```

Function: Writes a value of type T to a byte array in little-endian order.

Parameters:

- buffer: [Array](../../core/core_package_api/core_package_structs.md#struct-arrayt)\<[UInt8](../../core/core_package_api/core_package_intrinsics.md#uint8)> - Buffer to store the data to be written.

Returns:

- [Int64](../../core/core_package_api/core_package_intrinsics.md#int64) - Number of bytes written.

## interface SwapEndianOrder\<T>

```cangjie
public interface SwapEndianOrder<T>
```

Function: Byte order reversal interface.

Extensions:

- [extend Int16 <: BigEndianOrder\<Int16> & LittleEndianOrder\<Int16> & SwapEndianOrder\<Int16>](binary_package_extends.md#extend-int16--bigendianorderint16--littleendianorderint16--swapendianorderint16) (cross-package extension)
- [extend Int32 <: BigEndianOrder\<Int32> & LittleEndianOrder\<Int32> & SwapEndianOrder\<Int32>](binary_package_extends.md#extend-int32--bigendianorderint32--littleendianorderint32--swapendianorderint32) (cross-package extension)
- [extend Int64 <: BigEndianOrder\<Int64> & LittleEndianOrder\<Int64> & SwapEndianOrder\<Int64>](binary_package_extends.md#extend-int64--bigendianorderint64--littleendianorderint64--swapendianorderint64) (cross-package extension)
- [extend Int8 <: BigEndianOrder\<Int8> & LittleEndianOrder\<Int8> & SwapEndianOrder\<Int8>](binary_package_extends.md#extend-int8--bigendianorderint8--littleendianorderint8--swapendianorderint8) (cross-package extension)
- [extend UInt16 <: BigEndianOrder\<UInt16> & LittleEndianOrder\<UInt16> & SwapEndianOrder\<UInt16>](binary_package_extends.md#extend-uint16--bigendianorderuint16--littleendianorderuint16--swapendianorderuint16) (cross-package extension)
- [extend UInt32 <: BigEndianOrder\<UInt32> & LittleEndianOrder\<UInt32> & SwapEndianOrder\<UInt32>](binary_package_extends.md#extend-uint32--bigendianorderuint32--littleendianorderuint32--swapendianorderuint32) (cross-package extension)
- [extend UInt64 <: SwapEndianOrder\<UInt64>](binary_package_extends.md#extend-uint64--swapendianorderuint64) (cross-package extension)
- [extend UInt8 <: SwapEndianOrder\<UInt8>](binary_package_extends.md#extend-uint8--swapendianorderuint8) (cross-package extension)

### func swapBytes()

```cangjie
func swapBytes(): T
```

Function: Reverses the byte order of a value of type T.

Returns:

- T - The value of type T.

