# Extensions

## extend Bool <: Equatable\<Bool>

```cangjie
extend Bool <: Equatable<Bool>
```

Function: Extends the [Bool](core_package_intrinsics.md#bool) type with the [Equatable](core_package_interfaces.md#interface-equatablet)\<[Bool](core_package_intrinsics.md#bool)> interface to support equality comparison.

Extended type:

- [Bool](core_package_intrinsics.md#bool)

Extended interfaces:

- Equatable\<Bool>

## extend Bool <: Hashable

```cangjie
extend Bool <: Hashable
```

Function: Extends the [Bool](core_package_intrinsics.md#bool) type with the [Hashable](core_package_interfaces.md#interface-hashable) interface to support hash value computation.

Extended type:

- [Bool](core_package_intrinsics.md#bool)

Extended interfaces:

- Hashable

### func hashCode()

```cangjie
public func hashCode(): Int64
```

Function: Gets the hash value.

Returns:

- [Int64](core_package_intrinsics.md#int64) - The hash value.

Example:

<!-- verify -->
```cangjie
main() {
    var flag: Bool = false
    println(flag.hashCode())
    flag = true
    println(flag.hashCode())
}
```

Execution result:

```text
0
1
```

## extend Bool <: ToString

```cangjie
extend Bool <: ToString
```

Function: Extends the [Bool](core_package_intrinsics.md#bool) type with the [ToString](core_package_interfaces.md#interface-tostring) interface to support conversion to [String](core_package_structs.md#struct-string) type.

Extended type:

- [Bool](core_package_intrinsics.md#bool)

Extended interfaces:

- ToString

### func toString()

```cangjie
public func toString(): String
```

Function: Converts a [Bool](core_package_intrinsics.md#bool) value to an output-ready string.

Returns:

- [String](core_package_structs.md#struct-string) - The converted string.

Example:

<!-- verify -->
```cangjie
main() {
    var flag: Bool = false
    let str1: String = flag.toString()
    println(str1)
    flag = true
    let str2: String = flag.toString()
    println(str2)
}
```

Execution result:

```text
false
true
```

## extend Byte

```cangjie
extend Byte
```

Function: Implements a series of extension methods for the [Byte](core_package_types.md#type-byte) type, primarily providing character judgment and conversion operations within the ASCII character set range.

Extended type:

- [Byte](core_package_types.md#type-byte)

### func isAscii()

```cangjie
public func isAscii(): Bool
```

Function: Determines whether the [Byte](core_package_types.md#type-byte) is within the ASCII range.

Returns:

- [Bool](core_package_intrinsics.md#bool) - Returns true if the [Byte](core_package_types.md#type-byte) is within the ASCII range, otherwise returns false.

Example:

<!-- verify -->
```cangjie
main() {
    // Test isAscii() method
    let byte1: Byte = 65  // ASCII value of 'A'
    let byte2: Byte = 200 // Value outside ASCII range
    
    println("Is byte1 (${byte1}) within ASCII range: ${byte1.isAscii()}")
    println("Is byte2 (${byte2}) within ASCII range: ${byte2.isAscii()}")
}
```

Output:

```text
byte1 (65) is within ASCII range: true
byte2 (200) is within ASCII range: false
```

### func isAsciiControl()

```cangjie
public func isAsciiControl(): Bool
```

Function: Determines whether the [Byte](core_package_types.md#type-byte) is within the ASCII control character range. The valid range is the union of [00, 1F] and {7F}.

Returns:

- [Bool](core_package_intrinsics.md#bool) - Returns true if the [Byte](core_package_types.md#type-byte) is within the ASCII control character range, otherwise returns false.

Example:

<!-- verify -->
```cangjie
main() {
    // Test isAsciiControl() method
    let byte1: Byte = 0x09  // Tab '\t', an ASCII control character
    let byte2: Byte = 0x20  // Space ' ', not an ASCII control character
    let byte3: Byte = 0x7F  // DEL, an ASCII control character
    
    println("Is byte1 (0x${byte1}) an ASCII control character: ${byte1.isAsciiControl()}")
    println("Is byte2 (0x${byte2}) an ASCII control character: ${byte2.isAsciiControl()}")
    println("Is byte3 (0x${byte3}) an ASCII control character: ${byte3.isAsciiControl()}")
}
```

Output:

```text
byte1 (0x9) is an ASCII control character: true
byte2 (0x32) is an ASCII control character: false
byte3 (0x127) is an ASCII control character: true
```

### func isAsciiGraphic()

```cangjie
public func isAsciiGraphic(): Bool
```

Function: Determines whether the [Byte](core_package_types.md#type-byte) is within the ASCII graphic character range. The valid range is [21, 7E].

Returns:

- [Bool](core_package_intrinsics.md#bool) - Returns true if the [Byte](core_package_types.md#type-byte) is within the ASCII graphic character range, otherwise returns false.

Example:

<!-- verify -->
```cangjie
main() {
    // Test isAsciiGraphic() method
    let byte1: Byte = 0x21  // '!', an ASCII graphic character
    let byte2: Byte = 0x7E  // '~', an ASCII graphic character
    let byte3: Byte = 0x20  // ' ', space, not an ASCII graphic character
    
    println("Is byte1 (0x${byte1}) an ASCII graphic character: ${byte1.isAsciiGraphic()}")
    println("Is byte2 (0x${byte2}) an ASCII graphic character: ${byte2.isAsciiGraphic()}")
    println("Is byte3 (0x${byte3}) an ASCII graphic character: ${byte3.isAsciiGraphic()}")
}
```

Output:

```text
byte1 (0x33) is an ASCII graphic character: true
byte2 (0x126) is an ASCII graphic character: true
byte3 (0x32) is an ASCII graphic character: false
```

### func isAsciiHex()

```cangjie
public func isAsciiHex(): Bool
```

Function: Determines whether the [Byte](core_package_types.md#type-byte) is within the ASCII hexadecimal digit range.

Returns:

- [Bool](core_package_intrinsics.md#bool) - Returns true if the [Byte](core_package_types.md#type-byte) is within the ASCII hexadecimal digit range, otherwise returns false.

Example:

<!-- verify -->
```cangjie
main() {
    // Test isAsciiHex() method
    let byte1: Byte = 0x30  // '0', an ASCII hexadecimal digit
    let byte2: Byte = 0x46  // 'F', an ASCII hexadecimal digit
    let byte3: Byte = 0x67  // 'g', not an ASCII hexadecimal digit
    
    println("Is byte1 (0x${byte1}) an ASCII hexadecimal digit: ${byte1.isAsciiHex()}")
    println("Is byte2 (0x${byte2}) an ASCII hexadecimal digit: ${byte2.isAsciiHex()}")
    println("Is byte3 (0x${byte3}) an ASCII hexadecimal digit: ${byte3.isAsciiHex()}")
}
```

Output:

```text
byte1 (0x48) is an ASCII hexadecimal digit: true
byte2 (0x70) is an ASCII hexadecimal digit: true
byte3 (0x103) is an ASCII hexadecimal digit: false
```

### func isAsciiLetter()

```cangjie
public func isAsciiLetter(): Bool
```

Function: Determines whether the [Byte](core_package_types.md#type-byte) is within the ASCII Latin letter range.

Returns:

- [Bool](core_package_intrinsics.md#bool) - Returns true if the [Byte](core_package_types.md#type-byte) is within the ASCII Latin letter range, otherwise returns false.

Example:

<!-- verify -->
```cangjie
main() {
    // Test isAsciiLetter() method
    let byte1: Byte = 0x41  // 'A', an ASCII Latin letter
    let byte2: Byte = 0x7A  // 'z', an ASCII Latin letter
    let byte3: Byte = 0x30  // '0', digit, not an ASCII Latin letter
    
    println("Is byte1 (0x${byte1}) an ASCII Latin letter: ${byte1.isAsciiLetter()}")
    println("Is byte2 (0x${byte2}) an ASCII Latin letter: ${byte2.isAsciiLetter()}")
    println("Is byte3 (0x${byte3}) an ASCII Latin letter: ${byte3.isAsciiLetter()}")
}
```

Output:

```text
byte1 (0x65) is an ASCII Latin letter: true
byte2 (0x122) is an ASCII Latin letter: true
byte3 (0x48) is an ASCII Latin letter: false
```

### func isAsciiLowerCase()

```cangjie
public func isAsciiLowerCase(): Bool
```

Function: Determines whether the [Byte](core_package_types.md#type-byte) is within the ASCII lowercase Latin letter range.

Returns:

- [Bool](core_package_intrinsics.md#bool) - Returns true if the [Byte](core_package_types.md#type-byte) is within the ASCII lowercase Latin letter range, otherwise returns false.

Example:

<!-- verify -->
```cangjie
main() {
    // Test isAsciiLowerCase() method
    let byte1: Byte = 0x61  // 'a', an ASCII lowercase Latin letter
    let byte2: Byte = 0x7A  // 'z', an ASCII lowercase Latin letter
    let byte3: Byte = 0x41  // 'A', uppercase letter, not an ASCII lowercase Latin letter
    
    println("Is byte1 (0x${byte1}) an ASCII lowercase Latin letter: ${byte1.isAsciiLowerCase()}")
    println("Is byte2 (0x${byte2}) an ASCII lowercase Latin letter: ${byte2.isAsciiLowerCase()}")
    println("Is byte3 (0x${byte3}) an ASCII lowercase Latin letter: ${byte3.isAsciiLowerCase()}")
}
```

Output:

```text
byte1 (0x97) is an ASCII lowercase Latin letter: true
byte2 (0x122) is an ASCII lowercase Latin letter: true
byte3 (0x65) is an ASCII lowercase Latin letter: false
```

### func isAsciiNumber()

```cangjie
public func isAsciiNumber(): Bool
```

Function: Determines whether the [Byte](core_package_types.md#type-byte) is within the ASCII decimal digit range.

Returns:

- [Bool](core_package_intrinsics.md#bool) - Returns true if the [Byte](core_package_types.md#type-byte) is within the ASCII decimal digit range, otherwise returns false.

Example:

<!-- verify -->
```cangjie
main() {
    // Test isAsciiNumber() method
    let byte1: Byte = 0x30  // '0', an ASCII decimal digit
    let byte2: Byte = 0x39  // '9', an ASCII decimal digit
    let byte3: Byte = 0x41  // 'A', letter, not an ASCII decimal digit
    
    println("Is byte1 (0x${byte1}) an ASCII decimal digit: ${byte1.isAsciiNumber()}")
    println("Is byte2 (0x${byte2}) an ASCII decimal digit: ${byte2.isAsciiNumber()}")
    println("Is byte3 (0x${byte3}) an ASCII decimal digit: ${byte3.isAsciiNumber()}")
}
```

Output:

```text
byte1 (0x48) is an ASCII decimal digit: true
byte2 (0x57) is an ASCII decimal digit: true
byte3 (0x65) is an ASCII decimal digit: false
```

### func isAsciiNumberOrLetter()

```cangjie
public func isAsciiNumberOrLetter(): Bool
```

Function: Determines whether the [Byte](core_package_types.md#type-byte) is within the ASCII decimal digit or Latin letter range.

Returns:

- [Bool](core_package_intrinsics.md#bool) - Returns true if the [Byte](core_package_types.md#type-byte) is within the ASCII decimal digit or Latin letter range, otherwise returns false.

Example:

<!-- verify -->
```cangjie
main() {
    // Test isAsciiNumberOrLetter() method
    let byte1: Byte = 0x30  // '0', digit, within ASCII decimal digit and Latin letter range
    let byte2: Byte = 0x41  // 'A', letter, within ASCII decimal digit and Latin letter range
    let byte3: Byte = 0x20  // ' ', space, not within ASCII decimal digit and Latin letter range
    
    println("Is byte1 (0x${byte1}) an ASCII decimal digit or Latin letter: ${byte1.isAsciiNumberOrLetter()}")
    println("Is byte2 (0x${byte2}) an ASCII decimal digit or Latin letter: ${byte2.isAsciiNumberOrLetter()}")
    println("Is byte3 (0x${byte3}) an ASCII decimal digit or Latin letter: ${byte3.isAsciiNumberOrLetter()}")
}
```

Output:

```text
byte1 (0x48) is an ASCII decimal digit or Latin letter: true
byte2 (0x65) is an ASCII decimal digit or Latin letter: true
byte3 (0x32) is an ASCII decimal digit or Latin letter: false
```

### func isAsciiOct()

```cangjie
public func isAsciiOct(): Bool
```

Function: Determines whether the [Byte](core_package_types.md#type-byte) is within the ASCII octal digit range.

Returns:

- [Bool](core_package_intrinsics.md#bool) - Returns true if the [Byte](core_package_types.md#type-byte) is within the ASCII octal digit range, otherwise returns false.

Example:

<!-- verify -->
```cangjie
main() {
    // Test isAsciiOct() method
    let byte1: Byte = 0x30  // '0', an ASCII octal digit
    let byte2: Byte = 0x37  // '7', an ASCII octal digit
    let byte3: Byte = 0x38  // '8', not an ASCII octal digit
    
    println("Is byte1 (0x${byte1}) an ASCII octal digit: ${byte1.isAsciiOct()}")
    println("Is byte2 (0x${byte2}) an ASCII octal digit: ${byte2.isAsciiOct()}")
    println("Is byte3 (0x${byte3}) an ASCII octal digit: ${byte3.isAsciiOct()}")
}
```

Output:

```text
byte1 (0x48) is an ASCII octal digit: true
byte2 (0x55) is an ASCII octal digit: true
byte3 (0x56) is an ASCII octal digit: false
```

### func isAsciiPunctuation()

```cangjie
public func isAsciiPunctuation(): Bool
```

Function: Determines whether the [Byte](core_package_types.md#type-byte) is within the ASCII punctuation range. The valid range is the union of [21, 2F], [3A, 40], [5B, 60], and [7B, 7E].

Returns:

- [Bool](core_package_intrinsics.md#bool) - Returns true if the [Byte](core_package_types.md#type-byte) is within the ASCII punctuation range, otherwise returns false.

Example:

<!-- verify -->
```cangjie
main() {
    // Test isAsciiPunctuation() method
    let byte1: Byte = 0x21  // '!', an ASCII punctuation
    let byte2: Byte = 0x2C  // ',', an ASCII punctuation
    let byte3: Byte = 0x30  // '0', digit, not an ASCII punctuation
    
    println("Is byte1 (0x${byte1}) an ASCII punctuation: ${byte1.isAsciiPunctuation()}")
    println("Is byte2 (0x${byte2}) an ASCII punctuation: ${byte2.isAsciiPunctuation()}")
    println("Is byte3 (0x${byte3}) an ASCII punctuation: ${byte3.isAsciiPunctuation()}")
}
```

Output:

```text
byte1 (0x33) is an ASCII punctuation: true
byte2 (0x44) is an ASCII punctuation: true
byte3 (0x48) is an ASCII punctuation: false
```

### func isAsciiUpperCase()

```cangjie
public func isAsciiUpperCase(): Bool
```

Function: Determines whether the [Byte](core_package_types.md#type-byte) is within the ASCII uppercase Latin letter range.

Returns:

- [Bool](core_package_intrinsics.md#bool) - Returns true if the [Byte](core_package_types.md#type-byte) is within the ASCII uppercase Latin letter range, otherwise returns false.

Example:

<!-- verify -->
```cangjie
main() {
    // Test isAsciiUpperCase() method
    let byte1: Byte = 0x41  // 'A', an ASCII uppercase Latin letter
    let byte2: Byte = 0x5A  // 'Z', an ASCII uppercase Latin letter
    let byte3: Byte = 0x61  // 'a', lowercase letter, not an ASCII uppercase Latin letter
    
    println("Is byte1 (0x${byte1}) an ASCII uppercase Latin letter: ${byte1.isAsciiUpperCase()}")
    println("Is byte2 (0x${byte2}) an ASCII uppercase Latin letter: ${byte2.isAsciiUpperCase()}")
    println("Is byte3 (0x${byte3}) an ASCII uppercase Latin letter: ${byte3.isAsciiUpperCase()}")
}
```

Output:

```text
byte1 (0x65) is an ASCII uppercase Latin letter: true
byte2 (0x90) is an ASCII uppercase Latin letter: true
byte3 (0x97) is an ASCII uppercase Latin letter: false
```

### func isAsciiWhiteSpace()

```cangjie
public func isAsciiWhiteSpace(): Bool
```

Function: Determines whether the [Byte](core_package_types.md#type-byte) is within the ASCII whitespace character range. The valid range is the union of [09, 0D] and {20}.

Returns:

- [Bool](core_package_intrinsics.md#bool) - Returns true if the [Byte](core_package_types.md#type-byte) is within the ASCII whitespace character range, otherwise returns false.

Example:

<!-- verify -->
```cangjie
main() {
    // Test isAsciiWhiteSpace() method
    let byte1: Byte = 0x09  // '\t', tab, an ASCII whitespace character
    let byte2: Byte = 0x20  // ' ', space, an ASCII whitespace character
    let byte3: Byte = 0x41  // 'A', letter, not an ASCII whitespace character
    
    println("Is byte1 (0x${byte1}) an ASCII whitespace character: ${byte1.isAsciiWhiteSpace()}")
    println("Is byte2 (0x${byte2}) an ASCII whitespace character: ${byte2.isAsciiWhiteSpace()}")
    println("Is byte3 (0x${byte3}) an ASCII whitespace character: ${byte3.isAsciiWhiteSpace()}")
}
```

Output:

```text
byte1 (0x9) is an ASCII whitespace character: true
byte2 (0x32) is an ASCII whitespace character: true
byte3 (0x65) is an ASCII whitespace character: false
```

### func toAsciiLowerCase()

```cangjie
public func toAsciiLowerCase(): Byte
```

Function: Converts the [Byte](core_package_types.md#type-byte) to its corresponding ASCII lowercase character [Byte](core_package_types.md#type-byte). If conversion is not## interface CType

Returns:

- [Byte](core_package_types.md#type-byte) - The converted [Byte](core_package_types.md#type-byte), or the original [Byte](core_package_types.md#type-byte) if conversion is not possible.

Example:

<!-- verify -->
```cangjie
main() {
    // Test the toAsciiLowerCase() method
    let byte1: Byte = 0x41  // 'A', uppercase letter
    let byte2: Byte = 0x5A  // 'Z', uppercase letter
    let byte3: Byte = 0x30  // '0', digit, cannot be converted
    
    let lower1 = byte1.toAsciiLowerCase()
    let lower2 = byte2.toAsciiLowerCase()
    let lower3 = byte3.toAsciiLowerCase()
    
    println("byte1 (0x${byte1}) converted to lowercase: 0x${lower1}")
    println("byte2 (0x${byte2}) converted to lowercase: 0x${lower2}")
    println("byte3 (0x${byte3}) converted to lowercase: 0x${lower3} (unchanged)")
}
```

Execution Result:

```text
byte1 (0x65) converted to lowercase: 0x97
byte2 (0x90) converted to lowercase: 0x122
byte3 (0x48) converted to lowercase: 0x48 (unchanged)
```

### func toAsciiUpperCase()

```cangjie
public func toAsciiUpperCase(): Byte
```

Function: Converts the [Byte](core_package_types.md#type-byte) to its corresponding ASCII uppercase character [Byte](core_package_types.md#type-byte), or leaves it unchanged if conversion is not possible.

Returns:

- [Byte](core_package_types.md#type-byte) - The converted [Byte](core_package_types.md#type-byte), or the original [Byte](core_package_types.md#type-byte) if conversion is not possible.

Example:

<!-- verify -->
```cangjie
main() {
    // Test the toAsciiUpperCase() method
    let byte1: Byte = 0x61  // 'a', lowercase letter
    let byte2: Byte = 0x7A  // 'z', lowercase letter
    let byte3: Byte = 0x30  // '0', digit, cannot be converted
    
    let upper1 = byte1.toAsciiUpperCase()
    let upper2 = byte2.toAsciiUpperCase()
    let upper3 = byte3.toAsciiUpperCase()
    
    println("byte1 (0x${byte1}) converted to uppercase: 0x${upper1}")
    println("byte2 (0x${byte2}) converted to uppercase: 0x${upper2}")
    println("byte3 (0x${byte3}) converted to uppercase: 0x${upper3} (unchanged)")
}
```

Execution Result:

```text
byte1 (0x97) converted to uppercase: 0x65
byte2 (0x122) converted to uppercase: 0x90
byte3 (0x48) converted to uppercase: 0x48 (unchanged)
```

## extend CString <: ToString

```cangjie
extend CString <: ToString
```

Function: Extends common methods for string pointers to the [CString](core_package_intrinsics.md#cstring) type, including null checks, length retrieval, equality checks, substring extraction, etc.

Extended type:

- [CString](core_package_intrinsics.md#cstring)

Extended interfaces:

- ToString

### func asResource()

```cangjie
public func asResource(): CStringResource
```

Function: Obtains the [CStringResource](core_package_structs.md#struct-cstringresource) instance corresponding to the current [CString](core_package_intrinsics.md#cstring) instance.

[CStringResource](core_package_structs.md#struct-cstringresource) implements the [Resource](core_package_interfaces.md#interface-resource) interface, enabling automatic resource release in `try-with-resource` syntax contexts.

Returns:

- [CStringResource](core_package_structs.md#struct-cstringresource) - The corresponding [CStringResource](core_package_structs.md#struct-cstringresource) C-string resource type instance.

Example:

<!-- verify -->
```cangjie
foreign func malloc(size: UIntNative): CPointer<Unit>

main() {
    var str: CString = unsafe { LibC.mallocCString("hello") }
    var ptrResource: CStringResource = str.asResource()
    try (r = ptrResource) {
        var p = r.value
        println(p.size())
    }
    println(ptrResource.isClosed())
}
```

Execution Result:

```text
5
true
```

### func compare(CString)

```cangjie
public func compare(str: CString): Int32
```

Function: Compares two strings lexicographically, equivalent to C's `strcmp`.

Parameters:

- str: [CString](core_package_intrinsics.md#cstring) - The target string to compare.

Returns:

- [Int32](core_package_intrinsics.md#int32) - Returns 0 if equal, -1 if the current string is lexicographically smaller than str, otherwise 1.

Exceptions:

- [IllegalMemoryException](core_package_exceptions.md#class-illegalmemoryexception) - Thrown if either [CString](core_package_intrinsics.md#cstring) being compared is a null pointer.

Example:

<!-- verify -->
```cangjie
main() {
    var str1: CString = unsafe { LibC.mallocCString("hello") }
    var str2: CString = unsafe { LibC.mallocCString("hello") }
    println(str1.compare(str2))

    var str3: CString = unsafe { LibC.mallocCString("hello") }
    var str4: CString = unsafe { LibC.mallocCString("hellow") }
    println(str3.compare(str4))

    var str5: CString = unsafe { LibC.mallocCString("hello") }
    var str6: CString = unsafe { LibC.mallocCString("allow") }
    println(str5.compare(str6))

    unsafe {
        LibC.free(str1)
        LibC.free(str2)
        LibC.free(str3)
        LibC.free(str4)
        LibC.free(str5)
        LibC.free(str6)
    }
}
```

Execution Result:

```text
0
-1
1
```

### func endsWith(CString)

```cangjie
public func endsWith(suffix: CString): Bool
```

Function: Checks if the string ends with the specified suffix.

Parameters:

- suffix: [CString](core_package_intrinsics.md#cstring) - The target suffix string to match.

Returns:

- [Bool](core_package_intrinsics.md#bool) - Returns true if the string ends with suffix, false otherwise. Returns false if either string pointer is null.

Example:

<!-- verify -->
```cangjie
main() {
    var str1: CString = unsafe { LibC.mallocCString("hello") }
    var str2: CString = unsafe { LibC.mallocCString("lo") }
    var str3: CString = unsafe { LibC.mallocCString("ao") }

    println(str1.endsWith(str2))
    println(str1.endsWith(str3))

    unsafe {
        LibC.free(str1)
        LibC.free(str2)
        LibC.free(str3)
    }
}
```

Execution Result:

```text
true
false
```

### func equals(CString)

```cangjie
public func equals(rhs: CString): Bool
```

Function: Checks if two strings are equal.

Parameters:

- rhs: [CString](core_package_intrinsics.md#cstring) - The target string to compare.

Returns:

- [Bool](core_package_intrinsics.md#bool) - Returns true if the strings are equal, false otherwise.

Example:

<!-- verify -->
```cangjie
main() {
    var str1: CString = unsafe { LibC.mallocCString("hello") }
    var str2: CString = unsafe { LibC.mallocCString("hello") }
    var str3: CString = unsafe { LibC.mallocCString("Hello") }
    println(str1.equals(str2))
    println(str1.equals(str3))

    unsafe {
        LibC.free(str1)
        LibC.free(str2)
        LibC.free(str3)
    }
}
```

Execution Result:

```text
true
false
```

### func equalsLower(CString)

```cangjie
public func equalsLower(rhs: CString): Bool
```

Function: Checks if two strings are equal, ignoring case.

Parameters:

- rhs: [CString](core_package_intrinsics.md#cstring) - The target string to match.

Returns:

- [Bool](core_package_intrinsics.md#bool) - Returns true if the strings are equal ignoring case, false otherwise.

Example:

<!-- verify -->
```cangjie
main() {
    var str1: CString = unsafe { LibC.mallocCString("hello") }
    var str2: CString = unsafe { LibC.mallocCString("HELLO") }
    var str3: CString = unsafe { LibC.mallocCString("Hello") }
    println(str1.equalsLower(str2))
    println(str1.equalsLower(str3))

    unsafe {
        LibC.free(str1)
        LibC.free(str2)
        LibC.free(str3)
    }
}
```

Execution Result:

```text
true
true
```

### func getChars()

```cangjie
public func getChars(): CPointer<UInt8>
```

Function: Retrieves the pointer to the string.

Returns:

- [CPointer](./core_package_intrinsics.md#cpointert)\<[UInt8](./core_package_intrinsics.md#uint8)> - The pointer to the string.

Example:

<!-- verify -->
```cangjie
main() {
    var str1: CString = unsafe { LibC.mallocCString("hello") }
    var ptr: CPointer<UInt8> = unsafe { str1.getChars() }
    var c: UInt8 = unsafe { ptr.read() }
    println(c) // ASCII code of 'h' is 104
    unsafe {
        LibC.free(str1)
    }
}
```

Execution Result:

```text
104
```

### func isEmpty()

```cangjie
public func isEmpty(): Bool
```

Function: Checks if the string is empty.

Returns:

- [Bool](core_package_intrinsics.md#bool) - Returns true if the string is empty or the pointer is null, false otherwise.

Example:

<!-- verify -->
```cangjie
main() {
    var str1: CString = unsafe { LibC.mallocCString("hello") }
    println(str1.isEmpty())

    unsafe {
        LibC.free(str1)
    }
}
```

Execution Result:

```text
false
```

### func isNotEmpty()

```cangjie
public func isNotEmpty(): Bool
```

Function: Checks if the string is not empty.

Returns:

- [Bool](core_package_intrinsics.md#bool) - Returns true if the string is not empty, false if the pointer is null.

Example:

<!-- verify -->
```cangjie
main() {
    var str1: CString = unsafe { LibC.mallocCString("hello") }
    println(str1.isNotEmpty())

    unsafe {
        LibC.free(str1)
    }
}
```

Execution Result:

```text
true
```

### func isNull()

```cangjie
public func isNull(): Bool
```

Function: Checks if the string pointer is null.

Returns:

- [Bool](core_package_intrinsics.md#bool) - Returns true if the pointer is null, false otherwise.

Example:

<!-- verify -->
```cangjie
main() {
    var str1: CString = unsafe { LibC.mallocCString("hello") }
    println(str1.isNull())

    unsafe {
        LibC.free(str1)
    }
}
```

Execution Result:

```text
false
```

### func size()

```cangjie
public func size(): Int64
```

Function: Returns the length of the string, equivalent to C's `strlen`.

Returns:

- [Int64](core_package_intrinsics.md#int64) - The length of the string.

Example:

<!-- verify -->
```cangjie
main() {
    var str1: CString = unsafe { LibC.mallocCString("hello") }
    println(str1.size())

    unsafe {
        LibC.free(str1)
    }
}
```

Execution Result:

```text
5
```

### func startsWith(CString)

```cangjie
public func startsWith(prefix: CString): Bool
```

Function: Checks if the string starts with the specified prefix.

Parameters:

- prefix: [CString](core_package_intrinsics.md#cstring) - The target prefix string to match.

Returns:

- [Bool](core_package_intrinsics.md#bool) - Returns true if the string starts with prefix, false otherwise. Returns false if either string pointer is null.

Example:

<!-- verify -->
```cangjie
main() {
    var str1: CString = unsafe { LibC.mallocCString("hello") }
    var str2: CString = unsafe { LibC.mallocCString("he") }
    println(str1.startsWith(str2))

    unsafe {
        LibC.free(str1)
        LibC.free(str2)
    }
}
```

Execution Result:

```text
true
```

### func subCString(UIntNative)

```cangjie
public func subCString(beginIndex: UIntNative): CString
```

Function: Extracts a substring starting from the specified position to the end of the string.

> **Note:**
>
> 1. This method returns a copy of the substring, which must be manually freed after use.
> 2. Returns a null pointer if beginIndex equals the string length.

Parameters:

- beginIndex: [UIntNative](core_package_intrinsics.md#uintnative) - The starting position for extraction, range [0, this.size()].

Returns:

- [CString](core_package_intrinsics.md#cstring) - The extracted substring.

Exceptions:

- [IndexOutOfBoundsException](core_package_exceptions.md#class-indexoutofboundsexception) - Thrown if beginIndex exceeds the string length.
- [IllegalMemoryException](core_package_exceptions.md#class-illegalmemoryexception) - Thrown if memory allocation or copying fails.

Example:

<!-- verify -->
```cangjie
main() {
    let index: UIntNative = 2
    var str1: CString = unsafe { LibC.mallocCString("hello") }
    var str2: CString = str1.subCString(index)
    println(str2)

    unsafe {
        LibC.free(str1)
        LibC.free(str2)
    }
}
```

Execution Result:

```text
llo
```

### func subCString(UIntNative, UIntNative)

```cangjie
public func subCString(beginIndex: UIntNative, subLen: UIntNative): CString
```

Function: Extracts a substring with specified starting position and length.

If the end position exceeds the string length, extraction stops at the string end.

> **Note:**
>
> 1. This method returns a copy of the substring, which must be manually freed after use.
> 2. Returns a null pointer if beginIndex equals the string length or subLen is 0.

Parameters:

- beginIndex: [UIntNative](core_package_intrinsics.md#uintnative) - The starting position for extraction, range [0, this.size()].
- subLen: [UIntNative](core_package_intrinsics.md#uintnative) - The length to extract, range [0, [UIntNative](core_package_intrinsics.md#uintnative).Max].

Returns:

- [CString](core_package_intrinsics.md#cstring) - The extracted substring.

Exceptions:

- [IndexOutOfBoundsException](core_package_exceptions.md#class-indexoutofboundsexception) - Thrown if beginIndex exceeds the string length.
- [IllegalMemoryException](core_package_exceptions.md#class-illegalmemoryexception) - Thrown if memory allocation or copying fails.

Example:

<!-- verify -->
```cangjie
main() {
    let index: UIntNative = 2
    let len: UIntNative = 2
    var str1: CString = unsafe { LibC.mallocCString("hello") }
    var str2: CString = str1.subCString(index, len)
    println(str2)

    unsafe {
        LibC.free(str1)
        LibC.free(str2)
    }
}
```

Execution Result:

```text
ll
```

### func toString()

```cangjie
public func toString(): String
```

Function: Converts [CString](core_package_intrinsics.md#cstring) to Cangjie's [String](core_package_structs.md#struct-string) type.

Returns:

- [String](core_package_structs.md#struct-string) - The converted string.

Exceptions:

- [IllegalArgumentException](core_package_exceptions.md#class-illegalargumentexception) - Thrown for invalid UTF-8 byte sequences.

Example:

<!-- verify -->
```cangjie
main() {
    var str1: CString = unsafe { LibC.mallocCString("hello") }
    println(str1.toString())

    unsafe {
        LibC.free(str1)
    }
}
```

Execution Result:

```text
hello
```

## extend Float16

```cangjie
extend Float16
```

Function: Supports conversion between [Float16](core_package_intrinsics.md#float16) and [UInt16](core_package_intrinsics.md#uint16).

Extended type:

- [Float16](core_package_intrinsics.md#float16)

### static func fromBits(UInt16)

```cangjie
public static func fromBits(bits: UInt16): Float16
```

Function: Converts the specified [UInt16](core_package_intrinsics.md#uint16) to a [Float16](core_package_intrinsics.md#float16) floating-point number according to the binary representation of float16 in the `IEEE 754` standard.

Parameter:

- bits: [UInt16](core_package_intrinsics.md#uint16) - The number to convert.

Return:

- [Float16](core_package_intrinsics.md#float16) - The conversion result, with the same bits as the parameter.

Example:

<!-- verify -->
```cangjie
main() {
    let v = Float16.fromBits(0x4A40u16)
    println(v)
}
```

Output:

```text
12.500000
```

### func toBits()

```cangjie
public func toBits(): UInt16
```

Function: Converts the specified [Float16](core_package_intrinsics.md#float16) number to a [UInt16](core_package_intrinsics.md#uint16) number representing its bits.

Return:

- [UInt16](core_package_intrinsics.md#uint16) - The conversion result, with the same bits as the [Float16](core_package_intrinsics.md#float16) value.

Example:

<!-- verify -->
```cangjie
main() {
    println(12.5f16.toBits()) // 0x4A40 19008
}
```

Output:

```text
19008
```

## extend Float16 <: Comparable\<Float16>

```cangjie
extend Float16 <: Comparable<Float16>
```

Function: Extends the [Float16](core_package_intrinsics.md#float16) type to implement the [Comparable](core_package_interfaces.md#interface-comparablet)\<[Float16](core_package_intrinsics.md#float16)> interface, supporting comparison operations.

Extended type:

- [Float16](core_package_intrinsics.md#float16)

Extended interfaces:

- Comparable\<Float16>

### func compare(Float16)

```cangjie
public func compare(rhs: Float16): Ordering
```

Function: Compares the current [Float16](core_package_intrinsics.md#float16) value with another specified [Float16](core_package_intrinsics.md#float16) value. `NaN` cannot participate in the comparison, the return value is meaningless.

Parameter:

- rhs: [Float16](core_package_intrinsics.md#float16) - Another [Float16](core_package_intrinsics.md#float16) value to compare.

Return:

- [Ordering](core_package_enums.md#enum-ordering) - Returns [Ordering](core_package_enums.md#enum-ordering).GT if greater, [Ordering](core_package_enums.md#enum-ordering).EQ if equal, or [Ordering](core_package_enums.md#enum-ordering).LT if less.

Example:

<!-- verify -->
```cangjie
main() {
    var num1: Float16 = 0.12
    var num2: Float16 = 0.234
    println(num1.compare(num2))
}
```

Output:

```text
Ordering.LT
```

## extend Float16 <: Hashable

```cangjie
extend Float16 <: Hashable
```

Function: Extends the [Float16](core_package_intrinsics.md#float16) type to implement the [Hashable](core_package_interfaces.md#interface-hashable) interface, supporting hash value computation, and extends the type to support certain mathematical constants.

Extended type:

- [Float16](core_package_intrinsics.md#float16)

Extended interfaces:

- Hashable

### static prop Inf

```cangjie
public static prop Inf: Float16
```

Function: Gets the infinity value for half-precision floating-point numbers.

Type: [Float16](./core_package_intrinsics.md#float16)

Example:

<!-- verify -->
```cangjie
main() {
    // Test Float16.Inf property
    let infValue = Float16.Inf
    println("Value of Float16.Inf: ${infValue}")
    
    // Verify it is indeed infinity
    println("Is Float16.Inf infinity: ${infValue.isInf()}")
}
```

Output:

```text
Value of Float16.Inf: inf
Is Float16.Inf infinity: true
```

### static prop Max

```cangjie
public static prop Max: Float16
```

Function: Gets the maximum value for half-precision floating-point numbers.

Type: [Float16](./core_package_intrinsics.md#float16)

Example:

<!-- verify -->
```cangjie
main() {
    // Test Float16.Max property
    let maxValue = Float16.Max
    println("Value of Float16.Max: ${maxValue}")
    
    // Verify it is indeed the maximum value
    println("Is Float16.Max a normal number: ${maxValue.isNormal()}")
}
```

Output:

```text
Value of Float16.Max: 65504.000000
Is Float16.Max a normal number: true
```

### static prop Min

```cangjie
public static prop Min: Float16
```

Function: Gets the minimum value for half-precision floating-point numbers.

Type: [Float16](./core_package_intrinsics.md#float16)

Example:

<!-- verify -->
```cangjie
main() {
    // Test Float16.Min property
    let minValue = Float16.Min
    println("Value of Float16.Min: ${minValue}")
    
    // Verify it is indeed the minimum value
    println("Is Float16.Min a normal number: ${minValue.isNormal()}")
}
```

Output:

```text
Value of Float16.Min: -65504.000000
Is Float16.Min a normal number: true
```

### static prop MinDenormal

```cangjie
public static prop MinDenormal: Float16
```

Function: Gets the smallest denormal number for half-precision floating-point numbers. The smallest positive denormal number is the smallest positive number representable in IEEE double-precision format.

Type: [Float16](./core_package_intrinsics.md#float16)

Example:

<!-- verify -->
```cangjie
main() {
    // Test Float16.MinDenormal property
    let minDenormalValue = Float16.MinDenormal
    println("Value of Float16.MinDenormal: ${minDenormalValue}")
    
    // Verify it is indeed the smallest denormal number
    println("Is Float16.MinDenormal a normal number: ${minDenormalValue.isNormal()}")
}
```

Output:

```text
Value of Float16.MinDenormal: 0.000000
Is Float16.MinDenormal a normal number: true
```

### static prop MinNormal

```cangjie
public static prop MinNormal: Float16
```

Function: Gets the smallest normal number for half-precision floating-point numbers.

Type: [Float16](./core_package_intrinsics.md#float16)

Example:

<!-- verify -->
```cangjie
main() {
    // Test Float16.MinNormal property
    let minNormalValue = Float16.MinNormal
    println("Value of Float16.MinNormal: ${minNormalValue}")
    
    // Verify it is indeed the smallest normal number
    println("Is Float16.MinNormal a normal number: ${minNormalValue.isNormal()}")
}
```

Output:

```text
Value of Float16.MinNormal: 0.000061
Is Float16.MinNormal a normal number: true
```

### static prop NaN

```cangjie
public static prop NaN: Float16
```

Function: Gets the Not-a-Number (NaN) value for half-precision floating-point numbers.

Type: [Float16](./core_package_intrinsics.md#float16)

Example:

<!-- verify -->
```cangjie
main() {
    // Test Float16.NaN property
    let nanValue = Float16.NaN
    println("Value of Float16.NaN: ${nanValue}")
    
    // Verify it is indeed NaN
    println("Is Float16.NaN Not-a-Number: ${nanValue.isNaN()}")
}
```

Output:

```text
Value of Float16.NaN: nan
Is Float16.NaN Not-a-Number: true
```

### static func max(Float16, Float16, Array\<Float16>)

```cangjie
public static func max(a: Float16, b: Float16, others: Array<Float16>): Float16
```

Function: Returns the maximum value among a set of [Float16](./core_package_intrinsics.md#float16) numbers. The third parameter is a variadic argument that allows comparing more than two [Float16](./core_package_intrinsics.md#float16) numbers. If any parameter is `NaN`, the function returns `NaN`.

Parameters:

- a: [Float16](./core_package_intrinsics.md#float16) - First number to compare.
- b: [Float16](./core_package_intrinsics.md#float16) - Second number to compare.
- others: [Array](core_package_structs.md#struct-arrayt)\<[Float16](./core_package_intrinsics.md#float16)> - Additional numbers to compare.

Return:

- [Float16](./core_package_intrinsics.md#float16) - The maximum value among the parameters.

Example:

<!-- verify -->
```cangjie
main() {
    // Test Float16.max static method
    let a: Float16 = 1.5f16
    let b: Float16 = 2.7f16
    let c: Float16 = 0.8f16
    let d: Float16 = 3.2f16
    let e: Float16 = 3.4f16
    let f: Float16 = 3.8f16
    let g: Float16 = 3.9f16
    
    // Find the maximum value using max method
    let maxValue = Float16.max(a, b, c, d, e, f, g)
    println("Result of Float16.max(1.5, 2.7, 0.8, 3.2, 3.4, 3.8, 3.9): ${maxValue}")
    
    // Verify the result
    println("Is the maximum value correct: ${maxValue == g}")
}
```

Output:

```text
Result of Float16.max(1.5, 2.7, 0.8, 3.2, 3.4, 3.8, 3.9): 3.900391
Is the maximum value correct: true
```

### static func min(Float16, Float16, Array\<Float16>)

```cangjie
public static func min(a: Float16, b: Float16, others: Array<Float16>): Float16
```

Function: Returns the minimum value among a set of [Float16](./core_package_intrinsics.md#float16) numbers. The third parameter is a variadic argument that allows comparing more than two [Float16](./core_package_intrinsics.md#float16) numbers. If any parameter is `NaN`, the function returns `NaN`.

Parameters:

- a: [Float16](./core_package_intrinsics.md#float16) - First number to compare.
- b: [Float16](./core_package_intrinsics.md#float16) - Second number to compare.
- others: [Array](core_package_structs.md#struct-arrayt)\<[Float16](./core_package_intrinsics.md#float16)> - Additional numbers to compare.

Return:

- [Float16](./core_package_intrinsics.md#float16) - The minimum value among the parameters.

Example:

<!-- verify -->
```cangjie
main() {
    // Test Float16.min static method
    let a: Float16 = 1.5f16
    let b: Float16 = 2.7f16
    let c: Float16 = 0.8f16
    let d: Float16 = 3.2f16
    let e: Float16 = 3.4f16
    let f: Float16 = 3.8f16
    let g: Float16 = 3.9f16
    
    // Find the minimum value using min method
    let minValue = Float16.min(a, b, c, d, e, f, g)
    println("Result of Float16.min(1.5, 2.7, 0.8, 3.2, 3.4, 3.8, 3.9): ${minValue}")
    
    // Verify the result
    println("Is the minimum value correct: ${minValue == c}")
}
```

Output:

```text
Result of Float16.min(1.5, 2.7, 0.8, 3.2, 3.4, 3.8, 3.9): 0.799805
Is the minimum value correct: true
```

### func hashCode()

```cangjie
public func hashCode(): Int64
```

Function: Computes the hash value.

Return:

- [Int64](core_package_intrinsics.md#int64) - The hash value.

Example:

<!-- verify -->
```cangjie
main() {
    // Test Float16.hashCode method
    let value1: Float16 = 1.5f16
    let value2: Float16 = 1.5f16
    let value3: Float16 = 2.0f16
    
    // Compute hash values
    let hash1 = value1.hashCode()
    let hash2 = value2.hashCode()
    let hash3 = value3.hashCode()
    
    println("Hash value of 1.5f16: ${hash1}")
    println("Hash value of 1.5f16: ${hash2}")
    println("Hash value of 2.0f16: ${hash3}")
    
    // Verify equal values have the same hash
    println("Do equal values have the same hash: ${hash1 == hash2}")
}
```

Output:

```text
Hash value of 1.5f16: 7729828949621302189
Hash value of 1.5f16: 772982894962130## Float32

Function: Represents a 32-bit floating-point number, conforming to the single-precision format (`binary32`) in `IEEE 754`.

### func isInf()

```cangjie
public func isInf(): Bool
```

Function: Determines whether a [Float16](./core_package_intrinsics.md#float16) value is infinity.

Return:

- [Bool](./core_package_intrinsics.md#bool) - Returns `true` if the [Float16](./core_package_intrinsics.md#float16) value is positive or negative infinity; otherwise, returns `false`.

Example:

<!-- verify -->
```cangjie
main() {
    // Test Float16.isInf method
    let normalValue: Float16 = 1.5f16
    let infValue = Float16.Inf
    let negInfValue = -Float16.Inf
    
    // Check if a normal value is infinity
    println("Result of 1.5f16.isInf(): ${normalValue.isInf()}")
    
    // Check if positive infinity is infinity
    println("Result of Float16.Inf.isInf(): ${infValue.isInf()}")
    
    // Check if negative infinity is infinity
    println("Result of (-Float16.Inf).isInf(): ${negInfValue.isInf()}")
}
```

Output:

```text
Result of 1.5f16.isInf(): false
Result of Float16.Inf.isInf(): true
Result of (-Float16.Inf).isInf(): true
```

### func isNaN()

```cangjie
public func isNaN(): Bool
```

Function: Determines whether a [Float16](./core_package_intrinsics.md#float16) value is Not-a-Number (NaN).

Return:

- [Bool](./core_package_intrinsics.md#bool) - Returns `true` if the [Float16](./core_package_intrinsics.md#float16) value is NaN; otherwise, returns `false`.

Example:

<!-- verify -->
```cangjie
main() {
    // Test Float16.isNaN method
    let normalValue: Float16 = 1.5f16
    let nanValue = Float16.NaN
    
    // Check if a normal value is NaN
    println("Result of 1.5f16.isNaN(): ${normalValue.isNaN()}")
    
    // Check if NaN value is NaN
    println("Result of Float16.NaN.isNaN(): ${nanValue.isNaN()}")
}
```

Output:

```text
Result of 1.5f16.isNaN(): false
Result of Float16.NaN.isNaN(): true
```

### func isNormal()

```cangjie
public func isNormal(): Bool
```

Function: Determines whether a [Float16](./core_package_intrinsics.md#float16) value is a normal number.

Return:

- [Bool](./core_package_intrinsics.md#bool) - Returns `true` if the [Float16](./core_package_intrinsics.md#float16) value is a normal floating-point number; otherwise, returns `false`.

Example:

<!-- verify -->
```cangjie
main() {
    // Test Float16.isNormal method
    let normalValue: Float16 = 1.5f16
    let zeroValue: Float16 = 0.0f16
    let infValue = Float16.Inf
    let nanValue = Float16.NaN
    
    // Check if a normal value is normal
    println("Result of 1.5f16.isNormal(): ${normalValue.isNormal()}")
    
    // Check if zero is normal
    println("Result of 0.0f16.isNormal(): ${zeroValue.isNormal()}")
    
    // Check if infinity is normal
    println("Result of Float16.Inf.isNormal(): ${infValue.isNormal()}")
    
    // Check if NaN is normal
    println("Result of Float16.NaN.isNormal(): ${nanValue.isNormal()}")
}
```

Output:

```text
Result of 1.5f16.isNormal(): true
Result of 0.0f16.isNormal(): false
Result of Float16.Inf.isNormal(): false
Result of Float16.NaN.isNormal(): false
```

## extend Float16 <: ToString

```cangjie
extend Float16 <: ToString
```

Function: Extends the [Float16](core_package_intrinsics.md#float16) type with the [ToString](core_package_interfaces.md#interface-tostring) interface to enable conversion to [String](core_package_structs.md#struct-string) type. By default, it retains 6 decimal places.

Extended type:

- [Float16](core_package_intrinsics.md#float16)

Extended interfaces:

- ToString

### func toString()

```cangjie
public func toString(): String
```

Function: Converts a [Float16](core_package_intrinsics.md#float16) value into an output-ready string.

Returns:

- [String](core_package_structs.md#struct-string) - The converted string.

Example:

<!-- verify -->
```cangjie
main() {
    // Test Float16.toString method
    let value1: Float16 = 1.5f16
    let value2: Float16 = 123.456f16
    let value3: Float16 = -0.001f16
    
    // Convert to strings
    let str1 = value1.toString()
    let str2 = value2.toString()
    let str3 = value3.toString()
    
    println("1.5f16 converted to string: \"${str1}\"")
    println("123.456f16 converted to string: \"${str2}\"")
    println("-0.001f16 converted to string: \"${str3}\"")
    
    // Verify type
    println("Is the converted type String: ${str1 is String}")
}
```

Execution Result:

```text
1.5f16 converted to string: "1.500000"
123.456f16 converted to string: "123.437500"
-0.001f16 converted to string: "-0.001000"
Is the converted type String: true
```

## extend Float32

```cangjie
extend Float32
```

Function: Supports conversion between [Float32](core_package_intrinsics.md#float32) and [UInt32](core_package_intrinsics.md#uint32).

Extended type:

- [Float32](core_package_intrinsics.md#float32)

### static func fromBits(UInt32)

```cangjie
public static func fromBits(bits: UInt32): Float32
```

Function: Converts the specified [UInt32](core_package_intrinsics.md#uint32) to the corresponding [Float32](core_package_intrinsics.md#float32) floating-point number according to the `IEEE 754` standard's binary representation for float32.

Parameters:

- bits: [UInt32](core_package_intrinsics.md#uint32) - The number to convert.

Returns:

- [Float32](core_package_intrinsics.md#float32) - The conversion result, with the same bits as the parameter bits.

Example:

<!-- verify -->
```cangjie
main() {
    let v = Float32.fromBits(0x415E147Bu32)
    println(v)
}
```

Output:

```text
13.880000
```

### func toBits()

```cangjie
public func toBits(): UInt32
```

Function: Converts the specified [Float32](core_package_intrinsics.md#float32) number to its bit representation as a [UInt32](core_package_intrinsics.md#uint32) number.

Returns:

- [UInt32](core_package_intrinsics.md#uint32) - The conversion result, with the same value as the bit representation of [Float32](core_package_intrinsics.md#float32).

Example:

<!-- verify -->
```cangjie
main() {
    println(13.88f32.toBits()) // 0x415E147B 1096684667
}
```

Output:

```text
1096684667
```

## extend Float32 <: Comparable\<Float32>

```cangjie
extend Float32 <: Comparable<Float32>
```

Function: Extends the [Float32](core_package_intrinsics.md#float32) type with the [Comparable](core_package_interfaces.md#interface-comparablet)\<[Float32](core_package_intrinsics.md#float32)> interface to support comparison operations.

Extended type:

- [Float32](core_package_intrinsics.md#float32)

Extended interfaces:

- Comparable\<Float32>

### func compare(Float32)

```cangjie
public func compare(rhs: Float32): Ordering
```

Function: Determines the relationship between the current [Float32](core_package_intrinsics.md#float32) value and the specified [Float32](core_package_intrinsics.md#float32) value. `NaN` cannot participate in the comparison, the return value is meaningless.

Parameters:

- rhs: [Float32](core_package_intrinsics.md#float32) - Another [Float32](core_package_intrinsics.md#float32) value to compare.

Returns:

- [Ordering](core_package_enums.md#enum-ordering) - Returns [Ordering](core_package_enums.md#enum-ordering).GT if greater; [Ordering](core_package_enums.md#enum-ordering).EQ if equal; [Ordering](core_package_enums.md#enum-ordering).LT if less.

Example:

<!-- verify -->
```cangjie
main() {
    var num1: Float32 = 0.12
    var num2: Float32 = 0.234
    println(num1.compare(num2))
}
```

Output:

```text
Ordering.LT
```

## extend Float32 <: Hashable

```cangjie
extend Float32 <: Hashable
```

Function: Extends the [Float32](core_package_intrinsics.md#float32) type with the [Hashable](core_package_interfaces.md#interface-hashable) interface to support hash value computation, and extends the type to support certain mathematical constants.

Extended type:

- [Float32](core_package_intrinsics.md#float32)

Extended interfaces:

- Hashable

### static prop Inf

```cangjie
public static prop Inf: Float32
```

Function: Gets the infinity value for single-precision floating-point numbers.

Type: [Float32](./core_package_intrinsics.md#float32)

Example:

<!-- verify -->
```cangjie
main() {
    // Test Float32.Inf property
    let infValue = Float32.Inf
    println("Value of Float32.Inf: ${infValue}")
    
    // Verify it is indeed infinity
    println("Is Float32.Inf infinity: ${infValue.isInf()}")
}
```

Output:

```text
Value of Float32.Inf: inf
Is Float32.Inf infinity: true
```

### static prop Max

```cangjie
public static prop Max: Float32
```

Function: Gets the maximum value for single-precision floating-point numbers.

Type: [Float32](./core_package_intrinsics.md#float32)

Example:

<!-- verify -->
```cangjie
main() {
    // Test Float32.Max property
    let maxValue = Float32.Max
    println("Value of Float32.Max: ${maxValue}")
    
    // Verify it is indeed the maximum value
    println("Is Float32.Max a normal number: ${maxValue.isNormal()}")
}
```

Output:

```text
Value of Float32.Max: 340282346638528859811704183484516925440.000000
Is Float32.Max a normal number: true
```

### static prop Min

```cangjie
public static prop Min: Float32
```

Function: Gets the minimum value for single-precision floating-point numbers.

Type: [Float32](./core_package_intrinsics.md#float32)

Example:

<!-- verify -->
```cangjie
main() {
    // Test Float32.Min property
    let minValue = Float32.Min
    println("Value of Float32.Min: ${minValue}")
    
    // Verify it is indeed the minimum value
    println("Is Float32.Min a normal number: ${minValue.isNormal()}")
}
```

Output:

```text
Value of Float32.Min: -340282346638528859811704183484516925440.000000
Is Float32.Min a normal number: true
```

### static prop MinDenormal

```cangjie
public static prop MinDenormal: Float32
```

Function: Gets the smallest denormal number for single-precision floating-point numbers.

Type: [Float32](./core_package_intrinsics.md#float32)

Example:

<!-- verify -->
```cangjie
main() {
    // Test Float32.MinDenormal property
    let minDenormalValue = Float32.MinDenormal
    println("Value of Float32.MinDenormal: ${minDenormalValue}")
    
    // Verify it is indeed the smallest denormal number
    println("Is Float32.MinDenormal a normal number: ${minDenormalValue.isNormal()}")
}
```

Output:

```text
Value of Float32.MinDenormal: 0.000000
Is Float32.MinDenormal a normal number: false
```

### static prop MinNormal

```cangjie
public static prop MinNormal: Float32
```

Function: Gets the smallest normal number for single-precision floating-point numbers.

Type: [Float32](./core_package_intrinsics.md#float32)

Example:

<!-- verify -->
```cangjie
main() {
    // Test Float32.MinNormal property
    let minNormalValue = Float32.MinNormal
    println("Value of Float32.MinNormal: ${minNormalValue}")
    
    // Verify it is indeed the smallest normal number
    println("Is Float32.MinNormal a normal number: ${minNormalValue.isNormal()}")
}
```

Output:

```text
Value of Float32.MinNormal: 0.000000
Is Float32.MinNormal a normal number: true
```

### static prop NaN

```cangjie
public static prop NaN: Float32
```

Function: Gets the Not-a-Number (NaN) value for single-precision floating-point numbers.

Type: [Float32](./core_package_intrinsics.md#float32)

Example:

<!-- verify -->
```cangjie
main() {
    // Test Float32.NaN property
    let nanValue = Float32.NaN
    println("Value of Float32.NaN: ${nanValue}")
    
    // Verify it is indeed NaN
    println("Is Float32.NaN Not-a-Number: ${nanValue.isNaN()}")
}
```

Output:

```text
Value of Float32.NaN: nan
Is Float32.NaN Not-a-Number: true
```

### static func max(Float32, Float32, Array\<Float32>)

```cangjie
public static func max(a: Float32, b: Float32, others: Array<Float32>): Float32
```

Function: Returns the maximum value among a set of [Float32](./core_package_intrinsics.md#float32) numbers. The third parameter is a variadic argument that allows comparing more than two [Float32](./core_package_intrinsics.md#float32) numbers. If any parameter is `NaN`, the function returns `NaN`.

Parameters:

- a: [Float32](./core_package_intrinsics.md#float32) - The first number to compare.
- b: [Float32](./core_package_intrinsics.md#float32) - The second number to compare.
- others: [Array](core_package_structs.md#struct-arrayt)\<[Float32](./core_package_intrinsics.md#float32)> - Additional numbers to compare.

Returns:

- [Float32](./core_package_intrinsics.md#float32) - The maximum value among the parameters.

Example:

<!-- verify -->
```cangjie
main() {
    // Test Float32.max static method
    let a: Float32 = 1.5f32
    let b: Float32 = 2.7f32
    let c: Float32 = 0.8f32
    let d: Float32 = 3.2f32
    
    // Find the maximum value using max method
    let maxValue = Float32.max(a, b, c, d)
    println("Result of Float32.max(1.5, 2.7, 0.8, 3.2): ${maxValue}")
    
    // Verify the result
    println("Is the maximum value correct: ${maxValue == d}")
}
```

Output:

```text
Result of Float32.max(1.5, 2.7, 0.8, 3.2): 3.200000
Is the maximum value correct: true
```

### static func min(Float32, Float32, Array\<Float32>)

```cangjie
public static func min(a: Float32, b: Float32, others: Array<Float32>): Float32
```

Function: Returns the minimum value among a set of [Float32](./core_package_intrinsics.md#float32) numbers. The third parameter is a variadic argument that allows comparing more than two [Float32](./core_package_intrinsics.md#float32) numbers. If any parameter is `NaN`, the function returns `NaN`.

Parameters:

- a: [Float32](./core_package_intrinsics.md#float32) - The first number to compare.
- b: [Float32](./core_package_intrinsics.md#float32) - The second number to compare.
- others: [Array](core_package_structs.md#struct-arrayt)\<[Float32](./core_package_intrinsics.md#float32)> - Additional numbers to compare.

Returns:

- [Float32](./core_package_intrinsics.md#float32) - The minimum value among the parameters.

Example:

<!-- verify -->
```cangjie
main() {
    // Test Float32.min static method
    let a: Float32 = 1.5f32
    let b: Float32 = 2.7f32
    let c: Float32 = 0.8f32
    let d: Float32 = 3.2f32
    
    // Find the minimum value using min method
    let minValue = Float32.min(a, b, c, d)
    println("Result of Float32.min(1.5, 2.7, 0.8, 3.2): ${minValue}")
    
    // Verify the result
    println("Is the minimum value correct: ${minValue == c}")
}
```

Output:

```text
Result of Float32.min(1.5, 2.7, 0.8, 3.2): 0.800000
Is the minimum value correct: true
```

### func hashCode()

```cangjie
public func hashCode(): Int64
```

Function: Gets the hash value.

Returns:

- [Int64](core_package_intrinsics.md#int64) - The hash value.

Example:

<!-- verify -->
```cangjie
main() {
    // Test Float32.hashCode method
    let value1: Float32 = 1.5f32
    let value2: Float32 = 1.5f32
    let value3: Float32 = 2.0f32
    
    // Compute hash values
    let hash1 = value1.hashCode()
    let hash2 = value2.hashCode()
    let hash3 = value3.hashCode()
    
    println("Hash value of 1.5f32: ${hash1}")
    println("Hash value of 1.5f32: ${hash2}")
    println("Hash value of 2.0f32: ${hash3}")
    
    // Verify equal values have the same hash value
    println("Do equal values have the same hash: ${hash1 == hash2}")
}
```

Output:

```text
Hash value of 1.5f32: 7729828949621302189
Hash value of 1.5f32: 7729828949621302189
Hash value of 2.0f32: 396734039217596648
Do equal values have the same hash: true
```

### func isInf()

```cangjie
public func isInf(): Bool
```

Function: Determines whether a floating-point number [Float32](./core_package_intrinsics.md#float32) is infinite.

Returns:

- [Bool](./core_package_intrinsics.md#bool) - Returns `true` if the [Float32](./core_package_intrinsics.md#float32) value is positive or negative infinity; otherwise, returns `false`.

Example:

<!-- verify -->
```cangjie
main() {
    // Test Float32.isInf method
    let normalValue: Float32 = 1.5f32
    let infValue = Float32.Inf
    let negInfValue = -Float32.Inf
    
    // Check if a normal value is infinite
    println("Result of 1.5f32.isInf(): ${normalValue.isInf()}")
    
    // Check if positive infinity is infinite
    println("Result of Float32.Inf.isInf(): ${infValue.isInf()}")
    
    // Check if negative infinity is infinite
    println("Result of (-Float32.Inf).isInf(): ${negInfValue.isInf()}")
}
```

Output:

```text
Result of 1.5f32.isInf(): false
Result of Float32.Inf.isInf(): true
Result of (-Float32.Inf).isInf(): true
```

### func isNaN()

```cangjie
public func isNaN(): Bool
```

Function: Determines whether a floating-point number [Float32](./core_package_intrinsics.md#float32) is Not-a-Number (NaN).

Returns:

- [Bool](./core_package_intrinsics.md#bool) - Returns `true` if the [Float32](./core_package_intrinsics.md#float32) value is NaN; otherwise, returns `false`.

Example:

<!-- verify -->
```cangjie
main() {
    // Test Float32.isNaN method
    let normalValue: Float32 = 1.5f32
    let nanValue = Float32.NaN
    
    // Check if a normal value is NaN
    println("Result of 1.5f32.isNaN(): ${normalValue.isNaN()}")
    
    // Check if NaN value is NaN
    println("Result of Float32.NaN.isNaN(): ${nanValue.isNaN()}")
}
```

Output:

```text
Result of 1.5f32.isNaN(): false
Result of Float32.NaN.isNaN(): true
```

### func isNormal()

```cangjie
public func isNormal(): Bool
```

Function: Determines whether a floating-point number [Float32](./core_package_intrinsics.md#float32) is a normal number.

Returns:

- [Bool](./core_package_intrinsics.md#bool) - Returns `true` if the [Float32](./core_package_intrinsics.md#float32) value is a normal floating-point number; otherwise, returns `false`.

Example:

<!-- verify -->
```cangjie
main() {
    // Test Float32.isNormal method
    let normalValue: Float32 = 1.5f32
    let zeroValue: Float32 = 0.0f32
    let infValue = Float32.Inf
    let nanValue = Float32.NaN
    
    // Check if a normal value is normal
    println("Result of 1.5f32.isNormal(): ${normalValue.isNormal()}")
    
    // Check if zero is normal
    println("Result of 0.0f32.isNormal(): ${zeroValue.isNormal()}")
    
    // Check if infinity is normal
    println("Result of Float32.Inf.isNormal(): ${infValue.isNormal()}")
    
    // Check if NaN is normal
    println("Result of Float32.NaN.isNormal(): ${nanValue.isNormal()}")
}
```

Output:

```text
Result of 1.5f32.isNormal(): true
Result of 0.0f32.isNormal(): false
Result of Float32.Inf.isNormal(): false
Result of Float32.NaN.isNormal(): false
```

## extend Float32 <: ToString

```cangjie
extend Float32 <: ToString
```

Function: Extends the [Float32](core_package_intrinsics.md#float32) type with the [ToString](core_package_interfaces.md#interface-tostring) interface to support conversion to [String](core_package_structs.md#struct-string) type. By default, retains 6 decimal places.

Extended type:

- [Float32](core_package_intrinsics.md#float32)

Extended interfaces:

- ToString

### func toString()

```cangjie
public func toString(): String
```

Function: Converts a [Float32](core_package_intrinsics.md#float32) value to a printable string.

Returns:

- [String](core_package_structs.md#struct-string) - The converted string.

Example:

<!-- verify -->
```cangjie
main() {
    // Testing Float32.toString method
    let value1: Float32 = 1.5f32
    let value2: Float32 = 123.456f32
    let value3: Float32 = -0.001f32
    
    // Convert to strings
    let str1 = value1.toString()
    let str2 = value2.toString()
    let str3 = value3.toString()
    
    println("1.5f32 converted to string: \"${str1}\"")
    println("123.456f32 converted to string: \"${str2}\"")
    println("-0.001f32 converted to string: \"${str3}\"")
    
    // Verify type
    println("Is converted type String: ${str1 is String}")
}
```

Execution Result:

```text
1.5f32 converted to string: "1.500000"
123.456f32 converted to string: "123.456001"
-0.001f32 converted to string: "-0.001000"
Is converted type String: true
```

## extend Float64

```cangjie
extend Float64
```

Function: Supports mutual conversion with [UInt64](core_package_intrinsics.md#uint64).

Extended type:

- [Float64](core_package_intrinsics.md#float32)

### static func fromBits(UInt64)

```cangjie
public static func fromBits(bits: UInt64): Float64
```

Function: Converts the specified [UInt64](core_package_intrinsics.md#uint64) to the corresponding [Float64](core_package_intrinsics.md#float32) floating-point number according to the binary representation of float64 in the `IEEE 754` standard.

Parameter:

- bits: [UInt64](core_package_intrinsics.md#uint64) - The number to convert.

Returns:

- [Float64](core_package_intrinsics.md#float64) - The conversion result, whose bits are identical to the parameter bits.

Example:

<!-- verify -->
```cangjie
main() {
    let v = Float64.fromBits(0x402BC28F5C28F5C3u64)
    println(v)
}
```

Output:

```text
13.880000
```

### func toBits()

```cangjie
public func toBits(): UInt64
```

Function: Converts the specified Float64 number to the corresponding [UInt64](core_package_intrinsics.md#uint64) number represented by its bits.

Returns:

- [UInt64](core_package_intrinsics.md#uint64) - The conversion result, whose value is identical to the bit representation of the [Float64](core_package_intrinsics.md#float64).

Example:

<!-- verify -->
```cangjie
main() {
    println(13.88f64.toBits()) // 0x402BC28F5C28F5C3 4624003363408246211
}
```

Output:

```text
4624003363408246211
```

## extend Float64

```cangjie
extend Float64
```

Function: Extends the [Float64](../../core/core_package_api/core_package_intrinsics.md#float64) type as the left operand and the [Duration](../../core/core_package_api/core_package_structs.md#struct-duration) type as the right operand for multiplication.

Extended type:

- [Float64](../../core/core_package_api/core_package_intrinsics.md#float64)

### operator func *(Duration)

```cangjie
public operator func *(r: Duration): Duration
```

Function: Implements multiplication between [Float64](../../core/core_package_api/core_package_intrinsics.md#float64) and [Duration](../../core/core_package_api/core_package_structs.md#struct-duration) types, i.e., the [Float64](../../core/core_package_api/core_package_intrinsics.md#float64) * [Duration](../../core/core_package_api/core_package_structs.md#struct-duration) operation.

Parameters:

- r: [Duration](../../core/core_package_api/core_package_structs.md#struct-duration) - A [Duration](../../core/core_package_api/core_package_structs.md#struct-duration) instance.

Returns:

- [Duration](../../core/core_package_api/core_package_structs.md#struct-duration) - The product of the [Float64](../../core/core_package_api/core_package_intrinsics.md#float64) instance and `r`.

Exceptions:

- [ArithmeticException](../../core/core_package_api/core_package_exceptions.md#class-arithmeticexception) - Thrown when the multiplication result exceeds the representable range of [Duration](../../core/core_package_api/core_package_structs.md#struct-duration).

Example:

<!-- verify -->
```cangjie
main() {
    // Test Float64 * Duration operation
    let floatVal: Float64 = 2.5
    let duration: Duration = Duration.second
    
    let result = floatVal * duration
    
    println("Float64 value: ${floatVal}")
    println("Duration value: ${duration}")
    println("Operation result: ${result}")
}
```

Output:

```text
Float64 value: 2.500000
Duration value: 1s
Operation result: 2s500ms
```

## extend Float64 <: Comparable\<Float64>

```cangjie
extend Float64 <: Comparable<Float64>
```

Function: Extends the [Float64](core_package_intrinsics.md#float64) type with the [Comparable](core_package_interfaces.md#interface-comparablet)\<[Float64](core_package_intrinsics.md#float64)> interface to support comparison operations.

Extended type:

- [Float64](core_package_intrinsics.md#float64)

Extended interfaces:

- Comparable\<Float64>

### func compare(Float64)

```cangjie
public func compare(rhs: Float64): Ordering
```

Function: Determines the relationship between the current [Float64](core_package_intrinsics.md#float64) value and the specified [Float64](core_package_intrinsics.md#float64) value. `NaN` cannot participate in the comparison, the return value is meaningless.

Parameter:

- rhs: [Float64](core_package_intrinsics.md#float64) - Another [Float64](core_package_intrinsics.md#float64) value to compare.

Returns:

- [Ordering](core_package_enums.md#enum-ordering) - Returns [Ordering](core_package_enums.md#enum-ordering).GT if greater than, [Ordering](core_package_enums.md#enum-ordering).EQ if equal, and [Ordering](core_package_enums.md#enum-ordering).LT if less than.

Example:

<!-- verify -->
```cangjie
main() {
    var num1: Float64 = 0.12
    var num2: Float64 = 0.234
    println(num1.compare(num2))
}
```

Output:

```text
Ordering.LT
```

## extend Float64 <: Hashable

```cangjie
extend Float64 <: Hashable
```

Function: Extends the [Float64](core_package_intrinsics.md#float64) type with the [Hashable](core_package_interfaces.md#interface-hashable) interface to support hash value computation, and extends the type to support certain mathematical constants.

Extended type:

- [Float64](core_package_intrinsics.md#float64)

Extended interfaces:

- Hashable

### static prop Inf

```cangjie
public static prop Inf: Float64
```

Function: Gets the infinity value for double-precision floating-point numbers.

Type: [Float64](./core_package_intrinsics.md#float64)

Example:

<!-- verify -->
```cangjie
main() {
    // Test Float64.Inf property
    let infValue = Float64.Inf
    println("Value of Float64.Inf: ${infValue}")
    
    // Verify it is indeed infinity
    println("Is Float64.Inf infinite: ${infValue.isInf()}")
}
```

Output:

```text
Value of Float64.Inf: inf
Is Float64.Inf infinite: true
```

### static prop Max

```cangjie
public static prop Max: Float64
```

Function: Gets the maximum value for double-precision floating-point numbers.

Type: [Float64](./core_package_intrinsics.md#float64)

Example:

<!-- verify -->
```cangjie
main() {
    // Test Float64.Max property
    let maxValue = Float64.Max
    println("Value of Float64.Max: ${maxValue}")
    
    // Verify it is indeed the maximum value
    println("Is Float64.Max a normal number: ${maxValue.isNormal()}")
}
```

Output:

```text
Value of Float64.Max: 179769313486231570814527423731704356798070567525844996598917476803157260780028538760589558632766878171540458953514382464234321326889464182768467546703537516986049910576551282076245490090389328944075868508455133942304583236903222948165808559332123348274797826204144723168738177180919299881250404026184124858368.000000
Is Float64.Max a normal number: true
```

### static prop Min

```cangjie
public static prop Min: Float64
```

Function: Gets the minimum value for double-precision floating-point numbers.

Type: [Float64](./core_package_intrinsics.md#float64)

Example:

<!-- verify -->
```cangjie
main() {
    // Test Float64.Min property
    let minValue = Float64.Min
    println("Value of Float64.Min: ${minValue}")
    
    // Verify it is indeed the minimum value
    println("Is Float64.Min a normal number: ${minValue.isNormal()}")
}
```

Output:

```text
Value of Float64.Min: -179769313486231570814527423731704356798070567525844996598917476803157260780028538760589558632766878171540458953514382464234321326889464182768467546703537516986049910576551282076245490090389328944075868508455133942304583236903222948165808559332123348274797826204144723168738177180919299881250404026184124858368.000000
Is Float64.Min a normal number: true
```

### static prop MinDenormal

```cangjie
public static prop MinDenormal: Float64
```

Function: Gets the smallest denormal number for double-precision floating-point numbers.

Type: [Float64](./core_package_intrinsics.md#float64)

Example:

<!-- verify -->
```cangjie
main() {
    // Test Float64.MinDenormal property
    let minDenormalValue = Float64.MinDenormal
    println("Value of Float64.MinDenormal: ${minDenormalValue}")
    
    // Verify it is indeed the smallest denormal number
    println("Is Float64.MinDenormal a normal number: ${minDenormalValue.isNormal()}")
}
```

Output:

```text
Value of Float64.MinDenormal: 0.000000
Is Float64.MinDenormal a normal number: false
```

### static prop MinNormal

```cangjie
public static prop MinNormal: Float64
```

Function: Gets the smallest normal number for double-precision floating-point numbers.

Type: [Float64](./core_package_intrinsics.md#float64)

Example:

<!-- verify -->
```cangjie
main() {
    // Test Float64.MinNormal property
    let minNormalValue = Float64.MinNormal
    println("Value of Float64.MinNormal: ${minNormalValue}")
    
    // Verify it is indeed the smallest normal number
    println("Is Float64.MinNormal a normal number: ${minNormalValue.isNormal()}")
}
```

Output:

```text
Value of Float64.MinNormal: 0.000000
Is Float64.MinNormal a normal number: true
```

### static prop NaN

```cangjie
public static prop NaN: Float64
```

Function: Gets the Not-a-Number (NaN) value for double-precision floating-point numbers.

Type: [Float64](./core_package_intrinsics.md#float64)

Example:

<!-- verify -->
```cangjie
main() {
    // Test Float64.NaN property
    let nanValue = Float64.NaN
    println("Value of Float64.NaN: ${nanValue}")
    
    // Verify it is indeed NaN
    println("Is Float64.NaN a NaN: ${nanValue.isNaN()}")
}
```

Output:

```text
Value of Float64.NaN: nan
Is Float64.NaN a NaN: true
```

### static func max(Float64, Float64, Array\<Float64>)

```cangjie
public static func max(a: Float64, b: Float64, others: Array<Float64>): Float64
```

Function: Returns the maximum value among a set of [Float64](./core_package_intrinsics.md#float64) numbers. The third parameter is a variadic argument that allows comparing more than two [Float64](./core_package_intrinsics.md#float64) numbers. If any parameter is `NaN`, the function returns `NaN`.

Parameters:

- a: [Float64](./core_package_intrinsics.md#float64) - The first number to compare.
- b: [Float64](./core_package_intrinsics.md#float64) - The second number to compare.
- others: [Array](core_package_structs.md#struct-arrayt)\<[Float64](./core_package_intrinsics.md#float64)> - Additional numbers to compare.

Returns:

- [Float64](./core_package_intrinsics.md#float64) - The maximum value among the parameters.

Example:

<!-- verify -->
```cangjie
main() {
    // Test Float64.max static method
    let a: Float64 = 1.5f64
    let b: Float64 = 2.7f64
    let c: Float64 = 0.8f64
    let d: Float64 = 3.2f64
    
    // Use max method to find the maximum value
    let maxValue = Float64.max(a, b, [c, d])
    println("Result of Float64.max(1.5, 2.7, [0.8, 3.2]): ${maxValue}")
    
    // Verify the result
    println("Is the maximum value correct: ${maxValue == d}")
}
```

Output:

```text
Result of Float64.max(1.5, 2.7, [0.8, 3.2]): 3.200000
Is the maximum value correct: true
```

### static func min(Float64, Float64, Array\<Float64>)

```cangjie
public static func min(a: Float64, b: Float64, others: Array<Float64>): Float64
```

Function: Returns the minimum value among a set of [Float64](./core_package_intrinsics.md#float64) numbers. The third parameter is a variadic argument that allows comparing more than two [Float64](./core_package_intrinsics.md#float64) numbers. If any parameter is `NaN`, the function returns `NaN`.

Parameters:

- a: [Float64](./core_package_intrinsics.md#float64) - The first number to compare.
- b: [Float64](./core_package_intrinsics.md#float64) - The second number to compare.
- others: [Array](core_package_structs.md#struct-arrayt)\<[Float64](./core_package_intrinsics.md#float64)> - Additional numbers to compare.

Returns:

- [Float64](./core_package_intrinsics.md#float64) - The minimum value among the parameters.

Example:

<!-- verify -->
```cangjie
main() {
    // Test Float64.min static method
    let a: Float64 = 1.5f64
    let b: Float64 = 2.7f64
    let c: Float64 = 0.8f64
    let d: Float64 = 3.2f64
    
    // Use min method to find the minimum value
    let minValue = Float64.min(a, b, [c, d])
    println("Result of Float64.min(1.5, 2.7, [0.8, 3.2]): ${minValue}")
    
    // Verify the result
    println("Is the minimum value correct: ${minValue == c}")
}
```

Output:

```text
Result of Float64.min(1.5, 2.7, [0.8, 3.2]): 0.800000
Is the minimum value correct: true
```

### func hashCode()

```cangjie
public func hashCode(): Int64
```

Function: Gets the hash value.

Returns:

- [Int64](core_package_intrinsics.md#int64) - The hash value.

Example:

<!-- verify -->
```cangjie
main() {
    // Test Float64.hashCode method
    let value1: Float64 = 1.5f64
    let value2: Float64 = 1.5f64
    let value3: Float64 = 2.0f64
    
    // Compute hash values
    let hash1 = value1.hashCode()
    let hash2 = value2.hashCode()
    let hash3 = value3.hashCode()
    
    println("Hash value of 1.5f64: ${hash1}")
    println("Hash value of 1.5f64: ${hash2}")
    println("Hash value of 2.0f64: ${hash3}")
    
    // Verify that equal values have the same hash value
    println("Do equal values have the same hash: ${hash1 == hash2}")
}
```

Execution result:

```text
Hash value of 1.5f64: -991904502584296975
Hash value of 1.5f64: -991904502584296975
Hash value of 2.0f64: 8889205371494514623
Do equal values have the same hash: true
```

### func isInf()

```cangjie
public func isInf(): Bool
```

Function: Determines whether a floating-point number [Float64](./core_package_intrinsics.md#float64) is infinite.

Returns:

- [Bool](./core_package_intrinsics.md#bool) - Returns `true` if the [Float64](./core_package_intrinsics.md#float64) value is positive or negative infinity; otherwise, returns `false`.

Example:

<!-- verify -->
```cangjie
main() {
    // Test Float64.isInf method
    let normalValue: Float64 = 1.5f64
    let infValue = Float64.Inf
    let negInfValue = -Float64.Inf
    
    // Check if a normal value is infinite
    println("Result of 1.5f64.isInf(): ${normalValue.isInf()}")
    
    // Check if positive infinity is infinite
    println("Result of Float64.Inf.isInf(): ${infValue.isInf()}")
    
    // Check if negative infinity is infinite
    println("Result of (-Float64.Inf).isInf(): ${negInfValue.isInf()}")
}
```

Output:

```text
Result of 1.5f64.isInf(): false
Result of Float64.Inf.isInf(): true
Result of (-Float64.Inf).isInf(): true
```

### func isNaN()

```cangjie
public func isNaN(): Bool
```

Function: Determines whether a floating-point number [Float64](./core_package_intrinsics.md#float64) is Not-a-Number (NaN).

Returns:

- [Bool](./core_package_intrinsics.md#bool) - Returns `true` if the [Float64](./core_package_intrinsics.md#float64) value is NaN; otherwise, returns `false`.

Example:

<!-- verify -->
```cangjie
main() {
    // Test Float64.isNaN method
    let normalValue: Float64 = 1.5f64
    let nanValue = Float64.NaN
    
    // Check if a normal value is NaN
    println("Result of 1.5f64.isNaN(): ${normalValue.isNaN()}")
    
    // Check if NaN value is NaN
    println("Result of Float64.NaN.isNaN(): ${nanValue.isNaN()}")
}
```

Output:

```text
Result of 1.5f64.isNaN(): false
Result of Float64.NaN.isNaN(): true
```

### func isNormal()

```cangjie
public func isNormal(): Bool
```

Function: Determines whether a floating-point number [Float64](./core_package_intrinsics.md#float64) is a normal number.

Returns:

- [Bool](./core_package_intrinsics.md#bool) - Returns `true` if the [Float64](./core_package_intrinsics.md#float64) value is a normal floating-point number; otherwise, returns `false`.

Example:

<!-- verify -->
```cangjie
main() {
    // Test Float64.isNormal method
    let normalValue: Float64 = 1.5f64
    let zeroValue: Float64 = 0.0f64
    let infValue = Float64.Inf
    let nanValue = Float64.NaN
    
    // Check if a normal value is normal
    println("Result of 1.5f64.isNormal(): ${normalValue.isNormal()}")
    
    // Check if zero is normal
    println("Result of 0.0f64.isNormal(): ${zeroValue.isNormal()}")
    
    // Check if infinity is normal
    println("Result of Float64.Inf.isNormal(): ${infValue.isNormal()}")
    
    // Check if NaN is normal
    println("Result of Float64.NaN.isNormal(): ${nanValue.isNormal()}")
}
```

Output:

```text
Result of 1.5f64.isNormal(): true
Result of 0.0f64.isNormal(): false
Result of Float64.Inf.isNormal(): false
Result of Float64.NaN.isNormal(): false
```

## extend Float64 <: ToString

```cangjie
extend Float64 <: ToString
```

Function: Extends the [Float64](core_package_intrinsics.md#float64) type to implement the [ToString](core_package_interfaces.md#interface-tostring) interface, enabling conversion to [String](core_package_structs.md#struct-string) type. By default retains 6 decimal places.

Extended type:

- [Float64](core_package_intrinsics.md#float64)

Extended interfaces:

- [ToString](core_package_interfaces.md#interface-tostring)

### func toString()

```cangjie
public func toString(): String
```

Function: Converts a [Float64](core_package_intrinsics.md#float64) value to an outputtable string.

Returns:

- [String](core_package_structs.md#struct-string) - The converted string.

Example:

<!-- verify -->
```cangjie
main() {
    // Test Float64.toString method
    let value1: Float64 = 1.5f64
    let value2: Float64 = 123.456f64
    let value3: Float64 = -0.001f64
    
    // Convert to string
    let str1 = value1.toString()
    let str2 = value2.toString()
    let str3 = value3.toString()
    
    println("1.5f64 converted to string: \"${str1}\"")
    println("123.456f64 converted to string: \"${str2}\"")
    println("-0.001f64 converted to string: \"${str3}\"")
    
    // Verify type
    println("Is converted type String: ${str1 is String}")
}
```

Execution result:

```text
1.5f64 converted to string: "1.500000"
123.456f64 converted to string: "123.456000"
-0.001f64 converted to string: "-0.001000"
Is converted type String: true
```

## extend Int16 <: Comparable\<Int16>

```cangjie
extend Int16 <: Comparable<Int16>
```

Function: Extends the [Int16](core_package_intrinsics.md#int16) type with the [Comparable](core_package_interfaces.md#interface-comparablet)\<[Int16](core_package_intrinsics.md#int16)> interface to support comparison operations.

Extended type:

- [Int16](core_package_intrinsics.md#int16)

Extended interfaces:

- Comparable\<Int16>

### func compare(Int16)

```cangjie
public func compare(rhs: Int16): Ordering
```

Function: Determines the relationship between the current [Int16](core_package_intrinsics.md#int16) value and the specified [Int16](core_package_intrinsics.md#int16) value.

Parameters:

- rhs: [Int16](core_package_intrinsics.md#int16) - Another [Int16](core_package_intrinsics.md#int16) value to compare.

Returns:

- [Ordering](core_package_enums.md#enum-ordering) - Returns [Ordering](core_package_enums.md#enum-ordering).GT if greater, [Ordering](core_package_enums.md#enum-ordering).EQ if equal, or [Ordering](core_package_enums.md#enum-ordering).LT if less.

Example:

<!-- verify -->
```cangjie
main() {
    var num1: Int16 = 2
    var num2: Int16 = 3
    println(num1.compare(num2))
}
```

Output:

```text
Ordering.LT
```

## extend Int16 <: Countable\<Int16>

```cangjie
extend Int16 <: Countable<Int16>
```

Function: Extends the [Int16](core_package_intrinsics.md#int16) type with the [Countable](core_package_interfaces.md#interface-countablet)\<[Int16](core_package_intrinsics.md#int16)> interface to support counting operations.

Extended type:

- [Int16](core_package_intrinsics.md#int16)

Extended interfaces:

- Countable\<Int16>

### func next(Int64)

```cangjie
public func next(right: Int64): Int16
```

Function: Gets the [Int16](core_package_intrinsics.md#int16) value at the position `right` units to the right of the current [Int16](core_package_intrinsics.md#int16) position on the number axis. If overflow occurs, counting continues from the leftmost end of the axis.

Parameters:

- right: [Int64](core_package_intrinsics.md#int64) - The number of units to move right.

Returns:

- [Int16](core_package_intrinsics.md#int16) - The [Int16](core_package_intrinsics.md#int16) value at the new position.

Example:

<!-- verify -->
```cangjie
main() {
    var num1: Int16 = 32767
    var num2: Int16 = 3
    println(num1.next(5))
    println(num2.next(10))
}
```

Output:

```text
-32764
13
```

### func position()

```cangjie
public func position(): Int64
```

Function: Gets the position information of the current [Int16](core_package_intrinsics.md#int16) value, i.e., converts the [Int16](core_package_intrinsics.md#int16) to an [Int64](core_package_intrinsics.md#int64) value.

Returns:

- [Int64](core_package_intrinsics.md#int64) - The position information of the current [Int16](core_package_intrinsics.md#int16) value.

Example:

<!-- verify -->
```cangjie
main() {
    var num1: Int16 = 32767
    var num2: Int16 = 3
    println(num1.position())
    println(num2.position())
}
```

Output:

```text
32767
3
```

## extend Int16 <: Hashable

```cangjie
extend Int16 <: Hashable
```

Function: Extends the [Int16](core_package_intrinsics.md#int16) type with the [Hashable](core_package_interfaces.md#interface-hashable) interface to support hash value computation, and extends the type to support certain mathematical constants.

Extended type:

- [Int16](core_package_intrinsics.md#int16)

Extended interfaces:

- Hashable

### static prop Max

```cangjie
public static prop Max: Int16
```

Function: Gets the maximum value of a 16-bit signed integer.

Type: [Int16](./core_package_intrinsics.md#int16)

Example:

<!-- verify -->
```cangjie
main() {
    // Test Int16.Max property
    let maxValue = Int16.Max
    println("Int16.Max value: ${maxValue}")
    
    // Verify it is indeed the maximum value
    println("Is Int16.Max equal to 32767: ${maxValue == 32767}")
}
```

Output:

```text
Int16.Max value: 32767
Is Int16.Max equal to 32767: true
```

### static prop Min

```cangjie
public static prop Min: Int16
```

Function: Gets the minimum value of a 16-bit signed integer.

Type: [Int16](./core_package_intrinsics.md#int16)

Example:

<!-- verify -->
```cangjie
main() {
    // Test Int16.Min property
    let minValue = Int16.Min
    println("Int16.Min value: ${minValue}")
    
    // Verify it is indeed the minimum value
    println("Is Int16.Min equal to -32768: ${minValue == -32768}")
}
```

Output:

```text
Int16.Min value: -32768
Is Int16.Min equal to -32768: true
```

### func hashCode()

```cangjie
public func hashCode(): Int64
```

Function: Computes the hash value.

Returns:

- [Int64](core_package_intrinsics.md#int64) - The hash value.

Example:

<!-- verify -->
```cangjie
main() {
    // Test Int16.hashCode method
    let value1: Int16 = 42
    let value2: Int16 = 42
    let value3: Int16 = -100
    
    // Compute hash values
    let hash1 = value1.hashCode()
    let hash2 = value2.hashCode()
    let hash3 = value3.hashCode()
    
    println("Hash value of 42: ${hash1}")
    println("Hash value of 42: ${hash2}")
    println("Hash value of -100: ${hash3}")
    
    // Verify equal values have the same hash
    println("Do equal values have the same hash: ${hash1 == hash2}")
}
```

Output:

```text
Hash value of 42: 42
Hash value of 42: 42
Hash value of -100: -100
Do equal values have the same hash: true
```

## extend Int16 <: ToString

```cangjie
extend Int16 <: ToString
```

Function: Extends the [Int16](core_package_intrinsics.md#int16) type with the [ToString](core_package_interfaces.md#interface-tostring) interface to support conversion to [String](core_package_structs.md#struct-string).

Extended type:

- [Int16](core_package_intrinsics.md#int16)

Extended interfaces:

- ToString

### func toString()

```cangjie
public func toString(): String
```

Function: Converts the [Int16](core_package_intrinsics.md#int16) value to an output-ready string.

Returns:

- [String](core_package_structs.md#struct-string) - The converted string.

Example:

<!-- verify -->
```cangjie
main() {
    // Test Int16.toString method
    let value1: Int16 = 42
    let value2: Int16 = -123
    let value3: Int16 = 0
    
    // Convert to strings
    let str1 = value1.toString()
    let str2 = value2.toString()
    let str3 = value3.toString()
    
    println("42 converted to string: \"${str1}\"")
    println("-123 converted to string: \"${str2}\"")
    println("0 converted to string: \"${str3}\"")
    
    // Verify type
    println("Is converted type String: ${str1 is String}")
}
```

Output:

```text
42 converted to string: "42"
-123 converted to string: "-123"
0 converted to string: "0"
Is converted type String: true
```

## extend Int32 <: Comparable\<Int32>

```cangjie
extend Int32 <: Comparable<Int32>
```

Function: Extends the [Int32](core_package_intrinsics.md#int32) type with the [Comparable](core_package_interfaces.md#interface-comparablet)\<[Int32](core_package_intrinsics.md#int32)> interface to support comparison operations.

Extended type:

- [Int32](core_package_intrinsics.md#int32)

Extended interfaces:

- Comparable\<Int32>

### func compare(Int32)

```cangjie
public func compare(rhs: Int32): Ordering
```

Function: Determines the relationship between the current [Int32](core_package_intrinsics.md#int32) value and the specified [Int32](core_package_intrinsics.md#int32) value.

Parameters:

- rhs: [Int32](core_package_intrinsics.md#int32) - Another [Int32](core_package_intrinsics.md#int32) value to compare.

Returns:

- [Ordering](core_package_enums.md#enum-ordering) - Returns [Ordering](core_package_enums.md#enum-ordering).GT if greater, [Ordering](core_package_enums.md#enum-ordering).EQ if equal, or [Ordering](core_package_enums.md#enum-ordering).LT if less.

Example:

<!-- verify -->
```cangjie
main() {
    var num1: Int32 = 8
    var num2: Int32 = 10
    println(num1.compare(num2))
}
```

Output:

```text
Ordering.LT
```

## extend Int32 <: Countable\<Int32>

```cangjie
extend Int32 <: Countable<Int32>
```

Function: Extends the [Int32](core_package_intrinsics.md#int32) type with the [Countable](core_package_interfaces.md#interface-countablet)\<[Int32](core_package_intrinsics.md#int32)> interface to support counting operations.

Extended type:

- [Int32](core_package_intrinsics.md#int32)

Extended interfaces:

- Countable\<Int32>

### func next(Int64)

```cangjie
public func next(right: Int64): Int32
```

Function: Gets the [Int32](core_package_intrinsics.md#int32) value at the position `right` units to the right of the current [Int32](core_package_intrinsics.md#int32) position on the number axis. If overflow occurs, counting continues from the leftmost end of the axis.

Parameters:

- right: [Int64](core_package_intrinsics.md#int64) - The number of units to move right.

Returns:

- [Int32](core_package_intrinsics.md#int32) - The [Int32](core_package_intrinsics.md#int32) value at the new position.

Example:

<!-- verify -->
```cangjie
main() {
    var num: Int32 = 3
    println(num.next(10))
}
```

Output:

```text
13
```

### func position()

```cangjie
public func position(): Int64
```

Function: Gets the position information of the current [Int32](core_package_intrinsics.md#int32) value, i.e., converts the [Int32](core_package_intrinsics.md#int32) to an [Int64](core_package_intrinsics.md#int64) value.

Returns:

- [Int64](core_package_intrinsics.md#int64) - The position information of the current [Int32](core_package_intrinsics.md#int32) value.

Example:

<!-- verify -->
```cangjie
main() {
    var num: Int32 = 3
    println(num.position())
}
```

Output:

```text
3
```

## extend Int32 <: Hashable

```cangjie
extend Int32 <: Hashable
```

Function: Extends the [Int32](core_package_intrinsics.md#int32) type with the [Hashable](core_package_interfaces.md#interface-hashable) interface to support hash value computation, and extends the type to support certain mathematical constants.

Extended type:

- [Int32](core_package_intrinsics.md#int32)

Extended interfaces:

- Hashable

### static prop Max

```cangjie
public static prop Max: Int32
```

Function: Gets the maximum value of a 32-bit signed integer.

Type: [Int32](./core_package_intrinsics.md#int32)

Example:

<!-- verify -->
```cangjie
main() {
    // Test Int32.Max property
    let maxValue = Int32.Max
    println("Int32.Max value: ${maxValue}")
    
    // Verify it is indeed the maximum value
    println("Is Int32.Max equal to 2147483647: ${maxValue == 2147483647}")
}
```

Output:

```text
Int32.Max value: 2147483647
Is Int32.Max equal to 2147483647: true
```

### static prop Min

```cangjie
public static prop Min: Int32
```

Function: Gets the minimum value of a 32-bit signed integer.

Type: [Int32](./core_package_intrinsics.md#int32)

Example:

<!-- verify -->
```cangjie
main() {
    // Test Int32.Min property
    let minValue = Int32.Min
    println("Int32.Min value: ${minValue}")
    
    // Verify it is indeed the minimum value
    println("Is Int32.Min equal to -2147483648: ${minValue == -2147483648}")
}
```

Output:

```text
Int32.Min value: -2147483648
Is Int32.Min equal to -2147483648: true
```

### func hashCode()

```cangjie
public func hashCode(): Int64
```

Function: Computes the hash value.

Returns:

- [Int64](core_package_intrinsics.md#int64) - The hash value.

Example:

<!-- verify -->
```cangjie
main() {
    // Test Int32.hashCode method
    let value1: Int32 = 42
    let value2: Int32 = 42
    let value3: Int32 = -100
    
    // Compute hash values
    let hash1 = value1.hashCode()
    let hash2 = value2.hashCode()
    let hash3 = value3.hashCode()
    
    println("Hash value of 42: ${hash1}")
    println("Hash value of 42: ${hash2}")
    println("Hash value of -100: ${hash3}")
    
    // Verify equal values have the same hash
    println("Do equal values have the same hash: ${hash1 == hash2}")
}
```

Output:

```text
Hash value of 42: 42
Hash value of 42: 42
Hash value of -100: -100
Do equal values have the same hash: true
```

## extend Int32 <: ToString

```cangjie
extend Int32 <: ToString
```

Function: Extends the [Int32](core_package_intrinsics.md#int32) type with the [ToString](core_package_interfaces.md#interface-tostring) interface to support conversion to [String](core_package_structs.md#struct-string).

Extended type:

- [Int32](core_package_intrinsics.md#int32)

Extended interfaces:

- ToString

### func toString()

```cangjie
public func toString(): String
```

Function: Converts the [Int32](core_package_intrinsics.md#int32) value to an output-ready string.

Returns:

- [String](core_package_structs.md#struct-string) - The converted string.

Example:

<!-- verify -->
```cangjie
main() {
    // Test Int32.toString method
    let value1: Int32 = 42
    let value2: Int32 = -123
    let value3: Int32 = 0
    
    // Convert to strings
    let str1 = value1.toString()
    let str2 = value2.toString()
    let str3 = value3.toString()
    
    println("42 converted to string: \"${str1}\"")
    println("-123 converted to string: \"${str2}\"")
    println("0 converted to string: \"${str3}\"")
    
    // Verify type
    println("Is converted type String: ${str1 is String}")
}
```

Output:

```text
42 converted to string: "42"
-123 converted to string: "-123"
0 converted to string: "0"
Is converted type String: true
```

## extend Int64

```cangjie
extend Int64
```

Function: Extends the [Int64](../../core/core_package_api/core_package_intrinsics.md#int64) type as the left operand and the [Duration](../../core/core_package_api/core_package_structs.md#struct-duration) type as the right operand for multiplication.

Extended type:

- [Int64](../../core/core_package_api/core_package_intrinsics.md#int64)

### operator func *(Duration)

```cangjie
public operator func *(r: Duration): Duration
```

Function: Implements multiplication between [Int64](../../core/core_package_api/core_package_intrinsics.md#int64) and [Duration](../../core/core_package_api/core_package_structs.md#struct-duration) types, i.e., the [Int64](../../core/core_package_api/core_package_intrinsics.md#int64) * [Duration](../../core/core_package_api/core_package_structs.md#struct-duration) operation.

For example, `2 * [Duration](../../core/core_package_api/core_package_structs.md#struct-duration).second` returns a [Duration](../../core/core_package_api/core_package_structs.md#struct-duration) instance representing a time interval of 2 seconds.

Parameters:

- r: [Duration](../../core/core_package_api/core_package_structs.md#struct-duration) - The right operand of the multiplication.

Returns:

- [Duration](../../core/core_package_api/core_package_structs.md#struct-duration) - The product of the [Int64](../../core/core_package_api/core_package_intrinsics.md#int64) instance and `r`.

Exceptions:

- [ArithmeticException](../../core/core_package_api/core_package_exceptions.md#class-arithmeticexception) - Thrown when the multiplication result exceeds the representable range of [Duration](../../core/core_package_api/core_package_structs.md#struct-duration).

Example:

<!-- verify -->
```cangjie
main() {
    // Test Int64 * Duration operation
    let intVal: Int64 = 3
    let duration: Duration = Duration.second
    
    let result = intVal * duration
    
    println("Int64 value: ${intVal}")
    println("Duration value: ${duration}")
    println("Operation result: ${result}")
    
    // Test example of 2 * Duration.second
    let result2 = 2 * Duration.second
    println("2 * Duration.second = ${result2}")
}
```

Output:

```text
Int64 value: 3
Duration value: 1s
Operation result: 3s
2 * Duration.second = 2s
```

## extend Int64 <: Comparable\<Int64>

```cangjie
extend Int64 <: Comparable<Int64>
```

Function: Extends the [Int64](core_package_intrinsics.md#int64) type with the [Comparable](core_package_interfaces.md#interface-comparablet)\<[Int64](core_package_intrinsics.md#int64)> interface to support comparison operations.

Extended type:

- [Int64](core_package_intrinsics.md#int64)

Extended interfaces:

- Comparable\<Int64>

### func compare(Int64)

```cangjie
public func compare(rhs: Int64): Ordering
```

Function: Determines the relationship between the current [Int64](core_package_intrinsics.md#int64) value and the specified [Int64](core_package_intrinsics.md#int64) value.

Parameters:

- rhs: [Int64](core_package_intrinsics.md#int64) - Another [Int64](core_package_intrinsics.md#int64) value to compare.

Returns:

- [Ordering](core_package_enums.md#enum-ordering) - Returns [Ordering](core_package_enums.md#enum-ordering).GT if greater; [Ordering](core_package_enums.md#enum-ordering).EQ if equal; [Ordering](core_package_enums.md#enum-ordering).LT if less.

Example:

<!-- verify -->
```cangjie
main() {
    var num1: Int64 = 2
    var num2: Int64 = 3
    println(num1.compare(num2))
}
```

Output:

```text
Ordering.LT
```

## extend Int64 <: Countable\<Int64>

```cangjie
extend Int64 <: Countable<Int64>
```

Function: Extends the [Int64](core_package_intrinsics.md#int64) type with the [Countable](core_package_interfaces.md#interface-countablet)\<[Int64](core_package_intrinsics.md#int64)> interface to support counting operations.

Extended type:

- [Int64](core_package_intrinsics.md#int64)

Extended interfaces:

- Countable\<Int64>

### func next(Int64)

```cangjie
public func next(right: Int64): Int64
```

Function: Gets the [Int64](core_package_intrinsics.md#int32) value at the position `right` units to the right on the number axis from the current [Int64](core_package_intrinsics.md#int32) position. If overflow occurs, it continues from the leftmost position of the number axis.

Parameters:

- right: [Int64](core_package_intrinsics.md#int64) - The number of units to count to the right.

Returns:

- [Int64](core_package_intrinsics.md#int64) - The [Int64](core_package_intrinsics.md#int64) value at the position after counting `right` units to the right.

Example:

<!-- verify -->
```cangjie
main() {
    var num: Int64 = 3
    println(num.next(10))
}
```

Output:

```text
13
```

### func position()

```cangjie
public func position(): Int64
```

Function: Gets the position information of the current [Int64](core_package_intrinsics.md#int64) value, i.e., returns the [Int64](core_package_intrinsics.md#int64) value itself.

Returns:

- [Int64](core_package_intrinsics.md#int64) - The position information of the current [Int64](core_package_intrinsics.md#int64) value.

Example:

<!-- verify -->
```cangjie
main() {
    var num: Int64 = 3
    println(num.position())
}
```

Output:

```text
3
```

## extend Int64 <: Hashable

```cangjie
extend Int64 <: Hashable
```

Function: Extends the [Int64](core_package_intrinsics.md#int64) type with the [Hashable](core_package_interfaces.md#interface-hashable) interface to support hash value computation, and extends the type to support certain mathematical constants.

Extended type:

- [Int64](core_package_intrinsics.md#int64)

Extended interfaces:

- Hashable

### static prop Max

```cangjie
public static prop Max: Int64
```

Function: Gets the maximum value of a 64-bit signed integer.

Type: [Int64](./core_package_intrinsics.md#int64)

Example:

<!-- verify -->
```cangjie
main() {
    // Test Int64.Max property
    let maxValue = Int64.Max
    println("Int64.Max value: ${maxValue}")
    
    // Verify it is indeed the maximum value
    println("Is Int64.Max equal to 9223372036854775807: ${maxValue == 9223372036854775807}")
}
```

Output:

```text
Int64.Max value: 9223372036854775807
Is Int64.Max equal to 9223372036854775807: true
```

### static prop Min

```cangjie
public static prop Min: Int64
```

Function: Gets the minimum value of a 64-bit signed integer.

Type: [Int64](./core_package_intrinsics.md#int64)

Example:

<!-- verify -->
```cangjie
main() {
    // Test Int64.Min property
    let minValue = Int64.Min
    println("Int64.Min value: ${minValue}")
    
    // Verify it is indeed the minimum value
    println("Is Int64.Min equal to -9223372036854775808: ${minValue == -9223372036854775808}")
}
```

Output:

```text
Int64.Min value: -9223372036854775808
Is Int64.Min equal to -9223372036854775808: true
```

### func hashCode()

```cangjie
public func hashCode(): Int64
```

Function: Gets the hash value.

Returns:

- [Int64](core_package_intrinsics.md#int64) - The hash value.

Example:

<!-- verify -->
```cangjie
main() {
    // Test Int64.hashCode() method
    let num: Int64 = 42
    let hash = num.hashCode()
    println("Int64 value: ${num}")
    println("hashCode: ${hash}")
    
    // Test hash codes for different values
    let negativeNum: Int64 = -42
    let negativeHash = negativeNum.hashCode()
    println("Int64 value: ${negativeNum}")
    println("hashCode: ${negativeHash}")
    
    // Test zero value
    let zero: Int64 = 0
    let zeroHash = zero.hashCode()
    println("Int64 value: ${zero}")
    println("hashCode: ${zeroHash}")
}
```

Output:

```text
Int64 value: 42
hashCode: 42
Int64 value: -42
hashCode: -42
Int64 value: 0
hashCode: 0
```

## extend Int64 <: ToString

```cangjie
extend Int64 <: ToString
```

Function: Extends the [Int64](core_package_intrinsics.md#int64) type with the [ToString](core_package_interfaces.md#interface-tostring) interface to support conversion to [String](core_package_structs.md#struct-string) type.

Extended type:

- [Int64](core_package_intrinsics.md#int64)

Extended interfaces:

- ToString

### func toString()

```cangjie
public func toString(): String
```

Function: Converts the [Int64](core_package_intrinsics.md#int64) value to an output string.

Returns:

- [String](core_package_structs.md#struct-string) - The converted string.

Example:

<!-- verify -->
```cangjie
main() {
    // Test Int64.toString() method
    let num: Int64 = 42
    let str = num.toString()
    println("Int64 value: ${num}")
    println("Converted string: ${str}")
    
    // Test negative number
    let negativeNum: Int64 = -42
    let negativeStr = negativeNum.toString()
    println("Int64 value: ${negativeNum}")
    println("Converted string: ${negativeStr}")
    
    // Test zero value
    let zero: Int64 = 0
    let zeroStr = zero.toString()
    println("Int64 value: ${zero}")
    println("Converted string: ${zeroStr}")
    
    // Test maximum and minimum values
    let maxVal = Int64.Max
    let maxStr = maxVal.toString()
    println("Int64 maximum value: ${maxVal}")
    println("Converted string: ${maxStr}")
    
    let minVal = Int64.Min
    let minStr = minVal.toString()
    println("Int64 minimum value: ${minVal}")
    println("Converted string: ${minStr}")
}
```

Output:

```text
Int64 value: 42
Converted string: 42
Int64 value: -42
Converted string: -42
Int64 value: 0
Converted string: 0
Int64 maximum value: 9223372036854775807
Converted string: 9223372036854775807
Int64 minimum value: -9223372036854775808
Converted string: -9223372036854775808
```

## extend Int8 <: Comparable\<Int8>

```cangjie
extend Int8 <: Comparable<Int8>
```

Function: Extends the [Int8](core_package_intrinsics.md#int8) type with the [Comparable](core_package_interfaces.md#interface-comparablet)\<[Int8](core_package_intrinsics.md#int8)> interface to support comparison operations.

Extended type:

- [Int8](core_package_intrinsics.md#int8)

Extended interfaces:

- Comparable\<Int8>

### func compare(Int8)

```cangjie
public func compare(rhs: Int8): Ordering
```

Function: Determines the relationship between the current [Int8](core_package_intrinsics.md#int8) value and the specified [Int8](core_package_intrinsics.md#int8) value.

Parameters:

- rhs: [Int8](core_package_intrinsics.md#int8) - Another [Int8](core_package_intrinsics.md#int8) value to compare.

Returns:

- [Ordering](core_package_enums.md#enum-ordering) - Returns [Ordering](core_package_enums.md#enum-ordering).GT if greater; [Ordering](core_package_enums.md#enum-ordering).EQ if equal; [Ordering](core_package_enums.md#enum-ordering).LT if less.

Example:

<!-- verify -->
```cangjie
main() {
    var num1: Int8 = 2
    var num2: Int8 = 3
    println(num1.compare(num2))
}
```

Output:

```text
Ordering.LT
```

## extend Int8 <: Countable\<Int8>

```cangjie
extend Int8 <: Countable<Int8>
```

Function: Extends the [Int8](core_package_intrinsics.md#int8) type with the [Countable](core_package_interfaces.md#interface-countablet)\<[Int8](core_package_intrinsics.md#int8)> interface to support counting operations.

Extended type:

- [Int8](core_package_intrinsics.md#int8)

Extended interfaces:

- Countable\<Int8>

### func next(Int64)

```cangjie
public func next(right: Int64): Int8
```

Function: Gets the [Int8](core_package_intrinsics.md#int32) value at the position `right` units to the right on the number axis from the current [Int8](core_package_intrinsics.md#int32) position. If overflow occurs, it continues from the leftmost position of the number axis.

Parameters:

- right: [Int64](core_package_intrinsics.md#int64) - The number of units to count to the right.

Returns:

- [Int8](core_package_intrinsics.md#int8) - The [Int8](core_package_intrinsics.md#int8) value at the position after counting `right` units to the right.

Example:

<!-- verify -->
```cangjie
main() {
    var num: Int8 = 3
    println(num.next(5))
}
```

Output:

```text
8
```

### func position()

```cangjie
public func position(): Int64
```

Function: Gets the position information of the current [Int8](core_package_intrinsics.md#int8) value, i.e., converts the [Int8](core_package_intrinsics.md#int8) value to [Int64](core_package_intrinsics.md#int64).

Returns:

- [Int64](core_package_intrinsics.md#int64) - The position information of the current [Int8](core_package_intrinsics.md#int8) value.

Example:

<!-- verify -->
```cangjie
main() {
    var num: Int8 = 3
    println(num.position())
}
```

Output:

```text
3
```

## extend Int8 <: Hashable

```cangjie
extend Int8 <: Hashable
```

Function: Extends the [Int8](core_package_intrinsics.md#int8) type with the [Hashable](core_package_interfaces.md#interface-hashable) interface to support hash value computation, and extends the type to support certain mathematical constants.

Extended type:

- [Int8](core_package_intrinsics.md#int8)

Extended interfaces:

- Hashable

### static prop Max

```cangjie
public static prop Max: Int8
```

Function: Gets the maximum value of an 8-bit signed integer.

Type: [Int8](./core_package_intrinsics.md#int8)

Example:

<!-- verify -->
```cangjie
main() {
    // Test Int8.Max property
    let maxValue = Int8.Max
    println("Int8.Max value: ${maxValue}")
    
    // Verify it is indeed the maximum value
    println("Is Int8.Max equal to 127: ${maxValue == 127}")
}
```

Output:

```text
Int8.Max value: 127
Is Int8.Max equal to 127: true
```

### static prop Min

```cangjie
public static prop Min: Int8
```

Function: Gets the minimum value of an 8-bit signed integer.

Type: [Int8](./core_package_intrinsics.md#int8)

Example:

<!-- verify -->
```cangjie
main() {
    // Test Int8.Min property
    let minValue = Int8.Min
    println("Int8.Min value: ${minValue}")
    
    // Verify it is indeed the minimum value
    println("Is Int8.Min equal to -128: ${minValue == -128}")
}
```

Output:

```text
Int8.Min value: -128
Is Int8.Min equal to -128: true
```

### func hashCode()

```cangjie
public func hashCode(): Int64
```

Function: Gets the hash value.

Returns:

- [Int64](core_package_intrinsics.md#int64) - The hash value.

Example:

<!-- verify -->
```cangjie
main() {
    // Test Int8.hashCode() method
    let num: Int8 = 42
    let hash = num.hashCode()
    println("Int8 value: ${num}")
    println("hashCode: ${hash}")
    
    // Test hash codes for different values
    let negativeNum: Int8 = -42
    let negativeHash = negativeNum.hashCode()
    println("Int8 value: ${negativeNum}")
    println("hashCode: ${negativeHash}")
    
    // Test zero value
    let zero: Int8 = 0
    let zeroHash = zero.hashCode()
    println("Int8 value: ${zero}")
    println("hashCode: ${zeroHash}")
    
    // Test maximum and minimum values
    let maxVal = Int8.Max
    let maxHash = maxVal.hashCode()
    println("Int8 maximum value: ${maxVal}")
    println("hashCode: ${maxHash}")
    
    let minVal = Int8.Min
    let minHash = minVal.hashCode()
    println("Int8 minimum value: ${minVal}")
    println("hashCode: ${minHash}")
}
```

Output:

```text
Int8 value: 42
hashCode: 42
Int8 value: -42
hashCode: -42
Int8 value: 0
hashCode: 0
Int8 maximum value: 127
hashCode: 127
Int8 minimum value: -128
hashCode: -128
```

## extend Int8 <: ToString

```cangjie
extend Int8 <: ToString
```

Function: Extends the [Int8](core_package_intrinsics.md#int8) type with the [ToString](core_package_interfaces.md#interface-tostring) interface to support conversion to [String](core_package_structs.md#struct-string) type.

Extended type:

- [Int8](core_package_intrinsics.md#int8)

Extended interfaces:

- ToString

### func toString()

```cangjie
public func toString(): String
```

Function: Converts the [Int8](core_package_intrinsics.md#int8) value to an output string.

Returns:

- [String](core_package_structs.md#struct-string) - The converted string.

Example:

<!-- verify -->
```cangjie
main() {
    // Test Int8.toString() method
    let num: Int8 = 42
    let str = num.toString()
    println("Int8 value: ${num}")
    println("Converted string: ${str}")
    
    // Test negative number
    let negativeNum: Int8 = -42
    let negativeStr = negativeNum.toString()
    println("Int8 value: ${negativeNum}")
    println("Converted string: ${negativeStr}")
    
    // Test zero value
    let zero: Int8 = 0
    let zeroStr = zero.toString()
    println("Int8 value: ${zero}")
    println("Converted to string: ${zeroStr}")
    
    // Testing maximum and minimum values
    let maxVal = Int8.Max
    let maxStr = maxVal.toString()
    println("Int8 maximum value: ${maxVal}")
    println("Converted to string: ${maxStr}")
    
    let minVal = Int8.Min
    let minStr = minVal.toString()
    println("Int8 minimum value: ${minVal}")
    println("Converted to string: ${minStr}")
}
```

Execution results:

```text
Int8 value: 42
Converted to string: 42
Int8 value: -42
Converted to string: -42
Int8 value: 0
Converted to string: 0
Int8 maximum value: 127
Converted to string: 127
Int8 minimum value: -128
Converted to string: -128
```

## extend IntNative <: Comparable\<IntNative>

```cangjie
extend IntNative <: Comparable<IntNative>
```

Function: Extends the [IntNative](core_package_intrinsics.md#intnative) type with the [Comparable](core_package_interfaces.md#interface-comparablet)\<[IntNative](core_package_intrinsics.md#intnative)> interface to support comparison operations.

Extended type:

- [IntNative](core_package_intrinsics.md#intnative)

Extended interfaces:

- Comparable\<IntNative>

### func compare(IntNative)

```cangjie
public func compare(rhs: IntNative): Ordering
```

Function: Determines the size relationship between the current [IntNative](core_package_intrinsics.md#intnative) value and the specified [IntNative](core_package_intrinsics.md#intnative) value.

Parameters:

- rhs: [IntNative](core_package_intrinsics.md#intnative) - Another [IntNative](core_package_intrinsics.md#intnative) value to compare.

Returns:

- [Ordering](core_package_enums.md#enum-ordering) - Returns [Ordering](core_package_enums.md#enum-ordering).GT if greater, [Ordering](core_package_enums.md#enum-ordering).EQ if equal, and [Ordering](core_package_enums.md#enum-ordering).LT if less.

Example:

<!-- verify -->
```cangjie
main() {
    var num1: IntNative = 8
    var num2: IntNative = 10
    println(num1.compare(num2))
}
```

Output:

```text
Ordering.LT
```

## extend IntNative <: Countable\<IntNative>

```cangjie
extend IntNative <: Countable<IntNative>
```

Function: Extends the [IntNative](core_package_intrinsics.md#intnative) type with the [Countable](core_package_interfaces.md#interface-countablet)\<[IntNative](core_package_intrinsics.md#intnative)> interface to support counting operations.

Extended type:

- [IntNative](core_package_intrinsics.md#intnative)

Extended interfaces:

- Countable\<IntNative>

### func next(Int64)

```cangjie
public func next(right: Int64): IntNative
```

Function: Gets the [IntNative](core_package_intrinsics.md#int32) value at the position `right` units to the right of the current [IntNative](core_package_intrinsics.md#int32) value on the number line. If overflow occurs, counting continues from the leftmost position of the number line.

Parameters:

- right: [Int64](core_package_intrinsics.md#int64) - The number of units to count to the right.

Returns:

- [IntNative](core_package_intrinsics.md#intnative) - The [IntNative](core_package_intrinsics.md#intnative) value at the position `right` units to the right.

Example:

<!-- verify -->
```cangjie
main() {
    var num: IntNative = 8
    println(num.next(4))
}
```

Output:

```text
12
```

### func position()

```cangjie
public func position(): Int64
```

Function: Gets the position information of the current [IntNative](core_package_intrinsics.md#intnative) value, i.e., converts the [IntNative](core_package_intrinsics.md#intnative) to an [Int64](core_package_intrinsics.md#int64) value.

Returns:

- [Int64](core_package_intrinsics.md#int64) - The position information of the current [IntNative](core_package_intrinsics.md#intnative) value.

Example:

<!-- verify -->
```cangjie
main() {
    var num: IntNative = 8
    println(num.position())
}
```

Output:

```text
8
```

## extend IntNative <: Hashable

```cangjie
extend IntNative <: Hashable
```

Function: Extends the [IntNative](core_package_intrinsics.md#intnative) type with the [Hashable](core_package_interfaces.md#interface-hashable) interface to support hash value computation, and extends the type to support certain mathematical constants.

Extended type:

- [IntNative](core_package_intrinsics.md#intnative)

Extended interfaces:

- Hashable

### static prop Max

```cangjie
public static prop Max: IntNative
```

Function: Gets the maximum value of the platform-dependent signed integer.

Type: [IntNative](./core_package_intrinsics.md#intnative)

Example:

<!-- verify -->
```cangjie
main() {
    // Test the IntNative.Max property
    let maxValue = IntNative.Max
    println("Value of IntNative.Max: ${maxValue}")
    
    // Verify it is indeed the maximum value (should be 9223372036854775807 on 64-bit systems)
    println("Is IntNative.Max equal to 9223372036854775807: ${maxValue == 9223372036854775807}")
}
```

Output:

```text
Value of IntNative.Max: 9223372036854775807
Is IntNative.Max equal to 9223372036854775807: true
```

### static prop Min

```cangjie
public static prop Min: IntNative
```

Function: Gets the minimum value of the platform-dependent signed integer.

Type: [IntNative](./core_package_intrinsics.md#intnative)

Example:

<!-- verify -->
```cangjie
main() {
    // Test the IntNative.Min property
    let minValue = IntNative.Min
    println("Value of IntNative.Min: ${minValue}")
    
    // Verify it is indeed the minimum value (should be -9223372036854775808 on 64-bit systems)
    println("Is IntNative.Min equal to -9223372036854775808: ${minValue == -9223372036854775808}")
}
```

Output:

```text
Value of IntNative.Min: -9223372036854775808
Is IntNative.Min equal to -9223372036854775808: true
```

### func hashCode()

```cangjie
public func hashCode(): Int64
```

Function: Gets the hash value.

Returns:

- [Int64](core_package_intrinsics.md#int64) - The hash value.

Example:

<!-- verify -->
```cangjie
main() {
    // Test the IntNative.hashCode() method
    let num: IntNative = 42
    let hash = num.hashCode()
    println("IntNative value: ${num}")
    println("hashCode: ${hash}")
    
    // Test hash codes for different values
    let negativeNum: IntNative = -42
    let negativeHash = negativeNum.hashCode()
    println("IntNative value: ${negativeNum}")
    println("hashCode: ${negativeHash}")
    
    // Test zero value
    let zero: IntNative = 0
    let zeroHash = zero.hashCode()
    println("IntNative value: ${zero}")
    println("hashCode: ${zeroHash}")
    
    // Test maximum and minimum values
    let maxVal = IntNative.Max
    let maxHash = maxVal.hashCode()
    println("IntNative maximum value: ${maxVal}")
    println("hashCode: ${maxHash}")
    
    let minVal = IntNative.Min
    let minHash = minVal.hashCode()
    println("IntNative minimum value: ${minVal}")
    println("hashCode: ${minHash}")
}
```

Output:

```text
IntNative value: 42
hashCode: 42
IntNative value: -42
hashCode: -42
IntNative value: 0
hashCode: 0
IntNative maximum value: 9223372036854775807
hashCode: 9223372036854775807
IntNative minimum value: -9223372036854775808
hashCode: -9223372036854775808
```

## extend IntNative <: ToString

```cangjie
extend IntNative <: ToString
```

Function: Extends the [IntNative](core_package_intrinsics.md#intnative) type with the [ToString](core_package_interfaces.md#interface-tostring) interface to support conversion to [String](core_package_structs.md#struct-string) type.

Extended type:

- [IntNative](core_package_intrinsics.md#intnative)

Extended interfaces:

- ToString

### func toString()

```cangjie
public func toString(): String
```

Function: Converts the [IntNative](core_package_intrinsics.md#intnative) value to an outputtable string.

Returns:

- [String](core_package_structs.md#struct-string) - The converted string.

Example:

<!-- verify -->
```cangjie
main() {
    // Test the IntNative.toString() method
    let num: IntNative = 42
    let str = num.toString()
    println("IntNative value: ${num}")
    println("Converted string: ${str}")
    
    // Test negative numbers
    let negativeNum: IntNative = -42
    let negativeStr = negativeNum.toString()
    println("IntNative value: ${negativeNum}")
    println("Converted string: ${negativeStr}")
    
    // Test zero value
    let zero: IntNative = 0
    let zeroStr = zero.toString()
    println("IntNative value: ${zero}")
    println("Converted string: ${zeroStr}")
    
    // Test maximum and minimum values
    let maxVal = IntNative.Max
    let maxStr = maxVal.toString()
    println("IntNative maximum value: ${maxVal}")
    println("Converted string: ${maxStr}")
    
    let minVal = IntNative.Min
    let minStr = minVal.toString()
    println("IntNative minimum value: ${minVal}")
    println("Converted string: ${minStr}")
}
```

Output:

```text
IntNative value: 42
Converted string: 42
IntNative value: -42
Converted string: -42
IntNative value: 0
Converted string: 0
IntNative maximum value: 9223372036854775807
Converted string: 9223372036854775807
IntNative minimum value: -9223372036854775808
Converted string: -9223372036854775808
```

## extend Ordering <: Comparable

```cangjie
extend Ordering <: Comparable<Ordering>
```

Function: Extends the [Ordering](core_package_enums.md#enum-ordering) type with the [Comparable](core_package_interfaces.md#interface-comparablet)\<[Ordering](core_package_enums.md#enum-ordering)> interface, enabling comparison operations.

Extended type:

- [Ordering](core_package_enums.md#enum-ordering)

Extended interfaces:

- Comparable\<Ordering>

### func compare(Ordering)

```cangjie
public func compare(that: Ordering): Ordering
```

Function: Determines the relative ordering between the current [Ordering](core_package_enums.md#enum-ordering) instance and the specified [Ordering](core_package_enums.md#enum-ordering) instance.

The ordering relationship for [Ordering](core_package_enums.md#enum-ordering) enums is: GT > EQ > LT.

Parameters:

- that: [Ordering](core_package_enums.md#enum-ordering) - The [Ordering](core_package_enums.md#enum-ordering) instance to compare with.

Returns:

- [Ordering](core_package_enums.md#enum-ordering) - Returns GT if greater than, EQ if equal, or LT if less than.

Example:

<!-- verify -->
```cangjie
main() {
    // Create Ordering instances
    var gt: Ordering = GT
    var eq: Ordering = EQ
    var lt: Ordering = LT
    
    // Test comparison between GT and EQ
    var result1 = gt.compare(eq)
    println("GT.compare(EQ): ${result1.toString()}")
    
    // Test comparison between EQ and LT
    var result2 = eq.compare(lt)
    println("EQ.compare(LT): ${result2.toString()}")
    
    // Test comparison between LT and GT
    var result3 = lt.compare(gt)
    println("LT.compare(GT): ${result3.toString()}")
    
    // Test equality case
    var result4 = gt.compare(gt)
    println("GT.compare(GT): ${result4.toString()}")
}
```

Execution Result:

```text
GT.compare(EQ): Ordering.GT
EQ.compare(LT): Ordering.GT
LT.compare(GT): Ordering.LT
GT.compare(GT): Ordering.EQ
```

## extend Ordering <: Hashable

```cangjie
extend Ordering <: Hashable
```

Function: Extends the [Ordering](core_package_enums.md#enum-ordering) type with the [Hashable](core_package_interfaces.md#interface-hashable) interface, enabling hash value computation.

Extended type:

- [Ordering](core_package_enums.md#enum-ordering)

Extended interfaces:

- Hashable

### func hashCode()

```cangjie
public func hashCode(): Int64
```

Function: Computes the hash value. GT has a hash value of 3, EQ has 2, and LT has 1.

Returns:

- [Int64](core_package_intrinsics.md#int64) - The computed hash value.

Example:

<!-- verify -->
```cangjie
main() {
    // Create Ordering instances
    var gt: Ordering = GT
    var eq: Ordering = EQ
    var lt: Ordering = LT
    
    // Compute hash values
    var gtHash = gt.hashCode()
    var eqHash = eq.hashCode()
    var ltHash = lt.hashCode()
    
    println("GT hash value: ${gtHash}")
    println("EQ hash value: ${eqHash}")
    println("LT hash value: ${ltHash}")
}
```

Execution Result:

```text
GT hash value: 3
EQ hash value: 2
LT hash value: 1
```

## extend Ordering <: ToString

```cangjie
extend Ordering <: ToString
```

Function: Extends the [Ordering](core_package_enums.md#enum-ordering) type with the [ToString](core_package_interfaces.md#interface-tostring) interface, enabling string conversion.

Extended type:

- [Ordering](core_package_enums.md#enum-ordering)

Extended interfaces:

- ToString

### func toString()

```cangjie
public func toString(): String
```

Function: Converts the [Ordering](core_package_enums.md#enum-ordering) instance to a printable string.

Conversion results are as follows:

- GT: "[Ordering](core_package_enums.md#enum-ordering).GT".
- LT: "[Ordering](core_package_enums.md#enum-ordering).ET".
- EQ: "[Ordering](core_package_enums.md#enum-ordering).EQ".

Returns:

- [String](core_package_structs.md#struct-string) - The converted string.

Example:

<!-- verify -->
```cangjie
main() {
    // Create Ordering instances
    var gt: Ordering = GT
    var eq: Ordering = EQ
    var lt: Ordering = LT
    
    // Convert to strings
    var gtStr = gt.toString()
    var eqStr = eq.toString()
    var ltStr = lt.toString()
    
    println("GT as string: ${gtStr}")
    println("EQ as string: ${eqStr}")
    println("LT as string: ${ltStr}")
}
```

Execution Result:

```text
GT as string: Ordering.GT
EQ as string: Ordering.EQ
LT as string: Ordering.LT
```

## extend Rune

```cangjie
extend Rune
```

Function: Implements a series of extension methods for the [Rune](core_package_intrinsics.md#rune) type, primarily for character judgment and conversion within the ASCII character set.

Extended type:

- [Rune](core_package_intrinsics.md#rune)

### static func fromUtf8(Array\<UInt8>, Int64)

```cangjie
public static func fromUtf8(arr: Array<UInt8>, index: Int64): (Rune, Int64)
```

Function: Converts the specified element in a byte array to a character according to UTF-8 encoding rules and returns the byte length occupied by the character.

Parameters:

- arr: [Array](core_package_structs.md#struct-arrayt)\<[UInt8](core_package_intrinsics.md#uint8)> - The byte array containing the bytes to convert.
- index: [Int64](core_package_intrinsics.md#int64) - The index of the byte to convert in the array.

Returns:

- ([Rune](core_package_intrinsics.md#rune), [Int64](core_package_intrinsics.md#int64)) - The converted character and the byte length occupied by the character.

Exceptions:

- [IllegalArgumentException](core_package_exceptions.md#class-illegalargumentexception) - Invalid UTF-8 byte sequence.

Example:

<!-- verify -->
```cangjie
main() {
    var arr: Array<UInt8> = [4, 8, 65] // A <=> 65
    var tuple = Rune.fromUtf8(arr, 2)
    println(tuple[0]) // Rune
    println(tuple[1]) // len
}
```

Output:

```text
A
1
```

### static func getPreviousFromUtf8(Array\<UInt8>, Int64)

```cangjie
public static func getPreviousFromUtf8(arr: Array<UInt8>, index: Int64): (Rune, Int64)
```

Function: Gets the UTF-8 encoded character that the byte at position `index - 1` (i.e., `arr[index - 1]`) belongs to, along with the byte length of the character.

Starting from `arr[index - 1]`, the function checks whether that byte is the leading byte of a UTF-8 character. If not, it continues searching backward until the leading byte is found, then decodes the byte sequence into the corresponding character. The valid range for `index` is `[1, arr.size]`.

Parameters:

- arr: [Array](core_package_structs.md#struct-arrayt)\<[UInt8](core_package_intrinsics.md#uint8)> - The byte array to search for the character.
- index: [Int64](core_package_intrinsics.md#int64) - The index in the byte array; the function finds the character that `arr[index - 1]` belongs to. Valid range: `[1, arr.size]`.

Returns:

- ([Rune](core_package_intrinsics.md#rune), [Int64](core_package_intrinsics.md#int64)) - The found character and the byte length of the character.

Exceptions:

- [IllegalArgumentException](core_package_exceptions.md#class-illegalargumentexception) - If `index` is less than or equal to 0, there is no preceding byte and an exception is thrown; if the leading byte cannot be found, i.e., the byte at `arr[index - 1]` does not conform to UTF-8 encoding, an exception is thrown.
- [IndexOutOfBoundsException](core_package_exceptions.md#class-indexoutofboundsexception) - If `index` is greater than `arr.size`, an exception is thrown.

Example:

<!-- verify -->
```cangjie
main() {
    // Test the Rune.getPreviousFromUtf8 method
    // Create a byte array containing a Chinese character
    var arr: Array<UInt8> = [72, 101, 108, 108, 111, 231, 136, 177] // UTF-8 encoding for "Hello爱"
    println("Byte array: ${arr}")
    
    // Get the character corresponding to index 7 (the second byte of the Chinese character "爱")
    var tuple = Rune.getPreviousFromUtf8(arr, 7)
    println("Found character: ${tuple[0]}")
    println("Byte length of character: ${tuple[1]}")
}
```

Output:

```text
Byte array: [72, 101, 108, 108, 111, 231, 136, 177]
Found character: 爱
Byte length of character: 3
```

### static func intoUtf8Array(Rune, Array\<UInt8>, Int64)

```cangjie
public static func intoUtf8Array(c: Rune, arr: Array<UInt8>, index: Int64): Int64
```

Function: Converts a character to a byte sequence and overwrites the bytes in the specified position of the [Array](core_package_structs.md#struct-arrayt) array.

Parameters:

- c: [Rune](core_package_intrinsics.md#rune) - The character to convert.
- arr: [Array](core_package_structs.md#struct-arrayt)\<[UInt8](core_package_intrinsics.md#uint8)> - The [Array](core_package_structs.md#struct-arrayt) array to overwrite.
- index: [Int64](core_package_intrinsics.md#int64) - The starting index of the target position.

Returns:

- [Int64](core_package_intrinsics.md#int64) - The byte length of the character, e.g., 3 for Chinese characters.

Example:

<!-- verify -->
```cangjie
main() {
    var arr: Array<UInt8> = [1, 2, 3, 230, 136, 145]
    var len: Int64 = Rune.intoUtf8Array(r'爱', arr, 2)
    println(len)
    println(arr[2]) // First byte of the UTF-8 encoding for "爱"
}
```

Output:

```text
3
231
```

### static func utf8Size(Array\<UInt8>, Int64)

```cangjie
public static func utf8Size(arr: Array<UInt8>, index: Int64): Int64
```

Function: Returns the number of bytes occupied by the character starting at the specified index in the byte array.

In UTF-8 encoding, the leading byte of an ASCII character does not start with 1, while the leading byte of other characters starts with a number of 1s indicating the byte length of the character. This function determines the byte length by scanning the leading byte. If the index does not point to a leading byte, an exception is thrown.

Parameters:

- arr: [Array](core_package_structs.md#struct-arrayt)\<[UInt8](core_package_intrinsics.md#uint8)> - The byte array to get the character from.
- index: [Int64](core_package_intrinsics.md#int64) - The index of the character.

Returns:

- [Int64](core_package_intrinsics.md#int64) - The byte length of the character, e.g., 3 for Chinese characters.

Exceptions:

- [IllegalArgumentException](core_package_exceptions.md#class-illegalargumentexception) - If the byte at the index does not conform to the leading byte rules.

Example:

<!-- verify -->
```cangjie
main() {
    var arr: Array<UInt8> = [1, 2, 231, 136, 177, 145]
    var len: Int64 = Rune.utf8Size(arr, 2)
    println(len) // The array elements at indices 2-4 are the UTF-8 encoding for the Chinese character "爱", occupying 3 bytes
}
```

Output:

```text
3
```

### static func utf8Size(Rune)

```cangjie
public static func utf8Size(c: Rune): Int64
```

Function: Returns the byte length of the UTF-8 encoding for the character, e.g., 3 for Chinese characters.

Parameters:

- `c`: [Rune](core_package_intrinsics.md#rune) - The character whose UTF-8 byte length is to be calculated.

Returns:

- [Int64](core_package_intrinsics.md#int64) - The UTF-8 byte length of the character.

Example:

<!-- verify -->
```cangjie
main() {
    var char: Rune = r'爱'
    var len: Int64 = Rune.utf8Size(char)
    println(len) // The Chinese character '爱' occupies 3 bytes in UTF-8 encoding
}
```

Execution Result:

```text
3
```

### func isAscii()

```cangjie
public func isAscii(): Bool
```

Function: Determines whether the character is an ASCII character.

Returns:

- [Bool](core_package_intrinsics.md#bool) - Returns `true` if the character is ASCII, otherwise `false`.

Example:

<!-- verify -->
```cangjie
main() {
    // Testing Rune.isAscii() method
    var asciiChar: Rune = r'A'  // ASCII character
    var nonAsciiChar: Rune = r'爱'  // Non-ASCII character
    
    println("Is character '${asciiChar}' ASCII? ${asciiChar.isAscii()}")
    println("Is character '${nonAsciiChar}' ASCII? ${nonAsciiChar.isAscii()}")
}
```

Execution Result:

```text
Is character 'A' ASCII? true
Is character '爱' ASCII? false
```

### func isAsciiControl()

```cangjie
public func isAsciiControl(): Bool
```

Function: Determines whether the character is an ASCII control character. The valid range is the union of [00, 1F] and {7F}.

Returns:

- [Bool](core_package_intrinsics.md#bool) - Returns `true` if the character is an ASCII control character, otherwise `false`.

Example:

<!-- verify -->
```cangjie
main() {
    // Testing Rune.isAsciiControl() method
    var controlChar: Rune = r'\n'  // ASCII control character
    var nonControlChar: Rune = r'A'  // Non-control character
    
    println("Is character '${controlChar}' an ASCII control character? ${controlChar.isAsciiControl()}")
    println("Is character '${nonControlChar}' an ASCII control character? ${nonControlChar.isAsciiControl()}")
}
```

Execution Result:

```text
Is character '
' an ASCII control character? true
Is character 'A' an ASCII control character? false
```

### func isAsciiGraphic()

```cangjie
public func isAsciiGraphic(): Bool
```

Function: Determines whether the character is an ASCII graphic character. The valid range is [21, 7E].

Returns:

- [Bool](core_package_intrinsics.md#bool) - Returns `true` if the character is an ASCII graphic character, otherwise `false`.

Example:

<!-- verify -->
```cangjie
main() {
    // Testing Rune.isAsciiGraphic() method
    var graphicChar: Rune = r'A'  // ASCII graphic character
    var nonGraphicChar: Rune = r'\n'  // Non-graphic character (control character)
    
    println("Is character '${graphicChar}' an ASCII graphic character? ${graphicChar.isAsciiGraphic()}")
    println("Is character '${nonGraphicChar}' an ASCII graphic character? ${nonGraphicChar.isAsciiGraphic()}")
}
```

Execution Result:

```text
Is character 'A' an ASCII graphic character? true
Is character '
' an ASCII graphic character? false
```

### func isAsciiHex()

```cangjie
public func isAsciiHex(): Bool
```

Function: Determines whether the character is an ASCII hexadecimal character.

Returns:

- [Bool](core_package_intrinsics.md#bool) - Returns `true` if the character is an ASCII hexadecimal character, otherwise `false`.

Example:

<!-- verify -->
```cangjie
main() {
    // Testing Rune.isAsciiHex() method
    var hexChar: Rune = r'A'  // ASCII hexadecimal character
    var nonHexChar: Rune = r'G'  // Non-hexadecimal character
    
    println("Is character '${hexChar}' an ASCII hexadecimal character? ${hexChar.isAsciiHex()}")
    println("Is character '${nonHexChar}' an ASCII hexadecimal character? ${nonHexChar.isAsciiHex()}")
}
```

Execution Result:

```text
Is character 'A' an ASCII hexadecimal character? true
Is character 'G' an ASCII hexadecimal character? false
```

### func isAsciiLetter()

```cangjie
public func isAsciiLetter(): Bool
```

Function: Determines whether the character is an ASCII alphabetic character.

Returns:

- [Bool](core_package_intrinsics.md#bool) - Returns `true` if the character is an ASCII alphabetic character, otherwise `false`.

Example:

<!-- verify -->
```cangjie
main() {
    // Testing Rune.isAsciiLetter() method
    var letterChar: Rune = r'A'  // ASCII alphabetic character
    var nonLetterChar: Rune = r'1'  // Non-alphabetic character
    
    println("Is character '${letterChar}' an ASCII alphabetic character? ${letterChar.isAsciiLetter()}")
    println("Is character '${nonLetterChar}' an ASCII alphabetic character? ${nonLetterChar.isAsciiLetter()}")
}
```

Execution Result:

```text
Is character 'A' an ASCII alphabetic character? true
Is character '1' an ASCII alphabetic character? false
```

### func isAsciiLowerCase()

```cangjie
public func isAsciiLowerCase(): Bool
```

Function: Determines whether the character is an ASCII lowercase character.

Returns:

- [Bool](core_package_intrinsics.md#bool) - Returns `true` if the character is an ASCII lowercase character, otherwise `false`.

Example:

<!-- verify -->
```cangjie
main() {
    // Testing Rune.isAsciiLowerCase() method
    var lowerCaseChar: Rune = r'a'  // ASCII lowercase character
    var upperCaseChar: Rune = r'A'  // ASCII uppercase character
    
    println("Is character '${lowerCaseChar}' an ASCII lowercase character? ${lowerCaseChar.isAsciiLowerCase()}")
    println("Is character '${upperCaseChar}' an ASCII lowercase character? ${upperCaseChar.isAsciiLowerCase()}")
}
```

Execution Result:

```text
Is character 'a' an ASCII lowercase character? true
Is character 'A' an ASCII lowercase character? false
```

### func isAsciiNumber()

```cangjie
public func isAsciiNumber(): Bool
```

Function: Determines whether the character is an ASCII numeric character.

Returns:

- [Bool](core_package_intrinsics.md#bool) - Returns `true` if the character is an ASCII numeric character, otherwise `false`.

Example:

<!-- verify -->
```cangjie
main() {
    // Testing Rune.isAsciiNumber() method
    var numberChar: Rune = r'1'  // ASCII numeric character
    var nonNumberChar: Rune = r'A'  // Non-numeric character
    
    println("Is character '${numberChar}' an ASCII numeric character? ${numberChar.isAsciiNumber()}")
    println("Is character '${nonNumberChar}' an ASCII numeric character? ${nonNumberChar.isAsciiNumber()}")
}
```

Execution Result:

```text
Is character '1' an ASCII numeric character? true
Is character 'A' an ASCII numeric character? false
```

### func isAsciiNumberOrLetter()

```cangjie
public func isAsciiNumberOrLetter(): Bool
```

Function: Determines whether the character is an ASCII numeric or Latin alphabetic character.

Returns:

- [Bool](core_package_intrinsics.md#bool) - Returns `true` if the character is an ASCII numeric or Latin alphabetic character, otherwise `false`.

Example:

<!-- verify -->
```cangjie
main() {
    // Testing Rune.isAsciiNumberOrLetter() method
    var numberChar: Rune = r'1'  // ASCII numeric character
    var letterChar: Rune = r'A'  // ASCII alphabetic character
    var specialChar: Rune = r'!'  // Special character
    
    println("Is character '${numberChar}' an ASCII numeric or alphabetic character? ${numberChar.isAsciiNumberOrLetter()}")
    println("Is character '${letterChar}' an ASCII numeric or alphabetic character? ${letterChar.isAsciiNumberOrLetter()}")
    println("Is character '${specialChar}' an ASCII numeric or alphabetic character? ${specialChar.isAsciiNumberOrLetter()}")
}
```

Execution Result:

```text
Is character '1' an ASCII numeric or alphabetic character? true
Is character 'A' an ASCII numeric or alphabetic character? true
Is character '!' an ASCII numeric or alphabetic character? false
```

### func isAsciiOct()

```cangjie
public func isAsciiOct(): Bool
```

Function: Determines whether the character is an ASCII octal character.

Returns:

- [Bool](core_package_intrinsics.md#bool) - Returns `true` if the character is an ASCII octal character, otherwise `false`.

Example:

<!-- verify -->
```cangjie
main() {
    // Testing Rune.isAsciiOct() method
    var octChar: Rune = r'7'  // ASCII octal character
    var nonOctChar: Rune = r'8'  // Non-octal character
    
    println("Is character '${octChar}' an ASCII octal character? ${octChar.isAsciiOct()}")
    println("Is character '${nonOctChar}' an ASCII octal character? ${nonOctChar.isAsciiOct()}")
}
```

Execution Result:

```text
Is character '7' an ASCII octal character? true
Is character '8' an ASCII octal character? false
```

### func isAsciiPunctuation()

```cangjie
public func isAsciiPunctuation(): Bool
```

Function: Determines whether the character is an ASCII punctuation character. The valid range is the union of [21, 2F], [3A, 40], [5B, 60], and [7B, 7E].

Returns:

- [Bool](core_package_intrinsics.md#bool) - Returns `true` if the character is an ASCII punctuation character, otherwise `false`.

Example:

<!-- verify -->
```cangjie
main() {
    // Testing Rune.isAsciiPunctuation() method
    var punctuationChar: Rune = r'!'  // ASCII punctuation character
    var nonPunctuationChar: Rune = r'A'  // Non-punctuation character
    
    println("Is character '${punctuationChar}' an ASCII punctuation character? ${punctuationChar.isAsciiPunctuation()}")
    println("Is character '${nonPunctuationChar}' an ASCII punctuation character? ${nonPunctuationChar.isAsciiPunctuation()}")
}
```

Execution Result:

```text
Is character '!' an ASCII punctuation character? true
Is character 'A' an ASCII punctuation character? false
```

### func isAsciiUpperCase()

```cangjie
public func isAsciiUpperCase(): Bool
```

Function: Determines whether the character is an ASCII uppercase character.

Returns:

- [Bool](core_package_intrinsics.md#bool) - Returns `true` if the character is an ASCII uppercase character, otherwise `false`.

Example:

<!-- verify -->
```cangjie
main() {
    // Testing Rune.isAsciiUpperCase() method
    var upperCaseChar: Rune = r'A'  // ASCII uppercase character
    var lowerCaseChar: Rune = r'a'  // ASCII lowercase character
    
    println("Is character '${upperCaseChar}' an ASCII uppercase character? ${upperCaseChar.isAsciiUpperCase()}")
    println("Is character '${lowerCaseChar}' an ASCII uppercase character? ${lowerCaseChar.isAsciiUpperCase()}")
}
```

Execution Result:

```text
Is character 'A' an ASCII uppercase character? true
Is character 'a' an ASCII uppercase character? false
```

### func isAsciiWhiteSpace()

```cangjie
public func isAsciiWhiteSpace(): Bool
```

Function: Determines whether the character is an ASCII whitespace character. The valid range is the union of [09, 0D] and {20}.

Returns:

- [Bool](core_package_intrinsics.md#bool) - Returns `true` if the character is an ASCII whitespace character, otherwise `false`.Returns:

- [Bool](core_package_intrinsics.md#bool) - Returns true if it's an ASCII whitespace character, otherwise returns false.

Example:

<!-- verify -->
```cangjie
main() {
    // Testing Rune.isAsciiWhiteSpace() method
    var whiteSpaceChar: Rune = r' '  // ASCII whitespace character
    var nonWhiteSpaceChar: Rune = r'A'  // Non-whitespace character
    
    println("Is character '${whiteSpaceChar}' an ASCII whitespace? ${whiteSpaceChar.isAsciiWhiteSpace()}")
    println("Is character '${nonWhiteSpaceChar}' an ASCII whitespace? ${nonWhiteSpaceChar.isAsciiWhiteSpace()}")
}
```

Execution Result:

```text
Is character ' ' an ASCII whitespace? true
Is character 'A' an ASCII whitespace? false
```

### func toAsciiLowerCase()

```cangjie
public func toAsciiLowerCase(): Rune
```

Function: Converts the character to ASCII lowercase. If conversion is not possible, returns the original character unchanged.

Returns:

- [Rune](core_package_intrinsics.md#rune) - The converted character, or the original [Rune](core_package_intrinsics.md#rune) if conversion is not possible.

Example:

<!-- verify -->
```cangjie
main() {
    // Testing Rune.toAsciiLowerCase() method
    var upperCaseChar: Rune = r'A'  // ASCII uppercase character
    var lowerCaseChar: Rune = r'a'  // ASCII lowercase character
    var nonLetterChar: Rune = r'1'  // Non-letter character
    
    var convertedUpper = upperCaseChar.toAsciiLowerCase()
    var convertedLower = lowerCaseChar.toAsciiLowerCase()
    var convertedNonLetter = nonLetterChar.toAsciiLowerCase()
    
    println("Uppercase '${upperCaseChar}' converted to lowercase: ${convertedUpper}")
    println("Lowercase '${lowerCaseChar}' converted to lowercase: ${convertedLower}")
    println("Non-letter '${nonLetterChar}' converted to lowercase: ${convertedNonLetter}")
}
```

Execution Result:

```text
Uppercase 'A' converted to lowercase: a
Lowercase 'a' converted to lowercase: a
Non-letter '1' converted to lowercase: 1
```

### func toAsciiUpperCase()

```cangjie
public func toAsciiUpperCase(): Rune
```

Function: Converts the character to ASCII uppercase. If conversion is not possible, returns the original character unchanged.

Returns:

- [Rune](core_package_intrinsics.md#rune) - The converted character, or the original [Rune](core_package_intrinsics.md#rune) if conversion is not possible.

Example:

<!-- verify -->
```cangjie
main() {
    // Testing Rune.toAsciiUpperCase() method
    var lowerCaseChar: Rune = r'a'  // ASCII lowercase character
    var upperCaseChar: Rune = r'A'  // ASCII uppercase character
    var nonLetterChar: Rune = r'1'  // Non-letter character
    
    var convertedLower = lowerCaseChar.toAsciiUpperCase()
    var convertedUpper = upperCaseChar.toAsciiUpperCase()
    var convertedNonLetter = nonLetterChar.toAsciiUpperCase()
    
    println("Lowercase '${lowerCaseChar}' converted to uppercase: ${convertedLower}")
    println("Uppercase '${upperCaseChar}' converted to uppercase: ${convertedUpper}")
    println("Non-letter '${nonLetterChar}' converted to uppercase: ${convertedNonLetter}")
}
```

Execution Result:

```text
Lowercase 'a' converted to uppercase: A
Uppercase 'A' converted to uppercase: A
Non-letter '1' converted to uppercase: 1
```

## extend Rune <: Comparable\<Rune>

```cangjie
extend Rune <: Comparable<Rune>
```

Function: Extends the [Rune](core_package_intrinsics.md#rune) type with the [Comparable](core_package_interfaces.md#interface-comparablet)\<[Rune](core_package_intrinsics.md#rune)> interface to support comparison operations.

Extended type:

- [Rune](core_package_intrinsics.md#rune)

Extended interfaces:

- Comparable\<Rune>

### func compare(Rune)

```cangjie
public func compare(rhs: Rune): Ordering
```

Function: Determines the size relationship between the current [Rune](core_package_intrinsics.md#rune) instance and the specified [Rune](core_package_intrinsics.md#rune) instance.

The size relationship of [Rune](core_package_intrinsics.md#rune) refers to the comparison of their corresponding Unicode code points.

Parameters:

- rhs: [Rune](core_package_intrinsics.md#rune) - Another [Rune](core_package_intrinsics.md#rune) instance to compare with.

Returns:

- [Ordering](core_package_enums.md#enum-ordering) - Returns [Ordering](core_package_enums.md#enum-ordering).GT if greater, [Ordering](core_package_enums.md#enum-ordering).EQ if equal, and [Ordering](core_package_enums.md#enum-ordering).LT if less.

Example:

<!-- verify -->
```cangjie
main() {
    var char1: Rune = r'i'
    var char2: Rune = r'j'
    println(char1.compare(char2))
}
```

Output:

```text
Ordering.LT
```

## extend Rune <: Countable\<Rune>

```cangjie
extend Rune <: Countable<Rune>
```

Function: Extends the [Rune](core_package_intrinsics.md#rune) type with the [Countable](core_package_interfaces.md#interface-countablet)\<[Rune](core_package_intrinsics.md#rune)> interface to support counting operations.

Extended type:

- [Rune](core_package_intrinsics.md#rune)

Extended interfaces:

- Countable\<Rune>

### func next(Int64)

```cangjie
public func next(right: Int64): Rune
```

Function: Gets the [Rune](core_package_intrinsics.md#rune) value `right` positions to the right of the current [Rune](core_package_intrinsics.md#rune) value.

Parameters:

- right: [Int64](core_package_intrinsics.md#int64) - The number of positions to move right.

Returns:

- [Rune](core_package_intrinsics.md#rune) - The [Rune](core_package_intrinsics.md#rune) value `right` positions to the right.

Exceptions:

- [OverflowException](core_package_exceptions.md#class-overflowexception) - Thrown if the result of the addition with [Int64](core_package_intrinsics.md#int64) is an invalid Unicode value.

Example:

<!-- verify -->
```cangjie
main() {
    // Test Rune.next method
    var char: Rune = r'A'
    var nextChar = char.next(1)
    println("Character: ${char}")
    println("Next character: ${nextChar}")
    
    // Test moving backward
    var prevChar = char.next(-1)
    println("Previous character: ${prevChar}")
}
```

Output:

```text
Character: A
Next character: B
Previous character: @
```

### func position()

```cangjie
public func position(): Int64
```

Function: Gets the position information of the current [Rune](core_package_intrinsics.md#rune) value, i.e., converts the [Rune](core_package_intrinsics.md#rune) to an [Int64](core_package_intrinsics.md#int64) value.

Returns:

- [Int64](core_package_intrinsics.md#int64) - The position information of the current [Rune](core_package_intrinsics.md#rune) value.

Example:

<!-- verify -->
```cangjie
main() {
    // Test Rune.position method
    var char: Rune = r'A'
    var position = char.position()
    println("Character: ${char}")
    println("Unicode code point position: ${position}")
    
    // Test Chinese character
    var chineseChar: Rune = r'爱'
    var chinesePosition = chineseChar.position()
    println("Character: ${chineseChar}")
    println("Unicode code point position: ${chinesePosition}")
}
```

Output:

```text
Character: A
Unicode code point position: 65
Character: 爱
Unicode code point position: 29233
```

## extend Rune <: Hashable

```cangjie
extend Rune <: Hashable
```

Function: Extends the [Rune](core_package_intrinsics.md#rune) type with the [Hashable](core_package_interfaces.md#interface-hashable) interface to support hash value computation.

Extended type:

- [Rune](core_package_intrinsics.md#rune)

Extended interfaces:

- Hashable

### func hashCode()

```cangjie
public func hashCode(): Int64
```

Function: Gets the hash value.

Returns:

- [Int64](core_package_intrinsics.md#int64) - The hash value.

Example:

<!-- verify -->
```cangjie
main() {
    // Test Rune.hashCode method
    var char: Rune = r'A'
    var hash = char.hashCode()
    println("Character: ${char}")
    println("Hash value: ${hash}")
    
    // Test Chinese character
    var chineseChar: Rune = r'爱'
    var chineseHash = chineseChar.hashCode()
    println("Character: ${chineseChar}")
    println("Hash value: ${chineseHash}")
}
```

Output:

```text
Character: A
Hash value: 65
Character: 爱
Hash value: 29233
```

## extend Rune <: ToString

```cangjie
extend Rune <: ToString
```

Function: Extends the [Rune](core_package_intrinsics.md#rune) type with the [ToString](core_package_interfaces.md#interface-tostring) interface to support conversion to [String](core_package_structs.md#struct-string).

Extended type:

- [Rune](core_package_intrinsics.md#rune)

Extended interfaces:

- ToString

### func toString()

```cangjie
public func toString(): String
```

Function: Converts the [Rune](core_package_intrinsics.md#rune) value to an outputtable string.

Returns:

- [String](core_package_structs.md#struct-string) - The converted string.

Example:

<!-- verify -->
```cangjie
main() {
    // Test Rune.toString method
    var char: Rune = r'A'
    var str = char.toString()
    println("Character: ${char}")
    println("Converted string: ${str}")
    
    // Test Chinese character
    var chineseChar: Rune = r'爱'
    var chineseStr = chineseChar.toString()
    println("Character: ${chineseChar}")
    println("Converted string: ${chineseStr}")
}
```

Output:

```text
Character: A
Converted string: A
Character: 爱
Converted string: 爱
```

## extend UInt16 <: Comparable\<UInt16>

```cangjie
extend UInt16 <: Comparable<UInt16>
```

Function: Extends the [UInt16](core_package_intrinsics.md#uint16) type with the [Comparable](core_package_interfaces.md#interface-comparablet)\<[UInt16](core_package_intrinsics.md#uint16)> interface to support comparison operations.

Extended type:

- [UInt16](core_package_intrinsics.md#uint16)

Extended interfaces:

- Comparable\<UInt16>

### func compare(UInt16)

```cangjie
public func compare(rhs: UInt16): Ordering
```

Function: Determines the size relationship between the current [UInt16](core_package_intrinsics.md#uint16) value and the specified [UInt16](core_package_intrinsics.md#uint16) value.

Parameters:

- rhs: [UInt16](core_package_intrinsics.md#uint16) - Another [UInt16](core_package_intrinsics.md#uint16) value to compare with.

Returns:

- [Ordering](core_package_enums.md#enum-ordering) - Returns [Ordering](core_package_enums.md#enum-ordering).GT if greater, [Ordering](core_package_enums.md#enum-ordering).EQ if equal, and [Ordering](core_package_enums.md#enum-ordering).LT if less.

Example:

<!-- verify -->
```cangjie
main() {
    var num1: UInt16 = 2
    var num2: UInt16 = 3
    println(num1.compare(num2))
}
```

Output:

```text
Ordering.LT
```

## extend UInt16 <: Countable\<UInt16>

```cangjie
extend UInt16 <: Countable<UInt16>
```

Function: Extends the [UInt16](core_package_intrinsics.md#uint16) type with the [Countable](core_package_interfaces.md#interface-countablet)\<[UInt16](core_package_intrinsics.md#uint16)> interface to support counting operations.

Extended type:

- [UInt16](core_package_intrinsics.md#uint16)

Extended interfaces:

- Countable\<UInt16>

### func next(Int64)

```cangjie
public func next(right: Int64): UInt16
```

Function: Gets the [UInt16](core_package_intrinsics.md#int32) value `right` positions to the right of the current [UInt16](core_package_intrinsics.md#int32) value on the number line. If the value overflows, it wraps around from the leftmost position.

Parameters:

- right: [Int64](core_package_intrinsics.md#int64) - The number of positions to move right.

Returns:

- [UInt16](core_package_intrinsics.md#uint16) - The [UInt16](core_package_intrinsics.md#uint16) value `right` positions to the right.

Example:

<!-- verify -->
```cangjie
main() {
    var num: UInt16 = 3
    println(num.next(10))
}
```

Output:

```text
13
```

### func position()

```cangjie
public func position(): Int64
```

Function: Gets the position information of the current [UInt16](core_package_intrinsics.md#uint16) value, i.e., converts the [UInt16](core_package_intrinsics.md#uint16) to an [Int64](core_package_intrinsics.md#int64) value.

Returns:

- [Int64](core_package_intrinsics.md#int64) - The position information of the current [UInt16](core_package_intrinsics.md#uint16) value.

Example:

<!-- verify -->
```cangjie
main() {
    var num: UInt16 = 8
    println(num.position())
}
```

Output:

```text
8
```

## extend UInt16 <: Hashable

```cangjie
extend UInt16 <: Hashable
```

Function: Extends the [UInt16](core_package_intrinsics.md#uint16) type with the [Hashable](core_package_interfaces.md#interface-hashable) interface to support hash value computation, and extends the type to support certain mathematical constants.

Extended type:

- [UInt16](core_package_intrinsics.md#uint16)

Extended interfaces:

- Hashable

### static prop Max

```cangjie
public static prop Max: UInt16
```

Function: Gets the maximum value of a 16-bit unsigned integer.

Type: [UInt16](./core_package_intrinsics.md#uint16)

Example:

<!-- verify -->
```cangjie
main() {
    // Test UInt16.Max property
    var maxVal = UInt16.Max
    println("Maximum value of UInt16: ${maxVal}")
    
    // Verify the maximum value
    if (maxVal == 65535) {
        println("Verification passed: UInt16.Max equals 65535")
    } else {
        println("Verification failed: UInt16.Max does not equal 65535")
    }
}
```

Output:

```text
Maximum value of UInt16: 65535
Verification passed: UInt16.Max equals 65535
```

### static prop Min

```cangjie
public static prop Min: UInt16
```

Function: Gets the minimum value of a 16-bit unsigned integer.

Type: [UInt16](./core_package_intrinsics.md#uint16)

Example:

<!-- verify -->
```cangjie
main() {
    // Test UInt16.Min property
    var minVal = UInt16.Min
    println("Minimum value of UInt16: ${minVal}")
    
    // Verify the minimum value
    if (minVal == 0) {
        println("Verification passed: UInt16.Min equals 0")
    } else {
        println("Verification failed: UInt16.Min does not equal 0")
    }
}
```

Output:

```text
Minimum value of UInt16: 0
Verification passed: UInt16.Min equals 0
```

### func hashCode()

```cangjie
public func hashCode(): Int64
```

Function: Gets the hash value.

Returns:

- [Int64](core_package_intrinsics.md#int64) - The hash value.

Example:

<!-- verify -->
```cangjie
main() {
    // Test UInt16.hashCode method
    var num: UInt16 = 42
    var hash = num.hashCode()
    println("UInt16 value: ${num}")
    println("Hash value: ${hash}")
    
    // Test maximum value
    var maxNum: UInt16 = UInt16.Max
    var maxHash = maxNum.hashCode()
    println("Maximum UInt16 value: ${maxNum}")
    println("Hash value: ${maxHash}")
}
```

Output:

```text
UInt16 value: 42
Hash value: 42
Maximum UInt16 value: 65535
Hash value: 65535
```

## extend UInt16 <: ToString

```cangjie
extend UInt16 <: ToString
```

Function: Extends the [UInt16](core_package_intrinsics.md#uint16) type with the [ToString](core_package_interfaces.md#interface-tostring) interface to support conversion to [String](core_package_structs.md#struct-string).

Extended type:

- [UInt16](core_package_intrinsics.md#uint16)

Extended interfaces:

- ToString

### func toString()

```cangjie
public func toString(): String
```

Function: Converts the [UInt16](core_package_intrinsics.md#uint16) value to an outputtable string.

Returns:

- [String](core_package_structs.md#struct-string) - The converted string.

Example:

<!-- verify -->
```cangjie
main() {
    // Test UInt16.toString method
    var num: UInt16 = 42
    var str = num.toString()
    println("UInt16 value: ${num}")
    println("Converted string: ${str}")
    
    // Test maximum value
    var maxNum: UInt16 = UInt16.Max
    var maxStr = maxNum.toString()
    println("Maximum UInt16 value: ${maxNum}")
    println("Converted string: ${maxStr}")
}
```

Output:

```text
UInt16 value: 42
Converted string: 42
Maximum UInt16 value: 65535
Converted string: 65535
```

## extend UInt32 <: Comparable\<UInt32>

```cangjie
extend UInt32 <: Comparable<UInt32>
```

Function: Extends the [UInt32](core_package_intrinsics.md#uint32) type with the [Comparable](core_package_interfaces.md#interface-comparablet)\<[UInt32](core_package_intrinsics.md#uint32)> interface to support comparison operations.

Extended type:

- [UInt32](core_package_intrinsics.md#uint32)

Extended interfaces:

- Comparable\<UInt32>

### func compare(UInt32)

```cangjie
public func compare(rhs: UInt32): Ordering
```

Function: Determines the relationship between the current [UInt32](core_package_intrinsics.md#uint32) value and the specified [UInt32](core_package_intrinsics.md#uint32) value.

Parameters:

- rhs: [UInt32](core_package_intrinsics.md#uint32) - Another [UInt32](core_package_intrinsics.md#uint32) value to compare.

Returns:

- [Ordering](core_package_enums.md#enum-ordering) - Returns [Ordering](core_package_enums.md#enum-ordering).GT if greater, [Ordering](core_package_enums.md#enum-ordering).EQ if equal, and [Ordering](core_package_enums.md#enum-ordering).LT if less.

Example:

<!-- verify -->
```cangjie
main() {
    var num1: UInt32 = 2
    var num2: UInt32 = 3
    println(num1.compare(num2))
}
```

Output:

```text
Ordering.LT
```

## extend UInt32 <: Countable\<UInt32>

```cangjie
extend UInt32 <: Countable<UInt32>
```

Function: Extends the [UInt32](core_package_intrinsics.md#uint32) type with the [Countable](core_package_interfaces.md#interface-countablet)\<[UInt32](core_package_intrinsics.md#uint32)> interface to support counting operations.

Extended type:

- [UInt32](core_package_intrinsics.md#uint32)

Extended interfaces:

- Countable\<UInt32>

### func next(Int64)

```cangjie
public func next(right: Int64): UInt32
```

Function: Gets the [UInt32](core_package_intrinsics.md#uint32) value at the position `right` units to the right of the current [UInt32](core_package_intrinsics.md#uint32) position on the number line. If the value overflows, it continues moving from the leftmost position of the number line.

Parameters:

- right: [Int64](core_package_intrinsics.md#int64) - The number of units to move right.

Returns:

- [UInt32](core_package_intrinsics.md#uint32) - The [UInt32](core_package_intrinsics.md#uint32) value at the new position after moving right by `right` units.

Example:

<!-- verify -->
```cangjie
main() {
    var num: UInt32 = 3
    println(num.next(10))
}
```

Output:

```text
13
```

### func position()

```cangjie
public func position(): Int64
```

Function: Gets the position information of the current [UInt32](core_package_intrinsics.md#uint32) value, i.e., converts the [UInt32](core_package_intrinsics.md#uint32) to a [Int64](core_package_intrinsics.md#int64) value.

Returns:

- [Int64](core_package_intrinsics.md#int64) - The position information of the current [UInt32](core_package_intrinsics.md#uint32) value.

Example:

<!-- verify -->
```cangjie
main() {
    var num: UInt32 = 8
    println(num.position())
}
```

Output:

```text
8
```

## extend UInt32 <: Hashable

```cangjie
extend UInt32 <: Hashable
```

Function: Extends the [UInt32](core_package_intrinsics.md#uint32) type with the [Hashable](core_package_interfaces.md#interface-hashable) interface to support hash value computation, and extends the type to support certain mathematical constants.

Extended type:

- [UInt32](core_package_intrinsics.md#uint32)

Extended interfaces:

- Hashable

### static prop Max

```cangjie
public static prop Max: UInt32
```

Function: Gets the maximum value of a 32-bit unsigned integer.

Type: [UInt32](./core_package_intrinsics.md#uint32)

Example:

<!-- verify -->
```cangjie
main() {
    var maxValue = UInt32.Max
    println(maxValue)
}
```

Output:

```text
4294967295
```

### static prop Min

```cangjie
public static prop Min: UInt32
```

Function: Gets the minimum value of a 32-bit unsigned integer.

Type: [UInt32](./core_package_intrinsics.md#uint32)

Example:

<!-- verify -->
```cangjie
main() {
    var minValue = UInt32.Min
    println(minValue)
}
```

Output:

```text
0
```

### func hashCode()

```cangjie
public func hashCode(): Int64
```

Function: Gets the hash value.

Returns:

- [Int64](core_package_intrinsics.md#int64) - The hash value.

Example:

<!-- verify -->
```cangjie
main() {
    var num: UInt32 = 42
    var hash = num.hashCode()
    println(hash)
}
```

Output:

```text
42
```

## extend UInt32 <: ToString

```cangjie
extend UInt32 <: ToString
```

Function: Extends the [UInt32](core_package_intrinsics.md#uint32) type with the [ToString](core_package_interfaces.md#interface-tostring) interface to support conversion to [String](core_package_structs.md#struct-string) type.

Extended type:

- [UInt32](core_package_intrinsics.md#uint32)

Extended interfaces:

- ToString

### func toString()

```cangjie
public func toString(): String
```

Function: Converts the [UInt32](core_package_intrinsics.md#uint32) value to an outputtable string.

Returns:

- [String](core_package_structs.md#struct-string) - The converted string.

Example:

<!-- verify -->
```cangjie
main() {
    var num: UInt32 = 42
    var str = num.toString()
    println(str)
}
```

Output:

```text
42
```

## extend UInt64 <: Comparable\<UInt64>

```cangjie
extend UInt64 <: Comparable<UInt64>
```

Function: Extends the [UInt64](core_package_intrinsics.md#uint64) type with the [Comparable](core_package_interfaces.md#interface-comparablet)\<[UInt64](core_package_intrinsics.md#uint64)> interface to support comparison operations.

Extended type:

- [UInt64](core_package_intrinsics.md#uint64)

Extended interfaces:

- Comparable\<UInt64>

### func compare(UInt64)

```cangjie
public func compare(rhs: UInt64): Ordering
```

Function: Determines the relationship between the current [UInt64](core_package_intrinsics.md#uint64) value and the specified [UInt64](core_package_intrinsics.md#uint64) value.

Parameters:

- rhs: [UInt64](core_package_intrinsics.md#uint64) - Another [UInt64](core_package_intrinsics.md#uint64) value to compare.

Returns:

- [Ordering](core_package_enums.md#enum-ordering) - Returns [Ordering](core_package_enums.md#enum-ordering).GT if greater, [Ordering](core_package_enums.md#enum-ordering).EQ if equal, and [Ordering](core_package_enums.md#enum-ordering).LT if less.

Example:

<!-- verify -->
```cangjie
main() {
    var num1: UInt64 = 2
    var num2: UInt64 = 3
    println(num1.compare(num2))
}
```

Output:

```text
Ordering.LT
```

## extend UInt64 <: Countable\<UInt64>

```cangjie
extend UInt64 <: Countable<UInt64>
```

Function: Extends the [UInt64](core_package_intrinsics.md#uint64) type with the [Countable](core_package_interfaces.md#interface-countablet)\<[UInt64](core_package_intrinsics.md#uint64)> interface to support counting operations.

Extended type:

- [UInt64](core_package_intrinsics.md#uint64)

Extended interfaces:

- Countable\<UInt64>

### func next(Int64)

```cangjie
public func next(right: Int64): UInt64
```

Function: Gets the [UInt64](core_package_intrinsics.md#uint64) value at the position `right` units to the right of the current [UInt64](core_package_intrinsics.md#uint64) position on the number line. If the value overflows, it continues moving from the leftmost position of the number line.

Parameters:

- right: [Int64](core_package_intrinsics.md#int64) - The number of units to move right.

Returns:

- [UInt64](core_package_intrinsics.md#uint64) - The [UInt64](core_package_intrinsics.md#uint64) value at the new position after moving right by `right` units.

Example:

<!-- verify -->
```cangjie
main() {
    var num: UInt64 = 3
    println(num.next(10))
}
```

Output:

```text
13
```

### func position()

```cangjie
public func position(): Int64
```

Function: Gets the position information of the current [UInt64](core_package_intrinsics.md#uint64) value, i.e., converts the [UInt64](core_package_intrinsics.md#uint64) to a [Int64](core_package_intrinsics.md#int64) value.

Returns:

- [Int64](core_package_intrinsics.md#int64) - The position information of the current [UInt64](core_package_intrinsics.md#uint64) value.

Example:

<!-- verify -->
```cangjie
main() {
    var num: UInt64 = 8
    println(num.position())
}
```

Output:

```text
8
```

## extend UInt64 <: Hashable

```cangjie
extend UInt64 <: Hashable
```

Function: Extends the [UInt64](core_package_intrinsics.md#uint64) type with the [Hashable](core_package_interfaces.md#interface-hashable) interface to support hash value computation, and extends the type to support certain mathematical constants.

Extended type:

- [UInt64](core_package_intrinsics.md#uint64)

Extended interfaces:

- Hashable

### static prop Max

```cangjie
public static prop Max: UInt64
```

Function: Gets the maximum value of a 64-bit unsigned integer.

Type: [UInt64](./core_package_intrinsics.md#uint64)

Example:

<!-- verify -->
```cangjie
main() {
    var maxValue = UInt64.Max
    println(maxValue)
}
```

Output:

```text
18446744073709551615
```

### static prop Min

```cangjie
public static prop Min: UInt64
```

Function: Gets the minimum value of a 64-bit unsigned integer.

Type: [UInt64](./core_package_intrinsics.md#uint64)

Example:

<!-- verify -->
```cangjie
main() {
    var minValue = UInt64.Min
    println(minValue)
}
```

Output:

```text
0
```

### func hashCode()

```cangjie
public func hashCode(): Int64
```

Function: Gets the hash value.

Returns:

- [Int64](core_package_intrinsics.md#int64) - The hash value.

Example:

<!-- verify -->
```cangjie
main() {
    var num: UInt64 = 42
    var hash = num.hashCode()
    println(hash)
}
```

Output:

```text
42
```

## extend UInt64 <: ToString

```cangjie
extend UInt64 <: ToString
```

Function: Extends the [UInt64](core_package_intrinsics.md#uint64) type with the [ToString](core_package_interfaces.md#interface-tostring) interface to support conversion to [String](core_package_structs.md#struct-string) type.

Extended type:

- [UInt64](core_package_intrinsics.md#uint64)

Extended interfaces:

- ToString

### func toString()

```cangjie
public func toString(): String
```

Function: Converts the [UInt64](core_package_intrinsics.md#uint64) value to an outputtable string.

Returns:

- [String](core_package_structs.md#struct-string) - The converted string.

Example:

<!-- verify -->
```cangjie
main() {
    var num: UInt64 = 42
    var str = num.toString()
    println(str)
}
```

Output:

```text
42
```

## extend UInt8 <: Comparable\<UInt8>

```cangjie
extend UInt8 <: Comparable<UInt8>
```

Function: Extends the [UInt8](core_package_intrinsics.md#uint8) type with the [Comparable](core_package_interfaces.md#interface-comparablet)\<[UInt8](core_package_intrinsics.md#uint8)> interface to support comparison operations.

Extended type:

- [UInt8](core_package_intrinsics.md#uint8)

Extended interfaces:

- Comparable\<UInt8>

### func compare(UInt8)

```cangjie
public func compare(rhs: UInt8): Ordering
```

Function: Determines the size relationship between the current [UInt8](core_package_intrinsics.md#uint8) value and the specified [UInt8](core_package_intrinsics.md#uint8) value.

Parameters:

- rhs: [UInt8](core_package_intrinsics.md#uint8) - Another [UInt8](core_package_intrinsics.md#uint8) value to compare.

Returns:

- [Ordering](core_package_enums.md#enum-ordering) - Returns [Ordering](core_package_enums.md#enum-ordering).GT if greater; [Ordering](core_package_enums.md#enum-ordering).EQ if equal; [Ordering](core_package_enums.md#enum-ordering).LT if less.

Example:

<!-- verify -->
```cangjie
main() {
    var num1: UInt8 = 2
    var num2: UInt8 = 3
    println(num1.compare(num2))
}
```

Output:

```text
Ordering.LT
```

## extend UInt8 <: Countable\<UInt8>

```cangjie
extend UInt8 <: Countable<UInt8>
```

Function: Extends the [UInt8](core_package_intrinsics.md#uint8) type with the [Countable](core_package_interfaces.md#interface-countablet)\<[UInt8](core_package_intrinsics.md#uint8)> interface to support counting operations.

Extended type:

- [UInt8](core_package_intrinsics.md#uint8)

Extended interfaces:

- Countable\<UInt8>

### func next(Int64)

```cangjie
public func next(right: Int64): UInt8
```

Function: Gets the [UInt8](core_package_intrinsics.md#uint8) value at the position `right` units to the right of the current [UInt8](core_package_intrinsics.md#int32) position on the number axis. If the value overflows, it continues moving from the leftmost position of the axis.

Parameters:

- right: [Int64](core_package_intrinsics.md#int64) - The number of units to move right.

Returns:

- [UInt8](core_package_intrinsics.md#uint8) - The [UInt8](core_package_intrinsics.md#uint8) value at the position after moving `right` units to the right.

Example:

<!-- verify -->
```cangjie
main() {
    var num: UInt8 = 3
    println(num.next(10))
}
```

Output:

```text
13
```

### func position()

```cangjie
public func position(): Int64
```

Function: Gets the position information of the current [UInt8](core_package_intrinsics.md#uint8) value, i.e., converts the [UInt8](core_package_intrinsics.md#uint8) to an [Int64](core_package_intrinsics.md#int64) value.

Returns:

- [Int64](core_package_intrinsics.md#int64) - The position information of the current [UInt8](core_package_intrinsics.md#uint8) value.

Example:

<!-- verify -->
```cangjie
main() {
    var num: UInt8 = 8
    println(num.position())
}
```

Output:

```text
8
```

## extend UInt8 <: Hashable

```cangjie
extend UInt8 <: Hashable
```

Function: Extends the [UInt8](core_package_intrinsics.md#uint8) type with the [Hashable](core_package_interfaces.md#interface-hashable) interface to support hash value computation, and extends the type to support certain mathematical constants.

Extended type:

- [UInt8](core_package_intrinsics.md#uint8)

Extended interfaces:

- Hashable

### static prop Max

```cangjie
public static prop Max: UInt8
```

Function: Gets the maximum value of an 8-bit unsigned integer.

Type: [UInt8](./core_package_intrinsics.md#uint8)

Example:

<!-- verify -->
```cangjie
main() {
    var maxValue = UInt8.Max
    println(maxValue)
}
```

Output:

```text
255
```

### static prop Min

```cangjie
public static prop Min: UInt8
```

Function: Gets the minimum value of an 8-bit unsigned integer.

Type: [UInt8](./core_package_intrinsics.md#uint8)

Example:

<!-- verify -->
```cangjie
main() {
    var minValue = UInt8.Min
    println(minValue)
}
```

Output:

```text
0
```

### func hashCode()

```cangjie
public func hashCode(): Int64
```

Function: Gets the hash value.

Returns:

- [Int64](core_package_intrinsics.md#int64) - The hash value.

Example:

<!-- verify -->
```cangjie
main() {
    var num: UInt8 = 42
    var hash = num.hashCode()
    println(hash)
}
```

Output:

```text
42
```

## extend UInt8 <: ToString

```cangjie
extend UInt8 <: ToString
```

Function: Extends the [UInt8](core_package_intrinsics.md#uint8) type with the [ToString](core_package_interfaces.md#interface-tostring) interface to support conversion to the [String](core_package_structs.md#struct-string) type.

Extended type:

- [UInt8](core_package_intrinsics.md#uint8)

Extended interfaces:

- ToString

### func toString()

```cangjie
public func toString(): String
```

Function: Converts the [UInt8](core_package_intrinsics.md#uint8) value to an outputtable string.

Returns:

- [String](core_package_structs.md#struct-string) - The converted string.

Example:

<!-- verify -->
```cangjie
main() {
    var num: UInt8 = 42
    var str = num.toString()
    println(str)
}
```

Output:

```text
42
```

## extend UIntNative <: Comparable\<UIntNative>

```cangjie
extend UIntNative <: Comparable<UIntNative>
```

Function: Extends the [UIntNative](core_package_intrinsics.md#uintnative) type with the [Comparable](core_package_interfaces.md#interface-comparablet)\<[UIntNative](core_package_intrinsics.md#uintnative)> interface to support comparison operations.

Extended type:

- [UIntNative](core_package_intrinsics.md#uintnative)

Extended interfaces:

- Comparable\<UIntNative>

### func compare(UIntNative)

```cangjie
public func compare(rhs: UIntNative): Ordering
```

Function: Determines the size relationship between the current [UIntNative](core_package_intrinsics.md#uintnative) value and the specified [UIntNative](core_package_intrinsics.md#uintnative) value.

Parameters:

- rhs: [UIntNative](core_package_intrinsics.md#uintnative) - Another [UIntNative](core_package_intrinsics.md#uintnative) value to compare.

Returns:

- [Ordering](core_package_enums.md#enum-ordering) - Returns [Ordering](core_package_enums.md#enum-ordering).GT if greater; [Ordering](core_package_enums.md#enum-ordering).EQ if equal; [Ordering](core_package_enums.md#enum-ordering).LT if less.

Example:

<!-- verify -->
```cangjie
main() {
    var num1: UIntNative = 2
    var num2: UIntNative = 3
    println(num1.compare(num2))
}
```

Output:

```text
Ordering.LT
```

## extend UIntNative <: Countable

```cangjie
extend UIntNative <: Countable<UIntNative>
```

Function: Extends the [UIntNative](core_package_intrinsics.md#uintnative) type with the [Countable](core_package_interfaces.md#interface-countablet)\<[UIntNative](core_package_intrinsics.md#uintnative)> interface to support counting operations.

Extended type:

- [UIntNative](core_package_intrinsics.md#uintnative)

Extended interfaces:

- Countable\<UIntNative>

### func next(Int64)

```cangjie
public func next(right: Int64): UIntNative
```

Function: Gets the [UIntNative](core_package_intrinsics.md#uintnative) value at the position `right` units to the right of the current [UIntNative](core_package_intrinsics.md#int32) position on the number axis. If the value overflows, it continues moving from the leftmost position of the axis.

Parameters:

- right: [Int64](core_package_intrinsics.md#int64) - The number of units to move right.

Returns:

- [UIntNative](core_package_intrinsics.md#uintnative) - The [UIntNative](core_package_intrinsics.md#uintnative) value at the position after moving `right` units to the right.

Example:

<!-- verify -->
```cangjie
main() {
    var num: UIntNative = 3
    println(num.next(10))
}
```

Output:

```text
13
```

### func position()

```cangjie
public func position(): Int64
```

Function: Gets the position information of the current [UIntNative](core_package_intrinsics.md#uintnative) value, i.e., converts the [UIntNative](core_package_intrinsics.md#uintnative) to an [Int64](core_package_intrinsics.md#int64) value.

Returns:

- [Int64](core_package_intrinsics.md#int64) - The position information of the current [UIntNative](core_package_intrinsics.md#uintnative) value.

Example:

<!-- verify -->
```cangjie
main() {
    var num: UIntNative = 8
    println(num.position())
}
```

Output:

```text
8
```

## extend UIntNative <: Hashable

```cangjie
extend UIntNative <: Hashable
```

Function: Extends the [UIntNative](core_package_intrinsics.md#uintnative) type with the [Hashable](core_package_interfaces.md#interface-hashable) interface to support hash value computation, and extends the type to support certain mathematical constants.

Extended type:

- [UIntNative](core_package_intrinsics.md#uintnative)

Extended interfaces:

- Hashable

### static prop Max

```cangjie
public static prop Max: UIntNative
```

Function: Gets the maximum value of a platform-dependent unsigned integer.

Type: [UIntNative](./core_package_intrinsics.md#uintnative)

Example:

<!-- verify -->
```cangjie
main() {
    var maxValue = UIntNative.Max
    println(maxValue)
}
```

Output:

```text
18446744073709551615
```

### static prop Min

```cangjie
public static prop Min: UIntNative
```

Function: Gets the minimum value of a platform-dependent unsigned integer.

Type: [UIntNative](./core_package_intrinsics.md#uintnative)

Example:

<!-- verify -->
```cangjie
main() {
    var minValue = UIntNative.Min
    println(minValue)
}
```

Output:

```text
0
```

### func hashCode()

```cangjie
public func hashCode(): Int64
```

Function: Gets the hash value.

Returns:

- [Int64](core_package_intrinsics.md#int64) - The hash value.

Example:

<!-- verify -->
```cangjie
main() {
    var num: UIntNative = 42
    var hash = num.hashCode()
    println(hash)
}
```

Output:

```text
42
```

## extend UIntNative <: ToString

```cangjie
extend UIntNative <: ToString
```

Function: Extends the [UIntNative](core_package_intrinsics.md#uintnative) type with the [ToString](core_package_interfaces.md#interface-tostring) interface to support conversion to the [String](core_package_structs.md#struct-string) type.

Extended type:

- [UIntNative](core_package_intrinsics.md#uintnative)

Extended interfaces:

- ToString

### func toString()

```cangjie
public func toString(): String
```

Function: Converts the [UIntNative](core_package_intrinsics.md#uintnative) value to an outputtable string.

Returns:

- [String](core_package_structs.md#struct-string) - The converted string.

Example:

<!-- verify -->
```cangjie
main() {
    var num: UIntNative = 12345
    var str = num.toString()
    println("The string representation is: " + str)
}
```

Output:

```text
The string representation is: 12345
```

## extend Unit <: Equatable\<Unit>

```cangjie
extend Unit <: Equatable<Unit>
```

Function: Extends the [Equatable](core_package_interfaces.md#interface-equatablet)\<[Unit](core_package_intrinsics.md#unit)> interface for the [Unit](core_package_intrinsics.md#unit) type.

Extended type:

- [Unit](core_package_intrinsics.md#unit)

Extended interfaces:

- Equatable\<Unit>

## extend Unit <: Hashable

```cangjie
extend Unit <: Hashable
```

Function: Extends the [Hashable](core_package_interfaces.md#interface-hashable) interface for the [Unit](core_package_intrinsics.md#unit) type, enabling hash value computation.

Extended type:

- [Unit](core_package_intrinsics.md#unit)

Extended interfaces:

- Hashable

### func hashCode()

```cangjie
public func hashCode(): Int64
```

Function: Retrieves the hash value. The hash value of [Unit](core_package_intrinsics.md#unit) is 0.

Returns:

- [Int64](core_package_intrinsics.md#int64) - The hash value.

Example:

<!-- verify -->
```cangjie
main() {
    var unitValue = ()
    var hash = unitValue.hashCode()
    println(hash)
}
```

Output:

```text
0
```

## extend Unit <: ToString

```cangjie
extend Unit <: ToString
```

Function: Extends the [ToString](core_package_interfaces.md#interface-tostring) interface for the [Unit](core_package_intrinsics.md#unit) type, enabling conversion to the [String](core_package_structs.md#struct-string) type.

Extended type:

- [Unit](core_package_intrinsics.md#unit)

Extended interfaces:

- ToString

### func toString()

```cangjie
public func toString(): String
```

Function: Converts a [Unit](core_package_intrinsics.md#unit) value to an output-ready string.

Returns:

- [String](core_package_structs.md#struct-string) - The converted string.

Example:

<!-- verify -->
```cangjie
main() {
    var unitValue = ()
    var str = unitValue.toString()
    println("The string representation is: " + str)
}
```

Output:

```text
The string representation is: ()
```

## extend\<T> Array\<Array\<T>>

```cangjie
extend<T> Array<Array<T>>
```

Function: Extends 2D arrays with methods to flatten them into 1D arrays.

Extended type:

- [Array\<Array\<T>>](./core_package_structs.md#struct-arrayt)

### func flatten()

```cangjie
public func flatten(): Array<T>
```

Function: Flattens the current 2D array into a 1D array.

For example, converts [[1, 2], [3, 4]] to [1, 2, 3, 4].

Returns:

- [Array](./core_package_structs.md#struct-arrayt)\<T> - The flattened 1D array.

Example:

<!-- verify -->
```cangjie
main(): Int64 {
    let arr = [[1, 2], [3, 4]].flatten()
    println(arr)
    return 0
}
```

Execution Result:

```text
[1, 2, 3, 4]
```

## extend\<T> Array\<T>

```cangjie
extend<T> Array<T>
```

Function: Extends the [Array](core_package_structs.md#struct-arrayt)\<T> type.

Extended type:

- [Array\<T>](core_package_structs.md#struct-arrayt)

### func enumerate()

```cangjie
public func enumerate(): Array<(Int64, T)>
```

Function: Retrieves an array with indices.

Returns:

- [Array](core_package_structs.md#struct-arrayt)\<([Int64](../../core/core_package_api/core_package_intrinsics.md#int64), T)> - Returns a new array with indices.

### func zip\<R>(Array\<R>)

```cangjie
public func zip<R>(other: Array<R>): Array<(T, R)>
```

Function: Merges two arrays into a new array (length determined by the shorter array).

> **Note：**
>
> Unsupported platform：OpenHarmony

Parameters:

- other: [Array](core_package_structs.md#struct-arrayt)\<R> - One of the arrays to merge.

Returns:

- [Array](core_package_structs.md#struct-arrayt)\<(T, R)> - Returns a new array.### extend\<T> Array\<T> <: Collection\<T>

```cangjie
extend<T> Array<T> <: Collection<T>
```

Function: Implements the [Collection](core_package_interfaces.md#interface-collectiont) interface for the [Array](core_package_structs.md#struct-arrayt)\<T> type.

Parent Types:

- [Collection](core_package_interfaces.md#interface-collectiont)\<T>

### prop size

```cangjie
public prop size: Int64
```

Function: Gets the number of elements.

Type: [Int64](core_package_intrinsics.md#int64)

Example:

<!-- verify -->
```cangjie
main() {
    let arr = [1, 2, 3, 4, 5]
    
    println("Array: ${arr}")
    println("Size of array: ${arr.size}")
    
    // Create an empty array
    let emptyArr: Array<Int64> = []
    
    println("Empty array: ${emptyArr}")
    println("Size of empty array: ${emptyArr.size}")
    
    // Create an array containing strings
    let strArr = ["Hello", "World", "Cangjie", "Language"]
    
    println("String array: ${strArr}")
    println("Size of string array: ${strArr.size}")
}
```

Output:

```text
Array: [1, 2, 3, 4, 5]
Size of array: 5
Empty array: []
Size of empty array: 0
String array: [Hello, World, Cangjie, Language]
Size of string array: 4
```

### func isEmpty()

```cangjie
public func isEmpty(): Bool
```

Function: Determines whether the array is empty.

Returns:

- [Bool](core_package_intrinsics.md#bool) - Returns true if the array is empty, otherwise returns false.

Example:

<!-- verify -->
```cangjie
main() {
    let arr = [1, 2, 3, 4, 5]
    
    println("Array: ${arr}")
    println("Is array empty? ${arr.isEmpty()}")
    
    // Create an empty array
    let emptyArr: Array<Int64> = []
    
    println("Empty array: ${emptyArr}")
    println("Is empty array empty? ${emptyArr.isEmpty()}")
    
    // Create an array containing strings
    let strArr = ["Hello", "World"]
    
    println("String array: ${strArr}")
    println("Is string array empty? ${strArr.isEmpty()}")
}
```

Output:

```text
Array: [1, 2, 3, 4, 5]
Is array empty? false
Empty array: []
Is empty array empty? true
String array: [Hello, World]
Is string array empty? false
```

### func iterator()

```cangjie
public func iterator(): Iterator<T>
```

Function: Gets an iterator for the current array, used to traverse the array.

Returns:

- [Iterator](core_package_classes.md#class-iteratort)\<T> - An iterator for the current array.

Example:

<!-- verify -->
```cangjie
main() {
    let arr = [1, 2, 3, 4, 5]
    
    println("Array: ${arr}")
    
    // Get the iterator for the array
    let iter = arr.iterator()
    
    println("Using iterator to traverse the array:")
    
    // Traverse the array using the iterator
    while (true) {
        match (iter.next()) {
            case Some(value) => println("Element: ${value}")
            case None => break
        }
    }
    
    // Create another array and demonstrate other iterator usages
    let arr2 = ["Hello", "World", "Cangjie"]
    let iter2 = arr2.iterator()
    
    println("\nSecond array: ${arr2}")
    println("Using iterator with for-in loop:")
    
    // Alternatively, use a for-in loop to traverse the iterator
    for (element in iter2) {
        println("Element: ${element}")
    }
}
```

Output:

```text
Array: [1, 2, 3, 4, 5]
Using iterator to traverse the array:
Element: 1
Element: 2
Element: 3
Element: 4
Element: 5

Second array: [Hello, World, Cangjie]
Using iterator with for-in loop:
Element: Hello
Element: World
Element: Cangjie
```

### func toArray()

```cangjie
public func toArray(): Array<T>
```

Function: Creates a new [Array](core_package_structs.md#struct-arrayt) instance by copying the current [Array](core_package_structs.md#struct-arrayt) instance.

Returns:

- [Array](core_package_structs.md#struct-arrayt)\<T> - A new [Array](core_package_structs.md#struct-arrayt) instance obtained by copying.

Example:

<!-- verify -->
```cangjie
main() {
    let arr = [1, 2, 3, 4, 5]
    
    println("Original array: ${arr}")
    
    // Use the toArray() method to create a copy of the array
    let copiedArr = arr.toArray()
    
    println("Copied array: ${copiedArr}")
    
    // Verify that the copied array is independent
    copiedArr[0] = 10
    println("After modifying copied array:")
    println("Original array: ${arr}")
    println("Copied array: ${copiedArr}")
    
    // Create an array containing strings and test toArray()
    let strArr = ["Hello", "World", "Cangjie"]
    let copiedStrArr = strArr.toArray()
    
    println("\nString array: ${strArr}")
    println("Copied string array: ${copiedStrArr}")
    
    // Verify that the copied string array is independent
    copiedStrArr[0] = "Hi"
    println("After modifying copied string array:")
    println("Original string array: ${strArr}")
    println("Copied string array: ${copiedStrArr}")
}
```

Output:

```text
Original array: [1, 2, 3, 4, 5]
Copied array: [1, 2, 3, 4, 5]
After modifying copied array:
Original array: [1, 2, 3, 4, 5]
Copied array: [10, 2, 3, 4, 5]

String array: [Hello, World, Cangjie]
Copied string array: [Hello, World, Cangjie]
After modifying copied string array:
Original string array: [Hello, World, Cangjie]
Copied string array: [Hi, World, Cangjie]
```

## extend\<T> Array\<T> <: Equatable\<Array\<T>> where T <: Equatable\<T>

```cangjie
extend<T> Array<T> <: Equatable<Array<T>> where T <: Equatable<T>
```

Function: Extends the [Equatable](core_package_interfaces.md#interface-equatablet)\<[Array](core_package_structs.md#struct-arrayt)\<T>> interface implementation for the [Array](core_package_structs.md#struct-arrayt)\<T> type, supporting equality comparison operations.

Extended type:

- [Array\<T>](core_package_structs.md#struct-arrayt)

Extended interfaces:

- Equatable\<Array\<T>>

### func contains(T)

```cangjie
public func contains(element: T): Bool
```

Function: Checks whether the current array contains the specified element.

Parameters:

- element: T - The target element to search for.

Returns:

- [Bool](core_package_intrinsics.md#bool) - Returns true if the element exists, otherwise returns false.

Example:

<!-- verify -->
```cangjie
main(): Int64 {
    let arr = [1, 2, 3, 4]
    println(arr.contains(1))
    return 0
}
```

Output:

```text
true
```

### func indexOf(Array\<T>)

```cangjie
public func indexOf(elements: Array<T>): Option<Int64>
```

Function: Returns the first position where the subarray `elements` appears in the array. If the array does not contain this subarray, returns None.

> **Note:**
>
> When the type of T is [Int64](core_package_intrinsics.md#int64), the variadic parameter syntax sugar version of this function may cause ambiguity with `public func indexOf(element: T, fromIndex: Int64): Option<Int64>`. According to priority, when the number of parameters is 2, `public func indexOf(element: T, fromIndex: Int64): Option<Int64>` will be called first.

Parameters:

- elements: [Array](core_package_structs.md#struct-arrayt)\<T> - The target subarray to locate.

Returns:

- [Option](core_package_enums.md#enum-optiont)\<[Int64](core_package_intrinsics.md#int64)> - The first position where the subarray `elements` appears in the array. If the array does not contain this subarray, returns None.

Example:

<!-- verify -->
```cangjie
main(): Int64 {
    let arr = [1, 2, 3, 4]
    let subArr = [2, 3]
    println(arr.indexOf(subArr))
    return 0
}
```

Output:

```text
Some(1)
```

### func indexOf(Array\<T>, Int64)

```cangjie
public func indexOf(elements: Array<T>, fromIndex: Int64): Option<Int64>
```

Function: Returns the first position where the subarray `elements` appears in the array after `fromIndex`. If not found, returns None.

The function checks the range of `fromIndex`. If `fromIndex` is less than 0, the search will start from position 0. If `fromIndex` is greater than or equal to the size of the array, the result is None.

Parameters:

- elements: [Array](core_package_structs.md#struct-arrayt)\<T> - The target subarray to locate.
- fromIndex: [Int64](core_package_intrinsics.md#int64) - The starting position for the search.

Returns:

- [Option](core_package_enums.md#enum-optiont)\<[Int64](core_package_intrinsics.md#int64)> - The first position where the subarray `elements` appears in the array after `fromIndex`. If not found, returns None.

Example:

<!-- verify -->
```cangjie
main(): Int64 {
    let arr = [1, 2, 3, 4, 2, 3]
    let subArr = [2, 3]
    println(arr.indexOf(subArr, 3))
    return 0
}
```

Output:

```text
Some(4)
```

### func indexOf(T)

```cangjie
public func indexOf(element: T): Option<Int64>
```

Function: Gets the first position where `element` appears in the array. If the array does not contain this element, returns None.

Parameters:

- element: T - The target element to locate.

Returns:

- [Option](core_package_enums.md#enum-optiont)\<[Int64](core_package_intrinsics.md#int64)> - The first position where `element` appears in the array. If the array does not contain this element, returns None.

Example:

<!-- verify -->
```cangjie
main() {
    let arr = [1, 2, 3, 4, 2, 5]
    
    println("Array: ${arr}")
    
    // Find the first occurrence of element 2
    let index1 = arr.indexOf(2)
    println("First occurrence of 2: ${index1}")
    
    // Find the first occurrence of element 6 (does not exist)
    let index2 = arr.indexOf(6)
    println("First occurrence of 6: ${index2}")
    
    // Create an array containing strings and test indexOf
    let strArr = ["Hello", "World", "Hello", "Cangjie"]
    
    println("\nString array: ${strArr}")
    
    // Find the first occurrence of the string "Hello"
    let strIndex1 = strArr.indexOf("Hello")
    println("First occurrence of \"Hello\": ${strIndex1}")
}
```

Output:

```text
Array: [1, 2, 3, 4, 2, 5]
First occurrence of 2: Some(1)
First occurrence of 6: None

String array: [Hello, World, Hello, Cangjie]
First occurrence of "Hello": Some(0)
```

### func indexOf(T, Int64)

```cangjie
public func indexOf(element: T, fromIndex: Int64): Option<Int64>
```

Function: Returns the first position where `element` appears in the array after `fromIndex`. If not found, returns None.

The function will search starting from index `fromIndex`. If `fromIndex` is less than 0, the search will start from position 0. If `fromIndex` is greater than or equal to the size of the array, the result is None.

Parameters:

- element: T - The target element to locate.
- fromIndex: [Int64](core_package_intrinsics.md#int64) - The starting position for the search.

Returns:

- [Option](core_package_enums.md#enum-optiont)\<[Int64](core_package_intrinsics.md#int64)> - The first position where `element` appears in the array after `fromIndex`. If not found, returns None.

Example:

<!-- verify -->
```cangjie
main() {
    let arr = [1, 2, 3, 4, 2, 5]
    
    println("Array: ${arr}")
    
    // Find the first occurrence of element 2 starting from index 2
    let index1 = arr.indexOf(2, 2)
    println("First occurrence of 2 starting from index 2: ${index1}")
    
    // Find the first occurrence of element 2 starting from index 5 (does not exist)
    let index2 = arr.indexOf(2, 5)
    println("First occurrence of 2 starting from index 5: ${index2}")
    
    // Create an array containing strings and test indexOf
    let strArr = ["Hello", "World", "Hello", "Cangjie"]
    
    println("\nString array: ${strArr}")
    
    // Find the first occurrence of the string "Hello" starting from index 1
    let strIndex = strArr.indexOf("Hello", 1)
    println("First occurrence of \"Hello\" starting from index 1: ${strIndex}")
}
```

Output:

```text
Array: [1, 2, 3, 4, 2, 5]
First occurrence of 2 starting from index 2: Some(4)
First occurrence of 2 starting from index 5: None

String array: [Hello, World, Hello, Cangjie]
First occurrence of "Hello" starting from index 1: Some(2)
```

Example:

<!-- verify -->
```cangjie
main(): Int64 {
    let arr = [1, 2, 3, 4, 2, 3]
    let subArr = [2, 3]
    println(arr.lastIndexOf(subArr, 3))
    return 0
}
```

Output:

```text
Some(4)
```

### func lastIndexOf(Array\<T>)

```cangjie
public func lastIndexOf(elements: Array<T>): Option<Int64>
```

Function: Returns the last position where the subarray `elements` appears in the array. If the array does not contain this subarray, returns None.

Parameters:

- elements: [Array](core_package_structs.md#struct-arrayt)\<T> - The target subarray to locate.

Returns:

- [Option](core_package_enums.md#enum-optiont)\<[Int64](core_package_intrinsics.md#int64)> - The last position where the subarray `elements` appears in the array. If the array does not contain this subarray, returns None.

Example:

<!-- verify -->
```cangjie
main() {
    let arr = [1, 2, 3, 2, 3, 4, 2, 3]
    let subArr = [2, 3]
    
    println("Array: ${arr}")
    println("Sub-array: ${subArr}")
    
    // Find the last occurrence of the subarray [2, 3]
    let lastIndex = arr.lastIndexOf(subArr)
    println("Last occurrence of [2, 3]: ${lastIndex}")
    
    // Find a non-existent subarray
    let notExist = [5, 6]
    let notExistIndex = arr.lastIndexOf(notExist)
    println("Last occurrence of [5, 6]: ${notExistIndex}")
}
```

Output:

```text
Array: [1, 2, 3, 2, 3, 4, 2, 3]
Sub-array: [2, 3]
Last occurrence of [2, 3]: Some(6)
Last occurrence of [5, 6]: None
```

### func lastIndexOf(Array\<T>, Int64)

```cangjie
public func lastIndexOf(elements: Array<T>, fromIndex: Int64): Option<Int64>
```

Function: Searches backward starting from `fromIndex` and returns the last position where the subarray `elements` appears in the array. If the array does not contain this subarray, returns None.

The function checks the range of `fromIndex`. If `fromIndex` is less than 0, the search will start from position 0. If `fromIndex` is greater than or equal to the size of the array, the result is None.

Parameters:

- elements: [Array](core_package_structs.md#struct-arrayt)\<T> - The target subarray to locate.
- fromIndex: [Int64](core_package_intrinsics.md#int64) - The starting position for the search.

Returns:

- [Option](core_package_enums.md#enum-optiont)\<[Int64](core_package_intrinsics.md#int64)> - The last position where the subarray `elements` appears in the array when searching backward from `fromIndex`. If the array does not contain this subarray, returns None.

Example:

<!-- verify -->
```cangjie
main() {
    let arr = [1, 2, 3, 2, 3, 4, 3, 3]
    let subArr = [2, 3]
    
    println("Array: ${arr}")
    println("Sub-array: ${subArr}")
    
    // Search backward from index 4 to find the last occurrence of sub-array [2, 3]
    let lastIndex = arr.lastIndexOf(subArr, 4)
    println("Last occurrence of [2, 3] starting from index 4: ${lastIndex}")
    
    // Search backward from index 2 to find the last occurrence of sub-array [2, 3]
    let lastIndex2 = arr.lastIndexOf(subArr, 2)
    println("Last occurrence of [2, 3] starting from index 2: ${lastIndex2}")
}
```

Execution Result:

```text
Array: [1, 2, 3, 2, 3, 4, 3, 3]
Sub-array: [2, 3]
Last occurrence of [2, 3] starting from index 4: None
Last occurrence of [2, 3] starting from index 2: Some(3)
```

### func lastIndexOf(T)

```cangjie
public func lastIndexOf(element: T): Option<Int64>
```

Function: Returns the last occurrence position of `element` in the array. Returns None if the element does not exist in the array.

Parameters:

- element: T - The target element to locate.

Returns:

- [Option](core_package_enums.md#enum-optiont)\<[Int64](core_package_intrinsics.md#int64)> - The last occurrence position of `element` in the array. Returns None if the element does not exist.

Example:

<!-- verify -->
```cangjie
main() {
    let arr = [1, 2, 3, 4, 2, 5, 2]
    
    println("Array: ${arr}")
    
    // Find the last occurrence position of element 2
    let lastIndex = arr.lastIndexOf(2)
    println("Last occurrence of 2: ${lastIndex}")
    
    // Find the last occurrence position of element 6 (does not exist)
    let notExistIndex = arr.lastIndexOf(6)
    println("Last occurrence of 6: ${notExistIndex}")
    
    // Create a string array and test lastIndexOf
    let strArr = ["Hello", "World", "Hello", "Cangjie", "Hello"]
    
    println("\nString array: ${strArr}")
    
    // Find the last occurrence position of string "Hello"
    let strLastIndex = strArr.lastIndexOf("Hello")
    println("Last occurrence of \"Hello\": ${strLastIndex}")
}
```

Execution Result:

```text
Array: [1, 2, 3, 4, 2, 5, 2]
Last occurrence of 2: Some(6)
Last occurrence of 6: None

String array: [Hello, World, Hello, Cangjie, Hello]
Last occurrence of "Hello": Some(4)
```

### func lastIndexOf(T, Int64)

```cangjie
public func lastIndexOf(element: T, fromIndex: Int64): Option<Int64>
```

Function: Searches backward starting from `fromIndex` and returns the last occurrence position of `element` in the array. Returns None if the element does not exist.

The function checks the range of `fromIndex`. If `fromIndex` is less than 0, the search starts from position 0. If `fromIndex` is greater than or equal to the array size, the result is None.

Parameters:

- element: T - The target element to locate.
- fromIndex: [Int64](core_package_intrinsics.md#int64) - The starting position for the search.

Returns:

- [Option](core_package_enums.md#enum-optiont)\<[Int64](core_package_intrinsics.md#int64)> - The last occurrence position of `element` when searching backward from `fromIndex`. Returns None if the element does not exist.

Example:

<!-- verify -->
```cangjie
main() {
    let arr = [1, 2, 3, 4, 2, 5, 3]
    
    println("Array: ${arr}")
    
    // Search backward from index 5 to find the last occurrence of element 2
    let lastIndex = arr.lastIndexOf(2, 5)
    println("Last occurrence of 2 starting from index 5: ${lastIndex}")
    
    // Search backward from index 3 to find the last occurrence of element 2
    let lastIndex2 = arr.lastIndexOf(2, 3)
    println("Last occurrence of 2 starting from index 3: ${lastIndex2}")
    
    // Create a string array and test lastIndexOf
    let strArr = ["Hello", "World", "Hello", "Cangjie", "Hello"]
    
    println("\nString array: ${strArr}")
    
    // Search backward from index 2 to find the last occurrence of string "Hello"
    let strLastIndex = strArr.lastIndexOf("Hello", 2)
    println("Last occurrence of \"Hello\" starting from index 2: ${strLastIndex}")
}
```

Execution Result:

```text
Array: [1, 2, 3, 4, 2, 5, 3]
Last occurrence of 2 starting from index 5: None
Last occurrence of 2 starting from index 3: Some(4)

String array: [Hello, World, Hello, Cangjie, Hello]
Last occurrence of "Hello" starting from index 2: Some(4)
```

### func removePrefix(Array\<T>)

```cangjie
public func removePrefix(prefix: Array<T>): Array<T>
```

Function: Removes the prefix.

If the beginning of the current array exactly matches the prefix, the prefix is removed. The return value is a slice of the current array after removing the prefix.

Parameters:

- prefix: [Array](./core_package_structs.md#struct-arrayt)\<T> - The prefix to be removed.

Returns:

- [Array](./core_package_structs.md#struct-arrayt)\<T> - The slice of the original array after removing the prefix.

Example:

<!-- verify -->
```cangjie
main(): Int64 {
    let arr = [1, 2, 1, 2, 3].removePrefix([1, 2])
    println(arr)
    return 0
}
```

Execution Result:

```text
[1, 2, 3]
```

### func removeSuffix(Array\<T>)

```cangjie
public func removeSuffix(suffix: Array<T>): Array<T>
```

Function: Removes the suffix.

If the end of the current array exactly matches the suffix, the suffix is removed. The return value is a slice of the current array after removing the suffix.

Parameters:

- suffix: [Array](./core_package_structs.md#struct-arrayt)\<T> - The suffix to be removed.

Returns:

- [Array](./core_package_structs.md#struct-arrayt)\<T> - The slice of the original array after removing the suffix.

Example:

<!-- verify -->
```cangjie
main(): Int64 {
    let arr = [1, 2, 3, 2, 3].removeSuffix([2, 3])
    println(arr)
    return 0
}
```

Execution Result:

```text
[1, 2, 3]
```

### func trimEnd(Array\<T>)

```cangjie
public func trimEnd(set: Array<T>): Array<T>
```

Function: Trims the current array by removing elements from the end that are in the specified set `set`, until the first element not in `set` is encountered, and returns the slice of the current array.

Parameters:

- set: [Array](core_package_structs.md#struct-arrayt)\<T> - The set of elements to be removed.

Returns:

- [Array](core_package_structs.md#struct-arrayt)\<T> - The trimmed array slice.

Example:

<!-- verify -->
```cangjie
main(): Int64 {
    let arr = [2, 1, 2, 2, 3].trimEnd([2, 3])
    println(arr)
    return 0
}
```

Execution Result:

```text
[2, 1]
```

### func trimEnd((T)->Bool)

```cangjie
public func trimEnd(predicate: (T)->Bool): Array<T>
```

Function: Trims the current array by removing elements from the end that satisfy the filtering condition, until the first element that does not satisfy the condition is encountered, and returns the slice of the current array.

Parameters:

- predicate: (T)->[Bool](./core_package_intrinsics.md#bool) - The filtering condition.

Returns:

- [Array](core_package_structs.md#struct-arrayt)\<T> - The trimmed array slice.

Example:

<!-- verify -->
```cangjie
main() {
    let arr = [1, 2, 3, 4, 5, 4, 3, 2, 1]
    
    println("Original array: ${arr}")
    
    // Remove all elements less than 4 from the end
    let trimmedArr = arr.trimEnd({x => x < 4})
    println("Array after trimming elements less than 4 from the end: ${trimmedArr}")
    
    // Create a string array and test trimEnd
    let strArr = ["a", "b", "c", "d", "c", "b", "a"]
    
    println("\nString array: ${strArr}")
    
    // Remove all elements not equal to "c" from the end
    let trimmedStrArr = strArr.trimEnd({s => s != "c"})
    println("String array after trimming elements not equal to \"c\" from the end: ${trimmedStrArr}")
}
```

Execution Result:

```text
Original array: [1, 2, 3, 4, 5, 4, 3, 2, 1]
Array after trimming elements less than 4 from the end: [1, 2, 3, 4, 5, 4]

String array: [a, b, c, d, c, b, a]
String array after trimming elements not equal to "c" from the end: [a, b, c, d, c]
```

### func trimStart(Array\<T>)

```cangjie
public func trimStart(set: Array<T>): Array<T>
```

Function: Trims the current array by removing elements from the start that are in the specified set `set`, until the first element not in `set` is encountered, and returns the slice of the current array.

Parameters:

- set: [Array](core_package_structs.md#struct-arrayt)\<T> - The set of elements to be removed.

Returns:

- [Array](core_package_structs.md#struct-arrayt)\<T> - The trimmed array slice.

Example:

<!-- verify -->
```cangjie
main(): Int64 {
    let arr = [1, 2, 1, 3, 1].trimStart([1, 2])
    println(arr)
    return 0
}
```

Execution Result:

```text
[3, 1]
```

### func trimStart((T)->Bool)

```cangjie
public func trimStart(predicate: (T)->Bool): Array<T>
```

Function: Trims the current array by removing elements from the start that satisfy the filtering condition, until the first element that does not satisfy the condition is encountered, and returns the slice of the current array.

Parameters:

- predicate: (T)->[Bool](./core_package_intrinsics.md#bool) - The filtering condition.

Returns:

- [Array](core_package_structs.md#struct-arrayt)\<T> - The trimmed array slice.

Example:

<!-- verify -->
```cangjie
main() {
    let arr = [1, 2, 3, 4, 5, 4, 3, 2, 1]
    
    println("Original array: ${arr}")
    
    // Remove all elements less than 4 from the start
    let trimmedArr = arr.trimStart({x => x < 4})
    println("Array after trimming elements less than 4 from the start: ${trimmedArr}")
    
    // Create a string array and test trimStart
    let strArr = ["a", "b", "c", "d", "c", "b", "a"]
    
    println("\nString array: ${strArr}")
    
    // Remove all elements not equal to "c" from the start
    let trimmedStrArr = strArr.trimStart({s => s != "c"})
    println("String array after trimming elements not equal to \"c\" from the start: ${trimmedStrArr}")
}
```

Execution Result:

```text
Original array: [1, 2, 3, 4, 5, 4, 3, 2, 1]
Array after trimming elements less than 4 from the start: [4, 5, 4, 3, 2, 1]

String array: [a, b, c, d, c, b, a]
String array after trimming elements not equal to "c" from the start: [c, d, c, b, a]
```

### operator func !=(Array\<T>)

```cangjie
public operator const func !=(that: Array<T>): Bool
```

Function: Determines whether the current instance is not equal to the specified [Array](core_package_structs.md#struct-arrayt)\<T> instance.

Parameters:

- that: [Array](core_package_structs.md#struct-arrayt)\<T> - Another [Array](core_package_structs.md#struct-arrayt)\<T> instance to compare with the current instance.

Returns:

- [Bool](core_package_intrinsics.md#bool) - Returns true if not equal; otherwise, returns false.

Example:

<!-- verify -->
```cangjie
main() {
    let arr1 = [1, 2, 3, 4, 5]
    let arr2 = [1, 2, 3, 4, 5]
    let arr3 = [1, 2, 3, 4, 6]
    let arr4 = [1, 2, 3]
    println("Array 1: ${arr1}")
    println("Array 2: ${arr2}")
    println("Array 3: ${arr3}")
    println("Array 4: ${arr4}")
    
    // Comparing equal arrays
    println("arr1 != arr2: ${arr1 != arr2}")
    
    // Comparing unequal arrays (different elements)
    println("arr1 != arr3: ${arr1 != arr3}")
    
    // Comparing unequal arrays (different lengths)
    println("arr1 != arr4: ${arr1 != arr4}")
    
    // Creating string arrays and testing the != operator
    let strArr1 = ["Hello", "World"]
    let strArr2 = ["Hello", "World"]
    let strArr3 = ["Hello", "Cangjie"]
    
    println("\nString array 1: ${strArr1}")
    println("String array 2: ${strArr2}")
    println("String array 3: ${strArr3}")
    
    // Comparing equal string arrays
    println("strArr1 != strArr2: ${strArr1 != strArr2}")
    
    // Comparing unequal string arrays
    println("strArr1 != strArr3: ${strArr1 != strArr3}")
}
```

Execution result:

```text
Array 1: [1, 2, 3, 4, 5]
Array 2: [1, 2, 3, 4, 5]
Array 3: [1, 2, 3, 4, 6]
Array 4: [1, 2, 3]
arr1 != arr2: false
arr1 != arr3: true
arr1 != arr4: true

String array 1: [Hello, World]
String array 2: [Hello, World]
String array 3: [Hello, Cangjie]
strArr1 != strArr2: false
strArr1 != strArr3: true
```

### operator func ==(Array\<T>)

```cangjie
public operator const func ==(that: Array<T>): Bool
```

Function: Determines whether the current instance is equal to the specified [Array](core_package_structs.md#struct-arrayt)\<T> instance.

Two [Array](core_package_structs.md#struct-arrayt)\<T> instances are considered equal if all their elements are equal.

Parameters:

- that: [Array](core_package_structs.md#struct-arrayt)\<T> - Another [Array](core_package_structs.md#struct-arrayt)\<T> instance to compare with the current instance.

Returns:

- [Bool](core_package_intrinsics.md#bool) - Returns true if equal, otherwise false.

Example:

<!-- verify -->
```cangjie
main() {
    let arr1 = [1, 2, 3, 4, 5]
    let arr2 = [1, 2, 3, 4, 5]
    let arr3 = [1, 2, 3, 4, 6]
    let arr4 = [1, 2, 3]
    
    println("Array 1: ${arr1}")
    println("Array 2: ${arr2}")
    println("Array 3: ${arr3}")
    println("Array 4: ${arr4}")
    
    // Comparing equal arrays
    println("arr1 == arr2: ${arr1 == arr2}")
    
    // Comparing unequal arrays (different elements)
    println("arr1 == arr3: ${arr1 == arr3}")
    
    // Comparing unequal arrays (different lengths)
    println("arr1 == arr4: ${arr1 == arr4}")
    
    // Creating string arrays and testing the == operator
    let strArr1 = ["Hello", "World"]
    let strArr2 = ["Hello", "World"]
    let strArr3 = ["Hello", "Cangjie"]
    
    println("\nString array 1: ${strArr1}")
    println("String array 2: ${strArr2}")
    println("String array 3: ${strArr3}")
    
    // Comparing equal string arrays
    println("strArr1 == strArr2: ${strArr1 == strArr2}")
    
    // Comparing unequal string arrays
    println("strArr1 == strArr3: ${strArr1 == strArr3}")
}
```

Execution result:

```text
Array 1: [1, 2, 3, 4, 5]
Array 2: [1, 2, 3, 4, 5]
Array 3: [1, 2, 3, 4, 6]
Array 4: [1, 2, 3]
arr1 == arr2: true
arr1 == arr3: false
arr1 == arr4: false

String array 1: [Hello, World]
String array 2: [Hello, World]
String array 3: [Hello, Cangjie]
strArr1 == strArr2: true
strArr1 == strArr3: false
```

## extend\<T> Array\<T> where T <: ToString

```cangjie
extend<T> Array<T> <: ToString where T <: ToString
```

Function: Extends the [Array](core_package_structs.md#struct-arrayt)\<T> type with the [ToString](core_package_interfaces.md#interface-tostring) interface, supporting string conversion operations.

Extended type:

- [Array\<T>](core_package_structs.md#struct-arrayt)

Extended interfaces:

- ToString

### func toString()

```cangjie
public func toString(): String
```

Function: Converts the array to an output-ready string.

The string format resembles "[1, 2, 3, 4, 5]"

Returns:

- [String](core_package_structs.md#struct-string) - The converted string.

Example:

<!-- verify -->
```cangjie
main() {
    let arr1 = [1, 2, 3, 4, 5]
    let arr2: Array<Int64> = []
    let arr3 = ["Hello", "World", "Cangjie"]
    
    println("Integer array: ${arr1}")
    println("toString() result: ${arr1.toString()}")
    
    println("\nEmpty array: ${arr2}")
    println("toString() result: ${arr2.toString()}")
    
    println("\nString array: ${arr3}")
    println("toString() result: ${arr3.toString()}")
    
    // Create a 2D array and test toString()
    let arr4 = [[1, 2], [3, 4], [5, 6]]
    
    println("\n2D array: ${arr4}")
    println("toString() result: ${arr4.toString()}")
}
```

Execution Result:

```text
Integer array: [1, 2, 3, 4, 5]
toString() result: [1, 2, 3, 4, 5]

Empty array: []
toString() result: []

String array: [Hello, World, Cangjie]
toString() result: [Hello, World, Cangjie]

2D array: [[1, 2], [3, 4], [5, 6]]
toString() result: [[1, 2], [3, 4], [5, 6]]
```

## extend\<T> Box\<T> <: Comparable\<Box\<T>> where T <: Comparable\<T>

```cangjie
extend<T> Box<T> <: Comparable<Box<T>> where T <: Comparable<T>
```

Function: Extends the [Box](core_package_classes.md#class-boxt)\<T> class with the [Comparable](core_package_interfaces.md#interface-comparablet)\<[Box](core_package_classes.md#class-boxt)\<T>> interface, providing comparison capabilities.

Extended type:

- [Box\<T>](core_package_classes.md#class-boxt)

Extended interfaces:

- Comparable\<Box\<T>>

### func compare(Box\<T>)

```cangjie
public func compare(that: Box<T>): Ordering
```

Function: Determines the size relationship between the current [Box](core_package_classes.md#class-boxt) instance and another [Box](core_package_classes.md#class-boxt) instance.

Parameters:

- that: [Box](core_package_classes.md#class-boxt)\<T> - The other [Box](core_package_classes.md#class-boxt) object to compare.

Returns:

- [Ordering](core_package_enums.md#enum-ordering) - Returns [Ordering](core_package_enums.md#enum-ordering).GT if the current [Box](core_package_classes.md#class-boxt) instance is greater than `that`, [Ordering](core_package_enums.md#enum-ordering).EQ if equal, and [Ordering](core_package_enums.md#enum-ordering).LT if less.

Example:

<!-- verify -->

```cangjie
struct Data <: Comparable<Data> {
    var a: Int64 = 0
    var b: Int64 = 0

    public init(a: Int64, b: Int64) {
        this.a = a
        this.b = b
    }

    public func compare(d: Data) {
        let tValue: Int64 = this.a + this.b
        let dValue: Int64 = d.a + d.b
        if (tValue > dValue) {
            return Ordering.GT
        } else if (tValue == dValue) {
            return Ordering.EQ
        } else {
            return Ordering.LT
        }
    }
}

main() {
    var data1: Box<Data> = Box<Data>(Data(12, 12))
    var data2: Box<Data> = Box<Data>(Data(7, 12))
    println(data1.compare(data2))
}
```

Output:

```text
Ordering.GT
```

### operator func !=(Box\<T>)

```cangjie
public operator func !=(that: Box<T>): Bool
```

Function: Compares whether [Box](core_package_classes.md#class-boxt) objects are unequal.

Parameters:

- that: [Box](core_package_classes.md#class-boxt)\<T> - The other [Box](core_package_classes.md#class-boxt) object to compare.

Returns:

- [Bool](core_package_intrinsics.md#bool) - Returns true if the current [Box](core_package_classes.md#class-boxt) object is not equal to the parameter [Box](core_package_classes.md#class-boxt) object, otherwise false.

Example:

<!-- verify -->
```cangjie
main() {
    var box1: Box<Int64> = Box<Int64>(42)
    var box2: Box<Int64> = Box<Int64>(42)
    var box3: Box<Int64> = Box<Int64>(100)
    
    // Compare equal Box objects
    println("box1 != box2: ${box1 != box2}")
    
    // Compare unequal Box objects
    println("box1 != box3: ${box1 != box3}")
}
```

Output:

```text
box1 != box2: false
box1 != box3: true
```

### operator func <(Box\<T>)

```cangjie
public operator func <(that: Box<T>): Bool
```

Function: Compares the size of [Box](core_package_classes.md#class-boxt) objects.

Parameters:

- that: [Box](core_package_classes.md#class-boxt)\<T> - The other [Box](core_package_classes.md#class-boxt) object to compare.

Returns:

- [Bool](core_package_intrinsics.md#bool) - Returns true if the current [Box](core_package_classes.md#class-boxt) object is less than the parameter [Box](core_package_classes.md#class-boxt) object, otherwise false.

Example:

<!-- verify -->
```cangjie
main() {
    var box1: Box<Int64> = Box<Int64>(42)
    var box2: Box<Int64> = Box<Int64>(100)
    
    // Compare Box object sizes
    println("box1 < box2: ${box1 < box2}")
    println("box2 < box1: ${box2 < box1}")
    println("box1 < box1: ${box1 < box1}")
}
```

Output:

```text
box1 < box2: true
box2 < box1: false
box1 < box1: false
```

### operator func <=(Box\<T>)

```cangjie
public operator func <=(that: Box<T>): Bool
```

Function: Compares the size of [Box](core_package_classes.md#class-boxt) objects.

Parameters:

- that: [Box](core_package_classes.md#class-boxt)\<T> - The other [Box](core_package_classes.md#class-boxt) object to compare.

Returns:

- [Bool](core_package_intrinsics.md#bool) - Returns true if the current [Box](core_package_classes.md#class-boxt) object is less than or equal to the parameter [Box](core_package_classes.md#class-boxt) object, otherwise false.

Example:

<!-- verify -->
```cangjie
main() {
    var box1: Box<Int64> = Box<Int64>(42)
    var box2: Box<Int64> = Box<Int64>(100)
    var box3: Box<Int64> = Box<Int64>(42)
    
    // Compare Box object sizes
    println("box1 <= box2: ${box1 <= box2}")
    println("box2 <= box1: ${box2 <= box1}")
    println("box1 <= box3: ${box1 <= box3}")
}
```

Output:

```text
box1 <= box2: true
box2 <= box1: false
box1 <= box3: true
```

### operator func ==(Box\<T>)

```cangjie
public operator func ==(that: Box<T>): Bool
```

Function: Compares whether [Box](core_package_classes.md#class-boxt) objects are equal.

Parameters:

- that: [Box](core_package_classes.md#class-boxt)\<T> - The other [Box](core_package_classes.md#class-boxt) object to compare.

Returns:

- [Bool](core_package_intrinsics.md#bool) - Returns true if the current [Box](core_package_classes.md#class-boxt) object equals the parameter [Box](core_package_classes.md#class-boxt) object, otherwise false.

Example:

<!-- verify -->
```cangjie
main() {
    var box1: Box<Int64> = Box<Int64>(42)
    var box2: Box<Int64> = Box<Int64>(42)
    var box3: Box<Int64> = Box<Int64>(100)
    
    // Compare equal Box objects
    println("box1 == box2: ${box1 == box2}")
    
    // Compare unequal Box objects
    println("box1 == box3: ${box1 == box3}")
}
```

Output:

```text
box1 == box2: true
box1 == box3: false
```

### operator func >(Box\<T>)

```cangjie
public operator func >(that: Box<T>): Bool
```

Function: Compares the size of [Box](core_package_classes.md#class-boxt) objects.

Parameters:

- that: [Box](core_package_classes.md#class-boxt)\<T> - The other [Box](core_package_classes.md#class-boxt) object to compare.

Returns:

- [Bool](core_package_intrinsics.md#bool) - Returns true if the current [Box](core_package_classes.md#class-boxt) object is greater than the parameter [Box](core_package_classes.md#class-boxt) object, otherwise false.

Example:

<!-- verify -->
```cangjie
main() {
    var box1: Box<Int64> = Box<Int64>(42)
    var box2: Box<Int64> = Box<Int64>(100)
    
    // Compare Box object sizes
    println("box1 > box2: ${box1 > box2}")
    println("box2 > box1: ${box2 > box1}")
    println("box1 > box1: ${box1 > box1}")
}
```

Output:

```text
box1 > box2: false
box2 > box1: true
box1 > box1: false
```

### operator func >=(Box\<T>)

```cangjie
public operator func >=(that: Box<T>): Bool
```

Function: Compares the size of [Box](core_package_classes.md#class-boxt) objects.

Parameters:

- that: [Box](core_package_classes.md#class-boxt)\<T> - The other [Box](core_package_classes.md#class-boxt) object to compare.

Returns:

- [Bool](core_package_intrinsics.md#bool) - Returns true if the current [Box](core_package_classes.md#class-boxt) object is greater than or equal to the parameter [Box](core_package_classes.md#class-boxt) object, otherwise false.

Example:

<!-- verify -->
```cangjie
main() {
    var box1: Box<Int64> = Box<Int64>(42)
    var box2: Box<Int64> = Box<Int64>(100)
    var box3: Box<Int64> = Box<Int64>(42)
    
    // Compare Box object sizes
    println("box1 >= box2: ${box1 >= box2}")
    println("box2 >= box1: ${box2 >= box1}")
    println("box1 >= box3: ${box1 >= box3}")
}
```

Output:

```text
box1 >= box2: false
box2 >= box1: true
box1 >= box3: true
```

## extend\<T> Box\<T> <: Hashable where T <: Hashable

```cangjie
extend<T> Box<T> <: Hashable where T <: Hashable
```

Function: Extends the [Box](core_package_classes.md#class-boxt)\<T> class with the [Hashable](core_package_interfaces.md#interface-hashable) interface, supporting hash value computation.

Extended type:

- [Box\<T>](core_package_classes.md#class-boxt)

Extended interfaces:

- Hashable

### func hashCode()

```cangjie
public func hashCode(): Int64
```

Function: Gets the hash value of the [Box](core_package_classes.md#class-boxt) object.

This value is actually the hash value of the wrapped `T` type instance.

Returns:

- [Int64](core_package_intrinsics.md#int64) - The hash value of the current [Box](core_package_classes.md#class-boxt) object.

Example:

<!-- verify -->
```cangjie
main() {
    var box1: Box<Int64> = Box<Int64>(42)
    var box2: Box<Int64> = Box<Int64>(42)
    var box3: Box<Int64> = Box<Int64>(100)
    
    // Get hash values of Box objects
    println("box1 hashCode: ${box1.hashCode()}")
    println("box2 hashCode: ${box2.hashCode()}")
    println("box3 hashCode: ${box3.hashCode()}")
    
    // Box objects with same values have same hash codes
    println("box1 and box2 have same hashCode: ${box1.hashCode() == box2.hashCode()}")
}
```

Output:

```text
box1 hashCode: 42
box2 hashCode: 42
box3 hashCode: 100
box1 and box2 have same hashCode: true
```

## extend\<T> Box\<T> <: ToString where T <: ToString

```cangjie
extend<T> Box<T> <: ToString where T <: ToString
```

Function: Extends the [Box](core_package_classes.md#class-boxt)\<T> type with the [ToString](core_package_interfaces.md#interface-tostring) interface, supporting string conversion operations.

Extended type:

- [Box\<T>](core_package_classes.md#class-boxt)

Extended interfaces:

- ToString

### func toString()

```cangjie
public func toString(): String
```

Function: Gets the string representation of the [Box](core_package_classes.md#class-boxt) object, which is the string representation of the wrapped `T` type instance.

Returns:

- [String](core_package_structs.md#struct-string) - The converted string.

Example:

<!-- verify -->
```cangjie
main() {
    var intBox: Box<Int64> = Box<Int64>(42)
    var stringBox: Box<String> = Box<String>("Hello")
    var boolBox: Box<Bool> = Box<Bool>(true)
    
    // Get string representations of Box objects
    println("intBox as string: ${intBox.toString()}")
    println("stringBox as string: ${stringBox.toString()}")
    println("boolBox as string: ${boolBox.toString()}")
}
```

Output:

```text
intBox as string: 42
stringBox as string: Hello
boolBox as string: true
```

## extend\<T> CPointer\<T>

```cangjie
extend<T> CPointer<T>
```

Function: Extends [CPointer](core_package_intrinsics.md#cpointert)\<T> with essential pointer-related interfaces including null checks and data read/write operations.

Extended type:

- [CPointer\<T>](core_package_intrinsics.md#cpointert)

### func asResource()

```cangjie
public func asResource(): CPointerResource<T>
```

Function: Gets the [CPointerResource](core_package_structs.md#struct-cpointerresourcet-where-t--ctype) instance for this pointer, which enables automatic resource release in `try-with-resource` syntax contexts.

Returns:

- [CPointerResource](core_package_structs.md#struct-cpointerresourcet-where-t--ctype)\<T> - The corresponding [CPointerResource](core_package_structs.md#struct-cpointerresourcet-where-t--ctype) instance for the current pointer.

Example:

<!-- verify -->
```cangjie
foreign func malloc(size: UIntNative): CPointer<Unit>

main() {
    let sizeofInt64: UIntNative = 8
    var p1 = unsafe { malloc(sizeofInt64) }
    var ptr = unsafe { CPointer<Int64>(p1) }
    unsafe { ptr.write(10) }
    var ptrResource: CPointerResource<Int64> = ptr.asResource()
    try (r = ptrResource) {
        var p = r.value
        let num: Int64 = unsafe { p.read() }
        println(num)
    }
    println(ptrResource.isClosed())
}
```

Execution result:

```text
10
true
```

### func isNotNull()

```cangjie
public func isNotNull(): Bool
```

Function: Checks if the pointer is not null.

Returns:

- [Bool](core_package_intrinsics.md#bool) - Returns true if not null, otherwise false.

Example:

<!-- verify -->
```cangjie
foreign func malloc(size: UIntNative): CPointer<Unit>

foreign func free(ptr: CPointer<Unit>): Unit

main() {
    let p1 = CPointer<Int64>()
    println(p1.isNotNull())
    let sizeofInt64: UIntNative = 8
    var p2 = unsafe { malloc(sizeofInt64) }
    println(p2.isNotNull())
    unsafe { free(p2) }
}
```

Execution result:

```text
false
true
```

### func isNull()

```cangjie
public func isNull(): Bool
```

Function: Checks if the pointer is null.

Returns:

- [Bool](core_package_intrinsics.md#bool) - Returns true if null, otherwise false.

Example:

<!-- verify -->
```cangjie
foreign func malloc(size: UIntNative): CPointer<Unit>

foreign func free(ptr: CPointer<Unit>): Unit

main() {
    let sizeofInt64: UIntNative = 8
    var p1 = unsafe { malloc(sizeofInt64) }
    var ptr = unsafe { CPointer<Int64>(p1) }
    unsafe { ptr.write(10) }
    let num: Int64 = unsafe { ptr.read() }
    println(num)
    unsafe { free(p1) }
}
```

Execution result:

```text
10
```

### func read()

```cangjie
public unsafe func read(): T
```

Function: Reads the first data element. Users must ensure pointer validity to avoid undefined behavior.

Returns:

- T - The first data element of the object type.

Example:

<!-- verify -->
```cangjie
foreign func malloc(size: UIntNative): CPointer<Unit>

foreign func free(ptr: CPointer<Unit>): Unit

main() {
    let p1 = CPointer<Int64>()
    println(p1.isNull())
    let sizeofInt64: UIntNative = 8
    var p2 = unsafe { malloc(sizeofInt64) }
    println(p2.isNull())
    unsafe { free(p2) }
}
```

Execution result:

```text
true
false
```

### func read(Int64)

```cangjie
public unsafe func read(idx: Int64): T
```

Function: Reads data at the specified index. Users must ensure pointer validity to avoid undefined behavior.

Parameters:

- idx: [Int64](core_package_intrinsics.md#int64) - The index of the data to read.

Returns:

- T - The data at the specified index.

Example:

<!-- verify -->
```cangjie
main() {
    var arr: Array<Int64> = [1, 2, 3, 4]
    var cptrHandle: CPointerHandle<Int64> = unsafe { acquireArrayRawData(arr) }
    var cptr: CPointer<Int64> = cptrHandle.pointer

    let num: Int64 = unsafe { cptr.read(2) }
    println("The third element of the array is ${num} ")

    unsafe { releaseArrayRawData<Int64>(cptrHandle) }
}
```

Execution result:

```text
The third element of the array is 3
```

### func toUIntNative()

```cangjie
public func toUIntNative(): UIntNative
```

Function: Gets the integer representation of the pointer.

Returns:

- [UIntNative](core_package_intrinsics.md#uintnative) - The integer representation of the pointer.

Example:

<!-- run -->
```cangjie
foreign func malloc(size: UIntNative): CPointer<Unit>

foreign func free(ptr: CPointer<Unit>): Unit

main() {
    let sizeofInt64: UIntNative = 8
    var p = unsafe { malloc(sizeofInt64) }
    var p1 = unsafe { CPointer<Int64>(p) }
    unsafe { p1.write(8) }
    println(p1.toUIntNative())
    unsafe { free(p) }
}
```

Execution result:

```text
93954490863648
```

### func write(Int64, T)

```cangjie
public unsafe func write(idx: Int64, value: T): Unit
```

Function: Writes data at the specified index. Users must ensure pointer validity to avoid undefined behavior.

Parameters:

- idx: [Int64](core_package_intrinsics.md#int64) - The index position to write.
- value: T - The data to write.

Example:

<!-- verify -->
```cangjie
main() {
    var arr: Array<Int64> = [1, 2, 3, 4]
    var cptrHandle: CPointerHandle<Int64> = unsafe { acquireArrayRawData(arr) }

    var cptr: CPointer<Int64> = cptrHandle.pointer

    unsafe { cptr.write(2, 6) }

    println("The third element of the array is ${arr[2]} ")
    // The first element of the array is 1
    unsafe { releaseArrayRawData<Int64>(cptrHandle) }
}
```

Execution result:

```text
The third element of the array is 6
```

### func write(T)

```cangjie
public unsafe func write(value: T): Unit
```

Function: Writes data at the first position. Users must ensure pointer validity to avoid undefined behavior.

Parameters:

- value: T - The data to write.

Example:

<!-- verify -->
```cangjie
foreign func malloc(size: UIntNative): CPointer<Unit>

foreign func free(ptr: CPointer<Unit>): Unit

main() {
    let sizeofInt64: UIntNative = 8
    var p = unsafe { malloc(sizeofInt64) }
    var p1 = unsafe { CPointer<Int64>(p) }
    unsafe { p1.write(8) }
    let value: Int64 = unsafe { p1.read() }
    println(value)
    unsafe { free(p) }
}
```

Execution result:

```text
8
```

### operator func +(Int64)

```cangjie
public unsafe operator func +(offset: Int64): CPointer<T>
```

Function: Moves the [CPointer](core_package_intrinsics.md#cpointert) pointer forward, equivalent to C's pointer addition operation.

Parameters:

- offset: [Int64](core_package_intrinsics.md#int64) - The offset value.

Returns:

- [CPointer](core_package_intrinsics.md#cpointert)\<T> - The pointer after offset.

Example:

<!-- verify -->
```cangjie
main() {
    var arr: Array<Int64> = [1, 2, 3, 4]
    var cptrHandle: CPointerHandle<Int64> = unsafe { acquireArrayRawData(arr) }
    var cptr: CPointer<Int64> = cptrHandle.pointer

    let num1: Int64 = unsafe { cptr.read() }
    println(num1)
    cptr = unsafe { cptr + 1 }
    let num2: Int64 = unsafe { cptr.read() }
    println(num2)
    unsafe { releaseArrayRawData<Int64>(cptrHandle) }
}
```

Execution result:

```text
1
2
```

### operator func -(Int64)

```cangjie
public unsafe operator func -(offset: Int64): CPointer<T>
```

Function: Moves the [CPointer](core_package_intrinsics.md#cpointert) pointer backward, equivalent to C's pointer subtraction operation.

Parameters:

- offset: [Int64](core_package_intrinsics.md#int64) - The offset value.

Returns:

- [CPointer](core_package_intrinsics.md#cpointert)\<T> - The pointer after offset.

Example:

<!-- verify -->
```cangjie
main() {
    var arr: Array<Int64> = [1, 2, 3, 4]
    var cptrHandle: CPointerHandle<Int64> = unsafe { acquireArrayRawData(arr) }
    var cptr: CPointer<Int64> = cptrHandle.pointer

    let num1: Int64 = unsafe { cptr.read() }
    println(num1)
    cptr = unsafe { cptr + 1 }
    let num2: Int64 = unsafe { cptr.read() }
    println(num2)
    cptr = unsafe { cptr - 1 }
    let num3: Int64 = unsafe { cptr.read() }
    println(num3)
    unsafe { releaseArrayRawData<Int64>(cptrHandle) }
}
```

Execution result:

```text
1
2
1
```

## extend\<T> Iterator\<T>

```cangjie
extend<T> Iterator<T>
```

Function: Extends the [Iterator](core_package_classes.md#class-iteratort)\<T> type.

Extended type:

- [Iterator\<T>](core_package_classes.md#class-iteratort)

### func all((T) -> Bool)

```cangjie
public func all(predicate: (T)-> Bool): Bool
```

Function: Checks if all elements in the iterator satisfy the condition. This method repeatedly retrieves and consumes elements until an element fails the condition.

Parameters:

- predicate: (T) -> [Bool](../../core/core_package_api/core_package_intrinsics.md#bool) - The given condition.

Returns:

- [Bool](../../core/core_package_api/core_package_intrinsics.md#bool) - Whether all elements satisfy the condition.

Example:

<!-- verify -->

```cangjie
main(): Int64 {
    var arr: Array<Int64> = [1, 2, 3, 4, 5]

    /* Get the container's iterator object */
    var iter = arr.iterator()
    var flag: Bool = iter.all({v: Int64 => v > 0})
    println(flag)
    return 0
}
```

Output:

```text
true
```

### func any((T) -> Bool)

```cangjie
public func any(predicate: (T)-> Bool): Bool
```

Function: Checks if any element in the iterator satisfies the condition. This method repeatedly retrieves and consumes elements until an element meets the condition.

Parameters:

- predicate: (T) -> [Bool](../../core/core_package_api/core_package_intrinsics.md#bool) - The given condition.

Returns:

- [Bool](../../core/core_package_api/core_package_intrinsics.md#bool) - Whether any element satisfies the condition.

Example:

<!-- verify -->

```cangjie
main(): Int64 {
    var arr: Array<Int64> = [1, 2, 3, 4, 5]

    /* Get the container's iterator object */
    var iter = arr.iterator()
    var flag: Bool = iter.any({v: Int64 => v > 4})
    println(flag)
    return 0
}
```

Output:

```text
true
```

### func at(Int64)

```cangjie
public func at(n: Int64): Option<T>
```

Function: Retrieves the nth element of the current iterator (indexing starts at 0). This method consumes all elements before the specified one (including the specified element).

Parameters:

- n: [Int64](../../core/core_package_api/core_package_intrinsics.md#int64) - The element index (0-based).

Returns:

- [Option](../../core/core_package_api/core_package_enums.md#enum-optiont)\<T> - The element at the specified position; returns None if n exceeds the remaining elements.

Example:

<!-- verify -->

```cangjie
main(): Int64 {
    var arr: Array<Int64> = [1, 2, 3, 4, 5]

    /* Get the container's iterator object */
    var iter = arr.iterator()
    var num: Option<Int64> = iter.at(2)
    println(num)
    return 0
}
```

Output:

```text
Some(3)
```

### func concat(Iterator\<T>)

```cangjie
public func concat(other: Iterator<T>): Iterator<T>
```

Function: Concatenates two iterators, with the current iterator first and the parameter iterator second.

Parameters:

- other: [Iterator](../../core/core_package_api/core_package_classes.md#class-iteratort)\<T> - The iterator to concatenate after.

Returns:

- [Iterator](../../core/core_package_api/core_package_classes.md#class-iteratort)\<T> - The concatenated new iterator.

Example:

<!-- verify -->

```cangjie
main(): Int64 {
    var arr1: Array<Int64> = [1, 2]
    var arr2: Array<Int64> = [3, 4]

    /* Get the container's iterator objects */
    var iter1 = arr1.iterator()
    var iter2 = arr2.iterator()

    /* Merge two iterators */
    var iter = iter1.concat(iter2)

    /* Traverse using the merged iterator */
    while (true) {
        match (iter.next()) {
            case Some(i) => println(i)
            case None => break
        }
    }
    return 0
}
```

Output:

```text
1
2
3
4
```

### func count()

```cangjie
public func count(): Int64
```

Function: Counts the number of elements in the current iterator. This method consumes all elements to compute the count.

> **Note:**
>
> This method consumes the iterator, meaning no elements remain after its use.

Returns:

- [Int64](../../core/core_package_api/core_package_intrinsics.md#int64) - The number of elements in the iterator.

Example:

<!-- verify -->

```cangjie
main(): Int64 {
    var arr: Array<Int64> = [1, 2]

    /* Get the container's iterator object */
    var iter = arr.iterator()
    let len: Int64 = iter.count()
    println(len)

    /* Attempt to traverse, but count() consumed all elements */
    while (true) {
        match (iter.next()) {
            case Some(i) => println(i)
            case None => break
        }
    }
    return 0
}
```

Output:

```text
2
```

### func enumerate()

```cangjie
public func enumerate(): Iterator<(Int64, T)>
```

Function: Creates an iterator with indices.

Returns:

- [Iterator](../../core/core_package_api/core_package_classes.md#class-iteratort)\<([Int64](../../core/core_package_api/core_package_intrinsics.md#int64), T)> - Returns an iterator with indices.

Example:

<!-- verify -->

```cangjie
main(): Int64 {
    var arr: Array<Int64> = [1, 2]

    /* Get the container's iterator object */
    var iter = arr.iterator()
    var iter1 = iter.enumerate()

    /* Traverse using the indexed iterator */
    while (true) {
        match (iter1.next()) {
            case Some(i) =>
                print(i[0].toString() + ' ')
                print(i[1])
                println()
            case None => break
        }
    }
    return 0
}
```

Output:

```text
0 1
1 2
```

### func filter((T) -> Bool)

```cangjie
public func filter(predicate: (T)-> Bool): Iterator<T>
```

Function: Filters elements that satisfy the condition.

Parameters:

- predicate: (T) -> [Bool](../../core/core_package_api/core_package_intrinsics.md#bool) - The given condition; elements returning `true` appear in order in the returned iterator.

Returns:

- [Iterator](../../core/core_package_api/core_package_classes.md#class-iteratort)\<T> - A new iterator.

Example:

<!-- verify -->

```cangjie
main(): Int64 {
    var arr: Array<Int64> = [1, 2, 3, 4, 5]

    /* Get a filtered iterator object */
    var iter = arr.iterator()
    var iter1 = iter.filter({value: Int64 => value > 2})

    /* Traverse using the filtered iterator */
    while (true) {
        match (iter1.next()) {
            case Some(i) => println(i)
            case None => break
        }
    }
    return 0
}
```

Output:

```text
3
4
5
```

### func filterMap\<R>((T) -> Option\<R>)

```cangjie
public func filterMap<R>(transform: (T) -> Option<R>): Iterator<R>
```

Function: Performs both filtering and mapping, returning a new iterator.

Parameters:

- transform: (T) -> [Option](../../core/core_package_api/core_package_enums.md#enum-optiont)\<R> - The mapping function. A return value of Some corresponds to a predicate of true in filter; otherwise, it's false.

Returns:

- [Iterator](../../core/core_package_api/core_package_classes.md#class-iteratort)\<R> - A new iterator.

Example:

<!-- verify -->

```cangjie
main(): Int64 {
    var arr: Array<Int64> = [1, 2, 3, 4, 5]

    /* Get a filtered and mapped iterator; mapping must return Option type */
    var iter = arr.iterator()
    var iter1 = iter.filterMap({
        value: Int64 => if (value > 2) {
            return Some(value + 1)
        } else {
            return None<Int64>
        }
    })

    /* Traverse using the new iterator */
    while (true) {
        match (iter1.next()) {
            case Some(i) => println(i)
            case None => break
        }
    }
    return 0
}
```

Output:

```text
4
5
6
```

### func first()

```cangjie
public func first(): Option<T>
```

Function: Retrieves the first element of the current iterator. This method consumes the first element.

Returns:

- [Option](../../core/core_package_api/core_package_enums.md#enum-optiont)\<T> - The first element; returns None if empty.

Example:

<!-- verify -->

```cangjie
main(): Int64 {
    var arr: Array<Int64> = [1, 2, 3, 4, 5]

    /* Get iterator object */
    var iter = arr.iterator()
    var head: Option<Int64> = iter.first()
    println(head)

    return 0
}
```

Execution result:

```text
Some(1)
```

### func flatMap\<R>((T) -> Iterator\<R>)

```cangjie
public func flatMap<R>(transform: (T) -> Iterator<R>): Iterator<R>
```

Function: Creates a mapping with [flatten](../../collection/collection_package_api/collection_package_function.md#func-flattent-riterablet-where-t--iterabler) capability.

Parameters:

- transform: (T) -> [Iterator](../../core/core_package_api/core_package_classes.md#class-iteratort)\<R> - The given mapping function.

Returns:

- [Iterator](../../core/core_package_api/core_package_classes.md#class-iteratort)\<R> - Returns a mapping with [flatten](../../collection/collection_package_api/collection_package_function.md#func-flattent-riterablet-where-t--iterabler) capability.

Example:

<!-- verify -->

```cangjie
main(): Int64 {
    var arr: Array<Array<Int64>> = [[1], [2], [3], [4, 5]]

    /* Get iterator object with flatten capability */
    var iter = arr.iterator()
    var iter1 = iter.flatMap({value => value.iterator()})

    /* Perform flattened traversal using the iterator */
    while (true) {
        match (iter1.next()) {
            case Some(i) => println(i)
            case None => break
        }
    }
    return 0
}
```

Execution result:

```text
1
2
3
4
5
```

### func fold\<R>(R, (R, T) -> R)

```cangjie
public func fold<R>(initial: R, operation: (R, T)->R): R
```

Function: Computes from left to right using the specified initial value. This method consumes all elements in the iterator.

Parameters:

- initial: R - The given initial value of type R.
- operation: (R, T) -> R - The given computation function.

Returns:

- R - Returns the final computed value.

Example:

<!-- verify -->

```cangjie
main(): Int64 {
    var arr: Array<Int64> = [1, 2, 3, 4, 5]

    /* Get iterator object and sum the array elements */
    var iter = arr.iterator()
    var sum: Int64 = iter.fold(0, {total, value => total + value})

    println(sum)
    return 0
}
```

Execution result:

```text
15
```

### func forEach((T) -> Unit)

```cangjie
public func forEach(action: (T)-> Unit): Unit
```

Function: Traverses all elements of the current iterator and performs the given operation on each element. This method consumes all elements in the iterator.

Parameters:

- action: (T) -> [Unit](../../core/core_package_api/core_package_intrinsics.md#unit) - The given operation function.

Example:

<!-- verify -->

```cangjie
main(): Int64 {
    var arr: Array<Int64> = [1, 2, 3, 4, 5]

    var iter = arr.iterator()
    iter.forEach({value => println(value)})

    return 0
}
```

Execution result:

```text
1
2
3
4
5
```

### func inspect((T) -> Unit)

```cangjie
public func inspect(action: (T) -> Unit): Iterator<T>
```

Function: Performs an additional operation on the current element each time the iterator calls next() (does not consume elements in the iterator).

Parameters:

- action: (T) -> [Unit](../../core/core_package_api/core_package_intrinsics.md#unit) - The given operation function.

Returns:

- [Iterator](../../core/core_package_api/core_package_classes.md#class-iteratort)\<T> - Returns a new iterator.

Example:

<!-- verify -->

```cangjie
main(): Int64 {
    var arr: Array<Int64> = [1, 2]

    /* Get iterator object and attach additional operations to the next function */
    var iter = arr.iterator()
    var iter1 = iter.inspect({value => println("Logging: Processing ${value}")})

    /* Perform traversal using the iterator */
    while (true) {
        match (iter1.next()) {
            case Some(i) => println("Processing ${i} !")
            case None => break
        }
    }
    return 0
}
```

Execution result:

```text
Logging: Processing 1
Processing 1 !
Logging: Processing 2
Processing 2 !
```

### func intersperse(T)

```cangjie
public func intersperse(separator: T): Iterator<T>
```

Function: Inserts a given new element between every two elements in the iterator.

Parameters:

- separator: T - The given element.

Returns:

- [Iterator](../../core/core_package_api/core_package_classes.md#class-iteratort)\<T> - Returns a new iterator.

Example:

<!-- verify -->

```cangjie
main(): Int64 {
    var arr: Array<Int64> = [1, 2]

    /* Get iterator object and insert a 0 between every two elements */
    var iter = arr.iterator()
    var iter1 = iter.intersperse(0)

    /* Perform traversal using the iterator */
    while (true) {
        match (iter1.next()) {
            case Some(i) => println(i)
            case None => break
        }
    }
    return 0
}
```

Execution result:

```text
1
0
2
```

### func isEmpty()

```cangjie
public func isEmpty(): Bool
```

Function: Determines whether the current iterator is empty. This method calls [next()](core_package_classes.md#func-next-1) and determines whether the current iterator is empty based on its return value. Therefore, if the current iterator is not empty, one element will be consumed.

Returns:

- [Bool](../../core/core_package_api/core_package_intrinsics.md#bool) - Returns whether the current iterator is empty.

Example:

<!-- verify -->

```cangjie
main(): Int64 {
    var arr: Array<Int64> = [1, 2]

    /* Get iterator object */
    var iter = arr.iterator()

    /* Check if the iterator has elements; if yes, one element will be consumed */
    println(iter.isEmpty())

    /* Perform traversal using the iterator */
    while (true) {
        match (iter.next()) {
            case Some(i) => println(i)
            case None => break
        }
    }
    println(iter.isEmpty())
    return 0
}
```

Execution result:

```text
false
2
true
```

### func last()

```cangjie
public func last(): Option<T>
```

Function: Gets the tail element of the current iterator. This method retrieves and consumes all elements in the iterator and returns the last element.

Returns:

- [Option](../../core/core_package_api/core_package_enums.md#enum-optiont)\<T> - Returns the tail element, or None if empty.

Example:

<!-- verify -->

```cangjie
main(): Int64 {
    var arr: Array<Int64> = [1, 2]

    /* Get iterator object */
    var iter = arr.iterator()
    println(iter.last())
    return 0
}
```

Execution result:

```text
Some(2)
```

### func map\<R>((T) -> R)

```cangjie
public func map<R>(transform: (T)-> R): Iterator<R>
```

Function: Creates a mapping.

Parameters:

- transform: (T) ->R - The given mapping function.

Returns:

- [Iterator](../../core/core_package_api/core_package_classes.md#class-iteratort)\<R> - Returns a mapping.

Example:

<!-- verify -->

```cangjie
main(): Int64 {
    var arr: Array<Int64> = [1, 2, 3, 4]

    /* Get iterator object, map the elements, and obtain a new iterator object */
    var iter = arr.iterator()
    var iter1 = iter.map({value => value * 2})

    /* Perform traversal using the iterator */
    while (true) {
        match (iter1.next()) {
            case Some(i) => println(i)
            case None => break
        }
    }
    return 0
}
```

Execution result:

```text
2
4
6
8
```

### func none((T) -> Bool)

```cangjie
public func none(predicate: (T)-> Bool): Bool
```

Function: Determines whether none of the elements in the current iterator satisfy the condition. This method repeatedly retrieves and consumes elements in the iterator until an element satisfies the condition.

Parameters:

- predicate: (T) -> [Bool](../../core/core_package_api/core_package_intrinsics.md#bool) - The given condition.

Returns:

- [Bool](../../core/core_package_api/core_package_intrinsics.md#bool) - Whether none of the elements in the iterator satisfy the condition.

Example:

<!-- verify -->

```cangjie
main(): Int64 {
    var arr: Array<Int64> = [1, 2, 3, 4]

    /* Get iterator object, map the elements, and obtain a new iterator object */
    var iter1 = arr.iterator()
    var iter2 = arr.iterator()

    /* There exists an element greater than 2, returns false */
    var flag1: Bool = iter1.none({value => value > 2})
    println(flag1)

    /* No element is greater than 5, returns true */
    var flag2: Bool = iter2.none({value => value > 5})
    println(flag2)
    return 0
}
```

Execution Result:

```text
false
true
```

### func reduce((T, T) -> T)

```cangjie
public func reduce(operation: (T, T) -> T): Option<T>
```

Function: Performs left-to-right reduction using the first element as the initial value. This method consumes all elements in the iterator.

Parameters:

- operation: (T, T) -> T - The specified computation function.

Returns:

- [Option](../../core/core_package_api/core_package_enums.md#enum-optiont)\<T> - Returns the computation result.

Example:

<!-- verify -->

```cangjie
main(): Int64 {
    var arr: Array<Int64> = [1, 2, 3, 4, 5]

    /* Obtain an iterator object and sum the array elements */
    var iter = arr.iterator()
    var sum: Option<Int64> = iter.reduce({total, value => total + value})
    println(sum)
    return 0
}
```

Execution Result:

```text
Some(15)
```

### func skip(Int64)

```cangjie
public func skip(count: Int64): Iterator<T>
```

Function: Skips a specified number of elements from the current iterator in a forward direction.

Throws an exception when count < 0. When count = 0, it effectively skips no elements and returns the original iterator. When count > 0 and count < the iterator's size, it skips 'count' elements and returns a new iterator containing the remaining elements. When count ≥ the iterator's size, it skips all elements and returns an empty iterator.

Parameters:

- count: [Int64](../../core/core_package_api/core_package_intrinsics.md#int64) - The number of elements to skip.

Returns:

- [Iterator](../../core/core_package_api/core_package_classes.md#class-iteratort)\<T> - Returns an iterator that skips the specified number of elements.

Exceptions:

- [IllegalArgumentException](../../core/core_package_api/core_package_exceptions.md#class-illegalargumentexception) - Thrown when count < 0.

Example:

<!-- verify -->

```cangjie
main(): Int64 {
    var arr: Array<Int64> = [1, 2, 3, 4, 5]

    /* Obtain an iterator object and skip the first two elements */
    var iter = arr.iterator()
    var iter1 = iter.skip(2)

    /* Perform iterative traversal using the iterator */
    while (true) {
        match (iter1.next()) {
            case Some(i) => println(i)
            case None => break
        }
    }
    return 0
}
```

Execution Result:

```text
3
4
5
```

### func step(Int64)

```cangjie
public func step(count: Int64): Iterator<T>
```

Function: Skips a specified number of elements each time next() is called on the iterator.

Throws an exception when count ≤ 0. When count > 0, each call to next() skips 'count' elements until the iterator is exhausted.

Parameters:

- count: [Int64](../../core/core_package_api/core_package_intrinsics.md#int64) - The number of elements to skip per next() call.

Returns:

- [Iterator](../../core/core_package_api/core_package_classes.md#class-iteratort)\<T> - Returns a new iterator that skips the specified number of elements per next() call.

Exceptions:

- [IllegalArgumentException](../../core/core_package_api/core_package_exceptions.md#class-illegalargumentexception) - Thrown when count ≤ 0.

Example:

<!-- verify -->

```cangjie
main(): Int64 {
    var arr: Array<Int64> = [1, 2, 3, 4, 5]

    /* Obtain an iterator object that skips two elements per next() call */
    var iter = arr.iterator()
    var iter1 = iter.step(2)

    /* Perform iterative traversal using the iterator */
    while (true) {
        match (iter1.next()) {
            case Some(i) => println(i)
            case None => break
        }
    }
    return 0
}
```

Execution Result:

```text
1
3
5
```

### func take(Int64)

```cangjie
public func take(count: Int64): Iterator<T>
```

Function: Takes a specified number of elements from the current iterator.

Takes elements from the current iterator in a forward direction. Throws an exception when count < 0. When count = 0, takes no elements and returns an empty iterator. When 0 < count < the iterator's size, takes the first 'count' elements and returns a new iterator. When count ≥ the iterator's size, takes all elements and returns the original iterator.

Parameters:

- count: [Int64](../../core/core_package_api/core_package_intrinsics.md#int64) - The number of elements to take.

Returns:

- [Iterator](../../core/core_package_api/core_package_classes.md#class-iteratort)\<T> - Returns an iterator containing the specified number of elements.

Exceptions:

- [IllegalArgumentException](../../core/core_package_api/core_package_exceptions.md#class-illegalargumentexception) - Thrown when count < 0.

Example:

<!-- verify -->

```cangjie
main(): Int64 {
    var arr: Array<Int64> = [1, 2, 3, 4, 5]

    /* Obtain an iterator object and take the first three elements */
    var iter = arr.iterator()
    var iter1 = iter.take(3)

    /* Perform iterative traversal using the iterator */
    while (true) {
        match (iter1.next()) {
            case Some(i) => println(i)
            case None => break
        }
    }
    return 0
}
```

Execution Result:

```text
1
2
3
```

### func zip\<R>(Iterator\<R>)

```cangjie
public func zip<R>(it: Iterator<R>): Iterator<(T, R)>
```

Function: Merges two iterators into one (length determined by the shorter iterator).

Parameters:

- it: [Iterator](../../core/core_package_api/core_package_classes.md#class-iteratort)\<R> - One of the iterators to merge.

Returns:

- [Iterator](../../core/core_package_api/core_package_classes.md#class-iteratort)\<(T, R)> - Returns a new iterator.

Example:

<!-- verify -->

```cangjie
main(): Int64 {
    var arr1: Array<Int64> = [1, 2, 3, 4]
    var arr2: Array<Int64> = [4, 5, 6]

    /* Obtain iterator objects and merge them; elements in the new iterator are tuples of corresponding index positions */
    var iter1 = arr1.iterator()
    var iter2 = arr2.iterator()
    var iter = iter1.zip(iter2)

    /* Perform traversal using the iterator; length depends on the shorter iterator */
    while (true) {
        match (iter.next()) {
            case Some(i) => println("The current element is (${i[0]}, ${i[1]})")
            case None => break
        }
    }
    return 0
}
```

Execution Result:

```text
The current element is (1, 4)
The current element is (2, 5)
The current element is (3, 6)
```

## extend\<T> Iterator\<T> where T <: Comparable\<T>

```cangjie
extend<T> Iterator<T> where T <: Comparable<T>
```

Function: Extends the [Iterator](core_package_classes.md#class-iteratort)\<T> type with the [Comparable](core_package_interfaces.md#interface-comparablet)\<T> interface to support comparison operations.

Extended type:

- [Iterator\<T>](core_package_classes.md#class-iteratort)

### func max()

```cangjie
public func max(): Option<T>
```

Function: Filters the maximum element. This method consumes all elements in the iterator.

Returns:

- [Option](core_package_enums.md#enum-optiont)\<T> - Returns the maximum element, or None if empty.

Example:

<!-- verify -->

```cangjie
main(): Int64 {
    var arr: Array<Int64> = [1, 2, 3, 4]

    /* Get iterator object and use max() to find the maximum value */
    var iter = arr.iterator()
    match (iter.max()) {
        case Some(i) => println(i)
        case None => println("None!")
    }
    return 0
}
```

Execution result:

```text
4
```

### func min()

```cangjie
public func min(): Option<T>
```

Function: Filters the minimum element. This method consumes all elements in the iterator.

Returns:

- [Option](core_package_enums.md#enum-optiont)\<T> - Returns the minimum element, or None if empty.

Example:

<!-- verify -->

```cangjie
main(): Int64 {
    var arr: Array<Int64> = [1, 2, 3, 4]

    /* Get iterator object and use min() to find the minimum value */
    var iter = arr.iterator()
    match (iter.min()) {
        case Some(i) => println(i)
        case None => println("None!")
    }
    return 0
}
```

Execution result:

```text
1
```

## extend\<T> Iterator\<T> where T <: Equatable\<T>

```cangjie
extend<T> Iterator<T> where T <: Equatable<T>
```

Function: Extends the [Iterator](core_package_classes.md#class-iteratort)\<T> type with the [Equatable](core_package_interfaces.md#interface-equatablet)\<T> interface to support equality operations.

Extended type:

- [Iterator\<T>](core_package_classes.md#class-iteratort)

### func contains(T)

```cangjie
public func contains(element: T): Bool
```

Function: Traverses all elements to determine if the specified element is contained. This method repeatedly fetches and consumes elements from the iterator until an element equal to the parameter `element` is found.

Parameters:

- element: T - The element to search for.

Returns:

- [Bool](../../core/core_package_api/core_package_intrinsics.md#bool) - Whether the specified element is contained.

Example:

<!-- verify -->

```cangjie
main(): Int64 {
    var arr: Array<Int64> = [1, 2, 3, 4]

    /* Get iterator object and check if it contains element 3 */
    var iter = arr.iterator()
    println(iter.contains(3))

    /* Use iterator to traverse and output remaining elements */
    while (true) {
        match (iter.next()) {
            case Some(i) => println(i)
            case None => break
        }
    }
    return 0
}
```

Execution result:

```text
true
4
```

## extend\<T> Option\<Option\<T>>

```cangjie
extend<T> Option<Option<T>>
```

Function: Extends certain functionalities for the Option\<Option\<T>> type.

Extended type:

- [Option\<Option\<T>>](core_package_enums.md#enum-optiont)

### func flatten()

```cangjie
public func flatten(): Option<T>
```

Function: Flattens the [Option](core_package_enums.md#enum-optiont)\<[Option](core_package_enums.md#enum-optiont)\<T>> type. If the current instance is [Some](core_package_enums.md#somet)([Option](core_package_enums.md#enum-optiont)\<T>.[Some](core_package_enums.md#somet)(v)), the flattened result is [Some](core_package_enums.md#somet)(v).

Returns:

- [Option](core_package_enums.md#enum-optiont)\<T> - The flattened result of [Option](core_package_enums.md#enum-optiont)\<[Option](core_package_enums.md#enum-optiont)\<T>> type.

Example:

<!-- verify -->
```cangjie
main() {
    // Create a Some(Some(42)) value of Option<Option<Int64>> type
    var nestedSome: Option<Option<Int64>> = Some(Some(42))
    
    // Flatten the nested Option
    var flattened1 = nestedSome.flatten()
    println("Some(Some(42)) flattened: ${flattened1}")
    
    // Create a Some(None) value of Option<Option<Int64>> type
    var someNone: Option<Option<Int64>> = Some(None)
    
    // Flatten the nested Option
    var flattened2 = someNone.flatten()
    println("Some(None) flattened: ${flattened2}")
    
    // Create a None value of Option<Option<Int64>> type
    var noneValue: Option<Option<Int64>> = None
    
    // Flatten the nested Option
    var flattened3 = noneValue.flatten()
    println("None flattened: ${flattened3}")
    
    // Demonstrate chained calls
    var chainedValue: Option<Option<Option<String>>> = Some(Some(Some("Hello")))
    var flattenedChained = chainedValue.flatten().flatten()
    println("Chained flattening of Some(Some(Some(\"Hello\"))): ${flattenedChained}")
}
```

Execution Result:

```text
Some(Some(42)) flattened: Some(42)
Some(None) flattened: None
None flattened: None
Chained flattening of Some(Some(Some("Hello"))): Some(Hello)
```

## extend\<T> Option\<T> <: Equatable\<Option\<T>> where T <: Equatable\<T>

```cangjie
extend<T> Option<T> <: Equatable<Option<T>> where T <: Equatable<T>
```

Function: Extends the [Equatable](core_package_interfaces.md#interface-equatablet)\<[Option](core_package_enums.md#enum-optiont)\<T>> interface for the [Option](core_package_enums.md#enum-optiont)\<T> enum to support equality comparison operations.

Extended type:

- [Option\<T>](core_package_enums.md#enum-optiont)

Extended interfaces:

- Equatable\<Option\<T>>

### operator func !=(Option\<T>)

```cangjie
public operator func !=(that: Option<T>): Bool
```

Function: Determines whether the current instance is not equal to the [Option](core_package_enums.md#enum-optiont)\<T> instance pointed to by the parameter.

Parameters:

- that: [Option](core_package_enums.md#enum-optiont)\<T> - The [Option](core_package_enums.md#enum-optiont)\<T> instance to compare.

Returns:

- [Bool](core_package_intrinsics.md#bool) - Returns true if not equal, otherwise returns false.

Example:

<!-- verify -->
```cangjie
main() {
    // Create identical Some values
    var someValue1: Option<Int64> = Some(42)
    var someValue2: Option<Int64> = Some(42)
    
    // Compare two identical Some values
    println("Some(42) != Some(42): ${someValue1 != someValue2}")
    
    // Create different Some values
    var someValue3: Option<Int64> = Some(42)
    var someValue4: Option<Int64> = Some(24)
    
    // Compare two different Some values
    println("Some(42) != Some(24): ${someValue3 != someValue4}")
    
    // Create a Some value and a None value
    var someValue5: Option<Int64> = Some(42)
    var noneValue1: Option<Int64> = None
    
    // Compare Some value with None value
    println("Some(42) != None: ${someValue5 != noneValue1}")
    
    // Create two None values
    var noneValue2: Option<Int64> = None
    var noneValue3: Option<Int64> = None
    
    // Compare two None values
    println("None != None: ${noneValue2 != noneValue3}")
}
```

Execution Result:

```text
Some(42) != Some(42): false
Some(42) != Some(24): true
Some(42) != None: true
None != None: false
```

### operator func ==(Option\<T>)

```cangjie
public operator func ==(that: Option<T>): Bool
```

Function: Determines whether the current instance is equal to the [Option](core_package_enums.md#enum-optiont)\<T> instance pointed to by the parameter.

If both are None, they are equal; if both are Some(v1) and Some(v2), and v1 equals v2, they are equal.

Parameters:

- that: [Option](core_package_enums.md#enum-optiont)\<T> - The [Option](core_package_enums.md#enum-optiont)\<T> instance to compare.

Returns:

- [Bool](core_package_intrinsics.md#bool) - Returns true if equal, otherwise returns false.

Example:

<!-- verify -->
```cangjie
main() {
    // Create identical Some values
    var someValue1: Option<Int64> = Some(42)
    var someValue2: Option<Int64> = Some(42)
    
    // Compare two identical Some values
    println("Some(42) == Some(42): ${someValue1 == someValue2}")
    
    // Create different Some values
    var someValue3: Option<Int64> = Some(42)
    var someValue4: Option<Int64> = Some(24)
    
    // Compare two different Some values
    println("Some(42) == Some(24): ${someValue3 == someValue4}")
    
    // Create a Some value and a None value
    var someValue5: Option<Int64> = Some(42)
    var noneValue1: Option<Int64> = None
    
    // Compare Some value with None value
    println("Some(42) == None: ${someValue5 == noneValue1}")
    
    // Create two None values
    var noneValue2: Option<Int64> = None
    var noneValue3: Option<Int64> = None
    
    // Compare two None values
    println("None == None: ${noneValue2 == noneValue3}")
}
```

Execution Result:

```text
Some(42) == Some(42): true
Some(42) == Some(24): false
Some(42) == None: false
None == None: true
```

## extend\<T> Option\<T> <: Hashable where T <: Hashable

```cangjie
extend<T> Option<T> <: Hashable where T <: Hashable
```

Function: Extends the [Hashable](core_package_interfaces.md#interface-hashable) interface for the [Option](core_package_enums.md#enum-optiont) type.

Extended type:

- [Option\<T>](core_package_enums.md#enum-optiont)

Extended interfaces:

- Hashable

### func hashCode()

```cangjie
public func hashCode(): Int64
```

Function: Retrieves the hash value.

Returns:

- [Int64](core_package_intrinsics.md#int64) - The hash value.

Example:

<!-- verify -->
```cangjie
main() {
    // Create a Some value
    var someValue: Option<Int64> = Some(42)
    
    // Get the hash code of Some value
    var someHashCode = someValue.hashCode()
    println("Hash code of Some(42): ${someHashCode}")
    
    // Create a None value
    var noneValue: Option<Int64> = None
    
    // Get the hash code of None value
    var noneHashCode = noneValue.hashCode()
    println("Hash code of None: ${noneHashCode}")
    
    // Compare hash codes of two identical Some values
    var someValue1: Option<Int64> = Some(42)
    var someValue2: Option<Int64> = Some(42)
    
    println("Hash codes of Some(42) and Some(42) are equal: ${someValue1.hashCode() == someValue2.hashCode()}")
    
    // Compare hash codes of two different Some values
    var someValue3: Option<Int64> = Some(42)
    var someValue4: Option<Int64> = Some(24)
    
    println("Hash codes of Some(42) and Some(24) are equal: ${someValue3.hashCode() == someValue4.hashCode()}")
}
```

Execution Result:

```text
Hash code of Some(42): 42
Hash code of None: 0
Hash codes of Some(42) and Some(42) are equal: true
Hash codes of Some(42) and Some(24) are equal: false
```

## extend\<T> Option\<T> <: ToString where T <: ToString

```cangjie
extend<T> Option<T> <: ToString where T <: ToString
```

Function: Implements the [ToString](core_package_interfaces.md#interface-tostring) interface for the [Option](core_package_enums.md#enum-optiont)\<T> enum to support string conversion operations.

Extended type:

- [Option\<T>](core_package_enums.md#enum-optiont)

Extended interfaces:

- ToString

### func toString()

```cangjie
public func toString(): String
```

Function: Converts [Option](core_package_enums.md#enum-optiont) to an output string, with content being "Some(${T.toString()})" or "None".

Returns:

- [String](core_package_structs.md#struct-string) - The converted string.

Example:

<!-- verify -->
```cangjie
main() {
    // Create a Some value
    var someValue: Option<Int64> = Some(42)
    
    // Convert Some value to string
    var someStr = someValue
    println("Some(42) converted to string: ${someStr}")
    
    // Create a None value
    var noneValue: Option<Int64> = None
    
    // Convert None value to string
    var noneStr = noneValue
    println("None converted to string: ${noneStr}")
    
    // Create a Some value of String type
    var someStringValue: Option<String> = Some("Hello")
    
    // Convert String-type Some value to string
    var someStringStr = someStringValue
    println("Some(\"Hello\") converted to string: ${someStringStr}")
}
```

Execution Result:

```text
Some(42) converted to string: Some(42)
None converted to string: None
Some("Hello") converted to string: Some(Hello)
```

## extend\<T> Range\<T> <: Equatable\<Range\<T>> where T <: Countable\<T> & Comparable\<T> & Equatable\<T>

```cangjie
extend<T> Range<T> <: Equatable<Range<T>> where T <: Countable<T> & Comparable<T> & Equatable<T>
```

Function: Extends the [Range](core_package_structs.md#struct-ranget-where-t--countablet--comparablet--equatablet)\<T> type with the [Equatable](core_package_interfaces.md#interface-equatablet)\<[Range](core_package_structs.md#struct-ranget-where-t--countablet--comparablet--equatablet)\<T>> interface.

Extended type:

- [Range\<T>](core_package_structs.md#struct-ranget-where-t--countablet--comparablet--equatablet)

Extended interfaces:

- Equatable\<Range\<T>>

### operator func ==(Range\<T>)

```cangjie
public operator func ==(that: Range<T>): Bool
```

Function: Determines whether two [Range](core_package_structs.md#struct-ranget-where-t--countablet--comparablet--equatablet) instances are equal.

Two [Range](core_package_structs.md#struct-ranget-where-t--countablet--comparablet--equatablet) instances are equal if they represent the same interval, i.e., their `start`, `end`, [step](core_package_structs.md#let-step), and `isClosed` values are equal.

Parameters:

- that: [Range](core_package_structs.md#struct-ranget-where-t--countablet--comparablet--equatablet)\<T> - The Range instance to compare.

Returns:

- [Bool](core_package_intrinsics.md#bool) - true if equal, false otherwise.

Example:

<!-- verify -->
```cangjie
main() {
    // Create two identical Range instances
    let range1 = Range<Int32>(1, 5, 1, true, false, false)
    let range2 = Range<Int32>(1, 5, 1, true, false, false)
    
    // Use the == operator to compare the two Range instances
    if (range1 == range2) {
        println("Range1 is equal to Range2")
    } else {
        println("Range1 is not equal to Range2")
    }
    
    // Create a different Range instance
    let range3 = Range<Int32>(1, 6, 1, true, false, false)
    
    // Use the == operator to compare two different Range instances
    if (range1 == range3) {
        println("Range1 is equal to Range3")
    } else {
        println("Range1 is not equal to Range3")
    }
}
```

Output:

```text
Range1 is equal to Range2
Range1 is not equal to Range3
```

## extend\<T> Range\<T> <: Hashable where T <: Hashable & Countable\<T> & Comparable\<T> & Equatable\<T>

```cangjie
extend<T> Range<T> <: Hashable where T <: Hashable & Countable<T> & Comparable<T> & Equatable<T>
```

Function: Extends the [Range](core_package_structs.md#struct-ranget-where-t--countablet--comparablet--equatablet) type with the [Hashable](core_package_interfaces.md#interface-hashable) interface, supporting hash value computation.

Extended type:

- [Range\<T>](core_package_structs.md#struct-ranget-where-t--countablet--comparablet--equatablet)

Extended interfaces:

- Hashable

### func hashCode()

```cangjie
public func hashCode(): Int64
```

Function: Gets the hash value, which is a combined hash computation of `start`, `end`, [step](core_package_structs.md#let-step), and `isClosed`.

Returns:

- [Int64](core_package_intrinsics.md#int64) - The hash value.

Example:

<!-- verify -->
```cangjie
main() {
    // Create a Range instance
    let range = Range<Int32>(1, 5, 1, true, false, false)
    
    // Get the hash value
    let hash = range.hashCode()
    println("Range hash code: ${hash}")
    
    // Create another identical Range instance
    let sameRange = Range<Int32>(1, 5, 1, true, false, false)
    let sameHash = sameRange.hashCode()
    println("Same range hash code: ${sameHash}")
    
    // Verify that identical Range instances have the same hash code
    if (hash == sameHash) {
        println("Same ranges have the same hash code")
    }
}
```

Output:

```text
Range hash code: -198835925178762
```
