# Step 4：并发清扫（Concurrent Sweep）详细设计

> **前置依赖**: Step 1（分配器改造）、Step 2（三色标记）、Step 3（混合写屏障）
> **目标**: 将 STW Sweep 改为并发执行，Sweep 由后台线程 + 分配时 lazy sweep 协同完成，消除清扫阶段的 STW 停顿。

---

## 目录

1. [现有回收机制深度分析](#1-现有回收机制深度分析)
2. [Go 并发清扫原理](#2-go-并发清扫原理)
3. [从 Copy GC 到 Mark-Sweep 的回收路径变化](#3-从-copy-gc-到-mark-sweep-的回收路径变化)
4. [ConcurrentSweeper 总体设计](#4-concurrentsweeper-总体设计)
5. [SweepOneRegion 详细实现](#5-sweeponeregion-详细实现)
6. [后台 Sweep 线程设计](#6-后台-sweep-线程设计)
7. [Lazy Sweep 与分配路径集成](#7-lazy-sweep-与分配路径集成)
8. [大对象并发 Sweep](#8-大对象并发-sweep)
9. [Pinned 对象 Sweep](#9-pinned-对象-sweep)
10. [gcEpoch 与 bitmap 管理](#10-gcepoch-与-bitmap-管理)
11. [FinalizerProcessor 集成改造](#11-finalizerprocessor-集成改造)
12. [与现有基础设施的复用与改造清单](#12-与现有基础设施的复用与改造清单)
13. [验证方案](#13-验证方案)

---

## 1. 现有回收机制深度分析

### 1.1 当前回收全流程（Copy GC / WCollector）

当前 WCollector 不存在传统意义上的"sweep"。它是 **Copy GC**——将存活对象复制到 to-space，然后释放整个 from-space。完整流程：

```
WCollector::DoGarbageCollection()
  │
  ├─ TraceHeap()                      // 并发标记（SATB）
  │
  ├─ PostTrace()  [STW]               // 标记终止
  │   ├─ ClearWeakRefBuffer()
  │   ├─ ClearSatbBuffer()
  │   ├─ CollectLargeGarbage()        // 遍历 oldLargeRegionList，未标记的→回收
  │   ├─ CollectPinnedGarbage()       // 遍历 oldPinnedRegionList，死亡 slot→free-list
  │   ├─ RefineFromSpace()            // 存活率高的 region 豁免（不 copy）
  │   └─ PrepareForwardTable()        // 建立 from→to 路由表
  │
  ├─ Preforward()  [STW]              // 预转发运行时 root
  │
  ├─ ForwardFromSpace()  [并发]       // 多线程 copy 存活对象到 to-space
  │
  ├─ TransitionToGCPhase(IDLE)
  ├─ FlipTagID()                      // 翻转 tag（用于读屏障判断对象是否已转发）
  │
  ├─ CollectSmallSpace()              // fromRegionList → garbageRegionList
  │   └─ NotifyToReclaimGarbage()     // 通知 gc-helper 异步回收
  │
  └─ UnbindPreviousLiveInfo()         // 清理 LiveInfo
```

### 1.2 Region 回收三阶段

当前 region 从"逻辑死亡"到"物理释放"经过三个阶段：

```
阶段 1: CollectRegion(region)
  └─ region 加入 garbageRegionList（仅改链表指针，O(1)）

阶段 2: ReclaimRegion(region)          [gc-helper 线程异步执行]
  ├─ region.InitFreeUnits()           // 重置 UnitRole = FREE
  └─ freeRegionManager.AddGarbageUnits(idx, num)  // 加入 dirtyUnitTree
     └─ CartesianTree::MergeInsert()  // 自动合并相邻空闲块

阶段 3: ReleaseGarbageRegions(target)  [gc-helper 线程可选执行]
  └─ 从 dirtyUnitTree 根节点（最大块）开始:
     ├─ ReleaseMemory()               // madvise(MADV_DONTNEED) / mmap(MAP_FIXED)
     └─ AddReleaseUnits()             // 加入 releasedUnitTree
     └─ 循环直到 dirty 缓存 ≤ target
```

### 1.3 各类 Region 的现有回收方式

| Region 类型 | 回收方法 | 调用时机 | 粒度 |
|-------------|---------|----------|------|
| **小对象 region** (FROM_REGION) | `CollectFromSpaceGarbage()` | STW 结束后 | 整个 region |
| **大对象 region** (LARGE_REGION) | `CollectLargeGarbage()` | PostTrace STW | 整个 region（检查一个 mark bit） |
| **Pinned region** | `CollectPinnedGarbage()` | PostTrace STW | 对象级（收集 free slot） |
| **Unmovable region** | 转入 from → 豁免 | PostTrace STW | 整个 region |
| **物理回收** | `ReclaimGarbageMemory()` | gc-helper 异步 | Unit 粒度 |

### 1.4 当前方案在 Mark-Sweep 下的不适用性

| 问题 | 说明 |
|------|------|
| **无对象级回收** | Copy GC 释放整个 from-space region，不需要逐对象判断存活；MS 必须逐对象扫描 |
| **无 free-list** | 小对象 region 使用纯 bump-pointer，没有 free-list 支持空洞复用 |
| **回收在 STW 中** | `CollectLargeGarbage` 和 `CollectPinnedGarbage` 在 PostTrace STW 中执行 |
| **bitmap 清理时机** | Copy GC 在 `UnbindPreviousLiveInfo` 中批量清理，MS 需要精确的 epoch 管理 |
| **gc-helper 功能耦合** | `FinalizerProcessor` 同时承担 finalizer 执行、物理回收、预分配三项职责 |

### 1.5 可复用的基础设施

尽管回收逻辑需重写，以下基础设施可直接复用：

| 组件 | 复用点 |
|------|--------|
| **garbageRegionList** | 整个空的 region 仍走此路径 |
| **FreeRegionManager** | dirty 树 / released 树 / CartesianTree 全部复用 |
| **ReclaimRegion / ReleaseRegion** | 空 region 的物理回收不变 |
| **RegionInfo::MarkObject / IsMarkedObject** | mark bitmap 机制直接复用 |
| **GcThreadPool** | 并行 sweep 可复用此线程池 |
| **FinalizerProcessor** | gc-helper 线程框架可复用 |
| **RegionList** | 无锁链表操作可复用 |

---

## 2. Go 并发清扫原理

### 2.1 Go Sweep 的核心设计

Go 1.5+ 的 sweep 是**完全并发**的，与 mutator 同时运行，不造成任何 STW。其设计基于两个关键观察：

1. **Sweep 不需要全局一致视图**：每个 span 独立 sweep，不影响其他 span
2. **Sweep 可以延迟执行**：只要在下一次 GC 开始前完成即可

### 2.2 两种协同机制

#### Background Sweep（后台扫描）

```go
// runtime/mgcsweep.go
func bgsweep() {
    for {
        // 等待 GC 完成信号
        sweep.g.park()
        
        // 循环 sweep 所有 span
        for sweepone() != ^uintptr(0) {
            sweep.nbgsweep++
            // 每 sweep 一定量后让出 CPU
            Gosched()
        }
        
        // 释放不需要的物理内存
        freeToOS()
    }
}
```

特点：
- 运行在独立 goroutine 中
- 每次只 sweep 一个 span，sweep 之间穿插 `Gosched()` 让出 CPU
- 不阻塞任何 mutator goroutine

#### Lazy Sweep（懒清扫）

```go
// runtime/mcache.go  
func (c *mcache) refill(spc spanClass) {
    s := c.alloc[spc]
    // 当前 span 的 free-list 用完了
    // 从 mcentral 获取新 span，获取前会触发 sweep
    s = mheap_.central[spc].mcentral.cacheSpan()
    c.alloc[spc] = s
}

// runtime/mcentral.go
func (c *mcentral) cacheSpan() *mspan {
    // 先从 partial swept 列表取
    // 再从 partial unswept 列表取（sweep 后使用）
    // 再从 full unswept 列表取（sweep 后看有没有空闲）
    // 最后从 mheap 分配新 span
}
```

特点：
- 分配路径中内嵌 sweep 操作
- 保证分配时看到的 span 一定已 sweep
- 每次只 sweep 一个 span，延迟可控

### 2.3 sweepgen 机制

Go 使用 `sweepgen` 代际计数器区分 span 的 sweep 状态：

```
mheap.sweepgen       // 全局代数（每次 GC 后 +2）

span.sweepgen 取值:
  sweepgen - 2  →  "上轮 GC 的，需要 sweep"
  sweepgen - 1  →  "正在被某个 goroutine sweep"
  sweepgen       →  "已 sweep 完成，当前轮可用"
  sweepgen + 1  →  "被缓存且不需要 sweep"（mcache 中）
  sweepgen + 3  →  "标记完成，等待下轮 sweep"
```

CAS `span.sweepgen` 从 `sweepgen-2` 到 `sweepgen-1` 来"认领"一个 span 的 sweep 权。

### 2.4 Go 的 Span Sweep 过程

```go
func (sl *sweepLocked) sweep(preserve bool) bool {
    s := sl.mspan
    
    // 1. 计算 free 对象: mark bitmap XOR alloc bitmap
    //    marked 的是存活，未 marked 的是死亡
    
    // 2. 处理 finalizer 对象
    //    未标记但有 finalizer → 执行 SetFinalizer(nil) 后标记为存活
    //    （finalizer 对象延迟一个周期回收）
    
    // 3. 构建 free-list（或 allocBits）
    //    dead 对象的 slot 变为可分配
    
    // 4. 如果 span 完全为空 → 归还给 mheap
    
    // 5. 更新统计：heapLive -= freedBytes
    
    // 6. span.sweepgen = mheap.sweepgen（标记为已 sweep）
}
```

### 2.5 对 Cangjie Runtime 的启示

| Go 设计 | Cangjie 适配 |
|---------|-------------|
| **span** = 连续 page + size class | **Region** = 连续 Unit + UnitRole |
| **sweepgen** 代际计数 | **gcEpoch** 代际计数（新增） |
| **mcentral** 集中 span 管理 | **CentralRegionPool**（Step1 新增） |
| **mcache** 线程本地缓存 | **AllocBuffer** 线程本地分配（已有） |
| **bgsweep goroutine** | **gc-helper 线程**（FinalizerProcessor 已有） |
| **CAS sweepgen** 认领 sweep | **CAS gcEpoch** 认领 sweep |
| **allocBits / gcmarkBits** 双 bitmap | **RegionBitmap** mark bitmap + sweep 清理 |

### 2.6 与 Step 2 STW Sweep 的关系

Step 2 设计了最简 STW sweep 作为正确性基线。Step 4 将其改造为并发：

| 对比项 | Step 2 STW Sweep | Step 4 Concurrent Sweep |
|--------|-------------------|------------------------|
| 执行时机 | 标记完成后 STW 中 | 标记完成后并发执行 |
| 执行者 | GC 线程单线程 | 后台线程 + 分配路径 lazy |
| region 粒度 | 全部 region 一次 sweep | 逐 region sweep |
| 分配阻塞 | 整个 sweep 完成前不能分配 | 仅 lazy sweep 一个 region |
| STW 时间 | ~全 region 扫描时间 | 0（纯并发） |
| 实现复杂度 | 低 | 中高（需处理竞态） |

---

## 3. 从 Copy GC 到 Mark-Sweep 的回收路径变化

### 3.1 回收路径对比总图

```
========== Copy GC (当前 WCollector) ==========

PostTrace [STW]:
  CollectLargeGarbage()     → 整个 region 级判断
  CollectPinnedGarbage()    → 对象级判断（收集 free slot）
  RefineFromSpace()         → 存活率高的豁免

ForwardFromSpace [并发]:
  ForwardRegion()           → copy 存活对象到 to-space

CollectSmallSpace [STW后]:
  CollectFromSpaceGarbage() → 整个 from-space → garbageRegionList

gc-helper [异步]:
  ReclaimGarbageRegions()   → dirty 树
  ReleaseGarbageRegions()   → released 树


========== Mark-Sweep (新设计) ==========

MarkTermination [STW，极短]:
  ClearWeakRef              → 清理弱引用
  ProcessFinalizers         → 入队 finalizable 对象
  CollectSweepableRegions() → 将所有 region 加入 sweepQueue（仅移链表指针）
  ↑ STW 结束

Background Sweep [并发]:
  BackgroundSweepLoop()
    └─ TakeSweepRegion() → SweepOneRegion()
       ├─ 小对象: 遍历对象构建 free-list
       ├─ 大对象: 检查 1 个 mark bit
       └─ 空 region → garbageRegionList

Lazy Sweep [分配路径]:
  SweepAndGetRegion(sizeClass)
    └─ 认领一个未 sweep 的 region → sweep → 返回给分配器

gc-helper [异步]:
  ReclaimGarbageRegions()   → dirty 树（复用）
  ReleaseGarbageRegions()   → released 树（复用）
```

### 3.2 关键差异

| 维度 | Copy GC | Mark-Sweep |
|------|---------|------------|
| **对象是否移动** | 是（copy 到 to-space） | 否（原地保留） |
| **回收粒度** | 整个 from-space region | 逐对象扫描 |
| **空间利用** | 需要 to-space 预留空间 | 原地回收，不需额外空间 |
| **碎片** | 无（copy 后连续） | 有（free-list 管理碎片） |
| **回收触发** | copy 完成后释放 from-space | sweep 逐 region 建 free-list |
| **STW 内容** | PostTrace + Preforward | 仅 MarkTermination（极短） |
| **并发性** | copy 并发，回收同步 | sweep 完全并发 |

### 3.3 Region 状态机变化

```
====== Copy GC Region 状态机 ======

THREAD_LOCAL → RECENT_FULL → FROM_REGION → [Forward] → GARBAGE → [Reclaim] → FREE
                                    ↓
                          UNMOVABLE_FROM (豁免)

====== Mark-Sweep Region 状态机 ======

THREAD_LOCAL → (满) → FULL_REGION → (GC标记完成) → UNSWEPT_REGION
                                                       ↓
                                              [Sweep: sweep 完成]
                                                       ↓
                                              ┌─ SWEPT_PARTIAL (有空洞)
                                              │     → 返回 CentralPool partial 列表
                                              │     → 分配器从 free-list 分配
                                              ├─ SWEPT_FULL (全部存活)
                                              │     → full 列表等待下次 GC
                                              └─ SWEPT_EMPTY (全部死亡)
                                                    → GARBAGE → [Reclaim] → FREE
```

### 3.4 新增的 RegionType 枚举

```cpp
// MS 模式新增的 RegionType（在 RegionInfo.h 中追加）
#ifdef MRT_USE_MARK_SWEEP
    MS_FULL_REGION,           // 已满，等待 GC
    MS_UNSWEPT_REGION,        // GC 标记完成，等待 sweep
    MS_SWEPT_PARTIAL_REGION,  // sweep 完成，有 free slot
    MS_SWEPT_FULL_REGION,     // sweep 完成，全部存活
    MS_LARGE_UNSWEPT_REGION,  // 大对象，等待 sweep
    MS_LARGE_SWEPT_REGION,    // 大对象，已 sweep 存活
#endif
```

### 3.5 从 garbageRegionList 到 FreeRegionManager 的复用

Mark-Sweep 模式下，sweep 发现的空 region 与 Copy GC 的 from-space region 走**完全相同**的物理回收路径：

```
SweepOneRegion() 发现 liveCount == 0
  │
  └→ ReturnEmptyRegion(region):
       regionManager.CollectRegion(region)     // → garbageRegionList
         │
         └→ [gc-helper 异步]
              regionManager.ReclaimGarbageRegions()  // → dirty 树
              freeRegionManager.ReleaseGarbageRegions(target)  // → released 树
```

这意味着 `FreeRegionManager`、`CartesianTree`、`ReclaimRegion`、`ReleaseRegion` 等基础设施**零改动**。

---

## 4. ConcurrentSweeper 总体设计

### 4.1 类结构概览

```cpp
// src/Heap/Sweeper/ConcurrentSweeper.h

class ConcurrentSweeper {
public:
    explicit ConcurrentSweeper(RegionManager& regionMgr);
    ~ConcurrentSweeper();

    // ====== 生命周期 ======
    
    // GC Mark 完成后调用（在 MarkTermination STW 中）
    // 收集所有需要 sweep 的 region 到 sweepQueue
    void PrepareSweep();
    
    // STW 结束后启动后台 sweep
    void StartBackgroundSweep();
    
    // 等待所有 sweep 完成（下次 GC 启动前调用）
    void EnsureSweepDone();
    
    // 查询状态
    bool IsSweepDone() const;

    // ====== Lazy Sweep（分配路径调用） ======
    
    // 为指定 sizeClass 认领并 sweep 一个 region
    // 返回 sweep 后有 free slot 的 region，或 nullptr
    RegionInfo* SweepAndGetRegion(uint8_t sizeClass);
    
    // ====== 单 Region Sweep ======
    
    // sweep 一个小对象 region：遍历对象，构建 free-list
    SweepResult SweepSmallRegion(RegionInfo* region);
    
    // sweep 一个大对象 region：检查 mark bit
    SweepResult SweepLargeRegion(RegionInfo* region);
    
    // sweep 一个 pinned region：遍历对象，收集 free slot
    SweepResult SweepPinnedRegion(RegionInfo* region);

    // ====== 统计 ======
    
    size_t GetSweptRegionCount() const;
    size_t GetReclaimedBytes() const;
    size_t GetBackgroundSweepCount() const;
    size_t GetLazySweepCount() const;

private:
    RegionManager& regionManager_;
    
    // ====== Sweep 队列 ======
    
    // 全局 sweep 队列（后台线程使用）
    AtomicRegionQueue globalSweepQueue_;
    
    // 按 sizeClass 分桶的 sweep 队列（lazy sweep 快速匹配）
    static constexpr size_t kNumSizeClasses = 68;  // 与 Step1 SizeClassTable 对齐
    AtomicRegionQueue sweepQueuePerClass_[kNumSizeClasses];
    
    // 大对象 sweep 队列
    AtomicRegionQueue largeSweepQueue_;
    
    // Pinned 对象 sweep 队列
    AtomicRegionQueue pinnedSweepQueue_;
    
    // ====== 状态 ======
    
    std::atomic<bool> sweepDone_{true};
    std::atomic<uint32_t> gcEpoch_{0};  // 代际计数，每次 GC +1
    
    // ====== 统计 ======
    
    std::atomic<size_t> sweptRegionCount_{0};
    std::atomic<size_t> reclaimedBytes_{0};
    std::atomic<size_t> bgSweepCount_{0};
    std::atomic<size_t> lazySweepCount_{0};
    
    // ====== 内部方法 ======
    
    void CollectSweepableRegions();
    void BackgroundSweepLoop();
    void ReturnEmptyRegion(RegionInfo* region);
    void ReturnPartialRegion(RegionInfo* region, uint8_t sizeClass);
    void ReturnFullRegion(RegionInfo* region);
};
```

### 4.2 SweepResult 结构

```cpp
struct SweepResult {
    enum Status {
        EMPTY,      // 全部死亡，region 可回收
        PARTIAL,    // 部分存活，有 free-list
        FULL,       // 全部存活，无可用空间
    };
    
    Status status;
    size_t liveCount;       // 存活对象数
    size_t liveBytes;       // 存活字节数
    size_t freeCount;       // 可用 slot 数
    size_t freeBytes;       // 可用字节数
    FreeSlot* freeList;     // 构建的 free-list 头指针（仅 PARTIAL 时有效）
};
```

### 4.3 AtomicRegionQueue 设计

sweep 队列需要支持多线程并发访问（后台线程 + 多个分配线程 lazy sweep），采用无锁单链表：

```cpp
// src/Heap/Sweeper/AtomicRegionQueue.h

class AtomicRegionQueue {
public:
    // 入队（可并发调用）
    void Push(RegionInfo* region) {
        RegionInfo* oldHead;
        do {
            oldHead = head_.load(std::memory_order_relaxed);
            region->SetNextSweepRegion(oldHead);
        } while (!head_.compare_exchange_weak(oldHead, region,
                    std::memory_order_release, std::memory_order_relaxed));
        count_.fetch_add(1, std::memory_order_relaxed);
    }
    
    // 出队（CAS 认领，保证不重复）
    RegionInfo* TryPop() {
        RegionInfo* head;
        do {
            head = head_.load(std::memory_order_acquire);
            if (head == nullptr) return nullptr;
        } while (!head_.compare_exchange_weak(head, head->GetNextSweepRegion(),
                    std::memory_order_release, std::memory_order_relaxed));
        count_.fetch_sub(1, std::memory_order_relaxed);
        head->SetNextSweepRegion(nullptr);
        return head;
    }
    
    // 批量入队（STW 中使用，无竞争）
    void PushBatch(RegionInfo* listHead, RegionInfo* listTail, size_t count) {
        listTail->SetNextSweepRegion(head_.load(std::memory_order_relaxed));
        head_.store(listHead, std::memory_order_release);
        count_.fetch_add(count, std::memory_order_relaxed);
    }
    
    bool IsEmpty() const {
        return head_.load(std::memory_order_acquire) == nullptr;
    }
    
    size_t Count() const {
        return count_.load(std::memory_order_relaxed);
    }

private:
    std::atomic<RegionInfo*> head_{nullptr};
    std::atomic<size_t> count_{0};
};
```

### 4.4 文件结构

```
src/Heap/Sweeper/
├── ConcurrentSweeper.h       // ConcurrentSweeper 类声明
├── ConcurrentSweeper.cpp     // 核心实现（PrepareSweep/StartBackgroundSweep/EnsureSweepDone）
├── SweepRegion.cpp           // SweepSmallRegion/SweepLargeRegion/SweepPinnedRegion 实现
├── AtomicRegionQueue.h       // 无锁 region 队列
└── SweepResult.h             // SweepResult 结构定义
```

### 4.5 与其他组件的交互关系

```
                    ┌─────────────────────┐
                    │  MarkSweepCollector  │
                    │  (Step2 设计)        │
                    └──────┬──────────────┘
                           │ MarkTermination 结束后
                           ▼
                    ┌──────────────────────┐
                    │  ConcurrentSweeper   │
                    │                      │
                    │  PrepareSweep()      │  ← STW 中调用
                    │  StartBackgroundSweep│  ← STW 结束后
                    └──┬──────────────┬────┘
                       │              │
            后台线程    │              │  分配路径
                       ▼              ▼
              BackgroundSweepLoop   SweepAndGetRegion(cls)
                       │              │
                       ▼              ▼
              SweepSmallRegion()   SweepSmallRegion()
                       │              │
              ┌────────┴──────┐       │
              ▼               ▼       ▼
        ReturnEmpty    ReturnPartial → CentralRegionPool
              │                         (Step1 设计)
              ▼
        garbageRegionList → FreeRegionManager（现有）
```

---

## 5. SweepOneRegion 详细实现

### 5.1 小对象 Region Sweep

小对象 region 是 sweep 的核心场景。需要遍历 region 中的每个对象，根据 mark bitmap 判断存活，为死亡对象构建 free-list。

```cpp
// src/Heap/Sweeper/SweepRegion.cpp

SweepResult ConcurrentSweeper::SweepSmallRegion(RegionInfo* region) {
    // 1. 获取 region 基本信息
    uint8_t sizeClass = region->GetSizeClass();
    size_t slotSize = SizeClassTable::ClassToSize(sizeClass);
    
    uintptr_t cursor = region->GetDataStart();    // region 数据起始地址
    uintptr_t allocEnd = region->GetAllocPosition(); // bump pointer 位置（已分配到的位置）
    
    // 2. 初始化统计
    FreeSlot* freeHead = nullptr;
    size_t freeCount = 0;
    size_t freeBytes = 0;
    size_t liveCount = 0;
    size_t liveBytes = 0;
    
    // 3. 遍历所有已分配的 slot
    while (cursor < allocEnd) {
        if (region->IsMarkedObject(cursor)) {
            // 存活对象：保留
            liveCount++;
            liveBytes += slotSize;
        } else {
            // 死亡对象：加入 free-list
            // 注意：finalizer 对象在 MarkTermination 阶段已处理
            
            // 清零对象头（防止悬挂指针被误读）
            // 仅清零 header，不清零整个 slot（lazy 清零在分配时做）
            ObjectHeader* header = reinterpret_cast<ObjectHeader*>(cursor);
            header->Clear();
            
            // 串入 free-list
            FreeSlot* slot = reinterpret_cast<FreeSlot*>(cursor);
            slot->next = freeHead;
            freeHead = slot;
            freeCount++;
            freeBytes += slotSize;
        }
        cursor += slotSize;
    }
    
    // 4. 处理 bump pointer 之后的未分配空间
    uintptr_t regionEnd = region->GetRegionEnd();
    if (allocEnd < regionEnd) {
        // 未分配的空间也串入 free-list
        while (allocEnd + slotSize <= regionEnd) {
            FreeSlot* slot = reinterpret_cast<FreeSlot*>(allocEnd);
            slot->next = freeHead;
            freeHead = slot;
            freeCount++;
            freeBytes += slotSize;
            allocEnd += slotSize;
        }
    }
    
    // 5. 清除 mark bitmap（为下次 GC 准备）
    region->ClearLiveInfo();
    
    // 6. 更新 region 元数据
    region->SetFreeList(freeHead);
    region->SetFreeBytes(freeBytes);
    region->SetLiveByteCount(liveBytes);
    
    // 7. 构造结果
    SweepResult result;
    if (liveCount == 0) {
        result.status = SweepResult::EMPTY;
    } else if (freeCount > 0) {
        result.status = SweepResult::PARTIAL;
    } else {
        result.status = SweepResult::FULL;
    }
    result.liveCount = liveCount;
    result.liveBytes = liveBytes;
    result.freeCount = freeCount;
    result.freeBytes = freeBytes;
    result.freeList = freeHead;
    
    return result;
}
```

### 5.2 FreeSlot 结构

```cpp
// src/Heap/Sweeper/SweepResult.h

// free-list 节点，复用死亡对象的内存空间
struct FreeSlot {
    FreeSlot* next;   // 指向下一个空闲 slot
    // 剩余空间由分配器在 alloc 时覆写
};

static_assert(sizeof(FreeSlot) <= 16, "FreeSlot must fit in minimum object size");
```

### 5.3 与 RegionInfo 既有方法的交互

| 方法调用 | 现有/新增 | 说明 |
|----------|----------|------|
| `region->GetDataStart()` | 新增 | 返回 region 数据区起始地址（跳过 region header） |
| `region->GetAllocPosition()` | 已有 | 即 `allocPtr`，当前 bump pointer 位置 |
| `region->GetRegionEnd()` | 已有 | 即 `regionEnd` |
| `region->IsMarkedObject(addr)` | 已有 | 查询 mark bitmap（RegionInfo L396） |
| `region->ClearLiveInfo()` | 已有 | 清除 liveInfo 和 liveByteCount（RegionInfo L736） |
| `region->GetSizeClass()` | 新增 | MS 模式 region 绑定 sizeClass（Step1） |
| `region->SetFreeList(head)` | 新增 | 设置 free-list 头指针 |
| `region->SetFreeBytes(bytes)` | 新增 | 设置空闲字节数 |

### 5.4 对象遍历的正确性分析

关键问题：**如何知道 region 中每个对象的边界？**

在 Copy GC 中，`VisitAllObjects` 通过 `ObjectHeader::GetSize()` 得到对象大小来步进。但 MS 模式下（Step1 改造后），小对象 region 绑定了 sizeClass，所有 slot 等大，因此：

- **步进方式**: `cursor += slotSize`（固定步长，不依赖对象头）
- **优势**: 不需要读取每个对象的 header，cache-friendly
- **前提**: Step1 分配器改造已保证每个 region 只存放一种 sizeClass

```
Region 内存布局（MS 模式）:

|──── slotSize ────|──── slotSize ────|──── slotSize ────|... 未分配 ...|
|  Obj A (live)    |  Obj B (dead)    |  Obj C (live)    |              |
|  [marked=1]      |  [marked=0]      |  [marked=1]      |              |
                   |  → FreeSlot.next |                  |              |
                   |    ↓             |                  |              |
                   |  freeHead        |                  |              |
```

### 5.5 mark bitmap 清理时机

sweep 时清除 mark bitmap（`ClearLiveInfo`）是安全的，因为：

1. **并发标记已结束**: sweep 发生在标记完成之后
2. **单 region 单 sweep**: CAS 认领保证每个 region 只被一个线程 sweep
3. **分配器不看 bitmap**: 分配器使用 free-list 或 bump pointer，不查 mark bitmap
4. **下次 GC 前必须清零**: 如果不在 sweep 时清，就需要在下次 GC 开始时批量清——sweep 时清更分散，延迟更低

### 5.6 并发安全保证

| 竞态场景 | 解决方案 |
|----------|---------|
| 两个线程同时 sweep 同一 region | CAS 机制保证只有一个成功认领 |
| sweep 线程和 mutator 同时访问 region | sweep 时 region 不在任何分配路径中（未被 AllocBuffer 持有） |
| sweep 和下次 GC 的 mark 重叠 | gcEpoch 保证 sweep 必须全部完成才能开始下次 GC |
| sweep 后 ClearLiveInfo 和并发标记冲突 | 同上，epoch 保护 |

---

## 6. 后台 Sweep 线程设计

### 6.1 后台 Sweep 主循环

```cpp
// src/Heap/Sweeper/ConcurrentSweeper.cpp

void ConcurrentSweeper::BackgroundSweepLoop() {
    while (!shouldExit_) {
        // 1. 等待 GC 完成的唤醒信号
        {
            std::unique_lock<std::mutex> lock(sweepMutex_);
            sweepCV_.wait(lock, [this]() {
                return shouldStartSweep_ || shouldExit_;
            });
            shouldStartSweep_ = false;
        }
        
        if (shouldExit_) break;
        
        // 2. 循环 sweep 全局队列中的 region
        RegionInfo* region;
        while ((region = globalSweepQueue_.TryPop()) != nullptr) {
            DispatchSweep(region);
            bgSweepCount_.fetch_add(1, std::memory_order_relaxed);
        }
        
        // 3. sweep 完所有 sizeClass 分桶队列中剩余的 region
        for (size_t cls = 0; cls < kNumSizeClasses; cls++) {
            while ((region = sweepQueuePerClass_[cls].TryPop()) != nullptr) {
                DispatchSweep(region);
                bgSweepCount_.fetch_add(1, std::memory_order_relaxed);
            }
        }
        
        // 4. sweep 大对象队列
        while ((region = largeSweepQueue_.TryPop()) != nullptr) {
            SweepResult result = SweepLargeRegion(region);
            HandleSweepResult(region, result, 0 /* no sizeClass for large */);
            bgSweepCount_.fetch_add(1, std::memory_order_relaxed);
        }
        
        // 5. sweep pinned 对象队列
        while ((region = pinnedSweepQueue_.TryPop()) != nullptr) {
            SweepResult result = SweepPinnedRegion(region);
            HandleSweepResult(region, result, 0);
            bgSweepCount_.fetch_add(1, std::memory_order_relaxed);
        }
        
        // 6. 标记 sweep 完成
        sweepDone_.store(true, std::memory_order_release);
        
        // 7. 通知等待者（如下一次 GC 的 EnsureSweepDone）
        sweepDoneCV_.notify_all();
    }
}

void ConcurrentSweeper::DispatchSweep(RegionInfo* region) {
    uint8_t sizeClass = region->GetSizeClass();
    SweepResult result = SweepSmallRegion(region);
    HandleSweepResult(region, result, sizeClass);
}

void ConcurrentSweeper::HandleSweepResult(
    RegionInfo* region, const SweepResult& result, uint8_t sizeClass) {
    
    sweptRegionCount_.fetch_add(1, std::memory_order_relaxed);
    reclaimedBytes_.fetch_add(result.freeBytes, std::memory_order_relaxed);
    
    switch (result.status) {
        case SweepResult::EMPTY:
            ReturnEmptyRegion(region);
            break;
        case SweepResult::PARTIAL:
            ReturnPartialRegion(region, sizeClass);
            break;
        case SweepResult::FULL:
            ReturnFullRegion(region);
            break;
    }
}
```

### 6.2 PrepareSweep（在 MarkTermination STW 中调用）

```cpp
void ConcurrentSweeper::PrepareSweep() {
    // 重置统计
    sweptRegionCount_.store(0, std::memory_order_relaxed);
    reclaimedBytes_.store(0, std::memory_order_relaxed);
    bgSweepCount_.store(0, std::memory_order_relaxed);
    lazySweepCount_.store(0, std::memory_order_relaxed);
    
    // 收集所有需要 sweep 的 region
    CollectSweepableRegions();
    
    // 标记 sweep 未完成
    sweepDone_.store(false, std::memory_order_release);
    
    // 递增 gcEpoch
    gcEpoch_.fetch_add(1, std::memory_order_release);
}

void ConcurrentSweeper::CollectSweepableRegions() {
    RegionManager& rm = regionManager_;
    
    // 小对象：fullRegionList 中的 region 按 sizeClass 分桶
    rm.GetFullRegionList().VisitAllRegions([this](RegionInfo* region) {
        uint8_t cls = region->GetSizeClass();
        region->SetRegionType(RegionType::MS_UNSWEPT_REGION);
        sweepQueuePerClass_[cls].Push(region);
    });
    
    // 大对象：oldLargeRegionList
    rm.GetLargeRegionList().VisitAllRegions([this](RegionInfo* region) {
        region->SetRegionType(RegionType::MS_LARGE_UNSWEPT_REGION);
        largeSweepQueue_.Push(region);
    });
    
    // Pinned 对象：oldPinnedRegionList
    rm.GetPinnedRegionList().VisitAllRegions([this](RegionInfo* region) {
        region->SetRegionType(RegionType::MS_UNSWEPT_REGION);
        pinnedSweepQueue_.Push(region);
    });
    
    // 注意：当前正在使用的 tlRegion（线程本地 region）不加入 sweep 队列
    // 它们在分配满后再进入 sweep 流程
}
```

### 6.3 StartBackgroundSweep / EnsureSweepDone

```cpp
void ConcurrentSweeper::StartBackgroundSweep() {
    std::lock_guard<std::mutex> lock(sweepMutex_);
    shouldStartSweep_ = true;
    sweepCV_.notify_one();
}

void ConcurrentSweeper::EnsureSweepDone() {
    if (sweepDone_.load(std::memory_order_acquire)) {
        return;  // 已完成
    }
    
    // 帮助 sweep：当前线程也参与 sweep 加速完成
    RegionInfo* region;
    for (size_t cls = 0; cls < kNumSizeClasses; cls++) {
        while ((region = sweepQueuePerClass_[cls].TryPop()) != nullptr) {
            DispatchSweep(region);
        }
    }
    while ((region = largeSweepQueue_.TryPop()) != nullptr) {
        SweepResult result = SweepLargeRegion(region);
        HandleSweepResult(region, result, 0);
    }
    while ((region = pinnedSweepQueue_.TryPop()) != nullptr) {
        SweepResult result = SweepPinnedRegion(region);
        HandleSweepResult(region, result, 0);
    }
    
    // 等待后台线程也完成
    std::unique_lock<std::mutex> lock(sweepMutex_);
    sweepDoneCV_.wait(lock, [this]() {
        return sweepDone_.load(std::memory_order_acquire);
    });
}
```

### 6.4 后台线程的创建和销毁

后台 sweep 线程复用 FinalizerProcessor 的 gc-helper 线程，不额外创建新线程。详见 §11 FinalizerProcessor 集成改造。

### 6.5 让出 CPU 策略

后台 sweep 线程在连续 sweep 多个 region 后应让出 CPU，避免与 mutator 竞争：

```cpp
// 在 BackgroundSweepLoop 的每 N 个 region 后插入
constexpr size_t kSweepBatchSize = 16;  // 每批 sweep 16 个 region 后让出

size_t batchCount = 0;
while ((region = globalSweepQueue_.TryPop()) != nullptr) {
    DispatchSweep(region);
    if (++batchCount >= kSweepBatchSize) {
        std::this_thread::yield();
        batchCount = 0;
    }
}
```

### 6.6 后台线程与 Lazy Sweep 的协同

```
时间线:
  GC Mark Done (STW)
    │  PrepareSweep(): 收集 region → sweepQueue
    │
    ▼  StartBackgroundSweep()
  ──────────────── STW 结束 ────────────────
    │
    │  后台线程: SweepRegion #1, #2, #3, ...
    │      │
    │      │  同时 Mutator 线程: Allocate()
    │      │     └─ free-list 用完
    │      │        └─ SweepAndGetRegion(cls)  // lazy sweep
    │      │           └─ 从 sweepQueuePerClass_[cls] CAS pop
    │      │              └─ SweepSmallRegion()
    │      │                 └─ 返回有 free slot 的 region
    │      │
    │  后台线程继续 sweep 剩余 region
    │      ...
    │  sweepDone = true
    ▼
  下一次 GC: EnsureSweepDone() 确认
```

关键设计：后台线程和 lazy sweep **共享同一组 queue**，通过 CAS pop 保证不重复 sweep。

---

## 7. Lazy Sweep 与分配路径集成

### 7.1 MS 模式分配路径总览

Step1 设计了 sizeClass 分桶分配。在 Step4 中，分配路径需要与 sweep 集成：

```
AllocateMS(size):
  cls = SizeClassTable::SizeToClass(size)
  
  ① 快速路径: TL free-list
     slot = tlRegionPerClass_[cls].PopFreeSlot()
     if (slot != nullptr) return slot
  
  ② 快速路径: TL bump-pointer
     slot = tlRegionPerClass_[cls].BumpAlloc(slotSize)
     if (slot != nullptr) return slot
  
  ③ 慢路径: 从 CentralRegionPool 获取 region
     region = centralPool_.GetPartialRegion(cls)   // 已 sweep、有 free slot
     if (region != nullptr) {
         tlRegionPerClass_[cls] = region
         return region->PopFreeSlot()
     }
  
  ④ Lazy Sweep 路径: sweep 一个未清扫的 region
     region = sweeper_.SweepAndGetRegion(cls)
     if (region != nullptr) {
         tlRegionPerClass_[cls] = region
         return region->PopFreeSlot()
     }
  
  ⑤ 新 region: 从 RegionManager 分配全新 region
     region = regionManager_.TakeRegion(1, THREAD_LOCAL_REGION, true)
     region->InitForSizeClass(cls)
     tlRegionPerClass_[cls] = region
     return region->BumpAlloc(slotSize)
  
  ⑥ 失败: 触发同步 GC → 重试
```

### 7.2 SweepAndGetRegion 实现

```cpp
RegionInfo* ConcurrentSweeper::SweepAndGetRegion(uint8_t sizeClass) {
    // 从对应 sizeClass 的 sweep 队列中认领一个 region
    RegionInfo* region = sweepQueuePerClass_[sizeClass].TryPop();
    
    if (region == nullptr) {
        // 该 sizeClass 无待 sweep 的 region
        return nullptr;
    }
    
    // 原地 sweep
    SweepResult result = SweepSmallRegion(region);
    lazySweepCount_.fetch_add(1, std::memory_order_relaxed);
    sweptRegionCount_.fetch_add(1, std::memory_order_relaxed);
    reclaimedBytes_.fetch_add(result.freeBytes, std::memory_order_relaxed);
    
    switch (result.status) {
        case SweepResult::PARTIAL:
            // 有 free slot → 直接返回给分配器
            region->SetRegionType(RegionType::MS_SWEPT_PARTIAL_REGION);
            return region;
            
        case SweepResult::EMPTY:
            // 全部死亡 → 回收后继续找下一个
            ReturnEmptyRegion(region);
            return SweepAndGetRegion(sizeClass);  // 递归尝试下一个
            
        case SweepResult::FULL:
            // 全部存活 → 放入 full 列表，继续找
            ReturnFullRegion(region);
            return SweepAndGetRegion(sizeClass);  // 递归尝试下一个
    }
    
    return nullptr;  // unreachable
}
```

### 7.3 递归深度控制

`SweepAndGetRegion` 的递归在极端情况下（连续 FULL/EMPTY region）可能很深。添加深度限制：

```cpp
RegionInfo* ConcurrentSweeper::SweepAndGetRegion(uint8_t sizeClass, size_t maxAttempts) {
    for (size_t attempt = 0; attempt < maxAttempts; attempt++) {
        RegionInfo* region = sweepQueuePerClass_[sizeClass].TryPop();
        if (region == nullptr) return nullptr;
        
        SweepResult result = SweepSmallRegion(region);
        // ... 统计更新 ...
        
        if (result.status == SweepResult::PARTIAL) {
            region->SetRegionType(RegionType::MS_SWEPT_PARTIAL_REGION);
            return region;
        } else if (result.status == SweepResult::EMPTY) {
            ReturnEmptyRegion(region);
        } else {
            ReturnFullRegion(region);
        }
    }
    return nullptr;  // 达到最大尝试次数
}
```

默认 `maxAttempts = 8`，分配路径延迟可控。

### 7.4 分配路径延迟分析

| 步骤 | 延迟 | 说明 |
|------|------|------|
| ① TL free-list pop | ~2ns | 单指针操作 |
| ② TL bump-pointer | ~3ns | 指针加法 + 比较 |
| ③ CentralPool get | ~100ns | 锁竞争 + 链表操作 |
| ④ Lazy sweep 1 region | ~1-10μs | 取决于 region 大小和存活率 |
| ⑤ TakeRegion | ~1-5μs | FreeRegionManager 树操作 |
| ⑥ 同步 GC | ~毫秒级 | 极端情况 |

**关键保证**: lazy sweep 每次只处理一个 region（最多 maxAttempts 个），分配延迟可预测。

### 7.5 与 CentralRegionPool 的交互

Step1 设计了 `CentralRegionPool` 管理已 sweep 有空洞的 region。sweep 完成后的 PARTIAL region 流入此 pool：

```
ConcurrentSweeper
  │ ReturnPartialRegion(region, cls)
  │
  ▼
CentralRegionPool
  │ partialRegions_[cls].Push(region)
  │
  ▼
AllocBuffer::AllocateImpl()
  │ centralPool_.GetPartialRegion(cls)
  └─ 从 partialRegions_[cls] pop，设为新的 tlRegion
```

### 7.6 线程本地 Region 的 Sweep 时机

当前正在使用的 `tlRegion`（线程本地 region）**不参与当轮 sweep**。处理策略：

1. **分配满时**：tlRegion 满后移入 fullRegionList，下次 GC 时进入 sweep 流程
2. **GC 开始时**：MarkTermination STW 中，回收所有 mutator 的 tlRegion → fullRegionList
3. **Mutator 退出时**：线程退出时归还 tlRegion

```cpp
// MarkTermination STW 中
void MarkSweepCollector::RetireAllTLRegions() {
    mutatorManager_.ForEachMutator([](Mutator* mut) {
        for (size_t cls = 0; cls < kNumSizeClasses; cls++) {
            RegionInfo* tlRegion = mut->allocBuffer_.RetireTLRegion(cls);
            if (tlRegion != nullptr) {
                regionManager_.AddToFullRegionList(tlRegion);
            }
        }
    });
}
```

---

## 8. 大对象并发 Sweep

### 8.1 大对象特点

大对象（≥ `LARGE_OBJECT_DEFAULT_THRESHOLD`，通常 32KB）独占一个或多个连续 Unit，特点：

- **整个 region 只有一个对象**
- **只需检查一个 mark bit**（不需遍历）
- **空间大**：回收后释放大量物理内存
- **数量少**：通常占总对象数 < 1%

### 8.2 SweepLargeRegion 实现

```cpp
SweepResult ConcurrentSweeper::SweepLargeRegion(RegionInfo* region) {
    SweepResult result;
    
    // 大对象 mark bit 在 region 的 isMarked 标志位上
    if (region->IsMarkedObject(region->GetDataStart())) {
        // 存活
        result.status = SweepResult::FULL;
        result.liveCount = 1;
        result.liveBytes = region->GetAllocatedSize();
        result.freeCount = 0;
        result.freeBytes = 0;
        result.freeList = nullptr;
    } else {
        // 死亡 → 整个 region 回收
        result.status = SweepResult::EMPTY;
        result.liveCount = 0;
        result.liveBytes = 0;
        result.freeCount = 0;
        result.freeBytes = region->GetAllocatedSize();
        result.freeList = nullptr;
    }
    
    // 清除 mark bit
    region->ResetMarkBit();
    
    return result;
}
```

### 8.3 大对象回收策略

根据大对象大小采取不同回收策略（与现有 `CollectLargeGarbage` 逻辑一致）：

```cpp
void ConcurrentSweeper::HandleLargeEmptyRegion(RegionInfo* region) {
    size_t regionSize = region->GetAllocatedSize();
    
    if (regionSize > LARGE_OBJECT_RELEASE_THRESHOLD) {
        // > 4MB: 立即释放物理内存（madvise/mmap）
        regionManager_.ReleaseRegion(region);
    } else {
        // ≤ 4MB: 加入 garbageRegionList，由 gc-helper 异步回收
        regionManager_.CollectRegion(region);
    }
}
```

### 8.4 大对象存活处理

存活的大对象保留在 `oldLargeRegionList` 中，下次 GC 时重新进入大对象 sweep 队列。

```cpp
void ConcurrentSweeper::ReturnLargeLiveRegion(RegionInfo* region) {
    region->SetRegionType(RegionType::MS_LARGE_SWEPT_REGION);
    regionManager_.GetLargeRegionList().PrependRegion(region, 
        RegionType::MS_LARGE_SWEPT_REGION);
}
```

### 8.5 大对象 Sweep 的并发安全

大对象 sweep 无特殊竞态问题：
- 每个大对象独占 region，CAS pop 保证不重复
- 大对象不参与 sizeClass 分桶，走独立队列
- 物理释放（`ReleaseRegion`）是 region 粒度的原子操作

---

## 9. Pinned 对象 Sweep

### 9.1 Pinned 对象的特殊性

Pinned 对象是不能被 GC 移动的对象（如被 FFI 传递给 C 代码的对象）。在 Copy GC 中用 `FULL_PINNED_REGION` 标记。在 MS 模式中：

- **所有对象都不移动**（MS 本身就是 non-moving）
- Pinned 属性的意义弱化，但仍需保留（用于 FFI 生命周期管理）
- Pinned region 可能混合存放不同大小的对象（与 sizeClass region 不同）

### 9.2 SweepPinnedRegion 实现

Pinned region 内的对象大小不统一，需要通过 `ObjectHeader::GetSize()` 步进：

```cpp
SweepResult ConcurrentSweeper::SweepPinnedRegion(RegionInfo* region) {
    uintptr_t cursor = region->GetDataStart();
    uintptr_t allocEnd = region->GetAllocPosition();
    
    FreeSlot* freeHead = nullptr;
    size_t freeCount = 0;
    size_t freeBytes = 0;
    size_t liveCount = 0;
    size_t liveBytes = 0;
    
    while (cursor < allocEnd) {
        ObjectHeader* obj = reinterpret_cast<ObjectHeader*>(cursor);
        size_t objSize = obj->GetAlignedSize();  // 对齐后的大小
        
        if (region->IsMarkedObject(cursor)) {
            // 存活
            liveCount++;
            liveBytes += objSize;
        } else {
            // 死亡 → 仅当 objSize >= sizeof(FreeSlot) 时可复用
            // pinned slot 需要对齐到 SlotList 的要求
            obj->Clear();
            
            if (objSize >= sizeof(FreeSlot)) {
                FreeSlot* slot = reinterpret_cast<FreeSlot*>(cursor);
                slot->next = freeHead;
                freeHead = slot;
                freeCount++;
                freeBytes += objSize;
            }
            // objSize 太小的空间丢弃（碎片损耗）
        }
        
        cursor += objSize;
    }
    
    // 清除 mark bitmap
    region->ClearLiveInfo();
    
    // 构造结果
    SweepResult result;
    if (liveCount == 0) {
        result.status = SweepResult::EMPTY;
    } else if (freeCount > 0) {
        result.status = SweepResult::PARTIAL;
    } else {
        result.status = SweepResult::FULL;
    }
    result.liveCount = liveCount;
    result.liveBytes = liveBytes;
    result.freeCount = freeCount;
    result.freeBytes = freeBytes;
    result.freeList = freeHead;
    
    return result;
}
```

### 9.3 与现有 FreePinnedSlotLists 的关系

当前 `CollectPinnedGarbage` 将死亡 pinned slot 收集到 `FreePinnedSlotLists`：

```cpp
struct FreePinnedSlotLists {
    SlotList freeAtomicSlotList;   // 16B 对象
    SlotList freeSyncSlotList;     // SYNC 对象
};
```

MS 模式下可简化：
- **短期**: 复用 `FreePinnedSlotLists` 收集死亡 slot，保持与现有 pinned 分配器兼容
- **长期**: 随着 MS 使 pinned 属性弱化，可考虑统一 pinned 和普通对象的分配路径

### 9.4 Pinned Region 回收后的归属

| Sweep 结果 | 处理 |
|-----------|------|
| EMPTY | `CollectRegion` → garbageRegionList → dirty 树（同普通 region） |
| PARTIAL | 返回 `FreePinnedSlotLists`，region 保留在 pinnedRegionList |
| FULL | region 保留在 pinnedRegionList，等待下次 GC |

---

## 10. gcEpoch 与 bitmap 管理

### 10.1 gcEpoch 代际计数器

`gcEpoch` 是一个全局单调递增的计数器，每次 GC 周期 +1。它解决两个核心问题：

1. **Sweep 完成性保证**：下次 GC 开始前必须确保上一轮 sweep 全部完成
2. **Region sweep 状态判断**：通过比较 region 的 sweepEpoch 与全局 gcEpoch 判断是否需要 sweep

```cpp
// 全局状态（在 ConcurrentSweeper 中）
std::atomic<uint32_t> gcEpoch_{0};

// 每个 Region 的状态（在 RegionInfo / UnitMetadata 中新增）
uint32_t sweepEpoch_;  // 该 region 最后被 sweep 的 epoch
```

### 10.2 Region Sweep 状态判断

```cpp
// 判断 region 是否需要 sweep
bool NeedsSweep(RegionInfo* region) const {
    return region->GetSweepEpoch() < gcEpoch_.load(std::memory_order_acquire);
}

// sweep 完成后更新 epoch
void MarkSwept(RegionInfo* region) {
    region->SetSweepEpoch(gcEpoch_.load(std::memory_order_relaxed));
}
```

### 10.3 gcEpoch 与 GC 周期的对应关系

```
时间线:
  gcEpoch = 0  [初始]
      ↓
  GC #1 开始
      MarkTermination: gcEpoch = 1, 所有 region 的 sweepEpoch 仍为 0
      Sweep 开始: region.sweepEpoch < 1 → 需要 sweep
      Sweep 完成: region.sweepEpoch = 1
      ↓
  GC #2 开始
      EnsureSweepDone(): 等待 gcEpoch=1 的 sweep 全部完成
      MarkTermination: gcEpoch = 2
      ↓
  ...
```

### 10.4 CAS 认领 Sweep（替代 sweepQueuePerClass 的方案）

除了使用 AtomicRegionQueue，也可以用 sweepEpoch CAS 来认领 region 的 sweep 权：

```cpp
// CAS 认领方式（Go 风格）
bool TryClaim(RegionInfo* region) {
    uint32_t expected = gcEpoch_.load() - 1;  // 未 sweep 的 epoch
    uint32_t sweeping = gcEpoch_.load();       // sweep 中的临时值
    return region->CasSweepEpoch(expected, sweeping);
}
```

**设计选择**：本方案采用 AtomicRegionQueue（§4.3），原因：
- 队列 pop 操作更省 CPU（不需要遍历链表找未 sweep 的）
- 按 sizeClass 分桶，lazy sweep 匹配更快
- 实现更简洁

### 10.5 Mark Bitmap 管理

#### 当前 mark bitmap 机制

```
RegionInfo (UnitMetadata)
  └─ liveInfo: LiveInfo* (union with ownerRegion)
       └─ markBitmap: RegionBitmap*
            └─ 按 bit 记录对象存活状态
```

- `MarkObject(addr)`: 设置 bitmap 中对应 bit（RegionInfo L322）
- `IsMarkedObject(addr)`: 查询 bitmap（RegionInfo L396）
- `ClearLiveInfo()`: 释放 bitmap 指针（RegionInfo L736）

#### MS 模式的 bitmap 生命周期

```
GC 周期开始:
  1. 标记开始前: bitmap 可能已在上轮 sweep 中清零
     - 已 sweep 的 region: bitmap 已清零 ✓
     - 新分配的 region: 无 bitmap ✓
     - 注意: 不需要全局批量清零（分散在 sweep 中完成）
  
  2. 并发标记: MarkObject 按需分配 bitmap + 设 bit
  
  3. 标记结束: bitmap 记录存活状态
  
  4. Sweep: 读 bitmap 判断存活 → 构建 free-list → ClearLiveInfo 清零
```

#### 优化：避免 bitmap 反复分配/释放

当前 `ClearLiveInfo` 会释放 bitmap 内存。在 MS 模式下，bitmap 每轮 GC 都需要，频繁分配/释放浪费。优化方案：

```cpp
// 方案 A: ClearLiveInfo 只清零 bit，不释放内存
void ClearLiveInfoMS() {
    if (liveInfo_ != nullptr) {
        liveInfo_->ClearBitmap();  // memset 0，保留分配
    }
    liveByteCount_ = 0;
}

// 方案 B: 使用 epoch 替代清零
// bitmap 不清零，在 IsMarkedObject 中加入 epoch 检查
bool IsMarkedForCurrentEpoch(uintptr_t addr) {
    if (sweepEpoch_ < globalGcEpoch) {
        return false;  // 上轮的 mark 不算
    }
    return bitmap_->IsSet(addr);
}
// 此方案免去清零开销，但增加 IsMarkedObject 的判断成本
```

**推荐方案 A**：清零 bitmap 但不释放内存。原因：
- 实现简单，语义清晰
- memset 在现代 CPU 上非常快（一个 region 的 bitmap 通常 < 1KB）
- 避免 CAS 分配 bitmap 的开销（当前实现用 CAS 懒分配）

### 10.6 大对象的 mark 位

大对象使用 `RegionInfo::isMarked` 位域字段（不使用 bitmap），sweep 时调用 `ResetMarkBit()` 清零即可，无额外复杂度。

---

## 11. FinalizerProcessor 集成改造

### 11.1 当前 FinalizerProcessor 架构

`FinalizerProcessor` 运行在 `gc-helper` 线程上，当前承担三项职责：

```
FinalizerProcessor::Run() 主循环:
  ① ProcessFinalizables()        // 执行已入队的 finalizer
  ② FeedHungryBuffers()          // 预分配 region 给饥饿的 AllocBuffer
  ③ ReclaimHeapGarbage()         // 物理回收 garbageRegionList
  → Wait()                       // 等待下一次唤醒
```

关键接口（FinalizerProcessor.h）：
- `EnqueueFinalizables(predicate)`: GC 调用，将未标记的 finalizable 对象入队
- `NotifyToReclaimGarbage()`: 设标记 + 唤醒
- `NotifyToFeedAllocBuffers()`: 设标记 + 唤醒
- `ProcessFinalizables()`: swap + 逐个执行 finalize

### 11.2 MS 模式下的改造

在 MS 模式下，FinalizerProcessor 需要新增后台 sweep 职责：

```
FinalizerProcessor::RunMS() 主循环:    // MS 模式新增
  ① ProcessFinalizables()              // 不变
  ② BackgroundSweep()                  // 新增: 调用 ConcurrentSweeper
  ③ FeedHungryBuffers()                // 不变
  ④ ReclaimHeapGarbage()               // 不变（sweep 产生的空 region 仍走此路径）
  → Wait()
```

### 11.3 Finalizer 与 Sweep 的执行顺序

**关键约束**: Finalizer 必须在 sweep 之前执行。

原因：
1. `MarkTermination` 阶段将未标记的 finalizable 对象标记为存活（resurrection）
2. Finalizer 执行时需要访问对象内容
3. sweep 会清零死亡对象的 header，如果先 sweep 再执行 finalizer → 访问已清零的对象 → 错误

执行顺序保证：

```
MarkTermination [STW]:
  EnqueueFinalizables()          // 未标记但有 finalizer 的对象入队
                                  // 这些对象被标记为 resurrected（不被 sweep 回收）
  PrepareSweep()                 // 收集 sweepable region

STW 结束 → gc-helper 线程唤醒:
  ① ProcessFinalizables()         // 先执行 finalizer
     - finalizable 对象已标为 resurrected，不会被 sweep 回收
     - 执行后将这些对象的 resurrected 标记清除
     - 下轮 GC 如果不再可达，才会被真正回收
  ② BackgroundSweep()             // 再 sweep
     - resurrected 对象不会被当作死亡对象
  ③ FeedHungryBuffers()
  ④ ReclaimHeapGarbage()
```

### 11.4 改造后的 FinalizerProcessor

```cpp
// src/Heap/Collector/FinalizerProcessor.cpp (MS 模式部分)

void FinalizerProcessor::RunMS() {
    while (!shouldStop_) {
        // 等待唤醒
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [this]() {
                return shouldProcessFinalizers_ || shouldSweep_ || 
                       shouldReclaimHeapGarbage_ || shouldFeedBuffers_ || shouldStop_;
            });
        }
        
        if (shouldStop_) break;
        
        // 1. 最优先：执行 finalizer
        if (shouldProcessFinalizers_.exchange(false)) {
            ProcessFinalizables();
        }
        
        // 2. 后台 sweep
        if (shouldSweep_.exchange(false)) {
            sweeper_->BackgroundSweepLoop();
        }
        
        // 3. 预分配
        if (shouldFeedBuffers_.exchange(false)) {
            allocator_->FeedHungryBuffers();
        }
        
        // 4. 物理回收
        if (shouldReclaimHeapGarbage_.exchange(false)) {
            allocator_->ReclaimGarbageMemory(false);
        }
    }
}
```

### 11.5 NotifySweep 接口

```cpp
void FinalizerProcessor::NotifyToSweep() {
    shouldSweep_.store(true, std::memory_order_release);
    cv_.notify_one();
}
```

GC 调用链：

```
MarkSweepCollector::MarkTermination()
  └─ sweeper_.PrepareSweep()
  └─ // STW 结束
  └─ finalizerProcessor_->NotifyToSweep()  // 唤醒 gc-helper
```

### 11.6 与 GcThreadPool 的关系

当前 `GcThreadPool` 用于并行转发（ForwardFromRegions）。在 MS 模式下，可以考虑两种后台 sweep 实现：

| 方案 | 说明 | 优缺点 |
|------|------|--------|
| **A: gc-helper 单线程** | 后台 sweep 仅在 gc-helper 线程上运行 | 实现简单；throughput 受限 |
| **B: GcThreadPool 多线程** | 使用线程池并行 sweep | throughput 更高；需更复杂的队列管理 |

**推荐方案 A（初始实现）**：
- gc-helper 单线程 sweep + lazy sweep 共同完成
- 大部分 sweep 工作由 lazy sweep 在分配路径中完成
- 单线程后台 sweep 作为兜底
- 如果实测 sweep 成为瓶颈，再升级到方案 B

---

## 12. 与现有基础设施的复用与改造清单

### 12.1 总览矩阵

| 组件 | 文件 | 复用方式 | 说明 |
|------|------|----------|------|
| **FreeRegionManager** | Allocator/FreeRegionManager.h | ✅ 直接复用 | dirty/released 树不变 |
| **CartesianTree** | Allocator/CartesianTree.h | ✅ 直接复用 | MergeInsert/TakeUnits 不变 |
| **RegionList** | Allocator/RegionList.h | ✅ 直接复用 | 双向链表操作不变 |
| **RegionManager** | Allocator/RegionManager.h/cpp | 🔄 修改 | 新增 MS region 生命周期管理 |
| **RegionInfo** | Allocator/RegionInfo.h | 🔄 修改 | 新增 sweepEpoch, sizeClass, freeList 字段 |
| **RegionSpace** | Allocator/RegionSpace.h/cpp | 🔄 修改 | 分配路径集成 lazy sweep |
| **FinalizerProcessor** | Collector/FinalizerProcessor.h/cpp | 🔄 修改 | 新增 BackgroundSweep 职责 |
| **GcThreadPool** | GcThreadPool.h/cpp | ✅ 直接复用 | 后期可用于并行 sweep |
| **Collector 基类** | Collector/Collector.h | ✅ 直接复用 | 虚函数接口不变 |
| **CollectRegion** | RegionManager.h L262 | ✅ 直接复用 | 空 region 入 garbageList |
| **ReclaimRegion** | RegionManager.cpp L400 | ✅ 直接复用 | 重置 + 入 dirty 树 |
| **ReleaseRegion** | RegionManager.cpp L414 | ✅ 直接复用 | madvise + 入 released 树 |
| **ReclaimGarbageMemory** | RegionSpace.h L107 | ✅ 直接复用 | 三阶段物理回收不变 |
| **MarkObject / IsMarkedObject** | RegionInfo.h L322/L396 | ✅ 直接复用 | bitmap 标记机制不变 |
| **MutatorManager** | Mutator/MutatorManager.h/cpp | ✅ 直接复用 | STW/LightSync 不变 |
| **WCollector** | WCollector/WCollector.h/cpp | ❌ 替换 | Copy GC 专属流程 |

### 12.2 新增文件

| 文件 | 说明 |
|------|------|
| `src/Heap/Sweeper/ConcurrentSweeper.h` | ConcurrentSweeper 类声明 |
| `src/Heap/Sweeper/ConcurrentSweeper.cpp` | 生命周期管理实现 |
| `src/Heap/Sweeper/SweepRegion.cpp` | 逐 region sweep 实现 |
| `src/Heap/Sweeper/AtomicRegionQueue.h` | 无锁 region 队列 |
| `src/Heap/Sweeper/SweepResult.h` | Sweep 结果结构 |

### 12.3 需修改的现有文件

| 文件 | 修改内容 |
|------|----------|
| `src/Heap/Allocator/RegionInfo.h` | UnitMetadata 新增 `sweepEpoch`, `sizeClass`, `freeList_`, `freeBytes_`；新增 `MS_*` RegionType 枚举值 |
| `src/Heap/Allocator/RegionManager.h/cpp` | 新增 `fullRegionList`（MS 模式用）；修改 `TakeRegion` 支持 lazy sweep |
| `src/Heap/Allocator/RegionSpace.h/cpp` | `AllocateMS` 路径集成 lazy sweep 调用 |
| `src/Heap/Collector/FinalizerProcessor.h/cpp` | 新增 `RunMS()`、`NotifyToSweep()`、`shouldSweep_` |
| `CMakeLists.txt` | 新增 Sweeper 目录源文件编译 |

### 12.4 RegionInfo 新增字段

```cpp
// 在 UnitMetadata 结构中新增（#ifdef MRT_USE_MARK_SWEEP）
struct UnitMetadata {
    // ... 现有字段 ...
    
#ifdef MRT_USE_MARK_SWEEP
    uint32_t sweepEpoch_{0};    // 最后 sweep 的 gcEpoch
    uint8_t sizeClass_{0};       // 绑定的 sizeClass（Step1）
    FreeSlot* freeList_{nullptr}; // sweep 后的 free-list 头
    size_t freeBytes_{0};        // 空闲字节数
#endif
};
```

### 12.5 改造优先级

| 阶段 | 任务 | 前置依赖 | 预估工作量 |
|------|------|----------|-----------|
| P0 | ConcurrentSweeper 核心框架 | Step2 MarkSweepCollector | 3 天 |
| P0 | SweepSmallRegion / SweepLargeRegion | ConcurrentSweeper | 2 天 |
| P0 | AtomicRegionQueue 实现 | 无 | 1 天 |
| P1 | FinalizerProcessor 集成 | ConcurrentSweeper | 2 天 |
| P1 | 分配路径 Lazy Sweep 集成 | SweepSmallRegion + Step1 分配器 | 3 天 |
| P1 | RegionInfo 字段扩展 | 无 | 1 天 |
| P2 | gcEpoch 管理 | ConcurrentSweeper + RegionInfo | 1 天 |
| P2 | bitmap 生命周期优化 | SweepSmallRegion | 1 天 |
| P2 | Pinned region sweep | SweepSmallRegion | 1 天 |
| P3 | GcThreadPool 并行 sweep（可选） | P1 完成后评估 | 2 天 |

---

## 13. 验证方案

### 13.1 单元测试

#### 13.1.1 SweepSmallRegion 正确性

```
测试名: TestSweepSmallRegion_MixedLiveness
前置条件: 一个 sizeClass=8（64B slot）的 region，分配了 100 个对象
          mark bitmap 中标记 60 个存活，40 个死亡
测试步骤:
  1. 调用 SweepSmallRegion(region)
  2. 验证返回 PARTIAL
  3. 验证 freeList 有 40 个 slot
  4. 验证 liveCount = 60, freeCount = 40
  5. 遍历 free-list 验证所有 slot 地址都在 region 范围内
  6. 验证 mark bitmap 已清零
预期: free-list 正确构建，bitmap 清零
```

```
测试名: TestSweepSmallRegion_AllDead
前置条件: 一个 region 有 50 个对象，全部未标记
测试步骤:
  1. 调用 SweepSmallRegion(region)
  2. 验证返回 EMPTY
  3. 验证 liveCount = 0
预期: EMPTY 状态，region 可回收
```

```
测试名: TestSweepSmallRegion_AllAlive
前置条件: 一个 region 有 50 个对象，全部标记存活
测试步骤:
  1. 调用 SweepSmallRegion(region)
  2. 验证返回 FULL
  3. 验证 freeList = nullptr
  4. 验证 liveCount = 50
预期: FULL 状态，无 free-list
```

#### 13.1.2 SweepLargeRegion 正确性

```
测试名: TestSweepLargeRegion_Dead
前置条件: 一个大对象 region，mark bit 未设置
测试步骤:
  1. 调用 SweepLargeRegion(region)
  2. 验证返回 EMPTY
  3. 验证 mark bit 已清零
预期: 大对象正确识别为死亡
```

```
测试名: TestSweepLargeRegion_Alive
前置条件: 一个大对象 region，mark bit 已设置
测试步骤:
  1. 调用 SweepLargeRegion(region)
  2. 验证返回 FULL
  3. 验证 mark bit 已清零
预期: 大对象正确识别为存活
```

#### 13.1.3 AtomicRegionQueue 正确性

```
测试名: TestAtomicRegionQueue_ConcurrentPushPop
线程数: 8
测试步骤:
  1. 4 个线程各 Push 1000 个 region
  2. 同时 4 个线程各 TryPop 1000 次
  3. 验证: Push 总数 = Pop 总数 + 队列剩余数
  4. 验证: 无重复 pop（每个 region 只被 pop 一次）
预期: 无数据丢失，无重复
```

#### 13.1.4 gcEpoch 状态转换

```
测试名: TestGcEpoch_SweepCompletion
测试步骤:
  1. gcEpoch = 0，所有 region sweepEpoch = 0
  2. 模拟 GC #1: gcEpoch = 1
  3. 验证所有 region NeedsSweep() = true
  4. sweep 所有 region → sweepEpoch = 1
  5. 验证所有 region NeedsSweep() = false
  6. 模拟 GC #2: gcEpoch = 2
  7. 验证所有 region NeedsSweep() = true
预期: epoch 正确驱动 sweep 状态转换
```

### 13.2 并发安全测试

#### 13.2.1 后台 Sweep + Lazy Sweep 并发

```
测试名: TestConcurrentSweep_BackgroundAndLazy
线程数: 1 gc-helper + 4 Mutator
Region 数: 200 个小对象 region（混合存活率）
测试步骤:
  1. PrepareSweep(): 200 个 region 进入 sweep 队列
  2. StartBackgroundSweep(): gc-helper 开始后台 sweep
  3. 同时 4 个 Mutator 线程调用 SweepAndGetRegion()
  4. 运行直到 sweepDone = true
  5. 验证:
     a. 没有 region 被 sweep 两次
     b. bgSweepCount + lazySweepCount = 200
     c. 所有 PARTIAL region 的 free-list 可用
     d. 所有 EMPTY region 已进入 garbageRegionList
预期: 无竞态，无重复 sweep
```

#### 13.2.2 Sweep + 分配并发

```
测试名: TestSweepAllocConcurrent
线程数: 1 gc-helper + 8 Mutator
运行时间: 5 秒
测试步骤:
  1. 8 个 Mutator 持续分配+释放对象
  2. 周期性触发 GC（每秒 1 次）
  3. 每次 GC 后启动并发 sweep
  4. Mutator 分配路径中自动调用 lazy sweep
  5. 验证:
     a. 分配从不失败（除非真正 OOM）
     b. 无 use-after-free
     c. 分配到的 slot 不与存活对象重叠
预期: 分配和 sweep 安全交错执行
```

#### 13.2.3 EnsureSweepDone 超时测试

```
测试名: TestEnsureSweepDone_HelpSweep
测试步骤:
  1. 创建 100 个待 sweep 的 region
  2. 不启动后台 sweep（模拟 gc-helper 忙）
  3. 调用 EnsureSweepDone()
  4. 验证当前线程帮助完成了所有 sweep
  5. 验证 sweepDone = true
预期: 调用线程帮助完成 sweep，不会无限等待
```

### 13.3 集成测试

#### 13.3.1 完整 GC 周期测试

```
测试名: TestMS_FullCycle_WithConcurrentSweep
测试步骤:
  1. 分配 10000 个小对象 + 100 个大对象
  2. 构建随机引用图（根集 → 可达对象）
  3. 触发 GC:
     a. 并发标记
     b. MarkTermination (STW)
     c. 并发 Sweep (后台 + lazy)
  4. 验证:
     a. 所有根集可达对象仍存活
     b. 不可达对象被正确回收
     c. 回收后的 free-list 可用于后续分配
  5. 再次分配 5000 个对象（复用 sweep 产生的 free-list）
  6. 验证新分配的对象不与旧存活对象冲突
预期: 完整 MS 周期正确
```

#### 13.3.2 多轮 GC 稳定性测试

```
测试名: TestMS_MultiCycle_Stability
运行: 100 轮 GC 周期
每轮:
  1. 分配 1000 个对象
  2. 释放（解除引用）500 个
  3. 触发 GC + 并发 sweep
  4. 验证堆大小在合理范围
  5. 验证 gcEpoch 单调递增
  6. 验证无内存泄漏（堆大小不持续增长）
预期: 100 轮后内存稳定
```

### 13.4 性能基准测试

| 测试项 | 指标 | 对比基线 | 目标 |
|--------|------|----------|------|
| STW 时间 | MarkTermination 停顿 | Step2 STW Sweep | 减少 50%+ |
| Sweep 总耗时 | 后台+lazy 完成时间 | Step2 STW Sweep | 持平或更快（并行加速） |
| 分配延迟 P99 | 第 99 百分位分配耗时 | Copy GC | ≤ 10μs |
| Lazy sweep 频率 | 分配路径触发 sweep 比例 | N/A（新增） | < 20% 分配触发 |
| 空 region 回收速度 | sweep 后物理释放延迟 | Copy GC ReclaimGarbage | 持平 |
| 吞吐量 | 应用 QPS | Copy GC | 持平或提升 |

### 13.5 回归验证清单

| 验证项 | 方法 | 通过标准 |
|--------|------|----------|
| gc.cj 基准测试 | `cjc gc.cj && ./main` | 输出正确，无崩溃 |
| gc_large.cj 大对象 | `cjc gc_large.cj && ./main` | 大对象分配+回收正常 |
| gc_mixed.cj 混合 | `cjc gc_mixed.cj && ./main` | 混合场景正常 |
| Finalizer 顺序 | 自定义 finalizer 测试 | finalizer 在 sweep 前执行 |
| Pinned 对象 | FFI 传递对象测试 | pinned 对象不被误回收 |
| 多轮 GC | 连续 1000 轮 GC | 内存稳定，无泄漏 |
| ASAN 检查 | 编译带 `-fsanitize=address` | 0 error |
| TSAN 检查 | 编译带 `-fsanitize=thread` | 0 data race |
| 编译宏切换 | CopyGC / MS 分别编译 | 两种模式均通过 |
| Lazy Sweep 压力 | 禁用后台 sweep，仅 lazy | 全部由 lazy sweep 完成 |
