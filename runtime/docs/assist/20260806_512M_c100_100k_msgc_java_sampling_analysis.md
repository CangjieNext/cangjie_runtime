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

结论：原始吞吐差距的主因是 Cangjie 流式 JSON 的未修改字段复制路径产生了额外的 byte array、UTF-8 String 与输出复制；MSGC mark/sweep assist 是第二层成本，而非单纯 GC 暂停或内存限制。第 6 节提出的 raw-copy 优化已于 2026-08-06 实施并安装，尚未以同一 60 s 条件完成 A/B 量化。

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

Cangjie 用例在本轮采样时的原样 value 复制实现：

```cangjie
func copyRawValue(reader: JsonReader, writer: JsonWriter): Unit {
    writer.jsonValue(String.fromUtf8(reader.readValueBytes()))
}
```

该实现并未构建 DOM，但也不是零复制：每次未修改字段都至少经历 `readValueBytes()` 分配/复制、`String.fromUtf8()` 物化、writer 输出三个步骤。`product_ids` 的每个既有元素同样走这个函数。该历史实现已被第 7 节所述的新 API 替换。

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

1. **已实施：JsonReader 到 JsonWriter 的 raw byte/value 直拷贝 API。** 已替换 `readValueBytes -> String.fromUtf8 -> jsonValue`，消除未修改 value 的中间 `Array<Byte>` 和 `String`。root 未修改字段、event 普通字段、attrs 与 product_ids 均已接入；细节和验证见第 7 节。
2. **优化浮点 JSON 输出。** `CJ_JSON_FloatPrint` 当前落到 `snprintf/__dtoa`；应优先评估使用更轻量、正确舍入的数值转字符串实现。
3. **量化 raw-copy 后的 sweep assist。** 保持相同参数，比较 `MSSweepAssist`、`SweepOneUnsweptRegionForAssist` 和吞吐变化；此时再决定是否调整后台 sweep 推进或 assist debt。
4. **不要先调大堆或降低 GC 次数。** 原始结果中 Java 的 RSS 更高、MSGC 的 CPU 更高，说明当前最直接的收益来自降低每请求 JSON 分配与复制，而不是放宽内存上限。

## 7. 2026-08-06 raw-copy 实施与验证

### 7.1 实现

`cangjie_stdx` 的 `JsonReader` 新增公开 API：

```cangjie
public func copyValueTo(writer: JsonWriter): Unit
```

该 API 先按 reader 的正常状态机确认下一个 token 为 JSON value，然后通过 `JsonWriter` 的内部 raw-value 写入口逐字节写出 scalar、string、array 或 object。对象和数组沿用 reader 的对称分段扫描与转义处理；不创建完整 value 的 `Array<Byte>` 或 `String`。API 只接受 value，不能在 reader 尚未读取 object field name 时调用，以保证 writer 的对象状态和逗号/冒号处理正确。

case25 的 `copyRawValue` 已改为：

```cangjie
func copyRawValue(reader: JsonReader, writer: JsonWriter): Unit {
    reader.copyValueTo(writer)
}
```

它被 root 的未修改字段、event 普通字段、`attrs` 的普通字段，以及 `product_ids` 的原有数组元素共同调用；因此这些路径均不再经过 `readValueBytes()` 或 `String.fromUtf8()`。

### 7.2 构建、安装与功能验证

使用 MSGC SDK 环境对 stdx 执行 release build，并将产物安装到：

`/Users/lipeisheng/code/cangjie/cangjie/cangjie_compiler_region_mark/output/msgc`

验证均使用安装后的 `stdx.encoding.json.stream` 动态库，Cangjie 编译明确使用 `-O2 --gc-mode=marksweep`：

| 验证项 | 结果 |
| --- | --- |
| 新增 `raw_json_stream_copy.cj` 回归用例 | 通过；嵌套 object/array、string、number、bool、null 的原样复制输出正确 |
| case25 `test_stream_transform.cj` 语义用例 | 通过；字段变换、追加字段和流式输出语义保持正确 |
| case25 实际 MSGC 冒烟 | 通过；`payload=100K`、`c=100`、`persistent=512MB`、10 s，`24,597 ok / 0 fail`，报告吞吐 `2,430 req/s` |

冒烟产物目录：`../result/20260806_512M_c100_100k_HeapGrow1_Scavenge2_compare_4`。该运行仅用于确认安装后的 SDK、真实 HTTP 请求和 marksweep Runtime 链路可用；时长和机器瞬时负载均不同于第 1 节的 60 s 基线，不能把 `2,430 req/s` 作为 raw-copy 的性能增幅结论。

### 7.3 后续量化

下一步应在相同 SDK、512MB 持久集、c100、100K payload、60 s 条件下，对 raw-copy 前后交替多轮运行并重新采样。除成功吞吐外，应同时比较每成功请求 CPU、`String.<init>`/`memmove`/`checkInvalid` 样本、GC 分阶段时间、`MSSweepAssist`、RSS 及输出字节正确性。这样才能区分 raw-copy 去除的业务分配成本与 GC assist 的剩余成本。

## 8. raw-copy 后的 compare_5：剩余吞吐差距与新优化点

### 8.1 正式 60 s 对比结果

结果目录：

`../result/20260806_512M_c100_100k_HeapGrow1_Scavenge2_compare_5`

参数仍为 `payload=100K`、`concurrency=100`、`duration=60s`、`persistent_bytes=512MB`、`persistent_workers=8`；MSGC 使用 `cjHeapSize=8gb`、`cjHeapGrowth=1`、`cjScavengeGrowth=2`、`cjMSMinGCThreshold=20mb`，Java 使用 `-Xmx8g`。CopyGC 本轮未执行，因此只比较 MSGC、Go、Java。

| 指标 | MSGC | Java | 解读 |
| --- | ---: | ---: | --- |
| 成功吞吐 | 2,727 req/s | 4,250 req/s | MSGC 为 Java 的 64.2%；Java 高 55.9% |
| 成功请求 | 164,070 | 255,129 | 两端均为 0 失败 |
| 平均 CPU | 460.92% | 462.02% | 两端均已使用约 4.6 个核，不存在 MSGC CPU 未跑满 |
| 每成功请求 CPU | 1.690 ms | 1.087 ms | MSGC 单请求多消耗 55.5% CPU，是吞吐差距的直接表现 |
| 平均 RSS | 1,523.22 MB | 1,859.24 MB | Java 多使用约 336 MB；MSGC 不受 RSS 或软堆上限压制 |
| GC 次数 / 总时间 | 213 / 2.462 s | 270 / 1.027 s | MSGC GC 有额外成本，但不足以解释每请求 0.603 ms 的 CPU 差 |

与 raw-copy 前的 `compare_2` 相比，MSGC 的绝对吞吐从 1,982 提升至 2,727 req/s；但 Java 同期也从 2,643 提升至 4,250 req/s。两个运行窗口的机器负载不同，不能把绝对值差异直接归因于 raw-copy；更重要的是 Java/MSGC 的同轮吞吐比从 1.334 增至 1.559。因此，当前结果不能证明 raw-copy 已带来端到端相对收益，必须以相同机器状态下的交替 A/B 才能给出结论。

### 8.2 GC 不是当前首因

本轮 MSGC 213 个 cycle 的日志聚合如下：

| 阶段 | 累计时间 |
| --- | ---: |
| ConcurrentMark | 1.185 s |
| `markLoop` | 1.084 s |
| `quiesce` | 0.049 s |
| PreEnumRoots | 0.254 s |
| EnumRoots | 0.295 s |
| PostMark | 0.571 s |
| MarkTermination（全部 STW） | 0.046 s |
| mark assist 活跃时间 | 0.277 s |

parallel sweep 共启动 204 次，平均 `targetHelpers=1.12`、最大 3；每轮发布的 `unsweptRegions` 平均为 22,052、最大为 23,390。sweep/assist 仍有优化空间，但 sample 中 collector 的 `AddRefToMarkBatch` 只有 153 个活跃样本，显著低于 JSON reader/writer 热点。调大堆、减少 GC 或直接改变 sweep helper 策略都只能作为后续 A/B 项，不应替代业务路径优化。

### 8.3 raw-copy 后采样：分配已降，逐字节循环成为热点

为定位 raw-copy 后的路径，额外执行了一次 12 s `sample`。该采样进程会显著扰动 benchmark，采样运行产生的 `compare_7` 吞吐不作为性能数据；以下仅使用其函数热点与调用关系。

非等待态热点的 flat top 样本为：

| 热点 | 样本数 | 含义 |
| --- | ---: | --- |
| `JsonReader.fillInBuf` | 4,928 | 读取 raw-copy / skip 时频繁进入 reader buffer 检查 |
| `JsonReader.skipArray` | 4,102 | client 为验证响应中的末尾 `server_ok`，跳过完整的嵌套数组 |
| `JsonReader.copySymmetrySegment` | 3,246 | 未修改 object/array 的 raw-copy 扫描 |
| `JsonWriter.writeRawByte` | 2,796 | raw-copy 每写一个 byte 都进行函数调用、容量判断和数组写入 |
| `JsonReader.readString` | 506 | 字段名仍被物化为 String |
| `String.toJson` | 499 | 已物化字段名再被重新编码输出 |
| `__dtoa` | 202 | `score`、`credits` 等浮点 JSON 输出 |
| `MarkSweepCollector::AddRefToMarkBatch` | 153 | 低于主要 JSON 热点 |

raw-copy 的目标已达成：该采样中不再以 `readValueBytes -> String.fromUtf8 -> jsonValue` 为热点链路。但现有实现的 `copySymmetrySegment` 对每个输入 byte 执行 `readNext()`、`writeRawByte()` 和结构字符 match；`readNext()` 又会进入 `fillInBuf(1)`。因此它消除了完整 value 的分配，却把大 value 复制转成高频 Cangjie 函数调用和逐字节边界检查。

client 侧还有一条独立的大路径：

```text
runRequest
  -> responseHasServerOk
     -> JsonReader.skip
        -> JsonReader.skipArray
           -> JsonReader.fillInBuf
```

`server_ok` 被服务端追加在响应 root 的末尾；client 为保持 JSON 语义，会读取 root 中每个此前字段并跳过其完整 value。Java 的对应路径也是 `parser.skipChildren()`，但 Jackson 的 UTF-8 scanner 以低层批量扫描实现，Cangjie 当前仍是逐 byte Cangjie 循环。

### 8.4 新的优化优先级

1. **P0：将 raw-copy 改为连续 span 批量复制。** `JsonWriter` 增加不分配的 `writeRawBytes(buffer, offset, length)`；`copySymmetrySegment` 扫描到引号、反斜杠或括号等结构字符前，将普通连续区间一次性复制到 writer buffer。这样保留结构/转义语义，同时移除每 byte 的 `writeRawByte`、容量判断和输入/输出边界调用。
2. **P0：以同一批量 scanner 改造 `skipArray` / `skipObject`。** 这是 client 响应验证的首要收益点，也能提升 stdx 的通用 `JsonReader.skip()` API。必须仍按 JSON string、escape、嵌套层级识别结构，不能以不安全的裸字符串搜索替代语义校验。
3. **P1：为固定字段名增加 raw-name fast path。** 当前 `readName()` 物化 String，随后 `writeName()` 调用 `String.toJson` 重新编码。可为无转义 ASCII 名称提供“匹配 known name + 原样写出”的路径；转义名称和未知名称回退现有 String 逻辑。
4. **P1：收紧 reader buffer fast path。** `JsonReader` 当前 buffer 为 1 KiB，且 `readNext()` 对每 byte 调用 `fillInBuf(1)`。应先内联“仅当 `availLen == 0` 才 refill”的路径，再单独评估 8/16 KiB buffer 对 HTTP/JSON 双层缓冲搬运的影响。
5. **P2：优化浮点 JSON 输出。** `__dtoa` 仍可见；在保持数值文本兼容和正确舍入的前提下，评估更低分配的 Float64 格式化。
6. **P2：业务 JSON 降本后再调 MSGC。** 使用上述优化后的 profile 重新评估 sweep helper 数、assist debt 和 GC threshold；不要在逐字节 JSON 路径仍占主导时先改 collector 策略。

### 8.5 验证要求

P0 改动后必须使用同一 SDK、512MB persistent、c100、100K、60 s 条件进行 raw-copy 前后交替多轮 A/B。报告至少包括：成功吞吐、每成功请求 CPU、RSS、GC 分阶段时间、`unsweptRegions` / assist 指标，以及新的 sample 中 `fillInBuf`、`skipArray`、`copySymmetrySegment`、`writeRawByte`、`String.<init>`、`memmove` 的变化。输出应继续经过 case25 语义测试与真实 HTTP 成功数校验。

## 9. 2026-08-06：P0 批量 raw-copy / skip 实施与验证

### 9.1 实现内容

本轮在 `cangjie_stdx` 的 `stdx.encoding.json.stream` 中完成第 8.4 节的两个 P0 项。

1. `JsonWriter` 新增公开的 `writeRawBytes(value, offset, length)`，并由内部 `appendRawBytes` 负责范围校验、按 writer buffer 剩余空间分段复制。连续普通 JSON 字节最终使用 `Array.copyTo`，落入底层 `memmove`；该 API 不分配中间 `Array<Byte>` 或 `String`。
2. `JsonReader.copySymmetrySegment` 仅扫描 `"`、`\\`、`{`、`[`、`}`、`]` 等会改变 JSON 结构或字符串状态的字节。两个结构字节之间的普通连续 span 通过 `appendRawBytes` 一次性写入；结构字节仍走原有状态机与 `writeRawByte`，因此转义、引号状态及嵌套 object/array 语义不变。
3. `skipArray` 与 `skipObject` 改为共同调用 `skipSymmetrySegment`。它同样批量跳过普通 span，仅在结构字节处读取、切换 string 状态或维护 `JsonStateStack` 深度；不能把该实现等同于裸 `substring` 搜索，因而仍能正确跳过带转义字符串和任意嵌套层级的 JSON value。
4. `readNext` 的 refill 条件收紧为仅在 `availLen == 0` 时调用 `fillInBuf()`，避免普通逐字符路径反复经过 `fillInBuf(1)` 的调用入口。

实现位置：

- `cangjie_stdx/src/stdx/encoding/json/stream/json_reader.cj`：`copySymmetrySegment`、`skipArray`、`skipObject`、`skipSymmetrySegment`、`readNext`；
- `cangjie_stdx/src/stdx/encoding/json/stream/json_writer.cj`：`writeRawBytes`、`appendRawBytes`。

### 9.2 构建与语义验证

stdx 已以 release 配置重建并安装到 Region-scan MSGC SDK：

`/Users/lipeisheng/code/cangjie/cangjie/cangjie_compiler_region_mark/output/msgc`

所有验证程序均显式使用 `-O2 --gc-mode=marksweep`，并链接安装后的 `stdx.encoding.json.stream` 动态库。

| 验证项 | 结果 | 覆盖内容 |
| --- | --- | --- |
| `raw_json_stream_copy.cj` 回归用例 | 通过 | 大于 reader/writer buffer 的 raw byte value、跨 buffer 写入、嵌套 array/object、带转义 string，以及 `skip()` 后继续读取 root 尾部 `server_ok` |
| case25 `test_stream_transform.cj` 语义用例 | 通过 | 真实 stream transform 的字段改写、字段追加及输出 JSON 语义 |
| 安装产物检查 | 通过 | 安装后的 `libstdx.encoding.json.stream.dylib` 包含 `writeRawBytes`、`appendRawBytes`、`skipSymmetrySegment` 符号 |

特别地，`skip()` 的回归用例会跳过含 8 KiB string、嵌套 array/object 与转义引号的 `payload` value，随后读取 root 的 `server_ok=true`。它验证了批量 skip 没有因跨 buffer 或 string 中的结构样式字符提前终止。

### 9.3 60 s 吞吐结果

批量实现后的正式结果目录：

`../result/20260806_512M_c100_100k_HeapGrow1_Scavenge2_compare_8`

参数为 `payload=100K`、`concurrency=100`、`duration=60s`、`persistent_bytes=512MB`、`persistent_workers=8`；MSGC 使用 `cjHeapSize=8gb`、`cjHeapGrowth=1`、`cjScavengeGrowth=2`、`cjMSMinGCThreshold=20mb`。本轮仅运行 MSGC 与 Java。

| 指标 | batch 前 compare_5 MSGC | batch 后 compare_8 MSGC | 变化 |
| --- | ---: | ---: | ---: |
| 成功吞吐 | 2,727 req/s | 3,033 req/s | +11.2% |
| 成功请求 | 164,070 | 182,430 | +11.2% |
| 失败请求 | 0 | 0 | 无回归 |
| 平均 CPU | 460.92% | 417.82% | -9.4% |
| GC 次数 | 213 | 206 | -3.3% |
| GC 总时间 | 2.462 s | 2.535 s | +2.9% |

该结果说明：在同一业务参数下，批量化后 MSGC 的成功吞吐提高约 11%，同时没有正确性失败；GC 总时间没有同步下降，支持本轮收益主要来自 JSON reader/writer 的业务 CPU 降本，而不是 GC 参数变化。

Java 在 compare_8 的同轮结果为 2,875 req/s，而 compare_5 为 4,250 req/s。这一大幅波动与 Java 本身代码变动无关，且两个目录并非严格交替 A/B（compare_5 还包含 Go side，机器后台负载也不受控）。因此不能从该轮得出“MSGC 已稳定超过 Java”或将 Java 下跌归因于本次 stdx 修改；本节只将 MSGC 的 2,727 -> 3,033 作为同参数的单次增益观测，严格结论仍需要随机化的交替多轮 A/B。

### 9.4 批量采样验证与剩余热点

在 batch 后另跑 30 s MSGC，并对 benchmark 执行 12 s `sample`。采样会显著干扰吞吐，采样期的 2,536 req/s 不作为性能结论；它只用于确认实际执行路径。

采样显示：

- `copySymmetrySegment` 的普通 span 已进入 `JsonWriter.appendRawBytes`，其下可见 `_platform_memmove`；`writeRawByte` 仍存在，但只对应引号、反斜杠和括号等结构字节，符合设计预期。
- client 的 `responseHasServerOk -> JsonReader.skip -> skipSymmetrySegment` 已替代旧 `skipArray` 逐字节执行体。`skipSymmetrySegment` 仍是高位路径，因为每个响应都必须从 root 开头按 JSON 结构扫描约 100K 内容才能确认末尾 `server_ok`；其样本中还可见网络 `fill/read`，说明读取响应体本身也是该路径的一部分。
- `CJ_JSON_FloatPrint -> snprintf/vsnprintf -> __dtoa` 仍处于 server transform 热点；在 raw-copy/skip 批量化后，它是下一类明确的 JSON CPU 优化对象。

结论：本轮 P0 已消除 raw-copy 对普通内容的“每 byte writer 调用 + 容量判断 + 数组边界检查”，并将通用 skip 改为同类批量扫描。剩余首要热点不是旧的中间 value 分配，而是仍不可避免的结构扫描、响应体读取、字段名 String 往返和浮点文本格式化。

### 9.5 后续测量建议

下一轮性能判断应固定只有 MSGC 与 Java 两个 side，保持 payload、持久集、端口和 SDK 不变，随机化 `A-B-B-A` 或 `B-A-A-B` 顺序，各至少三轮。以每轮成功吞吐和每成功请求 CPU 的中位数报告 batch 前后差异；同时归档 `sample`、GC phase 汇总和 RSS，避免单次机器负载把 Java/MSGC 相对结论误判为代码收益。

## 10. compare_13：batch 后的新增热点与后续优化项

### 10.1 本轮正式结果

结果目录：

`../result/20260806_512M_c100_100k_HeapGrow1_Scavenge2_compare_13`

参数为 `payload=100K`、`concurrency=100`、`duration=60s`、`persistent_bytes=512MB`、`persistent_workers=8`、`cjHeapSize=8gb`、`cjHeapGrowth=1`、`cjScavengeGrowth=2`、`cjMSMinGCThreshold=20mb`；本轮四侧均执行。MSGC 编译使用拆分后的 `msgc/stream_transform.cj`，即仍使用 `JsonReader.copyValueTo`。

| 指标 | MSGC | Java | 解读 |
| --- | ---: | ---: | --- |
| 成功吞吐 | 2,512 req/s | 3,283 req/s | MSGC 低 23.5%；Java 高 30.7% |
| 成功请求 | 151,083 | 197,117 | 两端均为 0 失败 |
| 平均 CPU | 356.67% | 377.31% | Java 使用略多 CPU，却完成更多请求 |
| 每成功请求 CPU | 1.420 ms | 1.149 ms | MSGC 单请求多消耗 23.6% CPU |
| 平均 RSS | 1,600.44 MB | 1,842.62 MB | MSGC 内存更低，非内存上限压制 |
| GC 次数 / 总时间 | 182 / 2.406 s | 228 / 0.939 s | MSGC 有额外 GC 成本，但不能解释全部 CPU 差 |

GC 总时间差约 1.47 s，仅占 60 s 墙钟约 2.4 个百分点；因此本轮仍应优先处理 JSON 业务路径，而不是先调整 GC helper、assist 或堆大小。

### 10.2 采样方法与对照证据

为定位 compare_13 后的热点，使用同参数的 MSGC 30 s 短跑抓取 12 s macOS `sample`；采样期吞吐不用于与正式 60 s 结果比较。Java 侧抓取 5 次 `jstack`；HotSpot JIT 后 native `sample` 未能在进程退出前附加，因此 Java 结论以这 5 次 Java 级栈为准。

MSGC sample 的代表性调用链：

```text
server: transformRequest -> transformEvents -> transformEvent / transformUser / transformAttrs
client: runRequest -> responseHasServerOk -> JsonReader.skip -> skipSymmetrySegment
```

一个 client worker 的聚合栈中，`responseHasServerOk` 有 697 个活跃样本，其下
`skipSymmetrySegment` 为 453 + 156 个样本；后者的 138 个样本继续落在
`HttpNormalBodyProvider.read -> BufferedReader.fill -> TcpSocket.read`。这说明 response
校验同时承担了完整 body 的读取和 JSON 结构扫描，不能仅把它视为一个轻量 Bool 判断。

Java 的 5 次 jstack 中，`JacksonStreamTransformer.transform` 出现 266 个栈帧，
`JsonGenerator.copyCurrentStructure` 出现 38 个栈帧，`hasServerOk -> skipChildren` 出现
7 个栈帧；Java 也执行完整 JSON copy/skip，只是 Jackson 的 UTF-8 parser/generator 路径更低层。

### 10.3 P0：补齐 scalar string / scalar value 的批量 raw-copy

第 9 节批量化了 object/array 使用的 `copySymmetrySegment`；sample 已能看到：

```text
copySymmetrySegment -> JsonWriter.appendRawBytes -> _platform_memmove
```

但 scalar 路径仍未批量化：

- `copyValue()` 对 number/bool/null 的每个普通 byte 调用 `writeRawByte`；
- `copyString()` 对每个 string byte 调用 `readNext + writeRawByte`，仅对反斜杠做额外处理。

应将两者改为与 `copySymmetrySegment` 相同的 span scanner：string 只把 `"` 和 `\\`
作为边界，scalar 扫描至分隔符或 whitespace，普通连续 span 均走 `appendRawBytes`。这保持
JSON 转义和 token 边界语义，并补齐 `copyValueTo` 尚未覆盖的 raw-copy 性能缺口。

### 10.4 P0：浮点读写去除重复格式化、解析和逐位 refill

MSGC sample 在业务 transform 中反复命中：

```text
CJ_JSON_FloatPrint -> snprintf_s("%f") -> vsnprintf -> __dtoa
```

case25 每个 event 修改 `score` 与 `user.attrs.credits`；100K payload 含约 85 个 event，
即每请求约 170 次 Float64 读写。当前 `Float64.toJson` 在 native `%f` 格式化后，又将刚写入
的 byte span 转为 `String`、调用 `Float64.parse()` 检查 NaN/Inf，之后再删除尾随 0 和小数点。

建议分两步实施：

1. 在格式化前直接以 `isNaN()/isInf()` 校验，去除 `String.fromUtf8 -> Float64.parse` 回读校验；
2. 评估采用 shortest-roundtrip 的 native 浮点格式化（如 `to_chars`/Ryu），替代 `%f` 与尾随 0
   删除循环，同时保持 JSON 数值语义和正确舍入。

此外，`JsonReader.readFloatEx` 的数字循环每消费一位都会调用一次 `fillInBuf()`，即使 buffer
尚有可读数据；应改为仅当 `availLen == 0` 时 refill。该变化与第 9 节的 `readNext` fast path
相同，但覆盖的是 Float64 解析的独立循环。

### 10.5 P1：字段名的 raw match / raw copy

MSGC transform 对每个字段均为：

```text
readName() -> String -> 名称比较 -> writeName(String) -> String.toJson()
```

sample 中 `JsonReader.readName/readString` 与 `JsonWriter.writeName/String.toJson` 都持续活跃。
100K payload 内每个 event 重复大量字段名，这会放大 String 分配、UTF-8 decode 和重新 escape/encode。

建议提供两组互补 API：

1. `readNameEquals`（或等价 API）：以无转义 ASCII bytes 匹配已知字段，如 `seq`、`score`、`user`；
   命中时不创建 String；
2. `copyNameTo(writer)`：未知字段直接把原始 JSON name span 写入 writer，避免 decode 后再 encode。

转义名称、非 ASCII 名称及未命中的字段仍回退当前 `readName()/writeName()`，以保证通用 JSON 语义。

### 10.6 协议级候选：避免扫描尾部 `server_ok`

`server_ok` 当前在 response root 的末尾追加。client 为读取它，必须从 root 开头调用 `skip()` 跳过
每个此前字段和完整嵌套 value；第 10.2 节的 `skipSymmetrySegment` sample 表明这是实质性 CPU 路径。

可作为单独的协议 A/B 项评估：

- 将 `server_ok` 放在 root 的第一个字段；或
- 通过 HTTP response header 返回成功标志。

这不是单纯 stdx/runtime 优化：它会改变 JSON 字段顺序或 HTTP 协议，必须同步修改 MSGC、CopyGC、
Go、Java 与语义测试，并把结果标为“协议级优化”，不可与 stdx CPU 优化的收益混合归因。若仍需
复用 HTTP 连接，client 在提前得到成功标志后还应确认 body drain/close 的连接复用语义。

### 10.7 优先级结论

建议实施和验证顺序：

1. scalar string/value 的 batch raw-copy；
2. Float64 读写路径；
3. raw-name match/copy；
4. 单独开启协议 A/B 验证 `server_ok` 前置或 header；
5. 在以上业务路径降本后，再重新评估 MSGC sweep assist、mark helper 和 GC threshold。

每项都应以固定的 MSGC/Java 两 side、`A-B-B-A` 或 `B-A-A-B` 顺序、至少三轮 60 s 运行验证，报告成功吞吐、每成功请求 CPU、RSS、GC 分阶段时间和 sample 热点变化。

## 11. 2026-08-06：scalar raw-copy、浮点与 raw-name 实施验证

本章实施第 10.3--10.5 节的三项 stdx/用例级优化。改动已 release 重建并安装到
Region-scan MSGC SDK：

`/Users/lipeisheng/code/cangjie/cangjie/cangjie_compiler_region_mark/output/msgc`

所有 Cangjie 验证和 case25 MSGC benchmark 均显式使用 `-O2 --gc-mode=marksweep`。

### 11.1 实现内容

#### scalar string / scalar value 批量 raw-copy

`JsonReader.copyValueTo` 先前只对 object/array 进入批量化的 `copySymmetrySegment`；
string 与 scalar 仍逐 byte 调用 reader/writer。

- `json_reader.cj:copyString` 现在只把 `"`、`\\` 作为边界；两者之间的普通连续 span 一次调用
  `JsonWriter.appendRawBytes`。转义字符仍按 JSON 语义逐个读取其后一个 byte。
- `json_reader.cj:copyValue` 对 number/bool/null 以 `,`、`]`、`}` 或 whitespace 作为边界，
  其余连续 span 一次拷贝；保持原有对 scalar 空白的忽略和对分隔符的不消费语义。

因此，长 scalar string 和普通 scalar 不再在每个 byte 上承担 `readNext + writeRawByte` 的调用、
容量判断和边界检查。

#### Float64 读写路径

- `JsonReader.readFloatEx` 在消费每个数字后不再无条件调用 `fillInBuf()`，仅当 `availLen <= 0`
  时 refill；常见短数字因此不再执行多余的 buffer 检查调用。
- `Float64.toJson` 在 native `CJ_JSON_FloatPrint` 前用 `isNaN()/isInf()` 直接拒绝非法值，移除了
  旧路径 `String.fromUtf8(已格式化字节) -> Float64.parse()` 的二次分配和解析。

本轮没有改动 native `CJ_JSON_FloatPrint -> snprintf/vsnprintf -> __dtoa`；它仍是后续可独立评估的
浮点格式化热点。

#### 字段名避免 String 物化和再编码

新增 stream API：

| API | 用途与回退语义 |
| --- | --- |
| `JsonReader.nextNameEquals(Array<Byte>)` | 不消费输入地匹配短、无转义且位于当前 reader buffer 的字段名；跨 buffer 或转义 name 返回 `false`，不会破坏后续通用处理。 |
| `JsonReader.copyNameTo(JsonWriter)` | 将已编码的输入 name 直接写到 writer，不创建 decoded `String`；`:` 前的 whitespace 按 `writeName` 语义规范化。 |
| `JsonWriter.writeRawName(Array<Byte>, offset, length)` | 写入无需转义的静态协议字段名，避免 `String.toJson()` 重新 escape/encode。 |
| `JsonReader.skipName()` | 公开无 String 分配的 name 消费路径，供只需要其 value 的 client 使用。 |

case25 的 `msgc/stream_transform.cj` 已全面使用这些 API：输入的 `counters`、`events`、`seq`、
`score`、`user`、`attrs`、`custom`、`product_ids` 等均先 raw-match、再 raw-copy；未知字段也直接
`copyNameTo + copyValueTo`。新增的 `server_marker`、`processed_seq`、`server_ok` 改用
`writeRawName`。client 的 `responseHasServerOk` 用 raw match + `skipName`，不再对每个 response
字段调用 `readName()` 分配 String。

### 11.2 构建与功能验证

stdx 使用 release 配置构建并安装；安装后以该 SDK 编译回归程序并链接
`libstdx.encoding.json.stream.dylib`。回归用例 `cangjie_stdx/test/raw_json_stream_copy.cj` 通过，
覆盖：

- 超过 8 KiB、跨 reader buffer 的 raw name/string/scalar copy；
- number（含指数）、bool、null 的 scalar copy；
- raw static name 写出；
- Float64 `-12.5`、`0.125`、`3e2` 读取与写出；
- 嵌套 object/array、转义 string 与通用 `skip()` 的既有跨 buffer 语义。

此外，case25 的最终 benchmark `_17` 编译成功并完成 `154,928` 个成功请求、`0` 失败，验证了
raw-name 路径覆盖真实 HTTP request transform 与 response `server_ok` 检查。

### 11.3 严格同参 60 s 效果

基线为本报告第 10 章的 `compare_13`；优化后结果为：

`../result/20260806_512M_c100_100k_HeapGrow1_Scavenge2_compare_17`

两轮相同的业务与 Runtime 参数为：`payload=100K`、`concurrency=100`、`duration=60s`、
`persistent_bytes=512MB`、`persistent_workers=8`、`cjHeapSize=8gb`、`cjHeapGrowth=1`、
`cjScavengeGrowth=2`、`cjMSMinGCThreshold=20mb`，均只运行 MSGC。两轮端口也均由 18230 回退为
18231；因此不存在端口选择差异。

| 指标 | 优化前 compare_13 | 优化后 compare_17 | 变化 |
| --- | ---: | ---: | ---: |
| 成功吞吐 | 2,512 req/s | 2,575 req/s | +2.5% |
| 成功请求 | 151,083 | 154,928 | +2.5% |
| 失败请求 | 0 | 0 | 无回归 |
| 平均 CPU | 351.00% | 324.54% | -7.5% |
| 每成功请求 CPU | 1.397 ms | 1.260 ms | -9.8% |
| 平均 RSS | 1,620.76 MB | 1,586.93 MB | -2.1% |
| GC 次数 | 182 | 172 | -5.5% |
| GC 总时间 | 2.406 s | 2.203 s | -8.4% |

吞吐的单轮增益为约 2.5%，应视为积极但仍需多轮 A/B 验证的结果；CPU/request、GC 次数和 GC
总时间同时下降，方向与减少 JSON name/String 分配及 scalar 逐 byte 调用一致。由于两轮并非
随机化交替执行，不能把这些数值作为稳定的最终收益承诺。

曾额外运行 `_16`（`persistent_bytes=512MB` 但 `cjHeapSize=512M`）得到 2,752 req/s；其 heap
参数与基线 8gb 不同，故**不纳入**上表或本轮优化归因。

### 11.4 剩余热点与后续建议

本轮已覆盖第 10.3--10.5 节中无需协议改动的低风险项。下一优先级仍是：

1. 对 `CJ_JSON_FloatPrint` 评估 shortest-roundtrip 格式化实现，替代 `%f`/尾零裁剪；
2. 协议级 A/B：将 response `server_ok` 前置或迁到 header，以消除 client 对完整 100K response 的
   `skipSymmetrySegment` 扫描；
3. 按第 10.7 节的 `A-B-B-A` 顺序至少三轮复测，报告中位数吞吐和每请求 CPU，再判断本章收益的
   稳定幅度。

## 12. compare_24：raw-copy 后的剩余差距与下一轮优化方向

### 12.1 正式对比结果

结果目录：

`../result/20260806_512M_c100_100k_HeapGrow1_Scavenge2_compare_24`

参数为 `payload=100K`、`concurrency=100`、`duration=60s`、`persistent_bytes=512MB`、
`persistent_workers=8`、`cjHeapSize=8gb`、`cjHeapGrowth=1`、`cjScavengeGrowth=2`、
`cjMSMinGCThreshold=20mb`。MSGC 已包含第 11 章的 scalar raw-copy、浮点 read fast path 和 raw-name
改动；两端均为 0 失败。

| 指标 | MSGC | Java | 解读 |
| --- | ---: | ---: | --- |
| 成功吞吐 | 5,073 req/s | 5,806 req/s | MSGC 低 12.6%；Java 高 14.5% |
| 成功请求 | 305,088 | 348,503 | 差 43,415 个成功请求 |
| 平均 CPU | 671.86% | 662.76% | MSGC 略多使用 0.09 核 |
| 每成功请求 CPU | 1.325 ms | 1.142 ms | MSGC 多消耗约 16.0% CPU |
| 平均 RSS | 1,607.22 MB | 2,122.84 MB | MSGC 少 24.3% |
| RSS 峰值 | 1,615.94 MB | 2,702.38 MB | MSGC 少 40.2% |
| GC 次数 | 319 | 323 | 两端频次基本相当 |
| GC 总时间 | 3.272 s | 0.940 s | MSGC 多 2.332 s |
| GC 平均 / P95 | 10.26 / 15.66 ms | 2.91 / 4.91 ms | MSGC 单次 GC 仍更长 |

结论首先是：这不是 CPU 未跑满，也不是 RSS 或机器内存限制压制 MSGC。MSGC 平均 CPU 已略高于 Java，
但每个成功请求仍多消耗约 0.183 ms CPU。GC 总时间差最多只能解释 60 s 墙钟中的约 3.9 个百分点，
无法单独解释 14.5% 的吞吐差距；下一轮应继续优先降低 JSON/HTTP 业务路径的 CPU 与分配，而非先调
HeapGrow、Scavenge 或 GC helper 数量。

### 12.2 补充采样方法与可信边界

为定位 compare_24 后的热点，额外以相同 MSGC 参数运行 45 s 短跑，并在稳定期抓取 15 s macOS
`sample`。该短跑仅用于调用栈定位，因采样本身会扰动吞吐，**不以其吞吐替代 compare_24 的正式结果**。

Java 侧以相同参数执行，并在运行期抓取 5 次 `jstack`。Java 栈用于确认两端的处理结构，不将该带
`jstack` 干扰的短跑吞吐用于比较。栈中可见 Java 服务端的
`UTF8StreamJsonParser.nextToken -> JsonGenerator.copyCurrentStructure`，以及客户端
`hasServerOk -> parser.skipChildren`；即 Java 同样会做完整 JSON copy 和尾部 `server_ok` 校验，
但 Jackson 的 UTF-8 parser/generator 经 JIT 后的执行路径更紧凑。

### 12.3 P0：responseHasServerOk 仍完整结构扫描整个约 100K response

本轮 MSGC sample 最明确的 client 热点为：

```text
runRequest
  -> responseHasServerOk                         1738 active samples
    -> JsonReader.skip                            1727
      -> JsonReader.skipSymmetrySegment           1071 + 372
        -> HttpNormalBodyProvider.read
          -> BufferedConn.read -> BufferedReader.fill -> TcpSocket.read
```

`server_ok` 在 root object 的最后一个字段。client 为找到它，必须跳过此前的完整 `counters`、`events`
及其嵌套 value；这条路径同时承担 response body 的读取和 JSON 结构验证，不是轻量 Bool 判断。

现有 `skipSymmetrySegment` 已对普通连续字节做 span 扫描，但仍有两个剩余瓶颈：

1. `JsonReader` 固定只使用 1 KiB input buffer。100K response 会产生大量 refill，并在 sample 中直接
   体现为 HTTP body provider、buffered reader 和 socket read；
2. `skipString` 仍通过 `readNext()` 逐 byte 跳过 string。并且 `skipSymmetrySegment` 在 string 内仍会
   因 `{`、`}`、`[`、`]` 中断普通 span，尽管它们在 string 内不会影响嵌套状态。

建议的通用 stdx 优化：为 `JsonReader` 提供可选的外部/reusable buffer 或可配置的 8--16 KiB buffer；
将 `skipString` 改为仅扫描 `"`、`\\` 的批量 span，并使 `skipSymmetrySegment` 在 `inString` 时只把
引号与转义作为边界。必须保留跨 buffer、转义字符和嵌套 object/array 的现有语义与回退测试。

协议级候选是将 `server_ok` 移到 root 第一个字段，或作为 HTTP response header 返回。这样可消除
client 对整份 response 的 JSON skip；但它改变输出字段顺序或 HTTP 协议，需同时修改四侧实现和测试，
并作为独立协议 A/B 项，不与 stdx 优化收益混合归因。

### 12.4 P0：服务端 ByteBuffer 从 32 B 扩容到约 100K，放大分配和 Sweep assist

case25 MSGC server 当前对每个请求创建：

```cj
let output = ByteBuffer()
transformRequest(ctx.request.body, output, sequence)
```

`ByteBuffer()` 默认容量为 32 B；而 100K request 的 response 也接近 100K。它以 1.5 倍规则反复
`reserve -> grow`，每次新建 Array 并复制旧内容。sample 已出现下列调用链：

```text
transformEvent -> copyNameTo -> JsonWriter.beforeName
  -> ByteBuffer.write -> ByteBuffer.reserve
    -> NewGenericArray -> RegionSpaceMS::AllocateLargeObject
```

同一热点簇中可见约 100 个 `ByteBuffer.reserve`、约 99 个大 Array 分配活跃样本，以及少量
`MSSweepAssist`/`MSMarkAssist`。这说明 assist 是高频输出临时分配的后果，而不是当前主要根因。

建议优先在 case25 server 中按已知 payload 规模预分配输出，例如 128 KiB，或采用 request
`Content-Length + 固定增量` 作为容量估计并保留安全 fallback。该优化不改变 JSON 语义，预期直接减少
Array 扩容、历史内容拷贝、大对象分配及随后的 GC/assist 工作。

后续通用化方向是扩展 `OutputStream` 的 range-write 快路径 `(buffer, offset, length)`，让
`JsonWriter.flushOutBuf` 与 `ByteBuffer`/HTTP body writer 避免为每个约 4 KiB flush 形成 range
临时对象和额外的整段边界处理；这项涉及 stdlib/stream 接口，应独立评估兼容性与收益。

### 12.5 P1：Float64 JSON 写出仍被 libc snprintf/__dtoa 主导

sample 的 server transform 仍重复命中：

```text
Float64.toJson -> CJ_JSON_FloatPrint
  -> snprintf/vsnprintf -> __dtoa
```

100K payload 含约 85 个 event，每个 event 修改 `score` 与 `credits`，因此约有 170 次 Float64 JSON
写出/请求。第 11 章已移除了格式化后的 `String.fromUtf8 -> Float64.parse` 回读，并减少 reader 的无效
refill；但 native 格式化本身仍走 libc、locale 与 dtoa 路径。

建议将 `CJ_JSON_FloatPrint` 改为正确舍入的 shortest-roundtrip 实现（优先评估 Ryu 或可用平台的
`to_chars`），保留 NaN/Inf 拒绝与现有 JSON 数值语义。该项是 stdlib 通用优化，收益不仅限于 case25。

### 12.6 P1/P2：仍可继续压缩 writer 与字段处理开销

raw-name 和 batch raw-copy 已移除 String 中间物化及普通 byte 的逐字节 writer 调用，但 sample 仍可见
`copyNameTo`、`JsonWriter.beforeName`、`appendRawBytes`、`readNext` 以及 `JsonStateStack.put`。其中：

- `copyNameTo`/静态字段写出属于短字段的频繁通用状态机调用；可评估小型 ASCII name 的专门 fast path，
  但收益应低于前两项；
- `copySymmetrySegment` 对 object/array 的普通内容已走 `_platform_memmove`，剩余的
  `writeRawByte` 主要对应引号、转义和括号等不可直接忽略的结构字节，继续优化应以减少状态切换为目标，
  不可破坏 JSON 结构语义；
- sample 中存在少量 URL parse、request builder 与 HTTP 对象创建，但不是当前 top 路径。应在完成
  JSON/输出分配优化后再评估 request-template 或连接复用专项优化，避免过早改变 benchmark 行为。

### 12.7 实施优先级与验证要求

建议实施顺序：

1. case25 response `ByteBuffer` 预分配，并对成功吞吐、每请求 CPU、Array 分配/assist 与 GC 做 A/B；
2. `JsonReader` 大 buffer/reuse + `skipString`/in-string skip span；
3. `CJ_JSON_FloatPrint` 的 shortest-roundtrip 格式化；
4. `JsonWriter`/`OutputStream` range-write 快路径；
5. 单独进行 `server_ok` 前置或 header 的协议 A/B。

每项保持 `payload=100K`、`concurrency=100`、512 MB persistent baseline 和 8 GB heap 不变；只启用
MSGC/Java 两侧，按 `A-B-B-A` 或 `B-A-A-B` 顺序各至少三轮 60 s。报告成功吞吐中位数、每成功请求 CPU、
RSS、GC phase/assist 时间及 sample 热点变化，避免把机器负载变化、协议改动或采样扰动误判为优化收益。

## 13. 协议级 A/B：将 response server_ok 前置（仅 MSGC 验证）

本章验证第 12.3 节的第一种协议候选：将 response root 的成功标记移到首字段。该项是**协议级优化**，
并非通用 MSGC GC 或 JSON parser 基准；本轮只修改并运行 MSGC，Go、Java、CopyGC 均未改动，因此不能把
本章吞吐直接作为新的跨语言结论。

### 13.1 协议与实现改动

旧 response 布局在完成原始字段变换后，于 root 尾部追加：

```json
{"...payload fields...": "...", "processed_seq": 7, "server_ok": true}
```

新 MSGC response 布局为：

```json
{"server_ok": true, "processed_seq": 7, "...payload fields...": "..."}
```

实现修改如下：

1. `case25_http_complex_json_stream/msgc/stream_transform.cj:transformRequest` 在
   `writer.startObject()` 后立即写入 `server_ok=true` 和 `processed_seq=seq`；原有 request payload
   字段仍以相同 raw-copy/transform 逻辑写出；
2. 同文件 `responseHasServerOk` 只检查 root 的首字段：`nextNameEquals(NAME_SERVER_OK)` 命中后读取
   Bool，不再为尾部字段执行 `reader.skip()`、`skipSymmetrySegment` 或完整 JSON 结构校验；
3. `stdx.encoding.json.stream.JsonReader` 新增 `discardRemaining()`。它先丢弃 reader 已经预读到的
   buffer 内容，再复用该 buffer 反复 `inputStream.read()` 到 EOF。故 client 仍消费完整 HTTP body，
   可以安全归还/复用连接；区别仅在于剩余 body 不再按 JSON token、字符串和嵌套结构解析。

第 3 项需要 stdx release 重建并安装至 Region-scan MSGC SDK。没有修改 Runtime GC、`benchmark.cj`、
Go、Java 或 CopyGC 的实现。

协议含义：MSGC client 现在要求首字段是 `server_ok`。若它连接仍使用旧尾部布局的服务端，会将 response
判为失败。因此未来若恢复四侧公平对比，Go、Java、CopyGC 必须同步调整各自 server 的字段顺序和 client
的首字段校验；否则它们仍承担完整 response JSON scan，结果会混入协议差异。

### 13.2 测试驱动验证与连接消费验证

先更新 case25 transform 测试的期望 JSON 顺序，并加入“尾部 `server_ok=true` 必须返回 false”的断言；
旧实现按预期失败。随后新增 stdx 回归用例：构造一个在首字段 `server_ok=true` 后仍含 8,194 B payload
的 `ByteBuffer`，读取成功标记后调用 `discardRemaining()`，断言底层 `input.remainLength == 0`。
在新增 API 前，该用例因 `discardRemaining` 不存在而按预期编译失败。

实现并安装后，下列两个测试均以 MSGC SDK 的 `-O2 --gc-mode=marksweep` 编译、执行通过：

- `cangjie_stdx/test/raw_json_stream_copy.cj`：覆盖跨 reader buffer 的 raw-copy/skip，以及本次新增的
  大 body drain；
- `case25_http_complex_json_stream/test_stream_transform.cj`：覆盖新 response 顺序、有效/无效成功标记及
  尾部标记拒绝。

### 13.3 严格同参 MSGC 配对结果

为排除 compare_24 与本轮机器状态不同的影响，使用同一台机器连续执行两个 MSGC-only 60 s run。两轮均为
`payload=100K`、`concurrency=100`、`persistent_bytes=512MB`、`persistent_workers=8`、`cjHeapSize=8gb`、
`cjHeapGrowth=1`、`cjScavengeGrowth=2`、`cjMSMinGCThreshold=20mb`，且均为 0 失败。

| 指标 | 旧协议（尾部 server_ok） | 新协议（首字段 server_ok） | 变化 |
| --- | ---: | ---: | ---: |
| 结果目录 | `compare_27` | `compare_28` | — |
| 成功吞吐 | 4,761 req/s | 5,446 req/s | **+14.39%** |
| 成功请求 | 286,354 | 327,508 | +14.37% |
| 平均 CPU | 593.21% | 573.15% | -3.4% |
| 每成功请求 CPU | 1.246 ms | 1.052 ms | **-15.53%** |
| 平均 RSS | 1,626.27 MB | 1,639.32 MB | +0.8% |
| GC 次数 | 298 | 334 | +12.1% |
| GC 总时间 | 3.304 s | 3.874 s | +17.2% |
| 每成功请求 GC 时间 | 11.54 us | 11.83 us | 基本持平 |

结果表明吞吐增益不是以少处理 response body 或连接泄漏换来的：两轮成功/失败结果正确，新增测试确认了
底层 body drain；RSS 基本持平。新协议单位请求 CPU 降低 15.5%，与第 12.3 节中
`responseHasServerOk -> skip -> skipSymmetrySegment` 的高位样本一致。GC 总时间和次数随完成请求数增长，
归一化到单请求后维持约 11--12 us，未显示新的 GC 负担。

本 A/B 仅各运行一轮，14.39% 应视为方向明确的单次配对收益；若要作为最终性能结论，应按第 12.7 节执行
至少三轮交替顺序复测。后续若同步到 Go/Java/CopyGC，应把“首字段 server_ok + raw drain”作为共同协议
基线，再重新抓取 MSGC/Java 对比样本。

## 14. 输出缓冲预分配与 JsonWriter range-write 实施（MSGC）

本章实施第 12.4 节提出的输出缓冲预分配，以及第 12.4 节末尾提出的
`JsonWriter`/`OutputStream` range-write 快路径。两项均不改变 JSON 字段、数值、response
顺序或 HTTP 协议；仅减少服务端 response 生成过程中的临时 Array、扩容和复制。

### 14.1 实现内容

#### 14.1.1 根据 request body size 预分配 response `ByteBuffer`

case25 MSGC server 原先每个请求以默认 32 B 创建输出：

```cj
let output = ByteBuffer()
```

现改为：

```cj
let output = ByteBuffer(initialResponseCapacity(ctx.request.bodySize))
```

`initialResponseCapacity` 对有效 `Content-Length` 返回 `bodySize + 8 KiB`；无法取得或长度不合法时使用
8 KiB fallback。100K request 的 response 与 request 接近，该策略避免默认 32 B buffer 以 1.5 倍规则反复
`reserve -> grow`，并避免每轮扩容时分配新 Array 和复制已写入的历史内容。

#### 14.1.2 以 range-write 消除 JsonWriter 4 KiB flush 的 range 临时 Array

stdlib 的 `OutputStream` 新增：

```cj
func write(buffer: Array<Byte>, offset: Int64, length: Int64): Unit
```

默认实现保留原有兼容语义；`ByteBuffer` 的实现以 `buffer.copyTo` 直接复制指定区间，不生成中间 Array。
`BufferedOutputStream` 和 `MultiOutputStream` 同步透传该区间，避免其自身 flush/转发重新切片。

stdx 的 `JsonWriter.flush` 与 `flushOutBuf` 原先分别调用：

```cj
out.write(buffer[0..curPos])
```

现改为：

```cj
out.write(buffer, 0, curPos)
```

因此每个约 4 KiB writer flush 到本用例的 `ByteBuffer` 时不再创建 range 临时对象，也减少了一次范围数组的
分配和复制。

涉及文件：

- `stdlib/libs/std/io/stream.cj`：通用 `OutputStream` range-write 默认接口；
- `stdlib/libs/std/io/byte_buffer.cj`：`ByteBuffer` 的直接范围复制；
- `stdlib/libs/std/io/buffered_output_stream.cj`、`multi_output_stream.cj`：范围转发和 buffered flush；
- `cangjie_stdx/src/stdx/encoding/json/stream/json_writer.cj`：两个 flush 点改用 range-write；
- `case25_http_complex_json_stream/benchmark.cj`、`msgc/stream_transform.cj`：response 容量估算与 server 接入。

### 14.2 构建和功能验证

最终按完整 SDK 路径执行 `runtime/build/scripts/incremental_rebuild_msgc_sdk.py`，显式传入当前的
compiler/runtime/stdlib/stdx 根目录及 OpenSSL 库目录。脚本成功重建并安装 compiler、Region-scan MSGC
runtime、stdlib 和 stdx 至：

```text
/Users/lipeisheng/code/cangjie/cangjie/cangjie_compiler_region_mark/output/msgc
```

验证均明确使用已安装 SDK 的 `cjc -O2 --gc-mode=marksweep`：

1. `cangjie_stdx/test/raw_json_stream_copy.cj` 新增 range-write 回归：对容量为 128 B 的 `ByteBuffer`
   写入 `"xxhello!"[2..7)`，断言输出为 `hello` 且 `capacity` 仍为 128；测试通过；
2. `case25_http_complex_json_stream/test_stream_transform.cj` 新增容量估算断言：
   `99,572 B -> 107,764 B`，无 body size -> `8,192 B`；同时保留完整 request/response transform 与
   `server_ok` 语义验证；测试通过；
3. case25 MSGC benchmark 成功以 `-O2 --gc-mode=marksweep` 编译、启动并完成 60 s 运行，0 失败请求。

### 14.3 吞吐验证结果

本次正式运行目录：

`../result/20260806_512M_c100_100k_HeapGrow1_Scavenge2_compare_29`

参数与第 12.1 节的 MSGC compare_24 一致：`payload=100K`、`concurrency=100`、`duration=60s`、
`persistent_bytes=512MB`、`persistent_workers=8`、`cjHeapSize=8gb`、`cjHeapGrowth=1`、
`cjScavengeGrowth=2`、`cjMSMinGCThreshold=20mb`；仅运行 MSGC，且均为 0 失败。

| 指标 | 改动前 compare_24 | 改动后 compare_29 | 变化 |
| --- | ---: | ---: | ---: |
| 成功吞吐 | 5,073 req/s | 6,002 req/s | **+18.31%** |
| 成功请求 | 305,088 | 360,944 | +18.31% |
| 平均 CPU | 658.6% | 569.6% | -13.51% |
| 每成功请求 CPU | 1.298 ms | 0.949 ms | **-26.89%** |
| 平均 RSS | 1,584.4 MB | 1,448.8 MB | -8.56% |
| RSS 峰值 | 1,615.9 MB | 1,477.4 MB | -8.57% |
| GC 次数 | 319 | 271 | -15.05% |
| GC 总时间 | 3.272 s | 2.507 s | -23.38% |
| 每成功请求 GC 时间 | 10.725 us | 6.946 us | -35.24% |

吞吐提升的直接证据与预期一致：默认 32 B 输出 buffer 的反复扩容被去除，writer flush 不再分配 4 KiB
range 临时 Array，因而单位请求 CPU、分配引发的 GC 次数和 GC 时间均下降。平均 CPU 从 6.586 核降至
5.696 核而成功吞吐仍增加，说明收益来自减少业务路径工作，而非增加 CPU 占用或放宽堆限制。

本次 compare_24 是此前同参数结果，并非以两个 SDK 二进制在本轮严格交替执行的 A/B；因此 18.31% 应视为
同配置下的强正向验证，而非消除全部机器负载波动后的最终中位数。若需要发布为稳定结论，应按第 12.7 节以
旧/新版本 `A-B-B-A` 顺序各至少三轮 60 s 复测，并报告中位数与离散度。

## 15. 2026-08-07：纯 C Ryu Float64 JSON 写出实施与验证（MSGC）

本章落实第 12.5 节的浮点 JSON 写出优化。初始候选为 C++17 `std::to_chars`，但目标 SDK 的
macOS 12 deployment target 不提供浮点 `to_chars`，且本次改动明确不能引入 C++17。因此最终实现采用
纯 C 的 Ryu `d2s`，不改变 `CJ_JSON_FloatPrint` FFI ABI，也不修改 Runtime GC、case25 协议、Go 或 Java。

### 15.1 实现内容

原 `CJ_JSON_FloatPrint` 通过：

```text
Float64.toJson -> CJ_JSON_FloatPrint -> snprintf_s("%f") -> libc __dtoa
```

写出固定六位小数后再由 Cangjie 逐字节删除末尾 `0` 与 `.`。除 libc 格式化、locale 处理和多层调用开销外，
该逻辑对 `0.0000001` 会生成 `0.000000`，继而错误裁剪为 `0`。

现改动如下：

1. 在 `cangjie_stdx/src/stdx/encoding/json/stream/native/ryu/` 引入 Apache-2.0 许可的 Ryu 纯 C
   `d2s` 实现及其所需表；`native/CMakeLists.txt` 仅编译 `utils.c` 与 `ryu/d2s.c`，没有 `.cpp` 文件或
   C++17 依赖。
2. `utils.c` 中的 `CJ_JSON_FloatPrint` 保留原 FFI 形状，调用 `d2s_buffered_n` 获取 shortest-roundtrip
   数字，再直接写入 `JsonWriter` 的现有 buffer。适配层将 Ryu 的 `E` 标记规范化为 JSON 常用的 `e`，并仅在
   fixed 表示不长于 scientific 表示时展开小数点；故常用值仍可写为 `300`、`0.125`，而 `1e-7` 保持为短文本
   `1e-7`。
3. `Float64.toJson` 删除旧 `%f` 专用的末尾零裁剪。shortest 结果的末尾零本身可能是有效整数的一部分，
   例如 `300`；继续裁剪会将其破坏为 `3`。
4. 流式 `JsonReader.readFloat` 的指数路径同步修正。Ryu 可以合法生成 `e-7`，但原先的整数指数幂调用会把
   负指数错误算为 0。现在显式调用 `pow(10.0, Float64(exponent))`，并对超出 `Float64` 十进制范围的指数
   保持有限性保护。

涉及文件：

- `cangjie_stdx/src/stdx/encoding/json/stream/native/utils.c`；
- `cangjie_stdx/src/stdx/encoding/json/stream/native/CMakeLists.txt`；
- `cangjie_stdx/src/stdx/encoding/json/stream/native/ryu/*`；
- `cangjie_stdx/src/stdx/encoding/json/stream/json_serializable.cj`；
- `cangjie_stdx/src/stdx/encoding/json/stream/json_reader.cj`；
- `cangjie_stdx/test/raw_json_stream_copy.cj`。

### 15.2 构建与正确性验证

先为 `0.0000001` 增加回归：writer 输出不能为 `"0"`，再由 `JsonReader` 读回并要求 `Float64.toBits()` 完全一致。
旧 `%f` 实现按预期失败；新实现输出 `1e-7` 并通过 exact bit round-trip。该测试同时覆盖了上述 reader 的负指数
路径，而不是只验证字符串非零。

最终使用规定的完整 SDK 脚本构建安装：

```text
runtime/build/scripts/incremental_rebuild_msgc_sdk.py
```

SDK 安装位置为：

```text
/Users/lipeisheng/code/cangjie/cangjie/cangjie_compiler_region_mark/output/msgc
```

以下验证均显式使用已安装 SDK 的 `cjc -O2 --gc-mode=marksweep`，并通过：

1. `cangjie_stdx/test/raw_json_stream_copy.cj`：包括已有 raw-copy/skip 覆盖及新增 shortest 浮点 exact
   round-trip；
2. `case25_http_complex_json_stream/test_stream_transform.cj`：完整 HTTP/JSON 流式 transform 语义；
3. case25 MSGC-only benchmark：编译、启动和 60 s 压测均成功，0 失败请求。

### 15.3 同参数 60 s 吞吐验证

新运行目录：

`../result/20260807_512M_c100_100k_HeapGrow1_Scavenge2_compare`

与第 14.3 节的 compare_29 使用相同用例参数：`payload=100K`、`concurrency=100`、`duration=60s`、
`persistent_bytes=512MB`、`persistent_workers=8`、`cjHeapSize=8gb`、`cjHeapGrowth=1`、
`cjScavengeGrowth=2`、`cjMSMinGCThreshold=20mb`；仅运行 MSGC。

| 指标 | 浮点优化前 compare_29 | Ryu 后本次运行 | 变化 |
| --- | ---: | ---: | ---: |
| 成功吞吐 | 6,002 req/s | 6,505 req/s | **+8.38%** |
| 成功请求 | 360,944 | 391,143 | +8.37% |
| 失败请求 | 0 | 0 | 0 |
| 平均 CPU | 569.6% | 636.7% | +11.78% |
| 每成功请求 CPU | 0.949 ms | 0.979 ms | +3.13% |
| 平均 RSS | 1,448.8 MB | 1,569.3 MB | +8.32% |
| RSS 峰值 | 1,477.4 MB | 1,603.8 MB | +8.56% |
| GC 次数 | 271 | 254 | -6.27% |
| GC 总时间 | 2.507 s | 2.352 s | -6.19% |
| 每成功请求 GC 时间 | 6.946 us | 6.013 us | **-13.43%** |

本次单次验证的成功吞吐增加 503 req/s；完成更多请求的同时 GC 次数和 GC 总时间均下降，归一化后的 GC 时间下降
13.43%，说明新路径未引入额外的 GC 压力。平均 CPU、RSS 及单位请求 CPU 则上升，不能据此声称已有稳定的
CPU 节省；这可能混入机器状态、并发调度和更高完成速率造成的差异。

因此结论是：纯 C Ryu 已在不使用 C++17 的前提下替换 `snprintf/__dtoa` 热路径，并在本次同参数运行中得到
8.38% 的正向吞吐验证，同时修复了小数与负指数的正确性缺陷。若需要作为最终性能结论，应保留优化前 SDK，按
旧/新 `A-B-B-A` 顺序各至少三轮 60 s 运行，报告成功吞吐、单位请求 CPU 和 GC 时间的中位数与离散度。
