# 仓颉 Runtime — Large Object Space (LOS) 整体实现概述

> **日期**: 2026-02-26（更新: 根据 LOS_Research_Report 补充 + 代码验证）
> **状态**: M1~M5 全部里程碑已实现，第一轮性能优化已完成

---

## 目录

- [1. 问题背景与设计目标](#1-问题背景与设计目标)
- [2. 三段内存模型](#2-三段内存模型)
- [3. 核心类与源码索引](#3-核心类与源码索引)
- [4. 分配流程](#4-分配流程)
- [5. GC 回收流程](#5-gc-回收流程)
- [6. CompactLOS 滑动整理](#6-compactlos-滑动整理)
- [7. RESERVE 借还机制](#7-reserve-借还机制)
- [8. Ghost Routing 迁移机制](#8-ghost-routing-迁移机制)
- [9. ShrinkInactiveZone](#9-shrinkinactivezone)
- [10. 已完成的优化](#10-已完成的优化)
- [11. 功能启用状态](#11-功能启用状态)
- [12. 性能现状](#12-性能现状)

---

## 1. 问题背景与设计目标

### 1.1 问题

仓颉 Runtime 的 GC 采用平滑并发复制式收集器（WCollector），大对象（≥ 32KB）与小对象共享同一 region 堆和 `FreeRegionManager`，存在三个核心问题：

1. **碎片放大**：大对象需连续多 unit 的 region，与小对象交错后空闲空间被切割
2. **无独立回收策略**：大对象仅做标记-清扫，不参与复制式搬迁，但与小对象共享空闲管理器
3. **缺乏弹性调度**：堆空间固定分配，无法根据运行时大/小对象比例动态调整

### 1.2 设计目标

| 目标 | 说明 |
|------|------|
| **空间隔离** | 大对象拥有独立的 LOS 空间，与 MAIN（小对象）物理隔离 |
| **弹性调度** | RESERVE 缓冲区支持 MAIN ↔ LOS 动态借还空间 |
| **碎片消除** | LOS 支持滑动整理（Sliding Compaction）清除碎片 |
| **零侵入降级** | 比例配置为 0 时完全回退到原有行为，无性能损失 |
| **GC 集成** | 与 WCollector GC 流程无缝集成 |

### 1.3 里程碑

| 里程碑 | 名称 | 核心内容 | 状态 |
|--------|------|----------|------|
| M1 | 空间划分 + 骨架 | 三段堆划分、LOSManager/ReserveManager 骨架 | ✅ |
| M2 | 分配分流 | LOS 优先分配、RESERVE 借用、MAIN 降级回退 | ✅ |
| M3 | GC 回收 + 迁移 | LOS 标记-清扫、MAIN→LOS Ghost Routing 迁移 | ✅ |
| M4a | LOS 滑动整理 | 7 阶段 Sliding Compaction + Pinned 屏障 | ✅ |
| M4b | MAIN 碎片整理 | 低地址优先分配 + ShrinkInactiveZone | ✅ |
| M5 | 动态调度 | RESERVE A+B 分块借用、watermark 归还、GC 周期计数 | ✅ |

## 2. 三段内存模型

堆空间在初始化时按可配置比例划分为三段：

```
regionHeapStart                                                            regionHeapEnd
    │                              │                    │                        │
    ▼                              ▼                    ▼                        ▼
    ┌──────────────────────────────┬────────────────────┬────────────────────────┐
    │         MAIN Space           │   RESERVE Zone     │       LOS Space        │
    │      （小对象 + 降级）        │    （弹性缓冲）     │     （大对象专用）       │
    │                              │                    │                        │
    │   inactiveZone ──────►       │◄── mainWatermark   │    ◄── losInactiveZone │
    │   低地址 → 高地址增长         │    losWatermark ──►│    高地址 → 低地址增长   │
    └──────────────────────────────┴────────────────────┴────────────────────────┘
    │<────── 默认 60% ────────────>│<──── 默认 30% ────>│<───── 默认 10% ───────>│
```

**关键设计原则**：

- **MAIN** 从低地址向高地址增长（`inactiveZone` 右移）
- **LOS** 从高地址向低地址增长（`losInactiveZone` 左移）
- **RESERVE** 夹在中间，两端可被借用：MAIN 从低端取（`mainWatermark` 右移），LOS 从高端取（`losWatermark` 左移）
- 两个 watermark 不可交叉，保证 RESERVE 完整性

### 比例配置

通过环境变量控制（默认 6:3:1）：

```cpp
int mainRatio    = GetSpaceRatio("cjMainRatio", 6);     // 默认 6
int reserveRatio = GetSpaceRatio("cjReserveRatio", 3);   // 默认 3
int losRatio     = GetSpaceRatio("cjLOSRatio", 1);       // 默认 1
```

**降级兼容**：`cjLOSRatio=0` 且 `cjReserveRatio=0` 时，全部空间归 MAIN，`losManager` 和 `reserveManager` 均为 `nullptr`，行为等同原版。

## 3. 核心类与源码索引

### 3.1 文件依赖关系

```
WCollector.cpp/h                     ← GC 主控，调度 LOS 回收/压缩/迁移
    └── LOSManager.h/cpp (457+907行)  ← LOS 核心：分配、回收、CompactLOS
            ├── FreeRegionManager.h (219行)  ← 空闲块管理（笛卡尔树 dirty+released 双树）
            │       └── CartesianTree.h/cpp (723+137行)
            ├── ReserveManager.h/cpp (203+218行)  ← RESERVE 缓冲区借还
            ├── RegionInfo.h (1297行)  ← 区域元数据
            └── RegionList.h (246行)   ← 区域链表

RegionManager.h/cpp (779+2046行)      ← 堆 owner，持有 LOS/RESERVE 管理器
    ├── LOSManager*
    ├── ReserveManager*
    └── FreeRegionManager (MAIN)

RegionSpace.h/cpp (278+356行)         ← 对外分配接口，路由到 LOS/MAIN
```

### 3.2 核心类职责

| 类 | 源文件 | 职责 |
|----|--------|------|
| `RegionManager` | `RegionManager.h/cpp` | 堆 owner，空间划分，分配入口路由，GC 统计 |
| `LOSManager` | `LOSManager.h/cpp` | LOS 空间：分配、GC 清扫、CompactLOS 滑动整理、迁移 |
| `ReserveManager` | `ReserveManager.h/cpp` | RESERVE 缓冲区：借用/归还、A+B 分块策略、watermark 管理 |
| `FreeRegionManager` | `FreeRegionManager.h` | 空闲 region 管理：CartesianTree (dirty + released) |
| `WCollector` | `WCollector.cpp` | GC 控制器：调度 LOS 清扫、整理、迁移、RESERVE 归还 |
| `RegionSpace` | `RegionSpace.h/cpp` | 分配 API 层：路由大/小对象，OOM 重试 + RESERVE 借用 |
| `CartesianTree` | `CartesianTree.h/cpp` | 笛卡尔树：max-heap-on-count + BST-on-index 空闲块管理 |

### 3.3 关键编译宏与配置

| 宏/环境变量 | 说明 |
|------------|------|
| `cjMainRatio` (env) | MAIN 空间比例，默认 6 |
| `cjReserveRatio` (env) | RESERVE 空间比例，默认 3 |
| `cjLOSRatio` (env) | LOS 空间比例，默认 1 |
| `cjLargeObjectThreshold` (env) | 大对象阈值，默认 32KB |
| `cjEnableLOSMigration` (env) | `=1` 开启 MAIN→LOS Ghost Routing 迁移（默认关闭） |
| `LOS_COMPACTION_FRAG_THRESHOLD` | 碎片阈值 0.3，超过则触发 CompactLOS |
| `LOS_RESERVE_RETURN_KEEP_RATIO` | 延迟归还保留 1/4 借入空间 |
| `MIN_BORROW_CHUNK_UNITS` | 最小借用块 16 unit |
| `BORROW_RATIO_DIVISOR` | 按剩余 RESERVE 的 10% 计算借用块 |
| `BORROW_COOLDOWN_CYCLES` | 常量 2（已定义，逻辑未实现） |
| `RETURN_COOLDOWN_CYCLES` | 常量 2（已定义，逻辑未实现） |

## 4. 分配流程

### 4.1 入口点

```
RegionSpace::TryAllocateOnce(size, allocType)
    │
    ├─ allocType == PINNED_OBJECT?  →  RegionManager::AllocPinned()
    ├─ size >= largeObjectThreshold (32KB)?  →  RegionManager::AllocLarge()
    └─ 刮                                      →  AllocBuffer::Allocate() (小对象快路径)
```

### 4.2 大对象分配路由 (AllocLarge)

```cpp
// RegionManager.h
uintptr_t AllocLarge(size_t size) {
    if (losManager != nullptr) {
        uintptr_t addr = losManager->AllocLarge(size);  // LOS 主路径
        if (addr != 0) return addr;
    }
    // MAIN fallback
    uintptr_t addr = AllocLargeInMain(size);
    if (addr != 0 && losManager != nullptr)
        losManager->IncrementFallbackCount();  // 统计 fallback
    return addr;
}
```

### 4.3 LOSManager::AllocLarge 内部路径

```
LOSManager::AllocLarge(size)
    │
    ├─ Step 1: losFreeMgr.TakeRegion()  ← 从笛卡尔树取回收的 region
    │     ├─ 尝试 dirtyTree (try_lock + O(log n))
    │     └─ 尝试 releasedTree (try_lock + O(log n))
    │
    └─ Step 2: AllocFromInactiveZone()  ← CAS 循环扩展 losInactiveZone
          └─ losInactiveZone 向低地址移动
```

分配成功后调用 `EnlistLargeRegion()` 将 region 编入相应链表（根据 GC 阶段放入 traceRegions 或 recentList）。

### 4.4 分配失败重试机制 (P1-P6)

`RegionSpace::Allocate` 在 `TryAllocateOnce` 返回 0 时进入重试循环（`ShouldRetryAllocation`）。
重试阶梯如下（来源: [RegionSpace.cpp](src/Heap/Allocator/RegionSpace.cpp#L33)）：

| 重试次数 | 代号 | 操作 |
|---------|------|------|
| 1 | P1 (RESCHEDULE) | `CJThreadResched()` — 让出 CPU 调度，仅其他仓颉线程释放后重试即可 |
| 2-3 | P2-P3 | GC 已启动 → `WaitForGCFinish()`（进安全区等待）；GC 未启动 → `RequestGC(HEU)` |
| 4 | P4 (TRIGGER_OOM) | `RequestGC(GC_REASON_OOM)` — 触发 OOM 级 GC（含 CompactLOS/ShrinkInactiveZone） |
| 5 | P5 (TRIGGER_RESERVE_BORROW) | `TryBorrowFromReserveIfNeeded(size)` — RESERVE 缓冲区借用 |
| 6 | P6 | `ExceptionManager::OutOfMemory()` — 抛出 OOM 异常 |

### 4.5 RESERVE 借用触发 (P5)

RESERVE 借用**不在**正常分配路径上，仅在 OOM GC 后仍然失败、重试第 5 次时触发。
`TryBorrowFromReserveIfNeeded` 同时检查 LOS 和 MAIN 两个维度（来源: [RegionSpace.cpp](src/Heap/Allocator/RegionSpace.cpp#L73)）：

```
TryBorrowFromReserveIfNeeded(allocSize)
    ├─ 大对象 + losManager != nullptr?
    │   └─ losRunway < allocSize → reserveManager->BorrowForLOS(allocSize)
    └─ mainRunway < allocSize   → reserveManager->BorrowForMain(allocSize)
```

> **注**: RESERVE 借用已从 `LOSManager::AllocFromInactiveZone()` 内部移至 `RegionSpace` 层级。
> LOS 的 `AllocFromInactiveZone()` 若空间不足直接返回 nullptr，让上层重试循环驱动借用。

### 4.6 分配热路径开销堆叠

| 步骤 | 操作 | 成本 |
|------|------|------|
| 1 | `allocType == PINNED` | 分支(UNLIKELY) |
| 2 | `allocSize >= threshold` | 分支(UNLIKELY) |
| 3 | `losManager != nullptr` | 分支 |
| 4 | `losFreeMgr.TakeRegion` | try_lock + O(log n) tree |
| 5 | (如果 tree 失败) `AllocFromInactiveZone` CAS | CAS |
| 6 | `EnlistLargeRegion` → `GetGCPhase` | 间接调用链 |
| 7 | `losAllocCount` fetch_add | atomic |

## 5. GC 回收流程

### 5.1 GC 周期中的 LOS 交互（完整调用链）

以下是 `DoGarbageCollection()` 中与 LOS 相关的完整调用序列
（来源: [WCollector.cpp](src/Heap/WCollector/WCollector.cpp#L962)）：

```
WCollector::DoGarbageCollection()
    │
    ├── TraceHeap()
    │    ├─ AssembleGarbageCandidates()             ← 前置准备
    │    │    ├─ AssembleMigrationCandidates()       ← 选择 MAIN→LOS 迁移候选（需 cjEnableLOSMigration=1）
    │    │    ├─ AssembleSmallGarbageCandidates()
    │    │    ├─ AssemblePinnedGarbageCandidates()
    │    │    ├─ AssembleLargeGarbageCandidates()
    │    │    └─ losMgr->AssembleLOSGarbageCandidates()  ← LOS recent→old 合并，清除存活信息
    │    ├─ DoEnumeration / DoTracing               ← 并发枚举 + 标记
    │    └─ ProcessFinalizers()
    │
    ├── PostTrace()
    │    ├─ HandleTraceRegions() / HandleLOSTraceRegions()  ← trace cache → recent 合并
    │    ├─ CollectLargeGarbage()                   ← LOS 标记-清扫
    │    │    └─ losMgr->CollectLOSGarbage()
    │    │         ├─ 死对象 → ReleaseLOSRegion() → losFreeMgr.AddReleaseUnits()
    │    │         └─ 存活对象 → 重置标记位
    │    ├─ CompactLOSIfNeeded()                    ← 仅 OOM GC: 有条件触发 STW 滑动整理
    │    │    └─ CompactLOSOnOOMIfNeeded()
    │    │         ├─ CanAllocLargeAfterGC() 跳过启发
    │    │         └─ ScopedStopTheWorld { CompactLOS + UpdateCompactedLOSReferences }
    │    ├─ CollectPinnedGarbage() / RefineFromSpace()
    │    ├─ PrepareForwardTable()
    │    └─ BuildLargeObjectMigrationRoutes()       ← MAIN→LOS Ghost Routing 迁移
    │
    ├── Preforward()                                ← 并发 preforward 阶段
    ├── ForwardFromSpace()                          ← 复制式搬迁
    ├── FinalizeLOSCompactionIfNeeded()             ← 结算异步压缩 pending 计划
    │
    ├── [OOM GC only] ScopedStopTheWorld { ShrinkInactiveZone() }
    │    ├─ Phase A: 收缩 MAIN inactiveZone
    │    └─ Phase B: 归还 RESERVE-borrowed 空间
    │
    ├── CleanupMigrationCandidates()                ← 归还未迁移的候选区域
    └── reserveManager->NotifyGCCycle()             ← 计数器 + 冷却跟踪
```

### 5.2 CollectLOSGarbage 实现要点

- 用内联数组 (`uintptr_t[32]`) 保护 pending compaction 目标区域，避免 `unordered_set` 堆分配
- 死区域统一走 `ReleaseLOSRegion` → `ReleaseUnits` + `AddReleaseUnits`（releasedTree）
  - Linux: `madvise(MADV_DONTNEED)` 惰性清零，无 memset
  - macOS: `mmap(MAP_FIXED)` 重映射
- 不再按 4MB 阈值分流 dirty/released，全部走 released 路径

## 6. CompactLOS 滑动整理

### 6.1 触发条件

仅在 **OOM GC** 时触发，且满足以下之一：
- 碎片率 > 30% (`LOS_COMPACTION_FRAG_THRESHOLD`)
- 有分配失败记录

**跳过启发式** (#10D): 如果普通 GC 已回收足够空间（`CanAllocLargeAfterGC()` 返回 true），则跳过昂贵的 CompactLOS。

### 6.2 7 阶段流程 (STW)

| 阶段 | 操作 | 复杂度 | 成本 |
|------|------|--------|------|
| Phase 1 | Collect: 分离 movable vs pinned | O(n) | 低 |
| Phase 2 | Sort + Compute: 按地址降序排列，计算目标位置 | O(n log n) | 低 |
| Phase 3 | Build migration map: `unordered_map<old, new>` | O(m) | 低 |
| Phase 4 | (空) 引用更新由 WCollector 管理 | - | - |
| Phase 5 | **memmove**: 按高目标优先执行对象搬移 | O(totalBytes) | **极高** |
| Phase 5b | Rebuild region lists | O(n) | 低 |
| Phase 6 | RebuildFreeRegionManager: ClearTrees + 重新插入间隙 | O(gaps × log n) | 中 |
| Phase 7 | Shrink: 归还 RESERVE 借用空间 (延迟归还，保留 25%) | O(1) | 可忽略 |

### 6.3 异步压缩路径 (Forwarding-based)

除了 STW 内的 memmove + 全堆扫描路径，LOS 还实现了基于 Forward 机制的**异步压缩路径**。
核心思想：将 CompactLOS 拆分为 3 步，利用 GC 的正常 Forward 阶段完成引用更新，避免 STW 全堆扫描。

```
GC 周期 N:
  BuildCompactionPlan → pendingCompactionPlan (带 mutex 保护)
  InstallPendingCompactionForForwarding()
    → 为每个 entry 建立 ghost route (PrepareForwardableLargeRegion)
    → 依赖关系处理 (目标区域被其他 entry 的源占据时递归安装)

GC 周期 N (Forward 阶段):
  正常 Forward 阶段自动 forward 这些对象

GC 周期 N 或 N+1:
  FinalizeLOSCompactionIfNeeded()
    → 检查每个 pending entry 是否已 forward
    → 已完成 → 建立新 RegionInfo、回收旧 region (ReleaseLOSRegion)
    → 未完成 → 保留在 pendingCompactionPlan，下周期继续
```

**依赖环处理**: `InstallCompactionEntryWithPredecessorsLocked()` 使用 `visiting` 集合检测依赖环。
当 entry A 的目标区域被 entry B 的源区域占据时，先递归安装 B，再安装 A。

**`EnsureCompactionRouteForObject()`**: Mutator Forward Barrier 遇到 pending compaction 对象时，
惰性安装 ghost route（来源: [LOSManager.cpp](src/Heap/Allocator/LOSManager.cpp#L505)）。

### 6.4 引用更新 (UpdateCompactedLOSReferences)

CompactLOS 后在 STW 中扫描全堆更新引用：

```─ 遍历 MAIN + LOS 所有存活对象的所有引用字段
─ 对每个引用先做 [minOldAddr, maxOldAddr] 范围检查 (跳过 >99%)
─ 范围内才查 migrationMap.find() (hash lookup)
─ 同样扫描所有 GC roots (静态根、线程栈、Finalizer、导出根等)
```

### 6.5 Pinned 对象处理

`rawPointerCount > 0` 的对象为 pinned，不可移动。采用 **Strategy A**：只压缩最高 pinned 对象之上的区域。

### 6.6 容器预分配

`compactMovableRegions_`、`compactPinnedRegions_`、`compactAllLiveRegions_` 为 LOSManager 成员变量，`clear()` 保留 capacity，STW 中零 malloc。已移除 `unordered_set`，改用线性扫描。

## 7. RESERVE 借还机制

### 7.1 借用流程

触发条件：分配失败后 OOM 重试第 5 次（`TRIGGER_RESERVE_BORROW`）。

```
TryBorrowFromReserveIfNeeded(allocSize)
    ├─ 大对象 + LOS 空间不足 → reserveManager->BorrowForLOS()
    └─ MAIN 空间不足      → reserveManager->BorrowForMain()
```

**ComputeBorrowChunk 策略 (A+B 混合)**：
- A = `max(requestSize, 256KB)`
- B = `10% of remaining RESERVE`
- target = `max(A, B)`，对齐到 16-unit 边界

### 7.2 归还流程（当前实现）

RESERVE 空间归还通过**直接水位推进**实现，分两个路径：

| 归还路径 | 触发时机 | 函数 | 说明 |
|---------|---------|------|------|
| **LOS 归还** | CompactLOS Phase 7 | `ReclaimLOSBorrowed(newWatermark)` | `losWatermark` 向高地址推进，保留 25% 缓冲 |
| **MAIN 归还** | ShrinkInactiveZone Phase B | `ReclaimMainBorrowed(newWatermark)` | `mainWatermark` 向低地址推进 |

两者均在 **STW** 下执行，无并发竞态。操作为 O(1) 原子 watermark store + 统计更新。

```
CompactLOS Phase 7:
  fullReturnWatermark = min(lowestLiveStart, losStart)
  keepBuffer = currentBorrowed / 4    // 保留 25% 缓冲
  actualWatermark = fullReturnWatermark - keepBuffer
  reserveManager->ReclaimLOSBorrowed(actualWatermark)

ShrinkInactiveZone Phase B:
  newWatermark = max(highestLiveBorrowedEnd, mainEnd)
  reserveManager->ReclaimMainBorrowed(newWatermark)
  ReleaseUnits(物理页释放)
```

### 7.3 设计中的增强机制（未实现）

以下机制已在设计文档 (M5_Implementation_Record) 中描述，但尚未进入源代码：

| 机制 | 状态 | 说明 |
|------|------|------|
| **Hole Bitmap** | 设计完成 | 非 LIFO 灵活归还、bitmap 标记 + coalesce + watermark 回退 |
| **pendingReserveReturns** | 设计完成 | GC 回收拦截 RESERVE region，暂存后集中归还 |
| **冷却/迟滞** | 常量已定义 | `BORROW_COOLDOWN_CYCLES=2`、`RETURN_COOLDOWN_CYCLES=2` 仅有常量，无决策逻辑 |
| **使用率阈值** | 未实现 | 借入 > 80%、归还 < 60% 等阈值决策 |

## 8. Ghost Routing 迁移机制

### 8.1 目的

将降级分配在 MAIN 空间的大对象迁移到 LOS，释放 MAIN 的 region 压力。

### 8.2 候选预选 (AssembleMigrationCandidates)

在 TraceHeap 前的 `AssembleGarbageCandidates()` 中调用（来源: [RegionManager.cpp](src/Heap/Allocator/RegionManager.cpp#L589)）。
**需要环境变量 `cjEnableLOSMigration=1` 开启**，否则直接返回。

选择条件：
1. 遍历 `oldLargeRegionList` 中的大对象区域
2. 排除 `rawPointerCount > 0` 的 pinned 对象
3. 排除 `regionStart >= mainEnd` 的 RESERVE-borrowed 区域（仅迁移 MAIN 空间本体的）
4. 排除 CompactLOS / 压缩冻结中的情况

候选区域从 `oldLargeRegionList` 移至 `migrationCandidateList`，标记为 `FROM_REGION` 类型。
这使得后续 Enum/Trace 阶段对其引用进行 tagging，为 Forward 阶段做准备。

### 8.3 迁移执行 (BuildLargeObjectMigrationRoutes)

在 PostTrace 阶段执行：

```
1. 快级快开：无迁移候选 → 直接返回
2. 快照候选到 vector
3. 对每个候选：
   ├─ 死对象 → 直接回收
   ├─ 在 LOS 分配目标空间 (AllocLarge)
   │   ├─ 成功 → 设置 ghost route (PrepareForwardableLargeRegion)
   │   └─ LOS 满 → 标记 UNMOVABLE_FROM_REGION
   └─ 复制到 ghostFromRegionList
```

### 8.4 ForwardBarrier 中的 Ghost 检查

- ForwardBarrier 仅在 `GC_PHASE_PREFORWARD` / `GC_PHASE_FORWARD` 期间激活
- 非 GC 阶段使用 `IdleBarrier`，零开销
- Forward 阶段热路径：`!tmpField.IsTagged()` (LIKELY 命中) → 直接返回
- 仅对 tagged 引用才检查 `InGhostFromRegion`（从 UnitInfo 读取 bit）

### 8.5 清理 (CleanupMigrationCandidates)

GC 结束后，将 `migrationCandidateList` 中未迁移的候选（LOS 满、标记为 `UNMOVABLE_FROM_REGION`）
归还至 `oldLargeRegionList`，恢复 `LARGE_REGION` 类型。

### 8.6 当前状态

**功能可用但默认关闭** — 由环境变量 `cjEnableLOSMigration=1` 开启。
默认不开启原因：迁移需要额外的 LOS 空间分配开销，且 Forward 阶段的 ghost routing 增加延迟。
适用场景：MAIN 空间碎片严重、大对象频繁降级分配的工作负载。

## 9. ShrinkInactiveZone

### 9.1 功能

两阶段 STW 操作（~280 行代码，来源: [RegionManager.cpp](src/Heap/Allocator/RegionManager.cpp#L1771)），
在 `DoGarbageCollection()` 中 Forward 阶段之后执行，**仅限 OOM GC**。

**Phase A — 收缩 MAIN inactiveZone**：
1. 线性扫描 `[regionHeapStart, min(oldInactive, mainEnd))`，找到 `highestLiveEnd`
2. P7-fix: 大 region 跨越 MAIN-RESERVE 边界时，cap 到 `mainEnd`
3. 更新 `inactiveZone.store(highestLiveEnd)`
4. 清理 `garbageRegionList` / `pendingGhostGarbageList` 中超出新 inactiveZone 的区域
5. `ReleaseUnits()` 释放物理页

**Phase B — 归还 RESERVE-borrowed 空间**：
1. 扫描 `[mainEnd, min(oldInactive, mainWm))`，找到 `highestLiveBorrowedEnd`
2. P7-fix: 仅扫描已初始化的区域（`oldInactive ≤ mainEnd` 时跳过）
3. `ReclaimMainBorrowed(newWatermark)` 推回 watermark
4. 清理借用区的 garbage/ghost 列表
5. `ReleaseUnits()` 释放物理页

**Fix B — Ghost Routing 目标保护**：
- 扫描 `ghostFromRegionList` + `pendingGhostGarbageList` 的所有 ghost region
- 读取 `RouteInfo` 中的 `toRegion1StartAddress` 和 `toRegion2Idx`
- 如果 ghost 目标在 MAIN 空间 → 提高 `highestLiveEnd`
- 如果 ghost 目标在 RESERVE-borrowed 空间 → 提高 `highestLiveBorrowedEnd`
- 确保不释放仍被 ghost route 引用的目标页

**freeRegionManager 重建**：
- `ClearTrees()` 原子清空两棵树
- 重新扫描 `[regionHeapStart, newInactive)` 和 `[mainEnd, highestLiveBorrowedEnd)` 的空闲区域
- 仅添加缩容后仍在界内的空闲块

### 9.2 当前状态

**已启用** — 在 OOM GC 时通过 `ScopedStopTheWorld` 包裹执行，消除了 ClearTrees 与 mutator 并发的竞态。
`lowAddressFirst` 已设为 `true`（`freeRegionManager.SetLowAddressFirst(true)`），
使分配优先从低地址取 region，最大化高端连续空闲区，利于 ShrinkInactiveZone 回收。

## 10. 已完成的优化

第一轮优化在 2026-02-25 完成，效果显著：

| 编号 | 优化内容 | 具体的改动 |
|------|---------|------------|
| #1 | 移除 `AllocLarge` 双重 atomic 死代码 | 移除 `isCompacting` + `losCompactionFreeze` 的 acquire load（STW 内设置，mutator 不可能见到） |
| #2 | ClearUnits 延迟清零 | LOS 回收统一走 `ReleaseLOSRegion` → releasedTree，Linux 上消除分配时 memset |
| #3A | 引用扫描范围快速跳过 | 预计算 `[minOldAddr, maxOldAddr]`，>99% 引用跳过 hash 查找 |
| #6 | 移除 getenv 守卫 | CompactLOS 默认启用，无需环境变量 |
| #8 | CompactLOS 容器预分配 | 3 个 vector 提升为成员变量，移除 `unordered_set` |
| #9 | CollectLOSGarbage 内联数组 | `uintptr_t[32]` 替代 `unordered_set`，零 malloc |
| **#10A** | **延迟 RESERVE 归还** | CompactLOS Phase 7 保留 25% 借入空间作缓冲 |
| **#10D** | **按需跳过 CompactLOS** | `CanAllocLargeAfterGC()` 检查 GC 后是否已有足够空间，跳过不必要的 STW 压缩 |

## 11. 功能启用状态

### 已启用的核心功能

| 功能 | 触发条件 | 保护方式 | 说明 |
|------|---------|---------|------|
| **CompactLOS** | OOM GC + 碎片率 > 30% | `ScopedStopTheWorld` | 7 阶段滑动整理 + 全堆引用更新 |
| **ShrinkInactiveZone** | OOM GC | `ScopedStopTheWorld` | 二阶段 MAIN + RESERVE 收缩 |
| **lowAddressFirst** | 始终 | — | `SetLowAddressFirst(true)`，低地址优先分配 |
| **RESERVE 借用** | 分配失败 P5 重试 | — | `BorrowForLOS` / `BorrowForMain` |
| **RESERVE 归还** | CompactLOS Phase 7 / ShrinkInactiveZone Phase B | STW 内执行 | 直接 watermark 推进 |
| **CanAllocLargeAfterGC 跳过** | OOM GC + GC 后空间充足 | — | 跳过不必要的 CompactLOS |

### 默认关闭（环境变量开启）

| 功能 | 开关 | 默认值 | 说明 |
|------|------|--------|------|
| **MAIN→LOS 迁移** | `cjEnableLOSMigration=1` | 关闭 | Ghost Routing 迁移，需权衡额外的 LOS 分配开销 |

### 设计完成但未实现

| 功能 | 设计文档 | 说明 |
|------|---------|------|
| Hole Bitmap 灵活归还 | M5_Implementation_Record §8 | 非 LIFO 归还、bitmap + coalesce |
| pendingReserveReturns 拦截归还 | M5_Implementation_Record §4 | GC 回收拦截 RESERVE region，集中归还 |
| 完整冷却/迟滞逻辑 | M5_Implementation_Record §5-6 | 借还冷却周期、使用率阈值决策 |

## 12. 性能现状

### 第一轮优化后 (2026-02-25)

以 C (FLAT, 纯 MAIN 10:0:0) 为基线：

| 堆大小 | 优化前 A/C 比 | 优化后 A/C 比 | 变化 |
|--------|--------------|--------------|------|
| **512MB** | 41% | **106%** | 超过基线 |
| **128MB** | 53% | **100%** | 追平基线 |

### 关键数据点

- **#10D 按需跳过 CompactLOS** 独占 -57% 提升，是最大单项优化
- **B (LOS OFF) 在 128MB 下 SIGSEGV 崩溃**，A (LOS ON) 正常通过 — 证明 LOS 空间隔离的必要性
- CompactLOS 的最大问题不是 memmove，而是 **全堆引用扫描** O(heapSize)
- RESERVE 借用设计良好，完全不影响正常分配路径
- Ghost routing 对正常执行无开销，仅在 Forward 阶段激活

### 分配热路径开销分层

```
每次 LOS 大对象分配的开销堆叠：
  ┌─ losFreeMgr.TakeRegion ────────── ~50-500ns (try_lock + CartesianTree O(log n))
  ├─ losInactiveZone CAS (fallback) ─── ~10-100ns
  ├─ EnlistLargeRegion ────────────── ~20-50ns (GC 阶段检查 + list prepend)
  └─ atomic 统计计数 ────────────── ~5ns (fetch_add)
  合计: ~100-700ns/次（ClearUnits memset 已优化掉）
```
