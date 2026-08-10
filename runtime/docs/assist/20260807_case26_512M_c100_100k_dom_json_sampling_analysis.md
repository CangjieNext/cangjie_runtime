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

## 17. macOS MSGC 后续优化基线（2026-08-10）

### 17.1 目的与固定配置

本节建立后续 case26 DOM JSON 优化的 **macOS 单侧 MSGC 基线**。后续未特别说明时，
优化前后都应保持本节的 SDK、负载、GC 参数和统计口径一致；主比较指标为
`累计 CPU 秒 / 成功请求数`，吞吐仅作为辅助结果。

本轮仅运行 Region-scan MSGC side；CopyGC、Go 与 Java 均关闭。用例由当前安装的
`/Users/lipeisheng/code/cangjie/cangjie/cangjie_compiler_region_mark/output/msgc` 重新以
`-O2 --gc-mode=marksweep` 编译并运行。

| 项目 | 固定值 |
| --- | --- |
| 平台 | macOS / darwin-aarch64 |
| payload | `100K.json`，99,572 B |
| 并发 / client mode | 100 / `sync` |
| 压测时长 | 60,000 ms |
| 持久对象基线 | 512 MiB，8 workers |
| `PROCS` | 10 |
| `cjHeapSize` | 16 GiB |
| `cjHeapGrowth` / `cjScavengeGrowth` | 1 / 2 |
| `cjMSMinGCThreshold` | 20 MiB |
| assist | 默认开启（未设置 `CJ_MS_DISABLE_ASSIST`） |
| 采样 | 关闭（`CASE26_SAMPLE_DURATION_SECONDS=0`） |

注意：首次按旧 Linux 记录尝试 `cjHeapSize=20gb` 时，macOS 当前 Runtime 输出
`Unsupported cjHeapSize parameter`；其支持上限为 16 GiB。因此 20 GiB 那轮结果不纳入
本基线，后续统一使用本节的 16 GiB 合法配置。

### 17.2 基线结果

执行实际耗时 60.168 s，所有请求成功。`rss.csv` 以约 200 ms 间隔记录 benchmark
进程的 user/sys CPU；最后一个累计值为 361.84 CPU 秒（user 318.44 s + sys 43.40 s）。

| 指标 | macOS MSGC 基线 |
| --- | ---: |
| 成功请求 | 143,582 |
| 失败请求 | 0 |
| 报告吞吐 | **2,386 req/s** |
| 精确吞吐（成功数 / 实际时长） | 2,386.35 req/s |
| 累计 CPU 时间 | 361.84 s |
| 每成功请求 CPU | **2.5201 ms/request** |
| 每 CPU 秒成功请求 | **396.81 success/CPU-s** |
| 平均 RSS | 1,953,881 KB |
| 平均 CPU | 587.6% |

产物目录：

- `MSGC_RESULT/Result/case26_http_complex_json/result/20260810_512M_c100_100k_HeapGrow1_Scavenge2_compare_1`
- `msgc/stdout.txt`：请求总数、成功/失败数与吞吐。
- `msgc/rss.csv`：CPU 与 RSS 原始时间序列。
- `msgc/run_metadata.env`：SDK、GC 模式和完整参数快照。

此目录与本节数值构成 macOS 后续优化的起点。后续每项优化至少记录成功数、实际时长、
累计 CPU 秒、CPU/request、success/CPU-s、RSS 和失败数；若 `cjHeapSize`、P 值、payload、
并发、持久对象大小或 SDK 发生变化，则必须新建独立基线，不得与本节直接比较。

## 18. macOS 基线 CPU 采样与下一轮优化方向（2026-08-10）

### 18.1 采样配置、产物与解释边界

在第 17 节相同的 Region-scan MSGC 配置上，只增加 macOS `/usr/bin/sample`：benchmark
启动后等待 10 秒，以 1 ms 间隔采样 30 秒。采样运行的结果为 109,976 成功请求、0 失败、
60.218 s、1,826 req/s；无采样基线为 2,386 req/s。因此本节只使用 sample 定位活跃调用链，
不把采样运行的吞吐、CPU 或 RSS 与第 17 节的正式性能基线直接比较。

产物：

- `MSGC_RESULT/Result/case26_http_complex_json/result/20260810_512M_c100_100k_HeapGrow1_Scavenge2_compare_2/msgc/sample_30s.txt`
- `MSGC_RESULT/Result/case26_http_complex_json/result/20260810_512M_c100_100k_HeapGrow1_Scavenge2_compare_2/msgc/sample.stderr.txt`
- `MSGC_RESULT/Result/case26_http_complex_json/result/20260810_512M_c100_100k_HeapGrow1_Scavenge2_compare_2/msgc/stdout.txt`

`sample` 的 `Sort by top of stack` 共聚合 297,197 个线程栈顶样本。`__psynch_cvwait`、
`__psynch_mutexwait`、`swtch_pri`、`kevent`、`__recvfrom` 等是 parked/socket 等待，不是
CPU 热点，不能据此优化业务代码。下表只列出真正运行中的业务、JSON 和 GC 路径；栈顶计数
不是严格 CPU 百分比，且 `parseJson` / `parseJsonObject` 等递归调用不能互相相加。

### 18.2 当前 TopN 活跃函数

| 路径 | 栈顶计数 | 归因 |
| --- | ---: | --- |
| `parseJsonString` | 10,173 | 当前最大的 JSON 业务热点；无转义路径仍在 Cangjie 中逐 byte 扫描 quote/backslash。 |
| `WriteBuffer.appendEscapeSmall` | 6,083 | 短 key/value 写出逐 byte 判断并写入；本 payload 的普通字段大量落入该路径。 |
| `parseJsonObject` | 5,895 | 对象语法、key 解析与字段插入的总路径。 |
| `HashMap<String, JsonValue>.add` | 5,062 | DOM object 字段建立；对应 `wyhash` 还有 2,926 个样本。 |
| `String.checkInvalid`（benchmark） | 4,580 | 主要来自 HTTP request/response body 的 `String.fromUtf8(...)`，并非 JSON parser 本体。 |
| `parseJson` | 3,843 | DOM 递归分派。 |
| `tryParseJsonIntFast` | 2,904 | 高频整数 token 解析。 |
| `WriteBuffer.checkAndExpend` | 2,836 | 每段输出前容量检查；虽已预留 response 容量，仍有大量小 append 边界。 |
| `JsonObject.writeCompactTo` | 2,086 | compact writer 的 object 遍历和 key/value 输出。 |
| `s2d_n` / `CJ_JSON_ParseFloat64` | 1,825 / 1,075 | Ryu 浮点解析仍有成本，但 libc `strtod` 只有 31 个样本，旧的 libc 格式化/解析主热点已被移除。 |
| `parseJsonNumberSlow` | 1,329 | 浮点和非 fast-int 数字的语法扫描。 |
| `ArrayList<JsonValue>.add` / `WriteBuffer.append(Array)` | 1,148 / 1,112 | DOM array 建立与 JSON 字节复制。 |

Runtime/GC 侧的活跃路径也较高：`MSMarkWordBatchStorage::Add` 为 5,794，
`AddRefToMarkBatch` 为 5,513，`RunRegionTask` 为 4,183，`ForEachBitmapWordFast` 为 2,803，
`TryPublishRegion` 为 2,658。`MSMarkAssist` 本身只有 152 个栈顶样本，sweep assist 也不在
当前首位；当前更符合“DOM 大量对象/引用建立带来持续 mark 工作”，而不是一个单独的 assist
调度故障。此前 writer 的 subtype dispatch 优化仍有效：`TypeInfo::IsSubType` 本次只有 19 个
样本，不再是优先方向。

### 18.3 源码核验

1. `parseJsonString` 在 `stdx/encoding/json/native.cj` 中从 opening quote 后逐 byte 读取，
   直到 quote 或 backslash；无转义分支虽已用 `String.withRawData` 消除字节 clone，但扫描仍在
   Cangjie 循环中。它是本轮最大的 JSON CPU 热点。
2. `WriteBuffer.appendEscapeSmall` 在 `stdx/encoding/json/write_buffer.cj` 中对小于 32 B 的
   字符串逐 byte 进行 control/quote/backslash/HTML-safe 检查，并逐 byte 写到 output buffer。
   `JsonObject.writeCompactTo` 对每个 key 都调用 `appendEscape`，所以短字段名会稳定触发该路径。
3. parser 仍以 `INITIAL_JSON_OBJECT_CAPACITY = 8` 创建所有 `JsonObject`，随后用
   `obj.put(key.getValue(), value)` 写入 `HashMap<String, JsonValue>`。因此字段较多的 event
   object 会集中产生 `HashMap.add`、`wyhash` 和扩容/rehash 成本；不能粗暴提高所有 object 的
   初始容量，因为本 payload 中多数 object 很小。
4. case26 `readHttpBodyString` 已按 `Content-Length` reserve，但仍执行
   `result.add(all: chunk[..n])`、`result.toArray()`、`String.fromUtf8(...)`。这解释了
   `String.checkInvalid` 的 4,580 个样本；它是 benchmark/HTTP body 路径，不应误归因给 DOM parser。
5. `JsonFloat` 的 raw token 回写和 Ryu 写出仍在使用；数字解析也先进入带长度的 `s2d_n`。少量
   `strtod` 是 Ryu 准入外 token 的兼容回退，不是当前主导成本。
6. `RunRegionTask` 对 region 内候选对象逐个枚举引用，并通过 `AddRefToMarkBatch` 将引用按 mark-word
   batch 发布；当前 DOM object/string/array 的短命分配会同时放大这条路径。直接先改 Runtime
   batching 的收益不容易与 JSON 分配下降区分。

### 18.4 下一轮优化优先级

1. **P0：对 `parseJsonString` 做低表示成本的批量扫描 A/B。** 目标是只降低无转义字符串的
   quote/backslash 扫描成本，不恢复第 9 节已验证回退的 `JsonString` raw-token descriptor、
   延迟物化状态或“每个字符串一次 FFI”的方案。优先设计 parser 侧的批量 tokenizer / 阈值化 native
   scan（例如只对较长字符串走 native scan），并先写 parser microbenchmark；必须同时测量
   100K/c100 的 CPU/request 和长期 DOM RSS，确认 FFI 边界没有吞掉收益。
2. **P0：聚焦短字符串 writer，而不是再次改 subtype dispatch。** `appendEscapeSmall` 已经消除旧的
   两阶段 native escape 调用，但仍是第二大 JSON 热点。可候选为“无 escape 的连续 span 一次复制 +
   遇到特殊字符再回退”的实现，或一个 scan-and-copy native helper；两种方案都需要先 microbenchmark。
   关键约束是不要为每个短字符串增加多个 descriptor 字段或多次 RawData acquire/release。当前
   `TypeInfo::IsSubType` 已降至 19 样本，禁止把这一轮资源投入到 writer 类型分派。
3. **P1：选择性处理 12-field event object 的 HashMap 容量。** 保持小 object 的 initial capacity=8；
   在 parser 识别到第 8/9 个字段时再做一次针对性 reserve/rehash 规避，或给已知对象形状提供容量提示。
   A/B 必须同时报告 `HashMap.add`、`wyhash`、RSS、GC 次数与 CPU/request，避免用全局 16/32 容量
   换取少量 event 的收益却扩大绝大多数小 object 的扫描成本。
4. **P1：减少 HTTP body 的受控复制和重复 UTF-8 检查。** 为“读取完成且不再修改的 byte array”提供
   验证 UTF-8 后接管 backing array 的 API，或在 benchmark 的受控路径使用等价所有权转移；不能直接
   将不可信 HTTP 输入改为 `fromUtf8Unchecked`。该项主要消除 `String.checkInvalid` 与大 body clone，
   同时也会减少 `__bzero`、大对象分配与 GC mark 输入。
5. **P1：收紧 WriteBuffer 小 append 边界。** `checkAndExpend` 仍有 2,836 样本，且 exact-fit 分支
   仍使用 `minCapacity < oldCapacity`。先修正为允许 `minCapacity == oldCapacity`，再用基准确认；
   真正收益更可能来自已知容量下的 fast ensure/unchecked append，不能只根据函数计数承诺端到端提升。
6. **P2：数字 parser 和 Runtime mark 作为后续 A/B。** Ryu 已将 `strtod` 压到边缘，继续改 `s2d_n`
   的优先级低于字符串与 HTTP body。GC 侧先在完成上述分配下降后，以 `CJ_MS_DISABLE_ASSIST=1`
   做同参数 A/B；若 `AddRefToMarkBatch`/`RunRegionTask` 仍保持高位，再针对 mark batch 发布、region
   claim/publish 进行 Runtime 级优化。

下一轮建议先实施并独立验证 P0 的第 2 项（短字符串无 escape 写出），原因是其热点紧邻
`parseJsonString`、改动局限于 DOM writer 且不需要改变 `JsonString` representation。实施前先增加
短 key/value、HTML-safe `&`、控制字符、quote/backslash 的正确性回归；实施后以第 17 节的
2.5201 ms/request 为唯一正式性能对照，并重新抓同配置 30 秒 sample 验证
`appendEscapeSmall` 是否从高位路径下降。

## 19. `parseJsonString` 原生批量扫描单次验证（2026-08-10）

### 19.1 实现

在 `cangjie_stdx/src/stdx/encoding/json/native.cj` 的 `parseJsonString` 中加入
`CJ_JSON_ScanUnescapedString`。opening quote 已消费后，该 native helper 在输入数组上连续扫描：
遇到 closing quote 时直接返回 offset；遇到 `\\` 或到达输入末尾则返回 `INT64_MIN`，并完全回退到
原有 Cangjie 转义解码循环。无转义成功路径仍只构造现有的 `String.withRawData` view，未恢复已验证
回退的 raw-token descriptor / 延迟物化表示。

该实现的目标是降低逐 byte Cangjie 循环的表示成本；但每次 JSON string 都会跨一次 FFI，因而必须用
端到端运行检验短字符串负载中的调用边界成本。

### 19.2 构建与功能验证

修改涉及 stdx，使用固定 SDK 脚本完成重编安装：

```text
runtime/build/scripts/incremental_rebuild_msgc_sdk.py
```

验证使用安装后的 `output/msgc`，且显式传入 `--gc-mode=marksweep`。新增回归
`cangjie_stdx/test/dom_json_native_batch_scan.cj` 覆盖无转义快路径、含转义回退路径和未闭合字符串错误；
执行结果为 `PASS dom_json_native_batch_scan`。源级契约测试
`test_dom_json_native_batch_scan.py` 也通过。

### 19.3 同参数单次 Case26 结果

按用户要求，本项没有重新做严格 A/B，只在完成安装后使用第 17 节相同参数跑了一次 MSGC：100K、c100、
60 s、512 MiB persistent / 8 workers、`PROCS=10`、`cjHeapSize=16gb`、HeapGrow=1、Scavenge=2、
`cjMSMinGCThreshold=20mb`，采样关闭。

| 指标 | 本次单次结果 |
| --- | ---: |
| 成功请求 | 121,267 |
| 失败请求 | 0 |
| 实际时长 | 60.158 s |
| 报告吞吐 | **2,015 req/s** |
| 按成功请求计算的精确吞吐 | 2,015.81 req/s |

产物目录：

- `MSGC_RESULT/Result/case26_http_complex_json/result/20260810_512M_c100_100k_HeapGrow1_Scavenge2_compare_3`

虽然这不是严格 A/B，但其吞吐低于第 17 节的 Mac 基线 2,386.35 req/s，不能将该实现宣称为收益。
结合本 payload 的 key/value 基本都是短字符串，最可能的原因是每个 string 的 `acquireArrayRawData` /
`releaseArrayRawData` 与 FFI 调用成本超过了原本很短的 Cangjie 扫描循环。后续若继续该方向，应改为
长度阈值（仅长字符串进入 native scan）或把扫描并入 parser/tokenizer 的单一 native 边界，再进行正式
CPU/request A/B；在此之前不应把这条无阈值路径视为默认优化。

## 20. 短字符串 writer：span-copy 与 native scan-and-copy 对比（2026-08-10）

`appendEscapeSmall` 是第 18 节第二高的 JSON 热点。本轮不再修改 writer 的 subtype dispatch，也没有给
`JsonString` 增加 descriptor。两版都保持既有转义语义，并用同一 Region-scan MSGC Case26 参数运行：100K、
c100、60 s、512 MiB persistent / 8 workers、`PROCS=10`、`cjHeapSize=16gb`、HeapGrow=1、Scavenge=2、
`cjMSMinGCThreshold=20mb`。

| 版本 | 实现 | 成功 / 失败 | 吞吐 | 累计 CPU | CPU / 成功请求 | 成功 / CPU-s |
| --- | --- | ---: | ---: | ---: | ---: | ---: |
| span-copy | Cangjie 扫描 special byte；普通连续区间用一次 `Array.copyTo` 写入 | 145,357 / 0 | **2,414 req/s** | 370.77 s | **2.5508 ms** | **392.04** |
| native scan-and-copy | 每个短字符串获取一对 input/output RawData，native 扫描后 `memcpy`；遇 special byte 回退 | 125,197 / 0 | 2,080 req/s | 362.51 s | 2.8955 ms | 345.36 |

产物：

- span-copy：`MSGC_RESULT/Result/case26_http_complex_json/result/20260810_512M_c100_100k_HeapGrow1_Scavenge2_compare_4`
- native：`MSGC_RESULT/Result/case26_http_complex_json/result/20260810_512M_c100_100k_HeapGrow1_Scavenge2_compare_5`

span-copy 的吞吐高 16.1%，CPU/request 低 11.9%，因此保留 span-copy。native 版本已从最终源码移除；
它在 no-escape 路径中避免了 Cangjie 的逐 byte 写入，但 Case26 的 key/value 大多不足 32 B，FFI 与
RawData 边界固定成本仍大于其收益。最终版本只改 `cangjie_stdx`，使用 `build.py build/install` 安装到
MSGC SDK；没有重建 compiler、Runtime 或 stdlib。短字符串普通、quote/backslash/control、HTML-safe `&`
与 `U+007F` 回归均通过。

## 21. HTTP body 最终数组直读与校验后接管（2026-08-10）

### 21.1 实现

本节更新第 8.1 节所述 HTTP body reader 的当前实现；第 8 节的旧实验数据保留为历史记录。本次只修改
`case26_http_complex_json/benchmark.cj`，不修改 Runtime、stdlib 或 stdx，因此由 case26 compare 脚本使用
已安装的 MSGC SDK 以 `-O2 --gc-mode=marksweep` 重新编译即可。

原路径在已知 `Content-Length` 时仍是：底层 body 写入 64 KiB `chunk`，复制到 `ArrayList`，`toArray()`
复制为精确数组，最后 `String.fromUtf8()` 在 UTF-8 校验后再 clone 一份数组。当前改为：

1. `bodySize=Some(size)` 且 `size > 0` 时，直接创建精确大小的 `Array<UInt8>`；每次
   `body.read(data[used..size])` 直接写入该数组的剩余 view。`Array` slice 只调整 start/length，
   不复制 body 字节。
2. 读完后使用 `String.checkUtf8Encoding` 保留原有不可信 HTTP 输入的 UTF-8 校验；校验成功才在私有、
   后续不再修改的数组上调用 `String.withRawData`，让 String 持有同一个 `RawArray`，不再 clone。
3. `bodySize=None` 仍使用 `ArrayList` 累积，但直接以 `getRawArray()[..size]` 的有效 view 进入相同的
   校验和接管步骤，去掉原先 `toArray()` 与 `fromUtf8()` 的两次整包复制。

`withRawData` 没有提升为通用公开 UTF-8 API；其调用被限制在该 helper 中，以保持 `String.fromUtf8` 对外
“输入数组后续仍可修改”的 clone 语义。无效 UTF-8 仍会抛出 `IllegalArgumentException`，没有改用
`fromUtf8Unchecked`。

### 21.2 正确性验证

新增 `BodyReadProbe` 自测输入流，并将 body 长度设为 `64 KiB + 1`。改动前，首次 read 的目标 buffer
固定为 64 KiB，新增断言按预期失败；改动后首次目标 buffer 等于完整 Content-Length，证明已知长度路径
直接写入最终数组。安装后的 MSGC SDK 使用显式 `--gc-mode=marksweep` 编译并运行
`CASE26_BODY_READ_SELF_TEST=1`，结果通过以下场景：

- 已知 Content-Length；
- 未知 body 长度；
- 大于 64 KiB 的已知长度 body；
- `HttpResponseBuilder.body(String)` 构造的 client response body。

### 21.3 单次 MSGC 10 秒验证

按第 17 节相同的 Mac 参数，仅把时长按最小化验证规则设为 10 秒：100K、c100、512 MiB persistent / 8 workers、
`PROCS=10`、`cjHeapSize=16gb`、HeapGrow=1、Scavenge=2、`cjMSMinGCThreshold=20mb`，仅启用 MSGC，关闭 sample。

| 指标 | 本次结果 |
| --- | ---: |
| 实际压测时长 | 10.129 s |
| 成功 / 失败 | 24,250 / 0 |
| 报告吞吐 | **2,394 req/s** |
| 累计 CPU 时间 | 60.41 s |
| CPU / 成功请求 | 2.4911 ms |
| success / CPU-s | 401.42 |

产物目录：

- `MSGC_RESULT/Result/case26_http_complex_json/result/20260810_512M_c100_100k_HeapGrow1_Scavenge2_compare_7`

本次短测确认新路径正确且无失败；由于没有为本项运行同条件的优化前 10 秒 A/B，且 10 秒累计 CPU 含启动、
持久对象初始化和 warmup 影响，不能将其与第 17 节的 60 秒基线或第 8 节的历史数据做严格收益归因。后续若要
量化该项的独立收益，应以同一 SDK、同一 10 秒配置进行优化前/后 CPU per success 对照，或各运行足够长的
稳定窗口后再比较。

### 21.4 结论与回退

本轮没有观察到足以支持保留该优化的明确收益，按结论将其从当前 `benchmark.cj` 移除，恢复为
`chunk -> ArrayList -> toArray -> String.fromUtf8` 路径，同时删除仅服务于直读实现的 `BodyReadProbe`
自测。第 21 节保留为已验证但未纳入默认实现的实验记录；其中的 10 秒数据不计入后续优化基线。

## 22. `parseJsonString` 16-byte 双字符批量扫描（2026-08-10）

### 22.1 实现

按第 18.4 的 P1/P2 方向，本轮将无转义 JSON string 的扫描从逐 byte 的 Cangjie 循环改为 16-byte
批量探测。公开调用点命名为 **`vectorIndexByte16`**；它接收 `Array<Byte>`、起始 offset、`'"'` 和
`'\\'` 两个目标 byte，返回区间内最早命中的相对位置，未命中返回 16。

实现分三层：

1. `std.core` 的 `vectorIndexByte16` 只提取既有 `Array.rawptr + Array.start`，随后调用
   `vectorIndexByte16Raw` intrinsic；没有新增 `JsonString` descriptor，也没有
   `acquireRawData` / `releaseRawData`。
2. compiler 为 `vectorIndexByte16Raw` 生成固定 `<16 x i8>` load、两次 byte splat/equal、OR 和首位
   命中计算。没有读取 `CJ_CORE_CanUseSIMD`，因此不存在“macOS AArch64 永远启用”的运行时开关；LLVM
   会按每个 Cangjie Native 目标将 fixed-vector IR 合法化，目标缺少对应向量指令时由后端降为合法的标量序列。
3. `parseJsonString` 仅在 `offset + 16 <= size` 时进入批量循环；发现 quote 或 backslash 后仍回到原有
   标量分支，分别走现有的 `String.withRawData` 无转义返回和转义解码逻辑。尾部不足 16 B 也保持原循环。

本机安装后的 `libstdx.encoding.json.dylib` 反汇编确认 `parseJsonString` 已生成 AArch64 NEON
`ldur q0`、`cmeq.16b`、`orr.16b` 等 16-byte 比较指令，而非运行时 CPU 特性分支。

### 22.2 构建与正确性验证

本轮同时修改 compiler、stdlib 与 stdx，因此严格使用：

```text
runtime/build/scripts/incremental_rebuild_msgc_sdk.py
```

脚本成功完成并安装到 `output/msgc`。新增结构契约测试
`cangjie_stdx/test/test_json_vector_index_byte16.py` 已通过；新增/扩展的
`cangjie_stdx/test/dom_json_short_escape.cj` 使用安装后的 SDK、显式
`--gc-mode=marksweep` 编译运行通过，覆盖：

- 普通短无转义字符串；
- 恰好跨过 16-byte 批量边界的无转义字符串；
- 16-byte 范围内出现 `\\n` 的转义回退；
- 原有 quote、backslash、HTML-safe `&` 与 Unicode escape 行为。

### 22.3 单次 MSGC 10 秒验证

按简洁验证规则，仅启用 MSGC、关闭 sample，参数为 100K / c100 / 10 s、512 MiB persistent / 8 workers、
`PROCS=10`、`cjHeapSize=16gb`、HeapGrow=1、Scavenge=2、`cjMSMinGCThreshold=20mb`。

| 指标 | 本次结果 |
| --- | ---: |
| 实际压测时长 | 10.122 s |
| 成功 / 失败 | 22,633 / 0 |
| 报告吞吐 | **2,236 req/s** |
| 精确吞吐（成功 / 实际时长） | 2,236.02 req/s |
| 累计 CPU 时间 | 61.29 s |
| 每成功请求 CPU | 2.7080 ms |
| 每 CPU 秒成功请求 | 369.28 |

产物目录：

- `MSGC_RESULT/Result/case26_http_complex_json/result/20260810_512M_c100_100k_HeapGrow1_Scavenge2_compare_8`

### 22.4 结论

本次确认 intrinsic、stdlib wrapper、stdx parser 和安装后的 MSGC SDK 全链路生效，且没有请求失败。
但此处只有一次 10 秒验证；其 CPU/request 高于第 17 节的 60 秒基线（2.5201 ms），吞吐也低于该基线。
10 秒窗口仍包含启动、持久对象初始化和 warmup，不能据此将差异完全归因于本优化，也不能宣称有端到端收益。
后续若继续评估，应以相同 10 秒窗口对优化前/后各运行多次，并以 CPU/request 为主判据；若仍无改善，
应回退此批量扫描，转向 `appendEscapeSmall` 或 DOM object 容量方向。

### 22.5 60 秒复测：确认没有端到端收益

为排除 10 秒窗口的 warmup 与启动扰动，使用与第 17 节 Mac 基线完全相同的 60 秒配置再次仅运行 MSGC：
100K / c100、512 MiB persistent / 8 workers、`PROCS=10`、`cjHeapSize=16gb`、HeapGrow=1、
Scavenge=2、`cjMSMinGCThreshold=20mb`，关闭 sample。

| 指标 | 第 17 节 Mac 基线 | 本轮 vectorIndexByte16 | 变化 |
| --- | ---: | ---: | ---: |
| 实际压测时长 | 60.168 s | 60.199 s | — |
| 成功 / 失败 | 143,582 / 0 | 121,938 / 0 | — |
| 精确吞吐（成功 / 实际时长） | 2,386.35 req/s | 2,025.58 req/s | **-15.12%** |
| 累计 CPU 时间 | 361.84 s | 323.05 s | — |
| 每成功请求 CPU | 2.5201 ms | 2.6493 ms | **+5.13%** |
| 每 CPU 秒成功请求 | 396.81 | 377.46 | **-4.88%** |

产物目录：

- `MSGC_RESULT/Result/case26_http_complex_json/result/20260810_512M_c100_100k_HeapGrow1_Scavenge2_compare_9`

结论更新：60 秒结果确认该实现没有收益，且 CPU 效率发生回退；不应将其保留为默认路径。虽已生成
NEON 的 16-byte load/compare，但当前“命中 mask → 首个 index”的 lowering 仍包含多条逐 lane 提取和
位组合指令，抵消甚至超过了逐 byte 扫描的成本；而 payload 中大量字符串本身较短，也限制了批量路径的
覆盖率。若再尝试向量化，需要先改进该 mask-to-index 降低方式，并以同样的 60 秒 CPU/request 验证；否则应
回退本轮 `vectorIndexByte16` 代码。
