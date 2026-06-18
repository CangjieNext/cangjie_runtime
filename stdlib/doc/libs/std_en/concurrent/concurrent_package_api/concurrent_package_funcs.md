# Functions

## func ThreadScope\<T, R>((ThreadGroup\<T>) -> R)

```cangjie
public func ThreadScope<T, R>(fn: (ThreadGroup<T>) -> R): R
```

Function: Runs concurrent tasks in a scope and waits for all launched tasks before returning.

Parameters:

- fn: ([ThreadGroup\<T>](./concurrent_package_classes.md#class-threadgroupt)) -> R - The scoped function that receives a thread group.

Returns:

- R - The return value of `fn`.

Exceptions:

- [Exception](../../core/core_package_api/core_package_exceptions.md#class-exception) - Propagates exceptions thrown by `fn` or launched tasks.
- [Error](../../core/core_package_api/core_package_exceptions.md#class-error) - Propagates errors thrown by `fn` or launched tasks.
