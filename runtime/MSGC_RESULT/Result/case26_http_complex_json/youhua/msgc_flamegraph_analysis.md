# Case26 MSGC 全进程热点与火焰图分析

## 1. 采样范围

本次采样针对已完成 JSON 容量优化的 case26 benchmark，配置保持与基准一致：

- payload=100K，concurrency=100，duration=60000ms
- persistent=512MiB/8 workers
- P=10，HEAP_SIZE=20GB
- `cjHeapGrowth=1`，`cjScavengeGrowth=2`
- compiler/runtime/stdlib：`/home/lipeisheng/cangjie/cangjie_compiler_CMSGC/output/msgc`

使用 Linux `perf 5.15.200`，99Hz、DWARF callchain，对 benchmark 全部线程采样约 58 秒。二进制带 debug_info、未 strip；有效样本约 61K，丢失样本为 0。

采样本身的结果为 49,339 请求、818 req/s；无采样的同版本正式结果为 841 req/s，采样扰动约 2.7%。因此这里的百分比用于热点排序，不用于精确计算单请求耗时。

## 2. 产物

- [SVG 火焰图](result/20260808_msgc_flamegraph_c100_100K_Procs10_v2/msgc_cpu_flamegraph.svg)
- [perf.data](result/20260808_msgc_flamegraph_c100_100K_Procs10_v2/perf.data)
- [累计热点报告](result/20260808_msgc_flamegraph_c100_100K_Procs10_v2/perf_hotspots_children.txt)
- [函数自身热点报告](result/20260808_msgc_flamegraph_c100_100K_Procs10_v2/perf_hotspots_self_nochildren.txt)
- [DSO 汇总](result/20260808_msgc_flamegraph_c100_100K_Procs10_v2/perf_by_dso.txt)
- [线程汇总](result/20260808_msgc_flamegraph_c100_100K_Procs10_v2/perf_by_thread.txt)
- [GC 日志](result/20260808_msgc_flamegraph_c100_100K_Procs10_v2/gc.log)

## 3. CPU 按模块和线程分布

### DSO

| 模块 | CPU 样本占比 |
|---|---:|
| `libstdx.encoding.json.so` | **37.30%** |
| `libcangjie-runtime.so` | **32.33%** |
| `libc.so.6` | 14.61% |
| benchmark | 7.89% |
| kernel | 6.31% |
| `libstdx.net.http.so` | 0.78% |

HTTP 库本身只有 0.78%，因此当前差距不能简单归因于 HTTP API；业务进程的主要 CPU 去向是 JSON 和 runtime/对象管理。

### 线程

| 线程 | CPU 样本占比 |
|---|---:|
| `schd-worker` | **78.73%** |
| `benchmark` 主线程 | 8.84% |
| `gc-helper` | 2.70% |
| `gc-main-thread` | 2.35% |
| `gc-pool-t1..t4` | 6.90% |
| `schmon` | 0.49% |

绝大多数业务工作由 `schd-worker` 执行，不能只采主线程；之前 runtime 内置 profiler 只看到 `std.core.sleep`，不适合定位本用例热点。

## 4. 函数自身热点（跨线程合并）

| 函数 | Self 样本占比 | 解释 |
|---|---:|---|
| `parseJsonString` | **7.33%** | JSON 字符串扫描、转义和字符串构造 |
| `parseJsonObject` | **5.94%** | object 字段解析、HashMap 插入路径 |
| `jsonWriteWithoutFormat` | **3.86%** | JSON 树递归序列化 |
| `WriteBuffer.append(Array)` | **3.15%** | 输出 buffer 写入和复制 |
| `memset` | **7.52%** | 对象/数组/buffer 初始化，JSON 和 runtime 共同触发 |
| `memcpy` | **2.11%** | 字符串、数组和输出 buffer 拷贝 |
| `MCC_AcquireRawData` + `ReleaseRawData` | **4.35%** | String/Array rawData 获取与释放 |
| `MRT_GetThreadLocalData` | 2.47% | Cangjie 对象/线程本地 runtime 访问 |
| `TypeInfo::IsSubType` | 2.42% | `JsonValue` 类型匹配/转换 |
| `wyhash` + `checkInvalid` | 2.77% | HashMap key/hash 与字符串检查 |
| Mark batch/`RunRegionTask` | 约 6.56% | 并发 Mark 的对象扫描、任务发布和 batch 操作 |
| Sweep region/Assist | 约 0.98%（self） | Sweep 及 mutator sweep assist |

Self 样本是函数自身 CPU，不包含子函数；累计调用链中，服务端 `JsonValue.fromStr` 约 39.04%，`toStringWithCapacity` 约 25.64%。两者不能相加到 self 表中，但可以说明完整 parser 调用链比单个入口函数更重。

## 5. 重点调用链

### 反序列化

```text
JsonValue.fromStr
  -> parseString
    -> parseJson
      -> parseJsonObject
        -> parseJson / parseJsonArray
          -> parseJsonObject ...
```

累计占比约 39%。输入 JSON 是约 100KB、包含大量嵌套 object/array 的复杂树，因此递归 object/array 解析、字符串构造、HashMap 插入和对象分配同时出现。

本地 `cangjie_stdx_CMSGC` 源码与采样二进制中的函数符号相符；但本次结果 metadata 没有单独记录 stdx 源码 commit，因此以下源码分析属于“高置信度对应”，精确版本仍应以后续构建记录为准。

源码证据：

- `JsonObject` 底层为 `HashMap<String, JsonValue>`；
- `JsonArray` 底层为 `ArrayList<JsonValue>`；
- object/array 初始容量均为 8；
- 转义字符串通过 `strCache` 收集后再 `String.fromUtf8Unchecked()` 构造。

源码：[native.cj](../../../../../../cangjie_stdx_CMSGC/src/stdx/encoding/json/native.cj)、[json_object.cj](../../../../../../cangjie_stdx_CMSGC/src/stdx/encoding/json/json_object.cj)。

### 序列化

```text
serializeServerJson
  -> JsonValue.toStringWithCapacity
    -> jsonWriteWithoutFormat
      -> WriteBuffer.append / appendEscape
        -> CJ_JSON_StringEscapeCharNumGet
```

累计占比约 25.64%，self 热点集中在 `jsonWriteWithoutFormat`、`WriteBuffer.append(Array)`、`appendEscape` 和 `memcpy`。

已经完成的容量优化跳过了 `toString()` 内部的 `asumecapacity()` 全树预扫描，正式吞吐由 792 req/s 提升到 841 req/s。当前序列化剩余重点是输出过程中的 Array/String rawData 获取、escape 扫描和拷贝。

### Runtime/GC

runtime 占 32.33%，但不能全部视为 GC：

- `MCC_AcquireRawData`、`MRT_GetThreadLocalData`、`TypeInfo::IsSubType` 是 JSON 对象/字符串操作的 runtime 支撑；
- Mark batch 和 `RunRegionTask` 才是并发 Mark 的直接成本；
- gc helper/pool/main 合计约 10.7% CPU，说明本轮优化后 GC 仍有开销，但不是最大单一热点；
- `libstdx.net.http.so` 仅 0.78%，网络库函数级 CPU 不是当前第一优先级。

本结论只用于当前 workload，不能外推到所有 payload 或堆配置。

## 6. 可执行的优化方向

按当前火焰图优先级排序：

### 优先级 A：JSON parser 字符串路径

`parseJsonString` 是最大单函数热点（7.33%），建议：

1. `JsonParser` 当前无条件把 `strCache` 初始容量设为 `max(input.size / 4, 64)`。100K 输入每次 parse 约预留 25KB，而 cache 主要只在遇到转义字符串时使用。优先改为懒分配或小初始容量，并只在真正遇到转义时增长；这是 `parseJsonString`、`memset` 和分配热点的共同候选原因。
2. 对无转义字符串继续保持一次构造路径，检查 `parser.data[start..end]` 是否产生 Array slice 拷贝；如果会复制，增加基于 rawData offset/length 的字符串构造接口。
3. 转义路径检查 `strCache.add(all: ...)` 的扩容与复制，按当前字段长度增长，而不是按整个输入固定预留。
4. 减少 `String.fromUtf8Unchecked()`、`rawData()` 的来回转换；对 ASCII-only key/value 做快速路径，避免通用 Rune/转义逻辑。

### 优先级 A：JSON object/array 容量

`parseJsonObject`、`parseJsonArray` 加起来 self 约 6.86%，累计路径约 39%。当前 object/array 初始容量固定为 8；可以从 payload 结构或解析阶段估算字段/元素数量，做 16/32/自适应容量 A/B，重点观察 HashMap resize、ArrayList grow 和 RSS。

### 优先级 B：序列化输出 buffer

当前已经消除了容量预扫描，剩余热点是：

- `WriteBuffer.append(Array)` 3.15%；
- `memcpy` 2.11%；
- `appendEscape`/`CJ_JSON_StringEscapeCharNumGet` 合计约 2.6%；
- `MCC_AcquireRawData/ReleaseRawData` 约 4.35%。

建议让 WriteBuffer 直接接收 raw bytes/指针区间，减少 `String.rawData()` 的 acquire/release；对无转义字符串避免先扫描再复制，或把扫描和写入合并为一次遍历。

还有三个明确的源码级候选：

1. `WriteBuffer.checkAndExpend()` 当前在 `minCapacity == oldCapacity` 时仍会扩容，条件可从 `<` 核验为 `<=`，避免刚好容纳时的无效扩容；
2. compact 序列化路径反复使用 `"{}".toArray()`、`"[]".toArray()`、`"\":".toArray()`，火焰图中 `String.toArray` self 约 1.12%，可以改为静态 byte 常量或逐 byte append；
3. `WriteBuffer` 初始化会把整个 capacity 填成空格，`getAndClear()` 又通过 slice 生成最终数组。当前 case26 的 12.5% 容量余量偏大，可收紧容量公式，并评估 buffer 复用，减少 `memset` 和最终复制。

### 优先级 B：对象类型判断和 runtime 支撑

`TypeInfo::IsSubType` 约 2.42%，来源是大量 `asObject/asArray/asInt/asFloat` 的动态类型匹配。可以在 JSON 内部使用 kind/tag 快速分派，减少频繁 TypeInfo 查询；需要先用独立 microbenchmark 验证正确性和收益。

### 优先级 C：分配初始化

`memset` 约 7.52%，`memcpy` 约 2.11%，包含 JSON buffer、对象数组和 runtime 分配初始化。应结合 allocation count 进一步拆分，避免直接把所有 memset 都归因到 JSON；可验证对象/数组批量初始化、WriteBuffer 初始填充和 raw buffer 复用。

### 暂不优先：HTTP 和 GC

- HTTP DSO 仅 0.78%，先不做 HTTP API 层重构；
- 具名 Mark/Sweep 函数的 self 样本约 7.5%，GC 专用线程合计约 10.7%；二者有重叠，不能相加。mutator 上还有 Mark/Sweep Assist，因此 GC 总预算高于专用线程占比，但 JSON 仍是当前最大、最直接的非 GC 优化面。本轮先不改 GC，避免把收益混在一起。

## 7. 建议的下一轮实验

每轮保持相同 SDK、P10、c100/100K、20GB heap 和 512MiB persistent，至少重复 3 次：

1. `strCache` 从 `input/4` 无条件预留改为懒分配/小容量，单独做 A/B；
2. parser object/array 初始容量：8（当前）、16、32；
3. WriteBuffer exact-fit 扩容条件、静态 byte 常量、rawData/escape 合并路径；
4. 每轮同时保存 20 秒 `CASE26_JSON_PROFILE=1` 阶段计时和一次 60 秒吞吐；
5. 对最优版本再做一次 perf 火焰图，确认热点是否从 JSON 转移到 runtime 或网络。

当前状态是“热点已验证、优化方向已提出”，parser 和 WriteBuffer 方向尚未修改或回归验证，不能提前宣称收益。
