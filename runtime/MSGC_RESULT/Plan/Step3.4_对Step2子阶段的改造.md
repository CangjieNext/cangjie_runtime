# Step 3.4：对 Step2 各子阶段的具体改造

> 本文档是 [Step3 混合写屏障详细设计](Step3_混合写屏障详细设计.md) 的子文档。
> 描述从 SATB 删除屏障升级为混合写屏障后，[Step2](Step2_三色标记与Sweep详细设计.md) 各子阶段需要的具体改造。
>
> 2026-04-05 当前代码状态校准：
> 1. 当前 barrier 落地形态是 `MSMarkBarrier + WriteBarrierBuffer`；每个 mutator 持有固定大小本地 buffer，不再以文中旧稿里的 per-P `localWorkBuf` / `MarkSatbBuffer` 为主路径。
> 2. `MarkTermination` 当前收敛顺序是 `FlushAllWriteBarrierBuffers()`、`FlushAllAssistLocalWorks()`、`DrainGlobalWorkQueueToMainQueue()`、`CollectResurrectionCandidatesToMain()`，而不是 `CollectSatbCandidatesToMain()`。
> 3. 因此，文中后续涉及 SATB 主队列、per-P 本地工作队列的改造描述，应按历史设计阅读，以 [Step2.3](Step2.3_MarkTermination_STW详细设计.md) 和 [Step3.2](Step3.2_WriteBarrierBuffer详细设计.md) 的当前实现口径为准。

---

## 目录

1. [总览：SATB → 混合写屏障的影响范围](#1-总览satb--混合写屏障的影响范围)
2. [对 Step2.1 MarkSetup（Handshake）的改造](#2-对-step21-marksetuphandshake的改造)
3. [对 Step2.2 ConcurrentMark 的改造](#3-对-step22-concurrentmark-的改造)
4. [对 Step2.3 MarkTermination（STW）的改造](#4-对-step23-markterminationstw的改造)
5. [对 Step2.4 PostMarkTermination 的改造](#5-对-step24-postmarktermination-的改造)
6. [Phase 状态机更新](#6-phase-状态机更新)

---

## 1. 总览：SATB → 混合写屏障的影响范围

Step2 设计时以 **SATB 删除屏障** 为基线（参见 [Step2 §2.3](Step2_三色标记与Sweep详细设计.md)）。Step3 将其升级为 **Go 风格混合写屏障**（shade(old) + shade(new)）。以下矩阵总结了各子阶段受影响的组件：

| 子阶段 | SATB 相关组件 | 混合写屏障替代方案 | 改动量 |
|--------|-------------|-------------------|--------|
| **Step2.1 MarkSetup** | 安装 `MSTraceBarrier`（SATB） | 安装 `MSMarkBarrier`（混合） | 小 |
| | `HandleGCPhase` drain SATB node | `HandleGCPhase` flush WBBuf | 小 |
| **Step2.2 ConcurrentMark** | SATB buffer 异步处理 | WBBuf → `globalWorkQueue_` / assist drain 实时处理 | 中 |
| | "SATB 不在工作队列中" | WBBuf overflow 刷入全局队列，STW flush 进入主队列 | 设计变更 |
| **Step2.3 MarkTermination** | `CollectSatbCandidatesToMain()` | `FlushAllWriteBarrierBuffers()` | 中 |
| | SATB 候选标记循环 | drain 更少（混合已提前标记） | 减少 |
| **Step2.4 PostMarkTermination** | Task 2: `ClearSatbBuffer()` | 无独立 SATB 清理任务；WBBuf 已在 Step2.3 flush | 小 |
| | 其他 5 个 Task | **不受影响** | 无 |

**关键简化**：混合写屏障的 `MarkAndEnqueue` 在写屏障执行时**立即原子标记**（bitmap CAS），而 SATB 只是记录到 buffer 等待后续处理。这意味着混合写屏障下：
- MarkTermination 需要处理的"灰色残留"更少
- STW 停顿时间预期更短
- 不再需要 `CollectSatbCandidatesToMain` 中复杂的"未标记筛选"逻辑

---

## 2. 对 Step2.1 MarkSetup（Handshake）的改造

### 2.1 Handshake 机制不变

Step2.1 的核心设计是 **Handshake 而非 STW**：通过 `TransitionToGCPhase(MS_PHASE_MARK_SETUP)` 让每个 Mutator 在到达 safepoint 时独立执行转换。**这一机制完全不变**。

### 2.2 安装的 Barrier 类型变化

| 维度 | Step2 (SATB) | Step3 (混合) |
|------|-------------|-------------|
| 安装的 Barrier | `MSTraceBarrier` | `MSMarkBarrier` |
| 写操作行为 | 记旧值到 SatbBuffer | `EnqueueRef(old/new)`，`MarkObject` 成功后才入 WBBuf |
| 读操作行为 | 直接 load（无 tag） | 直接 load（无 tag） |

`InstallBarrier(MS_PHASE_MARK_SETUP)` 的映射从 `MSTraceBarrier` 改为 `MSMarkBarrier`。

### 2.3 HandleGCPhase 分支改造

Step2.1 §3.2 中，`HandleGCPhase(MS_PHASE_MARK_SETUP)` 包含三步：
1. `ClearTLRegionBitmap()` — **不变**
2. `FlushTLRegion()` — **不变**
3. `GcPhaseEnum(newPhase)`（栈根自扫描）— **不变**

Step2.1 §3.2 还有 `HandleGCPhase(MS_PHASE_MARK_TERMINATION)` 分支：

```cpp
// Step2 原始（SATB）
else if (newPhase == GCPhase::GC_PHASE_MS_MARK_TERMINATION) {
    std::lock_guard<std::mutex> lg(mutatorLock);
    if (satbNode != nullptr) {
        SatbBuffer::Instance().RetireNode(satbNode);
        satbNode = nullptr;
    }
}
```

**Step3 改造后**：

```cpp
// Step3 混合写屏障版本
else if (newPhase == GCPhase::GC_PHASE_MS_MARK_TERMINATION) {
    // flush per-thread WBBuf 到全局工作队列
    // 注意：此时 Mutator 即将暂停，WBBuf 中的残留灰对象必须被收集
    wbBuf_.Flush();
}
```

**改造要点**：
- 不再需要 `satbNode` 的 retire 操作（因为没有 SatbBuffer）
- 改为 flush WBBuf，将残留灰对象推入工作队列
- 比 SATB retire 更简单（WBBuf.Flush() 无需 mutex，直接 push batch）

### 2.4 MarkNewObject（alloc-black）不变

Step2.1 中，`MS_PHASE_MARK_SETUP` / `MS_PHASE_MARK` / `MS_PHASE_MARK_TERMINATION` 阶段的新分配对象标记为黑色。这一策略在混合写屏障下**完全不变**——新分配对象仍需标黑，否则可能被误回收。

## 3. 对 Step2.2 ConcurrentMark 的改造

### 3.1 SATB 处理模式的根本变化

Step2.2 §6 明确指出："SATB 记录不在上述队列中，统一在 MarkTermination 中处理"。这意味着在 Step2 SATB 模式下，SATB buffer 中的对象在并发标记阶段**不被处理**，全部积压到 MarkTermination STW 中。

混合写屏障**彻底改变了这个模式**：

| 维度 | Step2 (SATB) | Step3 (混合) |
|------|-------------|-------------|
| 灰对象产生 | 记旧值到 SatbBuffer（不标记） | **立即 MarkObject + 入 WBBuf** |
| 并发阶段处理 | SatbBuffer 不参与并发标记 | WBBuf overflow / assistLocalWork → **持续被 drain** |
| 积压到 STW | 全部 SATB 内容 | **仅 WBBuf 残留**（大部分已被 flush） |

### 3.2 WBBuf 与当前工作队列的集成

当前实现里，WBBuf flush **不会直接进入 per-P 本地工作队列**。实际接线如下：

```
Mutator 写屏障 → MarkObject(bitmap) + WBBuf.Push(obj)
                            ↓ (WBBuf 满)
                     FlushWriteBarrierOverflow(obj)
                            ↓
                       globalWorkQueue_.Push(obj)

MarkTermination(STW) → FlushAllWriteBarrierBuffers(mainQueue)
              → FlushAllAssistLocalWorks(mainQueue)
```

并发阶段的 drain 主要由 GC 线程和 assist 路径从 `globalWorkQueue_` / `assistLocalWork` 消费灰对象；STW 只需要处理残留的 mutator 本地 WBBuf。

### 3.3 对 Mark Assist 的影响

Step2.2 §4 定义了 Mark Assist 机制：Mutator 累积分配债务 `allocDebt`，通过 drain 工作来偿还。

混合写屏障对 assist 的影响：

1. **工作量分布更均匀**：shade(new) 使更多对象在并发阶段被提前标灰，而非积压到 MarkTermination
2. **per-P 队列更活跃**：WBBuf flush 持续向 per-P 队列注入灰对象，assist drain 有更多工作可做
3. **收敛更快**：灰对象被实时处理（而非等 STW），标记波前推进更快

### 3.4 不再需要 "SATB 统一处理" 声明

Step2.2 中原来的声明"SATB 记录统一在 MarkTermination 中处理"在混合写屏障下**不再成立**。替代声明为：

> **混合写屏障产生的灰对象，通过 WBBuf → per-P 队列，在并发标记阶段被实时处理。MarkTermination 中仅需 flush 残留 WBBuf。**

---

## 4. 对 Step2.3 MarkTermination（STW）的改造

### 4.1 核心变化：CollectSatbCandidatesToMain → 当前 STW drain 组合

Step2.3 旧稿中的 `CollectSatbCandidatesToMain()` 需要替换为当前运行中的 STW 收敛组合：

```
MarkTermination(mainQueue):
    1. FlushAllWriteBarrierBuffers(mainQueue)
    2. FlushAllAssistLocalWorks(mainQueue)
    3. DrainGlobalWorkQueueToMainQueue(mainQueue)
    4. CollectResurrectionCandidatesToMain(mainQueue)
    5. ExecuteLikeDrainMainQueue(mainQueue)
```

因此，Step3 对 Step2.3 的真实改造不是“把 SATB 第 3 步换个名字”，而是把整条 SATB 收敛链替换成当前 WBBuf/global-queue/assist 的组合路径。

```cpp
void MarkSweepCollector::CollectUnmarkedToMainQueue(WorkStack& mainQueue)
{
    PublishLocalOverflowToGlobal(mainQueue);
    CollectGCDrainQueueToMain(mainQueue);
    
    // Step3 替换：SATB 收集 → WBBuf flush
    // 注意：WBBuf 中的对象已经被 MarkObject 标记（bitmap CAS），
    // 但可能还未被 drain（引用字段还未扫描）。
    // Flush 将这些灰对象推入 mainQueue，由后续 ExecuteLikeDrainMainQueue 统一 drain。
    FlushAllWriteBarrierBuffers(mainQueue);
    
    CollectExternCandidatesToMain(mainQueue);
    CollectResurrectionCandidatesToMain(mainQueue);
}

void MarkSweepCollector::FlushAllWriteBarrierBuffers(WorkStack& mainQueue)
{
    MutatorManager::Instance().VisitAllMutators([&mainQueue](Mutator& mutator) {
        WriteBarrierBuffer& wbBuf = mutator.GetWriteBarrierBuffer();
        // 直接将 WBBuf 内容推入 mainQueue
        for (size_t i = 0; i < wbBuf.Size(); ++i) {
            BaseObject* obj = wbBuf.buf_[i];
            // 对象已被 MarkObject 标记，但可能尚未 drain 其引用字段
            // 推入 mainQueue 让 ExecuteLikeDrainMainQueue 处理
            if (obj != nullptr && Heap::IsHeapAddress(obj)) {
                mainQueue.push_back(obj);
            }
        }
        wbBuf.Clear();
    });
}
```

### 4.2 对比：SATB vs 混合在 MarkTermination 中的工作量

| 维度 | Step2 (SATB) | Step3 (混合) |
|------|-------------|-------------|
| SATB/WBBuf 处理 | 遍历 SatbBuffer + retired 链表 → 筛选未标记 → 入队 | 遍历 WBBuf → 直接入队（已标记但未 drain） |
| 筛选逻辑 | 需要 `IsMarkedObject` 检查（SatbBuffer 中有已标记对象） | **不需要标记检查**（WBBuf 中都是首次标记的灰对象） |
| 积压量 | 整个并发阶段的 SATB 记录 | **仅 WBBuf 残留**（并发阶段已实时 flush） |
| drain 主循环工作量 | 可能很大（SATB 积压 + 引用字段递归） | **更小**（大部分灰对象已在并发阶段被 drain） |

**预期效果：MarkTermination STW 时间显著缩短**。

### 4.3 无需重扫描栈

混合写屏障的一个关键优势是**栈上无需写屏障**（Go 1.8 论文的核心结论）。这意味着：
- MarkSetup 阶段扫描过的栈在 MarkTermination 中**无需重扫描**
- 栈上新写入的指针要么来自堆（堆写有 MSMarkBarrier 保护），要么来自栈（栈对象在 MarkSetup 已标记）

### 4.4 更新后的 MarkTermination 伪代码

```cpp
void MarkSweepCollector::MarkTermination()
{
    ScopedStopTheWorld stw("MS MarkTermination", true, 
                           GCPhase::GC_PHASE_MS_MARK_TERMINATION);

    DisableAssistEntry();
    WorkStack mainQueue;

    // 1) 收集所有残留灰对象到主队列
    //    包括 per-P 队列、GC 线程本地、WBBuf 残留、Finalizer、复活候选
    CollectUnmarkedToMainQueue(mainQueue);

    // 2) 统一 drain 主队列（递归扫描引用字段，标记子对象）
    ExecuteLikeDrainMainQueue(mainQueue);

    // 3) 切换到下一阶段
    TransitionToGCPhase(GCPhase::GC_PHASE_MS_POST_MARK, true);
}
```

## 5. 对 Step2.4 PostMarkTermination 的改造

### 5.1 Step2.4 的 6 个 Task 回顾

[Step2.4](Step2.4_PostMarkTermination_详细设计.md) 定义了标记后的 6 个清理任务：

1. `ClearWeakRefBuffer()` — 清理弱引用（referent 未标记则置空）
2. 屏障缓冲收尾 — 当前无需独立 `ClearSatbBuffer()` task ← **受影响**
3. `ProcessFinalizers()` — 处理 Finalizer 队列
4. `PrepareCycleRef()` — 准备循环引用检测
5. `PostResolveCycleTask()` — 循环引用解决后处理
6. `CollectLargeGarbage()` — 回收大对象

### 5.2 当前实现中的屏障缓冲收尾

当前代码里，没有把 PostMarkTermination 的 Task 2 落成一个独立的 `ClearWriteBarrierBuffers()` 步骤。真实行为是：

1. Step2.3 的 STW 内已经执行 `FlushAllWriteBarrierBuffers(mainQueue)`；
2. 同一轮 STW 会继续 drain 主队列，mutator 恢复运行前，残留 WBBuf 已经收敛完毕；
3. PostMarkTermination 不再承担 SATB retired-node 释放职责。

因此，如果保留 Task 2，它的正确语义应当是：**无需额外 SATB 清理；WBBuf 已在 Step2.3 完成 flush/clear。**

### 5.3 其他 5 个 Task 不受影响

| Task | 依赖 SATB？ | 混合写屏障下 |
|------|-----------|-------------|
| 1. ClearWeakRefBuffer | 否（依赖 mark bitmap） | **不变** |
| 3. ProcessFinalizers | 否（依赖 mark bitmap） | **不变** |
| 4. PrepareCycleRef | 否 | **不变** |
| 5. PostResolveCycleTask | 否 | **不变** |
| 6. CollectLargeGarbage | 否（依赖 mark bitmap） | **不变** |

混合写屏障只影响灰对象的产生和收集方式，不影响标记完成后的清理逻辑。

---

## 6. Phase 状态机更新

### 6.1 Phase 编号不变

Step2 §8 定义的 Phase 枚举值在混合写屏障下**完全不变**：

```cpp
enum GCPhase {
    MS_PHASE_IDLE              = 20,
    MS_PHASE_MARK_SETUP        = 21,   // Handshake
    MS_PHASE_MARK              = 22,   // 并发标记
    MS_PHASE_MARK_TERMINATION  = 23,   // STW
    MS_PHASE_POST_MARK         = 24,   // 非 STW
};
```

### 6.2 各 Phase 对应 Barrier 的变化

| Phase | Step2 (SATB) Barrier | Step3 (混合) Barrier |
|-------|---------------------|---------------------|
| `MS_PHASE_IDLE` | `IdleBarrier` | `MSIdleBarrier` |
| `MS_PHASE_MARK_SETUP` | `MSTraceBarrier`（SATB） | **`MSMarkBarrier`（混合）** |
| `MS_PHASE_MARK` | `MSTraceBarrier`（SATB） | **`MSMarkBarrier`（混合）** |
| `MS_PHASE_MARK_TERMINATION` | `MSTraceBarrier`（SATB） | **`MSMarkBarrier`（混合）** |
| `MS_PHASE_POST_MARK` | `IdleBarrier` | `MSIdleBarrier` |

### 6.3 Phase 转换流程对比

```
Step2 (SATB):
  IDLE → [Handshake: 安装 MSTraceBarrier] → MARK_SETUP
       → [并发标记] → MARK
       → [STW: CollectSatbCandidatesToMain + drain] → MARK_TERMINATION
       → [非 STW: ClearSatbBuffer + 弱引用清理] → POST_MARK
       → IDLE

Step3 (混合):
  IDLE → [Handshake: 安装 MSMarkBarrier] → MARK_SETUP
       → [并发标记 + WBBuf 实时 flush] → MARK
    → [STW: FlushWBBuf + FlushAssist + DrainGlobalQueue + drain] → MARK_TERMINATION
    → [非 STW: 弱引用清理 / finalizer / cycle-ref] → POST_MARK
       → IDLE
```

### 6.4 InstallBarrier 映射表更新

```cpp
void HeapImpl::InstallBarrierMS(const GCPhase phase) {
    switch (phase) {
    case GCPhase::MS_PHASE_MARK_SETUP:
    case GCPhase::MS_PHASE_MARK:
    case GCPhase::MS_PHASE_MARK_TERMINATION:
        currentBarrier = &msWriteBarrier_;    // 混合写屏障
        break;
    case GCPhase::MS_PHASE_IDLE:
    case GCPhase::MS_PHASE_POST_MARK:
    default:
        currentBarrier = &msIdleBarrier_;     // 空闲屏障
        break;
    }
}
```
