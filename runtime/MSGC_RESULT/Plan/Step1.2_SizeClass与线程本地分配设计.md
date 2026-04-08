# Step 1.2：Size Class 与线程本地分配设计

> 本文档从 [Step1_分配器改造详细设计.md](Step1_分配器改造详细设计.md) 提取，涵盖 Size Class 设计、线程本地分配缓存（AllocBufferMS）和全局 Region 池（CentralRegionPool）。
>
> 2026-04-05 当前代码状态校准：
> 1. `CentralRegionPool::GetRegion()` 当前优先级是 `partialSwept -> partialUnswept -> fullUnswept -> emptySweptCache -> fresh`，而不是单纯依赖 dirty tree / sweep 完成后再统一放行。
> 2. GC 起始阶段当前不会调用文中旧稿里的 `ForceFinishSweep()`；真实收口点在 `MarkSweepCollector::PreEnumRoots()`，由 `StopAndWaitForAllSweep()` 与 unswept 重新分类完成。
> 3. `emptySweptCache`、`MaybeNotifyReclaim()` 和 `InlineReclaimForBalance()` 已经接入当前实现；文中凡是把空 region 直接建模成 dirty/released tree 的段落，应视为历史方案。

---

## 目录

1. [Size Class 设计](#1-size-class-设计)
2. [AllocBufferMS —— 线程本地分配缓存](#2-allocbufferms--线程本地分配缓存)
3. [CentralRegionPool —— 全局 Region 池](#3-centralregionpool--全局-region-池)
4. [Region 生命周期：获取与释放全流程](#4-region-生命周期获取与释放全流程)

---

## 1. Size Class 设计

### 1.1 为什么需要 Size Class

Mark-Sweep 后 Region 内部有大小不一的空洞。如果不按大小分类：
- 一个 24B 的空洞无法容纳 32B 的对象 → **外部碎片**
- 用 32B 的空洞存放 8B 的对象 → **内部碎片**
- 遍历 bitmap 找合适大小 → **分配变慢**

Go 的解决方案：**每个 span（对应我们的 Region）只服务一种 size class**。这样：
- 同一 Region 内所有 slot 大小相同 → 碎片可控
- 分配时不需搜索匹配大小 → O(1) CTZ 找空闲 bit 即可

### 1.2 Size Class 表设计

参考 Go 的 67 级 size-class，结合 Cangjie 对象特点（最小 8B 对齐、Region = 32KB），设计 36 级：

```cpp
// 新增文件: src/Heap/Allocator/SizeClass.h

#ifndef MRT_SIZE_CLASS_H
#define MRT_SIZE_CLASS_H

#include <cstddef>
#include <cstdint>

namespace MapleRuntime {

// Size Class 表：索引 → 对象大小上限（字节）
// 设计原则：
//   - 每级之间增长约 12.5~25%，平衡碎片与级数
//   - 8B 对齐
//   - Region = 32KB，大对象阈值 = 16KB（Region/2）
//   - 最大 class = 16384B (16KB)，超过走 Large 路径
static constexpr size_t kNumSizeClasses = 36;

static constexpr size_t kSizeClasses[kNumSizeClasses] = {
    //  小对象（8B ~ 128B），步进 8~16B
    8,    16,   24,   32,   48,   64,   80,   96,
    //  中小对象（112B ~ 512B），步进 16~128B
    112,  128,  160,  192,  224,  256,  320,  384,
    //  中等对象（448B ~ 2048B），步进 64~256B
    448,  512,  640,  768,  896,  1024, 1280, 1536,
    //  中大对象（1792B ~ 4096B），步进 256~512B
    1792, 2048, 2560, 3072, 3584, 4096,
    //  大对象（5120B ~ 16384B），步进 1024~2048B
    5120, 6144, 8192, 10240, 12288, 16384,
};

// 大对象阈值：超过此值走 AllocLarge 路径（独占多 Region）
// = REGION_SIZE / 2 = 32KB / 2 = 16KB
// 超过 16KB 的对象无法有效共享 Region（slot 数 < 2），走独立 Large Region
static constexpr size_t kLargeObjectThreshold = 16384;

// Size → Class 快速查找表（空间换时间）
// 对于 size <= 1024，直接用 size/8 做索引（最多 128 项）
// 对于 1024 < size <= 16384，用 (size-1024)/256 + 128 做索引
static constexpr size_t kMaxLookupSize = 16384;
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

### 1.3 内部碎片分析

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
| 29 | 4096 | 3584 | 12.5% |
| 33 | 10240 | 8192 | 20% |
| 35 | 16384 | 12288 | 25% |

**整体加权平均内部碎片约 12~15%**，与 Go 接近。

### 1.4 每 Region slot 数量

Region = 32KB：

| Size Class | 大小 (B) | Slots/Region | Region 利用率 |
|:----------:|:--------:|:------------:|:------------:|
| 0 | 8 | 4096 | 100% |
| 3 | 32 | 1024 | 100% |
| 5 | 64 | 512 | 100% |
| 9 | 128 | 256 | 100% |
| 13 | 224 | 146 | 99.9% |
| 15 | 384 | 85 | 99.6% |
| 19 | 768 | 42 | 98.4% |
| 23 | 1536 | 21 | 98.4% |
| 29 | 4096 | 8 | 100% |
| 33 | 10240 | 3 | 93.8% |
| 35 | 16384 | 2 | 100% |

### 1.5 与 HEADER_SIZE 的关系

当前 `RegionSpace::HEADER_SIZE = 0`（对象无额外头部）。`ToAllocSize` 仅做 8B 对齐：

```cpp
size_t ToAllocSize(size_t objSize) {
    return AlignUp(objSize + HEADER_SIZE, 8);  // HEADER_SIZE = 0
}
```

在 Mark-Sweep 模式下，**bitmap 以 1 bit 粒度管理每个 slot 的分配状态**（详见 [Step1.1 §3.5](Step1.1_MS_RegionInfo元数据改造设计.md)），最小 slot 大小即最小 size class = 8B。当前最小 size class = 8B，满足 8B 对齐约束。如果未来需要添加对象头（如标记位内联到对象头），需要相应调整 HEADER_SIZE 和 size class 表。

---

---

## 2. AllocBufferMS —— 线程本地分配缓存

### 2.1 对标 Go 的 mcache

Go 的每个 P（Processor）有一个 `mcache`，其中包含：
- 67 个 `mspan` 指针（每个 size class 一个活跃 span）
- 67 个 `mspan` 指针（noscan 版本，不含指针的对象）

我们的对应设计：`AllocBufferMS`，每个线程（或协程 Processor）一个，包含 36 个当前活跃 Region（每个 size class 一个）。

### 2.2 类设计

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

### 2.3 快速路径实现

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
        // 1. 优先从 bitmap 分配（§3.5.3 CTZ 算法）
        uintptr_t addr = region->AllocFromBitmap();
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

### 2.4 慢速路径实现

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
    uintptr_t addr = newRegion->AllocFromBitmap();
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

### 2.5 FlushAll——GC 前归还所有线程本地 Region

在 GC 的 STW 阶段（MarkSetup），需要将所有线程的 `AllocBufferMS` 中的活跃 Region 归还，这样 GC 可以统一遍历 central 池中的所有 Region 进行标记和 sweep。

```cpp
void AllocBufferMS::FlushAll() {
    for (uint8_t cls = 0; cls < kNumSizeClasses; cls++) {
        ReturnRegion(cls);
    }
}
```

### 2.6 与现有 AllocBuffer 的兼容方案

#### 2.6.1 现有编译器快速路径分析

现有编译器（cjc/LLVM 后端）为 CopyGC 生成了 **bump-pointer 内联快速路径**，硬编码了 `AllocBuffer` 的内存布局：

```cpp
// AllocBuffer.h — 编译器依赖的 ABI 约束
static_assert(offsetof(AllocBuffer, tlRegion) == 0,
              "need to modify the offset of this value in llvm-project at the same time");
```

编译器生成的机器码直接执行（无函数调用）：
```
TLS → AllocBuffer* buf
RegionInfo* r = *(buf + 0)        // 偏移 0 = tlRegion
uintptr_t ptr = r->allocPtr       // bump pointer
r->allocPtr = ptr + size
if (ptr + size <= r->regionEnd)   // 边界检查
  → return ptr                    // ✅ 3~4 条指令，~1-2ns
else
  → call AllocBuffer::Allocate    // ❌ 进入运行时慢速路径
```

**问题**：这条快速路径是纯 bump-pointer，**不感知 size class**。在 Mark-Sweep 模式下，不同大小的对象必须分配到对应 size class 的 Region 中，bump-pointer 无法正确处理。

#### 2.6.2 两阶段方案

| | 阶段一（Step 1，当前） | 阶段二（未来） |
|--|--|--|
| **策略** | 关闭编译器 bump-pointer 快速路径，所有分配走运行时接口 | 编译器内联 Go 风格 bitmap CTZ 快速路径 |
| **编译器改动** | **极小**：仅关闭内联快速路径，始终调用 `AllocBuffer::Allocate` | **较大**：需修改 LLVM codegen，生成新的内联序列 |
| **运行时改动** | `AllocBuffer::Allocate` 内部分派到 `AllocBufferMS::Allocate` | 快速路径在编译器侧完成，仅 refill 走运行时 |
| **分配路径** | 每次 → 函数调用 → `SizeToClass` → bitmap CTZ | 内联 bitmap CTZ → 命中直接返回；未命中 → 函数调用 refill |
| **单次分配耗时** | ~8-15 ns（函数调用 + 查表 + CTZ） | ~3-5 ns（内联 CTZ，与 Go 对齐） |
| **相比现有 CopyGC** | 慢 5~10x（从 ~1-2ns 到 ~8-15ns） | 慢 2~3x（从 ~1-2ns 到 ~3-5ns） |
| **对整体程序影响** | 分配密集型减速 10~30%，一般业务 5~15% | 分配密集型减速 5~10%，一般业务 <5% |
| **优先级** | ✅ **先做**——验证 MS 正确性，不依赖编译器团队 | 后续优化——正确性验证后再投入 |

#### 2.6.3 阶段一：关闭编译器快速路径

**做法**：在 Mark-Sweep 模式下，让编译器**不生成内联 bump-pointer 代码**，所有分配直接调用运行时接口。

**实现方式**（二选一）：

**方式 A（推荐）：编译开关控制**
```
编译器增加 flag: -gc-mode=marksweep
  → 关闭 bump-pointer 内联
  → 所有 new 操作直接 call AllocBuffer::Allocate(size, allocType)
```
编译器改动量极小：仅在生成分配代码时检查 flag，跳过内联 fast-path 的 emit。

**方式 B：运行时令快速路径始终失败**
```
Mark-Sweep 模式初始化时：
  tlRegion->allocPtr = 0
  tlRegion->regionEnd = 0
  → 编译器内联的 "allocPtr + size <= regionEnd" 检查永远失败
  → 必然 fall through 到 AllocBuffer::Allocate
```
无需任何编译器改动，但每次分配多执行 3~4 条无用指令。

**阶段一的完整分配路径**：
```
所有 new 操作:
  → call AllocBuffer::Allocate(size, allocType)
    → [CopyGC]     原有 bump-pointer 路径（不变）
    → [Mark-Sweep]  AllocBufferMS::Allocate(size)
                      → SizeToClass(size)           // O(1) 查表
                      → FastAlloc(cls)              // bitmap CTZ
                        → 命中 → 返回地址
                        → 未命中 → SlowAlloc → refill from CentralRegionPool
```

#### 2.6.4 阶段二：编译器内联 Go 风格快速路径（未来）

Go 编译器为每次 `mallocgc` 生成的内联代码（简化）：
```go
c := getg().m.p.mcache
s := c.alloc[sizeclass]
freeIndex := s.nextFreeIndex()         // CTZ(allocCache)
if freeIndex < s.nelems {
    return s.base() + freeIndex * s.elemsize  // ✅ ~3-5ns
}
→ call runtime.mcache.nextFree          // 慢路径 refill
```

**对应 Cangjie 的未来快速路径**：
```
编译器内联:
  AllocBufferMS* ms = TLS[alloc_buffer_ms_offset]
  uint8_t cls = lookupTable[size]                    // 编译期可计算
  RegionInfo* r = ms->tlRegionPerClass_[cls]
  uint64_t bits = r->allocBitmap[wordIdx]
  if (bits != 0) {
    int slot = CTZ(bits)                             // 找第一个空闲 bit
    r->allocBitmap[wordIdx] &= ~(1ULL << slot)      // 清除该 bit
    return r->base + slot * classSize                // ✅ ~3-5ns
  }
  → call AllocBufferMS::SlowAlloc(cls)               // 慢路径 refill
```

**编译器需要的改动**：
1. TLS 中存放 `AllocBufferMS*`，或增加新 TLS slot
2. 生成 `SizeToClass` 查表代码（固定大小可编译期直接计算 class 索引）
3. 生成 bitmap CTZ + bit clear 序列
4. 慢路径调用 `SlowAlloc`

**为什么不在 Step 1 就做**：
- 需修改 LLVM 后端 codegen，涉及编译器团队配合
- 内联代码正确性依赖运行时数据结构布局，需先稳定
- 阶段一性能足够验证 MS GC 的正确性和功能完整性
- 迁移完全透明——仅影响快速路径的位置，不影响分配逻辑

#### 2.6.5 性能预估对比

| 场景 | CopyGC（现有） | MS 阶段一（无内联） | MS 阶段二（内联 CTZ） | Go MS |
|------|--------------|-------------------|---------------------|-------|
| 单次分配 | ~1-2 ns | ~8-15 ns | ~3-5 ns | ~3-5 ns |
| 分配密集型吞吐 | 基准 1.0x | 0.7~0.85x | 0.9~0.95x | ~0.9x |
| GC 停顿 | 长（全堆复制） | 短（增量 sweep） | 短（增量 sweep） | 短 |
| **综合评价** | 分配快、停顿长 | 分配较慢、停顿短 | 分配接近 Go、停顿短 | 参考基线 |

> **阶段一的性能代价是可接受的**：Mark-Sweep 消除了 CopyGC 的全堆复制停顿，GC 停顿从 O(堆大小) 降为 O(存活对象)。分配路径的额外开销（~10ns/次）远小于 GC 停顿的收益。阶段二优化是锦上添花，不阻塞功能交付。

---


---

## 3. CentralRegionPool —— 全局 Region 池

### 3.1 对标 Go 的 mcentral

Go 1.16+ 的 `mcentral` 为每个 size class 维护 **四个** span 列表（按 sweep 状态 × 空闲状态组合）：
- `partialSwept`：有空闲 slot、已 sweep（可直接分配）
- `partialUnswept`：有空闲 slot、未 sweep（需原地 sweep 后分配）
- `fullSwept`：已满、已 sweep（等待下轮 GC）
- `fullUnswept`：已满、未 sweep（需 sweep 后判断是否可复用）

这种设计通过 `sweepGen` 代数机制实现链表身份的 O(1) 翻转，避免 GC 结束时集中 sweep 的停顿。

我们的 `CentralRegionPool` 采用相同的四链表设计，是 Go `mcentral` 在 Region 模型下的等价物。

### 3.2 类设计

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

    // 强制完成上轮所有剩余 sweep（GC 开始前调用，对标 Go finishsweep_m）
    // 确保所有 Region 的 markBitmap 已清零，可安全开始新一轮 Mark
    void ForceFinishSweep();

    // GC 代际翻转：globalSweepGen_ += 2
    // 所有 swept Region 自动变为 unswept，O(1) 无需移动链表
    void PrepareForSweep();

    // 获取所有非空 Region（供 GC 枚举根/标记使用）
    template<typename Visitor>
    void VisitAllRegions(Visitor&& visitor);

    // ===== 监控 =====
    size_t GetTotalPartialSweptRegions() const;
    size_t GetTotalFullSweptRegions() const;
    size_t GetTotalPartialUnsweptRegions() const;
    size_t GetTotalFullUnsweptRegions() const;

private:
    struct SizeClassBucket {
        MutexLock lock;

        // ── 已 sweep（region.sweepGen == globalSweepGen_）──
        RegionList partialSwept;    // 有空洞，可直接用（最快路径）
        RegionList fullSwept;       // 已满，等下轮 GC

        // ── 未 sweep（region.sweepGen == globalSweepGen_ - 2）──
        RegionList partialUnswept;  // 有空洞（上轮分类），需原地 sweep 后才能用
        RegionList fullUnswept;     // 已满（上轮分类），需原地 sweep 后才能用

        // GC 代际切换（零拷贝）：
        //   每轮 GC 完成后 globalSweepGen_ += 2
        //   原来的 swept 自动变成 unswept（sweepGen 不再等于新代数）
        //   无需移动链表节点，O(1) 完成身份翻转

        SizeClassBucket()
            : partialSwept("central-partial-swept"),
              fullSwept("central-full-swept"),
              partialUnswept("central-partial-unswept"),
              fullUnswept("central-full-unswept") {}
    };

    // 全局 sweep 代数，每轮 GC 完成后 +2
    static std::atomic<uint32_t> globalSweepGen_;

    SizeClassBucket buckets_[kNumSizeClasses];

    // 指向 RegionManager（用于 TakeRegion 获取新 Region）
    RegionManager* regionManager_;
};

} // namespace MapleRuntime
#endif // MRT_CENTRAL_REGION_POOL_H
```

### 3.3 核心方法实现

```cpp
// src/Heap/Allocator/CentralRegionPool.cpp

// ===== 核心获取方法（对齐 Go mcentral.cacheSpan 四级优先级）=====

RegionInfo* CentralRegionPool::GetRegion(uint8_t cls) {
    SizeClassBucket& bucket = buckets_[cls];
    uint32_t sg = globalSweepGen_.load(std::memory_order_acquire);

    // ---- 第一优先级：已 sweep 的 partial（最快，无 sweep 开销）----
    {
        LockGuard guard(bucket.lock);
        RegionInfo* region = bucket.partialSwept.TakeHeadRegion();
        if (region != nullptr) {
            region->SetRegionType(RegionType::THREAD_LOCAL_REGION);
            return region;
        }
    }

    // ---- 第二优先级：未 sweep 的 partial（大概率有空洞）----
    for (;;) {
        RegionInfo* region;
        {
            LockGuard guard(bucket.lock);
            region = bucket.partialUnswept.TakeHeadRegion();
        }
        if (region == nullptr) break;

        // CAS 抢占 sweep 权
        if (region->CasSweepGen(sg - 2, sg - 1)) {
            region->SweepRegion();           // 原地 sweep
            region->SetSweepGen(sg);         // 标记已 sweep

            if (region->IsCompletelyFree()) {
                ReleaseRegionInternal(region);
                continue;  // 全死，释放后继续尝试下一个
            }
            region->SetRegionType(RegionType::THREAD_LOCAL_REGION);
            return region;  // 有空洞，直接用 ✅
        }
        // CAS 失败 → 别人在 sweep，跳过
    }

    // ---- 第三优先级：未 sweep 的 full（可能 sweep 后产生空洞）----
    for (;;) {
        RegionInfo* region;
        {
            LockGuard guard(bucket.lock);
            region = bucket.fullUnswept.TakeHeadRegion();
        }
        if (region == nullptr) break;

        if (region->CasSweepGen(sg - 2, sg - 1)) {
            region->SweepRegion();
            region->SetSweepGen(sg);

            if (region->IsCompletelyFree()) {
                ReleaseRegionInternal(region);
                continue;
            }
            if (region->HasFreeSpace()) {
                region->SetRegionType(RegionType::THREAD_LOCAL_REGION);
                return region;  // sweep 后有空洞 ✅
            }
            // 仍然全满 → 放入 fullSwept
            LockGuard guard(bucket.lock);
            bucket.fullSwept.PrependRegion(region);
            continue;
        }
        // CAS 失败 → 跳过
    }

    return nullptr;  // 四链表都没有 → 让调用者请求全新 Region
}

RegionInfo* CentralRegionPool::GetFreshRegion(uint8_t cls) {
    // 通过现有 RegionManager::TakeRegion 获取
    RegionInfo* region = regionManager_->TakeRegion(1 /* nUnit = 默认 */);
    if (region == nullptr) return nullptr;

    // 初始化为 Mark-Sweep Region，sweepGen 设为当前代
    region->InitForSizeClass(cls);
    region->SetSweepGen(globalSweepGen_.load());
    region->SetRegionType(RegionType::THREAD_LOCAL_REGION);
    return region;
}

// ===== 归还方法 =====

void CentralRegionPool::ReturnPartialRegion(RegionInfo* region, uint8_t cls) {
    SizeClassBucket& bucket = buckets_[cls];
    LockGuard guard(bucket.lock);
    // 线程归还的 Region 已在本轮分配过，视为已 swept
    bucket.partialSwept.PrependRegion(region);
}

void CentralRegionPool::ReturnFullRegion(RegionInfo* region, uint8_t cls) {
    SizeClassBucket& bucket = buckets_[cls];
    LockGuard guard(bucket.lock);
    bucket.fullSwept.PrependRegion(region);
}

void CentralRegionPool::ReleaseRegion(RegionInfo* region, uint8_t /* cls */) {
    // 清理 Region 状态，归还给 FreeRegionManager
    region->InitFreeUnits();
    regionManager_->ReturnRegionToFreeManager(region);
}

// ===== GC 操作 =====

// 强制完成上轮所有剩余 sweep（GC 开始前调用）
// 对标 Go runtime/mgc.go 中 gcStart() → finishsweep_m()
void CentralRegionPool::ForceFinishSweep() {
    uint32_t sg = globalSweepGen_.load(std::memory_order_acquire);
    for (uint8_t cls = 0; cls < NUM_SIZE_CLASSES; cls++) {
        auto& bucket = buckets_[cls];
        // sweep 所有 partialUnswept
        while (auto* r = bucket.partialUnswept.Pop()) {
            SweepRegion(r, sg);  // CAS + allocBitmap = ~markBitmap + markBitmap 清零
            if (r->IsEmpty()) {
                ReleaseRegion(r, cls);
            } else if (r->IsFull()) {
                bucket.fullSwept.Push(r);
            } else {
                bucket.partialSwept.Push(r);
            }
        }
        // sweep 所有 fullUnswept
        while (auto* r = bucket.fullUnswept.Pop()) {
            SweepRegion(r, sg);
            if (r->IsEmpty()) {
                ReleaseRegion(r, cls);
            } else if (r->IsFull()) {
                bucket.fullSwept.Push(r);
            } else {
                bucket.partialSwept.Push(r);
            }
        }
    }
}

// GC 代际翻转（标记完成后调用）
void CentralRegionPool::PrepareForSweep() {
    // 原来的 swept（sweepGen == oldGen）自动变成 unswept（sweepGen != newGen）
    globalSweepGen_.fetch_add(2, std::memory_order_release);
    // 无需移动链表节点，O(1) 完成
}
```

### 3.4 Lazy Sweep 策略

核心优化：**不在 GC 结束时集中 sweep 所有 Region**，而是**在分配需要时按需 sweep**（Go 的做法）。四链表结构使得这一策略自然实现：

```
线程需要 class-5 的 Region:
  1. partialSwept[5] 非空 → pop → 直接用（最快，无 sweep 开销）
  2. partialUnswept[5] 非空 → pop → CAS sweepGen → 原地 sweep → 用
  3. fullUnswept[5] 非空 → pop → CAS sweepGen → sweep → 有空洞则用，否则放回 fullSwept
  4. 都没有 → GetFreshRegion(5) → 全新 Region
```

好处：
- **分摊 sweep 开销**：不集中 STW sweep，而是分散到各分配线程
- **冷 size class 延迟清理**：两轮 GC 之间，冷 size class 的 Region 不会在分配热路径上产生 sweep 开销；下轮 GC 开始时由 `ForceFinishSweep()` 统一处理（对标 Go `finishsweep_m`）
- **全程不阻塞分配**：并发标记期间 mutator 正常分配（新对象被 SATB 写屏障隐式标黑，产生浮动垃圾但保证正确性）；GC 结束后也无需 sweep 停顿，分配线程按需 Lazy Sweep
- **O(1) 代际切换**：`globalSweepGen += 2` 即可翻转所有链表身份，无需移动节点

### 3.5 与现有 RegionManager 的关系

```
                           ┌─────── CentralRegionPool ─────────┐
                           │  [cls N] per size class:            │
                           │    partialSwept   │ fullSwept      │
        AllocBufferMS ←──→ │    partialUnswept │ fullUnswept    │
        (per-thread)       │  sweepGen 代数机制实现 O(1) 翻转    │
                           └────────────┬──────────────────────┘
                                        │ GetFreshRegion / ReleaseRegion
                                        ↓
                           ┌──── RegionManager ─────────┐
                           │  FreeRegionManager         │ ← 二级获取
                           │  InactiveZone              │ ← (dirty/released → 虚拟地址扩展)
                           └────────────────────────────┘
```

**CentralRegionPool 是 RegionManager 的上层**：
- 从 RegionManager 获取"原始" Region
- 按 sweepGen 代数 + 空闲状态，将 Region 分入四链表（partialSwept/Unswept、fullSwept/Unswept）
- 向 AllocBufferMS 提供可用 Region，Lazy Sweep 在获取时按需触发

### 3.6 锁粒度与并发

| 操作 | 锁范围 | 争用分析 |
|------|--------|----------|
| FastAlloc（线程本地）| **无锁** | 最常见路径，纯线程本地操作 |
| SlowAlloc → GetRegion | `buckets_[cls].lock` | 按 size class 分桶，争用率低 |
| 原地 Sweep（CAS 抢占）| **无外部锁**（Region 已脱离链表）| CAS sweepGen 保证单线程 sweep |
| ReturnRegion | `buckets_[cls].lock` | 仅在 Region 满/空时触发 |

**CAS 替代自旋**：获取 Region 时遇到 `sweepGen == sg-1`（正在被他人 sweep）直接跳过，不等待。Go 采用相同策略，保证分配路径不会因 sweep 竞争阻塞。

---

---

## 4. Region 生命周期：获取与释放全流程

本节以阶段一（所有分配走运行时接口）为基准，描述一个 Region 从被获取到被释放的完整生命周期。

### 4.1 Region 获取流程（分配路径）

当 `AllocBufferMS` 的某个 size class 对应的 activeRegion 耗尽（bitmap 全 1）或尚未初始化时，触发 Region 获取。

**完整流程图：**

```
AllocBufferMS::Allocate(sizeClass)
│
├─ 1. 在 activeRegion[sizeClass] 的 allocBitmap 中 CTZ 找空闲 slot
│     ├─ 命中 → 置位 bitmap，返回地址 ✅
│     └─ 未命中（bitmap 全 1）→ 进入 RefillRegion
│
└─ 2. RefillRegion(sizeClass)
      │
      ├─ 2a. 归还当前 activeRegion → CentralRegionPool::ReturnPartialRegion()
      │       → 放入 partialSwept[sizeClass] 链表（当前代已 sweep）
      │
      └─ 2b. 向 CentralRegionPool 申请新 Region（GetRegion）
             │
             ├─ ① partialSwept[sizeClass] 非空？
             │     └─ 弹出 → 已 sweep，直接使用 ✅
             │
             ├─ ② partialUnswept[sizeClass] 非空？
             │     └─ 弹出 → CAS sweep → 有空洞则用 ✅
             │
             ├─ ③ fullUnswept[sizeClass] 非空？（Lazy Sweep 触发点）
             │     └─ 弹出 → CAS sweep → 全死则 Release，有空洞则用 ✅
             │
             ├─ ④ FreeRegionManager 有空闲 Region？
             │     └─ 取一个 freeRegion，初始化 MSRegionInfo ✅
             │
             ├─ ⑤ InactiveZone 可扩展？
             │     └─ 向 OS mmap 新页，创建 Region ✅
             │
             └─ ⑥ 全部失败 → 触发 GC
                   │
                   ├─ Mark-Sweep GC（仅标记 + 代际翻转）：
                   │   STW MarkSetup → Concurrent Mark → STW MarkFinish
                   │   → PrepareForSweep(): globalSweepGen_ += 2
                   │   → swept 自动变 unswept，Lazy Sweep 在下次分配时执行
                   │
                   └─ GC 后重试 ①~⑤ → 仍失败则 OOM
```

#### 4.1.1 Region 的 Sweep 状态与获取时的处理

每个 Region 维护一个 `sweepGen` 字段，用于判断是否需要 sweep（对标 Go 的 `mspan.sweepgen`）：

```cpp
// Region sweep 状态（与全局 GC 代数配合）
//   全局 sweepGen_ 每轮 GC 完成后 +2
//
//   region.sweepGen == sweepGen_ - 2  →  未 sweep（上轮 GC 标记完，尚未清扫）
//   region.sweepGen == sweepGen_ - 1  →  正在被某线程 sweep
//   region.sweepGen == sweepGen_      →  已 sweep（本轮已清扫完毕，可直接使用）
```

**获取 Region 时的三种情况及处理：**

```
从 partialUnswept / fullUnswept 弹出一个 Region r（partialSwept 已 sweep，无需处理）：
│
├─ 情况 A：r.sweepGen == sg（已 sweep 过）
│   └─ allocBitmap 已是最新（= ~markBitmap）
│      直接使用，无需额外操作 ✅
│   （注：此情况只会在 partialSwept 路径出现，unswept 链表中不会有）
│
├─ 情况 B：r.sweepGen == sg - 2（未 sweep 过，正常情况）
│   └─ 当前线程负责原地 sweep：
│      1. CAS(r.sweepGen, sg-2, sg-1)  // 标记"正在 sweep"
│         ├─ CAS 成功 → 执行 sweep:
│         │   a. allocBitmap = ~markBitmap（反转标记位→空闲位）
│         │   b. markBitmap 清零
│         │   c. r.sweepGen = sg（标记"已 sweep"）
│         │   d. 检查结果：
│         │      ├─ 全死（allocBitmap 全 0）→ Release → FreeRegionManager，重试下一个
│         │      ├─ 有空洞 → 直接用作 activeRegion ✅
│         │      └─ 全满（allocBitmap 全 1）→ 放回 fullSwept，重试下一个
│         │
│         └─ CAS 失败 → 说明其他线程正在 sweep，跳过此 Region，尝试下一个
│
└─ 情况 C：r.sweepGen == sg - 1（正在被其他线程 sweep）
    └─ 跳过此 Region，不等待，尝试链表中的下一个
       （避免自旋等待，减少线程间竞争）
```

**Go 的对应实现（`mcentral.cacheSpan` 简化）：**

```go
// Go runtime: src/runtime/mcentral.go
func (c *mcentral) cacheSpan() *mspan {
    sg := mheap_.sweepgen

    // 1. 尝试 partial[swept] — 已 sweep，直接用
    if s := c.partialSwept(sg).pop(); s != nil {
        return s
    }

    // 2. 尝试 partial[unswept] — 未 sweep，原地 sweep 后用
    for s := c.partialUnswept(sg).pop(); s != nil; ... {
        if atomic.Cas(&s.sweepgen, sg-2, sg-1) {
            s.sweep(true) // 原地 sweep
            return s
        }
        // CAS 失败 → 别人在 sweep，跳过
    }

    // 3. 尝试 full[unswept] — 未 sweep 的满 span
    for s := c.fullUnswept(sg).pop(); s != nil; ... {
        if atomic.Cas(&s.sweepgen, sg-2, sg-1) {
            s.sweep(true)
            // sweep 后可能变 partial → 可用
            // sweep 后仍 full → 放回 full[swept]，继续
        }
    }

    // 4. 都没有 → 从 mheap 申请新 span
    return c.grow()
}
```

**我们的完整获取顺序（对齐 Go 四级链表设计）：**

```
CentralRegionPool::GetRegion(cls):
│
│  ──── 第一优先级：已 sweep 的 partial（无锁 sweep、有空洞）────
├─ ① partialSwept[cls] 非空？
│     └─ pop → 直接使用 ✅（最快，无 sweep 开销）
│
│  ──── 第二优先级：未 sweep 的 partial（需原地 sweep、大概率有空洞）────
├─ ② partialUnswept[cls] 非空？
│     └─ pop → CAS sweepGen → sweep → 按结果使用/释放
│
│  ──── 第三优先级：未 sweep 的 full（需原地 sweep、可能产生空洞）────
├─ ③ fullUnswept[cls] 非空？
│     └─ pop → CAS sweepGen → sweep
│        ├─ 变 partial → 使用 ✅
│        ├─ 全死 → Release → FreeRegionManager，重试
│        └─ 仍 full → 放入 fullSwept，重试下一个
│
│  ──── 第四优先级：全新 Region ────
├─ ④ FreeRegionManager → 取 freeRegion，初始化 MSRegionInfo ✅
│
├─ ⑤ InactiveZone → mmap 新页 ✅
│
└─ ⑥ 全部失败 → 触发 GC → 重试 ④⑤ → OOM
```

#### 4.1.2 SizeClassBucket 四链表结构（详见 §3.2）

四链表数据结构的完整定义见 [§3.2 SizeClassBucket 数据结构](#32-sizeclassbucket-数据结构)，此处仅强调关键设计原则：

- **GC 代际切换（零拷贝）**：每轮 GC 完成后 `globalSweepGen_ += 2`，原来的 `swept` 自动变成 `unswept`（因为它们的 `sweepGen == oldGen`，而判定标准变成 `newGen`）。无需移动链表节点，O(1) 完成身份翻转。
- **二级获取**：MS 模式下不存在 CopyGC 的 `garbageRegionList`，Region 来源仅有 `FreeRegionManager` 和 `InactiveZone`。
- **partialRegion 优先**：优先复用已有空洞的 Region，减少内存碎片和 Region 总量。
- **Lazy Sweep 分摊**：未 sweep 的 Region 在被获取时由分配线程原地 sweep，分摊 GC 开销到各线程。
- **CAS 无阻塞**：遇到正在被 sweep 的 Region 直接跳过，不自旋等待，避免线程间竞争。
- **Mark-Sweep GC 兜底**：当四链表 + FreeRegionManager + InactiveZone 全部为空时，触发一次完整 Mark-Sweep，回收死对象释放 Region。

### 4.2 Region 释放流程（GC 路径）

Region 的释放由 Mark-Sweep GC 驱动。采用四链表 + sweepGen 机制后，GC 不再集中 sweep，而是**仅做标记 + 代际翻转**，sweep 延迟到分配时按需执行（Lazy Sweep）。

**阶段 1：STW MarkSetup**
```
1. Stop-The-World
2. CentralRegionPool::ForceFinishSweep()
   - 强制 sweep 所有剩余的 unswept Region（冷 size class 等）
   - 确保所有 markBitmap 已清零，可安全开始新一轮 Mark
   - 对标 Go gcStart() → finishsweep_m()
   → sweep 后：全死 Region → FreeRegionManager
              有空洞 → partialSwept，全满 → fullSwept
   → 此时所有四链表中仅剩 *Swept 链表有内容，*Unswept 链表为空
3. 对所有线程调用 AllocBufferMS::FlushAll()
   - 将各 activeRegion 归还 CentralRegionPool
     → 放入 partialSwept 或 fullSwept（因为还在当前代）
   - 清空线程本地缓存，确保所有 Region 对 GC 可见
4. 初始化 Mark 数据结构（灰色队列等）
```

**阶段 2：Concurrent Mark（并发标记）**
```
1. 从 GC Root 出发，遍历对象图
2. 对每个存活对象，在其所在 Region 的 markBitmap 中置位
3. 使用写屏障（SATB）保证并发正确性
```

**阶段 3：STW MarkFinish + 代际翻转**
```
1. 再次 Stop-The-World
2. 处理 SATB 缓冲区残留引用
3. 标记阶段结束，所有存活对象的 markBitmap 已完整
4. 调用 CentralRegionPool::PrepareForSweep()
   → globalSweepGen_ += 2
   → 所有原来的 swept Region 自动变成 unswept
     （因为它们的 sweepGen == oldGen，而判定标准变成 newGen）
   → 无需移动链表节点，O(1) 完成
```

**阶段 4：Lazy Sweep（延迟清扫——由分配触发）**
```
不再有集中的 sweep 阶段。当分配线程从 CentralRegionPool 获取 Region 时：
│
├─ 取到 partialSwept / fullSwept 中的 Region
│   → 此时 sweepGen != newGen → 实际已是 unswept
│   → CAS(sweepGen, newGen-2, newGen-1) → 原地 sweep：
│      a. allocBitmap = ~markBitmap（反转标记位→空闲位）
│      b. markBitmap 清零
│      c. sweepGen = newGen（标记已 sweep）
│      d. 按结果处理：
│         ├─ 全死（allocBitmap 全 0）→ Release → FreeRegionManager
│         ├─ 有空洞 → 用作 activeRegion ✅
│         └─ 全满 → 放回 fullSwept（此时是新代 swept）
│
└─ sweep 开销分摊到各分配线程，无 GC 停顿
```

**与之前集中 sweep 的对比：**

| | 集中 Sweep（传统 MS） | Lazy Sweep（四链表） |
|--|--|--|
| sweep 时机 | GC 结束后批量 | 分配时按需 |
| GC 停顿 | 包含 sweep 时间 | 仅标记，不含 sweep |
| 冷 size class | 与热 class 一起批量 sweep | 两轮 GC 间延迟，下轮 GC 开始时 `ForceFinishSweep()` 兜底 |
| 代际切换 | 遍历移动链表 O(N) | sweepGen += 2，O(1) |

### 4.3 Region 状态机

```
                                  ┌────────────────────────────┐
                                  │                            │
                                  ▼                            │
    ┌──────┐  GetFreshRegion  ┌──────────────┐                 │
    │ FREE │─────────────────▶│ THREAD_LOCAL │                 │
    └──────┘  (初始化          │ (activeRegion│                 │
       ▲      MSRegionInfo,    │  bitmap分配) │                 │
       │      sweepGen=当前代) └──────┬───────┘                 │
       │                              │                        │
       │                  RefillRegion / FlushAll               │
       │                        │           │                  │
       │                        ▼           ▼                  │
       │                ┌────────────┐  ┌───────────┐          │
       │                │ FULL_SWEPT │  │PART_SWEPT │          │
       │                │ (全满+已sw) │  │(有洞+已sw)│          │
       │                └─────┬──────┘  └─────┬─────┘          │
       │                      │               │                │
       │               globalSweepGen += 2（GC 代际翻转）       │
       │                      │               │                │
       │                      ▼               ▼                │
       │              ┌──────────────┐ ┌─────────────┐         │
       │              │FULL_UNSWEPT  │ │PART_UNSWEPT │         │
       │              │(全满+未sw)   │ │(有洞+未sw)  │         │
       │              └──────┬───────┘ └──────┬──────┘         │
       │                     │                │                │
       │                     └───────┬────────┘                │
       │                             │ 分配线程 CAS 抢占 sweep  │
       │                             ▼                         │
       │                       ┌───────────┐                   │
       │  全死(allocBitmap      │  SWEEPING │   有空洞           │
       │  全0) → Release        │(CAS抢占中) │───────────────────┘
       │◀───────────────────── └───────────┘  → THREAD_LOCAL
       │                             │
       │                        仍全满 → FULL_SWEPT
  FreeRegionManager
  (等待下次 GetFreshRegion)
```

**状态说明：**

| 状态 | sweepGen | 含义 | 所在链表 |
|------|----------|------|---------|
| FREE | 无 | 空闲，在 FreeRegionManager 中 | 不在四链表中 |
| THREAD_LOCAL | 当前代 | 正被某线程独占使用 | 不在四链表中（线程本地持有） |
| FULL_SWEPT | 当前代 | 已满、已 sweep | fullSwept |
| PARTIAL_SWEPT | 当前代 | 有空洞、已 sweep | partialSwept |
| FULL_UNSWEPT | 当前代-2 | 已满、未 sweep（上轮残留） | fullUnswept |
| PARTIAL_UNSWEPT | 当前代-2 | 有空洞、未 sweep（上轮残留） | partialUnswept |
| SWEEPING | 当前代-1 | 正在被某线程原地 sweep | 不在链表中（已取出） |

> **注意**：SWEPT ↔ UNSWEPT 的身份切换不是显式的链表移动，而是通过 `globalSweepGen += 2` 隐式实现。翻转后，原来 sweepGen == oldGen 的 Region 自动不再满足 `== newGen` 的判定条件，逻辑上变为 unswept。

### 4.4 典型场景走查

**场景 1：冷启动首次分配**
```
线程 T1 首次分配 64B 对象（sizeClass=4）
→ activeRegion[4] 为空
→ RefillRegion(4):
   partialSwept[4] 为空 → partialUnswept[4] 为空
   → fullUnswept[4] 为空（首次启动无历史 Region）
→ GetFreshRegion(4): FreeRegionManager 取一个 freeRegion
→ 初始化 MSRegionInfo（slotSize=64, slotCount=512, bitmap 全 0, sweepGen=当前代）
→ CTZ(allocBitmap) = slot 0，置位，返回地址
```

**场景 2：热路径连续分配**
```
线程 T1 连续分配 64B 对象
→ activeRegion[4] 的 allocBitmap 逐位置 1
→ 每次 CTZ 找到下一个 0 位（O(1)）
→ 无锁、无系统调用，纯用户态操作
→ 直到 bitmap 全 1 → 触发 RefillRegion
```

**场景 3：Region 耗尽触发 GC**
```
FreeRegionManager 为空，InactiveZone 不可扩展，四链表均为空
→ 触发 GC
→ STW MarkSetup:
   1. ForceFinishSweep() → 强制 sweep 剩余 unswept Region（清 markBitmap）
   2. FlushAll 归还所有 activeRegion → partialSwept/fullSwept
→ Concurrent Mark: 标记存活对象到 markBitmap
→ STW MarkFinish: globalSweepGen += 2
   → 原来 partialSwept/fullSwept 自动变成 partialUnswept/fullUnswept
→ 分配线程重试 GetRegion:
   → 从 partialUnswept 取 Region → CAS sweepGen → 原地 sweep → 有空洞则用 ✅
   → 或从 fullUnswept 取 → sweep 后全死 → Release → FreeRegionManager
   → 再从 FreeRegionManager 获取 ✅
```

**场景 4：Lazy Sweep 复用 unswept partial Region**
```
线程 T1 的 activeRegion[4] 满了
→ RefillRegion(4): 归还到 fullSwept[4]
→ GetRegion(4):
   ① partialSwept[4] 为空
   ② partialUnswept[4] 非空（上轮 GC 后遗留）
   → pop → CAS(sweepGen, sg-2, sg-1) 成功
   → 原地 sweep: allocBitmap = ~markBitmap, markBitmap 清零
   → sweepGen = sg（标记已 sweep）
   → 有空洞 → 直接用作 activeRegion ✅
   → CTZ 找空闲 slot 继续分配
```

**场景 5：分配时遇到正在被 sweep 的 Region**
```
线程 T1 和 T2 同时需要 class-4 的 Region
→ T1: 从 partialUnswept[4] pop Region R1
   → CAS(R1.sweepGen, sg-2, sg-1) 成功 → 开始 sweep
→ T2: 从 partialUnswept[4] pop Region R2
   → R2.sweepGen == sg-2 → CAS 成功 → sweep R2
→ 如果 T2 pop 到的是 R1 且 R1.sweepGen == sg-1（T1 正在 sweep）
   → CAS 失败 → 跳过 R1，尝试下一个
   → 无等待、无阻塞
```

### 4.5 与 Go 的对应关系

| 生命周期阶段 | Go | Cangjie MS |
|-------------|-----|-----------|
| 线程缓存获取 | `mcache.alloc[sizeClass]` → `mspan` | `AllocBufferMS.activeRegion[sizeClass]` → `Region` |
| 中心缓存获取 | `mcentral.uncacheSpan()` → 4 链表 | `CentralRegionPool.GetRegion()` → 4 链表 |
| 页堆获取 | `mheap.alloc()` → `pageAlloc` | `InactiveZone` → `mmap` |
| 线程缓存归还 | `mcache.releaseAll()` → `mcentral` | `AllocBufferMS.FlushAll()` → `CentralRegionPool` |
| GC 清扫 | `sweepone()` → `mspan.sweep()` (Lazy) | Lazy Sweep → 分配时原地 `allocBitmap = ~markBitmap` |
| GC 前强制完成 sweep | `gcStart()` → `finishsweep_m()` | `ForceFinishSweep()` → 遍历所有 *Unswept 链表，确保 markBitmap 清零 |
| 代际翻转 | `mheap.sweepgen += 2` | `PrepareForSweep()` → `globalSweepGen_ += 2` |
| 内存释放 | `mheap.freeSpan()` → `pageAlloc.free()` | `ReleaseRegion()` → `FreeRegionManager` |
| GC 触发 | `gcTrigger` (堆增长比例) | Region 不足时触发 Mark-Sweep GC |

**核心一致性：**
- Go 与 Cangjie 均采用 Lazy Sweep 模型：不在 GC 时集中 sweep，而是分配线程按需 sweep。
- 均使用 `sweepGen` 代际机制 + CAS 实现无阻塞 sweep 所有权争夺。
- Go 有 `scavenge`（归还物理页给 OS）；Cangjie 当前通过 `FreeRegionManager` 管理，可后续增加 scavenge 策略。

---