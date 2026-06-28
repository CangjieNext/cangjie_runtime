# MSGC Sweep Backlog HEU Gating 设计说明

## 1. 背景与问题

在 `case27_http_complex_json` 的 `c1000/p100K` 对比中，MSGC 尾部出现了两类明显的长 GC：

1. 中后段个别 GC 因 `EnumRoots`、`ConcurrentMark`、`MarkTermination` 工作量偏大而升高。
2. 最后的两轮 GC 主要被 `PreEnumRoots -> StopAndWaitForAllSweep -> ForceFinishAllUnswept` 拉长，表现为在 GC 开头串行补扫数万到近十万 `unswept region`。

当前实现里，`MSRegionManager::MaybeNotifyMark()` 仅以 `sweptBytes >= heapThreshold` 为启发式触发条件，请求 `GC_REASON_HEU`。这条触发轴没有感知当前 sweep backlog 是否已经明显积压，也没有在 backlog 较大时优先让 background sweep / assist sweep 继续还债。

结果是，当 live bytes 在尾部快速下降、`heapThreshold` 同步变小，而 `unswept region` 尚未被 background 或 assist 消化时，系统会很快再次进入下一轮 HEU GC。下一轮 GC 又会在 `PreEnumRoots()` 中强制补扫旧 backlog，最终把 pause 时间集中暴露出来。

本设计要解决的不是“去掉 GC 开头 force finish”，而是“尽量不让下一轮 GC 开头还有大量旧 backlog 可补”。

## 2. 目标与非目标

本次改动目标如下：

1. 在 `MaybeNotifyMark()` 的 HEU 触发路径上增加 `backlog-aware` 门控。
2. 当 `unswept backlog` 明显高于继续分配的 `runway` 时，优先让 background sweep / assist sweep 提升配速继续还债，而不是立刻发起下一轮 `GC_REASON_HEU`。
3. 只有当 sweep 已经收敛，或 backlog 下降到可接受水位线以下时，才重新允许 `GC_REASON_HEU`。
4. 尽量复用现有口径与字段，包括 `thresholdBytes`、`sweptBytes`、`unsweptRegions`、`sweepDone_`，不新建一套脱节的触发体系。

本次明确不做的事情：

1. 不移除 `PreEnumRoots()` 开头的 `StopAndWaitForAllSweep()` / `ForceFinishAllUnswept()` 正确性兜底。
2. 不改 `OOM`、`GC_REASON_FORCE`、`GC_REASON_HEU_SYNC` 等非普通启发式路径的语义。
3. 不在本轮设计里修改 `ClearAllMarkBitmaps()` 与 `sweepGen` 的基本正确性协议。
4. 不把这次改动扩展为全新的 heap threshold 计算模型。

## 3. 候选方案与取舍

本次考虑三种方案：

### 3.1 方案 A：仅提高 background / assist sweep 默认强度

做法是直接增大 `cjGCBGSweepMaxTime`、`cjMSAssistSweepMaxRegionsPerCall`，让 sweep 更积极。

优点：

1. 改动小，验证快。
2. 在很多中度 backlog 场景下可以直接降低 `ForceFinishAllUnswept` 的概率。

缺点：

1. 没有改变 HEU 触发条件，尾部 `threshold` 快速收缩时，仍可能在 backlog 未清完时再次进入下一轮 GC。
2. 只是“更快还债”，但没有“先还债再开新 GC”的明确策略。

### 3.2 方案 B：在 `MaybeNotifyMark()` 上增加 backlog 感知门控，同时把 sweep pressure 做成分档

做法是在 HEU 触发前同时观察 `unsweptRegions`、`runwayRegions`、`sweepDone`，如果 backlog 明显高于 runway，则暂缓 `GC_REASON_HEU`，转而唤醒更积极的 background sweep，并让 assist sweep 进入更高压力档位。

优点：

1. 直接命中“刚结束一轮 GC 又马上进下一轮”的根因。
2. 可以复用当前 `threshold / sweptBytes` 轴，不需要推翻现有 HEU 逻辑。
3. 能把“是否先还债”从隐式效果变成显式策略。

缺点：

1. 比纯调参多一层状态判定，需要设计好水位线与滞回，避免抖动。
2. 需要同时改 `MaybeNotifyMark()`、`BackgroundProcessor`、assist sweep 路径。

### 3.3 方案 C：彻底改 HEU 触发轴，让其直接基于 retained / active / unswept 的综合指标

优点：

1. 理论上可统一更多 GC 与 sweep 协同语义。

缺点：

1. 牵涉面大，容易影响当前已经稳定的 HEU 触发口径。
2. 本轮问题聚焦在“尾部 backlog 未还完就开启下一轮 GC”，没有必要一次性重写整套 trigger 模型。

推荐方案为方案 B，并保留方案 A 作为参数调优补充。也就是说：

1. 先做 backlog-aware HEU gating。
2. 再把 background / assist 的 pressure 档位显式接入这套 gating。
3. 后续若仍有不足，再决定是否进一步上调默认参数。 

## 4. 核心概念定义

本设计统一引入 `SweepDebtSnapshot`，它不是一套全新的 heap 模型，而是对现有 sweep 债务状态的统一快照。

### 4.1 `runwayRegions`

`runway` 表示“在当前 HEU 触发轴上，还允许继续分配多少 region，才会再次触发下一轮 GC”。

继续复用当前已存在的口径：

1. `thresholdBytes = gcStats.GetThreshold()`
2. `sweptBytes = space.GetSweptHeapBytes()`
3. `runwayBytes = max(thresholdBytes - sweptBytes, 0)`
4. `runwayRegions = ceil(runwayBytes / MS_REGION_SIZE)`

这样做的原因是：

1. 现有 `MaybeNotifyMark()` 就是基于 `sweptBytes >= thresholdBytes` 发 `GC_REASON_HEU`。
2. `PrepareForSweep()`、`AssistSweepForFreshRegions()`、`BackgroundProcessor::HasPendingSweepWork()` 也已经在使用相同轴上的 `remainingRunwayBytes`。
3. 复用同一口径可以避免新旧两套 trigger 语义冲突。

### 4.2 `unsweptRegions`

`unsweptRegions` 直接取 `CentralRegionPool::GetTotalUnsweptCount()`。

它代表当前周期内尚未被 sweep 消化掉的 region backlog，是这次设计中最核心的“债务量”指标。

### 4.3 `sweepDone`

`sweepDone` 直接复用 `CentralRegionPool::IsSweepDone()`。

它代表的不仅是 `sweepDone_ == true`，还要求：

1. background sweep 没有在运行
2. lazy sweep 计数为 0

也就是说，`sweepDone` 表示上一轮 sweep 在全局上已经基本静止，不只是一个单独标志位。

### 4.4 `waterline`

为了避免 `unsweptRegions` 在单一阈值附近反复抖动，采用带滞回的双水位线，而不是单一比较值。

定义三种情况：

1. `NORMAL`
   条件：`sweepDone == true`，或 `unsweptRegions` 没有明显高于 `runwayRegions`
2. `BACKLOG`
   条件：`runwayRegions > 0` 且 `unsweptRegions > backlogEnterRatio * runwayRegions`
3. `EMERGENCY`
   条件：`runwayRegions == 0` 且 `unsweptRegions > emergencyEnterRegions`

退出条件与进入条件分离：

1. `BACKLOG -> NORMAL`
   条件：`unsweptRegions <= backlogExitRatio * runwayRegions` 或 `sweepDone == true`
2. `EMERGENCY -> NORMAL`
   条件：`unsweptRegions <= emergencyExitRegions` 或 `sweepDone == true`

推荐初始参数：

1. `backlogEnterRatio = 2.0`
2. `backlogExitRatio = 1.0`
3. `emergencyEnterRegions = 64`
4. `emergencyExitRegions = 16`

这样可以确保：

1. 像 `cycle 339/340` 这种几万 region backlog 必然被识别为 backlog / emergency。
2. 少量尾巴 backlog 不会无限阻塞正常 HEU 触发。

### 4.5 `pressure mode`

当前 background sweep 只有一个 `bool sweepPressureMode_`，语义是“runway 已耗尽时把单 slice 预算扩大到 10x”。本设计将其扩展为三档压力模式：

1. `SWEEP_PRESSURE_NORMAL`
2. `SWEEP_PRESSURE_BACKLOG`
3. `SWEEP_PRESSURE_EMERGENCY`

建议各档语义：

1. `NORMAL`
   沿用当前默认行为
2. `BACKLOG`
   表示 sweep 债明显高于 runway，但还未到 `runway == 0` 的极端情况
3. `EMERGENCY`
   表示 runway 已经为 0，且 backlog 仍明显存在

推荐各档倍率：

1. background sweep：`NORMAL=1x`，`BACKLOG=4x`，`EMERGENCY=10x`
2. assist sweep 上限：`NORMAL=1x`，`BACKLOG=4x`，`EMERGENCY=10x`

## 5. 设计方案

### 5.1 新增统一债务快照接口

在 `CentralRegionPool` 中新增统一计算函数，输出 `SweepDebtSnapshot`：

1. `unsweptRegions`
2. `runwayRegions`
3. `sweepDone`
4. `mode`，即 `NORMAL / BACKLOG / EMERGENCY`

推荐放置位置：

1. 声明放在 `MSCentralRegionPool.h`
2. 实现放在 `MSCentralRegionPool.cpp`

原因是：

1. `unsweptRegions` 与 `sweepDone` 都属于 central pool 内部状态。
2. `runway` 虽然依赖 `threshold/sweptBytes`，但这些值在 pool 中同样可通过 `RegionSpaceMS` 拿到。
3. 由 pool 统一生成快照，可避免 `MaybeNotifyMark()`、background、assist 各自重复计算并产生分歧。

### 5.2 `MaybeNotifyMark()` 的 backlog-aware HEU gating

在 `MSRegionManager::MaybeNotifyMark()` 中保留现有基础检查：

1. `Heap::GetHeap().IsGcStarted()`
2. `gcInterval cooldown`
3. `pool != nullptr`
4. `sweptBytes >= thresholdBytes`

在这些检查之后新增 backlog gating：

1. 读取 `SweepDebtSnapshot`
2. 如果 `snapshot.mode == NORMAL`，沿用当前逻辑发 `GC_REASON_HEU`
3. 如果 `snapshot.mode != NORMAL` 且 `!snapshot.sweepDone`：
   - 不发 `GC_REASON_HEU`
   - 改为唤醒 `backgroundProcessor.NotifyToSweep(kNoPreferredSweepClass)`
   - 记录一条 “HEU deferred by sweep backlog” 日志

这样做后的语义是：

1. 只有普通启发式 GC 被暂缓
2. sweep backlog 会获得额外时间窗口继续清偿
3. 一旦 backlog 回落，后续 `MaybeNotifyMark()` 会重新允许 HEU 正常进入

### 5.3 background sweep 接入三档 pressure

将 `BackgroundProcessor` 里的 `sweepPressureMode_` 从单布尔升级为压力档位。

具体语义：

1. `HasPendingSweepWork()` 计算 `SweepDebtSnapshot`
2. 当 `snapshot.mode == NORMAL` 时，background sweep 使用默认 slice 时间预算
3. 当 `snapshot.mode == BACKLOG` 时，background sweep 使用中等放大预算
4. 当 `snapshot.mode == EMERGENCY` 时，background sweep 使用最高预算

这样可以把“只在 runway 用尽时才加压”改成“当 backlog 已经明显落后于 runway 时就提前加压”。

### 5.4 assist sweep 接入三档 pressure

当前 assist sweep 有两个关键入口：

1. `AssistSweepForFreshRegions()`
2. `ComputeAssistSweepRegions()`

本设计将 `bool hardPressure` 升级为 `SweepPressureMode`，并按 mode 决定 assist 强度：

1. `NORMAL`
   保持现有上限与比例
2. `BACKLOG`
   把 `normalMaxAssistRegions` 放大为 `4x`
3. `EMERGENCY`
   把 `normalMaxAssistRegions` 放大为 `10x`

保持当前按 `processorNum` 做摊分的基本模型不变，只改变每档的 cap 与 aggressive 程度。

### 5.5 不改变的行为

以下行为本轮保持不变：

1. `PreEnumRoots()` 仍然保留 `StopAndWaitForAllSweep()` 正确性兜底
2. `ForceFinishAllUnswept()` 的执行语义不改
3. `OOM`、`FORCE`、同步 GC 请求不经过这层 backlog gating
4. `PrepareForSweep()` 的 proportional credit 主体模型不改

因此这次设计是“在下一轮 GC 之前优先还债”，而不是“改变 GC 内部正确性协议”。

## 6. 代码改动范围

预计改动集中在以下文件：

1. [src/Heap/MAllocator/MSCentralRegionPool.h](/Users/lipeisheng/code/cangjie/cangjie/cangjie_runtime_MSGC/runtime/src/Heap/MAllocator/MSCentralRegionPool.h)
   - 新增 `SweepPressureMode`
   - 新增 `SweepDebtSnapshot`
   - 新增快照计算接口
2. [src/Heap/MAllocator/MSCentralRegionPool.cpp](/Users/lipeisheng/code/cangjie/cangjie/cangjie_runtime_MSGC/runtime/src/Heap/MAllocator/MSCentralRegionPool.cpp)
   - 实现快照计算
   - 改造 `ComputeAssistSweepRegions()`
   - 改造 `AssistSweepForFreshRegions()`
   - 改造 background sweep slice 时间预算选择
3. [src/Heap/MAllocator/MSRegionManager.cpp](/Users/lipeisheng/code/cangjie/cangjie/cangjie_runtime_MSGC/runtime/src/Heap/MAllocator/MSRegionManager.cpp)
   - 在 `MaybeNotifyMark()` 中接入 backlog-aware HEU gating
4. [src/Heap/MCollector/BackgroundProcessor.h](/Users/lipeisheng/code/cangjie/cangjie/cangjie_runtime_MSGC/runtime/src/Heap/MCollector/BackgroundProcessor.h)
   - 把 `sweepPressureMode_` 从布尔改为枚举或整型档位
5. [src/Heap/MCollector/BackgroundProcessor.cpp](/Users/lipeisheng/code/cangjie/cangjie/cangjie_runtime_MSGC/runtime/src/Heap/MCollector/BackgroundProcessor.cpp)
   - 在 `HasPendingSweepWork()` 里按 `SweepDebtSnapshot` 设置 pressure 档位

如果需要把水位线参数做成运行时可调，才再额外改：

1. [src/CjScheduler.cpp](/Users/lipeisheng/code/cangjie/cangjie/cangjie_runtime_MSGC/runtime/src/CjScheduler.cpp)
2. [src/Cangjie.h](/Users/lipeisheng/code/cangjie/cangjie/cangjie_runtime_MSGC/runtime/src/Cangjie.h)
3. [src/CangjieRuntimeApi.cpp](/Users/lipeisheng/code/cangjie/cangjie/cangjie_runtime_MSGC/runtime/src/CangjieRuntimeApi.cpp)

本轮建议先把初始阈值常量写在 MSGC 内部实现里，待验证有效后再决定是否开放环境变量。

## 7. 日志与观测

为了验证 backlog gating 是否生效，需要新增或增强以下日志：

1. `MaybeNotifyMark()` 在命中 backlog gating 时打印：
   - `sweptBytes`
   - `thresholdBytes`
   - `unsweptRegions`
   - `runwayRegions`
   - `mode`
   - 结论：`defer HEU` 或 `allow HEU`
2. background sweep 在切换 pressure 档位时打印：
   - 当前档位
   - `unsweptRegions`
   - `runwayRegions`
3. assist sweep 在使用 `BACKLOG/EMERGENCY` cap 时打印：
   - 目标档位
   - 本次 cap
   - 原始 cap

推荐日志风格：

1. `[MS-GC] HEU deferred by sweep backlog: swept=%zuB threshold=%zuB unswept=%zu runway=%zu mode=%u`
2. `[MS-GC] Sweep pressure mode: mode=%u unswept=%zu runway=%zu`

最终关注的观测结果有三类：

1. `total gc time` 中 `PreEnumRoots` 尾部尖峰是否下降
2. `ForceFinishAllUnswept` 的 `swept=%zu regions` 是否明显变小
3. 尾部 GC 之间 background / assist sweep 是否真正获得额外执行窗口

## 8. 验证方案

验证按三层推进：

### 8.1 编译与基本正确性

1. 用 `clang16` 编译 MSGC runtime
2. 替换到当前 MSGC SDK
3. 重新编译基准 `.cj` 用例，确认不会引入构建或启动错误

### 8.2 功能验证

1. 跑一轮小负载 case，确认普通场景下 `GC_REASON_HEU` 仍会正常触发
2. 观察 backlog 不大时，`MaybeNotifyMark()` 不会误长期抑制 HEU
3. 验证 `OOM / FORCE / sync` 路径不受影响

### 8.3 性能与长尾验证

重点跑：

1. `case27_http_complex_json`
2. `c1000`
3. `p100K`
4. 当前 MSGC 参数组，例如最近使用的 `heapgrow=3`、`scavengegrow=3`

重点对比项：

1. 尾部最后几轮 GC 的 `PreEnumRoots` 时间
2. `ForceFinishAllUnswept` 强扫 region 数
3. `sweep-counts: bg / lazy / assist`
4. 总吞吐与失败数是否出现明显回退

理想结果是：

1. 尾部 GC 次数可以不变，但 `PreEnumRoots` 尖峰显著下降
2. `ForceFinishAllUnswept` 仍存在，但单次补扫 region 数大幅减少
3. background / assist sweep 在尾部更积极，而不是等下一轮 GC 开头再统一偿债

## 9. 风险与回退

主要风险如下：

1. 如果 waterline 过于激进，可能把普通 HEU 触发压得过晚，造成活跃堆继续膨胀。
2. 如果 `BACKLOG` 压力档过强，mutator assist 可能在高分配路径上引入新的吞吐损失。
3. 如果档位切换没有滞回，可能在 `NORMAL/BACKLOG` 边界反复振荡。

对应缓解方式：

1. 采用进入/退出双阈值，避免抖动。
2. 保留 `EMERGENCY` 只在 `runway == 0` 时触发，避免高压模式滥用。
3. 优先只门控 `GC_REASON_HEU`，不影响其他更强语义的 GC 路径。

回退策略：

1. 如果效果不佳，可先回退到“只增加日志，不真正拦截 HEU”的诊断版本。
2. 若 backlog gating 方向正确但过于保守，可只调整常量而不回退结构。
3. 若实现引入明显回归，可完整回退本轮 gating 改动，恢复到当前 `sweptBytes >= threshold` 的单条件 HEU 触发模型。
