# Extensions

## extend\<T> BufferedInputStream\<T> <: Resource where T <: Resource

```cangjie
extend<T> BufferedInputStream<T> <: Resource where T <: Resource
```

Function: Implements the [Resource](../../core/core_package_api/core_package_interfaces.md#interface-resource) interface for [BufferedInputStream](./io_package_classes.md#class-bufferedinputstreamt-where-t--inputstream), enabling objects of this type to support automatic resource release in `try-with-resource` syntax contexts.

Extended type:

- [BufferedInputStream\<T>](./io_package_classes.md#class-bufferedinputstreamt-where-t--inputstream)

Extended interfaces:

- Resource

### func close()

```cangjie
public func close(): Unit
```

Function: Closes the current stream.

> **Note:**
>
> After calling this method, no other interfaces of [BufferedInputStream](io_package_classes.md#class-bufferedinputstreamt-where-t--inputstream) should be invoked, as it may lead to undefined behavior.

### func isClosed()

```cangjie
public func isClosed(): Bool
```

Function: Checks whether the current stream is closed.

Returns:

- [Bool](../../core/core_package_api/core_package_intrinsics.md#bool) - Returns `true` if the current stream has been closed, otherwise returns `false`.

Example:

<!-- verify -->
```cangjie
import std.io.BufferedInputStream
import std.io.InputStream
import std.io.ByteBuffer

/**
 * Custom class A implementing both InputStream and Resource interfaces
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

    /* Acquire resource using try-with-resource syntax */
    try (r = bufferedStream) {
        println("Get the resource")
        let data = Array<Byte>(11, repeat: 0)
        r.read(data)
        println(r.isClosed())
        println(String.fromUtf8(data))
    }

    /* Automatically calls close() to release resources */
    println(bufferedStream.isClosed())
}
```

Execution Result:

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

Function: Implements the [Seekable](./io_package_interfaces.md#interface-seekable) interface for [BufferedInputStream](./io_package_classes.md#class-bufferedinputstreamt-where-t--inputstream), supporting operations such as querying data length and moving the cursor.

Extended type:

- [BufferedInputStream\<T>](./io_package_classes.md#class-bufferedinputstreamt-where-t--inputstream)

Extended interfaces:

- Seekable

### prop length

```cangjie
public prop length: Int64
```

Function: Returns the total amount of data in the current stream (in bytes).

Type: [Int64](../../core/core_package_api/core_package_intrinsics.md#int64)

### prop position

```cangjie
public prop position: Int64
```

Function: Returns the current cursor position.

Type: [Int64](../../core/core_package_api/core_package_intrinsics.md#int64)

### prop remainLength

```cangjie
public prop remainLength: Int64
```

Function: Returns the amount of unread data in the current stream (in bytes, does not include the data in the buffer).

Type: [Int64](../../core/core_package_api/core_package_intrinsics.md#int64)

### func seek(SeekPosition)

```cangjie
public func seek(sp: SeekPosition): Int64
```

Function: Moves the cursor to the specified position.

> **Note:**
>
> - The specified position cannot be before the start of the data in the stream.
> - The specified position can exceed the end of the data in the stream.
> - Calling this function will first clear the buffer before moving the cursor position.

Parameters:

- sp: [SeekPosition](io_package_enums.md#enum-seekposition) - Specifies the position to move the cursor to.

Returns:

- [Int64](../../core/core_package_api/core_package_intrinsics.md#int64) - Returns the offset from the start of the stream data to the new position (in bytes).

Exceptions:

- [IOException](io_package_exceptions.md#class-ioexception) - Thrown when the specified position is before the start of the data in the stream.

Example:

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
 * Custom class A implementing InputStream and Seekable interfaces
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
    /* Output the total data volume in the current stream, current cursor position, and unread data volume in the stream */
    println("Length : " + bufferedStream.length.toString())
    println("Position : " + bufferedStream.position.toString())
    println("Remain Length : " + bufferedStream.remainLength.toString())

    /* Move cursor to specified position (legal even if exceeding the end of stream data) */
    println("Position after seek() : " + bufferedStream.seek(SeekPosition.Current(11)).toString())

    /* Attempt to move before data head (throws exception) */
    try {
        bufferedStream.seek(SeekPosition.Begin(-1))
    } catch (e: IOException) {
        println("Error: " + e.message)
    }

    /* Move cursor after the first word and read subsequent data */
    bufferedStream.seek(SeekPosition.Begin(6))
    println(String.fromUtf8(readToEnd(seekableStream.inputStream)))
}
```

Execution result:

```text
Length : 11
Position : 11
Remain Length : 0
Position after seek() : 22
Error: Can't move the position before the beginning of the stream.
World
```

## extend\<T> BufferedOutputStream\<T> <: Resource where T <: Resource

```cangjie
extend<T> BufferedOutputStream<T> <: Resource where T <: Resource
```

Function: Implements the [Resource](../../core/core_package_api/core_package_interfaces.md#interface-resource) interface for [BufferedOutputStream](./io_package_classes.md#class-bufferedoutputstreamt-where-t--outputstream), enabling automatic resource release in `try-with-resource` syntax contexts.

Extended type:

- [BufferedOutputStream\<T>](./io_package_classes.md#class-bufferedoutputstreamt-where-t--outputstream)

Extended interfaces:

- Resource

### func close()

```cangjie
public func close(): Unit
```

Function: Closes the current stream.

> **Note:**
>
> After calling this method, no other interfaces of [BufferedOutputStream](io_package_classes.md#class-bufferedoutputstreamt-where-t--outputstream) should be invoked, as it may lead to undefined behavior.

### func isClosed()

```cangjie
public func isClosed(): Bool
```

Function: Determines whether the current stream is closed.

Returns:

- [Bool](../../core/core_package_api/core_package_intrinsics.md#bool) - Returns `true` if the current stream is closed, otherwise returns `false`.

Example:

<!-- verify -->
```cangjie
import std.io.BufferedOutputStream
import std.io.OutputStream
import std.io.ByteBuffer
import std.io.readToEnd

/**
 * Custom class A implementing the OutputStream and Resource interfaces
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

    /* Acquire resource using try-with-resource syntax */
    try (r = bufferedStream) {
        println("Get the resource")
        let data = "Hello World".toArray()
        r.write(data)
        r.flush()
        println(r.isClosed())
        println(String.fromUtf8(readToEnd(resourceStream.outputStream)))
    }

    /* Automatically calls close() to release resources */
    println(bufferedStream.isClosed())
}
```

Execution Result:

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

Function: Implements the [Seekable](./io_package_interfaces.md#interface-seekable) interface for [BufferedOutputStream](./io_package_classes.md#class-bufferedoutputstreamt-where-t--outputstream), supporting operations such as querying data length and moving the cursor.

Extended type:

- [BufferedOutputStream\<T>](./io_package_classes.md#class-bufferedoutputstreamt-where-t--outputstream)

Extended interfaces:

- Seekable

### prop length

```cangjie
public prop length: Int64
```

Function: Returns the total amount of data in the current stream (in bytes).

Type: [Int64](../../core/core_package_api/core_package_intrinsics.md#int64)

### prop position

```cangjie
public prop position: Int64
```

Function: Returns the current cursor position.

Type: [Int64](../../core/core_package_api/core_package_intrinsics.md#int64)

### prop remainLength

```cangjie
public prop remainLength: Int64
```

Function: Returns the amount of unread data remaining in the current stream (in bytes).

Type: [Int64](../../core/core_package_api/core_package_intrinsics.md#int64)

### func seek(SeekPosition)

```cangjie
public func seek(sp: SeekPosition): Int64
```

Function: Moves the cursor to the specified position.

> **Note:**
>
> - The specified position cannot be before the beginning of the stream data.
> - The specified position can exceed the end of the stream data.
> - Calling this function will first write buffered data to the bound output stream before moving the cursor position.

Parameters:

- sp: [SeekPosition](io_package_enums.md#enum-seekposition) - Specifies the target position after cursor movement.

Returns:

- [Int64](../../core/core_package_api/core_package_intrinsics.md#int64) - Returns the offset (in bytes) from the start of the stream data to the new cursor position.

Exceptions:

- [IOException](io_package_exceptions.md#class-ioexception) - Thrown when the specified position is before the beginning of the stream data.

Example:

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
 * Custom class A implementing OutputStream and Seekable interfaces
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

    /* Output total data length, current cursor position, and remaining unread data length */
    println("Length : " + bufferedStream.length.toString())
    println("Position : " + bufferedStream.position.toString())
    println("Remain Length : " + bufferedStream.remainLength.toString())

    /* Move cursor to specified position (legal even when exceeding stream end) */
    println("Position after seek() : " + bufferedStream.seek(SeekPosition.Current(11)).toString())

    /* Attempt to move before stream beginning (throws exception) */
    try {
        bufferedStream.seek(SeekPosition.Begin(-1))
    } catch (e: IOException) {
        println("Error: " + e.message)
    }

    /* Move cursor after first word and read subsequent data */
    bufferedStream.seek(SeekPosition.Begin(6))
    println(String.fromUtf8(readToEnd(seekableStream.outputStream)))
}
```

Execution Result:

```text
Length : 11
Position : 0
Remain Length : 11
Position after seek() : 11
Error: Can't move the position before the beginning of the stream.
World
```

## extend\<T> StringReader\<T> <: Resource where T <: Resource

```cangjie
extend<T> StringReader<T> <: Resource where T <: Resource
```

Function: Implements the [Resource](../../core/core_package_api/core_package_interfaces.md#interface-resource) interface for [StringReader](./io_package_classes.md#class-stringreadert-where-t--inputstream), enabling automatic resource release in `try-with-resource` syntax contexts.

Extended type:

- [StringReader\<T>](./io_package_classes.md#class-stringreadert-where-t--inputstream)

Extended interfaces:

- Resource

### func close()

```cangjie
public func close(): Unit
```

Function: Closes the current stream.

> **Note:**
>
> After calling this method, no other interfaces of [StringReader](io_package_classes.md#class-stringreadert-where-t--inputstream) should be invoked, otherwise undefined behavior may occur.

### func isClosed()

```cangjie
public func isClosed(): Bool
```

Function: Determines whether the current stream is closed.

Returns:

- [Bool](../../core/core_package_api/core_package_intrinsics.md#bool) - Returns `true` if the current stream has been closed, otherwise returns `false`.

## extend\<T> StringReader\<T> <: Seekable where T <: Seekable

```cangjie
extend<T> StringReader<T> <: Seekable where T <: Seekable
```

Function: Implements the [Seekable](./io_package_interfaces.md#interface-seekable) interface for [StringReader](./io_package_classes.md#class-stringreadert-where-t--inputstream), supporting operations such as querying data length and moving the cursor.

Extended type:

- [StringReader\<T>](./io_package_classes.md#class-stringreadert-where-t--inputstream)

Extended interfaces:

- Seekable

### prop position

```cangjie
public prop position: Int64
```

Function: Returns the current cursor position.

Type: [Int64](../../core/core_package_api/core_package_intrinsics.md#int64)

### func seek(SeekPosition)

```cangjie
public func seek(sp: SeekPosition): Int64
```

Function: Moves the cursor to the specified position.

> **Note:**
>
> - The specified position cannot be before the start of the stream data.
> - The specified position can exceed the end of the stream data.

Parameters:

- sp: [SeekPosition](io_package_enums.md#enum-seekposition) - Specifies the position to which the cursor will be moved.

Returns:

- [Int64](../../core/core_package_api/core_package_intrinsics.md#int64) - Returns the offset (in bytes) from the start of the stream data to the new cursor position.

Exceptions:

- [IOException](io_package_exceptions.md#class-ioexception) - Thrown when the specified position is before the start of the stream data.

## extend\<T> StringWriter\<T> <: Resource where T <: Resource

```cangjie
extend<T> StringWriter<T> <: Resource where T <: Resource
```

Function: Implements the [Resource](../../core/core_package_api/core_package_interfaces.md#interface-resource) interface for [StringWriter](./io_package_classes.md#class-stringwritert-where-t--outputstream), enabling automatic resource release in `try-with-resource` syntax contexts.

Extended type:

- [StringWriter\<T>](./io_package_classes.md#class-stringwritert-where-t--outputstream)

Extended interfaces:

- Resource

### func close()

```cangjie
public func close(): Unit
```

Function: Closes the current stream.

> **Note:**
>
> After calling this method, no other interfaces of [StringWriter](io_package_classes.md#class-stringwritert-where-t--outputstream) should be invoked, as it may lead to undefined behavior.

### func isClosed()

```cangjie
public func isClosed(): Bool
```

Function: Determines whether the current stream is closed.

Returns:

- [Bool](../../core/core_package_api/core_package_intrinsics.md#bool) - Returns `true` if the current stream has been closed, otherwise returns `false`.

## extend\<T> StringWriter\<T> <: Seekable where T <: Seekable

```cangjie
extend<T> StringWriter<T> <: Seekable where T <: Seekable
```

Function: Implements the [Seekable](./io_package_interfaces.md#interface-seekable) interface for [StringWriter](./io_package_classes.md#class-stringwritert-where-t--outputstream), supporting operations such as querying data length and moving the cursor.

Extended type:

- [StringWriter\<T>](./io_package_classes.md#class-stringwritert-where-t--outputstream)

Extended interfaces:

- Seekable

### func seek(SeekPosition)

```cangjie
public func seek(sp: SeekPosition): Int64
```

Function: Moves the cursor to the specified position.

> **Note:**
>
> - The specified position cannot be before the head of the data in the stream.
> - The specified position can exceed the end of the data in the stream.

Parameters:

- sp: [SeekPosition](io_package_enums.md#enum-seekposition) - Specifies the position to which the cursor will be moved.

Returns:

- [Int64](../../core/core_package_api/core_package_intrinsics.md#int64) - Returns the offset (in bytes) from the start of the stream data to the new cursor position.

Exceptions:

- [IOException](io_package_exceptions.md#class-ioexception) - Thrown when the specified position is before the head of the data in the stream.

