# Case26 DOM JSON：512M CPU/request 基准与 MSGC/Java 采样分析

## 1. 目的与范围

分析 `case26_http_complex_json` 在普通 DOM JSON API（`JsonValue.fromStr` +
`JsonObject.toString`）下的 MSGC 与 Java 吞吐差距，定位 Cangjie JSON 库和
Runtime 的可优化点。

本次采样不是 case25 的流式 JSON 路径；因此不使用 `JsonReader` / `JsonWriter`
的 raw-copy 结论替代本结论。

## 2. 512M、100 并发、100K CPU/request 基准

执行时间：2026-08-07；只运行当前安装的 Region-scan MSGC SDK，不运行 CopyGC、Go
或 Java。该基准作为后续 MSGC 优化的 CPU 效率基线。

| 项目 | 值 |
| --- | --- |
| payload | `100K.json`，99,572 B |
| 并发 / client mode | 100 / `sync` |
| 压测时长 | 60,000 ms |
| 持久对象基线 | 512 MiB，8 workers |
| MSGC | `--gc-mode=marksweep`，`cjHeapSize=16gb`，`cjHeapGrowth=1`，`cjScavengeGrowth=2` |
| 其他 side | 全部关闭，仅 MSGC |

`run_compare.sh` 的 Cangjie runner 每 200 ms 读取 benchmark 进程的 `ps utime` 和
`stime`；最后一个累计值为 367.66 CPU 秒（user 323.71 s + sys 43.95 s）。因此使用
`累计 CPU 秒 / 成功请求数`，而不是会随机器负载波动的瞬时 `%CPU`：

| 指标 | 当前 MSGC 基准 |
| --- | ---: |
| 实际运行时间 | 60.180 s |
| 成功请求 | 92,943 |
| 失败请求 | 0 |
| 报告吞吐 | 1,544 req/s |
| 累计 CPU 时间 | 367.66 s |
| 每成功请求 CPU | **3.956 ms/request** |
| 每 CPU 秒成功请求 | **252.80 success/CPU-s** |

产物目录：

- `MSGC_RESULT/Result/case26_http_complex_json/result/20260807_512M_c100_100k_HeapGrow1_Scavenge2_compare`
- `msgc/stdout.txt`
- `msgc/rss.csv`（第 4 至第 6 列分别为 user CPU、sys CPU、累计 CPU 秒）

历史 `20260805_512M_c100_100k_HeapGrow1_Scavenge2_compare` 仅保留平均 `%CPU`，
不能得到同精度的累计 CPU 时间。以其 60.233 s、78,663 成功请求、平均 CPU 528.24%
估算为约 4.045 ms/request；当前基准约改善 2.2%。该历史估算仅作趋势参考，后续优化
必须与本节的 3.956 ms/request 直接比较。

## 3. 2G、10 并发采样的可复核执行配置

重新执行时间：2026-08-07。

| 项目 | 值 |
| --- | --- |
| 用例 | `case26_http_complex_json` |
| payload | `100K.json`，99,572 B |
| 并发 | 10，sync client |
| 压测时长 | 60,000 ms |
| 持久对象基线 | 2,048 MiB，8 workers |
| MSGC | `--gc-mode=marksweep`，`cjHeapSize=16gb`，`cjHeapGrowth=1`，`cjScavengeGrowth=2` |
| Java | JDK 8u381，`-Xmx16g` |
| 采样 | macOS `/usr/bin/sample`，启动后 8 s，持续 20 s，1 ms 间隔 |

产物目录：

- `MSGC_RESULT/Result/case26_http_complex_json/result/20260807_2048M_c10_100k_HeapGrow1_Scavenge2_compare`
- `msgc/sample_20s.txt`
- `java/sample_20s.txt`

case26 脚本新增了默认关闭的采样开关：

```bash
CASE27_SAMPLE_DURATION_SECONDS=20
CASE27_SAMPLE_DELAY_SECONDS=8
```

未设置时默认值为 `0`，不会改变普通基准的执行路径或结果。

## 4. 2G、10 并发采样结果

| 指标 | MSGC | Java | Java 相对 MSGC |
| --- | ---: | ---: | ---: |
| 总运行时间 | 61.250 s | 60.082 s | -1.9% |
| 成功请求 | 15,013 | 21,797 | +45.2% |
| 报告吞吐 | 245 req/s | 362 req/s | +47.8% |
| 失败请求 | 0 | 0 | - |
| 平均 RSS | 3,051 MB | 3,492 MB | +14.4% |
| 平均 CPU | 263.5% | 279.2% | +6.0% |
| GC 次数 | 62 | 85 | +37.1% |
| GC 总时间 | 8.305 s | 8.154 s | -1.8% |

### 4.1 启动期会放大整段吞吐差距

60 秒总吞吐包含了 Java 的前期高吞吐阶段。10 秒时 Java 已完成 8,479 请求，
MSGC 为 2,629 请求；随后两侧都进入包含持续 GC 的稳定阶段。

用约 10 秒之后至结束的进度计数计算：

| 时间段 | MSGC | Java | Java 相对 MSGC |
| --- | ---: | ---: | ---: |
| 约 10 s 至结束 | 241.8 req/s | 266.2 req/s | +10.1% |

因此，报告中的 `+47.8%` 是完整协议下的有效结果，但不能直接解释为稳定态 DOM
序列化/反序列化慢 47.8%。若要把优化收益稳定地归因到 JSON 库，应在持久对象创建、
预热和 GC 稳定后才开始计数，或单独输出 steady-state 吞吐。

## 5. MSGC 调用栈结论

`sample` 的“top of stack”计数不是严格 CPU 百分比，且包含等待线程；下表只用于
识别持续出现的活跃路径。关键样本如下：

| 路径 | top-of-stack 计数 | 说明 |
| --- | ---: | --- |
| `parseJsonString` | 2,530 | DOM 中大量 key / string 的解析和 String 构造 |
| `parseJsonObject` | 2,244 | JSON 对象解析和字段插入 |
| `HashMap<String, JsonValue>.add` | 1,286 | DOM 对象字段建立 |
| `__dtoa` | 517 | DOM `JsonFloat` 写出走 libc 浮点格式化 |
| `asumecapacity` | 498 | 输出前完整递归容量预估 |
| `jsonWriteWithoutFormat` | 720 | DOM 输出递归遍历 |
| `WriteBuffer.append(Array<Byte>)` | 570 | 字节写入 |
| `WriteBuffer.checkAndExpend` | 548 | 每次 append 的容量检查 |
| `parseJsonNumberSlow` | 278 | 浮点解析；底层可见 `strtod` / `fastParse64` |
| `SweepOneUnsweptRegionForAssist` | 1,067（递归栈） | 分配路径触发 sweep assist |
| `MSMarkAssist` | 271（递归栈） | 高分配率下的 mark assist |

同时出现的 `__bzero`、`MArray::NewArray`、`MObject::NewObject`、`memmove` 说明该
DOM 路径的短命对象和输出缓冲分配较多。

## 6. 与 Java 实现的关键差异

Java 采样在 JDK 8 JIT 代码处大部分显示为 `???`，不能把其函数名作为精确热点。
但 Java 源码和 MSGC 源码可直接确认一个决定性差异：数字 token 的保存方式不同。

- Java 的 `JsonNumber` 保存 `raw: String`。解析时会校验 `Double.parseDouble(raw)`，
  但未修改的数字在 `JsonWriter` 中直接 `output.append(raw)`。
- Cangjie 的 `parseJsonNumberSlow` 会立刻构造 `JsonFloat(Float64)`；DOM 输出时每个
  `JsonFloat` 都要重新格式化。

该 payload 有 1,269 个浮点数、85 个 event。服务端仅修改每个 event 的 `score` 和
`credits`，约 170 个浮点；Java 对剩余约 1,099 个未修改浮点直接复用原 token，而
Cangjie 会重新写出全部 1,269 个浮点。这正对应样本中的 `__dtoa` 热点。

此外，Java 的 writer 使用一次递归和 `StringBuilder` 增长；Cangjie DOM 的 `toString`
先调用 `asumecapacity` 全树遍历，再调用 `jsonWriteWithoutFormat` 写出，全树访问两次。

## 7. 优化项与优先级

### P0：将 Ryu 浮点写出扩展到 DOM JSON

状态：已实施并安装验证。DOM `WriteBuffer.append(Float64)` 已改为直接写入已有
buffer 的 `CJ_JSON_FloatPrint`（Ryu）结果，移除了临时 C 字符串、`strlen`、复制、
释放和去尾零循环。`1e-7` 的 DOM JSON round-trip 回归通过；采样中原来的
`snprintf/vsnprintf/__dtoa` 已消失，改为 `CJ_JSON_FloatPrint -> d2s_buffered_n`。

在 2G、10 并发、100K 的单次复测中，端到端吞吐为 240 req/s，未超过优化前的
245 req/s；因此此项的正确性和分配链路已改善，但不能把它作为 case26 端到端吞吐的
主要收益来源。512M 基准的后续优化改用本文件第 2 节的 CPU/request 指标判断。

实施前，DOM 的 `src/stdx/encoding/json/write_buffer.cj` 使用：

```text
CJ_CORE_Float64ToCPointer -> strlen -> memcpy_s -> free -> 去尾零
```

现已将 Ryu helper 接入 DOM JSON FFI，并使用 32 B 作为安全输出上界。该实现消除了
`__dtoa`、临时 C 字符串、`strlen`、复制、释放和去尾零循环；后续只需评估其余 DOM
解析与缓冲分配路径。

### P0：修正浮点容量估算，提供输入大小驱动的输出容量接口

状态：已实施并安装验证。

- `asumecapacity` 对有原始 token 的 `JsonFloat` 使用精确 token 长度；业务新建的
  `JsonFloat` 使用 Ryu 的 32 B 安全上界，不再固定预留 200 B。
- 新增 `JsonValue.toStringWithCapacity(capacity)`。它以调用方容量直接创建一个
  `WriteBuffer`，跳过 `asumecapacity` 的全树预遍历；负容量会抛出
  `IllegalArgumentException`。
- case26 server 调用 `root.toStringWithCapacity(bodyStr.size + 4096)`。输入 body 大小
  是约 100 KiB，4 KiB 覆盖根 marker、85 个 event marker、product id 与数值变化的
  响应增量，避免常见请求进入 `checkAndExpend` 扩容路径。

### P0/P1：为解析后的浮点保留原始 token

状态：已实施并安装验证。

`parseJsonNumberSlow` 完成 `Float64` 有限性校验后，将 parser 输入 `Array<Byte>` 和
token 的 offset/length 存入 `JsonFloat`。这个 descriptor 不创建每数值一个 `String`；
同一棵 DOM 树共享输入 byte array。DOM 写出通过新增的
`WriteBuffer.append(array, offset, length)` 一次 range-copy 未修改 token；业务中替换
score/credits 的新 `JsonFloat` 没有原 token，继续走 Ryu。

因此本用例中约 1,269 个浮点里，未修改的约 1,099 个不再重新格式化，只有约 170 个
修改值走 Ryu。行为也改为 `JsonValue.fromStr(...).toString()` 保留合法浮点的原始文本
形式（包括尾随零、科学计数法与 `-0.0`），与 Java 基准的 raw-number 写出一致。

回归覆盖：`{\"v\":1.2300e+03,\"z\":-0.0}` 能精确 round-trip；同一输入经
`toStringWithCapacity(source.size + 64)` 也能精确 round-trip；`1e-7` 的 Ryu 小浮点
round-trip 保持通过。

### 本轮 512M、c100、100K 验证结果

两次均为 MSGC-only、60 秒、`cjHeapSize=16gb`、512 MiB persistent、
`cjHeapGrowth=1`、`cjScavengeGrowth=2`。累计 CPU 取 benchmark 进程最后采样的
`utime + stime`，仅以成功请求计算：

| 指标 | 本轮前基准 | 两项优化后 | 变化 |
| --- | ---: | ---: | ---: |
| 实际运行时间 | 60.180 s | 60.184 s | +0.0% |
| 成功请求 | 92,943 | 93,791 | +0.9% |
| 报告吞吐 | 1,544 req/s | 1,558 req/s | +0.9% |
| 累计 CPU | 367.66 s | 366.90 s | -0.2% |
| CPU/request | 3.956 ms | **3.912 ms** | **-1.11%** |
| success/CPU-s | 252.80 | **255.63** | **+1.12%** |

优化产物目录：

- `MSGC_RESULT/Result/case26_http_complex_json/result/20260807_512M_c100_100k_HeapGrow1_Scavenge2_compare_1`

这是一轮有效的 CPU/request 改善，但幅度较小；后续应继续按此指标复跑确认，而不以
瞬时 CPU 利用率或单独吞吐作为结论。

### P1：减少字符串、对象和 HTTP body 的复制

HTTP body 子项已实施，结果见第 8 节。`parseJsonString` 的零拷贝/延迟物化与
`HashMap` 容量策略仍未实施；P1 后的重新采样表明两者仍是最重要的 DOM 优化方向，详见第
9 节。

### P2：Runtime assist 作为后续 A/B 项，而非首要项

MSGC 样本中存在 mark/sweep assist；但两侧 GC 总时间都约为运行时间的 13.6%，并没有
证据表明 GC 是当前 Java 差距的首因。DOM 分配和大输出缓冲下降后，应重新采样，再用
`CJ_MS_DISABLE_ASSIST` 等开关做隔离 A/B，判断 runtime 参数调优的真实剩余收益。

## 8. P1 HTTP body 复制优化与验证

### 8.1 实现

本项只修改 case26 benchmark，不修改 Runtime、stdlib 或 stdx，因此验证时由 compare
脚本使用已安装的 MSGC SDK 重新以 `-O2 --gc-mode=marksweep` 编译用例，不需要重建 SDK。

1. 新增 `readHttpBodyString(body: InputStream, bodySize: ?Int64)`，供 server request 与
   client response 共用。
   - 有 `Content-Length` 时以该长度创建 `ByteBuffer`；未知长度时从 8 KiB 起始。
   - 读取循环改为 `ByteBuffer.write(chunk, 0, n)`，不再构造 `chunk[..n]` 临时 Array，
     也不再经过 `ArrayList<Byte>.toArray()`。
   - 最终通过 `String.fromUtf8(result.bytes())` 生成 body String。
2. server response 从 `body(ByteBuffer(respStr.toArray()))` 改为 `body(respStr)`。
   `HttpResponseBuilder.body(String)` 直接持有 String 的底层只读字节，消除了每个约 100 KiB
   响应一次完整的 `String -> Array<Byte>` 复制。
3. 增加默认关闭的 `CASE27_BODY_READ_SELF_TEST=1`：覆盖已知 Content-Length、未知
   Content-Length 和 String response body，三条路径均精确保持 JSON 内容。

### 8.2 验证结果

配置均为 512 MiB persistent、c100、100K、60 秒、MSGC-only、`cjHeapSize=16gb`、
`cjHeapGrowth=1`、`cjScavengeGrowth=2`。与第 7 节 P0 优化后的结果比较，仍只使用
`累计 CPU 秒 / 成功请求数` 作为主指标。

| 版本 | 成功请求 | 报告吞吐 | 累计 CPU | CPU/request | success/CPU-s |
| --- | ---: | ---: | ---: | ---: | ---: |
| P0 后基准 | 93,791 | 1,558 req/s | 366.90 s | 3.9119 ms | 255.63 |
| P1 最终第 1 轮 | 94,895 | 1,576 req/s | 327.58 s | 3.4520 ms | 289.69 |
| P1 最终第 2 轮 | 88,774 | 1,474 req/s | 275.06 s | 3.0984 ms | 322.74 |
| P1 两轮均值 | - | - | - | **3.2752 ms** | **305.87** |

两轮 P1 的 CPU/request 相比 P0 后基准平均下降 **16.28%**。吞吐率仍会因调度与机器负载在
1,474--1,576 req/s 间波动，但两轮 CPU/request 均同向改善，因此收益主要归因于 response
整包复制消除；仅替换 body reader 的中间试验为 3.9551 ms/request，未显示独立收益。

产物目录：

- `MSGC_RESULT/Result/case26_http_complex_json/result/20260807_512M_c100_100k_HeapGrow1_Scavenge2_compare_2`（仅 body reader 中间试验）
- `MSGC_RESULT/Result/case26_http_complex_json/result/20260807_512M_c100_100k_HeapGrow1_Scavenge2_compare_3`（P1 最终第 1 轮）
- `MSGC_RESULT/Result/case26_http_complex_json/result/20260807_512M_c100_100k_HeapGrow1_Scavenge2_compare_4`（P1 最终第 2 轮）

## 9. P1 后 30 秒采样与剩余优化点

### 9.1 采样配置与解释边界

在 P1 最终代码上重新执行 MSGC-only：512 MiB persistent、c100、100K、60 秒；启动后 10 秒
使用 macOS `/usr/bin/sample` 以 1 ms 间隔持续采样 30 秒。

- 产物：`MSGC_RESULT/Result/case26_http_complex_json/result/20260807_512M_c100_100k_HeapGrow1_Scavenge2_compare_5/msgc/sample_30s.txt`
- 该运行有 80,189 个成功请求、1,331 req/s、310.85 CPU 秒。`sample` 本身会显著扰动线程
  调度和吞吐，不能与非采样基准直接比较；仅用来定位活跃调用路径。
- 下表是 `sample` 的 top-of-stack 计数，而非严格 CPU 百分比；其中包含 park / socket 等
  等待栈，故只比较活跃业务函数的相对位置。

| 活跃路径 | top-of-stack 计数 | 结论 |
| --- | ---: | --- |
| `parseJsonString` | 11,544 | 当前最大的 JSON 业务热点；包含逐字节扫描与 String 物化。 |
| `HashMap<String, JsonValue>.add` | 6,693 | DOM object 字段建立与哈希计算开销。 |
| `parseJsonObject` | 5,719 | 对象语法、key/value 解析与字段插入的总路径。 |
| `parseJson` | 3,936 | 通用递归分派。 |
| `CJ_JSON_StringEscapeCharNumGet` | 3,848 | 写出时对 String 进行完整 escape 预扫描。 |
| `tryParseJsonIntFast` | 3,554 | 整数 token 快速解析。 |
| `wyhash` | 2,961 | `HashMap` 的 String key 哈希。 |
| `_platform_memmove` | 2,946 | JSON byte copy 与容器扩容。 |
| `jsonWriteWithoutFormat` | 2,917 | DOM 输出递归。 |
| `WriteBuffer.append(Array)` / `checkAndExpend` | 2,789 / 2,457 | 输出 byte 写入与每段容量检查。 |
| `parseJsonNumberSlow` | 1,298 | 浮点/非快速整数解析；其下仍可见 `strtod`。 |

payload 的结构也支持上述判断：单个 100K 输入含 6,055 个 object key、2,299 个 String value，
共 51,770 字节；全部没有需要 JSON escape 的字符。它有 1,364 个 object，其中 85 个 event
object 为 12 字段，正是 HashMap 扩容/rehash 的敏感对象形状。

### 9.2 后续优化优先级

1. **P0：JsonString raw token + 延迟 String 物化。**
   `parseJsonString` 的无转义快路径目前仍以 `parser.data[start..end]` 创建 slice，并调用
   `String.fromUtf8Unchecked` 复制数据。为 `JsonString` 保存原始 token 的
   `(data, offset, length, escaped)`；未被业务读取或修改的 value 延迟创建 String，写出时直接
   range-copy 原 token（含引号）。这同时消除解析复制、`CJ_JSON_StringEscapeCharNumGet` 的
   二次全串扫描及普通 String 写出复制。object key 仍需立即物化以进入 HashMap，value 则可延迟。
   该项覆盖本次最高的解析和写出热点，应优先实施。

2. **P0：native 批量扫描无转义字符串。**
   即使保留现有 String API，`parseJsonString` 也不应逐字节调用 Cangjie 数组访问与边界检查。
   新增 native `findQuoteOrBackslash(data, start, end)`，一次扫描到 `\"` 或 `\\`；命中引号直接
   返回无转义 token，命中反斜杠才进入现有 decode 路径。此项直接针对 `parseJsonString` 与
   `checkInvalid` 热点，且不改变 JSON 语义。

3. **P1：清除 writer 固定字面量临时 Array，并提供已知容量快速 append。**
   `jsonWriteWithoutFormat` 对每个 object field 都会把 key/value 分隔符 String 转为
   临时 Array；sample 中 `String.toArray` 仍有 856 个 top-of-stack 样本。将该分隔符、`{}`
   与 `[]` 改为模块级 `Array<Byte>` 常量。随后可为 `toStringWithCapacity` 的足够容量路径减少重复
   `checkAndExpend` 调用；容量不足时仍保持原扩容行为。

4. **P1：减少 JsonObject/HashMap rehash，而非盲目提高全局初始容量。**
   `JsonObject` 当前以 capacity 8 创建，`HashMap` 的 16 bucket 在第 12 个元素触发 rehash；85 个
   12 字段 event 因此重复进入扩容路径。应先为 parser 提供针对性 reserve/容量提示，或修正
   HashMap 正好处于 0.75 load factor 时的扩容阈值，再以 RSS 和碰撞统计验证。直接把所有 object
   初始容量提高会放大另外 1,279 个小 object 的内存占用，不建议先做。

5. **P1/P2：替换浮点 `strtod` 解析。**
   `CJ_JSON_ParseFloat64` 目前复制 token 到临时 C buffer 后调用 `strtod`；采样可见
   `strtod -> _ffpp_strtoencf64_l -> localeconv_l` 及相关锁等待。短期可使用缓存 C locale 的
   `strtod_l` 降低 locale 路径；长期使用 C 版 Ryu s2d/同级 shortest-roundtrip parser，消除
   libc locale 与临时 token copy。该项的单点样本小于 String 路径，但 100K 输入有 1,431 个
   浮点，因此仍是通用 stdx 优化项。

6. **P2：GC assist 是分配压力的结果，暂不作为首改目标。**
   本次 60 秒运行记录 508 次 Mark（约 8.5 次/s）。最近 GC 周期中 mutator mark assist 占
   19.8%--31.0% 的 mark work unit，sample 亦有 `MSMarkAssist`、`MSSweepAssist`、
   `CentralRegionPool` 锁等待。MarkTermination 本身约 1--2 ms，未显示单独的停顿异常；先减少
   DOM 的 String/Array/HashMap 短命分配，之后再以 `CJ_MS_DISABLE_ASSIST` 做 A/B，判断 Runtime
   pacing 或 sweep assist 的真实剩余收益。

### 9.3 P0 验证：native 无转义扫描 + JsonString raw token（当前实现回退）

本轮同时实施了 9.2 的前两项，并使用
`runtime/build/scripts/incremental_rebuild_msgc_sdk.py` 完整重建、安装 Region-scan MSGC SDK。
实现位于 `stdx.encoding.json`：

1. 新增 native `CJ_JSON_ScanUnescapedString(data, start, end)`。它在 native 层一次扫描直到
   closing quote 或反斜杠；返回 closing quote offset，并同时携带是否出现字面 `&`。遇到反斜杠
   则回退到原有 decode/校验路径。
2. `JsonString` 保存输入 byte array 与 raw token 的 `(offset, length)`；无转义字符串只在调用
   `getValue()` 时通过 `String.fromUtf8Unchecked` 物化。对于已有 escape 的字符串，仍立即 decode
   以保持原有 JSON 校验，但同时保留 raw token。
3. 默认 HTML-safe 输出中，raw token 不含字面 `&` 时直接调用
   `WriteBuffer.append(data, offset, length)`；因此跳过 `CJ_JSON_StringEscapeCharNumGet` 的全串
   escape 预扫描和 `appendEscape` 的复制。含字面 `&` 的 token 保持原有 `\\u0026` 输出语义；
   `htmlSafe: false` 时可直接写回。

回归测试 `stdx/test/dom_json_string_raw_token.cj` 已在安装后的 SDK 上通过：

- `{"unicode":"\\u0061"}` DOM parse/write 后精确保留 `"\\u0061"`；
- `{"amp":"a&b"}` 默认输出仍为 `"a\\u0026b"`；
- 调用 `asString().getValue()` 后得到已延迟物化的语义值 `"plain"`；
- 既有 `dom_json_float_raw_token` 与 `dom_json_float_ryu` 回归也均以
  `--gc-mode=marksweep` 编译、运行成功。

验证参数均为 MSGC-only、100K、c100、60 秒、512 MiB persistent、`cjHeapSize=16gb`、
`cjHeapGrowth=1`、`cjScavengeGrowth=2`。CPU 仍取 `rss.csv` 最后一行 benchmark 进程累计
`utime + stime`，只按成功请求计算：

| 版本 | 成功请求 | 报告吞吐 | 累计 CPU | CPU/request | 相对 P1 均值 |
| --- | ---: | ---: | ---: | ---: | ---: |
| P1 两轮均值（9.2） | - | 1,525 req/s | - | **3.2752 ms** | - |
| raw token 第 1 轮 | 81,790 | 1,357 req/s | 374.64 s | 4.5805 ms | +39.9% |
| raw token 第 2 轮 | 82,125 | 1,363 req/s | 365.12 s | 4.4459 ms | +35.7% |
| raw token 两轮均值 | - | 1,360 req/s | - | **4.5132 ms** | **+37.8%** |

产物目录：

- `MSGC_RESULT/Result/case26_http_complex_json/result/20260807_512M_c100_100k_HeapGrow1_Scavenge2_compare_6`
- `MSGC_RESULT/Result/case26_http_complex_json/result/20260807_512M_c100_100k_HeapGrow1_Scavenge2_compare_7`

该 payload 的 6,055 个 key 与 2,299 个 String value（共 **8,354**）都没有需要 escape 的字符，
所以语义上确实走到了 raw fast path；但这也暴露出当前 representation 的成本：每个 string 都要
跨一次 Cangjie-to-native FFI 边界，并从原本只有一个 `String` 引用的 `JsonString` 扩展为 raw data、
offset、length、状态等多个字段。100 并发下，额外对象尺寸、字段访问和 GC 扫描成本超过了所省的
字符串 copy 与 escape 预扫描。因此这版验证为**功能正确、性能不接受**，不应把本结果算作 P0 收益。

若继续该方向，下一版必须先压缩 raw descriptor（将 offset/length/state 打包，避免 `Option` 与多个
独立字段），并避免每个短 key/value 各一次 FFI 调用，例如以 parser 侧批量 tokenizer 一次扫描多段
token；仅把逐 string 扫描换到 native 层不足以获得收益。对象 key 又必须用于 HashMap，优先只为
value 保存 raw token，亦可先减少约 6,055 次/请求的 descriptor 与 FFI 成本。

## 10. 结论

case26 的主要瓶颈仍是普通 DOM JSON 的字符串、对象和数字物化与输出路径，而不是单纯的
MSGC GC 次数。DOM 浮点写出、浮点原 token、输出容量和 HTTP response 整包复制均已优化；其中
P1 HTTP response 修改使 CPU/request 相对 P0 后基准平均降低 **16.28%**。JsonString raw token/
延迟物化与 native 无转义扫描已完成第一版验证，但在此 payload 上使 CPU/request 回退 **37.8%**；
后续必须先做 descriptor 压缩、parser 批量化或只覆盖 value，再进行 A/B。后续仍以 CPU/request
为主指标，避免瞬时 CPU、采样扰动或启动期吞吐造成误判。

## 11. MSGC 全进程 CPU 火焰图与热点分析

本节同步 `MSGC_RESULT/Result/case26_http_complex_json/youhua/baseline_analysis.md` 中的
全进程 `perf` 采样结果，作为后续优化的函数级基准。完整说明见
[msgc_flamegraph_analysis.md](../../MSGC_RESULT/Result/case26_http_complex_json/youhua/msgc_flamegraph_analysis.md)。
原始产物包括 [SVG 火焰图](../../MSGC_RESULT/Result/case26_http_complex_json/youhua/result/20260808_msgc_flamegraph_c100_100K_Procs10_v2/msgc_cpu_flamegraph.svg)、
[Self 热点报告](../../MSGC_RESULT/Result/case26_http_complex_json/youhua/result/20260808_msgc_flamegraph_c100_100K_Procs10_v2/perf_hotspots_self_nochildren.txt)
和 [累计热点报告](../../MSGC_RESULT/Result/case26_http_complex_json/youhua/result/20260808_msgc_flamegraph_c100_100K_Procs10_v2/perf_hotspots_children.txt)。

### 11.1 采样配置与有效性

采样使用已完成 JSON 序列化容量优化的 case26 benchmark，参数为 payload=100K、
concurrency=100、duration=60000ms、persistent=512MiB/8 workers、P=10、
HEAP_SIZE=20GB、`cjHeapGrowth=1`、`cjScavengeGrowth=2`。compiler、runtime、stdlib
均来自 `/home/lipeisheng/cangjie/cangjie_compiler_CMSGC/output/msgc`。

使用 Linux `perf 5.15.200`，99Hz、DWARF callchain，附加到真实 benchmark PID，覆盖全部线程
采样约 58 秒。获得约 61K 样本，丢失样本为 0；采样运行完成 49,339 个请求，吞吐 818 req/s，
全部成功。相同版本无采样正式结果为 841 req/s，采样扰动约 2.7%。因此以下百分比用于热点排序，
不直接换算精确的单请求耗时。

### 11.2 模块与线程分布

DSO Self 样本分布：`libstdx.encoding.json.so` 37.30%、`libcangjie-runtime.so` 32.33%、
`libc.so.6` 14.61%、benchmark 7.89%、kernel 6.31%、`libstdx.net.http.so` 0.78%。
JSON 标准库和 runtime 是主要 CPU 去向；HTTP 库自身不是当前第一瓶颈。

线程分布：`schd-worker` 78.73%、benchmark 主线程 8.84%、`gc-helper` 2.70%、
`gc-main-thread` 2.35%、`gc-pool-t1..t4` 合计 6.90%、`schmon` 0.49%。GC 专用线程约占
10.7%，但 Mark/Sweep Assist 会运行在 `schd-worker`，所以不能仅用专用线程占比表示 GC 总成本，
也不能将它与具名 GC 函数 Self 占比直接相加。

### 11.3 函数级热点

主要 Self 热点为：`memset` 7.52%、`parseJsonString` 7.33%、`parseJsonObject` 5.94%、
`jsonWriteWithoutFormat` 3.86%、`WriteBuffer.append(Array)` 3.15%、`MCC_AcquireRawData`
2.80%、`MRT_GetThreadLocalData` 2.47%、`TypeInfo::IsSubType` 2.42%、
`MarkSweepCollector::AddRefToMarkBatch` 2.23%、`memcpy` 2.11%、
`MarkSweepCollector::RunRegionTask` 1.81%、`CJ_JSON_StringEscapeCharNumGet` 1.77%。

近似分类显示：JSON parse 20.42%、JSON write 12.01%、`memset/memcpy` 9.87%、MSGC Mark
6.56%、RawData/安全检查 4.66%、TLS/调度 3.69%。这些分类仍有约 20.12% Other，且
`memset/memcpy` 可能同时来自 JSON、runtime 和对象初始化，不能视为精确归因。

### 11.4 累计调用链与结论

累计调用链中，`JsonValue.fromStr` 约占 39.04%，`JsonValue.toStringWithCapacity`/
`serializeServerJson` 约占 25.64%。累计占比包含子函数，不能与 Self 占比相加；它确认完整
JSON parse 是最大的业务 CPU 调用链，JSON write 次之。runtime 的 32.33% 也不能整体归入 GC，
其中包含 RawData、TLS、类型判断、对象操作和 GC 多类成本。

### 11.5 后续优化顺序

1. **Parser `strCache` 懒分配。** 当前 `JsonParser` 按 `max(input.size / 4, 64)` 无条件初始化，
   100K 输入每次约预留 25KB；建议只在遇到转义字符串时分配或增长。
2. **Object/array 容量 A/B。** 当前初始容量为 8，测试 8/16/32 或自适应容量，重点观察
   `parseJsonObject`、HashMap resize、ArrayList grow、RSS 和 CPU/request。
3. **WriteBuffer 优化。** 核验 exact-fit 时的扩容条件；将 `"{}".toArray()`、`"[]".toArray()`、
   `"\":".toArray()` 改为静态 bytes；评估收紧 12.5% 容量余量和 buffer 复用。
4. **减少 RawData/escape 重复遍历。** 评估 raw bytes range append 和无转义字符串一次扫描/复制。
5. **JSON kind/tag 快速分派。** 针对 `asObject/asArray/asInt/asFloat` 降低 `TypeInfo::IsSubType`
   成本，但需先做正确性和 microbenchmark。
6. HTTP DSO 仅 0.78%，暂不优先进行 HTTP API 层重构；GC 先保持不变，待 JSON 分配下降后再做
   `CJ_MS_DISABLE_ASSIST` A/B。

以上 parser、容器和 WriteBuffer 方向均属于待验证假设；后续每项保持相同 SDK、P10、c100/100K、
20GB heap 和 512MiB persistent，至少重复 3 次，并以 CPU/request 为主指标。最优版本再重新采集
全进程火焰图，确认热点是否转移。

## 12. Parser `strCache` 懒分配优化（2026-08-08）

### 12.1 改动

按第 11.5 节的第一项实施。`JsonParser` 以前在每次 `JsonValue.fromStr` 时无条件创建
`ArrayList<Byte>(max(input.size / 4, 64))`；对 case26 的 100K 输入，即使其中字符串没有转义字符，
也会预留约 25KB 临时缓存。

在 `cangjie_stdx_CMSGC/src/stdx/encoding/json/native.cj` 中将 `strCache` 改为
`?ArrayList<Byte>`：初始为 `None`，仅 `parseJsonString` 首次发现反斜杠转义时通过
`ensureStrCache()` 创建 64B 缓存；已创建的 parser 则在 `reset()` 时清空并复用。无转义的普通
JSON 字符串不再创建该缓存。

回归用例覆盖无转义字符串、普通转义、Unicode `\\u` 转义及 UTF-16 代理对，均通过。

### 12.2 构建、安装与单次 MSGC 验证

本次只修改 stdx，未重编 runtime、stdlib、compiler 或 LLVM。使用 MSGC SDK：
`/home/lipeisheng/cangjie/cangjie_compiler_CMSGC/output/msgc`；重编后的
`libstdx.encoding.json.so` 已同步到该 SDK，SDK 库与 stdx 构建产物 SHA-256 一致。

按项目优化规则，只运行 MSGC 单 SDK 的 case26 一次，未启动 CopyGC、Go 或 Java。参数为：
payload=100K、concurrency=100、duration=60000ms、persistent=512MiB/8 workers、
`cjHeapSize=20gb`、P=10、`cjHeapGrowth=1`、`cjScavengeGrowth=2`、
`cjMSMinGCThreshold=20mb`。

| 指标 | 最近基准 | `strCache` 懒分配 | 变化 |
| --- | ---: | ---: | ---: |
| 实际时长 | 60.240 s | 60.307 s | +0.1% |
| 成功请求 | 47,750 | 51,896 | +8.7% |
| 报告吞吐 | 792 req/s | **860 req/s** | **+8.6%** |
| 失败请求 | 0 | 0 | 无变化 |
| 平均 RSS | 1.87 GB | 1.90 GB | +1.5% |
| 最大 RSS | 1.92 GB | 1.97 GB | +2.5% |
| GC 次数 | 362 | 336 | -7.2% |
| GC 总时间 | 12.920 s | 14.443 s | +11.8% |
| 平均单次 GC | 35.7 ms | 43.0 ms | +20.4% |

最近基准目录为
`MSGC_RESULT/Result/case26_http_complex_json/result/20260808_512M_c100_100k_HeapGrow1_Scavenge2_Procs10_compare/msgc`；
本次结果目录为
`MSGC_RESULT/Result/case26_http_complex_json/result/20260808_512M_c100_100k_HeapGrow1_Scavenge2_Procs10_compare_1/msgc`。

### 12.3 结论

该单次复测中，跳过无转义输入的 25KB parser 缓存预留，使端到端吞吐提高约 8.6%，且 GC 周期数减少。
平均 RSS 与单次 GC 时长略升，说明本结果应作为正向趋势，而非把 GC 时长变化直接归因给该优化。
后续仍应以同参数的单次 MSGC 结果持续比较；只有用户明确要求时才做额外重复或完整 A/B。

## 13. DOM 浮点解析 Ryu 快路径（2026-08-08）

### 13.1 改动

采样中 `parseJsonNumberSlow` 的底层仍可见 `CJ_JSON_ParseFloat64`、临时 buffer、
`memcpy_s` 和 libc `strtod`。原实现必须先把不以 `\0` 结尾的 JSON number token 复制到
512B 栈缓冲；token 超过栈缓冲时还需使用 `malloc/free`，然后才能调用 `strtod`。

本次在 `cangjie_stdx_CMSGC/src/stdx/encoding/json` 中增加 Ryu `s2d_n`，并将其接入
DOM JSON 的 native FFI：

1. 新增 `stream/native/ryu/ryu_parse.h` 和 `stream/native/ryu/s2d.c`，提供带显式长度的
   decimal-to-double 转换，不要求输入以 `\0` 结尾。
2. `native/CMakeLists.txt` 将 `s2d.c` 加入 `stdx.encoding.jsonFFI`。
3. `CJ_JSON_ParseFloat64` 优先把 parser 输入的 `(data + start, end - start)` 直接传给
   `s2d_n`，成功时不再创建临时 buffer，也不执行 `memcpy_s`、`strtod` 或 `free`。
4. 新增 `CJ_JSON_CanUseRyu` 准入检查。尾数不超过 17 位、指数不超过 3 位的普通 JSON
   浮点进入 Ryu；更长或少见形式继续走原有 `strtod` 路径。该兜底保留现有输入兼容性，
   同时避免 Ryu 内部整数表示溢出。

该改动优化的是浮点 token 的“文本转 `Float64`”过程，不改变此前已经实施的浮点 raw token
保存、Ryu 浮点写出、`strCache` 懒分配和 writer 静态 byte 常量等改动。

### 13.2 构建、安装与正确性验证

本次只修改 stdx，因此仅执行 stdx 的 clean/build/install，没有重编 Runtime、LLVM、compiler
或完整 SDK。安装来源和验证组合如下：

- compiler、runtime、stdlib：保持使用
  `/home/lipeisheng/cangjie/cangjie_compiler_CMSGC/output/msgc` 中的现有版本；
- stdx：由 `/home/lipeisheng/cangjie/cangjie_stdx_CMSGC` 重新构建并安装；
- 安装位置：`/home/lipeisheng/cangjie/cangjie_compiler_CMSGC/output/msgc/linux_aarch64_cjnative`；
- stdx target 与 SDK 中的 `libstdx.encoding.json.so` SHA-256 均为
  `b77e4dd20fe0cb1036819e206a4deb728a6318a8adbf150b7f856ccb3ec19e63`。

针对当前 `100K.json` 的原始浮点 token 做了 Ryu 与 libc `strtod` 的 IEEE-754 位级比较：

| 项目 | 数量 |
| --- | ---: |
| 浮点 token 总数 | 1,431 |
| Ryu 快路径 | 1,346 |
| `strtod` 兜底 | 85 |
| 位级不一致 | **0** |

安装后的 `libstdx.encoding.json.so` 也确认同时包含 `CJ_JSON_ParseFloat64` 和 `s2d_n`
符号。因此本次验证确认：94.1% 的浮点 token 已进入无临时复制快路径，且这些 token 的
转换结果与原 `strtod` 完全一致。

### 13.3 case26 单次 MSGC 验证结果

按项目优化验证规则，只运行 MSGC SDK 一次，未启动 CopyGC、Go 或 Java。两次参数均为
payload=100K、concurrency=100、duration=60000ms、persistent=512MiB/8 workers、
`cjHeapSize=20gb`、P=10、`cjHeapGrowth=1`、`cjScavengeGrowth=2`、
`cjMSMinGCThreshold=20mb`。

| 指标 | 修改前最近结果 | Ryu 浮点解析 | 变化 |
| --- | ---: | ---: | ---: |
| 实际时长 | 60.262 s | 60.348 s | +0.1% |
| 成功请求 | 52,840 | 51,983 | -1.6% |
| 报告吞吐 | 876 req/s | 861 req/s | -1.7% |
| 失败请求 | 0 | 0 | 无变化 |
| 平均 RSS | 1,821,915 KB | 1,808,613 KB | -0.7% |
| 最大 RSS | 1,876,740 KB | 1,864,208 KB | -0.7% |
| 平均 `ps` CPU | 964.3% | 1,008.1% | +4.5% |
| GC 次数 | 348 | 373 | +7.2% |
| GC 总时间 | 13.923 s | 16.893 s | +21.3% |
| 平均单次 GC | 40.0 ms | 45.3 ms | +13.2% |

修改前结果目录：

- `MSGC_RESULT/Result/case26_http_complex_json/result/20260808_512M_c100_100k_HeapGrow1_Scavenge2_Procs10_compare_2/msgc`

本次结果目录：

- `MSGC_RESULT/Result/case26_http_complex_json/youhua/result/20260808_msgc_ryu_float_c100_100K_Procs10`

### 13.4 结论

Ryu 快路径已经消除了本 payload 中 1,346 次/请求的浮点临时 buffer 初始化、token 复制和
`strtod` 调用，且位级正确性验证通过。但本次端到端吞吐为 861 req/s，比最近单次结果低 1.7%，
没有观测到可归因于该修改的吞吐提升。平均 RSS 下降约 0.7%，同时 GC 时间有所增加；这些变化
受到单次 HTTP/GC 运行波动影响，不能直接归因于 Ryu。

因此本项当前结论是：**解析链路的复制与 libc 调用已按预期消除，功能正确，但单次 case26
端到端验证没有证明性能收益**。该快路径可保留；后续只有在重新采样仍显示浮点解析为显著热点，
或需要量化其微观收益时，再单独测量 `CJ_JSON_ParseFloat64` 的 CPU/token，而不把本轮 -1.7%
解释为确定回退。

## 14. Ryu 后 MSGC 全进程火焰图与新热点（2026-08-08）

### 14.1 采样配置与有效性

本次仅运行 MSGC SDK，没有启动 CopyGC、Go 或 Java。compiler、runtime、stdlib 均来自
`/home/lipeisheng/cangjie/cangjie_compiler_CMSGC/output/msgc`，stdx 为安装到该 SDK 的
`/home/lipeisheng/cangjie/cangjie_stdx_CMSGC` 当前 Ryu 版本。用例在采样前用该 SDK
重新编译。

参数为 100K/c100/60s、512MiB persistent/8 workers、P10、20GB heap、HeapGrowth=1、
ScavengeGrowth=2、MinGCThreshold=20MiB。`perf` 以 99Hz + DWARF callchain 附加到真实
benchmark PID，采样 58.052s，得到 60K samples，丢样为 0，benchmark 和 perf 退出码均为 0。
采样运行完成 53,806 个成功请求，报告 893 req/s；该数值只用于确认负载有效，
不与无采样的单次运行直接判定收益。

产物：

- [SVG 火焰图](../../MSGC_RESULT/Result/case26_http_complex_json/youhua/result/20260808_msgc_flamegraph_after_ryu_c100_100K_Procs10/msgc_cpu_flamegraph.svg)
- [聚合 Self 热点](../../MSGC_RESULT/Result/case26_http_complex_json/youhua/result/20260808_msgc_flamegraph_after_ryu_c100_100K_Procs10/perf_hotspots_self_aggregated.txt)
- [DSO 分布](../../MSGC_RESULT/Result/case26_http_complex_json/youhua/result/20260808_msgc_flamegraph_after_ryu_c100_100K_Procs10/perf_by_dso.txt)
- [线程分布](../../MSGC_RESULT/Result/case26_http_complex_json/youhua/result/20260808_msgc_flamegraph_after_ryu_c100_100K_Procs10/perf_by_thread.txt)

### 14.2 TopN Self 热点

| 排名 | 函数 | Self |
| ---: | --- | ---: |
| 1 | `parseJsonString` | 8.16% |
| 2 | `memset` | 7.70% |
| 3 | `parseJsonObject` | 6.11% |
| 4 | `jsonWriteWithoutFormat` | 5.06% |
| 5 | `MCC_AcquireRawData` | 3.10% |
| 6 | `TypeInfo::IsSubType` | 2.87% |
| 7 | `WriteBuffer.append(Array)` | 2.80% |
| 8 | `MRT_GetThreadLocalData` | 2.33% |
| 9 | `__aarch64_ldadd8_relax` | 2.11% |
| 10 | `String.checkInvalid` | 2.08% |
| 11 | `parseJson` | 1.93% |
| 12 | `AddRefToMarkBatch` | 1.91% |
| 13 | `WriteBuffer.checkAndExpend` | 1.88% |
| 14 | `CJ_JSON_StringEscapeCharNumGet` | 1.83% |
| 15 | `memcpy` | 1.69% |
| 16 | `CJ_MCC_ReleaseRawData` | 1.69% |
| 17 | `RunRegionTask` | 1.55% |
| 18 | `tryParseJsonIntFast` | 1.39% |
| 19 | `parseJsonNumberSlow` | 1.33% |
| 20 | `__aarch64_cas8_acq_rel` | 1.29% |

Inclusive 调用链中，JSON parse 入口 `JsonValue.fromStr` 占 43.54%，server JSON writer
`toStringWithCapacity` 占 30.05%，`appendEscape` 占 13.75%，泛型 Array 创建链占 10.30%，
`RunRegionTask` 占 6.90%，`SweepRegion` 占 6.18%，Mark Assist 占 4.76%。Inclusive 会嵌套，
不能与 Self 或其他 Inclusive 数值相加。

DSO Self 分布为 JSON 42.49%、runtime 31.17%、libc 11.88%、benchmark 7.70%。GC 专用线程
合计约 11.20%，但 mutator Mark/Sweep Assist 在 `schd-worker` 上执行，不包含在这个简单
加和中。

### 14.3 关键调用链归因

1. `memset` 不是普通 writer 填充为主：69.23% 的 `memset` 由 `SweepRegion` 清理死亡槽
   调用，26.73% 由 `TryAllocateLargeObjectOnce` 对大对象 Region 整体清零调用。
2. `TypeInfo::IsSubType` 的 99.71% 来自 `jsonWriteWithoutFormat` 的 `match (json)` 动态
   子类判定，是可以独立消除的 writer 调度成本。
3. `MCC_AcquireRawData` 的直接上层中，`appendEscape` 占 57.69%、浮点解析占 22.80%、
   整数 writer 占 16.55%。当前每个字符串的 escape 预扫描会产生明显的 RawData 进出成本。
4. `String.checkInvalid` 几乎全部由 case26 `readBodyStr`/`readRespStr` 的
   `String.fromUtf8(...)` 触发，分别占 48.18%/50.71%，不是 JSON parser 内部检查。
5. Ryu 后 libc `strtod` Self 由旧采样约 1.58% 降到约 0.20%；
   `CJ_JSON_ParseFloat64` Inclusive 由约 2.99% 降到 1.73%，`parseJsonNumberSlow` Inclusive
   由 9.14% 降到 7.90%。新的 `s2d_n` Self 为 0.79%，证明热点已从 libc 转入
   带长度的 Ryu 路径，但数字链路仍非零成本。

### 14.4 优化顺序

1. **P0：消除 writer 最后的整包克隆。** `WriteBuffer.getAndClear()` 已经把旧 buffer 与
   writer 分离，而 stdlib 已有不复制的 `String.withRawData(Array<UInt8>)`。case26 首先只把
   `JsonValue.toStringWithCapacity` 的 `String.fromUtf8Unchecked(buff.getAndClear())` 换成
   `String.withRawData(...)`，可每次 server serialize 少一个约 100K 数组克隆。这是 stdx-only
   修改，风险和验证范围最小。
2. **P0：无转义字符串直接构造 String 视图。** `parseJsonString` 普通分支现在先产生
   Array slice，再被 `fromUtf8Unchecked` 克隆。改为 `String.withRawData(slice)` 可避免字节数组
   克隆，且不增加上次 raw-token 方案的 descriptor 字段或逐字符串 FFI。风险是 DOM
   String 会保留整个输入 buffer；对本 payload，`JsonFloat.rawData` 已经保留该 buffer，但仍需
   单独做长生命 DOM 的 RSS 回归。
3. **P1：writer 用虚调度取代子类 match 链。** 在 `JsonValue` 上增加 package-internal
   `writeCompactTo` 并由各子类 override，以一次虚调用取代递归 `match (json)`，目标是
   消除 2.87% `IsSubType` Self 及部分 5.06% writer dispatch Self。仅用 `kind()` 后再
   `asObject/asArray` 仍会引入类型检查，不是首选实现。
4. **P1：合并无 escape 扫描与写出。** 对常见无转义路径增加一个 native
   scan-and-copy fast path，在同一次 RawData acquire 中扫描并写入；发现 escape 时回退原
   decode 路径。这样不在 `JsonString` 上增加状态字段，避免重复上次 +37.8%
   的 raw-token representation。
5. **P1：减少 `checkAndExpend` 调用边界。** case26 已一次预留输出容量，但每个
   byte/array append 仍调用该函数。先修正 exact-fit 判断为 `minCapacity <= oldCapacity`，
   再评估内联快速检查或“一次 ensure + 内部 unchecked append”。
6. **P1：HTTP body 的 owned UTF-8 转换。** `readBodyStr`/`readRespStr` 中 ArrayList
   `toArray()` 后数据已不再修改，可为这类场景提供“验证 UTF-8 后接管数组”的 API，
   或在可控 benchmark 中用 `withRawData`。直接跳过验证会改变不可信 HTTP 输入的安全语义，
   不应作为通用 stdx 默认修改。
7. **P2：先减少大数组分配，再改 GC 清零。** writer String 克隆、HTTP body 克隆和
   64K read chunk 都会进入大对象清零路径。优先做所有权转移/复用可同时减少 runtime
   `TryAllocateLargeObjectOnce` 和后续 Mark/Sweep 工作。若其后 `SweepRegion` 仍为顶部热点，
   再评估延迟清零、空 Region 批量 page reclaim 或内部未初始化 primitive array；这些都需要
   runtime 级正确性验证。

容器初始容量不再列为当前优先项：该 payload 有 1,364 个 object，其中 1,279 个字段数
不超过 7，只有 85 个是 12 字段；851 个 array 中有 850 个长度不超过 5。全局从 8
改到 16/32 会为大多数容器增加空间和 GC 扫描成本。另外，6,055 个 key 只有 87 个唯一值，
后续可考虑 parser-local key canonicalization，但必须避免通用全局 LRU/锁的成本，优先级低于
String 无拷贝视图。

## 15. DOM String 两条零拷贝路径（2026-08-08）

### 15.1 改动

本轮同时落地第 14.4 节的前两个 P0 项，只修改
`/home/lipeisheng/cangjie/cangjie_stdx_CMSGC/src/stdx/encoding/json`：

1. `JsonValue.toStringWithCapacity` 将
   `String.fromUtf8Unchecked(buff.getAndClear())` 改为
   `String.withRawData(buff.getAndClear())`。`WriteBuffer.getAndClear()` 返回旧 buffer 的有效
   slice，并立即给 writer 换成新的空 buffer，因此输出 `String` 可以接管旧 backing array，
   不再复制约 100K 的完整响应。
2. `parseJsonString` 的无转义分支将
   `String.fromUtf8Unchecked(parser.data[start..end])` 改为
   `String.withRawData(parser.data[start..end])`。普通 key/value 直接成为输入数据的 String
   视图，不再逐字符串克隆字节数组。包含反斜杠的字符串仍走 `strCache` 解码并复制，语义不变。

两条路径使用 `unsafe` API 的前提分别是：writer 的旧 buffer 已与后续写入隔离；parser 数据来自
输入 `String` 的只读 raw data，DOM 中的 slice 会通过 GC 引用保持 backing array 存活。对于当前
100K payload，浮点 raw token 已经会保留同一个输入 buffer，因此无转义字符串视图不会再额外
引入一份 100K 输入保留，但其他不含浮点值的长生命周期 DOM 仍应注意这一所有权变化。

### 15.2 stdx 构建、安装与正确性验证

本轮只修改 stdx，因此只执行 stdx build/install，没有重编 Runtime、LLVM、compiler 或 stdlib：

```bash
cd /home/lipeisheng/cangjie/cangjie_stdx_CMSGC
source /home/lipeisheng/cangjie/cangjie_compiler_CMSGC/output/msgc/envsetup.sh
python3 build.py build -t release --jobs 20 \
  --target-lib=/usr/lib/aarch64-linux-gnu \
  --build-args=--gc-mode=marksweep
python3 build.py install \
  --prefix /home/lipeisheng/cangjie/cangjie_compiler_CMSGC/output/msgc
```

验证时三件套来源如下：

- compiler、runtime、stdlib：
  `/home/lipeisheng/cangjie/cangjie_compiler_CMSGC/output/msgc` 原有版本；
- stdx：`/home/lipeisheng/cangjie/cangjie_stdx_CMSGC` 当前修改；
- 安装后的 JSON 动态库：
  `/home/lipeisheng/cangjie/cangjie_compiler_CMSGC/output/msgc/linux_aarch64_cjnative/dynamic/stdx/libstdx.encoding.json.so`；
- 安装产物 SHA-256：
  `5dbefda910cffc4f8843a2eecd2257c56eec168215eceb43b7c7bb6710a06118`。

回归用例覆盖：源字符串离开定义域后继续使用 DOM、无转义字符串、空字符串、原生 UTF-8、
普通转义、Unicode 转义、代理对，以及 `toStringWithCapacity` 输出。用例使用安装后的 stdx
重新编译，运行结果为 `[PASS] DOM JSON zero-copy string regression`，exit code 为 0。

回归产物目录：

- `MSGC_RESULT/Result/case26_http_complex_json/youhua/result/20260808_msgc_string_with_raw_data_regression`

### 15.3 case26 单次 MSGC 验证

按照当前优化验证规则，只运行一次 MSGC，没有运行 CopyGC、Go、Java，也没有回退代码做完整
A/B。case26 在运行前用当前 SDK 重新编译。参数与上一轮 Ryu 无采样结果一致：100K/c100/60s、
512MiB persistent/8 workers、P10、20GB heap、HeapGrowth=1、ScavengeGrowth=2、
MinGCThreshold=20MiB。

| 指标 | 修改前（Ryu） | 两条 String 零拷贝 | 变化 |
| --- | ---: | ---: | ---: |
| 实际时长 | 60.348 s | 60.253 s | -0.2% |
| 成功请求 | 51,983 | **58,247** | **+12.1%** |
| 报告吞吐 | 861 req/s | **966 req/s** | **+12.2%** |
| 失败请求 | 0 | 0 | 无变化 |
| 平均 RSS | 1,808,613 KB | 1,816,680 KB | +0.45% |
| 最大 RSS | 1,864,208 KB | 1,876,292 KB | +0.65% |
| 平均 `ps` CPU | 1,008.1% | 963.2% | -4.5% |
| GC 次数 | 373 | 348 | -6.7% |
| GC 总时间 | 16.893 s | 14.100 s | -16.5% |
| 平均单次 GC | 45.3 ms | 40.5 ms | -10.5% |
| 最大单次 GC | 73.3 ms | 67.3 ms | -8.1% |

修改前目录：

- `MSGC_RESULT/Result/case26_http_complex_json/youhua/result/20260808_msgc_ryu_float_c100_100K_Procs10`

本次目录：

- `MSGC_RESULT/Result/case26_http_complex_json/youhua/result/20260808_msgc_string_with_raw_data_c100_100K_Procs10`

60 秒 RSS 序列各有 243 个样本。前 20 个与后 20 个样本的均值差由修改前约 529,545 KB
变为约 524,570 KB，并未显示 backing data 保留使进程内存增长速度恶化；平均/峰值 RSS 的
0.45%/0.65% 上升较小。该结果可以排除当前 case26 负载下的明显 RSS 放大，但不能替代针对
“不含浮点、长期持有大量 DOM”的专门内存压力测试。

### 15.4 结论

两条零拷贝改动合并后，本次端到端吞吐从 861 提升到 966 req/s，提升 12.2%，同时 CPU
采样均值、GC 次数和 GC 总时间下降，且所有 58,247 个请求成功。结果符合“减少 parser
逐字符串克隆和 writer 约 100K 整包复制”的预期；当前 case26 没有观察到显著 RSS 代价。

由于用户要求直接保留两个改动并只做一次 MSGC 验证，本轮没有拆成两个独立 A/B，因此
**12.2% 是两项改动的合并效果，不能分别归因到 writer 或 parser**。

## 16. Compact writer 虚分派与短字符串 escape 融合（2026-08-08）

### 16.1 改动与实现边界

本轮实现第 14.4 节的两个 writer P1 项，只修改
`/home/lipeisheng/cangjie/cangjie_stdx_CMSGC/src/stdx/encoding/json`：

1. 在 `JsonValue` 上增加受保护的 `writeCompactTo(WriteBuffer, Int64, Bool)` 虚函数，
   `JsonNull`、`JsonBool`、`JsonInt`、`JsonFloat`、`JsonString`、`JsonArray` 和 `JsonObject`
   分别实现自己的 compact 输出。`jsonWriteWithoutFormat` 只保留深度检查和一次虚调用，移除
   每个 DOM 节点上的 `match (json)`/`IsSubType` 链。递归深度和 HTML-safe 参数继续逐层传递，
   pretty writer 不变。
2. `WriteBuffer.appendEscape` 对小于 32 字节的常见 key/value 使用 CJ 内单循环路径：扫描一个
   byte 后立即原样复制或输出 escape，避免旧路径先调用 native 统计 escape 数、再进入 native
   写出函数的两阶段处理及其 RawData acquire/release 边界。该路径保留 `\b`、`\f`、`\n`、
   `\r`、`\t`、引号、反斜杠、控制字符、DEL 和 HTML-safe `&` 的原有编码规则；32 字节及以上
   字符串仍走原 native 路径，避免在大字符串上引入逐 byte CJ 边界检查成本。

这次没有给 `JsonString` 增加 raw-token、descriptor、source offset 或其他解析状态，因此没有
恢复此前导致吞吐回退 37.8% 的 representation 方案。第 8 节已有的静态标点 byte 数组、
第 13 节的 Ryu 路径和第 15 节的 String 零拷贝改动均被保留。

涉及文件：

- `src/stdx/encoding/json/json_value.cj`
- `src/stdx/encoding/json/json_object.cj`
- `src/stdx/encoding/json/write_buffer.cj`

### 16.2 stdx 构建、安装与正确性验证

本轮只重建并安装 stdx，没有执行 LLVM、compiler、stdlib 或 runtime 的重建脚本。执行命令为：

```bash
cd /home/lipeisheng/cangjie/cangjie_stdx_CMSGC
source /home/lipeisheng/cangjie/cangjie_compiler_CMSGC/output/msgc/envsetup.sh
python3 build.py build -t release --jobs 20 \
  --target-lib=/usr/lib/aarch64-linux-gnu \
  --build-args=--gc-mode=marksweep
python3 build.py install \
  --prefix /home/lipeisheng/cangjie/cangjie_compiler_CMSGC/output/msgc
```

build 和 install 均以 exit code 0 完成。构建目录和 SDK 安装目录中的
`libstdx.encoding.json.so` SHA-256 均为：

```text
57221951c44cc808bcf8f60aab4c117c2a90143f7d12866a1321ed2f2ccc5026
```

验证时的来源为：

- compiler、runtime、stdlib：
  `/home/lipeisheng/cangjie/cangjie_compiler_CMSGC/output/msgc` 原有版本；
- stdx：`/home/lipeisheng/cangjie/cangjie_stdx_CMSGC` 当前源码重建后安装到上述 SDK；
- 用例：使用上述 SDK 和已安装的 stdx JSON 动态库重新编译，没有复用旧 binary。

正确性用例覆盖 7 种 `JsonValue` 子类的 compact 虚分派、普通 UTF-8、短字符串命名 escape、
控制字符十六进制 escape、HTML-safe 开/关、以及大于等于 32 字节的 native 回退路径。运行输出
为 `[PASS] DOM JSON writer regression`，exit code 为 0。

回归产物：

- `MSGC_RESULT/Result/case26_http_complex_json/youhua/result/20260808_msgc_virtual_writer_escape_fusion_regression`

### 16.3 case26 单次 MSGC 验证

按照当前优化验证规则，只执行一轮 MSGC，没有运行 CopyGC、Go、Java，也没有回退代码再做
完整 A/B。参数与第 15 节结果保持一致：100K/c100/60s、512MiB persistent/8 workers、
P10、20GB heap、HeapGrowth=1、ScavengeGrowth=2、MinGCThreshold=20MiB。

| 指标 | 修改前（String 零拷贝） | 虚分派 + escape 融合 | 变化 |
| --- | ---: | ---: | ---: |
| 实际时长 | 60.253 s | 60.189 s | -0.1% |
| 成功请求 | 58,247 | **66,325** | **+13.9%** |
| 报告吞吐 | 966 req/s | **1,101 req/s** | **+14.0%** |
| 失败请求 | 0 | 0 | 无变化 |
| 平均 RSS | 1,816,680 KB | 1,841,881 KB | +1.39% |
| 最大 RSS | 1,876,292 KB | 1,900,744 KB | +1.30% |
| 平均 `ps` CPU | 963.2% | 978.0% | +1.54% |
| GC 次数 | 348 | 404 | +16.1% |
| GC 总时间 | 14.100 s | 15.992 s | +13.4% |
| 平均单次 GC | 40.518 ms | 39.584 ms | -2.3% |
| 最大单次 GC | 67.348 ms | 68.059 ms | +1.1% |

修改前目录：

- `MSGC_RESULT/Result/case26_http_complex_json/youhua/result/20260808_msgc_string_with_raw_data_c100_100K_Procs10`

本次目录：

- `MSGC_RESULT/Result/case26_http_complex_json/youhua/result/20260808_msgc_virtual_writer_escape_fusion_c100_100K_Procs10`

GC 总量随完成请求数增加而上升。按请求归一化后，GC 时间约从 242.1 us/request 降到
241.1 us/request，基本持平；GC 频率从 5.98 次/千请求变为 6.09 次/千请求，增加约 1.9%。
因此 GC 总时间的 +13.4% 主要是新版本在相同 60 秒内处理了更多请求，而不是单次 GC 变慢。
RSS 序列各有 243 个样本，新版本平均/峰值增加约 1.3%～1.4%，没有出现数量级放大。

### 16.4 结论

本轮所有 66,325 个请求成功，单次 case26 吞吐由 966 提升到 1,101 req/s，提升 14.0%；
平均 CPU 从 9.63 核提高到 9.78 核，说明 writer 调度和短字符串 escape 边界成本下降后，P10
资源利用略有提高。平均单次 GC 反而下降 2.3%，GC 总时间增加与单位时间处理量增加相符。

结果方向符合采样中的 `TypeInfo::IsSubType`、`jsonWriteWithoutFormat`、
`MCC_AcquireRawData` 和 escape 扫描热点。但本轮遵循“直接保留改动、只执行一次 MSGC”的验证
规则，没有拆分两项修改或重复运行，因此 **14.0% 是两项修改叠加后的单次端到端结果，不能
分别归因，也不应排除运行波动**。后续 10 秒采样应重点确认 `TypeInfo::IsSubType` 是否已从
compact writer 调用链消失，以及 `MCC_AcquireRawData`/`CJ_JSON_StringEscapeCharNumGet` 在
短字符串 writer 路径中的占比是否下降。
