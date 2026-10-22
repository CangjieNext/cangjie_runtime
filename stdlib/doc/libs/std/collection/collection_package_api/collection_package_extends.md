# 扩展

## extend\<K, V> HashMap\<K, V> <: Equatable\<HashMap\<K, V>> where V <: Equatable\<V>

```cangjie
extend<K, V> HashMap<K, V> <: Equatable<HashMap<K, V>> where V <: Equatable<V>
```

功能：为 [HashMap](./collection_package_class.md#class-hashmapk-v-where-k--hashable--equatablek)\<K, V> 类型扩展 [Equatable](../../core/core_package_api/core_package_interfaces.md#interface-equatablet)\<[HashMap](./collection_package_class.md#class-hashmapk-v-where-k--hashable--equatablek)\<K, V>> 接口，支持判等操作。

被扩展类型：

- [HashMap\<K, V>](./collection_package_class.md#class-hashmapk-v-where-k--hashable--equatablek)

扩展接口：

- [Equatable](../../core/core_package_api/core_package_interfaces.md#interface-equatablet)\<[HashMap](./collection_package_class.md#class-hashmapk-v-where-k--hashable--equatablek)\<K, V>>

### operator func !=(HashMap\<K, V>)

```cangjie
public operator func !=(right: HashMap<K, V>): Bool
```

功能：判断当前实例与参数指向的 [HashMap](./collection_package_class.md#class-hashmapk-v-where-k--hashable--equatablek)\<K, V> 实例是否不等。

参数：

- right: [HashMap](./collection_package_class.md#class-hashmapk-v-where-k--hashable--equatablek)\<K, V> - 被比较的对象。

返回值：

- [Bool](../../core/core_package_api/core_package_intrinsics.md#bool) - 如果不等，则返回 true，否则返回 false。

示例：

<!-- verify -->
```cangjie
import std.collection.*

main() {
    // 创建两个HashMap
    let map1 = HashMap<String, Int64>()
    map1["x"] = 10
    map1["y"] = 20

    let map2 = HashMap<String, Int64>()
    map2["x"] = 10
    map2["y"] = 30

    let map3 = HashMap<String, Int64>()
    map3["y"] = 20
    map3["x"] = 10

    // 比较不相等的HashMap
    println("map1 != map2: ${map1 != map2}") // true

    // 比较相等的HashMap
    println("map1 != map3: ${map1 != map3}") // false

    return 0
}
```

运行结果：

```text
map1 != map2: true
map1 != map3: false
```

### operator func ==(HashMap\<K, V>)

```cangjie
public operator func ==(right: HashMap<K, V>): Bool
```

功能：判断当前实例与参数指向的 [HashMap](./collection_package_class.md#class-hashmapk-v-where-k--hashable--equatablek)\<K, V> 实例是否相等。

两个 [HashMap](./collection_package_class.md#class-hashmapk-v-where-k--hashable--equatablek)\<K, V> 相等指的是其中包含的键值对完全相等。

参数：

- right: [HashMap](./collection_package_class.md#class-hashmapk-v-where-k--hashable--equatablek)\<K, V> - 被比较的对象。

返回值：

- [Bool](../../core/core_package_api/core_package_intrinsics.md#bool) - 如果相等，则返回 true，否则返回 false。

示例：

<!-- verify -->
```cangjie
import std.collection.*

main() {
    // 创建两个HashMap
    let map1 = HashMap<String, Int64>()
    map1["a"] = 1
    map1["b"] = 2

    let map2 = HashMap<String, Int64>()
    map2["b"] = 2
    map2["a"] = 1

    let map3 = HashMap<String, Int64>()
    map3["a"] = 1
    map3["b"] = 3

    // 比较相等的HashMap
    println("map1 == map2: ${map1 == map2}") // true

    // 比较不相等的HashMap
    println("map1 == map3: ${map1 == map3}") // false

    return 0
}
```

运行结果：

```text
map1 == map2: true
map1 == map3: false
```

## extend\<K, V> HashMap\<K, V> <: ToString where V <: ToString, K <: ToString

```cangjie
extend<K, V> HashMap<K, V> <: ToString where V <: ToString, K <: ToString
```

功能：为 [HashMap](./collection_package_class.md#class-hashmapk-v-where-k--hashable--equatablek)\<K, V> 扩展 [ToString](../../core/core_package_api/core_package_interfaces.md#interface-tostring) 接口，支持转字符串操作。

被扩展类型：

- [HashMap\<K, V>](./collection_package_class.md#class-hashmapk-v-where-k--hashable--equatablek)

扩展接口：

- [ToString](../../core/core_package_api/core_package_interfaces.md#interface-tostring)

### func toString()

```cangjie
public func toString(): String
```

功能：将当前 [HashMap](./collection_package_class.md#class-hashmapk-v-where-k--hashable--equatablek)\<K, V> 实例转换为字符串。

该字符串包含 [HashMap](./collection_package_class.md#class-hashmapk-v-where-k--hashable--equatablek)\<K, V> 内每个键值对的字符串表示，形如："[(k1, v1), (k2, v2), (k3, v3)]"。

返回值：

- [String](../../core/core_package_api/core_package_structs.md#struct-string) - 转换得到的字符串。

示例：

<!-- verify -->
```cangjie
import std.collection.*

main() {
    // 创建HashMap
    let map = HashMap<String, Int64>()
    map["apple"] = 5
    map["banana"] = 3
    map["orange"] = 8

    // 转换为字符串
    let mapString = map.toString()

    println("空集合的字符串: ${HashMap<String, Int64>().toString()}") // []
    println("HashMap的字符串表示: ${mapString}")

    return 0
}
```

运行结果：

```text
空集合的字符串: []
HashMap的字符串表示: [(apple, 5), (banana, 3), (orange, 8)]
```

## extend\<K, V> TreeMap\<K, V> <: Equatable\<TreeMap\<K, V>> where V <: Equatable\<V>

```cangjie
extend<K, V> TreeMap<K, V> <: Equatable<TreeMap<K, V>> where V <: Equatable<V>
```

功能：为 [TreeMap](./collection_package_class.md#class-treemapk-v-where-k--comparablek)\<K, V> 类型扩展 [Equatable](../../core/core_package_api/core_package_interfaces.md#interface-equatablet)\<[TreeMap](./collection_package_class.md#class-treemapk-v-where-k--comparablek)\<K, V>> 接口，支持判等操作。

被扩展类型：

- [TreeMap\<K, V>](./collection_package_class.md#class-treemapk-v-where-k--comparablek)

扩展接口：

- [Equatable](../../core/core_package_api/core_package_interfaces.md#interface-equatablet)\<[TreeMap](./collection_package_class.md#class-treemapk-v-where-k--comparablek)\<K, V>>

### operator func !=(TreeMap\<K, V>)

```cangjie
public operator func !=(right: TreeMap<K, V>): Bool
```

功能：判断当前实例与参数指向的 [TreeMap](./collection_package_class.md#class-treemapk-v-where-k--comparablek)\<K, V> 实例是否不等。

参数：

- right: [TreeMap](./collection_package_class.md#class-treemapk-v-where-k--comparablek)\<K, V> - 被比较的对象。

返回值：

- [Bool](../../core/core_package_api/core_package_intrinsics.md#bool) - 如果不等，则返回 true，否则返回 false。

示例：

<!-- verify -->
```cangjie
import std.collection.*

main() {
    // 创建两个相同的TreeMap
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

    // 创建一个不同的TreeMap
    let map3 = TreeMap<String, Int64>()
    map3.add("apple", 10)
    map3.add("banana", 20)
    map3.add("date", 40) // 不同的键值对

    println("\nmap3: ${map3}")
    println("map1 != map3: ${map1 != map3}")

    // 创建一个键相同但值不同的TreeMap
    let map4 = TreeMap<String, Int64>()
    map4.add("apple", 15) // 不同的值
    map4.add("banana", 20)
    map4.add("cherry", 30)

    println("\nmap4: ${map4}")
    println("map1 != map4: ${map1 != map4}")

    // 两个空映射的比较
    let emptyMap = TreeMap<String, Int64>()
    let anotherEmptyMap = TreeMap<String, Int64>()
    println("emptyMap != anotherEmptyMap: ${emptyMap != anotherEmptyMap}")

    return 0
}
```

运行结果：

```text
map1: [(apple, 10), (banana, 20), (cherry, 30)]
map2: [(apple, 10), (banana, 20), (cherry, 30)]
map1 != map2: false

map3: [(apple, 10), (banana, 20), (date, 40)]
map1 != map3: true

map4: [(apple, 15), (banana, 20), (cherry, 30)]
map1 != map4: true
emptyMap != anotherEmptyMap: false
```

### operator func ==(TreeMap\<K, V>)

```cangjie
public operator func ==(right: TreeMap<K, V>): Bool
```

功能：判断当前实例与参数指向的 [TreeMap](./collection_package_class.md#class-treemapk-v-where-k--comparablek)\<K, V> 实例是否相等。

两个 [TreeMap](./collection_package_class.md#class-treemapk-v-where-k--comparablek)\<K, V> 相等指的是其中包含的键值对完全相等。

参数：

- right: [TreeMap](./collection_package_class.md#class-treemapk-v-where-k--comparablek)\<K, V> - 被比较的对象。

返回值：

- [Bool](../../core/core_package_api/core_package_intrinsics.md#bool) - 如果相等，则返回 true，否则返回 false。

示例：

<!-- verify -->
```cangjie
import std.collection.*

main() {
    // 创建两个相同的TreeMap
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

    // 创建一个不同的TreeMap
    let map3 = TreeMap<String, Int64>()
    map3.add("apple", 10)
    map3.add("banana", 20)
    map3.add("date", 40) // 不同的键值对

    println("\nmap3: ${map3}")
    println("map1 == map3: ${map1 == map3}")

    // 创建一个键相同但值不同的TreeMap
    let map4 = TreeMap<String, Int64>()
    map4.add("apple", 15) // 不同的值
    map4.add("banana", 20)
    map4.add("cherry", 30)

    println("\nmap4: ${map4}")
    println("map1 == map4: ${map1 == map4}")
    return 0
}
```

运行结果：

```text
map1: [(apple, 10), (banana, 20), (cherry, 30)]
map2: [(apple, 10), (banana, 20), (cherry, 30)]
map1 == map2: true

map3: [(apple, 10), (banana, 20), (date, 40)]
map1 == map3: false

map4: [(apple, 15), (banana, 20), (cherry, 30)]
map1 == map4: false
```

## extend\<K, V> TreeMap\<K, V> <: ToString where V <: ToString, K <: ToString & Comparable\<K>

```cangjie
extend<K, V> TreeMap<K, V> <: ToString where V <: ToString, K <: ToString & Comparable<K>
```

功能：为 [TreeMap](./collection_package_class.md#class-treemapk-v-where-k--comparablek)\<K, V> 扩展 [ToString](../../core/core_package_api/core_package_interfaces.md#interface-tostring) 接口，支持转字符串操作。

被扩展类型：

- [TreeMap\<K, V>](./collection_package_class.md#class-treemapk-v-where-k--comparablek)

扩展接口：

- [ToString](../../core/core_package_api/core_package_interfaces.md#interface-tostring)

### func toString()

```cangjie
public func toString(): String
```

功能：将当前 [TreeMap](./collection_package_class.md#class-treemapk-v-where-k--comparablek)\<K, V> 实例转换为字符串。

该字符串包含 [TreeMap](./collection_package_class.md#class-treemapk-v-where-k--comparablek)\<K, V> 内每个键值对的字符串表示，形如："[(k1, v1), (k2, v2), (k3, v3)]"。

返回值：

- [String](../../core/core_package_api/core_package_structs.md#struct-string) - 转换得到的字符串。

示例：

<!-- verify -->
```cangjie
import std.collection.*

main() {
    // 空映射的字符串表示
    let emptyMap = TreeMap<String, Int64>()
    println("空映射: ${emptyMap.toString()}")

    // 非空映射的字符串表示
    let map = TreeMap<String, Int64>()
    map.add("cherry", 30)
    map.add("apple", 10)
    map.add("banana", 20)

    println("非空映射: ${map.toString()}")

    // 直接打印（自动调用toString）
    println("直接打印: ${map}")

    return 0
}
```

运行结果：

```text
空映射: []
非空映射: [(apple, 10), (banana, 20), (cherry, 30)]
直接打印: [(apple, 10), (banana, 20), (cherry, 30)]
```

## extend\<T> ArrayDeque\<T> <: ToString where T <: ToString

```cangjie
extend<T> ArrayDeque<T> <: ToString where T <: ToString
```

功能：为 [ArrayDeque](./collection_package_class.md#class-arraydequet)\<T> 扩展 [ToString](../../core/core_package_api/core_package_interfaces.md#interface-tostring) 接口，支持转字符串操作。

被扩展类型：

- [ArrayDeque\<T>](./collection_package_class.md#class-arraydequet)

扩展接口：

- [ToString](../../core/core_package_api/core_package_interfaces.md#interface-tostring)

### func toString()

```cangjie
public func toString(): String
```

功能：获取当前[ArrayDeque](./collection_package_class.md#class-arraydequet)\<T>实例的字符串表示。

该字符串包含双端队列内每个元素的字符串表示，其顺序为从前到后的顺序，形如："[elem1, elem2, elem3]"。

返回值：

- [String](../../core/core_package_api/core_package_structs.md#struct-string) - 转换得到的字符串。

示例：

<!-- verify -->
```cangjie
import std.collection.*

main() {
    let deque = ArrayDeque<Int64>()
    deque.addLast(1)
    deque.addLast(2)
    deque.addLast(3)
    println(deque.toString()) // [1, 2, 3]
    return 0
}
```

运行结果：

```text
[1, 2, 3]
```

## extend\<T> ArrayList\<T>

```cangjie
extend<T> ArrayList<T>
```

功能：为 [ArrayList](collection_package_class.md#class-arraylistt)\<T> 类型进行拓展

被扩展类型：

- [ArrayList\<T>](collection_package_class.md#class-arraylistt)

### func enumerate()

```cangjie
public func enumerate(): ArrayList<(Int64, T)>
```

功能：用于获取带索引的 [ArrayList](collection_package_class.md#class-arraylistt) 。

> **注意：**
>
> 不支持平台：OpenHarmony。

返回值：

- [ArrayList](collection_package_class.md#class-arraylistt)\<([Int64](../../core/core_package_api/core_package_intrinsics.md#int64), T)> - 返回一个带索引的新 [ArrayList](collection_package_class.md#class-arraylistt)。

示例：

<!-- verify -->
```cangjie
import std.collection.*

main() {
    let list = ArrayList<String>(["apple", "banana", "cherry"])

    // 获取带索引的列表
    let enumerated = list.enumerate()

    println("原列表: ${list}") // ["apple", "banana", "cherry"]

    // 遍历带索引的列表
    enumerated.forEach({
        element => println("索引 ${element[0]}: ${element[1]}")
    })

    return 0
}
```

运行结果：

```text
原列表: [apple, banana, cherry]
索引 0: apple
索引 1: banana
索引 2: cherry
```

### func zip\<R>(ArrayList\<R>)

```cangjie
public func zip<R>(other: ArrayList<R>): ArrayList<(T, R)>
```

功能：将两个 [ArrayList](collection_package_class.md#class-arraylistt) 合并成一个新 [ArrayList](collection_package_class.md#class-arraylistt)（长度取决于短的那个 [ArrayList](collection_package_class.md#class-arraylistt)）。

参数：

- other: [ArrayList](collection_package_class.md#class-arraylistt)\<R> - 要合并的其中一个 [ArrayList](collection_package_class.md#class-arraylistt)。

返回值：

- [ArrayList](collection_package_class.md#class-arraylistt)\<(T, R)> - 返回一个新 [ArrayList](collection_package_class.md#class-arraylistt) 。

示例：

<!-- verify -->
```cangjie
import std.collection.*

main() {
    let names = ArrayList<String>(["Alice", "Bob", "Charlie"])

    let ages = ArrayList<Int64>([25, 30, 35])

    // 将两个列表合并
    let zipped = names.zip<Int64>(ages)

    println("姓名列表: ${names}") // ["Alice", "Bob", "Charlie"]
    println("年龄列表: ${ages}") // [25, 30, 35]

    // 遍历合并后的列表
    zipped.forEach({
        element => println("${element[0]} 的年龄是 ${element[1]} 岁")
    })

    return 0
}
```

运行结果：

```text
姓名列表: [Alice, Bob, Charlie]
年龄列表: [25, 30, 35]
Alice 的年龄是 25 岁
Bob 的年龄是 30 岁
Charlie 的年龄是 35 岁
```

<!--DelEnd-->

## extend\<T> ArrayList\<T> <: Equatable\<ArrayList\<T>> where T <: Equatable\<T>

```cangjie
extend<T> ArrayList<T> <: Equatable<ArrayList<T>> where T <: Equatable<T>
```

功能：为 [ArrayList](./collection_package_class.md#class-arraylistt)\<T> 类型扩展 [Equatable](../../core/core_package_api/core_package_interfaces.md#interface-equatablet)\<[ArrayList](./collection_package_class.md#class-arraylistt)\<T>> 接口，支持判等操作。

被扩展类型：

- [ArrayList\<T>](./collection_package_class.md#class-arraylistt)

扩展接口：

- [Equatable](../../core/core_package_api/core_package_interfaces.md#interface-equatablet)\<[ArrayList](collection_package_class.md#class-arraylistt)\<T>>

### func contains(T)

```cangjie
public func contains(element: T): Bool
```

功能：判断当前容器中是否包含指定元素。

参数：

- element: T - 待寻找的元素。

返回值：

- [Bool](../../core/core_package_api/core_package_intrinsics.md#bool) - 如果数组中包含指定元素，返回 true，否则返回 false。

示例：

<!-- verify -->
```cangjie
import std.collection.*

main() {
    let list = ArrayList<Int64>([1, 2, 3])

    println("列表包含元素2: ${list.contains(2)}") // true
    println("列表包含元素5: ${list.contains(5)}") // false

    return 0
}
```

运行结果：

```text
列表包含元素2: true
列表包含元素5: false
```

### operator func !=(ArrayList\<T>)

```cangjie
public operator func !=(other: ArrayList<T>): Bool
```

功能：判断当前实例与参数指向的 [ArrayList](./collection_package_class.md#class-arraylistt) 实例是否不等。

参数：

- other: [ArrayList](./collection_package_class.md#class-arraylistt)\<T> - 被比较的对象。

返回值：

- [Bool](../../core/core_package_api/core_package_intrinsics.md#bool) - 如果不等，则返回 true，否则返回 false。

示例：

<!-- verify -->
```cangjie
import std.collection.*

main() {
    let list1 = ArrayList<Int64>([1, 2, 3])
    let list2 = ArrayList<Int64>([1, 2])
    let list3 = ArrayList<Int64>([1, 2, 3])

    println("list1 != list2: ${list1 != list2}") // true
    println("list1 != list3: ${list1 != list3}") // false

    return 0
}
```

运行结果：

```text
list1 != list2: true
list1 != list3: false
```

### operator func ==(ArrayList\<T>)

```cangjie
public operator func ==(other: ArrayList<T>): Bool
```

功能：判断当前实例与参数指向的 [ArrayList](./collection_package_class.md#class-arraylistt) 实例是否相等。

两个数组相等指的是两者对应位置的元素分别相等。

参数：

- other: [ArrayList](./collection_package_class.md#class-arraylistt)\<T> - 被比较的对象。

返回值：

- [Bool](../../core/core_package_api/core_package_intrinsics.md#bool) - 如果相等，则返回 true，否则返回 false。

示例：

<!-- verify -->
```cangjie
import std.collection.*

main() {
    let list1 = ArrayList<Int64>([1, 2, 3])
    let list2 = ArrayList<Int64>([1, 2, 3])
    let list3 = ArrayList<Int64>([1, 2])

    println("list1 == list2: ${list1 == list2}") // true
    println("list1 == list3: ${list1 == list3}") // false

    return 0
}
```

运行结果：

```text
list1 == list2: true
list1 == list3: false
```

## extend\<T> ArrayList\<T> <: SortExtension where T <: Comparable\<T>

```cangjie
extend<T> ArrayList<T> where T <: Comparable<T>
```

功能：为 [ArrayList](./collection_package_class.md#class-arraylistt)\<T> 扩展 [SortExtension](../../sort/sort_package_api/sort_package_interfaces.md#interface-sortextension-deprecated) 接口，支持数组排序。

被扩展类型：

- [ArrayList\<T>](./collection_package_class.md#class-arraylistt)

扩展接口：

- [SortExtension](../../sort/sort_package_api/sort_package_interfaces.md#interface-sortextension-deprecated)

### func sort() <sup>(deprecated)</sup>

```cangjie
public func sort(): Unit
```

功能：将当前数组内元素以升序的方式非稳定排序。

> **注意：**
>
> 未来版本即将废弃，使用 [sort](../../sort/sort_package_api/sort_package_funcs.md#func-sorttlistt-bool-bool-where-t--comparablet) 替代。

示例：

<!-- verify -->
```cangjie
import std.collection.*

main() {
    let list = ArrayList<Int64>([3, 1, 4, 2])
    println("排序前: ${list}") // [3, 1, 4, 2]

    // 升序排序
    list.sort()

    println("排序后: ${list}") // [1, 2, 3, 4]

    return 0
}
```

运行结果：

```text
排序前: [3, 1, 4, 2]
排序后: [1, 2, 3, 4]
```

### func sort(Bool) <sup>(deprecated)</sup>

```cangjie
public func sort(stable!: Bool): Unit
```

功能：将当前数组内元素以升序的方式排序。

> **注意：**
>
> 未来版本即将废弃，使用 [sort](../../sort/sort_package_api/sort_package_funcs.md#func-sorttlistt-bool-bool-where-t--comparablet) 替代。

参数：

- stable!: [Bool](../../core/core_package_api/core_package_intrinsics.md#bool) - 是否使用稳定排序。

示例：

<!-- verify -->
```cangjie
import std.collection.*

main() {
    let list = ArrayList<Int64>([3, 1, 4, 2])

    println("排序前: ${list}")

    // 使用稳定排序升序排序
    list.sort(stable: true)

    println("稳定排序后: ${list}") // [1, 2, 3, 4]

    return 0
}
```

运行结果：

```text
排序前: [3, 1, 4, 2]
稳定排序后: [1, 2, 3, 4]
```

### func sortDescending() <sup>(deprecated)</sup>

```cangjie
public func sortDescending(): Unit
```

功能：将当前数组内元素以降序的方式非稳定排序。

> **注意：**
>
> 未来版本即将废弃，使用 [sort](../../sort/sort_package_api/sort_package_funcs.md#func-sorttlistt-bool-bool-where-t--comparablet) 替代。

示例：

<!-- verify -->
```cangjie
import std.collection.*

main() {
    let list = ArrayList<Int64>([3, 1, 4, 2])
    println("排序前: ${list}") // [3, 1, 4, 2]

    // 降序排序
    list.sortDescending()

    println("降序排序后: ${list}") // [4, 3, 2, 1]

    return 0
}
```

运行结果：

```text
排序前: [3, 1, 4, 2]
降序排序后: [4, 3, 2, 1]
```

### func sortDescending(Bool) <sup>(deprecated)</sup>

```cangjie
public func sortDescending(stable!: Bool): Unit
```

功能：将当前数组内元素以降序的方式排序。

> **注意：**
>
> 未来版本即将废弃，使用 [sort](../../sort/sort_package_api/sort_package_funcs.md#func-sorttlistt-bool-bool-where-t--comparablet) 替代。

参数：

- stable!: [Bool](../../core/core_package_api/core_package_intrinsics.md#bool) - 是否使用稳定排序。

示例：

<!-- verify -->
```cangjie
import std.collection.*

main() {
    let list = ArrayList<Int64>([3, 1, 4, 2])
    println("排序前: ${list}") // [3, 1, 4, 2]

    // 使用稳定排序降序排序
    list.sortDescending(stable: true)

    println("稳定降序排序后: ${list}") // [4, 3, 2, 1]

    return 0
}
```

运行结果：

```text
排序前: [3, 1, 4, 2]
稳定降序排序后: [4, 3, 2, 1]
```

## extend\<T> ArrayList\<T> <: ToString where T <: ToString

```cangjie
extend<T> ArrayList<T> <: ToString where T <: ToString
```

功能：为 [ArrayList](./collection_package_class.md#class-arraylistt)\<T> 扩展 [ToString](../../core/core_package_api/core_package_interfaces.md#interface-tostring) 接口，支持转字符串操作。

被扩展类型：

- [ArrayList\<T>](./collection_package_class.md#class-arraylistt)

扩展接口：

- [ToString](../../core/core_package_api/core_package_interfaces.md#interface-tostring)

### func toString()

```cangjie
public func toString(): String
```

功能：将当前数组转换为字符串。

该字符串包含数组内每个元素的字符串表示，形如："[elem1, elem2, elem3]"。

返回值：

- [String](../../core/core_package_api/core_package_structs.md#struct-string) - 转换得到的字符串。

示例：

<!-- verify -->
```cangjie
import std.collection.*

main() {
    let list = ArrayList<Int64>([1, 2, 3])

    let str = list.toString()

    println("字符串: ${str}") // [1, 2, 3]

    return 0
}
```

运行结果：

```text
字符串: [1, 2, 3]
```

## extend\<T> ArrayQueue\<T> <: ToString where T <: ToString

```cangjie
extend<T> ArrayQueue<T> <: ToString where T <: ToString
```

功能：为 [ArrayQueue](./collection_package_class.md#class-arrayqueuet)\<T> 扩展 [ToString](../../core/core_package_api/core_package_interfaces.md#interface-tostring) 接口，支持转字符串操作。

被扩展类型：

- [ArrayQueue\<T>](./collection_package_class.md#class-arrayqueuet)

扩展接口：

- [ToString](../../core/core_package_api/core_package_interfaces.md#interface-tostring)

### func toString()

```cangjie
public func toString(): String
```

功能：获取当前[ArrayQueue](./collection_package_class.md#class-arrayqueuet)\<T>实例的字符串表示。

该字符串包含队列内每个元素的字符串表示，其顺序为从前到后的顺序，形如："[elem1, elem2, elem3]"。

返回值：

- [String](../../core/core_package_api/core_package_structs.md#struct-string) - 转换得到的字符串。

示例：

<!-- verify -->
```cangjie
import std.collection.*

main() {
    let queue = ArrayQueue<Int64>()

    // 空队列转字符串
    let emptyStr = queue.toString()
    println("空队列字符串: ${emptyStr}") // []

    // 添加元素后转字符串
    queue.add(1)
    queue.add(2)
    queue.add(3)

    let str = queue.toString()
    println("队列字符串: ${str}") // [1, 2, 3]
    println("字符串长度: ${str.size}") // 9

    return 0
}
```

运行结果：

```text
空队列字符串: []
队列字符串: [1, 2, 3]
字符串长度: 9
```

## extend\<T> ArrayStack\<T> <: ToString where T <: ToString

```cangjie
extend<T> ArrayStack<T> <: ToString where T <: ToString
```

功能：为 ArrayStack 扩展 [ToString](../../core/core_package_api/core_package_interfaces.md#interface-tostring) 接口，支持转字符串操作。

被扩展类型：

- [ArrayStack\<T>](./collection_package_class.md#class-arraystackt)

扩展接口：

- [ToString](../../core/core_package_api/core_package_interfaces.md#interface-tostring)

### func toString()

```cangjie
public func toString(): String
```

功能：获取当前 [ArrayStack](./collection_package_class.md#class-arraystackt)\<T> 实例的字符串表示。

该字符串包含栈内每个元素的字符串表示，其顺序为从后到前的顺序。形如："[elem1, elem2, elem3]"。

返回值：

- [String](../../core/core_package_api/core_package_structs.md#struct-string) - 当前栈的字符串表示。

示例：

<!-- verify -->
```cangjie
import std.collection.*

main() {
    let stack = ArrayStack<Int64>()

    // 空栈转字符串
    let emptyStr = stack.toString()
    println("空栈字符串: ${emptyStr}") // []

    // 添加元素后转字符串
    stack.add(1)
    stack.add(2)
    stack.add(3)

    let str = stack.toString()
    println("栈字符串: ${str}") // [3, 2, 1]
    println("字符串长度: ${str.size}") // 9

    return 0
}
```

运行结果：

```text
空栈字符串: []
栈字符串: [3, 2, 1]
字符串长度: 9
```

## extend\<T> HashSet\<T> <: Equatable\<HashSet\<T>>

```cangjie
extend<T> HashSet<T> <: Equatable<HashSet<T>>
```

功能：为 [HashSet](./collection_package_class.md#class-hashsett-where-t--hashable--equatablet)\<T> 类型扩展 [Equatable](../../core/core_package_api/core_package_interfaces.md#interface-equatablet)\<[HashSet](./collection_package_class.md#class-hashsett-where-t--hashable--equatablet)\<T>> 接口，支持判等操作。

被扩展类型：

- [HashSet\<T>](./collection_package_class.md#class-hashsett-where-t--hashable--equatablet)

扩展接口：

- [Equatable](../../core/core_package_api/core_package_interfaces.md#interface-equatablet)\<[HashSet](./collection_package_class.md#class-hashsett-where-t--hashable--equatablet)\<T>>

### operator func !=(HashSet\<T>)

```cangjie
public operator func !=(other: HashSet<T>): Bool
```

功能：判断当前实例与参数指向的 [HashSet](./collection_package_class.md#class-hashsett-where-t--hashable--equatablet)\<T> 实例是否不等。

参数：

- other: [HashSet](./collection_package_class.md#class-hashsett-where-t--hashable--equatablet)\<T> - 被比较的对象。

返回值：

- [Bool](../../core/core_package_api/core_package_intrinsics.md#bool) - 如果不等，则返回 true，否则返回 false。

示例：

<!-- verify -->
```cangjie
import std.collection.*

main() {
    // 创建两个不同的HashSet
    let set1 = HashSet<String>(["apple", "banana"])

    let set2 = HashSet<String>(["apple", "orange"])

    println("集合1: ${set1}")
    println("集合2: ${set2}")
    println("集合1 != 集合2: ${set1 != set2}")

    // 测试相同的集合
    let set3 = HashSet<String>(["apple", "banana"])

    println("\n集合3: ${set3}")
    println("集合1 != 集合3: ${set1 != set3}")

    // 测试空集合和非空集合
    let emptySet = HashSet<String>()

    println("\n空集合 != 非空集合: ${emptySet != set1}")

    // 测试两个空集合
    let anotherEmptySet = HashSet<String>()
    println("空集合1 != 空集合2: ${emptySet != anotherEmptySet}")

    return 0
}
```

运行结果：

```text
集合1: [apple, banana]
集合2: [apple, orange]
集合1 != 集合2: true

集合3: [apple, banana]
集合1 != 集合3: false

空集合 != 非空集合: true
空集合1 != 空集合2: false
```

### operator func ==(HashSet\<T>)

```cangjie
public operator func ==(other: HashSet<T>): Bool
```

功能：判断当前实例与参数指向的 [HashSet](./collection_package_class.md#class-hashsett-where-t--hashable--equatablet)\<T> 实例是否相等。

两个 [HashSet](./collection_package_class.md#class-hashsett-where-t--hashable--equatablet)\<T> 相等指的是其中包含的元素完全相等。

参数：

- other: [HashSet](./collection_package_class.md#class-hashsett-where-t--hashable--equatablet)\<T> - 被比较的对象。

返回值：

- [Bool](../../core/core_package_api/core_package_intrinsics.md#bool) - 如果相等，则返回 true，否则返回 false。

示例：

<!-- verify -->
```cangjie
import std.collection.*

main() {
    // 创建两个相同的HashSet
    let set1 = HashSet<String>(["apple", "banana", "orange"])

    let set2 = HashSet<String>(["banana", "apple", "orange"])

    // 比较相同的集合
    println("集合1 == 集合2: ${set1 == set2}")

    // 创建不同的HashSet
    let set3 = HashSet<String>(["apple", "grape"])

    println("集合1 == 集合3: ${set1 == set3}")

    // 测试空集合
    let emptySet1 = HashSet<String>()
    let emptySet2 = HashSet<String>()

    println("\n空集合1 == 空集合2: ${emptySet1 == emptySet2}")
    println("空集合 == 非空集合: ${emptySet1 == set1}")

    return 0
}
```

运行结果：

```text
集合1 == 集合2: true
集合1 == 集合3: false

空集合1 == 空集合2: true
空集合 == 非空集合: false
```

## extend\<T> HashSet\<T> <: ToString where T <: ToString

```cangjie
extend<T> HashSet<T> <: ToString where T <: ToString
```

功能：为 [HashSet](./collection_package_class.md#class-hashsett-where-t--hashable--equatablet)\<T> 扩展 [ToString](../../core/core_package_api/core_package_interfaces.md#interface-tostring) 接口，支持转字符串操作。

被扩展类型：

- [HashSet\<T>](./collection_package_class.md#class-hashsett-where-t--hashable--equatablet)

扩展接口：

- [ToString](../../core/core_package_api/core_package_interfaces.md#interface-tostring)

### func toString()

```cangjie
public func toString(): String
```

功能：将当前 [HashSet](./collection_package_class.md#class-hashsett-where-t--hashable--equatablet)\<T> 实例转换为字符串。

该字符串包含 [HashSet](./collection_package_class.md#class-hashsett-where-t--hashable--equatablet)\<T> 内每个元素的字符串表示，形如："[elem1, elem2, elem3]"。

返回值：

- [String](../../core/core_package_api/core_package_structs.md#struct-string) - 转换得到的字符串。

示例：

<!-- verify -->
```cangjie
import std.collection.*

main() {
    // 创建空HashSet
    let emptySet = HashSet<String>()
    println("空集合: ${emptySet.toString()}")

    // 创建多元素HashSet
    let multiSet = HashSet<String>(["apple", "banana", "orange"])
    println("多元素集合: ${multiSet.toString()}")

    // 在println中直接使用（自动调用toString）
    println("自动调用toString: ${multiSet}")
    return 0
}
```

运行结果：

```text
空集合: []
多元素集合: [apple, banana, orange]
自动调用toString: [apple, banana, orange]
```

## extend\<T> LinkedList\<T>

```cangjie
extend<T> LinkedList<T>
```

功能：为 [LinkedList](./collection_package_class.md#class-linkedlistt)\<T> 类型进行拓展

被扩展类型：

- [LinkedList\<T>](./collection_package_class.md#class-linkedlistt)

### func enumerate()

```cangjie
public func enumerate(): LinkedList<(Int64, T)>
```

功能：用于获取带索引的链表。

> **注意：**
>
> 不支持平台：OpenHarmony。

返回值：

- [LinkedList](./collection_package_class.md#class-linkedlistt)\<([Int64](../../core/core_package_api/core_package_intrinsics.md#int64), T)> - 返回一个带索引的新 [LinkedList](./collection_package_class.md#class-linkedlistt)。

示例：

<!-- verify -->
```cangjie
import std.collection.*

main() {
    let list = LinkedList<String>(["第一个", "第二个", "第三个", "第四个"])
    println("原始链表: ${list}")

    // 使用enumerate获取带索引的链表
    let enumeratedList = list.enumerate()

    println("带索引的链表:")
    for (item in enumeratedList) {
        // item是一个元组(Int64, String)
        println(" 索引 ${item[0]}: ${item[1]}")
    }
    return 0
}
```

运行结果：

```text
原始链表: [第一个, 第二个, 第三个, 第四个]
带索引的链表:
 索引 0: 第一个
 索引 1: 第二个
 索引 2: 第三个
 索引 3: 第四个
```

### func zip\<R>(LinkedList\<R>)

```cangjie
public func zip<R>(other: LinkedList<R>): LinkedList<(T, R)>
```

功能：将两个 [LinkedList](./collection_package_class.md#class-linkedlistt) 合并成一个新 [LinkedList](./collection_package_class.md#class-linkedlistt)（长度取决于短的那个链表）。

参数：

- other: [LinkedList](./collection_package_class.md#class-linkedlistt)\<R> - 要合并的其中一个链表。

返回值：

- [LinkedList](./collection_package_class.md#class-linkedlistt)\<(T, R)> - 返回一个新链表。

示例：

<!-- verify -->
```cangjie
import std.collection.*

main() {
    let list1 = LinkedList<String>(["第一个", "第二个", "第三个", "第四个"])
    let list2 = LinkedList<Int64>([1, 2, 3])
    println("第一个链表: ${list1}")
    println("第二个链表: ${list2}")

    // 使用zip方法合并两个链表
    let zippedList = list1.zip(list2)

    println("合并后的链表:")
    for (item in zippedList) {
        // item是一个元组(String, Int64)
        println("(${item[0]}, ${item[1]})")
    }
    println("合并后链表大小: ${zippedList.size}")
    return 0
}
```

运行结果：

```text
第一个链表: [第一个, 第二个, 第三个, 第四个]
第二个链表: [1, 2, 3]
合并后的链表:
(第一个, 1)
(第二个, 2)
(第三个, 3)
合并后链表大小: 3
```

<!--DelEnd-->

## extend\<T> LinkedList\<T> <: Equatable\<LinkedList\<T>> where T <: Equatable\<T>

```cangjie
extend<T> LinkedList<T> <: Equatable<LinkedList<T>> where T <: Equatable<T>
```

功能：为 [LinkedList](./collection_package_class.md#class-linkedlistt)\<T> 类型扩展 [Equatable](../../core/core_package_api/core_package_interfaces.md#interface-equatablet)\<[LinkedList](./collection_package_class.md#class-linkedlistt)\<T>> 接口，支持判等操作。

被扩展类型：

- [LinkedList\<T>](./collection_package_class.md#class-linkedlistt)

扩展接口：

- [Equatable](../../core/core_package_api/core_package_interfaces.md#interface-equatablet)\<[LinkedList](collection_package_class.md#class-linkedlistt)\<T>>

### operator func !=(LinkedList\<T>)

```cangjie
public operator func !=(right: LinkedList<T>): Bool
```

功能：判断当前实例与参数指向的 [LinkedList](./collection_package_class.md#class-linkedlistt)\<T> 实例是否不等。

参数：

- right: [LinkedList](./collection_package_class.md#class-linkedlistt)\<T> - 被比较的对象。

返回值：

- [Bool](../../core/core_package_api/core_package_intrinsics.md#bool) - 如果不等，则返回 true，否则返回 false。

示例：

<!-- verify -->
```cangjie
import std.collection.*

main() {
    let list1 = LinkedList<String>()
    let list2 = LinkedList<String>()

    // 测试空链表的不等性
    println("两个空链表不等: ${list1 != list2}")

    // 向list1添加元素
    list1.addLast("第一个")
    list1.addLast("第二个")

    // 测试不同大小的链表
    println("不同大小的链表不等: ${list1 != list2}")

    // 向list2添加相同的元素
    list2.addLast("第一个")
    list2.addLast("第二个")

    println("具有相同元素的链表不等: ${list1 != list2}")

    // 创建一个具有不同元素的链表
    let list3 = LinkedList<String>()
    list3.addLast("第一个")
    list3.addLast("第三个") // 不同的元素

    println("具有不同元素的链表不等: ${list1 != list3}")

    // 创建一个具有相同元素但不同顺序的链表
    let list4 = LinkedList<String>()
    list4.addLast("第二个")
    list4.addLast("第一个")

    println("相同元素但不同顺序的链表不等: ${list1 != list4}")
    return 0
}
```

运行结果：

```text
两个空链表不等: false
不同大小的链表不等: true
具有相同元素的链表不等: false
具有不同元素的链表不等: true
相同元素但不同顺序的链表不等: true
```

### operator func ==(LinkedList\<T>)

```cangjie
public operator func ==(right: LinkedList<T>): Bool
```

功能：判断当前实例与参数指向的 [LinkedList](./collection_package_class.md#class-linkedlistt)\<T> 实例是否相等。

两个 [LinkedList](./collection_package_class.md#class-linkedlistt)\<T> 相等指的是其中包含的元素完全相等。

参数：

- right: [LinkedList](./collection_package_class.md#class-linkedlistt)\<T> - 被比较的对象。

返回值：

- [Bool](../../core/core_package_api/core_package_intrinsics.md#bool) - 如果相等，则返回 true，否则返回 false。

示例：

<!-- verify -->
```cangjie
import std.collection.*

main() {
    let list1 = LinkedList<Int64>()
    let list2 = LinkedList<Int64>()
    let list3 = LinkedList<Int64>()

    // 测试空链表的相等性
    println("两个空链表相等: ${list1 == list2}")

    // 向list1和list2添加相同的元素
    list1.addLast(1)
    list1.addLast(2)
    list1.addLast(3)

    list2.addLast(1)
    list2.addLast(2)
    list2.addLast(3)

    println("具有相同元素的链表相等: ${list1 == list2}")

    // 向list3添加不同的元素
    list3.addLast(1)
    list3.addLast(2)
    list3.addLast(4) // 不同的元素

    println("具有不同元素的链表不相等: ${list1 == list3}")

    // 测试不同大小的链表
    let list4 = LinkedList<Int64>()
    list4.addLast(1)
    list4.addLast(2)

    println("不同大小的链表不相等: ${list1 == list4}")

    // 测试相同元素但不同顺序
    let list5 = LinkedList<Int64>()
    list5.addLast(3)
    list5.addLast(2)
    list5.addLast(1)

    println("相同元素但不同顺序的链表不相等: ${list1 == list5}")
    return 0
}
```

运行结果：

```text
两个空链表相等: true
具有相同元素的链表相等: true
具有不同元素的链表不相等: false
不同大小的链表不相等: false
相同元素但不同顺序的链表不相等: false
```

## extend\<T> LinkedList\<T> <: ToString where T <: ToString

```cangjie
extend<T> LinkedList<T> <: ToString where T <: ToString
```

功能：为 [LinkedList](./collection_package_class.md#class-linkedlistt)\<T> 扩展 [ToString](../../core/core_package_api/core_package_interfaces.md#interface-tostring) 接口，支持转字符串操作。

被扩展类型：

- [LinkedList\<T>](./collection_package_class.md#class-linkedlistt)

扩展接口：

- [ToString](../../core/core_package_api/core_package_interfaces.md#interface-tostring)

### func toString()

```cangjie
public func toString(): String
```

功能：将当前 [LinkedList](./collection_package_class.md#class-linkedlistt)\<T> 实例转换为字符串。

该字符串包含 [LinkedList](./collection_package_class.md#class-linkedlistt)\<T> 内每个元素的字符串表示，形如："[elem1, elem2, elem3]"。

返回值：

- [String](../../core/core_package_api/core_package_structs.md#struct-string) - 转换得到的字符串。

示例：

<!-- verify -->
```cangjie
import std.collection.*

main() {
    // 测试空链表
    let emptyList = LinkedList<Int64>()
    println("空链表的toString: ${emptyList.toString()}")

    // 测试单个元素的链表
    let singleList = LinkedList<String>()
    singleList.addLast("单个元素")
    println("单个元素链表的toString: ${singleList.toString()}")

    // 测试多个元素的链表
    let numberList = LinkedList<Int64>([10, 20, 30, 40])
    println("数字链表的toString: ${numberList.toString()}")

    // 测试修改后的toString
    numberList.addFirst(5)
    numberList.addLast(50)
    println("修改后的数字链表toString: ${numberList.toString()}")

    return 0
}
```

运行结果：

```text
空链表的toString: []
单个元素链表的toString: [单个元素]
数字链表的toString: [10, 20, 30, 40]
修改后的数字链表toString: [5, 10, 20, 30, 40, 50]
```

## extend\<T> TreeSet\<T> <: Equatable\<TreeSet\<T>>

```cangjie
extend<T> TreeSet<T> <: Equatable<TreeSet<T>>
```

功能：为 [TreeSet](collection_package_class.md#class-treesett-where-t--comparablet)\<T> 类型扩展 [Equatable](../../core/core_package_api/core_package_interfaces.md#interface-equatablet)\<[TreeSet](collection_package_class.md#class-treesett-where-t--comparablet)\<T>> 接口，支持判等操作。

被扩展类型：

- [TreeSet\<T>](collection_package_class.md#class-treesett-where-t--comparablet)

扩展接口：

- [Equatable](../../core/core_package_api/core_package_interfaces.md#interface-equatablet)\<[TreeSet](collection_package_class.md#class-treesett-where-t--comparablet)\<T>>

### operator func !=(TreeSet\<T>)

```cangjie
public operator func !=(other: TreeSet<T>): Bool
```

功能：判断当前实例与参数指向的 [TreeSet](collection_package_class.md#class-treesett-where-t--comparablet)\<T> 实例是否不等。

参数：

- other: [TreeSet](collection_package_class.md#class-treesett-where-t--comparablet)\<T> - 被比较的对象。

返回值：

- [Bool](../../core/core_package_api/core_package_intrinsics.md#bool) - 如果不等，则返回 true，否则返回 false。

示例：

<!-- verify -->
```cangjie
import std.collection.*

main() {
    let set1 = TreeSet<String>(["apple", "banana", "cherry"])
    let set2 = TreeSet<String>(["apple", "banana", "cherry"])
    let set3 = TreeSet<String>(["apple", "banana", "date"])

    println("集合1: ${set1}")
    println("集合2: ${set2}")
    println("集合3: ${set3}")

    // 比较不相等的集合
    let result1 = set1 != set3
    println("set1 != set3: ${result1}")

    // 比较相等的集合
    let result2 = set1 != set2
    println("set1 != set2: ${result2}")

    // 比较自身
    let result3 = set1 != set1
    println("set1 != set1: ${result3}")

    return 0
}
```

运行结果：

```text
集合1: [apple, banana, cherry]
集合2: [apple, banana, cherry]
集合3: [apple, banana, date]
set1 != set3: true
set1 != set2: false
set1 != set1: false
```

### operator func ==(TreeSet\<T>)

```cangjie
public operator func ==(other: TreeSet<T>): Bool
```

功能：判断当前实例与参数指向的 [TreeSet](collection_package_class.md#class-treesett-where-t--comparablet)\<T> 实例是否相等。

两个 [TreeSet](collection_package_class.md#class-treesett-where-t--comparablet)\<T> 相等指的是其中包含的元素完全相等。

参数：

- other: [TreeSet](collection_package_class.md#class-treesett-where-t--comparablet)\<T> - 被比较的对象。

返回值：

- [Bool](../../core/core_package_api/core_package_intrinsics.md#bool) - 如果相等，则返回 true，否则返回 false。

示例：

<!-- verify -->
```cangjie
import std.collection.*

main() {
    let set1 = TreeSet<String>(["apple", "banana", "cherry"])
    let set2 = TreeSet<String>(["apple", "banana", "cherry"])
    let set3 = TreeSet<String>(["apple", "banana", "date"])

    println("集合1: ${set1}")
    println("集合2: ${set2}")
    println("集合3: ${set3}")

    // 比较相等的集合
    let result1 = set1 == set2
    println("set1 == set2: ${result1}")

    // 比较不相等的集合
    let result2 = set1 == set3
    println("set1 == set3: ${result2}")

    // 比较自身
    let result3 = set1 == set1
    println("set1 == set1: ${result3}")

    return 0
}
```

运行结果：

```text
集合1: [apple, banana, cherry]
集合2: [apple, banana, cherry]
集合3: [apple, banana, date]
set1 == set2: true
set1 == set3: false
set1 == set1: true
```

## extend\<T> TreeSet\<T> <: ToString where T <: ToString

```cangjie
extend<T> TreeSet<T> <: ToString where T <: ToString
```

功能：为 [TreeSet](collection_package_class.md#class-treesett-where-t--comparablet)\<T> 扩展 [ToString](../../core/core_package_api/core_package_interfaces.md#interface-tostring) 接口，支持转字符串操作。

被扩展类型：

- [TreeSet\<T>](collection_package_class.md#class-treesett-where-t--comparablet)

扩展接口：

- [ToString](../../core/core_package_api/core_package_interfaces.md#interface-tostring)

### func toString()

```cangjie
public func toString(): String
```

功能：将当前 [TreeSet](collection_package_class.md#class-treesett-where-t--comparablet)\<T> 实例转换为字符串。

该字符串包含 [TreeSet](collection_package_class.md#class-treesett-where-t--comparablet)\<T> 内每个元素的字符串表示，形如："[elem1, elem2, elem3]"。

返回值：

- [String](../../core/core_package_api/core_package_structs.md#struct-string) - 转换得到的字符串。

示例：

<!-- verify -->
```cangjie
import std.collection.*

main() {
    let set = TreeSet<String>(["apple", "banana", "cherry"])

    // 自动调用 toString() 方法
    println("TreeSet内容: ${set}")

    // 转换为字符串
    let str = set.toString()

    println("转换后的字符串: ${str}")
    return 0
}
```

运行结果：

```text
TreeSet内容: [apple, banana, cherry]
转换后的字符串: [apple, banana, cherry]
```
