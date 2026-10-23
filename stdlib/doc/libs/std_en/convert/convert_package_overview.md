# std.convert  

## Function Overview  

The `convert` package provides a series of `Convert` functions for converting strings to specific types.  

For example, converting the string `"true"` to the boolean value `true`, or the string `"123"` to the integer `123`.  

The `convert` package also offers formatting capabilities, primarily for converting Cangjie type instances into formatted strings.  

It defines the `Formattable` interface to standardize formatting methods and implements this interface for Cangjie types such as `Int8`, `Int16`, etc. Users can also implement this interface for custom types to gain formatting functionality.  

When converting Cangjie types to strings, the string format can be specified using formatting parameters such as width, alignment, etc. (In the methods defined by the `Formattable` interface, formatting parameters are passed as function arguments.)  

The detailed syntax for formatting parameters is as follows:  

```text  
format_spec := [flags][width][.precision][specifier]  
flags := '-' | '+' | '#' | '0'  
width := integer  
precision := integer  
specifier := 'b' | 'B' | 'o' | 'O' | 'x' | 'X' | 'e' | 'E' | 'g' | 'G'  
```  

### Flags:  

- `'-'`: Applies to `Int`, `UInt`, `Rune`, and `Float`. Indicates left alignment.  

    Example code:  

    ```cangjie  
    import std.convert.*  

    main() {  
        var c : Int32 = -20  
        print("\"${c.format("-10")}\"")  
    }  
    ```  

    Output:  

    ```text  
    "-20       "  
    ```  

- `'+'`: Applies to `Int`, `UInt`, and `Float`. Displays a `'+'` sign for positive numbers (negative numbers are unaffected).  

    Example code:  

    ```cangjie  
    import std.convert.*  

    main() {  
        var c: Int32 = 20  
        print("\"${c.format("+10")}\"")  
    }  
    ```  

    Output:  

    ```text  
    "       +20"  
    ```  

- `'#'`: Used for base-specific formatting. Prepends `'0b'` or `'0B'` for binary, `'0o'` or `'0O'` for octal, and `'0x'` or `'0X'` for hexadecimal.  

    Example code:  

    ```cangjie  
    import std.convert.*  

    main() {  
        var c: Int32 = 1  
        print("\"${c.format("#10x")}\"")  
    }  
    ```  

    Output:  

    ```text  
    "       0x1"  
    ```  

- `'0'`: Applies to `Int`, `UInt`, and `Float`. Pads empty spaces with zeros.  

    Example code:  

    ```cangjie  
    import std.convert.*  

    main() {  
        var c: Int32 = -20  
        print("\"${c.format("010")}\"")  
    }  
    ```  

    Output:  

    ```text  
    "-000000020"  
    ```  

### Width:  

- A positive integer applicable to `Int`, `UInt`, `Rune`, and `Float`. A negative sign before the width indicates left alignment; otherwise, right alignment is used. If the width is smaller than the actual length of the value, no truncation occurs.  
  Prefixes like `+`, `-`, `0x`, or `0o` occupy additional character positions.  

    Example code:  

    ```cangjie  
    import std.convert.*  

    main() {  
        var c: Int32 = 20  
        println("\"${c.format("1")}\"")  
        println("\"${c.format("+4")}\"")  
    }  
    ```  

    Output:  

    ```text  
    "20"  
    " +20"  
    ```  

### Precision:  

- A positive integer applicable to `Int`, `UInt`, and `Float`.  
  - For floating-point numbers, it specifies the number of decimal places. If unspecified, six decimal places are printed. If the precision is less than the actual length, rounding occurs; if greater, padding is applied (not necessarily zeros).  
  - For integer types, if unspecified or smaller than the actual length, no effect occurs. If greater, leading zeros are added.  

    Example code:  

    ```cangjie  
    import std.convert.*  

    main() {  
        var e: Float32 = 1234.1  
        println("\"${e.format("20.20")}\"")  
        var c: Int32 = -20  
        println("\"${c.format("10.8")}\"")  
    }  
    ```  

    Output:  

    ```text  
    "1234.09997558593750000000"  
    " -00000020"  
    ```  

### Specifiers:  

- `'b' | 'B' | 'o' | 'O' | 'x' | 'X'`: Applicable to `Int` and `UInt`.  
  - `'b' | 'B'`: Binary format.  
  - `'o' | 'O'`: Octal format.  
  - `'x' | 'X'`: Hexadecimal format.  

    Example code:  

    ```cangjie  
    import std.convert.*  

    main() {  
        var a = 20  
        println("\"${a.format("b")}\"")  
        println("\"${a.format("o")}\"")  
        println("\"${a.format("x")}\"")  
        println("\"${a.format("X")}\"")  
        println("\"${a.format("#X")}\"")  
    }  
    ```  

    Output:  

    ```text  
    "10100"  
    "24"  
    "14"  
    "14"  
    "0X14"  
    ```  

- `'e' | 'E' | 'g' | 'G'`: Applicable to `Float`.  
  - `'e' | 'E'`: Scientific notation (lowercase/uppercase).  
  - `'g' | 'G'`: General format. Chooses between decimal or scientific notation for concise representation.  

    Example code:  

    ```cangjie  
    import std.convert.*  

    main() {  
        var f: Float32 = 1234.1  
        var c: Float32 = 123412341234.1  
        println("\"${f.format("20.2e")}\"")  
        println("\"${f.format("20G")}\"")  
        println("\"${c.format("20G")}\"")  
        println("\"${f.format("20")}\"")  
        println("\"${c.format("20")}\"")  
    }  
    ```  

    Output:  

    ```text  
    "            1.23e+03"  
    "              1234.1"  
    "         1.23412E+11"  
    "         1234.099976"  
    " 123412340736.000000"  
    ```  

## API List  

### Interfaces  

| Interface Name | Description |  
| --------------------------- | ------------------------ |  
| [Formattable](./convert_package_api/convert_package_interfaces.md#interface-formattable) | Defines formatting functions to convert type instances into formatted strings based on parameters. |  
| [Parsable\<T>](./convert_package_api/convert_package_interfaces.md#interface-parsablet) | Interface for parsing strings into specific types. |  
| [RadixConvertible\<T>](./convert_package_api/convert_package_interfaces.md#interface-radixconvertiblet) | Interface for parsing strings into specific types with radix support. |

### Extensions

| Extension Name | Functionality |
| --- | --- |
| [extend Bool <: Parsable\<Bool>](convert_package_api/convert_package_extends.md#extend-bool--parsablebool) | This extension primarily implements functions for converting [Bool](../core/core_package_api/core_package_intrinsics.md#bool) type literal strings into [Bool](../core/core_package_api/core_package_intrinsics.md#bool) values. |
| [extend Float16 <: Formattable](convert_package_api/convert_package_extends.md#extend-float16--formattable) | Extends the [Formattable](convert_package_api/convert_package_interfaces.md#interface-formattable) interface for [Float16](../core/core_package_api/core_package_intrinsics.md#float16) to enable formatting [Float16](../core/core_package_api/core_package_intrinsics.md#float16) instances into strings. |
| [extend Float16 <: Parsable\<Float16>](convert_package_api/convert_package_extends.md#extend-float16--parsablefloat16) | This extension primarily implements operations for converting string literals of type [Float16](../core/core_package_api/core_package_intrinsics.md#float16) to [Float16](../core/core_package_api/core_package_intrinsics.md#float16) values. |
| [extend Float32 <: Formattable](convert_package_api/convert_package_extends.md#extend-float32--formattable) | Extends the [Formattable](convert_package_api/convert_package_interfaces.md#interface-formattable) interface for [Float32](../core/core_package_api/core_package_intrinsics.md#float32) to enable formatting [Float32](../core/core_package_api/core_package_intrinsics.md#float32) instances into strings. |
| [extend Float32 <: Parsable\<Float32>](convert_package_api/convert_package_extends.md#extend-float32--parsablefloat32) | This extension primarily implements operations for converting string literals of type [Float32](../core/core_package_api/core_package_intrinsics.md#float32) to [Float32](../core/core_package_api/core_package_intrinsics.md#float32) values. |
| [extend Float64 <: Formattable](convert_package_api/convert_package_extends.md#extend-float64--formattable) | Extends the [Formattable](convert_package_api/convert_package_interfaces.md#interface-formattable) interface for [Float64](../core/core_package_api/core_package_intrinsics.md#float64) to enable formatting [Float64](../core/core_package_api/core_package_intrinsics.md#float64) instances into strings. |
| [extend Float64 <: Parsable\<Float64>](convert_package_api/convert_package_extends.md#extend-float64--parsablefloat64) | This extension primarily implements operations for converting string literals of type [Float64](../core/core_package_api/core_package_intrinsics.md#float64) to [Float64](../core/core_package_api/core_package_intrinsics.md#float64) values. |
| [extend Int16 <: Formattable](convert_package_api/convert_package_extends.md#extend-int16--formattable) | Extends the [Formattable](convert_package_api/convert_package_interfaces.md#interface-formattable) interface for [Int16](../core/core_package_api/core_package_intrinsics.md#int16) to enable formatting [Int16](../core/core_package_api/core_package_intrinsics.md#int16) instances into strings. |
| [extend Int16 <: Parsable\<Int16>](convert_package_api/convert_package_extends.md#extend-int16--parsableint16) | This extension primarily implements operations for converting string literals of type [Int16](../core/core_package_api/core_package_intrinsics.md#int16) to [Int16](../core/core_package_api/core_package_intrinsics.md#int16) values. |
| [extend Int16 <: RadixConvertible\<Int16>](convert_package_api/convert_package_extends.md#extend-int16--radixconvertibleint16) | This extension primarily implements operations for converting [Int16](../core/core_package_api/core_package_intrinsics.md#int16) literal strings to [Int16](../core/core_package_api/core_package_intrinsics.md#int16) values. |
| [extend Int32 <: Formattable](convert_package_api/convert_package_extends.md#extend-int32--formattable) | Extends the [Formattable](convert_package_api/convert_package_interfaces.md#interface-formattable) interface for [Int32](../core/core_package_api/core_package_intrinsics.md#int32) to enable formatting [Int32](../core/core_package_api/core_package_intrinsics.md#int32) instances into strings. |
| [extend Int32 <: Parsable\<Int32>](convert_package_api/convert_package_extends.md#extend-int32--parsableint32) | This extension primarily implements operations for converting string literals of type [Int32](../core/core_package_api/core_package_intrinsics.md#int32) to [Int32](../core/core_package_api/core_package_intrinsics.md#int32) values. |
| [extend Int32 <: RadixConvertible\<Int32>](convert_package_api/convert_package_extends.md#extend-int32--radixconvertibleint32) | This extension primarily implements operations for converting [Int32](../core/core_package_api/core_package_intrinsics.md#int32) literal strings to [Int32](../core/core_package_api/core_package_intrinsics.md#int32) values. |
| [extend Int64 <: Formattable](convert_package_api/convert_package_extends.md#extend-int64--formattable) | Extends the [Formattable](convert_package_api/convert_package_interfaces.md#interface-formattable) interface for [Int64](../core/core_package_api/core_package_intrinsics.md#int64) to enable formatting [Int64](../core/core_package_api/core_package_intrinsics.md#int64) instances into strings. |
| [extend Int64 <: Parsable\<Int64>](convert_package_api/convert_package_extends.md#extend-int64--parsableint64) | This extension primarily implements operations for converting string literals of type [Int64](../core/core_package_api/core_package_intrinsics.md#int64) to [Int64](../core/core_package_api/core_package_intrinsics.md#int64) values. |
| [extend Int64 <: RadixConvertible\<Int64>](convert_package_api/convert_package_extends.md#extend-int64--radixconvertibleint64) |  |
| [extend Int8 <: Formattable](convert_package_api/convert_package_extends.md#extend-int8--formattable) | Extends the [Formattable](convert_package_api/convert_package_interfaces.md#interface-formattable) interface for [Int8](../core/core_package_api/core_package_intrinsics.md#int8) to enable formatting [Int8](../core/core_package_api/core_package_intrinsics.md#int8) instances into strings. |
| [extend Int8 <: Parsable\<Int8>](convert_package_api/convert_package_extends.md#extend-int8--parsableint8) | This extension primarily implements operations for converting string literals of type [Int8](../core/core_package_api/core_package_intrinsics.md#int8) to [Int8](../core/core_package_api/core_package_intrinsics.md#int8) values. |
| [extend Int8 <: RadixConvertible\<Int8>](convert_package_api/convert_package_extends.md#extend-int8--radixconvertibleint8) | This extension primarily implements operations for converting [Int8](../core/core_package_api/core_package_intrinsics.md#int8) literal strings to [Int8](../core/core_package_api/core_package_intrinsics.md#int8) values. |
| [extend Rune <: Formattable](convert_package_api/convert_package_extends.md#extend-rune--formattable) | Extends the [Rune](../core/core_package_api/core_package_intrinsics.md#rune) type with the [Formattable](convert_package_api/convert_package_interfaces.md#interface-formattable) interface to enable formatting Rune instances into strings. |
| [extend Rune <: Parsable\<Rune>](convert_package_api/convert_package_extends.md#extend-rune--parsablerune) | This extension primarily implements operations for converting string literals of type [Rune](../core/core_package_api/core_package_intrinsics.md#rune) to [Rune](../core/core_package_api/core_package_intrinsics.md#rune) values. |
| [extend UInt16 <: Formattable](convert_package_api/convert_package_extends.md#extend-uint16--formattable) | Extends the [UInt16](../core/core_package_api/core_package_intrinsics.md#uint16) type with the [Formattable](convert_package_api/convert_package_interfaces.md#interface-formattable) interface to enable formatting UInt16 instances into strings. |
| [extend UInt16 <: Parsable\<UInt16>](convert_package_api/convert_package_extends.md#extend-uint16--parsableuint16) | This extension primarily implements operations for converting string literals of type [UInt16](../core/core_package_api/core_package_intrinsics.md#uint16) to [UInt16](../core/core_package_api/core_package_intrinsics.md#uint16) values. |
| [extend UInt16 <: RadixConvertible\<UInt16>](convert_package_api/convert_package_extends.md#extend-uint16--radixconvertibleuint16) | This extension primarily implements operations for converting string literals of type [UInt16](../core/core_package_api/core_package_intrinsics.md#uint16) to [UInt16](../core/core_package_api/core_package_intrinsics.md#uint16) values. |
| [extend UInt32 <: Formattable](convert_package_api/convert_package_extends.md#extend-uint32--formattable) | Extends the [UInt32](../core/core_package_api/core_package_intrinsics.md#uint32) type with the [Formattable](convert_package_api/convert_package_interfaces.md#interface-formattable) interface to enable formatting UInt32 instances into strings. |
| [extend UInt32 <: Parsable\<UInt32>](convert_package_api/convert_package_extends.md#extend-uint32--parsableuint32) | This extension primarily implements operations for converting string literals of type [UInt32](../core/core_package_api/core_package_intrinsics.md#uint32) to [UInt32](../core/core_package_api/core_package_intrinsics.md#uint32) values. |
| [extend UInt32 <: RadixConvertible\<UInt32>](convert_package_api/convert_package_extends.md#extend-uint32--radixconvertibleuint32) | This extension primarily implements operations for converting string literals of type [UInt32](../core/core_package_api/core_package_intrinsics.md#uint32) to [UInt32](../core/core_package_api/core_package_intrinsics.md#uint32) values. |
| [extend UInt64 <: Formattable](convert_package_api/convert_package_extends.md#extend-uint64--formattable) | Extends the [UInt64](../core/core_package_api/core_package_intrinsics.md#uint64) type with the [Formattable](convert_package_api/convert_package_interfaces.md#interface-formattable) interface to enable formatting UInt64 instances into strings. |
| [extend UInt64 <: Parsable\<UInt64>](convert_package_api/convert_package_extends.md#extend-uint64--parsableuint64) | This extension primarily implements operations for converting string literals of type [UInt64](../core/core_package_api/core_package_intrinsics.md#uint64) to [UInt64](../core/core_package_api/core_package_intrinsics.md#uint64) values. |
| [extend UInt64 <: RadixConvertible\<UInt64>](convert_package_api/convert_package_extends.md#extend-uint64--radixconvertibleuint64) | This extension primarily implements operations for converting string literals of [UInt64](../core/core_package_api/core_package_intrinsics.md#uint64) type to [UInt64](../core/core_package_api/core_package_intrinsics.md#uint64) values. |
| [extend UInt8 <: Formattable](convert_package_api/convert_package_extends.md#extend-uint8--formattable) | Extends the [UInt8](../core/core_package_api/core_package_intrinsics.md#uint8) type with the [Formattable](convert_package_api/convert_package_interfaces.md#interface-formattable) interface to enable formatting UInt8 instances into strings. |
| [extend UInt8 <: Parsable\<UInt8>](convert_package_api/convert_package_extends.md#extend-uint8--parsableuint8) | This extension primarily implements operations for converting string literals of type [UInt8](../core/core_package_api/core_package_intrinsics.md#uint8) to [UInt8](../core/core_package_api/core_package_intrinsics.md#uint8) values. |
| [extend UInt8 <: RadixConvertible\<UInt8>](convert_package_api/convert_package_extends.md#extend-uint8--radixconvertibleuint8) | Converts a string literal of type [UInt8](../core/core_package_api/core_package_intrinsics.md#uint8) to an [Option](../core/core_package_api/core_package_enums.md#enum-optiont)\<[UInt8](../core/core_package_api/core_package_intrinsics.md#uint8)> value. |
