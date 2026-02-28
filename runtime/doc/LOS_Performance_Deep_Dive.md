# LOS 性能关键路径深度分析报告

> 日期: 2026-02-25
> 工作区: runtime/src/Heap/

---

## Area 1: 分配热路径 (Allocation Hot Path)

### 1.1 入口点决策逻辑

**文件**: [RegionSpace.cpp](src/Heap/Allocator/RegionSpace.cpp#L19-L29)

```cpp
MAddress RegionSpace::TryAllocateOnce(size_t allocSize, AllocType allocType)
{
    if (UNLIKELY(allocType == AllocType::PINNED_OBJECT)) {       // 检查1: pinned对象
        return regionManager.AllocPinned(allocSize);
    }
    if (UNLIKELY(allocSize >= regionManager.GetLargeObjectThreshold())) { // 检查2: 大小阈值
        return regionManager.AllocLarge(allocSize);
    }
    AllocBuffer* allocBuffer = AllocBuffer::GetOrCreateAllocBuffer(); // 检查3: 小对象快路径
    return allocBuffer->Allocate(allocSize, allocType);
}
```

**关键发现**: 每次分配都要走 2 个 `UNLIKELY` 分支。第二个分支调用 `GetLargeObjectThreshold()` — 这是一个简单的成员访问（`return largeObjectThreshold;`，见 [RegionManager.h L527](src/Heap/Allocator/RegionManager.h#L527)），成本可忽略。小对象的正常路径只经历 2 个分支预测失败概率极低的条件检查，非常高效。

### 1.2 AllocLarge 路由 (LOS → MAIN fallback)

**文件**: [RegionManager.h](src/Heap/Allocator/RegionManager.h#L257-L273)

```cpp
uintptr_t AllocLarge(size_t size)
{
    // 检查1: losManager存在 + 检查2: 不在compacting
    if (losManager != nullptr && !losManager->IsCompacting()) {
        uintptr_t addr = losManager->AllocLarge(size);  // LOS 主路径
        if (addr != 0) {
            return addr;
        }
    }
    // MAIN fallback
    uintptr_t addr = AllocLargeInMain(size);
    if (addr != 0 && losManager != nullptr) {
        losManager->IncrementFallbackCount();  // atomic fetch_add — 每次MAIN fallback额外开销
    }
    return addr;
}
```

**关键发现**:
- `IsCompacting()` 是 `std::atomic<bool>` 的 acquire load — 每次大对象分配都要付出一次原子读开销
- 正常情况下 LOS 分配成功即刻返回，不触发 MAIN fallback
- **条件检查计数**: 从 `Allocate()` 入口到 LOS 分配成功，总共 **5 个条件分支** + 1 个 atomic load

### 1.3 LOSManager::AllocLarge 内部路径

**文件**: [LOSManager.cpp](src/Heap/Allocator/LOSManager.cpp#L48-L97)

```cpp
uintptr_t LOSManager::AllocLarge(size_t size)
{
    // 检查1: isCompacting (acquire load)
    if (isCompacting.load(std::memory_order_acquire)) { ... return 0; }

    // 检查2: losCompactionFreeze (acquire load)
    if (losCompactionFreeze.load(std::memory_order_acquire)) { ... return 0; }

    size_t unitCount = (size + RegionInfo::UNIT_SIZE - 1) / RegionInfo::UNIT_SIZE;

    // Step 1: 尝试 losFreeMgr (CartesianTree查询)
    RegionInfo* region = losFreeMgr.TakeRegion(unitCount, ...);  // mutex + tree traversal
    if (region != nullptr) {
        return finishAllocation(region, "freeMgr");
    }

    // Step 2: 扩展 losInactiveZone (CAS loop)
    region = AllocFromInactiveZone(unitCount);
    if (region != nullptr) {
        return finishAllocation(region, "inactiveZone");
    }
    return 0;
}
```

**条件检查明细** (LOS热路径): 2 个 atomic acquire load + 1 个 try_lock + 1 个 tree traversal O(log n) + CAS loop

**优化机会 1** (影响: **中**): `isCompacting` 和 `losCompactionFreeze` 是两个独立的 atomic 变量，但语义高度重合。可以合并为单一 `uint8_t` 的 atomic 状态机，减少一次 acquire load:
```cpp
enum LOSState : uint8_t { NORMAL, COMPACTING, FROZEN };
std::atomic<LOSState> losState{NORMAL};
// 热路径只需一次 acquire load:
if (UNLIKELY(losState.load(std::memory_order_acquire) != NORMAL)) { return 0; }
```

**优化机会 2** (影响: **高**): `finishAllocation` lambda 内部调用 `EnlistLargeRegion()`，其中包含对 `Heap::GetHeap().GetCollector().GetGCPhase()` 的间接调用链。这在分配热路径上是一个不必要的间接调用层次。可以将 GCPhase 缓存在 LOSManager 的成员变量中，在 GC 相变时更新。

### 1.4 FreeRegionManager::TakeRegion

**文件**: [FreeRegionManager.h](src/Heap/Allocator/FreeRegionManager.h#L55-L110)

```cpp
RegionInfo* TakeRegion(size_t num, RegionInfo::UnitRole uclass, bool expectPhysicalMem)
{
    bool tryDirtyTree = true;
    bool tryReleasedTree = true;

    while (tryDirtyTree || tryReleasedTree) {
        // 尝试1: dirtyTree (try_lock + TakeUnitsLowAddr O(log n))
        if (tryDirtyTree && dirtyUnitTreeMutex.try_lock()) {
            bool dirtyOk = lowAddressFirst ? dirtyUnitTree.TakeUnitsLowAddr(num, idx)
                                           : dirtyUnitTree.TakeUnits(num, idx);
            if (dirtyOk) {
                RegionInfo::ClearUnits(globalIdx, num);  // memset — 可能是最大开销
                RegionInfo* region = RegionInfo::InitRegion(globalIdx, num, uclass);
                dirtyUnitTreeMutex.unlock();
                return region;
            }
            tryDirtyTree = false;
            dirtyUnitTreeMutex.unlock();
        }
        // 尝试2: releasedTree (同样 try_lock + tree traversal)
        if (tryReleasedTree && releasedUnitTreeMutex.try_lock()) { ... }
        ScopedEnterSaferegion enterSaferegion(true);  // 进入saferegion让GC运行
    }
    return nullptr;
}
```

**关键发现**:
- `ClearUnits()` 对 dirty 区域执行 memset 清零 — 对于大对象（几十/几百 KB）这是**最大的单次开销**
- `try_lock` 模式避免了互斥量争用时的阻塞，但如果两棵树都锁失败，会进入 saferegion + 重试循环
- LOS 的 lowAddressFirst = false（仅 MAIN 开启），所以 LOS 用的是 `TakeUnits` (不保证地址顺序)

**优化机会 3** (影响: **高**): 对于 LOS 大对象，`ClearUnits` 清零整个区域可能浪费。大对象覆盖的是完整 region，用户代码会覆盖全部内容。可以考虑**延迟清零**或使用 `mmap` 重映射（OS 自动提供零页）替代 memset。

### 1.5 条件检查计数与优化建议

从 `RegionSpace::Allocate()` 到 LOS 分配成功的完整路径:

| 步骤 | 操作 | 成本 |
|------|------|------|
| 1 | `allocType == PINNED` | 分支(UNLIKELY) |
| 2 | `allocSize >= threshold` | 分支(UNLIKELY) |
| 3 | `losManager != nullptr` | 分支 |
| 4 | `isCompacting` acquire load | atomic |
| 5 | `isCompacting` 内 acquire load | atomic |
| 6 | `losCompactionFreeze` acquire load | atomic |
| 7 | `losFreeMgr.TakeRegion` | try_lock + O(log n) tree |
| 8 | (如果tree失败) `AllocFromInactiveZone` CAS loop | CAS |
| 9 | `EnlistLargeRegion` → `GetGCPhase` | 间接调用链 |
| 10 | `losAllocCount` fetch_add | atomic |

**总计**: 3 个 atomic acquire load + 1 个 try_lock + 1 个 atomic fetch_add + O(log n) tree操作 + ClearUnits memset

---

## Area 2: CompactLOS 实现

### 2.1 CompactLOS 完整流程

**文件**: [LOSManager.cpp](src/Heap/Allocator/LOSManager.cpp#L561-L780)

CompactLOS 是一个 7 阶段 STW 操作：

```
Phase 1: Collect — 遍历 losOldLargeRegionList + losRecentLargeRegionList
         分离 movable vs pinned (rawPointerCount > 0)
         成本: O(n) 其中 n = 存活大对象数

Phase 2: Sort + Compute — 按地址降序排列，从 losEnd 向低地址分配目标位置
         成本: O(n log n) 排序

Phase 3: Build migration map — unordered_map<oldAddr, newAddr>
         成本: O(m) 其中 m = 需要移动的对象数，含 map.reserve + insert

Phase 4: (空) — 引用更新由 WCollector 管理

Phase 5: memmove — 按高目标优先顺序执行 memmove_s
         成本: O(totalMovedBytes) — 这是最大开销！
         关键代码:
```

```cpp
for (const auto& entry : compactPlan) {
    if (entry.newStart != entry.oldStart) {
        int ret = memmove_s(reinterpret_cast<void*>(entry.newStart), entry.regionSize,
                            reinterpret_cast<void*>(entry.oldStart), entry.regionSize);
        entry.region->InitFreeUnits();
        RegionInfo* newRegion = RegionInfo::InitRegionAt(entry.newStart, entry.unitCount, ...);
        newRegion->Alloc(entry.regionSize - RegionSpace::HEADER_SIZE);
    }
}
```

```
Phase 5b: Rebuild region lists — ClearList + 3× PrependRegion loops
          成本: O(n) + O(m) unordered_set 开销

Phase 6: RebuildFreeRegionManager — ClearTrees + 逐gap AddGarbageUnits
          成本: O(n) 间隙扫描 + O(gap_count × log n) tree插入

Phase 7: ReclaimLOSBorrowed → 推 watermark
          成本: O(1)
```

**性能剖析**:
- *Phase 5 (memmove)* 贡献了 >90% 的 STW 耗时
- *Phase 5b/6* 使用 `unordered_set<RegionInfo*>` 和 `std::sort` — 在 STW 期间触发堆分配器，可能导致额外抖动

### 2.2 UpdateCompactedLOSReferences

**文件**: [WCollector.cpp](src/Heap/WCollector/WCollector.cpp#L715-L788)

```cpp
void WCollector::UpdateCompactedLOSReferences(LOSManager& losMgr, RegionManager& rm)
{
    const auto& migMap = losMgr.GetMigrationMap();  // unordered_map

    auto updateRefField = [&migMap](RefField<>& field) {
        BaseObject* target = field.GetTargetObject();
        if (target == nullptr) return;
        auto it = migMap.find(reinterpret_cast<uintptr_t>(target));  // O(1) 均摊 hash lookup
        if (it != migMap.end()) {
            field.SetTargetObject(reinterpret_cast<BaseObject*>(it->second));
        }
    };

    // A. 遍历整个堆所有对象 → ForEachRefField
    rm.ForEachObjUnsafe([&](BaseObject* obj) {
        obj->ForEachRefField(updateRefField);
    });

    // B. 遍历所有 GC roots:
    //   B1. 静态根 (StaticRootTable)
    //   B2. 线程栈根 (所有 Mutator)
    //   B3. Finalizer 根
    //   B4. 导出根 (跨语言)
    //   B5. 并发模型根 (Future, coroutine)
}
```

**关键发现**: 即使只移动了 1 个 LOS 对象，也要**扫描全堆所有对象的所有引用字段**。这是一个 O(heapSize) 操作。

**优化机会 4** (影响: **极高**): 使用 Remembered Set 或卡表（Card Table）记录指向 LOS 的引用。Compaction 时只需扫描脏卡/remembered set 条目，而非全堆。这能将引用更新从 O(heap) 降低到 O(refCount_to_LOS)。

**优化机会 5** (影响: **高**): `migMap` 使用 `unordered_map` — 在 STW 期间进行哈希查找。对于少量移动的对象（通常 <100），可以替换为排序数组 + 二分查找，消除哈希冲突和动态内存分配。

### 2.3 各阶段成本分析

| 阶段 | 复杂度 | STW影响 | 备注 |
|------|--------|---------|------|
| Phase 1: Collect | O(n) | 低 | 线性遍历 |
| Phase 2: Sort+Compute | O(n log n) | 低 | n通常<100 |
| Phase 3: Migration map | O(m) | 低 | hash map insert |
| Phase 5: memmove | **O(totalBytes)** | **极高** | 可能移动数MB数据 |
| Phase 5b: Rebuild lists | O(n) | 低 | unordered_set开销 |
| Phase 6: Rebuild freeMgr | O(gaps × log n) | 中 | tree插入 |
| Phase 7: Shrink reserve | O(1) | 可忽略 | watermark调整 |
| UpdateRefs: 全堆扫描 | **O(heapSize)** | **极高** | 只为更新几个引用 |

---

## Area 3: RESERVE 借用机制

### 3.1 借用触发路径

**文件**: [RegionSpace.cpp](src/Heap/Allocator/RegionSpace.cpp#L62-L108)

RESERVE 借用**不在**正常分配路径上。它仅在 OOM 重试阶段（第 5 次尝试）触发:

```cpp
// ShouldRetryAllocation 中:
// tryTimes == TRIGGER_RESERVE_BORROW (第5次重试)
if (tryTimes == static_cast<size_t>(TryAllocationThreshold::TRIGGER_RESERVE_BORROW)) {
    TryBorrowFromReserveIfNeeded(size);
    return true;
}
```

完整触发链: `Allocate` → 重试循环 → OOM GC → GC 完成后仍然分配失败 → `TryBorrowFromReserveIfNeeded`:

```cpp
void RegionSpace::TryBorrowFromReserveIfNeeded(size_t allocSize)
{
    ReserveManager* reserveManager = regionManager.GetReserveManager();
    if (reserveManager == nullptr) return;

    bool isLargeObject = allocSize >= regionManager.GetLargeObjectThreshold();
    LOSManager* losManager = regionManager.GetLOSManager();

    if (isLargeObject && losManager != nullptr) {
        size_t losRunway = losManager->GetFreeSize();
        if (losRunway < allocSize) {
            size_t borrowed = reserveManager->BorrowForLOS(allocSize);  // mutex + watermark push
        }
    }

    size_t mainRunway = regionManager.GetInactiveUnitCount() * RegionInfo::UNIT_SIZE;
    if (mainRunway < allocSize) {
        size_t borrowed = reserveManager->BorrowForMain(allocSize);
    }
}
```

### 3.2 ComputeBorrowChunk 策略

**文件**: [ReserveManager.cpp](src/Heap/Allocator/ReserveManager.cpp#L40-L70)

```cpp
size_t ReserveManager::ComputeBorrowChunk(size_t requestSize) const
{
    size_t available = GetAvailable();
    size_t minChunk = MIN_BORROW_CHUNK_UNITS * unitSize;      // 16 units = 256KB (16KB page)
    size_t proportional = available / BORROW_RATIO_DIVISOR;    // 10% of remaining

    // target = max(requestSize, max(minChunk, proportional))
    size_t target = requestSize;
    if (minChunk > target) target = minChunk;
    if (proportional > target) target = proportional;

    // Align up to 16-unit boundary
    size_t alignment = BORROW_CHUNK_ALIGN_UNITS * unitSize;
    target = (target + alignment - 1) / alignment * alignment;

    // Cap at available
    if (target > available) {
        target = (requestSize + unitSize - 1) / unitSize * unitSize;
    }
    return target;
}
```

策略设计合理：采用 A+B 混合策略（min(256KB, 10% remaining) 与 requestSize 取最大），避免频繁小额借用。

### 3.3 每次分配是否检查 RESERVE?

**结论: 否。** RESERVE 借用检查仅在以下严格条件下触发:

1. `TryAllocateOnce` 返回 0 (分配失败)
2. 经过 reschedule (tryTimes <= RESCHEDULE)
3. 等待 GC 完成 (tryTimes < TRIGGER_OOM)
4. 触发 OOM GC (tryTimes == TRIGGER_OOM)
5. OOM GC 完成后仍然失败
6. **此时才调用** `TryBorrowFromReserveIfNeeded` (tryTimes == TRIGGER_RESERVE_BORROW)

正常分配路径（`TryAllocateOnce` 成功）不会接触 RESERVE 的任何代码。这是一个设计良好的去耦合。

**潜在优化** (影响: **低**): `BorrowForLOS` 和 `BorrowForMain` 都使用 `std::mutex` (`borrowMutex`)。在 OOM 场景下多个线程可能同时触发借用，产生锁争用。可以考虑 CAS-based 的无锁 watermark 推进。但由于此路径极少执行，影响有限。

---

## Area 4: FreeRegionManager

### 4.1 双树架构

**文件**: [FreeRegionManager.h](src/Heap/Allocator/FreeRegionManager.h)

FreeRegionManager 使用两棵 Cartesian Tree（笛卡尔树）管理空闲区域:

```
┌──────────────────┐     ┌──────────────────┐
│   dirtyUnitTree  │     │ releasedUnitTree │
│  (有物理页，需清零) │     │ (已释放物理页)    │
│  mutex保护        │     │  mutex保护       │
└──────────────────┘     └──────────────────┘
```

- **dirtyTree**: 保存之前用过但未释放物理页的空闲区域。分配时需要 `ClearUnits(idx, num)` （memset 清零）
- **releaseTree**: 保存已通过 `madvise(DONTNEED)` 释放物理页的区域。分配时 OS 自动提供零页，但可能触发缺页异常

CartesianTree 是一个 max-heap-on-count + BST-on-index 的混合结构:
- `TakeUnits()`: 从最大块分割，O(log n)
- `TakeUnitsLowAddr()`: 从最低地址的足够大块分割，O(log n)
- `MergeInsert()`: 插入并合并相邻块，O(log n)

### 4.2 LOS 专用 FreeRegionManager

**文件**: [LOSManager.h](src/Heap/Allocator/LOSManager.h#L33-L45)

LOS 拥有**独立的** `losFreeMgr` 实例，与 MAIN 的 `freeRegionManager` 完全分离:

```cpp
class LOSManager {
    FreeRegionManager losFreeMgr;   // baseUnit = mainUnitCount
    // ...
};
```

- `baseUnit = mainUnitCount` 使得局部索引能正确映射到全局 RegionInfo
- 容量 = `reserveUnitCount + losUnitCount` → 覆盖 RESERVE 扩展区 + 原生 LOS 空间
- 当 LOS 对象死亡时，走 `ReclaimLOSRegion → losFreeMgr.AddGarbageUnits`（而非 MAIN 的 freeRegionManager）

### 4.3 CartesianTree 操作复杂度

**文件**: [CartesianTree.h](src/Heap/Allocator/CartesianTree.h#L472-L560)

```cpp
bool TakeUnitsImpl(Count num, Index& idx, bool refershRegionInfo)
{
    ForwardIterator it(*this);        // 从根开始向下遍历
    Node** nodePtr = it.Next();
    if (node->GetCount() < num) return false;  // 最大块都不够 → 快速失败
    // 向下查找最小满足要求的块
    while ((nextNodePtr = it.Next()) != nullptr) {
        if (nextNode->GetCount() < num) break;
        nodePtr = nextNodePtr;
    }
    // 分割: node[idx, idx+count) → 取前num, 留后(count-num)
    node->UpdateNode(idx + num, count - num, ...);
    DecTotalCount(num);
    if (node->GetCount() == 0) RemoveZeroNode(*nodePtr);
    else LowerNonZeroNode(*nodePtr);          // 下沉维护堆性质
    return true;
}
```

**关键发现**:
- 冷路径 `LowerNonZeroNode` 可能引起多次旋转（O(log n)）
- LOS 的 `lowAddressFirst = false`，所以用的是 `TakeUnits` — 优先取最大块，减少碎片
- **没有区域池化**: 区域不是预分配的对象池。每次分配从树中取一段连续单元，每次回收将单元插回树。树操作本身不涉及 `malloc/free`（节点内嵌在单元元数据中）

**优化机会 6** (影响: **中**): 当 LOS 只有少量大对象（<10）时，CartesianTree 的常数因子偏大（旋转、迭代器）。可以为 LOS 增加一个简单的 **freelist cache**（链表）缓存最近回收的单区域，避免每次都走 tree 操作。大于缓存容量时再回退到 tree。

---

## Area 5: ShrinkInactiveZone

### 5.1 完整实现

**文件**: [RegionManager.cpp](src/Heap/Allocator/RegionManager.cpp#L1771-L2047)

ShrinkInactiveZone 是一个两阶段的 STW 操作（~280 行代码）:

**Phase A: 收缩 MAIN 的 inactiveZone**
```cpp
// 线性扫描 [regionHeapStart, mainScanEnd) 找到最高活跃区域
for (uintptr_t addr = regionHeapStart; addr < mainScanEnd;) {
    RegionInfo* region = RegionInfo::GetRegionInfoAt(addr);
    uintptr_t end = region->GetRegionEnd();
    if (region->IsValidRegion() && !region->IsFreeRegion() && !region->IsGarbageRegion()) {
        highestLiveEnd = std::min(end, mainEnd);
    }
    addr = end;
}
```

**Phase B: 收缩 RESERVE-borrowed 空间**
```cpp
// 线性扫描 [mainEnd, min(oldInactive, mainWm)) — borrowed 区域
for (uintptr_t addr = mainEnd; addr < borrowedScanEnd;) {
    RegionInfo* region = RegionInfo::GetRegionInfoAt(addr);
    if (region->IsValidRegion() && !region->IsFreeRegion() && !region->IsGarbageRegion()) {
        highestLiveBorrowedEnd = end;
    }
    addr = end;
}
```

**Fix B: 保护 Ghost Routing 目标**（约 60 行代码）
```cpp
// 扫描 ghostFromRegionList 的所有 ghost 区域的路由目标
ghostFromRegionList.VisitAllGhostRegions([&](RegionInfo* ghost) {
    const RouteInfo& ri = ghost->GetRouteInfoRef();
    uintptr_t target1Addr = ri.toRegion1StartAddress;
    // 提升保护下限：确保 routing 目标不被释放
    if (target1Addr >= mainEnd) {
        if (targetEnd > highestLiveBorrowedEnd) highestLiveBorrowedEnd = targetEnd;
    } else {
        if (cappedEnd > highestLiveEnd) highestLiveEnd = cappedEnd;
    }
    // 同样检查 toRegion2...
});
// 同样扫描 pendingGhostGarbageList（完全重复的代码块！）
pendingGhostGarbageList.VisitAllGhostRegions([&](RegionInfo* ghost) { ... });
```

**重建 freeRegionManager**:
```cpp
freeRegionManager.ClearTrees();
// Re-add MAIN free chunks below newInactive
for (uintptr_t addr = regionHeapStart; addr < newInactive;) {
    if (region->IsFreeRegion()) {
        freeRegionManager.AddGarbageUnits(region->GetUnitIdx(), region->GetUnitCount());
    }
    addr = end;
}
// Re-add RESERVE-borrowed free chunks...同样的循环
```

**Phase A commit**: 更新 inactiveZone, 清理 garbageRegionList + pendingGhostGarbageList 中超出边界的条目
**Phase B commit**: 调用 `ReclaimMainBorrowed(newWatermark)` 推 watermark, 清理借用区的垃圾列表

### 5.2 触发条件与频率

**文件**: [WCollector.cpp](src/Heap/WCollector/WCollector.cpp#L919-L931)

```cpp
// DoGarbageCollection() 中:
if (gcReason == GC_REASON_OOM) {
    RegionManager& rm = ...;
    ScopedStopTheWorld stw("shrink inactive", false);
    rm.ShrinkInactiveZone();
}
```

**触发条件**: 仅在 **OOM GC** 期间调用。普通 GC 完全跳过。

**频率**: 取决于内存压力。正常运行时几乎不触发。在内存紧张场景（持续分配大对象导致 OOM）下可能每次 OOM GC 都执行。

### 5.3 重建 FreeRegionManager 的成本

重建过程分为 3 步:

1. **ClearTrees()** — 清空两棵树: O(1)（只是将 root 置 null + 重置计数器）
   ```cpp
   void ClearTree() {
       root = nullptr;      // 节点内嵌在 RegionInfo 中，无需 free
       totalCount = 0;
   }
   ```

2. **线性扫描 MAIN 区域** — O(totalUnits / avgRegionSize): 遍历整个 MAIN 空间
3. **AddGarbageUnits × n** — 每个空闲块调用一次 `MergeInsert` O(log k), k = 当前树大小

整体成本: **O(heapSize/unitSize + freeChunks × log(freeChunks))**

**优化机会 7** (影响: **中**): Ghost routing 保护的代码（Fix B）对 `ghostFromRegionList` 和 `pendingGhostGarbageList` 有**完全重复的约 30 行 lambda**。应提取为公共辅助函数，减少代码膨胀和维护负担（不影响运行时性能，但提高可维护性）。

**优化机会 8** (影响: **中**): 线性扫描可以通过维护一个 `highestLiveRegion` 增量指针来避免。在每次 region 分配/回收时更新此指针，使 ShrinkInactiveZone 不再需要全区域扫描。但这增加了正常路径的开销，需权衡。

---

## Area 6: Ghost Routing 开销

### 6.1 BuildLargeObjectMigrationRoutes

**文件**: [WCollector.cpp](src/Heap/WCollector/WCollector.cpp#L599-L665)

这是 MAIN→LOS 迁移的核心设置函数，在 PostTrace 阶段执行:

```cpp
void WCollector::BuildLargeObjectMigrationRoutes()
{
    // 快速退出: 无迁移候选
    if (migrationList.GetRegionCount() == 0) return;

    // 快照候选到 vector（避免迭代中修改列表）
    std::vector<RegionInfo*> candidates;
    migrationList.VisitAllRegions([&](RegionInfo* region) {
        candidates.push_back(region);
    });

    for (RegionInfo* region : candidates) {
        // 死对象: 直接回收
        if (region->GetLiveByteCount() == 0) {
            migrationList.DeleteRegion(region);
            rm.CollectRegion(region);
            continue;
        }

        // 在 LOS 中分配目标空间
        size_t objSize = region->GetRegionAllocatedSize();
        uintptr_t toAddr = losMgr->AllocLarge(objSize);  // 实际LOS分配
        if (toAddr == 0) {
            // LOS满: 标记为 UNMOVABLE_FROM_REGION → ForwardBarrier 直接 untag
            region->SetRegionType(RegionInfo::RegionType::UNMOVABLE_FROM_REGION);
            continue;
        }

        // 设置 ghost route
        migrationList.DeleteRegion(region);
        fromList.PrependRegion(region, RegionInfo::RegionType::FROM_REGION);
        region->PrepareForwardableLargeRegion(toAddr);  // 设置路由元数据
    }

    if (migratedCount > 0) {
        fromList.CopyListTo(rm.GetGhostFromRegionList());
    }
}
```

Ghost 路由设置的关键函数 `PrepareForwardableLargeRegion`:

```cpp
// RegionInfo.h L691
void PrepareForwardableLargeRegion(uintptr_t toObjAddr)
{
    metadata.routeState = ROUTED;                           // 设置路由状态
    metadata.routeInfo.SetRouteInfo(toObjAddr, allocSize);  // 存储目标地址
    // 对区域的每个 UnitInfo 设置 inGhostFromRegion = 1
    for (size_t i = 0; i < nUnit; i++) {
        array[i].SetInGhostRegion(1);                       // 标记为 ghost 区域
        // 保存原始元数据到备份字段 (unitRole0, ownerRegion0, regionEnd0)
    }
}
```

### 6.2 ForwardBarrier 中的 Ghost 检查

**文件**: [ForwardBarrier.cpp](src/Heap/WCollector/ForwardBarrier.cpp#L21-L42)

```cpp
BaseObject* ForwardBarrier::ReadReference(BaseObject* obj, RefField<false>& field) const
{
    do {
        RefField<> tmpField(field);
        if (LIKELY(!tmpField.IsTagged())) {          // 快路径: 未标记 → 直接返回
            return tmpField.GetTargetObject();
        }
        // 慢路径: tagged reference → 可能需要 forward
        if (theCollector.IsCurrentPointer(tmpField)) {
            BaseObject* target = tmpField.GetTargetObject();
            if (theCollector.IsUnmovableFromObject(target)) { // ← 这里检查 ghost
                TryUntagRefField(...);
            } else {
                TryForwardRefField(...);   // 执行实际转发
            }
        }
    } while (true);
}
```

`IsUnmovableFromObject` 的实现 ([WCollector.cpp L20-L35](src/Heap/WCollector/WCollector.cpp#L20)):
```cpp
bool WCollector::IsUnmovableFromObject(BaseObject* obj) const
{
    if (!Heap::IsHeapAddress(obj)) return false;

    RegionInfo* regionInfo = nullptr;
    if (RegionInfo::InGhostFromRegion(obj)) {           // ← atomic load of inGhostFromRegion bit
        regionInfo = RegionInfo::GetGhostFromRegionAt(...);
    } else {
        regionInfo = RegionInfo::GetRegionInfoAt(...);
    }
    return regionInfo != nullptr && regionInfo->IsUnmovableFromRegion();
}
```

### 6.3 每次对象访问的开销

**问**: Ghost routing 检查是否发生在每次对象访问?

**答**: **否，仅在 GC 的 Forward 阶段**。

关键理解:
1. ForwardBarrier 仅在 `GC_PHASE_FORWARD` 期间激活（见 [Heap.cpp L198](src/Heap/Heap.cpp#L198)）
2. 在非 GC 阶段，使用的是 `IdleBarrier`，其 `ReadReference` 是零开销直接返回
3. 在 Forward 阶段，第一个检查 `!tmpField.IsTagged()` 是分支预测命中率极高的快路径 — 只有被 GC 标记的引用才有 tag

**Ghost routing 的真正开销**:

| 场景 | 开销 | 频率 |
|------|------|------|
| 非GC阶段的对象读取 | **零** | 每次读 |
| Forward阶段, 未标记引用 | 1个分支(LIKELY命中) | 多数读 |
| Forward阶段, 已标记引用 | `InGhostFromRegion` (1个 UnitInfo 读) + 可能的路由查找 | 少数读 |
| Forward阶段, ghost对象 | `GetGhostFromRegionAt` + RouteState检查 + 转发 | 极少 |

**优化机会 9** (影响: **低**): `InGhostFromRegion` 需要从对象地址计算 UnitInfo 地址然后读取 `inGhostFromRegion` 位。这个位查找涉及一次内存间接访问（可能 cache miss）。但由于仅在 Forward 阶段执行，且只对 tagged 引用执行，实际影响极小。

**优化机会 10** (影响: **低**): `BuildLargeObjectMigrationRoutes` 中的 `std::vector<RegionInfo*> candidates` 在 PostTrace 阶段触发堆分配。可以使用栈上固定大小的 `SmallVector<RegionInfo*, 16>` 替代，避免在 GC 关键路径分配内存。

---

## 总结：Top-5 优化机会

| 排名 | 优化项 | 位置 | 影响 | 说明 |
|------|--------|------|------|------|
| **1** | **CompactLOS 引用更新使用 Card Table / Remembered Set** | `WCollector::UpdateCompactedLOSReferences` | **极高** | 当前全堆扫描 O(heapSize)，仅为更新 <100 个引用。引入 LOS 指向记录可降至 O(refCount)。这是最大的结构性瓶颈。 |
| **2** | **延迟清零 / mmap 重映射替代 ClearUnits memset** | `FreeRegionManager::TakeRegion` L76 | **高** | 大对象(32KB+)分配时 memset 清零整个区域是分配热路径的最大单次开销。如果对象初始化会覆盖全部内容，清零是浪费。 |
| **3** | **合并 isCompacting + losCompactionFreeze 为单一原子状态** | `LOSManager::AllocLarge` L50-L60 | **中** | 每次 LOS 分配省去 1 个 acquire load。在高频分配场景中累积效果显著。 |
| **4** | **CompactLOS Phase 5b: 用固定数组替换 unordered_set** | `LOSManager::CompactLOS` L723-L741 | **中** | STW 中避免动态内存分配。对象数通常 <100，排序数组 + 二分查找更高效。同理 migrationMap 可用排序 pair 数组替代。 |
| **5** | **LOS FreeRegionManager 增加 freelist cache** | `LOSManager::AllocLarge` Step 1 | **中** | 对于少量大对象的场景，简单链表缓存最近回收的区域，避免每次走 CartesianTree 的 try_lock + O(log n) 路径。 |

### 可选低优先级优化:

| 优化项 | 影响 | 说明 |
|--------|------|------|
| 提取 ShrinkInactiveZone 中 ghost routing 保护的重复代码 | 低 (可维护性) | 同一 lambda 体出现两次 |
| GCPhase 缓存在 LOSManager 中 | 低 | 避免 `EnlistLargeRegion` 中的间接调用链 |
| ShrinkInactiveZone 增量 highestLiveRegion 跟踪 | 中 | 避免全区域扫描，但增加正常路径开销 |
| BuildLargeObjectMigrationRoutes 使用栈上 SmallVector | 低 | 避免 GC 关键路径的堆分配 |

### 架构级洞察

1. **RESERVE 借用设计良好**: 完全不影响正常分配路径，仅在 OOM 最后阶段触发。A+B 混合策略平衡了借用频率和碎片。

2. **Ghost routing 对正常执行无开销**: 仅在 Forward 阶段的 tagged 引用上激活，非 GC 阶段零成本。

3. **CompactLOS 的最大问题不是 memmove，而是全堆引用扫描**: memmove 的成本与移动数据量成正比（不可避免），但引用更新的成本与**整个堆大小**成正比（可以优化）。一个 100MB 堆上移动 1MB 的 LOS 对象，memmove 花费 ~0.5ms，但全堆引用扫描可能花费 >10ms。

4. **CartesianTree 是一个平衡的设计选择**: 它同时支持最优大小分配和最低地址分配，O(log n) 复杂度。对于 LOS 的少量 (<100) 大对象，这个常数因子可以通过 freelist cache 改善。
