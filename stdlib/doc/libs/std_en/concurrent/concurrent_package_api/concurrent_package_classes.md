# Classes

## class ThreadGroup\<T>

```cangjie
public class ThreadGroup<T> <: Iterable<T>
```

Function: Provides APIs to launch, cancel, and collect results from concurrent tasks.

Parent Types:

- [Iterable\<T>](../../core/core_package_api/core_package_interfaces.md#interface-iterablet)

### func cancelAll()

```cangjie
public func cancelAll(): Unit
```

Function: Cancels all tasks launched by this [ThreadGroup](./concurrent_package_classes.md#class-threadgroupt).

### func iterator()

```cangjie
public func iterator(): Iterator<T>
```

Function: Gets an iterator used to consume results from tasks launched by this [ThreadGroup](./concurrent_package_classes.md#class-threadgroupt).

Returns:

- [Iterator\<T>](../../core/core_package_api/core_package_interfaces.md#interface-iteratort) - An iterator over task results.

### func launch(() -> T)

```cangjie
public func launch(task: () -> T): Unit
```

Function: Launches a concurrent task in this [ThreadGroup](./concurrent_package_classes.md#class-threadgroupt).

Parameters:

- task: () -> T - The task to launch.
