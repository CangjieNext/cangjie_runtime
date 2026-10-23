# Interfaces

## interface SortByExtension\<T> <sup>(deprecated)</sup>

```cangjie
public interface SortByExtension<T>
```

Function: This interface serves as an auxiliary interface for sorting operations. By passing a comparison function that returns an [Ordering](../../core/core_package_api/core_package_enums.md#enum-ordering) result, it enables custom sorting of type T.

> **Note:**
>
> Will be deprecated in future versions.

### func sortBy((T, T) -> Ordering) <sup>(deprecated)</sup>

```cangjie
func sortBy(comparator!: (T, T) -> Ordering): Unit
```

Function: Enables custom sorting of type T by passing a comparison function that returns an [Ordering](../../core/core_package_api/core_package_enums.md#enum-ordering) result.

> **Note:**
>
> Will be deprecated in future versions.

Parameters:

- comparator!: (T, T) ->[Ordering](../../core/core_package_api/core_package_enums.md#enum-ordering) - User-provided comparison function.

### func sortBy(Bool, (T, T) -> Ordering) <sup>(deprecated)</sup>

```cangjie
func sortBy(stable!: Bool, comparator!: (T, T) -> Ordering): Unit
```

Function: Enables custom sorting of type T by passing a comparison function that returns an [Ordering](../../core/core_package_api/core_package_enums.md#enum-ordering) result and a stable sorting flag.

> **Note:**
>
> Will be deprecated in future versions.

Parameters:

- stable!: [Bool](../../core/core_package_api/core_package_intrinsics.md#bool) - Whether to use stable sorting.
- comparator!: (T, T) ->[Ordering](../../core/core_package_api/core_package_enums.md#enum-ordering) - User-provided comparison function.

> **Note:**
>
> Will be deprecated in future versions.

## interface SortExtension <sup>(deprecated)</sup>

```cangjie
public interface SortExtension
```

Function: This interface serves as an auxiliary interface for sorting operations.

### func sort() <sup>(deprecated)</sup>

```cangjie
func sort(): Unit
```

Function: Implements sorting for the corresponding type.

> **Note:**
>
> Will be deprecated in future versions.

### func sort(Bool) <sup>(deprecated)</sup>

```cangjie
func sort(stable!: Bool): Unit
```

Function: Implements stable or unstable sorting for the corresponding type based on the input parameter.

> **Note:**
>
> Will be deprecated in future versions.

Parameters:

- stable!: [Bool](../../core/core_package_api/core_package_intrinsics.md#bool) - Whether to use stable sorting.

### func sortDescending() <sup>(deprecated)</sup>

```cangjie
func sortDescending(): Unit
```

Function: Implements descending order sorting for the corresponding type.

> **Note:**
>
> Will be deprecated in future versions.

### func sortDescending(Bool) <sup>(deprecated)</sup>

```cangjie
func sortDescending(stable!: Bool): Unit
```

Function: Implements stable or unstable descending order sorting for the corresponding type based on the input parameter.

Parameters:

- stable!: [Bool](../../core/core_package_api/core_package_intrinsics.md#bool) - Whether to use stable sorting.

> **Note:**
>
> Will be deprecated in future versions.

