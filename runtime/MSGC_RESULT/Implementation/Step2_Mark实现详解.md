# Step2 Mark 实现详解

> 位置：MSGC Step2 的实现说明文档。
> 目的：只按**当前 runtime 代码**描述 MSGC 的标记流程。
> 口径：当前实现是 **mark-only + queue protocol**。对象状态只使用 mark 位；grey / black 由队列协议表达。

---

## 目录

1. [总览](#1-总览)
2. [核心结构与状态模型](#2-核心结构与状态模型)
3. [PreEnumRoots](#3-preenumroots)
4. [EnumRoots 与 Root Staging](#4-enumroots-与-root-staging)
5. [发现对象与唯一 Grey 生产入口](#5-发现对象与唯一-grey-生产入口)
6. [ConcurrentMark](#6-concurrentmark)
7. [写屏障与 Assist](#7-写屏障与-assist)
8. [MarkTermination](#8-marktermination)
9. [PostMarkTermination 与 ConcurrentSweep](#9-postmarktermination-与-concurrentsweep)
10. [新对象与对象状态总结](#10-新对象与对象状态总结)
11. [参数与可观测性](#11-参数与可观测性)
12. [当前回归验证](#12-当前回归验证)

---

## 1. 总览

当前 MSGC 的标记阶段不是单独一个 `mark()` 函数，而是 `MarkSweepCollector::DoGarbageCollection()` 里连续串起来的一条阶段流水线：

```text
DoGarbageCollection
    -> PreEnumRoots
    -> EnumRoots
    -> ConcurrentMark
    -> MarkTermination
    -> PostMarkTermination
    -> ConcurrentSweep
```

按运行时语义，可以把这条链看成下面 6 段：

| 阶段 | 入口函数 | 是否 STW | 主要职责 |
| --- | --- | --- | --- |
| 1 | `PreEnumRoots()` | 否 | 停 sweep、清 mark 状态、冻结本轮 sweep snapshot |
| 2 | `EnumRoots()` | 否，handshake | 让每个 mutator 自扫栈根，GC 线程 merge 根并建立初始 grey 集合 |
| 3 | `ConcurrentMark()` | 否 | GC 线程和 mutator assist 并发 drain grey work |
| 4 | `MarkTermination()` | 是 | STW 收口，把所有残留 grey work 汇总并做最后闭包 |
| 5 | `PostMarkTermination()` | 否 | 处理 weak ref、finalizer、large object eager reclaim |
| 6 | `ConcurrentSweep()` | 否 | 计算 true live bytes，并启动后台 sweep |

当前实现的标记模型可以直接记成：

1. `white`：对象未被 mark。
2. `grey`：对象已经 mark，并且已经进入 grey work 系统，等待被 trace。
3. `black`：对象已经 mark，而且它的引用字段已经被当前 drain 处理完，不再停留在任何 grey queue 中。

这里最关键的一点是：`black` 不落在对象状态位上。  
grey / black 的区别完全由**队列协议**表达。

---

## 2. 核心结构与状态模型

### 2.1 关键运行时结构

| 结构 | 当前职责 |
| --- | --- |
| `markWorkStack_` | GC 线程的本地 grey work 栈；根枚举阶段建立，ConcurrentMark 阶段消费 |
| `globalWorkQueue_` | GC 线程与 mutator assist 共享的全局 grey work 队列 |
| `assistLocalWork_` | 每个 mutator 自己持有的本地 assist work，允许跨多次 assist 调用保留 |
| `deferredMainThreadQueue_` | assist 不能处理、必须交由 GC 主线程处理的特殊项队列 |
| `WriteBarrierBuffer` | 每个 mutator 一个固定容量屏障缓冲，保存新产生的 grey work |
| `mainQueue` | `MarkTermination()` 内部构造的 STW 收口队列 |
| `WeakRefBuffer` | 记录弱引用对象，供 `PostMarkTermination()` 统一清理 |

### 2.2 `GreyWorkQueue` 的语义边界

当前灰对象队列不再直接暴露裸 `MarkStack<BaseObject*>` 作为主接口，而是包了一层 `GreyWorkQueue`。

这层包装的目的很明确：

1. 新 grey 对象只能通过 collector 提供的 helper 进入队列。
2. 队列之间的搬运是 `move` 语义，不是新的 grey 生产。
3. `GreyWorkQueue` 对外暴露的是 `insert / split / AdoptSplitBuffers / PushMovedGrey` 这类搬运接口。
4. `PushNewlyGrey()` 是私有接口，只允许 `MarkSweepCollector` 调用。

因此当前代码有一条很清晰的约束：

```text
新 grey 对象 -> TryEnqueueGreyObject
队列间搬运 -> GreyWorkQueue 的 move 接口
```

### 2.3 当前对象状态位

当前小对象 region 的 sidecar bitmap 只有两张：

1. `allocBitmap`：`1=free, 0=allocated`
2. `markBitmap`：`1=alive(marked), 0=dead(unmarked)`

large object 则只保留 `FLAG_MARKED` 作为并发安全的 mark 位。

也就是说，当前 Step2 的对象状态基础只有：

```text
small object: allocBitmap + markBitmap
large object: FLAG_MARKED
```

---

## 3. PreEnumRoots

`PreEnumRoots()` 在 GC 线程独占执行，但不是 STW。它的任务是给新一轮 mark 准备一套稳定的 heap 视图。

当前主流程是：

1. `activeCollector_.store(this)`，让写屏障和 assist 能看到本轮 collector。
2. 调 `CentralRegionPool::StopAndWaitForAllSweep()`，停掉后台和 lazy sweep，等待 in-flight sweep 退出。
3. 调 `CentralRegionPool::ClearAllMarkBitmaps()`，清掉上一轮的 mark 状态。
4. 调 `CentralRegionPool::ReclassifyUnsweptToSwept()`，把残留 unswept region 的链表视图改成 swept。
5. 调 `CentralRegionPool::PrepareForSweep()`，把当前 central pool 中的 swept region 翻入 unswept，冻结本轮 sweep snapshot。
6. `markedObjectCount` 清零。

这里需要记住两个实现细节：

1. `ClearAllMarkBitmaps()` 现在只清 **mark**。
   - small/medium region：`ClearMarkBitmap()`
   - large region：`ClearMarkedFlag()`
2. `PrepareForSweep()` 会在 MarkSetup 阶段递增 `globalSweepGen`，此后 mutator 归还的 active region 会按当前 generation 放入 swept 队列，绕过本轮 sweep。
3. `PreEnumRoots()` 只处理当前 mark 状态，不维护额外的黑态状态位。

因此这一阶段的本质是：

```text
停止 sweep 并等待退场
    -> 清当前 mark 状态
    -> 修正 region 列表视图
    -> 冻结本轮 sweep 候选集合
    -> 为新一轮 mark 打开 activeCollector_
```

`StopAndWaitForAllSweep()` 会保持 lazy sweep 关闭；它不会在 `PreEnumRoots()` 末尾立即恢复。lazy sweep 要等 `MarkTermination()` 完成最后闭包、归还 active TL region 后才重新打开，避免在 mark 尚未完成时消费本轮 unswept snapshot。

---

## 4. EnumRoots 与 Root Staging

`EnumRoots()` 先切到 `GC_PHASE_MS_MARK_SETUP`，这一步是 handshake 风格的 phase transition，不是长时间 STW。

### 4.1 Mutator 侧做什么

每个 mutator 在 `GC_PHASE_MS_MARK_SETUP` 下会执行自己的栈根扫描，并把发现的堆对象根暂存到 alloc buffer 的 root buffer。

这里的 root buffer 只是 **staging buffer**，不是 grey queue。

也就是说：

```text
PushRootToBuffer()
```

只表示“先把 root 暂存起来”，还不表示对象已经进入 grey work 系统。

### 4.2 GC 线程做什么

GC 线程接着做：

1. `markWorkStack_.clear()`
2. `EnumAndMarkRoots(markWorkStack_.UnsafeRaw())`

`EnumAndMarkRoots()` 当前是串行实现，内部会做：

1. `EnumAllCommonRootsSerial()`
2. `EnumAllExportRoots()`

其中 `EnumAllCommonRootsSerial()` 里最关键的一步是 `MergeMutatorRoots()`。它会把每个 alloc buffer 里暂存的 root 合并出来，再逐个调用：

```text
DiscoverAndPushObject(root, workStack)
```

也就是说，root 真正变成 grey work 的时刻，不是 mutator 扫描栈时，而是 GC 线程 merge 这些 staged roots 时。

因此当前根枚举的数据流应该记成：

```text
Mutator scan stack
    -> PushRootToBuffer (staging)
    -> GC merge roots
    -> DiscoverAndPushObject
    -> markWorkStack_
```

---

## 5. 发现对象与唯一 Grey 生产入口

当前 mark 阶段的核心协议，是“先 mark，再 enqueue”。

### 5.1 `MarkObject()` / `DiscoverObject()`

`MarkObject(obj)` 是底层 mark 原语：

1. 如果地址在 heap 内，走 `MSRegionInfoArray::MarkObjectInHeap(addr)`。
2. small object：原子 `fetch_or` 对应 slot 的 `markBitmap`。
3. large object：原子置 `FLAG_MARKED`。
4. 返回值语义是：
   - `true`：之前已经 mark 过
   - `false`：这次是第一次 mark

`DiscoverObject(obj)` 在此之上再加一层 collector 语义：

1. 调 `MarkObject(obj)`。
2. 只有第一次 `white -> grey` 成功时才返回 `true`。
3. 同时递增 `markedObjectCount`。

当前实现默认由外层调用点保证传进来的对象已经满足：

1. 非空
2. 属于 managed heap

### 5.2 `TryEnqueueGreyObject()`

当前唯一的新 grey 生产入口是：

```text
TryEnqueueGreyObject(BaseObject* obj, GreyWorkSink& sink)
TryEnqueueGreyObject(BaseObject* obj, GreyWorkQueue& queue)
```

它的固定协议是：

```text
DiscoverObject(obj)
    -> success 才 enqueue
```

这意味着：

1. 新 grey 对象一定先完成 `white -> grey` 的原子状态转换。
2. 只有第一次成功的人，才有资格把对象放入 grey queue。
3. 队列本身不负责去重，去重发生在 enqueue 前。

`DiscoverAndPushObject()` 是这条协议在普通 `WorkStack` 上的包装，它内部仍然经由统一的 enqueue helper 把 newly-grey 对象送进 `WorkStack`。

### 5.3 `TraceRefField()` 的发现逻辑

对象 trace 时，`TraceRefField()` 会：

1. 读出引用字段目标。
2. release 构建下直接调用 `DiscoverAndPushObject(target, workStack)`。
3. debug 构建下先读一次 `IsMarkedObject(target)` 做快路径判断；如果目标当前还没 mark，再调用 `DiscoverAndPushObject(target, workStack)`。

这里真正的语义是：

```text
release:
    child discovery
        -> DiscoverObject
        -> enqueue on first white -> grey

debug:
    child discovery
        -> DiscoverObject when target still looks white
```

当前实现的核心仍然是 **mark-before-enqueue**。  
release 路径不依赖 `IsMarkedObject(target)` 这类预检查，真正的唯一判定仍然在 `DiscoverObject()` 里完成。

### 5.4 consumer 默认相信协议

当前 `RunCollectorDrainSlice()`、`MSMarkAssist()`、`ExecuteLikeDrainMainQueue()` 在 pop 出对象之后，默认相信这条协议：

```text
凡是出现在 grey queue 里的对象，都应该已经是 marked 的 grey work
```

debug 模式下，会通过 `DebugVerifyQueuedGreyObject()` 显式校验这个不变量。

---

## 6. ConcurrentMark

`ConcurrentMark()` 会先切到 `GC_PHASE_MS_CONCURRENT_MARK`，然后做三件事：

1. 清零 assist / global queue 相关统计。
2. `assistEnabled_ = true`
3. 调 `RunConcurrentMarkLoop(markWorkStack_)`

### 6.1 GC 线程主循环

`RunConcurrentMarkLoop()` 的主循环很直接：

1. 本地 work 足够多时，`ShareWorkToGlobal()`，把一半 buffer share 给全局队列。
2. 如果本地 backlog 仍然明显大于共享阈值，并且全局队列为空，GC 线程才会额外暴露 1 个 buffer 到 global，给 mutator assist 留一个可见的偷取窗口。
3. 本地不空时，`RunCollectorDrainSlice()` 消费一个 slice。
4. 本地空时，`PopFromGlobal()` 从全局补货；如果 global 里只剩下刚为 assist 暴露的保留 buffer，GC 会先短暂让步，再决定是否取回。
5. 本地和全局都空时，累计 `convergenceStableRounds_`。
6. 连续空到 `kConvergenceRequiredRounds`，才认为并发标记收敛。

因此当前收敛判断依赖的是：

```text
GC local work empty
    && global work empty
    && 连续多轮稳定为空
```

### 6.2 `RunCollectorDrainSlice()`

GC 线程 drain 一个对象时的当前语义是：

1. pop 一个对象。
2. 过滤空指针和非 heap 地址。
3. fatal 校验对象 `stateWord` 必须有效。
4. debug 校验该对象必须已经 marked。
5. 没有引用字段就结束。
6. `WeakRef` 走专门分支。
7. 普通对象走 `TraceObjectRefFields(obj, workStack.UnsafeRaw())`。

这里有两个要点：

1. pop 出来后不会再做 mark。
2. 对象 trace 完后直接退出当前 grey work 系统。

当前的 black 语义就是：

```text
这个对象已经被 pop 出来并完成了字段 trace
```

而不是“对象头/bitmap 上有一个 black 位”。

---

## 7. 写屏障与 Assist

### 7.1 `MSMarkBarrier`

当前写屏障是混合屏障，写引用时的顺序是：

```text
discover(old)
    -> EnqueueRef(oldRef)
store(new)
    -> field.SetTargetObject(ref)
discover(new)
    -> EnqueueRef(newRef)
```

`EnqueueRef()` 的流程是：

1. 过滤空指针和非 heap 地址。
2. 取当前 mutator 的 `WriteBarrierBuffer`。
3. 如果 buffer 已满，先通过 `activeCollector_` 把旧 buffer 批量 flush 到 `globalWorkQueue_`。
4. 再通过 collector 的统一 enqueue helper，把首次 discover 成功的对象写入 `WriteBarrierBuffer`。

因此写屏障缓冲里放的不是“所有写过的对象”，而是“当前这次写第一次 discover 成功的 grey 对象”。

### 7.2 `WriteBarrierBuffer`

`WriteBarrierBuffer` 是第一层临时缓冲：

1. 每 mutator 一个。
2. 容量固定。
3. `Push()` 是单写者追加。
4. `Flush()` 是批量搬运到目标 work stack。

它本身不负责判断对象是否首次被发现；首次发现由 `TryEnqueueGreyObject()` 保证。

### 7.3 `MSMarkAssist()`

assist 不是独立线程，而是由分配路径被动触发。

当前小对象分配成功后，如果 mutator phase 处在：

1. `GC_PHASE_MS_MARK_SETUP`
2. `GC_PHASE_MS_CONCURRENT_MARK`
3. `GC_PHASE_MS_MARK_TERMINATION`

则不会再对新对象执行 alloc-black。新对象所在的 active/fresh region 不属于 MarkSetup 冻结的本轮 sweep snapshot；该 region 归还 central pool 时会按当前 `globalSweepGen` 进入 swept 队列，留到下一轮 GC 再成为 sweep 候选。

分配路径仍会累计 mutator assist 债务；债务达到阈值后，才会考虑触发 `MSMarkAssist()`。

assist 的当前规则是：

1. 优先继续处理本地残留 `assistLocalWork_`。
2. 本地空时才尝试从 `globalWorkQueue_` 补货。
3. 只有 collector 已经把 reserve work 暴露到 `globalWorkQueue_`，或者全局本来就有共享 work 时，assist 才会真正进入 drain。
4. 单次 assist 最多处理 `kAssistMaxObjectsPerCall` 个对象。
5. 本地 work 堆积过多时，`ShareWorkToGlobal()` 分流给共享队列。

### 7.4 assist 里的特殊项处理

assist 与 GC 线程最大的差别，是它不能处理所有对象类型。

当前明确被单独剥离出来的，是 `WeakRef`：

1. assist 遇到 `WeakRef`，不会继续走共享 grey queue。
2. 它会把对象转存到 `deferredMainThreadQueue_`。
3. 等 `MarkTermination()` 再由 GC 主线程统一处理。

所以当前并发标记的数据流实际上是：

```text
newly discovered grey
    -> markWorkStack_ / WriteBarrierBuffer / assistLocalWork_
    -> 必要时搬运到 globalWorkQueue_
    -> assist 不能处理的特殊项转到 deferredMainThreadQueue_
```

---

## 8. MarkTermination

`MarkTermination()` 是当前 Step2 唯一明确的 STW 收口点。

进入 `ScopedStopTheWorld(...)` 之后，第一件事就是：

```text
activeCollector_ = nullptr
```

这表示并发标记窗口已经关闭，后续不会再有新的 assist / barrier 并发路径加入当前 collector。

### 8.1 `mainQueue` 的三类来源

当前 `mainQueue` 的输入明确分成三类：

1. **direct source**
   - `FlushAllWriteBarrierBuffers(mainQueue)`
2. **pending concurrent work**
   - `FlushAllAssistLocalWorks(mainQueue)`
   - `DrainGlobalWorkQueueToMainQueue(mainQueue)`
   - `DrainDeferredMainThreadQueueToMainQueue(mainQueue)`
3. **resurrection / finalizer related candidates**
   - `CollectResurrectionCandidatesToMain(mainQueue)`
   - 以及其他需要通过 `TryEnqueueGreyObject()` 补进主队列的候选项

这套分类很重要，因为它对应了当前 `MarkTermination total` 日志里的含义：

1. `flushed`：direct source + pending concurrent work
2. `resurrection`：额外补入的 resurrection/finalizer candidates
3. `processed`：本次 STW drain 实际弹出的对象数
4. `newlyMarked`：STW 阶段新完成 `white -> grey` 的对象数

### 8.2 `ExecuteLikeDrainMainQueue()`

STW 收口时并不会切换到另一套 trace 算法，而是沿用和并发阶段同一套协议：

1. pop 一个 grey item。
2. debug 校验它必须已 mark。
3. 没有引用字段就结束。
4. `WeakRef` 走专门分支。
5. 普通对象继续 `TraceObjectRefFields(obj, mainQueue.UnsafeRaw())`。

因此当前 `MarkTermination()` 的本质不是“改用另一种标记模型”，而是：

```text
把并发阶段没来得及消费完的 grey work 汇总起来
    -> 用同一套 queue protocol 做最后闭包
```

### 8.3 allocator 侧收尾

mark 收口之后，termination 还会立刻做两件 allocator 相关工作：

1. `FlushAllAllocBuffers()`
2. `ReenableLazySweep()`

`FlushAllAllocBuffers()` 归还的 active TL/cache region 不进入本轮 unswept backlog，而是被 `CentralRegionPool` 盖上当前 `globalSweepGen` 后放入 swept 队列。这样它们绕过当前 sweep snapshot，但在下一次 MarkSetup 的 `PrepareForSweep()` 中仍会正常翻入 unswept，参与下一轮 sweep。

### 8.4 `GC_PHASE_MS_SWEEP` 在 STW 内切换

当前 phase 切换到 `GC_PHASE_MS_SWEEP` 发生在 STW 内部。

这样做的效果是：

1. mutator 恢复时看到的已经是 `MS_SWEEP`
2. 不会出现“mutator 已恢复，但最后一批 mark work 还没收口”的相位空窗

---

## 9. PostMarkTermination 与 ConcurrentSweep

### 9.1 `PostMarkTermination()`

当前 `PostMarkTermination()` 只做 mark 结果的后处理，顺序固定为：

1. `WeakRefBuffer::Instance().ClearWeakRefBuffer()`
2. `ProcessFinalizers()`
3. `CollectLargeGarbage()`

其中：

1. weak ref 清理基于已经稳定下来的 mark 结果进行。
2. finalizer 只负责把不可达 finalizable 对象送到处理器，不在这里直接执行 finalizer。
3. large object 清理走 `CentralRegionPool::SweepLargeRegions()`。

### 9.2 `ConcurrentSweep()`

`ConcurrentSweep()` 已经不再参与 Step2 的标记收敛，它做的是：

1. 基于稳定的 mark 状态计算 `trueLiveBytes`
2. 通知 `MSRegionManager` / `CentralRegionPool` 启动后台 sweep

因此它是“消费 mark 结果”的开始，不再是 mark 的一部分。

---

## 10. 新对象与对象状态总结

### 10.1 `MarkNewObject()`

当前新对象在标记相关 phase 下不再使用 alloc-black：

1. `AllocBufferMS::Allocate()` 不再调用 `MarkNewObject(obj)`。
2. `MarkSweepCollector::MarkNewObject()` 保留为空实现，避免新对象仅因分配而设置 mark bit。
3. `RegionSpaceMS::RegisterLargeObjectMS()` 也不再给新 large object 设置 `FLAG_MARKED`。
4. 新对象依赖 region snapshot 逃逸本轮 sweep：GC 开始后的 fresh / TL region 以当前 `globalSweepGen` 进入 swept 队列，下一轮 MarkSetup 才会被翻入 unswept。

因此当前新对象的语义是：

```text
new object during marking
    -> not marked by allocation
    -> protected by region generation from current sweep
    -> becomes next-cycle sweep candidate when MarkSetup flips swept lists
```

### 10.2 当前 Step2 的最终状态模型

把当前代码压缩成一句话，就是：

```text
发现对象时先原子 mark
    -> 第一次成功的人负责 enqueue
    -> consumer pop 后直接 trace
    -> trace 完成后对象自然退出 grey work 系统
```

换成对象状态表示，就是：

| 状态 | 当前表达方式 |
| --- | --- |
| white | 未 mark |
| grey | 已 mark，并且当前停留在 grey queue / grey work 系统中 |
| black | 已 mark，且已经被 drain 完，不再位于任何 grey queue 中 |

这就是当前 Step2 的完整语义边界。

---

## 11. 参数与可观测性

当前实现里和 mark 直接相关的关键常量包括：

| 常量 | 当前值 | 含义 |
| --- | --- | --- |
| `kSliceMaxObjects` | `256` | GC 线程单次 drain slice 最多处理的对象数 |
| `kShareWorkBufs` | `2` | GC 本地 work stack 达到该 buffer 数时，share 一半到全局 |
| `kAssistMaxObjectsPerCall` | `256` | 单次 assist 最多处理对象数 |
| `kAssistLocalFlushBufs` | `2` | assist 本地工作超过该 buffer 数时开始 share |
| `kAssistProbeIntervalCalls` | `8` | 全局看起来为空时，每 8 次 assist 做一次强制 probe |
| `kConvergenceRequiredRounds` | `3` | 本地与全局连续空 3 轮才判定并发标记收敛 |
| `AllocBufferMS::kGCAssistTriggerBytes` | `64KB` | 分配债务达到阈值后才考虑触发 assist |

当前排查 mark 阶段时，最值得看的可观测项是：

1. `MSGCCycleStats`
   - `preEnumRootsUs`
   - `markSetupUs`
   - `enumRootsUs`
   - `concurrentMarkUs`
   - `markTerminationUs`
   - `postMarkUs`
   - `assistCallCount / assistProcessedObjects / assistTracedObjects`
   - `markTermProcessedObjects / markTermNewlyMarkedObjects`
2. `MarkTermination total` 日志
   - `flushed`
   - `resurrection`
   - `processed`
   - `newlyMarked`
3. `MSMarkBarrier_DumpAndResetCounters()`
   - 观察混合写屏障在一个 GC 周期内的 discover 计数
4. `AssistObs` 日志
   - `debtHits / noCollector / noVisibleWork`
   - `visible(local/global)`
   - `entryLocal / refill(attempt/empty)`
   - `markAlloc(small/large)`
   - `localPeak`

如果要继续沿着代码看下去，当前最推荐的阅读顺序是：

1. `DoGarbageCollection()`
2. `PreEnumRoots()` / `EnumRoots()`
3. `DiscoverObject()` / `TryEnqueueGreyObject()`
4. `RunConcurrentMarkLoop()` / `RunCollectorDrainSlice()`
5. `MSMarkBarrier::EnqueueRef()` / `MSMarkAssist()`
6. `MarkTermination()` / `ExecuteLikeDrainMainQueue()`
7. `PostMarkTermination()` / `ConcurrentSweep()`

---

## 12. 当前回归验证

本轮 snapshot/no-alloc-black 调整后，已完成一次 seed5 case2 回归：

```text
Result dir: MSGC_RESULT/Result/case2_50x_pressure/20260426_snapshot_no_allocblack_seed5
cjHeapSize: 4gb
CJ_MS_FORCE_ENQUEUE_MARKED: unset
CJ_MS_SWEEP_DIAG: unset
Functional: 5/5 passed
F5_HashMap: PASS 8034 ms (1000 rounds)
Benchmark exit code: 0
Peak RSS: 117.8 MB
```

这说明当前修复路径已经能覆盖 case2/F5 的直接失败场景，且不依赖“已 marked 对象也强制入队”的诊断开关。
