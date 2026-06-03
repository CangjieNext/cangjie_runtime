# Step12 后台通知链与 GC Helper 执行路径详解

> 基于当前 `runtime/src/Heap` 代码状态整理，聚焦四类通知入口：
>
> - `MaybeNotifyMark()`
> - `MaybeNotifySweep()`
> - `MaybeNotifyReclaim()`
> - `NotifyToFeedAllocBuffers()`
>
> 以及后台 `sweep / reclaim / feed` 的真实执行路径。

---

## 1. 总体关系

当前 MSGC 里，“通知”和“真正执行后台动作”是两层：

1. **通知层**
   - 决定要不要发起某类后台工作
   - 主要在 `MSRegionManager`、`AllocBufferMS`、`CentralRegionPool` 中

2. **执行层**
   - 由 `BackgroundProcessor` 的 `gc-helper` 线程统一消费
   - 真正调用 `BackgroundSweep()`、`ReclaimGarbageMemory()`、`BackgroundFillAllocCaches()`

可以先把关系看成：

```text
Mutator / CentralPool / AllocBuffer
    -> notify mark / sweep / reclaim / feed
    -> BackgroundProcessor flags / direct RequestGC
    -> gc-helper thread loop
    -> sweep / reclaim / feed
```

---

## 2. 四类通知入口

### 2.1 `MaybeNotifyMark()`

**实现位置**

- [MSRegionManager.cpp](/Users/lipeisheng/code/cangjie/cangjie/cangjie_runtime_MSGC/runtime/src/Heap/MAllocator/MSRegionManager.cpp)
- 对应声明：[MSRegionManager.h](/Users/lipeisheng/code/cangjie/cangjie/cangjie_runtime_MSGC/runtime/src/Heap/MAllocator/MSRegionManager.h)

**职责**

- 决定是否直接请求下一轮 `GC`
- 这是四类通知里唯一一个**不经过 `BackgroundProcessor` 标志位**、而是直接：

```cpp
collector.RequestGC(GC_REASON_HEU, true);
```

**当前支持的 trigger kind**

- 只保留 `TAKE_REGIONS`

**当前主逻辑**

1. 如果 `Heap::GetHeap().IsGcStarted()`，直接返回
2. 做 `HEU_REQUEST_COOLDOWN` 防抖
3. 进入 `TAKE_REGIONS` 判断
4. 若满足条件，直接 `RequestGC(GC_REASON_HEU, true)`
5. 打 `take-regions` trigger 日志

**当前 trigger 的语义**

1. `TAKE_REGIONS`
   - 分配路径主入口
   - 只根据 `effectiveAlloc >= threshold` 判定是否提前开下一轮 GC
   - `notifyMark` 不再承接 sweep/reclaim 的升级路径

**当前调用点**

- `MSRegionManager::TakeRegions()` 入口

#### `TAKE_REGIONS` 详细判断

`TAKE_REGIONS` 是 mutator 在 `MSRegionManager::TakeRegions()` 里、真正尝试从 released tree / bump 区取 region 之前的第一道 HEU 判定。

它的完整前置条件是：

1. 当前不能已经处于 GC 中：`!Heap::GetHeap().IsGcStarted()`
2. 当前不能命中 HEU 冷却窗口：

```cpp
nowNs - lastHeuRequestNs >= CJ_MS_HEU_REQUEST_COOLDOWN
```

只有过了这两关，才会进入 `TAKE_REGIONS` 的具体计算。

#### `TAKE_REGIONS` 的触发路径

代码里现在只保留一条触发路径，满足就会 `RequestGC(GC_REASON_HEU, true)`：

1. **threshold 路径**：`effectiveAlloc >= threshold`

对应公式是：

```cpp
effectiveAlloc = currentRegionBytes
threshold = gcStats.takeRegionsThreshold
thresholdTriggered = effectiveAlloc >= threshold
```

这里的 `threshold` 不再在 `MaybeNotifyMark()` 里现算，而是在上一轮 GC 结束时就已经预计算好并持久化。

#### 各个参数的含义

下面这些量都直接来自当前代码：

| 参数 | 代码来源 | 含义 |
|---|---|---|
| `currentRegionBytes` | `CentralRegionPool::GetTotalMSRegionBytes()` | 当前被占据的 region 总字节数。这里直接复用 `totalMSRegionBytes_`，不再拆开统计；它覆盖 TL / central / large / empty 这些仍占据 region 的部分。 |
| `baselineAllocatedBytes` | `gcStats.baselineAllocatedBytes` | 上一轮 GC 结束时记录下来的 `committedBytes` 快照。它表示“从上一轮 GC 刚结束到现在，active 区域基线是多少”。 |
| `lastTrueLiveBytes` | `gcStats.lastTrueLiveBytes` | 上一轮 GC 标记得到的 true-live bytes。这个值是“真实存活对象量”，不是活跃 region 总量。 |
| `lastRegionBytes` | `gcStats.lastRegionBytes` | 上一轮 GC 结束时记录下来的“被占据的 region 总字节数”快照，和 `currentRegionBytes` 口径一致。 |
| `effectiveAlloc` | `currentRegionBytes` | `TAKE_REGIONS` 第一条判断里当前使用的有效压力值，直接取当前被占据的 region 总字节数。 |
| `threshold` | `gcStats.takeRegionsThreshold` | `TAKE_REGIONS` 当前直接使用的最终门槛。它在 GC 结束时预计算，`MaybeNotifyMark()` 里只做 `effectiveAlloc >= threshold`。 |

其中 `baselineAllocatedBytes`、`lastTrueLiveBytes`、`lastRegionBytes` 和 `takeRegionsThreshold` 会在上一轮 GC 的 `postGCMark` 收尾阶段一起持久化：

```cpp
gcStats.lastTrueLiveBytes = liveBytes;
gcStats.baselineAllocatedBytes = committedBytes;
gcStats.lastRegionBytes = committedBytes;
gcStats.takeRegionsThreshold = min(regionGrowthThreshold, trueLiveGrowthThreshold);
```

这意味着 `TAKE_REGIONS` 的分配路径现在不再承担 threshold 公式计算，只消费 GC 结束时留下来的快照结果。

#### `threshold` 是怎么计算的，有什么作用

这里要区分两类 threshold：

1. `gcStats.heapThreshold` / `gcStats.GetThreshold()`
   - 这是 collector 侧的通用阈值，仍然主要用于 assist / pacing 等逻辑
2. `gcStats.takeRegionsThreshold`
   - 这是 `TAKE_REGIONS` 专用阈值
   - 在 GC 收尾时计算并缓存，分配路径只读取它

下面这节说的是 **`gcStats.takeRegionsThreshold`**。

GC 结束时会先算两条候选门槛，再取更小的那个：

```text
regionGrowthThreshold = min(lastRegionBytes * (1 + heapGrowth), gcThreshold)
trueLiveGrowthThreshold = min(lastTrueLiveBytes * (1 + heapGrowth * 10), gcThreshold)
threshold = min(regionGrowthThreshold, trueLiveGrowthThreshold)
```

其中：

- `lastRegionBytes` 是上一轮 GC 结束时的 occupied region bytes 快照
- `lastTrueLiveBytes` 是上一轮 GC 标记得到的 true-live bytes 快照
- `heapGrowth` 是运行时 heap 参数中的增长系数
- `gcThreshold` 是 runtime 维护的当前 heap size；堆增长后会同步更新到增长后的大小

如果 `lastTrueLiveBytes == 0`，实现会退化为只使用 `regionGrowthThreshold`，避免把第二条门槛算成 0。

启动期在还没有完成首轮 GC 时，`takeRegionsThreshold` 会先初始化为 `gcThreshold`，也就是当前 heap size；首轮 GC 结束后才切换到上面的预计算公式。

所以 `threshold` 本质上是：

**上一轮 region working set 增长门槛** 和 **上一轮 true-live 膨胀门槛** 两者中的更早触发点，并且绝不超过当前 heap size。

#### `gcThreshold` 现在是什么意思

当前实现里，`gcThreshold` 不再作为外部配置项使用，而是 runtime 内部维护的“当前 heap size 镜像”：

- runtime 初始化时，直接把它设置为初始 heap size
- 如果后续 committed heap 扩容，`Heap::OnHeapExtended()` 会把它同步为扩容后的 heap size
- 外部通过 `RuntimeParam.gcParam.gcThreshold`、`cjGCThreshold` 或 `MCC_SetGCThreshold` 传入的值都不会再决定它

因此现在更准确的理解是：

`gcThreshold = currentHeapSize`

它仍然保留在各种 `min(..., gcThreshold)` 公式里，但语义已经从“可配置的 GC 上限”变成了“当前 heap size 的硬边界”。

#### `threshold` 在 `TAKE_REGIONS` 中起什么作用

在 `TAKE_REGIONS` 中，它现在就是唯一的判断门槛：

```cpp
thresholdTriggered = effectiveAlloc >= threshold
```

这里的 `effectiveAlloc` 现在直接就是当前 occupied region bytes：

```cpp
effectiveAlloc = currentRegionBytes
```

也就是说，代码现在拿“当前 occupied region 总量”直接去撞 GC 上一轮已经算好的最终 threshold。

这样做的目的，是把复杂的 trigger 公式统一收口到 GC 收尾阶段，避免在高频分配路径里重复计算和维护多套现场状态。

#### `TAKE_REGIONS` 这条 threshold 和 `gcStats.GetThreshold()` 的区别

这里不要混淆两件事：

1. `TAKE_REGIONS` 当前使用的专用 threshold
   - `gcStats.takeRegionsThreshold`
   - 在上一轮 GC 结束时预计算出来，只用于 `MaybeNotifyMark(TAKE_REGIONS)`

2. collector 侧的 `gcStats.GetThreshold()`
   - 仍然服务于 assist / pacing 等其他逻辑
   - 它不是这次 `TAKE_REGIONS` 第一条门槛使用的值

所以当前 `TAKE_REGIONS` 的门槛，不应该理解成 `MaybeNotifyMark()` 当场根据某个局部值现算出来的 threshold，而应该理解成：

**上一轮 GC 完成时基于 `lastRegionBytes` 和 `lastTrueLiveBytes` 共同推导出来的最终触发门槛。**

#### threshold 路径怎么判断

分配路径只保留一条判断：

```cpp
thresholdTriggered = effectiveAlloc >= threshold
```

只要满足，就立即允许触发 GC。

这一条路径的设计目标是：

1. 用 `lastRegionBytes` 保证 region working set 的增长不能无限推迟
2. 用 `lastTrueLiveBytes` 保证“壳子涨得太快、真实 live 很小”时能更早收紧门槛
3. 把判断简化成一次 compare，减少高频分配路径里的额外逻辑

#### 最终触发条件可以概括成什么

`TAKE_REGIONS` 最终可以压缩成下面这段逻辑：

```text
未在 GC 中
并且
未命中 HEU cooldown
并且
effectiveAlloc >= threshold
=> RequestGC(GC_REASON_HEU, true)
```

换句话说，`TAKE_REGIONS` 现在只保留一套触发机制：

1. **GC 收尾时预计算最终 threshold**
2. **分配路径上用 `effectiveAlloc >= threshold` 直接触发**

#### 可调参数有哪些

`TAKE_REGIONS` 直接相关的调参项如下：

| 参数 | 默认值 | 作用 |
|---|---|---|
| `CJ_MS_HEU_REQUEST_COOLDOWN` | `10ms` | HEU 请求的最小间隔，防止连续抖动触发。 |
| `heapGrowth` | 运行时 heap 参数，默认 `0.25` | 同时决定 `regionGrowthThreshold = lastRegionBytes * (1 + heapGrowth)` 和 `trueLiveGrowthThreshold = lastTrueLiveBytes * (1 + heapGrowth * 10)`。 |
| `gcThreshold` | GC 参数上限 | 对 `threshold` 做上限裁剪。 |

其中最后两个不是 `MSRegionManager.cpp` 自己的环境变量，但它们会直接影响 `threshold`，因此也是 `TAKE_REGIONS` 的关键调节项。

#### 为什么整体这么设置

从当前实现看，`TAKE_REGIONS` 的设计目标是三件事同时成立：

1. **指标稳定**：所有阈值都在 GC 结束时基于稳定快照计算，不在高频分配路径里临时推导
2. **同时约束 region working set 和 true-live 膨胀**：谁更早达到上限，就用谁作为最终门槛
3. **路径简单**：分配路径只剩 cooldown + 单次 compare，更容易解释和验证

所以这套逻辑本质上是在平衡两类风险：

1. 触发太晚，RSS / active working set 被继续顶高
2. 触发太早，GC 周期过密、吞吐下降

---

### 2.2 `MaybeNotifySweep()`

**实现位置**

- [MSRegionManager.cpp](/Users/lipeisheng/code/cangjie/cangjie/cangjie_runtime_MSGC/runtime/src/Heap/MAllocator/MSRegionManager.cpp)

**职责**

- 决定要不要唤醒后台 sweep
- 它本身不做 sweep，只是把请求转发给 `BackgroundProcessor`

**核心行为**

1. 必须带 `centralPool` 和明确的 `cls`
2. 读取目标 `cls` 的 `BucketPressure`
3. 只有同时满足：
   - `readyCount < GetBgSweepNotifyReadyWatermark()`
   - `unsweptCount > 0`
   - 才：

```cpp
backgroundProcessor.NotifyToSweep(cls);
```

**当前调用点**

- `CentralRegionPool::MaybeNotifySweep(cls)`
- `CentralRegionPool::RefillFromCentral / GetRegion` 相关路径在 supply 紧张时回调

也就是说，后台 sweep 不再有“无条件 kick”的入口，必须是某个具体 `cls` 既缺 ready、又确实还有 unswept backlog 时才会被唤醒。

**转发后做了什么**

- `BackgroundProcessor::NotifyToSweep(cls)`
  - 记录 `preferredSweepCls_`
  - 设置 `shouldBackgroundSweep_ = true`
  - `notify_one()` 唤醒 `gc-helper`

---

### 2.3 `MaybeNotifyReclaim()`

**实现位置**

- [MSRegionManager.cpp](/Users/lipeisheng/code/cangjie/cangjie/cangjie_runtime_MSGC/runtime/src/Heap/MAllocator/MSRegionManager.cpp)

**职责**

- 决定是否要让后台 reclaim/scavenge 运行

**当前判断条件**

读取 `CentralRegionPool`：

1. `emptyCount >= CJ_MS_BG_RECLAIM_NOTIFY_EMPTY_REGIONS`
2. 默认阈值是 `100`
3. 只有满足这条，才调用：

```cpp
backgroundProcessor.NotifyToReclaimGarbage();
```

**当前调用点**

- 只在 `CentralRegionPool::PushEmptySweptRegion()` 里调用

也就是说，现在 reclaim 通知不再从 `TakeRegions()` 入口探测，也不再看 `committedBytes / targetBytes` 之类额外条件；它只关心 empty region 数量有没有达到后台回收阈值。

**转发后做了什么**

- `BackgroundProcessor::NotifyToReclaimGarbage()`
  - 设置 `shouldReclaimHeapGarbage = true`
  - 唤醒 `gc-helper`

---

### 2.4 `NotifyToFeedAllocBuffers()`

**实现位置**

- [BackgroundProcessor.cpp](/Users/lipeisheng/code/cangjie/cangjie/cangjie_runtime_MSGC/runtime/src/Heap/MCollector/BackgroundProcessor.cpp)

**职责**

- 唤醒后台补货，不属于 `MSRegionManager` 三个 `MaybeNotify*` 之一
- 这是 `AllocBufferMS` 主动请求后台给 `cachedRegion_` 提前补 spare region 的入口

**当前调用点**

- `AllocBufferMS::NotifyBackgroundFillIfNeeded(cls)`
- `AllocBufferMS::SlowAlloc(cls)` 在 mutator 慢路径中
- 典型是在：
  - 当前 `cls` 是主热点
  - 缓存 region 数量很低
  - 且当前压力级别允许 feed

**当前行为**

```cpp
preferredFeedCls_.store(cls);
shouldFeedHungryBuffers = true;
notify_one();
```

与 `sweep / reclaim` 一样，真正补货动作发生在 `gc-helper` 主循环里。

---

## 3. `gc-helper` 线程主循环

**实现位置**

- [BackgroundProcessor.cpp](/Users/lipeisheng/code/cangjie/cangjie/cangjie_runtime_MSGC/runtime/src/Heap/MCollector/BackgroundProcessor.cpp)

主循环 `Run()` 的真实顺序是：

1. 处理 finalizer
2. 如果 `shouldBackgroundSweep_` 为真，执行 `BackgroundSweepLoop()`
3. 如果 `shouldReclaimHeapGarbage` 为真，执行 `ReclaimHeapGarbage()`
4. 如果 `shouldFeedHungryBuffers` 为真，执行 `BackgroundFillAllocCaches()`
5. 若有立即继续的工作，则 burst 模式继续一小段
6. 否则 sleep / condvar wait

也就是说，后台优先级大致是：

```text
finalizer > sweep > reclaim > feed
```

这也是为什么 `feed` 会被认为是最容易被高压态抑制的一层。

---

## 4. 后台 Sweep 的实现

### 4.1 入口

执行入口：

- `BackgroundProcessor::BackgroundSweepLoop()`
- `Allocator::BackgroundSweep(preferredCls)`
- `RegionSpaceMS::BackgroundSweep(preferredCls)`
- `CentralRegionPool::BackgroundSweep(preferredCls)`

### 4.2 `BackgroundSweepLoop()` 做的事

1. 取走 `preferredSweepCls_`
2. 调 `allocator.BackgroundSweep(preferredCls)`
3. 根据 `allocator.HasPendingSweepWork()` 回写 `shouldBackgroundSweep_`

这里不再做两类事情：

1. 不再因为 sweep 结果去补发 reclaim 通知
2. 不再因为 active pressure / sweep exhausted 去触发 `notifyMark`

### 4.3 `CentralRegionPool::BackgroundSweep()` 真正做的事

1. 使用固定的 sweep slice 预算
2. 构建 class 扫描顺序：
   - 优先 `preferredCls`
   - 再按内部顺序扫其他类

3. 分别扫：
   - `partialUnswept`
   - `fullUnswept`

4. 每扫完一个 region：
   - 如果完全 free，进 `emptySweptCache`
   - 否则回到 `partialSwept/fullSwept`

5. 一轮结束后只做两件事：
   - 打 `BackgroundSweep slice` 日志
   - 根据是否还有 unswept backlog 回写 `shouldBackgroundSweep_`

**一句话**

后台 sweep 的职责现在就是：

**按优先级扫一片 unswept region，把它们变成 ready region 或 empty region；不负责直接退 OS，也不负责升级成 mark。**

---

## 5. 后台 Reclaim 的实现

### 5.1 入口

执行入口：

- `BackgroundProcessor::ReclaimHeapGarbage()`
- `Allocator::ReclaimGarbageMemory(false)`
- `RegionSpaceMS::ReclaimGarbageMemory(false)`
- `CentralRegionPool::HandleBackgroundScavenge(false)`

### 5.2 `ReclaimHeapGarbage()` 做的事

1. 调 `allocator.ReclaimGarbageMemory(false)`
2. 本轮 reclaim 结束后直接清掉 `shouldReclaimHeapGarbage`

也就是说，后台 reclaim 现在是一种“收到通知就执行一片”的 slice 动作，而不是会在后台线程里按旧条件连续自我放大。

### 5.3 `HandleBackgroundScavenge(false)` 真正做的事

当前 reclaim 已经被收敛成单段式：

1. 读取当前 `emptySweptCache_` 的 region 数量
2. 如果不是 `releaseAll`，本轮目标就是释放当前 empty region 的一半
3. 逐个从 `emptySweptCache_` 取 region
4. 调 `MSRegionManager::ReclaimEmptyRegionToReleased(region)`
5. committed bytes 随之减少

这里不再做两件事：

1. 不再做 `FastReclaimBySweeping()`
2. 不再从 reclaim 路径升级到 `notifyMark`

**一句话**

后台 reclaim 的职责是：

**只从现有 `emptySweptCache_` 里按 slice 退页；默认一次以“当前 empty region 的一半”为目标。**

---

## 6. 后台 Feed 的实现

### 6.1 入口

执行入口：

- `BackgroundProcessor::BackgroundFillAllocCaches()`
- `Allocator::BackgroundFillAllocCaches(preferredCls)`
- `RegionSpaceMS::BackgroundFillAllocCaches(preferredCls)`
- `CentralRegionPool::BackgroundFillAllocCaches(preferredCls)`
- `AllocBufferMS::BackgroundFillCache(preferredCls, reclaimPressure, pressureLevel)`

### 6.2 `BackgroundFillAllocCaches()` 做的事

1. 如果当前还有 pending reclaim work
   - 先 `WaitForActiveBackoff(...)`
2. 清掉 `shouldFeedHungryBuffers`
3. 取走 `preferredFeedCls_`
4. 真正执行 `allocator.BackgroundFillAllocCaches(preferredCls)`
5. 把结果累计到 `pendingFeedStats_`

### 6.3 `CentralRegionPool::BackgroundFillAllocCaches()` 做的事

1. 计算：
   - `reclaimPressure`
   - `activeWorkingSetRegions`
   - `pressureLevel`

2. 如果 `pressureLevel == EXTREME`
   - 直接返回，不补货

3. 否则遍历所有 `AllocBufferMS`
   - 调每个 buffer 的 `BackgroundFillCache(...)`
   - 汇总 `filledClasses / filledRegions`

### 6.4 `AllocBufferMS::BackgroundFillCache(...)` 的职责

它是真正的“给每个 `cls` 填 cached spare region”的地方：

1. 根据 `preferredCls`、热点程度、优先级排序 class
2. 根据 `pressureLevel` 和 `reclaimPressure` 决定 `targetSlots`
3. 对每个 `cls`
   - 如果 cache 不满
   - 且该类允许补
   - 就 `RefillFromCentral(cls, true)` 拿 region
   - 填进 `cachedRegion_[cls][slot]`

4. 打 `feed cls` 细粒度日志
5. 最后由 `BackgroundProcessor::FlushBackgroundFeedStats()` 汇总成一条：

```text
[MS-GC] feed alloc caches: buffers=... touchedBuffers=... filledClasses=... filledRegions=...
```

### 6.5 当前 feed 压力调节行为

当前代码已经做了分级：

- `LOW`
  - 正常补
- `MEDIUM`
  - 主热点 2，次热点 1
- `HIGH`
  - 只给主热点留 1 个
- `EXTREME`
  - 后台不补
  - mutator 侧 `NotifyBackgroundFillIfNeeded()` 也抑制

**一句话**

后台 feed 的职责是：

**不是分配对象，而是给 `AllocBufferMS` 提前补少量 spare region，减少 mutator 慢路径。**

---

## 7. 调用点总表

### 7.1 `notifyMark`

| 调用点 | 位置 | 触发种类 |
|---|---|---|
| `MSRegionManager::TakeRegions()` | 分配入口 | `TAKE_REGIONS` |

### 7.2 `notifySweep`

| 调用点 | 位置 | 目的 |
|---|---|---|
| `CentralRegionPool::MaybeNotifySweep(cls)` | ready 紧张 | 唤醒后台 sweep |

### 7.3 `notifyReclaim`

| 调用点 | 位置 | 目的 |
|---|---|---|
| `CentralRegionPool::PushEmptySweptRegion()` | empty cache 增长路径 | `emptyRegion` 达到阈值后唤醒后台 reclaim |

### 7.4 `notifyFeed`

| 调用点 | 位置 | 目的 |
|---|---|---|
| `AllocBufferMS::NotifyBackgroundFillIfNeeded(cls)` | mutator 慢路径 / 热点类 miss | 唤醒后台给 `cachedRegion_` 补 spare |

---

## 8. 当前实现的关键区别

把四类通知放在一起看，当前代码的分层已经比较清楚：

- `mark`：决定**下一轮 GC 周期**
- `sweep`：决定**旧 region 何时变成 ready / empty**
- `reclaim`：决定**empty region 何时退给 OS**
- `feed`：决定**alloc buffer 何时提前补 spare cache**

它们不是一回事，也不是谁替代谁，而是：

```text
mark 负责周期
sweep 负责供给
reclaim 负责退页
feed 负责局部缓存
```

而 `BackgroundProcessor` 把 `sweep / reclaim / feed` 三条后台链统一收口到同一个 `gc-helper` 线程里顺序执行。

当前这一版和前面的实现差异很大，关键点可以直接记三条：

1. `notifyMark` 现在只剩 `TAKE_REGIONS`
2. `notifySweep` 必须是某个具体 `cls` 同时满足“ready 低 + unswept>0”
3. `notifyReclaim` 只在 `PushEmptySweptRegion()` 达到阈值后触发，后台只退现有 empty 的一半

---

## 9. 结论

当前 MSGC 的后台通知链可以概括成：

1. `MaybeNotifyMark()`：直接 HEU 请求 GC
2. `MaybeNotifySweep()`：唤醒后台 sweep
3. `MaybeNotifyReclaim()`：唤醒后台 reclaim
4. `NotifyToFeedAllocBuffers()`：唤醒后台 feed

后台线程统一执行：

1. `BackgroundSweepLoop()`
2. `ReclaimHeapGarbage()`
3. `BackgroundFillAllocCaches()`

所以从实现结构上看，当前系统已经不是“分配失败了才临时补救”，而是：

**用一套更窄的通知链把 GC 周期、ready 供给、empty 退页和 alloc buffer 缓存补货串起来：mark 只管 `takeRegions`，sweep/reclaim 只做各自的 slice 工作，不再互相升级。**
