# 扩展

## extend\<T> BufferedInputStream\<T> <: Resource where T <: Resource

```cangjie
extend<T> BufferedInputStream<T> <: Resource where T <: Resource
```

功能：为 [BufferedInputStream](./io_package_classes.md#class-bufferedinputstreamt-where-t--inputstream) 实现 [Resource](../../core/core_package_api/core_package_interfaces.md#interface-resource) 接口，该类型对象可在 `try-with-resource` 语法上下文中实现自动资源释放。

被扩展类型：

- [BufferedInputStream\<T>](./io_package_classes.md#class-bufferedinputstreamt-where-t--inputstream)

扩展接口：

- [Resource](../../core/core_package_api/core_package_interfaces.md#interface-resource)

### func close()

```cangjie
public func close(): Unit
```

功能：关闭当前流。

> **注意：**
>
> 调用此方法后不可再调用 [BufferedInputStream](io_package_classes.md#class-bufferedinputstreamt-where-t--inputstream) 的其他接口，否则会造成非预期现象。

示例：

<!-- verify -->
```cangjie
import std.io.BufferedInputStream
import std.io.InputStream
import std.io.ByteBuffer

/**
 * 自定义实现 InputStream 和 Resource 接口的类
 */
public class TestStream <: InputStream & Resource {
    private var closed: Bool = false

    public func read(buffer: Array<Byte>): Int64 {
        if (this.closed) {
            return 0
        }
        let data = "Hello World".toArray()
        let inputStream = ByteBuffer(data)
        return inputStream.read(buffer)
    }

    public func isClosed(): Bool {
        return closed
    }

    public func close(): Unit {
        println("Stream is closed")
        closed = true
    }
}

main(): Unit {
    let testStream = TestStream()
    let bufferedStream = BufferedInputStream(testStream)

    // 检查流是否关闭
    println("Is closed before close(): ${bufferedStream.isClosed()}")

    // 关闭流
    bufferedStream.close()

    // 检查流是否关闭
    println("Is closed after close(): ${bufferedStream.isClosed()}")
}
```

运行结果：

```text
Is closed before close(): false
Stream is closed
Is closed after close(): true
```

### func isClosed()

```cangjie
public func isClosed(): Bool
```

功能：判断当前流是否关闭。

返回值：

- [Bool](../../core/core_package_api/core_package_intrinsics.md#bool) - 如果当前流已经被关闭，返回 true，否则返回 false。

示例：

<!-- verify -->
```cangjie
import std.io.BufferedInputStream
import std.io.InputStream
import std.io.ByteBuffer

/**
 * 自定义实现 InputStream 和 Resource 接口的类 A
 */
public class A <: InputStream & Resource {
    private var closed: Bool = false

    public func read(buffer: Array<Byte>): Int64 {
        let inputData = "Hello World".toArray()
        let inputStream = ByteBuffer(inputData)
        let num = inputStream.read(buffer)
        return num
    }

    public func isClosed(): Bool {
        return closed
    }

    public func close(): Unit {
        println("Resource is closed")
        closed = true
    }
}

main(): Unit {
    let bufferedStream = BufferedInputStream(A())

    /* 使用 try-with-resource 语法获取资源 */
    try (r = bufferedStream) {
        println("Get the resource")
        let data = Array<Byte>(11, repeat: 0)
        r.read(data)
        println(r.isClosed())
        println(String.fromUtf8(data))
    }

    /* 自动调用 close() 函数释放资源 */
    println(bufferedStream.isClosed())
}
```

运行结果：

```text
Get the resource
false
Hello World
Resource is closed
true
```

## extend\<T> BufferedInputStream\<T> <: Seekable where T <: Seekable

```cangjie
extend<T> BufferedInputStream<T> <: Seekable where T <: Seekable
```

功能：为 [BufferedInputStream](./io_package_classes.md#class-bufferedinputstreamt-where-t--inputstream) 实现 [Seekable](./io_package_interfaces.md#interface-seekable) 接口，支持查询数据长度，移动光标等操作。

被扩展类型：

- [BufferedInputStream\<T>](./io_package_classes.md#class-bufferedinputstreamt-where-t--inputstream)

扩展接口：

- [Seekable](io_package_interfaces.md#interface-seekable)

### prop length

```cangjie
public prop length: Int64
```

功能：返回当前流中的总数据量（以字节为单位）。

类型：[Int64](../../core/core_package_api/core_package_intrinsics.md#int64)

示例：

<!-- verify -->
```cangjie
import std.io.BufferedInputStream
import std.io.InputStream
import std.io.ByteBuffer
import std.io.Seekable
import std.io.SeekPosition

/**
 * 自定义实现 InputStream 和 Seekable 接口的类
 */
public class TestStream <: InputStream & Seekable {
    private var inputStream: ByteBuffer = ByteBuffer("Hello World".toArray())

    public func read(buffer: Array<Byte>): Int64 {
        return this.inputStream.read(buffer)
    }

    public func seek(sp: SeekPosition): Int64 {
        return this.inputStream.seek(sp)
    }
}

main(): Unit {
    let testStream = TestStream()
    let bufferedStream = BufferedInputStream(testStream)

    // 输出流的总长度
    println("Length: ${bufferedStream.length}")
}
```

运行结果：

```text
Length: 11
```

### prop position

```cangjie
public prop position: Int64
```

功能：返回当前光标位置。

类型：[Int64](../../core/core_package_api/core_package_intrinsics.md#int64)

示例：

<!-- verify -->
```cangjie
import std.io.BufferedInputStream
import std.io.InputStream
import std.io.ByteBuffer
import std.io.Seekable
import std.io.SeekPosition

/**
 * 自定义实现 InputStream 和 Seekable 接口的类
 */
public class TestStream <: InputStream & Seekable {
    private var inputStream: ByteBuffer = ByteBuffer("Hello World".toArray())

    public func read(buffer: Array<Byte>): Int64 {
        return this.inputStream.read(buffer)
    }

    public func seek(sp: SeekPosition): Int64 {
        return this.inputStream.seek(sp)
    }
}

main(): Unit {
    let testStream = TestStream()
    let bufferedStream = BufferedInputStream(testStream)

    // 读取一些数据
    let buffer = Array<Byte>(5, repeat: 0)
    bufferedStream.read(buffer)

    // 输出当前光标位置
    println("Position: ${bufferedStream.position}")
}
```

运行结果：

```text
Position: 11
```

### prop remainLength

```cangjie
public prop remainLength: Int64
```

功能：返回当前流中未读的数据量（以字节为单位，不包含缓冲区中的数据）。

类型：[Int64](../../core/core_package_api/core_package_intrinsics.md#int64)

示例：

<!-- verify -->
```cangjie
import std.io.BufferedInputStream
import std.io.InputStream
import std.io.ByteBuffer
import std.io.Seekable
import std.io.SeekPosition

/**
 * 自定义实现 InputStream 和 Seekable 接口的类
 */
public class TestStream <: InputStream & Seekable {
    private var inputStream: ByteBuffer = ByteBuffer("Hello World".toArray())

    public func read(buffer: Array<Byte>): Int64 {
        return this.inputStream.read(buffer)
    }

    public func seek(sp: SeekPosition): Int64 {
        return this.inputStream.seek(sp)
    }
}

main(): Unit {
    let testStream = TestStream()
    let bufferedStream = BufferedInputStream(testStream)

    // 输出初始未读数据量
    println("Initial Remain Length: ${bufferedStream.remainLength}")

    // 读取一些数据
    let buffer = Array<Byte>(5, repeat: 0)
    bufferedStream.read(buffer)

    // 输出读取后的未读数据量
    println("Remain Length after reading 5 bytes: ${bufferedStream.remainLength}")
}
```

运行结果：

```text
Initial Remain Length: 11
Remain Length after reading 5 bytes: 0
```

### func seek(SeekPosition)

```cangjie
public func seek(sp: SeekPosition): Int64
```

功能：移动光标到指定的位置。

> **说明：**
>
> - 指定的位置不能位于流中数据头部之前。
> - 指定位置可以超过流中数据末尾。
> - 调用该函数会先清空缓冲区，再移动光标的位置。

参数：

- sp: [SeekPosition](io_package_enums.md#enum-seekposition) - 指定光标移动后的位置。

返回值：

- [Int64](../../core/core_package_api/core_package_intrinsics.md#int64) - 返回流中数据的起点到移动后位置的偏移量（以字节为单位）。

异常：

- [IOException](io_package_exceptions.md#class-ioexception) - 当指定的位置位于流中数据头部之前时，抛出异常。

示例：

<!-- verify -->
```cangjie
import std.io.BufferedInputStream
import std.io.InputStream
import std.io.ByteBuffer
import std.io.Seekable
import std.io.SeekPosition
import std.io.IOException
import std.io.readToEnd

/**
 * 自定义实现 InputStream 和 Seekable 接口的类 A
 */
public class A <: InputStream & Seekable {
    public var inputStream: ByteBuffer = ByteBuffer()

    public func read(buffer: Array<Byte>): Int64 {
        let inputData = "Hello World".toArray()
        inputStream = ByteBuffer(inputData)
        let num = inputStream.read(buffer)
        return num
    }

    public func seek(sp: SeekPosition): Int64 {
        return inputStream.seek(sp)
    }
}

main(): Unit {
    let seekableStream = A()
    let bufferedStream = BufferedInputStream(seekableStream)
    let buffer = Array<Byte>(11, repeat: 0)
    bufferedStream.read(buffer)

    /* 输出当前流中总数据量，当前光标位置，当前流中未读的数据量 */
    println("Length : ${bufferedStream.length}")
    println("Position : ${bufferedStream.position}")
    println("Remain Length : ${bufferedStream.remainLength}")

    /* 移动光标到指定位置，虽然超过了流中数据末尾但是合法的 */
    println("Position after seek() : ${bufferedStream.seek(SeekPosition.Current(11))}")

    /* 尝试移动到数据头部之前，抛出异常 */
    try {
        bufferedStream.seek(SeekPosition.Begin(-1))
    } catch (e: IOException) {
        println("Error: " + e.message)
    }

    /* 将光标移动到第一个单词之后，读取后续的数据 */
    bufferedStream.seek(SeekPosition.Begin(6))
    println(String.fromUtf8(readToEnd(seekableStream.inputStream)))
}
```

运行结果：

```text
Length : 11
Position : 11
Remain Length : 0
Position after seek() : 22
Error: Failed to seek: position '-1' is before the beginning of the stream.
World
```

## extend\<T> BufferedOutputStream\<T> <: Resource where T <: Resource

```cangjie
extend<T> BufferedOutputStream<T> <: Resource where T <: Resource
```

功能：为 [BufferedOutputStream](./io_package_classes.md#class-bufferedoutputstreamt-where-t--outputstream) 实现 [Resource](../../core/core_package_api/core_package_interfaces.md#interface-resource) 接口，该类型对象可在 `try-with-resource` 语法上下文中实现自动资源释放。

被扩展类型：

- [BufferedOutputStream\<T>](./io_package_classes.md#class-bufferedoutputstreamt-where-t--outputstream)

扩展接口：

- [Resource](../../core/core_package_api/core_package_interfaces.md#interface-resource)

### func close()

```cangjie
public func close(): Unit
```

功能：关闭当前流。

> **注意：**
>
> 调用此方法后不可再调用 [BufferedOutputStream](io_package_classes.md#class-bufferedoutputstreamt-where-t--outputstream) 的其他接口，否则会造成非预期现象。

示例：

<!-- verify -->
```cangjie
import std.io.BufferedOutputStream
import std.io.OutputStream
import std.io.ByteBuffer

/**
 * 自定义实现 OutputStream 和 Resource 接口的类
 */
public class TestStream <: OutputStream & Resource {
    private var closed: Bool = false
    private var outputStream: ByteBuffer = ByteBuffer()

    public func write(buffer: Array<Byte>): Unit {
        if (this.closed) {
            return
        }
        this.outputStream = ByteBuffer(buffer)
    }

    public func isClosed(): Bool {
        return closed
    }

    public func close(): Unit {
        println("Stream is closed")
        closed = true
    }
}

main(): Unit {
    let testStream = TestStream()
    let bufferedStream = BufferedOutputStream(testStream)

    // 检查流是否关闭
    println("Is closed before close(): ${bufferedStream.isClosed()}")

    // 关闭流
    bufferedStream.close()

    // 检查流是否关闭
    println("Is closed after close(): ${bufferedStream.isClosed()}")
}
```

运行结果：

```text
Is closed before close(): false
Stream is closed
Is closed after close(): true
```

### func isClosed()

```cangjie
public func isClosed(): Bool
```

功能：判断当前流是否关闭。

返回值：

- [Bool](../../core/core_package_api/core_package_intrinsics.md#bool) - 如果当前流已经被关闭，返回 true，否则返回 false。

示例：

<!-- verify -->
```cangjie
import std.io.BufferedOutputStream
import std.io.OutputStream
import std.io.ByteBuffer
import std.io.readToEnd

/**
 * 自定义实现 OutputStream 和 Resource 接口的类 A
 */
public class A <: OutputStream & Resource {
    private var closed: Bool = false
    public var outputStream = ByteBuffer()

    public func write(buffer: Array<Byte>): Unit {
        this.outputStream = ByteBuffer(buffer)
    }

    public func isClosed(): Bool {
        return closed
    }

    public func close(): Unit {
        println("Resource is closed")
        closed = true
    }
}

main(): Unit {
    let resourceStream = A()
    let bufferedStream = BufferedOutputStream(resourceStream)

    /* 使用 try-with-resource 语法获取资源 */
    try (r = bufferedStream) {
        println("Get the resource")
        let data = "Hello World".toArray()
        r.write(data)
        r.flush()
        println(r.isClosed())
        println(String.fromUtf8(readToEnd(resourceStream.outputStream)))
    }

    /* 自动调用 close() 函数释放资源 */
    println(bufferedStream.isClosed())
}
```

运行结果：

```text
Get the resource
false
Hello World
Resource is closed
true
```

## extend\<T> BufferedOutputStream\<T> <: Seekable where T <: Seekable

```cangjie
extend<T> BufferedOutputStream<T> <: Seekable where T <: Seekable
```

功能：为 [BufferedOutputStream](./io_package_classes.md#class-bufferedoutputstreamt-where-t--outputstream) 实现 [Seekable](./io_package_interfaces.md#interface-seekable) 接口，支持查询数据长度，移动光标等操作。

被扩展类型：

- [BufferedOutputStream\<T>](./io_package_classes.md#class-bufferedoutputstreamt-where-t--outputstream)

扩展接口：

- [Seekable](io_package_interfaces.md#interface-seekable)

### prop length

```cangjie
public prop length: Int64
```

功能：返回当前流中的总数据量（以字节为单位）。

类型：[Int64](../../core/core_package_api/core_package_intrinsics.md#int64)

示例：

<!-- verify -->
```cangjie
import std.io.BufferedOutputStream
import std.io.OutputStream
import std.io.ByteBuffer
import std.io.Seekable
import std.io.SeekPosition

/**
 * 自定义实现 OutputStream 和 Seekable 接口的类
 */
public class TestStream <: OutputStream & Seekable {
    private var outputStream: ByteBuffer = ByteBuffer()

    public func write(buffer: Array<Byte>): Unit {
        this.outputStream = ByteBuffer(buffer)
    }

    public func seek(sp: SeekPosition): Int64 {
        return this.outputStream.seek(sp)
    }
}

main(): Unit {
    let testStream = TestStream()
    let bufferedStream = BufferedOutputStream(testStream)

    // 写入一些数据
    let data = "Hello World".toArray()
    bufferedStream.write(data)
    bufferedStream.flush()

    // 输出流的总长度
    println("Length: ${bufferedStream.length}")
}
```

运行结果：

```text
Length: 11
```

### prop position

```cangjie
public prop position: Int64
```

功能：返回当前光标位置。

类型：[Int64](../../core/core_package_api/core_package_intrinsics.md#int64)

示例：

<!-- verify -->
```cangjie
import std.io.BufferedOutputStream
import std.io.OutputStream
import std.io.ByteBuffer
import std.io.Seekable
import std.io.SeekPosition

/**
 * 自定义实现 OutputStream 和 Seekable 接口的类
 */
public class TestStream <: OutputStream & Seekable {
    private var outputStream: ByteBuffer = ByteBuffer()

    public func write(buffer: Array<Byte>): Unit {
        this.outputStream = ByteBuffer(buffer)
    }

    public func seek(sp: SeekPosition): Int64 {
        return this.outputStream.seek(sp)
    }
}

main(): Unit {
    let testStream = TestStream()
    let bufferedStream = BufferedOutputStream(testStream)

    // 写入一些数据
    let data = "Hello World".toArray()
    bufferedStream.write(data)
    bufferedStream.flush()

    // 输出当前光标位置
    println("Position: ${bufferedStream.position}")
}
```

运行结果：

```text
Position: 0
```

### prop remainLength

```cangjie
public prop remainLength: Int64
```

功能：返回当前流中未读的数据量（以字节为单位）。

类型：[Int64](../../core/core_package_api/core_package_intrinsics.md#int64)

示例：

<!-- verify -->
```cangjie
import std.io.BufferedOutputStream
import std.io.OutputStream
import std.io.ByteBuffer
import std.io.Seekable
import std.io.SeekPosition

/**
 * 自定义实现 OutputStream 和 Seekable 接口的类
 */
public class TestStream <: OutputStream & Seekable {
    private var outputStream: ByteBuffer = ByteBuffer()

    public func write(buffer: Array<Byte>): Unit {
        this.outputStream = ByteBuffer(buffer)
    }

    public func seek(sp: SeekPosition): Int64 {
        return this.outputStream.seek(sp)
    }
}

main(): Unit {
    let testStream = TestStream()
    let bufferedStream = BufferedOutputStream(testStream)

    // 写入一些数据
    let data = "Hello World".toArray()
    bufferedStream.write(data)
    bufferedStream.flush()

    // 输出流中未读的数据量
    println("Remain Length: ${bufferedStream.remainLength}")
}
```

运行结果：

```text
Remain Length: 11
```

### func seek(SeekPosition)

```cangjie
public func seek(sp: SeekPosition): Int64
```

功能：移动光标到指定的位置。

> **说明：**
>
> - 指定的位置不能位于流中数据头部之前。
> - 指定位置可以超过流中数据末尾。
> - 调用该函数会先将缓冲区内的数据写到绑定的输出流里，再移动光标的位置。

参数：

- sp: [SeekPosition](io_package_enums.md#enum-seekposition) - 指定光标移动后的位置。

返回值：

- [Int64](../../core/core_package_api/core_package_intrinsics.md#int64) - 返回流中数据的起点到移动后位置的偏移量（以字节为单位）。

异常：

- [IOException](io_package_exceptions.md#class-ioexception) - 当指定的位置位于流中数据头部之前时，抛出异常。

示例：

<!-- verify -->
```cangjie
import std.io.BufferedOutputStream
import std.io.OutputStream
import std.io.ByteBuffer
import std.io.Seekable
import std.io.SeekPosition
import std.io.IOException
import std.io.readToEnd

/**
 * 自定义实现 OutputStream 和 Seekable 接口的类 A
 */
public class A <: OutputStream & Seekable {
    public var outputStream: ByteBuffer = ByteBuffer()

    public func write(buffer: Array<Byte>): Unit {
        this.outputStream = ByteBuffer(buffer)
    }

    public func seek(sp: SeekPosition): Int64 {
        return outputStream.seek(sp)
    }
}

main(): Unit {
    let seekableStream = A()
    let bufferedStream = BufferedOutputStream(seekableStream)
    let data = "Hello World".toArray()
    bufferedStream.write(data)
    bufferedStream.flush()

    /* 输出当前流中总数据量，当前光标位置，当前流中未读的数据量 */
    println("Length : ${bufferedStream.length}")
    println("Position : ${bufferedStream.position}")
    println("Remain Length : ${bufferedStream.remainLength}")

    /* 移动光标到指定位置，虽然超过了流中数据末尾但是合法的 */
    println("Position after seek() : ${bufferedStream.seek(SeekPosition.Current(11))}")

    /* 尝试移动到数据头部之前，抛出异常 */
    try {
        bufferedStream.seek(SeekPosition.Begin(-1))
    } catch (e: IOException) {
        println("Error: " + e.message)
    }

    /* 将光标移动到第一个单词之后，读取后续的数据 */
    bufferedStream.seek(SeekPosition.Begin(6))
    println(String.fromUtf8(readToEnd(seekableStream.outputStream)))
}
```

运行结果：

```text
Length : 11
Position : 0
Remain Length : 11
Position after seek() : 11
Error: Failed to seek: position '-1' is before the beginning of the stream.
World
```

## extend\<T> StringReader\<T> <: Resource where T <: Resource

```cangjie
extend<T> StringReader<T> <: Resource where T <: Resource
```

功能：为 [StringReader](./io_package_classes.md#class-stringreadert-where-t--inputstream) 实现 [Resource](../../core/core_package_api/core_package_interfaces.md#interface-resource) 接口，该类型对象可在 `try-with-resource` 语法上下文中实现自动资源释放。

被扩展类型：

- [StringReader\<T>](./io_package_classes.md#class-stringreadert-where-t--inputstream)

扩展接口：

- [Resource](../../core/core_package_api/core_package_interfaces.md#interface-resource)

### func close()

```cangjie
public func close(): Unit
```

功能：关闭当前流。

> **注意：**
>
> 调用此方法后不可再调用 [StringReader](io_package_classes.md#class-stringreadert-where-t--inputstream) 的其他接口，否则会造成非预期现象。

示例：

<!-- verify -->
```cangjie
import std.io.*

/**
 * 自定义实现 InputStream 和 Resource 接口的类
 */
public class TestStream <: InputStream & Resource {
    private var closed: Bool = false

    public func read(buffer: Array<Byte>): Int64 {
        if (this.closed) {
            return 0
        }
        let data = "Hello World".toArray()
        let inputStream = ByteBuffer(data)
        return inputStream.read(buffer)
    }

    public func isClosed(): Bool {
        return closed
    }

    public func close(): Unit {
        println("Stream is closed")
        closed = true
    }
}

main(): Unit {
    let testStream = TestStream()
    let stringReader = StringReader(testStream)

    // 检查流是否关闭
    println("Is closed before close(): ${stringReader.isClosed()}")

    // 关闭流
    stringReader.close()

    // 检查流是否关闭
    println("Is closed after close(): ${stringReader.isClosed()}")
}
```

运行结果：

```text
Is closed before close(): false
Stream is closed
Is closed after close(): true
```

### func isClosed()

```cangjie
public func isClosed(): Bool
```

功能：判断当前流是否关闭。

返回值：

- [Bool](../../core/core_package_api/core_package_intrinsics.md#bool) - 如果当前流已经被关闭，返回 true，否则返回 false。

示例：

<!-- verify -->
```cangjie
import std.io.*

/**
 * 自定义实现 InputStream 和 Resource 接口的类
 */
public class TestStream <: InputStream & Resource {
    private var closed: Bool = false

    public func read(buffer: Array<Byte>): Int64 {
        if (this.closed) {
            return 0
        }
        let data = "Hello World".toArray()
        let inputStream = ByteBuffer(data)
        return inputStream.read(buffer)
    }

    public func isClosed(): Bool {
        return closed
    }

    public func close(): Unit {
        println("Stream is closed")
        closed = true
    }
}

main(): Unit {
    let testStream = TestStream()
    let stringReader = StringReader(testStream)

    // 检查流是否关闭
    println("Is closed before close(): ${stringReader.isClosed()}")

    // 关闭流
    stringReader.close()

    // 检查流是否关闭
    println("Is closed after close(): ${stringReader.isClosed()}")
}
```

运行结果：

```text
Is closed before close(): false
Stream is closed
Is closed after close(): true
```

## extend\<T> StringReader\<T> <: Seekable where T <: Seekable

```cangjie
extend<T> StringReader<T> <: Seekable where T <: Seekable
```

功能：为 [StringReader](./io_package_classes.md#class-stringreadert-where-t--inputstream) 实现 [Seekable](./io_package_interfaces.md#interface-seekable) 接口，支持查询数据长度，移动光标等操作。

被扩展类型：

- [StringReader\<T>](./io_package_classes.md#class-stringreadert-where-t--inputstream)

扩展接口：

- [Seekable](io_package_interfaces.md#interface-seekable)

### prop position

```cangjie
public prop position: Int64
```

功能：返回当前光标位置。

类型：[Int64](../../core/core_package_api/core_package_intrinsics.md#int64)

示例：

<!-- verify -->
```cangjie
import std.io.*

/**
 * 自定义实现 InputStream 和 Seekable 接口的类
 */
public class TestStream <: InputStream & Seekable {
    private var inputStream: ByteBuffer = ByteBuffer("Hello World".toArray())

    public func read(buffer: Array<Byte>): Int64 {
        return this.inputStream.read(buffer)
    }

    public func seek(sp: SeekPosition): Int64 {
        return this.inputStream.seek(sp)
    }
}

main(): Unit {
    let testStream = TestStream()
    let stringReader = StringReader(testStream)

    // 读取一些数据
    stringReader.read()

    // 输出当前光标位置
    println("Position: ${stringReader.position}")
}
```

运行结果：

```text
Position: 1
```

### func seek(SeekPosition)

```cangjie
public func seek(sp: SeekPosition): Int64
```

功能：移动光标到指定的位置。

> **说明：**
>
> - 指定的位置不能位于流中数据头部之前。
> - 指定位置可以超过流中数据末尾。

参数：

- sp: [SeekPosition](io_package_enums.md#enum-seekposition) - 指定光标移动后的位置。

返回值：

- [Int64](../../core/core_package_api/core_package_intrinsics.md#int64) - 返回流中数据的起点到移动后位置的偏移量（以字节为单位）。

异常：

- [IOException](io_package_exceptions.md#class-ioexception) - 当指定的位置位于流中数据头部之前时，抛出异常。

示例：

<!-- verify -->
```cangjie
import std.io.*

/**
 * 自定义实现 InputStream 和 Seekable 接口的类
 */
public class TestStream <: InputStream & Seekable {
    public var inputStream: ByteBuffer = ByteBuffer("Hello World".toArray())

    public func read(buffer: Array<Byte>): Int64 {
        return this.inputStream.read(buffer)
    }

    public func seek(sp: SeekPosition): Int64 {
        return this.inputStream.seek(sp)
    }
}

main(): Unit {
    let seekableStream = TestStream()
    let stringReader = StringReader(seekableStream)
    stringReader.read()

    /* 输出当前光标位置，当前流中总数据量，当前流中未读的数据量 */
    println("Position : ${stringReader.position}")
    println("Length : ${stringReader.length}")
    println("Remain Length : ${stringReader.remainLength}")

    /* 移动光标到指定位置，虽然超过了流中数据末尾但是合法的 */
    println("Position after seek() : ${stringReader.seek(SeekPosition.Current(11))}")

    /* 尝试移动到数据头部之前，抛出异常 */
    try {
        stringReader.seek(SeekPosition.Begin(-1))
    } catch (e: IOException) {
        println("Error: " + e.message)
    }

    /* 将光标移动到第一个单词之后，读取后续的数据 */
    stringReader.seek(SeekPosition.Begin(6))
    println(String.fromUtf8(readToEnd(seekableStream.inputStream)))
}
```

运行结果：

```text
Position : 1
Length : 11
Remain Length : 10
Position after seek() : 12
Error: Failed to seek: position '-1' is before the beginning of the stream.
World
```

## extend\<T> StringWriter\<T> <: Resource where T <: Resource

```cangjie
extend<T> StringWriter<T> <: Resource where T <: Resource
```

功能：为 [StringWriter](./io_package_classes.md#class-stringwritert-where-t--outputstream) 实现 [Resource](../../core/core_package_api/core_package_interfaces.md#interface-resource) 接口，该类型对象可在 `try-with-resource` 语法上下文中实现自动资源释放。

被扩展类型：

- [StringWriter\<T>](./io_package_classes.md#class-stringwritert-where-t--outputstream)

扩展接口：

- [Resource](../../core/core_package_api/core_package_interfaces.md#interface-resource)

### func close()

```cangjie
public func close(): Unit
```

功能：关闭当前流。

> **注意：**
>
> 调用此方法后不可再调用 [StringWriter](io_package_classes.md#class-stringwritert-where-t--outputstream) 的其他接口，否则会造成非预期现象。

示例：

<!-- verify -->
```cangjie
import std.io.*

/**
 * 自定义实现 OutputStream 和 Resource 接口的类
 */
public class TestStream <: OutputStream & Resource {
    private var closed: Bool = false
    private var outputStream: ByteBuffer = ByteBuffer()

    public func write(buffer: Array<Byte>): Unit {
        if (this.closed) {
            return
        }
        this.outputStream = ByteBuffer(buffer)
    }

    public func isClosed(): Bool {
        return closed
    }

    public func close(): Unit {
        println("Stream is closed")
        closed = true
    }
}

main(): Unit {
    let testStream = TestStream()
    let stringWriter = StringWriter(testStream)

    // 检查流是否关闭
    println("Is closed before close(): ${stringWriter.isClosed()}")

    // 关闭流
    stringWriter.close()

    // 检查流是否关闭
    println("Is closed after close(): ${stringWriter.isClosed()}")
}
```

运行结果：

```text
Is closed before close(): false
Stream is closed
Is closed after close(): true
```

### func isClosed()

```cangjie
public func isClosed(): Bool
```

功能：判断当前流是否关闭。

返回值：

- [Bool](../../core/core_package_api/core_package_intrinsics.md#bool) - 如果当前流已经被关闭，返回 true，否则返回 false。

示例：

<!-- verify -->
```cangjie
import std.io.*

/**
 * 自定义实现 OutputStream 和 Resource 接口的类
 */
public class TestStream <: OutputStream & Resource {
    private var closed: Bool = false
    public var outputStream: ByteBuffer = ByteBuffer()

    public func write(buffer: Array<Byte>): Unit {
        if (this.closed) {
            return
        }
        this.outputStream = ByteBuffer(buffer)
    }

    public func isClosed(): Bool {
        return closed
    }

    public func close(): Unit {
        println("Stream is closed")
        closed = true
    }
}

main(): Unit {
    let testStream = TestStream()
    let stringWriter = StringWriter(testStream)

    /* 使用 try-with-resource 语法获取资源 */
    try (r = stringWriter) {
        println("Get the resource")
        let data = "Hello World".toArray()
        r.write(data)
        r.flush()
        println(r.isClosed())
        println(String.fromUtf8(readToEnd(testStream.outputStream)))
    }

    /* 自动调用 close() 函数释放资源 */
    println(stringWriter.isClosed())
}
```

运行结果：

```text
Get the resource
false
[72, 101, 108, 108, 111, 32, 87, 111, 114, 108, 100]
Stream is closed
true
```

## extend\<T> StringWriter\<T> <: Seekable where T <: Seekable

```cangjie
extend<T> StringWriter<T> <: Seekable where T <: Seekable
```

功能：为 [StringWriter](./io_package_classes.md#class-stringwritert-where-t--outputstream) 实现 [Seekable](./io_package_interfaces.md#interface-seekable) 接口，支持查询数据长度，移动光标等操作。

被扩展类型：

- [StringWriter\<T>](./io_package_classes.md#class-stringwritert-where-t--outputstream)

扩展接口：

- [Seekable](io_package_interfaces.md#interface-seekable)

### func seek(SeekPosition)

```cangjie
public func seek(sp: SeekPosition): Int64
```

功能：移动光标到指定的位置。

> **说明：**
>
> - 指定的位置不能位于流中数据头部之前。
> - 指定位置可以超过流中数据末尾。

参数：

- sp: [SeekPosition](io_package_enums.md#enum-seekposition) - 指定光标移动后的位置。

返回值：

- [Int64](../../core/core_package_api/core_package_intrinsics.md#int64) - 返回流中数据的起点到移动后位置的偏移量（以字节为单位）。

异常：

- [IOException](io_package_exceptions.md#class-ioexception) - 当指定的位置位于流中数据头部之前时，抛出异常。

示例：

<!-- verify -->
```cangjie
import std.io.*

/**
 * 自定义实现 OutputStream 和 Seekable 接口的类 A
 */
public class A <: OutputStream & Seekable {
    public var outputStream: ByteBuffer = ByteBuffer()

    public func write(buffer: Array<Byte>): Unit {
        this.outputStream = ByteBuffer(buffer)
    }

    public func seek(sp: SeekPosition): Int64 {
        return outputStream.seek(sp)
    }
}

main(): Unit {
    let seekableStream = A()
    let stringWriter = StringWriter(seekableStream)
    let data = "Hello World".toArray()
    stringWriter.write(data)
    stringWriter.flush()

    /* 移动光标到指定位置，虽然超过了流中数据末尾但是合法的 */
    println("Position after seek() : ${stringWriter.seek(SeekPosition.Current(11))}")

    /* 尝试移动到数据头部之前，抛出异常 */
    try {
        stringWriter.seek(SeekPosition.Begin(-1))
    } catch (e: IOException) {
        println("Error: " + e.message)
    }

    /* 将光标移动到第一个单词之后，读取后续的数据（输入流已经作为 String 进入） */
    stringWriter.seek(SeekPosition.Begin(29))
    println(String.fromUtf8(readToEnd(seekableStream.outputStream)))
}
```

运行结果：

```text
Position after seek() : 11
Error: Failed to seek: position '-1' is before the beginning of the stream.
87, 111, 114, 108, 100]
```

