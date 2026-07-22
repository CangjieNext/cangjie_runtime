# Step 22：超 64P 并发 Region 获取与中央池回收路径并发化设计

> 2026-06-09 设计稿（实现前评审用）
> 目标：支持 **64P 以上**并发 mutator 同时构建对象时稳定进行 `region` 获取，
> 避免线程在 `TryTakeRegions()` 与中央池伴生回收锁上长时间串行阻塞。

---

## 目录

- [Step 22：超 64P 并发 Region 获取与中央池回收路径并发化设计](#step-22超-64p-并发-region-获取与中央池回收路径并发化设计)
  - [目录](#目录)
  - [1. 背景与目标](#1-背景与目标)
  - [2. 本轮聚焦的两块热点](#2-本轮聚焦的两块热点)
  - [3. 现状一：`TryTakeRegions()` 路径](#3-现状一-trytakeregions-路径)
  - [4. 现状二：中央池伴生锁与回收路径](#4-现状二-中央池伴生锁与回收路径)
  - [5. 根因归纳：为什么当前实现难以支撑超 64P](#5-根因归纳为什么当前实现难以支撑超-64p)
  - [6. 总体优化原则](#6-总体优化原则)
  - [7. 方案 A：`TryTakeRegions()` 并发化重构](#7-方案-a-trytakeregions-并发化重构)
  - [8. 方案 B：中央池伴生锁与回收路径并发化重构](#8-方案-b-中央池伴生锁与回收路径并发化重构)
  - [9. 推荐落地顺序（分阶段）](#9-推荐落地顺序分阶段)
  - [10. 观测指标与验证方法](#10-观测指标与验证方法)
  - [11. 风险与取舍](#11-风险与取舍)
  - [12. 最终建议](#12-最终建议)

---

## 1. 背景与目标

在最近一轮 `case25` / `case27` 排查中，已经确认 mutator 的前台等待里，有两类 runtime 内部热点会直接影响吞吐：

1. `AllocBufferMS::SlowAlloc -> GetFreshRegion -> TryTakeRegions()` 这一条 `region` 补货慢路径；
2. 围绕中央池空 region / full region / reclaim 的伴生锁路径：
   `ReclaimRegionsReleased / PushEmptySweptRegion / TakeEmptySweptRegion / ReturnFullRegion`。

在 `case27` 的 MSGC sample 中，top-of-stack 聚合直接显示：

- `MapleRuntime::FreeManager::TryTakeRegions(...)`：`241`
- `MapleRuntime::FreeManager::ReclaimRegionsReleased(...)`：`75`
- `MapleRuntime::CentralRegionPool::PushEmptySweptRegion(...)`：`45`
- `MapleRuntime::CentralRegionPool::TakeEmptySweptRegion(...)`：`24`
- `MapleRuntime::CentralRegionPool::ReturnFullRegion(...)`：`22`

这说明：

- 问题已经不是单纯“GC thread 有没有跑满”；
- 而是 mutator 在并发构建对象时，前台会掉进共享的 region 获取与回收路径；
- 当并发度继续向 `64P+` 拉高时，这些共享锁会越来越像串行瓶颈。

本 Step 的目标不是只做微调，而是明确一条能支撑 **超 64P 并发 region 获取**的演进路线：

1. 让普通对象构建尽量停留在本地 / 分片本地，不要频繁进入全局 page-heap；
2. 让中央池的空 region / full region 回流与 reclaim 从 mutator 热路径解耦；
3. 让 `region` 的“获取”和“回收”都以批量、分片、异步为主，而不是单 region、单全局锁、同步回收。

## 2. 本轮聚焦的两块热点

这轮只聚焦两块最直接影响 mutator 并发构建对象的路径。

### 2.1 热点 A：`TryTakeRegions()` / `pageHeapMutex_`

典型 regular alloc 慢路径：

```text
AllocBufferMS::SlowAlloc
  -> CentralRegionPool::GetFreshRegion
  -> MSRegionManager::TakeRegion
  -> FreeManager::TryTakeRegions
```

典型 large alloc 路径：

```text
ObjectManager::NewKnownWidthArray
  -> RegionSpaceMS::AllocateLargeObject
  -> TryAllocateLargeObjectOnce
  -> MSRegionManager::TakeRegionForLarge
  -> FreeManager::TryTakeRegions
```

两条路径都会汇聚到 `FreeManager::TryTakeRegions()`。

### 2.2 热点 B：中央池伴生锁与回收路径

当 mutator 在慢路径上返还 / 复用 / 回收 region 时，会碰到：

```text
ReturnFullRegion
PushEmptySweptRegion
TakeEmptySweptRegion
ReclaimEmptyRegion
ReclaimRegionsReleased
```

这里的问题不只是“多了一把锁”，而是：

1. mutator 不仅负责 return，还在同步参与 reclaim 决策；
2. 空 region cache 是全局入口，容易聚到单点；
3. reclaim 最终又会重新打到 `FreeManager::ReclaimRegionsReleased()` 的 `pageHeapMutex_` 上。

## 3. 现状一：`TryTakeRegions()` 路径

当前 `FreeManager::TryTakeRegions()` 实现位于 [MSFreeManager.cpp](/Users/lipeisheng/code/cangjie/cangjie/cangjie_runtime_MSGC/runtime/src/Heap/MAllocator/MSFreeManager.cpp:190)，其核心形态是：

```cpp
std::lock_guard<std::mutex> lg(pageHeapMutex_);
if (!FindFreeRunLocked(num, idx)) { return EMPTY; }
MarkAllocatedLocked(idx, num, reusedScavenged);
if (idx <= searchIdx_) {
    searchIdx_ = FindNextFreeRegionLocked(idx + num);
}
```

这把 `pageHeapMutex_` 保护的共享状态包括：

- `allocBits_`
- `scavengedBits_`
- `chunkSummaries_`
- `freeRegionCount_`
- `scavengedRegionCount_`
- `searchIdx_`
- `committedRegionCount_ / committedUnitCount_`

也就是说，这里不是“锁住一个空闲链表头”，而是锁住了一整套 page-heap 元数据。

### 3.1 锁内扫描仍然偏重

`FindFreeRunLocked()` 虽然已经引入 `ChunkSummary`，但锁内仍然会做：

1. 按 chunk 遍历；
2. 命中候选 chunk 后继续 `probe`；
3. 反复调用 `PreciseFitFromLocked()` 做 region 级检查。

见 [MSFreeManager.cpp](/Users/lipeisheng/code/cangjie/cangjie/cangjie_runtime_MSGC/runtime/src/Heap/MAllocator/MSFreeManager.cpp:445)。

### 3.2 锁内状态更新是多趟扫描

`MarkAllocatedLocked()` 当前会：

1. `CountScavengedInRangeLocked(idx, num)`
2. `CountFreeInRangeLocked(idx, num)`
3. 再逐 region `SetBit/ClearBit`
4. 再 `UpdateChunkSummaryRangeLocked()`

见 [MSFreeManager.cpp](/Users/lipeisheng/code/cangjie/cangjie/cangjie_runtime_MSGC/runtime/src/Heap/MAllocator/MSFreeManager.cpp:507)。

这意味着：

- 同一个 span 会被重复扫描多次；
- 对 `num == 1` 的小 region 请求，这个成本显得尤其不划算。

### 3.3 分配与回收共享同一把锁

除了 `TryTakeRegions()` 之外，下列路径也会直接打到同一个 `pageHeapMutex_`：

- `ReclaimRegionsReleased()`，见 [MSFreeManager.cpp](/Users/lipeisheng/code/cangjie/cangjie/cangjie_runtime_MSGC/runtime/src/Heap/MAllocator/MSFreeManager.cpp:222)
- `GrowOneSegment()`，见 [MSFreeManager.cpp](/Users/lipeisheng/code/cangjie/cangjie/cangjie_runtime_MSGC/runtime/src/Heap/MAllocator/MSFreeManager.cpp:233)
- `ScavengeFreeRegions()` / `ScavengeFreeRegionsUpTo()`
- 各种统计函数，如 `GetHotFreeBytes()`

因此当前形态下，**分配补货、空 region 回收、物理页归还、统计读取**都可能在同一把锁上会合。

### 3.4 上层进入频率仍然偏高

虽然 `AllocBufferMS` 已经有：

- `activeRegions_[cls]`
- 单槽 `cachedRegion_[cls]`

见 [MSAllocBuffer.cpp](/Users/lipeisheng/code/cangjie/cangjie/cangjie_runtime_MSGC/runtime/src/Heap/MAllocator/MSAllocBuffer.cpp:350)，
但当前线程本地缓存仍然偏薄：

- regular alloc miss 后容易直接掉到 `GetFreshRegion()`
- large alloc 直接走 `TakeRegionForLarge()`

这导致 `TryTakeRegions()` 既是“真正的全局 page-heap 获取接口”，又被大量前台 mutator 高频调用。

## 4. 现状二：中央池伴生锁与回收路径

中央池相关热点主要集中在 [MSCentralRegionPool.cpp](/Users/lipeisheng/code/cangjie/cangjie/cangjie_runtime_MSGC/runtime/src/Heap/MAllocator/MSCentralRegionPool.cpp:422) 这一段。

### 4.1 `ReturnFullRegion()`：按 class bucket 串行回流

`ReturnFullRegion()` 当前会直接拿 `bucket.lock`，然后把 region 放回：

- `fullSwept`
- 或 `fullUnswept`

见 [MSCentralRegionPool.cpp](/Users/lipeisheng/code/cangjie/cangjie/cangjie_runtime_MSGC/runtime/src/Heap/MAllocator/MSCentralRegionPool.cpp:422)。

问题在于：

- 高频满 region 回流会在每个 size class 的 bucket 上聚集；
- 当前 swept 的 full region 本质上是“稳定节点”，但仍然走 mutex 串行。

### 4.2 `PushEmptySweptRegion()`：mutator 顺手做了 reclaim 决策

当前 `PushEmptySweptRegion()` 会：

1. `ResetToEmptyCache()`
2. 拿 `emptySweptLock_` 把 region 放进 `emptySweptCache_`
3. 立刻调用 `GetEmptySweptRegionCount()`
4. 超阈值时同步执行 `ReclaimEmptyRegion()`

见 [MSCentralRegionPool.cpp](/Users/lipeisheng/code/cangjie/cangjie/cangjie_runtime_MSGC/runtime/src/Heap/MAllocator/MSCentralRegionPool.cpp:440)。

也就是说，mutator 在返回空 region 时，不只是做 producer，还顺手承担了：

- 统计读取
- 阈值判断
- reclaim 触发

这会把“空 region 容量管理”这件本应后台化的事重新塞回前台热路径。

### 4.3 `TakeEmptySweptRegion()`：全局空 cache 单点入口

`TakeEmptySweptRegion()` 当前使用全局 `emptySweptLock_`：

1. 从 `emptySweptCache_` 取 head
2. 解锁
3. `InitForSizeClass(cls)`
4. 设置 `allocPtr/regionEnd`

见 [MSCentralRegionPool.cpp](/Users/lipeisheng/code/cangjie/cangjie/cangjie_runtime_MSGC/runtime/src/Heap/MAllocator/MSCentralRegionPool.cpp:469)。

这意味着：

- 所有 mutator 对 empty region 的复用入口都汇聚到一把锁；
- 在高并发下，空 cache 会成为新的全局会合点。

### 4.4 `ReclaimEmptyRegion()`：全局 empty cache 与全局 page-heap 再次串联

`ReclaimEmptyRegion()` 会反复：

1. 拿 `emptySweptLock_` 从 `emptySweptCache_` 取一个 region
2. 调 `msRegionManager_->ReclaimEmptyRegionToHotFree(region)`
3. 后者再调 `freeManager_.ReclaimRegionsReleased()`

见：

- [MSCentralRegionPool.cpp](/Users/lipeisheng/code/cangjie/cangjie/cangjie_runtime_MSGC/runtime/src/Heap/MAllocator/MSCentralRegionPool.cpp:505)
- [MSRegionManager.cpp](/Users/lipeisheng/code/cangjie/cangjie/cangjie_runtime_MSGC/runtime/src/Heap/MAllocator/MSRegionManager.cpp:471)
- [MSFreeManager.cpp](/Users/lipeisheng/code/cangjie/cangjie/cangjie_runtime_MSGC/runtime/src/Heap/MAllocator/MSFreeManager.cpp:222)

它的问题在于：

- 不是 batch reclaim，而是单个 region 循环 reclaim；
- 最后还是回到 `pageHeapMutex_`；
- 于是中央池锁与 page-heap 锁之间形成串联放大。

### 4.5 `InlineReclaimForBalance()` 仍让 mutator 主动下场 reclaim

large alloc 路径里当前还有：

```text
TakeRegionForLarge
  -> InlineReclaimForBalance
  -> TakeEmptySweptRegionForReclaim
  -> ReclaimEmptyRegionToHotFree
  -> ReclaimRegionsReleased
```

见 [MSRegionManager.cpp](/Users/lipeisheng/code/cangjie/cangjie/cangjie_runtime_MSGC/runtime/src/Heap/MAllocator/MSRegionManager.cpp:497)。

这意味着 large object 分配不只会打 `TryTakeRegions()`，还可能顺手去打 `emptySweptLock_` 和 `pageHeapMutex_`，进一步拉长 mutator 前台尾延迟。

## 5. 根因归纳：为什么当前实现难以支撑超 64P

把上面两块热点合在一起看，当前实现难以支撑 `64P+` 的根因可以归纳为 5 条。

### 5.1 全局共享状态过重

`FreeManager` 不是一个轻量 free-list，而是一整套 page-heap 元数据中心。只要还保持“一份全局 bitmap + 一把全局 `pageHeapMutex_`”，并发度足够高时就一定会撞墙。

### 5.2 前台 mutator 进入全局慢路径的频率太高

现在的线程本地缓存层级不足以把大部分 `region` 获取吸收在本地，因此 mutator 很容易直接进入：

- `TryTakeRegions()`
- `TakeEmptySweptRegion()`
- `PushEmptySweptRegion()`

这不适合高 P 数。

### 5.3 “return / reclaim / policy” 没有解耦

当前 mutator return 一个 empty region 时，会顺手做：

- empty cache count
- threshold 判定
- reclaim 触发

这本质上把 policy 带进了热路径。

### 5.4 reclaim 粒度太细

无论是 `TryTakeRegions()` 还是 `ReclaimEmptyRegion()`，现在都大量以单 region 甚至 `num == 1` 的粒度工作，导致：

- 锁获取次数高
- 每次锁内工作量小但固定成本大
- 无法靠 batch 摊薄共享锁开销

### 5.5 当前结构只适合“中等并发”，不适合“分片并行”

如果目标只是 `8P~16P`，单锁 + 本地缓存薄优化还能撑一段时间；但如果目标明确是 **超 64P 并发 region 获取**，最终必须转向：

- 分片所有权
- 批量搬运
- 背景异步回收
- 前台尽量无锁 / 少锁

仅靠在现有全局锁模型上微调扫描逻辑，无法从根上解决问题。

## 6. 总体优化原则

围绕上面的根因，后续设计应遵守以下原则。

### 6.1 热路径本地化

普通对象构建时，优先命中：

- 当前 `active region`
- 本线程 / 本 P 的 `region magazine`
- 本 shard 的局部空 region / fresh region

不要让 mutator 频繁直接触碰全局 page-heap。

### 6.2 全局页堆只做“批量供给”

`FreeManager` 不应继续充当前台每次 miss 都来敲一次的服务台，而应退化成：

- 按 batch 给 shard / P 补货；
- 按 batch 回收连续 span；
- 偶发 grow / shrink；
- 统计读取尽量走 atomic snapshot。

### 6.3 return 与 reclaim 解耦

mutator 的责任应尽量退化成：

- 把 region 交回某个本地/分片队列；
- 打一个轻量信号；

真正的 reclaim / page return / threshold 策略交给后台线程。

### 6.4 队列与缓存都要分片

只要还是全局：

- `emptySweptCache`
- `fullSwept/fullUnswept`
- `pageHeap`

就还会继续形成高并发会合点。

因此 region 相关缓存和队列要优先按：

- P
- size class
- shard / arena

做局部化。

### 6.5 单次开销不够，必须看“总碰撞次数”

优化不能只盯“每次 `TryTakeRegions()` 更快一点”，而要优先压：

- 进入 `TryTakeRegions()` 的次数
- 进入 `ReclaimRegionsReleased()` 的次数
- 进入 `emptySweptLock_` 的次数

在 `64P+` 下，降低共享锁碰撞次数通常比压单次函数几条指令更重要。

## 7. 方案 A：`TryTakeRegions()` 并发化重构

### 7.1 结论先行

如果目标只是把当前实现再快一点，那么可以只做局部优化；  
如果目标是**支持超 64P 并发 region 获取**，那么仅优化 `TryTakeRegions()` 函数体本身不够，最终必须同时做到：

1. **减少进入 `TryTakeRegions()` 的次数**
2. **让 `TryTakeRegions()` 从“全局单实例”变成“分片多实例”**
3. **让 page-heap 的交互改成 batch，而不是单 region**

### 7.2 第一层：先减少进入频率

这是最先应该做、收益也最稳定的一层。

#### 方案 A1：把单槽 `cachedRegion_` 升级为小型 magazine

当前 `AllocBufferMS` 只有：

- `activeRegions_[cls]`
- 单槽 `cachedRegion_[cls]`

建议改成：

- 每个 class 持有 `2~4` 个 cached region 的小型本地 magazine；
- `SlowAlloc` miss 时先消费本地 magazine；
- magazine 低水位后再触发后台 fill 或向 shard 批量申请。

收益：

- 最多能把多次 `SlowAlloc -> GetFreshRegion -> TryTakeRegions()` 合并成一次批量 refill；
- 对 `num == 1` 的 regular alloc 提升最直接。

代价：

- RSS 会略升；
- 本地缓存 region 数变多后，可能增加一点碎片和局部不均衡。

#### 方案 A2：把 page-heap 获取改成 batch refill

当前 `TakeRegion()` 本质上是按 `1 region` 获取，large alloc 则按对象大小取 span。

建议改成：

- regular alloc 不直接请求 `1 region`；
- 而是 `TryTakeRegions(batchN)`，比如 `8` / `16` 个 region；
- 其中 `1` 个给当前 `active region`，其余放入本地 magazine / per-P spare。

这样 page-heap 锁的成本能摊薄到多次对象构建上。

### 7.3 第二层：把 `FreeManager` 从单全局改成 shard / arena

这是能否真正支撑 `64P+` 的决定性一层。

#### 方案 A3：`FreeManager` 分片

当前 `FreeManager` 维护的是：

- 一份 `allocBits_`
- 一份 `scavengedBits_`
- 一份 `chunkSummaries_`
- 一把 `pageHeapMutex_`

建议演进为：

- `N` 个 `FreeManagerShard`
- 每个 shard 独立持有：
  - 自己的 `allocBits_ / scavengedBits_ / chunkSummaries_`
  - 自己的 `pageHeapMutex_`
  - 自己的 `searchIdx_`
  - 自己的 free/scavenged 统计

然后按 `P id` 或 `mutator home processor` 做优先绑定：

```text
mutator / processor
  -> preferred shard
  -> local shard batch refill
  -> local shard miss 时 steal 其他 shard
  -> 所有 shard 都空时再 grow
```

这样 `64P+` 时的并发模型会从：

```text
64+ threads -> 1 pageHeapMutex_
```

变成：

```text
64+ threads -> many shards -> many pageHeapMutex_
```

这是从“优化单点”变成“去掉单点”。

#### 方案 A4：large alloc 单独走 shard / arena 路径

large object / large array 路径不应继续和 small regular alloc 完全共用一个 refill 热点。

建议：

- 大对象仍可共用 shard 基础设施；
- 但逻辑上单独维护 large-span 空间或 large-span 子池；
- 避免大对象高峰把 small object 的 page-heap 获取全挤住。

### 7.4 第三层：优化 shard 内部 `TryTakeRegions()`

在已经分片的前提下，再做函数体内部优化才最值。

#### 方案 A5：`num == 1` 做 word-level fast path

当前 `FindFreeRunLocked()` 对 `num == 1` 仍会走 chunk summary + probe + precise fit。

建议：

- 为 `num == 1` 增加专门路径；
- 直接按 `uint64_t` word 查找第一个 free bit（`ctz(~word)` 类思路）；
- 命中后直接标记，不再走通用 run 查找逻辑。

因为 regular alloc 的主流补货请求本来就是 `1 region`。

#### 方案 A6：合并锁内多趟扫描

当前 `MarkAllocatedLocked()` 至少会多趟遍历同一个区间。

建议：

- 把 `CountScavengedInRangeLocked`
- `CountFreeInRangeLocked`
- 位图更新
- chunk summary 增量维护

合并成一次按 word/span 的锁内扫描。

目标不是让逻辑更复杂，而是让锁内只摸一次同一片元数据。

#### 方案 A7：把 chunk summary 改成增量维护

当前 `UpdateChunkSummaryLocked()` 会整 chunk 重算。

建议：

- 对单点分配 / 单段回收做局部增量维护；
- 只有在跨 chunk 或复杂 merge/split 时回退到重算。

这样可以进一步压短 `pageHeapMutex_` 持有时间。

### 7.5 第四层：为回收提供 batch API

为了和方案 B 配合，`FreeManager` 还应新增：

- `ReclaimRegionsReleasedBatch(spans)`
- `TryTakeRegionsBatch(...)`

让 page-heap 看到的是：

- 一次回收多个连续 span
- 一次补多个 region

而不是一连串 `num == 1` 的细粒度操作。

## 8. 方案 B：中央池伴生锁与回收路径并发化重构

### 8.1 结论先行

中央池这块真正的问题不是“锁太多”，而是**mutator 在 return region 时承担了过多额外职责**。

要把它改成适合高并发的结构，核心是：

1. `return` 变成纯 producer；
2. `empty cache` 分片；
3. reclaim 从同步变异步、从单个变批量；
4. 统计从上锁 `Count()` 变成 atomic snapshot。

### 8.2 `PushEmptySweptRegion()` 改成纯 producer

当前 `PushEmptySweptRegion()` 最不该做的事情，是在 mutator 热路径里同步跑：

- count
- threshold check
- reclaim

建议改成：

```text
PushEmptySweptRegion
  -> ResetToEmptyCache
  -> push to local/shard empty cache
  -> atomic emptyCount++
  -> if overflow: set reclaimPending + notify helper
  -> return
```

也就是说：

- mutator 只负责“把 region 放回去”
- reclaim 的 policy 与执行都交给后台

这样可以直接消掉一大段前台伴生工作。

### 8.3 `emptySweptCache` 分片

当前 `TakeEmptySweptRegion()` / `PushEmptySweptRegion()` 共享一把全局 `emptySweptLock_`。

建议改成：

- `N` 个 empty cache shard
- 每个 shard 有自己的队列和计数
- mutator 优先访问：
  - 本 P shard
  - 本 arena/shard 绑定 cache
- miss 时再 steal 其他 shard

这样会把：

```text
many mutators -> 1 emptySweptLock_
```

拆成：

```text
many mutators -> many empty cache shards
```

对于 `TakeEmptySweptRegion()`，这通常比继续优化全局单锁更有效。

### 8.4 reclaim 由 helper 批量 drain

建议把当前：

```text
pop one region
  -> ReclaimRegionsReleased(1)
repeat
```

改成：

1. helper 一次从一个或多个 empty cache shard 里 drain 一批 region；
2. 暂存在本地 vector；
3. 按 `regionIdx` 排序；
4. 合并连续 span；
5. 一次调用 `ReclaimRegionsReleasedBatch(spans)`。

收益：

- 减少 `emptySweptLock_` 获取次数；
- 减少 `pageHeapMutex_` 获取次数；
- 回收路径更适合高并发下的后台吞吐。

### 8.5 `ReturnFullRegion()` 分拆为快慢两类

当前 `ReturnFullRegion()` 不区分是否为“当前 swept generation 的稳定 full region”。

建议：

- `fullSwept` 走 lock-free stack 或每 P return list；
- `fullUnswept` 保留 bucket 锁；
- GC phase 切换时再统一整理。

这样可以把最常见、最稳定的一类 full region 回流从 mutex 路径里剥离出去。

### 8.6 去掉 mutator 上的 `InlineReclaimForBalance()`

当前 large alloc 之后，mutator 还会主动做：

- `TakeEmptySweptRegionForReclaim()`
- `ReclaimRegionsReleased()`

这会把本该后台消化的 reclaim 成本重新挂回前台。

建议改成：

- 只记录 `reclaim debt`；
- 或只发 `notify` 给 helper；
- helper 按预算异步释放 empty cache region。

如果必须保留同步平衡，也应改成“低频批量平衡”，而不是“每次 large alloc 都尝试 reclaim”。

### 8.7 统计路径改成 atomic snapshot

当前：

- `GetEmptySweptRegionCount()`
- `GetLargeHotCacheRegionCount()`
- 部分 `BucketPressure`

仍然会拿锁。

建议：

- `emptySweptCount_` 维护为 atomic；
- 每 bucket 的 `partial/full/unswept` 数量维护为 atomic；
- `largeHotCacheRegionCount_` 维护为 atomic；
- 统计读取默认走 snapshot，必要时调试模式再做精确 count。

否则只是“观测压力”本身就会继续放大锁竞争。

## 9. 推荐落地顺序（分阶段）

### Phase 0：先补观测

先加最小统计，不改结构：

- `TryTakeRegions` 调用次数 / 平均 `num`
- `pageHeapMutex_` wait time / hold time
- `PushEmptySweptRegion` 调用次数
- 前台同步 reclaim 次数
- `InlineReclaimForBalance` 触发次数
- `ReclaimRegionsReleased` 平均 batch size

目标：把“哪一类共享路径最该先切”量化清楚。

### Phase 1：低风险减前台工作

先做不会改 allocator 主体拓扑的改动：

1. `PushEmptySweptRegion()` 改成异步 reclaim
2. `emptySweptCount_` / bucket count 原子化
3. `cachedRegion_` 升级为小 magazine
4. `TryTakeRegions(num==1)` fast path

这一阶段的目标是：

- 先显著降低 mutator 热路径上的同步附加工作；
- 让 sample 里 `PushEmptySweptRegion / ReclaimRegionsReleased / TryTakeRegions` 的前台命中下降。

### Phase 2：批量化接口

1. 新增 `ReclaimRegionsReleasedBatch(spans)`
2. helper 批量 drain empty cache
3. regular alloc 支持 batch refill
4. 移除 mutator 上的 `InlineReclaimForBalance()`

这一阶段的目标是：

- 降低共享锁碰撞次数；
- 把 page-heap 从“每次 1 region”推进到“每次 1 batch”。

### Phase 3：结构性并发化

1. `FreeManager` shard / arena 化
2. `emptySweptCache` shard 化
3. per-P / per-shard return queue
4. `ReturnFullRegion(fullSwept)` lock-free 化

这一阶段才是真正面向 **64P+** 的主改造。

### Phase 4：收尾优化

1. shard steal 策略
2. grow/shrink 与 shard 平衡
3. large alloc 与 regular alloc 的进一步隔离
4. 调优 batch size / shard 数量 / magazine 大小

## 10. 观测指标与验证方法

除了 benchmark wall time / throughput，还需要新增 allocator 内部指标。

### 10.1 `TryTakeRegions()` 相关

- `try_take_calls`
- `try_take_regions_total`
- `try_take_avg_batch`
- `try_take_wait_ns`
- `try_take_hold_ns`
- `grow_calls`
- `grow_wait_ns`

### 10.2 中央池与 reclaim 相关

- `empty_push_count`
- `empty_push_overflow_count`
- `empty_take_count`
- `empty_reclaim_regions`
- `empty_reclaim_batch_avg`
- `inline_reclaim_mutator_count`
- `return_full_count`
- `return_full_swept_fast_count`

### 10.3 分片后新增

- `shard_local_hit`
- `shard_steal_hit`
- `shard_grow_hit`
- `shard_imbalance_events`

### 10.4 验证 workload

至少需要 3 类验证：

1. 纯 allocator stress：
   - `64P / 96P / 128P` 并发构建对象
   - 重点看 region 获取是否还明显串行
2. `case25_http_complex_json`
3. `case27_http_complex_json`

验证重点不是只看 GC 总时间，而是看：

- mutator 在 sample 中落入 `TryTakeRegions` / `ReclaimRegionsReleased` 的比例是否下降
- `pageHeapMutex_` 的等待是否不再随 P 数近似线性恶化

## 11. 风险与取舍

### 11.1 RSS 与局部碎片会上升

更多 local magazine / shard cache 会换来更高的并发，但代价是：

- 短期 RSS 更高
- 空 region 分布更分散

这是典型的“用空间换并发”。

### 11.2 shard 不均衡需要额外治理

如果某些 P / class 远比其他热点更高，某些 shard 可能更容易被打空。

因此 shard 化之后必须补：

- steal 策略
- rebalance 策略
- 后台 refill 策略

### 11.3 lock-free / per-P 回流要严格处理 phase 语义

像 `fullSwept` / `fullUnswept` / `empty cache` 这些结构都带 sweep generation 语义，不能只为了并发把 phase 边界搞乱。

### 11.4 只做微优化无法达成最终目标

如果最终只做：

- `num==1 fast path`
- 少量锁内代码压缩

它会有收益，但**无法保证超 64P 下稳定扩展**。

因此必须接受：

- Phase 1 / 2 是止血；
- Phase 3 才是最终架构方向。

## 12. 最终建议

最终建议如下。

### 12.1 对 `TryTakeRegions()` 的判断

如果目标明确是：

> “支持超 64P 并发进行 region 获取，避免在多线程并发构建对象时阻塞在 region 获取中”

那么 `TryTakeRegions()` 不能继续维持单全局 `FreeManager` 架构。

正确方向应是：

```text
per-thread / per-P magazine
  -> per-shard FreeManager
  -> global grow/shrink only as fallback
```

### 12.2 对中央池伴生锁的判断

这块不应继续让 mutator 同步参与 reclaim 决策。

正确方向应是：

```text
mutator return = pure producer
empty cache / reclaim policy = helper async
reclaim to FreeManager = batch
```

### 12.3 推荐实施顺序

推荐按下面顺序推进：

1. `PushEmptySweptRegion` 异步化
2. local magazine + batch refill
3. `ReclaimRegionsReleasedBatch`
4. 去掉 mutator `InlineReclaimForBalance`
5. `FreeManager` shard 化
6. `emptySweptCache` shard 化

### 12.4 最终形态

最终想要的 allocator / region 获取模型应是：

```text
对象构建
  -> active region
  -> local magazine
  -> local shard refill(batch)
  -> other shard steal(batch)
  -> global grow (rare)

empty/full region 回流
  -> local/shard queue
  -> helper async drain
  -> batch reclaim to shard/page-heap
```

只有这样，mutator 在 `64P+` 下并发构建对象时，才不会持续阻塞在 `region` 获取与中央池伴生回收锁上。
