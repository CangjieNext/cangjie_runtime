# MAIN 与 LOS 水位线回缩与空间归还 — 设计文档

> 创建日期: 2026-02-22
> 里程碑: M4b (MAIN defrag) + M4a (LOS compact) 恢复
> 前置: M3 (MAIN→LOS 迁移) 已完成并验证

---

## 1. 背景

堆内存分为三段，从低到高：

```
[     MAIN     ][  RESERVE  ][       LOS       ]
regionHeapStart  mainEnd      losStart      losEnd
                 reserveStart  reserveEnd
```

- **MAIN** 从低地址向高地址增长，水位线 `inactiveZone` 向上推进
- **LOS** 从高地址向低地址增长，水位线 `losInactiveZone` 向下推进
- **RESERVE** 居中，双方都可借用（MAIN 从左侧借、LOS 从右侧借）

水位线只升不降意味着：

| 后果 | 说明 |
|------|------|
| MAIN 占用只增不减 | 即使大量对象死亡，高地址的空闲 region 无法归还，inactive zone 被永久消耗 |
| LOS 占用只增不减 | 同理，LOS 低地址端的空闲空间无法回收 |
| RESERVE 借出无法收回 | 水位线不回缩 → RESERVE 空间持续被蚕食 → 最终三方都分配困难 |
| 碎片化加剧 | 没有 ShrinkInactiveZone，lowAddressFirst 无意义（没有回缩目标） |

当前状态：**ShrinkInactiveZone (MAIN)** 和 **CompactLOS + ShrinkToReserve (LOS)** 均已禁用，根因是与并发 mutator 的竞态。

---

## 2. 问题分析

### 2.1 MAIN ShrinkInactiveZone 的竞态

`ShrinkInactiveZone` 的操作：
1. 线性扫描 `[regionHeapStart, inactiveZone)` 找到最高存活 region 边界 `highestLiveEnd`
2. `freeRegionManager.ClearTrees()` 清空 dirty/released 两棵 CartesianTree
3. 只将 `[regionHeapStart, highestLiveEnd)` 内的 free region 重新加入树
4. `inactiveZone.store(highestLiveEnd)` 回缩水位线

#### 竞态 1：ClearTrees + 重建 vs mutator 的 ReclaimRegion

```
GC 线程                                  Mutator 线程
──────────                               ──────────────
                                         TakeRegion():
                                           garbageList.TakeHead() → region R
                                           ReclaimRegion(R):
                                             R->InitFreeUnits()    ← IsFreeRegion()=true
                                         ┌── 窗口：还没 AddGarbageUnits ──┐
ClearTrees()                             │                                │
re-scan: IsFreeRegion(R)==true           │                                │
  → AddGarbageUnits(R)  [第1次]          │                                │
                                         │  AddGarbageUnits(R)  [第2次]   │
                                         └────────── 双重插入! ───────────┘
```

`InitFreeUnits()` 将 region metadata 标记为 free，但尚未将其加入 CartesianTree。ShrinkInactiveZone 的 re-scan 看到此 region 是 free，也调用 `AddGarbageUnits()` — 同一 region 被插入两次。

虽然 `AddGarbageUnits` 有防御代码（检测 `MergeInsert` 失败时跳过重复 free region），但这依赖于两次插入都获取同一个锁的串行化。更根本的问题是树的状态不一致——ClearTrees 之前的旧节点被清除，但 mutator 的 AddGarbageUnits 基于旧状态插入。

#### 竞态 2：inactiveZone.store() vs inactiveZone.fetch_add()

```
GC 线程                                  Mutator 线程
──────────                               ──────────────
oldInactive = inactiveZone.load()
scan [start, oldInactive) ...
                                         TakeRegion():
                                           freeRegionManager 无空闲
                                           inactiveZone.fetch_add(size)
                                             → 水位线 = oldInactive + size
                                             → 分配 [oldInactive, oldInactive+size)
inactiveZone.store(highestLiveEnd)        ← 覆盖 fetch_add 的结果!
                                           水位线被回退到 highestLiveEnd
                                           下次 fetch_add 会再次分配 [oldInactive, ...]
                                           → 地址重叠，两个 region 使用相同物理内存!
```

#### 竞态 3（隐蔽）：garbageRegionList 残留

`ShrinkInactiveZone` 扫描时跳过 garbage region（`!IsGarbageRegion()`），所以 `highestLiveEnd` 不包含高地址处的 garbage region。Shrink 降低水位线后：

```
garbageRegionList 中仍有 region G，地址 ∈ [highestLiveEnd, oldInactive)
                                                     ↑↑ 已回缩为 inactive zone

Mutator/FP 线程：
  garbageList.TakeHead() → 取到 G
  ReclaimRegion(G):
    G->InitFreeUnits()
    freeRegionManager.AddGarbageUnits(G)  ← G 地址在 inactive zone 内!
                                            → dirtyTree 中有节点，fetch_add 也会分配同地址
                                            → 双重分配!
```

#### 物理内存未释放

纯逻辑回缩（`inactiveZone.store`）不会释放物理页。`[highestLiveEnd, oldInactive)` 中的物理页仍驻留在 RSS 中。需要显式 `madvise(MADV_DONTNEED)` 才能真正释放。

#### 重新扩展时的脏数据

首次 `fetch_add` 分配的内存是 mmap 匿名页（OS 全零）。Shrink 后重新扩展到同一地址时，内存有旧对象数据。如果没有 `madvise(MADV_DONTNEED)` 和 `ClearUnits`，会读到脏数据。

---

### 2.2 LOS CompactLOS + ShrinkToReserve 的竞态

LOS 压缩的操作（CompactLOS 7 阶段）：
1. 收集存活 region（movable + pinned）
2. 计算压缩目标地址（从 `losEnd` 向低地址紧凑排列）
3. 构建 `migrationMap`（旧地址→新地址）
4. `memmove_s` 移动对象
5. 重建 region lists
6. `RebuildFreeRegionManager` — `losFreeMgr.ClearTrees()` + 扫描间隙重建
7. `ShrinkToReserve` — 归还低地址空闲空间给 RESERVE

**与 MAIN 同类的问题**：

| 问题 | LOS 等价形式 |
|------|-------------|
| ClearTrees + re-scan vs mutator | `RebuildFreeRegionManager` 中 `losFreeMgr.ClearTrees()` + gap re-scan，与 mutator 的 `AllocLarge → ReclaimLOSRegion → AddGarbageUnits` 竞态 |
| 水位线覆盖 | `losInactiveZone.store(lowestLiveStart)` vs mutator 的 `losInactiveZone CAS 推进` |
| 对象移动 vs 并发读写 | `memmove_s` 移动存活对象时，mutator 可能正在读写该对象 → SIGSEGV |
| 引用更新 vs 并发读 | `UpdateCompactedLOSReferences` 更新根+堆引用时，mutator 并发读到旧地址 → dangling pointer |

**额外问题**：LOS 的 `memmove_s` 物理移动对象本身就需要 STW，不是单纯的 metadata 操作。这是 MAIN Shrink 不存在的额外复杂度。

---

### 2.3 RESERVE 空间归还的问题

`ReturnBorrowedToReserve` 本身没有并发竞态（在 GC 尾部调用，pending list 有 mutex 保护）。但有两个间接问题：

1. **不归还就不归还** — 没有 ShrinkInactiveZone，MAIN 的高地址 region 永远占用，不会变成 garbage → 永远不会走到 `ReclaimRegion → IsInReserveTerritory → pendingReserveReturns` 路径
2. **LOS 不压缩就不归还** — 没有 CompactLOS，LOS 的碎片化空间无法整理 → `ShrinkToReserve` 无从执行 → RESERVE 空间持续被 LOS 借用

---

## 3. 方案设计

### 3.1 总体策略

| 组件 | 策略 | 理由 |
|------|------|------|
| **MAIN ShrinkInactiveZone** | STW 保护 | 操作是微秒级 metadata 扫描，STW 开销极小（< 5ms） |
| **MAIN lowAddressFirst** | 直接启用 | 纯算法层面变更，在 tree mutex 保护下无竞态 |
| **LOS CompactLOS** | STW 保护（已有 OOM 路径） | memmove 物理移动对象必须 STW，无法用 barrier 回避 |
| **LOS RebuildFreeRegionManager** | 在 CompactLOS STW 内完成 | 属于 CompactLOS 的一个阶段 |
| **LOS ShrinkToReserve** | 在 CompactLOS STW 内完成 | 与 RebuildFreeRegionManager 同步 |
| **ReclaimRegion/ReleaseRegion** | 增加 inactiveZone 边界检查 | 防止 garbageList 残留 region 回缩后被加入 tree |

### 3.2 MAIN 空间：ShrinkInactiveZone + lowAddressFirst

#### 3.2.1 lowAddressFirst — 零风险启用

```cpp
// RegionManager.cpp Initialize()
// 将:
freeRegionManager.SetLowAddressFirst(false);
// 改为:
freeRegionManager.SetLowAddressFirst(true);
```

效果：`TakeRegion` 分配 region 时优先选择低地址空闲块。操作在 `dirtyUnitTreeMutex` / `releasedUnitTreeMutex` `try_lock` 保护下执行。`TakeUnitsLowAddr` 使用 `FindLowestSuitablePtr` 在 BST 上 O(log n) 搜索，开销与默认路径相当。

lowAddressFirst 使存活 region 向低地址聚集 → 高地址端产生连续空闲区 → ShrinkInactiveZone 回缩效果最大化。

#### 3.2.2 ShrinkInactiveZone — STW 包裹 + 补全

**改造后的完整流程（STW 内）：**

```
STW {
  // Step 1: 扫描 active zone，找到 highestLiveEnd
  for addr in [regionHeapStart, oldInactive):
    if region.IsValid && !IsFree && !IsGarbage:
      highestLiveEnd = max(highestLiveEnd, region.end)

  // Step 2: ClearTrees + 重建 [regionHeapStart, highestLiveEnd) 内的 free region
  freeRegionManager.ClearTrees()
  for addr in [regionHeapStart, highestLiveEnd):
    if region.IsFreeRegion():
      freeRegionManager.AddGarbageUnits(region)

  // Step 3: 释放 [highestLiveEnd, oldInactive) 的物理页
  //   使用 RegionInfo::ReleaseUnits 释放物理页（内部按平台选择 madvise/mmap/VirtualFree）
  ReleaseUnits(highestLiveEndIdx, oldInactiveIdx - highestLiveEndIdx)

  // Step 4: 回缩水位线
  inactiveZone.store(highestLiveEnd)

  // Step 5: 如果回缩区间包含 RESERVE 借用的空间（oldInactive > mainEnd），
  //   将 [max(highestLiveEnd, mainEnd), oldInactive) 归还 RESERVE
  if (oldInactive > mainEnd && reserveManager != nullptr):
    returnStart = max(highestLiveEnd, mainEnd)
    reserveManager->ReturnFromMain(returnStart, oldInactive - returnStart)
}
```

**STW 保证**：
- 所有 mutator 处于 saferegion
- 没有并发 `TakeRegion → ReclaimRegion → AddGarbageUnits`
- 没有并发 `inactiveZone.fetch_add`
- 一次性解决竞态 1、2、3

**停顿估算**：
- MAIN 64MB × 60% = 38.4MB，unit=16KB → 2400 units
- 两遍线性扫描元数据 ≈ 微秒级
- STW 同步等 mutator 到 safepoint ≈ 0.1-2ms
- `madvise` 系统调用 ≈ 微秒级
- 总计 **< 5ms**

#### 3.2.3 garbageRegionList 残留防护

ShrinkInactiveZone 执行后，garbageRegionList 中可能残留地址在 `[highestLiveEnd, oldInactive)` 的 region。这些 region 后续被 `ReclaimRegion` 处理时，不应加入 freeRegionManager（因为该地址已属于 inactive zone）。

在 `ReclaimRegion` 和 `ReleaseRegion` 中增加边界检查：

```cpp
void RegionManager::ReclaimRegion(RegionInfo* region) {
    region->InitFreeUnits();

    // M5: RESERVE 领域检查（已有）
    if (IsInReserveTerritory(region->GetRegionStart())) {
        std::lock_guard<std::mutex> lock(pendingReserveReturnsMutex);
        pendingReserveReturns.push_back({unitIndex, num});
        return;
    }

    // 新增：如果 region 地址已在 inactive zone 以上（已被 Shrink 回收），
    // 不加入 freeRegionManager，避免 tree 与 inactive zone 地址重叠
    if (region->GetRegionStart() >= inactiveZone.load(std::memory_order_acquire)) {
        DLOG(REGION, "skip reclaim region %p@[%#zx, %#zx): above inactiveZone",
             region, region->GetRegionStart(), region->GetRegionEnd());
        return;
    }

    freeRegionManager.AddGarbageUnits(unitIndex, num);
}
```

`ReleaseRegion` 同理增加相同检查。

#### 3.2.4 在 GC 流程中的位置

```cpp
void WCollector::DoGarbageCollection() {
    TraceHeap();
    PostTrace();
    Preforward();
    ForwardFromSpace();
    FinalizeLOSCompactionIfNeeded();

    TransitionToGCPhase(GC_PHASE_IDLE, true);
    MergeResurrectExportObjects();
    PostResolveCycleTask();
    FlipTagID();

    CollectSmallSpace();                          // from-space → garbageList
    UnbindPreviousLiveInfo();
    CleanupMigrationCandidates();

    // ---- M4b: MAIN 水位线回缩 ----
    {
        ScopedStopTheWorld stw("shrink inactive");
        reinterpret_cast<RegionSpace&>(theAllocator)
            .GetRegionManager().ShrinkInactiveZone();
    }

    ReturnBorrowedToReserve();                    // M5: 借用归还
}
```

选择在 `CollectSmallSpace()` 之后：此时 from-space 的垃圾已移到 garbageList，更多 region 变为 free，Shrink 效果最大化。

#### 3.2.5 条件触发 — 不是每次 GC 都执行

ShrinkInactiveZone + STW 有成本（暂停 mutator），不应每次 GC 都执行。触发条件：

```cpp
bool shouldShrink =
    // 条件 1: MAIN 曾经借用过 RESERVE 空间（inactiveZone 越过 mainEnd）
    //   说明堆有过高压力阶段，水位线回缩可以释放借用空间
    (reserveManager != nullptr && inactiveZone.load() > mainEnd)
    ||
    // 条件 2: 即使没有借用 RESERVE，但高地址端有明确可回缩空间
    //   (这里用启发式：garbageRegionList + freeRegionManager 有足够空闲量)
    (freeRegionManager.GetTotalFreeUnits() > shrinkThreshold);
```

**核心判断逻辑**：

| 条件 | 含义 | 理由 |
|------|------|------|
| `inactiveZone > mainEnd` | MAIN 水位线已越过自身边界，借用了 RESERVE | 回缩可直接归还 RESERVE，收益高 |
| `freeUnits > threshold` | 高地址端有足够空闲 region | 回缩能释放物理内存，降低 RSS |
| 两者 OR | 只要满足其一就执行 | RESERVE 借用是优先触发条件 |

**Threshold 建议值**：`shrinkThreshold = mainUnitCount * 0.2`（MAIN 总容量的 20% 空闲才触发非借用场景的 Shrink）。

**实际实现中第一阶段只检查条件 1**（借用了 RESERVE），待验证稳定后再加条件 2。

更新后的 GC 流程：

```cpp
// 在 CollectSmallSpace() 之后
RegionManager& rm = reinterpret_cast<RegionSpace&>(theAllocator).GetRegionManager();
ReserveManager* resMgr = rm.GetReserveManager();

// 条件触发：只有借用了 RESERVE 才执行 STW Shrink
if (resMgr != nullptr && rm.GetInactiveZone() > rm.GetMainEnd()) {
    ScopedStopTheWorld stw("shrink inactive", false);
    rm.ShrinkInactiveZone();
}
```

#### 3.2.6 LOS 水位线回缩 — 无需额外代码

经确认，`CompactLOS`（在 `CompactLOSOnOOMIfNeeded` 中 STW 执行）的 Phase 6-7 已完成：
- Phase 6: `losInactiveZone.store(lowestLiveStart)` + `RebuildFreeRegionManager(allLiveRegions)`
- Phase 7: `ShrinkToReserve(freedBytes)`

三项操作均在 `ScopedStopTheWorld` 保护下完成，无竞态。**LOS 不需要额外的 Shrink 代码**。

---

### 3.3 LOS 空间：CompactLOS + RebuildFreeRegionManager + ShrinkToReserve

#### 3.3.1 为何 LOS 必须用 STW

LOS 压缩涉及 `memmove_s` 物理移动存活对象 + 全堆引用更新，与 MAIN Shrink 纯 metadata 操作本质不同：

| 操作 | 能否用 barrier 避免 STW |
|------|------------------------|
| MAIN ClearTrees + re-scan | 理论上可以用 rwlock，但 STW 更简单 |
| LOS memmove 移动对象 | **不行** — 移动过程中对象的 header/field 是撕裂状态，任何读都会 SIGSEGV |
| LOS 引用更新 | 理论上可以用 forwarding pointer + read barrier，但需要大量改造 |

**结论**：LOS 压缩必须 STW。当前已有 OOM 触发的 STW CompactLOS 路径（`CompactLOSOnOOMIfNeeded`），可以复用。

#### 3.3.2 触发策略：定期 + OOM

当前 CompactLOS 只在 OOM 时触发（被动）。改为主动触发条件：

```
shouldCompact = (losFragmentationRatio > 0.3)       // 碎片率 > 30%
             && (losFreeMgr.dirtyUnits + releasedUnits > threshold)  // 有足够碎片值得压缩
             && (gcCyclesSinceLastCompact >= 3)      // 冷却：避免每次 GC 都压缩
```

执行位置保持不变（`FinalizeLOSCompactionIfNeeded`），用 STW 包裹整个 CompactLOS + RebuildFreeRegionManager + ShrinkToReserve。

#### 3.3.3 STW CompactLOS 停顿估算

```
LOS 6.4MB (64MB × 10%)，假设 10 个存活大对象 × 64KB = 640KB 需要 memmove
memmove 640KB ≈ 50-100μs（M1 芯片内存带宽 ~60GB/s）
引用更新扫描全堆 ≈ 数百μs（取决于堆中引用密度）
总计 ≈ 1-5ms
```

对于 GC 暂停已经存在的场景（OOM），这个额外停顿可以接受。如果定期触发，可设置上限（如最多移动 N 个对象/每次）分摊停顿。

#### 3.3.4 LOS ReclaimLOSRegion 残留防护

与 MAIN 同理，`ReclaimLOSRegion` / `ReleaseLOSRegion` 增加 `losInactiveZone` 边界检查：

```cpp
void LOSManager::ReclaimLOSRegion(RegionInfo* region) {
    region->InitFreeUnits();

    // 已有: RESERVE 领域检查
    if (IsInReserveTerritory(region->GetRegionStart())) {
        pendingReserveReturns.push_back({unitIndex, num});
        return;
    }

    // 新增：如果 region 地址低于 losInactiveZone（已被 Shrink 回收），跳过
    // LOS 方向相反：losInactiveZone 从 losEnd 向 losStart 增长
    // active zone = [losInactiveZone, losEnd)
    // inactive zone = [losStart, losInactiveZone)
    if (region->GetRegionStart() < losInactiveZone.load(std::memory_order_acquire)) {
        return;  // 已在 inactive zone 内
    }

    losFreeMgr.AddGarbageUnits(unitIndex, num);
}
```

---

### 3.4 RESERVE 归还联动

ShrinkInactiveZone 回缩 MAIN 水位线后，释放出的高地址空间不直接"归还"给 RESERVE（它们在 MAIN 的地址范围内，不是从 RESERVE 借的）。但 Shrink 有间接效果：

```
Shrink MAIN 水位线
  → MAIN 可用 inactive zone 增大
  → MAIN 分配压力降低
  → 减少向 RESERVE 借用的需求
  → RESERVE 已借出的空间自然死亡 → ReturnBorrowedToReserve 归还
```

CompactLOS + ShrinkToReserve 是直接归还：

```
CompactLOS 压缩 LOS 对象向高地址紧凑
  → losInactiveZone 上移（LOS inactive zone 在低地址端扩大）
  → ShrinkToReserve 将低地址端的空闲空间通过 ReturnFromLOS 归还 RESERVE
```

**执行顺序**：

```
DoGarbageCollection():
  ... (GC 标记/转发/清理) ...

  // 1. LOS 压缩 + 归还 (STW)
  FinalizeLOSCompactionIfNeeded()         // 内部 STW: CompactLOS → RebuildFreeMgr → ShrinkToReserve

  // 2. MAIN 水位线回缩 (STW)
  {
    ScopedStopTheWorld stw("shrink inactive");
    ShrinkInactiveZone();
  }

  // 3. M5 借用归还 (无竞态)
  ReturnBorrowedToReserve();              // MAIN + LOS 的 pending 借用归还
```

---

## 4. 变更清单（MAIN 空间 — 第一阶段实施）

| # | 文件 | 变更 | 说明 |
|---|------|------|------|
| 1 | `RegionManager.cpp` Initialize() | `SetLowAddressFirst(true)` | 启用低地址优先分配 |
| 2 | `RegionManager.cpp` ShrinkInactiveZone() | 增加 `ReleaseUnits` + RESERVE 归还 | 释放物理页 + 归还 RESERVE 借用空间 |
| 3 | `RegionManager.cpp` ReclaimRegion() | 增加 inactiveZone 边界检查 | 防止 garbageList 残留 region 回缩后加入 tree |
| 4 | `RegionManager.cpp` ReleaseRegion() | 同上 | 同上 |
| 5 | `RegionManager.h` | 增加 `GetInactiveZone()` / `GetMainEnd()` 公共接口 | 供 WCollector 条件判断 |
| 6 | `WCollector.cpp` DoGarbageCollection() | 条件触发 STW 包裹 ShrinkInactiveZone 调用 | 仅 RESERVE 被借用时执行 |

> **LOS 不需要额外变更**：CompactLOS Phase 6-7 已在 STW 内完成指针回缩 + 树重建 + ShrinkToReserve。

---

## 5. 风险评估

| 风险 | 影响 | 缓解 |
|------|------|------|
| STW 停顿增加 | MAIN Shrink < 5ms；LOS Compact < 5ms | 分别独立 STW，可单独禁用；LOS Compact 可设频率限制 |
| lowAddressFirst 降低分配吞吐 | `FindLowestSuitablePtr` 额外 O(log n) 搜索 | 实测 CartesianTree 节点数通常 < 100，开销忽略 |
| Shrink 后 re-expand 性能 | madvise 后重新分配需要 OS 分配物理页 | 页错误代价 ~1μs/page，远小于 GC 收益 |
| garbageList 残留 region 被跳过 | 永远不加入 tree，物理页也不释放 | 下次 Shrink 或 fetch_add 扩展时会覆盖；madvise 已释放物理页 |

---

## 6. 验证计划

1. **单元验证**：构造 64MB 堆，分配→释放大量 region → 触发 GC → 验证 `inactiveZone` 和 `losInactiveZone` 回缩
2. **并发压测**：多线程高频分配/释放，开启 ASAN/TSAN 检测竞态
3. **环境变量门控**：初期用 `cjEnableShrinkInactive=1` 控制开关，默认禁用，验证通过后默认启用
4. **性能对比**：对比启用/禁用 lowAddressFirst + ShrinkInactiveZone 的 GC 停顿时间和 RSS
