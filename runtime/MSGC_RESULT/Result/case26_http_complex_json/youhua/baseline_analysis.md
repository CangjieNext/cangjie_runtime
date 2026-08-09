# Case26 HTTP Complex JSON：MSGC 性能基准与优化分析

> 基准目录：`../result/20260808_512M_c100_100k_HeapGrow1_Scavenge2_Procs10_compare`

本文档记录 case26 当前基准数据、采样证据、已验证结论以及后续优化建议。本文档只描述分析结果，不包含 runtime 源码修改。

## 1. 基准范围与执行配置

本基准来自 Linux/aarch64，执行时间约 60 秒：

| 参数 | 值 |
|---|---:|
| payload | 100K |
| concurrency | 100 |
| duration | 60000 ms |
| persistent bytes/workers | 512 MiB / 8 |
| HEAP_SIZE | 20 GB |
| PROCS | 10（Java 使用 `ActiveProcessorCount=10`，cpuset 为 0-9） |
| MSGC HeapGrowth/ScavengeGrowth | 1 / 2 |
| MSGC 最小 GC 阈值参数 | 20 MB |
| client mode/max inflight | sync / 32 |

MSGC compiler/runtime SDK 为 `/home/lipeisheng/cangjie/cangjie_compiler_CMSGC/output/msgc`，GC 模式为 marksweep，runtime md5 为 `84a5ce4c657e5bdc60343253884293f7`。Java 使用 `/usr/lib/jvm/java-25-openjdk-arm64/bin/java`，版本 OpenJDK 25.0.3，`-Xmx20g`。

完整来源记录见 [MSGC metadata](../result/20260808_512M_c100_100k_HeapGrow1_Scavenge2_Procs10_compare/msgc/run_metadata.env) 和 [Java metadata](../result/20260808_512M_c100_100k_HeapGrow1_Scavenge2_Procs10_compare/java/run_metadata.env)。

## 2. 基准结果

| 指标 | CopyGC | MSGC | Go | Java 25 |
|---|---:|---:|---:|---:|
| 吞吐 (req/s) | 513 | **792** | 545 | **1255** |
| 总请求数 | 31,096 | **47,750** | 32,827 | **75,441** |
| RSS 平均 (MB) | 3194.08 | **1858.55** | 1278.34 | 5280.77 |
| RSS 峰值 (MB) | 3807.06 | **1872.74** | 1332.36 | 5522.46 |
| CPU 平均 | 959.90% | 962.73% | 854.60% | 905.61% |
| GC 次数 | 155 | 362 | 186 | **实际 110** |
| GC 总时间 (ms) | 44796.86 | 12920.14 | 5786.36 | **1929.20** |
| GC 平均时间 (ms) | 289.01 | 35.69 | 31.11 | **17.54** |

MSGC 相比 Java 吞吐低约 36.9%，Java 约为 MSGC 的 1.58 倍；但 Java 平均 RSS 约为 MSGC 的 2.84 倍。两者 CPU 均接近 10 个核心，不能归因于 Java 获得了更多 CPU。

原始图表和报告见 [compare_report.md](../result/20260808_512M_c100_100k_HeapGrow1_Scavenge2_Procs10_compare/compare_report.md)。报告中的 Java GC=0 是解析器不兼容 Java 25 日志格式导致的错误，不是 Java 没有 GC。

## 3. GC 分析

MSGC 362 个周期的 GC 时间分布：

| 阶段 | 时间 (ms) | 占比 |
|---|---:|---:|
| PreEnum | 1574.33 | 12.2% |
| EnumRoots | 702.56 | 5.4% |
| ConcurrentMark | 6788.52 | 52.5% |
| MarkTermination | 1340.46 | 10.4% |
| PostMarkTermination | 2349.30 | 18.2% |
| 其他 | 164.50 | 1.3% |

核心成本是 Mark 及其前后处理，而不是单独的 Sweep。后期周期中 Assist 通常承担约 40%～53% 的 Mark WU，但吞吐只有约 13～17 WU/us；helper/gc-main 通常约 22～30 WU/us。Assist 已经明显占用 mutator 预算。

Assist 不能简单关闭：在 [noassist 实验](../result/20260808_512M_c100_100k_HeapGrow1_Scavenge2_Procs10_compare/profile/msgc_noassist.stdout.txt) 中，进程最终 SIGABRT，RSS 达到约 21.3 GB，未完成 Sweep backlog 达到约 27 万 region。正确方向是减少无效 Assist、提高 Assist 的有效吞吐，而不是移除 Assist。

`PostMarkTermination()` 中会执行 `ComputeMarkedStats`、LARGE region 处理、阈值更新和 Sweep handoff；源码位置为 [MarkSweepCollector.cpp:2705](../../../../src/Heap/MCollector/MarkSweepCollector.cpp:2705)。该阶段已经占 18.2%，应作为重点优化对象。

## 4. Java JFR 与 MSGC 采样分析

### Java JFR

20 秒 JFR 运行结果为 23,529 请求、1172 req/s。CPU 样本主要集中在：

- `JsonParser.parseObject/parseValue`
- `JsonWriter.append`
- `StringBuilder.append`
- `HashMap.put`、`LinkedHashMap.newNode`
- `Arrays.copyOf/copyOfRange`

分配样本也主要是数组扩容、字符串构造和 Map 扩容，说明 Java 的业务计算并非没有成本，而是 JIT 对这些路径优化得更充分。

JFR 还记录到 5909 次 SocketRead、5864 次 SocketWrite，以及 6029 个 `SocketTimeoutException: Read timed out`。由于最终请求全部成功，这些异常需要单独确认是否来自连接超时/关闭阶段，暂时不把它们作为主吞吐瓶颈。

### MSGC 采样限制

MSGC 使用 runtime 内置 CPU profiler 和 GDB 采样，profile 主要覆盖主 mutator，样本大量落在 `std.core.sleep`，没有完整覆盖 HTTP worker。它可以证明调度线程状态，但不能作为 MSGC JSON 热点的最终证据。系统当前没有可用的 `perf` 硬件采样（`perf_event_paranoid=4`）。

采样文件见 [MSGC profile](../result/20260808_512M_c100_100k_HeapGrow1_Scavenge2_Procs10_compare/profile/msgc_profile.json) 和 [Java JFR](../result/20260808_512M_c100_100k_HeapGrow1_Scavenge2_Procs10_compare/profile/java_profile.jfr)。

## 5. A/B 实验

### HeapGrowth=2

20 秒结果：782 req/s、约 82 个 GC、RSS 约 2.59 GB、稳态 threshold 约 2.34 GB。它显著降低 GC 频率，但相比 MSGC 基线没有证明吞吐提升，代价是 RSS 增加。它是内存/吞吐权衡参数，不是确定性的性能优化。

详见 [heapgrowth2 日志](../result/20260808_512M_c100_100k_HeapGrow1_Scavenge2_Procs10_compare/profile/msgc_heapgrowth2.gc.log)。

### cjMSMinGCThreshold=3gb

该实验没有把稳态阈值直接提升到 3 GB，稳态仍约 1.535 GB。当前参数在 runtime 中承担初始/floor/cap 相关语义，不能当作直接的固定 GC threshold 开关。

### CJ_MS_DISABLE_ASSIST=1

实验失败并 OOM/abort，说明 Assist 是维持 Mark/Sweep 进度所必需的。

## 6. 优化建议与验证计划

按优先级建议：

1. **优化 HTTP body 缓冲和拷贝。** `readBodyStr/readRespStr` 当前使用 8 KiB chunk、无预留的 `ArrayList<UInt8>`，最后再 `toArray()` 和 UTF-8 转换，存在多次扩容与复制。应按 Content-Length 预留、复用 buffer、减少中间 Array/切片，并对连接池大小做 A/B。
2. **减少 Assist 空转。** 继续提高 backlog 准入门槛，降低无效 claim/publication；同时保留 Assist，并测量有效 WU、empty、admitted、rejectedBacklog 和请求吞吐。
3. **并行化 PostMark。** 检查 `ComputeMarkedStats` 的 helper 选择、bitmap 扫描和 LARGE region 统计，目标是降低当前 18.2% 的 PostMark 时间。
4. **降低 PreEnum/MarkTermination 协调成本。** 减少跨 shard 探测、root handoff 和 task publication，观察 12.2% PreEnum 与 10.4% MarkTermination 是否下降。
5. **做受控 HeapGrowth A/B。** 建议使用相同 workload 重复测试 Growth=1、2，并记录 RSS、GC 周期、Mark WU/us 和吞吐；不要只根据单次运行判断。
6. **修复 Java GC 报告解析。** 让比较脚本识别 Java 25 `-Xlog:gc*` 的 `Pause Young` 行，避免基准报告误导后续判断。
7. **完善 MSGC worker 级采样。** 需要覆盖所有 CJ mutator、GC helper 和网络 worker，才能定位 JSON、HTTP、futex 和调度开销的真实占比。

每项优化都应使用相同的 payload、并发、持续时间、P、HEAP_SIZE 和 SDK，并至少重复 3 次；替换 runtime 后必须重新编译 case26 用例。

## 8. 本轮用例优化实测

本轮暂不修改 GC，只修改 [case26 benchmark](../benchmark.cj)：

- body/response 读取块从 8 KiB 增大到 64 KiB；
- 根据 `HttpRequest.bodySize/HttpResponse.bodySize` 对 body `ArrayList` 预留容量；
- 增加默认关闭的 `CASE26_JSON_PROFILE=1` 分阶段计时；
- 服务端序列化由 `root.toString()` 改为 `root.toStringWithCapacity(requestBytes + 12.5% + 1 KiB)`。

### body buffer A/B

60 秒、c100/100K、P10、20GB heap、512MiB persistent、HeapGrowth=1、ScavengeGrowth=2 下，body buffer 改动结果为 792 req/s，与原基准相同。因此 8 KiB 读块和 ArrayList 扩容不是本 workload 的第一主瓶颈。

结果目录：[http buffer A/B](../youhua/result/20260808_http_buffer_opt_c100_100K_Procs10)

### JSON 分阶段采样

20 秒诊断运行得到：

| 阶段 | 调用数 | 总耗时 | 平均耗时 |
|---|---:|---:|---:|
| 服务端反序列化 | 15,646 | 51.64 s | 3.30 ms |
| 服务端字段修改 | 15,646 | 4.81 s | 0.31 ms |
| 服务端序列化 | 15,646 | **165.64 s** | **10.59 ms** |
| 客户端反序列化 | 15,626 | 49.31 s | 3.15 ms |

诊断计时使用原子计数，会扰动吞吐；该组吞吐为 775 req/s，仅用于阶段占比，不用于和正式基准比较。

结果目录：[JSON phase profile](../youhua/result/20260808_json_phase_profile_c100_100K_Procs10)

### 序列化容量优化

JSON runtime 的 `JsonObject.toString()` 先调用 `asumecapacity()` 全量遍历 JSON 树估算输出容量，再调用 `jsonWriteWithoutFormat()` 第二次遍历并生成字符串。case26 的请求大小已知，因此改用 `toStringWithCapacity()`，直接按输入 body 大小加余量分配输出 buffer，跳过容量估算遍历。

20 秒控制组结果为 829 req/s；正式 60 秒结果为：

| 指标 | 原基准 | JSON 容量优化 | 变化 |
|---|---:|---:|---:|
| 吞吐 | 792 req/s | **841 req/s** | **+6.2%** |
| 总请求数 | 47,750 | **50,700** | +6.2% |
| 成功率 | 100% | 100% | 无回归 |
| RSS 峰值 | 1872.74 MB | 1855.78 MB | -0.9% |

正式结果目录：[JSON capacity optimized 60s](../youhua/result/20260808_json_capacity_opt_c100_100K_Procs10_60s)

这次提升与 JSON 序列化热点相符：字段修改只占约 0.31 ms/请求，而序列化约 10.6 ms/请求。由于当前计时 profile 会产生明显扰动，序列化优化后的 profile 运行只用于确认方向，不能据此比较绝对吞吐。

### 当前 JSON 优化判断

本轮已经验证的第一优化点是：**消除 `toString()` 的 JSON 树容量预扫描**。它在不改变 JSON 结构、不改变 GC 参数的情况下，使 MSGC 正式吞吐从 792 req/s 提升到 841 req/s。

完整的全进程 CPU 火焰图与函数级热点见：[msgc_flamegraph_analysis.md](msgc_flamegraph_analysis.md)。

反序列化仍然约占两端 JSON 处理的主要成本之一。源码中 `JsonParser` 为每次输入创建 JSON object/array 树，object 使用 `HashMap<String, JsonValue>`，array 使用 `ArrayList<JsonValue>`；字符串转义路径还会通过 `strCache` 累积并构造新字符串。下一步应围绕这些路径做独立 A/B：

1. 评估 parser 的 object/array 初始容量，减少 HashMap/ArrayList 扩容；
2. 检查 `strCache` 的复用和字符串构造次数；
3. 对只读校验场景评估流式/字段选择性解析，避免完整构造 JSON 树；
4. 继续用 `CASE26_JSON_PROFILE=1` 做阶段计时，并增加 parser 内部计数后再判断具体函数级收益。

## 7. 结论与限制

当前已证实：MSGC 的主要差距来自较高的 GC/并发 Mark 成本，以及 Cangjie HTTP/JSON 路径中的缓冲区和对象拷贝成本；Java 使用更大的实际内存空间换取了更高吞吐。Assist 必须保留，优先优化其准入和有效工作率；PostMark、PreEnum 和 MarkTermination 也有明确的时间占比。

尚未证实：MSGC HTTP/JSON 的具体函数级 CPU 热点、Java SocketTimeoutException 是否影响稳态请求、以及单独调整 threshold 对吞吐的因果收益。这些需要覆盖全部 worker 的采样和多轮 A/B 验证。

## 9. MSGC 全进程 CPU 火焰图与热点分析

本节补充优化后 MSGC 用例的全进程 `perf` 采样结果。完整的独立分析见 [msgc_flamegraph_analysis.md](msgc_flamegraph_analysis.md)，主要原始产物包括 [SVG 火焰图](result/20260808_msgc_flamegraph_c100_100K_Procs10_v2/msgc_cpu_flamegraph.svg)、[Self 热点报告](result/20260808_msgc_flamegraph_c100_100K_Procs10_v2/perf_hotspots_self_nochildren.txt) 和 [累计热点报告](result/20260808_msgc_flamegraph_c100_100K_Procs10_v2/perf_hotspots_children.txt)。

### 9.1 采样配置与有效性

采样使用已经完成 JSON 序列化容量优化的 case26 benchmark，运行参数与基准保持一致：

- payload=100K，concurrency=100，duration=60000ms；
- persistent=512MiB，persistent workers=8；
- P=10，HEAP_SIZE=20GB；
- `cjHeapGrowth=1`，`cjScavengeGrowth=2`；
- compiler、runtime、stdlib 均来自 `/home/lipeisheng/cangjie/cangjie_compiler_CMSGC/output/msgc`。

使用 Linux `perf 5.15.200`，以 99Hz、DWARF callchain 附加到真实 benchmark PID，对全部线程采样约 58 秒。共获得约 61K 个样本，丢失样本为 0。采样运行完成 49,339 个请求，吞吐为 818 req/s，全部成功；相同版本无采样的正式结果为 841 req/s，采样扰动约 2.7%。因此，本节百分比适合判断热点排序，不应直接换算为精确的单请求耗时。

### 9.2 CPU 的模块与线程分布

按 DSO 汇总的 Self 样本如下：

| 模块 | CPU 样本占比 |
|---|---:|
| `libstdx.encoding.json.so` | **37.30%** |
| `libcangjie-runtime.so` | **32.33%** |
| `libc.so.6` | 14.61% |
| benchmark | 7.89% |
| kernel | 6.31% |
| `libstdx.net.http.so` | 0.78% |
| dynamic linker | 0.36% |
| `libboundscheck` | 0.23% |

JSON 标准库和 runtime 是两个主要 CPU 去向。`libstdx.net.http.so` 只有 0.78%，说明 HTTP 库函数自身不是当前第一瓶颈；但 `libc`、kernel 以及 runtime 中仍可能包含由 HTTP、字符串和对象操作触发的间接成本。

按线程名汇总如下：

| 线程 | CPU 样本占比 |
|---|---:|
| `schd-worker` | **78.73%** |
| benchmark 主线程 | 8.84% |
| `gc-helper` | 2.70% |
| `gc-main-thread` | 2.35% |
| `gc-pool-t1..t4` | 6.90% |
| `schmon` | 0.49% |

GC 专用线程合计约 10.7%，但这不是 GC 的完整 CPU 占比：Mark/Sweep Assist 会运行在 `schd-worker` 上。反过来，也不能把 GC 专用线程占比与具名 GC 函数的 Self 占比直接相加，因为两者存在重叠。

### 9.3 函数自身热点

跨线程合并后的主要 Self 热点为：

| 函数或路径 | Self 样本占比 | 主要含义 |
|---|---:|---|
| `memset` | **7.52%** | 对象、数组和 buffer 初始化，来源不只 JSON |
| `parseJsonString` | **7.33%** | 字符串扫描、转义和构造 |
| `parseJsonObject` | **5.94%** | object 解析与 HashMap 插入 |
| `jsonWriteWithoutFormat` | **3.86%** | JSON 树递归序列化 |
| `WriteBuffer.append(Array)` | **3.15%** | 输出 buffer 写入与复制 |
| `MCC_AcquireRawData` | 2.80% | String/Array rawData 获取 |
| `MRT_GetThreadLocalData` | 2.47% | runtime 线程本地数据访问 |
| `TypeInfo::IsSubType` | 2.42% | `JsonValue` 动态类型判断 |
| `__aarch64_ldadd8_relax` | 2.23% | 原子更新竞争成本 |
| `MarkSweepCollector::AddRefToMarkBatch` | 2.23% | Mark batch 构建与发布 |
| `memcpy` | 2.11% | 字符串、数组和输出 buffer 拷贝 |
| `MarkSweepCollector::RunRegionTask` | 1.81% | Region Mark 扫描 |
| `CJ_JSON_StringEscapeCharNumGet` | 1.77% | 序列化转义扫描 |
| `parseJson` | 1.67% | 通用 JSON 类型分派 |
| CAS | 1.62% | 并发原子操作 |
| `WriteBuffer.checkAndExpend` | 1.61% | 输出 buffer 容量检查和扩容 |
| `strtod` | 1.58% | 浮点数解析 |
| `CJ_MCC_ReleaseRawData` | 1.55% | rawData 释放 |
| `checkInvalid` | 1.53% | 字符串有效性检查 |
| `parseJsonNumberSlow` | 1.30% | JSON 数字慢路径 |
| `tryParseJsonIntFast` | 1.25% | JSON 整数快路径 |
| `wyhash` | 1.24% | object key 的 HashMap 哈希 |
| `String.toArray` | 1.12% | String 到 Array 转换 |
| `parseJsonArray` | 0.92% | array 解析入口自身成本 |

按符号归类后，已列出样本中的近似 Self 占比为：JSON parse 20.42%、JSON write 12.01%、`memset/memcpy` 9.87%、MSGC Mark 6.56%、RawData/安全检查 4.66%、TLS/调度 3.69%、类型判断 2.90%、哈希/字符串校验 2.77%、kernel/network 1.95%、MSGC Sweep 0.98%。这些是便于观察方向的近似分类，仍有约 20.12% 的 Other，且同一底层函数可能由不同上层路径调用，不能将分类结果解释为精确的模块耗时。

### 9.4 累计调用链

累计调用链显示：

- `JsonValue.fromStr` 反序列化调用链约占 39.04%；
- `JsonValue.toStringWithCapacity`/`serializeServerJson` 序列化调用链约占 25.64%。

累计占比包含子函数，不能与上一节的 Self 占比相加。它说明完整 JSON parse 调用链是当前最大的业务 CPU 消耗，序列化仍是第二个主要方向。runtime 的 32.33% 也不能整体归为 GC，其中包含 RawData、TLS、类型判断、对象操作及 GC 等多类成本。

### 9.5 基于热点的优化方向

以下方向按当前证据优先级排列，除已经完成的 `toStringWithCapacity` 优化外，其余均是待 A/B 验证的假设：

1. **优先优化 parser 的 `strCache`。** 当前对应 stdx 源码中，`JsonParser` 会按 `max(input.size / 4, 64)` 无条件初始化 `strCache`。100K 输入每次解析约预留 25KB，而该 cache 主要服务于转义字符串。建议先做懒分配或小初始容量 A/B，仅在遇到转义时增长。它同时对应 `parseJsonString`、`memset` 和分配热点。
2. **调整 object/array 初始容量。** 当前对应源码的 object/array 初始容量均为 8。可分别测试 8、16、32 或自适应估算，观察 `parseJsonObject`、HashMap resize、ArrayList grow、吞吐和 RSS 的变化。
3. **减少 WriteBuffer 的扩容与临时数组。** 核验 `checkAndExpend()` 在 `minCapacity == oldCapacity` 时是否发生无效扩容；把 compact 序列化中反复出现的 `"{}".toArray()`、`"[]".toArray()`、`"\":".toArray()` 改为静态 byte 数据或逐 byte append；评估收紧 12.5% 容量余量和复用 buffer，以降低 `String.toArray`、`memset` 与最终复制。
4. **减少 RawData 和 escape 的重复遍历。** 评估 WriteBuffer 直接接收 raw bytes/地址区间，并为无转义字符串提供一次扫描/复制路径，降低 Acquire/ReleaseRawData、escape scan 和 `memcpy`。
5. **评估 JSON kind/tag 快速分派。** `TypeInfo::IsSubType` Self 为 2.42%，可针对频繁的 `asObject/asArray/asInt/asFloat` 测试内部 kind/tag 快速路径，但必须通过独立正确性测试和 microbenchmark 验证。
6. **暂不优先重构 HTTP。** HTTP DSO Self 只有 0.78%，在当前 workload 下收益预期低于 JSON parser、WriteBuffer 和 runtime 支撑路径。

本地 stdx 源码中的函数和采样二进制符号相符，但本次采样 metadata 没有单独记录 stdx 源码 commit。因此，上述源码对应关系属于高置信度判断，不应表述为已经验证的唯一根因。`memset/memcpy` 也包含 JSON、runtime 和其他对象初始化来源，后续需要结合 allocation count 或更细的调用链继续拆分。

### 9.6 本轮结论

全进程采样已经补足了此前“MSGC HTTP/JSON 具体函数级热点尚未证实”的证据缺口：当前优化版本中，JSON parse 是最大的完整业务调用链，JSON write 次之；GC 仍然有明显成本，但 runtime 样本不能全部归入 GC，HTTP 库函数自身也不是首要热点。

下一轮建议保持相同 SDK 和 case26 参数，每项至少重复 3 次，按 `strCache` 懒分配、object/array 容量、WriteBuffer exact-fit/静态 bytes/RawData 路径的顺序分别做单变量 A/B。最优版本再重新采集一次全进程火焰图，确认热点是否从 JSON 转移到 runtime、GC 或网络路径。
