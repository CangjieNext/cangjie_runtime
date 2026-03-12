# Step 1.3：大对象分配与堆增长设计

> 本文档从 Step1 §12/§13 抽取并细化，聚焦两个主题：
> 1. **大对象分配**：16KB < size ≤ 4MB 走 FreeRegionManager（Tier 1），> 4MB 走独立 mmap（Tier 2）
> 2. **堆增长**：大虚拟预留 + 分段元数据，元数据向低地址增长、堆向高地址增长

---

## 目录

1. [大对象分类与阈值](#1-大对象分类与阈值)
2. [Tier 1：FreeRegionManager 分配（16KB~4MB）](#2-tier-1freeregionmanager-分配16kb4mb)
3. [低地址优先分配改造](#3-低地址优先分配改造)
4. [Region 倍数对齐](#4-region-倍数对齐)
5. [Tier 2：mmap 超大对象（>4MB）](#5-tier-2mmap-超大对象4mb)
6. [堆增长与元数据方向](#6-堆增长与元数据方向)
7. [代码修改清单与 Go 对比](#7-代码修改清单与-go-对比)

---

## 1. 大对象分类与阈值

### 1.1 三级分配策略

```
RegionSpace::Allocate(totalSize)
│
├─ totalSize ≤ 16KB（kLargeObjectThreshold）
│   └─ 小对象路径：AllocBufferMS → SizeClass bitmap 分配（见 Step1.2）
│
├─ 16KB < totalSize ≤ 4MB（kHugeObjectThreshold）
│   └─ Tier 1 大对象：FreeRegionManager 分配
│      - 从 dirtyTree / releaseTree 取连续 Unit
│      - Region 大小按 32KB 倍数对齐
│      - 低地址优先策略减少碎片
│      - 释放后 Unit 归还 FreeRegionManager
│
└─ totalSize > 4MB
    └─ Tier 2 超大对象：独立 mmap
       - 每个对象单独 mmap 一段虚拟内存
       - 释放时 munmap 立即归还 OS
       - LargeObjectSpace 统一管理元数据
```

### 1.2 阈值设计依据

| 阈值 | 值 | 依据 |
|------|-----|------|
| `kLargeObjectThreshold` | 16KB | = Region(32KB) / 2，超过后 slot 数 < 2，bitmap 分配无意义 |
| `kHugeObjectThreshold` | 4MB | ≈ 128 个 Region(32KB)；超过后连续 Unit 需求过高，碎片风险大；Go 不区分此级别因为 arena 机制不同 |

```cpp
// SizeClassInfo.h（Step1.2 已定义）
static constexpr size_t kLargeObjectThreshold = 16384;   // 16KB

// 新增
static constexpr size_t kHugeObjectThreshold = 4 * 1024 * 1024;  // 4MB
```

### 1.3 与 Go 的对比

| | Go | Cangjie（改造后） |
|--|-----|------------------|
| 小对象 | ≤ 32KB → mcache/mcentral size class | ≤ 16KB → AllocBufferMS size class |
| 大对象 | > 32KB → mheap.alloc（pageAlloc 基数树） | 16KB~4MB → FreeRegionManager（CartesianTree） |
| 超大对象 | 无独立区分（全在 arena 内） | > 4MB → 独立 mmap（LargeObjectSpace） |

Go 不需要 Tier 2 是因为其 arena 机制（512GB 虚拟预留 + pageAlloc 基数树）可以高效管理任意大小的连续页。Cangjie 的 CartesianTree 在超大连续块需求下碎片风险较高，因此 4MB 以上独立 mmap 是更优选择。

## 2. Tier 1：FreeRegionManager 分配（16KB~4MB）

### 2.1 现有分配路径

```
RegionManager::AllocLarge(size)
  → regionCount = ceil(size / UNIT_SIZE)       // 4KB 对齐
  → TakeRegion(regionCount, LARGE_SIZED_UNITS)
    → FreeRegionManager::TakeRegion(num, uclass, expectPhysical)
      → ① dirtyUnitTree.TakeUnits(num, idx)   // 优先 dirty（已有物理页）
      → ② releasedUnitTree.TakeUnits(num, idx) // fallback released
    → 失败 → inactiveZone 扩展
  → region->Alloc(size)  // bump-pointer 一次性分配
  → 加入 recentLargeRegionList
```

### 2.2 现有问题

| 问题 | 原因 | 后果 |
|------|------|------|
| **UNIT_SIZE(4KB) 对齐碎片** | 80KB 对象 → 20 个 Unit；释放后这 20 个 Unit 不是 Region(32KB) 整数倍 | 无法被小对象的 GetFreshRegion（8 Unit 整块）复用，变成碎片 |
| **best-fit 策略散布碎片** | `TakeUnitsImpl` 用 BFS 找"最小满足块"，倾向于在各处留下零碎空洞 | 地址空间碎片化，大块需求越来越难满足 |
| **大小对象争用同一树** | 小对象 GetFreshRegion 和大对象 AllocLarge 都走同一对 dirtyTree/releaseTree | 大对象拿走大块后，小对象可用连续空间减少 |

### 2.3 改造方案

**两项关键改造**（详见 §3 和 §4）：

1. **低地址优先分配**：替换 best-fit 为 address-ordered first-fit，所有 TakeRegion 调用统一从低地址开始分配。碎片集中在高地址端，低地址端保持紧凑。

2. **Region 倍数对齐**：大对象 Region 大小按 32KB(REGION_SIZE) 的整数倍向上取整。释放后的 Unit 块恰好是 N 个完整 Region，可被小对象直接复用。

### 2.4 改造后的分配路径

```
RegionManager::AllocLarge(size)
  → regionCount = ceil(size / REGION_SIZE)     // 改造点：按 32KB 对齐
  → unitCount = regionCount * UNITS_PER_REGION // = regionCount * 8
  → TakeRegion(unitCount, LARGE_SIZED_UNITS)
    → FreeRegionManager::TakeRegion(num, uclass, expectPhysical)
      → ① dirtyUnitTree.TakeUnitsLowestAddr(num, idx)   // 改造点：低地址优先
      → ② releasedUnitTree.TakeUnitsLowestAddr(num, idx)
    → 失败 → inactiveZone 扩展
  → region->Alloc(size)
  → 加入 recentLargeRegionList
```

### 2.5 Tier 1 大对象的 GC 集成

Tier 1 大对象仍在主堆内，但**不参与中央池四-list 结构**（没有 size class），使用独立的 `LargeRegionList` 跟踪，采用 **Eager Sweep**（标记完成后立即回收）。

#### 2.5.1 为什么不放入中央池

| 维度 | 小对象 Region | Tier 1 大对象 Region |
|------|--------------|---------------------|
| size class | 有（0~35） | **无**（每个大小不同） |
| 线程本地缓存 | AllocBufferMS[cls] | **无**（直接从 FreeRegionManager 分配） |
| 中央池归属 | SizeClassRegionList[cls] 四-list | **不参与** |
| GC 开始时 flush | FlushAll() 归还线程本地 Region | **不需要**（没有线程本地缓存） |
| 一个 Region 中对象数 | 多个（bitmap 管理） | **1 个** |

大对象 Region 没有 size class，无法放入 per-size-class 的四-list 结构。即使强行加入，四-list 的 Lazy Sweep 机制（swept/unswept 分流、按需 sweep）对大对象也没有意义——大对象 Region 只含 1 个对象，sweep 仅是检查 1 bit。

#### 2.5.2 LargeRegionList 跟踪结构

```cpp
// 独立于中央池，单独跟踪所有 Tier1 大对象 Region
class LargeRegionList {
    std::mutex mu_;
    RegionInfo* head_ = nullptr;     // 所有存活的大对象 Region 链表
    size_t count_ = 0;

public:
    void Add(RegionInfo* region);          // AllocLarge 后加入
    void Remove(RegionInfo* region);       // 回收时移除
    void ForEach(std::function<void(RegionInfo*)> visitor);  // GC 遍历
    void SweepAndReclaim(FreeRegionManager& frm);            // Eager Sweep
};
```

#### 2.5.3 GC 各阶段行为

```
阶段 1: MarkSetup (STW)
│
│  小对象：FlushAll() 归还线程本地 Region 到中央池
│  小对象：ForceFinishSweep() 清扫上轮残留 unswept
│
│  大对象：无操作（没有线程本地缓存，没有 lazy sweep 残留）
│
阶段 2: ConcurrentMark
│
│  小对象：遍历中央池所有 Region，扫描 markBitmap
│  大对象：遍历 LargeRegionList，标记可达对象
│         → 每个大对象 Region 只有 1 个对象，检查 1 个根引用即可
│
阶段 3: MarkTerminate (STW)
│
│  小对象：处理 SATB 队列，最终标记
│  大对象：同上
│
阶段 4: 回收
│
│  小对象：Lazy Sweep — region 留在 *Unswept list，按需 sweep
│  大对象：Eager Sweep — 标记完成后立即遍历 LargeRegionList
│         → marked   → 清除 mark bit，保留在 LargeRegionList
│         → unmarked → 从 LargeRegionList 移除 → Unit 归还 FreeRegionManager
│
│  Tier 2 mmap：Eager Free — 遍历 LargeObjectSpace
│         → marked   → 清除 mark bit，保留
│         → unmarked → 直接 munmap 释放，立即归还 OS
```

#### 2.5.4 为什么用 Eager Sweep 而非 Lazy Sweep

| 维度 | Lazy Sweep（小对象方式） | Eager Sweep（大对象方式） |
|------|------------------------|-------------------------|
| sweep 代价 | 每个 Region 翻转整个 bitmap | 每个 Region **只检查 1 bit** |
| 数量级 | 可能数千个小对象 Region | 大对象 Region 通常很少（几十个） |
| 总耗时 | 需分摊到分配路径 | **几十次 bit 检查，微秒级** |
| 释放收益 | 延迟释放，碎片停留更久 | **立即归还 FreeRegionManager，减少碎片** |
| 实现复杂度 | 需要 swept/unswept 两组 list | 只需一个 list + 遍历 |

**核心理由**：大对象 Region 数量少、sweep 代价极低（1 bit/region），及时释放对碎片管理至关重要。没必要引入 Lazy Sweep 的复杂度。

#### 2.5.5 Eager Sweep 实现

```cpp
// GC MarkTerminate 之后立即调用
void LargeRegionList::SweepAndReclaim(FreeRegionManager& frm) {
    std::lock_guard lock(mu_);
    RegionInfo* curr = head_;
    while (curr != nullptr) {
        RegionInfo* next = curr->next_;
        if (curr->IsMarked()) {
            // 存活：清除 mark，保留
            curr->ClearMark();
            curr = next;
        } else {
            // 死亡：从链表移除，归还 FreeRegionManager
            Remove(curr);
            size_t unitCount = curr->GetUnitCount();
            size_t unitIdx = curr->GetUnitIndex();
            frm.AddGarbageUnits(unitIdx, unitCount);  // 归还 dirty tree
            curr = next;
        }
    }
}
```

#### 2.5.6 与 Go 对比

| | Go | Cangjie |
|--|-----|--------|
| 大对象跟踪 | 统一在 `mheap.sweepSpans` 中 | 独立 `LargeRegionList` |
| sweep 策略 | Lazy Sweep（与小对象统一） | **Eager Sweep**（标记后立即回收） |
| 原因 | Go 统一管理所有 span | 大对象不在中央池，独立管理更简洁 |

## 3. 低地址优先分配改造

### 3.1 现有 CartesianTree 分配策略分析

当前 `CartesianTree::TakeUnitsImpl` 的行为：

```cpp
// 现有代码（CartesianTree.h L407-440）
bool TakeUnitsImpl(Count num, Index& idx, bool refreshRegionInfo) {
    ForwardIterator it(*this);       // BFS 遍历（先根再子）
    Node** nodePtr = it.Next();      // 从根开始（根 = 最大块）
    Node* node = *nodePtr;
    if (node->GetCount() < num) return false;  // 最大块都不够

    // 逐层 BFS，找满足 >= num 的最后一个节点
    Node** nextNodePtr = nullptr;
    while ((nextNodePtr = it.Next()) != nullptr) {
        Node* nextNode = *nextNodePtr;
        if (nextNode->GetCount() < num) break;
        nodePtr = nextNodePtr;  // 记录最后一个满足条件的
    }
    // → 结果：best-fit（BFS 中最小的满足块）
    // → 问题：分配位置不确定，碎片散布在地址空间各处
}
```

**策略问题**：best-fit 总是找最紧凑的块来切割，短期看减少单次浪费，但长期导致大量零碎小块分布在地址空间中间，无法合并。

### 3.2 改造目标：address-ordered first-fit

将 `TakeUnits` 改为 **低地址优先**（address-ordered first-fit）：在所有 >= num 的空闲块中，选择 **起始地址最低** 的那个。

**碎片效果对比**：

```
初始：[===================== 256 Units 空闲 ======================]
       addr 0                                              addr 255

=== best-fit（现有）===
分配 3 → 从某个"恰好 3"的块切   → 碎片散布
分配 5 → 从某个"恰好 5-7"的块切 → 碎片散布
分配 8 → ...
结果：[ 2 | 用 |3| 用 |1| 用 | 5 |用| 用 | ... | 大块 ]
                                                ↑ 大块在高地址，但中间全是碎片

=== 低地址优先（改造后）===
分配 3 → 从 addr 0 切   → [用用用 | 剩余 253 ...]
分配 5 → 从 addr 3 切   → [用用用|用用用用用 | 剩余 248 ...]
分配 8 → 从 addr 8 切   → [用用用|用用用用用|用用用用用用用用 | 剩余 240 ...]
结果：低地址紧凑排列，高地址保持大块连续
```

### 3.3 CartesianTree 节点组织与查找

CartesianTree 是一棵 **Treap**（Tree + Heap）：
- **堆性质**：父节点的 Count ≥ 子节点的 Count（max-heap on Count）
- **BST 性质**：左子树的 Index < 当前 Index < 右子树的 Index（BST on Index）

**低地址查找利用 BST 性质**：最低地址的满足块一定在**左子树的最左路径**上。

```
                    Root (idx=100, cnt=50)      ← 最大块
                   /                    \
         (idx=20, cnt=30)         (idx=200, cnt=20)
         /            \
  (idx=5, cnt=12)  (idx=60, cnt=25)
```

要找 >= 10 的最低地址块：沿左子树走，idx=5 cnt=12 ≥ 10 → 命中（地址最低）。

### 3.4 新增方法：`TakeUnitsLowestAddr`

```cpp
// CartesianTree.h — 新增方法
bool TakeUnitsLowestAddr(Count num, Index& idx, bool refreshRegionInfo = true) {
    if (root == nullptr || num == 0) return false;
    if (root->GetCount() < num) return false;  // 最大块都不够

    // 利用 BST 性质找最低地址的满足块
    Node** bestPtr = nullptr;
    Node** curr = &root;
    while (*curr != nullptr) {
        Node* node = *curr;
        if (node->GetCount() >= num) {
            bestPtr = curr;        // 记录当前满足块
            curr = &(node->l);     // 尝试更低地址（左子树）
        } else {
            curr = &(node->r);     // 当前不满足，往右找更大的
        }
    }

    if (bestPtr == nullptr) return false;

    Node* best = *bestPtr;
    idx = best->GetIndex();
    auto count = best->GetCount();

    // 从低地址端切割（保留高地址部分）
    best->UpdateNode(idx + num, count - num, refreshRegionInfo);
    DecTotalCount(num);

    if (best->GetCount() == 0) {
        RemoveZeroNode(*bestPtr);
    } else {
        LowerNonZeroNode(*bestPtr);  // count 减少后可能需要下沉维护堆性质
    }
    return true;
}
```

**复杂度**：O(h) 其中 h 为树高。Treap 期望树高 O(log N)，N 为空闲块数量。

### 3.5 注意事项：BST 性质不完美

CartesianTree 的 BST 性质是按 `Index`（起始地址）排序的。但由于 `TakeUnitsLowestAddr` 查找时判断的是 `Count >= num`，而 Count 是 heap 维度，不是 BST 维度。因此不能简单地"一路向左走"。

上述实现的正确性：
- `node->GetCount() >= num` 时：当前节点满足，但左子树可能有更低地址的满足块 → 向左探
- `node->GetCount() < num` 时：当前不满足，左子树的 Count 只会更小（heap 性质）→ 左子树也不可能满足 → 向右探

**但这有个错误**：向右走时，右子树的左子树可能有更低地址但 Count 更大的节点（因为左子树的 Count 也受 heap 限制在子树内）。

实际上正确的做法需要考虑：**heap 性质保证子节点 Count ≤ 父节点 Count**。所以如果当前节点 Count < num，其所有子节点的 Count 也 < num，可以直接剪枝。修正：

```cpp
bool TakeUnitsLowestAddr(Count num, Index& idx, bool refreshRegionInfo = true) {
    if (root == nullptr || num == 0) return false;
    if (root->GetCount() < num) return false;

    // 递归搜索：在满足 Count >= num 的节点中，找 Index 最小的
    Node** bestPtr = FindLowestAddr(&root, num);
    if (bestPtr == nullptr) return false;

    Node* best = *bestPtr;
    idx = best->GetIndex();
    auto count = best->GetCount();

    best->UpdateNode(idx + num, count - num, refreshRegionInfo);
    DecTotalCount(num);

    if (best->GetCount() == 0) {
        RemoveZeroNode(*bestPtr);
    } else {
        LowerNonZeroNode(*bestPtr);
    }
    return true;
}

private:
// 在子树中找 Count >= num 且 Index 最小的节点
Node** FindLowestAddr(Node** nodePtr, Count num) {
    Node* node = *nodePtr;
    if (node == nullptr || node->GetCount() < num) {
        return nullptr;  // heap 性质：子节点 Count 更小，剪枝
    }

    // 先尝试左子树（地址更低，BST 性质）
    Node** leftResult = FindLowestAddr(&(node->l), num);
    if (leftResult != nullptr) {
        return leftResult;  // 左子树有满足块，地址一定更低
    }

    // 左子树没有满足块 → 当前节点就是最低地址满足块
    return nodePtr;
    // 注意：不需要搜索右子树，因为右子树地址一定更高
}
```

**复杂度**：期望 O(log N)，最坏 O(N)（退化为链表时）。

### 3.6 FreeRegionManager 改造

所有 `TakeRegion` 调用统一走低地址优先：

```cpp
// FreeRegionManager.h — 修改 TakeRegion
RegionInfo* TakeRegion(size_t num, RegionInfo::UnitRole uclass, bool expectPhysicalMem) {
    UnitIndex idx = 0;
    bool tryDirtyTree = true;
    bool tryReleasedTree = true;

    while (tryDirtyTree || tryReleasedTree) {
        if (tryDirtyTree && dirtyUnitTreeMutex.try_lock()) {
            // 改造点：TakeUnits → TakeUnitsLowestAddr
            if (dirtyUnitTree.TakeUnitsLowestAddr(num, idx)) {
                RegionInfo::ClearUnits(idx, num);
                RegionInfo* region = RegionInfo::InitRegion(idx, num, uclass);
                dirtyUnitTreeMutex.unlock();
                return region;
            }
            tryDirtyTree = false;
            dirtyUnitTreeMutex.unlock();
        }

        if (tryReleasedTree && releasedUnitTreeMutex.try_lock()) {
            // 改造点：TakeUnits → TakeUnitsLowestAddr
            if (releasedUnitTree.TakeUnitsLowestAddr(num, idx)) {
                RegionInfo* region = RegionInfo::InitRegion(idx, num, uclass);
                releasedUnitTreeMutex.unlock();
                PrehandleReleasedUnit(expectPhysicalMem, idx, num);
                return region;
            }
            tryReleasedTree = false;
            releasedUnitTreeMutex.unlock();
        }
        ScopedEnterSaferegion enterSaferegion(true);
    }
    return nullptr;
}
```

### 3.7 低地址优先的整体效果

| 场景 | 现有 best-fit | 低地址优先 |
|------|--------------|-----------|
| 小对象 GetFreshRegion | 碎片散布 | 小 Region 紧凑排列在低地址 |
| 大对象 AllocLarge | 碎片散布 | 大 Region 紧接小 Region 后排列 |
| 释放后合并 | 相邻概率低（因为分散） | 相邻概率高（因为低地址紧凑） |
| inactiveZone | 早期就被切碎 | 高地址保持连续，延迟 inactiveZone 消耗 |
| 与 Go 对比 | — | Go 的 `searchAddr` 也是低地址优先提示 |

## 4. Region 倍数对齐

### 4.1 问题：UNIT_SIZE 对齐导致碎片不可复用

当前大对象按 `UNIT_SIZE`（4KB）对齐：

```
80KB 对象 → ceil(80KB / 4KB) = 20 个 Unit
释放后 → dirtyTree 中有 20 个连续 Unit（80KB）

小对象 GetFreshRegion 需要 8 个 Unit（32KB = 1 个 Region）
→ 从这 20 Unit 中取 8 → 剩余 12 Unit
→ 再取 8 → 剩余 4 Unit
→ 4 Unit（16KB）无法构成完整 Region → 碎片！
```

### 4.2 改造：按 REGION_SIZE(32KB) 倍数向上对齐

```cpp
// RegionManager.cpp — AllocLarge 改造
uintptr_t AllocLarge(size_t size) {
    // 改造前：
    // size_t regionCount = (size + RegionInfo::UNIT_SIZE - 1) / RegionInfo::UNIT_SIZE;

    // 改造后：按 32KB Region 倍数对齐
    static constexpr size_t REGION_SIZE = 32 * 1024;  // 32KB
    static constexpr size_t UNITS_PER_REGION = REGION_SIZE / RegionInfo::UNIT_SIZE;  // 8

    size_t regionCount = (size + REGION_SIZE - 1) / REGION_SIZE;  // 向上取整到 Region 倍数
    size_t unitCount = regionCount * UNITS_PER_REGION;            // 转换为 Unit 数

    RegionInfo* region = TakeRegion(unitCount, RegionInfo::UnitRole::LARGE_SIZED_UNITS);
    if (region == nullptr) return 0;
    uintptr_t addr = region->Alloc(size);
    return addr;
}
```

### 4.3 对齐效果

```
80KB 对象 → ceil(80KB / 32KB) = 3 个 Region = 96KB = 24 Unit
              内部碎片：96KB - 80KB = 16KB（20%）

释放后 → dirtyTree 中有 24 个连续 Unit（96KB）
→ 小对象取 8 Unit → 剩余 16 Unit
→ 再取 8 Unit → 剩余 8 Unit
→ 再取 8 Unit → 剩余 0 Unit
→ 完美复用，零碎片！
```

### 4.4 内部碎片分析

| 对象大小 | 现有(4KB对齐) | 改造后(32KB对齐) | 碎片增量 |
|---------|--------------|-----------------|---------|
| 20KB | 20KB (5 Unit) | 32KB (8 Unit) | +12KB (60%) |
| 32KB | 32KB (8 Unit) | 32KB (8 Unit) | 0 |
| 48KB | 48KB (12 Unit) | 64KB (16 Unit) | +16KB (33%) |
| 64KB | 64KB (16 Unit) | 64KB (16 Unit) | 0 |
| 80KB | 80KB (20 Unit) | 96KB (24 Unit) | +16KB (20%) |
| 1MB | 1MB (256 Unit) | 1MB (256 Unit) | 0 |
| 4MB | 4MB (1024 Unit) | 4MB (1024 Unit) | 0 |

**结论**：
- 最坏情况：刚超过 N×32KB 一点 → 浪费近 32KB → 对于 > 16KB 的大对象，最坏碎片率 < 100%（对于刚超过 16KB 的对象）
- 实际上对于 > 48KB 的对象，碎片率 < 33%，且随着对象变大迅速趋近 0
- **关键收益**：释放后的 Unit 块可被小对象的 GetFreshRegion 完美复用，消除了外部碎片

### 4.5 起始地址对齐

除了大小按 Region 倍数对齐，起始地址也应对齐到 Region 边界：

```cpp
// TakeUnitsLowestAddr 返回的 idx 应按 UNITS_PER_REGION 对齐
// 这样释放后的块起始地址也在 Region 边界上
size_t alignedIdx = RoundUp(idx, UNITS_PER_REGION);
```

但 CartesianTree 内部管理是连续块，强制对齐起始地址会丢弃 `[idx, alignedIdx)` 这段。更好的做法是：

**方案 A**：`inactiveZone` 本身就是 Region 对齐的（REGION_SIZE = 8 × UNIT_SIZE），初始堆地址已对齐。低地址优先 + Region 倍数大小 → 释放后的块自然在 Region 边界上（因为每次分配/释放都是 8 的倍数 Unit）。

**方案 B**：在 `TakeUnitsLowestAddr` 中，要求分配时 `idx` 按 `UNITS_PER_REGION` 对齐。如果找到的最低地址块起始不对齐，先切掉前面的碎片（归还 tree），再从对齐位置开始分配。

推荐 **方案 A**——只要保证初始堆地址已对齐（当前代码 `regionHeapStart` 已是页对齐，页 = 4KB，而 Region = 32KB = 8×4KB，在 `Initialize` 时可保证 32KB 对齐），后续所有分配/释放都按 Region 倍数，起始地址自然对齐。

## 5. Tier 2：mmap 超大对象（>4MB）

### 5.1 设计理由

4MB+ 的对象在主堆内需要 128+ 个连续 Region（1024+ Unit），即使低地址优先也会面临：
- 连续性需求过高，碎片风险大
- 占用主堆大量虚拟地址空间，挤压小对象
- 释放后的大块可能长期无法被同等大小的请求复用

独立 mmap 的好处：
- `munmap` 立即归还 OS，无需等 GC / Scavenger
- 不占主堆 Unit 配额
- 不干扰 FreeRegionManager 的碎片管理

### 5.2 LargeObjectSpace

```cpp
// 新增: src/Heap/Allocator/LargeObjectSpace.h

struct LargeObjectInfo {
    MAddress baseAddr;          // mmap 起始地址（页对齐）
    size_t   mappedSize;        // mmap 映射大小（页对齐）
    size_t   objectSize;        // 实际对象大小
    uint8_t  gcMark;            // GC 标记位
    bool     isTraceAllocated;  // 是否在 GC trace 阶段分配
    LargeObjectInfo* next;      // 链表指针
    LargeObjectInfo* prev;
};

class LargeObjectSpace {
public:
    MAddress Allocate(size_t size);               // mmap + 注册
    void Free(MAddress addr);                     // munmap + 注销
    bool Contains(MAddress addr) const;           // O(1) 查找
    LargeObjectInfo* GetObjectInfo(MAddress addr) const;

    // GC 集成
    void MarkObject(MAddress addr);
    void SweepUnmarkedObjects();                  // sweep 未标记 → munmap
    void ClearAllMarks();

    // 遍历（供 GC Mark 使用）
    void ForEachObject(const std::function<void(MAddress, size_t)>& visitor);

    size_t GetTotalMappedSize() const;
    size_t GetObjectCount() const;

private:
    mutable std::shared_mutex mu_;
    std::unordered_map<MAddress, LargeObjectInfo*> objectMap_;  // 页对齐地址 key
    LargeObjectInfo* head_ = nullptr;                          // 遍历链表
    std::atomic<size_t> totalMappedSize_{0};
    std::atomic<size_t> objectCount_{0};
};
```

### 5.3 分配与释放

```cpp
MAddress LargeObjectSpace::Allocate(size_t size) {
    size_t allocSize = size + HEADER_SIZE;
    size_t mappedSize = RoundUp(allocSize, MRT_PAGE_SIZE);

    MAddress base = mmap(nullptr, mappedSize, PROT_READ|PROT_WRITE,
                         MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
    if (base == MAP_FAILED) return 0;

    auto* info = new LargeObjectInfo{base, mappedSize, size, 0, false, nullptr, nullptr};

    {
        std::unique_lock lock(mu_);
        objectMap_[base] = info;
        info->next = head_;
        if (head_) head_->prev = info;
        head_ = info;
    }
    totalMappedSize_.fetch_add(mappedSize);
    objectCount_.fetch_add(1);
    return base + HEADER_SIZE;
}

void LargeObjectSpace::Free(MAddress addr) {
    MAddress base = addr - HEADER_SIZE;
    std::unique_lock lock(mu_);
    auto it = objectMap_.find(base);
    if (it == objectMap_.end()) return;

    LargeObjectInfo* info = it->second;
    // 从链表移除
    if (info->prev) info->prev->next = info->next;
    if (info->next) info->next->prev = info->prev;
    if (info == head_) head_ = info->next;
    objectMap_.erase(it);

    totalMappedSize_.fetch_sub(info->mappedSize);
    objectCount_.fetch_sub(1);
    munmap(reinterpret_cast<void*>(info->baseAddr), info->mappedSize);
    delete info;
}
```

### 5.4 IsHeapAddress 问题与修正

#### 5.4.1 现有实现

```cpp
// Heap.h — 当前代码
static bool IsHeapAddress(MAddress addr) {
    return (addr >= heapStartAddr) && (addr < heapCurrentEnd);
}
```

纯区间检查：只判断地址是否在 `[heapStartAddr, heapCurrentEnd)` 主堆连续区间内。

#### 5.4.2 问题：三类地址无法识别

引入 Tier 2 LargeObjectSpace 和动态堆增长后，以下地址会被 `IsHeapAddress` **错误地返回 false**：

| 地址类型 | 原因 | 后果 |
|---------|------|------|
| **Tier 2 mmap 大对象** | `mmap(nullptr, ...)` 返回的地址由 OS 决定，不在主堆 `[heapStart, heapEnd)` 内 | GC 不追踪、写屏障不触发 → **存活对象被误回收** |
| **堆增长后新 Segment** | 如果 `heapCurrentEnd` 更新不原子，存在窗口期新分配地址 > 旧 `heapCurrentEnd` | 极短窗口内 barrier 判定错误（低风险但需注意） |
| **堆收缩后已释放区域** | Scavenger `madvise(MADV_DONTNEED)` 后物理页归还但虚拟地址仍在范围内 | `IsHeapAddress` 仍返回 true（**不是问题**——虚拟地址范围不变，只是物理页被释放） |

**最严重的问题是 Tier 2 mmap**。LOS 对象地址完全在主堆之外，当前实现完全无法识别。

#### 5.4.3 影响的调用方

```
IsHeapAddress 调用方（grep 结果）：
│
├─ Mutator.h (写屏障)
│   if (LIKELY(Heap::IsHeapAddress(obj))) { ... barrier ... }
│   → LOS 对象写屏障被跳过 → 并发标记期间引用更新丢失 → 悬挂引用
│
├─ WCollector.h (GC 遍历)
│   if (Heap::IsHeapAddress(obj)) { ... mark ... }
│   → LOS 中存活对象不被标记 → 被误 munmap
│
├─ Allocator.h (分配器内部)
│   bool IsHeapAddress(MAddress addr) const { return Heap::IsHeapAddress(addr); }
│   → 逻辑判断错误
│
└─ CjHeapData.cpp (Inspector)
    → 堆统计遗漏 LOS 对象
```

#### 5.4.4 修正方案

```cpp
// Heap.h — 修改后
static bool IsHeapAddress(MAddress addr) {
    // 快速路径：主堆范围（>99.9% 命中率）
    if (LIKELY(addr >= heapStartAddr && addr < heapCurrentEnd)) {
        return true;
    }
    // 慢速路径：LOS 查找（O(1) hash lookup）
    if (largeObjectSpace_ != nullptr) {
        return largeObjectSpace_->Contains(addr);
    }
    return false;
}
```

#### 5.4.5 各场景正确性分析

**场景 A：Tier 2 mmap 大对象**

```
LOS::Allocate(5MB)
  → mmap 返回 addr = 0x7f1234000000（OS 随机分配，不在主堆内）
  → objectMap_[0x7f1234000000] = info

写屏障触发：obj = 0x7f1234000000
  → addr >= heapStartAddr && addr < heapCurrentEnd → false（不在主堆）
  → largeObjectSpace_->Contains(0x7f1234000000) → true（hash 命中）
  → 执行 barrier ✅
```

**场景 B：堆增长（GrowOneSegment）**

```
GrowOneSegment():
  1. mprotect 提交新堆段物理页
  2. mprotect 提交对应元数据物理页
  3. heapCurrentEnd.store(newEnd, memory_order_release)  // 原子更新

竞态窗口：step 1-2 完成但 step 3 未执行时
  → 新分配的地址 > 旧 heapCurrentEnd
  → IsHeapAddress 返回 false
  → 但此时不会有对该地址的写屏障请求（对象尚未分配出去）
  → 安全 ✅（TakeRegion 必须在 heapCurrentEnd 更新后才返回新地址）
```

**场景 C：堆收缩（Scavenger 归还物理页）**

```
Scavenger 对空闲 Region 执行 madvise(MADV_DONTNEED):
  → 物理页归还 OS，但虚拟地址范围不变
  → heapStartAddr / heapCurrentEnd 不变
  → IsHeapAddress 对这些地址仍返回 true
  → 正确 ✅（这些地址仍是有效的堆虚拟地址，只是暂时没有物理页支撑）

注意：Scavenger 只释放 FreeRegionManager 管理的空闲 Unit，
不会释放正在使用的 Region。再次分配到这些 Unit 时，
OS 会自动重新分配物理页（page fault → COW）。
```

**场景 D：LOS 对象被 GC 回收（munmap）**

```
LOS::SweepUnmarkedObjects():
  → munmap(base, mappedSize)  // 虚拟地址释放
  → objectMap_.erase(base)    // 从 hash 表移除

此后 IsHeapAddress(base):
  → 主堆检查 false
  → largeObjectSpace_->Contains(base) → false（已从 map 移除）
  → 返回 false ✅
  → 正确：该地址已不再是堆地址
```

#### 5.4.6 性能影响

| 路径 | 频率 | 开销 |
|------|------|------|
| 快速路径（主堆命中） | >99.9% | 2 次比较，与现有相同 |
| 慢速路径（LOS lookup） | <0.1% | `unordered_map::count()`，O(1) 均摊 |
| `LIKELY` 提示 | — | 编译器分支预测优化，慢速路径不影响流水线 |

LOS 对象数量极少（通常几十个），hash 表很小，慢速路径实测开销可忽略。

#### 5.4.7 SafeGetRegionInfo 联动

`IsHeapAddress` 返回 true 不代表可以调 `GetRegionInfoAt()`——LOS 对象无 UnitInfo 元数据。所有需要访问 RegionInfo 的路径必须使用安全版本：

```cpp
inline RegionInfo* SafeGetRegionInfo(MAddress addr) {
    // 仅主堆地址有 RegionInfo
    if (LIKELY(addr >= Heap::heapStartAddr && addr < Heap::heapCurrentEnd)) {
        return RegionInfo::GetRegionInfoAt(addr);
    }
    return nullptr;  // LOS 对象或非堆对象 → 无 RegionInfo
}
```

**写屏障中的使用**：

```cpp
void WriteBarrier(MObject* obj, MObject** field, MObject* newVal) {
    if (!Heap::IsHeapAddress(obj)) return;          // 非堆对象，跳过

    RegionInfo* region = SafeGetRegionInfo(obj);
    if (region != nullptr) {
        // 主堆对象：正常 barrier（SATB / card marking）
        NormalWriteBarrier(region, obj, field, newVal);
    } else {
        // LOS 对象：简化 barrier（只需 SATB 入队，无需 card）
        SATBEnqueue(*field);  // 记录旧值供并发标记
    }
}
```

### 5.5 GC 集成

**标记**：GC Mark 遍历对象图时，遇到指向 LOS 的引用 → `LargeObjectSpace::MarkObject()`。

**回收（Eager Free）**：MarkTerminate 结束后**立即**调用 `SweepUnmarkedObjects()` → 未标记的超大对象直接 `munmap`，物理内存立即归还 OS。不走 Lazy Sweep，与 Tier 1 大对象统一在标记完成后立即回收。

```cpp
// GC MarkTerminate 之后，与 LargeRegionList::SweepAndReclaim 一起调用
void LargeObjectSpace::SweepUnmarkedObjects() {
    std::unique_lock lock(mu_);
    LargeObjectInfo* curr = head_;
    while (curr != nullptr) {
        LargeObjectInfo* next = curr->next;
        if (curr->gcMark) {
            curr->gcMark = 0;  // 清除 mark，保留
            curr = next;
        } else {
            // 未标记 → 直接 munmap 释放
            if (curr->prev) curr->prev->next = curr->next;
            if (curr->next) curr->next->prev = curr->prev;
            if (curr == head_) head_ = curr->next;
            objectMap_.erase(curr->baseAddr);
            totalMappedSize_.fetch_sub(curr->mappedSize);
            objectCount_.fetch_sub(1);
            munmap(reinterpret_cast<void*>(curr->baseAddr), curr->mappedSize);
            delete curr;
            curr = next;
        }
    }
}
```

**写屏障**：LOS 对象不在 Region 中，不能调用 `GetRegionInfoAt()`。使用安全版本：

```cpp
inline RegionInfo* SafeGetRegionInfo(MAddress addr) {
    if (LIKELY(addr >= Heap::heapStartAddr && addr < Heap::heapCurrentEnd)) {
        return RegionInfo::GetRegionInfoAt(addr);
    }
    return nullptr;  // LOS 或非堆对象
}
```

## 6. 堆增长与元数据方向

### 6.1 当前内存布局

```
mmap 一次性预留（MAP_NORESERVE）：
┌───────────────────────────────┬─────────────────────────────────────────────┐
│     UnitInfo 元数据区           │              Region 堆空间                  │
│   (nUnit × sizeof(UnitInfo))   │           (nUnit × UNIT_SIZE)               │
│                                │                                             │
│   ← 元数据从 base 开始,         │      → 堆从 regionHeapStart 开始,            │
│     连续排列到 heapStart        │        inactiveZone 向高地址增长              │
├───────────────────────────────┼─────────────────────────────────────────────┤
│ map->GetBaseAddr()     regionHeapStart        inactiveZone   regionHeapEnd  │
│ (= regionInfoStart)                                              ↑ 硬限制！ │
└───────────────────────────────┴─────────────────────────────────────────────┘
```

**元数据寻址方式**（`RegionInfo.h`）：
```cpp
// UnitInfo 排列在堆前方，第 idx 个 Unit 的 UnitInfo 地址：
static UnitInfo* GetUnitInfo(size_t idx) {
    return reinterpret_cast<UnitInfo*>(heapStartAddress) - (idx + 1);
    //                                 ↑ regionHeapStart    ↑ 向低地址偏移
}
```

这意味着：**元数据确实是向低地址增长的**（`base → heapStart`），**堆空间是向高地址增长的**（`heapStart → heapEnd`）。两者方向相反。

### 6.2 为什么方向相反是关键

```
元数据区    ←────────────────   regionInfoStart
                                    │
idx=0 的 UnitInfo 在 heapStart - sizeof(UnitInfo)
idx=1 的 UnitInfo 在 heapStart - 2×sizeof(UnitInfo)
...
idx=N 的 UnitInfo 在 heapStart - (N+1)×sizeof(UnitInfo)
                                    │
堆空间区    ────────────────→   inactiveZone → heapEnd
```

**扩展时的问题**：如果要增加堆空间（更多 Unit），需要同时增加元数据（更多 UnitInfo）。

- 堆空间向高地址扩展 → `regionHeapEnd` 右移 ✅ 没问题
- 元数据向低地址扩展 → 需要 `regionInfoStart` 左移 ❌ **但左边可能没有空间！**

### 6.3 解决方案：大虚拟预留 + 分段提交

**核心思路**：初始化时按**最大堆容量**预留元数据区和堆空间区的虚拟地址，但仅按需提交物理页。

```
初始化时的虚拟预留（64GB 虚拟，0 物理）:
┌────────────────────────────────┬──────────────────────────────────────────┐
│       元数据区（预留）           │              堆空间区（预留）              │
│                                │                                          │
│  ┌─────┐                       │  ┌─────────┐                             │
│  │已提交│← 首段元数据           │  │已提交     │← 首段堆空间                │
│  │ 物理 │                       │  │ 物理     │                             │
│  └─────┘                       │  └─────────┘                             │
│  │←── 预留但未提交 ──→│         │  │←── 预留但未提交 ──→│                  │
│                                │                                          │
├────────────────────────────────┼──────────────────────────────────────────┤
│ metaBase           metaEnd     │ heapStart              maxVirtualEnd      │
│ (= map->base)  (= heapStart)  │                                          │
└────────────────────────────────┴──────────────────────────────────────────┘
```

**扩展操作**（`HeapGrowthManager::GrowOneSegment`）：

```
当 inactiveZone 接近已提交堆空间上界时：
1. 提交下一段堆空间物理页（高地址方向）
   mprotect(heapStart + committed*SEGMENT_SIZE, SEGMENT_SIZE, PROT_READ|PROT_WRITE)

2. 提交对应的元数据物理页（低地址端，但在预留范围内）
   mprotect(metaBase + committed*META_PER_SEGMENT, META_PER_SEGMENT, PROT_READ|PROT_WRITE)

3. 更新 totalUnitCount 和 regionHeapEnd
```

### 6.4 元数据增长的正确语义

虽然 `GetUnitInfo(idx)` 的偏移方向是"从 heapStart 向低地址"，但**物理页提交方向**可以是正向的：

```
元数据物理页提交顺序:
  metaBase → 第一段 → 第二段 → ... → metaEnd(=heapStart)

UnitInfo 逻辑排列（idx 寻址方向）:
  heapStart ←── idx=0 ←── idx=1 ←── ... ←── idx=N ──→ metaBase

这两个方向不矛盾：
- 物理页是"从 metaBase 往 heapStart 方向"逐段提交
- UnitInfo 寻址是"从 heapStart 往 metaBase 方向"按 idx 偏移
- 只要 idx 对应的 UnitInfo 地址在已提交范围内，就能安全读写
```

**关键约束**：提交第 K 段堆空间时，必须同时提交它对应的元数据段。顺序：
```
堆空间第 0 段（Unit 0 ~ UNITS_PER_SEGMENT-1）
  → 需要 UnitInfo[0..UNITS_PER_SEGMENT-1]
  → 这些 UnitInfo 位于 heapStart 前最近处
  → = 元数据区的最后一段（最靠近 heapStart 的部分）

堆空间第 K 段
  → 需要 UnitInfo[K*UNITS_PER_SEGMENT .. (K+1)*UNITS_PER_SEGMENT - 1]
  → 这些 UnitInfo 位于 heapStart 前更远处
  → = 元数据区中更靠近 metaBase 的部分
```

**结论：堆空间向高地址增长，对应的元数据需求向低地址延伸**——两者方向确实相反，但因为整个虚拟地址范围已预留，提交操作只是 `mprotect` 让已预留的页变为可读写，不存在地址冲突。

### 6.5 参数与段管理

```cpp
static constexpr size_t MAX_VIRTUAL_RESERVE = 64ULL * 1024 * 1024 * 1024;  // 64GB
static constexpr size_t SEGMENT_SIZE = 256ULL * 1024 * 1024;                // 256MB / 段
static constexpr size_t UNITS_PER_SEGMENT = SEGMENT_SIZE / RegionInfo::UNIT_SIZE; // 65536
static constexpr size_t META_PER_SEGMENT = UNITS_PER_SEGMENT * sizeof(UnitInfo);

// 64GB = 256 个 Segment
// 每段元数据: 65536 × sizeof(UnitInfo) ≈ 5MB
// 全部元数据预留: 256 × 5MB ≈ 1.3GB 虚拟地址
```

### 6.6 GC 堆目标控制（类似 GOGC）

动态堆增长需要 GC 来限制膨胀：

```cpp
class HeapGoalController {
    size_t gcPercent = 100;  // 类似 GOGC

    size_t CalculateNextGCGoal(size_t liveSize) const {
        return liveSize + liveSize * gcPercent / 100;
    }

    bool ShouldTriggerGC(size_t currentHeapSize) const {
        return currentHeapSize >= nextGCGoal_;
    }
};
```

`gcPercent=100` 意味着堆用量达到上次 GC 存活量的 2 倍时触发下一次 GC。

### 6.7 Scavenger 后台归还

参照 Go `bgscavenge`，高于水位线的空闲物理页通过 `madvise(MADV_DONTNEED)` 归还 OS：

```
Scavenger 循环:
1. retainedGoal = max(nextGCGoal, heapMinimum)
2. releaseTarget = currentMapped - retainedGoal
3. releaseTarget > 0 → 扫描 FreeRegionManager 中 dirtyUnits:
   → madvise(MADV_DONTNEED) → 从 dirtyTree 移到 releasedTree
4. sleep 限流
```

### 6.8 与 Go 对比

| 维度 | Go | Cangjie |
|------|-----|---------|
| 虚拟预留 | 512GB+（arena hint） | 64GB（可配置） |
| 增长单位 | arena (64MB) | Segment (256MB) |
| 元数据 | heapArena 按 arena 独立分配 | UnitInfo 分段提交（连续地址偏移） |
| 元数据 vs 堆方向 | 独立（不相邻） | 相反（元数据在堆前方，向低地址延伸） |
| 地址→元数据 | arenaIndex 二级映射 | 直接偏移计算 `heapStart - (idx+1) × sizeof(UnitInfo)` |
| 收缩 | bgscavenge 线程 | Scavenger 线程 |
| GC 触发 | GOGC 百分比 | gcPercent（类似 GOGC） |

## 7. 代码修改清单与 Go 对比

### 7.1 代码修改清单

按模块分组，列出所有需要修改或新增的代码：

#### 7.1.1 CartesianTree 改造

| 文件 | 变更 | 说明 |
|------|------|------|
| `CartesianTree.h` | 新增 `FindLowestAddr(node, num)` | 递归最低地址搜索（§3.3） |
| `CartesianTree.h` | 新增 `TakeUnitsLowestAddr(num)` | 低地址优先分配公开接口（§3.4） |

#### 7.1.2 FreeRegionManager 改造

| 文件 | 变更 | 说明 |
|------|------|------|
| `FreeRegionManager.h` | `TakeRegion` 改用 `TakeUnitsLowestAddr` | 所有路径统一低地址优先（§3.5） |

#### 7.1.3 大对象分配改造

| 文件 | 变更 | 说明 |
|------|------|------|
| `RegionAllocator.cpp` | `AllocLarge` 对齐粒度从 UNIT_SIZE 改为 REGION_SIZE | `numUnits = ceil(size/REGION_SIZE)*UNITS_PER_REGION`（§4） |
| `RegionAllocator.cpp` | 新增 Tier2 分支：`size > 4MB` 走 LargeObjectSpace | 阈值路由（§2.4） |

#### 7.1.4 LargeObjectSpace（新增）

| 文件 | 变更 | 说明 |
|------|------|------|
| `LargeObjectSpace.h/cpp` | **新增** | Tier2 超大对象空间（§5） |
| `LargeObjectSpace.h/cpp` | `Allocate(size_t)` → `MAddress` | mmap 分配 + 注册 |
| `LargeObjectSpace.h/cpp` | `Free(MAddress)` | munmap 释放 + 注销 |
| `LargeObjectSpace.h/cpp` | `Contains(MAddress)` → `bool` | O(1) 查询 |
| `LargeObjectSpace.h/cpp` | `ForEach(callback)` | GC 遍历接口 |

#### 7.1.5 堆管理改造

| 文件 | 变更 | 说明 |
|------|------|------|
| `RegionManager.cpp` | `Initialize` 改为大虚拟预留 + 分段提交 | 64GB MAP_NORESERVE（§6.3） |
| `RegionManager.cpp` | 新增 `GrowOneSegment()` | 同时提交堆段 + 元数据段（§6.3） |
| `RegionManager.h` | 新增 `HeapGoalController` | gcPercent 触发控制（§6.6） |
| `RegionManager.cpp/h` | 新增 `Scavenger` 线程 | 周期性 `madvise(MADV_DONTNEED)`（§6.7） |

#### 7.1.6 GC 集成与 IsHeapAddress 修正

| 文件 | 变更 | 说明 |
|------|------|------|
| `Heap.h` | `IsHeapAddress` 增加 LOS 慢速路径 | 快速路径主堆区间 + 慢速路径 `los->Contains()`（§5.4） |
| `Heap.h` | 新增 `SafeGetRegionInfo()` | LOS 对象无 UnitInfo，需安全版本（§5.4.7） |
| `Mutator.h` | 写屏障适配 LOS 对象 | `SafeGetRegionInfo` 返回 nullptr 时走简化 SATB 路径（§5.4.7） |
| `Collector.cpp` | 标记阶段遍历 LargeObjectSpace | `los->ForEach` 标记可达对象 |
| `Collector.cpp` | 回收阶段释放 LOS 不可达对象 | `los->SweepUnmarkedObjects` 立即 munmap |
| `Collector.cpp` | 回收阶段释放 Tier1 大对象 | `LargeRegionList::SweepAndReclaim` 归还 FreeRegionManager |
| `Collector.cpp` | GC 结束后更新 `nextGCGoal` | `HeapGoalController::CalculateNextGCGoal` |
| `CjHeapData.cpp` | Inspector 堆统计加入 LOS | `GetTotalMappedSize()` + `GetObjectCount()` |

### 7.2 Go 对比汇总

| 维度 | Go | Cangjie Step1.3 |
|------|-----|-----------------|
| **小对象** | mspan（size class） | SizeClassRegionList（Step1.2） |
| **大对象阈值** | 32KB | **16KB**（与 size class 上界对齐） |
| **大对象分配** | `largeAlloc` → mheap → treap | Tier1: FreeRegionManager → CartesianTree |
| **超大对象** | 同上（Go 不区分） | **Tier2: >4MB 走 mmap**（独立地址空间） |
| **Treap 策略** | address-ordered first-fit | **low-address first-fit**（`FindLowestAddr`） |
| **对齐粒度** | pageSize (8KB) | **REGION_SIZE (32KB)**（Region 倍数） |
| **堆虚拟预留** | 512GB+（arena hint） | 64GB（连续 MAP_NORESERVE） |
| **地址→元数据** | arenaIndex 二级间接 | 直接偏移（`heapStart - (idx+1)*UnitInfo`） |
| **堆增长单位** | arena (64MB) | Segment (256MB) |
| **GC 触发** | GOGC + memory limit | gcPercent（类似 GOGC） |
| **物理页归还** | bgscavenge → sysUnused | Scavenger → madvise(MADV_DONTNEED) |
| **dirty/released 分离** | scavengeIndex + pageAlloc | dirtyTree / releasedTree（已有） |

### 7.3 实施分阶段建议

| 阶段 | 内容 | 依赖 |
|------|------|------|
| **P0** | CartesianTree `TakeUnitsLowestAddr` + FreeRegionManager 切换 | 无 |
| **P0** | `AllocLarge` Region 倍数对齐 | 无 |
| **P1** | LargeObjectSpace 新增 + GC 集成 | P0 |
| **P2** | 大虚拟预留 + 分段提交 + HeapGoalController | P0 |
| **P3** | Scavenger 后台归还 | P2 |
