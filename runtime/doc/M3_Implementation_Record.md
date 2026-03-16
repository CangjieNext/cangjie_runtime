# M3 实现记录：LOS GC 回收 + MAIN→LOS 大对象迁移

**实现日期**：2026年2月19日

---

## 1. 实现目标

M3 阶段目标：在现有 GC 策略基础上，实现 LOS 空间的独立回收和 MAIN→LOS 大对象迁移。

- LOS 空间的死亡对象直接回收到 `losFreeMgr`（LOS 自己的 FreeRegionManager），而非 MAIN 的 freeRegionManager
- GC 标记阶段感知 LOS region（AssembleGarbageCandidates、PrepareTrace、HandleTraceRegions）
- 回收后将 MAIN 空间中存活的大对象迁移到 LOS 空间
- 迁移使用两阶段设计 + ghost route 机制，引用通过 ForwardBarrier 自动转发

### 实现范围

| 步骤 | 状态 | 说明 |
|------|------|------|
| LOS 垃圾候选装配 | ✅ 已实现 | `AssembleLOSGarbageCandidates()` |
| LOS trace region 管理 | ✅ 已实现 | `HandleLOSTraceRegions()` + `PrepareTrace` 集成 |
| LOS 死亡对象清扫 | ✅ 已实现 | `CollectLOSGarbage()` → `losFreeMgr` |
| 迁移候选预选（Trace 前） | ✅ 已实现 | `AssembleMigrationCandidates()` — 两阶段设计 |
| 迁移路由构建（PostTrace 后） | ✅ 已实现 | `BuildLargeObjectMigrationRoutes()` — ghost route 机制 |
| 未迁移候选清理 | ✅ 已实现 | `CleanupMigrationCandidates()` |
| isMarked 残留 Bug 修复 | ✅ 已实现 | `ResetMarkBit()` 在 `AssembleMigrationCandidates` 中调用 |

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

// M3 统计
size_t GetGarbageCollectedBytes() const;
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
// migrationMap 由 M4a CompactLOS 使用（unordered_map<uintptr_t, uintptr_t>）
```

### 2.2 RegionManager.h / RegionManager.cpp

**新增 M3 迁移相关**：

```cpp
// M3 两阶段迁移：Trace 前选候选
void AssembleMigrationCandidates();

// M3 GC 结束后：未迁移候选归还 oldLargeRegionList
void CleanupMigrationCandidates();

// 迁移候选列表
RegionList migrationCandidateList;
```

`AssembleMigrationCandidates()` 关键逻辑：
1. 遍历 `oldLargeRegionList`，过滤 rawPointer 和 RESERVE 借用 region
2. 移入 `migrationCandidateList`，类型设为 `FROM_REGION`
3. 调用 `ClearLiveInfo()` + `ResetMarkBit()` — 确保 trace 重新标记并正确计算 liveByteCount

> **isMarked 残留 Bug**：大对象的标记使用 `isMarked` 标志而非 bitmap。`ClearLiveInfo()` 不清除 `isMarked`。从 `oldLargeRegionList` 移出的候选不再被 `CollectLargeGarbage` 的 `ResetMarkBit()` 覆盖，导致 `isMarked` 残留为 1 → trace 跳过 `AddLiveByteCount` → 活对象误判为死对象。修复：在移入候选列表时立即调用 `ResetMarkBit()`。

---

### 2.3 RegionSpace.h

**修改 `AssembleGarbageCandidates()`**：

增加迁移候选预选和 LOS 垃圾候选装配：

```cpp
void AssembleGarbageCandidates(bool collectAll = false)
{
    regionManager.AssembleMigrationCandidates();    // M3: 迁移候选预选（必须在 Trace 前）
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

### 2.4 RegionManager.h

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

### 2.5 WCollector.h / WCollector.cpp

**新增 `BuildLargeObjectMigrationRoutes()` 方法（WCollector.h 声明）**：

```cpp
// M3: Build forwarding routes to migrate surviving MAIN large objects to LOS
void BuildLargeObjectMigrationRoutes();
```

**实现（WCollector.cpp）**：

迁移使用 ghost route 机制（`PrepareForwardableLargeRegion`），引用通过 ForwardBarrier 自动转发：

```cpp
void WCollector::BuildLargeObjectMigrationRoutes()
{
    // 遍历 migrationCandidateList 中的候选
    for (RegionInfo* region : candidates) {
        if (region->GetLiveByteCount() == 0) {
            // 死对象：直接回收
            migrationList.DeleteRegion(region);
            rm.CollectRegion(region);
            continue;
        }
        // 尝试在 LOS 分配空间（含 RESERVE 借用重试）
        uintptr_t toAddr = losMgr->AllocLarge(objSize);
        if (toAddr == 0) {
            // LOS+RESERVE 均满：标记 UNMOVABLE_FROM_REGION，ForwardBarrier 仅 untag
            region->SetRegionType(RegionInfo::RegionType::UNMOVABLE_FROM_REGION);
            continue;
        }
        // 成功：设置 ghost route，移入 fromRegionList
        migrationList.DeleteRegion(region);
        fromList.PrependRegion(region, RegionInfo::RegionType::FROM_REGION);
        region->PrepareForwardableLargeRegion(toAddr);
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
  → CollectPinnedGarbage()
  → RefineFromSpace()
  → PrepareForwardTable()         // 清理旧 ghost route
  → BuildLargeObjectMigrationRoutes()  // ← M3：构建迁移路由（必须在 PrepareForwardTable 之后）
```

> **注**：`BuildLargeObjectMigrationRoutes` 必须在 `PrepareForwardTable` 之后执行，因为后者会清理上一轮的 ghost route（`DispelGhostFromRegion`）。

---

## 3. 关键设计决策

### 3.1 LOS 回收路由

LOS 死亡 region 回收到 `losFreeMgr`，而不是 MAIN 的 `freeRegionManager`。这保证 LOS 空间回收后可以复用给后续 LOS 分配，不会被 MAIN 占用。

采用与 `RegionManager` 一致的两级回收策略：
- **≤ 4MB**：`ReclaimLOSRegion()` → `InitFreeUnits()` + `losFreeMgr.AddGarbageUnits()` 进入 dirtyTree
- **> 4MB**：`ReleaseLOSRegion()` → `InitFreeUnits()` + `ReleaseUnits()`（物理释放）+ `losFreeMgr.AddReleaseUnits()` 进入 releasedTree

阈值常量 `LARGE_OBJECT_RELEASE_THRESHOLD = 4096 * KB = 4MB`，与 MAIN 空间一致。

### 3.2 迁移策略（两阶段设计）

**阶段一（Trace 前）：`AssembleMigrationCandidates()`**

在 `AssembleGarbageCandidates()` 最前面执行：
1. 遍历 MAIN 的 `oldLargeRegionList`
2. 跳过 pinned 对象（`rawPointerCount > 0`）和 RESERVE 借用 region
3. 移入 `migrationCandidateList`，类型设为 `FROM_REGION`
4. 调用 `ClearLiveInfo()` + `ResetMarkBit()` — 确保 trace 重新标记
5. 候选的 `IsFromRegion() == true` → Enum/Trace 阶段引用会被 tagged

**阶段二（PostTrace 后）：`BuildLargeObjectMigrationRoutes()`**

在 `PrepareForwardTable()` 之后执行（需先清理旧 ghost route）：
1. **死对象**（`liveByteCount == 0`）→ 直接 `CollectRegion` 回收
2. **活对象 + LOS 可分配** → `AllocLarge` + `PrepareForwardableLargeRegion` 设置 ghost route，移入 `fromRegionList`
3. **活对象 + LOS+RESERVE 均满** → 设置 `UNMOVABLE_FROM_REGION`，ForwardBarrier 仅 untag 不转发

**GC 结束后：`CleanupMigrationCandidates()`**
- 将未迁移的候选（`UNMOVABLE_FROM_REGION`）merge 回 `oldLargeRegionList`，类型恢复为 `LARGE_REGION`

### 3.3 引用更新

迁移使用 ghost route 机制，引用通过 GC 的 ForwardBarrier 自动转发：

| 引用目标状态 | `IsFromRegion()` | `IsUnmovableFromRegion()` | ForwardBarrier 行为 |
|-------------|-------------------|---------------------------|---------------------|
| 已迁移到 LOS | true | false | → `TryForwardRefField` → 通过 ghost route 转发到 LOS 新地址 |
| LOS 满未迁移 | false | true | → `TryUntagRefField` → 去掉 tag bit，返回原地址 |
| 未参与迁移 | false | false | → 引用未 tagged，快速路径直接返回 |

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
  │    ├─ AssembleGarbageCandidates()
  │    │    ├─ AssembleMigrationCandidates()     ← M3: 选迁移候选（FROM_REGION + ResetMarkBit）
  │    │    ├─ AssembleSmallGarbageCandidates()
  │    │    ├─ AssemblePinnedGarbageCandidates()
  │    │    ├─ AssembleLargeGarbageCandidates()
  │    │    └─ AssembleLOSGarbageCandidates()    ← M3: LOS recent→old
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
  │    ├─ CollectPinnedGarbage()
  │    ├─ RefineFromSpace()
  │    ├─ PrepareForwardTable()             // 清理旧 ghost route
  │    └─ BuildLargeObjectMigrationRoutes() ← M3: 构建 ghost route 迁移路由
  │
  ├─ Preforward()
  ├─ ForwardFromSpace()
  ├─ ...
  ├─ CleanupMigrationCandidates()          ← M3: 未迁移候选归还 oldLargeRegionList
  ├─ FlipTagID()
  └─ CollectSmallSpace()
```

---

## 5. 文件修改清单

| 文件 | 修改类型 | 说明 |
|------|----------|------|
| `src/Heap/Allocator/LOSManager.h` | 修改 | 新增 M3 GC 回收方法声明、统计字段 |
| `src/Heap/Allocator/LOSManager.cpp` | 修改 | 实现 3 个 M3 GC 方法 + 2 个私有回收方法 |
| `src/Heap/Allocator/RegionSpace.h` | 修改 | `AssembleGarbageCandidates` / `CollectLargeGarbage` / `PrepareTrace` 集成 LOS |
| `src/Heap/Allocator/RegionManager.h` | 修改 | `HandleTraceRegions` / `PrepareTrace` 集成 LOS + 新增 `AssembleMigrationCandidates` / `CleanupMigrationCandidates` / `migrationCandidateList` |
| `src/Heap/Allocator/RegionManager.cpp` | 修改 | 实现 `AssembleMigrationCandidates`（含 `ResetMarkBit` 修复） + `CleanupMigrationCandidates` |
| `src/Heap/WCollector/WCollector.h` | 修改 | 新增 `BuildLargeObjectMigrationRoutes()` 声明 |
| `src/Heap/WCollector/WCollector.cpp` | 修改 | 新增 `BuildLargeObjectMigrationRoutes()` 实现 + `PostTrace` 调用链 |

---

## 6. 编译验证

```bash
python3 build.py build   # macOS aarch64 Release — 通过，无错误无警告
```
