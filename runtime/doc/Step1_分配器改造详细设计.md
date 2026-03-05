# Step 1：分配器改造详细设计 —— 从 Bump-Pointer 到 Free-List 混合模式

> 本文档是 [Go 风格 GC 迁移方案](Go风格GC迁移方案.md) Step 1 的独立详细设计文档。
> 目标：在保留 Region 框架的前提下，改造分配器以支持 Mark-Sweep 模式下对象的原地回收与空间复用。

---

## 目录

1. [现有分配器架构深度分析](#1-现有分配器架构深度分析)
2. [问题分析：为什么需要改造](#2-问题分析为什么需要改造)
3. [Size Class 设计](#3-size-class-设计)
4. [Region 内部 Free-List 设计](#4-region-内部-free-list-设计)
5. [AllocBufferMS —— 线程本地分配缓存](#5-allocbufferms--线程本地分配缓存)
6. [CentralRegionPool —— 全局 Region 池](#6-centralregionpool--全局-region-池)
7. [分配路径改造：完整调用链](#7-分配路径改造完整调用链)
8. [Sweep 集成点设计](#8-sweep-集成点设计)
9. [代码修改清单](#9-代码修改清单)
10. [验证方案](#10-验证方案)
11. [大对象管理改造设计](#11-大对象管理改造设计)
12. [动态堆增长与收缩设计](#12-动态堆增长与收缩设计)

---

## 1. 现有分配器架构深度分析

### 1.1 整体类层次

```
Allocator (抽象接口)
  └── RegionSpace (唯一实现)
        ├── RegionManager    - Region 生命周期管理（15+ 个 RegionList）
        ├── FreeRegionManager - 空闲 Unit 树（CartesianTree）
        ├── RegionInfo       - 单个 Region 的元数据 + bump-pointer 分配
        └── AllocBuffer      - 线程本地分配缓存（每线程一个 tlRegion）
```

核心源文件：

| 文件 | 职责 |
|------|------|
| `src/Heap/Allocator/RegionSpace.h/cpp` | 分配器入口，协调 RegionManager 与 AllocBuffer |
| `src/Heap/Allocator/RegionInfo.h` | Region 元数据：`allocPtr` bump-pointer、bitmap、RouteState FSM |
| `src/Heap/Allocator/AllocBuffer.h` | 线程本地：`tlRegion`、`preparedRegion`、raw-pointer regions |
| `src/Heap/Allocator/RegionManager.h` | 所有 RegionList 的增删查改、TakeRegion 三级查找 |
| `src/Heap/Allocator/FreeRegionManager.h` | 笛卡尔树管理 dirty/released Unit |
| `src/Heap/Allocator/RegionList.h` | 双向链表 + mutex 的基础数据结构 |

### 1.2 内存层次模型

```
             HeapSpace (虚拟地址预留区)
                │
     ┌──────────┼──────────┐
     │ Unit 0   │ Unit 1   │ Unit 2   │ Unit 3   │ ...
     │ (16KB)*  │ (16KB)*  │ (16KB)*  │ (16KB)*  │
     └──────────┴──────────┴──────────┴──────────┘
     │←────── Region (N Units) ──────→│

     * macOS: UNIT_SIZE = 16KB (PAGE_SIZE)
     * Linux: UNIT_SIZE = 4KB  (PAGE_SIZE)
     * 默认 Region = 128KB = 32 Units (Linux) 或 8 Units (macOS)
```

- **Unit**：最小物理内存管理单位，等于操作系统页大小
- **Region**：由连续 N 个 Unit 组成，是 GC 管理的基本单位
- **RegionInfo**：每个 Unit 有一个 `UnitInfo`，Region 首 Unit 的 `UnitInfo` 即 `RegionInfo`

### 1.3 RegionInfo 核心结构（`UnitMetadata`）

```cpp
// src/Heap/Allocator/RegionInfo.h:990
struct UnitMetadata {
    // ---- 快速分配区（位于结构体最前端） ----
    uintptr_t allocPtr;             // bump-pointer 当前位置
    uintptr_t regionEnd;            // Region 尾地址（allocPtr <= regionEnd）

    // ---- 链表索引 ----
    uint32_t  nextRegionIdx;
    uint32_t  prevRegionIdx;

    // ---- GC 统计 ----
    uint32_t  liveByteCount;        // 本轮 GC 中存活字节数
    int32_t   rawPointerObjectCount;

    // ---- GC 元信息 ----
    LiveInfo* liveInfo;             // 懒分配：markBitmap + liveBitmap
    uintptr_t regionEnd0;           // Ghost Region 备份
    RouteInfo routeInfo;            // Forward 路由信息

    // ---- 标志位 ----
    uint8_t unitRole  : 4;         // SMALL_SIZED_UNITS / LARGE_SIZED_UNITS
    uint8_t regionType: 4;         // FREE_REGION / THREAD_LOCAL_REGION / FULL_PINNED_REGION / ...
    uint8_t isTraceRegion: 1;
    uint8_t inGhostFromRegion: 1;
    // ...
};
```

**关键观察**：`allocPtr` 位于结构体偏移 0 处，是热路径上最频繁访问的字段。

### 1.4 Bump-Pointer 分配——当前快速路径

```cpp
// RegionInfo::Alloc（非原子版，线程本地 Region 使用）
uintptr_t Alloc(size_t size) {
    size_t limit = GetRegionEnd();
    if (metadata.allocPtr + size <= limit) {
        uintptr_t addr = metadata.allocPtr;
        metadata.allocPtr += size;     // 指针单调递增
        return addr;
    }
    return 0;  // Region 空间不足
}
```

这是经典的 bump-pointer 分配：**O(1)** 时间、无碎片、无锁（线程本地）。但**不支持 Region 内部空间回收**——一旦分配，中间释放的对象空间无法复用，只能整个 Region 在 Copy GC 后整体回收。

### 1.5 分配入口完整调用链

```
用户代码 new → CompilerCalls → RegionSpace::Allocate(size, allocType)
  │
  ├── ToAllocSize(size): objSize + HEADER_SIZE(0), 8-byte alignment
  │
  └── TryAllocateOnce(totalSize, allocType):
        │
        ├── allocType == PINNED_OBJECT?
        │     └── RegionManager::AllocPinned(totalSize)
        │           ├── 尝试 recentPinnedRegionList 头部空闲 slot
        │           ├── 遍历 freePinnedSlotLists 找合适 slot
        │           └── 新建 Region → 加入 recentPinnedRegionList
        │
        ├── totalSize >= largeObjectThreshold?
        │     └── RegionManager::AllocLarge(totalSize)
        │           ├── 计算需要的 Unit 数：ceil(size / UNIT_SIZE)
        │           ├── FreeRegionManager::TakeRegion(nUnit)
        │           └── 加入 recentLargeRegionList
        │
        └── MOVEABLE_OBJECT (小对象，最常见路径):
              └── AllocBuffer::Allocate(totalSize, allocType)
                    │
                    ├── 快速路径: tlRegion->Alloc(totalSize)  ← 上面的 bump-pointer
                    │     └── 成功 → 返回地址
                    │
                    └── 慢速路径: AllocateImpl(totalSize, allocType)
                          ├── tlRegion 已满 → EnlistFull(tlRegion)
                          ├── 尝试 preparedRegion → CAS 交换为 tlRegion
                          ├── TakeRegion → 从 garbageRegionList/FreeRegionManager/InactiveZone 获取
                          └── 分配成功 → 返回
```

### 1.6 Region 获取三级查找（`RegionManager::TakeRegion`）

```
TakeRegion(nUnit = 1):
  │
  ├── 第 1 级: garbageRegionList.TakeHeadRegion()
  │     └── 上轮 GC 标记为垃圾的 Region（已清理，可直接复用）
  │
  ├── 第 2 级: FreeRegionManager::TakeRegion(nUnit)
  │     ├── dirtyUnitTree（有内容但未释放的空闲 Unit，需 ClearUnits)
  │     └── releasedUnitTree（已 madvise 释放的 Unit，需 recommit）
  │
  └── 第 3 级: InactiveZone 扩展
        └── 从预留虚拟地址空间中 commit 新页
```

### 1.7 AllocBuffer —— 线程本地缓存

```cpp
class AllocBuffer {
    RegionInfo*             tlRegion;      // 当前活跃 Region（快速路径直接使用）
    std::atomic<RegionInfo*> preparedRegion;// 预备 Region（异步预分配）
    RegionList              tlRawPointerRegions;      // raw-pointer 对象
    RegionList              tlLargeRawPointerRegions;  // 大 raw-pointer 对象
    std::list<BaseObject*>  stackRoots;    // 并发枚举期间暂存根对象
};
```

**编译器约束**：`static_assert(offsetof(AllocBuffer, tlRegion) == 0)` —— 编译器生成的分配代码**硬编码**假设 `tlRegion` 在 `AllocBuffer` 偏移 0 处，JIT 直接通过 TLS 指针 + 0 偏移读取 `tlRegion`，再通过 `tlRegion + 0` 偏移读取 `allocPtr` 实现最快分配。

### 1.8 RegionList 管理总览

RegionManager 维护了 15+ 个 RegionList，各有不同生命周期意义：

| RegionList | 用途 | 生命周期 |
|------------|------|----------|
| `tlRegionList` | 当前正被线程使用的 Region | 分配中 → 满后 enlist |
| `recentFullRegionList` | 本轮刚填满的 Region | 满 → GC 开始时转 fromList |
| `fromRegionList` | 本轮 GC 的 from-space | GC 标记+转发 → 回收 |
| `garbageRegionList` | 整体垃圾 Region（无存活对象）| GC 后 → 被 TakeRegion 复用 |
| `recentPinnedRegionList` | Pinned 对象 Region | 分配中 → GC 转 oldPinned |
| `oldPinnedRegionList` | 历史 Pinned Region | 累积 |
| `recentLargeRegionList` | 本轮新分配的大对象 Region | 类似 recentFull |
| `oldLargeRegionList` | 历史大对象 Region | 累积 |
| `rawPointerPinnedRegionList` | raw-pointer pinned Region | 特殊管理 |
| `unmovableFromRegionList` | 不可移动的 from Region | Copy GC 特有 |

---

## 2. 问题分析：为什么需要改造

### 2.1 Copy GC 分配器的核心假设

当前分配器建立在 **Copy GC** 的核心假设之上：

1. **对象可移动**：存活对象在 Forward 阶段被复制到新 Region，旧 Region 整体释放
2. **Region 整体回收**：Region 要么全保留，要么全回收——不存在"Region 内部分空闲"
3. **Bump-pointer 足够**：因为不需要 Region 内部复用，线性分配是最优策略
4. **无需 sweep**：Copy GC 天然不需要 sweep 阶段——活对象已搬走，死对象的 Region 直接释放

### 2.2 Mark-Sweep 模式下的矛盾

在 Mark-Sweep GC 中，对象**不移动**。GC 标记完成后：

```
Region (128KB):
  [obj1:live][obj2:dead][obj3:live][obj4:dead][obj5:dead][obj6:live]...
              ↑ 空洞      ↑ 空洞          ↑ 空洞
```

如果继续使用 bump-pointer：
- **allocPtr 只能向前**：已分配区域中的空洞无法复用
- **内存利用率急剧下降**：每轮 GC 后大量 Region 处于"半满"状态，既不能回收也不能继续分配
- **碎片化**：不同大小对象混合在同一 Region 中，sweep 后的空洞大小不一

### 2.3 具体影响估算

假设平均对象存活率 50%，Region 大小 128KB：

| 场景 | Copy GC | Mark-Sweep (无改造) | Mark-Sweep (改造后) |
|------|---------|--------------------|--------------------|
| Region 满后可用空间 | 0（整体释放） | ~64KB 空洞不可用 | ~64KB 通过 free-list 复用 |
| 内存利用率 | 约 50%（需要 to-space）| 约 50%（空洞浪费）| 约 85-95%（free-list 复用）|
| 分配吞吐 | 极高（纯 bump-pointer）| 极高（但很快 OOM）| 高（free-list 快速路径 + bump-pointer 兜底）|

### 2.4 改造目标

1. **Region 内部空间复用**：GC sweep 后，将 Region 内的空闲空间组织为 free-list，供后续分配复用
2. **保留 Region 框架**：不改变 Region/Unit/RegionManager 的整体架构，最小化改动范围
3. **保留快速路径**：新 Region 第一次分配仍使用 bump-pointer（最快），free-list 仅用于 sweep 后复用
4. **兼容编译器假设**：保持 `AllocBuffer::tlRegion` 在偏移 0 处、`RegionInfo::allocPtr` 在偏移 0 处的约束
5. **编译期可切换**：通过 `MRT_USE_MARK_SWEEP` 宏控制，与现有 Copy GC 代码共存

---

## 3. Size Class 设计

### 3.1 为什么需要 Size Class

Mark-Sweep 后 Region 内部有大小不一的空洞。如果不按大小分类：
- 一个 24B 的空洞无法容纳 32B 的对象 → **外部碎片**
- 用 32B 的空洞存放 8B 的对象 → **内部碎片**
- 遍历 free-list 找合适大小 → **分配变慢**

Go 的解决方案：**每个 span（对应我们的 Region）只服务一种 size class**。这样：
- 同一 Region 内所有 slot 大小相同 → 碎片可控
- 分配时不需搜索匹配大小 → O(1) 取链表头即可

### 3.2 Size Class 表设计

参考 Go 的 67 级 size-class，结合 Cangjie 对象特点（最小 8B 对齐），设计 32 级：

```cpp
// 新增文件: src/Heap/Allocator/SizeClass.h

#ifndef MRT_SIZE_CLASS_H
#define MRT_SIZE_CLASS_H

#include <cstddef>
#include <cstdint>

namespace MapleRuntime {

static constexpr size_t kNumSizeClasses = 32;

// Size Class 表：索引 → 对象大小上限（字节）
// 设计原则：
//   - 每级之间增长约 12.5~25%，平衡碎片与级数
//   - 8B 对齐
//   - 最大 class = 6144B，超过走 Large 路径
static constexpr size_t kSizeClasses[kNumSizeClasses] = {
    //  小对象（8B ~ 128B），步进 8~16B
    8,    16,   24,   32,   48,   64,   80,   96,
    //  中小对象（112B ~ 512B），步进 16~128B
    112,  128,  160,  192,  224,  256,  320,  384,
    //  中等对象（448B ~ 2048B），步进 64~256B
    448,  512,  640,  768,  896,  1024, 1280, 1536,
    //  中大对象（1792B ~ 6144B），步进 256~1024B
    1792, 2048, 2560, 3072, 3584, 4096, 5120, 6144,
};

// 大对象阈值：超过此值走 AllocLarge 路径
// 保持与现有 RegionManager::largeObjectThreshold 兼容
static constexpr size_t kLargeObjectThreshold = 6144;

// Size → Class 快速查找表（空间换时间）
// 对于 size <= 1024，直接用 size/8 做索引（最多 128 项）
// 对于 1024 < size <= 6144，用 (size-1024)/256 + 128 做索引
static constexpr size_t kMaxLookupSize = 6144;
static constexpr size_t kSmallLookupBound = 1024;
static constexpr size_t kSmallGranularity = 8;
static constexpr size_t kLargeGranularity = 256;

class SizeClassInfo {
public:
    static void Init();

    // O(1) 查表：对象大小 → size class 索引
    static inline uint8_t SizeToClass(size_t size) {
        if (LIKELY(size <= kSmallLookupBound)) {
            return smallLookup_[(size + kSmallGranularity - 1) / kSmallGranularity];
        }
        if (LIKELY(size <= kMaxLookupSize)) {
            return largeLookup_[(size - kSmallLookupBound - 1) / kLargeGranularity];
        }
        return kNumSizeClasses; // 超大对象，走 Large 路径
    }

    // size class 索引 → 实际分配大小
    static inline size_t ClassToSize(uint8_t cls) {
        return kSizeClasses[cls];
    }

    // 单个 Region 可容纳的 slot 数量
    static inline size_t SlotsPerRegion(uint8_t cls) {
        return slotsPerRegion_[cls];
    }

private:
    // 查找表（在 Init() 中填充）
    static uint8_t smallLookup_[kSmallLookupBound / kSmallGranularity + 1];
    static uint8_t largeLookup_[(kMaxLookupSize - kSmallLookupBound) / kLargeGranularity + 1];
    static size_t  slotsPerRegion_[kNumSizeClasses];
};

} // namespace MapleRuntime
#endif // MRT_SIZE_CLASS_H
```

### 3.3 内部碎片分析

| Size Class | 大小 (B) | 前一级 | 内部碎片上限 |
|:----------:|:--------:|:------:|:----------:|
| 0 | 8 | - | 0% |
| 1 | 16 | 8 | 50% |
| 2 | 24 | 16 | 33% |
| 3 | 32 | 24 | 25% |
| 4 | 48 | 32 | 33% |
| 5 | 64 | 48 | 25% |
| ... | ... | ... | ... |
| 15 | 384 | 320 | 16.7% |
| 23 | 1536 | 1280 | 16.7% |
| 31 | 6144 | 5120 | 16.7% |

**整体加权平均内部碎片约 12~15%**，与 Go 接近。

### 3.4 每 Region slot 数量

以 macOS (UNIT_SIZE=16KB, 默认 Region=128KB) 为例：

| Size Class | 大小 (B) | Slots/Region | Region 利用率 |
|:----------:|:--------:|:------------:|:------------:|
| 0 | 8 | 16384 | 100% |
| 3 | 32 | 4096 | 100% |
| 5 | 64 | 2048 | 100% |
| 9 | 128 | 1024 | 100% |
| 13 | 224 | 585 | 99.9% |
| 15 | 384 | 341 | 99.8% |
| 19 | 768 | 170 | 99.6% |
| 23 | 1536 | 85 | 99.6% |
| 31 | 6144 | 21 | 98.3% |

### 3.5 与 HEADER_SIZE 的关系

当前 `RegionSpace::HEADER_SIZE = 0`（对象无额外头部）。`ToAllocSize` 仅做 8B 对齐：

```cpp
size_t ToAllocSize(size_t objSize) {
    return AlignUp(objSize + HEADER_SIZE, 8);  // HEADER_SIZE = 0
}
```

在 Mark-Sweep 模式下，**free-list 利用死亡对象的前 8 字节存储链表指针**，因此最小分配大小必须 ≥ 8B。当前最小 size class = 8B，满足此约束。如果未来需要添加对象头（如标记位内联到对象头），需要相应调整 HEADER_SIZE 和 size class 表。

---

## 4. Region 内部 Free-List 设计

### 4.1 核心思路

每个 Region 绑定一个 size class 后，Region 内部变成**等大小 slot 的数组**。Sweep 后死亡对象所在 slot 串成 free-list：

```
Region (128KB, sizeClass=3, slotSize=32B):
┌────┬────┬────┬────┬────┬────┬────┬────┐
│obj1│ F  │obj3│ F  │ F  │obj6│obj7│ F  │ ...
│live│free│live│free│free│live│live│free│
└────┴─┬──┴────┴─┬──┴─┬──┴────┴────┴─┬──┘
       │         │    │               │
       └────→────┘    └──────→────────┘
       freeList_ 链表（利用 slot 前 8 字节）
```

### 4.2 FreeSlot 结构

```cpp
// 新增到 src/Heap/Allocator/RegionInfo.h

// 利用死亡对象的前 8 字节作为 free-list 指针
// 要求：最小 slot 大小 ≥ sizeof(FreeSlot) = 8 bytes
struct FreeSlot {
    FreeSlot* next;
};
```

**安全性说明**：

1. **不需要额外内存**：FreeSlot 指针写在已死亡对象的内存空间中，不占用额外空间
2. **不影响 GC 标记**：free-list 构建发生在 sweep 阶段，此时标记已完成
3. **不影响 bitmap**：标记信息存在 `LiveInfo::markBitmap` 中，与对象内容无关
4. **线程安全**：free-list 操作只发生在持有 Region 的线程上（线程本地）或在持有锁的 central 池操作中

### 4.3 RegionInfo 扩展

在 `UnitMetadata` 中新增 Mark-Sweep 相关字段：

```cpp
struct UnitMetadata {
    // ===== 现有字段 (不变) =====
    uintptr_t allocPtr;         // [偏移 0] bump-pointer（保持不变！编译器依赖此偏移）
    uintptr_t regionEnd;        // [偏移 8]
    uint32_t  nextRegionIdx;    // [偏移 16]
    uint32_t  prevRegionIdx;    // [偏移 20]
    uint32_t  liveByteCount;    // [偏移 24]
    int32_t   rawPointerObjectCount; // [偏移 28]

    // ... 现有 liveInfo, regionEnd0, routeInfo 等保持不变 ...

    // ===== 新增字段 (仅 Mark-Sweep 模式) =====
#ifdef MRT_USE_MARK_SWEEP
    FreeSlot* freeListHead;     // sweep 后的空闲 slot 链表头
    uint32_t  freeSlotCount;    // 当前空闲 slot 数量
    uint32_t  totalSlotCount;   // Region 中总 slot 数量
    uint8_t   sizeClass;        // 此 Region 服务的 size class (0~31)
    uint8_t   sweepState;       // 0=未 sweep, 1=已 sweep
    uint16_t  padding_ms;       // 对齐填充
#endif
};
```

**布局约束**：
- `allocPtr` 必须维持在偏移 0 —— 编译器硬编码此偏移
- 新增字段放在已有字段后面，不影响现有字段的偏移
- 使用 `#ifdef` 隔离，Copy GC 模式下 `sizeof(UnitMetadata)` 不变

### 4.4 新增 Region 方法

```cpp
// 在 RegionInfo 类中新增 Mark-Sweep 相关方法

#ifdef MRT_USE_MARK_SWEEP

    // 初始化 Region 为指定 size class
    void InitForSizeClass(uint8_t cls) {
        metadata.sizeClass = cls;
        metadata.freeListHead = nullptr;
        metadata.freeSlotCount = 0;
        size_t slotSize = SizeClassInfo::ClassToSize(cls);
        metadata.totalSlotCount = static_cast<uint32_t>(GetRegionSize() / slotSize);
        metadata.sweepState = 0;
    }

    // 从 free-list 弹出一个 slot（O(1)，线程本地调用，无需同步）
    uintptr_t AllocFromFreeList() {
        FreeSlot* slot = metadata.freeListHead;
        if (LIKELY(slot != nullptr)) {
            metadata.freeListHead = slot->next;
            metadata.freeSlotCount--;
            return reinterpret_cast<uintptr_t>(slot);
        }
        return 0;
    }

    // Sweep 阶段：扫描 Region，将未标记对象加入 free-list
    // 返回本次回收的字节数
    size_t SweepRegion() {
        size_t slotSize = SizeClassInfo::ClassToSize(metadata.sizeClass);
        uintptr_t cursor = GetRegionStart();
        uintptr_t end = metadata.allocPtr;  // 只扫描已分配区域
        size_t freedBytes = 0;

        while (cursor < end) {
            if (!IsMarkedAtAddress(cursor)) {
                // 未标记 → 死亡对象 → 加入 free-list
                FreeSlot* slot = reinterpret_cast<FreeSlot*>(cursor);
                slot->next = metadata.freeListHead;
                metadata.freeListHead = slot;
                metadata.freeSlotCount++;
                freedBytes += slotSize;
            } else {
                // 存活对象 → 清除标记（为下轮 GC 准备）
                ClearMarkAtAddress(cursor);
            }
            cursor += slotSize;
        }

        metadata.sweepState = 1;
        return freedBytes;
    }

    // 判断 Region 是否完全空闲（可整体释放回 FreeRegionManager）
    bool IsCompletelyFree() const {
        return metadata.freeSlotCount == metadata.totalSlotCount;
    }

    // 判断 Region 是否有空闲空间（free-list 或 bump-pointer）
    bool HasFreeSpace() const {
        return metadata.freeSlotCount > 0 ||
               metadata.allocPtr + SizeClassInfo::ClassToSize(metadata.sizeClass) <= GetRegionEnd();
    }

    uint8_t GetSizeClass() const { return metadata.sizeClass; }
    uint32_t GetFreeSlotCount() const { return metadata.freeSlotCount; }

#endif // MRT_USE_MARK_SWEEP
```

### 4.5 分配优先级：Free-List 优先 + Bump-Pointer 兜底

对于绑定了 size class 的 Region，Mark-Sweep 模式下分配顺序：

```
RegionInfo::AllocMS(size_t size):
  │
  ├── 1. AllocFromFreeList() → 非零则返回       ← O(1)，最优先
  │
  ├── 2. Alloc(size) → bump-pointer            ← O(1)，用于 Region 尾部未使用区域
  │     (metadata.allocPtr + size <= regionEnd)
  │
  └── 3. 返回 0 → Region 已满                  ← 上层需获取新 Region
```

**关键细节**：即使 free-list 为空，Region 尾部仍可能有 bump-pointer 空间（首次分配且未经 sweep，或 sweep 后仍有尾部空间）。

### 4.6 标记位操作

当前标记信息存储在 `LiveInfo::markBitmap` 中（懒分配）。在 Mark-Sweep 模式下复用此 bitmap：

```cpp
// 现有方法（不需修改）
bool IsMarkedAtAddress(uintptr_t addr) {
    LiveInfo* info = GetLiveInfo();
    if (info == nullptr) return false;
    size_t offset = addr - GetRegionStart();
    size_t bitIdx = offset / kMinAllocSize;  // 8B 对齐
    return info->markBitmap.Test(bitIdx);
}

void ClearMarkAtAddress(uintptr_t addr) {
    LiveInfo* info = GetLiveInfo();
    if (info == nullptr) return;
    size_t offset = addr - GetRegionStart();
    size_t bitIdx = offset / kMinAllocSize;
    info->markBitmap.Clear(bitIdx);
}
```

**注意**：现有 `MarkObject` 方法使用 `RegionBitmap` 以 bit 位标记对象。在 size-class 模式下，每个 slot 起始地址对齐到 size class 大小，bitmap 索引仍按最小粒度（8B）计算。

### 4.7 与现有 Pinned 机制的关系

当前 `AllocPinned` 已使用了 slot-based 分配（`FreePinnedSlotLists`），其设计思路与本改造类似：

```cpp
// 现有: RegionManager::AllocPinned
MAddress AllocPinned(size_t size) {
    // 1. 尝试从 recentPinnedRegionList 头部 Region 分配
    // 2. 遍历 freePinnedSlotLists 找合适大小的 slot
    // 3. 都失败 → 新建 Region
}
```

**可复用点**：Pinned 对象的 slot-list 管理逻辑可以作为 free-list 的参考。但 Pinned 使用的 slot 大小不固定（动态适配），而 Mark-Sweep 的 free-list 是 **size-class 固定大小**，更简单高效。

在 Mark-Sweep 模式下，**所有对象都不移动**，`PINNED_OBJECT` 和 `MOVEABLE_OBJECT` 的区分不再有实际意义。但为保持 API 兼容性，建议保留 allocType 参数，仅在行为上统一走 Mark-Sweep 路径。

---

## 5. AllocBufferMS —— 线程本地分配缓存

### 5.1 对标 Go 的 mcache

Go 的每个 P（Processor）有一个 `mcache`，其中包含：
- 67 个 `mspan` 指针（每个 size class 一个活跃 span）
- 67 个 `mspan` 指针（noscan 版本，不含指针的对象）

我们的对应设计：`AllocBufferMS`，每个线程（或协程 Processor）一个，包含 32 个当前活跃 Region。

### 5.2 类设计

```cpp
// 新增文件: src/Heap/Allocator/AllocBufferMS.h

#ifndef MRT_ALLOC_BUFFER_MS_H
#define MRT_ALLOC_BUFFER_MS_H

#include "RegionInfo.h"
#include "SizeClass.h"
#include "RegionList.h"

namespace MapleRuntime {

class CentralRegionPool;  // 前向声明

class AllocBufferMS {
public:
    AllocBufferMS();
    ~AllocBufferMS();

    void Init(CentralRegionPool* central);
    void Fini();

    // 分配入口（来自 RegionSpace::TryAllocateOnce 的 MOVEABLE_OBJECT 路径）
    MAddress Allocate(size_t size);

    // GC 发起的全局操作：将所有线程本地 Region 归还到 central 池
    void FlushAll();

    // 获取当前活跃 Region（给 GC 遍历用）
    RegionInfo* GetActiveRegion(uint8_t cls) const {
        return tlRegionPerClass_[cls];
    }

    static AllocBufferMS* GetOrCreate();
    static AllocBufferMS* Get();

private:
    // ===== 快速路径（内联） =====
    MAddress FastAlloc(uint8_t cls);

    // ===== 慢速路径 =====
    MAddress SlowAlloc(uint8_t cls);

    // 从 central 池获取新 Region
    RegionInfo* RefillFromCentral(uint8_t cls);

    // 将活跃 Region 归还到 central 池
    void ReturnRegion(uint8_t cls);

    // ===== 数据成员 =====

    // 每个 size class 一个当前活跃 Region
    // 为了兼容编译器的偏移假设，第 0 个元素可作为 "默认" tlRegion
    RegionInfo* tlRegionPerClass_[kNumSizeClasses];

    // 指向全局 CentralRegionPool
    CentralRegionPool* central_ = nullptr;

    // ===== 编译器兼容层 =====
    // 现有编译器假设 AllocBuffer 偏移 0 处是 tlRegion:
    //   static_assert(offsetof(AllocBuffer, tlRegion) == 0)
    //
    // 方案A（推荐）：让 AllocBufferMS 继承 AllocBuffer，
    //   tlRegionPerClass_[0] 的语义等价于旧 tlRegion。
    //
    // 方案B：保留 AllocBuffer 不变，AllocBufferMS 作为独立结构，
    //   在 TryAllocateOnce 入口处判断 GC 模式后分派。
    //
    // 本设计采用方案 B，降低对现有代码的侵入性。
};

} // namespace MapleRuntime
#endif // MRT_ALLOC_BUFFER_MS_H
```

### 5.3 快速路径实现

```cpp
// AllocBufferMS::Allocate —— 分配入口
MAddress AllocBufferMS::Allocate(size_t size) {
    uint8_t cls = SizeClassInfo::SizeToClass(size);
    if (UNLIKELY(cls >= kNumSizeClasses)) {
        return 0;  // 超大对象，由上层走 AllocLarge
    }
    return FastAlloc(cls);
}

// 内联快速路径
inline MAddress AllocBufferMS::FastAlloc(uint8_t cls) {
    RegionInfo* region = tlRegionPerClass_[cls];
    if (LIKELY(region != nullptr)) {
        // 1. 优先从 free-list 分配
        uintptr_t addr = region->AllocFromFreeList();
        if (LIKELY(addr != 0)) {
            return addr;
        }
        // 2. 尝试 bump-pointer
        addr = region->Alloc(SizeClassInfo::ClassToSize(cls));
        if (LIKELY(addr != 0)) {
            return addr;
        }
    }
    // 3. 慢速路径：当前 Region 已满或无 Region
    return SlowAlloc(cls);
}
```

### 5.4 慢速路径实现

```cpp
MAddress AllocBufferMS::SlowAlloc(uint8_t cls) {
    // 步骤 1: 归还当前已满的 Region
    if (tlRegionPerClass_[cls] != nullptr) {
        ReturnRegion(cls);
    }

    // 步骤 2: 从 central 池获取新 Region（含空闲 slot 的 partial Region 优先）
    RegionInfo* newRegion = RefillFromCentral(cls);
    if (newRegion == nullptr) {
        return 0;  // 上层触发 GC 或 OOM
    }

    // 步骤 3: 设为本线程活跃 Region
    tlRegionPerClass_[cls] = newRegion;

    // 步骤 4: 从新 Region 分配
    uintptr_t addr = newRegion->AllocFromFreeList();
    if (addr != 0) return addr;

    addr = newRegion->Alloc(SizeClassInfo::ClassToSize(cls));
    return addr;
}

void AllocBufferMS::ReturnRegion(uint8_t cls) {
    RegionInfo* region = tlRegionPerClass_[cls];
    if (region == nullptr) return;

    if (region->IsCompletelyFree()) {
        // 整体空闲 → 释放回 FreeRegionManager（物理页回收）
        central_->ReleaseRegion(region, cls);
    } else if (region->HasFreeSpace()) {
        // 有空闲 slot → 归还到 central 的 partial 列表
        central_->ReturnPartialRegion(region, cls);
    } else {
        // 完全满 → 归还到 central 的 full 列表
        central_->ReturnFullRegion(region, cls);
    }

    tlRegionPerClass_[cls] = nullptr;
}

RegionInfo* AllocBufferMS::RefillFromCentral(uint8_t cls) {
    // 1. 优先从 partial 列表获取（有空闲 slot，减少浪费）
    RegionInfo* region = central_->GetPartialRegion(cls);
    if (region != nullptr) return region;

    // 2. 没有 partial → 从 RegionManager 获取全新 Region
    region = central_->GetFreshRegion(cls);
    return region;
}
```

### 5.5 FlushAll——GC 前归还所有线程本地 Region

在 GC 的 STW 阶段（MarkSetup），需要将所有线程的 `AllocBufferMS` 中的活跃 Region 归还，这样 GC 可以统一遍历 central 池中的所有 Region 进行标记和 sweep。

```cpp
void AllocBufferMS::FlushAll() {
    for (uint8_t cls = 0; cls < kNumSizeClasses; cls++) {
        ReturnRegion(cls);
    }
}
```

### 5.6 与现有 AllocBuffer 的兼容方案

现有 `AllocBuffer` 的编译器约束：

```cpp
// AllocBuffer.h
static_assert(offsetof(AllocBuffer, tlRegion) == 0);
```

编译器生成的分配代码直接访问：
1. TLS → `AllocBuffer*` 指针
2. `AllocBuffer* + 0` → `RegionInfo* tlRegion`
3. `tlRegion + 0` → `allocPtr`
4. `allocPtr += size`，检查 `<= regionEnd`

**兼容策略**：

| 方案 | 描述 | 优点 | 缺点 |
|------|------|------|------|
| A. 继承 | AllocBufferMS 继承 AllocBuffer，偏移 0 保持 tlRegion | 编译器无需修改 | 侵入现有 AllocBuffer |
| B. 独立 | AllocBufferMS 独立于 AllocBuffer，在入口处分派 | 不侵入现有代码 | 编译器需感知 GC 模式 |
| C. 统一 | 修改 AllocBuffer，加 `#ifdef` 分支 | 单一类定义 | 增加复杂度 |

**推荐方案 B**：保持 `AllocBuffer` 完全不变，在 `RegionSpace::TryAllocateOnce` 中判断 GC 模式后分派到 `AllocBufferMS::Allocate`。编译器生成的**快速路径仍走现有 AllocBuffer**——但在 Mark-Sweep 模式下，`tlRegion` 始终指向一个"默认 size class"的活跃 Region（如 class 3 = 32B），应对编译器内联的快速分配。**不匹配的 size class 则走慢速路径到 AllocBufferMS**。

```
编译器内联快速路径:
  addr = tlRegion->allocPtr
  tlRegion->allocPtr += size
  if (allocPtr <= regionEnd) → 返回 addr  // 最快，但只适用于默认 size class

运行时慢速路径:
  → TryAllocateOnce
    → [Mark-Sweep] AllocBufferMS::Allocate(size)
      → SizeToClass(size) → 正确 size class 的 Region 分配
```

这样编译器不需要任何修改，仅在慢速路径上有分派开销。

---

## 6. CentralRegionPool —— 全局 Region 池

### 6.1 对标 Go 的 mcentral

Go 的 `mcentral` 为每个 size class 维护两个 span 列表：
- `partial`：有空闲 slot 的 span（sweep 后可复用）
- `full`：已满的 span（等待 sweep）

我们的 `CentralRegionPool` 是 Go `mcentral` 在 Region 模型下的等价物。

### 6.2 类设计

```cpp
// 新增文件: src/Heap/Allocator/CentralRegionPool.h

#ifndef MRT_CENTRAL_REGION_POOL_H
#define MRT_CENTRAL_REGION_POOL_H

#include "RegionList.h"
#include "RegionInfo.h"
#include "SizeClass.h"

namespace MapleRuntime {

class RegionManager;  // 前向声明

class CentralRegionPool {
public:
    explicit CentralRegionPool(RegionManager* regionManager);
    ~CentralRegionPool();

    // ===== 线程本地 Region 获取/归还 =====

    // 从 partial 列表获取一个有空闲 slot 的 Region（线程调用）
    RegionInfo* GetPartialRegion(uint8_t cls);

    // 从 RegionManager 获取全新 Region 并初始化为指定 size class
    RegionInfo* GetFreshRegion(uint8_t cls);

    // 归还有空闲 slot 的 Region 到 partial 列表
    void ReturnPartialRegion(RegionInfo* region, uint8_t cls);

    // 归还已满 Region 到 full 列表
    void ReturnFullRegion(RegionInfo* region, uint8_t cls);

    // 释放完全空闲的 Region 回 FreeRegionManager
    void ReleaseRegion(RegionInfo* region, uint8_t cls);

    // ===== GC 操作 =====

    // Sweep 阶段：遍历 full 列表中的 Region 进行 sweep
    // sweep 后根据空闲情况移到 partial 或释放
    void SweepFullRegions(uint8_t cls);

    // Sweep 所有 size class 的 full Region
    void SweepAll();

    // 获取所有非空 Region（供 GC 枚举根/标记使用）
    template<typename Visitor>
    void VisitAllRegions(Visitor&& visitor);

    // ===== 监控 =====
    size_t GetTotalPartialRegions() const;
    size_t GetTotalFullRegions() const;

private:
    struct SizeClassBucket {
        MutexLock lock;

        // partial: 有空闲 slot（sweep 后或首次分配未满）
        RegionList partialRegions;

        // full: 已满（正在使用中或等待 sweep）
        RegionList fullRegions;

        // 统计
        size_t partialCount = 0;
        size_t fullCount = 0;

        SizeClassBucket()
            : partialRegions("central-partial"),
              fullRegions("central-full") {}
    };

    SizeClassBucket buckets_[kNumSizeClasses];

    // 指向 RegionManager（用于 TakeRegion 获取新 Region）
    RegionManager* regionManager_;
};

} // namespace MapleRuntime
#endif // MRT_CENTRAL_REGION_POOL_H
```

### 6.3 核心方法实现

```cpp
// src/Heap/Allocator/CentralRegionPool.cpp

RegionInfo* CentralRegionPool::GetPartialRegion(uint8_t cls) {
    SizeClassBucket& bucket = buckets_[cls];
    LockGuard guard(bucket.lock);

    RegionInfo* region = bucket.partialRegions.TakeHeadRegion();
    if (region != nullptr) {
        bucket.partialCount--;
        region->SetRegionType(RegionType::THREAD_LOCAL_REGION);
        return region;
    }

    // partial 列表为空，尝试从 full 列表取一个并 sweep
    region = bucket.fullRegions.TakeHeadRegion();
    if (region != nullptr) {
        bucket.fullCount--;
        // 在持锁条件下执行 sweep（或先释放锁再 sweep，取决于性能权衡）
        region->SweepRegion();

        if (region->IsCompletelyFree()) {
            // 完全空闲 → 释放
            ReleaseRegionInternal(region);
            return nullptr;  // 递归或让调用者重试
        }

        region->SetRegionType(RegionType::THREAD_LOCAL_REGION);
        return region;
    }

    return nullptr;  // 都没有 → 让调用者请求全新 Region
}

RegionInfo* CentralRegionPool::GetFreshRegion(uint8_t cls) {
    // 通过现有 RegionManager::TakeRegion 获取
    RegionInfo* region = regionManager_->TakeRegion(1 /* nUnit = 默认 */);
    if (region == nullptr) return nullptr;

    // 初始化为 Mark-Sweep Region
    region->InitForSizeClass(cls);
    region->SetRegionType(RegionType::THREAD_LOCAL_REGION);
    return region;
}

void CentralRegionPool::ReturnPartialRegion(RegionInfo* region, uint8_t cls) {
    SizeClassBucket& bucket = buckets_[cls];
    LockGuard guard(bucket.lock);
    region->SetRegionType(RegionType::RECENT_PINNED_REGION);  // 复用现有类型或新增
    bucket.partialRegions.PrependRegion(region);
    bucket.partialCount++;
}

void CentralRegionPool::ReturnFullRegion(RegionInfo* region, uint8_t cls) {
    SizeClassBucket& bucket = buckets_[cls];
    LockGuard guard(bucket.lock);
    bucket.fullRegions.PrependRegion(region);
    bucket.fullCount++;
}

void CentralRegionPool::ReleaseRegion(RegionInfo* region, uint8_t /* cls */) {
    // 清理 Region 状态，归还给 FreeRegionManager
    region->InitFreeUnits();
    regionManager_->ReturnRegionToFreeManager(region);
}
```

### 6.4 Lazy Sweep 策略

核心优化：**不在 GC 结束时集中 sweep 所有 full Region**，而是**在分配需要时按需 sweep**（Go 的做法）。

```
线程需要 class-5 的 Region:
  1. GetPartialRegion(5) → partial 列表非空 → 直接返回
  2. GetPartialRegion(5) → partial 为空 → 取 full 列表头部 → 原地 sweep → 变成 partial → 返回
  3. 都没有 → GetFreshRegion(5) → 全新 Region
```

好处：
- **分摊 sweep 开销**：不集中 STW sweep，而是分散到各分配线程
- **按需清理**：冷 size class 的 Region 可能永远不需要 sweep
- **与并发标记配合**：标记完成后立即可以开始分配，不等 sweep 完成

### 6.5 与现有 RegionManager 的关系

```
                           ┌──── CentralRegionPool ────┐
                           │  [cls 0] partial / full   │
        AllocBufferMS ←──→ │  [cls 1] partial / full   │
        (per-thread)       │  [cls 2] partial / full   │
                           │        ...                 │
                           └───────────┬───────────────┘
                                       │ GetFreshRegion / ReleaseRegion
                                       ↓
                           ┌──── RegionManager ─────────┐
                           │  garbageRegionList         │ ← 三级获取
                           │  FreeRegionManager         │ ← (dirty/released)
                           │  InactiveZone              │ ← 虚拟地址扩展
                           └────────────────────────────┘
```

**CentralRegionPool 是 RegionManager 的上层**：
- 从 RegionManager 获取"原始" Region
- 管理 Region 的 size-class 绑定、partial/full 分类
- 向 AllocBufferMS 提供可用 Region

### 6.6 锁粒度与并发

| 操作 | 锁范围 | 争用分析 |
|------|--------|----------|
| FastAlloc（线程本地）| **无锁** | 最常见路径，纯线程本地操作 |
| SlowAlloc → GetPartialRegion | `buckets_[cls].lock` | 按 size class 分桶，争用率低 |
| ReturnRegion | `buckets_[cls].lock` | 仅在 Region 满/空时触发 |
| SweepRegion | 无外部锁（Region 已脱离列表）| sweep 在返回前完成 |

Go 的 `mcentral` 在 Go 1.16 后改用无锁 CAS 实现。我们初始版本使用互斥锁（简单可靠），后续可优化为 CAS。

---

## 7. 分配路径改造：完整调用链

### 7.1 改造后的分配流程图

```
用户代码 new
    │
    ▼
CompilerCalls → RegionSpace::Allocate(size, allocType)
    │
    ├── ToAllocSize(size): 8B 对齐
    │
    └── TryAllocateOnce(totalSize, allocType):
          │
          ├── allocType == PINNED_OBJECT?
          │     └── [Mark-Sweep] 统一走 AllocBufferMS::Allocate
          │         (因为所有对象都不移动，pinned 语义自动满足)
          │
          ├── totalSize > kLargeObjectThreshold (6144B)?
          │     └── RegionManager::AllocLarge(totalSize)  ← 不变
          │
          └── 小对象 (最常见路径):
                │
                ├── [Copy GC]   AllocBuffer::Allocate(totalSize)     ← 现有路径不变
                │
                └── [Mark-Sweep] AllocBufferMS::Allocate(totalSize)
                      │
                      ├── cls = SizeToClass(totalSize)
                      │
                      ├── FastAlloc(cls):                          ← 内联
                      │     ├── tlRegionPerClass_[cls]->AllocFromFreeList()
                      │     │     └── 成功 → 返回
                      │     └── tlRegionPerClass_[cls]->Alloc(classSize)
                      │           └── 成功 → 返回
                      │
                      └── SlowAlloc(cls):                          ← 非内联
                            ├── ReturnRegion(cls)  // 归还满 Region
                            │     └── → CentralRegionPool::ReturnFull/Partial/Release
                            ├── RefillFromCentral(cls)
                            │     ├── CentralRegionPool::GetPartialRegion(cls)
                            │     │     ├── partial 列表非空 → 返回
                            │     │     └── 取 full 列表头部 → SweepRegion → 返回
                            │     └── CentralRegionPool::GetFreshRegion(cls)
                            │           └── RegionManager::TakeRegion → InitForSizeClass
                            └── 从新 Region 分配 → 返回
```

### 7.2 模式分派机制

在 `RegionSpace::TryAllocateOnce` 中增加编译期分派：

```cpp
// src/Heap/Allocator/RegionSpace.cpp

MAddress RegionSpace::TryAllocateOnce(size_t totalSize, AllocType allocType) {
#ifdef MRT_USE_MARK_SWEEP
    // Mark-Sweep 模式
    if (totalSize > kLargeObjectThreshold) {
        return regionManager_.AllocLarge(totalSize);
    }
    AllocBufferMS* buffer = AllocBufferMS::Get();
    MAddress addr = buffer->Allocate(totalSize);
    return addr;
#else
    // Copy GC 模式（现有代码不变）
    if (allocType == AllocType::PINNED_OBJECT) {
        return regionManager_.AllocPinned(totalSize);
    }
    if (totalSize >= regionManager_.GetLargeObjectThreshold()) {
        return regionManager_.AllocLarge(totalSize);
    }
    AllocBuffer* allocBuffer = AllocBuffer::GetOrCreateAllocBuffer();
    return allocBuffer->Allocate(totalSize, allocType);
#endif
}
```

### 7.3 ShouldRetryAllocation 改造

现有重试逻辑（失败→触发 GC→再试→OOM）同样适用于 Mark-Sweep：

```cpp
bool RegionSpace::ShouldRetryAllocation(MAddress addr, size_t totalSize, AllocType allocType) {
    if (addr != 0) return false;  // 分配成功

    switch (retryLevel_) {
    case 0:
        retryLevel_ = 1;
        // 尝试重新调度（让其他线程释放 Region）
        return true;

    case 1:
        retryLevel_ = 2;
        // 触发启发式 GC（Mark-Sweep 周期）
#ifdef MRT_USE_MARK_SWEEP
        TriggerMarkSweepGC(GCReason::ALLOC_FAILURE);
#else
        TriggerCopyGC(GCReason::HEU);
#endif
        return true;

    case 2:
        retryLevel_ = 3;
        // 触发 OOM GC（最后机会）
#ifdef MRT_USE_MARK_SWEEP
        TriggerMarkSweepGC(GCReason::OOM);
#else
        TriggerCopyGC(GCReason::OOM);
#endif
        return true;

    default:
        // OOM
        return false;
    }
}
```

### 7.4 编译器快速路径兼容

编译器生成的分配快速路径（内联在用户代码中）：

```asm
; 伪汇编（aarch64）
ldr   x0, [tls, #AllocBuffer_offset]     ; x0 = AllocBuffer*
ldr   x1, [x0, #0]                        ; x1 = tlRegion (RegionInfo*)
ldr   x2, [x1, #0]                        ; x2 = allocPtr
add   x3, x2, size                         ; x3 = allocPtr + size
ldr   x4, [x1, #8]                        ; x4 = regionEnd
cmp   x3, x4
b.hi  slow_path                            ; allocPtr + size > regionEnd → 慢速路径
str   x3, [x1, #0]                        ; allocPtr = allocPtr + size
; x2 = 新对象地址
```

**在 Mark-Sweep 模式下，此快速路径仍然有效**：
- `tlRegion` 指向当前 size class 的活跃 Region
- `allocPtr` bump-pointer 用于 Region 尾部的空间
- 当 bump-pointer 耗尽时，进入 `slow_path`，由运行时走 `AllocBufferMS` 路径

**限制**：编译器快速路径不感知 free-list，只能用 bump-pointer。free-list 分配完全在运行时慢速路径中处理。这意味着 sweep 后的 Region 复用需要经过一次慢速路径调用。

### 7.5 Raw-Pointer 对象处理

`RAW_POINTER_OBJECT` 是暴露给外部 runtime 的对象，生命周期由 runtime 显式管理。在 Mark-Sweep 模式下处理方式：

```cpp
// Mark-Sweep 下 raw-pointer 对象仍使用独立 Region（不混入 size-class Region）
// 复用现有 tlRawPointerRegions / tlLargeRawPointerRegions 逻辑
if (allocType == AllocType::RAW_POINTER_OBJECT) {
    return AllocateRawPointerObject(totalSize);  // 现有路径不变
}
```

---

## 8. Sweep 集成点设计

### 8.1 Sweep 在 GC 流程中的位置

当前 Copy GC 无 sweep 阶段。Mark-Sweep 需要在标记完成后增加 sweep：

```
Copy GC 阶段:
  IDLE → INIT → ENUM → TRACE → POST_TRACE → PREFORWARD → FORWARD → IDLE

Mark-Sweep 阶段（目标）:
  IDLE → MARK_SETUP(STW) → CONCURRENT_MARK → MARK_TERM(STW) → CONCURRENT_SWEEP → IDLE
```

**Sweep 在标记终止（MARK_TERM）之后执行**。但本 Step 1 文档仅设计分配器侧的 sweep 接口——完整 GC 流程改造在 Step 2/3 中进行。

### 8.2 Sweep 粒度

| 粒度 | 描述 | 优缺点 |
|------|------|--------|
| **Region 级 sweep** | 遍历一个 Region 中所有 slot，按 bitmap 标记构建 free-list | 简单、缓存友好；粒度适中 |
| Slot 级 sweep | 逐 slot 检查 | 太细，开销大 |
| 批量 sweep | 一次 sweep 多个 Region | 可以 SIMD 优化 bitmap 扫描，批量处理效率高 |

我们选择 **Region 级 sweep**，与 `RegionInfo::SweepRegion()` 对应。

### 8.3 Sweep 触发方式

**方式 1：Lazy Sweep（推荐，Go 的做法）**

```
分配线程需要 Region → CentralRegionPool::GetPartialRegion(cls)
  → partial 为空 → 取 full 列表的头部 Region → SweepRegion() → 变成 partial → 返回
```

优点：
- **零额外暂停**：sweep 开销分摊到各分配线程
- **按需清理**：不活跃的 size class 不 sweep
- **实现简单**：只需在 `GetPartialRegion` 中增加 sweep 逻辑

**方式 2：后台 Sweep（可选增强）**

```
GC 标记完成后 → 启动后台 sweep 线程 → 遍历所有 full Region → SweepRegion()
与分配线程并发执行
```

优点：减少分配延迟。缺点：需要额外线程和同步。

**本 Step 选择方式 1（Lazy Sweep）**，后续可增加方式 2 作为优化。

### 8.4 Region 整体回收

sweep 后如果 Region 中**所有 slot 都是空闲的**，则 Region 可以整体释放回 `FreeRegionManager`：

```cpp
void CentralRegionPool::SweepFullRegions(uint8_t cls) {
    SizeClassBucket& bucket = buckets_[cls];
    LockGuard guard(bucket.lock);

    // 临时列表，避免在遍历中修改原列表
    RegionList toSweep("temp-sweep");
    toSweep.MergeRegionList(bucket.fullRegions);
    bucket.fullCount = 0;

    // 释放锁后 sweep（sweep 过程不需要锁）
    guard.unlock();

    RegionInfo* region;
    while ((region = toSweep.TakeHeadRegion()) != nullptr) {
        region->SweepRegion();

        LockGuard innerGuard(bucket.lock);
        if (region->IsCompletelyFree()) {
            ReleaseRegionInternal(region);
        } else {
            bucket.partialRegions.PrependRegion(region);
            bucket.partialCount++;
        }
    }
}
```

### 8.5 Bitmap 清理时机

标记位（markBitmap）在 sweep 时需要清理。两种策略：

| 策略 | 描述 | 优缺点 |
|------|------|--------|
| **Sweep 时清理** | `SweepRegion` 中对存活对象清除标记位 | 简单，与 sweep 一体 |
| 双 bitmap 交替 | 两套 bitmap 交替使用，不需要清理 | 快，但内存翻倍 |

本 Step 选择 **Sweep 时清理**（已在 4.4 节 `SweepRegion` 中实现）。

### 8.6 与 liveByteCount 的关系

当前 `RegionInfo::metadata.liveByteCount` 在 Copy GC 中用于判断 Region 是否值得 forward。在 Mark-Sweep 中复用此字段：

```cpp
// Mark 阶段：标记对象时累加 liveByteCount
void MarkAndCount(uintptr_t addr, size_t objSize) {
    MarkObject(addr);
    __atomic_fetch_add(&metadata.liveByteCount, objSize, __ATOMIC_RELAXED);
}

// Sweep 阶段开始前检查：
//   liveByteCount == 0 → 整个 Region 都是垃圾 → 直接释放（不需要逐 slot sweep）
//   liveByteCount == regionAllocatedSize → 整个 Region 都是存活 → 跳过 sweep
//   其他 → 需要 sweep
```

这个**快速跳过**优化可以避免对全活或全死 Region 的不必要遍历。

---

## 9. 代码修改清单

### 9.1 新增文件

| 文件 | 内容 | 依赖 |
|------|------|------|
| `src/Heap/Allocator/SizeClass.h` | Size class 表定义和查找函数 | 无 |
| `src/Heap/Allocator/SizeClass.cpp` | `SizeClassInfo::Init()` 填充查找表 | SizeClass.h |
| `src/Heap/Allocator/AllocBufferMS.h` | 线程本地 Mark-Sweep 分配缓存声明 | SizeClass.h, RegionInfo.h |
| `src/Heap/Allocator/AllocBufferMS.cpp` | AllocBufferMS 实现（快速/慢速路径）| AllocBufferMS.h, CentralRegionPool.h |
| `src/Heap/Allocator/CentralRegionPool.h` | 全局 Region 池声明 | RegionList.h, SizeClass.h |
| `src/Heap/Allocator/CentralRegionPool.cpp` | CentralRegionPool 实现 | CentralRegionPool.h, RegionManager.h |

### 9.2 修改文件

| 文件 | 修改内容 | 改动量级 |
|------|----------|---------|
| `src/Heap/Allocator/RegionInfo.h` | 增加 `FreeSlot` 结构、`UnitMetadata` 扩展字段、`AllocFromFreeList`/`SweepRegion`/`InitForSizeClass` 等方法 | **中** |
| `src/Heap/Allocator/RegionSpace.h` | 增加 `CentralRegionPool` 成员、Mark-Sweep 初始化接口 | **小** |
| `src/Heap/Allocator/RegionSpace.cpp` | `TryAllocateOnce` 增加 `MRT_USE_MARK_SWEEP` 分支、`ShouldRetryAllocation` 适配 | **中** |
| `src/Heap/Allocator/RegionManager.h` | 增加 `ReturnRegionToFreeManager` 公开接口（如果不存在）| **小** |
| `CMakeLists.txt` | 增加新文件到编译目标、增加 `MRT_USE_MARK_SWEEP` 编译宏选项 | **小** |
| `config.cmake` | 增加 `MARK_SWEEP_FLAG` 选项 | **小** |
| `build.py` | 增加 `--gc-type mark-sweep` 命令行选项 | **小** |

### 9.3 不修改的文件（确保兼容）

| 文件 | 原因 |
|------|------|
| `src/Heap/Allocator/AllocBuffer.h` | 保持编译器兼容性，不修改 `tlRegion` 偏移 |
| `src/Heap/Allocator/FreeRegionManager.h` | 底层内存管理不变 |
| `src/Heap/Allocator/RegionList.h` | 基础数据结构不变 |
| `src/Heap/Collector/` | Collector 改造在 Step 2-4，本 Step 不涉及 |
| `src/Heap/CJBarrier/` | 屏障改造在 Step 4，本 Step 不涉及 |

### 9.4 编译宏设计

```cmake
# config.cmake
option(MARK_SWEEP_FLAG "Enable Mark-Sweep GC mode" OFF)

if(MARK_SWEEP_FLAG)
    add_definitions(-DMRT_USE_MARK_SWEEP=1)
    message(STATUS "GC mode: Mark-Sweep")
else()
    message(STATUS "GC mode: Copy GC (default)")
endif()
```

```python
# build.py 增加参数
parser.add_argument('--gc-type', choices=['copy', 'mark-sweep'],
                    default='copy', help='GC algorithm type')
```

### 9.5 逐文件修改说明

#### 9.5.1 RegionInfo.h 修改详情

```diff
+ // 新增: FreeSlot 结构
+ struct FreeSlot {
+     FreeSlot* next;
+ };

  struct UnitMetadata {
      uintptr_t allocPtr;
      uintptr_t regionEnd;
      // ... 现有字段 ...

+     #ifdef MRT_USE_MARK_SWEEP
+     FreeSlot* freeListHead = nullptr;
+     uint32_t  freeSlotCount = 0;
+     uint32_t  totalSlotCount = 0;
+     uint8_t   sizeClass = 0;
+     uint8_t   sweepState = 0;
+     uint16_t  padding_ms = 0;
+     #endif
  };

  class RegionInfo {
      // ... 现有方法 ...

+     #ifdef MRT_USE_MARK_SWEEP
+     void InitForSizeClass(uint8_t cls);
+     uintptr_t AllocFromFreeList();
+     size_t SweepRegion();
+     bool IsCompletelyFree() const;
+     bool HasFreeSpace() const;
+     uint8_t GetSizeClass() const;
+     uint32_t GetFreeSlotCount() const;
+     #endif
  };
```

#### 9.5.2 RegionSpace.cpp 修改详情

```diff
  MAddress RegionSpace::TryAllocateOnce(size_t totalSize, AllocType allocType) {
+ #ifdef MRT_USE_MARK_SWEEP
+     if (allocType == AllocType::RAW_POINTER_OBJECT) {
+         return AllocateRawPointerObject(totalSize);
+     }
+     if (totalSize > SizeClassInfo::kLargeObjectThreshold) {
+         return regionManager_.AllocLarge(totalSize);
+     }
+     AllocBufferMS* buffer = AllocBufferMS::GetOrCreate();
+     return buffer->Allocate(totalSize);
+ #else
      // 现有 Copy GC 逻辑不变
      if (allocType == AllocType::PINNED_OBJECT) {
          return regionManager_.AllocPinned(totalSize);
      }
      // ...
+ #endif
  }
```

### 9.6 实施顺序

```
第 1 阶段：基础设施（可独立编译测试）
  ├── [1a] SizeClass.h/cpp — 实现 size class 表和查找函数
  └── [1b] RegionInfo.h 扩展 — 增加 FreeSlot、MS 字段和方法

第 2 阶段：核心组件（依赖第 1 阶段）
  ├── [2a] CentralRegionPool.h/cpp — 全局 Region 池
  └── [2b] AllocBufferMS.h/cpp — 线程本地分配缓存

第 3 阶段：集成（依赖第 2 阶段）
  ├── [3a] RegionSpace.cpp 分派逻辑
  ├── [3b] CMakeLists.txt / config.cmake / build.py 构建配置
  └── [3c] 编写单元测试
```

---

## 10. 验证方案

### 10.1 编译验证

```bash
# 验证 Copy GC 模式不受影响（默认，不加 mark-sweep 标志）
python3 build.py build --target native --build-type debug -v 0.0.1

# 验证 Mark-Sweep 模式可编译
python3 build.py build --target native --build-type debug -v 0.0.1 --gc-type mark-sweep

# 确保两种模式都无编译错误和警告（-Werror 启用）
```

### 10.2 单元测试

#### 10.2.1 SizeClass 测试

```cpp
// test_size_class.cpp
void TestSizeToClass() {
    // 边界测试
    assert(SizeClassInfo::SizeToClass(1) == 0);   // → 8B
    assert(SizeClassInfo::SizeToClass(8) == 0);   // → 8B
    assert(SizeClassInfo::SizeToClass(9) == 1);   // → 16B
    assert(SizeClassInfo::SizeToClass(16) == 1);  // → 16B
    assert(SizeClassInfo::SizeToClass(6144) == 31);// → 6144B
    assert(SizeClassInfo::SizeToClass(6145) == kNumSizeClasses); // → Large

    // 回环测试
    for (uint8_t cls = 0; cls < kNumSizeClasses; cls++) {
        size_t sz = SizeClassInfo::ClassToSize(cls);
        assert(SizeClassInfo::SizeToClass(sz) == cls);
    }
}
```

#### 10.2.2 Region Free-List 测试

```cpp
void TestRegionFreeList() {
    RegionInfo* region = AllocTestRegion();
    region->InitForSizeClass(3);  // 32B slots

    // 1. bump-pointer 分配 4 个对象
    uintptr_t obj1 = region->Alloc(32);
    uintptr_t obj2 = region->Alloc(32);
    uintptr_t obj3 = region->Alloc(32);
    uintptr_t obj4 = region->Alloc(32);
    assert(obj1 && obj2 && obj3 && obj4);

    // 2. 模拟标记：obj1, obj3 存活，obj2, obj4 死亡
    region->MarkObject(obj1);
    region->MarkObject(obj3);

    // 3. Sweep
    size_t freed = region->SweepRegion();
    assert(freed == 64);  // 2 * 32B
    assert(region->GetFreeSlotCount() == 2);

    // 4. 从 free-list 分配
    uintptr_t reuse1 = region->AllocFromFreeList();
    assert(reuse1 == obj4 || reuse1 == obj2);  // 复用死亡对象空间
    uintptr_t reuse2 = region->AllocFromFreeList();
    assert(reuse2 == obj4 || reuse2 == obj2);
    assert(reuse1 != reuse2);

    // 5. free-list 耗尽后回退到 bump-pointer
    uintptr_t fromBump = region->AllocFromFreeList();
    assert(fromBump == 0);
    uintptr_t bump = region->Alloc(32);  // 应从 allocPtr 继续分配
    assert(bump != 0);
}
```

#### 10.2.3 AllocBufferMS 测试

```cpp
void TestAllocBufferMS() {
    CentralRegionPool pool(&regionManager);
    AllocBufferMS buffer;
    buffer.Init(&pool);

    // 分配不同大小的小对象
    MAddress addr8 = buffer.Allocate(8);
    MAddress addr32 = buffer.Allocate(32);
    MAddress addr256 = buffer.Allocate(256);
    MAddress addr4096 = buffer.Allocate(4096);
    assert(addr8 && addr32 && addr256 && addr4096);

    // 验证不同 size class 使用不同 Region
    RegionInfo* r8 = RegionInfo::GetRegionInfoByAddr(addr8);
    RegionInfo* r32 = RegionInfo::GetRegionInfoByAddr(addr32);
    assert(r8->GetSizeClass() == SizeClassInfo::SizeToClass(8));
    assert(r32->GetSizeClass() == SizeClassInfo::SizeToClass(32));

    buffer.FlushAll();
}
```

### 10.3 集成测试

使用现有的仓颉测试用例（`gc/gc.cj`）进行集成验证：

```bash
# 编译 Mark-Sweep 版本 runtime
cd /Users/lipeisheng/code/cangjie/cangjie/cangjie_runtime/runtime
python3 build.py build --gc-type mark-sweep

# 替换 SDK 中的 dylib
cp output/common/darwin_release_aarch64/runtime/lib/darwin_aarch64_cjnative/libcangjie-runtime.dylib \
   /Users/lipeisheng/code/cangjie/cangjie-pkg/dev/20260220/cangjie/runtime/lib/darwin_aarch64_cjnative/

# 编译并运行仓颉测试
source /Users/lipeisheng/code/cangjie/cangjie-pkg/dev/20260220/cangjie/envsetup.sh
cd gc && cjc gc.cj && ./main
```

**注意**：由于本 Step 只改造了分配器，尚未实现完整的 Mark-Sweep GC 流程。集成测试需要一个简化的"手动标记+sweep"harness，或暂时禁用 GC 仅测试分配路径。

### 10.4 性能基准

| 指标 | 测试方法 | 合格标准 |
|------|----------|----------|
| 单线程分配吞吐 | 循环分配 1M 个小对象（混合 size），测量总耗时 | ≥ Copy GC 的 90% |
| 多线程分配吞吐 | 8 线程并行分配，测量总吞吐 | ≥ Copy GC 的 85% |
| free-list 分配延迟 | sweep 后从 free-list 分配，测量 P99 延迟 | ≤ 200ns |
| Region 复用率 | 分配 → 标记 50% → sweep → 再分配，测量 Region 新增数量 | 新增 Region 减少 ≥ 40% |

### 10.5 正确性检查清单

- [ ] `SizeToClass` 对所有合法大小（1B ~ 6144B）返回正确 class
- [ ] `ClassToSize(SizeToClass(n)) >= n` 对所有 n 成立
- [ ] Region sweep 后 `freeSlotCount + liveSlotCount == totalSlotCount`
- [ ] 每个 free-list slot 的地址都在 Region 地址范围内
- [ ] free-list 无环（可通过快慢指针检测）
- [ ] sweep 后 free-list 中的 slot 不包含任何存活对象
- [ ] 编译器快速路径分配的对象地址对齐到 8B
- [ ] `AllocBuffer::tlRegion` 偏移仍为 0（`static_assert` 通过）
- [ ] Mark-Sweep 模式和 Copy GC 模式可独立编译且互不影响
- [ ] 大对象路径（`AllocLarge`）行为不变

---

## 11. 大对象管理改造设计

> 本节设计参照 Go runtime 的 mheap 大对象管理机制，解决 Cangjie 当前大对象与小对象共享固定堆空间导致的 OOM 风险和碎片化问题。

### 11.1 现状问题分析

#### 11.1.1 当前大对象分配路径

```
RegionSpace::Allocate(size)
  → size > largeObjectThreshold (默认 32KB)?
    → RegionManager::AllocLarge(size)
      → regionCount = ceil(size / UNIT_SIZE)    // 按页对齐
      → TakeRegion(regionCount, LARGE_SIZED_UNITS)
        → ① garbageRegionList 查找可复用 Region
        → ② FreeRegionManager (CartesianTree) 查找连续空闲 Unit
        → ③ inactiveZone 扩展（受 regionHeapEnd 硬限制）
      → region->Alloc(size)  // bump-pointer 一次性分配
      → 加入 recentLargeRegionList / largeTraceRegions
```

**关键代码**（`RegionManager.h` `AllocLarge`）：
```cpp
uintptr_t AllocLarge(size_t size) {
    size_t regionCount = (size + RegionInfo::UNIT_SIZE - 1) / RegionInfo::UNIT_SIZE;
    RegionInfo* region = TakeRegion(regionCount, RegionInfo::UnitRole::LARGE_SIZED_UNITS);
    if (region == nullptr) return 0;  // OOM!
    uintptr_t addr = region->Alloc(size);
    // ... 加入 large region list
    return addr;
}
```

#### 11.1.2 核心问题

| 问题 | 原因 | 影响 |
|------|------|------|
| **固定堆上界 OOM** | `regionHeapEnd` 在 `Init()` 时固定（默认 64MB），`TakeRegion` inactiveZone 扩展受此硬限 | 大对象消耗堆空间后，小对象也可能 OOM |
| **大小对象争用** | 大对象和小对象共享同一 Region 空间（同一 `inactiveZone`） | 几个大对象即可消耗大量 Unit，挤压小对象空间 |
| **连续 Unit 碎片** | 大对象需要连续 N 个 Unit，释放后的碎片可能无法被复用 | CartesianTree 中存在大量小碎片，无法满足大分配请求 |
| **无独立生命周期** | 大对象释放后的 Unit 回到通用 FreeRegionManager，与小 Region 混合 | 无法针对大对象做专门的页面回收策略 |

#### 11.1.3 数值分析

以默认 64MB 堆为例（`UNIT_SIZE = 4KB`）：
- 总 Unit 数 = 64MB / 4KB = **16,384**
- 一个 1MB 对象需要 256 个连续 Unit（占总堆的 **1.56%**）
- 一个 4MB 对象需要 1,024 个连续 Unit（占总堆的 **6.25%**）
- **仅 16 个 4MB 对象即可完全耗尽堆空间**

### 11.2 Go 的大对象管理机制

Go 对大对象（`> 32KB`，即 size class 0）的处理：

```
Go 大对象分配路径:
  mallocgc(size > 32KB)
    → mcache.allocLarge(size)
      → mheap.alloc(npages)              // 直接从页堆分配
        → pageAlloc.alloc(npages)         // 基数树查找连续空闲页
          → searchAddr 快速定位            // O(1) 地址提示
          → pallocBits.find(npages)       // 位图搜索
        → 如果不够 → mheap.grow(npage)    // 向 OS 申请新 arena
      → span.init(base, npages)
      → heapBitsForAddr(base) 设置标记位
```

**Go 的设计要点**：

| 特性 | 实现 | 效果 |
|------|------|------|
| 无固定上界 | arena 按需向 OS 申请（64MB/个） | 堆大小仅受 OS 虚拟内存限制 |
| 高效页面查找 | pageAlloc 基数树（5 层 summary） | O(log N) 查找连续空闲页 |
| 独立生命周期 | 大 span 直接在 mheap 管理 | 释放后立即可被 OS 回收 |
| 后台归还 | scavenger 线程按 `scavengeGoal` 归还 | 平衡内存占用与分配性能 |

### 11.3 方案设计：两层大对象管理

```
                    ┌─────────────────────────────────────────┐
                    │           RegionSpace::Allocate          │
                    │  size > largeObjectThreshold (32KB)?     │
                    └──────────────┬──────────────────────────┘
                                   │ YES
                    ┌──────────────▼──────────────────────────┐
                    │   size <= HUGE_OBJECT_THRESHOLD (4MB)?   │
                    └──────┬───────────────────────┬──────────┘
                      YES  │                       │  NO
              ┌────────────▼────────────┐  ┌───────▼──────────────┐
              │  Tier 1: 堆内大对象      │  │  Tier 2: 堆外超大对象 │
              │  (32KB ~ 4MB)           │  │  (> 4MB)             │
              │  继续用 TakeRegion       │  │  独立 mmap            │
              │  走现有 Region 路径      │  │  LargeObjectSpace     │
              │  受益于扩大的虚拟预留    │  │  独立生命周期管理      │
              └─────────────────────────┘  └──────────────────────┘
```

#### 11.3.1 Tier 1：堆内大对象（32KB ~ 4MB）

**不改变现有路径**，通过 §12 的动态堆增长方案（扩大虚拟地址预留）来解决空间不足问题。

改进点：
- `inactiveZone` 可增长到远超 64MB 的范围（见 §12）
- 大对象释放后的连续 Unit 优先放入专用 `largeFreeUnitTree`
- `CartesianTree` 拆分为 Small 和 Large 两棵树，减少碎片干扰

```cpp
// 新增：FreeRegionManager 按大小分离
class FreeRegionManager {
    CartesianTree dirtySmallUnits;  // < LARGE_TREE_THRESHOLD (e.g., 64 Units)
    CartesianTree dirtyLargeUnits;  // >= LARGE_TREE_THRESHOLD
    CartesianTree releasedSmallUnits;
    CartesianTree releasedLargeUnits;

    RegionInfo* TakeRegion(size_t num, UnitRole type, bool expectPhysical) {
        if (num >= LARGE_TREE_THRESHOLD) {
            // 优先从 large 树查找
            region = dirtyLargeUnits.TakeRegion(num, type);
            if (region) return region;
            region = releasedLargeUnits.TakeRegion(num, type);
            if (region) return region;
        }
        // fallback 到 small 树或全局搜索
        return TakeFromSmallTrees(num, type);
    }
};
```

#### 11.3.2 Tier 2：堆外超大对象空间（LargeObjectSpace）

对于超过 `HUGE_OBJECT_THRESHOLD`（默认 4MB, 可通过环境变量配置）的对象，使用独立 mmap 分配。

**设计理由**：
- 4MB+ 的对象在堆内需要 1024+ 连续 Unit，连续性要求高，碎片风险大
- 独立 mmap 后可通过 `munmap` 立即归还 OS，无需等待 Region 回收流程
- 不占用主堆的 Unit 配额，不影响小对象分配

**核心数据结构**：

```cpp
// 新增文件: src/Heap/Allocator/LargeObjectSpace.h

struct LargeObjectInfo {
    MAddress baseAddr;          // mmap 起始地址（页对齐）
    size_t   mappedSize;        // mmap 映射大小（页对齐）
    size_t   objectSize;        // 实际对象大小
    uint8_t  gcMark;            // GC 标记位（三色标记用）
    bool     isTraceAllocated;  // 是否在 GC trace 阶段分配
    LargeObjectInfo* next;      // 链表指针
    LargeObjectInfo* prev;
};

class LargeObjectSpace {
public:
    // 分配：mmap + 注册
    MAddress Allocate(size_t size);

    // 释放：munmap + 注销
    void Free(MAddress addr);

    // 查询：O(1) 平均复杂度
    bool Contains(MAddress addr) const;
    LargeObjectInfo* GetObjectInfo(MAddress addr) const;

    // GC 集成
    void MarkObject(MAddress addr);
    void SweepUnmarkedObjects();
    void ClearAllMarks();

    // 遍历所有存活大对象（用于 GC root 扫描）
    void ForEachObject(const std::function<void(MAddress, size_t)>& visitor);

    // 统计
    size_t GetTotalMappedSize() const { return totalMappedSize_.load(); }
    size_t GetObjectCount() const { return objectCount_.load(); }

private:
    // 地址 → LargeObjectInfo 的快速查找
    // 使用页对齐地址作为 key，避免哈希碰撞
    mutable std::shared_mutex mu_;
    std::unordered_map<MAddress, LargeObjectInfo*> objectMap_;

    // 用于 GC 遍历的双向链表
    LargeObjectInfo* head_ = nullptr;

    // 统计信息
    std::atomic<size_t> totalMappedSize_{0};
    std::atomic<size_t> objectCount_{0};

    // 内部 mmap 封装
    MAddress MapPages(size_t size);
    void UnmapPages(MAddress addr, size_t size);
};
```

**分配实现**：

```cpp
MAddress LargeObjectSpace::Allocate(size_t size) {
    // 1. 加上对象头
    size_t allocSize = size + HEADER_SIZE;
    size_t mappedSize = RoundUp(allocSize, MRT_PAGE_SIZE);

    // 2. 独立 mmap
    MAddress base = MapPages(mappedSize);
    if (base == 0) return 0;

    // 3. 注册到查找结构
    auto* info = new LargeObjectInfo{
        .baseAddr = base,
        .mappedSize = mappedSize,
        .objectSize = size,
        .gcMark = 0,
        .isTraceAllocated = false
    };

    {
        std::unique_lock lock(mu_);
        objectMap_[base] = info;
        // 加入链表
        info->next = head_;
        if (head_) head_->prev = info;
        head_ = info;
    }

    totalMappedSize_.fetch_add(mappedSize);
    objectCount_.fetch_add(1);

    return base + HEADER_SIZE;
}
```

**与 `IsHeapAddress` 的集成**：

```cpp
// 修改 Heap.h
class Heap {
    static LargeObjectSpace* largeObjectSpace_;

    static bool IsHeapAddress(MAddress addr) {
        // 快速路径：主堆范围检查（绝大多数对象命中）
        if (addr >= heapStartAddr && addr < heapCurrentEnd) {
            return true;
        }
        // 慢速路径：LOS 查找（仅超大对象）
        if (largeObjectSpace_ != nullptr) {
            return largeObjectSpace_->Contains(addr);
        }
        return false;
    }
};
```

**性能考量**：
- 主堆范围检查仍是 O(1)，绝大多数对象（>99.9%）命中快速路径
- LOS 的 `Contains()` 使用 `shared_mutex` 读锁 + `unordered_map` 查找，平均 O(1)
- 超大对象数量通常极少（<100 个），哈希表开销可忽略

### 11.4 GC 集成方案

#### 11.4.1 标记阶段

```
GC Mark Phase:
  1. 标记根集 → 如果根引用指向 LOS 对象，调用 LOS::MarkObject()
  2. 三色标记遍历:
     - 每个灰色对象的引用字段:
       - IsHeapAddress(ref) → true?
         - 在主堆範围? → 正常 Region 路径
         - 在 LOS?     → LOS::MarkObject(ref)
  3. LOS 对象自身的引用字段也需要扫描（递归标记）
```

#### 11.4.2 清扫阶段

```cpp
void LargeObjectSpace::SweepUnmarkedObjects() {
    std::unique_lock lock(mu_);
    LargeObjectInfo* curr = head_;
    while (curr != nullptr) {
        LargeObjectInfo* next = curr->next;
        if (curr->gcMark == 0) {
            // 未标记 → 死对象，立即释放
            objectMap_.erase(curr->baseAddr);
            // 从链表移除
            if (curr->prev) curr->prev->next = curr->next;
            if (curr->next) curr->next->prev = curr->prev;
            if (curr == head_) head_ = curr->next;

            totalMappedSize_.fetch_sub(curr->mappedSize);
            objectCount_.fetch_sub(1);

            UnmapPages(curr->baseAddr, curr->mappedSize);
            delete curr;
        } else {
            curr->gcMark = 0;  // 重置标记
        }
        curr = next;
    }
}
```

#### 11.4.3 写屏障适配

`Barrier.cpp` 中大量使用 `Heap::IsHeapAddress()` 决定屏障行为。修改后：

```cpp
// Barrier.cpp 中的典型模式（无需修改逻辑，仅 IsHeapAddress 行为变化）
if ((obj != nullptr && !obj->HasRefField()) ||
    (!Heap::IsHeapAddress(obj) && !Heap::IsHeapAddress(src))) {
    return;  // 非堆对象，跳过屏障
}
```

由于 `IsHeapAddress` 已扩展支持 LOS，写屏障逻辑无需结构性修改。但需要注意：
- LOS 对象不在 Region 中，不能调用 `GetRegionInfoAt()`
- 写屏障中如需获取 Region 元数据，需先判断是否为 LOS 对象

```cpp
// 安全的 Region 查找模式
inline RegionInfo* SafeGetRegionInfo(MAddress addr) {
    if (LIKELY(addr >= Heap::heapStartAddr && addr < Heap::heapCurrentEnd)) {
        return RegionInfo::GetRegionInfoAt(addr);  // 快速路径
    }
    return nullptr;  // LOS 对象或非堆对象，无 RegionInfo
}
```

### 11.5 大对象相关代码修改清单

| 文件 | 修改类型 | 内容 |
|------|----------|------|
| `src/Heap/Allocator/LargeObjectSpace.h` | **新增** | LargeObjectInfo 结构体 + LargeObjectSpace 类声明 |
| `src/Heap/Allocator/LargeObjectSpace.cpp` | **新增** | Allocate/Free/Contains/Sweep 实现 |
| `src/Heap/Allocator/FreeRegionManager.h` | 修改 | 拆分 CartesianTree 为 Small/Large 两组 |
| `src/Heap/Allocator/RegionSpace.cpp` | 修改 | `Allocate()` 增加 Tier 2 分发路径 |
| `src/Heap/Heap.h` | 修改 | `IsHeapAddress()` 增加 LOS fallback |
| `src/Heap/Barrier/Barrier.cpp` | 修改 | 增加 `SafeGetRegionInfo()` 安全检查 |
| `src/Heap/Collector/*.cpp` | 修改 | Mark/Sweep 阶段集成 LOS 遍历 |
| `src/CangjieRuntimeApi.cpp` | 修改 | 增加 `HUGE_OBJECT_THRESHOLD` 环境变量配置 |

---

## 12. 动态堆增长与收缩设计

> 本节设计参照 Go runtime 的 arena 动态增长和 scavenger 收缩机制，消除 Cangjie 固定堆上界的限制。

### 12.1 现状分析

#### 12.1.1 当前堆初始化流程

```cpp
// RegionSpace::Init() 关键逻辑
void RegionSpace::Init(const HeapParam& vmHeapParam) {
    size_t heapSize = vmHeapParam.heapSize * 1024;         // e.g., 64KB→64MB
    size_t totalSize = RegionManager::GetHeapMemorySize(heapSize); // metadata + heap
    size_t unitNum = RegionManager::GetHeapUnitCount(heapSize);

    map = MemMap::MapMemory(totalSize, totalSize, opt);     // 一次性 mmap
    regionManager.Initialize(unitNum, metadata);
    reservedStart = regionManager.GetRegionHeapStart();
    reservedEnd = map->GetMappedEndAddr();                  // 固定上界!
}
```

关键约束：
- `MapMemory(totalSize, totalSize, opt)` 一次性预留 `totalSize` 虚拟地址
- `MAP_NORESERVE` 标志：虚拟地址预留不消耗物理内存
- `regionHeapEnd` = `reservedEnd` → TakeRegion 的硬上界
- UnitInfo 元数据在堆前方连续存储：`(heapStart - (idx+1) * sizeof(UnitInfo))`

#### 12.1.2 为什么当前设计限制了堆大小

```
内存布局（当前）:
┌─────────────────────────────┬─────────────────────────────────────────────┐
│    UnitInfo 元数据区         │              Region 堆空间                  │
│  (unitNum * sizeof(UnitInfo))│         (unitNum * UNIT_SIZE)               │
│  ← 向低地址增长              │         → 向高地址增长 (inactiveZone)        │
├─────────────────────────────┼─────────────────────────────────────────────┤
│ map->GetBaseAddr()          │ regionHeapStart              regionHeapEnd  │
└─────────────────────────────┴─────────────────────────────────────────────┘
                                                                ↑ 硬限制！
```

- UnitInfo 元数据必须预分配（`sizeof(UnitInfo) * unitNum`）
- 堆空间 = `unitNum * UNIT_SIZE`，两者绑定
- 如果要扩大堆空间，元数据区也必须同步扩大
- 但元数据区在堆前方，向低地址生长 → 扩大元数据会破坏已有布局

### 12.2 Go 的动态堆机制

```
Go arena（64位 Linux）:
  ┌──────────────────────────────────────────────────────┐
  │          512GB 虚拟地址保留（heapArenaBytes = 64MB）   │
  │  Arena 0  │ Arena 1 │ Arena 2 │ ... │ Arena N       │
  │  (64MB)   │ (64MB)  │ (64MB)  │     │ (64MB)        │
  │  已提交    │ 已提交   │ 未提交   │     │ 未提交         │
  └──────────────────────────────────────────────────────┘

  arenaIndex[addr] → arenaInfo（位图、span 指针）
  元数据独立于数据区，按 arena 粒度按需分配
```

Go 的关键设计点：
1. **超大虚拟预留**：预留 512GB+ 虚拟地址，仅按需提交物理页
2. **元数据按需分配**：每个 arena 的 `heapArena` 结构独立分配
3. **增长 = 新建 arena**：`mheap.grow()` 用 `sysMap` 提交新 arena 的物理页
4. **收缩 = scavenger**：后台线程用 `madvise(MADV_DONTNEED)` 归还物理页

### 12.3 方案设计：大虚拟预留 + 分段元数据

#### 12.3.1 核心思路

```
┌──────────────────────────────────────────────────────────────────────────┐
│                     改造后的内存布局                                      │
│                                                                          │
│    ┌───────────────────────┐  ┌────────────────────────────────────────┐ │
│    │  元数据区(Segments)    │  │             Region 堆空间              │ │
│    │  ┌─────┬─────┬─────┐  │  │  ┌─────────┬──────────┬────────────┐  │ │
│    │  │Seg 0│Seg 1│Seg 2│  │  │  │ Segment 0│Segment 1 │  未提交... │  │ │
│    │  │已提交│已提交│未提交│  │  │  │ (已用)   │(部分用)  │            │  │ │
│    │  └─────┴─────┴─────┘  │  │  └─────────┴──────────┴────────────┘  │ │
│    └───────────────────────┘  │  heapStart          inactiveZone       │ │
│    metaBase                   └─────────────────── maxVirtualEnd ──────┘ │
└──────────────────────────────────────────────────────────────────────────┘
```

**方案核心**：
1. 初始化时预留超大虚拟地址空间（如 64GB），`MAP_NORESERVE` 不占物理内存
2. 元数据区也按需提交：按 **Segment**（如每段对应 256MB 堆空间）增量 `mprotect`
3. `regionHeapEnd` 不再是硬限制，改为 **软目标**（GC 触发阈值），`maxVirtualEnd` 才是真正上界
4. 堆增长：当 `inactiveZone` 接近当前已提交段上界时，提交新段
5. 堆收缩：Scavenger 线程通过 `madvise` 释放远超水位线的空闲页

#### 12.3.2 关键参数

```cpp
// 新增常量
static constexpr size_t MAX_VIRTUAL_RESERVE = 64ULL * 1024 * 1024 * 1024;  // 64GB 虚拟预留
static constexpr size_t SEGMENT_SIZE = 256ULL * 1024 * 1024;                // 256MB/段
static constexpr size_t UNITS_PER_SEGMENT = SEGMENT_SIZE / RegionInfo::UNIT_SIZE; // 65536 Units/段
static constexpr size_t META_PER_SEGMENT = UNITS_PER_SEGMENT * sizeof(UnitInfo);  // 元数据/段

// 64GB 总共 = 256 个 Segment，每个 Segment 的元数据约 5MB
// 全部元数据预留空间 = 256 * 5MB ≈ 1.3GB 虚拟地址（MAP_NORESERVE 无物理消耗）
```

#### 12.3.3 HeapGrowthManager

```cpp
// 新增文件: src/Heap/Allocator/HeapGrowthManager.h

class HeapGrowthManager {
public:
    // 初始化：大虚拟预留 + 首段提交
    bool Init(size_t initialHeapSize);

    // 检查是否需要扩展并执行
    // 返回 true 表示成功扩展或无需扩展
    bool EnsureCapacity(size_t requiredUnits);

    // 提交下一个 Segment 的物理页（堆空间 + 元数据）
    bool GrowOneSegment();

    // Scavenger 接口：归还空闲物理页
    size_t ScavengeToTarget(size_t targetRetainedSize);

    // 查询
    size_t GetCommittedSegmentCount() const { return committedSegments_.load(); }
    size_t GetMaxVirtualCapacity() const { return maxVirtualCapacity_; }
    size_t GetCommittedCapacity() const {
        return committedSegments_.load() * SEGMENT_SIZE;
    }

private:
    // 虚拟地址基址
    MAddress metaBase_ = 0;      // 元数据区起始
    MAddress heapBase_ = 0;      // 堆空间起始
    size_t   maxVirtualCapacity_ = 0; // 虚拟预留总大小

    // 段管理
    std::atomic<size_t> committedSegments_{0};
    std::mutex growMutex_;  // 保护段提交操作

    // Scavenger 相关
    std::atomic<uintptr_t> scavengeWatermark_{0}; // 高于此线的空闲页可归还
};
```

**段提交实现**：

```cpp
bool HeapGrowthManager::GrowOneSegment() {
    std::lock_guard lock(growMutex_);

    size_t segIdx = committedSegments_.load();
    size_t maxSegments = maxVirtualCapacity_ / SEGMENT_SIZE;
    if (segIdx >= maxSegments) {
        return false;  // 虚拟地址用尽（极端情况）
    }

    // 1. 提交堆空间物理页
    MAddress heapSegStart = heapBase_ + segIdx * SEGMENT_SIZE;
    if (!MemMap::CommitPages(heapSegStart, SEGMENT_SIZE)) {
        return false;
    }

    // 2. 提交对应元数据区物理页
    MAddress metaSegStart = metaBase_ + segIdx * META_PER_SEGMENT;
    if (!MemMap::CommitPages(metaSegStart, META_PER_SEGMENT)) {
        MemMap::DecommitPages(heapSegStart, SEGMENT_SIZE);  // 回滚
        return false;
    }

    // 3. 更新 UnitInfo::totalUnitCount
    size_t newTotalUnits = (segIdx + 1) * UNITS_PER_SEGMENT;
    RegionInfo::UnitInfo::totalUnitCount = newTotalUnits;

    // 4. 更新 regionHeapEnd
    regionManager_.regionHeapEnd = heapBase_ + newTotalUnits * RegionInfo::UNIT_SIZE;

    // 5. 更新 Heap::heapCurrentEnd（影响 IsHeapAddress）
    Heap::OnHeapExtended(regionManager_.regionHeapEnd);

    committedSegments_.fetch_add(1);
    return true;
}
```

#### 12.3.4 Init 改造

```cpp
// 改造后的 RegionSpace::Init()
void RegionSpace::Init(const HeapParam& vmHeapParam) {
    size_t initialHeapSize = vmHeapParam.heapSize * 1024;

    // === 改造点 1：计算大虚拟预留 ===
    size_t maxHeapSize = MAX_VIRTUAL_RESERVE;  // 64GB
    size_t maxUnits = maxHeapSize / RegionInfo::UNIT_SIZE;
    size_t maxMetaSize = RegionManager::GetMetadataSize(maxUnits);
    size_t totalVirtualSize = maxMetaSize + maxHeapSize;

    // === 改造点 2：mmap 大虚拟空间但仅提交初始段 ===
    MemMap::Option opt = MemMap::DEFAULT_OPTIONS;
    opt.tag = "cangjie_heap";
    // MapMemory(requestSize, initialCommitSize, opt)
    // 修改 MapMemory 接口：requestSize = 总虚拟，initialCommitSize = 首段
    map = MemMap::MapMemory(totalVirtualSize, 0, opt);  // 全部 PROT_NONE

    MAddress metadata = map->GetBaseAddr();
    size_t initialUnits = initialHeapSize / RegionInfo::UNIT_SIZE;

    // === 改造点 3：仅提交初始段的元数据和堆空间 ===
    size_t initialSegments = (initialUnits + UNITS_PER_SEGMENT - 1) / UNITS_PER_SEGMENT;
    size_t initialMetaSize = RegionManager::GetMetadataSize(initialUnits);
    MemMap::CommitPages(metadata, initialMetaSize);         // 元数据
    MAddress heapStart = metadata + maxMetaSize;            // 堆起始 = 元数据区后
    MemMap::CommitPages(heapStart, initialSegments * SEGMENT_SIZE); // 堆空间

    // === 改造点 4：初始化 ===
    regionManager.Initialize(initialUnits, metadata);
    reservedStart = heapStart;
    reservedEnd = heapStart + maxHeapSize;  // 虚拟上界（非物理上界）

    Heap::OnHeapCreated(reservedStart);
    Heap::OnHeapExtended(heapStart + initialSegments * SEGMENT_SIZE); // 初始已提交上界

    // === 改造点 5：初始化 HeapGrowthManager ===
    growthManager_.Init(metadata, heapStart, maxHeapSize, initialSegments);
}
```

#### 12.3.5 TakeRegion 改造

```cpp
// 修改 RegionManager::TakeRegion() 的 inactiveZone 扩展逻辑
RegionInfo* RegionManager::TakeRegion(size_t num, UnitRole type, bool expectPhysicalMem) {
    // ... 现有的 garbageRegionList 和 FreeRegionManager 查找 ...

    size_t size = num * RegionInfo::UNIT_SIZE;

    // === 改造点：移除硬限制，增加动态扩展 ===
    if (num <= GetInactiveUnitCount()) {
        uintptr_t addr = inactiveZone.fetch_add(size);
        if (addr < regionHeapEnd - size) {
            // 仍在已提交范围内，直接使用
            region = RegionInfo::InitRegionAt(addr, num, type);
            return region;
        } else {
            (void)inactiveZone.fetch_sub(size);  // 回滚
        }
    }

    // === 新增：尝试动态扩展 ===
    if (growthManager_.EnsureCapacity(num)) {
        // 扩展成功，重试
        uintptr_t addr = inactiveZone.fetch_add(size);
        if (addr < regionHeapEnd - size) {
            region = RegionInfo::InitRegionAt(addr, num, type);
            return region;
        } else {
            (void)inactiveZone.fetch_sub(size);
        }
    }

    return nullptr;  // 真正的 OOM：虚拟地址空间也用尽
}
```

### 12.4 GC 目标堆大小控制（Go 风格 GOGC）

动态堆增长需要 GC 来控制"有效堆大小"防止无限膨胀，参照 Go 的 `GOGC` 机制：

```
Go 的堆增长控制:
  nextGC = lastLiveSize * (1 + GOGC/100)
  例如: lastLiveSize = 100MB, GOGC = 100
        nextGC = 200MB
  当 heapAlloc 达到 200MB 时触发 GC
```

Cangjie 适配：

```cpp
// GC 触发阈值计算
class HeapGoalController {
public:
    // GOGC 百分比（100 = 首轮活跃 2 倍触发，200 = 3 倍触发）
    size_t gcPercent = 100;  // 类似 GOGC，可通过环境变量配置

    size_t CalculateNextGCGoal(size_t liveSize) const {
        if (gcPercent == 0) return liveSize;  // 极端保守
        return liveSize + liveSize * gcPercent / 100;
    }

    // GC 后更新
    void OnGCComplete(size_t currentLiveSize) {
        nextGCGoal_ = CalculateNextGCGoal(currentLiveSize);
    }

    // 分配时检查是否达到阈值
    bool ShouldTriggerGC(size_t currentHeapSize) const {
        return currentHeapSize >= nextGCGoal_;
    }

private:
    size_t nextGCGoal_ = 0;
};
```

### 12.5 Scavenger 后台线程

参照 Go 的 `runtime.bgscavenge()`，实现后台线程归还空闲物理页：

```cpp
// 新增文件: src/Heap/Scavenger.h

class Scavenger {
public:
    void Start();   // 启动后台线程
    void Stop();    // 停止

    // 设置归还目标：保留 retainedSize 字节物理内存，超出部分归还
    void SetScavengeGoal(size_t retainedSize);

private:
    void ScavengeLoop();

    // 核心逻辑：扫描已释放的 Region/Unit，madvise 归还物理页
    size_t ScavengeOnce(size_t targetRelease);

    // 归还单个 Region 的物理页
    void DecommitRegion(RegionInfo* region);

    std::thread thread_;
    std::atomic<bool> running_{false};
    std::condition_variable cv_;
    std::mutex mu_;
    size_t scavengeGoal_ = 0;
};
```

**Scavenger 执行策略**（参照 Go）：

```
Scavenger 循环:
  while (running) {
    1. 计算 scavengeGoal:
       retainedGoal = max(nextGCGoal, heapMinimum)
       releaseTarget = currentMapped - retainedGoal

    2. 如果 releaseTarget <= 0，sleep 等待下次 GC 结束

    3. 否则，扫描 FreeRegionManager 中的 dirtyUnits：
       - 对每个空闲且未 release 的 Region:
         - madvise(MADV_DONTNEED) / mmap(MAP_FIXED) 归还物理页
         - 从 dirtyTree 移到 releasedTree
       - 直到累计归还 >= releaseTarget

    4. sleep(scavengeSleep) 限流，避免影响 mutator
  }
```

**与现有 `ReleaseGarbageRegions` 的关系**：

当前 `RegionManager::ReleaseGarbageRegions()` 在 GC 结束后同步释放。改造后：
- 保留 `ReleaseGarbageRegions()` 的 **Region 回收**逻辑（归还到 FreeRegionManager）
- **物理页归还**交给 Scavenger 异步执行，减少 GC 停顿
- `RegionInfo::ReleaseUnits()` 已实现了 `madvise` 逻辑，Scavenger 可直接复用

### 12.6 MemMap 接口扩展

```cpp
// 新增接口
class MemMap {
public:
    // 提交物理页（mprotect PROT_READ|PROT_WRITE）
    static bool CommitPages(MAddress addr, size_t size);

    // 归还物理页但保留虚拟地址（madvise MADV_DONTNEED）
    static bool DecommitPages(MAddress addr, size_t size);

    // 平台实现
    // Linux:   CommitPages → mprotect(PROT_READ|PROT_WRITE)
    //          DecommitPages → madvise(MADV_DONTNEED)
    // macOS:   CommitPages → mprotect(PROT_READ|PROT_WRITE)
    //          DecommitPages → madvise(MADV_FREE) 或 mmap(MAP_FIXED) 重映射
    // Windows: CommitPages → VirtualAlloc(MEM_COMMIT)
    //          DecommitPages → VirtualFree(MEM_DECOMMIT)
};
```

### 12.7 UnitInfo 寻址兼容性

当前 `UnitInfo::GetUnitIdxAt()` 的关键实现：

```cpp
static size_t GetUnitIdxAt(uintptr_t allocAddr) {
    if (heapStartAddress <= allocAddr &&
        allocAddr < (heapStartAddress + totalUnitCount * UNIT_SIZE)) {
        return (allocAddr - heapStartAddress) / UNIT_SIZE;
    }
    std::abort();  // 越界直接 abort!
}
```

**兼容方案**：Segment 提交时同步更新 `totalUnitCount`：

```cpp
// HeapGrowthManager::GrowOneSegment() 中:
RegionInfo::UnitInfo::totalUnitCount = (segIdx + 1) * UNITS_PER_SEGMENT;
```

这样 `GetUnitIdxAt`、`GetUnitInfo`、`GetUnitAddress` 等函数无需修改，
只要保证 `totalUnitCount` 始终等于已提交堆空间对应的 Unit 数量即可。

**线程安全**：`totalUnitCount` 是 `static size_t`，写入发生在 `growMutex_` 保护下，
读取的竞态条件可接受（即使读到旧值，只会导致访问被拒绝而不会越界）。
建议改为 `std::atomic<size_t>` 以确保可见性：

```cpp
// RegionInfo.h 修改
class UnitInfo {
    static std::atomic<size_t> totalUnitCount;  // 原: static size_t
};
```

### 12.8 `GetRegionInfoAt` 安全防护

当前 `GetRegionInfoAt` 内部调用 `GetUnitIdxAt`，越界会 abort。
对于 LOS（§11）返回的堆外地址，调用 `GetRegionInfoAt` 是不安全的。

**新增安全版本**：

```cpp
// RegionInfo.h 新增
static RegionInfo* TryGetRegionInfoAt(uintptr_t allocAddr) {
    if (allocAddr < UnitInfo::heapStartAddress ||
        allocAddr >= UnitInfo::heapStartAddress + UnitInfo::totalUnitCount * UNIT_SIZE) {
        return nullptr;  // 堆外地址，安全返回
    }
    return GetRegionInfoAt(allocAddr);
}
```

在所有可能接收 LOS 地址的路径中，使用 `TryGetRegionInfoAt` 替代 `GetRegionInfoAt`。

### 12.9 动态堆相关代码修改清单

| 文件 | 修改类型 | 内容 |
|------|----------|------|
| `src/Heap/Allocator/HeapGrowthManager.h/cpp` | **新增** | 段管理、动态提交、Scavenger 协调 |
| `src/Heap/Scavenger.h/cpp` | **新增** | 后台归还线程 |
| `src/Heap/Allocator/MemMap.h/cpp` | 修改 | 新增 `CommitPages()` / `DecommitPages()` |
| `src/Heap/Allocator/RegionSpace.cpp` | 修改 | `Init()` 大虚拟预留 + 首段提交 |
| `src/Heap/Allocator/RegionManager.cpp` | 修改 | `TakeRegion()` 增加动态扩展逻辑 |
| `src/Heap/Allocator/RegionInfo.h` | 修改 | `totalUnitCount` 改为 atomic；新增 `TryGetRegionInfoAt` |
| `src/Heap/Heap.h` | 修改 | `OnHeapExtended()` 在段提交时调用 |
| `src/CangjieRuntimeApi.cpp` | 修改 | 增加 `GOGC` / `MAX_HEAP_SIZE` 环境变量 |
| `src/Heap/HeapGoalController.h/cpp` | **新增** | GC 目标堆大小控制 |

### 12.10 与 Go 对比总结

| 维度 | Go | Cangjie 改造后 |
|------|-----|---------------|
| 虚拟预留 | 512GB+（arena hint） | 64GB（可配置） |
| 增长单位 | arena (64MB) | Segment (256MB) |
| 元数据管理 | heapArena 按 arena 独立分配 | UnitInfo 分段提交（连续地址） |
| 地址→元数据 | arenaIndex 二级映射 | 直接偏移计算（不变） |
| 收缩机制 | bgscavenge 线程 | Scavenger 线程 |
| GC 触发 | GOGC 百分比 | gcPercent（类似 GOGC） |
| 堆外大对象 | 无（所有对象在 arena 内） | LOS 独立 mmap（§11） |

---

## 附录 A：关键数据结构对照表

| Go 概念 | Cangjie 现有概念 | Step 1 新增 | 说明 |
|---------|-----------------|-------------|------|
| mspan | Region(RegionInfo) | Region + size class | 保留 Region 框架，增加 size class 绑定 |
| mcache | AllocBuffer | AllocBufferMS | 线程本地，从 1 个 Region 扩展到 32 个 |
| mcentral | - (无直接对应) | CentralRegionPool | 全局 partial/full Region 管理 |
| mheap | RegionManager + FreeRegionManager | HeapGrowthManager | 底层 Region 获取 + 动态堆增长 |
| mheap.alloc (large) | RegionManager::AllocLarge | AllocLarge + LargeObjectSpace | Tier 1 堆内 + Tier 2 堆外 |
| pageAlloc | FreeRegionManager (CartesianTree) | 拆分 Small/Large 两组树 | 减少大小对象碎片干扰 |
| arena | 固定堆 (Init 一次性 mmap) | Segment (256MB) | 分段提交，按需增长 |
| bgscavenge | - (无) | Scavenger | 后台线程归还空闲物理页 |
| GOGC | - (无) | HeapGoalController | GC 触发阈值百分比控制 |
| size class table | - (无) | SizeClassInfo | 32 级 size class |
| sweepgen | - (无) | sweepState | Region 级 sweep 状态 |
| freeindex | allocPtr | allocPtr + freeListHead | bump-pointer + free-list 混合 |

## 附录 B：与后续 Step 的接口约定

本 Step 为后续 Step 预留以下接口：

| 接口 | 调用方 | 作用 |
|------|--------|------|
| `RegionInfo::SweepRegion()` | Step 3 (MarkSweepCollector) | GC 结束后 sweep Region |
| `CentralRegionPool::SweepAll()` | Step 3 (MarkSweepCollector) | 批量 sweep 所有 full Region |
| `CentralRegionPool::VisitAllRegions()` | Step 2 (Marker) | 标记阶段遍历所有 Region 中的对象 |
| `AllocBufferMS::FlushAll()` | Step 2 (STW MarkSetup) | STW 时归还所有线程本地 Region |
| `SizeClassInfo::SizeToClass()` | Step 4 (WriteBarrier) | 屏障中判断对象大小 |
| `RegionInfo::IsCompletelyFree()` | Step 3 (Collector) | 判断 Region 是否可整体释放 |
| `LargeObjectSpace::Allocate()` | RegionSpace | 超大对象堆外分配 |
| `LargeObjectSpace::SweepUnmarkedObjects()` | Step 3 (Collector) | GC 后清扫 LOS 死对象 |
| `LargeObjectSpace::ForEachObject()` | Step 2 (Marker) | 标记阶段遍历 LOS 对象 |
| `HeapGrowthManager::EnsureCapacity()` | RegionManager::TakeRegion | 堆空间不足时动态扩展 |
| `HeapGrowthManager::GrowOneSegment()` | EnsureCapacity 内部 | 提交新 Segment 物理页 |
| `Scavenger::SetScavengeGoal()` | Step 3 (GC 结束后) | 设置归还目标，触发后台释放 |
| `HeapGoalController::ShouldTriggerGC()` | 分配路径 | 判断是否需要触发 GC |
| `RegionInfo::TryGetRegionInfoAt()` | Barrier / Collector | 安全获取 Region 元数据（LOS 兼容） |
