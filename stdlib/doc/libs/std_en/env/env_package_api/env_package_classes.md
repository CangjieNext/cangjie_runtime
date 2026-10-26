# Classes

## class ConsoleReader

```cangjie
public class ConsoleReader <: InputStream
```

Function: Provides the capability to read data from the console and convert it into characters or strings.

This type cannot be instantiated directly; instances can only be obtained via [getStdIn()](./env_package_funcs.md#func-getStdIn).  
Read operations are synchronous, with an internal buffer storing console input content. When the end of the input stream is reached and the current call has read no data, reading functions that return an optional type return `None`, while `read(arr: Array<Byte>)` and `readSome(arr: Array<Byte>)` return `0`.

> **Note:**
>
> There is only one instance of [ConsoleReader](./env_package_classes.md#class-consolereader), with all methods sharing the same buffer. For reading functions that return an optional type, the following circumstances may cause subsequent reads to return `None`:
>
> - When standard input is redirected to a file and the end-of-file (EOF) is reached.
> - In Linux environments, when `Ctrl+D` is pressed.
> - In Windows environments, when `Ctrl+Z` is pressed followed by Enter.

Parent Type:

- [InputStream](../../io/io_package_api/io_package_interfaces.md#interface-inputstream)

### func read()

```cangjie
public func read(): ?Rune
```

Function: Reads the next character from standard input.

Returns:

- ?[Rune](../../core/core_package_api/core_package_intrinsics.md#rune) - Returns the read character as ?[Rune](../../core/core_package_api/core_package_intrinsics.md#rune), or `None` if no character is read.

Exceptions:

- [IllegalArgumentException](../../core/core_package_api/core_package_exceptions.md#class-illegalargumentexception) - Thrown when the input does not conform to a `UTF-8` encoded string.

Example:
<!-- compile -->

```cangjie
import std.env.*

main() {
    getStdOut().write("Please enter information: ")
    var c = getStdIn().read() // Input: abc
    var r = c.getOrThrow()
    getStdOut().write("Input received: ")
    getStdOut().writeln(r)

    return
}
```

Execution Result:

```text
Please enter information: abc
Input received: a
```

### func read(Array\<Byte>)

```cangjie
public func read(arr: Array<Byte>): Int64
```

Function: Reads standard input into `arr`, preserving the existing behavior of obtaining line-oriented input and attempting to fill the array.

If the internal buffer cannot fill the array, the function continues obtaining data through the line-reading path, which may wait for a newline. If input ends or a read fails, the function returns the valid prefix obtained so far, which may be shorter than `arr.size`. An empty array returns `0`. This method is unsuitable for pipe interactions where the writer sends some data and waits for a response. To allow short reads, explicitly use [readSome(Array\<Byte>)](#func-readsomearraybyte).

The current implementation preserves the historical behavior that read failures may also return `0`. A zero return cannot distinguish all underlying errors from normal EOF.

For redirected input on Windows, the line-reading path preserves default text semantics: CRLF becomes LF, and `Ctrl+Z` (`0x1A`) acts as EOF.

> **Warning:**
>
> This function carries a risk: the read result might truncate a `UTF-8 code point` in the middle. If truncation occurs, converting this [Array](../../core/core_package_api/core_package_structs.md#struct-arrayt)\<[Byte](../../core/core_package_api/core_package_types.md#type-byte)> to a string may produce incorrect results or throw an exception.

Parameters:

- arr: [Array](../../core/core_package_api/core_package_structs.md#struct-arrayt)\<[Byte](../../core/core_package_api/core_package_types.md#type-byte)> - The target array.

Returns:

- [Int64](../../core/core_package_api/core_package_intrinsics.md#int64) - The number of bytes read by this call, `n`, where `0 <= n <= arr.size`. The data is stored in the first `n` elements of `arr`. Returns `0` if `arr` is empty, or if the end of the input stream is reached and the current call has read no data.

Example:
<!-- compile -->

```cangjie
import std.env.*

main() {
    // Obtain the standard input stream.
    let stdin = getStdIn()

    // Create a byte array to receive data.
    let buffer: Array<Byte> = [0, 0, 0, 0, 0]

    // The return value may be less than buffer.size; process only the bytes read.
    let readSize = stdin.read(buffer)
    println("Bytes read: ${readSize}")
    if (readSize > 0) {
        println("Data read: ${buffer.slice(0, readSize)}")
    }
}
```

With standard input redirected from a file containing only `abc` (without a newline), example output is:

```text
Bytes read: 3
Data read: [97, 98, 99]
```

### func readSome(Array\<Byte>)

```cangjie
public func readSome(arr: Array<Byte>): Int64
```

Function: Reads up to `arr.size` bytes from standard input into `arr`, allowing fewer bytes than the array length to be returned.

If the internal buffer contains data, the function copies an available prefix and returns without waiting for new data to fill the array. Otherwise, it attempts to read input. Reading from a redirected pipe does not require a newline or wait for the destination to fill or the writer to close. It may still block if no data is available and input has not ended. Interactive terminals remain subject to input modes such as line buffering. Allowing short reads does not mean nonblocking I/O.

This method shares buffered input with the other standard-input read methods. A short read does not indicate EOF, and a read does not correspond to one complete message. Process only the prefix identified by the returned count and accumulate data according to the protocol. Byte chunks may split UTF-8 characters; do not assume each chunk can be independently converted to a string.

For redirected input on Windows, using this method on its own reads raw bytes, including CRLF and `0x1A`. If `read`, `readln`, or `readUntil` has already prefetched data into this instance's buffer, this method first returns the remaining text-translated data. Mixing methods within the instance preserves consumption order, but does not guarantee unchanged raw bytes. When raw bytes are required, use this method from the start.

The method preserves the behavior of returning `0` for some underlying read failures. A zero return cannot distinguish all read errors from normal EOF.

Parameters:

- arr: [Array](../../core/core_package_api/core_package_structs.md#struct-arrayt)\<[Byte](../../core/core_package_api/core_package_types.md#type-byte)> - The destination array; an array slice may also be used.

Returns:

- [Int64](../../core/core_package_api/core_package_intrinsics.md#int64) - The number of bytes read, `n`, where `0 <= n <= arr.size`. Only the first `n` elements receive data from this call; the remaining elements are unchanged. Returns `0` for an empty array, normal EOF without data, or some read failures.

Example:

<!-- compile -->
```cangjie
import std.env.getStdIn

main() {
    let buffer = Array<Byte>(256, repeat: 0)
    let count = getStdIn().readSome(buffer)
    println(buffer.slice(0, count))
}
```

If a pipe writer sends `abc` and keeps its end open, this program can print `[97, 98, 99]` without requiring a newline or the writer to close the pipe.

### func readToEnd()

```cangjie
public func readToEnd(): ?String
```

Function: Reads all characters from standard input.

Reading continues until the end-of-file marker `EOF` is encountered, or until `Ctrl+D` is pressed in Linux or `Ctrl+Z` + Enter in Windows. Returns the read characters as ?[String](../../core/core_package_api/core_package_structs.md#struct-string), or `None` if no characters are read. This interface does not throw exceptions; even if the input is not a valid `UTF-8` encoded string, it will construct and return a [String](../../core/core_package_api/core_package_structs.md#struct-string), behaving equivalently to [String](../../core/core_package_api/core_package_structs.md#struct-string).[fromUtf8Uncheck](../../core/core_package_api/core_package_structs.md#static-func-fromutf8uncheckedarrayuint8)([Array](../../core/core_package_api/core_package_structs.md#struct-arrayt)\<[Byte](../../core/core_package_api/core_package_types.md#type-byte)>).

Returns:

- ?[String](../../core/core_package_api/core_package_structs.md#struct-string) - Returns all read data as ?[String](../../core/core_package_api/core_package_structs.md#struct-string).

### func readUntil((Rune) -> Bool)

```cangjie
public func readUntil(predicate: (Rune) -> Bool): ?String
```

Function: Reads data from standard input until a character satisfies the `predicate` condition.

The character that satisfies predicate: (Rune) -> [Bool](../../core/core_package_api/core_package_intrinsics.md#bool) is included in the result. Returns `None` if reading fails.

Parameters:

- predicate: (Rune) ->[Bool](../../core/core_package_api/core_package_intrinsics.md#bool) - The condition to terminate reading.

Returns:

- ?[String](../../core/core_package_api/core_package_structs.md#struct-string) - Returns the read data as ?[String](../../core/core_package_api/core_package_structs.md#struct-string).

Example:
<!-- compile -->

```cangjie
import std.env.*

main() {
    getStdOut().write("Please enter information: ")
    var c = getStdIn().readUntil({ch: Rune => ch > r'd' && ch < r'g'}) // Input: abcdefg
    var r = c.getOrThrow()
    getStdOut().writeln("Input received: " + r)

    return
}
```

Execution Result:

```text
Please enter information: abcdefg
Input received: abcde
```

### func readUntil(Rune)

```cangjie
public func readUntil(ch: Rune): ?String
```

Function: Reads data from standard input until the character `ch` is encountered.

`ch` is included in the result. If the end-of-file marker EOF is encountered, all read information is returned. Returns `None` if reading fails.

Parameters:

- ch: [Rune](../../core/core_package_api/core_package_intrinsics.md#rune) - The terminating character.

Returns:

- ?[String](../../core/core_package_api/core_package_structs.md#struct-string) - Returns the read data as ?[String](../../core/core_package_api/core_package_structs.md#struct-string).

Example:
<!-- compile -->

```cangjie
import std.env.*

main() {
    getStdOut().write("Please enter information: ")
    var c = getStdIn().readUntil(r'b') // Input: abc
    var r = c.getOrThrow()
    getStdOut().write("Input received: ")
    getStdOut().writeln(r)

    return
}
```

Execution Result:

```text
Please enter information: abc
Input received: ab
```

### func readln()

```cangjie
public func readln(): ?String
```

Function: Reads a line of text from standard input.

Returns the read characters as ?[String](../../core/core_package_api/core_package_structs.md#struct-string), excluding the trailing newline character. This interface does not throw exceptions; even if the input is not a valid `UTF-8` encoded string, it will construct and return a [String](../../core/core_package_api/core_package_structs.md#struct-string), behaving equivalently to [String](../../core/core_package_api/core_package_structs.md#struct-string).[fromUtf8Uncheck](../../core/core_package_api/core_package_structs.md#static-func-fromutf8uncheckedarrayuint8)([Array](../../core/core_package_api/core_package_structs.md#struct-arrayt)\<[Byte](../../core/core_package_api/core_package_types.md#type-byte)>).

Returns:

- ?[String](../../core/core_package_api/core_package_structs.md#struct-string) - The read line of data, or `None` if reading fails.

Example:
<!-- compile -->

```cangjie
import std.env.*

main() {
    getStdOut().write("Please enter information: ")
    var c = getStdIn().readln() // Input: abc
    var r = c.getOrThrow()
    getStdOut().write("Input received: ")
    getStdOut().writeln(r)

    return
}
```

Execution Result:

```text
Please enter information: abc
Input received: abc
```

## class ConsoleWriter

```cangjie
public class ConsoleWriter <: OutputStream
```

Function: This class provides thread-safe standard output functionality.

Each `write` call produces complete output to the console, and results from different `write` function calls will not be interleaved.  
This type cannot be instantiated directly; instances can only be obtained via [getStdOut()](./env_package_funcs.md#func-getStdOut) for standard output or [getStdErr()](./env_package_funcs.md#func-getStdErr) for standard error.

Parent Type:

- [OutputStream](../../io/io_package_api/io_package_interfaces.md#interface-outputstream)

### func flush()

```cangjie
public func flush(): Unit
```

Function: Flushes the output stream.

### func write(Array\<Byte>)

```cangjie
public func write(buffer: Array<Byte>): Unit
```

Function: Writes the specified byte array `buffer` to standard output or standard error stream.

Parameters:

- buffer: [Array](../../core/core_package_api/core_package_structs.md#struct-arrayt)\<[Byte](../../core/core_package_api/core_package_types.md#type-byte)> - The byte array to be written.

### func write(Bool)

```cangjie
public func write(v: Bool): Unit
```

Function: Writes the text representation of the specified boolean value to standard output or standard error stream.

Parameters:

- v: [Bool](../../core/core_package_api/core_package_intrinsics.md#bool) - The value to write.

### func write(Float16)

```cangjie
public func write(v: Float16): Unit
```

Function: Writes the text representation of the specified 16-bit floating-point value to standard output or standard error stream.

Parameters:- v: [Float16](../../core/core_package_api/core_package_intrinsics.md#float16) - The value to be written.

### func write(Float32)

```cangjie
public func write(v: Float32): Unit
```

Function: Writes the text representation of the specified 32-bit floating-point value to the standard output or standard error stream.

Parameters:

- v: [Float32](../../core/core_package_api/core_package_intrinsics.md#float32) - The value to be written.

### func write(Float64)

```cangjie
public func write(v: Float64): Unit
```

Function: Writes the text representation of the specified 64-bit floating-point value to the standard output or standard error stream.

Parameters:

- v: [Float64](../../core/core_package_api/core_package_intrinsics.md#float64) - The value to be written.

### func write(Int16)

```cangjie
public func write(v: Int16): Unit
```

Function: Writes the text representation of the specified 16-bit signed integer value to the standard output or standard error stream.

Parameters:

- v: [Int16](../../core/core_package_api/core_package_intrinsics.md#int16) - The value to be written.

### func write(Int32)

```cangjie
public func write(v: Int32): Unit
```

Function: Writes the text representation of the specified 32-bit signed integer value to the standard output or standard error stream.

Parameters:

- v: [Int32](../../core/core_package_api/core_package_intrinsics.md#int32) - The value to be written.

### func write(Int64)

```cangjie
public func write(v: Int64): Unit
```

Function: Writes the text representation of the specified 64-bit signed integer value to the standard output or standard error stream.

Parameters:

- v: [Int64](../../core/core_package_api/core_package_intrinsics.md#int64) - The value to be written.

### func write(Int8)

```cangjie
public func write(v: Int8): Unit
```

Function: Writes the text representation of the specified 8-bit signed integer value to the standard output or standard error stream.

Parameters:

- v: [Int8](../../core/core_package_api/core_package_intrinsics.md#int8) - The value to be written.

### func write(Rune)

```cangjie
public func write(v: Rune): Unit
```

Function: Writes the Unicode character value of the specified [Rune](../../../std_en/core/core_package_api/core_package_intrinsics.md#rune) to the standard output or standard error stream.

Parameters:

- v: [Rune](../../../std_en/core/core_package_api/core_package_intrinsics.md#rune) - The value to be written.

### func write(String)

```cangjie
public func write(v: String): Unit
```

Function: Writes the specified string value to the standard output or standard error stream.

Parameters:

- v: [String](../../core/core_package_api/core_package_structs.md#struct-string) - The value to be written.

### func write(UInt16)

```cangjie
public func write(v: UInt16): Unit
```

Function: Writes the text representation of the specified 16-bit unsigned integer value to the standard output or standard error stream.

Parameters:

- v: [UInt16](../../core/core_package_api/core_package_intrinsics.md#uint16) - The value to be written.

### func write(UInt32)

```cangjie
public func write(v: UInt32): Unit
```

Function: Writes the text representation of the specified 32-bit unsigned integer value to the standard output or standard error stream.

Parameters:

- v: [UInt32](../../core/core_package_api/core_package_intrinsics.md#uint32) - The value to be written.

### func write(UInt64)

```cangjie
public func write(v: UInt64): Unit
```

Function: Writes the text representation of the specified 64-bit unsigned integer value to the standard output or standard error stream.

Parameters:

- v: [UInt64](../../core/core_package_api/core_package_intrinsics.md#uint64) - The value to be written.

### func write(UInt8)

```cangjie
public func write(v: UInt8): Unit
```

Function: Writes the text representation of the specified 8-bit unsigned integer value to the standard output or standard error stream.

Parameters:

- v: [UInt8](../../core/core_package_api/core_package_intrinsics.md#uint8) - The value to be written.

### func write\<T>(T) where T <: ToString

```cangjie
public func write<T>(v: T): Unit where T <: ToString
```

Function: Writes a data type that implements the [ToString](../../core/core_package_api/core_package_interfaces.md#interface-tostring) interface to the standard output or standard error stream.

Parameters:

- v: T - An instance of the [ToString](../../core/core_package_api/core_package_interfaces.md#interface-tostring) type to be written.

### func writeln(Array\<Byte>)

```cangjie
public func writeln(buffer: Array<Byte>): Unit
```

Function: Writes the specified byte array buffer (followed by a newline) to the standard output or standard error stream.

Parameters:

- buffer: [Array](../../core/core_package_api/core_package_structs.md#struct-arrayt)\<[Byte](../../core/core_package_api/core_package_types.md#type-byte)> - The value to be written.

### func writeln(Bool)

```cangjie
public func writeln(v: Bool): Unit
```

Function: Writes the text representation of the specified boolean value (followed by a newline) to the standard output or standard error stream.

Parameters:

- v: [Bool](../../core/core_package_api/core_package_intrinsics.md#bool) - The value to be written.

### func writeln(Float16)

```cangjie
public func writeln(v: Float16): Unit
```

Function: Writes the text representation of the specified 16-bit floating-point value (followed by a newline) to the standard output or standard error stream.

Parameters:

- v: [Float16](../../core/core_package_api/core_package_intrinsics.md#float16) - The value to be written.

### func writeln(Float32)

```cangjie
public func writeln(v: Float32): Unit
```

Function: Writes the text representation of the specified 32-bit floating-point value (followed by a newline) to the standard output or standard error stream.

Parameters:

- v: [Float32](../../core/core_package_api/core_package_intrinsics.md#float32) - The value to be written.

### func writeln(Float64)

```cangjie
public func writeln(v: Float64): Unit
```

Function: Writes the text representation of the specified 64-bit floating-point value (followed by a newline) to the standard output or standard error stream.

Parameters:

- v: [Float64](../../core/core_package_api/core_package_intrinsics.md#float64) - The value to be written.

### func writeln(Int16)

```cangjie
public func writeln(v: Int16): Unit
```

Function: Writes the text representation of the specified 16-bit signed integer value (followed by a newline) to the standard output or standard error stream.

Parameters:

- v: [Int16](../../core/core_package_api/core_package_intrinsics.md#int16) - The value to be written.

### func writeln(Int32)

```cangjie
public func writeln(v: Int32): Unit
```

Function: Writes the text representation of the specified 32-bit signed integer value (followed by a newline) to the standard output or standard error stream.

Parameters:

- v: [Int32](../../core/core_package_api/core_package_intrinsics.md#int32) - The value to be written.

### func writeln(Int64)

```cangjie
public func writeln(v: Int64): Unit
```

Function: Writes the text representation of the specified 64-bit signed integer value (followed by a newline) to the standard output or standard error stream.

Parameters:

- v: [Int64](../../core/core_package_api/core_package_intrinsics.md#int64) - The value to be written.

### func writeln(Int8)

```cangjie
public func writeln(v: Int8): Unit
```

Function: Writes the text representation of the specified 8-bit signed integer value (followed by a newline) to the standard output or standard error stream.

Parameters:

- v: [Int8](../../core/core_package_api/core_package_intrinsics.md#int8) - The value to be written.

### func writeln(Rune)

```cangjie
public func writeln(v: Rune): Unit
```

Function: Writes the specified Unicode character value (followed by a newline) to the standard output or standard error stream.

Parameters:

- v: Rune - The value to be written.

### func writeln(String)

```cangjie
public func writeln(v: String): Unit
```

Function: Writes the specified string value (followed by a newline) to the standard output or standard error stream.

Parameters:

- v: [String](../../core/core_package_api/core_package_structs.md#struct-string) - The value to be written.

### func writeln(UInt16)

```cangjie
public func writeln(v: UInt16): Unit
```

Function: Writes the text representation of the specified 16-bit unsigned integer value (followed by a newline) to the standard output or standard error stream.

Parameters:- v: [UInt16](../../core/core_package_api/core_package_intrinsics.md#uint16) - The value to be written.

### func writeln(UInt32)

```cangjie
public func writeln(v: UInt32): Unit
```

Function: Writes the text representation of the specified 32-bit unsigned integer value (followed by a newline) to the standard output or standard error stream.

Parameters:

- v: [UInt32](../../core/core_package_api/core_package_intrinsics.md#uint32) - The value to be written.

### func writeln(UInt64)

```cangjie
public func writeln(v: UInt64): Unit
```

Function: Writes the text representation of the specified 64-bit unsigned integer value (followed by a newline) to the standard output or standard error stream.

Parameters:

- v: [UInt64](../../core/core_package_api/core_package_intrinsics.md#uint64) - The value to be written.

### func writeln(UInt8)

```cangjie
public func writeln(v: UInt8): Unit
```

Function: Writes the text representation of the specified 8-bit unsigned integer value (followed by a newline) to the standard output or standard error stream.

Parameters:

- v: [UInt8](../../core/core_package_api/core_package_intrinsics.md#uint8) - The value to be written.

### func writeln\<T>(T) where T <: ToString

```cangjie
public func writeln<T>(v: T): Unit where T <: ToString
```

Function: Writes the string converted from a data type implementing the [ToString](../../core/core_package_api/core_package_interfaces.md#interface-tostring) interface (followed by a newline) to the standard output or standard error stream.

Parameters:

- v: T - The value to be written.
