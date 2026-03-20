# MSRegionManager 独立化迁移设计

> 目标：将 MS-GC 从共享 CopyGC 的 RegionManager 迁移到独立的 MSRegionManager，
> 并将 MSRegionInfo[] 直接放置在堆前方元数据区（占据原 UnitInfo[] 的空间）。

> 2026-03-19 代码复核结论：这份文档已有相当一部分内容被当前实现吸收，但文档本身没有同步收敛，属于“历史设计 + 部分已落地事实”混合态。阅读时应优先参考下面的状态摘要，再看后续分章节设计。

## 0. 代码复核状态摘要

### 0.1 已基本实现

1. 已存在独立的 `MSRegionManager`，不再走“MS 共享 CopyGC RegionManager 做分配/回收”的旧主路径。
2. `RegionSpace::Init()` 的 MS 分支已切到 `MSRegionManager::ComputeMaxVirtualHeapSize()`、`GetVirtualReserveSize()` 和 `Initialize(heapSize, mmapBase, maxVirtualHeapSize)`。
3. `MSRegionInfo[]` 的主路径已放到 mmap 前端，`MSRegionInfoArray::Init(..., array)` 默认接收 `MSRegionManager` 提供的外部数组，不再走独立 malloc 主路径。
4. `CentralRegionPool::GetFreshRegion()`、`ReleaseRegionInternal()`、`SweepLargeRegions()` 已直接调用 `MSRegionManager::TakeRegion()`、`ReclaimRegion()`、`ReclaimRegionLarge()`。
5. 已有 `GrowOneSegment()`、`ReleaseGarbageRegions()`、`ShrinkHeap()`，说明“虚拟大预留 + committed 增长/回收”的基础能力已经落地。

### 0.2 部分实现

1. `MSRegionInfoArray` 没有完全删除，只是主路径已切到 mmap-front array，同时保留了 `malloc` fallback 和 `Cleanup()` 中的 `ownedByMalloc_` 兼容逻辑。
2. `RegionInfo` / `RegionManager` 没有被彻底从 MS 编译路径中隔离，当前仍保留部分兼容用途，例如 `RegionInfo::Initialize(...)` 仍在 MS 初始化阶段被调用。
3. 堆增长设计已部分内联进 `MSRegionManager`，但不是文档里单独 `HeapGrowthManager` 类的形态。

### 0.3 未按文档原方案实现

1. 文档中“CartesianTree 保持 Unit 索引语义”的推荐方案已经过时；当前代码已切到 **region index / region count** 粒度。
2. 文档中单独的 `HeapGrowthManager`、`HeapGoalController`、`Scavenger` 类当前并不存在。
3. 文档中 `Step F` 所描述的“通过 `#ifndef MRT_USE_MARK_SWEEP` 完全隔离 CopyGC 的 RegionManager/RegionInfo”当前未完成。

---

## 目录

1. [现状分析](#1-现状分析)
2. [MSRegionInfo[] 放置堆头的可行性分析](#2-msregioninfo-放置堆头的可行性分析)
3. [新内存布局设计](#3-新内存布局设计)
4. [MSRegionManager 设计](#4-msregionmanager-设计)
5. [迁移步骤](#5-迁移步骤)
6. [风险与回退](#6-风险与回退)
7. [堆增长方案设计](#7-堆增长方案设计)

---

## 1. 现状分析

### 1.1 当前堆 mmap 布局

RegionSpace::Init() 通过 `MemMap::MapMemory()` 一次性 mmap 整个堆，布局如下：

```
低地址 ──────────────────────────────────────────────── 高地址
┌─────────────────────────┬──────────────────────────────┐
│    UnitInfo[] 镜像区     │       Heap Region 空间        │
│   (metadataSize bytes)  │  (nUnit × UNIT_SIZE bytes)   │
└─────────────────────────┴──────────────────────────────┘
↑                         ↑                              ↑
regionInfoStart       heapStart                      heapEnd
(mmap base)           (regionInfoStart + metadataSize)
```

- `metadataSize = num_units × sizeof(RegionInfo)` — 向上页对齐
- `sizeof(RegionInfo) == sizeof(UnitInfo) ≈ 120B`（`RegionInfo.h:65` 注释强制要求相等）
- UnitInfo[i] 位于 `heapStart - (i+1) × sizeof(UnitInfo)` — **倒序镜像**数组

### 1.2 数量级分析（256MB 堆）

| 平台 | UNIT_SIZE | Unit 数量 | sizeof(UnitInfo) | 镜像区大小 |
|------|-----------|-----------|------------------|-----------|
| macOS aarch64 | 16KB | 16384 | ~120B | **~1.9MB** |
| Linux x86_64 | 4KB | 65536 | ~120B | **~7.5MB** |

### 1.3 MS 模式现状：两套元数据共存

当前 MS-GC 同时维护：

| 元数据 | 存储位置 | 大小 | 管理方式 |
|--------|---------|------|---------|
| **UnitInfo[]** | 堆 mmap 前段（镜像区） | ~1.9MB | RegionManager，为 CopyGC 设计 |
| **MSRegionInfo[]** | 独立 malloc | ~512KB | MSRegionInfoArray，为 MS 设计 |

这导致：
1. **内存浪费**：UnitInfo[] 的 ~120B/Unit 大部分字段 MS 不使用（routeInfo、Ghost 字段、rwLock 等）
2. **双重管理**：同一块 Region 同时有 UnitInfo 和 MSRegionInfo，两者必须保持一致
3. **额外 malloc**：MSRegionInfo[] 单独 malloc，增加碎片和 TLB 压力
4. **查找路径复杂**：地址 → MSRegionInfo 需要 `GetRegionInfoValidated()` 的启发式校正（因 UNIT_SIZE ≠ MS_REGION_SIZE）

### 1.4 MS 对 RegionManager 的实际依赖

通过全量代码扫描，MS 模式仅使用 RegionManager 的 5 个核心接口：

| 接口 | 调用次数 | 用途 |
|------|---------|------|
| `Initialize()` | 1 | 建立堆布局 |
| `TakeRegion(n, uclass, physMem)` | 3 | 分配新 Region（小对象/大对象） |
| `ReclaimRegion(region)` | 2 | 回收空闲 Region |
| `GetRegionHeapStart()` | 1 | 获取堆起始地址 |
| `UNIT_SIZE` | 多处 | 单元大小常量 |

而 RegionManager 的 619 行代码中，**14 个 RegionList**、Ghost Region 管理、GarbageCandidate 选择等全部是 CopyGC 专有。

---

## 2. MSRegionInfo[] 放置堆头的可行性分析

### 2.1 核心问题

> **MSRegionInfo[] 能否直接放在堆 mmap 的前段，占据原 UnitInfo[] 的位置？**

**结论：完全可行，且有显著优势。**

### 2.2 空间充裕性

以 256MB 堆为例：

| 指标 | CopyGC UnitInfo[] | MS MSRegionInfo[] |
|------|-------------------|-------------------|
| 粒度 | 16KB (UNIT_SIZE) | 32KB (MS_REGION_SIZE) |
| 元素数量 | 16384 | 8192 |
| 每元素大小 | ~120B | **64B** |
| 镜像区总大小 | **~1.9MB** | **~512KB** |

MSRegionInfo[] 仅需 512KB，远小于现有 UnitInfo[] 的 1.9MB。**即使完全复用原来的 mmap，也绰绰有余。**

### 2.3 三种放置方案对比

#### 方案 A：正向数组 + 重新计算 MetadataSize（推荐）

```
低地址 ────────────────────────────────────────────── 高地址
┌──────────────┬────────────────────────────────────────┐
│MSRegionInfo[]│        32KB Region 0, 1, 2, ...        │
│ (512KB)      │                                        │
└──────────────┴────────────────────────────────────────┘
↑              ↑                                        ↑
mmap_base   heapStart = mmap_base + 512KB(页对齐)     heapEnd
```

- MSRegionInfo[i] = `mmap_base + i × sizeof(MSRegionInfo)`
- `GetMetadataSize_MS(n) = n × 64` 向上页对齐
- **完全替换 UnitInfo[]**，MS 模式不再需要 RegionManager

| 优势 | 说明 |
|------|------|
| 零额外分配 | 不再 malloc MSRegionInfo[]，随堆 mmap 一次分配 |
| 节省 1.4MB | 512KB vs 1.9MB |
| O(1) 查找极简 | `idx = (addr - heapStart) / MS_REGION_SIZE` → `array[idx]` |
| 无对齐歧义 | MSRegionInfo 粒度 = MS_REGION_SIZE = 32KB，天然对齐 |
| Cache 友好 | 64B = 1 cache line，遍历时每个 entry 恰好一个 cache line |

| 劣势 | 说明 |
|------|------|
| 需完全独立 MSRegionManager | 不能再依赖 CopyGC 的 RegionManager/FreeRegionManager |
| 改造工作量较大 | 需改写 RegionSpace::Init()、TakeRegion/ReclaimRegion 等 |

#### 方案 B：正向数组放入现有 UnitInfo[] 空间（过渡方案）

```
低地址 ────────────────────────────────────────────── 高地址
┌──────────────┬─────────────┬─────────────────────────┐
│MSRegionInfo[]│  未使用空间  │    Heap Region 空间       │
│ (512KB)      │  (~1.4MB)   │                          │
└──────────────┴─────────────┴─────────────────────────┘
↑              ↑             ↑                          ↑
mmap_base  512KB         heapStart(1.9MB)           heapEnd
```

- 保留原有 mmap 大小（不改 `GetMetadataSize()` 计算）
- MSRegionInfo[] 放在 `regionInfoStart` 开始的正向数组
- 同时保留 UnitInfo[] 镜像区（两者不冲突，UnitInfo 从 heapStart 向低地址增长，MSRegionInfo 从 mmap_base 向高地址增长）

| 优势 | 说明 |
|------|------|
| 改动极小 | 仅改 `MSRegionInfoArray::Init()`，把 malloc 换成直接使用 mmap 地址 |
| 可渐进迁移 | CopyGC RegionManager 仍可工作，MS 逐步接管 |

| 劣势 | 说明 |
|------|------|
| 浪费 ~1.4MB | UnitInfo[] 空间仍被保留但未使用 |
| 仍有双重管理 | 两套元数据共存直到 CopyGC 依赖完全移除 |

#### 方案 C：镜像数组（与 UnitInfo 相同模式） — 不推荐

- 按 UnitInfo 的倒序模式放 MSRegionInfo
- 问题：`sizeof(MSRegionInfo) = 64 ≠ sizeof(UnitInfo) ≈ 120`，所有镜像公式都需要改
- 增加复杂度，无额外收益

### 2.4 推荐路径

**两阶段迁移**：

1. **Phase 1（立即可做）**：方案 B — MSRegionInfo[] 放在现有 mmap 前端，保留 UnitInfo[]
2. **Phase 2（完整替换）**：方案 A — 独立 MSRegionManager，重新计算 MetadataSize，删除 UnitInfo[]

> 复核备注：当前代码已经越过这里描述的“Phase 1/Phase 2 规划态”，主路径实际上已处于“独立 MSRegionManager + mmap-front MSRegionInfo[] + 虚拟大预留”的后期形态。后文的部分步骤因此应按“历史迁移说明”理解，而不是待做清单。

### 2.5 关键约束确认

| 约束 | 是否满足 | 说明 |
|------|---------|------|
| `allocPtr` 在偏移 0 | ✅ | MSRegionInfo.allocPtr 仍在 offset 0 |
| 正向数组与镜像数组不冲突 | ✅ | UnitInfo 从 heapStart 向下增长，MSRegionInfo 从 mmap_base 向上增长，不重叠（512KB << 1.9MB） |
| 堆地址计算不变 | ✅ | heapStart/heapEnd 不变，对象分配地址不受影响 |
| CartesianTree 可复用 | ✅ | 仅需将索引语义从 "unit idx" 调整为 "region idx" |
| 编译器 fast-path 兼容 | ✅ | `AllocBuffer::tlRegion->allocPtr` 仍在 offset 0 |

---

## 3. 新内存布局设计

### 3.1 Phase 1：共存布局（方案 B）

不改 mmap 大小，MSRegionInfo[] 叠加到现有 metadata 区域的低地址端：

```
低地址 ──────────────────────────────────────────────────────── 高地址

mmap_base                                      heapStart              heapEnd
    │                                              │                     │
    v                                              v                     v
    ┌────────────┬──────────────┬───────────────────┬─────────────────────┐
    │ MSRegionInfo[] │  未使用   │  UnitInfo[] 镜像  │   Heap Regions      │
    │  (正向增长)    │  padding  │  (反向增长)       │   (32KB × N)        │
    │  512KB         │  ~900KB  │  ~480KB 有效      │                      │
    └────────────┴──────────────┴───────────────────┴─────────────────────┘

    |<------------- metadataSize (~1.9MB) ---------->|
```

**内存映射关系**（256MB heap, macOS aarch64）：

```
MSRegionInfo[0]   @ mmap_base + 0           → 对应 Region[0] @ heapStart + 0
MSRegionInfo[1]   @ mmap_base + 64          → 对应 Region[1] @ heapStart + 32KB
MSRegionInfo[i]   @ mmap_base + i × 64      → 对应 Region[i] @ heapStart + i × 32KB
MSRegionInfo[8191] @ mmap_base + 524224     → 对应 Region[8191]

UnitInfo[0]       @ heapStart - 120         → 对应 Unit[0] @ heapStart + 0
UnitInfo[1]       @ heapStart - 240         → 对应 Unit[1] @ heapStart + 16KB
```

**安全检验**：MSRegionInfo[] 占 512KB，UnitInfo[] 从 heapStart 向下使用 ~1.9MB。
两者是否重叠？

```
MSRegionInfo[] 范围: [mmap_base, mmap_base + 512KB)
UnitInfo[]     范围: [heapStart - 1.9MB, heapStart) = [mmap_base, mmap_base + 1.9MB)
```

**重叠！** UnitInfo[] 使用整个 metadata 区域（从 mmap_base 开始）。MSRegionInfo[] 的低 512KB 会覆盖 UnitInfo[] 的高 index 端（即堆中最高地址 Region 对应的 UnitInfo）。

**解决方案**：Phase 1 中 MSRegionInfo[] 与 UnitInfo[] **不能同时存在于同一个 mmap 区域**。有两种处理方式：

- **Phase 1a**：MSRegionInfo[] 仍然 malloc，但标记为"待迁移到 mmap"
- **Phase 1b**：直接进入 Phase 2（推荐，见下文）

### 3.2 Phase 2：独立布局（方案 A — 推荐最终方案）

**完全替换 UnitInfo[] 为 MSRegionInfo[]**，重新计算 MetadataSize：

```
低地址 ──────────────────────────────────────────────────── 高地址

mmap_base          heapStart                            heapEnd
    │                  │                                    │
    v                  v                                    v
    ┌──────────────────┬────────────────────────────────────┐
    │  MSRegionInfo[]  │         Heap Regions               │
    │  (正向数组)       │   Region[0] | Region[1] | ...      │
    │  512KB (页对齐)  │   32KB × 8192 = 256MB              │
    └──────────────────┴────────────────────────────────────┘

    |<-- metadataMS -->|<--------- heapSize = 256MB ------->|
    metadataMS = 8192 × 64 = 524288 → 页对齐 = 524288 (32 pages × 16KB)
```

**地址计算公式**（全部 O(1)）：

```cpp
// 堆地址 → Region 索引
size_t GetRegionIdx(uintptr_t addr) {
    return (addr - heapStart) / MS_REGION_SIZE;   // 32KB 整除
}

// Region 索引 → MSRegionInfo
MSRegionInfo* GetRegionInfo(size_t idx) {
    return reinterpret_cast<MSRegionInfo*>(mmap_base + idx * sizeof(MSRegionInfo));
    // 等价于: &array[idx]   (因 array == mmap_base)
}

// 堆地址 → MSRegionInfo（组合）
MSRegionInfo* GetRegionInfoAt(uintptr_t addr) {
    return GetRegionInfo(GetRegionIdx(addr));
}

// Region 索引 → 堆地址
uintptr_t GetRegionAddress(size_t idx) {
    return heapStart + idx * MS_REGION_SIZE;
}
```

### 3.3 各平台内存开销对比

| 平台 | UNIT_SIZE | 堆大小 | CopyGC UnitInfo[] | MS MSRegionInfo[] | 节省 |
|------|-----------|--------|-------------------|-------------------|------|
| macOS aarch64 | 16KB | 256MB | 1.9MB | **512KB** | **1.4MB (73%)** |
| macOS aarch64 | 16KB | 1GB | 7.5MB | **2MB** | **5.5MB (73%)** |
| Linux x86_64 | 4KB | 256MB | 7.5MB | **512KB** | **7MB (93%)** |
| Linux x86_64 | 4KB | 1GB | 30MB | **2MB** | **28MB (93%)** |

> **Linux 获益最大**：因为 UNIT_SIZE=4KB 导致 Unit 数量是 macOS 的 4 倍，UnitInfo[] 开销更可怕。
> MS 模式统一使用 32KB Region 粒度，跨平台一致。

### 3.4 MetadataSize 计算（MS 模式）

```cpp
// 新增：MS 模式下的 metadata 大小计算
static size_t GetMetadataSize_MS(size_t heapSize) {
    size_t regionCount = heapSize / MS_REGION_SIZE;  // 32KB per region
    size_t rawSize = regionCount * sizeof(MSRegionInfo);  // 64B per region
    return RoundUp<size_t>(rawSize, MRT_PAGE_SIZE);  // 页对齐
}

// MS 模式 mmap 总大小
static size_t GetHeapMemorySize_MS(size_t heapSize) {
    heapSize = RoundUp<size_t>(heapSize, MS_REGION_SIZE);
    return GetMetadataSize_MS(heapSize) + heapSize;
}
```

### 3.5 与 CartesianTree 的索引语义对齐

当前 CartesianTree 的索引单位是 "Unit"（UNIT_SIZE = 16KB/4KB）。
MS 的 Region 粒度是 32KB。对齐方式：

| 方案 | CartesianTree 索引语义 | 粒度 | 说明 |
|------|----------------------|------|------|
| **A. 保持 Unit 索引** | 1 index = UNIT_SIZE | 16KB (macOS) | MS Region = 2 个 Unit，TakeRegion(2) |
| **B. 改为 Region 索引** | 1 index = MS_REGION_SIZE | 32KB | 更直观，但需修改 CartesianTree |

**推荐方案 A**：保持 Unit 索引不变。

> 复核备注：这一条已过时。当前 `MSRegionManager` 中的 `dirtyTree_` / `releasedTree_` 已经按 **region index/count** 工作，而不再按 unit index/count 工作。

理由：
- CartesianTree 已经支持任意 `num` 参数的 TakeUnits
- 大对象分配需要 N 个连续 Unit，Unit 粒度更灵活
- 不需要修改 CartesianTree 代码
- 索引转换：`regionIdx = unitIdx / (MS_REGION_SIZE / UNIT_SIZE)` 即 `unitIdx / 2` (macOS)

---

## 4. MSRegionManager 设计

### 4.1 类结构（~200 行）

```cpp
class MSRegionManager {
public:
    // ========== 初始化 ==========
    static size_t GetHeapMemorySize(size_t heapSize);
    void Initialize(size_t heapSize, uintptr_t mmapBase);

    // ========== Region 分配/回收（对应 RegionManager 的 5 个核心接口） ==========
    MSRegionInfo* TakeRegion();                      // 分配 1 个 32KB Region（小对象）
    MSRegionInfo* TakeRegionForLarge(size_t nUnit);  // 分配 N 个连续 Unit（Tier1 大对象）
    void ReclaimRegion(MSRegionInfo* info);           // 回收 Region 到空闲池
    void ReleaseRegion(MSRegionInfo* info);           // 回收 + madvise 释放物理页

    // ========== 地址查找（O(1)） ==========
    MSRegionInfo* GetRegionInfoAt(uintptr_t addr);
    MSRegionInfo* GetRegionInfoByIdx(size_t idx);
    uintptr_t     GetRegionAddress(size_t idx);
    size_t        GetRegionIdx(uintptr_t addr);

    // ========== 堆属性 ==========
    uintptr_t GetHeapStart() const { return heapStart_; }
    uintptr_t GetHeapEnd()   const { return heapEnd_; }
    size_t    GetMaxRegionCount() const { return maxRegionCount_; }
    bool      IsHeapAddress(uintptr_t addr) const;

    // ========== 堆统计 ==========
    size_t GetActiveRegionCount() const;

private:
    // ========== 内部成员 ==========
    MSRegionInfo*  array_;            // = mmap_base，正向数组
    uintptr_t      heapStart_;        // 第一个 Region 的起始地址
    uintptr_t      heapEnd_;          // 堆末尾
    size_t         maxRegionCount_;   // heapSize / MS_REGION_SIZE
    size_t         maxUnitCount_;     // heapSize / UNIT_SIZE

    // Region 空闲池：复用 FreeRegionManager（CartesianTree）
    // 但索引语义仍为 Unit 粒度
    CartesianTree  dirtyUnitTree_;     // 含脏数据的空闲 Unit
    CartesianTree  releasedUnitTree_;  // 已 madvise 的空闲 Unit
    std::mutex     dirtyMutex_;
    std::mutex     releasedMutex_;

    // 高水位扩展
    std::atomic<uintptr_t> inactiveZone_;  // 下一个可扩展的堆地址
};
```

### 4.2 Initialize 实现

```cpp
void MSRegionManager::Initialize(size_t heapSize, uintptr_t mmapBase)
{
    heapSize = RoundUp<size_t>(heapSize, MS_REGION_SIZE);
    maxRegionCount_ = heapSize / MS_REGION_SIZE;
    maxUnitCount_ = heapSize / RegionInfo::UNIT_SIZE;

    // MetadataSize: MSRegionInfo[] 直接位于 mmap 前端
    size_t metadataSize = RoundUp<size_t>(
        maxRegionCount_ * sizeof(MSRegionInfo), MRT_PAGE_SIZE);

    array_ = reinterpret_cast<MSRegionInfo*>(mmapBase);
    heapStart_ = mmapBase + metadataSize;
    heapEnd_ = heapStart_ + heapSize;
    inactiveZone_.store(heapStart_, std::memory_order_relaxed);

    // 零初始化 MSRegionInfo[]
    memset(array_, 0, maxRegionCount_ * sizeof(MSRegionInfo));
    for (size_t i = 0; i < maxRegionCount_; i++) {
        array_[i].regionType = MSRegionType::MS_REGION_FREE;
        array_[i].nextRegionIdx = MSRegionInfo::INVALID_IDX;
        array_[i].prevRegionIdx = MSRegionInfo::INVALID_IDX;
    }

    // 初始化 CartesianTree 空闲池
    dirtyUnitTree_.Init(maxUnitCount_);
    releasedUnitTree_.Init(maxUnitCount_);

    LOG(RTLOG_INFO, "[MS-GC] MSRegionManager::Initialize: "
        "heap [0x%zx, 0x%zx), regions=%zu, metadata=%zuKB, array@%p",
        heapStart_, heapEnd_, maxRegionCount_, metadataSize / 1024, array_);
}
```

### 4.3 TakeRegion 实现

保持 RegionManager 的三级查找策略，但不再需要 14 个 RegionList：

```cpp
MSRegionInfo* MSRegionManager::TakeRegion()
{
    constexpr size_t unitsPerRegion = MS_REGION_SIZE / RegionInfo::UNIT_SIZE;  // = 2 on macOS

    // Level 1: 尝试从 dirty tree 取空闲 Unit
    CartesianTree::Index idx = 0;
    {
        std::unique_lock<std::mutex> lk(dirtyMutex_, std::try_to_lock);
        if (lk.owns_lock() && dirtyUnitTree_.TakeUnits(unitsPerRegion, idx)) {
            return InitRegionFromUnits(idx, unitsPerRegion);
        }
    }

    // Level 2: 尝试从 released tree 取空闲 Unit
    {
        std::unique_lock<std::mutex> lk(releasedMutex_, std::try_to_lock);
        if (lk.owns_lock() && releasedUnitTree_.TakeUnits(unitsPerRegion, idx)) {
            return InitRegionFromUnits(idx, unitsPerRegion);
        }
    }

    // Level 3: 扩展 inactiveZone
    uintptr_t old = inactiveZone_.load(std::memory_order_acquire);
    uintptr_t next = old + MS_REGION_SIZE;
    if (next <= heapEnd_ &&
        inactiveZone_.compare_exchange_strong(old, next, std::memory_order_acq_rel)) {
        size_t unitIdx = (old - heapStart_) / RegionInfo::UNIT_SIZE;
        return InitRegionFromUnits(unitIdx, unitsPerRegion);
    }

    return nullptr;  // OOM
}

MSRegionInfo* MSRegionManager::InitRegionFromUnits(size_t unitIdx, size_t nUnit)
{
    // 清零 Region 内存（dirty units 需要 memset）
    uintptr_t regionAddr = heapStart_ + unitIdx * RegionInfo::UNIT_SIZE;
    // ... ClearUnits if taken from dirty tree ...

    // 计算 region 索引 → MSRegionInfo
    size_t regionIdx = unitIdx / (MS_REGION_SIZE / RegionInfo::UNIT_SIZE);
    MSRegionInfo* info = &array_[regionIdx];

    info->allocPtr = regionAddr;
    info->regionEnd = regionAddr + MS_REGION_SIZE;
    info->regionType = MSRegionType::MS_REGION_FREE;  // 调用方设置具体类型

    return info;
}
```

### 4.4 ReclaimRegion 实现

```cpp
void MSRegionManager::ReclaimRegion(MSRegionInfo* info)
{
    size_t regionIdx = static_cast<size_t>(info - array_);
    size_t unitIdx = regionIdx * (MS_REGION_SIZE / RegionInfo::UNIT_SIZE);
    size_t nUnit = MS_REGION_SIZE / RegionInfo::UNIT_SIZE;

    // 释放 bitmap
    info->ReleaseBitmaps();
    info->Reset();

    // 归还到 dirty tree
    std::lock_guard<std::mutex> lg(dirtyMutex_);
    dirtyUnitTree_.MergeInsert(unitIdx, nUnit, true);
}
```

### 4.5 与现有组件的接口映射

| 现有调用 | 现有接口 | 新接口 |
|---------|---------|--------|
| `regionManager.TakeRegion(2, TL_REGION, true)` | `RegionManager::TakeRegion` | `msRegionManager.TakeRegion()` |
| `regionManager.TakeRegion(n, LARGE_REGION, true)` | `RegionManager::TakeRegion` | `msRegionManager.TakeRegionForLarge(n)` |
| `regionManager.ReclaimRegion(rawRegion)` | `RegionManager::ReclaimRegion` | `msRegionManager.ReclaimRegion(msInfo)` |
| `MSRegionInfoArray::GetRegionInfo(addr)` | 独立 malloc 数组 | `msRegionManager.GetRegionInfoAt(addr)` |
| `RegionInfo::GetRegionInfoAt(addr)` | UnitInfo 镜像 | `msRegionManager.GetRegionInfoAt(addr)` |

### 4.6 消除 的层级

当前 Sweep 回收链路有两层间接：

```
CentralRegionPool::ReleaseRegionInternal(MSRegionInfo*)
  → RegionInfo::GetRegionInfoAt(addr)         // 反查 UnitInfo
  → RegionManager::ReclaimRegion(RegionInfo*)  // 归还 CopyGC 层
```

迁移后简化为：

```
CentralRegionPool::ReleaseRegionInternal(MSRegionInfo*)
  → MSRegionManager::ReclaimRegion(MSRegionInfo*)   // 直接归还
```

**省去一层指针解引用 + UnitInfo 查找。**

---

## 5. 迁移步骤

### 5.1 整体计划

| 步骤 | 内容 | 改动文件 | 估计行数 |
|------|------|---------|---------|
| **Step A** | 新增 MSRegionManager 类 | 新文件 `MSRegionManager.h/.cpp` | ~250 行 |
| **Step B** | 修改 RegionSpace::Init() MS 分支 | `RegionSpace.cpp` | ~30 行 |
| **Step C** | 迁移 TakeRegion 调用点 | `RegionSpace.cpp`, `CentralRegionPool.cpp` | ~40 行 |
| **Step D** | 迁移 ReclaimRegion 调用点 | `CentralRegionPool.cpp` | ~20 行 |
| **Step E** | 删除 MSRegionInfoArray 的 malloc | `MSRegionInfo.h/.cpp` | -50 行 |
| **Step F** | 条件编译隔离 CopyGC 代码 | `RegionManager.h`, `RegionInfo.h` | ~10 行 |
| **总计** | | | **净增 ~200 行** |

> 复核备注：按当前代码看，Step A-D 已基本完成；Step E 为部分完成；Step F 未完成。

### 5.2 Step A：新增 MSRegionManager

创建 `src/Heap/Allocator/MSRegionManager.h` 和 `MSRegionManager.cpp`：

```cpp
// MSRegionManager.h — 核心类声明
// 见 §4.1 的类结构

// MSRegionManager.cpp — 实现
// 包含: Initialize, TakeRegion, TakeRegionForLarge, ReclaimRegion, ReleaseRegion
// 复用: CartesianTree（直接使用，无需修改）
```

**CMakeLists.txt 加入新文件**：
```cmake
# 在 MS 源文件列表中添加
set(MS_SOURCES
    ...
    Allocator/MSRegionManager.cpp
)
```

### 5.3 Step B：修改 RegionSpace::Init()

当前 `RegionSpace::Init()` 流程：

```cpp
// 现有代码（RegionSpace.cpp:189-211）
size_t totalSize = RegionManager::GetHeapMemorySize(heapSize);  // CopyGC 计算
size_t unitNum = RegionManager::GetHeapUnitCount(heapSize);
map = MemMap::MapMemory(totalSize, totalSize, opt);
MAddress metadata = map->GetBaseAddr();
regionManager.Initialize(unitNum, metadata);   // CopyGC 初始化
```

改为 MS 分支：

```cpp
#ifdef MRT_USE_MARK_SWEEP
    // MS 模式：使用更小的 MetadataSize
    size_t totalSize = MSRegionManager::GetHeapMemorySize(heapSize);
    map = MemMap::MapMemory(totalSize, totalSize, opt);
    MAddress base = map->GetBaseAddr();
    msRegionManager_.Initialize(heapSize, base);
    reservedStart = msRegionManager_.GetHeapStart();
    reservedEnd = msRegionManager_.GetHeapEnd();
    // 不再调用 RegionInfo::Initialize 和 regionManager.Initialize
#else
    // CopyGC 原有路径不变
    size_t totalSize = RegionManager::GetHeapMemorySize(heapSize);
    ...
#endif
```

**同时删除**：`MSRegionInfoArray::Init()` 调用（已由 MSRegionManager 内部完成）。

### 5.4 Step C：迁移 TakeRegion 调用点

需要修改的 3 个 MS 模式 TakeRegion 调用：

| 位置 | 当前代码 | 迁移后 |
|------|---------|--------|
| `RegionSpace::TryAllocateOnceMS` (小对象) | `regionManager.TakeRegion(2, TL_REGION, true)` | `msRegionManager_.TakeRegion()` |
| `RegionSpace::TryAllocateOnceMS` (大对象) | `regionManager.TakeRegion(nUnit, LARGE_REGION, true)` | `msRegionManager_.TakeRegionForLarge(nUnit)` |
| `CentralRegionPool::RefillFromFreePool` | `regionManager.TakeRegion(...)` | `msRegionManager_.TakeRegion()` |

同时，TakeRegion 返回值从 `RegionInfo*` 变为 `MSRegionInfo*`，不再需要 `MSRegionInfoArray::GetRegionInfo()` 反查。

### 5.5 Step D：迁移 ReclaimRegion 调用点

需要修改的 2 个 MS 模式 ReclaimRegion 调用：

| 位置 | 当前代码 | 迁移后 |
|------|---------|--------|
| `CentralRegionPool::ReleaseRegionInternal` | `RegionInfo::GetRegionInfoAt(addr)` → `regionManager.ReclaimRegion(rawRegion)` | `msRegionManager_.ReclaimRegion(msInfo)` |
| `CentralRegionPool::SweepLargeRegions` | 同上 | 同上 |

**关键简化**：不再需要 MSRegionInfo → 地址 → UnitInfo → RegionInfo 的二层反查。

### 5.6 Step E：MSRegionInfoArray 改造

`MSRegionInfoArray` 从独立 malloc 改为指向 MSRegionManager 的数组：

```cpp
// 改造前
void MSRegionInfoArray::Init(uintptr_t heapStart, size_t maxRegionCount) {
    array_ = static_cast<MSRegionInfo*>(malloc(arrayBytes));  // malloc
    ...
}

// 改造后（简化为 MSRegionManager 的 accessor）
void MSRegionInfoArray::Init(MSRegionInfo* array, uintptr_t heapStart, size_t maxRegionCount) {
    array_ = array;  // 直接指向 mmap 内的 MSRegionInfo[]
    heapStartAddress_ = heapStart;
    maxRegionCount_ = maxRegionCount;
    heapEndAddress_ = heapStart + maxRegionCount * MS_REGION_SIZE;
    // 不再 malloc，不再 free
}
```

或者直接删除 `MSRegionInfoArray`，将 `GetRegionInfo()` 等方法全部移入 `MSRegionManager`。

### 5.7 Step F：条件编译隔离

确保 MS 模式完全不依赖以下 CopyGC 组件：

```cpp
// RegionManager.h
#ifndef MRT_USE_MARK_SWEEP
class RegionManager {
    // 全部 CopyGC 代码
};
#endif

// RegionInfo.h
#ifndef MRT_USE_MARK_SWEEP
class RegionInfo {
    // 全部 CopyGC 代码
};
#endif
```

> **注意**：CartesianTree.h 是通用组件，**不加** `#ifdef`，MS 和 CopyGC 共享。

---

## 6. 风险与回退

### 6.1 风险分析

| 风险 | 影响 | 缓解措施 |
|------|------|---------|
| **CartesianTree 索引语义变化** | Unit 索引到 Region 索引的映射可能出错 | Unit 索引不变，仅在 MSRegionManager 内部做 `unitIdx / unitsPerRegion` 转换 |
| **大对象跨 Region 边界** | TakeRegionForLarge 返回 N 个 Unit，对应多个 MSRegionInfo | 使用 `MS_REGION_SPAN` 类型标记从属条目，`reserved` 字段指回主条目 |
| **inactiveZone 对齐** | CopyGC 的 inactiveZone 以 UNIT_SIZE 对齐，MS 需 MS_REGION_SIZE 对齐 | `inactiveZone_.fetch_add(MS_REGION_SIZE)`，MS_REGION_SIZE 是 UNIT_SIZE 的 2 倍，天然满足 |
| **RegionInfo::UNIT_SIZE 依赖** | ClearUnits/ReleaseUnits 等底层函数仍使用 UNIT_SIZE | 直接使用 memset/madvise 替代，不再依赖 RegionInfo 的静态方法 |
| **编译器内联 fast-path** | 编译器可能依赖 UnitInfo 布局生成地址计算代码 | 确认编译器 fast-path 仅依赖 `allocPtr` 在 offset 0（MSRegionInfo 满足） |
| **Sanitizer 集成** | ASAN/TSAN 可能依赖堆起始地址 | 堆起始地址仍由 `Heap::OnHeapCreated(heapStart)` 传递，不受影响 |

### 6.2 回退策略

如果迁移过程中发现重大问题，可随时回退：

1. **MSRegionManager 与 RegionManager 可共存**：通过 `#ifdef MRT_USE_MARK_SWEEP` 选择
2. **MSRegionInfo[] 存储位置可切换**：加一个 `USE_MMAP_MSREGIONINFO` 宏，在 malloc 和 mmap 之间切换
3. **迁移步骤可独立回退**：每个 Step 都是独立的 commit，可逐步 revert

### 6.3 验证 Checklist

- [ ] 编译通过：`python3 build.py build`
- [ ] Test 1（Binary Tree Churn）PASS
- [ ] Test 2（Generational）PASS
- [ ] Test 3（Large Object Allocation）PASS
- [ ] 大对象分配（Tier1 多 Unit）正确分配和回收
- [ ] GC 日志中 MSRegionInfo 地址在 mmap 范围内
- [ ] MSRegionInfoArray::Cleanup() 不再调用 free()
- [ ] `sizeof(MSRegionInfo) == 64` static_assert 通过
- [ ] Valgrind / ASAN 无内存错误

### 6.4 最终收益总结

| 维度 | 迁移前 | 迁移后 | 改善 |
|------|--------|--------|------|
| 元数据内存（256MB 堆） | 1.9MB (UnitInfo) + 512KB (malloc) = **2.4MB** | **512KB** (mmap 内) | **-80%** |
| 元数据内存（1GB 堆, Linux） | 30MB + 2MB = **32MB** | **2MB** | **-94%** |
| malloc 调用 | 1 次 (512KB) | **0 次** | 消除 |
| 地址查找路径 | addr → UnitInfo → RegionInfo + addr → MSRegionInfo | addr → MSRegionInfo | **一层** |
| 代码量 | RegionManager 619行 + MSRegionInfoArray 80行 | MSRegionManager ~200行 | **-70%** |
| CopyGC 专有死代码 | ~800 行（14 个 RegionList、Ghost、Route 等） | **0 行** | 完全消除 |

---

## 7. 堆增长方案设计

> 目标：将当前固定大小堆改造为动态可增长堆，支持按需分段提交物理页，
> 并在内存压力降低时通过后台线程归还物理页给 OS。
> 本方案基于 MSRegionManager 独立化后的新架构（MSRegionInfo[] 在 mmap 前端）。

### 7.1 当前实现现状与差距分析

#### 7.1.1 当前堆初始化方式（固定堆）

`RegionSpace::Init()` 通过配置参数一次性确定堆大小，整个生命周期内不可扩展：

```cpp
// RegionSpace.cpp:200
size_t heapSize = vmHeapParam.heapSize * 1024;   // 配置参数，启动时固定
size_t totalSize = MSRegionManager::GetHeapMemorySize(heapSize);
map = MemMap::MapMemory(totalSize, totalSize, opt);  // 一次性 mmap 全部
```

- `inactiveZone` 从 `heapStart_` 向 `heapEnd_` 单调推进
- 到达 `heapEnd_` 后即 OOM，**即使系统还有大量可用内存也无法扩展**
- 堆大小完全由启动配置决定，需要用户预估峰值内存

#### 7.1.2 当前 GC 阈值机制

GC 触发阈值已有自适应调整（`TracingCollector::UpdateGCStats`），但仅控制 **GC 触发时机**，不涉及堆增长：

| 组件 | 当前状态 | 说明 |
|------|---------|------|
| **初始阈值** | ✅ 已实现 | `min(gcThreshold, 20MB)` 与 `maxCapacity × 20%` 取较小值 |
| **GC 后阈值调整** | ✅ 已实现 | 4 个竞争公式加权平均（`GcStats.cpp`、`TracingCollector.cpp`） |
| **阈值上限** | ✅ 已实现 | `min(newThreshold, maxCapacity × 0.98)` |
| **OHOS 自适应间隔** | ✅ 已实现 | 根据 collectionRate 动态调整 `GC_REASON_HEU` 最小间隔 |

#### 7.1.3 差距对照表

> 复核备注：本表是早期设计视角，已与当前代码存在明显偏差。当前代码已经具备 `GrowOneSegment()`、`ReleaseGarbageRegions()`、`ShrinkHeap()` 和 `dirtyTree_/releasedTree_`，但仍没有独立的 `HeapGrowthManager` / `HeapGoalController` / `Scavenger` 类。

| 设计组件 | Step1.3 设计 | 当前代码 | 状态 |
|---------|-------------|---------|------|
| **64GB 虚拟预留** | `MAP_NORESERVE` 预留 64GB，按需 `mprotect` | 一次性 `MapMemory(totalSize)` 固定大小 | ❌ 未实现 |
| **Segment 分段提交** | 256MB/段，`GrowOneSegment()` 按需提交 | 不存在分段概念 | ❌ 未实现 |
| **HeapGrowthManager** | 管理段提交/回收，协调元数据与堆空间 | 不存在此类 | ❌ 未实现 |
| **HeapGoalController** | 类 GOGC 的 `gcPercent`，`CalculateNextGCGoal` | 不存在此类（阈值逻辑分散在 TracingCollector） | ❌ 未实现 |
| **Scavenger** | 后台线程 `madvise(MADV_DONTNEED)` 归还空闲物理页 | 不存在后台归还线程 | ❌ 未实现 |
| **dirtyTree/releasedTree** | 空闲 Unit 双树管理 | ✅ MSRegionManager 已有 | ✅ 基础就绪 |
| **madvise 释放** | 空闲 Region 物理页释放 | ✅ CopyGC 路径已有，MS 路径待接入 | ⚠️ 部分就绪 |
| **CartesianTree** | 空闲块管理 | ✅ 已复用 | ✅ 就绪 |

#### 7.1.4 当前行为的问题

1. **内存利用率低**：用户必须预估峰值内存，大部分时间堆远大于实际需求，浪费物理内存
2. **OOM 不可恢复**：`inactiveZone` 到达 `heapEnd` 且 dirtyTree/releasedTree 为空时，分配失败无退路
3. **无物理页归还**：GC 回收的空闲 Region 虽回到 dirtyTree，但物理页始终占用，不归还 OS
4. **阈值与堆容量脱节**：当前阈值上限是 `maxCapacity × 0.98`（固定值），无法随堆增长动态调整

### 7.2 动态堆的内存布局设计

#### 7.2.1 设计目标

将当前的"一次性 mmap 固定堆"改为"大虚拟预留 + 分段物理提交"，适配 MSRegionManager 独立化后的新架构。

#### 7.2.2 新内存布局

初始化时按最大虚拟容量预留，仅首段提交物理页：

```
初始化时（虚拟预留 MAX_VIRTUAL_HEAP，首段物理提交）:

低地址 ────────────────────────────────────────────────────────── 高地址

mmap_base                heapStart                              maxVirtualEnd
   │                        │                                       │
   v                        v                                       v
   ┌────────────────────────┬──────────┬────────────────────────────┐
   │  MSRegionInfo[] 元数据  │ Segment 0│   未提交虚拟空间            │
   │     (按需提交)          │ (已提交)  │   (MAP_NORESERVE)         │
   │                        │  256MB   │                            │
   └────────────────────────┴──────────┴────────────────────────────┘
   │<── metadataReserve ───>│<─ 256MB ─>│<── 未提交 ──────────────>│
   │     (按最大容量预留)     │           │                          │
```

扩展后（提交 Segment 1）：

```
   ┌────────────────────────┬──────────┬──────────┬─────────────────┐
   │  MSRegionInfo[]         │ Segment 0│ Segment 1│  未提交          │
   │  (提交 Meta0 + Meta1)  │ (已提交)  │ (新提交)  │                 │
   └────────────────────────┴──────────┴──────────┴─────────────────┘
```

**关键特性**：
- 元数据（MSRegionInfo[]）和堆空间（Region）在同一个 mmap 区域内
- 元数据随堆扩展同步提交——第 K 段堆空间对应的 MSRegionInfo 条目必须同时提交
- `heapEnd_` 动态更新为已提交堆空间的上界
- `maxVirtualEnd` 为虚拟预留的硬上限，不可超越

#### 7.2.3 参数设计

```cpp
// MSRegionManager 新增常量
static constexpr size_t MAX_VIRTUAL_HEAP = 16ULL * 1024 * 1024 * 1024;   // 16GB 虚拟预留
static constexpr size_t SEGMENT_SIZE = 256ULL * 1024 * 1024;              // 256MB / 段
static constexpr size_t REGIONS_PER_SEGMENT = SEGMENT_SIZE / MS_REGION_SIZE;  // 8192 regions/段
static constexpr size_t META_PER_SEGMENT = REGIONS_PER_SEGMENT * sizeof(MSRegionInfo);  // 512KB/段
```

**为什么选 16GB 而非 64GB**：
- MSRegionInfo[] 仅 64B/Region（vs UnitInfo 120B/Unit），元数据开销极小
- 16GB 虚拟预留 = 64 个 Segment，对应 16GB / 32KB = 524288 个 MSRegionInfo = 32MB 元数据预留
- 对 macOS ARM64 和 Linux x86_64 的虚拟地址空间而言，16GB 预留非常小
- 实际物理提交由 Segment 按需控制，启动时仅首段 256MB + 512KB 元数据

#### 7.2.4 元数据与堆空间的提交对应关系

```
Segment K 的堆空间: [heapStart + K*SEGMENT_SIZE, heapStart + (K+1)*SEGMENT_SIZE)
  → 包含 REGIONS_PER_SEGMENT 个 Region
  → 对应 MSRegionInfo[K*REGIONS_PER_SEGMENT .. (K+1)*REGIONS_PER_SEGMENT - 1]
  → 元数据所在范围: [array_ + K*META_PER_SEGMENT, array_ + (K+1)*META_PER_SEGMENT)
```

MSRegionInfo[] 是正向数组（§3.2），第 K 段堆空间的元数据位于数组中固定偏移处，
可直接 `mprotect` 对应的元数据页。

#### 7.2.5 与 Go 的对比

| 维度 | Go | Cangjie MS（改造后） |
|------|-----|---------------------|
| 虚拟预留 | 512GB+（arena hint，分散） | 16GB（连续 mmap） |
| 增长单位 | arena (64MB) | Segment (256MB) |
| 元数据存储 | heapArena 独立分配，arenaIndex 二级映射 | MSRegionInfo[] 连续正向数组，O(1) 偏移 |
| 元数据/堆方向 | 独立（不相邻） | 同一 mmap 内连续（元数据在前，堆在后） |
| 收缩 | bgscavenge 线程 | Scavenger 线程（§7.5） |

### 7.3 HeapGrowthManager 设计

管理 Segment 级别的物理页提交与回收，是堆增长的核心协调器。

#### 7.3.1 职责

- 初始化时按 `MAX_VIRTUAL_HEAP` 预留虚拟地址，仅提交首段
- 当 `inactiveZone` 接近已提交上界时，提交下一个 Segment（堆空间 + 对应元数据）
- 维护已提交的 Segment 数量，供 Scavenger 决策物理页归还范围
- 提供堆容量查询接口（已提交容量 vs 最大虚拟容量）

#### 7.3.2 类结构

```cpp
class HeapGrowthManager {
public:
    // 初始化：大虚拟预留 + 首段提交
    // initialHeapSize: 初始堆大小（向上对齐到 SEGMENT_SIZE）
    void Initialize(uintptr_t mmapBase, size_t maxVirtualHeapSize, size_t initialHeapSize);

    // 尝试提交下一个 Segment。成功返回 true，到达虚拟上限返回 false。
    // 调用方：MSRegionManager::TakeUnitsAligned() 在 inactiveZone 扩展失败时调用。
    bool GrowOneSegment();

    // 归还指定 Segment 的物理页（madvise DONTNEED）。
    // 调用方：Scavenger 后台线程。
    void ShrinkSegment(size_t segmentIdx);

    // 查询
    size_t GetCommittedSegmentCount() const { return committedSegments_.load(); }
    size_t GetCommittedHeapSize() const { return GetCommittedSegmentCount() * SEGMENT_SIZE; }
    size_t GetMaxVirtualHeapSize() const { return maxVirtualHeapSize_; }
    uintptr_t GetCommittedHeapEnd() const { return heapStart_ + GetCommittedHeapSize(); }

private:
    uintptr_t mmapBase_ = 0;          // mmap 起始（元数据区起点）
    uintptr_t heapStart_ = 0;         // 堆空间起始（元数据区之后）
    size_t maxVirtualHeapSize_ = 0;   // 虚拟预留的最大堆空间
    size_t maxSegmentCount_ = 0;      // maxVirtualHeapSize / SEGMENT_SIZE

    std::atomic<size_t> committedSegments_{0};  // 已提交的 Segment 数
    std::mutex growMutex_;                       // GrowOneSegment 串行化
};
```

#### 7.3.3 GrowOneSegment 实现

```cpp
bool HeapGrowthManager::GrowOneSegment()
{
    std::lock_guard<std::mutex> lk(growMutex_);
    size_t current = committedSegments_.load(std::memory_order_acquire);
    if (current >= maxSegmentCount_) {
        return false;  // 已达虚拟上限
    }

    // 1. 提交堆空间物理页
    uintptr_t segHeapStart = heapStart_ + current * SEGMENT_SIZE;
    int ret = mprotect(reinterpret_cast<void*>(segHeapStart), SEGMENT_SIZE,
                       PROT_READ | PROT_WRITE);
    if (ret != 0) return false;

    // 2. 提交对应的元数据物理页
    uintptr_t segMetaStart = mmapBase_ + current * META_PER_SEGMENT;
    ret = mprotect(reinterpret_cast<void*>(segMetaStart), META_PER_SEGMENT,
                   PROT_READ | PROT_WRITE);
    if (ret != 0) return false;

    // 3. 零初始化新提交的 MSRegionInfo 条目
    size_t baseRegionIdx = current * REGIONS_PER_SEGMENT;
    MSRegionInfo* array = reinterpret_cast<MSRegionInfo*>(mmapBase_);
    memset(&array[baseRegionIdx], 0, REGIONS_PER_SEGMENT * sizeof(MSRegionInfo));
    for (size_t i = 0; i < REGIONS_PER_SEGMENT; i++) {
        array[baseRegionIdx + i].regionType = MSRegionType::MS_REGION_FREE;
    }

    // 4. 更新已提交计数
    committedSegments_.store(current + 1, std::memory_order_release);

    LOG(RTLOG_INFO, "[MS-GC] HeapGrowthManager: committed segment %zu, "
        "heap range now [0x%zx, 0x%zx), total %zuMB",
        current, heapStart_, GetCommittedHeapEnd(), GetCommittedHeapSize() / MB);
    return true;
}
```

#### 7.3.4 与 MSRegionManager 的集成

`MSRegionManager::TakeUnitsAligned()` 的 bump fallback（inactiveZone 扩展）需改造：

```cpp
// 现有：inactiveZone 到达 heapEnd_ 即 OOM
// 改造后：inactiveZone 到达已提交上界时，先尝试 GrowOneSegment

// Level 3: 扩展 inactiveZone
uintptr_t old = inactiveZone_.load(std::memory_order_acquire);
uintptr_t next = old + requiredBytes;

// 检查是否超出已提交范围
if (next > growthManager_->GetCommittedHeapEnd()) {
    // 尝试提交新 Segment
    if (!growthManager_->GrowOneSegment()) {
        return false;  // 真正的 OOM：虚拟上限已达
    }
    // 更新 heapEnd_
    heapEnd_ = growthManager_->GetCommittedHeapEnd();
}

if (next <= heapEnd_ &&
    inactiveZone_.compare_exchange_strong(old, next, std::memory_order_acq_rel)) {
    idx = (old - heapStart_) / unitSize_;
    return true;
}
```

同时需要更新 `Heap::OnHeapExtended(newEnd)` 以通知 barrier 和 `IsHeapAddress` 范围变化。

### 7.4 HeapGoalController 设计

将当前分散在 `TracingCollector::UpdateGCStats()` 中的阈值逻辑收敛为独立组件，
并增加与堆增长的联动。

#### 7.4.1 当前阈值机制的问题

当前 `UpdateGCStats()` 中的阈值计算存在两个问题：

1. **阈值上限与堆容量耦合**：`newThreshold = min(newThreshold, maxCapacity * 0.98)`，
   在固定堆下等于固定数值，无法随堆增长动态提升
2. **无堆增长触发语义**：阈值仅用于触发 GC，没有"GC 后堆仍不够 → 应该增长堆"的判断

#### 7.4.2 HeapGoalController 职责

- 根据 GC 后存活量计算下一次 GC 触发点（`nextGCGoal`）
- 判断是否需要触发堆增长（GC 后存活量接近已提交容量）
- 提供 Scavenger 的目标保留水位线

#### 7.4.3 类结构

```cpp
class HeapGoalController {
public:
    void Init(size_t initialCapacity);

    // GC 完成后调用：根据存活量更新下一次 GC 目标
    void UpdateAfterGC(size_t liveBytesAfterGC, size_t committedHeapSize);

    // 分配时检查：是否应触发 GC
    bool ShouldTriggerGC(size_t allocatedBytes) const {
        return allocatedBytes >= nextGCGoal_;
    }

    // GC 完成后检查：是否需要扩展堆
    bool ShouldGrowHeap(size_t liveBytesAfterGC, size_t committedHeapSize) const {
        // 存活量超过已提交容量的 80% → 需要扩展
        return liveBytesAfterGC > committedHeapSize * 4 / 5;
    }

    // Scavenger 使用：目标保留水位线
    size_t GetRetainedGoal() const {
        return std::max(nextGCGoal_, heapMinimum_);
    }

    size_t GetNextGCGoal() const { return nextGCGoal_; }

    // 配置参数（类似 GOGC）
    void SetGCPercent(size_t percent) { gcPercent_ = percent; }

private:
    size_t gcPercent_ = 100;       // 类似 GOGC: 存活量增长 100% 时触发 GC
    size_t nextGCGoal_ = 0;        // 下一次 GC 触发点
    size_t heapMinimum_ = 20 * MB; // 堆最低保留大小
};
```

#### 7.4.4 UpdateAfterGC 实现

```cpp
void HeapGoalController::UpdateAfterGC(size_t liveBytesAfterGC, size_t committedHeapSize)
{
    // 核心公式：nextGCGoal = liveBytes × (1 + gcPercent / 100)
    // gcPercent=100 → 堆用量翻倍时触发下一次 GC
    size_t goal = liveBytesAfterGC + liveBytesAfterGC * gcPercent_ / 100;

    // 不超过已提交容量的 95%（留余量给分配突发）
    goal = std::min(goal, committedHeapSize * 95 / 100);

    // 不低于最低保留
    goal = std::max(goal, heapMinimum_);

    nextGCGoal_ = goal;
}
```

#### 7.4.5 与现有 UpdateGCStats 的关系

短期（Phase 1）：HeapGoalController 与现有 `UpdateGCStats` **并行运行**。
- 现有 4 公式加权逻辑继续控制 `GC_REASON_HEU` 的触发阈值
- HeapGoalController 额外提供堆增长判断和 Scavenger 水位线
- 两者不冲突——前者控制 GC 频率，后者控制堆容量

长期（Phase 2）：可将 `UpdateGCStats` 的阈值逻辑统一收敛到 HeapGoalController 中。

### 7.5 Scavenger 后台归还设计

参照 Go `bgscavenge`，在内存压力降低后，将空闲 Region 的物理页通过 `madvise(MADV_DONTNEED)` 归还 OS。

#### 7.5.1 核心逻辑

```
Scavenger 循环（后台线程，低优先级）:
  1. retainedGoal = HeapGoalController::GetRetainedGoal()
  2. currentMapped = committedHeapSize - releasedBytes
  3. releaseTarget = currentMapped - retainedGoal
  4. 如果 releaseTarget > 0:
     a. 从 MSRegionManager::dirtyTree_ 中找空闲 Unit 块
     b. 对这些 Unit 执行 madvise(MADV_DONTNEED)
     c. 将它们从 dirtyTree_ 转移到 releasedTree_
  5. sleep(scavengeInterval)   // 限流，避免 CPU 开销
```

#### 7.5.2 类结构

```cpp
class Scavenger {
public:
    void Start();    // 启动后台线程
    void Stop();     // 停止（runtime 销毁时）
    void Wake();     // GC 完成后唤醒（可能有大量空闲 Region）

private:
    void ScavengeLoop();   // 主循环
    size_t ScavengeOnce(); // 单次扫描，返回释放字节数

    std::thread thread_;
    std::mutex mutex_;
    std::condition_variable cv_;
    bool running_ = false;

    // 限流参数
    static constexpr uint64_t SCAVENGE_INTERVAL_MS = 100;  // 100ms 扫描间隔
    static constexpr size_t MIN_RELEASE_BYTES = 1 * MB;    // 单次最少释放 1MB
};
```

#### 7.5.3 ScavengeOnce 实现

```cpp
size_t Scavenger::ScavengeOnce()
{
    MSRegionManager& mgr = *msRegionManager_;
    HeapGoalController& goal = *heapGoalController_;

    size_t retainedGoal = goal.GetRetainedGoal();
    size_t currentUsed = mgr.GetActiveBytes();  // inactiveZone - heapStart - freeBytes
    if (currentUsed <= retainedGoal) {
        return 0;  // 无需释放
    }

    size_t releaseTarget = currentUsed - retainedGoal;
    if (releaseTarget < MIN_RELEASE_BYTES) {
        return 0;
    }

    size_t released = 0;
    // 从 dirtyTree 取空闲块，执行 madvise 后转入 releasedTree
    std::lock_guard<std::mutex> lk(mgr.dirtyTreeMutex_);
    // 遍历 dirtyTree 中连续空闲块，逐块释放
    // ... 每释放 SEGMENT_SIZE 检查一次是否达到目标 ...

    return released;
}
```

#### 7.5.4 触发时机

| 时机 | 触发方式 | 说明 |
|------|---------|------|
| GC 完成后 | `Scavenger::Wake()` | GC 回收大量 Region 后立即检查 |
| 定时唤醒 | `condition_variable::wait_for` | 兜底：即使无 GC 也定期检查 |
| 内存压力事件 | 系统信号（可选） | macOS `DISPATCH_SOURCE_TYPE_MEMORYPRESSURE`，Linux cgroups OOM 通知 |

#### 7.5.5 与现有 dirtyTree/releasedTree 的关系

MSRegionManager 已有双树结构（§4.1）。Scavenger 的操作流程：

```
空闲 Region → ReclaimRegion → dirtyTree
                                  ↓ Scavenger
                          madvise(MADV_DONTNEED)
                                  ↓
                             releasedTree
                                  ↓ TakeRegion
                          重新 commit（OS 自动 fault-in）
```

双树之间转移的基础机制已就绪，Scavenger 仅需实现遍历 + madvise + 转移的编排逻辑。

### 7.6 GC 阈值机制适配

堆增长引入后，GC 阈值需要与堆容量联动，避免两个极端：
- **阈值太低**：堆还有大量空间却频繁 GC，浪费 CPU
- **阈值太高**：堆用尽才 GC，导致长时间 STW 或 OOM

#### 7.6.1 改造点

| 改造点 | 当前代码 | 改造后 |
|--------|---------|--------|
| **阈值上限** | `min(newThreshold, maxCapacity × 0.98)` — 固定值 | `min(newThreshold, committedHeapSize × 0.95)` — 随堆增长动态提升 |
| **GC 后堆增长判断** | 无 | GC 完成 → `HeapGoalController::ShouldGrowHeap()` → `HeapGrowthManager::GrowOneSegment()` |
| **OOM 前兜底** | allocation retry 5 次后 OOM | retry 中插入 `GrowOneSegment()` 尝试 |

#### 7.6.2 分配失败的升级路径（改造后）

```
分配失败 (TakeRegion returns nullptr)
  │
  ├─ tryTimes < 3: yield + 触发 GC_REASON_HEU（异步）
  │
  ├─ tryTimes == 3: 触发 GC_REASON_HEU（同步等待 GC 完成）
  │
  ├─ tryTimes == 4: 尝试 GrowOneSegment()       ← 新增
  │   ├─ 成功 → 更新 heapEnd_，重试分配
  │   └─ 失败 → 继续
  │
  ├─ tryTimes == 5: 触发 GC_REASON_OOM（强制同步 GC）
  │
  └─ tryTimes > 5: 再次尝试 GrowOneSegment()   ← 新增
      ├─ 成功 → 重试
      └─ 失败 → 真正 OOM
```

#### 7.6.3 GC 完成后的堆扩展检查

在 `TracingCollector::UpdateGCStats()` 末尾新增：

```cpp
// GC 完成后：检查是否需要扩展堆
if (heapGoalController_->ShouldGrowHeap(liveBytes, committedHeapSize)) {
    if (growthManager_->GrowOneSegment()) {
        // 更新 MSRegionManager 的 heapEnd_
        msRegionManager_->SetHeapEnd(growthManager_->GetCommittedHeapEnd());
        Heap::OnHeapExtended(growthManager_->GetCommittedHeapEnd());
    }
}

// 更新 HeapGoalController
heapGoalController_->UpdateAfterGC(liveBytes, growthManager_->GetCommittedHeapSize());

// 唤醒 Scavenger 检查是否有空闲页可归还
scavenger_->Wake();
```

### 7.7 实施步骤

#### 7.7.1 分阶段计划

| 阶段 | 内容 | 改动文件 | 依赖 |
|------|------|---------|------|
| **P0: 大虚拟预留** | `RegionSpace::Init()` 改为 `mmap(MAP_NORESERVE)` 大虚拟区 + 首段 `mprotect` 提交 | `RegionSpace.cpp`, `MSRegionManager.h/.cpp`, `MemMap.h` | §5 MSRegionManager 迁移完成 |
| **P1: HeapGrowthManager** | 新增类，实现 `GrowOneSegment()` + `ShrinkSegment()` | 新文件 `HeapGrowthManager.h/.cpp` | P0 |
| **P2: inactiveZone 扩展改造** | `TakeUnitsAligned` bump fallback 接入 `GrowOneSegment` | `MSRegionManager.cpp` | P1 |
| **P3: HeapGoalController** | 新增类，GC 后堆增长判断 + 阈值联动 | 新文件 `HeapGoalController.h/.cpp`, `TracingCollector.cpp` | P1 |
| **P4: Scavenger** | 新增后台线程，dirtyTree → madvise → releasedTree | 新文件 `Scavenger.h/.cpp` | P1, P3 |
| **P5: 分配重试适配** | OOM 重试路径中插入 `GrowOneSegment` | `RegionSpace.cpp` | P2 |

#### 7.7.2 P0 详细改动：大虚拟预留

```cpp
// RegionSpace::Init() 改造

#ifdef MRT_USE_MARK_SWEEP
    // 计算虚拟预留大小
    size_t initialHeapSize = RoundUp(vmHeapParam.heapSize * 1024, SEGMENT_SIZE);
    size_t maxVirtualHeapSize = std::max(initialHeapSize,
        static_cast<size_t>(HeapGrowthManager::MAX_VIRTUAL_HEAP));
    size_t maxMetadataSize = MSRegionManager::GetMetadataSize(maxVirtualHeapSize);
    size_t totalVirtualSize = maxMetadataSize + maxVirtualHeapSize;

    // 大虚拟预留（MAP_NORESERVE，不提交物理页）
    MemMap::Option opt = MemMap::DEFAULT_OPTIONS;
    opt.flags |= MAP_NORESERVE;
    map = MemMap::MapMemory(totalVirtualSize, totalVirtualSize, opt);
    MAddress mmapBase = map->GetBaseAddr();

    // 首段提交
    growthManager_ = new HeapGrowthManager();
    growthManager_->Initialize(mmapBase, maxVirtualHeapSize, initialHeapSize);

    // MSRegionManager 初始化（heapEnd 为首段提交上界）
    msRegionManager_->Initialize(initialHeapSize, mmapBase);
#endif
```

#### 7.7.3 P1-P2 改动量估算

| 新增文件 | 行数 | 说明 |
|---------|------|------|
| `HeapGrowthManager.h` | ~40 行 | 类声明 |
| `HeapGrowthManager.cpp` | ~80 行 | Initialize + GrowOneSegment + ShrinkSegment |
| `HeapGoalController.h` | ~30 行 | 类声明 |
| `HeapGoalController.cpp` | ~50 行 | Init + UpdateAfterGC |
| `Scavenger.h` | ~30 行 | 类声明 |
| `Scavenger.cpp` | ~80 行 | Start/Stop/ScavengeLoop/ScavengeOnce |
| **合计新增** | **~310 行** | |

| 修改文件 | 改动行数 | 说明 |
|---------|---------|------|
| `RegionSpace.cpp` | ~30 行 | Init() 改为大虚拟预留 |
| `MSRegionManager.cpp` | ~15 行 | TakeUnitsAligned bump fallback 接入 GrowOneSegment |
| `TracingCollector.cpp` | ~10 行 | UpdateGCStats 末尾新增堆扩展检查 |
| `RegionSpace.cpp` | ~10 行 | OOM 重试路径 |
| **合计修改** | **~65 行** | |

### 7.8 验证 Checklist

#### 7.8.1 基础功能验证

- [ ] 编译通过：`python3 build.py build`
- [ ] 初始堆大小 = 配置值（首段 256MB 提交）
- [ ] `inactiveZone` 推进到首段末尾后自动触发 `GrowOneSegment()`
- [ ] 堆扩展后新 Region 可正常分配和使用
- [ ] MSRegionInfo[] 对应新 Segment 的条目正确初始化
- [ ] GC 触发阈值随堆增长动态调整

#### 7.8.2 Scavenger 验证

- [ ] GC 完成后 Scavenger 被唤醒
- [ ] 空闲 Region 物理页通过 `madvise(MADV_DONTNEED)` 归还
- [ ] dirtyTree → releasedTree 转移正确
- [ ] releasedTree 中的 Unit 被再次分配时 OS 自动 fault-in

#### 7.8.3 边界条件

- [ ] 连续快速分配触发多次 `GrowOneSegment()`（不死锁、不重复提交）
- [ ] 虚拟上限到达后 `GrowOneSegment()` 返回 false
- [ ] 虚拟上限到达 + GC 无法回收 → 正常 OOM
- [ ] `IsHeapAddress()` 在堆扩展后正确识别新范围地址
- [ ] 多线程并发触发堆扩展（`growMutex_` 串行化正确）

#### 7.8.4 性能指标

| 指标 | 合格标准 |
|------|----------|
| `GrowOneSegment` 延迟 | ≤ 1ms（`mprotect` 系统调用） |
| Scavenger 单次扫描 CPU | ≤ 0.5ms |
| 堆扩展对分配吞吐的影响 | ≤ 5% 下降（仅首次触及新 Segment 时） |
| Scavenger 归还后 RSS 下降 | `madvise` 后 RSS 应在 5s 内反映物理页释放 |

#### 7.8.5 测试用例

```cj
// 测试 1：堆增长——分配超过初始堆大小
main() {
    let list = ArrayList<Array<Byte>>()
    // 初始堆 256MB，连续分配到 512MB+
    for (i in 0..20000) {
        list.append(Array<Byte>(32768, item: 0))  // 32KB × 20000 = 640MB
    }
    println("Heap growth test passed: allocated ${list.size} chunks")
}

// 测试 2：堆增长 + GC 交替
main() {
    for (round in 0..10) {
        let temp = ArrayList<Array<Byte>>()
        for (i in 0..5000) {
            temp.append(Array<Byte>(32768, item: 0))
        }
        // temp 离开作用域 → GC 回收 → 堆应收缩（Scavenger 归还）
    }
    println("Heap growth + GC test passed")
}
```

---

## 附录 A：相关文件清单

| 文件 | 角色 | 迁移影响 |
|------|------|---------|
| `RegionSpace.cpp` | 堆初始化 + 分配入口 | 重写 Init() MS 分支 |
| `RegionManager.h/.cpp` | CopyGC Region 管理 | MS 模式不再使用 |
| `RegionInfo.h` | CopyGC UnitInfo/UnitMetadata | MS 模式不再使用 |
| `FreeRegionManager.h` | CartesianTree 封装 | 可选复用或直接使用 CartesianTree |
| `CartesianTree.h` | 通用空闲块管理 | **直接复用，不修改** |
| `MSRegionInfo.h/.cpp` | MS 元数据 + 数组管理 | 改为 mmap 存储，删除 malloc |
| `CentralRegionPool.cpp` | Sweep 回收路径 | 改用 MSRegionManager 接口 |
| `AllocBufferMS.h/.cpp` | 线程本地分配 | 改用 MSRegionInfo 返回 |
| `MSRegionManager.h/.cpp` | **新文件** | 独立 MS Region 管理器 |
