# Extensions

## extend\<T> Array\<T> <: SortByExtension\<T>

```cangjie
extend<T> Array<T> <: SortByExtension<T>
```

Function: This extension implements the `sortBy` function for [Array](../../core/core_package_api/core_package_structs.md#struct-arrayt).

Extended type:

- [Array\<T>](../../core/core_package_api/core_package_structs.md#struct-arrayt)

Extended interfaces:

- [SortByExtension\<T>](sort_package_interfaces.md#interface-sortbyextensiont-deprecated)

### func sortBy((T, T) -> Ordering) <sup>(deprecated)</sup>

```cangjie
public func sortBy(comparator!: (T, T) -> Ordering): Unit
```

Function: Enables custom sorting of an array by passing a comparison function that returns an [Ordering](../../core/core_package_api/core_package_enums.md#enum-ordering) result.

> **Note:**
>
> Will be deprecated in future versions. Use [sort](sort_package_funcs.md#func-sorttarrayt-t-t---ordering-bool-bool) instead.

Parameters:

- comparator!: (T, T) ->[Ordering](../../core/core_package_api/core_package_enums.md#enum-ordering) - User-provided comparison function. For example, comparator: (t1: T, t2: T) -> [Ordering](../../core/core_package_api/core_package_enums.md#enum-ordering). If the return value is [Ordering](../../core/core_package_api/core_package_enums.md#enum-ordering).GT, `t1` will be placed after `t2` after sorting; if the return value is [Ordering](../../core/core_package_api/core_package_enums.md#enum-ordering).LT, `t1` will be placed before `t2`; if the return value is [Ordering](../../core/core_package_api/core_package_enums.md#enum-ordering).EQ and stable sorting is used, the relative positions of `t1` and `t2` remain unchanged; if unstable sorting is used, the order of `t1` and `t2` becomes indeterminate.

### func sortBy(Bool, (T, T) -> Ordering) <sup>(deprecated)</sup>

```cangjie
public func sortBy(stable!: Bool, comparator!: (T, T) -> Ordering): Unit
```

Function: Enables custom sorting of an array by passing a comparison function that returns an [Ordering](../../core/core_package_api/core_package_enums.md#enum-ordering) result and a stable sorting flag.

> **Note:**
>
> Will be deprecated in future versions. Use [sort](sort_package_funcs.md#func-sorttarrayt-t-t---ordering-bool-bool) instead.

Parameters:

- stable!: [Bool](../../core/core_package_api/core_package_intrinsics.md#bool) - Whether to use stable sorting.
- comparator!: (T, T) ->[Ordering](../../core/core_package_api/core_package_enums.md#enum-ordering) - User-provided comparison function. For example, comparator: (t1: T, t2: T) -> [Ordering](../../core/core_package_api/core_package_enums.md#enum-ordering). If the return value is [Ordering](../../core/core_package_api/core_package_enums.md#enum-ordering).GT, `t1` will be placed after `t2` after sorting; if the return value is [Ordering](../../core/core_package_api/core_package_enums.md#enum-ordering).LT, `t1` will be placed before `t2`; if the return value is [Ordering](../../core/core_package_api/core_package_enums.md#enum-ordering).EQ and stable sorting is used, the relative positions of `t1` and `t2` remain unchanged; if unstable sorting is used, the order of `t1` and `t2` becomes indeterminate.

## extend\<T> Array\<T> <: SortExtension where T <: Comparable\<T>

```cangjie
extend<T> Array<T> <: SortExtension where T <: Comparable<T>
```

Function: This extension implements the `sort/sortDescending` functions for [Array](../../core/core_package_api/core_package_structs.md#struct-arrayt).

Extended type:

- [Array\<T>](../../core/core_package_api/core_package_structs.md#struct-arrayt)

Extended interfaces:

- SortExtension

### func sort() <sup>(deprecated)</sup>

```cangjie
public func sort(): Unit
```

Function: Sorts [Array](../../core/core_package_api/core_package_structs.md#struct-arrayt) in ascending order using unstable sorting.

> **Note:**
>
> Will be deprecated in future versions. Use [sort](sort_package_funcs.md#func-sorttarrayt-bool-bool-where-t--comparablet) instead.

### func sort(Bool) <sup>(deprecated)</sup>

```cangjie
public func sort(stable!: Bool): Unit
```

Function: Sorts [Array](../../core/core_package_api/core_package_structs.md#struct-arrayt) in ascending order.

> **Note:**
>
> Will be deprecated in future versions. Use [sort](sort_package_funcs.md#func-sorttarrayt-bool-bool-where-t--comparablet) instead.

Parameters:

- stable!: [Bool](../../core/core_package_api/core_package_intrinsics.md#bool) - Whether to use stable sorting.

### func sortDescending() <sup>(deprecated)</sup>

```cangjie
public func sortDescending(): Unit
```

Function: Sorts [Array](../../core/core_package_api/core_package_structs.md#struct-arrayt) in descending order.

> **Note:**
>
> Will be deprecated in future versions. Use [sort](sort_package_funcs.md#func-sorttarrayt-bool-bool-where-t--comparablet) instead.

### func sortDescending(Bool) <sup>(deprecated)</sup>

```cangjie
public func sortDescending(stable!: Bool): Unit
```

Function: Sorts [Array](../../core/core_package_api/core_package_structs.md#struct-arrayt) in descending order.

> **Note:**
>
> Will be deprecated in future versions. Use [sort](sort_package_funcs.md#func-sorttarrayt-bool-bool-where-t--comparablet) instead.

Parameters:

- stable!: [Bool](../../core/core_package_api/core_package_intrinsics.md#bool) - Whether to use stable sorting.

