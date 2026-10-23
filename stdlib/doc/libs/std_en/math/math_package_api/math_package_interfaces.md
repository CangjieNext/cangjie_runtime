# Interface

## interface FloatingPoint\<T>

```cangjie
public interface FloatingPoint<T> <: Number<T>
```

Function: This interface provides methods related to floating-point numbers.

Parent Types:

- [Number](#interface-numbert)\<T>

Extensions:

- [extend Float16 <: FloatingPoint\<Float16>](math_package_extends.md#extend-float16--floatingpointfloat16) (cross-package extension)
- [extend Float32 <: FloatingPoint\<Float32>](math_package_extends.md#extend-float32--floatingpointfloat32) (cross-package extension)
- [extend Float64 <: FloatingPoint\<Float64>](math_package_extends.md#extend-float64--floatingpointfloat64) (cross-package extension)

### static func getE()

```cangjie
static func getE(): T
```

Function: Gets the natural constant of type T.

Returns:

- T - The natural constant of type T.

### static func getInf()

```cangjie
static func getInf(): T
```

Function: Gets the infinity value of floating-point numbers.

Returns:

- T - The infinity value of type T.

### static func getPI()

```cangjie
static func getPI(): T
```

Function: Gets the pi constant of type T.

Returns:

- T - The pi constant of type T.

### static func getMinDenormal()

```cangjie
static func getMinDenormal(): T
```

Function: Gets the minimum denormal number of single-precision floating-point numbers.

Returns:

- T - The minimum denormal number of type T.

### static func getMinNormal()

```cangjie
static func getMinNormal(): T
```

Function: Gets the minimum normal number of single-precision floating-point numbers.

Returns:

- T - The minimum normal number of type T.

### static func getNaN()

```cangjie
static func getNaN(): T
```

Function: Gets the Not-a-Number (NaN) value of floating-point numbers.

Returns:

- T - The NaN value of type T.

### func isInf()

```cangjie
func isInf(): Bool
```

Function: Determines whether the floating-point number is infinite.

Returns:

- [Bool](../../core/core_package_api/core_package_intrinsics.md#bool) - Returns `true` if the floating-point value is positive or negative infinity; otherwise, returns `false`.

### func isNaN()

```cangjie
func isNaN(): Bool
```

Function: Determines whether the floating-point number is Not-a-Number (NaN).

Returns:

- [Bool](../../core/core_package_api/core_package_intrinsics.md#bool) - Returns `true` if the floating-point value is NaN; otherwise, returns `false`.

### func isNormal()

```cangjie
func isNormal(): Bool
```

Function: Determines whether the floating-point number is normal.

Returns:

- [Bool](../../core/core_package_api/core_package_intrinsics.md#bool) - Returns `true` if the floating-point number is normal; otherwise, returns `false`.

## interface Integer\<T>

```cangjie
public interface Integer<T> <: Number<T>
```

Function: This interface provides methods related to integer types.

Parent Types:

- [Number\<T>](#interface-numbert)### static func isSigned()

```cangjie
static func isSigned(): Bool
```

Function: Determines whether the type is signed.

Returns:

- [Bool](../../core/core_package_api/core_package_intrinsics.md#bool) - Returns `true` if the type is signed; otherwise returns `false`.

Extensions:

- [extend Int16 <: Integer\<Int16>](math_package_extends.md#extend-int16--integerint16) (cross-package extension)
- [extend Int32 <: Integer\<Int32>](math_package_extends.md#extend-int32--integerint32) (cross-package extension)
- [extend Int64 <: Integer\<Int64>](math_package_extends.md#extend-int64--integerint64) (cross-package extension)
- [extend Int8 <: Integer\<Int8>](math_package_extends.md#extend-int8--integerint8) (cross-package extension)
- [extend IntNative <: Integer\<IntNative>](math_package_extends.md#extend-intnative--integerintnative) (cross-package extension)
- [extend UInt16 <: Integer\<UInt16>](math_package_extends.md#extend-uint16--integeruint16) (cross-package extension)
- [extend UInt32 <: Integer\<UInt32>](math_package_extends.md#extend-uint32--integeruint32) (cross-package extension)
- [extend UInt64 <: Integer\<UInt64>](math_package_extends.md#extend-uint64--integeruint64) (cross-package extension)
- [extend UInt8 <: Integer\<UInt8>](math_package_extends.md#extend-uint8--integeruint8) (cross-package extension)
- [extend UIntNative <: Integer\<UIntNative>](math_package_extends.md#extend-uintnative--integeruintnative) (cross-package extension)

### operator func %(T)

```cangjie
operator func %(rhs: T): T
```

Function: Arithmetic operator that calculates the remainder.

Parameters:

- rhs: T - The right-hand side operand, representing the divisor.

Returns:

- T - The calculated remainder.

### operator func &(T)

```cangjie
operator func &(rhs: T): T
```

Function: Bitwise AND operator.

Parameters:

- rhs: T - The right-hand side operand.

Returns:

- T - The computed result.

### operator func |(T)

```cangjie
operator func |(rhs: T): T
```

Function: Bitwise OR operator.

Parameters:

- rhs: T - The right-hand side operand.

Returns:

- T - The computed result.

### operator func ^(T)

```cangjie
operator func ^(rhs: T): T
```

Function: Bitwise XOR operator.

Parameters:

- rhs: T - The right-hand side operand.

Returns:

- T - The computed result.

### operator func !()

```cangjie
operator func !(): T
```

Function: Bitwise NOT operator.

Returns:

- T - The computed result.

### operator func >>(Int64)

```cangjie
operator func >>(n: Int64): T
```

Function: Bitwise right shift operator.

Parameters:

- n: [Int64](../../core/core_package_api/core_package_intrinsics.md#int64) - The right-hand side operand, representing the number of bits to shift right.

Returns:

- T - The computed result.

### operator func <<(Int64)

```cangjie
operator func <<(n: Int64): T
```

Function: Bitwise left shift operator.

Parameters:

- n: [Int64](../../core/core_package_api/core_package_intrinsics.md#int64) - The right-hand side operand, representing the number of bits to shift left.

Returns:

- T - The computed result.

## interface MathExtension\<T> <sup>(deprecated)</sup>

```cangjie
public interface MathExtension<T>
```

Function: Provides unified methods to obtain mathematical constants.

> **Note:**
>
> This interface will be deprecated in future versions. Use [FloatingPoint\<T>](#interface-floatingpointt) instead.

Extensions:

- [extend Float16 <: MathExtension\<Float16>](math_package_extends.md#extend-float16--mathextensionfloat16) (cross-package extension)
- [extend Float32 <: MathExtension\<Float32>](math_package_extends.md#extend-float32--mathextensionfloat32) (cross-package extension)
- [extend Float64 <: MathExtension\<Float64>](math_package_extends.md#extend-float64--mathextensionfloat64) (cross-package extension)

### static func GetPI()

```cangjie
static func GetPI(): T
```

Function: Retrieves the π constant for type T.

Returns:

- T - The π constant of type T.

### static func GetE()

```cangjie
static func GetE(): T
```

Function: Retrieves the natural constant e for type T.

Returns:

- T - The natural constant e of type T.

## interface MaxMinValue\<T>

```cangjie
public interface MaxMinValue<T> {
    static func getMax(): T
    static func getMin(): T
}
```

Function: Provides methods to get maximum and minimum values.

Extensions:

- [extend Float16 <: MaxMinValue\<Float16>](math_package_extends.md#extend-float16--maxminvaluefloat16) (cross-package extension)
- [extend Float32 <: MaxMinValue\<Float32>](math_package_extends.md#extend-float32--maxminvaluefloat32) (cross-package extension)
- [extend Float64 <: MaxMinValue\<Float64>](math_package_extends.md#extend-float64--maxminvaluefloat64) (cross-package extension)
- [extend Int16 <: MaxMinValue\<Int16>](math_package_extends.md#extend-int16--maxminvalueint16) (cross-package extension)
- [extend Int32 <: MaxMinValue\<Int32>](math_package_extends.md#extend-int32--maxminvalueint32) (cross-package extension)
- [extend Int64 <: MaxMinValue\<Int64>](math_package_extends.md#extend-int64--maxminvalueint64) (cross-package extension)
- [extend Int8 <: MaxMinValue\<Int8>](math_package_extends.md#extend-int8--maxminvalueint8) (cross-package extension)
- [extend IntNative <: MaxMinValue\<IntNative>](math_package_extends.md#extend-intnative--maxminvalueintnative) (cross-package extension)
- [extend UInt16 <: MaxMinValue\<UInt16>](math_package_extends.md#extend-uint16--maxminvalueuint16) (cross-package extension)
- [extend UInt32 <: MaxMinValue\<UInt32>](math_package_extends.md#extend-uint32--maxminvalueuint32) (cross-package extension)
- [extend UInt64 <: MaxMinValue\<UInt64>](math_package_extends.md#extend-uint64--maxminvalueuint64) (cross-package extension)
- [extend UInt8 <: MaxMinValue\<UInt8>](math_package_extends.md#extend-uint8--maxminvalueuint8) (cross-package extension)
- [extend UIntNative <: MaxMinValue\<UIntNative>](math_package_extends.md#extend-uintnative--maxminvalueuintnative) (cross-package extension)

### static func getMax()

```cangjie
static func getMax(): T
```

Function: Gets the maximum value.

Returns:

- T - The maximum value.

### static func getMin()

```cangjie
static func getMin(): T
```

Function: Gets the minimum value.

Returns:

- T - The minimum value.

## interface Number\<T>

```cangjie
public interface Number<T>
```

Function: Provides methods related to numeric types.

Extensions:

- [extend Float16 <: Number\<Float16>](math_package_extends.md#extend-float16--numberfloat16) (cross-package extension)
- [extend Float32 <: Number\<Float32>](math_package_extends.md#extend-float32--numberfloat32) (cross-package extension)
- [extend Float64 <: Number\<Float64>](math_package_extends.md#extend-float64--numberfloat64) (cross-package extension)
- [extend Int16 <: Number\<Int16>](math_package_extends.md#extend-int16--numberint16) (cross-package extension)
- [extend Int32 <: Number\<Int32>](math_package_extends.md#extend-int32--numberint32) (cross-package extension)
- [extend Int64 <: Number\<Int64>](math_package_extends.md#extend-int64--numberint64) (cross-package extension)
- [extend Int8 <: Number\<Int8>](math_package_extends.md#extend-int8--numberint8) (cross-package extension)
- [extend IntNative <: Number\<IntNative>](math_package_extends.md#extend-intnative--numberintnative) (cross-package extension)
- [extend UInt16 <: Number\<UInt16>](math_package_extends.md#extend-uint16--numberuint16) (cross-package extension)
- [extend UInt32 <: Number\<UInt32>](math_package_extends.md#extend-uint32--numberuint32) (cross-package extension)
- [extend UInt64 <: Number\<UInt64>](math_package_extends.md#extend-uint64--numberuint64) (cross-package extension)
- [extend UInt8 <: Number\<UInt8>](math_package_extends.md#extend-uint8--numberuint8) (cross-package extension)
- [extend UIntNative <: Number\<UIntNative>](math_package_extends.md#extend-uintnative--numberuintnative) (cross-package extension)

### operator func +(T)

```cangjie
operator func +(rhs: T): T
```

Function: Arithmetic operator for addition.

Parameters:

- rhs: T - The right-hand side operand, representing another addend.

Returns:

- T - The computed sum.

### operator func -(T)

```cangjie
operator func -(rhs: T): T
```

Function: Arithmetic operator for subtraction.

Parameters:

- rhs: T - The right-hand side operand, representing the subtrahend.

Returns:

- T - The computed difference.

### operator func *(T)

```cangjie
operator func *(rhs: T): T
```

Function: Arithmetic operator for multiplication.

Parameters:

- rhs: T - The right-hand side operand, representing another multiplier.

Returns:

- T - The computed product.

### operator func /(T)

```cangjie
operator func /(rhs: T): T
```

Function: Arithmetic operator for division.

Parameters:

- rhs: T - The right-hand side operand, representing the divisor.

Returns:

- T - The computed quotient.

### operator func -()

```cangjie
operator func -(): T
```

Function: Arithmetic operator for negation.

Returns:

- T - The negated value.

