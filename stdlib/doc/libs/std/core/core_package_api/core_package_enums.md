# 枚举

## enum AnnotationKind

```cangjie
public enum AnnotationKind
```

功能：表示自定义注解希望支持的位置。

### EnumConstructor

```cangjie
EnumConstructor
```

功能：枚举构造器声明。

### Extension

```cangjie
Extension
```

功能：扩展声明。

### GlobalFunction

```cangjie
GlobalFunction
```

功能：全局函数声明。

### GlobalVariable

```cangjie
GlobalVariable
```

功能：全局变量声明。

### Init

```cangjie
Init
```

功能：构造函数声明。

### MemberFunction

```cangjie
MemberFunction
```

功能：成员函数声明。

### MemberProperty

```cangjie
MemberProperty
```

功能：成员属性声明。

### MemberVariable

```cangjie
MemberVariable
```

功能：成员变量声明。

### Parameter

```cangjie
Parameter
```

功能：成员函数/构造函数中的参数（不包括枚举构造器的参数）。

### Type

```cangjie
Type
```

功能：类型声明（class、struct、enum、interface）。

## enum Endian

```cangjie
public enum Endian
```

功能：枚举类型 [Endian](core_package_enums.md#enum-endian) 表示运行平台的端序，分为大端序和小端序。

### Big

```cangjie
Big
```

功能：表示大端序。

### Little

```cangjie
Little
```

功能：表示小端序。

### static prop Platform

```cangjie
public static prop Platform: Endian
```

功能：获取所在运行平台的端序。

类型：[Endian](core_package_enums.md#enum-endian)

异常：

- [UnsupportedException](core_package_exceptions.md#class-unsupportedexception) - 当所运行平台返回的端序无法识别时，抛出异常。

示例：

<!-- verify -->
```cangjie
main() {
    let e = Endian.Platform
    match (e) {
        case Big => println("BigEndian")
        case Little => println("LittleEndian")
    }
}
```

运行结果：

```text
LittleEndian
```

## enum Option\<T>

```cangjie
public enum Option<T>
```

功能：[Option](core_package_enums.md#enum-optiont)\<T> 是对 `T` 类型的封装，表示可能有值也可能无值。

它包含两个构造器：[Some](#somet) 和 [None](#none)。其中，[Some](#somet) 会携带一个参数，表示有值；[None](#none) 不带参数，表示无值。当需要表示某个类型可能有值，也可能没有值的时候，可选择使用 [Option](core_package_enums.md#enum-optiont) 类型。

[Option](core_package_enums.md#enum-optiont) 类型的另一种写法是在类型名前加 `?`，即对于任意类型 `Type`，`?Type` 等价于 [Option](core_package_enums.md#enum-optiont)\<Type>。

扩展：

- [extend\<T> Option\<Option\<T>>](core_package_extends.md#extendt-optionoptiont)
- [extend\<T> Option\<T> <: Equatable\<Option\<T>> where T <: Equatable\<T>](core_package_extends.md#extendt-optiont--equatableoptiont-where-t--equatablet)
- [extend\<T> Option\<T> <: Hashable where T <: Hashable](core_package_extends.md#extendt-optiont--hashable-where-t--hashable)
- [extend\<T> Option\<T> <: ToString where T <: ToString](core_package_extends.md#extendt-optiont--tostring-where-t--tostring)

### None

```cangjie
None
```

功能：构造一个不带参数的 [Option](core_package_enums.md#enum-optiont)\<T> 实例，表示无值。

### Some(T)

```cangjie
Some(T)
```

功能：构造一个携带参数的 [Option](core_package_enums.md#enum-optiont)\<T> 实例，表示有值。

### func filter((T)->Bool)

```cangjie
public func filter(predicate: (T) -> Bool): Option<T>
```

功能：提供 [Option](core_package_enums.md#enum-optiont) 类型的“过滤”功能。

参数：

- predicate: (T) -> [Bool](core_package_intrinsics.md#bool) - 过滤函数。

返回值：

- Option\<T> - 如果 [Option](core_package_enums.md#enum-optiont) 值是 [Some](#somet)(v)，并且 v 满足 `predicate(v) = true` 时，返回 [Some](#somet)(v)， 否则返回 [None](#none)。

示例：

<!-- verify -->
```cangjie
main() {
    // 创建一个Some值
    var someValue: Option<Int64> = Some(5)

    // 使用filter过滤大于3的值
    var filtered1 = someValue.filter({x => x > 3})
    println("过滤大于3的值: ${filtered1}")

    // 使用filter过滤小于3的值
    var filtered2 = someValue.filter({x => x < 3})
    println("过滤小于3的值: ${filtered2}")

    // 创建一个None值
    var noneValue: Option<Int64> = None

    // 对None值使用filter
    var filtered3 = noneValue.filter({x => x > 3})
    println("对None值过滤: ${filtered3}")
}
```

运行结果：

```text
过滤大于3的值: Some(5)
过滤小于3的值: None
对None值过滤: None
```

### func flatMap\<R>((T) -> Option\<R>)

```cangjie
public func flatMap<R>(transform: (T) -> Option<R>): Option<R>
```

功能：提供从 [Option](core_package_enums.md#enum-optiont)\<T> 类型到 [Option](core_package_enums.md#enum-optiont)\<R> 类型的映射函数，如果当前实例值是 [Some](#somet)，执行 transform 函数，并且返回结果，否则返回 [None](#none)。

参数：

- transform: (T) -> [Option](core_package_enums.md#enum-optiont)\<R> - 映射函数。

返回值：

- [Option](core_package_enums.md#enum-optiont)\<R> - 如果当前实例值是 [Some](#somet)，执行 transform 函数并返回，否则返回 [None](#none)。

示例：

<!-- verify -->
```cangjie
// 定义一个函数，将Int64转换为Option<String>
func intToStringOption(x: Int64): Option<String> {
    if (x > 0) {
        return Some("Positive: ${x}")
    } else {
        return None
    }
}

main() {
    // 创建一个Some值
    var someValue: Option<Int64> = Some(5)

    // 使用flatMap将Int64转换为String
    var flatMapped1 = someValue.flatMap(intToStringOption)
    println("对Some值使用flatMap: ${flatMapped1}")

    // 创建一个负数的Some值
    var someNegativeValue: Option<Int64> = Some(-3)

    // 使用flatMap处理负数
    var flatMapped2 = someNegativeValue.flatMap(intToStringOption)
    println("对负数Some值使用flatMap: ${flatMapped2}")

    // 创建一个None值
    var noneValue: Option<Int64> = None

    // 对None值使用flatMap
    var flatMapped3 = noneValue.flatMap(intToStringOption)
    println("对None值使用flatMap: ${flatMapped3}")
}
```

运行结果：

```text
对Some值使用flatMap: Some(Positive: 5)
对负数Some值使用flatMap: None
对None值使用flatMap: None
```

### func getOrDefault(() -> T)

```cangjie
public func getOrDefault(other: () -> T): T
```

功能：获得值或返回默认值。如果 [Option](core_package_enums.md#enum-optiont) 值是 [Some](#somet)，则返回类型为 `T` 的实例，如果 [Option](core_package_enums.md#enum-optiont) 值是 [None](#none)，则调用入参，返回类型 `T` 的值。

参数：

- other: () -> T - 默认函数，如果当前实例的值是 [None](#none)，调用该函数得到类型为 `T` 的实例，并将其返回。

返回值：

- T - 如果当前实例的值是 [Some](#somet)\<T>，则返回当前实例携带的类型为 `T` 的实例，如果 [Option](core_package_enums.md#enum-optiont) 值是 [None](#none)，调用入参指定的函数，得到类型为 `T` 的实例，并将其返回。

示例：

<!-- verify -->
```cangjie
main() {
    var value1: Option<Int64> = Some(2)
    println(value1.getOrDefault({=> 0}))

    var value2: Option<Int64> = None
    println(value2.getOrDefault({=> 0}))
}
```

运行结果：

```text
2
0
```

### func getOrThrow(() -> Exception)

```cangjie
public func getOrThrow(exception: ()->Exception): T
```

功能：获得值或抛出指定异常。

参数：

- exception: () ->[Exception](core_package_exceptions.md#class-exception) - 异常函数，如果当前实例值是 [None](#none)，将执行该函数并将其返回值作为异常抛出。

返回值：

- T - 如果当前实例值是 [Some](#somet)\<T>，返回类型为 `T` 的实例。

异常：

- [Exception](core_package_exceptions.md#class-exception) - 如果当前实例是 [None](#none)，抛出异常函数返回的异常。

示例：

<!-- verify -->
```cangjie
main() {
    // 创建一个Some值
    var someValue: Option<Int64> = Some(42)

    // 对Some值使用getOrThrow，应该返回值
    var value1 = someValue.getOrThrow({=> Exception("Value is None")})
    println("从Some值获取的值: ${value1}")

    // 创建一个None值
    var noneValue: Option<Int64> = None

    // 对None值使用getOrThrow，应该抛出异常
    try {
        noneValue.getOrThrow({=> Exception("Value is None")})
        println("这行不会被执行")
    } catch (e: Exception) {
        println("捕获到异常: ${e.message}")
    }
}
```

运行结果：

```text
从Some值获取的值: 42
捕获到异常: Value is None
```

### func getOrThrow()

```cangjie
public func getOrThrow(): T
```

功能：获得值或抛出异常。

返回值：

- T - 如果当前实例值是 [Some](#somet)\<T>，返回类型为 `T` 的实例。

异常：

- [NoneValueException](core_package_exceptions.md#class-nonevalueexception) - 如果当前实例是 [None](#none)，抛出异常。

示例：

<!-- verify -->
```cangjie
main() {
    // 创建一个Some值
    var someValue: Option<Int64> = Some(42)

    // 对Some值使用getOrThrow，应该返回值
    var value1 = someValue.getOrThrow()
    println("从Some值获取的值: ${value1}")

    // 创建一个None值
    var noneValue: Option<Int64> = None

    // 对None值使用getOrThrow，应该抛出NoneValueException
    try {
        noneValue.getOrThrow()
        println("这行不会被执行")
    } catch (e: NoneValueException) {
        println("捕获到NoneValueException异常")
    }
}
```

运行结果：

```text
从Some值获取的值: 42
捕获到NoneValueException异常
```

### func isNone()

```cangjie
public func isNone(): Bool
```

功能：判断当前实例值是否为 [None](#none)。

返回值：

- [Bool](core_package_intrinsics.md#bool) - 如果当前实例值是 [None](#none)，则返回 true，否则返回 false。

示例：

<!-- verify -->
```cangjie
main() {
    // 创建一个Some值
    var someValue: Option<Int64> = Some(42)

    // 检查是否为None
    println("Some(42) is None: ${someValue.isNone()}")

    // 创建一个None值
    var noneValue: Option<Int64> = None

    // 检查是否为None
    println("None is None: ${noneValue.isNone()}")
}
```

运行结果：

```text
Some(42) is None: false
None is None: true
```

### func isSome()

```cangjie
public func isSome(): Bool
```

功能：判断当前实例值是否为 [Some](#somet)。

返回值：

- [Bool](core_package_intrinsics.md#bool) - 如果当前实例值是 [Some](#somet)，则返回 true，否则返回 false。

示例：

<!-- verify -->
```cangjie
main() {
    // 创建一个Some值
    var someValue: Option<Int64> = Some(42)

    // 检查是否为Some
    println("Some(42) is Some: ${someValue.isSome()}")

    // 创建一个None值
    var noneValue: Option<Int64> = None

    // 检查是否为Some
    println("None is Some: ${noneValue.isSome()}")
}
```

运行结果：

```text
Some(42) is Some: true
None is Some: false
```

### func map\<R>((T)->R)

```cangjie
public func map<R>(transform: (T)-> R): Option<R>
```

功能：提供从 [Option](#enum-optiont)\<T> 类型到 [Option](#enum-optiont)\<R> 类型的映射函数，如果当前实例值是 [Some](#somet)，执行 transform 函数，并且返回 [Some](#somet) 封装的结果，否则返回 [None](#none)。

参数：

- transform: (T)-> R - 映射函数。

返回值：

- [Option](core_package_enums.md#enum-optiont)\<R> - 如果当前实例值是 [Some](#somet)，执行 transform 函数，并且返回 [Option](#enum-optiont)\<R> 类型的结果，否则返回 [None](#none)。

示例：

<!-- verify -->
```cangjie
main() {
    // 创建一个Some值
    var someValue: Option<Int64> = Some(42)

    // 使用map将Int64转换为String
    var mapped1 = someValue.map({x => "Number: ${x}"})
    println("对Some值使用map: ${mapped1}")

    // 创建一个None值
    var noneValue: Option<Int64> = None

    // 对None值使用map
    var mapped2 = noneValue.map({x => "Number: ${x}"})
    println("对None值使用map: ${mapped2}")
}
```

运行结果：

```text
对Some值使用map: Some(Number: 42)
对None值使用map: None
```

## enum Ordering

```cangjie
public enum Ordering
```

功能：[Ordering](core_package_enums.md#enum-ordering) 表示比较大小的结果，它包含三种情况：小于，大于和等于。

扩展：

- [extend Ordering <: Comparable](core_package_extends.md#extend-ordering--comparable)
- [extend Ordering <: Hashable](core_package_extends.md#extend-ordering--hashable)
- [extend Ordering <: ToString](core_package_extends.md#extend-ordering--tostring)

### EQ

```cangjie
EQ
```

功能：构造一个 [Ordering](core_package_enums.md#enum-ordering) 实例，表示等于。

### GT

```cangjie
GT
```

功能：构造一个 [Ordering](core_package_enums.md#enum-ordering) 实例，表示大于。

### LT

```cangjie
LT
```

功能：构造一个 [Ordering](core_package_enums.md#enum-ordering) 实例，表示小于。

## enum ThreadState

```cangjie
public enum ThreadState <: ToString
```

功能：表示线程的状态。

父类型：

- [ToString](core_package_interfaces.md#interface-tostring)

### Pending

```cangjie
Pending
```

功能：表示线程正被挂起。

### Ready

```cangjie
Ready
```

功能：表示线程刚创建或结束挂起，正在等待被调度执行。

### Running

```cangjie
Running
```

功能：表示线程正在执行。

### Terminated

```cangjie
Terminated
```

功能：表示线程已结束执行。

### func toString()

```cangjie
public func toString(): String
```

功能：将 [ThreadState](core_package_enums.md#enum-threadstate) 转换为字符串。

转换结果如下：

- Ready: "Ready"。
- Running: "Running"。
- Pending: "Pending"。
- Terminated: "Terminated"。

返回值：

- [String](core_package_structs.md#struct-string) - 转化后的字符串。

示例：

<!-- verify -->
```cangjie
main() {
    let t = ThreadState.Running
    println("Thread State: ${t}")
}
```

运行结果：

```text
Thread State: Running
```
