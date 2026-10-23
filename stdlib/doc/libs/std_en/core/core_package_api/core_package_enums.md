# Enumerations

## enum AnnotationKind

```cangjie
public enum AnnotationKind
```

Function: Indicates the supported locations for custom annotations.

### EnumConstructor

```cangjie
EnumConstructor
```

Function: Enum constructor declaration.

### Extension

```cangjie
Extension
```

Function: Extension declaration.

### Init

```cangjie
Init
```

Function: Constructor declaration.

### GlobalFunction

```cangjie
GlobalFunction
```

Function: Global function declaration.

### GlobalVariable

```cangjie
GlobalVariable
```

Function: Global variable declaration.

### MemberFunction

```cangjie
MemberFunction
```

Function: Member function declaration.

### MemberProperty

```cangjie
MemberProperty
```

Function: Member property declaration.

### MemberVariable

```cangjie
MemberVariable
```

Function: Member variable declaration.

### Parameter

```cangjie
Parameter
```

Function: Parameters in member functions/constructors (excluding enum constructor parameters).

### Type

```cangjie
Type
```

Function: Type declaration (class, struct, enum, interface).

## enum Endian

```cangjie
public enum Endian
```

Function: The enumeration type [Endian](core_package_enums.md#enum-endian) represents the endianness of the runtime platform, divided into big-endian and little-endian.

### Big

```cangjie
Big
```

Function: Represents big-endian.

### Little

```cangjie
Little
```

Function: Represents little-endian.

### static prop Platform

```cangjie
public static prop Platform: Endian
```

Function: Gets the endianness of the runtime platform.

Type: [Endian](core_package_enums.md#enum-endian)

Exceptions:

- [UnsupportedException](core_package_exceptions.md#class-unsupportedexception) - Thrown when the endianness returned by the runtime platform is unrecognizable.

Example:

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

Execution Result:

```text
LittleEndian
```

## enum Option\<T>

```cangjie
public enum Option<T>
```

Function: [Option](core_package_enums.md#enum-optiont)\<T> is a wrapper for type `T`, indicating the possibility of having a value or no value.

It contains two constructors: [Some](#somet) and [None](#none). [Some](#somet) carries a parameter indicating a value exists, while [None](#none) has no parameter, indicating no value. When you need to represent that a type may or may not have a value, you can use the [Option](core_package_enums.md#enum-optiont) type.

An alternative notation for the [Option](core_package_enums.md#enum-optiont) type is prefixing the type name with `?`. For any type `Type`, `?Type` is equivalent to [Option](core_package_enums.md#enum-optiont)\<Type>.

Extensions:

- [extend\<T> Option\<Option\<T>>](core_package_extends.md#extendt-optionoptiont)
- [extend\<T> Option\<T> <: Equatable\<Option\<T>> where T <: Equatable\<T>](core_package_extends.md#extendt-optiont--equatableoptiont-where-t--equatablet)
- [extend\<T> Option\<T> <: Hashable where T <: Hashable](core_package_extends.md#extendt-optiont--hashable-where-t--hashable)
- [extend\<T> Option\<T> <: ToString where T <: ToString](core_package_extends.md#extendt-optiont--tostring-where-t--tostring)

### None

```cangjie
None
```

Function: Constructs a parameterless [Option](core_package_enums.md#enum-optiont)\<T> instance, indicating no value.

### Some(T)

```cangjie
Some(T)
```

Function: Constructs an [Option](core_package_enums.md#enum-optiont)\<T> instance with a parameter, indicating a value exists.

### func filter((T)->Bool)

```cangjie
public func filter(predicate: (T) -> Bool): Option<T>
```

Function: Provides "filter" functionality for the [Option](core_package_enums.md#enum-optiont) type.

Parameters:

- predicate: (T) -> [Bool](core_package_intrinsics.md#bool) - The filter function.

Returns:

- Option\<T> - If the [Option](core_package_enums.md#enum-optiont) value is [Some](#somet)(v) and v satisfies `predicate(v) = true`, returns [Some](#somet)(v); otherwise, returns [None](#none).

Example:

<!-- verify -->
```cangjie
main() {
    // Create a Some value
    var someValue: Option<Int64> = Some(5)
    
    // Filter values greater than 3
    var filtered1 = someValue.filter({x => x > 3})
    println("Filter values > 3: ${filtered1}")
    
    // Filter values less than 3
    var filtered2 = someValue.filter({x => x < 3})
    println("Filter values < 3: ${filtered2}")
    
    // Create a None value
    var noneValue: Option<Int64> = None
    
    // Apply filter to None value
    var filtered3 = noneValue.filter({x => x > 3})
    println("Filter None value: ${filtered3}")
}
```

Execution Result:

```text
Filter values > 3: Some(5)
Filter values < 3: None
Filter None value: None
```

### func flatMap\<R>((T) -> Option\<R>)

```cangjie
public func flatMap<R>(transform: (T) -> Option<R>): Option<R>
```

Function: Provides a mapping function from [Option](core_package_enums.md#enum-optiont)\<T> to [Option](core_package_enums.md#enum-optiont)\<R>. If the current instance value is [Some](#somet), executes the transform function and returns the result; otherwise, returns [None](#none).

Parameters:

- transform: (T) -> [Option](core_package_enums.md#enum-optiont)\<R> - The mapping function.

Returns:

- [Option](core_package_enums.md#enum-optiont)\<R> - If the current instance value is [Some](#somet), executes the transform function and returns the result; otherwise, returns [None](#none).

Example:

<!-- verify -->
```cangjie
// Define a function to convert Int64 to Option<String>
func intToStringOption(x: Int64): Option<String> {
    if (x > 0) {
        return Some("Positive: ${x}")
    } else {
        return None
    }
}

main() {
    // Create a Some value
    var someValue: Option<Int64> = Some(5)
    
    // Use flatMap to convert Int64 to String
    var flatMapped1 = someValue.flatMap(intToStringOption)
    println("Apply flatMap to Some value: ${flatMapped1}")
    
    // Create a negative Some value
    var someNegativeValue: Option<Int64> = Some(-3)
    
    // Apply flatMap to negative value
    var flatMapped2 = someNegativeValue.flatMap(intToStringOption)
    println("Apply flatMap to negative Some value: ${flatMapped2}")
    
    // Create a None value
    var noneValue: Option<Int64> = None
    
    // Apply flatMap to None value
    var flatMapped3 = noneValue.flatMap(intToStringOption)
    println("Apply flatMap to None value: ${flatMapped3}")
}
```

Execution Result:

```text
Apply flatMap to Some value: Some(Positive: 5)
Apply flatMap to negative Some value: None
Apply flatMap to None value: None
```

### func getOrDefault(() -> T)

```cangjie
public func getOrDefault(other: () -> T): T
```

Function: Gets the value or returns a default value. If the [Option](core_package_enums.md#enum-optiont) value is [Some](#somet), returns the instance of type `T`; if the [Option](core_package_enums.md#enum-optiont) value is [None](#none), calls the input parameter and returns the value of type `T`.

Parameters:

- other: () -> T - The default function. If the current instance value is [None](#none), calls this function to get an instance of type `T` and returns it.

Returns:

- T - If the current instance value is [Some](#somet)\<T>, returns the instance of type `T` carried by the current instance. If the [Option](core_package_enums.md#enum-optiont) value is [None](#none), calls the specified function to get an instance of type `T` and returns it.

Example:

<!-- verify -->
```cangjie
main() {
    var value1: Option<Int64> = Some(2)
    println(value1.getOrDefault({=> 0}))

    var value2: Option<Int64> = None
    println(value2.getOrDefault({=> 0}))
}
```

Execution Result:

```text
2
0
```

### func getOrThrow(() -> Exception)

```cangjie
public func getOrThrow(exception: ()->Exception): T
```

Function: Gets the value or throws a specified exception.

Parameters:

- exception: () ->[Exception](core_package_exceptions.md#class-exception) - The exception function. If the current instance value is [None](#none), executes this function and throws the returned exception.

Returns:

- T - If the current instance value is [Some](#somet)\<T>, returns the instance of type `T`.

Exceptions:

- [Exception](core_package_exceptions.md#class-exception) - If the current instance is [None](#none), throws the exception returned by the exception function.

Example:

<!-- verify -->
```cangjie
main() {
    // Create a Some value
    var someValue: Option<Int64> = Some(42)
    
    // Apply getOrThrow to Some value, should return the value
    var value1 = someValue.getOrThrow({=> Exception("Value is None")})
    println("Value obtained from Some: ${value1}")
    
    // Create a None value
    var noneValue: Option<Int64> = None
    
    // Apply getOrThrow to None value, should throw exception
    try {
        noneValue.getOrThrow({=> Exception("Value is None")})
        println("This line won't execute")
    } catch (e: Exception) {
        println("Caught exception: ${e.message}")
    }
}
```

Execution Result:

```text
Value obtained from Some: 42
Caught exception: Value is None
```

### func getOrThrow()

```cangjie
public func getOrThrow(): T
```

Function: Gets the value or throws an exception.

Returns:

- T - If the current instance value is [Some](#somet)\<T>, returns the instance of type `T`.

Exceptions:

- [NoneValueException](core_package_exceptions.md#class-nonevalueexception) - If the current instance is [None](#none), throws an exception.

Example:

<!-- verify -->
```cangjie
main() {
    // Create a Some value
    var someValue: Option<Int64> = Some(42)
    
    // Apply getOrThrow to Some value, should return the value
    var value1 = someValue.getOrThrow()
    println("Value obtained from Some: ${value1}")
    
    // Create a None value
    var noneValue: Option<Int64> = None
    
    // Apply getOrThrow to None value, should throw NoneValueException
    try {
        noneValue.getOrThrow()
        println("This line won't execute")
    } catch (e: NoneValueException) {
        println("Caught NoneValueException")
    }
}
```

Execution Result:

```text
Value obtained from Some: 42
Caught NoneValueException
```

### func isNone()

```cangjie
public func isNone(): Bool
```

Function: Determines whether the current instance value is [None](#none).

Returns:

- [Bool](core_package_intrinsics.md#bool) - Returns true if the current instance value is [None](#none), otherwise returns false.

Example:

<!-- verify -->
```cangjie
main() {
    // Create a Some value
    var someValue: Option<Int64> = Some(42)
    
    // Check if it is None
    println("Some(42) is None: ${someValue.isNone()}")
    
    // Create a None value
    var noneValue: Option<Int64> = None
    
    // Check if it is None
    println("None is None: ${noneValue.isNone()}")
}
```

Execution Result:

```text
Some(42) is None: false
None is None: true
```

### func isSome()

```cangjie
public func isSome(): Bool
```

Function: Determines whether the current instance value is [Some](#somet).

Returns:

- [Bool](core_package_intrinsics.md#bool) - Returns true if the current instance value is [Some](#somet), otherwise returns false.

Example:

<!-- verify -->
```cangjie
main() {
    // Create a Some value
    var someValue: Option<Int64> = Some(42)
    
    // Check if it is Some
    println("Some(42) is Some: ${someValue.isSome()}")
    
    // Create a None value
    var noneValue: Option<Int64> = None
    
    // Check if it is Some
    println("None is Some: ${noneValue.isSome()}")
}
```

Execution Result:

```text
Some(42) is Some: true
None is Some: false
```

### func map\<R>((T)->R)

```cangjie
public func map<R>(transform: (T)-> R): Option<R>
```

Function: Provides a mapping function from [Option](#enum-optiont)\<T> type to [Option](#enum-optiont)\<R> type. If the current instance value is [Some](#somet), executes the transform function and returns the result encapsulated in [Some](#somet); otherwise, returns [None](#none).

Parameters:

- transform: (T)-> R - The mapping function.

Returns:

- [Option](core_package_enums.md#enum-optiont)\<R> - If the current instance value is [Some](#somet), executes the transform function and returns the result as [Option](#enum-optiont)\<R> type; otherwise, returns [None](#none).

Example:

<!-- verify -->
```cangjie
main() {
    // Create a Some value
    var someValue: Option<Int64> = Some(42)
    
    // Use map to convert Int64 to String
    var mapped1 = someValue.map({x => "Number: ${x}"})
    println("Applying map to Some value: ${mapped1}")
    
    // Create a None value
    var noneValue: Option<Int64> = None
    
    // Apply map to None value
    var mapped2 = noneValue.map({x => "Number: ${x}"})
    println("Applying map to None value: ${mapped2}")
}
```

Execution Result:

```text
Applying map to Some value: Some(Number: 42)
Applying map to None value: None
```

## enum Ordering

```cangjie
public enum Ordering
```

Function: [Ordering](core_package_enums.md#enum-ordering) represents the result of a comparison operation, containing three possible cases: less than, greater than, and equal to.

Extensions:

- [extend Ordering <: Comparable](core_package_extends.md#extend-ordering--comparable)
- [extend Ordering <: Hashable](core_package_extends.md#extend-ordering--hashable)
- [extend Ordering <: ToString](core_package_extends.md#extend-ordering--tostring)

### EQ

```cangjie
EQ
```

Function: Constructs an [Ordering](core_package_enums.md#enum-ordering) instance representing equality.

### GT

```cangjie
GT
```

Function: Constructs an [Ordering](core_package_enums.md#enum-ordering) instance representing greater than.

### LT

```cangjie
LT
```

Function: Constructs an [Ordering](core_package_enums.md#enum-ordering) instance representing less than.

## enum ThreadState

```cangjie
public enum ThreadState <: ToString
```

Function: Represents the state of a thread.

Parent Types:

- [ToString](core_package_interfaces.md#interface-tostring)

### Ready

```cangjie
Ready
```

Function: Indicates that the thread has just been created or has ended suspension and is waiting to be scheduled for execution.

### Running

```cangjie
Running
```

Function: Indicates that the thread is currently executing.

### Pending

```cangjie
Pending
```

Function: Indicates that the thread is suspended.

### Terminated

```cangjie
Terminated
```

Function: Indicates that the thread has finished execution.

### func toString()

```cangjie
public func toString(): String
```

Function: Converts the [ThreadState](core_package_enums.md#enum-threadstate) instance to a printable string.

Conversion results are as follows:

- Ready: "Ready".
- Running: "Running".
- Pending: "Pending".
- Terminated: "Terminated".

Returns:

- [String](core_package_structs.md#struct-string) - The converted string.

