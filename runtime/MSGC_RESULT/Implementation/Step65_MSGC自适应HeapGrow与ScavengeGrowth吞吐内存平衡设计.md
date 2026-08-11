# Step65：MSGC 自适应 HeapGrow / ScavengeGrowth 吞吐—内存平衡设计

## 1. 目标与结论

目标是在高分配 HTTP/JSON 负载中接近高 `cjHeapGrowth` 的吞吐，同时避免把大量已经
回收、但仍保留在运行时中的 Region 长时间留在 RSS 中。

结论是：`cjHeapGrowth` 与 `cjScavengeGrowth` 不应作为一个固定档位一起调大或调小。
前者控制下一轮 Mark 的**分配 runway**，直接影响 GC 频率和吞吐；后者控制已回收
Region 的**物理内存保留水位**，主要影响 RSS。自适应策略应保持两者解耦：

```text
内存有余量        -> 增大 Mark runway，减少 GC 频率
内存接近高水位    -> 先降低 Scavenge 保留水位、后台归还空闲页
Scavenge 已尽力    -> 才逐步收紧 Mark runway
接近软上限        -> 保留现有 OOM Mark 保护路径
```

本文是设计和验证计划，尚未修改 Runtime 行为。

## 2. 现有 case27 证据

对比同一 SDK、`case27`、100 并发、100K payload、60 秒、零失败的两次 MSGC 运行：

| 参数/指标 | `HeapGrow=1, Scavenge=2` | `HeapGrow=10, Scavenge=5` | 变化 |
| --- | ---: | ---: | ---: |
| 成功吞吐 | 1,176 req/s | 2,267 req/s | +92.8% |
| 成功请求 | 70,824 | 136,462 | +65,638 |
| CPU 平均 | 616.07% | 618.57% | 基本相同 |
| CPU / 成功请求 | 约 5.24 ms | 约 2.73 ms | -47.9% |
| GC 次数 | 4,518 | 2,312 | -48.8% |
| GC 总时间 | 54.61 s | 22.41 s | -59.0% |
| GC P95 | 23.23 ms | 13.89 ms | -40.2% |
| RSS 平均 | 238.88 MB | 464.02 MB | +94.3% |
| RSS P95 | 336.46 MB | 470.30 MB | +39.8% |
| RSS 峰值 | 646.27 MB | 470.33 MB | 高增长反而更平稳 |

原始报告：

- `MSGC_RESULT/Result/case27_http_complex_json/result/20260810_c100_100k_HeapGrow1_Scavenge2_compare/compare_report.md`
- `MSGC_RESULT/Result/case27_http_complex_json/result/20260810_c100_100k_HeapGrow10_Scavenge5_compare/compare_report.md`

低增长配置末尾的 GC threshold 约为 33 MB；高增长配置则扩展至约 220--248 MB。
两次 CPU 占用相近，说明高增长并非消耗更多 CPU，而是将近似同样的 CPU 时间用于业务请求，
而非过于频繁地启动 Mark/Sweep。

> GC 总时间为各 GC 周期统计值之和，可能包含并发阶段，不能解释为纯 STW 时间；本节以 GC
> 次数、吞吐和 CPU/request 共同判断调度开销。

## 3. 当前实现和问题

### 3.1 HeapGrow

`MarkSweepCollector::UpdateGCStats()` 当前以本轮 mark bitmap 的 live bytes 计算：

```text
rawGcThreshold = markedLiveBytes × (1 + cjHeapGrowth)
```

现有 `MSHeapThresholdPolicy` 已在实际阈值决策中提供 10% deadband、12.5% 单周期上/下调限幅。
该文件也已有 8 样本 median 的 `EvaluateMSStableLive()` helper，但当前尚未接入
`UpdateGCStats()` 的 threshold base 路径；自适应实现应在接入该 helper 后，以其 stable live
作为下限基准。`cjHeapGrowth` 本身在启动时从环境变量读取一次，运行中不参与反馈控制。

相关位置：

- `src/CjScheduler.cpp`：读取 `cjHeapGrowth` / `cjScavengeGrowth`。
- `src/Heap/MCollector/MarkSweepCollector.cpp::UpdateGCStats()`：计算并发布 threshold。
- `src/Heap/MCollector/MSHeapThresholdPolicy.h::EvaluateMSHeapThreshold()`：deadband 与限幅。

### 3.2 ScavengeGrowth

当前 Scavenge threshold 为：

```text
scavengeThreshold = gcThreshold × max(cjScavengeGrowth, 1)
releaseTarget      = max(scavengeThreshold, currentInUseBytes)
```

后台 Scavenge 只会归还超过 `releaseTarget` 的 empty-swept / hot-free Region。因此较大的
`cjScavengeGrowth` 会保留较大的可复用缓存；它不应被误用为减少 GC 的主要手段。

相关位置：

- `src/Heap/MCollector/MSHeapThresholdPolicy.h::EvaluateMSScavengeThreshold()`。
- `src/Heap/MAllocator/MSRegionSpace.cpp::GetScavengeReleaseTargetBytes()`。
- `src/Heap/MCollector/BackgroundProcessor.cpp::BackgroundScavengeLoop()`。

### 3.3 必须解决的矛盾

若仅把 HeapGrow 降低以控制 RSS，会回到小 runway、高 GC 次数、低吞吐的状态。若仅把
ScavengeGrowth 提高以压低后台 Scavenge，则会将回收后的 Region 长期留在 RSS。

正确次序必须是：**先保留足够的 GC runway，再按内存压力归还可回收 Region。**

## 4. 自适应控制器

### 4.1 状态与观测值

在 `MarkSweepCollector` 增加按 GC 周期更新的 `MSAdaptiveGrowthState`，保存 EWMA 和
滞回状态，不在分配或 Mark 热路径增加原子、锁或 OS 调用。

建议输入：

| 输入 | 来源 | 用途 |
| --- | --- | --- |
| `allocationRateEWMA` | 相邻 GC 完成时刻的已分配/有效 swept 字节 | 推导所需 GC runway |
| `concurrentMarkUs`、周期间隔 | `cycleStats_` | 判断 GC 是否过密 |
| `heapInUseBytes` | `RegionSpaceMS::GetHeapInUseBytes()` | Scavenge 不可低于此值 |
| `heapRetainedBytes` | `RegionSpaceMS::GetHeapRetainedBytes()` | Runtime 自己的内存压力指标 |
| `reclaimableBytes` | empty-swept + hot-free | 区分“可释放缓存”和真实存活压力 |
| `softLimitBytes` | `cjHeapSize` 对应 MSGC soft limit | 运行时内存预算 |

不要在每次 GC 读取 OS RSS：RSS 含代码段、线程栈、socket 和第三方库，且系统调用会污染
GC 路径。需要进程级观测时，以 1 秒级监控作为外部验证或低频保护，不作为每周期主控制量。

### 4.2 HeapGrow 的有效目标

控制器不直接在 `1` 与 `10` 间跳变，而是先计算所需阈值：

```text
desiredRunway = clamp(allocationRateEWMA × targetGcInterval,
                      minRunway,
                      softLimit × runwayCapRatio)

desiredGcThreshold = max(stableLive × (1 + cjHeapGrowth),
                         desiredRunway)
```

其中 `cjHeapGrowth` 是下限而不是每轮固定值：例如 `cjHeapGrowth=1` 时，阈值最低为
`2 × stableLive`。自适应打开后，控制器可在此基础上增大 runway；业务分配速率下降或持续
内存压力出现后，阈值最多回落到该下限。它不会在自适应模式下被写回 RuntimeParam。

建议内部固定常量：

| 项目 | 建议初值 | 说明 |
| --- | ---: | --- |
| `minRunway` | 32 MB | 保证小 live set 下不会被 20 MB floor 锁死 |
| `runwayCapRatio` | 50% | 不允许单个阈值吃掉大部分 soft limit |
| 上调确认 | 连续 3 周期 | 周期过密且内存低于低水位才上调 |
| 下调确认 | 连续 3 周期 | 防止 burst 后马上缩小并反复 grow |
| EWMA 系数 | 1/4 | 兼顾突发响应与防抖 |
| high / critical water | 75% / 85% | 均相对于 `cjHeapSize` |
| OOM cooldown | 4 周期 | 防止 OOM 后立即重新放大 runway |

将 `desiredGcThreshold` 映射为有效 growth，或者直接扩展 `EvaluateMSHeapThreshold()` 接受
额外的 runway floor。仍复用既有 10% deadband 与 12.5% 限幅；OOM 周期继续 bypass，不使用
普通自适应历史。

分配速率的采样不应以 `heapInUseBytes` 的净增量为依据，因为 Region 被复用时该值会低估业务
分配量。推荐在普通 HEU 周期中使用：

```text
mutatorInterval     = thisGcStartTime - previousGcFinishTime
allocationRateSample = previousHeapThreshold / mutatorInterval
allocationRateEWMA   = EWMA(allocationRateSample)
```

上一轮的 HEU threshold 本身就是该轮允许消耗的 allocation runway；采样精度受 Region 粒度影响，
但不要求为分配快路径新增全局计数器或原子操作。

### 4.3 ScavengeGrowth 的边界

本期只实施 HeapGrow 自适应，`cjScavengeGrowth` 保持现有静态语义，不新增
`cjMSScavenge*` 类参数。控制器只读取 retained bytes 作为 HeapGrow 上调的安全门：

```text
retained <= 75% softLimit  -> 允许按分配速率提高 HeapGrow
75% < retained <= 85%      -> 保持 HeapGrow，不继续上调
retained > 85% softLimit   -> 优先唤醒现有 Scavenge；撤销额外 runway 的增长资格
OOM                        -> 保持原有同步 OOM GC，HeapGrow 回到 cjHeapGrowth 下限
```

`heapInUseBytes` 是 Scavenge release target 的硬下界：Scavenge 只能归还 free Region，不能通过
伪造低目标释放仍在使用的 Region。未来若需要让 ScavengeGrowth 也自适应，应单独验证其收益，
不能和本期 HeapGrow 控制器混在同一组参数或同一次性能归因中。

## 5. 推荐状态机

```text
              GC 过密，且 retained < lowWater
NORMAL  --------------------------------------> THROUGHPUT_PRESSURE
  ^                                                   |
  | 连续稳定                                           | 提高 GC runway
  |                                                   v
  |                                              高吞吐运行
  |                                                   |
  | retained > highWater                              | retained > highWater
  +---------------------------------------------- MEMORY_PRESSURE
                                                       |
                                      唤醒既有后台 Scavenge
                                                       |
                         retained > critical 且 reclaimable 很少
                                                       v
                                                RUNWAY_CONSTRAINED
                                      禁止继续上调；OOM 时回到最小增长；保留 OOM 兜底
```

状态转换仅发生在 `UpdateGCStats()`，不在 mutator 分配快路径中判断。

## 6. 配置接口和兼容性

不新增独立的 enable 开关，也不暴露水位、EWMA、runway cap 等调参项。只新增一个参数：

| 参数 | 默认 | 作用 |
| --- | --- | --- |
| `cjMSHeapTargetInterval` | `0` | `0` 关闭自适应；非零开启，并表示期望的相邻 HEU GC 间隔 |

保留已有参数，但自适应开启时的解释如下：

| 参数 | 自适应关闭（`cjMSHeapTargetInterval=0`） | 自适应开启（非零） |
| --- | --- | --- |
| `cjHeapGrowth` | 固定增长系数 | 最小增长系数 / 最小 HeapGrow |
| `cjScavengeGrowth` | 固定 Scavenge 保留倍率 | 现有 Scavenge 初始保留倍率；本期不改为自适应参数 |
| `cjHeapSize` | MSGC soft limit | 内存预算、水位与 OOM 判断依据 |

推荐默认目标间隔为 `30ms`；其含义不是严格定时触发，而是根据 EWMA 分配速率调整下一轮
allocation runway，使普通 HEU 周期趋近该量级。case27 的高吞吐静态配置平均约 26ms 一轮，
因此从 `30ms` 起步比 80--120ms 更能平衡吞吐和 retained memory。

示例：

```bash
export cjHeapGrowth=1
export cjMSHeapTargetInterval=30ms
export cjHeapSize=1024mb
```

这表示 threshold 至少维持 `2 × stableLive`；当业务分配率高时，控制器自动增大 threshold，
分配压力和 retained memory 下降后再逐步回落。若不设置或显式设置
`cjMSHeapTargetInterval=0`，行为与当前版本完全一致。

`cjHeapSize` 继续是 MSGC soft limit，而不是自动控制器的“建议值”。部署者必须设置实际内存
预算；当前 case27 的 `HEAP_SIZE=16gb` 不会对约 500 MB RSS 的进程形成有效约束。该负载建议
先以 `cjHeapSize=768mb` 验证，给 Runtime 堆、网络和短时波动留出空间。

### 6.1 升高、保持与降低

控制器只在 `MarkSweepCollector::UpdateGCStats()` 更新，避免进入分配热路径：

| 观测条件 | 动作 |
| --- | --- |
| 连续 3 个普通 HEU 周期小于 `0.75 × targetInterval`，且 retained `< 75% cjHeapSize` | 增大 runway / HeapGrow |
| 周期接近目标，或 retained 落在 75%--85% 区间 | 保持 |
| 连续 3 个周期大于 `2 × targetInterval`，EWMA 分配率下降 | 按正常限幅降低 HeapGrow |
| retained `> 85% cjHeapSize` | 先收紧 Scavenge target；暂停 HeapGrow 上调 |
| OOM | 清除增长趋势，取消 runway bonus，回落至 `cjHeapGrowth` 下限并冷却 4 周期 |

HeapGrow 降低本身不会直接归还 OS 内存；它只会让下一次 Mark 更早发生。因此内存压力首先由
Scavenge 处理，只有其无法改善且发生 OOM 时，才撤销 HeapGrow 的额外 runway。最终阈值仍经过
现有 10% deadband 和单周期最大 12.5% 变化限幅，避免在升/降方向之间振荡。

### 6.2 GC 日志

保留已有的 `heapThreshold` 与 `heapThreshold stability` 日志，再增加一条结构化日志，便于脚本
直接解析：

```text
[MS-GC] heap-adaptive: cycle=2313 reason=HEU action=RAISE old=220MiB new=247MiB \
live=22MiB minGrowth=1.00 effectiveGrowth=10.23 interval=24.8ms target=30ms \
sampleRate=8.87GiB/s ewmaRate=8.21GiB/s liveTarget=44MiB runwayTarget=246MiB \
retained=470MiB softLimit=1024MiB water=LOW denseStreak=3 reliefStreak=0 cooldown=0
```

OOM 使用同一格式，`action=RESET_TO_MIN`，并打印 `retained`、`softLimit` 与 cooldown。日志只在
每轮 GC 末尾输出；不在每次分配、Sweep slice 或 OS RSS 采样时打印。

## 7. 最小实施顺序

1. 在 `CjScheduler.cpp` 读取 `cjMSHeapTargetInterval`，默认 `0`；只在非零时启用控制器。
2. 新建纯函数 `MSAdaptiveHeapGrowthPolicy`，仅包含 EWMA、滞回和目标阈值决策，并覆盖上升、
   回落、内存压力、OOM reset 的单元测试。
3. 在 `MarkSweepCollector::UpdateGCStats()` 周期末保存 controller state，计算 runway floor，
   记录有效 growth 与结构化日志。
4. 扩展 `EvaluateMSHeapThreshold()` 接受额外 runway floor，继续统一使用既有 deadband/限幅。
5. 保持 `ResolveMSMarkTrigger()` 的 soft-limit OOM 路径；Scavenge 仅作为高水位下的优先释放动作，
   不改变 Region epoch/claim 的并发协议。

## 8. 验证矩阵与通过标准

固定 `case27 c100/100K`，运行 60 秒；每组至少重复 3 次，并保留同一 SDK、同一机器负载和
同一 `cjHeapSize`。

| 组别 | HeapGrow | ScavengeGrowth | 用途 |
| --- | ---: | ---: | --- |
| A | 1 | 2 | 内存优先基线 |
| B | 10 | 5 | 吞吐优先基线 |
| C | 10 | 1.5--2 | 先验证“高 runway + 低保留”是否已足够 |
| D | 自动 | 自动 | 验证 controller |

通过标准：

- D 的吞吐不低于 B 的 90%；
- D 的 RSS 平均/P95 明显低于 B，优先目标为平均低于 350 MB、P95 低于 420 MB；
- D 的 GC 次数不回退到 A 的数量级；
- 无 OOM、无失败请求、无 Region/bitmap verifier 异常；
- 比较 CPU/request、GC P95、RSS P95，而不仅是单次峰值或 GC 总时间。

若 C 已经接近 B 的吞吐并显著降低 RSS，应先采用静态组合，再决定是否引入自适应控制器；
这样可以把“参数解耦的收益”与“控制器本身的收益”分开验证。

## 9. 已实施：基于 `cjHeapSize` 的 retain low/high 水位（2026-08-11）

本轮没有引入第 4--7 节所述的 HeapGrow 自适应控制器，也没有新增环境变量。普通 Mark 的
HeapGrow 阈值仍使用原有 `EvaluateMSHeapThreshold()` 路径；本轮只将后台 Scavenge 从单一
`cjScavengeGrowth` target 改为由 `cjHeapSize` 驱动的 retain low/high 滞回控制。因此用户只需
设置现有的 `cjHeapSize` 作为堆预算。

### 9.1 水位计算

在下一轮 `PreEnumRoots()` 中、`StopAndWaitForAllSweep()` **之前**记录
`preSweepInUseBytes`。这是上一 GC 完成后到本轮 GC 之间真实占用过的 Region 容量；即使本轮
强制 sweep 随后把其中一部分 Region 变为 free，也仍以它作为可复用 Region cache 的下界，避免
下一轮立即重新提交同样的 Region。

设原有 HeapGrow 算得的 Mark 阈值为 `T`，soft heap budget 为 `H`，一个 Segment 大小为 `S`：

```text
emergencyReserve = max(2 × S, H / 16)
retainBudget     = H - emergencyReserve
retainLow        = max(preSweepInUseBytes, min(T, retainBudget))
releaseBand      = max(S, T)
desiredHigh      = max(retainLow, min(retainBudget, retainLow + releaseBand))
```

如果活跃使用量已经高于 `retainBudget`，low/high 保持在实际 active footprint，Scavenge 不会错误地
尝试归还仍在用的 Region；原有 `cjHeapSize` OOM Mark 通知仍负责真正的软上限压力。

`retainLow` 每轮仍直接发布为上述 `preSweepInUseBytes` 驱动的值，不做平滑；这样低水位的语义
仍是“当前上一分配周期实际用过的容量”。`retainHigh` 则是可复用 cache 的触发门，不能随高频 GC
采样即时下降。设上一轮已发布 high 为 `oldHigh`：

```text
desiredHigh >= oldHigh  -> high = desiredHigh              # 立即上调
desiredHigh <  oldHigh，距离上次 high 变化不足 1 秒
                         -> high = oldHigh                 # 保持
否则                    -> high = max(desiredHigh,
                                        oldHigh - 1 × S)     # 每秒最多下降一个 Segment
```

实现只在 `MarkSweepCollector` 保留一个内部时间戳 `lastScavengeHighDecayUs_`；没有增加用户参数。
因此短暂 burst 后 Region cache 不会因为一次较小的 `preSweepInUseBytes` 采样而立刻被 Scavenge
归还，但持续低负载仍会逐步降低 RSS。

### 9.2 Scavenge 行为与年龄优先级

- `retained <= retainHigh`：不唤醒 Scavenge；
- `retained > retainHigh`：开始回收，目标为 `retainLow`；真正计算释放量时使用
  `max(retainLow, currentInUse)`，确保并发分配期间不会把 release target 压到 active usage 以下；
- hot-free Region 按 sweep epoch 年龄桶跨 shard 优先：`age >= 4`、`>= 2`、`>= 1`。这是无候选数组、
  无排序的实现，较旧 Region 会先消耗 release budget；当老桶不足时才回退到较新的已跨 epoch
  Region。刚在当前 epoch 变 free 的 Region 不参与本次回收。

`cjScavengeGrowth` 仍可被旧脚本传入以保持兼容，但不再参与 MSGC 的 Scavenge 水位计算。

### 9.3 代码位置与日志

- `src/Heap/MCollector/MarkSweepCollector.cpp`：在强制 sweep 前记录 `preSweepInUseBytes`，并在
  `UpdateGCStats()` 发布 low/high；
- `src/Heap/Collector/GcStats.h`：增加 `scavengeTriggerThreshold`（high watermark）；
- `src/Heap/MAllocator/MSRegionManager.cpp`：仅在 retained 超过 high 时通知后台 Scavenge；
- `src/Heap/MCollector/MSRegionSpace.cpp`：回收目标由 high 触发、向 low 释放；
- `src/Heap/MAllocator/MSFreeManager.cpp`：按 age bucket 选择 hot-free Region。

每轮 Mark 末尾输出：

```text
[MS-GC] scavengeWatermark: oldLow=... oldHigh=... low=... desiredHigh=... high=... \
  heapGoal=... preSweepInUse=... retainBudget=... band=... \
  highDecayElapsedUs=... highDecayBytes=... highRaised=... highHeld=... highDecayed=...
```

### 9.4 验证

已使用 `build/scripts/rebuild_msgc_runtime_output.py` 重建并安装 Region-scan MSGC Runtime，并以
显式 `--gc-mode=marksweep` 运行 case27 短验证：`HEAP_SIZE=512mb`、`c100`、`100K`、`10s`。
结果目录：

`MSGC_RESULT/Result/case27_http_complex_json/result/20260811_c100_100k_HeapGrow1_Scavenge5_compare`

MSGC 运行成功（10,650 成功请求、0 失败），GC 日志实际打印 low/high。例如一轮为：

```text
low=101482496B high=168591360B heapGoal=44129772B \
preSweepInUse=101482496B retainBudget=402653184B band=67108864B
```

这证明 low 取到了强制 sweep 前的 in-use footprint，而不是 Mark 后的纯 live bytes；短验证仅确认
功能与安装链路，不将该单次吞吐用作性能结论。

### 9.5 High 迟滞验证（2026-08-11）

针对高 HeapGrow case27 的 RSS 波动，已将 high reserve 从原来的最多 `T / 4`（该负载实际常被
64 MiB Segment 下限限制）改为完整 `T`。纯策略测试覆盖：high 上升立即生效；当 desired high
下降时，999,999 us 内保持不变；满 1 秒后恰好下降一个 Segment。

使用新安装 Runtime 的短验证：`HEAP_SIZE=16gb`、`c100`、`100K`、`10s`、`HeapGrow=10`，结果目录：

`MSGC_RESULT/Result/case27_http_complex_json/result/20260811_c100_100k_HeapGrow10_Scavenge5_compare_2`

MSGC 成功 21,609、失败 0、吞吐 2,135 req/s。369 个 GC 周期中 high 立即上调 31 次、保持 331 次、
按时间下降 6 次；后台 Scavenge 仅 3 次实际释放、约 0.2 GiB。日志中 steady-state 例子为
`low=247 MiB`、`desiredHigh=451 MiB`、已发布 `high=468 MiB`、`highHeld=1`，表明 low 仍可下降，
但 high 不会随单轮采样立即下跳。
