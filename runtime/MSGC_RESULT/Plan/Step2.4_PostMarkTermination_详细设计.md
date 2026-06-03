# Step 2.4：Post Mark Termination 详细设计

> 前置：[Step 2.3：Mark Termination（STW）详细设计](Step2.3_MarkTermination_STW详细设计.md)
>
> 2026-04-05 当前代码状态校准：
> 1. `PostMarkTermination` 当前只负责 STW 之后的清理、日志和后台阶段衔接；`GC_PHASE_MS_SWEEP` 的切换已经在 Step2.3 的 STW 中完成。
> 2. 文中如果仍把 SATB drain、`MarkSatbBuffer` 或“在 PostMarkTermination 中决定主收敛队列”写成现状，应以 Step2.3 的当前实现为准。
> 3. 物理回收与 region 返还 OS 的主路径当前也不在这里完成，而是由 Step4 的 `emptySweptCache` / background scavenge 体系处理。

---

## 目录

- [1. 目标与边界](#1-目标与边界)
- [2. 与 Step2.3 的交接](#2-与-step23-的交接)
- [3. 任务清单](#3-任务清单)
- [4. 各任务说明](#4-各任务说明)
- [5. 伪代码](#5-伪代码)
- [6. 并发安全分析](#6-并发安全分析)

---

## 1. 目标与边界

Step2.3 MarkTermination（STW）结束后，标记阶段已完成，mark bitmap 稳定，屏障已切换到 Sweep 模式，mutator 已恢复运行。

本阶段负责**标记结束后、Sweep 实际回收前**的清理和准备工作。这些操作读取的是已稳定的标记结果，不需要 STW 保护。

## 2. 与 Step2.3 的交接

Step2.3 退出时的状态：

- mark bitmap 已稳定（所有可达对象已标记）
- `WeakRefBuffer` 中记录了标记期间遇到的所有弱引用对象（尚未清理）
- SATB 缓冲中的对象已处理完毕，但缓冲区本身未释放
- 屏障已通过 `TransitionToGCPhase(MS_SWEEP)` 切换
- mutator 已恢复

## 3. 任务清单

| 序号 | 任务 | 来源 | 说明 |
|------|------|------|------|
| 1 | `ClearWeakRefBuffer()` | WCollector PostTrace | 遍历 WeakRefBuffer，referent 未标记则置空 |
| 2 | 无独立 SATB 清理任务 | 当前 MS | `WriteBarrierBuffer` 已在 Step2.3 的 STW 内完成 flush/clear |
| 3 | `ProcessFinalizers()` | WCollector TraceHeap | 将未标记的 finalizer 对象入队，通知 Finalizer 线程执行析构 |
| 4 | `PrepareCycleRef()` | WCollector PostTrace | OHOS 专用：合并 discoveredExternObjects 到 cycleRefWorkStack |
| 5 | `PostResolveCycleTask()` | WCollector DoGarbageCollection | OHOS 专用：兜底触发 CJ_MRT_RolveCycleRef |
| 6 | `CollectLargeGarbage()` | WCollector PostTrace | 当前主路径回收堆内 large region；`LargeObjectSpace` 如存在则仅作为辅助兼容路径 |

> 注：WCollector PostTrace 中的 `HandleTraceRegions`、`RefineFromSpace`、`fwdTable.PrepareForwardTable` 是 Copy GC forward 流程专用，MS 不需要。
> 注：WCollector 的 `MergeResurrectExportObjects` 和 `PreforwardDiscoveredExternObjects` 是 forward 阶段对象地址更新，MS 无 forward，不需要。
> 注：WCollector PostTrace 中的 `CollectPinnedGarbage` 在 MS 中不需要，pinned region 由 Sweep 阶段统一处理。

## 4. 各任务说明

### 4.1 ClearWeakRefBuffer — 弱引用终态清理

**作用**：遍历标记期间收集的所有弱引用对象，检查其 referent 的标记状态：
- referent 已标记（存活）→ 不做处理
- referent 未标记（死亡）→ 将 referent 字段置空

**为什么不需要 STW**：
- mark bitmap 在 Step2.3 结束后不再变化，读取标记状态是安全的
- 写入 referent 字段（置空）是原子操作，与 mutator 的弱引用读取通过 `ReadWeakRef` 屏障协调
- 现有 WCollector 的 `ClearWeakRefBuffer` 也在 PostTrace 中执行，不在 STW 内

**现有实现**：`WeakRefBuffer::Instance().ClearWeakRefBuffer()`（[SatbBuffer.h](../src/Mutator/SatbBuffer.h) L301-323）

### 4.2 屏障缓冲后处理（当前 MS 说明）

历史设计中的 `ClearSatbBuffer()` 对应 CopyGC/SATB retired-node 释放。当前 MSGC 主路径不再依赖 SATB 作为写屏障收敛核心。

当前真实语义是：
- Step2.3 的 STW 内已经完成 `FlushAllWriteBarrierBuffers()` 并 drain 主队列；
- PostMarkTermination 不再负责决定主收敛队列，也不再承担 SATB retired-node 释放；
- 如果 runtime 中仍保留 `SatbBuffer` 兼容设施，也不应把它视为当前 MS 主流程的一部分。

### 4.3 ProcessFinalizers — Finalizer 入队

**作用**：遍历 FinalizerProcessor 中注册的 finalizer 对象，将未标记（不可达）的对象入队到 Finalizer 线程，由其异步执行析构函数。

**为什么不需要 STW**：
- mark bitmap 已稳定，判断 `!IsMarkedObject(obj)` 安全
- `EnqueueFinalizables` + `Notify` 是线程安全的（FinalizerProcessor 内部有锁）
- 现有 WCollector 也在 TraceHeap 末尾（非 STW）调用

**现有实现**：
```cpp
void TracingCollector::ProcessFinalizers() {
    std::function<bool(BaseObject*)> finalizable = [this](BaseObject* obj) {
        return !IsMarkedObject(obj);
    };
    FinalizerProcessor& fp = collectorResources.GetFinalizerProcessor();
    fp.EnqueueFinalizables(finalizable, snapshotFinalizerNum);
    fp.Notify();
}
```

**注意**：`snapshotFinalizerNum` 由 Step2.3 的 `CollectResurrectionCandidatesToMain` 中 `VisitFinalizers` 返回值设置。

### 4.4 PrepareCycleRef — 跨语言循环引用准备（OHOS）

**作用**：将本轮标记阶段发现的 `discoveredExternObjects`（export 对象 → foreign 对象列表映射）合并到持久化的 `cycleRefWorkStack`，供 mutator 侧的 `ResolveCycleRef` 消费。

**数据流**：
```
标记阶段: DFSTraceExportObject → 填充 discoveredExternObjects
Step2.3:  CollectExternCandidatesToMain → 补标未标记的 extern 对象
Step2.4:  PrepareCycleRef → 合并到 cycleRefWorkStack，清空 discoveredExternObjects
之后:     ResolveCycleRef (mutator) → 从 cycleRefWorkStack 消费，调用 ResolveCycleRefStub
```

**为什么不需要 STW**：
- `cycleRefWorkStack` 受 `cycleWorkStackMtx` 保护
- `discoveredExternObjects` 在 GC 线程独占（标记完成后 mutator 不写入）
- WCollector 中也在非 STW 的 PostTrace 调用

**MS 简化**：与 WCollector 实现完全一致，无需修改：
```cpp
void PrepareCycleRef() {
    std::lock_guard<std::mutex> lg(cycleWorkStackMtx);
    cycleRefWorkStack.insert(discoveredExternObjects.begin(), discoveredExternObjects.end());
    discoveredExternObjects.clear();
}
```

> MS 不需要 `PreforwardDiscoveredExternObjects`（无 forward，对象地址不变）。
> MS 不需要 `MergeResurrectExportObjects`（无 forward 阶段，不存在 `resurrectedExportObjectesForwardPhase`）。

### 4.5 PostResolveCycleTask — 跨语言循环引用解析通知（OHOS）

**作用**：通过 `CJ_MRT_RolveCycleRef()` 向 OHOS 事件循环（UI 线程）post 一个任务，触发 `ResolveCycleRef` 在 mutator 上下文中执行。

**为什么是“通知”而不是“直接执行”**：
- `ResolveCycleRef` 需要 `ScopedObjectAccess`（mutator 上下文）
- 需要调用 ArkTS 侧的 `ResolveCycleRefStub`，必须在 UI 线程执行
- 因此 `CJ_MRT_RolveCycleRef` 实际是通过 `RunResolveCycle` → `PostTask` 往事件循环投递任务

**WCollector 中放在 Forward 之后的原因**：Copy GC 的 forward 会更新对象地址，`ResolveCycleRef` 需要使用最新地址。MS 无 forward，对象地址不变，可在 `PrepareCycleRef` 之后立即调用。

**现有实现**：
```cpp
void PostResolveCycleTask() {
#if defined(__OHOS__)
    if (cycleRefWorkStack.empty()) return;
    CJ_MRT_RolveCycleRef();
#endif
}
```

### 4.6 CollectLargeGarbage — 大对象 region 回收

**作用**：遍历 `oldLargeRegionList`，对未标记的大对象 region 按大小采取不同回收策略：

| region 类型 | 操作 | 说明 |
|-------------|------|------|
| 堆内 large region（当前主路径） | 回收后交给当前 large-region / region-manager 路径处理 | 当前实现的 large object 主分配与主回收都围绕堆内 large region 展开 |
| `LargeObjectSpace` 对象（辅助路径） | 如存在则走 LOS 自身兼容回收 | 不应再把它理解成“>4MB 即主路径直接释放物理页” |

对于存活的大对象 region，仅重置其 mark bit（`ResetMarkBit`）以备下次 GC 使用。

**为什么放在 PostMarkTermination**：
- 大对象每个独占一个 region，只需检查 offset=0 的 mark bit，判断开销极低
- 无需等到 Sweep 阶段，提前回收可尽早释放内存，减少内存压力
- mark bitmap 已稳定，读取安全

**现有实现**：`RegionManager::CollectLargeGarbage()`（[RegionManager.cpp](../src/Heap/Allocator/RegionManager.cpp) L705-740）

MS 可直接复用，通过 `RegionSpace::CollectLargeGarbage()` 调用。

## 5. 伪代码

```cpp
void MarkSweepCollector::PostMarkTermination()
{
    MRT_PHASE_TIMER("PostMarkTermination");

    // 1) 弱引用终态清理：referent 未标记则置空
    WeakRefBuffer::Instance().ClearWeakRefBuffer();

    // 2) 无独立 SATB 清理任务；WBBuf 已在 MarkTermination 中 flush/clear

    // 3) Finalizer 入队：通知 Finalizer 线程处理不可达的 finalizer 对象
    ProcessFinalizers();

    // 4) OHOS 跨语言循环引用：合并 extern 对象到 cycleRefWorkStack
    PrepareCycleRef();

    // 5) OHOS 跨语言循环引用：通知 UI 线程解析循环引用
    PostResolveCycleTask();

    // 6) 大对象回收：当前主路径回收堆内 large region；LOS 仅作辅助兼容
    CollectLargeGarbage();
}
```

调用位置在 `DoGarbageCollection` 主流程中，紧接 `MarkTermination` 之后：

```cpp
void MarkSweepCollector::DoGarbageCollection()
{
    MarkSetup();          // Step2.1 (STW)
    ConcurrentMark();     // Step2.2
    MarkTermination();    // Step2.3 (STW)
    PostMarkTermination();// Step2.4 （非 STW，并发安全，包含 CycleRef 通知）
    Sweep();              // Step3（待设计）
}
```

## 6. 并发安全分析

本阶段执行时，mutator 已恢复运行。各操作的并发安全性：

| 操作 | 读 | 写 | 与 mutator 竞争 | 安全机制 |
|------|----|----|-----------------|---------|
| `ClearWeakRefBuffer` | mark bitmap（稳定） | referent 字段（原子置空） | mutator 可能通过 `ReadWeakRef` 读 referent | `ReadWeakRef` 屏障会检查 referent 存活性，置空后返回 null，语义一致 |
| 屏障缓冲后处理 | 无新增主任务 | `WriteBarrierBuffer` 已在 Step2.3 收敛 | 无（屏障已切到 sweep/idle 语义） | 安全 |
| `ProcessFinalizers` | mark bitmap（稳定） | FinalizerProcessor 内部队列 | 无（FinalizerProcessor 内部加锁） | 线程安全 |
| `PrepareCycleRef` | `discoveredExternObjects`（GC 独占） | `cycleRefWorkStack`（加锁） | mutator 的 `ResolveCycleRef` 也持锁 | `cycleWorkStackMtx` 互斥保护 |
| `PostResolveCycleTask` | `cycleRefWorkStack` | 调用外部 resolve 函数 | 独立调度 | OHOS 侧保证安全 |
| `CollectLargeGarbage` | mark bitmap（稳定）、`oldLargeRegionList` | region 状态（回收/释放） | mutator 不在 old large region 上分配 | region list 操作在 GC 线程独占 |