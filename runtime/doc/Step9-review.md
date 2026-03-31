# Step9 Review：当前实现复核与三层控制环说明

> 更新时间：2026-03-28
>
> 范围：基于当前 runtime 源码，对 Step9 的实际落地情况做实现复核。本文不再重复设计稿本身，而是回答 5 个问题：
> 1. 当前实现分为哪几层；
> 2. 每一层的目标是什么；
> 3. 每一层在什么时间点触发；
> 4. 为什么要这样做；
> 5. 当前实现已经做到什么，以及还能优化什么。

## 1. 结论摘要

当前 MarkSweep GC 已经基本形成了 Step9 期望的三层控制环：

1. 第一层：GC 周期驱动
   - 已经从“局部 free-tree 状态主导”收敛为“`true live + threshold` 主导，pressure 只做 fallback”。
   - 小对象和大对象都在补货边界做触发判断，而不是每次 slot 分配都判断。

2. 第二层：后台 sweep 回中央池
   - 已经把 sweep 的职责收敛为“把 unswept region 变成可复用 region”。
   - 空 region 不再回到 MSGC 的 `dirtyTree`，而是进入 `CentralRegionPool::emptySweptCache`。

3. 第三层：后台 scavenger / reclaim
   - 已经改成从 `emptySweptCache` 出发，把超出水位的空 region 释放到 `releasedTree`，再由 manager 统一管理物理页释放后的可复用段。

从实现上看，Step9 的主轴已经落地：

- `dirtyTree -> releasedTree` 已经不再是 MSGC 小对象 steady-state 的主路径；
- `emptySweptCache -> releasedTree` 已经成为第三层 reclaim 的主路径；
- `ReleaseGarbageRegions()` 在 MSGC 中已经退化为 no-op；
- 设计稿里“中央池全局无 class 空缓存”的模型已经真实实现。

仍然值得继续优化的点，主要不是“架构方向错了”，而是“参数、触发条件和节流策略还偏保守”，具体见第 6 节。

## 2. 当前实现分层

### 2.1 第一层：GC 周期驱动层

核心代码：

- `src/Heap/Collector/MarkSweepCollector.cpp`
- `src/Heap/Collector/GcStats.cpp`
- `src/Heap/Allocator/MSRegionManager.cpp`

这一层回答的问题只有一个：

> 什么时候启动下一轮 GC？

当前实现里，这一层由三部分组成：

1. `GcStats::Init()` 初始化默认 `heapThreshold`
2. `MarkSweepCollector::UpdateGCStats()` 在 GC 结束后更新下一轮 threshold
3. `MSRegionManager::MaybeTriggerHeuGC()` / `MaybeTriggerHeuGCForLarge()` 在补货边界触发 HEU GC

### 2.2 第二层：后台 sweep 层

核心代码：

- `src/Heap/Allocator/CentralRegionPool.cpp`
- `src/Heap/Collector/MarkSweepCollector.cpp`
- `src/Heap/Collector/FinalizerProcessor.cpp`
- `src/Heap/Allocator/RegionSpaceMS.cpp`

这一层回答的问题是：

> 当前 GC 结束后，如何把 unswept region 逐步变成“中央池可复用”的 region？

当前实现中，第二层的输入是 `partialUnswept/fullUnswept`，输出是：

- `partialSwept`
- `fullSwept`
- `emptySweptCache`

### 2.3 第三层：后台 reclaim / scavenger 层

核心代码：

- `src/Heap/Allocator/CentralRegionPool.cpp`
- `src/Heap/Allocator/MSRegionManager.cpp`
- `src/Heap/Allocator/RegionSpaceMS.cpp`
- `src/Heap/Collector/FinalizerProcessor.cpp`

这一层回答的问题是：

> 中央池里已经 sweep 且完全空闲的 region，什么时候应该继续归还给 OS？

当前实现中，第三层的输入是 `emptySweptCache`，输出是 `releasedTree`。

## 3. 每一层的目标、触发时机、原因与当前实现

## 3.1 第一层：GC 周期驱动

### 目标

第一层的目标是：

1. 让 GC 周期主要由真实 live heap 增长驱动；
2. 避免 allocator 局部瞬时状态把 GC 周期拖成抖动状态；
3. 把触发判断放在 refill 边界，而不是放在每次对象分配上。

### 触发时间

当前代码里，GC 周期驱动主要在以下时间点发生：

1. GC 完成后：
   - `MarkSweepCollector::UpdateGCStats()` 重新计算 `heapThreshold`
   - 同时持久化：
     - `lastTrueLiveBytes`
     - `baselineAllocatedBytes`

2. 小对象补货时：
   - `MSRegionManager::TakeRegion()`
   - 先 `MaybeNotifyBackgroundWork()`
   - 再 `MaybeTriggerHeuGC()`

3. 大对象补货时：
   - `MSRegionManager::TakeRegionForLarge()`
   - 先 `MaybeNotifyBackgroundWorkForLarge()`
   - 再 `MaybeTriggerHeuGCForLarge()`

### 为什么这么做

原因是 refill 边界比 slot 分配边界更稳定：

1. refill 频率远低于单对象分配频率；
2. 它更接近 Go 的 `mcache -> mcentral/mheap refill` 语义；
3. 它天然能过滤掉线程本地快路径里的微小抖动。

### 当前实现

当前实现已经具备以下 Step9 特征：

1. threshold 的 base 改成了 `trueLiveBytes`
   - `UpdateGCStats()` 不再把 active-region slack 直接混到 threshold base 里；
   - 而是走：

```text
liveDrivenThreshold = trueLiveBytes * heapGrowth
newThreshold = max(liveDrivenThreshold, trueLiveBytes + minHeadroom)
```

2. 正常触发条件是：

```text
effectiveAlloc = lastTrueLiveBytes + max(0, AllocatedBytes - baselineAllocatedBytes)
effectiveAlloc >= heapThreshold
```

3. pressure fallback 只在小对象 `source == BUMP` 且 supply 真正吃紧时才启用
   - `ready` 低
   - `unswept` 低
   - `empty cache` 低
   - `releasedTree` 低

4. 节流已经是双层的
   - `MSRegionManager` 本地 `HEU_REQUEST_COOLDOWN_NS = 10ms`
   - `MarkSweepCollector::Init()` 里还给 `GC_REASON_HEU` 设了 `gcInterval`

### 当前还能优化的点

1. `effectiveAlloc` 仍使用 `AllocatedBytes()`，它本质上是 active-region/committed 视角，不是纯对象分配字节数。
   - 这比旧版已经好很多，但仍然会受 region 粒度影响。

2. pressure fallback 仍然只看单个 class 的局部供给，不看全局 backlog 的“恢复速度”。
   - 也就是说，它会看“有没有”，但不太看“多久能补回来”。

3. `minHeadroom` 仍然是固定规则，尚未根据对象尺寸分布和大对象压力自适应。

## 3.2 第二层：后台 sweep 回中央池

### 目标

第二层的目标是：

1. 把 unswept region 转成中央池可复用 region；
2. 尽量降低 mutator 在分配路径上做 lazy sweep 的次数；
3. 不在 sweep 这一层直接承担 RSS 控制。

### 触发时间

当前 sweep 的主触发时间有三类：

1. GC 尾部启动
   - `MarkTermination()` 在 STW 内：
     - `FlushAllAllocBuffers()`
     - `PrepareForSweep()`
   - `ConcurrentSweep()` 再通知 `FinalizerProcessor::NotifyToSweep()`

2. 分配压力提前唤醒
   - `CentralRegionPool::GetRegion()` 在 bucket ready 过低且 unswept backlog 存在时，会 `NotifyToSweep()`
   - `MSRegionManager::MaybeNotifyBackgroundWork()` 也会做同样的唤醒

3. gc-helper 同次唤醒内继续 drain
   - `FinalizerProcessor::Run()` 中如果某个 sweep slice 返回 `pending`，同次 wake 会继续跑有限 burst

### 为什么这么做

这么做的原因是：

1. GC 尾部必须显式开启 sweep generation，让后续 sweep 有确定的输入集合；
2. 仅靠 GC 尾部一次通知不够，因为 mutator 会继续制造 bucket 级别的 ready shortage；
3. 仅做单 slice 又太保守，所以当前实现允许单次 wake 内有限 burst drain。

### 当前实现

当前实现和 Step9 的一致性已经比较高：

1. `PrepareForSweep()` 把 `partialSwept/fullSwept` 翻转为新一代的 unswept 输入
2. `BackgroundSweep()` 只做 sweep，不做 reclaim
3. sweep 后分类已经明确分为三类：
   - 部分空闲 -> `partialSwept`
   - 仍然全满 -> `fullSwept`
   - 完全空 -> `emptySweptCache`

4. `GetRegion()` 的优先级已经是：

```text
partialSwept
-> emptySweptCache
-> partialUnswept (lazy sweep fallback)
-> fullUnswept (lazy sweep fallback)
-> fresh region
```

5. 小对象空 region 已经不再回 MSGC 的 `dirtyTree`

### 当前还能优化的点

1. lazy sweep 仍然存在于 `GetRegion()` fallback 路径中，说明后台 sweep 还没有强到足以完全吃掉 steady-state backlog。

2. 当前 pressure-mode budget 是固定阈值：
   - 正常：64 regions / 2ms
   - 压力：256 regions / 8ms
   这套参数是静态的，后续可以按 backlog 深度或最近 refill miss 自适应。

3. `PrepareForSweep()` 仍然依赖 STW 下的 alloc-buffer flush，这部分成本在高并发场景下仍值得继续压缩。

## 3.3 第三层：后台 reclaim / scavenger

### 目标

第三层的目标是：

1. 控制 RSS，而不是控制对象回收；
2. 保留一部分中央池空缓存，避免刚释放就立刻重新 fault-in；
3. 只把“超过缓存水位的空 region”释放到 `releasedTree`。

### 触发时间

当前 reclaim 的触发时机主要有四类：

1. `emptySweptCache` 跨过 high watermark 时
   - `PushEmptySweptRegion()` 会设置 `backgroundScavengeActive_`
   - 并 `NotifyToReclaimGarbage()`

2. 分配路径发现当前已经存在 reclaim backlog 时
   - `MSRegionManager::MaybeNotifyBackgroundWork()`
   - `MSRegionManager::MaybeNotifyBackgroundWorkForLarge()`

3. gc-helper 在一次 sweep slice 结束后发现 reclaim backlog 仍在
   - `BackgroundSweepLoop()` 会把 `shouldReclaimHeapGarbage` 置起

4. gc-helper 主循环主动自检
   - `RegionSpaceMS::HasPendingReclaimWork()` 通过 `ShouldContinueBackgroundScavenge()` 判断是否还有 backlog

### 为什么这么做

原因是 reclaim 的目标不是“每轮 GC 后统一扫一遍”，而是：

1. 当空缓存过高时尽快启动；
2. 当同次唤醒里已经有 backlog 时尽量顺手 drain；
3. 平时没有 backlog 时保持低频等待，避免后台线程常驻高活跃。

### 当前实现

当前实现已经非常接近 Step9 设计稿：

1. reclaim 输入源已经变成 `emptySweptCache`
2. `ScavengeEmptyRegionsToReleased(false)` 使用高低水位
3. watermarks 目前按 region count 计算：
   - `low = max(P*2, 32)`，上限 256
   - `high = low * 2`
4. reclaim 的输出路径是：

```text
emptySweptCache
-> ReclaimEmptyRegionToReleased()
-> ReleaseUnits()
-> releasedTree.MergeInsert()
```

5. `RegionSpace::ReclaimGarbageMemory(false)` 的职责已经清晰：
   - 先 scavange empty cache
   - 再 `ShrinkHeap()`

6. large object 仍然保持 eager release
   - 不参与小对象 empty cache scavenger 扫描

### 当前还能优化的点

1. 水位判断目前只按 region count，不看 bytes、age、RSS 目标或 idle time。
   - 这让策略简单，但也更粗糙。

2. reclaim 每次 slice 后都会走 `ShrinkHeap()`，在 backlog 很大时可能带来额外管理开销。

3. 第三层目前没有“空缓存年龄”概念，只知道数量是否超过 high watermark，不知道哪些 region 已经空闲很久。

## 4. 与设计稿对照后的实现结论

按当前代码看，Step9 的状态可以总结为：

### 已实现

1. 三层控制环已经拆开
2. GC 周期已经由 threshold 主导，pressure 只做 fallback
3. sweep 的职责已经收敛为“回中央池”
4. `emptySweptCache` 已经真实存在，且是全局无 class 空缓存
5. reclaim 已经改成 `emptySweptCache -> releasedTree`
6. MSGC 的 `ReleaseGarbageRegions()` 已经 no-op
7. large object 仍然 eager release

### 部分实现但仍可继续优化

1. 第一层 trigger 已经分层，但触发量纲仍偏 active-region 粒度
2. 第二层仍保留 lazy sweep fallback，说明后台 sweep 吞吐还可以继续增强
3. 第三层已有高低水位，但还没有 bytes/age/RSS target 等更细粒度策略

### 已经不再成立的旧结论

以下说法对“当前代码”已经不成立：

1. “小对象空 region 还主要混在 `partialSwept` 里”
   - 现在已经是显式 `emptySweptCache`

2. “MSGC reclaim 仍以 `dirtyTree` 为核心”
   - 现在 small steady-state 路径已经改成 `emptySweptCache -> releasedTree`

3. “`ReleaseGarbageRegions()` 仍是 MSGC 的主回收入口”
   - 现在 MSGC 中它已经是 no-op

## 5. 主要问题的直接回答

### 问题 1：当前分为哪几层？

当前实现可以明确分为三层：

1. GC 周期层
2. sweep 层
3. reclaim / scavenger 层

### 问题 2：每一层的目标是什么？

1. GC 周期层：决定何时启动下一轮 GC，降低周期抖动。
2. sweep 层：把 unswept region 变成中央池可复用 region，降低 mutator lazy sweep。
3. reclaim 层：把中央池中过多的空 region 归还给 OS，稳定 RSS。

### 问题 3：每一层的触发时间是什么？

1. GC 周期层：在 `TakeRegion` / `TakeRegionForLarge` 的补货边界触发判断；在每轮 GC 结束后更新 threshold。
2. sweep 层：在 `MarkTermination + PrepareForSweep` 建立 sweep 代之后，由 `ConcurrentSweep` 和分配压力共同唤醒 gc-helper。
3. reclaim 层：在 `emptySweptCache` 超过高水位、分配路径检测到 backlog、或 sweep slice 结束后发现仍有 backlog 时触发。

### 问题 4：为什么这么做？

核心原因是把三个目标拆开：

1. GC 周期负责“何时做对象回收”
2. sweep 负责“如何把 region 变回可复用状态”
3. reclaim 负责“什么时候该把空缓存再归还给 OS”

不拆开的话，就会反复出现：

- GC 周期被 allocator 局部状态绑架
- 刚 sweep 完就 release
- 刚 release 又立刻从 OS 重新拿回来

### 问题 5：当前实现还能优化什么？

最值得继续做的点有四个：

1. 第一层：把 trigger metric 从 active-region 粒度再向“真实分配增长”收紧。
2. 第二层：继续提高后台 sweep 覆盖率，进一步压低 lazy sweep 发生次数。
3. 第三层：把 reclaim 水位从“只看 region count”扩展到“count + bytes + age + RSS target”。
4. helper 调度：继续优化 slice/burst/backoff 参数，让后台更平滑，但又不重新回到常驻忙等。

## 6. 建议的下一步

如果要继续推进 Step9，建议按优先级做：

1. 先做第三层 reclaim 水位增强
   - region count -> count + bytes + age

2. 再做第二层 sweep 预算自适应
   - backlog 深度越高，slice 越大；backlog 变浅后自动回落

3. 最后再细化第一层 trigger metric
   - 尽量减少 active-region slack 对 `effectiveAlloc` 的影响

原因是当前架构已经是对的，下一步最需要的是“参数更稳”，不是再回到旧 free-tree 模型。