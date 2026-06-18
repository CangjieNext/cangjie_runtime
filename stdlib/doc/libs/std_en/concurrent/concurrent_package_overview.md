# std.concurrent

## Feature Overview

The concurrent package provides APIs for structured concurrent task execution.

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
