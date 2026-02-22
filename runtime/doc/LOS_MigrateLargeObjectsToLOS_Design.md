# MAIN→LOS 大对象并发迁移设计

> 日期: 2026-02-22  
> 里程碑: M3  
> 状态: **已实现，编译通过，待运行时测试**

## 1. 问题背景

当前大对象（large object）分配在 MAIN 空间的 `oldLargeRegionList` 中。大对象占用多个连续 unit，不参与 copying GC 的 forward 流程，只做标记回收。这导致：

1. **MAIN 空间碎片化**: 大对象区域无法被小对象复用，造成 MAIN 空间空洞
2. **空间利用率低**: LOS（Large Object Space）专为大对象设计，有独立的 `FreeRegionManager`，更适合管理大对象的分配与回收
3. **无法参与压缩**: MAIN 中的大对象不参与 forwarding，长期驻留加剧碎片

**目标**: 每次 GC 后，**并发**将存活的 MAIN 大对象迁移到 LOS 中，复用现有的 ghost forwarding 机制完成引用修正，无需 STW。

## 2. 核心机制：Ghost Forwarding 生命周期

迁移复用 WCollector 已有的 ghost forwarding 机制，其生命周期与小对象 forwarding 完全一致：

### 2.1 小对象 Ghost Forwarding 回顾

| 阶段 | 操作 |
|------|------|
| **第 N 轮 PrepareFromRegionList** | `DispelGhostFromRegion` 清除上一轮 ghost → `PrepareForwardableRegion` 设置本轮 ghost (routeState=FORWARDABLE, inGhostFromRegion=1) → `CopyListTo(ghostFromRegionList)` |
| **第 N 轮 Preforward** | Barrier 将 mutator 的读操作重定向到新地址；修正特定根 (ConcurrencyModel / Finalizer / Export / DiscoveredExtern / ResurrectedExport) |
| **第 N 轮 ForwardFromSpace** | GC 线程：`ForwardObjectExclusive` → `CopyObject`（实际拷贝）→ `CollectRegion`（回收旧物理空间，但 ghost 元数据保留）|
| **GC 间隔期** | Ghost 持续存在，读屏障 (ForwardBarrier) 通过 `GetGhostFromRegionAt` → `GetRoute` 将旧地址重定向到新地址 |
| **第 N+1 轮 Enum/Trace** | 遇到旧 tagged pointer → `FindLatestVersion` 经由 ghost 找到新地址 → CAS 更新 ref field |
| **第 N+1 轮 PrepareFromRegionList** | `DispelGhostFromRegion` 清除第 N 轮设置的 ghost，生命周期结束 |

**关键约束**: Ghost **必须跨越两轮 GC**。Preforward 只修正特定根类型，**不扫描堆中的 ref field**。堆内引用的修正依赖：
- GC 间隔期：读屏障惰性重定向
- 下一轮 GC 的 Enum/Trace：主动扫描 + CAS 更新

### 2.2 大对象迁移的 Ghost Forwarding

大对象迁移复用同一套机制，唯一区别是：
- 小对象使用 `PrepareForwardableRegion`（routeState = **FORWARDABLE**，目标地址在 forward 时由 `RouteObject` 动态分配）
- 大对象迁移使用 `PrepareForwardableLargeRegion`（routeState = **ROUTED**，目标地址在构建 route 时已确定）

## 3. 迁移流程设计

### 3.1 总体流程

```
PostTrace                           第 N 轮 GC
  ├── CollectLargeGarbage            回收死亡大对象
  ├── ...
  ├── RefineFromSpace
  ├── fwdTable.PrepareForwardTable()
  └── BuildLargeObjectMigrationRoutes()  ← 新增：构建迁移路由
                                          (在 PrepareForwardTable 之后)
Preforward                          第 N 轮 GC  
  └── 修正特定根 + 启用 barrier 重定向

ForwardFromSpace                    第 N 轮 GC
  └── 处理 fromRegionList 中的迁移大对象 region
      → ForwardObjectExclusive → CopyObject（实际拷贝到 LOS）
      → CollectRegion（回收旧 MAIN 空间，ghost 元数据保留）

  [GC 间隔期: ghost 持续存在，barrier 重定向]

Enum/Trace                          第 N+1 轮 GC
  └── 遇到旧 tagged ptr → FindLatestVersion → CAS 修正 ref field

PrepareFromRegionList               第 N+1 轮 GC
  └── DispelGhostFromRegion 清除迁移 ghost
```

### 3.2 Step 1: 候选对象筛选（PostTrace 阶段）

在 `PostTrace()` 中 `fwdTable.PrepareForwardTable()` **之后**执行。

**为什么必须在 PrepareForwardTable 之后？**  
`PrepareFromRegionList`（在 PrepareForwardTable 内部调用）会先 `DispelGhostFromRegion` 清除上一轮的 ghost。如果在这之前添加了新的迁移 ghost，会被误清除。

**筛选条件**:
- 来自 `oldLargeRegionList`（存活的 MAIN 大对象）
- 标记为存活（survived trace）
- 非 pinned
- 地址在 MAIN 空间范围内（排除已在 LOS 中的）
- LOS 有足够空间接收

```cpp
void WCollector::BuildLargeObjectMigrationRoutes()
{
    RegionSpace& space = reinterpret_cast<RegionSpace&>(theAllocator);
    RegionManager& rm = space.GetRegionManager();
    LOSManager* losMgr = rm.GetLOSManager();
    if (losMgr == nullptr) {
        return;
    }

    // 遍历 oldLargeRegionList，筛选可迁移候选
    rm.GetOldLargeRegionList().VisitAllRegions([&](RegionInfo* region) {
        // 条件: 存活 + 非 pinned + 在 MAIN 空间
        if (region->GetLiveByteCount() == 0) return;        // 死对象，跳过
        if (region->IsPinnedRegion()) return;                 // pinned，不能移动
        if (!rm.IsInMainSpace(region)) return;                // 已在 LOS，无需迁移
        
        size_t objSize = region->GetRegionAllocatedSize();
        
        // 在 LOS 中分配目标空间
        RegionInfo* losRegion = losMgr->AllocLargeRegion(objSize);
        if (losRegion == nullptr) return;  // LOS 空间不足，跳过
        
        uintptr_t toAddr = losRegion->GetRegionStart();
        
        // 构建 ghost route（不拷贝对象！）
        region->PrepareForwardableLargeRegion(toAddr);
        
        // 添加到 fromRegionList（ForwardFromSpace 处理）
        // 添加到 ghostFromRegionList（barrier 识别）
        rm.AppendToFromRegionList(region);
        rm.AppendToGhostFromRegionList(region);
    });
}
```

### 3.3 Step 2: 路由构建详情

调用 `PrepareForwardableLargeRegion(toAddr)` 完成以下设置：

```
RegionInfo (MAIN 大对象 region)
├── routeState = ROUTED          // 目标地址已确定
├── unitRole0 = LARGE_SIZED_UNITS // 保存原始角色
├── regionEnd0 = regionEnd        // 保存原始 region 边界
├── routeInfo = {toAddr, size}    // 目标 LOS 地址 + 大小
├── nextRegionIdx0 = nextRegionIdx
└── 所有 unit:
    ├── inGhostFromRegion = 1     // 标记为 ghost（barrier 可识别）
    ├── unitRole0 = SUBORDINATE_UNIT (从属 unit)
    └── ownerRegion0 = this       // 指回 region header
```

**此时不进行任何对象拷贝**。对象数据仍在 MAIN 原始位置。

### 3.4 Step 3: ForwardFromSpace 执行实际拷贝

`ForwardFromSpace` 遍历 `fromRegionList`，对每个 region 调用 `ForwardRegion`：

```
ForwardRegion(region)
  → RouteRegion(region)                    // routeState: ROUTED → ROUTING
  → VisitLiveObjectsUntilFalse(visitor)    // 大对象 region 有且只有 1 个对象（offset 0）
    → ForwardObjectImpl(obj, region)
      → TryLockObject
      → ForwardObjectExclusive(obj)
        → RouteObject(obj)                 // 返回目标 LOS 地址
        → CopyObject(from, to, size)       // <<<< 实际内存拷贝发生在这里
        → obj->UnlockObject(FORWARDED)     // 标记完成
  → SetRouteState(FORWARDED)
  → CollectRegion(region)                  // 回收旧 MAIN units，但 ghost 元数据保留
```

**需验证的关键点**:
1. `VisitLiveObjectsUntilFalse` 是否兼容 `LARGE_SIZED_UNITS`（大对象只有 1 个 object，位于 offset 0）
2. `ForwardObjectExclusive` → `CopyObject` 是否能处理大对象 size
3. `CollectRegion` 对大对象 region 的处理逻辑（需要释放 MAIN units 但保留 ghost 元数据）

### 3.5 Step 4: GC 间隔期 — Barrier 重定向

Forward 完成后到下一轮 GC 之间，ghost 持续存在。任何 mutator 访问旧 MAIN 地址时：

```
ForwardBarrier::ReadReference(field)
  → RefField 是 tagged pointer
  → GetGhostFromRegionAt(oldAddr)   // inGhostFromRegion=1 → 返回 ghost region
  → region->GetRoute(obj)           // routeInfo → 返回 LOS 新地址
  → 返回新地址给 mutator
```

**注意**: 此阶段堆中的 ref field 仍可能指向旧 MAIN 地址。这是正常的——只有通过 barrier 读取时才会重定向，ref field 本身不会被主动修正。

### 3.6 Step 5: 下一轮 GC 完成引用修正

第 N+1 轮 GC 的 Enum/Trace 阶段：

```
TraceRefField(obj, field, workStack)
  → RefField 携带旧 tag
  → IsOldPointer(field) == true
  → FindLatestVersion(targetObj)       // 经由 ghost route 找到 LOS 新地址
  → GetAndTryTagRefField(latest)       // 用新 tag 包装
  → field.CompareExchange(old, new)    // CAS 更新 ref field → 指向 LOS 新地址
```

至此，所有堆内引用都已更新为 LOS 新地址。

### 3.7 Step 6: 清除 Ghost

第 N+1 轮 GC 的 `PrepareFromRegionList` 中：

```
ghostFromRegionList.VisitAllGhostRegions([](RegionInfo* region) {
    region->DispelGhostFromRegion();    // 清除 inGhostFromRegion 位
});
```

迁移 ghost region 随小对象 ghost 一起被统一清除。生命周期结束。

## 4. 实际修改的文件

| 文件 | 修改内容 | 状态 |
|------|----------|------|
| `src/Heap/WCollector/WCollector.cpp` | 新增 `BuildLargeObjectMigrationRoutes()` (~90行)，在 `PostTrace()` 的 `fwdTable.PrepareForwardTable()` 之后调用 | ✅ 已实现 |
| `src/Heap/WCollector/WCollector.h` | 声明 `void BuildLargeObjectMigrationRoutes();` (private) | ✅ 已实现 |
| `src/Heap/Allocator/RegionManager.h` | 新增 `GetFromRegionList()` 和 `GetGhostFromRegionList()` 两个访问器方法 | ✅ 已实现 |
| `src/Heap/Allocator/RegionManager.cpp` | **无需修改** — `ForwardRegion`/`CollectRegion`/`RouteRegion` 已兼容 `LARGE_SIZED_UNITS` | ✅ 验证通过 |
| `src/Heap/Allocator/RegionInfo.h` | **无需修改** — `PrepareForwardableLargeRegion` 已存在（LOS compaction 使用），直接复用 | ✅ 验证通过 |
| `src/Heap/Allocator/LOSManager.cpp/h` | **无需修改** — `AllocLarge(size)` 已返回 `uintptr_t` 目标地址 + 自动 `EnlistLargeRegion` | ✅ 验证通过 |

### 4.1 兼容性验证详情

**ForwardRegion CHECK 通过**:
- `CHECK_DETAIL(region->IsFromRegion() || ...)` — 通过 `PrependRegion(region, FROM_REGION)` 设置 region type 满足
- `VisitLiveObjectsUntilFalse` — 有 `IsLargeRegion()` 分支，大对象只有 1 个 object 在 offset 0
- `CollectRegion` — 有 `IsLargeRegion()` 分支，回收旧 MAIN units 并保留 ghost 元数据
- `GetRoute` — 有 `LARGE_SIZED_UNITS` 分支，正确返回 routeInfo 中的目标地址

**RouteRegion 无 CHECK(FORWARDABLE) 问题**:
- `PrepareForwardableLargeRegion` 设置 `routeState = ROUTED`（不是 FORWARDABLE）
- `RouteRegion` 遇到 `ROUTED` 状态直接 `return true`，**不会**到达 `CHECK(oldState == FORWARDABLE)` 行

## 5. 执行时序约束

```
PostTrace 时序 (WCollector.cpp:419-436):
  ...
  CollectLargeGarbage();            // 先回收死亡大对象
  CompactLOSIfNeeded();             // LOS 压缩（可选）
  CollectPinnedGarbage();
  RefineFromSpace();
  fwdTable.PrepareForwardTable();   // ← 内含 PrepareFromRegionList (DispelGhost + PrepareForwardable)
  BuildLargeObjectMigrationRoutes(); // ← 新增：必须在 PrepareForwardTable 之后
```

**关键时序依赖**:
1. `CollectLargeGarbage` 先执行 → 确保候选列表中只有存活对象
2. `PrepareForwardTable` 先执行 → 确保上一轮 ghost 已清除，不会与本轮迁移 ghost 冲突
3. `BuildLargeObjectMigrationRoutes` 最后执行 → 将迁移 region 加入 fromRegionList / ghostFromRegionList

## 6. 与 LOS Compaction 的关系

- **LOS Compaction**（现有）: LOS 内部碎片压缩，使用 STW + memmove + `UpdateCompactedLOSReferences`
- **MAIN→LOS Migration**（本设计）: 跨空间迁移，使用 ghost forwarding + 并发拷贝

两者互补：
1. Migration 把大对象从 MAIN 迁入 LOS，释放 MAIN 碎片
2. Compaction 压缩 LOS 内部碎片，保持 LOS 空间紧凑

**互斥约束**: 同一轮 GC 中，如果 LOS Compaction（STW）已经移动了某个 LOS 对象，不应该同时有 migration 往 LOS 写入。建议在 `CompactLOSIfNeeded` 和 `BuildLargeObjectMigrationRoutes` 之间做互斥检查。

## 7. 实现细节补充

### 7.1 三阶段实现架构

实际实现采用三阶段架构（避免遍历中修改列表的问题）：

```
Phase 1: Collect candidates
    oldLargeList.VisitAllRegions → 筛选候选 → std::vector<MigrationCandidate>

Phase 2: Allocate + Route (逐个处理)
    for (candidate) {
        LOSManager::AllocLarge(size)       → 在 LOS 分配目标空间
        oldLargeList.DeleteRegion(region)   → 从 MAIN 大对象列表移除
        fromList.PrependRegion(region, FROM_REGION)  → 加入 from 链表
        PrepareForwardableLargeRegion(toAddr)         → 设置 ghost route
    }

Phase 3: Rebuild ghost chain
    fromList.CopyListTo(ghostFromRegionList)  → 重建 ghost 链
```

### 7.2 安全门控

- 通过环境变量 `cjEnableLOSMigration=1` 启用，默认关闭
- 与 LOS compaction 互斥：如果 `IsCompacting() || IsCompactionFrozen()` 则跳过
- LOS 空间不足时优雅降级：`AllocLarge` 返回 0 则停止迁移

### 7.3 筛选条件

```cpp
// 排除条件:
region->GetLiveByteCount() == 0       // 死对象
region->GetRawPointerObjectCount() != 0  // 有原生指针引用（future/monitor等）
region->GetRegionStart() >= rm.GetMainEnd()  // 不在 MAIN 空间（RESERVE 借用区域）
```

### 7.4 Ghost 链完整性

`PrepareForwardableLargeRegion(toAddr)` 内部会保存 `nextRegionIdx0 = nextRegionIdx`。
`CopyListTo(ghostFromRegionList)` 从 fromRegionList 复制 head/tail/count。
Ghost 遍历 (`VisitAllGhostRegions`) 使用 `GetNextGhostRegion()` 读取 `nextRegionIdx0`，
因此迁移 region 和小对象 from-region 共享同一条 ghost 链。

## 8. 待验证事项（运行时测试）

1. **VisitLiveObjectsUntilFalse 兼容性**: 大对象 region (LARGE_SIZED_UNITS) 只有 1 个 object 在 offset 0，`VisitLiveObjectsUntilFalse` 需要兼容这种布局
2. **CopyObject 大小限制**: 确认 `CopyObject` 对大对象（可能数 MB）的拷贝行为正确
3. **CollectRegion 对大对象的处理**: 回收 MAIN 大对象的多个 units，同时保留 ghost 元数据
4. **oldLargeRegionList 修改**: 迁移后的 region 从 `oldLargeRegionList` 移除的时机
5. **LOS region 生命周期**: 迁移目标 LOS region 需要加入 LOS 的 `oldLargeRegionList` 进行正常的标记/回收管理
6. **ForwardTable 兼容性**: `fwdTable.RouteRegion` / `fwdTable.RouteObject` 对迁移大对象的处理

## 9. 测试方案

### 9.1 启用迁移
```bash
export cjEnableLOSMigration=1
export MRT_REPORT=./gc_migration.log
export cjHeapSize=256mb
```

### 9.2 测试用例重点
- `gc_los_migrate_test.cj`: 专用测试，高频创建大对象 + 保持引用 + 验证数据完整性
- `gc.cj` 现有 Test 3/7/8/9/10: 大对象分配 + 碎片化场景

### 9.3 验证标准
1. **日志**: grep `LOS migration:` 确认有迁移发生
2. **数据完整性**: 迁移后通过 checksum 验证对象内容未损坏
3. **引用正确性**: 迁移后旧引用通过 barrier/Enum 正确重定向到新地址
4. **无崩溃**: 全测试套件在 `cjEnableLOSMigration=1` 下稳定运行

## 10. 设计总结

本方案的核心思想是：**将 MAIN→LOS 大对象迁移完全融入现有的 ghost forwarding 流水线**，与小对象 forwarding 共享同一套 barrier / Enum / Trace / DispelGhost 机制。

优势：
- **无额外 STW**: 不同于 LOS Compaction 的 STW memmove，迁移在 ForwardFromSpace 中并发完成
- **无全堆扫描**: 不同于 `UpdateCompactedLOSReferences` 的全堆 ref field 扫描，迁移利用 tagged pointer + barrier + 下一轮 Enum/Trace 惰性修正引用
- **机制复用**: 完全复用 `PrepareForwardableLargeRegion` / `ForwardObjectExclusive` / `CopyObject` / `DispelGhostFromRegion` 等现有代码
- **Ghost 生命周期一致**: 与小对象 forwarding 相同的两轮 GC 生命周期，无需特殊处理