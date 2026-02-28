# LOS 待优化点清单

> **日期**: 2026-02-26
> **基于**: 第一轮优化完成后的代码状态 (A/C 比 512MB=106%, 128MB=100%)

---

## 目录

- [1. 优化现状总览](#1-优化现状总览)
- [2. P1: losFreeMgr LIFO 快速缓存](#2-p1-losfreemgr-lifo-快速缓存)
- [3. P1: CompactLOS 增量限制](#3-p1-compactlos-增量限制)
- [4. P2: 恢复 MigrateLargeObjectsToLOS](#4-p2-恢复-migratelargeobjectstolos)
- [5. P2: 恢复 ShrinkInactiveZone + lowAddressFirst](#5-p2-恢复-shrinkinactivezone--lowaddressfirst)
- [6. P3: EnlistLargeRegion GC 阶段缓存](#6-p3-enlistlargeregion-gc-阶段缓存)
- [7. P3: macOS ReleaseUnits 冗余 memset](#7-p3-macos-releaseunits-冗余-memset)
- [8. P4: Card Table / Remembered Set](#8-p4-card-table--remembered-set)
- [9. P4: 并发压缩](#9-p4-并发压缩)
- [10. 代码质量改进](#10-代码质量改进)

---

## 1. 优化现状总览

### 已完成优化 (2026-02-25)

| 编号 | 优化点 | 状态 |
|------|--------|------|
| #1 | 移除双重 atomic 死代码 | ✅ |
| #2 | ClearUnits 延迟清零 (releasedTree) | ✅ |
| #3A | 引用扫描范围快速跳过 | ✅ |
| #6 | 移除 getenv 守卫 | ✅ |
| #8 | CompactLOS 容器预分配 | ✅ |
| #9 | CollectLOSGarbage 内联数组 | ✅ |
| #10A | 延迟 RESERVE 归还 (25% buffer) | ✅ |
| #10D | 按需跳过 CompactLOS | ✅ (-57%) |

### 待实施优化

| 优先级 | 优化点 | 预期收益 | 难度 | 风险 |
|--------|--------|---------|------|------|
| **P1** | losFreeMgr LIFO 快速缓存 | 中 | 中 | 低 |
| **P1** | CompactLOS 增量限制 | 中 | 低 | 低 |
| **P2** | 恢复 MigrateLargeObjectsToLOS (STW) | 中 | 中 | 中 |
| **P2** | 恢复 ShrinkInactiveZone + lowAddressFirst | 中 | 中 | 中 |
| **P3** | EnlistLargeRegion GC 阶段缓存 | 低-中 | 低 | 低 |
| **P3** | macOS ReleaseUnits 冗余 memset | 低 | 低 | 低 |
| **P4** | Card Table / Remembered Set | **高** | **高** | 中 |
| **P4** | 并发压缩 (无 STW) | **高** | **极高** | 高 |

### 性能基线 (A/C 比, C = FLAT 基线)

| 堆大小 | 当前 A/C 比 | 目标 |
|--------|-------------|------|
| 512MB | 106% | 保持 ≥100% |
| 128MB | 100% | 保持 ≥100% |

## 2. P1: losFreeMgr LIFO 快速缓存

### 问题

LOS 分配通过 `losFreeMgr.TakeRegion()` 查找回收的 region，内部使用 CartesianTree (O(log n)) + `try_lock`。典型场景中同一大小的对象反复分配/回收，tree 查找是浪费的。

### 当前开销

```
losFreeMgr.TakeRegion:  try_lock (~10-50ns) + CartesianTree O(log n) (~50-500ns)
```

### 优化方案

在 losFreeMgr 前增加单尺寸 LIFO 快速缓存（链表/固定数组）：

```cpp
// LOSManager 新增
static constexpr size_t LOS_REGION_CACHE_SIZE = 8;
struct CachedRegion {
    UnitIndex unitIdx;
    UnitCount unitCount;
};
std::atomic<size_t> cacheCount{0};
CachedRegion regionCache[LOS_REGION_CACHE_SIZE];

// 分配快速路径: CAS 取出，无需 try_lock
RegionInfo* TryAllocFromCache(size_t unitCount);

// 回收时优先放入缓存，满则回退到 AddGarbageUnits
void ReturnToCache(UnitIndex idx, UnitCount count);
```

### 收益

- 消除热路径的 try_lock + tree 遍历，~50-500ns → ~10ns
- 对 Bench 2 (循环分配/回收) 改善最大
- cache miss 回退到原有路径，零风险

### 实施步骤

1. LOSManager.h 新增 `regionCache` 数据结构 + `TryAllocFromCache`/`ReturnToCache` 接口
2. `AllocLarge()` Step 1 前插入缓存查寻
3. `CollectLOSGarbage` 回收时优先 `ReturnToCache`
4. 性能测试对比

## 3. P1: CompactLOS 增量限制

### 问题

当前 CompactLOS 为全量压缩，一次可能搬移数十个 region + 重建整个 FreeRegionManager，导致 STW 暂停时间不可预测。memmove 数 MB 数据可能耗时数毫秒。

### 优化方案

**方案 A: 增量压缩（每次最多搬 N 个 region）**

```cpp
static constexpr size_t MAX_COMPACT_REGIONS_PER_GC = 4;

// CompactLOS 中限制 compactPlan 大小
if (compactPlan.size() >= MAX_COMPACT_REGIONS_PER_GC) {
    break;  // 剩余留到下次 GC
}
```

每次 GC 只搬 4 个 region（约 512KB），STW 中 memmove 时间可控。

**方案 B: 启发式提前压缩**

不等 OOM，在常规 GC（`GC_REASON_HEU`）中检测碎片率 > 50% 时触发小规模压缩。

### 收益

- STW 暂停时间可预测、可控
- 多次小批量替代一次大批量，平滑延迟
- 方案 B 可减少 OOM GC 触发频率

### 实施步骤

1. `BuildCompactionPlan()` 中加 `MAX_COMPACT_REGIONS_PER_GC` 捕半
2. 未完成的 entry 留在 `pendingCompactionPlan` 下次继续
3. 可选：在 HEU GC 后检查碎片率，标记 `RequestIncrementalCompact()`
4. 性能测试验证 STW 曲线平坦度

## 4. P2: 恢复 MigrateLargeObjectsToLOS

### 背景

当前 `MigrateLargeObjectsToLOS()` 入口处 `return;`，大对象始终留在 MAIN 空间，无法享受 LOS 独立管理。

禁用原因：在 PostTrace 中执行时 mutator 并发运行，复制对象到 LOS 后更新引用时，mutator 可能读取旧引用导致 SIGSEGV。

### 恢复方案

用 `ScopedStopTheWorld` 包裹迁移 + 引用更新：

```cpp
void WCollector::MigrateLargeObjectsToLOS() {
    ScopedStopTheWorld stw("migrate-large-to-LOS", false);
    BuildLargeObjectMigrationRoutes();  // 分配 LOS 目标 + 设置 ghost route
    // memmove 对象到 LOS
    // UpdateMigratedReferences() 扫描全堆更新引用
}
```

### 风险

- STW 更新引用扫描全堆 O(heapSize)，增加暂停时间
- 与 CompactLOS 共享 `migrationMap` 和 `UpdateMigratedReferences`，建议一起测试
- 可复用 #3A 的范围快速跳过优化减少引用扫描开销

### 实施步骤

1. 移除 `MigrateLargeObjectsToLOS` 入口的 `return;`
2. 包裹 `ScopedStopTheWorld`
3. 确保 `BuildLargeObjectMigrationRoutes` + memmove + `UpdateMigratedReferences` 在 STW 内完成
4. 功能测试：128MB LOS ON，验证大对象确实迁移到 LOS
5. 性能测试：对比启用迁移前后的 STW 暂停时间

## 5. P2: 恢复 ShrinkInactiveZone + lowAddressFirst

### 背景

ShrinkInactiveZone 在 OOM GC 后收缩 MAIN 的高地址空闲区域，配合 lowAddressFirst 将分配集中在低地址，使高地址可安全收缩。

禁用原因：ClearTrees + 重新扫描与 mutator 的 `TakeRegion` → `AddGarbageUnits` 竞态，导致 CartesianTree 重复插入。

### 恢复方案

```cpp
// WCollector.cpp
if (gcReason == GC_REASON_OOM) {
    ScopedStopTheWorld stw("shrink inactive", false);
    rm.ShrinkInactiveZone();
}
```

同时恢复：
```cpp
// RegionManager.cpp
freeRegionManager.SetLowAddressFirst(true);  // 低地址优先分配
```

### 并发安全

STW 内执行，mutator 全部暂停，无竞态。支付是额外的 STW 暂停时间（线性扫描 + tree 重建）。

### 收益

- OOM 后归还未使用的高地址空间给 RESERVE
- lowAddressFirst 减少 MAIN 空间碎片
- 与 RESERVE 借还机制协同，改善小堆下的空间利用率

### 实施步骤

1. 取消 ShrinkInactiveZone 调用的注释，包裹 `ScopedStopTheWorld`
2. 恢复 `SetLowAddressFirst(true)`
3. 功能测试：128MB LOS ON，多轮分配/回收后验证 inactiveZone 收缩
4. 性能测试：对比 STW 暂停时间和分配吐吐量

## 6. P3: EnlistLargeRegion GC 阶段缓存

### 问题

每次 LOS 分配成功后，`EnlistLargeRegion` 需调用：

```cpp
GCPhase phase = Heap::GetHeap().GetCollector().GetGCPhase();  // 跨模块间接调用链
```

在 Bench 5 混合场景（1150 对象/秒）中累积开销被放大。

### 优化方案

将 GC 阶段缓存到 LOSManager 成员变量，由 GC 线程在阶段切换时更新：

```cpp
// GC 阶段切换时
void LOSManager::NotifyGCPhaseChange(GCPhase phase) {
    cachedGCPhase.store(phase, std::memory_order_release);
}

// EnlistLargeRegion 使用缓存
GCPhase phase = cachedGCPhase.load(std::memory_order_relaxed);
```

### 收益

- 减少跨模块函数调用链
- relaxed 语义安全：最坏情况 region 放入错误 list，但原始代码已有此竞态窗口
- 影响：低-中

## 7. P3: macOS ReleaseUnits 冗余 memset

### 问题

macOS 的 `ReleaseUnits` 做了 **memset + mmap(MAP_FIXED)** 双重操作：

```cpp
// RegionInfo.h — macOS 路径
MemorySet(unitAddress, size, 0, size);  // memset 仍在做!
mmap(unitAddress, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
```

`mmap MAP_FIXED` 本身就映射零页面，前面的 memset 是冗余的。

### 优化

移除 macOS 路径中 `ReleaseUnits` 的 `MemorySet` 调用：

```cpp
// macOS 优化版 ReleaseUnits
// MemorySet(unitAddress, size, 0, size);  // 移除
void* ret = mmap(unitAddress, size, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
```

### 收益

- macOS 上消除每次 LOS 回收的冗余 memset
- 影响：低（回收是冷路径，且 mmap 系统调用本身开销更大）

## 8. P4: Card Table / Remembered Set

### 问题

`UpdateCompactedLOSReferences` 是 CompactLOS 后的引用更新，扫描全堆所有对象的所有引用字段：

```复杂度: O(heapSize) — 即使只移动了 1 个 LOS 对象
```

当前已有 #3A 范围跳过优化（>99% 引用跳过 hash 查找），但仍需遍历每个对象的每个引用字段。

### 优化方案

维护“谁引用了 LOS 对象”的记录：

**方案 B: Remembered Set**
- 写屏障中检测引用目标是否在 LOS，是则加入 remembered set
- CompactLOS 后只扫描 remembered set 条目
- 复杂度: O(refCount_to_LOS)

**方案 C: Card Table**
- 将堆分为固定大小的卡（如 512B），写屏障标记脏卡
- CompactLOS 后只扫描脏卡内的引用
- 复杂度: O(dirty_cards)

### 实施难度

- **高**: 需在写屏障中增加 LOS 地址判断，影响每次引用写入的开销
- 需要与现有 GC 屏障机制集成
- 其他 GC 操作也可潜在受益（如任何需要扫描引用的场景）

### 收益

- 将引用更新从 O(heapSize) 降为 O(refCount_to_LOS)
- 是最大的**结构性瓶颈**，但当前 #10D 跳过机制已大幅降低 CompactLOS 触发频率
- 留作长期规划

## 9. P4: 并发压缩

### 问题

当前 CompactLOS 必须在 STW 内执行，memmove + 引用更新会导致所有 mutator 暂停。

### 优化方案

将 CompactLOS 移出 STW，使用写屏障 + forwarding pointer 实现并发搬移：

1. 异步线程执行 memmove
2. 设置 forwarding pointer 指向新地址
3. 写屏障检查引用目标是否已搬移，透明转发
4. 所有引用更新完成后清理旧对象

### 实施难度

**极高**：需要大量架构改动，与现有 ForwardBarrier / GC 阶段深度集成。

### 收益

- 完全消除 CompactLOS 的 STW 暂停
- 留作远期规划

## 10. 代码质量改进

以下不影响运行时性能，但提高可维护性：

| 改进点 | 位置 | 说明 |
|--------|------|------|
| Ghost routing 保护重复代码 | `RegionManager::ShrinkInactiveZone` | `ghostFromRegionList` 和 `pendingGhostGarbageList` 有完全重复的 ~30 行 lambda，应提取为公共辅助函数 |
| BuildLargeObjectMigrationRoutes 堆分配 | `WCollector.cpp` | `std::vector<RegionInfo*> candidates` 在 PostTrace 触发堆分配，可用栈上 `SmallVector<RegionInfo*, 16>` 替代 |
| LOSManager 头文件 include | `LOSManager.h` | 仍 include `<unordered_set>` 和 `<unordered_map>`，实际使用已经减少，可清理 |

---

## 建议实施路线

```
第 2 阶段 (核心改进, 3-5 天):
  ├─ P1: losFreeMgr LIFO 缓存 (§2)
  ├─ P1: CompactLOS 增量限制 (§3)
  └─ P2: 恢复 ShrinkInactiveZone + lowAddressFirst (§5)

第 3 阶段 (细化优化, 2-3 天):
  ├─ P2: 恢复 MigrateLargeObjectsToLOS (§4)
  ├─ P3: EnlistLargeRegion GC 阶段缓存 (§6)
  └─ P3: macOS ReleaseUnits 冗余 memset (§7)

第 4 阶段 (架构级, 未来规划):
  ├─ P4: Card Table / Remembered Set (§8)
  └─ P4: 并发压缩 (§9)
```
