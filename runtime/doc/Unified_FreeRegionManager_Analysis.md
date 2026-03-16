# 统一 FreeRegionManager 可行性分析

**日期**：2026年3月16日

---

## 1. 提案概述

将当前 MAIN 的 `freeRegionManager` 和 LOS 的 `losFreeMgr` 合并为一个统一的 FreeRegionManager，覆盖整个堆空间 `[regionHeapStart, losEnd)`。分配时通过地址边界约束区分空间归属：

- **MAIN 分配**：低地址优先，取到的块不能超过 `inactiveZone`（包含 RESERVE 借用部分）
- **LOS 分配**：高地址优先，取到的块不能低于 `losInactiveZone`
- **跨边界块**：截取本空间内的部分，剩余归还到树中
- **核心收益**：GC 期间不再需要重构/修剪 tree

---

## 2. 当前架构 vs 提案架构

### 2.1 当前架构

```
MAIN freeRegionManager                          LOS losFreeMgr
  dirtyTree (local idx 0-based)                   dirtyTree (local idx 0-based, base=mainUnitCount)
  releasedTree                                    releasedTree
  mutex × 2                                       mutex × 2
  lowAddressFirst = true                          lowAddressFirst = false
```

问题：
- ShrinkInactiveZone 需要 `RemoveAboveIndex()` 修剪 MAIN 树
- CompactLOS Phase 6 需要 `ClearTrees()` + 完全重建 LOS 树
- RESERVE 归还需要在空间之间移动 watermark，逻辑分散

### 2.2 提案架构

```
Unified FreeRegionManager
  dirtyTree (global idx, 覆盖 [0, totalUnitCount))
  releasedTree (global idx)
  mutex × 2 (共享)

MAIN: TakeRegionBounded(num, upperBound=inactiveZoneIdx)  — 低地址优先
LOS:  TakeRegionBounded(num, lowerBound=losInactiveZoneIdx) — 高地址优先
```

---

## 3. 可行性分析：✅ 可行，但需要解决几个关键问题

### 3.1 ✅ ShrinkInactiveZone — 完全不需要修改树

**当前**：`RemoveAboveIndex(threshold)` 从树中删除高地址空闲块。

**统一后**：只需更新 `inactiveZone` 值。高于 `inactiveZone` 的空闲块仍在树中，但 MAIN 的 `TakeRegionBounded(num, upperBound)` 不会取到它们，因为有上界约束。这些块对 LOS 侧仍可用（如果在 LOS 范围内）。

```
Before:  RemoveAboveIndex(threshold)  → O(log n) 树修剪 + 节点删除
After:   inactiveZone = newValue      → O(1) 变量赋值
```

**收益**：O(log n) → O(1)，且消除了 ghost route 保护的复杂逻辑。

### 3.2 ⚠️ CompactLOS Phase 6 — 仍需部分清理，但简化

**当前**：`ClearTrees()` 清空 losFreeMgr 的两棵树 → 重新扫描间隙 → 逐个 `AddReleaseUnits`。

**统一后**：不能 ClearTrees（会清掉 MAIN 的空闲块）。需要：

**方案 A：Range Remove + 重新插入**
```
1. RemoveInRange(losActiveStart, losEnd)  — 删除 LOS 区域的所有旧空闲块
2. 执行 memmove 整理
3. 重新插入新间隙到统一树
```

需要在 CartesianTree 中新增 `RemoveInRange(lo, hi)` 方法。利用 BST 性质可以 O(k log n) 完成（k = LOS 中空闲块数量，通常很小）。

**方案 B：逐个删除旧块 + 插入新块**
```
1. 整理前记录 LOS 区域的所有空闲块 [(idx, count), ...]
2. 逐个从统一树中删除（Remove by index）
3. 执行 memmove 整理
4. 插入新间隙
```

CartesianTree 已有按 index 查找的能力（BST 性质），新增 `RemoveByIndex(idx)` 即可。成本 O(k log n)。

**方案 C：标记-跳过**（更激进，但最简单）

不删除旧块。整理后：
- 旧空闲块位置可能被 moved 对象占据 → 在 RegionInfo 中标记为已占用
- TakeRegion 取到已占用块时检测 → 放弃并重试

❌ 不推荐：破坏了 CartesianTree 的不变量，且有浪费。

**推荐方案 A**，新增 `RemoveInRange`。

### 3.3 ✅ RESERVE 借用/归还 — 大幅简化

**当前**：
- BorrowForMain → 移动 mainWatermark → 初始化元数据 → 新空间进入 MAIN 的 freeRegionManager
- BorrowForLOS → 移动 losWatermark → 初始化元数据 → 新空间进入 losFreeMgr
- ReclaimMainBorrowed → 推回 mainWatermark
- ReclaimLOSBorrowed → 推回 losWatermark

**统一后**：
- "借用" = 扩展 inactiveZone（MAIN）或 losInactiveZone（LOS）到 RESERVE 区域
- 初始化新空间的元数据 → 空闲块自然在统一树中可用
- "归还" = 收缩 inactiveZone / losInactiveZone
- 无需移动 watermark，空闲块不需要移动

```
Before:  BorrowForMain → mainWatermark CAS → init metadata → space available
After:   inactiveZone CAS expand → init metadata → space already in unified tree
```

💡 **关键简化**：ReserveManager 的 watermark 管理可以与 inactiveZone/losInactiveZone 合并。"RESERVE 可用量" = losInactiveZone - inactiveZone（中间未被任何一方 claim 的区域）。

### 3.4 ⚠️ CartesianTree API 需要扩展

需要新增的方法：

| 方法 | 用途 | 复杂度 |
|------|------|--------|
| `TakeUnitsLowAddrBounded(num, upperBound)` | MAIN 分配：低地址优先 + 上界约束 | O(log n) |
| `TakeUnitsHighAddrBounded(num, lowerBound)` | LOS 分配：高地址优先 + 下界约束 | O(log n) |
| `RemoveInRange(lo, hi)` | CompactLOS 前清除 LOS 区域旧空闲块 | O(k log n) |

#### TakeUnitsLowAddrBounded 算法

```cpp
// 找最低地址的 node 满足：count >= num 且 index + num <= upperBound
Node** FindLowestSuitableBounded(Node** nodePtr, Count num, Index upperBound) const
{
    if (!nodePtr || !*nodePtr || (*nodePtr)->GetCount() < num) return nullptr;
    Node* node = *nodePtr;
    
    // 左子树有更低地址
    Node** left = FindLowestSuitableBounded(&node->l, num, upperBound);
    if (left) return left;
    
    // 当前节点：检查是否在边界内
    if (node->GetIndex() + num <= upperBound) {
        return nodePtr;  // 完整块在边界内
    }
    
    // 当前节点跨边界：可以截取 [index, upperBound) 部分
    // 但 upperBound - index 可能 < num → 不够 → 无法满足
    // 如果 upperBound - index >= num → 这等价于 index + num <= upperBound，已在上面处理
    // 所以到这里说明当前节点和右子树都无法满足（右子树地址更高）
    return nullptr;
}
```

**跨边界块截取**的场景：

```
如果请求 num=1 个 unit，有空闲块 [index=98, count=10)，upperBound=100：
  → index + num = 99 <= 100 ✓  → 可以从块的低端取 1 个 unit
  → 块变为 [99, 9)，仍在树中
```

这与现有的 `TakeUnitsLowAddr` 行为一致——总是从块的低端分配，块的高端部分留在树中。**所以只需要加上界检查，不需要特殊的截取逻辑**。

#### TakeUnitsHighAddrBounded 算法

```cpp
// 找最高地址的 node 满足：count >= num 且 index >= lowerBound
Node** FindHighestSuitableBounded(Node** nodePtr, Count num, Index lowerBound) const
{
    if (!nodePtr || !*nodePtr || (*nodePtr)->GetCount() < num) return nullptr;
    Node* node = *nodePtr;
    
    // 右子树有更高地址
    Node** right = FindHighestSuitableBounded(&node->r, num, lowerBound);
    if (right) return right;
    
    // 当前节点：检查是否在边界内
    if (node->GetIndex() >= lowerBound) {
        return nodePtr;  // 块完全在边界内
    }
    
    // 当前节点跨界：块起始 < lowerBound，但末尾可能 > lowerBound
    // 检查 lowerBound 以上的部分是否足够
    if (node->GetIndex() + node->GetCount() > lowerBound) {
        Index usableStart = lowerBound;
        Count usableCount = node->GetIndex() + node->GetCount() - lowerBound;
        if (usableCount >= num) {
            return nodePtr;  // 需要特殊处理：从块高端截取
        }
    }
    
    return nullptr;
}
```

**高地址分配的截取逻辑**：

```
空闲块 [index=95, count=10)，lowerBound=98，请求 num=3：
  → 可用部分 [98, 105) = 7 个 unit >= 3 ✓
  → 从高端取 3 个 unit：分配 [102, 105)
  → 块更新为 [95, 7)
  或者等价地：从可用区域低端取，分配 [98, 101)
  → 块拆分为 [95, 3) 在界外（留在树中）
```

需要修改 `TakeUnitsHighAddrBounded` 的取块逻辑：不像 LowAddr 那样从 index 端取，而是：
- 如果整块在 lowerBound 以上：从块高端取（`index + count - num`）
- 如果块跨界：截掉 lowerBound 以下部分，从剩余中取

### 3.5 ⚠️ 锁竞争

**当前**：MAIN 和 LOS 各有独立的 2 个 mutex（dirtyMutex + releasedMutex），互不干扰。

**统一后**：共享 2 个 mutex。

**影响分析**：
- LOS 分配频率远低于 MAIN（大对象少）→ 竞争增加有限
- GC STW 期间只有 GC 线程操作树 → 无竞争
- Mutator 阶段：MAIN TakeRegion（高频）vs LOS AllocLarge（低频）→ 偶尔竞争
- 已有 `try_lock` + `ScopedEnterSaferegion` 退避机制 → 竞争时不会死锁

**结论**：可接受。如果成为瓶颈，可考虑读写锁或分段锁。

### 3.6 ✅ baseUnit 处理

当前 losFreeMgr 使用 `baseUnit = mainUnitCount`，将 LOS 的 local index 映射到 global index。

统一后，所有操作都使用 global index（`baseUnit = 0`），消除了 local↔global 转换。代码更简单。

---

## 4. 需要修改的组件

| 组件 | 改动 | 工作量 |
|------|------|--------|
| **CartesianTree.h** | 新增 `FindLowestSuitableBounded`、`FindHighestSuitableBounded`、`TakeUnitsHighAddr`、`RemoveInRange` | 中 |
| **FreeRegionManager.h** | 新增 `TakeRegionBounded(num, upperBound/lowerBound, direction)`；移除 `baseUnit` 概念 | 中 |
| **RegionManager.h/cpp** | `TakeRegion` 传入 `inactiveZone` 作为上界；移除独立的 `freeRegionManager` 字段，改用共享实例 | 中 |
| **LOSManager.h/cpp** | 移除 `losFreeMgr`；分配/回收都用统一 FreeRegionManager + 下界约束 | 大 |
| **LOSManager.cpp (CompactLOS)** | Phase 6 改用 `RemoveInRange` + 重新插入，不再 ClearTrees | 中 |
| **RegionManager.cpp (ShrinkInactiveZone)** | 移除 `RemoveAboveIndex` 调用，只更新 `inactiveZone` | 小（简化） |
| **ReserveManager** | 简化或移除水位管理，借用 = 扩展 inactiveZone/losInactiveZone | 中 |

---

## 5. 详细设计草案

### 5.1 地址空间模型

```
regionHeapStart                    inactiveZone  losInactiveZone                losEnd
    |                                   |              |                           |
    [  MAIN 活跃区 (live + free)  ][  未初始化  ][  LOS 活跃区 (live + free)  ]
    |<-- MAIN 可分配范围 -------->|              |<-- LOS 可分配范围 -------->|
                                  |<-- 可借用 -->|
```

- **MAIN 分配**：`TakeRegionBounded(num, upperBound=UnitIdx(inactiveZone))`
- **LOS 分配**：`TakeRegionBounded(num, lowerBound=UnitIdx(losInactiveZone))`
- **RESERVE 可用** = `losInactiveZone - inactiveZone`
- **MAIN 借用** = 向上移动 `inactiveZone`（进入 RESERVE 区）
- **LOS 借用** = 向下移动 `losInactiveZone`（进入 RESERVE 区）

### 5.2 统一 FreeRegionManager API

```cpp
class UnifiedFreeRegionManager {
public:
    // MAIN: 低地址优先，块的结束地址不超过 upperBound
    RegionInfo* TakeRegionLowAddr(size_t num, UnitRole role, bool expectPhys, UnitIndex upperBound);
    
    // LOS: 高地址优先，块的起始地址不低于 lowerBound
    RegionInfo* TakeRegionHighAddr(size_t num, UnitRole role, bool expectPhys, UnitIndex lowerBound);
    
    // 通用：添加/回收空闲块（不区分空间）
    void AddGarbageUnits(UnitIndex idx, UnitCount num);   // → dirtyTree
    void AddReleaseUnits(UnitIndex idx, UnitCount num);    // → releasedTree
    
    // CompactLOS 专用：删除指定范围的空闲块
    UnitCount RemoveInRange(UnitIndex lo, UnitIndex hi);
    
    // ShrinkInactiveZone 不再需要任何树操作
    // RemoveAboveIndex → 可保留但不再被 ShrinkInactiveZone 调用
};
```

### 5.3 MAIN TakeRegion 修改

```cpp
// Before:
RegionInfo* region = freeRegionManager.TakeRegion(num, role, expectPhys);
if (!region) expandInactiveZone();

// After:
UnitIndex upperBound = AddrToUnitIdx(inactiveZone.load());
RegionInfo* region = unifiedFreeMgr.TakeRegionLowAddr(num, role, expectPhys, upperBound);
if (!region) {
    // 扩展 inactiveZone（可能进入 RESERVE）
    expandInactiveZone(num);
    upperBound = AddrToUnitIdx(inactiveZone.load());
    region = unifiedFreeMgr.TakeRegionLowAddr(num, role, expectPhys, upperBound);
}
```

### 5.4 LOS AllocLarge 修改

```cpp
// Before:
RegionInfo* region = losFreeMgr.TakeRegion(unitCount, LARGE_SIZED_UNITS, false);
if (!region) expandLosInactiveZone();

// After:
UnitIndex lowerBound = AddrToUnitIdx(losInactiveZone.load());
RegionInfo* region = unifiedFreeMgr.TakeRegionHighAddr(unitCount, LARGE_SIZED_UNITS, false, lowerBound);
if (!region) {
    // 扩展 losInactiveZone 向低地址（可能进入 RESERVE）
    expandLosInactiveZone(unitCount);
    lowerBound = AddrToUnitIdx(losInactiveZone.load());
    region = unifiedFreeMgr.TakeRegionHighAddr(unitCount, LARGE_SIZED_UNITS, false, lowerBound);
}
```

### 5.5 CompactLOS Phase 6 修改

```cpp
// Before:
losFreeMgr.ClearTrees();
for (gap in sortedGaps) {
    losFreeMgr.AddReleaseUnits(gapIdx, gapCount);
}

// After:
UnitIndex losRangeStart = AddrToUnitIdx(losInactiveZone.load());
UnitIndex losRangeEnd = AddrToUnitIdx(losEnd);
unifiedFreeMgr.RemoveInRange(losRangeStart, losRangeEnd);  // 删除 LOS 旧空闲块
for (gap in sortedGaps) {
    unifiedFreeMgr.AddReleaseUnits(gapIdx, gapCount);      // 插入新间隙
}
```

### 5.6 ShrinkInactiveZone 修改

```cpp
// Before:
freeRegionManager.RemoveAboveIndex(thresholdUnitIdx);
inactiveZone = newInactiveZone;
// Phase B: ReclaimMainBorrowed(...)

// After:
inactiveZone = newInactiveZone;  // 就这一行！
// 高于 newInactiveZone 的空闲块仍在树中，但 MAIN TakeRegion 的 upperBound 约束会跳过它们
// Phase B 也不需要了 — inactiveZone 直接收缩到 mainEnd 以下就完成了归还
```

---

## 6. 风险与注意事项

### 6.1 树的节点容量

当前两棵独立树各自分配了 `nodeCount = memoryBlockCount/2 + 1` 个节点的空间。统一后需要一次性分配覆盖整个堆的节点空间。

```
当前：MAIN tree ~4K nodes, LOS tree ~1K nodes
统一：~5K nodes (总量不变，只是合并)
```

影响不大。

### 6.2 高地址优先分配从哪端截取

LOS 高地址优先分配时，应从空闲块的**高端**取出 unit。现有 CartesianTree 只支持从低端取（`UpdateNode(idx + num, count - num)`）。

高端取需要：`UpdateNode(idx, count - num)` + 返回 `idx + count - num` 作为分配的起始地址。

```cpp
// 高地址优先：从块的高端分配
Index allocStart = node->GetIndex() + node->GetCount() - num;
node->UpdateNode(node->GetIndex(), node->GetCount() - num, refreshRegionInfo, baseIndex);
// node 的 index 不变，count 减少 → 堆性质可能被破坏 → LowerNonZeroNode
```

这里 index 不变、count 减少的情况下，BST 性质保持不变（index 没变），但 max-heap 性质可能违反（count 减小）。可以用现有的 `LowerNonZeroNode` 修复。✅ 可行。

### 6.3 跨界块的截取

场景：空闲块 `[90, 20)`（unit 90-109），MAIN upperBound=100，请求 num=5。

- 块从低端取 5 个 unit：分配 `[90, 95)`，块变为 `[95, 15)`
- `95 + 5 = 100 <= upperBound=100` ✓

另一个场景：空闲块 `[90, 20)`，MAIN upperBound=93，请求 num=5。

- `90 + 5 = 95 > 93` ❌ → 可用部分只有 `[90, 93)` = 3 个 unit < 5 → 此块无法满足
- 继续搜索下一个

**结论**：`FindLowestSuitableBounded` 只需检查 `index + num <= upperBound` 即可。不需要截取逻辑——因为低地址优先分配总是从块的低端取，未取完的高端部分留在树中。

### 6.4 与 RegionInfo 元数据的关系

统一树中的 unit index 是全局的。AddGarbageUnits / AddReleaseUnits 已经接收全局 index（当前 losFreeMgr 通过 `idx - baseUnit` 转为 local）。统一后取消 local 转换即可。

---

## 7. 总结

| 方面 | 评估 |
|------|------|
| **整体可行性** | ✅ 可行 |
| **ShrinkInactiveZone 收益** | ✅✅ 最大收益：O(1) 替代 O(log n) 树修剪 + 消除 ghost route 保护复杂度 |
| **CompactLOS 收益** | ⚠️ 中等收益：仍需清理旧 LOS 空闲块，但不需要 ClearTrees 全量重建 |
| **RESERVE 管理收益** | ✅ 大幅简化：watermark 与 inactiveZone 合并 |
| **代码复杂度** | ⚠️ CartesianTree 需新增 3 个方法（~100 行） |
| **锁竞争** | ⚠️ 可接受，LOS 分配频率低 |
| **风险** | 中等：需要仔细验证高地址优先分配的截取逻辑和边界条件 |

**建议实施顺序**：
1. 先实现 `TakeUnitsLowAddrBounded` 和 `TakeUnitsHighAddrBounded`（带地址约束的分配）
2. 新增 `RemoveInRange`（支持 CompactLOS）
3. 合并 freeRegionManager + losFreeMgr 为统一实例
4. 修改 RegionManager/LOSManager 的分配路径
5. 简化 ShrinkInactiveZone（移除 RemoveAboveIndex 调用）
6. 修改 CompactLOS Phase 6（RemoveInRange + 重新插入）
7. 简化 ReserveManager（与 inactiveZone 合并）
