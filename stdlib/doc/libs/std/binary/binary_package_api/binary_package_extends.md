# 扩展

## extend UInt64 <: BigEndianOrder\<UInt64> & LittleEndianOrder\<UInt64> & SwapEndianOrder\<UInt64>

```cangjie
extend UInt64 <: BigEndianOrder<UInt64> & LittleEndianOrder<UInt64> & SwapEndianOrder<UInt64>
```

功能：为 [UInt64](../../core/core_package_api/core_package_intrinsics.md#uint64) 扩展 B[BigEndianOrder](binary_package_interfaces.md#interface-bigendianordert)、[LittleEndianOrder](binary_package_interfaces.md#interface-littleendianordert)、[SwapEndianOrder](binary_package_interfaces.md#interface-swapendianordert) 接口。实现将 [UInt64](../../core/core_package_api/core_package_intrinsics.md#uint64) 值和大端序字节序列的转换、将 [UInt64](../../core/core_package_api/core_package_intrinsics.md#uint64) 值和小端序字节序列的转换以及将 [UInt64](../../core/core_package_api/core_package_intrinsics.md#uint64) 值的字节顺序反转。

被扩展类型：

- [UInt64](../../core/core_package_api/core_package_intrinsics.md#uint64)

扩展接口：

- [BigEndianOrder](binary_package_interfaces.md#interface-bigendianordert)\<[UInt64](../../core/core_package_api/core_package_intrinsics.md#uint64)>
- [LittleEndianOrder](binary_package_interfaces.md#interface-littleendianordert)\<[UInt64](../../core/core_package_api/core_package_intrinsics.md#uint64)>
- [SwapEndianOrder](binary_package_interfaces.md#interface-swapendianordert)\<[UInt64](../../core/core_package_api/core_package_intrinsics.md#uint64)>

### static func readBigEndian(Array\<Byte>)

```cangjie
public static func readBigEndian(buffer: Array<Byte>): UInt64
```

功能：从字节数组中以大端序的方式读取一个 [UInt64](../../core/core_package_api/core_package_intrinsics.md#uint64) 值。

参数：

- buffer: [Array](../../core/core_package_api/core_package_structs.md#struct-arrayt)\<[Byte](../../core/core_package_api/core_package_types.md#type-byte)> - 缓冲区，用于存放待读取的数据。

返回值：

- [UInt64](../../core/core_package_api/core_package_intrinsics.md#uint64) - [UInt64](../../core/core_package_api/core_package_intrinsics.md#uint64) 值。

异常：

- [IndexOutOfBoundsException](../../core/core_package_api/core_package_exceptions.md#class-indexoutofboundsexception) - 当 buffer 太小，不足以读出 [UInt64](../../core/core_package_api/core_package_intrinsics.md#uint64) 值时，抛出异常。

示例：

<!-- run -->
```cangjie
import std.binary.*
import std.unittest.*
import std.unittest.testmacro.*

main() {
    let buffer: Array<Byte> = [0x12, 0x34, 0x56, 0x78, 0x90, 0x12, 0x34, 0x56]
    let n = UInt64.readBigEndian(buffer)
    @Assert(n, 0x1234567890123456)
}
```

### static func readLittleEndian(Array\<Byte>)

```cangjie
public static func readLittleEndian(buffer: Array<Byte>): UInt64
```

功能：从字节数组中以小端序的方式读取一个 [UInt64](../../core/core_package_api/core_package_intrinsics.md#uint64) 值。

参数：

- buffer: [Array](../../core/core_package_api/core_package_structs.md#struct-arrayt)\<[Byte](../../core/core_package_api/core_package_types.md#type-byte)> - 缓冲区，用于存放待读取的数据。

返回值：

- [UInt64](../../core/core_package_api/core_package_intrinsics.md#uint64) - [UInt64](../../core/core_package_api/core_package_intrinsics.md#uint64) 值。

异常：

- [IndexOutOfBoundsException](../../core/core_package_api/core_package_exceptions.md#class-indexoutofboundsexception) - 当 buffer 太小，不足以读出 [UInt64](../../core/core_package_api/core_package_intrinsics.md#uint64) 值时，抛出异常。

示例：

<!-- run -->
```cangjie
import std.binary.*
import std.unittest.*
import std.unittest.testmacro.*

main() {
    let buffer: Array<Byte> = [0x56, 0x34, 0x12, 0x90, 0x78, 0x56, 0x34, 0x12]
    let n = UInt64.readLittleEndian(buffer)
    @Assert(n, 0x1234567890123456)
}
```

### func writeBigEndian(Array\<Byte>)

```cangjie
public func writeBigEndian(buffer: Array<Byte>): Int64
```

功能：将 [UInt64](../../core/core_package_api/core_package_intrinsics.md#uint64) 值以大端序的方式写入字节数组中。

参数：

- buffer: [Array](../../core/core_package_api/core_package_structs.md#struct-arrayt)\<[Byte](../../core/core_package_api/core_package_types.md#type-byte)> - 缓冲区，用于存放待写入的数据。

返回值：

- [Int64](../../core/core_package_api/core_package_intrinsics.md#int64) - 写入的数据的字节数。

异常：

- [IndexOutOfBoundsException](../../core/core_package_api/core_package_exceptions.md#class-indexoutofboundsexception) - 当 buffer 太小，不足以存储 [UInt64](../../core/core_package_api/core_package_intrinsics.md#uint64) 值时，抛出异常。

示例：

<!-- run -->
```cangjie
import std.binary.*
import std.unittest.*
import std.unittest.testmacro.*

main() {
    let buffer = Array<Byte>(8, repeat: 0)
    let n = 0x1234567890123456u64.writeBigEndian(buffer)
    @Assert(n, 8)
    @Assert(buffer[..n] == [0x12, 0x34, 0x56, 0x78, 0x90, 0x12, 0x34, 0x56])
}
```

### func writeLittleEndian(Array\<Byte>)

```cangjie
public func writeLittleEndian(buffer: Array<Byte>): Int64
```

功能：将 [UInt64](../../core/core_package_api/core_package_intrinsics.md#uint64) 值以小端序的方式写入字节数组中。

参数：

- buffer: [Array](../../core/core_package_api/core_package_structs.md#struct-arrayt)\<[Byte](../../core/core_package_api/core_package_types.md#type-byte)> - 缓冲区，用于存放待写入的数据。

返回值：

- [Int64](../../core/core_package_api/core_package_intrinsics.md#int64) - 写入的数据的字节数。

异常：

- [IndexOutOfBoundsException](../../core/core_package_api/core_package_exceptions.md#class-indexoutofboundsexception) - 当 buffer 太小，不足以存储 [UInt64](../../core/core_package_api/core_package_intrinsics.md#uint64) 值时，抛出异常。

示例：

<!-- run -->
```cangjie
import std.binary.*
import std.unittest.*
import std.unittest.testmacro.*

main() {
    let buffer = Array<Byte>(8, repeat: 0)
    let n = 0x1234567890123456u64.writeLittleEndian(buffer)
    @Assert(n, 8)
    @Assert(buffer[..n] == [0x56, 0x34, 0x12, 0x90, 0x78, 0x56, 0x34, 0x12])
}
```

### func swapBytes()

```cangjie
public func swapBytes(): UInt64
```

功能：反转 [UInt64](../../core/core_package_api/core_package_intrinsics.md#uint64) 值的字节顺序。

返回值：

- [UInt64](../../core/core_package_api/core_package_intrinsics.md#uint64) - [UInt64](../../core/core_package_api/core_package_intrinsics.md#uint64) 值。

示例：

<!-- run -->
```cangjie
import std.binary.*
import std.unittest.*
import std.unittest.testmacro.*

main() {
    let n = 0x1234567890123456u64
    let m = n.swapBytes()
    @Assert(m, 0x5634129078563412)
}
```
