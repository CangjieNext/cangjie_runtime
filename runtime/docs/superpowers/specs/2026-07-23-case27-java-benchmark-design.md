# Case27 Java Benchmark Design

## Goal

为 `case27_http_complex_json` 增加一个与现有 Cangjie/Go 用例功能一致的 Java 8 benchmark，并让 `run_compare.sh`、`run_compare_portsafe.sh` 和比较器产出 `CopyGC / MSGC / Go / Java` 四方对比数据。

## Constraints

- 当前机器只有 Oracle JDK 8，因此 Java 源码必须兼容 Java 8。
- 不引入 Maven、Gradle 或第三方 JSON/HTTP 依赖。
- 保持现有环境变量、HTTP 请求模型、JSON 修改语义、日志格式和结果目录结构兼容。
- 不改变现有 Cangjie 与 Go benchmark 的行为。

## Approach

新增 `java_benchmark/Case27HttpComplexJsonBenchmark.java`，使用 JDK 标准库实现全部能力：

- `com.sun.net.httpserver.HttpServer` 提供本地 `/process` HTTP server；
- `HttpURLConnection` 实现带超时和请求头的 POST client；
- 自定义递归下降 JSON parser 与紧凑 serializer，构造 object/array/scalar 树并保留未修改数字的原始表示；
- `ExecutorService`、`Semaphore` 和原子计数器实现 sync worker 与 async sender；
- 统一输出 `[INFO]`、`[CONFIG]`、`[PROGRESS]`、`[RESULT]`、`[DONE]` 行。

Java server 的数据处理与 Go/Cangjie 对齐：更新 `counters.in/ok`，添加 `processed_seq` 和 `server_ok`，遍历 `events` 修改 `seq`、`score`、`user.attrs.credits`，追加 `custom.product_ids`，并写入 `server_marker`。

## Script Integration

两个对比脚本都新增 Java side，受以下变量控制：

- `JAVA_ENABLE`：默认 `1`，设为 `0` 时跳过 Java；
- `JAVA_BIN` / `JAVAC_BIN`：Java 运行和编译命令覆盖；
- `JAVA_OPTS`：额外 JVM 参数；
- `JAVA_GC_OPTS`：可选的完整 GC 参数覆盖。

Java side 在独立结果目录中编译和运行，保存源码快照、payload 快照、stdout、GC 日志、RSS/CPU 采样和 metadata。脚本根据 Java major version 选择 Java 8 GC 日志参数；运行环境使用干净子 shell，避免污染其他 side。

默认新的结果标签为 `output_msgc_vs_copygc_go_java`，既有结果目录不改名、不覆盖。

## Comparison Integration

`script/compare_results.py` 新增 Java GC 日志识别和耗时解析，继续使用现有多侧 `--run Label=Path` CLI。Java 侧将自动参与：

- 吞吐、总请求、成功/失败、传输字节数；
- RSS/CPU 时间序列及汇总；
- GC 次数、总耗时、平均/最大/P95 耗时及单周期图表；
- Markdown 对比报告。

现有两侧 CLI 和 Cangjie/Go 解析逻辑保持兼容。

## Verification

按 TDD 顺序验证：

1. 先增加 Java GC 日志解析测试、比较器四侧 CLI 测试和 Java JSON/HTTP smoke test，使测试在实现前失败；
2. 编写最小 Java 实现并运行 Java 8 编译、短时 `1K`/低并发 benchmark；
3. 运行四侧脚本的短时验证，确认每个 side 有退出码、`stdout.txt`、`gc.log`/采样文件和 `[RESULT]`；
4. 运行现有 Python 比较器测试以及脚本语法检查；
5. 检查结果报告包含 `Java`，且没有复用或修改现有 benchmark 二进制。

