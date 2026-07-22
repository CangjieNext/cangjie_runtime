# Step31：MSGC Go 风格 Sweep Assist 设计

> 2026-06-24
> 目标：对照 Go runtime 的 proportional sweep / sweep assist 机制，结合当前 MSGC 代码结构，给出一版适合落到现有 runtime 上的实现方案。
> 口径：**以当前代码为准**。本文不讨论“理想化重写”，只讨论如何在现有 `MSCentralRegionPool + MSRegionManager + AllocBufferMS + BackgroundProcessor` 结构上增量实现。

## 代码同步说明（2026-06-27）

当前已落地代码是本文方案的**保守第一阶段**，阅读时请先记住这几点：

1. 触发点已经落在两个 fresh path 上：
   - 小对象：`AllocBufferMS::RefillFromCentral()` 在 `GetFreshRegion()` 前调用 `AssistSweepForFreshRegions(1, cls, this)`；
   - 大对象：`MSRegionManager::TakeRegionForLarge()` 在 `TakeRegions()` 前调用 `AssistSweepForFreshRegions(regionCount, ..., owner)`。
2. 已经存在独立 assist 原语：`CentralRegionPool::AssistSweepForFreshRegions()` / `AssistSweep()` / `SweepOneUnsweptRegionForAssist()`。
3. empty region 的 assist 结果会优先尝试回当前 `preferredOwner->localEmptyRegionCache_`，失败后才回全局 empty swept cache。
4. **当前代码还没有完整实现本文后文描述的 `requestedRegions × ratio` proportional assist**：
   - `requestedRegions` 目前只用于 0 值快速返回；
   - 当前补扫量由 `ComputeAssistSweepRegions()` 按 `unsweptRegions`、`remainingRunwayBytes`、`processorNum` 和 `msAssistSweepMaxRegionsPerCall` 做启发式裁剪；
   - 也没有单独持久化 `sweepAssistRatio` 状态。

因此，本文后续凡是谈到 “ratio” 或 “requestedRegions × ratio” 的段落，都应理解为**设计目标**；实际行为请以 `src/Heap/MAllocator/MSCentralRegionPool.cpp` 为准。

---

## 目录

1. [结论先行](#1-结论先行)
2. [Go 的 sweep assist 到底在做什么](#2-go-的-sweep-assist-到底在做什么)
3. [当前 MSGC 的实际状态](#3-当前-msgc-的实际状态)
4. [我们真正要补的能力](#4-我们真正要补的能力)
5. [总体设计](#5-总体设计)
6. [建议的实现挂点](#6-建议的实现挂点)
7. [建议新增的接口与职责划分](#7-建议新增的接口与职责划分)
8. [为什么不能直接复用当前 `TakeUnsweptRegion`](#8-为什么不能直接复用当前-takeunsweptregion)
9. [分阶段落地建议](#9-分阶段落地建议)
10. [风险点与正确性约束](#10-风险点与正确性约束)
11. [验证口径](#11-验证口径)
12. [一句话总结](#12-一句话总结)

---

## 1. 结论先行

如果我们也要做 Go 风格 sweep assist，建议直接采用下面这条主线：

1. **保留当前 BackgroundSweep 作为第一层并发清扫。**
2. **新增一层“按取 region 即时偿还 sweep”的 mutator assist。**
3. **assist 只挂在“拿新 region / 大对象拿 region”这类慢路径上，不碰对象级 fast path。**
4. **assist 的工作单位按 region 做，不按 object 做。**
5. **assist 不直接替代当前 lazy sweep，而是补上一层“配速驱动的主动清扫”。**

这几点里最关键的是第 5 点。

当前 MSGC 已经有：

- 后台时间片 sweep；
- mutator 走到 unswept list 时的 lazy sweep。

但这两者都还不是 Go 意义上的 proportional sweep。

Go 的核心不是“有后台 sweep”，而是：

> **每当 mutator 即将继续消耗新的可分配空间时，先检查当前 sweep 进度是否落后；如果落后，就先按当前配速补一部分 sweep。**

所以我们要补的，不是另一个 sweep 线程，而是：

> **一个跟取 region 速率绑定的 sweep 配速机制。**

---

## 2. Go 的 sweep assist 到底在做什么

Go 这边有三块要一起看：

1. `bgsweep`：后台低优先级 sweep goroutine；
2. `deductSweepCredit(spanBytes, callerSweepPages)`：分配前扣除 sweep credit；
3. `gcPaceSweeper(trigger)`：按“剩余分配预算 / 剩余待 sweep 页数”计算 sweep 配速比。

它的关键语义可以概括成：

```text
后台 sweep 尽量提前做；
如果后台没跟上，分配线程在“拿新 span / 大块页”之前补 sweep；
补多少，不是拍脑袋，而是按当前剩余 unswept / 剩余 heap runway 动态计算。
```

Go 里 sweep assist 的典型挂点有两个：

1. 小对象 refill：`mcentral.cacheSpan()` 先 `deductSweepCredit(...)`
2. 大对象分配：`mcache.allocLarge()` 先 `deductSweepCredit(...)`

也就是说，Go **没有**把 sweep assist 挂在每次对象分配 fast path 上，而是挂在：

> **span/page 级别的慢路径边界。**

这一点和当前 MSGC 的 region 模型是天然相容的。

---

## 3. 当前 MSGC 的实际状态

先把当前代码里的真实状态分清楚。

### 3.1 当前已经有后台 sweep

后台 sweep 主逻辑在：

- `src/Heap/MAllocator/MSCentralRegionPool.cpp`
  - `CentralRegionPool::BackgroundSweep`
- `src/Heap/MCollector/BackgroundProcessor.cpp`
  - `BackgroundProcessor::Run`

当前模式是：

1. `BackgroundProcessor` 按唤醒/轮询机制触发后台工作；
2. `CentralRegionPool::BackgroundSweep` 在 deadline 内扫一批 unswept region；
3. 每个 slice 的时间预算由 `GetBgSweepMaxTimeUs(...)` 控制；
4. 当前 pressure mode 已经按：

```text
unsweptBytes > heapThresholdBytes - sweptHeapBytes
```

来判定。

这说明：

> **我们已经有一个“后台尽量追赶”的 sweep 线程。**

### 3.2 当前已经有 lazy sweep

mutator 侧在：

- `src/Heap/MAllocator/MSAllocBuffer.cpp`
  - `AllocBufferMS::RefillFromCentral`
- `src/Heap/MAllocator/MSCentralRegionPool.cpp`
  - `CentralRegionPool::TakeUnsweptRegion`

当前行为是：

1. refill 时优先拿 partial swept；
2. 再尝试 empty cache；
3. 再走 `TakeUnsweptRegion(cls)`；
4. `TakeUnsweptRegion` 里真正对 unswept list 做 sweep，并直接返回可分配 region。

这意味着当前已经存在：

> **“需要 region 时，顺手把某个 unswept region 扫出来自己用”的 availability-driven lazy sweep。**

### 3.3 当前还没有 proportional sweep assist

虽然当前有 lazy sweep，但它的触发条件是：

```text
我现在缺一个 region
→ partial swept / empty cache 不够
→ 那我才去扫一个 unswept region 出来
```

这和 Go 的 proportional sweep 不是一回事。

Go 的核心触发条件是：

```text
我正准备继续消耗新的 span/page 供应
→ 先看 sweep 进度是否已经落后
→ 如果落后，就先按当前 ratio 补 sweep
→ 再继续分配
```

区别在于：

| 机制 | 当前 MSGC | Go sweep assist |
| --- | --- | --- |
| 触发条件 | region 不够用了 | sweep 进度落后于分配进度 |
| 核心目标 | 弄到一个可用 region | 保证本轮 GC 结束前 sweep 能追平 |
| 工作单位 | 取一个可用 region | 按配速补 sweep credit |
| 是否主动 | 否，偏被动 | 是，按配速主动追赶 |

### 3.4 当前最适合挂 assist 的分配边界已经存在

当前小对象/大对象的关键边界很清晰：

- 小对象：
  - `AllocBufferMS::SlowAlloc`
  - `AllocBufferMS::RefillFromCentral`
  - `CentralRegionPool::GetFreshRegion`
- 大对象：
  - `MSRegionManager::TakeRegionForLarge`
- 更底层统一 fresh supply：
  - `MSRegionManager::TakeRegions`

这意味着如果要做 sweep assist，完全没有必要侵入对象级 fast path。

---

## 4. 我们真正要补的能力

我们真正缺的是下面这件事：

> **当 mutator 即将继续申请 fresh region 供应时，先按“当前剩余 unswept backlog / 当前剩余 heap runway”计算出当前 sweep 配速，再按本次获取的 region 个数即时补 sweep。**

所以建议把目标收敛成下面三条：

1. **新增 sweep assist ratio**
   - 在 `HasPendingSweepWork()` 这类统一判断点计算
   - pressure=false 时 ratio=0
   - pressure=true 时按 `unsweptBytes / (thresholdBytes - sweptBytes)` 一类口径计算

2. **新增取 region 时的即时偿还逻辑**
   - 在 `RefillFromCentral` 或 `TakeRegionForLarge` 获取 region 时
   - 按“本次获取 region 个数 × 当前 ratio”算这次要补扫多少 region

3. **新增 assist 专用 sweep 原语**
   - 不是“取一个可用 region”
   - 而是“扫一个 unswept region，并把它正确放回 swept 体系”

这三条合起来，才是一个真正的 MSGC sweep assist。

---

## 5. 总体设计

### 5.1 设计目标

目标不是让 mutator 大量做 sweep，而是：

1. 后台 sweep 能追上时，mutator 几乎不做额外工作；
2. 后台 sweep 跟不上时，mutator 在取 fresh region 边界上按 ratio 补 sweep；
3. 不让 unswept backlog 在高并发分配下持续堆积到下一轮 GC；
4. 不破坏当前 `TakeUnsweptRegion` / `BackgroundSweep` 的稳定性。

### 5.2 配速量定义

这里不建议再单独维护一套长期累积的 debt 状态，而是：

> **在 `HasPendingSweepWork()` 中统一做两件事：**
>
> 1. 计算 `sweepPressureMode_`
> 2. 计算当前 sweep assist ratio

建议新增一个方法，例如：

```cpp
double ComputeSweepAssistRatio(size_t unsweptRegions,
                               size_t remainingRunwayRegions);
```

语义：

```text
当 pressure=false 时，ratio=0；
当 pressure=true 时，ratio 反映“当前每获取 1 个 fresh region，大致应补多少 unswept region”。
```

推荐口径：

```text
remainingRunwayBytes = max(thresholdBytes - sweptBytes, 0)
unsweptRegions = ceil(unsweptBytes / MS_REGION_SIZE)

if unsweptRegions == 0:
    pressure = false
    hardPressure = false
    ratio = 0
elif remainingRunwayBytes == 0:
    pressure = true
    hardPressure = true
    ratio = 0
else:
    remainingRunwayRegions = ceil(remainingRunwayBytes / MS_REGION_SIZE)
    pressure = (unsweptRegions > remainingRunwayRegions)
    hardPressure = false
    ratio = pressure ? (unsweptRegions / remainingRunwayRegions) : 0
```

其中：

- `unsweptBytes = totalUnsweptRegions * MS_REGION_SIZE`
- `thresholdBytes = heapThresholdBytes`
- `sweptBytes = sweptHeapBytes`
- `remainingRunwayBytes` 是真正的“剩余分配 runway”
- `remainingRunwayRegions` 是把 runway 换算到 region 粒度后的结果

这套口径和当前后台 pressure mode 完全一致，优点是：

1. 后台 sweep 与 mutator assist 共享同一套压力判断；
2. `unsweptBytes` 越大，ratio 自然越大；
3. `thresholdBytes - sweptBytes` 为负数或 0 时不会直接参与除法；
4. 可以自然区分普通 pressure 和 hard pressure；
5. 不需要再维护另一套复杂的 debt pacing 基线。

### 5.3 即时偿还口径

这里不建议做成“每次对象分配都累积 debt”，也不建议额外引入持久化的 per-owner sweep debt 字段，而是：

> **只在即将消耗 fresh region 供应时，按当前 ratio 和本次获取的 region 个数，直接算这次要补扫多少 region。**

原因：

1. 当前 MSGC 的天然供应单位就是 region；
2. 语义更直观，和 `RefillFromCentral` / `TakeRegionForLarge` 更贴合；
3. 避免再引入一套本地 debt 累积/清零语义；
4. 易于直接观测“这次取了几个 region，就顺手扫了几个 region”。

建议的计算方式：

```text
requestedRegions = 这次即将获取的 fresh region 个数

if unsweptRegions == 0:
    assistRegions = 0
elif hardPressure:
    assistRegions = kHardPressureAssistRegionsPerCall
elif ratio == 0:
    assistRegions = 0
else:
    assistRegions = ceil(requestedRegions * ratio)
```

为了避免在 ratio 很小但 pressure 已经成立时永远不触发 assist，建议这里使用：

```text
assistRegions = max(1, ceil(requestedRegions * ratio))   // 仅在 ratio > 0 时
```

同时必须做上限约束，避免一次取 region 时扫太多：

```text
assistRegions = min(assistRegions, unsweptRegions)
assistRegions = min(assistRegions, kNormalPressureAssistRegionsPerCall)
```

这样：

1. `ratio == 0` 时完全跳过；
2. 一旦进入 pressure mode，至少会对本次 fresh region 获取补 1 个 region 的 sweep；
3. `requestedRegions` 越多，assistRegions 也越大；
4. 即使 `unsweptRegions / remainingRunwayRegions` 很夸张，也不会一次扫爆。

推荐把最终逻辑收敛成：

```text
if unsweptRegions == 0:
    return 0

if hardPressure:
    return min(unsweptRegions, kHardPressureAssistRegionsPerCall)

if ratio == 0:
    return 0

assistRegions = max(1, ceil(requestedRegions * ratio))
assistRegions = min(assistRegions, unsweptRegions)
assistRegions = min(assistRegions, kNormalPressureAssistRegionsPerCall)
return assistRegions
```

其中建议：

- `kNormalPressureAssistRegionsPerCall = 1~4`
- `kHardPressureAssistRegionsPerCall = 8~16`

第一版可以先保守取：

- `kNormalPressureAssistRegionsPerCall = 4`
- `kHardPressureAssistRegionsPerCall = 16`

### 5.4 偿还方式

建议引入 assist 专用原语：

```text
AssistSweep(targetRegions, preferredCls)
```

语义：

1. 尝试从 unswept list 中扫若干 region；
2. 扫完后按结果回收到：
   - partial swept
   - full swept
   - empty swept cache
3. 不直接把扫出来的 region 交给当前分配路径消费；
4. 返回实际补扫的 region 数。

这样做的好处是：

> assist 的职责是“补扫 backlog”，不是“直接拿到一个分配 region”。

这里额外有一个实现约束需要明确：

> **如果 assist sweep 扫出来的是 empty region，应优先回当前本地 P 的 `localEmptyRegionCache_`，而不是一律先回 central global empty swept cache。**

原因：

1. assist sweep 的触发点本来就在当前 mutator / 当前 `AllocBufferMS` 的取 region 路径上；
2. 如果 empty region 先回本地，再次 `GetCacheRegion()` 时更容易直接命中；
3. 这样更符合“当前 P 自己补出来的 empty shell，优先服务当前 P”这个局部性目标。

需要注意的是，当前代码里：

1. 已经有本地 `localEmptyRegionCache_`；
2. 已经有“从 global empty swept cache 批量 refill 到本地”的逻辑；
3. 但**还没有一个公开方法**能把“一个指定 empty region”直接优先塞回当前 `AllocBufferMS` 的 `localEmptyRegionCache_`。

所以如果按本文落地，建议额外补一个 `AllocBufferMS` 侧入口，例如：

```cpp
void PushLocalEmptyRegion(MSRegionInfo* region);
```

该入口应负责：

1. `region->ResetToEmptyCache()`
2. 加 `localEmptyRegionLock_`
3. `localEmptyRegionCache_.Prepend(region)`
4. 维护 `EmptySweptCount` 统计的一致性

只有当本地 empty cache 不适合继续接收，或当前路径拿不到本地 `AllocBufferMS` owner 时，才退化回 central global empty swept cache。

### 5.5 preferred class 策略

assist 应优先从当前热点 size class 下手：

1. 先扫当前 `preferredCls` 的 partial unswept
2. 再扫当前 `preferredCls` 的 full unswept
3. 若不足，再扫其它 class

理由：

1. 当前 mutator 最可能马上继续要这个 class；
2. 提高“刚 assist 完，后续 refill 就直接命中 swept region”的概率；
3. 与当前 `BackgroundSweep` 的 preferred class 方向一致。

### 5.6 对大对象路径的特殊处理

大对象路径第一版不建议做额外的 caller self-credit 抵扣。

更稳妥的口径是：

1. 先拿到本次 `requestedRegions`
2. 用同一份当前 ratio 直接算 `assistRegions`
3. 仍然只做 bounded assist

也就是说，小对象和大对象都统一成：

```text
assistRegions = f(requestedRegions, currentRatio)
```

先保持同一套规则，后面如果 benchmark 证明大对象需要更特殊的抵扣语义，再单独细化。

---

## 6. 建议的实现挂点

### 6.1 小对象路径

最推荐的挂点是：

- `AllocBufferMS::RefillFromCentral`
  - 在决定走 `GetFreshRegion` 之前

建议顺序改成：

```text
TakePartialSweptRegion
→ GetCacheRegion
→ TakeUnsweptRegion
→ 如果还没有，则在 GetFreshRegion 之前按当前 ratio 计算 assistRegions
→ 先 AssistSweep(assistRegions, cls)
→ 再 GetFreshRegion
```

这样做的意义是：

1. 已有 swept/empty/unswept 资源先吃掉；
2. 只有当系统真的要继续向 `MSRegionManager` 要 fresh region 时，才做 proportional assist；
3. 不会干扰当前 lazy sweep 的直接收益。

### 6.2 大对象路径

最推荐的挂点是：

- `MSRegionManager::TakeRegionForLarge`
  - 在 `TakeRegions(regionCount, ...)` 之前
  - 按这次 `regionCount` 和当前 ratio 计算 assistRegions

原因：

1. 大对象天然就是 region 粒度；
2. 大对象更容易快速推高 fresh supply 消耗；
3. 这个挂点和 Go 的 `allocLarge` 最像。

### 6.3 统一底层挂点

如果后续希望统一 small/large 两条路径，可考虑在：

- `MSRegionManager::TakeRegions`

前面增加一个“仅对 fresh supply 生效”的 hook。

但不建议第一版直接放到底层统一入口。

原因：

1. `TakeRegions` 还承担 mark/scavenge/shrink 通知；
2. 这里离 size class 语义已经远了；
3. 第一版更适合保留 `preferredCls` 信息，而不是过早抽象。

结论：

> 第一版优先在 `RefillFromCentral(...)->GetFreshRegion` 和 `TakeRegionForLarge` 两个点做，并且都按“requestedRegions × currentRatio”即时算 assistRegions。

---

## 7. 建议新增的接口与职责划分

下面给一版更贴近当前代码结构的接口建议。

### 7.1 `CentralRegionPool`：新增 assist 原语

建议新增：

```cpp
size_t AssistSweep(size_t targetRegions, uint8_t preferredCls);
size_t AssistSweepClass(size_t targetRegions, uint8_t cls);
bool SweepOneUnsweptRegionForAssist(SizeClassBucket& bucket,
                                    SizeClassBucket::UnsweptListKind kind,
                                    uint32_t sg,
                                    size_t* emptied);
```

职责：

1. 负责真正从 unswept list 拿 region；
2. 负责 CAS 抢 sweep ownership；
3. 调 `region->SweepRegion()`；
4. 调整 `sweepGen`；
5. 根据结果把 region 放回 swept 体系；
   - 若 sweep 后变成 empty region，优先尝试回当前本地 `AllocBufferMS::localEmptyRegionCache_`
   - 否则再回 central 对应 swept list / empty swept cache
6. 返回实际完成的 sweep 数。

这个原语应该尽量复用当前 `BackgroundSweep / SweepUnsweptList / TakeUnsweptRegion` 的 sweep ownership 协议，而不是另起一套。

### 7.2 `CentralRegionPool`：新增 ratio 查询/偿还入口

建议新增：

```cpp
double ComputeSweepAssistRatio(size_t unsweptRegions,
                               size_t remainingRunwayRegions) const;

size_t ComputeAssistSweepRegions(size_t requestedRegions,
                                 size_t unsweptRegions,
                                 bool hardPressure,
                                 double ratio) const;

size_t AssistSweepForFreshRegions(size_t requestedRegions,
                                  uint8_t preferredCls,
                                  size_t unsweptRegions,
                                  bool hardPressure,
                                  double ratio);
```

职责：

1. 根据当前 unswept backlog 和剩余 runway 计算普通 pressure / hard pressure；
2. 在普通 pressure 下计算 ratio；
3. 根据 `requestedRegions`、`unsweptRegions`、pressure 状态和 ratio 计算本次 `assistRegions`；
4. 对本次 assist 做 bounded clamp；
5. 返回本次 assist 实际补扫量。

### 7.3 `RegionSpaceMS`：提供现成统计口径

`RegionSpaceMS` 现在已经有：

- `GetSweptHeapBytes()`
- `GetHeapThresholdBytes()`

所以第一版不需要再新增新的 heap-level metric。

如果后续要做更细的配速，也可以再补：

```cpp
size_t GetUnsweptHeapBytes() const;
```

但第一版其实用 `GetTotalUnsweptCount() * MS_REGION_SIZE` 就够了。

### 7.4 `AllocBufferMS`：小对象 fresh path 前偿还

职责：

1. 只在 `RefillFromCentral` 即将走 `GetFreshRegion(cls, ...)` 前执行；
2. 从统一维护的 sweep state 中拿到当前 `unsweptRegions / pressure / hardPressure / ratio`；
3. 以 `requestedRegions = 1` 计算 `assistRegions`；
4. 调 `central_->AssistSweepForFreshRegions(1, cls, unsweptRegions, hardPressure, ratio)`。

补充约束：

1. 这段逻辑所在的位置最清楚“当前 owner 就是哪个 `AllocBufferMS`”；
2. 因此 assist sweep 若产出 empty region，这里是最适合把 empty region 优先回填到当前 `localEmptyRegionCache_` 的位置；
3. 文档建议 assist API 保留把“当前 empty region 回填目标 owner”传下去的能力，而不是在 central 层完全丢失 owner 信息。

### 7.5 `MSRegionManager`：大对象 fresh path 前偿还

职责：

1. 在 `TakeRegionForLarge` 内部、`TakeRegions(regionCount, ...)` 前触发；
2. 取当前 `unsweptRegions / pressure / hardPressure / ratio`；
3. 以 `requestedRegions = regionCount` 计算 `assistRegions`；
4. 调 central pool 补扫。

补充约束：

1. 如果大对象路径也能拿到当前 `AllocBufferMS` owner，assist sweep 产出的 empty region 同样应优先回该 owner 的 `localEmptyRegionCache_`；
2. 若当前路径确实没有合适的本地 owner，再退化回 central global empty swept cache。

### 7.6 `HasPendingSweepWork` 的职责调整

建议把 sweep assist ratio 的计算放在和 pressure mode 同一个判断点。

也就是说，`HasPendingSweepWork()` 除了返回“当前是否还有待 sweep 工作”，还负责：

1. 计算 `unsweptBytes`
2. 计算 `remainingRunwayBytes = max(thresholdBytes - sweptBytes, 0)`
3. 计算 `unsweptRegions`
4. 更新 `sweepPressureMode_`
5. 更新 `hardPressure`
6. 更新当前 `sweepAssistRatio`

推荐语义：

```text
if allocator/pool 不存在:
    sweepPressureMode_ = false
    hardPressure = false
    sweepAssistRatio = 0

if !allocator.HasPendingSweepWork():
    sweepPressureMode_ = false
    hardPressure = false
    sweepAssistRatio = 0

remainingRunwayBytes = max(thresholdBytes - sweptBytes, 0)
unsweptRegions = ceil(unsweptBytes / MS_REGION_SIZE)

if unsweptRegions == 0:
    sweepPressureMode_ = false
    hardPressure = false
    sweepAssistRatio = 0
elif remainingRunwayBytes == 0:
    sweepPressureMode_ = true
    hardPressure = true
    sweepAssistRatio = 0
else:
    remainingRunwayRegions = ceil(remainingRunwayBytes / MS_REGION_SIZE)
    isPressure = (unsweptRegions > remainingRunwayRegions)
    sweepPressureMode_ = isPressure
    hardPressure = false
    sweepAssistRatio = isPressure ? (unsweptRegions / remainingRunwayRegions) : 0
```

这样 mutator 侧不需要自己再单独推导一套 ratio，只消费当前后台已经算好的结果即可。

### 7.7 环境变量建议

第一版建议加一个总开关，便于快速回退：

```text
CJ_MS_SWEEP_ASSIST=0/1
```

可选地再加一个上限：

```text
cjGCSweepAssistMaxRegionsPerCall
```

但不建议一开始就暴露过多参数。

第一版最好只保留：

1. enable/disable 开关
2. 单次最大 assist region 数

这样更容易看清机制本身是否有效。

> 代码现状（2026-07-08）：
> 当前 runtime 源码已经落地独立开关：
> `cjMSSweepAssist`（环境变量路径）以及 `GCParam.msSweepAssist`
>（`InitCJRuntime` 输入路径，`0=default(on), 1=on, 2=off`）。
> `cjMSAssistSweepMaxRegionsPerCall` 仍只负责单次 assist 上限，不负责开关。

---

## 8. 为什么不能直接复用当前 `TakeUnsweptRegion`

这点必须单独说清楚。

表面上看，当前已经有：

```cpp
CentralRegionPool::TakeUnsweptRegion(cls)
```

似乎只要在 fresh path 前多调几次它，就等价于 sweep assist。

但实际上不建议这样做，原因有 4 个。

### 8.1 语义不对

`TakeUnsweptRegion` 的语义是：

> 扫一个 unswept region，然后把它**直接作为分配结果返回**。

而 assist 的语义应该是：

> 扫一部分 unswept backlog，**按本次取 region 的规模补扫后再继续正常分配决策**。

两者不是一个抽象层次。

### 8.2 容易破坏当前 refill 策略

当前 refill 顺序里：

1. partial swept
2. empty cache
3. unswept
4. fresh

如果为了 assist 提前多调几次 `TakeUnsweptRegion`，很容易把“按 ratio 补扫”和“当前真的需要一个 region”混在一起，导致：

1. refill 决策复杂化；
2. 更难分析为什么这次 region 来源变了；
3. 后续难以区分“因为缺 region 做的 lazy sweep”和“因为 pacing 做的 assist sweep”。

### 8.3 无法自然统计 assist 成本

如果 assist 只是“多调几次 `TakeUnsweptRegion`”，那么：

1. 无法区分 lazy sweep 次数和 assist sweep 次数；
2. 无法单独观测 assist 对吞吐/延迟的影响；
3. 难以给出 case27 这类 benchmark 的解释。

### 8.4 不利于后续扩展

后续如果要做：

1. assist budget；
2. preferred class 扩散；
3. assist metrics；
4. caller self-credit；

都需要 assist 是一个独立原语，而不是把 `TakeUnsweptRegion` 强行拿来复用。

结论：

> `TakeUnsweptRegion` 可以复用其中的 sweep ownership / CAS / list 迁移逻辑，但不应直接作为 assist 的对外语义。

---

## 9. 分阶段落地建议

### 9.1 第一阶段：最小可落地版

目标：

> 只补“fresh region 前按 ratio 即时补扫的 proportional sweep assist”，不动现有 lazy sweep 和 background sweep 主流程。

内容：

1. 新增 assist 开关
2. 新增 `CentralRegionPool::AssistSweep(...)`
3. 在 `HasPendingSweepWork()` 中统一计算 pressure、hardPressure 与 ratio
4. 小对象在 `GetFreshRegion` 前按 `requestedRegions=1` 触发
5. 大对象在 `TakeRegionForLarge` 前按 `requestedRegions=regionCount` 触发
6. 打点统计 assist 次数、assist swept regions、assist elapsed

这阶段的优点是：

1. 改动集中；
2. 回退容易；
3. 最能回答“这套机制本身有没有收益”。

### 9.2 第二阶段：统一 pressure 口径

把当前后台 pressure mode 和 assist pacing 的判断口径尽量统一：

```text
remainingUnsweptBytes
vs
remainingAllocBudgetBytes
```

这样后台 sweep 和 mutator assist 不会各用一套标准。

可理解为：

- 后台 sweep：低优先级主动追
- assist sweep：后台没追上时，分配线程按 bounded per-call 方式补差额

### 9.3 第三阶段：再考虑更细的优化

例如：

1. large alloc caller self-credit
2. assist budget 动态上限
3. 更细的 preferred class 扩散策略
4. 与 empty swept cache / local empty cache 的更细耦合

这些都不建议在第一版一起做。

---

## 10. 风险点与正确性约束

### 10.1 必须复用现有 sweep ownership 协议

当前 `TakeUnsweptRegion` 和 `BackgroundSweep` 已经有：

1. `sweepGen` 协议
2. CAS 抢占 sweep ownership
3. `stopSweepRequested_`
4. 与 `StopAndWaitForAllSweep` 之间的同步约束

assist 绝不能绕开这套协议。

否则很容易重新引入：

- 和 stop-sweep 并发冲突；
- 在清 mark bitmap 时并发 sweep；
- stale region state。

### 10.2 assist 不要碰 fast path

绝对不建议把 assist 逻辑挂到：

- `AllocBufferMS::FastAlloc`
- bump-pointer inline path

原因很简单：

1. fast path 是吞吐命根子；
2. assist 的自然粒度就是 region；
3. 小对象每次分配都做 assist 检查，成本远大于收益。

### 10.3 assist 要有单次上限

哪怕按配速应该还更多债，也要给单次 assist 加上限。

否则在某些高 backlog 时，mutator 一次 fresh region 就可能卡太久。

建议第一版直接设一个固定上限，比如：

```text
普通 pressure：单次 assist 最多 sweep 1~4 个 region
hard pressure：单次 assist 最多 sweep 8~16 个 region
```

先保守，再根据 benchmark 调。

### 10.4 assist 结束后 region 必须回到“正确列表”

assist sweep 完的 region 不能只是“扫过了”就算完。

必须像当前正常 sweep 一样，按结果回到：

1. empty swept cache
2. partial swept
3. full swept

否则会出现：

1. 指标上看 backlog 下降了；
2. 实际可分配供应体系没接住；
3. 反而造成额外碎片或统计失真。

### 10.5 empty region 的本地回填优先级

如果 assist sweep 扫出的是 completely free / empty region，建议遵守下面的优先级：

1. **优先回当前本地 P 的 `AllocBufferMS::localEmptyRegionCache_`**
2. 本地不适合接收时，再回 central global empty swept cache

原因：

1. 这样最符合当前 assist 的触发位置，因为 assist 本来就是在当前 P 取 fresh region 时触发的；
2. 后续 `GetCacheRegion()` 更容易直接命中本地 empty shell，减少再次访问全局 central；
3. 有利于保留局部性，避免 assist 刚扫出来的 empty region 立刻被别的 P 抢走。

实现时要特别注意：

1. 当前代码虽然有本地 `localEmptyRegionCache_`，但没有现成公开 API 把一个 empty region 直接塞回本地；
2. 因此需要补一个 `AllocBufferMS` 侧入口，而不是简单复用 central 的 `PushEmptySweptRegion`；
3. 该入口必须同时维护 `EmptySweptCount` 统计一致性。

### 10.6 第一版不要和 lazy sweep 语义混合

建议维持下面的边界：

- lazy sweep：为了拿到一个当前急需的 region
- sweep assist：为了按当前 pacing ratio 补扫 backlog

这两层在实现里可以共用底层 sweep helper，但对外语义和统计最好分开。

---

## 11. 验证口径

如果后面按本文实现，建议至少看下面几组指标。

### 11.1 功能正确性

1. 不引入死锁 / 卡死 / stop-sweep race
2. `case1` 这类稳定性回归不过线
3. `gc.log` 中 assist sweep 次数与 unswept backlog 变化趋势一致

### 11.2 机制是否真的生效

重点观察：

1. `remaining unswept` 是否更早下降
2. `BackgroundSweep` 是否更少在高压阶段拖到下一轮
3. `GetFreshRegion` / `TakeRegionForLarge` 前 assist 是否真实触发
4. `remainingRunwayBytes <= 0` 时是否正确进入 hard pressure 而不是发生异常除法
5. ratio 为 0 时是否完全跳过
6. ratio 变大时 assistRegions 是否同步增大
7. assistRegions 是否被单次上限正确截断

### 11.3 对 case27 的观测重点

以 `case27_http_complex_json` 这类高并发高分配场景为主，看：

1. `c8 / c100 / c1000`
2. `p10K`
3. `heapgrow=0.5`
4. `ScavengeGrowth=10`

重点比较：

1. 吞吐是否更稳
2. `ConcurrentSweep` backlog 是否减轻
3. GC 周期后半段是否更少堆积 unswept region
4. RSS 是否更平滑

### 11.4 建议新增统计

第一版最好直接补 3 组统计：

1. `assistSweepCalls`
2. `assistSweepRegions`
3. `assistSweepElapsedUs`

再加 2 组对照统计会更好：

1. `lazySweepRegions`
2. `backgroundSweepRegions`

这样后面一眼就能看出来：

> 这次收益到底来自 background sweep、lazy sweep，还是新加的 assist sweep。

---

## 12. 一句话总结

如果要把 Go 的 sweep assist 思路引到当前 MSGC，最合适的做法不是“照搬 Go”，也不是“多调几次 `TakeUnsweptRegion`”，而是：

> **在当前 `BackgroundSweep + lazy sweep` 之上，再补一层“由 `HasPendingSweepWork()` 统一计算 pressure、hardPressure 和 ratio，并在 fresh region 获取前按 `requestedRegions × ratio` 做 bounded assist sweep”的机制。**

最小可落地版本只需要抓住两个点：

1. `RefillFromCentral(...)->GetFreshRegion` 之前
2. `TakeRegionForLarge(...)` 之前

先把这两个点跑通，再决定是否继续把 pacing、pressure mode 和统计体系完全统一。
