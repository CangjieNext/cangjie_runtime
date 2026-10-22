# 接口

## interface FloatingPoint\<T>

```cangjie
public interface FloatingPoint<T> <: Number<T>
```

功能：本接口提供了浮点数相关的方法。

父类型：

- [Number](#interface-numbert)\<T>

扩展：

- [extend Float16 <: FloatingPoint\<Float16>](math_package_extends.md#extend-float16--floatingpointfloat16)
- [extend Float32 <: FloatingPoint\<Float32>](math_package_extends.md#extend-float32--floatingpointfloat32)
- [extend Float64 <: FloatingPoint\<Float64>](math_package_extends.md#extend-float64--floatingpointfloat64)

### static func getE()

```cangjie
static func getE(): T
```

功能：获取 T 类型的自然常数。

返回值：

- T - 类型 T 的自然常数。

### static func getInf()

```cangjie
static func getInf(): T
```

功能：获取浮点数的无穷数。

返回值：

- T - 类型 T 的无穷数。

### static func getMinDenormal()

```cangjie
static func getMinDenormal(): T
```

功能：获取单精度浮点数的最小次正规数。

返回值：

- T - 类型 T 的最小次正规数。

### static func getMinNormal()

```cangjie
static func getMinNormal(): T
```

功能：获取单精度浮点数的最小正规数。

返回值：

- T - 类型 T 的最小正规数。

### static func getNaN()

```cangjie
static func getNaN(): T
```

功能：获取浮点数的非数。

返回值：

- T - 类型 T 的非数。

### static func getPI()

```cangjie
static func getPI(): T
```

功能：获取 T 类型的圆周率常数。

返回值：

- T - 类型 T 的圆周率常数。

### func isInf()

```cangjie
func isInf(): Bool
```

功能：判断浮点数是否为无穷数值。

返回值：

- [Bool](../../core/core_package_api/core_package_intrinsics.md#bool) - 如果浮点数的值正无穷大或负无穷大，则返回 `true`；否则，返回 `false`。

### func isNaN()

```cangjie
func isNaN(): Bool
```

功能：判断浮点数是否为非数值。

返回值：

- [Bool](../../core/core_package_api/core_package_intrinsics.md#bool) - 如果浮点数的值为非数值，则返回 `true`；否则，返回 `false`。

### func isNormal()

```cangjie
func isNormal(): Bool
```

功能：判断浮点数是否为常规数值。

返回值：

- [Bool](../../core/core_package_api/core_package_intrinsics.md#bool) - 如果是正常的浮点数，返回 `true`；否则，返回 `false`。

## interface Integer\<T>

```cangjie
public interface Integer<T> <: Number<T>
```

功能：本接口提供了整数类型相关的方法。

父类型：

- [Number\<T>](#interface-numbert)

扩展：

- [extend Int16 <: Integer\<Int16>](math_package_extends.md#extend-int16--integerint16)
- [extend Int32 <: Integer\<Int32>](math_package_extends.md#extend-int32--integerint32)
- [extend Int64 <: Integer\<Int64>](math_package_extends.md#extend-int64--integerint64)
- [extend Int8 <: Integer\<Int8>](math_package_extends.md#extend-int8--integerint8)
- [extend IntNative <: Integer\<IntNative>](math_package_extends.md#extend-intnative--integerintnative)
- [extend UInt16 <: Integer\<UInt16>](math_package_extends.md#extend-uint16--integeruint16)
- [extend UInt32 <: Integer\<UInt32>](math_package_extends.md#extend-uint32--integeruint32)
- [extend UInt64 <: Integer\<UInt64>](math_package_extends.md#extend-uint64--integeruint64)
- [extend UInt8 <: Integer\<UInt8>](math_package_extends.md#extend-uint8--integeruint8)
- [extend UIntNative <: Integer\<UIntNative>](math_package_extends.md#extend-uintnative--integeruintnative)
- [extend BigInt <: Integer\<BigInt>](../../math_numeric/math_numeric_package_api/math_numeric_package_extends.md#extend-bigint--integerbigint)

### static func isSigned()

```cangjie
static func isSigned(): Bool
```

功能：判断类型是否是有符号的。

返回值：

- [Bool](../../core/core_package_api/core_package_intrinsics.md#bool) - 如果类型是有符号的，返回 `true`；否则返回 `false`。

### operator func !()

```cangjie
operator func !(): T
```

功能：位运算符，按位取反。

返回值：

- T - 计算所得结果。

### operator func %(T)

```cangjie
operator func %(other: T): T
```

功能：算术运算符，计算余数。

参数：

- other: T - 运算符右边的数，表示除数。

返回值：

- T - 计算所得余数。

### operator func &(T)

```cangjie
operator func &(other: T): T
```

功能：位运算符，按位与。

参数：

- other: T - 运算符右边的数。

返回值：

- T - 计算所得结果。

### operator func <<(Int64)

```cangjie
operator func <<(n: Int64): T
```

功能：位运算符，按位左移。

参数：

- n: [Int64](../../core/core_package_api/core_package_intrinsics.md#int64) - 运算符右边的数，表示左移的位数。

返回值：

- T - 计算所得结果。

### operator func >>(Int64)

```cangjie
operator func >>(n: Int64): T
```

功能：位运算符，按位右移。

参数：

- n: [Int64](../../core/core_package_api/core_package_intrinsics.md#int64) - 运算符右边的数，表示右移的位数。

返回值：

- T - 计算所得结果。

### operator func ^(T)

```cangjie
operator func ^(other: T): T
```

功能：位运算符，按位异或。

参数：

- other: T - 运算符右边的数。

返回值：

- T - 计算所得结果。

### operator func |(T)

```cangjie
operator func |(other: T): T
```

功能：位运算符，按位或。

参数：

- other: T - 运算符右边的数。

返回值：

- T - 计算所得结果。

## interface MathExtension\<T> <sup>(deprecated)</sup>

```cangjie
public interface MathExtension<T>
```

功能：本接口提供了统一的方法获取一些数学常数。

> **注意：**
>
> 未来版本即将废弃，使用 [FloatingPoint\<T>](#interface-floatingpointt) 替代。

扩展：

- [extend Float16 <: MathExtension\<Float16>](math_package_extends.md#extend-float16--mathextensionfloat16)
- [extend Float32 <: MathExtension\<Float32>](math_package_extends.md#extend-float32--mathextensionfloat32)
- [extend Float64 <: MathExtension\<Float64>](math_package_extends.md#extend-float64--mathextensionfloat64)

### static func GetE()

```cangjie
static func GetE(): T
```

功能：获取 T 类型的自然常数。

返回值：

- T - 类型 T 的自然常数。

### static func GetPI()

```cangjie
static func GetPI(): T
```

功能：获取 T 类型的圆周率常数。

返回值：

- T - 类型 T 的圆周率常数。

## interface MaxMinValue\<T>

```cangjie
public interface MaxMinValue<T>
```

功能：提供获取最大值和最小值的方法。

扩展：

- [extend Float16 <: MaxMinValue\<Float16>](math_package_extends.md#extend-float16--maxminvaluefloat16)
- [extend Float32 <: MaxMinValue\<Float32>](math_package_extends.md#extend-float32--maxminvaluefloat32)
- [extend Float64 <: MaxMinValue\<Float64>](math_package_extends.md#extend-float64--maxminvaluefloat64)
- [extend Int16 <: MaxMinValue\<Int16>](math_package_extends.md#extend-int16--maxminvalueint16)
- [extend Int32 <: MaxMinValue\<Int32>](math_package_extends.md#extend-int32--maxminvalueint32)
- [extend Int64 <: MaxMinValue\<Int64>](math_package_extends.md#extend-int64--maxminvalueint64)
- [extend Int8 <: MaxMinValue\<Int8>](math_package_extends.md#extend-int8--maxminvalueint8)
- [extend IntNative <: MaxMinValue\<IntNative>](math_package_extends.md#extend-intnative--maxminvalueintnative)
- [extend UInt16 <: MaxMinValue\<UInt16>](math_package_extends.md#extend-uint16--maxminvalueuint16)
- [extend UInt32 <: MaxMinValue\<UInt32>](math_package_extends.md#extend-uint32--maxminvalueuint32)
- [extend UInt64 <: MaxMinValue\<UInt64>](math_package_extends.md#extend-uint64--maxminvalueuint64)
- [extend UInt8 <: MaxMinValue\<UInt8>](math_package_extends.md#extend-uint8--maxminvalueuint8)
- [extend UIntNative <: MaxMinValue\<UIntNative>](math_package_extends.md#extend-uintnative--maxminvalueuintnative)

### static func getMax()

```cangjie
static func getMax(): T
```

功能：获取最大值。

返回值：

- T - 最大值。

### static func getMin()

```cangjie
static func getMin(): T
```

功能：获取最小值。

返回值：

- T - 最小值。

## interface Number\<T>

```cangjie
public interface Number<T>
```

功能：提供数值类型相关的方法。

扩展：

- [extend Float16 <: Number\<Float16>](math_package_extends.md#extend-float16--numberfloat16)
- [extend Float32 <: Number\<Float32>](math_package_extends.md#extend-float32--numberfloat32)
- [extend Float64 <: Number\<Float64>](math_package_extends.md#extend-float64--numberfloat64)
- [extend Int16 <: Number\<Int16>](math_package_extends.md#extend-int16--numberint16)
- [extend Int32 <: Number\<Int32>](math_package_extends.md#extend-int32--numberint32)
- [extend Int64 <: Number\<Int64>](math_package_extends.md#extend-int64--numberint64)
- [extend Int8 <: Number\<Int8>](math_package_extends.md#extend-int8--numberint8)
- [extend IntNative <: Number\<IntNative>](math_package_extends.md#extend-intnative--numberintnative)
- [extend UInt16 <: Number\<UInt16>](math_package_extends.md#extend-uint16--numberuint16)
- [extend UInt32 <: Number\<UInt32>](math_package_extends.md#extend-uint32--numberuint32)
- [extend UInt64 <: Number\<UInt64>](math_package_extends.md#extend-uint64--numberuint64)
- [extend UInt8 <: Number\<UInt8>](math_package_extends.md#extend-uint8--numberuint8)
- [extend UIntNative <: Number\<UIntNative>](math_package_extends.md#extend-uintnative--numberuintnative)
- [extend BigInt <: Number\<BigInt>](../../math_numeric/math_numeric_package_api/math_numeric_package_extends.md#extend-bigint--numberbigint)
- [extend Decimal <: Number\<Decimal>](../../math_numeric/math_numeric_package_api/math_numeric_package_extends.md#extend-decimal--numberdecimal)

### operator func *(T)

```cangjie
operator func *(other: T): T
```

功能：算术运算符，计算乘法。

参数：

- other: T - 运算符右边的数，表示另一个乘数。

返回值：

- T - 计算所得积。

### operator func +(T)

```cangjie
operator func +(other: T): T
```

功能：算术运算符，计算加法。

参数：

- other: T - 运算符右边的数，表示另一个加数。

返回值：

- T - 计算所得和。

### operator func -()

```cangjie
operator func -(): T
```

功能：算术运算符，计算取负的值。

返回值：

- T - 取负的值。

### operator func -(T)

```cangjie
operator func -(other: T): T
```

功能：算术运算符，计算减法。

参数：

- other: T - 运算符右边的数，表示减数。

返回值：

- T - 计算所得差。

### operator func /(T)

```cangjie
operator func /(other: T): T
```

功能：算术运算符，计算除法。

参数：

- other: T - 运算符右边的数，表示除数。

返回值：

- T - 计算所得商。

