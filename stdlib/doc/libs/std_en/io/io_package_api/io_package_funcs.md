# Functions

## func copy(InputStream, OutputStream)

```cangjie
public func copy(from: InputStream, to!: OutputStream): Int64
```

Function: Copies unread data from an input stream to an output stream.

Parameters:

- from: [InputStream](io_package_interfaces.md#interface-inputstream) - The input stream to read data from.
- to!: [OutputStream](io_package_interfaces.md#interface-outputstream) - The output stream to copy data to.

Returns:

- [Int64](../../core/core_package_api/core_package_intrinsics.md#int64) - The number of bytes copied.

Example:

<!-- verify -->
```cangjie
import std.io.ByteBuffer
import std.io.copy

main(): Unit {
    let sourceStream = ByteBuffer()
    let targetStream = ByteBuffer()

    /* Write data to source input stream */
    let sourceData = "Hello, World!".toArray()
    sourceStream.write(sourceData)

    /* Use copy function to transfer data from source to target stream */
    let copiedBytes = copy(sourceStream, to: targetStream)
    println("Copied ${copiedBytes} bytes.")

    /* Read data from target output stream */
    let targetData: Array<Byte> = Array<Byte>(sourceData.size, repeat: 0)
    targetStream.read(targetData)
    println("Data copied to target stream: ${String.fromUtf8(targetData)}")
}
```

Execution Result:

```text
Copied 13 bytes.
Data copied to target stream: Hello, World!
```

## func readFully(InputStream, Array\<Byte>)

```cangjie
public func readFully(from: InputStream, buffer: Array<Byte>): Unit
```

Function: Repeatedly calls `from.read` until `buffer` is full. A normal return means exactly `buffer.size` bytes were read. Each read targets only the unfilled portion. Once full, the function stops without reading further or waiting for EOF. An empty buffer succeeds without calling the underlying reader.

The function inherits the waiting behavior of `from.read` and may block waiting for further input. It does not provide timeouts, cancellation, or message framing, and does not close the stream. Multiple reads are not atomic; callers must coordinate other consumers of the same stream.

Parameters:

- from: [InputStream](io_package_interfaces.md#interface-inputstream) - The input stream to read from.
- buffer: [Array](../../core/core_package_api/core_package_structs.md#struct-arrayt)\<[Byte](../../core/core_package_api/core_package_types.md#type-byte)> - The destination to fill; an array slice may also be used.

Exceptions:

- [IOException](io_package_exceptions.md#class-ioexception) - The underlying reader returns `0` before the buffer is full, or returns a count below `0` or above the requested length. An incomplete-read message includes the expected and completed byte counts.
- Exceptions thrown by the underlying `read` are propagated unchanged. On failure, the destination may be partially modified and consumed input is not rolled back. The entire buffer must not be treated as a complete result.

> **Note:**
>
> This function relies on the concrete stream's EOF and error reporting. If a stream also maps read failures to `0`, this function reports an incomplete read but cannot recover the lost error cause. It does not change the concrete stream's existing error handling.

Passing a `ConsoleReader` to this function still calls its legacy line-oriented `read`, which may wait for a newline; redirected input on Windows follows the default text-translation rules. For exact reads from standard input through its short-read path, call [ConsoleReader.readFully(Array\<Byte>)](../../env/env_package_api/env_package_classes.md#func-readfullyarraybyte). When mixed with line reads, that method is still affected by text-translated data in the `ConsoleReader` buffer.

Example:

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
        println("Fewer than 8 input bytes")
    }
}
```

Output:

```text
[97, 98, 99, 100, 101, 102, 103, 104]
Fewer than 8 input bytes
```

## func readString\<T>(T) where T <: InputStream & Seekable

```cangjie
public func readString<T>(from: T): String where T <: InputStream & Seekable
```

Function: Reads all remaining content from the parameter and returns a string.

Parameters:

- from: T - The object to read data from.

Returns:

- [String](../../core/core_package_api/core_package_structs.md#struct-string) - The resulting string read.

Exceptions:

- [ContentFormatException](io_package_exceptions.md#class-contentformatexception) - Thrown when remaining bytes don't conform to UTF-8 encoding rules.

Example:

<!-- verify -->
```cangjie
import std.io.ByteBuffer
import std.io.readString
import std.io.ContentFormatException

main(): Unit {
    let inputStream = ByteBuffer()

    /* Write data to input stream */
    let sourceData = "Hello, World!".toArray()
    inputStream.write(sourceData)

    /* Use readString to read all remaining content */
    try {
        let result = readString(inputStream)
        println("Read string: ${result}")
    } catch (e: ContentFormatException) {
        println("Error: ${e.message}")
    }

    /* Write an invalid UTF-8 string to input stream */
    let sourceDataError: Array<UInt8> = [0xC3, 0x28, 0x48, 0x65, 0x6C, 0x6C, 0x6F]
    inputStream.write(sourceDataError)

    /* Use readString to read all remaining content */
    try {
        let result = readString(inputStream)
        println("Read string: ${result}")
    } catch (e: ContentFormatException) {
        println("Error: ${e.message}")
    }
}
```

Execution Result:

```text
Read string: Hello, World!
Error: Invalid unicode scalar value.
```

## func readStringUnchecked\<T>(T) where T <: InputStream & Seekable

```cangjie
public unsafe func readStringUnchecked<T>(from: T): String where T <: InputStream & Seekable
```

Function: Reads all remaining content from the parameter and returns a string without validation.

Parameters:

- from: T - The object to read data from.

Returns:

- [String](../../core/core_package_api/core_package_structs.md#struct-string) - The resulting string read.

Example:

<!-- run -->
```cangjie
import std.io.ByteBuffer
import std.io.readStringUnchecked
import std.io.ContentFormatException

main(): Unit {
    let inputStream = ByteBuffer()

    /* Write data to input stream */
    let sourceData = "Hello, World!".toArray()
    inputStream.write(sourceData)

    /* Use readString to read all remaining content (normal output: Read string: Hello, World!) */
    unsafe {
        let result = readStringUnchecked(inputStream)
        println("Read string: ${result}")
    }

    /* Write an invalid UTF-8 string to input stream */
    let sourceDataError: Array<UInt8> = [0xC3, 0x28, 0x48, 0x65, 0x6C, 0x6C, 0x6F]
    inputStream.write(sourceDataError)

    /* Using readString on invalid UTF-8 will produce unexpected output characters */
    unsafe {
        let result = readStringUnchecked(inputStream)
        println("Read string: ${result}")
    }
}
```

## func readToEnd\<T>(T) where T <: InputStream & Seekable

```cangjie
public func readToEnd<T>(from: T): Array<Byte> where T <: InputStream & Seekable
```

Function: Retrieves all unread data from the parameter.

Parameters:

- from: T - The object to read data from.

Returns:

- [Array](../../core/core_package_api/core_package_structs.md#struct-arrayt)\<[Byte](../../core/core_package_api/core_package_types.md#type-byte)> - A copy of the unread data.

Example:

<!-- verify -->
```cangjie
import std.io.ByteBuffer
import std.io.readToEnd

main(): Unit {
    let inputStream = ByteBuffer()

    /* Write data to input stream */
    let sourceData = "Hello, World!".toArray()
    inputStream.write(sourceData)

    /* Use readToEnd to read all remaining content */
    let data = readToEnd(inputStream)
    println("${String.fromUtf8(data)}")
}
```

Execution Result:

```text
Hello, World!
```
