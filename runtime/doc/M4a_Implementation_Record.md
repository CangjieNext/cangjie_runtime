# M4a 实现记录：LOS 滑动整理（Sliding Compaction）

## 1. 目标

在 GC 清扫（M3）之后对 LOS 空间执行条件性滑动整理，消除碎片、回收空间并归还给 RESERVE。

核心目标：
- **消除碎片**：将可移动对象向高地址（losEnd）滑动紧凑排列
- **重建空闲管理**：整理后完全重建 losFreeMgr 的两棵 CartesianTree
- **归还 RESERVE**：整理释放的低地址空间立即归还给 RESERVE

## 2. 修改文件清单

| 文件 | 改动类型 | 说明 |
|------|----------|------|
| `src/Heap/Allocator/FreeRegionManager.h` | 新增方法 | `ClearTrees()` — 清空 dirtyTree + releasedTree |
| `src/Heap/Allocator/LOSManager.h` | 新增声明 | `CompactLOS()`, `NeedsCompaction()`, `GetFragmentationRatio()`, compaction 统计字段, `RebuildFreeRegionManager()`, `isCompacting`/`compactionRequested`/`losCompactionFreeze` 原子标志 |
| `src/Heap/Allocator/LOSManager.cpp` | 新增实现 | CompactLOS 7 阶段实现 + RebuildFreeRegionManager |
| `src/Heap/WCollector/WCollector.h` | 新增声明 | `CompactLOSIfNeeded()`, `CompactLOSOnOOMIfNeeded()`, `UpdateCompactedLOSReferences()` |
| `src/Heap/WCollector/WCollector.cpp` | 新增实现 | CompactLOS 集成 + 引用更新 + OOM 门控 |

## 3. 设计决策

### 3.1 插入位置

CompactLOS 在两个位置执行：

**1、每次 GC 后（`DoGarbageCollection` 中，`ForwardFromSpace` 之后）：**

当 LOS 碎片率 > 30% 时，在 STW 下执行。

**2、OOM GC 的 PostTrace 中（`CompactLOSOnOOMIfNeeded`）：**

当 `gcReason == GC_REASON_OOM` 且 `NeedsCompaction()` 返回 true 时执行。带 #10A 优化：如果常规 GC 已释放足够 LOS 空间（`CanAllocLargeAfterGC()`），则跳过昂贵的 CompactLOS。

**理由**：清扫后碎片信息最准确；整理后 LOS 有更大连续空间，利于后续分配。

### 3.2 触发条件

条件性触发（不是每次 GC 都整理）：

1. **碎片率阈值**：`(dirtyUnits + releasedUnits) / totalActiveUnits > 0.3`
2. **分配失败计数**：自上次整理以来有新的 LOS `AllocLarge` 失败

阈值常量：`LOS_COMPACTION_FRAG_THRESHOLD = 0.3`

### 3.3 整理策略

**滑动整理向高地址（losEnd）靠拢，策略 A（Pinned 屏障）**：

- 找到最高地址的 pinned 对象（`rawPointerCount > 0`）的 `regionEnd` 作为屏障
- 只整理屏障以上的可移动对象
- 从 losEnd 向低地址分配目标位置，对象紧凑排列

### 3.4 Forwarding Map 与引用更新

M4a compaction 使用 `migrationMap`（`unordered_map<uintptr_t, uintptr_t>`）存储 old→new 地址映射。

CompactLOS 完成后，WCollector 调用 `UpdateCompactedLOSReferences()` 扫描全堆更新引用：
- 预计算 moved 对象的地址范围 `[minOldAddr, maxOldAddr]` 做快速拒绝
- 扫描所有堆对象的 RefField + 静态根
- 通过 `migrationMap` 哈希查找替换旧地址为新地址
- 更新完成后调用 `ClearMigrationMap()` 清除映射表

### 3.5 losFreeMgr 重建

整理后完全重建 `losFreeMgr`：

1. 调用 `ClearTrees()` 清空 dirtyTree + releasedTree
2. 遍历排序后的存活 region 列表，计算相邻间隙
3. 对每个间隙：`InitRegionAt` + `InitFreeUnits` + `ReleaseUnits`（物理释放）+ `AddReleaseUnits`（进入 releasedTree）

> **P0-opt 优化**：间隙页先经 madvise 释放物理页（`ReleaseUnits`）再加入 releasedTree，新分配时 `TakeRegion` 无需 memset 清零。

### 3.6 RESERVE 归还

整理释放的低地址空间直接通过 `ReserveManager::ReclaimLOSBorrowed()` 归还，无保留缓冲区（No keep buffer）。防振荡由碎片率门控（>30%）间接实现。

## 4. GC 流程中的 LOS 整理

```
                ┌─────────────────────┐
                │ CollectLargeGarbage  │   清扫 MAIN + LOS 死对象
                └──────────┬──────────┘
                           │
                           ▼
                ┌─────────────────────┐
                │ CompactLOSIfNeeded  │   检查碎片率 / 分配失败
                └──────────┬──────────┘
                           │
              NeedsCompaction()?
              ┌────────────┴────────────┐
              │ YES                     │ NO
              ▼                         ▼
   ┌──────────────────┐          (skip)
   │   CompactLOS()   │
   ├──────────────────┤
   │ Phase1: 收集     │  分离 movable / pinned
   │ Phase2: 计算     │  从 losEnd 向下分配目标地址
   │ Phase3: 转发表   │  构建 migrationMap
   │ Phase5: 移动     │  memmove_s + 重建 RegionInfo
   │ Phase5b: 重建列表│  clear + prepend all live regions
   │ Phase6: 重建FRM  │  ClearTrees + insert gaps (releasedTree)
   │ Phase7: 归还     │  ReclaimLOSBorrowed()
   └──────────────────┘
              │
              ▼
   ┌──────────────────────────┐
   │ MigrateLargeObjectsToLOS │  MAIN → LOS 迁移 (追加 forwarding)
   └──────────────────────────┘
```

### 整理前后内存布局

```
整理前：
losStart                                            losInactiveZone    losEnd
    |     [obj1]  [   ]  [obj2]  [PIN]  [obj3]  [   ]  [obj4]     |         |
    v                                                              v         v
    低地址 ──────────────────────────────────────────────────> 高地址

整理后：
losStart      newLosInactiveZone                                   losEnd
    |    [空闲区域]  [PIN]  [obj1] [obj2] [obj3] [obj4]            |         |
    v                 ^                                             v         v
    低地址 ───────────│────────────────────────────────────> 高地址
                      │
               PIN 对象不动，可移动对象向高地址紧凑排列
               低地址空闲空间归还 RESERVE
```

## 5. 新增 API 说明

### LOSManager

| 方法 | 说明 |
|------|------|
| `NeedsCompaction()` | 检查是否需要整理：碎片率 > 30% 或有新的分配失败 |
| `CompactLOS()` | 执行滑动整理算法（Phase 1/2/3/5/5b/6/7），返回释放字节数 |
| `GetFragmentationRatio()` | 计算碎片率 = freeUnits / activeUnits |
| `RebuildFreeRegionManager()` | 清空并重建 losFreeMgr 的两棵树（间隙进 releasedTree） |
| `CanAllocLargeAfterGC()` | #10A: 检查 GC 后是否已有足够空间，跳过昂贵整理 |

### FreeRegionManager

| 方法 | 说明 |
|------|------|
| `ClearTrees()` | 清空 dirtyTree + releasedTree（线程安全） |

### WCollector

| 方法 | 说明 |
|------|------|
| `CompactLOSIfNeeded()` | PostTrace 中调用（OOM GC 时），条件性执行 LOS 整理 |
| `CompactLOSOnOOMIfNeeded()` | OOM 门控 + #10A skip 逻辑 |
| `UpdateCompactedLOSReferences()` | STW 下扫描全堆更新 compaction 引用 |

### 统计字段（LOSManager 新增）

| 字段 | 含义 |
|------|------|
| `losCompactCount` | 整理执行次数 |
| `losCompactMovedBytes` | 上次整理移动字节数 |
| `losCompactFreedBytes` | 上次整理释放字节数 |
| `losCompactShrunkBytes` | 上次整理归还 RESERVE 字节数 |
| `lastCompactAllocFailCount` | 上次整理时的 allocFailCount（冷却计数） |

## 6. 异步整理（Async Compaction）— 设计已完成，代码未实现

> **注意**：以下为设计方案，实际代码中 `SignalLOSCompaction`、`NotifyToCompactLOS`、`PerformLOSCompaction`、`shouldCompactLOS` 等 API 均未实现。FinalizerProcessor 中仅有 `#include "LOSManager.h"`，无异步整理相关逻辑。
>
> 当前 CompactLOS 仅通过两条同步路径触发：
> 1. `DoGarbageCollection` 中 `ForwardFromSpace` 之后（碎片率 > 30%）
> 2. OOM GC 的 PostTrace 中（`CompactLOSOnOOMIfNeeded`）

### 6.1 设计动机

同步整理需要在 STW 期间由 GC 线程触发。某些场景下 LOS 碎片化严重但常规 GC 未被触发，LOS 分配频繁失败并降级到 MAIN。设计方案是通过 FinalizerProcessor 线程检测碎片化后主动请求 sync GC 来执行整理。

### 6.2 设计方案概要（未实现）

- 分配路径 LOS 分配失败时调用 `SignalLOSCompaction()` 设置 `compactionRequested` 并唤醒 FP 线程
- FP 线程主循环检查 `shouldCompactLOS`，调用 `PerformLOSCompaction()` 请求 sync GC
- 在该 GC 的 STW 阶段执行 `CompactLOS()`

### 6.3 已实现的状态标志

| 标志 | 位置 | 含义 |
|------|------|------|
| `LOSManager::isCompacting` | `std::atomic<bool>` | 整理进行中，阻止所有 LOS 新分配 |
| `LOSManager::compactionRequested` | `std::atomic<bool>` | 分配路径设置，用于触发 compaction |
| `LOSManager::losCompactionFreeze` | `std::atomic<bool>` | 冻结标志，暂停 compaction |

## 7. 编译验证

```
Build and preinstall completed successfully.
```

macOS aarch64 Release 构建成功，无错误。
