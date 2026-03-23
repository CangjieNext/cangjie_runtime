# 从一次对象分配到一次 GC 回收的逐阶段调用链

## 1. 文档目标

本文按当前源码，分别梳理 `CopyGC` 和 `MSGC` 从一次对象分配开始，到触发 GC，再到本轮回收结束的完整调用链。

本文只按当前实现写，不按设计文档推导“理想形态”。

主要源码入口：
- `src/CompilerCalls.cpp`
- `src/ObjectModel/MObject.cpp`
- `src/HeapManager.cpp`
- `src/Heap/Allocator/RegionSpace.cpp`
- `src/Heap/Collector/CollectorProxy.cpp`
- `src/Heap/WCollector/WCollector.cpp`
- `src/Heap/Collector/MarkSweepCollector.cpp`
- `src/Heap/Collector/FinalizerProcessor.cpp`

## 2. 统一分配入口

### 2.1 语言运行时入口

一次普通对象分配通常从下面这条链开始：

```text
编译器生成代码
  -> MCC_NewObject / MCC_NewArray
  -> ObjectManager::NewObject / NewArray
  -> MObject::NewObject / MArray::NewArray
  -> HeapManager::Allocate
  -> Heap::GetHeap().Allocate
  -> RegionSpace::Allocate
```

相关源码：
- `src/CompilerCalls.cpp:172-178`
- `src/ObjectManager.inline.h`
- `src/ObjectModel/MObject.cpp:13-27`
- `src/HeapManager.cpp:15-18`
- `src/Heap/Allocator/RegionSpace.cpp:144-170`

### 2.2 写引用入口

对象分配出来后，字段写入统一经由 barrier 接口：

```text
MCC_WriteRefField
  -> Heap::GetBarrier().WriteReference
```

相关源码：
- `src/CompilerCalls.cpp:282-289`
- `src/CompilerCalls.cpp:291-310`
- `src/CompilerCalls.cpp:354-368`

因此，一次“对象构造 + 初始化字段 + 被 GC 看见”的关键链条，实际上分成：

1. 分配链
2. barrier 写引用链
3. GC 触发/执行链
4. 回收链

## 3. CopyGC 调用链

### 3.1 分配阶段

```text
RegionSpace::Allocate(size, allocType)
  -> ToAllocSize(size)
  -> TryAllocateOnce(allocSize, allocType)
     -> 如果是 pinned: RegionManager::AllocPinned
     -> 如果 >= large threshold: RegionManager::AllocLarge
     -> 否则:
        AllocBuffer::GetOrCreateAllocBuffer()
        -> AllocBuffer::Allocate
        -> tlRegion fast path
        -> AllocateImpl slow path
           -> RegionManager::AllocateThreadLocalRegion
           -> 新 region 成为 tlRegion
```

相关源码：
- `src/Heap/Allocator/RegionSpace.cpp:97-110`
- `src/Heap/Allocator/RegionSpace.cpp:144-170`
- `src/Heap/Allocator/RegionSpace.cpp:439-552`

### 3.2 分配失败后的重试与 GC 触发

当当前分配失败时，`RegionSpace::Allocate()` 会进入统一重试逻辑：

```text
Allocate()
  -> TryAllocateOnce 失败
  -> ShouldRetryAllocation()
     -> 早期轮次: RequestGC(GC_REASON_HEU, false)
     -> OOM 轮次: RequestGC(GC_REASON_OOM, false)
     -> 如果已有 GC 在跑: WaitForGCFinish()
     -> 最后仍失败: OutOfMemory()
```

相关源码：
- `src/Heap/Allocator/RegionSpace.cpp:113-142`
- `src/Heap/Allocator/RegionSpace.cpp:149-161`

### 3.3 GC 触发到 collector 入口

```text
HeapManager::RequestGC
  -> Collector::RequestGC
  -> CollectorProxy::RunGarbageCollection
  -> currentCollector = wCollector
  -> WCollector::RunGarbageCollection
  -> WCollector::DoGarbageCollection
```

相关源码：
- `src/HeapManager.inline.h:23-29`
- `src/Heap/Collector/CollectorProxy.cpp:35-56`

### 3.4 CopyGC 一轮 GC 的逐阶段调用链

```text
WCollector::DoGarbageCollection
  1. TraceHeap
     1.1 RegionSpace::AssembleGarbageCandidates
     1.2 TransitionToGCPhase(ENUM)
         -> MutatorManager::TransitionAllMutatorsToGCPhase
         -> InstallBarrier(EnumBarrier)
         -> Mutator::HandleGCPhase(GC_PHASE_ENUM)
     1.3 DoEnumeration
     1.4 TransitionToGCPhase(TRACE)
         -> InstallBarrier(TraceBarrier)
     1.5 RegionSpace::PrepareTrace
     1.6 DoTracing
     1.7 ProcessFinalizers

  2. PostTrace
     2.1 TransitionToGCPhase(POST_TRACE)
     2.2 HandleTraceRegions
     2.3 WeakRefBuffer::ClearWeakRefBuffer
     2.4 SatbBuffer::ClearBuffer
     2.5 PrepareCycleRef
     2.6 CollectLargeGarbage
     2.7 CollectPinnedGarbage
     2.8 RefineFromSpace
     2.9 PrepareForwardTable

  3. Preforward
     3.1 TransitionToGCPhase(PREFORWARD)
     3.2 修正并发模型 roots / finalizer roots / export roots

  4. ForwardFromSpace
     4.1 真正执行对象转发与复制

  5. TransitionToGCPhase(IDLE)
  6. MergeResurrectExportObjects
  7. PostResolveCycleTask
  8. FlipTagID / SetTagID
  9. CollectSmallSpace
 10. UnbindPreviousLiveInfo
```

相关源码：
- `src/Heap/WCollector/WCollector.cpp:370-410`
- `src/Heap/WCollector/WCollector.cpp:412-545`
- `src/Heap/Collector/TracingCollector.h:298-301`
- `src/Mutator/MutatorManager.cpp:370-385`

### 3.5 一轮回收结束后空间状态

CopyGC 收尾时，关键状态变化是：

1. `from-space` 被回收
2. 存活对象已经搬到 to-space
3. 指针 tag 代号翻转
4. 小对象空间经 `CollectSmallSpace()` 收敛

因此，CopyGC 的一次 GC 结束，本质上是“对象位置可能改变，引用语义重新稳定下来”。

## 4. MSGC 调用链

### 4.1 分配阶段

`MSGC` 的分配从 `RegionSpace::TryAllocateOnceMS()` 分成两条路：

```text
RegionSpace::Allocate
  -> TryAllocateOnce
  -> TryAllocateOnceMS
     -> 如果 allocSize >= 16KB:
        MSRegionManager::TakeRegionForLarge
        -> RegisterLargeObjectMS
     -> 否则:
        GetOrCreateAllocBufferMS
        -> AllocBufferMS::Allocate
           -> SizeClassInfo::SizeToClass
           -> FastAlloc
           -> SlowAlloc
              -> CentralRegionPool::GetRegion
              -> 若没有可复用 region:
                 CentralRegionPool::GetFreshRegion
                 -> MSRegionManager::TakeRegion
```

相关源码：
- `src/Heap/Allocator/RegionSpace.cpp:36-88`
- `src/Heap/Allocator/RegionSpace.cpp:249-300`
- `src/Heap/Allocator/AllocBufferMS.cpp:49-75`
- `src/Heap/Allocator/AllocBufferMS.cpp:116-182`
- `src/Heap/Allocator/CentralRegionPool.cpp:32-112`

### 4.2 分配时的 mark-at-alloc

当前 `MSGC` 一个很关键的实现点是“分配成功后立即按 phase 决定是否打 mark”：

```text
AllocBufferMS::Allocate
  -> FastAlloc / SlowAlloc 成功
  -> 如果 phase in [MS_MARK_SETUP, MS_SWEEP]
     -> RegionSpace::MarkObject(addr)
```

相关源码：
- `src/Heap/Allocator/AllocBufferMS.cpp:57-72`

这意味着当前实现里，即使对象是在 `MS_SWEEP` 阶段新分配出来，也会被标黑，避免下一次 generation flip 后被误扫。

### 4.3 分配失败后的重试与 GC 触发

#### 小中对象

小中对象仍复用 `RegionSpace::ShouldRetryAllocation()` 的统一策略：

```text
TryAllocateOnceMS 返回 0
  -> ShouldRetryAllocation
  -> RequestGC(HEU / OOM)
  -> WaitForGCFinish
  -> 重试
```

#### 大对象

大对象有自己的一次“先同步等 GC，再重试一次”的逻辑：

```text
TryAllocateOnceMS(large)
  -> MSRegionManager::TakeRegionForLarge 失败
  -> RequestGC(GC_REASON_OOM, false)
  -> WaitForGCFinish
  -> 再次 TakeRegionForLarge
```

相关源码：
- `src/Heap/Allocator/RegionSpace.cpp:44-71`
- `src/Heap/Allocator/RegionSpace.cpp:113-142`

### 4.4 GC 触发到 collector 入口

```text
HeapManager::RequestGC
  -> Collector::RequestGC
  -> CollectorProxy::RunGarbageCollection
  -> currentCollector = msCollector
  -> MarkSweepCollector::RunGarbageCollection
  -> MarkSweepCollector::DoGarbageCollection
```

相关源码：
- `src/Heap/Collector/CollectorProxy.cpp:35-56`
- `src/Heap/Collector/MarkSweepCollector.cpp:68-115`

### 4.5 MSGC 一轮 GC 的逐阶段调用链

```text
MarkSweepCollector::DoGarbageCollection
  1. MarkSetup
     1.1 SatbBuffer::Init
     1.2 CentralRegionPool::WaitForSweepDone
         -> 必要时 ForceFinishSweep
     1.3 STW:
         -> CentralRegionPool::FlushAllAllocBuffers
         -> CentralRegionPool::ClearLargeMarks
         -> reset markedObjectCount
     1.4 TransitionToGCPhase(MS_MARK_SETUP)
         -> InstallBarrier(MSSatbBarrier)
         -> Mutator::HandleGCPhase(MS_MARK_SETUP)
         -> GcPhaseEnum(newPhase)
     1.5 EnumAndMarkRoots

  2. ConcurrentMark
     2.1 TransitionToGCPhase(MS_CONCURRENT_MARK)
     2.2 assistEnabled = true
     2.3 RunConcurrentMarkLoop
         -> RunCollectorDrainSlice
         -> PopFromGlobal / ShareWorkToGlobal
         -> MSMarkAssist 只在当前实现中作为辅助机制存在
     2.4 assistEnabled = false

  3. MarkTermination
     3.1 STW
     3.2 DrainSatbBufferToMainQueue
     3.3 drain globalWorkQueue
     3.4 CollectExternCandidatesToMain
     3.5 CollectResurrectionCandidatesToMain
     3.6 ExecuteLikeDrainMainQueue
     3.7 TransitionToGCPhase(MS_SWEEP) 发生在 STW 内
     3.8 SatbBuffer::ClearBuffer

  4. PostMarkTermination
     4.1 WeakRefBuffer::ClearWeakRefBuffer
     4.2 ProcessFinalizers
     4.3 CollectLargeGarbage

  5. ConcurrentSweep
     5.1 ComputeMarkedBytes
     5.2 STW:
         -> FlushAllAllocBuffers
         -> CentralRegionPool::PrepareForSweep
     5.3 FinalizerProcessor::NotifyToSweep
     5.4 FinalizerProcessor::NotifyToReclaimGarbage
```

相关源码：
- `src/Heap/Collector/MarkSweepCollector.cpp:106-115`
- `src/Heap/Collector/MarkSweepCollector.cpp:126-210`
- `src/Heap/Collector/MarkSweepCollector.cpp:260-328`
- `src/Heap/Collector/MarkSweepCollector.cpp:522-590`
- `src/Heap/Collector/MarkSweepCollector.cpp:728-761`
- `src/Heap/Collector/MarkSweepCollector.cpp:808-846`
- `src/Mutator/Mutator.cpp:583-608`

### 4.6 Sweep 后空间状态如何变化

`MSGC` 的空间状态变化和 `CopyGC` 完全不同：

1. 对象位置不变
2. `PrepareForSweep()` 做的是 `swept <-> unswept` 的代际翻转
3. 真正回收小对象槽位，依赖：
   - Lazy sweep: `CentralRegionPool::GetRegion`
   - Background sweep: `FinalizerProcessor::BackgroundSweepLoop -> CentralRegionPool::BackgroundSweep`
   - Force sweep: 下一轮 `MarkSetup` 前 `WaitForSweepDone/ForceFinishSweep`

也就是说，`MSGC` 一轮 GC 结束时，并不保证所有 region 都已经立刻被 sweep 完；它保证的是：

- mark 已完成
- phase 已切到 `MS_SWEEP`
- sweep 工作已经具备继续推进的条件

## 5. 分配失败与 GC 触发链

### 5.1 统一 GC phase 切换链

无论 `CopyGC` 还是 `MSGC`，phase 切换都统一走：

```text
TracingCollector::TransitionToGCPhase
  -> MutatorManager::TransitionAllMutatorsToGCPhase
  -> Heap::InstallBarrier(phase)
  -> Heap::SetGCPhase(phase)
  -> VisitAllMutators
  -> Mutator::HandleGCPhase(newPhase)
```

相关源码：
- `src/Heap/Collector/TracingCollector.h:298-301`
- `src/Mutator/MutatorManager.cpp:370-385`
- `src/Mutator/Mutator.cpp:583-608`

### 5.2 CopyGC 与 MSGC 在触发链上的区别

#### CopyGC

- 分配失败后主要靠 `ShouldRetryAllocation()` 统一触发 `HEU/OOM GC`
- collector 完成一次 GC 后，空间会更直接地释放出来

#### MSGC

- 也会走统一 `ShouldRetryAllocation()`
- 但大对象路径会在 `TryAllocateOnceMS()` 内部额外主动触发一次 OOM GC 重试
- GC 完成后可用空间恢复速度还依赖 sweep 及时性

## 6. 一次 GC 完成后对象/空间状态变化

### 6.1 CopyGC

一次 GC 结束后：

- 存活对象可能已经搬迁
- 老引用可能已经被 fix 成新地址
- tag 代号翻转
- from-space 被收走

因此，对象“存活”的同时，也伴随“地址语义重写”。

### 6.2 MSGC

一次 GC 结束后：

- 存活对象仍在原地址
- `markBitmap` 已经给出本轮存活快照
- 小对象 region 进入下一轮 sweep generation
- 小对象空槽何时真正重新可分配，取决于 lazy/background/force sweep 的推进
- 大对象会在 `CollectLargeGarbage()` 或 `ForceFinishSweep()` 里释放

因此，`MSGC` 的一次 GC 更像“完成判活并启动回收”，而不是“在 collector 返回点立刻完成全部空间收敛”。

## 7. 关键差异总结

```text
一次对象分配到一次 GC 回收结束：

CopyGC
  分配 -> 枚举/标记 -> PostTrace -> Preforward -> Forward -> CollectSmallSpace
  重点在“对象移动后如何修正引用”

MSGC
  分配 -> MarkSetup -> ConcurrentMark -> MarkTermination -> PostMark -> ConcurrentSweep
  重点在“对象不移动时如何让 sweep 及时、安全地兑现空闲槽位”
```

最本质的差异不是“一个叫 Copy，一个叫 MS”，而是：

- `CopyGC` 把复杂度放在引用修正和对象搬迁上
- `MSGC` 把复杂度放在 size class 分配、bitmap、sweep 协调和回收时机上

这也是为什么：

- `CopyGC` 的 barrier 更重、更复杂
- `MSGC` 的分配器和 sweep 子系统更复杂
