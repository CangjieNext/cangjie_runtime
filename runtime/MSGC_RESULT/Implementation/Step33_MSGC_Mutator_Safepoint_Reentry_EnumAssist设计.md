# Step33：MSGC Mutator Safepoint Re-entry Enum Assist 设计

> 2026-06-26
> 目标：在当前 `MS_MARK_SETUP` / `GC_PHASE_ENUM` 握手框架不大改的前提下，引入一套“恢复业务后再次命中 safepoint 时，mutator 顺手帮助 GC 线程完成剩余 enum 收口”的增量方案。
> 口径：以当前 runtime 源码为准，重点讨论 **mutator 根扫描收口加速**，不覆盖 `allCJThreadList` / parked coroutine roots 的并行化问题。

---

## 目录

1. [结论先行](#1-结论先行)
2. [想解决的具体问题](#2-想解决的具体问题)
3. [当前机制的真实语义](#3-当前机制的真实语义)
4. [为什么不能直接复用当前 `undoneMutators`](#4-为什么不能直接复用当前-undonemutators)
5. [总体设计](#5-总体设计)
6. [状态与数据结构建议](#6-状态与数据结构建议)
7. [执行时序](#7-执行时序)
8. [伪代码草案](#8-伪代码草案)
9. [与 `EnsurePhaseTransition` 的关系](#9-与-ensurephasetransition-的关系)
10. [正确性约束与风险点](#10-正确性约束与风险点)
11. [如何保证只在 MSGC 下执行](#11-如何保证只在-msgc-下执行)
12. [预计改动文件](#12-预计改动文件)
13. [`flag` 生命周期与状态流转图](#13-flag-生命周期与状态流转图)
14. [`SUSPENSION_FOR_GC_ENUM_ASSIST` 的清理流程](#14-suspension_for_gc_enum_assist-的清理流程)
15. [第一版中的 helper/GC 线程撞车语义](#15-第一版中的-helpergc-线程撞车语义)
16. [分阶段落地建议](#16-分阶段落地建议)
17. [一句话总结](#17-一句话总结)

---

## 1. 结论先行

如果要把“未完成的 mutator enum 收口”进一步并行化，建议采用下面这条主线：

1. **保留当前 `TransitionAllMutatorsToGCPhase()` + `EnsurePhaseTransition()` 作为主收口框架。**
2. **不要直接复用当前栈上 `undoneMutators` 作为共享任务池。**
3. **新增一类独立于 `SUSPENSION_FOR_GC_PHASE` 的 assist request，例如 `SUSPENSION_FOR_GC_ENUM_ASSIST`。**
4. **每个 mutator 先按现有逻辑完成一次自己的 `GcPhaseEnum()`。**
5. **已完成自扫的 mutator，在后续 safepoint / LeaveSaferegion 慢路径中，如果发现 assist request 仍然存在，就顺手帮 GC 线程对其他已 saferegion 的 mutator 执行 `TransitionGCPhase(false)`。**
6. **单次 assist 只处理固定批量的 mutator，最终收口仍由 `EnsurePhaseTransition()` 负责。**

这套方案的核心收益是：

> **不要求把 `MS_MARK_SETUP` 改造成全新的两阶段大握手，也不要求 mutator 长时间停在 transition 内部，而是复用现有 safepoint slow path，让已经恢复业务的 mutator 在后续 safepoint 上“顺手补一点 enum 收口工作”。**

---

## 2. 想解决的具体问题

当前 `MS_MARK_SETUP` 的收口方式是：

1. GC 线程广播 `SUSPENSION_FOR_GC_PHASE`；
2. 每个 mutator 在命中 safepoint / 离开 saferegion 时执行一次 `TransitionGCPhase(true)`；
3. 对 `GC_PHASE_ENUM` / `GC_PHASE_MS_MARK_SETUP` 来说，真正执行的是 `GcPhaseEnum()`；
4. GC 线程在 `EnsurePhaseTransition()` 中等待所有 mutator 完成。

这套机制本身是正确的，但有一个特征：

> **一旦某些 mutator 已经快速完成自扫并恢复业务，它们不会再主动参与“帮助其他还没完成 enum 的 mutator 收口”，剩余收口主要靠 GC 线程自己轮询 `EnsurePhaseTransition()`。**

在高并发协程 / 大量 mutator 场景下，这会造成：

1. 一部分 mutator 很快完成自扫，但后续执行资源没有被继续利用；
2. 少数慢 mutator 或 saferegion 时机不理想的 mutator，拖长整个 `EnumRoots` 收口时间；
3. `EnsurePhaseTransition()` 更像单线程 barrier，而不是“GC 线程 + mutator 共治”的 barrier。

所以这里真正要补的不是“再做一轮 root 枚举”，而是：

> **让已经完成一次 `GcPhaseEnum()` 的 mutator，在后续 safepoint 上再次被拦下时，顺手替还未完成的 mutator 做一小批 phase transition / root 扫描收口。**

---

## 3. 当前机制的真实语义

先把当前代码里的关键语义摆正。

### 3.1 `SUSPENSION_FOR_GC_PHASE` 的广播

`src/Mutator/MutatorManager.cpp`

当前广播逻辑本质是：

1. `SetSuspensionFlag(SUSPENSION_FOR_GC_PHASE)`
   - 令 `transitionState = NEED_TRANSITION`
   - 打开 `suspensionFlag`
2. `SetSafepointActive(true)`
   - 使后续 safepoint / LeaveSaferegion 慢路径可感知这次 phase 请求
3. 把 mutator 记入 `undoneMutators`

### 3.2 mutator 什么时候真的执行 `HandleGCPhase`

当前主要有三类入口：

1. **离开 saferegion 时**
   - `DoLeaveSaferegion()`
   - 若发现 `HasAnySuspensionRequest()`，则进入 `HandleSuspensionRequest()`
2. **运行中命中 safepoint / suspension slow path**
   - 进入 `HandleSuspensionRequest()`
3. **已经在 saferegion 时，被 GC 线程代执行**
   - `EnsurePhaseTransition()` 中的 `mutator->TransitionGCPhase(false)`

### 3.3 同一个 mutator 对同一轮 phase 只 transition 一次

这一点非常关键。

当前同一个 mutator 在同一轮 `SUSPENSION_FOR_GC_PHASE` 下：

1. 第一次进入 `TransitionGCPhase(true/false)`；
2. 执行 `TransitionToGCPhaseExclusive()`；
3. 完成后：
   - `transitionState = FINISH_TRANSITION`
   - `ClearSuspensionFlag(SUSPENSION_FOR_GC_PHASE)`
   - `SetMutatorPhase(newPhase)`

所以：

> **当前机制允许“多个不同 mutator 在 enum 期间分别进入 `HandleGCPhase`”，但不允许“同一个 mutator 对同一轮 phase 重复用 `SUSPENSION_FOR_GC_PHASE` 多次进入 `HandleGCPhase`”。**

这也是为什么“assist enum”不应该继续复用 `SUSPENSION_FOR_GC_PHASE`。

---

## 4. 为什么不能直接复用当前 `undoneMutators`

用户最初的直觉是：

> “如果 mutator 自己已经 `GcPhaseEnum` 完成了，能不能直接从 `undoneMutators` 里拿几个 mutator，顺手帮它们做 `mutator->InSaferegion() && mutator->TransitionGCPhase(false)`？”

这个方向在思想上是对的，但当前 `undoneMutators` 不能直接这样用，原因有四个。

### 4.1 它是局部变量

当前 `undoneMutators` 是 `TransitionAllMutatorsToGCPhase()` 栈上的 `std::list<Mutator*>`，生命周期和可见性都只在 GC 线程这条调用链里。

### 4.2 它不是线程安全 work queue

当前只有 GC 线程会在 `EnsurePhaseTransition()` 里遍历和擦除它。

如果 mutator 线程也开始并发 pop / erase：

1. 容器本身就不安全；
2. 迭代器失效和 ABA 问题会马上出现。

### 4.3 它的语义太粗

`undoneMutators` 表示的是：

> “这轮 phase 还没完成的人名单”

但这份名单里混着两类对象：

1. **已经在 saferegion，可被别人代做**
2. **还在运行态，还不能安全代做**

如果直接把它当 work queue，会不断抽到“现在还不能动”的 mutator。

### 4.4 它会和 `EnsurePhaseTransition()` 的主收口逻辑相互踩踏

当前 `EnsurePhaseTransition()` 依赖这份 list 做 barrier 收口。

如果 mutator assist 线程也在改这个容器，就会把：

1. “谁还没完成”
2. “谁已经被 helper 看过一次”
3. “谁已经真正完成”

这三种语义混在一起，收口证明会很难做。

所以结论是：

> **可以复用“`mutator->InSaferegion() && mutator->TransitionGCPhase(false)` 这个代做动作”，但不能直接复用当前 `undoneMutators` 容器本身。**

---

## 5. 总体设计

### 5.1 设计目标

目标收敛成一句话：

> **在 `MS_MARK_SETUP` 尚未完全收口时，让已经完成自扫的 mutator，在后续 safepoint / LeaveSaferegion 慢路径上重新被拦下，并顺手帮助 GC 线程完成一小批剩余 mutator 的 phase transition / root 扫描。**

### 5.2 设计原则

建议坚持下面四条原则：

1. **主 barrier 仍由 `EnsurePhaseTransition()` 负责。**
2. **assist 只是加速器，不是新的 completion 判定源。**
3. **assist 只能作用于“已经在 saferegion 的目标 mutator”。**
4. **assist 单次只做固定小批量，避免对业务线程造成长尾抖动。**

### 5.3 与 Step32 方案的边界

这个 Step33 方案解决的是：

- mutator 自扫完成后的“剩余 mutator root 扫描收口”问题

它**不直接解决**：

- `EnumConcurrencyModelRoots()` 对 `allCJThreadList` 的串行遍历慢问题

两者可以叠加，但不是同一条优化线。

---

## 6. 状态与数据结构建议

### 6.1 新增 assist request

在 `Mutator::SuspensionType` 中新增：

```cpp
SUSPENSION_FOR_GC_ENUM_ASSIST = ...;
```

语义：

- `SUSPENSION_FOR_GC_PHASE`：我自己要完成一次 phase transition
- `SUSPENSION_FOR_GC_ENUM_ASSIST`：我这轮 phase 自己已经做完了，但还需要在 safepoint 上顺手 assist 一小批别人

### 6.2 新增共享 assist 上下文

建议挂在 `MutatorManager` 上，示意如下：

```cpp
struct EnumAssistContext {
    GCPhase phase;
    std::vector<Mutator*> candidates;
    std::atomic<bool> active { false };
    std::atomic<uint32_t> cursor { 0 };
    std::atomic<uint32_t> remaining { 0 };
    uint32_t batchSize = 4;
    uint64_t epoch = 0;
};
```

字段含义：

1. `phase`
   - 当前 assist 服务的 phase，当前只针对 `GC_PHASE_ENUM` / `GC_PHASE_MS_MARK_SETUP`
2. `candidates`
   - 本轮 `VisitAllMutators()` 快照
3. `active`
   - assist 通道当前是否开启
4. `cursor`
   - 抢任务扫描游标
5. `remaining`
   - 剩余未完成 transition 的 mutator 近似计数
6. `batchSize`
   - 单次 assist 最多处理多少个
7. `epoch`
   - 避免下一轮 GC 复用旧 assist 状态

### 6.3 第一版不强制要求 per-mutator “本轮 enum 已完成”标记

按当前确定下来的 **方案 B**：

1. `SUSPENSION_FOR_GC_PHASE`
   - 只负责“这个 mutator 自己完成一次 `GcPhaseEnum()`”
2. `SUSPENSION_FOR_GC_ENUM_ASSIST`
   - 只会在该 mutator 完成 `TransitionGCPhase(true)` 之后才挂上

所以第一版里其实已经有一条足够清晰的职责切分：

```text
有 SUSPENSION_FOR_GC_PHASE
-> 说明自己还没完成本轮 enum

有 SUSPENSION_FOR_GC_ENUM_ASSIST
-> 说明自己已经完成本轮 enum，现在只负责 assist
```

在这个前提下，第一版**不强制要求**再给每个 mutator 增加：

```cpp
enumDoneEpoch_
```

作为额外状态。

也就是说：

> **第一版建议直接依赖两个 request flag 的生命周期完成“自扫”与“assist”职责切分，而不是再额外引入 per-mutator 完成标记。**

如果后续为了：

1. 调试；
2. 统计；
3. 更强的断言；
4. 更复杂的 assist 变种；

需要补一个：

```cpp
std::atomic<uint64_t> enumDoneEpoch_;
```

也是可以的，但它更适合作为第二版增强项，而不是第一版闭环必需项。

---

## 7. 执行时序

### 7.1 GC 线程广播 phase 请求

`TransitionAllMutatorsToGCPhase(phase)`：

1. 设置 barrier / 全局 phase
2. `VisitAllMutators(...)`
   - `SetSuspensionFlag(SUSPENSION_FOR_GC_PHASE)`
   - `SetSafepointActive(true)`
   - 填充 `EnumAssistContext.candidates`
3. `EnumAssistContext.active = true`
4. `EnumAssistContext.remaining = candidates.size()`
5. 进入 `EnsurePhaseTransition()`

### 7.2 mutator 第一次命中 `GC_PHASE_ENUM`

在 `HandleSuspensionRequest()` 里：

1. 命中 `SUSPENSION_FOR_GC_PHASE`
2. 执行 `TransitionGCPhase(true)`
3. 进入 `HandleGCPhase(newPhase)`
4. 若当前 phase 是 `GC_PHASE_ENUM` / `GC_PHASE_MS_MARK_SETUP`
   - 执行一次自己的 `GcPhaseEnum()`
5. `TransitionToGCPhaseExclusive()` 正常结束

### 7.3 mutator 恢复业务后再次命中 safepoint

如果这时：

1. `EnumAssistContext.active == true`
2. 当前 mutator 已经完成了本轮 `TransitionGCPhase(true)`，也就是它的 `SUSPENSION_FOR_GC_PHASE` 已被清掉
3. 当前 mutator 已被提升成 `SUSPENSION_FOR_GC_ENUM_ASSIST`
4. 还有 `remaining > 0`

则它在 `HandleSuspensionRequest()` 里命中：

- `SUSPENSION_FOR_GC_ENUM_ASSIST`

然后执行 `HandleEnumAssist()`：

1. 先从共享 `EnumAssistContext` 中 **claim 一小批** 候选 mutator 到当前 mutator 的本地 `localBatch[]`；
2. 单次 claim 的上限是 `batchSize`；
3. 再对本地 `localBatch[]` 中的每个候选：
   - 如果 `target == this`，跳过；
   - 如果 `!target->InSaferegion()`，跳过；
   - 如果 `target->TransitionGCPhase(false)` 成功，说明顺手替它完成了一次 phase transition；
4. 若 `remaining == 0`，后续 assist request 自动失效。

这里推荐采用 **方案 B：只让它下次 safepoint 再 assist**，而不是在同一次 `HandleSuspensionRequest()` 里把：

1. `TransitionGCPhase(true)`
2. `GcPhaseEnum()`
3. `HandleEnumAssist()`

连续串起来。

也就是说，当前 mutator 在完成自己的 `GcPhaseEnum()` 之后：

1. 本次 `SUSPENSION_FOR_GC_PHASE` 正常完成并返回；
2. 在这次 phase transition 的收尾阶段，若全局 `EnumAssistContext` 仍处于 `active` 且 `remaining > 0`，则把当前 mutator 从：
   - `SUSPENSION_FOR_GC_PHASE`
   - **切换到**
   - `SUSPENSION_FOR_GC_ENUM_ASSIST`
3. 更具体地说：
   - 原有 `TransitionToGCPhaseExclusive()` 会清掉 `SUSPENSION_FOR_GC_PHASE`
   - 随后由新增的 assist promotion 逻辑给当前 mutator 挂上 `SUSPENSION_FOR_GC_ENUM_ASSIST`
   - 并保持 safepoint gate 仍可再次工作；
4. **但不在本次同一个 `HandleSuspensionRequest()` 循环里立即消费 assist request；**
5. 等它恢复业务后，未来下一次再命中 safepoint / `DoLeaveSaferegion()` 慢路径时，再进入 `HandleEnumAssist()`。

这样做的原因是：

1. 更符合“恢复业务后的 opportunistic assist”语义；
2. 避免一次 slow path 停太久，把自扫和 assist 全挤在同一个停顿里；
3. phase transition 的完成边界更清楚：
   - `SUSPENSION_FOR_GC_PHASE` 只负责“自己完成一次 enum”
   - `SUSPENSION_FOR_GC_ENUM_ASSIST` 只负责“后续回来顺手帮别人”

### 7.4 当前推荐方案里两者是怎么切换的

第一版建议把切换点放在：

- `TransitionToGCPhaseExclusive()` 完成 `HandleGCPhase(newPhase)` 之后的收尾阶段
- 或者一个与其紧邻的 `MaybePromoteToEnumAssist()` 小逻辑里

推荐顺序是：

1. `HandleGCPhase(newPhase)`
   - 对 `GC_PHASE_ENUM` / `MS_MARK_SETUP` 来说，这里执行 `GcPhaseEnum()`
2. `SetSafepointActive(false)`
3. `ClearSuspensionFlag(SUSPENSION_FOR_GC_PHASE)`
4. `SetMutatorPhase(newPhase)`
5. 若：
   - 当前 phase 是 `GC_PHASE_ENUM` / `GC_PHASE_MS_MARK_SETUP`
   - `EnumAssistContext.active == true`
   - `EnumAssistContext.remaining > 0`
   则再执行：
   - `SetSuspensionFlag(SUSPENSION_FOR_GC_ENUM_ASSIST)`
   - `SetSafepointActive(true)`

也就是：

```text
SUSPENSION_FOR_GC_PHASE
  -> 完成 GcPhaseEnum
  -> 清掉 GC_PHASE request
  -> 若仍需帮忙，则挂上 SUSPENSION_FOR_GC_ENUM_ASSIST
```

注意，这里是 **切换 request**，不是“在同一次 `HandleSuspensionRequest()` 里立刻继续做 assist”。

### 7.5 GC 线程最终收口

GC 线程的 `EnsurePhaseTransition()` 逻辑不变：

1. 已完成的 mutator 从未完成集合中移除；
2. 已在 saferegion 的 mutator，GC 线程自己也可以继续 `TransitionGCPhase(false)`；
3. 直到所有 mutator 完成。

最后：

1. `EnumAssistContext.active = false`
2. 清理 assist request
3. 进入后续 root 汇总与 mark

---

## 8. 伪代码草案

### 8.1 广播阶段

```cpp
void MutatorManager::TransitionAllMutatorsToGCPhase(GCPhase phase)
{
    PrepareAssistContextIfNeeded(phase);

    VisitAllMutators([&](Mutator& mutator) {
        mutator.SetSuspensionFlag(SUSPENSION_FOR_GC_PHASE);
        mutator.SetSafepointActive(true);
        if (phase == GC_PHASE_ENUM || phase == GC_PHASE_MS_MARK_SETUP) {
            enumAssistContext_.candidates.push_back(&mutator);
        }
    });

    if (phase == GC_PHASE_ENUM || phase == GC_PHASE_MS_MARK_SETUP) {
        enumAssistContext_.remaining.store(enumAssistContext_.candidates.size());
        enumAssistContext_.active.store(true);
    }

    EnsurePhaseTransition(phase, undoneMutators);
    FinalizeAssistContextIfNeeded(phase);
}
```

### 8.2 mutator 自扫完成后切换到 assist request

```cpp
void Mutator::HandleGCPhase(GCPhase newPhase)
{
    if (newPhase == GC_PHASE_ENUM || newPhase == GC_PHASE_MS_MARK_SETUP) {
        GcPhaseEnum(newPhase);
        return;
    }
}

void Mutator::TransitionToGCPhaseExclusive(GCPhase newPhase)
{
    HandleGCPhase(newPhase);
    SetSafepointActive(false);
    ClearSuspensionFlag(SUSPENSION_FOR_GC_PHASE);
    SetMutatorPhase(newPhase);

    if ((newPhase == GC_PHASE_ENUM || newPhase == GC_PHASE_MS_MARK_SETUP) &&
        MutatorManager::Instance().NeedEnumAssistAfterSelfEnum()) {
        SetSuspensionFlag(SUSPENSION_FOR_GC_ENUM_ASSIST);
        SetSafepointActive(true);
    }
}
```

### 8.3 safepoint assist

```cpp
void Mutator::HandleSuspensionRequest()
{
    for (;;) {
        SetInSaferegion(SAFE_REGION_TRUE);

        if (HasSuspensionRequest(SUSPENSION_FOR_GC_PHASE)) {
            TransitionGCPhase(true);
        } else if (HasSuspensionRequest(SUSPENSION_FOR_GC_ENUM_ASSIST)) {
            HandleEnumAssist();
        } else if (HasSuspensionRequest(SUSPENSION_FOR_CPU_PROFILE)) {
            TransitionToCpuProfile(true);
        }

        SetInSaferegion(SAFE_REGION_FALSE);
        if (!HasAnySuspensionRequest() && !HasObserver()) {
            return;
        }
    }
}
```

### 8.4 claim 一批 assist target 到本地

```cpp
uint32_t Mutator::ClaimEnumAssistTargets(Mutator** localBatch, uint32_t maxBatch)
{
    EnumAssistContext& ctx = MutatorManager::Instance().GetEnumAssistContext();
    uint32_t count = 0;
    while (count < maxBatch) {
        uint32_t idx = ctx.cursor.fetch_add(1);
        if (idx >= ctx.candidates.size()) {
            break;
        }
        localBatch[count++] = ctx.candidates[idx];
    }
    return count;
}
```

### 8.5 assist 主体

```cpp
void Mutator::HandleEnumAssist()
{
    EnumAssistContext& ctx = MutatorManager::Instance().GetEnumAssistContext();
    if (!ctx.active.load()) {
        ClearSuspensionFlag(SUSPENSION_FOR_GC_ENUM_ASSIST);
        return;
    }

    Mutator* localBatch[kAssistBatchSizeMax];
    uint32_t count = ClaimEnumAssistTargets(localBatch, ctx.batchSize);
    uint32_t done = 0;
    for (uint32_t i = 0; i < count; ++i) {
        Mutator* target = localBatch[i];
        if (target == this) {
            continue;
        }
        if (!target->InSaferegion()) {
            continue;
        }
        if (target->TransitionGCPhase(false)) {
            ++done;
            ctx.remaining.fetch_sub(1);
        }
    }

    if (!ctx.active.load() || ctx.remaining.load() == 0) {
        ClearSuspensionFlag(SUSPENSION_FOR_GC_ENUM_ASSIST);
    }
}
```

注意：

1. 上面伪代码只表达方向；
2. 第一版建议采用“先 claim 到本地 batch，再执行”的两段式结构；
3. 真实实现里 `cursor` 轮询扫描仍需要处理“有些目标暂时不在 saferegion，后面还要再看”的问题，不能简单把一次跳过当成永久放弃；
4. 如果后续要进一步减少 helper/GC 线程撞车，可以在第二版把 `ClaimEnumAssistTargets()` 升级成带 `UNCLAIMED / CLAIMED / DONE` 状态的显式 claim 机制。

---

## 9. 与 `EnsurePhaseTransition` 的关系

这个方案最重要的设计点是：

> **`EnsurePhaseTransition()` 仍然是唯一 authoritative barrier，assist 只是并行加速。**

也就是说：

1. mutator assist 成功
   - 只是提前替 GC 线程完成了一部分 `TransitionGCPhase(false)`
2. mutator assist 失败
   - 不影响正确性，GC 线程后面仍可继续代做
3. mutator 长时间不再命中 safepoint
   - 也不影响正确性，最终还是由 `EnsurePhaseTransition()` 等待并收口

所以这不是“换掉原来的 barrier”，而是：

> **把原来只有 GC 线程能做的 saferegion 代做动作，扩展成“GC 线程 + 已完成自扫的 mutator 都能做”。**

---

## 10. 正确性约束与风险点

### 10.1 只能 assist 已经在 saferegion 的 mutator

这是第一条红线。

assist 逻辑只能对：

```cpp
target->InSaferegion() == true
```

的 mutator 执行 `TransitionGCPhase(false)`。

否则就变成“跨线程扫还在运行的 mutator root”，这是不成立的。

### 10.2 不能再次复用 `SUSPENSION_FOR_GC_PHASE`

如果为了 assist 又重新把 `SUSPENSION_FOR_GC_PHASE` 置回去，会直接扰乱：

1. `transitionState`
2. `FinishedTransition()`
3. `SetMutatorPhase(newPhase)`

所以 assist request 必须独立。

### 10.3 assist 单次必须限批

如果每次 safepoint 都无限扫，会带来两个问题：

1. 某个业务 mutator 一次 slow path 停太久；
2. 长尾延迟反而被放大。

所以单次建议默认只 assist：

- `4`
- 或 `8`

个目标 mutator，可后续再参数化。

### 10.4 assist context 仍建议保留 epoch，但第一版不强制要求 per-mutator enumDoneEpoch

第一版虽然不强制要求每个 mutator 再加：

```cpp
enumDoneEpoch_
```

但共享的 `EnumAssistContext` 仍建议保留：

```cpp
epoch
```

原因是：

1. 上一轮 assist request 可能残留；
2. 下一轮 GC 需要一个明确的“这是新 assist 轮次”的标识；
3. 后续如果再补更强的调试/统计或 per-mutator 标记，也能直接与该 `epoch` 对齐。

也就是说：

> **第一版建议保留 context-level epoch，去掉 per-mutator enumDoneEpoch 的硬依赖。**

### 10.5 不解决 `allCJThreadList` 串行枚举瓶颈

这个方案只优化：

- mutator 自扫完成后的 phase 收口速度

它不解决：

- `CJThreadModel::VisitGCRoots()` 本身串行遍历 `allCJThreadList` 的耗时

这一点要和 Step32 里的 parked coroutine roots 并行化方案分开看。

---

## 11. 如何保证只在 MSGC 下执行

这里单独记录“这套方案怎么保证只在 MSGC 下生效”，避免后续实现时误影响 CopyGC 或其他 GC phase。

### 11.1 编译期保证

第一层保证是：

> **所有新增的 enum assist 逻辑都放在 `#ifdef MRT_USE_MARK_SWEEP` 之下。**

包括但不限于：

1. `SUSPENSION_FOR_GC_ENUM_ASSIST`
2. `EnumAssistContext`
3. `HandleEnumAssist()`
4. `NeedEnumAssistAfterSelfEnum()`
5. `TransitionToGCPhaseExclusive()` 中 phase -> assist 的 promotion 逻辑

这样在非 MSGC 构建里，这套逻辑根本不会进入二进制。

### 11.2 运行时 phase 保证

第二层保证是：

> **即便在 `MRT_USE_MARK_SWEEP` 下，也只对 `GC_PHASE_MS_MARK_SETUP` 生效，而不对通用 `GC_PHASE_ENUM` 生效。**

这点要明确，因为：

1. CopyGC 也有 `GC_PHASE_ENUM`
2. 当前这套方案的目标，是缩短 MSGC 的 `MS_MARK_SETUP` 收口
3. 所以不能把这套 assist 逻辑写成“所有 enum phase 都启用”

建议运行时口径统一成：

1. `TransitionAllMutatorsToGCPhase()` 中
   - 只有 `phase == GC_PHASE_MS_MARK_SETUP` 时才初始化 `EnumAssistContext`
2. `TransitionToGCPhaseExclusive()` 中
   - 只有 `newPhase == GC_PHASE_MS_MARK_SETUP` 时才允许从 `SUSPENSION_FOR_GC_PHASE` promotion 到 `SUSPENSION_FOR_GC_ENUM_ASSIST`
3. `HandleEnumAssist()` 中
   - 也要校验当前 assist context 绑定的 phase 是 `GC_PHASE_MS_MARK_SETUP`

### 11.3 参数也应只按 MSGC 语义解释

比如后面新增的：

- `cjMSEnumAssistBatchSize`

虽然是统一从 runtime 参数解析，但其语义应明确写成：

> **仅对 MSGC `MS_MARK_SETUP` assist 生效。**

这样就不会和 CopyGC 或别的 phase 参数语义混在一起。

---

## 12. 预计改动文件

下面记录第一版预计会落到哪些文件，以及每个文件承担什么职责。

### 12.1 `src/Mutator/Mutator.h`

预计放：

1. `SuspensionType` 新增 `SUSPENSION_FOR_GC_ENUM_ASSIST`
2. `HandleEnumAssist()` 声明
3. `ClaimEnumAssistTargets()` 声明
4. 少量 `#ifdef MRT_USE_MARK_SWEEP` 下的 helper 声明

### 12.2 `src/Mutator/Mutator.cpp`

预计放：

1. `HandleSuspensionRequest()` 新增 `SUSPENSION_FOR_GC_ENUM_ASSIST` 分支
2. `HandleEnumAssist()` 实现
3. `HandleGCPhase()` 仍只负责 `GcPhaseEnum()`，不直接做 assist
4. `TransitionToGCPhaseExclusive()` 中 phase -> assist 的 promotion 逻辑

### 12.3 `src/Mutator/MutatorManager.h`

预计放：

1. `EnumAssistContext` 结构
2. `GetEnumAssistContext()`
3. `NeedEnumAssistAfterSelfEnum()`
4. assist context 的少量管理接口

### 12.4 `src/Mutator/MutatorManager.cpp`

预计放：

1. `TransitionAllMutatorsToGCPhase()` 中
   - 仅在 `phase == GC_PHASE_MS_MARK_SETUP` 时初始化 assist context
2. `EnsurePhaseTransition()` 收口后
   - 统一关闭 assist context
   - 统一清理 `SUSPENSION_FOR_GC_ENUM_ASSIST`

### 12.5 `src/Cangjie.h`

预计放：

1. `GCParam` 新增字段，例如：

```cpp
size_t msEnumAssistBatchSize;
```

### 12.6 `src/CjScheduler.cpp`

预计放：

1. 启动时参数解析
2. 新增：

```text
cjMSEnumAssistBatchSize
```

默认值 `10`，风格与现有 GC 参数保持一致。

> 代码现状（2026-07-08）：
> `cjMSEnumAssistBatchSize` 当前只控制单次 claim 的 batch 大小，
> **不是** enable/disable 开关；传 `0` 会在
> `src/CjScheduler.cpp` / `src/CangjieRuntimeApi.cpp` 中被归一化回默认值 `10`。
> 当前 runtime 源码已经新增独立开关：
> `cjMSEnumAssist`（环境变量路径）以及 `GCParam.msEnumAssist`
>（`InitCJRuntime` 输入路径，`0=default(on), 1=on, 2=off`）。

### 12.7 `src/CangjieRuntimeApi.cpp`

如果需要走 runtime param 归一化 / 默认值落地，则这里也要补：

1. `GCParam` 默认值处理
2. 对 `msEnumAssistBatchSize` 的归一化

### 12.8 第一版暂不准备动的文件

第一版原则上不打算碰：

1. `TracingCollector.*`
   - 因为这版不解决 `allCJThreadList` 的 parked coroutine roots 串行遍历慢问题
2. `MarkSweepCollector.cpp`
   - 除非后面为了统计/日志需要补极少量 hook
3. `Base/EnvParam.cpp`
   - 现有 `ReadCountParameter(...)` 已足够读取 `batchSize`

---

## 13. `flag` 生命周期与状态流转图

这里单独把第一版推荐方案里两个 request flag 的生命周期写成一个清晰的状态机，避免后续实现时把：

1. “自己完成本轮 enum”
2. “后续回来 assist 别人”

混成一个 request。

### 11.1 两个 flag 的职责边界

1. `SUSPENSION_FOR_GC_PHASE`
   - 表示：**这个 mutator 自己还没完成这轮 phase transition / `GcPhaseEnum()`**
2. `SUSPENSION_FOR_GC_ENUM_ASSIST`
   - 表示：**这个 mutator 自己已经完成这轮 enum，但在后续 safepoint 上还应顺手 assist 一小批别人**

### 11.2 推荐的 flag 生命周期

推荐采用下面这个状态流：

```text
[初始广播]
GC线程:
  SetSuspensionFlag(SUSPENSION_FOR_GC_PHASE)
  SetSafepointActive(true)

    |
    v

[第一次命中 safepoint / LeaveSaferegion]
HandleSuspensionRequest():
  命中 SUSPENSION_FOR_GC_PHASE
  -> TransitionGCPhase(true)
  -> HandleGCPhase(GC_PHASE_ENUM / MS_MARK_SETUP)
  -> GcPhaseEnum()

    |
    v

[自扫完成]
TransitionToGCPhaseExclusive():
  ClearSuspensionFlag(SUSPENSION_FOR_GC_PHASE)
  SetMutatorPhase(newPhase)
  transitionState = FINISH_TRANSITION

  if (EnumAssistContext.active && remaining > 0) {
      SetSuspensionFlag(SUSPENSION_FOR_GC_ENUM_ASSIST)
      SetSafepointActive(true)
  } else {
      SetSafepointActive(false)
  }

    |
    v

[恢复业务]
当前 mutator 继续运行

    |
    v

[下一次命中 safepoint / LeaveSaferegion]
HandleSuspensionRequest():
  命中 SUSPENSION_FOR_GC_ENUM_ASSIST
  -> HandleEnumAssist()
  -> assist 一小批 target

    |
    v

[assist 结束或本轮已关闭]
  ClearSuspensionFlag(SUSPENSION_FOR_GC_ENUM_ASSIST)
  if (!HasAnySuspensionRequest()) {
      SetSafepointActive(false)
  }
```

### 11.3 为什么推荐“下次 safepoint 再 assist”

这里明确采用 **方案 B**：

> **自扫完成后，先把 assist flag 挂上，但不在本次同一个 `HandleSuspensionRequest()` 里立即执行 assist；而是等它恢复业务后，下一次再命中 safepoint 时才去 assist。**

原因：

1. phase transition 的边界更清楚；
2. 一次 slow path 不会被 phase + assist 串成很长的停顿；
3. 更符合“opportunistic re-entry assist”的原始目标。

### 11.4 不推荐的做法

第一版不建议做成：

```text
一次 HandleSuspensionRequest
  -> 先 TransitionGCPhase(true)
  -> 再立刻 HandleEnumAssist()
  -> 再继续循环
```

这种写法的问题是：

1. 让 `SUSPENSION_FOR_GC_PHASE` 和 `SUSPENSION_FOR_GC_ENUM_ASSIST` 的职责边界变模糊；
2. 一次停顿里做太多事情；
3. 更难控制单次 assist 的 budget。

所以第一版推荐口径应当明确写成：

> **`SUSPENSION_FOR_GC_PHASE` 负责“这次进来把自己扫完”；`SUSPENSION_FOR_GC_ENUM_ASSIST` 负责“以后再回来顺手帮别人”。**

---

## 14. `SUSPENSION_FOR_GC_ENUM_ASSIST` 的清理流程

这里单独记录 assist request 的清理语义，避免后续实现时留下“assist 已结束，但某些 mutator 身上还残留 `SUSPENSION_FOR_GC_ENUM_ASSIST`”的问题。

### 12.1 总原则

`SUSPENSION_FOR_GC_ENUM_ASSIST` 的清理建议采用 **两层收口**：

1. **mutator 本地懒清理**
   - mutator 再次命中 safepoint / `HandleEnumAssist()` 时，发现 assist 已无必要，就当场清掉自己的 assist flag；
2. **GC 线程全局强制清理**
   - 当 `EnsurePhaseTransition()` 确认本轮 `MS_MARK_SETUP` / `GC_PHASE_ENUM` 已完全收口后，统一关闭 assist context，并扫一遍 mutator 把 assist flag 清干净。

这样做的原因是：

1. 只做本地清理不够
   - 某些 mutator 在 assist 结束后，可能很久都不再命中 safepoint；
2. 只做 GC 线程全局清理也不够
   - assist 已经结束，但某个 mutator 正好又命中一次 safepoint，仍可能白进一次 `HandleEnumAssist()`。

所以推荐口径是：

> **本地懒清一次，全局收尾再强制清一次。**

### 12.2 mutator 本地清理时机

在 `HandleEnumAssist()` 末尾，若满足下面任一条件，就应当清掉当前 mutator 的：

```cpp
SUSPENSION_FOR_GC_ENUM_ASSIST
```

推荐条件包括：

1. `EnumAssistContext.active == false`
   - 这轮 assist 全局已关闭；
2. `EnumAssistContext.epoch != myAssistEpoch`
   - 当前 assist request 已经不是自己这轮的 assist；
3. `EnumAssistContext.remaining == 0`
   - 理论上已经没有剩余未完成 transition；
4. 本次进入 `HandleEnumAssist()` 后，发现已无可做工作
   - 这是可选弱条件，用于进一步减少无效再次进入。

建议清理逻辑写成：

```cpp
ClearSuspensionFlag(SUSPENSION_FOR_GC_ENUM_ASSIST);
if (!HasAnySuspensionRequest()) {
    SetSafepointActive(false);
}
```

注意：

> **不要因为清了 assist flag 就无脑 `SetSafepointActive(false)`。**

因为该 mutator 身上可能还挂着：

1. `SUSPENSION_FOR_GC_PHASE`
2. `SUSPENSION_FOR_SYNC`
3. `SUSPENSION_FOR_CPU_PROFILE`
4. preempt request

只有当 `HasAnySuspensionRequest() == false` 时，才可以顺手把 safepoint gate 关掉。

### 12.3 GC 线程全局清理时机

当 `EnsurePhaseTransition()` 真正结束，也就是：

1. 本轮 `undoneMutators` 已清空；
2. 所有 mutator 都完成了 `GC_PHASE_ENUM` / `MS_MARK_SETUP` transition；

这时 GC 线程应执行一轮全局 assist cleanup：

1. `EnumAssistContext.active = false`
2. `EnumAssistContext.remaining = 0`
3. `EnumAssistContext.epoch++` 或切到下一轮 epoch
4. `VisitAllMutators(...)`
   - `ClearSuspensionFlag(SUSPENSION_FOR_GC_ENUM_ASSIST)`
   - 若该 mutator 没有别的 suspend request，则 `SetSafepointActive(false)`

这样可以保证：

1. 即便某个 mutator 之后很久不再命中 safepoint，也不会一直带着过期 assist flag；
2. 下一轮 GC 不会误读上一轮 assist 残留状态；
3. safepoint gate 与 assist 生命周期严格对齐。

### 12.3.1 当前落地补充：先关新 claim，再等 in-flight helper 退出

在真正实现里，仅仅把：

1. `EnumAssistContext.active = false`
2. `remaining = 0`
3. `VisitAllMutators(...ClearSuspensionFlag...)`

连着做掉还不够。

原因是：

1. 某个 helper mutator 可能已经进入 `HandleEnumAssist()`；
2. 它已经从共享 context claim 了一批目标到自己的本地 batch；
3. 此时即使 GC 线程把全局 `active` 关掉，这个 helper 仍可能继续消费旧 batch；
4. 若 GC 主线程随后立刻推进到下一 phase，就会出现“helper 还在按上一 phase 的本地 batch 做 transition”的竞态。

所以当前更稳的收尾顺序应当是：

1. GC 线程先关闭 `EnumAssistContext.active`
2. 阻止新的 helper 再 claim 新 batch
3. 但**暂时不要认为 assist 已完全结束**
4. 等所有已经进入 `HandleEnumAssist()` 的 mutator 从本地 batch 退出
5. 再统一清理 `SUSPENSION_FOR_GC_ENUM_ASSIST`
6. 然后允许后续 phase 继续推进

实现上不需要把状态搬到 `MSAllocBuffer/P` 上；更小的做法是在 `Mutator` 上增加一个极轻量的：

```cpp
enumAssistDoing
```

进入 `HandleEnumAssist()` 置位，退出时清零；`FinalizeEnumAssistContext()` 在 close context 之后轮询等待所有 `doing == false` 即可。

### 12.4 为什么必须保留全局兜底

这个兜底很重要，因为当前方案本质上是：

> **让已经恢复业务的 mutator 在未来某次 safepoint 上 opportunistic 地回来帮忙。**

既然它是 opportunistic，就不能假设每个 mutator 一定会再回来一次。  
因此：

1. assist request 的清理不能只依赖 mutator 本地再次进入；
2. 必须由 GC 线程在 authoritative barrier 完成时做全局收尾。

### 12.5 推荐的实现顺序

建议清理流程按下面顺序实现：

1. mutator 本地先支持“过期 assist 自清理”
2. 再在 `EnsurePhaseTransition()` 结束点加全局强制清理
3. 最后再加 epoch 校验，防止跨轮复用

这样第一版即使还没有做很复杂的 claim state，也能先把 assist 生命周期管理清楚。

---

## 15. 第一版中的 helper/GC 线程撞车语义

这里单独说明一下第一版最容易被担心的问题：

> **如果 mutator helper 和 GC 线程同时盯上同一个 target mutator，会不会把同一个 mutator 的 root 扫描做两遍？**

第一版的答案是：

1. **有可能同时尝试同一个 target；**
2. **但正常不应该出现“同一个 target 完整完成两次 `GcPhaseEnum()`”的正确性问题；**
3. **主要代价是空转竞争，而不是重复完成。**

### 11.1 为什么会撞车

第一版里：

1. mutator helper 通过共享 `candidates` + `cursor` 选到一个 target；
2. GC 线程同时也在 `EnsurePhaseTransition()` 中遍历未完成 mutator；
3. 如果两边刚好都看到同一个 target 且它已经 `InSaferegion()`，就都可能去调用：

```cpp
target->TransitionGCPhase(false)
```

所以：

> **第一版允许“争抢同一个 target”的竞争尝试。**

### 11.2 为什么通常不会真的重复完成

关键在当前 `TransitionGCPhase()` 已经自带一次性 ownership 机制。

`Mutator::transitionState` 当前状态机是：

1. `NEED_TRANSITION`
2. `IN_TRANSITION`
3. `FINISH_TRANSITION`

`TransitionGCPhase(false)` 内部只有从：

```text
NEED_TRANSITION -> IN_TRANSITION
```

CAS 成功的线程，才会真正执行：

```cpp
TransitionToGCPhaseExclusive(...)
```

也就是才会真正走到：

- `HandleGCPhase(newPhase)`
- `GcPhaseEnum()`

如果另一个线程稍晚一步看到：

1. `IN_TRANSITION`
2. 或 `FINISH_TRANSITION`

它就不会再次完整执行一遍这次 phase transition。

所以：

> **第一版允许“同时看上同一个 target”，但当前 `transitionState` 机制仍会保证：正常只会有一个线程真正完成这次 target mutator 的 enum transition。**

### 11.3 第一版真正的代价是什么

第一版的主要问题不是 correctness，而是性能上的空转：

1. helper claim 到一个 target；
2. GC 线程也正好遍历到这个 target；
3. 两边都调用一次 `TransitionGCPhase(false)`；
4. 最终只有一个成功，另一个只是白跑了一次状态检查 / CAS 尝试。

因此第一版的代价主要是：

1. 原子状态竞争增加；
2. 部分 helper assist 时间花在无效尝试上；
3. assist 的理论收益会被冲淡。

### 11.4 为什么第一版仍然值得先做

因为第一版的目标本来就不是最优分工，而是：

1. 尽量少改动当前 `EnsurePhaseTransition()` 主收口逻辑；
2. 利用现有 `transitionState` 正确性保护，快速验证“mutator safepoint re-entry assist”是否有价值；
3. 先判断这条思路对 `MS_MARK_SETUP` 尾部长尾是否真的有收益。

所以第一版的定位应该是：

> **允许撞车，但依赖 `transitionState` 保证不会重复完成；先验证收益，再决定是否值得做更复杂的任务归属状态。**

### 11.5 第二版如果要继续优化，可以做什么

如果后续要继续减少 helper/GC 线程撞车，可以考虑：

1. 为 assist target 增加显式 claim state：
   - `UNCLAIMED`
   - `CLAIMED_BY_HELPER`
   - `DONE`
2. GC 线程在 `EnsurePhaseTransition()` 中感知 helper claim，降低与 helper 的热点重叠；
3. 或让 helper 与 GC 线程处理不同区间，降低竞争概率。

这些都属于“第二版提效项”，不是第一版闭环所必需。

---

## 16. 分阶段落地建议

### 阶段 1：最小闭环

1. 新增 `SUSPENSION_FOR_GC_ENUM_ASSIST`
2. 新增 `EnumAssistContext`
3. `GcPhaseEnum()` 完成后给 mutator 挂 assist 资格
4. `HandleSuspensionRequest()` 新增 `HandleEnumAssist()`
5. 单次 assist 固定批量，例如 `4`

目标：

- 先验证这套机制能否缩短 `EnsurePhaseTransition()` 的尾部长尾

### 阶段 2：更稳的候选筛选

1. 不再简单线性扫 `candidates`
2. 单独维护“已完成 / 未完成 / 已被检查过但暂不可代做”的状态
3. 降低空转扫描比例

### 阶段 3：与 Step32 组合

1. Step33 负责“mutator 自扫收口”
2. Step32 的 P-assisted snapshot 负责“`allCJThreadList` / parked coroutine roots 并行 visit”

两者结合后，`EnumRoots` 可以同时优化：

1. mutator root 扫描收口
2. coroutine root 串行 visit

---

## 17. 一句话总结

如果要把当前 `MS_MARK_SETUP` 的收口进一步并行化，最稳的增量路线不是反复复用 `SUSPENSION_FOR_GC_PHASE`，而是：

> **让每个 mutator 先正常完成一次自己的 `GcPhaseEnum()`；随后在未完全收口前，通过独立的 `SUSPENSION_FOR_GC_ENUM_ASSIST`，让它在后续 safepoint 上再回来帮 GC 线程完成一小批“已 saferegion 但尚未完成 transition”的 mutator 收口工作；最终 barrier 仍交给 `EnsurePhaseTransition()`。**
