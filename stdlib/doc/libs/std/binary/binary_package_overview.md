# std.binary

## 功能介绍

当前 binary 包提供了如下功能：

- 仓颉数据类型和二进制字节序列间的互相转换接口，分为大端序和小端序两种转换类型。
- 仓颉数据类型自身大小端序转换的接口。

> **说明：**
>
> - 一般来说，多字节对象被存储为连续的字节序列。存储器或数字通信链路中，字节的排列顺序称为端序（Endianness）。端序又称字节顺序或者尾序。
> - 字节有两种排列方式：将一个多位数的低位存储在内存的低地址端，高位存储在内存的高地址端，称为小端序（Little-endian）；反之称为大端序（Big-endian）。

## API 列表

### 接口

| 接口名                                                       | 功能                     |
| ------------------------------------------------------------ | ------------------------ |
| [BigEndianOrder\<T>](./binary_package_api/binary_package_interfaces.md#interface-bigendianordert) | 大端序字节序列转换接口。 |
| [LittleEndianOrder\<T>](./binary_package_api/binary_package_interfaces.md#interface-littleendianordert) | 小端序字节序列转换接口。 |
| [SwapEndianOrder\<T>](./binary_package_api/binary_package_interfaces.md#interface-swapendianordert) | 反转字节顺序接口。       |

### 扩展

| 扩展名 | 功能 |
| --- | --- |
| [extend Bool <: BigEndianOrder\<Bool> & LittleEndianOrder\<Bool>](binary_package_api/binary_package_extends.md#extend-bool--bigendianorderbool--littleendianorderbool) | 为 [Bool](../core/core_package_api/core_package_intrinsics.md#bool) 扩展 [BigEndianOrder](binary_package_api/binary_package_interfaces.md#interface-bigendianordert)、[LittleEndianOrder](binary_package_api/binary_package_interfaces.md#interface-littleendianordert) 接口。 |
| [extend Float16 <: BigEndianOrder\<Float16> & LittleEndianOrder\<Float16>](binary_package_api/binary_package_extends.md#extend-float16--bigendianorderfloat16--littleendianorderfloat16) | 为 [Float16](../core/core_package_api/core_package_intrinsics.md#float16) 扩展 [BigEndianOrder](binary_package_api/binary_package_interfaces.md#interface-bigendianordert)、[LittleEndianOrder](binary_package_api/binary_package_interfaces.md#interface-littleendianordert) 接口。 |
| [extend Float32 <: BigEndianOrder\<Float32> & LittleEndianOrder\<Float32>](binary_package_api/binary_package_extends.md#extend-float32--bigendianorderfloat32--littleendianorderfloat32) | 为 [Float32](../core/core_package_api/core_package_intrinsics.md#float32) 扩展 [BigEndianOrder](binary_package_api/binary_package_interfaces.md#interface-bigendianordert)、[LittleEndianOrder](binary_package_api/binary_package_interfaces.md#interface-littleendianordert) 接口。 |
| [extend Float64 <: BigEndianOrder\<Float64> & LittleEndianOrder\<Float64>](binary_package_api/binary_package_extends.md#extend-float64--bigendianorderfloat64--littleendianorderfloat64) | 为 [Float64](../core/core_package_api/core_package_intrinsics.md#float64) 扩展 [BigEndianOrder](binary_package_api/binary_package_interfaces.md#interface-bigendianordert)、[LittleEndianOrder](binary_package_api/binary_package_interfaces.md#interface-littleendianordert) 接口。 |
| [extend Int16 <: BigEndianOrder\<Int16> & LittleEndianOrder\<Int16> & SwapEndianOrder\<Int16>](binary_package_api/binary_package_extends.md#extend-int16--bigendianorderint16--littleendianorderint16--swapendianorderint16) | 为 [Int16](../core/core_package_api/core_package_intrinsics.md#int16) 扩展 [BigEndianOrder](binary_package_api/binary_package_interfaces.md#interface-bigendianordert)、[LittleEndianOrder](binary_package_api/binary_package_interfaces.md#interface-littleendianordert)、[SwapEndianOrder](binary_package_api/binary_package_interfaces.md#interface-swapendianordert) 接口。 |
| [extend Int32 <: BigEndianOrder\<Int32> & LittleEndianOrder\<Int32> & SwapEndianOrder\<Int32>](binary_package_api/binary_package_extends.md#extend-int32--bigendianorderint32--littleendianorderint32--swapendianorderint32) | 为 [Int32](../core/core_package_api/core_package_intrinsics.md#int32) 扩展 [BigEndianOrder](binary_package_api/binary_package_interfaces.md#interface-bigendianordert)、[LittleEndianOrder](binary_package_api/binary_package_interfaces.md#interface-littleendianordert)、[SwapEndianOrder](binary_package_api/binary_package_interfaces.md#interface-swapendianordert) 接口。 |
| [extend Int64 <: BigEndianOrder\<Int64> & LittleEndianOrder\<Int64> & SwapEndianOrder\<Int64>](binary_package_api/binary_package_extends.md#extend-int64--bigendianorderint64--littleendianorderint64--swapendianorderint64) | 为 [Int64](../core/core_package_api/core_package_intrinsics.md#int64) 扩展 [BigEndianOrder](binary_package_api/binary_package_interfaces.md#interface-bigendianordert)、[LittleEndianOrder](binary_package_api/binary_package_interfaces.md#interface-littleendianordert)、[SwapEndianOrder](binary_package_api/binary_package_interfaces.md#interface-swapendianordert) 接口。 |
| [extend Int8 <: BigEndianOrder\<Int8> & LittleEndianOrder\<Int8> & SwapEndianOrder\<Int8>](binary_package_api/binary_package_extends.md#extend-int8--bigendianorderint8--littleendianorderint8--swapendianorderint8) | 为 [Int8](../core/core_package_api/core_package_intrinsics.md#int8) 扩展 [BigEndianOrder](binary_package_api/binary_package_interfaces.md#interface-bigendianordert)、[LittleEndianOrder](binary_package_api/binary_package_interfaces.md#interface-littleendianordert)、[SwapEndianOrder](binary_package_api/binary_package_interfaces.md#interface-swapendianordert) 接口。 |
| [extend UInt16 <: BigEndianOrder\<UInt16> & LittleEndianOrder\<UInt16> & SwapEndianOrder\<UInt16>](binary_package_api/binary_package_extends.md#extend-uint16--bigendianorderuint16--littleendianorderuint16--swapendianorderuint16) | 为 [UInt16](../core/core_package_api/core_package_intrinsics.md#uint16) 扩展 [BigEndianOrder](binary_package_api/binary_package_interfaces.md#interface-bigendianordert)、[LittleEndianOrder](binary_package_api/binary_package_interfaces.md#interface-littleendianordert)、[SwapEndianOrder](binary_package_api/binary_package_interfaces.md#interface-swapendianordert) 接口。 |
| [extend UInt32 <: BigEndianOrder\<UInt32> & LittleEndianOrder\<UInt32> & SwapEndianOrder\<UInt32>](binary_package_api/binary_package_extends.md#extend-uint32--bigendianorderuint32--littleendianorderuint32--swapendianorderuint32) | 为 [UInt32](../core/core_package_api/core_package_intrinsics.md#uint32) 扩展 [BigEndianOrder](binary_package_api/binary_package_interfaces.md#interface-bigendianordert)、[LittleEndianOrder](binary_package_api/binary_package_interfaces.md#interface-littleendianordert)、[SwapEndianOrder](binary_package_api/binary_package_interfaces.md#interface-swapendianordert) 接口。 |
| [extend UInt64 <: BigEndianOrder\<UInt64> & LittleEndianOrder\<UInt64> & SwapEndianOrder\<UInt64>](binary_package_api/binary_package_extends.md#extend-uint64--bigendianorderuint64--littleendianorderuint64--swapendianorderuint64) | 为 [UInt64](../core/core_package_api/core_package_intrinsics.md#uint64) 扩展 [BigEndianOrder](binary_package_api/binary_package_interfaces.md#interface-bigendianordert)、[LittleEndianOrder](binary_package_api/binary_package_interfaces.md#interface-littleendianordert)、[SwapEndianOrder](binary_package_api/binary_package_interfaces.md#interface-swapendianordert) 接口。 |
| [extend UInt8 <: BigEndianOrder\<UInt8> & LittleEndianOrder\<UInt8> & SwapEndianOrder\<UInt8>](binary_package_api/binary_package_extends.md#extend-uint8--bigendianorderuint8--littleendianorderuint8--swapendianorderuint8) | 为 [UInt8](../core/core_package_api/core_package_intrinsics.md#uint8) 扩展 [BigEndianOrder](binary_package_api/binary_package_interfaces.md#interface-bigendianordert)、[LittleEndianOrder](binary_package_api/binary_package_interfaces.md#interface-littleendianordert)、[SwapEndianOrder](binary_package_api/binary_package_interfaces.md#interface-swapendianordert) 接口。 |
