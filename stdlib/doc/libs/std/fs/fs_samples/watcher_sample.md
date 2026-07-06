# 监听目录变化

示例：
<!-- verify -->

```cangjie
import std.fs.Watcher
import std.time.Duration

main(): Int64 {
    let w = Watcher("./src", recursive: true)
    w.start()
    let fut = spawn {
        => while (let Some(ev) <- w.nextEvent()) {
            println("${ev.kind}: ${ev.path}")
        }
    }
    sleep(10 * Duration.second)
    w.stop()
    fut.get()
    0
}
```

运行结果（取决于 ./src 目录下的实际文件变化）：

```text
create: new_file.cj
modify: existing_file.cj
delete: old_file.cj
```
