# Classes

## class ThreadGroup\<T>

```cangjie
public class ThreadGroup<T> <: Iterable<T>
```

Function: Provides APIs to launch concurrent tasks, request cancellation for all launched tasks, and consume task results.

A `ThreadGroup<T>` records one result for every task launched by [launch(() -> T)](#func-launch---t). The result is either the task return value, an [Exception](../../core/core_package_api/core_package_exceptions.md#class-exception), or an [Error](../../core/core_package_api/core_package_exceptions.md#class-error). The group implements [Iterable\<T>](../../core/core_package_api/core_package_interfaces.md#interface-iterablet), so `for (result in group)` can be used to wait for and consume all pending task results.

Results are consumed once. After all launched task results have been consumed, the iterator returns `None`.

> **Note:**
>
> Results are yielded as tasks complete. The iteration order is not guaranteed to match the order in which tasks were launched.

Parent Types:

- [Iterable\<T>](../../core/core_package_api/core_package_interfaces.md#interface-iterablet)

### func cancelAll()

```cangjie
public func cancelAll(): Unit
```

Function: Sends a cancellation request to every task launched by this [ThreadGroup](./concurrent_package_classes.md#class-threadgroupt).

This function requests cancellation through each task's [Future](../../core/core_package_api/core_package_classes.md#class-futuret). It does not immediately stop task execution. A task can observe the request through [Thread.currentThread.hasPendingCancellation](../../core/core_package_api/core_package_classes.md#prop-haspendingcancellation) and decide how to stop.

> **Note:**
>
> Prefer [ThreadScope](./concurrent_package_funcs.md#func-threadscopet-rthreadgroupt---r) for scoped task lifetimes. `ThreadScope` calls `cancelAll` automatically when the scoped function or a launched task fails.

### func iterator()

```cangjie
public func iterator(): Iterator<T>
```

Function: Gets an iterator used to consume results from tasks launched by this [ThreadGroup](./concurrent_package_classes.md#class-threadgroupt).

Calling `next` on the iterator returns the next completed task result. If no completed result is available but launched tasks are still pending, the current thread blocks until a result is available. If the task completed by throwing an [Exception](../../core/core_package_api/core_package_exceptions.md#class-exception) or [Error](../../core/core_package_api/core_package_exceptions.md#class-error), `next` rethrows that failure. If no unconsumed task results remain, `next` returns `None`.

Returns:

- [Iterator\<T>](../../core/core_package_api/core_package_interfaces.md#interface-iteratort) - An iterator over task results.

Exceptions:

- [Exception](../../core/core_package_api/core_package_exceptions.md#class-exception) - Thrown if the consumed task result is an exception.
- [Error](../../core/core_package_api/core_package_exceptions.md#class-error) - Thrown if the consumed task result is an error.

### func launch(() -> T)

```cangjie
public func launch(task: () -> T): Unit
```

Function: Launches a concurrent task in this [ThreadGroup](./concurrent_package_classes.md#class-threadgroupt).

The task runs in a Cangjie thread created by `spawn`. When the task returns normally, its value is stored as a result of this group. When the task throws an [Exception](../../core/core_package_api/core_package_exceptions.md#class-exception) or [Error](../../core/core_package_api/core_package_exceptions.md#class-error), that failure is stored and rethrown when the result is consumed through the group iterator.

Parameters:

- task: () -> T - The task to launch.
