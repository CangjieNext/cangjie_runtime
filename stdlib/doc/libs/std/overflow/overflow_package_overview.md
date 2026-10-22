# std.overflow

## 功能介绍

overflow 包提供了整数运算溢出时的处理能力。

在整数运算时，若运算结果大于其类型最大值或小于其类型最小值即是溢出。默认情况下，出现溢出时会抛出异常。

overflow 包提供了四种溢出处理策略，并定义了对应的接口，列举如下：

| 策略                   | 接口                          | 描述                                           |
| ---------------------- | ----------------------------- | --------------------------------------------- |
| 返回 Option         | [CheckedOp](./overflow_package_api/overflow_package_interfaces.md#interface-checkedopt)    | 当整数运算出现溢出，返回 `None`。                  |
| 饱和                    | [SaturatingOp](./overflow_package_api/overflow_package_interfaces.md#interface-saturatingopt) | 当计算结果大于目标类型的 MAX 值，返回 MAX 值；当计算结果小于目标类型的 MIN 值，返回 MIN 值。 |
| 抛出异常                | [ThrowingOp](./overflow_package_api/overflow_package_interfaces.md#interface-throwingopt)   | 当整数运算出现溢出，抛出异常。                              |
| 高位截断                | [WrappingOp](./overflow_package_api/overflow_package_interfaces.md#interface-wrappingopt)   | 当整数运算出现溢出，将运算结果中超出目标类型位数的高位截断。                              |

overflow 包中通过扩展为所有的整数类型提供了这些接口的实现，用户可以用同样的方式为其他类型实现 overflow 接口。

## API 列表

### 接口

|              接口名          |           功能           |
| --------------------------- | ------------------------ |
| [CarryingOp](./overflow_package_api/overflow_package_interfaces.md#interface-carryingopt)    | 提供返回整数运算是否发生了截断以及运算结果的接口。                        |
| [CarryingPow](./overflow_package_api/overflow_package_interfaces.md#interface-carryingpow)    | 提供使用 [wrapping](./overflow_package_api//overflow_package_interfaces.md#interface-wrappingopt) 策略的幂运算接口。                        |
| [CheckedOp](./overflow_package_api/overflow_package_interfaces.md#interface-checkedopt)    | 当整数运算出现溢出，返回 `None`。                        |
| [CheckedPow](./overflow_package_api/overflow_package_interfaces.md#interface-checkedpow)    | 提供返回 [Option](../core/core_package_api/core_package_enums.md#enum-optiont) 策略的幂运算接口。                        |
| [SaturatingOp](./overflow_package_api/overflow_package_interfaces.md#interface-saturatingopt) | 当整数运算出现溢出，饱和处理。                           |
| [SaturatingPow](./overflow_package_api/overflow_package_interfaces.md#interface-saturatingpow) | 提供饱和策略的幂运算接口。                           |
| [ThrowingOp](./overflow_package_api/overflow_package_interfaces.md#interface-throwingopt)   | 当整数运算出现溢出，抛出异常。                           |
| [ThrowingPow](./overflow_package_api/overflow_package_interfaces.md#interface-throwingpow)   | 提供使用抛出异常策略的幂运算接口。                           |
| [WrappingOp](./overflow_package_api/overflow_package_interfaces.md#interface-wrappingopt)   | 当整数运算出现溢出，将运算结果中超出目标类型位数的高位截断。                           |
| [WrappingPow](./overflow_package_api/overflow_package_interfaces.md#interface-wrappingpow)   | 提供使用高位截断策略的幂运算接口。                           |

### 异常类

|                 类名              |                功能                 |
| --------------------------------- | ---------------------------------- |
| [OvershiftException](./overflow_package_api/overflow_package_exceptions.md#class-overshiftexception) | 移位运算时移位位数超过操作数位数时抛出的异常。       |
| [UndershiftException](./overflow_package_api/overflow_package_exceptions.md#class-undershiftexception) | 移位运算时移位位数小于 0 时抛出的异常。          |

### 扩展

| 扩展名 | 功能 |
| --- | --- |
| [extend Int16 <: CarryingOp\<Int16>](overflow_package_api/overflow_package_extends.md#extend-int16--carryingopint16) | 为 [Int16](../core/core_package_api/core_package_intrinsics.md#int16) 实现 [CarryingOp](overflow_package_api/overflow_package_interfaces.md#interface-carryingopt) 接口。 |
| [extend Int16 <: CheckedOp\<Int16>](overflow_package_api/overflow_package_extends.md#extend-int16--checkedopint16) | 为 [Int16](../core/core_package_api/core_package_intrinsics.md#int16) 实现 [CheckedOp](overflow_package_api/overflow_package_interfaces.md#interface-checkedopt) 接口。 |
| [extend Int16 <: SaturatingOp\<Int16>](overflow_package_api/overflow_package_extends.md#extend-int16--saturatingopint16) | 为 [Int16](../core/core_package_api/core_package_intrinsics.md#int16) 实现 [SaturatingOp](overflow_package_api/overflow_package_interfaces.md#interface-saturatingopt) 接口。 |
| [extend Int16 <: ThrowingOp\<Int16>](overflow_package_api/overflow_package_extends.md#extend-int16--throwingopint16) | 为 [Int16](../core/core_package_api/core_package_intrinsics.md#int16) 实现 [ThrowingOp](overflow_package_api/overflow_package_interfaces.md#interface-throwingopt) 接口。 |
| [extend Int16 <: WrappingOp\<Int16>](overflow_package_api/overflow_package_extends.md#extend-int16--wrappingopint16) | 为 [Int16](../core/core_package_api/core_package_intrinsics.md#int16) 实现 [WrappingOp](overflow_package_api/overflow_package_interfaces.md#interface-wrappingopt) 接口。 |
| [extend Int32 <: CarryingOp\<Int32>](overflow_package_api/overflow_package_extends.md#extend-int32--carryingopint32) | 为 [Int32](../core/core_package_api/core_package_intrinsics.md#int32) 实现 [CarryingOp](overflow_package_api/overflow_package_interfaces.md#interface-carryingopt) 接口。 |
| [extend Int32 <: CheckedOp\<Int32>](overflow_package_api/overflow_package_extends.md#extend-int32--checkedopint32) | 为 [Int32](../core/core_package_api/core_package_intrinsics.md#int32) 实现 [CheckedOp](overflow_package_api/overflow_package_interfaces.md#interface-checkedopt) 接口。 |
| [extend Int32 <: SaturatingOp\<Int32>](overflow_package_api/overflow_package_extends.md#extend-int32--saturatingopint32) | 为 [Int32](../core/core_package_api/core_package_intrinsics.md#int32) 实现 [SaturatingOp](overflow_package_api/overflow_package_interfaces.md#interface-saturatingopt) 接口。 |
| [extend Int32 <: ThrowingOp\<Int32>](overflow_package_api/overflow_package_extends.md#extend-int32--throwingopint32) | 为 [Int32](../core/core_package_api/core_package_intrinsics.md#int32) 实现 [ThrowingOp](overflow_package_api/overflow_package_interfaces.md#interface-throwingopt) 接口。 |
| [extend Int32 <: WrappingOp\<Int32>](overflow_package_api/overflow_package_extends.md#extend-int32--wrappingopint32) | 为 [Int32](../core/core_package_api/core_package_intrinsics.md#int32) 实现 [WrappingOp](overflow_package_api/overflow_package_interfaces.md#interface-wrappingopt) 接口。 |
| [extend Int64 <: CarryingOp\<Int64> & CarryingPow](overflow_package_api/overflow_package_extends.md#extend-int64--carryingopint64--carryingpow) | 为 [Int64](../core/core_package_api/core_package_intrinsics.md#int64) 实现 [CarryingOp](overflow_package_api/overflow_package_interfaces.md#interface-carryingopt) 接口和 [CarryingPow](overflow_package_api/overflow_package_interfaces.md#interface-carryingpow) 接口。 |
| [extend Int64 <: CheckedOp\<Int64> & CheckedPow](overflow_package_api/overflow_package_extends.md#extend-int64--checkedopint64--checkedpow) | 为 [Int64](../core/core_package_api/core_package_intrinsics.md#int64) 实现 [CheckedOp](overflow_package_api/overflow_package_interfaces.md#interface-checkedopt) 和 [CheckedPow](overflow_package_api/overflow_package_interfaces.md#interface-checkedpow) 接口。 |
| [extend Int64 <: SaturatingOp\<Int64> & SaturatingPow](overflow_package_api/overflow_package_extends.md#extend-int64--saturatingopint64--saturatingpow) | 为 [Int64](../core/core_package_api/core_package_intrinsics.md#int64) 实现 [SaturatingOp](overflow_package_api/overflow_package_interfaces.md#interface-saturatingopt) 和 [SaturatingPow](overflow_package_api/overflow_package_interfaces.md#interface-saturatingpow) 接口。 |
| [extend Int64 <: ThrowingOp\<Int64> & ThrowingPow](overflow_package_api/overflow_package_extends.md#extend-int64--throwingopint64--throwingpow) | 为 [Int64](../core/core_package_api/core_package_intrinsics.md#int64) 实现 [ThrowingOp](overflow_package_api/overflow_package_interfaces.md#interface-throwingopt) 和 [ThrowingPow](overflow_package_api/overflow_package_interfaces.md#interface-throwingpow) 接口。 |
| [extend Int64 <: WrappingOp\<Int64> & WrappingPow](overflow_package_api/overflow_package_extends.md#extend-int64--wrappingopint64--wrappingpow) | 为 [Int64](../core/core_package_api/core_package_intrinsics.md#int64) 实现 [WrappingOp](overflow_package_api/overflow_package_interfaces.md#interface-wrappingopt) 和 [WrappingPow](overflow_package_api/overflow_package_interfaces.md#interface-wrappingpow) 接口。 |
| [extend Int8 <: CarryingOp\<Int8>](overflow_package_api/overflow_package_extends.md#extend-int8--carryingopint8) | 为 [Int8](../core/core_package_api/core_package_intrinsics.md#int8) 实现 [CarryingOp](overflow_package_api/overflow_package_interfaces.md#interface-carryingopt) 接口。 |
| [extend Int8 <: CheckedOp\<Int8>](overflow_package_api/overflow_package_extends.md#extend-int8--checkedopint8) | 为 [Int8](../core/core_package_api/core_package_intrinsics.md#int8) 实现 [CheckedOp](overflow_package_api/overflow_package_interfaces.md#interface-checkedopt) 接口。 |
| [extend Int8 <: SaturatingOp\<Int8>](overflow_package_api/overflow_package_extends.md#extend-int8--saturatingopint8) | 为 [Int8](../core/core_package_api/core_package_intrinsics.md#int8) 实现 [SaturatingOp](overflow_package_api/overflow_package_interfaces.md#interface-saturatingopt) 接口。 |
| [extend Int8 <: ThrowingOp\<Int8>](overflow_package_api/overflow_package_extends.md#extend-int8--throwingopint8) | 为 [Int8](../core/core_package_api/core_package_intrinsics.md#int8) 实现 [ThrowingOp](overflow_package_api/overflow_package_interfaces.md#interface-throwingopt) 接口。 |
| [extend Int8 <: WrappingOp\<Int8>](overflow_package_api/overflow_package_extends.md#extend-int8--wrappingopint8) | 为 [Int8](../core/core_package_api/core_package_intrinsics.md#int8) 实现 [WrappingOp](overflow_package_api/overflow_package_interfaces.md#interface-wrappingopt) 接口。 |
| [extend IntNative <: CarryingOp\<IntNative>](overflow_package_api/overflow_package_extends.md#extend-intnative--carryingopintnative) | 为 [IntNative](../core/core_package_api/core_package_intrinsics.md#intnative) 实现 [CarryingOp](overflow_package_api/overflow_package_interfaces.md#interface-carryingopt) 接口。 |
| [extend IntNative <: CheckedOp\<IntNative>](overflow_package_api/overflow_package_extends.md#extend-intnative--checkedopintnative) | 为 [IntNative](../core/core_package_api/core_package_intrinsics.md#intnative) 实现 [CheckedOp](overflow_package_api/overflow_package_interfaces.md#interface-checkedopt) 接口。 |
| [extend IntNative <: SaturatingOp\<IntNative>](overflow_package_api/overflow_package_extends.md#extend-intnative--saturatingopintnative) | 为 [IntNative](../core/core_package_api/core_package_intrinsics.md#intnative) 实现 [SaturatingOp](overflow_package_api/overflow_package_interfaces.md#interface-saturatingopt) 接口。 |
| [extend IntNative <: ThrowingOp\<IntNative>](overflow_package_api/overflow_package_extends.md#extend-intnative--throwingopintnative) | 为 [IntNative](../core/core_package_api/core_package_intrinsics.md#intnative) 实现 [ThrowingOp](overflow_package_api/overflow_package_interfaces.md#interface-throwingopt) 接口。 |
| [extend IntNative <: WrappingOp\<IntNative>](overflow_package_api/overflow_package_extends.md#extend-intnative--wrappingopintnative) | 为 [IntNative](../core/core_package_api/core_package_intrinsics.md#intnative) 实现 [WrappingOp](overflow_package_api/overflow_package_interfaces.md#interface-wrappingopt) 接口。 |
| [extend UInt16 <: CarryingOp\<UInt16>](overflow_package_api/overflow_package_extends.md#extend-uint16--carryingopuint16) | 为 [UInt16](../core/core_package_api/core_package_intrinsics.md#uint16) 实现 [CarryingOp](overflow_package_api/overflow_package_interfaces.md#interface-carryingopt) 接口。 |
| [extend UInt16 <: CheckedOp\<UInt16>](overflow_package_api/overflow_package_extends.md#extend-uint16--checkedopuint16) | 为 [UInt16](../core/core_package_api/core_package_intrinsics.md#uint16) 实现 [CheckedOp](overflow_package_api/overflow_package_interfaces.md#interface-checkedopt) 接口。 |
| [extend UInt16 <: SaturatingOp\<UInt16>](overflow_package_api/overflow_package_extends.md#extend-uint16--saturatingopuint16) | 为 [UInt16](../core/core_package_api/core_package_intrinsics.md#uint16) 实现 [SaturatingOp](overflow_package_api/overflow_package_interfaces.md#interface-saturatingopt) 接口。 |
| [extend UInt16 <: ThrowingOp\<UInt16>](overflow_package_api/overflow_package_extends.md#extend-uint16--throwingopuint16) | 为 [UInt16](../core/core_package_api/core_package_intrinsics.md#uint16) 实现 [ThrowingOp](overflow_package_api/overflow_package_interfaces.md#interface-throwingopt) 接口。 |
| [extend UInt16 <: WrappingOp\<UInt16>](overflow_package_api/overflow_package_extends.md#extend-uint16--wrappingopuint16) | 为 [UInt16](../core/core_package_api/core_package_intrinsics.md#uint16) 实现 [WrappingOp](overflow_package_api/overflow_package_interfaces.md#interface-wrappingopt) 接口。 |
| [extend UInt32 <: CarryingOp\<UInt32>](overflow_package_api/overflow_package_extends.md#extend-uint32--carryingopuint32) | 为 [UInt32](../core/core_package_api/core_package_intrinsics.md#uint32) 实现 [CarryingOp](overflow_package_api/overflow_package_interfaces.md#interface-carryingopt) 接口。 |
| [extend UInt32 <: CheckedOp\<UInt32>](overflow_package_api/overflow_package_extends.md#extend-uint32--checkedopuint32) | 为 [UInt32](../core/core_package_api/core_package_intrinsics.md#uint32) 实现 [CheckedOp](overflow_package_api/overflow_package_interfaces.md#interface-checkedopt) 接口。 |
| [extend UInt32 <: SaturatingOp\<UInt32>](overflow_package_api/overflow_package_extends.md#extend-uint32--saturatingopuint32) | 为 [UInt32](../core/core_package_api/core_package_intrinsics.md#uint32) 实现 [SaturatingOp](overflow_package_api/overflow_package_interfaces.md#interface-saturatingopt) 接口。 |
| [extend UInt32 <: ThrowingOp\<UInt32>](overflow_package_api/overflow_package_extends.md#extend-uint32--throwingopuint32) | 为 [UInt32](../core/core_package_api/core_package_intrinsics.md#uint32) 实现 [ThrowingOp](overflow_package_api/overflow_package_interfaces.md#interface-throwingopt) 接口。 |
| [extend UInt32 <: WrappingOp\<UInt32>](overflow_package_api/overflow_package_extends.md#extend-uint32--wrappingopuint32) | 为 [UInt32](../core/core_package_api/core_package_intrinsics.md#uint32) 实现 [WrappingOp](overflow_package_api/overflow_package_interfaces.md#interface-wrappingopt) 接口。 |
| [extend UInt64 <: CarryingOp\<UInt64>](overflow_package_api/overflow_package_extends.md#extend-uint64--carryingopuint64) | 为 [UInt64](../core/core_package_api/core_package_intrinsics.md#uint64) 实现 [CarryingOp](overflow_package_api/overflow_package_interfaces.md#interface-carryingopt) 接口。 |
| [extend UInt64 <: CheckedOp\<UInt64>](overflow_package_api/overflow_package_extends.md#extend-uint64--checkedopuint64) | 为 [UInt64](../core/core_package_api/core_package_intrinsics.md#uint64) 实现 [CheckedOp](overflow_package_api/overflow_package_interfaces.md#interface-checkedopt) 接口。 |
| [extend UInt64 <: SaturatingOp\<UInt64>](overflow_package_api/overflow_package_extends.md#extend-uint64--saturatingopuint64) | 为 [UInt64](../core/core_package_api/core_package_intrinsics.md#uint64) 实现 [SaturatingOp](overflow_package_api/overflow_package_interfaces.md#interface-saturatingopt) 接口。 |
| [extend UInt64 <: ThrowingOp\<UInt64>](overflow_package_api/overflow_package_extends.md#extend-uint64--throwingopuint64) | 为 [UInt64](../core/core_package_api/core_package_intrinsics.md#uint64) 实现 [ThrowingOp](overflow_package_api/overflow_package_interfaces.md#interface-throwingopt) 接口。 |
| [extend UInt64 <: WrappingOp\<UInt64>](overflow_package_api/overflow_package_extends.md#extend-uint64--wrappingopuint64) | 为 [UInt64](../core/core_package_api/core_package_intrinsics.md#uint64) 实现 [WrappingOp](overflow_package_api/overflow_package_interfaces.md#interface-wrappingopt) 接口。 |
| [extend UInt8 <: CarryingOp\<UInt8>](overflow_package_api/overflow_package_extends.md#extend-uint8--carryingopuint8) | 为 [UInt8](../core/core_package_api/core_package_intrinsics.md#uint8) 实现 [CarryingOp](overflow_package_api/overflow_package_interfaces.md#interface-carryingopt) 接口。 |
| [extend UInt8 <: CheckedOp\<UInt8>](overflow_package_api/overflow_package_extends.md#extend-uint8--checkedopuint8) | 为 [UInt8](../core/core_package_api/core_package_intrinsics.md#uint8) 实现 [CheckedOp](overflow_package_api/overflow_package_interfaces.md#interface-checkedopt) 接口。 |
| [extend UInt8 <: SaturatingOp\<UInt8>](overflow_package_api/overflow_package_extends.md#extend-uint8--saturatingopuint8) | 为 [UInt8](../core/core_package_api/core_package_intrinsics.md#uint8) 实现 [SaturatingOp](overflow_package_api/overflow_package_interfaces.md#interface-saturatingopt) 接口。 |
| [extend UInt8 <: ThrowingOp\<UInt8>](overflow_package_api/overflow_package_extends.md#extend-uint8--throwingopuint8) | 为 [UInt8](../core/core_package_api/core_package_intrinsics.md#uint8) 实现 [ThrowingOp](overflow_package_api/overflow_package_interfaces.md#interface-throwingopt) 接口。 |
| [extend UInt8 <: WrappingOp\<UInt8>](overflow_package_api/overflow_package_extends.md#extend-uint8--wrappingopuint8) | 为 [UInt8](../core/core_package_api/core_package_intrinsics.md#uint8) 实现 [WrappingOp](overflow_package_api/overflow_package_interfaces.md#interface-wrappingopt) 接口。 |
| [extend UIntNative <: CarryingOp\<UIntNative>](overflow_package_api/overflow_package_extends.md#extend-uintnative--carryingopuintnative) | 为 [UIntNative](../core/core_package_api/core_package_intrinsics.md#uintnative) 实现 [CarryingOp](overflow_package_api/overflow_package_interfaces.md#interface-carryingopt) 接口。 |
| [extend UIntNative <: CheckedOp\<UIntNative>](overflow_package_api/overflow_package_extends.md#extend-uintnative--checkedopuintnative) | 为 [UIntNative](../core/core_package_api/core_package_intrinsics.md#uintnative) 实现 [CheckedOp](overflow_package_api/overflow_package_interfaces.md#interface-checkedopt) 接口。 |
| [extend UIntNative <: SaturatingOp\<UIntNative>](overflow_package_api/overflow_package_extends.md#extend-uintnative--saturatingopuintnative) | 为 [UIntNative](../core/core_package_api/core_package_intrinsics.md#uintnative) 实现 [SaturatingOp](overflow_package_api/overflow_package_interfaces.md#interface-saturatingopt) 接口。 |
| [extend UIntNative <: ThrowingOp\<UIntNative>](overflow_package_api/overflow_package_extends.md#extend-uintnative--throwingopuintnative) | 为 [UIntNative](../core/core_package_api/core_package_intrinsics.md#uintnative) 实现 [ThrowingOp](overflow_package_api/overflow_package_interfaces.md#interface-throwingopt) 接口。 |
| [extend UIntNative <: WrappingOp\<UIntNative>](overflow_package_api/overflow_package_extends.md#extend-uintnative--wrappingopuintnative) | 为 [UIntNative](../core/core_package_api/core_package_intrinsics.md#uintnative) 实现 [WrappingOp](overflow_package_api/overflow_package_interfaces.md#interface-wrappingopt) 接口。 |
