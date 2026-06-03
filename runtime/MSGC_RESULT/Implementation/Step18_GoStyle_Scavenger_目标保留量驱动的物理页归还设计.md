# Step18：Go 风格 Scavenger — 目标保留量驱动的物理页归还设计

> 注：Step16 为 pageAlloc 页堆设计，Step17 为 Barrier/Assist/GCThread/GlobalBuf 整体设计。本文档为 Step18。

---

## 1. 背景与问题

### 1.1 当前 BackgroundReclaim 的问题

目前 `BackgroundReclaimLoop` → `BackgroundReclaim()` → `ReclaimGarbageMemory()` 把两件完全不同的事情混在一起：

| 步骤 | 内容 | 系统调用 | 性质 |
|------|------|---------|------|
| Step A: `HandleBackgroundScavenge` | 把 `emptySweptCache_` 里的 region 转移到 FreeManager（标记为 free） | 无 | 元数据操作，必须尽快执行，让 region 可被 `TakeRegions` 复用 |
| Step B: `ScavengeFreeRegions` | 对 FreeManager 中 free 但未归还的 region 调 madvise/mmap | 有（madvise/mmap） | 物理页归还，应目标驱动、分片执行 |
| Step C: `ShrinkHeap` | 收缩 `committedEnd_` | 有（madvise/mmap） | 虚拟地址段归还，应配合 Step B 一起目标驱动 |

**关键问题**：

1. **没有归还目标**：Step B 能归还多少就归还多少（每次最多 `segmentSize / regionSize` 个 region，64MB～512MB），不考虑下轮 GC 是否立刻需要这些页。
2. **缺乏速率控制**：一次调用可归还 64MB～512MB，RSS 骤降 → 下轮分配集中 page fault → 延迟毛刺。
3. **Step A 被 Step B 拖慢**：两者在同一个 `ReclaimGarbageMemory` 里串行，Step B 的 deadline/限额会影响 Step A 的完成时机。

### 1.2 Go 的做法（对照）

Go 在 `runtime/mgcscavenge.go` 中有独立的 `bgscavenge` goroutine：

- 触发条件：`heapRetained > heapGoal`（当前物理占用超过目标保留量）
- 每次归还量：`scavengeQuantum = 64KB`，归还后 sleep 一段时间，目标在约 1 秒内平滑完成
- 归还目标：`heapGoal = heapMarked × (1 + GOGC/100)`，精确到字节

本方案借鉴 Go 的核心思想，结合 MSGC 现有架构，做适配性改造。

---

## 2. 核心概念定义

### 2.1 三个内存量

```
heapInUse  = totalMSRegionBytes_ - emptySweptBytes_
           = 含有存活对象的 region 字节数（sweep 之后非空 region 的总大小）

heapFree   = FreeManager.hotFreeBytes
           = (freeRegionCount_ - scavengedRegionCount_) × MS_REGION_SIZE
           = FreeManager 中已标记 free 但物理页尚未归还的字节数

heapRetained = heapInUse + heapFree
             = runtime 当前从 OS 实际持有的物理页字节数
             = totalMSRegionBytes_ + FreeManager.hotFreeBytes
             （无论 HandleBackgroundScavenge 是否已运行，此等式始终成立）
```

说明：
- `emptySweptCache_` 中的 region 在 `HandleBackgroundScavenge` 运行前仍计入 `totalMSRegionBytes_`，此时 `FreeManager.hotFree = 0`；运行后 `totalMSRegionBytes_` 减少，`hotFree` 增加，`heapRetained` 保持不变。等式恒成立。

### 2.2 heapGoal：目标保留量

```
heapGoal = survivingRegionBytes × (1 + headroomFactor)
         = "本轮 GC 存活对象占据的 region 字节数" × (1 + 增长系数)
```

**含义**：下一轮 GC 开始前我们"预计"会用到的堆大小上限。只要 `heapRetained <= heapGoal`，就无需归还物理页。

`headroomFactor` 与现有 `heapGrowth` 参数相同（默认 1.0），可复用，不引入新参数。

### 2.3 toScavenge：本次需要归还的量

```
toScavenge = heapRetained - heapGoal
           = heapFree - heapInUse × headroomFactor
```

若 `toScavenge <= 0`，无需归还任何物理页。

---

## 3. survivingRegionBytes 的计算方案

这是本方案的关键问题：GC mark 结束时（`UpdateGCStats` 调用时），sweep 尚未开始，无法精确知道哪些 region 最终会被 sweep 为空。

### 3.1 在 GC 结束（sweep 完成）时统一刷新

`heapGoal` **只在 GC 完整结束后（后台 sweep 全部完成时）刷新一次**，不做 mark-end 的保守估计。原因：

- mark 阶段结束时，尚不知道哪些 region 会被 sweep 为空，此时计算值偏高，会导致 scavenger 目标过于宽松
- sweep 完成后，`emptySweptCache_` 已充分积累，`totalMSRegionBytes_ - emptySweptBytes_` 就是精确的 `survivingRegionBytes`
- 这个值同时作为**下次 GC 触发阈值**和**scavenger 归还目标**，两者统一，无需维护两套数值

刷新时机：`BackgroundSweepLoop` 检测到 `HasPendingSweepWork()` 返回 false（当轮 sweep 全部完成）：

```cpp
// sweep 完成 → survivingRegionBytes 精确可知
size_t nonEmptyBytes = space.GetHeapInUseBytes();
// GetHeapInUseBytes() = totalMSRegionBytes_ - emptySweptBytes_
gcStats.lastSurvivingRegionBytes = nonEmptyBytes;
double heapGrowth = CangjieRuntime::GetHeapParam().heapGrowth;
gcStats.heapGoal = static_cast<size_t>(nonEmptyBytes * (1.0 + heapGrowth));
// heapGoal 同时用于：
//   1. 下次 GC 触发判断（与 takeRegionsThreshold 语义对齐）
//   2. BackgroundScavengeLoop 的归还目标
```

### 3.2 为什么不用 liveBytes（trueLiveBytes）作为 base

`liveBytes` 是标记 bitmap 统计的对象字节数（不包含对象间空洞/padding），远小于 `totalMSRegionBytes_`：

```
liveBytes = Σ(对象实际大小)
nonEmptyRegionBytes = 含有≥1个存活对象的 region 数 × 32KB
```

同样 60MB 的存活对象，可能分散在 300 个 region 里（utilization = 60MB / (300 × 32KB) ≈ 63%）。
若用 `liveBytes × (1 + factor)` 计算 `heapGoal`，会严重低估，导致 scavenger 过度归还物理页，触发大量 page fault。

因此必须用 **region 粒度** 的存活量（`nonEmptyRegionBytes`），而不是字节粒度的 `liveBytes`。

---

## 4. 架构设计

### 4.1 新增 BackgroundScavengeLoop，移除 BackgroundReclaimLoop

```
BackgroundProcessor::Run()
  ├─ BackgroundSweepLoop()        ← 已有，sweep 扫空 region
  ├─ BackgroundScavengeLoop()     ← 新增，集成 Step A（按需）+ Step B+C（分片归还物理页）
  └─ BackgroundFeedLoop()         ← 已有，补充 AllocBuffer 热缓存
```

`BackgroundReclaimLoop` 及其对应的 `ShouldNotifyBackgroundReclaim`/`MaybeNotifyReclaim`/`NotifyToReclaim` 一并移除。
Step A（`HandleBackgroundScavenge`：`emptySweptCache_` → FreeManager）按需内联到 `BackgroundScavengeLoop` 中执行：每次 slice 前检查 FreeManager 中热空闲是否不足，不足时先将 `emptySweptCache_` 的一半转入 FreeManager，再继续 Step B 归还到 OS。

### 4.2 触发机制

触发点选在 **`MSRegionManager::TakeRegions`**（分配路径），而非 `PushEmptySweptRegion`（归还路径）。

理由：`TakeRegions` 是 allocation 需要拿新 region 的热路径，此时堆的实际使用量刚发生变化，是判断"当前 heapRetained 是否超过 heapGoal"的自然时机；且 `TakeRegions` 本身在 `MSRegionManager` 层，可直接访问 `FreeManager.GetHotFreeBytes()`，无需跨层。

```
allocation 申请新 region
    → MSCentralRegionPool::TakeRegion()
         → MSRegionManager::TakeRegions()
              ← 从 FreeManager 取出 region
              └─ 新增：MaybeNotifyScavenge()
                   → 内部完成触发判断 + GC 守卫
                   → BackgroundProcessor::NotifyToScavenge()
                        → shouldScavengeHeap_.exchange(true) + Notify()
```

触发判断内联在 `MaybeNotifyScavenge` 中（不再拆分为独立的 `ShouldNotify` 函数）：

```cpp
void MSRegionManager::MaybeNotifyScavenge()
{
    const GCStats& gcStats = Heap::GetHeap().GetCollector().GetGCStats();
    size_t goal = gcStats.heapGoal.load(std::memory_order_acquire);
    if (goal == 0) return;  // 未经过任何 GC，不触发

    // heapRetained = totalMSRegionBytes_ + hotFreeBytes（实时计算，无 snapshot 字段）
    size_t totalBytes   = GetTotalMSRegionBytes();
    size_t hotFreeBytes = freeManager_.GetHotFreeBytes();
    size_t heapRetained = totalBytes + hotFreeBytes;

    // hysteresis=10%，防止在 goal 附近频繁触发
    static constexpr double kHysteresis = 0.1;
    if (heapRetained <= static_cast<size_t>(goal * (1.0 + kHysteresis))) return;

    // GC 正在进行时不触发（等 sweep 完成刷新 heapGoal 后再评估）
    if (Heap::GetHeap().GetCollector().IsGcRunning()) return;

    BackgroundProcessor::GetInstance().NotifyToScavenge();
}
```

### 4.3 BackgroundScavengeLoop：分片执行

```cpp
bool BackgroundProcessor::BackgroundScavengeLoop()
{
    if (!IsBackgroundScavengeEnabled()) {
        shouldScavengeHeap_.store(false, std::memory_order_release);
        return false;
    }

    RegionSpaceMS& space = static_cast<RegionSpaceMS&>(Heap::GetHeap().GetAllocator());

    // 每次 loop 重新计算当前差值（sweep 进展中 heapGoal 可能已更新）
    size_t retained = space.GetHeapRetainedBytes();
    size_t goal     = space.GetHeapGoalBytes();   // = gcStats.heapGoal

    if (retained <= goal) {
        shouldScavengeHeap_.store(false, std::memory_order_release);
        return false;
    }

    size_t toScavenge = retained - goal;

    // Step A（按需）：若 FreeManager 中热空闲不足以覆盖归还目标，
    // 且 emptySweptCache_ 中仍有可转移的空 region，则将其一半转入 FreeManager，
    // 使 Step B 立即有足够的物理页可归还。
    if (space.GetHotFreeBytes() < toScavenge && space.HasEmptySweptRegions()) {
        space.FlushHalfEmptySweptToFreeManager();
    }

    // Step B：执行本次 slice 的物理页归还（ScavengeFreeRegionsUpTo）
    // per-iteration quantum：把 toScavenge 分 kScavengeRounds 轮完成
    // Run() 的 burst loop 大约每轮 1~2ms，kRounds=8 → 约 8~16ms 完成
    // 额外多归还 kOvershootFactor（5%），防止 goal 附近频繁重新触发
    static constexpr size_t kScavengeRounds = 8;
    static constexpr double kOvershootFactor = 0.1;
    size_t quantum = static_cast<size_t>((toScavenge / kScavengeRounds) * (1.0 + kOvershootFactor));
    quantum = std::max(quantum, MS_REGION_SIZE);           // 至少归还 1 个 region
    quantum = std::min(quantum, GetBgScavengeMaxQuantum()); // 每次上限（默认 segmentSize）

    BackgroundScavengeStats sliceStats = space.BackgroundScavenge(quantum);
    if (sliceStats.HasWork()) {
        pendingScavengeStats_.Merge(sliceStats);
    }

    // 每次都用最新的 heapRetained vs heapGoal 动态判断（不缓存旧值）
    // 好处：sweep 推进时 heapGoal 可能已被 sweep-done 回调刷新为精确值
    bool hasMore = space.HasPendingScavengeWork();
    if (!hasMore) {
        // Step C：ShrinkHeap 在全部 slice 完成后执行一次
        // 避免每个 slice 都触发 mmap/madvise 系统调用
        space.BackgroundShrinkHeap();

        if (pendingScavengeStats_.HasWork()) {
            VLOG(REPORT,
                 "[MS-GC] scavenge done: slices=%zu regions=%zu bytes=%zuB elapsed=%zuus",
                 pendingScavengeStats_.slices,
                 pendingScavengeStats_.releasedRegions,
                 pendingScavengeStats_.releasedBytes,
                 static_cast<size_t>(pendingScavengeStats_.elapsedUs));
            pendingScavengeStats_.Reset();
        }
        shouldScavengeHeap_.store(false, std::memory_order_release);
    }
    return hasMore;
}
```

### 4.4 BackgroundScavenge（单次 slice 实现）

```cpp
// MSRegionSpace.cpp

// 单次 slice：仅做 Step B（物理页归还），不调 ShrinkHeap
BackgroundScavengeStats RegionSpaceMS::BackgroundScavenge(size_t quantumBytes)
{
    uint64_t t0 = TimeUtil::MicroSeconds();

    size_t released = 0;
    if (msRegionManager_ != nullptr) {
        released = msRegionManager_->ScavengeFreeRegionsUpTo(quantumBytes, false);
        // ShrinkHeap 不在每次 slice 里调——由 BackgroundScavengeLoop 全部完成后调一次
    }

    BackgroundScavengeStats s;
    s.slices = 1;
    s.releasedRegions = released / MS_REGION_SIZE;
    s.releasedBytes = released;
    s.elapsedUs = TimeUtil::MicroSeconds() - t0;
    return s;
}

// Step C：ShrinkHeap，由 BackgroundScavengeLoop 在 hasMore=false 后调一次
void RegionSpaceMS::BackgroundShrinkHeap()
{
    if (msRegionManager_ != nullptr) {
        msRegionManager_->ShrinkHeap();
    }
}

// HasPendingScavengeWork：每次动态计算最新 heapRetained vs heapGoal
// 不缓存旧值，确保每轮都能感知 sweep-done 回调刷新 heapGoal 后的最新状态
bool RegionSpaceMS::HasPendingScavengeWork() const
{
    return GetHeapRetainedBytes() > GetHeapGoalBytes();
}
```

### 4.5 ReclaimGarbageMemory 的精简

移除 `BackgroundReclaimLoop` 后，`ReclaimGarbageMemory` 的日常（`releaseAll=false`）调用入口也一并消失。该函数仅保留 `releaseAll=true`（STW 全量归还）路径，供 GC 强制触发时使用：

```cpp
size_t RegionSpaceMS::ReclaimGarbageMemory(bool releaseAll)
{
    if (msRegionManager_ == nullptr || msCentralPool_ == nullptr) return 0;

    // BackgroundReclaimLoop 已移除；Step A（emptySweptCache → FreeManager）
    // 现在按需集成在 BackgroundScavengeLoop 中（FlushHalfEmptySweptToFreeManager）。

    // releaseAll=true：STW 全量路径，一次性归还所有空 region 和物理页
    if (releaseAll) {
        (void)msCentralPool_->HandleBackgroundScavenge(true);  // 全量 Step A
        msRegionManager_->ScavengeFreeRegions(true);           // 全量 Step B
        msRegionManager_->ShrinkHeap();                        // Step C
    }
    return 0;
}
```

---

## 5. GC 结束（sweep 完成）时统一刷新 heapGoal

`heapGoal` 的**唯一刷新时机**是当前 GC 轮的后台 sweep 全部完成时（`HasPendingSweepWork()` 返回 false）。此时 `nonEmptyBytes` 是精确的 survivingRegionBytes，`heapGoal` 同时承担两个职责：

1. **下次 GC 触发阈值**：与 `takeRegionsThreshold` 语义对齐，表示"堆使用量增长到此时触发下次 GC"
2. **Scavenger 归还目标**：`BackgroundScavengeLoop` 用此决定是否继续归还物理页

两者统一为同一个值，不需要维护两套阈值。

```cpp
// BackgroundProcessor.cpp - BackgroundSweepLoop() 内
bool hasMore = HasPendingSweepWork();
if (!hasMore && pendingSweepStats_.HasWork()) {
    // ... 打印 sweep done 日志 ...

    // *** GC 结束：统一刷新 heapGoal ***
    // heapGoal = survivingRegionBytes × (1 + heapGrowth)
    // 同时作为：① 下次 GC 触发阈值  ② Scavenger 归还目标
    RegionSpaceMS& space = static_cast<RegionSpaceMS&>(Heap::GetHeap().GetAllocator());
    size_t nonEmptyBytes = space.GetHeapInUseBytes();
    // GetHeapInUseBytes() = totalMSRegionBytes_ - emptySweptBytes_
    // sweep 已完成，emptySweptBytes_ 稳定，此值精确
    if (nonEmptyBytes > 0) {
        double heapGrowth = CangjieRuntime::GetHeapParam().heapGrowth;
        size_t newGoal = static_cast<size_t>(nonEmptyBytes * (1.0 + heapGrowth));
        GCStats& gcStats = Heap::GetHeap().GetCollector().GetGCStats();
        gcStats.lastSurvivingRegionBytes = nonEmptyBytes;
        gcStats.heapGoal = newGoal;
        VLOG(REPORT,
             "[MS-GC] GC done, heapGoal updated: nonEmpty=%zuB survivingRegions=%zu goal=%zuB",
             nonEmptyBytes, nonEmptyBytes / MS_REGION_SIZE, newGoal);

        // 刷新 heapGoal 后立即评估是否需要启动 scavenge
        space.MaybeNotifyScavenge();
    }
}
```

---

## 6. UpdateGCStats 不修改 heapGoal

`UpdateGCStats()` 在 mark 阶段结束时调用，此时 sweep 尚未开始，`survivingRegionBytes` 无法精确计算，因此**不在这里写入 `heapGoal`**。`heapGoal` 的唯一刷新点是 Section 5 中 sweep 完成时的回调。

`UpdateGCStats` 继续维护已有的 `takeRegionsThreshold`（HEU GC 触发阈值）和 `liveBytes` 统计，不做额外修改。sweep 完成后写入的 `gcStats.heapGoal` 与 `takeRegionsThreshold` 语义对齐：

```
heapGoal            = survivingRegionBytes × (1 + heapGrowth)   ← sweep-done 写入，region 粒度精确值
takeRegionsThreshold ≈ liveBytes × (1 + heapGrowth)             ← UpdateGCStats 写入，字节粒度估算
```

两者均为"存活量 × (1+系数)"，差别仅在计量单位（region 粒度 vs 字节粒度）。`heapGoal` 使用 region 粒度，与 `GetHeapRetainedBytes()`（也是 region 粒度）直接可比。

---

## 7. 需要新增的数据结构

### 7.1 GCStats（新增两个字段）

```cpp
// Collector.h 或 GCStats 定义处
struct GCStats {
    // ... 已有字段 ...
    size_t heapGoal = 0;                  // 目标保留量（随 sweep 进展逐步精化）
    size_t lastSurvivingRegionBytes = 0;  // 上轮 sweep 后非空 region 字节数
};
```

### 7.2 BackgroundScavengeStats（新增，与 BackgroundReclaimStats 对称）

```cpp
struct BackgroundScavengeStats {
    size_t slices = 0;
    size_t releasedRegions = 0;
    size_t releasedBytes = 0;
    uint64_t elapsedUs = 0;
    bool HasWork() const { return releasedBytes > 0; }
    void Merge(const BackgroundScavengeStats& s) {
        slices += s.slices;
        releasedRegions += s.releasedRegions;
        releasedBytes += s.releasedBytes;
        elapsedUs += s.elapsedUs;
    }
    void Reset() { *this = {}; }
};
```

---

## 8. 需要修改的文件清单

### 8.1 新增功能性接口

| 文件 | 新增内容 | 说明 |
|------|---------|------|
| `MSFreeManager.h / .cpp` | `GetHotFreeBytes()` | free - scavenged 的 region 字节数 |
| `MSFreeManager.h / .cpp` | `ScavengeFreeRegionsUpTo(upToBytes, releaseAll)` | 带 budget 截止的归还 |
| `MSRegionManager.h / .cpp` | `GetHotFreeBytes()` delegate | 委托给 FreeManager |
| `MSRegionManager.h / .cpp` | `ScavengeFreeRegionsUpTo(upToBytes, releaseAll)` delegate | 委托给 FreeManager |
| `MSRegionSpace.h / .cpp` | `GetHeapInUseBytes()` | `totalMSRegionBytes_ - emptySweptBytes_` |
| `MSRegionSpace.h / .cpp` | `GetHeapRetainedBytes()` | `totalMSRegionBytes_ + hotFreeBytes` |
| `MSRegionSpace.h / .cpp` | `GetHeapGoalBytes()` | 从 `gcStats.heapGoal` 读取 |
| `MSRegionSpace.h / .cpp` | `BackgroundScavenge(quantum)` | Step B 分片实现（仅 ScavengeFreeRegionsUpTo，不含 ShrinkHeap） |
| `MSRegionSpace.h / .cpp` | `BackgroundShrinkHeap()` | Step C，由 BackgroundScavengeLoop 在全部 slice 完成后调一次 |
| `MSRegionSpace.h / .cpp` | `HasPendingScavengeWork()` | 动态计算 `heapRetained > heapGoal`（不缓存旧值） |
| `MSRegionSpace.h / .cpp` | `MaybeNotifyScavenge()` | 对外暴露，供 sweep-done 回调用 |
| `MSRegionSpace.h / .cpp` | `GetHotFreeBytes()` | 委托给 MSRegionManager → FreeManager |
| `MSRegionSpace.h / .cpp` | `HasEmptySweptRegions()` | 检查 emptySweptCache_ 是否非空 |
| `MSRegionSpace.h / .cpp` | `FlushHalfEmptySweptToFreeManager()` | 将 emptySweptCache_ 的一半转入 FreeManager（Step A 按需版） |
| `MSRegionManager.h / .cpp` | `MaybeNotifyScavenge()` | 触发判断 + GC 守卫 + 调 BackgroundProcessor（内联原 ShouldNotify 逻辑） |
| `Allocator.h` | `virtual BackgroundScavengeStats BackgroundScavenge(size_t)` | 虚函数声明 |
| `Allocator.h` | `virtual bool HasPendingScavengeWork()` | 虚函数声明 |

### 8.2 修改已有文件

| 文件 | 修改内容 |
|------|---------|
| `GCStats` 定义处 | 新增 `heapGoal`、`lastSurvivingRegionBytes` |
| `MSRegionManager.cpp` `TakeRegions` | 取出 region 后直接调 `MaybeNotifyScavenge()` |
| `MSRegionSpace.cpp` `ReclaimGarbageMemory` | 仅保留 `releaseAll=true` 的 STW 全量路径；日常增量归还由 BackgroundScavengeLoop 负责 |
| `BackgroundProcessor.h` | 新增 `shouldScavengeHeap_` 原子标志、`pendingScavengeStats_`；**移除** `shouldReclaimHeap_`/`pendingReclaimStats_` |
| `BackgroundProcessor.cpp` `Run()` | 新增 `BackgroundScavengeLoop()` 调用；**移除** `BackgroundReclaimLoop()` 调用 |
| `BackgroundProcessor.cpp` | 实现 `BackgroundScavengeLoop()`、`NotifyToScavenge()`；**移除** `BackgroundReclaimLoop()`、`NotifyToReclaim()`、`MaybeNotifyReclaim()` |
| `BackgroundProcessor.cpp` `BackgroundSweepLoop()` | sweep-done（GC 结束）时刷新 `heapGoal` 并触发 scavenge notify |
| `BackgroundProcessor.cpp` `HasPendingWakeWork()` | 加入 `shouldScavengeHeap_` 检查；移除 `shouldReclaimHeap_` 检查 |
| `MSCentralRegionPool.cpp` `PushEmptySweptRegion` | **移除** `ShouldNotifyBackgroundReclaim()` → `MaybeNotifyReclaim()` 触发调用 |

---

## 9. 关键常量与环境变量

| 名称 | 默认值 | 含义 |
|------|-------|------|
| `kScavengeRounds` | 8 | 把 toScavenge 分几轮完成 |
| `kOvershootFactor` | 0.05（5%） | 每轮多归还 5%，防止在 goal 附近频繁触发 |
| `kHysteresis` | 0.1（10%） | 触发阈值比 goal 高 10%，防止抖动 |
| `CJ_MS_BG_SCAVENGE` | true | 是否启用后台 scavenge |
| `CJ_MS_BG_SCAVENGE_ROUNDS` | 8 | 可覆盖 kScavengeRounds |

---

## 10. 调用流程全景

```
 ┌─ GC mark 结束
 │     MarkSweepCollector::UpdateGCStats()
 │     → 维护已有 takeRegionsThreshold / liveBytes 统计
 │     → heapGoal 不在此处写入（sweep 未完成，无法精确）
 │
 ├─ 并发 Sweep 运行
 │     Sweeper 发现 region 全空
 │     → CentralRegionPool::PushEmptySweptRegion()  [不再触发 ReclaimLoop]
 │
 ├─ Allocation 请求新 region
 │     → MSRegionManager::TakeRegions()
 │          └─ MaybeNotifyScavenge()  [新增，触发判断内联在此方法中]
 │               heapRetained = totalMSRegionBytes_ + hotFreeBytes
 │               heapRetained > heapGoal × (1 + 0.1)? → NotifyToScavenge()
 │               → shouldScavengeHeap_ = true + Notify()
 │
 ├─ BackgroundSweepLoop 完成当轮所有 sweep（GC 结束）
 │     HasPendingSweepWork() → false
 │     → nonEmptyBytes = totalMSRegionBytes_ - emptySweptBytes_
 │     → gcStats.heapGoal = nonEmptyBytes × (1 + heapGrowth)   [唯一刷新点，精确值]
 │       （兼作下次 GC 触发阈值 + scavenger 目标）
 │     → space.MaybeNotifyScavenge()
 │
 ├─ BackgroundProcessor::Run()
 │     └─ BackgroundScavengeLoop()  [新增，已集成 Step A+B+C]
 │           retained = GetHeapRetainedBytes()     ← 每次动态计算，不缓存
 │           goal     = GetHeapGoalBytes()         ← 读 gcStats.heapGoal
 │           toScavenge = retained - goal
 │           [Step A 按需] hotFree < toScavenge && HasEmptySweptRegions()?
 │                → space.FlushHalfEmptySweptToFreeManager()  [无系统调用]
 │           quantum  = toScavenge / kRounds × (1 + kOvershoot)
 │           → space.BackgroundScavenge(quantum)   [Step B]
 │                → FreeManager::ScavengeFreeRegionsUpTo(quantum)  [madvise/mmap]
 │           → HasPendingScavengeWork()? → 继续（下轮可能再触发 Step A）
 │           → hasMore=false 时：space.BackgroundShrinkHeap()      [Step C，一次]
 │
 └─ 目标达成：heapRetained ≤ heapGoal
       shouldScavengeHeap_ = false
       打印 scavenge done 日志
```

---

## 11. 数值示例

**场景**：
- `heapGrowth = 1.0`（默认）
- 本轮 GC：存活 60MB，sweep 后非空 region = 200 个 × 32KB = 6.4MB... 

等等，更真实的数字：存活 60MB 的对象，平均每个 region 利用率 62.5%，则非空 region = 60MB / (32KB × 0.625) ≈ 3000 个，`nonEmptyBytes = 3000 × 32KB = 93.75MB`。

```
nonEmptyBytes   = 93.75 MB
heapGoal        = 93.75 × (1 + 1.0) = 187.5 MB
heapRetained    = nonEmptyBytes + hotFreeBytes
                = 93.75 + 150 (假设 sweep 空出了 150MB 的热页) = 243.75 MB
toScavenge      = 243.75 - 187.5 = 56.25 MB

kScavengeRounds = 8
per-iteration quantum = 56.25MB / 8 × 1.05 ≈ 7.4 MB（约 231 个 region）

Run() burst loop 每轮约 2ms，8 轮 ≈ 16ms 完成 56.25MB 的归还
```

对比改造前：一次 `ScavengeFreeRegions` 可能归还 64MB～512MB（单次 < 2ms），RSS 突降，触发 page fault 波。

---

## 12. 风险与注意事项

1. **`heapGoal` 线程安全**：`gcStats.heapGoal` 被 `BackgroundScavengeLoop`（后台线程）读、被 `UpdateGCStats` 和 sweep-done 回调写。写操作都在 GC 线程/后台线程单点，可用 `std::atomic<size_t>` 或 GC 线程独占写（当前 GCStats 字段已无锁保护，需检查是否需要改成 atomic）。

2. **`heapGoal = 0` 时**：第一次 GC 前 `heapGoal = 0`，`ShouldNotifyBackgroundScavenge` 返回 false，不触发 scavenge。符合预期。

3. **GC 频繁时的竞争**：如果 GC 结束重置 `heapGoal` 时，scavenge loop 正在进行，下一次 `GetHeapGoalBytes()` 读到新值即自动修正，不需要额外同步。

4. **`releaseAll=true` 路径保留**：GC 结束的全量 reclaim（`SweepAndRelease` 或进程退出时）仍调用 `ScavengeFreeRegions(true)` + `ShrinkHeap()`，不受分片限制。这条路径通过 `ReclaimGarbageMemory(releaseAll=true)` 维持不变。

5. **`ShrinkHeap` 在所有 slice 完成后调一次**：每次 `BackgroundScavengeLoop` 的所有 quantum 跑完（`hasMore=false`）后，统一调 `BackgroundShrinkHeap()` 一次。`ShrinkHeap` 是从 `committedEnd_` 尾部收缩虚拟地址段的操作，放到循环末尾可减少不必要的重复检查。

---

## 13. 实现顺序建议

1. **Step 1**：在 `GCStats` 中加 `heapGoal`、`lastSurvivingRegionBytes`。无行为变化，仅增加统计字段。`UpdateGCStats` 不做修改。

2. **Step 2**：在 `MSFreeManager` 中加 `GetHotFreeBytes()`、`ScavengeFreeRegionsUpTo()`；委托链向上传到 `MSRegionSpace`。加 `GetHeapInUseBytes()`、`GetHeapRetainedBytes()`、`GetHeapGoalBytes()`、`HasPendingScavengeWork()`、`BackgroundShrinkHeap()`。

3. **Step 3**：在 `BackgroundProcessor` 中加 `BackgroundScavengeLoop()` 和 `shouldScavengeHeap_`；加 `NotifyToScavenge()`；在 `Run()` 中注册；在 `HasPendingWakeWork()` 中加入。同时从 `ReclaimGarbageMemory` 中剥离 Step B+C（`releaseAll=false` 路径）。

4. **Step 4**：在 `MSRegionManager::TakeRegions` 中增加 `ShouldNotifyBackgroundScavenge()` → `MaybeNotifyScavenge()` 调用；在 `BackgroundSweepLoop` sweep-done 时刷新 `heapGoal`（唯一刷新点）并触发 scavenge notify。

5. **验证**：运行 case1_baseline，对比 MSGC 和 CopyGC 的 RSS 峰值、page fault 数（用 `/usr/bin/time -l` 的 `page reclaims` 和 `page faults`）。预期：RSS 峰值相近或更低，page fault 数量下降，wall time 稳定。
