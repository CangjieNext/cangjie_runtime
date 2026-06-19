# ThreadGroup Iteration Example

The following example creates a [ThreadGroup](../concurrent_package_api/concurrent_package_classes.md#class-threadgroupt), launches tasks, and consumes all task results through group iteration.

The group yields results as tasks complete. This example prints only aggregate values, so the output does not depend on task completion order.

<!-- verify -->
```cangjie
import std.concurrent.*

main() {
    let group = ThreadGroup<Int64>()

    group.launch { 4 }
    group.launch { 9 }
    group.launch { 16 }

    var count = 0
    var sum = 0
    for (result in group) {
        count++
        sum += result
    }

    println("count: ${count}")
    println("sum: ${sum}")
}
```

Execution Result:

```text
count: 3
sum: 29
```
