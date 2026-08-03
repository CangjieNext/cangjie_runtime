# 结构体

> **说明：**
>
> 1. 普通情况：与 [class 内容模板](./xxx_package_classes.md)一致，如 [Struct1](#struct-struct1)。
> 2. 特殊情况：如果 struct 被 `@C` 修饰，
>
>     - 在声明中给出 `@C`；
>     - 在内容中给出 “C 互操作内存布局参考”字段（体现 let/var 变量布局，顺序与源码一致）。模板示例如 [Struct2](#struct-struct2)。
>     - 如果该 struct 存在非公开成员变量，单独给说明提醒。

## struct Struct1

参考 [class 内容模板](./xxx_package_classes.md)

## struct Struct2

```cangjie
@C
public struct Struct2
```

【必写】功能： xxx

【涉及必写】C 互操作内存布局参考：

```cangjie
public let xxx
public var xxx
```

> 【涉及必写】**说明：**
>
> 该结构体存在未公开成员变量，其布局参考行为未定义。
