# M2 实现记录：大对象分配分流（LOS 优先 + MAIN 降级 + RESERVE 借用）

**实现日期**：2026年2月19日

---

## 1. 实现目标

M2 阶段目标：实现大对象分配分流，LOS 空间优先分配，失败时自动降级到 MAIN 空间；实现 RESERVE 借用机制，LOS 和 MAIN 均可从 RESERVE 缓冲区借用空间。

- `LOSManager::AllocLarge()` 实现步骤 1-3（losFreeMgr + inactiveZone + RESERVE 借用）
- `RegionManager::AllocLarge()` 增加 LOS 路由逻辑
- 原 `AllocLarge` 逻辑提取为 `AllocLargeInMain()`，作为降级路径（含步骤 4c RESERVE 借用）
- `ReserveManager` 封装 A+B 混合分块策略与内部缓冲区管理
- LOS region 列表管理（含 GC trace-phase 感知）
- 分配统计字段

### 当前实现范围

| 计划步骤 | 状态 | 说明 |
|----------|------|------|
| 步骤 1：LOS losFreeMgr 获取 | ✅ 已实现 | `losFreeMgr.TakeRegion()` |
| 步骤 2：LOS inactiveZone 扩展 | ✅ 已实现 | CAS 方式向低地址扩展 |
| 步骤 3：RESERVE 借用（LOS） | ✅ 已实现 | `reserveManager->BorrowForLOS(regionSize)` |
| 步骤 4：降级到 MAIN | ✅ 已实现 | `AllocLargeInMain()` |
| 步骤 4c：RESERVE 借用（MAIN） | ✅ 已实现 | `reserveManager->BorrowForMain(regionSize)` |
| 步骤 5-6：STW 整理 | 🔲 TODO | M4 实现 |
| 步骤 7：OOM | 已有 | 由 RegionSpace 层处理 |

---

## 2. 修改文件

### 2.1 ReserveManager.h / ReserveManager.cpp（新增）

**设计理念**：将 A+B 混合分块策略和缓冲区管理完全封装在 ReserveManager 内部，对外只暴露简单的 `BorrowForMain(size)` / `BorrowForLOS(size)` 接口。调用方（LOSManager、RegionManager）无需关心分块策略细节。

**公共 API**：

```cpp
// 初始化 RESERVE 区域边界
void Initialize(uintptr_t start, uintptr_t end, size_t unitSize);

// 借用空间（内部自动分块和缓冲）
uintptr_t BorrowForMain(size_t size);   // MAIN 方向：低→高
uintptr_t BorrowForLOS(size_t size);    // LOS 方向：高→低

// 归还空间（栈式归还）
void ReturnFromMain(uintptr_t addr, size_t size);
void ReturnFromLOS(uintptr_t addr, size_t size);

// 查询
size_t GetAvailable() const;            // 未提交的空间
size_t GetMainBorrowedSize() const;     // MAIN 已提交总量
size_t GetLOSBorrowedSize() const;      // LOS 已提交总量
size_t GetMainChunkBorrowCount() const; // MAIN 水位线提交次数
size_t GetLOSChunkBorrowCount() const;  // LOS 水位线提交次数
size_t GetMainBorrowBytes() const;      // MAIN 提交总字节
size_t GetLOSBorrowBytes() const;       // LOS 提交总字节
```

**A+B 混合分块策略**（私有方法 `ComputeBorrowChunk`）：

```
chunkSize = align_up(
    max(requestSize, max(minChunk, available * 10%)),
    alignment
)
如果 chunkSize > available，降级为精确请求大小
```

| 常量 | 值 | 含义 |
|------|----|------|
| `MIN_BORROW_CHUNK_UNITS` | 16 | 最小分块大小（16KB-page 系统上 = 256KB） |
| `BORROW_CHUNK_ALIGN_UNITS` | 16 | 分块对齐粒度（同上） |
| `BORROW_RATIO_DIVISOR` | 10 | 比例因子：available 的 10% |

**内部缓冲区机制**：

```
MAIN 缓冲区（低→高）：
  [mainBufCursor ... mainBufCeiling)  = 可用空间

LOS 缓冲区（高→低）：
  [losBufFloor ... losBufCursor)  = 可用空间

借用流程：
  1. 尝试从缓冲区分配（无需移动水位线）
  2. 缓冲区不足 → ComputeBorrowChunk 计算分块大小
  3. CommitChunkForMain/LOS 移动水位线提交新块
  4. 更新缓冲区边界 → 从新缓冲区分配
```

**水位线模型**：

```
RESERVE 区域:
  reserveStart                                              reserveEnd
       |                                                         |
       v                                                         v
       [=== MAIN 已提交 ===][--- 未提交 ---][=== LOS 已提交 ===]
                            ^               ^
                      mainWatermark    losWatermark

MAIN 从 reserveStart 向高地址提交（mainWatermark 右移）
LOS 从 reserveEnd 向低地址提交（losWatermark 左移）
两个水位线不可交叉

归还：栈式归还（只允许从水位线边缘归还），归还时若缓冲区与归还区域重叠则失效化缓冲区
```

### 2.2 LOSManager.h

**新增方法**：

```cpp
// M2: LOS 大对象分配入口
uintptr_t AllocLarge(size_t size);

// 将新分配的 region 加入对应的 region list（处理 GC trace-phase）
void EnlistLargeRegion(RegionInfo* region);

// 获取 trace-region 缓存
RegionCache& GetLargeTraceRegions();
```

**新增私有方法**：

```cpp
// 从 LOS inactive zone 分配（向低地址扩展）
RegionInfo* AllocFromInactiveZone(size_t unitCount);
```

**新增成员**：

```cpp
RegionCache losLargeTraceRegions;  // trace-phase region cache
size_t losReserveBorrowCount = 0;  // 从 RESERVE 借用成功的 LOS 分配次数
```

**新增统计接口**：

```cpp
void IncrementAllocCount();
size_t GetReserveBorrowCount() const;
```

### 2.3 LOSManager.cpp

**新增 include**：

```cpp
#include "Collector/Collector.h"
#include "Heap.h"
#include "ReserveManager.h"
```

**新增实现**：

- `LOSManager::AllocLarge(size_t size)`
  - 步骤 1：`losFreeMgr.TakeRegion()` 尝试从回收区域获取
  - 步骤 2：`AllocFromInactiveZone()` 尝试 inactiveZone 扩展
  - 步骤 3：`reserveManager->BorrowForLOS(regionSize)` 从 RESERVE 借用
  - 失败返回 0，由 RegionManager 处理降级

- `LOSManager::AllocFromInactiveZone(size_t unitCount)`
  - 使用 `compare_exchange_weak` CAS 向低地址方向扩展
  - 边界检查：`currentZone >= losStart + regionSize`
  - 成功后 `RegionInfo::InitRegionAt(newZone, ...)` 创建 region

- `LOSManager::EnlistLargeRegion(RegionInfo* region)`
  - 复用 RegionManager 的 trace-phase 感知逻辑
  - GC trace 阶段：放入 `losLargeTraceRegions`
  - 其他阶段：放入 `losRecentLargeRegionList`

### 2.4 RegionManager.h

**修改 include**：

```cpp
// 将 LOSManager 从前向声明改为完整 include
#include "LOSManager.h"
#include "ReserveManager.h"
```

**修改 `AllocLarge()`**：

```cpp
uintptr_t AllocLarge(size_t size)
{
    // Step 1-3: Try LOS allocation (losFreeMgr + inactiveZone + RESERVE borrow)
    if (losManager != nullptr) {
        uintptr_t addr = losManager->AllocLarge(size);
        if (addr != 0) {
            return addr;
        }
    }

    // Step 4: Fall back to MAIN space
    uintptr_t addr = AllocLargeInMain(size);
    if (addr != 0 && losManager != nullptr) {
        losManager->IncrementFallbackCount();
    }
    return addr;
}
```

**修改 `AllocLargeInMain()`**：

- 即原 `AllocLarge()` 的完整逻辑（`TakeRegion` + region list 管理）
- 新增步骤 4c：`reserveManager->BorrowForMain(regionSize)` 从 RESERVE 借用
- MAIN 原有分配路径完全不变，RESERVE 借用作为 inactiveZone 之后的额外尝试

---

## 3. 分配流程

```
RegionSpace::TryAllocateOnce(size >= largeObjectThreshold)
    │
    ▼
RegionManager::AllocLarge(size)
    │
    ├── losManager != nullptr ?
    │       │
    │       ▼ YES
    │   LOSManager::AllocLarge(size)
    │       │
    │       ├── Step 1: losFreeMgr.TakeRegion()  ← 回收区域
    │       │       成功 → EnlistLargeRegion → return addr
    │       │
    │       ├── Step 2: AllocFromInactiveZone()  ← 未激活区域
    │       │       成功 → EnlistLargeRegion → return addr
    │       │
    │       ├── Step 3: reserveManager->BorrowForLOS()  ← RESERVE 借用
    │       │       成功 → InitRegionAt → EnlistLargeRegion → return addr
    │       │
    │       └── return 0  (LOS 分配失败)
    │
    ▼ (LOS 失败 or losManager == nullptr)
RegionManager::AllocLargeInMain(size)
    │
    ├── Step 4a-b: TakeRegion() → garbageRegionList → freeRegionManager → inactiveZone
    │       成功 → enlist → return addr
    │
    ├── Step 4c: reserveManager->BorrowForMain()  ← RESERVE 借用
    │       成功 → InitRegionAt → enlist → return addr
    │
    └── return 0  (MAIN 也失败 → 上层触发 GC 或 OOM)
```

### 3.1 RESERVE 借用内部流程

```
BorrowForMain(size) / BorrowForLOS(size):
    │
    ├── [加锁 borrowMutex]
    │
    ├── 缓冲区命中？（MAIN: cursor+size ≤ ceiling, LOS: cursor ≥ floor+size）
    │       │
    │       ▼ YES
    │   直接从缓冲区分配 → return addr
    │
    ├── ComputeBorrowChunk(size)         ← A+B 混合策略
    │   chunkSize = align_up(max(size, max(minChunk, available*10%)), alignment)
    │
    ├── CommitChunkForMain/LOS(chunkSize)  ← 移动水位线
    │       失败 → return 0
    │
    ├── 更新缓冲区 [cursor, ceiling) / [floor, cursor)
    │
    └── 从新缓冲区分配 → return addr
```

---

## 4. LOS inactiveZone 扩展机制

```
初始状态：
losStart                                               losInactiveZone = losEnd
    |                   (全部未激活)                          |
    v                                                        v
    [  ............... inactive zone ......................  ]

分配后：
losStart              losInactiveZone (已移动)                losEnd
    |                        |                                   |
    v                        v                                   v
    [ ... inactive zone ... ][ region1 ][ region2 ][ region3 ]

方向：losInactiveZone 向低地址（losStart 方向）扩展
线程安全：使用 compare_exchange_weak (CAS) 保证并发安全
```

---

## 5. Region List 管理

LOS 有独立的 region list，与 MAIN 的 region list 分离：

| Region List | 用途 | 对应 MAIN |
|-------------|------|-----------|
| `losOldLargeRegionList` | LOS 老生代大对象 | `oldLargeRegionList` |
| `losRecentLargeRegionList` | LOS 新近分配大对象 | `recentLargeRegionList` |
| `losLargeTraceRegions` | GC trace 阶段分配的 LOS 大对象 | `largeTraceRegions` |

---

## 6. 统计字段

| 字段 | 含义 | 位置 |
|------|------|------|
| `losAllocCount` | LOS 分配成功次数 | LOSManager |
| `losAllocFailCount` | LOS 分配失败次数（触发降级） | LOSManager |
| `losFallbackCount` | 降级到 MAIN 的次数 | LOSManager |
| `losReserveBorrowCount` | 通过 RESERVE 借用成功的 LOS 分配次数 | LOSManager |
| `mainReserveBorrowCount` | 通过 RESERVE 借用成功的 MAIN 分配次数 | RegionManager |
| `mainChunkBorrowCount` | MAIN 方向水位线实际提交次数 | ReserveManager |
| `losChunkBorrowCount` | LOS 方向水位线实际提交次数 | ReserveManager |
| `mainBorrowBytes` | MAIN 方向水位线提交总字节 | ReserveManager |
| `losBorrowBytes` | LOS 方向水位线提交总字节 | ReserveManager |

> **说明**：`losReserveBorrowCount` / `mainReserveBorrowCount` 是调用方的"成功次数"统计；
> `mainChunkBorrowCount` / `losChunkBorrowCount` 是 ReserveManager 内部的"水位线移动次数"，
> 因为 A+B 分块策略会缓冲多余空间，所以 chunk 提交次数通常远小于借用成功次数。

---

## 7. 验收状态

- [x] 编译通过 (native debug)
- [x] 无新增错误或警告
- [x] `losManager == nullptr` 时完全走原路径（向后兼容）
- [x] LOS 分配流程：losFreeMgr → inactiveZone → RESERVE 借用 → 降级 MAIN
- [x] MAIN 分配流程增加步骤 4c：RESERVE 借用
- [x] RESERVE 借用策略封装在 ReserveManager 内部（A+B 混合分块 + 缓冲区）
- [x] Region list 管理含 GC trace-phase 感知
- [x] 统计字段正确递增（调用方计数 + ReserveManager 内部计数）

---

## 8. TODO（待后续里程碑完善）— 完成状态

### 需着重处理的部分

1. **LOS 与 MAIN 空间的边界交互**：✅ **M3 已完成**
   - LOS 分配失败降级到 MAIN 后，MAIN 空间的 region list 与 LOS 的 region list 如何在 GC 时统一遍历
   - `CollectLargeGarbage()` 需要同时处理 MAIN 和 LOS 的大对象回收
   - *实现*：M3 的 `CollectLOSGarbage()` + `MigrateLargeObjectsFromMain()` 解决了统一遍历和回收

2. **RESERVE 归还与缩容**：✅ **M3-M5 已完成**
   - 当前 `ReturnFromMain` / `ReturnFromLOS` 仅支持栈式归还（从水位线边缘）
   - GC 回收后批量归还 RESERVE 借用空间的完整流程
   - 缓冲区失效化策略的完善
   - *实现*：M5 的 `pendingReserveReturns` 批量归还 + cooldown/hysteresis 控制抖动

3. **整理与缩容**（步骤 5-6）：✅ **M4a/M4b 已完成**
   - `LOSManager::CompactLOS()` STW 整理
   - `LOSManager::ShrinkToReserve()` 缩容归还
   - 整理期间分配直接走 MAIN（`IsCompacting()` 标志）
   - *实现*：M4a 的 7 阶段滑动整理 + 异步整理；M4b 的 MAIN 低地址优先 + ShrinkInactiveZone

4. **GC 集成**（M3）：✅ **M3 已完成**
   - LOS region 的标记-清扫回收
   - `AssembleLOSGarbageCandidates()` recent → old 合并
   - 回收后 region 归还 `losFreeMgr`
   - RESERVE 借用区域的 region 回收后归还 ReserveManager

---

## 9. 后续工作（M3）— ✅ 已完成

M3 已实现 LOS 回收（标记-清扫），详见 `M3_Implementation_Record.md`：

1. ✅ `LOSManager::CollectLOSGarbage()` — 遍历 losOldLargeRegionList 回收未标记对象
2. ✅ `LOSManager::MigrateLargeObjectsFromMain()` — GC 后将 MAIN 大对象迁入 LOS
3. ✅ 引用更新 — migrationMap + `UpdateMigratedReferences()` 全堆扫描
4. ✅ `WCollector::PostTrace()` 调用 LOS 回收链路
