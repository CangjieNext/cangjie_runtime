# Step55：MSGC `ComputeMarkedStats` 作用、开销与优化分析

## 1. 结论

1. `ComputeMarkedStats()` 不是为了打印 `Mark bitmap` 日志，而是生成两个会影响后续 GC
   行为的周期级权威值：
   - `liveBytes`：作为 GC threshold 的真实存活基线，并用于 GC 结束时的利用率统计；
   - `objectCount`：作为下一轮 mutator mark-assist 配速的输入。
2. 当前实现用一次串行 bitmap popcount 替代了“每成功标记一个对象就更新一个全局原子”的旧实现。
   这是有意的折中：把 mark 热路径上的共享 cache-line 争用，换成 mark 完成后的一次顺序扫描。
3. 它的成本不直接等于 `O(maxHeapBytes)`，也不扫描未提交或空闲的最大堆。small object 部分的成本
   取决于当前被 CentralPool 或 `AllocBufferMS` 持有的 small Region 数及 size-class 分布；large
   object 部分取决于存活 large object 条目数，而不是 large span 的总字节数。
4. case27 P128 中，单轮 preGC Central small Region 最多约 25 万个，对应 mark bitmap 约
   26.29 MiB；约 3,470 个 active Region 额外需要 0.03～1.69 MiB bitmap。这个量级可以产生
   数毫秒到数十毫秒的串行指针追踪与 popcount，但不能单独解释日志中 0.4～0.84 秒的异常窗口。
5. 当前日志只能得到 `CollectLargeGarbage + ComputeMarkedStats` 的合并上界：23 轮累计
   3,569.946 ms，其中 OOM 轮 2,303.205 ms。日志没有给两个函数分别计时，不能把这
   3.57 秒直接归因于 `ComputeMarkedStats()`。
6. 第一优先级应是增加无行为变化的子阶段计时和扫描量统计。若确认 bitmap pass 是热点，推荐
   用 GC-worker/P-local 的 mark-winner 计数替代全量扫描，但必须先与 bitmap 权威值双算对拍。
   不能恢复逐对象全局原子，也不建议第一步就与异步 Sweep 融合。

## 2. 为什么必须做这项统计

### 2.1 `liveBytes`：决定下一轮 GC threshold 的真实存活基线

`PostMarkTermination()` 把 `markedStats.liveBytes` 写入：

```cpp
cycleStats_.trueLiveBytes = markedStats.liveBytes;
```

随后 `UpdateGCStats()` 把它作为 mark-bitmap live 输入
`EvaluateMSStableLive()`。正常样本中：

```text
thresholdBase = markBitmapLiveBytes
nextStableLive = markBitmapLiveBytes
```

之后按照 `heapGrowth` 计算 raw target，并应用最小阈值、最大堆 cap、上涨限速和 25% live
headroom。也就是说，少算 live 会让 threshold 偏低、增加 GC 频率甚至形成 GC thrash；
多算 live 会让 threshold 偏高、推迟 GC，并增加顶到 max heap/OOM 的风险。

当并发 Mark 窗口中堆增长过快时，`EvaluateMSStableLive()` 会识别 floating-live inflation：

```text
markEndInUse >= 4 * markStartInUse
并且 bitmapLive > 2 * markStartInUse
```

此时本轮 bitmap live 仍会作为 `lastTrueLiveBytes` 保存，但 threshold 基线改用
`max(lastStableLive, markStartInUse)`，避免一个膨胀样本直接污染稳定基线。该判定只使用
相对增长关系，不再要求 `inUse` 增量达到 `maxHeap / 4`。

当前源码中，Scavenge threshold 的主要输入是最近 in-use/burst/cold-reuse 历史，并不直接以
`liveBytes` 作为函数参数；因此应准确表述为：`liveBytes` 直接驱动 heap threshold 和利用率，
而不是当前 Scavenge threshold 的直接计算项。

### 2.2 `objectCount`：决定下一轮 mutator mark-assist 的初始配速

`PostMarkTermination()` 还会保存：

```cpp
prevCycleMarkedObjects_.store(markedStats.objectCount);
```

下一轮 `ConcurrentMark()` 用它计算基础 assist ratio：

```text
rawRatio =
    prevMarkedObjects
    * kMSAssistRatioScale
    * baseRatioPercent
    / (allocatedBytes * 100)
```

随后还会经过 cycle-to-cycle rise limit 和上一轮 assist share feedback，再写入
`assistWorkPerByteScaled_`。这个值表示 mutator 每分配一定字节应承担多少标记工作。

因此 `objectCount` 影响的是下一轮 assist pacing，不是当前版本中 pressure-based Mark Helper
线程数的直接输入。低估会使 assist 偏弱、标记 backlog 更容易堆积；高估会把过多标记工作压给
mutator，损害业务吞吐和尾延迟。

### 2.3 为什么不继续在每个 mark winner 上更新全局原子

旧方案在每次 white→grey 成功后更新共享 `markedObjectCount`。多 GC worker、mutator assist
和 barrier 路径会反复写同一个原子 cache line，P 数和标记对象数上升后产生严重的
cache-line bouncing。

Step41 已将这项逐对象热原子移除，改为 bitmap 周期末统计。历史 case27 数据中，移除热原子后
ConcurrentMark 平均耗时和吞吐均有大幅改善。该历史结果证明的是“不能恢复全局逐对象原子”，
不等于当前 P128 下 bitmap pass 已经没有优化空间。

## 3. 当前实现与计算公式

MSGC 的 small Region 固定为 32 KiB，共 36 个 size class，slot size 从 8 B 到 16 KiB。
每个 small Region 的 sidecar 中连续保存：

```text
[ allocBitmap | markBitmap ]
```

其中：

```text
totalSlotCount = floor(32768 / slotSize)
bitmapWordCount = ceil(totalSlotCount / 64)
```

`ComputeMarkedStats()` 当前串行执行三个步骤。

### 3.1 CentralPool small Region

对每个 size class，遍历：

```text
partialSwept
fullSwept
partialUnswept
fullUnswept
```

每个 Region 执行：

```text
markedSlots = sum(popcount(markBitmap[word]))
objectCount += markedSlots
liveBytes += markedSlots * slotSize
```

它扫描整张 mark bitmap，与 bitmap 中实际置位多少无关。空 bitmap 和全满 bitmap 的读取字节数
相同；存活对象数只轻微影响 popcount 结果，不决定循环次数。

### 3.2 所有 `AllocBufferMS` 的 active Region

函数持有 `msAllocBufferLock_`，遍历已注册 buffer，并检查每个 buffer 的 36 个 active Region
槽。非空 Region 使用与 CentralPool 相同的 popcount 公式。

P=128 不表示固定扫描 128×36 个 Region。实际 registry 中还可能有额外 buffer，本轮日志为
130 个 buffer；真正有 active Region 的数量约 3,300～3,470 个。循环槽位上限是
`bufferCount * 36`，bitmap 扫描量取决于非空槽。

### 3.3 live large Region

`CollectLargeGarbage()` 已先删除未标记 large Region。`ComputeMarkedStats()` 随后在
`largeLock_` 下遍历剩余 large list：

```text
每个 marked large primary:
    objectCount += 1
    liveBytes += LargeSpanBytes()
```

一个跨 1,000 个 Region 的单个 large object，在这里仍只访问一个 primary 条目；因此 large
统计的循环次数与 large object 个数相关，而不是与 span 字节数线性相关。

### 3.4 返回值

`MSMarkedStats` 只有两个 `size_t`：

```cpp
struct MSMarkedStats {
    size_t objectCount;
    size_t liveBytes;
};
```

函数自身不分配大块内存，主要成本是 Region/list 元数据访问、bitmap 读取、popcount 和三类锁/
并发访问带来的等待。

## 4. 与堆大小、对象数和 P 的关系

### 4.1 更准确的复杂度

令：

- `Rc`：CentralPool small Region 集合；
- `Ra`：所有 active small Region 集合；
- `W(r)`：Region `r` 的 mark bitmap word 数；
- `L`：回收 large garbage 后的 live large primary 数；
- `B`：已注册 `AllocBufferMS` 数。

则主要成本为：

```text
T ≈ O(
    Σ(r ∈ Rc) W(r)
  + Σ(r ∈ Ra) W(r)
  + 36 * B
  + L
)
```

它不是 `O(maxHeapBytes)`，也不是 `O(liveObjectCount)`。更接近“当前 small Region footprint
按 size class 加权后的 bitmap 大小 + Region/list 条目数 + live large object 条目数”。

### 4.2 每个 32 KiB Region 实际扫描多少 mark bitmap

典型 size class：

| slot size | slots/Region | bitmap words | mark bitmap 字节 |
| ---: | ---: | ---: | ---: |
| 8 B | 4,096 | 64 | 512 B |
| 16 B | 2,048 | 32 | 256 B |
| 24 B | 1,365 | 22 | 176 B |
| 32 B | 1,024 | 16 | 128 B |
| 64 B | 512 | 8 | 64 B |
| 160 B | 204 | 4 | 32 B |
| 512 B | 64 | 1 | 8 B |
| 16 KiB | 2 | 1 | 8 B |

所以 small Region 占用相同的 32 KiB payload 时，8 B class 的 bitmap 扫描量是 512 B class
及以上的 64 倍。堆内 size-class 分布比单纯“用了多少 GiB”更能决定这个函数的成本。

### 4.3 与配置最大堆的关系

- 只把 `cjHeapSize` 从 8 GiB 改为 16 GiB，但工作集仍只使用 2 GiB，不会让本函数扫描完整
  16 GiB。
- 若工作负载确实把更多 small Region 放入 Central/active 集合，成本会随这些 Region 增长。
- 未提交、FreeManager free、empty cache 以及 hot-free Region 不在当前遍历的 small list 中，
  不会按最大堆容量全部进入 popcount。
- large object 的 span 会增加 liveBytes，但单个 span 再大也只贡献一个 large list 条目。

因此“跟堆大小有关”的准确答案是：与当前活跃/中央 small Region footprint 有近似线性关系，
与配置 max heap 只有间接关系；large heap 字节数与 large 统计成本并不线性。

### 4.4 与 P 的关系

P 增加会间接增加：

- `AllocBufferMS` 数；
- 每个 buffer 可能持有的 active size-class Region；
- mutator 与 registry/large/list 锁的竞争；
- PostMark 与业务线程的 CPU、cache 和内存带宽竞争。

但 P 不会让已经在 CentralPool 中的每个 Region 被重复扫描。P=128 的直接增量主要是
`36 * bufferCount` 个指针槽检查和几千个 active Region bitmap。

## 5. case27 P128 的实际数据

数据来自：

```text
MSGC_RESULT/Result/case27_http_complex_json_msgc_high_p/result/
20260726_110103_p128_gc_analysis_p128_c1000_100K_60000ms/msgc/gc.log
```

按照每轮 preGC 的 36 class Region 数和当前 `kSizeClasses` 反算 Central mark bitmap：

| cycle | reason | Central small Region | active Region | preGC large object | Central mark bitmap | PostMark→Mark bitmap |
| ---: | --- | ---: | ---: | ---: | ---: | ---: |
| 7 | heuristic | 5,484 | 2,572 | 2,093 | 0.57 MiB | 55.379 ms |
| 8 | heuristic | 90,595 | 3,336 | 23,233 | 9.61 MiB | 288.249 ms |
| 9 | heuristic | 129,722 | 3,349 | 28,993 | 13.65 MiB | 762.929 ms |
| 10 | heuristic | 193,268 | 3,377 | 45,146 | 20.38 MiB | 158.120 ms |
| 11 | OOM | 250,255 | 3,316 | 17,602 | 26.29 MiB | 83.225 ms |
| 12 | OOM | 164,808 | 3,322 | 38,813 | 17.27 MiB | 841.775 ms |
| 13 | OOM | 238,317 | 3,330 | 52,467 | 24.86 MiB | 45.247 ms |
| 18 | OOM | 170,889 | 3,421 | 36,498 | 17.74 MiB | 570.472 ms |
| 20 | OOM | 242,342 | 3,372 | 51,991 | 25.46 MiB | 40.995 ms |
| 23 | heuristic | 10,889 | 3,470 | 18 | 1.23 MiB | 1.217 ms |

active Region 没有按 class 逐项打印，因此只能给范围。3,470 个 active Region 的 mark bitmap
理论范围是：

```text
全为 >=512 B class: 3470 * 8 B   = 0.026 MiB
全为 8 B class:     3470 * 512 B = 1.694 MiB
```

由此可见，在最大轮次中 Central bitmap 是主要 small bitmap 字节量，P-local active bitmap
不是数量级主因。不过 25 万个 Region 意味着 25 万次 RegionInfo/list/sidecar 指针访问，
真实成本不等于把 26 MiB 连续数组做一次理想带宽 memcpy。

还有两个重要现象：

1. 26.29 MiB 的 cycle 11 合并窗口是 83.225 ms，而 17.27 MiB 的 cycle 12 是
   841.775 ms；
2. 24.86 MiB 的 cycle 13 只有 45.247 ms。

因此该窗口的尖峰与 bitmap 字节数不单调，明显还包含 large reclaim、锁等待、调度以及与 P128
mutator 的资源竞争。仅根据窗口长短不能推断 popcount 本身的速度。

## 6. 为什么当前日志不能直接证明它耗时 3.57 秒

当前顺序是：

```text
PostMarkTermination start
  -> ProcessFinalizers
  -> CollectLargeGarbage
       -> ReclaimLargeHotCache(previous cycle)
       -> 扫 large list，移除 dead primary
       -> 对每个 dead large object 清理所有 secondary span RegionInfo
       -> 放入 large hot cache / reclaim
  -> ComputeMarkedStats
  -> "Mark bitmap" 日志
  -> OOM emergency parallel small sweep
  -> OOM cache flush
```

本轮 23 个 cycle：

| 可观测窗口 | 累计 |
| --- | ---: |
| `PostMark start → Mark bitmap` | 3,569.946 ms |
| 其中 12 个 OOM cycle | 2,303.205 ms |
| 其中 11 个非 OOM cycle | 1,266.741 ms |
| Finalizer notify | 约 0.084 ms |

Finalizer 很小，所以 3.57 秒几乎都在 `CollectLargeGarbage + ComputeMarkedStats`，但两者没有
独立 timer。`Mark bitmap` 之后的 OOM emergency Sweep 不在 3.57 秒内；它另有约
27.312 秒 operational window，是 31.127 秒 PostMark 的主要来源。

尤其 `CollectLargeGarbage()` 的成本不能只按 large object 数判断。它还会：

- 释放上一轮 large hot cache；
- 为 dead large object 构造临时 vector；
- 对每个 large span 的所有 secondary `MSRegionInfo` 执行 `Reset()`；
- 与并发 large allocation 的注册/缓存路径竞争锁。

所以当前证据支持：

```text
ComputeMarkedStats 单轮耗时 <= PostMark→Mark bitmap 合并窗口
```

但不支持：

```text
ComputeMarkedStats 单轮耗时 == 该合并窗口
```

要回答它本身的绝对耗时和 popcount 带宽，必须新增独立计时。

## 7. 当前实现的并发边界

源码把 `PostMarkTermination()` 标记为 non-STW/concurrent-safe，但当前实现中存在需要先厘清的
snapshot 边界：

1. `ComputeMarkedStats()` 的注释称“markBitmaps stable 且 small sweep 尚未开始”；
2. OOM policy 的确在 MarkTermination 后继续关闭 lazy/assist/helper sweep，直到 emergency
   Sweep 和 cache flush 完成；
3. 普通 GC policy 却在 `MarkTermination()` 内、mutator 恢复前调用
   `ReenableLazySweep()`，后台 Sweep 虽要等稍后的 notify，但 mutator lazy/assist 路径已重新开放；
4. `partialSwept_` 使用 `VisitPartialSweptUnsafe()`，而分配侧可用 lock-free `Pop()` 取走节点；
5. registry lock 只保护 `msAllocBuffers_` 容器成员关系，`activeRegions_[cls]` 本身由 mutator
   普通读写，统计侧并没有冻结 active Region ownership。

这至少表明“注释声明的稳定窗口”与 normal-GC policy 的实际发布时序不完全一致。任何并行化、
拆锁或 snapshot 优化前，都应先用 contract/test 明确以下不变量：

- 统计期间是否允许 Central Region 在 list 与 active buffer 间移动；
- 统计到的是 mark-termination 瞬间存活集，还是 PostMark 并发期间的近似快照；
- 新分配对象的 mark bit 是否应计入本轮 true live；
- 一个移动中的 Region 如何保证既不重算也不漏算。

否则即使性能变快，也可能让 `liveBytes` 和 `objectCount` 变成非确定性样本。

## 8. 优化方案与优先级

### P0：先拆分计时和工作量，零算法风险

建议给 PostMark 增加：

```text
largeSweepUs
markedStatsUs
markedStatsCentralRegions
markedStatsActiveRegions
markedStatsLargeObjects
markedStatsBitmapWords
```

必要时再测：

```text
largeLock wait/hold
msAllocBufferLock wait/hold
各 size-class bucket lock hold
```

REPORT 每轮只打印一行汇总，不打印每个 Region。验收时至少得到：

```text
serial bitmap throughput = bitmapWords * 8 / markedStatsUs
ns / visited small Region
largeSweepUs 与 dead large object/span 数的关系
```

这是当前最重要的动作，因为 P128 日志里的 0.04～0.84 秒合并窗口并不随 bitmap MiB 单调变化。
没有独立数据就直接改算法，很可能优化错函数。

### P1：先固定统计 snapshot 契约

在 normal GC 上，推荐二选一：

1. 把 lazy/assist/helper sweep admission 的重新开放延后到 `ComputeMarkedStats()` 完成；
2. 在 MarkTermination STW 内冻结一份只含 Region 指针/size class 的 epoch snapshot，PostMark
   只读 snapshot，完成后再释放。

第一种改动小，但会把 normal GC 的 lazy sweep 启动延后到统计结束；第二种并发性好，但要承担
snapshot 构造、容量复用和 Region 生命周期证明。不要把完整 popcount 移入 STW，否则会直接拉长
暂停。

### P2：若确认 bitmap pass 是热点，首选 mark-winner local stats

每个对象只有一个执行方能成功完成 white→grey bitmap CAS。winner 已知道对象所在 Region，
因此可以在执行方本地累加：

```text
localObjectCount += 1
localLiveBytes += slotSize / LargeSpanBytes
```

周期结束后低频归并。需要覆盖：

- GC main；
- Mark Helper；
- mutator assist；
- barrier P-local；
- root enumeration；
- finalizer resurrection；
- large object；
- mutator 在 Mark 期间退出或 buffer 生命周期变化。

这能彻底删除全 bitmap pass，但必须遵守：

- worker-local/P-local 普通字段热更新，不能每对象写共享原子；
- 生命周期边界只做低频汇总；
- 第一阶段仍保留 `ComputeMarkedStats()` 作为 authority；
- 每轮比较 local 与 bitmap 的 objectCount/liveBytes，要求完全相等；
- 覆盖 OOM、normal、finalizer、weak ref、large object、mutator exit 后才能切换 authority。

这是收益上限最高的长期方案，也是协议扩散面最大的方案。Step41 已证明恢复单个
`globalMarked.fetch_add()` 是不可接受的回退。

### P3：较低复杂度备选——冻结后并行 bitmap reduction

若独立计时证明统计占比高、但短期无法覆盖所有 mark-winner 上下文，可以：

1. 先取得稳定 Region snapshot；
2. 按 size class 或 bitmap-word 工作量切分给 GC worker；
3. 每个 worker 写自己的 `MSMarkedStats`；
4. join 后由 GC main 做一次归并。

优点是统计语义不变，仍以 bitmap 为 authority。限制是：

- 只降低 wall time，不减少总 bitmap 读取和内存带宽；
- case27 当前 GC 配置为 main + 3 helper，收益不会随 P=128 线性扩展；
- 26 MiB 已可能是内存延迟/Region pointer chasing 主导，过多 worker 反而争带宽；
- 小堆轮次只有几十 KiB～1 MiB bitmap，task setup 会得不偿失。

因此必须按 `bitmapWords` 或 Region 数设置并行门槛，并保留 serial fast path。

### P4：优化当前串行 pass，但预期收益有限

可实验的局部优化：

- 将 Central/active 的重复 popcount 逻辑提取为 inline helper，确保编译器稳定生成 popcount；
- 对下一 Region 的 sidecar 做适度 prefetch；
- 对 `MS_REGION_BUMP` active Region 只扫描 `allocPtr` 已覆盖的 slot 前缀；
- 按 bitmap word 数而不是 Region 数分块，改善 size-class 负载均衡；
- 若后续 Sweep 会再次 popcount，可在 side table 缓存 per-Region marked count，让 Sweep 复用，
  从而减少整个 GC 周期的重复 bitmap 读取。

这些方案都需 microbenchmark 验证。AArch64 的 `cnt/addv` 向量实现未必比编译器生成的标量
popcount 更快；prefetch 也可能在 P128 内存压力下造成额外污染，不能仅凭代码形态决定。

### 不推荐：直接与 Sweep 融合

Sweep 本来会再读 mark bitmap，理论上可以一边 Sweep 一边汇总。但是：

- normal Sweep 是异步的，可能在 PostMark 很久以后才完成；
- `UpdateGCStats()` 需要本轮结束时及时发布 threshold；
- active/current-generation Region 不一定参与这一轮 Sweep；
- 延迟统计会改变下一轮 HEU/GC 触发时序；
- OOM 与 normal 两条路径的 Sweep 发布策略不同。

所以这不是局部性能 patch，而是 GC pacing publication 的重设计。优先级低于 local stats 或
稳定 snapshot 后的并行 reduction。

### 不推荐：近似 liveBytes 或按 Region 利用率估算

`objectCount` 只服务 assist pacing，理论上可容忍一定偏差；但 `liveBytes` 是 heap threshold 的
关键输入。为了省一次扫描而用 active bytes、alloc bytes 或抽样估算，会把误差传导到 GC 频率和
OOM headroom。除非同时重新设计误差边界和安全 cap，否则不应采用。

## 9. 推荐实施顺序和验收标准

### Phase A：只增加观测

1. 分别计时 `CollectLargeGarbage` 和 `ComputeMarkedStats`；
2. 记录 Central/active/large 条目数与 bitmap words；
3. 在相同 case27 P128 参数下至少重复 3 次；
4. 同时采样 CPU profile，确认时间是执行 popcount、指针 cache miss，还是等待锁。

进入下一阶段的门槛建议为：

```text
markedStatsUs / total GC time >= 3%
或 P95 markedStatsUs 对请求尾延迟有清晰贡献
```

如果实际占比远低于 OOM Sweep、MarkTermination 和 EnumRoots，停止优化该函数。

### Phase B：修正/固化 snapshot 契约

新增 contract 和并发压力测试，证明：

- normal/OOM 两条路径都不会漏算或重算 Region；
- 统计期间 Region ownership 转移的规则明确；
- 多轮结果稳定，ThreadSanitizer 不报告 active Region/list 数据竞争；
- 不把 bitmap scan 搬入 STW。

### Phase C：选择实现

- 需要快速降低 wall time：stable snapshot + threshold-gated parallel reduction；
- 需要删除重复 pass、追求长期吞吐：mark-winner worker/P-local stats。

local-stats 切换必须经过双算：

```text
每轮 localObjectCount == bitmapObjectCount
每轮 localLiveBytes == bitmapLiveBytes
连续 case27 short/60s、OOM、large/finalizer 专项均 0 mismatch
```

### Phase D：性能验收

至少比较：

- `markedStatsUs` avg/P95/max/total；
- `CollectLargeGarbageUs`，防止把 large 成本误报成统计收益；
- PostMark、GC total、STW；
- mutator throughput、P95/P99 请求延迟；
- CPU user/system、RSS peak；
- next-cycle assist ratio 和 GC threshold 是否逐轮一致；
- OOM 次数、Sweep backlog、GC 次数是否发生异常变化。

性能目标不能只写“函数变快”，还应要求业务指标不回退、GC pacing 结果不失真。

## 10. 源码索引

| 主题 | 源码 |
| --- | --- |
| PostMark 调用和结果发布 | `src/Heap/MCollector/MarkSweepCollector.cpp:1852` |
| `ComputeMarkedStats` 实现 | `src/Heap/MAllocator/MSCentralRegionPool.cpp:1351` |
| 返回统计结构 | `src/Heap/MAllocator/MSMarkedStats.h:12` |
| 32 KiB Region 和 36 size class | `src/Heap/MAllocator/MSSizeClass.h:37` |
| bitmap sidecar 初始化 | `src/Heap/MAllocator/MSRegionInfo.cpp:19` |
| objectCount 的 assist consumer | `src/Heap/MCollector/MarkSweepCollector.cpp:791` |
| assist ratio 公式 | `src/Heap/MCollector/MSAssistPacing.h:81` |
| liveBytes 的 threshold consumer | `src/Heap/MCollector/MarkSweepCollector.cpp:2368` |
| stable-live/floating-live 策略 | `src/Heap/MCollector/MSHeapThresholdPolicy.h:79` |
| normal/OOM PostMark Sweep policy | `src/Heap/MAllocator/MSOOMEmergencyPolicy.h:18` |
| large eager reclaim | `src/Heap/MAllocator/MSCentralRegionPool.cpp:1290` |

补充区分：`ClearAllMarkBitmaps()` 在下一轮 PreEnum STW 中遍历 `GetMaxRegionCount()` 个
`MSRegionInfo`，它更接近与最大堆 Region 容量相关的操作；本报告分析的
`ComputeMarkedStats()` 则只遍历 Central/active small Region 和 live large list。二者不能因为
都操作 mark bitmap 而混为同一个复杂度。
