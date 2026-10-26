# 类

## class ConsoleReader

```cangjie
public class ConsoleReader <: InputStream {}
```

功能：提供从控制台读出数据并转换成字符或字符串的功能。

该类型无法构造实例，只能通过 [getStdIn()](./env_package_funcs.md#func-getstdin) 获取实例。
读操作是同步的，内部设有缓冲区来保存控制台输入的内容。到达输入流末尾且本次调用未读到数据时，返回可选类型的读取函数返回 `None`，`read(arr: Array<Byte>)` 和 `readSome(arr: Array<Byte>)` 返回 `0`。

> **说明：**
>
> [ConsoleReader](./env_package_classes.md#class-consolereader) 只有一个实例，所有方法共享同一个缓冲区。对于返回可选类型的读取函数，以下情形可能使后续读取返回 `None`：
>
> - 将标准输入重定向到文件时，读取到文件结尾 EOF。
> - Linux 环境，按下 `Ctrl+D`。
> - Windows 环境，按下 `Ctrl+Z` 后加回车。

父类型：

- [InputStream](../../io/io_package_api/io_package_interfaces.md#interface-inputstream)

### func read()

```cangjie
public func read(): ?Rune
```

功能：从标准输入中读取下一个字符。

返回值：

- ?[Rune](../../core/core_package_api/core_package_intrinsics.md#rune) - 读取到字符，返回 ?[Rune](../../core/core_package_api/core_package_intrinsics.md#rune)，否则返回 `None`。

异常：

- [IllegalArgumentException](../../core/core_package_api/core_package_exceptions.md#class-illegalargumentexception) - 当输入不符合 `UTF-8` 编码的字符串时，抛此异常。

示例：

<!-- compile -->
```cangjie
import std.env.*

main() {
    // 获取标准输入流
    let stdin = getStdIn()

    // 读取下一个字符
    // 注意：在实际运行中，程序会等待用户输入一个字符，假设用户输入了一个 H
    let r = stdin.read()
    println("用户输入了一个 ${r}")
}
```

运行结果：

```text
用户输入了一个 Some(H)
```

### func read(Array\<Byte>)

```cangjie
public func read(arr: Array<Byte>): Int64
```

功能：从标准输入中读取数据放入 `arr`，保留原有的按行取数、尝试填满数组的行为。

当内部缓存不足以填满数组时，继续通过行读取路径获取数据；该路径可能等待换行。输入结束或读取失败时，返回已取得的有效前缀，其长度可能小于 `arr.size`。空数组返回 `0`。该方法不适合写端发送部分数据后等待响应的管道交互；需要短读时，请主动使用 [readSome(Array\<Byte>)](#func-readsomearraybyte)。

当前实现保留了读取失败也可能返回 `0` 的历史行为，因此返回 `0` 不能用于区分所有底层错误与正常 EOF。

Windows 重定向输入的行读取路径保留默认文本语义：CRLF 转为 LF，`Ctrl+Z`（`0x1A`）作为 EOF。

> **注意：**
>
> 该函数存在风险，可能读取出来的结果恰好把 `UTF-8 code point` 从中截断，如果发生截断，将导致该 [Array](../../core/core_package_api/core_package_structs.md#struct-arrayt)\<[Byte](../../core/core_package_api/core_package_types.md#type-byte)> 转换成字符串的结果不正确或抛出异常。

参数：

- arr: [Array](../../core/core_package_api/core_package_structs.md#struct-arrayt)\<[Byte](../../core/core_package_api/core_package_types.md#type-byte)> - 目标 [Array](../../core/core_package_api/core_package_structs.md#struct-arrayt)。

返回值：

- [Int64](../../core/core_package_api/core_package_intrinsics.md#int64) - 本次实际读取的字节数 `n`，满足 `0 <= n <= arr.size`。读取的数据存放在 `arr` 的前 `n` 个元素中。`arr` 为空，或到达输入流末尾且本次调用未读到数据时，返回 `0`。

示例：

<!-- compile -->
```cangjie
import std.env.*

main() {
    // 获取标准输入流
    let stdin = getStdIn()

    // 创建一个字节数组用于读取数据
    let buffer: Array<Byte> = [0, 0, 0, 0, 0]

    // 返回值可能小于 buffer.size，只处理本次实际读取的字节。
    let readSize = stdin.read(buffer)
    println("读取到的字节数：${readSize}")
    if (readSize > 0) {
        println("读取到的数据：${buffer.slice(0, readSize)}")
    }
}
```

将仅包含 `abc`（无换行符）的文件重定向为标准输入，输出示例：

```text
读取到的字节数：3
读取到的数据：[97, 98, 99]
```

### func readSome(Array\<Byte>)

```cangjie
public func readSome(arr: Array<Byte>): Int64
```

功能：从标准输入中读取最多 `arr.size` 个字节并放入 `arr`，允许返回不足数组长度的数据。

内部缓存已有数据时，复制其中可用的前缀并返回，不继续等待新数据填满数组。缓存为空时，尝试读取输入。重定向管道读取不要求换行，也不等待目标数组填满或写端关闭；没有数据且输入未结束时仍可能阻塞。交互终端仍受行缓冲等输入模式影响，允许短读不等于非阻塞读取。

该方法与其他标准输入读取方法共享缓存。一次短读不表示 EOF，也不对应一条完整消息。调用方只处理返回计数对应的前缀，按协议累计数据。按字节分块可能截断 UTF-8 字符，不能假定每块数据都能独立转换为字符串。

在 Windows 重定向输入中，独立使用本方法可读取原始字节，包括 CRLF 和 `0x1A`。若此前 `read`、`readln` 或 `readUntil` 已将数据预取到本实例缓存，本方法会先返回已经过文本转换的剩余数据。实例内混用保持消费顺序，但不保证原始字节不变；需要原始字节时，应从开始就使用本方法。

该方法保留了部分底层读取失败也返回 `0` 的行为，不能用 `0` 区分所有读取错误与正常 EOF。

参数：

- arr: [Array](../../core/core_package_api/core_package_structs.md#struct-arrayt)\<[Byte](../../core/core_package_api/core_package_types.md#type-byte)> - 目标数组，也可以传入数组切片。

返回值：

- [Int64](../../core/core_package_api/core_package_intrinsics.md#int64) - 实际读取的字节数 `n`，满足 `0 <= n <= arr.size`。仅前 `n` 个元素写入本次数据，其余元素不变。空数组，或正常到达输入结束且本次未读到数据时返回 `0`；部分读取失败也返回 `0`。

示例：

<!-- compile -->
```cangjie
import std.env.getStdIn

main() {
    let buffer = Array<Byte>(256, repeat: 0)
    let count = getStdIn().readSome(buffer)
    println(buffer.slice(0, count))
}
```

管道写端发送 `abc` 并保持打开时，上述程序可以输出 `[97, 98, 99]`，无需写端发送换行或关闭管道。

### func readln()

```cangjie
public func readln(): ?String
```

功能：从标准输入中读取一行字符串。

读取到字符，返回 ?[String](../../core/core_package_api/core_package_structs.md#struct-string)，结果不包含末尾换行符。该接口不会抛出异常，即使输入不符合 `UTF-8` 编码的字符串，也会构造出一个 [String](../../core/core_package_api/core_package_structs.md#struct-string) 并返回，其行为等同于 [String](../../core/core_package_api/core_package_structs.md#struct-string).[fromUtf8Uncheck](../../core/core_package_api/core_package_structs.md#static-func-fromutf8uncheckedarrayuint8)([Array](../../core/core_package_api/core_package_structs.md#struct-arrayt)\<[Byte](../../core/core_package_api/core_package_types.md#type-byte)>)。

返回值：

- ?[String](../../core/core_package_api/core_package_structs.md#struct-string) - 读取到的行数据，读取失败返回 `None`。

示例：

<!-- compile -->
```cangjie
import std.env.*

main() {
    getStdOut().write("请输入信息：")
    var c = getStdIn().readln() // 输入：abc
    var r = c.getOrThrow()
    getStdOut().write("输入的信息为：")
    getStdOut().writeln(r)

    return
}
```

运行结果：

```text
请输入信息：abc
输入的信息为：abc
```

### func readToEnd()

```cangjie
public func readToEnd(): ?String
```

功能：从标准输入中读取所有字符。

该函数在终端规范模式下工作，读取过程如下：

1. 读取输入直到遇到文件结束符 EOF
2. 在 Linux 环境下，按 `Ctrl+D` 触发 EOF：
   - 若输入缓冲区有内容但未按回车，需按两次 `Ctrl+D`（第一次返回缓冲区内容，第二次触发 EOF）
   - 若输入缓冲区为空，按一次 `Ctrl+D` 即可触发 EOF
   - 若已按回车，再按 `Ctrl+D` 即可触发 EOF
3. 在 Windows 环境下，需按 `Ctrl+Z` 后加回车触发 EOF

读取成功返回 ?[String](../../core/core_package_api/core_package_structs.md#struct-string)，无输入或到达 EOF 时返回 `None`。该接口不会抛出异常，即使输入不符合 `UTF-8` 编码的字符串，也会构造出一个 [String](../../core/core_package_api/core_package_structs.md#struct-string) 并返回，其行为等同于 [String](../../core/core_package_api/core_package_structs.md#struct-string).[fromUtf8Uncheck](../../core/core_package_api/core_package_structs.md#static-func-fromutf8uncheckedarrayuint8)([Array](../../core/core_package_api/core_package_structs.md#struct-arrayt)\<[Byte](../../core/core_package_api/core_package_types.md#type-byte)>)。

> **注意：**
>
> 由于该函数使用终端规范模式（行缓冲），输入内容后需按回车或 `Ctrl+D` 才能被读取。若需按键即时响应（如游戏、TUI 应用），当前暂不支持。

返回值：

- ?[String](../../core/core_package_api/core_package_structs.md#struct-string) - 将读取到的所有数据以 ?[String](../../core/core_package_api/core_package_structs.md#struct-string) 的形式返回。

示例：

<!-- compile -->
```cangjie
import std.console.*

main() {
    // 获取标准输入流
    let stdin = Console.stdIn

    // 从标准输入读取所有字符直到遇到EOF
    // 注意：在实际运行中，程序会等待用户输入，直到遇到EOF（Ctrl+D或Ctrl+Z）
    // 当前示例假设输入abcde之后按Ctrl+D两次
    // 若输入缓冲区不为空（比如你刚输入了 abcde 但没按回车）：Ctrl+D 仅将缓冲区的内容 “flush（刷新）” 给程序，不触发 EOF
    // 若输入缓冲区为空（比如你刚按了回车，光标在新行开头，没有未提交的输入）：Ctrl+D 才会向程序发送 EOF 信号，告知 “没有更多输入了”。）
    let str = stdin.readToEnd()
    println("输入的是: ${str}")
}
```

运行结果：

```text
abcde输入的是: Some(abcde)
```

### func readUntil((Rune) -> Bool)

```cangjie
public func readUntil(predicate: (Rune) -> Bool): ?String
```

功能：从标准输入中读取数据直到读取到的字符满足 `predicate` 条件结束。

满足 predicate: (Rune) -> [Bool](../../core/core_package_api/core_package_intrinsics.md#bool) 条件的字符包含在结果中，读取失败时会返回 `None`。

参数：

- predicate: (Rune) ->[Bool](../../core/core_package_api/core_package_intrinsics.md#bool) - 终止读取的条件。

返回值：

- ?[String](../../core/core_package_api/core_package_structs.md#struct-string) - 将读取到的数据以 ?[String](../../core/core_package_api/core_package_structs.md#struct-string) 的形式返回。

示例：

<!-- compile -->
```cangjie
import std.env.*

main() {
    getStdOut().write("请输入信息：")
    var c = getStdIn().readUntil({ch: Rune => ch > r'd' && ch < r'g'}) // 输入：abcdefg
    var r = c.getOrThrow()
    getStdOut().writeln("输入的信息为：" + r)

    return
}
```

运行结果：

```text
请输入信息：abcdefg
输入的信息为：abcde
```

### func readUntil(Rune)

```cangjie
public func readUntil(ch: Rune): ?String
```

功能：从标准输入中读取数据直到读取到字符 `ch` 结束。

`ch` 包含在结果中，如果读取到文件结束符 EOF，将返回读取到的所有信息，读取失败时会返回 `None`。

参数：

- ch: [Rune](../../core/core_package_api/core_package_intrinsics.md#rune) - 终止字符。

返回值：

- ?[String](../../core/core_package_api/core_package_structs.md#struct-string) - 将读取到的数据以 ?[String](../../core/core_package_api/core_package_structs.md#struct-string) 的形式返回。

示例：

<!-- compile -->
```cangjie
import std.env.*

main() {
    getStdOut().write("请输入信息：")
    var c = getStdIn().readUntil(r'b') // 输入：abc
    var r = c.getOrThrow()
    getStdOut().write("输入的信息为：")
    getStdOut().writeln(r)

    return
}
```

运行结果：

```text
请输入信息：abc
输入的信息为：ab
```

## class ConsoleWriter

```cangjie
public class ConsoleWriter <: OutputStream {}
```

功能：此类提供保证线程安全的标准输出功能。

每次 write 调用写到控制台的结果是完整的，不同的 write 函数调用的结果不会混合到一起。
该类型无法构造实例，只能通过 [getStdOut()](./env_package_funcs.md#func-getstdout) 获取标准输出实例或者  [getStdErr()](./env_package_funcs.md#func-getstderr) 获取标准错误的实例。

父类型：

- [OutputStream](../../io/io_package_api/io_package_interfaces.md#interface-outputstream)

### func flush()

```cangjie
public func flush(): Unit
```

功能：刷新输出流。

示例：

<!-- verify -->
```cangjie
import std.env.*

main() {
    // 获取标准输出实例
    let stdout = getStdOut()

    // 写入一些数据但不换行
    stdout.write("Hello, ")
    stdout.write("World!")

    // 刷新输出流以确保数据被写入
    stdout.flush()

    // 再写入一行
    stdout.writeln(" Flush completed.")
}
```

运行结果：

```text
Hello, World! Flush completed.
```

### func write(Array\<Byte>)

```cangjie
public func write(buffer: Array<Byte>): Unit
```

功能：指定的将字节数组 buffer 写入标准输出或标准错误流中。

参数：

- buffer: [Array](../../core/core_package_api/core_package_structs.md#struct-arrayt)\<[Byte](../../core/core_package_api/core_package_types.md#type-byte)> - 要被写入的字节数组。

示例：

<!-- verify -->
```cangjie
import std.env.*

main() {
    // 获取标准输出实例
    let stdout = getStdOut()

    // 创建一个字节数组，包含 "Hello" 的 ASCII 码
    let bytes = [72, 101, 108, 108, 111] // "Hello" 的 ASCII 码

    // 写入字节数组
    stdout.write(bytes)

    // 写入换行
    stdout.writeln("")
}
```

运行结果：

```text
[72, 101, 108, 108, 111]
```

### func write(Bool)

```cangjie
public func write(v: Bool): Unit
```

功能：将指定的布尔值的文本表示形式写入标准输出或标准错误流中。

参数：

- v: [Bool](../../core/core_package_api/core_package_intrinsics.md#bool) - 要写入的值。

示例：

<!-- verify -->
```cangjie
import std.env.*

main() {
    // 获取标准输出实例
    let stdout = getStdOut()

    // 写入布尔值
    stdout.write(true)
    stdout.write(" ")
    stdout.write(false)

    // 写入换行
    stdout.writeln("")
}
```

运行结果：

```text
true false
```

### func write(Float16)

```cangjie
public func write(v: Float16): Unit
```

功能：将指定的 16 位浮点数值的文本表示写入标准输出或标准错误流中。

参数：

- v: [Float16](../../core/core_package_api/core_package_intrinsics.md#float16) - 要写入的值。

示例：

<!-- verify -->
```cangjie
import std.env.*

main() {
    // 获取标准输出实例
    let stdout = getStdOut()

    // 写入 Float16 值
    stdout.write(3.14f16)

    // 写入换行
    stdout.writeln("")
}
```

运行结果：

```text
3.140625
```

### func write(Float32)

```cangjie
public func write(v: Float32): Unit
```

功能：将指定的 32 位浮点数值的文本表示写入标准输出或标准错误流中。

参数：

- v: [Float32](../../core/core_package_api/core_package_intrinsics.md#float32) - 要写入的值。

示例：

<!-- verify -->
```cangjie
import std.env.*

main() {
    // 获取标准输出实例
    let stdout = getStdOut()

    // 写入 Float32 值
    stdout.write(3.14159f32)

    // 写入换行
    stdout.writeln("")
}
```

运行结果：

```text
3.141590
```

### func write(Float64)

```cangjie
public func write(v: Float64): Unit
```

功能：将指定的 64 位浮点数值的文本表示写入标准输出或标准错误流中。

参数：

- v: [Float64](../../core/core_package_api/core_package_intrinsics.md#float64) - 要写入的值。

示例：

<!-- verify -->
```cangjie
import std.env.*

main() {
    // 获取标准输出实例
    let stdout = getStdOut()

    // 写入 Float64 值
    stdout.write(3.141592653589793f64)

    // 写入换行
    stdout.writeln("")
}
```

运行结果：

```text
3.141593
```

### func write(Int16)

```cangjie
public func write(v: Int16): Unit
```

功能：将指定的 16 位有符号整数值的文本表示写入标准输出或标准错误流中。

参数：

- v: [Int16](../../core/core_package_api/core_package_intrinsics.md#int16) - 要写入的值。

示例：

<!-- verify -->
```cangjie
import std.env.*

main() {
    // 获取标准输出实例
    let stdout = getStdOut()

    // 写入 Int16 值
    stdout.write(12345i16)

    // 写入换行
    stdout.writeln("")
}
```

运行结果：

```text
12345
```

### func write(Int32)

```cangjie
public func write(v: Int32): Unit
```

功能：将指定的 32 位有符号整数值的文本表示写入标准输出或标准错误流中。

参数：

- v: [Int32](../../core/core_package_api/core_package_intrinsics.md#int32) - 要写入的值。

示例：

<!-- verify -->
```cangjie
import std.env.*

main() {
    // 获取标准输出实例
    let stdout = getStdOut()

    // 写入 Int32 值
    stdout.write(123456789i32)

    // 写入换行
    stdout.writeln("")
}
```

运行结果：

```text
123456789
```

### func write(Int64)

```cangjie
public func write(v: Int64): Unit
```

功能：将指定的 64 位有符号整数值的文本表示写入标准输出或标准错误流中。

参数：

- v: [Int64](../../core/core_package_api/core_package_intrinsics.md#int64) - 要写入的值。

示例：

<!-- verify -->
```cangjie
import std.env.*

main() {
    // 获取标准输出实例
    let stdout = getStdOut()

    // 写入 Int64 值
    stdout.write(123456789012345i64)

    // 写入换行
    stdout.writeln("")
}
```

运行结果：

```text
123456789012345
```

### func write(Int8)

```cangjie
public func write(v: Int8): Unit
```

功能：将指定的 8 位有符号整数值的文本表示写入标准输出或标准错误流中。

参数：

- v: [Int8](../../core/core_package_api/core_package_intrinsics.md#int8) - 要写入的值。

示例：

<!-- verify -->
```cangjie
import std.env.*

main() {
    // 获取标准输出实例
    let stdout = getStdOut()

    // 写入 Int8 值
    stdout.write(42i8)

    // 写入换行
    stdout.writeln("")
}
```

运行结果：

```text
42
```

### func write(Rune)

```cangjie
public func write(v: Rune): Unit
```

功能：将指定的 [Rune](../../core/core_package_api/core_package_intrinsics.md#rune) 的 Unicode 字符值写入标准输出或标准错误流中。

参数：

- v: [Rune](../../core/core_package_api/core_package_intrinsics.md#rune) - 要写入的值。

示例：

<!-- verify -->
```cangjie
import std.env.*

main() {
    // 获取标准输出实例
    let stdout = getStdOut()

    // 写入 Rune 值
    stdout.write(r'A')

    // 写入换行
    stdout.writeln("")
}
```

运行结果：

```text
A
```

### func write(String)

```cangjie
public func write(v: String): Unit
```

功能：将指定的字符串值写入标准输出或标准错误流中。

参数：

- v: [String](../../core/core_package_api/core_package_structs.md#struct-string) - 要写入的值。

示例：

<!-- verify -->
```cangjie
import std.env.*

main() {
    // 获取标准输出实例
    let stdout = getStdOut()

    // 写入字符串
    stdout.write("Hello, World!")

    // 写入换行
    stdout.writeln("")
}
```

运行结果：

```text
Hello, World!
```

### func write(UInt16)

```cangjie
public func write(v: UInt16): Unit
```

功能：将指定的 16 位无符号整数值的文本表示写入标准输出或标准错误流中。

参数：

- v: [UInt16](../../core/core_package_api/core_package_intrinsics.md#uint16) - 要写入的值。

示例：

<!-- verify -->
```cangjie
import std.env.*

main() {
    // 获取标准输出实例
    let stdout = getStdOut()

    // 写入 UInt16 值
    stdout.write(12345u16)

    // 写入换行
    stdout.writeln("")
}
```

运行结果：

```text
12345
```

### func write(UInt32)

```cangjie
public func write(v: UInt32): Unit
```

功能：将指定的 32 位无符号整数值的文本表示写入标准输出或标准错误流中。

参数：

- v: [UInt32](../../core/core_package_api/core_package_intrinsics.md#uint32) - 要写入的值。

示例：

<!-- verify -->
```cangjie
import std.env.*

main() {
    // 获取标准输出实例
    let stdout = getStdOut()

    // 写入 UInt32 值
    stdout.write(123456789u32)

    // 写入换行
    stdout.writeln("")
}
```

运行结果：

```text
123456789
```

### func write(UInt64)

```cangjie
public func write(v: UInt64): Unit
```

功能：将指定的 64 位无符号整数值的文本表示写入标准输出或标准错误流中。

参数：

- v: [UInt64](../../core/core_package_api/core_package_intrinsics.md#uint64) - 要写入的值。

示例：

<!-- verify -->
```cangjie
import std.env.*

main() {
    // 获取标准输出实例
    let stdout = getStdOut()

    // 写入 UInt64 值
    stdout.write(123456789012345u64)

    // 写入换行
    stdout.writeln("")
}
```

运行结果：

```text
123456789012345
```

### func write(UInt8)

```cangjie
public func write(v: UInt8): Unit
```

功能：将指定的 8 位无符号整数值的文本表示写入标准输出或标准错误流中。

参数：

- v: [UInt8](../../core/core_package_api/core_package_intrinsics.md#uint8) - 要写入的值。

示例：

<!-- verify -->
```cangjie
import std.env.*

main() {
    // 获取标准输出实例
    let stdout = getStdOut()

    // 写入 UInt8 值
    stdout.write(42u8)

    // 写入换行
    stdout.writeln("")
}
```

运行结果：

```text
42
```

### func write\<T>(T) where T <: ToString

```cangjie
public func write<T>(v: T): Unit where T <: ToString
```

功能：将实现了 [ToString](../../core/core_package_api/core_package_interfaces.md#interface-tostring) 接口的数据类型写入标准输出或标准错误流中。

参数：

- v: T - 要被写入的 [ToString](../../core/core_package_api/core_package_interfaces.md#interface-tostring) 类型的实例。

示例：

<!-- verify -->
```cangjie
import std.env.*

// 定义一个实现了 ToString 接口的类
class Person <: ToString {
    var name: String
    var age: Int64

    public init(name: String, age: Int64) {
        this.name = name
        this.age = age
    }

    public func toString(): String {
        return "Person(name: ${this.name}, age: ${this.age})"
    }
}

main() {
    // 获取标准输出实例
    let stdout = getStdOut()

    // 创建一个 Person 实例
    let person = Person("Alice", 30)

    // 使用泛型 write 方法写入 Person 实例
    stdout.write(person)

    // 写入换行
    stdout.writeln("")
}
```

运行结果：

```text
Person(name: Alice, age: 30)
```

### func writeln(Array\<Byte>)

```cangjie
public func writeln(buffer: Array<Byte>): Unit
```

功能：指定的将字节数组 buffer （后跟换行符）写入标准输出或标准错误流中。

参数：

- buffer: [Array](../../core/core_package_api/core_package_structs.md#struct-arrayt)\<[Byte](../../core/core_package_api/core_package_types.md#type-byte)> - 要写入的值。

示例：

<!-- verify -->
```cangjie
import std.env.*

main() {
    // 获取标准输出实例
    let stdout = getStdOut()

    // 创建一个字节数组，包含 "Hello" 的 ASCII 码
    let bytes = [72, 101, 108, 108, 111] // "Hello" 的 ASCII 码

    // 写入字节数组并换行
    stdout.writeln(bytes)
}
```

运行结果：

```text
[72, 101, 108, 108, 111]
```

### func writeln(Bool)

```cangjie
public func writeln(v: Bool): Unit
```

功能：将指定的布尔值的文本表示形式（后跟换行符）写入标准输出或标准错误流中。

参数：

- v: [Bool](../../core/core_package_api/core_package_intrinsics.md#bool) - 要写入的值。

示例：

<!-- verify -->
```cangjie
import std.env.*

main() {
    // 获取标准输出实例
    let stdout = getStdOut()

    // 写入布尔值并换行
    stdout.writeln(true)

    // 再写入一个布尔值并换行
    stdout.writeln(false)
}
```

运行结果：

```text
true
false
```

### func writeln(Float16)

```cangjie
public func writeln(v: Float16): Unit
```

功能：将指定的 16 位浮点数值的文本表示（后跟换行符）写入标准输出或标准错误流中。

参数：

- v: [Float16](../../core/core_package_api/core_package_intrinsics.md#float16) - 要写入的值。

示例：

<!-- verify -->
```cangjie
import std.env.*

main() {
    // 获取标准输出实例
    let stdout = getStdOut()

    // 写入 Float16 值并换行
    stdout.writeln(3.14f16)
}
```

运行结果：

```text
3.140625
```

### func writeln(Float32)

```cangjie
public func writeln(v: Float32): Unit
```

功能：将指定的 32 位浮点数值的文本表示（后跟换行符）写入标准输出或标准错误流中。

参数：

- v: [Float32](../../core/core_package_api/core_package_intrinsics.md#float32) - 要写入的值。

示例：

<!-- verify -->
```cangjie
import std.env.*

main() {
    // 获取标准输出实例
    let stdout = getStdOut()

    // 写入 Float32 值并换行
    stdout.writeln(3.14159f32)
}
```

运行结果：

```text
3.141590
```

### func writeln(Float64)

```cangjie
public func writeln(v: Float64): Unit
```

功能：将指定的 64 位浮点数值的文本表示（后跟换行符）写入标准输出或标准错误流中。

参数：

- v: [Float64](../../core/core_package_api/core_package_intrinsics.md#float64) - 要写入的值。

示例：

<!-- verify -->
```cangjie
import std.env.*

main() {
    // 获取标准输出实例
    let stdout = getStdOut()

    // 写入 Float64 值并换行
    stdout.writeln(3.141592653589793f64)
}
```

运行结果：

```text
3.141593
```

### func writeln(Int16)

```cangjie
public func writeln(v: Int16): Unit
```

功能：将指定的 16 位有符号整数值的文本表示（后跟换行符）写入标准输出或标准错误流中。

参数：

- v: [Int16](../../core/core_package_api/core_package_intrinsics.md#int16) - 要写入的值。

示例：

<!-- verify -->
```cangjie
import std.env.*

main() {
    // 获取标准输出实例
    let stdout = getStdOut()

    // 写入 Int16 值并换行
    stdout.writeln(12345i16)
}
```

运行结果：

```text
12345
```

### func writeln(Int32)

```cangjie
public func writeln(v: Int32): Unit
```

功能：将指定的 32 位有符号整数值的文本表示（后跟换行符）写入标准输出或标准错误流中。

参数：

- v: [Int32](../../core/core_package_api/core_package_intrinsics.md#int32) - 要写入的值。

示例：

<!-- verify -->
```cangjie
import std.env.*

main() {
    // 获取标准输出实例
    let stdout = getStdOut()

    // 写入 Int32 值并换行
    stdout.writeln(123456789i32)
}
```

运行结果：

```text
123456789
```

### func writeln(Int64)

```cangjie
public func writeln(v: Int64): Unit
```

功能：将指定的 64 位有符号整数值的文本表示（后跟换行符）写入标准输出或标准错误流中。

参数：

- v: [Int64](../../core/core_package_api/core_package_intrinsics.md#int64) - 要写入的值。

示例：

<!-- verify -->
```cangjie
import std.env.*

main() {
    // 获取标准输出实例
    let stdout = getStdOut()

    // 写入 Int64 值并换行
    stdout.writeln(123456789012345i64)
}
```

运行结果：

```text
123456789012345
```

### func writeln(Int8)

```cangjie
public func writeln(v: Int8): Unit
```

功能：将指定的 8 位有符号整数值的文本表示（后跟换行符）写入标准输出或标准错误流中。

参数：

- v: [Int8](../../core/core_package_api/core_package_intrinsics.md#int8) - 要写入的值。

示例：

<!-- verify -->
```cangjie
import std.env.*

main() {
    // 获取标准输出实例
    let stdout = getStdOut()

    // 写入 Int8 值并换行
    stdout.writeln(42i8)
}
```

运行结果：

```text
42
```

### func writeln(Rune)

```cangjie
public func writeln(v: Rune): Unit
```

功能：将指定的 Unicode 字符值（后跟换行符）写入标准输出或标准错误流中。

参数：

- v: Rune - 要写入的值。

示例：

<!-- verify -->
```cangjie
import std.env.*

main() {
    // 获取标准输出实例
    let stdout = getStdOut()

    // 写入 Rune 值并换行
    stdout.writeln(r'A')
}
```

运行结果：

```text
A
```

### func writeln(String)

```cangjie
public func writeln(v: String): Unit
```

功能：将指定的字符串值（后跟换行符）写入标准输出或标准错误流中。

参数：

- v: [String](../../core/core_package_api/core_package_structs.md#struct-string) - 要写入的值。

示例：

<!-- verify -->
```cangjie
import std.env.*

main() {
    // 获取标准输出实例
    let stdout = getStdOut()

    // 写入字符串并换行
    stdout.writeln("Hello, World!")
}
```

运行结果：

```text
Hello, World!
```

### func writeln(UInt16)

```cangjie
public func writeln(v: UInt16): Unit
```

功能：将指定的 16 位无符号整数值的文本表示（后跟换行符）写入标准输出或标准错误流中。

参数：

- v: [UInt16](../../core/core_package_api/core_package_intrinsics.md#uint16) - 要写入的值。

示例：

<!-- verify -->
```cangjie
import std.env.*

main() {
    // 获取标准输出实例
    let stdout = getStdOut()

    // 写入 UInt16 值并换行
    stdout.writeln(12345u16)
}
```

运行结果：

```text
12345
```

### func writeln(UInt32)

```cangjie
public func writeln(v: UInt32): Unit
```

功能：将指定的 32 位无符号整数值的文本表示（后跟换行符）写入标准输出或标准错误流中。

参数：

- v: [UInt32](../../core/core_package_api/core_package_intrinsics.md#uint32) - 要写入的值。

示例：

<!-- verify -->
```cangjie
import std.env.*

main() {
    // 获取标准输出实例
    let stdout = getStdOut()

    // 写入 UInt32 值并换行
    stdout.writeln(123456789u32)
}
```

运行结果：

```text
123456789
```

### func writeln(UInt64)

```cangjie
public func writeln(v: UInt64): Unit
```

功能：将指定的 64 位无符号整数值的文本表示（后跟换行符）写入标准输出或标准错误流中。

参数：

- v: [UInt64](../../core/core_package_api/core_package_intrinsics.md#uint64) - 要写入的值。

示例：

<!-- verify -->
```cangjie
import std.env.*

main() {
    // 获取标准输出实例
    let stdout = getStdOut()

    // 写入 UInt64 值并换行
    stdout.writeln(123456789012345u64)
}
```

运行结果：

```text
123456789012345
```

### func writeln(UInt8)

```cangjie
public func writeln(v: UInt8): Unit
```

功能：将指定的 8 位无符号整数值的文本表示（后跟换行符）写入标准输出或标准错误流中。

参数：

- v: [UInt8](../../core/core_package_api/core_package_intrinsics.md#uint8) - 要写入的值。

示例：

<!-- verify -->
```cangjie
import std.env.*

main() {
    // 获取标准输出实例
    let stdout = getStdOut()

    // 写入 UInt8 值并换行
    stdout.writeln(42u8)
}
```

运行结果：

```text
42
```

### func writeln\<T>(T) where T <: ToString

```cangjie
public func writeln<T>(v: T): Unit where T <: ToString
```

功能：将实现了 [ToString](../../core/core_package_api/core_package_interfaces.md#interface-tostring) 接口的数据类型转换成的字符串（后跟换行符）写入标准输出或标准错误流中。

参数：

- v: T - 要写入的值。

示例：

<!-- verify -->
```cangjie
import std.env.*

// 定义一个实现了 ToString 接口的类
class Person <: ToString {
    var name: String
    var age: Int64

    public init(name: String, age: Int64) {
        this.name = name
        this.age = age
    }

    public func toString(): String {
        return "Person(name: ${this.name}, age: ${this.age})"
    }
}

main() {
    // 获取标准输出实例
    let stdout = getStdOut()

    // 创建一个 Person 实例
    let person = Person("Alice", 30)

    // 使用泛型 writeln 方法写入 Person 实例并换行
    stdout.writeln(person)
}
```

运行结果：

```text
Person(name: Alice, age: 30)
```
