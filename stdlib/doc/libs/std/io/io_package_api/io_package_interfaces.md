# 接口

## interface InputStream

```cangjie
public interface InputStream {
    func read(buffer: Array<Byte>): Int64
}
```

功能：输入流接口。

### func read(Array\<Byte>)

```cangjie
func read(buffer: Array<Byte>): Int64
```

功能：从输入流中读取数据放到 `buffer` 中。

参数：

- buffer: [Array](../../core/core_package_api/core_package_structs.md#struct-arrayt)\<[Byte](../../core/core_package_api/core_package_types.md#type-byte)> - 缓冲区，用于存放从输入流中读取的数据。

返回值：

- [Int64](../../core/core_package_api/core_package_intrinsics.md#int64) - 读取的数据的字节数。

## interface IOStream

```cangjie
public interface IOStream <: InputStream & OutputStream {}
```

功能：输入输出流接口。

父类型：

- [InputStream](#interface-inputstream)
- [OutputStream](#interface-outputstream)

## interface OutputStream

```cangjie
public interface OutputStream {
    func write(buffer: Array<Byte>): Unit
    func flush(): Unit
}
```

功能：输出流接口。

### func flush()

```cangjie
func flush(): Unit
```

功能：清空缓存区。该函数的默认实现为空。

### func write(Array\<Byte>)

```cangjie
func write(buffer: Array<Byte>): Unit
```

功能：将 `buffer` 中的数据写入到输出流中。

参数：

- buffer: [Array](../../core/core_package_api/core_package_structs.md#struct-arrayt)\<[Byte](../../core/core_package_api/core_package_types.md#type-byte)> - 缓冲区，用于存放待写入输出流的数据。

## interface Seekable

```cangjie
public interface Seekable {
    prop length: Int64
    prop position: Int64
    prop remainLength: Int64
    func seek(sp: SeekPosition): Int64
}
```

功能：移动光标接口。

### prop length

```cangjie
prop length: Int64
```

功能：返回当前流中的总数据量（以字节为单位）。

类型：[Int64](../../core/core_package_api/core_package_intrinsics.md#int64)

### prop position

```cangjie
prop position: Int64
```

功能：返回当前光标位置。

类型：[Int64](../../core/core_package_api/core_package_intrinsics.md#int64)

### prop remainLength

```cangjie
prop remainLength: Int64
```

功能：返回当前流中未读的数据量（以字节为单位）。

类型：[Int64](../../core/core_package_api/core_package_intrinsics.md#int64)

### func seek(SeekPosition)

```cangjie
func seek(sp: SeekPosition): Int64
```

功能：移动光标到指定的位置。

> **说明：**
>
> - 带有内部缓冲区的实现（例如缓冲流），在移动逻辑位置之前，必须先同步自身缓冲区（例如：输出流将未写入数据 flush 到底层流；输入流丢弃或使读缓冲区与目标位置一致）。这样，包装类（如 [BufferedInputStream](io_package_classes.md#class-bufferedinputstreamt-where-t--inputstream)）在调用内层流的 [seek](./io_package_interfaces.md#func-seekseekposition) 时，可依赖内层已同步其缓冲，无需再显式调用内层的 [flush](./io_package_interfaces.md#func-flush) 等方法。

参数：

- sp: [SeekPosition](io_package_enums.md#enum-seekposition) - 指定光标移动后的位置。

返回值：

- [Int64](../../core/core_package_api/core_package_intrinsics.md#int64) - 返回流中数据的起点到移动后位置的偏移量（以字节为单位）。
