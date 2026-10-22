# 接口

## interface InputStream

```cangjie
public interface InputStream {
    func read(buffer: Array<Byte>): Int64
    func readFully(buffer: Array<Byte>): Unit
}
```

功能：输入流接口。

### func read(Array\<Byte>)

```cangjie
func read(buffer: Array<Byte>): Int64
```

功能：从输入流中读取数据放到 `buffer` 中。

单次调用不保证填满 `buffer`。实现可以尽量填充缓冲区，也可以只读取当前可用的数据；没有数据可读时，调用可能阻塞等待输入。允许短读不等于非阻塞读取。

返回值 `n` 表示本次实际读取的字节数，满足 `0 <= n <= buffer.size`。调用方应仅处理 `buffer` 的前 `n` 个元素。对于非空缓冲区，正常读取到流末尾且本次未读到数据时返回 `0`，不能用 `0 < n < buffer.size` 判断流已经结束。空缓冲区和读取失败的处理方式见具体实现说明。

需要填满缓冲区时，可以使用 [readFully(Array\<Byte>)](#func-readfullyarraybyte)。

参数：

- buffer: [Array](../../core/core_package_api/core_package_structs.md#struct-arrayt)\<[Byte](../../core/core_package_api/core_package_types.md#type-byte)> - 缓冲区，用于存放从输入流中读取的数据。

返回值：

- [Int64](../../core/core_package_api/core_package_intrinsics.md#int64) - 读取的数据的字节数。

### func readFully(Array\<Byte>)

```cangjie
func readFully(buffer: Array<Byte>): Unit
```

该方法由接口提供默认实现，现有输入流只实现 `read` 即可继承使用。覆盖该方法时必须保持本节所述行为契约。

功能：使用本输入流的 `read` 默认循环读取，直到填满 `buffer`。正常返回表示恰好读取了 `buffer.size` 个字节。每次只向尚未填充的部分读入数据，填满后不再读取，也不等待流结束。空数组直接成功，不调用底层读取。

该函数可能阻塞等待后续输入，不提供超时、取消或消息边界识别能力，也不关闭输入流。多次读取不构成原子操作；同一输入流有多个消费者时，调用方需要自行协调。

参数：

- buffer: [Array](../../core/core_package_api/core_package_structs.md#struct-arrayt)\<[Byte](../../core/core_package_api/core_package_types.md#type-byte)> - 需要填满的目标数组，也可以传入数组切片。

异常：

- [IOException](io_package_exceptions.md#class-ioexception) - 数组未填满时，底层读取返回 `0`；或者底层返回小于 `0`、大于本次请求长度的字节数。未填满异常信息包含目标长度和已读长度。
- 底层 `read` 抛出的异常原样向上传递。失败时目标数组可能已部分修改，已消费的数据不回退；调用方不能把整个数组当作完整结果。

> **说明：**
>
> 本函数依赖具体输入流对结束与错误的报告方式。若具体实现将读取失败也映射为 `0`，本函数会报告未填满，但无法恢复其已丢失的错误原因。它不改变具体流原有的错误处理方式。

示例：

<!-- verify -->
```cangjie
import std.io.*

main(): Unit {
    let input = ByteBuffer("abcdefgh".toArray())
    let buffer = Array<Byte>(8, repeat: 0)
    input.readFully(buffer)
    println(buffer)

    try {
        ByteBuffer("abc".toArray()).readFully(Array<Byte>(8, repeat: 0))
    } catch (e: IOException) {
        println("输入不足 8 字节")
    }
}
```

运行结果：

```text
[97, 98, 99, 100, 101, 102, 103, 104]
输入不足 8 字节
```

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

功能：清空缓冲区。该函数的默认实现为空。

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
    func seek(sp: SeekPosition): Int64
    prop position: Int64
    prop remainLength: Int64
    prop length: Int64
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

参数：

- sp: [SeekPosition](io_package_enums.md#enum-seekposition) - 指定光标移动后的位置。

返回值：

- [Int64](../../core/core_package_api/core_package_intrinsics.md#int64) - 返回流中数据的起点到移动后位置的偏移量（以字节为单位）。
