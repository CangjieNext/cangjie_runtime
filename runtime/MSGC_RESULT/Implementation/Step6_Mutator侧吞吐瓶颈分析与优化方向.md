# Step 6：Mutator 侧吞吐瓶颈分析与优化方向

> 对应分析产物：[case1_msgc_slowdown_analysis.md](../Result/case1_baseline/analysis/20260422_msgc_profile/case1_msgc_slowdown_analysis.md)
>
> 2026-04-22 基于 case1_baseline 的 profile、对比报告与当前代码状态整理

---

## 目录

1. [问题背景](#1-问题背景)
2. [核心结论：当前慢点不在 STW，而在 mutator refill 路径](#2-核心结论当前慢点不在-stw而在-mutator-refill-路径)
3. [case1 的直接证据](#3-case1-的直接证据)
4. [当前已确认的 mutator 吞吐瓶颈](#4-当前已确认的-mutator-吞吐瓶颈)
5. [已完成优化与收益](#5-已完成优化与收益)
6. [剩余优化优先级](#6-剩余优化优先级)
7. [结论](#7-结论)

---

## 1. 问题背景

在 case1_baseline 中，MSGC 相比 CopyGC 已经表现出明显更低的 GC 时间和更好的 RSS 指标，但 benchmark 总时间仍然偏高。

以当前对比结果为例：

- CopyGC nofastpath：5337 ms
- MSGC（bulkzeroopt 后）：6314 ms
- MSGC 相对 CopyGC 仍慢约 18.3%

与此同时，MSGC 的 GC 相关指标明显更优：

- GC 总时间更低
- GC P95 明显更低
- GC 次数更少
- RSS 平均/峰值更低

这说明当前问题已经不是“GC pause 太长”，而是 **mutator 自身的分配/回收协同路径吞吐不足**。

---

## 2. 核心结论：当前慢点不在 STW，而在 mutator refill 路径

基于 Time Profiler、sample 文本与 GC log，当前 MSGC 的主要额外成本集中在以下调用链：

```text
RegionSpaceMS::Allocate
  -> AllocBufferMS::SlowAlloc
    -> CentralRegionPool::GetRegion
      -> MSRegionInfo::SweepRegion
```

也就是说，mutator 在申请新 region 时，仍然频繁承担以下同步工作：

1. 从 central pool 查找可用 region
2. 对 unswept region 做 lazy sweep
3. 对已死 slot 做清零
4. 在 empty/fresh region 上重建 size class 元数据

这些工作越接近分配热路径，就越直接拖慢 mutator 吞吐。

---

## 3. case1 的直接证据

### 3.1 benchmark 分阶段差异符合“分配压力型退化”

profile 采样结果显示，MSGC 相比 CopyGC 的主要损失集中在大对象、碎片化和 churn 更强的阶段：

| 阶段 | MSGC | CopyGC nofastpath | 差值 |
|------|------:|------------------:|-----:|
| P1_SmallAlloc | 330 ms | 264 ms | +66 ms |
| P2_LargeAlloc | 1273 ms | 1000 ms | +273 ms |
| P3_MixedWorkload | 791 ms | 599 ms | +192 ms |
| P4_LongLivedChurn | 652 ms | 498 ms | +154 ms |
| P5_Fragmentation | 1072 ms | 807 ms | +265 ms |

退化最大的正是最依赖 region refill、sweep、reclaim 策略的阶段。

### 3.2 sample 热点直接落在 SlowAlloc / GetRegion / SweepRegion

在 [sample_msgc.txt](../Result/case1_baseline/analysis/20260422_msgc_profile/sample_msgc.txt) 中，可以看到以下反复出现的热点栈：

- `AllocBufferMS::SlowAlloc` → `CentralRegionPool::GetRegion` → `MSRegionInfo::SweepRegion`
- `CentralRegionPool::GetRegion` 的 partialUnswept / fullUnswept 路径重复命中
- `MarkSweepCollector::MarkNewObject` 仅极少量出现，不是主热点

这说明主要 CPU 并没有耗在 concurrent mark 本身，而是耗在 mutator 为了拿到可分配 region 所付出的同步 refill 成本上。

### 3.3 GC log 显示 sweep backlog 很大

case1 的 GC log 中，cycle 1 结束后出现了明显 unswept backlog：

- `partialUnswept: 14 regions`
- `fullUnswept: 2559 regions`

这意味着后台 sweep 即使已经启动，仍无法在 mutator 需要分配时及时把足够多的 region 转成 ready 状态，进而迫使 mutator 落入 lazy sweep 路径。

### 3.4 seed5 火焰图显示：当前热点已从“顶层 TLS 取数”转向“分配 + 泛型辅助 + 拷贝”复合成本

在旧样本 [sample_seed5_msgc.txt](../Result/case1_baseline/analysis/20260422_seed5_msgc_profile/sample_seed5_msgc.txt) 中，seed5 的热点形态与前面的 case1 baseline 有明显区别：

> 其中与泛型容器、`TypeInfoManager`、`WriteGeneric` / `WriteStruct` 相关的热点，已单独整理为 [Step7_泛型运行时辅助开销分析与优化方向.md](Step7_%E6%B3%9B%E5%9E%8B%E8%BF%90%E8%A1%8C%E6%97%B6%E8%BE%85%E5%8A%A9%E5%BC%80%E9%94%80%E5%88%86%E6%9E%90%E4%B8%8E%E4%BC%98%E5%8C%96%E6%96%B9%E5%90%91.md)。

- `RegionSpaceMS::Allocate` / `TryAllocateOnceMS` 仍然是最宽的分配主链
- `AllocBufferMS::SlowAlloc` → `CentralRegionPool::GetRegion` → `MSRegionInfo::SweepRegion` 仍能看到，但不再是唯一主热点
- 更多样本落在 `Mutator::GetMutator` → `ThreadLocal::GetThreadLocalData` → `_tlv_get_addr`
- 另外一簇明显热点来自 `TypeInfoManager::GetOrCreateTypeInfo`、`Barrier::WriteGeneric` / `WriteStruct`、`memcpy_s`

2026-04-23 又补抓了一份“当前代码”的 seed5 实样 [sample_seed5_msgc_current.txt](../.tmp/profile_case1_seed5_current_20260423_hotspot2/sample_seed5_msgc_current.txt)，结论已经比旧样本更清楚：

- `RegionSpaceMS::Allocate` / `TryAllocateOnceMS` 依然是主链，说明分配与 refill 仍是 mutator 第一热点
- `AllocBufferMS::SlowAlloc`、`CentralRegionPool::GetRegion`、`MSRegionInfo::SweepRegion` 仍然存在，但占比已经从“唯一主导”变成“分配主链中的一段”
- 旧样本里最显眼的 `Mutator::GetMutator` / `ThreadLocal::GetThreadLocalData` / `_tlv_get_addr` 顶层热点已经明显收缩，取而代之的是 `MRT_UpdateThreadLocalDataPhase` 的较窄残余成本
- 新样本里更突出的热点变成了 `Barrier::WriteGeneric` / `WriteStruct` / `CJ_MCC_AssignGeneric` 下面的 `memcpy_s`，以及 `TypeInfoManager::GetOrCreateTypeInfo`
- 另外还能稳定看到 `TypeInfo::GetMethodOuterTI`，说明接口方法外层类型解析也开始进入主路径视野

这说明在 seed5 这种更激进的启发式参数下，mutator 的问题不再只是“偶尔掉进一次 lazy sweep”，而是：

1. 分配更频繁
2. 每次分配附带的类型查询、写屏障、字段拷贝和少量 TLS phase 刷新也更频繁
3. 这些原本分散的小成本被整体堆成了稳定的吞吐损失

因此，seed5 的吞吐下降并不代表 collector 核心变慢，而更接近“高频 allocation pressure 下，mutator fast path 仍然偏厚，只是当前最厚的位置已经从顶层 TLS 取数转移到了分配、泛型辅助和结构体拷贝上”。

---

## 4. 当前已确认的 mutator 吞吐瓶颈

### 4.1 瓶颈 A：GetRegion 仍会在 mutator 线程上执行 lazy sweep

代码位置：

- [MSCentralRegionPool.cpp](../../src/Heap/MAllocator/MSCentralRegionPool.cpp)

`CentralRegionPool::GetRegion()` 当前优先顺序为：

```text
partialSwept
-> partialUnswept (lazy sweep)
-> fullUnswept (lazy sweep)
-> emptySweptCache
-> fresh region
```

当 partialSwept 不足时，mutator 会直接进入 `partialUnswept` 和 `fullUnswept` 分支，并同步调用 `region->SweepRegion()`。这会把 sweep 成本直接压在分配线程上。

**影响**：ready region 不足时，吞吐立即退化；后台 sweep 越跟不上，mutator 越容易被迫“自扫自用”。

从 2026-04-23 的当前 seed5 sample 看，这一段仍然值得被当作 mutator 主成本来处理，原因不是它在火焰图上“单函数最宽”，而是它仍然是分配主链里最贵、最难被其他优化掩盖的一段：

- 小对象一旦在 `FastAlloc` 上 miss，就会立刻进入 `SlowAlloc`
- `SlowAlloc` 的核心成本又集中在 `RefillFromCentral` → `GetRegion`
- `GetRegion` 命中 unswept 路径后，会把 `SweepRegion`、空 region rebinding、list 迁移这些工作同步压回 mutator
- 即使单次 `SweepRegion` 样本占比不如 `memcpy_s` 那么显眼，它仍然属于“miss 一次就要整段付费”的高延迟链路

换句话说，当前分配热点可以分成两层：

1. **高频固定税**：`Allocate` / TLS phase 更新 / TypeInfo / Barrier / `memcpy_s`
2. **高代价 miss 链**：`SlowAlloc` → `GetRegion` → lazy sweep / refill / rebind

第一层决定“每次分配都贵一点”，第二层决定“只要 active region 用尽，延迟就会突然抬高一截”。对 case1 这类小对象分配密集的 workload，第二层虽然不是唯一热点，但仍然是吞吐掉档时最直接的结构性原因。

这一串如果继续优化，建议按“减少 miss 次数”与“降低 miss 单次成本”两条线拆开做：

1. **先减少 mutator 掉进 `SlowAlloc` 的频率**
  - 目标不是简单提早唤醒后台，而是提高 `partialSwept` / `cachedRegion_` 的命中率
  - 更值得做的是让后台更稳定地产生“已经绑好 size class、可立即分配”的 ready region，而不是等 miss 发生后再补
  - 可以重点看 `BackgroundFillCache()` 与 `BackgroundSweep()` 之间是否还能建立更直接的 ready-region 交接，而不是只通过 `cacheMiss_` 反应式触发

2. **把 lazy sweep 从 mutator 路径继续往后台剥离**
  - 当前 `GetRegion` 的问题不只是会 sweep，而是它在 `partialUnswept` / `fullUnswept` 上现取现扫
  - 可以考虑给 mutator 的 `GetRegion` 再收紧策略：优先只吃真正 ready 的 `partialSwept` / 已初始化 empty region，不足时优先推动后台 sweep，而不是让 mutator 无限承担 lazy sweep
  - 这条线要谨慎，核心不是“完全禁用 lazy sweep”，而是给 mutator 设更小、更可控的 lazy-sweep 预算，避免一次 miss 做太多工作

3. **继续降低单次 `SweepRegion` 的准备成本**
  - 当前已经有“全空 region 整块清零”的 fast path，但部分存活 region 仍要扫 bitmap、清 free slot、重建 region 状态
  - 如果后续继续做这一块，重点应放在“减少每次 sweep 需要处理的元数据/zero bytes 总量”，而不只是单纯把 `memset` 调用从多次并成一次
  - 也就是说，优化目标应是让 `SweepRegion` 更像一次轻量状态转换，而不是一次完整整理

4. **把 refill 后处理压成更便宜的 rebind，而不是完整初始化**
  - 现在 `TakeEmptySweptRegion` / `GetFreshRegion` 后面仍可能落到 `InitForSizeClass`
  - 如果能让 empty region 在后台就预先完成更多 size-class 相关元数据准备，那么 mutator miss 时就更接近“拿现成 region + 改少量状态”
  - 这条线比“单纯增加后台补货频率”更有价值，因为它是在减少 ready-region 的生产成本

5. **针对 `SlowAlloc` 建立更明确的观测指标**
  - 当前 profile 已经能看到 `SlowAlloc` / `GetRegion` / `SweepRegion`，但还缺一组直接反映其退化程度的 runtime 计数
  - 建议补几项轻量统计：每个 size class 的 `SlowAlloc` 次数、`GetRegion` 命中 `partialUnswept/fullUnswept` 次数、mutator lazy sweep 耗时/region 数
  - 这样后续每次改动都能直接判断：到底是 miss 次数降了，还是 miss 还在但单次成本变低了

如果目标是**优先减少掉进 `SlowAlloc` 的次数，提高 `ready region` / `cachedRegion_` 命中率**，则更具体的落地顺序建议如下：

1. **把后台补货触发点从“miss 之后”前移到“快要用尽 active region”**
   - 当前 `cachedRegion_` 的补货只在 `SlowAlloc()` miss 后累加 `cacheMiss_[cls]`，超过阈值才通过 `NotifyToFeedAllocBuffers()` 叫后台线程补货
   - 这意味着现状是“先 miss，再补货”，天然来不及兜住当前这次分配
   - 更直接的做法是在 `FastAlloc()` 成功后，检测 active region 剩余可用 slot 是否低于一个小阈值；如果低于阈值且 `cachedRegion_[cls] == nullptr`，就提前把该 class 标记为需要后台补货
   - 这样补货信号发生在 region 真正耗尽之前，目标是让下一次 region 切换优先命中 `cachedRegion_`，而不是先掉进 `SlowAlloc`

2. **把 `cacheMiss_` 从“纯 miss 计数器”改成“需求信号”**
   - 当前 `BackgroundFillCache()` 的入口条件是 `cacheMiss_[cls] >= kCacheFillThreshold`
   - 对减少 `SlowAlloc` 次数来说，这个条件太滞后，因为它必须已经 miss 了多次，后台才会介入
   - 可以把它改成两段式：
     - `miss` 仍然递增，保留当前退避语义
     - 另加一个轻量 `needRefill_[cls]` / `prefetchPending_[cls]` 标志，由 active region 低水位直接置位
   - `BackgroundFillCache()` 优先处理这些“即将耗尽”的 class，而不是只盯着已经 miss 很多次的 class

3. **让 `BackgroundFillCache()` 在没有现成 ready region 时，继续生产 ready region**
   - 当前 `BackgroundFillCache()` 只做一件事：`central_->GetRegion(cls)`；拿不到就直接放弃
   - 这会导致一个问题：如果 central pool 当下没有 ready region，但系统其实有 empty/fresh region 可用，后台线程也不会继续往下做
   - 更可行的方向不是简单打开“fresh region 全量预热”，而是给后台补货一条受控的 fallback：
     - 先尝试 `GetRegion(cls)` 拿 ready region
     - 若 ready 不足，再在严格预算下尝试 `TakeEmptySweptRegion/Rebind` 或少量 `GetFreshRegion(cls)`
   - 关键点是要加预算，避免像之前的 aggressive prefill 一样把 committed/RSS 又抬起来

4. **把 `MaybeNotifySweep()` 的触发从“已经进入 unswept 路径”再往前推半步**
   - 当前 `MaybeNotifySweep()` 是在 `GetRegion()` 进入 unswept 路径时才通知后台 sweep
   - 这仍然属于“mutator 自己都快要下场扫了，才去叫后台”
   - 更合理的做法是在以下条件组合出现时就提前通知：
     - 当前 class 的 ready count 低
     - 当前 alloc buffer 的 active region 剩余空间也低
     - `cachedRegion_[cls]` 为空
   - 也就是说，后台 sweep 的信号应该同时看“central pool 的压力”和“alloc buffer 的本地饥饿感”，而不是只看前者

5. **把 ready-region 水位从“全局常数”收敛成“按 class / 按 buffer 热度”的目标值**
   - 现在 `MaybeNotifySweep()` 主要使用 `CJ_MS_BG_SWEEP_NOTIFY_READY_WATERMARK` 这类全局水位
   - 但 case1 里热点通常集中在少数 size class，统一阈值容易出现两种问题：
     - 热 class ready 不够，但总水位看起来还行
     - 冷 class 被过度准备，抬高内存与后台成本
   - 因此更合适的方案是：以“最近一段时间内该 class 的 `SlowAlloc` 次数 / active region 消耗速度”为依据，给热点 class 更高的 ready/cached 目标值

6. **优先做“一深度 backup 命中”而不是多层缓存**
   - 当前结构已经有 `activeRegions_[cls] + cachedRegion_[cls]` 这套一深度设计
   - 对 case1 这类负载，先把“一深度 backup 真的大多数时候都有货”做好，通常比继续加二级/三级缓存更划算
   - 因为更深缓存虽然可能继续减少 miss，但也更容易带来：
     - 更多 region 提前绑定 size class
     - 更多 empty/fresh region 被保留在 thread-local 侧
     - RSS/GC 时间回升

7. **先用统计确认该做的是“减少 miss”还是“提高 cached 命中率”**
   - 两者看起来接近，但优化侧重点不同：
     - 如果 `SlowAlloc` 次数高，但 `cachedRegion_` 命中后效果很好，说明重点是让后台更早把 backup 填好
     - 如果 `cachedRegion_` 命中率本身不低，但仍然频繁 `SlowAlloc`，说明问题更可能在 active region 太小、切换过于频繁，或 ready region 生产成本仍太高
   - 所以建议先补三类统计：
     - `FastAlloc -> cachedRegion_` 命中次数
     - `SlowAlloc` 次数
     - `BackgroundFillCache` 成功填充次数 / 因无 ready region 失败次数
   - 这三组数一出来，后续就能更快判断应该优先改触发时机，还是优先改 ready-region 生产路径

关于“要不要在每次 fast path 都检查并通知后台 sweep”，当前结论是否定的：**不值得把通知挂在每次 fast path 上。**

原因很直接：

- `FastAlloc` 是最热路径，哪怕只多一次剩余 slot 检查和一次条件分支，累计成本也会非常高
- 当前问题不是“完全没有通知后台 sweep”，而是**现有通知普遍偏晚**，多发生在 ready region 已经不够、甚至 mutator 已经开始走 refill 之后
- 因此更合适的思路不是“每次分配都看一眼”，而是把通知挂在**状态转折点**上：`cachedRegion_` 落空、active region 耗尽、后台补货失败、fresh region 被拿走这些事件都远比“每次 fast path”便宜

### 4.1.1 当前 sweep 通知点盘点

按当前代码，后台 sweep 主要在以下几个地方被通知：

1. **`CentralRegionPool::GetRegion()` 进入 `partialUnswept` 路径时**
  - 代码位置：[MSCentralRegionPool.cpp](../../src/Heap/MAllocator/MSCentralRegionPool.cpp)
  - 这是当前最直接的 mutator 侧通知点，但时机明显偏晚：说明 mutator 已经准备自己去碰 unswept region 了

2. **`MSRegionManager::TakeRegion()` 拿到 fresh region 之后**
  - 代码位置：[MSRegionManager.cpp](../../src/Heap/MAllocator/MSRegionManager.cpp)
  - 这说明系统在消费 fresh region 时会顺手 kick 一下 sweep，但它仍然发生在 ready region 已经不够、只能去拿 fresh region 之后

3. **`MarkSweepCollector::ConcurrentSweep` 阶段结束时无条件 kick**
  - 代码位置：[MarkSweepCollector.cpp](../../src/Heap/MCollector/MarkSweepCollector.cpp)
  - 这个点是 GC 周期内的正常背景 sweep 启动点，作用更像“收尾后让后台继续扫”，并不负责兜住 mutator 的瞬时饥饿

4. **`CentralRegionPool::HandleBackgroundScavenge()` 发现 true-live slack / empty-cache 压力时**
  - 代码位置：[MSCentralRegionPool.cpp](../../src/Heap/MAllocator/MSCentralRegionPool.cpp)
  - 这条线更偏向 reclaim / pressure 管理，不是按某个热点 size class 的分配需求来驱动的

5. **`MSRegionManager::MaybeNotifySweep()` 本身的阈值判断**
  - 代码位置：[MSRegionManager.cpp](../../src/Heap/MAllocator/MSRegionManager.cpp)
  - 当前判断很简单：`ReadyCount() < CJ_MS_BG_SWEEP_NOTIFY_READY_WATERMARK && UnsweptCount() > 0`
  - 这意味着它只看 central pool 的 bucket 压力，不看 alloc buffer 本地是否已经快断粮

### 4.1.2 更值得加通知的地方

如果只考虑“以尽量小的额外成本，更早通知后台 sweep”，比起每次 fast path 都检查，更值得加通知的地方是这几个：

1. **`SlowAlloc()` 在 `cachedRegion_` miss、调用 `RefillFromCentral()` 之前**
  - 代码位置：[MSAllocBuffer.cpp](../../src/Heap/MAllocator/MSAllocBuffer.cpp)
  - 这是最值得加的一个点，因为它表示：
    - active region 已经耗尽
    - thread-local backup 也没有命中
    - mutator 现在马上要去 central pool 要 region
  - 在这里先调用一次 `MaybeNotifySweep(central_, cls)`，比等 `GetRegion()` 真进 unswept 分支再通知更早半步，而且频率是“每次 active region 用尽一次”，不是“每次 fast path 一次”

2. **`BackgroundFillCache()` 在 `GetRegion(cls)` 失败时**
  - 代码位置：[MSAllocBuffer.cpp](../../src/Heap/MAllocator/MSAllocBuffer.cpp)
  - 这是第二个很合适的点，因为它发生在后台线程里，额外成本几乎不压在 mutator 身上
  - 语义也很清楚：后台本来就是来补 `cachedRegion_` 的，如果连它都拿不到 ready region，说明该 class 的 ready 供应确实偏紧，这时顺手再 kick 一次 sweep 很合理

3. **`ReturnRegion(cls)` 之后，若 `cachedRegion_[cls]` 为空且 active region 被判定为 exhausted**
  - 代码位置：[MSAllocBuffer.cpp](../../src/Heap/MAllocator/MSAllocBuffer.cpp)
  - 这是一个比 `FastAlloc` 更粗粒度的转折点：一个 region 生命周期已经结束，下一次同 class 分配很快就会需要新 region
  - 如果这里结合 central bucket 的 ready/unswept 状态做一次轻量判断，再决定是否通知 sweep，会比“每次分配都看”便宜得多

### 4.1.3 现有通知是否需要更激进

如果只在现有代码基础上做“更激进一些”的调整，优先建议是：

1. **先调激进 `MaybeNotifySweep()` 的 ready watermark，而不是把通知塞进每次 fast path**
  - 也就是优先尝试调大 `CJ_MS_BG_SWEEP_NOTIFY_READY_WATERMARK`
  - 这会让当前已有的 `GetRegion()` / `TakeRegion()` 通知点更容易触发，而不会增加 fast path 常态开销

2. **但仅调激进现有阈值还不够，因为当前主要问题是“触发点太晚”**
  - `GetRegion()` 路径上的通知再激进，本质上也发生在 mutator 已经进入 refill 之后
  - `TakeRegion()` 路径上的通知更晚，因为 fresh region 都已经开始被消费了
  - 所以单纯调阈值，只能缓解，不能从根上减少 `SlowAlloc`

3. **最合理的组合是：保留当前阈值机制，再新增 1 到 2 个更早但更粗粒度的通知点**
  - 首选：`SlowAlloc()` 的 `cachedRegion_` miss 前
  - 次选：`BackgroundFillCache()` 拿不到 ready region 时
  - 这两个点加起来，已经能覆盖“mutator 即将断粮”和“后台补货也失败”两种真正需要更早 sweep 的情况

因此，当前更推荐的方向不是“让每次 fast path 都检查”，而是：

- **不碰每次 fast path**
- **先把 `MaybeNotifySweep()` 的现有水位调得略激进**
- **再把 sweep 通知前移到 `SlowAlloc cached miss` 和 `BackgroundFillCache miss` 这两个状态转折点上**

这样额外开销更低，也更贴近真正导致 `SlowAlloc` 发生的事件。

### 4.1.4 当前 GC 通知点盘点与“下沉到后台 refill 线程”的改造方向

如果继续优化 mutator 吞吐，一个很值得单独处理的问题是：**当前大量 GC 相关通知仍然由 mutator 分配路径直接触发**。这会让分配线程除了“拿 region”之外，还顺手承担一次 sweep / reclaim / mark 的策略判断与唤醒动作。

当前通知点可以按“是否发生在 mutator 线程上”分成两类：

#### A. 当前由 mutator 路径直接触发的通知

1. **`AllocBufferMS::SlowAlloc()` 在本地 backup miss 时**
  - 代码位置：[MSAllocBuffer.cpp](../../src/Heap/MAllocator/MSAllocBuffer.cpp)
  - 当前行为：在 `cachedRegion_` miss 后，会直接调用 `central_->MaybeNotifySweep(cls)`；同时还会通过 `NotifyToFeedAllocBuffers()` 叫后台补 alloc cache
  - 问题：这说明 mutator 刚发现“自己快断粮”时，不仅要 refill，还要顺手做一次 sweep 通知

2. **`CentralRegionPool::GetRegion()` 进入 `partialUnswept` 路径时**
  - 代码位置：[MSCentralRegionPool.cpp](../../src/Heap/MAllocator/MSCentralRegionPool.cpp)
  - 当前行为：在真正开始碰 unswept region 前，会调用 `msRegionManager_->MaybeNotifySweep(this, cls)`
  - 问题：通知时机已经很晚，因为这意味着 mutator 已经准备自己下场走 lazy sweep 了

3. **`MSRegionManager::TakeRegion()` 拿到 fresh region 之后**
  - 代码位置：[MSRegionManager.cpp](../../src/Heap/MAllocator/MSRegionManager.cpp)
  - 当前行为：会继续调用 `MaybeNotifySweep()`、`MaybeNotifyReclaim()`、`MaybeNotifyMark()`
  - 问题：fresh region 都已经被消费了，才开始做 sweep / reclaim / mark 的启发式判断，本质上仍是“mutator 先兜底，再顺手叫后台”

4. **`MSRegionManager::TakeRegionForLarge()` 拿到 large span 之后**
  - 代码位置：[MSRegionManager.cpp](../../src/Heap/MAllocator/MSRegionManager.cpp)
  - 当前行为：调用 `MaybeNotifyReclaim()`、`MaybeNotifyMark()`
  - 问题：大对象路径本来就重，再叠加启发式判断，会继续拉长 mutator 分配延迟

5. **empty cache 跨高水位时触发 reclaim**
  - 代码位置：[MSCentralRegionPool.cpp](../../src/Heap/MAllocator/MSCentralRegionPool.cpp)
  - 当前行为：`PushEmptySweptRegion()` 在 empty cache 跨高水位时，会调用 `MaybeNotifyReclaim()`
  - 问题：这条线虽然不总发生在 mutator 上，但经常是 mutator 归还 region 时同步触发出来的

#### B. 已经发生在后台线程上的通知

1. **`BackgroundFillCache()` 拿不到 ready region 时**
  - 代码位置：[MSAllocBuffer.cpp](../../src/Heap/MAllocator/MSAllocBuffer.cpp)
  - 当前行为：后台 alloc-cache refill 失败时，也会顺手 `MaybeNotifySweep(cls)`
  - 价值：这是最接近“把通知责任上移到后台 refill 线程”的现成落点

2. **`HandleBackgroundScavenge()` 发现 true-live slack / empty-cache 压力时**
  - 代码位置：[MSCentralRegionPool.cpp](../../src/Heap/MAllocator/MSCentralRegionPool.cpp)
  - 当前行为：后台 scavenge 过程中会先选一个真实受压的 size class，再通过 `MaybeNotifySweep(this, cls)` 反向催促 sweep
  - 价值：已经属于后台线程主导的 pressure 管理

3. **`BackgroundSweep()` 在 unswept backlog 耗尽时**
  - 代码位置：[MSCentralRegionPool.cpp](../../src/Heap/MAllocator/MSCentralRegionPool.cpp)
  - 当前行为：若背景 sweep 把 unswept 清空，会调用 `MaybeNotifyMark(BG_SWEEP_EXHAUSTED, this)`
  - 价值：这已经是“后台先观察状态，再决定是否推动下一轮 GC”的雏形

#### C. 当前三类策略判断分别在哪

1. **Sweep**：`MSRegionManager::MaybeNotifySweep()`
  - 代码位置：[MSRegionManager.cpp](../../src/Heap/MAllocator/MSRegionManager.cpp)
  - 当前主要依据：`ReadyCount() < ready watermark && UnsweptCount() > 0`

2. **Reclaim**：`MSRegionManager::MaybeNotifyReclaim()`
  - 代码位置：[MSRegionManager.cpp](../../src/Heap/MAllocator/MSRegionManager.cpp)
  - 当前主要依据：empty cache 是否跨高水位、committed bytes 是否高于 true-live slack target

3. **Mark**：`MSRegionManager::MaybeNotifyMark()`
  - 代码位置：[MSRegionManager.cpp](../../src/Heap/MAllocator/MSRegionManager.cpp)
  - 当前主要依据：GC threshold、released tree 压力、ready/unswept/empty cache 状态，以及 cooldown

这说明当前真正“重”的部分并不是 `NotifyToSweep()` / `NotifyToReclaimGarbage()` 这些唤醒动作本身，而是：**mutator 每次 fresh miss 都会顺手跑一轮策略判断。**

#### D. 更推荐的改造方向：mutator 只记账，后台 refill 线程负责判定与通知

后续如果沿这条线继续做，更合理的目标不是“把所有通知都删掉”，而是把职责改成：

1. **mutator 线程只做轻量记账，不直接做策略判断**
  - small alloc fresh miss：记录 `smallAllocMissByClass[cls]`
  - bump source fresh region：记录 `bumpAllocPressureByClass[cls]`
  - large alloc：记录 `largeAllocCount` 或 `largeAllocBytes`
  - empty cache 跨高水位：置 `reclaimPending`

2. **后台 alloc-cache refill pass 统一 drain 这些 hint/counter**
  - 最自然的落点是 `BackgroundFillAllocCaches()` / `BackgroundFillCache()` 之后
  - 后台线程在一次 refill pass 里：
    - snapshot 并清空 hint
    - 对命中的 class 聚合
    - 再调用现有的 `MaybeNotifySweep()` / `MaybeNotifyReclaim()` / `MaybeNotifyMark()`

3. **通知改成批量 / 阈值触发，而不是“一次 miss 一次判断”**
  - `MaybeNotifySweep()`：对同一 class 累积到一定 miss 次数后再触发
  - `MaybeNotifyReclaim()`：一个 refill pass 最多做一次整体 reclaim 判定
  - `MaybeNotifyMark()`：按累计 small fresh refill 次数、large alloc 次数或字节阈值，批量做一次启发式判断

#### E. 为什么这条线值得做

如果改造完成，职责会从：

```text
mutator = 分配 + 压力判定 + 发通知
```

变成：

```text
mutator = 分配 + 记账
background refill thread = 汇总压力 + 判定 + 发通知
```

这样做的直接收益是：

1. `TakeRegion()` / `TakeRegionForLarge()` 热路径更短
2. sweep / reclaim / mark 的判断频率从“每次 fresh miss”降为“每批次 refill 一次”
3. 后台 refill 线程天然更适合观察 `cacheMiss_`、ready region 水位、empty cache 压力这些指标

从实现复杂度看，这条线也相对可控，因为：

- 现有 `MaybeNotifySweep()` / `MaybeNotifyReclaim()` / `MaybeNotifyMark()` 三套策略函数本身并不一定要重写
- 更需要改的是“谁调用它们、调用时机是什么”
- 已有的 `BackgroundFillCache()`、`HandleBackgroundScavenge()`、`BackgroundSweep()` 已经证明：后台线程本来就在承担一部分观察与触发职责

因此，这条优化的本质不是“新增一个后台机制”，而是**把原本分散在 mutator 路径里的通知责任，进一步收敛到后台 refill / background processor 上。**

### 4.2 瓶颈 B：部分存活 region 的清零仍是逐 slot 扫描

代码位置：

- [MSRegionInfo.cpp](../../src/Heap/MAllocator/MSRegionInfo.cpp)

虽然已经加入“整个 region 无活对象时整块 memset”的 fast path，但只要 region 里仍有部分 live slot，就仍然会对 free bits 做逐 slot 遍历和逐 slot `memset`。

**影响**：在 P3/P4/P5 这类 churn/fragmentation 场景中，很多 region 不是“全空”，而是“部分存活 + 大量空洞”，因此这部分成本依然保留在 mutator 上。

### 4.3 瓶颈 C：SlowAlloc 在主路径里还会同步做一次 backup prefetch

代码位置：

- [MSAllocBuffer.cpp](../../src/Heap/MAllocator/MSAllocBuffer.cpp)

> 2026-04-22：该同步 prefetch 已先在代码中关闭，当前保留的 backup cache 补充路径仅剩后台 `BackgroundFillCache()`。

`AllocBufferMS::SlowAlloc()` 在拿到 `newRegion` 之后，还会尝试额外执行一次：

```text
central_->GetRegion(cls)
```

并把结果塞到 `cachedRegion_[cls]` 中作为下次备用。

这意味着一次 SlowAlloc 可能包含两段 central pool 成本：

1. 当前真正需要的 refill
2. 为未来准备的同步 prefetch

如果第二次 `GetRegion` 也命中 unswept 路径，mutator 会在一次 miss 里支付两次 refill/lazy sweep 成本。

这里需要特别区分两套机制：

1. **同步 prefetch**：发生在 `SlowAlloc()` 的当前线程里，目标是“既然这次已经 miss 了，就顺手把下一次要用的 backup region 也取好”。它的优点是立刻生效，不依赖后台线程调度；缺点是会直接把第二次 `GetRegion()` 的成本压回 mutator。
2. **后台补货**：发生在 `BackgroundFillCache()` 中，由 gc-helper/background 线程在 `cacheMiss_` 达到阈值后异步执行。它的目标是“持续给饥饿的 alloc buffer 补一个 cached region”，不阻塞当前这次分配。

换句话说，后台实现原本就是存在的；同步 prefetch 不是“没有后台实现所以只能同步补”，而是一个更激进的就地优化：希望用一次 slow path 顺手换来下一次 fast path。但在 case1 这种 `GetRegion()` 本身就可能落入 lazy sweep 的场景里，这个优化开始反噬，因此先关闭同步版本，只保留后台补货路径，后续再看 benchmark 是否改善。

### 4.4 瓶颈 D：empty/fresh region 的 InitForSizeClass 仍在 mutator 路径里

代码位置：

- [MSCentralRegionPool.cpp](../../src/Heap/MAllocator/MSCentralRegionPool.cpp)

无论是：

- `TakeEmptySweptRegion(cls)`
- `GetFreshRegion(cls)`

都会在 mutator 线程上调用 `InitForSizeClass(cls)`，重新建立当前 size class 所需的 bitmap、slot count、region 元数据。

**影响**：即使没有 lazy sweep，只要 ready region 不够，mutator 仍可能落到“现拿现初始化”的路径。

### 4.5 瓶颈 E：后台填充与后台 sweep 当前更偏“反应式”

代码位置：

- [MSAllocBuffer.cpp](../../src/Heap/MAllocator/MSAllocBuffer.cpp)
- [MSRegionManager.cpp](../../src/Heap/MAllocator/MSRegionManager.cpp)

当前后台填充依赖 `cacheMiss_` 达阈值后才唤醒；后台 sweep 也主要在 ready region 低于水位时才被通知。

这种策略的特点是：

- 先发生 miss
- 再唤醒后台线程
- 后台线程之后才尝试补货

因此对于强突发负载，mutator 经常会先进入 SlowAlloc，再等后台慢慢把 cache 补上。

### 4.6 瓶颈 F：大对象路径附带 inline reclaim 与 pacing debt

代码位置：

- [MSRegionSpace.cpp](../../src/Heap/MAllocator/MSRegionSpace.cpp)
- [MSRegionManager.cpp](../../src/Heap/MAllocator/MSRegionManager.cpp)

大对象分配走 `TakeRegionForLarge()` 时，当前还会：

1. 触发 `InlineReclaimForBalance()`
2. 注册 large region 元数据
3. 追加 pacing debt

其中 `InlineReclaimForBalance()` 会同步释放一个 empty-cache region 来平衡 committed bytes。

**影响**：在 P2_LargeAlloc 阶段，这部分同步附带工作会直接增加 mutator 分配延迟。

### 4.7 瓶颈 G：分配快路径里的 TLS 开销已收缩，但仍有残余 thread-local 与 GC phase 刷新成本

代码位置：

- [MSRegionSpace.cpp](../../src/Heap/MAllocator/MSRegionSpace.cpp)
- [MSAllocBuffer.cpp](../../src/Heap/MAllocator/MSAllocBuffer.cpp)
- [Mutator.cpp](../../src/Mutator/Mutator.cpp)
- [ThreadLocal.cpp](../../src/Mutator/ThreadLocal.cpp)

旧 seed5 sample 中，`Mutator::GetMutator` / `ThreadLocal::GetThreadLocalData` / `_tlv_get_addr` 曾在分配主链里反复出现；但从 2026-04-23 的当前样本看，这条链已经不再是最宽的顶层热点。

当前典型链路是：

```text
RegionSpaceMS::TryAllocateOnceMS
  -> AllocBufferMS::GetOrCreateAllocBufferMS
    -> ThreadLocal::GetThreadLocalData

AllocBufferMS::Allocate
  -> Mutator::GetMutator
    -> ThreadLocal::GetMutator
      -> ThreadLocal::GetThreadLocalData
```

这意味着在原始路径下，一次普通小对象分配即便完全走 fast path，也至少会重复做两次 thread-local 访问：

1. 先拿 `AllocBufferMS`
2. 再拿 `Mutator` / `GCPhase`

当前代码已经把这部分重复取数压掉一部分，但样本里仍能看到两类残余成本：

1. `RegionSpaceMS::Allocate` 入口本身仍要做一次 `ThreadLocal::GetThreadLocalData`
2. `CJ_MCC_NewObject` 返回后的 `MRT_UpdateThreadLocalDataPhase` 仍会在高频分配下累计成可见成本

**影响**：这仍然不是单次 miss 的大 stall，而是“每次分配都多付一点成本”；只是从当前样本看，它已经从第一优先级下降为需要继续压薄、但不再单独主导总吞吐的次级热点。

---

## 5. 已完成优化与收益

### 5.1 已完成：完全空 region 的 bulk clear fast path

已在 `MSRegionInfo::SweepRegion()` 中加入：

- 若 `liveCount == 0`，直接整块 `memset(region, 0, MS_REGION_SIZE)`
- 否则保留原先的逐 slot 清零逻辑

### 5.2 当前收益

在 case1_baseline 中，对比优化前后的 MSGC：

- 总时间：6537 ms → 6314 ms（约 -3.4%）
- P5_Fragmentation：962 ms → 908 ms（约 -5.6%）
- GC 总时间：100.22 ms → 83.60 ms（约 -16.6%）
- GC P95：10.13 ms → 5.71 ms（约 -43.6%）

这说明“清零策略”确实是 mutator 热点的一部分，但只解决了整个问题中的一层。

### 5.3 关闭同步 backup prefetch 的实验结果

在保留后台 `BackgroundFillCache()`、仅关闭 `SlowAlloc()` 中同步 backup prefetch 后，case1_baseline 进一步得到如下结果：

- 总时间：6314 ms → 6247 ms（约 -1.1%）
- RSS 平均：89.14 MB → 80.58 MB（约 -9.6%）
- RSS 峰值：191.64 MB → 163.84 MB（约 -14.5%）
- GC 总时间：83.60 ms → 71.79 ms（约 -14.1%）
- GC P95：5.71 ms → 4.97 ms（约 -12.9%）

分阶段看也都是小幅正向：

- P1：295 ms → 289 ms
- P2：1095 ms → 1079 ms
- P3：670 ms → 661 ms
- P4：530 ms → 520 ms
- P5：908 ms → 895 ms

这说明 Step 6.4.3 的判断基本成立：在 `GetRegion()` 本身已不便宜、且可能落入 lazy sweep 的场景里，同步 prefetch 的“顺手再拿一个”并没有摊薄成本，反而会把额外 refill 压回 mutator。当前 case1 上，保留后台补货、去掉同步版本是更优策略。

### 5.4 “更主动后台 refill” 两轮实验结果

在 5.3 的 noprefetch 基线之上，进一步尝试过两种“让后台补货更主动”的做法：

1. `proactivefill`：在 miss 成功 refill 后，直接把 `cacheMiss_[cls]` 抬到阈值，并额外通知后台补货。
2. `backupconsume`：收窄为只在消费掉 `cachedRegion_[cls]` 后立即通知后台补货，不再强行抬升 `cacheMiss_`。

两轮 case1 结果都没有优于 noprefetch 基线：

- noprefetch → proactivefill：6247 ms → 6323 ms（+1.2%）
- noprefetch → backupconsume：6247 ms → 6302 ms（+0.9%）

配套指标也同步变差：

- proactivefill：GC 总时间 71.79 ms → 106.41 ms，GC P95 4.97 ms → 12.78 ms
- backupconsume：GC 总时间 71.79 ms → 86.21 ms，RSS 平均 80.58 MB → 89.93 MB

这说明当前 case1 上，“更主动”的问题不在于后台线程唤醒太晚，而在于唤醒条件一旦提前，就会把更多 region 提前拉入 fill/sweep/保留集合，反而抬高 CPU、GC 与 RSS 成本。换句话说，**后台 refill 不能只靠更频繁通知来提速；如果 ready region 的生产成本本身不够低，提前补货只会把成本前置，而不会把成本消掉。**

因此当前代码已经回退到 5.3 的 noprefetch 版本，后续不再沿“单纯提前通知后台补货”这条线继续放大。

### 5.5 “部分存活 region 批量清零” 与 “后台 fresh-region 预热” 的实验结果

围绕 4.2 和 4.4，还额外做过两组更直接的实现尝试：

1. **runzero**：把部分存活 region 的清零从“每个 free slot 一次 `memset`”改成“按连续 free run 合并后批量 `memset`”。
2. **runzero + prefill**：在 1 的基础上，进一步让 `BackgroundFillCache()` 在 `GetRegion()` 失败后继续走 `GetFreshRegion()`，尝试把 fresh region 的 `InitForSizeClass` 搬到后台 helper 线程。

从实现上看，这两条线对应的落点分别是：

- `MSRegionInfo::SweepRegion()`：把 free bitmap 中连续的 1-bit run 合并成一次大块清零。
- `AllocBufferMS::BackgroundFillCache()`：后台 miss 达阈值后，不仅拿现成 region，还允许直接申请 fresh region 并提前完成 `InitForSizeClass`。

但 case1 的单轮结果都没有优于 noprefetch 基线：

- noprefetch → runzero：6247 ms → 6300 ms（+0.8%）
- noprefetch → runzero_prefill：6247 ms → 6387 ms（+2.2%）

配套指标说明得更清楚：

- runzero：GC 总时间 71.79 ms → 99.70 ms，GC P95 4.97 ms → 10.99 ms
- runzero_prefill：GC 总时间 71.79 ms → 103.18 ms，GC P95 4.97 ms → 11.42 ms

这说明两点：

1. **部分存活 region 的问题不只是 `memset` 调用次数太多。** 即便把逐 slot 清零合并成连续 run 批量清零，case1 上也没有转成吞吐收益，说明真正的主成本更可能仍在 sweep 期间的 bitmap 遍历、region 管理、以及 free bytes 总量本身，而不是函数调用粒度。
2. **把 `InitForSizeClass` 提前到后台线程，也不能单独解决问题。** `GetFreshRegion()` 虽然能把初始化时间从 mutator miss 挪走，但它同时也会更早地扩大 ready region / committed region 供应，最终在 case1 上把 GC 时间和总时间一起抬高。

因此当前代码没有保留这两项实验；现阶段更稳妥的方向仍然是：

- 保留“完全空 region 整块清零”的 fast path；
- 不再继续放大 fresh-region 后台预热；
- 后续如果还要继续做 4.2/4.4，对象应该从“减少 zero bytes 总量 / 降低 region 准备成本”入手，而不是只改清零粒度或把现有成本前移到后台。

### 5.6 “减少分配快路径里的 TLS / Mutator 重复取数” 的实验结果

针对 4.7，已在当前代码里先做了一版最小收敛：

- `RegionSpaceMS::TryAllocateOnceMS()` 里只取一次 `ThreadLocalData*`
- 用同一个 `local` 同时拿 `msAllocBuffer` 和 `mutator`
- `AllocBufferMS::Allocate()` 支持直接复用上层传入的 `Mutator*`，避免再次走 `Mutator::GetMutator()`

也就是说，这次改动的目标不是改变 allocator 结构，而只是把：

```text
GetThreadLocalData()
-> GetOrCreateAllocBufferMS()
-> GetMutator()
-> GetThreadLocalData()
```

收敛成一次 thread-local 取数，再把已有的 `mutator` 往下传。

case1_baseline 的两轮验证结果如下：

- noprefetch 基线：6247 ms
- tlsmutatorfastpath 第 1 轮：6322 ms
- tlsmutatorfastpath 第 2 轮：6035 ms

配套指标也呈现同样特征：

- 第 1 轮：GC 总时间 105.41 ms，明显劣于基线 71.79 ms
- 第 2 轮：GC 总时间 78.07 ms，接近基线并且总时间优于基线

这说明两点：

1. **这类 fast-path 小改动的单次收益量级并不大，已经落入 case1 单轮波动区间。**
2. **从代码结构上它是合理的“去重复取数”，但仅靠一到两轮 benchmark 还不足以下结论说它稳定提升了吞吐。**

2026-04-23 的最新 seed5 sample 进一步给了这项改动一个更明确的定性信号：

- 旧画像中的 `Mutator::GetMutator` / `ThreadLocal::GetThreadLocalData` 顶层热点已经明显收缩
- 新画像里还能看到 `MRT_UpdateThreadLocalDataPhase` 和少量 `_tlv_get_addr`，说明 TLS 相关成本没有消失，但已经退居分配、Barrier、TypeInfo 之后
- 这和代码改动的预期一致：它更像是在“去掉一层固定税”，而不是把分配主链整体改写掉

因此当前更准确的结论应当是：

- 这项修改已经把分配主链里一部分重复 TLS / Mutator 取数消掉；
- 语义上与原路径兼容，构建也已通过；
- 从 sample 角度看，方向是对的，因为热点形态已经发生迁移；
- 但从 case1 单轮数据看，收益量级仍不大，后续需要用多轮平均或更细粒度 profile 再确认。

### 5.7 “后台 alloc-cache 补货线程接线修正” 与 “后台 fresh fallback” 的实验结果

围绕 4.1.4 和 4.5，最近又补做了两轮更聚焦的实验：

1. **rewire-fill-thread**：把 `SlowAlloc()` 在 `cacheMiss_[cls] >= threshold` 时发出的 `NotifyToFeedAllocBuffers()`，从 `FinalizerProcessor` 改到真正会执行 `BackgroundFillAllocCaches()` 的 `BackgroundProcessor`。
2. **rewire-fill-thread + bg-fresh-fallback**：在 1 的基础上，进一步让 `BackgroundFillCache()` 在 `GetRegion(cls)` 失败时继续尝试 `GetFreshRegion(cls)`，希望把 fresh region 的准备也搬到后台线程。

这两轮的结论都是否定的。

以当时同口径 case1 基线 5243.2 ms 为参照：

- rewire-fill-thread：5330.6 ms（+87.4 ms，约 +1.67%）
- rewire-fill-thread + bg-fresh-fallback：5335.8 ms（+92.6 ms，约 +1.77%）

其中第二轮 5 次结果为：

- 5452 ms
- 5425 ms
- 5260 ms
- 5255 ms
- 5287 ms

这说明两点：

1. **把后台补货线程接线修正到 `BackgroundProcessor` 是正确性修复，但它本身没有在 case1 上转化成稳定吞吐收益。** 也就是说，后台补货机制之前确实“发错了线程”，但修正后并没有出现“cachedRegion 一旦真被后台补，就明显提速”的现象。
2. **继续给后台补货增加 fresh-region fallback 也没有解决问题。** 这说明 case1 当前的主矛盾不是“后台线程拿不到 fresh region”，而更接近：
  - mutator miss 时 `TakeRegion` / `MaybeNotify*` / `InitForSizeClass` 这条控制面本身偏重；
  - 后台提前生产 ready region 只是把一部分成本前移，并没有把总成本消掉；
  - 一旦后台更早消耗 released tree / bump 区资源，甚至可能轻微放大与 mutator 的竞争。

因此当前代码状态保留的是：

- **保留补货线程接线修正**：`NotifyToFeedAllocBuffers()` 仍发往 `BackgroundProcessor`；
- **回退后台 fresh fallback**：`BackgroundFillCache()` 仍只消费 `GetRegion(cls)` 的 ready/reuse 路径，不再额外调用 `GetFreshRegion(cls)`。

这轮实验的价值在于把一条常见直觉排除了：**当前 case1 上，问题已经不是“后台补货线程没有真正工作”或“后台补货拿不到 fresh region”。**

### 5.8 “成功分配后固定税削减” 的实验结果

围绕 4.7，又进一步做了一轮更直接的 fast-path 瘦身，目标是把 non-marking 常态分配尽量压成 “alloc and return”，避免每次成功分配后再去碰全局 heap phase。

这轮修改包含 4 个层面：

1. 在 `ThreadLocalData` 中增加 **TLS GC fast state**，缓存当前线程是否处于 `MS_MARKING_ACTIVE` / `MS_ASSIST_ACTIVE`。
2. 在 `ThreadLocalData` 中增加 **encoded GC phase byte**，缓存写回 barrier fast path 所需的高 8 位 phase 编码。
3. 在 `Mutator::SetMutatorPhase()` / `TransitionToGCPhaseExclusive()` 上同步刷新上述 TLS 状态，使相位切换时一次性更新，而不是在热路径上现查 heap。
4. 把 `AllocBufferMS::Allocate()` 和 `MRT_UpdateThreadLocalDataPhase()` 从“每次读取 `Heap::GetHeap().GetGCPhase()` / `IsGcStarted()`”改成“直接读取 TLS 缓存”。

也就是说，当前 non-marking 小对象成功分配后的路径从：

```text
FastAlloc success
-> Heap::GetHeap().GetGCPhase()
-> 判断是否 marking / assist
-> return
```

收敛成了更接近：

```text
FastAlloc success
-> ThreadLocal::GetGCFastState()
-> 非 marking 直接 return
```

case1 的 5 次验证结果如下：

- 5027 ms
- 5131 ms
- 5031 ms
- 5132 ms
- 5000 ms

均值为：

- **5064.2 ms**

对比历史基线 5243.2 ms：

- **-179.0 ms，约 -3.41%**

这轮结果说明：

1. **成功分配后的固定税确实是真热点，而且已经到了能在 case1 上稳定体现收益的量级。**
2. **相比继续围绕后台补货做文章，当前更值得优先压的是 allocator success path 和 TLS/barrier phase 编码路径上的全局 phase 查询。**
3. **4.7 的结论需要更新。** 之前 4.7 里提到的 `MRT_UpdateThreadLocalDataPhase` 与少量 TLS phase 刷新成本，已经不再只是“还可继续压一点”的次级点，而是已经被证明：只要把它们从全局 heap 查询改成 TLS fast state，就能在 case1 上换来明确的吞吐改善。

从这轮实验之后，4.7 更准确的表述应当是：

- 顶层 TLS 取数热点已明显收缩；
- `MRT_UpdateThreadLocalDataPhase` 的全局 phase 查询固定税已进一步被削掉；
- 当前 remaining 的 allocator 热点正在从“成功分配后的 phase 判断”继续转向 `SlowAlloc` miss 链、`MaybeNotify*` 控制面、以及 `GetRegion/TakeRegion` 相关路径。

### 5.8.1 在 5.8 基础上的两处 TLS 小收敛（seed5 复验）

在 5.8 的 TLS GC fast state / encoded phase 基础上，2026-05-02 又补做了两处更小的固定税削减，目标不是再引入新的缓存层，而是把当前热路径里已经拿到的 `ThreadLocalData*` 继续复用到底：

1. `RegionSpaceMS::TryAllocateRegularObjectOnce()` 直接把已有的 `ThreadLocalData* local` 下传给 `AllocBufferMS::Allocate()`。
2. `AllocBufferMS::Allocate()` 直接读取 `local->gcFastState` 与 `local->mutator`，避免成功分配后再做一次 `ThreadLocal::GetGCFastState()` / `Mutator::GetMutator()`。
3. `MRT_UpdateThreadLocalDataPhase()` 直接从传入地址解出的 `ThreadLocalData` 读取 `encodedGCPhase`，不再额外调用 `ThreadLocal::GetEncodedGCPhase()`。

这组改动在 `case1_baseline + seed5` 上做了 5 次复验，结果如下：

- 6043 ms
- 5993 ms
- 5961 ms
- 6068 ms
- 6016 ms

均值为：

- **6016.2 ms**

波动范围为：

- **5961 ~ 6068 ms**（spread 107 ms）

对比之前两组 seed5 口径：

- 对比原始 seed5 基线 6596 ms：**-579.8 ms，约 -8.79%**
- 对比 reclaim-only 版 6605 ms：**-588.8 ms，约 -8.91%**

配套指标均值为：

- RSS 平均：**23.72 MB**
- RSS 峰值：**47.01 MB**
- GC 次数：**234.2**
- GC 总时间：**700.14 ms**

这组结果说明：

1. **这两处 TLS 小改在 seed5 压力参数下是稳定正收益，不是单次幸运值。** 5 次结果全部落在 6.0s 左右，且全部明显优于 6596/6605 ms 这一档旧结果。
2. **它们并不是“5000ms 级别”收益的主要来源。** 真正把 case1 常态均值压到 5064.2 ms 的关键，是 5.8 里那一轮更大的“成功分配后固定税削减”：把 allocator success path 和 `MRT_UpdateThreadLocalDataPhase()` 上的全局 heap phase 查询改成了 TLS fast state / encoded phase 缓存。
3. **这两处补丁更像是 5.8 的后续收口。** 也就是在 TLS fast state 方向已经成立之后，继续把剩余的 `_tlv_get_addr` / TLS 取数薄片从分配主链上再削掉一点；在 seed5 这种高频 allocation pressure 下，这类小固定税会被放大，因此可以转化成稳定收益。

2026-05-02 又针对当前代码补抓了一份新样本 [sample_seed5_tls_local_faststate.txt](../.tmp/profile_case1_seed5_tls_local_faststate_20260502/sample_seed5_tls_local_faststate.txt)，用来和旧样本 [sample_seed5_msgc.txt](../Result/case1_baseline/analysis/20260422_seed5_msgc_profile/sample_seed5_msgc.txt) 以及 2026-04-23 的“当前代码”样本 [sample_seed5_msgc_current.txt](../.tmp/profile_case1_seed5_current_20260423_hotspot2/sample_seed5_msgc_current.txt) 做对照。新的结论可以收敛成三点：

1. **热点确实继续从“顶层 TLS 取数”转向了 Barrier / `memcpy_s` 与 TypeInfo 辅助路径。** 在 2026-04-22 的旧样本里，`Mutator::GetMutator` / `ThreadLocal::GetThreadLocalData` / `_tlv_get_addr` 还是最显眼的一簇；到 2026-05-02 的新样本里，最宽的用户态热点已经稳定落在 `Barrier::WriteGeneric` / `WriteStruct` / `CJ_MCC_AssignGeneric` 下的 `memcpy_s`，以及 `TypeInfoManager::GetOrCreateTypeInfo`、`TypeInfo::GetMethodOuterTI`。
2. **TLS 成本没有消失，但已经从“主导热点”退成了残余固定税。** 新样本末尾汇总里，`_tlv_get_addr` 仍有 77 个 sample、`Mutator::GetMutator` 有 9 个、`MRT_UpdateThreadLocalDataPhase` 有 6 个；但同一份样本里 `memcpy_s` 已有 36 个 sample，`TypeInfo::GetMethodOuterTI` 有 16 个，而且在主调用链里更常直接挂在容器/泛型 workload 下，说明 TLS 相关成本还在，却不再决定主要火焰宽度。
3. **`SlowAlloc` miss 链仍然可见，但这次没有进一步上升成主热点。** 新样本里还能看到 `AllocBufferMS::SlowAlloc`，也能看到少量 `GetFreshRegion` / `MaybeNotifyMark` / `MSRegionInfo::SweepRegion`；但相较 2026-04-23 样本里更成串的 `SlowAlloc -> GetRegion -> SweepRegion`，这次 miss 链更像偶发高成本尾巴，而不是已经压过 Barrier/TypeInfo 的第一热点。

因此，当前更准确的表述应该是：

- **是，热点已经进一步从 TLS 取数转向 Barrier 的 `memcpy_s` 与 TypeInfo 辅助路径；**
- **否，它没有进一步转成以 `SlowAlloc` miss 链为主。** miss 链依然值得优化，但在这份最新 seed5 sample 里，它更像 secondary tail，而不是当前最宽的 mutator 主热点。

### 5.9 “GC 通知责任下沉到后台 refill pass” 的实验结果

围绕 4.1.4，2026-05-02 又做了一轮更直接的落地尝试：

1. **全量下沉版**：把 `TakeRegion()` / `TakeRegionForLarge()` / `PushEmptySweptRegion()` 上的 `MaybeNotifyMark()` / `MaybeNotifyReclaim()` 改成只记录 hint，由 `BackgroundFillAllocCaches()` 在 gc-helper 线程统一 drain。
2. **reclaim-only 版**：保留 `MaybeNotifyMark()` 在 fresh-miss 路径上的直接触发，只把 `MaybeNotifyReclaim()` 改成 hint + 后台 drain。

case1 + seed5 的结果非常明确：

- **全量下沉版**：6450 ms，RSS 平均 542.22 MB，RSS 峰值 1182.09 MB，GC 仅 5 次
- **reclaim-only 版**：6605 ms，RSS 平均 20.41 MB，RSS 峰值 46.41 MB，GC 227 次
- **改动前 seed5 基线**：6596 ms，RSS 平均 19.30 MB，RSS 峰值 45.34 MB，GC 261 次

这组结果说明两点（对应当时那版实现）：

1. **`MaybeNotifyMark()` 不能简单从 fresh-miss 路径整体下沉到后台 refill pass。**
  case1 + seed5 上一旦把 mark 启发式也延后，GC 触发时机会明显变晚，直接表现为 GC 次数骤降和 RSS 暴涨。
2. **`MaybeNotifyReclaim()` 下沉到后台线程是安全的，但收益有限。**
  只保留 reclaim-only 版本后，functional 与 RSS/GC 形态都回到了正常区间，但 benchmark 总时间没有出现新的稳定收益。

因此那轮实验之后保留的状态曾经是：

- **保留 reclaim-only 下沉**：`TakeRegion()` / `TakeRegionForLarge()` / `PushEmptySweptRegion()` 只记录 reclaim pressure，由 `BackgroundFillAllocCaches()` 在后台 drain。
- **不保留 mark 全量下沉**：`MaybeNotifyMark()` 仍留在 fresh-miss 路径，避免丢失当下的 alloc pressure / ready-region 压力语义。

注意：以上结论是该阶段实验记录，不一定反映当前最新代码。若当前实现已改为把分配路径的 mark HEU 统一下沉到 `TakeRegions()` 并收缩为 threshold-only，则应以源码为准理解。

这轮实验把 4.1.4 的结论进一步收窄成了一个更可执行的版本：

- mutator 侧的**reclaim** 策略判断可以继续往后台搬；
- mutator 侧的 **mark** 启发式如果要继续下沉，前提是必须保留“fresh miss 当下的压力快照”，不能只靠后台事后观察池状态来代替。

---

## 6. 剩余优化优先级

结合当前 profile 证据，建议后续按以下顺序推进：

### 优先级 1：继续压分配主链本身，而不是只前移后台补货时机

目标：减少 `RegionSpaceMS::Allocate` / `TryAllocateOnceMS` / `AllocBufferMS::Allocate` 这一整条主链上的固定成本，包括 thread-local 读取、refill 判定和 miss 后的附带工作。

优先理由：

- 最新 sample 里最宽的仍然是分配主链本身
- 4.7 对应的“成功分配后固定税”已经通过 TLS GC fast state / encoded phase 优化进一步压薄，并在 case1 上带来约 3.41% 收益
- 因此当前这条线的重点已经从“继续压顶层 TLS/phase 查询”部分转向：`SlowAlloc` miss 链、`GetRegion/TakeRegion` 控制面，以及 miss 后附带的 `MaybeNotify*` / `InitForSizeClass`
- 这条线一旦继续变瘦，small alloc、large alloc 和 mixed workload 都会同时受益

这里的“压分配主链”需要特别包含对 `SlowAlloc` miss 链的处理，而不是只看 fast path 指令数。更具体地说：

- fast path 优化解决的是“每次分配固定税”
- `SlowAlloc` / `GetRegion` / lazy sweep 优化解决的是“用尽 active region 后的突刺延迟”

从当前 case1 画像看，这两部分都要做，但后一部分更接近 MSGC 与 CopyGC 在吞吐上拉开差距的结构性来源。

### 优先级 2：压 Barrier/AssignGeneric 路径上的 memcpy_s 成本

目标：围绕 `Barrier::WriteGeneric` / `WriteStruct` / `CJ_MCC_AssignGeneric` 评估小尺寸固定长度 copy fast path，减少 `memcpy_s` / `memmove` 在高频泛型容器操作中的占比。

优先理由：

- 当前 seed5 实样里，`memcpy_s` 已经成为最突出的非分配类热点之一
- 这部分成本直接落在 mutator 主线程，不依赖 GC 阶段
- Step7 虽然覆盖了泛型运行时辅助，但拷贝路径本身仍需要在 Step6 里作为 mutator 吞吐问题记录下来

### 优先级 3：继续压 TypeInfo / 方法外层类型解析的辅助开销

目标：继续减少 `TypeInfoManager::GetOrCreateTypeInfo` 与 `TypeInfo::GetMethodOuterTI` 的命中成本，优先评估把 thread-local last-hit cache 再贴近 `ThreadLocalData`，以及是否能进一步缩短 `GetMethodOuterTI` miss 路径。

优先理由：

- 当前样本里 `GetOrCreateTypeInfo` 仍然稳定出现，并且内部还会带出 `_tlv_get_addr`
- `GetMethodOuterTI` 在 checksum 路径里已形成稳定热点，不再只是边缘开销
- 这类辅助成本在高频泛型代码里会和分配成本叠加

### 优先级 4：让更多 InitForSizeClass 搬到后台预热，但不再扩大 fresh-region 预热范围

目标：把 empty/fresh region 的 size class 绑定从 mutator 路径挪到后台填充路径。

优先理由：

- 本质上是在降低 ready region 的生产成本，而不是只提前消费它
- 能同时改善 small alloc 和 fragmentation 场景
- 5.7 / 5.5 已经说明：单纯把 `GetFreshRegion()` 提前到后台线程，并不会自动转化成吞吐收益；如果继续做这一项，重点应放在“降低初始化/准备成本”，而不是扩大后台提前占有 fresh region 的范围

### 优先级 5：继续优化部分存活 region 的清零策略

目标：把“按 free bit 逐 slot memset”改成更粗粒度的批量清零，例如按连续 free run 合并。

优先理由：

- 当前 bulk clear 只覆盖全空 region
- P3/P4/P5 的大量 region 更接近“部分活 + 大量空洞”

### 优先级 6：增强后台 sweep / fill 的前瞻性，但只做“降低生产成本型”改造

目标：不是简单更早唤醒后台，而是让 ready region 更便宜地产生出来，例如减少后台 fill 时的 `InitForSizeClass` 成本、让后台 sweep 更稳地把 unswept 转成 ready。

优先理由：

- 两轮 case1 实验已经证明，“单纯更频繁通知”会回归
- 只有当 ready region 生产成本下降后，前瞻式 refill 才可能真正转化成吞吐收益

### 优先级 7：单独评估 large alloc 路径的 inline reclaim

目标：确认 `InlineReclaimForBalance()` 是否在 P2 中造成明显同步开销。

优先理由：

- 当前 sample 中已能看到 large-object path，但占比还未超过 lazy sweep 主链
- 适合在前四项之后单独做针对性实验

---

## 7. 结论

当前 MSGC 的主要问题已经从“GC 停顿过长”转移为“mutator 主路径负担过重”。

最核心的结构性矛盾是：

> **后台 sweep / cache fill 还没有足够快地把可用 region 准备好，同时 mutator 自身的分配、泛型辅助与结构体拷贝路径也仍然偏厚，因此高频分配下会被多类固定成本共同拖慢。**

因此 Step 6 的核心目标不是继续压缩 STW，而是：

1. 让分配主链本身更瘦，继续减少 fast path 上的固定税
2. 让 mutator 更少进入 SlowAlloc
3. 让 SlowAlloc 更少触发同步 GetRegion / lazy sweep
4. 让泛型辅助与结构体拷贝路径的固定成本下降
5. 让 refill 后处理（zero / init / reclaim）尽可能前移到后台

只要这几条线持续推进，MSGC 的吞吐差距才有机会继续向 CopyGC 收敛。
