# Step 2.3：Mark Termination（STW）详细设计

> 本文档从 [Step 2：三色标记与 Sweep 详细设计](Step2_三色标记与Sweep详细设计.md) §6 拆分而来。
> 前置：[Step 2.2：Concurrent Mark（并发标记）详细设计](Step2.2_ConcurrentMark_详细设计.md)

---

## 目录

- [Step 2.3：Mark Termination（STW）详细设计](#step-23mark-terminationstw详细设计)
  - [目录](#目录)
  - [1. 目标与边界](#1-目标与边界)
  - [2. 输入/输出不变量](#2-输入输出不变量)
  - [3. 总体流程（推荐 STW-first）](#3-总体流程推荐-stw-first)
  - [4. SATB 收敛策略](#4-satb-收敛策略)
    - [4.1 `MarkSatbBuffer` 现有实现评估](#41-marksatbbuffer-现有实现评估)
    - [4.2 复用建议：部分复用，不直接照搬](#42-复用建议部分复用不直接照搬)
    - [4.3 推荐实现：统一收集 + Execute 同构标记](#43-推荐实现统一收集--execute-同构标记)
  - [5. Finalizer / WeakRef 顺序](#5-finalizer--weakref-顺序)
  - [6. 与 Step2.2 的交接契约](#6-与-step22-的交接契约)
  - [7. 伪代码（Step 2 落地版）](#7-伪代码step-2-落地版)
  - [8. STW 时间与降级策略](#8-stw-时间与降级策略)

---

## 1. 目标与边界

`MarkTermination` 的职责是：在一次短 STW 窗口内，把并发标记阶段的“尾巴”收敛到一致状态，然后切到 Sweep。

本阶段**只做标记一致性收尾**，不承担并发阶段吞吐任务；并发吞吐由 Step2.2 负责。

## 2. 输入/输出不变量

进入本阶段前（来自 Step2.2）：

1. 主/分队列已达到稳定空（连续 N 轮收敛）
2. 写屏障仍在工作，SATB 仍可能有残留
3. assist 入口已关闭，局部队列内容可在 STW 内统一回吐/收敛

退出本阶段后：

1. SATB 缓冲已处理并清空
2. 终结器复活链已完成补标记
3. 弱引用已按存活性清理/修正
4. 屏障切回 idle，进入 Sweep 阶段

## 3. 总体流程（推荐 STW-first）

Step 2 推荐先采用“直接 STW 收敛”的简单可靠方案：

1. Stop-The-World，冻结 mutator
2. 关闭 assist 入口
3. 将“需要补标”的未标记对象统一收集到主队列（P 队列 / GC 队列 / SATB / Finalizer / WeakRef）
4. 使用与 `void Execute(size_t) override` 同构的循环，持续消费主队列完成补标
5. 卸载标记屏障并切到 Sweep

该方案实现简单、语义清晰，且与当前 Runtime 的保守策略一致。

## 4. SATB 收敛策略

### 4.1 `MarkSatbBuffer` 现有实现评估

`TracingCollector::MarkSatbBuffer(WorkStack&)` 的核心能力：

- 遍历所有 mutator 的 SATB node + retired node
- 把未标记对象放入 `workStack`
- 循环调用 `TracingImpl(...)` 直到无新对象

可见它在语义上可用于 MarkTermination 收尾。

### 4.2 复用建议：部分复用，不直接照搬

直接调用原 `MarkSatbBuffer` 在 Step2 可行，但有两个不匹配点：

1. 它内含 tracing 通用并发调度分支，不是“纯 STW 收尾接口”
2. 它内部会触发 `GC_PHASE_CLEAR_SATB_BUFFER` 等通用 phase 过渡，和 MS 专用 phase 编排耦合度偏高

因此建议：

- **复用其“收集 SATB 对象 + 迭代直到稳定空”的核心逻辑**
- **在 MarkSweepCollector 中提供 STW 专用封装**（仅 GC 主线程串行收敛）
- **MS 阶段切换统一通过 `TransitionToGCPhase(...)` 完成**（其内部已负责 `InstallBarrier`，不再单独显式安装屏障）

### 4.3 推荐实现：统一收集 + Execute 同构标记

推荐新增 STW 版本（可先内联在 `MarkTermination`）：

- 输入：`WorkStack& mainQueue`
- 收集：按当前 `MarkSatbBuffer` 的方式收集 SATB 对象到主队列
- 执行：通过 Execute 同构循环消费主队列
- 退出：一次“收集后主队列为空”

其中消费逻辑与 `Execute(size_t)` 保持一致：`MarkObject` + `TraceObjectRefFields` + WeakRef 分支。

## 5. Finalizer / WeakRef 顺序

推荐顺序：

1. 收集 SATB 候选到主队列
2. 收集 Finalizer 相关待补标对象到主队列
3. 复用 `FindUselessExternObjects` 选择逻辑，将候选对象回灌主队列
4. 复用 `DoResurrection` 选择逻辑，将复活候选对象回灌主队列
5. 统一执行 Execute 同构循环标记
6. 标记完成后做 WeakRef 终态清理（referent 白则置空）

理由：Finalizer 可能引入新的可达链，应先完成复活补标，再做弱引用终态裁剪。

## 6. 与 Step2.2 的交接契约

Step2.2 结束条件是“灰队列稳定空”，不是“SATB 已空”。

因此交接契约为：

- Step2.2 负责并发吞吐与队列收敛
- Step2.3 负责 STW 尾收敛（SATB / Finalizer / WeakRef）

## 7. 伪代码（Step 2 落地版）

```cpp
void MarkSweepCollector::MarkTermination()
{
    ScopedStopTheWorld stw("MS MarkTermination", true, GCPhase::GC_PHASE_MS_MARK_TERMINATION);

    DisableAssistEntry();
    WorkStack mainQueue;

    // 1) 统一收集未标记对象到主队列
    CollectUnmarkedToMainQueue(mainQueue);

    // 2) 使用 Execute 同构逻辑进行补标
    ExecuteLikeDrainMainQueue(mainQueue);

    // 3) 切换到 Sweep 阶段（卸载标记屏障）
    TransitionToGCPhase(GCPhase::GC_PHASE_MS_SWEEP, true);

    // 注：弱引用终态清理、SATB 缓冲清空等后续工作
    // 不需要在 STW 窗口内完成，移至 Step2.4 PostMarkTermination 阶段。
}

void MarkSweepCollector::CollectUnmarkedToMainQueue(WorkStack& mainQueue)
{
  PublishLocalOverflowToGlobal(mainQueue);        // 收集 P 上标记队列到主队列
  CollectGCDrainQueueToMain(mainQueue);           // 收集 GC 线程本地队列到主队列

  // SATB 收集方式与当前实现一致：
  // 1) 遍历所有 mutator 当前 SATB node
  // 2) 合并 retired SATB objects
  // 3) 仅将 heap 且未标记对象推入主队列
  CollectSatbCandidatesToMain(mainQueue);

  // 复用 FindUselessExternObjects 的“未标记则保留/处理”筛选口径：
  // 对 discoveredExternObjects 中仍未标记对象，回灌到 mainQueue 等待统一补标。
  CollectExternCandidatesToMain(mainQueue);

  // 复用 DoResurrection 的“finalizer 指向且未标记”筛选口径：
  // 将 finalizer 可复活候选回灌到 mainQueue，再由 Execute 同构逻辑统一扩展可达图。
  CollectResurrectionCandidatesToMain(mainQueue);

}

void MarkSweepCollector::CollectSatbCandidatesToMain(WorkStack& mainQueue)
{
  WorkStack satbTmp;

  // 1) 遍历所有 mutator 的当前 SATB node
  MutatorManager::Instance().VisitAllMutators([&satbTmp](Mutator& mutator) {
    const SatbBuffer::Node* node = mutator.GetSatbBufferNode();
    if (node != nullptr) {
      const_cast<SatbBuffer::Node*>(node)->GetObjects(satbTmp);
    }
  });

  // 2) 合并 retired SATB objects
  SatbBuffer::Instance().GetRetiredObjects(satbTmp);

  // 3) 仅将 heap 且未标记对象回灌到主队列
  while (!satbTmp.empty()) {
    BaseObject* obj = satbTmp.back();
    satbTmp.pop_back();
    if (Heap::IsHeapAddress(obj) && !IsMarkedObject(obj)) {
      mainQueue.push_back(obj);
    }
  }
}

void MarkSweepCollector::CollectExternCandidatesToMain(WorkStack& mainQueue)
{
  for (auto& [exportObj, externList] : discoveredExternObjects) {
    (void)exportObj;
    for (BaseObject* obj : externList) {
      if (!IsMarkedObject(obj)) {
        mainQueue.push_back(obj);
      }
    }
  }
}

void MarkSweepCollector::CollectResurrectionCandidatesToMain(WorkStack& mainQueue)
{
  VisitFinalizerRoots([&](ObjectRef& ref) {
    RefField<>& refField = reinterpret_cast<RefField<>&>(ref);
    BaseObject* finalizerObj = refField.GetTargetObject();
    if (finalizerObj != nullptr && !IsMarkedObject(finalizerObj)) {
      mainQueue.push_back(finalizerObj);
    }
  });
}

void MarkSweepCollector::ExecuteLikeDrainMainQueue(WorkStack& mainQueue)
{
  while (!mainQueue.empty()) {
    BaseObject* obj = mainQueue.back();
    mainQueue.pop_back();

    bool wasMarked = MarkObject(obj);
    if (wasMarked) {
      continue;
    }
    if (!obj->HasRefField()) {
      continue;
    }
    if (UNLIKELY(obj->IsWeakRef())) {
      RefField<>* referentField = reinterpret_cast<RefField<>*>((uintptr_t)obj + TYPEINFO_PTR_SIZE);
      BaseObject* referent = GetAndTryTagObj(obj, *referentField);
      if (referent != nullptr) {
        TraceObjectRefFields(referent, mainQueue);
        WeakRefBuffer::Instance().Insert(obj);
      }
    } else {
      TraceObjectRefFields(obj, mainQueue);
    }
  }
}
```

## 8. STW 时间与降级策略

目标：常规场景 STW < 1ms（工程目标，非硬保证）。

若出现异常积压：

1. 首选继续 STW 串行 drain 到空（保证正确性）
2. 若停顿超预算，优先缩小单轮处理预算并分轮触发，不扩展 worker 执行主体
3. 不建议在 Step 2 首版引入复杂的“退出 STW 再并发重入”流程

结论：**Step 2 先用 STW-first 实现 MarkTermination，`MarkSatbBuffer` 采用“核心逻辑复用 + MS 专用封装”。**
