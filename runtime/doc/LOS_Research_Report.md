# LOS (Large Object Space) 实现研究报告

> 本报告基于 Cangjie Runtime 源码的全面分析，覆盖 LOS 分配、GC、压缩、迁移、RESERVE 借用/归还等完整流程。

## 1. 概述与源文件清单

### 1.1 什么是 LOS

LOS（Large Object Space）是 Cangjie Runtime 专门用于管理**大对象**的独立内存空间。大对象的默认阈值为 **32KB**（可通过环境变量 `cjLargeObjectThreshold` 配置）。LOS 位于堆地址空间的**高端**，从高地址向低地址增长，与 MAIN Space 的增长方向相反。

### 1.2 相关源文件

| 文件 | 行数 | 角色 |
|------|------|------|
| `src/Heap/Allocator/LOSManager.h` | ~434 | LOS 管理器头文件。定义 `LOSManager` 类，包含 `FreeRegionManager`、Region 链表、压缩计划等核心数据结构 |
| `src/Heap/Allocator/LOSManager.cpp` | ~840 | LOS 管理器实现。分配、回收、压缩、重建 FreeRegionManager 的全部逻辑 |
| `src/Heap/Allocator/ReserveManager.h` | ~204 | RESERVE 缓冲区管理器头文件。定义 `mainWatermark`/`losWatermark` 双向借用模型 |
| `src/Heap/Allocator/ReserveManager.cpp` | ~220 | RESERVE 管理器实现。`BorrowForLOS()`/`BorrowForMain()`/`ReclaimXxxBorrowed()` |
| `src/Heap/Allocator/RegionManager.h` | ~780 | 中央 Region 管理器头文件。`AllocLarge()` 路由逻辑（LOS → MAIN 回退） |
| `src/Heap/Allocator/RegionManager.cpp` | ~2047 | Region 管理器实现。初始化空间划分、`TakeRegion()`、`ShrinkInactiveZone()`、迁移候选集装配 |
| `src/Heap/Allocator/RegionSpace.h` | ~279 | 分配空间头文件。`CollectLargeGarbage()`、`AssembleGarbageCandidates()` 编排 |
| `src/Heap/Allocator/RegionSpace.cpp` | ~357 | 分配入口。`TryAllocateOnce()`（大小分流）、`ShouldRetryAllocation()`（OOM 重试 + RESERVE 借用触发）|
| `src/Heap/Allocator/FreeRegionManager.h` | ~220 | 基于 CartesianTree 的空闲 Region 管理器。`dirtyUnitTree` + `releasedUnitTree` 双树模型 |
| `src/Heap/WCollector/WCollector.cpp` | ~1160 | GC 收集器。编排 LOS 回收/压缩/迁移/引用更新的完整流程 |

## 2. 内存布局与初始化

### 2.1 地址空间布局

```
低地址                                                                      高地址
┌──────────────────┬──────────────────┬──────────────────┐
│    MAIN Space    │  RESERVE Zone    │    LOS Space     │
│  (向高地址增长)    │  (双向借用缓冲)    │  (向低地址增长)    │
├──────────────────┼──────────────────┼──────────────────┤
│ regionHeapStart  │ mainEnd          │ losUnitStart     │
│      ↑           │ = reserveStart   │  = reserveEnd    │
│  inactiveZone    │                  │  losInactiveZone │
│      ↑           │ mainWm ↑  ↓ losWm│      ↓          │
│ (CAS 增长)       │ (借用水位线)      │ (CAS 增长到低地址) │
└──────────────────┴──────────────────┴──────────────────┘
```

### 2.2 默认比例

通过环境变量 `cjMainRatio`、`cjReserveRatio`、`cjLOSRatio` 配置，**默认 6:3:1**：

- MAIN: 60% — 普通小对象 + MAIN 中的大对象回退
- RESERVE: 30% — 双向缓冲区，可被 MAIN 或 LOS 借用
- LOS: 10% — 专用大对象空间

### 2.3 初始化流程 (`RegionManager::Initialize`)

```cpp
// RegionManager.cpp, line ~411
void RegionManager::Initialize(uintptr_t heapStart, size_t totalUnits) {
    // 1. 读取环境变量比例
    int mainRatio = GetEnvInt("cjMainRatio", 6);
    int reserveRatio = GetEnvInt("cjReserveRatio", 3);
    int losRatio = GetEnvInt("cjLOSRatio", 1);
    int totalRatio = mainRatio + reserveRatio + losRatio;

    // 2. 按比例计算各空间的 unit 数量
    size_t mainUnitCount = totalUnits * mainRatio / totalRatio;
    size_t reserveUnitCount = totalUnits * reserveRatio / totalRatio;
    size_t losUnitCount = totalUnits - mainUnitCount - reserveUnitCount;

    // 3. 创建 ReserveManager（cjReserveRatio=0 时跳过）
    if (reserveRatio > 0)
        reserveManager = new ReserveManager(mainEnd, reserveEnd);

    // 4. 创建 LOSManager（cjLOSRatio=0 时跳过）
    if (losRatio > 0)
        losManager = new LOSManager(losStart, losEnd, &losFreeMgr, reserveManager);

    // 5. 初始化 MAIN freeRegionManager（容量 = mainUnitCount + reserveUnitCount）
    freeRegionManager.Initialize(0, mainUnitCount + reserveUnitCount);

    // 6. LOS freelistManager 的 baseUnit = mainUnitCount，容量 = reserveUnitCount + losUnitCount
    losFreeMgr.Initialize(mainUnitCount, reserveUnitCount + losUnitCount);
}
```

关键设计点：
- **LOS 的 `FreeRegionManager` 使用 `baseUnit = mainUnitCount`**，这样 RESERVE-borrowed regions 的 unit 索引对 losFreeMgr 来说是有效的，未来归还后可无缝回收。
- MAIN 的 `freeRegionManager` 容量包含 RESERVE 空间，允许 MAIN 借用 RESERVE 区域后将其纳入正常分配流程。

## 3. LOS 分配路径 (Allocation Path)

### 3.1 整体调用链

```
RegionSpace::TryAllocateOnce(size)
  └─ size >= largeObjectThreshold ?
       └─ YES → RegionManager::AllocLarge(size)
                  ├─ [1] LOSManager::AllocLarge(size)     // LOS 优先
                  │     ├─ losFreeMgr.TakeRegion()         // 空闲树
                  │     └─ AllocFromInactiveZone()         // 扩展 losInactiveZone
                  └─ [2] AllocLargeInMain(size)            // MAIN 回退
                        ├─ freeRegionManager.TakeRegion()
                        └─ inactiveZone CAS 扩展
       └─ NO  → AllocBuffer() / AllocPinned()
```

### 3.2 入口：`RegionSpace::TryAllocateOnce`

```cpp
// RegionSpace.cpp, line ~28
uintptr_t RegionSpace::TryAllocateOnce(size_t size) {
    if (pinned) return AllocPinned(size);
    if (size >= largeObjectThreshold) {
        return regionManager.AllocLarge(size);
    }
    return AllocBuffer(size);
}
```

### 3.3 路由：`RegionManager::AllocLarge`

```cpp
// RegionManager.h, inline, line ~253
uintptr_t AllocLarge(size_t size) {
    // 优先分配到 LOS
    if (losManager != nullptr && !losManager->IsCompacting()) {
        uintptr_t addr = losManager->AllocLarge(size);
        if (addr != 0) return addr;
    }
    // 回退到 MAIN
    return AllocLargeInMain(size);
}
```

**注意**：`IsCompacting()` 检查在每次 `AllocLarge` 调用时都会执行（原子 load），这是热路径上的固定开销。

### 3.4 核心：`LOSManager::AllocLarge`

```cpp
// LOSManager.cpp, line ~48
uintptr_t LOSManager::AllocLarge(size_t size) {
    // 快速拒绝：压缩期间不分配
    if (isCompacting.load(std::memory_order_acquire)) return 0;
    if (losCompactionFreeze.load(std::memory_order_acquire)) return 0;

    size_t unitCount = (size + RegionInfo::UNIT_SIZE - 1) / RegionInfo::UNIT_SIZE;

    // Step 1: 从空闲树分配
    uintptr_t addr = losFreeMgr.TakeRegion(unitCount, /* lowAddressFirst */ false);
    if (addr != 0) {
        InitLargeRegion(addr, size, unitCount);
        return addr;
    }

    // Step 2: 扩展 inactiveZone
    addr = AllocFromInactiveZone(unitCount);
    if (addr != 0) {
        InitLargeRegion(addr, size, unitCount);
        return addr;
    }
    return 0;  // 分配失败 → 由上层重试/OOM 处理
}
```

### 3.5 InactiveZone 扩展：`AllocFromInactiveZone`

```cpp
// LOSManager.cpp, line ~99
uintptr_t LOSManager::AllocFromInactiveZone(size_t unitCount) {
    size_t allocSize = unitCount * RegionInfo::UNIT_SIZE;
    uintptr_t effectiveStart = GetEffectiveLOSStart(); // 考虑 RESERVE losWatermark
    while (true) {
        uintptr_t oldZone = losInactiveZone.load(std::memory_order_acquire);
        uintptr_t newZone = oldZone - allocSize;  // LOS 向低地址增长
        if (newZone < effectiveStart) return 0;    // 空间不足
        if (losInactiveZone.compare_exchange_weak(oldZone, newZone)) {
            return newZone;
        }
        // CAS 失败 → 重试
    }
}
```

`GetEffectiveLOSStart()` 返回 `reserveManager ? reserveManager->GetLOSWatermark() : losStart`，即考虑了 RESERVE 已借出的部分。

### 3.6 分配失败的重试机制

当 `AllocLarge` 返回 0 时，控制流回到 `RegionSpace::ShouldRetryAllocation()`，该函数实现 5 级重试策略：

| 级别 | 策略 | 说明 |
|------|-----|------|
| P1 | Reschedule | 让出 CPU，等待其他线程完成 |
| P2 | Wait for GC | 等待正在进行的 GC 完成 |
| P3 | Trigger OOM GC | 主动触发一次 OOM GC |
| P4 | Wait for OOM GC | 等待 OOM GC 完成 |
| P5 | **RESERVE Borrow** | 调用 `TryBorrowFromReserveIfNeeded()` |
| P6 | OutOfMemory | 抛出 OOM 异常 |

**P5 路径关键代码：**
```cpp
// RegionSpace.cpp, line ~73
void RegionSpace::TryBorrowFromReserveIfNeeded() {
    // 检查 LOS 空间余量
    if (losManager && losManager->GetAvailableSpace() < largeObjectThreshold) {
        reserveManager->BorrowForLOS(largeObjectThreshold);
    }
    // 检查 MAIN 空间余量
    if (GetMainAvailableSpace() < threshold) {
        reserveManager->BorrowForMain(threshold);
    }
}
```

## 4. LOS GC / 回收路径 (GC Path)

### 4.1 GC 编排全流程

GC 由 `WCollector::DoGarbageCollection()` 编排，LOS 相关步骤标注如下：

```
DoGarbageCollection()
  ├─ AssembleGarbageCandidates()          // [LOS] 装配垃圾候选集
  │    ├─ AssembleMigrationCandidates()   // [LOS] MAIN→LOS 迁移候选（opt-in）
  │    ├─ AssembleSmall/Pinned/Large()    // MAIN 侧
  │    └─ losMgr->AssembleLOSGarbageCandidates()  // [LOS] LOS 侧
  │
  ├─ TraceHeap()                          // 并发标记（Enum + Trace 阶段）
  │    └─ EnlistLargeRegion()             // [LOS] 被追踪到的 LOS region 加入 trace 列表
  │
  ├─ PostTrace()
  │    ├─ HandleTraceRegions()            // [LOS] 处理 LOS trace regions
  │    ├─ CollectLargeGarbage()           // [LOS] 回收死亡大对象
  │    │    ├─ MAIN 大对象回收
  │    │    └─ losMgr->CollectLOSGarbage()  // [LOS] LOS 死亡区域回收
  │    ├─ CompactLOSIfNeeded()            // [LOS] 判断是否需要压缩
  │    └─ BuildLargeObjectMigrationRoutes() // [LOS] 构建迁移路由表
  │
  ├─ Preforward()
  ├─ ForwardFromSpace()                   // STW 转发
  ├─ FinalizeLOSCompactionIfNeeded()      // [LOS] 完成异步压缩
  ├─ ShrinkInactiveZone()                 // [LOS] OOM GC 时收缩非活跃区域
  ├─ CollectSmallSpace()
  └─ CleanupMigrationCandidates()         // [LOS] 清理迁移候选集
```

### 4.2 候选集装配：`AssembleLOSGarbageCandidates`

```cpp
// LOSManager.cpp, line ~143
void LOSManager::AssembleLOSGarbageCandidates() {
    // 合并 recent → old，清除存活信息
    losOldLargeRegionList.MergeAndClearLiveInfo(losRecentLargeRegionList);
}
```

LOS 使用两个链表管理 region 生命周期：
- `losRecentLargeRegionList`：本轮 GC 之后新分配的 region
- `losOldLargeRegionList`：上一轮 GC 周期已存在的 region（sweep 目标）

### 4.3 标记追踪：`EnlistLargeRegion`

```cpp
// LOSManager.cpp, line ~125
void LOSManager::EnlistLargeRegion(RegionInfo* region) {
    if (isTracing) {
        losLargeTraceRegions.Add(region);  // GC trace 期间：加入 trace cache
    } else {
        losRecentLargeRegionList.AddRegion(region);  // 非 GC 期间：加入 recent 列表
    }
}
```

### 4.4 回收：`CollectLOSGarbage`

```cpp
// LOSManager.cpp, line ~157
size_t LOSManager::CollectLOSGarbage() {
    size_t reclaimed = 0;
    // 构建待压缩保护集（pendingCompactionPlan 中的目标地址）
    std::unordered_set<uintptr_t> protectedTargets;
    for (auto& entry : pendingCompactionPlan) {
        protectedTargets.insert(entry.targetAddr);
    }

    RegionInfo* region = losOldLargeRegionList.GetHeadRegion();
    while (region != nullptr) {
        RegionInfo* next = region->GetNextRegion();

        if (!region->HasLiveObjects()) {
            // 死亡区域 → 回收
            losOldLargeRegionList.DeleteRegion(region);

            // 下半部分的 release 区域不回收物理页
            if (region->IsReleasedRegion()) {
                ReleaseLOSRegion(region);  // → losFreeMgr.AddReleaseUnits()
            } else {
                ReclaimLOSRegion(region);  // → losFreeMgr.AddGarbageUnits()
            }

            reclaimed += region->GetRegionSize();
        } else {
            // 存活区域 → 重置标记位
            region->ClearLiveInfo();
        }
        region = next;
    }
    return reclaimed;
}
```

**关键设计点：**
- 死亡区域通过 `losFreeMgr` 回收（`AddGarbageUnits` / `AddReleaseUnits`），**不是**回到 MAIN 的 freeRegionManager
- `protectedTargets` 防止尚未完成的压缩计划的目标被意外回收
- `AddGarbageUnits` → 插入 `dirtyUnitTree`（脏页，可直接使用）
- `AddReleaseUnits` → 插入 `releasedUnitTree`（已释放物理页，需重新 commit）

## 5. LOS 压缩路径 (Compaction Path)

### 5.1 触发条件

```cpp
// WCollector.cpp, line ~667
void WCollector::CompactLOSIfNeeded() {
    if (!cjEnableLOSCompact) return;  // 环境变量 cjEnableLOSCompact 控制

    bool hasAsyncPlan = losManager->HasPendingCompactionPlan();
    bool needsCompact = losManager->NeedsCompaction();

    // 两种触发路径：
    // 1. 有上一轮遗留的异步压缩计划待执行
    // 2. OOM GC 场景下碎片率超标
    if (hasAsyncPlan || (isOOMGC && needsCompact)) {
        CompactLOSOnOOMIfNeeded();
    }
}
```

碎片率判断：
```cpp
// LOSManager.h
static constexpr double LOS_COMPACTION_FRAG_THRESHOLD = 0.3; // 30%

bool NeedsCompaction() {
    return GetFragmentationRatio() > LOS_COMPACTION_FRAG_THRESHOLD;
}
```

### 5.2 CompactLOS 七阶段流程

`LOSManager::CompactLOS()` 是压缩的核心函数（~220 行），包含以下阶段：

```
CompactLOS()
  ├─ Phase 1: Classify (分类)
  │   收集所有存活 region → movableRegions + pinnedRegions
  │
  ├─ Phase 2: Plan (规划)
  │   按地址从高到低排序 movable regions
  │   从 losEnd 向低地址分配目标地址
  │   Strategy A: 不跨越最高 pinned region
  │
  ├─ Phase 3: Build Migration Map (构建迁移映射)
  │   migrationMap[oldAddr] = newAddr（unordered_map）
  │
  ├─ Phase 4: Detach (脱离)
  │   将所有 region 从 old/recent 链表中脱离
  │
  ├─ Phase 5a: Move (移动)
  │   对每个 movable region 执行 memmove_s()
  │   更新 RegionInfo 元数据
  │
  ├─ Phase 5b: Rebuild Lists (重建链表)
  │   将所有 region（已移动 + pinned）重新加入 region 链表
  │
  ├─ Phase 6: Rebuild FreeRegionManager (重建空闲管理器)
  │   ClearTrees() → 遍历排序后的存活 region → 插入间隙
  │
  └─ Phase 7: Reclaim RESERVE (归还借用)
      调用 ReclaimLOSBorrowed()，将不再需要的 RESERVE 空间归还
```

### 5.3 引用更新：`UpdateCompactedLOSReferences`

压缩后必须更新所有指向旧地址的引用：

```cpp
// WCollector.cpp, line ~715
void WCollector::UpdateCompactedLOSReferences() {
    auto& migMap = losManager->GetMigrationMap();

    // 扫描所有堆对象（MAIN + LOS）
    regionSpace.ForEachObjUnsafe([&](ObjectHeader* obj) {
        obj->VisitReferences([&](ObjectHeader** ref) {
            uintptr_t addr = (uintptr_t)*ref;
            auto it = migMap.find(GetRegionStart(addr));
            if (it != migMap.end()) {
                uintptr_t offset = addr - it->first;
                *ref = (ObjectHeader*)(it->second + offset);
            }
        });
    });

    // 扫描所有根：
    // - 静态变量 (statics)
    // - 栈帧 (stacks)
    // - Finalizer 队列
    // - 导出表 (exports)
    // - 并发模型 (concurrency model)
    UpdateRoots(migMap);
}
```

**性能警告**：这是一次**全堆扫描**，时间复杂度 O(存活对象总数)，是 LOS 压缩最大的性能开销所在。

### 5.4 异步压缩路径

除了 STW 同步压缩外，还有基于 Forwarding 的异步压缩：

```
BuildCompactionPlan()          // PostTrace: 规划但不执行
  → InstallPendingCompactionForForwarding()  // 在 Forward 表中安装转发
  → FinalizePendingCompaction()              // Forward 完成后执行实际搬移
```

这种路径利用已有的 GC forwarding 机制来处理引用更新，避免单独做全堆扫描。

### 5.5 重建 FreeRegionManager

```cpp
// LOSManager.cpp, line ~790
void LOSManager::RebuildFreeRegionManager() {
    losFreeMgr.ClearTrees();

    // 排序所有存活 region
    std::sort(liveRegions.begin(), liveRegions.end(),
              [](RegionInfo* a, RegionInfo* b) {
                  return a->GetRegionStart() < b->GetRegionStart();
              });

    // 遍历排序后的 region，将间隙作为 dirty units 插入
    uintptr_t cursor = losInactiveZone;
    for (auto* region : liveRegions) {
        uintptr_t gapStart = cursor;
        uintptr_t gapEnd = region->GetRegionStart();
        if (gapEnd > gapStart) {
            size_t gapUnits = (gapEnd - gapStart) / RegionInfo::UNIT_SIZE;
            losFreeMgr.AddGarbageUnits(toUnitIdx(gapStart), gapUnits);
        }
        cursor = region->GetRegionEnd();
    }
    // losEnd 之后的空间也是空闲的
    if (losEnd > cursor) {
        size_t tailUnits = (losEnd - cursor) / RegionInfo::UNIT_SIZE;
        losFreeMgr.AddGarbageUnits(toUnitIdx(cursor), tailUnits);
    }
}
```

## 6. MAIN→LOS 迁移路径 (Migration Path)

### 6.1 背景

除了直接在 LOS 中分配大对象外，MAIN Space 中也可能存在大对象（当 LOS 空间不足时，`AllocLarge` 会回退到 MAIN）。这些大对象会增加 MAIN 的碎片压力。Migration 机制允许将 MAIN 中的大对象迁移到 LOS，需要通过环境变量 `cjEnableLOSMigration` 显式启用。

### 6.2 候选集装配

```cpp
// RegionManager.cpp, line ~589
void RegionManager::AssembleMigrationCandidates() {
    if (!cjEnableLOSMigration) return;

    RegionInfo* region = oldLargeRegionList.GetHeadRegion();
    while (region != nullptr) {
        RegionInfo* next = region->GetNextRegion();
        // 将 MAIN 中的大对象标记为 FROM_REGION（迁移源）
        region->SetRegionType(FROM_REGION);
        oldLargeRegionList.DeleteRegion(region);
        migrationCandidateList.AddRegion(region);
        region = next;
    }
}
```

**执行时序**：`AssembleMigrationCandidates()` 在 `AssembleGarbageCandidates()` 中**先于**候选集合并执行，确保迁移候选在 Trace 阶段被正确标记。

### 6.3 迁移路由构建

```cpp
// WCollector.cpp, line ~599
void WCollector::BuildLargeObjectMigrationRoutes() {
    RegionInfo* region = migrationCandidateList.GetHeadRegion();
    while (region != nullptr) {
        RegionInfo* next = region->GetNextRegion();

        // 在 LOS 中为迁移目标分配空间
        uintptr_t targetAddr = losManager->AllocLarge(region->GetRegionSize());

        if (targetAddr != 0) {
            // 成功：设置 ghost routing
            // Ghost route = 保留旧地址的 region 元信息，使旧指针自动转发到新地址
            region->SetGhostRoute(targetAddr);
        } else {
            // LOS 空间不足：标记为不可移动
            region->SetRegionType(UNMOVABLE_FROM_REGION);
        }
        region = next;
    }
}
```

Ghost Routing 是一种透明转发机制：旧 region 保留元数据，并在后续 GC forwarding 阶段将所有旧地址引用转发到新 LOS 地址。

## 7. RESERVE 借用/归还机制

### 7.1 数据模型

```
RESERVE Zone: [reserveStart (=mainEnd) ... reserveEnd (=losStart)]

           mainWatermark ──→   ←── losWatermark
           (MAIN 借用方向)       (LOS 借用方向)
           grows →               ← grows

Available = losWatermark - mainWatermark （未借出的中间空隙）
```

- `mainWatermark`：从 `reserveStart` 向高地址增长（MAIN 侧借用）
- `losWatermark`：从 `reserveEnd` 向低地址增长（LOS 侧借用）
- 两者之间的空隙 = RESERVE 可用空间

### 7.2 借用流程

#### BorrowForLOS

```cpp
// ReserveManager.cpp
size_t ReserveManager::BorrowForLOS(size_t requestSize) {
    std::lock_guard<std::mutex> lock(borrowMutex);
    size_t available = GetAvailable();
    if (available < requestSize) return 0;

    // 计算借用块大小：max(requestSize, max(minChunk, available/10))
    size_t chunk = ComputeBorrowChunk(requestSize, available);

    // 提交：losWatermark 向低地址移动
    CommitChunkForLOS(chunk);  // losWatermark -= chunk
    return chunk;
}
```

#### BorrowForMain

```cpp
size_t ReserveManager::BorrowForMain(size_t requestSize) {
    std::lock_guard<std::mutex> lock(borrowMutex);
    size_t available = GetAvailable();
    if (available < requestSize) return 0;

    size_t chunk = ComputeBorrowChunk(requestSize, available);
    CommitChunkForMain(chunk);  // mainWatermark += chunk
    return chunk;
}
```

### 7.3 借用块策略：`ComputeBorrowChunk`

```cpp
// ReserveManager.h, line ~150
size_t ComputeBorrowChunk(size_t requestSize, size_t available) {
    size_t minChunk = MIN_BORROW_CHUNK_UNITS * UNIT_SIZE;  // 16 units
    size_t tenthAvail = available / BORROW_RATIO_DIVISOR;   // available / 10
    size_t chunk = std::max(requestSize, std::max(minChunk, tenthAvail));
    // 对齐到 BORROW_CHUNK_ALIGN_UNITS (16 units)
    chunk = AlignUp(chunk, BORROW_CHUNK_ALIGN_UNITS * UNIT_SIZE);
    return std::min(chunk, available);
}
```

策略特点：
- **最小 16 units**：避免频繁的小额借用
- **available/10**：按比例借用，避免一次耗尽
- **16-unit 对齐**：减少碎片

### 7.4 归还流程

#### ReclaimLOSBorrowed（LOS 归还）

触发位置：
1. `LOSManager::CompactLOS()` Phase 7
2. `RegionManager::ShrinkInactiveZone()` Phase B（间接，通过 `ReclaimMainBorrowed`）

```cpp
// ReserveManager.cpp
size_t ReserveManager::ReclaimLOSBorrowed(uintptr_t newWatermark) {
    std::lock_guard<std::mutex> lock(borrowMutex);
    if (newWatermark >= losWatermark && newWatermark <= reserveEnd) {
        size_t reclaimed = newWatermark - losWatermark;
        losWatermark = newWatermark;  // 推回高地址
        return reclaimed;
    }
    return 0;
}
```

#### ReclaimMainBorrowed（MAIN 归还）

触发位置：`ShrinkInactiveZone()` Phase B

```cpp
size_t ReserveManager::ReclaimMainBorrowed(uintptr_t newWatermark) {
    std::lock_guard<std::mutex> lock(borrowMutex);
    if (newWatermark >= reserveStart && newWatermark <= mainWatermark) {
        size_t reclaimed = mainWatermark - newWatermark;
        mainWatermark = newWatermark;  // 推回低地址
        return reclaimed;
    }
    return 0;
}
```

### 7.5 借用触发时机

RESERVE 借用仅在 **OOM 重试的第 5 级（P5）** 触发，即：

```
TryAllocateOnce() 失败
  → ShouldRetryAllocation()
    → P1: Reschedule
    → P2: Wait GC
    → P3: Trigger OOM GC
    → P4: Wait OOM GC
    → P5: TryBorrowFromReserveIfNeeded()  ← HERE
    → P6: OOM Exception
```

这是一个**延迟借用**设计：先尝试 GC 回收空间，仅在 GC 后仍不够时才借用 RESERVE。

## 8. ShrinkInactiveZone 详解

`RegionManager::ShrinkInactiveZone()` 是 OOM GC 专用的两阶段收缩函数（仅 STW 期间调用），负责回收 MAIN 和 RESERVE-borrowed 空间中未使用的高地址区域。

### 8.1 Phase A：收缩 MAIN inactiveZone

```
扫描范围: [regionHeapStart, min(oldInactive, mainEnd))
找到 highestLiveEnd = 最高存活 region 的 end 地址（cap 到 mainEnd）
newInactive = highestLiveEnd
回收量 = min(oldInactive, mainEnd) - newInactive
```

关键修正点（P7-fix）：当大对象 region 跨越 MAIN-RESERVE 边界时，`highestLiveEnd` 限制在 `mainEnd`，防止与 Phase B 的处理范围重叠。

### 8.2 Phase B：收缩 RESERVE-borrowed 空间

```
扫描范围: [mainEnd, min(oldInactive, mainWatermark))
找到 highestLiveBorrowedEnd = 最高存活借用 region 的 end 地址
如果 highestLiveBorrowedEnd < mainWatermark → 可收缩
newWatermark = max(highestLiveBorrowedEnd, mainEnd)
调用 ReclaimMainBorrowed(newWatermark) → 推回 mainWatermark
```

### 8.3 Ghost Routing 保护（Fix B）

在决定收缩边界前，必须保护 ghost routing 的目标 region 不被释放：

```cpp
ghostFromRegionList.VisitAllGhostRegions([&](RegionInfo* ghost) {
    // 检查 ghost route 的 toRegion1/toRegion2
    // 如果目标在 MAIN → 提升 highestLiveEnd
    // 如果目标在 RESERVE-borrowed → 提升 highestLiveBorrowedEnd
});
// 同样扫描 pendingGhostGarbageList
```

这确保了即使 ghost-from region 已标记为垃圾，其指向的 TO-space region 在 ghost 完全消除前不会被释放。

### 8.4 FreeRegionManager 重建

收缩后必须重建 `freeRegionManager`，因为高地址的空闲节点已失效：

```
1. ClearTrees() — 原子清除所有节点
2. 重新扫描 [regionHeapStart, newInactive) → 插入 MAIN 空闲 chunks
3. 重新扫描 [mainEnd, highestLiveBorrowedEnd) → 插入借用区空闲 chunks
```

### 8.5 物理页释放

收缩后调用 `RegionInfo::ReleaseUnits()` 释放回收区域的物理页（madvise DONTNEED 或类似机制），将内存归还操作系统。

## 9. 性能敏感热路径分析

### 9.1 分配热路径

| 热点 | 位置 | 类型 | 影响 |
|------|------|------|------|
| `isCompacting` 原子 load | `RegionManager::AllocLarge` | 每次大对象分配 | 低：acquire load 在 x86 上接近零开销 |
| `losCompactionFreeze` 原子 load | `LOSManager::AllocLarge` | 每次 LOS 分配 | 低：同上 |
| `losInactiveZone` CAS 竞争 | `AllocFromInactiveZone` | 多线程并发分配 | **中**：高并发下 CAS 重试率上升 |
| `FreeRegionManager::TakeRegion` 锁 | `losFreeMgr.TakeRegion()` | 每次空闲树分配 | **中-高**：try_lock dirtyTree → lock releasedTree，CartesianTree 操作非 O(1) |
| `AllocLargeInMain` 向上对齐 | `RegionManager::AllocLargeInMain` | MAIN 回退路径 | 低：仅在 LOS 满时 |

#### FreeRegionManager 的 try_lock 模式

```cpp
// FreeRegionManager.h
uintptr_t TakeRegion(size_t unitCount, bool lowAddressFirst) {
    // Step 1: 先 try_lock dirtyTree（无阻塞）
    if (dirtyMutex.try_lock()) {
        auto result = dirtyUnitTree.TakeRegion(unitCount, lowAddressFirst);
        dirtyMutex.unlock();
        if (result) return result;
    }
    // Step 2: 阻塞 lock releasedTree
    std::lock_guard<std::mutex> lock(releasedMutex);
    return releasedUnitTree.TakeRegion(unitCount, lowAddressFirst);
}
```

**问题**：在高频分配场景下，`dirtyMutex` 的 try_lock 失败率可能显著上升，导致频繁跌入 `releasedTree`（需重新 commit 物理页）。

### 9.2 GC 热路径

| 热点 | 位置 | 类型 | 影响 |
|------|------|------|------|
| `CollectLOSGarbage` 中的 `unordered_set` | `LOSManager.cpp:157` | 每次 GC sweep | **中**：每次 GC 构建 protectedTargets 集合，含哈希操作 |
| `UpdateCompactedLOSReferences` 全堆扫描 | `WCollector.cpp:715` | 压缩后 | **高**：O(存活对象总数) 的全堆遍历，STW 暂停最大贡献者 |
| `RebuildFreeRegionManager` 排序 | `LOSManager.cpp:790` | 压缩/收缩后 | **中**：O(n log n) 排序 + 遍历 |
| `ShrinkInactiveZone` ghost 扫描 | `RegionManager.cpp:1830+` | OOM GC | **中**：遍历两个 ghost 链表的所有 region |
| `ForEachObjUnsafe` 全堆遍历 | `RegionManager.cpp:688` | 引用更新 | **高**：遍历 MAIN + LOS 所有 region 的所有对象 |

### 9.3 内存开销

| 数据结构 | 位置 | 大小 | 说明 |
|----------|------|------|------|
| `migrationMap` (unordered_map) | LOSManager | O(迁移对象数) | 压缩期间存活 |
| `pendingCompactionPlan` (vector) | LOSManager | O(待压缩 region 数) | 异步压缩期间存活 |
| `protectedTargets` (unordered_set) | CollectLOSGarbage 栈上 | O(待压缩 region 数) | 每次 GC sweep 临时创建 |
| CartesianTree 节点 | FreeRegionManager | O(空闲片段数) | 常驻 |

### 9.4 已识别的潜在优化点

1. **CartesianTree → 分层 Freelist**：对于大对象通常的大小类（32KB, 64KB, ...），可以用 size-class freelist 替代树结构，将分配降到 O(1)。

2. **全堆扫描消除**：`UpdateCompactedLOSReferences` 是最大的 STW 停顿来源。可考虑：
   - 用 remembered set 跟踪跨空间引用，仅扫描包含 LOS 引用的 region
   - 利用写屏障记录 MAIN→LOS 指针，避免全堆扫描

3. **protectedTargets 集合预计算**：当前每次 GC 都重新构建，可以在 `BuildCompactionPlan` 时一次性计算并缓存。

4. **CAS 竞争缓解**：`AllocFromInactiveZone` 的 CAS 在极高并发下可加入 backoff 策略。

5. **借用块大小自适应**：当前固定 available/10 的策略过于保守，可根据历史 OOM 频率动态调整。

## 10. 总结与建议

### 10.1 架构总结

LOS 是一个精心设计的大对象管理子系统，其核心设计特点：

1. **独立空间 + 回退策略**：LOS 优先分配，空间不足时透明回退到 MAIN，对调用者完全透明
2. **RESERVE 双向缓冲**：MAIN 和 LOS 可以按需借用 RESERVE 空间，实现弹性内存管理
3. **延迟借用**：借用仅在 OOM 重试的 P5 阶段触发，最大化正常 GC 的回收效果
4. **两种压缩路径**：STW 同步压缩 + 基于 Forwarding 的异步压缩
5. **双向迁移**：MAIN→LOS 迁移减少 MAIN 碎片，LOS 压缩减少 LOS 碎片
6. **Ghost Routing 安全保护**：收缩/回收操作均考虑 ghost 路由的目标 region 安全

### 10.2 关键数据流总结

```
分配流：
  TryAllocateOnce → AllocLarge → LOS(freeMgr/inactiveZone) → MAIN(fallback) → OOM retry → RESERVE borrow

GC 流：
  Assemble → Trace/Mark → Sweep(CollectLOSGarbage) → Compact(optional) → Migrate(optional) → Shrink(OOM only)

RESERVE 流：
  Borrow(P5 OOM retry) → ... → Reclaim(CompactLOS Phase7 / ShrinkInactiveZone Phase B)
```

### 10.3 建议

| 优先级 | 建议 | 预期收益 |
|--------|------|----------|
| 高 | 为 `UpdateCompactedLOSReferences` 引入 remembered set，避免全堆扫描 | 大幅降低压缩后 STW 停顿 |
| 高 | `protectedTargets` 缓存化，避免每次 GC 重新构建 | 减少每轮 GC 的哈希表开销 |
| 中 | FreeRegionManager 为常见大小类引入快速路径（size-class freelist） | 降低分配延迟 |
| 中 | CAS 竞争添加 exponential backoff | 高并发场景下降低 CPU 空转 |
| 低 | RESERVE 借用块大小自适应（根据 OOM 历史调整） | 减少借用次数 |
| 低 | `ShrinkInactiveZone` ghost 扫描并行化 | OOM GC 场景下减少 STW 停顿 |

---

*报告基于源码静态分析，代码片段为简化后的伪代码，实际实现细节以源码为准。*
