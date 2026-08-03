# 类

> **说明：**
>
> 1. 类(class)按 **字母顺序** 展示。
> 2. 对于class内的子API，按 **成员变量(static let/let/var)、属性(static prop/prop)、const构造函数(const init)、构造函数(init)、函数(static func/func)、操作符函数(operator func)、扩展(extend)** 的类型顺序展示。
> 3. class的各子类型内部，子API按 **字母顺序** 排列。
> 4. 对于源码中的主构造函数，按照仓颉语法糖解糖后普通构造方式呈现（注意若主构造中有公开变量，需当作单独子API在文档中呈现）。
> 5. 若子类中重写了父类的函数，需要在文档中体现该function。
> 6. public open/abstract class 的 protected 子成员需给出文档。
> 7. 在文档中提到其他的API都需要使用超链接的方式链过去，例：
>
>    ```text
>    [ArrayList](./xx/xxx.md#class-arraylist)
>    ```

## class Class1

【必写】（定义声明）

```cangjie
public class Class1
```

【必写】功能：提供xxx功能。(*一句话概述API功能*)

【可选】（*功能描述，详细介绍API功能、原理、使用场景等信息*）

【例】

> **说明：**
>
> 不支持平台：Windows

【可选】说明/注意事项，参考 [说明注意格式要求](../../../../overview.md#说明注意警告可选)。

【涉及必写】扩展：

- [extend Class1](./xxx_package_extends.md#extend-class1可带泛型如-extendt-at)

【必写】示例，参考 [示例代码规范及格式要求](../../../../overview.md#示例必写)。

### static let aab

【必写】（定义声明）

```cangjie
public static let aab: dtype
```

【必写】功能：表示xxx。

【必写】类型：[dtype](link)

### let param2

同上

### static prop property1

同上

### prop property1

同上

### const init(dtype)

参考 [函数模板](./xxx_package_funcs.md)

（*注意：功能说明使用 “构造了一个xxx对象”*）

### init()

同上

### init(int, int)

同上

### func function1()

参考 [函数模板](./xxx_package_funcs.md)

### operator func ==(Ordering)

同上

## class class2

【必写】扩展：

- [extend Class2 <: Interface1 & Interface2](./xxx_package_extends.md#extend-class2--interface1--interface2)

模板其他内容同 Class1
