# Case26/27 HTTP Complex JSON：128 MiB 与 2 GiB 持久对象下 MSGC Mark 速率分析

## 1. 文档目的

本文比较同一个 MSGC runtime、同一个 `case27_http_complex_json` 用例在两种持久对象规模下的
Mark 行为：

- 128 MiB 持久对象：
  `20260727_095319_output_msgc_vs_copygc_go_java_c1000_p100K_compare/msgc`
- 2 GiB 持久对象：
  `20260727_095905_output_msgc_vs_copygc_go_java_c1000_p100K_compare/msgc`

目标不是只比较 GC 耗时，而是回答四个问题：

1. 最终 mark bitmap 对应的有效字节/对象速率下降了多少；
2. 下降是否只由 live set 变大造成，还是单位对象 Mark 效率也下降；
3. `debt-stalled` 和 `MarkTermination` 对 2 GiB 长尾贡献多大；
4. 现有日志能证明哪些原因，哪些原因必须通过 profile 或新增计数验证。

本文不修改 Runtime，也不把尚未采样的 cache、bitmap 竞争或 Scavenge 干扰写成确定根因。

## 2. 对比输入与可比性

### 2.1 结果目录

```text
128 MiB:
MSGC_RESULT/Result/case26_http_complex_json/result/
20260727_095319_output_msgc_vs_copygc_go_java_c1000_p100K_compare/msgc

2 GiB:
MSGC_RESULT/Result/case26_http_complex_json/result/
20260727_095905_output_msgc_vs_copygc_go_java_c1000_p100K_compare/msgc
```

### 2.2 共同条件

两组 `run_metadata.env` 记录的关键条件相同：

| 参数 | 128 MiB | 2 GiB |
| --- | ---: | ---: |
| GC 模式 | `marksweep` | `marksweep` |
| payload | `100K` | `100K` |
| concurrency | `1000` | `1000` |
| 主压测时长 | `60000 ms` | `60000 ms` |
| client mode | `sync` | `sync` |
| persistent workers | `16` | `16` |
| heap size | `16gb` | `16gb` |
| `cjHeapGrowth` | `1` | `1` |
| `cjScavengeGrowth` | `5` | `5` |
| runtime dylib MD5 | `d326934e4672aa5a77d11b4a90f8e71b` | 相同 |

唯一主要自变量是：

```text
134217728 B  = 128 MiB
2147483648 B = 2 GiB
```

因此这两组可以作为同一 runtime 下的持久对象规模 A/B。两组运行都正常输出最终
`[RESULT]`；但 2 GiB 有 2025 个请求失败，所以业务吞吐只作为 GC 压力背景，不作为
Mark 内核速率的直接分母。

### 2.3 持久对象不是“同样对象变大”，而是对象数接近 16 倍

benchmark 把 99% 的持久 payload 按以下 small `Array<Byte>` 尺寸等字节分配：

```text
128, 256, 512, 1024, 2048, 4096, 8192, 16384 B
```

剩余约 1% 使用：

```text
65536, 262144, 1048576 B
```

按 `persistentObjectCount()` 和 `appendPersistentTier()` 的整数除法重算：

| 持久 payload | small arrays | large arrays | 数组总数 |
| --- | ---: | ---: | ---: |
| 128 MiB | 258,570 | 17 | **258,587** |
| 2 GiB | 4,136,291 | 156 | **4,136,447** |

数组数放大：

```text
4,136,447 / 258,587 = 15.996x
```

这些数组分别由静态 `PersistentRootShard.objects` 和长期存活协程栈中的
`stackRoots` 持有。每个 `Array<Byte>` 是叶子对象：父容器扫描到它时仍需执行地址解析、
TypeInfo 校验和 mark bitmap 原子置位，但它自身没有引用字段，不会继续进入 trusted-grey
WorkBuf。

这个对象图结构决定了后文必须同时看：

- 最终 bitmap 的 `objects/liveBytes`；
- `ConcurrentMark + MarkTermination`；
- WorkBuf `processed` 的口径限制。

## 3. 统计范围与口径

### 3.1 主压测窗口

benchmark 在全部持久对象构建完成、Server 启动并完成 warmup 后才记录 `t0`，随后执行
`60000 ms` 主压测。`stdout.txt` 的 `[RESULT]` 没有 wall-clock 时间戳，因此本文使用：

```text
近似 t0 = gc.log 最后时间戳 - [RESULT] duration
主窗口 = [近似 t0, 近似 t0 + 60000 ms]
```

只纳入窗口内已经完整输出以下字段的 GC：

```text
ConcurrentMark time
[mark-workers]
[assist]
MarkTermination total
Mark bitmap
total gc time
```

窗口内得到：

- 128 MiB：464 个完整周期；
- 2 GiB：19 个完整周期。

该边界存在日志刷新级别的小误差。把窗口扩大到全部 post-baseline 周期后，端到端有效字节速率
分别为约 `15.54/2.88 GiB/s`，与主窗口的 `15.59/2.93 GiB/s` 结论一致。

### 3.2 主口径：端到端有效 Mark 速率

源码中的完整 Mark 闭包由并发阶段和 STW 收尾共同完成：

```text
DoGarbageCollection()
  -> ConcurrentMark()
  -> MarkTermination()
  -> PostMarkTermination()  // 统计稳定 bitmap
```

因此主口径是：

```text
端到端 Mark wall =
  Σ(ConcurrentMarkUs + MarkTerminationUs)

有效字节速率 =
  Σ(final mark bitmap liveBytes) / 端到端 Mark wall

有效对象速率 =
  Σ(final mark bitmap objects) / 端到端 Mark wall
```

它们衡量“本轮从 roots 到最终稳定 bitmap 的有效完成速率”，不是硬件顺序内存带宽：

- `liveBytes` 包含对象 payload、header 和本轮最终仍为 live 的对象；
- Mark 访问是对象、TypeInfo、RegionInfo、bitmap 的非连续访问；
- 同一个对象可能被多条边 probe，但最终 bitmap 只计一次；
- Mark 期间的新分配和写屏障会改变本轮实际工作集。

### 3.3 辅助口径：WorkBuf `processed`

日志中的：

```text
mainProcessed + helperProcessed + assistProcessed
```

只统计从 WorkBuf 取出并扫描的 trusted-grey 对象。producer 不变量是：

```text
valid + mark winner + HasRefField
```

字段扫描发现目标后，只有 `newlyMarked && targetTypeInfo->HasRefField()` 才把目标加入
WorkBuf。因此，数百万个持久 `Array<Byte>` 会付出 `DiscoverObject()` 和 bitmap mark 成本，
但不会进入 `processed`。

本文仍报告：

```text
processed rate =
  Σ(main + helper + assist) / Σ(ConcurrentMark wall)
```

但只把它解释为“引用对象 WorkBuf 消费速率”。它不能换算为：

- 全部 marked objects/s；
- visited edges/s；
- mark bitmap 原子提交/s；
- 单对象扫描内核的纯性能。

### 3.4 Natural 与 debt-stalled 分组

每轮另按 `ConcurrentMark time` 中的 reason 分组：

- `natural`：并发可见工作和 in-flight worker/assist 自然收敛；
- `debt-stalled`：连续三个 checkpoint 中 Global debt 没有下降，进入 one-way quiesce，
  把残余工作交给 STW `MarkTermination`。

两组必须分开看。否则 2 GiB 大量 `debt-stalled` 会把“并发扫描效率”和“STW 残余工作”
混成一个数字。

## 4. 核心结果

### 4.1 主窗口汇总

| 指标 | 128 MiB | 2 GiB | 变化 |
| --- | ---: | ---: | ---: |
| 完整 GC 周期 | 464 | 19 | - |
| bitmap liveBytes 中位数 | 0.461 GiB | 5.189 GiB | **11.26x** |
| bitmap objects 中位数 | 1.946 M | 28.220 M | **14.50x** |
| 加权 bytes/marked object | 245.4 B | 196.3 B | 对象密度提高 1.25x |
| 有效字节速率 | **15.587 GiB/s** | **2.931 GiB/s** | 2 GiB 慢 **5.32x** |
| 有效对象速率 | **68.201 Mobj/s** | **16.033 Mobj/s** | 慢 **4.25x** |
| WorkBuf processed 速率 | 34.942 Mobj/s | 4.114 Mobj/s | 表面慢 8.49x，受叶子对象口径影响 |
| Mark 中位 wall | 27.413 ms | 1685.668 ms | **61.49x** |
| Mark P95 wall | 56.437 ms | 3676.664 ms | **65.15x** |
| 平均 ConcurrentMark | 22.904 ms | 1026.375 ms | **44.81x** |
| 平均 MarkTermination | 7.187 ms | 730.743 ms | **101.68x** |
| MarkTermination 时间占比 | 23.88% | 41.59% | +17.71 pct |
| `debt-stalled` | 77 / 464（16.59%） | 14 / 19（73.68%） | +57.09 pct |
| Assist 占 processed | 23.78% | 53.67% | +29.89 pct |

结论：

1. 2 GiB 最终 bitmap 的对象数不是 11 倍，而是约 14.5 倍；对象密度也更高。
2. Mark wall 增长约 61.5 倍，远大于工作集增长，说明不能只用“对象更多”解释。
3. 端到端单位对象效率下降约 4.25 倍，单位字节效率下降约 5.32 倍。
4. 2 GiB 不仅 ConcurrentMark 更长，还频繁把大量工作带入 STW termination。

### 4.2 只看 natural 周期

| 指标 | 128 MiB natural | 2 GiB natural | 变化 |
| --- | ---: | ---: | ---: |
| 周期数 | 387 | 5 | - |
| 有效字节速率 | 16.916 GiB/s | 4.859 GiB/s | 慢 **3.48x** |
| 有效对象速率 | 76.062 Mobj/s | 23.464 Mobj/s | 慢 **3.24x** |
| WorkBuf processed 速率 | 35.556 Mobj/s | 9.817 Mobj/s | 表面慢 3.62x |
| 平均 Mark wall | 28.205 ms | 950.381 ms | 33.70x |
| 平均 MarkTermination | 2.311 ms | 3.673 ms | 接近毫秒级 |
| termination 时间占比 | 8.20% | 0.39% | 2 GiB natural 尾部很小 |

Natural 对照证明两点：

- 即使不发生 `debt-stalled`，大工作集下的端到端 Mark 有效速率仍下降约 3.2～3.5 倍；
- 2 GiB 的长 STW 不是“大 live set 必然导致”，因为能够自然收敛的 5 个周期平均
  MarkTermination 只有 3.7 ms。

2 GiB natural 只有 5 个样本，因此这个 3.2～3.5 倍用于确定方向，不应当作高精度常数。

### 4.3 只看 debt-stalled 周期

| 指标 | 128 MiB debt-stalled | 2 GiB debt-stalled |
| --- | ---: | ---: |
| 周期数 | 77 | 14 |
| 有效字节速率 | 10.828 GiB/s | 2.611 GiB/s |
| 有效对象速率 | 40.040 Mobj/s | 14.800 Mobj/s |
| 平均 ConcurrentMark | 7.879 ms | 1054.828 ms |
| 平均 MarkTermination | 31.691 ms | **990.411 ms** |
| termination 时间占比 | 80.09% | 48.43% |
| termination `flushed` 中位数 | 3,080 | **103,998** |

`flushed` 是 fixed-point drain 从 active logs、各 Per-P GC work、Global lane 和 deferred
queue 收集到 main queue 的 source entries，不是 termination 实际访问的全部对象数。一个 source
entry 可以继续展开子图。但它仍是有用的残余工作代理量：

- 128 MiB 全部主窗口中，`MarkTermination time` 与 `flushed` 的 Pearson 相关系数约 `0.922`；
- 2 GiB 中约 `0.843`；
- 2 GiB debt-stalled 的 `flushed` 中位数是 128 MiB 的约 `33.8x`；
- 对应平均 termination 从 31.7 ms 增加到 990.4 ms。

这构成“残余工作进入 STW 是 2 GiB 长尾主因”的直接日志证据。

## 5. 代表性 GC 周期

### 5.1 128 MiB natural：cycle 467

```text
ConcurrentMark = 31.436 ms, reason=natural
MarkTermination = 5.717 ms, flushed=1,345
bitmap objects = 2,669,368
bitmap liveBytes = 582,130,280 B = 0.542 GiB
端到端有效字节速率 = 14.59 GiB/s
```

日志：

- `.../095319.../msgc/gc.log:48306`
- `.../095319.../msgc/gc.log:48314`
- `.../095319.../msgc/gc.log:48317`

### 5.2 128 MiB debt-stalled：cycle 466

```text
ConcurrentMark = 1.795 ms, reason=debt-stalled
MarkTermination = 37.466 ms, flushed=2,729
bitmap objects = 1,322,895
bitmap liveBytes = 447,528,912 B = 0.417 GiB
端到端有效字节速率 = 10.62 GiB/s
```

这一周期并发阶段很早退出，约 95% 的 Mark wall 落在 STW termination。

### 5.3 2 GiB natural：cycle 37

```text
ConcurrentMark = 885.123 ms, reason=natural
MarkTermination = 1.185 ms, flushed=125
bitmap objects = 28,062,843
bitmap liveBytes = 5,571,607,288 B = 5.189 GiB
端到端有效字节速率 = 5.85 GiB/s
```

日志：

- `.../095905.../msgc/gc.log:4674`
- `.../095905.../msgc/gc.log:4682`
- `.../095905.../msgc/gc.log:4685`

这个周期说明：即使 live set 超过 5 GiB，只要并发阶段自然收敛，STW 收尾仍可保持约 1 ms。

### 5.4 2 GiB debt-stalled：cycle 43

```text
ConcurrentMark = 1.787398 s, reason=debt-stalled
MarkTermination = 2.071388 s, flushed=219,740
bitmap objects = 47,784,090
bitmap liveBytes = 7,428,182,576 B = 6.918 GiB
端到端有效字节速率 = 1.79 GiB/s
```

日志：

- `.../095905.../msgc/gc.log:5858`
- `.../095905.../msgc/gc.log:5907`
- `.../095905.../msgc/gc.log:5910`

这个周期的 termination 已经比 ConcurrentMark 更长，说明 one-way quiesce 时仍有大规模闭包工作。

### 5.5 `flushed` 不是 termination 总工作量：2 GiB cycle 38

cycle 38 只有 `3,183` 个 flushed source entries，但 MarkTermination 达到 `507.656 ms`，
最终 bitmap 有 896 万对象。原因是 source entry 会在 `ExecuteLikeDrainMainQueue()` 中继续展开
引用闭包。

因此后续必须增加 `MarkTerminationProcessed/VisitedEdges/MarkWinners`，不能只凭 `flushed`
估算 termination 对象速率。

## 6. 原因分析

### 6.1 第一层：持久对象数放大约 16 倍，最终 marked objects 放大约 14.5 倍

这是最直接、无需推断的工作量变化：

```text
持久 Array<Byte> 数：
258,587 -> 4,136,447 = 15.996x

主窗口 bitmap objects 中位数：
1.946 M -> 28.220 M = 14.50x

主窗口 bitmap liveBytes 中位数：
0.461 GiB -> 5.189 GiB = 11.26x
```

2 GiB 场景不是只把已有对象 payload 扩大，而是让 GC 每轮面对数千万个 marked objects。
此外，加权 bytes/marked object 从 245.4 B 降到 196.3 B；每 GiB 中需要完成的对象解析、
TypeInfo 检查和 bitmap 标记次数约增加 25%。

因此，Mark wall 增长中的第一部分是正常的工作量增长。但工作量只增长约 11～14.5 倍，
wall 却增长约 61.5 倍，仍有约 4～5 倍的单位工作效率下降需要解释。

### 6.2 第二层：大量叶子数组让 `processed` 低估真实 Mark 工作

字段扫描路径是：

```text
TraceObjectRefFieldsToGCWork(parent)
  -> 对每个 RefField 调用 DiscoverObject(target)
  -> ResolveMarkTarget
  -> probe mark bit
  -> 读取 TypeInfo
  -> atomic TryMark
  -> 仅当 target HasRefField 时 PutObj(target)
```

`Array<Byte>` 没有引用字段。它会被发现、校验并赢得 mark bit，但不会进入 WorkBuf。
`DrainCurrentBuf()` 的 `nProcessed` 又只按输入 WorkBuf 的 `len` 计数。

所以 2 GiB 下 `processed rate` 从 34.94 降到 4.11 Mobj/s，包含两种效应：

1. 引用对象 WorkBuf 消费本身变慢；
2. 每个被消费的 ArrayList/RefArray 父对象会发现更多未计数的叶子数组。

这解释了为什么 `processed rate` 表面下降 8.49 倍，明显大于最终 bitmap object rate
下降的 4.25 倍。8.49 倍不能直接写成“单对象 Mark 内核慢了 8.49 倍”。

当前日志缺少：

- `visitedEdges`
- `alreadyMarked`
- `markWinner`
- `leafWinner`
- `casLoser`
- `atomicWordCommits`

因此现有数据无法把父对象字段扫描、叶子发现和重复边 probe 分拆。

### 6.3 第三层：即使 natural 收敛，单位工作效率仍显著下降

排除 `debt-stalled` 后：

```text
有效字节速率：16.916 -> 4.859 GiB/s，下降 3.48x
有效对象速率：76.062 -> 23.464 Mobj/s，下降 3.24x
```

这证明速率下降不只是 termination 策略造成。当前源码中每个新发现 small object 都要经过：

```text
对象地址
  -> heap-relative RegionInfo 定位
  -> Region/slot/alignment/BUMP 边界校验
  -> mark bitmap word/mask
  -> marked probe
  -> white slot TypeInfo acquire
  -> mark bitmap atomic fetch_or
```

大工作集会扩大对象、TypeInfo、RegionInfo 和 bitmap 的访问范围。2 GiB 主窗口中
`liveBytes` 与端到端 Mark wall 的 Pearson 相关系数约 `0.905`；128 MiB 中约 `0.668`。
相关性不能单独证明 cache/DRAM 根因，但方向与地址工作集放大一致。

已有同 runtime 系列的 Step52 采样还表明：

- 稳态 Helper Global mutex wait 已降到约 1%～2%；
- GC Main 的 Global pop wait 约 1.29%；
- 主要活跃栈仍在
  `DrainCurrentBuf -> TraceObjectRefFieldsToGCWork -> DiscoverObject`。

因此，以下解释具有源码和历史 profile 支持，属于高概率原因：

1. 对象、TypeInfo、RegionInfo、bitmap 的 LLC/TLB/DRAM 局部性下降；
2. 每个白对象独立执行 bitmap atomic RMW；
3. 相邻 slot/bitmap word 的 cache-line ownership 迁移和 false sharing；
4. 通用 WorkBuf 缺少 Region affinity，worker 在更大地址空间中跳转。

但本次两个结果目录没有保存 Mark 窗口的 `sample`、Instruments 或硬件 cache/带宽计数。
所以不能写成“已经证明内存带宽打满”或“bitmap 原子是唯一根因”。

### 6.4 第四层：debt-stalled 比例从 16.6% 升到 73.7%

当前 `MSMarkPressurePolicy`：

```text
kCheckpointBuffers = 10
kBacklogBuffersPerLevel = 10
kNoProgressWindowsBeforeSTW = 3
```

GC Main 每完成 10 个 WorkBuf 执行一次 checkpoint。只要当前 Global debt
没有严格小于上一个 checkpoint：

```text
noProgressWindows++
pressureLevel++
```

连续三个窗口没有下降就返回 `ShouldTerminate()`，原因记录为 `debt-stalled`。

这里判断的是“已发布 Global buffer 数是否净下降”，不是：

- 剩余总 visited edges；
- 所有 private partial builder 的对象数；
- 所有 mutator active log 的对象数；
- 按实时扫描/分配速率计算的完成 deadline。

2 GiB 对象图展开时间更长，HTTP mutator 又持续分配和修改对象。Global debt 在三个离散
checkpoint 中不下降的概率显著提高，最终 14/19 个主窗口周期走 `debt-stalled`。

日志直接证明“2 GiB 更常触发该策略”；但要判断三窗口策略是否过于激进，还需要记录
每个 checkpoint 之间的：

- 扫描对象数和 visited edges；
- 新发布 Full/Partial 数；
- mutator active-log 增量；
- helper/assist 活跃时间；
- 总 work produced 与 consumed。

### 6.5 第五层：one-way quiesce 把残余闭包转移到单线程 STW

`QuiesceMarkWorkers()` 的当前设计是：

1. 禁止新的 helper/assist claim；
2. 等待 in-flight 归零；
3. 不主动发布 GC Main/Helper 的 private partial tail；
4. 停止 Mark worker；
5. 进入 `MarkTermination`。

随后 STW `DrainMarkTerminationToFixedPoint()` 反复执行：

```text
ExecuteLikeDrainMainQueue
FlushAllWriteBarrierBuffers
DisposeAllPerPGCWork
DrainGlobalLFStackToMainQueue
DrainDeferredMainThreadQueueToMainQueue
直到 mainQueue 为空
```

这保证正确性，但残余闭包由 GC Main 在 STW 下完成。2 GiB debt-stalled 周期的
`flushed` 中位数达到 103,998，平均 termination 达到 990.4 ms；natural 周期平均
termination 只有 3.7 ms。

因此可以把原因表述为：

> 2 GiB 长尾的直接机制不是“MarkTermination 固定成本随堆线性增长”，而是并发阶段频繁因
> debt-stalled 提前 one-way quiesce，随后把大规模 active-log/private/global/deferred
> 残余闭包交给单线程 STW fixed-point drain。

### 6.6 第六层：长 ConcurrentMark 与 mutator 工作形成放大回路

2 GiB 平均 ConcurrentMark 为 1.026 s，128 MiB 只有 22.9 ms。更长的并发窗口意味着 mutator
有更多 wall time：

- 分配新的请求临时对象；
- 修改引用并产生 mark-at-write/active-log 工作；
- 触发 Mutator Mark Assist；
- 让本轮 bitmap 捕获更多 floating live。

日志中的 Assist 占 `processed` 比例从 23.78% 增加到 53.67%，说明 2 GiB 场景已经把更多
Mark 工作转移给业务线程。最终 bitmap liveBytes 中位数为 5.189 GiB，明显高于 2 GiB
payload；其中还包含对象 header、容器容量、协程/HTTP 对象和 Mark 期间存活的临时图。

合理的放大链是：

```text
持久对象更多
  -> ConcurrentMark 更长
  -> Mark 期间 mutator 产生更多工作和 floating live
  -> Global debt 更难在三个 checkpoint 内下降
  -> debt-stalled + 大 termination backlog
  -> GC wall 和业务请求收尾进一步变长
```

这条链的前半段和后半段都有日志证据，但“每秒新产生多少 mark work”当前没有直接计数，
所以应称为机制一致的高概率放大因素，而不是已经完整量化的闭环根因。

### 6.7 不应误判为主要原因的项目

1. **不是 roots 数量直接增长 16 倍。** `EnumRoots` 日志的 roots 中位数约为
   432/436；数百万数组藏在 ArrayList/协程栈持有的对象图中，由 tracing 展开。
2. **EnumRoots 不是本文 Mark 速率分母。** 它的平均时间约从 10.1 ms 增至 38.9 ms，
   会影响总 GC，但不能解释本文 `ConcurrentMark + MarkTermination` 的全部下降。
3. **不能直接归因于 Global WorkBuf mutex。** 当前两个目录没有锁 profile；历史同 runtime
   采样显示稳态锁等待已不是第一热点。
4. **不能直接归因于 Scavenge/Sweep 重叠。** 源码允许后台任务争用内存系统，历史采样也看到
   Scavenge 清零/mmap，但本轮没有按 Mark 区间对齐的重叠字节和 CPU 样本。
5. **不能用更低业务吞吐证明 GC 更轻。** 2 GiB 仍有 1000 并发，且单轮 Mark 持续秒级；
   每个 Mark 窗口覆盖的 mutator 工作总量可能仍然很大。

## 7. 结论边界

### 7.1 已由本次结果直接证明

1. 两组使用同一 runtime dylib MD5 和相同主要参数，持久 payload 是主要自变量。
2. benchmark 设计使持久数组数从 258,587 增加到 4,136,447，约 16 倍。
3. 主窗口最终 marked objects 中位数增加约 14.5 倍，marked liveBytes 增加约 11.3 倍。
4. 端到端有效对象/字节速率分别下降约 4.25/5.32 倍。
5. 排除 `debt-stalled` 后，2 GiB natural 有效速率仍下降约 3.2～3.5 倍。
6. `debt-stalled` 占比从 16.6% 增至 73.7%。
7. 2 GiB debt-stalled 的 termination residual source entries 和 termination wall
   都大幅增加。
8. 2 GiB natural 周期的 termination 仍为毫秒级，秒级 STW 主要出现在 residual backlog
   路径。

### 7.2 已由当前源码确认

1. `processed` 只统计 WorkBuf 中含引用字段的 trusted-grey 对象。
2. 叶子对象仍执行 `DiscoverObject`、TypeInfo 校验和 bitmap atomic mark，但不进入 WorkBuf。
3. pressure policy 每 10 个 GC-main buffer 检查一次 Global debt。
4. debt 连续三个 checkpoint 不下降就以 `debt-stalled` 请求 one-way quiesce。
5. quiesce 不发布所有 private partial tail；STW termination 统一 flush 并固定点清空。
6. `Mark bitmap: objects/liveBytes` 是 Mark 完成后、Sweep 发布前的稳定 bitmap 统计。

### 7.3 高概率但尚未由本次运行直接证明

1. 大地址工作集降低对象/TypeInfo/RegionInfo/bitmap 的 cache/TLB 局部性。
2. bitmap 原子 RMW 和相邻 word cache-line ownership 限制多线程扩展。
3. 通用 WorkBuf 缺少 Region affinity，放大跨 Region 跳转。
4. Mark 与 Scavenge/Sweep 在大堆下争用 LLC/DRAM。
5. 更长 Mark 窗口通过 mutator allocation/write-barrier work 放大 floating live 和 debt。

### 7.4 当前不能得出的结论

1. 不能把 WorkBuf `processed rate` 当成全部对象或 edge 扫描速率。
2. 不能断言 bitmap 竞争是唯一或第一根因。
3. 不能断言硬件内存带宽已经打满。
4. 不能仅凭 `flushed` 计算 MarkTermination objects/s。
5. 不能仅凭这一次运行决定应删除 `debt-stalled` 或简单增大 checkpoint 阈值。
6. 不能仅靠增加 GC Helper 解决问题；缺少实际 Helper active time 和同工作集线程扩展 A/B。

## 8. 后续验证实验

### 8.1 实验 A：补齐规模曲线

目的：确认速率下降是平滑工作集效应，还是跨过某个规模后突变。

固定：

- 同一 compiler/runtime/stdlib/stdx；
- runtime MD5；
- `100K/c1000/60000ms/16gb`；
- `persistentWorkers=16`；
- `cjHeapGrowth=1`、`cjScavengeGrowth=5`。

只改变：

```text
CASE27_PERSISTENT_BYTES =
128MiB / 256MiB / 512MiB / 1GiB / 2GiB
```

每档至少 3 轮，分别报告：

- natural/debt-stalled 周期数；
- bitmap objects/liveBytes；
- `ConcurrentMark/MarkTermination`；
- effective GiB/s 和 bitmap Mobj/s；
- termination flushed；
- Assist share；
- 成功请求吞吐和失败率。

### 8.2 实验 B：固定对象图 Mark 微基准

目的：把对象图/bitmap 扫描效率与 HTTP 持续分配分开。

建议流程：

1. 构建与当前 benchmark 相同尺寸分布的固定可达对象图；
2. 等对象图完成后停止扩大图和请求分配；
3. 对同一对象图重复触发 GC；
4. 依次测试 128 MiB、2 GiB；
5. 每档测试 `cjGCThreads=1/2/4`；
6. 第一轮固定 `CJ_MS_DISABLE_ASSIST=1`；
7. 至少重复 5～10 次，报告中位数和 P90。

如果固定图下 2 GiB 仍有 3 倍以上单位对象速率下降，优先检查 locality/bitmap RMW。
如果固定图接近稳定、只有 HTTP 场景下降，优先检查 mutator work production、Assist 和后台任务重叠。

### 8.3 实验 C：Assist 单变量 A/B

同一持久规模分别运行：

```text
CJ_MS_DISABLE_ASSIST=1
CJ_MS_DISABLE_ASSIST 未设置
```

同时固定 `cjGCThreads=4`。观察：

- Main+Helper processed；
- total processed；
- natural/debt-stalled；
- Mark wall；
- HTTP 吞吐/失败；
- RSS 和 CPU。

目标不是证明 Assist 越多越好，而是判断它是在帮助并发收敛，还是与 GC worker 争用并把
业务 CPU 转成 GC CPU。

### 8.4 实验 D：精确记录 Mark 工作产生和消费

建议增加 worker-local 计数，周期结束汇总，避免 Mark 热路径全局原子：

```text
visitedEdges
alreadyMarked
markWinner
leafWinner
refWinner
casLoser
atomicWordCommits
regionSwitches
```

pressure checkpoint 增加：

```text
globalFullBuffers/globalPartialBuffers
producedObjects/consumedObjects
activeLogObjects
privatePartialObjects
activeHelpers
assistsInFlight
```

这样可以判断 debt“不下降”究竟来自：

- 对象图真实产生速度高于消费速度；
- active/private work 延迟发布；
- Helper 没有按目标活跃；
- Assist 与后台 worker 之间的工作迁移。

### 8.5 实验 E：补齐 MarkTermination 同口径计数

至少增加：

```text
terminationSourceEntriesByKind:
  activeLog / perPGC / globalFull / globalPartial / deferred

terminationProcessedObjects
terminationVisitedEdges
terminationMarkWinners
terminationRounds
```

cycle 38 已证明 `flushed` 不能代表实际闭包工作。只有补齐这些计数后，才能定量回答：

- 哪一类 residual source 最大；
- 单线程 termination 的实际对象/edge 速率；
- debt-stalled 是否把可并发处理的工作过早转入 STW。

### 8.6 实验 F：对齐 Mark 窗口做 profile

在 128 MiB 和 2 GiB 各选择：

- 一个相近类型的 natural 周期；
- 一个 debt-stalled 长周期。

用 macOS `sample` 或 Instruments 按 `ConcurrentMark` 时间戳对齐，分别统计：

- `DiscoverObject/ResolveMarkTarget`；
- TypeInfo/field scanner；
- bitmap mark；
- Global/Pool mutex wait；
- Helper active/wait；
- Scavenge `MemorySet/mmap` 重叠；
- GC 与 mutator CPU。

如果条件允许，追加硬件计数或系统级带宽观测；没有这些数据前不宣称“DRAM 已饱和”。

### 8.7 实验 G：诊断 debt-stalled 策略，而不是直接修改默认值

先完成 8.4/8.5 的可观测性，再做诊断分支：

1. 保持默认三个 no-progress checkpoint；
2. 临时延长窗口或仅记录“如果不终止还需多久自然收敛”；
3. 比较 ConcurrentMark 增量、STW 减少量、业务吞吐、RSS 和失败率。

验收目标是：

```text
总 Mark wall 不恶化
MarkTermination 显著下降
RSS/分配失败不恶化
业务吞吐不下降
GC 正确性验证通过
```

在这些数据之前，不建议删除 `debt-stalled`，也不建议简单把三次改成更大的常量作为正式修复。

## 9. 关联源码与结果文件

### 9.1 原始结果

128 MiB：

```text
MSGC_RESULT/Result/case26_http_complex_json/result/
20260727_095319_output_msgc_vs_copygc_go_java_c1000_p100K_compare/msgc/
  run_metadata.env
  stdout.txt
  gc.log
  rss.csv
  benchmark.cj.snapshot
```

2 GiB：

```text
MSGC_RESULT/Result/case26_http_complex_json/result/
20260727_095905_output_msgc_vs_copygc_go_java_c1000_p100K_compare/msgc/
  run_metadata.env
  stdout.txt
  gc.log
  rss.csv
  benchmark.cj.snapshot
```

### 9.2 Benchmark 对象图

```text
benchmark.cj.snapshot:71   PERSISTENT_SMALL_OBJECT_SIZES
benchmark.cj.snapshot:73   PERSISTENT_LARGE_OBJECT_SIZES
benchmark.cj.snapshot:92   persistentObjectCount
benchmark.cj.snapshot:118  appendPersistentTier
benchmark.cj.snapshot:139  allocatePersistentRoots
benchmark.cj.snapshot:161  persistentAllocationWorker
benchmark.cj.snapshot:193  每秒 touch 持久对象
benchmark.cj.snapshot:684  先构建 persistent baseline
benchmark.cj.snapshot:792  warmup 后记录主压测 t0
```

### 9.3 Runtime 关键源码

```text
src/Heap/MCollector/MarkSweepCollector.cpp
  RunConcurrentMarkLoop
  DiscoverObject
  TraceObjectRefFieldsToGCWork
  DrainCurrentBuf
  QuiesceMarkWorkers
  MarkTermination
  DrainMarkTerminationToFixedPoint
  ExecuteLikeDrainMainQueue
  PostMarkTermination / ComputeMarkedStats

src/Heap/MCollector/MSMarkTerminationPolicy.h
  MSMarkPressurePolicy
  kCheckpointBuffers
  kBacklogBuffersPerLevel
  kNoProgressWindowsBeforeSTW

src/Heap/MAllocator/MSRegionInfo.cpp
  TestAndSetMarkBit
  MarkBitmapWordResolved

src/Heap/MAllocator/MSCentralRegionPool.cpp
  ComputeMarkedStats
```

### 9.4 相关 Implementation 文档

```text
Step50_MSGC_Mark线程扩展效率与Helper_Assist对照分析.md
Step52_MSGC并发Mark效率瓶颈采样分析与优化优先级.md
Step56_MSGC_P5_P50_P100_P150_Mark_Sweep_Scavenge速率对比分析.md
Step57_MSGC_case27_GC时延波动根因与Go_Pacer稳定化方案.md
Step58_MSGC高P并发Mark数据局部性_Bitmap竞争与任务分区优化方案.md
```

### 9.5 最终结论

2 GiB 场景的 Mark 退化不是单一因素：

```text
约 16x 持久叶子对象
  + 约 14.5x 最终 marked objects
  + natural 单位效率下降约 3.2～3.5x
  + debt-stalled 从 16.6% 增至 73.7%
  + 大 residual closure 转入单线程 STW
  = 端到端 Mark 字节速率下降约 5.32x，
    Mark 中位 wall 增加约 61.5x
```

当前最明确的工程优先级是先补齐 edge/winner/leaf/termination residual/worker active
可观测性，再分别验证数据局部性、bitmap RMW、Assist 和 debt-stalled policy。现有数据足以证明
问题发生在哪些阶段，但还不足以直接选择某一个结构性优化作为唯一修复。

## 10. 大堆 Mark 速率下降的进一步拆解与优化优先级

### 10.1 退化的乘法分解

把一次 Mark 的时间近似写成：

```text
Mark wall ≈ marked objects / effective object rate
```

代入主窗口统计：

```text
marked objects 中位数倍率 = 28.220M / 1.946M = 14.50x
effective object rate 降幅 = 68.201 / 16.033 = 4.25x
14.50 × 4.25 = 61.6x

实测 Mark wall 中位数倍率 = 1685.668 / 27.413 = 61.49x
```

两者几乎闭合。因而不能把 61.5 倍 wall 增长全部描述成“Mark 算法慢了 61.5 倍”：

1. 约 `14.5x` 来自本轮实际需要标记的对象数量增加；
2. 约 `4.25x` 来自单位对象端到端效率下降；
3. 两者相乘形成约 `61.5x` 的 wall 放大。

再用 natural 周期剥离 termination 长尾，单位对象速率只下降 `3.24x`。主窗口的
`4.25x / 3.24x ≈ 1.31x` 差异主要来自 2 GiB 中 `debt-stalled` 比例和单线程
termination residual 增加。由于 natural 与全窗口不是完全相同的周期总体，这个 `1.31x`
只能作为分层量级，不能当作严格的独立乘数。

因此优化目标也必须分成两类：

- 降低每个 live object/edge 的扫描和标记成本；
- 避免把仍可并发完成的闭包过早转移到单线程 STW。

### 10.2 Benchmark 持久 touch 与 Mark 争用内存层级

`persistentAllocationWorker()` 在构建完持久对象后并没有休眠不动，而是持续执行：

```text
遍历 staticRoots 和 stackRoots 中的每一个 Array<Byte>
  -> 读取 object.size
  -> 读取 object[0]
  -> 读取 object[object.size - 1]
  -> 完成一轮后 sleep 1s
```

这意味着 2 GiB 对比项同时引入了一个明显更大的 mutator 内存扫描负载：

| 每个持久 Root List | 128 MiB 总场景 | 2 GiB 总场景 |
|---|---:|---:|
| 持有的 Array 数量 | 约 8.1K | 约 129K |
| backing RefArray payload | 约 65 KiB | 约 1.0 MiB |
| 全部持久 Array 数量 | 约 0.26M | 约 4.14M |

2 GiB 每轮 touch 至少要顺序读取约 413 万个引用和对象 header，并读取每个 Byte Array
首尾两个位置。若粗略按“引用/backing、对象 header、payload 端点”触达三个 64B cache line
估算，一次冷遍历就可能带来约：

```text
4.14M × 3 × 64B ≈ 0.74 GiB cache-line traffic
```

这不是 DRAM 实测值，因为 cache 命中、页驻留和硬件预取会改变实际流量；但它说明 2 GiB
运行在 ConcurrentMark 之外还额外制造了足以扰动 LLC/TLB/DRAM 的工作。

更关键的是时间重叠：

- 128 MiB natural Mark 通常只有几十毫秒，只覆盖持久 touch 的很小时间片；
- 2 GiB natural Mark 接近或超过一秒，基本必然与一轮或多轮持久 touch 重叠；
- 16 个持久协程再叠加 HTTP mutator，会与 4 条 Mark 线程共同争用 CPU 和内存层级。

持久 touch 只读 Byte Array，不产生引用写屏障工作，因此它主要解释 natural 周期的单位效率下降，
而不是 `debt-stalled` 的新增 grey work。当前没有 Mark 窗口硬件带宽计数，所以应先做
`persistent touch on/off` 单变量 A/B；若关闭 touch 后 2 GiB natural Mark 明显恢复，
即可把这部分从“高概率”提升为“已量化”。

### 10.3 少量超大 RefArray 形成粗粒度扫描任务

16 个 persistent worker 各自保留 static/stack 两组 `ArrayList<Array<Byte>>`，因此持久图的
数百万叶子数组主要由约 32 个 Root List 及其 backing RefArray 引出。2 GiB 下每个 list
约有 12.9 万个引用，backing RefArray payload 约 1 MiB；128 MiB 下每个 list 只有约
8.1K 个引用和 65 KiB payload。

当前扫描实现：

```text
DrainCurrentBuf
  -> 取出一个 grey object
  -> ForEachRefFieldGCKnownType
  -> 若为引用数组，从 index=0 一直扫描到 arrayLength
```

当前调度实现：

- `WorkBuf` 只保存 `BaseObject*`，容量为 1024；
- worker 领取一个 WorkBuf 后由 `DrainCurrentBuf()` 一次性处理完整 buffer；
- 数组内部没有 range task、抢占点或跨 worker 切分；
- 一个含 12.9 万引用的 RefArray 在统计中仍只算一个 `processed object`。

因此，大 RefArray 会同时造成：

1. **任务粒度失真。** 一个普通两字段对象和一个 12.9 万引用数组都计作一个对象；
2. **负载均衡受限。** 某个 worker 一旦领取大数组，就要独自扫描完整数组；
3. **Assist 成本不可控。** mutator 领取的一个 WorkBuf 可能含大数组，实际耗时远高于按
   `processed object` 计算的偿债量；
4. **pressure 反馈滞后。** GC main 每完成 10 个 WorkBuf 才 checkpoint，但一个 buffer
   的真实 edge 数可能相差几个数量级。

当前日志只能证明 root 数约 432/436、Mark 配置为 4 线程，不能还原 32 个大 backing array
分别由哪条线程、哪个 WorkBuf 扫描。因此“粗粒度数组任务”是源码和对象图直接支持的机制，
但其时间占比仍需增加以下计数后确认：

```text
bufferVisitedEdges
bufferScanUs
maxArrayLength
largeRefArrayCount
perWorkerVisitedEdges / activeUs
```

对应优化不是简单减小 `WorkBuf::kCapacity`，因为再小的对象 buffer 也无法拆开单个大数组。
更有效的做法是为大 RefArray 引入 `RefArrayRangeTask(parent, begin, end)`：

- 例如每 2K～8K 个引用切成一个 range；
- range task 可由不同 Mark worker 领取；
- parent 已标记，range 只负责扫描字段；
- 普通小对象继续走现有 BaseObject WorkBuf；
- 必须验证并发数组写和 mark-at-write barrier 能覆盖切片扫描前后的竞态。

这项优化既能改善并行度，也能让 edge-based pressure 指标获得自然的调度单位。

### 10.4 bitmap 与对象元数据热路径

每个候选引用当前都要执行：

```text
load RefField
  -> heap-range gate
  -> heap offset 解析 RegionInfo 和 regionOffset
  -> region type / size class / slot 边界与对齐检查
  -> relaxed probe mark bitmap
  -> white slot acquire-load TypeInfo
  -> atomic fetch_or 获取 mark ownership
  -> winner 根据 TypeInfo 判断是否需要入 WorkBuf
```

2 GiB 中约 413 万个持久 `Array<Byte>` 都是叶子对象。它们不会进入 WorkBuf，但每个对象仍要
执行 RegionInfo 定位、TypeInfo 校验和 bitmap atomic mark。与此同时，HTTP 临时图又把最终
marked objects 推到中位约 2822 万。

大堆下单位成本上升的主要硬件机制是：

1. 对象 header 分布在数 GiB 地址空间，white slot 的 TypeInfo load 更容易落到 LLC/DRAM；
2. RegionInfo、对象页和 bitmap 是三条不同地址流，扩大 TLB 和 cache working set；
3. `fetch_or` 即使没有逻辑冲突，也要获取 bitmap cache line 的独占状态；
4. GC main、3 个 helper 和多个 mutator assist 可能同时更新相邻 bitmap word，产生
   cache-line ownership 迁移或 false sharing；
5. 引用按容器顺序遍历，但通用 `DiscoverObject()` 没有利用“连续引用经常指向同一
   Region/bitmap word”的局部性。

历史同 runtime 的稳态采样中，Global Pop mutex wait 已降到约 1%～2%，4 条 Mark 线程相对
单线程只有约 `1.779x` 加速、并行效率约 `44.5%`。这更符合共享内存层级和每-edge 标记成本
限制，而不支持继续把 Global WorkBuf mutex 当成第一优化目标。

优先考虑的热路径优化：

1. **RefArray bitmap-word batching。** 对同一 Region/word 的候选先逐个完成地址和 white-slot
   合法性校验，再合并 mask，只执行一次 `fetch_or`；根据返回的 winning bits 决定哪些对象入队。
   不能为了 batching 跳过 TypeInfo/stale-slot 校验。
2. **same-Region 快路径。** 引用仍落在上一 Region 时复用 `MSRegionInfo*`、size class 和
   bitmap base，减少元数据重新定位。
3. **软件流水和预取。** 在确认引用属于 heap 后，提前预取后续对象 header、RegionInfo 和
   bitmap word，用批次隐藏内存延迟。
4. **按 Region 切分 range。** RefArray range 尽量沿 Region 边界分片，让不同 worker 少写同一
   bitmap cache line。

其中 bitmap-word batching 最贴合本 benchmark：对象按 size class 成批分配后按相同顺序加入
ArrayList，128/256B 数组又占对象数大头，同一 bitmap word 连续命中的概率较高。实际收益必须用
`atomicWordCommits / markWinner` 和 cache profile 验证，不能直接按“最多 64 bit/word”宣称
64 倍收益。

### 10.5 pressure/debt 指标与真实 edge 工作量失配

当前 `MSMarkPressurePolicy` 观察的是 Global Full+Partial `WorkBuf` 数量：

```text
每当 GC main 完成 10 个 buffer：
  currentDebt < previousDebt  -> 有进展
  currentDebt >= previousDebt -> noProgress++

连续 3 个 noProgress window -> debt-stalled -> one-way quiesce
```

这个指标在对象成本近似均匀时可以工作，但在本对象图中存在四类盲区：

1. **不知道 edge 数。** 一个 12.9 万引用数组和普通对象都只占一个 WorkBuf entry；
2. **看不到 in-flight 剩余工作。** worker 正在扫描超大 RefArray 时，Global buffer 数可能不降；
3. **看不到 private partial。** GC main/helper/assist 的 `putCur_` 未满时不在 Global debt 中；
4. **看不到 mutator active log。** 未满写屏障 log 要到 STW termination 才统一 flush。

此外，“Global debt 没下降”不等于“系统没有取得进展”。如果一个窗口内消费 100 个 buffer，
同时由大对象展开或写屏障新产生 100 个 buffer，队列净值不变，但 collector 实际完成了大量工作。

2 GiB cycle 38 是最直观的反例：

```text
ConcurrentMark = 8.696 ms
reason = debt-stalled
MarkTermination = 507.656 ms
flushed = 3,183 source entries
最终 marked objects = 8,963,614
```

并发阶段不到 9 ms 就终止，随后在单线程 STW 中运行约 508 ms。`flushed` 只有 3183 并不矛盾：
这些 source entries 展开后可以形成数百万对象的闭包，而日志没有记录 termination processed/edges。

pressure 优化应先改变“进展定义”，而不是直接把 `3` 改成更大的常量：

```text
effectiveOutstanding =
  publishedEntries
  + privatePartialEntries
  + activeLogEntries
  + inFlightRemainingEdges 的折算值

progress =
  consumedEdges / elapsed
  与 producedEdges / elapsed 的差
```

推荐策略：

1. checkpoint 同时满足最小时间或最小 visited-edge 窗口，避免 30 个轻量 buffer 在数毫秒内触发；
2. 只在 `effectiveOutstanding` 持续增长且 `producedRate >= consumedRate` 时认定 stalled；
3. stalled 前先拉满 helper，并请求 mutator 在安全点发布 partial active log；
4. 关闭新 Assist claim 后，先发布 GC-owned private tails，再做一个有界 concurrent drain grace；
5. grace 后仍无法收敛才进入 STW；
6. STW ordinary closure 使用 helper 并行 fixed-point drain，完成后再做 finalizer/weakref 顺序处理。

第 3～5 步必须保持 one-way 状态机清晰，避免 QUIESCING 后反复恢复并发；可以新增明确的
`DRAINING_RESIDUAL` 子阶段，而不是在现有 `QUIESCING` 上隐式 reopen。

### 10.6 优化方案与优先级

| 优先级 | 优化项 | 主要解决 | 风险/前置条件 |
|---|---|---|---|
| P0 | 增加 edge/winner/leaf/atomic/array-length/per-worker active 计数 | 先确定 3.24x natural 降速构成 | worker-local 计数，避免热路径全局原子 |
| P0 | persistent touch on/off、Assist on/off、1/2/4 worker A/B | 区分 mutator 内存争用、Assist 争用和 GC 内核成本 | 必须固定同一对象图和 runtime |
| P1 | 大 RefArray range task | 粗粒度任务、单 worker 长扫描、buffer 计价失真 | 需要验证并发数组写 barrier |
| P1 | bitmap-word batching + same-Region fast path | 降低原子 RMW、元数据定位和 cache-line 迁移 | 不能跳过 white-slot 合法性验证 |
| P1 | edge/time-based pressure + residual drain grace | 减少过早 `debt-stalled` 和大 STW residual | 先补齐 produced/consumed/private 指标 |
| P2 | 限制同时 Assist 数，并按 edge/time 而非 object 偿债 | 避免 GC worker 与 mutator 过度并发、业务线程长时间陷入大数组扫描 | 需同时观察 HTTP 吞吐和 Mark wall |
| P2 | 并行 MarkTermination ordinary closure | 缩短 unavoidable residual 的 STW | finalizer/weakref 必须在普通闭包稳定后处理 |
| P2 | Region-affinity queue/range 分区 | 改善对象、RegionInfo、bitmap 局部性 | 调度结构改动较大 |
| P3 | old generation / immutable arena / remembered set | 避免每轮重扫长期不变的 2 GiB live set | 架构级改造，但长期收益最大 |

短期最值得先实现的是 **RefArray range task + bitmap-word batching**。前者提高并行可分性，
后者直接减少这组连续叶子数组最昂贵的原子写；两者组合还能让不同 worker 按 Region range
分工，降低 bitmap false sharing。

如果目标是降低最大 STW，而不是只看总 Mark CPU，优先级应调整为：

```text
edge-based debt policy
  -> concurrent residual grace
  -> parallel ordinary termination closure
```

如果目标是提高 HTTP 业务吞吐，还必须单独处理 Assist。当前 Assist processed share 从
`23.78%` 增至 `53.67%`，说明大堆时一半以上已记录 WorkBuf 工作由 mutator 完成。更多 Assist
可能缩短 GC wall，却也可能让业务线程与 GC helper 争抢核心和内存带宽；不能只根据 Mark 时间
判断好坏。

当前不建议优先做：

- 继续增加 GC helper：现有 4 线程扩展效率只有约 44.5%，可能先放大内存争用；
- 把 Global queue 改成 lock-free：历史稳态锁等待约 1%～2%，不是第一矛盾；
- 删除 Region/slot/TypeInfo 校验：并发 Sweep/stale edge 安全前提尚不能放松；
- 只增大 `kNoProgressWindowsBeforeSTW`：可能降低 STW，也可能让 mutator 持续产出导致
  ConcurrentMark 无界延长；
- 只调大 GC threshold：减少 GC 次数，但单次仍需扫描完整大 live set，并提高峰值内存。

### 10.7 推荐实施顺序

建议每次只改变一个机制，按以下顺序推进：

1. **先补观测，不改策略。**
   增加 worker-local `visitedEdges/alreadyMarked/markWinner/leafWinner/casLoser/
   atomicWordCommits/regionSwitches`，再增加每个 WorkBuf 的 edge 数、scan time 和最大数组长度。
   同时记录 `maxAssistsInFlight`、每条 worker active time 以及 termination 同口径计数。
2. **先排除 benchmark 干扰。**
   为 persistent touch 增加诊断开关，执行 128 MiB/2 GiB touch on/off；再执行
   `CJ_MS_DISABLE_ASSIST=1` 与默认 Assist 的 A/B。该阶段不作为产品默认配置变更。
3. **优化单位 edge 成本。**
   先实现只针对对象 RefArray 的 same-Region cache/预取，再做 bitmap-word batching。
   每个改动独立测试，使用 `atomicWordCommits/markWinner` 证明原子操作确实下降。
4. **拆分大数组任务。**
   增加 RefArray range task，验证 1/2/4 worker 下的 `visitedEdges/s`、per-worker active time、
   p99 buffer/range scan time和业务吞吐。若 range task 有效，pressure 也切换到 edge/range 口径。
5. **修正收敛策略。**
   先以 shadow decision 记录“新策略何时会终止”而不改变行为；确认后再启用
   edge/time-based stalled 判定和 residual grace，比较 natural/debt-stalled 分布及 STW。
6. **并行 ordinary termination closure。**
   只并行普通闭包；finalizer 分类、resurrection fixed point 和 WeakRef 清理仍保持既定顺序。
7. **最后评估代际或 immutable 方案。**
   如果以上常数优化后，2 GiB 每轮仍受 O(live objects) 限制，则说明继续微调无法消除全堆扫描
   的算法下界，应转向 old generation、immutable region 或显式 long-lived arena。

每一步至少同时报告：

```text
natural effective objects/s 和 GiB/s
visitedEdges/s、markWinner/s、atomicWordCommits/s
1/2/4 线程扩展效率
debt-stalled 比例
ConcurrentMark / MarkTermination P50、P95、max
Assist CPU/processed share
HTTP 吞吐、失败率、RSS
```

综合当前证据，最可能取得可见收益的路径是：

```text
关闭或隔离 benchmark persistent touch 证明内存争用
  -> RefArray range task 解决任务不可分
  -> bitmap-word batching/same-Region fast path 降低每-edge 成本
  -> edge-based debt policy 避免过早 STW
  -> parallel termination 收敛剩余长尾
```

其中前三项主要改善 natural Mark 速率，后两项主要改善 `debt-stalled` 和 STW 尾延迟。

## 11. 2 GiB 持久对象下 Go 仍保持低 GC 延迟和高吞吐的原因

### 11.1 结论摘要与数据口径

结论先行：

1. 报告中的 Go `GC 平均时间=60.53 ms` 不是平均停顿，而是
   `STW sweep termination + concurrent mark/scan + STW mark termination` 三段 wall clock
   的简单相加。其中绝大部分是可与业务并发的中间段。
2. Go 稳态 103 轮 GC 的单轮总 STW 中位数只有 `0.290 ms`，P95 为 `0.430 ms`，
   最大值 `0.590 ms`；并发 mark/scan 中位数是 `45 ms`。
3. 2 GiB 持久 payload 主要由无指针 byte backing array 构成。Go 的 `[]byte` backing
   和 Cangjie 的 `RawArray<Byte>` 都必须被标记存活，但两边都不会扫描 2 GiB 字节内容；
   两边的持久闭包都主要是外层容器中的约 414 万条引用。**这是共同前提，不是 Go 相对
   MSGC 更快的原因。**
4. Go 1.26 本轮实际启用了 Green Tea GC。它按 span 批量处理小对象、合并 mark/scan bitmap
   元数据访问并预取；大型含指针对象又按最多 128 KiB 的 oblet 切分，避免一个 worker
   独占大引用数组。
5. Go 在进入 STW mark termination 前，通过 distributed termination + ragged barrier
   发布并排空每个 P 的 write-barrier/gcWork 私有缓存。STW 阶段检查到的应是空队列，
   不再像当前 MSGC 的 `debt-stalled` 路径那样单线程补完整个剩余闭包。

对 `go/gc.log` 重新解析，去掉前 6 个持久堆构建阶段的启动 cycle 后，`gc 7..109`
覆盖 `1.995s..61.794s`，结果如下：

| Go 稳态指标 | 数值 |
|---|---:|
| GC 轮数 | 103 |
| sweep termination + mark termination STW 总和 | 30.603 ms |
| 单轮 STW 中位数 / P95 / max | 0.290 / 0.430 / 0.590 ms |
| concurrent mark/scan 总 wall time | 6463.000 ms |
| 单轮 concurrent mark 中位数 / P95 / max | 45 / 157 / 205 ms |
| GC CPU 总和 | 9374.5 ms |
| 8P 总 CPU 容量中的 GC 占比 | 约 1.96% |
| live heap 中位数 / 均值 | 2276 / 2318.9 MB |
| heap goal 中位数 | 4559 MB |
| GC 间隔中位数 | 0.569 s |

典型日志：

```text
gc 7   ... 0.11+126+0.072 ms clock ... 4051->4284->2397 MB ... 8 P
gc 50  ... 0.17+36+0.088  ms clock ... 4498->4579->2262 MB ... 8 P
gc 100 ... 0.17+37+0.12   ms clock ... 4428->4507->2255 MB ... 8 P
gc 109 ... 0.39+50+0.13   ms clock ... 4428->4539->2280 MB ... 8 P
```

Go 1.26 `runtime/extern.go` 明确定义三段 clock 时间依次为：

```text
STW sweep termination
concurrent mark and scan
STW mark termination
```

因此对比报告的 `Go GC 总时间=6598.30 ms` 只能理解为所有三段 clock field 的加总，
不能当成请求累计被暂停 6.6 秒。实际全部 109 轮的两段 STW 加总只有 `32.895 ms`。

同一结果目录中，MSGC 的 51 轮包括持久堆构建期。以首次 `liveBytes >= 2 GiB` 起的后 25 轮
作为高内存阶段，重新解析得到：

| MSGC 高内存阶段指标 | 数值 |
|---|---:|
| GC 轮数 | 25 |
| ConcurrentMark 中位数 / P95 / max | 687.241 / 1930.759 / 2063.076 ms |
| MarkTermination 中位数 / P95 / max | 264.357 / 1824.475 / 2071.388 ms |
| MarkTermination 总和 | 16007.958 ms |
| 标记 live 中位数 / 均值 / max | 4.726 / 4.698 / 7.365 GiB |
| 标记对象数中位数 / 均值 / max | 24.09M / 24.02M / 51.49M |
| natural / debt-stalled | 9 / 16 |

这里 Go 和 MSGC 的 live 口径、对象布局不同，不能按字节直接除出“GC 快多少倍”；但
MSGC 后 25 轮有 16 轮因 `debt-stalled` 进入 STW，并在 MarkTermination 中补做大量闭包，
而 Go 的闭包完成判定发生在 concurrent mark 内，这个结构性差异已经足以解释停顿量级差距。

### 11.2 两边的 2 GiB live bytes 都不等于 2 GiB scan work

Go snapshot 中的持久对象结构是：

```go
var persistentStaticRootShards [][][]byte

staticRoots [][]byte
stackRoots  [][]byte
```

16 个 allocation worker 各保留一组 static roots 和一组 goroutine stack roots，总 payload
精确为 2 GiB；大小分布为：

```text
small: 128, 256, 512, 1K, 2K, 4K, 8K, 16K
large: 64K, 256K, 1M
large payload 占比: 0.9765625%
```

按 snapshot 的 `persistentObjectCount()` 公式复算：

```text
[]byte backing array 对象数 = 4,137,440
[][]byte -> []byte 引用边数 = 4,137,440
slice header 总大小          = 99,298,560 B = 94.70 MiB
其中 pointer slot 总大小     = 33,099,520 B = 31.57 MiB
```

每个 Go `[]byte` 有两层表示：

1. `[][]byte` backing array 中保存 3 word 的 slice header：data pointer、len、cap；
2. data pointer 指向单独的 byte backing array。

byte backing array 是 `noscan` 对象。`runtime.greyobject()` 在赢得 mark bit 后，对
`spanclass.noscan()` 直接累计 `bytesMarked` 并返回，不把对象放入 scan queue。因此：

```text
2 GiB payload:
  需要保活、设置 mark bit
  不需要逐 byte 读取或解释为引用

约 94.7 MiB 的 slice-header backing arrays:
  需要按精确 pointer bitmap 扫描
  实际产生约 414 万条指向 byte backing array 的引用
```

Go 的 scan accounting 还会包含 stack、globals、HTTP/JSON 对象和对象元数据，不能把
`31.57 MiB pointer slots` 当作本轮全部 scan work；但它证明了 **2 GiB 存活字节不是
2 GiB 指针扫描量**。这类 workload 的成本更接近“标记约 414 万个叶子对象 + 扫描约
414 万条持久引用”，而不是扫描 2 GiB 任意数据。

Cangjie snapshot 同样构造约 414 万个 `Array<Byte>` 值，由
`ArrayList<Array<Byte>>` 保留。这里必须区分语言层 `Array<Byte>` 和堆上的 raw backing：

```cangjie
public struct Array<T> {
    let rawptr: RawArray<T>
    let start: Int64
    let len: Int64
}
```

`Array<Byte>` 是 3-word struct，内联存放在 `ArrayList` 的
`RawArray<Array<Byte>>` backing 中，并不是每个值再单独分配一个 wrapper 对象。每个值的
`rawptr` 指向一个独立的 `RawArray<Byte>`；后者才是承载 128B～1MiB payload 的叶子堆对象。

这与 Go 布局几乎同构：

| Go | Cangjie |
|---|---|
| `[]byte {data,len,cap}` | `Array<Byte> {rawptr,start,len}` |
| 内联存入 `[][]byte` backing | 内联存入 `RawArray<Array<Byte>>` |
| data 指向 noscan byte backing | rawptr 指向无引用 `RawArray<Byte>` |

因此两侧外层描述符都是约 `4,137,440 × 24B = 94.7 MiB`，其中各有约 414 万个 pointer
slot。当前 MSGC 的实际路径是：

```text
扫描外层 RawArray<Array<Byte>>
  -> component 是 struct，按 Array<Byte> 的 GCTib 只访问 rawptr
  -> 对每个 RawArray<Byte> 调用 DiscoverObject()
  -> ResolveMarkTarget + stale/TypeInfo 校验 + TryMark()
  -> RawArray<Byte> 的 HasRefField() 为 false
  -> 不进入 grey WorkBuf
```

`ForEachRefFieldGCInline()` 会在 `!typeInfo->HasRefField()` 时直接返回。因此 Cangjie
同样只标记 `RawArray<Byte>` backing，不遍历 byte payload。

从持久结构看，两边需要处理的工作本质相同：

```text
约 414 万条外层 rawptr 引用
  + 约 414 万个 RawArray<Byte> 叶子对象的 mark bit
  + 外层容器和少量 root
```

所以这组 benchmark 不仅 payload 字节和 size distribution 相同，持久描述符布局和
pointer edge 数也高度等价。真正能解释速率差距的是这些引用和叶子对象“如何被标记”：

- Go Green Tea 对小 noscan 对象直接使用 span inline mark bits，拿到 size class 后
  `atomic.Or8` 标记并累计 bytes，立即返回；
- MSGC 对每条引用执行 Region/slot/stale/TypeInfo 定位和 bitmap 原子标记；
- Go 的大型含指针 backing array 可按 128 KiB oblet 切分，当前 MSGC 的大 RefArray
  仍缺少同等细粒度的 range task；
- Go 按 scan work/runway 调节并发工作，并在 STW 前完成 distributed termination；
  MSGC 仍可能因 WorkBuf debt 误判进入 `debt-stalled`，把 residual closure 留到 STW。

### 11.3 Go 1.26 Green Tea 的标记热路径

本轮 metadata 记录的版本是：

```text
go version go1.26.0 darwin/arm64
GODEBUG=gctrace=1
GOGC=<unset>          # 默认 100
GOMEMLIMIT=16GiB
gctrace: 8 P
```

对该 Go 安装执行 `go list`，实际选中的 experiment 文件是
`exp_greenteagc_on.go`；binary 也包含完整的 `runtime.scanSpan`、
`runtime.scanObjectsSmall` 和 `runtime.scanObject` 实现。因此本轮不能用旧版
“逐对象 workbuf 扫描器”的印象解释。

Go 1.26 `runtime/mgcmark_greenteagc.go` 对 Green Tea 核心思路的源码注释可以归纳为：

1. 首次发现小对象时设置 `marks` bit，并把对象所在 span 排队；
2. 暂缓逐对象扫描，让同一 span 内更多待扫对象聚合起来；
3. 出队 span 时对 `marks` 和 `scans` bitmap 做 word 级 union/difference；
4. 稀疏 span 走 localized sparse scan；密集 span 走 packed span scan；
5. span queue 使用 FIFO，以提高同 span 聚合概率和局部性；
6. 批量读取对象元数据、复用 size class 信息并为后续引用地址做 prefetch。

这与当前 MSGC 每条引用都通过通用 `DiscoverObject()` 重新定位 Region、检查 slot/TypeInfo、
原子更新 mark bitmap 的路径有本质的常数差异。Green Tea 不改变 tracing 的 O(edges)
下界，但减少了：

- 对象元数据的随机访问次数；
- 同一 span bitmap 的重复定位；
- 一个对象一个调度 entry 的队列开销；
- cache line 来回切换和无法提前预取的问题。

Go 仍保留精确的对象级语义：已经标记的不重复处理，noscan leaf 只标记不扫描，
含指针对象只遍历 pointer bitmap 指定的 slot。它不是“看到 span 就保活整个 span”的
保守 GC。

对大型含指针对象，Go 还有第二层任务切分。`maxObletBytes = 128 << 10`，源码注释明确
说明大对象被拆成最大 128 KiB 的 oblet，目标是提高并行性、降低单次不可抢占扫描延迟，
并把 scan preemption 控制在约 100 微秒量级。

这个机制正好命中本 benchmark 的外层 root backing array：

```text
16 workers × (staticRoots + stackRoots) = 32 个大型 [][]byte backing arrays
总 slice-header 大小约 94.7 MiB
平均每个 backing array 约 2.96 MiB
```

即使某个 root array 有数十万条引用，也不会由一个 worker 一口气扫到底；它会被切成
多个 128 KiB oblet 供其他 P 消费。当前 MSGC 文档前文建议的 `RefArray range task`
正是对应能力，且这组数据进一步提高了它的优先级。

### 11.4 Pacer、并发完成判定和亚毫秒 Mark Termination

#### 11.4.1 Pacer 给 mark 留出足够 runway

本轮 `GOGC` 未设置，所以默认是 100。稳态 live heap 中位数约 2276 MB，heap goal
中位数约 4559 MB，基本体现“goal 约为上一轮 live 的两倍”。`GOMEMLIMIT=16GiB`
明显高于 4.3～5.3 GB 的实际 goal，本轮不是 memory-limit hard regime。

Go pacer 不只是固定到达某个百分比就启动。源码中的 `consMark` 持续估计：

```text
应用 allocation rate（bytes / mutator CPU time）
--------------------------------------------------
GC scan rate（scan bytes / GC CPU time）
```

再据此计算下一轮需要的 heap runway 和 trigger。Assist 比例也按：

```text
remaining scan work / remaining heap allocation bytes
```

动态更新。与当前 MSGC 以 WorkBuf 数量和 object processed 近似压力相比，这种计价更能
区分“一个 entry 含 12.9 万条 edge”和“一个 entry 是 noscan leaf”。

Go 的 `gcBackgroundUtilization` 是 0.25；8P 下后台 mark 目标约占 2 个 P，同时还允许
mutator assist 和 idle worker。稳态 103 轮 gctrace 的 GC CPU 总和约 9.3745 CPU-seconds，
相对 `59.799s × 8P` 的总容量约 1.96%，与日志显示的 `1%～2%` 一致。这说明本轮实际
scan work 远小于 2 GiB payload，GC 也没有长期吃满 2 个核。

#### 11.4.2 Go 在 STW 前完成闭包，而不是在 STW 中补闭包

Go 的 concurrent mark 完成条件不是“连续几个窗口 Global buffer 数没下降”。核心流程是：

```text
所有 GC worker 调用 gcEndWork()
  -> nwait == nproc 且全局/root/span work 均不可用
  -> gcMarkDone() 取得 transition lock
  -> forEachP flush 每个 P 的 write-barrier buffer 和 gcWork
  -> 如果 flush 出新 grey work，回到 concurrent mark 继续 drain
  -> 再次确认 global/local work 都为空
  -> 才 stop-the-world 进入 mark termination
```

进入 STW 后，`gcMark()` 会检查：

- root jobs 已全部完成；
- global full queue 为空；
- 每个 P 的 `gcWork` cache 为空。

若缓存非空，runtime 直接认为不变式被破坏，而不是在这里串行继续常规 tracing。
completion barrier 之后产生的 write-barrier pointer 理论上都指向 black object，所以
正常模式下可以直接丢弃 buffer。Go 也保留检测到 race 后恢复 concurrent mark 的防御路径，
但本轮日志没有表现出明显的 termination residual。

当前 MSGC 的高内存阶段恰好相反：

```text
25 cycles:
  natural      = 9
  debt-stalled = 16

debt-stalled:
  连续 3 个 WorkBuf debt no-progress window
  -> one-way quiesce
  -> STW flush active/private work
  -> 单线程 fixed-point drain 普通闭包
```

因此 Go 的亚毫秒 mark termination 不是因为“终止阶段的单线程 mark 特别快”，而是因为
大部分时候进入 STW 前已经没有闭包可做；MSGC 的长 MarkTermination 则包含了本应尽量留在
并发阶段完成的 residual tracing。

### 11.5 吞吐高不只来自 GC

本轮汇总数据：

| 指标 | Go | MSGC | Go / MSGC |
|---|---:|---:|---:|
| 吞吐 | 1224 req/s | 271 req/s | 4.52x |
| 平均 CPU | 595.66% | 324.94% | 1.83x |
| 每 100% CPU 的吞吐 | 205.49 req/s | 83.40 req/s | 2.46x |
| 失败率 | 0.0148% | 11.3541% | — |
| 平均 RSS | 5080 MB | 4451 MB | 1.14x |
| 峰值 RSS | 5557 MB | 7299 MB | 0.76x |

所以 Go 吞吐高有两个同时成立的原因：

1. **使用了更多 CPU。** Go 平均约占 5.96 个核，MSGC 约占 3.25 个核。仅这一项就解释了
   吞吐差距的一部分；Go scheduler、netpoll、server/client goroutine 和 GC worker 能让
   更多核心持续做有效工作。
2. **单位 CPU 完成的请求也更多。** 归一化后仍约 2.46x，这不可能仅由“CPU 打得更满”
   解释。GC 常数更低、分配器/调度器、`net/http`、`encoding/json` 和对象模型都参与了差异。

Go 的 GC CPU 在稳态约 9.37 CPU-seconds。按 `595.66% × 59.799s` 粗算，本轮进程总 CPU
约 356 CPU-seconds，GC 约占其中 2.6%；大部分 CPU 仍在做 HTTP、JSON 和调度，而不是 GC。
MSGC 侧仅高内存阶段的 MarkTermination 就累计约 16.0 秒 STW，足以让 5 秒 client timeout
成批触发，并产生连接关闭、broken pipe 和后续重试/清理开销。

不过不能把 4.52x 全标成 GC 收益。两边虽然 payload、并发数、持久 payload 字节和角色配置
一致，但并不共享同一套：

- HTTP client/server；
- JSON parser/writer；
- string/map/array 对象布局；
- socket、timer、scheduler；
- 分配 fast path 和内存复用策略。

因此这轮能严谨得出的结论是：

```text
Go 的 GC 不构成主要吞吐瓶颈；
MSGC 的长 ConcurrentMark + 大量 STW residual 是明确瓶颈之一；
剩余每核吞吐差还需要 CPU profile 按 allocator / JSON / HTTP / scheduler 拆分。
```

平均 RSS 方面 Go 比 MSGC 高约 14%，这是它以更大 heap runway 换取低 GC CPU 的一部分成本；
但 Go 峰值反而低约 24%，说明稳定的约 4.5 GiB heap goal 比 MSGC 标记集膨胀到 7.365 GiB、
峰值 RSS 7.299 GiB 更可控。

### 11.6 Go 与 Cangjie MSGC 的 Mark 差异及减速原因汇总

先固定比较前提：本用例的 2 GiB 持久 payload 在两边都由无引用 byte backing
承载，两边都只标记 backing 对象而不扫描其中的 byte 内容。持久图均约为 414 万条引用加
414 万个 noscan 叶子对象，因此“Cangjie 扫描了 2 GiB byte 内容”不是速率差异来源。

| 比较维度 | Go 1.26 Green Tea | Cangjie MSGC 当前实现 | 对 Mark 速率的可能影响 | 判断 |
|---|---|---|---|---|
| 2 GiB byte payload | `[]byte` backing 为 noscan，只设置 mark bit | `RawArray<Byte>` 的 `HasRefField=false`，只设置 mark bit | 两边相同，不能解释差距 | 已确认，排除项 |
| 持久描述符和引用数 | 约 414 万个 3-word slice header、414 万条 data pointer | 约 414 万个 3-word `Array<Byte>` struct、414 万条 `rawptr` | 两边的持久图规模高度等价 | 已确认，排除项 |
| 小型 noscan 叶子发现 | 利用 span、size class 和 inline mark bits；叶子标记后直接返回 | 每条边进入通用 `DiscoverObject/ResolveMarkTarget` 路径 | MSGC 为每个叶子支付更多固定指令和元数据访问，414 万次累计后影响大 | 高概率主因 |
| 地址和对象合法性校验 | span 上下文可复用，Green Tea 以 span 为局部处理单位 | 逐边解析 Region、size class、slot 边界、对齐、`allocPtr`，white slot 再检查 `stateWord/TypeInfo` | 增加随机访存、分支和 cache/TLB miss；大堆时元数据局部性进一步下降 | 源码已确认，影响待 A/B 定量 |
| Mark bitmap 更新 | 小对象 mark bit 位于 span 内；后续 span scan 可批量处理 mark/scan bitmap | 每个 white leaf 对目标 Region bitmap 执行原子 `fetch_or` | 两边都需要原子置位，但 MSGC 缺少同等 span 聚合和元数据复用；bitmap cache-line 竞争/伪共享可能更重 | 高概率主因 |
| 元数据局部性与预取 | Green Tea 按 span 聚合、FIFO 排队、批量读取元数据并预取引用目标 | 以对象/WorkBuf 为中心，边到目标 Region、对象头和 bitmap 的访问更分散 | 在 414 万个小对象和大堆下更容易受 LLC、TLB、DRAM 延迟限制 | 高概率主因 |
| 大型外层引用数组切分 | 大型含指针对象按最大 128 KiB oblet 切成可并行任务 | `RawArray<Array<Byte>>` 当前按整个数组扫描，缺少等价 range task | 少数约 3 MiB 的大数组可能被单 worker 长时间独占，限制并行度并形成长尾 | 源码结构已确认，P1 原因 |
| 工作量计价 | pacer/assist 主要使用 scan work、scan bytes 和剩余分配 runway | pressure 主要观察 WorkBuf debt、processed object 和历史 object count | 一个大引用数组只占少量 WorkBuf，却包含大量 edge；可能低估在途工作并错误判断 no-progress | 高概率主因 |
| 私有工作可见性 | P-local work 会在分布式完成协议中统一 flush，再决定是否结束并发标记 | GC main/helper 的 private partial 和 mutator `activeLog_` 可暂时不反映在 Global debt 中 | Global debt 不能完整代表真实剩余工作，可能过早 quiesce | 已由当前协议确认 |
| 并发完成与 Mark Termination | 所有 worker 无工作后 flush 每个 P；若产生新 grey work，返回 concurrent mark 继续清空 | 连续 no-progress 可 one-way quiesce，late/private residual 交给 STW `MarkTermination` 固定点清空 | 不一定降低纯 concurrent objects/s，但会显著降低“ConcurrentMark + Termination”的端到端 mark 速率并拉长 STW | 本轮直接证据：16/25 轮 `debt-stalled` |
| GC 触发和 runway | 默认 `GOGC=100`，本轮 goal 约为上一轮 live 的 2 倍；按 allocation/scan rate 调整 trigger | threshold、WorkBuf pressure和固定 time budget共同控制；未按真实 edge/scan bytes建立等价 runway模型 | 触发过晚会在堆更大、局部性更差时追赶；过早 quiesce会把闭包转入 STW | 中高概率，需同 live-set A/B |
| Mutator assist | 按剩余 scan work / 剩余可分配字节更新 assist 比例 | 以历史对象数、GC main credit和 WorkBuf债务近似；对象数不等于 edge 数 | 高扇出对象下 assist 定价可能偏低或滞后，后台 mark 跟不上分配速度 | 中高概率 |
| Worker 扩展效率 | span/oblet 提供较细粒度、局部性较好的并行任务 | 当前 4 mark 线程代表性加速仅约 1.779x，约 44.47% 并行效率 | 单条边访存瓶颈、bitmap 原子写和任务粒度共同限制“继续加线程”的收益 | 已有 case27 采样证据 |
| Sweep/Scavenge 内存带宽竞争 | GC/scavenger有独立 pacing，当前用例未见其成为主要 GC CPU | MSGC 后台 Scavenge 可与 Mark 重叠执行 `bzero/mmap` | 大堆下争用 LLC/DRAM，可能进一步压低 mark throughput；不能仅凭聚合 sample 定论 | 可疑次因，需重叠窗口 A/B |
| 实际完整对象图 | 持久 byte 图之外还有 Go HTTP/JSON/runtime 临时对象 | 持久图等价，但本轮 MSGC 每轮实际标记约 24M～51M 对象，还包括 Cangjie HTTP/JSON/runtime 对象 | 即便持久图等价，语言库和对象布局仍可能让 MSGC 的总对象数、总 edge 数更大；这会直接拉长 mark | 高影响，但需统一 `visitedEdges/leafWinners` 后定量 |
| 长寿命对象重复标记 | 当前仍需在每轮 tracing 中重新确认长寿命可达图 | 同样每轮重新追踪约 414 万条持久边和叶子 | 这是两边共同的 O(long-lived graph) 成本，不解释当前 Go/MSGC 差距，但说明代际/old-region 方案仍有长期收益 | 共同成本 |

#### 11.6.1 这四项差异共同依赖什么数据结构

两边共同使用的抽象都是“将同一分配粒度中的对象放入固定大小分组，再用组级元数据从地址定位
对象槽和 mark bit”，但分组元数据编码的信息不同：

| 数据结构职责 | Go | Cangjie MSGC |
|---|---|---|
| 地址目录 | `mheap_.arenas -> heapArena`；`heapArena.spans[page]` 可定位 `mspan` | 连续 `MSRegionInfoArray`；`(addr-heapStart)>>15` 直接定位 `MSRegionInfo` |
| 对象分组 | `mspan`，由一个或多个 Go 8 KiB page 组成 | 固定 32 KiB Region |
| 分组分类键 | `spanclass = sizeclass + noscan bit` | `sizeClass`，没有 scan/noscan 维度 |
| 分组内对象布局 | `startAddr/limit/elemsize/nelems/divMul` | `regionEnd/sizeClass/totalSlotCount/divMagic/allocPtr` |
| 分配状态 | `mspan.allocBits`、`freeindex` | `MSRegionInfo.allocBitmap`、`allocPtr`、`regionType` |
| 普通 mark 状态 | `mspan.gcmarkBits` | `MSRegionInfo.markBitmap` 指向 `MSBitmapArena` sidecar |
| Green Tea 小对象 mark/scan 状态 | span 尾部 `spanInlineMarkBits{scans,owned,marks,class}` | 当前没有独立 scans bitmap，也不按 Region 聚合待扫对象 |
| 是否含引用 | `spanclass.noscan()` 是 span 级属性 | `BaseObject.stateWord -> TypeInfo::HasRefField()` 是对象/类型级属性 |
| 精确字段布局 | 小对象 heap bits 或对象 type metadata | `TypeInfo -> GCTib/component TypeInfo` |
| 对象头 | 小对象不依赖对象头判断 noscan | 每个对象开头 8 B `stateWord` 保存 `TypeInfo` 和状态 |

这里最重要的不是“span 比 Region 小”，而是 **Go 的分配分组键包含 noscan，MSGC 的分组键
只有 size class**。Go 对每个 size class 分别维护 scan span 和 noscan span；MSGC 的
`activeRegions_[36]` 只按 36 个 size class 索引，同一个 Region 可以保存大小相同但 TypeInfo
不同的对象。因此：

```text
Go:
  已知 span/spanclass
  -> 已知对象大小
  -> 已知整个 span 是否 noscan

MSGC:
  已知 MSRegionInfo/sizeClass
  -> 只知道对象大小
  -> 不知道该 slot 的具体类型和是否含引用
  -> 必须读取对象 stateWord 和 TypeInfo
```

#### 11.6.2 Go 小 noscan 叶子的地址到 mark-bit 路径

Go 1.26 Green Tea 对 `16B <= elemsize <= 512B` 且只有一页的 span 使用 inline mark bits。
`spanInlineMarkBits` 是 128 B，直接放在该 Go page 的尾部：

```text
one-page span
+----------------------------------------------------------+
| same spanClass objects | heap pointer bits | inline bits |
+----------------------------------------------------------+
                                               |
                                               +-- scans[63]
                                               +-- owned
                                               +-- marks[63]
                                               +-- class(sizeclass + noscan)
```

`tryDeferToSpanScan(p)` 的 noscan 叶子快路径为：

```text
p
 -> heapArenaOf(p)
 -> 检查 heapArena.pageUseSpanInlineMarkBits[page]
 -> base = alignDown(p, GoPageSize)
 -> q = base + GoPageSize - sizeof(spanInlineMarkBits)
 -> 从 q.class 取得 sizeclass/noscan
 -> objIndex = (p-base) * classDivMagic >> 32
 -> atomic load/Or8(q.marks[objIndex])
 -> q.class.noscan() 为 true
 -> bytesMarked += classSize，直接返回
```

这条路径不读取 `mspan`，也不读取 byte backing 的对象内容或类型头。它只访问 arena
目录中的能力 bitmap 和目标 page 尾部的 inline mark metadata。

本用例 small tier 将字节平均分给 128/256/512/1K/2K/4K/8K/16K 八类。按对象数计算，
128/256/512 B 三类约占所有持久 byte backing 的 88%，正好命中该 inline 快路径。

大于 512 B 的对象不使用 inline mark bits，路径退化为：

```text
p
 -> heapArena.spans[page] 得到 mspan
 -> 检查 mspan.state/base/limit
 -> objIndex = (p-base) * mspan.divMul >> 32
 -> mspan.gcmarkBits[objIndex]
 -> mspan.spanclass.noscan() 为 true后直接返回
```

虽然这条路径会读取 `mspan`，但是否 noscan 仍是 span 级属性，仍不需要读取叶子对象头。

#### 11.6.3 Cangjie MSGC 的地址到 mark-bit 路径

MSGC 使用固定 32 KiB Region。每个 Region 对应一个 64 B、单 cache line 的
`MSRegionInfo`：

```text
MSRegionInfo
  allocPtr / regionEnd
  nextRegionIdx / prevRegionIdx
  allocBitmap* / markBitmap*
  totalSlotCount / freeSlotCount / divMagic
  sizeClass / sweepState / regionType / flags
  sweepGen / reserved
```

bitmap 位于独立的 `MSBitmapArena`。每个 Region 预留固定 1024 B sidecar slot，实际按当前
size class 的 `bitmapWords` 紧凑使用：

```text
bitmapArenaBase + regionIdx * 1024
+----------------------------------------------+
| allocBitmap[bitmapWords] | markBitmap[bitmapWords] |
+----------------------------------------------+

alloc bit: 1=free, 0=allocated
mark bit : 1=alive, 0=dead
```

`DiscoverObject(p)` 的当前路径为：

```text
p
 -> heap-range gate
 -> heapOffset = p - heapStart
 -> regionOffset = heapOffset & (32KiB-1)
 -> info = regionInfoArray[heapOffset >> 15]
 -> 检查 regionType/sizeClass/markBitmap/totalSlotCount
 -> slotIdx = regionOffset * divMagic >> 32
 -> 检查 slotIdx、精确 slot 对齐和 BUMP allocPtr
 -> relaxed load info->markBitmap[wordIdx]
 -> 如果已经 marked，返回
 -> acquire 读取 p 开头的 8 B stateWord
 -> 解码 TypeInfo*，检查非空，防止 stale/free slot 被误标
 -> atomic fetch_or(info->markBitmap[wordIdx], bit)
 -> 赢得 mark 后调用 TypeInfo::HasRefField()
 -> RawArray<Byte> 返回 false，不进入 WorkBuf
```

因此 Cangjie 对一个新发现的 noscan 叶子，至少可能触及四组不同位置：

```text
MSRegionInfo cache line
MSBitmapArena mark word
对象首 cache line中的 stateWord
TypeInfo（Array 类型还可能访问 component TypeInfo）
```

Go 的 Green Tea 小 noscan 快路径主要触及：

```text
heapArena 能力 bitmap
目标 span 尾部、与对象同一 page 的 inline marks/class
```

它不需要读取 128～512 B byte backing 的首 cache line。大堆下，这个“每个叶子是否必须触碰
对象本体和独立 TypeInfo”的差别会放大 LLC/TLB/DRAM 成本。

#### 11.6.4 四个性能现象如何由这些结构产生

| 性能现象 | 数据结构根因 |
|---|---|
| 叶子对象发现路径更长 | MSGC Region 不区分 scan/noscan，必须从对象 `stateWord` 找 `TypeInfo`；Go 从 `spanclass` 直接知道 noscan |
| 对象合法性检查更多 | MSGC 允许并发 Sweep、slot 复用和 mixed-TypeInfo Region，需要确认 Region/slot/`allocPtr`/`stateWord`；Go inline 快路径依赖精确 GC 引用和 span 能力 bitmap |
| Bitmap 更新局部性较弱 | Go 小对象 marks 与 class 内联在目标 span page；MSGC 先读 `MSRegionInfo.markBitmap*`，再访问独立 sidecar |
| 元数据局部性更差 | MSGC 一条 edge 横跨 RegionInfo、sidecar、对象头、TypeInfo；Go 可在 span 内批量合并 `marks &^ scans`，复用 class、大小和字段 bitmap |

需要注意：两边最终都要执行原子 mark-bit 更新，差距不能简单归因为 `Or8` 对
`fetch_or(uint64)`。更重要的是：

1. 原子操作前需要多少地址解析和合法性检查；
2. noscan 判断是否需要读取对象本体；
3. mark metadata 是否与同组对象共址；
4. 能否按 span/bitmap word 合并后续扫描和调度。

对应 MSGC 的结构性优化方向是：

1. 将小对象池从 `sizeClass` 扩展为 `(sizeClass, scanClass)`，在 `MSRegionInfo` 中保存
   `noscan` 属性，使叶子判断可以停留在 Region 级；
2. 为 Mark 建立并发安全的 slot occupancy 协议。当前 BUMP 分配不实时维护
   `allocBitmap`，BITMAP 分配又依赖 owner 普通读写，不能直接删除 white-slot
   `stateWord` 校验；
3. 对大 RefArray 使用 range task，并在每个 range 内缓存 `MSRegionInfo/sizeClass/bitmap`
   坐标，按 Region 或 bitmap word 批量处理；
4. 评估将 mark hot metadata 与 `MSRegionInfo` 更紧密地布局，或通过确定性 sidecar
   地址计算减少 `markBitmap*` 间接加载；
5. 更长期可增加独立 mark/scanned 状态和 Region 级待扫任务，迁移 Green Tea 的“延迟后
   批量扫描”原则，但 mixed TypeInfo 对象仍需要精确的字段布局来源。

#### 11.6.5 针对六类问题的 MSGC 分阶段改造方案

不能把六类问题分别做成六个零散补丁。它们实际上需要三层能力：

```text
第一层：把当前逐 edge 热路径批量化
第二层：让 Region 本身携带 scan/noscan 分类
第三层：建立不读取对象头也能证明 slot 有效的并发协议
```

##### 第一阶段：Runtime-only 热路径改造

这一阶段不改变 compiler/runtime 分配 ABI，也不删除现有合法性检查。

| 改造项 | 具体设计 | 主要解决的问题 | 安全边界 |
|---|---|---|---|
| Worker-local `MSMarkResolveCache` | 缓存上一目标的 Region 地址范围、`MSRegionInfo*`、`sizeClass/divMagic`、bitmap base；同一 Region 命中时跳过地址到 RegionInfo 的重复解析和静态字段检查 | 叶子发现路径长、元数据分散 | 只缓存当前 worker 私有解析结果；Region 生命周期变化时按 GC epoch 清空 |
| RefArray range task | 将大 `RawArray` 拆成约 64～128 KiB 描述符范围；任务保存 `array/typeInfo/start/end`，由多个 helper 并行扫描 | 批处理弱、单 worker 长尾 | 使用独立 GC-only range queue，不修改 write-barrier `WorkBuf` ABI |
| bitmap-word batching | range 内先解析一小批引用，按 `(MSRegionInfo*, wordIdx)` 聚合 bit mask；white candidate 仍逐个做 `stateWord` 校验，最后一次 `fetch_or(mask)`，根据返回旧值识别 winner | Bitmap RMW 多、cache-line 竞争 | 必须先验证每个 white slot；禁止先置位再验证 |
| same-Region顺序扫描 | 对连续目标复用 `MSRegionInfo/markBitmap/divMagic`，region变化时才刷新；本用例按size tier顺序保存叶子，命中率预计很高 | 叶子发现路径、元数据局部性 | 先以worker-local `sameRegionHits`计数验证 |
| 软件流水与prefetch | 处理当前引用时预取后续目标的RegionInfo和mark word；在仍需读对象头时同时预取后续stateWord | 随机访存延迟 | 预取距离需按macOS arm64实测，不能固定照搬Go |
| 固定sidecar分区 | 保持每Region 1024B预留，但固定前512B为alloc、后512B为mark，使mark base可由`bitmapArenaBase + regionIdx*1024 + 512`直接计算 | bitmap指针间接加载、布局计算 | 需要同步核对Sweep、统计、清bitmap和compiler ABI；`markBitmap*`字段可先保留兼容 |

建议为第一阶段新增worker-local计数，周期末汇总：

```text
visitedEdges
sameRegionHits / regionSwitches
markProbeHits
whiteStateWordLoads
leafWinners / pointerfulWinners
bitmapRMWs / bitsSubmitted / bitsWon
rangeTasks / rangeEdges / rangeSteals
```

这一阶段对 case26 最直接，因为约94.7 MiB外层描述符可以拆成数百个range，且引用按
size tier分布，适合same-Region和bitmap-word batching。

##### 第二阶段：按 `(sizeClass, scanClass)` 分 Region

当前 `activeRegions_[36]` 只按size class选Region。目标结构应变为逻辑上的：

```text
activeRegions_[36][2]
                    0 = NO_REF
                    1 = HAS_REF
```

不建议直接把现有固定ABI数组原地扩大为72项。当前LLVM fast path依赖
`AllocBufferMS::activeRegions_`的固定偏移。较安全的迁移顺序是：

1. 在`MSRegionInfo::flags_`增加`FLAG_NO_REF_REGION`，不改变64B布局；
2. Runtime先增加独立的no-ref slow allocation入口和独立Region bucket；
3. compiler在已知`TypeInfo`时向分配入口传递一个`scanClass` bit；
4. LLVM fast path再升级为按`sizeClass + scanClass`选择active Region；
5. 保留旧size-only ABI一段时间，未知类型统一进入`HAS_REF`保守路径。

分配时只计算一次：

```text
TypeInfo::HasRefField()
  -> NO_REF Region
  或 HAS_REF Region
```

随后每轮GC都可以直接从`MSRegionInfo`知道该Region是否可能含引用。第一版仍保留
white-slot `stateWord != 0`校验，但winner不再需要解引用`TypeInfo::HasRefField()`：

```text
NO_REF Region:
  resolve slot
  -> 验证stateWord非空
  -> mark bit
  -> 直接结束
```

这一步能消除TypeInfo/component TypeInfo访问，但还不能消除对象首cache line访问。

large object也应在primary `MSRegionInfo::flags_`保存NO_REF属性。large路径本来已经验证
`base->allocPtr == addr`和span边界，因此比small slot更容易建立不读取TypeInfo的叶子快路径。

##### 第三阶段：建立稳定occupancy协议，消除noscan对象头访问

仅有`FLAG_NO_REF_REGION`还不够。当前white-slot `stateWord`检查同时承担“这个对齐地址是否仍是
已分配对象”的证明。直接删除会把stale edge指向的free slot置为marked，其他线程看到mark bit后
可能跳过验证，破坏GC正确性。

当前`allocBitmap`不能直接替代：

- BUMP分配热路径不实时更新`allocBitmap`；
- BITMAP Region由owner使用普通load/store分配；
- Mark与分配并发时尚无可供collector原子读取的slot发布协议。

推荐优先评估“GC epoch封存”方案，避免为每次分配增加全局原子：

```text
MarkSetup:
  封存旧NO_REF active Region
  -> materialize稳定alloc bitmap
  -> 本轮不再向这些Region分配

ConcurrentMark:
  旧NO_REF Region用稳定alloc bitmap证明slot有效
  -> mark bit
  -> 不读取对象stateWord

Mark期间新分配:
  进入new-epoch Region
  -> 按black allocation处理
  -> 下一轮再成为普通旧Region
```

如果封存造成的Region尾部浪费不可接受，备选方案是增加`publishedAllocBitmap`：

1. 对象和stateWord完成初始化；
2. 以release语义发布slot bit；
3. Marker以acquire语义检查slot bit；
4. Sweep清slot时与分配/Mark遵守同一epoch协议。

该方案会给分配热路径增加原子或发布成本，必须同时衡量mutator吞吐，不能只看mark变快。

完成第三阶段后，NO_REF small leaf才可以安全走：

```text
addr
 -> direct RegionInfo
 -> FLAG_NO_REF_REGION
 -> stable occupancy bit
 -> atomic mark bit
 -> return
```

此时才能真正做到“不读取对象头、不读取TypeInfo、不进入WorkBuf”。

##### 第四阶段：Region级 `mark/scanned` 聚合

前面三阶段主要优化noscan叶子和大数组。对于含引用的小对象图，可以再引入类似Green Tea的
Region聚合扫描：

```text
markBitmap
scanBitmap
region scan ownership/dirty state
Region scan queue
```

首次发现对象时设置mark bit并尝试将Region入队；worker出队后计算：

```text
toScan = markBitmap & ~scanBitmap
scanBitmap |= markBitmap
```

再按slot顺序扫描`toScan`对象。由于HAS_REF Region中仍可能混合不同TypeInfo，扫描每个winner时
仍需读取TypeInfo/GCTib，但RegionInfo、slot size、bitmap和对象地址计算可以批量复用。

当前sidecar最多需要：

```text
allocBitmap 512B
markBitmap  512B
scanBitmap  512B
```

因此需要扩大sidecar或新建scan bitmap arena，不能在并发分配/Mark期间直接复用alloc bitmap。

##### 推荐实施顺序与预期收益

| 优先级 | 实施内容 | 改动范围 | 对case26的预期价值 |
|---|---|---|---|
| P0 | 补齐edge/region/bitmap/stateWord计数 | Mark Worker/REPORT | 先确认成本分布，避免按objects/s误判 |
| P1 | RefArray range task + completion计价 | Collector/Work queue | 提升并行度，消除大数组单worker长尾 |
| P1 | same-Region cache + bitmap-word batching | Discover/array scanner | 减少Region解析、bitmap RMW和cache-line竞争 |
| P1 | 固定sidecar mark半区 + 直接地址计算 | BitmapArena/RegionInfo/Sweep | 减少markBitmap间接加载，改善metadata规律性 |
| P2 | `(sizeClass, scanClass)` Region | Allocator + Runtime ABI + LLVM | 消除noscan winner的TypeInfo/component访问 |
| P2 | NO_REF Region epoch封存/稳定occupancy | Allocator/MarkSetup/Sweep | 完全消除旧noscan叶子的对象头访问 |
| P3 | Region级mark/scanned聚合 | Collector/BitmapArena/queue | 优化一般pointerful小对象图 |
| P3 | old/immutable Region或代际GC | Heap整体架构 | 避免每轮重复追踪2 GiB长寿命图 |

不建议优先做的操作：

```text
直接删除stateWord/Region/slot合法性检查
把mark probe和fetch_or合成“先标再验证”
只增加GC helper数量
直接把activeRegions_[36]改成[72]而不升级LLVM ABI
完整照搬Go mspan布局
```

前三项推荐落地能力应组合设计：

```text
RefArray range task
  + range内same-Region/bitmap-word batching
  + edge/range/private/in-flight completion计价
```

否则只拆range却仍以WorkBuf对象数判断进度，可能继续把大量在途edge误判成no-progress。

#### 11.6.6 全部优化叠加时的冲突、依赖与最终架构

这些优化不能按独立patch全部相加。存在三类关系：

```text
直接冲突：两个设计占用同一布局或采用不同正确性协议
顺序依赖：后一个优化必须等待前一个建立安全前提
集成冲突：两者可以共存，但必须共享调度/终止状态
```

##### 直接冲突和替代关系

| 优化组合 | 关系 | 冲突原因 | 推荐处理 |
|---|---|---|---|
| 固定1024B sidecar为`alloc 512 + mark 512` vs Region级`scanBitmap` | 直接布局冲突 | 三张最大bitmap需要1536B，1024B容不下 | 若确定最终做`mark/scanned`，不要把1024B二分布局作为终态；直接设计三平面bitmap |
| GC epoch封存 vs 每对象`publishedAllocBitmap` | 两种occupancy替代方案 | 前者用Region封存换取稳定性，后者用分配发布成本换取继续复用；同时完整实现会重复付费和放大状态空间 | 先选择epoch封存作为主方案；只有尾部浪费不可接受时再评估published bitmap |
| `activeRegions_[36]`原地扩成72项 vs 现有LLVM fast-path ABI | 直接ABI冲突 | `activeRegions_`和`activeLog_=+896`偏移已固化 | 保留旧36项作为保守HAS_REF路径，在ABI窗口后增加NO_REF数组或发布v2 fast-path state |
| 将direct bitmap地址固化到compiler vs bitmap布局继续演进 | ABI/演进冲突 | scan bitmap、plane布局和sidecar扩容会改变地址公式 | direct地址计算先只放Runtime；最终布局稳定后再暴露版本化compiler ABI |

因此前文“固定sidecar mark半区”只适合作为短期实验。如果目标是全部优化一次落地，应改为：

```text
alloc bitmap plane: allocBase + regionIdx * 512
mark  bitmap plane: markBase  + regionIdx * 512
scan  bitmap plane: scanBase  + regionIdx * 512
```

三个plane分别做虚拟地址预留并按chunk懒提交。这样mark地址确定、没有`markBitmap*`间接寻址，
也为scan bitmap保留扩展空间。最大逻辑元数据从每Region 1024B增加到1536B，即相对32KiB
Region从3.125%增加到4.6875%；实际RSS仍可按已使用Region和HAS_REF Region懒提交。

##### 严格顺序依赖

| 后续优化 | 必须先完成 | 原因 |
|---|---|---|
| NO_REF winner不访问`TypeInfo::HasRefField()` | `(sizeClass, scanClass)` Region分类 | 否则Region中可能混有HAS_REF对象 |
| NO_REF leaf不读取`stateWord` | 稳定occupancy协议 | 否则无法区分已分配slot和stale/free slot |
| 删除white-slot合法性检查 | 修复并证明并发Sweep/stale edge不变式，或用occupancy替代 | 先置mark再验证会让其他线程错误跳过扫描 |
| Region级`mark/scanned`队列 | scan bitmap + ownership/dirty/requeue协议 | 扫描期间出现新mark时必须保证Region再次被处理 |
| 按bitmap word批量置位后驱动Region队列 | 从RMW旧值计算winner并更新dirty/queue状态 | 不能只合并mark bit而遗漏新产生的scan work |
| byte/edge based pacer | range、Region scan和普通WorkBuf统一计数 | 否则pacer仍看不见某类在途工作 |
| old/immutable或代际GC | 对old-to-young/跨Region写屏障和card/remembered set | 否则跳过old graph会漏掉新引用 |

##### 可以共存但必须统一调度的组合

| 组合 | 潜在集成问题 | 统一方式 |
|---|---|---|
| RefArray range queue + 普通WorkBuf | 只看WorkBuf为空会提前终止 | completion同时检查range queue、WorkBuf、private work和各类in-flight |
| Range queue + Region scan queue | 两类worker可能互相饥饿，或一类队列为空时错误降worker | 使用统一work epoch和可配置领取顺序；pressure按剩余edges/scan bytes而非queue entry |
| bitmap batching + Region scan queue | batch在Region被worker持有期间设置新mark | batch发布`dirty`；owner release后检测dirty并重新入队 |
| same-Region cache + Sweep/Region复用 | cache可能保存已重置RegionInfo | cache绑定`gcCycle/region generation`，只在本次Mark epoch内使用 |
| prefetch + 最终NO_REF no-body fast path | 继续预取stateWord会浪费带宽 | 按Region flag选择prefetch：NO_REF只预取bitmap，HAS_REF才预取对象头 |
| scanClass分Region + epoch封存 | 两者都会增加active/尾部Region数量 | Region按需创建、NO_REF独立限额、封存只作用于本轮需要稳定occupancy的Region |
| scanClass分Region + 高P | 每P最坏多持有36个额外NO_REF Region | 不预创建72个active Region；按实际size class lazy创建，并允许低频class走共享slow path |

##### 最终推荐架构

最终形态不应是多个互不认识的fast path，而应由一套Region元数据和统一Mark调度承载：

```text
MSRegionInfo v2
  sizeClass
  scanClass: NO_REF / HAS_REF
  allocEpoch / regionGeneration
  scan ownership / dirty / queued state

Bitmap planes
  alloc plane
  mark plane
  scan plane（仅HAS_REF Region实际提交）

Mark work scheduler
  Object WorkBuf       # 普通/large/特殊对象
  RefArray RangeQueue  # 大引用数组
  Region ScanQueue     # 小HAS_REF对象批扫
  Unified in-flight + completion epoch

Worker-local MarkContext
  last Region cache
  bitmap-word batch builder
  TypeInfo/scan descriptor cache
  edge/byte/range统计
```

统一发现接口可以分成三条明确路径：

```text
NO_REF + stable occupancy:
  Region/slot -> occupancy -> mark -> return

HAS_REF small:
  Region/slot -> validate -> mark
  -> Region dirty/queue

large RefArray:
  mark object -> enqueue range tasks
  -> range内部same-Region + bitmap-word batching
```

统一终止条件必须同时满足：

```text
Object WorkBuf为空
RangeQueue为空
RegionScanQueue为空
所有worker local builder/private queue已flush
write-barrier active log已flush
object/range/region/assist in-flight全部为0
dirty Region不存在
```

##### 避免返工的实施顺序

| 阶段 | 实施内容 | 为什么这样排序 |
|---|---|---|
| 0 | 先冻结`MSRegionInfo v2`、bitmap三平面和fast-path ABI版本方案 | 避免先做1024B二分和72项数组后再推翻 |
| 1 | 统一统计与completion框架 | 后续每增加一种task都能被正确计价和终止 |
| 2 | RefArray range + same-Region cache + bitmap batching | 不依赖scanClass，可先取得case26收益 |
| 3 | 引入scanClass Region，旧ABI保守兼容 | 先消除TypeInfo/component访问 |
| 4 | 实施选定的occupancy方案，默认NO_REF epoch封存 | 再安全删除noscan对象头访问 |
| 5 | 增加scan plane和Region scan queue | 复用前面统一completion与Region分类 |
| 6 | byte/edge based pacer和assist | 此时所有真实work类型已经可计量 |
| 7 | old/immutable或代际方案 | 在稳定的Region/write-barrier基础上减少major GC频率 |

结论是：大部分优化在最终架构中可以共存，但不能无序叠加。最需要提前决策的是
bitmap三平面布局、occupancy方案和版本化allocation ABI；最需要统一实现的是三类work queue的
completion协议。

按本轮证据，最值得优先验证的减速链路是：

```text
通用逐边 DiscoverObject 的高固定成本
  + 大 RefArray 缺少 range task
  + WorkBuf debt 看不见真实 edge/in-flight/private work
  -> 并发扩展效率低
  -> debt-stalled 提前 one-way quiesce
  -> 大量 residual closure 转入 STW MarkTermination
```

因此不能只用 `marked objects / ConcurrentMark time` 判断优化效果。后续 A/B 至少同时记录：

```text
visitedEdges、leafWinners、alreadyMarked、CAS losers
RefArray range 数与每个 range 的 scan bytes
GC main/helper/assist 各自 processed edges 和活跃时间
quiesce 时 Global/private/in-flight/residual work
ConcurrentMark wall/CPU 与 MarkTermination wall/CPU
```

### 11.7 对 MSGC 的可落地优化启示

建议不要照搬某个 Go 数据结构，而是迁移它解决问题的原则。

| 优先级 | MSGC 改进 | 本轮证据 | 预期作用 |
|---|---|---|---|
| P0 | 报告拆分 concurrent wall / GC CPU / true STW | Go 6.6s clock sum 实际仅 32.9ms STW | 防止错误归因和错误调参 |
| P0 | 增加 `visitedEdges/scanBytes/leafWinners` | 2GiB payload 实际约 414 万持久 edges | 建立真实 mark work 口径 |
| P1 | RefArray range task，目标粒度约 64～128KiB refs/metadata | Go 128KiB oblet；当前大数组任务不可分 | 提高并行性和可抢占性 |
| P1 | same-Region + bitmap-word batching + prefetch | Green Tea span batching 显著复用元数据 | 降低每 edge 原子/元数据成本 |
| P1 | distributed completion + concurrent residual grace | 16/25 轮 debt-stalled，STW term 共 16s | 把普通闭包留在并发阶段完成 |
| P1 | pacer 改为 scan-work/runway 模型 | WorkBuf debt 不认识 edge 和 in-flight work | 避免过早终止或过晚触发 |
| P2 | GC background utilization 目标 + byte-based assist | Go 按 scan work / alloc byte 调节 | 平衡 GC 进度与业务 CPU |
| P2 | HTTP/JSON/allocator 单独 profile | 归一化后仍有 2.46x 每核差 | 找出非 GC 吞吐瓶颈 |
| P3 | old/immutable region 或代际方案 | 2GiB 长寿命集合每轮重复标记 | 消除 O(long-lived edges) 重扫 |

其中最先应实现的不是“再加 helper”，而是三项配套能力：

```text
RefArray range task
  + edge/scan-byte 进展计数
  + private/global/in-flight 全量 completion protocol
```

只有 range task 没有新 completion 口径，pressure 仍可能把正在处理的大 range 误判成
no-progress；只有 completion protocol 没有 range task，超大引用数组仍会形成单 worker
长尾；只有计数没有前两项，则只能观察问题。

Span/Region batching 在 MSGC 中必须保留当前安全检查：

- 引用必须先确认属于有效 heap/Region；
- white slot、TypeInfo、stateWord/stale edge 检查不能因 batching 被跳过；
- bitmap word 合并后必须根据原子操作返回的 winning bits 精确决定入队对象；
- 并发 Sweep 未完全稳定前，不能假设同 Region 的所有 slot 都可安全解引用。

可先做低风险版本：RefArray 扫描时缓存上一个 `MSRegionInfo*` 和 bitmap base，对连续候选
按 bitmap word 聚合；验证正确后，再评估更完整的 Region queue/marks-vs-scans 设计。

### 11.8 证据边界与后续验证

已确认事实：

- 两侧都配置 2 GiB persistent payload、16 workers、c1000、100K、60s 和 16 GiB heap/limit；
- Go snapshot 精确产生 4,137,440 个 byte backing arrays；
- Go 1.26 本轮启用了 Green Tea GC；
- Go 稳态 STW 中位数 0.290 ms、P95 0.430 ms；
- Go 的 noscan leaf fast path、128 KiB oblet、25% background utilization、
  scan-work assist 和 ragged completion barrier 均来自本机对应版本源码；
- MSGC 高内存后 25 轮中 16 轮是 `debt-stalled`，MarkTermination 总计约 16.0 秒。

尚不能从当前日志单独证明：

1. Go 每轮精确的 `/gc/scan/heap:bytes`。gctrace 给出了 heap start/end/live，但未单列完整
   scan work；94.7 MiB descriptor 和 414 万 edges 是持久基线的静态下界，不含 HTTP 图。
2. MSGC 标记 live 从 2 GiB payload 膨胀到中位 4.73 GiB 的构成。可能同时包含 JSON/HTTP
   对象布局、1000 in-flight graph、长 mark 窗口内的 black allocation/floating garbage；
   需要按类型/分配 epoch 统计才能拆分。
3. Green Tea 单项贡献了多少。当前只有“该实现确实启用”和机制匹配证据，没有同一 Go
   binary 的 Green Tea on/off A/B。
4. Go/MSGC 4.52x 吞吐差中 GC、HTTP、JSON、scheduler、allocator 各自的占比。

建议下一轮验证：

1. Go 侧同时采集 `runtime/metrics` 的 `/gc/scan/heap:bytes`、stack/globals scan bytes、
   GC assist/background CPU；用 `GODEBUG=gctrace=2` 保留 Green Tea size-class/span 统计。
2. 若 Go 1.26 toolchain 支持，使用 `GOEXPERIMENT=nogreenteagc` 重新编译同一 snapshot，
   保持 GOGC/GOMEMLIMIT/CPU 完全一致，做 Green Tea on/off A/B。
3. MSGC 增加每 cycle 的 `visitedEdges`、`leafWinners`、`scanBytes`、最大 RefArray、
   range 数、private/global/in-flight work 和 termination processed/edges。
4. 两侧分别做 CPU profile；先按 GC、allocator、JSON、HTTP、scheduler 五类聚合，
   不要直接把总吞吐差全部记到 GC。
5. 保持 persistent touch on/off、1/2/4 mark worker A/B，用同一对象图验证内存带宽争用和
   range task 的并行扩展。

本轮最终判断：

```text
Go 和 Cangjie 都不扫描 2GiB byte payload，
两边实际都要处理约 414 万条持久引用和约 414 万个 byte backing 叶子 mark；
Go 的优势来自 Green Tea inline/span batching、128KiB oblet 和 scan-work pacer
把相同性质的工作以更低的每-edge/每-object 成本并行完成；
distributed termination 又保证 STW 前闭包基本已经排空。

当前 MSGC 的主要差距不是 GC 算法名称本身，
而是每-edge 常数、超大 RefArray 可分性、work/debt 计价和 termination 收敛协议。
```
