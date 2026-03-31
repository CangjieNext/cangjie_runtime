# Step 9：基于三层控制环的 GC 周期、后台 Sweep 与归还 OS 详细设计

> 目标：重新定义 MS-GC 中 `GC 周期驱动`、`后台 sweep`、`归还 OS 内存` 三者的职责边界，
> 在保持内存稳定且较低的前提下，尽量减少 GC 频率、mutator 参与 sweep 的次数，以及后台线程连续运行的频率。

> 2026-03-27 设计重写说明：本版 Step9 不再以 `dirtyTree -> releasedTree` 为主设计轴心，
> 而改为更接近 Go 的三层模型：
> 1. pacer/trigger 负责 GC 周期；
> 2. 后台 sweep 负责把 region 变为中央池可复用缓存；
> 3. 后台 scavenger 负责把“中央池中长期空闲的 region”归还给 OS。

## 目录

1. [问题背景](#1-问题背景)
2. [当前实现复核](#2-当前实现复核)
3. [重设计目标](#3-重设计目标)
4. [总体设计：三层控制环](#4-总体设计三层控制环)
5. [第一层：GC 周期驱动](#5-第一层gc-周期驱动)
6. [第二层：后台 Sweep 回中央池](#6-第二层后台-sweep-回中央池)
7. [第三层：后台归还 OS](#7-第三层后台归还-os)
8. [数据结构调整](#8-数据结构调整)
9. [大对象与对象尺寸边界](#9-大对象与对象尺寸边界)
10. [与 Go 实现的对应关系](#10-与-go-实现的对应关系)
11. [端到端时序](#11-端到端时序)
12. [迁移步骤](#12-迁移步骤)
13. [风险与验证](#13-风险与验证)

## 1. 问题背景

当前 MS-GC 的问题已经不再是“没有 sweep / reclaim 机制”，而是：

1. **GC 周期控制与 allocator 内部状态耦合过深**
   - `heapThreshold` 在算，但 GC 启动并不稳定地由它主导。
   - `ready/unswept/dirty/released` 的瞬时状态容易反客为主，导致 GC 周期抖动。

2. **后台 sweep 与归还 OS 内存的职责边界不够清晰**
   - sweep 本应负责把 region 变为“可复用”；
   - scavenger/reclaim 本应负责把“长期空闲 region”归还给 OS；
   - 如果两者混在一起，容易出现“刚 sweep 完就释放”、“刚释放又重新申请”的振荡。

3. **小对象空 region 的稳定缓存语义还不够清晰**
   - 当前实现里，完全空的小 region 已经开始绕开 `dirtyTree`，直接 `ResetToBump + partialSwept`。
   - 这是正确方向，但语义还不彻底：
     - `partialSwept` 混入“部分空闲 region”和“完全空的 bump-ready region”，语义不够干净；
     - 后台归还 OS 仍然主要围绕 `dirtyTree` 设计，而不是围绕中央池中的空缓存设计。

4. **整体目标冲突**
   - 想减少 GC 频率；
   - 想让 CPU 平滑；
   - 想让 RSS 低且稳定；
   - 同时还想保留一部分 region 缓存用于性能与稳定性。

这些目标不能靠“把当前 dirtyTree/releasedTree 调慢一点”解决，必须重新拆分职责。

## 2. 当前实现复核

### 2.1 已经偏向正确方向的部分

当前代码中，`CentralRegionPool` 已经不再把空的小 region 一律送回 `dirtyTree`。

在 `ForceFinishSweep()`、`SweepAndClassify()`、`BackgroundSweep()` 里，完全空的小 region 已经走：

```text
SweepRegion()
  -> IsCompletelyFree()
  -> ResetToBump()
  -> bucket.partialSwept.Push(region)
```

这说明小 region 的“就地复用缓存”已经开始从 `MSRegionManager` 外移到 `CentralRegionPool`。

这是正确方向，因为对于小对象 steady-state：

- 复用空 region 的最佳位置是中央池；
- 不是先回收给 `dirtyTree`，再由下次分配重新取回。

### 2.2 仍然不符合目标架构的部分

当前实现仍有以下遗留：

1. `MSRegionManager` 仍然维护 `dirtyTree_ + releasedTree_` 双树，并且 reclaim 逻辑仍以 `dirtyTree` 为核心。
2. `ReleaseGarbageRegions()` 仍是 “dirtyTree -> releasedTree” 的模型。
3. `TakeRegion()` 的 HEU trigger 仍会被 allocator pressure 路径大量主导。
4. 中央池里没有明确的“完全空但已 sweep 的缓存池”语义，空 region 只是暂时混在 `partialSwept` 里。

因此，当前实现虽然已经“局部开始摆脱 dirtyTree”，但整体架构仍不一致。

### 2.3 本文档的态度

本版 Step9 不再把 `dirtyTree` 当作长期保留的核心机制，而是将其视为：

- **需要被逐步移除的旧 free-tree 语义**
- 至少在小/中对象 steady-state 路径中，应当彻底退出

换句话说：

> 本版 Step9 的目标不是“优化 dirtyTree 的策略”，而是“把小对象路径的控制平面从 dirtyTree 迁移到中央池 + releasedTree”。

## 3. 重设计目标

### 3.1 总目标

1. **保持内存稳定且较低**
2. **尽量减少 GC 频率**
3. **尽量减少后台线程连续运行频率**
4. **允许一定的 region 缓存，以加速复用并减小内存波动**

### 3.2 子目标

1. GC 周期必须由 `threshold + trigger` 主导，而不是由 free-list/空缓存的局部瞬态主导。
2. sweep 负责“把 region 变成中央池可复用状态”，不负责“马上归还 OS”。
3. 归还 OS 负责“从中央池空缓存中选一部分长期空 region 释放到 `releasedTree`”，不负责对象回收。
4. 大对象路径继续保持 eager release，不纳入小 region 后台 sweep/scavenge 扫描。

## 4. 总体设计：三层控制环

新的 Step9 采用严格分层：

### 第一层：GC 周期驱动

负责：

- `GC threshold` 的设置
- `GC` 是否启动的判断

目标：

- GC 不要频繁抖动
- 周期由 live heap 增长驱动，而不是由 allocator 局部缓存状态驱动

### 第二层：后台 Sweep 回中央池

负责：

- 后台线程逐步 sweep `partialUnswept/fullUnswept`
- 把 sweep 过的 region 归还给中央池
- 减少 mutator 在分配路径上的 lazy sweep 次数

目标：

- 减少 mutator 参与 sweep
- 不直接控制归还 OS 内存

### 第三层：后台归还 OS

负责：

- 扫描中央池中已经 sweep 且已经空闲的 region 缓存
- 将超过缓存水位的 region 释放物理页并送入 `releasedTree`
- 大对象不参与这一层扫描，因为大对象在回收时就立即释放

目标：

- 维持内存较低且稳定
- 保留适量 region 缓存，用于复用加速和内存稳定

## 5. 第一层：GC 周期驱动

### 5.1 设计原则

GC 周期控制应尽量接近 Go pacer：

```text
nextTrigger = f(lastTrueLiveBytes, heapGrowth, optional floor/cap)
```

它回答的问题只有一个：

> 什么时候应该启动下一轮 GC？

它不回答：

- 后台 sweep 要不要继续扫
- 中央池里应该保留多少空 region
- 哪些空 region 应该归还给 OS

### 5.2 触发点

触发判断放在 `TakeRegion` 边界，而不是放在每次 slot 分配上。

原因：

1. `TakeRegion` 是“小对象分配从线程本地快路径跌落到全局补货”的边界；
2. 这是最接近 Go `mcache -> mcentral/mheap refill` 的位置；
3. 以 region 粒度做 trigger 判断，可以天然过滤大量微小分配抖动。

### 5.3 正常触发条件

正常触发应是：

```text
effectiveAlloc >= heapThreshold
```

其中：

```text
effectiveAlloc = lastTrueLiveBytes + max(0, currentAllocated - baselineAllocatedBytes)
```

这表示：

- 上一轮真正存活的对象有多少；
- 从那之后又新增了多少真实分配压力。

### 5.4 压力兜底的边界

allocator pressure 只能做 fallback，不能主导日常 GC 周期。

只有在以下情况同时满足时，才允许绕过 threshold 提前触发：

1. 中央池该 size class 的 `ready` 缓存极低；
2. 中央池空缓存也低；
3. `releasedTree` 可立即复用的 region 也低；
4. 当前并不存在明显的 unswept backlog 可以通过 sweep 解决。

也就是说：

- **有 unswept backlog 时，优先让第二层 sweep 去解决；**
- **只有 sweep 也解决不了时，第一层才提早启动 GC。**

### 5.5 防抖要求

为了防止 GC 周期抖动，第一层必须具备：

1. **cooldown**
2. **最小 headroom / floor**
3. **触发条件的单向性**

目标不是让 GC 永远更晚，而是让 GC 周期可预测、可调参。

## 6. 第二层：后台 Sweep 回中央池

### 6.1 设计原则

后台 sweep 的职责只有一个：

> 把 unswept region 变成中央池中“已经 sweep、可以复用”的 region。

它不负责：

- 把空 region 立刻交给 OS；
- 控制 RSS；
- 直接触发新一轮 GC。

### 6.2 sweep 后的分类

背景 sweep 对小/中对象 region 的分类应是：

1. **仍然全满**
   - 放回 `fullSwept`

2. **部分空闲**
   - 放回 `partialSwept`

3. **完全空**
   - 不再进 `dirtyTree`
   - 进入中央池的 **空缓存池**

### 6.3 为什么不能再回 dirtyTree

如果小 region sweep 完全空后回 `dirtyTree`，就会出现：

```text
sweep -> 回 MSRegionManager -> 下次分配再从 tree 取回 -> 清零/重建
```

这会导致：

1. 中央池和外部 free-tree 之间的往返；
2. 小对象 steady-state 复用路径过长；
3. 中央池无法稳定持有自己的空缓存；
4. reclaim 机制和复用机制混在一起。

这与 Go 中的“先把 span 变成可复用，再决定是否 scavange”方向相反。

### 6.4 建议的数据结构：显式全局无 class 空缓存池

完全空的 swept region 不应继续混在 `partialSwept` 中，而应单独建模：

```text
CentralRegionPool
  per-class:
    partialSwept
    fullSwept
    partialUnswept
    fullUnswept

  global:
    emptySweptCache
```

`emptySweptCache` 的含义是：

- region 已经完全空；
- 已经脱离旧 size class；
- 尚未归还 OS；
- 可被任何 size class 重新绑定和复用。

这比把它塞进某个旧 size class 的 `partialSwept` 更合理，因为空 region 本质上已经脱离原 size class。

这一版明确采用“**全局无 class empty cache**”，不是“每个 class 各自留一份空 cache”。

这样设计的原因是：

1. 完全空 region 已经没有对象布局约束，继续保留旧 class 身份只会制造无意义耦合；
2. scavenger 只需要观察一个全局池的水位，不需要聚合各 class 的空 region；
3. 分配缺货时，可以把空 region 优先让给真正短缺的 class，而不是被旧 class 占住。

### 6.5 `emptySweptCache` 的精确定义

为了让“全局无 class”不是口号而是可实现语义，进入 `emptySweptCache` 的 region 必须满足以下不变式：

1. **对象层面已经完全空**
   - `freeSlotCount == totalSlotCount`
   - 不再包含 live object

2. **class 相关元数据已经解绑**
   - 不能再依赖旧的 `sizeClass`
   - 不能再把旧 bitmap 视为有效分配状态

3. **不再占有 class 私有资源**
   - 释放旧 `allocBitmap/markBitmap/scannedBitmap`
   - region 只保留“这是一个可复用的空 region”这一事实

4. **仍然归 CentralRegionPool 所有**
   - 它不是 `dirtyTree`
   - 也不是 `releasedTree`
   - 只是中央池内部的一种全局空缓存

换句话说，进入 `emptySweptCache` 不应调用“回收给 `MSRegionManager`”的路径，而应调用“脱离旧 class，但仍留在中央池”的路径。

### 6.6 入池路径设计

后台 sweep 或 lazy sweep 发现 region 完全空后，应走以下路径：

```text
SweepRegion()
  -> IsCompletelyFree()
  -> ReleaseBitmaps()
  -> ResetEmptyCacheMeta()
  -> Push(emptySweptCache)
```

其中 `ResetEmptyCacheMeta()` 的目标语义应是：

- `allocPtr = RegionStart()`
- `regionEnd` 保持不变
- `sizeClass = INVALID_CLASS`
- `totalSlotCount/freeSlotCount/liveSlotCount = 0`
- `allocBitmap/markBitmap = nullptr`
- `regionType = EMPTY_CACHE`
- `sweepGen = current`

这里最关键的一点是：

> **进入全局无 class empty cache 之前，必须先释放旧 class bitmap。**

否则这个 region 只是“逻辑上想脱离旧 class”，实际上仍然绑定着旧 class 的 slot 布局，后续 rebind 时语义会混乱。

### 6.7 出池路径设计

当某个 size class 缺 region 时，从 `emptySweptCache` 取出的 region 应走“重新绑定”路径，而不是直接作为 bump-ready region 返回：

```text
TakeEmptyRegionForClass(cls)
  -> Pop(emptySweptCache)
  -> InitForSizeClass(cls)
  -> SetSweepGen(current)
  -> return region
```

这一步的语义是：

- 重新为目标 class 分配 bitmap
- 重新计算 `totalSlotCount/freeSlotCount`
- 重新建立该 class 的 bump/bitmap 分配布局
- 从“全局空 region”变回“某个 class 的新鲜 region”

也就是说，`emptySweptCache` 的 region 不是“带旧 class 的热复用对象”，而是“无需向 OS 重新申请物理页的裸 region 壳”。

### 6.8 为什么要接受一次 bitmap 重建

这套设计下，`emptySweptCache` 的 rebind 需要重新 `InitForSizeClass`，看上去比“保留旧 bitmap 直接复用”多了一步。

这里的取舍是刻意的：

1. 我们优先要的是**全局无 class 语义成立**；
2. 其次要的是**第三层 scavenger 观察对象简单**；
3. 再其次才是“完全空 region 的 rebind 极致便宜”。

因为空 region 的复用已经避免了：

- `dirtyTree` 往返
- `releasedTree` fault-in
- `GrowOneSegment`

所以即使在 rebind 时重建 bitmap，整体仍然比“回 manager 再取回”更短、更稳定。

### 6.9 分配优先级

新的小对象取 region 顺序建议为：

```text
partialSwept[cls]
-> emptySweptCache
-> partialUnswept[cls]   (lazy sweep fallback)
-> fullUnswept[cls]      (lazy sweep fallback)
-> releasedTree
-> bump / GrowOneSegment
```

理由：

1. `partialSwept` 最快，因为已经是目标 size class 且还有空洞；
2. `emptySweptCache` 次之，因为不需要 OS fault-in，只需重新绑定 size class；
3. lazy sweep 仍保留，但应退回 fallback；
4. `releasedTree` 再次申请时会重新触碰物理页，应晚于空缓存；
5. bump / grow 最后。

## 7. 第三层：后台归还 OS

### 7.1 设计原则

第三层的职责是：

> 从中央池的空缓存中选择长期空闲 region，释放物理页，转入 `releasedTree`。

它不做：

- 对象回收；
- unswept 清扫；
- GC 启动决策。

### 7.2 观测对象改变

旧模型以 `dirtyTree` 为主要观测对象。

新模型中，第三层的主要观测对象应变成：

- `emptySweptCache` 的大小
- `releasedTree` 的大小
- 当前 `trueLive / activeRegion / heapGoal`

因为：

- 小 region 的“空缓存”已经不再在 `dirtyTree`；
- `dirtyTree` 对小对象 steady-state 不再有必要；
- 第三层真正关心的是“中央池里空缓存是否过多”，而不是“free-tree 里有多少脏块”。

### 7.3 高低水位

第三层仍应使用高低水位，而不是单点 target：

```text
emptyCacheLow
emptyCacheHigh
```

策略为：

- `emptyCache <= low`：不归还 OS
- `low < emptyCache < high`：保持现状
- `emptyCache >= high`：后台 slice scavenging，逐步释放到 `releasedTree`

建议第一版同时记录两类指标：

- `emptyCacheRegionCount`
- `emptyCacheBytes`

其中：

- 水位判断可以先以 `regionCount` 为主，简单直接；
- DFX 和策略调优同时输出 `bytes`，便于和 RSS、`releasedTree` 大小联动分析。

### 7.4 扫描对象

第三层只扫描：

- `emptySweptCache`

不扫描：

- `partialSwept`
- `fullSwept`
- 大对象列表

原因：

1. `partial/full swept` 中 region 仍有活对象或仍承担目标 size class 的直接复用职责；
2. 大对象在回收时就立即释放，不应再进统一小 region scavenger。

### 7.5 scavenger 扫描与输出目标

第三层后台线程每次只处理一个有限 slice，而不是一次性清空 `emptySweptCache`：

```text
ScavengeEmptyRegionsToReleased(maxRegions, maxTimeUs)
  -> while emptyCache > low and budget not exhausted
       -> Pop(emptySweptCache)
       -> ReleaseUnits(region)
       -> ResetReleasedMeta()
       -> MergeInsert(releasedTree)
```

这里的 `ResetReleasedMeta()` 语义应与 empty-cache 元数据继续区分：

- `allocBitmap/markBitmap = nullptr`
- `sizeClass = INVALID_CLASS`
- `regionType = FREE/RELEASED-ready`
- region 已经不再属于中央池缓存，而属于 manager 的 released 供给

这样第二层和第三层的职责边界就会很清楚：

- 第二层把 region 送到“中央池可复用”
- 第三层才把“中央池中过多的空 region”送回 manager / OS

### 7.6 输出目标

第三层的输出是：

```text
emptySweptCache --release pages--> releasedTree
```

也就是说，`releasedTree` 在新模型中的角色保留，但来源改变了：

- 旧模型：`dirtyTree -> releasedTree`
- 新模型：`emptySweptCache -> releasedTree`

## 8. 数据结构调整

### 8.1 `dirtyTree` 的处理

本版设计明确：

- **小/中对象 steady-state 路径移除 `dirtyTree`**
- `dirtyTree` 不再参与小对象 sweep 后空 region 的缓存与复用

这意味着：

1. `ReclaimRegion()` 不再是小对象空 region 的常规回收路径；
2. `ReleaseGarbageRegions()` 不再表示 `dirty -> released`；
3. `MSRegionManager` 在小对象路径上主要保留：
   - `releasedTree`
   - `inactiveZone`
   - `GrowOneSegment`
   - `ShrinkHeap`

### 8.2 中央池新增“空缓存”概念

建议在 `CentralRegionPool` 中新增显式空缓存池，例如：

```text
MSRegionList emptySweptCache
```

建议第一版直接补全以下成员，而不是只加一个链表名：

```text
CentralRegionPool
  MSRegionList emptySweptCache
  mutex emptySweptLock
  atomic<size_t> emptySweptRegionCount
  atomic<size_t> emptySweptBytes
```

它们分别用于：

- `emptySweptCache`: 存储真正“无 class”的空 region
- `emptySweptLock`: 保护全局空缓存链表
- `emptySweptRegionCount/Bytes`: 提供 scavenger 水位判断与 DFX

它可以继续演进为：

- 一个全局链表；或
- 若后续要限流和 locality，再演进为分 size class / 分 CPU 的空缓存池

第一版设计建议先用 **全局 empty cache**，降低语义复杂度。

### 8.3 需要新增的接口语义

为了让全局无 class `emptySweptCache` 真正可落地，建议显式引入以下接口语义：

1. `PushEmptySweptRegion(region)`
   - 输入：完全空的小/中对象 region
   - 动作：释放 bitmap，清掉 class 绑定，放入全局空缓存

2. `TakeEmptySweptRegion()`
   - 输入：无
   - 输出：一个无 class 空 region

3. `BindEmptyRegionToClass(region, cls)`
   - 输入：无 class 空 region + 目标 class
   - 动作：`InitForSizeClass(cls)`，恢复 class 分配语义

4. `ScavengeEmptyRegionsToReleased()`
   - 输入：budget + 水位
   - 动作：从全局空缓存释放到 `releasedTree`

这四个动作拆开后，第二层和第三层都不会再借用 `dirtyTree` 旧路径。

### 8.4 `ReleaseGarbageRegions` 的重定义

在新模型下，`ReleaseGarbageRegions` 这个名字会变得不准确。

更贴切的职责应该是：

```text
ScavengeEmptyRegionsToReleased()
```

即：

- 扫描中央池空缓存；
- 释放物理页；
- 移入 `releasedTree`

## 9. 大对象与对象尺寸边界

### 9.1 大对象不是 Step9 的主设计对象

本版 Step9 的主目标是重构 **小/中对象 steady-state 路径**，也就是：

- GC 周期如何驱动；
- sweep 完成后小 region 如何回到中央池；
- 中央池中的空 region 如何再归还给 OS。

从这个角度看，大对象不是 Step9 的主矛盾，原因有三：

1. 大对象不走 `CentralRegionPool` 的 size-class 分配/复用路径；
2. 大对象不参与第二层“后台 sweep 回中央池”的主设计；
3. 大对象在 GC 后仍保持 eager reclaim，回收后直接释放物理页并进入 `releasedTree`。

因此，第三层后台归还 OS 不需要扫描大对象列表。

### 9.2 但大对象并非“完全不影响全局”

虽然大对象不是 Step9 的核心设计对象，但它们仍会影响：

- `active-region bytes`
- `trueLiveBytes`
- `heapThreshold` 的演化
- 连续大块空间需求与 grow 压力

因此更准确的说法应是：

> 大对象不是 Step9 的主要优化对象，但仍会影响全局内存占用和 GC 周期统计；只是在本阶段不把它作为主控制环的中心。

### 9.3 当前代码中的对象尺寸边界

按**当前实现**，小/大对象分界不是 32KB，而是 **16KB**：

- `kLargeObjectThreshold = 16384`（16KB）
- `>= 16KB` 的对象当前走 large object 路径
- `MS_REGION_SIZE = 32KB` 是 region 大小，不是当前小对象上限

也就是说，当前实现中：

- `< 16KB`：走 size-class / `AllocBufferMS` / `CentralRegionPool`
- `16KB ~ 32KB`：已经走 large object path
- `>= 32KB`：同样走 large object path，只是占用更多 region

### 9.4 本版 Step9 明确不调整对象尺寸边界

这次 Step9 只重构“控制环与缓存语义”，**不修改 `16KB` 小/大对象边界**。

也就是说，本版实施范围内：

- `< 16KB` 仍走小对象 / 中央池路径
- `>= 16KB` 仍走 large object path
- `emptySweptCache` 只覆盖当前小对象 steady-state 路径

这样做的目的是把变更范围收敛住：

1. 先把第一层 trigger、第二层 sweep、第三层 scavenger 的职责边界做对；
2. 不把 `SizeClass` 扩容、`kLargeObjectThreshold` 调整、`16KB~32KB` 归属变更混入本次改动；
3. 先验证全局无 class `emptySweptCache` 本身是否能稳定降低 `dirtyTree` 依赖和 RSS 抖动。

### 9.5 对后续边界调整的态度

未来是否把边界上移到 32KB，可以作为独立后续专题讨论，但不再属于本版 Step9 的实施前提。

这意味着：

- Step9 完成后，仍然允许存在 `16KB~32KB` 走 large path 的现状；
- 只要三层控制环在当前边界下能稳定工作，本版目标就算达成；
- 若后续要扩到 `<= 32KB`，应单独评估 `SizeClassInfo`、`AllocBufferMS`、`CentralRegionPool`、large object tracking 的连带改动。

## 10. 与 Go 实现的对应关系

| Cangjie 新 Step9 | Go 对应概念 | 含义 |
|---|---|---|
| 第一层：`heapThreshold` + `TakeRegion` trigger | `mgcpacer` | live heap 驱动下一轮 GC |
| 第二层：`BackgroundSweep` | `mcentral` + lazy/background sweep | span/region 变成可复用 |
| 第二层输出：`partial/full/empty swept` | `mcentral` 可复用 span | 不等同于归还 OS |
| 第三层：空缓存高低水位归还 OS | `bgscavenge` | 将长期空闲 span/arena 归还 OS |
| `releasedTree` | scavenged free runs | 已归还物理页，后续可重新 fault-in |

Go 的关键启发是：

1. **GC 周期由 live heap 决定**
2. **sweep 负责复用，不负责立刻归还 OS**
3. **scavenge 单独负责 RSS**

本版 Step9 正是在把这三件事分开。

## 11. 端到端时序

新的目标时序如下：

```text
Mutator 分配
  -> 若线程本地 region 用尽，则进入 TakeRegion 边界
  -> 第一层判断是否该启动 GC

GC 完成
  -> PrepareForSweep
  -> 第二层后台逐步 sweep unswept region
  -> sweep 后 region 回中央池：
       partial/full/empty

后台归还 OS
  -> 第三层观察 empty cache 水位
  -> 若空缓存过高，逐步释放到 releasedTree

后续分配
  -> 优先 partialSwept
  -> 再 empty cache
  -> 再 lazy sweep fallback
  -> 再 releasedTree
  -> 最后 bump/grow
```

## 12. 迁移步骤

建议按以下顺序迁移，而不是一次性大改：

### 第一步：固定第一层职责

1. `heapThreshold` 计算独立收敛
2. `TakeRegion` 边界触发 threshold 主导
3. pressure 只保留 fallback

### 第二步：把空 region 明确留在中央池

1. 将完全空 region 从“混入 `partialSwept`”升级为显式 `emptySweptCache`
2. 明确“入空缓存前释放旧 bitmap、解绑旧 class”的语义
3. 小对象空 region 不再进入 `dirtyTree`

### 第三步：重写后台归还 OS

1. 移除小对象 `dirtyTree -> releasedTree` 的主路径
2. 改为 `emptySweptCache -> releasedTree`
3. 大对象保持 eager release，不参与第三层扫描

### 第四步：清理旧语义

1. 缩减或删除小对象 steady-state 对 `dirtyTree` 的依赖
2. 重新整理 `MSRegionManager` 的职责说明

## 13. 风险与验证

### 13.1 风险

1. **空缓存过大**
   - RSS 下降不明显

2. **空缓存过小**
   - 中央池频繁落到 `releasedTree` 或 grow，性能波动

3. **threshold 仍被 fallback 抢主导**
   - GC 频率仍旧抖动

4. **empty cache 语义不清**
   - 仍然把完全空 region 塞进 `partialSwept`，后续 scavenger 难以稳定控制

### 13.2 关键验证点

1. GC 次数是否下降
2. `TakeRegion` 中由 threshold 触发的比例是否显著提高
3. mutator 的 lazy sweep 次数是否下降
4. `emptySweptCache` 是否稳定在水位附近
5. `releasedTree` 是否只承担“归还 OS 后的可重用来源”
6. RSS 峰值和均值是否都更稳定

### 13.3 最终结论

本版 Step9 的核心不是“继续调 `dirtyTree` 的策略”，而是：

1. **第一层**：用 `threshold + TakeRegion` 稳定 GC 周期；
2. **第二层**：后台 sweep 只负责把 region 送回中央池；
3. **第三层**：后台 scavenger 再从中央池空缓存中选择一部分归还 OS；
4. **整体上移除小对象路径对 `dirtyTree` 的依赖。**

只有这样，才能同时实现：

- 内存稳定且较低；
- GC 频率更低；
- 后台线程执行频率更低；
- 并保留适量 region 缓存来换取性能与稳定性。
