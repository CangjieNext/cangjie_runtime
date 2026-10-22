# 扩展

## extend Bool <: Equatable\<Bool>

```cangjie
extend Bool <: Equatable<Bool>
```

功能：为 [Bool](core_package_intrinsics.md#bool) 类型扩展 [Equatable](core_package_interfaces.md#interface-equatablet)\<[Bool](core_package_intrinsics.md#bool)> 接口，支持判等操作。

被扩展类型：

- [Bool](core_package_intrinsics.md#bool)

扩展接口：

- [Equatable](core_package_interfaces.md#interface-equatablet)\<[Bool](core_package_intrinsics.md#bool)>

## extend Bool <: Hashable

```cangjie
extend Bool <: Hashable
```

功能：为 [Bool](core_package_intrinsics.md#bool) 类型扩展 [Hashable](core_package_interfaces.md#interface-hashable) 接口，支持计算哈希值。

被扩展类型：

- [Bool](core_package_intrinsics.md#bool)

扩展接口：

- [Hashable](core_package_interfaces.md#interface-hashable)

### func hashCode()

```cangjie
public func hashCode(): Int64
```

功能：获取哈希值。

返回值：

- [Int64](core_package_intrinsics.md#int64) - 哈希值。

示例：

<!-- verify -->
```cangjie
main() {
    var flag: Bool = false
    println(flag.hashCode())
    flag = true
    println(flag.hashCode())
}
```

运行结果：

```text
0
1
```

## extend Bool <: ToString

```cangjie
extend Bool <: ToString
```

功能：为 [Bool](core_package_intrinsics.md#bool) 类型扩展 [ToString](core_package_interfaces.md#interface-tostring) 接口，实现向 [String](core_package_structs.md#struct-string) 类型的转换。

被扩展类型：

- [Bool](core_package_intrinsics.md#bool)

扩展接口：

- [ToString](core_package_interfaces.md#interface-tostring)

### func toString()

```cangjie
public func toString(): String
```

功能：将 [Bool](core_package_intrinsics.md#bool) 值转换为字符串。

返回值：

- [String](core_package_structs.md#struct-string) - 转化后的字符串。

示例：

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

运行结果：

```text
false
true
```

## extend Byte

```cangjie
extend Byte
```

功能：为 [Byte](core_package_types.md#type-byte) 类型实现一系列扩展方法，主要为在 Ascii 字符集范围内的一些字符判断、转换等操作。

被扩展类型：

- [Byte](core_package_types.md#type-byte)

### func isAscii()

```cangjie
public func isAscii(): Bool
```

功能：判断 [Byte](core_package_types.md#type-byte) 是否是在 Ascii 范围内。

返回值：

- [Bool](core_package_intrinsics.md#bool) - 如果 [Byte](core_package_types.md#type-byte) 在 Ascii 范围内返回 true，否则返回 false。

示例：

<!-- verify -->
```cangjie
main() {
    // 测试 isAscii() 方法
    let byte1: Byte = 65 // 'A' 的 ASCII 值
    let byte2: Byte = 200 // 超出 ASCII 范围的值

    println("byte1 (${byte1}) 是否在 ASCII 范围内: ${byte1.isAscii()}")
    println("byte2 (${byte2}) 是否在 ASCII 范围内: ${byte2.isAscii()}")
}
```

运行结果：

```text
byte1 (65) 是否在 ASCII 范围内: true
byte2 (200) 是否在 ASCII 范围内: false
```

### func isAsciiControl()

```cangjie
public func isAsciiControl(): Bool
```

功能：判断 [Byte](core_package_types.md#type-byte) 是否是在 Ascii 控制字符范围内。其取值范围为 [00, 1F] 和 {7F} 的并集。

返回值：

- [Bool](core_package_intrinsics.md#bool) - 如果 [Byte](core_package_types.md#type-byte) 在 Ascii 控制字符范围内返回 true，否则返回 false。

示例：

<!-- verify -->
```cangjie
main() {
    // 测试 isAsciiControl() 方法
    let byte1: Byte = 0x09 // 制表符 '\t'，属于 ASCII 控制字符
    let byte2: Byte = 0x20 // 空格 ' '，不属于 ASCII 控制字符
    let byte3: Byte = 0x7F // DEL，属于 ASCII 控制字符

    println("byte1 (0x${byte1}) 是否是 ASCII 控制字符: ${byte1.isAsciiControl()}")
    println("byte2 (0x${byte2}) 是否是 ASCII 控制字符: ${byte2.isAsciiControl()}")
    println("byte3 (0x${byte3}) 是否是 ASCII 控制字符: ${byte3.isAsciiControl()}")
}
```

运行结果：

```text
byte1 (0x9) 是否是 ASCII 控制字符: true
byte2 (0x32) 是否是 ASCII 控制字符: false
byte3 (0x127) 是否是 ASCII 控制字符: true
```

### func isAsciiGraphic()

```cangjie
public func isAsciiGraphic(): Bool
```

功能：判断 [Byte](core_package_types.md#type-byte) 是否是在 Ascii 图形字符范围内。其取值范围为 [21, 7E]。

返回值：

- [Bool](core_package_intrinsics.md#bool) - 如果 [Byte](core_package_types.md#type-byte) 在 Ascii 图形字符范围内返回 true，否则返回 false。

示例：

<!-- verify -->
```cangjie
main() {
    // 测试 isAsciiGraphic() 方法
    let byte1: Byte = 0x21 // '!'，属于 ASCII 图形字符
    let byte2: Byte = 0x7E // '~'，属于 ASCII 图形字符
    let byte3: Byte = 0x20 // ' '，空格，不属于 ASCII 图形字符

    println("byte1 (0x${byte1}) 是否是 ASCII 图形字符: ${byte1.isAsciiGraphic()}")
    println("byte2 (0x${byte2}) 是否是 ASCII 图形字符: ${byte2.isAsciiGraphic()}")
    println("byte3 (0x${byte3}) 是否是 ASCII 图形字符: ${byte3.isAsciiGraphic()}")
}
```

运行结果：

```text
byte1 (0x33) 是否是 ASCII 图形字符: true
byte2 (0x126) 是否是 ASCII 图形字符: true
byte3 (0x32) 是否是 ASCII 图形字符: false
```

### func isAsciiHex()

```cangjie
public func isAsciiHex(): Bool
```

功能：判断 [Byte](core_package_types.md#type-byte) 是否是在 Ascii 十六进制数字范围内。

返回值：

- [Bool](core_package_intrinsics.md#bool) - 如果 [Byte](core_package_types.md#type-byte) 在 Ascii 十六进制数字范围内返回 true，否则返回 false。

示例：

<!-- verify -->
```cangjie
main() {
    // 测试 isAsciiHex() 方法
    let byte1: Byte = 0x30 // '0'，属于 ASCII 十六进制数字
    let byte2: Byte = 0x46 // 'F'，属于 ASCII 十六进制数字
    let byte3: Byte = 0x67 // 'g'，不属于 ASCII 十六进制数字

    println("byte1 (0x${byte1}) 是否是 ASCII 十六进制数字: ${byte1.isAsciiHex()}")
    println("byte2 (0x${byte2}) 是否是 ASCII 十六进制数字: ${byte2.isAsciiHex()}")
    println("byte3 (0x${byte3}) 是否是 ASCII 十六进制数字: ${byte3.isAsciiHex()}")
}
```

运行结果：

```text
byte1 (0x48) 是否是 ASCII 十六进制数字: true
byte2 (0x70) 是否是 ASCII 十六进制数字: true
byte3 (0x103) 是否是 ASCII 十六进制数字: false
```

### func isAsciiLetter()

```cangjie
public func isAsciiLetter(): Bool
```

功能：判断 [Byte](core_package_types.md#type-byte) 是否是在 Ascii 拉丁字母范围内。

返回值：

- [Bool](core_package_intrinsics.md#bool) - 如果 [Byte](core_package_types.md#type-byte) 在 Ascii 拉丁字母范围内返回 true，否则返回 false。

示例：

<!-- verify -->
```cangjie
main() {
    // 测试 isAsciiLetter() 方法
    let byte1: Byte = 0x41 // 'A'，属于 ASCII 拉丁字母
    let byte2: Byte = 0x7A // 'z'，属于 ASCII 拉丁字母
    let byte3: Byte = 0x30 // '0'，数字，不属于 ASCII 拉丁字母

    println("byte1 (0x${byte1}) 是否是 ASCII 拉丁字母: ${byte1.isAsciiLetter()}")
    println("byte2 (0x${byte2}) 是否是 ASCII 拉丁字母: ${byte2.isAsciiLetter()}")
    println("byte3 (0x${byte3}) 是否是 ASCII 拉丁字母: ${byte3.isAsciiLetter()}")
}
```

运行结果：

```text
byte1 (0x65) 是否是 ASCII 拉丁字母: true
byte2 (0x122) 是否是 ASCII 拉丁字母: true
byte3 (0x48) 是否是 ASCII 拉丁字母: false
```

### func isAsciiLowerCase()

```cangjie
public func isAsciiLowerCase(): Bool
```

功能：判断 [Byte](core_package_types.md#type-byte) 是否是在 Ascii 小写拉丁字母范围内。

返回值：

- [Bool](core_package_intrinsics.md#bool) - 如果 [Byte](core_package_types.md#type-byte) 在 Ascii 小写拉丁字母范围内返回 true，否则返回 false。

示例：

<!-- verify -->
```cangjie
main() {
    // 测试 isAsciiLowerCase() 方法
    let byte1: Byte = 0x61 // 'a'，属于 ASCII 小写拉丁字母
    let byte2: Byte = 0x7A // 'z'，属于 ASCII 小写拉丁字母
    let byte3: Byte = 0x41 // 'A'，大写字母，不属于 ASCII 小写拉丁字母

    println("byte1 (0x${byte1}) 是否是 ASCII 小写拉丁字母: ${byte1.isAsciiLowerCase()}")
    println("byte2 (0x${byte2}) 是否是 ASCII 小写拉丁字母: ${byte2.isAsciiLowerCase()}")
    println("byte3 (0x${byte3}) 是否是 ASCII 小写拉丁字母: ${byte3.isAsciiLowerCase()}")
}
```

运行结果：

```text
byte1 (0x97) 是否是 ASCII 小写拉丁字母: true
byte2 (0x122) 是否是 ASCII 小写拉丁字母: true
byte3 (0x65) 是否是 ASCII 小写拉丁字母: false
```

### func isAsciiNumber()

```cangjie
public func isAsciiNumber(): Bool
```

功能：判断 [Byte](core_package_types.md#type-byte) 是否是在 Ascii 十进制数字范围内。

返回值：

- [Bool](core_package_intrinsics.md#bool) - 如果 [Byte](core_package_types.md#type-byte) 在 Ascii 十进制数字范围内返回 true，否则返回 false。

示例：

<!-- verify -->
```cangjie
main() {
    // 测试 isAsciiNumber() 方法
    let byte1: Byte = 0x30 // '0'，属于 ASCII 十进制数字
    let byte2: Byte = 0x39 // '9'，属于 ASCII 十进制数字
    let byte3: Byte = 0x41 // 'A'，字母，不属于 ASCII 十进制数字

    println("byte1 (0x${byte1}) 是否是 ASCII 十进制数字: ${byte1.isAsciiNumber()}")
    println("byte2 (0x${byte2}) 是否是 ASCII 十进制数字: ${byte2.isAsciiNumber()}")
    println("byte3 (0x${byte3}) 是否是 ASCII 十进制数字: ${byte3.isAsciiNumber()}")
}
```

运行结果：

```text
byte1 (0x48) 是否是 ASCII 十进制数字: true
byte2 (0x57) 是否是 ASCII 十进制数字: true
byte3 (0x65) 是否是 ASCII 十进制数字: false
```

### func isAsciiNumberOrLetter()

```cangjie
public func isAsciiNumberOrLetter(): Bool
```

功能：判断 [Byte](core_package_types.md#type-byte) 是否是在 Ascii 十进制数字或拉丁字母范围内。

返回值：

- [Bool](core_package_intrinsics.md#bool) - 如果 [Byte](core_package_types.md#type-byte) 在 Ascii 十进制数字或拉丁字母范围内返回 true，否则返回 false。

示例：

<!-- verify -->
```cangjie
main() {
    // 测试 isAsciiNumberOrLetter() 方法
    let byte1: Byte = 0x30 // '0'，数字，属于 ASCII 十进制数字和拉丁字母范围
    let byte2: Byte = 0x41 // 'A'，字母，属于 ASCII 十进制数字和拉丁字母范围
    let byte3: Byte = 0x20 // ' '，空格，不属于 ASCII 十进制数字和拉丁字母范围

    println("byte1 (0x${byte1}) 是否是 ASCII 十进制数字和拉丁字母: ${byte1.isAsciiNumberOrLetter()}")
    println("byte2 (0x${byte2}) 是否是 ASCII 十进制数字和拉丁字母: ${byte2.isAsciiNumberOrLetter()}")
    println("byte3 (0x${byte3}) 是否是 ASCII 十进制数字和拉丁字母: ${byte3.isAsciiNumberOrLetter()}")
}
```

运行结果：

```text
byte1 (0x48) 是否是 ASCII 十进制数字和拉丁字母: true
byte2 (0x65) 是否是 ASCII 十进制数字和拉丁字母: true
byte3 (0x32) 是否是 ASCII 十进制数字和拉丁字母: false
```

### func isAsciiOct()

```cangjie
public func isAsciiOct(): Bool
```

功能：判断 [Byte](core_package_types.md#type-byte) 是否是在 Ascii 八进制数字范围内。

返回值：

- [Bool](core_package_intrinsics.md#bool) - 如果 [Byte](core_package_types.md#type-byte) 在 Ascii 八进制数字范围内返回 true，否则返回 false。

示例：

<!-- verify -->
```cangjie
main() {
    // 测试 isAsciiOct() 方法
    let byte1: Byte = 0x30 // '0'，属于 ASCII 八进制数字
    let byte2: Byte = 0x37 // '7'，属于 ASCII 八进制数字
    let byte3: Byte = 0x38 // '8'，不属于 ASCII 八进制数字

    println("byte1 (0x${byte1}) 是否是 ASCII 八进制数字: ${byte1.isAsciiOct()}")
    println("byte2 (0x${byte2}) 是否是 ASCII 八进制数字: ${byte2.isAsciiOct()}")
    println("byte3 (0x${byte3}) 是否是 ASCII 八进制数字: ${byte3.isAsciiOct()}")
}
```

运行结果：

```text
byte1 (0x48) 是否是 ASCII 八进制数字: true
byte2 (0x55) 是否是 ASCII 八进制数字: true
byte3 (0x56) 是否是 ASCII 八进制数字: false
```

### func isAsciiPunctuation()

```cangjie
public func isAsciiPunctuation(): Bool
```

功能：判断 [Byte](core_package_types.md#type-byte) 是否是在 Ascii 标点符号范围内。其取值范围为 [21, 2F]、[3A, 40]、[5B, 60] 和 [7B, 7E] 的并集。

返回值：

- [Bool](core_package_intrinsics.md#bool) - 如果 [Byte](core_package_types.md#type-byte) 在 Ascii 标点符号范围内返回 true，否则返回 false。

示例：

<!-- verify -->
```cangjie
main() {
    // 测试 isAsciiPunctuation() 方法
    let byte1: Byte = 0x21 // '!'，属于 ASCII 标点符号
    let byte2: Byte = 0x2C // ','，属于 ASCII 标点符号
    let byte3: Byte = 0x30 // '0'，数字，不属于 ASCII 标点符号

    println("byte1 (0x${byte1}) 是否是 ASCII 标点符号: ${byte1.isAsciiPunctuation()}")
    println("byte2 (0x${byte2}) 是否是 ASCII 标点符号: ${byte2.isAsciiPunctuation()}")
    println("byte3 (0x${byte3}) 是否是 ASCII 标点符号: ${byte3.isAsciiPunctuation()}")
}
```

运行结果：

```text
byte1 (0x33) 是否是 ASCII 标点符号: true
byte2 (0x44) 是否是 ASCII 标点符号: true
byte3 (0x48) 是否是 ASCII 标点符号: false
```

### func isAsciiUpperCase()

```cangjie
public func isAsciiUpperCase(): Bool
```

功能：判断 [Byte](core_package_types.md#type-byte) 是否是在 Ascii 大写拉丁字母范围内。

返回值：

- [Bool](core_package_intrinsics.md#bool) - 如果 [Byte](core_package_types.md#type-byte) 在 Ascii 大写拉丁字母范围内返回 true，否则返回 false。

示例：

<!-- verify -->
```cangjie
main() {
    // 测试 isAsciiUpperCase() 方法
    let byte1: Byte = 0x41 // 'A'，属于 ASCII 大写拉丁字母
    let byte2: Byte = 0x5A // 'Z'，属于 ASCII 大写拉丁字母
    let byte3: Byte = 0x61 // 'a'，小写字母，不属于 ASCII 大写拉丁字母

    println("byte1 (0x${byte1}) 是否是 ASCII 大写拉丁字母: ${byte1.isAsciiUpperCase()}")
    println("byte2 (0x${byte2}) 是否是 ASCII 大写拉丁字母: ${byte2.isAsciiUpperCase()}")
    println("byte3 (0x${byte3}) 是否是 ASCII 大写拉丁字母: ${byte3.isAsciiUpperCase()}")
}
```

运行结果：

```text
byte1 (0x65) 是否是 ASCII 大写拉丁字母: true
byte2 (0x90) 是否是 ASCII 大写拉丁字母: true
byte3 (0x97) 是否是 ASCII 大写拉丁字母: false
```

### func isAsciiWhiteSpace()

```cangjie
public func isAsciiWhiteSpace(): Bool
```

功能：判断 [Byte](core_package_types.md#type-byte) 是否是在 Ascii 空白字符范围内。其取值范围为 [09, 0D] 和 {20} 的并集。

返回值：

- [Bool](core_package_intrinsics.md#bool) - 如果 [Byte](core_package_types.md#type-byte) 在 Ascii 空白字符范围内返回 true，否则返回 false。

示例：

<!-- verify -->
```cangjie
main() {
    // 测试 isAsciiWhiteSpace() 方法
    let byte1: Byte = 0x09 // '\t'，制表符，属于 ASCII 空白字符
    let byte2: Byte = 0x20 // ' '，空格，属于 ASCII 空白字符
    let byte3: Byte = 0x41 // 'A'，字母，不属于 ASCII 空白字符

    println("byte1 (0x${byte1}) 是否是 ASCII 空白字符: ${byte1.isAsciiWhiteSpace()}")
    println("byte2 (0x${byte2}) 是否是 ASCII 空白字符: ${byte2.isAsciiWhiteSpace()}")
    println("byte3 (0x${byte3}) 是否是 ASCII 空白字符: ${byte3.isAsciiWhiteSpace()}")
}
```

运行结果：

```text
byte1 (0x9) 是否是 ASCII 空白字符: true
byte2 (0x32) 是否是 ASCII 空白字符: true
byte3 (0x65) 是否是 ASCII 空白字符: false
```

### func toAsciiLowerCase()

```cangjie
public func toAsciiLowerCase(): Byte
```

功能：将 [Byte](core_package_types.md#type-byte) 转换为对应的 Ascii 小写字符 [Byte](core_package_types.md#type-byte)，如果无法转换则保持现状。

返回值：

- [Byte](core_package_types.md#type-byte) - 转换后的 [Byte](core_package_types.md#type-byte)，如果无法转换则返回原来的 [Byte](core_package_types.md#type-byte)。

示例：

<!-- verify -->
```cangjie
main() {
    // 测试 toAsciiLowerCase() 方法
    let byte1: Byte = 0x41 // 'A'，大写字母
    let byte2: Byte = 0x5A // 'Z'，大写字母
    let byte3: Byte = 0x30 // '0'，数字，无法转换

    let lower1 = byte1.toAsciiLowerCase()
    let lower2 = byte2.toAsciiLowerCase()
    let lower3 = byte3.toAsciiLowerCase()

    println("byte1 (0x${byte1}) 转换为小写: 0x${lower1}")
    println("byte2 (0x${byte2}) 转换为小写: 0x${lower2}")
    println("byte3 (0x${byte3}) 转换为小写: 0x${lower3} (保持不变)")
}
```

运行结果：

```text
byte1 (0x65) 转换为小写: 0x97
byte2 (0x90) 转换为小写: 0x122
byte3 (0x48) 转换为小写: 0x48 (保持不变)
```

### func toAsciiUpperCase()

```cangjie
public func toAsciiUpperCase(): Byte
```

功能：将 [Byte](core_package_types.md#type-byte) 转换为对应的 Ascii 大写字符 [Byte](core_package_types.md#type-byte)，如果无法转换则保持现状。

返回值：

- [Byte](core_package_types.md#type-byte) - 转换后的 [Byte](core_package_types.md#type-byte)，如果无法转换则返回原来的 [Byte](core_package_types.md#type-byte)。

示例：

<!-- verify -->
```cangjie
main() {
    // 测试 toAsciiUpperCase() 方法
    let byte1: Byte = 0x61 // 'a'，小写字母
    let byte2: Byte = 0x7A // 'z'，小写字母
    let byte3: Byte = 0x30 // '0'，数字，无法转换

    let upper1 = byte1.toAsciiUpperCase()
    let upper2 = byte2.toAsciiUpperCase()
    let upper3 = byte3.toAsciiUpperCase()

    println("byte1 (0x${byte1}) 转换为大写: 0x${upper1}")
    println("byte2 (0x${byte2}) 转换为大写: 0x${upper2}")
    println("byte3 (0x${byte3}) 转换为大写: 0x${upper3} (保持不变)")
}
```

运行结果：

```text
byte1 (0x97) 转换为大写: 0x65
byte2 (0x122) 转换为大写: 0x90
byte3 (0x48) 转换为大写: 0x48 (保持不变)
```

## extend CString <: ToString

```cangjie
extend CString <: ToString
```

功能：为 [CString](core_package_intrinsics.md#cstring) 类型扩展一些字符串指针常用方法，包括判空、获取长度、判等、获取子串等。

被扩展类型：

- [CString](core_package_intrinsics.md#cstring)

扩展接口：

- [ToString](core_package_interfaces.md#interface-tostring)

### func asResource()

```cangjie
public func asResource(): CStringResource
```

功能：获取当前 [CString](core_package_intrinsics.md#cstring) 实例对应的 [CStringResource](core_package_structs.md#struct-cstringresource) C 字符串资源类型实例。

[CStringResource](core_package_structs.md#struct-cstringresource) 实现了 [Resource](core_package_interfaces.md#interface-resource) 接口，可以在 `try-with-resource` 语法上下文中实现资源自动释放。

返回值：

- [CStringResource](core_package_structs.md#struct-cstringresource) - 对应的 [CStringResource](core_package_structs.md#struct-cstringresource) C 字符串资源类型实例。

示例：

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

运行结果：

```text
5
true
```

### func compare(CString)

```cangjie
public func compare(str: CString): Int32
```

功能：按字典序比较两个字符串，同 C 语言中的 `strcmp`。

参数：

- str: [CString](core_package_intrinsics.md#cstring) - 比较的目标字符串。

返回值：

- [Int32](core_package_intrinsics.md#int32) - 两者相等返回 0，如果当前字符串比参数 str 小，返回 -1，否则返回 1。

异常：

- [IllegalMemoryException](core_package_exceptions.md#class-illegalmemoryexception) - 如果被比较的两个 [CString](core_package_intrinsics.md#cstring) 中存在空指针，抛出异常。

示例：

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

运行结果：

```text
0
-1
1
```

### func endsWith(CString)

```cangjie
public func endsWith(suffix: CString): Bool
```

功能：判断字符串是否包含指定后缀。

参数：

- suffix: [CString](core_package_intrinsics.md#cstring) - 匹配的目标后缀字符串。

返回值：

- [Bool](core_package_intrinsics.md#bool) - 如果该字符串包含 suffix 后缀，返回 true，如果该字符串不包含 suffix 后缀，返回 false，特别地，如果原字符串或者 suffix 后缀字符串指针为空，均返回 false。

示例：

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

运行结果：

```text
true
false
```

### func equals(CString)

```cangjie
public func equals(other: CString): Bool
```

功能：判断两个字符串是否相等。

参数：

- other: [CString](core_package_intrinsics.md#cstring) - 比较的目标字符串。

返回值：

- [Bool](core_package_intrinsics.md#bool) - 如果两个字符串相等，返回 true，否则返回 false。

示例：

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

运行结果：

```text
true
false
```

### func equalsLower(CString)

```cangjie
public func equalsLower(other: CString): Bool
```

功能：判断两个字符串是否相等，忽略大小写。

参数：

- other: [CString](core_package_intrinsics.md#cstring) - 匹配的目标字符串。

返回值：

- [Bool](core_package_intrinsics.md#bool) - 如果两个字符串忽略大小写相等，返回 true，否则返回 false。

示例：

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

运行结果：

```text
true
true
```

### func getChars()

```cangjie
public func getChars(): CPointer<UInt8>
```

功能：获取该字符串的指针。

返回值：

- [CPointer](./core_package_intrinsics.md#cpointert)\<[UInt8](./core_package_intrinsics.md#uint8)> - 该字符串的指针。

示例：

<!-- verify -->
```cangjie
main() {
    var str1: CString = unsafe { LibC.mallocCString("hello") }
    var ptr: CPointer<UInt8> = unsafe { str1.getChars() }
    var c: UInt8 = unsafe { ptr.read() }
    println(c) // h的ascii码为104
    unsafe {
        LibC.free(str1)
    }
}
```

运行结果：

```text
104
```

### func isEmpty()

```cangjie
public func isEmpty(): Bool
```

功能：判断字符串是否为空字符串。

返回值：

- [Bool](core_package_intrinsics.md#bool) - 如果为空字符串或字符串指针为空，返回 true，否则返回 false。

示例：

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

运行结果：

```text
false
```

### func isNotEmpty()

```cangjie
public func isNotEmpty(): Bool
```

功能：判断字符串是否不为空字符串。

返回值：

- [Bool](core_package_intrinsics.md#bool) - 如果不为空字符串，返回 true，如果字符串指针为空，返回 false。

示例：

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

运行结果：

```text
true
```

### func isNull()

```cangjie
public func isNull(): Bool
```

功能：判断字符串指针是否为空。

返回值：

- [Bool](core_package_intrinsics.md#bool) - 如果字符串指针为空，返回 true，否则返回 false。

示例：

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

运行结果：

```text
false
```

### func size()

```cangjie
public func size(): Int64
```

功能：返回该字符串长度，同 C 语言中的 `strlen`。

返回值：

- [Int64](core_package_intrinsics.md#int64) - 字符串长度。

示例：

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

运行结果：

```text
5
```

### func startsWith(CString)

```cangjie
public func startsWith(prefix: CString): Bool
```

功能：判断字符串是否包含指定前缀。

参数：

- prefix: [CString](core_package_intrinsics.md#cstring) - 匹配的目标前缀字符串。

返回值：

- [Bool](core_package_intrinsics.md#bool) - 如果该字符串包含 prefix 前缀，返回 true，如果该字符串不包含 prefix 前缀，返回 false，特别地，如果原字符串或者 prefix 前缀字符串指针为空，均返回 false。

示例：

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

运行结果：

```text
true
```

### func subCString(UIntNative)

```cangjie
public func subCString(beginIndex: UIntNative): CString
```

功能：截取指定位置开始至字符串结束的子串。

> **注意：**
>
> 1. 该接口返回为字符串的副本，返回的子串使用完后需要手动 free。
> 2. 如果 beginIndex 与字符串长度相等，将返回空指针。

参数：

- beginIndex: [UIntNative](core_package_intrinsics.md#uintnative) - 截取的起始位置，取值范围为 [0, this.size()]。

返回值：

- [CString](core_package_intrinsics.md#cstring) - 截取的子串。

异常：

- [IndexOutOfBoundsException](core_package_exceptions.md#class-indexoutofboundsexception) - 如果 beginIndex 大于字符串长度，抛出异常。
- [IllegalMemoryException](core_package_exceptions.md#class-illegalmemoryexception) - 如果内存申请失败或内存拷贝失败时，抛出异常。

示例：

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

运行结果：

```text
llo
```

### func subCString(UIntNative, UIntNative)

```cangjie
public func subCString(beginIndex: UIntNative, subLen: UIntNative): CString
```

功能：截取字符串的子串，指定起始位置和截取长度。

如果截取的末尾位置超出字符串长度，截取至字符串末尾。

> **注意：**
>
> 1. 该接口返回为字符串的副本，返回的子串使用完后需要手动 free。
> 2. 如果 beginIndex 等于于字符串长度，或 subLen 等于 0，返回空指针。

参数：

- beginIndex: [UIntNative](core_package_intrinsics.md#uintnative) - 截取的起始位置，取值范围为 [0, this.size()]。
- subLen: [UIntNative](core_package_intrinsics.md#uintnative) - 截取长度，取值范围为 [0, [UIntNative](core_package_intrinsics.md#uintnative).Max]。

返回值：

- [CString](core_package_intrinsics.md#cstring) - 截取的子串。

异常：

- [IndexOutOfBoundsException](core_package_exceptions.md#class-indexoutofboundsexception) - 如果 beginIndex 大于字符串长度，抛出异常。
- [IllegalMemoryException](core_package_exceptions.md#class-illegalmemoryexception) - 如果内存申请失败或内存拷贝失败时，抛出异常。

示例：

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

运行结果：

```text
ll
```

### func toString()

```cangjie
public func toString(): String
```

功能：将 [CString](core_package_intrinsics.md#cstring) 类型转为仓颉的 [String](core_package_structs.md#struct-string) 类型。

返回值：

- [String](core_package_structs.md#struct-string) - 转换后的字符串。

异常：

- [IllegalArgumentException](core_package_exceptions.md#class-illegalargumentexception) - 不合法的 UTF-8 字节序列。

示例：

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

运行结果：

```text
hello
```

## extend Float16

```cangjie
extend Float16
```

功能：支持与 [UInt16](core_package_intrinsics.md#uint16) 互相转换。

被扩展类型：

- [Float16](core_package_intrinsics.md#float16)

### static func fromBits(UInt16)

```cangjie
public static func fromBits(bits: UInt16): Float16
```

功能：将指定的 [UInt16](core_package_intrinsics.md#uint16) 按照 `IEEE 754` 标准中 float16 的二进制位表示形式，直接转换为对应的 [Float16](core_package_intrinsics.md#float16) 浮点数。

参数：

- bits: [UInt16](core_package_intrinsics.md#uint16) - 要转换的数字。

返回值：

- [Float16](core_package_intrinsics.md#float16) - 转换结果，其位与参数 bits 值相同。

示例：

<!-- verify -->
```cangjie
main() {
    let v = Float16.fromBits(0x4A40u16)
    println(v)
}
```

运行结果：

```text
12.500000
```

### func toBits()

```cangjie
public func toBits(): UInt16
```

功能：将指定的 [Float16](core_package_intrinsics.md#float16) 数转换为以位表示的对应 [UInt16](core_package_intrinsics.md#uint16) 数。

返回值：

- [UInt16](core_package_intrinsics.md#uint16) - 转换结果，其值与 [Float16](core_package_intrinsics.md#float16) 的位表示相同。

示例：

<!-- verify -->
```cangjie
main() {
    println(12.5f16.toBits()) // 0x4A40 19008
}
```

运行结果：

```text
19008
```

## extend Float16 <: Comparable\<Float16>

```cangjie
extend Float16 <: Comparable<Float16>
```

功能：为 [Float16](core_package_intrinsics.md#float16) 类型扩展 [Comparable](core_package_interfaces.md#interface-comparablet)\<[Float16](core_package_intrinsics.md#float16)> 接口，支持比较操作。

被扩展类型：

- [Float16](core_package_intrinsics.md#float16)

扩展接口：

- [Comparable](core_package_interfaces.md#interface-comparablet)\<[Float16](core_package_intrinsics.md#float16)>

### func compare(Float16)

```cangjie
public func compare(other: Float16): Ordering
```

功能：判断当前 [Float16](core_package_intrinsics.md#float16) 值与指定 [Float16](core_package_intrinsics.md#float16) 值的大小关系。`NaN` 无法参与比较，返回值无意义。

参数：

- other: [Float16](core_package_intrinsics.md#float16) - 待比较的另一个 [Float16](core_package_intrinsics.md#float16) 值。

返回值：

- [Ordering](core_package_enums.md#enum-ordering) - 如果大于，返回 [Ordering](core_package_enums.md#enum-ordering).GT；如果等于，返回 [Ordering](core_package_enums.md#enum-ordering).EQ；如果小于，返回 [Ordering](core_package_enums.md#enum-ordering).LT。

示例：

<!-- verify -->
```cangjie
main() {
    var num1: Float16 = 0.12
    var num2: Float16 = 0.234
    println(num1.compare(num2))
}
```

运行结果：

```text
Ordering.LT
```

## extend Float16 <: Hashable

```cangjie
extend Float16 <: Hashable
```

功能：为 [Float16](core_package_intrinsics.md#float16) 类型扩展 [Hashable](core_package_interfaces.md#interface-hashable) 接口，支持计算哈希值，并拓展该类型以支持一些数学常数。

被扩展类型：

- [Float16](core_package_intrinsics.md#float16)

扩展接口：

- [Hashable](core_package_interfaces.md#interface-hashable)

### static prop Inf

```cangjie
public static prop Inf: Float16
```

功能：获取半精度浮点数的无穷数。

类型：[Float16](./core_package_intrinsics.md#float16)

示例：

<!-- verify -->
```cangjie
main() {
    // 测试 Float16.Inf 属性
    let infValue = Float16.Inf
    println("Float16.Inf 的值: ${infValue}")

    // 验证它确实是无穷大
    println("Float16.Inf 是否为无穷大: ${infValue.isInf()}")
}
```

运行结果：

```text
Float16.Inf 的值: inf
Float16.Inf 是否为无穷大: true
```

### static prop Max

```cangjie
public static prop Max: Float16
```

功能：获取半精度浮点数的最大值。

类型：[Float16](./core_package_intrinsics.md#float16)

示例：

<!-- verify -->
```cangjie
main() {
    // 测试 Float16.Max 属性
    let maxValue = Float16.Max
    println("Float16.Max 的值: ${maxValue}")

    // 验证它确实是最大值：等于 -Float16.Min
    println("Float16.Max 是否等于 -Float16.Min: ${maxValue == -Float16.Min}")
}
```

运行结果：

```text
Float16.Max 的值: 65504.000000
Float16.Max 是否等于 -Float16.Min: true
```

### static prop Min

```cangjie
public static prop Min: Float16
```

功能：获取半精度浮点数的最小值。

类型：[Float16](./core_package_intrinsics.md#float16)

示例：

<!-- verify -->
```cangjie
main() {
    // 测试 Float16.Min 属性
    let minValue = Float16.Min
    println("Float16.Min 的值: ${minValue}")

    // 验证它确实是最小值：等于 -Float16.Max
    println("Float16.Min 是否等于 -Float16.Max: ${minValue == -Float16.Max}")
}
```

运行结果：

```text
Float16.Min 的值: -65504.000000
Float16.Min 是否等于 -Float16.Max: true
```

### static prop MinDenormal

```cangjie
public static prop MinDenormal: Float16
```

功能：获取半精度浮点数的最小次正规数。

类型：[Float16](./core_package_intrinsics.md#float16)

示例：

<!-- verify -->
```cangjie
main() {
    // 测试 Float16.MinDenormal 属性
    let minDenormalValue = Float16.MinDenormal

    // 验证它确实是最小次正规数：大于 0 且小于最小正规数
    println("Float16.MinDenormal 是否大于 0: ${minDenormalValue > Float16(0.0)}")
    println("Float16.MinDenormal 是否小于最小正规数: ${minDenormalValue < Float16.MinNormal}")
}
```

运行结果：

```text
Float16.MinDenormal 是否大于 0: true
Float16.MinDenormal 是否小于最小正规数: true
```

### static prop MinNormal

```cangjie
public static prop MinNormal: Float16
```

功能：获取半精度浮点数的最小正规数。

类型：[Float16](./core_package_intrinsics.md#float16)

示例：

<!-- verify -->
```cangjie
main() {
    // 测试 Float16.MinNormal 属性
    let minNormalValue = Float16.MinNormal
    println("Float16.MinNormal 的值: ${minNormalValue}")

    // 验证它确实是最小正规数
    println("Float16.MinNormal 是否为常规数值: ${minNormalValue.isNormal()}")
}
```

运行结果：

```text
Float16.MinNormal 的值: 0.000061
Float16.MinNormal 是否为常规数值: true
```

### static prop NaN

```cangjie
public static prop NaN: Float16
```

功能：获取半精度浮点数的非数。

类型：[Float16](./core_package_intrinsics.md#float16)

示例：

<!-- verify -->
```cangjie
main() {
    // 测试 Float16.NaN 属性
    let nanValue = Float16.NaN
    println("Float16.NaN 的值: ${nanValue}")

    // 验证它确实是 NaN
    println("Float16.NaN 是否为非数值: ${nanValue.isNaN()}")
}
```

运行结果：

```text
Float16.NaN 的值: nan
Float16.NaN 是否为非数值: true
```

### static func max(Float16, Float16, Array\<Float16>)

```cangjie
public static func max(a: Float16, b: Float16, others: Array<Float16>): Float16
```

功能：返回一组[Float16](./core_package_intrinsics.md#float16)中的最大值，此函数的第三个参数是一个变长参数，可以获取二个以上的[Float16](./core_package_intrinsics.md#float16)最大值，如果参数中有 `NaN`，该函数会返回 `NaN`。

参数：

- a: [Float16](./core_package_intrinsics.md#float16) - 第一个待比较的数。
- b: [Float16](./core_package_intrinsics.md#float16) - 第二个待比较的数。
- others: [Array](core_package_structs.md#struct-arrayt)\<[Float16](./core_package_intrinsics.md#float16)> - 其他待比较的数。

返回值：

- [Float16](./core_package_intrinsics.md#float16) - 返回参数中的最大值。

示例：

<!-- verify -->
```cangjie
main() {
    // 测试 Float16.max 静态方法
    let a: Float16 = 1.5f16
    let b: Float16 = 2.7f16
    let c: Float16 = 0.8f16
    let d: Float16 = 3.2f16
    let e: Float16 = 3.4f16
    let f: Float16 = 3.8f16
    let g: Float16 = 3.9f16

    // 使用 max 方法找出最大值
    let maxValue = Float16.max(a, b, c, d, e, f, g)
    println("Float16.max(1.5, 2.7, 0.8, 3.2, 3.4, 3.8, 3.9) 的结果: ${maxValue}")

    // 验证结果
    println("最大值是否正确: ${maxValue == g}")
}
```

运行结果：

```text
Float16.max(1.5, 2.7, 0.8, 3.2, 3.4, 3.8, 3.9) 的结果: 3.900391
最大值是否正确: true
```

### static func min(Float16, Float16, Array\<Float16>)

```cangjie
public static func min(a: Float16, b: Float16, others: Array<Float16>): Float16
```

功能：返回一组[Float16](./core_package_intrinsics.md#float16)中的最小值，此函数的第三个参数是一个变长参数，可以获取二个以上的[Float16](./core_package_intrinsics.md#float16)最小值，如果参数中有 `NaN`，该函数会返回 `NaN`。

参数：

- a: [Float16](./core_package_intrinsics.md#float16) - 第一个待比较的数。
- b: [Float16](./core_package_intrinsics.md#float16) - 第二个待比较的数。
- others: [Array](core_package_structs.md#struct-arrayt)\<[Float16](./core_package_intrinsics.md#float16)> - 其他待比较的数。

返回值：

- [Float16](./core_package_intrinsics.md#float16) - 返回参数中的最小值。

示例：

<!-- verify -->
```cangjie
main() {
    // 测试 Float16.min 静态方法
    let a: Float16 = 1.5f16
    let b: Float16 = 2.7f16
    let c: Float16 = 0.8f16
    let d: Float16 = 3.2f16
    let e: Float16 = 3.4f16
    let f: Float16 = 3.8f16
    let g: Float16 = 3.9f16

    // 使用 min 方法找出最小值
    let minValue = Float16.min(a, b, c, d, e, f, g)
    println("Float16.min(1.5, 2.7, 0.8, 3.2, 3.4, 3.8, 3.9) 的结果: ${minValue}")

    // 验证结果
    println("最小值是否正确: ${minValue == c}")
}
```

运行结果：

```text
Float16.min(1.5, 2.7, 0.8, 3.2, 3.4, 3.8, 3.9) 的结果: 0.799805
最小值是否正确: true
```

### func hashCode()

```cangjie
public func hashCode(): Int64
```

功能：获取哈希值。

返回值：

- [Int64](core_package_intrinsics.md#int64) - 哈希值。

示例：

<!-- verify -->
```cangjie
main() {
    // 测试 Float16.hashCode 方法
    let value1: Float16 = 1.5f16
    let value2: Float16 = 1.5f16
    let value3: Float16 = 2.0f16

    // 计算哈希值
    let hash1 = value1.hashCode()
    let hash2 = value2.hashCode()
    let hash3 = value3.hashCode()

    println("1.5f16 的哈希值: ${hash1}")
    println("1.5f16 的哈希值: ${hash2}")
    println("2.0f16 的哈希值: ${hash3}")

    // 验证相等的值具有相同的哈希值
    println("相等值的哈希值是否相同: ${hash1 == hash2}")
}
```

运行结果：

```text
1.5f16 的哈希值: 7729828949621302189
1.5f16 的哈希值: 7729828949621302189
2.0f16 的哈希值: 396734039217596648
相等值的哈希值是否相同: true
```

### func isInf()

```cangjie
public func isInf(): Bool
```

功能：判断某个浮点数 [Float16](./core_package_intrinsics.md#float16) 是否为无穷数值。

返回值：

- [Bool](./core_package_intrinsics.md#bool) - 如果 [Float16](./core_package_intrinsics.md#float16) 的值正无穷大或负无穷大，则返回 `true`；否则，返回 `false`。

示例：

<!-- verify -->
```cangjie
main() {
    // 测试 Float16.isInf 方法
    let normalValue: Float16 = 1.5f16
    let infValue = Float16.Inf
    let negInfValue = -Float16.Inf

    // 检查普通值是否为无穷
    println("1.5f16.isInf() 的结果: ${normalValue.isInf()}")

    // 检查正无穷是否为无穷
    println("Float16.Inf.isInf() 的结果: ${infValue.isInf()}")

    // 检查负无穷是否为无穷
    println("(-Float16.Inf).isInf() 的结果: ${negInfValue.isInf()}")
}
```

运行结果：

```text
1.5f16.isInf() 的结果: false
Float16.Inf.isInf() 的结果: true
(-Float16.Inf).isInf() 的结果: true
```

### func isNaN()

```cangjie
public func isNaN(): Bool
```

功能：判断某个浮点数 [Float16](./core_package_intrinsics.md#float16) 是否为非数值。

返回值：

- [Bool](./core_package_intrinsics.md#bool) - 如果 [Float16](./core_package_intrinsics.md#float16) 的值为非数值，则返回 `true`；否则，返回 `false`。

示例：

<!-- verify -->
```cangjie
main() {
    // 测试 Float16.isNaN 方法
    let normalValue: Float16 = 1.5f16
    let nanValue = Float16.NaN

    // 检查普通值是否为 NaN
    println("1.5f16.isNaN() 的结果: ${normalValue.isNaN()}")

    // 检查 NaN 值是否为 NaN
    println("Float16.NaN.isNaN() 的结果: ${nanValue.isNaN()}")
}
```

运行结果：

```text
1.5f16.isNaN() 的结果: false
Float16.NaN.isNaN() 的结果: true
```

### func isNormal()

```cangjie
public func isNormal(): Bool
```

功能：判断某个浮点数 [Float16](./core_package_intrinsics.md#float16) 是否为常规数值。

返回值：

- [Bool](./core_package_intrinsics.md#bool) - 如果 [Float16](./core_package_intrinsics.md#float16) 的值是正常的浮点数，返回 `true`；否则，返回 `false`。

示例：

<!-- verify -->
```cangjie
main() {
    // 测试 Float16.isNormal 方法
    let normalValue: Float16 = 1.5f16
    let zeroValue: Float16 = 0.0f16
    let infValue = Float16.Inf
    let nanValue = Float16.NaN

    // 检查普通值是否为常规数值
    println("1.5f16.isNormal() 的结果: ${normalValue.isNormal()}")

    // 检查零值是否为常规数值
    println("0.0f16.isNormal() 的结果: ${zeroValue.isNormal()}")

    // 检查无穷值是否为常规数值
    println("Float16.Inf.isNormal() 的结果: ${infValue.isNormal()}")

    // 检查 NaN 值是否为常规数值
    println("Float16.NaN.isNormal() 的结果: ${nanValue.isNormal()}")
}
```

运行结果：

```text
1.5f16.isNormal() 的结果: true
0.0f16.isNormal() 的结果: false
Float16.Inf.isNormal() 的结果: false
Float16.NaN.isNormal() 的结果: false
```

## extend Float16 <: ToString

```cangjie
extend Float16 <: ToString
```

功能：为 [Float16](core_package_intrinsics.md#float16) 类型扩展 [ToString](core_package_interfaces.md#interface-tostring) 接口，实现向 [String](core_package_structs.md#struct-string) 类型的转换。默认保留 6 位小数。

被扩展类型：

- [Float16](core_package_intrinsics.md#float16)

扩展接口：

- [ToString](core_package_interfaces.md#interface-tostring)

### func toString()

```cangjie
public func toString(): String
```

功能：将 [Float16](core_package_intrinsics.md#float16) 值转换为字符串。

返回值：

- [String](core_package_structs.md#struct-string) - 转化后的字符串。

示例：

<!-- verify -->
```cangjie
main() {
    // 测试 Float16.toString 方法
    let value1: Float16 = 1.5f16
    let value2: Float16 = 123.456f16
    let value3: Float16 = -0.001f16

    // 转换为字符串
    let str1 = value1.toString()
    let str2 = value2.toString()
    let str3 = value3.toString()

    println("1.5f16 转换为字符串: \"${str1}\"")
    println("123.456f16 转换为字符串: \"${str2}\"")
    println("-0.001f16 转换为字符串: \"${str3}\"")

    // 验证类型
    println("转换后的类型是否为 String: ${str1 is String}")
}
```

运行结果：

```text
1.5f16 转换为字符串: "1.500000"
123.456f16 转换为字符串: "123.437500"
-0.001f16 转换为字符串: "-0.001000"
转换后的类型是否为 String: true
```

## extend Float32

```cangjie
extend Float32
```

功能：支持与 [UInt32](core_package_intrinsics.md#uint32) 互相转换。

被扩展类型：

- [Float32](core_package_intrinsics.md#float32)

### static func fromBits(UInt32)

```cangjie
public static func fromBits(bits: UInt32): Float32
```

功能：将指定的 [UInt32](core_package_intrinsics.md#uint32)按照 `IEEE 754` 标准中 float32 的二进制位表示形式，直接转换为对应的[Float32](core_package_intrinsics.md#float32)浮点数。

参数：

- bits: [UInt32](core_package_intrinsics.md#uint32) - 要转换的数字。

返回值：

- [Float32](core_package_intrinsics.md#float32) - 转换结果，其位与参数 bits 值相同。

示例：

<!-- verify -->
```cangjie
main() {
    let v = Float32.fromBits(0x415E147Bu32)
    println(v)
}
```

运行结果：

```text
13.880000
```

### func toBits()

```cangjie
public func toBits(): UInt32
```

功能：将指定的 [Float32](core_package_intrinsics.md#float32) 数转换为以位表示的对应 [UInt32](core_package_intrinsics.md#uint32) 数。

返回值：

- [UInt32](core_package_intrinsics.md#uint32) - 转换结果，其值与 [Float32](core_package_intrinsics.md#float32) 的位表示相同。

示例：

<!-- verify -->
```cangjie
main() {
    println(13.88f32.toBits()) // 0x415E147B 1096684667
}
```

运行结果：

```text
1096684667
```

## extend Float32 <: Comparable\<Float32>

```cangjie
extend Float32 <: Comparable<Float32>
```

功能：为 [Float32](core_package_intrinsics.md#float32) 类型扩展 [Comparable](core_package_interfaces.md#interface-comparablet)\<[Float32](core_package_intrinsics.md#float32)> 接口，支持比较操作。

被扩展类型：

- [Float32](core_package_intrinsics.md#float32)

扩展接口：

- [Comparable](core_package_interfaces.md#interface-comparablet)\<[Float32](core_package_intrinsics.md#float32)>

### func compare(Float32)

```cangjie
public func compare(other: Float32): Ordering
```

功能：判断当前 [Float32](core_package_intrinsics.md#float32) 值与指定 [Float32](core_package_intrinsics.md#float32) 值的大小关系。`NaN` 无法参与比较，返回值无意义。

参数：

- other: [Float32](core_package_intrinsics.md#float32) - 待比较的另一个 [Float32](core_package_intrinsics.md#float32) 值。

返回值：

- [Ordering](core_package_enums.md#enum-ordering) - 如果大于，返回 [Ordering](core_package_enums.md#enum-ordering).GT；如果等于，返回 [Ordering](core_package_enums.md#enum-ordering).EQ；如果小于，返回 [Ordering](core_package_enums.md#enum-ordering).LT。

示例：

<!-- verify -->
```cangjie
main() {
    var num1: Float32 = 0.12
    var num2: Float32 = 0.234
    println(num1.compare(num2))
}
```

运行结果：

```text
Ordering.LT
```

## extend Float32 <: Hashable

```cangjie
extend Float32 <: Hashable
```

功能：为 [Float32](core_package_intrinsics.md#float32) 类型扩展 [Hashable](core_package_interfaces.md#interface-hashable) 接口，支持计算哈希值，并拓展该类型以支持一些数学常数。

被扩展类型：

- [Float32](core_package_intrinsics.md#float32)

扩展接口：

- [Hashable](core_package_interfaces.md#interface-hashable)

### static prop Inf

```cangjie
public static prop Inf: Float32
```

功能：获取单精度浮点数的无穷数。

类型：[Float32](./core_package_intrinsics.md#float32)

示例：

<!-- verify -->
```cangjie
main() {
    // 测试 Float32.Inf 属性
    let infValue = Float32.Inf
    println("Float32.Inf 的值: ${infValue}")

    // 验证它确实是无穷大
    println("Float32.Inf 是否为无穷大: ${infValue.isInf()}")
}
```

运行结果：

```text
Float32.Inf 的值: inf
Float32.Inf 是否为无穷大: true
```

### static prop Max

```cangjie
public static prop Max: Float32
```

功能：获取单精度浮点数的最大值。

类型：[Float32](./core_package_intrinsics.md#float32)

示例：

<!-- verify -->
```cangjie
main() {
    // 测试 Float32.Max 属性
    let maxValue = Float32.Max
    println("Float32.Max 的值: ${maxValue}")

    // 验证它确实是最大值：等于 -Float32.Min
    println("Float32.Max 是否等于 -Float32.Min: ${maxValue == -Float32.Min}")
}
```

运行结果：

```text
Float32.Max 的值: 340282346638528859811704183484516925440.000000
Float32.Max 是否等于 -Float32.Min: true
```

### static prop Min

```cangjie
public static prop Min: Float32
```

功能：获取单精度浮点数的最小值。

类型：[Float32](./core_package_intrinsics.md#float32)

示例：

<!-- verify -->
```cangjie
main() {
    // 测试 Float32.Min 属性
    let minValue = Float32.Min
    println("Float32.Min 的值: ${minValue}")

    // 验证它确实是最小值：等于 -Float32.Max
    println("Float32.Min 是否等于 -Float32.Max: ${minValue == -Float32.Max}")
}
```

运行结果：

```text
Float32.Min 的值: -340282346638528859811704183484516925440.000000
Float32.Min 是否等于 -Float32.Max: true
```

### static prop MinDenormal

```cangjie
public static prop MinDenormal: Float32
```

功能：获取单精度浮点数的最小次正规数。

类型：[Float32](./core_package_intrinsics.md#float32)

示例：

<!-- verify -->
```cangjie
main() {
    // 测试 Float32.MinDenormal 属性
    let minDenormalValue = Float32.MinDenormal

    // 验证它确实是最小次正规数：大于 0 且小于最小正规数
    println("Float32.MinDenormal 是否大于 0: ${minDenormalValue > Float32(0.0)}")
    println("Float32.MinDenormal 是否小于最小正规数: ${minDenormalValue < Float32.MinNormal}")
}
```

运行结果：

```text
Float32.MinDenormal 是否大于 0: true
Float32.MinDenormal 是否小于最小正规数: true
```

### static prop MinNormal

```cangjie
public static prop MinNormal: Float32
```

功能：获取单精度浮点数的最小正规数。

类型：[Float32](./core_package_intrinsics.md#float32)

示例：

<!-- verify -->
```cangjie
main() {
    // 测试 Float32.MinNormal 属性
    let minNormalValue = Float32.MinNormal
    println("Float32.MinNormal 的值: ${minNormalValue}")

    // 验证它确实是最小正规数
    println("Float32.MinNormal 是否为常规数值: ${minNormalValue.isNormal()}")
}
```

运行结果：

```text
Float32.MinNormal 的值: 0.000000
Float32.MinNormal 是否为常规数值: true
```

### static prop NaN

```cangjie
public static prop NaN: Float32
```

功能：获取单精度浮点数的非数。

类型：[Float32](./core_package_intrinsics.md#float32)

示例：

<!-- verify -->
```cangjie
main() {
    // 测试 Float32.NaN 属性
    let nanValue = Float32.NaN
    println("Float32.NaN 的值: ${nanValue}")

    // 验证它确实是 NaN
    println("Float32.NaN 是否为非数值: ${nanValue.isNaN()}")
}
```

运行结果：

```text
Float32.NaN 的值: nan
Float32.NaN 是否为非数值: true
```

### static func max(Float32, Float32, Array\<Float32>)

```cangjie
public static func max(a: Float32, b: Float32, others: Array<Float32>): Float32
```

功能：返回一组[Float32](./core_package_intrinsics.md#float32)中的最大值，此函数的第三个参数是一个变长参数，可以获取二个以上的[Float32](./core_package_intrinsics.md#float32)最大值，如果参数中有 `NaN`，该函数会返回 `NaN`。

参数：

- a: [Float32](./core_package_intrinsics.md#float32) - 第一个待比较的数。
- b: [Float32](./core_package_intrinsics.md#float32) - 第二个待比较的数。
- others: [Array](core_package_structs.md#struct-arrayt)\<[Float32](./core_package_intrinsics.md#float32)> - 其他待比较的数。

返回值：

- [Float32](./core_package_intrinsics.md#float32) - 返回参数中的最大值。

示例：

<!-- verify -->
```cangjie
main() {
    // 测试 Float32.max 静态方法
    let a: Float32 = 1.5f32
    let b: Float32 = 2.7f32
    let c: Float32 = 0.8f32
    let d: Float32 = 3.2f32

    // 使用 max 方法找出最大值
    let maxValue = Float32.max(a, b, c, d)
    println("Float32.max(1.5, 2.7, 0.8, 3.2) 的结果: ${maxValue}")

    // 验证结果
    println("最大值是否正确: ${maxValue == d}")
}
```

运行结果：

```text
Float32.max(1.5, 2.7, 0.8, 3.2) 的结果: 3.200000
最大值是否正确: true
```

### static func min(Float32, Float32, Array\<Float32>)

```cangjie
public static func min(a: Float32, b: Float32, others: Array<Float32>): Float32
```

功能：返回一组[Float32](./core_package_intrinsics.md#float32)中的最小值，此函数的第三个参数是一个变长参数，可以获取二个以上的[Float32](./core_package_intrinsics.md#float32)最小值，如果参数中有 `NaN`，该函数会返回 `NaN`。

参数：

- a: [Float32](./core_package_intrinsics.md#float32) - 第一个待比较的数。
- b: [Float32](./core_package_intrinsics.md#float32) - 第二个待比较的数。
- others: [Array](core_package_structs.md#struct-arrayt)\<[Float32](./core_package_intrinsics.md#float32)> - 其他待比较的数。

返回值：

- [Float32](./core_package_intrinsics.md#float32) - 返回参数中的最小值。

示例：

<!-- verify -->
```cangjie
main() {
    // 测试 Float32.min 静态方法
    let a: Float32 = 1.5f32
    let b: Float32 = 2.7f32
    let c: Float32 = 0.8f32
    let d: Float32 = 3.2f32

    // 使用 min 方法找出最小值
    let minValue = Float32.min(a, b, c, d)
    println("Float32.min(1.5, 2.7, 0.8, 3.2) 的结果: ${minValue}")

    // 验证结果
    println("最小值是否正确: ${minValue == c}")
}
```

运行结果：

```text
Float32.min(1.5, 2.7, 0.8, 3.2) 的结果: 0.800000
最小值是否正确: true
```

### func hashCode()

```cangjie
public func hashCode(): Int64
```

功能：获取哈希值。

返回值：

- [Int64](core_package_intrinsics.md#int64) - 哈希值。

示例：

<!-- verify -->
```cangjie
main() {
    // 测试 Float32.hashCode 方法
    let value1: Float32 = 1.5f32
    let value2: Float32 = 1.5f32
    let value3: Float32 = 2.0f32

    // 计算哈希值
    let hash1 = value1.hashCode()
    let hash2 = value2.hashCode()
    let hash3 = value3.hashCode()

    println("1.5f32 的哈希值: ${hash1}")
    println("1.5f32 的哈希值: ${hash2}")
    println("2.0f32 的哈希值: ${hash3}")

    // 验证相等的值具有相同的哈希值
    println("相等值的哈希值是否相同: ${hash1 == hash2}")
}
```

运行结果：

```text
1.5f32 的哈希值: 7729828949621302189
1.5f32 的哈希值: 7729828949621302189
2.0f32 的哈希值: 396734039217596648
相等值的哈希值是否相同: true
```

### func isInf()

```cangjie
public func isInf(): Bool
```

功能：判断某个浮点数 [Float32](./core_package_intrinsics.md#float32) 是否为无穷数值。

返回值：

- [Bool](./core_package_intrinsics.md#bool) - 如果 [Float32](./core_package_intrinsics.md#float32) 的值正无穷大或负无穷大，则返回 `true`；否则，返回 `false`。

示例：

<!-- verify -->
```cangjie
main() {
    // 测试 Float32.isInf 方法
    let normalValue: Float32 = 1.5f32
    let infValue = Float32.Inf
    let negInfValue = -Float32.Inf

    // 检查普通值是否为无穷
    println("1.5f32.isInf() 的结果: ${normalValue.isInf()}")

    // 检查正无穷是否为无穷
    println("Float32.Inf.isInf() 的结果: ${infValue.isInf()}")

    // 检查负无穷是否为无穷
    println("(-Float32.Inf).isInf() 的结果: ${negInfValue.isInf()}")
}
```

运行结果：

```text
1.5f32.isInf() 的结果: false
Float32.Inf.isInf() 的结果: true
(-Float32.Inf).isInf() 的结果: true
```

### func isNaN()

```cangjie
public func isNaN(): Bool
```

功能：判断某个浮点数 [Float32](./core_package_intrinsics.md#float32) 是否为非数值。

返回值：

- [Bool](./core_package_intrinsics.md#bool) - 如果 [Float32](./core_package_intrinsics.md#float32) 的值为非数值，则返回 `true`；否则，返回 `false`。

示例：

<!-- verify -->
```cangjie
main() {
    // 测试 Float32.isNaN 方法
    let normalValue: Float32 = 1.5f32
    let nanValue = Float32.NaN

    // 检查普通值是否为 NaN
    println("1.5f32.isNaN() 的结果: ${normalValue.isNaN()}")

    // 检查 NaN 值是否为 NaN
    println("Float32.NaN.isNaN() 的结果: ${nanValue.isNaN()}")
}
```

运行结果：

```text
1.5f32.isNaN() 的结果: false
Float32.NaN.isNaN() 的结果: true
```

### func isNormal()

```cangjie
public func isNormal(): Bool
```

功能：判断某个浮点数 [Float32](./core_package_intrinsics.md#float32) 是否为常规数值。

返回值：

- [Bool](./core_package_intrinsics.md#bool) - 如果 [Float32](./core_package_intrinsics.md#float32) 的值是正常的浮点数，返回 `true`；否则，返回 `false`。

示例：

<!-- verify -->
```cangjie
main() {
    // 测试 Float32.isNormal 方法
    let normalValue: Float32 = 1.5f32
    let zeroValue: Float32 = 0.0f32
    let infValue = Float32.Inf
    let nanValue = Float32.NaN

    // 检查普通值是否为常规数值
    println("1.5f32.isNormal() 的结果: ${normalValue.isNormal()}")

    // 检查零值是否为常规数值
    println("0.0f32.isNormal() 的结果: ${zeroValue.isNormal()}")

    // 检查无穷值是否为常规数值
    println("Float32.Inf.isNormal() 的结果: ${infValue.isNormal()}")

    // 检查 NaN 值是否为常规数值
    println("Float32.NaN.isNormal() 的结果: ${nanValue.isNormal()}")
}
```

运行结果：

```text
1.5f32.isNormal() 的结果: true
0.0f32.isNormal() 的结果: false
Float32.Inf.isNormal() 的结果: false
Float32.NaN.isNormal() 的结果: false
```

## extend Float32 <: ToString

```cangjie
extend Float32 <: ToString
```

功能：为 [Float32](core_package_intrinsics.md#float32) 类型扩展 [ToString](core_package_interfaces.md#interface-tostring) 接口，实现向 [String](core_package_structs.md#struct-string) 类型的转换。默认保留 6 位小数。

被扩展类型：

- [Float32](core_package_intrinsics.md#float32)

扩展接口：

- [ToString](core_package_interfaces.md#interface-tostring)

### func toString()

```cangjie
public func toString(): String
```

功能：将 [Float32](core_package_intrinsics.md#float32) 值转换为字符串。

返回值：

- [String](core_package_structs.md#struct-string) - 转化后的字符串。

示例：

<!-- verify -->
```cangjie
main() {
    // 测试 Float32.toString 方法
    let value1: Float32 = 1.5f32
    let value2: Float32 = 123.456f32
    let value3: Float32 = -0.001f32

    // 转换为字符串
    let str1 = value1.toString()
    let str2 = value2.toString()
    let str3 = value3.toString()

    println("1.5f32 转换为字符串: \"${str1}\"")
    println("123.456f32 转换为字符串: \"${str2}\"")
    println("-0.001f32 转换为字符串: \"${str3}\"")

    // 验证类型
    println("转换后的类型是否为 String: ${str1 is String}")
}
```

运行结果：

```text
1.5f32 转换为字符串: "1.500000"
123.456f32 转换为字符串: "123.456001"
-0.001f32 转换为字符串: "-0.001000"
转换后的类型是否为 String: true
```

## extend Float64

```cangjie
extend Float64
```

功能：支持与 [UInt64](core_package_intrinsics.md#uint64) 互相转换。

被扩展类型：

- [Float64](core_package_intrinsics.md#float64)

### static func fromBits(UInt64)

```cangjie
public static func fromBits(bits: UInt64): Float64
```

功能：将指定的 [UInt64](core_package_intrinsics.md#uint64)按照 `IEEE 754` 标准中 float64 的二进制位表示形式，直接转换为对应的[Float64](core_package_intrinsics.md#float64)浮点数。

参数：

- bits: [UInt64](core_package_intrinsics.md#uint64) - 要转换的数字。

返回值：

- [Float64](core_package_intrinsics.md#float64) - 转换结果，其位与参数 bits 值相同。

示例：

<!-- verify -->
```cangjie
main() {
    let v = Float64.fromBits(0x402BC28F5C28F5C3u64)
    println(v)
}
```

运行结果：

```text
13.880000
```

### func toBits()

```cangjie
public func toBits(): UInt64
```

功能：将指定的 Float64 数转换为以位表示的对应 [UInt64](core_package_intrinsics.md#uint64) 数。

返回值：

- [UInt64](core_package_intrinsics.md#uint64) - 转换结果，其值与 [Float64](core_package_intrinsics.md#float64) 的位表示相同。

示例：

<!-- verify -->
```cangjie
main() {
    println(13.88f64.toBits()) // 0x402BC28F5C28F5C3 4624003363408246211
}
```

运行结果：

```text
4624003363408246211
```

## extend Float64

```cangjie
extend Float64
```

功能：拓展了 [Float64](../../core/core_package_api/core_package_intrinsics.md#float64) 类型作为左操作数和 [Duration](../../core/core_package_api/core_package_structs.md#struct-duration) 类型作为右操作数的乘法运算。

被扩展类型：

- [Float64](../../core/core_package_api/core_package_intrinsics.md#float64)

### operator func *(Duration)

```cangjie
public operator func *(r: Duration): Duration
```

功能：实现 [Float64](../../core/core_package_api/core_package_intrinsics.md#float64) 类型和 [Duration](../../core/core_package_api/core_package_structs.md#struct-duration) 类型的乘法，即 [Float64](../../core/core_package_api/core_package_intrinsics.md#float64) * [Duration](../../core/core_package_api/core_package_structs.md#struct-duration) 运算。

参数：

- r: [Duration](../../core/core_package_api/core_package_structs.md#struct-duration) - [Duration](../../core/core_package_api/core_package_structs.md#struct-duration) 实例。

返回值：

- [Duration](../../core/core_package_api/core_package_structs.md#struct-duration) - [Float64](../../core/core_package_api/core_package_intrinsics.md#float64) 类型实例和 `r` 的乘积。

异常：

- [ArithmeticException](../../core/core_package_api/core_package_exceptions.md#class-arithmeticexception) - 当相乘后的结果超出 [Duration](../../core/core_package_api/core_package_structs.md#struct-duration) 的表示范围时，抛出异常。

示例：

<!-- verify -->
```cangjie
main() {
    // 测试 Float64 * Duration 运算
    let floatVal: Float64 = 2.5
    let duration: Duration = Duration.second

    let result = floatVal * duration

    println("Float64值: ${floatVal}")
    println("Duration值: ${duration}")
    println("运算结果: ${result}")
}
```

运行结果：

```text
Float64值: 2.500000
Duration值: 1s
运算结果: 2s500ms
```

## extend Float64 <: Comparable\<Float64>

```cangjie
extend Float64 <: Comparable<Float64>
```

功能：为 [Float64](core_package_intrinsics.md#float64) 类型扩展 [Comparable](core_package_interfaces.md#interface-comparablet)\<[Float64](core_package_intrinsics.md#float64)> 接口，支持比较操作。

被扩展类型：

- [Float64](core_package_intrinsics.md#float64)

扩展接口：

- [Comparable](core_package_interfaces.md#interface-comparablet)\<[Float64](core_package_intrinsics.md#float64)>

### func compare(Float64)

```cangjie
public func compare(other: Float64): Ordering
```

功能：判断当前 [Float64](core_package_intrinsics.md#float64) 值与指定 [Float64](core_package_intrinsics.md#float64) 值的大小关系。`NaN` 无法参与比较，返回值无意义。

参数：

- other: [Float64](core_package_intrinsics.md#float64) - 待比较的另一个 [Float64](core_package_intrinsics.md#float64) 值。

返回值：

- [Ordering](core_package_enums.md#enum-ordering) - 如果大于，返回 [Ordering](core_package_enums.md#enum-ordering).GT；如果等于，返回 [Ordering](core_package_enums.md#enum-ordering).EQ；如果小于，返回 [Ordering](core_package_enums.md#enum-ordering).LT。

示例：

<!-- verify -->
```cangjie
main() {
    var num1: Float64 = 0.12
    var num2: Float64 = 0.234
    println(num1.compare(num2))
}
```

运行结果：

```text
Ordering.LT
```

## extend Float64 <: Hashable

```cangjie
extend Float64 <: Hashable
```

功能：为 [Float64](core_package_intrinsics.md#float64) 类型扩展 [Hashable](core_package_interfaces.md#interface-hashable) 接口，支持计算哈希值，并拓展该类型以支持一些数学常数。

被扩展类型：

- [Float64](core_package_intrinsics.md#float64)

扩展接口：

- [Hashable](core_package_interfaces.md#interface-hashable)

### static prop Inf

```cangjie
public static prop Inf: Float64
```

功能：获取双精度浮点数的无穷数。

类型：[Float64](./core_package_intrinsics.md#float64)

示例：

<!-- verify -->
```cangjie
main() {
    // 测试 Float64.Inf 属性
    let infValue = Float64.Inf
    println("Float64.Inf 的值: ${infValue}")

    // 验证它确实是无穷大
    println("Float64.Inf 是否为无穷大: ${infValue.isInf()}")
}
```

运行结果：

```text
Float64.Inf 的值: inf
Float64.Inf 是否为无穷大: true
```

### static prop Max

```cangjie
public static prop Max: Float64
```

功能：获取双精度浮点数的最大值。

类型：[Float64](./core_package_intrinsics.md#float64)

示例：

<!-- verify -->
```cangjie
main() {
    // 测试 Float64.Max 属性
    let maxValue = Float64.Max
    println("Float64.Max 的值: ${maxValue}")

    // 验证它确实是最大值：等于 -Float64.Min
    println("Float64.Max 是否等于 -Float64.Min: ${maxValue == -Float64.Min}")
}
```

运行结果：

```text
Float64.Max 的值: 179769313486231570814527423731704356798070567525844996598917476803157260780028538760589558632766878171540458953514382464234321326889464182768467546703537516986049910576551282076245490090389328944075868508455133942304583236903222948165808559332123348274797826204144723168738177180919299881250404026184124858368.000000
Float64.Max 是否等于 -Float64.Min: true
```

### static prop Min

```cangjie
public static prop Min: Float64
```

功能：获取双精度浮点数的最小值。

类型：[Float64](./core_package_intrinsics.md#float64)

示例：

<!-- verify -->
```cangjie
main() {
    // 测试 Float64.Min 属性
    let minValue = Float64.Min
    println("Float64.Min 的值: ${minValue}")

    // 验证它确实是最小值：等于 -Float64.Max
    println("Float64.Min 是否等于 -Float64.Max: ${minValue == -Float64.Max}")
}
```

运行结果：

```text
Float64.Min 的值: -179769313486231570814527423731704356798070567525844996598917476803157260780028538760589558632766878171540458953514382464234321326889464182768467546703537516986049910576551282076245490090389328944075868508455133942304583236903222948165808559332123348274797826204144723168738177180919299881250404026184124858368.000000
Float64.Min 是否等于 -Float64.Max: true
```

### static prop MinDenormal

```cangjie
public static prop MinDenormal: Float64
```

功能：获取双精度浮点数的最小次正规数。

类型：[Float64](./core_package_intrinsics.md#float64)

示例：

<!-- verify -->
```cangjie
main() {
    // 测试 Float64.MinDenormal 属性
    let minDenormalValue = Float64.MinDenormal

    // 验证它确实是最小次正规数：大于 0 且小于最小正规数
    println("Float64.MinDenormal 是否大于 0: ${minDenormalValue > Float64(0.0)}")
    println("Float64.MinDenormal 是否小于最小正规数: ${minDenormalValue < Float64.MinNormal}")
}
```

运行结果：

```text
Float64.MinDenormal 是否大于 0: true
Float64.MinDenormal 是否小于最小正规数: true
```

### static prop MinNormal

```cangjie
public static prop MinNormal: Float64
```

功能：获取双精度浮点数的最小正规数。

类型：[Float64](./core_package_intrinsics.md#float64)

示例：

<!-- verify -->
```cangjie
main() {
    // 测试 Float64.MinNormal 属性
    let minNormalValue = Float64.MinNormal
    println("Float64.MinNormal 的值: ${minNormalValue}")

    // 验证它确实是最小正规数
    println("Float64.MinNormal 是否为常规数值: ${minNormalValue.isNormal()}")
}
```

运行结果：

```text
Float64.MinNormal 的值: 0.000000
Float64.MinNormal 是否为常规数值: true
```

### static prop NaN

```cangjie
public static prop NaN: Float64
```

功能：获取双精度浮点数的非数。

类型：[Float64](./core_package_intrinsics.md#float64)

示例：

<!-- verify -->
```cangjie
main() {
    // 测试 Float64.NaN 属性
    let nanValue = Float64.NaN
    println("Float64.NaN 的值: ${nanValue}")

    // 验证它确实是 NaN
    println("Float64.NaN 是否为非数值: ${nanValue.isNaN()}")
}
```

运行结果：

```text
Float64.NaN 的值: nan
Float64.NaN 是否为非数值: true
```

### static func max(Float64, Float64, Array\<Float64>)

```cangjie
public static func max(a: Float64, b: Float64, others: Array<Float64>): Float64
```

功能：返回一组[Float64](./core_package_intrinsics.md#float64)中的最大值，此函数的第三个参数是一个变长参数，可以获取二个以上的[Float64](./core_package_intrinsics.md#float64)最大值，如果参数中有 `NaN`，该函数会返回 `NaN`。

参数：

- a: [Float64](./core_package_intrinsics.md#float64) - 第一个待比较的数。
- b: [Float64](./core_package_intrinsics.md#float64) - 第二个待比较的数。
- others: [Array](core_package_structs.md#struct-arrayt)\<[Float64](./core_package_intrinsics.md#float64)> - 其他待比较的数。

返回值：

- [Float64](./core_package_intrinsics.md#float64) - 返回参数中的最大值。

示例：

<!-- verify -->
```cangjie
main() {
    // 测试 Float64.max 静态方法
    let a: Float64 = 1.5f64
    let b: Float64 = 2.7f64
    let c: Float64 = 0.8f64
    let d: Float64 = 3.2f64

    // 使用 max 方法找出最大值
    let maxValue = Float64.max(a, b, [c, d])
    println("Float64.max(1.5, 2.7, [0.8, 3.2]) 的结果: ${maxValue}")

    // 验证结果
    println("最大值是否正确: ${maxValue == d}")
}
```

运行结果：

```text
Float64.max(1.5, 2.7, [0.8, 3.2]) 的结果: 3.200000
最大值是否正确: true
```

### static func min(Float64, Float64, Array\<Float64>)

```cangjie
public static func min(a: Float64, b: Float64, others: Array<Float64>): Float64
```

功能：返回一组[Float64](./core_package_intrinsics.md#float64)中的最小值，此函数的第三个参数是一个变长参数，可以获取二个以上的[Float64](./core_package_intrinsics.md#float64)最小值，如果参数中有 `NaN`，该函数会返回 `NaN`。

参数：

- a: [Float64](./core_package_intrinsics.md#float64) - 第一个待比较的数。
- b: [Float64](./core_package_intrinsics.md#float64) - 第二个待比较的数。
- others: [Array](core_package_structs.md#struct-arrayt)\<[Float64](./core_package_intrinsics.md#float64)> - 其他待比较的数。

返回值：

- [Float64](./core_package_intrinsics.md#float64) - 返回参数中的最小值。

示例：

<!-- verify -->
```cangjie
main() {
    // 测试 Float64.min 静态方法
    let a: Float64 = 1.5f64
    let b: Float64 = 2.7f64
    let c: Float64 = 0.8f64
    let d: Float64 = 3.2f64

    // 使用 min 方法找出最小值
    let minValue = Float64.min(a, b, [c, d])
    println("Float64.min(1.5, 2.7, [0.8, 3.2]) 的结果: ${minValue}")

    // 验证结果
    println("最小值是否正确: ${minValue == c}")
}
```

运行结果：

```text
Float64.min(1.5, 2.7, [0.8, 3.2]) 的结果: 0.800000
最小值是否正确: true
```

### func hashCode()

```cangjie
public func hashCode(): Int64
```

功能：获取哈希值。

返回值：

- [Int64](core_package_intrinsics.md#int64) - 哈希值。

示例：

<!-- verify -->
```cangjie
main() {
    // 测试 Float64.hashCode 方法
    let value1: Float64 = 1.5f64
    let value2: Float64 = 1.5f64
    let value3: Float64 = 2.0f64

    // 计算哈希值
    let hash1 = value1.hashCode()
    let hash2 = value2.hashCode()
    let hash3 = value3.hashCode()

    println("1.5f64 的哈希值: ${hash1}")
    println("1.5f64 的哈希值: ${hash2}")
    println("2.0f64 的哈希值: ${hash3}")

    // 验证相等的值具有相同的哈希值
    println("相等值的哈希值是否相同: ${hash1 == hash2}")
}
```

运行结果：

```text
1.5f64 的哈希值: -991904502584296975
1.5f64 的哈希值: -991904502584296975
2.0f64 的哈希值: 8889205371494514623
相等值的哈希值是否相同: true
```

### func isInf()

```cangjie
public func isInf(): Bool
```

功能：判断某个浮点数 [Float64](./core_package_intrinsics.md#float64) 是否为无穷数值。

返回值：

- [Bool](./core_package_intrinsics.md#bool) - 如果 [Float64](./core_package_intrinsics.md#float64) 的值正无穷大或负无穷大，则返回 `true`；否则，返回 `false`。

示例：

<!-- verify -->
```cangjie
main() {
    // 测试 Float64.isInf 方法
    let normalValue: Float64 = 1.5f64
    let infValue = Float64.Inf
    let negInfValue = -Float64.Inf

    // 检查普通值是否为无穷
    println("1.5f64.isInf() 的结果: ${normalValue.isInf()}")

    // 检查正无穷是否为无穷
    println("Float64.Inf.isInf() 的结果: ${infValue.isInf()}")

    // 检查负无穷是否为无穷
    println("(-Float64.Inf).isInf() 的结果: ${negInfValue.isInf()}")
}
```

运行结果：

```text
1.5f64.isInf() 的结果: false
Float64.Inf.isInf() 的结果: true
(-Float64.Inf).isInf() 的结果: true
```

### func isNaN()

```cangjie
public func isNaN(): Bool
```

功能：判断某个浮点数 [Float64](./core_package_intrinsics.md#float64) 是否为非数值。

返回值：

- [Bool](./core_package_intrinsics.md#bool) - 如果 [Float64](./core_package_intrinsics.md#float64) 的值为非数值，则返回 `true`；否则，返回 `false`。

示例：

<!-- verify -->
```cangjie
main() {
    // 测试 Float64.isNaN 方法
    let normalValue: Float64 = 1.5f64
    let nanValue = Float64.NaN

    // 检查普通值是否为 NaN
    println("1.5f64.isNaN() 的结果: ${normalValue.isNaN()}")

    // 检查 NaN 值是否为 NaN
    println("Float64.NaN.isNaN() 的结果: ${nanValue.isNaN()}")
}
```

运行结果：

```text
1.5f64.isNaN() 的结果: false
Float64.NaN.isNaN() 的结果: true
```

### func isNormal()

```cangjie
public func isNormal(): Bool
```

功能：判断某个浮点数 [Float64](./core_package_intrinsics.md#float64) 是否为常规数值。

返回值：

- [Bool](./core_package_intrinsics.md#bool) - 如果 [Float64](./core_package_intrinsics.md#float64) 的值是正常的浮点数，返回 `true`；否则，返回 `false`。

示例：

<!-- verify -->
```cangjie
main() {
    // 测试 Float64.isNormal 方法
    let normalValue: Float64 = 1.5f64
    let zeroValue: Float64 = 0.0f64
    let infValue = Float64.Inf
    let nanValue = Float64.NaN

    // 检查普通值是否为常规数值
    println("1.5f64.isNormal() 的结果: ${normalValue.isNormal()}")

    // 检查零值是否为常规数值
    println("0.0f64.isNormal() 的结果: ${zeroValue.isNormal()}")

    // 检查无穷值是否为常规数值
    println("Float64.Inf.isNormal() 的结果: ${infValue.isNormal()}")

    // 检查 NaN 值是否为常规数值
    println("Float64.NaN.isNormal() 的结果: ${nanValue.isNormal()}")
}
```

运行结果：

```text
1.5f64.isNormal() 的结果: true
0.0f64.isNormal() 的结果: false
Float64.Inf.isNormal() 的结果: false
Float64.NaN.isNormal() 的结果: false
```

## extend Float64 <: ToString

```cangjie
extend Float64 <: ToString
```

功能：为 [Float64](core_package_intrinsics.md#float64) 类型扩展 [ToString](core_package_interfaces.md#interface-tostring) 接口，实现向 [String](core_package_structs.md#struct-string) 类型的转换。默认保留 6 位小数。

被扩展类型：

- [Float64](core_package_intrinsics.md#float64)

扩展接口：

- [ToString](core_package_interfaces.md#interface-tostring)

### func toString()

```cangjie
public func toString(): String
```

功能：将 [Float64](core_package_intrinsics.md#float64) 值转换为字符串。

返回值：

- [String](core_package_structs.md#struct-string) - 转化后的字符串。

示例：

<!-- verify -->
```cangjie
main() {
    // 测试 Float64.toString 方法
    let value1: Float64 = 1.5f64
    let value2: Float64 = 123.456f64
    let value3: Float64 = -0.001f64

    // 转换为字符串
    let str1 = value1.toString()
    let str2 = value2.toString()
    let str3 = value3.toString()

    println("1.5f64 转换为字符串: \"${str1}\"")
    println("123.456f64 转换为字符串: \"${str2}\"")
    println("-0.001f64 转换为字符串: \"${str3}\"")

    // 验证类型
    println("转换后的类型是否为 String: ${str1 is String}")
}
```

运行结果：

```text
1.5f64 转换为字符串: "1.500000"
123.456f64 转换为字符串: "123.456000"
-0.001f64 转换为字符串: "-0.001000"
转换后的类型是否为 String: true
```

## extend Int16 <: Comparable\<Int16>

```cangjie
extend Int16 <: Comparable<Int16>
```

功能：为 [Int16](core_package_intrinsics.md#int16) 类型扩展 [Comparable](core_package_interfaces.md#interface-comparablet)\<[Int16](core_package_intrinsics.md#int16)> 接口，支持比较操作。

被扩展类型：

- [Int16](core_package_intrinsics.md#int16)

扩展接口：

- [Comparable](core_package_interfaces.md#interface-comparablet)\<[Int16](core_package_intrinsics.md#int16)>

### func compare(Int16)

```cangjie
public func compare(other: Int16): Ordering
```

功能：判断当前 [Int16](core_package_intrinsics.md#int16) 值与指定 [Int16](core_package_intrinsics.md#int16) 值的大小关系。

参数：

- other: [Int16](core_package_intrinsics.md#int16) - 待比较的另一个 [Int16](core_package_intrinsics.md#int16) 值。

返回值：

- [Ordering](core_package_enums.md#enum-ordering) - 如果大于，返回 [Ordering](core_package_enums.md#enum-ordering).GT；如果等于，返回 [Ordering](core_package_enums.md#enum-ordering).EQ；如果小于，返回 [Ordering](core_package_enums.md#enum-ordering).LT。

示例：

<!-- verify -->
```cangjie
main() {
    var num1: Int16 = 2
    var num2: Int16 = 3
    println(num1.compare(num2))
}
```

运行结果：

```text
Ordering.LT
```

## extend Int16 <: Countable\<Int16>

```cangjie
extend Int16 <: Countable<Int16>
```

功能：为 [Int16](core_package_intrinsics.md#int16) 类型扩展 [Countable](core_package_interfaces.md#interface-countablet)\<[Int16](core_package_intrinsics.md#int16)> 接口，支持计数操作。

被扩展类型：

- [Int16](core_package_intrinsics.md#int16)

扩展接口：

- [Countable](core_package_interfaces.md#interface-countablet)\<[Int16](core_package_intrinsics.md#int16)>

### func next(Int64)

```cangjie
public func next(right: Int64): Int16
```

功能：获取在数轴上当前 [Int16](core_package_intrinsics.md#int16) 位置往右移动 `right` 后对应位置的 [Int16](core_package_intrinsics.md#int16) 值。如果值溢出，则会从数轴最左边继续移动。

参数：

- right: [Int64](core_package_intrinsics.md#int64) - 往右数的个数。

返回值：

- [Int16](core_package_intrinsics.md#int16) - 往右数 `right` 后所到位置的 [Int16](core_package_intrinsics.md#int16) 值。

示例：

<!-- verify -->
```cangjie
main() {
    var num1: Int16 = 32767
    var num2: Int16 = 3
    println(num1.next(5))
    println(num2.next(10))
}
```

运行结果：

```text
-32764
13
```

### func position()

```cangjie
public func position(): Int64
```

功能：获取当前 [Int16](core_package_intrinsics.md#int16) 值的位置信息，即将该 [Int16](core_package_intrinsics.md#int16) 转换为 [Int64](core_package_intrinsics.md#int64) 值。

返回值：

- [Int64](core_package_intrinsics.md#int64) - 当前 [Int16](core_package_intrinsics.md#int16) 值的位置信息。

示例：

<!-- verify -->
```cangjie
main() {
    var num1: Int16 = 32767
    var num2: Int16 = 3
    println(num1.position())
    println(num2.position())
}
```

运行结果：

```text
32767
3
```

## extend Int16 <: Hashable

```cangjie
extend Int16 <: Hashable
```

功能：为 [Int16](core_package_intrinsics.md#int16) 类型扩展 [Hashable](core_package_interfaces.md#interface-hashable) 接口，支持计算哈希值，并拓展该类型以支持一些数学常数。

被扩展类型：

- [Int16](core_package_intrinsics.md#int16)

扩展接口：

- [Hashable](core_package_interfaces.md#interface-hashable)

### static prop Max

```cangjie
public static prop Max: Int16
```

功能：获取 16 位有符号整数的最大值。

类型：[Int16](./core_package_intrinsics.md#int16)

示例：

<!-- verify -->
```cangjie
main() {
    // 测试 Int16.Max 属性
    let maxValue = Int16.Max
    println("Int16.Max 的值: ${maxValue}")

    // 验证它确实是最大值
    println("Int16.Max 是否等于 32767: ${maxValue == 32767}")
}
```

运行结果：

```text
Int16.Max 的值: 32767
Int16.Max 是否等于 32767: true
```

### static prop Min

```cangjie
public static prop Min: Int16
```

功能：获取 16 位有符号整数的最小值。

类型：[Int16](./core_package_intrinsics.md#int16)

示例：

<!-- verify -->
```cangjie
main() {
    // 测试 Int16.Min 属性
    let minValue = Int16.Min
    println("Int16.Min 的值: ${minValue}")

    // 验证它确实是最小值
    println("Int16.Min 是否等于 -32768: ${minValue == -32768}")
}
```

运行结果：

```text
Int16.Min 的值: -32768
Int16.Min 是否等于 -32768: true
```

### func hashCode()

```cangjie
public func hashCode(): Int64
```

功能：获取哈希值。

返回值：

- [Int64](core_package_intrinsics.md#int64) - 哈希值。

示例：

<!-- verify -->
```cangjie
main() {
    // 测试 Int16.hashCode 方法
    let value1: Int16 = 42
    let value2: Int16 = 42
    let value3: Int16 = -100

    // 计算哈希值
    let hash1 = value1.hashCode()
    let hash2 = value2.hashCode()
    let hash3 = value3.hashCode()

    println("42 的哈希值: ${hash1}")
    println("42 的哈希值: ${hash2}")
    println("-100 的哈希值: ${hash3}")

    // 验证相等的值具有相同的哈希值
    println("相等值的哈希值是否相同: ${hash1 == hash2}")
}
```

运行结果：

```text
42 的哈希值: 42
42 的哈希值: 42
-100 的哈希值: -100
相等值的哈希值是否相同: true
```

## extend Int16 <: ToString

```cangjie
extend Int16 <: ToString
```

功能：这里为 [Int16](core_package_intrinsics.md#int16) 类型扩展 [ToString](core_package_interfaces.md#interface-tostring) 接口，实现向 [String](core_package_structs.md#struct-string) 类型的转换。

被扩展类型：

- [Int16](core_package_intrinsics.md#int16)

扩展接口：

- [ToString](core_package_interfaces.md#interface-tostring)

### func toString()

```cangjie
public func toString(): String
```

功能：将 [Int16](core_package_intrinsics.md#int16) 值转换为字符串。

返回值：

- [String](core_package_structs.md#struct-string) - 转化后的字符串。

示例：

<!-- verify -->
```cangjie
main() {
    // 测试 Int16.toString 方法
    let value1: Int16 = 42
    let value2: Int16 = -123
    let value3: Int16 = 0

    // 转换为字符串
    let str1 = value1.toString()
    let str2 = value2.toString()
    let str3 = value3.toString()

    println("42 转换为字符串: \"${str1}\"")
    println("-123 转换为字符串: \"${str2}\"")
    println("0 转换为字符串: \"${str3}\"")

    // 验证类型
    println("转换后的类型是否为 String: ${str1 is String}")
}
```

运行结果：

```text
42 转换为字符串: "42"
-123 转换为字符串: "-123"
0 转换为字符串: "0"
转换后的类型是否为 String: true
```

## extend Int32 <: Comparable\<Int32>

```cangjie
extend Int32 <: Comparable<Int32>
```

功能：为 [Int32](core_package_intrinsics.md#int32) 类型扩展 [Comparable](core_package_interfaces.md#interface-comparablet)\<[Int32](core_package_intrinsics.md#int32)> 接口，支持比较操作。

被扩展类型：

- [Int32](core_package_intrinsics.md#int32)

扩展接口：

- [Comparable](core_package_interfaces.md#interface-comparablet)\<[Int32](core_package_intrinsics.md#int32)>

### func compare(Int32)

```cangjie
public func compare(other: Int32): Ordering
```

功能：判断当前 [Int32](core_package_intrinsics.md#int32) 值与指定 [Int32](core_package_intrinsics.md#int32) 值的大小关系。

参数：

- other: [Int32](core_package_intrinsics.md#int32) - 待比较的另一个 [Int32](core_package_intrinsics.md#int32) 值。

返回值：

- [Ordering](core_package_enums.md#enum-ordering) - 如果大于，返回 [Ordering](core_package_enums.md#enum-ordering).GT；如果等于，返回 [Ordering](core_package_enums.md#enum-ordering).EQ；如果小于，返回 [Ordering](core_package_enums.md#enum-ordering).LT。

示例：

<!-- verify -->
```cangjie
main() {
    var num1: Int32 = 8
    var num2: Int32 = 10
    println(num1.compare(num2))
}
```

运行结果：

```text
Ordering.LT
```

## extend Int32 <: Countable\<Int32>

```cangjie
extend Int32 <: Countable<Int32>
```

功能：为 [Int32](core_package_intrinsics.md#int32) 类型扩展 [Countable](core_package_interfaces.md#interface-countablet)\<[Int32](core_package_intrinsics.md#int32)> 接口，支持计数操作。

被扩展类型：

- [Int32](core_package_intrinsics.md#int32)

扩展接口：

- [Countable](core_package_interfaces.md#interface-countablet)\<[Int32](core_package_intrinsics.md#int32)>

### func next(Int64)

```cangjie
public func next(right: Int64): Int32
```

功能：获取在数轴上当前 [Int32](core_package_intrinsics.md#int32) 位置往右移动 `right` 后对应位置的 [Int32](core_package_intrinsics.md#int32) 值。如果值溢出，则会从数轴最左边继续移动。

参数：

- right: [Int64](core_package_intrinsics.md#int64) - 往右数的个数。

返回值：

- [Int32](core_package_intrinsics.md#int32) - 往右数 `right` 后所到位置的 [Int32](core_package_intrinsics.md#int32) 值。

示例：

<!-- verify -->
```cangjie
main() {
    var num: Int32 = 3
    println(num.next(10))
}
```

运行结果：

```text
13
```

### func position()

```cangjie
public func position(): Int64
```

功能：获取当前 [Int32](core_package_intrinsics.md#int32) 值的位置信息，即将该 [Int32](core_package_intrinsics.md#int32) 转换为 [Int64](core_package_intrinsics.md#int64) 值。

返回值：

- [Int64](core_package_intrinsics.md#int64) - 当前 [Int32](core_package_intrinsics.md#int32) 值的位置信息。

示例：

<!-- verify -->
```cangjie
main() {
    var num: Int32 = 3
    println(num.position())
}
```

运行结果：

```text
3
```

## extend Int32 <: Hashable

```cangjie
extend Int32 <: Hashable
```

功能：为 [Int32](core_package_intrinsics.md#int32) 类型扩展 [Hashable](core_package_interfaces.md#interface-hashable) 接口，支持计算哈希值，并拓展该类型以支持一些数学常数。

被扩展类型：

- [Int32](core_package_intrinsics.md#int32)

扩展接口：

- [Hashable](core_package_interfaces.md#interface-hashable)

### static prop Max

```cangjie
public static prop Max: Int32
```

功能：获取 32 位有符号整数的最大值。

类型：[Int32](./core_package_intrinsics.md#int32)

示例：

<!-- verify -->
```cangjie
main() {
    // 测试 Int32.Max 属性
    let maxValue = Int32.Max
    println("Int32.Max 的值: ${maxValue}")

    // 验证它确实是最大值
    println("Int32.Max 是否等于 2147483647: ${maxValue == 2147483647}")
}
```

运行结果：

```text
Int32.Max 的值: 2147483647
Int32.Max 是否等于 2147483647: true
```

### static prop Min

```cangjie
public static prop Min: Int32
```

功能：获取 32 位有符号整数的最小值。

类型：[Int32](./core_package_intrinsics.md#int32)

示例：

<!-- verify -->
```cangjie
main() {
    // 测试 Int32.Min 属性
    let minValue = Int32.Min
    println("Int32.Min 的值: ${minValue}")

    // 验证它确实是最小值
    println("Int32.Min 是否等于 -2147483648: ${minValue == -2147483648}")
}
```

运行结果：

```text
Int32.Min 的值: -2147483648
Int32.Min 是否等于 -2147483648: true
```

### func hashCode()

```cangjie
public func hashCode(): Int64
```

功能：获取哈希值。

返回值：

- [Int64](core_package_intrinsics.md#int64) - 哈希值。

示例：

<!-- verify -->
```cangjie
main() {
    // 测试 Int32.hashCode 方法
    let value1: Int32 = 42
    let value2: Int32 = 42
    let value3: Int32 = -100

    // 计算哈希值
    let hash1 = value1.hashCode()
    let hash2 = value2.hashCode()
    let hash3 = value3.hashCode()

    println("42 的哈希值: ${hash1}")
    println("42 的哈希值: ${hash2}")
    println("-100 的哈希值: ${hash3}")

    // 验证相等的值具有相同的哈希值
    println("相等值的哈希值是否相同: ${hash1 == hash2}")
}
```

运行结果：

```text
42 的哈希值: 42
42 的哈希值: 42
-100 的哈希值: -100
相等值的哈希值是否相同: true
```

## extend Int32 <: ToString

```cangjie
extend Int32 <: ToString
```

功能：这里为 [Int32](core_package_intrinsics.md#int32) 类型扩展 [ToString](core_package_interfaces.md#interface-tostring) 接口，实现向 [String](core_package_structs.md#struct-string) 类型的转换。

被扩展类型：

- [Int32](core_package_intrinsics.md#int32)

扩展接口：

- [ToString](core_package_interfaces.md#interface-tostring)

### func toString()

```cangjie
public func toString(): String
```

功能：将 [Int32](core_package_intrinsics.md#int32) 值转换为字符串。

返回值：

- [String](core_package_structs.md#struct-string) - 转化后的字符串。

示例：

<!-- verify -->
```cangjie
main() {
    // 测试 Int32.toString 方法
    let value1: Int32 = 42
    let value2: Int32 = -123
    let value3: Int32 = 0

    // 转换为字符串
    let str1 = value1.toString()
    let str2 = value2.toString()
    let str3 = value3.toString()

    println("42 转换为字符串: \"${str1}\"")
    println("-123 转换为字符串: \"${str2}\"")
    println("0 转换为字符串: \"${str3}\"")

    // 验证类型
    println("转换后的类型是否为 String: ${str1 is String}")
}
```

运行结果：

```text
42 转换为字符串: "42"
-123 转换为字符串: "-123"
0 转换为字符串: "0"
转换后的类型是否为 String: true
```

## extend Int64

```cangjie
extend Int64
```

功能：拓展了 [Int64](../../core/core_package_api/core_package_intrinsics.md#int64) 类型作为左操作数和 [Duration](../../core/core_package_api/core_package_structs.md#struct-duration) 类型作为右操作数的乘法运算。

被扩展类型：

- [Int64](../../core/core_package_api/core_package_intrinsics.md#int64)

### operator func *(Duration)

```cangjie
public operator func *(r: Duration): Duration
```

功能：实现 [Int64](../../core/core_package_api/core_package_intrinsics.md#int64) 类型和 [Duration](../../core/core_package_api/core_package_structs.md#struct-duration) 类型的乘法，即 [Int64](../../core/core_package_api/core_package_intrinsics.md#int64) * [Duration](../../core/core_package_api/core_package_structs.md#struct-duration) 运算。

例如 2 * [Duration](../../core/core_package_api/core_package_structs.md#struct-duration).second 返回表示时间间隔为 2 秒的 [Duration](../../core/core_package_api/core_package_structs.md#struct-duration) 实例。

参数：

- r: [Duration](../../core/core_package_api/core_package_structs.md#struct-duration) - 乘法的右操作数。

返回值：

- [Duration](../../core/core_package_api/core_package_structs.md#struct-duration) - [Int64](../../core/core_package_api/core_package_intrinsics.md#int64) 类型实例和 `r` 的乘积。

异常：

- [ArithmeticException](../../core/core_package_api/core_package_exceptions.md#class-arithmeticexception) - 当相乘后的结果超出 [Duration](../../core/core_package_api/core_package_structs.md#struct-duration) 的表示范围时，抛出异常。

示例：

<!-- verify -->
```cangjie
main() {
    // 测试 Int64 * Duration 运算
    let intVal: Int64 = 3
    let duration: Duration = Duration.second

    let result = intVal * duration

    println("Int64值: ${intVal}")
    println("Duration值: ${duration}")
    println("运算结果: ${result}")

    // 测试 2 * Duration.second 的示例
    let result2 = 2 * Duration.second
    println("2 * Duration.second = ${result2}")
}
```

运行结果：

```text
Int64值: 3
Duration值: 1s
运算结果: 3s
2 * Duration.second = 2s
```

## extend Int64 <: Comparable\<Int64>

```cangjie
extend Int64 <: Comparable<Int64>
```

功能：为 [Int64](core_package_intrinsics.md#int64) 类型扩展 [Comparable](core_package_interfaces.md#interface-comparablet)\<[Int64](core_package_intrinsics.md#int64)> 接口，支持比较操作。

被扩展类型：

- [Int64](core_package_intrinsics.md#int64)

扩展接口：

- [Comparable](core_package_interfaces.md#interface-comparablet)\<[Int64](core_package_intrinsics.md#int64)>

### func compare(Int64)

```cangjie
public func compare(other: Int64): Ordering
```

功能：判断当前 [Int64](core_package_intrinsics.md#int64) 值与指定 [Int64](core_package_intrinsics.md#int64) 值的大小关系。

参数：

- other: [Int64](core_package_intrinsics.md#int64) - 待比较的另一个 [Int64](core_package_intrinsics.md#int64) 值。

返回值：

- [Ordering](core_package_enums.md#enum-ordering) - 如果大于，返回 [Ordering](core_package_enums.md#enum-ordering).GT；如果等于，返回 [Ordering](core_package_enums.md#enum-ordering).EQ；如果小于，返回 [Ordering](core_package_enums.md#enum-ordering).LT。

示例：

<!-- verify -->
```cangjie
main() {
    var num1: Int64 = 2
    var num2: Int64 = 3
    println(num1.compare(num2))
}
```

运行结果：

```text
Ordering.LT
```

## extend Int64 <: Countable\<Int64>

```cangjie
extend Int64 <: Countable<Int64>
```

功能：为 [Int64](core_package_intrinsics.md#int64) 类型扩展 [Countable](core_package_interfaces.md#interface-countablet)\<[Int64](core_package_intrinsics.md#int64)> 接口，支持计数操作。

被扩展类型：

- [Int64](core_package_intrinsics.md#int64)

扩展接口：

- [Countable](core_package_interfaces.md#interface-countablet)\<[Int64](core_package_intrinsics.md#int64)>

### func next(Int64)

```cangjie
public func next(right: Int64): Int64
```

功能：获取在数轴上当前 [Int64](core_package_intrinsics.md#int32) 位置往右移动 `right` 后对应位置的 [Int64](core_package_intrinsics.md#int32) 值。如果值溢出，则会从数轴最左边继续移动。

参数：

- right: [Int64](core_package_intrinsics.md#int64) - 往右数的个数。

返回值：

- [Int64](core_package_intrinsics.md#int64) - 往右数 `right` 后所到位置的 [Int64](core_package_intrinsics.md#int64) 值。

示例：

<!-- verify -->
```cangjie
main() {
    var num: Int64 = 3
    println(num.next(10))
}
```

运行结果：

```text
13
```

### func position()

```cangjie
public func position(): Int64
```

功能：获取当前 [Int64](core_package_intrinsics.md#int64) 值的位置信息，即返回该 [Int64](core_package_intrinsics.md#int64) 值本身。

返回值：

- [Int64](core_package_intrinsics.md#int64) - 当前 [Int64](core_package_intrinsics.md#int64) 值的位置信息。

示例：

<!-- verify -->
```cangjie
main() {
    var num: Int64 = 3
    println(num.position())
}
```

运行结果：

```text
3
```

## extend Int64 <: Hashable

```cangjie
extend Int64 <: Hashable
```

功能：为 [Int64](core_package_intrinsics.md#int64) 类型扩展 [Hashable](core_package_interfaces.md#interface-hashable) 接口，支持计算哈希值，并拓展该类型以支持一些数学常数。

被扩展类型：

- [Int64](core_package_intrinsics.md#int64)

扩展接口：

- [Hashable](core_package_interfaces.md#interface-hashable)

### static prop Max

```cangjie
public static prop Max: Int64
```

功能：获取 64 位有符号整数的最大值。

类型：[Int64](./core_package_intrinsics.md#int64)

示例：

<!-- verify -->
```cangjie
main() {
    // 测试 Int64.Max 属性
    let maxValue = Int64.Max
    println("Int64.Max 的值: ${maxValue}")

    // 验证它确实是最大值
    println("Int64.Max 是否等于 9223372036854775807: ${maxValue == 9223372036854775807}")
}
```

运行结果：

```text
Int64.Max 的值: 9223372036854775807
Int64.Max 是否等于 9223372036854775807: true
```

### static prop Min

```cangjie
public static prop Min: Int64
```

功能：获取 64 位有符号整数的最小值。

类型：[Int64](./core_package_intrinsics.md#int64)

示例：

<!-- verify -->
```cangjie
main() {
    // 测试 Int64.Min 属性
    let minValue = Int64.Min
    println("Int64.Min 的值: ${minValue}")

    // 验证它确实是最小值
    println("Int64.Min 是否等于 -9223372036854775808: ${minValue == -9223372036854775808}")
}
```

运行结果：

```text
Int64.Min 的值: -9223372036854775808
Int64.Min 是否等于 -9223372036854775808: true
```

### func hashCode()

```cangjie
public func hashCode(): Int64
```

功能：获取哈希值。

返回值：

- [Int64](core_package_intrinsics.md#int64) - 哈希值。

示例：

<!-- verify -->
```cangjie
main() {
    // 测试 Int64.hashCode() 方法
    let num: Int64 = 42
    let hash = num.hashCode()
    println("Int64 值: ${num}")
    println("hashCode: ${hash}")

    // 测试不同值的哈希码
    let negativeNum: Int64 = -42
    let negativeHash = negativeNum.hashCode()
    println("Int64 值: ${negativeNum}")
    println("hashCode: ${negativeHash}")

    // 测试零值
    let zero: Int64 = 0
    let zeroHash = zero.hashCode()
    println("Int64 值: ${zero}")
    println("hashCode: ${zeroHash}")
}
```

运行结果：

```text
Int64 值: 42
hashCode: 42
Int64 值: -42
hashCode: -42
Int64 值: 0
hashCode: 0
```

## extend Int64 <: ToString

```cangjie
extend Int64 <: ToString
```

功能：这里为 [Int64](core_package_intrinsics.md#int64) 类型扩展 [ToString](core_package_interfaces.md#interface-tostring) 接口，实现向 [String](core_package_structs.md#struct-string) 类型的转换。

被扩展类型：

- [Int64](core_package_intrinsics.md#int64)

扩展接口：

- [ToString](core_package_interfaces.md#interface-tostring)

### func toString()

```cangjie
public func toString(): String
```

功能：将 [Int64](core_package_intrinsics.md#int64) 值转换为字符串。

返回值：

- [String](core_package_structs.md#struct-string) - 转化后的字符串。

示例：

<!-- verify -->
```cangjie
main() {
    // 测试 Int64.toString() 方法
    let num: Int64 = 42
    let str = num.toString()
    println("Int64 值: ${num}")
    println("转换为字符串: ${str}")

    // 测试负数
    let negativeNum: Int64 = -42
    let negativeStr = negativeNum.toString()
    println("Int64 值: ${negativeNum}")
    println("转换为字符串: ${negativeStr}")

    // 测试零值
    let zero: Int64 = 0
    let zeroStr = zero.toString()
    println("Int64 值: ${zero}")
    println("转换为字符串: ${zeroStr}")

    // 测试最大值和最小值
    let maxVal = Int64.Max
    let maxStr = maxVal.toString()
    println("Int64 最大值: ${maxVal}")
    println("转换为字符串: ${maxStr}")

    let minVal = Int64.Min
    let minStr = minVal.toString()
    println("Int64 最小值: ${minVal}")
    println("转换为字符串: ${minStr}")
}
```

运行结果：

```text
Int64 值: 42
转换为字符串: 42
Int64 值: -42
转换为字符串: -42
Int64 值: 0
转换为字符串: 0
Int64 最大值: 9223372036854775807
转换为字符串: 9223372036854775807
Int64 最小值: -9223372036854775808
转换为字符串: -9223372036854775808
```

## extend Int8 <: Comparable\<Int8>

```cangjie
extend Int8 <: Comparable<Int8>
```

功能：为 [Int8](core_package_intrinsics.md#int8) 类型扩展 [Comparable](core_package_interfaces.md#interface-comparablet)\<[Int8](core_package_intrinsics.md#int8)> 接口，支持比较操作。

被扩展类型：

- [Int8](core_package_intrinsics.md#int8)

扩展接口：

- [Comparable](core_package_interfaces.md#interface-comparablet)\<[Int8](core_package_intrinsics.md#int8)>

### func compare(Int8)

```cangjie
public func compare(other: Int8): Ordering
```

功能：判断当前 [Int8](core_package_intrinsics.md#int8) 值与指定 [Int8](core_package_intrinsics.md#int8) 值的大小关系。

参数：

- other: [Int8](core_package_intrinsics.md#int8) - 待比较的另一个 [Int8](core_package_intrinsics.md#int8) 值。

返回值：

- [Ordering](core_package_enums.md#enum-ordering) - 如果大于，返回 [Ordering](core_package_enums.md#enum-ordering).GT；如果等于，返回 [Ordering](core_package_enums.md#enum-ordering).EQ；如果小于，返回 [Ordering](core_package_enums.md#enum-ordering).LT。

示例：

<!-- verify -->
```cangjie
main() {
    var num1: Int8 = 2
    var num2: Int8 = 3
    println(num1.compare(num2))
}
```

运行结果：

```text
Ordering.LT
```

## extend Int8 <: Countable\<Int8>

```cangjie
extend Int8 <: Countable<Int8>
```

功能：为 [Int8](core_package_intrinsics.md#int8) 类型扩展 [Countable](core_package_interfaces.md#interface-countablet)\<[Int8](core_package_intrinsics.md#int8)> 接口，支持计数操作。

被扩展类型：

- [Int8](core_package_intrinsics.md#int8)

扩展接口：

- [Countable](core_package_interfaces.md#interface-countablet)\<[Int8](core_package_intrinsics.md#int8)>

### func next(Int64)

```cangjie
public func next(right: Int64): Int8
```

功能：获取在数轴上当前 [Int8](core_package_intrinsics.md#int32) 位置往右移动 `right` 后对应位置的 [Int8](core_package_intrinsics.md#int32) 值。如果值溢出，则会从数轴最左边继续移动。

参数：

- right: [Int64](core_package_intrinsics.md#int64) - 往右数的个数。

返回值：

- [Int8](core_package_intrinsics.md#int8) - 往右数 `right` 后所到位置的 [Int8](core_package_intrinsics.md#int8) 值。

示例：

<!-- verify -->
```cangjie
main() {
    var num: Int8 = 3
    println(num.next(5))
}
```

运行结果：

```text
8
```

### func position()

```cangjie
public func position(): Int64
```

功能：获取当前 [Int8](core_package_intrinsics.md#int8) 值的位置信息，即将该 [Int8](core_package_intrinsics.md#int8) 转换为 [Int64](core_package_intrinsics.md#int64) 值。

返回值：

- [Int64](core_package_intrinsics.md#int64) - 当前 [Int8](core_package_intrinsics.md#int8) 值的位置信息。

示例：

<!-- verify -->
```cangjie
main() {
    var num: Int8 = 3
    println(num.position())
}
```

运行结果：

```text
3
```

## extend Int8 <: Hashable

```cangjie
extend Int8 <: Hashable
```

功能：为 [Int8](core_package_intrinsics.md#int8) 类型扩展 [Hashable](core_package_interfaces.md#interface-hashable) 接口，支持计算哈希值，并拓展该类型以支持一些数学常数。

被扩展类型：

- [Int8](core_package_intrinsics.md#int8)

扩展接口：

- [Hashable](core_package_interfaces.md#interface-hashable)

### static prop Max

```cangjie
public static prop Max: Int8
```

功能：获取 8 位有符号整数的最大值。

类型：[Int8](./core_package_intrinsics.md#int8)

示例：

<!-- verify -->
```cangjie
main() {
    // 测试 Int8.Max 属性
    let maxValue = Int8.Max
    println("Int8.Max 的值: ${maxValue}")

    // 验证它确实是最大值
    println("Int8.Max 是否等于 127: ${maxValue == 127}")
}
```

运行结果：

```text
Int8.Max 的值: 127
Int8.Max 是否等于 127: true
```

### static prop Min

```cangjie
public static prop Min: Int8
```

功能：获取 8 位有符号整数的最小值。

类型：[Int8](./core_package_intrinsics.md#int8)

示例：

<!-- verify -->
```cangjie
main() {
    // 测试 Int8.Min 属性
    let minValue = Int8.Min
    println("Int8.Min 的值: ${minValue}")

    // 验证它确实是最小值
    println("Int8.Min 是否等于 -128: ${minValue == -128}")
}
```

运行结果：

```text
Int8.Min 的值: -128
Int8.Min 是否等于 -128: true
```

### func hashCode()

```cangjie
public func hashCode(): Int64
```

功能：获取哈希值。

返回值：

- [Int64](core_package_intrinsics.md#int64) - 哈希值。

示例：

<!-- verify -->
```cangjie
main() {
    // 测试 Int8.hashCode() 方法
    let num: Int8 = 42
    let hash = num.hashCode()
    println("Int8 值: ${num}")
    println("hashCode: ${hash}")

    // 测试不同值的哈希码
    let negativeNum: Int8 = -42
    let negativeHash = negativeNum.hashCode()
    println("Int8 值: ${negativeNum}")
    println("hashCode: ${negativeHash}")

    // 测试零值
    let zero: Int8 = 0
    let zeroHash = zero.hashCode()
    println("Int8 值: ${zero}")
    println("hashCode: ${zeroHash}")

    // 测试最大值和最小值
    let maxVal = Int8.Max
    let maxHash = maxVal.hashCode()
    println("Int8 最大值: ${maxVal}")
    println("hashCode: ${maxHash}")

    let minVal = Int8.Min
    let minHash = minVal.hashCode()
    println("Int8 最小值: ${minVal}")
    println("hashCode: ${minHash}")
}
```

运行结果：

```text
Int8 值: 42
hashCode: 42
Int8 值: -42
hashCode: -42
Int8 值: 0
hashCode: 0
Int8 最大值: 127
hashCode: 127
Int8 最小值: -128
hashCode: -128
```

## extend Int8 <: ToString

```cangjie
extend Int8 <: ToString
```

功能：这里为 [Int8](core_package_intrinsics.md#int8) 类型扩展 [ToString](core_package_interfaces.md#interface-tostring) 接口，实现向 [String](core_package_structs.md#struct-string) 类型的转换。

被扩展类型：

- [Int8](core_package_intrinsics.md#int8)

扩展接口：

- [ToString](core_package_interfaces.md#interface-tostring)

### func toString()

```cangjie
public func toString(): String
```

功能：将 [Int8](core_package_intrinsics.md#int8) 值转换为字符串。

返回值：

- [String](core_package_structs.md#struct-string) - 转化后的字符串。

示例：

<!-- verify -->
```cangjie
main() {
    // 测试 Int8.toString() 方法
    let num: Int8 = 42
    let str = num.toString()
    println("Int8 值: ${num}")
    println("转换为字符串: ${str}")

    // 测试负数
    let negativeNum: Int8 = -42
    let negativeStr = negativeNum.toString()
    println("Int8 值: ${negativeNum}")
    println("转换为字符串: ${negativeStr}")

    // 测试零值
    let zero: Int8 = 0
    let zeroStr = zero.toString()
    println("Int8 值: ${zero}")
    println("转换为字符串: ${zeroStr}")

    // 测试最大值和最小值
    let maxVal = Int8.Max
    let maxStr = maxVal.toString()
    println("Int8 最大值: ${maxVal}")
    println("转换为字符串: ${maxStr}")

    let minVal = Int8.Min
    let minStr = minVal.toString()
    println("Int8 最小值: ${minVal}")
    println("转换为字符串: ${minStr}")
}
```

运行结果：

```text
Int8 值: 42
转换为字符串: 42
Int8 值: -42
转换为字符串: -42
Int8 值: 0
转换为字符串: 0
Int8 最大值: 127
转换为字符串: 127
Int8 最小值: -128
转换为字符串: -128
```

## extend IntNative <: Comparable\<IntNative>

```cangjie
extend IntNative <: Comparable<IntNative>
```

功能：为 [IntNative](core_package_intrinsics.md#intnative) 类型扩展 [Comparable](core_package_interfaces.md#interface-comparablet)\<[IntNative](core_package_intrinsics.md#intnative)> 接口，支持比较操作。

被扩展类型：

- [IntNative](core_package_intrinsics.md#intnative)

扩展接口：

- [Comparable](core_package_interfaces.md#interface-comparablet)\<[IntNative](core_package_intrinsics.md#intnative)>

### func compare(IntNative)

```cangjie
public func compare(other: IntNative): Ordering
```

功能：判断当前 [IntNative](core_package_intrinsics.md#intnative) 值与指定 [IntNative](core_package_intrinsics.md#intnative) 值的大小关系。

参数：

- other: [IntNative](core_package_intrinsics.md#intnative) - 待比较的另一个 [IntNative](core_package_intrinsics.md#intnative) 值。

返回值：

- [Ordering](core_package_enums.md#enum-ordering) - 如果大于，返回 [Ordering](core_package_enums.md#enum-ordering).GT；如果等于，返回 [Ordering](core_package_enums.md#enum-ordering).EQ；如果小于，返回 [Ordering](core_package_enums.md#enum-ordering).LT。

示例：

<!-- verify -->
```cangjie
main() {
    var num1: IntNative = 8
    var num2: IntNative = 10
    println(num1.compare(num2))
}
```

运行结果：

```text
Ordering.LT
```

## extend IntNative <: Countable\<IntNative>

```cangjie
extend IntNative <: Countable<IntNative>
```

功能：为 [IntNative](core_package_intrinsics.md#intnative) 类型扩展 [Countable](core_package_interfaces.md#interface-countablet)\<[IntNative](core_package_intrinsics.md#intnative)> 接口，支持计数操作。

被扩展类型：

- [IntNative](core_package_intrinsics.md#intnative)

扩展接口：

- [Countable](core_package_interfaces.md#interface-countablet)\<[IntNative](core_package_intrinsics.md#intnative)>

### func next(Int64)

```cangjie
public func next(right: Int64): IntNative
```

功能：获取在数轴上当前 [IntNative](core_package_intrinsics.md#int32) 位置往右移动 `right` 后对应位置的 [IntNative](core_package_intrinsics.md#int32) 值。如果值溢出，则会从数轴最左边继续移动。

参数：

- right: [Int64](core_package_intrinsics.md#int64) - 往右数的个数。

返回值：

- [IntNative](core_package_intrinsics.md#intnative) - 往右数 `right` 后所到位置的 [IntNative](core_package_intrinsics.md#intnative) 值。

示例：

<!-- verify -->
```cangjie
main() {
    var num: IntNative = 8
    println(num.next(4))
}
```

运行结果：

```text
12
```

### func position()

```cangjie
public func position(): Int64
```

功能：获取当前 [IntNative](core_package_intrinsics.md#intnative) 值的位置信息，即将该 [IntNative](core_package_intrinsics.md#intnative) 转换为 [Int64](core_package_intrinsics.md#int64) 值。

返回值：

- [Int64](core_package_intrinsics.md#int64) - 当前 [IntNative](core_package_intrinsics.md#intnative) 值的位置信息。

示例：

<!-- verify -->
```cangjie
main() {
    var num: IntNative = 8
    println(num.position())
}
```

运行结果：

```text
8
```

## extend IntNative <: Hashable

```cangjie
extend IntNative <: Hashable
```

功能：为 [IntNative](core_package_intrinsics.md#intnative) 类型扩展 [Hashable](core_package_interfaces.md#interface-hashable) 接口，支持计算哈希值，并拓展该类型以支持一些数学常数。

被扩展类型：

- [IntNative](core_package_intrinsics.md#intnative)

扩展接口：

- [Hashable](core_package_interfaces.md#interface-hashable)

### static prop Max

```cangjie
public static prop Max: IntNative
```

功能：获取平台相关有符号整数的最大值。

类型：[IntNative](./core_package_intrinsics.md#intnative)

示例：

<!-- verify -->
```cangjie
main() {
    // 测试 IntNative.Max 属性
    let maxValue = IntNative.Max
    println("IntNative.Max 的值: ${maxValue}")

    // 验证它确实是最大值（在64位系统上应该是 9223372036854775807）
    println("IntNative.Max 是否等于 9223372036854775807: ${maxValue == 9223372036854775807}")
}
```

运行结果：

```text
IntNative.Max 的值: 9223372036854775807
IntNative.Max 是否等于 9223372036854775807: true
```

### static prop Min

```cangjie
public static prop Min: IntNative
```

功能：获取平台相关有符号整数的最小值。

类型：[IntNative](./core_package_intrinsics.md#intnative)

示例：

<!-- verify -->
```cangjie
main() {
    // 测试 IntNative.Min 属性
    let minValue = IntNative.Min
    println("IntNative.Min 的值: ${minValue}")

    // 验证它确实是最小值（在64位系统上应该是 -9223372036854775808）
    println("IntNative.Min 是否等于 -9223372036854775808: ${minValue == -9223372036854775808}")
}
```

运行结果：

```text
IntNative.Min 的值: -9223372036854775808
IntNative.Min 是否等于 -9223372036854775808: true
```

### func hashCode()

```cangjie
public func hashCode(): Int64
```

功能：获取哈希值。

返回值：

- [Int64](core_package_intrinsics.md#int64) - 哈希值。

示例：

<!-- verify -->
```cangjie
main() {
    // 测试 IntNative.hashCode() 方法
    let num: IntNative = 42
    let hash = num.hashCode()
    println("IntNative 值: ${num}")
    println("hashCode: ${hash}")

    // 测试不同值的哈希码
    let negativeNum: IntNative = -42
    let negativeHash = negativeNum.hashCode()
    println("IntNative 值: ${negativeNum}")
    println("hashCode: ${negativeHash}")

    // 测试零值
    let zero: IntNative = 0
    let zeroHash = zero.hashCode()
    println("IntNative 值: ${zero}")
    println("hashCode: ${zeroHash}")

    // 测试最大值和最小值
    let maxVal = IntNative.Max
    let maxHash = maxVal.hashCode()
    println("IntNative 最大值: ${maxVal}")
    println("hashCode: ${maxHash}")

    let minVal = IntNative.Min
    let minHash = minVal.hashCode()
    println("IntNative 最小值: ${minVal}")
    println("hashCode: ${minHash}")
}
```

运行结果：

```text
IntNative 值: 42
hashCode: 42
IntNative 值: -42
hashCode: -42
IntNative 值: 0
hashCode: 0
IntNative 最大值: 9223372036854775807
hashCode: 9223372036854775807
IntNative 最小值: -9223372036854775808
hashCode: -9223372036854775808
```

## extend IntNative <: ToString

```cangjie
extend IntNative <: ToString
```

功能：这里为 [IntNative](core_package_intrinsics.md#intnative) 类型扩展 [ToString](core_package_interfaces.md#interface-tostring) 接口，实现向 [String](core_package_structs.md#struct-string) 类型的转换。

被扩展类型：

- [IntNative](core_package_intrinsics.md#intnative)

扩展接口：

- [ToString](core_package_interfaces.md#interface-tostring)

### func toString()

```cangjie
public func toString(): String
```

功能：将 [IntNative](core_package_intrinsics.md#intnative) 值转换为字符串。

返回值：

- [String](core_package_structs.md#struct-string) - 转化后的字符串。

示例：

<!-- verify -->
```cangjie
main() {
    // 测试 IntNative.toString() 方法
    let num: IntNative = 42
    let str = num.toString()
    println("IntNative 值: ${num}")
    println("转换为字符串: ${str}")

    // 测试负数
    let negativeNum: IntNative = -42
    let negativeStr = negativeNum.toString()
    println("IntNative 值: ${negativeNum}")
    println("转换为字符串: ${negativeStr}")

    // 测试零值
    let zero: IntNative = 0
    let zeroStr = zero.toString()
    println("IntNative 值: ${zero}")
    println("转换为字符串: ${zeroStr}")

    // 测试最大值和最小值
    let maxVal = IntNative.Max
    let maxStr = maxVal.toString()
    println("IntNative 最大值: ${maxVal}")
    println("转换为字符串: ${maxStr}")

    let minVal = IntNative.Min
    let minStr = minVal.toString()
    println("IntNative 最小值: ${minVal}")
    println("转换为字符串: ${minStr}")
}
```

运行结果：

```text
IntNative 值: 42
转换为字符串: 42
IntNative 值: -42
转换为字符串: -42
IntNative 值: 0
转换为字符串: 0
IntNative 最大值: 9223372036854775807
转换为字符串: 9223372036854775807
IntNative 最小值: -9223372036854775808
转换为字符串: -9223372036854775808
```

## extend Ordering <: Comparable

```cangjie
extend Ordering <: Comparable<Ordering>
```

功能：为 [Ordering](core_package_enums.md#enum-ordering) 类型扩展 [Comparable](core_package_interfaces.md#interface-comparablet)\<[Ordering](core_package_enums.md#enum-ordering)> 接口，支持比较操作。

被扩展类型：

- [Ordering](core_package_enums.md#enum-ordering)

扩展接口：

- [Comparable](core_package_interfaces.md#interface-comparablet)\<[Ordering](core_package_enums.md#enum-ordering)>

### func compare(Ordering)

```cangjie
public func compare(other: Ordering): Ordering
```

功能：判断当前 [Ordering](core_package_enums.md#enum-ordering) 实例与参数指定的 [Ordering](core_package_enums.md#enum-ordering) 实例的大小关系。

[Ordering](core_package_enums.md#enum-ordering) 枚举的大小关系为：GT > EQ > LT。

参数：

- other: [Ordering](core_package_enums.md#enum-ordering) - 待比较的 [Ordering](core_package_enums.md#enum-ordering) 实例。

返回值：

- [Ordering](core_package_enums.md#enum-ordering) - 如果大于，返回 GT；如果等于，返回 EQ；如果小于，返回 LT。

示例：

<!-- verify -->
```cangjie
main() {
    // 创建Ordering实例
    var gt: Ordering = GT
    var eq: Ordering = EQ
    var lt: Ordering = LT

    // 测试GT与EQ的比较
    var result1 = gt.compare(eq)
    println("GT.compare(EQ): ${result1}")

    // 测试EQ与LT的比较
    var result2 = eq.compare(lt)
    println("EQ.compare(LT): ${result2}")

    // 测试LT与GT的比较
    var result3 = lt.compare(gt)
    println("LT.compare(GT): ${result3}")

    // 测试相等的情况
    var result4 = gt.compare(gt)
    println("GT.compare(GT): ${result4}")
}
```

运行结果：

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

功能：为 [Ordering](core_package_enums.md#enum-ordering) 类型扩展 [Hashable](core_package_interfaces.md#interface-hashable) 接口，支持计算哈希值。

被扩展类型：

- [Ordering](core_package_enums.md#enum-ordering)

扩展接口：

- [Hashable](core_package_interfaces.md#interface-hashable)

### func hashCode()

```cangjie
public func hashCode(): Int64
```

功能：获取哈希值，GT 的哈希值是 3，EQ 的哈希值是 2，LT 的哈希值是 1。

返回值：

- [Int64](core_package_intrinsics.md#int64) - 哈希值。

示例：

<!-- verify -->
```cangjie
main() {
    // 创建Ordering实例
    var gt: Ordering = GT
    var eq: Ordering = EQ
    var lt: Ordering = LT

    // 获取哈希值
    var gtHash = gt.hashCode()
    var eqHash = eq.hashCode()
    var ltHash = lt.hashCode()

    println("GT的哈希值: ${gtHash}")
    println("EQ的哈希值: ${eqHash}")
    println("LT的哈希值: ${ltHash}")
}
```

运行结果：

```text
GT的哈希值: 3
EQ的哈希值: 2
LT的哈希值: 1
```

## extend Ordering <: ToString

```cangjie
extend Ordering <: ToString
```

功能：为 [Ordering](core_package_enums.md#enum-ordering) 类型扩展 [ToString](core_package_interfaces.md#interface-tostring) 接口，支持转字符串操作。

被扩展类型：

- [Ordering](core_package_enums.md#enum-ordering)

扩展接口：

- [ToString](core_package_interfaces.md#interface-tostring)

### func toString()

```cangjie
public func toString(): String
```

功能：将 [Ordering](core_package_enums.md#enum-ordering) 转换为字符串。

转换结果如下：

- GT: "[Ordering](core_package_enums.md#enum-ordering).GT"。
- LT: "[Ordering](core_package_enums.md#enum-ordering).LT"。
- EQ: "[Ordering](core_package_enums.md#enum-ordering).EQ"。

返回值：

- [String](core_package_structs.md#struct-string) - 转化后的字符串。

示例：

<!-- verify -->
```cangjie
main() {
    // 创建Ordering实例
    var gt: Ordering = GT
    var eq: Ordering = EQ
    var lt: Ordering = LT

    // 转换为字符串
    var gtStr = gt.toString()
    var eqStr = eq.toString()
    var ltStr = lt.toString()

    println("GT转换为字符串: ${gtStr}")
    println("EQ转换为字符串: ${eqStr}")
    println("LT转换为字符串: ${ltStr}")
}
```

运行结果：

```text
GT转换为字符串: Ordering.GT
EQ转换为字符串: Ordering.EQ
LT转换为字符串: Ordering.LT
```

## extend Rune

```cangjie
extend Rune
```

功能：为 [Rune](core_package_intrinsics.md#rune) 类型实现一系列扩展方法，主要为在 Ascii 字符集范围内的一些字符判断、转换等操作。

被扩展类型：

- [Rune](core_package_intrinsics.md#rune)

### static func fromUtf8(Array\<UInt8>, Int64)

```cangjie
public static func fromUtf8(arr: Array<UInt8>, index: Int64): (Rune, Int64)
```

功能：将字节数组中的指定元素，根据 UTF-8 编码规则转换成字符，并告知字符占用字节长度。

参数：

- arr: [Array](core_package_structs.md#struct-arrayt)\<[UInt8](core_package_intrinsics.md#uint8)> - 待转换字节所在的字节数组。
- index: [Int64](core_package_intrinsics.md#int64) - 待转换字节在数组中的下标。

返回值：

- ([Rune](core_package_intrinsics.md#rune), [Int64](core_package_intrinsics.md#int64)) - 转换得到的字符，以及该字符占用的字节长度。

异常：

- [IllegalArgumentException](core_package_exceptions.md#class-illegalargumentexception) - 不合法的 UTF-8 字节序列。

示例：

<!-- verify -->
```cangjie
main() {
    var arr: Array<UInt8> = [4, 8, 65] // A <=> 65
    var tuple = Rune.fromUtf8(arr, 2)
    println(tuple[0]) // Rune
    println(tuple[1]) // len
}
```

运行结果：

```text
A
1
```

### static func getPreviousFromUtf8(Array\<UInt8>, Int64)

```cangjie
public static func getPreviousFromUtf8(arr: Array<UInt8>, index: Int64): (Rune, Int64)
```

功能：获取字节数组中索引 `index` 前一个字节（即 `arr[index - 1]`）所在的 UTF-8 编码字符，同时返回该字符字节的长度。

函数从 `arr[index - 1]` 开始，根据 UTF-8 规则判断该字节是否为字符的首字节码，如果不是则继续向前遍历，直到找到首字节码，然后利用字节码序列解码对应的字符。参数 `index` 的有效取值范围为 `[1, arr.size]`。

参数：

- arr: [Array](core_package_structs.md#struct-arrayt)\<[UInt8](core_package_intrinsics.md#uint8)> - 待从中获取字符的字节数组。
- index: [Int64](core_package_intrinsics.md#int64) - 字节数组中的索引，函数将查找 `arr[index - 1]` 所在的字符，取值范围为 `[1, arr.size]`。

返回值：

- ([Rune](core_package_intrinsics.md#rune), [Int64](core_package_intrinsics.md#int64)) - 找到的字符，以及该字符字节码的长度。

异常：

- [IllegalArgumentException](core_package_exceptions.md#class-illegalargumentexception) - 如果 `index` 小于等于 0，不存在前一个字节，抛出异常；如果找不到对应首字节码，即 `arr[index - 1]` 所在位置的字节不符合 UTF-8 编码，抛出异常。
- [IndexOutOfBoundsException](core_package_exceptions.md#class-indexoutofboundsexception) - 如果 `index` 大于 `arr.size`，抛出异常。

示例：

<!-- verify -->
```cangjie
main() {
    // 创建一个包含中文字符的字节数组
    var arr: Array<UInt8> = [72, 101, 108, 108, 111, 231, 136, 177] // "Hello爱" 的 UTF-8 编码
    println("字节数组: ${arr}")

    // 获取索引为 7（即中文字符"爱"的第二个字节）对应的字符
    var tuple = Rune.getPreviousFromUtf8(arr, 7)
    println("获取的字符: ${tuple[0]}")
    println("字符的字节长度: ${tuple[1]}")
}
```

运行结果：

```text
字节数组: [72, 101, 108, 108, 111, 231, 136, 177]
获取的字符: 爱
字符的字节长度: 3
```

### static func intoUtf8Array(Rune, Array\<UInt8>, Int64)

```cangjie
public static func intoUtf8Array(c: Rune, arr: Array<UInt8>, index: Int64): Int64
```

功能：该函数会把字符转成字节码序列然后覆盖 [Array](core_package_structs.md#struct-arrayt) 数组内指定位置的字节码。

参数：

- c: [Rune](core_package_intrinsics.md#rune) - 待转换的字符。
- arr: [Array](core_package_structs.md#struct-arrayt)\<[UInt8](core_package_intrinsics.md#uint8)> - 待覆盖的 [Array](core_package_structs.md#struct-arrayt) 数组。
- index: [Int64](core_package_intrinsics.md#int64) - 目标位置的起始索引。

返回值：

- [Int64](core_package_intrinsics.md#int64) - 字符的字节码长度，例如中文是三个字节码长度。

示例：

<!-- verify -->
```cangjie
main() {
    var arr: Array<UInt8> = [1, 2, 3, 230, 136, 145]
    var len: Int64 = Rune.intoUtf8Array(r'爱', arr, 2)
    println(len)
    println(arr[2]) // 字符爱的utf-8编码的第一个字节
}
```

运行结果：

```text
3
231
```

### static func utf8Size(Array\<UInt8>, Int64)

```cangjie
public static func utf8Size(arr: Array<UInt8>, index: Int64): Int64
```

功能：该函数会返回字节数组指定索引位置为起始的字符占用的字节数。

在 UTF-8 编码中，ASCII 码首位字节第一位不为 1，其他长度的字符首位字节开头 1 的个数表明了该字符对应的字节码长度，该函数通过扫描首位，判断字节码长度。如果索引对应的不是首位字节码，就会抛出异常。

参数：

- arr: [Array](core_package_structs.md#struct-arrayt)\<[UInt8](core_package_intrinsics.md#uint8)> - 待获取字符的字节数组。
- index: [Int64](core_package_intrinsics.md#int64) - 指定字符的索引。

返回值：

- [Int64](core_package_intrinsics.md#int64) - 字符的字节码长度，例如中文是三个字节码长度。

异常：

- [IllegalArgumentException](core_package_exceptions.md#class-illegalargumentexception) - 如果索引位置的字节码不符合首位字节码规则，会抛出异常。

示例：

<!-- verify -->
```cangjie
main() {
    var arr: Array<UInt8> = [1, 2, 231, 136, 177, 145]
    var len: Int64 = Rune.utf8Size(arr, 2)
    println(len) // 索引为2-4的数组元素为中文字符爱的utf-8编码，占用三字节
}
```

运行结果：

```text
3
```

### static func utf8Size(Rune)

```cangjie
public static func utf8Size(c: Rune): Int64
```

功能：返回字符对应的 UTF-8 编码的字节码长度，例如中文字符的字节码长度是 3。

参数：

- c: [Rune](core_package_intrinsics.md#rune) - 待计算 UTF-8 字节码长度的字符。

返回值：

- [Int64](core_package_intrinsics.md#int64) - 字符的 UTF-8 字节码长度。

示例：

<!-- verify -->
```cangjie
main() {
    var char: Rune = r'爱'
    var len: Int64 = Rune.utf8Size(char)
    println(len) // 中文字符爱的utf-8编码，占用三字节
}
```

运行结果：

```text
3
```

### func isAscii()

```cangjie
public func isAscii(): Bool
```

功能：判断字符是否是 Ascii 中的字符。

返回值：

- [Bool](core_package_intrinsics.md#bool) - 如果是 Ascii 字符返回 true，否则返回 false。

示例：

<!-- verify -->
```cangjie
main() {
    // 测试 Rune.isAscii() 方法
    var asciiChar: Rune = r'A' // ASCII 字符
    var nonAsciiChar: Rune = r'爱' // 非 ASCII 字符

    println("字符 '${asciiChar}' 是 ASCII 字符吗? ${asciiChar.isAscii()}")
    println("字符 '${nonAsciiChar}' 是 ASCII 字符吗? ${nonAsciiChar.isAscii()}")
}
```

运行结果：

```text
字符 'A' 是 ASCII 字符吗? true
字符 '爱' 是 ASCII 字符吗? false
```

### func isAsciiControl()

```cangjie
public func isAsciiControl(): Bool
```

功能：判断字符是否是 Ascii 控制字符。其取值范围为 [00, 1F] 和 {7F} 的并集。

返回值：

- [Bool](core_package_intrinsics.md#bool) - 如果是 Ascii 控制字符返回 true，否则返回 false。

示例：

<!-- verify -->
```cangjie
main() {
    // 测试 Rune.isAsciiControl() 方法
    var controlChar: Rune = r'\n' // ASCII 控制字符
    var nonControlChar: Rune = r'A' // 非控制字符

    println("字符 '${controlChar}' 是 ASCII 控制字符吗? ${controlChar.isAsciiControl()}")
    println("字符 '${nonControlChar}' 是 ASCII 控制字符吗? ${nonControlChar.isAsciiControl()}")
}
```

运行结果：

```text
字符 '
' 是 ASCII 控制字符吗? true
字符 'A' 是 ASCII 控制字符吗? false
```

### func isAsciiGraphic()

```cangjie
public func isAsciiGraphic(): Bool
```

功能：判断字符是否是 Ascii 图形字符。其取值范围为 [21, 7E]。

返回值：

- [Bool](core_package_intrinsics.md#bool) - 如果是 Ascii 图形字符返回 true，否则返回 false。

示例：

<!-- verify -->
```cangjie
main() {
    // 测试 Rune.isAsciiGraphic() 方法
    var graphicChar: Rune = r'A' // ASCII 图形字符
    var nonGraphicChar: Rune = r'\n' // 非图形字符（控制字符）

    println("字符 '${graphicChar}' 是 ASCII 图形字符吗? ${graphicChar.isAsciiGraphic()}")
    println("字符 '${nonGraphicChar}' 是 ASCII 图形字符吗? ${nonGraphicChar.isAsciiGraphic()}")
}
```

运行结果：

```text
字符 'A' 是 ASCII 图形字符吗? true
字符 '
' 是 ASCII 图形字符吗? false
```

### func isAsciiHex()

```cangjie
public func isAsciiHex(): Bool
```

功能：判断字符是否是 Ascii 十六进制字符。

返回值：

- [Bool](core_package_intrinsics.md#bool) - 如果是 Ascii 十六进制字符返回 true，否则返回 false。

示例：

<!-- verify -->
```cangjie
main() {
    // 测试 Rune.isAsciiHex() 方法
    var hexChar: Rune = r'A' // ASCII 十六进制字符
    var nonHexChar: Rune = r'G' // 非十六进制字符

    println("字符 '${hexChar}' 是 ASCII 十六进制字符吗? ${hexChar.isAsciiHex()}")
    println("字符 '${nonHexChar}' 是 ASCII 十六进制字符吗? ${nonHexChar.isAsciiHex()}")
}
```

运行结果：

```text
字符 'A' 是 ASCII 十六进制字符吗? true
字符 'G' 是 ASCII 十六进制字符吗? false
```

### func isAsciiLetter()

```cangjie
public func isAsciiLetter(): Bool
```

功能：判断字符是否是 Ascii 字母字符。

返回值：

- [Bool](core_package_intrinsics.md#bool) - 如果是 Ascii 字母字符返回 true，否则返回 false。

示例：

<!-- verify -->
```cangjie
main() {
    // 测试 Rune.isAsciiLetter() 方法
    var letterChar: Rune = r'A' // ASCII 字母字符
    var nonLetterChar: Rune = r'1' // 非字母字符

    println("字符 '${letterChar}' 是 ASCII 字母字符吗? ${letterChar.isAsciiLetter()}")
    println("字符 '${nonLetterChar}' 是 ASCII 字母字符吗? ${nonLetterChar.isAsciiLetter()}")
}
```

运行结果：

```text
字符 'A' 是 ASCII 字母字符吗? true
字符 '1' 是 ASCII 字母字符吗? false
```

### func isAsciiLowerCase()

```cangjie
public func isAsciiLowerCase(): Bool
```

功能：判断字符是否是 Ascii 小写字符。

返回值：

- [Bool](core_package_intrinsics.md#bool) - 如果是 Ascii 小写字符返回 true，否则返回 false。

示例：

<!-- verify -->
```cangjie
main() {
    // 测试 Rune.isAsciiLowerCase() 方法
    var lowerCaseChar: Rune = r'a' // ASCII 小写字符
    var upperCaseChar: Rune = r'A' // ASCII 大写字符

    println("字符 '${lowerCaseChar}' 是 ASCII 小写字符吗? ${lowerCaseChar.isAsciiLowerCase()}")
    println("字符 '${upperCaseChar}' 是 ASCII 小写字符吗? ${upperCaseChar.isAsciiLowerCase()}")
}
```

运行结果：

```text
字符 'a' 是 ASCII 小写字符吗? true
字符 'A' 是 ASCII 小写字符吗? false
```

### func isAsciiNumber()

```cangjie
public func isAsciiNumber(): Bool
```

功能：判断字符是否是 Ascii 数字字符。

返回值：

- [Bool](core_package_intrinsics.md#bool) - 如果是 Ascii 数字字符返回 true，否则返回 false。

示例：

<!-- verify -->
```cangjie
main() {
    // 测试 Rune.isAsciiNumber() 方法
    var numberChar: Rune = r'1' // ASCII 数字字符
    var nonNumberChar: Rune = r'A' // 非数字字符

    println("字符 '${numberChar}' 是 ASCII 数字字符吗? ${numberChar.isAsciiNumber()}")
    println("字符 '${nonNumberChar}' 是 ASCII 数字字符吗? ${nonNumberChar.isAsciiNumber()}")
}
```

运行结果：

```text
字符 '1' 是 ASCII 数字字符吗? true
字符 'A' 是 ASCII 数字字符吗? false
```

### func isAsciiNumberOrLetter()

```cangjie
public func isAsciiNumberOrLetter(): Bool
```

功能：判断字符是否是 Ascii 数字或拉丁字母字符。

返回值：

- [Bool](core_package_intrinsics.md#bool) - 如果是 Ascii 数字或拉丁字母字符返回 true，否则返回 false。

示例：

<!-- verify -->
```cangjie
main() {
    // 测试 Rune.isAsciiNumberOrLetter() 方法
    var numberChar: Rune = r'1' // ASCII 数字字符
    var letterChar: Rune = r'A' // ASCII 字母字符
    var specialChar: Rune = r'!' // 特殊字符

    println("字符 '${numberChar}' 是 ASCII 数字或字母字符吗? ${numberChar.isAsciiNumberOrLetter()}")
    println("字符 '${letterChar}' 是 ASCII 数字或字母字符吗? ${letterChar.isAsciiNumberOrLetter()}")
    println("字符 '${specialChar}' 是 ASCII 数字或字母字符吗? ${specialChar.isAsciiNumberOrLetter()}")
}
```

运行结果：

```text
字符 '1' 是 ASCII 数字或字母字符吗? true
字符 'A' 是 ASCII 数字或字母字符吗? true
字符 '!' 是 ASCII 数字或字母字符吗? false
```

### func isAsciiOct()

```cangjie
public func isAsciiOct(): Bool
```

功能：判断字符是否是 Ascii 八进制字符。

返回值：

- [Bool](core_package_intrinsics.md#bool) - 如果是 Ascii 八进制字符返回 true，否则返回 false。

示例：

<!-- verify -->
```cangjie
main() {
    // 测试 Rune.isAsciiOct() 方法
    var octChar: Rune = r'7' // ASCII 八进制字符
    var nonOctChar: Rune = r'8' // 非八进制字符

    println("字符 '${octChar}' 是 ASCII 八进制字符吗? ${octChar.isAsciiOct()}")
    println("字符 '${nonOctChar}' 是 ASCII 八进制字符吗? ${nonOctChar.isAsciiOct()}")
}
```

运行结果：

```text
字符 '7' 是 ASCII 八进制字符吗? true
字符 '8' 是 ASCII 八进制字符吗? false
```

### func isAsciiPunctuation()

```cangjie
public func isAsciiPunctuation(): Bool
```

功能：判断字符是否是 Ascii 标点符号字符。其取值范围为 [21, 2F]、[3A, 40]、[5B, 60] 和 [7B, 7E] 的并集。

返回值：

- [Bool](core_package_intrinsics.md#bool) - 如果是 Ascii 标点符号字符返回 true，否则返回 false。

示例：

<!-- verify -->
```cangjie
main() {
    // 测试 Rune.isAsciiPunctuation() 方法
    var punctuationChar: Rune = r'!' // ASCII 标点符号字符
    var nonPunctuationChar: Rune = r'A' // 非标点符号字符

    println("字符 '${punctuationChar}' 是 ASCII 标点符号字符吗? ${punctuationChar.isAsciiPunctuation()}")
    println("字符 '${nonPunctuationChar}' 是 ASCII 标点符号字符吗? ${nonPunctuationChar.isAsciiPunctuation()}")
}
```

运行结果：

```text
字符 '!' 是 ASCII 标点符号字符吗? true
字符 'A' 是 ASCII 标点符号字符吗? false
```

### func isAsciiUpperCase()

```cangjie
public func isAsciiUpperCase(): Bool
```

功能：判断字符是否是 Ascii 大写字符。

返回值：

- [Bool](core_package_intrinsics.md#bool) - 如果是 Ascii 大写字符返回 true，否则返回 false。

示例：

<!-- verify -->
```cangjie
main() {
    // 测试 Rune.isAsciiUpperCase() 方法
    var upperCaseChar: Rune = r'A' // ASCII 大写字符
    var lowerCaseChar: Rune = r'a' // ASCII 小写字符

    println("字符 '${upperCaseChar}' 是 ASCII 大写字符吗? ${upperCaseChar.isAsciiUpperCase()}")
    println("字符 '${lowerCaseChar}' 是 ASCII 大写字符吗? ${lowerCaseChar.isAsciiUpperCase()}")
}
```

运行结果：

```text
字符 'A' 是 ASCII 大写字符吗? true
字符 'a' 是 ASCII 大写字符吗? false
```

### func isAsciiWhiteSpace()

```cangjie
public func isAsciiWhiteSpace(): Bool
```

功能：判断字符是否是 Ascii 空白字符。其取值范围为 [09, 0D] 和 {20} 的并集。

返回值：

- [Bool](core_package_intrinsics.md#bool) - 如果是 Ascii 空白字符返回 true，否则返回 false。

示例：

<!-- verify -->
```cangjie
main() {
    // 测试 Rune.isAsciiWhiteSpace() 方法
    var whiteSpaceChar: Rune = r' ' // ASCII 空白字符
    var nonWhiteSpaceChar: Rune = r'A' // 非空白字符

    println("字符 '${whiteSpaceChar}' 是 ASCII 空白字符吗? ${whiteSpaceChar.isAsciiWhiteSpace()}")
    println("字符 '${nonWhiteSpaceChar}' 是 ASCII 空白字符吗? ${nonWhiteSpaceChar.isAsciiWhiteSpace()}")
}
```

运行结果：

```text
字符 ' ' 是 ASCII 空白字符吗? true
字符 'A' 是 ASCII 空白字符吗? false
```

### func toAsciiLowerCase()

```cangjie
public func toAsciiLowerCase(): Rune
```

功能：将字符转换为 Ascii 小写字符，如果无法转换则保持现状。

返回值：

- [Rune](core_package_intrinsics.md#rune) - 转换后的字符，如果无法转换则返回原来的 [Rune](core_package_intrinsics.md#rune)。

示例：

<!-- verify -->
```cangjie
main() {
    // 测试 Rune.toAsciiLowerCase() 方法
    var upperCaseChar: Rune = r'A' // ASCII 大写字符
    var lowerCaseChar: Rune = r'a' // ASCII 小写字符
    var nonLetterChar: Rune = r'1' // 非字母字符

    var convertedUpper = upperCaseChar.toAsciiLowerCase()
    var convertedLower = lowerCaseChar.toAsciiLowerCase()
    var convertedNonLetter = nonLetterChar.toAsciiLowerCase()

    println("大写字符 '${upperCaseChar}' 转换为小写: ${convertedUpper}")
    println("小写字符 '${lowerCaseChar}' 转换为小写: ${convertedLower}")
    println("非字母字符 '${nonLetterChar}' 转换为小写: ${convertedNonLetter}")
}
```

运行结果：

```text
大写字符 'A' 转换为小写: a
小写字符 'a' 转换为小写: a
非字母字符 '1' 转换为小写: 1
```

### func toAsciiUpperCase()

```cangjie
public func toAsciiUpperCase(): Rune
```

功能：将字符转换为 Ascii 大写字符，如果无法转换则保持现状。

返回值：

- [Rune](core_package_intrinsics.md#rune) - 转换后的字符，如果无法转换则返回原来的 [Rune](core_package_intrinsics.md#rune)。

示例：

<!-- verify -->
```cangjie
main() {
    // 测试 Rune.toAsciiUpperCase() 方法
    var lowerCaseChar: Rune = r'a' // ASCII 小写字符
    var upperCaseChar: Rune = r'A' // ASCII 大写字符
    var nonLetterChar: Rune = r'1' // 非字母字符

    var convertedLower = lowerCaseChar.toAsciiUpperCase()
    var convertedUpper = upperCaseChar.toAsciiUpperCase()
    var convertedNonLetter = nonLetterChar.toAsciiUpperCase()

    println("小写字符 '${lowerCaseChar}' 转换为大写: ${convertedLower}")
    println("大写字符 '${upperCaseChar}' 转换为大写: ${convertedUpper}")
    println("非字母字符 '${nonLetterChar}' 转换为大写: ${convertedNonLetter}")
}
```

运行结果：

```text
小写字符 'a' 转换为大写: A
大写字符 'A' 转换为大写: A
非字母字符 '1' 转换为大写: 1
```

## extend Rune <: Comparable\<Rune>

```cangjie
extend Rune <: Comparable<Rune>
```

功能：为 [Rune](core_package_intrinsics.md#rune) 类型扩展 [Comparable](core_package_interfaces.md#interface-comparablet)\<[Rune](core_package_intrinsics.md#rune)> 接口，支持比较操作。

被扩展类型：

- [Rune](core_package_intrinsics.md#rune)

扩展接口：

- [Comparable](core_package_interfaces.md#interface-comparablet)\<[Rune](core_package_intrinsics.md#rune)>

### func compare(Rune)

```cangjie
public func compare(other: Rune): Ordering
```

功能：判断当前 [Rune](core_package_intrinsics.md#rune) 实例与指定 [Rune](core_package_intrinsics.md#rune) 实例的大小关系。

[Rune](core_package_intrinsics.md#rune) 的大小关系指的是它们对应的 unicode 码点的大小关系。

参数：

- other: [Rune](core_package_intrinsics.md#rune) - 待比较的另一个 [Rune](core_package_intrinsics.md#rune) 实例。

返回值：

- [Ordering](core_package_enums.md#enum-ordering) - 如果大于，返回 [Ordering](core_package_enums.md#enum-ordering).GT；如果等于，返回 [Ordering](core_package_enums.md#enum-ordering).EQ；如果小于，返回 [Ordering](core_package_enums.md#enum-ordering).LT。

示例：

<!-- verify -->
```cangjie
main() {
    var char1: Rune = r'i'
    var char2: Rune = r'j'
    println(char1.compare(char2))
}
```

运行结果：

```text
Ordering.LT
```

## extend Rune <: Countable\<Rune>

```cangjie
extend Rune <: Countable<Rune>
```

功能：为 [Rune](core_package_intrinsics.md#rune) 类型扩展 [Countable](core_package_interfaces.md#interface-countablet)\<[Rune](core_package_intrinsics.md#rune)> 接口，支持计数操作。

被扩展类型：

- [Rune](core_package_intrinsics.md#rune)

扩展接口：

- [Countable](core_package_interfaces.md#interface-countablet)\<[Rune](core_package_intrinsics.md#rune)>

### func next(Int64)

```cangjie
public func next(right: Int64): Rune
```

功能：获取当前 [Rune](core_package_intrinsics.md#rune) 值往右数 `right` 后所到位置的 [Rune](core_package_intrinsics.md#rune) 值。

参数：

- right: [Int64](core_package_intrinsics.md#int64) - 往右数的个数。

返回值：

- [Rune](core_package_intrinsics.md#rune) - 往右数 `right` 后所到位置的 [Rune](core_package_intrinsics.md#rune) 值。

异常：

- [OverflowException](core_package_exceptions.md#class-overflowexception) - 如果与 [Int64](core_package_intrinsics.md#int64) 数进行加法运算后为不合法的 Unicode 值，抛出异常。

示例：

<!-- verify -->
```cangjie
main() {
    // 测试 Rune.next 方法
    var char: Rune = r'A'
    var nextChar = char.next(1)
    println("字符: ${char}")
    println("下一个字符: ${nextChar}")

    // 测试向后移动
    var prevChar = char.next(-1)
    println("前一个字符: ${prevChar}")
}
```

运行结果：

```text
字符: A
下一个字符: B
前一个字符: @
```

### func position()

```cangjie
public func position(): Int64
```

功能：获取当前 [Rune](core_package_intrinsics.md#rune) 值的位置信息，即将该 [Rune](core_package_intrinsics.md#rune) 转换为 [Int64](core_package_intrinsics.md#int64) 值。

返回值：

- [Int64](core_package_intrinsics.md#int64) - 当前 [Rune](core_package_intrinsics.md#rune) 值的位置信息。

示例：

<!-- verify -->
```cangjie
main() {
    // 测试 Rune.position 方法
    var char: Rune = r'A'
    var position = char.position()
    println("字符: ${char}")
    println("Unicode码点位置: ${position}")

    // 测试中文字符
    var chineseChar: Rune = r'爱'
    var chinesePosition = chineseChar.position()
    println("字符: ${chineseChar}")
    println("Unicode码点位置: ${chinesePosition}")
}
```

运行结果：

```text
字符: A
Unicode码点位置: 65
字符: 爱
Unicode码点位置: 29233
```

## extend Rune <: Hashable

```cangjie
extend Rune <: Hashable
```

功能：为 [Rune](core_package_intrinsics.md#rune) 类型扩展 [Hashable](core_package_interfaces.md#interface-hashable) 接口，支持计算哈希值。

被扩展类型：

- [Rune](core_package_intrinsics.md#rune)

扩展接口：

- [Hashable](core_package_interfaces.md#interface-hashable)

### func hashCode()

```cangjie
public func hashCode(): Int64
```

功能：获取哈希值。

返回值：

- [Int64](core_package_intrinsics.md#int64) - 哈希值。

示例：

<!-- verify -->
```cangjie
main() {
    // 测试 Rune.hashCode 方法
    var char: Rune = r'A'
    var hash = char.hashCode()
    println("字符: ${char}")
    println("哈希值: ${hash}")

    // 测试中文字符
    var chineseChar: Rune = r'爱'
    var chineseHash = chineseChar.hashCode()
    println("字符: ${chineseChar}")
    println("哈希值: ${chineseHash}")
}
```

运行结果：

```text
字符: A
哈希值: 65
字符: 爱
哈希值: 29233
```

## extend Rune <: ToString

```cangjie
extend Rune <: ToString
```

功能：这里为 [Rune](core_package_intrinsics.md#rune) 类型扩展 [ToString](core_package_interfaces.md#interface-tostring) 接口，实现向 [String](core_package_structs.md#struct-string) 类型的转换。

被扩展类型：

- [Rune](core_package_intrinsics.md#rune)

扩展接口：

- [ToString](core_package_interfaces.md#interface-tostring)

### func toString()

```cangjie
public func toString(): String
```

功能：将 [Rune](core_package_intrinsics.md#rune) 值转换为字符串。

返回值：

- [String](core_package_structs.md#struct-string) - 转化后的字符串。

示例：

<!-- verify -->
```cangjie
main() {
    // 测试 Rune.toString 方法
    var char: Rune = r'A'
    var str = char.toString()
    println("字符: ${char}")
    println("转换为字符串: ${str}")

    // 测试中文字符
    var chineseChar: Rune = r'爱'
    var chineseStr = chineseChar.toString()
    println("字符: ${chineseChar}")
    println("转换为字符串: ${chineseStr}")
}
```

运行结果：

```text
字符: A
转换为字符串: A
字符: 爱
转换为字符串: 爱
```

## extend UInt16 <: Comparable\<UInt16>

```cangjie
extend UInt16 <: Comparable<UInt16>
```

功能：为 [UInt16](core_package_intrinsics.md#uint16) 类型扩展 [Comparable](core_package_interfaces.md#interface-comparablet)\<[UInt16](core_package_intrinsics.md#uint16)> 接口，支持比较操作。

被扩展类型：

- [UInt16](core_package_intrinsics.md#uint16)

扩展接口：

- [Comparable](core_package_interfaces.md#interface-comparablet)\<[UInt16](core_package_intrinsics.md#uint16)>

### func compare(UInt16)

```cangjie
public func compare(other: UInt16): Ordering
```

功能：判断当前 [UInt16](core_package_intrinsics.md#uint16) 值与指定 [UInt16](core_package_intrinsics.md#uint16) 值的大小关系。

参数：

- other: [UInt16](core_package_intrinsics.md#uint16) - 待比较的另一个 [UInt16](core_package_intrinsics.md#uint16) 值。

返回值：

- [Ordering](core_package_enums.md#enum-ordering) - 如果大于，返回 [Ordering](core_package_enums.md#enum-ordering).GT；如果等于，返回 [Ordering](core_package_enums.md#enum-ordering).EQ；如果小于，返回 [Ordering](core_package_enums.md#enum-ordering).LT。

示例：

<!-- verify -->
```cangjie
main() {
    var num1: UInt16 = 2
    var num2: UInt16 = 3
    println(num1.compare(num2))
}
```

运行结果：

```text
Ordering.LT
```

## extend UInt16 <: Countable\<UInt16>

```cangjie
extend UInt16 <: Countable<UInt16>
```

功能：为 [UInt16](core_package_intrinsics.md#uint16) 类型扩展 [Countable](core_package_interfaces.md#interface-countablet)\<[UInt16](core_package_intrinsics.md#uint16)> 接口，支持计数操作。

被扩展类型：

- [UInt16](core_package_intrinsics.md#uint16)

扩展接口：

- [Countable](core_package_interfaces.md#interface-countablet)\<[UInt16](core_package_intrinsics.md#uint16)>

### func next(Int64)

```cangjie
public func next(right: Int64): UInt16
```

功能：获取在数轴上当前 [UInt16](core_package_intrinsics.md#int32) 位置往右移动 `right` 后对应位置的 [UInt16](core_package_intrinsics.md#int32) 值。如果值溢出，则会从数轴最左边继续移动。

参数：

- right: [Int64](core_package_intrinsics.md#int64) - 往右数的个数。

返回值：

- [UInt16](core_package_intrinsics.md#uint16) - 往右数 `right` 后所到位置的 [UInt16](core_package_intrinsics.md#uint16) 值。

示例：

<!-- verify -->
```cangjie
main() {
    var num: UInt16 = 3
    println(num.next(10))
}
```

运行结果：

```text
13
```

### func position()

```cangjie
public func position(): Int64
```

功能：获取当前 [UInt16](core_package_intrinsics.md#uint16) 值的位置信息，即将该 [UInt16](core_package_intrinsics.md#uint16) 转换为 [Int64](core_package_intrinsics.md#int64) 值。

返回值：

- [Int64](core_package_intrinsics.md#int64) - 当前 [UInt16](core_package_intrinsics.md#uint16) 值的位置信息。

示例：

<!-- verify -->
```cangjie
main() {
    var num: UInt16 = 8
    println(num.position())
}
```

运行结果：

```text
8
```

## extend UInt16 <: Hashable

```cangjie
extend UInt16 <: Hashable
```

功能：为 [UInt16](core_package_intrinsics.md#uint16) 类型扩展 [Hashable](core_package_interfaces.md#interface-hashable) 接口，支持计算哈希值，并拓展该类型以支持一些数学常数。

被扩展类型：

- [UInt16](core_package_intrinsics.md#uint16)

扩展接口：

- [Hashable](core_package_interfaces.md#interface-hashable)

### static prop Max

```cangjie
public static prop Max: UInt16
```

功能：获取 16 位无符号整数的最大值。

类型：[UInt16](./core_package_intrinsics.md#uint16)

示例：

<!-- verify -->
```cangjie
main() {
    // 测试 UInt16.Max 属性
    var maxVal = UInt16.Max
    println("UInt16的最大值: ${maxVal}")

    // 验证最大值是否正确
    if (maxVal == 65535) {
        println("验证通过: UInt16.Max 等于 65535")
    } else {
        println("验证失败: UInt16.Max 不等于 65535")
    }
}
```

运行结果：

```text
UInt16的最大值: 65535
验证通过: UInt16.Max 等于 65535
```

### static prop Min

```cangjie
public static prop Min: UInt16
```

功能：获取 16 位无符号整数的最小值。

类型：[UInt16](./core_package_intrinsics.md#uint16)

示例：

<!-- verify -->
```cangjie
main() {
    // 测试 UInt16.Min 属性
    var minVal = UInt16.Min
    println("UInt16的最小值: ${minVal}")

    // 验证最小值是否正确
    if (minVal == 0) {
        println("验证通过: UInt16.Min 等于 0")
    } else {
        println("验证失败: UInt16.Min 不等于 0")
    }
}
```

运行结果：

```text
UInt16的最小值: 0
验证通过: UInt16.Min 等于 0
```

### func hashCode()

```cangjie
public func hashCode(): Int64
```

功能：获取哈希值。

返回值：

- [Int64](core_package_intrinsics.md#int64) - 哈希值。

示例：

<!-- verify -->
```cangjie
main() {
    // 测试 UInt16.hashCode 方法
    var num: UInt16 = 42
    var hash = num.hashCode()
    println("UInt16值: ${num}")
    println("哈希值: ${hash}")

    // 测试最大值
    var maxNum: UInt16 = UInt16.Max
    var maxHash = maxNum.hashCode()
    println("最大UInt16值: ${maxNum}")
    println("哈希值: ${maxHash}")
}
```

运行结果：

```text
UInt16值: 42
哈希值: 42
最大UInt16值: 65535
哈希值: 65535
```

## extend UInt16 <: ToString

```cangjie
extend UInt16 <: ToString
```

功能：这里为 [UInt16](core_package_intrinsics.md#uint16) 类型扩展 [ToString](core_package_interfaces.md#interface-tostring) 接口，实现向 [String](core_package_structs.md#struct-string) 类型的转换。

被扩展类型：

- [UInt16](core_package_intrinsics.md#uint16)

扩展接口：

- [ToString](core_package_interfaces.md#interface-tostring)

### func toString()

```cangjie
public func toString(): String
```

功能：将 [UInt16](core_package_intrinsics.md#uint16) 值转换为字符串。

返回值：

- [String](core_package_structs.md#struct-string) - 转化后的字符串。

示例：

<!-- verify -->
```cangjie
main() {
    // 测试 UInt16.toString 方法
    var num: UInt16 = 42
    var str = num.toString()
    println("UInt16值: ${num}")
    println("转换为字符串: ${str}")

    // 测试最大值
    var maxNum: UInt16 = UInt16.Max
    var maxStr = maxNum.toString()
    println("最大UInt16值: ${maxNum}")
    println("转换为字符串: ${maxStr}")
}
```

运行结果：

```text
UInt16值: 42
转换为字符串: 42
最大UInt16值: 65535
转换为字符串: 65535
```

## extend UInt32 <: Comparable\<UInt32>

```cangjie
extend UInt32 <: Comparable<UInt32>
```

功能：为 [UInt32](core_package_intrinsics.md#uint32) 类型扩展 [Comparable](core_package_interfaces.md#interface-comparablet)\<[UInt32](core_package_intrinsics.md#uint32)> 接口，支持比较操作。

被扩展类型：

- [UInt32](core_package_intrinsics.md#uint32)

扩展接口：

- [Comparable](core_package_interfaces.md#interface-comparablet)\<[UInt32](core_package_intrinsics.md#uint32)>

### func compare(UInt32)

```cangjie
public func compare(other: UInt32): Ordering
```

功能：判断当前 [UInt32](core_package_intrinsics.md#uint32) 值与指定 [UInt32](core_package_intrinsics.md#uint32) 值的大小关系。

参数：

- other: [UInt32](core_package_intrinsics.md#uint32) - 待比较的另一个 [UInt32](core_package_intrinsics.md#uint32) 值。

返回值：

- [Ordering](core_package_enums.md#enum-ordering) - 如果大于，返回 [Ordering](core_package_enums.md#enum-ordering).GT；如果等于，返回 [Ordering](core_package_enums.md#enum-ordering).EQ；如果小于，返回 [Ordering](core_package_enums.md#enum-ordering).LT。

示例：

<!-- verify -->
```cangjie
main() {
    var num1: UInt32 = 2
    var num2: UInt32 = 3
    println(num1.compare(num2))
}
```

运行结果：

```text
Ordering.LT
```

## extend UInt32 <: Countable\<UInt32>

```cangjie
extend UInt32 <: Countable<UInt32>
```

功能：为 [UInt32](core_package_intrinsics.md#uint32) 类型扩展 [Countable](core_package_interfaces.md#interface-countablet)\<[UInt32](core_package_intrinsics.md#uint32)> 接口，支持计数操作。

被扩展类型：

- [UInt32](core_package_intrinsics.md#uint32)

扩展接口：

- [Countable](core_package_interfaces.md#interface-countablet)\<[UInt32](core_package_intrinsics.md#uint32)>

### func next(Int64)

```cangjie
public func next(right: Int64): UInt32
```

功能：获取在数轴上当前 [UInt32](core_package_intrinsics.md#int32) 位置往右移动 `right` 后对应位置的 [UInt32](core_package_intrinsics.md#int32) 值。如果值溢出，则会从数轴最左边继续移动。

参数：

- right: [Int64](core_package_intrinsics.md#int64) - 往右数的个数。

返回值：

- [UInt32](core_package_intrinsics.md#uint32) - 往右数 `right` 后所到位置的 [UInt32](core_package_intrinsics.md#uint32) 值。

示例：

<!-- verify -->
```cangjie
main() {
    var num: UInt32 = 3
    println(num.next(10))
}
```

运行结果：

```text
13
```

### func position()

```cangjie
public func position(): Int64
```

功能：获取当前 [UInt32](core_package_intrinsics.md#uint32) 值的位置信息，即将该 [UInt32](core_package_intrinsics.md#uint32) 转换为 [Int64](core_package_intrinsics.md#int64) 值。

返回值：

- [Int64](core_package_intrinsics.md#int64) - 当前 [UInt32](core_package_intrinsics.md#uint32) 值的位置信息。

示例：

<!-- verify -->
```cangjie
main() {
    var num: UInt32 = 8
    println(num.position())
}
```

运行结果：

```text
8
```

## extend UInt32 <: Hashable

```cangjie
extend UInt32 <: Hashable
```

功能：为 [UInt32](core_package_intrinsics.md#uint32) 类型扩展 [Hashable](core_package_interfaces.md#interface-hashable) 接口，支持计算哈希值，并拓展该类型以支持一些数学常数。

被扩展类型：

- [UInt32](core_package_intrinsics.md#uint32)

扩展接口：

- [Hashable](core_package_interfaces.md#interface-hashable)

### static prop Max

```cangjie
public static prop Max: UInt32
```

功能：获取 32 位无符号整数的最大值。

类型：[UInt32](./core_package_intrinsics.md#uint32)

示例：

<!-- verify -->
```cangjie
main() {
    var maxValue = UInt32.Max
    println(maxValue)
}
```

运行结果：

```text
4294967295
```

### static prop Min

```cangjie
public static prop Min: UInt32
```

功能：获取 32 位无符号整数的最小值。

类型：[UInt32](./core_package_intrinsics.md#uint32)

示例：

<!-- verify -->
```cangjie
main() {
    var minValue = UInt32.Min
    println(minValue)
}
```

运行结果：

```text
0
```

### func hashCode()

```cangjie
public func hashCode(): Int64
```

功能：获取哈希值。

返回值：

- [Int64](core_package_intrinsics.md#int64) - 哈希值。

示例：

<!-- verify -->
```cangjie
main() {
    var num: UInt32 = 42
    var hash = num.hashCode()
    println(hash)
}
```

运行结果：

```text
42
```

## extend UInt32 <: ToString

```cangjie
extend UInt32 <: ToString
```

功能：这里为 [UInt32](core_package_intrinsics.md#uint32) 类型扩展 [ToString](core_package_interfaces.md#interface-tostring) 接口，实现向 [String](core_package_structs.md#struct-string) 类型的转换。

被扩展类型：

- [UInt32](core_package_intrinsics.md#uint32)

扩展接口：

- [ToString](core_package_interfaces.md#interface-tostring)

### func toString()

```cangjie
public func toString(): String
```

功能：将 [UInt32](core_package_intrinsics.md#uint32) 值转换为字符串。

返回值：

- [String](core_package_structs.md#struct-string) - 转化后的字符串。

示例：

<!-- verify -->
```cangjie
main() {
    var num: UInt32 = 42
    var str = num.toString()
    println(str)
}
```

运行结果：

```text
42
```

## extend UInt64 <: Comparable\<UInt64>

```cangjie
extend UInt64 <: Comparable<UInt64>
```

功能：为 [UInt64](core_package_intrinsics.md#uint64) 类型扩展 [Comparable](core_package_interfaces.md#interface-comparablet)\<[UInt64](core_package_intrinsics.md#uint64)> 接口，支持比较操作。

被扩展类型：

- [UInt64](core_package_intrinsics.md#uint64)

扩展接口：

- [Comparable](core_package_interfaces.md#interface-comparablet)\<[UInt64](core_package_intrinsics.md#uint64)>

### func compare(UInt64)

```cangjie
public func compare(other: UInt64): Ordering
```

功能：判断当前 [UInt64](core_package_intrinsics.md#uint64) 值与指定 [UInt64](core_package_intrinsics.md#uint64) 值的大小关系。

参数：

- other: [UInt64](core_package_intrinsics.md#uint64) - 待比较的另一个 [UInt64](core_package_intrinsics.md#uint64) 值。

返回值：

- [Ordering](core_package_enums.md#enum-ordering) - 如果大于，返回 [Ordering](core_package_enums.md#enum-ordering).GT；如果等于，返回 [Ordering](core_package_enums.md#enum-ordering).EQ；如果小于，返回 [Ordering](core_package_enums.md#enum-ordering).LT。

示例：

<!-- verify -->
```cangjie
main() {
    var num1: UInt64 = 2
    var num2: UInt64 = 3
    println(num1.compare(num2))
}
```

运行结果：

```text
Ordering.LT
```

## extend UInt64 <: Countable\<UInt64>

```cangjie
extend UInt64 <: Countable<UInt64>
```

功能：为 [UInt64](core_package_intrinsics.md#uint64) 类型扩展 [Countable](core_package_interfaces.md#interface-countablet)\<[UInt64](core_package_intrinsics.md#uint64)> 接口，支持计数操作。

被扩展类型：

- [UInt64](core_package_intrinsics.md#uint64)

扩展接口：

- [Countable](core_package_interfaces.md#interface-countablet)\<[UInt64](core_package_intrinsics.md#uint64)>

### func next(Int64)

```cangjie
public func next(right: Int64): UInt64
```

功能：获取在数轴上当前 [UInt64](core_package_intrinsics.md#int32) 位置往右移动 `right` 后对应位置的 [UInt64](core_package_intrinsics.md#int32) 值。如果值溢出，则会从数轴最左边继续移动。

参数：

- right: [Int64](core_package_intrinsics.md#int64) - 往右数的个数。

返回值：

- [UInt64](core_package_intrinsics.md#uint64) - 往右数 `right` 后所到位置的 [UInt64](core_package_intrinsics.md#uint64) 值。

示例：

<!-- verify -->
```cangjie
main() {
    var num: UInt64 = 3
    println(num.next(10))
}
```

运行结果：

```text
13
```

### func position()

```cangjie
public func position(): Int64
```

功能：获取当前 [UInt64](core_package_intrinsics.md#uint64) 值的位置信息，即将该 [UInt64](core_package_intrinsics.md#uint64) 转换为 [Int64](core_package_intrinsics.md#int64) 值。

返回值：

- [Int64](core_package_intrinsics.md#int64) - 当前 [UInt64](core_package_intrinsics.md#uint64) 值的位置信息。

示例：

<!-- verify -->
```cangjie
main() {
    var num: UInt64 = 8
    println(num.position())
}
```

运行结果：

```text
8
```

## extend UInt64 <: Hashable

```cangjie
extend UInt64 <: Hashable
```

功能：为 [UInt64](core_package_intrinsics.md#uint64) 类型扩展 [Hashable](core_package_interfaces.md#interface-hashable) 接口，支持计算哈希值，并拓展该类型以支持一些数学常数。

被扩展类型：

- [UInt64](core_package_intrinsics.md#uint64)

扩展接口：

- [Hashable](core_package_interfaces.md#interface-hashable)

### static prop Max

```cangjie
public static prop Max: UInt64
```

功能：获取 64 位无符号整数的最大值。

类型：[UInt64](./core_package_intrinsics.md#uint64)

示例：

<!-- verify -->
```cangjie
main() {
    var maxValue = UInt64.Max
    println(maxValue)
}
```

运行结果：

```text
18446744073709551615
```

### static prop Min

```cangjie
public static prop Min: UInt64
```

功能：获取 64 位无符号整数的最小值。

类型：[UInt64](./core_package_intrinsics.md#uint64)

示例：

<!-- verify -->
```cangjie
main() {
    var minValue = UInt64.Min
    println(minValue)
}
```

运行结果：

```text
0
```

### func hashCode()

```cangjie
public func hashCode(): Int64
```

功能：获取哈希值。

返回值：

- [Int64](core_package_intrinsics.md#int64) - 哈希值。

示例：

<!-- verify -->
```cangjie
main() {
    var num: UInt64 = 42
    var hash = num.hashCode()
    println(hash)
}
```

运行结果：

```text
42
```

## extend UInt64 <: ToString

```cangjie
extend UInt64 <: ToString
```

功能：这里为 [UInt64](core_package_intrinsics.md#uint64) 类型扩展 [ToString](core_package_interfaces.md#interface-tostring) 接口，实现向 [String](core_package_structs.md#struct-string) 类型的转换。

被扩展类型：

- [UInt64](core_package_intrinsics.md#uint64)

扩展接口：

- [ToString](core_package_interfaces.md#interface-tostring)

### func toString()

```cangjie
public func toString(): String
```

功能：将 [UInt64](core_package_intrinsics.md#uint64) 值转换为字符串。

返回值：

- [String](core_package_structs.md#struct-string) - 转化后的字符串。

示例：

<!-- verify -->
```cangjie
main() {
    var num: UInt64 = 42
    var str = num.toString()
    println(str)
}
```

运行结果：

```text
42
```

## extend UInt8 <: Comparable\<UInt8>

```cangjie
extend UInt8 <: Comparable<UInt8>
```

功能：为 [UInt8](core_package_intrinsics.md#uint8) 类型扩展 [Comparable](core_package_interfaces.md#interface-comparablet)\<[UInt8](core_package_intrinsics.md#uint8)> 接口，支持比较操作。

被扩展类型：

- [UInt8](core_package_intrinsics.md#uint8)

扩展接口：

- [Comparable](core_package_interfaces.md#interface-comparablet)\<[UInt8](core_package_intrinsics.md#uint8)>

### func compare(UInt8)

```cangjie
public func compare(other: UInt8): Ordering
```

功能：判断当前 [UInt8](core_package_intrinsics.md#uint8) 值与指定 [UInt8](core_package_intrinsics.md#uint8) 值的大小关系。

参数：

- other: [UInt8](core_package_intrinsics.md#uint8) - 待比较的另一个 [UInt8](core_package_intrinsics.md#uint8) 值。

返回值：

- [Ordering](core_package_enums.md#enum-ordering) - 如果大于，返回 [Ordering](core_package_enums.md#enum-ordering).GT；如果等于，返回 [Ordering](core_package_enums.md#enum-ordering).EQ；如果小于，返回 [Ordering](core_package_enums.md#enum-ordering).LT。

示例：

<!-- verify -->
```cangjie
main() {
    var num1: UInt8 = 2
    var num2: UInt8 = 3
    println(num1.compare(num2))
}
```

运行结果：

```text
Ordering.LT
```

## extend UInt8 <: Countable\<UInt8>

```cangjie
extend UInt8 <: Countable<UInt8>
```

功能：为 [UInt8](core_package_intrinsics.md#uint8) 类型扩展 [Countable](core_package_interfaces.md#interface-countablet)\<[UInt8](core_package_intrinsics.md#uint8)> 接口，支持计数操作。

被扩展类型：

- [UInt8](core_package_intrinsics.md#uint8)

扩展接口：

- [Countable](core_package_interfaces.md#interface-countablet)\<[UInt8](core_package_intrinsics.md#uint8)>

### func next(Int64)

```cangjie
public func next(right: Int64): UInt8
```

功能：获取在数轴上当前 [UInt8](core_package_intrinsics.md#int32) 位置往右移动 `right` 后对应位置的 [UInt8](core_package_intrinsics.md#int32) 值。如果值溢出，则会从数轴最左边继续移动。

参数：

- right: [Int64](core_package_intrinsics.md#int64) - 往右数的个数。

返回值：

- [UInt8](core_package_intrinsics.md#uint8) - 往右数 `right` 后所到位置的 [UInt8](core_package_intrinsics.md#uint8) 值。

示例：

<!-- verify -->
```cangjie
main() {
    var num: UInt8 = 3
    println(num.next(10))
}
```

运行结果：

```text
13
```

### func position()

```cangjie
public func position(): Int64
```

功能：获取当前 [UInt8](core_package_intrinsics.md#uint8) 值的位置信息，即将该 [UInt8](core_package_intrinsics.md#uint8) 转换为 [Int64](core_package_intrinsics.md#int64) 值。

返回值：

- [Int64](core_package_intrinsics.md#int64) - 当前 [UInt8](core_package_intrinsics.md#uint8) 值的位置信息。

示例：

<!-- verify -->
```cangjie
main() {
    var num: UInt8 = 8
    println(num.position())
}
```

运行结果：

```text
8
```

## extend UInt8 <: Hashable

```cangjie
extend UInt8 <: Hashable
```

功能：为 [UInt8](core_package_intrinsics.md#uint8) 类型扩展 [Hashable](core_package_interfaces.md#interface-hashable) 接口，支持计算哈希值，并拓展该类型以支持一些数学常数。

被扩展类型：

- [UInt8](core_package_intrinsics.md#uint8)

扩展接口：

- [Hashable](core_package_interfaces.md#interface-hashable)

### static prop Max

```cangjie
public static prop Max: UInt8
```

功能：获取 8 位无符号整数的最大值。

类型：[UInt8](./core_package_intrinsics.md#uint8)

示例：

<!-- verify -->
```cangjie
main() {
    var maxValue = UInt8.Max
    println(maxValue)
}
```

运行结果：

```text
255
```

### static prop Min

```cangjie
public static prop Min: UInt8
```

功能：获取 8 位无符号整数的最小值。

类型：[UInt8](./core_package_intrinsics.md#uint8)

示例：

<!-- verify -->
```cangjie
main() {
    var minValue = UInt8.Min
    println(minValue)
}
```

运行结果：

```text
0
```

### func hashCode()

```cangjie
public func hashCode(): Int64
```

功能：获取哈希值。

返回值：

- [Int64](core_package_intrinsics.md#int64) - 哈希值。

示例：

<!-- verify -->
```cangjie
main() {
    var num: UInt8 = 42
    var hash = num.hashCode()
    println(hash)
}
```

运行结果：

```text
42
```

## extend UInt8 <: ToString

```cangjie
extend UInt8 <: ToString
```

功能：这里为 [UInt8](core_package_intrinsics.md#uint8) 类型扩展 [ToString](core_package_interfaces.md#interface-tostring) 接口，实现向 [String](core_package_structs.md#struct-string) 类型的转换。

被扩展类型：

- [UInt8](core_package_intrinsics.md#uint8)

扩展接口：

- [ToString](core_package_interfaces.md#interface-tostring)

### func toString()

```cangjie
public func toString(): String
```

功能：将 [UInt8](core_package_intrinsics.md#uint8) 值转换为字符串。

返回值：

- [String](core_package_structs.md#struct-string) - 转化后的字符串。

示例：

<!-- verify -->
```cangjie
main() {
    var num: UInt8 = 42
    var str = num.toString()
    println(str)
}
```

运行结果：

```text
42
```

## extend UIntNative <: Comparable\<UIntNative>

```cangjie
extend UIntNative <: Comparable<UIntNative>
```

功能：为 [UIntNative](core_package_intrinsics.md#uintnative) 类型扩展 [Comparable](core_package_interfaces.md#interface-comparablet)\<[UIntNative](core_package_intrinsics.md#uintnative)> 接口，支持比较操作。

被扩展类型：

- [UIntNative](core_package_intrinsics.md#uintnative)

扩展接口：

- [Comparable](core_package_interfaces.md#interface-comparablet)\<[UIntNative](core_package_intrinsics.md#uintnative)>

### func compare(UIntNative)

```cangjie
public func compare(other: UIntNative): Ordering
```

功能：判断当前 [UIntNative](core_package_intrinsics.md#uintnative) 值与指定 [UIntNative](core_package_intrinsics.md#uintnative) 值的大小关系。

参数：

- other: [UIntNative](core_package_intrinsics.md#uintnative) - 待比较的另一个 [UIntNative](core_package_intrinsics.md#uintnative) 值。

返回值：

- [Ordering](core_package_enums.md#enum-ordering) - 如果大于，返回 [Ordering](core_package_enums.md#enum-ordering).GT；如果等于，返回 [Ordering](core_package_enums.md#enum-ordering).EQ；如果小于，返回 [Ordering](core_package_enums.md#enum-ordering).LT。

示例：

<!-- verify -->
```cangjie
main() {
    var num1: UIntNative = 2
    var num2: UIntNative = 3
    println(num1.compare(num2))
}
```

运行结果：

```text
Ordering.LT
```

## extend UIntNative <: Countable\<UIntNative>

```cangjie
extend UIntNative <: Countable<UIntNative>
```

功能：为 [UIntNative](core_package_intrinsics.md#uintnative) 类型扩展 [Countable](core_package_interfaces.md#interface-countablet)\<[UIntNative](core_package_intrinsics.md#uintnative)> 接口，支持计数操作。

被扩展类型：

- [UIntNative](core_package_intrinsics.md#uintnative)

扩展接口：

- [Countable](core_package_interfaces.md#interface-countablet)\<[UIntNative](core_package_intrinsics.md#uintnative)>

### func next(Int64)

```cangjie
public func next(right: Int64): UIntNative
```

功能：获取在数轴上当前 [UIntNative](core_package_intrinsics.md#int32) 位置往右移动 `right` 后对应位置的 [UIntNative](core_package_intrinsics.md#int32) 值。如果值溢出，则会从数轴最左边继续移动。

参数：

- right: [Int64](core_package_intrinsics.md#int64) - 往右数的个数。

返回值：

- [UIntNative](core_package_intrinsics.md#uintnative) - 往右数 `right` 后所到位置的 [UIntNative](core_package_intrinsics.md#uintnative) 值。

示例：

<!-- verify -->
```cangjie
main() {
    var num: UIntNative = 3
    println(num.next(10))
}
```

运行结果：

```text
13
```

### func position()

```cangjie
public func position(): Int64
```

功能：获取当前 [UIntNative](core_package_intrinsics.md#uintnative) 值的位置信息，即将该 [UIntNative](core_package_intrinsics.md#uintnative) 转换为 [Int64](core_package_intrinsics.md#int64) 值。

返回值：

- [Int64](core_package_intrinsics.md#int64) - 当前 [UIntNative](core_package_intrinsics.md#uintnative) 值的位置信息。

示例：

<!-- verify -->
```cangjie
main() {
    var num: UIntNative = 8
    println(num.position())
}
```

运行结果：

```text
8
```

## extend UIntNative <: Hashable

```cangjie
extend UIntNative <: Hashable
```

功能：为 [UIntNative](core_package_intrinsics.md#uintnative) 类型扩展 [Hashable](core_package_interfaces.md#interface-hashable) 接口，支持计算哈希值，并拓展该类型以支持一些数学常数。

被扩展类型：

- [UIntNative](core_package_intrinsics.md#uintnative)

扩展接口：

- [Hashable](core_package_interfaces.md#interface-hashable)

### static prop Max

```cangjie
public static prop Max: UIntNative
```

功能：获取平台相关无符号整数的最大值。

类型：[UIntNative](./core_package_intrinsics.md#uintnative)

示例：

<!-- verify -->
```cangjie
main() {
    var maxValue = UIntNative.Max
    println(maxValue)
}
```

运行结果：

```text
18446744073709551615
```

### static prop Min

```cangjie
public static prop Min: UIntNative
```

功能：获取平台相关无符号整数的最小值。

类型：[UIntNative](./core_package_intrinsics.md#uintnative)

示例：

<!-- verify -->
```cangjie
main() {
    var minValue = UIntNative.Min
    println(minValue)
}
```

运行结果：

```text
0
```

### func hashCode()

```cangjie
public func hashCode(): Int64
```

功能：获取哈希值。

返回值：

- [Int64](core_package_intrinsics.md#int64) - 哈希值。

示例：

<!-- verify -->
```cangjie
main() {
    var num: UIntNative = 42
    var hash = num.hashCode()
    println(hash)
}
```

运行结果：

```text
42
```

## extend UIntNative <: ToString

```cangjie
extend UIntNative <: ToString
```

功能：这里为 [UIntNative](core_package_intrinsics.md#uintnative) 类型扩展 [ToString](core_package_interfaces.md#interface-tostring) 接口，实现向 [String](core_package_structs.md#struct-string) 类型的转换。

被扩展类型：

- [UIntNative](core_package_intrinsics.md#uintnative)

扩展接口：

- [ToString](core_package_interfaces.md#interface-tostring)

### func toString()

```cangjie
public func toString(): String
```

功能：将 [UIntNative](core_package_intrinsics.md#uintnative) 值转换为字符串。

返回值：

- [String](core_package_structs.md#struct-string) - 转化后的字符串。

示例：

<!-- verify -->
```cangjie
main() {
    var num: UIntNative = 12345
    var str = num.toString()
    println("The string representation is: " + str)
}
```

运行结果：

```text
The string representation is: 12345
```

## extend Unit <: Equatable\<Unit>

```cangjie
extend Unit <: Equatable<Unit>
```

功能：为 [Unit](core_package_intrinsics.md#unit) 类型扩展 [Equatable](core_package_interfaces.md#interface-equatablet)\<[Unit](core_package_intrinsics.md#unit)> 接口。

被扩展类型：

- [Unit](core_package_intrinsics.md#unit)

扩展接口：

- [Equatable](core_package_interfaces.md#interface-equatablet)\<[Unit](core_package_intrinsics.md#unit)>

## extend Unit <: Hashable

```cangjie
extend Unit <: Hashable
```

功能：为 [Unit](core_package_intrinsics.md#unit) 类型扩展 [Hashable](core_package_interfaces.md#interface-hashable) 接口，支持计算哈希值。

被扩展类型：

- [Unit](core_package_intrinsics.md#unit)

扩展接口：

- [Hashable](core_package_interfaces.md#interface-hashable)

### func hashCode()

```cangjie
public func hashCode(): Int64
```

功能：获取哈希值，[Unit](core_package_intrinsics.md#unit) 的哈希值为 0。

返回值：

- [Int64](core_package_intrinsics.md#int64) - 哈希值。

示例：

<!-- verify -->
```cangjie
main() {
    var unitValue = ()
    var hash = unitValue.hashCode()
    println(hash)
}
```

运行结果：

```text
0
```

## extend Unit <: ToString

```cangjie
extend Unit <: ToString
```

功能：为 [Unit](core_package_intrinsics.md#unit) 类型扩展 [ToString](core_package_interfaces.md#interface-tostring) 接口，实现向 [String](core_package_structs.md#struct-string) 类型的转换。

被扩展类型：

- [Unit](core_package_intrinsics.md#unit)

扩展接口：

- [ToString](core_package_interfaces.md#interface-tostring)

### func toString()

```cangjie
public func toString(): String
```

功能：将 [Unit](core_package_intrinsics.md#unit) 值转换为字符串。

返回值：

- [String](core_package_structs.md#struct-string) - 转化后的字符串。

示例：

<!-- verify -->
```cangjie
main() {
    var unitValue = ()
    var str = unitValue.toString()
    println("The string representation is: " + str)
}
```

运行结果：

```text
The string representation is: ()
```

## extend\<T> Array\<Array\<T>>

```cangjie
extend<T> Array<Array<T>>
```

功能：为二维数组进行扩展，提供将其展开为一维数组的方法。

被扩展类型：

- [Array\<Array\<T>>](./core_package_structs.md#struct-arrayt)

### func flatten()

```cangjie
public func flatten(): Array<T>
```

功能：将当前二维数组展开为一维数组。

例如将 [[1, 2], [3, 4]] 展开为 [1, 2, 3, 4]。

返回值：

- [Array](./core_package_structs.md#struct-arrayt)\<T> - 展开后的一维数组。

示例：

<!-- verify -->
```cangjie
main(): Int64 {
    let arr = [[1, 2], [3, 4]].flatten()
    println(arr)
    return 0
}
```

运行结果：

```text
[1, 2, 3, 4]
```

<!--Del-->

## extend\<T> Array\<T>

```cangjie
extend<T> Array<T>
```

功能：为 [Array](core_package_structs.md#struct-arrayt)\<T> 类型进行拓展

被扩展类型：

- [Array\<T>](core_package_structs.md#struct-arrayt)

### func enumerate()

```cangjie
public func enumerate(): Array<(Int64, T)>
```

功能：用于获取带索引的数组。

> **注意：**
>
> 不支持平台：OpenHarmony。

返回值：

- [Array](core_package_structs.md#struct-arrayt)\<([Int64](../../core/core_package_api/core_package_intrinsics.md#int64), T)> - 返回一个带索引的新数组。

示例：

<!-- verify -->
```cangjie
main() {
    let arr = ["apple", "banana", "cherry"]

    println("原数组: ${arr}")

    // 使用enumerate获取带索引的数组
    let enumerated = arr.enumerate()

    // 遍历带索引的数组
    println("遍历带索引的数组:")
    for (pair in enumerated) {
        let (index, value) = pair
        println(" (${index}, ${value})")
    }
    return 0
}
```

运行结果：

```text
原数组: [apple, banana, cherry]
遍历带索引的数组:
 (0, apple)
 (1, banana)
 (2, cherry)
```

### func zip\<R>(Array\<R>)

```cangjie
public func zip<R>(other: Array<R>): Array<(T, R)>
```

功能：将两个数组合并成一个新数组（长度取决于短的那个数组）。

参数：

- other: [Array](core_package_structs.md#struct-arrayt)\<R> - 要合并的其中一个数组。

返回值：

- [Array](core_package_structs.md#struct-arrayt)\<(T, R)> - 返回一个新数组。

示例：

<!-- verify -->
```cangjie
main() {
    let arr1 = [1, 2, 3, 4]
    let arr2 = ["apple", "banana", "cherry", "date"]

    println("数组1: ${arr1}")
    println("数组2: ${arr2}")

    // 使用zip合并两个数组
    let zipped = arr1.zip(arr2)
    println("合并后的数组:")
    for (pair in zipped) {
        let (index, value) = pair
        println(" (${index}, ${value})")
    }
    return 0
}
```

运行结果：

```text
数组1: [1, 2, 3, 4]
数组2: [apple, banana, cherry, date]
合并后的数组:
 (1, apple)
 (2, banana)
 (3, cherry)
 (4, date)
```

<!--DelEnd-->

## extend\<T> Array\<T> <: Collection\<T>

```cangjie
extend<T> Array<T> <: Collection<T>
```

功能：为 [Array](core_package_structs.md#struct-arrayt)\<T> 类型实现 [Collection](core_package_interfaces.md#interface-collectiont) 接口。

被扩展类型：

- [Array\<T>](core_package_structs.md#struct-arrayt)

扩展接口：

- [Collection](core_package_interfaces.md#interface-collectiont)\<T>

### prop size

```cangjie
public prop size: Int64
```

功能：获取元素数量。

类型：[Int64](core_package_intrinsics.md#int64)

示例：

<!-- verify -->
```cangjie
main() {
    let arr = [1, 2, 3, 4, 5]

    println("Array: ${arr}")
    println("Size of array: ${arr.size}")

    // 创建一个空数组
    let emptyArr: Array<Int64> = []

    println("Empty array: ${emptyArr}")
    println("Size of empty array: ${emptyArr.size}")

    // 创建一个包含字符串的数组
    let strArr = ["Hello", "World", "Cangjie", "Language"]

    println("String array: ${strArr}")
    println("Size of string array: ${strArr.size}")
}
```

运行结果：

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

功能：判断数组是否为空。

返回值：

- [Bool](core_package_intrinsics.md#bool) - 如果数组为空，返回 true，否则，返回 false。

示例：

<!-- verify -->
```cangjie
main() {
    let arr = [1, 2, 3, 4, 5]

    println("Array: ${arr}")
    println("Is array empty? ${arr.isEmpty()}")

    // 创建一个空数组
    let emptyArr: Array<Int64> = []

    println("Empty array: ${emptyArr}")
    println("Is empty array empty? ${emptyArr.isEmpty()}")

    // 创建一个包含字符串的数组
    let strArr = ["Hello", "World"]

    println("String array: ${strArr}")
    println("Is string array empty? ${strArr.isEmpty()}")
}
```

运行结果：

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

功能：获取当前数组的迭代器，用于遍历数组。

返回值：

- [Iterator](core_package_classes.md#class-iteratort)\<T> - 当前数组的迭代器。

示例：

<!-- verify -->
```cangjie
main() {
    let arr = [1, 2, 3, 4, 5]

    println("Array: ${arr}")

    // 获取数组的迭代器
    let iter = arr.iterator()

    println("Using iterator to traverse the array:")

    // 使用迭代器遍历数组
    while (true) {
        match (iter.next()) {
            case Some(value) => println("Element: ${value}")
            case None => break
        }
    }

    // 创建另一个数组并演示迭代器的其他用法
    let arr2 = ["Hello", "World", "Cangjie"]
    let iter2 = arr2.iterator()

    println("\nSecond array: ${arr2}")
    println("Using iterator with for-in loop:")

    // 也可以使用for-in循环遍历迭代器
    for (element in iter2) {
        println("Element: ${element}")
    }
}
```

运行结果：

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

功能：根据当前 [Array](core_package_structs.md#struct-arrayt) 实例拷贝一个新的 [Array](core_package_structs.md#struct-arrayt) 实例。

返回值：

- [Array](core_package_structs.md#struct-arrayt)\<T> - 拷贝得到的新的 [Array](core_package_structs.md#struct-arrayt) 实例。

示例：

<!-- verify -->
```cangjie
main() {
    let arr = [1, 2, 3, 4, 5]

    println("Original array: ${arr}")

    // 使用toArray()方法创建数组的副本
    let copiedArr = arr.toArray()

    println("Copied array: ${copiedArr}")

    // 验证拷贝后的数组是独立的
    copiedArr[0] = 10
    println("After modifying copied array:")
    println("Original array: ${arr}")
    println("Copied array: ${copiedArr}")

    // 创建一个包含字符串的数组并测试toArray()
    let strArr = ["Hello", "World", "Cangjie"]
    let copiedStrArr = strArr.toArray()

    println("\nString array: ${strArr}")
    println("Copied string array: ${copiedStrArr}")

    // 验证拷贝后的字符串数组是独立的
    copiedStrArr[0] = "Hi"
    println("After modifying copied string array:")
    println("Original string array: ${strArr}")
    println("Copied string array: ${copiedStrArr}")
}
```

运行结果：

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

功能：为 [Array](core_package_structs.md#struct-arrayt)\<T> 类型扩展 [Equatable](core_package_interfaces.md#interface-equatablet)\<[Array](core_package_structs.md#struct-arrayt)\<T>> 接口实现，支持判等操作。

被扩展类型：

- [Array\<T>](core_package_structs.md#struct-arrayt)

扩展接口：

- [Equatable](core_package_interfaces.md#interface-equatablet)\<[Array](core_package_structs.md#struct-arrayt)\<T>>

### func contains(T)

```cangjie
public func contains(element: T): Bool
```

功能：查找当前数组是否包含指定元素。

参数：

- element: T - 需要查找的目标元素。

返回值：

- [Bool](core_package_intrinsics.md#bool) - 如果存在，则返回 true，否则返回 false。

示例：

<!-- verify -->
```cangjie
main(): Int64 {
    let arr = [1, 2, 3, 4]
    println(arr.contains(1))
    return 0
}
```

运行结果：

```text
true
```

### func indexOf(Array\<T>)

```cangjie
public func indexOf(elements: Array<T>): Option<Int64>
```

功能：返回数组中子数组 `elements` 出现的第一个位置，如果数组中不包含此数组，返回 None。

> **注意：**
>
> 当 T 的类型是 [Int64](core_package_intrinsics.md#int64) 时，此函数的变长参数语法糖版本可能会和 `public func indexOf(element: T, fromIndex: Int64): Option<Int64>` 产生歧义，根据优先级，当参数数量是 2 个时，会优先调用 `public func indexOf(element: T, fromIndex: Int64): Option<Int64>`。

参数：

- elements: [Array](core_package_structs.md#struct-arrayt)\<T> - 需要定位的目标数组。

返回值：

- [Option](core_package_enums.md#enum-optiont)\<[Int64](core_package_intrinsics.md#int64)> - 数组中子数组 `elements` 出现的第一个位置，如果数组中不包含此数组，返回 None。

示例：

<!-- verify -->
```cangjie
main(): Int64 {
    let arr = [1, 2, 3, 4]
    let subArr = [2, 3]
    println(arr.indexOf(subArr))
    return 0
}
```

运行结果：

```text
Some(1)
```

### func indexOf(Array\<T>, Int64)

```cangjie
public func indexOf(elements: Array<T>, fromIndex: Int64): Option<Int64>
```

功能：返回数组中在 `fromIndex` 之后，子数组 `elements` 出现的第一个位置，未找到返回 None。

函数会对 `fromIndex` 范围进行检查，`fromIndex` 小于 0 时，将会从第 0 位开始搜索，当 `fromIndex` 大于等于本数组的大小时，结果为 None。

参数：

- elements: [Array](core_package_structs.md#struct-arrayt)\<T> - 需要定位的元素。
- fromIndex: [Int64](core_package_intrinsics.md#int64) - 开始搜索的起始位置。

返回值：

- [Option](core_package_enums.md#enum-optiont)\<[Int64](core_package_intrinsics.md#int64)> - 数组中在 `fromIndex` 之后，子数组 `elements` 出现的第一个位置，未找到返回 None。

示例：

<!-- verify -->
```cangjie
main(): Int64 {
    let arr = [1, 2, 3, 4, 2, 3]
    let subArr = [2, 3]
    println(arr.indexOf(subArr, 3))
    return 0
}
```

运行结果：

```text
Some(4)
```

### func indexOf(T)

```cangjie
public func indexOf(element: T): Option<Int64>
```

功能：获取数组中 `element` 出现的第一个位置，如果数组中不包含此元素，返回 None。

参数：

- element: T - 需要定位的元素。

返回值：

- [Option](core_package_enums.md#enum-optiont)\<[Int64](core_package_intrinsics.md#int64)> - 数组中 `element` 出现的第一个位置，如果数组中不包含此元素，返回 None。

示例：

<!-- verify -->
```cangjie
main() {
    let arr = [1, 2, 3, 4, 2, 5]

    println("Array: ${arr}")

    // 查找元素2第一次出现的位置
    let index1 = arr.indexOf(2)
    println("First occurrence of 2: ${index1}")

    // 查找元素6第一次出现的位置（不存在）
    let index2 = arr.indexOf(6)
    println("First occurrence of 6: ${index2}")

    // 创建一个包含字符串的数组并测试indexOf
    let strArr = ["Hello", "World", "Hello", "Cangjie"]

    println("\nString array: ${strArr}")

    // 查找字符串"Hello"第一次出现的位置
    let strIndex1 = strArr.indexOf("Hello")
    println("First occurrence of \"Hello\": ${strIndex1}")
}
```

运行结果：

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

功能：返回数组中在 `fromIndex` 之后， `element` 出现的第一个位置，未找到返回 None。

函数会从下标 `fromIndex` 开始查找，`fromIndex` 小于 0 时，将会从第 0 位开始搜索，当 `fromIndex` 大于等于本数组的大小时，结果为 None。

参数：

- element: T - 需要定位的元素。
- fromIndex: [Int64](core_package_intrinsics.md#int64) - 查找的起始位置。

返回值：

- [Option](core_package_enums.md#enum-optiont)\<[Int64](core_package_intrinsics.md#int64)> - 返回数组中在 `fromIndex` 之后， `element` 出现的第一个位置，未找到返回 None。

示例：

<!-- verify -->
```cangjie
main() {
    let arr = [1, 2, 3, 4, 2, 5]

    println("Array: ${arr}")

    // 从索引2开始查找元素2第一次出现的位置
    let index1 = arr.indexOf(2, 2)
    println("First occurrence of 2 starting from index 2: ${index1}")

    // 从索引5开始查找元素2第一次出现的位置（不存在）
    let index2 = arr.indexOf(2, 5)
    println("First occurrence of 2 starting from index 5: ${index2}")

    // 创建一个包含字符串的数组并测试indexOf
    let strArr = ["Hello", "World", "Hello", "Cangjie"]

    println("\nString array: ${strArr}")

    // 从索引1开始查找字符串"Hello"第一次出现的位置
    let strIndex = strArr.indexOf("Hello", 1)
    println("First occurrence of \"Hello\" starting from index 1: ${strIndex}")
}
```

运行结果：

```text
Array: [1, 2, 3, 4, 2, 5]
First occurrence of 2 starting from index 2: Some(4)
First occurrence of 2 starting from index 5: None

String array: [Hello, World, Hello, Cangjie]
First occurrence of "Hello" starting from index 1: Some(2)
```

### func lastIndexOf(Array\<T>)

```cangjie
public func lastIndexOf(elements: Array<T>): Option<Int64>
```

功能：返回数组中子数组 `elements` 出现的最后一个位置，如果数组中不存在此子数组，返回 None。

参数：

- elements: [Array](core_package_structs.md#struct-arrayt)\<T> - 需要定位的目标数组。

返回值：

- [Option](core_package_enums.md#enum-optiont)\<[Int64](core_package_intrinsics.md#int64)> - 数组中 `elements` 出现的最后一个位置，如果数组中不存在此子数组，返回 None。

示例：

<!-- verify -->
```cangjie
main() {
    let arr = [1, 2, 3, 2, 3, 4, 2, 3]
    let subArr = [2, 3]

    println("Array: ${arr}")
    println("Sub-array: ${subArr}")

    // 查找子数组[2, 3]最后一次出现的位置
    let lastIndex = arr.lastIndexOf(subArr)
    println("Last occurrence of [2, 3]: ${lastIndex}")

    // 查找不存在的子数组
    let notExist = [5, 6]
    let notExistIndex = arr.lastIndexOf(notExist)
    println("Last occurrence of [5, 6]: ${notExistIndex}")
}
```

运行结果：

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

功能：从 `fromIndex` 开始向后搜索，返回数组中子数组 `elements` 出现的最后一个位置，如果数组中不存在此子数组，返回 None。

函数会对 `fromIndex` 范围进行检查，`fromIndex` 小于 0 时，将会从第 0 位开始搜索，当 `fromIndex` 大于等于本数组的大小时，结果为 None。

参数：

- elements: [Array](core_package_structs.md#struct-arrayt)\<T> - 需要定位的目标数组。
- fromIndex: [Int64](core_package_intrinsics.md#int64) - 搜索开始的位置。

返回值：

- [Option](core_package_enums.md#enum-optiont)\<[Int64](core_package_intrinsics.md#int64)> - 从 `fromIndex` 开始向后搜索，数组中子数组 `elements` 出现的最后一个位置，如果数组中不存在此子数组，返回 None。

示例：

<!-- verify -->
```cangjie
main() {
    let arr = [1, 2, 3, 2, 3, 4, 3, 3]
    let subArr = [2, 3]

    println("Array: ${arr}")
    println("Sub-array: ${subArr}")

    // 从索引4开始向后搜索，查找子数组[2, 3]最后一次出现的位置
    let lastIndex = arr.lastIndexOf(subArr, 4)
    println("Last occurrence of [2, 3] starting from index 4: ${lastIndex}")

    // 从索引2开始向后搜索，查找子数组[2, 3]最后一次出现的位置
    let lastIndex2 = arr.lastIndexOf(subArr, 2)
    println("Last occurrence of [2, 3] starting from index 2: ${lastIndex2}")
}
```

运行结果：

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

功能：返回数组中 `element` 出现的最后一个位置，如果数组中不存在此元素，返回 None。

参数：

- element: T - 需要定位的目标元素。

返回值：

- [Option](core_package_enums.md#enum-optiont)\<[Int64](core_package_intrinsics.md#int64)> - 数组中 `element` 出现的最后一个位置，如果数组中不存在此元素，返回 None。

示例：

<!-- verify -->
```cangjie
main() {
    let arr = [1, 2, 3, 4, 2, 5, 2]

    println("Array: ${arr}")

    // 查找元素2最后一次出现的位置
    let lastIndex = arr.lastIndexOf(2)
    println("Last occurrence of 2: ${lastIndex}")

    // 查找元素6最后一次出现的位置（不存在）
    let notExistIndex = arr.lastIndexOf(6)
    println("Last occurrence of 6: ${notExistIndex}")

    // 创建一个包含字符串的数组并测试lastIndexOf
    let strArr = ["Hello", "World", "Hello", "Cangjie", "Hello"]

    println("\nString array: ${strArr}")

    // 查找字符串"Hello"最后一次出现的位置
    let strLastIndex = strArr.lastIndexOf("Hello")
    println("Last occurrence of \"Hello\": ${strLastIndex}")
}
```

运行结果：

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

功能：从 `fromIndex` 开始向后搜索，返回数组中 `element` 出现的最后一个位置，如果数组中不存在此元素，返回 None。

函数会对 `fromIndex` 范围进行检查，`fromIndex` 小于 0 时，将会从第 0 位开始搜索，当 `fromIndex` 大于等于本数组的大小时，结果为 None。

参数：

- element: T - 需要定位的目标元素。
- fromIndex: [Int64](core_package_intrinsics.md#int64) - 搜索开始的位置。

返回值：

- [Option](core_package_enums.md#enum-optiont)\<[Int64](core_package_intrinsics.md#int64)> - 从 `fromIndex` 开始向后搜索，返回数组中 `element` 出现的最后一个位置，如果数组中不存在此元素，返回 None。

示例：

<!-- verify -->
```cangjie
main() {
    let arr = [1, 2, 3, 4, 2, 5, 3]

    println("Array: ${arr}")

    // 从索引5开始向后搜索，查找元素2最后一次出现的位置
    let lastIndex = arr.lastIndexOf(2, 5)
    println("Last occurrence of 2 starting from index 5: ${lastIndex}")

    // 从索引3开始向后搜索，查找元素2最后一次出现的位置
    let lastIndex2 = arr.lastIndexOf(2, 3)
    println("Last occurrence of 2 starting from index 3: ${lastIndex2}")

    // 创建一个包含字符串的数组并测试lastIndexOf
    let strArr = ["Hello", "World", "Hello", "Cangjie", "Hello"]

    println("\nString array: ${strArr}")

    // 从索引2开始向后搜索，查找字符串"Hello"最后一次出现的位置
    let strLastIndex = strArr.lastIndexOf("Hello", 2)
    println("Last occurrence of \"Hello\" starting from index 2: ${strLastIndex}")
}
```

运行结果：

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

功能：删除前缀。

如果当前数组开头与 prefix 完全匹配，删除其前缀。返回值为当前数组删除前缀后得到的切片。

参数：

- prefix: [Array](./core_package_structs.md#struct-arrayt)\<T> - 待删除的前缀。

返回值：

- [Array](./core_package_structs.md#struct-arrayt)\<T> - 删除前缀后得到的原数组切片。

示例：

<!-- verify -->
```cangjie
main(): Int64 {
    let arr = [1, 2, 1, 2, 3].removePrefix([1, 2])
    println(arr)
    return 0
}
```

运行结果：

```text
[1, 2, 3]
```

### func removeSuffix(Array\<T>)

```cangjie
public func removeSuffix(suffix: Array<T>): Array<T>
```

功能：删除后缀。

如果当前数组结尾与 suffix 完全匹配，删除其后缀。返回值为当前数组删除后缀后得到的切片。

参数：

- suffix: [Array](./core_package_structs.md#struct-arrayt)\<T> - 待删除的后缀。

返回值：

- [Array](./core_package_structs.md#struct-arrayt)\<T> - 删除后缀后得到的原数组切片。

示例：

<!-- verify -->
```cangjie
main(): Int64 {
    let arr = [1, 2, 3, 2, 3].removeSuffix([2, 3])
    println(arr)
    return 0
}
```

运行结果：

```text
[1, 2, 3]
```

### func trimEnd((T)->Bool)

```cangjie
public func trimEnd(predicate: (T)->Bool): Array<T>
```

功能：修剪当前数组，从尾开始删除满足过滤条件的元素，直到第一个不满足过滤条件的元素为止，并返回当前数组的切片。

参数：

- predicate: (T)->[Bool](./core_package_intrinsics.md#bool) - 过滤条件。

返回值：

- [Array](core_package_structs.md#struct-arrayt)\<T> - 当前数组的切片。

示例：

<!-- verify -->
```cangjie
main() {
    let arr = [1, 2, 3, 4, 5, 4, 3, 2, 1]

    println("Original array: ${arr}")

    // 从尾部开始删除所有小于4的元素
    let trimmedArr = arr.trimEnd({x => x < 4})
    println("Array after trimming elements less than 4 from the end: ${trimmedArr}")

    // 创建一个包含字符串的数组并测试trimEnd
    let strArr = ["a", "b", "c", "d", "c", "b", "a"]

    println("\nString array: ${strArr}")

    // 从尾部开始删除所有不等于"c"的元素
    let trimmedStrArr = strArr.trimEnd({s => s != "c"})
    println("String array after trimming elements not equal to \"c\" from the end: ${trimmedStrArr}")
}
```

运行结果：

```text
Original array: [1, 2, 3, 4, 5, 4, 3, 2, 1]
Array after trimming elements less than 4 from the end: [1, 2, 3, 4, 5, 4]

String array: [a, b, c, d, c, b, a]
String array after trimming elements not equal to "c" from the end: [a, b, c, d, c]
```

### func trimEnd(Array\<T>)

```cangjie
public func trimEnd(set: Array<T>): Array<T>
```

功能：修剪当前数组，从尾开始删除在指定集合 set 中的元素，直到第一个不在 set 中的元素为止，并返回当前数组的切片。

参数：

- set: [Array](core_package_structs.md#struct-arrayt)\<T> - 待删除的元素的集合。

返回值：

- [Array](core_package_structs.md#struct-arrayt)\<T> - 当前数组的切片。

示例：

<!-- verify -->
```cangjie
main(): Int64 {
    let arr = [2, 1, 2, 2, 3].trimEnd([2, 3])
    println(arr)
    return 0
}
```

运行结果：

```text
[2, 1]
```

### func trimStart((T)->Bool)

```cangjie
public func trimStart(predicate: (T)->Bool): Array<T>
```

功能：修剪当前数组，从头开始删除满足过滤条件的元素，直到第一个不满足过滤条件的元素为止，并返回当前数组的切片。

> **注意：**
>
> 切片不会对数组数据进行拷贝，是对原数据特定区间的引用，修改切片中的元素会影响原数组中对应位置的元素。

参数：

- predicate: (T)->[Bool](./core_package_intrinsics.md#bool) - 过滤条件。

返回值：

- [Array](core_package_structs.md#struct-arrayt)\<T> - 当前数组的切片。

示例：

<!-- verify -->
```cangjie
main() {
    let arr = [1, 2, 3, 4, 5, 4, 3, 2, 1]

    println("Original array: ${arr}")

    // 从头部开始删除所有小于4的元素
    let trimmedArr = arr.trimStart({x => x < 4})
    println("Array after trimming elements less than 4 from the start: ${trimmedArr}")

    // 创建一个包含字符串的数组并测试trimStart
    let strArr = ["a", "b", "c", "d", "c", "b", "a"]

    println("\nString array: ${strArr}")

    // 从头部开始删除所有不等于"c"的元素
    let trimmedStrArr = strArr.trimStart({s => s != "c"})
    println("String array after trimming elements not equal to \"c\" from the start: ${trimmedStrArr}")
}
```

运行结果：

```text
Original array: [1, 2, 3, 4, 5, 4, 3, 2, 1]
Array after trimming elements less than 4 from the start: [4, 5, 4, 3, 2, 1]

String array: [a, b, c, d, c, b, a]
String array after trimming elements not equal to "c" from the start: [c, d, c, b, a]
```

### func trimStart(Array\<T>)

```cangjie
public func trimStart(set: Array<T>): Array<T>
```

功能：修剪当前数组，从头开始删除在指定集合 set 中的元素，直到第一个不在 set 中的元素为止，并返回当前数组的切片。

> **注意：**
>
> 切片不会对数组数据进行拷贝，是对原数据特定区间的引用，修改切片中的元素会影响原数组中对应位置的元素。

参数：

- set: [Array](core_package_structs.md#struct-arrayt)\<T> - 待删除的元素的集合。

返回值：

- [Array](core_package_structs.md#struct-arrayt)\<T> - 当前数组的切片。

示例：

<!-- verify -->
```cangjie
main(): Int64 {
    let arr = [1, 2, 1, 3, 1].trimStart([1, 2])
    println(arr)
    return 0
}
```

运行结果：

```text
[3, 1]
```

### operator func !=(Array\<T>)

```cangjie
public operator const func !=(other: Array<T>): Bool
```

功能：判断当前实例与指定 [Array](core_package_structs.md#struct-arrayt)\<T> 实例是否不等。

参数：

- other: [Array](core_package_structs.md#struct-arrayt)\<T> - 用于与当前实例比较的另一个 [Array](core_package_structs.md#struct-arrayt)\<T> 实例。

返回值：

- [Bool](core_package_intrinsics.md#bool) - 如果不相等，则返回 true；相等则返回 false。

示例：

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

    // 比较相等的数组
    println("arr1 != arr2: ${arr1 != arr2}")

    // 比较不相等的数组（元素不同）
    println("arr1 != arr3: ${arr1 != arr3}")

    // 比较不相等的数组（长度不同）
    println("arr1 != arr4: ${arr1 != arr4}")

    // 创建包含字符串的数组并测试!=操作符
    let strArr1 = ["Hello", "World"]
    let strArr2 = ["Hello", "World"]
    let strArr3 = ["Hello", "Cangjie"]

    println("\nString array 1: ${strArr1}")
    println("String array 2: ${strArr2}")
    println("String array 3: ${strArr3}")

    // 比较相等的字符串数组
    println("strArr1 != strArr2: ${strArr1 != strArr2}")

    // 比较不相等的字符串数组
    println("strArr1 != strArr3: ${strArr1 != strArr3}")
}
```

运行结果：

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
public operator const func ==(other: Array<T>): Bool
```

功能：判断当前实例与指定 [Array](core_package_structs.md#struct-arrayt)\<T> 实例是否相等。

两个 [Array](core_package_structs.md#struct-arrayt)\<T> 相等指的是其中的每个元素都相等。

参数：

- other: [Array](core_package_structs.md#struct-arrayt)\<T> - 用于与当前实例比较的另一个 [Array](core_package_structs.md#struct-arrayt)\<T> 实例。

返回值：

- [Bool](core_package_intrinsics.md#bool) - 如果相等，则返回 true，否则返回 false。

示例：

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

    // 比较相等的数组
    println("arr1 == arr2: ${arr1 == arr2}")

    // 比较不相等的数组（元素不同）
    println("arr1 == arr3: ${arr1 == arr3}")

    // 比较不相等的数组（长度不同）
    println("arr1 == arr4: ${arr1 == arr4}")

    // 创建包含字符串的数组并测试==操作符
    let strArr1 = ["Hello", "World"]
    let strArr2 = ["Hello", "World"]
    let strArr3 = ["Hello", "Cangjie"]

    println("\nString array 1: ${strArr1}")
    println("String array 2: ${strArr2}")
    println("String array 3: ${strArr3}")

    // 比较相等的字符串数组
    println("strArr1 == strArr2: ${strArr1 == strArr2}")

    // 比较不相等的字符串数组
    println("strArr1 == strArr3: ${strArr1 == strArr3}")
}
```

运行结果：

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

功能：为 [Array](core_package_structs.md#struct-arrayt)\<T> 类型扩展 [ToString](core_package_interfaces.md#interface-tostring) 接口，支持转字符串操作。

被扩展类型：

- [Array\<T>](core_package_structs.md#struct-arrayt)

扩展接口：

- [ToString](core_package_interfaces.md#interface-tostring)

### func toString()

```cangjie
public func toString(): String
```

功能：将数组转换为字符串。

字符串形如 "[1, 2, 3, 4, 5]"

返回值：

- [String](core_package_structs.md#struct-string) - 转化后的字符串。

示例：

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

    // 创建二维数组并测试toString()
    let arr4 = [[1, 2], [3, 4], [5, 6]]

    println("\n2D array: ${arr4}")
    println("toString() result: ${arr4.toString()}")
}
```

运行结果：

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

功能：为 [Box](core_package_classes.md#class-boxt)\<T> 类扩展 [Comparable](core_package_interfaces.md#interface-comparablet)\<[Box](core_package_classes.md#class-boxt)\<T>> 接口，提供比较大小的能力。

被扩展类型：

- [Box\<T>](core_package_classes.md#class-boxt)

扩展接口：

- [Comparable](core_package_interfaces.md#interface-comparablet)\<[Box](core_package_classes.md#class-boxt)\<T>>

### func compare(Box\<T>)

```cangjie
public func compare(other: Box<T>): Ordering
```

功能：判断当前 [Box](core_package_classes.md#class-boxt) 实例与另一个 [Box](core_package_classes.md#class-boxt) 实例的大小关系。

参数：

- other: [Box](core_package_classes.md#class-boxt)\<T> - 比较的另外一个 [Box](core_package_classes.md#class-boxt) 对象。

返回值：

- [Ordering](core_package_enums.md#enum-ordering) - 如果当前 [Box](core_package_classes.md#class-boxt) 实例大于 that，返回 [Ordering](core_package_enums.md#enum-ordering).GT，等于返回 [Ordering](core_package_enums.md#enum-ordering).EQ，小于返回 [Ordering](core_package_enums.md#enum-ordering).LT。

示例：

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

运行结果：

```text
Ordering.GT
```

### operator func !=(Box\<T>)

```cangjie
public operator func !=(other: Box<T>): Bool
```

功能：比较 [Box](core_package_classes.md#class-boxt) 对象是否不相等。

参数：

- other: [Box](core_package_classes.md#class-boxt)\<T> - 比较的另外一个 [Box](core_package_classes.md#class-boxt) 对象。

返回值：

- [Bool](core_package_intrinsics.md#bool) - 当前 [Box](core_package_classes.md#class-boxt) 对象不等于参数 [Box](core_package_classes.md#class-boxt) 对象返回 true，否则返回 false。

示例：

<!-- verify -->
```cangjie
main() {
    var box1: Box<Int64> = Box<Int64>(42)
    var box2: Box<Int64> = Box<Int64>(42)
    var box3: Box<Int64> = Box<Int64>(100)

    // 比较相等的Box对象
    println("box1 != box2: ${box1 != box2}")

    // 比较不相等的Box对象
    println("box1 != box3: ${box1 != box3}")
}
```

运行结果：

```text
box1 != box2: false
box1 != box3: true
```

### operator func <(Box\<T>)

```cangjie
public operator func <(other: Box<T>): Bool
```

功能：比较 [Box](core_package_classes.md#class-boxt) 对象的大小。

参数：

- other: [Box](core_package_classes.md#class-boxt)\<T> - 比较的另外一个 [Box](core_package_classes.md#class-boxt) 对象。

返回值：

- [Bool](core_package_intrinsics.md#bool) - 当前 [Box](core_package_classes.md#class-boxt) 对象小于参数 [Box](core_package_classes.md#class-boxt) 对象返回 true，否则返回 false。

示例：

<!-- verify -->
```cangjie
main() {
    var box1: Box<Int64> = Box<Int64>(42)
    var box2: Box<Int64> = Box<Int64>(100)

    // 比较Box对象的大小
    println("box1 < box2: ${box1 < box2}")
    println("box2 < box1: ${box2 < box1}")
    println("box1 < box1: ${box1 < box1}")
}
```

运行结果：

```text
box1 < box2: true
box2 < box1: false
box1 < box1: false
```

### operator func <=(Box\<T>)

```cangjie
public operator func <=(other: Box<T>): Bool
```

功能：比较 [Box](core_package_classes.md#class-boxt) 对象的大小。

参数：

- other: [Box](core_package_classes.md#class-boxt)\<T> - 比较的另外一个 [Box](core_package_classes.md#class-boxt) 对象。

返回值：

- [Bool](core_package_intrinsics.md#bool) - 当前 [Box](core_package_classes.md#class-boxt) 对象小于等于参数 [Box](core_package_classes.md#class-boxt) 对象返回 true，否则返回 false。

示例：

<!-- verify -->
```cangjie
main() {
    var box1: Box<Int64> = Box<Int64>(42)
    var box2: Box<Int64> = Box<Int64>(100)
    var box3: Box<Int64> = Box<Int64>(42)

    // 比较Box对象的大小
    println("box1 <= box2: ${box1 <= box2}")
    println("box2 <= box1: ${box2 <= box1}")
    println("box1 <= box3: ${box1 <= box3}")
}
```

运行结果：

```text
box1 <= box2: true
box2 <= box1: false
box1 <= box3: true
```

### operator func ==(Box\<T>)

```cangjie
public operator func ==(other: Box<T>): Bool
```

功能：比较 [Box](core_package_classes.md#class-boxt) 对象是否相等。

参数：

- other: [Box](core_package_classes.md#class-boxt)\<T> - 比较的另外一个 [Box](core_package_classes.md#class-boxt) 对象。

返回值：

- [Bool](core_package_intrinsics.md#bool) - 当前 [Box](core_package_classes.md#class-boxt) 对象等于参数 [Box](core_package_classes.md#class-boxt) 对象返回 true，否则返回 false。

示例：

<!-- verify -->
```cangjie
main() {
    var box1: Box<Int64> = Box<Int64>(42)
    var box2: Box<Int64> = Box<Int64>(42)
    var box3: Box<Int64> = Box<Int64>(100)

    // 比较相等的Box对象
    println("box1 == box2: ${box1 == box2}")

    // 比较不相等的Box对象
    println("box1 == box3: ${box1 == box3}")
}
```

运行结果：

```text
box1 == box2: true
box1 == box3: false
```

### operator func >(Box\<T>)

```cangjie
public operator func >(other: Box<T>): Bool
```

功能：比较 [Box](core_package_classes.md#class-boxt) 对象的大小。

参数：

- other: [Box](core_package_classes.md#class-boxt)\<T> - 比较的另外一个 [Box](core_package_classes.md#class-boxt) 对象。

返回值：

- [Bool](core_package_intrinsics.md#bool) - 当前 [Box](core_package_classes.md#class-boxt) 对象大于参数 [Box](core_package_classes.md#class-boxt) 对象返回 true，否则返回 false。

示例：

<!-- verify -->
```cangjie
main() {
    var box1: Box<Int64> = Box<Int64>(42)
    var box2: Box<Int64> = Box<Int64>(100)

    // 比较Box对象的大小
    println("box1 > box2: ${box1 > box2}")
    println("box2 > box1: ${box2 > box1}")
    println("box1 > box1: ${box1 > box1}")
}
```

运行结果：

```text
box1 > box2: false
box2 > box1: true
box1 > box1: false
```

### operator func >=(Box\<T>)

```cangjie
public operator func >=(other: Box<T>): Bool
```

功能：比较 [Box](core_package_classes.md#class-boxt) 对象的大小。

参数：

- other: [Box](core_package_classes.md#class-boxt)\<T> - 比较的另外一个 [Box](core_package_classes.md#class-boxt) 对象。

返回值：

- [Bool](core_package_intrinsics.md#bool) - 当前 [Box](core_package_classes.md#class-boxt) 对象大于等于参数 [Box](core_package_classes.md#class-boxt) 对象返回 true，否则返回 false。

示例：

<!-- verify -->
```cangjie
main() {
    var box1: Box<Int64> = Box<Int64>(42)
    var box2: Box<Int64> = Box<Int64>(100)
    var box3: Box<Int64> = Box<Int64>(42)

    // 比较Box对象的大小
    println("box1 >= box2: ${box1 >= box2}")
    println("box2 >= box1: ${box2 >= box1}")
    println("box1 >= box3: ${box1 >= box3}")
}
```

运行结果：

```text
box1 >= box2: false
box2 >= box1: true
box1 >= box3: true
```

## extend\<T> Box\<T> <: Hashable where T <: Hashable

```cangjie
extend<T> Box<T> <: Hashable where T <: Hashable
```

功能：为 [Box](core_package_classes.md#class-boxt)\<T> 类扩展 [Hashable](core_package_interfaces.md#interface-hashable) 接口，支持计算哈希值。

被扩展类型：

- [Box\<T>](core_package_classes.md#class-boxt)

扩展接口：

- [Hashable](core_package_interfaces.md#interface-hashable)

### func hashCode()

```cangjie
public func hashCode(): Int64
```

功能：获取 [Box](core_package_classes.md#class-boxt) 对象的哈希值。

实际上该值为 [Box](core_package_classes.md#class-boxt) 中封装的 `T` 类型实例的哈希值。

返回值：

- [Int64](core_package_intrinsics.md#int64) - 当前 [Box](core_package_classes.md#class-boxt) 对象的哈希值。

示例：

<!-- verify -->
```cangjie
main() {
    var box1: Box<Int64> = Box<Int64>(42)
    var box2: Box<Int64> = Box<Int64>(42)
    var box3: Box<Int64> = Box<Int64>(100)

    // 获取Box对象的哈希值
    println("box1 hashCode: ${box1.hashCode()}")
    println("box2 hashCode: ${box2.hashCode()}")
    println("box3 hashCode: ${box3.hashCode()}")

    // 相同值的Box对象具有相同的哈希值
    println("box1 and box2 have same hashCode: ${box1.hashCode() == box2.hashCode()}")
}
```

运行结果：

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

功能：为 [Box](core_package_classes.md#class-boxt)\<T> 类型扩展 [ToString](core_package_interfaces.md#interface-tostring) 接口，支持转字符串操作。

被扩展类型：

- [Box\<T>](core_package_classes.md#class-boxt)

扩展接口：

- [ToString](core_package_interfaces.md#interface-tostring)

### func toString()

```cangjie
public func toString(): String
```

功能：获取 [Box](core_package_classes.md#class-boxt) 对象的字符串表示，字符串内容为当前实例封装的 `T` 类型实例的字符串表示。

返回值：

- [String](core_package_structs.md#struct-string) - 转换后的字符串。

示例：

<!-- verify -->
```cangjie
main() {
    var intBox: Box<Int64> = Box<Int64>(42)
    var stringBox: Box<String> = Box<String>("Hello")
    var boolBox: Box<Bool> = Box<Bool>(true)

    // 获取Box对象的字符串表示
    println("intBox as string: ${intBox.toString()}")
    println("stringBox as string: ${stringBox.toString()}")
    println("boolBox as string: ${boolBox.toString()}")
}
```

运行结果：

```text
intBox as string: 42
stringBox as string: Hello
boolBox as string: true
```

## extend\<T> CPointer\<T>

```cangjie
extend<T> CPointer<T>
```

功能：为 [CPointer](core_package_intrinsics.md#cpointert)\<T> 扩展一些必要的指针使用相关接口，包含判空、读写数据等接口。

被扩展类型：

- [CPointer\<T>](core_package_intrinsics.md#cpointert)

### func asResource()

```cangjie
public func asResource(): CPointerResource<T>
```

功能：获取该指针 [CPointerResource](core_package_structs.md#struct-cpointerresourcet-where-t--ctype) 实例，该实例可以在 `try-with-resource` 语法上下文中实现内容自动释放。

返回值：

- [CPointerResource](core_package_structs.md#struct-cpointerresourcet-where-t--ctype)\<T> - 当前指针对应的 [CPointerResource](core_package_structs.md#struct-cpointerresourcet-where-t--ctype) 实例。

示例：

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

运行结果：

```text
10
true
```

### func isNotNull()

```cangjie
public func isNotNull(): Bool
```

功能：判断指针是否不为空。

返回值：

- [Bool](core_package_intrinsics.md#bool) - 如果不为空返回 true，否则返回 false。

示例：

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

运行结果：

```text
false
true
```

### func isNull()

```cangjie
public func isNull(): Bool
```

功能：判断指针是否为空。

返回值：

- [Bool](core_package_intrinsics.md#bool) - 如果为空返回 true，否则返回 false。

示例：

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

运行结果：

```text
10
```

### func read()

```cangjie
public unsafe func read(): T
```

功能：读取第一个数据，该接口需要用户保证指针的合法性，否则发生未定义行为。

返回值：

- T - 该对象类型的第一个数据。

示例：

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

运行结果：

```text
true
false
```

### func read(Int64)

```cangjie
public unsafe func read(idx: Int64): T
```

功能：根据下标读取对应的数据，该接口需要用户保证指针的合法性，否则发生未定义行为。

参数：

- idx: [Int64](core_package_intrinsics.md#int64) - 要获取数据的下标。

返回值：

- T - 输入下标对应的数据。

示例：

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

运行结果：

```text
The third element of the array is 3
```

### func toUIntNative()

```cangjie
public func toUIntNative(): UIntNative
```

功能：获取该指针的整型形式。

返回值：

- [UIntNative](core_package_intrinsics.md#uintnative) - 该指针的整型形式。

示例：

<!-- run -->
```cangjie
foreign func malloc(size: UIntNative): CPointer<Unit>

foreign func free(ptr: CPointer<Unit>): Unit

main() {
    let sizeofInt64: UIntNative = 8
    var p = unsafe { malloc(sizeofInt64) }
    var p1 = unsafe { CPointer<Int64>(p) }
    unsafe { p1.write(8) }
    println("toUIntNative: ${p1.toUIntNative()}")
    unsafe { free(p) }
}
```

运行结果：

```text
toUIntNative: 94019473199456
```

### func write(Int64, T)

```cangjie
public unsafe func write(idx: Int64, value: T): Unit
```

功能：在指定下标位置写入一个数据，该接口需要用户保证指针的合法性，否则发生未定义行为。

参数：

- idx: [Int64](core_package_intrinsics.md#int64) - 指定的下标位置。
- value: T - 写入的数据。

示例：

<!-- verify -->
```cangjie
main() {
    var arr: Array<Int64> = [1, 2, 3, 4]
    var cptrHandle: CPointerHandle<Int64> = unsafe { acquireArrayRawData(arr) }

    var cptr: CPointer<Int64> = cptrHandle.pointer

    unsafe { cptr.write(2, 6) }

    println("The third element of the array is ${arr[2]} ")
    unsafe { releaseArrayRawData<Int64>(cptrHandle) }
}
```

运行结果：

```text
The third element of the array is 6
```

### func write(T)

```cangjie
public unsafe func write(value: T): Unit
```

功能：写入一个数据，该数据总是在第一个，该接口需要用户保证指针的合法性，否则发生未定义行为。

参数：

- value: T - 要写入的数据。

示例：

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

运行结果：

```text
8
```

### operator func +(Int64)

```cangjie
public unsafe operator func +(offset: Int64): CPointer<T>
```

功能：[CPointer](core_package_intrinsics.md#cpointert) 对象指针后移，同 C 语言的指针加法操作。

参数：

- offset: [Int64](core_package_intrinsics.md#int64) - 偏移量。

返回值：

- [CPointer](core_package_intrinsics.md#cpointert)\<T> - 返回偏移后的指针。

示例：

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

运行结果：

```text
1
2
```

### operator func -(Int64)

```cangjie
public unsafe operator func -(offset: Int64): CPointer<T>
```

功能：[CPointer](core_package_intrinsics.md#cpointert) 对象指针前移，同 C 语言的指针减法操作。

参数：

- offset: [Int64](core_package_intrinsics.md#int64) - 偏移量。

返回值：

- [CPointer](core_package_intrinsics.md#cpointert)\<T> - 返回偏移后的指针。

示例：

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

运行结果：

```text
1
2
1
```

## extend\<T> Iterator\<T>

```cangjie
extend<T> Iterator<T>
```

功能：扩展 [Iterator](core_package_classes.md#class-iteratort)\<T> 类型。

被扩展类型：

- [Iterator\<T>](core_package_classes.md#class-iteratort)

### func all((T) -> Bool)

```cangjie
public func all(predicate: (T)-> Bool): Bool
```

功能：判断迭代器所有元素是否都满足条件。此方法会重复获取并消耗迭代器中元素直到某个元素不满足条件。

参数：

- predicate: (T) -> [Bool](../../core/core_package_api/core_package_intrinsics.md#bool) - 给定的条件。

返回值：

- [Bool](../../core/core_package_api/core_package_intrinsics.md#bool) - 元素是否都满足条件。

示例：

<!-- verify -->

```cangjie
main(): Int64 {
    var arr: Array<Int64> = [1, 2, 3, 4, 5]

    /* 获取容器的迭代器对象 */
    var iter = arr.iterator()
    var flag: Bool = iter.all({v: Int64 => v > 0})
    println(flag)
    return 0
}
```

运行结果：

```text
true
```

### func any((T) -> Bool)

```cangjie
public func any(predicate: (T)-> Bool): Bool
```

功能：判断迭代器是否存在任意一个满足条件的元素。此方法会重复获取并消耗迭代器中元素直到某个元素满足条件。

参数：

- predicate: (T) -> [Bool](../../core/core_package_api/core_package_intrinsics.md#bool) - 给定的条件。

返回值：

- [Bool](../../core/core_package_api/core_package_intrinsics.md#bool) - 是否存在任意满足条件的元素。

示例：

<!-- verify -->

```cangjie
main(): Int64 {
    var arr: Array<Int64> = [1, 2, 3, 4, 5]

    /* 获取容器的迭代器对象 */
    var iter = arr.iterator()
    var flag: Bool = iter.any({v: Int64 => v > 4})
    println(flag)
    return 0
}
```

运行结果：

```text
true
```

### func at(Int64)

```cangjie
public func at(n: Int64): Option<T>
```

功能：获取当前迭代器第 n 个元素，n 从 0 开始计数。此方法会消耗指定元素前的所有元素（包括指定元素）。

参数：

- n: [Int64](../../core/core_package_api/core_package_intrinsics.md#int64) - 给定的元素序号，序号从 0 开始。

返回值：

- [Option](../../core/core_package_api/core_package_enums.md#enum-optiont)\<T> - 返回对应位置元素，若 n 大于剩余元素数量则返回 None。

示例：

<!-- verify -->

```cangjie
main(): Int64 {
    var arr: Array<Int64> = [1, 2, 3, 4, 5]

    /* 获取容器的迭代器对象 */
    var iter = arr.iterator()
    var num: Option<Int64> = iter.at(2)
    println(num)
    return 0
}
```

运行结果：

```text
Some(3)
```

### func concat(Iterator\<T>)

```cangjie
public func concat(other: Iterator<T>): Iterator<T>
```

功能：串联两个迭代器，当前迭代器在先，参数表示的迭代器在后。

参数：

- other: [Iterator](../../core/core_package_api/core_package_classes.md#class-iteratort)\<T> - 要串联在后面的迭代器。

返回值：

- [Iterator](../../core/core_package_api/core_package_classes.md#class-iteratort)\<T> - 返回串联后的新迭代器。

示例：

<!-- verify -->

```cangjie
main(): Int64 {
    var arr1: Array<Int64> = [1, 2]
    var arr2: Array<Int64> = [3, 4]

    /* 获取容器的迭代器对象 */
    var iter1 = arr1.iterator()
    var iter2 = arr2.iterator()

    /* 合并两个迭代器 */
    var iter = iter1.concat(iter2)

    /* 使用迭代器进行遍历 */
    while (true) {
        match (iter.next()) {
            case Some(i) => println(i)
            case None => break
        }
    }
    return 0
}
```

运行结果：

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

功能：统计当前迭代器包含元素数量。此方法会消耗迭代器中所有元素来计算迭代器中的元素数量。

> **注意：**
>
> 该方法会消耗迭代器，即使用该方法后迭代器内不再包含任何元素。

返回值：

- [Int64](../../core/core_package_api/core_package_intrinsics.md#int64) - 返回迭代器包含元素数量。

示例：

<!-- verify -->

```cangjie
main(): Int64 {
    var arr: Array<Int64> = [1, 2]

    /* 获取容器的迭代器对象 */
    var iter = arr.iterator()
    let len: Int64 = iter.count()
    println(len)

    /* 使用迭代器进行遍历，但是count消耗了迭代器中的元素，因此不会打印 */
    while (true) {
        match (iter.next()) {
            case Some(i) => println(i)
            case None => break
        }
    }
    return 0
}
```

运行结果：

```text
2
```

### func enumerate()

```cangjie
public func enumerate(): Iterator<(Int64, T)>
```

功能：用于获取带索引的迭代器。

返回值：

- [Iterator](../../core/core_package_api/core_package_classes.md#class-iteratort)\<([Int64](../../core/core_package_api/core_package_intrinsics.md#int64), T)> - 返回一个带索引的迭代器。

示例：

<!-- verify -->

```cangjie
main(): Int64 {
    var arr: Array<Int64> = [1, 2]

    /* 获取容器的迭代器对象 */
    var iter = arr.iterator()
    var iter1 = iter.enumerate()

    /* 使用迭代器进行遍历 */
    while (true) {
        match (iter1.next()) {
            case Some(i) => println("${i[0]} ${i[1]}")
            case None => break
        }
    }
    return 0
}
```

运行结果：

```text
0 1
1 2
```

### func filter((T) -> Bool)

```cangjie
public func filter(predicate: (T)-> Bool): Iterator<T>
```

功能：筛选出满足条件的元素。

参数：

- predicate: (T) -> [Bool](../../core/core_package_api/core_package_intrinsics.md#bool) - 给定的条件，条件为 `true` 的元素会按顺序出现在返回的迭代器里。

返回值：

- [Iterator](../../core/core_package_api/core_package_classes.md#class-iteratort)\<T> - 返回一个新迭代器。

示例：

<!-- verify -->

```cangjie
main(): Int64 {
    var arr: Array<Int64> = [1, 2, 3, 4, 5]

    /* 获取过滤后的迭代器对象 */
    var iter = arr.iterator()
    var iter1 = iter.filter({value: Int64 => value > 2})

    /* 使用迭代器进行遍历 */
    while (true) {
        match (iter1.next()) {
            case Some(i) => println(i)
            case None => break
        }
    }
    return 0
}
```

运行结果：

```text
3
4
5
```

### func filterMap\<R>((T) -> Option\<R>)

```cangjie
public func filterMap<R>(transform: (T) -> Option<R>): Iterator<R>
```

功能：同时进行筛选操作和映射操作，返回一个新的迭代器。

参数：

- transform: (T) -> [Option](../../core/core_package_api/core_package_enums.md#enum-optiont)\<R> - 给定的映射函数。函数返回值为 Some 对应 filter 的 predicate 为 true，反之表示 false。

返回值：

- [Iterator](../../core/core_package_api/core_package_classes.md#class-iteratort)\<R> - 返回一个新迭代器。

示例：

<!-- verify -->

```cangjie
main(): Int64 {
    var arr: Array<Int64> = [1, 2, 3, 4, 5]

    /* 获取过滤后的迭代器对象，对元素进行过滤和映射,映射需返回Option类型 */
    var iter = arr.iterator()
    var iter1 = iter.filterMap({
        value: Int64 => if (value > 2) {
            return Some(value + 1)
        } else {
            return None<Int64>
        }
    })

    /* 使用迭代器进行遍历 */
    while (true) {
        match (iter1.next()) {
            case Some(i) => println(i)
            case None => break
        }
    }
    return 0
}
```

运行结果：

```text
4
5
6
```

### func first()

```cangjie
public func first(): Option<T>
```

功能：获取当前迭代器的头部元素。此方法会获取并消耗第一个元素。

返回值：

- [Option](../../core/core_package_api/core_package_enums.md#enum-optiont)\<T> - 返回头部元素，若为空则返回 None。

示例：

<!-- verify -->

```cangjie
main(): Int64 {
    var arr: Array<Int64> = [1, 2, 3, 4, 5]

    /* 获取迭代器对象 */
    var iter = arr.iterator()
    var head: Option<Int64> = iter.first()
    println(head)

    return 0
}
```

运行结果：

```text
Some(1)
```

### func flatMap\<R>((T) -> Iterator\<R>)

```cangjie
public func flatMap<R>(transform: (T) -> Iterator<R>): Iterator<R>
```

功能：创建一个带 [flatten](../../collection/collection_package_api/collection_package_function.md#func-flattent-riterablet-where-t--iterabler) 功能的映射。

参数：

- transform: (T) -> [Iterator](../../core/core_package_api/core_package_classes.md#class-iteratort)\<R> - 给定的映射函数。

返回值：

- [Iterator](../../core/core_package_api/core_package_classes.md#class-iteratort)\<R> - 返回一个带 [flatten](../../collection/collection_package_api/collection_package_function.md#func-flattent-riterablet-where-t--iterabler) 功能的映射。

示例：

<!-- verify -->

```cangjie
main(): Int64 {
    var arr: Array<Array<Int64>> = [[1], [2], [3], [4, 5]]

    /* 获取带flatten功能的迭代器对象 */
    var iter = arr.iterator()
    var iter1 = iter.flatMap({value => value.iterator()})

    /* 使用迭代器进行展开遍历 */
    while (true) {
        match (iter1.next()) {
            case Some(i) => println(i)
            case None => break
        }
    }
    return 0
}
```

运行结果：

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

功能：使用指定初始值，从左向右计算。此方法会消耗迭代器中的所有元素。

参数：

- initial: R - 给定的 R 类型的初始值。
- operation: (R, T) -> R - 给定的计算函数。

返回值：

- R - 返回最终计算得到的值。

示例：

<!-- verify -->

```cangjie
main(): Int64 {
    var arr: Array<Int64> = [1, 2, 3, 4, 5]

    /* 获取迭代器对象，对数组元素进行求和 */
    var iter = arr.iterator()
    var sum: Int64 = iter.fold(0, {total, value => total + value})

    println(sum)
    return 0
}
```

运行结果：

```text
15
```

### func forEach((T) -> Unit)

```cangjie
public func forEach(action: (T)-> Unit): Unit
```

功能：遍历当前迭代器所有元素，对每个元素执行给定的操作。此方法会消耗迭代器中的所有元素。

参数：

- action: (T) -> [Unit](../../core/core_package_api/core_package_intrinsics.md#unit) - 给定的操作函数。

示例：

<!-- verify -->

```cangjie
main(): Int64 {
    var arr: Array<Int64> = [1, 2, 3, 4, 5]

    var iter = arr.iterator()
    iter.forEach({value => println(value)})

    return 0
}
```

运行结果：

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

功能：迭代器每次调用 next() 对当前元素执行额外操作（不会消耗迭代器中元素）。

参数：

- action: (T) -> [Unit](../../core/core_package_api/core_package_intrinsics.md#unit) - 给定的操作函数。

返回值：

- [Iterator](../../core/core_package_api/core_package_classes.md#class-iteratort)\<T> - 返回一个新迭代器。

示例：

<!-- verify -->

```cangjie
main(): Int64 {
    var arr: Array<Int64> = [1, 2]

    /* 获取迭代器对象，并为next函数附加额外操作 */
    var iter = arr.iterator()
    var iter1 = iter.inspect({value => println("Logging: Processing ${value}")})

    /* 使用迭代器进行展开遍历 */
    while (true) {
        match (iter1.next()) {
            case Some(i) => println("Processing ${i} !")
            case None => break
        }
    }
    return 0
}
```

运行结果：

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

功能：迭代器每两个元素之间插入一个给定的新元素。

参数：

- separator: T - 给定的元素。

返回值：

- [Iterator](../../core/core_package_api/core_package_classes.md#class-iteratort)\<T> - 返回一个新迭代器。

示例：

<!-- verify -->

```cangjie
main(): Int64 {
    var arr: Array<Int64> = [1, 2]

    /* 获取迭代器对象，每两个元素之间插入一个0 */
    var iter = arr.iterator()
    var iter1 = iter.intersperse(0)

    /* 使用迭代器进行展开遍历 */
    while (true) {
        match (iter1.next()) {
            case Some(i) => println(i)
            case None => break
        }
    }
    return 0
}
```

运行结果：

```text
1
0
2
```

### func isEmpty()

```cangjie
public func isEmpty(): Bool
```

功能：判断当前迭代器是否为空。此方法会调用 [next()](core_package_classes.md#func-next-1) ，根据其返回值判断当前迭代器是否为空。因此如果当前迭代器不为空，则会消耗一个元素。

返回值：

- [Bool](../../core/core_package_api/core_package_intrinsics.md#bool) - 返回当前迭代器是否为空。

示例：

<!-- verify -->

```cangjie
main(): Int64 {
    var arr: Array<Int64> = [1, 2]

    /* 获取迭代器对象 */
    var iter = arr.iterator()

    /* 判断迭代器中是否有元素，如果有会消耗一个元素 */
    println(iter.isEmpty())

    /* 使用迭代器进行展开遍历 */
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

运行结果：

```text
false
2
true
```

### func last()

```cangjie
public func last(): Option<T>
```

功能：获取当前迭代器尾部元素。此方法会获取并消耗迭代器中的所有元素，并返回最后一个元素。

返回值：

- [Option](../../core/core_package_api/core_package_enums.md#enum-optiont)\<T> - 返回尾部元素，若为空则返回 None。

示例：

<!-- verify -->

```cangjie
main(): Int64 {
    var arr: Array<Int64> = [1, 2]

    /* 获取迭代器对象 */
    var iter = arr.iterator()
    println(iter.last())
    return 0
}
```

运行结果：

```text
Some(2)
```

### func map\<R>((T) -> R)

```cangjie
public func map<R>(transform: (T)-> R): Iterator<R>
```

功能：创建一个映射。

参数：

- transform: (T) ->R - 给定的映射函数。

返回值：

- [Iterator](../../core/core_package_api/core_package_classes.md#class-iteratort)\<R> - 返回一个映射。

示例：

<!-- verify -->

```cangjie
main(): Int64 {
    var arr: Array<Int64> = [1, 2, 3, 4]

    /* 获取迭代器对象，并对元素进行映射，获取新的迭代器对象 */
    var iter = arr.iterator()
    var iter1 = iter.map({value => value * 2})

    /* 使用迭代器进行展开遍历 */
    while (true) {
        match (iter1.next()) {
            case Some(i) => println(i)
            case None => break
        }
    }
    return 0
}
```

运行结果：

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

功能：判断当前迭代器中所有元素是否都不满足条件。此方法会重复获取并消耗迭代器中元素直到某个元素满足条件。

参数：

- predicate: (T) -> [Bool](../../core/core_package_api/core_package_intrinsics.md#bool) - 给定的条件。

返回值：

- [Bool](../../core/core_package_api/core_package_intrinsics.md#bool) - 当前迭代器中元素是否都不满足条件。

示例：

<!-- verify -->

```cangjie
main(): Int64 {
    var arr: Array<Int64> = [1, 2, 3, 4]

    /* 获取迭代器对象，并对元素进行映射，获取新的迭代器对象 */
    var iter1 = arr.iterator()
    var iter2 = arr.iterator()

    /* 存在元素大于2，返回false */
    var flag1: Bool = iter1.none({value => value > 2})
    println(flag1)

    /* 不存在元素大于5，返回true */
    var flag2: Bool = iter2.none({value => value > 5})
    println(flag2)
    return 0
}
```

运行结果：

```text
false
true
```

### func reduce((T, T) -> T)

```cangjie
public func reduce(operation: (T, T) -> T): Option<T>
```

功能：使用第一个元素作为初始值，从左向右计算。此方法会消耗迭代器中的所有元素。

参数：

- operation: (T, T) -> T - 给定的计算函数。

返回值：

- [Option](../../core/core_package_api/core_package_enums.md#enum-optiont)\<T> - 返回计算结果。

示例：

<!-- verify -->

```cangjie
main(): Int64 {
    var arr: Array<Int64> = [1, 2, 3, 4, 5]

    /* 获取迭代器对象，对数组元素进行求和 */
    var iter = arr.iterator()
    var sum: Option<Int64> = iter.reduce({total, value => total + value})
    println(sum)
    return 0
}
```

运行结果：

```text
Some(15)
```

### func skip(Int64)

```cangjie
public func skip(count: Int64): Iterator<T>
```

功能：从前往后从当前迭代器跳过指定个数的元素，返回一个新的迭代器，原迭代器保持不变。

> **说明：**
>
> - 当 `count < 0` 时，抛出异常。
> - 当 `count = 0` 时，不跳过任何元素，返回一个与原迭代器内容一致的新迭代器。
> - 当 `0 < count < 迭代器元素总数` 时，跳过前 count 个元素，返回包含剩余元素的新迭代器。
> - 当 `count ≥ 迭代器元素总数` 时，跳过所有元素，返回空的新迭代器。

参数：

- count: [Int64](../../core/core_package_api/core_package_intrinsics.md#int64) - 要跳过的个数。

返回值：

- [Iterator](../../core/core_package_api/core_package_classes.md#class-iteratort)\<T> - 返回一个跳过指定数量元素的新迭代器。

异常：

- [IllegalArgumentException](../../core/core_package_api/core_package_exceptions.md#class-illegalargumentexception) - 当 `count < 0` 时，抛出异常。

示例：

<!-- verify -->

```cangjie
main(): Int64 {
    var arr: Array<Int64> = [1, 2, 3, 4, 5]

    /* 获取迭代器对象，跳过前两个元素 */
    var iter = arr.iterator()
    var iter1 = iter.skip(2)

    /* 使用迭代器进行展开遍历 */
    while (true) {
        match (iter1.next()) {
            case Some(i) => println(i)
            case None => break
        }
    }
    return 0
}
```

运行结果：

```text
3
4
5
```

### func step(Int64)

```cangjie
public func step(count: Int64): Iterator<T>
```

功能：迭代器每次调用 next() 跳过特定个数。

当 count 小于等于 0 时，抛出异常。当 count 大于 0 时，每次调用 next() 跳过 count 次，直到迭代器为空。

参数：

- count: [Int64](../../core/core_package_api/core_package_intrinsics.md#int64) - 每次调用 next() 要跳过的个数。

返回值：

- [Iterator](../../core/core_package_api/core_package_classes.md#class-iteratort)\<T> - 返回一个新迭代器，这个迭代器每次调用 next() 会跳过特定个数。

异常：

- [IllegalArgumentException](../../core/core_package_api/core_package_exceptions.md#class-illegalargumentexception) - 当 count <= 0 时，抛出异常。

示例：

<!-- verify -->

```cangjie
main(): Int64 {
    var arr: Array<Int64> = [1, 2, 3, 4, 5]

    /* 获取迭代器对象，每次调用 next() 会跳过两个元素 */
    var iter = arr.iterator()
    var iter1 = iter.step(2)

    /* 使用迭代器进行展开遍历 */
    while (true) {
        match (iter1.next()) {
            case Some(i) => println(i)
            case None => break
        }
    }
    return 0
}
```

运行结果：

```text
1
3
5
```

### func take(Int64)

```cangjie
public func take(count: Int64): Iterator<T>
```

功能：从当前迭代器取出特定个数。

从前往后取出当前迭代器特定个数的元素。当 count 小于 0 时，抛出异常。当 count 等于 0 时，不取元素，返回空迭代器。当 count 大于 0 小于迭代器的大小时，取前 count 个元素，返回新迭代器。当 count 大于等于迭代器的大小时，取所有元素，返回原迭代器。

参数：

- count: [Int64](../../core/core_package_api/core_package_intrinsics.md#int64) - 要取出的个数。

返回值：

- [Iterator](../../core/core_package_api/core_package_classes.md#class-iteratort)\<T> - 返回一个取出指定数量元素的迭代器。

异常：

- [IllegalArgumentException](../../core/core_package_api/core_package_exceptions.md#class-illegalargumentexception) - 当 count < 0 时，抛出异常。

示例：

<!-- verify -->

```cangjie
main(): Int64 {
    var arr: Array<Int64> = [1, 2, 3, 4, 5]

    /* 获取迭代器对象，取出前三个元素 */
    var iter = arr.iterator()
    var iter1 = iter.take(3)

    /* 使用迭代器进行展开遍历 */
    while (true) {
        match (iter1.next()) {
            case Some(i) => println(i)
            case None => break
        }
    }
    return 0
}
```

运行结果：

```text
1
2
3
```

### func zip\<R>(Iterator\<R>)

```cangjie
public func zip<R>(it: Iterator<R>): Iterator<(T, R)>
```

功能：将两个迭代器合并成一个（长度取决于短的那个迭代器）。

参数：

- it: [Iterator](../../core/core_package_api/core_package_classes.md#class-iteratort)\<R> - 要合并的其中一个迭代器。

返回值：

- [Iterator](../../core/core_package_api/core_package_classes.md#class-iteratort)\<(T, R)> - 返回一个新迭代器。

示例：

<!-- verify -->

```cangjie
main(): Int64 {
    var arr1: Array<Int64> = [1, 2, 3, 4]
    var arr2: Array<Int64> = [4, 5, 6]

    /* 获取迭代器对象并合并，新迭代器中的元素为对应索引位置元素的元组 */
    var iter1 = arr1.iterator()
    var iter2 = arr2.iterator()
    var iter = iter1.zip(iter2)

    /* 使用迭代器进行遍历，长度取决于较短的迭代器 */
    while (true) {
        match (iter.next()) {
            case Some(i) => println("The current element is (${i[0]}, ${i[1]})")
            case None => break
        }
    }
    return 0
}
```

运行结果：

```text
The current element is (1, 4)
The current element is (2, 5)
The current element is (3, 6)
```

## extend\<T> Iterator\<T> where T <: Comparable\<T>

```cangjie
extend<T> Iterator<T> where T <: Comparable<T>
```

功能：为 [Iterator](core_package_classes.md#class-iteratort)\<T> 类型扩展 [Comparable](core_package_interfaces.md#interface-comparablet)\<T> 接口，支持比较操作。

被扩展类型：

- [Iterator\<T>](core_package_classes.md#class-iteratort)

### func max()

```cangjie
public func max(): Option<T>
```

功能：筛选最大的元素。此方法会消耗迭代器中的所有元素。

返回值：

- [Option](core_package_enums.md#enum-optiont)\<T> - 返回最大的元素，若为空则返回 None。

示例：

<!-- verify -->

```cangjie
main(): Int64 {
    var arr: Array<Int64> = [1, 2, 3, 4]

    /* 获取迭代器对象，使用 max() 求最大值 */
    var iter = arr.iterator()
    match (iter.max()) {
        case Some(i) => println(i)
        case None => println("None!")
    }
    return 0
}
```

运行结果：

```text
4
```

### func min()

```cangjie
public func min(): Option<T>
```

功能：筛选最小的元素。此方法会消耗迭代器中的所有元素。

返回值：

- [Option](core_package_enums.md#enum-optiont)\<T> - 返回最小的元素，若为空则返回 None。

示例：

<!-- verify -->

```cangjie
main(): Int64 {
    var arr: Array<Int64> = [1, 2, 3, 4]

    /* 获取迭代器对象，使用 min() 求最小值 */
    var iter = arr.iterator()
    match (iter.min()) {
        case Some(i) => println(i)
        case None => println("None!")
    }
    return 0
}
```

运行结果：

```text
1
```

## extend\<T> Iterator\<T> where T <: Equatable\<T>

```cangjie
extend<T> Iterator<T> where T <: Equatable<T>
```

功能：为 [Iterator](core_package_classes.md#class-iteratort)\<T> 类型扩展 扩展 [Equatable](core_package_interfaces.md#interface-equatablet)\<T> 接口，支持判等操作。

被扩展类型：

- [Iterator\<T>](core_package_classes.md#class-iteratort)

### func contains(T)

```cangjie
public func contains(element: T): Bool
```

功能：遍历所有元素，判断是否包含指定元素。此方法会重复获取并消耗迭代器中元素直到某个元素与入参元素相等。

参数：

- element: T - 要查找的元素。

返回值：

- [Bool](../../core/core_package_api/core_package_intrinsics.md#bool) - 是否包含指定元素。

示例：

<!-- verify -->

```cangjie
main(): Int64 {
    var arr: Array<Int64> = [1, 2, 3, 4]

    /* 获取迭代器对象，查找是否包含元素 3 */
    var iter = arr.iterator()
    println(iter.contains(3))

    /* 使用迭代器进行遍历，输出剩余元素 */
    while (true) {
        match (iter.next()) {
            case Some(i) => println(i)
            case None => break
        }
    }
    return 0
}
```

运行结果：

```text
true
4
```

## extend\<T> Option\<Option\<T>>

```cangjie
extend<T> Option<Option<T>>
```

功能：为 Option\<Option\<T>> 类型扩展实现某些功能。

被扩展类型：

- [Option\<Option\<T>>](core_package_enums.md#enum-optiont)

### func flatten()

```cangjie
public func flatten(): Option<T>
```

功能：将 [Option](core_package_enums.md#enum-optiont)\<[Option](core_package_enums.md#enum-optiont)\<T>> 类型展开，如果当前实例是 [Some](core_package_enums.md#somet)([Option](core_package_enums.md#enum-optiont)\<T>.[Some](core_package_enums.md#somet)(v)), 展开后的结果为 [Some](core_package_enums.md#somet)(v)。

返回值：

- [Option](core_package_enums.md#enum-optiont)\<T> - [Option](core_package_enums.md#enum-optiont)\<[Option](core_package_enums.md#enum-optiont)\<T>> 类型展开后的结果。

示例：

<!-- verify -->
```cangjie
main() {
    // 创建Option<Option<Int64>>类型的Some(Some(42))值
    var nestedSome: Option<Option<Int64>> = Some(Some(42))

    // 展开嵌套的Option
    var flattened1 = nestedSome.flatten()
    println("Some(Some(42))展开后: ${flattened1}")

    // 创建Option<Option<Int64>>类型的Some(None)值
    var someNone: Option<Option<Int64>> = Some(None)

    // 展开嵌套的Option
    var flattened2 = someNone.flatten()
    println("Some(None)展开后: ${flattened2}")

    // 创建Option<Option<Int64>>类型的None值
    var noneValue: Option<Option<Int64>> = None

    // 展开嵌套的Option
    var flattened3 = noneValue.flatten()
    println("None展开后: ${flattened3}")

    // 演示链式调用
    var chainedValue: Option<Option<Option<String>>> = Some(Some(Some("Hello")))
    var flattenedChained = chainedValue.flatten().flatten()
    println("链式展开Some(Some(Some(\"Hello\"))): ${flattenedChained}")
}
```

运行结果：

```text
Some(Some(42))展开后: Some(42)
Some(None)展开后: None
None展开后: None
链式展开Some(Some(Some("Hello"))): Some(Hello)
```

## extend\<T> Option\<T> <: Equatable\<Option\<T>> where T <: Equatable\<T>

```cangjie
extend<T> Option<T> <: Equatable<Option<T>> where T <: Equatable<T>
```

功能：为 [Option](core_package_enums.md#enum-optiont)\<T> 枚举扩展 [Equatable](core_package_interfaces.md#interface-equatablet)\<[Option](core_package_enums.md#enum-optiont)\<T>> 接口，支持判等操作。

被扩展类型：

- [Option\<T>](core_package_enums.md#enum-optiont)

扩展接口：

- [Equatable](core_package_interfaces.md#interface-equatablet)\<[Option](core_package_enums.md#enum-optiont)\<T>>

### operator func !=(Option\<T>)

```cangjie
public operator func !=(other: Option<T>): Bool
```

功能：判断当前实例与参数指向的 [Option](core_package_enums.md#enum-optiont)\<T> 实例是否不等。

参数：

- other: [Option](core_package_enums.md#enum-optiont)\<T> - 待比较的 [Option](core_package_enums.md#enum-optiont)\<T> 实例。

返回值：

- [Bool](core_package_intrinsics.md#bool) - 如果不相等，则返回 true，否则返回 false。

示例：

<!-- verify -->
```cangjie
main() {
    // 创建相同的Some值
    var someValue1: Option<Int64> = Some(42)
    var someValue2: Option<Int64> = Some(42)

    // 比较两个相同的Some值
    println("Some(42) != Some(42): ${someValue1 != someValue2}")

    // 创建不同的Some值
    var someValue3: Option<Int64> = Some(42)
    var someValue4: Option<Int64> = Some(24)

    // 比较两个不同的Some值
    println("Some(42) != Some(24): ${someValue3 != someValue4}")

    // 创建一个Some值和一个None值
    var someValue5: Option<Int64> = Some(42)
    var noneValue1: Option<Int64> = None

    // 比较Some值和None值
    println("Some(42) != None: ${someValue5 != noneValue1}")

    // 创建两个None值
    var noneValue2: Option<Int64> = None
    var noneValue3: Option<Int64> = None

    // 比较两个None值
    println("None != None: ${noneValue2 != noneValue3}")
}
```

运行结果：

```text
Some(42) != Some(42): false
Some(42) != Some(24): true
Some(42) != None: true
None != None: false
```

### operator func ==(Option\<T>)

```cangjie
public operator func ==(other: Option<T>): Bool
```

功能：判断当前实例与参数指向的 [Option](core_package_enums.md#enum-optiont)\<T> 实例是否相等。

如果两者同为 None，则相等；如果两者为 Some(v1) 和 Some(v2)，且 v1 和 v2 相等，则相等。

参数：

- other: [Option](core_package_enums.md#enum-optiont)\<T> - 待比较的 [Option](core_package_enums.md#enum-optiont)\<T> 实例。

返回值：

- [Bool](core_package_intrinsics.md#bool) - 如果相等，则返回 true，否则返回 false。

示例：

<!-- verify -->
```cangjie
main() {
    // 创建相同的Some值
    var someValue1: Option<Int64> = Some(42)
    var someValue2: Option<Int64> = Some(42)

    // 比较两个相同的Some值
    println("Some(42) == Some(42): ${someValue1 == someValue2}")

    // 创建不同的Some值
    var someValue3: Option<Int64> = Some(42)
    var someValue4: Option<Int64> = Some(24)

    // 比较两个不同的Some值
    println("Some(42) == Some(24): ${someValue3 == someValue4}")

    // 创建一个Some值和一个None值
    var someValue5: Option<Int64> = Some(42)
    var noneValue1: Option<Int64> = None

    // 比较Some值和None值
    println("Some(42) == None: ${someValue5 == noneValue1}")

    // 创建两个None值
    var noneValue2: Option<Int64> = None
    var noneValue3: Option<Int64> = None

    // 比较两个None值
    println("None == None: ${noneValue2 == noneValue3}")
}
```

运行结果：

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

功能：为 [Option](core_package_enums.md#enum-optiont) 类型扩展 [Hashable](core_package_interfaces.md#interface-hashable) 接口。

被扩展类型：

- [Option\<T>](core_package_enums.md#enum-optiont)

扩展接口：

- [Hashable](core_package_interfaces.md#interface-hashable)

### func hashCode()

```cangjie
public func hashCode(): Int64
```

功能：获取哈希值。

返回值：

- [Int64](core_package_intrinsics.md#int64) - 哈希值。

示例：

<!-- verify -->
```cangjie
main() {
    // 创建Some值
    var someValue: Option<Int64> = Some(42)

    // 获取Some值的哈希码
    var someHashCode = someValue.hashCode()
    println("Some(42)的哈希码: ${someHashCode}")

    // 创建None值
    var noneValue: Option<Int64> = None

    // 获取None值的哈希码
    var noneHashCode = noneValue.hashCode()
    println("None的哈希码: ${noneHashCode}")

    // 比较两个相同的Some值的哈希码
    var someValue1: Option<Int64> = Some(42)
    var someValue2: Option<Int64> = Some(42)

    println("Some(42)和Some(42)的哈希码是否相等: ${someValue1.hashCode() == someValue2.hashCode()}")

    // 比较两个不同的Some值的哈希码
    var someValue3: Option<Int64> = Some(42)
    var someValue4: Option<Int64> = Some(24)

    println("Some(42)和Some(24)的哈希码是否相等: ${someValue3.hashCode() == someValue4.hashCode()}")
}
```

运行结果：

```text
Some(42)的哈希码: 42
None的哈希码: 0
Some(42)和Some(42)的哈希码是否相等: true
Some(42)和Some(24)的哈希码是否相等: false
```

## extend\<T> Option\<T> <: ToString where T <: ToString

```cangjie
extend<T> Option<T> <: ToString where T <: ToString
```

功能：为 [Option](core_package_enums.md#enum-optiont)\<T> 枚举实现 [ToString](core_package_interfaces.md#interface-tostring) 接口，支持转字符串操作。

被扩展类型：

- [Option\<T>](core_package_enums.md#enum-optiont)

扩展接口：

- [ToString](core_package_interfaces.md#interface-tostring)

### func toString()

```cangjie
public func toString(): String
```

功能：将 [Option](core_package_enums.md#enum-optiont) 转换为字符串，字符串内容为 "Some(${T.toString()})" 或 "None"。

返回值：

- [String](core_package_structs.md#struct-string) - 转化后的字符串。

示例：

<!-- verify -->
```cangjie
main() {
    // 创建Some值
    var someValue: Option<Int64> = Some(42)

    // 将Some值转换为字符串
    var someStr = someValue
    println("Some(42)转换为字符串: ${someStr}")

    // 创建None值
    var noneValue: Option<Int64> = None

    // 将None值转换为字符串
    var noneStr = noneValue
    println("None转换为字符串: ${noneStr}")

    // 创建字符串类型的Some值
    var someStringValue: Option<String> = Some("Hello")

    // 将字符串类型的Some值转换为字符串
    var someStringStr = someStringValue
    println("Some(\"Hello\")转换为字符串: ${someStringStr}")
}
```

运行结果：

```text
Some(42)转换为字符串: Some(42)
None转换为字符串: None
Some("Hello")转换为字符串: Some(Hello)
```

## extend\<T> Range\<T> <: Equatable\<Range\<T>> where T <: Countable\<T> & Comparable\<T> & Equatable\<T>

```cangjie
extend<T> Range<T> <: Equatable<Range<T>> where T <: Countable<T> & Comparable<T> & Equatable<T>
```

功能：为 [Range](core_package_structs.md#struct-ranget-where-t--countablet--comparablet--equatablet)\<T> 类型扩展 [Equatable](core_package_interfaces.md#interface-equatablet)\<[Range](core_package_structs.md#struct-ranget-where-t--countablet--comparablet--equatablet)\<T>> 接口。

被扩展类型：

- [Range\<T>](core_package_structs.md#struct-ranget-where-t--countablet--comparablet--equatablet)

扩展接口：

- [Equatable](core_package_interfaces.md#interface-equatablet)\<[Range](core_package_structs.md#struct-ranget-where-t--countablet--comparablet--equatablet)\<T>>

### operator func ==(Range\<T>)

```cangjie
public operator func ==(other: Range<T>): Bool
```

功能：判断两个 [Range](core_package_structs.md#struct-ranget-where-t--countablet--comparablet--equatablet) 实例是否相等。

两个 [Range](core_package_structs.md#struct-ranget-where-t--countablet--comparablet--equatablet) 实例相等指的是它们表示同一个区间，即 `start`、`end`、[step](core_package_structs.md#let-step)、`isClosed` 值相等。

参数：

- other: [Range](core_package_structs.md#struct-ranget-where-t--countablet--comparablet--equatablet)\<T> - 待比较的 [Range](core_package_structs.md#struct-ranget-where-t--countablet--comparablet--equatablet) 实例。

返回值：

- [Bool](core_package_intrinsics.md#bool) - true 代表相等，false 代表不相等。

示例：

<!-- verify -->
```cangjie
main() {
    // 创建两个相同的Range实例
    let range1 = Range<Int32>(1, 5, 1, true, false, false)
    let range2 = Range<Int32>(1, 5, 1, true, false, false)

    // 使用==操作符比较两个Range实例
    if (range1 == range2) {
        println("Range1 is equal to Range2")
    } else {
        println("Range1 is not equal to Range2")
    }

    // 创建一个不同的Range实例
    let range3 = Range<Int32>(1, 6, 1, true, false, false)

    // 使用==操作符比较两个不同的Range实例
    if (range1 == range3) {
        println("Range1 is equal to Range3")
    } else {
        println("Range1 is not equal to Range3")
    }
}
```

运行结果：

```text
Range1 is equal to Range2
Range1 is not equal to Range3
```

## extend\<T> Range\<T> <: Hashable where T <: Hashable & Countable\<T> & Comparable\<T> & Equatable\<T>

```cangjie
extend<T> Range<T> <: Hashable where T <: Hashable & Countable<T> & Comparable<T> & Equatable<T>
```

功能：为 [Range](core_package_structs.md#struct-ranget-where-t--countablet--comparablet--equatablet) 类型扩展 [Hashable](core_package_interfaces.md#interface-hashable) 接口，支持计算哈希值。

被扩展类型：

- [Range\<T>](core_package_structs.md#struct-ranget-where-t--countablet--comparablet--equatablet)

扩展接口：

- [Hashable](core_package_interfaces.md#interface-hashable)

### func hashCode()

```cangjie
public func hashCode(): Int64
```

功能：获取哈希值，该值为 `start`、`end`、[step](core_package_structs.md#let-step)、`isClosed` 的组合哈希运算结果。

返回值：

- [Int64](core_package_intrinsics.md#int64) - 哈希值。

示例：

<!-- verify -->
```cangjie
main() {
    // 创建一个Range实例
    let range = Range<Int32>(1, 5, 1, true, false, false)

    // 获取哈希值
    let hash = range.hashCode()
    println("Range hash code: ${hash}")

    // 创建另一个相同的Range实例
    let sameRange = Range<Int32>(1, 5, 1, true, false, false)
    let sameHash = sameRange.hashCode()
    println("Same range hash code: ${sameHash}")

    // 验证相同Range实例的哈希值相同
    if (hash == sameHash) {
        println("Same ranges have the same hash code")
    }
}
```

运行结果：

```text
Range hash code: -1988359251787629792
Same range hash code: -1988359251787629792
Same ranges have the same hash code
```
