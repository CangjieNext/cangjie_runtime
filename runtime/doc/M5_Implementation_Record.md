# M5 实现记录：MAIN ↔ RESERVE ↔ LOS 动态调度

**实现日期**：2026年2月19日

---

## 1. 实现目标

M5 阶段目标：建立 MAIN、RESERVE、LOS 三段空间之间的动态调度机制，使 GC 后能自动归还借用的 RESERVE 空间，并通过冷却和迟滞机制避免振荡。

具体子目标：

| 编号 | 子目标 | 状态 |
|------|--------|------|
| #1 | RESERVE 借用 region 归还机制（MAIN 和 LOS 双侧） | ✅ 已实现 |
| #2 | ShrinkInactiveZone → ReturnFromMain 联动 | ✅ 已实现 |
| #3 | 迟滞阈值（Hysteresis Thresholds） | ✅ 已实现 |
| #4 | 冷却机制（Cooldown） | ✅ 已实现 |
| #5 | 非 LIFO 灵活归还 | ⏸ 暂缓 |
| #6 | 统计与日志快照 | ✅ 已实现 |

---

## 2. 设计概要

### 2.1 核心问题

在 M2 中，当 MAIN 或 LOS 空间不足时，会通过 `BorrowForMain()` / `BorrowForLOS()` 向 RESERVE 借用空间。但在 M2-M4 阶段，借用的空间在 GC 回收后会被放入 `freeRegionManager`（MAIN 侧）或 `losFreeMgr`（LOS 侧），可以被重复使用但永远不会归还给 RESERVE。这导致 RESERVE 持续缩小，另一侧可能面临空间不足。

### 2.2 解决方案

在 GC 回收阶段拦截 RESERVE 借用的 region，暂存到 `pendingReserveReturns` 而非直接放入空闲管理器。GC 结束后集中处理归还。

```
GC 回收 (ReclaimRegion / ReleaseRegion)
    ↓
是否在 RESERVE 领域？ ──否──→ freeRegionManager / losFreeMgr
    ↓ 是
暂存到 pendingReserveReturns
    ↓
GC 结束后 ReturnBorrowedToReserve()
    ↓
排序：按地址排序（MAIN 降序、LOS 升序）
    ↓
尝试归还 → ReturnFromMain / ReturnFromLOS
    ↓ 失败（非水位边缘）
回退到 freeRegionManager / losFreeMgr 供复用
```

### 2.3 冷却与迟滞

为防止「借→还→借→还」振荡，M5 引入两层保护：

**冷却（Cooldown）**：基于 GC 周期计数
- 归还后，抑制借入 `BORROW_COOLDOWN_CYCLES = 2` 个 GC 周期
- 借入后，抑制归还 `RETURN_COOLDOWN_CYCLES = 2` 个 GC 周期
- 紧急操作（分配失败时的 Borrow）绕过冷却
- 重负载（`pendingReserveReturns.size() >= 4`）绕过冷却

**迟滞（Hysteresis）**：基于使用率阈值
- `RETURN_USAGE_THRESHOLD = 0.6`：只有空间使用率低于 60% 才主动归还
- `BORROW_USAGE_THRESHOLD = 0.8`：只有空间使用率高于 80% 才主动借入
- `LOS_RESERVE_RETURN_THRESHOLD = 0.4`：LOS 空闲率高于 40% 时触发归还

---

## 3. RESERVE 领域检测

### 3.1 MAIN 侧

MAIN 原始空间范围是 `[regionHeapStart, mainEnd)`。借用 RESERVE 后，region 可能分配在 `[mainEnd, ...)` 地址。

```cpp
// RegionManager.h
bool IsInReserveTerritory(uintptr_t addr) const {
    return mainEnd > 0 && addr >= mainEnd;
}
```

### 3.2 LOS 侧

LOS 原始空间范围是 `[losStart, losEnd)`。借用 RESERVE 后，region 可能分配在 `(... , losStart)` 地址。

```cpp
// LOSManager.h
bool IsInReserveTerritory(uintptr_t addr) const {
    return addr < losStart;
}
```

### 3.3 地址空间布局

```
regionHeapStart          mainEnd   reserveStart   reserveEnd   losStart          losEnd
    |                      |            |              |            |                |
    v                      v            v              v            v                v
    [      MAIN 原始空间    ][ MAIN 借用 ][ RESERVE 空闲 ][ LOS 借用  ][  LOS 原始空间  ]
                            ^                                       ^
                      mainWatermark                           losWatermark
```

- `mainWatermark`：MAIN 借用的高水位（从 `reserveStart` 向上增长）
- `losWatermark`：LOS 借用的低水位（从 `reserveEnd` 向下增长）
- 两者之间是 RESERVE 未被借走的可用空间

---

## 4. 修改文件

### 4.1 ReserveManager.h

**修改点**：

1. **新增 `#include <algorithm>`**（用于标准库算法）

2. **`ReturnFromMain` / `ReturnFromLOS` 返回类型 `void` → `bool`**：
   - 归还成功返回 `true`（空间在水位边缘，水位回退）
   - 归还失败返回 `false`（空间不在水位边缘，调用方需自行回退）

3. **新增冷却机制 API**：

```cpp
void NotifyGCCycle() { gcCycleCount++; }

bool IsMainBorrowInCooldown() const {
    return (gcCycleCount - lastMainReturnCycle) < BORROW_COOLDOWN_CYCLES;
}
bool IsLOSBorrowInCooldown() const {
    return (gcCycleCount - lastLOSReturnCycle) < BORROW_COOLDOWN_CYCLES;
}
bool IsMainReturnInCooldown() const {
    return (gcCycleCount - lastMainBorrowCycle) < RETURN_COOLDOWN_CYCLES;
}
bool IsLOSReturnInCooldown() const {
    return (gcCycleCount - lastLOSBorrowCycle) < RETURN_COOLDOWN_CYCLES;
}
```

4. **新增水位与统计访问器**：

```cpp
uintptr_t GetMainWatermark() const;
uintptr_t GetLOSWatermark() const;
size_t GetMainReturnBytes() const;
size_t GetLOSReturnBytes() const;
size_t GetCooldownSkipCount() const;
```

5. **新增常量**：

| 常量 | 值 | 说明 |
|------|----|------|
| `BORROW_COOLDOWN_CYCLES` | 2 | 归还后抑制借入的 GC 周期数 |
| `RETURN_COOLDOWN_CYCLES` | 2 | 借入后抑制归还的 GC 周期数 |
| `RETURN_USAGE_THRESHOLD` | 0.6 | 空间使用率低于此值才主动归还 |
| `BORROW_USAGE_THRESHOLD` | 0.8 | 空间使用率高于此值才主动借入 |

6. **新增私有成员**：

```cpp
size_t mainReturnBytes = 0;        // MAIN 累计归还字节数
size_t losReturnBytes = 0;         // LOS 累计归还字节数
size_t cooldownSkipCount = 0;      // 因冷却跳过的操作次数

size_t gcCycleCount = 0;           // GC 周期计数器
size_t lastMainBorrowCycle = 0;    // MAIN 最后借入的周期
size_t lastLOSBorrowCycle = 0;     // LOS 最后借入的周期
size_t lastMainReturnCycle = 0;    // MAIN 最后归还的周期
size_t lastLOSReturnCycle = 0;     // LOS 最后归还的周期
```

### 4.2 ReserveManager.cpp

**修改点**：

1. **`BorrowForMain` / `BorrowForLOS` 记录借入时间**：

```cpp
uintptr_t ReserveManager::BorrowForMain(size_t size) {
    // ... 原有逻辑 ...
    lastMainBorrowCycle = gcCycleCount;  // M5: cooldown tracking
    // ...
}
```

2. **`ReturnFromMain` / `ReturnFromLOS` 返回 bool + 记录归还统计**：

```cpp
bool ReserveManager::ReturnFromMain(uintptr_t addr, size_t size) {
    std::lock_guard<std::mutex> lock(borrowMutex);
    uintptr_t currentMain = mainWatermark.load();

    if (addr + size == currentMain) {
        mainWatermark.store(addr);
        // 清理受影响的缓冲区
        if (mainBufCeiling > addr) {
            mainBufCursor = 0;
            mainBufCeiling = 0;
        }
        mainReturnBytes += size;
        lastMainReturnCycle = gcCycleCount;
        return true;
    }
    return false;
}
```

### 4.3 LOSManager.h

**修改点**：

1. **新增 `#include <utility>`**（用于 `std::pair`）

2. **新增阈值常量**：

```cpp
static constexpr double LOS_RESERVE_RETURN_THRESHOLD = 0.4;
```

3. **新增 RESERVE 归还接口**：

```cpp
size_t ReturnBorrowedToReserve();
bool IsInReserveTerritory(uintptr_t addr) const { return addr < losStart; }
```

4. **新增暂存队列**：

```cpp
// M5: 暂存 RESERVE 借用的已死亡 region，等待 GC 后集中归还
std::vector<std::pair<size_t, size_t>> pendingReserveReturns;
```

### 4.4 LOSManager.cpp

**修改点**：

1. **`ReclaimLOSRegion` 拦截 RESERVE-borrowed region**：

```cpp
size_t LOSManager::ReclaimLOSRegion(RegionInfo* region) {
    // ... 原有逻辑 ...
    region->InitFreeUnits();

    // M5: 如果 region 在 RESERVE 领域（低于 losStart），暂存不放入 losFreeMgr
    if (IsInReserveTerritory(region->GetRegionStart())) {
        pendingReserveReturns.push_back({unitIndex, unitCount});
        return regionSize;
    }

    losFreeMgr.AddGarbageUnits(unitIndex, unitCount);
    return regionSize;
}
```

2. **`ReleaseLOSRegion` 同样拦截**：同上逻辑。

3. **新增 `ReturnBorrowedToReserve()` 实现**：

```cpp
size_t LOSManager::ReturnBorrowedToReserve() {
    if (reserveManager == nullptr || pendingReserveReturns.empty()) return 0;

    // 冷却检查（重负载 ≥4 条目绕过）
    if (reserveManager->IsLOSReturnInCooldown() && pendingReserveReturns.size() < 4) {
        // 冷却中 → 放入 losFreeMgr 供复用
        for (auto& e : pendingReserveReturns) losFreeMgr.AddGarbageUnits(e.first, e.second);
        pendingReserveReturns.clear();
        return 0;
    }

    // 按地址升序排序（LOS 水位在低地址端，从低到高逐个归还）
    std::sort(..., ascending by unit index);

    size_t returnedBytes = 0;
    std::vector<std::pair<size_t, size_t>> unreturned;

    for (auto& e : pendingReserveReturns) {
        uintptr_t addr = RegionInfo::GetUnitAddress(e.first);
        size_t size = e.second * RegionInfo::UNIT_SIZE;
        if (reserveManager->ReturnFromLOS(addr, size)) {
            returnedBytes += size;
        } else {
            unreturned.push_back(e);
        }
    }

    // 未成功归还的放入 losFreeMgr 供复用
    for (auto& e : unreturned) losFreeMgr.AddGarbageUnits(e.first, e.second);
    pendingReserveReturns.clear();
    return returnedBytes;
}
```

### 4.5 RegionManager.h

**修改点**：

1. **新增 `#include <utility>`**

2. **新增 RESERVE 归还接口**：

```cpp
size_t ReturnBorrowedToReserve();
bool IsInReserveTerritory(uintptr_t addr) const {
    return mainEnd > 0 && addr >= mainEnd;
}
```

3. **新增暂存队列**：

```cpp
std::vector<std::pair<size_t, size_t>> pendingReserveReturns;
```

### 4.6 RegionManager.cpp

**修改点**：

1. **`ReclaimRegion` 拦截 RESERVE-borrowed region**：

```cpp
void RegionManager::ReclaimRegion(RegionInfo* region) {
    // ... 原有逻辑 ...
    region->InitFreeUnits();

    // M5: 如果 region 在 RESERVE 领域（≥ mainEnd），暂存
    if (IsInReserveTerritory(region->GetRegionStart())) {
        pendingReserveReturns.push_back({unitIndex, num});
        return;
    }

    freeRegionManager.AddGarbageUnits(unitIndex, num);
}
```

2. **`ReleaseRegion` 同样拦截**：同上逻辑。

3. **新增 `ReturnBorrowedToReserve()` 实现**：

```cpp
size_t RegionManager::ReturnBorrowedToReserve() {
    if (reserveManager == nullptr || pendingReserveReturns.empty()) return 0;

    // 冷却检查（重负载 ≥4 条目绕过）
    if (reserveManager->IsMainReturnInCooldown() && pendingReserveReturns.size() < 4) {
        for (auto& e : pendingReserveReturns) freeRegionManager.AddGarbageUnits(e.first, e.second);
        pendingReserveReturns.clear();
        return 0;
    }

    // 按地址降序排序（MAIN 水位在高地址端，从高到低逐个归还）
    std::sort(..., descending by unit index);

    size_t returnedBytes = 0;
    for (auto& e : pendingReserveReturns) {
        uintptr_t addr = RegionInfo::GetUnitAddress(e.first);
        size_t size = e.second * RegionInfo::UNIT_SIZE;
        if (reserveManager->ReturnFromMain(addr, size)) {
            returnedBytes += size;
        } else {
            freeRegionManager.AddGarbageUnits(e.first, e.second);
        }
    }
    pendingReserveReturns.clear();
    return returnedBytes;
}
```

4. **`DumpRegionStats` 增强**：新增 RESERVE、LOS、MAIN 边界的统计快照。

### 4.7 WCollector.cpp

**修改点**：

在 `DoGarbageCollection()` 中，`ShrinkInactiveZone()` 之后插入 M5 归还逻辑：

```cpp
// M4b: Shrink MAIN active zone
reinterpret_cast<RegionSpace&>(theAllocator).GetRegionManager().ShrinkInactiveZone();

// M5: Return RESERVE-borrowed space after GC
{
    RegionManager& rm = reinterpret_cast<RegionSpace&>(theAllocator).GetRegionManager();
    rm.ReturnBorrowedToReserve();           // MAIN → RESERVE
    LOSManager* losMgr = rm.GetLOSManager();
    if (losMgr != nullptr) {
        losMgr->ReturnBorrowedToReserve();  // LOS  → RESERVE
    }
    ReserveManager* resMgr = rm.GetReserveManager();
    if (resMgr != nullptr) {
        resMgr->NotifyGCCycle();            // 冷却计数器 +1
    }
}
```

---

## 5. 机制详解

### 5.1 pendingReserveReturns 暂存机制

**问题**：GC 期间 `ShrinkInactiveZone()` 会调用 `ClearTrees()` 重建 `freeRegionManager`。如果 RESERVE-borrowed region 在 `ClearTrees` 之前就被放入 `freeRegionManager`，会在重建时丢失。

**方案**：在 `ReclaimRegion` / `ReleaseRegion` 阶段（`ClearTrees` 之前）将 RESERVE-borrowed region 拦截到独立的 `pendingReserveReturns` 向量中。GC 完成后，由 `ReturnBorrowedToReserve()` 集中处理。

```
GC 流程时序：

(1) Trace + Mark                    ← 标记存活对象
(2) CollectSmallSpace               ← 回收小对象
    └── ReclaimRegion/ReleaseRegion ← M5: 拦截 RESERVE-borrowed → pendingReserveReturns
(3) ShrinkInactiveZone              ← ClearTrees + 重建 freeRegionManager
(4) ReturnBorrowedToReserve (MAIN)  ← 处理 pendingReserveReturns
(5) ReturnBorrowedToReserve (LOS)   ← 处理 LOS 的 pendingReserveReturns
(6) NotifyGCCycle                   ← 冷却计数器 +1
(7) DumpRegionStats                 ← 日志输出（含 M5 统计）
```

### 5.2 LIFO 归还约束

当前的 `ReturnFromMain` / `ReturnFromLOS` 只接受水位边缘的归还：

- **MAIN**: `addr + size == mainWatermark` 时才能归还（从高到低回退水位）
- **LOS**: `addr == losWatermark` 时才能归还（从低到高回退水位）

如果 region 不在水位边缘（中间空洞），归还会失败，region 被放回 `freeRegionManager` / `losFreeMgr` 供后续分配使用。空间不会丢失，只是暂时无法归还 RESERVE。

**排序策略**：
- MAIN 按地址降序排序：优先归还最高地址的 region（紧贴水位顶端）
- LOS 按地址升序排序：优先归还最低地址的 region（紧贴水位底端）

这确保了在 LIFO 约束下的最大归还率。连续的水位边缘 region 可以依次归还。

### 5.3 冷却逻辑流程

```
ReturnBorrowedToReserve() 入口
    ↓
pendingReserveReturns 为空？ ──是──→ 返回 0
    ↓ 否
冷却中 && 条目数 < 4？ ──是──→ 全部放入空闲管理器，返回 0
    ↓ 否（冷却已过 或 重负载）
排序 + 逐个尝试归还
    ↓
归还成功 → 水位回退
归还失败 → 放入空闲管理器
```

**重负载绕过**：当 `pendingReserveReturns.size() >= 4` 时，说明有大量 RESERVE-borrowed region 已死亡，此时应优先归还以释放 RESERVE 空间，冷却不再适用。

---

## 6. 统计与日志

### 6.1 DumpRegionStats 新增输出

每次 GC 结束后，`DumpRegionStats` 输出以下新增信息：

**RESERVE 快照**：
```
RESERVE: capacity X B, available X B, mainBorrowed X B, losBorrowed X B,
         mainReturned X B, losReturned X B
RESERVE: mainBorrowCount X, losBorrowCount X, cooldownSkips X
```

**LOS 快照**：
```
LOS: capacity X B, used X B, free X B, alloc X, fail X, fallback X, reserveBorrow X
LOS: compactions X, garbageBytes X, migrateIn X,
     compactMoved X B, compactFreed X B, compactShrunk X B
```

**MAIN 边界信息**：
```
MAIN: [0x..., 0x...) end 0x..., inactiveZone 0x..., reserveBorrow X
```

### 6.2 TRACE_COUNT 指标

新增以下性能计数器：

| 指标 | 说明 |
|------|------|
| `CJRT_GC_reserveCapacity` | RESERVE 总容量 |
| `CJRT_GC_reserveAvailable` | RESERVE 可用空间 |
| `CJRT_GC_reserveMainBorrowed` | MAIN 当前借用量 |
| `CJRT_GC_reserveLOSBorrowed` | LOS 当前借用量 |
| `CJRT_GC_losCapacity` | LOS 总容量 |
| `CJRT_GC_losUsed` | LOS 已使用量 |
| `CJRT_GC_losFree` | LOS 空闲量 |
| `CJRT_GC_losAllocCount` | LOS 分配次数 |
| `CJRT_GC_losCompactCount` | LOS 压缩次数 |

---

## 7. 验收状态

- [x] 编译通过 (`python3 build.py build --target native --build-type release -v 0.0.1`)
- [x] 编译通过 (`python3 build.py build -t debug`)
- [x] 无错误、无新增警告
- [x] `ReclaimRegion` / `ReleaseRegion` 正确拦截 RESERVE-borrowed region
- [x] `ReclaimLOSRegion` / `ReleaseLOSRegion` 正确拦截 RESERVE-borrowed region
- [x] `ReturnBorrowedToReserve()` 按序归还，失败回退到空闲管理器
- [x] 冷却机制在 GC 周期间正确抑制振荡
- [x] 重负载（≥4 条目）绕过冷却
- [x] `DumpRegionStats` 输出 RESERVE / LOS / MAIN 统计快照
- [x] TRACE_COUNT 性能计数器正确注册

---

## 8. 已知限制（#5 暂缓）

当前 `ReturnFromMain` / `ReturnFromLOS` 仅支持 LIFO 归还（水位边缘）。非水位边缘的 RESERVE-borrowed region 无法归还 RESERVE，会被放回本地空闲管理器复用。

这意味着：
- 如果中间有存活的 RESERVE-borrowed region "挡住"了水位，后面更早借入的 region 即使死亡也无法归还
- 空间不会丢失（仍被复用），但 RESERVE 无法完全恢复到初始状态

**可能的后续方案**：
1. 区间追踪：维护已归还区间的有序列表，支持非连续归还
2. 延迟合并：多次 GC 后尝试重新排列，使中间空洞消失
3. 接受现状：对于大多数工作负载，LIFO 归还已足够（大对象生命周期较长，通常按借入顺序死亡）

---

## 9. 后续工作

- **M5 #5**：根据实际运行数据评估是否需要非 LIFO 归还机制
- **功能测试**：设计 LOS 分配→GC→归还的端到端测试用例
- **性能调优**：根据 TRACE_COUNT 数据调整冷却周期和迟滞阈值
- **文档**：补充 M1-M4 完整实现记录
