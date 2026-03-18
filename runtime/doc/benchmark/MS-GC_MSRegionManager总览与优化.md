# MSRegionManager 总览与优化分析

> 基于源码 `src/Heap/Allocator/MSRegionManager.{h,cpp}` 及其调用链分析。
> 日期：2026-03-17

---

## 1. 内存布局与初始化

### 1.1 mmap 整体布局

MSRegionManager 管理一块连续 mmap 区域，前部为元数据数组，后部为可用堆空间：

```
[MSRegionInfo[] array | HeapRegion space                          ]
^mmapBase             ^heapStart                                  ^heapEnd
                      |<--- MS_REGION_SIZE (32KB) 对齐 ---------->|
```

- **MS_REGION_SIZE** = 32KB（`SizeClass.h` 定义）
- **MSRegionInfo** = 64 字节（恰好一个 cache line），与物理 Region 1:1 对应
- **MetadataSize** = `ceil(regionCount * 64, PAGE_SIZE)`
- macOS（16KB page）：`unitsPerMSRegion = 32KB / 16KB = 2 units`
- Linux（4KB page）：`unitsPerMSRegion = 32KB / 4KB = 8 units`

### 1.2 Initialize 流程

```cpp
void MSRegionManager::Initialize(size_t heapSize, uintptr_t mmapBase)
{
    unitSize_ = MRT_PAGE_SIZE;                    // 系统页大小
    unitsPerMSRegion_ = MS_REGION_SIZE / unitSize_;
    metadataSize = GetMetadataSize(heapSize);     // 对齐到页
    maxRegionCount_ = heapSize / MS_REGION_SIZE;
    maxUnitCount_ = heapAligned / unitSize_;

    array_ = reinterpret_cast<MSRegionInfo*>(mmapBase);  // 正向数组（非镜像）
    heapStart_ = mmapBase + metadataSize;
    heapEnd_ = heapStart_ + heapAligned;
    inactiveZone_ = heapStart_;                   // 初始水位线 = 堆起始

    // 初始化所有 MSRegionInfo → MS_REGION_FREE
    memset(array_, 0, maxRegionCount_ * sizeof(MSRegionInfo));

    // 初始化双 CartesianTree
    dirtyTree_.Init(maxUnitCount_);     // dirty: 回收过的脏页
    releasedTree_.Init(maxUnitCount_);  // released: madvise DONTNEED 过的页
}
```

### 1.3 关键数据结构

| 成员 | 类型 | 用途 |
|------|------|------|
| `array_` | `MSRegionInfo*` | 元数据数组，位于 mmap 头部 |
| `heapStart_/heapEnd_` | `uintptr_t` | 堆的有效地址范围 |
| `inactiveZone_` | `atomic<uintptr_t>` | 未使用堆空间的水位线（bump allocator） |
| `dirtyTree_` | CartesianTree | 回收后的脏页空闲池（含数据残留） |
| `releasedTree_` | CartesianTree | 已释放物理页的空闲池（OS 可回收） |
| `unitSize_` | `size_t` | = 系统页大小（macOS 16KB, Linux 4KB） |
| `unitsPerMSRegion_` | `size_t` | 每个 MS Region 包含的 unit 数 |

### 1.4 地址 → MSRegionInfo 查找 (O(1))

```cpp
MSRegionInfo* GetRegionInfoAt(uintptr_t addr) const {
    size_t regionIdx = (addr - heapStart_) / MS_REGION_SIZE;
    return &array_[regionIdx];
}
```

- 纯算术运算，无查表，O(1) 复杂度

## 2. TakeRegion — 小对象 Region 分配路径

### 2.1 调用链

```
AllocBufferMS::SlowAlloc(cls)
  → RefillFromCentral(cls)
    → CentralRegionPool::GetFreshRegion(cls)
      → MSRegionManager::TakeRegion()          ← 分配 32KB region
        → TakeUnitsAligned(unitsPerMSRegion_, unitsPerMSRegion_, idx)
          → TakeUnitsInternal() 或 inactiveZone bump
```

### 2.2 TakeRegion 实现逻辑

```cpp
MSRegionInfo* TakeRegion() {
    // 1. 启发式 GC 触发（非 GC 期间检查阈值）
    if (!IsGcStarted() && allocated >= threshold) {
        RequestGC(GC_REASON_HEU, true);
    }

    // 2. 分配对齐的 unit（必须对齐到 unitsPerMSRegion_）
    UnitIndex idx;
    if (!TakeUnitsAligned(unitsPerMSRegion_, unitsPerMSRegion_, idx))
        return nullptr;  // OOM

    // 3. 计算 regionIdx，返回 MSRegionInfo*
    size_t regionIdx = idx / unitsPerMSRegion_;
    MSRegionInfo* info = &array_[regionIdx];
    ASSERT(info->regionType == MS_REGION_FREE);
    return info;
}
```

### 2.3 TakeUnitsAligned — 对齐分配的 3 级策略

TakeRegion 使用 `TakeUnitsAligned`（而非 `TakeUnitsInternal`），因为 regionIdx = idx / unitsPerMSRegion_ 必须精确 roundtrip。

三级分配策略，优先复用已回收内存，最后才扩展水位线：

**Level 1: dirtyTree — 复用脏页（优先）**

```
dirtyTreeMutex_.try_lock()
  → dirtyTree_.TakeUnitsLowestAddr(totalNeeded, rawIdx)
  → 对齐 rawIdx → 归还前导/尾部碎片
  → ClearUnits（memset 置零 32KB）
```

- `try_lock` 非阻塞：获取失败则跳过，不自旋
- 从 dirty tree 取出后需 `ClearUnits`（memset 置零），因为页面包含上一轮残留数据
- 过度分配 `totalNeeded = num + alignment - 1`，对齐后通过 `ReclaimUnitsInternal` 归还碎片
- **优点**：复用已分配过的内存，局部性好
- **缺点**：需要 memset 清零 + 锁竞争

**Level 2: releasedTree — 复用已释放物理页**

```
releasedTreeMutex_.try_lock()
  → releasedTree_.TakeUnitsLowestAddr(totalNeeded, rawIdx)
  → 对齐 rawIdx → 归还碎片
  → 无需 ClearUnits（OS 已回收物理页，映射时自动零填充）
```

- 同样 `try_lock` 非阻塞
- 无需 memset — OS 通过 `madvise(DONTNEED)` 回收的页在再次访问时自动零填充
- **写入路径**：`ReclaimUnitsReleased()`（大对象回收）和 `ReleaseGarbageRegions()`（dirty→released 迁移）会向 releasedTree 插入节点。在大对象频繁分配/回收的场景下此路径会生效

**Level 3: inactiveZone bump — 扩展水位线（兜底）**

```
inactiveZone CAS → 对齐到 alignment*unitSize_ → 如果 ≤ heapEnd → 成功
```

- 无锁，仅 CAS 操作
- inactiveZone 是单调递增水位线，天然页对齐
- **优点**：最简路径，无锁无碎片
- **缺点**：不可逆 — 水位线只升不降，消耗未使用的堆空间
- **对齐间隙浪费**：CAS bump 时若当前 inactiveZone 未对齐到 alignment×unitSize_，会先跳到对齐地址，中间的间隙既不归还 dirtyTree 也不归还 releasedTree，造成永久浪费

**与 TakeUnitsInternal 的对比**

两者策略顺序一致（dirtyTree → releasedTree → inactiveZone），区别在于对齐处理：

| | TakeUnitsAligned | TakeUnitsInternal |
|--|---|---|
| 优先级1 | **dirtyTree** try_lock | **dirtyTree** try_lock |
| 优先级2 | releasedTree try_lock | releasedTree try_lock |
| 优先级3 | inactiveZone bump | inactiveZone bump |
| 对齐 | 必须对齐到 unitsPerMSRegion_ | 无要求 |
| 碎片处理 | 过度分配 + 切割归还 | 无 |

> **注意**：代码中 `TakeUnitsInternal` 实际上从未被外部直接调用，所有路径（TakeRegion、TakeRegionForLarge）均使用 `TakeUnitsAligned`。TakeUnitsInternal 仅作为内部策略实现被 TakeUnitsAligned 的对齐逻辑包装。

> **设计决策**：优先从 dirtyTree/releasedTree 复用已回收内存，保持堆空间紧凑；inactiveZone 作为最终兜底，避免扩展水位线过快导致 OOM。

### 2.4 GC 启发式触发

```cpp
if (!Heap::GetHeap().IsGcStarted()) {
    size_t threshold = collector.GetGCStats().GetThreshold();
    size_t allocated = Heap::GetHeap().GetAllocator().AllocatedBytes();
    if (allocated >= threshold) {
        collector.RequestGC(GC_REASON_HEU, true);  // 同步等待 GC
    }
}
```

- 仅在非 GC 期间检查
- `RequestGC(GC_REASON_HEU, true)` — 同步，mutator 阻塞等待 GC 完成
- **问题**：这是全局阈值，不区分 SizeClass，可能导致不必要的 GC

## 3. ReclaimRegion — 小对象 Region 回收路径

### 3.1 调用链

```
GC MarkSetup STW
  → CentralRegionPool::ForceFinishSweep()
    → region->SweepRegion()              // allocBitmap = ~markBitmap
    → if IsCompletelyFree():
        → region->ReleaseBitmaps()       // 释放 bitmap 堆内存
        → region->Reset()                // regionType → FREE, 清零字段
        → CentralRegionPool::ReleaseRegionInternal()
          → totalMSRegionBytes_ -= MS_REGION_SIZE
          → MSRegionManager::ReclaimRegion(region)     ← 归还物理空间
            → ReclaimUnitsInternal(unitIdx, unitCount)
              → dirtyTree_.MergeInsert(idx, num)       ← 合并到脏页池
```

### 3.2 ReclaimRegion 实现

```cpp
void ReclaimRegion(MSRegionInfo* region) {
    // 1. 指针算术：MSRegionInfo* → regionIdx → unitIdx
    size_t regionIdx = region - array_;
    UnitIndex unitIdx = regionIdx * unitsPerMSRegion_;
    UnitCount unitCount = unitsPerMSRegion_;

    // 2. 断言：region 必须已被 Reset() 为 FREE
    ASSERT(region->regionType == MS_REGION_FREE);

    // 3. 归还到 dirtyTree
    ReclaimUnitsInternal(unitIdx, unitCount);
}
```

### 3.3 ReclaimUnitsInternal — 加锁归还

```cpp
void ReclaimUnitsInternal(UnitIndex idx, UnitCount num) {
    ScopedEnterSaferegion enterSaferegion(true);   // 进入安全区（允许 GC STW）
    std::lock_guard<std::mutex> lg(dirtyTreeMutex_);
    dirtyTree_.MergeInsert(idx, num, false);       // 插入并与相邻节点合并
}
```

**关键特征**：

| 特征 | 说明 |
|------|------|
| 锁类型 | `lock_guard`（阻塞等待），非 `try_lock` |
| 安全区 | `ScopedEnterSaferegion(true)` — 持锁期间允许 GC 暂停此线程 |
| 合并策略 | `MergeInsert` 自动合并相邻空闲区间，减少碎片 |
| 目标树 | 总是归还到 `dirtyTree_`（非 releasedTree） |

### 3.4 Region 生命周期状态转移

```
FREE ──TakeRegion──→ BUMP ──填满──→ FULL ──sweep──→ PARTIAL ──再分配──→ FULL
                                                     ↓ (全空)
                                                   Reset → FREE ──ReclaimRegion──→ dirtyTree
```

- **只有完全空闲的 region 才会归还**给 MSRegionManager
- 部分占用的 region 留在 CentralRegionPool 的 partial/full 链表中

## 4. 大对象分配与回收

### 4.1 TakeRegionForLarge

大对象定义：size > MS_REGION_SIZE (32KB)。

```cpp
MSRegionInfo* TakeRegionForLarge(size_t unitCount, uintptr_t& regionStart) {
    // 1. 启发式 GC 触发（同 TakeRegion）
    // 2. 对齐上取整：unitCount → alignedUnitCount（对齐到 unitsPerMSRegion_）
    UnitCount alignedUnitCount = ceil(unitCount, unitsPerMSRegion_) * unitsPerMSRegion_;
    // 3. 分配：TakeUnitsAligned(alignedUnitCount, unitsPerMSRegion_, idx)
    // 4. regionStart = heapStart_ + idx * unitSize_
    // 5. 返回 array_[idx / unitsPerMSRegion_] （base MSRegionInfo）
}
```

**对齐要求**：
- **起始对齐**：idx 必须对齐到 unitsPerMSRegion_，否则 regionIdx 映射错误
- **大小对齐**：unitCount 上取整到 unitsPerMSRegion_ 的倍数，否则尾部 partial region 的 MSRegionInfo 不被 spanCount 覆盖 → 元数据损坏

### 4.2 大对象 MSRegionInfo 的 span 结构

大对象跨越多个 32KB region，使用 base + span 结构：

```
array_[baseIdx]:    regionType = MS_REGION_LARGE, allocPtr=start, regionEnd=end
array_[baseIdx+1]:  regionType = MS_REGION_SPAN, reserved = baseIdx
array_[baseIdx+2]:  regionType = MS_REGION_SPAN, reserved = baseIdx
...
```

### 4.3 ReclaimRegionLarge

```cpp
void ReclaimRegionLarge(uintptr_t regionStart, size_t regionBytes) {
    UnitIndex unitIdx = (regionStart - heapStart_) / unitSize_;
    UnitCount unitCount = ceil(regionBytes, unitSize_);
    ReclaimUnitsInternal(unitIdx, unitCount);  // 归还到 dirtyTree
}
```

调用方 `CentralRegionPool::SweepLargeRegions()` 负责：
1. 收集所有 `isMarked == 0` 的死亡大对象
2. 重置 span MSRegionInfo entries（`Reset()`）
3. 重置 base MSRegionInfo（`Reset()`）
4. 调用 `ReclaimRegionLarge(regionStart, regionBytes)`

> **注意**：采用 two-pass 策略 — 先收集死亡列表（持锁），再逐个释放（无锁），避免持 `largeLock_` 期间调用 `ReclaimRegion` 导致锁嵌套。

## 5. 锁竞争与并发分析

### 5.1 锁资源清单

| 锁 | 保护对象 | 热度 | 使用方式 |
|----|---------|------|---------|
| `dirtyTreeMutex_` | `dirtyTree_` | **高** | 分配路径 `try_lock`；回收路径 `lock_guard` |
| `releasedTreeMutex_` | `releasedTree_` | 中 | 分配路径 `try_lock`；大对象回收 + ReleaseGarbageRegions 写入 |

### 5.2 分配路径的 try_lock 策略

TakeUnitsInternal / TakeUnitsAligned 使用 `try_lock`：

```cpp
while (tryDirtyTree || tryReleasedTree) {
    if (tryDirtyTree && dirtyTreeMutex_.try_lock()) {
        if (dirtyTree_.TakeUnitsLowestAddr(num, idx, false)) {
            dirtyTreeMutex_.unlock();
            ClearUnits(idx, num);   // ← 锁外进行 memset
            return true;
        }
        tryDirtyTree = false;       // 树空，不再尝试
        dirtyTreeMutex_.unlock();
    }
    // ... releasedTree 类似 ...
    ScopedEnterSaferegion enterSaferegion(true);  // 让出，避免自旋
}
```

**分析**：
- `try_lock` 失败不重试，直接 fallback 到下一级 → 低争用
- 但如果 dirtyTree 非空且锁被占用，会跳过有效空闲空间，改用 inactiveZone bump
- 这导致 **inactiveZone 水位线升高过快**，即使 dirtyTree 有可用空间

### 5.3 回收路径的阻塞锁

```cpp
void ReclaimUnitsInternal(UnitIndex idx, UnitCount num) {
    ScopedEnterSaferegion enterSaferegion(true);  // 允许 GC 暂停
    std::lock_guard<std::mutex> lg(dirtyTreeMutex_);
    dirtyTree_.MergeInsert(idx, num, false);
}
```

- **阻塞等待** — 多个 sweep 线程同时回收时会串行化
- `ScopedEnterSaferegion` 进入安全区，允许 GC STW — 避免持锁阻塞 GC

### 5.4 分配 vs 回收的锁冲突

| 场景 | 分配线程 | 回收线程 | 冲突 |
|------|---------|---------|------|
| 正常分配 | `try_lock` dirtyTree | — | 无冲突 |
| 分配 + sweep | `try_lock` 失败 → bump | `lock_guard` 持锁 | **分配 fallback 到 bump** |
| 多线程回收 | — | `lock_guard` 排队 | **回收串行化** |

### 5.5 ScopedEnterSaferegion 的作用

在 `try_lock` 循环中和 `ReclaimUnitsInternal` 中都使用了 `ScopedEnterSaferegion(true)`：

- **含义**：当前线程进入安全区，GC 可以 STW 暂停该线程
- **目的**：避免 mutator 持有 MSRegionManager 锁时阻塞 GC STW
- **风险**：如果 GC STW 在安全区内触发，锁会被持有直到 STW 结束 → 其他线程等锁更久

## 6. 性能瓶颈与优化方向汇总

### 6.1 瓶颈 P1: ClearUnits memset 开销

**问题**：从 dirtyTree 取出的 region 必须 `memset(0)` 清零 32KB（macOS 下 2 个 16KB 页）。

```cpp
void ClearUnits(UnitIndex idx, UnitCount num) const {
    uintptr_t addr = GetUnitAddress(idx);
    size_t bytes = num * unitSize_;        // 32KB for small region
    memset(reinterpret_cast<void*>(addr), 0, bytes);
}
```

**影响**：每次从 dirtyTree 分配新 region，分配线程阻塞 ~1-2μs（memset 32KB）。高分配速率下累积显著。

**优化方向**：
- **O6-a: 延迟清零**：不在 TakeRegion 时清零，而是在 `InitForSizeClass` 时仅清零 bitmap 相关区域（~几百字节）。对象分配时逐 slot 初始化即可。
- **O6-b: madvise 清零**：将 dirtyTree 的物理页 `madvise(DONTNEED)` 释放给 OS，移入 releasedTree。下次分配时 OS 自动提供零页。当前 releasedTree 从未被写入 — 可激活。
- **O6-c: 后台清零线程**：GC 完成后，后台线程将 dirtyTree 中的节点逐批 memset + 移入 releasedTree。

### 6.2 瓶颈 P2: releasedTree 小对象路径未激活

**问题**：小对象 Region 回收（`ReclaimRegion` → `ReclaimUnitsInternal`）总是写入 dirtyTree。只有大对象回收（`ReclaimUnitsReleased`）和 `ReleaseGarbageRegions()` 才写入 releasedTree。因此在纯小对象场景下 releasedTree 为空，双树设计的分离优势未充分利用。

```cpp
// ReclaimUnitsInternal — 小对象回收，总是写入 dirtyTree
std::lock_guard<std::mutex> lg(dirtyTreeMutex_);
dirtyTree_.MergeInsert(idx, num, false);

// ReclaimUnitsReleased — 大对象回收，写入 releasedTree（已存在）
std::lock_guard<std::mutex> lg(releasedTreeMutex_);
releasedTree_.MergeInsert(idx, num, false);

// ReleaseGarbageRegions — 将 dirtyTree 中的节点 madvise 后迁移到 releasedTree
```

**影响**：
- 纯小对象场景下，分配路径的 `releasedTreeMutex_.try_lock()` 分支空跑（树为空）
- 小对象 region 回收后仍需 memset 清零，未利用 OS 自动零页
- 如果将小对象回收也走 released 路径，可省去 ClearUnits 的 memset

#### CopyGC 的双路径方案（可复用参考）

CopyGC 的 `RegionManager` 提供了两个回收入口：

| 方法 | 目标树 | madvise | 用途 |
|------|--------|---------|------|
| `ReclaimRegion()` | **dirtyTree** | 无 | 小 region（<4MB），保留物理页 |
| `ReleaseRegion()` | **releasedTree** | 有 | 大 region（≥4MB），释放物理页给 OS |

**CopyGC ReleaseRegion 核心流程**（`RegionManager.cpp:422`）：

```cpp
size_t RegionManager::ReleaseRegion(RegionInfo* region) {
    region->InitFreeUnits();
    RegionInfo::ReleaseUnits(unitIndex, num);          // ← 释放物理页
    freeRegionManager.AddReleaseUnits(unitIndex, num); // ← 插入 releasedTree
    return res;
}
```

**ReleaseUnits 的平台实现**（`RegionInfo.h:564`）：

```cpp
static void ReleaseUnits(size_t idx, size_t cnt) {
    void* addr = GetUnitAddress(idx);
    size_t size = cnt * UNIT_SIZE;
#if defined(_WIN64)
    VirtualFree(addr, size, MEM_DECOMMIT);       // Windows: 解除提交
#elif defined(__APPLE__)
    memset(addr, 0, size);                         // macOS: 先清零
    mmap(addr, size, PROT_READ|PROT_WRITE,        //        再 MAP_FIXED 重映射
         MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED, -1, 0);
#else
    madvise(addr, size, MADV_DONTNEED);            // Linux: 通知内核释放
#endif
}
```

**分配侧差异**（`FreeRegionManager::TakeRegion`）：

```cpp
// 从 dirtyTree 分配 → 需要 ClearUnits(memset) 清零
if (dirtyUnitTree.TakeUnits(num, idx)) {
    RegionInfo::ClearUnits(idx, num);   // ← memset 32KB
    return region;
}
// 从 releasedTree 分配 → 无需清零（OS 自动零页）
if (releasedUnitTree.TakeUnits(num, idx)) {
    return region;   // ← 省去 memset！
}
```

#### MSRegionManager 小对象复用方案

**已有基础设施**：`ReclaimUnitsReleased()` 和 `ReleaseGarbageRegions()` 已实现 released 路径。**只需修改小对象回收路径**，让 `ReclaimRegion` 走 release 而非 dirty：

```cpp
// 修改 ReclaimRegion — 释放物理页 + 插入 releasedTree（而非 dirtyTree）
void MSRegionManager::ReclaimRegion(MSRegionInfo* region) {
    size_t regionIdx = region - array_;
    UnitIndex unitIdx = regionIdx * unitsPerMSRegion_;
    UnitCount unitCount = unitsPerMSRegion_;
    ReleaseUnits(unitIdx, unitCount);               // 释放物理页（已有 ReleaseGarbageRegions 中的实现可复用）
    ReclaimUnitsReleased(unitIdx, unitCount);        // 插入 releasedTree（方法已存在）
}
```

**效果**：
- 小对象 region 回收也走 released 路径 → **分配时省去 ClearUnits 的 32KB memset**
- 物理内存及时归还 OS → **降低 RSS**
- 充分利用已有的双树设计

**注意**：macOS 的 `mmap(MAP_FIXED)` 开销比 Linux 的 `madvise` 高（需要一次 memset + 一次系统调用）。如果 32KB region 回收频率很高，可考虑仅对大对象（`ReclaimRegionLarge`）做 release，小 region 仍走 dirtyTree — 这与 CopyGC 的策略一致。

### 6.3 瓶颈 P3: dirtyTreeMutex_ 分配/回收锁冲突

**问题**：分配用 `try_lock`，回收用 `lock_guard`。高压力下 sweep 线程长时间持锁做 MergeInsert，分配线程 try_lock 失败 → fallback 到 inactiveZone bump，导致水位线不必要地升高。

**影响**：
- inactiveZone 过早到达 heapEnd → 更频繁 OOM → 更频繁 GC
- dirtyTree 中明明有可用空间，却因为锁竞争无法使用

**优化方向**：
- **O6-e: 读写锁 / 无锁 Treap**：用 `shared_mutex` 或无锁数据结构替代 `std::mutex`。但 CartesianTree 结构复杂，改造成本高。
- **O6-f: 批量回收**：ReclaimUnitsInternal 累积到一定数量后一次性加锁插入，减少锁持有频率。
- **O6-g: 分配侧重试**：try_lock 失败后，在 fallback 到 inactiveZone 前先 `yield` + 重试一次 dirtyTree。避免过早消耗未使用堆空间。

### 6.4 瓶颈 P4: TakeUnitsLowestAddr O(log N) 复杂度

**问题**：`TakeUnitsLowestAddr` 在 CartesianTree 中找满足 count ≥ num 的最低地址节点，复杂度 O(log N)。虽然不算慢，但每次分配都要走一遍树搜索。

**影响**：对比 CopyGC 的简单 bump allocator（O(1)），MS-GC 每次 TakeRegion 多出约 100-500ns 的树操作。

**优化方向**：
- **O6-h: Region 缓存**：CentralRegionPool 层面已有 partial list 缓存，但可增加 "fresh region cache"（预取 N 个空闲 region），减少 TakeRegion 调用频率。
- **O6-i: 低地址优先可选**：如果不需要严格低地址优先，可使用 `TakeUnits`（First-Fit）替代 `TakeUnitsLowestAddr`，降低搜索开销。

### 6.5 瓶颈 P5: 只有完全空闲的 Region 才归还

**问题**：部分占用的 region 留在 CentralRegionPool 的 partial/full 列表中，不归还给 MSRegionManager。只有 `IsCompletelyFree()` 的 region 才执行 `Reset() + ReclaimRegion()`。

**影响**：
- 如果大量 region 只有 1-2 个存活对象，占用没法释放 → **内存碎片**
- 极端场景意味着堆利用率很低但 MSRegionManager 看到的空闲空间很少

**优化方向**：
- **O6-j: 碎片整理（Compaction）**：选择存活率低的 region，将存活对象搬移到其他 region → 完全清空 → 归还。这本质上是 CopyGC 的做法，在 MS-GC 中是选择性的。
- **O6-k: Region 合并复用**：不归还，但允许 CentralRegionPool 将多个同 SizeClass 的低存活率 partial region 合并。

### 6.6 优化优先级排序

| 编号 | 优化 | 影响面 | 实现难度 | 关联 Benchmark 瓶颈 |
|------|------|--------|---------|---------------------|
| **O6-d** | 小对象回收走 releasedTree | 高 | 低 | P1+P2: 消除 ClearUnits memset |
| **O6-g** | 分配侧 try_lock 重试 | 中 | 低 | P3: 减少不必要的 bump |
| **O6-f** | 批量回收 | 中 | 中 | P3: 减少锁频率 |
| **O6-h** | Fresh region 预取缓存 | 中 | 中 | P4: 减少树操作频率 |
| **O6-a** | 延迟清零 | 中 | 中 | P1: 分配路径加速 |
| **O6-j** | 碎片整理 | 高 | 高 | P5: 减少碎片，提高堆利用率 |

> **建议优先做 O6-d**：工程量最小（修改 `ReclaimRegion` 调用已有的 `ReleaseUnits` + `ReclaimUnitsReleased`），同时解决 P1（免 memset）和 P2（小对象也用 releasedTree），预期性能提升显著。注意 macOS 的 `mmap(MAP_FIXED)` 开销比 Linux 的 `madvise` 高，可考虑仅对大 region（或累积超过一定数量后批量）执行 release。
