# 接口

## interface CarryingOp\<T>

```cangjie
public interface CarryingOp<T>
```

功能：提供返回整数运算是否发生了截断以及运算结果的接口。

扩展：

- [extend Int16 <: CarryingOp\<Int16>](overflow_package_extends.md#extend-int16--carryingopint16)
- [extend Int32 <: CarryingOp\<Int32>](overflow_package_extends.md#extend-int32--carryingopint32)
- [extend Int64 <: CarryingOp\<Int64> & CarryingPow](overflow_package_extends.md#extend-int64--carryingopint64--carryingpow)
- [extend Int8 <: CarryingOp\<Int8>](overflow_package_extends.md#extend-int8--carryingopint8)
- [extend IntNative <: CarryingOp\<IntNative>](overflow_package_extends.md#extend-intnative--carryingopintnative)
- [extend UInt16 <: CarryingOp\<UInt16>](overflow_package_extends.md#extend-uint16--carryingopuint16)
- [extend UInt32 <: CarryingOp\<UInt32>](overflow_package_extends.md#extend-uint32--carryingopuint32)
- [extend UInt64 <: CarryingOp\<UInt64>](overflow_package_extends.md#extend-uint64--carryingopuint64)
- [extend UInt8 <: CarryingOp\<UInt8>](overflow_package_extends.md#extend-uint8--carryingopuint8)
- [extend UIntNative <: CarryingOp\<UIntNative>](overflow_package_extends.md#extend-uintnative--carryingopuintnative)

### func carryingAdd(T)

```cangjie
func carryingAdd(y: T): (Bool, T)
```

功能：返回一个元组，元组的第一个元素表示加法运算是否发生了截断，发生截断时为 `true`，元组的第二个元素是运算的结果。

参数：

- y: T - 加数。

返回值：

- ([Bool](../../core/core_package_api/core_package_intrinsics.md#bool), T) - 加法运算是否发生截断以及运算的结果。

### func carryingDec()

```cangjie
func carryingDec(): (Bool, T)
```

功能：返回一个元组，元组的第一个元素表示自减运算是否发生了截断，发生截断时为 `true`，元组的第二个元素是运算的结果。

返回值：

- ([Bool](../../core/core_package_api/core_package_intrinsics.md#bool), T) - 自减运算是否发生截断以及运算的结果。

### func carryingDiv(T)

```cangjie
func carryingDiv(y: T): (Bool, T)
```

功能：返回一个元组，元组的第一个元素表示除法运算是否发生了截断，发生截断时为 `true`，元组的第二个元素是运算的结果。

参数：

- y: T - 除数。

返回值：

- ([Bool](../../core/core_package_api/core_package_intrinsics.md#bool), T) - 除法运算是否发生截断以及运算的结果。

### func carryingInc()

```cangjie
func carryingInc(): (Bool, T)
```

功能：返回一个元组，元组的第一个元素表示自增运算是否发生了截断，发生截断时为 `true`，元组的第二个元素是运算的结果。

返回值：

- ([Bool](../../core/core_package_api/core_package_intrinsics.md#bool), T) - 自增运算是否发生截断以及运算的结果。

### func carryingMod(T)

```cangjie
func carryingMod(y: T): (Bool, T)
```

功能：返回一个元组，元组的第一个元素表示取余运算是否发生了截断，发生截断时为 `true`，元组的第二个元素是运算的结果。

参数：

- y: T - 除数。

返回值：

- ([Bool](../../core/core_package_api/core_package_intrinsics.md#bool), T) - 取余运算是否发生截断以及运算的结果。

### func carryingMul(T)

```cangjie
func carryingMul(y: T): (Bool, T)
```

功能：返回一个元组，元组的第一个元素表示乘法运算是否发生了截断，发生截断时为 `true`，元组的第二个元素是运算的结果。

参数：

- y: T - 乘数。

返回值：

- ([Bool](../../core/core_package_api/core_package_intrinsics.md#bool), T) - 乘法运算是否发生截断以及运算的结果。

### func carryingNeg()

```cangjie
func carryingNeg(): (Bool, T)
```

功能：返回一个元组，元组的第一个元素表示负号运算是否发生了截断，发生截断时为 `true`，元组的第二个元素是运算的结果。

返回值：

- ([Bool](../../core/core_package_api/core_package_intrinsics.md#bool), T) - 负号运算是否发生截断以及运算的结果。

### func carryingShl(UInt64)

```cangjie
func carryingShl(y: UInt64): (Bool, T)
```

功能：返回一个元组，元组的第一个元素表示左移运算是否发生了截断，发生截断时为 `true`，元组的第二个元素是运算的结果。

参数：

- y: [UInt64](../../core/core_package_api/core_package_intrinsics.md#uint64) - 移位位数。

返回值：

- ([Bool](../../core/core_package_api/core_package_intrinsics.md#bool), T) - 左移运算是否发生截断以及运算的结果。

### func carryingShr(UInt64)

```cangjie
func carryingShr(y: UInt64): (Bool, T)
```

功能：返回一个元组，元组的第一个元素表示右移运算是否发生了截断，发生截断时为 `true`，元组的第二个元素是运算的结果。

参数：

- y: [UInt64](../../core/core_package_api/core_package_intrinsics.md#uint64) - 移位位数。

返回值：

- ([Bool](../../core/core_package_api/core_package_intrinsics.md#bool), T) - 右移运算是否发生截断以及运算的结果。

### func carryingSub(T)

```cangjie
func carryingSub(y: T): (Bool, T)
```

功能：返回一个元组，元组的第一个元素表示减法运算是否发生了截断，发生截断时为 `true`，元组的第二个元素是运算的结果。

参数：

- y: T - 减数。

返回值：

- ([Bool](../../core/core_package_api/core_package_intrinsics.md#bool), T) - 减法运算是否发生截断以及运算的结果。

## interface CarryingPow

```cangjie
public interface CarryingPow
```

功能：提供使用 [wrapping](./overflow_package_interfaces.md#interface-wrappingopt) 策略的幂运算接口。

扩展：

- [extend Int64 <: CarryingOp\<Int64> & CarryingPow](overflow_package_extends.md#extend-int64--carryingopint64--carryingpow)

### func carryingPow(UInt64)

```cangjie
func carryingPow(y: UInt64): (Bool, Int64)
```

功能：使用 [wrapping](./overflow_package_interfaces.md#interface-wrappingopt) 策略的幂运算。

当运算出现溢出时，返回 `true` 和运算结果，否则返回 `false` 和运算结果。

参数：

- y: [UInt64](../../core/core_package_api/core_package_intrinsics.md#uint64) - 指数。

返回值：

- ([Bool](../../core/core_package_api/core_package_intrinsics.md#bool), [Int64](../../core/core_package_api/core_package_intrinsics.md#int64)) - 返回一个元组，元组的第一个元素表示运算是否发生了截断，发生截断时为 `true`，元组的第二个元素是运算的结果。

## interface CheckedOp\<T>

```cangjie
public interface CheckedOp<T>
```

功能：当整数运算出现溢出，返回 `None`。

扩展：

- [extend Int16 <: CheckedOp\<Int16>](overflow_package_extends.md#extend-int16--checkedopint16)
- [extend Int32 <: CheckedOp\<Int32>](overflow_package_extends.md#extend-int32--checkedopint32)
- [extend Int64 <: CheckedOp\<Int64> & CheckedPow](overflow_package_extends.md#extend-int64--checkedopint64--checkedpow)
- [extend Int8 <: CheckedOp\<Int8>](overflow_package_extends.md#extend-int8--checkedopint8)
- [extend IntNative <: CheckedOp\<IntNative>](overflow_package_extends.md#extend-intnative--checkedopintnative)
- [extend UInt16 <: CheckedOp\<UInt16>](overflow_package_extends.md#extend-uint16--checkedopuint16)
- [extend UInt32 <: CheckedOp\<UInt32>](overflow_package_extends.md#extend-uint32--checkedopuint32)
- [extend UInt64 <: CheckedOp\<UInt64>](overflow_package_extends.md#extend-uint64--checkedopuint64)
- [extend UInt8 <: CheckedOp\<UInt8>](overflow_package_extends.md#extend-uint8--checkedopuint8)
- [extend UIntNative <: CheckedOp\<UIntNative>](overflow_package_extends.md#extend-uintnative--checkedopuintnative)

### func checkedAdd(T)

```cangjie
func checkedAdd(y: T): ?T
```

功能：使用返回 [Option](../../core/core_package_api/core_package_enums.md#enum-optiont) 策略的加法运算。

当运算出现溢出时，返回 `?T.None`，否则返回运算结果。

参数：

- y: T - 加数。

返回值：

- ?T - 加法运算结果。

### func checkedDec()

```cangjie
func checkedDec(): ?T
```

功能：使用返回 [Option](../../core/core_package_api/core_package_enums.md#enum-optiont) 策略的自减运算。

当运算出现溢出时，返回 `?T.None`，否则返回运算结果。

返回值：

- ?T - 自减运算结果。

### func checkedDiv(T)

```cangjie
func checkedDiv(y: T): ?T
```

功能：使用返回 [Option](../../core/core_package_api/core_package_enums.md#enum-optiont) 策略的除法运算。

当运算出现溢出时，返回 `?T.None`，否则返回运算结果。

参数：

- y: T - 除数。

返回值：

- ?T - 除法运算结果。

### func checkedInc()

```cangjie
func checkedInc(): ?T
```

功能：使用返回 [Option](../../core/core_package_api/core_package_enums.md#enum-optiont) 策略的自增运算。

当运算出现溢出时，返回 `?T.None`，否则返回运算结果。

返回值：

- ?T - 自增运算结果。

### func checkedMod(T)

```cangjie
func checkedMod(y: T): ?T
```

功能：使用返回 [Option](../../core/core_package_api/core_package_enums.md#enum-optiont) 策略的取余运算。

当运算出现溢出时，返回 `?T.None`，否则返回运算结果。

参数：

- y: T - 除数。

返回值：

- ?T - 取余运算结果。

### func checkedMul(T)

```cangjie
func checkedMul(y: T): ?T
```

功能：使用返回 [Option](../../core/core_package_api/core_package_enums.md#enum-optiont) 策略的乘法运算。

当运算出现溢出时，返回 `?T.None`，否则返回运算结果。

参数：

- y: T - 乘数。

返回值：

- ?T - 乘法运算结果。

### func checkedNeg()

```cangjie
func checkedNeg(): ?T
```

功能：使用返回 [Option](../../core/core_package_api/core_package_enums.md#enum-optiont) 策略的负号运算。

当运算出现溢出时，返回 `?T.None`，否则返回运算结果。

返回值：

- ?T - 负号运算结果。

### func checkedShl(UInt64)

```cangjie
func checkedShl(y: UInt64): ?T
```

功能：使用返回 [Option](../../core/core_package_api/core_package_enums.md#enum-optiont) 策略的左移运算。

当移位位数大于等于操作数位数时，返回 `?T.None`，否则返回运算结果。

参数：

- y: [UInt64](../../core/core_package_api/core_package_intrinsics.md#uint64) - 移位位数。

返回值：

- ?T - 左移运算结果。

### func checkedShr(UInt64)

```cangjie
func checkedShr(y: UInt64): ?T
```

功能：使用返回 [Option](../../core/core_package_api/core_package_enums.md#enum-optiont) 策略的右移运算。

当移位位数大于等于操作数位数时，返回 `?T.None`，否则返回运算结果。

参数：

- y: [UInt64](../../core/core_package_api/core_package_intrinsics.md#uint64) - 移位位数。

返回值：

- ?T - 右移运算结果。

### func checkedSub(T)

```cangjie
func checkedSub(y: T): ?T
```

功能：使用返回 [Option](../../core/core_package_api/core_package_enums.md#enum-optiont) 策略的减法运算。

当运算出现溢出时，返回 `?T.None`，否则返回运算结果。

参数：

- y: T - 减数。

返回值：

- ?T - 减法运算结果。

## interface CheckedPow

```cangjie
public interface CheckedPow
```

功能：提供返回 [Option](../../core/core_package_api/core_package_enums.md#enum-optiont) 策略的幂运算接口。

扩展：

- [extend Int64 <: CheckedOp\<Int64> & CheckedPow](overflow_package_extends.md#extend-int64--checkedopint64--checkedpow)

### func checkedPow(UInt64)

```cangjie
func checkedPow(y: UInt64): ?Int64
```

功能：使用返回 [Option](../../core/core_package_api/core_package_enums.md#enum-optiont) 策略的幂运算。

当运算出现溢出时，返回 ?[Int64](../../core/core_package_api/core_package_intrinsics.md#int64).None，否则返回运算结果。

参数：

- y: [UInt64](../../core/core_package_api/core_package_intrinsics.md#uint64) - 指数。

返回值：

- ?[Int64](../../core/core_package_api/core_package_intrinsics.md#int64) - 幂运算结果。

## interface SaturatingOp\<T>

```cangjie
public interface SaturatingOp<T>
```

功能：当整数运算出现溢出，饱和处理。

扩展：

- [extend Int16 <: SaturatingOp\<Int16>](overflow_package_extends.md#extend-int16--saturatingopint16)
- [extend Int32 <: SaturatingOp\<Int32>](overflow_package_extends.md#extend-int32--saturatingopint32)
- [extend Int64 <: SaturatingOp\<Int64> & SaturatingPow](overflow_package_extends.md#extend-int64--saturatingopint64--saturatingpow)
- [extend Int8 <: SaturatingOp\<Int8>](overflow_package_extends.md#extend-int8--saturatingopint8)
- [extend IntNative <: SaturatingOp\<IntNative>](overflow_package_extends.md#extend-intnative--saturatingopintnative)
- [extend UInt16 <: SaturatingOp\<UInt16>](overflow_package_extends.md#extend-uint16--saturatingopuint16)
- [extend UInt32 <: SaturatingOp\<UInt32>](overflow_package_extends.md#extend-uint32--saturatingopuint32)
- [extend UInt64 <: SaturatingOp\<UInt64>](overflow_package_extends.md#extend-uint64--saturatingopuint64)
- [extend UInt8 <: SaturatingOp\<UInt8>](overflow_package_extends.md#extend-uint8--saturatingopuint8)
- [extend UIntNative <: SaturatingOp\<UIntNative>](overflow_package_extends.md#extend-uintnative--saturatingopuintnative)

### func saturatingAdd(T)

```cangjie
func saturatingAdd(y: T): T
```

功能：使用饱和策略的加法运算。

当运算出现上溢时，返回操作数类型的最大值，运算出现下溢时，返回操作数类型的最小值，否则返回运算结果。

参数：

- y: T - 加数。

返回值：

- T - 加法运算结果。

### func saturatingDec()

```cangjie
func saturatingDec(): T
```

功能：使用饱和策略的自减运算。

当运算出现下溢时，返回操作数类型的最小值，否则返回运算结果。

返回值：

- T - 自减运算结果。

### func saturatingDiv(T)

```cangjie
func saturatingDiv(y: T): T
```

功能：使用饱和策略的除法运算。

当运算出现上溢时，返回操作数类型的最大值，否则返回运算结果。

参数：

- y: T - 除数。

返回值：

- T - 除法运算结果。

### func saturatingInc()

```cangjie
func saturatingInc(): T
```

功能：使用饱和策略的自增运算。

当运算出现上溢时，返回操作数类型的最大值，否则返回运算结果。

返回值：

- T - 自增运算结果。

### func saturatingMod(T)

```cangjie
func saturatingMod(y: T): T
```

功能：使用饱和策略的取余运算。

当运算出现上溢时，返回操作数类型的最大值，否则返回运算结果。

参数：

- y: T - 除数。

返回值：

- T - 取余运算结果。

### func saturatingMul(T)

```cangjie
func saturatingMul(y: T): T
```

功能：使用饱和策略的乘法运算。

当运算出现上溢时，返回操作数类型的最大值，运算出现下溢时，返回操作数类型的最小值，否则返回运算结果。

参数：

- y: T - 乘数。

返回值：

- T - 乘法运算结果。

### func saturatingNeg()

```cangjie
func saturatingNeg(): T
```

功能：使用饱和策略的负号运算。

当运算出现上溢时，返回操作数类型的最大值，运算出现下溢时，返回操作数类型的最小值，否则返回运算结果。

返回值：

- T - 负号运算结果。

### func saturatingShl(UInt64)

```cangjie
func saturatingShl(y: UInt64): T
```

功能：使用饱和策略的左移运算。

当移位位数大于等于操作数位数时，将移位位数置为操作数位数 - 1，返回运算结果。

参数：

- y: [UInt64](../../core/core_package_api/core_package_intrinsics.md#uint64) - 移位位数。

返回值：

- T - 左移运算结果。

### func saturatingShr(UInt64)

```cangjie
func saturatingShr(y: UInt64): T
```

功能：使用饱和策略的右移运算。

当移位位数大于等于操作数位数时，将移位位数置为操作数位数 - 1，返回运算结果。

参数：

- y: [UInt64](../../core/core_package_api/core_package_intrinsics.md#uint64) - 移位位数。

返回值：

- T - 右移运算结果。

### func saturatingSub(T)

```cangjie
func saturatingSub(y: T): T
```

功能：使用饱和策略的减法运算。

当运算出现上溢时，返回操作数类型的最大值，运算出现下溢时，返回操作数类型的最小值，否则返回运算结果。

参数：

- y: T - 减数。

返回值：

- T - 减法运算结果。

## interface SaturatingPow

```cangjie
public interface SaturatingPow
```

功能：提供饱和策略的幂运算接口。

扩展：

- [extend Int64 <: SaturatingOp\<Int64> & SaturatingPow](overflow_package_extends.md#extend-int64--saturatingopint64--saturatingpow)

### func saturatingPow(UInt64)

```cangjie
func saturatingPow(y: UInt64): Int64
```

功能：使用饱和策略的幂运算。

当运算出现上溢时，返回操作数类型的最大值，运算出现下溢时，返回操作数类型的最小值，否则返回运算结果。

参数：

- y: [UInt64](../../core/core_package_api/core_package_intrinsics.md#uint64) - 指数。

返回值：

- [Int64](../../core/core_package_api/core_package_intrinsics.md#int64) - 幂运算结果。

## interface ThrowingOp\<T>

```cangjie
public interface ThrowingOp<T>
```

功能：当整数运算出现溢出，抛出异常。

扩展：

- [extend Int16 <: ThrowingOp\<Int16>](overflow_package_extends.md#extend-int16--throwingopint16)
- [extend Int32 <: ThrowingOp\<Int32>](overflow_package_extends.md#extend-int32--throwingopint32)
- [extend Int64 <: ThrowingOp\<Int64> & ThrowingPow](overflow_package_extends.md#extend-int64--throwingopint64--throwingpow)
- [extend Int8 <: ThrowingOp\<Int8>](overflow_package_extends.md#extend-int8--throwingopint8)
- [extend IntNative <: ThrowingOp\<IntNative>](overflow_package_extends.md#extend-intnative--throwingopintnative)
- [extend UInt16 <: ThrowingOp\<UInt16>](overflow_package_extends.md#extend-uint16--throwingopuint16)
- [extend UInt32 <: ThrowingOp\<UInt32>](overflow_package_extends.md#extend-uint32--throwingopuint32)
- [extend UInt64 <: ThrowingOp\<UInt64>](overflow_package_extends.md#extend-uint64--throwingopuint64)
- [extend UInt8 <: ThrowingOp\<UInt8>](overflow_package_extends.md#extend-uint8--throwingopuint8)
- [extend UIntNative <: ThrowingOp\<UIntNative>](overflow_package_extends.md#extend-uintnative--throwingopuintnative)

### func throwingAdd(T)

```cangjie
func throwingAdd(y: T): T
```

功能：使用抛出异常策略的加法运算。

当运算出现溢出时，抛出异常，否则返回运算结果。

参数：

- y: T - 加数。

返回值：

- T - 加法运算结果。

异常：

- [OverflowException](../../core/core_package_api/core_package_exceptions.md#class-overflowexception) - 当加法运算出现溢出时，抛出异常。

### func throwingDec()

```cangjie
func throwingDec(): T
```

功能：使用抛出异常策略的自减运算。

当运算出现溢出时，抛出异常，否则返回运算结果。

返回值：

- T - 自减运算结果。

异常：

- [OverflowException](../../core/core_package_api/core_package_exceptions.md#class-overflowexception) - 当自减运算出现溢出时，抛出异常。

### func throwingDiv(T)

```cangjie
func throwingDiv(y: T): T
```

功能：使用抛出异常策略的除法运算。

当运算出现溢出时，抛出异常，否则返回运算结果。

参数：

- y: T - 除数。

返回值：

- T - 除法运算结果。

异常：

- [OverflowException](../../core/core_package_api/core_package_exceptions.md#class-overflowexception) - 当除法运算出现溢出时，抛出异常。

### func throwingInc()

```cangjie
func throwingInc(): T
```

功能：使用抛出异常策略的自增运算。

当运算出现溢出时，抛出异常，否则返回运算结果。

返回值：

- T - 自增运算结果。

异常：

- [OverflowException](../../core/core_package_api/core_package_exceptions.md#class-overflowexception) - 当自增运算出现溢出时，抛出异常。

### func throwingMod(T)

```cangjie
func throwingMod(y: T): T
```

功能：使用抛出异常策略的取余运算。

当运算出现溢出时，抛出异常，否则返回运算结果。

参数：

- y: T - 除数。

返回值：

- T - 取余运算结果。

异常：

- [OverflowException](../../core/core_package_api/core_package_exceptions.md#class-overflowexception) - 当取余运算出现溢出时，抛出异常。

### func throwingMul(T)

```cangjie
func throwingMul(y: T): T
```

功能：使用抛出异常策略的乘法运算。

当运算出现溢出时，抛出异常，否则返回运算结果。

参数：

- y: T - 乘数。

返回值：

- T - 乘法运算结果。

异常：

- [OverflowException](../../core/core_package_api/core_package_exceptions.md#class-overflowexception) - 当乘法运算出现溢出时，抛出异常。

### func throwingNeg()

```cangjie
func throwingNeg(): T
```

功能：使用抛出异常策略的负号运算。

当运算出现溢出时，抛出异常，否则返回运算结果。

返回值：

- T - 负号运算结果。

异常：

- [OverflowException](../../core/core_package_api/core_package_exceptions.md#class-overflowexception) - 当负号运算出现溢出时，抛出异常。

### func throwingShl(UInt64)

```cangjie
func throwingShl(y: UInt64): T
```

功能：使用抛出异常策略的左移运算。

当移位位数大于等于操作数位数时，抛出异常，否则返回运算结果。

参数：

- y: [UInt64](../../core/core_package_api/core_package_intrinsics.md#uint64) - 移位位数。

返回值：

- T - 左移运算结果。

异常：

- [OvershiftException](overflow_package_exceptions.md#class-overshiftexception) - 当移位位数大于等于操作数位数时，抛出异常。

### func throwingShr(UInt64)

```cangjie
func throwingShr(y: UInt64): T
```

功能：右移运算。

当移位位数大于等于操作数位数时，抛出异常，否则返回运算结果。

参数：

- y: [UInt64](../../core/core_package_api/core_package_intrinsics.md#uint64) - 移位位数。

返回值：

- T - 右移运算结果。

异常：

- [OvershiftException](overflow_package_exceptions.md#class-overshiftexception) - 当移位位数大于等于操作数位数时，抛出异常。

### func throwingSub(T)

```cangjie
func throwingSub(y: T): T
```

功能：使用抛出异常策略的减法运算。

当运算出现溢出时，抛出异常，否则返回运算结果。

参数：

- y: T - 减数。

返回值：

- T - 减法运算结果。

异常：

- [OverflowException](../../core/core_package_api/core_package_exceptions.md#class-overflowexception) - 当减法运算出现溢出时，抛出异常。

## interface ThrowingPow

```cangjie
public interface ThrowingPow
```

功能：提供使用抛出异常策略的幂运算接口。

扩展：

- [extend Int64 <: ThrowingOp\<Int64> & ThrowingPow](overflow_package_extends.md#extend-int64--throwingopint64--throwingpow)

### func throwingPow(UInt64)

```cangjie
func throwingPow(y: UInt64): Int64
```

功能：使用抛出异常策略的幂运算。

当运算出现溢出时，抛出异常，否则返回运算结果。

参数：

- y: [UInt64](../../core/core_package_api/core_package_intrinsics.md#uint64) - 指数。

返回值：

- [Int64](../../core/core_package_api/core_package_intrinsics.md#int64) - 幂运算结果。

异常：

- [OverflowException](../../core/core_package_api/core_package_exceptions.md#class-overflowexception) - 当幂运算出现溢出时，抛出异常。

## interface WrappingOp\<T>

```cangjie
public interface WrappingOp<T>
```

功能：当整数运算出现溢出，高位截断。

扩展：

- [extend Int16 <: WrappingOp\<Int16>](overflow_package_extends.md#extend-int16--wrappingopint16)
- [extend Int32 <: WrappingOp\<Int32>](overflow_package_extends.md#extend-int32--wrappingopint32)
- [extend Int64 <: WrappingOp\<Int64> & WrappingPow](overflow_package_extends.md#extend-int64--wrappingopint64--wrappingpow)
- [extend Int8 <: WrappingOp\<Int8>](overflow_package_extends.md#extend-int8--wrappingopint8)
- [extend IntNative <: WrappingOp\<IntNative>](overflow_package_extends.md#extend-intnative--wrappingopintnative)
- [extend UInt16 <: WrappingOp\<UInt16>](overflow_package_extends.md#extend-uint16--wrappingopuint16)
- [extend UInt32 <: WrappingOp\<UInt32>](overflow_package_extends.md#extend-uint32--wrappingopuint32)
- [extend UInt64 <: WrappingOp\<UInt64>](overflow_package_extends.md#extend-uint64--wrappingopuint64)
- [extend UInt8 <: WrappingOp\<UInt8>](overflow_package_extends.md#extend-uint8--wrappingopuint8)
- [extend UIntNative <: WrappingOp\<UIntNative>](overflow_package_extends.md#extend-uintnative--wrappingopuintnative)

### func wrappingAdd(T)

```cangjie
func wrappingAdd(y: T): T
```

功能：使用高位截断策略的加法运算。

当运算出现溢出时，高位截断，否则返回运算结果。

参数：

- y: T - 加数。

返回值：

- T - 加法运算结果。

### func wrappingDec()

```cangjie
func wrappingDec(): T
```

功能：使用高位截断策略的自减运算。

当运算出现溢出时，高位截断，否则返回运算结果。

返回值：

- T - 自减运算结果。

### func wrappingDiv(T)

```cangjie
func wrappingDiv(y: T): T
```

功能：使用高位截断策略的除法运算。

当运算出现溢出时，高位截断，否则返回运算结果。

参数：

- y: T - 除数。

返回值：

- T - 除法运算结果。

### func wrappingInc()

```cangjie
func wrappingInc(): T
```

功能：使用高位截断策略的自增运算。

当运算出现溢出时，高位截断，否则返回运算结果。

返回值：

- T - 自增运算结果。

### func wrappingMod(T)

```cangjie
func wrappingMod(y: T): T
```

功能：使用高位截断策略的取余运算。

当运算出现溢出时，高位截断，否则返回运算结果。

参数：

- y: T - 除数。

返回值：

- T - 取余运算结果。

### func wrappingMul(T)

```cangjie
func wrappingMul(y: T): T
```

功能：使用高位截断策略的乘法运算。

当运算出现溢出时，高位截断，否则返回运算结果。

参数：

- y: T - 乘数。

返回值：

- T - 乘法运算结果。

### func wrappingNeg()

```cangjie
func wrappingNeg(): T
```

功能：使用高位截断策略的负号运算。

当运算出现溢出时，高位截断，否则返回运算结果。

返回值：

- T - 负号运算结果。

### func wrappingShl(UInt64)

```cangjie
func wrappingShl(y: UInt64): T
```

功能：使用高位截断策略的左移运算。

当移位位数大于等于操作数位数时，高位截断。例如，对 [Int8](../../core/core_package_api/core_package_intrinsics.md#int8) 类型的数进行移位，当 y (移位位数)超大于等于 8 时，仅取 y 的低 3 位作为移位位数，以此保证移位位数在 0 到 7 之间。

参数：

- y: [UInt64](../../core/core_package_api/core_package_intrinsics.md#uint64) - 移位位数。

返回值：

- T - 左移运算结果。

### func wrappingShr(UInt64)

```cangjie
func wrappingShr(y: UInt64): T
```

功能：使用高位截断策略的右移运算。

当移位位数大于等于操作数位数时，高位截断。例如，对 [Int8](../../core/core_package_api/core_package_intrinsics.md#int8) 类型的数进行移位，当 y (移位位数)超大于等于 8 时，仅取 y 的低 3 位作为移位位数，以此保证移位位数在 0 到 7 之间。

参数：

- y: [UInt64](../../core/core_package_api/core_package_intrinsics.md#uint64) - 移位位数。

返回值：

- T - 右移运算结果。

### func wrappingSub(T)

```cangjie
func wrappingSub(y: T): T
```

功能：使用高位截断策略的减法运算。

当运算出现溢出时，高位截断，否则返回运算结果。

参数：

- y: T - 减数。

返回值：

- T - 减法运算结果。

## interface WrappingPow

```cangjie
public interface WrappingPow
```

功能：提供使用高位截断策略的幂运算接口。

扩展：

- [extend Int64 <: WrappingOp\<Int64> & WrappingPow](overflow_package_extends.md#extend-int64--wrappingopint64--wrappingpow)

### func wrappingPow(UInt64)

```cangjie
func wrappingPow(y: UInt64): Int64
```

功能：使用高位截断策略的幂运算。

当运算出现溢出时，高位截断，否则返回运算结果。

参数：

- y: [UInt64](../../core/core_package_api/core_package_intrinsics.md#uint64) - 指数。

返回值：

- [Int64](../../core/core_package_api/core_package_intrinsics.md#int64) - 幂运算结果。
