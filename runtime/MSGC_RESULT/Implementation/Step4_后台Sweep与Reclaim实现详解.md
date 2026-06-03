# Step 4：后台 Sweep 与 Reclaim 实现详解

> 对应设计文档：[Step4.2 Sweep调度与CentralRegionPool集成](../../MSGC/Step4.2_Sweep调度与CentralRegionPool集成.md)、[Step4.3 后台Sweep与FinalizerProcessor集成](../../MSGC/Step4.3_后台Sweep与FinalizerProcessor集成.md)、[Step9 GC阈值与SweepReclaim联动](../../MSGC_RESULT/Plan/Step9_GC阈值与SweepReclaim联动详细设计.md)
>
> 2026-04-05 基于当前代码状态整理

---

## 目录

1. [总体架构：三层控制环](#1-总体架构三层控制环)
2. [角色与线程模型](#2-角色与线程模型)
3. [第一层：GC 周期驱动（HEU Trigger）](#3-第一层gc-周期驱动heu-trigger)
4. [第二层：后台 Sweep 与 Lazy Sweep](#4-第二层后台-sweep-与-lazy-sweep)
5. [第三层：后台 Scavenge 与物理回收](#5-第三层后台-scavenge-与物理回收)
6. [emptySweptCache 生命周期](#6-emptysweptcache-生命周期)
7. [环境变量调参一览](#7-环境变量调参一览)
8. [结论](#8-结论)

---

## 1. 总体架构：三层控制环

当前 MSGC 的 Sweep/Reclaim 体系采用三层控制环，各层职责严格分离：

| 层级 | 职责 | 驱动信号 | 核心代码位置 |
|------|------|----------|-------------|
| **第一层：GC 周期驱动** | 决定何时启动下一轮 GC | `effectiveAlloc >= threshold`，以及后台 sweep 耗尽时的全局压力补判 | `MSRegionManager::MaybeNotifyMark()` |
| **第二层：后台 Sweep** | 把 unswept region 变成中央池可复用 region | GC Mark 完成后 `NotifyToSweep()` | `MSCentralRegionPool::BackgroundSweep()` |
| **第三层：后台 Scavenge** | 把中央池空缓存中过多的 empty region 释放给 OS | `emptySweptCache >= highWatermark` 或 committed > target | `MSCentralRegionPool::HandleBackgroundScavenge()` |

三层之间的反馈关系：

```
                ┌──────────────────────────────────────────────────────┐
                │                  分配路径 (Mutator)                    │
                │  GetRegion → P1~P5 → GetFreshRegion → TakeRegion    │
                └────┬──────────────────────┬──────────────┬──────────┘
                     │                      │              │
              MaybeNotifySweep        MaybeNotifyReclaim   MaybeNotifyMark
                (唤醒 Layer 2)         (唤醒 Layer 3)      (触发 Layer 1)
                     │                      │              │
                     ▼                      ▼              ▼
        ┌────────────────┐    ┌─────────────────┐   ┌──────────────┐
        │ Layer 2: Sweep │    │ Layer 3: Scavenge│   │ Layer 1: GC  │
        │  bg/lazy sweep │───►│ emptyCache→OS    │   │ RequestGC()  │
        │  → partialSwept│    │ → releasedTree   │   └──────────────┘
        │  → emptyCache  │    └─────────────────┘
        └────────────────┘          │
               │                    │ (反向触发)
               │◄───────────────────┘ shouldPressureSweep
               │                        (emptyCache==0 && unswept>0 && slack high)
```

**关键设计原则**：

1. **GC 周期由 live heap 增长驱动**，不被 allocator 局部缓存状态劫持
2. **Sweep 只负责回收到中央池**，不直接归还 OS
3. **Scavenge 只从 emptySweptCache 中释放超额 region**，不涉及对象回收
4. **dirtyTree 已退出小对象 steady-state**，releasedTree 仅作为底层物理页供给/回收层

---

## 2. 角色与线程模型

### 2.1 MSGC 使用独立的 BackgroundProcessor

**代码位置**：
- [src/Heap/MCollector/BackgroundProcessor.h](../../src/Heap/MCollector/BackgroundProcessor.h)
- [src/Heap/MCollector/BackgroundProcessor.cpp](../../src/Heap/MCollector/BackgroundProcessor.cpp)
- [src/Heap/Collector/CollectorResources.h](../../src/Heap/Collector/CollectorResources.h) — `#ifdef MRT_USE_MARK_SWEEP` 路由

### 2.2 BackgroundProcessor 主循环

```
Run() → Init (创建 MSGC_BACKGROUND_THREAD Mutator) → while(running) {
    1. if hasFinalizableJob      → ProcessFinalizables()
    2. if shouldBackgroundSweep_ → BackgroundSweepLoop()
         → Allocator::BackgroundSweep() → CentralRegionPool::BackgroundSweep()
         → 若 sweep 完成且有 pendingReclaim → 设 shouldReclaimHeapGarbage
    3. if shouldFeedHungryBuffers → BackgroundFillAllocCaches()
    4. if shouldReclaimHeapGarbage → ReclaimHeapGarbage()
         → Allocator::ReclaimGarbageMemory() → HandleBackgroundScavenge()

    // Burst 控制：连续有工作时最多 4 轮 or 12ms，之后 WaitForActiveBackoff(1ms)
    // 无工作时：Wait(2000ms) on condvar
}
```

### 2.2 通知机制

| 事件 | 发送方 | 接收方 | 机制 |
|------|--------|--------|------|
| Finalizer 就绪 | `PostMarkTermination()` | BackgroundProcessor | `EnqueueFinalizables → NotifyFinalizerWork → condvar.notify_one()` |
| Sweep 启动 | `ConcurrentSweep()` | BackgroundProcessor | `MaybeNotifySweep → NotifyToSweep → shouldBackgroundSweep_=true + condvar.notify_one()` |
| Reclaim 触发 | `TakeRegions` / `PushEmptySweptRegion` | BackgroundProcessor | `MaybeNotifyReclaim → NotifyToReclaimGarbage()` |
| Sweep 停止请求 | `PreEnumRoots()` | BackgroundSweep 循环 | `stopSweepRequested_=true`，spin-wait `bgSweepRunning_==false && activeLazySweepCount_==0` |

## 3. 第一层：GC 周期驱动（HEU Trigger）

### 3.1 核心入口

**统一入口：`MSRegionManager::MaybeNotifyMark(HeuTriggerKind, centralPool, cls, bumpSource)`**

调用点：

| 调用者 | HeuTriggerKind | 时机 |
|--------|---------------|------|
| `TakeRegions()` | `TAKE_REGIONS` | 在真正探测 releasedTree / inactiveZone bump / grow 之前，统一做 threshold-only 检查 |
| `BackgroundSweep()` 结束 | `BG_SWEEP_EXHAUSTED` | 后台 sweep 在 slice 收口时确认 `unswept` 扫空后，直接上抬为一次 HEU 请求 |

### 3.2 阈值触发（主路径）

```
effectiveAlloc = lastTrueLiveBytes + max(0, currentAllocatedBytes - baselineAllocatedBytes)
thresholdTriggered = (effectiveAlloc >= gcStats.threshold)
```

- `lastTrueLiveBytes`：上轮 GC 结束后 `ComputeMarkedBytes()` 统计的真实活跃字节数
- `currentAllocatedBytes - baselineAllocatedBytes`：本轮新增分配量
- **不使用 `totalMSRegionBytes_`**：后者包含未 sweep 的死区域字节数，会导致阈值虚高

### 3.3 后台 sweep 收口触发

当前分配路径已经移除 `SMALL_ALLOC / LARGE_ALLOC` 的专属 pressure 检查，只保留 **threshold-only** 的 HEU 判断。

`BG_SWEEP_EXHAUSTED`` 路径的含义现在是：后台 sweep 把 unswept backlog 扫空后，sweep 侧如果决定要上抬到 mark，就调用 `MaybeNotifyMark(BG_SWEEP_EXHAUSTED, ...)`；而 `MaybeNotifyMark()` 进入该分支后不再做 supply 总量门槛判断，而是直接请求下一轮 GC，并记录 `swept/unswept/empty` 三类 region 数量。

进入这个分支之前，sweep 线程仍然可能遍历所有 36 个 sizeClass，观察例如下列 bucket 状态：

| 条件 | 含义 | 默认水位 |
|------|------|---------|
| `readyCount < HEU_READY_WATERMARK` | 当前 cls 可用 region 极低 | 4 |
| `unsweptCount < HEU_UNSWEPT_WATERMARK` | 可 sweep 的 region 也不够 | 16 |
| `emptySweptCount < lowWatermark` | emptyCache 也不够 | 32 |

### 3.4 冷却机制

```
 CAS lastHeuRequestNs_ → `TAKE_REGIONS` / `BG_SWEEP_EXHAUSTED` 间隔 < HEU_REQUEST_COOLDOWN (默认 10ms) 的请求被丢弃；`RECLAIM_SWEEP_EXHAUSTED` 不走这层防抖
```

防止高频分配导致重复 GC 请求。

### 3.5 三个 `MaybeNotify*` 的职责分层与调用关系

当前代码里真正的“后台控制入口”已经分成四类，不只是三个 `MaybeNotify*`：

| 入口 | 所在位置 | 解决的问题 | 直接动作 |
|------|----------|-----------|----------|
| `MaybeNotifyMark()` | `MSRegionManager` | 是否该开启下一轮 GC | `collector.RequestGC(GC_REASON_HEU, true)` |
| `MaybeNotifySweep()` | `MSRegionManager` | 是否该催后台 sweep 生产 ready region | `backgroundProcessor.NotifyToSweep()` |
| `MaybeNotifyReclaim()` | `MSRegionManager` | 是否该把 empty region 还给 OS | `backgroundProcessor.NotifyToReclaimGarbage()` |
| `NotifyToFeedAllocBuffers()` | `BackgroundProcessor` 调度，触发点主要在 `AllocBufferMS` | 是否该让 gc-helper 异步补 `AllocBufferMS::cachedRegion_` | `shouldFeedHungryBuffers=true`，随后执行 `BackgroundFillAllocCaches()` |

四者虽然都和“后台动作”有关，但它们并不是一回事：

- `Mark` 决定 **GC 周期什么时候开始**
- `Sweep` 决定 **旧 region 什么时候从 unswept 变成 ready**
- `Reclaim` 决定 **empty region 什么时候归还 OS**
- `FeedAllocBuffers` 决定 **什么时候给每个 alloc buffer 提前补一个 cached region**

#### 3.5.1 `MaybeNotifyMark()`：第一层，决定“要不要开新一轮 GC”

实现位置：`MSRegionManager::MaybeNotifyMark()`。

当前版本已经统一为一个入口、两个 trigger：

| Trigger | 调用点 | 语义 |
|---------|--------|------|
| `TAKE_REGIONS` | `TakeRegions()` 入口 | 分配路径 threshold-only 检查 |
| `BG_SWEEP_EXHAUSTED` | `BackgroundSweep()` 扫空 unswept 后 | 后台收口后直接上抬为 HEU 请求 |

公共主逻辑先算：

```text
effectiveAlloc = lastTrueLiveBytes + max(0, allocatedBytes - baselineAllocatedBytes)
thresholdTriggered = (effectiveAlloc >= gcStats.threshold)
```

在进入具体 trigger 分支前，先做一层统一的快速拒绝：

```text
if IsGcStarted():
    return

if now - lastHeuRequestNs < HEU_REQUEST_COOLDOWN:
    return
```

然后在第一个 `switch(triggerKind)` 分支里，分别完成该分支自己的：

1. 是否应该触发
2. 计算该 trigger 需要的局部字段
3. `compare_exchange_strong(lastHeuRequestNs, nowNs)` 抢占本次 HEU 请求资格
4. `collector.RequestGC(...)`
5. 直接输出该 trigger 的日志

也就是说，当前实现已经不再是“前面统一判断、后面第二个 switch 再打日志”的两段式，而是**每个 trigger 自己在分支内部完成计算、CAS、RequestGC 和日志收口**。

具体到两个 trigger：

1. **`TAKE_REGIONS`**
   - 不看 size class 的局部压力
   - 只要 `effectiveAlloc >= threshold`，就走 HEU 请求
   - 用于把分配路径上的 mark 启发式收敛到一个统一的 threshold-only 入口

2. **`BG_SWEEP_EXHAUSTED`**
     - 后台 sweep 在本轮 slice 结束后，若确认 `unswept` 已扫空，会在 sweep 线程上直接升级成一次 HEU 请求
   - sweep 线程会遍历所有 size class，寻找是否存在：
     - `readyCount < HEU_READY_WATERMARK`
     - `unsweptCount < HEU_UNSWEPT_WATERMARK`
   - 只有 sweep 侧已经确认“需要补判”时，才会携带对应 `cls` 调用 `MaybeNotifyMark(BG_SWEEP_EXHAUSTED, this, cls, ...)`
     - 因此 `MaybeNotifyMark()` 本体在这个分支里不再做额外 supply 门槛判断，而是直接 `shouldRequest = true`
     - 日志只记录当前三类 region 数量：`sweptRegionCount`、`unsweptRegionCount`、`emptyRegionCount`

最后统一经过 `HEU_REQUEST_COOLDOWN` 冷却，满足条件才 `RequestGC(...)`。

一句话：`MaybeNotifyMark()` 管的是“**该不该开下一轮 GC**”，不是解决当前 ready region 缺货。

#### 3.5.2 `MaybeNotifySweep()`：第二层，决定“要不要催后台 sweep”

实现位置：`MSRegionManager::MaybeNotifySweep()`。

它只关心“**ready 供给是不是已经偏紧**”，不关心 GC 阈值，也不关心 RSS：

```text
if centralPool == nullptr:
    NotifyToSweep()   // 无条件启动后台 sweep
    return

if IsGcStarted():
    return

pressure = centralPool->GetBucketPressure(cls)
if readyCount < BG_SWEEP_NOTIFY_READY_WATERMARK
   && unsweptCount > 0:
    NotifyToSweep()
```

主要调用点：

| 调用点 | 时机 | 含义 |
|------|------|------|
| `MarkSweepCollector::ConcurrentSweep()` | 一轮 GC 的 `ConcurrentSweep` 阶段开始时 | 无条件启动后台 sweep |
| `MSCentralRegionPool::GetRegion()` P2 入口 | P1 miss，真正进入 `partialUnswept` lazy sweep 路径时 | 当前 cls 已经开始直接吃 unswept backlog，顺带催后台 sweep |
| `HandleBackgroundScavenge()` | scavenge 想释放物理页但 `emptyCache==0`、`unswept>0`、`committed>target` 时 | 反向挑一个真实受压 cls，再走 `MaybeNotifySweep(this, cls)` |

一句话：`MaybeNotifySweep()` 管的是“**先把旧 unswept region 扫出来顶上**”。

#### 3.5.3 `MaybeNotifyReclaim()`：第三层，决定“要不要归还 OS”

实现位置：`MSRegionManager::MaybeNotifyReclaim()`。

它不看 size class 的 ready/unswept，也不决定新一轮 GC，只看 **empty cache 和 committed bytes**：

```text
if emptyCount >= highWatermark:
    NotifyToReclaimGarbage()
    return

targetBytes = min(lastTrueLiveBytes × CJ_MS_RECLAIM_TRUE_LIVE_TARGET_RATIO,
                  gcStats.heapThreshold)
if committedBytes > targetBytes && emptyCount > 0:
    NotifyToReclaimGarbage()
```

主要调用点：

| 调用点 | 时机 | 含义 |
|------|------|------|
| `TakeRegions()` | 在真正探测 releasedTree / bump-grow 前 | 与 `MaybeNotifyMark(TAKE_REGIONS)` 并列做一次 reclaim 判定 |
| `PushEmptySweptRegion()` | `emptySweptCache` 跨过高水位时 | 直接触发 reclaim 判定 |

后续链路固定是：

```text
MaybeNotifyReclaim()
    -> NotifyToReclaimGarbage()
    -> BackgroundProcessor::ReclaimHeapGarbage()
    -> Allocator::ReclaimGarbageMemory()
    -> CentralRegionPool::HandleBackgroundScavenge()
```

一句话：`MaybeNotifyReclaim()` 管的是“**现在的空缓存 / RSS 是不是该往下压了**”。

#### 3.5.4 `NotifyToFeedAllocBuffers()` / `BackgroundFillAllocCaches()`：后台补货链

这条链不属于 `MSRegionManager`，但它是当前代码里另一条非常重要的后台控制面，作用是给 `AllocBufferMS` 的 `cachedRegion_[cls]` 预填一块备用 region。

触发点主要在 `AllocBufferMS`：

| 触发点 | 代码位置 | 条件 | 说明 |
|--------|----------|------|------|
| `SlowAlloc()` cached miss | `AllocBufferMS::SlowAlloc()` | `missCount >= GetCacheFillThreshold(cls)` | 说明某个 class 已经开始持续 miss，需要后台补 cached region |
| active region 切换后热点 spare 不足 | `AllocBufferMS::NotifyBackgroundFillIfNeeded()` | `targetSlots > baseSlots && cachedCount < targetSlots` | 热点 class 想维持更多 spare cache |

触发后走：

```text
NotifyToFeedAllocBuffers()
    -> BackgroundProcessor::shouldFeedHungryBuffers = true
    -> BackgroundProcessor::BackgroundFillAllocCaches()
    -> RegionSpaceMS::BackgroundFillAllocCaches()
    -> CentralRegionPool::BackgroundFillAllocCaches()
    -> AllocBufferMS::BackgroundFillCache()
```

`BackgroundFillCache()` 会：

1. 按 `GetBackgroundFillPriority(cls)` 排序热点 class
2. 对每个 class 看 `cachedRegion_[cls]` 是否低于 `targetSlots`
3. 需要时走 `RefillFromCentral(cls, true)`
4. `RefillFromCentral()` 先尝试 `central_->GetRegion(cls)`，不够再 `GetFreshRegion(cls)`
5. 成功后放进 `cachedRegion_[cls][slot]`

一句话：`NotifyToFeedAllocBuffers` 管的是“**不是马上给当前分配解围，而是给下一次 miss 预先补一块 cached region**”。

#### 3.5.5 四条链的关系

当前代码里，这四条链分别服务四种不同目标：

1. `MaybeNotifySweep()`：补 ready supply
2. `NotifyToFeedAllocBuffers()`：补 per-buffer cached spare
3. `MaybeNotifyMark()`：决定是否该开下一轮 GC
4. `MaybeNotifyReclaim()`：决定是否该回收物理页 / 压 RSS

可以把它理解成：

```text
分配压力上来
  -> 先看 ready region 够不够，不够就 MaybeNotifySweep()
  -> 再看本地 cached spare 够不够，不够就 NotifyToFeedAllocBuffers()
  -> 再看整体 live-growth 是否该触发下一轮 GC，走 MaybeNotifyMark()
  -> 最后独立看 empty cache / committed 是否该往 OS 归还，走 MaybeNotifyReclaim()
```

这也是为什么当前实现里“后台补货”不能简单归类为 `MaybeNotifySweep()` 或 `MaybeNotifyReclaim()` 的一部分：  
它关注的不是 GC 周期，也不是 sweep backlog，更不是 RSS，而是 **AllocBuffer 的局部备用缓存命中率**。

#### 3.5.6 这些 `Notify*` 的具体实现

真正把“需要后台做事”转换成线程唤醒的实现，集中在 `BackgroundProcessor`：

```text
NotifyToSweep()              -> RequestWork(shouldBackgroundSweep_)
NotifyToReclaimGarbage()     -> RequestWork(shouldReclaimHeapGarbage)
NotifyToFeedAllocBuffers()   -> RequestWork(shouldFeedHungryBuffers)
```

`RequestWork()` 的实现非常统一：

```text
if !flag.exchange(true):
    Notify()   // condvar.notify_one()
```

这意味着有两个关键性质：

1. **去重唤醒**
   - 同一种后台工作对应一个原子 flag
   - 只有 `false -> true` 的第一次请求会真正 `notify_one()`
   - 后续重复请求只会把 flag 保持在 `true`，不会重复唤醒线程

2. **工作状态和线程唤醒分离**
   - `flag=true` 表示“这类工作仍待处理”
   - `Notify()` 只是“把 sleeping 的 gc-helper 叫醒”
   - 真正要不要继续下一轮，由后台处理函数自己的返回值或后续 `HasPending...()` 决定

对应到四条链：

| 入口 | 最终落到哪里 | 后台线程是否参与 |
|------|--------------|-----------------|
| `MaybeNotifyMark()` | `collector.RequestGC(GC_REASON_HEU, true)` | **否**，走 GC 请求队列，不走 `BackgroundProcessor` |
| `MaybeNotifySweep()` | `BackgroundProcessor::NotifyToSweep()` | **是** |
| `MaybeNotifyReclaim()` | `BackgroundProcessor::NotifyToReclaimGarbage()` | **是** |
| `NotifyToFeedAllocBuffers()` | `BackgroundProcessor::NotifyToFeedAllocBuffers()` | **是** |

需要特别注意：`MaybeNotifyMark()` 虽然名字也叫 `Notify`，但它的“通知对象”不是 gc-helper，而是 GC 调度器；因此下面的后台线程章节**不包含** `MaybeNotifyMark()`。

#### 3.5.7 触发后，后台线程怎么做；每轮什么时候停

`BackgroundProcessor::Run()` 的主循环顺序是固定的：

```text
while (running):
    1. finalizer
    2. background sweep
    3. fill alloc-buffer caches
    4. reclaim garbage
```

也就是说，一次被唤醒后，gc-helper 会按这个顺序“把当前已经挂起的工作类型都扫一遍”。

##### A. `NotifyToSweep()` 之后：后台 Sweep 怎么跑，什么时候停一轮

链路：

```text
MaybeNotifySweep()
    -> BackgroundProcessor::NotifyToSweep()
    -> shouldBackgroundSweep_ = true
    -> Run()
    -> BackgroundSweepLoop()
    -> allocator.BackgroundSweep()
    -> CentralRegionPool::BackgroundSweep()
```

`BackgroundSweepLoop()` 会调用 `allocator.BackgroundSweep()`，并把返回值写回 `shouldBackgroundSweep_`：

```text
pendingSweep = allocator.BackgroundSweep()
shouldBackgroundSweep_ = pendingSweep
if !pendingSweep && allocator.HasPendingReclaimWork():
    shouldReclaimHeapGarbage = true
```

所以 **一轮后台 sweep slice** 的停止条件是：

1. `stopSweepRequested_ == true`
   - 新一轮 `PreEnumRoots()` 要开始，要求 sweep 立即让路
2. `GetTotalUnsweptCount() == 0`
   - backlog 已经扫空
3. 单轮预算耗尽
   - `sweptInSlice >= maxRegions`
   - 或 `now >= deadlineUs`
4. `SweepUnsweptList()` 在中途因为预算/stop 请求返回 `false`

这里要区分：

- **“停一轮 slice”**：本次 `BackgroundSweep()` 返回
- **“停完整个 sweep 任务”**：返回 `pendingSweep == false`，并把 `shouldBackgroundSweep_` 清掉

如果只是预算到期，通常会返回 `pendingSweep == true`，意味着这轮 slice 停了，但后台 sweep 任务**还没结束**，gc-helper 在 burst 模式里会继续下一轮。

##### B. `NotifyToFeedAllocBuffers()` 之后：后台补货怎么跑，什么时候停一轮

链路：

```text
NotifyToFeedAllocBuffers()
    -> shouldFeedHungryBuffers = true
    -> Run()
    -> BackgroundFillAllocCaches()
    -> allocator.BackgroundFillAllocCaches()
    -> CentralRegionPool::BackgroundFillAllocCaches()
    -> AllocBufferMS::BackgroundFillCache()
```

`BackgroundProcessor::BackgroundFillAllocCaches()` 一进来就先：

```text
shouldFeedHungryBuffers = false
```

然后跑一遍 `BackgroundFillAllocCaches()`。所以补货链的特点是：

- **一次唤醒只做一轮 pass**
- 没有 `pendingFill` 这类“自动续传”返回值
- 如果后面还需要补货，必须由新的 miss / hot-spare 不足再次触发 `NotifyToFeedAllocBuffers()`

因此，**一轮后台补货** 的停止条件非常简单：

1. 遍历完当前注册的所有 `AllocBufferMS`
2. 对每个 buffer 的 `BackgroundFillCache()` 执行完一轮 class 扫描
3. 没有“做一点再回来”的 pending 语义，函数返回即该轮结束

##### C. `NotifyToReclaimGarbage()` 之后：后台 Reclaim 怎么跑，什么时候停一轮

链路：

```text
MaybeNotifyReclaim()
    -> NotifyToReclaimGarbage()
    -> shouldReclaimHeapGarbage = true
    -> Run()
    -> ReclaimHeapGarbage()
    -> allocator.ReclaimGarbageMemory(false)
    -> CentralRegionPool::HandleBackgroundScavenge()
```

`ReclaimHeapGarbage()` 的收口逻辑和 sweep 不一样：

```text
releasedBytes = allocator.ReclaimGarbageMemory(false)
pendingReclaim = allocator.HasPendingReclaimWork()
shouldReclaimHeapGarbage = pendingReclaim
```

所以 **一轮 reclaim slice** 的停止条件是：

1. `HandleBackgroundScavenge()` 本次 slice 释放循环结束
   - `emptyCount <= lowWatermark`（且 !trueLiveSlackHigh）
   - `TakeHead() == nullptr`
   - `releasedRegions >= maxRegions`
   - `now >= deadline`
2. 然后再看 `HasPendingReclaimWork()`
   - `true`：本轮 slice 停，但 reclaim 任务还没结束，`shouldReclaimHeapGarbage` 保持为 `true`
   - `false`：本轮 slice 停，同时 reclaim 任务也结束

##### D. gc-helper 线程整体什么时候继续，什么时候真正停住

一次 `Run()` 循环做完当前工作后，会根据 `shouldContinueImmediate` 进入两种模式：

1. **Burst 模式继续**
   - 连续有工作时，最多连续跑 `4` 轮或 `12ms`
   - 中间 `yield()`
   - 超过预算后 `WaitForActiveBackoff(1ms)`，再继续

2. **Idle 等待**
   - 没有待处理 flag，也没有 finalizer job
   - `Wait(2000ms)` 睡眠
   - 直到新的 `Notify*()` 或 finalizer work 到来

因此从“后台线程”的视角看：

- `Sweep` / `Reclaim` 都有“**slice 停了，但任务可能继续**”的语义
- `FillAllocBuffers` 是“**做完一轮 pass 就结束**”
- 真正让 gc-helper 进入 idle 的条件，是 `HasPendingWakeWork() == false`

#### 3.5.8 三层参数总表（默认值 vs seed5）

下面按“三层控制环”整理当前代码**仍在主路径中读取并参与判定**的参数。`seed5` 值来自：
`MSGC_RESULT/Result/seed/seed5.env`。

**第一层：GC 周期驱动 / 下一轮 threshold 更新**

| 参数 | 含义 | 默认值 | seed5 值 |
|------|------|--------|---------|
| `cjGCThreshold` | Collector 侧 GC 阈值上限；GC 结束后算出的 `gcStats.heapThreshold` 最终会 clamp 到它 | `initHeapSize / 2`（由 `CjScheduler` 初始化） | `3mb` |
| `cjGCInterval` | Collector 层全局最小 GC 间隔；所有 `RequestGC(...)` 都会受它约束 | `20ms` | `15ms` |
| `CJ_MS_HEU_REQUEST_COOLDOWN` | MSGC 自己的 HEU 冷却时间；限制 `TAKE_REGIONS` / `BG_SWEEP_EXHAUSTED` 重复请求频率，`RECLAIM_SWEEP_EXHAUSTED` 不受此防抖限制 | `10ms` | `1ms` |

**第二层：后台 Sweep / Lazy Sweep**

| 参数 | 含义 | 默认值 | seed5 值 |
|------|------|--------|---------|
| `CJ_MS_BG_SWEEP_NOTIFY_READY_WATERMARK` | `MaybeNotifySweep()` 中，ready 低于此值才唤醒后台 sweep | `8` regions | 未覆盖（默认） |
| `CJ_MS_BG_SWEEP_MAX_REGIONS_PER_SLICE` | normal 模式下，一轮后台 sweep 最多扫多少 region | `64` regions | `192` |
| `CJ_MS_BG_SWEEP_MAX_TIME` | normal 模式下，一轮后台 sweep 的时间预算 | `2000us` | 未覆盖（默认） |
| `CJ_MS_BG_SWEEP_PRESSURE_REGIONS_PER_SLICE` | pressure 模式下，一轮后台 sweep 最多扫多少 region | `256` regions | `384` |
| `CJ_MS_BG_SWEEP_PRESSURE_TIME` | pressure 模式下，一轮后台 sweep 的时间预算 | `8000us` | 未覆盖（默认） |
| `CJ_MS_BG_SWEEP_PRESSURE_READY_THRESHOLD` | `BackgroundSweep()` 内部判断“进入 pressure 模式”的 ready 阈值 | `4` regions | 未覆盖（默认） |
| `CJ_MS_LAZY_EMPTY_CACHE_REFILL_LIMIT` | lazy sweep 遇到 completely-free region 时，最多贡献多少次 empty-cache refill 后再直接 rebind 返回 | `1` | 未覆盖（默认） |

**第三层：后台 Scavenge / Reclaim**

| 参数 | 含义 | 默认值 | seed5 值 |
|------|------|--------|---------|
| `CJ_MS_EMPTY_CACHE_LOW_PER_BUFFER` | 每个 `AllocBufferMS` 对 low watermark 的基础贡献值 | `2` regions/buffer | 未覆盖（默认） |
| `CJ_MS_EMPTY_CACHE_LOW_FLOOR_REGIONS` | empty cache low watermark 的绝对下限 | `32` regions | `2` |
| `CJ_MS_EMPTY_CACHE_LOW_CAP_REGIONS` | empty cache low watermark 的绝对上限 | `256` regions | `4` |
| `CJ_MS_EMPTY_CACHE_HIGH_FACTOR` | high watermark = low watermark × factor | `2` | `1` |
| `CJ_MS_RECLAIM_TRUE_LIVE_TARGET_RATIO` | reclaim / empty-pressure sweep 使用的 true-live 目标倍率；`targetBytes = min(lastTrueLive × ratio, gcThreshold)`，独立于 GC threshold 的 `cjHeapGrowth` | `1.2` | 未覆盖（默认） |
| `CJ_MS_EMPTY_SCAVENGE_MAX_TIME` | 一轮后台 scavenge 的时间预算 | `4000us` | 未覆盖（默认） |
| `CJ_MS_EMPTY_SCAVENGE_MAX_REGIONS_PER_SLICE` | 一轮后台 scavenge 最多释放多少 empty region | `256` regions | `256` |
| `CJ_MS_INLINE_RECLAIM` | 大对象分配时是否启用 `InlineReclaimForBalance` | `1`（开启） | `1` |

**seed5 中仍保留、但当前主路径已不再生效的历史覆盖**

| 参数 | seed5 值 | 当前状态 |
|------|---------|---------|

**补货链（`NotifyToFeedAllocBuffers()`）说明**

| 项目 | 说明 |
|------|------|
| dedicated env 参数 | 当前 `AllocBufferMS::BackgroundFillCache()` 没有独立的 `CJ_MS_*` 环境变量开关；主要由代码内的 miss/heat 逻辑驱动 |
| seed5 覆盖情况 | `seed5.env` 没有专门覆盖补货链参数；它主要通过第一层 / 第二层 / 第三层参数间接影响补货命中率 |

## 4. 第二层：后台 Sweep 与 Lazy Sweep

### 4.1 sweepGen 代际机制

**`globalSweepGen_`** — `std::atomic<uint32_t>`，每轮 GC 完成后 +2：

```
region.sweepGen == sg - 2   →  未 sweep（需要清扫）
region.sweepGen == sg - 1   →  正在被某线程 sweep（CAS 进行中）
region.sweepGen == sg       →  已 sweep（可直接使用）
```

+2 而非 +1，是为了单字段实现三态区分，无需额外 `sweepState` 字段。

**`MSCentralRegionPool::PrepareForSweep()`** — 在 MarkTermination STW 阶段执行：

```
for each cls:
    partialSwept → partialUnswept  (StealChainTo: O(1) 链表头转移)
    fullSwept    ↔ fullUnswept     (Swap: O(1) 头指针/计数交换)
globalSweepGen_ += 2
sweepDone_ = false
sweepStats_.Reset()
```

所有上轮 swept 的 region 瞬间变为 unswept，无需逐个移动。这是方案 B（O(1) 指针交换）。

### 4.2 Sweep 通知、执行与停止机制

#### 4.2.1 谁会通知后台执行 Sweep？

所有通知最终都会收敛到 `BackgroundProcessor::NotifyToSweep()` → `RequestWork(shouldBackgroundSweep_)` → `condvar.notify_one()` 唤醒 gc-helper 线程。通知来源有三处：

| # | 调用方 | 代码位置 | 触发条件 | 说明 |
|---|--------|----------|----------|------|
| **①** | `MarkSweepCollector::ConcurrentSweep()` | [MarkSweepCollector.cpp](../../src/Heap/MCollector/MarkSweepCollector.cpp#L1033) `MaybeNotifySweep()` | **无条件** — `centralPool == nullptr` 走无条件分支 | GC Mark 完成后启动新一轮后台 Sweep 的**主入口** |
| **②** | `MSCentralRegionPool::GetRegion()` P2 入口 | [MSCentralRegionPool.cpp](../../src/Heap/MAllocator/MSCentralRegionPool.cpp#L183) → `MaybeNotifySweep(this, cls)` | `readyCount < BG_SWEEP_NOTIFY_READY_WATERMARK(默认4)` **且** `unsweptCount > 0` **且** `!IsGcStarted()` | **仅在 P1(partialSwept) 未命中、实际进入 P2(partialUnswept) lazy sweep 路径时才通知**——确认确实需要从 unswept 队列获取 region 后才唤醒后台 sweep，避免 P1 命中时的无效唤醒 |
| **③** | `HandleBackgroundScavenge()` 反向触发 | [MSCentralRegionPool.cpp](../../src/Heap/MAllocator/MSCentralRegionPool.cpp#L535) 选择一个真实受压的 `cls` 后调用 `MaybeNotifySweep(this, cls)` | `shouldPressureSweep`: emptyCache==0 **且** unsweptCount>0 **且** committed>target **且** `!IsGcStarted()` | Layer 3 Scavenge 想释放物理页但 emptyCache 为空，经 `MaybeNotifySweep` 复用现有 sweep 判定与唤醒逻辑 |

**`MaybeNotifySweep` 条件分支汇总**（[MSRegionManager.cpp](../../src/Heap/MAllocator/MSRegionManager.cpp#L245)）：

```
MaybeNotifySweep(centralPool, cls):
    if centralPool == nullptr:             // ← 来自 ConcurrentSweep (路径①)
        NotifyToSweep()                    //    无条件唤醒
        return

    if IsGcStarted():                      // GC 进行中不干扰
        return

    pressure = centralPool->GetBucketPressure(cls)
    if pressure.ReadyCount() < 4           // ← 来自 GetRegion (路径②)
       && pressure.UnsweptCount() > 0:
        NotifyToSweep()                    //    有条件唤醒
```

其中 `ReadyCount() = partialSwept.Count() + partialUnswept.Count()`（已 swept 和可 lazy sweep 的 region 和），`UnsweptCount() = partialUnswept.Count() + fullUnswept.Count()`。

> **设计要点**：路径 ② 和 ③ 的条件是**per-sizeClass**的：只有分配目标 cls 的 ready region 低于水位时才通知。其他 cls 即使积压严重也不会触发（由后台 sweep 遍历所有 cls 覆盖）。

#### 4.2.2 后台 Sweep 的执行与退出条件

**两层循环**：`BackgroundProcessor::Run()` → `BackgroundSweepLoop()` → `CentralRegionPool::BackgroundSweep()`。

**内层：`BackgroundSweep()` 单 slice 执行**（[MSCentralRegionPool.cpp](../../src/Heap/MAllocator/MSCentralRegionPool.cpp#L615)）：

每次调用执行一个"sweep slice"，遍历 cls 0..35，对每个 cls 的 `partialUnswept` 和 `fullUnswept` 链表调用 `SweepUnsweptList()`。退出条件：

| 退出条件 | 检查位置 | 含义 |
|---------|---------|------|
| `sweptInSlice >= maxRegions` | `SweepUnsweptList` 循环头 | 达到预算上限：常规 64 / 压力 256 个 region |
| `now >= deadlineUs` | `SweepUnsweptList` 循环头 | 达到时间上限：常规 2000µs / 压力 8000µs |
| `stopSweepRequested_ == true` | 每个 cls 边界检查 | GC 要求停止（PreEnumRoots） |
| 所有 unswept 链表为空 | `SweepUnsweptList` 返回 true（正常完成） | 本轮 sweep 全部完成 |

**退出后状态设置**：

```
if 达到预算上限 or stopRequested:
    pendingWork = true  → BackgroundSweepLoop 返回 true → shouldBackgroundSweep_ 保持 true
                        → Run() 主循环继续下一轮 sweep

if 所有 unswept 遍历完 且 !stopRequested:
    sweepDone_ = true
    pendingWork = false → BackgroundSweepLoop 返回 false → shouldBackgroundSweep_ 设为 false
                        → 如果 totalUnsweptCount == 0，先由 sweep 线程做 pressure 补判
                        → 补判成立时才额外触发 MaybeNotifyMark(BG_SWEEP_EXHAUSTED)
```

**外层：`BackgroundProcessor::Run()` burst 控制**：

```
while(running):
    ...
    if shouldBackgroundSweep_:
        shouldContinueImmediate = BackgroundSweepLoop()   // ← 执行一个 slice

    if shouldContinueImmediate:
        // Burst 模式：连续执行，但受限
        if burstRounds < 4 && burstElapsed < 12000µs:
            yield() → continue                            // 立即再来一轮
        else:
            WaitForActiveBackoff(1ms)                     // 强制冷却 1ms
            continue
    else:
        // 无工作：Wait condvar (最长 2000ms)
        while !HasPendingWakeWork():
            Wait(2000ms)
```

**Sweep 连续执行的总预算**：每 burst 最多 4 轮 × 每轮 64~256 region = **256~1024 region**，或 4 轮 × 2~8ms = **8~32ms**，之后强制 backoff 1ms。

#### 4.2.3 什么时候会停止 Sweep？

Sweep 有三种停止方式：

| 停止方式 | 触发者 | 机制 | 影响范围 |
|---------|--------|------|---------|
| **正常结束** | BackgroundSweep 自身 | 所有 unswept 链表遍历完 → `sweepDone_=true`, `shouldBackgroundSweep_=false` | 后台 Sweep 停止；Lazy Sweep 因无 unswept 自然无事可做 |
| **GC 请求停止** | `PreEnumRoots()` 调用 `StopAndWaitForAllSweep()` | ① `stopSweepRequested_=true` → 后台 Sweep 在下一个 cls 边界退出 ② `skipLazySweep_=true` → 新 lazy sweep 被禁止 ③ spin-wait `bgSweepRunning_==false` ④ spin-wait `activeLazySweepCount_==0` | **全部 Sweep 停止**（后台 + lazy） |
| **预算耗尽暂停** | BackgroundSweep 达到 region/time 上限 | `pendingWork=true` → `shouldBackgroundSweep_` 保持 true → 下一轮 Run() 继续 | 仅暂停当前 slice，下一轮继续 |

**停止的关键 flag 状态表**：

| flag | 正常运行 | 预算暂停 | GC 停止 | 说明 |
|------|---------|---------|---------|------|
| `shouldBackgroundSweep_` | true | **true**（保留） | 由 BackgroundSweep 返回值设置 | 控制 Run() 是否调用 BackgroundSweepLoop |
| `stopSweepRequested_` | false | false | **true** | 后台 Sweep 每 cls 检查 |
| `skipLazySweep_` | false | false | **true** | GetRegion P2/P3 入口做 double-check |
| `bgSweepRunning_` | true (执行中) | false | false (被等待至 false) | 后台 Sweep 进出函数设置 |
| `sweepDone_` | false | false | 不变 | 全部 sweep 完成后 true |

#### 4.2.4 什么时候会重新开启 Sweep？

| 开启时机 | 代码位置 | 具体操作 |
|---------|---------|---------|
| **GC 停+清后重新启用** | `PreEnumRoots()` → `ReenableLazySweep()` ([MSCentralRegionPool.cpp](../../src/Heap/MAllocator/MSCentralRegionPool.cpp#L1215)) | `skipLazySweep_=false`, `stopSweepRequested_=false` |
| **新一轮 GC Mark 完成** | `ConcurrentSweep()` → `MaybeNotifySweep()` (路径 ①) | 无条件 `NotifyToSweep()` → `shouldBackgroundSweep_=true` + condvar 唤醒 |
| **分配路径发现压力** | `GetRegion()` → `MaybeNotifySweep(pool, cls)` (路径 ②) | 有条件 `NotifyToSweep()` |
| **Scavenge 反向触发** | `HandleBackgroundScavenge()` (路径 ③) | 有条件选择受压 `cls` 后 `MaybeNotifySweep(this, cls)` → `NotifyToSweep()` |

**完整的一轮 Sweep 生命周期**：

```
GC#N MarkTermination
  └─ PrepareForSweep(): sweepGen+=2, 链表翻转, sweepDone_=false
  └─ ConcurrentSweep(): MaybeNotifySweep() 无条件唤醒 → shouldBackgroundSweep_=true
       │
       ▼
  gc-helper: Run() → BackgroundSweepLoop() → BackgroundSweep()
       │  ← 每 slice 最多 64~256 region / 2~8ms
       │  ← burst 最多 4 轮 or 12ms，然后 backoff 1ms
       │  ← 期间 Mutator GetRegion 可 lazy sweep (P2/P3)
       │  ← 如果分配压力大，GetRegion/TakeRegion 额外 NotifyToSweep 保持唤醒
       │
       ├─ 全部 unswept 完成 → sweepDone_=true, shouldBackgroundSweep_=false
       │                       → MaybeNotifyMark(BG_SWEEP_EXHAUSTED) 驱动 GC 检查
       │
  GC#N+1 PreEnumRoots
       ├─ StopAndWaitForAllSweep():
       │    stopSweepRequested_=true, skipLazySweep_=true
       │    spin-wait bg + lazy 全部退出
       ├─ (此时如有残留 unswept → 内部 ForceFinish)
       ├─ ClearAllMarkBitmaps()
       ├─ ReclassifyUnsweptToSwept()
       └─ ReenableLazySweep():
            stopSweepRequested_=false, skipLazySweep_=false
            → Sweep 机制恢复，等待下一轮 PrepareForSweep + ConcurrentSweep 再次启动
```

### 4.3 BackgroundSweep 主循环

**`MSCentralRegionPool::BackgroundSweep()`** — 由 `BackgroundProcessor::BackgroundSweepLoop()` 间接调用：

```
1. 压力检测:
   ShouldPressureSweepForTrueLiveSlack()
     → emptyCount==0 && unsweptCount>0 && committed>targetBytes
   或任一 cls: readyCount < PRESSURE_READY_THRESHOLD && unsweptCount > 0

2. 选择预算:
   压力模式: maxRegions=256,  deadline=8000µs
   常规模式: maxRegions=64,   deadline=2000µs

3. 遍历 cls 0..35:
   - 先 SweepUnsweptList(partialUnswept)，再 SweepUnsweptList(fullUnswept)
   - 每 4 个 cls 执行 yield()
   - 达到预算上限 → pendingWork=true, break

4. 完成判定:
   if !pendingWork && !stopRequested:
       sweepDone_ = true
       若 unsweptCount==0 → MaybeNotifyMark(BG_SWEEP_EXHAUSTED)
```

**`MSCentralRegionPool::SweepUnsweptList()`** — 对一个 sizeClass 的一条 unswept 链表逐 region sweep：

```
while sweptInSlice < maxRegions && now < deadline:
    region = list.TakeHead()        (under bucket.lock)
    if CasSweepGen(sg-2, sg-1):     // 抢占 sweep 所有权
        freedBytes = region->SweepRegion()    // allocBitmap = ~markBitmap
        region->SetSweepGen(sg)

        if IsCompletelyFree():
            PushEmptySweptRegion(region)  → 进入 emptySweptCache (Layer 3 入口)
        elif HasFreeSpace():
            → partialSwept (lock-free Push)
        else:
            → fullSwept (under lock, Prepend)
    // CAS 失败: lazy sweep 或 ForceFinish 已处理, skip
    每 8 个 region yield 一次
```

### 4.4 Lazy Sweep — 分配路径集成

**`MSCentralRegionPool::GetRegion(cls)`** — 分配路径中的五优先级获取：

| 优先级 | 来源 | 操作 | 性能 |
|--------|------|------|------|
| **P1** | `partialSwept` (lock-free Pop) | 直接返回 | 最快，零 sweep 开销 |
| **P2** | `partialUnswept` (lazy sweep) | CAS claim → SweepRegion → 若 empty 则 `PushEmptySweptRegion`（限 1 次），否则返回 partial | ~7-20ns sweep |
| **P3** | `fullUnswept` (lazy sweep) | 同 P2，sweep 后可能仍 full → 放回 fullSwept 继续循环 | 可能多次循环 |
| **P4** | `emptySweptCache` (rebind) | `TakeEmptySweptRegion → RebindEmptyRegionForClass(cls)` | 需 InitForSizeClass |
| **P5** | 返回 nullptr | 调用方执行 `GetFreshRegion()` → `MSRegionManager::TakeRegion()` | 最慢，可能 BUMP 扩展 |

**Lazy Sweep 限流**：mutator 在 P2/P3 中发现完全空闲 region 时，最多向 emptySweptCache 贡献 `LAZY_EMPTY_CACHE_REFILL_LIMIT`（默认 1）个，避免分配路径延迟过大。

**GetRegion P2 入口额外动作**：进入 P2（partialUnswept lazy sweep）路径时调 `MaybeNotifySweep(this, cls)` 唤醒后台 sweep 补给。仅在 P1 未命中时才触发，P3 不重复通知（P2 已通知过，且后台 sweep 覆盖所有 cls）。

### 4.5 三路 Sweep 协作

| 机制 | 触发者 | 时机 | 目标 | CAS |
|------|--------|------|------|-----|
| **后台 Sweep** | gc-helper 线程 | GC Mark 完成后 `NotifyToSweep()` | 尽量 sweep 完所有 Region（主力） | 标准 CAS |
| **Lazy Sweep** | 分配线程 | `GetRegion` P2/P3 按需 | sweep 一个即返回 | 标准 CAS |
| **ForceFinish** | GC 启动 | PreEnumRoots 阶段 | 排空**全部**残留 Unswept | CAS 不能失败(FATAL) |

三者通过 **sweepGen CAS** 协作，不会重复 sweep 同一 region：
- 链表 pop 在 `bucket.lock` 保护下互斥
- 不同 region 各自 CAS → 无竞争
- CAS 失败 → 跳过（不自旋等待）

## 5. 第三层：后台 Scavenge 与物理回收

### 5.1 核心入口

**`MSCentralRegionPool::HandleBackgroundScavenge()`** — 由 `BackgroundProcessor::ReclaimHeapGarbage()` → `Allocator::ReclaimGarbageMemory()` 间接调用。

### 5.2 物理回收全路径总览与主路径分析

物理页归还（`madvise(MADV_DONTNEED)` / macOS `mmap MAP_FIXED`）在代码中有**三条调用路径**，全部最终汇聚到 `FreeManager::ReclaimRegionsReleased()` → `ReleaseUnits()`：

```
                         ┌─────────────────────────────────────────────────────┐
                         │         FreeManager::ReclaimRegionsReleased()       │
                         │   → ReleaseUnits() → madvise / mmap MAP_FIXED      │
                         │   → InsertRegions() → 放入 CartesianTree            │
                         └───────┬──────────────────┬──────────────┬──────────┘
                                 │                  │              │
                     ┌───────────┘                  │              └───────────┐
                     │                              │                         │
              ┌──────┴──────┐            ┌──────────┴──┐           ┌──────────┴───────┐
              │ 路径 A      │            │ 路径 B       │           │ 路径 C            │
              │Background   │            │InlineReclaim │           │SweepLarge        │
              │Scavenge     │            │ForBalance    │           │Regions           │
              │(小对象批量)  │            │(大对象同步)   │           │(大对象即时)       │
              └─────────────┘            └─────────────┘           └──────────────────┘
```

#### 三条路径详细对比

| 维度 | 路径 A: BackgroundScavenge | **路径 B: InlineReclaim** | 路径 C: SweepLargeRegions |
|------|--------------------------|--------------------------|--------------------------|
| **对象类型** | 小对象 region | **大对象分配时释放等量小对象 empty region** | 大对象 region (≥MS_REGION_SIZE) |
| **触发位置** | `BackgroundProcessor::ReclaimHeapGarbage()` | **`TakeRegionForLarge()` 内同步调用** | `MarkSweepCollector::ConcurrentSweep()` |
| **执行线程** | gc-helper 后台线程 | **Mutator 线程（分配路径上）** | gc-helper 后台线程 |
| **触发条件** | emptyCache ≥ highWatermark 或 trueLiveSlack | **InlineReclaim 启用（不区分 BUMP/RELEASED）** | GC mark 完成后 |
| **每次释放量** | 最多 `BG_EMPTY_SCAVENGE_MAX` 个 (默认 256)，受时间窗口限制 | **regionCount 个**（与大对象占用的 region 数等量） | 所有未标记大对象 |
| **代码入口** | `MSCentralRegionPool.cpp` L529 | **`MSRegionManager.cpp` TakeRegionForLarge** | `MSCentralRegionPool.cpp` L932 |
| **当前调用状态** | ✅ 活跃 | ✅ 活跃 | ✅ 活跃 |

#### 路径 B 详解：TakeRegionForLarge 时的同步 Reclaim

**设计变更**：InlineReclaim 从小对象 `TakeRegion()` 移至大对象 `TakeRegionForLarge()`。

**原因**：小对象走到 `TakeRegion()`（即 GetRegion P5→GetFreshRegion）时，P4 已经确认 `emptySweptCache` 为空或不足，此时再从 emptyCache 取 region 释放物理页几乎不可能成功。而大对象分配 BUMP 扩展时，emptyCache 中的小对象空 region 大概率仍有存量，释放等量 region 对冲 committed 增长是有效的。

`MSRegionManager::TakeRegionForLarge()` （[MSRegionManager.cpp](../../src/Heap/MAllocator/MSRegionManager.cpp#L550)）在分配大对象时**无条件**同步释放等量 empty region：

```cpp
MSRegionInfo* MSRegionManager::TakeRegionForLarge(size_t unitCount, uintptr_t& regionStart,
                                                   const CentralRegionPool* centralPool)
{
    RegionCount regionCount = ...;  // ceil(unitCount / unitsPerMSRegion)
    TakeRegions(regionCount, regionIdx, &source);

    // ★ 无论 BUMP 或 RELEASED，同步释放 regionCount 个 emptyCache region
    //   BUMP: 新虚拟地址首次 touch → committed 增加
    //   RELEASED: 已 madvise 的 span 重新 touch → 物理页重新 commit
    //   两者都会增加实际 RSS，等量释放对冲增长
    if (GetInlineReclaimEnabled() && centralPool != nullptr) {
        for (RegionCount i = 0; i < regionCount; ++i) {
            InlineReclaimForBalance(*centralPool);     // ← 路径 B 入口，每次释放 1 个
        }
    }

    // reclaim / mark 统一在 TakeRegions() 入口做 MaybeNotifyReclaim() / MaybeNotifyMark(TAKE_REGIONS)
    // ...
}
```

**等量释放语义**：大对象占用 N 个 region → 循环调用 `InlineReclaimForBalance` N 次 → 最多释放 N 个 empty region。每次调用内部都检查 `emptySweptCount > lowWatermark`，不足时提前退出。

**不区分 BUMP/RELEASED 的原因**：`TakeRegions()` 有两个来源（[MSRegionManager.cpp](../../src/Heap/MAllocator/MSRegionManager.cpp#L423)）：

| 来源 | 含义 | 物理页状态 | InlineReclaim |
|------|------|-----------|---------------|
| `BUMP` | 从未分配区推进指针（可能触发 `GrowOneSegment`） | 新虚拟地址，首次 touch 时 OS commit 物理页 | ✅ 需要对冲 |
| `RELEASED` | 从 CartesianTree 取出已 madvise 的 span | 物理页已释放，重新 touch 时 OS 重新 commit | ✅ 同样增加 RSS，需要对冲 |

两者都会在分配后 touch 时增加实际物理页驻留（RSS），因此统一执行 InlineReclaim 释放等量 emptyCache region 对冲。

InlineReclaim 的完整逻辑（[MSRegionManager.cpp](../../src/Heap/MAllocator/MSRegionManager.cpp#L630)）：

```
InlineReclaimForBalance(pool):
    if emptySweptCount <= lowWatermark → return   // 不掏空 cache
    region = pool.TakeEmptySweptRegionForReclaim() // 从 emptyCache 取一个
    if region == null → return
    ReclaimEmptyRegionToReleased(region)           // Reset + ReleaseUnits 释放物理页
    pool.DecrementTotalMSRegionBytes(MS_REGION_SIZE)
```

#### 主路径判定

对于**小对象 region** 的物理回收，路径 A 是唯一的批量路径：

| 场景 | 主力路径 | 原因 |
|------|---------|------|
| **稳态运行** (堆大小稳定) | **路径 A** (BackgroundScavenge) | 大部分 TakeRegion 从 RELEASED 复用。emptyCache 积累靠 BackgroundScavenge 批量释放 |
| **GC 后 RSS 回落** | **路径 A** (BackgroundScavenge) | GC 后大量 region 变 empty → emptyCache 越过 highWatermark → 后台批量 scavenge 释放物理页 |

对于**大对象分配期间**的 committed 增长控制，路径 B 提供即时对冲：

| 场景 | 路径 | 原因 |
|------|------|------|
| **大对象 BUMP 扩展** | **路径 B** (InlineReclaim) | 大对象占用 N 个 region → 同步释放 N 个 emptyCache region 对冲 committed 增长 |

**结论**：
- **路径 A (BackgroundScavenge)** 是**主路径**：批量释放（每轮最多 256 个 region、4ms 时间窗），负责绝大部分物理页归还
- **路径 B (InlineReclaim)** 是**大对象分配补偿路径**：等量释放（N:N 对冲），不区分 BUMP/RELEASED 来源，任何大对象分配都触发，稳定 RSS
- **路径 C (SweepLargeRegions)** 是**大对象回收路径**：sweep 时即时归还，不经过 emptyCache 中转
- 小对象 `TakeRegion()` 不再做 InlineReclaim——走到 P5 时 emptyCache 已确认不足，释放无意义

> 类比 Go runtime：路径 A ≈ `bgscavenge()` 后台归还，路径 B ≈ Go 中 `mheap.alloc` 时的 scavenge 对冲（以 span 为单位），路径 C ≈ Go 的 `sweep/mspan.sweep` 对大 span 的处理。

### 5.3 唤醒后台 Scavenge 的时机与逻辑

所有 Scavenge 唤醒最终通过 `BackgroundProcessor::NotifyToReclaimGarbage()` → `RequestWork(shouldReclaimHeapGarbage)` → `condvar.notify_one()` 唤醒 gc-helper 线程。当前代码已经不再通过 `RecordReclaimPressure + DrainBackgroundReclaimPressure` 中转，而是直接在策略入口调用 `MaybeNotifyReclaim()`。触发来源有两处：

| # | 调用方 | 代码位置 | 触发条件 | 说明 |
|---|--------|----------|----------|------|
| **①** | `MSRegionManager::TakeRegions()` | `MaybeNotifyReclaim()` | 每次进入 fresh region / large span 底层取数前 | 与 `MaybeNotifyMark(HeuTriggerKind::TAKE_REGIONS)` 并列，统一做 reclaim 判定 |
| **②** | `MSCentralRegionPool::PushEmptySweptRegion()` | `MaybeNotifyReclaim()` | `previousCount < highWatermark && newCount >= highWatermark` | empty cache 越过高水位时直接触发 reclaim 判定 |

**`MaybeNotifyReclaim` 的条件逻辑**：

```
MaybeNotifyReclaim():
    emptyCount = pool.GetEmptySweptRegionCount()
    highWatermark = pool.GetEmptyCacheHighWatermarkRegions()

    // 条件 A: emptyCache 越过高水位
    if emptyCount >= highWatermark:
        NotifyToReclaimGarbage()
        return

    // 条件 B: committed 超出目标 + 有可释放的 empty region
    committedBytes = pool.GetTotalMSRegionBytes()
    targetBytes = pool.GetTrueLiveSlackTargetBytes()
    if targetBytes != 0 && committedBytes > targetBytes && emptyCount > 0:
        NotifyToReclaimGarbage()
```

**两个触发条件独立**，满足任一即唤醒：
1. **emptyCount ≥ highWatermark** — 空缓存过多，需要归还物理页
2. **trueLiveSlackHigh && emptyCount > 0** — committed 超出活跃目标字节数，有 empty region 可释放

**唤醒去重机制**：`RequestWork()` 内部使用 `exchange(true)`，只有 `false→true` 时才 `condvar.notify_one()`，重复唤醒会被自动过滤。

### 5.4 后台 Scavenge 执行过程

gc-helper 线程 `BackgroundProcessor::Run()` 检测到 `shouldReclaimHeapGarbage == true` → 调用 `ReclaimHeapGarbage()` → `Allocator::ReclaimGarbageMemory()` → **`MSCentralRegionPool::HandleBackgroundScavenge()`**。

#### 5.4.1 状态收集

`CollectBackgroundScavengeState()` 一次性原子快照收集决策所需的全部状态：

| 字段 | 来源 | 含义 |
|------|------|------|
| `emptyCount` | `emptySweptRegionCount_` | 当前 emptySweptCache 中的 region 数 |
| `lowWatermark` | 动态计算 | 低水位：保留不释放的最少 empty region 数 |
| `highWatermark` | `lowWatermark × HIGH_FACTOR` | 高水位：超过则必须 scavenge |
| `committedBytes` | `totalMSRegionBytes_` | 当前小对象已 committed 字节数 |
| `targetBytes` | `min(lastTrueLiveBytes × CJ_MS_RECLAIM_TRUE_LIVE_TARGET_RATIO, gcThreshold)` | reclaim / pressure sweep 的目标字节数；默认 true-live × 1.2，且不超过 GC 阈值 |
| `trueLiveSlackHigh` | `targetBytes != 0 && committedBytes > targetBytes` | committed 是否超出"应有量" |
| `shouldScavenge` | 综合判定 | `emptyCount >= highWatermark` 或 `(trueLiveSlackHigh && emptyCount > 0)` |
| `shouldPressureSweep` | 综合判定 | `emptyCount == 0 && unsweptCount > 0 && trueLiveSlackHigh` |

#### 5.4.2 反向触发 Layer 2

进入 `HandleBackgroundScavenge` 后，**优先检查反向触发**：

```
if !IsGcStarted() && shouldPressureSweep:
    MaybeNotifySweep(this, candidateCls)    // 选择真实受压 class，经统一 sweep 判定入口催促 Layer 2
```

当 scavenge 想释放物理页但 emptyCache 为空（emptyCount==0），同时 unswept 积压仍在（可 sweep 出 empty region）且 committed 超标，此时反向催促 Layer 2 加速 sweep。

#### 5.4.3 释放循环

```
// 守卫条件
if !releaseAll && !shouldScavenge → return 0
if emptyCount <= lowWatermark && !trueLiveSlackHigh → return 0

// 预算
deadline = now + EMPTY_SCAVENGE_MAX_TIME (默认 4000µs)
maxRegions = releaseAll ? emptyCount : EMPTY_SCAVENGE_MAX_REGIONS_PER_SLICE (默认 256)

// 释放循环
while (emptyCount > lowWatermark || (trueLiveSlackHigh && emptyCount > 0)):
    if releasedRegions >= maxRegions → break    // 数量预算用尽
    if !releaseAll && now >= deadline → break   // 时间预算用尽
    lock(emptySweptLock_): region = emptySweptCache_.TakeHead()
    if region == null → break                   // 缓存耗尽
    emptySweptRegionCount_--
    MSRegionManager::ReclaimEmptyRegionToReleased(region)
    totalMSRegionBytes_ -= MS_REGION_SIZE
    releasedRegions++
```

**`ReclaimEmptyRegionToReleased()`** 的完整语义：
1. 检查 `region->regionType == MS_REGION_EMPTY_CACHE`
2. `region->Reset()` — 清空全部元数据，type 变为 `MS_REGION_FREE`
3. `freeManager_.ReclaimRegionsReleased(regionIdx, 1)` — 归还 CartesianTree releasedTree
4. FreeManager 内部执行 `madvise(MADV_DONTNEED)` 释放物理页面

### 5.5 停止 Scavenge 的条件

后台 Scavenge 有**四种退出方式**：

| 退出条件 | 检查位置 | 含义 |
|---------|---------|------|
| **emptyCount ≤ lowWatermark**（且 !trueLiveSlackHigh） | 循环 while 条件 | 缓存已降到安全水位，不再释放 |
| **缓存耗尽** | `TakeHead() == nullptr` | 没有更多 empty region 可释放 |
| **数量预算用尽** | `releasedRegions >= maxRegions` (默认 256) | 单 slice 释放上限 |
| **时间预算用尽** | `now >= deadline` (默认 4000µs) | 单 slice 时间上限 |

**退出后的状态更新**（`BackgroundProcessor::ReclaimHeapGarbage()`）：

```
releasedBytes = allocator.ReclaimGarbageMemory(false)    // 执行 HandleBackgroundScavenge
pendingReclaim = allocator.HasPendingReclaimWork()       // 检查是否还需续做
shouldReclaimHeapGarbage = pendingReclaim                // 自动续传 or 清除
```

`HasPendingReclaimWork()` 检查条件与 `MaybeNotifyReclaim` 相同：`emptyCount >= highWatermark` 或 `(trueLiveSlackHigh && emptyCount > 0)`。如果仍满足，`shouldReclaimHeapGarbage` 保持 `true`，`Run()` 主循环**立即进入下一轮** scavenge（走 burst 模式）。

### 5.6 重新唤醒 Scavenge 的时机

| 时机 | 触发者 | 具体路径 |
|------|--------|---------|
| **预算暂停后自动续传** | `ReclaimHeapGarbage()` 返回 `pendingReclaim=true` | `shouldReclaimHeapGarbage` 保持 true → `Run()` burst 模式继续（最多 4 轮 / 12ms，之后 backoff 1ms） |
| **Sweep 完成后联动** | `BackgroundSweepLoop()` 检测 sweep 无 pending + HasPendingReclaimWork | `shouldReclaimHeapGarbage.store(true)` → 下一轮 Run() 执行 scavenge |
| **分配路径触发** | `TakeRegions()` → `MaybeNotifyReclaim()` | 统一 fresh/large 底层取数入口直接判定 |
| **Sweep 入池越过水位** | `PushEmptySweptRegion()` 越过 highWatermark | 直接执行 `MaybeNotifyReclaim()` |
| **idle 轮询** | `BackgroundProcessor::Run()` idle wait | 每 2000ms 超时唤醒，检查所有 work flag |

**完整的一轮 Scavenge 生命周期**：

```
GC#N Mark 完成 → Sweep 启动 → emptyCache 积累
  │
  ├─ TakeRegions / PushEmptySweptRegion
  │   └─ MaybeNotifyReclaim() → NotifyToReclaimGarbage()
  │       └─ shouldReclaimHeapGarbage = true + condvar.notify_one()
  │
  ▼
gc-helper: Run() → shouldReclaimHeapGarbage == true
  └─ ReclaimHeapGarbage()
       └─ HandleBackgroundScavenge()
            ├─ CollectBackgroundScavengeState()
            ├─ shouldPressureSweep → 反向 MaybeNotifySweep(this, candidateCls)（如需）
            └─ 释放循环：逐个 TakeHead → ReclaimEmptyRegionToReleased
                 └─ 退出：lowWatermark / 数量预算 / 时间预算 / 缓存耗尽
  │
  ├─ pendingReclaim = HasPendingReclaimWork()
  │   ├─ true  → shouldReclaimHeapGarbage 保持 true → burst 模式继续
  │   └─ false → shouldReclaimHeapGarbage = false → 进入 idle wait
  │
  ... (后续 Sweep 继续产出 empty region → 再次越过 highWatermark → 重新唤醒)
```

### 5.7 水位计算

低水位和高水位动态计算，与 AllocBuffer 数量挂钩：

```
lowWatermark  = min(max(allocBufferCount × EMPTY_CACHE_LOW_PER_BUFFER,
                        EMPTY_CACHE_LOW_FLOOR_REGIONS),
                    EMPTY_CACHE_LOW_CAP_REGIONS)
highWatermark = lowWatermark × EMPTY_CACHE_HIGH_FACTOR
```

默认值（假设 4 个 AllocBuffer）：`low = min(max(4×2, 32), 256) = 32`，`high = 64`

| 参数 | 默认值 | 说明 |
|------|--------|------|
| `EMPTY_CACHE_LOW_PER_BUFFER` | 2 | 每个 AllocBuffer 贡献的低水位 region 数 |
| `EMPTY_CACHE_LOW_FLOOR_REGIONS` | 32 | 低水位绝对下限 |
| `EMPTY_CACHE_LOW_CAP_REGIONS` | 256 | 低水位绝对上限 |
| `EMPTY_CACHE_HIGH_FACTOR` | 2 | 高水位 = 低水位 × factor |



## 6. emptySweptCache 生命周期

### 6.1 数据结构

`emptySweptCache` 是 `MSCentralRegionPool` 中的**全局无 sizeClass 空缓存池**，存储完全空闲且已脱离旧 sizeClass 的 region：

```
MSCentralRegionPool:
    MSRegionList  emptySweptCache_         // 空 region 链表
    std::mutex    emptySweptLock_           // 保护链表的互斥锁
    atomic<size_t> emptySweptRegionCount_   // 空 region 计数
    atomic<size_t> emptySweptBytes_         // 空 region 字节数
```

### 6.2 入池路径

**`PushEmptySweptRegion(region)`** — 由 BackgroundSweep 或 Lazy Sweep 调用：

```
1. region->ResetToEmptyCache()
   - ReleaseBitmaps() → 释放 allocBitmap/markBitmap 至 BitmapArena
   - sizeClass = 0xFF (INVALID), type = MS_REGION_EMPTY_CACHE
   - allocPtr = RegionStart(), 清空 slot 计数

2. lock(emptySweptLock_)
3. emptySweptCache_.Prepend(region)
4. emptySweptRegionCount_++, emptySweptBytes_ += MS_REGION_SIZE

5. if previousCount < highWatermark && newCount >= highWatermark:
       MaybeNotifyReclaim() // empty cache 越过高水位时直接做 reclaim 判定
```

### 6.3 出池路径（分配复用）

**`TakeEmptySweptRegion(cls)`** — 在 `GetRegion` P4 优先级中调用：

```
lock(emptySweptLock_): region = emptySweptCache_.TakeHead()
emptySweptRegionCount_--, emptySweptBytes_ -= MS_REGION_SIZE
RebindEmptyRegionForClass(region, cls):
    if sizeClass==cls && allocBitmap!=nullptr:
        ResetToBump()              // 保留 bitmap，快速复用
    else:
        ResetToEmptyCache()        // 释放旧 bitmap
        InitForSizeClass(cls)      // 重新分配 bitmap + 建立 class 布局
```

### 6.4 出池路径（Scavenge 释放）

**`TakeEmptySweptRegionForReclaim()`** — Layer 3 scavenge 或 InlineReclaim 调用：

```
lock(emptySweptLock_): region = emptySweptCache_.TakeHead()
emptySweptRegionCount_--, emptySweptBytes_ -= MS_REGION_SIZE
→ 调用方执行 ReclaimEmptyRegionToReleased(region)
  → region->Reset() → freeManager_.ReclaimRegionsReleased()
```

### 6.5 MSRegionInfo 状态转换全景

```
                           InitForSizeClass(cls)
  ┌──────────┐        ┌────────────────────────┐       ┌──────────────┐
  │ FREE     │───────►│ MS_REGION_THREAD_LOCAL  │──────►│ PARTIAL/FULL │
  │(released │ TakeRegion │(有活对象，参与分配)    │ Return  │(在中央池)    │
  │ tree/bump)│        └────────────────────────┘ Region │              │
  └──────────┘                                          └──────┬───────┘
       ▲                                                       │
       │ ReclaimToReleased                              SweepRegion()
       │                                                       │
  ┌────┴──────┐     PushEmptySweptRegion   ┌───────────────────▼──────┐
  │ (madvise  │◄────────────────────────── │ IsCompletelyFree         │
  │  released)│    ResetToEmptyCache()     │ → MS_REGION_EMPTY_CACHE  │
  └───────────┘                            └──────────────────────────┘
                                                  │            ▲
                                                  │            │
                                         TakeEmpty │    RebindForClass
                                         ForClass  │
                                                  ▼
                                           ┌──────────────────┐
                                           │ THREAD_LOCAL     │
                                           │ (重新分配中)      │
                                           └──────────────────┘
```

---

## 7. 环境变量调参一览

### 7.1 后台 Sweep 预算

| 环境变量 | 默认值 | 说明 |
|---------|--------|------|
| `CJ_MS_BG_SWEEP_MAX_REGIONS_PER_SLICE` | 64 | 常规模式每 slice 最大 region 数 |
| `CJ_MS_BG_SWEEP_MAX_TIME` | 2000µs | 常规模式每 slice 时间预算 |
| `CJ_MS_BG_SWEEP_PRESSURE_REGIONS_PER_SLICE` | 256 | 压力模式每 slice 最大 region 数 |
| `CJ_MS_BG_SWEEP_PRESSURE_TIME` | 8000µs | 压力模式每 slice 时间预算 |
| `CJ_MS_BG_SWEEP_PRESSURE_READY_THRESHOLD` | 4 | 任一 cls ready < 此值且有 unswept → 进入压力模式 |
| `CJ_MS_LAZY_EMPTY_CACHE_REFILL_LIMIT` | 1 | lazy sweep 中 mutator 向 emptyCache 回填的上限 |

### 7.2 emptySweptCache 水位

| 环境变量 | 默认值 | 说明 |
|---------|--------|------|
| `CJ_MS_EMPTY_CACHE_LOW_PER_BUFFER` | 2 | 低水位 = max(bufferCount × 此值, floor) |
| `CJ_MS_EMPTY_CACHE_LOW_FLOOR_REGIONS` | 32 | 低水位绝对下限 |
| `CJ_MS_EMPTY_CACHE_LOW_CAP_REGIONS` | 256 | 低水位绝对上限 |
| `CJ_MS_EMPTY_CACHE_HIGH_FACTOR` | 2 | 高水位 = 低水位 × 此值 |

### 7.3 后台 Scavenge 预算

| 环境变量 | 默认值 | 说明 |
|---------|--------|------|
| `CJ_MS_RECLAIM_TRUE_LIVE_TARGET_RATIO` | 1.2 | reclaim / empty-pressure sweep 的 committed 目标倍率；`targetBytes = min(lastTrueLive × ratio, gcThreshold)` |
| `CJ_MS_EMPTY_SCAVENGE_MAX_TIME` | 4000µs | scavenge 每 slice 时间预算 |
| `CJ_MS_EMPTY_SCAVENGE_MAX_REGIONS_PER_SLICE` | 256 | scavenge 每 slice 最大释放 region 数 |

### 7.4 HEU GC 触发

| 环境变量 | 默认值 | 说明 |
|---------|--------|------|
| `cjGCThreshold` | `initHeapSize / 2` | Collector 侧 GC 阈值上限 |
| `cjGCInterval` | 20ms | Collector 层全局最小 GC 间隔 |
| `CJ_MS_HEU_REQUEST_COOLDOWN` | 10ms | `MaybeNotifyMark()` 的 HEU 请求冷却间隔；`RECLAIM_SWEEP_EXHAUSTED` 不受此防抖限制 |

### 7.5 分配路径 Sweep 触发

| 环境变量 | 默认值 | 说明 |
|---------|--------|------|
| `CJ_MS_BG_SWEEP_NOTIFY_READY_WATERMARK` | 8 | ready < 此值且 unswept > 0 → 唤醒后台 sweep |
| `CJ_MS_INLINE_RECLAIM` | 1 (开启) | BUMP 扩展时是否同步释放一个 emptyCache region |

---

## 8. 结论

### 8.1 三层控制环落地总结

当前 MSGC 的 Sweep/Reclaim 体系完整实现了 Step9 设计的三层控制环模型：

1. **第一层（GC 触发）**：`TAKE_REGIONS` 路径按 `effectiveAlloc >= threshold` 触发；`BG_SWEEP_EXHAUSTED` 路径进入分支后直接请求 GC，并由 `CJ_MS_HEU_REQUEST_COOLDOWN` 防抖；`RECLAIM_SWEEP_EXHAUSTED` 直接请求 GC，不走该防抖。

2. **第二层（后台 Sweep）**：`BackgroundSweep` + Lazy Sweep + ForceFinishAllUnswept 三路协作，通过 sweepGen CAS 保证不重复 sweep。双预算模式（常规/压力）适应不同负载。

3. **第三层（后台 Scavenge）**：`HandleBackgroundScavenge` 基于 emptySweptCache 高低水位和 trueLiveSlack 释放，小对象 empty region 不经 dirtyTree 直接到 releasedTree。
