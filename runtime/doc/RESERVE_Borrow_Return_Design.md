# RESERVE 空间借还机制详细设计文档

> 日期: 2026-02-23  
> 状态: 代码已实现，编译通过

## 目录

1. [概览](#1-概览)
2. [内存布局与初始化](#2-内存布局与初始化)
3. [借入机制（Borrow）](#3-借入机制borrow)
4. [MAIN 空间分配与使用](#4-main-空间分配与使用)
5. [LOS 空间分配与使用](#5-los-空间分配与使用)
6. [归还机制（Return）](#6-归还机制return)
7. [MAIN 空间整理（ShrinkInactiveZone）](#7-main-空间整理shrinkinactivezone)
8. [LOS 空间整理（CompactLOS）](#8-los-空间整理compactlos)
9. [水线推进与收缩总结](#9-水线推进与收缩总结)
10. [完整时序：一次 OOM 分配的生命周期](#10-完整时序一次-oom-分配的生命周期)
11. [发现的问题](#11-发现的问题)

---

## 1. 概览

### 1.1 核心思想

堆内存被划分为三个连续区域：**MAIN**（小对象）、**RESERVE**（缓冲区）、**LOS**（大对象）。
RESERVE 不直接分配对象，而是作为 MAIN 和 LOS 的"伸缩空间"：

- 当 MAIN 空间不足时，可以向 RESERVE **借入**一块空间，扩展 MAIN 的分配上界
- 当 LOS 空间不足时，可以向 RESERVE **借入**一块空间，扩展 LOS 的分配下界
- GC 整理后，如果借入的空间不再需要，则**归还**给 RESERVE

**借入的本质就是移动水线指针**，不涉及内存拷贝或映射变更。

### 1.2 关键设计原则

| 原则 | 说明 |
|------|------|
| **延迟借入** | 只在 OOM GC 后仍分配失败时才借入（P5），避免频繁借还 |
| **OOM-only 归还** | MAIN 的 RESERVE 归还只在 OOM GC 的 STW 中执行（P5）|
| **Chunk 摊销** | 每次借入不是精确的请求大小，而是一个较大的 chunk，减少借入次数 |
| **正常路径无感知** | 借入后，分配走正常的 `fetch_add` / CAS 路径，不需要特殊逻辑 |
| **STW 整理** | ShrinkInactiveZone 和 CompactLOS 都在 STW 中执行，消除并发竞态 |

### 1.3 参与的类

| 类 | 角色 | 关键文件 |
|----|------|----------|
| `ReserveManager` | RESERVE 空间管理器，维护两个水线指针 | `ReserveManager.h/cpp` |
| `RegionManager` | MAIN 空间管理器，内含 `inactiveZone` 指针 | `RegionManager.h/cpp` |
| `LOSManager` | LOS 空间管理器，内含 `losInactiveZone` 指针 | `LOSManager.h/cpp` |
| `RegionSpace` | 分配入口，重试逻辑和借入决策在此 | `RegionSpace.h/cpp` |
| `WCollector` | GC 收集器，驱动整理和归还流程 | `WCollector.cpp` |
| `FreeRegionManager` | 空闲 region 树管理（dirtyTree + releaseTree）| `FreeRegionManager.h` |

## 2. 内存布局与初始化

### 2.1 物理布局

```
低地址 ──────────────────────────────────────────────────────── 高地址
│  RegionInfo 元数据  │     MAIN     │  RESERVE  │     LOS      │
│  (每 unit 一个)     │              │           │              │
└─────────────────────┴──────────────┴───────────┴──────────────┘
                      ↑              ↑           ↑              ↑
                regionHeapStart    mainEnd    reserveEnd      losEnd
                                  (=reserveStart) (=losStart)
```

默认比例 **6:3:1**（MAIN:RESERVE:LOS），通过环境变量可调：
- `cjMainRatio` (默认 6)
- `cjReserveRatio` (默认 3)  
- `cjLOSRatio` (默认 1)

### 2.2 初始化流程 (`RegionManager::Initialize`)

```
RegionManager::Initialize(nUnit, regionInfoAddr)
│
├─ 计算 mainUnitCount / reserveUnitCount / losUnitCount
│
├─ 计算地址边界:
│   mainEnd     = regionHeapStart + mainUnitCount * UNIT_SIZE
│   reserveStart = mainEnd
│   reserveEnd   = reserveStart + reserveUnitCount * UNIT_SIZE
│   losStart     = reserveEnd
│   losEnd       = regionHeapEnd
│
├─ inactiveZone = regionHeapStart     ← MAIN 分配指针，从低地址向高增长
├─ this->mainEnd = mainEnd            ← MAIN 固定上界
│
├─ new ReserveManager()
│   └─ Initialize(reserveStart, reserveEnd, UNIT_SIZE)
│       ├─ mainWatermark = reserveStart   ← MAIN 借入到这里（初始无借入）
│       └─ losWatermark  = reserveEnd     ← LOS 借入到这里（初始无借入）
│
├─ new LOSManager(this, mainUnitCount, reserveManager)
│   └─ Initialize(losStart, losEnd, losUnitStart, losUnitEnd, reserveUnitCount)
│       ├─ losInactiveZone = losEnd       ← LOS 分配指针，从高地址向低增长
│       └─ losFreeMgr.Initialize(reserveUnitCount + losUnitCount)
│           └─ baseUnit = mainUnitCount   ← 索引偏移，覆盖 RESERVE+LOS
│
└─ freeRegionManager.Initialize(mainUnitCount + reserveUnitCount)
    └─ 覆盖 MAIN + RESERVE 空间（借入的死 region 可进入此树）
```

### 2.3 初始指针状态

```
                    MAIN                    RESERVE                   LOS
 regionHeapStart ──────── mainEnd ──── reserveStart ──── reserveEnd ──── losEnd
      ↑                     ↑              ↑                ↑             ↑
  inactiveZone         mainWatermark   reserveStart    losWatermark   losInactiveZone
  (= regionHeapStart)  (= reserveStart)               (= reserveEnd) (= losEnd)
```

此时 RESERVE 可用空间 = `losWatermark - mainWatermark` = 全部 RESERVE 容量。
MAIN 和 LOS 的 effective 边界就是各自的固定边界（mainEnd, losStart）。

## 3. 借入机制（Borrow）

### 3.1 触发时机

借入**不在**分配的热路径上，而是在分配多次失败后的最后手段：

```
RegionSpace::Allocate(size) 重试循环:

  tryTimes=1~3:  CJThreadResched()           ← 让出时间片
  tryTimes=4:    RequestGC(GC_REASON_HEU)    ← 启发式 GC
  tryTimes=5:    RequestGC(GC_REASON_OOM)    ← OOM GC（记录 allocSize、isLargeObject）
                 ↓ OOM GC 执行:
                 │  DoGarbageCollection()
                 │  ├─ ShrinkInactiveZone(returnToReserve = true)  ← Phase A+B 归还
                 │  └─ CompactLOSOnOOMIfNeeded()                   ← LOS 整理+Phase 7 归还
                 │  ReclaimGarbageMemory(true)
                 ↓
  tryTimes=6:    TryBorrowFromReserveIfNeeded(size)  ← ★ 借入决策点
  tryTimes=7:    OutOfMemoryException                ← 真正 OOM
```

**关键**：tryTimes=6 时（OOM GC 后重试仍失败），才尝试借入。这确保了：
1. GC 已经回收了尽可能多的内存
2. ShrinkInactiveZone 已经归还了不需要的 RESERVE 空间
3. 只有在真正空间不足时才借入

### 3.2 借入决策逻辑 (`RegionSpace::TryBorrowFromReserveIfNeeded`)

```cpp
// 来源: RegionSpace.cpp
void RegionSpace::TryBorrowFromReserveIfNeeded(size_t allocSize)
{
    ReserveManager* reserveManager = regionManager.GetReserveManager();
    if (reserveManager == nullptr) return;

    bool isLargeObject = allocSize >= regionManager.GetLargeObjectThreshold();

    // 大对象 → 检查 LOS 跑道
    if (isLargeObject && losManager != nullptr) {
        size_t losRunway = losManager->GetFreeSize();  // losInactiveZone − effectiveLOSStart
        if (losRunway < allocSize) {
            reserveManager->BorrowForLOS(allocSize);   // ★ LOS 借入
        }
    }

    // 检查 MAIN 跑道（小对象唯一路径，大对象 fallback 路径）
    size_t mainRunway = regionManager.GetInactiveUnitCount() * UNIT_SIZE;
    if (mainRunway < allocSize) {
        reserveManager->BorrowForMain(allocSize);      // ★ MAIN 借入
    }
}
```

> 注意：大对象同时检查 LOS 和 MAIN 两个跑道，因为 `AllocLarge` 会先试 LOS、再 fallback 到 MAIN。

### 3.3 借入的实现 — 水线推进

借入的本质就是**移动水线指针**：

#### MAIN 借入 (`BorrowForMain`)

```
借入前:
  inactiveZone ──→          mainEnd(=mainWatermark)
  ├── MAIN 已用 ──┤── MAIN 空闲 ─┤  RESERVE  │
                                  ↑
                             effectiveMainEnd = mainWatermark = reserveStart

借入后 (mainWatermark 右移):
  inactiveZone ──→                         mainWatermark (新)
  ├── MAIN 已用 ──┤── MAIN 空闲(含借入)────┤  RESERVE 剩余  │
                                            ↑
                                       effectiveMainEnd = 新 mainWatermark
```

代码路径：
```
BorrowForMain(size)
├─ lock(borrowMutex)
├─ chunkSize = ComputeBorrowChunk(size)   ← A+B 混合策略
│   └─ target = max(requestSize, max(256KB, available*10%))
│      对齐到 256KB，cap 在 available 内
├─ CommitChunkForMain(chunkSize)
│   ├─ newMain = mainWatermark + chunkSize
│   ├─ if (newMain > losWatermark) → fail  ← 防碰撞
│   └─ mainWatermark.store(newMain)        ← ★ 水线右移
└─ return chunkSize                        ← 返回实际借入字节数
```

#### LOS 借入 (`BorrowForLOS`)

```
借入前:
                              losStart(=losWatermark)     ←── losInactiveZone
  │  RESERVE  │               ├── LOS 空闲 ─┤── LOS 已用 ──┤

借入后 (losWatermark 左移):
                  losWatermark (新)                        ←── losInactiveZone
  │  RESERVE 剩余  │── LOS 空闲(含借入)──┤── LOS 已用 ──┤
                    ↑
               effectiveLOSStart = 新 losWatermark
```

代码路径：
```
BorrowForLOS(size)
├─ lock(borrowMutex)
├─ chunkSize = ComputeBorrowChunk(size)
├─ CommitChunkForLOS(chunkSize)
│   ├─ newLOS = losWatermark - chunkSize
│   ├─ if (newLOS < mainWatermark) → fail  ← 防碰撞
│   └─ losWatermark.store(newLOS)          ← ★ 水线左移
└─ return chunkSize
```

### 3.4 Chunk 大小计算策略 (`ComputeBorrowChunk`)

采用 **A+B 混合策略**，避免频繁小额借入：

```
target = max(requestSize,                    ← A: 至少满足本次请求
             max(MIN_BORROW_CHUNK_UNITS * UNIT_SIZE,  ← B1: 最小 16 units (256KB@16KB-page)
                 available / 10))            ← B2: RESERVE 剩余的 10%

对齐到 BORROW_CHUNK_ALIGN_UNITS * UNIT_SIZE (256KB)

if target > available:
    target = align_up(requestSize, UNIT_SIZE)  ← 降级为精确大小
```

### 3.5 碰撞保护

MAIN 和 LOS 可以同时借入 RESERVE，但两个水线不能交叉：

```
  mainWatermark ──→          ←── losWatermark
  │  MAIN 已借  │  可用空间  │  LOS 已借  │
```

`CommitChunkForMain` 检查 `newMain <= losWatermark`；
`CommitChunkForLOS` 检查 `newLOS >= mainWatermark`。  
都在 `borrowMutex` 保护下执行，互斥。

## 4. MAIN 空间分配与使用

### 4.1 分配路径总览

```
RegionSpace::TryAllocateOnce(allocSize, allocType)
│
├─ PINNED → AllocPinned()                    ← 固定对象，不讨论
├─ allocSize >= largeObjectThreshold → AllocLarge()  ← 见第5节
└─ 普通小对象 → AllocBuffer::Allocate()
    └─ buffer 用完 → AllocateThreadLocalRegion()
        └─ TakeRegion(maxUnitCountPerRegion)  ← ★ 核心分配
```

### 4.2 TakeRegion — MAIN 空间的 region 获取

`TakeRegion` 是 MAIN 空间获取连续 unit 的核心函数，按优先级依次尝试三个来源：

```
TakeRegion(num, type)
│
├─ ① garbageRegionList.TakeHeadRegion()     ← 上次 GC 的垃圾 region（快，但大小可能不匹配）
│   ├─ 大小匹配 → 直接复用
│   └─ 大小不匹配 → ReclaimRegion(head) → 放入 freeRegionManager
│
├─ ② freeRegionManager.TakeRegion(num)      ← 笛卡尔树（dirty+released），O(log n)
│   └─ lowAddressFirst=true → 优先低地址，压缩活对象到低端
│
└─ ③ inactiveZone.fetch_add(size)           ← 从未使用区域前推
    ├─ 检查: addr + size <= GetEffectiveMainEnd()
    │   └─ effectiveMainEnd = reserveManager ? mainWatermark : mainEnd
    ├─ 成功 → InitRegionAt(addr, num, type)
    └─ 失败 → fetch_sub 回滚 → return nullptr
```

### 4.3 effectiveMainEnd 的含义

```
无借入时:  effectiveMainEnd = mainEnd = reserveStart
有借入时:  effectiveMainEnd = mainWatermark > mainEnd

                    MAIN 固定空间              借入区间
  regionHeapStart ──────────── mainEnd ────── mainWatermark
       ↑                                        ↑
   inactiveZone                           effectiveMainEnd
```

`inactiveZone` 的 `fetch_add` 只会和 `effectiveMainEnd` 比较。
借入后 `effectiveMainEnd` 变大，`fetch_add` 就有了更多跑道。
**分配路径完全不知道借入的存在**——它只看到一个更大的边界。

### 4.4 MAIN 大对象分配 (`AllocLargeInMain`)

大对象走 `AllocLarge` → 优先 LOS → fallback 到 MAIN。MAIN 的大对象分配：

```
AllocLargeInMain(size)
├─ regionCount = ceil(size / UNIT_SIZE)
├─ 对齐到 maxUnitCountPerRegion 的倍数     ← 防止碎片化
└─ TakeRegion(regionCount, LARGE_SIZED_UNITS)
```

MAIN 中的大对象使用相同的 `TakeRegion`，受 `effectiveMainEnd` 约束。

## 5. LOS 空间分配与使用

### 5.1 LOS 分配入口 (`LOSManager::AllocLarge`)

```
LOSManager::AllocLarge(size)
│
├─ 如果 isCompacting → return 0           ← 整理期间拒绝分配
├─ 如果 losCompactionFreeze → return 0     ← 整理冻结窗口
│
├─ ① losFreeMgr.TakeRegion(unitCount)     ← 回收的空闲 region（dirty+released 树）
│
└─ ② AllocFromInactiveZone(unitCount)     ← 从未使用区域扩展
    ├─ effectiveStart = GetEffectiveLOSStart()
    │   └─ = reserveManager ? losWatermark : losStart
    ├─ if currentZone < effectiveStart + regionSize → return nullptr
    └─ CAS: losInactiveZone -= regionSize  ← ★ 向低地址扩展
```

### 5.2 LOS 的方向 — 从高到低

LOS 与 MAIN 方向相反：

```
                effectiveLOSStart    losInactiveZone                    losEnd
                      ↓                    ↓                              ↓
  │  RESERVE 剩余  │  LOS 空闲跑道  │  LOS 已分配(活+死)  │
                    ← 分配方向 ←
```

- `losInactiveZone` 初始 = `losEnd`（最高地址）
- 每次分配，`losInactiveZone` 向低地址推进（CAS 减法）
- `effectiveLOSStart` 是下界：无借入时 = `losStart`，有借入时 = `losWatermark`（更低）

### 5.3 effectiveLOSStart 与借入

```
无借入时:  effectiveLOSStart = losStart = reserveEnd
有借入时:  effectiveLOSStart = losWatermark < losStart

           losWatermark      losStart     losInactiveZone         losEnd
               ↓                ↓              ↓                    ↓
  │  RESERVE  │  借入扩展区  │  LOS 原生空闲  │  LOS 已分配  │
               ↑
          effectiveLOSStart
```

与 MAIN 完全对称：借入只是把下界推低，`losInactiveZone` 的 CAS 就有了更多空间。
**LOS 分配代码完全不感知借入**——它只看到 `effectiveLOSStart` 变小了。

### 5.4 LOS 死对象回收

GC mark 后，`CollectLOSGarbage()` 扫描 `losOldLargeRegionList`：
- 死 region → `ReclaimLOSRegion` (小) 或 `ReleaseLOSRegion` (大) → 进入 `losFreeMgr`
- 活 region → 重置 mark bit

回收的 region 进入 **LOS 自己的** `losFreeMgr`（不是 MAIN 的 `freeRegionManager`）。
下次 `AllocLarge` 的 Step 1 就能从 `losFreeMgr` 取到这些空间。

### 5.5 大对象分配的 fallback 路径

```
RegionManager::AllocLarge(size)
├─ ① LOSManager::AllocLarge(size)         ← 优先 LOS
│   ├─ losFreeMgr → 成功: 返回
│   └─ losInactiveZone → 成功: 返回
│
└─ ② AllocLargeInMain(size)               ← LOS 满 → fallback 到 MAIN
    └─ TakeRegion(...) → MAIN 的 freeRegionManager + inactiveZone
```

因此 `TryBorrowFromReserveIfNeeded` 需要同时检查 LOS 和 MAIN 的跑道。

## 6. 归还机制（Return）

### 6.1 设计原则

| 原则 | 说明 |
|------|------|
| **仅 OOM GC 路径** | 归还只在 OOM GC 触发的整理路径中进行（P5 设计） |
| **整理先行** | 先整理空间（ShrinkInactiveZone / CompactLOS），再归还 |
| **单调回退** | watermark 只向 RESERVE 内部退（mainWatermark 向下，losWatermark 向上） |
| **实际归还量** | 只归还确认无活对象的区域，不是盲目归还全部 |

### 6.2 MAIN 借入归还: `ReclaimMainBorrowed`

**调用链**: `ShrinkInactiveZone(isOOM=true)` → Phase B → `ReclaimMainBorrowed`

```
ShrinkInactiveZone Phase B (仅 OOM GC):
│
├─ 扫描 [mainEnd ... mainWatermark) 的借入区域
│   └─ 找到最高地址的活 region 边界 → newMainWatermark
│
├─ if newMainWatermark < 当前 mainWatermark:
│   └─ reserve.ReclaimMainBorrowed(newMainWatermark)
│       ├─ mainWatermark = newMainWatermark   ← ★ 向下退回
│       └─ 回收空间归入 RESERVE
│
└─ 释放 [newMainWatermark ... 旧mainWatermark) 的物理页
```

水线变化：
```
归还前:  reserveStart   mainWatermark(高)                reserveEnd
              ↓              ↓                              ↓
         │  RESERVE 剩余  │       MAIN 借入区域          │

归还后:  reserveStart  mainWatermark(低=newWatermark)    reserveEnd
              ↓              ↓                              ↓
         │  RESERVE 回收  ←  │  仍被借用 │
```

### 6.3 LOS 借入归还: `ReclaimLOSBorrowed`

**调用链**: `CompactLOS Phase 7` → `ReclaimLOSBorrowed`

```
CompactLOS Phase 7:
│
├─ 滑动压缩已将活对象紧贴 losEnd 排列
├─ lowestLiveStart = 最低的活对象起始地址
│
├─ newLosWatermark = min(lowestLiveStart, losStart)
│   └─ 至少退回到 losStart（完全归还所有借入）
│
├─ reserve.ReclaimLOSBorrowed(newLosWatermark)
│   ├─ losWatermark = newLosWatermark   ← ★ 向上退回
│   └─ 回收空间归入 RESERVE
│
└─ 更新 losInactiveZone 到压缩后的新位置
```

水线变化：
```
归还前:  losWatermark(低)      losStart                losEnd
              ↓                   ↓                      ↓
         │  LOS 借入扩展区  │  LOS 原区域  │  ...活/死对象...  │

归还后(完全归还):  losWatermark=losStart           losEnd
                       ↓                              ↓
         │  RESERVE  │  LOS 原区域  │  活对象(紧贴右端)  │    释放
```

### 6.4 两种归还对比

| 维度 | MAIN 侧归还 | LOS 侧归还 |
|------|-------------|-------------|
| 触发路径 | ShrinkInactiveZone Phase B | CompactLOS Phase 7 |
| 触发条件 | isOOM = true | OOM GC 时执行 CompactLOS |
| 收缩方向 | mainWatermark 向下（向 reserveStart） | losWatermark 向上（向 reserveEnd） |
| 归还量计算 | 扫描最高活 region 边界 | 滑动压缩后的最低活对象 |
| API | `ReclaimMainBorrowed(newWatermark)` | `ReclaimLOSBorrowed(newWatermark)` |
| 最大归还 | 退回到 reserveStart（全部归还） | 退回到 losStart（全部归还） |

### 6.5 归还的安全保证

1. **Phase B 仅在 OOM 路径**: 正常 GC 不做归还，避免频繁借还振荡
2. **互斥锁保护**: Borrow 和 Reclaim 共享 `borrowMutex`
3. **先整理后归还**: 只有确认区域无活对象后才推水线
4. **物理页释放**: 归还后 `MadviseRelease` 归还物理页给 OS

## 7. MAIN 空间整理（ShrinkInactiveZone）

### 7.1 概述

`ShrinkInactiveZone` 是 GC STW 期间执行的两阶段空间回收函数，负责回收 MAIN 区域和 RESERVE 借入区域的尾部空闲空间。

```
size_t RegionManager::ShrinkInactiveZone(bool returnToReserve)
```

- **Phase A**: 每次 GC 都执行，收缩 MAIN 原生 inactiveZone
- **Phase B**: 仅 OOM GC 执行（`returnToReserve=true`），归还 RESERVE 借入空间

### 7.2 Phase A: MAIN inactiveZone 收缩

**目标**: 找到 MAIN 区域 `[regionHeapStart, oldInactive)` 中最高地址的活 region，将不活跃区域向下收缩。

```
Phase A 流程:
│
├─ 1. 扫描 [regionHeapStart, oldInactive): 线性遍历所有 region
│     └─ 跳过 FreeRegion、GarbageRegion → 记录 highestLiveEnd
│
├─ 2. newInactive = max(highestLiveEnd, regionHeapStart)
│
├─ 3. ClearTrees(): 清空 freeRegionManager 中所有节点
│
├─ 4. Re-add: 重建 [regionHeapStart, newInactive) 中的 free chunks
│
├─ 5. inactiveZone.store(newInactive)
│
├─ 6. 清理 garbageRegionList: 删除 ≥ newInactive 的 garbage region
│
└─ 7. ReleaseUnits: 将 [newInactive, oldInactive) 的物理页释放给 OS
```

收缩示意：
```
收缩前:  regionHeapStart    活region   死死死   oldInactive     mainEnd
              ↓                ↓       ↓↓↓        ↓               ↓
         │  活活  空  活  │  死  空  死  │  未分配(inactive)  │

收缩后:  regionHeapStart    highestLiveEnd=newInactive          mainEnd
              ↓                ↓                                   ↓
         │  活活  空  活  │            释放给 OS              │
                            ↑
                    inactiveZone 新位置
```

### 7.3 Phase B: RESERVE 借入区域收缩（仅 OOM GC）

**目标**: 扫描 `[mainEnd, mainWatermark)` 的借入区域，找到最高活 region，归还尾部空闲给 RESERVE。

```
Phase B 流程（仅当 returnToReserve=true 且 mainWatermark > mainEnd）:
│
├─ 1. 扫描 [mainEnd, mainWatermark): 找到 highestLiveBorrowedEnd
│
├─ 2. Re-add: 重建 [mainEnd, highestLiveBorrowedEnd) 中的 free chunks
│     └─ 这些碎片仍可被 MAIN 使用
│
├─ 3. newWatermark = max(highestLiveBorrowedEnd, mainEnd)
│     └─ 如果无活 region → 完全归还: newWatermark = mainEnd
│
├─ 4. ReclaimMainBorrowed(newWatermark)
│     ├─ mainWatermark = newWatermark
│     └─ return 归还字节数
│
├─ 5. 清理 garbageRegionList: 删除 [newWatermark, 旧mainWm) 的 garbage
│
└─ 6. ReleaseUnits: 归还物理页
```

### 7.4 ClearTrees 与重建

`ShrinkInactiveZone` 采用 **全量清空+重建** 策略：

1. `freeRegionManager.ClearTrees()` — 原子清除 dirty 和 released 两棵笛卡尔树
2. 按新边界遍历，`AddGarbageUnits` 重新插入有效的 free chunks
3. 之所以不做增量删除：
   - STW 期间无并发修改，全量重建更简单可靠
   - 笛卡尔树节点可能跨越回收边界，增量删除复杂度高
   - 重建同时也消除了旧的碎片，有利于后续分配

### 7.5 garbageRegionList 清理

garbageRegionList 中的 region 是已死但尚未归入 freeRegionManager 的 region。
ShrinkInactiveZone 在收缩边界后，遍历并删除落入回收区域的 garbage region：

- Phase A: 删除 `[newInactive, mainEnd)` 范围的 garbage
- Phase B: 删除 `[newWatermark, 旧mainWm)` 范围的 garbage

这避免了 garbage region 指向已释放的物理页。

### 7.6 调用时机

```
WCollector::DoGarbageCollection
│
├─ ... Mark / Sweep / Copy ...
│
├─ ShrinkInactiveZone(gcReason == GC_REASON_OOM)
│   ├─ Phase A: always                    ← 每次 GC
│   └─ Phase B: only if OOM              ← 仅 OOM GC
│
├─ CompactLOSOnOOMIfNeeded(gcReason)      ← LOS 侧整理
│
└─ NotifyGCCycle()                        ← 通知 ReserveManager
```

## 8. LOS 空间整理（CompactLOS）

### 8.1 概述

`CompactLOS` 是 LOS 的滑动压缩整理，在 OOM GC 时通过 `CompactLOSOnOOMIfNeeded` 调用。
将活对象向 `losEnd`（高地址）方向滑动紧凑排列，释放低地址碎片空间。

### 8.2 七阶段流程

```
CompactLOS()
│
├─ Phase 1: 收集活 region → 分为 movable / pinned
│   ├─ movable: rawPointerCount == 0（可移动）
│   └─ pinned:  rawPointerCount > 0（含裸指针，不可移动）
│   └─ highestPinnedEnd = 最高 pinned region 的上界
│
├─ Phase 2: 计算目标地址（高→低排布）
│   ├─ cursor = losEnd, 从高往低分配 target
│   ├─ movable 按地址降序排列
│   └─ 不能越过 highestPinnedEnd（Strategy A）
│
├─ Phase 3: 构建 migrationMap（转发映射）
│   └─ { oldObjAddr → newObjAddr }
│
├─ Phase 4: 引用更新（由 WCollector 的 forward 阶段通过 route 机制完成）
│
├─ Phase 5: memmove_s 移动对象
│   ├─ 按 compactPlan 顺序（最高 target 先移动）
│   ├─ 清理旧 RegionInfo → 初始化新 RegionInfo
│   └─ Phase 5b: 重建 region list（移动+未移动+pinned）
│
├─ Phase 6: 重建 losFreeMgr + 更新 losInactiveZone
│   ├─ lowestLiveStart = 所有活 region 的最低起始地址
│   ├─ losInactiveZone = lowestLiveStart
│   └─ RebuildFreeRegionManager: 清空树，插入活 region 间的间隙
│
└─ Phase 7: 归还 RESERVE 借入空间
    ├─ newLosWatermark = min(lowestLiveStart, losStart)
    ├─ reserveManager->ReclaimLOSBorrowed(newLosWatermark)
    └─ 统计更新: losCompactMovedBytes, FreedBytes, ShrunkBytes
```

### 8.3 滑动压缩示意

```
压缩前:  effectiveLOSStart   losInactiveZone                        losEnd
              ↓                    ↓                                    ↓
         │  RESERVE  │  空闲  │  活  空  空  活  pin  空  活  空  活  │

压缩后:  effectiveLOSStart   losInactiveZone(new)                   losEnd
              ↓                    ↓                                    ↓
         │  RESERVE  │  空闲大块  │  pin  │  活活活活(紧凑)  │

                     ← 碎片合并 →       ← 活对象滑向右端 →
```

pinned 对象不动，movable 对象在 pinned 之上向 `losEnd` 滑动。

### 8.4 Phase 7 归还细节

```
newLosWatermark = min(lowestLiveStart, losStart)
```

- 如果 `lowestLiveStart >= losStart` → 所有活对象在原生 LOS 范围内 → **完全归还**全部借入
- 如果 `lowestLiveStart < losStart` → 有活对象仍在借入区域 → **部分归还**到 lowestLiveStart

### 8.5 调用条件: `CompactLOSOnOOMIfNeeded`

```
WCollector::CompactLOSOnOOMIfNeeded(gcReason)
│
├─ if gcReason != GC_REASON_OOM → return
│
├─ LOSManager::SetCompacting(true)
│
├─ CompactLOS()           ← 执行整理+归还
│
├─ if migrationMap 非空:
│   ├─ FixupReferences    ← 更新所有引用（roots + heap）
│   └─ ExecuteMoves       ← 实际移动对象
│
└─ LOSManager::SetCompacting(false)
```

### 8.6 与 ShrinkInactiveZone 的配合

在一次 OOM GC 中，两者都会执行：

| 时序 | 操作 | 效果 |
|------|------|------|
| ① | ShrinkInactiveZone Phase A | MAIN inactiveZone 收缩 |
| ② | ShrinkInactiveZone Phase B | MAIN 侧 RESERVE 借入归还 |
| ③ | CompactLOS Phase 1-6 | LOS 滑动压缩 |
| ④ | CompactLOS Phase 7 | LOS 侧 RESERVE 借入归还 |

两侧的归还操作独立、互不干扰，各自通过 `borrowMutex` 保护。

## 9. 水线推进与收缩总结

### 9.1 所有水线/指针一览

| 指针 | 所属 | 含义 | 移动方向 | 何时移动 |
|------|------|------|----------|----------|
| `inactiveZone` | RegionManager | MAIN 已分配区域的上界 | ↑ 向高地址 | TakeRegion 分配时 CAS 推进 |
| `mainWatermark` | ReserveManager | MAIN 借入 RESERVE 的上界 | ↑ 借入时推进 / ↓ 归还时回退 | BorrowForMain ↑ / ReclaimMainBorrowed ↓ |
| `losInactiveZone` | LOSManager | LOS 已分配区域的下界 | ↓ 向低地址 | AllocFromInactiveZone 分配时 CAS 推进 |
| `losWatermark` | ReserveManager | LOS 借入 RESERVE 的下界 | ↓ 借入时推进 / ↑ 归还时回退 | BorrowForLOS ↓ / ReclaimLOSBorrowed ↑ |

### 9.2 水线推进（空间扩展）

```
MAIN 侧:
  inactiveZone ↑        — 每次 TakeRegion 从 inactiveZone 分配
  mainWatermark ↑       — OOM 重试第 6 次 BorrowForMain

LOS 侧:
  losInactiveZone ↓     — 每次 AllocFromInactiveZone
  losWatermark ↓        — OOM 重试第 6 次 BorrowForLOS
```

推进的约束：
- `inactiveZone` 不超过 `effectiveMainEnd` (= mainWatermark 或 mainEnd)
- `losInactiveZone` 不低于 `effectiveLOSStart` (= losWatermark 或 losStart)
- `mainWatermark` 不超过 `reserveEnd - losWatermark偏移` (碰撞保护)
- `losWatermark` 不低于 `mainWatermark` (碰撞保护)

### 9.3 水线收缩（空间回收）

```
MAIN 侧:
  inactiveZone ↓        — ShrinkInactiveZone Phase A（每次 GC）
  mainWatermark ↓       — ShrinkInactiveZone Phase B（仅 OOM GC）

LOS 侧:
  losInactiveZone ↑     — CompactLOS Phase 6 更新到 lowestLiveStart
  losWatermark ↑        — CompactLOS Phase 7（仅 OOM GC）
```

### 9.4 借入与归还的对称性

```
                          RESERVE 空间
                 reserveStart          reserveEnd
                      ↓                    ↓
 ◄─ MAIN 借入 ─►│          可用          │◄─ LOS 借入 ─►
  mainWatermark                             losWatermark
      向右推进 →                           ← 向左推进

  借入: mainWatermark → →    losWatermark ← ←
  归还: mainWatermark ← ←    losWatermark → →

  碰撞保护: mainWatermark 不得 ≥ losWatermark
```

### 9.5 水线状态机总结

```
初始态:
  mainWatermark = reserveStart
  losWatermark  = reserveEnd
  [全部 RESERVE 空间可用]

借入态:
  mainWatermark > reserveStart   (MAIN 借了一些)
  losWatermark  < reserveEnd     (LOS 借了一些)
  [RESERVE 空间被两端蚕食]

归还态（OOM GC 后）:
  mainWatermark → 退到有活对象的边界或 reserveStart
  losWatermark  → 退到有活对象的边界或 losStart(=reserveEnd)
  [RESERVE 空间部分或全部恢复]
```

## 10. 完整时序：一次 OOM 分配的生命周期

### 10.1 分配重试循环

```
Mutator: RegionSpace::Allocate(size)
│
├─ tryTimes=1: TakeRegion → 失败（MAIN 满）
│   └─ RESCHEDULE 未达到, continue
│
├─ tryTimes=2: TakeRegion → 失败
│
├─ tryTimes=3: tryTimes ≥ RESCHEDULE_THRESHOLD(3)
│   └─ Reschedule GC（普通 GC, 不等待完成）
│
├─ tryTimes=4: TakeRegion → 失败（GC 可能在执行）
│
├─ tryTimes=5: tryTimes ≥ TRIGGER_OOM_THRESHOLD(5)
│   └─ ShouldRetryAllocation:
│       ├─ 打印 OOM 诊断日志
│       └─ 触发 OOM GC（GC_REASON_OOM）★ → 等待完成
│                │
│                ├─ WCollector::DoGarbageCollection
│                │   ├─ Mark → Sweep → Copy
│                │   ├─ ShrinkInactiveZone(isOOM=true)
│                │   │   ├─ Phase A: 收缩 MAIN inactiveZone
│                │   │   └─ Phase B: 归还 MAIN 侧 RESERVE 借入
│                │   ├─ CompactLOSOnOOMIfNeeded
│                │   │   ├─ CompactLOS Phase 1-6: 滑动压缩
│                │   │   └─ CompactLOS Phase 7: 归还 LOS 侧 RESERVE 借入
│                │   └─ NotifyGCCycle()
│                │
│                └─ GC 完成，空间可能已恢复
│
├─ tryTimes=5 继续: TakeRegion → 可能成功（GC 回收了空间）
│   └─ 如果成功 → 返回   如果失败 → 继续
│
├─ tryTimes=6: tryTimes ≥ TRIGGER_RESERVE_BORROW(6)
│   └─ ShouldRetryAllocation:
│       └─ TryBorrowFromReserveIfNeeded(size)
│           ├─ 检查 MAIN 跑道: effectiveMainEnd - inactiveZone
│           │   └─ 不足 → BorrowForMain(deficit)
│           │       ├─ chunk = max(deficit, 256KB, 可用10%)
│           │       ├─ mainWatermark += chunk
│           │       └─ effectiveMainEnd 自动更新
│           │
│           └─ 如果是大对象: 检查 LOS 跑道
│               └─ 不足 → BorrowForLOS(deficit)
│                   ├─ chunk = max(deficit, 256KB, 可用10%)
│                   └─ losWatermark -= chunk
│
├─ tryTimes=6 继续: TakeRegion → 成功 ✓（RESERVE 扩展了跑道）
│   └─ 返回分配地址
│
└─ tryTimes=7+: 如果还失败 → 抛出 OOM 异常
```

### 10.2 关键时间点

| 时间点 | 事件 | 水线变化 |
|--------|------|----------|
| T0 | 分配失败开始重试 | 无 |
| T1 (try=5) | 触发 OOM GC | 无 |
| T2 | Phase A: 收缩 inactiveZone | inactiveZone ↓ |
| T3 | Phase B: 归还 MAIN 借入 | mainWatermark ↓ |
| T4 | CompactLOS: 压缩+归还 | losInactiveZone ↑, losWatermark ↑ |
| T5 (try=6) | BorrowForMain | mainWatermark ↑ |
| T6 | TakeRegion 成功 | inactiveZone ↑ |

### 10.3 为什么先 GC 归还再借入？

设计意图：
1. **先整理**: OOM GC 回收死对象、压缩碎片、归还旧借入
2. **再检查**: GC 后可能已有足够空间，不需要借入
3. **最后借入**: 仅当 GC 后仍不够时才借入新空间
4. **避免累积**: 先归后借避免 RESERVE 空间被持续蚕食

## 11. 发现的问题

在审查代码过程中发现以下问题，按严重程度排列：

### 11.1 [中] Cooldown 机制为死代码

**现象**: `ReserveManager` 中定义了完整的冷却机制，但**从未被任何调用路径检查**。

定义的 API：
```cpp
bool IsMainBorrowInCooldown() const;   // 定义在 ReserveManager.h:126
bool IsLOSBorrowInCooldown() const;    // 定义在 ReserveManager.h:129
bool IsMainReturnInCooldown() const;   // 定义在 ReserveManager.h:132
bool IsLOSReturnInCooldown() const;    // 定义在 ReserveManager.h:135
```

跟踪变量会被更新：
```cpp
lastMainBorrowCycle = gcCycleCount;    // BorrowForMain 中更新
lastLOSBorrowCycle = gcCycleCount;     // BorrowForLOS 中更新
lastMainReturnCycle = gcCycleCount;    // ReclaimMainBorrowed 中更新
lastLOSReturnCycle = gcCycleCount;     // ReclaimLOSBorrowed 中更新
```

`NotifyGCCycle()` 在每次 GC 后被调用（WCollector.cpp:790），`gcCycleCount` 正常递增。
但 `Is...InCooldown()` 函数**在整个 src/ 目录中没有任何调用点**。

**影响**: 冷却机制完全无效。借入和归还没有频率控制，理论上可能在连续 OOM 场景中出现借→归→借→归的高频振荡。

**建议**: 
- 方案 A: 在 `TryBorrowFromReserveIfNeeded` 和 `ShrinkInactiveZone Phase B` / `CompactLOS Phase 7` 中加入冷却检查
- 方案 B: 如果当前设计（先 GC 归还再借入的 try=5/6 分离）已足够抑制振荡，则删除冷却相关代码以减少维护负担

### 11.2 [低] BORROW_COOLDOWN_CYCLES / RETURN_COOLDOWN_CYCLES 常量未验证

`ReserveManager.h` 中定义了：
```cpp
static constexpr size_t BORROW_COOLDOWN_CYCLES = 2;
static constexpr size_t RETURN_COOLDOWN_CYCLES = 1;
```

这些值是硬编码的，未通过运行时配置暴露，且由于冷却机制本身是死代码，从未被实际验证过其对性能的影响。

### 11.3 [低] ShrinkInactiveZone Phase A 与 Phase B 的 garbageRegionList 清理可能重叠

Phase A 的 garbage 清理条件：
```cpp
if ((gStart >= newInactive && gStart < mainEnd) ||
    (gStart >= highestLiveBorrowedEnd && hasBorrowed && highestLiveBorrowedEnd > 0))
```

Phase B 有独立的 garbage 清理：
```cpp
if (g->GetRegionStart() >= newWatermark && g->GetRegionStart() < mainWm)
```

Phase A 的条件中已经包含了部分借入区域的清理（第二个 OR 分支），可能与 Phase B 的清理存在重叠。
虽然 `DeleteRegion` 对已删除 region 再次操作是安全的（链表操作会判空），但逻辑上有冗余。

**建议**: Phase A 的 garbage 清理只处理 `[newInactive, mainEnd)` 范围，借入区域的清理完全交给 Phase B。

### 11.4 [信息] freeRegionManager 初始化覆盖 RESERVE 范围

```cpp
freeRegionManager.Initialize(mainUnitCount + reserveUnitCount);  // RegionManager.cpp:484
```

MAIN 的 freeRegionManager 容量 = mainUnitCount + reserveUnitCount，这意味着 freeRegionManager 的索引空间覆盖了整个 RESERVE 区域。这是正确的设计——借入后 AddGarbageUnits 可在整个借入范围内操作。

同样，LOS 的 losFreeMgr：
```cpp
losFreeMgr.Initialize(reserveUnitCount + losUnitCount);  // LOSManager.cpp:38
```

LOS 的 losFreeMgr 也预留了 RESERVE 空间。两边的 FreeRegionManager 都预备了最大可能的借入范围。

**注意**: 虽然两个 FreeRegionManager 的索引空间在 RESERVE 区域有重叠，但实际使用中 mainWatermark 和 losWatermark 的碰撞保护确保不会同时占用同一区域。

### 11.5 [中-高] ShrinkInactiveZone rebuild 循环扫描范围重叠（可触发 CartesianTree 碰撞告警）

**现象**: 当 MAIN 的 `inactiveZone` 越过 `mainEnd` 进入借入区域后（因为 `effectiveMainEnd = mainWatermark > mainEnd`，TakeRegion 的 `fetch_add` 把 `inactiveZone` 推到了 `mainEnd` 以上），`ShrinkInactiveZone` 的两个 rebuild 循环会出现扫描范围重叠。

```cpp
// Loop 1: Re-add MAIN free chunks below newInactive
for (addr = regionHeapStart; addr < newInactive; ...) { ... }

// Loop 2: Re-add RESERVE-borrowed free chunks below highestLiveBorrowedEnd
if (highestLiveBorrowedEnd > mainEnd) {
    for (addr = mainEnd; addr < highestLiveBorrowedEnd; ...) { ... }
}
```

当 `newInactive > mainEnd` 时（Phase A 扫描到借入区域有活 region），loop 1 会扫描到 `newInactive`，loop 2 从 `mainEnd` 开始扫描。两者的重叠范围为 `[mainEnd, min(newInactive, highestLiveBorrowedEnd))`。

**触发场景**:
```
regionHeapStart   mainEnd   live_region   newInactive   mainWatermark
      ↓              ↓         ↓              ↓              ↓
  │  MAIN 空间  │  借入区域(死,死,活,死)  │  inactive  │

Loop 1 扫描: [regionHeapStart ──────────── newInactive)
Loop 2 扫描:              [mainEnd ─── highestLiveBorrowedEnd)
                          ^^^^^^^^ 重叠 ^^^^^^^^
```

**后果**: 重叠范围中的 free chunks 被 `AddGarbageUnits` 插入两次。CartesianTree 的 `MergeInsertInternal` 检测到范围碰撞返回 `false`，`AddGarbageUnits` 的安全检查发现 `allFree=true` 后静默跳过并输出 DLOG 告警。**不会造成内存损坏**，但：
1. STW 期间浪费时间做冗余扫描和碰撞检测
2. 产生误导性的 "skip duplicate dirty units insertion" DLOG 日志
3. 代码逻辑不清晰，增加维护负担

**修复方案**: Loop 1 的上界应限为 `min(newInactive, mainEnd)`，借入区域完全交给 Loop 2：
```cpp
// Loop 1: only scan MAIN's own space
uintptr_t loop1End = std::min(newInactive, mainEnd);
for (uintptr_t addr = regionHeapStart; addr < loop1End;) { ... }

// Loop 2: scan entire borrowed zone (unchanged)
if (highestLiveBorrowedEnd > mainEnd) {
    for (uintptr_t addr = mainEnd; addr < highestLiveBorrowedEnd;) { ... }
}
```

### 11.6 [低] RETURN_COOLDOWN_CYCLES 常量已修改但注释未更新

`ReserveManager.h` 中：
```cpp
static constexpr size_t BORROW_COOLDOWN_CYCLES = 2;  // after return, wait before borrow
static constexpr size_t RETURN_COOLDOWN_CYCLES = 2;  // after borrow, wait before return
```

注释说 `RETURN_COOLDOWN_CYCLES = 1`，但实际值是 2。（参考文档第 11.2 节最初记录时值为 1，但实际代码为 2。）属于文档/注释不一致。

### 11.7 [信息] RETURN_USAGE_THRESHOLD / BORROW_USAGE_THRESHOLD 为未使用常量

```cpp
static constexpr double RETURN_USAGE_THRESHOLD = 0.6;
static constexpr double BORROW_USAGE_THRESHOLD = 0.8;
```

这两个阈值在 `ReserveManager.h` 中定义，用于"主动 RESERVE 归还"的迟滞控制（usage ratio < 0.6 才归还，> 0.8 才借入）。但与冷却机制类似，**没有任何代码引用这两个常量**。属于预留 API，当前为死代码。
