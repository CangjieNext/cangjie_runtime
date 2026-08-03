# 扩展

> **说明：**
>
> 1. 扩展(extend)按 **直接扩展、接口扩展** 的顺序展示。直接扩展、接口扩展分别按 **字母顺序** 排列。扩展下的子API顺序参考[class](./xxx_package_classes.md)子API顺序。
> 2. extend文档写在其代码定义的package中。
> 3. extend文档中需要体现的成员与其.cj.d中的实现保持一致。
> 4. 扩展与被扩展API需要提供互相链接，来体现扩展关系。详见示例。

## extend Class1（可带泛型，如 extend\<T> A\<T>）

【必写】（定义声明）

```cangjie
extend Class1
```

【必写】功能，参考 [功能模块写作规范](../../../../overview.md#功能必写)。

【必写】被扩展类型：

- [Class1](./xxx_package_classes.md#class-class1)

### func a()

参考 [函数模板](./xxx_package_funcs.md)

### func b()

参考 [函数模板](./xxx_package_funcs.md)

## extend Class2 <: Interface1 & Interface2

【必写】（定义声明）

```cangjie
extend Class2 <: Interface1 & Interface2
```

【必写】功能，参考 [功能模块写作规范](../../../../overview.md#功能必写)。

【必写】被扩展类型：

- [Class2](./xxx_package_classes.md#class-class2)

【必写】扩展接口：

- [Interface1](./xxx_package_interfaces.md#interface-interface1)
- [Interface2](./xxx_package_interfaces.md#interface-interface2)

### func d()

参考 [函数模板](./xxx_package_funcs.md)
