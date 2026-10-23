# Interface

## interface CarryingOp\<T>

```cangjie
public interface CarryingOp<T>
```

Function: Provides an interface that returns whether integer operations resulted in truncation and their computation results.

Extensions:

- [extend Int16 <: CarryingOp\<Int16>](overflow_package_extends.md#extend-int16--carryingopint16) (cross-package extension)
- [extend Int32 <: CarryingOp\<Int32>](overflow_package_extends.md#extend-int32--carryingopint32) (cross-package extension)
- [extend Int64 <: CarryingOp\<Int64> & CarryingPow](overflow_package_extends.md#extend-int64--carryingopint64--carryingpow) (cross-package extension)
- [extend Int8 <: CarryingOp\<Int8>](overflow_package_extends.md#extend-int8--carryingopint8) (cross-package extension)
- [extend IntNative <: CarryingOp\<IntNative>](overflow_package_extends.md#extend-intnative--carryingopintnative) (cross-package extension)
- [extend UInt16 <: CarryingOp\<UInt16>](overflow_package_extends.md#extend-uint16--carryingopuint16) (cross-package extension)
- [extend UInt32 <: CarryingOp\<UInt32>](overflow_package_extends.md#extend-uint32--carryingopuint32) (cross-package extension)
- [extend UInt64 <: CarryingOp\<UInt64>](overflow_package_extends.md#extend-uint64--carryingopuint64) (cross-package extension)
- [extend UInt8 <: CarryingOp\<UInt8>](overflow_package_extends.md#extend-uint8--carryingopuint8) (cross-package extension)
- [extend UIntNative <: CarryingOp\<UIntNative>](overflow_package_extends.md#extend-uintnative--carryingopuintnative) (cross-package extension)

### func carryingAdd(T)

```cangjie
func carryingAdd(y: T): (Bool, T)
```

Function: Returns a tuple where the first element indicates whether addition operation resulted in truncation (`true` if truncated), and the second element is the computation result.

Parameters:

- y: T - The addend.

Returns:

- ([Bool](../../core/core_package_api/core_package_intrinsics.md#bool), T) - Whether addition resulted in truncation and the computation result.

### func carryingDec()

```cangjie
func carryingDec(): (Bool, T)
```

Function: Returns a tuple where the first element indicates whether decrement operation resulted in truncation (`true` if truncated), and the second element is the computation result.

Returns:

- ([Bool](../../core/core_package_api/core_package_intrinsics.md#bool), T) - Whether decrement resulted in truncation and the computation result.

### func carryingDiv(T)

```cangjie
func carryingDiv(y: T): (Bool, T)
```

Function: Returns a tuple where the first element indicates whether division operation resulted in truncation (`true` if truncated), and the second element is the computation result.

Parameters:

- y: T - The divisor.

Returns:

- ([Bool](../../core/core_package_api/core_package_intrinsics.md#bool), T) - Whether division resulted in truncation and the computation result.

### func carryingInc()

```cangjie
func carryingInc(): (Bool, T)
```

Function: Returns a tuple where the first element indicates whether increment operation resulted in truncation (`true` if truncated), and the second element is the computation result.

Returns:

- ([Bool](../../core/core_package_api/core_package_intrinsics.md#bool), T) - Whether increment resulted in truncation and the computation result.

### func carryingMod(T)

```cangjie
func carryingMod(y: T): (Bool, T)
```

Function: Returns a tuple where the first element indicates whether modulo operation resulted in truncation (`true` if truncated), and the second element is the computation result.

Parameters:

- y: T - The divisor.

Returns:

- ([Bool](../../core/core_package_api/core_package_intrinsics.md#bool), T) - Whether modulo resulted in truncation and the computation result.

### func carryingMul(T)

```cangjie
func carryingMul(y: T): (Bool, T)
```

Function: Returns a tuple where the first element indicates whether multiplication operation resulted in truncation (`true` if truncated), and the second element is the computation result.

Parameters:

- y: T - The multiplier.

Returns:

- ([Bool](../../core/core_package_api/core_package_intrinsics.md#bool), T) - Whether multiplication resulted in truncation and the computation result.

### func carryingNeg()

```cangjie
func carryingNeg(): (Bool, T)
```

Function: Returns a tuple where the first element indicates whether negation operation resulted in truncation (`true` if truncated), and the second element is the computation result.

Returns:

- ([Bool](../../core/core_package_api/core_package_intrinsics.md#bool), T) - Whether negation resulted in truncation and the computation result.

### func carryingShl(UInt64)

```cangjie
func carryingShl(y: UInt64): (Bool, T)
```

Function: Returns a tuple where the first element indicates whether left shift operation resulted in truncation (`true` if truncated), and the second element is the computation result.

Parameters:

- y: [UInt64](../../core/core_package_api/core_package_intrinsics.md#uint64) - The number of bits to shift.

Returns:

- ([Bool](../../core/core_package_api/core_package_intrinsics.md#bool), T) - Whether left shift resulted in truncation and the computation result.

### func carryingShr(UInt64)

```cangjie
func carryingShr(y: UInt64): (Bool, T)
```

Function: Returns a tuple where the first element indicates whether right shift operation resulted in truncation (`true` if truncated), and the second element is the computation result.

Parameters:

- y: [UInt64](../../core/core_package_api/core_package_intrinsics.md#uint64) - The number of bits to shift.

Returns:- ([Bool](../../core/core_package_api/core_package_intrinsics.md#bool), T) - Whether right shift operation resulted in truncation and the operation result.

### func carryingSub(T)

```cangjie
func carryingSub(y: T): (Bool, T)
```

Function: Returns a tuple where the first element indicates whether subtraction resulted in truncation (`true` if occurred), and the second element is the operation result.

Parameters:

- y: T - The subtrahend.

Returns:

- ([Bool](../../core/core_package_api/core_package_intrinsics.md#bool), T) - Whether subtraction resulted in truncation and the operation result.

## interface CarryingPow

```cangjie
public interface CarryingPow
```

Function: Provides an interface for exponentiation operations using the [wrapping](./overflow_package_interfaces.md#interface-wrappingopt) strategy.

Extensions:

- [extend Int64 <: CarryingOp\<Int64> & CarryingPow](overflow_package_extends.md#extend-int64--carryingopint64--carryingpow) (cross-package extension)

### func carryingPow(UInt64)

```cangjie
func carryingPow(y: UInt64): (Bool, Int64)
```

Function: Exponentiation operation using the [wrapping](./overflow_package_interfaces.md#interface-wrappingopt) strategy.

Returns `true` and the operation result when overflow occurs; otherwise returns `false` and the operation result.

Parameters:

- y: [UInt64](../../core/core_package_api/core_package_intrinsics.md#uint64) - Exponent.

Returns:

- ([Bool](../../core/core_package_api/core_package_intrinsics.md#bool), [Int64](../../core/core_package_api/core_package_intrinsics.md#int64)) - Returns a tuple where the first element indicates whether truncation occurred during the operation (`true` if truncation occurred), and the second element is the operation result.

- ?T - Left shift operation result.

### func checkedShr(UInt64)

```cangjie
func checkedShr(y: UInt64): ?T
```

Function: Left shift operation using the [Option](../../core/core_package_api/core_package_enums.md#enum-optiont) return strategy.

When the shift count is greater than or equal to the operand bit width, returns `?T.None`; otherwise returns the operation result.

Parameters:

- y: [UInt64](../../core/core_package_api/core_package_intrinsics.md#uint64) - Shift count.

Returns:

- ?T - Left shift operation result.

### func checkedSub(T)

```cangjie
func checkedSub(y: T): ?T
```

Function: Subtraction operation using the [Option](../../core/core_package_api/core_package_enums.md#enum-optiont) return strategy.

Returns `?T.None` when overflow occurs; otherwise returns the operation result.

Parameters:

- y: T - Subtrahend.

Returns:

- ?T - Subtraction operation result.- ?T - Left shift operation result.

### func checkedShr(UInt64)

```cangjie
func checkedShr(y: UInt64): ?T
```

Function: Right shift operation using the [Option](../../core/core_package_api/core_package_enums.md#enum-optiont) return strategy.

When the shift count is greater than or equal to the operand bit width, returns `?T.None`; otherwise returns the operation result.

Parameters:

- y: [UInt64](../../core/core_package_api/core_package_intrinsics.md#uint64) - Shift count.

Returns:

- ?T - Right shift operation result.

### func checkedSub(T)

```cangjie
func checkedSub(y: T): ?T
```

Function: Subtraction operation using the [Option](../../core/core_package_api/core_package_enums.md#enum-optiont) return strategy.

Returns `?T.None` when overflow occurs; otherwise returns the operation result.

Parameters:

- y: T - Subtrahend.

Returns:

- ?T - Subtraction operation result.

## interface CheckedPow

```cangjie
public interface CheckedPow
```

Function: Provides an exponentiation interface using the [Option](../../core/core_package_api/core_package_enums.md#enum-optiont) return strategy.

Extensions:

- [extend Int64 <: CheckedOp\<Int64> & CheckedPow](overflow_package_extends.md#extend-int64--checkedopint64--checkedpow) (cross-package extension)

### func checkedPow(UInt64)

```cangjie
func checkedPow(y: UInt64): ?Int64
```

Function: Exponentiation operation using the [Option](../../core/core_package_api/core_package_enums.md#enum-optiont) return strategy.

When overflow occurs during the operation, returns ?[Int64](../../core/core_package_api/core_package_intrinsics.md#int64).None; otherwise returns the operation result.

Parameters:

- y: [UInt64](../../core/core_package_api/core_package_intrinsics.md#uint64) - The exponent.

Returns:

- ?[Int64](../../core/core_package_api/core_package_intrinsics.md#int64) - The exponentiation result.

## interface SaturatingOp\<T>

```cangjie
public interface SaturatingOp<T>
```

Function: Performs saturation handling when integer operations overflow.

Extensions:

- [extend Int16 <: SaturatingOp\<Int16>](overflow_package_extends.md#extend-int16--saturatingopint16) (cross-package extension)
- [extend Int32 <: SaturatingOp\<Int32>](overflow_package_extends.md#extend-int32--saturatingopint32) (cross-package extension)
- [extend Int64 <: SaturatingOp\<Int64> & SaturatingPow](overflow_package_extends.md#extend-int64--saturatingopint64--saturatingpow) (cross-package extension)
- [extend Int8 <: SaturatingOp\<Int8>](overflow_package_extends.md#extend-int8--saturatingopint8) (cross-package extension)
- [extend IntNative <: SaturatingOp\<IntNative>](overflow_package_extends.md#extend-intnative--saturatingopintnative) (cross-package extension)
- [extend UInt32 <: SaturatingOp\<UInt32>](overflow_package_extends.md#extend-uint32--saturatingopuint32) (cross-package extension)
- [extend UInt64 <: SaturatingOp\<UInt64>](overflow_package_extends.md#extend-uint64--saturatingopuint64) (cross-package extension)
- [extend UInt8 <: SaturatingOp\<UInt8>](overflow_package_extends.md#extend-uint8--saturatingopuint8) (cross-package extension)
- [extend UIntNative <: SaturatingOp\<UIntNative>](overflow_package_extends.md#extend-uintnative--saturatingopuintnative) (cross-package extension)

### func saturatingAdd(T)

```cangjie
func saturatingAdd(y: T): T
```

Function: Addition operation using saturation strategy.

When overflow occurs, returns the maximum value of the operand type; when underflow occurs, returns the minimum value of the operand type; otherwise returns the operation result.

Parameters:

- y: T - The addend.

Returns:

- T - The addition operation result.

### func saturatingDec()

```cangjie
func saturatingDec(): T
```

Function: Decrement operation using saturation strategy.

When underflow occurs, returns the minimum value of the operand type; otherwise returns the operation result.

Returns:

- T - The decrement operation result.

### func saturatingDiv(T)

```cangjie
func saturatingDiv(y: T): T
```

Function: Division operation using saturation strategy.

When overflow occurs, returns the maximum value of the operand type; otherwise returns the operation result.

Parameters:

- y: T - The divisor.

Returns:

- T - The division operation result.

### func saturatingInc()

```cangjie
func saturatingInc(): T
```

Function: Increment operation using saturation strategy.

When overflow occurs, returns the maximum value of the operand type; otherwise returns the operation result.

Returns:

- T - The increment operation result.

### func saturatingMod(T)

```cangjie
func saturatingMod(y: T): T
```

Function: Modulus operation using saturation strategy.

When overflow occurs, returns the maximum value of the operand type; otherwise returns the operation result.

Parameters:

- y: T - The divisor.

Returns:

- T - The modulus operation result.

### func saturatingMul(T)

```cangjie
func saturatingMul(y: T): T
```

Function: Saturation-based multiplication operation.

When overflow occurs, returns the maximum value of the operand type; when underflow occurs, returns the minimum value of the operand type; otherwise returns the computation result.

Parameters:

- y: T - Multiplier.

Returns:

- T - Result of the multiplication operation.

### func saturatingNeg()

```cangjie
func saturatingNeg(): T
```

Function: Saturation-based negation operation.

When overflow occurs, returns the maximum value of the operand type; when underflow occurs, returns the minimum value of the operand type; otherwise returns the computation result.

Returns:

- T - Result of the negation operation.

### func saturatingShl(UInt64)

```cangjie
func saturatingShl(y: UInt64): T
```

Function: Saturation-based left shift operation.

When the shift count is greater than or equal to the bit width of the operand, sets the shift count to (operand bit width - 1) and returns the computation result.

Parameters:

- y: [UInt64](../../core/core_package_api/core_package_intrinsics.md#uint64) - Shift count.

Returns:

- T - Result of the left shift operation.

### func saturatingShr(UInt64)

```cangjie
func saturatingShr(y: UInt64): T
```

Function: Saturation-based right shift operation.

When the shift count is greater than or equal to the bit width of the operand, sets the shift count to (operand bit width - 1) and returns the computation result.

Parameters:

- y: [UInt64](../../core/core_package_api/core_package_intrinsics.md#uint64) - Shift count.

Returns:

- T - Result of the right shift operation.

### func saturatingSub(T)

```cangjie
func saturatingSub(y: T): T
```

Function: Saturation-based subtraction operation.

When overflow occurs, returns the maximum value of the operand type; when underflow occurs, returns the minimum value of the operand type; otherwise returns the computation result.

Parameters:

- y: T - Subtrahend.

Returns:

- T - Result of the subtraction operation.

## interface SaturatingPow

```cangjie
public interface SaturatingPow
```

Function: Provides power operation interface using saturation strategy.

Extensions:

- [extend Int64 <: SaturatingOp\<Int64> & SaturatingPow](overflow_package_extends.md#extend-int64--saturatingopint64--saturatingpow) (cross-package extension)

### func saturatingPow(UInt64)

```cangjie
func saturatingPow(y: UInt64): Int64
```

Function: Power operation using saturation strategy.

When overflow occurs, returns the maximum value of the operand type; when underflow occurs, returns the minimum value of the operand type; otherwise returns the operation result.

Parameters:

- y: [UInt64](../../core/core_package_api/core_package_intrinsics.md#uint64) - Exponent.

Returns:

- [Int64](../../core/core_package_api/core_package_intrinsics.md#int64) - Result of power operation.

## interface ThrowingOp\<T>

```cangjie
public interface ThrowingOp<T>
```

Function: Throws exceptions when integer operations overflow.

Extensions:

- [extend Int16 <: ThrowingOp\<Int16>](overflow_package_extends.md#extend-int16--throwingopint16) (cross-package extension)
- [extend Int32 <: ThrowingOp\<Int32>](overflow_package_extends.md#extend-int32--throwingopint32) (cross-package extension)
- [extend Int64 <: ThrowingOp\<Int64> & ThrowingPow](overflow_package_extends.md#extend-int64--throwingopint64--throwingpow) (cross-package extension)
- [extend Int8 <: ThrowingOp\<Int8>](overflow_package_extends.md#extend-int8--throwingopint8) (cross-package extension)
- [extend IntNative <: ThrowingOp\<IntNative>](overflow_package_extends.md#extend-intnative--throwingopintnative) (cross-package extension)
- [extend UInt16 <: ThrowingOp\<UInt16>](overflow_package_extends.md#extend-uint16--throwingopuint16) (cross-package extension)
- [extend UInt32 <: ThrowingOp\<UInt32>](overflow_package_extends.md#extend-uint32--throwingopuint32) (cross-package extension)
- [extend UInt64 <: ThrowingOp\<UInt64>](overflow_package_extends.md#extend-uint64--throwingopuint64) (cross-package extension)
- [extend UInt8 <: ThrowingOp\<UInt8>](overflow_package_extends.md#extend-uint8--throwingopuint8) (cross-package extension)
- [extend UIntNative <: ThrowingOp\<UIntNative>](overflow_package_extends.md#extend-uintnative--throwingopuintnative) (cross-package extension)

### func throwingAdd(T)

```cangjie
func throwingAdd(y: T): T
```

Function: Addition operation using exception-throwing strategy.

Throws an exception when overflow occurs; otherwise returns the operation result.

Parameters:

- y: T - Addend.

Returns:

- T - Result of addition operation.

Exceptions:

- [OverflowException](../../core/core_package_api/core_package_exceptions.md#class-overflowexception) - Throws exception when addition operation overflows.

### func throwingDec()

```cangjie
func throwingDec(): T
```

Function: Decrement operation using exception-throwing strategy.

Throws an exception when overflow occurs; otherwise returns the operation result.

Returns:

- T - Result of decrement operation.

Exceptions:

- [OverflowException](../../core/core_package_api/core_package_exceptions.md#class-overflowexception) - Throws exception when decrement operation overflows.

### func throwingDiv(T)

```cangjie
func throwingDiv(y: T): T
```

Function: Division operation using exception-throwing strategy.Throws an exception when overflow occurs during the operation; otherwise, returns the operation result.

Parameters:

- y: T - The divisor.

Returns:

- T - The result of the division operation.

Exceptions:

- [OverflowException](../../core/core_package_api/core_package_exceptions.md#class-overflowexception) - Thrown when overflow occurs during the division operation.

### func throwingInc()

```cangjie
func throwingInc(): T
```

Function: Increment operation with exception-throwing strategy.

Throws an exception when overflow occurs during the operation; otherwise, returns the operation result.

Returns:

- T - The result of the increment operation.

Exceptions:

- [OverflowException](../../core/core_package_api/core_package_exceptions.md#class-overflowexception) - Thrown when overflow occurs during the increment operation.

### func throwingMod(T)

```cangjie
func throwingMod(y: T): T
```

Function: Modulo operation with exception-throwing strategy.

Throws an exception when overflow occurs during the operation; otherwise, returns the operation result.

Parameters:

- y: T - The divisor.

Returns:

- T - The result of the modulo operation.

Exceptions:

- [OverflowException](../../core/core_package_api/core_package_exceptions.md#class-overflowexception) - Thrown when overflow occurs during the modulo operation.

### func throwingMul(T)

```cangjie
func throwingMul(y: T): T
```

Function: Multiplication operation with exception-throwing strategy.

Throws an exception when overflow occurs during the operation; otherwise, returns the operation result.

Parameters:

- y: T - The multiplier.

Returns:

- T - The result of the multiplication operation.

Exceptions:

- [OverflowException](../../core/core_package_api/core_package_exceptions.md#class-overflowexception) - Thrown when overflow occurs during the multiplication operation.

### func throwingNeg()

```cangjie
func throwingNeg(): T
```

Function: Negation operation with exception-throwing strategy.

Throws an exception when overflow occurs during the operation; otherwise, returns the operation result.

Returns:

- T - The result of the negation operation.

Exceptions:

- [OverflowException](../../core/core_package_api/core_package_exceptions.md#class-overflowexception) - Thrown when overflow occurs during the negation operation.

### func throwingShl(UInt64)

```cangjie
func throwingShl(y: UInt64): T
```

Function: Left shift operation with exception-throwing strategy.

Throws an exception when the shift count is greater than or equal to the operand's bit width; otherwise, returns the operation result.

Parameters:

- y: [UInt64](../../core/core_package_api/core_package_intrinsics.md#uint64) - The shift count.

Returns:

- T - The result of the left shift operation.

Exceptions:

- [OvershiftException](overflow_package_exceptions.md#class-overshiftexception) - Thrown when the shift count is greater than or equal to the operand's bit width.

### func throwingShr(UInt64)

```cangjie
func throwingShr(y: UInt64): T
```

Function: Right shift operation.

Throws an exception when the shift count is greater than or equal to the operand's bit width; otherwise, returns the operation result.

Parameters:

- y: [UInt64](../../core/core_package_api/core_package_intrinsics.md#uint64) - The shift count.

Returns:

- T - The result of the right shift operation.

Exceptions:

- [OvershiftException](overflow_package_exceptions.md#class-overshiftexception) - Thrown when the shift count is greater than or equal to the operand's bit width.

### func throwingSub(T)

```cangjie
func throwingSub(y: T): T
```

Function: Subtraction operation with exception-throwing strategy.

Throws an exception when overflow occurs during the operation; otherwise, returns the operation result.

Parameters:- y: T - The subtrahend.

Returns:

- T - The result of the subtraction operation.

Exceptions:

- [OverflowException](../../core/core_package_api/core_package_exceptions.md#class-overflowexception) - Thrown when the subtraction operation overflows.

## interface ThrowingPow

```cangjie
public interface ThrowingPow
```

Function: Provides an interface for exponentiation operations with exception-throwing strategy.

Extensions:

- [extend Int64 <: ThrowingOp\<Int64> & ThrowingPow](overflow_package_extends.md#extend-int64--throwingopint64--throwingpow) (cross-package extension)

### func throwingPow(UInt64)

```cangjie
func throwingPow(y: UInt64): Int64
```

Function: Exponentiation operation with exception-throwing strategy.

Throws an exception when overflow occurs, otherwise returns the operation result.

Parameters:

- y: [UInt64](../../core/core_package_api/core_package_intrinsics.md#uint64) - Exponent.

Returns:

- [Int64](../../core/core_package_api/core_package_intrinsics.md#int64) - Result of exponentiation operation.

Exceptions:

- [OverflowException](../../core/core_package_api/core_package_exceptions.md#class-overflowexception) - Thrown when exponentiation operation overflows.

## interface WrappingOp\<T>

```cangjie
public interface WrappingOp<T>
```

Function: Performs integer operations with high-order truncation when overflow occurs.

Extensions:

- [extend Int16 <: WrappingOp\<Int16>](overflow_package_extends.md#extend-int16--wrappingopint16) (cross-package extension)
- [extend Int32 <: WrappingOp\<Int32>](overflow_package_extends.md#extend-int32--wrappingopint32) (cross-package extension)
- [extend Int64 <: WrappingOp\<Int64> & WrappingPow](overflow_package_extends.md#extend-int64--wrappingopint64--wrappingpow) (cross-package extension)
- [extend Int8 <: WrappingOp\<Int8>](overflow_package_extends.md#extend-int8--wrappingopint8) (cross-package extension)
- [extend IntNative <: WrappingOp\<IntNative>](overflow_package_extends.md#extend-intnative--wrappingopintnative) (cross-package extension)
- [extend UInt16 <: WrappingOp\<UInt16>](overflow_package_extends.md#extend-uint16--wrappingopuint16) (cross-package extension)
- [extend UInt32 <: WrappingOp\<UInt32>](overflow_package_extends.md#extend-uint32--wrappingopuint32) (cross-package extension)
- [extend UInt64 <: WrappingOp\<UInt64>](overflow_package_extends.md#extend-uint64--wrappingopuint64) (cross-package extension)
- [extend UInt8 <: WrappingOp\<UInt8>](overflow_package_extends.md#extend-uint8--wrappingopuint8) (cross-package extension)
- [extend UIntNative <: WrappingOp\<UIntNative>](overflow_package_extends.md#extend-uintnative--wrappingopuintnative) (cross-package extension)

### func wrappingAdd(T)

```cangjie
func wrappingAdd(y: T): T
```

Function: Addition operation with high-order truncation strategy.

Returns the operation result with high-order truncation when overflow occurs.

Parameters:

- y: T - Addend.

Returns:

- T - Result of addition operation.

### func wrappingDec()

```cangjie
func wrappingDec(): T
```

Function: Decrement operation with high-order truncation strategy.

Returns the operation result with high-order truncation when overflow occurs.

Returns:

- T - Result of decrement operation.

### func wrappingDiv(T)

```cangjie
func wrappingDiv(y: T): T
```

Function: Division operation with high-order truncation strategy.

Returns the operation result with high-order truncation when overflow occurs.

Parameters:

- y: T - Divisor.

Returns:

- T - Result of division operation.

### func wrappingInc()

```cangjie
func wrappingInc(): T
```

Function: Increment operation with high-order truncation strategy.

Returns the operation result with high-order truncation when overflow occurs.

Returns:

- T - Result of increment operation.

### func wrappingMod(T)

```cangjie
func wrappingMod(y: T): T
```

Function: Modulo operation with high-order truncation strategy.

Returns the operation result with high-order truncation when overflow occurs.

Parameters:

- y: T - Divisor.

Returns:

- T - Result of modulo operation.

### func wrappingMul(T)

```cangjie
func wrappingMul(y: T): T
```

Function: Multiplication operation with high-order truncation strategy.

Returns the operation result with high-order truncation when overflow occurs.

Parameters:

- y: T - Multiplier.

Returns:

- T - Result of multiplication operation.

### func wrappingNeg()

```cangjie
func wrappingNeg(): T
```

Function: Negation operation with high-order truncation strategy.

Returns the operation result with high-order truncation when overflow occurs.

Returns:

- T - Result of negation operation.

### func wrappingShl(UInt64)

```cangjie
func wrappingShl(y: UInt64): T
```

Function: Left shift operation with high-order truncation strategy.

Performs high-order truncation when shift count exceeds operand bit width. For example, when shifting an [Int8](../../core/core_package_api/core_package_intrinsics.md#int8) value, if y (shift count) is greater than or equal to 8, only the lower 3 bits of y are used as the shift count to ensure it remains between 0 and 7.

Parameters:

- y: [UInt64](../../core/core_package_api/core_package_intrinsics.md#uint64) - Shift count.

Returns:

- T - Result of left shift operation.

### func wrappingShr(UInt64)

```cangjie
func wrappingShr(y: UInt64): T
```

Function: Right shift operation with high-order truncation strategy.

Performs high-order truncation when shift count exceeds operand bit width. For example, when shifting an [Int8](../../core/core_package_api/core_package_intrinsics.md#int8) value, if y (shift count) is greater than or equal to 8, only the lower 3 bits of y are used as the shift count to ensure it remains between 0 and 7.

Parameters:

- y: [UInt64](../../core/core_package_api/core_package_intrinsics.md#uint64) - Shift count.

Returns:

- T - Result of right shift operation.

### func wrappingSub(T)

```cangjie
func wrappingSub(y: T): T
```

Function: Subtraction operation with high-order truncation strategy.

Returns the operation result with high-order truncation when overflow occurs.

Parameters:

- y: T - Subtrahend.

Returns:

- T - Result of subtraction operation.

## interface WrappingPow

```cangjie
public interface WrappingPow {
    func wrappingPow(y: UInt64): Int64
```

Feature: Provides an interface for exponentiation operations using high-order truncation strategy.

### func wrappingPow(UInt64)

```cangjie
func wrappingPow(y: UInt64): Int64
```

Function: Performs exponentiation using high-order truncation strategy.

Extensions:

- [extend Int64 <: WrappingOp\<Int64> & WrappingPow](overflow_package_extends.md#extend-int64--wrappingopint64--wrappingpow) (cross-package extension)

When overflow occurs during computation, the high-order bits are truncated; otherwise, returns the computation result.

Parameters:

- y: [UInt64](../../core/core_package_api/core_package_intrinsics.md#uint64) - The exponent.

Returns:

- [Int64](../../core/core_package_api/core_package_intrinsics.md#int64) - The result of the exponentiation operation.