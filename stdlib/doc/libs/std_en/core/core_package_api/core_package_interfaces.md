# Interface

## interface Any

```cangjie
public interface Any
```

Function: [Any](core_package_interfaces.md#interface-any) is the parent type of all types. All `interface` types implicitly inherit from it, and all non-`interface` types implicitly implement it.

## interface CType

```cangjie
sealed interface CType
```

Function: Represents an interface that supports interoperability with the C language.

The [CType](core_package_interfaces.md#interface-ctype) interface is a built-in empty interface in the language. It is a concrete implementation of the [CType](core_package_interfaces.md#interface-ctype) constraint. All C-interoperable types implicitly implement this interface, thus all C-interoperable types can be used as subtypes of the [CType](core_package_interfaces.md#interface-ctype) type.

> **Note:**
>
> - The [CType](core_package_interfaces.md#interface-ctype) interface is an interface type in Cangjie and does not itself satisfy the [CType](core_package_interfaces.md#interface-ctype) constraint.
> - The [CType](core_package_interfaces.md#interface-ctype) interface cannot be inherited or extended by users.
> - The [CType](core_package_interfaces.md#interface-ctype) interface does not bypass subtype usage restrictions.

Example:

<!-- run -->
```cangjie
@C
struct Data {}

@C
func foo() {}

main() {
    var c: CType = Data() // ok
    c = 0 // ok
    c = true // ok
    c = CString(CPointer<UInt8>()) // ok
    c = CPointer<Int8>() // ok
    c = foo // ok
}
```

## interface Collection\<T>

```cangjie
public interface Collection<T> <: Iterable<T>
```

Function: This interface represents a collection. Container types should generally implement this interface.

Parent Types:

- [Iterable](#interface-iterablee)\<T>

### prop size

```cangjie
prop size: Int64
```

Function: Gets the size of the current collection, i.e., the number of elements in the collection.

Type: [Int64](core_package_intrinsics.md#int64)

### func isEmpty()

```cangjie
func isEmpty(): Bool
```

Function: Determines whether the current collection is empty.

Returns:

- [Bool](core_package_intrinsics.md#bool) - Returns `true` if empty, otherwise `false`.

### func toArray()

```cangjie
func toArray(): Array<T>
```

Function: Converts the current collection to an array type.

Returns:

- [Array](core_package_structs.md#struct-arrayt)\<T> - The resulting array after conversion.

## interface Comparable\<T>

```cangjie
public interface Comparable<T> <: Equatable<T> & Less<T> & Greater<T> & LessOrEqual<T> & GreaterOrEqual<T>
```

Function: This interface represents comparison operations and is a composite of equality, inequality, less-than, greater-than, less-than-or-equal, and greater-than-or-equal interfaces.

This interface provides default implementations for the operator overloads `==`, `!=`, `<`, `<=`, `>`, and `>=`. The default implementations determine their return values based on the return value of the `compare` function. For example: if `a.compare(b)` returns `EQ`, then `a == b` returns `true`; otherwise, it returns `false`.

Parent Types:

- [Equatable](#interface-equatablet)\<T>
- [Less](#interface-lesst)\<T>
- [Greater](#interface-greatert)\<T>
- [LessOrEqual](#interface-lessorequalt)\<T>
- [GreaterOrEqual](#interface-greaterorequalt)\<T>

### func compare(T)

```cangjie
func compare(that: T): Ordering
```

Function: Determines the size relationship between the current instance of type `T` and another instance of type `T` pointed to by the parameter.

Parameters:

- that: T - Another instance to compare with the current instance.

Returns:

- [Ordering](core_package_enums.md#enum-ordering) - Returns [Ordering](core_package_enums.md#enum-ordering).GT if greater, [Ordering](core_package_enums.md#enum-ordering).EQ if equal, and [Ordering](core_package_enums.md#enum-ordering).LT if less.

### operator func <(T)

```cangjie
operator func <(rhs: T): Bool
```

Function: Determines whether the current instance of type `T` is less than another instance of type `T` pointed to by the parameter. This function is a default implementation of this interface.

Parameters:

- rhs: T - Another instance to compare with the current instance.

Returns:

- [Bool](core_package_intrinsics.md#bool) - Returns `true` if less, otherwise `false`.

### operator func <=(T)

```cangjie
operator func <=(rhs: T): Bool
```

Function: Determines whether the current instance of type `T` is less than or equal to another instance of type `T` pointed to by the parameter. This function is a default implementation of this interface.

Parameters:

- rhs: T - Another instance to compare with the current instance.

Returns:

- [Bool](core_package_intrinsics.md#bool) - Returns `true` if less than or equal, otherwise `false`.

### operator func ==(T)

```cangjie
operator func ==(rhs: T): Bool
```

Function: Determines whether two instances are equal. This function is a default implementation of this interface.

Parameters:

- rhs: T - Another instance to compare.

Returns:

- [Bool](core_package_intrinsics.md#bool) - Returns `true` if equal, otherwise `false`.

### operator func >(T)

```cangjie
operator func >(rhs: T): Bool
```

Function: Determines whether the current instance of type `T` is greater than another instance of type `T` pointed to by the parameter. This function is a default implementation of this interface.

Parameters:

- rhs: T - Another instance to compare with the current instance.

Returns:

- [Bool](core_package_intrinsics.md#bool) - Returns `true` if greater, otherwise `false`.

### operator func >=(T)

```cangjie
operator func >=(rhs: T): Bool
```

Function: Determines whether the current instance of type `T` is greater than or equal to another instance of type `T` pointed to by the parameter. This function is a default implementation of this interface.

Parameters:

- rhs: T - Another instance to compare with the current instance.

Returns:

- [Bool](core_package_intrinsics.md#bool) - Returns `true` if greater than or equal, otherwise `false`.

## interface Countable\<T>

```cangjie
public interface Countable<T>
```

Function: This interface indicates that a type is countable.

Each instance of a countable type corresponds to a position value (an [Int64](core_package_intrinsics.md#int64) value). Other instances of the type can be obtained by counting forward.

### func next(Int64)

```cangjie
func next(right: Int64): T
```

Function: Gets the instance of type `T` at the position obtained by moving `right` positions to the right from the current instance.

Parameters:

- right: [Int64](core_package_intrinsics.md#int64) - The number of positions to count to the right.

Returns:

- T - The instance of type `T` at the position obtained by moving `right` positions to the right.

### func position()

```cangjie
func position(): Int64
```

Function: Gets the position information of the current countable instance, i.e., converts the current instance to an [Int64](core_package_intrinsics.md#int64) type.

Returns:

- [Int64](core_package_intrinsics.md#int64) - The converted [Int64](core_package_intrinsics.md#int64) value.

## interface Equal\<T>

```cangjie
public interface Equal<T>
```

Function: This interface supports equality comparison operations.

### operator func ==(T)

```cangjie
operator func ==(rhs: T): Bool
```

Function: Determines whether two instances are equal.

Parameters:

- rhs: T - Another instance to compare.

Returns:

- [Bool](core_package_intrinsics.md#bool) - Returns `true` if equal, otherwise `false`.

## interface Equatable\<T>

```cangjie
public interface Equatable<T> <: Equal<T> & NotEqual<T>
```

Function: This interface is a composite of equality and inequality comparison interfaces.

This interface provides a default implementation for the `!=` operator overload. The default implementation determines its return value based on the return value of the `==` operation. For example: if `a == b` returns `true`, then `a != b` returns `false`; otherwise, it returns `true`.

This interface has been implemented for some Cangjie types, including: [Unit](core_package_intrinsics.md#unit), [Bool](core_package_intrinsics.md#bool), [Rune](core_package_intrinsics.md#rune), [Int64](core_package_intrinsics.md#int64), [Int32](core_package_intrinsics.md#int32), [Int16](core_package_intrinsics.md#int16), [Int8](core_package_intrinsics.md#int8), [UIntNative](core_package_intrinsics.md#uintnative), [UInt64](core_package_intrinsics.md#uint64), [UInt32](core_package_intrinsics.md#uint32), [UInt16](core_package_intrinsics.md#uint16), [UInt8](core_package_intrinsics.md#uint8), [Float64](core_package_intrinsics.md#float64), [Float32](core_package_intrinsics.md#float32), [Float16](core_package_intrinsics.md#float16), [String](core_package_structs.md#struct-string), [Array](core_package_structs.md#struct-arrayt), [Box](core_package_classes.md#class-boxt), [ArrayList](../../collection/collection_package_api/collection_package_class.md#class-arraylistt), [HashSet](../../collection/collection_package_api/collection_package_class.md#class-hashsett-where-t--hashable--equatablet).

Parent Types:

- [Equal](#interface-equalt)\<T>
- [NotEqual](#interface-notequalt)\<T>

### operator func !=(T)

```cangjie
operator func !=(rhs: T): Bool
```

Function: Determines whether two instances are not equal. This function is a default implementation of this interface.

Parameters:

- rhs: T - Another instance to compare.

Returns:

- [Bool](core_package_intrinsics.md#bool) - Returns `true` if not equal, otherwise `false`.## interface GreaterOrEqual\<T>

```cangjie
public interface GreaterOrEqual<T> {
    operator func >=(rhs: T): Bool
}
```

Function: This interface represents the greater-than-or-equal-to operation.

### operator func >=(T)

```cangjie
operator func >=(rhs: T): Bool
```

Function: Determines whether the current instance of type `T` is greater than or equal to the instance of type `T` pointed to by the parameter.

Parameters:

- rhs: T - Another instance to be compared with the current instance.

Returns:

- [Bool](core_package_intrinsics.md#bool) - Returns true if greater than or equal, otherwise returns false.

## interface Greater\<T>

```cangjie
public interface Greater<T>
```

Function: This interface represents the greater-than operation.

### operator func >(T)

```cangjie
operator func >(rhs: T): Bool
```

Function: Determines whether the current instance of type `T` is greater than the instance of type `T` pointed to by the parameter.

Parameters:

- rhs: T - Another instance to be compared with the current instance.

Returns:

- [Bool](core_package_intrinsics.md#bool) - Returns true if greater, otherwise returns false.

## interface Hashable

```cangjie
public interface Hashable
```

Function: This interface is used for computing hash values.

It has been implemented for some Cangjie types, including: [Bool](core_package_intrinsics.md#bool), [Rune](core_package_intrinsics.md#rune), [IntNative](core_package_intrinsics.md#intnative), [Int64](core_package_intrinsics.md#int64), [Int32](core_package_intrinsics.md#int32), [Int16](core_package_intrinsics.md#int16), [Int8](core_package_intrinsics.md#int8), [UIntNative](core_package_intrinsics.md#uintnative), [UInt64](core_package_intrinsics.md#uint64), [UInt32](core_package_intrinsics.md#uint32), [UInt16](core_package_intrinsics.md#uint16), [UInt8](core_package_intrinsics.md#uint8), [Float64](core_package_intrinsics.md#float64), [Float32](core_package_intrinsics.md#float32), [Float16](core_package_intrinsics.md#float16), [String](core_package_structs.md#struct-string), [Box](core_package_classes.md#class-boxt).

### func hashCode()

```cangjie
func hashCode(): Int64
```

Function: Obtains the hash value of the instance type.

Returns:

- [Int64](core_package_intrinsics.md#int64) - Returns the hash value of the instance type.

## interface Hasher

```cangjie
public interface Hasher
```

Function: This interface is used for handling combined hash operations.

A series of write functions can be used to pass instances of different data types and compute their combined hash values.

### func finish()

```cangjie
func finish(): Int64
```

Function: Obtains the result of the hash operation.

Returns:

- [Int64](core_package_intrinsics.md#int64) - The computed hash value.

### func reset()

```cangjie
mut func reset(): Unit
```

Function: Resets the hash value to 0.

### func write(Bool)

```cangjie
mut func write(value: Bool): Unit
```

Function: Passes the [Bool](core_package_intrinsics.md#bool) value to be hashed and performs the combined hash operation.

Parameters:

- value: [Bool](core_package_intrinsics.md#bool) - The value to be hashed.

### func write(Float16)

```cangjie
mut func write(value: Float16): Unit
```

Function: Passes the [Float16](core_package_intrinsics.md#float16) value to be hashed and performs the combined hash operation.

Parameters:

- value: [Float16](core_package_intrinsics.md#float16) - The value to be hashed.

### func write(Float32)

```cangjie
mut func write(value: Float32): Unit
```

Function: Passes the [Float32](core_package_intrinsics.md#float32) value to be hashed and performs the combined hash operation.

Parameters:

- value: [Float32](core_package_intrinsics.md#float32) - The value to be hashed.

### func write(Float64)

```cangjie
mut func write(value: Float64): Unit
```

Function: Passes the [Float64](core_package_intrinsics.md#float64) value to be hashed and performs the combined hash operation.

Parameters:

- value: [Float64](core_package_intrinsics.md#float64) - The value to be hashed.

### func write(Int16)

```cangjie
mut func write(value: Int16): Unit
```

Function: Passes the [Int16](core_package_intrinsics.md#int16) value to be hashed and performs the combined hash operation.

Parameters:

- value: [Int16](core_package_intrinsics.md#int16) - The value to be hashed.

### func write(Int32)

```cangjie
mut func write(value: Int32): Unit
```

Function: Passes the [Int32](core_package_intrinsics.md#int32) value to be hashed and performs the combined hash operation.

Parameters:

- value: [Int32](core_package_intrinsics.md#int32) - The value to be hashed.

### func write(Int64)

```cangjie
mut func write(value: Int64): Unit
```

Function: Passes the [Int64](core_package_intrinsics.md#int64) value to be hashed and performs the combined hash operation.

Parameters:

- value: [Int64](core_package_intrinsics.md#int64) - The value to be hashed.

### func write(Int8)

```cangjie
mut func write(value: Int8): Unit
```

Function: Passes the [Int8](core_package_intrinsics.md#int8) value to be hashed and performs the combined hash operation.

Parameters:

- value: [Int8](core_package_intrinsics.md#int8) - The value to be hashed.

### func write(Rune)

```cangjie
mut func write(value: Rune): Unit
```

Function: Passes the [Rune](core_package_intrinsics.md#rune) value to be hashed and performs the combined hash operation.

Parameters:

- value: [Rune](core_package_intrinsics.md#rune) - The value to be hashed.

### func write(String)

```cangjie
mut func write(value: String): Unit
```

Function: Passes the [String](core_package_structs.md#struct-string) value to be hashed and performs the combined hash operation.

Parameters:

- value: [String](core_package_structs.md#struct-string) - The value to be hashed.

### func write(UInt16)

```cangjie
mut func write(value: UInt16): Unit
```

Function: Passes the [UInt16](core_package_intrinsics.md#uint16) value to be hashed and performs the combined hash operation.

Parameters:

- value: [UInt16](core_package_intrinsics.md#uint16) - The value to be hashed.

### func write(UInt32)

```cangjie
mut func write(value: UInt32): Unit
```

Function: Passes the [UInt32](core_package_intrinsics.md#uint32) value to be hashed and performs the combined hash operation.

Parameters:

- value: [UInt32](core_package_intrinsics.md#uint32) - The value to be hashed.

### func write(UInt64)

```cangjie
mut func write(value: UInt64): Unit
```

Function: Passes the [UInt64](core_package_intrinsics.md#uint64) value to be hashed and performs the combined hash operation.

Parameters:

- value: [UInt64](core_package_intrinsics.md#uint64) - The value to be hashed.

### func write(UInt8)

```cangjie
mut func write(value: UInt8): Unit
```

Function: Passes the [UInt8](core_package_intrinsics.md#uint8) value to be hashed and performs the combined hash operation.

Parameters:

- value: [UInt8](core_package_intrinsics.md#uint8) - The value to be hashed.

## interface Iterable\<E>

```cangjie
public interface Iterable<E>
```

Function: This interface represents iterability. Types implementing this interface (typically container types) can be iterated over in `for-in` statements or obtain corresponding iterator instances to implement iteration by calling the `next` function.

This package has already implemented this interface for basic container types such as [Array](core_package_structs.md#struct-arrayt), [ArrayList](../../collection/collection_package_api/collection_package_class.md#class-arraylistt), [HashMap](../../collection/collection_package_api/collection_package_class.md#class-hashmapk-v-where-k--hashable--equatablek). Users can implement this interface for other types to support iteration functionality.

### func iterator()

```cangjie
func iterator(): Iterator<E>
```

Function: Obtains the iterator.

Returns:

- [Iterator](core_package_classes.md#class-iteratort)\<E> - The iterator.

## interface LessOrEqual\<T>

```cangjie
public interface LessOrEqual<T>
```

Function: This interface represents the less-than-or-equal-to operation.

### operator func <=(T)

```cangjie
operator func <=(rhs: T): Bool
```

Function: Determines whether the current instance of type `T` is less than or equal to the instance of type `T` pointed to by the parameter.

Parameters:

- rhs: T - Another instance to be compared with the current instance.

Returns:

- [Bool](core_package_intrinsics.md#bool) - Returns true if less than or equal, otherwise returns false.

## interface Less\<T>

```cangjie
public interface Less<T>
```

Function: This interface represents the less-than operation.

### operator func <(T)

```cangjie
operator func <(rhs: T): Bool
```

Function: Determines whether the current instance of type `T` is less than the instance of type `T` pointed to by the parameter.

Parameters:

- rhs: T - Another instance to be compared with the current instance.

Returns:

- [Bool](core_package_intrinsics.md#bool) - Returns true if less, otherwise returns false.

## interface NotEqual\<T>

```cangjie
public interface NotEqual<T>
```

Function: This interface is used to support inequality operations.

### operator func !=(T)

```cangjie
operator func !=(rhs: T): Bool
```

Function: Determines whether two instances are not equal.

Parameters:

- rhs: T - Another instance to be compared.

Returns:

- [Bool](core_package_intrinsics.md#bool) - Returns true if not equal, otherwise returns false.

## interface Resource

```cangjie
public interface Resource
```

Function: This interface is used for resource management, typically for closing and releasing resources such as memory and handles.

Types implementing this interface can achieve automatic resource release in `try-with-resource` syntax contexts.

### func isClosed()

```cangjie
func isClosed(): Bool
```

Function: Determines whether the resource has been closed.

Returns:

- [Bool](core_package_intrinsics.md#bool) - Returns true if already closed, otherwise returns false.

### func close()

```cangjie
func close(): Unit
```

Function: Closes the resource.## interface ThreadContext

```cangjie
public interface ThreadContext {
    func end(): Unit
    func hasEnded(): Bool
}
```

Function: Cangjie thread context interface.

When users create a `thread`, in addition to the default `spawn` expression parameters, they can also pass instances of different [ThreadContext](core_package_interfaces.md#interface-threadcontext) types to select different thread contexts, thereby controlling concurrent behavior to some extent.

Currently, users are not allowed to implement the [ThreadContext](core_package_interfaces.md#interface-threadcontext) interface themselves. The Cangjie language provides `MainThreadContext` based on usage scenarios. For specific definitions, please refer to the terminal framework library.

### func end()

```cangjie
func end(): Unit
```

Function: Termination method, used to send a termination request to the current context.

### func hasEnded()

```cangjie
func hasEnded(): Bool
```

Function: Check method, used to verify whether the current context has terminated.

Returns:

- [Bool](core_package_intrinsics.md#bool) - Returns true if terminated, otherwise returns false.

## interface ToString

```cangjie
public interface ToString
```

Function: This interface provides string representation for concrete types.

Extensions:

- [extend Float64 <: ToString](core_package_extends.md#extend-float64--tostring)

### func toString()

```cangjie
func toString(): String
```

Function: Obtains the string representation of an instance type.

Returns:

- [String](core_package_structs.md#struct-string) - Returns the string representation of the instance type.