# 水线收缩机制分析与问题记录

## 1. 背景：堆空间布局与水线概念

### 堆空间三区布局

```
低地址                                                                      高地址
┌──────────────────┬───────────────────────────┬──────────────────┐
│      MAIN        │         RESERVE           │       LOS        │
│   (小/大对象)     │      (借用缓冲区)          │    (大对象)       │
│   grows →        │                           │      ← grows     │
└──────────────────┴───────────────────────────┴──────────────────┘
↑                  ↑          ↑          ↑     ↑                  ↑
regionHeapStart  mainEnd   mainWm     losWm  losStart           losEnd

MAIN 分配方向：
regionHeapStart ──[已分配+gaps]──→ inactiveZone ──[未激活]──→ effectiveMainEnd(=mainWm)
                                      ↑ fetch_add 推高

LOS 分配方向：
effectiveLOSStart(=losWm) ──[未激活]──→ losInactiveZone ──[已分配+gaps]──→ losEnd
                                              ↑ CAS 推低
```

RESERVE 空间被 MAIN 和 LOS 从两端借用：
- MAIN 从低地址（reserveStart = mainEnd）向高地址借 → `mainWatermark` 推高
- LOS 从高地址（reserveEnd = losStart）向低地址借 → `losWatermark` 推低
- 两个 watermark 之间的空间 `[mainWatermark, losWatermark)` 为 RESERVE 可用空间

- **MAIN**：小对象分配区，从低地址向高地址增长
- **RESERVE**：借用缓冲区，可被 MAIN 或 LOS 借用扩展
- **LOS**：大对象分配区，从高地址向低地址增长

### 三条水线与分配指针

#### inactiveZone（MAIN 分配前沿）

- **类型**：`std::atomic<uintptr_t>`，定义在 `RegionManager.h:762`
- **含义**：MAIN 的分配前沿。`[regionHeapStart, inactiveZone)` 为活跃区域（已分配或空闲树中），`[inactiveZone, effectiveEnd)` 为未激活区域
- **增长方式**：`TakeRegion` 中 `inactiveZone.fetch_add(size)` 向高地址推进
- **收缩**：仅在 `ShrinkInactiveZone Phase A` 中通过 `inactiveZone.store(newInactive)` 回退
- 当 MAIN 借用了 RESERVE 空间后，inactiveZone 可以越过 `mainEnd` 进入借用区域

#### losInactiveZone（LOS 分配前沿）

- **类型**：`std::atomic<uintptr_t>`，定义在 `LOSManager.h`
- **含义**：LOS 的分配前沿。LOS 从高地址向低地址增长，`[losInactiveZone, losEnd)` 为活跃区域（已分配或空闲树中），`[effectiveLOSStart, losInactiveZone)` 为未激活区域
- **初始值**：`losEnd`（最高地址，表示 LOS 初始为空）
- **增长方式**（向低地址）：`AllocFromInactiveZone` 中 `losInactiveZone.compare_exchange_weak(cur, cur - regionSize)` 向低地址推进
- **收缩方式**（向高地址）：
  - `CompactLOS` Phase 6：`losInactiveZone.store(lowestLiveStart)` — 压缩后将前沿退到最低活跃 region 的起始地址
- 当 LOS 借用了 RESERVE 空间后，losInactiveZone 可以越过 `losStart` 进入借用区域（低地址方向）

```
LOS 布局（从高到低）：
losEnd ──────────────────────────────── losStart ─── losWatermark(借用后)
   [ 活跃区域(已分配+gaps) | 未激活 |   借用区域   ]
                          ↑                ↑
                   losInactiveZone   effectiveLOSStart
```

#### LOS 分配流程（`AllocLarge`）

1. **Step 1**：尝试 `losFreeMgr`（空闲树，包含 dirtyTree 和 releaseTree 中的回收 region）
2. **Step 2**：尝试 `AllocFromInactiveZone`，CAS 推低 `losInactiveZone`
3. 若两步都失败，返回 0（分配失败），上层会触发 OOM GC 或回退到 MAIN

#### losWatermark（RESERVE-LOS 借用水线）

- **类型**：`std::atomic<uintptr_t>`，定义在 `ReserveManager.h:184`
- **含义**：LOS 从 RESERVE 借用的低水位。`[losWatermark, reserveEnd)` 已被 LOS 借走
- **初始值**：`reserveEnd`（最高地址，表示 LOS 初始未借用任何空间）
- **增长方式**（向低地址）：`CommitChunkForLOS` 中 `losWatermark.store(newLOS)` 向低地址推进
- **收缩方式**（向高地址）：`CompactLOS` 尾部通过 `ReclaimLOSBorrowed(actualWatermark)` → `losWatermark.store(newWatermark)` 归还

> **与 MAIN 的关键区别**：LOS 的水线收缩发生在 `CompactLOS` 中（压缩后自然归还），而非单独的 ShrinkInactiveZone。且 LOS 归还时保留 25% 借用空间作为 runway buffer。

#### mainWatermark（RESERVE-MAIN 借用水线）

- **类型**：`std::atomic<uintptr_t>`，定义在 `ReserveManager.h`
- **含义**：MAIN 从 RESERVE 借用的高水位。`[reserveStart, mainWatermark)` 已被 MAIN 借走
- **增长方式**：`BorrowForMain` 中 `mainWatermark.store(newMain)` 向高地址推进
- **收缩**：仅在 `ShrinkInactiveZone Phase B` 中通过 `ReclaimMainBorrowed` → `mainWatermark.store(newWatermark)` 回退

## 2. 所有水线变量的写操作

### inactiveZone 写操作（MAIN，全代码库共 4 处）

| # | 操作 | 文件 | 行号 | 方向 | 场景 |
|---|------|------|------|------|------|
| 1 | `this->inactiveZone = regionHeapStart` | RegionManager.cpp | L449 | 初始化 | 构造函数，设为堆起始地址 |
| 2 | `inactiveZone.fetch_add(size)` | RegionManager.cpp | L750 | **增长 ↑** | `TakeRegion` 分配新 region，推高前沿 |
| 3 | `inactiveZone.fetch_sub(size)` | RegionManager.cpp | L769 | **回退 ↓** | `TakeRegion` 分配超出 effectiveEnd，撤销 fetch_add |
| 4 | `inactiveZone.store(newInactive)` | RegionManager.cpp | L1943 | **收缩 ↓** | **ShrinkInactiveZone Phase A** — 唯一的真正收缩 |

> **结论**：`fetch_sub` 只是分配失败的原子撤销，不算收缩。`inactiveZone` 的真正收缩**仅发生在 `ShrinkInactiveZone` 中**。

### losInactiveZone 写操作（LOS，全代码库共 3 处）

| # | 操作 | 文件 | 行号 | 方向 | 场景 |
|---|------|------|------|------|------|
| 1 | `losInactiveZone.store(losEnd)` | LOSManager.cpp | L31 | 初始化 | 构造函数，设为 losEnd（LOS 初始为空） |
| 2 | `losInactiveZone.compare_exchange_weak(cur, cur - regionSize)` | LOSManager.cpp | L107 | **增长 ↓** | `AllocFromInactiveZone` CAS 向低地址扩展 |
| 3 | `losInactiveZone.store(lowestLiveStart)` | LOSManager.cpp | L791 | **收缩 ↑** | **CompactLOS Phase 6** — 压缩后退到最低活跃 region |

> **结论**：`losInactiveZone` 的收缩发生在 **`CompactLOS`** 中（压缩后自然回收），**不依赖 ShrinkInactiveZone**。

### mainWatermark 写操作（RESERVE→MAIN，全代码库共 3 处）

| # | 操作 | 文件 | 行号 | 方向 | 场景 |
|---|------|------|------|------|------|
| 1 | `mainWatermark.store(reserveStart)` | ReserveManager.cpp | L20 | 初始化 | 构造函数 |
| 2 | `mainWatermark.store(newMain)` | ReserveManager.cpp | L85 | **增长 ↑** | `BorrowForMain` — MAIN 借用 RESERVE 时推高 |
| 3 | `mainWatermark.store(newWatermark)` | ReserveManager.cpp | L169 | **收缩 ↓** | `ReclaimMainBorrowed` — 被 ShrinkInactiveZone Phase B 调用 |

> **结论**：`mainWatermark` 的收缩也**仅在 `ShrinkInactiveZone Phase B`** 中通过 `ReclaimMainBorrowed` 发生。

### losWatermark 写操作（RESERVE→LOS，全代码库共 3 处）

| # | 操作 | 文件 | 行号 | 方向 | 场景 |
|---|------|------|------|------|------|
| 1 | `losWatermark.store(reserveEnd)` | ReserveManager.cpp | L21 | 初始化 | 构造函数，设为 reserveEnd |
| 2 | `losWatermark.store(newLOS)` | ReserveManager.cpp | L104 | **增长 ↓** | `CommitChunkForLOS` — LOS 借用 RESERVE 时向低地址推进 |
| 3 | `losWatermark.store(newWatermark)` | ReserveManager.cpp | L197 | **收缩 ↑** | `ReclaimLOSBorrowed` — 被 CompactLOS 调用归还借用空间 |

> **结论**：`losWatermark` 的收缩发生在 **`CompactLOS`** 尾部通过 `ReclaimLOSBorrowed` 完成。归还时保留 25% 借用空间作为 runway buffer。

### 四条水线收缩机制对比

| 水线 | 收缩位置 | 触发条件 | 实际执行频率 |
|------|---------|---------|------------|
| inactiveZone (MAIN) | `ShrinkInactiveZone Phase A` | OOM GC + `!willBorrow` | **几乎不执行** |
| mainWatermark (RESERVE→MAIN) | `ShrinkInactiveZone Phase B` | OOM GC + `!willBorrow` | **几乎不执行** |
| losInactiveZone (LOS) | `CompactLOS Phase 6` | OOM GC 触发压缩 | 正常执行 |
| losWatermark (RESERVE→LOS) | `CompactLOS` 尾部 | OOM GC 触发压缩 | 正常执行（但 LOS 借不到 RESERVE） |

## 3. ShrinkInactiveZone 触发时机

### 调用位置

`ShrinkInactiveZone` 在整个代码库中仅有 **1 处调用**：

```
WCollector.cpp:987    rm.ShrinkInactiveZone();
```

### 触发条件代码（WCollector.cpp L980-L988）

```cpp
if (gcReason == GC_REASON_OOM) {
    RegionSpace& space = reinterpret_cast<RegionSpace&>(theAllocator);
    RegionManager& rm = space.GetRegionManager();
    bool willBorrow = (space.GetOOMSource() != OOMSource::NONE) &&
                      (rm.GetReserveManager() != nullptr);
    if (!willBorrow) {
        ScopedStopTheWorld stw("shrink inactive", false);
        rm.ShrinkInactiveZone();
    }
}
```

### 条件分析

必须同时满足两个条件才能执行：

1. **必须是 OOM GC**：`gcReason == GC_REASON_OOM`
2. **不会紧接着进行 RESERVE 借用**：`!willBorrow`

`willBorrow` 为 true 的条件：
- `OOMSource != NONE`（存在因分配失败触发的 OOM 源）**且**
- `reserveManager != nullptr`（有 RESERVE 区域可借）

### 关键问题：willBorrow 门控几乎总是 true

OOM GC 本身就是因为分配失败触发的，所以 `OOMSource` 几乎一定不是 `NONE`。同时只要配置了 RESERVE（`cjReserveRatio > 0`），`reserveManager` 也不为 null。

这意味着在有 RESERVE 的配置下，`willBorrow` **几乎总是 true**，`ShrinkInactiveZone` **几乎永远不被执行**。

### willBorrow 门控的设计意图

代码注释解释了跳过的原因：

> Skip entirely when proactive borrowing will follow: ShrinkInactiveZone would push inactiveZone back and release pages, but OOM-triggered proactive borrowing means space is immediately needed. The push-back + ClearTrees + rebuild + ReleaseUnits would invalidate from-space forwarding pointers that mutators and barriers still need.

即：如果 GC 后马上要借用更多 RESERVE 空间，那先收缩再借用是浪费——收缩释放的页面马上又要被用掉。但这个优化导致了 **RESERVE 借出后永远无法归还** 的副作用。

## 4. ShrinkInactiveZone 内部流程

整个方法在 STW（Stop-The-World）下执行，消除并发竞争。定义在 `RegionManager.cpp:1766`。

### Phase A：收缩 MAIN inactiveZone

1. **扫描 MAIN 活跃区域** `[regionHeapStart, min(oldInactive, mainEnd))`
   - 遍历所有 region，找到最高活跃（非 free、非 garbage）region 的 end 地址 → `highestLiveEnd`
   - 对跨越 MAIN-RESERVE 边界的大 region，cap 到 `mainEnd`

2. **Ghost routing 保护**
   - 扫描 `ghostFromRegionList` 和 `pendingGhostGarbageList`
   - 将 ghost routing 目标 region 纳入保护范围，防止释放仍被引用的 TO-space 页面
   - 目标在 MAIN 区域：提升 `highestLiveEnd`
   - 目标在借用区域：提升 `highestLiveBorrowedEnd`

3. **重建 freeRegionManager**
   - `ClearTrees()` 清空所有空闲树节点
   - 重新添加 `[regionHeapStart, newInactive)` 范围内的 free region

4. **提交 inactiveZone**
   - `inactiveZone.store(newInactive)` — 降低水线
   - 清理 `garbageRegionList` 和 `pendingGhostGarbageList` 中被回收区域的 region
   - 计算回收量 `mainReclaimed = min(oldInactive, mainEnd) - newInactive`
   - `ReleaseUnits` 释放 `[newInactive, min(oldInactive, mainEnd))` 的物理页

### Phase B：收缩 RESERVE 水线

1. **扫描借用区域** `[mainEnd, min(oldInactive, mainWatermark))`
   - 找最高活跃借用 region → `highestLiveBorrowedEnd`
   - 同样计入 ghost routing 目标保护

2. **提交 mainWatermark**
   - `newWatermark = max(highestLiveBorrowedEnd, mainEnd)`
   - 调用 `ReclaimMainBorrowed(newWatermark)` 归还空闲借用空间
   - 清理借用区域中的垃圾 region
   - 释放 `[newWatermark, oldMainWm)` 的物理页
   - 回收量通过 `ReclaimMainBorrowed` 返回值获取

### 返回值

返回 Phase A + Phase B 总回收字节数。若两个 phase 都无可回收空间，提前返回 0。

## 5. CompactLOS 触发时机与内部流程

### 调用链

CompactLOS 在每次 GC 的 `PostTrace` 阶段被调用，调用链如下：

```
PostTrace()                          // WCollector.cpp:575
  └→ CompactLOSIfNeeded()            // WCollector.cpp:681
       ├─ Gate 1: losMgr == nullptr?  → return（LOS 不存在）
       └→ CompactLOSOnOOMIfNeeded()  // WCollector.cpp:697
            ├─ Gate 2: needsCompact?  → 不满足则 return
            ├─ Gate 3: CanAllocLargeAfterGC? → 满足则 skip
            └→ STW CompactLOS()      // LOSManager.cpp:598
```

### 三个门控条件

#### Gate 1：LOS 存在性检查

```cpp
LOSManager* losMgr = space.GetRegionManager().GetLOSManager();
if (losMgr == nullptr) {
    return;
}
```

仅当配置了 LOS（`cjLOSRatio > 0`）时才继续。这个门控在正常配置下总是通过。

#### Gate 2：OOM + 需要压缩（WCollector.cpp L698-701）

```cpp
bool needsCompact = (gcReason == GC_REASON_OOM) && (hasPendingPlan || losMgr.NeedsCompaction());
if (!needsCompact) {
    return;
}
```

必须同时满足：
- **OOM GC**：`gcReason == GC_REASON_OOM`（非 OOM GC 不触发压缩）
- **需要压缩**：`hasPendingPlan`（上次已规划但未执行）**或** `NeedsCompaction()` 返回 true

#### Gate 3：#10A 跳过优化（WCollector.cpp L707-710）

```cpp
if (!hasPendingPlan && losMgr.CanAllocLargeAfterGC()) {
    DLOG(REPORT, "Skip CompactLOS: GC freed enough LOS space for allocation");
    return;
}
```

如果没有 pending plan，且常规 GC（`CollectLargeGarbage`）已经回收了足够空间满足上次失败的分配，则跳过代价高昂的 CompactLOS。

### NeedsCompaction 判断条件（LOSManager.cpp L287）

满足**任一**条件即返回 true：

| 条件 | 表达式 | 含义 |
|------|--------|------|
| 碎片率超阈值 | `GetFragmentationRatio() > 0.3` | 活跃区域中空闲单元占比 > 30% |
| 新分配失败 | `losAllocFailCount > lastCompactAllocFailCount` | 自上次压缩后出现新的 LOS 分配失败 |

**碎片率计算**（`GetFragmentationRatio`，LOSManager.cpp L272）：
```
fragRatio = (dirtyFreeUnits + releasedFreeUnits) / totalActiveUnits

totalActiveUnits = (losEnd - losInactiveZone) / UNIT_SIZE
```

即活跃区域（`[losInactiveZone, losEnd)`）中，空闲树里可用单元占总单元的比例。

### CanAllocLargeAfterGC 跳过条件（LOSManager.cpp L309）

在 `CollectLargeGarbage` 回收死亡 region 后检查，满足**任一**则跳过压缩：

| 检查 | 表达式 | 含义 |
|------|--------|------|
| 空闲树有足够连续块 | `losFreeMgr.GetMaxContiguousFreeUnits() >= neededUnits` | 最大连续空闲块能满足上次失败的分配 |
| 未激活区 runway 足够 | `(losInactiveZone - effectiveLOSStart) >= neededSize` | 还有足够的未激活空间可以扩展 |

其中 `neededSize = losLastFailedAllocSize`，在 `AllocLarge` 失败时记录（LOSManager.cpp L89）。

### CompactLOS 内部流程（水线相关部分）

完整的 CompactLOS 在 STW 下执行（`ScopedStopTheWorld stw("OOM LOS compact")`），核心步骤：

1. **Phase 1-5**：构建压缩规划 → memmove 移动对象 → 更新引用
2. **Phase 6**：重建 losFreeMgr，更新 losInactiveZone
   - 找到所有存活 region 中的最低地址 → `lowestLiveStart`
   - `losInactiveZone.store(lowestLiveStart)` — **收缩 losInactiveZone**
   - 重建空闲树，插入存活 region 之间的间隙
3. **Phase 7**：归还 RESERVE 借用空间
   - 计算安全归还边界 `fullReturnWatermark = min(lowestLiveStart, losStart)`
   - **保留 25% 借用空间**作为 runway buffer，防止借-还-借 ping-pong
   - 调用 `ReclaimLOSBorrowed(actualWatermark)` → `losWatermark.store(newWatermark)` — **收缩 losWatermark**

```
Phase 7 归还逻辑：
canReturn = fullReturnWatermark - currentLosWatermark
keepBuffer = currentBorrowed / 4     // 25% keep ratio
actualReturn = canReturn - keepBuffer // 实际归还量
```

### 与 ShrinkInactiveZone 的对比

| 维度 | ShrinkInactiveZone (MAIN) | CompactLOS (LOS) |
|------|--------------------------|------------------|
| 触发时机 | PostCopy 阶段 | PostTrace 阶段 |
| 前提条件 | OOM GC + `!willBorrow` | OOM GC + 碎片/失败 |
| 门控问题 | `willBorrow` 几乎总为 true → **几乎不执行** | 门控条件合理 → **正常执行** |
| 收缩对象 | inactiveZone + mainWatermark | losInactiveZone + losWatermark |
| 执行方式 | STW 扫描 + 重建空闲树 | STW memmove + 引用更新 + 重建空闲树 |
| RESERVE 归还 | 全量归还（但从不执行） | 保留 25% buffer 后归还 |
| 实际效果 | 0 次执行（baseline 243 cycles） | 可正常执行，但 LOS 借不到 RESERVE |

> **关键矛盾**：CompactLOS 的门控和执行机制是健康的，但它无法从根本上解决 LOS 饥饿问题，因为 RESERVE 已被 MAIN 通过 P6 几何借用全部借走。CompactLOS 只能压缩 LOS 自有空间的碎片，无法为 LOS 从 RESERVE 借入新空间（因为 RESERVE 已空）。

## 6. 实测数据：水线收缩的实际表现

### 测试配置

```bash
cjHeapSize=128mb cjMainRatio=3 cjReserveRatio=5 cjLOSRatio=2 cjEnableLOSCompact=1
```

堆空间分配：MAIN 128MB / RESERVE 64MB / LOS 25.6MB

### P6 几何借用过程

原始代码使用几何增长借用（25% → 50% → 100% of remaining）：

| OOM Cycle | 借用量 | RESERVE 剩余 | 累计 mainBorrowed |
|-----------|--------|-------------|------------------|
| 1 | 16 MB (25%) | 50 MB | 16 MB |
| 2 | 25 MB (50% of 50MB) | 25 MB | 41 MB |
| 3 | 25 MB (100% of 25MB) | **0 MB** | **64 MB** |

**仅 3 个 OOM cycle 即耗尽全部 RESERVE，LOS 获得 0 字节。**

### ShrinkInactiveZone 执行情况

从 baseline 日志（243 个 GC cycle）中搜索：

- `ShrinkInactiveZone Phase A` 日志：**0 次**
- `ShrinkInactiveZone Phase B` 日志：**0 次**
- `ShrinkInactiveZone` 相关日志：**0 次**

原因：每次 OOM GC 中 `willBorrow = true`，ShrinkInactiveZone 被完全跳过。

### 最终堆状态

```
RESERVE: capacity 67,108,864 B
         available 0 B
         mainBorrowed 67,108,864 B (100%)
         losBorrowed 0 B (0%)
         mainBorrowCount 3

MAIN: [0x109500000, 0x111500000)
      inactiveZone 0x10fb60000
      large-object regions 21: 5460 units (89,456,640 B = 85.3 MB)

LOS:  capacity 26,836,992 B
      used 25,264,128 B (94%)
      fail 1375, fallback 1067
      reserveBorrow 0
```

### 数据解读

1. MAIN 独占了全部 64MB RESERVE 空间，LOS 一个字节都没借到
2. MAIN 中有 21 个大对象 region，占用 85.3MB（MAIN 总空间 192MB 的 44.4%）
3. LOS 有 1375 次分配失败、1067 次回退到 MAIN，but RESERVE borrow = 0
4. ShrinkInactiveZone 从未执行，借出的空间永远无法归还

## 7. 问题总结与改进方向

### 核心问题

```
借用通路（BorrowForMain）和归还通路（ShrinkInactiveZone）存在严重不对称：
- 借用：每次 OOM GC 都可触发，3 次即借完全部 RESERVE
- 归还：被 willBorrow 门控阻断，在有 RESERVE 的配置下几乎不执行
```

这导致 RESERVE 是一个**单向阀**：一旦借出，永远不会归还。

### 问题影响

1. **LOS 饥饿**：MAIN 借走全部 RESERVE，LOS 无法扩展，大对象分配失败后只能回退到 MAIN
2. **大对象在 MAIN 中堆积**：MAIN 中大对象使用 128KB 对齐，比 LOS 的 16KB 对齐浪费更多空间
3. **内存利用率下降**：借用区域中即使有空闲空间也无法被其他区域使用

### 可能的改进方向

#### 方向 A：让 ShrinkInactiveZone 更容易执行

修改 willBorrow 门控条件，例如：
- 当 RESERVE 已借出超过 N%（如 50%）时，即使 willBorrow=true 也执行收缩
- 或者在非 OOM GC 中（如定期 GC）也执行轻量级收缩

#### 方向 B：限制 P6 借用上限

在 `CopyCollector` 的 P6 借用逻辑中加上限：
- 不允许 mainBorrowed 超过 RESERVE 容量的 N%
- 为 LOS 预留一定比例的 RESERVE 空间

#### 方向 C：将 MAIN 大对象迁移到 LOS

在 ShrinkInactiveZone 时额外执行 STW 迁移：
- 提取 MAIN 中的大对象 region
- 在 LOS（含 RESERVE 借用）中分配目标空间
- memcpy 数据并更新所有引用
- 回收 MAIN 中的原始 region

#### 方向 D：借+还在同一 GC cycle 内完成

在 OOM GC 的 STW 阶段：先做 ShrinkInactiveZone 归还空间，再做 P6 借用。
避免 "先借后收缩" 的冲突。

### 优先级建议

方向 B（限制借用上限）+ 方向 C（大对象迁移）组合是最有效的：
- B 防止 RESERVE 被单方面耗尽
- C 减少 MAIN 中的大对象占用，释放空间给小对象
