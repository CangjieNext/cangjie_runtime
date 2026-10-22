# 接口

## interface Any

```cangjie
public interface Any
```

功能：[Any](core_package_interfaces.md#interface-any) 是所有类型的父类型，所有 `interface` 都默认继承它，所有非 `interface` 类型都默认实现它。

## interface Collection\<T>

```cangjie
public interface Collection<T> <: Iterable<T>
```

功能：该接口用来表示集合，通常容器类型应该实现该接口。

父类型：

- [Iterable](#interface-iterablee)\<T>

扩展：

- [extend\<T> Array\<T> <: Collection\<T>](core_package_extends.md#extendt-arrayt--collectiont)

### prop size

```cangjie
prop size: Int64
```

功能：获取当前集合的大小，即集合中元素的个数。

类型：[Int64](core_package_intrinsics.md#int64)

### func isEmpty()

```cangjie
func isEmpty(): Bool
```

功能：判断当前集合是否为空。

返回值：

- [Bool](core_package_intrinsics.md#bool) - 如果为空返回 true，否则返回 false。

### func toArray()

```cangjie
func toArray(): Array<T>
```

功能：将当前集合转为数组类型。

返回值：

- [Array](core_package_structs.md#struct-arrayt)\<T> - 转换得到的数组。

## interface Comparable\<T>

```cangjie
public interface Comparable<T> <: Equatable<T> & Less<T> & Greater<T> & LessOrEqual<T> & GreaterOrEqual<T>
```

功能：该接口表示比较运算，是等于、不等于、小于、大于、小于等于、大于等于接口的集合体。

该接口中提供运算符 ==、!=、<、<=、>、>= 重载的默认实现，默认实现根据 compare 函数的返回值来确定其返回值。例如：如果 a.compare(b) 的返回值为 EQ，则 a == b 返回 true，否则返回 false。

父类型：

- [Equatable](#interface-equatablet)\<T>
- [Less](#interface-lesst)\<T>
- [Greater](#interface-greatert)\<T>
- [LessOrEqual](#interface-lessorequalt)\<T>
- [GreaterOrEqual](#interface-greaterorequalt)\<T>

扩展：

- [extend Float16 <: Comparable\<Float16>](core_package_extends.md#extend-float16--comparablefloat16)
- [extend Float32 <: Comparable\<Float32>](core_package_extends.md#extend-float32--comparablefloat32)
- [extend Float64 <: Comparable\<Float64>](core_package_extends.md#extend-float64--comparablefloat64)
- [extend Int16 <: Comparable\<Int16>](core_package_extends.md#extend-int16--comparableint16)
- [extend Int32 <: Comparable\<Int32>](core_package_extends.md#extend-int32--comparableint32)
- [extend Int64 <: Comparable\<Int64>](core_package_extends.md#extend-int64--comparableint64)
- [extend Int8 <: Comparable\<Int8>](core_package_extends.md#extend-int8--comparableint8)
- [extend IntNative <: Comparable\<IntNative>](core_package_extends.md#extend-intnative--comparableintnative)
- [extend Ordering <: Comparable](core_package_extends.md#extend-ordering--comparable)
- [extend Rune <: Comparable\<Rune>](core_package_extends.md#extend-rune--comparablerune)
- [extend UInt16 <: Comparable\<UInt16>](core_package_extends.md#extend-uint16--comparableuint16)
- [extend UInt32 <: Comparable\<UInt32>](core_package_extends.md#extend-uint32--comparableuint32)
- [extend UInt64 <: Comparable\<UInt64>](core_package_extends.md#extend-uint64--comparableuint64)
- [extend UInt8 <: Comparable\<UInt8>](core_package_extends.md#extend-uint8--comparableuint8)
- [extend UIntNative <: Comparable\<UIntNative>](core_package_extends.md#extend-uintnative--comparableuintnative)
- [extend\<T> Box\<T> <: Comparable\<Box\<T>> where T <: Comparable\<T>](core_package_extends.md#extendt-boxt--comparableboxt-where-t--comparablet)

### func compare(T)

```cangjie
func compare(other: T): Ordering
```

功能：判断当前 `T` 类型实例与参数指向的 `T` 类型实例的大小关系。

参数：

- other: T - 待与当前实例比较的另一个实例。

返回值：

- [Ordering](core_package_enums.md#enum-ordering) - 如果大于，返回 [Ordering](core_package_enums.md#enum-ordering).GT，如果等于，返回 [Ordering](core_package_enums.md#enum-ordering).EQ，如果小于，返回 [Ordering](core_package_enums.md#enum-ordering).LT。

### operator func <(T)

```cangjie
operator func <(other: T): Bool
```

功能：判断当前 `T` 类型实例是否小于参数指向的 `T` 类型实例，该函数是此接口的一个默认实现函数。

参数：

- other: T - 待与当前实例比较的另一个实例。

返回值：

- [Bool](core_package_intrinsics.md#bool) - 如果小于，返回 true，否则返回 false。

### operator func <=(T)

```cangjie
operator func <=(other: T): Bool
```

功能：判断当前 `T` 类型实例是否小于等于参数指向的 `T` 类型实例，该函数是此接口的一个默认实现函数。

参数：

- other: T - 待与当前实例比较的另一个实例。

返回值：

- [Bool](core_package_intrinsics.md#bool) - 如果小于等于，返回 true，否则返回 false。

### operator func ==(T)

```cangjie
operator func ==(other: T): Bool
```

功能：判断两个实例是否相等，该函数是此接口的一个默认实现函数。

参数：

- other: T - 待比较的另一个实例。

返回值：

- [Bool](core_package_intrinsics.md#bool) - 如果相等，返回 true，否则返回 false。

### operator func >(T)

```cangjie
operator func >(other: T): Bool
```

功能：判断当前 `T` 类型实例是否大于参数指向的 `T` 类型实例，该函数是此接口的一个默认实现函数。

参数：

- other: T - 待与当前实例比较的另一个实例。

返回值：

- [Bool](core_package_intrinsics.md#bool) - 如果大于，返回 true，否则返回 false。

### operator func >=(T)

```cangjie
operator func >=(other: T): Bool
```

功能：判断当前 `T` 类型实例是否大于等于参数指向的 `T` 类型实例，该函数是此接口的一个默认实现函数。

参数：

- other: T - 待与当前实例比较的另一个实例。

返回值：

- [Bool](core_package_intrinsics.md#bool) - 如果大于等于，返回 true，否则返回 false。

## interface Countable\<T>

```cangjie
public interface Countable<T>
```

功能：该接口表示类型可数。

可数类型的每一个实例都对应一个位置信息（[Int64](core_package_intrinsics.md#int64) 值），可以通过往后数数得到其他的该类型实例。

扩展：

- [extend Int16 <: Countable\<Int16>](core_package_extends.md#extend-int16--countableint16)
- [extend Int32 <: Countable\<Int32>](core_package_extends.md#extend-int32--countableint32)
- [extend Int64 <: Countable\<Int64>](core_package_extends.md#extend-int64--countableint64)
- [extend Int8 <: Countable\<Int8>](core_package_extends.md#extend-int8--countableint8)
- [extend IntNative <: Countable\<IntNative>](core_package_extends.md#extend-intnative--countableintnative)
- [extend Rune <: Countable\<Rune>](core_package_extends.md#extend-rune--countablerune)
- [extend UInt16 <: Countable\<UInt16>](core_package_extends.md#extend-uint16--countableuint16)
- [extend UInt32 <: Countable\<UInt32>](core_package_extends.md#extend-uint32--countableuint32)
- [extend UInt64 <: Countable\<UInt64>](core_package_extends.md#extend-uint64--countableuint64)
- [extend UInt8 <: Countable\<UInt8>](core_package_extends.md#extend-uint8--countableuint8)
- [extend UIntNative <: Countable\<UIntNative>](core_package_extends.md#extend-uintnative--countableuintnative)

### func next(Int64)

```cangjie
func next(right: Int64): T
```

功能：获取当前实例向右移动 `right` 后对应位置的 `T` 类型实例。

参数：

- right: [Int64](core_package_intrinsics.md#int64) - 往右数的个数。

返回值：

- T - 向右移动 `right` 后对应位置的 `T` 类型实例。

### func position()

```cangjie
func position(): Int64
```

功能：获取当前可数实例的位置信息，即将当前实例转为 [Int64](core_package_intrinsics.md#int64) 类型。

返回值：

- [Int64](core_package_intrinsics.md#int64) - 转换后的 [Int64](core_package_intrinsics.md#int64) 值。

## interface CType

```cangjie
sealed interface CType
```

功能：表示支持与 C 语言互操作的接口。

[CType](core_package_interfaces.md#interface-ctype) 接口是一个语言内置的空接口，它是 [CType](core_package_interfaces.md#interface-ctype) 约束的具体实现，所有 C 互操作支持的类型都隐式地实现了该接口，因此所有 C 互操作支持的类型都可以作为 [CType](core_package_interfaces.md#interface-ctype) 类型的子类型使用。

> **注意：**
>
> - [CType](core_package_interfaces.md#interface-ctype) 接口是仓颉中的一个接口类型，它本身不满足 [CType](core_package_interfaces.md#interface-ctype) 约束。
> - [CType](core_package_interfaces.md#interface-ctype) 接口不允许被用户继承、扩展。
> - [CType](core_package_interfaces.md#interface-ctype) 接口不会突破子类型的使用限制。

示例：

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

## interface Equal\<T>

```cangjie
public interface Equal<T>
```

功能：该接口用于支持判等操作。

### operator func ==(T)

```cangjie
operator func ==(other: T): Bool
```

功能：判断两个实例是否相等。

参数：

- other: T - 待比较的另一个实例。

返回值：

- [Bool](core_package_intrinsics.md#bool) - 如果相等，返回 true，否则返回 false。

## interface Equatable\<T>

```cangjie
public interface Equatable<T> <: Equal<T> & NotEqual<T>
```

功能：该接口是判等和判不等两个接口的集合体。

该接口中提供运算符 != 重载的默认实现，默认实现根据 == 运算的返回值来确定其返回值。例如：如果 a == b 的返回值为 true，则 a != b 返回 false，否则返回 true。

已为部分仓颉类型实现该接口，包括：[Unit](core_package_intrinsics.md#unit)、[Bool](core_package_intrinsics.md#bool) 、[Rune](core_package_intrinsics.md#rune)、[Int64](core_package_intrinsics.md#int64)、[Int32](core_package_intrinsics.md#int32)、[Int16](core_package_intrinsics.md#int16)、[Int8](core_package_intrinsics.md#int8)、[UIntNative](core_package_intrinsics.md#uintnative)、[UInt64](core_package_intrinsics.md#uint64)、[UInt32](core_package_intrinsics.md#uint32)、[UInt16](core_package_intrinsics.md#uint16)、[UInt8](core_package_intrinsics.md#uint8)、[Float64](core_package_intrinsics.md#float64)、[Float32](core_package_intrinsics.md#float32)、[Float16](core_package_intrinsics.md#float16)、[String](core_package_structs.md#struct-string)、[Array](core_package_structs.md#struct-arrayt)、[Box](core_package_classes.md#class-boxt)、[ArrayList](../../collection/collection_package_api/collection_package_class.md#class-arraylistt)、[HashSet](../../collection/collection_package_api/collection_package_class.md#class-hashsett-where-t--hashable--equatablet)。

父类型：

- [Equal](#interface-equalt)\<T>
- [NotEqual](#interface-notequalt)\<T>

扩展：

- [extend\<K, V> HashMap\<K, V> <: Equatable\<HashMap\<K, V>> where V <: Equatable\<V>](../../collection/collection_package_api/collection_package_extends.md#extendk-v-hashmapk-v--equatablehashmapk-v-where-v--equatablev)
- [extend\<K, V> TreeMap\<K, V> <: Equatable\<TreeMap\<K, V>> where V <: Equatable\<V>](../../collection/collection_package_api/collection_package_extends.md#extendk-v-treemapk-v--equatabletreemapk-v-where-v--equatablev)
- [extend\<T> ArrayList\<T> <: Equatable\<ArrayList\<T>> where T <: Equatable\<T>](../../collection/collection_package_api/collection_package_extends.md#extendt-arraylistt--equatablearraylistt-where-t--equatablet)
- [extend\<T> HashSet\<T> <: Equatable\<HashSet\<T>>](../../collection/collection_package_api/collection_package_extends.md#extendt-hashsett--equatablehashsett)
- [extend\<T> LinkedList\<T> <: Equatable\<LinkedList\<T>> where T <: Equatable\<T>](../../collection/collection_package_api/collection_package_extends.md#extendt-linkedlistt--equatablelinkedlistt-where-t--equatablet)
- [extend\<T> TreeSet\<T> <: Equatable\<TreeSet\<T>>](../../collection/collection_package_api/collection_package_extends.md#extendt-treesett--equatabletreesett)
- [extend Bool <: Equatable\<Bool>](core_package_extends.md#extend-bool--equatablebool)
- [extend Unit <: Equatable\<Unit>](core_package_extends.md#extend-unit--equatableunit)
- [extend\<T> Array\<T> <: Equatable\<Array\<T>> where T <: Equatable\<T>](core_package_extends.md#extendt-arrayt--equatablearrayt-where-t--equatablet)
- [extend\<T> Option\<T> <: Equatable\<Option\<T>> where T <: Equatable\<T>](core_package_extends.md#extendt-optiont--equatableoptiont-where-t--equatablet)
- [extend\<T> Range\<T> <: Equatable\<Range\<T>> where T <: Countable\<T> & Comparable\<T> & Equatable\<T>](core_package_extends.md#extendt-ranget--equatableranget-where-t--countablet--comparablet--equatablet)

### operator func !=(T)

```cangjie
operator func !=(other: T): Bool
```

功能：判断两个实例是否不相等，该函数是此接口的一个默认实现函数。

参数：

- other: T - 待比较的另一个实例。

返回值：

- [Bool](core_package_intrinsics.md#bool) - 如果不相等，返回 true，否则返回 false。

## interface Greater\<T>

```cangjie
public interface Greater<T>
```

功能：该接口表示大于计算。

### operator func >(T)

```cangjie
operator func >(other: T): Bool
```

功能：判断当前 `T` 类型实例是否大于参数指向的 `T` 类型实例。

参数：

- other: T - 待与当前实例比较的另一个实例。

返回值：

- [Bool](core_package_intrinsics.md#bool) - 如果大于，返回 true，否则返回 false。

## interface GreaterOrEqual\<T>

```cangjie
public interface GreaterOrEqual<T>
```

功能：该接口表示大于等于计算。

### operator func >=(T)

```cangjie
operator func >=(other: T): Bool
```

功能：判断当前 `T` 类型实例是否大于等于参数指向的 `T` 类型实例。

参数：

- other: T - 待与当前实例比较的另一个实例。

返回值：

- [Bool](core_package_intrinsics.md#bool) - 如果大于等于，返回 true，否则返回 false。

## interface Hashable

```cangjie
public interface Hashable
```

功能：该接口用于计算哈希值。

已为部分仓颉类型实现该接口，包括：[Bool](core_package_intrinsics.md#bool)、[Rune](core_package_intrinsics.md#rune)、[IntNative](core_package_intrinsics.md#intnative)、[Int64](core_package_intrinsics.md#int64)、[Int32](core_package_intrinsics.md#int32)、[Int16](core_package_intrinsics.md#int16)、[Int8](core_package_intrinsics.md#int8)、[UIntNative](core_package_intrinsics.md#uintnative)、[UInt64](core_package_intrinsics.md#uint64)、[UInt32](core_package_intrinsics.md#uint32)、[UInt16](core_package_intrinsics.md#uint16)、[UInt8](core_package_intrinsics.md#uint8)、[Float64](core_package_intrinsics.md#float64)、[Float32](core_package_intrinsics.md#float32)、[Float16](core_package_intrinsics.md#float16)、[String](core_package_structs.md#struct-string)、[Box](core_package_classes.md#class-boxt)。

扩展：

- [extend Bool <: Hashable](core_package_extends.md#extend-bool--hashable)
- [extend Float16 <: Hashable](core_package_extends.md#extend-float16--hashable)
- [extend Float32 <: Hashable](core_package_extends.md#extend-float32--hashable)
- [extend Float64 <: Hashable](core_package_extends.md#extend-float64--hashable)
- [extend Int16 <: Hashable](core_package_extends.md#extend-int16--hashable)
- [extend Int32 <: Hashable](core_package_extends.md#extend-int32--hashable)
- [extend Int64 <: Hashable](core_package_extends.md#extend-int64--hashable)
- [extend Int8 <: Hashable](core_package_extends.md#extend-int8--hashable)
- [extend IntNative <: Hashable](core_package_extends.md#extend-intnative--hashable)
- [extend Ordering <: Hashable](core_package_extends.md#extend-ordering--hashable)
- [extend Rune <: Hashable](core_package_extends.md#extend-rune--hashable)
- [extend UInt16 <: Hashable](core_package_extends.md#extend-uint16--hashable)
- [extend UInt32 <: Hashable](core_package_extends.md#extend-uint32--hashable)
- [extend UInt64 <: Hashable](core_package_extends.md#extend-uint64--hashable)
- [extend UInt8 <: Hashable](core_package_extends.md#extend-uint8--hashable)
- [extend UIntNative <: Hashable](core_package_extends.md#extend-uintnative--hashable)
- [extend Unit <: Hashable](core_package_extends.md#extend-unit--hashable)
- [extend\<T> Box\<T> <: Hashable where T <: Hashable](core_package_extends.md#extendt-boxt--hashable-where-t--hashable)
- [extend\<T> Option\<T> <: Hashable where T <: Hashable](core_package_extends.md#extendt-optiont--hashable-where-t--hashable)
- [extend\<T> Range\<T> <: Hashable where T <: Hashable & Countable\<T> & Comparable\<T> & Equatable\<T>](core_package_extends.md#extendt-ranget--hashable-where-t--hashable--countablet--comparablet--equatablet)

### func hashCode()

```cangjie
func hashCode(): Int64
```

功能：获得实例类型的哈希值。

返回值：

- [Int64](core_package_intrinsics.md#int64) - 返回实例类型的哈希值。

## interface Hasher

```cangjie
public interface Hasher
```

功能：该接口用于处理哈希组合运算。

可以使用一系列 write 函数传入不同数据类型实例，并计算它们的组合哈希值。

### func finish()

```cangjie
func finish(): Int64
```

功能：获取哈希运算的结果。

返回值：

- [Int64](core_package_intrinsics.md#int64) - 经过计算后的哈希值。

### func reset()

```cangjie
mut func reset(): Unit
```

功能：重置哈希值到 0。

### func write(Bool)

```cangjie
mut func write(value: Bool): Unit
```

功能：把想要哈希运算的 [Bool](core_package_intrinsics.md#bool) 值传入，然后进行哈希组合运算。

参数：

- value: [Bool](core_package_intrinsics.md#bool) - 待运算的值。

### func write(Float16)

```cangjie
mut func write(value: Float16): Unit
```

功能：通过该函数把想要哈希运算的 [Float16](core_package_intrinsics.md#float16) 值传入，然后进行哈希组合运算。

参数：

- value: [Float16](core_package_intrinsics.md#float16) - 待运算的值。

### func write(Float32)

```cangjie
mut func write(value: Float32): Unit
```

功能：通过该函数把想要哈希运算的 [Float32](core_package_intrinsics.md#float32) 值传入，然后进行哈希组合运算。

参数：

- value: [Float32](core_package_intrinsics.md#float32) - 待运算的值。

### func write(Float64)

```cangjie
mut func write(value: Float64): Unit
```

功能：通过该函数把想要哈希运算的 [Float64](core_package_intrinsics.md#float64) 值传入，然后进行哈希组合运算。

参数：

- value: [Float64](core_package_intrinsics.md#float64) - 待运算的值。

### func write(Int16)

```cangjie
mut func write(value: Int16): Unit
```

功能：通过该函数把想要哈希运算的 [Int16](core_package_intrinsics.md#int16) 值传入，然后进行哈希组合运算。

参数：

- value: [Int16](core_package_intrinsics.md#int16) - 待运算的值。

### func write(Int32)

```cangjie
mut func write(value: Int32): Unit
```

功能：通过该函数把想要哈希运算的 [Int32](core_package_intrinsics.md#int32) 值传入，然后进行哈希组合运算。

参数：

- value: [Int32](core_package_intrinsics.md#int32) - 待运算的值。

### func write(Int64)

```cangjie
mut func write(value: Int64): Unit
```

功能：通过该函数把想要哈希运算的 [Int64](core_package_intrinsics.md#int64) 值传入，然后进行哈希组合运算。

参数：

- value: [Int64](core_package_intrinsics.md#int64) - 待运算的值。

### func write(Int8)

```cangjie
mut func write(value: Int8): Unit
```

功能：通过该函数把想要哈希运算的 [Int8](core_package_intrinsics.md#int8) 值传入，然后进行哈希组合运算。

参数：

- value: [Int8](core_package_intrinsics.md#int8) - 待运算的值。

### func write(Rune)

```cangjie
mut func write(value: Rune): Unit
```

功能：通过该函数把想要哈希运算的 [Rune](core_package_intrinsics.md#rune) 值传入，然后进行哈希组合运算。

参数：

- value: [Rune](core_package_intrinsics.md#rune) - 待运算的值。

### func write(String)

```cangjie
mut func write(value: String): Unit
```

功能：通过该函数把想要哈希运算的 [String](core_package_structs.md#struct-string) 值传入，然后进行哈希组合运算。

参数：

- value: [String](core_package_structs.md#struct-string) - 待运算的值。

### func write(UInt16)

```cangjie
mut func write(value: UInt16): Unit
```

功能：通过该函数把想要哈希运算的 [UInt16](core_package_intrinsics.md#uint16) 值传入，然后进行哈希组合运算。

参数：

- value: [UInt16](core_package_intrinsics.md#uint16) - 待运算的值。

### func write(UInt32)

```cangjie
mut func write(value: UInt32): Unit
```

功能：通过该函数把想要哈希运算的 [UInt32](core_package_intrinsics.md#uint32) 值传入，然后进行哈希组合运算。

参数：

- value: [UInt32](core_package_intrinsics.md#uint32) - 待运算的值。

### func write(UInt64)

```cangjie
mut func write(value: UInt64): Unit
```

功能：通过该函数把想要哈希运算的 [UInt64](core_package_intrinsics.md#uint64) 值传入，然后进行哈希组合运算。

参数：

- value: [UInt64](core_package_intrinsics.md#uint64) - 待运算的值。

### func write(UInt8)

```cangjie
mut func write(value: UInt8): Unit
```

功能：通过该函数把想要哈希运算的 [UInt8](core_package_intrinsics.md#uint8) 值传入，然后进行哈希组合运算。

参数：

- value: [UInt8](core_package_intrinsics.md#uint8) - 待运算的值。

## interface Iterable\<E>

```cangjie
public interface Iterable<E>
```

功能：该接口表示可迭代，实现了该接口的类型（通常为容器类型）可以在 `for-in` 语句中实现迭代，也可以获取其对应的迭代器类型实例，调用 `next` 函数实现迭代。

本包已经为 [Array](core_package_structs.md#struct-arrayt)、[ArrayList](../../collection/collection_package_api/collection_package_class.md#class-arraylistt)、[HashMap](../../collection/collection_package_api/collection_package_class.md#class-hashmapk-v-where-k--hashable--equatablek) 等基础容器类型实现了该接口，用户可以为其他类型实现该接口，使之支持迭代遍历的功能。

### func iterator()

```cangjie
func iterator(): Iterator<E>
```

功能：获取迭代器。

返回值：

- [Iterator](core_package_classes.md#class-iteratort)\<E> - 迭代器。

## interface Less\<T>

```cangjie
public interface Less<T>
```

功能：该接口表示小于计算。

### operator func <(T)

```cangjie
operator func <(other: T): Bool
```

功能：判断当前 `T` 类型实例是否小于参数指向的 `T` 类型实例。

参数：

- other: T - 待与当前实例比较的另一个实例。

返回值：

- [Bool](core_package_intrinsics.md#bool) - 如果小于，返回 true，否则返回 false。

## interface LessOrEqual\<T>

```cangjie
public interface LessOrEqual<T>
```

功能：该接口表示小于等于计算。

### operator func <=(T)

```cangjie
operator func <=(other: T): Bool
```

功能：判断当前 `T` 类型实例是否小于等于参数指向的 `T` 类型实例。

参数：

- other: T - 待与当前实例比较的另一个实例。

返回值：

- [Bool](core_package_intrinsics.md#bool) - 如果小于等于，返回 true，否则返回 false。

## interface NotEqual\<T>

```cangjie
public interface NotEqual<T>
```

功能：该接口用于支持判不等操作。

### operator func !=(T)

```cangjie
operator func !=(other: T): Bool
```

功能：判断两个实例是否不相等。

参数：

- other: T - 待比较的另一个实例。

返回值：

- [Bool](core_package_intrinsics.md#bool) - 如果不相等，返回 true，否则返回 false。

## interface Resource

```cangjie
public interface Resource
```

功能：该接口用于资源管理，通常用于内存、句柄等资源的关闭和释放。

实现了该接口的类型可以在 `try-with-resource` 语法上下文中实现自动资源释放。

扩展：

- [extend\<T> BufferedInputStream\<T> <: Resource where T <: Resource](../../io/io_package_api/io_package_extends.md#extendt-bufferedinputstreamt--resource-where-t--resource)
- [extend\<T> BufferedOutputStream\<T> <: Resource where T <: Resource](../../io/io_package_api/io_package_extends.md#extendt-bufferedoutputstreamt--resource-where-t--resource)
- [extend\<T> StringReader\<T> <: Resource where T <: Resource](../../io/io_package_api/io_package_extends.md#extendt-stringreadert--resource-where-t--resource)
- [extend\<T> StringWriter\<T> <: Resource where T <: Resource](../../io/io_package_api/io_package_extends.md#extendt-stringwritert--resource-where-t--resource)

### func close()

```cangjie
func close(): Unit
```

功能：关闭资源。

### func isClosed()

```cangjie
func isClosed(): Bool
```

功能：判断资源是否已经关闭。

返回值：

- [Bool](core_package_intrinsics.md#bool) - 如果已经关闭返回 true，否则返回 false。

## interface ThreadContext

```cangjie
public interface ThreadContext
```

功能：仓颉线程上下文接口。

用户创建 `thread` 时，除了缺省 `spawn` 表达式入参，也可以通过传入不同 [ThreadContext](core_package_interfaces.md#interface-threadcontext) 类型的实例，选择不同的线程上下文，然后一定程度上控制并发行为。

目前不允许用户自行实现 [ThreadContext](core_package_interfaces.md#interface-threadcontext) 接口，仓颉语言根据使用场景，提供了 `MainThreadContext`, 具体定义可在终端框架库中查阅。

### func end()

```cangjie
func end(): Unit
```

功能：结束方法，用于向当前 context 发送结束请求。

### func hasEnded()

```cangjie
func hasEnded(): Bool
```

功能：检查方法，用于检查当前 context 是否已结束。

返回值：

- [Bool](core_package_intrinsics.md#bool) - 如果结束返回 true，否则返回 false。

## interface ToString

```cangjie
public interface ToString
```

功能：该接口用来提供具体类型的字符串表示。

扩展：

- [extend\<K, V> HashMap\<K, V> <: ToString where V <: ToString, K <: ToString](../../collection/collection_package_api/collection_package_extends.md#extendk-v-hashmapk-v--tostring-where-v--tostring-k--tostring)
- [extend\<K, V> TreeMap\<K, V> <: ToString where V <: ToString, K <: ToString & Comparable\<K>](../../collection/collection_package_api/collection_package_extends.md#extendk-v-treemapk-v--tostring-where-v--tostring-k--tostring--comparablek)
- [extend\<T> ArrayDeque\<T> <: ToString where T <: ToString](../../collection/collection_package_api/collection_package_extends.md#extendt-arraydequet--tostring-where-t--tostring)
- [extend\<T> ArrayList\<T> <: ToString where T <: ToString](../../collection/collection_package_api/collection_package_extends.md#extendt-arraylistt--tostring-where-t--tostring)
- [extend\<T> ArrayQueue\<T> <: ToString where T <: ToString](../../collection/collection_package_api/collection_package_extends.md#extendt-arrayqueuet--tostring-where-t--tostring)
- [extend\<T> ArrayStack\<T> <: ToString where T <: ToString](../../collection/collection_package_api/collection_package_extends.md#extendt-arraystackt--tostring-where-t--tostring)
- [extend\<T> HashSet\<T> <: ToString where T <: ToString](../../collection/collection_package_api/collection_package_extends.md#extendt-hashsett--tostring-where-t--tostring)
- [extend\<T> LinkedList\<T> <: ToString where T <: ToString](../../collection/collection_package_api/collection_package_extends.md#extendt-linkedlistt--tostring-where-t--tostring)
- [extend\<T> TreeSet\<T> <: ToString where T <: ToString](../../collection/collection_package_api/collection_package_extends.md#extendt-treesett--tostring-where-t--tostring)
- [extend Bool <: ToString](core_package_extends.md#extend-bool--tostring)
- [extend CString <: ToString](core_package_extends.md#extend-cstring--tostring)
- [extend Float16 <: ToString](core_package_extends.md#extend-float16--tostring)
- [extend Float32 <: ToString](core_package_extends.md#extend-float32--tostring)
- [extend Float64 <: ToString](core_package_extends.md#extend-float64--tostring)
- [extend Int16 <: ToString](core_package_extends.md#extend-int16--tostring)
- [extend Int32 <: ToString](core_package_extends.md#extend-int32--tostring)
- [extend Int64 <: ToString](core_package_extends.md#extend-int64--tostring)
- [extend Int8 <: ToString](core_package_extends.md#extend-int8--tostring)
- [extend IntNative <: ToString](core_package_extends.md#extend-intnative--tostring)
- [extend Ordering <: ToString](core_package_extends.md#extend-ordering--tostring)
- [extend Rune <: ToString](core_package_extends.md#extend-rune--tostring)
- [extend UInt16 <: ToString](core_package_extends.md#extend-uint16--tostring)
- [extend UInt32 <: ToString](core_package_extends.md#extend-uint32--tostring)
- [extend UInt64 <: ToString](core_package_extends.md#extend-uint64--tostring)
- [extend UInt8 <: ToString](core_package_extends.md#extend-uint8--tostring)
- [extend UIntNative <: ToString](core_package_extends.md#extend-uintnative--tostring)
- [extend Unit <: ToString](core_package_extends.md#extend-unit--tostring)
- [extend\<T> Array\<T> where T <: ToString](core_package_extends.md#extendt-arrayt-where-t--tostring)
- [extend\<T> Box\<T> <: ToString where T <: ToString](core_package_extends.md#extendt-boxt--tostring-where-t--tostring)
- [extend\<T> Option\<T> <: ToString where T <: ToString](core_package_extends.md#extendt-optiont--tostring-where-t--tostring)

### func toString()

```cangjie
func toString(): String
```

功能：获取实例类型的字符串表示。

返回值：

- [String](core_package_structs.md#struct-string) - 返回实例类型的字符串表示。
