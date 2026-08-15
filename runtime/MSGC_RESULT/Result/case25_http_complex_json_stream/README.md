# case25_http_complex_json_stream

case25 是可独立复制、运行的复杂 JSON HTTP 对比用例。它包含 Cangjie（MSGC / CopyGC）、
Go 和 Java 的源码、payload、结果汇总脚本；执行不读取 case25 目录以外的用例文件。

Cangjie 服务端使用流式 JSON：请求由 `JsonReader` 消费，未修改的子树按原始 token
转发，目标字段在流中读改写，响应由 `JsonWriter` 写入 `ByteBuffer`；客户端也以流式
reader 校验 `server_ok`。Go 的活动模块 `go_benchmark` 是 case26 风格的标准库 DOM 实现：
完整解析对象树、修改后整体序列化。对应的标准库混合流式实现保存为 `go_benchmark_stream`：
目标字段按 token 改写，未修改 value 解码为 `json.RawMessage` 后直接转写；它不由 runner
默认执行。Java 使用随用例携带的 Jackson Core 流式 API。

Cangjie 的 transform 源码按 SDK 分开保存，避免把 MSGC 专有的 stdx API 编译到 CopyGC：

- `msgc/stream_transform.cj`：使用 `JsonReader.copyValueTo(JsonWriter)` 的批量 raw-copy；
- `copygc/stream_transform.cj`：使用 CopyGC SDK 支持的
  `readValueBytes() -> String.fromUtf8() -> jsonValue()` 回退路径。

两者保持完全相同的 HTTP/JSON 业务语义；`run_compare.sh` 运行每个 Cangjie side 时自动
选择对应目录下的源码，并将实际编译文件记录为结果目录的
`stream_transform.cj.snapshot`。`run_msgc.sh` 固定使用 `msgc/stream_transform.cj`。

## 流式 JSON 与普通 DOM JSON 的区别

case25 的服务端路径如下：

```text
HTTP 请求 InputStream
  → JsonReader 逐 token 读取
  → 命中目标字段时读出、修改并写出
  → 未修改 value 原样转发
  → JsonWriter 写入响应 ByteBuffer
  → HTTP 响应
```

流式实现不会先将完整请求转换成 `JsonObject`、`JsonArray`、`JsonValue` 组成的对象树。
对于未修改字段，MSGC 使用 `reader.copyValueTo(writer)` 批量转发；CopyGC 因 SDK 没有该
API，执行 `reader.readValueBytes()` 后通过 `writer.jsonValue(...)` 写回；
对于目标字段，则只读取所需标量或进入所需对象/数组后立即改写。响应校验也只扫描
`server_ok`，其余字段由 reader 跳过。

| 方面 | Cangjie 流式 JSON | Go 标准库流式 JSON | Java 普通 DOM JSON |
| --- | --- | --- | --- |
| 解析方式 | 顺序读取 JSON token | 先解析完整对象树 | `JsonParser` 顺序读取 token |
| 修改方式 | 走到目标字段时立即修改 | 在对象树上查找、修改节点 | 走到目标字段时立即修改 |
| 未修改字段 | 完整 raw value 转发 | 重新从对象树序列化 | `copyCurrentStructure` 直接复制 token 结构 |
| 主要分配 | Reader/Writer 状态、临时 raw bytes/String | 完整 JSON 的对象、数组、字符串、数值节点 | Jackson parser/generator 缓冲区和响应 bytes |
| 文本保真 | 未修改子树尽量沿用原 JSON 表达 | 空白、转义、数值格式可能被重写 | 空白会规范化；字符串转义与数值 token 由 Jackson 输出 |

因此，流式路径通常能降低大 JSON 或深层 JSON 的对象分配和 GC 压力；但 case25 当前并非
端到端零拷贝：客户端 payload 仍完整保存在内存；Cangjie 服务端响应仍先写入完整
`ByteBuffer`，未修改 value 也会创建临时 bytes 和 `String`。Go 服务端/客户端则直接在
HTTP `Body` 上流式读取；默认 Go 用例先读取完整 HTTP body、构建完整 DOM 对象树，再序列化
响应。Java 服务端将 HTTP 请求流直接交给 Jackson，响应因 `HttpExchange` 需要先知道长度而
暂存为完整 bytes 后写出；Java 客户端直接在 HTTP 响应流上扫描 `server_ok`。Cangjie 与 Java
消除了完整 DOM 对象树；`go_benchmark_stream` 可用于单独研究 Go 的混合流式路径。

持久对象基线和 case26 使用相同的模型：启用时各 worker 分别持有静态根与长生命周期
协程栈根，并持续触碰对象。默认 `CASE25_PERSISTENT_BYTES=0`，因此**不分配任何
初始化对象**。只有显式设置正数才启用；此时 `CASE25_PERSISTENT_WORKERS` 默认为 `8`，
且 bytes 必须是 `workers * 256` 的整数倍。例如：

```bash
CASE25_PERSISTENT_BYTES=2048 CASE25_PERSISTENT_WORKERS=8 ./run_msgc.sh
```

流式改写与 case27 的业务语义一致：

- `counters.in`、`counters.ok` 加一；
- 每个 event 的 `seq` 加一、`score` 加倍；
- `user.attrs.credits` 加倍；
- `custom.product_ids` 追加 `eventIndex + 9999`；
- 每个 event 添加 `server_marker`，根对象添加 `processed_seq` 与 `server_ok`。

因此四方吞吐结果可用于观察端到端表现，但 JSON 解析策略仍不完全相同：Cangjie、Java 为
流式，默认 Go 为 DOM，不能把它作为严格的“同一 JSON 实现”性能结论。若需 Go 流式对照，
可在 `go_benchmark_stream` 单独编译运行；默认 runner 只运行 `go_benchmark`。Java 使用的
`java_benchmark/lib/jackson-core-2.17.2.jar` 已随 case25 一同保存，runner 使用本地 jar 的
classpath 编译和运行，不依赖 Maven、全局 jar 或 case25 外部文件。

完整对比：

```bash
cd runtime/MSGC_RESULT/Result/case25_http_complex_json_stream
CASE25_PAYLOAD=10K CASE25_CONCURRENCY=8 CASE25_DURATION_MS=60000 \
HEAP_SIZE=2gb ./run_compare.sh
```

Linux 使用独立脚本 `run_compare_linux.sh`。脚本根据 `uname -m` 选择
`linux_x86_64_cjnative` 或 `linux_aarch64_cjnative`，并为 x86_64/aarch64
分别选择 Java `amd64`/`arm64` 路径。CopyGC 包也按架构选择候选位置：x86_64
优先 `/home/lipeisheng/cangjie-pkg/260723`，aarch64 优先
`/home/lipeisheng/cangjie-pkg/main/260723`，并验证 envsetup、stdx 和 runtime
均存在后才使用。默认 MSGC SDK 为
`/home/lipeisheng/cangjie/cangjie_compiler_CMSGC/output/msgc`，同时使用 Go 1.26 和 Java 25：

```bash
CASE25_PAYLOAD=100K CASE25_CONCURRENCY=100 CASE25_DURATION_MS=60000 \
CASE25_PERSISTENT_BYTES=512mb CASE25_PERSISTENT_WORKERS=8 \
HEAP_SIZE=20gb PROCS=8 MSGC_HEAP_GROWTH=1 MSGC_SCAVENGE_GROWTH=2 \
MSGC_MIN_GC_THRESHOLD=20mb ./run_compare_linux.sh
```

Linux 脚本使用 `LD_LIBRARY_PATH` 和 `.so`，在隔离子 shell 中分别加载 SDK；
Go 通过 `GO_BIN`（默认取当前 shell 中的 Go，避免清理 PATH 后误选系统旧版本），
并将 `PROCS` 映射为 `GOMAXPROCS`。Java 的编译器和运行时可以分别用
`JAVAC_BIN`、`JAVA_BIN` 覆盖。

四个实现的依赖检查彼此独立。某个 SDK、Go 或 Java 工具不存在时，脚本会输出
`[WARN] skipping ...` 并只跳过对应实现，继续执行其他可用实现；只有所有实现都
不可用时才退出。也可以用 `COPYGC_ENABLE=0`、`GO_ENABLE=0` 或 `JAVA_ENABLE=0`
显式关闭不需要的实现。

`run_compare_linux.sh` 会在本目录 `result/` 下生成一个时间戳结果目录，运行依赖可用的
MSGC、CopyGC、Go、Java；成功运行至少两项时，由相邻的 `script/compare_results.py`
输出汇总。`throughput` 始终按成功请求数 `ok` 计算。

默认 `CASE25_PERSISTENT_BYTES=0`，四侧都不初始化持久对象；显式设为正数时才启用，
`CASE25_PERSISTENT_WORKERS` 默认为 `8`，且 bytes 必须是 `workers * 256` 的整数倍。
为了复用复制进来的 Go/Java 源码，runner 在它们的子进程中映射为 `CASE27_*` 环境变量，
这是 case25 内部兼容细节，不依赖 case27 文件。

只运行 MSGC 时可继续使用 `run_msgc.sh`；它固定以 `-O2 --gc-mode=marksweep` 编译。
