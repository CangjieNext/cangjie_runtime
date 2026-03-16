# M4b 实现记录：MAIN freeRegionManager 碎片整理

**实现日期**：2026年2月19日

---

## 1. 实现目标

M4b 阶段目标：通过低地址优先分配和 inactiveZone 缩回，解决 MAIN 空间碎片化问题，使空闲内存可归还给 RESERVE。

### 问题根因

经过多轮 GC 后，MAIN 活跃区 `[regionHeapStart, inactiveZone)` 中的活跃 region 和空闲 region 交错分布。`ForwardFromSpace` 复制存活小对象时，通过 `TakeRegion` → `CartesianTree::TakeUnitsImpl` 分配 to-region。原有分配策略使用 BFS 遍历取"最后一个满足条件的节点"，不偏好任何地址方向，导致新 to-region 分散在整个活跃区。

### 后果

1. 多单元的连续分配请求失败（即使总空闲空间足够）
2. `inactiveZone` 无法缩回（高地址端有散布的活跃 region）
3. 无法向 RESERVE 归还空间

### 解决方案

- **低地址优先分配**：所有 MAIN 空间分配优先使用最低地址的空闲块
- **每次 GC 后缩回 inactiveZone**：扫描活跃区，找到最高活跃 region 末尾，将 inactiveZone 缩回

---

## 2. 内存布局方向

```
MAIN 活跃区                              MAIN 未激活      RESERVE       LOS
[live][free][live][free][free]  ...  [inactiveZone] ... [mainEnd] ... [losEnd]
regionHeapStart                          ↑ 缩回这里       reserveStart
```

- **活跃 region** → 向低地址（`regionHeapStart`）紧凑
- **空闲空间** → 集中在高地址（靠近 `inactiveZone`）
- 缩小 `inactiveZone` → 高端空间自动可被 RESERVE 吸纳

---

## 3. 修改的文件

### 3.1 CartesianTree.h

**新增公共方法**：

```cpp
bool TakeUnitsLowAddr(Count num, Index& idx, bool refreshRegionInfo = true)
```

**新增私有方法**：

```cpp
Node* FindLowestSuitable(Node* node, Count num) const
bool TakeUnitsLowAddrImpl(Count num, Index& idx, bool refreshRegionInfo)
```

**算法说明**：
- `FindLowestSuitable` 利用 CartesianTree 的双重性质：
  - **BST on index**：左子树 = 更低地址，右子树 = 更高地址
  - **Max-heap on count**：如果 `node->count < num`，其所有后代也不满足
- 递归先搜左子树（更低地址），如果左子树无合适节点，当前节点即为最低地址的合适节点
- 时间复杂度 O(log n)，与原有 BFS 遍历的 TakeUnitsImpl 相当

### 3.2 FreeRegionManager.h

**新增成员**：

```cpp
bool lowAddressFirst = false;  // 是否优先使用最低地址空闲块
```

**新增公共方法**：

```cpp
void SetLowAddressFirst(bool enabled)
```

**修改 `TakeRegion`**：根据 `lowAddressFirst` 标志选择 `TakeUnitsLowAddr` 或 `TakeUnits`。MAIN 的 freeRegionManager 设置 `lowAddressFirst = true`，LOS 的 losFreeMgr 保持默认 `false`。

### 3.3 RegionManager.h

**新增成员**：

```cpp
uintptr_t mainEnd = 0;  // MAIN 空间真实上界（不含 RESERVE/LOS）
```

**新增公共方法**：

```cpp
uintptr_t GetMainEnd() const;
size_t ShrinkInactiveZone();
```

**修改 `GetInactiveUnitCount`**：使用 `mainEnd` 代替 `regionHeapEnd`，修复 MAIN 空间边界计算。

### 3.4 RegionManager.cpp

**Initialize 修改**：
- 存储 `this->mainEnd = mainEnd`（原来是局部变量）
- 调用 `freeRegionManager.SetLowAddressFirst(true)` 启用低地址优先

**TakeRegion 修改**：
- inactiveZone 扩展检查改用 `mainEnd` 而非 `regionHeapEnd`

**新增 `ShrinkInactiveZone()`**：

算法分两个 Phase：

**Phase A（MAIN 自有空间）**：
- 扫描 `[regionHeapStart, inactiveZone)` 找到最高活跃 region 末尾（`highestLiveEnd`）

**Phase B（RESERVE 借用空间）**：
- 扫描 `[mainEnd, mainWm)` 找到最高活跃 borrowed region 末尾（`highestLiveBorrowedEnd`）

**Ghost 路由保护**：
- 遍历 `ghostFromRegionList` 和 `pendingGhostGarbageList`，提取 toRegion1/toRegion2 目标地址
- 如果 ghost route 的目标地址在当前扫描范围内，更新高水位标记，避免将 ghost 目标 region 错误回收

**树修剪**：
- 调用 `freeRegionManager.RemoveAboveIndex(thresholdUnitIdx)` 截断阈值以上的所有空闲块
- 跨越阈值的块被截断（保留阈值以下部分）

**提交**：
- Phase A：更新 `inactiveZone`，purge 垃圾 region，释放 units
- Phase B：调用 `ReclaimMainBorrowed()` 归还借用空间，purge 垃圾 region

### 3.5 WCollector.cpp

**修改 `DoGarbageCollection()`**：
- 在 `ForwardFromSpace()` + `FinalizeLOSCompactionIfNeeded()` 之后，STW 下调用 `ShrinkInactiveZone()`
- 由碎片率门控：仅当 `GetMainFragmentationRatio() > 0.30` 时执行

---

## 4. 关键设计决策

| 决策 | 选择 | 理由 |
|------|------|------|
| 低地址优先的适用范围 | 所有 MAIN 分配（包括 mutator 和 GC） | 持续保持低址址紧凑，每次 GC 都有回收机会 |
| ShrinkInactiveZone 触发时机 | 碎片率 > 30% 时，STW 下执行 | 避免每次 GC 都做昂贵的扫描和树修剪 |
| freeRegionManager 重建方式 | RemoveAboveIndex 树修剪 | O(log n) 截断阈值以上空闲块，跨越阈值的块截断保留 |
| LOS 分配策略 | 不变（使用原有 TakeUnits） | LOS 需要不同的分配方向 |
| mainEnd 存储 | RegionManager 新增成员 | 修复原有 bug：inactiveZone 不应超过 MAIN 边界 |

---

## 5. Pinned Region 交互

- Pinned region（`rawPointerPinnedRegionList`、`oldPinnedRegionList`）不可移动
- 如果高地址存在 pinned region，`ShrinkInactiveZone` 自动停在其上方
- 低地址优先分配会自动绕过 pinned region（它们不在 freeRegionManager 中）
- 极端情况：pinned region 在最高地址 → 无法缩回 inactiveZone → 退化为只做碎片整理

---

## 6. 大对象 Region 对齐

### 6.1 问题

`AllocLargeInMain()` 分配大对象时，原始 `regionCount` 由对象大小向上取整得出：

```cpp
size_t regionCount = (size + RegionInfo::UNIT_SIZE - 1) / RegionInfo::UNIT_SIZE;
```

这可能产生非标准大小的 region（如 3 个 unit、5 个 unit），释放后进入 `freeRegionManager` 形成不规则空闲块。这些不规则空闲块难以被后续标准大小（`maxUnitCountPerRegion`）的分配请求复用，导致碎片化累积。

### 6.2 解决方案

在 `AllocLargeInMain()` 中将 `regionCount` 向上对齐到 `maxUnitCountPerRegion` 的整数倍：

```cpp
// M4b: Round up to nearest multiple of maxUnitCountPerRegion (standard region size).
// This ensures that when the large object is freed, the released units form
// whole standard regions, preventing irregular fragmentation in freeRegionManager.
regionCount = ((regionCount + maxUnitCountPerRegion - 1) / maxUnitCountPerRegion) * maxUnitCountPerRegion;
```

### 6.3 效果

| 对齐前 | 对齐后 | 说明 |
|--------|--------|------|
| 需要 3 个 unit | 分配 `maxUnitCountPerRegion` 个 unit | 内部碎片可接受（大对象本身就大，尾部浪费占比低） |
| 需要 17 个 unit | 分配 `2 × maxUnitCountPerRegion` 个 unit | 释放后形成 2 个完整标准 region |
| 需要恰好 `maxUnitCountPerRegion` | 不变 | 已对齐，无额外开销 |

- **释放后可复用**：整对齐的空闲块可被 `TakeRegion` 直接复用为标准 thread-local region
- **利于低地址紧凑**：整对齐的空闲块更容易参与 `ShrinkInactiveZone` 回收
- **LOS 不受影响**：此对齐仅应用于 `AllocLargeInMain`（MAIN 空间降级路径），LOS 空间的 `AllocLarge` 使用 losFreeMgr 自身的分配策略

### 6.4 代码位置

[RegionManager.h](../src/Heap/Allocator/RegionManager.h) `AllocLargeInMain()` 方法，对齐逻辑紧跟在 `regionCount` 初始化之后。

---

## 7. 构建验证

```bash
python3 build.py build --target native --build-type release -v 0.0.1
# [100%] Built target cangjie-runtime-static
# Build and preinstall completed successfully.
```
