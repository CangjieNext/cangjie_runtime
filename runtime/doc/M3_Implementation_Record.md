# M3 实现记录：LOS GC 回收 + MAIN→LOS 大对象迁移

**实现日期**：2026年2月19日

---

## 1. 实现目标

M3 阶段目标：在现有 GC 策略基础上，实现 LOS 空间的独立回收和 MAIN→LOS 大对象迁移。

- LOS 空间的死亡对象直接回收到 `losFreeMgr`（LOS 自己的 FreeRegionManager），而非 MAIN 的 freeRegionManager
- GC 标记阶段感知 LOS region（AssembleGarbageCandidates、PrepareTrace、HandleTraceRegions）
- 回收后将 MAIN 空间中存活的大对象迁移到 LOS 空间
- 迁移后遍历全堆更新引用（STW 下直接写方式）

### 实现范围

| 步骤 | 状态 | 说明 |
|------|------|------|
| LOS 垃圾候选装配 | ✅ 已实现 | `AssembleLOSGarbageCandidates()` |
| LOS trace region 管理 | ✅ 已实现 | `HandleLOSTraceRegions()` + `PrepareTrace` 集成 |
| LOS 死亡对象清扫 | ✅ 已实现 | `CollectLOSGarbage()` → `losFreeMgr` |
| MAIN→LOS 迁移 | ✅ 已实现 | `MigrateLargeObjectsFromMain()` |
| 引用更新 | ✅ 已实现 | `UpdateMigratedReferences()` |
| WCollector 集成 | ✅ 已实现 | `MigrateLargeObjectsToLOS()` |

---

## 2. 修改文件

### 2.1 LOSManager.h / LOSManager.cpp

**新增 M3 公共接口**：

```cpp
// GC 准备阶段：将 recent→old，清除存活信息
void AssembleLOSGarbageCandidates();

// GC trace 后：deactivate trace cache，合并回 recent
void HandleLOSTraceRegions();

// GC 回收：遍历 losOldLargeRegionList 清扫死亡 region 到 losFreeMgr
size_t CollectLOSGarbage();

// MAIN→LOS 迁移：将 MAIN oldLargeRegionList 中存活大对象拷贝到 LOS
size_t MigrateLargeObjectsFromMain(RegionList& mainOldList, RegionList& mainGarbageList);

// 更新全堆引用：扫描堆对象 + 静态根
void UpdateMigratedReferences();

// 迁移状态管理
void ClearMigrationState();
bool HasPendingMigrations() const;

// M3 统计
size_t GetGarbageCollectedBytes() const;
size_t GetMigrateInCount() const;
size_t GetMigrateInBytes() const;
```

**新增 M3 私有方法**：

```cpp
// 回收 region 到 losFreeMgr（≤4MB 走 AddGarbageUnits）
size_t ReclaimLOSRegion(RegionInfo* region);

// 释放 region 到 losFreeMgr（>4MB 走 ReleaseUnits + AddReleaseUnits）
size_t ReleaseLOSRegion(RegionInfo* region);
```

**新增 M3 成员**：

```cpp
size_t losGarbageCollectedBytes = 0;     // 当轮 GC 回收字节数
size_t losMigrateInCount = 0;            // 累计迁入对象数
size_t losMigrateInBytes = 0;            // 累计迁入字节数
std::vector<std::pair<uintptr_t, uintptr_t>> migrationMap;  // old→new 地址映射
```

---

### 2.2 RegionSpace.h

**修改 `AssembleGarbageCandidates()`**：

增加 LOS 垃圾候选装配：

```cpp
void AssembleGarbageCandidates(bool collectAll = false)
{
    regionManager.AssembleSmallGarbageCandidates();
    regionManager.AssemblePinnedGarbageCandidates(collectAll);
    regionManager.AssembleLargeGarbageCandidates();
    // M3: Assemble LOS large garbage candidates
    LOSManager* losMgr = regionManager.GetLOSManager();
    if (losMgr != nullptr) {
        losMgr->AssembleLOSGarbageCandidates();
    }
}
```

**修改 `CollectLargeGarbage()`**：

增加 LOS 死亡 region 清扫，返回值包含 MAIN + LOS 两部分垃圾：

```cpp
size_t CollectLargeGarbage()
{
    size_t mainGarbage = regionManager.CollectLargeGarbage();
    // M3: Collect LOS garbage — sweep dead LOS regions to losFreeMgr
    LOSManager* losMgr = regionManager.GetLOSManager();
    if (losMgr != nullptr) {
        mainGarbage += losMgr->CollectLOSGarbage();
    }
    return mainGarbage;
}
```

**修改 `PrepareTrace()`**：

增加 LOS trace cache 激活（由 `RegionManager::PrepareTrace` 内部处理）。

---

### 2.3 RegionManager.h

**修改 `HandleTraceRegions()`**：

增加 LOS trace region 处理：

```cpp
void HandleTraceRegions()
{
    // ... 原有 MAIN full/large trace regions 处理 ...

    // M3: Handle LOS trace regions
    if (losManager != nullptr) {
        losManager->HandleLOSTraceRegions();
    }

    // ... 原有清理逻辑 ...
}
```

**修改 `PrepareTrace()`**：

增加 LOS trace cache 激活：

```cpp
void PrepareTrace()
{
    fullTraceRegions.ActivateRegionCache();
    largeTraceRegions.ActivateRegionCache();

    // M3: Activate LOS trace cache
    if (losManager != nullptr) {
        losManager->GetLargeTraceRegions().ActivateRegionCache();
    }
}
```

**新增 accessor**：

```cpp
// M3: expose region lists for migration
RegionList& GetOldLargeRegionList() { return oldLargeRegionList; }
RegionList& GetGarbageRegionList() { return garbageRegionList; }
```

---

### 2.4 WCollector.h / WCollector.cpp

**新增 `MigrateLargeObjectsToLOS()` 方法（WCollector.h 声明）**：

```cpp
// M3: Migrate surviving MAIN large objects into LOS after sweep
void MigrateLargeObjectsToLOS();
```

**实现（WCollector.cpp）**：

```cpp
void WCollector::MigrateLargeObjectsToLOS()
{
    MRT_PHASE_TIMER("MigrateLargeObjectsToLOS");
    RegionSpace& space = reinterpret_cast<RegionSpace&>(theAllocator);
    LOSManager* losMgr = space.GetRegionManager().GetLOSManager();
    if (losMgr == nullptr) {
        return;
    }
    RegionManager& rm = space.GetRegionManager();
    size_t migratedCount = losMgr->MigrateLargeObjectsFromMain(
        rm.GetOldLargeRegionList(), rm.GetGarbageRegionList());
    if (migratedCount > 0) {
        losMgr->UpdateMigratedReferences();
        losMgr->ClearMigrationState();
    }
}
```

**修改 `PostTrace()` 调用链**：

```
PostTrace()
  → HandleTraceRegions()          // 含 LOS trace region 处理
  → ClearWeakRefBuffer()
  → ClearBuffer()
  → PrepareCycleRef()
  → CollectLargeGarbage()         // MAIN + LOS 死亡 region 清扫
  → MigrateLargeObjectsToLOS()    // ← M3 新增：MAIN→LOS 迁移 + 引用更新
  → CollectPinnedGarbage()
  → RefineFromSpace()
  → PrepareForwardTable()
```

---

## 3. 关键设计决策

### 3.1 LOS 回收路由

LOS 死亡 region 回收到 `losFreeMgr`，而不是 MAIN 的 `freeRegionManager`。这保证 LOS 空间回收后可以复用给后续 LOS 分配，不会被 MAIN 占用。

采用与 `RegionManager` 一致的两级回收策略：
- **≤ 4MB**：`ReclaimLOSRegion()` → `InitFreeUnits()` + `losFreeMgr.AddGarbageUnits()` 进入 dirtyTree
- **> 4MB**：`ReleaseLOSRegion()` → `InitFreeUnits()` + `ReleaseUnits()`（物理释放）+ `losFreeMgr.AddReleaseUnits()` 进入 releasedTree

阈值常量 `LARGE_OBJECT_RELEASE_THRESHOLD = 4096 * KB = 4MB`，与 MAIN 空间一致。

### 3.2 迁移策略

迁移在 STW（PostTrace）阶段执行，位于 `CollectLargeGarbage()` 之后、`CollectPinnedGarbage()` 之前。

迁移流程：
1. 遍历 MAIN 的 `oldLargeRegionList`
2. 跳过 pinned 对象（`rawPointerCount > 0`）
3. 在 LOS 中分配等大空间（`LOSManager::AllocLarge`）
4. `memmove_s` 拷贝对象数据
5. 记录 old→new 地址到 `migrationMap`
6. 将 MAIN 旧 region 移入 `garbageRegionList` 等待回收
7. LOS 分配失败时提前终止（本轮迁移部分完成）

### 3.3 引用更新

使用简单向量 `migrationMap` 而非复杂的 forwarding 机制：

- STW 阶段无并发修改，直接写安全
- 每轮迁移对象数量有限（受 LOS 容量约束），线性扫描 map 可接受
- 扫描范围：
  1. **堆对象**：`Heap::GetHeap().ForEachObj()` + `obj->ForEachRefField()`
  2. **静态根**：`Heap::GetHeap().VisitStaticRoots()`
- 更新方式：`RefField::SetTargetObject()` 直接替换目标地址

### 3.4 GC 标记集成

LOS region 参与 GC 标记流程，与 MAIN 大对象使用相同的 region 级 `isMarked` 标记：

- **标记前**：`AssembleLOSGarbageCandidates()` 将 recent→old，清存活信息
- **标记中**：`PrepareTrace()` 激活 LOS trace cache
- **标记后**：`HandleLOSTraceRegions()` deactivate trace cache，合并回 recent
- **回收时**：`CollectLOSGarbage()` 检查 `IsSurvivedObject(0)` 判断存活

---

## 4. GC 完整流程（含 M3 修改点）

```
DoGarbageCollection()
  │
  ├─ TraceHeap()
  │    ├─ AssembleGarbageCandidates()       ← M3: + AssembleLOSGarbageCandidates()
  │    ├─ DoEnumeration()
  │    ├─ PrepareTrace()                    ← M3: + LOS trace cache 激活
  │    ├─ DoTracing()
  │    └─ ProcessFinalizers()
  │
  ├─ PostTrace()
  │    ├─ HandleTraceRegions()              ← M3: + HandleLOSTraceRegions()
  │    ├─ ClearWeakRefBuffer()
  │    ├─ ClearBuffer()
  │    ├─ PrepareCycleRef()
  │    ├─ CollectLargeGarbage()             ← M3: + CollectLOSGarbage() → losFreeMgr
  │    ├─ MigrateLargeObjectsToLOS()        ← M3 新增
  │    │    ├─ MigrateLargeObjectsFromMain()   拷贝 MAIN 大对象到 LOS
  │    │    ├─ UpdateMigratedReferences()      全堆引用更新
  │    │    └─ ClearMigrationState()           清除映射表
  │    ├─ CollectPinnedGarbage()
  │    ├─ RefineFromSpace()
  │    └─ PrepareForwardTable()
  │
  ├─ Preforward()
  ├─ ForwardFromSpace()
  ├─ FlipTagID()
  └─ CollectSmallSpace()
```

---

## 5. 文件修改清单

| 文件 | 修改类型 | 说明 |
|------|----------|------|
| `src/Heap/Allocator/LOSManager.h` | 修改 | 新增 M3 GC 回收 + 迁移方法声明、统计字段、migrationMap |
| `src/Heap/Allocator/LOSManager.cpp` | 修改 | 实现 6 个 M3 方法 + 2 个私有回收方法 |
| `src/Heap/Allocator/RegionSpace.h` | 修改 | `AssembleGarbageCandidates` / `CollectLargeGarbage` / `PrepareTrace` 集成 LOS |
| `src/Heap/Allocator/RegionManager.h` | 修改 | `HandleTraceRegions` / `PrepareTrace` 集成 LOS + getter 方法 |
| `src/Heap/WCollector/WCollector.h` | 修改 | 新增 `MigrateLargeObjectsToLOS()` 声明 |
| `src/Heap/WCollector/WCollector.cpp` | 修改 | 新增 `MigrateLargeObjectsToLOS()` 实现 + `PostTrace` 调用链 + `LOSManager.h` include |

---

## 6. 编译验证

```bash
python3 build.py build   # macOS aarch64 Release — 通过，无错误无警告
```
