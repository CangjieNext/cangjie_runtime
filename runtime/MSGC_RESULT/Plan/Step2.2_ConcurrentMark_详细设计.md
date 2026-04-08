# Step 2.2：Concurrent Mark（并发标记）详细设计

> 本文档从 [Step 2：三色标记与 Sweep 详细设计](Step2_三色标记与Sweep详细设计.md) §5 拆分而来。
> 前置：[Step 2.1：Mark Setup 详细设计](Step2.1_MarkSetup_STW详细设计.md)
> 后续：[Step 2.3：Mark Termination（STW）详细设计](Step2.3_MarkTermination_STW详细设计.md)

---

## 目录

- [Step 2.2：Concurrent Mark（并发标记）详细设计](#step-22concurrent-mark并发标记详细设计)
  - [目录](#目录)
  - [1. 目标](#1-目标)
  - [2. 伪代码](#2-伪代码)
  - [3. DoConcurrentMarking 详细流程](#3-doconcurrentmarking-详细流程)
    - [3.1 Step 0：初始化状态](#31-step-0初始化状态)
    - [3.2 Step 1：`RunCollectorDrainSlice`（主线程推进）](#32-step-1runcollectordrainslice主线程推进)
    - [3.3 Step 2：`ControlAssistAndBackpressure`（调参与背压）](#33-step-2controlassistandbackpressure调参与背压)
      - [3.3.1 全局队列在哪里维护](#331-全局队列在哪里维护)
    - [3.4 Step 3：`MarkConverged`（收敛判定）](#34-step-3markconverged收敛判定)
      - [3.4.1 并发期间对象还在增长时，是否还能收敛](#341-并发期间对象还在增长时是否还能收敛)
    - [3.5 Step 4：`SleepOrYieldShort`（让出/短睡眠）](#35-step-4sleeporyieldshort让出短睡眠)
    - [3.6 Step 5：返回 `ConcurrentMark` 下一步（进入 MarkTermination）](#36-step-5返回-concurrentmark-下一步进入-marktermination)
    - [3.7 运行时状态（共享支撑）](#37-运行时状态共享支撑)
    - [3.8 TraceRefField 改造](#38-tracereffield-改造)
    - [3.9 新分配对象的处理](#39-新分配对象的处理)
    - [3.10 并发标记的终止条件（与 3.4 合并口径）](#310-并发标记的终止条件与-34-合并口径)
  - [4. 标记期间的辅助标记（Mark Assist）](#4-标记期间的辅助标记mark-assist)
    - [4.1 设计目标](#41-设计目标)
    - [4.2 核心思路：分配债务（assist debt）](#42-核心思路分配债务assist-debt)
    - [4.3 触发点与快速路径](#43-触发点与快速路径)
    - [4.4 MSMarkAssist 伪代码](#44-msmarkassist-伪代码)
    - [4.5 并发安全与边界条件](#45-并发安全与边界条件)
    - [4.6 参数建议（Step 2 默认值）](#46-参数建议step-2-默认值)
    - [4.7 运行热路径补充（RunAssistSlice，与 4.4 合并口径）](#47-运行热路径补充runassistslice与-44-合并口径)
  - [5. 并发标记期间的 Region 获取策略（sweepGen + gcEpoch）](#5-并发标记期间的-region-获取策略sweepgen--gcepoch)
    - [5.1 sweepGen / gcEpoch 概念](#51-sweepgen--gcepoch-概念)
    - [5.2 标记期间的 Region 获取来源](#52-标记期间的-region-获取来源)
      - [5.2.1 sweepGen 与四队列的兼容规则](#521-sweepgen-与四队列的兼容规则)
      - [5.2.2 gcEpoch 与四队列的兼容规则](#522-gcepoch-与四队列的兼容规则)
    - [5.3 与 Go sweepgen 的对比](#53-与-go-sweepgen-的对比)
    - [5.4 sweepGen / gcEpoch 在 MarkSetup 中的更新](#54-sweepgen--gcepoch-在-marksetup-中的更新)
  - [6. 并发标记框架复用分析与性能对比（assist 主导版）](#6-并发标记框架复用分析与性能对比assist-主导版)
    - [6.1 框架层 — 部分复用（assist 主导）](#61-框架层--部分复用assist-主导)
    - [6.2 算法层 — 需 Override 的 virtual 方法](#62-算法层--需-override-的-virtual-方法)
    - [6.3 标记热路径性能对比](#63-标记热路径性能对比)
    - [6.4 定量估算](#64-定量估算)
    - [6.5 Trade-off 总结](#65-trade-off-总结)
  - [7. WCollector::TraceHeap 复用分析](#7-wcollectortraceheap-复用分析)
    - [7.1 现有代码（WCollector.cpp L370-393）](#71-现有代码wcollectorcpp-l370-393)
    - [7.2 逐行复用判定](#72-逐行复用判定)
    - [7.3 结论：去除 "fix ref" 即可](#73-结论去除-fix-ref-即可)
    - [7.4 MarkSweepCollector 新 TraceHeap 伪代码](#74-marksweepcollector-新-traceheap-伪代码)
    - [7.5 需 Override 的 virtual 方法简化版实现](#75-需-override-的-virtual-方法简化版实现)

---

## 1. 目标

与 Mutator 并发执行，追踪所有可达对象并标记为黑色。这是 GC 周期中耗时最长的阶段。

## 2. 伪代码

```cpp
void MarkSweepCollector::ConcurrentMark() {
    // Phase 已在 MarkSetup 末尾切换到并发标记阶段
    TransitionToGCPhase(GCPhase::GC_PHASE_MS_CONCURRENT_MARK, true);
    
    // 1. 并发标记（assist 主导）
    //    Mutator 在分配路径/ safepoint 按债务执行 MSMarkAssist，
    //    从每P本地 work buffer + 全局 work list 消耗灰对象
    DoConcurrentMarking();
    
    // 2. DoConcurrentMarking 只负责把灰对象队列收敛到稳定空
    //    SATB 尾巴处理不在这里做，统一放在下一步 MarkTermination
    EnterMarkTermination();
}
```

## 3. DoConcurrentMarking 详细流程

```cpp
void MarkSweepCollector::DoConcurrentMarking() {
    ResetConcurrentMarkRuntimeState();

    // assist 主路径：Mutator 分片偿还（见 §4）
    // GC 主线程同时持续参与主队列 drain，负责“控速 + 主动推进”
    while (true) {
        size_t processedInLastSlice = RunCollectorDrainSlice();
        ControlAssistAndBackpressure();

        MarkRuntimeStats s = CollectMarkRuntimeStats();
        if (MarkConverged()) {
            break;
        }
        SleepOrYieldShort(s, processedInLastSlice);
    }
}
```

### 3.1 Step 0：初始化状态

进入并发标记后，先执行：

```cpp
ResetConcurrentMarkRuntimeState();
```

作用：清零收敛轮次计数、重置窗口统计、恢复默认背压参数，避免沿用上轮 GC 残留状态。

### 3.2 Step 1：`RunCollectorDrainSlice`（主线程推进）

主线程每轮先推进一段标记工作：

```cpp
size_t processedInLastSlice = RunCollectorDrainSlice();
```

不是“只标记一个队列元素”，而是“**在一次受限预算内连续处理多个灰对象**”。

- 数据来源：优先从**全局主队列**取对象（避免直接干扰各 P 本地队列）。
- 工作内容：对取出的每个对象执行 `MarkObject`，并按普通对象 / `WeakRef` 分支处理（与 `ConcurrentMarkingWork::Execute` 对齐）。
- 产出流向：新发现灰对象先进入主线程本地临时栈，达到阈值后批量回吐全局主队列。
- 退出条件：达到对象数预算或时间预算，或主队列暂时为空。

```cpp
size_t MarkSweepCollector::RunCollectorDrainSlice()
{
    const uint64_t deadline = TimeUtil::NanoSeconds() + collectorSliceMaxNs_;
    size_t processed = 0;

    WorkStack local;
    while (processed < collectorSliceMaxObjects_ && TimeUtil::NanoSeconds() < deadline) {
        BaseObject* obj = PopGlobalWork();
        if (obj == nullptr) {
            break;
        }
        bool wasMarked = MarkObject(obj);
        if (!wasMarked) {
            if (!obj->HasRefField()) {
                ++processed;
                continue;
            }
            if (UNLIKELY(obj->IsWeakRef())) {
                RefField<>* referentField = reinterpret_cast<RefField<>*>((uintptr_t)obj + TYPEINFO_PTR_SIZE);
                BaseObject* referent = GetAndTryTagObj(obj, *referentField);
                if (referent != nullptr) {
                    TraceObjectRefFields(referent, local);
                    WeakRefBuffer::Instance().Insert(obj); // 记录 live weakref，MarkTermination 统一处理
                }
            } else {
                TraceObjectRefFields(obj, local);
            }
        }
        ++processed;

        if (local.size() >= collectorLocalFlushThreshold_) {
            PushBatchToGlobal(local);
            local.clear();
        }
    }
    if (!local.empty()) {
        PushBatchToGlobal(local);
    }
    return processed;
}
```

### 3.3 Step 2：`ControlAssistAndBackpressure`（调参与背压）

该步骤不做重标记热路径，职责是“**观测 → 调参 → 兜底决策**”。

#### 3.3.1 全局队列在哪里维护

全局灰对象队列由 **`MarkSweepCollector`（collector 侧）统一维护**，不归属于单个 `Processor`。

- **所有权**：`MarkSweepCollector` 持有 `globalWorkList_`（或等价结构）及其并发保护（锁/无锁队列）。
- **写入方**：
  - GC 主线程在 `RunCollectorDrainSlice()` 中把本地临时栈 `PushBatchToGlobal(...)` 回吐；
  - Mutator assist 在本地队列高水位时 `DrainLocalToGlobal(...)` 回吐。
- **读取方**：
  - GC 主线程通过 `PopGlobalWork()` 消费；
  - Mutator assist 通过 `RefillLocalFromGlobal(...)` 批量领取。
- **统一入口函数**：`PopGlobalWork / PushBatchToGlobal / RefillLocalFromGlobal / PublishLocalOverflowToGlobal`。

也就是说，**全局队列在 collector 内维护，P 侧只维护本地分队列**；两者通过上述 API 交互。

```cpp
void MarkSweepCollector::ControlAssistAndBackpressure()
{
    MarkRuntimeStats s = CollectMarkRuntimeStats();

    // 1) 维护可见性：避免本地囤积（必要时把高水位本地队列回吐全局）
    PublishLocalOverflowToGlobal();

    // 2) 根据 backlog 调 assistRatio（带平滑与上下限）
    //    目标：分配越快/积压越高，assist 越强
    double base = static_cast<double>(s.backlogBytes) /
                  static_cast<double>(std::max<size_t>(s.allocBytesWindow, 1));
    double targetRatio = Clamp(base, minAssistRatio_, maxAssistRatio_);
    double newRatio = assistRatio_ * (1.0 - assistRatioEwmaAlpha_) +
                      targetRatio * assistRatioEwmaAlpha_;
    assistRatio_.store(newRatio, std::memory_order_relaxed);

    // 3) 动态调触发阈值：积压高时提前触发，积压低时放宽
    if (s.backlogItems >= backlogHighWatermarkItems_) {
        assistTriggerBytes_ = assistTriggerBytesHighPressure_;
    } else if (s.backlogItems <= backlogLowWatermarkItems_) {
        assistTriggerBytes_ = assistTriggerBytesNormal_;
    }

    // 4) Step2 约束：不启用任何兜底 worker/pool。
    //    并发标记仅由 Mutator assist + GC 主线程推进。
}
```

### 3.4 Step 3：`MarkConverged`（收敛判定）

收敛判定采用“**静态条件 + 连续稳定轮次**”，避免瞬时抖动误判结束。

```cpp
bool MarkSweepCollector::MarkConverged()
{
    MarkRuntimeStats s = CollectMarkRuntimeStats();

    const bool noGrayWork = (s.globalWorkItems == 0 && s.localWorkItems == 0);

    if (noGrayWork) {
        ++convergedStableRounds_;
    } else {
        convergedStableRounds_ = 0;
    }

    return convergedStableRounds_ >= convergeStableRoundsRequired_;
}
```

建议默认值（Step 2）：

- `convergeStableRoundsRequired_ = 3`

含义：至少连续 3 轮都满足“主队列+分队列清空”才退出并发标记。

> SATB 记录不是并发标记队列的一部分，不作为并发阶段退出硬门槛；它在 MarkTermination 统一 re-mark 收敛。

#### 3.4.1 并发期间对象还在增长时，是否还能收敛

可以收敛，但前提不是“只靠 SATB”，而是以下机制共同成立：

1. **新分配对象直接置黑**（见 §3.9），避免“新对象白色漏标”。
2. **写屏障（`MSTraceBarrier`）持续记录并发引用变更**，避免并发修改导致可达对象漏追踪。
3. **assist + 主线程 drain** 保证平均标记处理速率不长期低于产生速率。

因此 `MarkConverged` 用“主/分队列清空并稳定 N 轮”作为退出判定是成立的：只要灰对象生产速率最终被压下，队列会收敛到空。

边界场景（分配/写入长期高压）下，可能出现“长时间不收敛”，此时不应无限等待，建议触发降级策略：

- 提高 `assistRatio` 与触发频率（更强背压）；
- 提高 assist 强度并增大主线程单轮 drain 预算；
- 超过并发标记时间上限后进入强制 MarkTermination（短 STW 收敛尾巴）。

> 说明：你说的 “STAB” 这里应为 `SATB`。`SATB` 负责并发修改可见性，但它不是单独的“收敛控制器”；收敛还依赖背压与调度。

### 3.5 Step 4：`SleepOrYieldShort`（让出/短睡眠）

`SleepOrYieldShort()` 的作用是：当本轮主线程推进后没有立即需要继续重活时，**短暂让出 CPU 或短睡眠**，避免并发标记控制循环空转占满核心。

它是**需要的**，但建议做成“条件触发”，而不是每轮固定睡眠：

- **需要让出**：本轮 `RunCollectorDrainSlice()` 处理对象很少/为 0，且队列接近空。
- **不应让出**：backlog 高或 assist 债务高时，应继续下一轮推进。

```cpp
void MarkSweepCollector::SleepOrYieldShort(const MarkRuntimeStats& s,
                                           size_t processedInLastSlice)
{
    const bool idleLike = (processedInLastSlice == 0) && (s.backlogItems == 0);
    const bool highPressure = (s.backlogItems >= backlogHighWatermarkItems_) ||
                              (s.totalAssistDebtBytes >= assistHighDebtBytes_);

    if (highPressure) {
        return; // 高压期不 sleep，保持追赶
    }

    if (idleLike) {
        NanoSleep(idleSleepNs_); // 例如 50~200us，防空转
    } else {
        ThreadYield(); // 轻量让出时间片
    }
}
```

结论：`SleepOrYieldShort` 不是“功能正确性必须项”，但在工程上是并发阶段稳定性与 CPU 友好性的必要项。

### 3.6 Step 5：返回 `ConcurrentMark` 下一步（进入 MarkTermination）

`DoConcurrentMarking` 跳出循环后直接返回上层；并发阶段到此结束，不在本函数内做 SATB 尾巴处理：

```cpp
// DoConcurrentMarking 返回后
EnterMarkTermination();
// 在 MarkTermination 内部执行 SATB re-mark：
// MarkSatbBuffer(workStack_)
```

作用：职责分层更清晰——`DoConcurrentMarking` 只负责并发收敛，`MarkTermination` 负责最终一致性收尾。

Step 2.3 的默认实现口径：

1. MarkTermination 采用 **STW-first** 收敛；
2. SATB 尾巴采用 `MarkSatbBuffer` 的核心逻辑复用，但通过 MS 专用封装执行；
3. 并发阶段不再尝试在本函数内“提前清 SATB”。

### 3.7 运行时状态（共享支撑）

`RunCollectorDrainSlice / ControlAssistAndBackpressure / MarkConverged / SleepOrYieldShort` 共用同一组轻量统计：

```cpp
struct MarkRuntimeStats {
    size_t globalWorkItems;          // 全局 work list 长度
    size_t localWorkItems;           // 所有 P 本地 work 合计长度
    size_t totalAssistDebtBytes;     // 所有 P 债务总和
    size_t backlogItems;             // global + local
    size_t backlogBytes;             // backlog 的字节估算（按待处理灰对象）
    size_t allocBytesWindow;         // 最近窗口分配字节
    size_t markedBytesWindow;        // 最近窗口标记字节
};
```

> 这些值不要求强一致，允许近似；目标是“控制正确方向”，不是做精确会计。

建议默认值（Step 2）：

- `collectorSliceMaxObjects_ = 256`
- `collectorSliceMaxNs_ = 200us`
- `collectorLocalFlushThreshold_ = 64`

实现要点：

1. 主线程主要做标记推进（`RunCollectorDrainSlice`），`ControlAssistAndBackpressure` 只做控制面逻辑。
2. `assistRatio` 更新必须 `Clamp + EWMA`，避免抖动导致分配尾延迟震荡。
3. 不引入兜底 worker 线程；并发阶段仅允许 Mutator assist 与 GC 主线程参与。
4. 在高压场景优先提升 assist 比例与主线程 drain 预算，而不是扩展执行主体。

### 3.8 TraceRefField 改造

```cpp
// [改造] 简化版的 TraceRefField — 去除 tag/forward 相关逻辑
void MarkSweepCollector::TraceRefField(BaseObject* obj, RefField<>& field, WorkStack& workStack) const {
    BaseObject* target = field.GetTargetObject();
    if (target == nullptr) return;
    
    // [去除] 不需要 IsTaggedPointer / FindLatestVersion / UpdateField
    //        Mark-Sweep 下对象不会移动，引用永远有效
    
    // 检查是否在堆范围内
    if (!Heap::IsHeapAddress(target)) return;
    
    // 标记对象（CAS 设置 bitmap 位）
    bool newlyMarked = MarkObject(target);
    if (newlyMarked) {
        workStack.push_back(target);  // 新发现的白色对象 → 入灰色队列
    }
}
```

**与现有 `WCollector::TraceRefField()` 的对比**：

| 现有逻辑（WCollector.cpp L196-231） | Mark-Sweep 版本 |
|-------------------------------------|-----------------|
| `IsCurrentPointer(ref)` → 当前指针处理 | **去除**：无指针 tagging |
| `IsOldPointer(ref)` → `FindLatestVersion(ref)` | **去除**：对象不移动 |
| `GetAndTryTagRefField` + `CompareExchange` | **去除**：无需 tag/CAS 更新引用 |
| `IsHeapAddress` → `MarkObject(ref)` + `push_back` | **保留**：核心标记逻辑完全相同 |

### 3.9 新分配对象的处理

并发标记期间，Mutator 继续分配新对象。这些对象必须标记为**黑色**（已标记），否则可能被误回收：

```cpp
// 复用现有 WCollector::MarkNewObject() 机制
// 在分配路径中（Allocator::Allocate）：
void MarkSweepCollector::MarkNewObject(BaseObject* obj, size_t size) {
    GCPhase phase = Mutator::GetMutator()->GetMutatorPhase();
    if (phase == GC_PHASE_MS_MARK_SETUP ||
        phase == GC_PHASE_MS_CONCURRENT_MARK ||
        phase == GC_PHASE_MS_MARK_TERMINATION) {
        // 新分配对象直接标记为黑色（已标记但不入灰色队列）
        RegionInfo* region = RegionInfo::GetRegionInfoAt(obj);
        region->MarkObject(obj, size);
        // 注意：不 push 到 workStack_（黑色 = 已处理）
    }
}
```

### 3.10 并发标记的终止条件（与 3.4 合并口径）

并发标记阶段的退出口径与 §3.4 `MarkConverged` 保持一致，终止条件为：
1. 全局主队列为空（无待处理灰对象）
2. 所有 P 的本地分队列为空（无待处理灰对象）
3. 为避免抖动误判，以上条件需满足连续 `N` 轮（建议 `N=3`）
4. **注意**：SATB 记录不在上述队列中，统一在 MarkTermination 中处理

## 4. 标记期间的辅助标记（Mark Assist）

### 4.1 设计目标

在 `GC_PHASE_MS_CONCURRENT_MARK` 期间，当分配速度明显快于标记速度时，引入 Mutator 辅助标记，避免：

1. 灰对象队列长时间堆积导致 MarkTermination 压力上升
2. GC 线程独自追赶失败，导致标记尾部长尾
3. 高分配线程“只分配不偿还”造成全局不公平

目标是不改变并发标记正确性，只控制吞吐与尾延迟。

### 4.2 核心思路：分配债务（assist debt）

采用 Go 风格组织：

- **每 P 本地 work buffer**（`p.gcw` 等价物）：优先消费，减少全局竞争
- **全局 work buffer 列表**：本地耗尽后再从全局领取/归还
- **按 P 记录债务**：`assistDebtBytes[P]`，而不是记在单个 Mutator 对象上

债务规则：

- 分配越多，`assistDebtBytes[P]` 越大
- assist 执行后按“完成的标记工作量”抵扣债务
- 单次 assist 只偿还一部分（分片执行），避免业务线程长时间停顿

全局维护一个动态系数 `assistRatio`：

```text
assistRatio = max(minAssistRatio,
                  markedBytesTarget / max(allocatedBytesSinceMarkStart, 1))
```

直观含义：每分配 1B 需要偿还多少“标记工作字节”。

> 实现上可先用固定比率（例如 0.25），再演进为动态比率。

### 4.3 触发点与快速路径

触发策略采用“双触发”：

1. **分配路径主触发**（与 Go 一致，背压最及时）
2. **safepoint 补触发**（Mutator 进入 safepoint 时继续偿还，平滑尾延迟）

分配路径示意：

```cpp
MAddress Allocator::Allocate(size_t size) {
    MAddress addr = FastOrSlowAlloc(size);
    if (UNLIKELY(addr == 0)) {
        return 0;
    }

    if (UNLIKELY(GetGCPhase() == GC_PHASE_MS_CONCURRENT_MARK)) {
        Processor* p = Processor::Current();
        size_t debt = static_cast<size_t>(size * assistRatio_.load(std::memory_order_relaxed));
        p->assistDebtBytes += debt;

        if (UNLIKELY(p->assistDebtBytes >= assistTriggerBytes_)) {
            MSMarkAssist(*p, AssistTrigger::ALLOC_PATH);
        }
    }
    return addr;
}
```

safepoint 补触发示意：

```cpp
void Mutator::OnSafepoint()
{
    if (GetGCPhase() == GC_PHASE_MS_CONCURRENT_MARK) {
        Processor* p = Processor::Current();
        if (p->assistDebtBytes >= assistSafepointTriggerBytes_) {
            collector->MSMarkAssist(*p, AssistTrigger::SAFEPOINT_PATH);
        }
    }
}
```

### 4.4 MSMarkAssist 伪代码

```cpp
void MarkSweepCollector::MSMarkAssist(Processor& p, AssistTrigger trigger)
{
    if (GetGCPhase() != GC_PHASE_MS_CONCURRENT_MARK) {
        p.assistDebtBytes = 0;
        return;
    }

    // 单次辅助上限：避免一次 assist 卡住业务线程过久
    size_t remainDebt = std::min(p.assistDebtBytes, assistMaxWorkPerCallBytes_);
    uint64_t deadline = TimeUtil::NanoSeconds() + assistMaxNsPerCall_;

    WorkStack& local = p.localMarkWork; // 每P本地work buffer
    while (remainDebt > 0 && TimeUtil::NanoSeconds() < deadline) {
        BaseObject* obj = nullptr;

        // 1) 先消费本地 work
        if (!local.empty()) {
            obj = local.back();
            local.pop_back();
        }

        // 2) 本地空 -> 从全局 work list 批量领取
        if (obj == nullptr) {
            RefillLocalFromGlobal(p);
            if (!local.empty()) {
                obj = local.back();
                local.pop_back();
            }
        }

        // 3) 仍为空 -> 尝试 steal 其他 P 的本地 work
        if (obj == nullptr) {
            obj = StealFromOtherP(p);
        }

        if (obj == nullptr) {
            // 无活可做，允许提前退出（避免空转）
            break;
        }

        // 复用并发标记主逻辑
        bool wasMarked = MarkObject(obj);
        if (!wasMarked) {
            TraceObjectRefFields(obj, local);
        }

        // 按“标记对象大小”抵扣债务（近似即可）
        remainDebt -= std::min(remainDebt, obj->GetSize());
    }

    size_t repaid = std::min(p.assistDebtBytes, assistMaxWorkPerCallBytes_ - remainDebt);
    p.assistDebtBytes -= repaid;

    // 4) 本地 work 过大时回吐到全局，避免某个 P 长期囤积任务
    if (local.size() > localWorkHighWatermark_) {
        DrainLocalToGlobal(p, localWorkTarget_);
    }
}
```

### 4.5 并发安全与边界条件

1. **只在并发标记阶段生效**：非 `GC_PHASE_MS_CONCURRENT_MARK` 直接清债务并返回。
2. **不改变屏障语义**：assist 仅消费灰色队列，写屏障仍由 `MSTraceBarrier` 保证。
3. **分配主触发 + safepoint 补触发**：既保证及时背压，也避免只靠 safepoint 导致追赶滞后。
4. **预算+截止时间双限流**：防止单次 assist 造成业务线程长暂停（分片偿还）。
5. **本地优先、全局兜底、可 steal**：减少全局锁竞争并提升负载均衡。
6. **执行主体固定**：并发标记仅由 Mutator assist 与 GC 主线程执行，不引入额外 worker 线程。

> 设计约束：**不建议仅在 safepoint 触发 assist**。原因是分配突发时，Mutator 可能在两个 safepoint 之间持续高速分配，若无分配路径主触发，背压会明显滞后，导致灰对象队列积压并把压力推迟到 MarkTermination。

### 4.6 参数建议（Step 2 默认值）

Step 2 建议先启用“保守 assist”：

- `assistTriggerBytes_ = 64KB`
- `assistSafepointTriggerBytes_ = 32KB`
- `assistMaxWorkPerCallBytes_ = 256KB`
- `assistMaxNsPerCall_ = 200us`
- `minAssistRatio = 0.10`（可先固定）
- `localWorkHighWatermark_ = 256`（对象数）
- `localWorkTarget_ = 128`（对象数）

分阶段策略：

1. **MVP（先落地）**：固定 `assistRatio` + 单次预算，验证正确性
2. **优化期**：引入动态 `assistRatio`（按分配/标记速率调节）
3. **高级优化**：按 Mutator 分配速率做差异化债务（高分配线程偿还更多）

### 4.7 运行热路径补充（RunAssistSlice，与 4.4 合并口径）

`4.4` 给出的是 assist 的预算与调度框架，本节补充单对象处理热路径，并与 §3.2 `RunCollectorDrainSlice` 保持同构口径：

```cpp
// assist 分片执行（Mutator 侧）
void MarkSweepCollector::RunAssistSlice(Processor& p) {
    while (true) {
        BaseObject* obj = PopFromLocalGlobalOrSteal(p);  // 本地→全局→steal
        if (obj == nullptr) break;                       // 无灰色对象 → 结束

        bool wasMarked = collector_->MarkObject(obj);
        if (wasMarked) continue;  // 已标记，跳过

        if (!obj->HasRefField()) {
            continue;
        }

        if (obj->IsWeakRef()) {
            RefField<>* referentField = reinterpret_cast<RefField<>*>((uintptr_t)obj + TYPEINFO_PTR_SIZE);
            BaseObject* referent = collector_->GetAndTryTagObj(obj, *referentField);
            if (referent != nullptr) {
                TraceObjectRefFields(referent, p.localMarkWork);
                WeakRefBuffer::Instance().Insert(obj);
            }
            continue;
        }

        TraceObjectRefFields(obj, p.localMarkWork);

        if (p.localMarkWork.size() > localWorkHighWatermark_) {
            DrainLocalToGlobal(p, localWorkTarget_);
        }
    }
}
```

## 5. 并发标记期间的 Region 获取策略（sweepGen + gcEpoch）

### 5.1 sweepGen / gcEpoch 概念

并发标记期间 Region 安全使用的核心是 `sweepGen`，`gcEpoch` 作为跨轮次分配纪元信息补充：

```cpp
// sweep 代际：每轮 GC 递增 2
std::atomic<uint64_t> globalSweepGen_;

// 全局 GC 纪元：记录 Region 分配所属 GC 轮次
std::atomic<uint64_t> globalGCEpoch_;

// 每个 Region 记录自己的 GC 纪元
struct RegionInfo {
    uint64_t sweepGen; // 该 Region 的 sweep 代际
    uint64_t gcEpoch;  // 该 Region 分配所属 epoch
    // ...
};
```

**判定含义（以 sweepGen 为主）**：

| Region.sweepGen 值 | 含义 | 可否在标记期间获取 |
|---------------------|------|-------------------|
| `== globalSweepGen_ - 2` | 上一轮残留，**未 sweep** | ✅ 可获取，但必须先 CAS 抢占并 sweep |
| `== globalSweepGen_ - 1` | 正在被其他线程 sweep | ⚠️ 跳过，不自旋等待 |
| `== globalSweepGen_` | 已 sweep 或本轮新分配 | ✅ 可直接使用 |

### 5.2 标记期间的 Region 获取来源

以 [Step1.2](Step1.2_SizeClass与线程本地分配设计.md) 的 `CentralRegionPool` 四队列为准，Mutator 获取顺序应为：

```
1) partialSwept[cls]（首选）
    条件：队列中 Region 具备空闲 slot，且 sweepGen == globalSweepGen_
    操作：直接返回给 TL，零 sweep 延迟

2) partialUnswept[cls]
    条件：有空闲潜力，但 sweepGen == globalSweepGen_-2
    操作：CAS(sg-2 -> sg-1) 抢占 sweep 权
          成功 -> 原地 sweep -> 设为 sg
                     有空洞 -> 直接返回
                     全死   -> ReleaseRegion() 后继续取下一个
                     仍全满 -> 放入 fullSwept
          失败 -> 说明他线程在 sweep，跳过

3) fullUnswept[cls]
    条件：历史上“满”，但 sweep 后可能释放出空洞
    操作：同 2) 先 CAS 抢占再 sweep
          sweep 后若出现空洞则可返回，否则进 fullSwept 或释放

4) GetFreshRegion(cls)
    条件：上述三步都无可用 Region
    操作：从 FreeRegionManager / InactiveZone 获取新 Region
          初始化 sweepGen = globalSweepGen_, gcEpoch = globalGCEpoch_
```

`fullSwept[cls]` 不是分配直接来源（它语义上“当前无空洞”），仅作为状态池等待后续代际翻转或再次 sweep 后转移。

#### 5.2.1 sweepGen 与四队列的兼容规则

四队列表达的是“空闲形态”（partial/full），`sweepGen` 才是“是否可直接分配”的真值来源。两者组合规则：

| 队列 | 典型 sweepGen | 分配时动作 |
|------|---------------|-----------|
| `partialSwept` | `== globalSweepGen_` | 直接分配 |
| `partialUnswept` | `== globalSweepGen_-2` | CAS 抢占 + 原地 sweep 后再判定 |
| `fullUnswept` | `== globalSweepGen_-2` | CAS 抢占 + 原地 sweep，可能转 partial 可分配 |
| `fullSwept` | `== globalSweepGen_` | 当前不分配，作为状态池 |

实现要点：**永远不要仅凭“所在队列名”判断可分配性，取出后必须再检查 `sweepGen` 并执行 CAS。**

> 实现备注：如果采用 Step1.2 的“队列身份 O(1) 翻转”（逻辑上 swept/unswept 角色切换），或采用“不翻转队列、仅靠 `sweepGen` 二次校验”两种实现，以上判定都成立；对外语义统一由 `sweepGen` 保证。

#### 5.2.2 gcEpoch 与四队列的兼容规则

`gcEpoch` 与四队列正交，不参与“是否需要 sweep”的快速判定：

- `sweepGen`：决定该 Region 是否要 sweep（分配路径热判定）
- `gcEpoch`：记录 Region 分配所属 GC 轮次（统计/调试/策略辅助）

建议规则：

1. `GetFreshRegion` 时写入 `gcEpoch = globalGCEpoch_`。
2. Region 在四队列之间流转（partial/full、swept/unswept）时，不修改 `gcEpoch`。
3. 任何“能否直接分配/是否先 sweep”决策只看 `sweepGen`，不看 `gcEpoch`。
4. 需要做跨轮次分析（如浮动垃圾、分配热点）时再使用 `gcEpoch`。

### 5.3 与 Go sweepgen 的对比

| 维度 | Go sweepgen | Cangjie sweepGen |
|------|-------------|-----------------|
| 递增方式 | `mheap_.sweepgen += 2` | `globalSweepGen_ += 2` |
| 存储位置 | `mspan.sweepgen` | `RegionInfo.sweepGen` |
| 判断逻辑 | `sweepgen-2`=需 sweep, `sweepgen-1`=正在 sweep, `sweepgen`=已 sweep | 相同 |
| lazy sweep | `sweepone()` in `mallocgc()` | 第一版急切 sweep，后续可改为 lazy |

### 5.4 sweepGen / gcEpoch 在 MarkSetup 中的更新

GC 周期开始时，在 MarkSetup Handshake 完成后先递增 `globalSweepGen_`，再递增 `globalGCEpoch_`（见 Step2.1 §2 伪代码）。此后旧 Region 通过 `sweepGen` 判定是否需要 sweep，新分配 Region 记录当前 `gcEpoch`。

## 6. 并发标记框架复用分析与性能对比（assist 主导版）

### 6.1 框架层 — 部分复用（assist 主导）

| 组件 | 所在文件 | assist 主导版使用方式 | 是否保留 |
|------|---------|-----------------------|----------|
| **每P本地 work buffer** | 新增（Processor 侧） | Mutator assist 优先消费 | ✅ 主路径 |
| **全局 work list** | 新增（collector 侧） | 本地耗尽后的领取/回吐 | ✅ 主路径 |
| **MarkSatbBuffer** | TracingCollector.cpp L441-497 | MarkTermination 阶段收尾 re-mark（MS 专用封装复用核心逻辑） | ✅ 核心复用 |
| **DoTracing / TracingImpl** | TracingCollector.cpp | 不作为 MS 并发标记执行路径 | ❌ 不复用 |
| **ConcurrentMarkingWork** | TracingCollector.cpp L62-145 | 仅参考其单对象处理分支语义 | ⚠️ 语义参考 |
| **TryForkTask** | ConcurrentMarkingWork 内 | worker 任务分裂机制，MS 路径禁用 | ❌ 不使用 |

**关键发现**：assist 主路径与 `ConcurrentMarkingWork` 在“单对象处理逻辑”上是同构的，仍复用相同 virtual 差异注入点（对应 §3.2 `RunCollectorDrainSlice` 的对象处理分支）：

```cpp
// assist slice / worker loop 核心处理（逻辑同构）
for (;;) {
    BaseObject* obj = PopFromLocalGlobalOrSteal(...);
    if (obj == nullptr) break;
    bool wasMarked = collector.MarkObject(obj);      // ← virtual
    if (!wasMarked) {
        if (!obj->HasRefField()) continue;
        if (UNLIKELY(obj->IsWeakRef())) {
            auto* referent = collector.GetAndTryTagObj(obj, *referentField);  // ← virtual
            if (referent != nullptr) {
                collector.TraceObjectRefFields(referent, workStack);          // ← virtual
                WeakRefBuffer::Instance().Insert(obj);
            }
        } else {
            collector.TraceObjectRefFields(obj, workStack);  // ← virtual
        }
    }
    // assist 主路径：按预算退出 + 本地回吐全局
}
```

因此 **并发标记执行主体固定为“Mutator assist + GC 主线程”**；保持 virtual override 不变即可完成语义迁移。

### 6.2 算法层 — 需 Override 的 virtual 方法

| virtual 方法 | WCollector 实现 | MarkSweepCollector 实现 |
|-------------|----------------|------------------------|
| `TraceRefField` | 6 步骤：IsCurrentPointer → IsOldPointer → FindLatestVersion → GetAndTryTagRefField → CAS → push（~40 行） | 4 步骤：GetTargetObject → IsHeapAddress → MarkObject → push（~6 行） |
| `TraceObjectRefFields` | 包装 TraceRefField | 包装简化版 TraceRefField（不变） |
| `GetAndTryTagObj` | tag 检查 + FindLatest + CAS（~25 行） | 直接 GetTargetObject + IsHeapAddress（~5 行） |
| `MarkObject` | `region->MarkObject(obj, size)` + AddLiveByteCount | 基类版本功能等价，可直接复用 |
| `DoResurrection` | GetAndTryTagRefField + tag/CAS | 需 Override：去除 tag/CAS |

### 6.3 标记热路径性能对比

以 `TraceRefField`（**每个引用字段调用一次**）为分析单元：

| 步骤 | WCollector (Copy-GC) | MarkSweepCollector | 开销差异 |
|------|---------------------|--------------------|----------|
| 读取引用字段 | 1 次 load | 1 次 load | 相同 |
| Tag 检查 ×2 | `IsCurrentPointer` + `IsOldPointer`（2 次比较+分支） | — | **省去 2 次分支** |
| 查找最新版本 | `FindLatestVersion`（查 ForwardTable，可能 **cache miss**） | — | **省去 1 次间接查表** |
| Tag + CAS | `GetAndTryTagRefField` → `CompareExchange`（原子操作） | — | **省去 1 次原子 CAS** |
| 堆地址检查 | `IsHeapAddress` | `IsHeapAddress` | 相同 |
| 标记检查/入栈 | `IsMarkedObject` → push | `MarkObject` → push | 相同 |

**每个引用字段的额外开销**：WCollector 比 MarkSweep 多出 2 次 tag 比较 + 1 次可能的 ForwardTable 查找 + 1 次原子 CAS。

### 6.4 定量估算

假设典型对象有 4 个引用字段，标记 100 万个对象：

```
WCollector:  100万 × 4字段 × (2 tag check + 1 FindLatest + 1 CAS) = 400万次额外操作
MarkSweep:   100万 × 4字段 × (1 load + 1 check) = 基础操作

CAS 在现代 CPU 上约 10-20ns，400万次 CAS ≈ 40-80ms 额外开销
Cache miss (FindLatest) 约 50-100ns，假设 10% 命中 ≈ 40万次 × 75ns ≈ 30ms

预估标记阶段性能提升：10~30%（取决于对象图结构和旧指针比例）
```

### 6.5 Trade-off 总结

| 维度 | CopyGC (WCollector) | MarkSweep |
|------|--------------------|-----------|
| **标记热路径效率** | 慢（额外 tag/CAS/ForwardTable） | **快**（纯标记，零 CAS） |
| **Sweep/回收阶段** | **快**（复制后 from-space 整体回收，无 Sweep） | 慢（需逐 Region 遍历 Sweep） |
| **STW 暂停** | 较长（Enum + Trace + Forward 均需 STW 配合） | **更短**（仅 MarkSetup + MarkTermination 短暂 STW） |
| **碎片** | **无**（复制式自动紧缩） | 有（原地回收，需 FreeList 管理） |
| **总 CPU 开销** | 复制大对象成本高 | assist 分摊标记开销 + GC 主线程持续推进 |
| **适用场景** | 短生命周期对象多、堆较小 | **大堆、长生命周期对象多** |

## 7. WCollector::TraceHeap 复用分析

### 7.1 现有代码（WCollector.cpp L370-393）

```cpp
void WCollector::TraceHeap()
{
    WorkStack workStack = NewWorkStack();
    WorkStack foreignStack = NewWorkStack();
    // assemble garbage candidates for tracing.
    reinterpret_cast<RegionSpace&>(theAllocator).AssembleGarbageCandidates();

    {
        MRT_PHASE_TIMER("enum roots & update old pointers within");
        TransitionToGCPhase(GCPhase::GC_PHASE_ENUM, true);
        DoEnumeration(workStack, foreignStack);
    }

    {
        MRT_PHASE_TIMER("trace live objects & update old pointers in ref-fields");
        markedObjectCount.store(0, std::memory_order_relaxed);
        TransitionToGCPhase(GCPhase::GC_PHASE_TRACE, true);
        reinterpret_cast<RegionSpace&>(theAllocator).PrepareTrace();
        DoTracing(workStack, foreignStack);

        ProcessFinalizers();
    }
}
```

### 7.2 逐行复用判定

| 行 / 调用 | 作用 | 复用判定 | MS 改造 |
|-----------|------|---------|---------|
| `NewWorkStack()` | 创建工作栈 | ✅ 直接复用 | — |
| `AssembleGarbageCandidates()` | 标记 from-space Region，为 CopyGC 选择回收集合 | ❌ **去除** | MS 不需要 from-space 选择 |
| `TransitionToGCPhase(ENUM, true)` | 切换到枚举阶段（Handshake） | ⚠️ 改造 | 合并到 MarkSetup（Step2.1 已完成） |
| `DoEnumeration(workStack, foreignStack)` | 枚举根对象并入栈 | ⚠️ 改造 | 移至 MarkSetup 中（Step2.1 §3 EnumAndMarkRoots） |
| `markedObjectCount.store(0)` | 标记计数重置 | ✅ 直接复用 | — |
| `TransitionToGCPhase(TRACE, true)` | 切换到并发标记阶段 | ⚠️ 改造 | Phase 值改为 `GC_PHASE_MS_CONCURRENT_MARK` |
| `PrepareTrace()` | 准备 GhostRegion（CopyGC 用于 from→to 映射） | ❌ **去除** | MS 不需要 GhostRegion |
| **`DoTracing(workStack, foreignStack)`** | 并发标记框架 | ❌ **不复用** | MS 并发标记由 assist + GC 主线程实现 |
| `ProcessFinalizers()` | Finalizer 处理 | ✅ 直接复用 | 移到 MarkTermination 中 |

### 7.3 结论：去除 "fix ref" 即可

`TraceHeap` 本身**不包含 fix ref 逻辑**。所谓 "fix ref"（tag 检查、`FindLatestVersion`、CAS 更新引用）全部封装在 `WCollector` 的以下 virtual 方法中：

- `WCollector::TraceRefField()` — ~40 行 tag/forward/CAS 逻辑
- `WCollector::GetAndTryTagObj()` — ~25 行 tag/forward/CAS 逻辑
- `WCollector::EnumRefFieldRoot()` — tag 检查 + CAS

**MarkSweepCollector 只需 override 这些 virtual 方法为简化版本（~5 行/方法），即可达到 "去除 fix ref" 的效果**。主路径由 assist + GC 主线程驱动，不引入 worker 兜底路径。

### 7.4 MarkSweepCollector 新 TraceHeap 伪代码

```cpp
void MarkSweepCollector::ConcurrentMark() {
    // ❌ 去除: AssembleGarbageCandidates() — 无 from-space
    // ❌ 去除: DoEnumeration() — 已在 MarkSetup 中完成（Handshake 时各 Mutator 自扫描栈根）
    // ❌ 去除: PrepareTrace() — 无 GhostRegion

    // ✅ 复用
    markedObjectCount.store(0, std::memory_order_relaxed);
    TransitionToGCPhase(GCPhase::GC_PHASE_MS_CONCURRENT_MARK, true);

    // ✅ assist 主路径：Mutator 在分配/ safepoint 上偿还债务，
    //    从每P本地 + 全局 work buffer 处理灰对象
    RunAssistDrivenMarking();

    // 不启用 worker/pool 兜底路径

    // ✅ ProcessFinalizers 移至 MarkTermination
}
```

### 7.5 需 Override 的 virtual 方法简化版实现

```cpp
// MarkSweepCollector::TraceRefField — 去除所有 tag/forward/CAS
void MarkSweepCollector::TraceRefField(BaseObject* obj, RefField<>& field, WorkStack& workStack) const
{
    BaseObject* target = field.GetTargetObject();
    if (target == nullptr || !Heap::IsHeapAddress(target)) {
        return;
    }
    // 无需 IsCurrentPointer / IsOldPointer / FindLatestVersion / GetAndTryTagRefField / CAS
    if (MarkObject(target)) {
        workStack.push_back(target);
    }
}

// MarkSweepCollector::GetAndTryTagObj — 去除 tag/CAS，保留弱引用语义
BaseObject* MarkSweepCollector::GetAndTryTagObj(BaseObject* obj, RefField<>& field)
{
    BaseObject* target = field.GetTargetObject();
    if (target == nullptr || !Heap::IsHeapAddress(target)) {
        return nullptr;
    }
    // 无需 tag/CAS
    return target;
}

// MarkSweepCollector::EnumRefFieldRoot — 去除 tag/CAS
void MarkSweepCollector::EnumRefFieldRoot(RefField<>& field, RootSet& rootSet) const
{
    BaseObject* target = field.GetTargetObject();
    if (target == nullptr || !Heap::IsHeapAddress(target)) {
        return;
    }
    rootSet.push_back(target);
}
```
