# 大对象 Region 对齐 & 低地址优先分配 & 垃圾回收路径优化

> 日期：2026-02-28
> 分支：LOS-mac3

## 1. 概述

本文档记录三组碎片优化改动的实现细节：

| 改动 | 目标 | 核心思路 |
|------|------|----------|
| **大对象 region 对齐** | 消除不规则碎片 | 大对象分配的 unit 数向上对齐到标准 region 大小的整数倍 |
| **低地址优先分配** | 聚拢存活数据、腾出高地址连续空闲区 | dirtyTree/releasedTree 分配时选择最低地址的空闲块 |
| **garbageRegionList 地址排序 + 大对象直回 dirtyTree** | 垃圾复用也低地址优先，大对象回收旁路加速 | garbageRegionList 按地址升序维护；<4MB 大对象跳过 garbageRegionList 直接进入 dirtyTree |

三者配合，使堆空间中存活 region 紧凑排列在低地址端，高地址端形成大块连续空闲区，有利于多次分配后堆碎片的控制。

---

## 2. 改动一：大对象以 region 对齐

### 2.1 问题背景

堆空间以 unit（16 KB）为最小分配粒度，标准 region 由 `maxUnitCountPerRegion` 个 unit 组成（默认 128 KB / 16 KB = **8 units**）。

原始实现中，大对象分配的 unit 数仅按 `UNIT_SIZE` 向上取整：

```
regionCount = (size + UNIT_SIZE - 1) / UNIT_SIZE
```

例如一个 256 KB 的大对象需要 16 units，正好是 2 个标准 region。但如果是 260 KB，则需要 17 units——释放后在 freeRegionManager 中留下一个 **1 unit 的碎片空洞**，无法容纳任何标准 region，成为不可回收的碎片。

### 2.2 实现

**文件**：`src/Heap/Allocator/RegionManager.h` — `AllocLargeInMain()`

```cpp
uintptr_t AllocLargeInMain(size_t size)
{
    size_t regionCount = (size + RegionInfo::UNIT_SIZE - 1) / RegionInfo::UNIT_SIZE;
    // Round up to nearest multiple of maxUnitCountPerRegion (standard region size).
    // This ensures that when the large object is freed, the released units form
    // whole standard regions, preventing irregular fragmentation in freeRegionManager.
    regionCount = ((regionCount + maxUnitCountPerRegion - 1)
                   / maxUnitCountPerRegion) * maxUnitCountPerRegion;
    RegionInfo* region = TakeRegion(regionCount, RegionInfo::UnitRole::LARGE_SIZED_UNITS);
    // ...
}
```

**关键常量**：
- `UNIT_SIZE` = 系统页大小（macOS aarch64 = 16 KB）
- `MAX_UNIT_COUNT_PER_REGION` = 128 KB / 16 KB = **8 units**
- 对齐公式：`regionCount = ceil(regionCount / 8) × 8`

### 2.3 效果举例

| 对象大小 | 原始 units | 对齐后 units | 浪费 |
|----------|:---:|:---:|:---:|
| 256 KB | 16 | 16 (2×8) | 0 |
| 260 KB | 17 | 24 (3×8) | 7 units (112 KB) |
| 384 KB | 24 | 24 (3×8) | 0 |
| 130 KB | 9 | 16 (2×8) | 7 units (112 KB) |

**权衡**：对齐可能导致单个大对象多占 0–7 units（0–112 KB）。但释放后回归的是完整的标准 region 块，不会产生无法使用的碎片残片。

---

## 3. 改动二：低地址优先分配

### 3.1 问题背景

堆空间的空闲 unit 管理在 `FreeRegionManager` 中，底层使用两棵笛卡尔树（CartesianTree）：
- **dirtyUnitTree**：GC 回收后的脏页 unit（物理内存仍在）
- **releasedUnitTree**：已 `madvise(MADV_DONTNEED)` 释放的 unit（物理页已归还 OS）

原始的 `TakeUnits()` 使用最佳适配（Best Fit）策略——从根节点出发，在满足 `count >= num` 的节点中选择最小的块，分配到的地址是**随机分布**的——取决于树的形状，不保证地址顺序。

这导致存活 region 散落在堆地址空间的各处，高地址区域夹杂着存活和空闲交替的碎片，ShrinkInactiveZone 无法有效回收高地址尾部空间。

### 3.2 实现

改动分三层：

#### 3.2.1 CartesianTree 新增 `TakeUnitsLowAddr()` 和 `FindLowestSuitablePtr()`

**文件**：`src/Heap/Allocator/CartesianTree.h`

```cpp
// 在树中查找满足 count >= num 的最低地址节点
// 利用 BST 性质（左子树 = 更低地址）和堆性质（count < num 则子树都不满足）剪枝
// 时间复杂度 O(log n)
Node** FindLowestSuitablePtr(Node** nodePtr, Count num) const
{
    if (nodePtr == nullptr || *nodePtr == nullptr || (*nodePtr)->GetCount() < num) {
        return nullptr;
    }
    // 左子树地址更低，优先搜索
    Node** left = FindLowestSuitablePtr(&((*nodePtr)->l), num);
    if (left != nullptr) {
        return left;  // 左子树有满足条件的节点，它地址更低
    }
    // 左子树没有，当前节点就是最低地址的满足者
    return nodePtr;
}

// 从最低地址的满足节点分配 num 个 units
bool TakeUnitsLowAddrImpl(Count num, Index& idx, bool refreshRegionInfo)
{
    Node** nodePtr = FindLowestSuitablePtr(&root, num);
    if (nodePtr == nullptr) {
        return false;
    }
    Node* node = *nodePtr;
    idx = node->GetIndex();
    auto count = node->GetCount();

    // 从该节点的起始位置切走 num 个 units
    node->UpdateNode(idx + num, count - num, refreshRegionInfo, baseIndex);
    DecTotalCount(num);

    if (node->GetCount() == 0) {
        RemoveZeroNode(*nodePtr);
    } else {
        LowerNonZeroNode(*nodePtr);  // count 减小后下沉维护堆性质
    }
    return true;
}
```

**算法要点**：
- 笛卡尔树以 index（地址）为 BST 键、以 count（连续块大小）为 max-heap 键
- `FindLowestSuitablePtr` 先递归左子树（更低地址），利用堆性质 `count < num` 提前剪枝
- 从找到的节点**头部**切走 units（保留尾部），确保分配到该块中最低的地址

#### 3.2.2 FreeRegionManager 根据 `lowAddressFirst` 标志选择分配策略

**文件**：`src/Heap/Allocator/FreeRegionManager.h`

```cpp
// TakeRegion() 中的分配分支：
if (tryDirtyTree && dirtyUnitTreeMutex.try_lock()) {
    bool dirtyOk = lowAddressFirst
        ? dirtyUnitTree.TakeUnitsLowAddr(num, idx)   // 低地址优先
        : dirtyUnitTree.TakeUnits(num, idx);          // 原始策略（最佳适配）
    // ...
}

if (tryReleasedTree && releasedUnitTreeMutex.try_lock()) {
    bool releasedOk = lowAddressFirst
        ? releasedUnitTree.TakeUnitsLowAddr(num, idx) // 低地址优先
        : releasedUnitTree.TakeUnits(num, idx);       // 原始策略
    // ...
}
```

成员变量：
```cpp
bool lowAddressFirst = false;  // 默认关闭
void SetLowAddressFirst(bool enabled) { lowAddressFirst = enabled; }
```

#### 3.2.3 RegionManager 初始化时启用

**文件**：`src/Heap/Allocator/RegionManager.cpp` — `Init()`

```cpp
// Enable lowAddressFirst — allocate from low address to pack live regions at low end,
// maximizing the contiguous free zone at the high end for ShrinkInactiveZone.
freeRegionManager.SetLowAddressFirst(true);
```

### 3.3 效果示意

**未启用低地址优先**（存活 region 散乱分布）：
```
堆地址空间 (低 → 高)
[live][free][live][free][live][free][live][free][free][free]
                                                ↑ inactiveZone 只能回收这一小段
```

**启用低地址优先后**（存活 region 聚拢在低端）：
```
堆地址空间 (低 → 高)
[live][live][live][live][free][free][free][free][free][free]
                        ↑ inactiveZone 可以回收整个高端连续区
```

---

## 4. 两个改动的协同效应

| 场景 | 仅 region 对齐 | 仅低地址优先 | 两者配合 |
|------|:---:|:---:|:---:|
| 大对象释放后碎片 | 无碎片残片 | 仍有不规则残片 | **无碎片残片** |
| 存活 region 布局 | 散乱 | 聚拢低端 | **聚拢低端** |
| 高端连续空闲区 | 不保证 | 受碎片残片影响 | **最大化** |
| ShrinkInactiveZone 回收 | 不稳定 | 较好 | **最优** |

1. **region 对齐**确保释放的 units 是标准 region 的整数倍 → 重新分配时不会留下无法使用的碎片
2. **低地址优先**确保新分配的 region 排列在低端 → 释放的 units 聚集在高端形成连续块
3. 两者结合：低端紧密、高端整洁，ShrinkInactiveZone 可将整段高端空间归还 RESERVE

---

## 5. 改动三：garbageRegionList 地址排序

### 5.1 问题背景

GC 完成后，被回收的 region 进入 `garbageRegionList`。下次分配时，`TakeRegion()` 优先从 garbageRegionList 头部取 region 复用（无需 madvise，性能好）。

原始实现中 `garbageRegionList` 使用 `PrependRegion`（LIFO 插入链表头部），region 的地址顺序是**随机的**。这导致从 garbageRegionList 复用 region 时，可能取到高地址的 region，破坏了低地址优先分配的聚拢效果。

### 5.2 堆地址增长方向

确认：**堆从低地址向高地址增长**。

```cpp
// RegionManager::Initialize()
this->inactiveZone = regionHeapStart;  // 初始 = 堆最低地址

// TakeRegion() 扩展 inactiveZone
uintptr_t addr = inactiveZone.fetch_add(size);  // 向高地址推进
```

### 5.3 实现

#### 5.3.1 RegionList 新增地址排序插入方法

**文件**：`src/Heap/Allocator/RegionList.h`

```cpp
// Insert region maintaining address-ascending order (lowest address at head).
void InsertRegionSorted(RegionInfo* region, RegionInfo::RegionType type);
void InsertRegionSortedLocked(RegionInfo* region, RegionInfo::RegionType type);

// Merge source list into this list maintaining address-ascending order.
void MergeRegionListSorted(RegionList& srcList, RegionInfo::RegionType regionType);
```

**文件**：`src/Heap/Allocator/RegionManager.cpp`

```cpp
void RegionList::InsertRegionSortedLocked(RegionInfo* region, RegionInfo::RegionType type)
{
    // ... SetRegionType, SetPrevRegion(nullptr), IncCounts ...
    uintptr_t addr = region->GetRegionStart();

    // 查找插入点：在第一个地址更高的 region 之前插入
    RegionInfo* cur = listHead;
    while (cur != nullptr && cur->GetRegionStart() < addr) {
        cur = cur->GetNextRegion();
    }

    if (cur == nullptr) {
        // 追加到 tail（当前最高地址）
        listTail->SetNextRegion(region);
        region->SetPrevRegion(listTail);
        listTail = region;
    } else if (cur == listHead) {
        // 插入到 head（最低地址）
        region->SetNextRegion(listHead);
        listHead->SetPrevRegion(region);
        listHead = region;
    } else {
        // 在 cur 之前插入
        RegionInfo* prev = cur->GetPrevRegion();
        prev->SetNextRegion(region);
        region->SetPrevRegion(prev);
        region->SetNextRegion(cur);
        cur->SetPrevRegion(region);
    }
}
```

`MergeRegionListSorted` 将源链表逐个节点调用 `InsertRegionSortedLocked` 排序插入。

**复杂度**：单次插入 O(n)，n 为 garbageRegionList 长度。GC 时回收 k 个 region 总代价 O(k·n)。对于目前几百个 region 的规模，影响可忽略。

#### 5.3.2 替换 garbageRegionList 的插入方式

**文件**：`src/Heap/Allocator/RegionManager.h`

```cpp
// CollectRegion() — 单个 region 收集（小对象 region、<4MB 大对象 region）
size_t CollectRegion(RegionInfo* region)
{
    region->LockWriteRegion();
    // sorted insert — keep garbageRegionList in address-ascending order.
    garbageRegionList.InsertRegionSorted(region, RegionInfo::RegionType::GARBAGE_REGION);
    region->UnlockWriteRegion();
    // ...
}

// CollectFromSpaceGarbage() — from space 批量收集
void CollectFromSpaceGarbage()
{
    // sorted merge — keep garbageRegionList in address-ascending order
    // so TakeHeadRegion returns the lowest-address region first.
    garbageRegionList.MergeRegionListSorted(fromRegionList, RegionInfo::RegionType::GARBAGE_REGION);
}
```

#### 5.3.3 获取时自然低地址优先

`TakeRegion()` 中复用 garbageRegionList 的逻辑不需要改动：

```cpp
RegionInfo* head = garbageRegionList.TakeHeadRegion();
// head 现在是地址最低的 garbage region
```

因为链表已按地址升序排列，`TakeHeadRegion()` 自然返回最低地址的 region。

### 5.4 效果

garbageRegionList 的复用路径与 dirtyTree/releasedTree 的分配路径保持一致——**都优先选择低地址**。全链路低地址优先，杜绝任何环节将存活数据分散到高地址。

---

## 6. 改动四：大对象（<4MB）直回 dirtyTree

### 6.1 问题背景

`CollectLargeGarbage()` 中，被回收的大对象 region 按大小分两条路径：

| 原始路径 | 条件 | 去向 |
|----------|------|------|
| `ReleaseRegion` | > 4MB (`LARGE_OBJECT_RELEASE_THRESHOLD`) | `releasedTree`（释放物理页） |
| `CollectRegion` | ≤ 4MB | `garbageRegionList`（保留物理页，等待复用） |

小于 4MB 的大对象进入 garbageRegionList 后，复用时可能与小对象 region 需求不匹配（大对象 region 通常是多个标准 region 大小，如 16 units = 256KB），导致：
1. 在 `TakeRegion` 中取到但大小不匹配，需要 `ReclaimRegion` 再进 dirtyTree，多走一轮
2. 排序链表中大对象 region 占据低地址位置，阻塞小对象 region 的低地址复用

### 6.2 实现

**文件**：`src/Heap/Allocator/RegionManager.cpp` — `CollectLargeGarbage()`

```cpp
if (del->GetRegionSize() > RegionInfo::LARGE_OBJECT_RELEASE_THRESHOLD) {
    garbageSize += ReleaseRegion(del);  // >=4MB: 释放物理页 → releasedTree
} else {
    // small large objects (<4MB) go directly to dirtyTree,
    // skip garbageRegionList to avoid fragmenting the sorted garbage list.
    garbageSize += del->GetRegionSize();
    ReclaimRegion(del);                 // <4MB: 保留物理页 → dirtyTree
}
```

### 6.3 效果

- **<4MB 大对象**：直接 `ReclaimRegion` → `AddGarbageUnits` → `dirtyTree`。由 CartesianTree 的 `MergeInsert` 自动与相邻空闲块合并，被 `TakeUnitsLowAddr` 正常调度
- **garbageRegionList** 中不再混入大对象 region，链表更短，排序插入更快
- 原来 garbageRegionList 中大对象 region 取出后大小不匹配需要 `ReclaimRegion` 的额外步骤被消除

### 6.4 与改动三的配合

| 路径 | 小对象 region | <4MB 大对象 | ≥4MB 大对象 |
|------|:---:|:---:|:---:|
| garbageRegionList（排序） | ✅ 低地址优先复用 | ❌ 不再进入 | ❌ 不再进入 |
| dirtyTree（低地址优先） | ✅ | ✅ 新增 | ❌ |
| releasedTree（低地址优先） | ✅ | ❌ | ✅ |

所有回收路径的 region 最终都进入按地址排序的数据结构，保证全链路低地址优先。

---

## 7. 全局协同效应

| 场景 | 仅改动一+二 | 全部改动 |
|------|:---:|:---:|
| 大对象释放后碎片 | 无碎片残片 | **无碎片残片** |
| 存活 region 布局 | 聚拢低端（分配侧） | **聚拢低端（分配 + 复用全链路）** |
| garbageRegionList 复用 | 随机地址 | **低地址优先** |
| 大对象回收路径 | 经 garbageRegionList 中转 | **直回 dirtyTree，减少中间步骤** |
| 高端连续空闲区 | 较好 | **最优** |
| ShrinkInactiveZone 回收 | 较好 | **最优** |

---

## 8. 源码索引

| 文件 | 关键位置 | 内容 |
|------|----------|------|
| `src/Heap/Allocator/RegionManager.h` | `AllocLargeInMain()` | 大对象 unit 数向上对齐到 `maxUnitCountPerRegion` 的整数倍 |
| `src/Heap/Allocator/RegionManager.cpp` | `Init()` | `freeRegionManager.SetLowAddressFirst(true)` |
| `src/Heap/Allocator/FreeRegionManager.h` | `TakeRegion()` | 根据 `lowAddressFirst` 选择 `TakeUnitsLowAddr` 或 `TakeUnits` |
| `src/Heap/Allocator/CartesianTree.h` | `FindLowestSuitablePtr()` | O(log n) 查找最低地址满足节点 |
| `src/Heap/Allocator/CartesianTree.h` | `TakeUnitsLowAddrImpl()` | 从最低地址节点头部切走 units |
| `src/Heap/Allocator/RegionList.h` | `InsertRegionSorted()` | 按地址升序插入 garbageRegionList |
| `src/Heap/Allocator/RegionList.h` | `MergeRegionListSorted()` | 批量排序合并 from space 到 garbageRegionList |
| `src/Heap/Allocator/RegionManager.cpp` | `InsertRegionSortedLocked()` | O(n) 地址排序插入实现 |
| `src/Heap/Allocator/RegionManager.cpp` | `MergeRegionListSorted()` | 逐节点排序合并实现 |
| `src/Heap/Allocator/RegionManager.h` | `CollectRegion()` | `PrependRegion` → `InsertRegionSorted` |
| `src/Heap/Allocator/RegionManager.h` | `CollectFromSpaceGarbage()` | `MergeRegionList` → `MergeRegionListSorted` |
| `src/Heap/Allocator/RegionManager.cpp` | `CollectLargeGarbage()` | <4MB 大对象 `ReclaimRegion` → dirtyTree |
