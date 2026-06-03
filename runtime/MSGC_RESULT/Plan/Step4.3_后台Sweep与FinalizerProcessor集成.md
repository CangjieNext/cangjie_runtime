# Step 4.3：后台 Sweep 与 FinalizerProcessor 集成

> 本文档是 [Step4 并发清扫详细设计](Step4_并发清扫详细设计.md) 的子文档。
> 描述 gc-helper 线程的后台 sweep 循环、Finalizer 执行顺序保证、物理回收路径。
>
> 2026-04-05 当前代码状态校准：
> 1. 当前后台 sweep 由 `MarkSweepCollector::ConcurrentSweep()` 触发 `NotifyToSweep()`，具体工作循环仍在 `FinalizerProcessor::Run()` 中协调，不存在对外暴露的独立 `RunMS()` collector 入口。
> 2. GC 起始前的 sweep 收口当前依赖 `MarkSweepCollector::PreEnumRoots()` 中的 `StopAndWaitForAllSweep()`，不是新增一个常态 `ForceFinishSweep()` 主流程。
> 3. 物理回收当前以 `emptySweptCache`、`TakeEmptySweptRegionForReclaim()` 和后台 scavenge 为中心；文中凡是把“每次 sweep 完 region 立即直接归还 OS”写成既定现状的段落，都应视为设计稿。

---

## 目录

1. [概述](#1-概述)
2. [FinalizerProcessor 现有架构回顾](#2-finalizerprocessor-现有架构回顾)
3. [MS 模式下的 RunMS 主循环](#3-ms-模式下的-runms-主循环)
4. [BackgroundSweepLoop 实现](#4-backgroundsweeploop-实现)
5. [Finalizer 与 Sweep 的顺序保证](#5-finalizer-与-sweep-的顺序保证)
6. [物理回收路径](#6-物理回收路径)
7. [EnsureSweepDone 协调机制](#7-ensureswe​epdone-协调机制)
8. [统计与监控](#8-统计与监控)

---

## 1. 概述

### 1.1 设计目标

后台 Sweep 是并发清扫三路协作（[Step4 §2](Step4_并发清扫详细设计.md)）的**主力路径**，负责在 GC Mark 完成后异步消化绝大部分 Unswept Region，减少分配路径 Lazy Sweep 的触发频率。

后台 Sweep 集成在现有 `FinalizerProcessor`（gc-helper 线程）中，复用其线程和事件循环，无需新增独立线程。

### 1.2 关键约束

| 约束 | 说明 |
|------|------|
| **Finalizer 先于 Sweep** | 带 finalizer 的死对象必须在 sweep 释放内存前执行 finalizer |
| **不阻塞分配** | 后台 Sweep 使用 yield 策略，不持有全局锁 |
| **与 Lazy Sweep 协作** | 通过 CAS sweepGen 保证同一 Region 不被重复 sweep |
| **ForceFinishSweep 兜底** | 后台未完成的由下轮 GC 启动时兜底清扫 |
| **编译隔离** | 全部新增代码在 `#ifdef MRT_USE_MARK_SWEEP` 下 |

### 1.3 核心角色

```
gc-helper 线程 (FinalizerProcessor):
┌─────────────────────────────────────┐
│  RunMS() 主循环                      │
│  ├─ 1. ProcessFinalizables()        │  ← Finalizer 执行
│  ├─ 2. BackgroundSweepLoop()        │  ← 后台 Sweep（新增）
│  ├─ 3. FeedHungryBuffers()          │  ← 喂养空闲 AllocBuffer
│  └─ 4. ReclaimHeapGarbage()         │  ← 物理回收 (madvise)
└─────────────────────────────────────┘
```

---

## 2. FinalizerProcessor 现有架构回顾

### 2.1 现有 CopyGC 下的 gc-helper 线程

当前运行时的 `FinalizerProcessor` 是 gc-helper 线程的核心驱动，位于 `src/Heap/FinalizerProcessor.cpp`。

```cpp
// 现有 CopyGC 模式（简化）
void FinalizerProcessor::Run() {
    while (!shouldStop_) {
        WaitForGCNotification();      // 阻塞等待 GC 完成通知

        ProcessFinalizables();         // 执行 finalizer 回调
        FeedHungryBuffers();           // 将空闲 Region 喂给 AllocBuffer
        ReclaimHeapGarbage();          // madvise(MADV_DONTNEED) 物理回收
    }
}
```

### 2.2 现有职责

| 方法 | 功能 | MS 模式是否保留 |
|------|------|----------------|
| `ProcessFinalizables()` | 遍历 finalizer 队列，执行死对象的 finalizer 回调 | ✅ 保留 |
| `FeedHungryBuffers()` | 将从 GC 回收的 Region 分配给需要的 AllocBuffer | ✅ 改造（从 CentralRegionPool 取） |
| `ReclaimHeapGarbage()` | 对空闲页调用 `madvise(MADV_DONTNEED)` 释放物理内存 | ✅ 保留 |

### 2.3 MS 模式改造点

- **新增 `BackgroundSweepLoop()`**：在 `ProcessFinalizables()` 之后、`FeedHungryBuffers()` 之前插入
- **改造 `FeedHungryBuffers()`**：不再从 GC 直接获取回收 Region，而是从 `CentralRegionPool.partialSwept` 获取
- **改造通知机制**：CopyGC 的 GC 完成通知改为 MS 的 Mark 完成通知

---

## 3. MS 模式下的 RunMS 主循环

### 3.1 主循环设计

```cpp
// src/Heap/FinalizerProcessor.cpp
// #ifdef MRT_USE_MARK_SWEEP

void FinalizerProcessor::RunMS() {
    while (!shouldStop_) {
        // 阻塞等待 Mark 完成通知
        WaitForMarkDone();

        // ━━━ Task 1: Finalizer 执行（必须最先）━━━
        ProcessFinalizables();

        // ━━━ Task 2: 后台 Sweep（新增核心）━━━
        BackgroundSweepLoop();

        // ━━━ Task 3: 喂养 AllocBuffer（可选优化）━━━
        FeedHungryBuffers();

        // ━━━ Task 4: 物理回收━━━
        ReclaimHeapGarbage();

        // 通知 GC 清扫完成（设置 sweepDone_）
        NotifySweepComplete();
    }
}
```

### 3.2 任务执行顺序说明

```
Mark 完成通知
    │
    ▼
┌──────────────────────────────────────────────────────┐
│ 1. ProcessFinalizables()                              │
│    - 必须最先：finalizer 可能复活对象（resurrection） │
│    - 复活的对象在本轮 sweep 中不能被释放              │
│    - 实现：遍历 finalizerQueue_，对每个死对象调用回调 │
├──────────────────────────────────────────────────────┤
│ 2. BackgroundSweepLoop()                              │
│    - 遍历 36 size classes 的 Unswept 链表             │
│    - CAS sweep 每个 Region → bitmap NOT 清扫          │
│    - 全死 Region → Release 到 FreeRegionManager       │
│    - 有空洞 Region → 归还到 partialSwept 链表          │
├──────────────────────────────────────────────────────┤
│ 3. FeedHungryBuffers()                                │
│    - 从 partialSwept 取 Region 喂给等待中的线程       │
│    - MS 模式下此步可能较轻（分配线程已有 Lazy Sweep）  │
├──────────────────────────────────────────────────────┤
│ 4. ReclaimHeapGarbage()                               │
│    - 对 FreeRegionManager 中的空闲 Region 执行         │
│      madvise(MADV_DONTNEED) 释放物理页                │
│    - 降低 RSS 占用                                    │
└──────────────────────────────────────────────────────┘
    │
    ▼
sweepDone_ = true → 允许下一轮 GC 启动
```

### 3.3 WaitForMarkDone 通知机制

```cpp
void FinalizerProcessor::WaitForMarkDone() {
    std::unique_lock<std::mutex> lock(mutex_);
    markDoneCv_.wait(lock, [this] { return markDoneFlag_ || shouldStop_; });
    markDoneFlag_ = false;  // 消费通知
}

// 由 MSCollector::PostMarkTermination() 调用
void FinalizerProcessor::NotifyMarkDone() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        markDoneFlag_ = true;
    }
    markDoneCv_.notify_one();
}
```

---

## 4. BackgroundSweepLoop 实现

### 4.1 核心循环

```cpp
// src/Heap/FinalizerProcessor.cpp
// 对齐 Go runtime/mgcsweep.go: bgsweep() goroutine

void FinalizerProcessor::BackgroundSweepLoop() {
    CentralRegionPool& pool = heapManager_->GetCentralRegionPool();
    uint32_t sg = pool.GetSweepGen();

    uint32_t totalSwept = 0;
    uint32_t totalReleased = 0;

    for (uint8_t cls = 0; cls < kNumSizeClasses; ++cls) {
        // 4.1a: sweep partialUnswept[cls]
        totalSwept += SweepBucketList(pool, cls, /* isPartial */ true, sg);

        // 4.1b: sweep fullUnswept[cls]
        totalSwept += SweepBucketList(pool, cls, /* isPartial */ false, sg);

        // 适时让步，避免长时间占用 CPU
        if (cls % 6 == 5) {
            std::this_thread::yield();
        }
    }

    LOG_MS_SWEEP("BackgroundSweep: swept=%u released=%u", totalSwept, totalReleased);
}
```

### 4.2 单链表 Sweep 逻辑

```cpp
uint32_t FinalizerProcessor::SweepBucketList(
    CentralRegionPool& pool,
    uint8_t cls,
    bool isPartial,
    uint32_t sg)
{
    uint32_t sweptCount = 0;

    for (;;) {
        // 从 Unswept 链表取一个 Region
        RegionInfo* region = pool.PopUnsweptRegion(cls, isPartial);
        if (region == nullptr) break;

        // CAS 抢占 sweep 权
        if (!region->CasSweepGen(sg - 2, sg - 1)) {
            continue;  // 被分配线程 Lazy Sweep 抢走了
        }

        // 执行 bitmap NOT sweep
        region->SweepRegion();
        region->SetSweepGen(sg);
        ++sweptCount;

        // 根据 sweep 结果分流
        if (region->IsCompletelyFree()) {
            pool.ReleaseRegionInternal(region);
        } else if (region->HasFreeSpace()) {
            pool.ReturnPartialSwept(cls, region);
        } else {
            pool.ReturnFullSwept(cls, region);
        }
    }

    return sweptCount;
}
```

### 4.3 与 Lazy Sweep 的竞争处理

后台 Sweep 和分配线程的 Lazy Sweep 可能同时访问同一 sizeClass 的 Unswept 链表：

```
后台线程: PopUnsweptRegion(cls=8, partial) → region_A → CAS(sg-2, sg-1) → sweep
分配线程: PopUnsweptRegion(cls=8, partial) → region_B → CAS(sg-2, sg-1) → sweep
```

**安全性保证**：
1. `PopUnsweptRegion` 在 `buckets_[cls].lock` 保护下取节点（互斥）
2. 不同节点各自 CAS → 无竞争
3. 同一节点不可能被两者同时取到（锁内 pop）

### 4.4 让步策略

```
36 size classes，每 6 个 yield 一次：
cls 0-5:  sweep → sweep → sweep → sweep → sweep → sweep → yield
cls 6-11: sweep → sweep → sweep → sweep → sweep → sweep → yield
...
cls 30-35: sweep → sweep → sweep → sweep → sweep → sweep → yield
```

总计 6 次 yield。每个 sizeClass 内部的多个 Region sweep 是连续的（bitmap NOT 操作极快），不额外让步。

**优化方向**（后续可调整）：
- 根据 pending 分配请求数动态调整让步频率
- 如果分配线程在等待，减少让步；如果 CPU idle，加快 sweep

---

## 5. Finalizer 与 Sweep 的顺序保证

### 5.1 问题定义

Java/C# 等语言的 GC 有明确的 finalizer 语义：**对象被回收前必须执行其 finalizer**。如果 sweep 先于 finalizer 释放了内存，finalizer 访问已释放对象将导致 UAF（Use-After-Free）。

### 5.2 仓颉 Finalizer 机制

仓颉语言通过 `~ClassName()` 析构函数定义 finalizer。运行时在 Mark 阶段识别带 finalizer 的死对象，将其加入 `finalizerQueue_`。

```
Mark 阶段：
  对象 X (带 finalizer) 未被标记 → 加入 finalizerQueue_
  对象 X 被标记为 "finalizer-reachable"（不释放，等待执行）

PostMarkTermination：
  finalizerQueue_ 准备就绪 → 通知 gc-helper 线程
```

### 5.3 顺序保证实现

在 `RunMS()` 中，**Task 1 (ProcessFinalizables) 严格先于 Task 2 (BackgroundSweepLoop)**：

```cpp
void FinalizerProcessor::RunMS() {
    WaitForMarkDone();

    // ═══ 顺序保证点 ═══
    ProcessFinalizables();      // 1. 先执行 finalizer
    // - 执行完毕后，finalizer 复活的对象已安全
    // - 未复活的对象可以被 sweep 释放

    BackgroundSweepLoop();      // 2. 再执行 sweep
    // - 此时所有 finalizer 已执行完毕
    // - sweep 释放的内存不包含待 finalizer 对象
    // ═══════════════════
}
```

### 5.4 Resurrection（复活）处理

Finalizer 执行过程中可能将死对象重新赋值给活引用（复活）：

```
对象 A (finalizer) → 在 finalizer 中：globalRef = this
```

**处理方式**：
- Mark 阶段已将 finalizer-reachable 对象**标记为存活**（markBitmap 中置位）
- 因此 sweep 时 `allocBitmap[i] = ~markBitmap[i]` 不会将其 bit 置 1（不标为可分配）
- 复活对象在下一轮 GC 中按正常存活对象处理

### 5.5 对比 Go 的处理

| 方面 | Go runtime | Cangjie MS |
|------|-----------|------------|
| Finalizer 名称 | `runtime.SetFinalizer` | `~ClassName()` 析构函数 |
| 执行时机 | sweep 后由独立 goroutine 执行 | **sweep 前**由 gc-helper 执行 |
| 复活处理 | 支持（一次复活后不再注册 finalizer） | 支持（标记为存活） |
| 顺序保证 | 弱保证（finalizer goroutine 与 sweep 并发） | **强保证**（串行执行） |

> 我们选择**强顺序保证**（先 finalizer 后 sweep），虽然可能增加 pause-to-reclaim 延迟，但实现简单、语义清晰。

---

## 6. 物理回收路径

### 6.1 回收层次

Region 被 sweep 后有三种归宿，物理回收针对第一种（全死 Region）：

```
SweepRegion() 结果：
├─ 全死 (IsCompletelyFree)
│   └─ ReleaseRegionInternal(region)
│       ├─ 1. ReleaseBitmaps()          释放 bitmap 内存
│       ├─ 2. ResetMetadata()           清空 MSRegionInfo 元数据
│       ├─ 3. FreeRegionManager.Free()  归还到空闲 Region 池
│       └─ 4. 延迟 madvise             在 ReclaimHeapGarbage 中执行
│
├─ 有空洞 (HasFreeSpace)
│   └─ 归还 partialSwept → 下次分配复用（不释放物理内存）
│
└─ 全满 (IsFull)
    └─ 归还 fullSwept → 等待下轮 GC 标记
```

### 6.2 ReleaseRegionInternal 实现

```cpp
// src/Heap/Allocator/CentralRegionPool.cpp

void CentralRegionPool::ReleaseRegionInternal(RegionInfo* region) {
    // 1. 释放 bitmap 内存
    region->ReleaseBitmaps();
    // allocBitmap_ 和 markBitmap_ 是动态分配的
    // ReleaseBitmaps() 调用 free() 释放并置 nullptr

    // 2. 重置元数据
    region->sizeClass_ = 0;
    region->freeSlotCount_ = 0;
    region->liveSlotCount_ = 0;
    region->totalSlotCount_ = 0;
    region->regionType_ = MSRegionType::MS_REGION_FREE;
    region->sweepGen_ = 0;

    // 3. 归还到 FreeRegionManager
    freeRegionManager_->FreeRegion(region);
}
```

### 6.3 madvise 物理回收

`ReclaimHeapGarbage()` 对 `FreeRegionManager` 中的空闲 Region 执行物理内存释放：

```cpp
// src/Heap/FinalizerProcessor.cpp

void FinalizerProcessor::ReclaimHeapGarbage() {
    FreeRegionManager& frm = heapManager_->GetFreeRegionManager();

    // 遍历空闲 Region 列表，对超出保留阈值的 Region 执行 madvise
    size_t freeCount = frm.GetFreeRegionCount();
    size_t reserveCount = frm.GetReserveThreshold();

    if (freeCount <= reserveCount) return;  // 不需要回收

    size_t reclaimCount = freeCount - reserveCount;
    for (size_t i = 0; i < reclaimCount; ++i) {
        RegionInfo* region = frm.PopReclaimCandidate();
        if (region == nullptr) break;

        // 释放物理页，保留虚拟地址空间
        madvise(region->GetBaseAddr(), kRegionSize, MADV_DONTNEED);
        frm.MarkReclaimed(region);
    }
}
```

### 6.4 保留阈值策略

| 参数 | 默认值 | 说明 |
|------|--------|------|
| `reserveThreshold_` | `max(4, heapSize / kRegionSize * 10%)` | 保留一定空闲 Region 避免频繁 mmap |
| 回收时机 | 每轮 GC 后的 `ReclaimHeapGarbage()` 中 | 不在分配路径中 |
| 平台差异 | Linux: `MADV_DONTNEED`, macOS: `MADV_FREE` | macOS 更惰性 |

### 6.5 大对象物理回收

大对象（>16KB）的回收在 [Step2.4 CollectLargeGarbage()](Step2.4_PostMarkTermination_详细设计.md) 中处理：
- Tier1（16KB-4MB）：Region 归还 FreeRegionManager，后续由 `ReclaimHeapGarbage` 执行 madvise
- Tier2（>4MB）：直接 `munmap` 释放虚拟+物理内存

---

## 7. EnsureSweepDone 协调机制

### 7.1 问题场景

分配线程在 GC 频繁时可能遇到后台 Sweep 尚未完成就需要触发新一轮 GC 的情况。此时需要确保上轮 sweep 已全部完成。

### 7.2 实现

```cpp
// src/Heap/Allocator/CentralRegionPool.cpp

void CentralRegionPool::EnsureSweepDone() {
    // 快速路径：后台 Sweep 已完成
    if (sweepDone_.load(std::memory_order_acquire)) {
        return;
    }

    // 慢路径：调用者帮忙完成剩余 sweep
    ForceFinishSweep();
}
```

### 7.3 调用点

```
GC 启动序列（MarkSetup）：
│
├─ EnsureSweepDone()          ← 确保上轮 sweep 完成
│   ├─ sweepDone_ == true → 直接返回（正常路径）
│   └─ sweepDone_ == false → ForceFinishSweep()（帮忙清扫）
│
├─ ResetMarkBitmaps()
├─ PrepareForSweep()          ← globalSweepGen_ += 2
└─ EnableWriteBarrier()
```

### 7.4 与后台 Sweep 的交互

```
场景：GC#1 Mark 完成 → 后台开始 sweep → 后台尚未完成 → GC#2 触发

时间线：
  T0: GC#1 MarkDone → notify gc-helper
  T1: gc-helper ProcessFinalizables()
  T2: gc-helper BackgroundSweepLoop() (sweep cls 0-20...)
  T3: GC#2 触发条件满足 → MarkSetup 调用 EnsureSweepDone()
  T4: sweepDone_ == false → ForceFinishSweep() 排空 cls 21-35
  T5: sweepDone_ = true → 继续 GC#2 Mark
  T6: gc-helper BackgroundSweepLoop() 从 cls 21 继续 → 发现链表为空 → 自然结束
```

**安全性**：ForceFinishSweep 和 BackgroundSweepLoop 通过 CAS sweepGen 协作：
- ForceFinishSweep pop 的 Region，BackgroundSweep 不会再取到
- BackgroundSweep 已 CAS 成功正在 sweep 的 Region，ForceFinishSweep 无法 CAS

### 7.5 sweepDone_ 生命周期

```
PrepareForSweep():
  sweepDone_ = false          ← 新轮开始，标记未完成

BackgroundSweepLoop() 结束后：
  sweepDone_ = true           ← 后台 sweep 完成

EnsureSweepDone() / ForceFinishSweep()：
  sweepDone_ = true           ← 兜底完成
```

---

## 8. 统计与监控

### 8.1 Sweep 统计指标

```cpp
// src/Heap/Allocator/CentralRegionPool.h

struct SweepStats {
    // 后台 Sweep 统计
    uint32_t bgSweptRegions;        // 后台 sweep 的 Region 数
    uint32_t bgReleasedRegions;     // 后台释放的全死 Region 数
    uint64_t bgSweptBytes;          // 后台 sweep 释放的内存字节数
    uint64_t bgSweepTimeNs;        // 后台 sweep 总耗时

    // Lazy Sweep 统计
    uint32_t lazySweptRegions;      // Lazy sweep 的 Region 数
    uint64_t lazySweptBytes;        // Lazy sweep 释放的内存字节数

    // ForceFinish 统计
    uint32_t forceSweptRegions;     // ForceFinish sweep 的 Region 数
    bool forceFinishTriggered;      // 本轮是否触发了 ForceFinish

    // 物理回收统计
    uint32_t reclaimedRegions;      // madvise 回收的 Region 数
    uint64_t reclaimedBytes;        // madvise 回收的字节数
};
```

### 8.2 日志输出

每轮 GC 后输出 sweep 统计摘要：

```
[MS-Sweep] GC#3 Summary:
  Background: 245/280 regions swept (87.5%), 12 released, 1.2ms
  Lazy:       28 regions swept by allocators
  Force:      7 regions swept at MarkSetup
  Reclaim:    12 regions madvised (3.0MB)
  sweepGen:   6 → 8
```

### 8.3 性能监控要点

| 指标 | 健康范围 | 异常信号 |
|------|---------|---------|
| 后台 Sweep 覆盖率 | > 80% | < 50% 说明后台跟不上分配速度 |
| Force Finish 触发 | 极少（< 5%轮次） | 频繁触发说明 GC 间隔太短 |
| Lazy Sweep 频率 | < 20% Region | > 50% 说明后台 Sweep 太慢 |
| Sweep 总耗时 | < 5ms (per GC cycle) | > 20ms 需优化让步策略 |
| madvise 回收量 | 与堆增长匹配 | 持续增长而 RSS 不降说明碎片化 |

### 8.4 调试支持

```cpp
// MRT_DEBUG 模式下额外检查
#ifdef MRT_DEBUG
void CentralRegionPool::VerifySweepInvariant() {
    uint32_t sg = globalSweepGen_.load();
    for (uint8_t cls = 0; cls < kNumSizeClasses; ++cls) {
        SizeClassBucket& bucket = buckets_[cls];

        // 所有 swept 链表中的 Region 必须 sweepGen == sg
        bucket.partialSwept.ForEach([sg](RegionInfo* r) {
            ASSERT(r->GetSweepGen() == sg);
        });
        bucket.fullSwept.ForEach([sg](RegionInfo* r) {
            ASSERT(r->GetSweepGen() == sg);
        });

        // 所有 unswept 链表中的 Region 必须 sweepGen == sg-2
        bucket.partialUnswept.ForEach([sg](RegionInfo* r) {
            ASSERT(r->GetSweepGen() == sg - 2);
        });
        bucket.fullUnswept.ForEach([sg](RegionInfo* r) {
            ASSERT(r->GetSweepGen() == sg - 2);
        });
    }
}
#endif
```
