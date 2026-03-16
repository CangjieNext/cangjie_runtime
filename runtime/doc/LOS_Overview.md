# LOS（Large Object Space）实现概述

**日期**：2026年3月16日  
**分支**：LOS-mac2  
**平台**：macOS aarch64

---

## 1. 项目目标

为 Cangjie Runtime 的 GC 系统实现独立的大对象空间（Large Object Space），解决原有架构中大对象与小对象混合分配导致的碎片化问题。

**核心收益**：
- 碎片率从 41% 降至 17%（-24 个百分点）
- OOM 容量提升 42%（从 ~151MB 到 ~215MB，256MB 堆）
- 大对象有独立的生命周期管理，不干扰小对象的 region 分配

---

## 2. 内存布局

```
regionHeapStart         mainEnd   reserveStart   reserveEnd   losStart          losEnd
    |                     |            |              |            |               |
    [    MAIN 原始空间     ][ MAIN 借用 ][ RESERVE 空闲 ][ LOS 借用  ][ LOS 原始空间  ]
                           ^                                       ^
                     mainWatermark                           losWatermark
```

- **MAIN**：小对象 + 降级大对象，从低地址向高地址增长
- **RESERVE**：MAIN 与 LOS 之间的缓冲区，双向水位线借用
- **LOS**：大对象（>32KB），从高地址向低地址增长

---

## 3. 实现阶段总览

| 阶段 | 功能 | 核心文件 |
|------|------|----------|
| **M1** | 堆三段划分 + LOSManager/ReserveManager 骨架 | LOSManager, ReserveManager, RegionManager |
| **M2** | LOS 空间初始化 + 大对象分配 + RESERVE 借用 | LOSManager, ReserveManager |
| **M3** | LOS GC 回收 + MAIN→LOS 大对象迁移（两阶段 ghost route） | LOSManager, RegionManager, WCollector |
| **M4a** | LOS 滑动整理（Sliding Compaction） | LOSManager, WCollector |
| **M4b** | MAIN 低地址优先分配 + inactiveZone 缩回 | CartesianTree, FreeRegionManager, RegionManager |
| **M5** | RESERVE 空间动态归还（通过 bulk watermark reclaim） | ReserveManager, RegionManager, WCollector |

---

## 4. M1：堆三段划分与骨架

### 4.1 空间划分

将原有单一 MAIN 堆划分为三段：

```
[    MAIN    ][  RESERVE  ][   LOS   ]
     60%          30%         10%
```

比例通过环境变量可配：`cjMainRatio`、`cjReserveRatio`、`cjLOSRatio`。

### 4.2 核心组件

- **LOSManager**：LOS 空间管理器骨架（分配、回收、统计）
- **ReserveManager**：RESERVE 缓冲区管理器（双向水位线借用）
- **RegionManager**：新增 `mainEnd`、`losManager`、`reserveManager` 指针

### 4.3 后向兼容

未配置环境变量时退化为原有单 MAIN 模式（RESERVE=0, LOS=0）。

---

## 5. M2：LOS 空间初始化与分配

### 5.1 分配流程

```
RegionManager::AllocLarge(size)
  → LOSManager::AllocLarge(size)
    → losFreeMgr (dirtyTree → releaseTree)     // 复用已回收空间
    → AllocFromInactiveZone()                   // 扩展 losInactiveZone
    → BorrowForLOS() + 重试                      // RESERVE 借用
  → 失败：AllocLargeInMain()                    // MAIN 降级
```

### 5.2 RESERVE 借用（A+B 混合块策略）

`ReserveManager::BorrowForMain/LOS` 使用分块策略：
```
chunkSize = align_up(max(requestSize, max(minChunk, available * 10%)), alignment)
```
- 最小块：16 个 unit（256KB @ 16KB 页）
- 比例分配：剩余 RESERVE 的 10%
- 水位线保护：`mainWatermark` 不超过 `losWatermark`

---

## 6. M3：LOS GC 回收与 MAIN→LOS 迁移

### 6.1 LOS 独立回收

LOS 死亡 region 回收到 `losFreeMgr`（LOS 自己的 FreeRegionManager），不进入 MAIN 的 `freeRegionManager`。

回收策略与 MAIN 一致：
- `ReclaimLOSRegion`：≤4MB → dirtyTree
- `ReleaseLOSRegion`：>4MB → 物理释放 + releasedTree

### 6.2 两阶段迁移设计

**阶段一（Trace 前）：`AssembleMigrationCandidates()`**

在 `AssembleGarbageCandidates()` 最前面执行：
1. 遍历 MAIN 的 `oldLargeRegionList`
2. 过滤不可迁移 region（有 rawPointer、在 RESERVE 领域）
3. 移入 `migrationCandidateList`，类型设为 `FROM_REGION`
4. 调用 `ClearLiveInfo()` + `ResetMarkBit()` — 确保 trace 重新标记

**阶段二（PostTrace 后）：`BuildLargeObjectMigrationRoutes()`**

在 `PrepareForwardTable()` 之后执行（需先清理旧的 ghost route）：
1. **死对象**（`liveByteCount == 0`）→ 直接 `CollectRegion` 回收
2. **活对象 + LOS 可分配** → `AllocLarge` + `PrepareForwardableLargeRegion` 设置 ghost route → 移入 `fromRegionList`
3. **活对象 + LOS+RESERVE 均满** → 设置 `UNMOVABLE_FROM_REGION` 留在 `migrationCandidateList`

**GC 结束后：`CleanupMigrationCandidates()`**
- 将未迁移的候选（`UNMOVABLE_FROM_REGION`）merge 回 `oldLargeRegionList`，类型恢复为 `LARGE_REGION`

### 6.3 isMarked 残留 Bug 修复

**根因**：`ClearLiveInfo()` 只清除 `liveByteCount`，不清除大对象的 `isMarked` 标志。候选移出 `oldLargeRegionList` 后，`ResetMarkBit()` 不再覆盖到它们 → `isMarked` 残留为 1 → trace 跳过 `AddLiveByteCount` → 活对象被误判为死对象。

**修复**：`AssembleMigrationCandidates()` 中 `ClearLiveInfo()` 之后立即调用 `ResetMarkBit()`。

---

## 7. M4a：LOS 滑动整理

### 7.1 触发条件

CompactLOS 在两种场景触发：
1. **每次 GC 后**（DoGarbageCollection 中）：当 LOS 碎片率 > 30% 时
2. **OOM GC 的 PostTrace 中**：当 `gcReason == GC_REASON_OOM` 且 `NeedsCompaction()` 返回 true

> **#10A 优化**：OOM GC 中，如果常规 GC 已释放足够 LOS 空间（`CanAllocLargeAfterGC()`），则跳过昂贵的 CompactLOS。

### 7.2 执行阶段

```
CompactLOS() — 在 STW 下执行
  Phase 1: 收集 — 分离 movable / pinned regions
  Phase 2: 计算 — 从 losEnd 向低地址分配目标位置（Strategy A：不跨越 pinned 屏障）
  Phase 3: 转发表 — 构建 migrationMap（old → new）
  Phase 4: （由 WCollector 处理引用更新）
  Phase 5: 移动 — memmove_s（高地址优先，对象向 losEnd 方向滑动）
  Phase 5b: 重建 region 列表（清空 + 重新 prepend 所有存活 region）
  Phase 6: 重建 losFreeMgr（ClearTrees + 插入间隙到 releasedTree）
  Phase 7: RESERVE 归还 — ReclaimLOSBorrowed（无保留缓冲区，全额归还）
```

### 7.3 整理前后内存布局

```
整理前：
losInactiveZone                                                        losEnd
    |     [obj1]  [   ]  [obj2]  [PIN]  [obj3]  [   ]  [obj4]           |
    v                                                                   v
    低地址 ──────────────────────────────────────────────────────> 高地址

整理后：
newLosInactiveZone                                                     losEnd
    |    [空闲/归还]  [PIN]  [obj1] [obj2] [obj3] [obj4]                 |
    v                 ^                                                  v
                      │
               PIN 对象不动，可移动对象向高地址紧凑排列
               低地址空闲空间归还 RESERVE
```

### 7.4 引用更新

整理完成后，WCollector 调用 `UpdateCompactedLOSReferences()` 扫描全堆更新引用：
- 预计算 moved 对象的地址范围 `[minOldAddr, maxOldAddr]` 做快速拒绝
- 扫描所有堆对象的 RefField + 静态根
- 通过 `migrationMap` 哈希查找替换旧地址为新地址

### 7.5 异步整理（已设计，未实现）

`LOSManager` 提供 `isCompacting` 和 `compactionRequested` 原子标志用于协调：
- `isCompacting=true` 时 `AllocLarge()` 返回 0，强制 MAIN 降级
- `losCompactionFreeze=true` 时拒绝新的 LOS 分配

> **注**：M4a 文档中描述的 `FinalizerProcessor` 异步触发路径（`SignalLOSCompaction/NotifyToCompactLOS/PerformLOSCompaction`）在当前代码中**未实现**。实际 CompactLOS 完全在 GC 线程的 STW 阶段同步执行。

---

## 8. M4b：MAIN 碎片整理

### 8.1 低地址优先分配

`CartesianTree::TakeUnitsLowAddr()` 利用 CartesianTree 的 BST 性质（左子树=低地址）优先取最低可用空闲块。

在 `RegionManager::Initialize()` 中对 MAIN 的 freeRegionManager 启用：
```cpp
freeRegionManager.SetLowAddressFirst(true);
```

LOS 的 losFreeMgr 不启用（保持默认）。

### 8.2 inactiveZone 缩回（ShrinkInactiveZone）

**执行时机**：每次 GC 后，当 MAIN 碎片率 > 30% 时在 STW 下执行。

**两阶段扫描**：
- **Phase A**：扫描 MAIN 原始空间 `[regionHeapStart, mainEnd)` 找到 `highestLiveEnd`
- **Phase B**：扫描 RESERVE 借用空间 `[mainEnd, mainWatermark)` 找到 `highestLiveBorrowedEnd`
- **Ghost 路由保护**：扫描 `ghostFromRegionList` 和 `pendingGhostGarbageList` 中的 to-region 目标地址，确保不缩回正在使用的 ghost 目标空间

**树修剪（而非重建）**：
使用 `freeRegionManager.RemoveAboveIndex(threshold)` 修剪高地址节点，而非 `ClearTrees()` + 全量重建。避免扫描已缩回区域的 metadata（防止 borrow-on-demand 导致的 metadata 一致性崩溃）。

### 8.3 大对象 region 对齐

`AllocLargeInMain()` 中 `regionCount` 向上对齐到 `maxUnitCountPerRegion` 整数倍：
```cpp
regionCount = ((regionCount + maxUnitCountPerRegion - 1) / maxUnitCountPerRegion) * maxUnitCountPerRegion;
```
释放后形成整数个标准 region，利于复用。

---

## 9. M5：RESERVE 动态归还

### 9.1 实际实现方式

与 M5 原始设计文档不同，当前代码**未使用** `pendingReserveReturns` 暂存队列、`ReturnBorrowedToReserve()` 函数、Hole Bitmap、冷却/迟滞阈值机制。

实际归还机制：

| 归还侧 | 触发位置 | 机制 |
|--------|----------|------|
| **MAIN → RESERVE** | `ShrinkInactiveZone()` Phase B | 扫描借用空间找最高活跃 region → `ReclaimMainBorrowed(newWatermark)` 直接回退水位 |
| **LOS → RESERVE** | `CompactLOS()` Phase 7 | 整理后 `lowestLiveStart` 作为新边界 → `ReclaimLOSBorrowed(newWatermark)` 直接回退水位 |

### 9.2 ReserveManager API

```cpp
// 批量回收 — 直接推回水位线（无 bitmap、无 LIFO 约束）
size_t ReclaimMainBorrowed(uintptr_t newWatermark);  // MAIN 侧
size_t ReclaimLOSBorrowed(uintptr_t newWatermark);   // LOS 侧
```

### 9.3 冷却机制

代码中保留了冷却常量定义（`BORROW_COOLDOWN_CYCLES = 2`）和 GC 周期计数器（`NotifyGCCycle()`），但冷却检查 API（`IsMainBorrowInCooldown` 等）**未实现**。归还由碎片率门控（>30%）间接实现防振荡。

### 9.4 LOS_RESERVE_RETURN_THRESHOLD

LOSManager 中保留了 `LOS_RESERVE_RETURN_THRESHOLD = 0.4` 常量定义，但 CompactLOS Phase 7 中**未引用此阈值**，实际是全额归还（无保留缓冲区）。

> **注**：`LOS_RESERVE_RETURN_KEEP_RATIO = 4`（25% 保留）在 LOSManager.h 中有定义，但 CompactLOS Phase 7 代码注释明确写了"No keep buffer"。

---

## 10. GC 完整流程（当前代码实际执行顺序）

```
DoGarbageCollection()
  │
  ├─ TraceHeap()
  │    ├─ AssembleGarbageCandidates()
  │    │    ├─ AssembleMigrationCandidates()        ← M3: 选迁移候选（FROM_REGION + ResetMarkBit）
  │    │    ├─ AssembleSmallGarbageCandidates()
  │    │    ├─ AssemblePinnedGarbageCandidates()
  │    │    ├─ AssembleLargeGarbageCandidates()
  │    │    └─ AssembleLOSGarbageCandidates()        ← M3: LOS recent→old，清存活信息
  │    ├─ DoEnumeration()
  │    ├─ PrepareTrace()                             ← M3: 含 LOS trace cache 激活
  │    ├─ DoTracing()
  │    └─ ProcessFinalizers()
  │
  ├─ PostTrace()
  │    ├─ HandleTraceRegions()                       ← M3: + HandleLOSTraceRegions()
  │    ├─ ClearWeakRefBuffer()
  │    ├─ ClearBuffer()
  │    ├─ PrepareCycleRef()
  │    ├─ CollectLargeGarbage()                      ← M3: MAIN + LOS 死亡 region 清扫
  │    ├─ CollectPinnedGarbage()
  │    ├─ RefineFromSpace()
  │    ├─ PrepareForwardTable()
  │    └─ BuildLargeObjectMigrationRoutes()          ← M3: MAIN→LOS 迁移路由（ghost route）
  │
  ├─ Preforward()
  ├─ ForwardFromSpace()
  ├─ FinalizeLOSCompactionIfNeeded()
  │
  ├─ ShrinkInactiveZone()                            ← M4b + P3: MAIN 缩回 + RESERVE 归还
  │    └─ (gated: MAIN fragRatio > 30%)
  │
  ├─ CompactLOS()                                    ← M4a: LOS 滑动整理 + RESERVE 归还
  │    ├─ (gated: LOS fragRatio > 30%)
  │    ├─ CompactLOS() — 7 阶段
  │    └─ UpdateCompactedLOSReferences()
  │
  ├─ CleanupMigrationCandidates()                    ← M3: 未迁移候选归还 oldLargeRegionList
  │
  ├─ FlipTagID()
  ├─ CollectSmallSpace()
  │
  └─ NotifyGCCycle()                                 ← M5: 冷却计数器
```

---

## 11. 关键数据结构

### 11.1 LOSManager

| 字段 | 类型 | 说明 |
|------|------|------|
| `losFreeMgr` | `FreeRegionManager` | LOS 独立的空闲管理（dirtyTree + releasedTree），baseUnit = mainUnitCount |
| `losOldLargeRegionList` | `RegionList` | 上一轮存活的 LOS 大对象 |
| `losRecentLargeRegionList` | `RegionList` | 本轮新分配的 LOS 大对象 |
| `losLargeTraceRegions` | `RegionCache` | trace 阶段的 region 缓存 |
| `migrationMap` | `unordered_map<uintptr_t, uintptr_t>` | Compaction + Migration 共享的 old→new 转发表 |
| `isCompacting` | `atomic<bool>` | 整理进行中标志 |
| `compactionRequested` | `atomic<bool>` | 异步整理请求标志 |
| `losCompactionFreeze` | `atomic<bool>` | 冻结 LOS 分配标志（A-STEP1） |
| `pendingCompactionPlan` | `vector<CompactEntry>` | 延迟执行的整理计划 |

### 11.2 RegionManager（LOS 相关新增）

| 字段 | 类型 | 说明 |
|------|------|------|
| `migrationCandidateList` | `RegionList` | M3 标记前选定的迁移候选 |
| `mainEnd` | `uintptr_t` | MAIN 原始空间上界（不含 RESERVE/LOS） |
| `losManager` | `LOSManager*` | 指向 LOS 管理器 |
| `reserveManager` | `ReserveManager*` | 指向 RESERVE 管理器 |

### 11.3 ReserveManager

| 字段 | 类型 | 说明 |
|------|------|------|
| `mainWatermark` | `atomic<uintptr_t>` | MAIN 借用高水位 |
| `losWatermark` | `atomic<uintptr_t>` | LOS 借用低水位 |
| `gcCycleCount` | `size_t` | GC 周期计数器 |

### 11.4 CartesianTree

双重性质：BST on index（地址）+ Max-heap on count（块大小）。

新增 `TakeUnitsLowAddr()`：利用 BST 性质优先搜左子树（低地址），O(log n)。

新增 `RemoveAboveIndex()`：修剪高于阈值的节点（用于 ShrinkInactiveZone 的树修剪优化）。

---

## 12. 性能数据（256MB 堆，gc.cj 测试）

| 指标 | LOS-mac2 | Origin | 差异 |
|------|----------|--------|------|
| OOM Round | **360** | 260 | **+38.5%** |
| 有效数据量 | **~215 MB** | ~151 MB | **+42.4%** |
| 碎片率 | **17.0%** | 41.0% | **-24pp** |
| Wall time | 25.6s | 21.2s | +20.8% |
| Heuristic GC 次数 | 71 | 114 | -37.7% |
| Heuristic GC 平均耗时 | 15.9ms | 7.1ms | +124% |
| OOM GC 次数 | 160 | 35 | +357% |
| OOM GC 平均耗时 | 19.2ms | 23.6ms | -18.5% |
| 总 GC 次数 | 231 | 149 | +55% |
| MAIN active | 60% | 99.97% | — |
| LOS 压缩次数 | 137 | — | — |

**关键结论**：
- OOM 容量提升显著（+42%），碎片率大幅下降
- Heuristic GC 单次耗时增长 2.2 倍（LOS 管理开销 + 迁移路由构建 + RESERVE 调整）
- OOM GC 在高压力下反而更快（-18.5%）
- 需要调优 heuristic 触发阈值（当前 69% GC 由 OOM 触发，不理想）

---

## 13. 稳定性问题

LOS-mac2 存在偶现崩溃（8 次中 3 次，37.5%）：
- 均为 SIGSEGV/SIGABRT，发生在 `BaseObject::GetSize()` 
- 访问地址 0x8 或 0x16（空指针偏移），推测为 LOS 迁移/整理期间并发安全问题
- 启用 GC 日志（`MRT_REPORT`）时 crash rate 更高

---

## 14. 后续优化方向

### 14.1 稳定性修复（P0）
- 排查并修复 LOS 迁移/整理期间的并发安全问题（37.5% crash rate）
- 重点关注 ForwardBarrier 与 CompactLOS 的竞态窗口

### 14.2 GC 性能优化（P1）
- **Heuristic GC 耗时回归**：单次 GC 从 7.1ms 增长到 15.9ms（+124%）
  - 分析 `BuildLargeObjectMigrationRoutes` 的开销（每次 GC 都执行）
  - 仅在 MAIN 大对象存在时才进入迁移路径
- **Heuristic 触发调优**：69% GC 由 OOM 触发，应增加 heuristic GC 比例
- **CompactLOS 优化**：考虑增量/分批整理，减少单次 STW 暂停时间

### 14.3 异步整理实现（P2）
- M4a 文档中设计的 FinalizerProcessor 异步触发路径尚未编码
- 可在 LOS 分配失败降级 MAIN 时异步请求一次带 CompactLOS 的 GC

### 14.4 RESERVE 冷却/迟滞机制（P3）
- M5 设计的冷却和迟滞阈值未实现
- 当前依赖碎片率门控（>30%）间接防振荡
- 如果观察到频繁借还（borrow-return ping-pong），可实现冷却机制

---

## 15. 源文件清单

| 文件 | 行数 | 核心改动 |
|------|------|----------|
| `LOSManager.h` | ~460 | LOS 管理器定义：分配、回收、整理、迁移映射、异步协调标志 |
| `LOSManager.cpp` | ~900 | CompactLOS 7 阶段实现、RebuildFreeRegionManager、GC 回收 |
| `ReserveManager.h` | ~200 | RESERVE 管理器：水位借用、批量回收、A+B 分块策略 |
| `ReserveManager.cpp` | ~220 | 借用/回收实现、ComputeBorrowChunk |
| `RegionManager.h` | ~800 | 新增 migrationCandidateList、mainEnd、LOS/RESERVE 指针 |
| `RegionManager.cpp` | ~2000 | AssembleMigrationCandidates、ShrinkInactiveZone（Phase A+B） |
| `FreeRegionManager.h` | ~230 | TakeUnitsLowAddr、ClearTrees、RemoveAboveIndex、lowAddressFirst |
| `CartesianTree.h` | ~400 | TakeUnitsLowAddr/LowAddrImpl、RemoveAboveIndex、FindLowestSuitable |
| `RegionSpace.h` | — | AssembleGarbageCandidates/CollectLargeGarbage 集成 LOS |
| `WCollector.h` | — | BuildLargeObjectMigrationRoutes、CompactLOSIfNeeded 声明 |
| `WCollector.cpp` | — | 完整 GC 流程集成：迁移、整理、引用更新、shrink |
