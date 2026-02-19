# M1 实现记录：空间划分 + LOSManager / ReserveManager 骨架

**实现日期**：2026年2月15日

---

## 1. 实现目标

M1 阶段目标：在不切流量的前提下，完成堆空间三段划分和管理器骨架搭建。

- 堆空间划分为 `MAIN | RESERVE | LOS` 三段
- 创建 `LOSManager` 和 `ReserveManager` 骨架
- 通过环境变量控制比例，默认 6:3:1
- 比例全为 0 时回退到原有行为

---

## 2. 新增文件

### 2.1 LOSManager.h

**路径**：`src/Heap/Allocator/LOSManager.h`

**职责**：管理 LOS（Large Object Space）空间

**核心成员**：

```cpp
class LOSManager {
    ReserveManager* reserveManager;        // 关联的 RESERVE 管理器

    uintptr_t losStart;                    // LOS 空间起始地址（低地址边界）
    uintptr_t losEnd;                      // LOS 空间结束地址（高地址边界）
    std::atomic<uintptr_t> losInactiveZone; // LOS 未激活分界线

    UnitCount losUnitStart;                // LOS 全局 unit 起始索引
    UnitCount losUnitEnd;                  // LOS 全局 unit 结束索引

    // 独立的 FreeRegionManager 组件
    std::mutex releasedUnitTreeMutex;
    CartesianTree releasedUnitTree;
    std::mutex dirtyUnitTreeMutex;
    CartesianTree dirtyUnitTree;

    // Region 列表
    RegionList losOldLargeRegionList;
    RegionList losRecentLargeRegionList;

    // 统计
    size_t losAllocCount;
    size_t losAllocFailCount;
    size_t losFallbackCount;
};
```

**公开接口**：

- `Initialize(start, end, unitStart, unitEnd)` - 初始化 LOS 空间
- `GetLOSStart()` / `GetLOSEnd()` / `GetInactiveZone()` - 边界查询
- `IsInLOS(addr)` - 地址范围判断
- `GetCapacity()` / `GetUsedSize()` / `GetFreeSize()` - 容量统计
- `IncrementAllocFailCount()` / `IncrementFallbackCount()` - 统计累加

### 2.2 LOSManager.cpp

**路径**：`src/Heap/Allocator/LOSManager.cpp`

**实现**：

```cpp
void LOSManager::Initialize(uintptr_t start, uintptr_t end, UnitCount unitStart, UnitCount unitEnd)
{
    losStart = start;
    losEnd = end;
    losUnitStart = unitStart;
    losUnitEnd = unitEnd;

    // LOS 从高地址向低地址增长
    losInactiveZone.store(losEnd, std::memory_order_relaxed);

    // 初始化独立的 CartesianTree
    UnitCount losUnitCount = unitEnd - unitStart;
    releasedUnitTree.Init(losUnitCount);
    dirtyUnitTree.Init(losUnitCount);
}
```

### 2.3 ReserveManager.h

**路径**：`src/Heap/Allocator/ReserveManager.h`

**职责**：管理 RESERVE 缓冲区，支持 MAIN 和 LOS 借还空间

**核心成员**：

```cpp
class ReserveManager {
    uintptr_t reserveStart;                // RESERVE 起始地址
    uintptr_t reserveEnd;                  // RESERVE 结束地址
    size_t unitSize;                       // 单元大小（页大小）

    std::atomic<uintptr_t> mainBorrow;     // MAIN 借用水位（从低地址向上）
    std::atomic<uintptr_t> losBorrow;      // LOS 借用水位（从高地址向下）
    std::mutex borrowMutex;                // 借还互斥锁
};
```

**公开接口**：

- `Initialize(start, end, unitSize)` - 初始化 RESERVE 区域
- `GetReserveStart()` / `GetReserveEnd()` - 边界查询
- `GetAvailable()` / `GetCapacity()` - 容量查询
- `IsInReserve(addr)` - 地址范围判断
- `BorrowForMain(size)` - MAIN 借用空间（从低地址向上）
- `BorrowForLOS(size)` - LOS 借用空间（从高地址向下）
- `ReturnFromMain(addr, size)` - MAIN 归还空间
- `ReturnFromLOS(addr, size)` - LOS 归还空间
- `GetMainBorrowedSize()` / `GetLOSBorrowedSize()` - 借用统计

### 2.4 ReserveManager.cpp

**路径**：`src/Heap/Allocator/ReserveManager.cpp`

**借用逻辑**：

```cpp
uintptr_t ReserveManager::BorrowForMain(size_t size)
{
    std::lock_guard<std::mutex> lock(borrowMutex);
    uintptr_t currentMain = mainBorrow.load();
    uintptr_t currentLOS = losBorrow.load();
    uintptr_t newMain = currentMain + size;

    if (newMain > currentLOS) {
        return 0;  // 空间不足
    }

    mainBorrow.store(newMain);
    return currentMain;
}

uintptr_t ReserveManager::BorrowForLOS(size_t size)
{
    std::lock_guard<std::mutex> lock(borrowMutex);
    uintptr_t currentMain = mainBorrow.load();
    uintptr_t currentLOS = losBorrow.load();
    uintptr_t newLOS = currentLOS - size;

    if (newLOS < currentMain) {
        return 0;  // 空间不足
    }

    losBorrow.store(newLOS);
    return newLOS;
}
```

---

## 3. 修改文件

### 3.1 RegionManager.h

**修改点**：

1. **新增前向声明**：

```cpp
namespace MapleRuntime {
class CopyCollector;
class CompactCollector;
class LOSManager;      // 新增
class ReserveManager;  // 新增
```

2. **新增成员变量**：

```cpp
private:
    uintptr_t regionInfoStart = 0;
    uintptr_t regionHeapStart = 0;
    uintptr_t regionHeapEnd = 0;

    // LOS (Large Object Space) manager - nullptr when cjLOSRatio=0
    LOSManager* losManager = nullptr;      // 新增
    // RESERVE zone manager - nullptr when cjReserveRatio=0
    ReserveManager* reserveManager = nullptr;  // 新增
```

### 3.2 RegionManager.cpp

**修改点**：

1. **新增 include**：

```cpp
#include "Allocator/LOSManager.h"
#include "Allocator/ReserveManager.h"
```

2. **新增辅助函数**：

```cpp
static int GetSpaceRatio(const char* envName, int defaultValue)
{
    auto env = std::getenv(envName);
    if (env != nullptr) {
        int value = std::atoi(env);
        if (value >= 0) {
            return value;
        }
    }
    return defaultValue;
}
```

3. **修改 Initialize() 函数**：

原逻辑：

```cpp
void RegionManager::Initialize(size_t nUnit, uintptr_t regionInfoAddr)
{
    // ... 直接使用全部 nUnit 初始化 freeRegionManager
    freeRegionManager.Initialize(nUnit);
}
```

新逻辑：

```cpp
void RegionManager::Initialize(size_t nUnit, uintptr_t regionInfoAddr)
{
    // 读取空间比例（默认 6:3:1）
    int mainRatio = GetSpaceRatio("cjMainRatio", 6);
    int reserveRatio = GetSpaceRatio("cjReserveRatio", 3);
    int losRatio = GetSpaceRatio("cjLOSRatio", 1);
    int totalRatio = mainRatio + reserveRatio + losRatio;

    // 计算各空间 unit 数量
    size_t losUnitCount = (nUnit * losRatio) / totalRatio;
    size_t reserveUnitCount = (nUnit * reserveRatio) / totalRatio;
    size_t mainUnitCount = nUnit - losUnitCount - reserveUnitCount;

    // 计算边界地址
    uintptr_t mainEnd = regionHeapStart + mainUnitCount * UNIT_SIZE;
    uintptr_t reserveStart = mainEnd;
    uintptr_t reserveEnd = reserveStart + reserveUnitCount * UNIT_SIZE;
    uintptr_t losStart = reserveEnd;
    uintptr_t losEnd = regionHeapEnd;

    // 创建 ReserveManager
    if (reserveRatio > 0 && reserveUnitCount > 0) {
        reserveManager = new ReserveManager();
        reserveManager->Initialize(reserveStart, reserveEnd, UNIT_SIZE);
    }

    // 创建 LOSManager
    if (losRatio > 0 && losUnitCount > 0) {
        losManager = new LOSManager(reserveManager);
        losManager->Initialize(losStart, losEnd, losUnitStart, losUnitEnd);
    }

    // freeRegionManager 只管理 MAIN 空间
    freeRegionManager.Initialize(mainUnitCount);
}
```

### 3.3 CMakeLists.txt

**路径**：`src/Heap/Allocator/CMakeLists.txt`

**修改**：

```cmake
set(SRC_LIST
    "Allocator.cpp"
    "MemMap.cpp"
    "RegionSpace.cpp"
    "RegionManager.cpp"
    "CartesianTree.cpp"
    "LOSManager.cpp"      # 新增
    "ReserveManager.cpp"  # 新增
)
```

---

## 4. 配置项

| 环境变量         | 默认值 | 说明             |
| ---------------- | ------ | ---------------- |
| `cjMainRatio`    | 6      | MAIN 空间比例    |
| `cjReserveRatio` | 3      | RESERVE 空间比例 |
| `cjLOSRatio`     | 1      | LOS 空间比例     |

**比例计算**：

```
mainSize = heapSize * cjMainRatio / (cjMainRatio + cjReserveRatio + cjLOSRatio)
```

**关闭 LOS**：

```bash
export cjLOSRatio=0
export cjReserveRatio=0
# 此时全部空间归 MAIN，行为等同原版本
```

---

## 5. 内存布局

```
regionHeapStart                                                       regionHeapEnd
    |                                |                  |                    |
    v                                v                  v                    v
    [         MAIN Space            ][  RESERVE Zone   ][     LOS Space      ]
    |<------ ~60% heapSize -------->||<---- ~30% ----->||<----- ~10% ------->|
                                     ^                  ^
                                reserveStart         losStart
```

- **MAIN**：从低地址向高地址增长（现有 inactiveZone 机制）
- **RESERVE**：中间缓冲区，MAIN 和 LOS 可按需借用
- **LOS**：从高地址向低地址增长（losInactiveZone 向低地址扩展）

---

## 6. 验收状态

- [x] 编译通过 (`python3 build.py build`)
- [x] 无错误、无新增警告
- [x] `cjLOSRatio=0 cjReserveRatio=0` 时全部空间归 MAIN
- [x] 默认配置（6:3:1）正常初始化

---

## 7. M1 重构：LOSManager 复用 FreeRegionManager

**重构日期**：2026年2月

### 7.1 重构动机

M1 初始实现中，LOSManager 内部直接持有两棵原始 `CartesianTree` 及对应的 `std::mutex`，与 RegionManager 中已有的 `FreeRegionManager` 功能重复。为了在 M2 阶段复用 `FreeRegionManager` 的 `TakeRegion()` / `AddGarbageUnits()` / `AddReleaseUnits()` / `ReleaseGarbageRegions()` 等方法，需要将 LOSManager 的树管理替换为独立的 `FreeRegionManager` 实例。

**核心挑战**：`CartesianTree::Node` 内部存储的是局部索引 `[0, unitCount)`，但 `RefreshFreeRegionInfo()` 和 `ReleaseMemory()` 调用 `RegionInfo` 时需要全局索引。对于 MAIN 空间（base=0）这是透明的，但 LOS 空间（base=losUnitStart>0）需要偏移量。

### 7.2 修改文件

#### CartesianTree.h / CartesianTree.cpp

- 新增 `baseIndex` 成员变量（默认 0，MAIN 不变；LOS 设为 losUnitStart）
- `Init(count, base=0)` 保存 baseIndex
- `Node` 构造、`IncCount`、`UpdateNode`、`RefreshFreeRegionInfo`、`ReleaseMemory` 方法均增加 `base` 参数（默认 0，保持向后兼容）
- 所有内部方法（`MergeInsert`、`MergeToRight`、`MergeToLeft`、`TakeUnitsImpl`、`AllocateLowestAddressFromNode`、`ReleaseRootNode`）自动传递 `baseIndex`
- `RefreshFreeRegionInfo(base)` → `RegionInfo::InitFreeRegion(localIdx + base, cnt)`
- `ReleaseMemory(base)` → `RegionInfo::ReleaseUnits(localIdx + base, cnt)`

#### FreeRegionManager.h

- 新增 `baseUnit` 成员变量（构造函数参数，默认 0）
- `Initialize(regionCnt)` 传递 baseUnit 给 `CartesianTree::Init(cnt, baseUnit)`
- `TakeRegion()` 中 `idx` 由树返回的局部索引转换为 `globalIdx = idx + baseUnit` 用于 RegionInfo 操作
- `AddGarbageUnits(idx, num)` / `AddReleaseUnits(idx, num)` 接收全局索引，转换为 `localIdx = idx - baseUnit` 用于树操作
- 新增 `GetBaseUnit()` 访问器

#### LOSManager.h

- **移除**：`releasedUnitTreeMutex`、`releasedUnitTree`、`dirtyUnitTreeMutex`、`dirtyUnitTree` 四个原始成员
- **新增**：`FreeRegionManager losFreeMgr` 成员（baseUnit = losUnitStart）
- 构造函数改为 `LOSManager(RegionManager& manager, UnitCount baseUnitIndex, ReserveManager*)`
- 析构函数改为 `= default`（FreeRegionManager 自行管理树的清理）
- 新增 `GetFreeRegionManager()` / `GetUnitStart()` / `GetUnitEnd()` 访问器

#### LOSManager.cpp

- `Initialize()` 使用 `losFreeMgr.Initialize(losUnitCount)` 替代原始树的 `Init()`

#### RegionManager.cpp

- LOSManager 构造函数调用更新为 `new LOSManager(*this, losUnitStart, reserveManager)`

### 7.3 更新后的 LOSManager 核心成员

```cpp
class LOSManager {
    FreeRegionManager losFreeMgr;          // 独立的 FreeRegionManager (baseUnit = losUnitStart)
    ReserveManager* reserveManager;        // RESERVE 管理器

    uintptr_t losStart;                    // LOS 空间起始地址
    uintptr_t losEnd;                      // LOS 空间结束地址
    std::atomic<uintptr_t> losInactiveZone;

    UnitCount losUnitStart;                // 全局 unit 起始索引
    UnitCount losUnitEnd;                  // 全局 unit 结束索引

    RegionList losOldLargeRegionList;
    RegionList losRecentLargeRegionList;

    size_t losAllocCount;
    size_t losAllocFailCount;
    size_t losFallbackCount;
};
```

### 7.4 索引转换示意

```
全局 RegionInfo 索引:  [0 ......... mainUnitCount-1][reserveUnits][losUnitStart ... nUnit-1]
                        |<--- MAIN (base=0) --->|                |<--- LOS (base=losUnitStart) --->|

MAIN FreeRegionManager:  baseUnit=0,  树内局部索引=[0, mainUnitCount)
LOS  FreeRegionManager:  baseUnit=losUnitStart, 树内局部索引=[0, losUnitCount)

转换规则:
  globalIdx = localIdx + baseUnit
  localIdx  = globalIdx - baseUnit
```

### 7.5 验收

- [x] 编译通过 (native debug)
- [x] 无新增错误或警告
- [x] 所有默认参数 base=0 确保 MAIN 路径完全不变（向后兼容）
- [x] LOS FreeRegionManager 的 baseUnit 偏移确保 RegionInfo 访问使用正确的全局索引

---

## 8. 后续工作（M2）

M2 将实现大对象分配分流：

1. `LOSManager::AllocLarge()` 实现完整分配逻辑——直接调用 `losFreeMgr.TakeRegion()`
2. `RegionManager::AllocLarge()` 增加 LOS 优先判断
3. RESERVE 借用触发机制
4. 降级到 MAIN 的统计和日志
