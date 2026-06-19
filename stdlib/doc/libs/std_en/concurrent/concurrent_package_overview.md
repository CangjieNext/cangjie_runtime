# std.concurrent

## Feature Overview

The `concurrent` package provides APIs for structured concurrent task execution.

Use [ThreadScope\<T, R>](./concurrent_package_api/concurrent_package_funcs.md#func-threadscopet-rthreadgroupt---r) to create a scope for related Cangjie threads. The scope receives a [ThreadGroup\<T>](./concurrent_package_api/concurrent_package_classes.md#class-threadgroupt), launches tasks through the group, and does not return until all tasks launched in the group have completed or a failure has been handled.

`ThreadGroup<T>` stores the result of each launched task. The group implements [Iterable\<T>](../core/core_package_api/core_package_interfaces.md#interface-iterablet), so task results can be consumed with a `for` loop. Results are yielded as tasks complete; the iteration order is not guaranteed to match launch order.

If the scoped function throws an [Exception](../core/core_package_api/core_package_exceptions.md#class-exception) or [Error](../core/core_package_api/core_package_exceptions.md#class-error), or if an unhandled task failure is rethrown while consuming task results, `ThreadScope` sends cancellation requests to the tasks launched in the group, waits for outstanding task results to be drained, and then rethrows the failure. Cancellation is cooperative. A task that needs to stop early should check [Thread.currentThread.hasPendingCancellation](../core/core_package_api/core_package_classes.md#prop-haspendingcancellation).

This package provides the following concurrency APIs:

- [ThreadGroup\<T>](./concurrent_package_api/concurrent_package_classes.md#class-threadgroupt): A group used to launch, cancel, and collect results from concurrent tasks.

- [ThreadScope\<T, R>((ThreadGroup\<T>) -> R)](./concurrent_package_api/concurrent_package_funcs.md#func-threadscopet-rthreadgroupt---r): Runs concurrent tasks in a scope and waits for all launched tasks before returning.

## API List

### Functions

| Function Name | Description |
| ------------- | ----------- |
| [ThreadScope\<T, R>((ThreadGroup\<T>) -> R)](./concurrent_package_api/concurrent_package_funcs.md#func-threadscopet-rthreadgroupt---r) | Runs concurrent tasks in a scope and waits for all launched tasks before returning. |

### Classes

| Class | Description |
| ----- | ----------- |
| [ThreadGroup\<T>](./concurrent_package_api/concurrent_package_classes.md#class-threadgroupt) | Provides APIs to launch, cancel, and collect results from concurrent tasks. |

## Tutorial Examples

- [ThreadScope Usage Example](./concurrent_samples/thread_scope_sample.md)
- [ThreadGroup Iteration Example](./concurrent_samples/thread_group_iteration_sample.md)
- [ThreadScope Exception Handling Example](./concurrent_samples/thread_scope_exception_sample.md)
- [ThreadScope Cancellation Example](./concurrent_samples/thread_scope_cancellation_sample.md)
