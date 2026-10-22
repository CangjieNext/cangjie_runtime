# 接口

## interface BigEndianOrder\<T>

```cangjie
public interface BigEndianOrder<T>
```

功能：大端序字节序列转换接口。

扩展：

- [extend Bool <: BigEndianOrder\<Bool> & LittleEndianOrder\<Bool>](binary_package_extends.md#extend-bool--bigendianorderbool--littleendianorderbool)
- [extend Float16 <: BigEndianOrder\<Float16> & LittleEndianOrder\<Float16>](binary_package_extends.md#extend-float16--bigendianorderfloat16--littleendianorderfloat16)
- [extend Float32 <: BigEndianOrder\<Float32> & LittleEndianOrder\<Float32>](binary_package_extends.md#extend-float32--bigendianorderfloat32--littleendianorderfloat32)
- [extend Float64 <: BigEndianOrder\<Float64> & LittleEndianOrder\<Float64>](binary_package_extends.md#extend-float64--bigendianorderfloat64--littleendianorderfloat64)
- [extend Int16 <: BigEndianOrder\<Int16> & LittleEndianOrder\<Int16> & SwapEndianOrder\<Int16>](binary_package_extends.md#extend-int16--bigendianorderint16--littleendianorderint16--swapendianorderint16)
- [extend Int32 <: BigEndianOrder\<Int32> & LittleEndianOrder\<Int32> & SwapEndianOrder\<Int32>](binary_package_extends.md#extend-int32--bigendianorderint32--littleendianorderint32--swapendianorderint32)
- [extend Int64 <: BigEndianOrder\<Int64> & LittleEndianOrder\<Int64> & SwapEndianOrder\<Int64>](binary_package_extends.md#extend-int64--bigendianorderint64--littleendianorderint64--swapendianorderint64)
- [extend Int8 <: BigEndianOrder\<Int8> & LittleEndianOrder\<Int8> & SwapEndianOrder\<Int8>](binary_package_extends.md#extend-int8--bigendianorderint8--littleendianorderint8--swapendianorderint8)
- [extend UInt16 <: BigEndianOrder\<UInt16> & LittleEndianOrder\<UInt16> & SwapEndianOrder\<UInt16>](binary_package_extends.md#extend-uint16--bigendianorderuint16--littleendianorderuint16--swapendianorderuint16)
- [extend UInt32 <: BigEndianOrder\<UInt32> & LittleEndianOrder\<UInt32> & SwapEndianOrder\<UInt32>](binary_package_extends.md#extend-uint32--bigendianorderuint32--littleendianorderuint32--swapendianorderuint32)
- [extend UInt64 <: BigEndianOrder\<UInt64> & LittleEndianOrder\<UInt64> & SwapEndianOrder\<UInt64>](binary_package_extends.md#extend-uint64--bigendianorderuint64--littleendianorderuint64--swapendianorderuint64)
- [extend UInt8 <: BigEndianOrder\<UInt8> & LittleEndianOrder\<UInt8> & SwapEndianOrder\<UInt8>](binary_package_extends.md#extend-uint8--bigendianorderuint8--littleendianorderuint8--swapendianorderuint8)

### static func readBigEndian(Array\<Byte>)

```cangjie
static func readBigEndian(buffer: Array<Byte>): T
```

功能：从字节数组中以大端序的方式读取一个 T 值。

参数：

- buffer: [Array](../../core/core_package_api/core_package_structs.md#struct-arrayt)\<[Byte](../../core/core_package_api/core_package_types.md#type-byte)> - 缓冲区，用于存放待读取的数据。

返回值：

- T - T 值。

### func writeBigEndian(Array\<Byte>)

```cangjie
func writeBigEndian(buffer: Array<Byte>): Int64
```

功能：将 T 值以大端序的方式写入字节数组中。

参数：

- buffer: [Array](../../core/core_package_api/core_package_structs.md#struct-arrayt)\<[Byte](../../core/core_package_api/core_package_types.md#type-byte)> - 缓冲区，用于存放待写入的数据。

返回值：

- [Int64](../../core/core_package_api/core_package_intrinsics.md#int64) - 写入的数据的字节数。

## interface LittleEndianOrder\<T>

```cangjie
public interface LittleEndianOrder<T>
```

功能：小端序字节序列转换接口。

扩展：

- [extend Bool <: BigEndianOrder\<Bool> & LittleEndianOrder\<Bool>](binary_package_extends.md#extend-bool--bigendianorderbool--littleendianorderbool)
- [extend Float16 <: BigEndianOrder\<Float16> & LittleEndianOrder\<Float16>](binary_package_extends.md#extend-float16--bigendianorderfloat16--littleendianorderfloat16)
- [extend Float32 <: BigEndianOrder\<Float32> & LittleEndianOrder\<Float32>](binary_package_extends.md#extend-float32--bigendianorderfloat32--littleendianorderfloat32)
- [extend Float64 <: BigEndianOrder\<Float64> & LittleEndianOrder\<Float64>](binary_package_extends.md#extend-float64--bigendianorderfloat64--littleendianorderfloat64)
- [extend Int16 <: BigEndianOrder\<Int16> & LittleEndianOrder\<Int16> & SwapEndianOrder\<Int16>](binary_package_extends.md#extend-int16--bigendianorderint16--littleendianorderint16--swapendianorderint16)
- [extend Int32 <: BigEndianOrder\<Int32> & LittleEndianOrder\<Int32> & SwapEndianOrder\<Int32>](binary_package_extends.md#extend-int32--bigendianorderint32--littleendianorderint32--swapendianorderint32)
- [extend Int64 <: BigEndianOrder\<Int64> & LittleEndianOrder\<Int64> & SwapEndianOrder\<Int64>](binary_package_extends.md#extend-int64--bigendianorderint64--littleendianorderint64--swapendianorderint64)
- [extend Int8 <: BigEndianOrder\<Int8> & LittleEndianOrder\<Int8> & SwapEndianOrder\<Int8>](binary_package_extends.md#extend-int8--bigendianorderint8--littleendianorderint8--swapendianorderint8)
- [extend UInt16 <: BigEndianOrder\<UInt16> & LittleEndianOrder\<UInt16> & SwapEndianOrder\<UInt16>](binary_package_extends.md#extend-uint16--bigendianorderuint16--littleendianorderuint16--swapendianorderuint16)
- [extend UInt32 <: BigEndianOrder\<UInt32> & LittleEndianOrder\<UInt32> & SwapEndianOrder\<UInt32>](binary_package_extends.md#extend-uint32--bigendianorderuint32--littleendianorderuint32--swapendianorderuint32)
- [extend UInt64 <: BigEndianOrder\<UInt64> & LittleEndianOrder\<UInt64> & SwapEndianOrder\<UInt64>](binary_package_extends.md#extend-uint64--bigendianorderuint64--littleendianorderuint64--swapendianorderuint64)
- [extend UInt8 <: BigEndianOrder\<UInt8> & LittleEndianOrder\<UInt8> & SwapEndianOrder\<UInt8>](binary_package_extends.md#extend-uint8--bigendianorderuint8--littleendianorderuint8--swapendianorderuint8)

### static func readLittleEndian(Array\<Byte>)

```cangjie
static func readLittleEndian(buffer: Array<Byte>): T
```

功能：从字节数组中以小端序的方式读取一个 T 值。

参数：

- buffer: [Array](../../core/core_package_api/core_package_structs.md#struct-arrayt)\<[Byte](../../core/core_package_api/core_package_types.md#type-byte)> - 缓冲区，用于存放待读取的数据。

返回值：

- T - T 值。

### func writeLittleEndian(Array\<Byte>)

```cangjie
func writeLittleEndian(buffer: Array<Byte>): Int64
```

功能：将 T 值以小端序的方式写入字节数组中。

参数：

- buffer: [Array](../../core/core_package_api/core_package_structs.md#struct-arrayt)\<[Byte](../../core/core_package_api/core_package_types.md#type-byte)> - 缓冲区，用于存放待写入的数据。

返回值：

- [Int64](../../core/core_package_api/core_package_intrinsics.md#int64) - 写入的数据的字节数。

## interface SwapEndianOrder\<T>

```cangjie
public interface SwapEndianOrder<T>
```

功能：反转字节顺序接口。

扩展：

- [extend Int16 <: BigEndianOrder\<Int16> & LittleEndianOrder\<Int16> & SwapEndianOrder\<Int16>](binary_package_extends.md#extend-int16--bigendianorderint16--littleendianorderint16--swapendianorderint16)
- [extend Int32 <: BigEndianOrder\<Int32> & LittleEndianOrder\<Int32> & SwapEndianOrder\<Int32>](binary_package_extends.md#extend-int32--bigendianorderint32--littleendianorderint32--swapendianorderint32)
- [extend Int64 <: BigEndianOrder\<Int64> & LittleEndianOrder\<Int64> & SwapEndianOrder\<Int64>](binary_package_extends.md#extend-int64--bigendianorderint64--littleendianorderint64--swapendianorderint64)
- [extend Int8 <: BigEndianOrder\<Int8> & LittleEndianOrder\<Int8> & SwapEndianOrder\<Int8>](binary_package_extends.md#extend-int8--bigendianorderint8--littleendianorderint8--swapendianorderint8)
- [extend UInt16 <: BigEndianOrder\<UInt16> & LittleEndianOrder\<UInt16> & SwapEndianOrder\<UInt16>](binary_package_extends.md#extend-uint16--bigendianorderuint16--littleendianorderuint16--swapendianorderuint16)
- [extend UInt32 <: BigEndianOrder\<UInt32> & LittleEndianOrder\<UInt32> & SwapEndianOrder\<UInt32>](binary_package_extends.md#extend-uint32--bigendianorderuint32--littleendianorderuint32--swapendianorderuint32)
- [extend UInt64 <: BigEndianOrder\<UInt64> & LittleEndianOrder\<UInt64> & SwapEndianOrder\<UInt64>](binary_package_extends.md#extend-uint64--bigendianorderuint64--littleendianorderuint64--swapendianorderuint64)
- [extend UInt8 <: BigEndianOrder\<UInt8> & LittleEndianOrder\<UInt8> & SwapEndianOrder\<UInt8>](binary_package_extends.md#extend-uint8--bigendianorderuint8--littleendianorderuint8--swapendianorderuint8)

### func swapBytes()

```cangjie
func swapBytes(): T
```

功能：反转 T 值的字节顺序。

返回值：

- T - T 值。

