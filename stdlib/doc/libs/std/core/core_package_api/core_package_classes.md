# 类

## class ArrayIterator\<T>

```cangjie
public class ArrayIterator<T> <: Iterator<T>
```

功能：数组迭代器，迭代功能详述见 [Iterable](core_package_interfaces.md#interface-iterablee) 和 [Iterator](core_package_classes.md#class-iteratort) 说明。

父类型：

- [Iterator](#class-iteratort)\<T>

### init(Array\<T>)

```cangjie
public init(data: Array<T>)
```

功能：给定一个 [Array](core_package_structs.md#struct-arrayt) 数组实例，创建其对应的迭代器，用来迭代遍历该数组实例中全部对象。

参数：

- data: [Array](core_package_structs.md#struct-arrayt)\<T> - 数组实例。

示例：

<!-- verify -->
```cangjie
main() {
    var arr: Array<Int64> = [1, 2, 3]
    // 初始化迭代器
    let ai = ArrayIterator(arr)
    println("迭代器初始化成功")
    return 0
}
```

运行结果：

```text
迭代器初始化成功
```

### func next()

```cangjie
public func next(): Option<T>
```

功能：返回数组迭代器中的下一个值。

返回值：

- [Option](core_package_enums.md#enum-optiont)\<T> - 数组迭代器中的下一个成员，用 [Option](core_package_enums.md#enum-optiont) 封装，迭代到末尾时返回 `None`。

示例：

<!-- verify -->

```cangjie
main() {
    var arr: Array<Int64> = [1, 2, 3, 4]
    var arrIterator: ArrayIterator<Int64> = ArrayIterator(arr)
    var num: Option<Int64>
    while (true) {
        num = arrIterator.next()
        if (let Some(element) <- num) {
            println(element)
        } else {
            break
        }
    }
}
```

运行结果：

```text
1
2
3
4
```

## class Box\<T>

```cangjie
public class Box<T>
```

功能：[Box](core_package_classes.md#class-boxt) 类型提供了为其他类型添加一层 `class` 封装的能力。

如果 `T` 类型本身不具备引用能力，如 `struct` 类型，封装后 [Box](core_package_classes.md#class-boxt)\<T> 类型将可被引用。

扩展：

- [extend\<T> Box\<T> <: Comparable\<Box\<T>> where T <: Comparable\<T>](core_package_extends.md#extendt-boxt--comparableboxt-where-t--comparablet)
- [extend\<T> Box\<T> <: Hashable where T <: Hashable](core_package_extends.md#extendt-boxt--hashable-where-t--hashable)
- [extend\<T> Box\<T> <: ToString where T <: ToString](core_package_extends.md#extendt-boxt--tostring-where-t--tostring)

### var value

```cangjie
public var value: T
```

功能：获取或修改被包装的值。

类型：T

示例：

<!-- verify -->
```cangjie
main() {
    var box: Box<Int64> = Box<Int64>(42)

    // 获取被包装的值
    println("Box contains: ${box.value}")

    // 修改被包装的值
    box.value = 100
    println("Box now contains: ${box.value}")
}
```

运行结果：

```text
Box contains: 42
Box now contains: 100
```

### init(T)

```cangjie
public init(v: T)
```

功能：给定 `T` 类型实例，构造对应的 [Box](core_package_classes.md#class-boxt)\<T> 实例。

参数：

- v: T - 任意类型实例。

示例：

<!-- verify -->
```cangjie
main() {
    // 使用整数创建Box实例
    var intBox: Box<Int64> = Box<Int64>(42)
    println("Integer box contains: ${intBox.value}")

    // 使用字符串创建Box实例
    var stringBox: Box<String> = Box<String>("Hello, Box!")
    println("String box contains: ${stringBox.value}")
}
```

运行结果：

```text
Integer box contains: 42
String box contains: Hello, Box!
```

## class Future\<T>

```cangjie
public class Future<T>
```

功能：[Future](core_package_classes.md#class-futuret)\<T> 实例代表一个仓颉线程任务，可用于获取仓颉线程的计算结果，向仓颉线程发送取消信号。

`spawn` 表达式的返回类型是 [Future](core_package_classes.md#class-futuret)\<T>，其中 `T` 的类型取决于 `spawn` 表达式中的闭包的返回值类型。

### prop thread

```cangjie
public prop thread: Thread
```

功能：获得对应仓颉线程的 [Thread](core_package_classes.md#class-thread) 实例。

类型：[Thread](core_package_classes.md#class-thread)

示例：

<!-- verify -->
```cangjie
main(): Int64 {
    let future: Future<Int64> = spawn {
        => return 42
    }

    // 获取Future对应的线程实例
    let thread: Thread = future.thread
    println("Thread id: ${thread.id}")

    // 等待线程完成
    let result: Int64 = future.get()
    println("Result: ${result}")

    return 0
}
```

运行结果：

```text
Thread id: 2
Result: 42
```

### func cancel()

```cangjie
public func cancel(): Unit
```

功能：给当前 [Future](core_package_classes.md#class-futuret) 实例对应的仓颉线程发送取消请求。该方法不会立即停止线程执行，仅发送请求，相应地，[Thread](core_package_classes.md#class-thread) 类的函数 `hasPendingCancellation` 可用于检查线程是否存在取消请求，开发者可以通过该检查来自行决定是否提前终止线程以及如何终止线程。

示例：

<!-- verify -->

```cangjie
main(): Unit {
    /* 创建线程 */
    let future = spawn {
        while (true) {
            if (Thread.currentThread.hasPendingCancellation) {
                return 0
            }
        }
        return 1
    }
    /* 向线程发送取消请求 */
    future.cancel()
    let res = future.get()
    println(res)
}
```

运行结果：

```text
0
```

### func get()

```cangjie
public func get(): T
```

功能：阻塞当前线程，等待并获取当前 [Future](core_package_classes.md#class-futuret)\<T> 对象对应的线程的结果。

返回值：

- T - 当前 [Future](core_package_classes.md#class-futuret)\<T> 实例代表的线程运行结束后的返回值。

示例：

<!-- verify -->

```cangjie
main(): Int64 {
    let fut: Future<Int64> = spawn {
        =>
            sleep(1000 * Duration.millisecond) /* 睡眠 1 秒 */
            return 1
    }

    /* 等待线程完成 */
    let result: Int64 = fut.get()
    println(result)
    return 0
}
```

运行结果：

```text
1
```

### func get(Duration)

```cangjie
public func get(timeout: Duration): T
```

功能：阻塞当前线程，等待指定时长并获取当前 [Future](core_package_classes.md#class-futuret)\<T> 对象对应的线程的返回值。

需指定等待的超时时间，如果相应的线程在指定时间内未完成执行，则该函数将抛出异常[TimeoutException](./core_package_exceptions.md#class-timeoutexception)。如果 timeout <= Duration.Zero，等同于 get()，即不限制等待时长。如果线程抛出异常退出执行，在 get 调用处将继续抛出该异常。

参数：

- timeout: [Duration](./core_package_structs.md#struct-duration) - 等待时间。

返回值：

- T - 返回指定时长后仓颉线程执行结果。

异常：

- [TimeoutException](./core_package_exceptions.md#class-timeoutexception) - 如果相应的线程在指定时间内未完成执行，则该函数将抛出此异常。

示例：

<!-- verify -->

```cangjie
main(): Int64 {
    let fut: Future<Int64> = spawn {
        =>
            sleep(1000 * Duration.millisecond) /* 睡眠 1 秒 */
            return 1
    }

    let result: Int64 = fut.get(2000 * Duration.millisecond)
    /* 最大等待时间为 2 秒， 超过该时间抛出 TimeoutException */

    println(result)
    return 0
}
```

运行结果：

```text
1
```

### func tryGet()

```cangjie
public func tryGet(): Option<T>
```

功能：尝试获取执行结果，不会阻塞当前线程。如果相应的线程未完成，则该函数返回 `None`。

返回值：

- [Option](core_package_enums.md#enum-optiont)\<T> - 如果当前仓颉线程未完成返回 `None`，否则返回执行结果。

示例：

<!-- verify -->

```cangjie
main(): Int64 {
    let fut: Future<Int64> = spawn {
        =>
            sleep(1000 * Duration.millisecond) /* 睡眠 1 秒 */
            return 1
    }

    /* 主线程等待 4 秒，保证创建线程已经完成 */
    sleep(4000 * Duration.millisecond)

    /* 尝试获取创建线程的运行结果 */
    let result: Option<Int64> = fut.tryGet()
    println(result)
    return 0
}
```

运行结果：

```text
Some(1)
```

## class Iterator\<T>

```cangjie
public abstract class Iterator<T> <: Iterable<T>
```

功能：该类表示迭代器，提供 `next` 方法支持对容器内的成员进行迭代遍历。

父类型：

- [Iterable](core_package_interfaces.md#interface-iterablee)\<T>

扩展：

- [extend\<T> Iterator\<T>](core_package_extends.md#extendt-iteratort)
- [extend\<T> Iterator\<T> where T <: Comparable\<T>](core_package_extends.md#extendt-iteratort-where-t--comparablet)
- [extend\<T> Iterator\<T> where T <: Equatable\<T>](core_package_extends.md#extendt-iteratort-where-t--equatablet)

### init()

```cangjie
public init()
```

功能：构造一个默认的 [Iterator](core_package_classes.md#class-iteratort)\<T> 对象。

示例：

<!-- verify -->
```cangjie
main(): Int64 {
    // 抽象类 Iterator 本身无法初始化
    MyIterator<Int64>()
    println("抽象的子类完成了初始化")
    return 0
}

public class MyIterator<T> <: Iterator<T> {
    public init() {}
    public func next(): ?T {
        return None<T>
    }
}
```

运行结果：

```text
抽象的子类完成了初始化
```

### func iterator()

```cangjie
public func iterator(): Iterator<T>
```

功能：返回迭代器自身。

返回值：

- [Iterator](core_package_classes.md#class-iteratort)\<T> - 迭代器自身。

示例：

<!-- verify -->
```cangjie
main(): Int64 {
    // 创建一个数组并获取其迭代器
    var arr: Array<Int64> = [10, 20, 30]
    var iter: Iterator<Int64> = arr.iterator()

    // 调用iterator()方法获取迭代器自身
    var iter2: Iterator<Int64> = iter.iterator()

    // 验证两个迭代器是同一个对象
    println("Iterators are the same object")

    // 使用两个迭代器遍历元素
    println("打印迭代器 iter 的第一个元素: ${iter.next()}")
    println("打印迭代器 iter2 的第二个元素: ${iter2.next()}")
    println("打印迭代器 iter 的第三个元素: ${iter.next()}")
    println("打印迭代器 iter2 的第四个元素: ${iter2.next()}")

    return 0
}
```

运行结果：

```text
Iterators are the same object
打印迭代器 iter 的第一个元素: Some(10)
打印迭代器 iter2 的第二个元素: Some(20)
打印迭代器 iter 的第三个元素: Some(30)
打印迭代器 iter2 的第四个元素: None
```

### func next()

```cangjie
public func next(): Option<T>
```

功能：获取迭代过程中的下一个元素。

返回值：

- [Option](core_package_enums.md#enum-optiont)\<T> - 迭代过程中的下一个元素。

示例：

<!-- verify -->

```cangjie
main(): Int64 {
    var arr: Array<Int64> = [1, 2, 3, 4, 5]
    var iter = arr.iterator() /* 获取容器的迭代器对象 */

    while (true) { /* 使用迭代器进行遍历 */
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
5
```

## class Object

```cangjie
public open class Object <: Any
```

功能：[Object](core_package_classes.md#class-object) 是所有 `class` 的父类，所有 `class` 都默认继承它。[Object](core_package_classes.md#class-object) 类中不包含任何成员，即 [Object](core_package_classes.md#class-object) 是一个“空”的类。

父类型：

- [Any](core_package_interfaces.md#interface-any)

### init()

```cangjie
public const init()
```

功能：构造一个 `object` 实例。

示例：

<!-- run -->
```cangjie
main(): Int64 {
    Object()
    return 0
}
```

## class RangeIterator\<T> <: Iterator\<T> where T <: Countable\<T> & Comparable\<T> & Equatable\<T>

```cangjie
public class RangeIterator<T> <: Iterator<T> where T <: Countable<T> & Comparable<T> & Equatable<T>
```

功能：[Range](core_package_structs.md#struct-ranget-where-t--countablet--comparablet--equatablet) 类型的迭代器，迭代功能详述见 [Iterable](core_package_interfaces.md#interface-iterablee) 和 [Iterator](core_package_classes.md#class-iteratort) 接口说明。

父类型：

- [Iterator](#class-iteratort)\<T>

### func next()

```cangjie
public func next(): Option<T>
```

功能：获取 [Range](core_package_structs.md#struct-ranget-where-t--countablet--comparablet--equatablet) 迭代器中的下一个值。

返回值：

- [Option](core_package_enums.md#enum-optiont)\<T> - [Range](core_package_structs.md#struct-ranget-where-t--countablet--comparablet--equatablet) 迭代器中的下一个成员，用 [Option](core_package_enums.md#enum-optiont) 封装，迭代到末尾时返回 `None`。

示例：

<!-- verify -->
```cangjie
main(): Int64 {
    // 创建一个Range并获取其迭代器
    let range: Range<Int64> = 1..=5
    var iter = range.iterator()

    // 使用迭代器遍历Range中的所有值
    println("Iterating through range 1..=5:")
    while (true) {
        match (iter.next()) {
            case Some(value) => println(value)
            case None => break
        }
    }

    return 0
}
```

运行结果：

```text
Iterating through range 1..=5:
1
2
3
4
5
```

## class StackTraceElement

```cangjie
public open class StackTraceElement <: ToString
```

功能：表示一个异常堆栈的具体信息，包括异常发生的类名、函数名、文件名、行号。

父类型：

- [ToString](core_package_interfaces.md#interface-tostring)

### let declaringClass

```cangjie
public let declaringClass: String
```

功能：获取异常发生的类名。

类型：[String](core_package_structs.md#struct-string)

示例：

<!-- verify -->
```cangjie
main(): Int64 {
    // 创建一个StackTraceElement实例
    let stackTraceElement = StackTraceElement(
        "MyClass",
        "myMethod",
        "MyClass.cj",
        42
    )

    // 获取并打印类名
    let className: String = stackTraceElement.declaringClass
    println("Class name: ${className}")

    return 0
}
```

运行结果：

```text
Class name: MyClass
```

### let fileName

```cangjie
public let fileName: String
```

功能：获取异常发生的文件名。

类型：[String](core_package_structs.md#struct-string)

示例：

<!-- verify -->
```cangjie
main(): Int64 {
    // 创建一个StackTraceElement实例
    let stackTraceElement = StackTraceElement(
        "MyClass",
        "myMethod",
        "MyClass.cj",
        42
    )

    // 获取并打印文件名
    let fileName: String = stackTraceElement.fileName
    println("File name: ${fileName}")

    return 0
}
```

运行结果：

```text
File name: MyClass.cj
```

### let lineNumber

```cangjie
public let lineNumber: Int64
```

功能：获取异常发生的行号。

类型：[Int64](core_package_intrinsics.md#int64)

示例：

<!-- verify -->
```cangjie
main(): Int64 {
    // 创建一个StackTraceElement实例
    let stackTraceElement = StackTraceElement(
        "MyClass",
        "myMethod",
        "MyClass.cj",
        42
    )

    // 获取并打印行号
    let lineNum: Int64 = stackTraceElement.lineNumber
    println("Line number: ${lineNum}")

    return 0
}
```

运行结果：

```text
Line number: 42
```

### let methodName

```cangjie
public let methodName: String
```

功能：获取异常发生的函数名。

类型：[String](core_package_structs.md#struct-string)

示例：

<!-- verify -->
```cangjie
main(): Int64 {
    // 创建一个StackTraceElement实例
    let stackTraceElement = StackTraceElement(
        "MyClass",
        "myMethod",
        "MyClass.cj",
        42
    )

    // 获取并打印函数名
    let methodName: String = stackTraceElement.methodName
    println("Method name: ${methodName}")

    return 0
}
```

运行结果：

```text
Method name: myMethod
```

### init(String, String, String, Int64)

```cangjie
public init(declaringClass: String, methodName: String, fileName: String, lineNumber: Int64)
```

功能：构造一个异常堆栈实例，指定类名、函数名、文件名、行号。

参数：

- declaringClass: [String](core_package_structs.md#struct-string) - 类名。
- methodName: [String](core_package_structs.md#struct-string) - 函数名。
- fileName: [String](core_package_structs.md#struct-string) - 文件名。
- lineNumber: [Int64](core_package_intrinsics.md#int64) - 行号。

示例：

<!-- verify -->
```cangjie
main(): Int64 {
    // 创建一个StackTraceElement实例
    let stackTraceElement = StackTraceElement(
        "MyClass",
        "myMethod",
        "MyClass.cj",
        42
    )

    // 打印堆栈跟踪元素的信息
    println("Class: ${stackTraceElement.declaringClass}")
    println("Method: ${stackTraceElement.methodName}")
    println("File: ${stackTraceElement.fileName}")
    println("Line: ${stackTraceElement.lineNumber}")

    return 0
}
```

运行结果：

```text
Class: MyClass
Method: myMethod
File: MyClass.cj
Line: 42
```

### func  toString()

```cangjie
public func toString(): String
```

功能：获取 [StackTraceElement](core_package_classes.md#class-stacktraceelement) 对象的字符串表示。

返回值：

- [String](core_package_structs.md#struct-string) - 转换后的字符串。

示例：

<!-- verify -->
```cangjie
main(): Int64 {
    // 创建一个StackTraceElement实例
    let stackTraceElement = StackTraceElement(
        "MyClass",
        "myMethod",
        "MyClass.cj",
        42
    )

    // 直接打印
    println(stackTraceElement)
    return 0
}
```

运行结果：

```text
MyClass.myMethod(MyClass.cj:42)
```

## class StringBuilder

```cangjie
public class StringBuilder <: ToString
```

功能：该类主要用于字符串的构建。

[StringBuilder](core_package_classes.md#class-stringbuilder) 在字符串的构建上效率高于 [String](core_package_structs.md#struct-string)：

- 在功能上支持传入多个类型的值，该类将自动将其转换为 [String](core_package_structs.md#struct-string) 类型对象，并追加到构造的字符串中。
- 在性能上使用动态扩容算法，减少内存申请频率，构造字符串的速度更快，占用内存资源通常更少。

> **注意：**
>
> [StringBuilder](core_package_classes.md#class-stringbuilder) 仅支持 UTF-8 编码的字符数据。

父类型：

- [ToString](core_package_interfaces.md#interface-tostring)

### prop capacity

```cangjie
public prop capacity: Int64
```

功能：获取 [StringBuilder](core_package_classes.md#class-stringbuilder) 实例此时能容纳字符串的长度，该值会随扩容的发生而变大。

类型：[Int64](core_package_intrinsics.md#int64)

示例：

<!-- verify -->
```cangjie
main(): Int64 {
    // 创建一个StringBuilder实例
    var sb = StringBuilder()

    // 获取初始容量
    let initialCapacity: Int64 = sb.capacity
    println("Initial capacity: ${initialCapacity}")

    // 添加一些内容
    sb.append("Hello, World!")

    // 获取添加内容后的容量
    let currentCapacity: Int64 = sb.capacity
    println("Current capacity: ${currentCapacity}")

    return 0
}
```

运行结果：

```text
Initial capacity: 32
Current capacity: 32
```

### prop size

```cangjie
public prop size: Int64
```

功能：获取 [StringBuilder](core_package_classes.md#class-stringbuilder) 实例中字符串长度。

类型：[Int64](core_package_intrinsics.md#int64)

示例：

<!-- verify -->
```cangjie
main(): Int64 {
    // 创建一个StringBuilder实例
    var sb = StringBuilder()

    // 获取初始大小
    let initialSize: Int64 = sb.size
    println("Initial size: ${initialSize}")

    // 添加一些内容
    sb.append("Hello")

    // 获取添加内容后的大小
    let currentSize: Int64 = sb.size
    println("Current size: ${currentSize}")

    return 0
}
```

运行结果：

```text
Initial size: 0
Current size: 5
```

### init()

```cangjie
public init()
```

功能：构造一个初始容量为 32 的空 [StringBuilder](core_package_classes.md#class-stringbuilder) 实例。

示例：

<!-- verify -->
```cangjie
main(): Int64 {
    // 使用默认构造函数创建一个StringBuilder实例
    var sb = StringBuilder()

    // 验证初始状态
    println("Initial capacity: ${sb.capacity}")
    println("Initial size: ${sb.size}")
    println("Initial content: '${sb}'")

    return 0
}
```

运行结果：

```text
Initial capacity: 32
Initial size: 0
Initial content: ''
```

### init(Array\<Rune>)

```cangjie
public init(value: Array<Rune>)
```

功能：使用参数 `value` 指定的字符数组初始化一个 [StringBuilder](core_package_classes.md#class-stringbuilder) 实例，该实例的初始容量为 `value` 大小，初始内容为 `value` 包含的字符内容。

参数：

- value: [Array](core_package_structs.md#struct-arrayt)\<Rune> - 初始化 [StringBuilder](core_package_classes.md#class-stringbuilder) 实例的字符数组。

示例：

<!-- verify -->
```cangjie
main(): Int64 {
    // 创建一个Rune数组
    let runes: Array<Rune> = [r'H', r'e', r'l', r'l', r'o']

    // 使用Rune数组创建StringBuilder实例
    var sb = StringBuilder(runes)

    // 验证初始化结果
    println("Capacity: ${sb.capacity}")
    println("Size: ${sb.size}")
    println("Content: '${sb}'")

    return 0
}
```

运行结果：

```text
Capacity: 37
Size: 5
Content: 'Hello'
```

### init(Int64)

```cangjie
public init(capacity: Int64)
```

功能：使用参数 `capacity` 指定的容量初始化一个空 [StringBuilder](core_package_classes.md#class-stringbuilder) 实例，该实例的初始容量为 `value` 大小，初始内容为若干 `\0` 字符。

参数：

- capacity: [Int64](core_package_intrinsics.md#int64) - 初始化 [StringBuilder](core_package_classes.md#class-stringbuilder) 的字节容量，取值范围为 (0, [Int64.Max](./core_package_intrinsics.md)]。

异常：

- [IllegalArgumentException](core_package_exceptions.md#class-illegalargumentexception) - 当参数 `capacity` 的值小于等于 0 时，抛出异常。

示例：

<!-- verify -->
```cangjie
main(): Int64 {
    // 使用指定容量创建一个StringBuilder实例
    var sb = StringBuilder(100)

    // 验证初始化结果
    println("Capacity: ${sb.capacity}")
    println("Size: ${sb.size}")
    println("Content: '${sb}'")

    // 添加内容
    sb.append("Hello, World!")
    println("After append - Size: ${sb.size}")
    println("After append - Content: '${sb}'")

    return 0
}
```

运行结果：

```text
Capacity: 100
Size: 0
Content: ''
After append - Size: 13
After append - Content: 'Hello, World!'
```

### init(Rune, Int64)

```cangjie
public init(r: Rune, n: Int64)
```

功能：使用 `n` 个 `r` 字符初始化 [StringBuilder](core_package_classes.md#class-stringbuilder) 实例，该实例的初始容量为 `n`，初始内容为 `n` 个 `r` 字符。

参数：

- r: Rune - 初始化 [StringBuilder](core_package_classes.md#class-stringbuilder) 实例的字符。
- n: [Int64](core_package_intrinsics.md#int64) - 字符 `r` 的数量，取值范围为 [0, [Int64.Max](./core_package_intrinsics.md)]。

异常：

- [IllegalArgumentException](core_package_exceptions.md#class-illegalargumentexception) - 当参数 `n` 小于 0 时，抛出异常。

示例：

<!-- verify -->
```cangjie
main(): Int64 {
    // 使用5个'*'字符创建一个StringBuilder实例
    var sb = StringBuilder(r'*', 5)

    // 验证初始化结果
    println("Capacity: ${sb.capacity}")
    println("Size: ${sb.size}")
    println("Content: '${sb}'")

    // 添加更多内容
    sb.append("Hello")
    println("After append - Size: ${sb.size}")
    println("After append - Content: '${sb}'")

    return 0
}
```

运行结果：

```text
Capacity: 37
Size: 5
Content: '*****'
After append - Size: 10
After append - Content: '*****Hello'
```

### init(String)

```cangjie
public init(str: String)
```

功能：根据指定初始字符串构造 [StringBuilder](core_package_classes.md#class-stringbuilder) 实例，该实例的初始容量为指定字符串的大小，初始内容为指定字符串。

参数：

- str: [String](core_package_structs.md#struct-string) - 初始化 [StringBuilder](core_package_classes.md#class-stringbuilder) 实例的字符串。

示例：

<!-- verify -->
```cangjie
main(): Int64 {
    // 使用初始字符串创建一个StringBuilder实例
    var sb = StringBuilder("Hello, World!")

    // 验证初始化结果
    println("Capacity: ${sb.capacity}")
    println("Size: ${sb.size}")
    println("Content: '${sb}'")

    // 添加更多内容
    sb.append(" Welcome!")
    println("After append - Size: ${sb.size}")
    println("After append - Content: '${sb}'")

    return 0
}
```

运行结果：

```text
Capacity: 45
Size: 13
Content: 'Hello, World!'
After append - Size: 22
After append - Content: 'Hello, World! Welcome!'
```

### func append(Array\<Rune>)

```cangjie
public func append(runeArr: Array<Rune>): Unit
```

功能：在 [StringBuilder](core_package_classes.md#class-stringbuilder) 末尾插入一个 `Rune` 数组中所有字符。

参数：

- runeArr: [Array](core_package_structs.md#struct-arrayt)\<Rune> - 插入的 `Rune` 数组。

示例：

<!-- verify -->
```cangjie
main(): Int64 {
    // 创建一个StringBuilder实例
    var sb = StringBuilder("Hello")

    // 创建一个Rune数组
    let runes: Array<Rune> = [r' ', r'W', r'o', r'r', r'l', r'd']

    // 将Rune数组追加到StringBuilder
    sb.append(runes)

    // 验证结果
    println("Content: '${sb}'")
    println("Size: ${sb.size}")

    return 0
}
```

运行结果：

```text
Content: 'Hello World'
Size: 11
```

### func append(Bool)

```cangjie
public func append(b: Bool): Unit
```

功能：在 [StringBuilder](core_package_classes.md#class-stringbuilder) 末尾插入参数 `b` 的字符串表示。

参数：

- b: [Bool](core_package_intrinsics.md#bool) - 插入的 [Bool](core_package_intrinsics.md#bool) 类型的值。

示例：

<!-- verify -->
```cangjie
main(): Int64 {
    // 创建一个StringBuilder实例
    var sb = StringBuilder("Boolean values: ")

    // 追加布尔值
    sb.append(true)
    sb.append(" and ")
    sb.append(false)

    // 验证结果
    println("Content: '${sb}'")
    println("Size: ${sb.size}")

    return 0
}
```

运行结果：

```text
Content: 'Boolean values: true and false'
Size: 30
```

### func append(CString)

```cangjie
public func append(cstr: CString): Unit
```

功能：在 [StringBuilder](core_package_classes.md#class-stringbuilder) 末尾插入参数 `cstr` 指定 [CString](core_package_intrinsics.md#cstring) 中的内容。

参数：

- cstr: [CString](core_package_intrinsics.md#cstring) - 插入的 [CString](core_package_intrinsics.md#cstring)。

示例：

<!-- verify -->
```cangjie
main(): Int64 {
    // 创建一个StringBuilder实例
    var sb = StringBuilder("Message: ")

    // 创建一个CString
    let cstr: CString = unsafe { LibC.mallocCString("Hello from C string") }

    // 追加CString
    sb.append(cstr)

    unsafe { LibC.free(cstr) }
    // 验证结果
    println("Content: '${sb}'")
    println("Size: ${sb.size}")

    return 0
}
```

运行结果：

```text
Content: 'Message: Hello from C string'
Size: 28
```

### func append(Float16)

```cangjie
public func append(n: Float16): Unit
```

功能：在 [StringBuilder](core_package_classes.md#class-stringbuilder) 末尾插入参数 `n` 的字符串表示。

参数：

- n: [Float16](core_package_intrinsics.md#float16) - 插入的 [Float16](core_package_intrinsics.md#float16) 类型的值。

示例：

<!-- verify -->
```cangjie
main(): Int64 {
    // 创建一个StringBuilder实例
    var sb = StringBuilder("Float16 value: ")

    // 追加Float16值
    sb.append(3.14f16)

    // 验证结果
    println("Content: '${sb}'")
    println("Size: ${sb.size}")

    return 0
}
```

运行结果：

```text
Content: 'Float16 value: 3.140625'
Size: 23
```

### func append(Float32)

```cangjie
public func append(n: Float32): Unit
```

功能：在 [StringBuilder](core_package_classes.md#class-stringbuilder) 末尾插入参数 `n` 的字符串表示。

参数：

- n: [Float32](core_package_intrinsics.md#float32) - 插入的 [Float32](core_package_intrinsics.md#float32) 类型的值。

示例：

<!-- verify -->
```cangjie
main(): Int64 {
    // 创建一个StringBuilder实例
    var sb = StringBuilder("Float32 value: ")

    // 追加Float32值
    sb.append(3.14159f32)

    // 验证结果
    println("Content: '${sb}'")
    println("Size: ${sb.size}")

    return 0
}
```

运行结果：

```text
Content: 'Float32 value: 3.141590'
Size: 23
```

### func append(Float64)

```cangjie
public func append(n: Float64): Unit
```

功能：在 [StringBuilder](core_package_classes.md#class-stringbuilder) 末尾插入参数 `n` 的字符串表示。

参数：

- n: [Float64](core_package_intrinsics.md#float64) - 插入的 [Float64](core_package_intrinsics.md#float64) 类型的值。

示例：

<!-- verify -->
```cangjie
main(): Int64 {
    // 创建一个StringBuilder实例
    var sb = StringBuilder("Float64 value: ")

    // 追加Float64值
    sb.append(3.141592653589793)

    // 验证结果
    println("Content: '${sb}'")
    println("Size: ${sb.size}")

    return 0
}
```

运行结果：

```text
Content: 'Float64 value: 3.141593'
Size: 23
```

### func append(Int16)

```cangjie
public func append(n: Int16): Unit
```

功能：在 [StringBuilder](core_package_classes.md#class-stringbuilder) 末尾插入参数 `n` 的字符串表示。

参数：

- n: [Int16](core_package_intrinsics.md#int16) - 插入的 [Int16](core_package_intrinsics.md#int16) 类型的值。

示例：

<!-- verify -->
```cangjie
main(): Int64 {
    // 创建一个StringBuilder实例
    var sb = StringBuilder("Int16 value: ")

    // 追加Int16值
    sb.append(12345i16)

    // 验证结果
    println("Content: '${sb}'")
    println("Size: ${sb.size}")

    return 0
}
```

运行结果：

```text
Content: 'Int16 value: 12345'
Size: 18
```

### func append(Int32)

```cangjie
public func append(n: Int32): Unit
```

功能：在 [StringBuilder](core_package_classes.md#class-stringbuilder) 末尾插入参数 `n` 的字符串表示。

参数：

- n: [Int32](core_package_intrinsics.md#int32) - 插入的 [Int32](core_package_intrinsics.md#int32) 类型的值。

示例：

<!-- verify -->
```cangjie
main(): Int64 {
    // 创建一个StringBuilder实例
    var sb = StringBuilder("Int32 value: ")

    // 追加Int32值
    sb.append(1234567890i32)

    // 验证结果
    println("Content: '${sb}'")
    println("Size: ${sb.size}")

    return 0
}
```

运行结果：

```text
Content: 'Int32 value: 1234567890'
Size: 23
```

### func append(Int64)

```cangjie
public func append(n: Int64): Unit
```

功能：在 [StringBuilder](core_package_classes.md#class-stringbuilder) 末尾插入参数 `n` 的字符串表示。

参数：

- n: [Int64](core_package_intrinsics.md#int64) - 插入的 [Int64](core_package_intrinsics.md#int64) 类型的值。

示例：

<!-- verify -->
```cangjie
main(): Int64 {
    // 创建一个StringBuilder实例
    var sb = StringBuilder("Int64 value: ")

    // 追加Int64值
    sb.append(123456789012345)

    // 验证结果
    println("Content: '${sb}'")
    println("Size: ${sb.size}")

    return 0
}
```

运行结果：

```text
Content: 'Int64 value: 123456789012345'
Size: 28
```

### func append(Int8)

```cangjie
public func append(n: Int8): Unit
```

功能：在 [StringBuilder](core_package_classes.md#class-stringbuilder) 末尾插入参数 `n` 的字符串表示。

参数：

- n: [Int8](core_package_intrinsics.md#int8) - 插入的 [Int8](core_package_intrinsics.md#int8) 类型的值。

示例：

<!-- verify -->
```cangjie
main(): Int64 {
    // 创建一个StringBuilder实例
    var sb = StringBuilder("Int8 value: ")

    // 追加Int8值
    sb.append(123i8)

    // 验证结果
    println("Content: '${sb}'")
    println("Size: ${sb.size}")

    return 0
}
```

运行结果：

```text
Content: 'Int8 value: 123'
Size: 15
```

### func append(Rune)

```cangjie
public func append(r: Rune): Unit
```

功能：在 [StringBuilder](core_package_classes.md#class-stringbuilder) 末尾插入参数 `r` 指定的字符。

参数：

- r: Rune - 插入的字符。

示例：

<!-- verify -->
```cangjie
main(): Int64 {
    // 创建一个StringBuilder实例
    var sb = StringBuilder("Hello")

    // 追加一个字符
    sb.append(r'!')

    // 验证结果
    println("Content: '${sb}'")
    println("Size: ${sb.size}")

    return 0
}
```

运行结果：

```text
Content: 'Hello!'
Size: 6
```

### func append(String)

```cangjie
public func append(str: String): Unit
```

功能：在 [StringBuilder](core_package_classes.md#class-stringbuilder) 末尾插入参数 `str` 指定的字符串。

参数：

- str: [String](core_package_structs.md#struct-string) - 插入的字符串。

示例：

<!-- verify -->
```cangjie
main(): Int64 {
    // 创建一个StringBuilder实例
    var sb = StringBuilder("Hello")

    // 追加一个字符串
    sb.append(", World!")

    // 验证结果
    println("Content: '${sb}'")
    println("Size: ${sb.size}")

    return 0
}
```

运行结果：

```text
Content: 'Hello, World!'
Size: 13
```

### func append(StringBuilder)

```cangjie
public func append(sb: StringBuilder): Unit
```

功能：在 [StringBuilder](core_package_classes.md#class-stringbuilder) 末尾插入参数 `sb` 指定的 [StringBuilder](core_package_classes.md#class-stringbuilder) 中的内容。

参数：

- sb: [StringBuilder](core_package_classes.md#class-stringbuilder) - 插入的 [StringBuilder](core_package_classes.md#class-stringbuilder) 实例。

示例：

<!-- verify -->
```cangjie
main(): Int64 {
    // 创建两个StringBuilder实例
    var sb1 = StringBuilder("Hello")
    var sb2 = StringBuilder(", World!")

    // 将sb2的内容追加到sb1
    sb1.append(sb2)

    // 验证结果
    println("Content: '${sb1}'")
    println("Size: ${sb1.size}")

    return 0
}
```

运行结果：

```text
Content: 'Hello, World!'
Size: 13
```

### func append(UInt16)

```cangjie
public func append(n: UInt16): Unit
```

功能：在 [StringBuilder](core_package_classes.md#class-stringbuilder) 末尾插入参数 `n` 的字符串表示。

参数：

- n: [UInt16](core_package_intrinsics.md#uint16) - 插入的 [UInt16](core_package_intrinsics.md#uint16) 类型的值。

示例：

<!-- verify -->
```cangjie
main(): Int64 {
    // 创建一个StringBuilder实例
    var sb = StringBuilder("UInt16 value: ")

    // 追加UInt16值
    sb.append(12345u16)

    // 验证结果
    println("Content: '${sb}'")
    println("Size: ${sb.size}")

    return 0
}
```

运行结果：

```text
Content: 'UInt16 value: 12345'
Size: 19
```

### func append(UInt32)

```cangjie
public func append(n: UInt32): Unit
```

功能：在 [StringBuilder](core_package_classes.md#class-stringbuilder) 末尾插入参数 `n` 的字符串表示。

参数：

- n: [UInt32](core_package_intrinsics.md#uint32) - 插入的 [UInt32](core_package_intrinsics.md#uint32) 类型的值。

示例：

<!-- verify -->
```cangjie
main(): Int64 {
    // 创建一个StringBuilder实例
    var sb = StringBuilder("UInt32 value: ")

    // 追加UInt32值
    sb.append(1234567890u32)

    // 验证结果
    println("Content: '${sb}'")
    println("Size: ${sb.size}")

    return 0
}
```

运行结果：

```text
Content: 'UInt32 value: 1234567890'
Size: 24
```

### func append(UInt64)

```cangjie
public func append(n: UInt64): Unit
```

功能：在 [StringBuilder](core_package_classes.md#class-stringbuilder) 末尾插入参数 `n` 的字符串表示。

参数：

- n: [UInt64](core_package_intrinsics.md#uint64) - 插入的 [UInt64](core_package_intrinsics.md#uint64) 类型的值。

示例：

<!-- verify -->
```cangjie
main(): Int64 {
    // 创建一个StringBuilder实例
    var sb = StringBuilder("UInt64 value: ")

    // 追加UInt64值
    sb.append(123456789012345u64)

    // 验证结果
    println("Content: '${sb}'")
    println("Size: ${sb.size}")

    return 0
}
```

运行结果：

```text
Content: 'UInt64 value: 123456789012345'
Size: 29
```

### func append(UInt8)

```cangjie
public func append(n: UInt8): Unit
```

功能：在 [StringBuilder](core_package_classes.md#class-stringbuilder) 末尾插入参数 `n` 的字符串表示。

> **注意：**
>
> - 插入的是该数值的**十进制文本表示**，而非字节本身。例如 `append(0x41u8)` 追加的是 `"65"`，而不是字符 `'A'`。
> - `Byte` 是 `UInt8` 的类型别名，对 `String` 逐字节取值（如 `s[i]`）后调用本函数同样追加十进制文本。如需追加字节本身（UTF-8 字节级拼接），请使用 [appendFromUtf8](#func-appendfromutf8arraybyte) / [appendFromUtf8Unchecked](#func-appendfromutf8uncheckedarraybyte)；如需按字符追加，请使用 `append(Rune)`。

参数：

- n: [UInt8](core_package_intrinsics.md#uint8) - 插入的 [UInt8](core_package_intrinsics.md#uint8) 类型的值。

示例：

<!-- verify -->
```cangjie
main(): Int64 {
    // 创建一个StringBuilder实例
    var sb = StringBuilder("UInt8 value: ")

    // 追加UInt8值（追加的是其十进制文本表示"123"）
    sb.append(123u8)

    // 验证结果
    println("Content: '${sb}'")
    println("Size: ${sb.size}")

    return 0
}
```

运行结果：

```text
Content: 'UInt8 value: 123'
Size: 16
```

### func append\<T>(Array\<T>) where T <: ToString

```cangjie
public func append<T>(val: Array<T>): Unit where T <: ToString
```

功能：在 [StringBuilder](core_package_classes.md#class-stringbuilder) 末尾插入参数 `val` 指定的 [Array](core_package_structs.md#struct-arrayt)\<T> 的字符串表示，类型 `T` 需要实现 [ToString](core_package_interfaces.md#interface-tostring) 接口。

参数：

- val: [Array](core_package_structs.md#struct-arrayt)\<T> - 插入的 [Array](core_package_structs.md#struct-arrayt)\<T> 类型实例。

示例：

<!-- verify -->
```cangjie
main(): Int64 {
    // 创建一个StringBuilder实例
    var sb = StringBuilder("Numbers: ")

    // 创建一个整数数组
    let numbers: Array<Int64> = [1, 2, 3, 4, 5]

    // 将整数数组追加到StringBuilder
    sb.append(numbers)

    // 验证结果
    println("Content: '${sb}'")
    println("Size: ${sb.size}")

    return 0
}
```

运行结果：

```text
Content: 'Numbers: 12345'
Size: 14
```

### func append\<T>(T) where T <: ToString

```cangjie
public func append<T>(v: T): Unit where T <: ToString
```

功能：在 [StringBuilder](core_package_classes.md#class-stringbuilder) 末尾插入参数 `v` 指定 `T` 类型的字符串表示，类型 `T` 需要实现 [ToString](core_package_interfaces.md#interface-tostring) 接口。

参数：

- v: T - 插入的 `T` 类型实例。

示例：

<!-- verify -->
```cangjie
main(): Int64 {
    // 创建一个StringBuilder实例
    var sb = StringBuilder("The answer is: ")

    // 追加一个整数（实现了ToString接口）
    sb.append(42)

    // 验证结果
    println("Content: '${sb}'")
    println("Size: ${sb.size}")

    return 0
}
```

运行结果：

```text
Content: 'The answer is: 42'
Size: 17
```

### func appendFromUtf8(Array\<Byte>)

```cangjie
public func appendFromUtf8(arr: Array<Byte>): Unit
```

功能：在 [StringBuilder](core_package_classes.md#class-stringbuilder) 末尾插入指定字节数组。

该函数要求入参符合 UTF-8 编码，如果不符合，将抛出异常。

参数：

- arr: [Array](core_package_structs.md#struct-arrayt)\<[Byte](core_package_types.md#type-byte)> - 插入的字节数组。

异常：

- [IllegalArgumentException](core_package_exceptions.md#class-illegalargumentexception) - 当字节数组不符合 UTF-8 编码规则时，抛出异常。

示例：

<!-- verify -->
```cangjie
main(): Int64 {
    // 创建一个StringBuilder实例
    var sb = StringBuilder("Hello, ")

    // 创建一个UTF-8编码的字节数组（"World"的UTF-8编码）
    let utf8Bytes: Array<Byte> = [87, 111, 114, 108, 100] // "World"的ASCII码

    // 追加UTF-8字节数组
    sb.appendFromUtf8(utf8Bytes)

    // 验证结果
    println("Content: '${sb}'")
    println("Size: ${sb.size}")

    return 0
}
```

运行结果：

```text
Content: 'Hello, World'
Size: 12
```

### func appendFromUtf8Unchecked(Array\<Byte>)

```cangjie
public unsafe func appendFromUtf8Unchecked(arr: Array<Byte>): Unit
```

功能：在 [StringBuilder](core_package_classes.md#class-stringbuilder) 末尾插入参数 `arr` 指向的字节数组。

相较于 `appendFromUtf8` 函数，它并没有针对于字节数组进行 UTF-8 相关规则的检查，所以它所构建的字符串并不一定保证是合法的，甚至出现非预期的异常，如果不是某些场景下的速度考虑，请优先使用安全的 `appendFromUtf8` 函数。

参数：

- arr: [Array](core_package_structs.md#struct-arrayt)\<[Byte](core_package_types.md#type-byte)> - 插入的字节数组。

示例：

<!-- verify -->
```cangjie
main(): Int64 {
    // 创建一个StringBuilder实例
    var sb = StringBuilder("Hello, ")

    // 创建一个字节数组（"World"的ASCII码）
    let bytes: Array<Byte> = [87, 111, 114, 108, 100] // "World"的ASCII码

    // 追加字节数组（不检查UTF-8编码）
    unsafe { sb.appendFromUtf8Unchecked(bytes) }

    // 验证结果
    println("Content: '${sb}'")
    println("Size: ${sb.size}")

    return 0
}
```

运行结果：

```text
Content: 'Hello, World'
Size: 12
```

### func reserve(Int64)

```cangjie
public func reserve(additional: Int64): Unit
```

功能：以指定大小进行扩容。

> **说明：**
>
> - 若入参 additional ≤ 0，不执行任何扩容操作。
> - 若当前剩余容量 ≥ additional，不进行扩容，直接返回。
> - 若当前剩余容量 < additional，则按以下两者计算最大者执行扩容：
>     - 1.原始容量的 1.5 倍（结果向下取整）
>     - 2.已使用容量 + additional。

参数：

- additional: [Int64](core_package_intrinsics.md#int64) - 指定 [StringBuilder](core_package_classes.md#class-stringbuilder) 的扩容大小。

示例：

<!-- verify -->
```cangjie
main(): Int64 {
    // 创建一个StringBuilder实例
    var sb = StringBuilder("Hello")

    // 查看初始容量和大小
    println("Initial capacity: ${sb.capacity}")
    println("Initial size: ${sb.size}")

    // 预留额外空间
    sb.reserve(100)

    // 查看扩容后的容量
    println("Capacity after reserve: ${sb.capacity}")
    println("Size after reserve: ${sb.size}")

    return 0
}
```

运行结果：

```text
Initial capacity: 37
Initial size: 5
Capacity after reserve: 105
Size after reserve: 5
```

### func reset(Option\<Int64>)

```cangjie
public func reset(capacity!: Option<Int64> = None): Unit
```

功能：清空当前 [StringBuilder](core_package_classes.md#class-stringbuilder)，并将容量重置为 `capacity` 指定的值。

参数：

- capacity!: [Option](core_package_enums.md#enum-optiont)\<[Int64](core_package_intrinsics.md#int64)> - 重置后 [StringBuilder](core_package_classes.md#class-stringbuilder) 实例的容量大小，取值范围为 `None` 和 (`Some(0)`, `Some(Int64.Max)`]，默认值 `None` 表示采用默认大小容量（32）。

异常：

- [IllegalArgumentException](core_package_exceptions.md#class-illegalargumentexception) - 当参数 `capacity` 的值小于等于 0 时，抛出异常。

示例：

<!-- verify -->
```cangjie
main(): Int64 {
    // 创建一个StringBuilder实例并添加一些内容
    var sb = StringBuilder("Hello, World!")
    println("Before reset - Content: '${sb}', Size: ${sb.size}, Capacity: ${sb.capacity}")

    // 重置StringBuilder，使用默认容量
    sb.reset()
    println("After reset - Content: '${sb}', Size: ${sb.size}, Capacity: ${sb.capacity}")

    // 添加新内容
    sb.append("New content")
    println("After append - Content: '${sb}', Size: ${sb.size}")

    // 使用指定容量重置
    sb.reset(capacity: Some(50))
    println("After reset with capacity - Content: '${sb}', Size: ${sb.size}, Capacity: ${sb.capacity}")

    return 0
}
```

运行结果：

```text
Before reset - Content: 'Hello, World!', Size: 13, Capacity: 45
After reset - Content: '', Size: 0, Capacity: 32
After append - Content: 'New content', Size: 11
After reset with capacity - Content: '', Size: 0, Capacity: 50
```

### func toString()

```cangjie
public func toString(): String
```

功能：获取 [StringBuilder](core_package_classes.md#class-stringbuilder) 实例中的字符串。

> **注意：**
>
> 该函数不会将字符串数据进行拷贝。

返回值：

- [String](core_package_structs.md#struct-string) - [StringBuilder](core_package_classes.md#class-stringbuilder) 实例中的字符串。

示例：

<!-- verify -->
```cangjie
main(): Int64 {
    // 创建一个StringBuilder实例并添加一些内容
    var sb = StringBuilder("Hello")
    sb.append(", World!")

    // 获取字符串表示
    let result: String = sb.toString()
    println("Content: '${result}'")
    println("Size: ${sb.size}")

    // 验证返回的是String类型
    println("Type check: ${result is String}")

    return 0
}
```

运行结果：

```text
Content: 'Hello, World!'
Size: 13
Type check: true
```

## class Thread

```cangjie
public class Thread
```

功能：该类代表一个仓颉线程，可用于获取线程 ID 及名字、查询线程是否存在取消请求、注册线程未处理异常的处理函数等。

该类型实例无法通过构造得到，仅能通过 [Future](core_package_classes.md#class-futuret) 对象的 `thread` 属性或是 [Thread](core_package_classes.md#class-thread) 类的 `currentThread` 静态属性获取。

### static prop currentThread

```cangjie
public static prop currentThread: Thread
```

功能：获取当前执行线程的 [Thread](core_package_classes.md#class-thread) 对象。

类型：[Thread](core_package_classes.md#class-thread)

示例：

<!-- verify -->
```cangjie
main(): Int64 {
    // 获取当前线程
    let currentThread = Thread.currentThread

    // 打印线程信息
    println("Current thread ID: ${currentThread.id}")
    println("Current thread name: '${currentThread.name}'")

    return 0
}
```

运行结果：

```text
Current thread ID: 1
Current thread name: ''
```

### prop hasPendingCancellation

```cangjie
public prop hasPendingCancellation: Bool
```

功能：判断当前线程是否接收过取消请求，即其他线程是否通过 future.cancel() 发送过取消请求，常见使用方为 [Thread](core_package_classes.md#class-thread).currentThread.hasPendingCancellation。

类型：[Bool](core_package_intrinsics.md#bool)

示例：

<!-- verify -->
```cangjie
main(): Int64 {
    // 获取当前线程
    let currentThread = Thread.currentThread

    // 检查是否有取消请求
    let hasPendingCancellation: Bool = currentThread.hasPendingCancellation

    println("Has pending cancellation: ${hasPendingCancellation}")

    let future = spawn {
        while (true) {
            if (Thread.currentThread.hasPendingCancellation) {
                println("Has pending cancellation: ${Thread.currentThread.hasPendingCancellation}")
                return 0
            }
        }
        return 1
    }
    /* 向线程发送取消请求 */
    future.cancel()
    /* 等待线程结果 */
    future.get()
    return 0
}
```

运行结果：

```text
Has pending cancellation: false
Has pending cancellation: true
```

### prop id

```cangjie
public prop id: Int64
```

功能：获取当前执行线程的标识，以 [Int64](core_package_intrinsics.md#int64) 表示，所有存活的线程都有不同标识，但不保证当线程执行结束后复用它的标识。

类型：[Int64](core_package_intrinsics.md#int64)

示例：

<!-- verify -->
```cangjie
main(): Int64 {
    // 获取当前线程
    let currentThread = Thread.currentThread

    // 获取线程ID
    let threadId: Int64 = currentThread.id

    println("Current thread ID: ${threadId}")

    return 0
}
```

运行结果：

```text
Current thread ID: 1
```

### prop name

```cangjie
public mut prop name: String
```

功能：获取或设置线程的名称，获取设置都具有原子性。

类型：[String](core_package_structs.md#struct-string)

示例：

<!-- verify -->
```cangjie
main(): Int64 {
    // 获取当前线程
    let currentThread = Thread.currentThread

    // 获取当前线程名称
    let originalName: String = currentThread.name
    println("Original thread name: '${originalName}'")

    // 设置线程名称
    currentThread.name = "MyCustomThread"

    // 获取更新后的线程名称
    let newName: String = currentThread.name
    println("New thread name: '${newName}'")

    return 0
}
```

运行结果：

```text
Original thread name: ''
New thread name: 'MyCustomThread'
```

### prop state

```cangjie
public prop state: ThreadState
```

功能：获取线程的状态。

类型：[ThreadState](core_package_enums.md#enum-threadstate)

示例：

<!-- verify -->
```cangjie
main(): Int64 {
    // 获取当前线程
    let currentThread = Thread.currentThread

    // 获取当前线程状态
    let state = currentThread.state
    println("Current thread state: '${state}'")
    return 0
}
```

运行结果：

```text
Current thread state: 'Running'
```

<!--Del-->
### static func handleUncaughtErrorBy((Error) -> Unit)

```cangjie
public static func handleUncaughtErrorBy(erHandler: (Error) -> Unit): Unit
```

功能：注册线程未处理错误的处理函数。

当某一线程因错误而提前终止后：

- 如果全局的未处理错误函数被注册，那么将调用该函数并结束线程，在该函数内抛出异常或错误时，将向终端打印简单的提示信息，同时结束线程（如果处理函数内抛出的是异常）或结束进程（如果处理函数内抛出的是错误）。
- 如果没有注册全局错误处理函数，那么默认会向终端打印错误信息。

多次注册处理函数时，后续的注册函数将覆盖之前的处理函数。

当有多个线程同时因异常而终止时，处理函数将被并发执行，因而开发者需要在处理函数中确保并发正确性。

处理函数的参数类型为 [Error](core_package_exceptions.md#class-error)，是线程未处理的错误。

> **注意：**
>
> 不支持平台：OpenHarmony。

参数：

- erHandler: ([Error](core_package_exceptions.md#class-error)) -> [Unit](core_package_intrinsics.md#unit) - 注册的处理函数。

示例：

<!-- run -->
```cangjie
// 定义异常处理函数
func handleError(err: Error): Unit {
    println("Unhandled error in thread ${Thread.currentThread.name}: ${err}")
}

// 模拟大内存使用，抛出OutOfMemoryError
func consumeMemory() {
    let array = Array(1024 * 1024 * 1024 * 1024, repeat: 1024)
    for (i in 0..array.size) {
        array[i] = 0
    }
}

main(): Int64 {
    // 注册未处理错误处理函数
    Thread.handleUncaughtErrorBy(handleError)

    // 创建一个会抛出错误的线程
    let future = spawn {
        consumeMemory()
    }

    try {
        // 等待线程结果
        future.get()
    } catch (e: Exception) {
        println("Caught exception: ${e.message}")
    }
    return 0
}
```

运行结果：

```text
An exception has occurred:
    Out of memory
Unhandled error in thread : OutOfMemoryError
An exception has occurred:
    Out of memory
```

<!--DelEnd-->
### static func handleUncaughtExceptionBy((Thread, Exception) -> Unit)

```cangjie
public static func handleUncaughtExceptionBy(exHandler: (Thread, Exception) -> Unit): Unit
```

功能：注册线程未处理异常的处理函数。

当某一线程因异常而提前终止后：

- 如果全局的未处理异常函数被注册，那么将调用该函数并结束线程，在该函数内抛出异常时，将向终端打印提示信息并结束线程，但不会打印异常调用栈信息。
- 如果没有注册全局异常处理函数，那么默认会向终端打印异常调用栈信息。

多次注册处理函数时，后续的注册函数将覆盖之前的处理函数。

当有多个线程同时因异常而终止时，处理函数将被并发执行，因而开发者需要在处理函数中确保并发正确性。

处理函数的参数第一个参数类型为 [Thread](core_package_classes.md#class-thread)，是发生异常的线程，第二个参数类型为 [Exception](core_package_exceptions.md#class-exception)，是线程未处理的异常。

参数：

- exHandler: ([Thread](core_package_classes.md#class-thread), [Exception](core_package_exceptions.md#class-exception)) -> [Unit](core_package_intrinsics.md#unit) - 注册的处理函数。

示例：

<!-- verify -->
```cangjie
// 定义异常处理函数
func handleException(thread: Thread, exception: Exception): Unit {
    println("Unhandled exception in thread ${thread.name}: ${exception.message}")
}

main(): Int64 {
    // 注册未处理异常处理函数
    Thread.handleUncaughtExceptionBy(handleException)

    // 创建一个会抛出异常的线程
    let future = spawn {
        throw Exception("This is a thread exception.")
    }

    try {
        // 等待线程结果
        future.get()
    } catch (e: Exception) {
        println("Caught exception: ${e.message}")
    }
    return 0
}
```

运行结果：

```text
Unhandled exception in thread : This is a thread exception.
Caught exception: This is a thread exception.
```

## class ThreadLocal\<T>

```cangjie
public class ThreadLocal<T>
```

功能：该类表示仓颉线程局部变量。

和普通变量相比，线程局部变量有不同的访问语义。当多个线程共享使用同一线程局部变量时，每个线程都有各自的一份值拷贝。线程对变量的访问会读写线程本地的值，而不会影响其他线程中变量的值。

### func get()

```cangjie
public func get(): ?T
```

功能：获得仓颉线程局部变量的值。

返回值：

- ?T - 如果当前线程局部变量不为空值，返回该值，如果为空值，返回 `None`。

示例：

<!-- run -->
```cangjie
let local = ThreadLocal<Int64>()

func printUserID(): Unit {
    println("获知用户ID: ${local.get() ?? 0}")
}

func setUserID(id: Int64): Unit {
    local.set(Some(id))
}

main() {
    let future = spawn {
        // 模拟获取用户id
        var id = 1001
        setUserID(id)

        // 通过 ThreadLocal 可以不用传参就得到用户ID
        printUserID()
    }
    let future1 = spawn {
        // 模拟获取用户id
        var id = 1002
        setUserID(id)

        // 通过 ThreadLocal 可以不用传参就得到用户ID
        printUserID()
    }
    let future2 = spawn {
        // 模拟获取用户id
        var id = 1003
        setUserID(id)

        // 通过 ThreadLocal 可以不用传参就得到用户ID
        printUserID()
    }
    future.get()
    future1.get()
    future2.get()
    return 0
}
```

可能的运行结果：

```text
获知用户ID: 1003
获知用户ID: 1001
获知用户ID: 1002
```

### func set(?T)

```cangjie
public func set(value: ?T): Unit
```

功能：通过 value 设置仓颉线程局部变量的值，如果传入 `None`，该局部变量的值将被删除，在线程后续操作中将无法获取。

参数：

- value: ?T - 需要设置的局部变量的值。

示例：

<!-- run -->
```cangjie
let local = ThreadLocal<Int64>()

func printUserID(): Unit {
    println("获知用户ID: ${local.get() ?? 0}")
}

func setUserID(id: Int64): Unit {
    local.set(Some(id))
}

main() {
    let future = spawn {
        // 模拟获取用户id
        var id = 1001
        setUserID(id)

        // 通过 ThreadLocal 可以不用传参就得到用户ID
        printUserID()
    }
    let future1 = spawn {
        // 模拟获取用户id
        var id = 1002
        setUserID(id)

        // 通过 ThreadLocal 可以不用传参就得到用户ID
        printUserID()
    }
    let future2 = spawn {
        // 模拟获取用户id
        var id = 1003
        setUserID(id)

        // 通过 ThreadLocal 可以不用传参就得到用户ID
        printUserID()
    }
    future.get()
    future1.get()
    future2.get()
    return 0
}
```

可能的运行结果：

```text
获知用户ID: 1003
获知用户ID: 1001
获知用户ID: 1002
```

## class ThreadSnapshot

```cangjie
public class ThreadSnapshot <: ToString
```

功能：获取当前线程或者所有线程的信息，包含名称、id、状态、调用栈。

该类型实例无法通过构造得到，仅能通过 [class ThreadSnapshot](core_package_classes.md#class-threadsnapshot)类的 [dumpCurrentThread](core_package_classes.md#func-dumpcurrentthread)和[dumpAllThreads](core_package_classes.md#func-dumpallthreads) 静态函数获取。

父类型：

- [ToString](./core_package_interfaces.md#interface-tostring)

### let id

```cangjie
public let id: Int64
```

功能：获取线程的 id。

类型：[Int64](core_package_intrinsics.md#int64)

示例：

<!-- verify -->
```cangjie
main() {
    // 获取当前线程的快照
    let threadSnapshot = ThreadSnapshot.dumpCurrentThread()

    // 访问id字段
    let threadId = threadSnapshot.id
    println("线程ID: ${threadId}")

    return 0
}
```

运行结果：

```text
线程ID: 1
```

### let name

```cangjie
public let name: String
```

功能：获取线程的名称。

类型：[String](core_package_structs.md#struct-string)

示例：

<!-- verify -->
```cangjie
main(): Int64 {
    // 获取当前线程并设置名称
    let currentThread = Thread.currentThread
    currentThread.name = "MainThread"

    // 获取当前线程的快照
    let threadSnapshot = ThreadSnapshot.dumpCurrentThread()

    // 访问name字段
    let threadName = threadSnapshot.name
    println("线程名称: ${threadName}")

    return 0
}
```

运行结果：

```text
线程名称: MainThread
```

### let stackTrace

```cangjie
public let stackTrace: Array<StackTraceElement>
```

功能：获取线程的调用栈信息。

类型：[Array](core_package_structs.md#struct-arrayt)\<[StackTraceElement](core_package_classes.md#class-stacktraceelement)>

示例：

<!-- verify -->
```cangjie
main() {
    // 获取当前线程的快照
    let threadSnapshot = ThreadSnapshot.dumpCurrentThread()

    // 访问stackTrace字段
    let trace = threadSnapshot.stackTrace
    println("调用栈大小: ${trace.size}")

    return 0
}
```

运行结果：

```text
调用栈大小: 2
```

### let state

```cangjie
public let state: ThreadState
```

功能：获取线程的状态。

类型：[ThreadState](core_package_enums.md#enum-threadstate)

示例：

<!-- verify -->
```cangjie
main() {
    // 获取当前线程的快照
    let threadSnapshot = ThreadSnapshot.dumpCurrentThread()

    // 访问state字段
    let threadState = threadSnapshot.state
    println("线程状态: ${threadState}")

    return 0
}
```

运行结果：

```text
线程状态: Running
```

### func dumpAllThreads()

```cangjie
public static func dumpAllThreads(): Array<ThreadSnapshot>
```

功能：获取当前进程中所有线程的信息。

返回值：

- [Array](core_package_structs.md#struct-arrayt)\<[ThreadSnapshot](core_package_classes.md#class-threadsnapshot)> - 返回一个包含当前进程所有线程信息的[ThreadSnapshot](core_package_classes.md#class-threadsnapshot)数组。

示例：

<!-- run -->
```cangjie
main(): Unit {
    /* 创建一个线程 */
    let future = spawn {
        while (true) {
            sleep(1 * Duration.second)
            if (Thread.currentThread.hasPendingCancellation) {
                return
            }
        }
    }
    /* 获取所有线程的信息 */
    let threadInfoArray: Array<ThreadSnapshot> = ThreadSnapshot.dumpAllThreads()
    /* 循环打印线程信息 */
    let size = threadInfoArray.size
    for (i in 0..size) {
        let threadInfoData = threadInfoArray[i]
        println(threadInfoData)
    }
}
```

可能的运行结果：

```text
ThreadSnapshot(id=1, name=, state=Running)
stack trace:
         at std.core.ThreadSnapshot::dumpCurrentThread()(thread.cj:176)
         at default.test4()(hello.cj:46)
         at default.main()(hello.cj:146)
ThreadSnapshot(id=2, name=, state=Pending)
stack trace:
         at std.core.sleep(std.core::Duration)(sleep.cj:36)
         at default.test6::lambda.0()(hello.cj:66)
         at std.core.Future<...>::execute()(future.cj:161)
```

### func dumpCurrentThread()

```cangjie
public static func dumpCurrentThread(): ThreadSnapshot
```

功能：获取当前线程的信息。

返回值：

- [ThreadSnapshot](core_package_classes.md#class-threadsnapshot) - 返回一个包含当前线程信息的[ThreadSnapshot](core_package_classes.md#class-threadsnapshot)对象。

示例：

<!-- run -->
```cangjie
main(): Unit {
    /* 获取当前线程信息 */
    let threadInfo: ThreadSnapshot = ThreadSnapshot.dumpCurrentThread()
    /* 打印信息 */
    println(threadInfo)
}
```

可能的运行结果：

```text
ThreadSnapshot(id=1, name=, state=Running)
stack trace:
         at std.core.ThreadSnapshot::dumpCurrentThread()(thread.cj:181)
         at default.main()(test.cj:3)
```

### func toString()

```cangjie
public func toString(): String
```

功能：获取 [ThreadSnapshot](core_package_classes.md#class-threadsnapshot) 对象的字符串表示。

返回值：

- [String](core_package_structs.md#struct-string) - 转换后的字符串。

示例：

<!-- run -->
```cangjie
main() {
    // 获取当前线程的快照
    let threadSnapshot = ThreadSnapshot.dumpCurrentThread()

    // 调用toString方法
    let str = threadSnapshot.toString()
    println("线程快照字符串表示: ${str}")

    return 0
}
```

可能的运行结果：

```text
线程快照字符串表示: ThreadSnapshot(id=1, name=, state=Running)
stack trace:
         at std.core.ThreadSnapshot::dumpCurrentThread()(thread.cj:181)
         at default.main()(test_threadsnapshot_tostring.cj:3)
```
