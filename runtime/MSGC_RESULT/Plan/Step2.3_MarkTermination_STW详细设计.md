# Step 2.3：Mark Termination（STW）详细设计

> 本文档从 [Step 2：三色标记与 Sweep 详细设计](Step2_三色标记与Sweep详细设计.md) §6 拆分而来。
> 2026-04-05 按当前 `MarkSweepCollector::MarkTermination()` 实现校准：当前收敛主线不再是 `MarkSatbBuffer`，而是 `WriteBarrierBuffer + assistLocalWork + globalWorkQueue_ + resurrection candidates` 的 STW 汇总与 drain。

---

## 目录

1. [阶段目标](#1-阶段目标)
2. [输入与输出不变量](#2-输入与输出不变量)
3. [当前真实流程](#3-当前真实流程)
4. [各类输入源的地位](#4-各类输入源的地位)
5. [当前伪代码](#5-当前伪代码)
6. [与 Step2.4 的边界](#6-与-step24-的边界)
7. [实现结论](#7-实现结论)

---

## 1. 阶段目标

`MarkTermination` 当前的职责是：

1. 在 STW 下冻结所有 Mutator。
2. 汇总并发标记尾部残留工作。
3. 用与并发标记同构的 drain 逻辑把主队列清空。
4. 在 Mutator 恢复前完成 `PrepareForSweep()`，并把 phase 切到 `GC_PHASE_MS_SWEEP`。

它不是一个“重新跑一遍 SATB 收尾”的阶段，也不是弱引用/终结器的最终处理阶段。

---

## 2. 输入与输出不变量

### 2.1 进入本阶段前

来自 `ConcurrentMark()` 的真实状态：

1. GC 线程本地 `markWorkStack_` 已达到稳定收敛。
2. `globalWorkQueue_` 可能仍有残留。
3. 某些 Mutator 的 `assistLocalWork_` 可能仍有未做完的局部工作。
4. `WriteBarrierBuffer` 可能仍缓存着最新写屏障产生的灰对象。
5. `activeCollector_` 仍对写屏障可见，直到进入 STW 后才清空。

### 2.2 退出本阶段后

当前代码保证：

1. `WriteBarrierBuffer` 残留已并入主队列。
2. `assistLocalWork_` 与 `globalWorkQueue_` 已并入主队列并完成 drain。
3. resurrection candidates 已纳入补标记收敛。
4. `CentralRegionPool::FlushAllAllocBuffers()` 与 `PrepareForSweep()` 已执行。
5. phase 已在 STW 内切到 `GC_PHASE_MS_SWEEP`。

不在本阶段完成的工作：

- `WeakRefBuffer::ClearWeakRefBuffer()`
- `ProcessFinalizers()`
- `CollectLargeGarbage()`

这些都在 [Step2.4](Step2.4_PostMarkTermination_详细设计.md) 的 `PostMarkTermination()` 中执行。

---

## 3. 当前真实流程

当前 `MarkSweepCollector::MarkTermination()` 的顺序是：

```text
1. ScopedStopTheWorld(..., GC_PHASE_MS_MARK_TERMINATION)
2. activeCollector_ = nullptr
3. mainQueue = NewWorkStack()
4. FlushAllWriteBarrierBuffers(mainQueue)
5. FlushAllAssistLocalWorks(mainQueue)
6. DrainGlobalWorkQueueToMainQueue(mainQueue)
7. CollectResurrectionCandidatesToMain(mainQueue)
8. ExecuteLikeDrainMainQueue(mainQueue)
9. centralPool->FlushAllAllocBuffers()
10. centralPool->PrepareForSweep()
11. TransitionToGCPhase(GC_PHASE_MS_SWEEP, false)
```

几个关键事实：

1. **direct source 当前只包含 `WriteBarrierBuffer`**。
2. assist-local 和 global queue 被当作“并发标记残留工作”，在 direct source 之后统一回灌。
3. resurrection candidates 单独统计，但仍和主队列一起用同一套 drain 逻辑处理。
4. phase 切换到 `MS_SWEEP` 发生在 STW 内部，避免恢复 Mutator 后再出现“最后一次写屏障缓冲未被收敛”的窗口。

---

## 4. 各类输入源的地位

### 4.1 当前直接输入源

| 来源 | 当前是否进入 `MarkTermination` 主线 | 说明 |
|------|------------------------------------|------|
| `WriteBarrierBuffer` | 是 | 通过 `FlushAllWriteBarrierBuffers(mainQueue)` 进入 |
| `assistLocalWork_` | 是 | 通过 `FlushAllAssistLocalWorks(mainQueue)` 进入 |
| `globalWorkQueue_` | 是 | 通过 `DrainGlobalWorkQueueToMainQueue(mainQueue)` 进入 |
| resurrection candidates | 是 | 通过 `CollectResurrectionCandidatesToMain(mainQueue)` 进入 |

### 4.2 当前不在主线中的来源

| 来源 | 当前状态 |
|------|----------|
| `MarkSatbBuffer` | 不是 `MarkTermination()` 的主收敛入口 |
| `CollectSatbCandidatesToMain()` | 当前 `MarkTermination()` 不调用 |
| `CollectExternCandidatesToMain()` | helper 仍在，但当前 `MarkTermination()` 不走这条路径 |

因此，这一阶段的文档不能再把 `SATB buffer drain` 写成主轴。

---

## 5. 当前伪代码

```cpp
void MarkSweepCollector::MarkTermination()
{
    ScopedStopTheWorld stw("MS MarkTermination", true, GCPhase::GC_PHASE_MS_MARK_TERMINATION);

    activeCollector_.store(nullptr, std::memory_order_release);

    WorkStack mainQueue = NewWorkStack();

    FlushAllWriteBarrierBuffers(mainQueue);
    FlushAllAssistLocalWorks(mainQueue);
    DrainGlobalWorkQueueToMainQueue(mainQueue);
    CollectResurrectionCandidatesToMain(mainQueue);

    ExecuteLikeDrainMainQueue(mainQueue);

    if (CentralRegionPool* centralPool = space.GetCentralPool()) {
        centralPool->FlushAllAllocBuffers();
        centralPool->PrepareForSweep();
    }

    TransitionToGCPhase(GCPhase::GC_PHASE_MS_SWEEP, false);
}
```

这里的 `ExecuteLikeDrainMainQueue()` 与并发标记阶段采用同样的“pop grey object -> mark -> trace children -> set scanned”逻辑，因此 `MarkTermination` 不是另一套标记器，而是一次 STW 下的最终 drain。

---

## 6. 与 Step2.4 的边界

当前阶段划分应明确为：

### Step2.3 `MarkTermination`

- 负责补齐“还没被真正 trace 完”的灰对象工作。
- 负责为下一轮 sweep 代建立稳定起点。

### Step2.4 `PostMarkTermination`

- 处理弱引用清理。
- 处理终结器入队。
- 处理大对象垃圾回收。
- 记录 `PostMark` 日志与统计。

当前代码中，这条边界已经是清晰分离的，文档不应再把 `WeakRef` 清理或 `CollectLargeGarbage` 归入 `MarkTermination` 主体。

---

## 7. 实现结论

截至当前源码，`MarkTermination` 的正确表述应是：

> 一个 STW 下的最终工作汇总与 drain 阶段，直接从 `WriteBarrierBuffer`、assist 局部残留、collector 全局队列以及 resurrection candidates 收敛尾部工作，然后在恢复 Mutator 前完成 `PrepareForSweep()` 并切到 `GC_PHASE_MS_SWEEP`。

`SATB` 基础设施仍存在，但它已经不是本阶段的当前主收敛路径。
