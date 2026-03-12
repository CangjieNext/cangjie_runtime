# Step 4.2：Sweep 调度与 CentralRegionPool 集成

> 本文档是 [Step4 并发清扫详细设计](Step4_并发清扫详细设计.md) 的子文档。
> 描述 sweepGen 代际机制、Lazy Sweep 在分配路径中的集成、ForceFinishSweep 兜底策略。

---

## 目录

1. [概述](#1-概述)
2. [sweepGen 代际机制](#2-sweepgen-代际机制)
3. [Lazy Sweep 在 GetRegion 中的集成](#3-lazy-sweep-在-getregion-中的集成)
4. [ForceFinishSweep：GC 前兜底](#4-forcefinishsweepgc-前兜底)
5. [O(1) 代际翻转：PrepareForSweep](#5-o1-代际翻转prepareforsweep)
6. [与 AllocBufferMS 分配路径的集成](#6-与-allocbufferms-分配路径的集成)
7. [锁粒度与并发分析](#7-锁粒度与并发分析)
8. [与 Go runtime 的精确对应](#8-与-go-runtime-的精确对应)

---

## 1. 概述

本文档描述 Sweep 操作如何与 [Step1.2](Step1.2_SizeClass与线程本地分配设计.md) 定义的 `CentralRegionPool` 四链表架构集成。核心设计是 **Lazy Sweep**：不在 GC 结束时集中清扫所有 Region，而是在分配需要时按需清扫，将 sweep 开销分摊到各分配线程。

关键组件：
- **`globalSweepGen_`**：全局代际计数器，每轮 GC +2（对标 Go `mheap.sweepgen`）
- **`CentralRegionPool::GetRegion(cls)`**：四级优先级获取 Region，内嵌 Lazy Sweep
- **`ForceFinishSweep()`**：下轮 GC 开始前强制完成所有残余 sweep
- **`PrepareForSweep()`**：`sweepGen += 2`，O(1) 代际翻转

**与旧版 Step4 的关键差异**：旧版使用自定义 `AtomicRegionQueue`（全局+分桶+大对象+pinned 4 种队列）和 `gcEpoch_` (+1/GC)。新版完全复用 Step1.2 已定义的 CentralRegionPool 四链表 + `globalSweepGen_` (+2/GC)，不引入任何新的队列数据结构。

---

## 2. sweepGen 代际机制

### 2.1 设计原理

`globalSweepGen_` 是一个 `std::atomic<uint32_t>` 全局计数器，每轮 GC 完成后 **+2**。+2（而非 +1）的原因是需要区分三种状态：

```
region.sweepGen == globalSweepGen_ - 2   →  未 sweep（需要清扫）
region.sweepGen == globalSweepGen_ - 1   →  正在被某线程 sweep（进行中）
region.sweepGen == globalSweepGen_       →  已 sweep（可直接使用）
```

如果只 +1，就无法区分"未 sweep"和"正在 sweep"，导致无法实现无阻塞 CAS 抢占。

### 2.2 sweepGen 与 Go 的精确对应

```
Go runtime (src/runtime/mheap.go):

    mheap.sweepgen += 2                        // gcStart → finishsweep_m 后

    span.sweepgen == sweepgen - 2  →  需要 sweep
    span.sweepgen == sweepgen - 1  →  正在 sweep
    span.sweepgen == sweepgen      →  已 sweep
    span.sweepgen == sweepgen + 1  →  GC 前已缓存（不需要 sweep）
    span.sweepgen == sweepgen + 3  →  GC 前已缓存且已被 sweep
```

我们简化为前三种状态（不需要 +1/+3 的 cached 状态，因为我们的 `FlushAll()` 在 MarkSetup 时显式归还所有 activeRegion）：

```cpp
// src/Heap/Allocator/CentralRegionPool.h

class CentralRegionPool {
    static std::atomic<uint32_t> globalSweepGen_;  // 全局 sweep 代数

    // 判定 Region 是否需要 sweep
    static bool NeedsSweep(const MSRegionInfo* region) {
        uint32_t sg = globalSweepGen_.load(std::memory_order_acquire);
        return region->GetSweepGen() == sg - 2;
    }

    // CAS 抢占 sweep 所有权
    static bool TryClaimSweep(MSRegionInfo* region) {
        uint32_t sg = globalSweepGen_.load(std::memory_order_acquire);
        return region->CasSweepGen(sg - 2, sg - 1);
    }

    // sweep 完成后标记
    static void MarkSweptDone(MSRegionInfo* region) {
        uint32_t sg = globalSweepGen_.load(std::memory_order_relaxed);
        region->SetSweepGen(sg);
    }
};
```

### 2.3 sweepGen 生命周期

```
时间线:
  初始: globalSweepGen_ = 0

  GC #1 完成:
    PrepareForSweep(): globalSweepGen_ = 2
    所有 Region.sweepGen == 0 → NeedsSweep (0 == 2-2) ✓
    sweep Region R → CAS(0, 1) → SweepRegion() → R.sweepGen = 2

  GC #2 开始前:
    ForceFinishSweep(): sweep 所有 sweepGen < 2 的（冷 class 残余）
  GC #2 完成:
    PrepareForSweep(): globalSweepGen_ = 4
    上轮 swept 的 R.sweepGen == 2 → NeedsSweep (2 == 4-2) ✓
    // 隐式翻转！无需移动链表节点

  GC #3 开始前:
    ForceFinishSweep(): sweep 所有 sweepGen < 4 的
  ...
```

### 2.4 为什么不用 gcEpoch (+1/GC)

旧版 Step4 使用 `gcEpoch_` (+1/GC)：

| 维度 | gcEpoch (+1) | sweepGen (+2) |
|------|-------------|--------------|
| 状态区分 | 只能区分"未 sweep"和"已 sweep" | 可区分"未 sweep"、"正在 sweep"、"已 sweep" |
| CAS 抢占 | 需要额外 `sweepState` 字段辅助 | sweepGen 本身即为 CAS 仲裁器 |
| 实现复杂度 | 双字段（epoch + state）| 单字段 (sweepGen) |
| Go 对齐 | 不对齐 | 完全对齐 Go `mheap.sweepgen` |

**选择 sweepGen (+2/GC)**：单字段实现三态区分，简化并发控制，与 Go 完全对齐。

---

## 3. Lazy Sweep 在 GetRegion 中的集成

### 3.1 四级优先级获取

`CentralRegionPool::GetRegion(cls)` 是分配路径中获取 Region 的核心方法。四级优先级设计直接来自 [Step1.2 §3.3](Step1.2_SizeClass与线程本地分配设计.md)，Lazy Sweep 在第二、三优先级中自然嵌入：

```cpp
// src/Heap/Allocator/CentralRegionPool.cpp
// 对齐 Go runtime/mcentral.go: mcentral.cacheSpan()

RegionInfo* CentralRegionPool::GetRegion(uint8_t cls) {
    SizeClassBucket& bucket = buckets_[cls];
    uint32_t sg = globalSweepGen_.load(std::memory_order_acquire);

    // ━━━ 第一优先级：已 sweep 的 partial（最快，无 sweep 开销）━━━
    {
        LockGuard guard(bucket.lock);
        RegionInfo* region = bucket.partialSwept.TakeHeadRegion();
        if (region != nullptr) {
            region->SetRegionType(RegionType::THREAD_LOCAL_REGION);
            return region;
        }
    }

    // ━━━ 第二优先级：未 sweep 的 partial（Lazy Sweep 触发点 A）━━━
    for (;;) {
        RegionInfo* region;
        {
            LockGuard guard(bucket.lock);
            region = bucket.partialUnswept.TakeHeadRegion();
        }
        if (region == nullptr) break;

        // CAS 抢占 sweep 权
        if (region->CasSweepGen(sg - 2, sg - 1)) {
            region->SweepRegion();               // 原地 bitmap NOT sweep
            region->SetSweepGen(sg);             // 标记已完成

            if (region->IsCompletelyFree()) {
                ReleaseRegionInternal(region);    // 全死 → FreeRegionManager
                continue;                         // 重试下一个
            }
            region->SetRegionType(RegionType::THREAD_LOCAL_REGION);
            return region;                        // 有空洞 → 直接用 ✅
        }
        // CAS 失败 → 别人已抢占，跳过此 Region
    }

    // ━━━ 第三优先级：未 sweep 的 full（Lazy Sweep 触发点 B）━━━
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
                return region;                    // sweep 后有空洞 ✅
            }
            // 仍全满 → 放入 fullSwept
            LockGuard guard(bucket.lock);
            bucket.fullSwept.PrependRegion(region);
            continue;
        }
    }

    // ━━━ 第四优先级：全新 Region ━━━
    return nullptr;  // 调用者 fallback 到 GetFreshRegion(cls)
}
```

### 3.2 四级优先级流程图

```
GetRegion(cls):
│
│  ──── ① partialSwept[cls] ────────────────────────────
├─ pop → 已 sweep + 有空洞 → 直接用作 activeRegion ✅
│        (零 sweep 开销，最优路径)
│
│  ──── ② partialUnswept[cls] ──────────────────────────
├─ pop → CAS(sweepGen, sg-2, sg-1) 
│        ├─ 成功 → SweepRegion() → sweepGen = sg
│        │         ├─ 全死 → Release → FreeRegionManager → 重试下一个
│        │         └─ 有空洞 → 用 ✅
│        └─ 失败 → 跳过(他人在 sweep) → 重试下一个
│
│  ──── ③ fullUnswept[cls] ─────────────────────────────
├─ pop → CAS(sweepGen, sg-2, sg-1)
│        ├─ 成功 → SweepRegion() → sweepGen = sg
│        │         ├─ 全死 → Release → 重试
│        │         ├─ 有空洞 → 用 ✅
│        │         └─ 仍全满 → 放回 fullSwept → 重试
│        └─ 失败 → 跳过 → 重试
│
│  ──── ④ 全部为空 ──────────────────────────────────────
└─ return nullptr → 调用者调用 GetFreshRegion(cls)
   → FreeRegionManager 取一个 freeRegion
   → InitForSizeClass(cls) → sweepGen = 当前代
   → 用 ✅
```

### 3.3 CAS 失败的处理策略

当 `CasSweepGen(sg-2, sg-1)` 失败时，有两种可能：

| sweepGen 值 | 含义 | 处理 |
|------------|------|------|
| `sg - 1` | 其他线程正在 sweep 此 Region | **跳过**，不等待 |
| `sg` | 已被其他线程 sweep 完成 | **跳过**（不应从 Unswept 链表取到已 swept 的） |

Go 采用相同策略：CAS 失败即跳过，不自旋等待，保证分配路径**永不阻塞在 sweep 竞争上**。

### 3.4 Lazy Sweep 对分配延迟的影响

- **最佳情况**：`partialSwept` 非空 → 零 sweep 开销，与普通链表 pop 相同
- **一般情况**：从 `partialUnswept` sweep 一个 Region → 一次 bitmap NOT（~7ns for 512-slot Region）
- **最差情况**：`fullUnswept` 中连续多个全死 Region → Release 后重试 → 延迟可能到 μs 级

后台 sweep（[Step4.3](Step4.3_后台Sweep与FinalizerProcessor集成.md)）的存在使大部分 Region 在分配前已被 sweep，Lazy Sweep 实际触发率预期 < 20%。

---

## 4. ForceFinishSweep：GC 前兜底

### 4.1 设计目标

**核心不变式**：新一轮 GC Mark 开始前，**所有 Region 的 sweepGen 必须等于当前代**（即全部已 sweep），不存在任何 Unswept Region。

对应 Go `runtime/mgc.go: gcStart()` 中的 `finishsweep_m()` 调用。

### 4.2 触发时机

```
GC 周期 N+1 启动序列（MarkSetup）：
┌─────────────────────────────────────────────┐
│ 1. ForceFinishSweep()    ← 最先执行          │
│ 2. ResetMarkBitmaps()                       │
│ 3. globalSweepGen_ += 2  (PrepareForSweep)  │
│ 4. EnableWriteBarrier()                     │
└─────────────────────────────────────────────┘
```

ForceFinishSweep 在 MarkSetup 的**最早期**执行，保证：
- Mark 开始前所有上轮 sweep 残留被清理
- markBitmap 可安全重置（sweep 已全部完成，bitmap 不再被读取）

### 4.3 实现

```cpp
// src/Heap/Allocator/CentralRegionPool.cpp
// 对齐 Go runtime/mgcsweep.go: finishsweep_m()

void CentralRegionPool::ForceFinishSweep() {
    uint32_t sg = globalSweepGen_.load(std::memory_order_acquire);

    for (uint8_t cls = 0; cls < kNumSizeClasses; ++cls) {
        SizeClassBucket& bucket = buckets_[cls];

        // 1. 排空 partialUnswept → sweep → 分流到 partialSwept 或 Release
        DrainAndSweep(bucket, bucket.partialUnswept, sg);

        // 2. 排空 fullUnswept → sweep → 分流到 fullSwept 或 Release
        DrainAndSweep(bucket, bucket.fullUnswept, sg);
    }

    sweepDone_.store(true, std::memory_order_release);
}

// 辅助函数：从 unsweptList 逐个取出并 sweep
void CentralRegionPool::DrainAndSweep(
    SizeClassBucket& bucket,
    RegionList& unsweptList,
    uint32_t sg)
{
    for (;;) {
        RegionInfo* region;
        {
            LockGuard guard(bucket.lock);
            region = unsweptList.TakeHeadRegion();
        }
        if (region == nullptr) break;

        // CAS 抢占 sweep 权（与 Lazy Sweep 统一路径）
        if (region->CasSweepGen(sg - 2, sg - 1)) {
            region->SweepRegion();          // bitmap NOT sweep
            region->SetSweepGen(sg);        // 标记完成

            if (region->IsCompletelyFree()) {
                ReleaseRegionInternal(region);  // 全死 → 归还 FreeRegionManager
                continue;
            }
        }

        // sweep 后的 Region 放回 swept 链表
        LockGuard guard(bucket.lock);
        if (region->HasFreeSpace()) {
            bucket.partialSwept.PrependRegion(region);
        } else {
            bucket.fullSwept.PrependRegion(region);
        }
    }
}
```

### 4.4 三层保底协作

| 机制 | 触发者 | 时机 | 目标 |
|------|--------|------|------|
| **后台 Sweep** | gc-helper 线程 | GC Mark 完成后持续 | 尽量 sweep 完所有 Region（>80%） |
| **Lazy Sweep** | 分配线程 | GetRegion 按需 | sweep 一个即返回 |
| **ForceFinishSweep** | GC 启动 | MarkSetup 最早期 | 排空**全部**残留 Unswept |

正常运行时后台 Sweep 已消化绝大多数 Region，ForceFinishSweep 遍历 36×2 个链表发现全空，开销近零。

---

## 5. O(1) 代际翻转：PrepareForSweep

### 5.1 核心操作

```cpp
// src/Heap/Collector/MSCollector.cpp
// 在 MarkTermination → PostMarkTermination 完成后调用
// 对齐 Go runtime/mgcsweep.go: sweepone() 前的 mheap.sweepgen += 2

void MSCollector::PrepareForSweep() {
    // 一条原子加，所有 swept Region 隐式变为 unswept
    globalSweepGen_.fetch_add(2, std::memory_order_release);

    // 重置 sweep 完成标志
    sweepDone_.store(false, std::memory_order_release);
}
```

### 5.2 为什么是 O(1)

传统方案需要遍历所有 Region 修改状态，复杂度 O(N)。sweepGen +2 方案：

| 操作 | 复杂度 | 说明 |
|------|--------|------|
| `globalSweepGen_ += 2` | **O(1)** | 一条原子操作 |
| 导致的语义变化 | 逻辑 O(N) | 但**延迟到 GetRegion 时逐个检测**，无集中遍历 |

**效果**：加 2 之后，原来 `sweepGen == sg` (已 sweep) 的 Region 变为 `sweepGen == sg_new - 2` (未 sweep)，无需移动链表节点。

### 5.3 链表无需移动的关键

```
GC #N 结束时：
  partialSwept[cls]  → 含若干已 sweep Region (sweepGen = 4)
  fullSwept[cls]     → 含若干已 sweep Region (sweepGen = 4)
  partialUnswept[cls]→ 空（ForceFinishSweep 已清空）
  fullUnswept[cls]   → 空

PrepareForSweep(): globalSweepGen_ = 4 + 2 = 6

GC #N+1 开始时（不移动任何节点，仅语义变化）：
  partialSwept[cls]  → 节点 sweepGen=4，新 sg=6，差值=2 → 语义变为 unswept
  fullSwept[cls]     → 同上
  partialUnswept[cls]→ 空
  fullUnswept[cls]   → 空
```

**但这里有一个问题**：链表名是 `partialSwept`，但节点语义已变为 unswept。需要明确 GetRegion 的逻辑不依赖链表名，而是**以 sweepGen 值判断**：

```cpp
// GetRegion 中判断 Region 是否需要 sweep
bool NeedsSweep(RegionInfo* region, uint32_t currentSG) {
    return region->GetSweepGen() != currentSG;  // sweepGen < currentSG → 需要 sweep
}
```

### 5.4 链表重命名方案

两种实现选择：

**方案 A：物理链表不动，逻辑判别**（推荐 ✅）
- 4 个链表改名为 `listA[cls]` / `listB[cls]` / `listC[cls]` / `listD[cls]`
- GetRegion 遍历时根据 `sweepGen` 值判断每个 Region 的实际状态
- 优点：零开销翻转
- 缺点：遍历链表时可能遇到混合状态节点

**方案 B：O(1) 指针交换**（Go 方案 ✅ 推荐）
- PrepareForSweep 中 `swap(partialSwept, partialUnswept)` + `swap(fullSwept, fullUnswept)`
- 两次指针交换 = O(1)
- ForceFinishSweep 已保证此时 Unswept 链表为空，所以 swap 后：
  - `partialUnswept` = 旧的 `partialSwept`（语义正确：上轮 swept = 本轮 unswept）
  - `partialSwept` = 空（正确：本轮还没 sweep 过任何 Region）

```cpp
void CentralRegionPool::PrepareForSweep() {
    globalSweepGen_.fetch_add(2, std::memory_order_release);
    sweepDone_.store(false, std::memory_order_release);

    for (uint8_t cls = 0; cls < kNumSizeClasses; ++cls) {
        SizeClassBucket& bucket = buckets_[cls];
        LockGuard guard(bucket.lock);

        // ForceFinishSweep 保证 Unswept 链表此时为空
        ASSERT(bucket.partialUnswept.IsEmpty());
        ASSERT(bucket.fullUnswept.IsEmpty());

        bucket.partialSwept.Swap(bucket.partialUnswept);
        bucket.fullSwept.Swap(bucket.fullUnswept);
    }
}
```

> 推荐方案 B：O(1) 指针交换，简单可靠，与 Go `mcentral` 的 `partial/full [2]spanSet` 交替索引等价。

---

## 6. 与 AllocBufferMS 分配路径的集成

### 6.1 分配路径全景

[Step1.2 §2](Step1.2_SizeClass与线程本地分配设计.md) 定义的分配路径，sweep 嵌入点标注如下：

```
应用线程 Allocate(size):
│
├─ 1. 计算 cls = SizeToClass(size)
│
├─ 2. FastAlloc: activeRegions_[cls] 上 bitmap CTZ 分配
│     └─ 成功 → return ptr                       ← 无锁，无 sweep
│
├─ 3. SlowAlloc:
│     ├─ 3a. ReturnRegion(activeRegions_[cls])    ← 归还旧 Region ★
│     │       ├─ 全死 → Release → FreeRegionManager
│     │       ├─ 有空洞 → CentralRegionPool.ReturnPartial(cls, region)
│     │       └─ 全满 → CentralRegionPool.ReturnFull(cls, region)
│     │
│     ├─ 3b. RefillFromCentral:
│     │       └─ CentralRegionPool.GetRegion(cls)   ← Lazy Sweep 触发 ★★
│     │           ├─ 非空 → activeRegions_[cls] = region → FastAlloc
│     │           └─ nullptr ↓
│     │
│     └─ 3c. GetFreshRegion(cls):
│             └─ FreeRegionManager.AllocRegion()
│                 → InitForSizeClass(cls)
│                 → activeRegions_[cls] = region → FastAlloc
│
└─ 4. 全部失败 → TriggerGC / OOM
```

### 6.2 ReturnRegion 归还逻辑

当 `activeRegion` 分配耗尽（bitmap 全零）或切换 sizeClass 时，需要归还 Region：

```cpp
// src/Heap/Allocator/AllocBufferMS.cpp

void AllocBufferMS::ReturnRegion(uint8_t cls) {
    RegionInfo* region = activeRegions_[cls];
    if (region == nullptr) return;

    activeRegions_[cls] = nullptr;

    if (region->IsCompletelyFree()) {
        // 分配后又全部死亡（罕见但可能：分配后立即 GC 标记全死）
        centralPool_->ReleaseRegion(region);
        return;
    }

    if (region->HasFreeSpace()) {
        // 仍有空洞 → 归还到 partialSwept（当前代已在使用 = 已 swept）
        centralPool_->ReturnPartialSwept(cls, region);
    } else {
        // 全满 → 归还到 fullSwept
        centralPool_->ReturnFullSwept(cls, region);
    }
}
```

**关键**：从 `AllocBufferMS` 归还的 Region 其 `sweepGen` 等于当前代（因为是从 GetRegion 获取时已确保 swept），所以归还到 **Swept** 链表。

### 6.3 InitForSizeClass

当 GetFreshRegion 获取到一个全新 freeRegion 时，需要初始化为特定 sizeClass：

```cpp
void MSRegionInfo::InitForSizeClass(uint8_t cls) {
    sizeClass_ = cls;
    slotSize_ = kSizeClassToSize[cls];
    totalSlotCount_ = kRegionSize / slotSize_;

    // 分配 bitmap（allocBitmap 全 1 表示全部可分配）
    AllocBitmaps();
    memset(allocBitmap_, 0xFF, BitmapSizeInBytes());
    memset(markBitmap_, 0x00, BitmapSizeInBytes());

    // 处理尾部多余 bit
    uint32_t tailBits = totalSlotCount_ % 64;
    if (tailBits != 0) {
        allocBitmap_[BitmapSizeInWords() - 1] = (1ULL << tailBits) - 1;
    }

    freeSlotCount_ = totalSlotCount_;
    liveSlotCount_ = 0;
    regionType_ = MSRegionType::MS_REGION_THREAD_LOCAL;
    sweepGen_ = globalSweepGen_.load(std::memory_order_acquire);  // 当前代
}
```

### 6.4 分配路径中的锁竞争分析

| 操作 | 锁 | 竞争频率 |
|------|-----|---------|
| FastAlloc (bitmap CTZ) | **无锁** | 最高频（>95%分配） |
| ReturnRegion | `buckets_[cls].lock` | 低频（Region 耗尽时） |
| GetRegion | `buckets_[cls].lock`（短持有） | 低频 |
| GetFreshRegion | `FreeRegionManager.lock_` | 极低频 |

36 个 sizeClass 各自独立锁，线程间锁竞争被有效隔离。

---

## 7. 锁粒度与并发分析

### 7.1 锁作用域总览

```
┌────────────────────────────────────────────────────────────────┐
│                    CentralRegionPool                           │
│                                                                │
│  buckets_[0].lock   buckets_[1].lock  ...  buckets_[35].lock  │
│   ┌──────────┐       ┌──────────┐          ┌──────────┐       │
│   │partial S │       │partial S │          │partial S │       │
│   │partial U │       │partial U │          │partial U │       │
│   │full S    │       │full S    │          │full S    │       │
│   │full U    │       │full U    │          │full U    │       │
│   └──────────┘       └──────────┘          └──────────┘       │
│                                                                │
│  globalSweepGen_  ← atomic, 无锁                               │
│  sweepDone_       ← atomic, 无锁                               │
└────────────────────────────────────────────────────────────────┘
│
│  FreeRegionManager.lock_  ← 独立锁，仅 GetFreshRegion/Release 时
```

### 7.2 操作 × 锁矩阵

| 操作 | 获取的锁 | 持有时间 | 并发安全机制 |
|------|---------|---------|-------------|
| `FastAlloc` (bitmap CTZ) | **无** | — | 每线程独占 activeRegion |
| `GetRegion(cls)` | `buckets_[cls].lock` | 极短（pop 一个节点） | 仅同 sizeClass 竞争 |
| `GetRegion` 中 `SweepRegion()` | **无外部锁** | ~7-20ns | CAS sweepGen 保护 |
| `ReturnRegion` | `buckets_[cls].lock` | 极短（push 一个节点） | 仅同 sizeClass 竞争 |
| `ForceFinishSweep` | 逐个 `buckets_[cls].lock` | 短（排空一个链表） | STW 或独占期 |
| `PrepareForSweep` | 逐个 `buckets_[cls].lock` | 极短（swap 指针） | STW |
| `GetFreshRegion` | `FreeRegionManager.lock_` | 中等 | 全局一把锁 |
| `ReleaseRegion` | `FreeRegionManager.lock_` | 中等 | 全局一把锁 |

### 7.3 关键并发场景分析

**场景 1：两个线程同时 GetRegion(cls=5)**
```
线程 A: lock(buckets_[5].lock) → pop region_X → unlock → CAS sweep → SweepRegion
线程 B: lock(buckets_[5].lock) → pop region_Y → unlock → CAS sweep → SweepRegion
// 各自 sweep 不同 Region，无竞争
```

**场景 2：分配线程 GetRegion + 后台线程 BackgroundSweep 竞争同一 Region**
```
后台线程: pop region_Z → CAS(sweepGen, sg-2, sg-1) → 成功 → sweep 中...
分配线程: pop region_Z → 不可能（已从链表移除）
// 链表 pop 在锁内，不会取到同一 Region
```

**场景 3：分配线程 Lazy Sweep + ForceFinishSweep 竞争**
```
ForceFinishSweep 在 MarkSetup 中调用（此时处于 STW 或准 STW 阶段）
不会与分配线程的 Lazy Sweep 并发 → 无竞争
```

### 7.4 锁竞争热点与优化

| 热点 | 预期竞争度 | 优化措施 |
|------|-----------|---------|
| `buckets_[cls].lock` | 低：36 个锁分散竞争 | sizeClass 分桶天然隔离 |
| `FreeRegionManager.lock_` | 极低：仅 Region 耗尽时 | 批量预取可进一步降低 |
| CAS sweepGen | 无锁：失败即跳过 | 无自旋，不阻塞 |

**最坏情况估算**：高分配压力下，36 个线程各分配不同 sizeClass — 零锁竞争。极端情况下所有线程集中在同一 sizeClass — `buckets_[cls].lock` 短暂竞争，但持有时间 <100ns，吞吐影响可忽略。

---

## 8. 与 Go runtime 的精确对应

### 8.1 核心组件映射

| Go runtime 组件 | Cangjie MS 对应 | 说明 |
|----------------|-----------------|------|
| `mheap.sweepgen` | `globalSweepGen_` | 全局代际计数器，+2/GC |
| `mspan.sweepgen` | `MSRegionInfo.sweepGen_` | 每 Region 代际标记 |
| `mcentral` | `CentralRegionPool` | 按 sizeClass 管理 Region |
| `mcentral.partial[sweepgen/2%2]` | `buckets_[cls].partialSwept` | 已 sweep 有空洞 |
| `mcentral.full[sweepgen/2%2]` | `buckets_[cls].fullSwept` | 已 sweep 全满 |
| `mcentral.partial[(sweepgen/2+1)%2]` | `buckets_[cls].partialUnswept` | 未 sweep 有空洞 |
| `mcentral.full[(sweepgen/2+1)%2]` | `buckets_[cls].fullUnswept` | 未 sweep 全满 |
| `mcentral.cacheSpan()` | `CentralRegionPool::GetRegion(cls)` | 四级优先级获取 |
| `mcentral.uncacheSpan()` | `AllocBufferMS::ReturnRegion(cls)` | 归还 Region |
| `finishsweep_m()` | `ForceFinishSweep()` | GC 前兜底排空 |
| `mheap.sweepgen += 2` | `PrepareForSweep()` | O(1) 翻转 |
| `mcache.alloc[cls]` | `AllocBufferMS.activeRegions_[cls]` | 线程本地活跃 Region |
| `mcache.refill()` | `AllocBufferMS::RefillFromCentral(cls)` | 耗尽后重填 |

### 8.2 GetRegion vs cacheSpan 详细对比

```go
// Go runtime/mcentral.go
func (c *mcentral) cacheSpan() *mspan {
    sg := mheap_.sweepgen

    // 1. partial[swept] → 直接返回
    // 2. partial[unswept] → sweep → 返回
    // 3. full[unswept] → sweep → 如果有空洞返回
    // 4. 都没有 → mheap_.alloc() 分配新 mspan
}
```

```cpp
// Cangjie CentralRegionPool::GetRegion(cls)
// 与 cacheSpan 逻辑完全对齐，仅以下差异：
// 1. Go 使用 spanSet (可伸缩无锁结构)，我们使用 RegionList + Mutex
// 2. Go 有 5 种 sweepgen 状态，我们简化为 3 种（sg-2/sg-1/sg）
// 3. Go sweep 回收到 mheap，我们回收到 FreeRegionManager
```

### 8.3 简化与差异说明

| 维度 | Go runtime | Cangjie MS | 简化原因 |
|------|-----------|------------|---------|
| spanSet 结构 | `spanSet`（无锁，可伸缩） | `RegionList` + `Mutex` | 初期简单实现，后续可优化 |
| sweep 协助 | `sweepone()` 精确协助 | Lazy Sweep + Background | 仅两层，无比例控制 |
| scavenge | `bgscavenge` 独立 goroutine | 集成在后台 Sweep 中 | 简化架构 |
| sweepgen 状态数 | 5 种（sg-2, sg-1, sg, sg+1, sg+2） | 3 种（sg-2, sg-1, sg） | 无需 sg+1/sg+2 |
| `sweep.npagesInUse` 计数 | 精确追踪 | 暂不实现 | 后续按需添加 |

### 8.4 Go 代码关键引用

供实现时参考的 Go 源码位置：

| 功能 | Go 文件 | 函数 |
|------|---------|------|
| sweep 循环 | `runtime/mgcsweep.go` | `sweepone()` |
| 兜底清扫 | `runtime/mgcsweep.go` | `finishsweep_m()` |
| GC 启动 | `runtime/mgc.go` | `gcStart()` |
| mcentral 获取 | `runtime/mcentral.go` | `cacheSpan()` |
| mcentral 归还 | `runtime/mcentral.go` | `uncacheSpan()` |
| sweepgen 定义 | `runtime/mheap.go` | `mheap.sweepgen` |
