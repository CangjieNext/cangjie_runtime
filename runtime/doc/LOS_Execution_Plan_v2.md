# LOS 落地执行方案（可逐步实施）

## 0. 目标与边界

目标：在不破坏当前 `WCollector + RegionSpace` 主体架构下，新增可控的 LOS（Large Object Space）能力，支持：

1. 堆空间三段划分：`MAIN | RESERVE | LOS`，各段独立管理；
2. 大对象优先进入 LOS；LOS 不足时可降级到现有大对象路径；
3. RESERVE 作为弹性缓冲区，MAIN 和 LOS 可按需借还；
4. GC 后可尝试把"主堆大对象"回迁 LOS；
5. 全流程可观测、可开关、可回退。

边界：

- 第一阶段不做并发整理，仅做 STW 整理能力预留；
- 第一阶段不改变小对象路径；
- 通过比例设置保证可 100% 回退到当前行为（`cjLOSRatio=0` 且 `cjReserveRatio=0` 时全部空间归 MAIN）。

---

## A. 堆内存布局

```
regionHeapStart                                                       regionHeapEnd
    |                                |                  |                    |
    v                                v                  v                    v
    [         MAIN Space            ][  RESERVE Zone   ][     LOS Space      ]
    |<------ ~60% heapSize -------->||<---- ~30% ----->||<----- ~10% ------->|
                                     ^                  ^
                                reserveStart         losStart
```

默认比例 **6:3:1**（MAIN:RESERVE:LOS），可通过环境变量 `cjMainRatio` / `cjReserveRatio` / `cjLOSRatio` 自定义。

- MAIN 从低地址向高地址增长（现有 `inactiveZone` 机制）。
- LOS 从高地址端使用，分配时优先从 LOS 自己的 `dirtyTree/releaseTree` 取回收区域，其次从 `losInactiveZone` 向低地址扩展。
- RESERVE 是中间缓冲区，MAIN 和 LOS 可按需向 RESERVE 借空间；无人借时 RESERVE 不参与分配。
- `cjLOSRatio=0` 且 `cjReserveRatio=0` 时，全部空间留给 MAIN，行为完全等同当前版本（**关闭 LOS 功能**）。

---

## B. 核心管理器架构

### B.1 `LOSManager`（管理 LOS 空间）

```
LOSManager
├── FreeRegionManager losFreeMgr       // 独立的 dirtyTree + releaseTree（unit index 限制在 LOS 范围）
├── RegionList losOldLargeRegionList   // 老生代 LOS 大对象
├── RegionList losRecentLargeRegionList// 新近分配的 LOS 大对象
├── RegionCache losTraceRegions        // trace 阶段分配的大对象
├── uintptr_t losStart / losEnd        // LOS 空间地址边界
├── std::atomic<uintptr_t> losInactiveZone  // LOS 内未激活分界线
├── size_t losUnitStart / losUnitEnd   // 全局 unit index 范围
│
├── AllocLarge(size) -> uintptr_t      // 在 LOS 中分配大对象 region
├── CollectLOSGarbage() -> size_t      // 标记-清扫回收
├── AssembleLOSGarbageCandidates()     // GC 前组装回收候选
├── ReclaimLOSGarbageRegions()         // 回收 region 到 losFreeMgr
├── CompactLOS()                       // STW 原地整理
├── ShrinkToReserve(size) -> size_t    // 缩容：整理后归还 RESERVE
├── ExpandFromReserve(size) -> bool    // 扩容：从 RESERVE 借空间
│
└── 统计字段
    ├── losSpaceSize / losGarbageSize
    ├── losAllocFailCount / losFallbackCount
    └── losMigrateInCount
```

**复用关系**：

- `FreeRegionManager` → 新建实例，构造时传入 `LOSManager` 的引用（替代 `RegionManager` 引用）。
- `CartesianTree` → 完全复用，`losFreeMgr` 的 dirtyTree/releaseTree 索引对应 LOS 区域的 unit range。
- `RegionList / RegionCache` → 新建实例，类型定义复用。
- `RegionInfo` → 不改；LOS region 仍然是 `LARGE_REGION` / `RECENT_LARGE_REGION`，通过地址范围判定来源。

### B.2 `ReserveManager`（管理预留区）

```
ReserveManager
├── uintptr_t reserveStart / reserveEnd   // 预留区地址边界
├── std::atomic<uintptr_t> mainBorrow     // MAIN 侧已借走的高水位（从 reserveStart 向上）
├── std::atomic<uintptr_t> losBorrow      // LOS 侧已借走的低水位（从 reserveEnd 向下）
├── std::mutex borrowMutex                // 借还互斥
│
├── BorrowForMain(unitCount) -> uintptr_t // MAIN 向 RESERVE 借空间
├── BorrowForLOS(unitCount) -> uintptr_t  // LOS 向 RESERVE 借空间
├── ReturnFromMain(unitIdx, cnt)          // MAIN 归还（GC 回收后）
├── ReturnFromLOS(unitIdx, cnt)           // LOS 归还（整理/缩容后）
├── GetAvailable() -> size_t              // 剩余可借空间
├── RequestLOSShrink()                    // 通知 LOS 缩容
│
└── 先占先得逻辑
    ├── MAIN 分不出 → TakeRegion 失败 → BorrowForMain
    ├── LOS 分不出 → AllocLarge 失败 → BorrowForLOS → 再不行触发 CompactLOS
    └── RESERVE 空 + MAIN 紧张 → RequestLOSShrink
```

### B.3 `RegionManager`（保持现有职责，管理 MAIN 空间）

改动最小化：

```
RegionManager（现有）
├── FreeRegionManager freeRegionManager  // 不变，作用域限制在 MAIN unit range
├── 现有全部 region lists               // 不变
├── LOSManager* losManager               // 新增：持有 LOS 管理器指针（cjLOSRatio=0 时为 nullptr）
├── ReserveManager* reserveManager       // 新增：持有 Reserve 管理器指针（cjReserveRatio=0 时为 nullptr）
│
├── Initialize() 中划分三段空间，创建 LOSManager / ReserveManager
├── TakeRegion() 中 MAIN 空间不足时尝试 reserveManager->BorrowForMain()
├── AllocLarge() 改为：
│   ├── if (losManager != nullptr) losManager->AllocLarge(size)
│   ├── 失败 → 现有 AllocLarge（降级到 MAIN 大对象路径）
│   └── 再失败 → reserveManager->BorrowForMain + 重试
└── CollectLargeGarbage() 后增加迁移窗口（M4）
```

### B.4 与 LOS.md 设计的对应关系

| LOS.md 描述                                | 架构映射                                                           |
| ------------------------------------------ | ------------------------------------------------------------------ |
| "主空间维持原状"                           | `RegionManager` 保持现有逻辑不变                                   |
| "预留10%空间为LOS，预留20%空间为预留区"    | `RegionSpace::Init()` 按比例划分，默认 6:3:1（可通过环境变量调整） |
| "各自使用各自的空间"                       | LOSManager 有自己的 FreeRegionManager                              |
| "dirtyTree与releaseTree单独实例"           | LOSManager 内部的 `losFreeMgr`                                     |
| "先占先得"                                 | ReserveManager 的 BorrowForMain / BorrowForLOS                     |
| "降级到主空间分配，沿用当前大对象分配逻辑" | AllocLarge 先走 LOS，失败走 RegionManager 原路径                   |
| "GC后主堆大对象迁移"                       | M4 迁移窗口                                                        |
| "缩容从靠近主空间处进行"                   | LOS 缩容时从靠近 RESERVE 侧归还                                    |
| "整理应该向堆底靠拢"                       | CompactLOS 线性紧凑向 losStart 方向                                |

---

## 1. 里程碑与顺序（建议严格按序）
∏
### M1：空间划分 + LOSManager / ReserveManager 骨架（只搭框架，不切流量）

**改动点**

- 新增文件：
  - `src/Heap/Allocator/LOSManager.h` / `src/Heap/Allocator/LOSManager.cpp`
  - `src/Heap/Allocator/ReserveManager.h` / `src/Heap/Allocator/ReserveManager.cpp`
- 新增配置项（环境变量）：
  - `cjMainRatio`（MAIN 占堆比例，整数，默认 6）
  - `cjReserveRatio`（RESERVE 占堆比例，整数，默认 3）
  - `cjLOSRatio`（LOS 占堆比例，整数，默认 1）
  - 比例计算：`mainSize = heapSize * cjMainRatio / (cjMainRatio + cjReserveRatio + cjLOSRatio)`
  - **关闭 LOS**：设置 `cjLOSRatio=0` 且 `cjReserveRatio=0`（全部空间归 MAIN）
- `RegionSpace::Init()` 中按比例划分堆空间为 MAIN / RESERVE / LOS 三段。
- `RegionManager::Initialize()` 中：
  - `cjLOSRatio > 0` 时创建 `LOSManager`；
  - `cjReserveRatio > 0` 时创建 `ReserveManager`；
  - 两者都为 0 时不创建（nullptr），全部空间归 MAIN。
- LOSManager 初始化自己的 `FreeRegionManager`（独立的 dirtyTree + releaseTree）。
- ReserveManager 初始化边界与原子水位。
- 不切流量：所有分配仍走现有路径。

**验收标准**

- `cjLOSRatio=0 cjReserveRatio=0` 时行为完全等同当前版本（日志和统计无变化）。
- 默认配置（6:3:1）可正常构建与启动，日志可见三段空间边界信息。
- LOSManager / ReserveManager 构造和初始化无崩溃。

**回退策略**

- `cjLOSRatio=0 cjReserveRatio=0` 即回退，零开销。

---

### M2：大对象分配分流（LOS 优先 + 降级 + RESERVE 借用）

#### 分配流程图

```
大对象分配请求 (size >= largeObjectThreshold)
             │
             ▼
┌─────────────────────────────────────┐
│ 1. LOS losFreeMgr 获取              │  ← 优先从 LOS 已回收的 dirtyTree/releaseTree 取
│    (dirtyTree → releaseTree)        │
└─────────────────────────────────────┘
             │ 失败
             ▼
┌─────────────────────────────────────┐
│ 2. LOS losInactiveZone 后移         │  ← LOS 未激活区域向低地址扩展
│    (losInactiveZone -= regionSize)   │
└─────────────────────────────────────┘
             │ 失败 (losInactiveZone <= losStart)
             ▼
┌─────────────────────────────────────┐
│ 3. RESERVE 借用（给 LOS）            │  ← reserveManager->BorrowForLOS()
│    (losBorrow += regionSize)         │
└─────────────────────────────────────┘
             │ 失败 (RESERVE 已被借尽)
             ▼
┌─────────────────────────────────────┐
│ 4. 降级到 MAIN 大对象路径            │  ← 现有 RegionManager::AllocLargeInMain()
│    (触发 LOS 异步整理信号)            │     4a. freeRegionManager 获取
│                                     │     4b. inactiveZone 后移
│                                     │     4c. RESERVE 借用（给 MAIN）
└─────────────────────────────────────┘
             │ 失败 (MAIN + RESERVE 都分不出)
             ▼
┌─────────────────────────────────────┐
│ 5. STW 立即整理 LOS                  │  ← LOSManager::CompactLOS()
│    - 紧凑存活对象                    │     整理后归还部分空间给 RESERVE
│    - 归还空闲尾部给 RESERVE           │
│    - 重试分配                        │
└─────────────────────────────────────┘
             │ 仍失败
             ▼
┌─────────────────────────────────────┐
│ 6. STW 整理 MAIN 空间                │  ← RegionManager::CompactMainLargeRegions()
│    - 紧凑大对象 region               │     整理后归还部分空间给 RESERVE
│    - 归还空闲高地址端给 RESERVE       │
│    - 重试分配                        │
└─────────────────────────────────────┘
             │ 仍失败
             ▼
┌─────────────────────────────────────┐
│ 7. 抛出 OOM                         │
└─────────────────────────────────────┘
```

#### 详细步骤说明

**步骤 1：LOS losFreeMgr 获取**

```cpp
uintptr_t LOSManager::AllocLarge(size_t size) {
    size_t unitCount = (size + UNIT_SIZE - 1) / UNIT_SIZE;
    // 优先从已回收区域获取
    RegionInfo* region = losFreeMgr.TakeRegion(unitCount, UnitRole::LARGE_SIZED_UNITS, false);
    if (region != nullptr) {
        return region->Alloc(size);
    }
    // 继续步骤 2...
}
```

**步骤 2：LOS losInactiveZone 后移**

```cpp
// LOS 从高地址向低地址增长
uintptr_t newZone = losInactiveZone.fetch_sub(regionSize);
if (newZone - regionSize >= losStart) {
    RegionInfo* region = RegionInfo::InitRegionAt(newZone - regionSize, unitCount, UnitRole::LARGE_SIZED_UNITS);
    return region->Alloc(size);
}
// 回退并继续步骤 3
losInactiveZone.fetch_add(regionSize);
```

**步骤 3：RESERVE 借用**

```cpp
uintptr_t addr = reserveManager->BorrowForLOS(unitCount);
if (addr != 0) {
    RegionInfo* region = RegionInfo::InitRegionAt(addr, unitCount, UnitRole::LARGE_SIZED_UNITS);
    // 记录这是借来的空间，后续缩容时需归还
    return region->Alloc(size);
}
```

**步骤 4：降级到 MAIN 大对象路径**

```cpp
// 降级分配（包含 MAIN 借用 RESERVE）
uintptr_t addr = regionManager->AllocLargeInMain(size);  // 步骤 4a: freeRegionManager
if (addr == 0) {
    addr = regionManager->AllocLargeFromInactiveZone(size);  // 步骤 4b: inactiveZone
}
if (addr == 0) {
    addr = reserveManager->BorrowForMain(size);  // 步骤 4c: RESERVE 借用
}
if (addr != 0) {
    // ⚠️ 触发 LOS 异步整理信号
    losManager->SignalCompactionNeeded();
    losAllocFailCount++;
    losFallbackCount++;
    return addr;
}
```

> **关键设计**：
>
> - 降级成功意味着 LOS 空间已碎片化，应触发后台整理
> - MAIN 分配也会尝试从 RESERVE 借用空间（步骤 4c）
> - 整理期间所有大对象分配直接走 MAIN 路径，避免阻塞 mutator

**步骤 5：STW 立即整理 LOS**

```cpp
// 此时 MAIN 也分配失败，必须立即整理
{
    ScopedSTW stw;  // 停止所有 mutator
    losManager->CompactLOS();
    // 整理后尝试归还部分空间给 RESERVE
    losManager->ShrinkToReserve(targetShrinkSize);
}
// 重试
return losManager->AllocLarge(size);
```

**步骤 6：STW 整理 MAIN 空间**

```cpp
// LOS 整理后仍分配失败，整理 MAIN 大对象区域
{
    ScopedSTW stw;
    // 触发 MAIN 大对象压缩
    size_t freedSize = regionManager->CompactMainLargeRegions();
    // 归还部分空间给 RESERVE
    if (freedSize > MIN_SHRINK_SIZE) {
        reserveManager->ReturnFromMain(freedSize * SHRINK_RATIO);
    }
}
// 最后重试（先尝试 LOS，再尝试 MAIN）
uintptr_t addr = losManager->AllocLarge(size);
if (addr == 0) {
    addr = regionManager->AllocLargeInMain(size);
}
return addr;
```

> **MAIN 整理方向**：MAIN 大对象向低地址靠拢，释放高地址端空闲空间给 RESERVE。

**步骤 7：OOM**

```cpp
VLOG(REPORT, "Cannot allocate large object of %zu bytes", size);
ExceptionManager::OutOfMemory();
return 0;
```

#### 整理期间的分配策略

当 `losManager->IsCompacting()` 为 true 时：

- 所有大对象分配**直接走 MAIN 路径**，跳过步骤 1-3
- 避免整理期间的并发复杂度
- 整理完成后恢复正常流程

```cpp
uintptr_t AllocLarge(size_t size) {
    if (losManager != nullptr && !losManager->IsCompacting()) {
        uintptr_t addr = losManager->AllocLarge(size);  // 步骤 1-3
        if (addr != 0) return addr;
    }
    // 步骤 4：降级或整理期间直接走 MAIN
    return AllocLargeInMain(size);
}
```

#### 归还空间策略

**整理后归还条件**：

- LOS 整理后，若空闲率 > 30%，归还 20% 给 RESERVE
- MAIN 整理后，若空闲率 > 30%，归还 20% 给 RESERVE
- 归还时从靠近 RESERVE 边界处开始（LOS 从低地址端，MAIN 从高地址端）

```cpp
void LOSManager::ShrinkToReserve(size_t targetSize) {
    // 只有整理后才能缩容（对象已紧凑到高地址端）
    size_t freeAtLowEnd = losInactiveZone - losStart;
    size_t shrinkSize = std::min(targetSize, freeAtLowEnd);
    if (shrinkSize > 0) {
        losStart += shrinkSize;
        reserveManager->ReturnFromLOS(shrinkSize / UNIT_SIZE);
    }
}
```

#### 改动点

- `LOSManager::AllocLarge()` 实现完整的 7 步分配流程
- `LOSManager::SignalCompactionNeeded()` 设置整理信号，后台线程响应
- `LOSManager::IsCompacting()` 查询整理状态
- `LOSManager::CompactLOS()` STW 原地整理（详见 M5）
- `LOSManager::ShrinkToReserve()` 缩容归还 RESERVE
- `RegionManager::AllocLarge()` 增加降级和整理触发逻辑
- `RegionManager::CompactMainLargeRegions()` 主堆大对象压缩（可选，首版可不实现）
- `ReserveManager::BorrowForLOS()` / `ReturnFromLOS()` 借还接口
- 通过地址范围判定对象来源（`IsInLOS(addr)` / `IsInMain(addr)`），无需改对象头

#### 统计字段（M2 新增）

| 字段                      | 含义                                   |
| ------------------------- | -------------------------------------- |
| `losAllocCount`           | LOS 分配成功次数                       |
| `losAllocFailCount`       | LOS 本地分配失败次数（触发降级或借用） |
| `losFallbackCount`        | 降级到 MAIN 的次数                     |
| `losReserveBorrowCount`   | 从 RESERVE 借用次数                    |
| `losCompactTriggerCount`  | 触发整理的次数                         |
| `losShrinkToReserveBytes` | 归还给 RESERVE 的字节数                |

**验收标准**

- 大对象压力下，可见 LOS 分配成功率和降级次数统计。
- LOS 满时分配可稳定降级，无崩溃、无死锁。
- RESERVE 借用可正常工作。
- 降级成功时可见整理信号触发日志。
- 整理后可见空间归还 RESERVE 的日志。

**回退策略**

- 保留原路径完整代码；LOS 分配异常时直接降级。
- 整理失败时保持现状，继续尝试 MAIN 分配。
- `cjLOSRatio=0 cjReserveRatio=0` 时全部走现有路径。

---

### M3：LOS 回收（标记-清扫）+ GC 后回迁（主堆大对象 → LOS）

**改动点**

#### 3.1 LOS 回收（标记-清扫）

- `LOSManager` 实现：
  - `AssembleLOSGarbageCandidates()`：合并 recent → old，清除 live info。
  - `CollectLOSGarbage()`：遍历 `losOldLargeRegionList`，未标记对象回收到 `losFreeMgr`。
  - `ReclaimLOSGarbageRegions()`：释放物理内存（>4MB 的 release，其他走 dirty）。
- 在 `WCollector::PostTrace()` 后：
  - 先调用现有 `CollectLargeGarbage()`（MAIN 大对象）。
  - 再调用 `losManager->CollectLOSGarbage()`（LOS 大对象）。

#### 3.2 GC 后回迁（主堆大对象 → LOS）

- 在 `PostTrace` 或 `Forward` 之后新增"回迁窗口"：
  - 遍历 `oldLargeRegionList`（MAIN 大对象），筛选可迁移条件（**无 raw-pointer、无 pin**）；
  - 尝试 `losManager->AllocLarge(size)` 并复制对象数据；
  - 成功后更新引用（复用现有 forward/route 基础设施）；
  - 原 MAIN region 回收到 `freeRegionManager`。
- 控制每轮迁移预算（按字节或对象数），避免长停顿。

#### 统计字段（M3 新增）

| 字段                | 含义                        |
| ------------------- | --------------------------- |
| `losSpaceSize`      | LOS 当前占用空间            |
| `losGarbageSize`    | 本轮 GC 回收的 LOS 垃圾大小 |
| `losMigrateInCount` | 从 MAIN 迁入 LOS 的对象数   |
| `losMigrateInBytes` | 从 MAIN 迁入 LOS 的字节数   |

**验收标准**

- GC 后 LOS 垃圾可回收，`losFreeMgr` 空闲空间增长。
- 可观测到 `losMigrateInCount` 增长。
- 回迁后对象可达性正确，功能测试稳定。
- LOS 统计随压力变化符合预期。

**回退策略**

- `losManager == nullptr` 时不执行 LOS 回收/回迁分支，不影响主堆回收链路。
- 回迁失败保持原地对象，不影响正确性。

---

### M4：LOS / MAIN 空间整理（Sliding Compaction）

#### LOS 整理触发机制

```
┌──────────────────────────────────────────────────────────────────┐
│                        整理触发条件                               │
├──────────────────────────────────────────────────────────────────┤
│ 触发源                    │ 整理方式    │ 说明                    │
├──────────────────────────────────────────────────────────────────┤
│ LOS 分配失败 + 降级成功   │ 异步整理    │ gc-helper 线程执行      │
│ 定时检查碎片率 > 阈值     │ 异步整理    │ gc-helper 线程执行      │
│ MAIN 也分配失败           │ STW 同步整理│ 立即整理，阻塞 mutator  │
└──────────────────────────────────────────────────────────────────┘
```

#### 异步整理流程

```
大对象分配 → LOS 分配失败 → 降级到 MAIN 成功
                                │
                                ▼
                    ┌─────────────────────────┐
                    │ losManager->SignalCompactionNeeded() │
                    └─────────────────────────┘
                                │
                                ▼
                    ┌─────────────────────────┐
                    │ gc-helper 线程检测到信号 │
                    │ 或定时检查碎片率 > 阈值  │
                    └─────────────────────────┘
                                │
                                ▼
                    ┌─────────────────────────┐
                    │ 设置 isCompacting = true │
                    │ （此后大对象分配直接走 MAIN）│
                    └─────────────────────────┘
                                │
                                ▼
                    ┌─────────────────────────┐
                    │ 请求 STW（短暂停顿）     │
                    │ 执行 CompactLOS()        │
                    │ 归还空间给 RESERVE       │
                    └─────────────────────────┘
                                │
                                ▼
                    ┌─────────────────────────┐
                    │ 设置 isCompacting = false│
                    │ （恢复 LOS 分配路径）    │
                    └─────────────────────────┘
```

#### 同步整理流程（紧急）

```
大对象分配 → LOS 分配失败 → 降级到 MAIN 也失败
                                │
                                ▼
                    ┌─────────────────────────┐
                    │ STW 立即整理 LOS         │
                    │ CompactLOS()             │
                    │ 归还空间给 RESERVE       │
                    └─────────────────────────┘
                                │
                                ▼
                          重试分配
                                │
                    ┌───────────┴───────────┐
                    │ 成功                   │ 失败
                    ▼                        ▼
                  返回                 STW 整理 MAIN
                                             │
                                             ▼
                                       重试分配
                                             │
                                    ┌────────┴────────┐
                                    │ 成功            │ 失败
                                    ▼                 ▼
                                  返回              OOM
```

#### 整理方向与策略

**整理方向**：向高地址（尾部）靠拢

```
整理前：
losStart                                            losInactiveZone    losEnd
    |     [obj1]  [   ]  [obj2]  [PIN]  [obj3]  [   ]  [obj4]     |         |
    v                                                              v         v
    低地址 -------------------------------------------------> 高地址

整理后：
losStart      newLosInactiveZone                                   losEnd
    |    [空闲区域]  [PIN]  [obj1] [obj2] [obj3] [obj4]            |         |
    v                 ^                                             v         v
    低地址 -----------│------------------------------------------> 高地址
                      │
               PIN 对象不动，可移动对象向高地址紧凑
```

#### 原地整理算法（滑动整理法 - Sliding Compaction）

**核心思想**：从高地址向低地址扫描，把可移动对象"滑动"到高地址端紧凑排列。

```cpp
void LOSManager::CompactLOS() {
    // 阶段 1：扫描并收集信息
    std::vector<RegionInfo*> movableRegions;      // 可移动对象
    std::vector<RegionInfo*> pinnedRegions;       // 不可移动对象（rawpointer）
    uintptr_t highestPinned = losStart;           // 最高地址的 pinned 对象

    for (RegionInfo* r : losOldLargeRegionList) {
        if (r->HasRawPointer()) {
            pinnedRegions.push_back(r);
            highestPinned = std::max(highestPinned, r->GetRegionEnd());
        } else {
            movableRegions.push_back(r);
        }
    }

    // 阶段 2：计算目标地址（从 losEnd 向低地址分配）
    // 策略：只整理 highestPinned 以上的区域，pinned 对象作为"障碍"
    uintptr_t cursor = losEnd;  // 从最高地址开始
    std::map<RegionInfo*, uintptr_t> targetAddrs;

    // 按原地址从高到低排序
    std::sort(movableRegions.begin(), movableRegions.end(),
              [](RegionInfo* a, RegionInfo* b) {
                  return a->GetRegionStart() > b->GetRegionStart();
              });

    for (RegionInfo* r : movableRegions) {
        size_t size = r->GetRegionSize();
        cursor -= size;
        // 确保不会覆盖 pinned 对象
        if (cursor < highestPinned) {
            cursor = highestPinned;  // 碰到 pinned 障碍，停止
            break;
        }
        targetAddrs[r] = cursor;
    }

    // 阶段 3：构建 forwarding table（对象旧地址 → 新地址）
    // 用于更新引用

    // 阶段 4：STW 更新引用
    // 扫描全堆（roots + 所有存活对象），更新指向 LOS 的引用
    UpdateAllReferences(targetAddrs);

    // 阶段 5：移动对象（从高地址向低地址，避免覆盖）
    for (auto& [region, target] : targetAddrs) {
        if (target != region->GetRegionStart()) {
            MoveRegion(region, target);
        }
    }

    // 阶段 6：更新分配指针，释放低地址空闲区域
    uintptr_t lowestLive = cursor;
    for (auto& r : pinnedRegions) {
        lowestLive = std::min(lowestLive, r->GetRegionStart());
    }

    // 分配指针回退到最低存活对象位置
    losInactiveZone = lowestLive;

    // 阶段 7：归还低地址空闲空间给 RESERVE
    size_t freeAtLowEnd = losInactiveZone - losStart;
    if (freeAtLowEnd > MIN_SHRINK_SIZE) {
        ShrinkToReserve(freeAtLowEnd * SHRINK_RATIO);  // 归还一部分
    }
}
```

#### Pinned 对象处理策略

| 策略               | 描述                                         | 优点         | 缺点               |
| ------------------ | -------------------------------------------- | ------------ | ------------------ |
| **策略 A（推荐）** | 找到最高地址的 pinned 对象，只整理其上方空间 | 简单可靠     | 低地址空间无法整理 |
| **策略 B**         | 把可移动对象填充到 pinned 对象之间的空隙     | 空间利用率高 | 实现复杂           |
| **策略 C**         | 先把所有可移动对象复制到临时区域，再回填     | 无覆盖风险   | 需要额外内存       |

**首版采用策略 A**：以最高地址的 pinned 对象为界限，只整理其上方的可移动对象。

#### 引用更新机制

```cpp
void LOSManager::UpdateAllReferences(const std::map<RegionInfo*, uintptr_t>& targetAddrs) {
    // 构建 forwarding table
    ForwardingTable fwdTable;
    for (auto& [region, target] : targetAddrs) {
        uintptr_t oldStart = region->GetRegionStart();
        uintptr_t newStart = target;
        if (oldStart != newStart) {
            // 对于每个对象（大对象 region 只有一个对象）
            fwdTable.Insert(oldStart + HEADER_SIZE, newStart + HEADER_SIZE);
        }
    }

    // 扫描 roots
    ScanRoots([&fwdTable](MAddress* slot) {
        MAddress old = *slot;
        if (IsInLOS(old)) {
            MAddress forwarded = fwdTable.Lookup(old);
            if (forwarded != 0) {
                *slot = forwarded;
            }
        }
    });

    // 扫描全堆对象的引用字段
    Heap::GetHeap().ForEachObject([&fwdTable](BaseObject* obj) {
        obj->VisitReferences([&fwdTable](MAddress* slot) {
            MAddress old = *slot;
            if (IsInLOS(old)) {
                MAddress forwarded = fwdTable.Lookup(old);
                if (forwarded != 0) {
                    *slot = forwarded;
                }
            }
        });
    });
}
```

#### gc-helper 线程整合

```cpp
// gc-helper 线程主循环
void GCHelperThread::Run() {
    while (!shouldStop) {
        // 检查整理信号
        if (losManager->NeedsCompaction()) {
            PerformAsyncCompaction();
        }

        // 定时检查碎片率
        if (ShouldCheckFragmentation()) {
            double fragRatio = losManager->GetFragmentationRatio();
            if (fragRatio > FRAG_THRESHOLD) {
                losManager->SignalCompactionNeeded();
            }
        }

        WaitForSignalOrTimeout(CHECK_INTERVAL);
    }
}

void GCHelperThread::PerformAsyncCompaction() {
    losManager->SetCompacting(true);  // 大对象分配走 MAIN

    // 请求 STW 执行整理
    Heap::GetHeap().RequestSTW([this]() {
        losManager->CompactLOS();
    });

    losManager->SetCompacting(false);  // 恢复 LOS 分配
}
```

#### 改动点

- `LOSManager::CompactLOS()` 实现滑动整理算法
- `LOSManager::SignalCompactionNeeded()` 设置异步整理信号
- `LOSManager::NeedsCompaction()` 查询整理信号
- `LOSManager::IsCompacting()` / `SetCompacting()` 整理状态管理
- `LOSManager::GetFragmentationRatio()` 计算碎片率
- `LOSManager::UpdateAllReferences()` 更新全堆引用
- `LOSManager::MoveRegion()` 移动 region 数据
- `LOSManager::ShrinkToReserve()` 整理后归还空间
- `GCHelperThread` 增加 LOS 整理检查逻辑
- `RegionInfo::HasRawPointer()` 判断是否有 rawpointer 引用

#### 统计字段（M4 新增）

| 字段                    | 含义                 |
| ----------------------- | -------------------- |
| `losAsyncCompactCount`  | 异步整理次数         |
| `losSyncCompactCount`   | 同步整理次数（紧急） |
| `losCompactMovedBytes`  | 整理移动的字节数     |
| `losCompactTimeMs`      | 整理耗时（毫秒）     |
| `losFragmentationRatio` | 当前碎片率           |

**验收标准**

- 异步整理时大对象分配正常走 MAIN，无阻塞。
- STW 整理后 `losInactiveZone` 正确回退，低地址空闲区域可见。
- 引用更新正确，整理后对象可达性正确。
- Pinned 对象整理前后位置不变。
- 整理后可见空间归还 RESERVE 的日志。

**回退策略**

- 整理开关 `cjEnableLOSCompaction`（默认 1）可关闭整理功能。
- 整理失败时保持现状，继续尝试 MAIN 分配。

---

#### MAIN 空间整理（Sliding Compaction）

当 MAIN 分配失败且 RESERVE 无可用空间时，触发 MAIN 空间整理，将碎片化的已用区域向低地址端滑动，腾出高地址端连续空间归还给 RESERVE。

**触发条件**

1. **同步触发（STW）**：M2 步骤 6——MAIN 分配失败 + RESERVE 借用失败 + LOSCompaction 后仍失败
2. **异步触发（gc-helper）**：MAIN 空间利用率低于阈值（如 `mainUsed / mainCapacity < 0.6`）时，在 GC 收集后由 gc-helper 后台执行

**整理算法（与 LOS 相反方向）**

```
MAIN 空间（低地址 → 高地址方向）:

整理前:
[A][_][B][_][_][C][D][_][E][_][_][_]   ← 多个空洞
  ↓  滑动整理（向低地址方向压缩）

整理后:
[A][B][C][D][E][_][_][_][_][_][_][_]   ← 高地址端连续空闲
                 │←─ 归还 RESERVE ─→│
```

**Pinned/RawPointer 对象处理（策略 A）**

MAIN 空间中的 pinned 对象采用与 LOS 相同的策略：

```
[A][B][P][_][C][D][_][E]    P = pinned 对象
      │
      └── 只整理 P 右侧的对象，P 左侧不动

整理后:
[A][B][P][C][D][E][_][_]    P 原地不动，其右侧对象左移
                  │←归还│
```

**核心代码（伪码）**

```cpp
// RegionManager.cpp
void RegionManager::CompactMainLargeRegions()
{
    // 1. 扫描 MAIN 大对象 Region（> 32KB）收集存活且可移动的对象
    std::vector<MainLargeRegionInfo> liveRegions;
    size_t pinnedMaxAddr = 0;  // 最高 pinned 对象地址

    for (auto* region : mainLargeRegionList_) {
        if (!region->IsAlive()) continue;
        if (region->IsPinned() || region->HasRawPointer()) {
            pinnedMaxAddr = std::max(pinnedMaxAddr, region->End());
            continue;  // pinned 对象不参与整理
        }
        liveRegions.push_back({region, region->Size()});
    }

    if (liveRegions.empty()) return;

    // 2. 按地址排序（低地址优先）
    std::sort(liveRegions.begin(), liveRegions.end(),
              [](auto& a, auto& b) { return a.region->Start() < b.region->Start(); });

    // 3. 找到目标区域起始地址（紧接 pinnedMaxAddr 之后）
    uintptr_t targetAddr = std::max(mainSpace_.Begin(), pinnedMaxAddr);

    // 4. 滑动整理：向低地址方向移动（与 LOS 相反）
    for (auto& info : liveRegions) {
        if (info.region->Start() <= targetAddr) {
            // 已经在低地址端，跳过
            targetAddr = info.region->End();
            continue;
        }
        // 移动对象到 targetAddr
        MoveRegion(info.region, targetAddr);
        targetAddr += info.size;
    }

    // 5. 计算释放的空间（从 targetAddr 到 MAIN 原边界）
    size_t freedSize = mainSpace_.End() - AlignUp(targetAddr, REGION_SIZE);

    // 6. 将释放的空间归还 RESERVE
    if (freedSize > 0) {
        uintptr_t releaseStart = AlignUp(targetAddr, REGION_SIZE);
        reserveManager_->ReturnFromMain(releaseStart, freedSize);
        mainSpace_.SetEnd(releaseStart);  // 收缩 MAIN 边界
        mainCompactionReleasedSize_ += freedSize;
    }
}
```

**与 gc-helper 集成**

```cpp
// 在 GC 收集后检查是否需要 MAIN 整理
void GCPostCollectCallback()
{
    // ... 其他后处理 ...

    // 检查 MAIN 空间碎片率
    double mainFragRate = (double)(mainCapacity_ - mainUsed_) / mainCapacity_;
    if (mainFragRate > 0.4 && !mainCompactionPending_.load()) {
        mainCompactionPending_.store(true);
        gcHelper_->ScheduleTask([this]() {
            CompactMainLargeRegions();
            mainCompactionPending_.store(false);
        });
    }
}
```

**统计字段**

```cpp
// RegionManager 新增统计
size_t mainCompactionCount_ = 0;          // MAIN 整理次数
size_t mainCompactionReleasedSize_ = 0;   // MAIN 整理释放给 RESERVE 的总字节
std::atomic<bool> mainCompactionPending_{false};  // MAIN 整理是否在排队
```

**验收标准**

- MAIN 分配失败时，先触发异步整理（gc-helper），再同步整理（STW）
- 整理后高地址端空间正确归还给 RESERVE
- Pinned 对象不移动，其余对象向低地址滑动
- 日志可观测整理次数、释放空间、耗时

**回退策略**

- 整理开关 `cjEnableMainCompaction`（默认 1）可关闭 MAIN 整理功能
- 整理失败时触发 OOM

---

### M5：扩缩容（LOS ↔ RESERVE ↔ MAIN 动态调度）

**改动点**

- 完善 `ReserveManager` 的借还逻辑：
  - LOS 扩容：`LOSManager::ExpandFromReserve()` → `ReserveManager::BorrowForLOS()`。
  - LOS 缩容：`LOSManager::ShrinkToReserve()` → 先 CompactLOS → `ReserveManager::ReturnFromLOS()`。
  - MAIN 紧张且 RESERVE 空时：`ReserveManager::RequestLOSShrink()` → 触发 LOS 缩容。
- 增加保护阈值（hysteresis），避免频繁抖动：
  - 扩容阈值 > 缩容阈值 + gap。
  - 冷却时间（两次扩缩容之间最小间隔）。
- 缩容方向：从靠近 RESERVE 侧（即 LOS 低地址端）整理并归还。

**验收标准**

- 长压测下无明显抖动，吞吐与内存稳定。
- MAIN/LOS/RESERVE 三段空间可动态伸缩。

**回退策略**

- 动态扩缩容可单独关闭，LOS 退化为固定区。

---

## 2. 每阶段执行模板（你让我开始执行时按这个来）

每次只做一个 Mx：

1. 改代码（只改该阶段范围）；
2. 编译验证：`python3 build.py build -t debug`；
3. 若有编译问题，先修到通过；
4. 输出变更清单 + 风险点 + 下一步。

---

## 3. 代码触点清单

### 新增文件

| 文件                                    | 归属                  | 里程碑 |
| --------------------------------------- | --------------------- | ------ |
| `src/Heap/Allocator/LOSManager.h`       | LOSManager 头文件     | M1     |
| `src/Heap/Allocator/LOSManager.cpp`     | LOSManager 实现       | M1     |
| `src/Heap/Allocator/ReserveManager.h`   | ReserveManager 头文件 | M1     |
| `src/Heap/Allocator/ReserveManager.cpp` | ReserveManager 实现   | M1     |

### 修改文件

| 文件                                     | 改动                                                                                                                   | 里程碑     |
| ---------------------------------------- | ---------------------------------------------------------------------------------------------------------------------- | ---------- |
| `src/Heap/Allocator/RegionSpace.cpp`     | `Init()` 三段划分；`TryAllocateOnce()` LOS 分流                                                                        | M1, M2     |
| `src/Heap/Allocator/RegionManager.h`     | 增加 `losManager*` / `reserveManager*` 成员；MAIN 整理统计字段                                                         | M1, M4     |
| `src/Heap/Allocator/RegionManager.cpp`   | `Initialize()` 创建管理器；`AllocLarge()` LOS 优先；`TakeRegion()` RESERVE 借用；`CompactMainLargeRegions()` MAIN 整理 | M1, M2, M4 |
| `src/Heap/Allocator/FreeRegionManager.h` | 构造函数适配 LOSManager 独立引用                                                                                       | M1         |
| `src/Heap/Allocator/RegionInfo.h`        | 增加 `HasRawPointer()` 方法                                                                                            | M4         |
| `src/Heap/WCollector/WCollector.cpp`     | `PostTrace()` 增加 `CollectLOSGarbage()` 和回迁调用                                                                    | M3         |
| `src/Heap/Collector/GcStats.h/.cpp`      | 增加 LOS 统计字段；增加 MAIN 整理统计字段                                                                              | M3, M4     |
| `src/Heap/GcThreadPool.cpp`              | gc-helper 线程增加 LOS 整理和 MAIN 整理检查逻辑                                                                        | M4         |
| `src/CMakeLists.txt`                     | 新增源文件编译                                                                                                         | M1         |
| `src/CangjieRuntime.cpp`                 | 读取 `cjMainRatio` / `cjReserveRatio` / `cjLOSRatio` / `cjEnableMainCompaction` 环境变量                               | M1, M4     |

---

## 4. 风险与控制

| 风险                                  | 等级 | 控制措施                                              |
| ------------------------------------- | ---- | ----------------------------------------------------- |
| **引用修正**（回迁/整理涉及全堆扫描） | 最高 | M3/M4 先做 STW，减少并发复杂度                        |
| **FreeRegionManager 耦合**            | 高   | LOSManager 新建独立实例，不共享 RegionManager 的 tree |
| **unit index 越界**                   | 高   | LOSManager 初始化时校验 unit range，TakeRegion 时断言 |
| **性能回退**                          | 中   | 每轮迁移/整理设置预算；默认关闭高级功能               |
| **兼容性**                            | 中   | 保留降级路径，`cjLOSRatio=0 cjReserveRatio=0` 全回退  |
| **RESERVE 抖动**                      | 低   | hysteresis + 冷却时间                                 |

---

## 5. 里程碑概览

| 里程碑 | 内容                         | 关键交付                                                                        |
| ------ | ---------------------------- | ------------------------------------------------------------------------------- |
| **M1** | 空间划分 + 管理器骨架 + 开关 | LOSManager / ReserveManager 骨架，三段空间                                      |
| **M2** | 大对象分配分流               | LOS 优先 → RESERVE 借用(LOS/MAIN) → 降级 MAIN → STW 整理 → OOM                  |
| **M3** | LOS 回收 + 回迁              | 标记-清扫回收 + GC 后主堆大对象迁入 LOS                                         |
| **M4** | LOS/MAIN 整理                | 异步/同步整理；LOS 向高地址、MAIN 向低地址；pinned 对象不动；释放空间归 RESERVE |
| **M5** | 扩缩容                       | MAIN ↔ RESERVE ↔ LOS 动态借还                                                   |

---

## 6. 推荐启动方式

建议从 **M1** 开始执行（空间划分 + 管理器骨架）。

原因：

- 把 `MAIN | RESERVE | LOS` 的空间边界建立起来，是后续所有步骤的前提；
- LOSManager / ReserveManager 的骨架必须先存在，M2 才能调用；
- 对现有行为影响最小（`cjLOSRatio=0 cjReserveRatio=0` 时零开销）；
- 可以最早验证三段划分的正确性和可回退性。

---

## 7. 实现状态跟踪

> 更新时间：2026年2月

### 各里程碑完成情况

| 里程碑 | 状态 | 实现内容 | 文档 |
|--------|------|----------|------|
| **M1** | ✅ 完成 | 三段空间划分（6:3:1 比例）；LOSManager / ReserveManager 骨架；CartesianTree baseIndex 重构；环境变量 `cjMainRatio` / `cjReserveRatio` / `cjLOSRatio` | `M1_Implementation_Record.md` |
| **M2** | ✅ 完成 | LOS 优先分配分流；RESERVE A 类（LOS）+ B 类（MAIN）借用；MAIN 降级路径；`AllocLarge` 4 步路由 | `M2_Implementation_Record.md` |
| **M3** | ✅ 完成 | LOS 标记-清扫回收；MAIN→LOS 大对象迁移；引用更新（migrationMap + 全堆扫描）；RESERVE 自动归还 | `M3_Implementation_Record.md` |
| **M4a-sync** | ✅ 完成 | LOS 7 阶段滑动整理（向高地址紧凑）；pinned 屏障；losFreeMgr 重建；归还 RESERVE | `M4a_Implementation_Record.md` §1-5 |
| **M4a-async** | ✅ 完成 | FinalizerProcessor 异步触发整理；`isCompacting` / `compactionRequested` 状态标志；`SignalLOSCompaction()` | `M4a_Implementation_Record.md` §6 |
| **M4b-core** | ✅ 完成 | CartesianTree 低地址优先分配；`ShrinkInactiveZone()`；`mainEnd` 边界修复 | `M4b_Implementation_Record.md` §1-5 |
| **M4b-alignment** | ✅ 完成 | `AllocLargeInMain()` region 对齐（向上取整到 `maxUnitCountPerRegion`） | `M4b_Implementation_Record.md` §6 |
| **M5 #1** | ✅ 完成 | `pendingReserveReturns` — GC 后批量归还 RESERVE-borrowed region | `M5_Implementation_Record.md` §4.1 |
| **M5 #2** | ✅ 完成 | Cooldown（冷却机制）— RESERVE 借出后设定保护期，避免即借即还抖动 | `M5_Implementation_Record.md` §4.2 |
| **M5 #3** | ✅ 完成 | Hysteresis（迟滞机制）— RESERVE 可用低于阈值时才暂停借出 | `M5_Implementation_Record.md` §4.3 |
| **M5 #4** | ✅ 完成 | `DumpRegionStats` 增强 — RESERVE / LOS 统计输出 | `M5_Implementation_Record.md` §4.4 |
| **M5 #5** | ✅ 完成 | Hole Bitmap 灵活归还（`returnBitmap` + coalesce），接受任意位置归还，无需排序，无失败分支 | `M5_Implementation_Record.md` §8 |
| **M5 #6** | ✅ 完成 | 环境变量开关（`cjReserveCooldownGCs`, `cjReserveHysteresisRatio`） | `M5_Implementation_Record.md` §4.5 |

### 计划与实际偏差

| 计划内容 | 实际偏差 | 原因 |
|----------|----------|------|
| M4 同步 + 异步整理合为一个步骤 | 拆分为 M4a（sync+async）和 M4b（MAIN defrag） | MAIN 碎片整理（低地址优先 + ShrinkInactiveZone）独立性强，同 LOS 整理解耦 |
| M5 完整扩缩容 | M5 #5 Hole Bitmap 已实现 | 方案 B（Hole Bitmap）在复杂度和完备性之间取得最佳平衡 |
| M4b 大对象 region 对齐未在原计划中 | 实现中发现碎片问题，追加 | MAIN 降级大对象释放后形成不规则空闲块，需要对齐到标准 region 大小 |
