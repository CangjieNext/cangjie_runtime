# Extensions

## extend\<K, V> HashMap\<K, V> <: Equatable\<HashMap\<K, V>> where V <: Equatable\<V>

```cangjie
extend<K, V> HashMap<K, V> <: Equatable<HashMap<K, V>> where V <: Equatable<V>
```

Function: Extends [HashMap](./collection_package_class.md#class-hashmapk-v-where-k--hashable--equatablek)\<K, V> with the [Equatable](../../core/core_package_api/core_package_interfaces.md#interface-equatablet)\<[HashMap](./collection_package_class.md#class-hashmapk-v-where-k--hashable--equatablek)\<K, V>> interface, enabling equality comparison.

Extended type:

- [HashMap\<K, V>](./collection_package_class.md#class-hashmapk-v-where-k--hashable--equatablek)

Extended interfaces:

- Equatable\<HashMap\<K, V>>

### operator func ==(HashMap\<K, V>)

```cangjie
public operator func ==(right: HashMap<K, V>): Bool
```

Function: Determines whether the current instance is equal to the parameter's [HashMap](./collection_package_class.md#class-hashmapk-v-where-k--hashable--equatablek)\<K, V> instance.

Two [HashMap](./collection_package_class.md#class-hashmapk-v-where-k--hashable--equatablek)\<K, V> instances are considered equal if they contain identical key-value pairs.

Parameters:

- right: [HashMap](./collection_package_class.md#class-hashmapk-v-where-k--hashable--equatablek)\<K, V> - The object to compare.

Returns:

- [Bool](../../core/core_package_api/core_package_intrinsics.md#bool) - Returns true if equal, false otherwise.

Example:

<!-- verify -->
```cangjie
import std.collection.*

main() {
    // Create two HashMaps
    let map1 = HashMap<String, Int64>()
    map1["a"] = 1
    map1["b"] = 2
    
    let map2 = HashMap<String, Int64>()
    map2["b"] = 2
    map2["a"] = 1
    
    let map3 = HashMap<String, Int64>()
    map3["a"] = 1
    map3["b"] = 3
    
    // Compare equal HashMaps
    println("map1 == map2: ${map1 == map2}")  // true
    
    // Compare unequal HashMaps
    println("map1 == map3: ${map1 == map3}")  // false
    
    return 0
}
```

Output:

```text
map1 == map2: true
map1 == map3: false
```

### operator func !=(HashMap\<K, V>)

```cangjie
public operator func !=(right: HashMap<K, V>): Bool
```

Function: Determines whether the current instance is not equal to the parameter's [HashMap](./collection_package_class.md#class-hashmapk-v-where-k--hashable--equatablek)\<K, V> instance.

Parameters:

- right: [HashMap](./collection_package_class.md#class-hashmapk-v-where-k--hashable--equatablek)\<K, V> - The object to compare.

Returns:

- [Bool](../../core/core_package_api/core_package_intrinsics.md#bool) - Returns true if not equal, false otherwise.

Example:

<!-- verify -->
```cangjie
import std.collection.*

main() {
    // Create two HashMaps
    let map1 = HashMap<String, Int64>()
    map1["x"] = 10
    map1["y"] = 20
    
    let map2 = HashMap<String, Int64>()
    map2["x"] = 10
    map2["y"] = 30
    
    let map3 = HashMap<String, Int64>()
    map3["y"] = 20
    map3["x"] = 10
    
    // Compare unequal HashMaps
    println("map1 != map2: ${map1 != map2}")  // true
    
    // Compare equal HashMaps
    println("map1 != map3: ${map1 != map3}")  // false
    
    return 0
}
```

Output:

```text
map1 != map2: true
map1 != map3: false
```

## extend\<K, V> HashMap\<K, V> <: ToString where V <: ToString, K <: ToString

```cangjie
extend<K, V> HashMap<K, V> <: ToString where V <: ToString, K <: ToString
```

Function: Extends [HashMap](./collection_package_class.md#class-hashmapk-v-where-k--hashable--equatablek)\<K, V> with the [ToString](../../core/core_package_api/core_package_interfaces.md#interface-tostring) interface, enabling string conversion.

Extended type:

- [HashMap\<K, V>](./collection_package_class.md#class-hashmapk-v-where-k--hashable--equatablek)

Extended interfaces:

- ToString

### func toString()

```cangjie
public func toString(): String
```

Function: Converts the current [HashMap](./collection_package_class.md#class-hashmapk-v-where-k--hashable--equatablek)\<K, V> instance to a string.

The resulting string contains string representations of all key-value pairs in the format: "[(k1, v1), (k2, v2), (k3, v3)]".

Returns:

- [String](../../core/core_package_api/core_package_structs.md#struct-string) - The converted string.

Example:

<!-- verify -->
```cangjie
import std.collection.*

main() {
    // Create HashMap
    let map = HashMap<String, Int64>()
    map["apple"] = 5
    map["banana"] = 3
    map["orange"] = 8
    
    // Convert to string
    let mapString = map.toString()
    
    println("String representation of empty collection: ${HashMap<String, Int64>().toString()}")  // []
    println("String representation of HashMap: ${mapString}")
    
    return 0
}
```

Output:

```text
String representation of empty collection: []
String representation of HashMap: [(apple, 5), (banana, 3), (orange, 8)]
```

## extend\<K, V> TreeMap\<K, V> <: Equatable\<TreeMap\<K, V>> where V <: Equatable\<V>

```cangjie
extend<K, V> TreeMap<K, V> <: Equatable<TreeMap<K, V>> where V <: Equatable<V>
```

Function: Extends the [TreeMap](./collection_package_class.md#class-treemapk-v-where-k--comparablek)\<K, V> type with the [Equatable](../../core/core_package_api/core_package_interfaces.md#interface-equatablet)\<[TreeMap](./collection_package_class.md#class-treemapk-v-where-k--comparablek)\<K, V>> interface, supporting equality comparison.

Extended type:

- [TreeMap\<K, V>](./collection_package_class.md#class-treemapk-v-where-k--comparablek)

Extended interfaces:

- Equatable\<TreeMap\<K, V>>

### operator func ==(TreeMap\<K, V>)

```cangjie
public operator func ==(right: TreeMap<K, V>): Bool
```

Function: Determines whether the current instance is equal to the parameter's [TreeMap](./collection_package_class.md#class-treemapk-v-where-k--comparablek)\<K, V> instance.

Two [TreeMap](./collection_package_class.md#class-treemapk-v-where-k--comparablek)\<K, V> instances are considered equal if they contain identical key-value pairs.

Parameters:

- right: [TreeMap](./collection_package_class.md#class-treemapk-v-where-k--comparablek)\<K, V> - The object to compare.

Returns:

- [Bool](../../core/core_package_api/core_package_intrinsics.md#bool) - Returns true if equal, otherwise false.

Example:

<!-- verify -->
```cangjie
import std.collection.*

main() {
    // Create two identical TreeMaps
    let map1 = TreeMap<String, Int64>()
    map1.add("apple", 10)
    map1.add("banana", 20)
    map1.add("cherry", 30)
    
    let map2 = TreeMap<String, Int64>()
    map2.add("apple", 10)
    map2.add("banana", 20)
    map2.add("cherry", 30)
    
    println("map1: ${map1}")
    println("map2: ${map2}")
    println("map1 == map2: ${map1 == map2}")
    
    // Create a different TreeMap
    let map3 = TreeMap<String, Int64>()
    map3.add("apple", 10)
    map3.add("banana", 20)
    map3.add("date", 40)  // Different key-value pair
    
    println("\nmap3: ${map3}")
    println("map1 == map3: ${map1 == map3}")
    
    // Create a TreeMap with the same keys but different values
    let map4 = TreeMap<String, Int64>()
    map4.add("apple", 15)  // Different value
    map4.add("banana", 20)
    map4.add("cherry", 30)
    
    println("\nmap4: ${map4}")
    println("map1 == map4: ${map1 == map4}")
    
    // Compare empty TreeMaps
    let emptyMap1 = TreeMap<String, Int64>()
    let emptyMap2 = TreeMap<String, Int64>()
    
    println("\nemptyMap1: ${emptyMap1}")
    println("emptyMap2: ${emptyMap2}")
    println("emptyMap1 == emptyMap2: ${emptyMap1 == emptyMap2}")
    println("map1 == emptyMap1: ${map1 == emptyMap1}")
    
    return 0
}
```

Execution Result:

```text
map1: [(apple, 10), (banana, 20), (cherry, 30)]
map2: [(apple, 10), (banana, 20), (cherry, 30)]
map1 == map2: true

map3: [(apple, 10), (banana, 20), (date, 40)]
map1 == map3: false

map4: [(apple, 15), (banana, 20), (cherry, 30)]
map1 == map4: false

emptyMap1: []
emptyMap2: []
emptyMap1 == emptyMap2: true
map1 == emptyMap1: false
```

### operator func !=(TreeMap\<K, V>)

```cangjie
public operator func !=(right: TreeMap<K, V>): Bool
```

Function: Determines whether the current instance is not equal to the parameter's [TreeMap](./collection_package_class.md#class-treemapk-v-where-k--comparablek)\<K, V> instance.

Parameters:

- right: [TreeMap](./collection_package_class.md#class-treemapk-v-where-k--comparablek)\<K, V> - The object to compare.

Returns:

- [Bool](../../core/core_package_api/core_package_intrinsics.md#bool) - Returns true if not equal, otherwise false.

Example:

<!-- verify -->
```cangjie
import std.collection.*

main() {
    // Create two identical TreeMaps
    let map1 = TreeMap<String, Int64>()
    map1.add("apple", 10)
    map1.add("banana", 20)
    map1.add("cherry", 30)
    
    let map2 = TreeMap<String, Int64>()
    map2.add("apple", 10)
    map2.add("banana", 20)
    map2.add("cherry", 30)
    
    println("map1: ${map1}")
    println("map2: ${map2}")
    println("map1 != map2: ${map1 != map2}")
    
    // Create a different TreeMap
    let map3 = TreeMap<String, Int64>()
    map3.add("apple", 10)
    map3.add("banana", 20)
    map3.add("date", 40)  // Different key-value pair
    
    println("\nmap3: ${map3}")
    println("map1 != map3: ${map1 != map3}")
    
    // Create a TreeMap with the same keys but different values
    let map4 = TreeMap<String, Int64>()
    map4.add("apple", 15)  // Different value
    map4.add("banana", 20)
    map4.add("cherry", 30)
    
    println("\nmap4: ${map4}")
    println("map1 != map4: ${map1 != map4}")
    
    // Compare empty and non-empty TreeMaps
    let emptyMap = TreeMap<String, Int64>()
    println("\nemptyMap: ${emptyMap}")
    println("map1 != emptyMap: ${map1 != emptyMap}")
    
    // Compare two empty TreeMaps
    let anotherEmptyMap = TreeMap<String, Int64>()
    println("emptyMap != anotherEmptyMap: ${emptyMap != anotherEmptyMap}")
    
    return 0
}
```

Execution Result:

```text
map1: [(apple, 10), (banana, 20), (cherry, 30)]
map2: [(apple, 10), (banana, 20), (cherry, 30)]
map1 != map2: false

map3: [(apple, 10), (banana, 20), (date, 40)]
map1 != map3: true

map4: [(apple, 15), (banana, 20), (cherry, 30)]
map1 != map4: true

emptyMap: []
map1 != emptyMap: true
emptyMap != anotherEmptyMap: false
```

## extend\<K, V> TreeMap\<K, V> <: ToString where V <: ToString, K <: ToString & Comparable\<K>

```cangjie
extend<K, V> TreeMap<K, V> <: ToString where V <: ToString, K <: ToString & Comparable<K>
```

Function: Extends the [TreeMap](./collection_package_class.md#class-treemapk-v-where-k--comparablek)\<K, V> with the [ToString](../../core/core_package_api/core_package_interfaces.md#interface-tostring) interface, supporting string conversion.

Extended type:

- [TreeMap\<K, V>](./collection_package_class.md#class-treemapk-v-where-k--comparablek)

Extended interfaces:

- ToString

### func toString()

```cangjie
public func toString(): String
```

Function: Converts the current [TreeMap](./collection_package_class.md#class-treemapk-v-where-k--comparablek)\<K, V> instance to a string.

The string includes the string representation of each key-value pair in the [TreeMap](./collection_package_class.md#class-treemapk-v-wwhere-k--comparablek)\<K, V>, formatted as: "[(k1, v1), (k2, v2), (k3, v3)]".

Returns:

- [String](../../core/core_package_api/core_package_structs.md#struct-string) - The resulting string.

Example:

<!-- verify -->
```cangjie
import std.collection.*

main() {
    // String representation of an empty TreeMap
    let emptyMap = TreeMap<String, Int64>()
    println("Empty TreeMap: ${emptyMap.toString()}")
    
    // String representation of a non-empty TreeMap
    let map = TreeMap<String, Int64>()
    map.add("cherry", 30)
    map.add("apple", 10)
    map.add("banana", 20)
    
    println("Non-empty TreeMap: ${map.toString()}")
    
    // Direct printing (automatically calls toString)
    println("Direct print: ${map}")
    
    return 0
}
```

Execution Result:

```text
Empty TreeMap: []
Non-empty TreeMap: [(apple, 10), (banana, 20), (cherry, 30)]
Direct print: [(apple, 10), (banana, 20), (cherry, 30)]
```

## extend\<T> ArrayDeque\<T> <: ToString where T <: ToString

```cangjie
extend<T> ArrayDeque<T> <: ToString where T <: ToString
```

Function: Extends [ArrayDeque](./collection_package_class.md#class-arraydequet)\<T> with the [ToString](../../core/core_package_api/core_package_interfaces.md#interface-tostring) interface, supporting string conversion.

Extended type:

- [ArrayDeque\<T>](./collection_package_class.md#class-arraydequet)

Extended interfaces:

- ToString

### func toString()

```cangjie
public func toString(): String
```

Function: Gets the string representation of the current [ArrayDeque](./collection_package_class.md#class-arraydequet)\<T> instance.

The string contains the string representation of each element in the deque in front-to-back order, formatted as: "[elem1, elem2, elem3]".

Returns:

- [String](../../core/core_package_api/core_package_structs.md#struct-string) - The converted string.

Example:

<!-- verify -->
```cangjie
import std.collection.*

main() {
    let deque = ArrayDeque<Int64>()
    deque.addLast(1)
    deque.addLast(2)
    deque.addLast(3)
    println(deque.toString())  // [1, 2, 3]
    return 0
}
```

Output:

```text
[1, 2, 3]
```

## extend\<T> ArrayList\<T>

```cangjie
extend<T> ArrayList<T>
```

Function: Extends the [ArrayList](collection_package_class.md#class-arraylistt)\<T> type.

Extended type:

- [ArrayList\<T>](collection_package_class.md#class-arraylistt)

### func enumerate()

```cangjie
public func enumerate(): ArrayList<(Int64, T)>
```

Function: Returns an [ArrayList](collection_package_class.md#class-arraylistt) with indices.

Returns:

- [ArrayList](collection_package_class.md#class-arraylistt)\<([Int64](../../core/core_package_api/core_package_intrinsics.md#int64), T)> - Returns a new [ArrayList](collection_package_class.md#class-arraylistt) with indices.

### func zip\<R>(ArrayList\<R>)

```cangjie
public func zip<R>(other: ArrayList<R>): ArrayList<(T, R)>
```

Function: Merges two [ArrayList](collection_package_class.md#class-arraylistt)s into a new [ArrayList](collection_package_class.md#class-arraylistt) (length determined by the shorter [ArrayList](collection_package_class.md#class-arraylistt)).


> **Note：**
>
> Unsupported platform：OpenHarmony

Parameters:

- other: [ArrayList](collection_package_class.md#class-arraylistt)\<R> - One of the [ArrayList](collection_package_class.md#class-arraylistt)s to merge.

Returns:

- [ArrayList](collection_package_class.md#class-arraylistt)\<(T, R)> - Returns a new [ArrayList](collection_package_class.md#class-arraylistt).### extend\<T> ArrayList\<T> <: Equatable\<ArrayList\<T>> where T <: Equatable\<T>

```cangjie
extend<T> ArrayList<T> <: Equatable<ArrayList<T>> where T <: Equatable<T>
```

Function: Extends the [ArrayList](./collection_package_class.md#class-arraylistt)\<T> type with the [Equatable](../../core/core_package_api/core_package_interfaces.md#interface-equatablet)\<[ArrayList](./collection_package_class.md#class-arraylistt)\<T>> interface to support equality comparison operations.

Parent Types:

- [Equatable](../../core/core_package_api/core_package_interfaces.md#interface-equatablet)\<[ArrayList](collection_package_class.md#class-arraylistt)\<T>>

### operator func ==(ArrayList\<T>)

```cangjie
public operator func ==(that: ArrayList<T>): Bool
```

Function: Determines whether the current instance is equal to the [ArrayList](./collection_package_class.md#class-arraylistt) instance pointed to by the parameter.

Two arrays are considered equal if their corresponding elements at each position are equal.

Parameters:

- that: [ArrayList](./collection_package_class.md#class-arraylistt)\<T> - The object to be compared.

Returns:

- [Bool](../../core/core_package_api/core_package_intrinsics.md#bool) - Returns true if equal, otherwise false.

Example:

<!-- verify -->
```cangjie
import std.collection.*

main() {
    let list1 = ArrayList<Int64>()
    list1.add(1)
    list1.add(2)
    list1.add(3)
    
    let list2 = ArrayList<Int64>()
    list2.add(1)
    list2.add(2)
    list2.add(3)
    
    let list3 = ArrayList<Int64>()
    list3.add(1)
    list3.add(2)
    
    println("list1 == list2: ${list1 == list2}")  // true
    println("list1 == list3: ${list1 == list3}")  // false
    
    return 0
}
```

Execution Result:

```text
list1 == list2: true
list1 == list3: false
```

### operator func !=(ArrayList\<T>)

```cangjie
public operator func !=(that: ArrayList<T>): Bool
```

Function: Determines whether the current instance is not equal to the [ArrayList](./collection_package_class.md#class-arraylistt) instance pointed to by the parameter.

Parameters:

- that: [ArrayList](./collection_package_class.md#class-arraylistt)\<T> - The object to be compared.

Returns:

- [Bool](../../core/core_package_api/core_package_intrinsics.md#bool) - Returns true if not equal, otherwise false.

Example:

<!-- verify -->
```cangjie
import std.collection.*

main() {
    let list1 = ArrayList<Int64>()
    list1.add(1)
    list1.add(2)
    list1.add(3)
    
    let list2 = ArrayList<Int64>()
    list2.add(1)
    list2.add(2)
    
    let list3 = ArrayList<Int64>()
    list3.add(1)
    list3.add(2)
    list3.add(3)
    
    println("list1 != list2: ${list1 != list2}")  // true
    println("list1 != list3: ${list1 != list3}")  // false
    
    return 0
}
```

Execution Result:

```text
list1 != list2: true
list1 != list3: false
```

### func contains(T)

```cangjie
public func contains(element: T): Bool
```

Function: Determines whether the current array contains the specified element `element`.

Parameters:

- element: T - The element to search for.

Returns:

- [Bool](../../core/core_package_api/core_package_intrinsics.md#bool) - Returns true if the array contains the specified element, otherwise false.

Example:

<!-- verify -->
```cangjie
import std.collection.*

main() {
    let list = ArrayList<Int64>()
    list.add(1)
    list.add(2)
    list.add(3)
    
    println("List contains element 2: ${list.contains(2)}")  // true
    println("List contains element 5: ${list.contains(5)}")  // false
    
    return 0
}
```

Execution Result:

```text
List contains element 2: true
List contains element 5: false
```

## extend\<T> ArrayList\<T> <: SortExtension where T <: Comparable\<T>

```cangjie
extend<T> ArrayList<T> where T <: Comparable<T>
```

Function: Extends the [ArrayList](./collection_package_class.md#class-arraylistt)\<T> with the [SortExtension](../../sort/sort_package_api/sort_package_interfaces.md#interface-sortextension-deprecated) interface to support array sorting.

Extended type:

- [ArrayList\<T>](./collection_package_class.md#class-arraylistt)

Extended interfaces:

- SortExtension

### func sort() <sup>(deprecated)</sup>

```cangjie
public func sort(): Unit
```

Function: Sorts the elements in the current array in ascending order using an unstable sort.

> **Note:**
>
> This will be deprecated in future versions. Use [sort](../../sort/sort_package_api/sort_package_funcs.md#func-sorttlistt-bool-bool-where-t--comparablet) instead.

Example:

<!-- verify -->
```cangjie
import std.collection.*

main() {
    let list = ArrayList<Int64>()
    list.add(3)
    list.add(1)
    list.add(4)
    list.add(2)
    // list is now [3, 1, 4, 2]
    println("Before sorting: ${list}")  // [3, 1, 4, 2]
    
    // Sort in ascending order
    list.sort()
    
    println("After sorting: ${list}")  // [1, 2, 3, 4]
    
    return 0
}
```

Execution Result:

```text
Before sorting: [3, 1, 4, 2]
After sorting: [1, 2, 3, 4]
```

### func sort(Bool) <sup>(deprecated)</sup>

```cangjie
public func sort(stable!: Bool): Unit
```

Function: Sorts the elements in the current array in ascending order.

Parameters:

- stable!: [Bool](../../core/core_package_api/core_package_intrinsics.md#bool) - Whether to use a stable sort.

> **Note:**
>
> This will be deprecated in future versions. Use [sort](../../sort/sort_package_api/sort_package_funcs.md#func-sorttlistt-bool-bool-where-t--comparablet) instead.

Example:

<!-- verify -->
```cangjie
import std.collection.*

main() {
    let list = ArrayList<Int64>()
    list.add(3)
    list.add(1)
    list.add(4)
    list.add(2)
    // list is now [3, 1, 4, 2]
    println("Before sorting: ${list}")  // [3, 1, 4, 2]
    
    // Sort in ascending order using stable sort
    list.sort(stable: true)
    
    println("After stable sorting: ${list}")  // [1, 2, 3, 4]
    
    return 0
}
```

Execution Result:

```text
Before sorting: [3, 1, 4, 2]
After stable sorting: [1, 2, 3, 4]
```

### func sortDescending() <sup>(deprecated)</sup>

```cangjie
public func sortDescending(): Unit
```

Function: Sorts the elements in the current array in descending order using an unstable sort.

> **Note:**
>
> This will be deprecated in future versions. Use [sort](../../sort/sort_package_api/sort_package_funcs.md#func-sorttlistt-bool-bool-where-t--comparablet) instead.

Example:

<!-- verify -->
```cangjie
import std.collection.*

main() {
    let list = ArrayList<Int64>()
    list.add(3)
    list.add(1)
    list.add(4)
    list.add(2)
    // list is now [3, 1, 4, 2]
    println("Before sorting: ${list}")  // [3, 1, 4, 2]
    
    // Sort in descending order
    list.sortDescending()
    
    println("After descending sort: ${list}")  // [4, 3, 2, 1]
    
    return 0
}
```

Execution Result:

```text
Before sorting: [3, 1, 4, 2]
After descending sort: [4, 3, 2, 1]
```

### func sortDescending(Bool) <sup>(deprecated)</sup>

```cangjie
public func sortDescending(stable!: Bool): Unit
```

Function: Sorts the elements in the current array in descending order.

Parameters:

- stable!: [Bool](../../core/core_package_api/core_package_intrinsics.md#bool) - Whether to use a stable sort.

> **Note:**
>
> This will be deprecated in future versions. Use [sort](../../sort/sort_package_api/sort_package_funcs.md#func-sorttlistt-bool-bool-where-t--comparablet) instead.

Example:

<!-- verify -->
```cangjie
import std.collection.*

main() {
    let list = ArrayList<Int64>()
    list.add(3)
    list.add(1)
    list.add(4)
    list.add(2)
    // list is now [3, 1, 4, 2]
    println("Before sorting: ${list}")  // [3, 1, 4, 2]
    
    // Sort in descending order using stable sort
    list.sortDescending(stable: true)
    
    println("After stable descending sort: ${list}")  // [4, 3, 2, 1]
    
    return 0
}
```

Execution Result:

```text
Before sorting: [3, 1, 4, 2]
After stable descending sort: [4, 3, 2, 1]
```

## extend\<T> ArrayList\<T> <: ToString where T <: ToString

```cangjie
extend<T> ArrayList<T> <: ToString where T <: ToString
```

Function: Extends the [ArrayList](./collection_package_class.md#class-arraylistt)\<T> with the [ToString](../../core/core_package_api/core_package_interfaces.md#interface-tostring) interface to support string conversion operations.

Extended type:

- [ArrayList\<T>](./collection_package_class.md#class-arraylistt)

Extended interfaces:

- ToString

### func toString()

```cangjie
public func toString(): String
```

Function: Converts the current array to a string.

The resulting string contains the string representation of each element in the array, formatted as: "[elem1, elem2, elem3]".

Returns:

- [String](../../core/core_package_api/core_package_structs.md#struct-string) - The converted string.

Example:

<!-- verify -->
```cangjie
import std.collection.*

main() {
    let list = ArrayList<Int64>()
    list.add(1)
    list.add(2)
    list.add(3)
    
    let str = list.toString()
    
    println("List: ${list}")     // [1, 2, 3]
    println("String: ${str}")    // [1, 2, 3]
    println("String length: ${str.size}") // 9
    
    return 0
}
```

Execution Result:

```text
List: [1, 2, 3]
String: [1, 2, 3]
String length: 9
```

## extend\<T> ArrayQueue\<T> <: ToString where T <: ToString

```cangjie
extend<T> ArrayQueue<T> <: ToString where T <: ToString
```

Function: Extends [ToString](../../core/core_package_api/core_package_interfaces.md#interface-tostring) interface for [ArrayQueue](./collection_package_class.md#class-arrayqueuet)\<T>, supporting string conversion.

Extended type:

- [ArrayQueue\<T>](./collection_package_class.md#class-arrayqueuet)

Extended interfaces:

- ToString

### func toString()

```cangjie
public func toString(): String
```

Function: Gets the string representation of this [ArrayQueue](./collection_package_class.md#class-arrayqueuet)\<T> instance.

The string contains string representations of all elements in front-to-back order, formatted as: "[elem1, elem2, elem3]".

Returns:

- [String](../../core/core_package_api/core_package_structs.md#struct-string) - The converted string.

Example:

<!-- verify -->
```cangjie
import std.collection.*

main() {
    let queue = ArrayQueue<Int64>()
    
    // Empty queue to string
    let emptyStr = queue.toString()
    println("Empty queue string: ${emptyStr}")  // []
    
    // Queue to string after adding elements
    queue.add(1)
    queue.add(2)
    queue.add(3)
    
    let str = queue.toString()
    println("Queue string: ${str}")      // [1, 2, 3]
    println("String length: ${str.size}")  // 9
    
    return 0
}
```

Output:

```text
Empty queue string: []
Queue string: [1, 2, 3]
String length: 9
```

## extend\<T> ArrayStack\<T> <: ToString where T <: ToString

```cangjie
extend<T> ArrayStack<T> <: ToString where T <: ToString
```

Function: Extends ArrayStack with the [ToString](../../core/core_package_api/core_package_interfaces.md#interface-tostring) interface to support string conversion.

Extended type:

- [ArrayStack\<T>](./collection_package_class.md#class-arraystackt)

Extended interfaces:

- ToString

### func toString()

```cangjie
public func toString(): String
```

Function: Gets the string representation of the current [ArrayStack](./collection_package_class.md#class-arraystackt)\<T> instance.

The string contains each element's string representation in reverse order, formatted as: "[elem1, elem2, elem3]".

Returns:

- [String](../../core/core_package_api/core_package_structs.md#struct-string) - The string representation of the stack.

Example:

<!-- verify -->
```cangjie
import std.collection.*

main() {
    let stack = ArrayStack<Int64>()
    
    // Empty stack to string
    let emptyStr = stack.toString()
    println("Empty stack string: ${emptyStr}")  // []
    
    // Add elements and convert to string
    stack.add(1)
    stack.add(2)
    stack.add(3)
    
    let str = stack.toString()
    println("Stack string: ${str}")      // [3, 2, 1]
    println("String length: ${str.size}")  // 9
    
    return 0
}
```

Output:

```text
Empty stack string: []
Stack string: [3, 2, 1]
String length: 9
```

## extend\<T> HashSet\<T> <: Equatable\<HashSet\<T>>

```cangjie
extend<T> HashSet<T> <: Equatable<HashSet<T>>
```

Function: Extends [HashSet](./collection_package_class.md#class-hashsett-where-t--hashable--equatablet)\<T> with [Equatable](../../core/core_package_api/core_package_interfaces.md#interface-equatablet)\<[HashSet](./collection_package_class.md#class-hashsett-where-t--hashable--equatablet)\<T>> interface to support equality comparison.

Extended type:

- [HashSet\<T>](./collection_package_class.md#class-hashsett-where-t--hashable--equatablet)

Extended interfaces:

- Equatable\<HashSet\<T>>

### operator func ==(HashSet\<T>)

```cangjie
public operator func ==(that: HashSet<T>): Bool
```

Function: Determines whether the current instance is equal to the specified [HashSet](./collection_package_class.md#class-hashsett-where-t--hashable--equatablet)\<T> instance.

Two [HashSet](./collection_package_class.md#class-hashsett-where-t--hashable--equatablet)\<T> instances are considered equal if they contain identical elements.

Parameters:

- that: [HashSet](./collection_package_class.md#class-hashsett-where-t--hashable--equatablet)\<T> - The object to compare.

Returns:

- [Bool](../../core/core_package_api/core_package_intrinsics.md#bool) - Returns true if equal, otherwise false.

Example:

<!-- verify -->
```cangjie
import std.collection.*

main() {
    // Create two identical HashSets
    let set1 = HashSet<String>()
    set1.add("apple")
    set1.add("banana")
    set1.add("orange")
    
    let set2 = HashSet<String>()
    set2.add("banana")
    set2.add("apple")  // Different order but same elements
    set2.add("orange")
    
    // Compare identical sets
    println("Set 1 size: ${set1.size}")
    println("Set 2 size: ${set2.size}")
    println("Set 1 == Set 2: ${set1 == set2}")
    
    // Create different HashSet
    let set3 = HashSet<String>()
    set3.add("apple")
    set3.add("grape")  // Different element
    
    println("\nSet 3 size: ${set3.size}")
    println("Set 1 == Set 3: ${set1 == set3}")
    
    // Test empty sets
    let emptySet1 = HashSet<String>()
    let emptySet2 = HashSet<String>()
    
    println("\nEmpty set 1 == Empty set 2: ${emptySet1 == emptySet2}")
    println("Empty set == Non-empty set: ${emptySet1 == set1}")
    
    return 0
}
```

Execution Result:

```text
Set 1 size: 3
Set 2 size: 3
Set 1 == Set 2: true

Set 3 size: 2
Set 1 == Set 3: false

Empty set 1 == Empty set 2: true
Empty set == Non-empty set: false
```

### operator func !=(HashSet\<T>)

```cangjie
public operator func !=(that: HashSet<T>): Bool
```

Function: Determines whether the current instance is not equal to the specified [HashSet](./collection_package_class.md#class-hashsett-where-t--hashable--equatablet)\<T> instance.

Parameters:

- that: [HashSet](./collection_package_class.md#class-hashsett-wwhere-t--hashable--equatablet)\<T> - The object to compare.

Returns:

- [Bool](../../core/core_package_api/core_package_intrinsics.md#bool) - Returns true if not equal, otherwise false.

Example:

<!-- verify -->
```cangjie
import std.collection.*

main() {
    // Create two different HashSets
    let set1 = HashSet<String>()
    set1.add("apple")
    set1.add("banana")
    
    let set2 = HashSet<String>()
    set2.add("apple")
    set2.add("orange")  // Different element
    
    println("Set 1: [apple, banana]")
    println("Set 2: [apple, orange]")
    println("Set 1 != Set 2: ${set1 != set2}")
    
    // Test identical sets
    let set3 = HashSet<String>()
    set3.add("apple")
    set3.add("banana")
    
    println("\nSet 3: [apple, banana]")
    println("Set 1 != Set 3: ${set1 != set3}")
    
    // Test empty vs non-empty sets
    let emptySet = HashSet<String>()
    
    println("\nEmpty set != Non-empty set: ${emptySet != set1}")
    
    // Test two empty sets
    let anotherEmptySet = HashSet<String>()
    println("Empty set 1 != Empty set 2: ${emptySet != anotherEmptySet}")
    
    return 0
}
```

Execution Result:

```text
Set 1: [apple, banana]
Set 2: [apple, orange]
Set 1 != Set 2: true

Set 3: [apple, banana]
Set 1 != Set 3: false

Empty set != Non-empty set: true
Empty set 1 != Empty set 2: false
```

## extend\<T> HashSet\<T> <: ToString where T <: ToString

```cangjie
extend<T> HashSet<T> <: ToString where T <: ToString
```

Function: Extends [HashSet](./collection_package_class.md#class-hashsett-where-t--hashable--equatablet)\<T> with [ToString](../../core/core_package_api/core_package_interfaces.md#interface-tostring) interface to support string conversion.

Extended type:

- [HashSet\<T>](./collection_package_class.md#class-hashsett-where-t--hashable--equatablet)

Extended interfaces:

- ToString

### func toString()

```cangjie
public func toString(): String
```

Function: Converts the current [HashSet](./collection_package_class.md#class-hashsett-where-t--hashable--equatablet)\<T> instance to a string.

The resulting string contains string representations of all elements in the [HashSet](./collection_package_class.md#class-hashsett-where-t--hashable--equatablet)\<T>, formatted as: "[elem1, elem2, elem3]".

Returns:

- [String](../../core/core_package_api/core_package_structs.md#struct-string) - The converted string.

Example:

<!-- verify -->
```cangjie
import std.collection.*

main() {
    // Create an empty HashSet
    let emptySet = HashSet<String>()
    println("Empty set: ${emptySet.toString()}")
    
    // Create a single-element HashSet
    let singleSet = HashSet<String>()
    singleSet.add("apple")
    println("Single-element set: ${singleSet.toString()}")
    
    // Create a multi-element HashSet
    let multiSet = HashSet<String>()
    multiSet.add("apple")
    multiSet.add("banana")
    multiSet.add("orange")
    
    println("Multi-element set: ${multiSet.toString()}")
    
    // Use numeric type HashSet
    let numSet = HashSet<Int64>()
    numSet.add(1)
    numSet.add(2)
    numSet.add(3)
    
    println("Numeric set: ${numSet.toString()}")
    
    // Direct usage in println (automatically calls toString)
    println("Automatic toString call: ${multiSet}")
    
    return 0
}
```

Execution results:

```text
Empty set: []
Single-element set: [apple]
Multi-element set: [apple, banana, orange]
Numeric set: [1, 2, 3]
Automatic toString call: [apple, banana, orange]
```

## extend\<T> LinkedList\<T>

```cangjie
extend<T> LinkedList<T>
```

Function: Extends the [LinkedList](./collection_package_class.md#class-linkedlistt)\<T> type.

Extended type:

- [LinkedList\<T>](./collection_package_class.md#class-linkedlistt)

### func enumerate()

```cangjie
public func enumerate(): LinkedList<(Int64, T)>
```

Function: Returns an indexed linked list.

Returns:

- [LinkedList](./collection_package_class.md#class-linkedlistt)\<([Int64](../../core/core_package_api/core_package_intrinsics.md#int64), T)> - Returns a new indexed [LinkedList](./collection_package_class.md#class-linkedlistt).

### func zip\<R>(LinkedList\<R>)

```cangjie
public func zip<R>(other: LinkedList<R>): LinkedList<(T, R)>
```

Function: Combines two [LinkedList](./collection_package_class.md#class-linkedlistt) instances into a new [LinkedList](./collection_package_class.md#class-linkedlistt) (length determined by the shorter list).


> **Note：**
>
> Unsupported platform：OpenHarmony

Parameters:

- other: [LinkedList](./collection_package_class.md#class-linkedlistt)\<R> - One of the lists to combine.

Returns:

- [LinkedList](./collection_package_class.md#class-linkedlistt)\<(T, R)> - Returns a new list.

## extend\<T> LinkedList\<T> <: Equatable\<LinkedList\<T>> where T <: Equatable\<T>

```cangjie
extend<T> LinkedList<T> <: Equatable<LinkedList<T>> where T <: Equatable<T>
```

Function: Extends the [LinkedList](./collection_package_class.md#class-linkedlistt)\<T> type with the [Equatable](../../core/core_package_api/core_package_interfaces.md#interface-equatablet)\<[LinkedList](./collection_package_class.md#class-linkedlistt)\<T>> interface, supporting equality comparison.

Extended type:

- [LinkedList\<T>](./collection_package_class.md#class-linkedlistt)

Extended interfaces:

- Equatable\<LinkedList\<T>>

### operator func ==(LinkedList\<T>)

```cangjie
public operator func ==(right: LinkedList<T>): Bool
```

Function: Determines whether the current instance is equal to the [LinkedList](./collection_package_class.md#class-linkedlistt)\<T> instance specified by the parameter.

Two [LinkedList](./collection_package_class.md#class-linkedlistt)\<T> instances are considered equal if they contain exactly the same elements.

Parameters:

- right: [LinkedList](./collection_package_class.md#class-linkedlistt)\<T> - The object to compare.

Returns:

- [Bool](../../core/core_package_api/core_package_intrinsics.md#bool) - Returns true if equal, otherwise false.

Example:

<!-- verify -->
```cangjie
import std.collection.*

main() {
    let list1 = LinkedList<Int64>()
    let list2 = LinkedList<Int64>()
    let list3 = LinkedList<Int64>()
    
    // Test equality of empty lists
    println("Two empty lists are equal: ${list1 == list2}")
    
    // Add identical elements to list1 and list2
    list1.addLast(1)
    list1.addLast(2)
    list1.addLast(3)
    
    list2.addLast(1)
    list2.addLast(2)
    list2.addLast(3)
    
    println("Lists with identical elements are equal: ${list1 == list2}")
    
    // Add different elements to list3
    list3.addLast(1)
    list3.addLast(2)
    list3.addLast(4) // Different element
    
    println("Lists with different elements are not equal: ${list1 == list3}")
    
    // Test lists of different sizes
    let list4 = LinkedList<Int64>()
    list4.addLast(1)
    list4.addLast(2)
    
    println("Lists of different sizes are not equal: ${list1 == list4}")
    
    // Test lists with same elements but different order
    let list5 = LinkedList<Int64>()
    list5.addLast(3)
    list5.addLast(2)
    list5.addLast(1)
    
    println("Lists with same elements but different order are not equal: ${list1 == list5}")
    
    // Display list contents for verification
    print("list1: ")
    for (element in list1) {
        print("${element} ")
    }
    println()
    
    print("list5: ")
    for (element in list5) {
        print("${element} ")
    }
    println()
    
    return 0
}
```

Output:

```text
Two empty lists are equal: true
Lists with identical elements are equal: true
Lists with different elements are not equal: false
Lists of different sizes are not equal: false
Lists with same elements but different order are not equal: false
list1: 1 2 3 
list5: 3 2 1 
```

### operator func !=(LinkedList\<T>)

```cangjie
public operator func !=(right: LinkedList<T>): Bool
```

Function: Determines whether the current instance is not equal to the [LinkedList](./collection_package_class.md#class-linkedlistt)\<T> instance specified by the parameter.

Parameters:

- right: [LinkedList](./collection_package_class.md#class-linkedlistt)\<T> - The object to compare.

Returns:

- [Bool](../../core/core_package_api/core_package_intrinsics.md#bool) - Returns true if not equal, otherwise false.

Example:

<!-- verify -->
```cangjie
import std.collection.*

main() {
    let list1 = LinkedList<String>()
    let list2 = LinkedList<String>()
    
    // Test inequality of empty lists
    println("Two empty lists are not equal: ${list1 != list2}")
    
    // Add elements to list1
    list1.addLast("First")
    list1.addLast("Second")
    
    // Test lists of different sizes
    println("Lists of different sizes are not equal: ${list1 != list2}")
    
    // Add identical elements to list2
    list2.addLast("First")
    list2.addLast("Second")
    
    println("Lists with identical elements are not equal: ${list1 != list2}")
    
    // Create a list with different elements
    let list3 = LinkedList<String>()
    list3.addLast("First")
    list3.addLast("Third") // Different element
    
    println("Lists with different elements are not equal: ${list1 != list3}")
    
    // Create a list with same elements but different order
    let list4 = LinkedList<String>()
    list4.addLast("Second")
    list4.addLast("First")
    
    println("Lists with same elements but different order are not equal: ${list1 != list4}")
    
    // Display all list contents for verification
    print("list1: ")
    for (element in list1) {
        print("${element} ")
    }
    println()
    
    print("list2: ")
    for (element in list2) {
        print("${element} ")
    }
    println()
    
    print("list3: ")
    for (element in list3) {
        print("${element} ")
    }
    println()
    
    print("list4: ")
    for (element in list4) {
        print("${element} ")
    }
    println()
    
    return 0
}
```

Output:

```text
Two empty lists are not equal: false
Lists of different sizes are not equal: true
Lists with identical elements are not equal: false
Lists with different elements are not equal: true
Lists with same elements but different order are not equal: true
list1: First Second 
list2: First Second 
list3: First Third 
list4: Second First 
```

## extend\<T> LinkedList\<T> <: ToString where T <: ToString

```cangjie
extend<T> LinkedList<T> <: ToString where T <: ToString
```

Function: Extends [LinkedList](./collection_package_class.md#class-linkedlistt)\<T> with the [ToString](../../core/core_package_api/core_package_interfaces.md#interface-tostring) interface, supporting string conversion.

Extended type:

- [LinkedList\<T>](./collection_package_class.md#class-linkedlistt)

Extended interfaces:

- ToString

### func toString()

```cangjie
public func toString(): String
```

Function: Converts the current [LinkedList](./collection_package_class.md#class-linkedlistt)\<T> instance to a string.

The string contains the string representation of each element in the [LinkedList](./collection_package_class.md#class-linkedlistt)\<T>, formatted as: "[elem1, elem2, elem3]".

Returns:

- [String](../../core/core_package_api/core_package_structs.md#struct-string) - The resulting string.

Example:

<!-- verify -->
```cangjie
import std.collection.*

main() {
    // Test empty list
    let emptyList = LinkedList<Int64>()
    println("toString of empty list: ${emptyList.toString()}")
    
    // Test single-element list
    let singleList = LinkedList<String>()
    singleList.addLast("SingleElement")
    println("toString of single-element list: ${singleList.toString()}")
    
    // Test multiple-element list
    let numberList = LinkedList<Int64>()
    numberList.addLast(10)
    numberList.addLast(20)
    numberList.addLast(30)
    numberList.addLast(40)
    println("toString of number list: ${numberList.toString()}")
    
    // Test string list
    let stringList = LinkedList<String>()
    stringList.addLast("Apple")
    stringList.addLast("Banana")
    stringList.addLast("Orange")
    println("toString of string list: ${stringList.toString()}")
    
    // Verify toString matches manually constructed string
    print("Manual traversal of number list: [")
    var isFirst = true
    for (element in numberList) {
        if (!isFirst) {
            print(", ")
        }
        print("${element}")
        isFirst = false
    }
    println("]")
    
    // Test modified toString
    numberList.addFirst(5)
    numberList.addLast(50)
    println("Modified number list toString: ${numberList.toString()}")
    
    return 0
}
```

Output:

```text
toString of empty list: []
toString of single-element list: [SingleElement]
toString of number list: [10, 20, 30, 40]
toString of string list: [Apple, Banana, Orange]
Manual traversal of number list: [10, 20, 30, 40]
Modified number list toString: [5, 10, 20, 30, 40, 50]
```

## extend\<T> TreeSet\<T> <: Equatable\<TreeSet\<T>>

```cangjie
extend<T> TreeSet<T> <: Equatable<TreeSet<T>>
```

Function: Extends [TreeSet](collection_package_class.md#class-treesett-where-t--comparablet)\<T> with [Equatable](../../core/core_package_api/core_package_interfaces.md#interface-equatablet)\<[TreeSet](collection_package_class.md#class-treesett-where-t--comparablet)\<T>> interface to support equality comparison.

Extended type:

- [TreeSet\<T>](collection_package_class.md#class-treesett-where-t--comparablet)

Extended interfaces:

- Equatable\<TreeSet\<T>>

### operator func ==(TreeSet\<T>)

```cangjie
public operator func ==(that: TreeSet<T>): Bool
```

Function: Determines whether the current instance is equal to the specified [TreeSet](collection_package_class.md#class-treesett-where-t--comparablet)\<T> instance.

Two [TreeSet](collection_package_class.md#class-treesett-where-t--comparablet)\<T> instances are considered equal if they contain exactly the same elements.

Parameters:

- that: [TreeSet](collection_package_class.md#class-treesett-where-t--comparablet)\<T> - The set to compare with.

Returns:

- [Bool](../../core/core_package_api/core_package_intrinsics.md#bool) - Returns true if equal, false otherwise.

Example:

<!-- verify -->
```cangjie
import std.collection.*

main() {
    let set1 = TreeSet<String>()
    set1.add("apple")
    set1.add("banana")
    set1.add("cherry")
    
    let set2 = TreeSet<String>()
    set2.add("apple")
    set2.add("banana")
    set2.add("cherry")
    
    let set3 = TreeSet<String>()
    set3.add("apple")
    set3.add("banana")
    set3.add("date")
    
    println("Set 1: ${set1}")
    println("Set 2: ${set2}")
    println("Set 3: ${set3}")
    
    // Compare equal sets
    let result1 = set1 == set2
    println("set1 == set2: ${result1}")
    
    // Compare unequal sets
    let result2 = set1 == set3
    println("set1 == set3: ${result2}")
    
    // Compare with self
    let result3 = set1 == set1
    println("set1 == set1: ${result3}")
    
    return 0
}
```

Output:

```text
Set 1: [apple, banana, cherry]
Set 2: [apple, banana, cherry]
Set 3: [apple, banana, date]
set1 == set2: true
set1 == set3: false
set1 == set1: true
```

### operator func !=(TreeSet\<T>)

```cangjie
public operator func !=(that: TreeSet<T>): Bool
```

Function: Determines whether the current instance is not equal to the specified [TreeSet](collection_package_class.md#class-treesett-where-t--comparablet)\<T> instance.

Parameters:

- that: [TreeSet](collection_package_class.md#class-treesett-where-t--comparablet)\<T> - The set to compare with.

Returns:

- [Bool](../../core/core_package_api/core_package_intrinsics.md#bool) - Returns true if not equal, false otherwise.

Example:

<!-- verify -->
```cangjie
import std.collection.*

main() {
    let set1 = TreeSet<String>()
    set1.add("apple")
    set1.add("banana")
    set1.add("cherry")
    
    let set2 = TreeSet<String>()
    set2.add("apple")
    set2.add("banana")
    set2.add("cherry")
    
    let set3 = TreeSet<String>()
    set3.add("apple")
    set3.add("banana")
    set3.add("date")
    
    println("Set 1: ${set1}")
    println("Set 2: ${set2}")
    println("Set 3: ${set3}")
    
    // Compare unequal sets
    let result1 = set1 != set3
    println("set1 != set3: ${result1}")
    
    // Compare equal sets
    let result2 = set1 != set2
    println("set1 != set2: ${result2}")
    
    // Compare with self
    let result3 = set1 != set1
    println("set1 != set1: ${result3}")
    
    return 0
}
```

Output:

```text
Set 1: [apple, banana, cherry]
Set 2: [apple, banana, cherry]
Set 3: [apple, banana, date]
set1 != set3: true
set1 != set2: false
set1 != set1: false
```

## extend\<T> TreeSet\<T> <: ToString where T <: ToString

```cangjie
extend<T> TreeSet<T> <: ToString where T <: ToString
```

Function: Extends the [TreeSet](collection_package_class.md#class-treesett-where-t--comparablet)\<T> with the [ToString](../../core/core_package_api/core_package_interfaces.md#interface-tostring) interface to support string conversion operations.

Extended type:

- [TreeSet\<T>](collection_package_class.md#class-treesett-where-t--comparablet)

Extended interfaces:

- ToString

### func toString()

```cangjie
public func toString(): String
```

Function: Converts the current [TreeSet](collection_package_class.md#class-treesett-where-t--comparablet)\<T> instance into a string.

The resulting string contains the string representation of each element in the [TreeSet](collection_package_class.md#class-treesett-where-t--comparablet)\<T>, formatted as: "[elem1, elem2, elem3]".

Returns:

- [String](../../core/core_package_api/core_package_structs.md#struct-string) - The converted string.

Example:

<!-- verify -->
```cangjie
import std.collection.*

main() {
    let set = TreeSet<String>()
    set.add("apple")
    set.add("banana")
    set.add("cherry")
    
    println("TreeSet contents: ${set}")
    
    // Convert to string
    let str = set.toString()
    
    println("Converted string: ${str}")
    
    // String representation of an empty set
    let emptySet = TreeSet<String>()
    let emptyStr = emptySet.toString()
    
    println("Empty set string: ${emptyStr}")
    
    return 0
}
```

Execution Result:

```text
TreeSet contents: [apple, banana, cherry]
Converted string: [apple, banana, cherry]
Empty set string: []
```

