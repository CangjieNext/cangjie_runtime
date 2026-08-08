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
