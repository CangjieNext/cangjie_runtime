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

本文只分析已有产物，未重新执行 benchmark。后续基于当前工作区复核时发现，上述正式
结果目录目前不在本地工作区中，因此本文记录的吞吐、GC cycle 和阶段耗时只能视为当时
分析结果，无法从原始日志再次独立核验。IDE 中现有的
`20260817_512M_c1000_100k_HeapGrow1_Scavenge2_compare` 属于不同并发与 GC 配置，不能
用来替代本文的 c100 正式结果。

第 7 节新增优化项是基于 2026-08-17 当前工作区源码做的符号级复核；当前 Runtime
工作区包含未提交改动，未必等同于生成上述正式结果时使用的 runtime snapshot。因此，
文中的历史性能数据与当前代码机制应分开理解，新增优化均需用重新构建后的固定 SDK
单独验证，不能由历史数据直接推导收益。

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
  `stdx.encoding.json.JsonValue`。当前 MSGC 专用用例的服务端和客户端均直接调用
  `JsonValue.fromOwnedUtf8Bytes(body)`，服务端通过
  `JsonObject.toStringWithCapacity(...)` 写出，不再经过显式的
  `String.fromUtf8(body)` 或无容量提示的 `JsonObject.toString()`。

owned 入口已经不再创建同尺寸的私有 `parserData`。本次又将严格 UTF-8 校验融合进
JSON 结构和字符串扫描，不再先调用 native 接口对整个 body 做一次独立预扫描；安全入口
`fromUtf8Bytes` 仍保留“严格校验并复制”的隔离语义。输出侧的
`toStringWithCapacity` 通过 `String.withRawData` 接管 `WriteBuffer` backing array，
`HttpRawBody(String)` 又直接读取该 String 的 raw data，所以当前输出路径不存在一次额外的
完整 String-to-bytes 数据复制。

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
在 `Mark control begin` 时的初始 helper 目标为 2。该日志只记录控制器初始值；当前
`MSMarkControllerState` 会在 Mark 期间按固定采样周期动态调整 helper，因此不能据此断言
整轮始终只有 2 个 helper。与此同时，稳定期的 ConcurrentMark 统计显示 helper 约完成
32%--43% WorkUnit，而 mutator assist 约完成 40%--51%，可以确认业务线程持续为并发
标记付费，但仍需补充 helper 目标的周期内聚合数据才能判断扩容策略是否响应不足。

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

### P0：先补齐组件级观测与脚本透传

`benchmark.cj` 已实现 `CASE26_JSON_PROFILE`，可以分别累计服务端 parse、mutate、serialize
以及客户端 parse 耗时。但 `run_compare.sh` 和 `run_compare_linux.sh` 都通过 `env -i`
创建隔离子环境，当前 optional 变量列表没有透传该开关，所以在外层设置后会被静默丢弃。

在继续优化前，应先向两个基础脚本补充以下可选变量；
`run_compare_new_java.sh` 和 `run_compare_linux_new_java.sh` 复用基础脚本，因此会自动继承：

- `CASE26_JSON_PROFILE`
- `cjGCThreads`
- `cjMSParallelSweepMaxHelpers`
- `cjMSAdaptiveGrowth`
- `cjMSAssistMaxMultiplier`
- `CJ_MS_STW_SWEEP`，仅作为并发 sweep 根因诊断开关

JSON profile 会在每次请求上读取时间，可能扰动正式吞吐，因此只用于短时归因运行；正式
对比应关闭该开关。GC 参数必须在运行日志中打印最终解析值，避免把未生效的外层环境变量
误认为实验组配置。

### P0：DOM 标量节点改为原地更新（已实施）

实施前的 `mutateRequestJson` 对 `counters.in/ok` 以及每个 event 的 `seq`、`score`、
`user.attrs.credits` 都创建新的 `JsonInt` / `JsonFloat`，再通过 `JsonObject.put` 替换旧节点。
`100K.json` 包含 85 个 event，因此每个服务端请求会额外创建：

```text
2 + 85 * 3 = 257 个标量 JSON 节点
```

stdx 已提供 `JsonInt.setValue` 和 `JsonFloat.setValue`，当前 case26 也已经使用这两个接口
原地修改。本用例没有保存这些标量节点的外部别名，因此可避免约 257 次节点分配和对应
HashMap replacement。按本文
1,243 req/s 估算，相当于减少约 31.9 万个短命节点/秒。`product_ids` 新元素、根 marker
和 event marker 属于语义要求，仍需正常创建。

历史单次/少量复测没有证明稳定吞吐增益，但恢复 replacement 调用点也没有更好；当前保留
setter API 和 case26 原地更新是合理的。它是已完成的分配削减，不再列为下一项待实施优化。

### P0：增加接管输入数组的零拷贝 DOM parser（已实施）

当前 `JsonValue.fromUtf8Bytes` 已消除了显式的 `String.fromUtf8`，但 `parseUtf8Bytes`
内部仍会分配与输入等大的 `parserData`，再调用 `CJ_JSON_ValidateUtf8AndCopy` 完整复制。
这样做是因为未转义的 DOM String 会保留 parser backing array 的 slice，现有安全 API 必须
防止调用方在 parse 后修改原数组。

case26 的 HTTP body 是新建的精确长度 Array，parse 后不会再修改。可以新增语义明确的
`fromOwnedUtf8Bytes`（或等价内部接口）：

1. UTF-8 校验仍然保留，但直接在输入数组上完成；
2. DOM 接管并保留该 backing array，不再创建 `parserData`；
3. API 明确规定调用后原 Array 不得再修改；
4. 不改变现有 `fromUtf8Bytes` 的复制隔离语义。

实施结果（2026-08-17）：stdx 已新增公开入口
`JsonValue.fromOwnedUtf8Bytes(data: Array<Byte>)`，owned 路径直接用输入数组构造
`JsonParser`，不再分配同尺寸 `parserData`。最初版本使用只读
`CJ_JSON_ValidateUtf8` 做整包校验；本次 integrated validation 实施后已经移除该 native
接口，owned 路径不再进行整包预扫描。`fromUtf8Bytes` 仍通过
`CJ_JSON_ValidateUtf8AndCopy` 保留原有严格校验和私有复制语义。

focused test 验证了安全入口不别名、owned 入口保留输入 backing array，以及两种入口均拒绝
非法 UTF-8；本次又补充 standalone continuation、overlong、surrogate、超过 U+10FFFF、
key 中截断多字节序列、尾随非法高位字节、未转义控制字符，以及中文和 emoji 合法输入。

case26 的服务端请求和客户端响应解析均已切换到 owned 入口。融合校验前的 owned-only 单次
验证得到 167,849 次成功、0 失败、`2787 req/s`；平均 CPU 590.63%，平均/峰值 RSS 分别为
4008.13/4222.91 MiB。该数据作为本次单次复测的最近同配置参考，不构成严格 A/B 基线。

两次脚本都请求 `cjHeapSize=20gb`，但 runtime 的有效上限为 16 GB，stdout 明确报告该参数
不受支持；GC 日志中的实际 `softLimit` 为 16384 MB。因此后文把 `20gb` 仅记为脚本请求值，
不再描述为实际生效的堆上限。

每个请求的服务端请求解析和客户端响应解析各可移除一份约 100 KB 的大数组。按本文吞吐
仅以 payload 大小估算，可少复制约 247 MB/s，同时减少两次 large-object 分配及后续 GC
登记、标记和回收工作。这比把输出 String 改成 bytes 更优先，因为当前输出 raw data 已经
被 HTTP body 直接复用。

### Java/Jackson 与当前仓颉 JSON 路径对照

当前两侧做的 JSON 业务工作是等价的，不存在 Java 只提取少数字段而仓颉构建完整 DOM 的
口径差异：

1. Java 服务端使用 `MAPPER.readTree(body)`，执行与仓颉相同的 counters、event、嵌套
   credits、product id 和 marker 修改，再调用 `writeValueAsBytes(root)`；
2. Java 客户端也对完整 response 执行 `readTree(body)`，然后读取 `server_ok`；
3. 仓颉服务端和客户端同样各做一次完整 DOM parse，服务端做同样的 mutation 和完整 compact
   serialization；当前标量原地更新使仓颉 mutation 分配量反而小于 Java 的节点 replacement；
4. Java 的 raw socket、`TCP_NODELAY`、header/body 单次连续写与仓颉 `stdx.net.http` 不同，
   这部分差异不能归因于 JSON 或 GC。若后续需要纯 JSON 归因，应增加固定 byte array 的
   parse-mutate-serialize 组件 microbenchmark，而不是修改端到端用例的语义。

Jackson 2.18 的 `JsonFactory.createParser(byte[])` 会把原始 byte array 直接交给
`ByteSourceJsonBootstrapper`，随后由 `UTF8StreamJsonParser` 在 token 解析过程中处理
`_decodeUtf8_2/3/4` 和非法字节；当前实现中没有与仓颉 owned 入口相同的“先整包 UTF-8 校验、
再从头解析”步骤。Jackson 还默认启用 `CANONICALIZE_FIELD_NAMES`、`INTERN_FIELD_NAMES` 和
线程本地 buffer recycling。不过这些机制不能直接照搬：仓颉 `String` 是由 RawArray、start、
length 组成的值类型，当前无转义 key 已经只是输入 backing array 的零拷贝 view，并没有为每个
key 再复制字符数组。

当前 `100K.json` 的结构也给出了更明确的优化边界：

| 项目 | 数量/特征 |
| --- | ---: |
| 输入 | 99,572 B，全 ASCII，无反斜杠 escape、无 `&` |
| object / array / scalar | 1,364 / 851 / 6,304 |
| object key | 6,055 个、仅 87 个唯一 key、最长 14 B |
| String value | 2,299 个、最长 19 B |
| number / boolean / null | 3,646 / 341 / 18 |
| object 大小 | 1--7 fields 共 1,279 个；12 fields 共 85 个 |
| array 大小 | 850 个不超过 5 items；1 个为 85 items |

因此不能通过全局放大 object/array 初始容量来解决问题；它只会让绝大多数小容器清零更多
空间。当前所有 8,354 个 key/String value 都进入短字符串路径，且都不需要 escape，这才是
更集中的 workload 特征。

### P0：owned parser 将 UTF-8 校验融入结构解析（已实施）

实施前，`parseOwnedUtf8Bytes` 先以 `CJ_JSON_ValidateUtf8` 线性扫描完整约 100 KB 输入，
再由 `parseValidatedUtf8Bytes` 重新解析同一数组。owned API 已经消除了输入 clone，但仍保留
一遍整包只读流量；服务端 request 与客户端 response 各发生一次。

本次保留安全 `fromUtf8Bytes` 的现有“校验并复制”语义，只为 owned 路径增加 integrated
validation mode，具体实现为：

1. JSON 结构字符、数字和 literal 仍由现有 parser 校验；结构位置遇到高位 byte 直接拒绝；
2. string scanner 在寻找 quote/backslash 的同一遍循环中，仅在遇到高位 byte 时执行严格
   UTF-8 code-point 校验，同时拒绝未转义的 `< 0x20` 控制字符；
3. escaped string 保持现有 `handleEscape` 语义，不引入每个短字符串一次 native FFI；
4. parser 成功到达输入末尾即同时完成 JSON 与 UTF-8 校验；尾随数据分支也会校验高位字节，
   不再调用整包 `CJ_JSON_ValidateUtf8`；
5. native 层只保留安全入口使用的 `CJ_JSON_ValidateUtf8AndCopy`，安装产物中已经不存在
   `CJ_JSON_ValidateUtf8` 符号。

该设计和此前回退的“每个字符串 native scan”不同：它不增加 FFI 次数、不扩大
`JsonString` descriptor，也不增加额外 token pass。focused test 先证明旧安装产物会错误接受
字符串中的未转义控制字符，随后在新安装产物上通过，覆盖非法首字节/续字节、overlong、
surrogate、超过 U+10FFFF、高位 byte 出现在结构或尾随位置、未转义控制字符、Unicode
合法输入、尾随垃圾和未闭合字符串。

构建安装只涉及 `cangjie_stdx_region_mark`，使用其 `build.py build` 和
`build.py install --prefix .../output/msgc`；compiler、runtime 和 stdlib 均未重编或替换。
安装后的 `libstdx.encoding.json.dylib` 与构建产物 SHA-256 一致，均为
`932cf0e1c8a377aeafbdcac3a59498a3affbb55520d30822e724c04699479320`。focused test 的
动态库探针确认 JSON stdx 和 runtime 均从该 MSGC SDK 加载。

随后仅运行一次 60 秒 MSGC case26：512M persistent、100K、c100、P10、
`cjHeapGrowth=3`、`cjScavengeGrowth=4`，并请求 `cjHeapSize=20gb`（如上所述，runtime 实际
采用 16 GB soft limit）。结果目录为
`result/20260817_512M_c100_100k_HeapGrow3_Scavenge4_compare_2/msgc`，结果如下：

| 指标 | 融合校验后单次结果 | 最近 owned-only 单次参考 |
| --- | ---: | ---: |
| 成功 / 失败 | 172,834 / 0 | 167,849 / 0 |
| 吞吐 | 2,872 req/s | 2,787 req/s |
| 平均 CPU | 613.46% | 590.63% |
| 平均 / 峰值 RSS | 4085.14 / 4326.45 MiB | 4008.13 / 4222.91 MiB |
| GC 次数 / GC 总时间 | 229 / 9389.07 ms | 220 / 8946.00 ms |

单次吞吐比最近参考高 85 req/s，约 `+3.05%`，且 172,834 个请求全部成功，证明 integrated
validation 可以稳定跑完目标负载。但 CPU、RSS 和 GC 工作量也同时上升，且本轮按要求没有
做 A/B 或重复采样，因此只能认为“功能正确、单次吞吐未回退且有所上升”，不能据此声称稳定的
3.05% 性能收益。按两个方向各约 100 KB 估算，本次消除了每请求约 200 KB 的整包预扫描，
在本轮吞吐下相当于避免约 574 MB/s 的额外只读扫描流量。

#### 为什么吞吐收益有限

2026-08-17 对现有三次同配置结果、parser 源码和一次 10 秒 `sample` 继续归因后，结论是本项
优化的实际成本占比本来就很小，而且新增的逐字节判断抵消了部分收益。

首先要区分 owned parser 的两步优化：上一项已经消除了输入 clone 和同尺寸大数组分配；本项
只消除只读 UTF-8 预扫描，不再减少任何 DOM 或 large-object 分配。因此本项不应预期带来上一项
那样的 GC 收益。融合前后 GC 时间分别为 53.30/54.32 us 每请求，large sweep 回收量约为
0.622/0.625 MiB 每请求，基本不变，符合这一实现差异。

其次，`100K.json` 的 99,572 字节全部是 ASCII。旧 native validator 对 ASCII fast path 每次
处理 8 字节；即使服务端和客户端各扫描一次，本轮也只有约 0.533 GiB/s 的连续读取，远低于
本机内存带宽。融合后虽然删掉了该扫描，但 `parseJsonStringValue` 必须在每个字符串字节上增加
`next < 0x20` 和 `integratedUtf8Validation && next >= 0x80` 判断。payload 的 key 和 String
value 内容合计 51,770 字节，占输入 51.99%；它们全部走 ASCII 分支，严格多字节校验函数
`validateUtf8SequenceAt` 不会执行，但新增分支仍然存在于最热的字符串循环中。

10 秒采样目录为
`result/20260817_512M_c100_100k_HeapGrow3_Scavenge4_compare_3/msgc`。该次 benchmark 只运行
20 秒并受 `sample` 扰动，吞吐不用于比较；调用栈仅用于热点归因。排除内核等待后，在已归类的
exclusive samples 中，JSON 约占 62.8%，Runtime/GC/分配约占 19.4%。JSON 内部热点分布为：

| JSON exclusive samples 分类 | 占 JSON samples |
| --- | ---: |
| `parseJsonStringValue` | 22.55% |
| writer（含 `appendEscapeSmall`） | 21.47% |
| 数字解析 | 16.84% |
| object 解析 | 12.48% |
| HashMap / ArrayList 容器插入 | 12.36% |
| parse dispatch | 7.89% |
| array 解析 | 3.99% |

`parseJsonStringValue` 仍是最大的 JSON exclusive 热点；独立 UTF-8 validator 已经不在调用栈
中，但总瓶颈只是回到了 DOM 构建、数字解析、HashMap 插入、writer 二次 escape 扫描以及
Runtime 分配/assist。Runtime 源码也与采样一致：DOM 分配进入
`RegionSpaceMS::AllocateRegularObject`，Region refill/消费通过 `UpdateMSMarkAssistDebt` 和
`UpdateMSSweepAssistDebt` 计入债务，随后可能调用 `MarkSweepCollector::MSMarkAssist` 或
`CentralRegionPool::MSSweepAssist`，所以大量 DOM 节点和容器分配仍会让业务线程承担 GC 工作。

最后，三次同配置吞吐为 2828、2787、2872 req/s，均值 2829 req/s，样本标准差约 1.50%，
最大最小差正好约 3.00%。融合校验后的 2872 req/s 只是比三次均值高 1.52%，单次 `+3.05%`
处于现有波动范围内。按 benchmark 时间窗换算，CPU 成本从 2.1192 增至 2.1355 ms/请求，增加
约 0.77%；每 CPU 秒完成的请求从 471.88 降至 468.27。换言之，本次更高吞吐伴随着更高 CPU
占用，并没有证据证明单位 CPU 效率提高。

因此本项优化应保留：它简化了 owned 路径并避免重复扫描，正确性也已覆盖；但不应继续把它视为
主要吞吐杠杆。下一步更值得优先处理采样中已经可见的 writer escape 重扫，其次是数字解析和
高重复 key 的 HashMap/hash 成本。任何下一项优化仍需用重复采样或正式 A/B 区分约 1.5% 的
自然波动。

### P0/P1：解析时缓存 escape 分类，writer 对安全字符串直接复制（已实施）

`parseJsonStringValue` 已经为寻找 closing quote/backslash 扫描每个字符串；compact writer
随后又让所有短 key/value 进入 `appendEscapeSmall`，再次检查控制字符、引号、反斜杠、DEL
和 HTML-safe `&`。本 payload 每次 parse/serialize 涉及 6,055 个 key 和 2,299 个 String
value，全部最长不超过 19 B 且无需 escape。

下一版可以在 parser 已有扫描中顺便生成紧凑的 escape 分类，使 writer 对已确认安全的内容
直接 `append`：

1. `JsonString` 保存 compact-safe / html-safe 状态；parser 在原扫描中产生状态，公开构造器和
   `setValue` 在赋值时更新状态；
2. `JsonObject` 保存 all-keys-safe 状态，parser 内部插入和公开 `put` 更新该状态；
3. `getFields()` 会暴露可变 `HashMap`，调用后必须把对象永久降级为 unknown，writer 对该对象
   的 key 继续走完整 escape，不能依赖可能失效的缓存；从外部 map 构造的对象也从 unknown
   开始；
4. compact writer 只在状态明确安全时直接 range-copy，unknown/unsafe 保持现有实现。

这不是此前已经回退的 raw-token representation：不保存 source offset/length/Option，不做
逐字符串 native 调用，也不延迟物化 object key。实施前必须用对象布局工具确认增加状态位不会
让 `JsonString`/`JsonObject` 跨越新的 size class；如果对象尺寸增加，应先改用 padding 内 bit
或只实施 object 级 key 状态。回归必须覆盖 quote、backslash、全部控制字符、DEL、`&`、原生
UTF-8、escape 解码后的特殊字符、`setValue`、`put`、`getFields` 后修改以及 htmlSafe 开关。

#### 2026-08-17 实施与单次验证

本次在 stdx JSON DOM 内实现了三态分类：`unknown`、`compact-safe`、`html-safe`。无 escape 的
parser 快速路径在寻找 closing quote 的同一轮扫描中生成分类；存在反斜杠的字符串保守标记为
`unknown`。`JsonString` 的公开构造器和 `setValue` 会重新分类，writer 只对状态明确安全的值
直接 `append`，其余继续使用原有 escape 路径。

对象 key 使用 object 级 `allKeysHtmlSafe`：parser 通过包内 `putParsed` 传播 key 分类，公开
`put` 会检查新 key；从外部 `HashMap` 构造或调用公开 `getFields()` 后永久降级到保守路径。
内部只读遍历改用 `getFieldsForRead()`，避免序列化和 DataModel 转换误触发降级。compact 和
formatted writer 都复用该状态，key 仍保持原有始终 HTML-safe 的语义。

涉及文件：

- `src/stdx/encoding/json/json_object.cj`：分类、`JsonString` 状态和 object key 状态；
- `src/stdx/encoding/json/native.cj`：解析期分类传播；
- `src/stdx/encoding/json/write_buffer.cj`：安全字符串直接复制；
- `src/stdx/encoding/json/to_json.cj`：内部只读 map 访问；
- `test/dom_json_escape_classification.cj` 和 `test/check_dom_json_escape_cache.sh`：语义与结构回归。

构建安装只执行 stdx release/MSGC 流程，144/144 target 成功，随后通过 `build.py install`
安装到 `/Users/lipeisheng/code/cangjie/cangjie/cangjie_compiler_region_mark/output/msgc`。安装前后
`libstdx.encoding.json.dylib` SHA-256 从
`932cf0e1c8a377aeafbdcac3a59498a3affbb55520d30822e724c04699479320` 变为
`4f3b4618aa5e0b1f4b2217fafdd2d324a00dd4f6b0617a3213ab699a8be6c570`；runtime dylib MD5
保持 `d41e9d86deb12474dbb6a46b37c103a9`。重新编译后的
`dom_json_escape_classification` 和 `dom_json_utf8_validate_copy` 均通过，并用 dyld 日志确认加载
的是上述 MSGC SDK 中的新 JSON dylib。

按约定只执行一次 60 s MSGC：100K、c100、512 MiB persistent、P10、HeapGrowth=3、
ScavengeGrowth=4、min threshold=20 MiB；命令请求 `cjHeapSize=20gb`，但 GC 日志中的实际
`softLimit` 为 16384 MiB。compiler、runtime、stdlib 均来自同一 `output/msgc` SDK，其中
runtime 未替换；stdx 是本次从 `cangjie_stdx_region_mark` 新构建并安装的产物。结果目录为
`result/20260817_512M_c100_100k_HeapGrow3_Scavenge4_compare_4/msgc`。

| 指标 | 最近同配置参考 `compare_2` | escape cache `compare_4` | 变化 |
| --- | ---: | ---: | ---: |
| 吞吐 | 2872 req/s | 2737 req/s | -4.70% |
| 成功 / 失败 | 172834 / 0 | 164619 / 0 | - |
| CPU 平均 | 613.46% | 574.04% | -6.43% |
| RSS 平均 / 峰值 | 4085.14 / 4326.45 MiB | 3891.06 / 4159.72 MiB | -4.75% / -3.85% |
| GC 次数 / 总时间 | 229 / 9389.07 ms | 220 / 8165.13 ms | -3.93% / -13.04% |
| CPU 成本 | 2.1355 ms/请求 | 2.0971 ms/请求 | -1.80% |
| 每 CPU 秒请求数 | 468.27 | 476.86 | +1.83% |

单次吞吐没有增长，因此不能把该实现判定为吞吐优化。另一方面，每请求 CPU 成本略降，而整轮
CPU 使用率比参考低 6.43%，说明总吞吐下降与机器/调度负载未吃满同时发生，不能仅凭这一轮证明
escape cache 本身回退。每次 GC 覆盖的请求数由 754.73 降至 748.27（约 -0.86%），也提示新增
DOM 状态可能带来轻微对象布局/分配密度成本；本轮未做对象布局测量，不能确认是否跨 size class。
按本任务约束没有追加回退重编或交错 A/B。当前结论是：功能正确，单位 CPU 效率呈小幅正向趋势，
但端到端吞吐单次结果为负；在决定保留为默认实现前，需要对象布局检查和多轮交错 A/B。

#### 2026-08-18：改为仅缓存对象 key，并恢复 JsonString 布局

后续对象布局检查确认，上一版为 `JsonString` 增加 `Int64 escapeClass` 后，其实例大小从 16 B
增长到 24 B，跨入新的 MSGC size class。case26 服务端和客户端每请求合计约创建 4,768 个
`JsonString`，因此每请求约增加 38 KB 分配；同时分类发生在服务端和客户端两次解析中，而
writer 快路径只使用一次。这会抵消免除 value escape 扫描的收益。

修正版删除了 `JsonString.escapeClass` 和 `appendEscapeClassified`，普通字符串恢复原 writer
escape 路径；只保留 `JsonObject.allKeysHtmlSafe`。parser 仅在读取对象 key 时融合 HTML-safe
分类，普通字符串 value 不再执行分类分支。公开 `put`、外部 map 构造和 `getFields()` 暴露后的
保守降级规则保持不变。构建产物的 `JsonString` type metadata 为 1 个字段、实例大小 16 B；
`JsonObject` 的 Bool 仍利用原有 padding，实例大小保持 16 B。

只重新构建并安装 stdx，Release/MSGC 构建 144/144 target 成功。安装后的
`libstdx.encoding.json.dylib` SHA-256 为
`72345b44341fd4bd80340677e6f1014793c129f3ded7727a10894159da64238d`，与 build 产物一致；runtime
仍为 `d41e9d86deb12474dbb6a46b37c103a9`，没有替换。重新编译后的 escape/UTF-8 focused tests
均通过，dyld 确认加载同一 `output/msgc` SDK 的 JSON stdx 和 runtime。

唯一一次 60 s MSGC 结果位于
`result/20260818_512M_c100_100k_HeapGrow3_Scavenge4_compare/msgc`：

| 指标 | 缓存前参考 `compare_2` | per-string cache `compare_4` | 仅 object key cache |
| --- | ---: | ---: | ---: |
| 吞吐 | 2872 req/s | 2737 req/s | 2862 req/s |
| 成功 / 失败 | 172834 / 0 | 164619 / 0 | 172196 / 0 |
| CPU 平均 | 613.46% | 574.04% | 610.19% |
| RSS 平均 / 峰值 | 4085.14 / 4326.45 MiB | 3891.06 / 4159.72 MiB | 3847.71 / 4092.45 MiB |
| GC 次数 / 总时间 | 229 / 9389.07 ms | 220 / 8165.13 ms | 242 / 9042.29 ms |

修正版相对 per-string cache 单次吞吐提高约 4.57%，相对缓存前参考仅低 0.35%，CPU 平均也恢复
到相近水平；这说明去掉 24 B `JsonString` 和双端 value 分类消除了上一实现的主要结构性成本。
但单次结果仍没有证明 object-key cache 相对缓存前存在稳定吞吐收益，应该表述为“恢复到参考水平，
同时保留无对象膨胀的 key 快路径”，而不是稳定提升 4.57%。本轮按约定没有运行 CopyGC、Go、
Java，也没有追加完整 A/B。

### P1：为小 JsonObject 设计 flat storage，再按需提升为 HashMap

当前每个 `JsonObject(8)` 都立即构造 `HashMap<String, JsonValue>(8)`。标准库实现会同时分配
buckets array 和 entries array；而本 payload 有 1,364 个 object，平均仅 4.44 个字段，只有
85 个 object 超过 8 个字段。每个 object 都承担两份容器 backing array、初始化和 hash 插入，
比仅调整 12-field object 的扩容更值得关注。

可以评估 JSON 专用 small-object representation：前 6--8 个 `(String, JsonValue)` 使用紧凑
flat storage 和线性查找，超过阈值后一次性提升为 `HashMap`。writer 应直接迭代内部 storage；
公开 `getFields()` 被调用时物化 HashMap 并永久切换，以保持返回 map 可继续修改的现有语义。
实现还必须保持重复 key 覆盖、`put/get/containsKey`、遍历和 mutation 行为。

这是比 parser 微调更大的 stdx API 内部改造，先做 1/3/5/7/12-field lookup/insert/iterate
microbenchmark，再决定阈值。array 侧可单独对 `INITIAL_JSON_ARRAY_CAPACITY=4` 做小矩阵：
850/851 个 array 不超过 5 items，但必须同时统计 5-item array 的额外 grow，不能只看分配容量。

#### 2026-08-18：small JsonObject flat storage 实施与单次验证

已按阈值 8 实现 JSON 专用 small-object representation。`JsonObject` 在 small mode 直接持有
一个 `Array<(String, JsonValue)>` 和逻辑长度，`put/get/containsKey` 最多线性扫描 8 个条目；
已有 key 在原槽位更新，不触发提升。第 9 个不同 key 才一次性创建 `HashMap` 并迁移条目。
公开 map 构造从一开始进入 map mode；公开 `getFields()` 会提升并返回保留的同一个 map，调用方
后续 mutation 仍然可见。compact/formatted writer、容量估算和 DataModel 转换均分支遍历内部
flat/map storage，不会因为只读操作而提升。

TDD 守卫先在旧实现上以“缺少 `SMALL_JSON_OBJECT_CAPACITY`”失败，改动后通过。重新构建并
安装 stdx 后，新编译的 Cangjie focused tests 覆盖了 8-field 插入与重复覆盖、第 9 个 key
提升、compact/formatted JSON、map constructor alias、`getFields()` mutation、unsafe key
escaping 和 owned UTF-8 parser，全部退出 0。Release/MSGC stdx 构建为 144/144 target；安装后
`libstdx.encoding.json.dylib` SHA-256 为
`ea3ec025078efc265867174d5b0d804646b1e1b7e22a6f2abcab5bc621760a47`。runtime MD5 仍为
`d41e9d86deb12474dbb6a46b37c103a9`，本轮未替换 runtime，也未重编 compiler/LLVM/stdlib。

唯一一次 60 s MSGC 结果位于
`result/20260818_512M_c100_100k_HeapGrow3_Scavenge4_compare_1/msgc`。benchmark 与 payload
快照 SHA-256 均和源文件一致，成功 203,225、失败 0，耗时 60,137 ms：

脚本仍按既定配置传入 `cjHeapSize=20gb`，但本 runtime 明确提示有效上限为 16 GiB，GC 日志中的
实际 `softLimit` 为 16,384 MiB。最近参考使用相同脚本参数和同一 runtime，因此下表仍是同条件
相对比较；后续报告不应把这轮的实际 soft limit 记成 20 GB。

| 指标 | 最近同配置参考 | small-object flat storage | 单次变化 |
| --- | ---: | ---: | ---: |
| 吞吐 | 2862 req/s | 3379 req/s | +18.06% |
| 后 30 s 平均吞吐 | 2888.1 req/s | 3445.0 req/s | +19.28% |
| CPU 平均 / 峰值 | 610.19% / 665.90% | 625.53% / 681.20% | +2.51% / +2.30% |
| 每成功请求 CPU | 2.1536 ms | 1.8684 ms | -13.24% |
| RSS 平均 / 峰值 | 3847.71 / 4092.45 MiB | 3771.81 / 3884.02 MiB | -1.97% / -5.09% |
| GC 次数 / 总时间 | 242 / 9042.29 ms | 189 / 5247.52 ms | -21.90% / -41.97% |

这次收益和设计目标一致：payload 中绝大多数对象不再为每个 `JsonObject` 分配并初始化
HashMap 的 buckets 与 entries 两个 backing array，hash 计算/插入也变成至多 8 项的顺序访问；
分配压力降低后 GC 次数和总时间同步下降。CPU 平均略高是吞吐提高后机器做了更多有效工作，
而每请求 CPU 明显下降。由于按约定只执行了一次 MSGC、没有回退重编或交错 A/B，不能据此给出
跨机器的稳定增益区间；但 +18% 总吞吐、+19% 稳态吞吐以及 CPU/request、RSS、GC 同向改善，
已经明显超过此前几轮的噪声幅度。本轮没有运行 CopyGC、Go 或 Java。

#### 2026-08-18：flat storage 后重新采样与下一轮优先级

为避免继续依据 flat storage 之前的热点排序，使用当前已安装版本追加了一次仅 MSGC 的
10 秒 macOS `sample`。运行参数为 100K、c100、512 MiB persistent、P10、16 GiB soft limit、
`HeapGrowth=3`、`ScavengeGrowth=4`，benchmark 总运行约 15 秒，采样延迟 2 秒、持续 10 秒。
结果位于
`result/20260818_512M_c100_100k_HeapGrow3_Scavenge4_compare_2/msgc`；采样完成，benchmark
成功 41,088、失败 0。受 `/usr/bin/sample` 明显扰动，本轮 2548 req/s 只用于确认采样覆盖了
有效业务阶段，不能与未采样的 60 秒正式吞吐作性能比较。

下表按 `sample` 的 top-of-stack 统计，剔除主要内核等待叶子后计算近似活跃样本占比。旧、新
采样并非严格 A/B，因此只用于观察热点迁移，不用于计算优化收益：

| 叶子热点 | flat storage 前 | flat storage 后 | 解释 |
| --- | ---: | ---: | --- |
| `HashMap` 相关 | 6.30% | 2.10% | small object 已显著减少 hash/bucket 工作 |
| `String.equals` | 0.51% | 4.20% | 成本转移到 flat storage 的线性 key 比较 |
| `parseJsonObjectKey` | 未单独显著 | 10.89% | 当前最大的 JSON 解析叶子热点 |
| `JsonObject.putField` | 未单独显著 | 4.75% | 包含 small-object 顺序查找与插入 |
| `ArrayList` | 1.43% | 1.58% | `JsonArray` 仍为每个数组创建包装容器 |
| `__bzero` | 3.48% | 4.06% | 数组、输出 buffer 和 large-object 清零仍明显 |

这证明 small-object 方向有效，但剩余成本已经从 HashMap 转移到 key 快速判等、`JsonArray`
容器和大数组清零。下一轮建议按以下顺序实施。

##### P0-1：flat JsonObject key 判等增加长度和首尾字节过滤

当前 `putField()` 和 `getFlatField()` 对每个候选条目直接执行完整 `String ==`。对 100K payload
的 1,364 个 object 统计，每次完整解析会产生约 15,600 次插入候选比较：

- 先比较 key 长度，只剩 2,641 次需要继续判断，占 16.93%；
- 再比较首字节，只剩 425 次，占 2.72%；
- 当前 payload 的不同 key 再比较尾字节后均可排除；真正重复 key 仍会进入完整 `String ==`。

建议增加统一的内部 `keyMayEqual()`：长度不同直接返回 false；空 key 单独处理；再比较首、尾
字节；全部相同才调用 `String ==`。该 helper 同时用于插入和查询，不引入 hash 计算，不改变
重复 key 的 last-write-wins 语义，也不会改变公开 API。它直接对应本轮新增的
`String.equals`/`putField` 热点，改动小、风险低，应作为下一项验证。

###### 2026-08-18：与 object/array delimiter 状态机合并实施后的结果

该过滤已经与 object/array delimiter 状态机重写一起实施。新状态机先单独处理空容器，解析
一个成员/元素后只读取一次 delimiter，再按结束符或逗号分支；功能回归覆盖重复 key、空 key、
同首尾不同中间字节、空/嵌套容器以及缺失或多余逗号。

同配置 60 秒 MSGC-only 结果为 3427 req/s、206118 成功、0 失败，后 30 秒平均
3462.40 req/s。对照 small JsonArray flat storage 的最近结果 3435 req/s、后 30 秒
3472.51 req/s，分别为 -0.23% 和 -0.29%，落在波动范围内，不能认为有吞吐收益。该结果说明
长度/首尾过滤虽减少了进入完整 `String.equals` 的次数，但每个 key 本来很短，新增的
`rawData()`、首尾读取和分支也有成本；delimiter 改写减少的判断量同样太小，无法改变端到端
吞吐。后续不再继续叠加类似的短 key 微分支，应把 key hash/ID 融入同一次扫描，或直接减少
DOM 节点与 backing array 数量。

##### P0-2：小 JsonArray 使用 flat storage，再按需提升为 ArrayList

当前 payload 有 851 个 array，其中 850 个长度不超过 5，只有 1 个长度为 85；服务端 mutation
追加元素后，常用小数组仍不超过 8。现有 `JsonArray` 无论大小都会创建一个
`ArrayList<JsonValue>` 和 backing array。

建议以 8 为阈值，small mode 直接持有 `Array<JsonValue> + flatSize`，第 9 个元素时提升为
`ArrayList`。公开 `getItems()` 被调用时也提升并永久保留该实例，以维持调用方可继续修改返回
列表的现有语义；compact/formatted writer、容量估算和 DataModel 转换必须直接分支读取内部
storage，避免只读操作触发提升。

服务端和客户端每个请求各解析一次完整 DOM，因此理论上每请求可减少约 1,702 个
`ArrayList` 包装对象；按最近正式结果 3379 req/s 估算，约减少 575 万个包装对象/秒。该项既
对应 `ArrayList.add` 热点，也能继续降低 GC 元数据和扫描压力。

###### 2026-08-18 实施与 MSGC 单次验证

该项已按上述设计实现。`JsonArray` 的 small mode 使用容量为 8 的 flat array 和逻辑长度；
第 9 个元素、公开 `getItems()` 以及需要保留外部 `ArrayList` 别名语义的构造路径会提升为
`ArrayList`。compact/formatted writer、容量估算和 DataModel 转换直接读取内部 storage，
只读序列化不会触发提升。新增用例覆盖 8 元素边界、第 9 个元素提升、索引异常、两种序列化、
`ArrayList` 构造别名以及 `getItems()` 返回列表的可变别名语义；原有 JsonObject flat storage、
escape 分类和 owned UTF-8 parser 用例也重新编译执行通过。

本次只重编并安装 stdx，没有修改或替换 compiler、runtime 和 stdlib。验证组合为：

- compiler：`cangjie_compiler_region_mark/output/msgc/bin/cjc`；
- runtime：同一 MSGC SDK 的 `libcangjie-runtime.dylib`，MD5
  `d41e9d86deb12474dbb6a46b37c103a9`，安装 stdx 前后未变化；
- stdlib：同一 MSGC SDK 的既有 stdlib；
- stdx：`cangjie_stdx_region_mark` 本次构建并安装的产物，JSON dylib SHA-256 从
  `ea3ec025078efc265867174d5b0d804646b1e1b7e22a6f2abcab5bc621760a47` 变为
  `0e55ec794a453d29f48465f82f8ab8a20d9b0d899581878f47707bb4f2e2cf14`。

60 秒验证仅运行 MSGC，参数与最近基准一致：100K payload、c100、512 MiB persistent、P10、
20 GB heap、`HeapGrowth=3`、`ScavengeGrowth=4`、`MinGCThreshold=20mb`。结果目录为
`result/20260818_512M_c100_100k_HeapGrow3_Scavenge4_compare_3/msgc`，对照为同日同配置的
`compare_1/msgc`：

| 指标 | 最近同配置结果 | small JsonArray flat storage | 变化 |
| --- | ---: | ---: | ---: |
| 总吞吐 | 3379 req/s | 3435 req/s | +1.66% |
| 后 30 秒平均吞吐 | 3444.95 req/s | 3472.51 req/s | +0.80% |
| 成功/失败 | 203225 / 0 | 206601 / 0 | 失败数不变 |
| RSS 平均值 | 3771.81 MiB | 3704.69 MiB | -1.78% |
| RSS 峰值 | 3884.02 MiB | 3820.19 MiB | -1.64% |
| CPU 平均值 | 625.53% | 638.22% | +2.03% |
| 每成功请求 CPU | 1.8653 ms | 1.8700 ms | +0.25% |
| GC 次数 | 189 | 192 | +1.59% |
| GC 总时间 | 5247.52 ms | 5135.31 ms | -2.14% |
| GC 平均时间 | 27.76 ms | 26.75 ms | -3.64% |
| GC P95 / 最大值 | 37.26 / 61.80 ms | 37.16 / 68.61 ms | P95 基本不变 |

单次结果呈现小幅正向趋势：总吞吐提高 1.66%，稳态后 30 秒提高 0.80%，平均和峰值 RSS
分别降低约 1.8% 和 1.6%，符合减少小数组包装对象的预期；但每请求 CPU 基本持平，GC 次数
略增，最大 GC 停顿也没有改善。该幅度接近当前环境约 1.5% 的样本标准差和约 3% 的历史区间，
因此只能判定“功能正确、内存方向有效、吞吐有正向趋势”，不能据此宣称稳定吞吐收益。
按本任务要求未运行 Java、Go、CopyGC，也未做回退重编或完整 A/B。

##### P1：large object 清零候选的优先级复核

源码与新采样继续支持后文“只清零实际对象范围”的候选：`__bzero` 约占活跃叶子样本 4.06%，
正式运行每轮 large sweep 回收约 759--761 MiB span。具体 `regionBytes`/`allocSize` 差异、内存
安全约束和验证要求沿用后文 P1 章节，不在这里重复。该项属于 Runtime 内存安全路径，仍排在
两个 stdx P0 改动之后。

##### P1/P2：解析和 writer 的剩余候选

- `parseJsonObjectKey` 约占活跃叶子样本 10.89%，但 key 平均只有约 5.5 字节；直接为每个 key
  调用 native scanner 可能被调用开销抵消。应先完成 key 判等过滤，再评估短 ASCII key 的
  Cangjie 内联分块扫描或受限 parser-local symbol table。
- 数字解析的 `tryParseJsonDecimalNumber`、`parseJsonNumber`、`s2d_n` 等仍是明显热点。后续可
  评估把 token 语法扫描、整数/浮点分类和转换合并为一个 native fast path，但必须保持溢出、
  指数、负零和非法格式行为。
- compact `WriteBuffer` 仍可尝试避免 BLANK 预填，但本轮 `_platform_memset` 叶子占比不足 0.5%，
  低于 key、array 和 large-object 路径，不再列为最先实施项。

本轮同时重新核对了响应输出链路：通用 `toString()/toJsonString()` 的
`String.fromUtf8Unchecked()` 会 clone 输入数组，但 case26 实际调用的 `toStringWithCapacity()`
已经使用 `String.withRawData()` 接管 `WriteBuffer` backing array；`HttpRawBody(String)` 随后只取
该 String 的 `rawData()` view。因此针对本 benchmark 再增加直接 bytes API 不会消除新的
100 KB 整包复制，不应列为 P0。若优化通用 JSON API，是否消除其他 `toString` 路径的 clone
应作为独立兼容性任务评估，不能记入 case26 收益。

### P2：有条件借鉴字段名 canonicalization，不直接照搬 Jackson

6,055 个 key 只有 87 个唯一值，表面上适合 symbol table；但当前仓颉 key 已经是共享输入数组的
16 B String view，`HashMap` 插入后也保存已计算 hash。简单增加一个
`HashMap<String, String>` 做 intern，可能新增一次 hash/lookup 和 symbol table 生命周期成本，
却没有减少 Java 那类独立 String/backing-array 分配。

只有在新采样仍显示 `String.hashCode/wyhash` 和 key equality 为显著 self hotspot 时，才评估
bounded parser-local symbol table 或“raw span + 预计算 hash”的 JSON 专用 key。必须限制表大小、
处理 hash collision，并分别测重复 key 与高基数/恶意 key 输入；当前不把 key intern 列为 P0。

### P2：buffer recycling 与直接 bytes 输出的适用边界

Jackson 默认启用线程本地 buffer recycling，但当前仓颉序列化结果会由
`String.withRawData` 接管，并由 `HttpRawBody` 直接发送；最终约 100 KB output backing array
在 response 生命周期结束前不能安全放回 pool。直接增加 `toBytes` 也不会消除现有整包复制，反而
可能重新引入一次 String/Array 转换。

若要复用最终输出 buffer，需要 stdx JSON 与 HTTP send completion 之间建立明确的 lease/return
协议，或者改成 16--32 KB staging buffer 的流式输出；后者还会改变 Content-Length、chunking 和
write 次数，必须单独验证 HTTP 吞吐与公平性。在完成 integrated UTF-8 validation、escape 状态和
small-object storage 前，不优先进行这项跨模块改造。

### 当前不应重复的方向

- 不恢复每个短字符串一次 native scan/copy；历史单次验证回退约 16.1%；
- 不恢复为每个 `JsonString` 保存完整 raw span/多个状态字段；历史验证出现明显吞吐回退；
- 不恢复旧 16-byte vector scanner，也不重复手工展开全部 writer capacity 检查；
- 不再优化 compact writer subtype dispatch，当前已经使用 `writeCompactTo` 虚分派；
- 不把客户端只扫描 `server_ok` 作为 JSON 库收益，该做法会让 Java/仓颉工作量失去等价性；
- 不共享可变 `JsonInt/JsonFloat/JsonString` 节点。Java 的小整数/boolean singleton 依赖节点不可变，
  当前仓颉标量已提供 setter，直接共享会产生别名语义错误；null/boolean parser 单例已经实现。

### P1：大对象仅清零实际对象范围

`RegionSpaceMS::TryAllocateLargeObjectOnce` 根据 `allocSize` 计算 Region span 后，当前使用
`memset(regionStart, 0, regionBytes)` 清零整个按 32 KB 向上取整的 span。约 100 KB 的
对象通常占用 128 KB span，因此一次分配可能额外清零约 16--28 KB 的不可见尾部 padding。
flat storage 后的新采样中 `__bzero` 约占剔除主要等待后的活跃叶子样本 4.06%；对应正式运行
每轮 large sweep 仍会回收约 759--761 MiB span，因此该项仍有明确的数据路径支撑。

可以评估只清零 `allocSize`，但必须先证明以下约束：

1. `allocSize` 覆盖完整对象 header、Array 元数据和全部可访问元素；
2. GC 扫描与 large-object size 计算不会读取对象尾部之外的数据；
3. hot-free span 复用、sanitizer 和越界诊断不依赖尾部被清零；
4. 补充跨 size/span 边界的数组零初始化与复用回归测试。

该项比立即增加 16--128 KB 中型对象分配器更小、更容易独立验证，并能直接减少 case26
高频大对象的无效内存写。

### P1：区分 compact 与 formatted WriteBuffer

`WriteBuffer` 当前会把整个初始数组和扩容数组预填为 BLANK。这个设计是为了让 formatted
serializer 的 `appendBlank` 只移动指针，但 case26 使用 compact serializer，不会调用
`appendBlank`，仍然为约 100 KB 输出 buffer 支付整块空格填充成本。

可评估：

1. compact writer 使用不需要 BLANK 预填的 backing array；
2. formatted writer 在 `appendBlank` 时显式写入空格；
3. 检查多个容量判断中的 `>=`，在 native writer 不需要尾部 sentinel 的前提下改为 `>`，
   避免刚好可容纳时发生 1.5 倍扩容；
4. 增加初始容量、最终长度和扩容次数的短时统计，再校准
   `requestBytes + requestBytes / 8 + 1024`。

不建议为了缩小 backing array 在序列化结束后再复制成精确长度数组，这会重新引入一次
完整大对象复制。

### P1：补齐 Mark helper 的周期内聚合观测

当前控制器以普通初始 helper 上限 2 启动，但会每 4 个 main slice 根据 scheduler backlog
和 elapsed pressure 调整 helper，并在 20 ms/100 ms 后分别进入 HIGH/CRITICAL 压力。
因此 `Mark control begin: helpers=2` 只能证明初始目标，不能证明整轮一直保持 2。

建议增加每轮一次的聚合日志：

- helper target 的 min/max/时间加权平均；
- 第一次扩容发生时间与扩容次数；
- 各 helper 宽度下的 backlog 变化；
- assist WU 占比和 assist multiplier 的 min/max。

只有在确认“backlog 持续、assist 比例高、helper 仍未及时扩容”后，才应把 assist share、
assist debt 增速或分配速率加入控制器输入。本轮 MSGC CPU 已约 990%，直接提高 helper
上限可能只会从 mutator 抢占 CPU；最终指标应是吞吐和每成功请求 CPU，而不是单纯降低
assist 占比。

### P1：重新校准 PreEnum 与 sweep 建议

当前 `PrepareMarkEpochUnderSTW` 已经按 size-class/scan-class lane 拆分 bitmap reset，并根据
预计 reset bytes 借用 GC worker pool 并行执行。因此“提高 reset 并行度”已不再是缺失
能力。后续应先使用现有子阶段日志区分：

- `Sweep force finish`：等待和强制完成旧 sweep backlog；
- `PrepareMarkEpoch STW`：collection set 迁移、bitmap reset、large membership freeze；
- `PreEnumRoots total`：上述工作与 STW 获取的总和。

可以在 `PrepareMarkEpoch STW` 的周期级日志中补充 resetBytes、laneCount 和 helperCount，
再判断瓶颈是 sweep 尾部还是 bitmap reset。lazy bitmap epoch 会改变 mark/sweep 之间的读取
协议，在没有拆分耗时证据前不应优先实现。

当前 sweep 也已经按压力动态调整：MEDIUM 使用基础时间片的 4 倍，HIGH 使用 10 倍，并
动态增加 parallel sweep helper。若基础值从 2 ms 改为 4/8 ms，实际高压力时间片会变成
40/80 ms，而不是 4/8 ms。在 P10 已基本满载的条件下，这可能长时间挤压业务线程。

因此 sweep A/B 应先记录 pressure tier、实际 slice budget、helper 数、mutator sweep assist
时间和每请求 CPU；不再把简单调大 `cjGCBGSweepMaxTime` 作为默认优化。

### P2：降低 large-object 元数据争用

每个 large object 注册时都会获取同一个 `largeLock_`，再向 `largeNextSupply_` 追加入口。
结合本文每轮约 5,985 个回收对象和约 24,683 个 span，该锁以及 large membership vector
可能成为 c100 下的次级争用点。

只有在 10 秒 perf/锁采样确认 `AddLargeLiveRegion`、`TakeRegionForLarge` 或 large sweep
元数据处理占比显著后，再考虑：

1. 按 P/home shard 暂存 large supply；
2. PreEnum STW 时冻结并合并各 shard；
3. 对 dead large span 的 metadata reset 和 FreeManager reclaim 做批处理。

不要直接恢复无界的 per-P large span cache；它可能以更高 RSS 和碎片换取局部命中。新增
16--128 KB 中型对象分配路径也应放在上述低风险优化之后，并先取得对象大小直方图。

### P2：参数实验定位边界

完成脚本透传后，可以进行以下诊断实验，但不应直接作为默认配置：

1. `CJ_MS_DISABLE_ASSIST=1`：判断 mark assist 对业务路径的影响；
2. `CJ_MS_STW_SWEEP=1`：判断并发 sweep 是否仍制造状态问题，不用于吞吐调优；
3. `cjMSParallelSweepMaxHelpers=2/4`：结合实际 helper 与每请求 CPU 判断扩容收益；
4. `cjMSAdaptiveGrowth=1`：比较吞吐、RSS 和 GC 周期稳定性；
5. `cjHeapGrowth=5/10`：仅在允许更高 RSS 时评估吞吐上限。

`cjGCBGSweepMaxTime` 必须结合 4 倍/10 倍压力乘数设计实验值，不再直接使用原文的
4 ms/8 ms 方案。

## 8. 后续验证标准

每项优化默认使用固定 SDK、相同 payload、512 MiB persistent、c100、P10、20 GB heap、
60 s 条件执行一次 MSGC-only 验证，并与最近同配置基准比较；不启动 CopyGC、Go、Java，也不
默认做回退重编或完整 A/B。若结果幅度落入已知波动区间，只记录为候选趋势；只有用户明确
要求时再做多轮或交错 A/B。每次同时记录：

- 成功吞吐与失败数；
- 累计 CPU 时间和每成功请求 CPU；
- RSS 平均值与峰值；
- JSON server/client parse、mutate、serialize 分项耗时；
- GC phase 时间、`assistWU` 占比、helper 目标的周期内 min/max/时间加权平均；
- `unsweptRegions`、large sweep 回收量和碎片率；
- sweep pressure tier、实际 slice budget、sweep helper 数和 mutator sweep assist；
- large-object 分配次数、对象字节、span 字节及两者差值；
- HTTP/JSON 语义校验结果。

DOM 标量原地更新、owned-byte parser 接管输入、owned parser 融合 UTF-8 校验、无对象膨胀的
key escape 分类、small JsonObject/JsonArray flat storage、flat key 长度/首尾过滤和 delimiter
状态机改写均已完成。最后一项单次结果为 -0.23%，说明继续叠加短 key/分隔符微分支的空间很小。
结合第 9 节的业界实现调研，当前建议先验证 parser slab arena，再评估分段 JSON 输出与 HTTP
发送；之后才是 compact document、整包 NEON structural scan 和有界 key name table。Runtime
large object 只清零实际分配范围仍是独立的 P1 候选。完成这些结构性优化前，不应把单纯调大堆、调高
`cjScavengeGrowth`、调大 sweep 时间片或关闭 assist 作为最终优化结论；这些只应作为定位和
权衡实验。

## 9. 业界 JSON 实现调研与可落地方案（2026-08-18）

### 9.1 调研范围与可比性边界

本节只采用项目官方文档、官方仓库或论文描述实现机制，不直接引用各项目自报 benchmark 数字
推导 case26 收益。不同语言的对象模型、allocator、GC、输入数据和 API 语义差异很大，行业
benchmark 只能用于发现设计方向，最终优先级仍以本 case 的对象统计、热点和 60 秒端到端结果
为准。

当前 Java 用例确实使用 Jackson 2.18.0：`jackson-core`、`jackson-databind` 和
`jackson-annotations` 均为 2.18.0；服务端复用一个静态 `ObjectMapper`，通过
`readTree(byte[])` 构造 `JsonNode` 树，修改 `ObjectNode/ArrayNode` 后调用
`writeValueAsBytes()`，客户端再次 `readTree(byte[])`。因此它是 Jackson tree model 对比，
不是 POJO codegen 或 streaming benchmark。Jackson 官方也说明 `ObjectMapper` 的 tree/data
binding 最终建立在 `JsonParser/JsonGenerator` 上，并建议复用 mapper；`JsonFactory` 则持有
byte/char 字段名 symbol table 和可回收 `BufferRecycler`。参见
[ObjectMapper 2.18 API](https://javadoc.io/static/com.fasterxml.jackson.core/jackson-databind/2.18.4/com/fasterxml/jackson/databind/ObjectMapper.html)、
[JsonFactory 2.18 API](https://javadoc.io/static/com.fasterxml.jackson.core/jackson-core/2.18.0/com/fasterxml/jackson/core/JsonFactory.html) 和
[JsonFactory.Feature](https://www.javadoc.io/static/com.fasterxml.jackson.core/jackson-core/2.18.5/com/fasterxml/jackson/core/JsonFactory.Feature.html)。
截至 2026-08-18，Jackson 官方 2.18 release 页面已列出 2.18.9，而本用例仍固定为 2.18.0；
这不影响本节对 2.18 架构的比较，但后续若升级 Java 基线必须显式记录版本并重建基准，不能
把 patch 升级与仓颉优化混在一次结果中。

| 实现 | 关键设计 | 对当前 stdx 的有效启发 | 不应直接照搬的部分 |
| --- | --- | --- | --- |
| Jackson 2.18 | streaming core；复用 mapper/factory；byte-quads 字段名 canonicalization；buffer recycler；受限输入长度 | 有界 parser context、扫描时生成 key hash/ID、复用纯临时 scratch | Java `String`/`HashMap` 成本与仓颉的输入切片 String 不同，不能把全局 intern 当成必然收益 |
| simdjson | 第一阶段 SIMD 分类结构字符，第二阶段消费索引；On-Demand 在原始输入上按访问顺序解析 | 一次 native 调用完成整包 AArch64 NEON structural scan；为只读/局部读取增加 lazy view | 当前可变 DOM API 不能直接替换成一次性 iterator；额外预扫描只有足够宽的 SIMD 收益才能抵消 |
| yyjson | immutable value/string 使用文档级连续存储；mutable document 单独表示；document 管理统一生命周期 | parser arena、compact immutable document、按需转 mutable | 当前 `JsonValue` 节点可原地 mutation，不能无条件改成 immutable 16-byte C struct 语义 |
| RapidJSON | DOM 默认 `MemoryPoolAllocator` 顺序分配；in-situ string；支持调用方 scratch；NEON whitespace scan | 用 slab/arena 消除每容器独立 backing allocation，保留 owned input | C++ 手工生命周期和 Cangjie GC 不同，arena 必须让 GC 正确扫描引用并维持 input 生命周期 |
| fastjson2 | UTF-8/ASCII 专用 reader；Vector API；字段 hash switch 与 symbol table；ASM 生成 typed reader/writer；JSONPath partial parse | schema 已知时生成直接 reader/writer；字段匹配在一次 key 扫描内完成；按路径跳过无关值 | 动态字节码生成不适合直接移植，优先考虑编译期宏/代码生成 |
| Sonic / serde_json | lazy/partial tree、SIMD scan；`RawValue` 可借用并原样转发未解析子树 | 为局部变换提供 raw subtree/span 和 streaming copy-through API | 原样转发会保留输入格式，不能静默替代承诺 compact 输出的现有 API |

对应资料：

- [Jackson core streaming 与输入限制](https://github.com/FasterXML/jackson-core)、
  [Jackson 2.18 数字解析优化](https://github.com/FasterXML/jackson/wiki/Jackson-Release-2.18)；
- [simdjson On-Demand 基础](https://github.com/simdjson/simdjson/blob/master/doc/basics.md)、
  [simdjson 论文](https://arxiv.org/abs/1902.08318)；
- [yyjson 数据结构](https://ibireme.github.io/yyjson/doc/doxygen/html/data-structures.html)；
- [RapidJSON DOM 与 allocator](https://rapidjson.org/md_doc_dom.html)、
  [RapidJSON SIMD/allocator internals](https://rapidjson.org/md_doc_internals.html)；
- [fastjson2 性能设计](https://github.com/alibaba/fastjson2/blob/main/docs/performance_en.md)、
  [fastjson2 架构](https://github.com/alibaba/fastjson2/blob/main/docs/ARCHITECTURE.md)；
- [Sonic C++](https://github.com/bytedance/sonic-cpp)、
  [serde_json RawValue](https://docs.rs/serde_json/latest/serde_json/value/struct.RawValue.html)。

Sonic C++ 当前官方要求是 x86/AVX2/Linux，因此这里只借鉴 compact tree、on-demand 和整块 SIMD
扫描思路，不能把其实现直接移植到本项目的 macOS AArch64；本项目需要独立的 NEON 路径。

### 9.2 当前 stdx 已具备和真正缺失的能力

当前 DOM parser 已经直接接管 `Array<Byte>`，普通 String/key 是输入数组上的 view，并已把
UTF-8 校验、escape 分类和数字 token 扫描融入解析；writer 也已经对安全 key/string 直接复制，
case26 的 `toStringWithCapacity()` 通过 `String.withRawData()` 接管最终 backing array。这意味着
“从 String 改成 byte[]”“普通字符串零拷贝”“再删一次 UTF-8 整包扫描”等行业常见建议在这里
已经完成，不能重复计算收益。

真正的结构性差距有四项：

1. 每个 `JsonObject/JsonArray` 即使进入 flat mode，仍各自分配一个 8 槽 backing array；
2. 6,304 个 scalar 加上 2,215 个 container 仍各自是 GC 对象，没有 document-level compact
   node storage；
3. compact writer 必须形成一块约 100 KB 的连续 Array 后，HTTP 才能走 `HttpRawBody` 快路径；
4. DOM parser 没有可配置/可复用 factory、字段名表或 schema-generated reader/writer。

现有 stream 包已经提供 `JsonReader.copyValueTo()`、`copyNameTo()`、`nextNameEquals()` 和
4 KB `JsonWriter` buffer，说明 raw copy-through 的底层能力并非空白；缺少的是路径匹配/变换层，
以及 JSON writer 到 HTTP send completion 之间可表达多段 body 生命周期的接口。

### 9.3 建议优先级

| 优先级 | 方案 | API 影响 | 主要命中成本 | 对 case26 的判断 |
| --- | --- | --- | --- | --- |
| P0-1 | parser slab arena：批量承载 small object/array 的 8 槽 backing storage | 无公开 API 变化 | 大量小 Array 分配、对象头、allocator/GC 元数据 | 最适合下一项验证；保持现有 DOM 与 mutation 语义 |
| P0-2 | segmented compact writer + HTTP segmented body/完成后归还 lease | 新增输出/body API | 每响应约 100 KB contiguous large object、扩容/清零、large sweep | 上限高，但跨 JSON/HTTP，必须控制 write 次数和生命周期 |
| P1-1 | immutable compact `JsonDocument`/node tape，按需 materialize 或 COW | 新 API | 每次解析 6,304 scalar 和 2,215 container 对象 | 架构收益上限最高，改造和兼容成本也最高 |
| P1-2 | 一次整包 AArch64 NEON structural scan + stage-2 parser | DOM API 不变 | key/string/whitespace/delimiter 标量扫描 | 只允许“一文档一次 native 调用”；需先做 parser microbenchmark |
| P1-3 | 有界 `JsonNameTable`，key 扫描同时计算 hash/ID | 可选 parser/factory API | 6,055 key、87 unique、重复 hash/equality | 比单独首尾过滤更合理，但先证明 table lookup 不抵消收益 |
| P1-4 | 编译期生成 typed reader/writer 和字段 hash switch | 新 typed API/宏 | DOM、反射式分派和中间节点 | 对 schema workload 很强；当前动态 DOM case 不能直接记为等价收益 |
| P2 | path transformer/raw subtree copy-through | 新 streaming API | 只改少数字段却解析/重写整棵树 | 适合生产变换；不应混入 Jackson tree model 的同口径对比 |
| P2 | parser context、scratch recycler、read/write constraints | 可选 factory API | 临时 buffer 分配与异常输入资源上限 | 稳定性和通用 workload 有价值，case26 直接收益较小 |

这里把 parser slab arena 排在 key canonicalization 之前，是因为它不需要猜测 key 分布，也不
新增 hash/table 查找，并且直接命中当前 flat storage 留下的分配形态。把 segmented writer 排在
compact document 之前，是因为它能先独立验证 MSGC large-object 路径，而不需要重写整个 DOM。

### 9.4 P0-1：parser slab arena 的最小落地设计

当前 parser 每次构造 1,364 个 `JsonObject` 和 851 个 `JsonArray`，所有容器先各分配一个 8 槽
Array；服务端和客户端各解析一次，所以理论上每请求产生约 4,430 个独立 container backing
array。按最近 3427 req/s 估算，约为 1,518 万个独立 backing array/s。small flat storage
减少了 `HashMap/ArrayList` 包装和 bucket，但没有消除这些 Array 的对象头、分配调用和 GC
元数据。

建议增加仅供 DOM parser 使用的 `ParsedFlatStorageArena`：

1. 分别维护 `(String, JsonValue)` 和 `JsonValue` 的分段 slab；
2. 每个 small object/array 从 slab 获取一个互不重叠的 8 槽 `Array.slice`，通过 internal
   constructor 注入，公开 constructor 保持不变；
3. slab 大小按实际元素布局校准并保持在 MSGC large-object 阈值以下，避免把很多小分配换成
   新的 large object；
4. 第 9 个成员/元素以及公开 `getFields()/getItems()` 逃逸时，沿用现有 promotion 语义，复制
   到独立 `HashMap/ArrayList`；未逃逸容器继续直接读写自己的独占 slice；
5. parser 完成后 arena owner 必须随 DOM 一起存活，所有引用槽必须按 GC 可扫描的零值初始化，
   不能使用 runtime 不认识的裸 native 指针承载 `JsonValue`。

第一版不尝试可变长度精确切片，也不把 scalar 一起塞入 arena。这样总槽位字节数大体不变，
但独立 Array 数量大幅下降，改动可以限制在 `native.cj` 和 `json_object.cj`。如果该版本只能降
RSS/GC 元数据而吞吐仍无变化，再决定是否升级为 node tape，而不是继续给 flat storage 增加
更多字段和分支。

验证除现有 JSON 回归外，应增加：0/1/8/9 边界、重复 key last-write-wins、嵌套容器、公开
map/list alias、promotion 后兄弟 slice 不受影响，以及长生命周期 DOM/input 保活。短时诊断需
记录每次 parse 的 object/array slab 数、独立 backing array 数、promotion 数和 slab 利用率；
60 秒仍只跑 MSGC 一次，并与当前 3427 req/s、后 30 秒 3462.40 req/s 比较。

#### 2026-08-18：实现与 MSGC-only 验证结果

本项已经按上述最小设计落地，只修改 stdx DOM parser 内部：

- `ParsedFlatStorageArena` 分别维护 object field slab 和 array item slab；每个 slab 承载 32 个
  small container，即 256 个槽位；
- parser 为每个新 `JsonObject/JsonArray` 取得一个互不重叠的 8 槽 `Array.slice`，通过 internal
  constructor 注入；公开 constructor 仍使用独立 storage；
- slice 持有 backing storage，arena 切换到下一 slab 后旧 DOM 仍能保持原 slab 存活；parser
  reset 不复用可能仍被返回 DOM 引用的旧槽位；
- object 第 9 个 field 仍提升为 `HashMap`，array 第 9 个 item 仍提升为 `ArrayList`，没有改变
  现有 mutation、公开 mutable view 和 duplicate-key last-write-wins 语义。

从 case26 的容器计数估算，单次 parse 的独立 backing storage 从 2,215 个下降为最多
`ceil(1364 / 32) + ceil(851 / 32) = 70` 个 slab；服务端和客户端合计从约 4,430 个下降为约
140 个，理论降幅 96.84%。这里减少的是独立 backing storage，不等价于消除 `JsonObject`、
`JsonArray` 或 slice descriptor 本身。

安装前后 runtime MD5 均为 `d41e9d86deb12474dbb6a46b37c103a9`；新安装的
`libstdx.encoding.json.dylib` SHA-256 为
`c87a3f41f0b3de9f046d1fea2e5bce6295fb65ff39f245dab42abd91e2451b72`。compiler、runtime、
stdlib 均沿用 `cangjie_compiler_region_mark/output/msgc`，只重编并安装 stdx。重新编译后的
聚焦用例确认实际加载该 SDK 下的 stdx JSON 和 MSGC runtime；跨 32-container slab 边界、
兄弟 slice 隔离、8/9 promotion、flat object/array 与 delimiter 状态机回归均通过。

60 秒验证仍使用 `100K / c100 / 512 MiB persistent / P10 / HeapGrowth=3 /
ScavengeGrowth=4 / cjMSMinGCThreshold=20mb`，只执行 MSGC 一次。`HEAP_SIZE=20gb` 与最近结果
保持一致，runtime 同样报告超出参数上限并采用 16 GiB soft limit，因此不影响本次同配置比较。

| 指标 | 最近同配置结果 `compare_5` | slab arena `compare_6` | 变化 |
| --- | ---: | ---: | ---: |
| 总成功请求 | 206,118 | 214,838 | +4.23% |
| 失败请求 | 0 | 0 | 0 |
| 整体吞吐 | 3427 req/s | 3572 req/s | +4.23% |
| 后 30 秒平均吞吐 | 3462.40 req/s | 3596.19 req/s | +3.86% |
| CPU 时间/成功请求 | 1.8746 ms | 1.8556 ms | -1.01% |
| 平均 RSS | 3509.72 MiB | 3596.33 MiB | +2.47% |
| 峰值 RSS | 3774.09 MiB | 3862.75 MiB | +2.35% |
| GC cycle | 196 | 205 | +4.59% |
| GC total | 6285.13 ms | 5561.58 ms | -11.51% |
| GC 平均/p95 | 32.07 / 45.43 ms | 27.13 / 38.37 ms | -15.40% / -15.53% |

新结果位于
`result/20260818_512M_c100_100k_HeapGrow3_Scavenge4_compare_6/msgc`。这一单次结果同时表现为
整体和稳态吞吐约 4% 正向、每请求 CPU 小幅下降、GC 累计时间与平均/p95 明显下降，方向与减少
backing allocation/GC 元数据的预期一致；但平均和峰值 RSS 没有下降，且 GC cycle 随完成请求数
增加。由于按约定没有做完整 A/B 或多轮统计，应把 4% 视为本机单次正向验证结果，而不是稳定
置信区间。后续若继续优化，优先进入 segmented writer/HTTP body 或用新的 10 秒 profiler 重新
选 compact document 与 structural scan 分支，不再继续扩大 slab 复杂度。

### 9.5 P0-2：分段 compact 输出与 HTTP 发送

当前 `toStringWithCapacity()` 已避免最终 clone，但它仍必须分配并清零一块约 100 KB 连续
Array。这个对象超过 MSGC small-object 路径，且响应每次都重新生成；现有 `JsonWriter` 虽然
使用 4 KB buffer 写 `OutputStream`，HTTP/1.1 对已知 `Content-Length` 的最佳路径却只专门处理
连续 `HttpRawBody`，直接套现有 stream writer 可能增加 4 KB 中转复制和系统调用。

建议作为一个完整链路设计，而不是只改 `WriteBuffer`：

1. `SegmentedWriteBuffer` 使用若干低于 large-object 阈值的 8--16 KB segment，append/escape/
   number writer 直接跨 segment 写入；
2. 新增只读 `JsonUtf8Segments(totalSize, segments)`，现有 `toString()`/`toStringWithCapacity()`
   保持兼容；
3. HTTP 增加 `HttpSegmentedBody`，已知总长度时设置 `Content-Length` 并直接发送各 slice；底层若
   支持 gather write，则合并多个 segment，避免每 8 KB 一次 syscall；
4. 只有 send completion 后才能把 segment 归还有界 pool。没有明确 lease/return 时不复用，
   防止响应仍在发送时被下一请求覆盖；
5. compact segment 不用 BLANK 预填，formatted writer 单独维护空格写入语义。

这项同时减少 output large-object 分配、1.5 倍扩容和 large sweep 压力，但结果容易受网络 write
次数影响。验证必须同时记录 output large-object 次数/字节、segment 数、连接 write 次数、每请求
CPU、RSS 和吞吐；若 JSON CPU 降低但 write 次数上升导致端到端回退，应优先补 gather write，
不能只报告 serializer microbenchmark。

### 9.6 P1：更高上限的解析架构

#### Compact immutable document + 按需 materialize

yyjson 的连续 immutable values、simdjson 的 tape 和 Sonic 的 lazy tree 指向同一原则：解析结果
不必天然等于“每个 JSON 值一个语言对象”。建议新增 opt-in `JsonDocument`，不直接改现有
`JsonValue.fromOwnedUtf8Bytes()` 的可变语义：

- document 持有 owned input 和一到数个紧凑 node 数组；node 保存 tag、scalar payload/raw
  span、container child count/end offset；
- object key 与 value 在文档顺序中相邻，array child 顺序连续，遍历不需要 HashMap/ArrayList；
- String/number 默认保留合法 raw span，writer 生成 `{ } [ ] , :`，仅复制安全 token，仍保持
  compact API 的格式语义；
- 只读查询返回轻量 `JsonView`；需要现有可变 API 时显式 `toMutable()`，或只为被修改的
  container 建立 copy-on-write overlay；
- duplicate key、object iteration order、number kind/overflow、输入生命周期和 error offset 必须
  与现有 API 分开写清，避免用实现细节偷偷改变语义。

该方案能同时压缩 6,304 个 scalar 和 2,215 个 container 的对象图，理论上限高于 slab，但
改造面也大。合理路线是先实现只读 `JsonDocument` 与 serializer，再增加局部 mutable overlay；
case26 只修改 root、events 和少数嵌套字段，大量未修改节点仍可保持 compact。若一开始就要求
完全模拟现有每个 mutable node 的引用别名，复杂度会吞掉主要收益。

#### 一次整包 AArch64 NEON structural scan

simdjson 的收益来自“宽 SIMD stage-1 + 紧凑 structural index + stage-2”，不是把每个 5 字节 key
送进一次 native 函数。当前 payload 全 ASCII、无 escape，而 `parseJsonObjectKey` 仍是明显热点，
可以评估一个文档只调用一次的 native scanner：按块分类 quote、backslash、control、JSON
structural 和 whitespace，并同步完成 UTF-8 状态；stage-2 使用 index 推进递归或显式 stack。

该方案必须满足三条门槛：

1. index 使用可复用、分段且受限的 scratch，不能每请求再制造一个约 100 KB GC 大数组；
2. stage-2 仍完整校验字符串、数字、delimiter、深度和 trailing data，不能只在“已知合法 payload”
   上快；
3. 先用 parse-only microbenchmark 证明“stage-1 + stage-2”优于当前融合标量 parser，再跑 10 秒
   profiler 和 60 秒端到端；否则额外整包预扫描可能重演此前删除 UTF-8 prescan 后收益有限的情况。

#### 有界字段名表：hash/ID 必须融入 key scan

Jackson 的 `ByteQuadsCanonicalizer` 和 fastjson2 的 `SymbolTable` 都说明重复字段名可被复用，但
当前仓颉普通 key 已是 16 B 输入 view，简单 `String.intern()` 不会消除独立 backing-array 分配，
还会新增全局同步和 hash。可落地版本应是可选 `JsonParserFactory/JsonNameTable`：

- 在 `parseJsonObjectKey` 原有扫描中同时累计 hash 或 packed short-key fingerprint，不做第二遍；
- table 有固定容量/最大 probe，collision 后比较完整 bytes，超过上限退回普通 String；
- 文档内命中返回第一个 canonical String descriptor，使同名 key 的 backing/start/length 相同；
- 若跨文档复用，canonical key 必须复制到小型独立 name arena，不能引用某个 100 KB request
  input，否则 87 个短 key 可能长期钉住整包输入；
- flat object 可优先比较 key ID，HashMap 路径则需要传递已计算 hash，避免 table 命中后再次
  `wyhash`。

这比已经无收益的长度/首尾过滤多解决了“重复扫描和重复 key 表示”问题，但也增加了 table
lookup。先做独立 key workload：当前 87 unique/6,055 occurrences、高基数 key、重复 key 和
碰撞攻击，再决定是否进入端到端验证。

### 9.7 P1/P2：新 API 场景的两条路线

#### 编译期生成 typed reader/writer

fastjson2 的 ASM reader/writer 和字段 hash switch 对 schema 已知对象很有效。仓颉不必复制 JVM
动态字节码方案；当前 stream 包已经有 `JsonSerializable`、`JsonDeserializable<T>`、
`JsonReader.readValue<T>()` 和 `JsonWriter.writeValue<T>()`，可以在此基础上增加编译期宏或构建期
生成器：一次 key scan 后按 hash/fingerprint switch 直接写字段，serializer 直接写字段名常量和
值，不创建 DOM。

这应作为 stdx 通用能力单独 benchmark。case26 当前要求动态 tree mutation，与 Jackson
`readTree/writeValueAsBytes` 同口径；若把仓颉改为 generated struct，而 Java 仍用 tree model，
所得差异应标记为“业务采用 typed API 的收益”，不能说成现有 DOM 已追平 Jackson。

#### Path transformer 与 raw subtree

现有 `copyValueTo/copyNameTo/nextNameEquals` 可以扩展成 `JsonTransformer`：注册少量 JSON path，
匹配字段时解析并替换，其余合法 subtree 直接流式复制。还可新增类似 serde_json `RawValue` 的
borrowed/owned span，用于延迟解析或原样转发。

该路线非常适合“100 KB 输入只改少数字段”的服务，但必须明确原样 copy 是否保留空白、数字
字面量和 key 顺序；如果输出契约要求 compact，则只能 copy 已证明 compact 的 token/subtree，
或由 transformer 重新生成 delimiter。它是生产优化，不是当前 DOM 库的透明替换。

### 9.8 明确不优先的方案

- 不继续叠加短 key 长度、首尾、多字节手工展开；最近组合验证已是 -0.23%。
- 不为每个短字符串调用一次 native/SIMD helper；只评估整文档一次的 stage-1。
- 不直接使用全局 `String.intern()`；优先有界、抗碰撞且不钉住 request input 的 name table。
- 不在没有 send-completion lease 时池化最终连续 output Array；这会产生并发覆写风险。
- 不为预测精确容量先完整遍历 DOM；第二次全树扫描可能比减少一次扩容更贵。
- 不把 JSONB/CBOR 等二进制格式列为本 case 优化；它们改变协议，不能与当前文本 JSON 对比。
- 不把客户端改成只找 `server_ok` 后跳过其余内容来计算 DOM 提升；这属于 partial/streaming API
  的独立 workload。

### 9.9 建议执行顺序与判定

1. 先实现 parser slab arena，只改 stdx DOM 内部；完成语义回归、分配计数和一次 60 秒
   MSGC-only 验证。若独立 backing array 显著下降但吞吐/CPU无变化，说明瓶颈已转移，不再扩大
   slab 复杂度。
2. 再实现 segmented writer + HTTP body 的最小原型；先验证 Content-Length、短写、异常关闭和
   lease 生命周期，再测 output large-object、write 次数和吞吐。
3. 用更新后的 10 秒 profiler 决定 P1 分支：若 allocator/GC 对象图仍主导，进入 compact
   `JsonDocument`；若 parser 标量扫描重新成为首要 CPU，进入整包 NEON stage-1。
4. name table 和 typed codegen 分别使用重复-key DOM workload、schema workload 独立验证，
   不和 case26 的透明优化混为一个结果。

本轮只完成源码对照、业界调研和方案排序，没有修改 stdx/runtime/LLVM，也没有重新构建或运行
benchmark；后续实施仍按本文第 8 节的 MSGC-only 规则逐项验证。

## 10. 2026-08-18 三项组合优化分阶段验证

### 10.1 阶段一：segmented writer + HTTP body

本阶段在 stdx 增加了 opt-in 的 `JsonValue.toSegmentedBytes()` 与 `JsonByteSegments`，writer 可按
固定大小分段生成 JSON，HTTP response builder 也可接收已知 Content-Length 的多段 byte array。
目标是避免 100 KB 响应序列化时形成一块连续大数组。语义测试覆盖了跨段 JSON、escape、数字、
空容器、flatten 后等价性以及 HTTP body 跨段 read/seek；Release stdx 全量构建 144/144 成功，
测试在 `--gc-mode=marksweep` 下通过。

构建安装来源如下：compiler、runtime、stdlib 均保持
`cangjie_compiler_region_mark/output/msgc` 中的原有产物，只有
`cangjie_stdx_region_mark` 的 Release stdx 重新构建并安装到该 MSGC SDK。动态库探针确认运行时加载
的是该 SDK 内的 `libstdx.encoding.json.dylib`、`libstdx.net.http.dylib` 和
`libcangjie-runtime.dylib`。验证过程中还确认：加载 MSGC runtime 的测试程序必须使用
`--gc-mode=marksweep` 重新编译；缺少该选项导致的启动 SIGSEGV 是验证环境不匹配，不是 segmented
writer 的功能错误。

60 秒 MSGC-only case26 结果位于
`result/20260818_512M_c100_100k_HeapGrow3_Scavenge4_compare_8/msgc`，与最近正式基线
`...compare_6/msgc` 对比如下：

| 指标 | 基线 | segmented HTTP | 变化 |
|---|---:|---:|---:|
| 总吞吐 | 3,572 req/s | 1,407 req/s | -60.61% |
| 后 30 秒平均吞吐 | 3,596.195 req/s | 1,228.315 req/s | -65.84% |
| 成功/失败请求 | 214,838 / 0 | 84,996 / 0 | 功能正确 |
| CPU/request | 1.8556 ms | 2.7740 ms | +49.49% |
| 平均 RSS | 3,596.33 MiB | 2,966.41 MiB | -17.52% |
| 峰值 RSS | 3,862.75 MiB | 3,493.06 MiB | -9.57% |
| GC 次数 | 205 | 97 | -52.68% |
| GC 总耗时 | 5,561.578 ms | 4,761.452 ms | -14.39% |
| GC 平均停顿 | 27.130 ms | 49.087 ms | +80.94% |

结论：它确实降低了连续大数组压力、RSS 和 GC 次数，但当前 HTTP socket API 没有 `writev/sendmsg`
式 gather write。约 100 KB 的响应被拆成 13 个约 8 KB segment，`HttpEngineConn1` 经
`BufferedConn.write()` 对每段执行一次 `socket.write()`；默认 `TCP_NODELAY=true`，因此额外的
managed socket/poll/write 边界直接压垮 CPU 与吞吐。这不是 segment 大小微调能消除的问题。

因此该实现没有通过性能门禁，最终从生产 JSON/HTTP 包回退，不安装 segmented writer/body API；
在 std.net 提供真正 gather write 前，case26 保持一次连续 body。测试、阶段结果和实现计划保留为
实验记录。阶段一是明确的负向验证结果，不把它混入 compact document 或 SIMD 的收益判断。

### 10.2 阶段二：compact JsonDocument/node tape + sparse COW

本阶段新增 opt-in `JsonDocument`/`JsonCursor`：owned input 由 document 持有，节点的 tag、token/key
span、child/sibling link 和 scalar payload 集中在平行 tape 中；未修改 string/number/bool/null 在
writer 中直接复制原 token，Int/Float 在 tape 原地更新，新增 object field 和 array element 才进入
稀疏 `JsonValue` overlay。case26 服务端改用 cursor，并通过 sibling cursor 顺序遍历 events，避免
数组按 index 形成 O(n²) 扫描。

功能测试覆盖 object/array 导航、Int/Float 修改、escaped/UTF-8 string、object put、array add、
serialize 后由旧 DOM 重新解析校验及非法 delimiter 拒绝。Release stdx 全量构建 144/144、安装、
动态库来源探针和 100 KB HTTP smoke 均通过；正式结果 44,717/44,717，失败为 0。

60 秒 MSGC-only 结果位于
`result/20260818_512M_c100_100k_HeapGrow3_Scavenge4_compare_9/msgc`：

| 指标 | DOM 基线 compare_6 | compact tape compare_9 | 变化 |
|---|---:|---:|---:|
| 总吞吐 | 3,572 req/s | 734 req/s | -79.5% |
| 后 30 秒平均吞吐 | 3,596.195 req/s | 689.443 req/s | -80.8% |
| 成功/失败请求 | 214,838 / 0 | 44,717 / 0 | 功能正确 |
| CPU/request | 1.8556 ms | 3.6952 ms | +99.1% |
| 平均 RSS | 3,729.37 MiB | 2,500.01 MiB | -33.0% |
| 峰值 RSS | 3,862.75 MiB | 3,345.61 MiB | -13.4% |
| 平均 CPU | 658.04% | 270.17% | -58.9% |
| GC 次数 | 205 | 70 | -65.9% |
| GC 总耗时 | 5,561.58 ms | 3,691.10 ms | -33.6% |
| GC 平均耗时 | 27.13 ms | 52.73 ms | +94.4% |

10 秒 JSON phase 诊断得到：server compact parse 3,347 us/request、mutation 253 us/request、
serialize 857 us/request，客户端旧 DOM parse 1,417 us/request。瓶颈首先是 compact parse，而不是
COW mutation。当前原型每个节点向 9-word `ArrayList<Int64>` tape 及三条 scalar side array 执行
多次 generic add；100 KB/8,519-node 输入形成数个新的大 backing array，扩容后还会留下旧大数组。
它减少了语言对象个数，却把负担转成每请求约十万次 tape append 和大数组复制/回收。结果与 RSS、
GC 次数下降但单次 GC 翻倍、CPU 并行度显著下降完全一致。

结论：这个 API 验证了 compact 表示降低 live/RSS 的方向，但当前“多条可增长平行数组”不是可上线
实现。后续若继续，应改成预扫描精确计数后一次分配的 packed 32-bit node arena，或固定 chunk 的
`CompactNode` slab；在 parse-only 超过现有 DOM 前不得切换 case26 默认路径。因此本阶段没有把
`JsonDocument` 安装进最终 SDK，原型完整保存在
`cangjie_stdx_region_mark/docs/experiments/json_document.cj.disabled`，最终组合恢复现有 DOM。

### 10.3 阶段三：AArch64 NEON whole-document structural scan

本阶段实现 `CJ_JSON_StructuralScan` 和 opt-in `JsonStructuralIndex`。AArch64 路径每 16 B 用 NEON
并行分类 quote、backslash、`{}[],:` 和 control byte，再用跨块 string/escape 状态排除字符串内部
的 comma/colon/brace；输出 structural byte offset。非 AArch64 保留标量 fallback。测试验证了
字符串内逗号不进入 index、quote/escape 状态正确以及 unterminated string 被拒绝；Release stdx
全量构建 146/146 并安装成功。

按预定 gate，先在同一 MSGC SDK、同一 99,572 B case26 payload 上执行 500 次 parse-only：

| 路径 | 总耗时 | 平均耗时 | 相对标量 parser |
|---|---:|---:|---:|
| 当前 owned DOM 融合标量 parse | 231,682 us | 463 us | 基线 |
| NEON structural scan + 当前 DOM parse | 323,197 us | 646 us | +39.50%（更慢） |

门槛失败的原因不是 NEON 比较本身，而是 stage-2 仍完全忽略 structural index：每请求先创建最坏
容量为 input size 的 `Array<Int32>`（约 389 KiB），整包扫描一次，再由旧 parser 从头扫描一次。
这正是此前要求避免的“额外预扫描”。只有把 compact parser 改成精确预分配的 packed arena，且
stage-2 直接按 index 跳转 delimiter/quote 时，这次 stage-1 才可能抵偿 allocation 和第二遍读取。

因此 scanner/index 没有进入最终 stdx，也不接入 `JsonValue.fromOwnedUtf8Bytes()`；实现思路、
测试和 microbenchmark 数据保留为实验记录。阶段三验证以 parse-only gate 为准；最终 60 秒验证
使用阶段前的默认组合：连续 HTTP body、现有 DOM parser/writer，三个失败实验均不参与生产路径。

### 10.4 最终组合验证、回退与环境判定

三个阶段均完成实现与独立验证，但没有一个满足进入最终默认路径的性能门槛：segmented HTTP 因
13 次 socket write 使稳态吞吐下降 65.84%；parallel tape compact parser 使稳态吞吐下降 80.8%；
NEON scan 在 stage-2 未消费 index 时使 parse-only 变慢 39.50%。继续把三者机械组合只会叠加已知
负收益，因此最终安装产物回退到阶段前的连续 HTTP body 和 owned DOM parser/writer；compact
原型移到非生产目录，segmented 与 structural scan 的生产改动撤销。

回退后的 Release stdx 构建、安装成功。最终语义回归包括 5 个源码结构检查，以及在
`--gc-mode=marksweep` 下实际编译运行的 flat array、flat object、parser slab arena、delimiter
state machine，全部通过。SDK 三件套来源保持固定：compiler、MSGC runtime、stdlib 均来自
`cangjie_compiler_region_mark/output/msgc`，只有 stdx 来自 `cangjie_stdx_region_mark`；最终探针和
校验值如下：

- `libstdx.encoding.json.dylib` SHA-256：
  `c9004b4522b30a911a33971945d240e4bcfd0f7ed80258243a2b537b624828e7`
- `libstdx.net.http.dylib` SHA-256：
  `3d44052ff72ddd4c216c1ff31d562c400e05ea8c0a60fd15be5eeb7cc779eaae`
- `libcangjie-runtime.dylib` MD5：`d41e9d86deb12474dbb6a46b37c103a9`，与 compare_6 基线一致。

最终 60 秒 MSGC-only 结果位于
`result/20260818_512M_c100_100k_HeapGrow3_Scavenge4_compare_19/msgc`：

| 指标 | compare_6 基线 | 最终 compare_19 | 表面变化 |
|---|---:|---:|---:|
| 总吞吐 | 3,572 req/s | 1,686 req/s | -52.8% |
| 后 30 秒平均吞吐 | 3,596.195 req/s | 1,671.8 req/s | -53.5% |
| 成功/失败请求 | 214,838 / 0 | 101,538 / 0 | 0 功能失败 |
| CPU/request | 1.8556 ms | 2.2625 ms | +21.9% |
| 平均 CPU | 658.04% | 381.45% | -42.0% |
| 平均/峰值 RSS | 3,729.37 / 3,862.75 MiB | 3,086.70 / 3,347.14 MiB | -17.2% / -13.3% |
| GC 次数 | 205 | 111 | -45.9% |
| GC 总时间 | 5,561.58 ms | 4,836.56 ms | -13.0% |
| GC 平均/p95 | 27.13 / 37.98 ms | 43.57 / 93.50 ms | +60.6% / +146.2% |

这组最终数字不能作为代码回退后的有效 A/B：最终 benchmark snapshot 与 compare_6 完全相同，
runtime 校验和完全相同，任务新增生产改动已全部退出构建，但测量前系统 load average 为
10.73/14.83/14.01，WindowServer、音乐播放器、浏览器和 IDE 等持续占用 CPU；MSGC 自身平均 CPU
也从基线 658.04% 降到 381.45%。这说明主要限制是本轮机器调度/CPU 可用量，而不是最终 JSON
路径。`pmset -g therm` 没有报告系统 thermal/performance warning，因此只能确认外部负载污染，
不能进一步断言具体是温控还是某一个进程造成。

最终结论分两层：功能上，三个方向都完成了可运行原型和阶段验证；性能上，三个门禁均失败，
所以没有把负收益实现留在最终 SDK。compare_19 如实保留为受污染的最终执行记录，不把其 -52.8%
归因于 JSON 代码，也不宣称它代表回退后稳定吞吐。需要可比较的最终数值时，应在系统负载恢复后
只重跑一次相同 60 秒 MSGC 用例，无需再次修改或重编代码。

## 11. 2026-08-18 通用大 JSON/HTTP segmented gather 完整实现

### 11.1 落地范围与设计

第 10.1 节原型的负收益来自“13 个 segment 对应 13 次 socket write”，而不是 segmented JSON
本身必然更慢。本轮以建设通用能力为目标，把缺失的 gather I/O 补齐，链路如下：

`JsonValue.toSegmentedBytes()` → `JsonByteSegments` → `HttpSegmentedBody` →
`BufferedConn.writeGathered()` → `StreamingSocket.writeGathered()` →
`CJ_MRT_SockWritevNonBlock()` → `sendmsg()`。

具体改动分三层：

- runtime 新增 `SockWritevNonBlock`。Unix TCP/domain socket 使用最多 64 个 `iovec` 调用一次
  `sendmsg`，处理 `EINTR`，并把 `EAGAIN/EWOULDBLOCK` 映射回调度层；Windows 保持不支持，交给
  上层顺序 write fallback。
- stdlib `StreamingSocket` 增加 gather API。Dopra TCP 路径 pin 每个源 segment，正确推进
  `(segmentIndex, segmentOffset)` 以处理 partial write 和空段；超过 64 段时循环多个 sendmsg batch，
  `EAGAIN` 时进入原有 writable wait。指针、长度和 pin handle 的 64 槽 scratch array 挂在 socket
  实例上复用，不再每个响应分配四组临时数组；现有 `SocketHolder.write` 单 writer 约束保证该 scratch
  不会被并发写者覆盖。其他 socket/backend 使用默认顺序 write，API 不破坏跨平台可用性。
- stdx JSON writer 以固定 8192 B segment 输出，HTTP request/response builder 接受
  `Array<Array<UInt8>>` 并保留精确 Content-Length；server/client 的已知长度 body 走 gather。
  原连续 writer 保持独立且没有新增 mode branch。长字符串先调用已有 native escape classifier：确认
  无需转义后直接跨 segment 复制，只有含 escape 的字符串才回退逐字节路径。

case26 新增 `CASE26_HTTP_OUTPUT_MODE=contiguous|segmented`，默认仍为 `contiguous`。这使通用能力可以
按 workload 显式启用，同时不会把实验结果自动变成所有 HTTP 服务的默认行为。

### 11.2 构建安装与功能验证

构建安装严格使用同一 Region-mark MSGC SDK：

- compiler：`cangjie_compiler_region_mark/output/msgc` 原有 compiler；
- runtime：本仓 `cangjie_runtime_region_mark/runtime`，通过固定脚本
  `build/scripts/rebuild_msgc_runtime_output.py` 构建并安装；
- stdlib：`cangjie_runtime_region_mark/stdlib`，以 `marksweep` 模式构建并安装；
- stdx：`cangjie_stdx_region_mark`，以 `marksweep` 模式构建并安装。

runtime 替换后，所有验证用 `.cj` 都重新使用该 SDK 和 `--gc-mode=marksweep` 编译，没有复用旧
binary。功能/契约验证包括：

1. runtime/stdlib 源码契约检查：导出符号、Unix `sendmsg`、64 iovec 上限、partial/EAGAIN 游标、
   per-socket scratch 复用以及默认 fallback 均存在；
2. 100 KB JSON 的 segmented bytes 与连续 writer 逐字节完全一致，覆盖跨段 escape、UTF-8、
   Int64.Min、Float、空 object/array；每段非空且不超过配置大小；
3. `HttpSegmentedBody` 的 Content-Length、跨段 read、seek/rewind 后再次读取均正确；
4. 真实 localhost TCP gather 覆盖 80 个以上非空段、夹杂空段和约 1.38 MB body，强制跨过 64
   iovec 单次上限并执行多轮 sendmsg，接收端结果逐字节一致；
5. case26 以 1K payload/c2/2s 做实际 HTTP smoke：26,693 成功、0 失败、12,662 req/s；
6. 动态库探针确认进程实际加载当前 SDK 中的 `libcangjie-runtime.dylib`、
   `libstdx.encoding.json.dylib`、`libstdx.net.http.dylib`。

最终安装产物均通过 codesign 校验；`CJ_MRT_SockWritevNonBlock` 在 SDK 的
`libcangjie-std-net.dylib` 中为全局 text symbol。SHA-256 为：runtime
`473b1895d41e863c06a17510be359b17689c07d8e73c071a3ec5bcd33fb3f677`，std.net
`a7ea1ea98d9dfba18012eea80211ea4fe92e581ba16a2a38ce4023d1028b737a`，stdx JSON
`d030d899fb0ccfaf552a5a44b6825aa53366ccd3ba3590912bb61a0e8fcccd23`，stdx HTTP
`0236c0454aa66bf7e56c7c19fb79c382bcf767a05a5a12d977b8975ea730519b`。

首次扩展多 batch 测试时出现的 `Abort trap` 已定位为验证命令遗漏 stdx 的
`DYLD_LIBRARY_PATH`，dyld 明确报 `Library not loaded: @rpath/libstdx.encoding.json.dylib`；补齐
隔离环境中的 stdx 动态库路径后同一测试通过，因此它不是 gather 实现或大 body 的运行时崩溃。

### 11.3 60 秒 MSGC 验证结果

固定配置为 100K/c100/60s、512 MiB persistent、20 GiB soft heap、P10、HeapGrowth3、
ScavengeGrowth4，仅运行 MSGC。`compare_20` 与 `compare_21` 使用同一套已安装的
runtime/stdlib/stdx 和同一 benchmark snapshot，唯一模式差异是
`CASE26_HTTP_OUTPUT_MODE=contiguous|segmented`，因此是本轮最稳定的直接 A/B：

| 指标 | contiguous compare_20 | segmented gather compare_21 | 变化 |
|---|---:|---:|---:|
| 总吞吐 | 2,541 req/s | 2,439 req/s | -4.0% |
| 前约 30 秒平均吞吐 | 2,524.96 req/s | 2,372.96 req/s | -6.0% |
| 后约 30 秒平均吞吐 | 2,563.30 req/s | 2,511.26 req/s | -2.0% |
| 成功/失败请求 | 152,900 / 0 | 146,762 / 0 | 均无功能失败 |
| CPU/request | 2.0212 ms | 2.1292 ms | +5.35% |
| 平均 RSS | 3,809.49 MiB | 3,703.31 MiB | -2.8% |
| 峰值 RSS | 4,006.00 MiB | 3,882.45 MiB | -3.1% |
| GC 次数 | 147 | 146 | -0.7% |
| GC 总耗时 | 6,873.97 ms | 6,106.74 ms | -11.2% |
| GC 平均/最大/p95 | 46.76 / 226.46 / 79.59 ms | 41.83 / 151.08 / 68.08 ms | 均下降 |

这说明完整 `sendmsg` gather 已把第 10.1 节的 13 次 socket write 灾难消除：稳态差距从
-65.84% 收敛到约 -2.0%。它同时保留了 segmented 输出在 RSS、峰值内存和 GC pause 上的收益；
剩余成本主要是每响应 13 个 managed segment 的遍历、pin/unpin、FFI 以及跨段 writer 边界。
完整 A/B 图表见
`result/20260818_512M_c100_100k_HeapGrow3_Scavenge4_compare_21/segmented_gather_ab_report.md`。

随后在 stdx 加入“长且无需 escape 的字符串 native 分类后直接 span copy”并重新构建安装，最终
segmented run 位于 `compare_22`：

| 指标 | contiguous compare_20 | fast-path segmented compare_22 | 变化/判读 |
|---|---:|---:|---:|
| 总吞吐 | 2,541 req/s | 2,176 req/s | -14.4%，受后半程 CPU 可用量下降污染 |
| 前约 30 秒平均吞吐 | 2,524.96 req/s | 2,519.18 req/s | -0.23% |
| 后约 30 秒平均吞吐 | 2,563.30 req/s | 1,838.42 req/s | -28.3%，不具备同负载可比性 |
| CPU/request | 2.0212 ms | 2.0448 ms | +1.17% |
| 成功/失败请求 | 152,900 / 0 | 130,940 / 0 | 均无功能失败 |
| 平均/峰值 RSS | 3,809.49 / 4,006.00 MiB | 3,797.60 / 4,079.81 MiB | -0.3% / +1.8% |
| GC 次数/总耗时 | 147 / 6,873.97 ms | 128 / 5,571.57 ms | 请求量不同，不单独归因 |

`compare_22` 前 30 秒与连续输出基本持平，CPU/request 差距也由 +5.35% 收窄到 +1.17%，证明
字符串 fast path 命中了预期成本；但约 36 秒后每 2 秒请求数持续下降，最后窗口仅 2,060，而进程
平均 CPU 从基线 508.88% 降到 442.82%。测试后系统 load average 为 7.90/9.33/9.24，存在持续的
WindowServer、音频及桌面应用负载。因同一 run 内前后段 CPU 供给明显变化，不能把总吞吐 -14.4%
全部归因于最终代码，也不能用这次受污染运行宣称吞吐已提升。完整图表见
`result/20260818_512M_c100_100k_HeapGrow3_Scavenge4_compare_22/segmented_gather_final_report.md`。

### 11.4 结论与使用建议

本轮已经完成“通用大 JSON/HTTP 输出能力”，而不是只做一个会把 body 拆成多次 write 的原型：
JSON 不需要最终连续大数组，HTTP 保留 Content-Length，socket 使用真正 gather write，并完整处理
partial write、EAGAIN、空段、超过 iovec 上限、多 backend fallback 和源数组 pin 生命周期。

性能结论应采用两层口径：

- 稳定直接 A/B 显示：完整 gather 后段吞吐约 -2.0%、CPU/request +5.35%，但平均/峰值 RSS
  -2.8%/-3.1%，GC pause 指标改善；适合更重视峰值内存、GC 抖动或更大 body 的场景。
- 最终 long-safe-string fast path 在未受污染的前 30 秒与连续输出只差 -0.23%，整段
  CPU/request 只差 +1.17%；它基本消除了 Cangjie 逐字节字符串扫描成本。后半段外部负载使本次
  60 秒总吞吐不可作为最终稳定门禁，结果如实保留但不做过度结论。

因此当前默认保持 `contiguous`，避免在 100 KB case 上自动承担小幅 CPU 成本；通用能力通过
`CASE26_HTTP_OUTPUT_MODE=segmented` 显式启用。对于数百 KB 到数 MB 的响应、RSS/GC 受限服务，
建议直接使用 segmented API；若未来要改成 HTTP 默认策略，应在空闲、固定系统负载下补一组
100 KB/1 MB/4 MB body-size matrix，并以 CPU/request、峰值 RSS、p95/p99 latency 和实际
sendmsg 次数共同判定，而不是只看一次总吞吐。
