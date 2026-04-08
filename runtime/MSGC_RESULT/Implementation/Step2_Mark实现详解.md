# Step2 Mark 实现详解

> 位置：MSGC Step2 的实现说明文档。
> 目的：结合当前 runtime 源码，说明三色标记阶段现在到底是怎么跑的。
> 口径：**以当前代码为准**，不重复旧版 Plan 中已经失效的阶段划分和数据流假设。

---

## 目录

1. [总览](#1-总览)
2. [关键结构](#2-关键结构)
3. [MarkSetup](#3-marksetup)
4. [ConcurrentMark](#4-concurrentmark)
5. [写屏障与 Assist 数据流](#5-写屏障与-assist-数据流)
6. [MarkTermination](#6-marktermination)
7. [PostMarkTermination](#7-postmarktermination)
8. [参数与可观测性](#8-参数与可观测性)

---

## 1. 总览

当前 Mark-Sweep 的实现并不是一个单独的 `mark()` 例程，而是 `MarkSweepCollector::DoGarbageCollection()` 中连续串起的一条阶段化流水线。按当前主链看，可以直接分成 6 个阶段：

1. `PreEnumRoots()`
2. `EnumRoots()`
3. `ConcurrentMark()`
4. `MarkTermination()`
5. `PostMarkTermination()`
6. `ConcurrentSweep()`

当前代码里的主链路可以直接记成：

```text
DoGarbageCollection
	-> PreEnumRoots
	-> EnumRoots
	-> ConcurrentMark
	-> MarkTermination
	-> PostMarkTermination
	-> ConcurrentSweep
```

其中前 5 段构成当前实现里的“Step2 Mark 主体”，最后一段 `ConcurrentSweep()` 属于后续 sweep 阶段，但在总览里最好一起看，否则很难解释为什么 `MarkTermination()` 结束后还会继续做 allocator / region 侧的收尾工作。

可以先把这 6 个阶段压缩成下面这张表：

| 阶段 | 入口函数 | 是否 STW | 主要做什么 | 作用 |
| --- | --- | --- | --- | --- |
| 1 | `PreEnumRoots()` | 否 | 停掉后台和 lazy sweep，等待所有 in-flight sweep 退出，再清理 mark/scanned bitmap，并把残留 unswept region 直接 stamp/reclassify 到 swept 视图，同时设置 `activeCollector_` | 给新一轮标记准备一套稳定的 region / bitmap 视图，并在新 GC 开始前放弃上一轮未完成的 sweep backlog 清理 |
| 2 | `EnumRoots()` | 否，采用 handshake | 切到 `GC_PHASE_MS_MARK_SETUP`，让每个 Mutator 自扫自己的栈根；GC 线程再 merge mutator root buffer，并串行枚举 static/concurrency/finalizer/export roots | 建立本轮初始灰对象集合，把根可达对象送入 `markWorkStack_` |
| 3 | `ConcurrentMark()` | 否 | 切到 `GC_PHASE_MS_CONCURRENT_MARK`，GC 线程 drain 本地 work stack，并通过 `globalWorkQueue_` 与 Mutator assist 协作推进 trace | 在不长时间停世界的前提下完成绝大多数对象图遍历，尽量把灰对象消化掉 |
| 4 | `MarkTermination()` | 是 | STW 收口，汇总 `WriteBarrierBuffer`、`assistLocalWork_`、`globalWorkQueue_` 和 resurrection candidates，做最后一次 drain，然后 `FlushAllAllocBuffers()` 与 `PrepareForSweep()`，最后切到 `GC_PHASE_MS_SWEEP` | 关闭并发标记窗口，保证没有遗漏的灰对象，并把 allocator / region 状态切换到下一轮 sweep 视图 |
| 5 | `PostMarkTermination()` | 否 | 清理 `WeakRefBuffer`、把不可达 finalizable 对象送入 `FinalizerProcessor`，并对 in-heap large region 做 eager reclaim | 处理 mark 结果的语义后效，例如 weak ref、finalizer 和 large region 回收 |
| 6 | `ConcurrentSweep()` | 否 | 计算 `trueLiveBytes`，并通知 `MSRegionManager` / `CentralRegionPool` 启动后台 sweep | 把本轮 mark 的结果交给 sweep 阶段消化，逐步回收 small/medium region 的空槽与空 region |

这里有两个容易和旧设计稿混淆的点：

1. Step2 的“MarkSetup”现在是 `PreEnumRoots + EnumRoots` 的组合，不是单独一段纯 handshake 或纯 STW。
2. Step2 的收敛主线已经不是 SATB buffer，而是 `WriteBarrierBuffer + assistLocalWork + globalWorkQueue`。

当前实现采用的是“`markBit` 表示 discovered，`scannedBit` 表示 children 已经 trace 完”的双状态模型：

1. 白到灰：`MarkObject(obj)` 把 mark bit 从 0 置 1。
2. 灰到黑：`SetScannedObject(obj)` 把 scanned bit 置 1。
3. 只有 `alreadyMarked && IsScannedObject(obj)` 时，才允许直接跳过该对象。

因此当前 Step2 的核心并不是“谁先把对象压进栈”，而是“谁负责发现对象，谁负责把 discovered 对象真正扫描完”。

## 2. 关键结构

### 2.1 代码映射

| 文件 | 当前职责 |
| --- | --- |
| `src/Heap/MCollector/MarkSweepCollector.h/.cpp` | Step2 五个子阶段的主实现 |
| `src/Heap/MBarrier/MSMarkBarrier.cpp` | 当前混合写屏障：old/new 引用发现 |
| `src/Heap/MBarrier/WriteBarrierBuffer.h` | 每 Mutator 的写屏障缓冲 |
| `src/Heap/MAllocator/MSAllocBuffer.h/.cpp` | 小对象分配、mark-at-alloc、assist 触发点 |
| `src/Mutator/Mutator.cpp` | `GcPhaseEnum()` 和 Mutator 侧 phase 切换 |
| `src/Mutator/MutatorManager.cpp` | `TransitionAllMutatorsToGCPhase()` |
| `src/Heap/MAllocator/MSCentralRegionPool.cpp` | MarkSetup / PrepareForSweep / large region sweep |

### 2.2 运行时关键结构

| 结构 | 当前角色 |
| --- | --- |
| `markWorkStack_` | GC 线程本地主工作栈；根枚举阶段填充，并发标记阶段消费 |
| `globalWorkQueue_` | GC 线程和 Mutator assist 共享的全局灰对象队列，受 `globalWorkMutex_` 保护 |
| `WriteBarrierBuffer` | 每 Mutator 一个固定 256 项的屏障缓冲；溢出时整批刷入 `globalWorkQueue_` |
| `assistLocalWork_` | 每 Mutator 的本地 assist 工作残留；并发标记结束前不强制回收，Termination 再统一 flush |
| `activeCollector_` | 屏障和 assist 可见性的开关；`PreEnumRoots()` 设置，`MarkTermination()` 中清空 |
| `markedObjectCount` | 本轮累计新发现对象计数；并发标记和 termination 都按批次累加 |
| `MSGCCycleStats` | 各阶段时间、assist 统计、large reclaim 统计和 sweep 摘要 |

还有一个当前代码层面很重要的事实：`TracingCollector::TransitionToGCPhase()` 现在只是简单调用 `MutatorManager::TransitionAllMutatorsToGCPhase()`，第二个 `bool` 参数已经不再决定“是 STW 还是 handshake”。真正的语义取决于调用时 world 是否已经停止。

## 3. MarkSetup

### 3.1 `PreEnumRoots()`：先把 sweep 和 bitmap 收口

`PreEnumRoots()` 当前由 GC 线程独占执行，不做全局 STW。它做的事情很集中：

1. `activeCollector_.store(this)`，让写屏障和 assist 可以看到当前 collector。
2. 通过 `CentralRegionPool` 执行：
	 - `StopAndWaitForAllSweep()`
	 - `ClearAllMarkBitmaps()`
	 - `ReclassifyUnsweptToSwept()`
	 - `ReenableLazySweep()`
3. `markedObjectCount` 清零。

这里的真实意图不是“开始枚举根”，而是先终止上一轮 sweep 的并发活动，再给新一轮 mark 准备一套稳定的 bitmap / region 视图。

其中 `StopAndWaitForAllSweep()` 的当前实现只做 sweep quiesce，不再补扫 backlog：

1. 先通过 `stopSweepRequested_ + skipLazySweep_` 停掉后台 sweep 和新的 lazy sweep，并等待所有 in-flight sweep 退出。
2. 它当前不会调用 `ForceFinishAllUnswept()`；也就是说，`partialUnswept/fullUnswept` 里的剩余 backlog 不会在这里被物理 sweep 完。

`ForceFinishAllUnswept()` 这个 helper 现在仍然存在于 `CentralRegionPool` 中，但并不在当前 `PreEnumRoots()` 主路径上被调用。

`ClearAllMarkBitmaps()` 当前行为也需要单独记住：

1. small/medium region：清 `markBitmap` 和 `scannedBitmap`。
2. 对仍然停留在旧一轮 unswept 状态的 region：通过 `CasSweepGen(sg - 2, sg)` 直接提升到当前 sweep generation，避免后续按旧 bitmap 再做 lazy sweep。
3. `MS_REGION_LARGE`：不清 bitmap，而是直接把 `isMarked` 和 `isScanned` 复位。

紧接着 `ReclassifyUnsweptToSwept()` 会把残留 `partialUnswept/fullUnswept` 链表中的节点直接搬到 swept 视图里。这里做的是链表归类修正，不是一次真实的 `SweepRegion()`。

所以现在的 MarkSetup 开头不是“先切 phase 再看根”，也不是“先停 sweep 再把 backlog 全量补扫”，而是“先停 sweep、等待并发 sweep 退场、清 bitmap、stamp/reclassify 残留 unswept region，然后进入根枚举”。

### 3.2 `EnumRoots()`：当前是 handshake 风格的根枚举

`EnumRoots()` 当前先调用：

```text
TransitionToGCPhase(GC_PHASE_MS_MARK_SETUP)
```

这一步最终走到 `MutatorManager::TransitionAllMutatorsToGCPhase()`：

1. 安装当前 phase 对应的 barrier。
2. 设置全局 `GCPhase`。
3. 给所有 Mutator 广播“需要进入新 phase”的请求。
4. 等待每个 Mutator 完成自己的 phase transition。

因为这一步发生时 world 没有 stop，所以实际效果是 handshake 风格的 phase 切换，而不是长时间 STW。

Mutator 侧在 `GC_PHASE_MS_MARK_SETUP` 会执行 `Mutator::GcPhaseEnum()`：

1. 访问自己的栈根。
2. 把堆对象根通过 `Allocator::PushRootToBuffer()` 写入当前分配缓冲的根缓存。
3. 对栈上对象继续 DFS，直到把可达的栈根都推入根缓冲。

随后 GC 线程执行：

1. `markWorkStack_.clear()`。
2. `EnumAndMarkRoots(markWorkStack_)`。

而 `EnumAndMarkRoots()` 在当前 MSGC 里是串行实现：

1. `EnumAllCommonRootsSerial()`
2. `EnumAllExportRoots()`

其中 `EnumAllCommonRootsSerial()` 又会做：

1. `MergeMutatorRoots()`，把每个 Mutator 分配缓冲里的 `stackRoots_` 合并进主 work stack。
2. 枚举 static roots。
3. 枚举 concurrency model roots。
4. 枚举 finalizer processor roots。
5. 枚举 resurrected export roots。

所以当前根枚举的真实模型是：Mutator 先各自把栈根写进本地 root buffer，GC 线程再统一 merge 并继续枚举全局根。

## 4. ConcurrentMark

### 4.1 相位切换与 assist 启动

`ConcurrentMark()` 首先切到 `GC_PHASE_MS_CONCURRENT_MARK`，然后重置 assist / global queue 统计，并打开 `assistEnabled_`。当前打开 assist 的顺序是：

1. 清零 assist 统计项。
2. `globalWorkAvailable_ = false`。
3. `assistEnabled_ = true`。
4. 进入 `RunConcurrentMarkLoop(markWorkStack_)`。
5. 并发标记退出后再把 `assistEnabled_ = false`。

也就是说，Mutator assist 的存活窗口严格包在 `ConcurrentMark()` 内部。

### 4.2 GC 线程主循环

当前 `RunConcurrentMarkLoop()` 的循环结构非常直接：

1. 当本地 `workStack.size() >= kShareWorkBufs` 时，`ShareWorkToGlobal()` 把一半 buffer 推给 `globalWorkQueue_`。
2. 本地不空时，`RunCollectorDrainSlice(workStack, kSliceMaxObjects)` 处理一个 slice。
3. 本地空时，`PopFromGlobal(workStack, 1)` 从全局拉 1 个 buffer。
4. 当本地和全局都空时，累加 `convergenceStableRounds_`。
5. 连续 `kConvergenceRequiredRounds` 轮都空，才判定收敛并退出。

当前收敛判定不是看 SATB retired node，也不是看某个 epoch，而是纯粹看“本地 work + 全局 work 是否连续多轮都为空”。为了给飞行中的 assist 留时间，空轮次之间还会 `usleep(50)` 做一个短让步。

### 4.3 `RunCollectorDrainSlice()`：当前 trace 语义

GC 线程 drain 当前采用的是 mark-on-pop 模式：

1. 从 work stack 弹对象。
2. `MarkObject(obj)`，如果原来没标记过，说明这次才第一次从白色变成灰色。
3. 如果 `alreadyMarked && IsScannedObject(obj)`，直接跳过。
4. 没有引用字段则只打 `scannedBit`。
5. `WeakRef` 走专门分支，记录进 `WeakRefBuffer`，不走普通字段 trace。
6. 普通对象走 `TraceObjectRefFields(obj, workStack)`，把子对象压回 work stack。
7. 最后 `SetScannedObject(obj)`。

`TraceRefField()` 当前也不是“发现时立即 mark”，而是：

1. 先读目标引用。
2. 如果目标尚未标记，只把目标压进 work stack。
3. 真正的 mark 发生在目标对象下一次被 pop 出来时。

这就是当前代码反复强调的“mark-on-pop + scannedBit”模型。

## 5. 写屏障与 Assist 数据流

### 5.1 `MSMarkBarrier`：当前是混合屏障，不是纯 SATB

当前 `MSMarkBarrier::WriteReference()` 的顺序是：

```text
shade(old)
	-> EnqueueRef(oldRef)
store(new)
	-> field.SetTargetObject(ref)
shade(new)
	-> EnqueueRef(newRef)
```

也就是“删除屏障 + 插入屏障”的混合屏障，不再依赖 SATB 的快照语义来做主收敛。

`EnqueueRef()` 的关键路径是：

1. 过滤空指针和非堆地址。
2. 先调用 `collector.MarkObject(ref)`。
3. 如果已经发现过，直接返回，不再入队。
4. 否则把对象写入当前 Mutator 的 `WriteBarrierBuffer`。
5. 如果 buffer 已满，则先整批刷入 `globalWorkQueue_`，再把当前对象写进 buffer。

因此现在的写屏障缓冲里放的不是“所有写过的对象”，而是“当前屏障第一次发现的灰对象”。

### 5.2 `WriteBarrierBuffer`：每 Mutator 两层缓冲的第一层

`WriteBarrierBuffer` 当前是一个非常简单的固定大小 inline 数组：

1. 容量 `256`。
2. `Push()` 是单写者 O(1)。
3. `Flush()` 把全部对象顺序压入目标 `WorkStack`。
4. MarkTermination 在 STW 下统一 flush。

溢出时并不会逐对象走慢路径，而是调用 `FlushWriteBarrierOverflow()` 一次把当前 buffer 批量刷入全局队列，再继续写入当前对象。

### 5.3 `MSMarkAssist()`：当前由分配路径被动触发

Mutator assist 的真实入口在 `AllocBufferMS::Allocate()`，而不是独立的后台线程。当前小对象分配成功后，如果 Mutator phase 处在：

1. `GC_PHASE_MS_MARK_SETUP`
2. `GC_PHASE_MS_CONCURRENT_MARK`
3. `GC_PHASE_MS_MARK_TERMINATION`

则会立刻执行 `MarkNewObject(addr)`，把新对象直接黑化。然后开始累计 `gcAssistBytes_`。当累计债务达到 `64KB` 时，只有在下面两个条件之一成立时才会真正触发 assist：

1. 当前 Mutator 自己还有 `assistLocalWork_` 残留。
2. `activeCollector_` 可见，且 `globalWorkAvailable_` 为 true。

`MSMarkAssist()` 的当前行为是：

1. 本地 `assistLocalWork_` 为空时，才考虑从 `globalWorkQueue_` 补货。
2. 如果 `globalWorkAvailable_` 看起来为空，并且这次不是第 `kAssistProbeIntervalCalls` 次 probe，则直接 fast-skip。
3. 每次 assist 最多处理 `kAssistMaxObjectsPerCall` 个对象。
4. 本地残留工作不清空，留到下一次 assist 继续。
5. 如果本地工作堆积超过 `kAssistLocalFlushBufs`，就把一半工作 share 到全局。

当前 weak ref 在 assist 路径上不会本地处理，而是重新推回全局队列，让 GC 主线程在统一路径上处理。

所以现在的两层缓冲关系是：

```text
Mutator write
	-> WriteBarrierBuffer
			 -> overflow 时刷到 globalWorkQueue

Mutator allocate
	-> debt >= 64KB
			 -> MSMarkAssist
						-> assistLocalWork
						-> 必要时从 globalWorkQueue 拉取
```

## 6. MarkTermination

### 6.1 当前 MarkTermination 的边界

`MarkTermination()` 一开始就进入：

```text
ScopedStopTheWorld("MS MarkTermination", true, GC_PHASE_MS_MARK_TERMINATION)
```

也就是当前 Step2 唯一明确的长 STW 收口点。进入 STW 之后，第一件事就是：

1. `activeCollector_ = nullptr`

这意味着从这一刻开始，屏障 overflow 和 assist 都不再把工作交给当前 collector 的并发路径。

### 6.2 主队列的真实来源

当前主队列 `mainQueue` 是按下面顺序构建的：

1. `FlushAllWriteBarrierBuffers(mainQueue)`
2. `FlushAllAssistLocalWorks(mainQueue)`
3. `DrainGlobalWorkQueueToMainQueue(mainQueue)`
4. `CollectResurrectionCandidatesToMain(mainQueue)`

这里要明确两点：

1. 当前 direct source 只有 `WriteBarrierBuffer`。
2. SATB buffer 不是当前 MarkTermination 的主输入源。

assist 残留和 global queue 被归类为“并发阶段尚未消费完的 pending work”，而 finalizer resurrection candidates 是第三类单独补进来的输入。

### 6.3 `ExecuteLikeDrainMainQueue()`：用同一套 mark/scanned 语义收尾

当前 termination 的 drain 不是另写一套 trace 算法，而是沿用和并发标记相同的基本规则：

1. `MarkObject()` 负责 discovered。
2. `scannedBit` 负责判断是否已经 trace 完。
3. 没引用字段则直接置 scanned。
4. 普通对象继续 `TraceObjectRefFields()`。
5. WeakRef 走自己的分支。

区别只在于：并发阶段的工作来源分散在本地 queue / assist queue / barrier queue，而 termination 把这些来源全部汇总成一个 `mainQueue` 后，再做最后一次排空。

### 6.4 `FlushAllAllocBuffers()` + `PrepareForSweep()`

mark 真正收敛之后，当前代码会立刻做两件 allocator 侧工作：

1. `centralPool->FlushAllAllocBuffers()`
2. `centralPool->PrepareForSweep()`

当前 `PrepareForSweep()` 的职责不是 sweep 本身，而是把列表和 generation 切到下一轮 sweep 视图：

1. `partialSwept -> partialUnswept`
2. `fullSwept <-> fullUnswept`
3. `globalSweepGen += 2`
4. `sweepDone = false`

也就是说，termination 的后半段其实是在给 Step4 并发 sweep 准备工作集。

### 6.5 `GC_PHASE_MS_SWEEP` 在 STW 内切换

当前代码在 STW 内部直接执行：

```text
TransitionToGCPhase(GC_PHASE_MS_SWEEP, false)
```

虽然这里第二个参数是 `false`，但当前 `TracingCollector::TransitionToGCPhase()` 并不读取这个参数。真正让这次 phase 切换变成“STW 内切换”的原因，是此时 world 已经在 `ScopedStopTheWorld` 保护下停止了。

这一点很关键，因为它消除了这样一个窗口：Mutator 恢复执行，但最后一批屏障条目还没在 termination 里排空。

当前 termination 结束前还会输出：

1. `cycleStats_.wbBufEntriesTotal`
2. `cycleStats_.markTermProcessedObjects`
3. `cycleStats_.markTermNewlyMarkedObjects`
4. `MSMarkBarrier_DumpAndResetCounters()` 的计数器日志

## 7. PostMarkTermination

`PostMarkTermination()` 当前已经不再承担相位切换或主队列收敛职责，它现在就是纯粹的“mark 后处理”。

当前顺序固定为三步：

1. `WeakRefBuffer::Instance().ClearWeakRefBuffer()`
2. `ProcessFinalizers()`
3. `CollectLargeGarbage()`

### 7.1 WeakRef 清理

当前弱引用清理发生在 mark bitmap 已经稳定之后，因此不需要再停世界。它处理的是 Step2 前面收集到 `WeakRefBuffer` 里的对象。

### 7.2 Finalizer 处理

`ProcessFinalizers()` 当前通过 `collectorResources.EnqueueFinalizables()` 把 `!IsMarkedObject(obj)` 的 finalizable 对象送给 `FinalizerProcessor`，然后 `NotifyFinalizerWork()`。

这一步只负责“根据 mark 结果入队”，不负责在这里直接执行 finalizer。

### 7.3 Large 对象 eager reclaim

当前 large object 清理已经完全收敛到：

1. `centralPool->SweepLargeRegions()`

它只处理当前 in-heap large region：

1. 把 `isMarked == 0` 的 large region 从 `largeRegionList_` 摘出来。
2. 清掉 span entry 和 base entry 的 `MSRegionInfo`。
3. 调用 `MSRegionManager::ReclaimRegionLarge()` 归还物理 region。
4. 更新 `cycleStats_.losFreedCount / losFreedRegionCount / losFreedBytes`。

这里已经没有独立的 `LargeObjectSpace` 旁路了。

Step2 到这里结束，后续 `ConcurrentSweep()` 才会计算 `trueLiveBytes`，并通知后台 sweep 开始消化小对象 unswept region。

## 8. 参数与可观测性

当前实现里和 Step2 直接相关的关键常量有：

| 常量 | 当前值 | 含义 |
| --- | --- | --- |
| `kSliceMaxObjects` | `256` | GC 线程单次 drain slice 最多处理的对象数 |
| `kShareWorkBufs` | `2` | GC 本地 work stack 达到该 buffer 数时，向全局 share 一半 |
| `kAssistMaxObjectsPerCall` | `64` | Mutator 单次 assist 最多处理的对象数 |
| `kAssistLocalFlushBufs` | `2` | assist 本地工作超过该 buffer 数时开始 share |
| `kAssistProbeIntervalCalls` | `8` | globalWork 看起来为空时，每 8 次 assist 才做一次强制 probe |
| `kConvergenceRequiredRounds` | `3` | 本地和全局队列连续空 3 轮才判定并发标记收敛 |
| `WriteBarrierBuffer::kCapacity` | `256` | 每 Mutator 屏障缓冲容量 |
| `AllocBufferMS::kGCAssistTriggerBytes` | `64KB` | 分配债务达到阈值后才考虑触发 assist |

当前可观测性主要看三类信息：

1. `MSGCCycleStats`
	 - `preEnumRootsUs`
	 - `markSetupUs`
	 - `enumRootsUs`
	 - `concurrentMarkUs`
	 - `markTerminationUs`
	 - `postMarkUs`
	 - `assistCallCount / assistMarkedObjects / assistProcessedObjects / assistTracedObjects`
	 - `losFreedCount / losFreedRegionCount / losFreedBytes`
2. `MSMarkBarrier_DumpAndResetCounters()`
	 - 在 MarkTermination 末尾输出屏障计数器
3. REPORT 级日志
	 - 每个子阶段入口和汇总点都有独立日志，便于对齐时序和对象数量

如果要继续读代码，当前最合理的顺序是：

1. `MarkSweepCollector::DoGarbageCollection()`
2. `PreEnumRoots()` / `EnumRoots()`
3. `MSMarkBarrier::WriteReference()`
4. `AllocBufferMS::Allocate()` / `MSMarkAssist()`
5. `MarkTermination()` / `ExecuteLikeDrainMainQueue()`
6. `PostMarkTermination()` / `CentralRegionPool::SweepLargeRegions()`