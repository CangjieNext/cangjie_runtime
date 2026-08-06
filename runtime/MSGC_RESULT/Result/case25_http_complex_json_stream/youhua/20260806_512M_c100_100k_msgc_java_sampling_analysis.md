# Case25 512M / C100 / 100K：MSGC 与 Java 采样分析

## 1. 基准结果与结论

原始对比结果目录：

`../result/20260806_512M_c100_100k_HeapGrow1_Scavenge2_compare_2`

参数：`payload=100K`、`concurrency=100`、`duration=60s`、`persistent_bytes=512MB`、`persistent_workers=8`、同步 HTTP client。MSGC 使用 `cjHeapSize=8gb`、`cjHeapGrowth=1`、`cjScavengeGrowth=2`、`cjMSMinGCThreshold=20mb`；Java 使用 `-Xmx8g`。

| 指标 | MSGC | Java | 结论 |
| --- | ---: | ---: | --- |
| 成功吞吐 | 1,982 req/s | 2,643 req/s | MSGC 低 25.0%；Java 高 33.4% |
| 成功请求 | 119,199 | 158,744 | 全部成功，无失败请求 |
| 平均 CPU | 345.61% | 309.35% | MSGC 消耗更多 CPU，却完成更少请求 |
| 平均 RSS | 1,581.94 MB | 1,646.09 MB | Java RSS 更高，非 MSGC 内存上限压制 |
| GC 次数 | 182 | 201 | 次数接近，不能据此解释吞吐差距 |
| GC 总时间 | 2,380.63 ms | 1,000.38 ms | MSGC 较 Java 多约 1.38s，最多解释约 2.3 个百分点的墙钟差异 |

结论：吞吐差距的主因是 Cangjie 流式 JSON 的未修改字段复制路径产生了额外的 byte array、UTF-8 String 与输出复制；MSGC mark/sweep assist 是第二层成本，而非单纯 GC 暂停或内存限制。

## 2. 本次采样文件

本目录归档本次新增的原始采样数据：

| 文件 | 采样对象 | 方法 | 用途 |
| --- | --- | --- | --- |
| `msgc_sample_12s.txt` | MSGC benchmark | `sample <pid> 12` | Native/Cangjie/Runtime 热点调用栈 |
| `msgc_short_run_stdout.txt` | MSGC benchmark | 30s 同参短跑 | 采样期间的进度和成功吞吐 |
| `msgc_short_run_gc.log` | MSGC runtime | 30s 同参短跑 | mark、sweep、assist 明细 |
| `java_sample_12s.txt` | Java JVM | `sample <pid> 12` | JVM native 栈、GC 与系统调用概览 |
| `java_short_run_stdout.txt` | Java benchmark | 28s 同参短跑 | 采样期间的进度和成功吞吐 |
| `java_jstack_10x.txt` | Java JVM | 10 次 `jstack` | Java 级 Jackson、HTTP server/client 栈 |

短跑吞吐只用于抓取热点，不能替代 60s 原始结果：`sample`、`jstack` 与采样时的机器负载都会干扰吞吐。原始吞吐结论统一以第 1 节的 60s compare 结果为准。

## 3. MSGC GC 与 assist 分解

从原始 60s `msgc/gc.log` 汇总：

| 阶段 | 累计墙钟时间 |
| --- | ---: |
| 全部 GC cycle | 2.381 s |
| ConcurrentMark | 1.112 s |
| `markLoop` | 1.005 s |
| `quiesce` | 0.051 s |
| PreEnum `clearBitmaps` | 0.241 s |
| EnumRoots | 0.302 s |
| MarkTermination | 0.045 s |

GC 周期内的耗时并不大，但每一轮 GC 都发布了待 sweep region：平均 `unsweptRegions=26,351`（峰值 27,609），并且 `mutatorAssistDisabled=0`；平均 `assistRatio=50,143`，峰值 251,990。

这意味着部分 sweep 成本发生在 mutator 分配慢路径，不会完整落入 `total gc time`。MSGC profile 中已实际捕获：

- `AllocBufferMS::AccumulateSweepDebt`
- `CentralRegionPool::MSSweepAssist`
- `CentralRegionPool::AssistSweep`
- `SweepOneUnsweptRegionForAssist`
- `Mutator::HandleEnumAssist`
- `MarkSweepCollector::*`

因此，GC/sweep assist 确实会压低业务吞吐；但它在 profile 中不如 JSON reader/writer 热点靠前，属于放大因素，不是当前 25% 差距的首因。

## 4. MSGC 热点与源码对应

12 秒 sample 的 Cangjie/Native 热点集中在以下路径：

| 热点 | 采样证据 | 含义 |
| --- | --- | --- |
| `JsonReader.fillInBuf`、`nextSymmetrySegment`、`skipArray` | flat top 位居前列 | 大量读取、扫描并定位 JSON 原样 value |
| `copyRawValue`、`readValueBytes` | profile 多次出现 | 未修改字段的复制路径 |
| `String.<init>`、`memmove`、`checkInvalid` | profile 多次出现 | byte array 转 UTF-8 String、数据搬运及校验 |
| `JsonWriter` value/name 操作 | profile 多次出现 | 重新输出 JSON 字段和值 |
| `CJ_JSON_FloatPrint -> snprintf/vsnprintf -> __dtoa` | profile 可见 | `score`、`credits` 等浮点值的格式化 |
| `MSSweepAssist`、`MarkSweepCollector` | profile 可见 | 分配与 concurrent GC 的 runtime 额外工作 |

Cangjie 用例的原样 value 复制实现：

```cangjie
func copyRawValue(reader: JsonReader, writer: JsonWriter): Unit {
    writer.jsonValue(String.fromUtf8(reader.readValueBytes()))
}
```

该实现现保留在 `../copygc/stream_transform.cj` 第 5--8 行，供不含 `copyValueTo` API 的 CopyGC SDK 编译。它并未构建 DOM，但也不是零复制：每次未修改字段都至少经历 `readValueBytes()` 分配/复制、`String.fromUtf8()` 物化、writer 输出三个步骤。`product_ids` 的每个既有元素同样走这个函数。MSGC 的对应实现使用 `copyValueTo`，位于 `../msgc/stream_transform.cj`。

## 5. Java 采样对照

`sample` 对 HotSpot JIT 后的 Java 业务函数解析不完整，因此使用 native `sample` 加 10 次 `jstack` 交叉确认。

`jstack` 中 Java HTTP server 工作线程的可运行栈主要为：

```text
UTF8StreamJsonParser.nextToken
JsonGenerator.copyCurrentStructure
JacksonStreamTransformer.copyValue
JacksonStreamTransformer.transformEvent / transformEvents / transformRoot
Case27HttpComplexJsonBenchmark$Benchmark$ProcessHandler.handle
```

10 次 jstack 中：

- `JacksonStreamTransformer.transform`：134 个栈帧；
- `copyValue`：100 个栈帧；
- `JsonGenerator.copyCurrentStructure`：96 个栈帧；
- client `Benchmark.doRequest`：1,000 个栈帧；
- `hasServerOk`：仅 13 个栈帧，响应校验不是热点。

Java 的对应复制实现是：

```java
private static void copyValue(JsonParser parser, JsonGenerator generator) throws IOException {
    generator.copyCurrentStructure(parser);
}
```

见 `../java_benchmark/Case27HttpComplexJsonBenchmark.java` 第 1546--1548 行。它由 Jackson 的 UTF-8 parser/generator 直接 token-copy；不会像 Cangjie 版本那样先将完整子 value 转为 Cangjie `String`。

Java 也在做 JSON parse/copy，并非没有此项成本；但其热点集中于 Jackson parser/generator，本地分配和字符串物化较少，因此同样的 HTTP + JSON 业务逻辑下单位请求 CPU 成本更低。

## 6. 优化优先级

1. **最高优先级：增加 JsonReader 到 JsonWriter 的 raw byte/value 直拷贝 API。** 目标是替换 `readValueBytes -> String.fromUtf8 -> jsonValue`，避免中间 `Array<Byte>` 和 `String` 分配。该改动应覆盖 root 的未修改字段、event 的普通字段、attrs 与 product_ids。
2. **优化浮点 JSON 输出。** `CJ_JSON_FloatPrint` 当前落到 `snprintf/__dtoa`；应优先评估使用更轻量、正确舍入的数值转字符串实现。
3. **raw-copy 优化后再量化 sweep assist。** 保持相同参数，比较 `MSSweepAssist`、`SweepOneUnsweptRegionForAssist` 和吞吐变化；此时再决定是否调整后台 sweep 推进或 assist debt。
4. **不要先调大堆或降低 GC 次数。** 原始结果中 Java 的 RSS 更高、MSGC 的 CPU 更高，说明当前最直接的收益来自降低每请求 JSON 分配与复制，而不是放宽内存上限。
