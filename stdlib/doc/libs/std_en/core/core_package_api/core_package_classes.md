# Classes

## class ArrayIterator\<T>

```cangjie
public class ArrayIterator<T> <: Iterator<T>
```

Function: An array iterator. For detailed iteration functionality, refer to [Iterable](core_package_interfaces.md#interface-iterablee) and [Iterator](core_package_classes.md#class-iteratort) documentation.

Parent Types:

- [Iterator](#class-iteratort)\<T>

### init(Array\<T>)

```cangjie
public init(data: Array<T>)
```

Function: Creates an iterator for a given [Array](core_package_structs.md#struct-arrayt) instance to traverse all elements in the array.

Parameters:

- data: [Array](core_package_structs.md#struct-arrayt)\<T> - The array instance.

Example:

<!-- verify -->
```cangjie
main() {
    var arr: Array<Int64> = [1, 2, 3]
    // Initialize iterator
    ArrayIterator(arr)
    println("Iterator initialized successfully")
    return 0
}
```

Output:

```text
Iterator initialized successfully
```

### func next()

```cangjie
public func next(): Option<T>
```

Function: Returns the next value in the array iterator.

Returns:

- [Option](core_package_enums.md#enum-optiont)\<T> - The next element in the array iterator, wrapped in [Option](core_package_enums.md#enum-optiont). Returns `None` when iteration reaches the end.

Example:

<!-- verify -->

```cangjie
main() {
    var arr: Array<Int64> = [1, 2, 3, 4]
    var arrIterator: ArrayIterator<Int64> = ArrayIterator(arr)
    var num: Option<Int64>
    while (true) {
        num = arrIterator.next()
        if (let Some(element) <- num) {
            println(element)
        } else {
            break
        }
    }
}
```

Output:

```text
1
2
3
4
```

## class Box\<T>

```cangjie
public class Box<T>
```

Function: The [Box](core_package_classes.md#class-boxt) type provides the ability to add a `class` wrapper layer to other types.

If type `T` itself lacks reference capability (e.g., `struct` types), the wrapped [Box](core_package_classes.md#class-boxt)\<T> type will become referenceable.

Extensions:

- [extend\<T> Box\<T> <: Comparable\<Box\<T>> where T <: Comparable\<T>](core_package_extends.md#extendt-boxt--comparableboxt-where-t--comparablet)
- [extend\<T> Box\<T> <: Hashable where T <: Hashable](core_package_extends.md#extendt-boxt--hashable-where-t--hashable)
- [extend\<T> Box\<T> <: ToString where T <: ToString](core_package_extends.md#extendt-boxt--tostring-where-t--tostring)

### var value

```cangjie
public var value: T
```

Function: Gets or modifies the wrapped value.

Type: T

Example:

<!-- verify -->
```cangjie
main() {
    var box: Box<Int64> = Box<Int64>(42)
    
    // Get wrapped value
    println("Box contains: ${box.value}")
    
    // Modify wrapped value
    box.value = 100
    println("Box now contains: ${box.value}")
}
```

Output:

```text
Box contains: 42
Box now contains: 100
```

### init(T)

```cangjie
public init(v: T)
```

Function: Constructs a corresponding [Box](core_package_classes.md#class-boxt)\<T> instance given a `T` type instance.

Parameters:

- v: T - An instance of any type.

Example:

<!-- verify -->
```cangjie
main() {
    // Create Box instance with integer
    var intBox: Box<Int64> = Box<Int64>(42)
    println("Integer box contains: ${intBox.value}")
    
    // Create Box instance with string
    var stringBox: Box<String> = Box<String>("Hello, Box!")
    println("String box contains: ${stringBox.value}")
}
```

Output:

```text
Integer box contains: 42
String box contains: Hello, Box!
```

## class Future\<T>

```cangjie
public class Future<T>
```

Function: A [Future](core_package_classes.md#class-futuret)\<T> instance represents a Cangjie thread task, which can be used to obtain the computation result of a Cangjie thread or send cancellation signals to it.

The return type of `spawn` expressions is [Future](core_package_classes.md#class-futuret)\<T>, where `T` depends on the return type of the closure within the `spawn` expression.

### prop thread

```cangjie
public prop thread: Thread
```

Function: Gets the [Thread](core_package_classes.md#class-thread) instance corresponding to the Cangjie thread.

Type: [Thread](core_package_classes.md#class-thread)

Example:

<!-- verify -->
```cangjie
main(): Int64 {
    let future: Future<Int64> = spawn {
        =>
        return 42
    }
    
    // Get the Thread instance corresponding to the Future
    let thread: Thread = future.thread
    println("Thread id: ${thread.id}")
    
    // Wait for thread completion
    let result: Int64 = future.get()
    println("Result: ${result}")
    
    return 0
}
```

Output:

```text
Thread id: 2
Result: 42
```

### func cancel()

```cangjie
public func cancel(): Unit
```

Function: Sends a cancellation request to the Cangjie thread corresponding to the current [Future](core_package_classes.md#class-futuret) instance. This method does not immediately stop thread execution but only sends the request. Correspondingly, the `hasPendingCancellation` function of the [Thread](core_package_classes.md#class-thread) class can be used to check if there is a cancellation request. Developers can use this check to decide whether to terminate the thread early and how to do so.

Example:

<!-- verify -->

```cangjie
main(): Unit {
    /* Create thread */
    let future = spawn {
        while (true) {
            if (Thread.currentThread.hasPendingCancellation) {
                return 0
            }
        }
        return 1
    }
    /* Send cancellation request to thread */
    future.cancel()
    let res = future.get()
    println(res)
}
```

Output:

```text
0
```

### func get()

```cangjie
public func get(): T
```

Function: Blocks the current thread, waits for, and retrieves the result of the thread corresponding to the current [Future](core_package_classes.md#class-futuret)\<T> object.

Returns:

- T - The return value of the thread represented by the current [Future](core_package_classes.md#class-futuret)\<T> instance after execution completes.

Example:

<!-- verify -->

```cangjie
main(): Int64 {
    let fut: Future<Int64> = spawn {
        =>
        sleep(1000 * Duration.millisecond) /* Sleep for 1 second */
        return 1
    }

    /* Wait for thread completion */
    let result: Int64 = fut.get()
    println(result)
    return 0
}
```

Output:

```text
1
```

### func get(Duration)

```cangjie
public func get(timeout: Duration): T
```

Function: Blocks the current thread, waits for a specified duration, and retrieves the return value of the thread corresponding to the current [Future](core_package_classes.md#class-futuret)\<T> object.

A timeout must be specified. If the corresponding thread does not complete execution within the specified time, this function throws a [TimeoutException](./core_package_exceptions.md#class-timeoutexception). If timeout <= Duration.Zero, it is equivalent to get(), meaning no waiting limit. If the thread throws an exception during execution, the exception will be rethrown at the get() call site.

Parameters:

- timeout: [Duration](./core_package_structs.md#struct-duration) - The waiting time.

Returns:

- T - The execution result of the Cangjie thread after the specified duration.

Exceptions:

- [TimeoutException](./core_package_exceptions.md#class-timeoutexception) - Thrown if the corresponding thread does not complete execution within the specified time.

Example:

<!-- verify -->

```cangjie
main(): Int64 {
    let fut: Future<Int64> = spawn {
        =>
        sleep(1000 * Duration.millisecond) /* Sleep for 1 second */
        return 1
    }

    let result: Int64 = fut.get(2000 * Duration.millisecond)
    /* Maximum wait time is 2 seconds; TimeoutException is thrown if exceeded */

    println(result)
    return 0
}
```

Output:

```text
1
```

### func tryGet()

```cangjie
public func tryGet(): Option<T>
```

Function: Attempts to retrieve the execution result without blocking the current thread. Returns `None` if the corresponding thread has not completed.

Returns:

- [Option](core_package_enums.md#enum-optiont)\<T> - Returns `None` if the Cangjie thread has not completed; otherwise, returns the execution result.

Example:

<!-- verify -->

```cangjie
main(): Int64 {
    let fut: Future<Int64> = spawn {
        =>
        sleep(1000 * Duration.millisecond) /* Sleep for 1 second */
        return 1
    }

    /* Main thread waits 4 seconds to ensure the spawned thread completes */
    sleep(4000 * Duration.millisecond)

    /* Attempt to retrieve the spawned thread's result */
    let result: Option<Int64> = fut.tryGet()
    println(result)
    return 0
}
```

Output:

```text
Some(1)
```

## class Iterator\<T>

```cangjie
public abstract class Iterator<T> <: Iterable<T>
```

Function: This class represents an iterator, providing the `next` method to support iterative traversal over container elements.

Parent Type:

- [Iterable](core_package_interfaces.md#interface-iterablee)\<T>

Extensions:

- [extend\<T> Iterator\<T>](core_package_extends.md#extendt-iteratort)
- [extend\<T> Iterator\<T> where T <: Comparable\<T>](core_package_extends.md#extendt-iteratort-where-t--comparablet)
- [extend\<T> Iterator\<T> where T <: Equatable\<T>](core_package_extends.md#extendt-iteratort-where-t--equatablet)

### init()

```cangjie
public init()
```

Function: Constructs a default [Iterator](core_package_classes.md#class-iteratort)\<T> object.

Example:

<!-- verify -->
```cangjie
main(): Int64 {
    // The abstract class Iterator cannot be instantiated directly
    MyIterator<Int64>()
    println("Abstract subclass initialized successfully")
    return 0
}

public class MyIterator<T> <: Iterator<T> {
    public init() {}
    public func next(): ?T {
        return None<T>
    }
}
```

Output:

```text
Abstract subclass initialized successfully
```

### func iterator()

```cangjie
public func iterator(): Iterator<T>
```

Function: Returns the iterator itself.

Returns:

- [Iterator](core_package_classes.md#class-iteratort)\<T> - The iterator itself.

Example:

<!-- verify -->
```cangjie
main(): Int64 {
    // Create an array and get its iterator
    var arr: Array<Int64> = [10, 20, 30]
    var iter: Iterator<Int64> = arr.iterator()
    
    // Call iterator() to get the iterator itself
    var iter2: Iterator<Int64> = iter.iterator()
    
    // Verify both iterators are the same object
    println("Iterators are the same object")
    
    // Traverse elements using both iterators
    println("First element from iter: ${iter.next()}")
    println("Second element from iter2: ${iter2.next()}")
    println("Third element from iter: ${iter.next()}")
    println("Fourth element from iter2: ${iter2.next()}")
    
    return 0
}
```

Output:

```text
Iterators are the same object
First element from iter: Some(10)
Second element from iter2: Some(20)
Third element from iter: Some(30)
Fourth element from iter2: None
```

### func next()

```cangjie
public func next(): Option<T>
```

Function: Retrieves the next element during iteration.

Returns:

- [Option](core_package_enums.md#enum-optiont)\<T> - The next element during iteration.

Example:

<!-- verify -->

```cangjie
main(): Int64 {
    var arr: Array<Int64> = [1, 2, 3, 4, 5]
    var iter = arr.iterator() /* Get the container's iterator object */

    while (true) { /* Traverse using the iterator */
        match (iter.next()) {
            case Some(i) => println(i)
            case None => break
        }
    }
    return 0
}
```

Output:

```text
1
2
3
4
5
```

## class Object

```cangjie
public open class Object <: Any
```

Function: [Object](core_package_classes.md#class-object) is the parent class of all `class` types, and all `class` types inherit from it by default. The [Object](core_package_classes.md#class-object) class contains no members, making it an "empty" class.

Parent type:

- [Any](core_package_interfaces.md#interface-any)

### init()

```cangjie
public const init()
```

Function: Constructs an `object` instance.

Example:

<!-- run -->
```cangjie
main(): Int64 {
    Object()
    return 0
}
```

## class RangeIterator\<T> <: Iterator\<T> where T <: Countable\<T> & Comparable\<T> & Equatable\<T>

```cangjie
public class RangeIterator<T> <: Iterator<T> where T <: Countable<T> & Comparable<T> & Equatable<T>
```

Function: Iterator for the [Range](core_package_structs.md#struct-ranget-where-t--countablet--comparablet--equatablet) type. For detailed iteration functionality, refer to the [Iterable](core_package_interfaces.md#interface-iterablee) and [Iterator](core_package_classes.md#class-iteratort) interface descriptions.

Parent type:

- [Iterator](#class-iteratort)\<T>

### func next()

```cangjie
public func next(): Option<T>
```

Function: Gets the next value in the [Range](core_package_structs.md#struct-ranget-where-t--countablet--comparablet--equatablet) iterator.

Returns:

- [Option](core_package_enums.md#enum-optiont)\<T> - The next member in the [Range](core_package_structs.md#struct-ranget-where-t--countablet--comparablet--equatablet) iterator, encapsulated in [Option](core_package_enums.md#enum-optiont). Returns `None` when iteration reaches the end.

Example:

<!-- verify -->
```cangjie
main(): Int64 {
    // Create a Range and get its iterator
    let range: Range<Int64> = 1..=5
    var iter = range.iterator()
    
    // Use iterator to traverse all values in the Range
    println("Iterating through range 1..=5:")
    while (true) {
        match (iter.next()) {
            case Some(value) => println(value)
            case None => break
        }
    }
    
    return 0
}
```

Execution result:

```text
Iterating through range 1..=5:
1
2
3
4
5
```

## class StackTraceElement

```cangjie
public open class StackTraceElement <: ToString
```

Function: Represents specific information about an exception stack trace, including the class name, method name, file name, and line number where the exception occurred.

Parent type:

- [ToString](core_package_interfaces.md#interface-tostring)

### let declaringClass

```cangjie
public let declaringClass: String
```

Function: Gets the class name where the exception occurred.

Type: [String](core_package_structs.md#struct-string)

Example:

<!-- verify -->
```cangjie
main(): Int64 {
    // Create a StackTraceElement instance
    let stackTraceElement = StackTraceElement(
        "MyClass", 
        "myMethod", 
        "MyClass.cj", 
        42
    )
    
    // Get and print the class name
    let className: String = stackTraceElement.declaringClass
    println("Class name: ${className}")
    
    return 0
}
```

Execution result:

```text
Class name: MyClass
```

### let fileName

```cangjie
public let fileName: String
```

Function: Gets the file name where the exception occurred.

Type: [String](core_package_structs.md#struct-string)

Example:

<!-- verify -->
```cangjie
main(): Int64 {
    // Create a StackTraceElement instance
    let stackTraceElement = StackTraceElement(
        "MyClass", 
        "myMethod", 
        "MyClass.cj", 
        42
    )
    
    // Get and print the file name
    let fileName: String = stackTraceElement.fileName
    println("File name: ${fileName}")
    
    return 0
}
```

Execution result:

```text
File name: MyClass.cj
```

### let lineNumber

```cangjie
public let lineNumber: Int64
```

Function: Gets the line number where the exception occurred.

Type: [Int64](core_package_intrinsics.md#int64)

Example:

<!-- verify -->
```cangjie
main(): Int64 {
    // Create a StackTraceElement instance
    let stackTraceElement = StackTraceElement(
        "MyClass", 
        "myMethod", 
        "MyClass.cj", 
        42
    )
    
    // Get and print the line number
    let lineNum: Int64 = stackTraceElement.lineNumber
    println("Line number: ${lineNum}")
    
    return 0
}
```

Execution result:

```text
Line number: 42
```

### let methodName

```cangjie
public let methodName: String
```

Function: Gets the method name where the exception occurred.

Type: [String](core_package_structs.md#struct-string)

Example:

<!-- verify -->
```cangjie
main(): Int64 {
    // Create a StackTraceElement instance
    let stackTraceElement = StackTraceElement(
        "MyClass", 
        "myMethod", 
        "MyClass.cj", 
        42
    )
    
    // Get and print the method name
    let methodName: String = stackTraceElement.methodName
    println("Method name: ${methodName}")
    
    return 0
}
```

Execution result:

```text
Method name: myMethod
```

### init(String, String, String, Int64)

```cangjie
public init(declaringClass: String, methodName: String, fileName: String, lineNumber: Int64)
```

Function: Constructs a stack trace instance with the specified class name, method name, file name, and line number.

Parameters:

- declaringClass: [String](core_package_structs.md#struct-string) - The class name.
- methodName: [String](core_package_structs.md#struct-string) - The method name.
- fileName: [String](core_package_structs.md#struct-string) - The file name.
- lineNumber: [Int64](core_package_intrinsics.md#int64) - The line number.

Example:

<!-- verify -->
```cangjie
main(): Int64 {
    // Create a StackTraceElement instance
    let stackTraceElement = StackTraceElement(
        "MyClass", 
        "myMethod", 
        "MyClass.cj", 
        42
    )
    
    // Print stack trace element information
    println("Class: ${stackTraceElement.declaringClass}")
    println("Method: ${stackTraceElement.methodName}")
    println("File: ${stackTraceElement.fileName}")
    println("Line: ${stackTraceElement.lineNumber}")
    
    return 0
}
```

Execution result:

```text
Class: MyClass
Method: myMethod
File: MyClass.cj
Line: 42
```

### func toString()

```cangjie
public func toString(): String
```

Function: Gets the string representation of the [StackTraceElement](core_package_classes.md#class-stacktraceelement) object.

Returns:

- [String](core_package_structs.md#struct-string) - The converted string.

Example:

<!-- verify -->
```cangjie
main(): Int64 {
    // Create a StackTraceElement instance
    let stackTraceElement = StackTraceElement(
        "MyClass", 
        "myMethod", 
        "MyClass.cj", 
        42
    )
    
    // Directly print
    println(stackTraceElement)
    return 0
}
```

Execution result:

```text
MyClass.myMethod(MyClass.cj:42)
```

## class StringBuilder

```cangjie
public class StringBuilder <: ToString
```

Function: This class is primarily used for string construction.

[StringBuilder](core_package_classes.md#class-stringbuilder) is more efficient than [String](core_package_structs.md#struct-string) for string construction:

- Functionally supports input of multiple value types, which are automatically converted to [String](core_package_structs.md#struct-string) type objects and appended to the constructed string.
- Performance-wise, it uses a dynamic expansion algorithm to reduce memory allocation frequency, resulting in faster string construction and typically lower memory resource usage.

> **Note:**
>
> [StringBuilder](core_package_classes.md#class-stringbuilder) only supports UTF-8 encoded character data.

Parent Type:

- [ToString](core_package_interfaces.md#interface-tostring)

### prop capacity

```cangjie
public prop capacity: Int64
```

Function: Gets the current capacity of the [StringBuilder](core_package_classes.md#class-stringbuilder) instance to hold strings. This value increases as expansion occurs.

Type: [Int64](core_package_intrinsics.md#int64)

Example:

<!-- verify -->
```cangjie
main(): Int64 {
    // Create a StringBuilder instance
    var sb = StringBuilder()
    
    // Get initial capacity
    let initialCapacity: Int64 = sb.capacity
    println("Initial capacity: ${initialCapacity}")
    
    // Append some content
    sb.append("Hello, World!")
    
    // Get capacity after appending
    let currentCapacity: Int64 = sb.capacity
    println("Current capacity: ${currentCapacity}")
    
    return 0
}
```

Output:

```text
Initial capacity: 32
Current capacity: 32
```

### prop size

```cangjie
public prop size: Int64
```

Function: Gets the length of the string in the [StringBuilder](core_package_classes.md#class-stringbuilder) instance.

Type: [Int64](core_package_intrinsics.md#int64)

Example:

<!-- verify -->
```cangjie
main(): Int64 {
    // Create a StringBuilder instance
    var sb = StringBuilder()
    
    // Get initial size
    let initialSize: Int64 = sb.size
    println("Initial size: ${initialSize}")
    
    // Append some content
    sb.append("Hello")
    
    // Get size after appending
    let currentSize: Int64 = sb.size
    println("Current size: ${currentSize}")
    
    return 0
}
```

Output:

```text
Initial size: 0
Current size: 5
```

### init()

```cangjie
public init()
```

Function: Constructs an empty [StringBuilder](core_package_classes.md#class-stringbuilder) instance with an initial capacity of 32.

Example:

<!-- verify -->
```cangjie
main(): Int64 {
    // Create a StringBuilder instance using default constructor
    var sb = StringBuilder()
    
    // Verify initial state
    println("Initial capacity: ${sb.capacity}")
    println("Initial size: ${sb.size}")
    println("Initial content: '${sb}'")
    
    return 0
}
```

Output:

```text
Initial capacity: 32
Initial size: 0
Initial content: ''
```

### init(Array\<Rune>)

```cangjie
public init(value: Array<Rune>)
```

Function: Initializes a [StringBuilder](core_package_classes.md#class-stringbuilder) instance with the character array specified by parameter `value`. The instance's initial capacity is the size of `value`, and its initial content is the characters contained in `value`.

Parameters:

- value: [Array](core_package_structs.md#struct-arrayt)\<Rune> - The character array used to initialize the [StringBuilder](core_package_classes.md#class-stringbuilder) instance.

Example:

<!-- verify -->
```cangjie
main(): Int64 {
    // Create a Rune array
    let runes: Array<Rune> = [r'H', r'e', r'l', r'l', r'o']
    
    // Create a StringBuilder instance using the Rune array
    var sb = StringBuilder(runes)
    
    // Verify initialization result
    println("Capacity: ${sb.capacity}")
    println("Size: ${sb.size}")
    println("Content: '${sb}'")
    
    return 0
}
```

Output:

```text
Capacity: 37
Size: 5
Content: 'Hello'
```

### init(Int64)

```cangjie
public init(capacity: Int64)
```

Function: Initializes an empty [StringBuilder](core_package_classes.md#class-stringbuilder) instance with the capacity specified by parameter `capacity`. The instance's initial capacity is the size of `value`, and its initial content consists of several `\0` characters.

Parameters:

- capacity: [Int64](core_package_intrinsics.md#int64) - The byte capacity for initializing the [StringBuilder](core_package_classes.md#class-stringbuilder) instance. Valid range is (0, [Int64.Max](./core_package_intrinsics.md#static-prop-max-5)].

Exceptions:

- [IllegalArgumentException](core_package_exceptions.md#class-illegalargumentexception) - Thrown when parameter `capacity` is less than or equal to 0.

Example:

<!-- verify -->
```cangjie
main(): Int64 {
    // Create a StringBuilder instance with specified capacity
    var sb = StringBuilder(100)
    
    // Verify initialization result
    println("Capacity: ${sb.capacity}")
    println("Size: ${sb.size}")
    println("Content: '${sb}'")
    
    // Append content
    sb.append("Hello, World!")
    println("After append - Size: ${sb.size}")
    println("After append - Content: '${sb}'")
    
    return 0
}
```

Output:

```text
Capacity: 100
Size: 0
Content: ''
After append - Size: 13
After append - Content: 'Hello, World!'
```

### init(Rune, Int64)

```cangjie
public init(r: Rune, n: Int64)
```

Function: Initializes a [StringBuilder](core_package_classes.md#class-stringbuilder) instance with `n` instances of character `r`. The instance's initial capacity is `n`, and its initial content consists of `n` instances of character `r`.

Parameters:

- r: Rune - The character used to initialize the [StringBuilder](core_package_classes.md#class-stringbuilder) instance.
- n: [Int64](core_package_intrinsics.md#int64) - The number of character `r`. Valid range is [0, [Int64.Max](./core_package_intrinsics.md#static-prop-max-5)].

Exceptions:

- [IllegalArgumentException](core_package_exceptions.md#class-illegalargumentexception) - Thrown when parameter `n` is less than 0.

Example:

<!-- verify -->
```cangjie
main(): Int64 {
    // Create a StringBuilder instance with 5 '*' characters
    var sb = StringBuilder(r'*', 5)
    
    // Verify initialization result
    println("Capacity: ${sb.capacity}")
    println("Size: ${sb.size}")
    println("Content: '${sb}'")
    
    // Append more content
    sb.append("Hello")
    println("After append - Size: ${sb.size}")
    println("After append - Content: '${sb}'")
    
    return 0
}
```

Output:

```text
Capacity: 37
Size: 5
Content: '*****'
After append - Size: 10
After append - Content: '*****Hello'
```

### init(String)

```cangjie
public init(str: String)
```

Function: Constructs a [StringBuilder](core_package_classes.md#class-stringbuilder) instance with the specified initial string. The instance's initial capacity is the size of the specified string, and its initial content is the specified string.

Parameters:

- str: [String](core_package_structs.md#struct-string) - The string used to initialize the [StringBuilder](core_package_classes.md#class-stringbuilder) instance.

Example:

<!-- verify -->
```cangjie
main(): Int64 {
    // Create a StringBuilder instance with initial string
    var sb = StringBuilder("Hello, World!")
    
    // Verify initialization result
    println("Capacity: ${sb.capacity}")
    println("Size: ${sb.size}")
    println("Content: '${sb}'")
    
    // Append more content
    sb.append(" Welcome!")
    println("After append - Size: ${sb.size}")
    println("After append - Content: '${sb}'")
    
    return 0
}
```

Output:

```text
Capacity: 45
Size: 13
Content: 'Hello, World!'
After append - Size: 22
After append - Content: 'Hello, World! Welcome!'
```

### func append(Array\<Rune>)

```cangjie
public func append(runeArr: Array<Rune>): Unit
```

Function: Appends all characters from a `Rune` array to the end of the [StringBuilder](core_package_classes.md#class-stringbuilder).

Parameters:

- runeArr: [Array](core_package_structs.md#struct-arrayt)\<Rune> - The `Rune` array to append.

Example:

<!-- verify -->
```cangjie
main(): Int64 {
    // Create a StringBuilder instance
    var sb = StringBuilder("Hello")
    
    // Create a Rune array
    let runes: Array<Rune> = [r' ', r'W', r'o', r'r', r'l', r'd']
    
    // Append the Rune array to StringBuilder
    sb.append(runes)
    
    // Verify result
    println("Content: '${sb}'")
    println("Size: ${sb.size}")
    
    return 0
}
```

Output:

```text
Content: 'Hello World'
Size: 11
```

### func append\<T>(Array\<T>) where T <: ToString

```cangjie
public func append<T>(val: Array<T>): Unit where T <: ToString
```

Function: Appends the string representation of the [Array](core_package_structs.md#struct-arrayt)\<T> specified by parameter `val` to the end of the [StringBuilder](core_package_classes.md#class-stringbuilder). Type `T` must implement the [ToString](core_package_interfaces.md#interface-tostring) interface.

Parameters:

- val: [Array](core_package_structs.md#struct-arrayt)\<T> - The [Array](core_package_structs.md#struct-arrayt)\<T> instance to append.

Example:

<!-- verify -->
```cangjie
main(): Int64 {
    // Create a StringBuilder instance
    var sb = StringBuilder("Numbers: ")
    
    // Create an integer array
    let numbers: Array<Int64> = [1, 2, 3, 4, 5]
    
    // Append the integer array to StringBuilder
    sb.append(numbers)
    
    // Verify result
    println("Content: '${sb}'")
    println("Size: ${sb.size}")
    
    return 0
}
```

Output:

```text
Content: 'Numbers: 12345'
Size: 14
```

### func append(Bool)

```cangjie
public func append(b: Bool): Unit
```

Function: Appends the string representation of parameter `b` to the end of [StringBuilder](core_package_classes.md#class-stringbuilder).

Parameters:

- b: [Bool](core_package_intrinsics.md#bool) - The [Bool](core_package_intrinsics.md#bool) value to append.

Example:

<!-- verify -->
```cangjie
main(): Int64 {
    // Create a StringBuilder instance
    var sb = StringBuilder("Boolean values: ")
    
    // Append boolean values
    sb.append(true)
    sb.append(" and ")
    sb.append(false)
    
    // Verify results
    println("Content: '${sb}'")
    println("Size: ${sb.size}")
    
    return 0
}
```

Execution Result:

```text
Content: 'Boolean values: true and false'
Size: 30
```

### func append(CString)

```cangjie
public func append(cstr: CString): Unit
```

Function: Appends the content specified by parameter `cstr` from [CString](core_package_intrinsics.md#cstring) to the end of [StringBuilder](core_package_classes.md#class-stringbuilder).

Parameters:

- cstr: [CString](core_package_intrinsics.md#cstring) - The [CString](core_package_intrinsics.md#cstring) to append.

Example:

<!-- verify -->
```cangjie
main(): Int64 {
    // Create a StringBuilder instance
    var sb = StringBuilder("Message: ")
    
    // Create a CString
    let cstr: CString = unsafe { LibC.mallocCString("Hello from C string") }
    
    // Append CString
    sb.append(cstr)
    
    unsafe { LibC.free(cstr) }
    // Verify results
    println("Content: '${sb}'")
    println("Size: ${sb.size}")
    
    return 0
}
```

Execution Result:

```text
Content: 'Message: Hello from C string'
Size: 28
```

### func append(Float16)

```cangjie
public func append(n: Float16): Unit
```

Function: Appends the string representation of parameter `n` to the end of [StringBuilder](core_package_classes.md#class-stringbuilder).

Parameters:

- n: [Float16](core_package_intrinsics.md#float16) - The [Float16](core_package_intrinsics.md#float16) value to append.

Example:

<!-- verify -->
```cangjie
main(): Int64 {
    // Create a StringBuilder instance
    var sb = StringBuilder("Float16 value: ")
    
    // Append Float16 value
    sb.append(3.14f16)
    
    // Verify results
    println("Content: '${sb}'")
    println("Size: ${sb.size}")
    
    return 0
}
```

Execution Result:

```text
Content: 'Float16 value: 3.140625'
Size: 23
```

### func append(Float32)

```cangjie
public func append(n: Float32): Unit
```

Function: Appends the string representation of parameter `n` to the end of [StringBuilder](core_package_classes.md#class-stringbuilder).

Parameters:

- n: [Float32](core_package_intrinsics.md#float32) - The [Float32](core_package_intrinsics.md#float32) value to append.

Example:

<!-- verify -->
```cangjie
main(): Int64 {
    // Create a StringBuilder instance
    var sb = StringBuilder("Float32 value: ")
    
    // Append Float32 value
    sb.append(3.14159f32)
    
    // Verify results
    println("Content: '${sb}'")
    println("Size: ${sb.size}")
    
    return 0
}
```

Execution Result:

```text
Content: 'Float32 value: 3.141590'
Size: 23
```

### func append(Float64)

```cangjie
public func append(n: Float64): Unit
```

Function: Appends the string representation of parameter `n` to the end of [StringBuilder](core_package_classes.md#class-stringbuilder).

Parameters:

- n: [Float64](core_package_intrinsics.md#float64) - The [Float64](core_package_intrinsics.md#float64) value to append.

Example:

<!-- verify -->
```cangjie
main(): Int64 {
    // Create a StringBuilder instance
    var sb = StringBuilder("Float64 value: ")
    
    // Append Float64 value
    sb.append(3.141592653589793)
    
    // Verify results
    println("Content: '${sb}'")
    println("Size: ${sb.size}")
    
    return 0
}
```

Execution Result:

```text
Content: 'Float64 value: 3.141593'
Size: 23
```

### func append(Int16)

```cangjie
public func append(n: Int16): Unit
```

Function: Appends the string representation of parameter `n` to the end of [StringBuilder](core_package_classes.md#class-stringbuilder).

Parameters:

- n: [Int16](core_package_intrinsics.md#int16) - The [Int16](core_package_intrinsics.md#int16) value to append.

Example:

<!-- verify -->
```cangjie
main(): Int64 {
    // Create a StringBuilder instance
    var sb = StringBuilder("Int16 value: ")
    
    // Append Int16 value
    sb.append(12345i16)
    
    // Verify results
    println("Content: '${sb}'")
    println("Size: ${sb.size}")
    
    return 0
}
```

Execution Result:

```text
Content: 'Int16 value: 12345'
Size: 18
```

### func append(Int32)

```cangjie
public func append(n: Int32): Unit
```

Function: Appends the string representation of parameter `n` to the end of [StringBuilder](core_package_classes.md#class-stringbuilder).

Parameters:

- n: [Int32](core_package_intrinsics.md#int32) - The [Int32](core_package_intrinsics.md#int32) value to append.

Example:

<!-- verify -->
```cangjie
main(): Int64 {
    // Create a StringBuilder instance
    var sb = StringBuilder("Int32 value: ")
    
    // Append Int32 value
    sb.append(1234567890i32)
    
    // Verify results
    println("Content: '${sb}'")
    println("Size: ${sb.size}")
    
    return 0
}
```

Execution Result:

```text
Content: 'Int32 value: 1234567890'
Size: 23
```

### func append(Int64)

```cangjie
public func append(n: Int64): Unit
```

Function: Appends the string representation of parameter `n` to the end of [StringBuilder](core_package_classes.md#class-stringbuilder).

Parameters:

- n: [Int64](core_package_intrinsics.md#int64) - The [Int64](core_package_intrinsics.md#int64) value to append.

Example:

<!-- verify -->
```cangjie
main(): Int64 {
    // Create a StringBuilder instance
    var sb = StringBuilder("Int64 value: ")
    
    // Append Int64 value
    sb.append(123456789012345)
    
    // Verify results
    println("Content: '${sb}'")
    println("Size: ${sb.size}")
    
    return 0
}
```

Execution Result:

```text
Content: 'Int64 value: 123456789012345'
Size: 28
```

### func append(Int8)

```cangjie
public func append(n: Int8): Unit
```

Function: Appends the string representation of parameter `n` to the end of [StringBuilder](core_package_classes.md#class-stringbuilder).

Parameters:

- n: [Int8](core_package_intrinsics.md#int8) - The [Int8](core_package_intrinsics.md#int8) value to append.

Example:

<!-- verify -->
```cangjie
main(): Int64 {
    // Create a StringBuilder instance
    var sb = StringBuilder("Int8 value: ")
    
    // Append Int8 value
    sb.append(123i8)
    
    // Verify results
    println("Content: '${sb}'")
    println("Size: ${sb.size}")
    
    return 0
}
```

Execution Result:

```text
Content: 'Int8 value: 123'
Size: 15
```

### func append(Rune)

```cangjie
public func append(r: Rune): Unit
```

Function: Appends the character specified by parameter `r` to the end of [StringBuilder](core_package_classes.md#class-stringbuilder).

Parameters:

- r: Rune - The character to append.

Example:

<!-- verify -->
```cangjie
main(): Int64 {
    // Create a StringBuilder instance
    var sb = StringBuilder("Hello")
    
    // Append a character
    sb.append(r'!')
    
    // Verify results
    println("Content: '${sb}'")
    println("Size: ${sb.size}")
    
    return 0
}
```

Execution Result:

```text
Content: 'Hello!'
Size: 6
```

### func append(String)

```cangjie
public func append(str: String): Unit
```

Function: Appends the string specified by parameter `str` to the end of [StringBuilder](core_package_classes.md#class-stringbuilder).

Parameters:

- str: [String](core_package_structs.md#struct-string) - The string to append.

Example:

<!-- verify -->
```cangjie
main(): Int64 {
    // Create a StringBuilder instance
    var sb = StringBuilder("Hello")
    
    // Append a string
    sb.append(", World!")
    
    // Verify results
    println("Content: '${sb}'")
    println("Size: ${sb.size}")
    
    return 0
}
```

Execution Result:

```text
Content: 'Hello, World!'
Size: 13
```

### func append(StringBuilder)

```cangjie
public func append(sb: StringBuilder): Unit
```

Function: Appends the content of the specified [StringBuilder](core_package_classes.md#class-stringbuilder) parameter `sb` to the end of this [StringBuilder](core_package_classes.md#class-stringbuilder).

Parameters:

- sb: [StringBuilder](core_package_classes.md#class-stringbuilder) - The [StringBuilder](core_package_classes.md#class-stringbuilder) instance to append.

Example:

<!-- verify -->
```cangjie
main(): Int64 {
    // Create two StringBuilder instances
    var sb1 = StringBuilder("Hello")
    var sb2 = StringBuilder(", World!")
    
    // Append sb2's content to sb1
    sb1.append(sb2)
    
    // Verify the result
    println("Content: '${sb1}'")
    println("Size: ${sb1.size}")
    
    return 0
}
```

Output:

```text
Content: 'Hello, World!'
Size: 13
```

### func append\<T>(T) where T <: ToString

```cangjie
public func append<T>(v: T): Unit where T <: ToString
```

Function: Appends the string representation of the parameter `v` of type `T` to the end of this [StringBuilder](core_package_classes.md#class-stringbuilder). Type `T` must implement the [ToString](core_package_interfaces.md#interface-tostring) interface.

Parameters:

- v: T - The instance of type `T` to append.

Example:

<!-- verify -->
```cangjie
main(): Int64 {
    // Create a StringBuilder instance
    var sb = StringBuilder("The answer is: ")
    
    // Append an integer (implements ToString interface)
    sb.append(42)
    
    // Verify the result
    println("Content: '${sb}'")
    println("Size: ${sb.size}")
    
    return 0
}
```

Output:

```text
Content: 'The answer is: 42'
Size: 17
```

### func append(UInt16)

```cangjie
public func append(n: UInt16): Unit
```

Function: Appends the string representation of the parameter `n` to the end of this [StringBuilder](core_package_classes.md#class-stringbuilder).

Parameters:

- n: [UInt16](core_package_intrinsics.md#uint16) - The [UInt16](core_package_intrinsics.md#uint16) value to append.

Example:

<!-- verify -->
```cangjie
main(): Int64 {
    // Create a StringBuilder instance
    var sb = StringBuilder("UInt16 value: ")
    
    // Append a UInt16 value
    sb.append(12345u16)
    
    // Verify the result
    println("Content: '${sb}'")
    println("Size: ${sb.size}")
    
    return 0
}
```

Output:

```text
Content: 'UInt16 value: 12345'
Size: 19
```

### func append(UInt32)

```cangjie
public func append(n: UInt32): Unit
```

Function: Appends the string representation of the parameter `n` to the end of this [StringBuilder](core_package_classes.md#class-stringbuilder).

Parameters:

- n: [UInt32](core_package_intrinsics.md#uint32) - The [UInt32](core_package_intrinsics.md#uint32) value to append.

Example:

<!-- verify -->
```cangjie
main(): Int64 {
    // Create a StringBuilder instance
    var sb = StringBuilder("UInt32 value: ")
    
    // Append a UInt32 value
    sb.append(1234567890u32)
    
    // Verify the result
    println("Content: '${sb}'")
    println("Size: ${sb.size}")
    
    return 0
}
```

Output:

```text
Content: 'UInt32 value: 1234567890'
Size: 24
```

### func append(UInt64)

```cangjie
public func append(n: UInt64): Unit
```

Function: Appends the string representation of the parameter `n` to the end of this [StringBuilder](core_package_classes.md#class-stringbuilder).

Parameters:

- n: [UInt64](core_package_intrinsics.md#uint64) - The [UInt64](core_package_intrinsics.md#uint64) value to append.

Example:

<!-- verify -->
```cangjie
main(): Int64 {
    // Create a StringBuilder instance
    var sb = StringBuilder("UInt64 value: ")
    
    // Append a UInt64 value
    sb.append(123456789012345u64)
    
    // Verify the result
    println("Content: '${sb}'")
    println("Size: ${sb.size}")
    
    return 0
}
```

Output:

```text
Content: 'UInt64 value: 123456789012345'
Size: 29
```

### func append(UInt8)

```cangjie
public func append(n: UInt8): Unit
```

Function: Appends the string representation of the parameter `n` to the end of this [StringBuilder](core_package_classes.md#class-stringbuilder).

Parameters:

- n: [UInt8](core_package_intrinsics.md#uint8) - The [UInt8](core_package_intrinsics.md#uint8) value to append.

Example:

<!-- verify -->
```cangjie
main(): Int64 {
    // Create a StringBuilder instance
    var sb = StringBuilder("UInt8 value: ")
    
    // Append a UInt8 value
    sb.append(123u8)
    
    // Verify the result
    println("Content: '${sb}'")
    println("Size: ${sb.size}")
    
    return 0
}
```

Output:

```text
Content: 'UInt8 value: 123'
Size: 16
```

### func appendFromUtf8(Array\<Byte>)

```cangjie
public func appendFromUtf8(arr: Array<Byte>): Unit
```

Function: Appends the byte array pointed to by parameter `arr` to the end of this [StringBuilder](core_package_classes.md#class-stringbuilder).

This function requires the parameter `arr` to be UTF-8 encoded. If not, an exception will be thrown.

Parameters:

- arr: [Array](core_package_structs.md#struct-arrayt)\<[Byte](core_package_types.md#type-byte)> - The byte array to append.

Exceptions:

- [IllegalArgumentException](core_package_exceptions.md#class-illegalargumentexception) - Thrown when the byte array does not conform to UTF-8 encoding rules.

Example:

<!-- verify -->
```cangjie
main(): Int64 {
    // Create a StringBuilder instance
    var sb = StringBuilder("Hello, ")
    
    // Create a UTF-8 encoded byte array (ASCII codes for "World")
    let utf8Bytes: Array<Byte> = [87, 111, 114, 108, 100] // ASCII for "World"
    
    // Append the UTF-8 byte array
    sb.appendFromUtf8(utf8Bytes)
    
    // Verify the result
    println("Content: '${sb}'")
    println("Size: ${sb.size}")
    
    return 0
}
```

Output:

```text
Content: 'Hello, World'
Size: 12
```

### func appendFromUtf8Unchecked(Array\<Byte>)

```cangjie
public unsafe func appendFromUtf8Unchecked(arr: Array<Byte>): Unit
```

Function: Appends the byte array pointed to by parameter `arr` to the end of this [StringBuilder](core_package_classes.md#class-stringbuilder).

Unlike the `appendFromUtf8` function, this does not perform UTF-8 encoding checks on the byte array. Therefore, the resulting string is not guaranteed to be valid and may cause unexpected exceptions. Unless performance is critical, prefer using the safer `appendFromUtf8` function.

Parameters:

- arr: [Array](core_package_structs.md#struct-arrayt)\<[Byte](core_package_types.md#type-byte)> - The byte array to append.

Example:

<!-- verify -->
```cangjie
main(): Int64 {
    // Create a StringBuilder instance
    var sb = StringBuilder("Hello, ")
    
    // Create a byte array (ASCII codes for "World")
    let bytes: Array<Byte> = [87, 111, 114, 108, 100] // ASCII for "World"
    
    // Append the byte array (without UTF-8 check)
    unsafe { sb.appendFromUtf8Unchecked(bytes) }
    
    // Verify the result
    println("Content: '${sb}'")
    println("Size: ${sb.size}")
    
    return 0
}
```

Output:

```text
Content: 'Hello, World'
Size: 12
```

### func reserve(Int64)

```cangjie
public func reserve(additional: Int64): Unit
```

Function: Expands the capacity of this [StringBuilder](core_package_classes.md#class-stringbuilder) by `additional` size.

No expansion occurs if `additional` is less than or equal to zero, or if the remaining capacity is greater than or equal to `additional`. When the remaining capacity is less than `additional`, the capacity is expanded to the maximum of either 1.5 times the current capacity (rounded down) or `size` + `additional`.

Parameters:

- additional: [Int64](core_package_intrinsics.md#int64) - The additional capacity to reserve.

Example:

<!-- verify -->
```cangjie
main(): Int64 {
    // Create a StringBuilder instance
    var sb = StringBuilder("Hello")
    
    // Check initial capacity and size
    println("Initial capacity: ${sb.capacity}")
    println("Initial size: ${sb.size}")
    
    // Reserve additional space
    sb.reserve(100)
    
    // Check capacity after reservation
    println("Capacity after reserve: ${sb.capacity}")
    println("Size after reserve: ${sb.size}")
    
    return 0
}
```

Output:

```text
Initial capacity: 37
Initial size: 5
Capacity after reserve: 105
Size after reserve: 5
```

### func reset(Option\<Int64>)

```cangjie
public func reset(capacity!: Option<Int64> = None): Unit
```

Function: Clears the current [StringBuilder](core_package_classes.md#class-stringbuilder) and resets its capacity to the value specified by `capacity`.

Parameters:

- capacity!: [Option](core_package_enums.md#enum-optiont)\<[Int64](core_package_intrinsics.md#int64)> - The capacity to reset the [StringBuilder](core_package_classes.md#class-stringbuilder) instance to. Valid values are `None` and (`Some(0)`, `Some(Int64.Max)`]. Default `None` uses the default capacity (32).

Exceptions:

- [IllegalArgumentException](core_package_exceptions.md#class-illegalargumentexception) - Thrown when the `capacity` parameter is less than or equal to 0.

Example:

<!-- verify -->
```cangjie
main(): Int64 {
    // Create a StringBuilder instance and add some content
    var sb = StringBuilder("Hello, World!")
    println("Before reset - Content: '${sb}', Size: ${sb.size}, Capacity: ${sb.capacity}")
    
    // Reset StringBuilder with default capacity
    sb.reset()
    println("After reset - Content: '${sb}', Size: ${sb.size}, Capacity: ${sb.capacity}")
    
    // Add new content
    sb.append("New content")
    println("After append - Content: '${sb}', Size: ${sb.size}")
    
    // Reset with specified capacity
    sb.reset(capacity: Some(50))
    println("After reset with capacity - Content: '${sb}', Size: ${sb.size}, Capacity: ${sb.capacity}")
    
    return 0
}
```

Output:

```text
Before reset - Content: 'Hello, World!', Size: 13, Capacity: 45
After reset - Content: '', Size: 0, Capacity: 32
After append - Content: 'New content', Size: 11
After reset with capacity - Content: '', Size: 0, Capacity: 50
```

### func toString()

```cangjie
public func toString(): String
```

Function: Retrieves the string from this [StringBuilder](core_package_classes.md#class-stringbuilder) instance.

> **Note:**
>
> This function does not copy the string data.

Returns:

- [String](core_package_structs.md#struct-string) - The string contained in this [StringBuilder](core_package_classes.md#class-stringbuilder) instance.

Example:

<!-- verify -->
```cangjie
main(): Int64 {
    // Create a StringBuilder instance and add some content
    var sb = StringBuilder("Hello")
    sb.append(", World!")
    
    // Get the string representation
    let result: String = sb.toString()
    println("Content: '${result}'")
    println("Size: ${sb.size}")
    
    // Verify the return type is String
    println("Type check: ${result is String}")
    
    return 0
}
```

Output:

```text
Content: 'Hello, World!'
Size: 13
Type check: true
```

## class Thread

```cangjie
public class Thread
```

Function: Retrieves thread ID and name, checks for thread cancellation requests, registers uncaught exception handlers, etc.

Instances of this type cannot be constructed directly. They can only be obtained through the `thread` property of a [Future](core_package_classes.md#class-futuret) object or the `currentThread` static property of the [Thread](core_package_classes.md#class-thread) class.

### static prop currentThread

```cangjie
public static prop currentThread: Thread
```

Function: Gets the [Thread](core_package_classes.md#class-thread) object representing the currently executing thread.

Type: [Thread](core_package_classes.md#class-thread)

Example:

<!-- verify -->
```cangjie
main(): Int64 {
    // Get current thread
    let currentThread = Thread.currentThread
    
    // Print thread information
    println("Current thread ID: ${currentThread.id}")
    println("Current thread name: '${currentThread.name}'")
    
    return 0
}
```

Output:

```text
Current thread ID: 1
Current thread name: ''
```

### prop hasPendingCancellation

```cangjie
public prop hasPendingCancellation: Bool
```

Function: Determines whether the current thread has received a cancellation request, i.e., whether another thread has sent a cancellation request via future.cancel(). Common usage: [Thread](core_package_classes.md#class-thread).currentThread.hasPendingCancellation.

Type: [Bool](core_package_intrinsics.md#bool)

Example:

<!-- verify -->
```cangjie
main(): Int64 {
    // Get current thread
    let currentThread = Thread.currentThread
    
    // Check for cancellation request
    let hasPendingCancellation: Bool = currentThread.hasPendingCancellation
    
    println("Has pending cancellation: ${hasPendingCancellation}")
    
    let future = spawn {
        while (true) {
            if (Thread.currentThread.hasPendingCancellation) {
                println("Has pending cancellation: ${Thread.currentThread.hasPendingCancellation}")
                return 0
            }
        }
        return 1
    }
    /* Send cancellation request to thread */
    future.cancel()
    /* Wait for thread result */
    future.get()
    return 0
}
```

Output:

```text
Has pending cancellation: false
Has pending cancellation: true
```

### prop id

```cangjie
public prop id: Int64
```

Function: Gets the identifier of the currently executing thread as an [Int64](core_package_intrinsics.md#int64). All live threads have unique identifiers, but there's no guarantee that a thread's identifier won't be reused after it terminates.

Type: [Int64](core_package_intrinsics.md#int64)

Example:

<!-- verify -->
```cangjie
main(): Int64 {
    // Get current thread
    let currentThread = Thread.currentThread
    
    // Get thread ID
    let threadId: Int64 = currentThread.id
    
    println("Current thread ID: ${threadId}")
    
    return 0
}
```

Output:

```text
Current thread ID: 1
```

### prop name

```cangjie
public mut prop name: String
```

Function: Gets or sets the thread name atomically.

Type: [String](core_package_structs.md#struct-string)

Example:

<!-- verify -->
```cangjie
main(): Int64 {
    // Get current thread
    let currentThread = Thread.currentThread
    
    // Get current thread name
    let originalName: String = currentThread.name
    println("Original thread name: '${originalName}'")
    
    // Set thread name
    currentThread.name = "MyCustomThread"
    
    // Get updated thread name
    let newName: String = currentThread.name
    println("New thread name: '${newName}'")
    
    return 0
}
```

Output:

```text
Original thread name: ''
New thread name: 'MyCustomThread'
```

### prop state

```cangjie
public prop state: ThreadState
```

Function: Gets the thread's state.

Type: [ThreadState](core_package_enums.md#enum-threadstate)

Example:

<!-- verify -->
```cangjie
main(): Int64 {
    // Get current thread
    let currentThread = Thread.currentThread
    
    // Get current thread state
    let state = currentThread.state
    println("Current thread state: '${state}'")
    return 0
}
```

Output:

```text
Current thread state: 'Running'
```

### static func handleUncaughtErrorBy((Error) -> Unit)

```cangjie
public static func handleUncaughtErrorBy(erHandler: (Error) -> Unit): Unit
```

Function: Registers a handler for uncaught thread errors.

When a thread terminates prematurely due to an error:

- if a global uncaught error handler is registered, it will be invoked before thread termination. If the handler throws an exception or error, a simple warning message will be printed to the terminal, and the thread (if an exception is thrown) or the process (if an error is thrown) will be terminated.
- If no handler is registered, the error message will be printed to the terminal by default.

Multiple registrations will overwrite previous handlers.

When multiple threads terminate due to exceptions concurrently, the handler will be executed concurrently for each thread, so thread safety must be ensured in the handler.

The handler's parameter is the uncaught [Error](core_package_exceptions.md#class-error).

> **Note：**
>
> Unsupported platform：OpenHarmony
Parameters:

- erHandler: ([Error](core_package_exceptions.md#class-error)) -> [Unit](core_package_intrinsics.md#unit) - The handler function to register.

Example:

<!-- verify -->
```cangjie
// Define error handler
func handleError(err: Error): Unit {
    println("Unhandled error in thread ${Thread.currentThread.name}: ${err}")
}

// Will throw OutOfMemoryError
func consumeMemory() {
  let array = Array(1024*1024*1024*1024, repeat: 1024)
  for (i in 0..array.size) {
    array[i] = 0
  }
}

main(): Int64 {
    // Register uncaught exception handler
    Thread.handleUncaughtErrorBy(handleError)
    
    // Create a thread that throws an errir
    let future = spawn {
        consumeMemory()
    }

    try {
        // Wait for thread result
        future.get()
    } catch (e: Exception) {
        println("Caught exception: ${e.message}")
    }
    return 0
}
```

Output：

```text
An exception has occurred:
    Out of memory
Unhandled error in thread : OutOfMemoryError
An exception has occurred:
    Out of memory
```

### static func handleUncaughtExceptionBy((Thread, Exception) -> Unit)

```cangjie
public static func handleUncaughtExceptionBy(exHandler: (Thread, Exception) -> Unit): Unit
```

Function: Registers a handler for uncaught thread exceptions.

When a thread terminates prematurely due to an exception:

- if a global uncaught exception handler is registered, it will be invoked before thread termination.
- If the handler itself throws an exception, a warning message will be printed to the terminal (without stack trace). If no handler is registered, the exception stack trace will be printed by default.

Multiple registrations will overwrite previous handlers.

When multiple threads terminate due to exceptions concurrently, the handler will be executed concurrently for each thread, so thread safety must be ensured in the handler.

The handler's first parameter is the [Thread](core_package_classes.md#class-thread) where the exception occurred, and the second parameter is the uncaught [Exception](core_package_exceptions.md#class-exception).

Parameters:

- exHandler: ([Thread](core_package_classes.md#class-thread), [Exception](core_package_exceptions.md#class-exception)) -> [Unit](core_package_intrinsics.md#unit) - The handler function to register.

Example:

<!-- verify -->
```cangjie
// Define exception handler
func handleException(thread: Thread, exception: Exception): Unit {
    println("Unhandled exception in thread ${thread.name}: ${exception.message}")
}

main(): Int64 {
    // Register uncaught exception handler
    Thread.handleUncaughtExceptionBy(handleException)
    
    // Create a thread that throws an exception
    let future = spawn {
        throw Exception("This is a thread exception.")
    }

    try {
        // Wait for thread result
        future.get()
    } catch (e: Exception) {
        println("Caught exception: ${e.message}")
    }
    return 0
}
```

Output:

```text
Unhandled exception in thread : This is a thread exception.
Caught exception: This is a thread exception.
```

## class ThreadLocal\<T>

```cangjie
public class ThreadLocal<T>
```

Function: Represents Cangjie thread-local variables.

Unlike regular variables, thread-local variables have different access semantics. When multiple threads share the same thread-local variable, each thread maintains its own copy. Threads access their local copies without affecting other threads' values.

### func get()

```cangjie
public func get(): ?T
```

Function: Gets the value of the Cangjie thread-local variable.

Returns:

- ?T - Returns the value if the thread-local variable is non-null, otherwise returns `None`.

Example:

<!-- run -->
```cangjie

let local = ThreadLocal<Int64>()

func printUserID(): Unit {
    println("User ID: ${local.get() ?? 0}")
}

func setUserID(id: Int64): Unit {
    local.set(Some(id))
}

main() {
    let future = spawn {
        // Simulate getting user ID
        var id = 1001
        setUserID(id)

        // ThreadLocal eliminates parameter passing
        printUserID()
    }
    let future1 = spawn {
        // Simulate getting user ID
        var id = 1002
        setUserID(id)

        // Using ThreadLocal allows obtaining the user ID without passing parameters
        printUserID()
    }
    let future2 = spawn {
        // Simulate getting user ID
        var id = 1003
        setUserID(id)

        // Using ThreadLocal allows obtaining the user ID without passing parameters
        printUserID()
    }
    future.get()
    future1.get()
    future2.get()
    return 0
}
```

Possible output:

```text
User ID: 1003
User ID: 1001
User ID: 1002
```

### func set(?T)

```cangjie
public func set(value: ?T): Unit
```

Function: Sets the value of the Cangjie thread-local variable. If `None` is passed, the value is removed and becomes inaccessible to the thread.

Parameters:

- value: ?T - The value to set.

Example:

<!-- run -->
```cangjie

let local = ThreadLocal<Int64>()

func printUserID(): Unit {
    println("User ID: ${local.get() ?? 0}")
}

func setUserID(id: Int64): Unit {
    local.set(Some(id))
}

main() {
    let future = spawn {
        // Simulate fetching user ID
        var id = 1001
        setUserID(id)

        // ThreadLocal allows obtaining user ID without passing parameters
        printUserID()
    }
    let future1 = spawn {
        // Simulate fetching user ID
        var id = 1002
        setUserID(id)

        // ThreadLocal allows obtaining user ID without passing parameters
        printUserID()
    }
    let future2 = spawn {
        // Simulate fetching user ID
        var id = 1003
        setUserID(id)

        // ThreadLocal allows obtaining user ID without passing parameters
        printUserID()
    }
    future.get()
    future1.get()
    future2.get()
    return 0
}
```

Possible output:

```text
User ID: 1003
User ID: 1001
User ID: 1002
```

## class ThreadSnapshot

```cangjie
public class ThreadSnapshot <: ToString
```

Function: Retrieves information about current or all threads, including name, ID, state, and call stack.

Instances of this type cannot be constructed directly. They can only be obtained through the static functions [dumpCurrentThread](core_package_classes.md#func-dumpcurrentthread) and [dumpAllThreads](core_package_classes.md#func-dumpallthreads) of the [ThreadSnapshot](core_package_classes.md#class-threadsnapshot) class.

Parent type:

* [ToString](core_package_interfaces.md#interface-tostring)

### let id

```cangjie
public let id: Int64
```

Function: Gets the thread's ID.

Type: [Int64](core_package_intrinsics.md#int64)

### let name

```cangjie
public let name: String
```

Function: Gets the thread's name.

Type: [String](core_package_structs.md#struct-string)

### let stackTrace

```cangjie
public let stackTrace: Array<StackTraceElement>
```

Function: Gets the thread's call stack information.

Type: [Array](core_package_structs.md#struct-arrayt)\<[StackTraceElement](core_package_classes.md#class-stacktraceelement)>

### let state

```cangjie
public let state: ThreadState
```

Function: Gets the thread's state.

Type: [ThreadState](core_package_enums.md#enum-threadstate)

### func dumpAllThreads()

```cangjie
public static func dumpAllThreads(): Array<ThreadSnapshot>
```

Function: Gets information about all threads in the current process.

Returns:

- [Array](core_package_structs.md#struct-arrayt)\<[ThreadSnapshot](core_package_classes.md#class-threadsnapshot)> - Returns an array of [ThreadSnapshot](core_package_classes.md#class-threadsnapshot) containing information about all threads in the current process.

Example:

<!-- verify -->

```cangjie
main(): Unit {
    /* Create a thread */
    let future =spawn {
        while(true) {
            sleep(1 * Duration.second)
            if (Thread.currentThread.hasPendingCancellation){
                return
            }
        }
    }
    /* Get information about all threads */
    let threadInfoArray: Array<ThreadSnapshot> = ThreadSnapshot.dumpAllThreads()
    /* Iterate and print thread information */
    let size = threadInfoArray.size
    for (i in 0..size) {
        let threadInfoData = threadInfoArray[i]
        println(threadInfoData)
    }
}
```

Execution Result:

```text
ThreadSnapshot(id=1, name=, state=Running)
stack trace:
         at std.core.ThreadSnapshot::dumpCurrentThread()(thread.cj:176)
         at default.test4()(hello.cj:46)
         at default.main()(hello.cj:146)
ThreadSnapshot(id=2, name=, state=Pending)
stack trace:
         at std.core.sleep(std.core::Duration)(sleep.cj:36)
         at default.test6::lambda.0()(hello.cj:66)
         at std.core.Future<...>::execute()(future.cj:161)
```

### func dumpCurrentThread()

```cangjie
public static func dumpCurrentThread(): ThreadSnapshot
```

Function: Gets information about the current thread.

Returns:

- [ThreadSnapshot](core_package_classes.md#class-threadsnapshot) - Returns a [ThreadSnapshot](core_package_classes.md#class-threadsnapshot) object containing information about the current thread.

Example:

<!-- verify -->

```cangjie
main(): Unit {
    /* Get current thread information */
    let threadInfo: ThreadSnapshot = ThreadSnapshot.dumpCurrentThread()
    /* Print the information */
    println(threadInfo)
}
```

Execution Result:

```text
ThreadSnapshot(id=1, name=, state=Running)
stack trace:
         at std.core.ThreadSnapshot::dumpAllThreads()(thread.cj:161)
         at default.test6()(hello.cj:74)
         at default.main()(hello.cj:148)
```

### func  toString()

```cangjie
public func toString(): String
```

Function: Gets the string representation of the [ThreadSnapshot](core_package_classes.md#class-threadsnapshot) object.

Returns:

- [String](core_package_structs.md#struct-string) - The converted string.
