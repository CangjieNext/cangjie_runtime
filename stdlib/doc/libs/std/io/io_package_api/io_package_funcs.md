# 函数

## func copy(InputStream, OutputStream)

```cangjie
public func copy(from: InputStream, to!: OutputStream): Int64
```

功能：将一个输入流中未被读取的数据拷贝到另一个输出流中。

参数：

- from: [InputStream](io_package_interfaces.md#interface-inputstream) - 待读取数据的输入流。
- to!: [OutputStream](io_package_interfaces.md#interface-outputstream) - 数据将要拷贝到的输出流。

返回值：

- [Int64](../../core/core_package_api/core_package_intrinsics.md#int64) - 拷贝数据的字节数。

示例：

<!-- verify -->
```cangjie
import std.io.ByteBuffer
import std.io.copy

main(): Unit {
    let sourceStream = ByteBuffer()
    let targetStream = ByteBuffer()

    /* 向源输入流写入数据 */
    let sourceData = "Hello, World!".toArray()
    sourceStream.write(sourceData)

    /* 使用 copy 函数将源输入流的数据拷贝到目标输出流 */
    let copiedBytes = copy(sourceStream, to: targetStream)
    println("Copied ${copiedBytes} bytes.")

    /* 读取目标输出流中的数据 */
    let targetData: Array<Byte> = Array<Byte>(sourceData.size, repeat: 0)
    targetStream.read(targetData)
    println("Data copied to target stream: ${String.fromUtf8(targetData)}")
}
```

运行结果：

```text
Copied 13 bytes.
Data copied to target stream: Hello, World!
```

## func readFully(InputStream, Array\<Byte>)

```cangjie
public func readFully(from: InputStream, buffer: Array<Byte>): Unit
```

功能：循环调用 `from.read`，直到填满 `buffer`。正常返回表示恰好读取了 `buffer.size` 个字节。每次只向尚未填充的部分读入数据，填满后不再读取，也不等待流结束。空数组直接成功，不调用底层读取。

该函数继承 `from.read` 的等待方式，可能阻塞等待后续输入，不提供超时、取消或消息边界识别能力，也不关闭输入流。多次读取不构成原子操作；同一输入流有多个消费者时，调用方需要自行协调。

参数：

- from: [InputStream](io_package_interfaces.md#interface-inputstream) - 待读取数据的输入流。
- buffer: [Array](../../core/core_package_api/core_package_structs.md#struct-arrayt)\<[Byte](../../core/core_package_api/core_package_types.md#type-byte)> - 需要填满的目标数组，也可以传入数组切片。

异常：

- [IOException](io_package_exceptions.md#class-ioexception) - 数组未填满时，底层读取返回 `0`；或者底层返回小于 `0`、大于本次请求长度的字节数。未填满异常信息包含目标长度和已读长度。
- 底层 `read` 抛出的异常原样向上传递。失败时目标数组可能已部分修改，已消费的数据不回退；调用方不能把整个数组当作完整结果。

> **说明：**
>
> 本函数依赖具体输入流对结束与错误的报告方式。若具体实现将读取失败也映射为 `0`，本函数会报告未填满，但无法恢复其已丢失的错误原因。它不改变具体流原有的错误处理方式。

将 `ConsoleReader` 传入本函数时，调用的仍是其原有按行取数的 `read`，可能等待换行；Windows 重定向输入沿用默认文本转换规则。标准输入需要通过短读路径读满时，应调用 [ConsoleReader.readFully(Array\<Byte>)](../../env/env_package_api/env_package_classes.md#func-readfullyarraybyte)；与行读取混用时，仍受 `ConsoleReader` 缓存中文本转换结果的影响。

示例：

<!-- verify -->
```cangjie
import std.io.*

main(): Unit {
    let input = ByteBuffer("abcdefgh".toArray())
    let buffer = Array<Byte>(8, repeat: 0)
    readFully(input, buffer)
    println(buffer)

    try {
        readFully(ByteBuffer("abc".toArray()), Array<Byte>(8, repeat: 0))
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

## func readString\<T>(T) where T <: InputStream & Seekable

```cangjie
public func readString<T>(from: T): String where T <: InputStream & Seekable
```

功能：读取入参中的所有剩余内容，并返回一个字符串。

参数：

- from: T - 要读取数据的对象。

返回值：

- [String](../../core/core_package_api/core_package_structs.md#struct-string) - 读取到的结果字符串。

异常：

- [ContentFormatException](io_package_exceptions.md#class-contentformatexception) - 当剩余字节不符合 UTF-8 编码规则时，抛出异常。

示例：

<!-- verify -->
```cangjie
import std.io.ByteBuffer
import std.io.readString
import std.io.ContentFormatException

main(): Unit {
    let inputStream = ByteBuffer()

    /* 向输入流写入数据 */
    let sourceData = "Hello, World!".toArray()
    inputStream.write(sourceData)

    /* 使用 readString 函数读取输入流中的所有剩余内容 */
    try {
        let result = readString(inputStream)
        println("Read string: ${result}")
    } catch (e: ContentFormatException) {
        println("Error: ${e.message}")
    }

    /* 向输入流写入一个不合法的 UTF-8 字符串 */
    let sourceDataError: Array<UInt8> = [0xC3, 0x28, 0x48, 0x65, 0x6C, 0x6C, 0x6F]
    inputStream.write(sourceDataError)

    /* 使用 readString 函数读取输入流中的所有剩余内容 */
    try {
        let result = readString(inputStream)
        println("Read string: ${result}")
    } catch (e: ContentFormatException) {
        println("Error: ${e.message}")
    }
}
```

运行结果：

```text
Read string: Hello, World!
Error: Invalid UTF-8: byte sequence starting at index '0' is not a valid 2-byte UTF-8 encoding.
```

## func readStringUnchecked\<T>(T) where T <: InputStream & Seekable

```cangjie
public unsafe func readStringUnchecked<T>(from: T): String where T <: InputStream & Seekable
```

功能：读取入参中的所有剩余内容，并返回一个字符串。该函数不会检查字符串的合法性。

参数：

- from: T - 要读取数据的对象。

返回值：

- [String](../../core/core_package_api/core_package_structs.md#struct-string) - 读取到的结果字符串。

示例：

<!-- verify -->
```cangjie
import std.io.ByteBuffer
import std.io.readStringUnchecked
import std.io.ContentFormatException

main(): Unit {
    let inputStream = ByteBuffer()

    /* 向输入流写入数据 */
    let sourceData = "Hello, World!".toArray()
    inputStream.write(sourceData)

    /* 使用 readString 函数读取输入流中的所有剩余内容，可以正常输出：Read string: Hello, World! */
    unsafe {
        let result = readStringUnchecked(inputStream)
        println("Read string: ${result}")
    }
}
```

运行结果：

```text
Read string: Hello, World!
```

## func readToEnd\<T>(T) where T <: InputStream & Seekable

```cangjie
public func readToEnd<T>(from: T): Array<Byte> where T <: InputStream & Seekable
```

功能：获取入参中未被读取的数据。

参数：

- from: T - 要读取数据的对象。

返回值：

- [Array](../../core/core_package_api/core_package_structs.md#struct-arrayt)\<[Byte](../../core/core_package_api/core_package_types.md#type-byte)> - 未被读取的数据的拷贝。

示例：

<!-- verify -->
```cangjie
import std.io.ByteBuffer
import std.io.readToEnd

main(): Unit {
    let inputStream = ByteBuffer()

    /* 向输入流写入数据 */
    let sourceData = "Hello, World!".toArray()
    inputStream.write(sourceData)

    /* 使用 readToEnd 函数读取输入流中的所有剩余内容 */
    let data = readToEnd(inputStream)
    println("${String.fromUtf8(data)}")
}
```

运行结果：

```text
Hello, World!
```
