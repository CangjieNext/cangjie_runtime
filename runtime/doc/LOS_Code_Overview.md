# LOS (Large Object Space) 代码全览

> 生成日期: 2026-02-23
> 涵盖文件: `LOSManager.h` (434行), `LOSManager.cpp` (840行), 以及 `WCollector.cpp`, `RegionManager.cpp/h` 中的 LOS 相关代码。

---

## 目录

1. [架构概览](#1-架构概览)
2. [内存布局与初始化](#2-内存布局与初始化)
3. [大对象分配 (M2)](#3-大对象分配-m2)
4. [GC Sweep — 垃圾回收 (M3)](#4-gc-sweep--垃圾回收-m3)
5. [MAIN→LOS 迁移 (M3)](#5-mainlos-迁移-m3)
6. [LOS Sliding Compaction (M4a)](#6-los-sliding-compaction-m4a)
7. [Compaction 引用更新](#7-compaction-引用更新)
8. [Async Compaction 流水线](#8-async-compaction-流水线)
9. [RESERVE 借用与归还 (M5)](#9-reserve-借用与归还-m5)
10. [GC 主流程中的 LOS 集成点](#10-gc-主流程中的-los-集成点)
11. [关键数据结构](#11-关键数据结构)
12. [统计与诊断](#12-统计与诊断)

---

## 1. 架构概览

LOS 是 Cangjie Runtime 堆内存中专门为大对象（>= `largeObjectThreshold`，通常 32KB）设计的独立空间。它与 MAIN 空间共享同一个 RegionInfo 元数据数组，但拥有**独立的 FreeRegionManager**（独立的 dirtyTree + releaseTree），与 MAIN 的 FreeRegionManager 完全隔离。

### 核心组件

| 组件 | 文件 | 职责 |
|------|------|------|
| `LOSManager` | `LOSManager.h/cpp` | LOS 空间管理器：分配、回收、压缩、迁移 |
| `FreeRegionManager` | `FreeRegionManager.h` | LOS 独立的空闲 region 管理（Cartesian Tree） |
| `ReserveManager` | `ReserveManager.h/cpp` | RESERVE 缓冲区管理，支持 LOS 向 RESERVE 借用空间 |
| `RegionManager` | `RegionManager.h/cpp` | 总管理器，持有 `LOSManager*`，路由大对象分配 |
| `WCollector` | `WCollector.cpp` | GC 收集器，调用 LOS 的 sweep/compaction/migration |

### 分配路由总流程

```
应用分配大对象 (size >= threshold)
    → RegionManager::AllocLarge()
        → Step 1: LOSManager::AllocLarge() (若 LOS 启用且未在压缩中)
            → losFreeMgr.TakeRegion() (回收的 region)
            → AllocFromInactiveZone() (扩展 inactiveZone)
        → Step 2: 若 LOS 返回 0, 回退到 AllocLargeInMain() (在 MAIN 中分配)
```

---

## 2. 内存布局与初始化

### 堆内存布局

```
低地址                                                          高地址
┌──────────────┬───────────────────┬──────────────────────────────┐
│  MAIN Space  │  RESERVE Zone     │  LOS Space                   │
│  (mainRatio) │  (reserveRatio)   │  (losRatio)                  │
├──────────────┼───────────────────┼──────────────────────────────┤
│ regionHeap   │ reserveStart      │ losStart       losEnd        │
│ Start        │ = mainEnd         │ = reserveEnd   = regionHeapEnd│
└──────────────┴───────────────────┴──────────────────────────────┘
                ← MAIN 往高地址增长     LOS 往低地址增长 →
```

- **MAIN**: 从低地址向高地址分配 (inactiveZone 从 `regionHeapStart` 上升)
- **LOS**: 从高地址向低地址分配 (`losInactiveZone` 从 `losEnd` 下降)
- **RESERVE**: 中间缓冲区，MAIN 和 LOS 都可以借用

### 空间比例配置

通过环境变量配置 (默认 6:3:1)：
- `cjMainRatio` = 6 → MAIN
- `cjReserveRatio` = 3 → RESERVE
- `cjLOSRatio` = 1 → LOS
- 当 `cjLOSRatio=0` 时，`losManager = nullptr`，所有大对象走 MAIN

### 初始化流程 (`RegionManager::Initialize`)

```cpp
// RegionManager.cpp L420-475
int mainRatio = GetSpaceRatio("cjMainRatio", 6);
int reserveRatio = GetSpaceRatio("cjReserveRatio", 3);
int losRatio = GetSpaceRatio("cjLOSRatio", 1);

losUnitCount = (nUnit * losRatio) / totalRatio;
reserveUnitCount = (nUnit * reserveRatio) / totalRatio;
mainUnitCount = nUnit - losUnitCount - reserveUnitCount;

// LOS baseUnit = mainUnitCount (RESERVE 起始)
// Capacity = reserveUnitCount + losUnitCount (覆盖 RESERVE 扩展区 + 原生 LOS)
losManager = new LOSManager(*this, mainUnitCount, reserveManager);
losManager->Initialize(losStart, losEnd, losUnitStart, losUnitEnd, reserveUnitCount);
```

### LOSManager::Initialize

```cpp
// LOSManager.cpp L22-43
void LOSManager::Initialize(uintptr_t start, uintptr_t end, ...)
{
    losStart = start;
    losEnd = end;
    losInactiveZone.store(losEnd);  // 初始全空，从 losEnd 开始向低地址扩展
    losFreeMgr.Initialize(reserveUnitCount + losUnitCount);  // 树容量覆盖 RESERVE+LOS
}
```

**关键设计**: `losFreeMgr` 的 `baseUnit = mainUnitCount`，这意味着树的 index 空间覆盖了 RESERVE 扩展区和 LOS 原生空间。当 RESERVE 借用给 LOS 的 region 死亡后，可以直接通过 `losFreeMgr` 回收，无需特殊处理。

---

## 3. 大对象分配 (M2)

### AllocLarge 分配流程

```cpp
// LOSManager.cpp L47-95
uintptr_t LOSManager::AllocLarge(size_t size)
{
    // Guard 1: 压缩进行中 → 拒绝分配，强制 MAIN 回退
    if (isCompacting) return 0;
    // Guard 2: 压缩冻结窗口 → 同上
    if (losCompactionFreeze) return 0;

    size_t unitCount = CEIL(size / UNIT_SIZE);

    // Step 1: 从 losFreeMgr 分配 (dirtyTree → releaseTree)
    RegionInfo* region = losFreeMgr.TakeRegion(unitCount, LARGE_SIZED_UNITS);
    if (region != nullptr) → finishAllocation("freeMgr");

    // Step 2: 扩展 losInactiveZone (CAS 向低地址移动)
    region = AllocFromInactiveZone(unitCount);
    if (region != nullptr) → finishAllocation("inactiveZone");

    // 失败 → 返回 0, 调用方回退到 MAIN
    losAllocFailCount++;
    return 0;
}
```

### AllocFromInactiveZone — CAS 扩展

```cpp
// LOSManager.cpp L97-124
RegionInfo* LOSManager::AllocFromInactiveZone(size_t unitCount)
{
    size_t regionSize = unitCount * UNIT_SIZE;
    uintptr_t effectiveStart = GetEffectiveLOSStart(); // 包含 RESERVE 借用的低边界

    // CAS 循环: losInactiveZone -= regionSize
    while (!losInactiveZone.compare_exchange_weak(currentZone, newZone)) {
        if (currentZone < effectiveStart + regionSize) return nullptr;
        newZone = currentZone - regionSize;
    }

    return RegionInfo::InitRegionAt(newZone, unitCount, LARGE_SIZED_UNITS);
}
```

### EnlistLargeRegion — 区分 GC 阶段

```cpp
// LOSManager.cpp L126-142
void LOSManager::EnlistLargeRegion(RegionInfo* region)
{
    GCPhase phase = Heap::GetCollector().GetGCPhase();
    bool isTracing = (phase == TRACE || phase == CLEAR_SATB_BUFFER);

    if (losLargeTraceRegions.TryPrepend(region)) {
        if (isTracing) region->SetTraceRegionFlag(1);  // GC 正在 trace，标记为需扫描
    } else {
        losRecentLargeRegionList.Prepend(region);       // 非 trace 阶段，进 recent 列表
    }
}
```

### 上层路由 (`RegionManager::AllocLarge`)

```cpp
// RegionManager.h L252-270
inline uintptr_t AllocLarge(size_t size)
{
    // Step 1: 尝试 LOS (跳过压缩中的 LOS)
    if (losManager != nullptr && !losManager->IsCompacting()) {
        uintptr_t addr = losManager->AllocLarge(size);
        if (addr != 0) return addr;
    }
    // Step 2: 回退到 MAIN
    uintptr_t addr = AllocLargeInMain(size);
    if (addr != 0 && losManager != nullptr) {
        losManager->IncrementFallbackCount();
        VLOG("AllocLarge MAIN fallback: size=%zu addr=0x%zx (LOS full)", size, addr);
    }
    return addr;
}
```

---

## 4. GC Sweep — 垃圾回收 (M3)

GC mark 阶段结束后，LOS 需要识别并回收死亡的大对象 region。整个 sweep 分为三步：

### 4.1 AssembleLOSGarbageCandidates — 准备回收候选

在 GC mark 之前调用，合并 region 列表并清除 live info：

```cpp
// LOSManager.cpp L145-155
void LOSManager::AssembleLOSGarbageCandidates()
{
    // recent → old (合并到 old 列表统一扫描)
    losOldLargeRegionList.MergeRegionList(losRecentLargeRegionList, LARGE_REGION);
    // 清除所有 region 的 liveInfo，为 mark 阶段做准备
    for (region in losOldLargeRegionList) {
        region->ClearLiveInfo();
    }
}
```

### 4.2 CollectLOSGarbage — 回收死亡 region

mark 阶段完成后调用，遍历 old 列表，死亡的 region 回收到 `losFreeMgr`：

```cpp
// LOSManager.cpp L161-218
size_t LOSManager::CollectLOSGarbage()
{
    // 保护 compaction 计划的目标 region 不被误回收
    std::unordered_set<uintptr_t> protectedTargetRegionStarts;
    for (entry in pendingCompactionPlan) {
        protectedTargetRegionStarts.insert(entry.newStart);
    }

    for (region in losOldLargeRegionList) {
        // 受保护的目标 region → 跳过
        if (protectedTargetRegionStarts.contains(region->Start)) {
            region->ResetMarkBit();
            continue;
        }

        if (!region->IsSurvivedObject(0)) {
            // 死亡 → 从列表删除，回收到 losFreeMgr
            if (region->Size > LARGE_OBJECT_RELEASE_THRESHOLD) {
                ReleaseLOSRegion(region);    // 释放物理内存 + 加入 releaseTree
            } else {
                ReclaimLOSRegion(region);    // 仅标记空闲 + 加入 dirtyTree
            }
        } else {
            region->ResetMarkBit();  // 存活 → 重置 mark bit
        }
    }

    // recent 列表：只重置 mark bit (本轮不回收)
    for (region in losRecentLargeRegionList) {
        region->ResetMarkBit();
    }
}
```

### 4.3 ReclaimLOSRegion / ReleaseLOSRegion

这两个函数镜像了 `RegionManager::CollectRegion / ReleaseRegion`，但路由到 `losFreeMgr` 而非 MAIN 的 `freeRegionManager`：

```cpp
// ReclaimLOSRegion: 标记空闲 → 加入 losFreeMgr.dirtyTree
region->InitFreeUnits();
losFreeMgr.AddGarbageUnits(unitIndex, unitCount);

// ReleaseLOSRegion: 标记空闲 + 释放物理内存 → 加入 losFreeMgr.releaseTree
region->InitFreeUnits();
RegionInfo::ReleaseUnits(unitIndex, unitCount);
losFreeMgr.AddReleaseUnits(unitIndex, unitCount);
```

### 4.4 HandleLOSTraceRegions — 处理 trace 阶段 region

trace 结束后，将 trace 缓存中的 region 合并回 recent 列表：

```cpp
void LOSManager::HandleLOSTraceRegions()
{
    losLargeTraceRegions.DeactivateRegionCache();
    losRecentLargeRegionList.MergeRegionList(losLargeTraceRegions, RECENT_LARGE_REGION);
}
```

---

## 5. MAIN→LOS 迁移 (M3)

当 MAIN 中存在大对象时，GC 在 PostTrace 阶段尝试将它们迁移到 LOS，以释放 MAIN 中宝贵的连续区域给 Copy GC 使用。

### 迁移流程 (`WCollector::BuildLargeObjectMigrationRoutes`)

```
PostTrace 阶段:
  1. 获取 RegionManager::migrationCandidateList (在 AssembleMigrationCandidates 中预选)
  2. 遍历每个候选 region:
     a. 若 liveByteCount == 0 → 死亡，直接 CollectRegion
     b. 尝试 LOSManager::AllocLarge(objSize) 分配 LOS 目标空间
        - 成功 → PrepareForwardableLargeRegion(toAddr)，加入 fromRegionList
        - 失败 → 标记为 UNMOVABLE_FROM_REGION，留在原地
  3. 若有迁移发生，重建 ghostFromRegionList
```

```cpp
// WCollector.cpp L448-512
void WCollector::BuildLargeObjectMigrationRoutes()
{
    for (RegionInfo* region : candidates) {
        if (region->GetLiveByteCount() == 0) {
            // 死亡 → 回收
            rm.CollectRegion(region);
            continue;
        }

        uintptr_t toAddr = losMgr->AllocLarge(objSize);
        if (toAddr == 0) {
            // LOS 满 → 标记不可移动
            region->SetRegionType(UNMOVABLE_FROM_REGION);
            continue;
        }

        // 设置 ghost route: old region → new LOS address
        fromList.PrependRegion(region, FROM_REGION);
        region->PrepareForwardableLargeRegion(toAddr);
    }
}
```

实际的数据搬运发生在 `ForwardFromSpace` 阶段，通过标准的 forwarding barrier 完成。

---

## 6. LOS Sliding Compaction (M4a)

当 LOS 碎片化严重时，通过滑动压缩将存活对象向高地址端紧密排列。

### 6.1 触发条件 (`NeedsCompaction`)

```cpp
bool LOSManager::NeedsCompaction() const
{
    // 条件 1: 碎片率 > 30% (LOS_COMPACTION_FRAG_THRESHOLD)
    if (GetFragmentationRatio() > 0.3) return true;
    // 条件 2: 自上次压缩以来有分配失败
    if (losAllocFailCount > lastCompactAllocFailCount) return true;
    return false;
}
```

碎片率计算: `freeUnits / totalActiveUnits`

### 6.2 CompactLOS 七阶段流程

```
Phase 1: Collect — 分离可移动 vs 固定 region
Phase 2: Compute — 从 losEnd 向低地址计算目标地址
Phase 3: Build — 构建 migrationMap (old→new)
Phase 4: Reference update — 通过 route-based forwarding
Phase 5: Move — memmove_s 搬运数据 + 重建 RegionInfo
Phase 5b: Rebuild lists — 重建 region 列表
Phase 6: Rebuild FreeRegionManager — 重建空闲 region 树
Phase 7: Return RESERVE — 归还借用空间
```

#### Phase 1: 收集与分类

```cpp
// 遍历 old + recent 列表
for (region in allLists) {
    if (region->rawPointerCount > 0) {
        pinnedRegions.push_back(region);         // 有原始指针 → 固定不动
        highestPinnedEnd = max(highestPinnedEnd, region->End);
    } else {
        movableRegions.push_back(region);        // 可移动
    }
}
// 按地址降序排序 (Strategy A: 从高向低分配目标)
sort(movableRegions, by address descending);
```

#### Phase 2: 计算目标地址

```cpp
uintptr_t cursor = losEnd;  // 从最高地址开始
for (region in movableRegions) {
    uintptr_t newStart = cursor - region->Size;
    if (newStart < highestPinnedEnd) break;  // 不越过固定对象屏障 (Strategy A)
    if (newStart != region->Start) {
        compactPlan.push_back({region, oldStart, newStart, size, unitCount});
    }
    cursor = newStart;
}
```

#### Phase 5: 数据搬运

```cpp
// 先清空 region 列表
losOldLargeRegionList.ClearList();
losRecentLargeRegionList.ClearList();

for (entry in compactPlan) {
    // 向高地址滑动 → memmove 处理重叠
    memmove_s(entry.newStart, entry.regionSize, entry.oldStart, entry.regionSize);
    entry.region->InitFreeUnits();  // 清除旧 metadata
    RegionInfo::InitRegionAt(entry.newStart, entry.unitCount, LARGE_SIZED_UNITS); // 新 metadata
}
```

#### Phase 6: 重建 FreeRegionManager

```cpp
void LOSManager::RebuildFreeRegionManager(const vector<RegionInfo*>& allLiveRegions)
{
    losFreeMgr.ClearTrees();  // 清空 dirtyTree + releaseTree

    // 遍历排序后的 live regions，找出间隙并加入 dirtyTree
    uintptr_t gapStart = losInactiveZone;
    for (region in allLiveRegions) {
        if (region->Start > gapStart) {
            // [gapStart, region->Start) 是空隙
            gapRegion = InitRegionAt(gapStart, gapUnits);
            gapRegion->InitFreeUnits();
            losFreeMgr.AddGarbageUnits(gapUnitIdx, gapUnits);
        }
        gapStart = region->End;
    }
}
```

#### Phase 7: 归还 RESERVE 借用

```cpp
if (reserveManager != nullptr) {
    uintptr_t newLosWatermark = min(lowestLiveStart, losStart);
    shrunkBytes = reserveManager->ReclaimLOSBorrowed(newLosWatermark);
}
```

---

## 7. Compaction 引用更新

CompactLOS 在 STW (Stop-The-World) 期间执行 memmove 后，所有堆上和根中的引用都需要更新。

### UpdateCompactedLOSReferences

```cpp
// WCollector.cpp L568-600+
void WCollector::UpdateCompactedLOSReferences(LOSManager& losMgr, RegionManager& rm)
{
    const auto& migMap = losMgr.GetMigrationMap();  // old addr → new addr

    // A. 扫描所有堆对象 (MAIN + LOS) 的引用字段
    auto updateRefField = [&migMap](RefField<>& field) {
        BaseObject* target = field.GetTargetObject();
        auto it = migMap.find(reinterpret_cast<uintptr_t>(target));
        if (it != migMap.end()) {
            field.SetTargetObject(reinterpret_cast<BaseObject*>(it->second));
        }
    };

    // B. 扫描所有 GC roots → 同样替换
    auto updateRoot = [&migMap](ObjectRef& ref) {
        auto it = migMap.find(reinterpret_cast<uintptr_t>(ref.object));
        if (it != migMap.end()) {
            ref.object = reinterpret_cast<BaseObject*>(it->second);
        }
    };
}
```

调用时机: `CompactLOSOnOOMIfNeeded` STW 内部:
```
STW("OOM LOS compact") {
    losMgr.CompactLOS();              // memmove + build migrationMap
    UpdateCompactedLOSReferences();   // 遍历堆+根更新引用
    losMgr.ClearMigrationMap();
}
```

---

## 8. Async Compaction 流水线

LOS 支持两种 compaction 模式：

### 模式 1: OOM STW Compaction (同步)

OOM GC PostTrace 阶段，STW 中执行 CompactLOS + 引用更新。

### 模式 2: Async Plan+Forward+Finalize (跨 GC 周期)

**阶段 A — BuildCompactionPlan**: 只计算 plan，不搬运数据  
**阶段 B — InstallPendingCompactionForForwarding**: 安装 ghost-from route  
**阶段 C — FinalizePendingCompaction**: 回收已 forward 的旧 region  

依赖处理: 如果 entry A 目标地址覆盖 entry B 源地址，则先安装 B，通过递归 DFS + cycle detection 实现。

### Compaction 冻结控制

| 标志 | 含义 |
|------|------|
| `isCompacting` | 压缩进行中 → AllocLarge 返回 0 |
| `compactionRequested` | 分配路径请求压缩 → 下次 GC 检查 |
| `losCompactionFreeze` | 冻结窗口 → 只读/回收，不接受新分配 |

---

## 9. RESERVE 借用与归还 (M5)

LOS 借用链: `AllocFromInactiveZone → GetEffectiveLOSStart() → reserveManager->GetLOSWatermark()`

| 操作 | API | 方向 |
|------|-----|------|
| 借用 | `ReserveManager::BorrowForLOS(size)` | losWatermark 从 reserveEnd 向低地址移动 |
| 归还 | `ReserveManager::ReclaimLOSBorrowed(newWatermark)` | losWatermark 向高地址收缩 |

迟滞: `LOS_RESERVE_RETURN_THRESHOLD = 0.4`，LOS 空闲率 > 40% 时才归还。

---

## 10. GC 主流程中的 LOS 集成点

```
DoGarbageCollection()
├── TraceHeap()
├── PostTrace()
│   ├── HandleLOSTraceRegions()           → trace cache → recent
│   ├── AssembleLOSGarbageCandidates()    → recent → old
│   ├── CollectLOSGarbage()               → sweep dead → losFreeMgr
│   ├── CompactLOSIfNeeded()              → OOM STW 压缩
│   └── BuildLargeObjectMigrationRoutes() → MAIN→LOS 迁移
├── ForwardFromSpace()
├── FinalizeLOSCompactionIfNeeded()       → async compact finalize
├── ShrinkInactiveZone() (OOM)
├── CollectSmallSpace()
└── CleanupMigrationCandidates()          → 清理未迁移候选
```

---

## 11. 关键数据结构

| 结构 | 类型 | 职责 |
|------|------|------|
| `losOldLargeRegionList` | RegionList | 上轮 GC 存活 region (sweep 候选) |
| `losRecentLargeRegionList` | RegionList | 本轮新分配 region |
| `losLargeTraceRegions` | RegionCache | trace 阶段分配的 region |
| `losFreeMgr` | FreeRegionManager | 独立空闲 region 树 (baseUnit=mainUnitCount) |
| `pendingCompactionPlan` | vector\<CompactEntry\> | 异步 compact 计划 |
| `migrationMap` | unordered_map | old→new 对象地址映射 |

---

## 12. 统计与诊断

### 分配统计 (原子)

| 计数器 | 含义 |
|--------|------|
| `losAllocCount` | LOS 成功分配次数 |
| `losAllocFailCount` | 分配失败次数 |
| `losFallbackCount` | MAIN 回退次数 |
| `losReserveBorrowCount` | RESERVE 借用次数 |

### Compaction 统计

| 计数器 | 含义 |
|--------|------|
| `losCompactCount` | 压缩执行次数 |
| `losCompactMovedBytes` | 搬运字节数 |
| `losCompactFreedBytes` | 释放字节数 |
| `losCompactShrunkBytes` | 归还 RESERVE 字节数 |

### 日志关键字

```bash
grep 'LOS alloc large'             # 分配事件
grep 'LOS reclaim'                 # GC sweep 回收
grep 'LOS CompactLOS'              # 压缩事件
grep 'LOS migration'               # MAIN→LOS 迁移
grep 'AllocLarge MAIN fallback'    # LOS 满回退 MAIN
grep 'BorrowForLOS'                # RESERVE 借用
```
