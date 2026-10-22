# 接口

## interface SortByExtension\<T> <sup>(deprecated)</sup>

```cangjie
public interface SortByExtension<T>
```

功能：此接口作为排序相关的辅助接口，通过传入的比较函数，根据其返回值 [Ordering](../../core/core_package_api/core_package_enums.md#enum-ordering) 类型的结果，可对 T 进行自定义排序。

> **注意：**
>
> 未来版本即将废弃。

扩展：

- [extend\<T> Array\<T> <: SortByExtension\<T>](sort_package_extends.md#extendt-arrayt--sortbyextensiont)

### func sortBy((T, T) -> Ordering) <sup>(deprecated)</sup>

```cangjie
func sortBy(comparator!: (T, T) -> Ordering): Unit
```

功能：通过传入的比较函数，根据其返回值 [Ordering](../../core/core_package_api/core_package_enums.md#enum-ordering) 类型的结果，可对 T 进行自定义排序。

> **注意：**
>
> 未来版本即将废弃。

参数：

- comparator!: (T, T) ->[Ordering](../../core/core_package_api/core_package_enums.md#enum-ordering) - 用户传入的比较函数。

### func sortBy(Bool, (T, T) -> Ordering) <sup>(deprecated)</sup>

```cangjie
func sortBy(stable!: Bool, comparator!: (T, T) -> Ordering): Unit
```

功能：通过传入的比较函数，根据其返回值 [Ordering](../../core/core_package_api/core_package_enums.md#enum-ordering) 类型的结果和稳定排序标志位，可对 T 进行自定义排序。

> **注意：**
>
> 未来版本即将废弃。

参数：

- stable!: [Bool](../../core/core_package_api/core_package_intrinsics.md#bool) - 是否使用稳定排序。
- comparator!: (T, T) ->[Ordering](../../core/core_package_api/core_package_enums.md#enum-ordering) - 用户传入的比较函数。

> **注意：**
>
> 未来版本即将废弃。

## interface SortExtension <sup>(deprecated)</sup>

```cangjie
public interface SortExtension
```

功能：此接口作为排序相关的辅助接口。

扩展：

- [extend\<T> ArrayList\<T> <: SortExtension where T <: Comparable\<T>](../../collection/collection_package_api/collection_package_extends.md#extendt-arraylistt--sortextension-where-t--comparablet)

- [extend\<T> Array\<T> <: SortExtension where T <: Comparable\<T>](sort_package_extends.md#extendt-arrayt--sortextension-where-t--comparablet)

### func sort() <sup>(deprecated)</sup>

```cangjie
func sort(): Unit
```

功能：实现对应类型的排序。

> **注意：**
>
> 未来版本即将废弃。

### func sort(Bool) <sup>(deprecated)</sup>

```cangjie
func sort(stable!: Bool): Unit
```

功能：依据传入的参数，实现对应类型的稳定或非稳定排序。

> **注意：**
>
> 未来版本即将废弃。

参数：

- stable!: [Bool](../../core/core_package_api/core_package_intrinsics.md#bool) - 是否使用稳定排序。

### func sortDescending() <sup>(deprecated)</sup>

```cangjie
func sortDescending(): Unit
```

功能：实现对应类型的降序方式排序。

> **注意：**
>
> 未来版本即将废弃。

### func sortDescending(Bool) <sup>(deprecated)</sup>

```cangjie
func sortDescending(stable!: Bool): Unit
```

功能：依据传入的参数，实现对应类型的稳定或非稳定降序排序。

> **注意：**
>
> 未来版本即将废弃。

参数：

- stable!: [Bool](../../core/core_package_api/core_package_intrinsics.md#bool) - 是否使用稳定排序。

