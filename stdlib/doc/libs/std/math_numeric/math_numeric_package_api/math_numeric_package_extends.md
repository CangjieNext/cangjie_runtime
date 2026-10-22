# 扩展

## extend BigInt <: Formattable

```cangjie
extend BigInt <: Formattable
```

功能：为 [BigInt](math_numeric_package_structs.md#struct-bigint) 扩展 [Formattable](../../convert/convert_package_api/convert_package_interfaces.md#interface-formattable) 接口，以实现将 [BigInt](math_numeric_package_structs.md#struct-bigint) 实例转换为格式化字符串。

被扩展类型：

- [BigInt](math_numeric_package_structs.md#struct-bigint)

扩展接口：

- [Formattable](../../convert/convert_package_api/convert_package_interfaces.md#interface-formattable)

### func format(String)

```cangjie
public func format(fmt: String): String
```

功能：根据格式化参数将当前 [BigInt](math_numeric_package_structs.md#struct-bigint) 类型实例格式化为对应格式的字符串。

参数：

- fmt: [String](../../core/core_package_api/core_package_structs.md#struct-string) - 格式化参数。

返回值：

- [String](../../core/core_package_api/core_package_structs.md#struct-string) - 将当前 [BigInt](math_numeric_package_structs.md#struct-bigint) 类型实例格式化后得到的字符串。

异常：

- [IllegalArgumentException](../../core/core_package_api/core_package_exceptions.md#class-illegalargumentexception) - 当 fmt 不合法时抛出异常。

示例：

<!-- verify -->
```cangjie
import std.math.numeric.*

main() {
    /* 根据格式化参数将 BigInt 实例格式化为对应格式的字符串 */
    let bigInt = BigInt(12345)
    let formatted = bigInt.format("x")
    println(formatted)
}
```

运行结果：

```text
3039
```

## extend BigInt <: Integer\<BigInt>

```cangjie
extend BigInt <: Integer<BigInt>
```

功能：为 [BigInt](math_numeric_package_structs.md#struct-bigint) 类型扩展 [Integer\<T>](../../math/math_package_api/math_package_interfaces.md#interface-integert) 接口。

被扩展类型：

- [BigInt](math_numeric_package_structs.md#struct-bigint)

扩展接口：

- [Integer](../../math/math_package_api/math_package_interfaces.md#interface-integert)\<[BigInt](math_numeric_package_structs.md#struct-bigint)>

### static func isSigned()

```cangjie
public static func isSigned(): Bool
```

功能：判断 [BigInt](math_numeric_package_structs.md#struct-bigint) 类型是否是有符号类型。

返回值：

- [Bool](../../core/core_package_api/core_package_intrinsics.md#bool) - 总是返回 `true`。

示例：

<!-- verify -->
```cangjie
import std.math.numeric.*

main() {
    /* 判断 BigInt 类型是否是有符号类型 */
    let signed = BigInt.isSigned()
    println(signed)
}
```

运行结果：

```text
true
```

## extend BigInt <: Number\<BigInt>

```cangjie
extend BigInt <: Number<BigInt>
```

功能：为 [BigInt](math_numeric_package_structs.md#struct-bigint) 类型扩展 [Number\<T>](../../math/math_package_api/math_package_interfaces.md#interface-numbert) 接口。

被扩展类型：

- [BigInt](math_numeric_package_structs.md#struct-bigint)

扩展接口：

- [Number](../../math/math_package_api/math_package_interfaces.md#interface-numbert)\<[BigInt](math_numeric_package_structs.md#struct-bigint)>

## extend BigInt <: Parsable\<BigInt>

```cangjie
extend BigInt <: Parsable<BigInt>
```

功能：此扩展主要用于实现将 [BigInt](math_numeric_package_structs.md#struct-bigint) 类型字面量的字符串转换为 [BigInt](math_numeric_package_structs.md#struct-bigint) 结构体的相关操作函数。

被扩展类型：

- [BigInt](math_numeric_package_structs.md#struct-bigint)

扩展接口：

- [Parsable](../../convert/convert_package_api/convert_package_interfaces.md#interface-parsablet)\<[BigInt](math_numeric_package_structs.md#struct-bigint)>

### static func parse(String)

```cangjie
public static func parse(value: String): BigInt
```

功能：将字符串解析成一个 [BigInt](math_numeric_package_structs.md#struct-bigint) 结构体。

字符串的规则如下，即开头是可选的符号（正号或负号），接进制前缀，再接一串字符串表示的数字：

IntegerString : SignString? BaseString? ValueString

- SignString : + | -

- BaseString : "0b" | "0B" | "0o" | "0O" | "0x" | "0X" | ""

- ValueString : Digits

    - Digits: Digit | Digit Digits

        - Digit : '0' ~ '9' | 'A' ~ 'Z' | 'a' ~ 'z'

            - 如果进制前缀是 "0b" 或 "0B"，则 Digit 取值范围应为 '0' ~ '1'；

            - 如果进制前缀是 "0o" 或 "0O"，则 Digit 取值范围应为 '0' ~ '7'；

            - 如果进制前缀是 "0x" 或 "0X"，则 Digit 取值范围应为 '0' ~ '9'、'a' ~ 'z' 或 'A' ~ 'Z'；

            - 如果进制前缀是空，则 Digit 取值范围应为 '0' ~ '9'。

参数：

- value: [String](../../core/core_package_api/core_package_structs.md#struct-string) - 用于构建 [BigInt](math_numeric_package_structs.md#struct-bigint) 结构体的字符串。字符串规则为，开头可选一个正号（+）或者负号（-）。接下来可选的进制前缀，默认为十进制，使用 "0b" 或 "0B" 表示二进制，使用 "0o" 或 "0O" 表示八进制，使用 "0x" 或 "0X" 表示十六进制。再接下来必选非空阿拉伯数字或大小写拉丁字母的字符序列，大小写字符含义一样，'a' 和 'A' 的大小等于十进制的 10，'b' 和 'B' 的大小等于十进制的 11，以此类推。序列中的字符应符合相应进制的字符集要求。

返回值：

- [BigInt](math_numeric_package_structs.md#struct-bigint) - 解析出的 [BigInt](math_numeric_package_structs.md#struct-bigint) 结构体。

异常：

- [IllegalArgumentException](../../core/core_package_api/core_package_exceptions.md#class-illegalargumentexception) - 如果字符串 `value` 不符合上述规则，抛此异常。

示例：

<!-- verify -->
```cangjie
import std.math.numeric.*

main() {
    /* 将字符串解析成一个 BigInt 结构体 */
    let bigInt = BigInt.parse("12345")
    println(bigInt)

    /* 使用十六进制前缀解析 */
    let hexBigInt = BigInt.parse("0x1000")
    println(hexBigInt)
}
```

运行结果：

```text
12345
4096
```

### static func tryParse(String)

```cangjie
public static func tryParse(value: String): ?BigInt
```

功能：尝试将字符串解析成一个 [BigInt](math_numeric_package_structs.md#struct-bigint) 结构体。

字符串的规则如下，即开头是可选的符号（正号或负号），接进制前缀，再接一串字符串表示的数字：

IntegerString : SignString? BaseString? ValueString

- SignString : + | -

- BaseString : "0b" | "0B" | "0o" | "0O" | "0x" | "0X" | ""

- ValueString : Digits

    - Digits: Digit | Digit Digits

        - Digit : '0' ~ '9' | 'A' ~ 'Z' | 'a' ~ 'z'

            - 如果进制前缀是 "0b" 或 "0B"，则 Digit 取值范围应为 '0' ~ '1'；

            - 如果进制前缀是 "0o" 或 "0O"，则 Digit 取值范围应为 '0' ~ '7'；

            - 如果进制前缀是 "0x" 或 "0X"，则 Digit 取值范围应为 '0' ~ '9'、'a' ~ 'z' 或 'A' ~ 'Z'；

            - 如果进制前缀是空，则 Digit 取值范围应为 '0' ~ '9'。

参数：

- value: [String](../../core/core_package_api/core_package_structs.md#struct-string) - 用于构建 [BigInt](math_numeric_package_structs.md#struct-bigint) 结构体的字符串。字符串规则为，开头可选一个正号（+）或者负号（-）。接下来可选的进制前缀，默认为十进制，使用 "0b" 或 "0B" 表示二进制，使用 "0o" 或 "0O" 表示八进制，使用 "0x" 或 "0X" 表示十六进制。再接下来必选非空阿拉伯数字或大小写拉丁字母的字符序列，大小写字符含义一样，'a' 和 'A' 的大小等于十进制的 10，'b' 和 'B' 的大小等于十进制的 11，以此类推。序列中的字符应符合相应进制的字符集要求。

返回值：

- ?[BigInt](math_numeric_package_structs.md#struct-bigint) - 解析出的 [BigInt](math_numeric_package_structs.md#struct-bigint) 结构体，解析失败则返回 `None`。

示例：

<!-- verify -->
```cangjie
import std.math.numeric.*

main() {
    /* 尝试将字符串解析成一个 BigInt 结构体 */
    let bigInt = BigInt.tryParse("12345")
    println(bigInt)

    /* 尝试解析无效字符串 */
    let invalid = BigInt.tryParse("invalid")
    println(invalid)
}
```

运行结果：

```text
Some(12345)
None
```

## extend BigInt <: RadixConvertible\<BigInt>

```cangjie
extend BigInt <: RadixConvertible<BigInt>
```

功能：此扩展主要用于实现将 [BigInt](math_numeric_package_structs.md#struct-bigint) 类型字面量的字符串转换为 [BigInt](math_numeric_package_structs.md#struct-bigint) 结构体的相关操作函数。

被扩展类型：

- [BigInt](math_numeric_package_structs.md#struct-bigint)

扩展接口：

- [RadixConvertible](../../convert/convert_package_api/convert_package_interfaces.md#interface-radixconvertiblet)\<[BigInt](math_numeric_package_structs.md#struct-bigint)>

### static func parse(String, Int64)

```cangjie
public static func parse(value: String, radix!: Int64): BigInt
```

功能：根据指定进制将字符串解析成一个 [BigInt](math_numeric_package_structs.md#struct-bigint) 结构体，支持 2 进制到 36 进制。

字符串的规则如下，即开头是可选的符号（正号或负号），接一串字符串表示的数字：

IntegerString : SignString? ValueString

- SignString : + | -

- ValueString : Digits

    - Digits: Digit | Digit Digits

        - Digit : '0' ~ '9' | 'A' ~ 'Z' | 'a' ~ 'z'

            - 如果 Digit 在 '0' ~ '9' 内， 需要满足 (Digit - '0') < radix；

            - 如果 Digit 在 'A' ~ 'Z' 内， 需要满足 (Digit - 'A') + 10 < radix；

            - 如果 Digit 在 'a' ~ 'z' 内， 需要满足 (Digit - 'A') + 10 < radix。

参数：

- value: [String](../../core/core_package_api/core_package_structs.md#struct-string) - 用于构建 [BigInt](math_numeric_package_structs.md#struct-bigint) 结构体的字符串。字符串规则为，开头可选一个正号（+）或者负号（-）。接下来必选非空阿拉伯数字或大小写拉丁字母的字符序列，大小写字符含义一样，'a' 和 'A' 的大小等于十进制的 10，'b' 和 'B' 的大小等于十进制的 11，以此类推。序列中的字符大小不得大于等于进制大小。
- radix!: [Int64](../../core/core_package_api/core_package_intrinsics.md#int64) - 进制。字符串所表示的进制，范围为 [2, 36]。

返回值：

- [BigInt](math_numeric_package_structs.md#struct-bigint) - 解析出的 [BigInt](math_numeric_package_structs.md#struct-bigint) 结构体。

异常：

- [IllegalArgumentException](../../core/core_package_api/core_package_exceptions.md#class-illegalargumentexception) - 如果字符串 `value` 不符合上述规则，或 `radix` 表示的进制不在 [2, 36] 区间内，抛此异常。

示例：

<!-- verify -->
```cangjie
import std.math.numeric.*

main() {
    /* 根据指定进制将字符串解析成一个 BigInt 结构体 */
    let bigInt = BigInt.parse("1010", radix: 2)
    println(bigInt)
}
```

运行结果：

```text
10
```

### static func tryParse(String, Int64)

```cangjie
public static func tryParse(value: String, radix!: Int64): ?BigInt
```

功能：尝试根据指定进制将字符串解析成一个 [BigInt](math_numeric_package_structs.md#struct-bigint) 结构体，支持 2 进制到 36 进制。

字符串的规则如下，即开头是可选的符号（正号或负号），接一串字符串表示的数字：

IntegerString : SignString? ValueString

- SignString : + | -

- ValueString : Digits

    - Digits: Digit | Digit Digits

        - Digit : '0' ~ '9' | 'A' ~ 'Z' | 'a' ~ 'z'

            - 如果 Digit 在 '0' ~ '9' 内， 需要满足 (Digit - '0') < radix；

            - 如果 Digit 在 'A' ~ 'Z' 内， 需要满足 (Digit - 'A') + 10 < radix；

            - 如果 Digit 在 'a' ~ 'z' 内， 需要满足 (Digit - 'A') + 10 < radix。

参数：

- value: [String](../../core/core_package_api/core_package_structs.md#struct-string) - 用于构建 [BigInt](math_numeric_package_structs.md#struct-bigint) 结构体的字符串。字符串规则为，开头可选一个正号（+）或者负号（-）。接下来必选非空阿拉伯数字或大小写拉丁字母的字符序列，大小写字符含义一样，'a' 和 'A' 的大小等于十进制的 10，'b' 和 'B' 的大小等于十进制的 11，以此类推。序列中的字符大小不得大于等于进制大小。
- radix!: [Int64](../../core/core_package_api/core_package_intrinsics.md#int64) - 进制。字符串所表示的进制，范围为 [2, 36]。

返回值：

- ?[BigInt](math_numeric_package_structs.md#struct-bigint) - 解析出的 [BigInt](math_numeric_package_structs.md#struct-bigint) 结构体，解析失败时返回 `None`。

示例：

<!-- verify -->
```cangjie
import std.math.numeric.*

main() {
    /* 尝试根据指定进制将字符串解析成一个 BigInt 结构体 */
    let bigInt = BigInt.tryParse("1010", radix: 2)
    println(bigInt)

    /* 尝试解析无效字符串 */
    let invalid = BigInt.tryParse("invalid", radix: 2)
    println(invalid)
}
```

运行结果：

```text
Some(10)
None
```

### func toString(Int64)

```cangjie
public func toString(radix!: Int64): String
```

功能：计算并返回此 [BigInt](math_numeric_package_structs.md#struct-bigint) 的任意进制字符串表示。

参数：

- radix!: [Int64](../../core/core_package_api/core_package_intrinsics.md#int64) - 进制。字符串所表示的进制，范围为 [2, 36]。

返回值：

- [String](../../core/core_package_api/core_package_structs.md#struct-string) - 返回此 [BigInt](math_numeric_package_structs.md#struct-bigint) 的 `radix` 进制字符串。

异常：

- [IllegalArgumentException](../../core/core_package_api/core_package_exceptions.md#class-illegalargumentexception) - 当入参 radix 不在 [2, 36] 范围内时，抛出异常。

示例：

<!-- verify -->
```cangjie
import std.math.numeric.BigInt

main() {
    let bigInt = BigInt(0x400)
    let toString = bigInt.toString(radix: 2)
    println(toString)
}
```

运行结果：

```text
10000000000
```

## extend Decimal <: Formattable

```cangjie
extend Decimal <: Formattable
```

功能：为 [Decimal](math_numeric_package_structs.md#struct-decimal) 扩展 [Formattable](../../convert/convert_package_api/convert_package_interfaces.md#interface-formattable) 接口，以实现将 [Decimal](math_numeric_package_structs.md#struct-decimal) 实例转换为格式化字符串。

被扩展类型：

- [Decimal](math_numeric_package_structs.md#struct-decimal)

扩展接口：

- [Formattable](../../convert/convert_package_api/convert_package_interfaces.md#interface-formattable)

### func format(String)

```cangjie
public func format(fmt: String): String
```

功能：根据格式化参数将当前 [Decimal](math_numeric_package_structs.md#struct-decimal) 类型实例格式化为对应格式的字符串。

参数：

- fmt: [String](../../core/core_package_api/core_package_structs.md#struct-string) - 格式化参数。

返回值：

- [String](../../core/core_package_api/core_package_structs.md#struct-string) - 将当前 [Decimal](math_numeric_package_structs.md#struct-decimal) 类型实例格式化后得到的字符串。

异常：

- [IllegalArgumentException](../../core/core_package_api/core_package_exceptions.md#class-illegalargumentexception) - 当 fmt 不合法时抛出异常。

示例：

<!-- verify -->
```cangjie
import std.math.numeric.*

main() {
    let decimal = Decimal(123.456)
    // 使用 format 函数格式化 Decimal
    let formatted = decimal.format("e")
    println(formatted)
}
```

运行结果：

```text
1.234560e+02
```

## extend Decimal <: Number\<Decimal>

```cangjie
extend Decimal <: Number<Decimal>
```

功能：为 [Decimal](math_numeric_package_structs.md#struct-decimal) 类型扩展 [Number\<T>](../../math/math_package_api/math_package_interfaces.md#interface-numbert) 接口。

被扩展类型：

- [Decimal](math_numeric_package_structs.md#struct-decimal)

扩展接口：

- [Number](../../math/math_package_api/math_package_interfaces.md#interface-numbert)\<[Decimal](math_numeric_package_structs.md#struct-decimal)>

## extend Decimal <: Parsable\<Decimal>

```cangjie
extend Decimal <: Parsable<Decimal>
```

功能：此扩展主要用于实现将 [Decimal](math_numeric_package_structs.md#struct-decimal) 类型字面量的字符串转换为 [Decimal](math_numeric_package_structs.md#struct-decimal) 结构体的相关操作函数。

被扩展类型：

- [Decimal](math_numeric_package_structs.md#struct-decimal)

扩展接口：

- [Parsable](../../convert/convert_package_api/convert_package_interfaces.md#interface-parsablet)\<[Decimal](math_numeric_package_structs.md#struct-decimal)>

### static func parse(String)

```cangjie
public static func parse(value: String): Decimal
```

功能：通过规定格式字符串构建 [Decimal](math_numeric_package_structs.md#struct-decimal) 结构体。默认采用精度值为 0，即无限精度进行构建。字符串需满足如下格式，即开头可选的符号（正号或负号），接 ValueString 字符串，再接可选的 ExponentString 字符串：

[Decimal](math_numeric_package_structs.md#struct-decimal) 字符串: SignString? ValueString ExponentString?

- SignString: + | -

- ValueString: IntegerPart.(FractionPart)? | .FractionPart | IntegerPart

    - IntegerPart：Digits

    - FractionPart：Digits

    - Digits: Digit | Digit Digits

        - Digit：'0' ~ '9'

- ExponentString: ExponentIndicator (SignString)? IntegerPart

    - ExponentIndicator：e | E

参数：

- value: [String](../../core/core_package_api/core_package_structs.md#struct-string) - 规定格式字符串。

返回值：

- [Decimal](math_numeric_package_structs.md#struct-decimal) - 解析出的 [Decimal](math_numeric_package_structs.md#struct-decimal) 结构体。

异常：

- [IllegalArgumentException](../../core/core_package_api/core_package_exceptions.md#class-illegalargumentexception) - 当入参字符串不满足规定格式时，抛此异常。
- [OverflowException](../../core/core_package_api/core_package_exceptions.md#class-overflowexception) - 当构建值标度溢出时，抛此异常。

示例：

<!-- verify -->
```cangjie
import std.math.numeric.*

main() {
    let decimalString = "123.456e2"
    let decimal = Decimal.parse(decimalString)
    println(decimal)
}
```

运行结果：

```text
12345.6
```

### static func tryParse(String)

```cangjie
public static func tryParse(value: String): ?Decimal
```

功能：尝试通过规定格式字符串构建 [Decimal](math_numeric_package_structs.md#struct-decimal) 结构体。默认采用精度值为 0，即无限精度进行构建。字符串需满足如下格式，即开头可选的符号（正号或负号），接 ValueString 字符串，再接可选的 ExponentString 字符串：

[Decimal](math_numeric_package_structs.md#struct-decimal) 字符串: SignString? ValueString ExponentString?

- SignString: + | -

- ValueString: IntegerPart.(FractionPart)? | .FractionPart | IntegerPart

    - IntegerPart：Digits

    - FractionPart：Digits

    - Digits: Digit | Digit Digits

        - Digit：'0' ~ '9'

- ExponentString: ExponentIndicator (SignString)? IntegerPart

    - ExponentIndicator：e | E

参数：

- value: [String](../../core/core_package_api/core_package_structs.md#struct-string) - 规定格式字符串。

返回值：

- ?[Decimal](math_numeric_package_structs.md#struct-decimal) - 解析出的 [Decimal](math_numeric_package_structs.md#struct-decimal) 结构体，解析失败则返回 `None`。

示例：

<!-- verify -->
```cangjie
import std.math.numeric.*

main() {
    let decimalString = "123.456e2"
    let decimal = Decimal.tryParse(decimalString)
    match (decimal) {
        case Some(d) => println(d)
        case None => println("Failed to parse the string")
    }
}
```

运行结果：

```text
12345.6
```

