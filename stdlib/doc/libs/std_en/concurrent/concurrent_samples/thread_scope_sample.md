# ThreadScope Usage Example

The following example demonstrates how to launch tasks in a [ThreadScope](../concurrent_package_api/concurrent_package_funcs.md#func-threadscopet-rthreadgroupt---r) and consume their results.

<!-- verify -->
```cangjie
import std.concurrent.*

main() {
    let total = ThreadScope<Int64, Int64> { group =>
        group.launch { 1 }
        group.launch { 2 }

        var sum = 0
        for (result in group) {
            sum += result
        }
        sum
    }

    println(total)
}
```

Execution Result:

```text
3
```
