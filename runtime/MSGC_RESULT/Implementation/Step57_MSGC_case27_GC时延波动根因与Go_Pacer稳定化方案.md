# Step57：MSGC case27 GC 时延波动根因与 Go Pacer 稳定化方案

## 1. 结论

本次 case27 中，请求吞吐按 2 秒窗口观察较稳定，但 MSGC 单轮 GC wall time
仍从数毫秒波动到 117.925ms。日志表明：

1. 主要波动不在 Sweep，也不在常规 STW，而在 `ConcurrentMark`。
2. `ConcurrentMark` 的工作量取决于 GC 实际看到的对象图，不只取决于外部请求速率。
   100 个同步请求即使整体吞吐稳定，也可能在 GC 窗口内处于不同的解析、构造和序列化阶段。
3. MSGC 当前 heap threshold 在相邻周期之间变化过大。短周期 bitmap live 降低时，
   threshold 立即下降；遇到 Mark 期间大分配时又由 live safety floor 快速抬高，
   形成触发频率和单轮工作量的交替震荡。
4. 当前 Mark helper 和 Assist 主要根据已形成的 WorkBuf backlog 事后升压，
   缺少类似 Go 的“预计扫描工作量、分配速率、扫描速率、剩余 heap runway”闭环。
5. 刚移除的 `inUseBurst >= maxHeap / 4` 条件能够识别本次历史数据中
   13 个最明显的 floating-live 长周期，但只能防止这些样本污染后续稳定基线，
   不能缩短已经发生的当轮 Mark。

因此，稳定化目标不应定义为“每轮 GC wall time 完全相同”，而应拆成：

- 把 STW/request p99 控制在明确预算内；
- 让 GC 在 heap goal 前稳定完成，避免偶发超大 Mark 工作集；
- 固定或平滑 GC CPU 占用，避免 helper/assist 突然抢占 Mutator；
- 降低 heap goal、trigger point 和 Assist ratio 的轮间震荡；
- 在保持吞吐和 RSS 可接受的前提下降低 GC wall time P95/P99。

建议优先实现：

```text
指标拆分
  → heap goal 与 trigger point 分离
  → 基于 allocRate/scanRate 的 runway pacer
  → 固定 GC CPU 配额
  → 轮内动态 Assist
  → Sweep/Scavenge 带宽协调
```

## 2. 测试环境与数据来源

### 2.1 结果目录

```text
MSGC_RESULT/Result/case27_http_complex_json/result/
20260726_150527_output_msgc_vs_copygc_go_c100_p100K_compare/
```

主要文件：

- `compare_report.md`：CopyGC、MSGC、Go、Java 汇总。
- `msgc/gc.log`：2805 轮 MSGC 的分阶段日志。
- `go/gc.log`：3481 轮 Go `GODEBUG=gctrace=1` 日志。
- `*/rss.csv`：RSS 和 CPU 时间序列。
- `*/stdout.txt`：2 秒吞吐进度和最终结果。
- `*/run_metadata.env`：SDK、堆大小、并发和 GC 参数。

### 2.2 MSGC 参数

```text
payload                 = 100K
concurrency             = 100
duration                = 60000ms
client_mode             = sync
send_interval           = 1ms
max_inflight            = 32
cjHeapSize              = 12GB
cjHeapGrowth            = 1
cjScavengeGrowth        = 5
SDK                     = cangjie_compiler/output/msgc
cjc_gc_mode             = marksweep
runtime dylib md5       = c5adedd63ec68d4ea97cca267bf7cab0
```

`run_metadata.env` 中记录了 `cj_ms_min_gc_threshold=20mb`，但当前 Runtime
没有读取 `cjMSMinGCThreshold` 的实现。源码仍在 `UpdateGCStats()` 中固定使用
`min(maxHeap, 20MB)` 作为 heap threshold floor，因此该 metadata 字段不能视为
一次实际生效的调优。

### 2.3 对照侧

- CopyGC：固定 Dev SDK `dev/20260507/cangjie`。
- Go：Go 1.26.0、darwin/arm64，默认 GOGC，启用 `GODEBUG=gctrace=1`。
- Java：同一 payload、并发和持续时间的 Java 实现。

该对比可以观察各 Runtime 的 GC 控制效果，但不同语言的对象布局、逃逸分析、
HTTP/JSON 实现和堆分配量不同，不能把 live heap 或 GC 次数直接解释成同一对象图上的
纯算法差异。

## 3. 指标口径：GC wall time 不等于 STW

`compare_report.md` 中的“GC 单次耗时”来自整个 GC cycle 的 wall time。
对 MSGC，它包含：

```text
PreEnum
  + EnumRoots
  + ConcurrentMark
  + MarkTermination
  + PostMark
  + PostGC
  + Other
```

其中 `ConcurrentMark` 与 Mutator 并发执行。因此：

```text
GC cycle wall time ≠ STW pause ≠ 单请求延迟
```

当前 MSGC 明确的暂停窗口主要包括：

1. `PreEnumRoots()` 中 `ScopedStopTheWorld` 包裹的 bitmap clear、
   residual reclassify 和 sweep generation prepare。
2. `MarkTermination()` 中冻结 Mutator、收口普通闭包、Finalizer resurrection、
   WeakRef 处理和 per-P WorkBuf 清理。
3. `EnumRoots()` 使用 phase handshake 让各 Mutator 在 safepoint 扫描自身栈根；
   该阶段会影响请求延迟，但不等价于单一全局 STW wall time。

按现有日志估算：

| 指标 | 平均 | P50 | P95 | P99 | 最大 |
| --- | ---: | ---: | ---: | ---: | ---: |
| PreEnum 核心 STW | 0.249ms | 0.225ms | 0.442ms | 0.619ms | 1.575ms |
| MarkTermination | 0.890ms | 0.706ms | 1.987ms | 4.110ms | 17.835ms |
| 两者合计 | 1.139ms | 0.958ms | 2.239ms | 4.338ms | 18.063ms |
| 再加 EnumRoots handshake | 2.073ms | 1.857ms | 3.393ms | 5.524ms | 18.954ms |

上表仍不是严格的请求 pause 分布：`MarkTermination total` 的日志计时点没有覆盖
整个 `ScopedStopTheWorld` 析构和恢复世界成本，单请求也可能受到调度、Assist、
allocator 锁和 CPU 竞争影响。后续验证必须给 benchmark 增加请求 latency histogram，
至少输出 p50、p95、p99 和最大值。

## 4. MSGC 波动的定量证据

### 4.1 汇总对比

| 指标 | CopyGC | MSGC | Go | Java |
| --- | ---: | ---: | ---: | ---: |
| 吞吐（req/s） | 658 | 825 | 896 | 1774 |
| GC 次数 | 226 | 2805 | 3481 | 524 |
| GC 平均时间 | 146.63ms | 14.39ms | 8.31ms | 6.17ms |
| GC P95 | 203.42ms | 39.14ms | 11.16ms | 16.70ms |
| GC 最大时间 | 292.82ms | 117.92ms | 36.11ms | 35.17ms |
| RSS 平均 | 1764.53MB | 834.26MB | 158.27MB | 1675.04MB |
| RSS 峰值 | 2509.59MB | 975.83MB | 164.59MB | 1755.77MB |

MSGC 的 2 秒请求进度样本平均为 1654.9 个请求，最小 1603、最大 1745，
变异系数约 1.83%，说明外部吞吐相对稳定。

### 4.2 MSGC 分阶段分布

| 阶段 | 平均 | P50 | P95 | P99 | 最大 | 占累计 GC 时间 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| PreEnum | 0.340ms | 0.247ms | 0.543ms | 1.402ms | 11.805ms | 2.4% |
| EnumRoots | 0.935ms | 0.866ms | 1.324ms | 2.200ms | 9.312ms | 6.5% |
| ConcurrentMark | 11.334ms | 5.980ms | 35.811ms | 55.607ms | 114.866ms | 78.8% |
| MarkTermination | 0.890ms | 0.706ms | 1.987ms | 4.110ms | 17.835ms | 6.2% |
| PostMark | 0.320ms | 0.283ms | 0.607ms | 0.924ms | 4.982ms | 2.2% |
| Other | 0.570ms | 0.322ms | 1.621ms | 2.851ms | 18.545ms | 4.0% |
| 整轮 | 14.389ms | 9.083ms | 39.121ms | 59.050ms | 117.925ms | 100% |

`ConcurrentMark` 是唯一同时具有高占比、高 P95 和高最大值的阶段。

### 4.3 相关性

| 对比项 | Pearson 相关系数 |
| --- | ---: |
| GC total 与 ConcurrentMark | 0.991 |
| GC total 与 trueLive | 0.985 |
| ConcurrentMark 与 trueLive | 0.987 |
| ConcurrentMark 与本轮 processed objects | 0.987 |
| processed objects 与 trueLive | 0.996 |
| GC total 与 markEndInUse | 0.831 |
| GC total 与 stopSweep | 0.060 |

这组数据支持以下判断：

- 长尾首先由本轮实际扫描工作量决定。
- Mark 结束时的 in-use 增长会扩大对象图，但不是唯一因素。
- `StopAndWaitForAllSweep()` 在本次数据中不是 GC 总时间波动的主导项。

### 4.4 短周期与长尾周期

| 指标 | `<10ms` 周期 | `>=50ms` 周期 |
| --- | ---: | ---: |
| 周期数 | 1512 | 58 |
| 平均 GC total | 6.735ms | 63.505ms |
| 平均 ConcurrentMark | 4.182ms | 58.623ms |
| 平均 trueLive | 43.7MB | 306.8MB |
| 平均 markEndInUse | 136.3MB | 439.3MB |
| 平均 Mark 期间 burst | 3.2MB | 262.5MB |
| 平均 processed objects | 15.6 万 | 155.3 万 |
| 最终 pressure level=2 | 9.7% | 60.3% |
| noProgress=1 | 9.7% | 58.6% |

长尾周期平均扫描对象数约为短周期的 9.9 倍。pressure level 和 no-progress
在长周期中明显增多，说明动态升压通常是在大工作集已经形成之后发生。

### 4.5 触发节奏与 threshold 震荡

- MSGC GC 启动间隔：平均 21.49ms、P50 15ms、P95 54ms、P99 78ms、最大 124ms。
- Go GC 启动间隔：平均 17.29ms、P50 17ms、P95 23ms、P99 27ms、最大 59ms。
- MSGC 2805 轮中：
  - threshold 下降 1190 次；
  - threshold 上涨 1610 次；
  - 不变 5 次；
  - 相邻周期 threshold 平均相对变化约 43.5%。

当前 `EvaluateMSHeapThreshold()` 对下降不做限幅，对上涨使用 12.5% rise limit，
但 `liveSafetyFloor = live * 1.25` 可以在 live 突增时越过 rise limit。
这能避免阈值低于当前 live，却也让 floating-live 样本把 threshold 瞬间抬高。

## 5. 典型长尾周期：cycle 2081

cycle 2081 是本次 MSGC 运行中 wall time 最长的一轮：

| 指标 | 数值 |
| --- | ---: |
| Mark 开始 in-use | 97.375MB |
| Mark 结束 in-use | 684.656MB |
| Mark 期间 burst | 587.281MB |
| bitmap true live | 548.948MB |
| 旧 threshold | 85.610MB |
| raw target | 1097.896MB |
| live safety floor | 686.185MB |
| 新 threshold | 686.185MB |
| processed objects | 2,906,719 |
| GC total | 117.925ms |

分阶段时间：

| 阶段 | 时间 |
| --- | ---: |
| PreEnum | 0.173ms |
| EnumRoots | 0.983ms |
| ConcurrentMark | 114.866ms |
| MarkTermination | 0.760ms |
| PostMark | 0.912ms |
| Other | 0.230ms |

`ConcurrentMark` 占该轮 wall time 的 97.4%。处理的 2,906,719 个对象中，
helper 处理 1,913,963 个，Mutator Assist 处理 946,176 个，主 Mark 线程处理
46,580 个。最终 pressure level 为 1，`noProgress=0`，说明这一轮并非线程卡住，
而是确实遇到了约 290.7 万个对象的巨大 Mark 工作集。

这一轮从 97.375MB 开始，Mark 期间 in-use 增加 587.281MB，bitmap true live
达到 548.948MB。旧策略随后用 `live * 1.25` 的安全下限把 threshold 从
85.610MB 直接提高到 686.185MB。它保护了下一轮不在低于当前 live 的位置触发，
但也把一次瞬时放大的对象图直接反馈进 threshold 控制器。

该样本同时说明“请求速度稳定”和“单轮 Mark 工作量稳定”是两个不同条件：
2 秒聚合吞吐只能说明平均到达和完成速率稳定，不能保证某个十几毫秒窗口内
100 个同步请求恰好处于相同的对象生命周期阶段。

## 6. 根因分析

### 6.1 瞬时对象图和 SATB floating live

并发 Mark 统计的是该轮 GC 必须保持可达的对象图。对于 HTTP complex JSON，
单个请求会经历接收、解析、构造中间对象、业务处理、序列化和发送等阶段。
总吞吐稳定时，请求之间的阶段重叠仍会改变某轮 GC 看到的根集合、对象数和引用边数。

SATB 语义还会保留 Mark 开始时可达、随后在并发 Mark 期间死亡的对象。
因此 bitmap live 可以包含：

- Mark 开始时真实存活的对象；
- Mark 期间仍然可达的对象；
- SATB 为保证正确性而保留到本轮结束的 floating garbage。

这解释了为什么 `trueLive`、processed objects 与 `ConcurrentMark` 高度相关，
而请求吞吐的 2 秒窗口仍然稳定。

### 6.2 Threshold 控制器放大轮间震荡

当前 threshold 的核心输入是上一轮 bitmap live：

```text
rawTarget       = live * (1 + heapGrowth)
liveSafetyFloor = live * 1.25
```

下降立即生效；普通上涨最多 12.5%，但 `liveSafetyFloor` 可以越过上涨限幅。
其结果是：

1. 短周期 live 下降后，threshold 很快降低，GC 更早、更频繁地启动；
2. 某轮 Mark 遇到大量分配和 floating live 时，安全下限又把 threshold 快速抬高；
3. 更晚启动的下一轮可能积累更大的对象图，然后再次产生长 Mark；
4. live 回落后 threshold 又迅速下降。

本次相邻周期 threshold 平均相对变化 43.5%，正是这个反馈回路缺少稳定
heap goal 和迟滞控制的表现。

### 6.3 Mark 资源控制是事后响应，不是提前配速

`MSMarkTerminationPolicy` 根据已经堆积的 WorkBuf backlog 和 no-progress 窗口
提高 helper pressure；`MSAssistPacing` 主要根据上一轮对象数与分配量计算基础比例，
并在 Assist share 超过 90% 时做反馈。

这两条路径都缺少以下轮内预测量：

- 当前分配速率；
- 当前扫描速率；
- 剩余扫描工作量；
- 距离 heap goal 的剩余空间；
- 按当前速率能否在耗尽 runway 前完成 Mark。

所以小对象图时资源可能过量，大对象图时又要等 backlog 已经形成后才升压。
长尾组中 `pressure level=2` 和 `noProgress=1` 的比例显著升高，是这种滞后控制的
结果，而不是长尾的独立根因。

### 6.4 “处理对象数”不是稳定的 Mark 工作单位

同样大小的对象可能具有完全不同的引用密度；同样数量的对象也可能需要扫描不同数量的
字段、数组元素和跨 Region 边。以对象数估算 Assist 比例会把“一个无引用小对象”和
“一个包含大量引用的数组对象”近似为同等工作。

更稳定的 Pacer 应统计 scan bytes 或 scan edges，并分别记录 heap、stack、
global roots 的扫描工作量。对象数仍可保留用于诊断，但不应作为唯一的工作预算。

### 6.5 Sweep 不是本次主因，Scavenge 重叠仍待验证

本次 `GC total` 与 `stopSweep` 的相关系数仅 0.060，且绝大部分长尾集中在
`ConcurrentMark`，因此不能把主因归到 `StopAndWaitForAllSweep()`。

现有 Sweep 已按 threshold runway 和 swept bytes 做一定比例控制。后续应验证其
实际速率是否与分配需求匹配，但不应先用“全量提前 Sweep”替代 Mark Pacer。

Scavenge 可能与 Mark 争用内存带宽或页锁，但当前日志没有精确记录其开始/结束、
回收字节和与 Mark 的重叠窗口。这个方向目前只是待验证假设，不能据此直接关闭
Scavenge。

### 6.6 同步请求可能放大相位对齐，但尚未证实

case27 使用 100 个同步 client。同步模式可能让大量请求在相近时间进入 JSON
解析或响应构造阶段，使某些 GC 窗口看到更大的瞬时对象图。验证方法是保持总吞吐
近似不变，分别运行：

- 当前同步模式；
- 给请求加入小范围随机 jitter；
- 限制或平滑并发启动；
- 等吞吐的异步模式。

若 trueLive、processed work 和 GC P99 随相位打散显著下降，才能确认相位对齐是
放大器；它仍不替代 Runtime 自身的稳定 Pacer。

## 7. 新 floating-live 判定的作用与边界

旧判定要求同时满足：

```text
markEndInUse / markStartInUse >= 4
trueLive > 2 * markStartInUse
inUseBurst >= maxHeap / 4
```

case27 的最大堆为 12GB，因此第三个条件要求 Mark 期间至少增长 3GB。
但 cycle 2081 的 burst 为 587.281MB，虽然已经出现明显 floating-live 膨胀，
仍远达不到 3GB。这个条件会随着最大堆配置线性放大，不能表达本轮 bitmap live
相对于 Mark 起点是否异常。

当前代码已移除第三个条件，只保留：

```text
markEndInUse / markStartInUse >= 4
trueLive > 2 * markStartInUse
```

用新条件离线回放旧 `msgc/gc.log`：

| 指标 | 结果 |
| --- | ---: |
| 全部周期 | 2805 |
| `>=50ms` 长尾周期 | 58 |
| 新判定命中的周期 | 13 |
| 命中且属于 `>=50ms` 长尾 | 13 |
| 覆盖的最长周期 | cycle 2081、2380、2460、262 |

它精确覆盖了 13 个明显的 floating-live 长尾样本，并包含最慢的 4 轮；但它只覆盖
58 个 `>=50ms` 周期中的 13 个，不能解释或消除全部长尾。

该防护的作用是：当本轮 bitmap live 很可能被 Mark 期间的瞬时对象图放大时，不让它
直接成为稳定 live 基线并污染下一轮 threshold。它不能：

- 减少已经进入 bitmap 的对象；
- 缩短当轮 `ConcurrentMark`；
- 代替 trigger/runway Pacer；
- 处理没有满足 4 倍增长条件的普通大 live 周期。

另外，当前分析目录中的日志来自条件移除之前，2805 轮均记录
`floatingLiveInflated=0`。上表是对旧日志做的新谓词离线模拟，不是新 Runtime
已经运行后的实测效果。必须重新构建 Runtime、替换 dylib、重新编译 `.cj` 用例并
复跑 case27，才能确认后续 threshold 和 GC P95/P99 的实际变化。

## 8. Go 1.26 如何控制 GC 波动

本次 Go 1.26.0 的 3481 轮 `gctrace` 统计如下：

| 指标 | 平均 | P50 | P95 | P99 | 最大 |
| --- | ---: | ---: | ---: | ---: | ---: |
| GC cycle wall | 8.314ms | 8.240ms | 11.161ms | 13.210ms | 36.109ms |
| Concurrent Mark | 8.131ms | — | 11.000ms | — | 36.000ms |
| 两端 STW 合计 | 0.184ms | 0.178ms | 0.280ms | 0.340ms | 0.869ms |
| live heap | 48.78MB | 49MB | 60MB | 64MB | 69MB |
| heap goal | 99.11MB | 100MB | 121MB | 130MB | 140MB |

Go 请求吞吐时间序列的变异系数约 5.93%，并不比 MSGC 的 1.83% 更稳定。
因此这里的结论不是“Go 的外部请求更平滑”，而是 Go 的 live heap、heap goal、
触发间隔和 STW 更集中。

以下机制依据 Go 1.26 当前 Runtime 源码；[GC guide](https://go.dev/doc/gc-guide)
用于解释成本模型，具体实现以
[`mgcpacer.go`](https://go.dev/src/runtime/mgcpacer.go)、
[`mgc.go`](https://go.dev/src/runtime/mgc.go)、
[`mgcsweep.go`](https://go.dev/src/runtime/mgcsweep.go) 和
[`mgclimit.go`](https://go.dev/src/runtime/mgclimit.go) 为准。

### 8.1 稳定 heap goal，并把 goal 与 trigger 分开

Go 的基础 heap goal 由上轮标记存活量、栈扫描量、全局扫描量和 GOGC 计算：

```text
goal = heapMarked
     + (heapMarked + lastStackScan + globalsScan) * GOGC / 100
```

heap goal 表示本轮允许增长到的位置；trigger point 则要更早启动 GC，为并发 Mark
预留 runway。两者分开后，“希望堆增长多少”和“按当前速度应该何时开始”不再由
一个 threshold 同时承担。

Pacer 根据分配速率与扫描速率估算 `consMark`，计算需要的 runway，使 Mark
尽量在 heap goal 附近完成。它还保留最近 4 轮 `lastConsMark`，避免完全跟随
单轮噪声。

### 8.2 用 scan work 表示成本

Go 将 heap、stack 和 global 的扫描字节纳入工作量，而不是只数对象个数。
这更接近 Mark 的实际 CPU 成本，也能让“剩余工作 / 剩余堆空间”的比值具有稳定单位。

### 8.3 给后台 Mark 一个稳定 CPU 预算

Go 默认把 `gcBackgroundUtilization` 设为 0.25，即后台 Mark 目标约占
`GOMAXPROCS` 的 25%。Runtime 组合使用：

- dedicated worker：独占一个 P 做 Mark；
- fractional worker：只使用某个 P 的一部分时间；
- idle worker：只消费当前空闲 CPU。

这不是固定线程数，而是固定 GC CPU 利用率目标。这样不同核数下都能保持近似的
Mutator/GC 资源比例，idle worker 又能利用业务暂时空闲的算力。

### 8.4 Assist 在轮内按剩余工作持续修正

当后台 Mark 按当前速度无法在 goal 前完成时，Go 让分配者按分配字节承担
Mark Assist。核心比例近似为：

```text
assistWorkPerByte = remainingScanWork / remainingHeapRunway
```

该比例会在本轮 GC 内随着实际扫描进度和堆增长重新计算。完成额外 Mark 工作可以获得
credit，之后的分配可以消费 credit。相比只根据上一轮对象数设定整轮比例，这种闭环
能更早、更平滑地纠偏。

### 8.5 Sweep 与分配按比例推进

Go 的 proportional sweep 计算 `sweepPagesPerByte`，把需要 Sweep 的页数摊到
后续分配上。分配越多，就推进相应 Sweep 工作，目标是在达到下一轮触发点前完成，
避免在 GC 开始时一次性等待大批 unswept pages。

### 8.6 内存限制同时保留 headroom 和 CPU 防抖

启用 memory limit 时，Go 不会简单把 heap goal 顶到硬限制；Runtime 为非堆内存和
波动保留 headroom。遇到极端内存压力时，GC CPU limiter 还会限制异常高的 GC CPU
消耗，避免应用进入“几乎所有 CPU 都用于 GC，但仍没有有效进展”的 thrashing。

对 MSGC 最可借鉴的不是某个常量，而是完整反馈链：

```text
稳定 live/goal
  → 预测 allocRate 与 scanRate
  → 提前确定 trigger runway
  → 固定后台 GC CPU
  → 轮内动态 Assist 修正误差
  → 分配侧按比例推进 Sweep
```

## 9. MSGC 稳定化方案

### P0：先补齐可归因的观测指标

在改变策略前，为每轮 GC 统一记录：

| 类别 | 必需指标 |
| --- | --- |
| Pause | PreEnum 精确 STW、EnumRoots handshake、MarkTermination 精确 STW |
| Mark 工作 | heap/stack/global scan bytes、scan edges、processed objects |
| Mark 进度 | expected/remaining work、scan rate、worker active time |
| 分配 | Mark 前后 in-use、Mark 期间 alloc bytes、alloc rate |
| Pacer | heap goal、trigger point、runway、预计完成点、预测误差 |
| Assist | ratio、work、wall time、触发次数、等待次数、credit |
| Worker | dedicated/fractional/idle 数量、GC CPU time、pressure level |
| Sweep/Scavenge | backlog、处理字节、耗时、与 Mark 重叠时间 |
| 业务 | 请求 p50/p95/p99/max、2 秒吞吐、进程 CPU/RSS |

当前 cycle wall time 只能回答“GC 从开始到结束用了多久”，不能直接回答业务暂停多久，
也不能区分 Mark 是 CPU 不足、扫描工作过大还是内存带宽受限。

### P1：分离 stable heap goal 和 trigger point

不再让单个 `heapThreshold` 同时代表“允许增长到哪里”和“何时开始 GC”：

```text
stableLive   = robustFilter(recentEffectiveLive)
heapGoal     = clamp(stableLive * (1 + heapGrowth), minGoal, maxGoal)
triggerPoint = heapGoal - requiredRunway
```

建议：

1. 将当前固定 20MB floor 改为可配置参数，并先做 20/128/256MB 单变量 A/B；
2. stable live 使用最近若干轮有效样本的中位数或限幅 EMA；
3. floating-live 样本不更新 stable live，但仍保留原始值用于日志和安全检查；
4. goal 下降也做限幅或迟滞，避免一轮低 live 立即触发高频 GC；
5. `liveSafetyFloor` 只保证安全，不直接覆盖长期稳定基线。

较大的 floor 可能降低 GC 频率，但会增加 RSS；它是实验变量，不是独立的最终修复。

### P2：增加 allocRate/scanRate runway Pacer

每轮维护最近 4–8 轮的稳健历史值，并在 GC 过程中滚动更新：

```text
expectedScanWork = predict(stable live, root scan, recent scan density)
allocRate        = allocatedBytes / mutatorTime
scanRate         = completedScanWork / gcCpuTime
requiredTime     = remainingScanWork / availableScanRate
requiredRunway   = allocRate * requiredTime + safetyMargin
triggerPoint     = heapGoal - requiredRunway
```

其中 `availableScanRate` 必须按计划给 GC 的 CPU 配额折算，而不是拿“所有 worker
满速运行”的理想值。预测误差较大时，用安全系数逐步扩大 runway，并对每轮改变量
限幅，防止 Pacer 自身振荡。

### P3：把 helper 数量升级为 GC CPU 利用率控制

当前依据 backlog 分级增加 helper，响应偏晚且会突然抢占 Mutator。建议建立类似
Go 的稳定后台 GC CPU 目标，初始可从 25% 做 A/B：

- 按总 Processor 数计算 dedicated worker；
- 余数由 fractional worker 分时兑现；
- 系统空闲时允许 idle worker 提前清空工作；
- backlog/no-progress 作为临时加压信号，而不是日常资源分配主轴；
- 记录实际 GC CPU utilization 与目标的误差。

目标不是让 `ConcurrentMark` wall time绝对最短，而是在不制造请求延迟尖峰的前提下，
让 Mark 以可预测速率在 heap goal 前完成。

### P4：按“剩余扫描工作 / 剩余 runway”动态调整 Assist

将整轮固定比例改为轮内闭环：

```text
assistWorkPerAllocByte =
    remainingScanWork / max(remainingHeapRunway, minimumRunway)
```

具体要求：

- 工作单位优先使用 scan bytes/edges，不只使用对象数；
- 随 Mark 进度定期重算，并对 ratio 做上下限和变化率限制；
- Mutator 多做的工作形成 credit，避免每次分配都被打断；
- 请求线程等待 Assist WorkBuf 的时间单独计量；
- 当 Assist 占比持续过高时，优先前移 trigger 或提高后台配额，而不是无限加重 Assist。

### P5：保持比例 Sweep，并协调 Scavenge 带宽

现有 Sweep 的 runway 思路应保留，并补充其预测误差和 backlog 观测。若发现下一轮
PreEnum 仍频繁等待 unswept backlog，再调整 `sweepBytesPerAllocByte` 或 worker
配额。

Scavenge 应增加带宽预算：当 ConcurrentMark 落后且内存压力未达到紧急水位时，
降低 Scavenge 并发；Mark 提前完成或 CPU/带宽空闲时再恢复。是否有效必须通过
Mark/Scavenge 重叠时间和内存带宽数据验证。

### P6：正确性与延迟保护

当前 Mark time budget 会把未完成的灰对象留给 STW `MarkTermination` 收口。
缩短 budget 看似限制了并发 Mark wall time，实质可能把大批剩余工作移动到 STW，
造成更严重的请求长尾。

因此 time budget 只适合作为诊断开关，不能作为生产消抖方案。任何 Pacer 改造都必须
维持 SATB、Finalizer resurrection、WeakRef 和普通闭包收敛的正确性，并保留
`CJ_MS_STW_SWEEP` 诊断 gate。

## 10. 验证方案

### 10.1 单变量实验矩阵

| 组别 | 变更 | 目的 |
| --- | --- | --- |
| A0 | 当前基线 | 建立相同机器、相同用例的重复性 |
| A1 | 仅新 floating-live 判定 | 验证 13 个历史命中类型是否不再污染后续 threshold |
| A2 | 仅 configurable floor/goal | 比较 20/128/256MB 的频率、RSS 和尾延迟 |
| A3 | 仅 goal/trigger 分离与 runway Pacer | 验证触发间隔、goal 和 GC work 是否收敛 |
| A4 | A3 + 固定 GC CPU 目标 | 验证后台资源是否更稳定 |
| A5 | A4 + 轮内动态 Assist | 验证预测误差能否平滑修正 |
| A6 | A5 + Sweep/Scavenge 协调 | 验证后台内存工作之间的竞争 |

每组至少重复 5 次，并随机化运行顺序；每次运行前记录 CPU 型号、系统负载、温度、
Runtime md5、SDK 来源和全部环境变量。

### 10.2 核心评价指标

每组同时比较：

- 业务：吞吐、request p50/p95/p99/max；
- GC wall：cycle p50/p95/p99/max；
- 暂停：PreEnum/EnumRoots/MarkTermination p50/p95/p99/max；
- 工作集：true live、scan bytes/edges、processed objects；
- 控制器：goal、trigger、runway、触发间隔及相邻变化率；
- 资源：GC CPU、Mutator CPU、Assist wall/share、RSS 平均和峰值；
- 正确性：崩溃、OOM、invalid state、漏扫、Finalizer/WeakRef 回归。

建议的初始通过门槛：

1. GC cycle P95/P99 和请求 p99 相对 A0 均有改善；
2. 吞吐回退不超过 3%；
3. 精确 STW p99 不劣化；
4. RSS 峰值处于事先选定的内存预算；
5. threshold/goal 相邻变化率和 trigger interval P95/P50 比值明显收敛；
6. 5 次重复运行零崩溃、零 GC invariant 错误。

这些门槛是本阶段的建议验收线，正式值应根据产品请求延迟和 RSS SLO 确认。

### 10.3 运行规范

每次替换 `libcangjie-runtime.dylib` 后必须：

1. 对 dylib 重新签名；
2. 使用目标 SDK 重新编译所有 `.cj` 测试文件；
3. 在隔离的 `env -i` 子 shell 中运行；
4. 用 `DYLD_PRINT_LIBRARIES=1` 验证实际加载的 Runtime 路径；
5. 明确记录 compiler、runtime、stdlib 三件套来源；
6. 检查 `gc.log`、stdout、stderr 和退出码后才判定成功。

case27 的固定 workload 之外，再增加三组敏感性验证：

- 并发数 50/100/200；
- payload 10K/100K/1M；
- 同步、加 jitter、等吞吐异步三种请求相位。

这样可以区分控制器本身是否稳定，以及改进是否只适配单一 benchmark。

## 11. 已证实事实与待验证假设

### 11.1 已由本次日志证实

- MSGC GC cycle 的主要波动来自 `ConcurrentMark`，累计占比 78.8%。
- GC total 与 `ConcurrentMark`、trueLive、processed objects 的相关系数都超过 0.98。
- `>=50ms` 周期的平均 processed objects 是 `<10ms` 周期的约 9.9 倍。
- cycle 2081 的 117.925ms 中，114.866ms 是 `ConcurrentMark`。
- `stopSweep` 与 GC total 的相关系数只有 0.060，不是本次首要矛盾。
- MSGC threshold 在 2805 轮中只有 5 轮不变，相邻相对变化平均约 43.5%。
- 2 秒请求吞吐变异系数约 1.83%，但 GC wall 仍有明显长尾。
- 新 floating-live 谓词离线回放可命中 13 个 `>=50ms` 周期，包含最慢 4 轮。

### 11.2 已由源码确认

- threshold 下降立即生效，普通上涨受 12.5% 限制，live safety floor 可越过限幅。
- 当前 20MB threshold floor 是源码固定值，metadata 中同名参数没有实际配置入口。
- GC 触发主要依据 cooldown 和 swept-retained bytes 是否达到 threshold。
- Mark pressure 依据 WorkBuf backlog/no-progress 事后升级。
- Assist 基础比例主要来自上一轮 processed objects 与本轮分配量。
- Mark time budget 的剩余灰对象会进入 STW `MarkTermination` 收口。
- Sweep 已经存在基于 threshold runway 的比例推进逻辑。

### 11.3 仍需实验验证

- 同步 client 的请求相位对齐是否显著放大瞬时对象图。
- Scavenge 与 ConcurrentMark 的内存带宽竞争是否贡献部分 P99。
- 新 floating-live 判定对后续 threshold、GC P95/P99 的实际改善幅度。
- 128MB 或 256MB floor 是否能在 RSS 预算内减少高频 GC。
- scan bytes/edges 是否比 processed objects 更稳定地预测 Mark 成本。
- 25% 是否是 MSGC 合适的后台 GC CPU 起点。
- Go 风格 runway Pacer 在不同 payload、并发和 heapGrowth 下能否保持收敛。

## 12. 最终建议

当前最合理的顺序是：

1. 用已经移除 burst 条件的 Runtime 重跑 case27，确认 floating-live 防护的真实效果；
2. 补齐 exact STW、scan work、alloc/scan rate、runway、GC CPU 和请求 histogram；
3. 分离 stable heap goal 与 trigger point，并让 20MB floor 可配置；
4. 实现基于 allocRate/scanRate 的提前触发；
5. 将 helper 调度改成稳定 GC CPU 利用率目标；
6. 将 Assist 改成轮内 `remainingWork / remainingRunway` 闭环；
7. 最后基于重叠数据协调 Sweep/Scavenge 带宽。

不建议把以下方式作为正式修复：

- 只增加 Mark helper 数量：可能缩短 GC wall，却会制造请求 CPU 抢占尖峰；
- 只提高 heap threshold：能减少频率，但可能把单轮工作集和 RSS 一起放大；
- 只依赖 floating-live 防护：它只保护后续基线，不缩短当轮 Mark；
- 关闭 Scavenge：当前没有证据证明它是主因，并可能推高 RSS；
- 设置很短的 Mark time budget：会把剩余工作转移到 STW；
- 追求每轮 GC wall 完全一致：并发 GC 的 wall time本来就受对象图和系统调度影响。

最终应以请求 p99 和精确 STW 为硬约束，以 GC P95/P99、吞吐与 RSS 为联合优化目标。
Go 的可借鉴点不是“GC 次数少”或“固定线程数”，而是用稳定 goal、预测 runway、
固定 CPU 预算和轮内 Assist 组成闭环，使 GC 在对象图波动时仍能可预测地完成。
