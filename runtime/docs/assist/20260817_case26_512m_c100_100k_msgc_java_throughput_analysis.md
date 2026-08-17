# Case26 512M / C100 / 100K：MSGC 与新 Java 用例吞吐分析及优化记录

## 1. 范围与结果来源

本文记录 `case26_http_complex_json` 在 Linux AArch64 上的一次四侧对比，并聚焦
MSGC 与 Java 的吞吐差距、MSGC 的 GC 行为，以及后续优化方向。

正式结果目录：

- `MSGC_RESULT/Result/case26_http_complex_json/result/20260817_512M_c100_100k_HeapGrow3_Scavenge4_Procs10_compare`
- 汇总报告：`compare_report.md`
- MSGC GC 日志：`msgc/gc.log`
- Java GC 日志：`java/gc.log`
- 实际执行的 Java 源码快照：`java/Case27Benchmark.java.snapshot`
- 实际执行的 MSGC 源码快照：`msgc/benchmark.cj.snapshot`

本文只分析已有产物，未重新执行 benchmark。

## 2. 可复核配置

| 项目 | MSGC | Java |
| --- | --- | --- |
| payload | `100K.json`，99,572 B | 相同 |
| 并发 / client mode | 100 / `sync` | 相同 |
| 压测时长 | 60,000 ms | 相同 |
| 持久对象基线 | 512 MiB，8 workers | 相同 |
| CPU 限制 | `cjProcessorNum=10` | cpuset `0-9`，`ActiveProcessorCount=10` |
| 最大堆 | `cjHeapSize=20gb` | `-Xmx20g` |
| GC 配置 | `marksweep`，`cjHeapGrowth=3`，`cjScavengeGrowth=4`，`cjMSMinGCThreshold=20mb` | JDK 25.0.3 默认 G1 |

Java 运行元数据确认本次使用的是新的
`java_new_benchmark/Case27Benchmark.java`，并不是旧的
`java_benchmark/Case27HttpComplexJsonBenchmark.java`。该差异是解释本轮 Java 吞吐变化
的必要前提。

## 3. 正式吞吐与资源结果

| 指标 | MSGC | Java | Java 相对 MSGC |
| --- | ---: | ---: | ---: |
| 总运行时间 | 60,206 ms | 60,199 ms | 基本相同 |
| 成功请求 | 74,888 | 115,699 | +54.5% |
| 失败请求 | 0 | 0 | - |
| 成功吞吐 | **1,243 req/s** | **1,921 req/s** | **+54.5%** |
| 平均 CPU | 990.08% | 921.17% | MSGC 高 7.5% |
| 平均 RSS | 3,794.68 MB | 2,280.24 MB | MSGC 高 66.4% |
| RSS 峰值 | 3,954.43 MB | 2,309.38 MB | MSGC 高 71.2% |

MSGC 完成的成功请求数约为 Java 的 64.7%。两侧均为零失败，因此吞吐差距不是错误
计数或统计口径导致的。

MSGC 在结束阶段出现的 `Socket is closed` WARN 发生在客户端停止、连接关闭之后；结果
统计为零失败，不能作为吞吐差距的根因。

## 4. 该对比不能只归因于 GC

两侧输入、持久对象和并发参数一致，但请求路径的实现成本并不相同：

- Java 新用例以 Jackson 直接从 `byte[]` 构建和写出 JSON DOM；客户端使用原生
  `Socket`、`TCP_NODELAY`，并将 HTTP header 和 body 拼为一个连续写入；服务端使用
  `HttpServer` 的 virtual-thread executor。
- MSGC 用例使用 `stdx.net.http.Client` / `ServerBuilder`、`HttpRequestBuilder` 和
  `stdx.encoding.json.JsonValue`。服务端和客户端 JSON 解析均为
  `Array<Byte> -> String.fromUtf8 -> JsonValue`，服务端写出为 `JsonObject.toString()`。

因此，本轮 54.5% 差距由以下两层共同组成：

1. Java 新用例的 HTTP、JSON 和线程实现较轻；它与 MSGC 用例不是同成本实现。
2. MSGC 的临时对象、large-object 分配、标记/清扫和 mutator assist 进一步放大了差距。

若目标是量化 Runtime GC 的纯收益，后续需要使两侧 HTTP 和 JSON 数据路径尽可能等价；
若目标是提升 MSGC 端到端吞吐，则应同时优化 stdx 与 Runtime。

## 5. MSGC GC 证据

| 指标 | MSGC | Java G1 |
| --- | ---: | ---: |
| GC 次数 | 122 | 243 |
| GC 总时间 | 6,945.94 ms | 834.23 ms |
| 单次平均 | 56.93 ms | 3.43 ms |
| P95 | 78.61 ms | 4.40 ms |
| 最大值 | 95.95 ms | 84.46 ms |

Java 的次数更多，但绝大部分是 2--5 ms 的 G1 young GC；MSGC 的单轮工作量明显更大。
MSGC 的 GC 时间包含并发阶段，不能直接等价为 STW 时间，但它仍会占用 CPU，且 mutator
assist 会直接落在业务分配路径上。

122 个 MSGC cycle 的日志聚合结果：

| 阶段 | 单轮平均 |
| --- | ---: |
| PreEnumRoots | 9.91 ms |
| ConcurrentMark | 24.85 ms |
| MarkTermination STW | 6.59 ms |
| PostMarkTermination | 13.19 ms |
| GC total | 56.93 ms |

运行时配置为 `gcThreads=5`、`P=10`，即最多 4 个 helper。实际日志中 115 个 cycle
在 `Mark control begin` 时只使用 2 个 helper。与此同时，稳定期的 ConcurrentMark
统计显示 helper 约完成 32%--43% WorkUnit，而 mutator assist 约完成 40%--51%。
这说明业务线程持续为并发标记付费。

典型稳定期 cycle 的观察：

- `liveBytes` 约 732--735 MB，但 `inUse` 约 2.8--3.6 GB；
- fragmentation 约 79.5%；已提交内存为 3.776 GB；
- 每轮发布约 80,700 个 unswept region，后台 sweep 一轮约需 195--218 ms；
- 每轮 large sweep 会回收约 5,985 个对象、24,683 个 span、约 771 MB。

MSGC 的 small-object Region 固定为 32 KB，超过 16 KB 的对象进入 large-object 路径。
100 KB 的 HTTP body、UTF-8 String、JSON 输出和相关临时数组会频繁进入该路径，正是
large sweep 与高碎片的直接来源。

## 6. 现有参数结果说明

目前已有相同 `512M / c100 / 100K / 60s / P10` 的另一组结果：

| `cjHeapGrowth` / `cjScavengeGrowth` | MSGC 吞吐 | MSGC GC 总时间 | MSGC 平均 RSS |
| --- | ---: | ---: | ---: |
| `0.5 / 2` | 1,154 req/s | 12,408.22 ms | 1,982.62 MB |
| `3 / 4`（本文） | 1,243 req/s | 6,945.94 ms | 3,794.68 MB |

当前配置以更高 RSS 换取了约 7.7% 吞吐提升和更少 GC 工作。因此，不应优先把
`cjHeapGrowth` 再调低。

本轮稳态日志中的 heuristic trigger 约为 2,886 MB，而 scavenge high watermark 约为
11,547 MB。GC 在达到 scavenge high watermark 之前已由 mark threshold 触发，故
`cjScavengeGrowth=4` 不是当前最直接的吞吐杠杆；`cjMSMinGCThreshold=20mb` 也远低于
实际阈值，基本不参与本轮 steady state。

## 7. 优化项与优先级

### P0：移除 bytes 与 String 的往返，并降低输出分配

当前 `benchmark.cj` 明确为了兼容 CopyGC 而使用 `JsonValue.fromStr` 和 `toString`。
源码注释说明 CopyGC 基线没有较新的 byte parser 或容量感知 serializer API。MSGC 专用
路径应优先验证以下改动：

1. 直接使用 byte-oriented JSON parser，例如 `JsonValue.fromUtf8Bytes`；
2. 直接写出 bytes 或写入预分配/可复用输出 buffer；
3. 避免 `String.fromUtf8(body)` 和 `JsonObject.toString()` 的完整中间字符串；
4. 为每个 worker 复用 HTTP body / response buffer，并减少 `HttpRequestBuilder`、header
   String 与 `ByteBuffer` 的短命对象。

该项同时降低业务 CPU、large-object churn、GC 标记量与 sweep 压力，是最优先的端到端
优化方向。若要保持 CopyGC/MSGC 严格对比，应把 byte API 下沉为 stdx 的通用实现，或将
结果明确标为 MSGC-specialized benchmark。

### P1：降低 large-object 路径成本

短期应先通过 P0 减少大数组和 String 的产生。长期 Runtime 方向可以评估：

1. 为 16 KB--128 KB 的高频临时对象增加中型对象分配路径，减少 large span 的登记、扫描
   与释放；
2. 为大对象分配增加按 P 的缓存或批量回收，减少全局 large-object 元数据操作；
3. 针对 HTTP/JSON buffer 提供专用可复用分配器。

不要直接修改 `MS_REGION_SIZE`。它目前是固定 32 KB，并有编译器 ABI 静态断言；这属于
高风险架构改动。

### P1：使 helper 扩容响应 mutator assist

当前 mark 控制器的普通初始 helper 上限为 2，helper 目标主要按 scheduler backlog
决定。该策略在本用例中一直维持 2 helper，但 mutator assist 长期接近一半标记工作。

建议在 `MSMarkControllerState` 中将以下信号纳入 helper 扩容：

1. mutator assist WorkUnit 比例；
2. assist debt 增长速度；
3. 分配速率与 mark work density。

目标是在 5 个 GC thread 的现有上限内，优先由 GC helper 完成更多标记工作，减少业务
线程在分配慢路径上的 assist。只提高 `cjGCThreads` 未必有效，因为当前瓶颈是动态 helper
目标通常被策略限制为 2，而不是线程池容量不足。

### P1：缩短 PreEnum 与后台 sweep

PreEnum 每轮约 10 ms，核心工作包含停止 sweep、重置 Region 的 mark/scan workspace，并
在 STW 窗口内翻转 epoch。可优化方向：

1. 对无活动 mark bitmap 的 Region 使用 lazy epoch reset；
2. 跳过空 Region 或未使用 bitmap 的 reset；
3. 在大 collection set 下提高 reset 并行度。

后台 sweep 的默认单 slice 预算为 2 ms。本用例可做受控 A/B：

```bash
cjGCBGSweepMaxTime=4ms
# 对照组再测试 8ms
```

目标不是单纯增加 GC CPU，而是验证更快清空 unswept backlog 后，mutator sweep assist 是否
下降、每成功请求 CPU 是否下降。

### P2：参数实验与脚本透传

`run_compare_linux.sh` 使用 `env -i` 创建 benchmark 子环境。当前没有透传以下 Runtime
参数，直接在外层 shell 设置不会进入 MSGC 进程：

- `cjGCThreads`
- `cjMSParallelSweepMaxHelpers`
- `cjMSAdaptiveGrowth`
- `cjMSAssistMaxMultiplier`

在进行参数 A/B 前，应先将它们加入脚本的 `child_env` optional 变量列表。可实验项：

1. `cjGCBGSweepMaxTime=4ms/8ms`；
2. `cjMSParallelSweepMaxHelpers=2/4`；
3. `cjMSAdaptiveGrowth=1`；
4. `CJ_MS_DISABLE_ASSIST=1`，仅用于判断 assist 是否是吞吐瓶颈，不作为默认配置；
5. `cjHeapGrowth=5/10`，仅在确认可接受更高 RSS 后比较 GC 周期数与吞吐。

## 8. 后续验证标准

每项优化应使用固定 SDK、相同 payload、512 MiB persistent、c100、P10、20 GB heap、
60 s 条件，至少三轮交替执行 `A-B-B-A` 或 `B-A-A-B`。报告中位数及离散度，并同时记录：

- 成功吞吐与失败数；
- 累计 CPU 时间和每成功请求 CPU；
- RSS 平均值与峰值；
- GC phase 时间、`assistWU` 占比、helper 目标；
- `unsweptRegions`、large sweep 回收量和碎片率；
- HTTP/JSON 语义校验结果。

在 byte-oriented JSON 与 buffer 优化完成前，不应把单纯调大堆、调高
`cjScavengeGrowth` 或关闭 assist 作为最终优化结论；这些只应作为定位和权衡实验。
