# Step19：大对象热缓存渐进式归还设计

> 本文档记录 case13_pure_large_objects 场景下大对象热缓存（large hot cache）的 RSS 计量盲区修复、scavenger 唤醒缺失修复，以及 BackgroundScavengeLoop 对大对象缓存的渐进式归还支持。

---

## 1. 背景与动机

### 1.1 v2 热缓存的效果与遗留问题

Step18 完成了目标保留量驱动的 scavenger 框架（BackgroundScavengeLoop）。在此之上，针对 case13_pure_large_objects 场景额外实现了大对象热缓存（large hot cache，v2 版本）：

**v2 热缓存设计**：
- `SweepLargeRegions()` 中，死亡大对象 region 不立即调用 `ReclaimRegionLarge` → madvise，而是先尝试放入 `largeHotCache_`（32MB 总量上限，按大小分桶）
- `TryAllocateLargeObjectOnce()` 中，大对象分配优先命中热缓存，跳过 `TakeRegionForLarge` + `InlineReclaimForBalance` + page fault
- GC 下一轮 `SweepLargeRegions()` 开始时 `FlushLargeHotCache()` 强制清空上轮残余

**v2 测试结果（case13，3 runs 均值）**：

| 指标 | CopyGC | MSGC v2 hot cache | delta |
|------|--------|-------------------|-------|
| Total wall time | 398ms | 413ms | **+3.7%** |
| P1 (Small) | 43ms | 31ms | **-27.9%** ✓ |
| P2 (Large alloc) | 6ms | 7ms | +16.7% |
| GC_cnt | 176 | 133 | -24.4% |
| GC_tot | 359ms | 261ms | -27.3% |
| RSS_pk | 73MB | 89MB | **+22% ↑** |

**遗留问题**：RSS_pk 比 CopyGC 高约 16MB，原因是：
1. v2 热缓存是"all-or-nothing"：GC 结束后缓存最多 32MB 死亡 region 驻留在物理内存，直到**下一轮 GC** 才被强制清空，scavenger 从未介入
2. 根因：scavenger 相关的 RSS 计量路径完全不知道 `largeHotCache_` 的存在，始终认为不需要归还

### 1.2 与 emptySweptCache_ 模式的对比

小对象热缓存（`emptySweptCache_`）的已有架构：

```
GC sweep 结束
  └→ 死亡空 region → emptySweptCache_（不立即 madvise，保持页驻留）
        ↓（BackgroundScavengeLoop Step A，并发）
     FlushHalfEmptySweptToHotFree → FreeManager.hotFree
        ↓（BackgroundScavengeLoop Step B）
     BackgroundScavenge → madvise → OS 归还物理页
```

大对象热缓存当前缺少中间的 Step A 阶段，且 scavenger 的所有计量路径都不包含 `largeHotCacheTotalBytes_`。本设计目标是让 `largeHotCache_` 完全对齐上述模式。

---

## 2. 问题根因分析

### 2.1 Gap 1：RSS 计量盲区（Critical）

**受影响路径**（均不包含 large cache bytes）：

| 路径 | 代码位置 | 错误 |
|------|---------|------|
| `GetHeapRetainedBytes()` | `MSRegionSpace.cpp:212` | `total + hotFree`，large cache 已从 `total` sub 出、未进 `hotFree` |
| `MaybeNotifyScavenge()` | `MSRegionManager.cpp:252` | `heapRetained = totalBytes + hotFree` 同上 |
| `MaybeNotifyScavenge()` | 同上 | `releasableSupply = hotFree + emptySwept`，large cache 不在供给里 |
| `BackgroundScavengeLoop` entry | `BackgroundProcessor.cpp:432` | `retained = space.GetHeapRetainedBytes()` 同上 |
| `HasPendingScavengeWork()` | `MSRegionSpace.cpp:280` | 委托 `GetHeapRetainedBytes()`，同上 |

**后果**：以 case13 为例，GC 后 `largeHotCache_` 持有 ~16MB 物理页，但：
- `retained = alive (~5MB) + hotFree (~0) = 5MB`
- `threshold ≈ 30MB`
- `retained(5MB) < threshold(30MB)` → scavenger entry 直接 return false
- 16MB 缓存从未被 gradual drain，在下轮 GC `FlushLargeHotCache` 之前始终占用 RSS

```cpp
// 错误的 retained 计算（当前实现）
size_t RegionSpaceMS::GetHeapRetainedBytes() const {
    size_t total = msCentralPool_->GetTotalMSRegionBytes(); // large cache 被 sub 出去了
    size_t hotFree = msRegionManager_->GetHotFreeBytes();   // large cache 未入 FreeManager
    return total + hotFree;                                  // ← 漏掉 ~16MB
}
```

**修复目标**：所有 `retained` 计算都加上 `pool->GetLargeHotCacheBytes()`。

### 2.2 Gap 2：纯热缓存命中场景 scavenger 从未被通知（Critical）

`MaybeNotifyScavenge()` 的唯一触发路径是 `MSRegionManager::TakeRegions()` 调用（即每次 BUMP/RELEASED 路径分配时）。

对于 case13 纯大对象场景：
- 第一轮 GC 后 `largeHotCache_` 充满
- 后续所有大对象分配走 `TryTakeLargeHotCache` 热路径
- `TakeRegionForLarge` / `TakeRegions` **完全不被调用**
- → `MaybeNotifyScavenge()` 永远不触发
- → 即使修好 Gap 1 里的 `retained` 计算，scavenger 仍不会醒来

**修复目标**：在 `PostMarkTermination()` 调用 `SweepLargeRegions()` 之后，显式唤醒 scavenger。

### 2.3 Gap 3：BackgroundScavengeLoop 缺少 large cache drain（计划中但未实现）

现有 Step A 的 gate：

```cpp
if (space.HasEmptySweptRegions()) {   // ← 只检查 emptySwept
    size_t hotFree = space.GetHotFreeBytes();
    if (hotFree < targetBytes) {
        refilledHotFree = space.FlushHalfEmptySweptToHotFree();
    }
}
```

缺少对 `largeHotCache_` 的并行 drain 分支。即使 Gap 1+2 修好（scavenger 醒来进入循环体），也无法把 large cache 转入 FreeManager → madvise。

此外 Step A 完成后的 guard 和 Step B 的 `hotFreeToKeep` 也没有将 residual large cache 计入"已有 reserve"，导致：
- guard 可能错误地认为总 free 不够而提前 abort
- `hotFreeToKeep` 偏高（emptySwept 满足后本应减去 large cache 的部分），多保留了本可释放的 hotFree

---

## 3. 内存模型修订

修复后 `heapRetained` 的正确定义：

```
heapRetained = totalMSRegionBytes_
             + FreeManager.hotFreeBytes
             + largeHotCacheTotalBytes_      ← 新增项
```

其中：
- `totalMSRegionBytes_` = 活跃小/中对象 + emptySweptCache region（`SweepLargeRegions` 已 sub）
- `hotFreeBytes` = FreeManager 中 free 但物理页未归还的字节
- `largeHotCacheTotalBytes_` = 大对象热缓存持有的物理页字节（**不在 FreeManager，需单独计量**）

三者之和 = runtime 当前实际占用的物理页总量（忽略碎片）。

---

## 4. 完整设计方案

### 4.1 总体流程

```
GC PostMarkTermination
  └→ SweepLargeRegions()
       └→ 死亡大 region → largeHotCache_（≤ kLargeHotCacheMaxTotalBytes = 32MB）
                         → 超限时 ReclaimRegionLarge（直接 madvise）
  └→ MaybeNotifyScavenge()   ← 新增：显式通知 scavenger

BackgroundScavengeLoop（异步，gc-helper 线程）
  entry 条件：retained(含 large cache) > threshold
  Step A1: HasEmptySweptRegions → FlushHalfEmptySweptToHotFree   （原有）
  Step A2: HasLargeFreeCache    → FlushHalfLargeFreeCacheToFreeManager  ← 新增
  snapshot: hotFreeAfterFlush, emptySweptAfterFlush, largeResidualBytes
  guard:    totalFreeAfterFlush = hotFree + emptySwept + largeResidual
  Step B:   hotFreeToKeep = max(0, reserveBytes - emptySwept - largeResidual)
            → BackgroundScavenge(quantum)  （madvise hotFree 超量部分）
  Step C:   BackgroundShrinkHeap
```

### 4.2 新增约束与参数

| 参数 | 值 | 说明 |
|------|---|------|
| `kLargeHotCacheMaxTotalBytes` | 32MB | 总量上限（现有） |
| `kLargeHotCacheMaxRegionCount` | 16 | 最大大小类（16×32KB=512KB，现有） |
| 新增：`largeHotCacheTotalBytes_` 类型 | `std::atomic<size_t>` | 从非原子改为原子，支持 lock-free 读 |

---

## 5. 需要改动的文件与模块

| 文件 | 改动类型 | 说明 |
|------|---------|------|
| `MSCentralRegionPool.h` | 字段类型 + 新增方法 | `atomic`、`GetLargeHotCacheBytes()`、`HasLargeFreeCache()`、`FlushHalfLargeFreeCacheToFreeManager()` |
| `MSCentralRegionPool.cpp` | `TryPutLargeHotCache`、`TryTakeLargeHotCache`、`FlushLargeHotCache`、新增 `FlushHalf...` | 适配 atomic；实现 flush half 逻辑 |
| `MSRegionSpace.h/.cpp` | `GetHeapRetainedBytes()`、`HasPendingScavengeWork()` | 加 large cache bytes |
| `MSRegionManager.cpp` | `MaybeNotifyScavenge()` | `heapRetained`、`releasableSupply` 加 large cache |
| `MarkSweepCollector.cpp` | `PostMarkTermination()` | SweepLargeRegions 后调 MaybeNotifyScavenge |
| `BackgroundProcessor.cpp` | `BackgroundScavengeLoop()` | Step A2、guard 修订、Step B reserve 修订 |

---

## 6. 详细代码变更

### 6.1 MSCentralRegionPool.h — 字段与方法声明

**改动 1**：`largeHotCacheTotalBytes_` 改为 atomic（private 字段区）

```cpp
// 修改前
size_t largeHotCacheTotalBytes_{0};

// 修改后
std::atomic<size_t> largeHotCacheTotalBytes_{0};
```

**改动 2**：新增三个 public 方法（放在 `FlushLargeHotCache` 声明之后）

```cpp
// Lock-free approximate read — safe to call without holding largeHotCacheLock_.
// Returns the total bytes currently held in the large hot cache.
size_t GetLargeHotCacheBytes() const
{
    return largeHotCacheTotalBytes_.load(std::memory_order_relaxed);
}

bool HasLargeFreeCache() const
{
    return largeHotCacheTotalBytes_.load(std::memory_order_relaxed) > 0;
}

// Drain approximately half the large hot cache into FreeManager (released tree).
// Called by BackgroundScavengeLoop Step A2. Returns flushed bytes.
size_t FlushHalfLargeFreeCacheToFreeManager();
```

### 6.2 MSCentralRegionPool.cpp — 适配 atomic + 新方法实现

**改动 1**：`TryPutLargeHotCache` — `fetch_add`

```cpp
bool CentralRegionPool::TryPutLargeHotCache(uintptr_t regionStart, size_t regionBytes)
{
    size_t regionCount = regionBytes / MS_REGION_SIZE;
    if (regionCount == 0 || regionCount > kLargeHotCacheMaxRegionCount) {
        return false;
    }
    std::lock_guard<std::mutex> lg(largeHotCacheLock_);
    // Re-check total cap under lock (atomic was used for the fast read above, but
    // the authoritative check must be done while holding the lock).
    size_t cur = largeHotCacheTotalBytes_.load(std::memory_order_relaxed);
    if (cur + regionBytes > kLargeHotCacheMaxTotalBytes) {
        return false;
    }
    largeHotCache_[regionCount - 1].push_back({regionStart, regionBytes});
    largeHotCacheTotalBytes_.fetch_add(regionBytes, std::memory_order_relaxed);
    return true;
}
```

> 注意：`push_back` 持锁，`fetch_add` 也持锁，不存在数据竞争；`std::memory_order_relaxed` 够用，因为唯一需要跨线程可见的仅是"粗略大小"，精确值只在锁内使用。

**改动 2**：`TryTakeLargeHotCache` — `fetch_sub`

```cpp
bool CentralRegionPool::TryTakeLargeHotCache(size_t regionBytes, uintptr_t& regionStart)
{
    size_t regionCount = regionBytes / MS_REGION_SIZE;
    if (regionCount == 0 || regionCount > kLargeHotCacheMaxRegionCount) {
        return false;
    }
    std::lock_guard<std::mutex> lg(largeHotCacheLock_);
    auto& bucket = largeHotCache_[regionCount - 1];
    if (bucket.empty()) {
        return false;
    }
    LargeHotEntry entry = bucket.back();
    bucket.pop_back();
    regionStart = entry.regionStart;
    largeHotCacheTotalBytes_.fetch_sub(regionBytes, std::memory_order_relaxed);
    return true;
}
```

**改动 3**：`FlushLargeHotCache` — store(0) 改为 fetch_sub / store

```cpp
void CentralRegionPool::FlushLargeHotCache()
{
    std::vector<LargeHotEntry> toReclaim;
    {
        std::lock_guard<std::mutex> lg(largeHotCacheLock_);
        for (size_t i = 0; i < kLargeHotCacheMaxRegionCount; i++) {
            for (auto& entry : largeHotCache_[i]) {
                toReclaim.push_back(entry);
            }
            largeHotCache_[i].clear();
        }
        largeHotCacheTotalBytes_.store(0, std::memory_order_relaxed);
    }
    for (auto& entry : toReclaim) {
        msRegionManager_->ReclaimRegionLarge(entry.regionStart, entry.regionBytes);
    }
}
```

**改动 4**：新增 `FlushHalfLargeFreeCacheToFreeManager`

```cpp
// FlushHalfLargeFreeCacheToFreeManager — BackgroundScavengeLoop Step A2.
// Moves approximately half the large hot cache (by byte count) into FreeManager
// (via ReclaimRegionLarge → releasedTree), making them eligible for madvise in
// Step B. Returns the number of bytes flushed.
size_t CentralRegionPool::FlushHalfLargeFreeCacheToFreeManager()
{
    size_t totalBytes = largeHotCacheTotalBytes_.load(std::memory_order_relaxed);
    if (totalBytes == 0) {
        return 0;
    }
    size_t targetBytes = totalBytes / 2;
    if (targetBytes == 0) {
        targetBytes = totalBytes; // flush the single entry if only one
    }

    std::vector<LargeHotEntry> toReclaim;
    size_t flushed = 0;
    {
        std::lock_guard<std::mutex> lg(largeHotCacheLock_);
        // Walk buckets from largest to smallest: prefer releasing big objects first
        // (they contribute most to RSS and are least likely to be reused soon).
        for (int i = static_cast<int>(kLargeHotCacheMaxRegionCount) - 1;
             i >= 0 && flushed < targetBytes; --i) {
            auto& bucket = largeHotCache_[i];
            while (!bucket.empty() && flushed < targetBytes) {
                LargeHotEntry entry = bucket.back();
                bucket.pop_back();
                toReclaim.push_back(entry);
                flushed += entry.regionBytes;
            }
        }
        largeHotCacheTotalBytes_.fetch_sub(flushed, std::memory_order_relaxed);
    }
    for (auto& entry : toReclaim) {
        msRegionManager_->ReclaimRegionLarge(entry.regionStart, entry.regionBytes);
    }
    return flushed;
}
```

设计决策：
- 从**最大**桶开始 flush：大对象 RSS 占比高，复用可能性低（大对象分配频率低于小大对象）
- 释放 totalBytes/2：保留另一半作为热路径备用，平衡 RSS 和 page fault 率
- 释放后走 `ReclaimRegionLarge` → `freeManager_.ReclaimRegionsReleased`（releasedTree），Stage B 的 `BackgroundScavenge` 再实际 madvise

### 6.3 MSRegionSpace.cpp — GetHeapRetainedBytes + HasPendingScavengeWork

```cpp
size_t RegionSpaceMS::GetHeapRetainedBytes() const
{
    if (msCentralPool_ == nullptr || msRegionManager_ == nullptr) {
        return 0;
    }
    size_t total    = msCentralPool_->GetTotalMSRegionBytes();
    size_t hotFree  = msRegionManager_->GetHotFreeBytes();
    size_t largeCache = msCentralPool_->GetLargeHotCacheBytes();  // 新增
    return total + hotFree + largeCache;
}
```

`HasPendingScavengeWork()` 已委托 `GetHeapRetainedBytes()`，无需单独修改。

### 6.4 MSRegionManager.cpp — MaybeNotifyScavenge

```cpp
void MSRegionManager::MaybeNotifyScavenge()
{
    // ...（GC 检查、threshold 检查不变）

    const CentralRegionPool* pool = ...;
    RegionSpaceMS& space = ...;

    size_t totalBytes = pool->GetTotalMSRegionBytes();
    size_t hotFree    = freeManager_.GetHotFreeBytes();
    size_t largeCache = pool->GetLargeHotCacheBytes();          // 新增
    size_t heapRetained = totalBytes + hotFree + largeCache;    // 修改

    size_t emptySwept = space.GetEmptySweptBytes();
    size_t releasableSupply = hotFree + emptySwept + largeCache; // 修改

    // wakeGuard 检查（用修正后的 releasableSupply）
    {
        size_t reserveBytes = gcStats.markWindowReserveBytes;
        size_t wakeGuard = std::max(reserveBytes,
                                    static_cast<size_t>(DEFAULT_MS_SCAVENGE_WAKE_BYTES));
        if (releasableSupply <= wakeGuard) {
            return;
        }
    }

    // 迟滞阈值检查（用修正后的 heapRetained）
    static constexpr double kHysteresis = 0.1;
    if (heapRetained <= static_cast<size_t>(
            static_cast<double>(threshold) * (1.0 + kHysteresis))) {
        return;
    }

    bp.NotifyToScavenge();
}
```

### 6.5 MarkSweepCollector.cpp — PostMarkTermination 末尾通知 scavenger

在 `SweepLargeRegions()` 调用之后，显式调用一次 `MaybeNotifyScavenge()`：

```cpp
void MarkSweepCollector::PostMarkTermination()
{
    // ...（现有代码不变）

    // Tier 1: in-heap large regions — eager sweep unmarked ones
    CentralRegionPool* centralPool = space.GetCentralPool();
    if (centralPool != nullptr) {
        MSLargeSweepStats stats = centralPool->SweepLargeRegions();
        cycleStats_.losFreedCount      += stats.objectCount;
        cycleStats_.losFreedRegionCount += stats.regionCount;
        cycleStats_.losFreedBytes      += stats.freedBytes;

        // Notify scavenger explicitly: dead large regions may have gone into
        // largeHotCache_, which is invisible to MaybeNotifyScavenge triggered
        // by TakeRegions (large hot cache hits bypass TakeRegions entirely).
        if (centralPool->HasLargeFreeCache()) {
            space.MaybeNotifyScavenge();
        }
    }
}
```

原理：`MaybeNotifyScavenge()` 内部有 `heapRetained <= threshold * 1.1` 的迟滞保护，不会在 RSS 没有超标时误触发。加上 `HasLargeFreeCache()` 前置 guard，完全空缓存时直接短路，不进入 `MaybeNotifyScavenge` 内部检查。

### 6.6 BackgroundProcessor.cpp — BackgroundScavengeLoop 修改

完整修改后的函数逻辑（仅展示变更部分）：

**改动点 1**：获取 pool 指针（在函数开头补一行）

```cpp
bool BackgroundProcessor::BackgroundScavengeLoop()
{
    RegionSpaceMS& space = static_cast<RegionSpaceMS&>(Heap::GetHeap().GetAllocator());
    CentralRegionPool* pool = space.GetCentralPool();            // 新增

    size_t retained = space.GetHeapRetainedBytes();              // 现在含 large cache
    size_t threshold = space.GetHeapThresholdBytes();
    // ...entry check 不变（GetHeapRetainedBytes 已修正）...
```

**改动点 2**：Step A 增加 large cache drain 分支（紧跟 emptySwept flush 之后）

```cpp
    // Step A1（原有）: emptySwept → hotFree
    if (space.HasEmptySweptRegions()) {
        size_t hotFree = space.GetHotFreeBytes();
        if (hotFree < targetBytes) {
            refilledHotFree = space.FlushHalfEmptySweptToHotFree();
        }
    }

    // Step A2（新增）: large hot cache → FreeManager (releasedTree)
    // Triggered independently of emptySwept: in pure-large-object workloads,
    // emptySwept may be empty while largeHotCache holds 10–32 MB.
    if (pool != nullptr && pool->HasLargeFreeCache()) {
        size_t hotFree = space.GetHotFreeBytes();
        if (hotFree < targetBytes) {
            refilledHotFree += pool->FlushHalfLargeFreeCacheToFreeManager();
        }
    }
```

**改动点 3**：snapshot 和 guard 加 residual large cache

```cpp
    size_t hotFreeAfterFlush    = space.GetHotFreeBytes();
    size_t emptySweptAfterFlush = space.GetEmptySweptBytes();
    // Residual large cache after Step A2 half-flush (the other half still held).
    // Counts toward reserve so hotFree is not over-released.
    size_t largeResidualBytes   = pool ? pool->GetLargeHotCacheBytes() : 0;  // 新增

    {
        const GCStats& gcStats = Heap::GetHeap().GetCollector().GetGCStats();
        size_t reserveBytes  = gcStats.markWindowReserveBytes;
        size_t scavengeGuard = std::max(reserveBytes,
                                        static_cast<size_t>(DEFAULT_MS_SCAVENGE_MIN_SLICE_BYTES));
        // Include residual large cache in total free: it is committed memory that
        // acts as a reserve buffer (will be flushed by future Step A2 slices).
        size_t totalFreeAfterFlush = hotFreeAfterFlush + emptySweptAfterFlush
                                   + largeResidualBytes;    // 修改
        if (totalFreeAfterFlush <= scavengeGuard) {
            ...
            return false;
        }
    }
```

**改动点 4**：Step B `hotFreeToKeep` 使用 combined reserve

```cpp
    {
        const GCStats& gcStats2 = Heap::GetHeap().GetCollector().GetGCStats();
        size_t reserveBytes2 = gcStats2.markWindowReserveBytes;
        // emptySwept + residual large cache both satisfy the reserve; only hold
        // back hotFree for the shortfall beyond their combined capacity.
        size_t combinedReserve = emptySweptAfterFlush + largeResidualBytes;  // 修改
        size_t hotFreeToKeep   = (reserveBytes2 > combinedReserve)
            ? (reserveBytes2 - combinedReserve)
            : 0;
        size_t availableToRelease = (hotFreeAfterFlush > hotFreeToKeep)
            ? (hotFreeAfterFlush - hotFreeToKeep)
            : 0;
        if (availableToRelease == 0) {
            shouldScavengeHeap_.store(false, std::memory_order_release);
            return false;
        }
        size_t quantum = std::min(roundTarget, availableToRelease);
        released = space.BackgroundScavenge(quantum);
    }
```

---

## 7. 关键权衡与边界情况

### 7.1 FlushHalf 策略的 RSS vs page fault 权衡

| 策略 | RSS 效果 | 下轮分配 |
|------|---------|---------|
| 全量 flush（现有 FlushLargeHotCache） | RSS 最低 | 全部 page fault |
| 半量 flush（本设计） | RSS 降约 50% | 保留一半热缓存，减少 page fault |
| 不 flush（仅靠下轮 GC） | RSS 最高（+32MB） | 全部命中缓存 |

半量 flush 是折中点，与 `emptySweptCache_` 的 `FlushHalfEmptySweptToHotFree` 保持一致。

### 7.2 flush 顺序：从最大桶开始的合理性

- 小大对象（2×32KB = 64KB）分配频率高，短期内可能再次被分配，保留价值高
- 大大对象（16×32KB = 512KB）分配频率低，占用 RSS 多，应优先释放
- 从大到小遍历桶，优先释放高 RSS 占比、低再利用概率的对象

### 7.3 `atomic<size_t>` 的 relaxed 语义足够

`largeHotCacheTotalBytes_` 的原子操作全部用 `memory_order_relaxed`，原因：
- 该值的精确性不影响任何安全性（只是近似 hint，不作为锁/同步点）
- 精确值只在 `largeHotCacheLock_` 保护的区域内读写，原子性只用于 lock-free 近似读（`MaybeNotifyScavenge`、`GetHeapRetainedBytes`）
- 只要最终可见（relaxed 在 x86/arm 上均满足），就可以接受短暂的过时读

### 7.4 与 FlushLargeHotCache（GC 开始时）的关系

两者不冲突：
- `FlushLargeHotCache()`：GC cycle 开始时的**安全网**，强制清空上一轮残余，确保 GC 看到的大对象列表是干净的
- `FlushHalfLargeFreeCacheToFreeManager()`：GC 结束后的**渐进释放**，在下一轮 GC 到来之前逐步降低 RSS

如果 scavenger 在下一轮 GC 前已经把 large cache 全部 drain 完（缓存为空），`FlushLargeHotCache()` 就是 no-op，完全正常。

### 7.5 `PostMarkTermination` 中调 `MaybeNotifyScavenge` 的时机安全性

- `PostMarkTermination` 在 MarkTermination STW 结束、mutator 已恢复后执行
- 此时 `Heap::IsGcStarted()` = false（GC 已完成标记阶段）
- `MaybeNotifyScavenge` 内部有 `IsGcStarted()` 短路保护
- `NotifyToScavenge` 是非阻塞（仅设置 flag + condvar notify），不影响主 GC 流程

---

## 8. 验证计划

### 8.1 构建与冒烟

```bash
cd /Users/lipeisheng/code/cangjie/cangjie/cangjie_runtime_MSGC/runtime
python3 build.py build --gc-mode marksweep

# 替换 + 重签名
cp output/temp/lib/aarch64_Release/libcangjie-runtime.dylib \
   /Users/lipeisheng/code/cangjie/cangjie-pkg/self/dev260509/cangjie/runtime/lib/darwin_aarch64_cjnative/libcangjie-runtime.dylib
codesign -f -s - /Users/lipeisheng/code/cangjie/cangjie-pkg/self/dev260509/cangjie/runtime/lib/darwin_aarch64_cjnative/libcangjie-runtime.dylib

# 冒烟（MS-GC hello test）
source /Users/lipeisheng/code/cangjie/cangjie-pkg/self/dev260509/cangjie/envsetup.sh
cd gc && cjc gc_hello.cj -o main_hello && ./main_hello
```

### 8.2 benchmark 对比

```bash
cd /Users/lipeisheng/code/cangjie/cangjie/cangjie_runtime_MSGC/runtime/MSGC_RESULT/Result

# case13: 大对象场景（主要验证目标）
env -i HOME=$HOME TERM=$TERM PATH=/usr/bin:/bin:/usr/sbin:/sbin:/opt/homebrew/bin \
  python3 script/compare.py --case case13_pure_large_objects --runs 3

# case14: ArrayList 小对象（验证无退化）
env -i HOME=$HOME TERM=$TERM PATH=/usr/bin:/bin:/usr/sbin:/sbin:/opt/homebrew/bin \
  python3 script/compare.py --case case14_arraylist_small_objs --runs 3

# case1: 基线综合（验证无退化）
env -i HOME=$HOME TERM=$TERM PATH=/usr/bin:/bin:/usr/sbin:/sbin:/opt/homebrew/bin \
  python3 script/compare.py --case case1_baseline --runs 3 --skip-build
```

### 8.3 预期结果

| 指标 | v2（当前） | v3（本设计）预期 | 说明 |
|------|-----------|----------------|------|
| case13 Total | +3.7% vs CopyGC | 略微变化（±2%） | wall time 基本不变 |
| case13 RSS_pk | 89MB | **75–80MB** | large cache gradual drain 效果 |
| case13 P1 | -27.9% | 保持 -20% 以上 | 热缓存仍有一半保留 |
| case1 Total | 无退化 | ±2% | 小对象路径未动 |

RSS_pk 预期从 89MB 降至约 75–80MB，接近 CopyGC 的 73MB（差值来自大对象缓存保留的另一半 + emptySwept 缓冲）。

### 8.4 关键日志验证

在 `FlushHalfLargeFreeCacheToFreeManager` 和 `MaybeNotifyScavenge` 中添加 `VLOG(REPORT, ...)` 打印，确认：

1. `SweepLargeRegions` 后 `HasLargeFreeCache() = true`
2. `MaybeNotifyScavenge` 被触发（打印 `[MS-GC] notifyScavenge: large cache=%zuB`）
3. `BackgroundScavengeLoop` Step A2 被执行（打印 `[MS-GC] scavenge step-A2 flush largeCache: flushed=%zuB`）
4. 第二轮 GC 的 `FlushLargeHotCache` 是 no-op（缓存已被 scavenger drain）

---

## 9. 与后续步骤的关系

| 后续方向 | 依赖本 Step 的内容 |
|---------|------------------|
| case13 RSS 进一步优化 | Gap 1+2 修复后，RSS 计量准确，可精细调 `kLargeHotCacheMaxTotalBytes` 阈值 |
| 多大小类 flush 策略调优 | `FlushHalfLargeFreeCacheToFreeManager` 的桶遍历顺序可按运行时分布动态调整 |
| GC 触发阈值与 large cache 联动 | `GetHeapRetainedBytes` 修正后，`heapGoal` 计算也包含了 large cache，GC 触发更准确 |
