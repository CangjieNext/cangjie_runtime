# MS-GC Step2 Mark 阶段实现复核与优化

> 复核范围：`doc/Step2_三色标记与Sweep详细设计.md` 及其子文档 `Step2.1` ~ `Step2.4`
> 对照代码：`src/Heap/Collector/MarkSweepCollector.*`、`Mutator.cpp`、`AllocBufferMS.cpp`、`TracingCollector.cpp`
> 结论时间：2026-03-19

---

## 1. 先说结论

当前 MS 的 mark 阶段主流程已经完整落地，`MarkSweepCollector::DoGarbageCollection()` 的 Phase 顺序与 Step2 设计一致：

```text
MarkSetup -> ConcurrentMark -> MarkTermination -> PostMarkTermination
```

但是，实现形态已经明显偏离 Step2 文档中的早期设计，主要有三点：

1. **Step2.1 不是纯 Handshake 方案**，而是“`ForceFinishSweep` + 短 STW flush/clear + phase 切换 + GcPhaseEnum root merge”的混合方案。
2. **Step2.2 的并发标记主增量来源不是 SATB 队列**，而是 `WriteBarrierBuffer + globalWorkQueue + Mutator assist`。
3. **Step2.3 / Step2.4 的阶段边界已经重划**：`MS_SWEEP` phase 切换发生在 MarkTermination 的 STW 内，而不是等 PostMarkTermination 结束后再切。

所以，当前最合适的理解方式不是“Step2 文档即实现说明”，而是：

- Step2 文档描述的是 **设计意图**
- `MarkSweepCollector` 代码体现的是 **当前工程化落地版本**

这两者在大框架上一致，但在 barrier 语义、STW 边界和并发队列来源上已经发生了实质偏移。

---

## 2. 当前代码中的 Mark 阶段总览

### 2.1 主流程

当前入口在 `MarkSweepCollector::DoGarbageCollection()`：

```text
Step 2.1  MarkSetup
Step 2.2  ConcurrentMark
Step 2.3  MarkTermination
Step 2.4  PostMarkTermination
Step 3/4  ConcurrentSweep
```

这部分和 Step2 主文档的阶段拆分是一致的。

### 2.2 当前真实的数据流

更接近当前实现的数据流如下：

```text
MarkSetup
  -> ForceFinishSweep
  -> STW: FlushAllAllocBuffers + ClearMarkBitmaps + ClearLargeMarks
  -> STW 内切到 MS_MARK_SETUP
  -> Mutator safepoint 里执行 GcPhaseEnum
  -> GC 线程 DoEnumeration + Mark roots

ConcurrentMark
  -> 切到 MS_CONCURRENT_MARK
  -> GC 线程本地 markWorkStack 推进
  -> 局部 work 与 globalWorkQueue 共享
  -> Mutator assist 从 globalWorkQueue 抽活
  -> 写屏障溢出对象进入 WriteBarrierBuffer / globalWorkQueue

MarkTermination
  -> STW
  -> FlushAllWriteBarrierBuffers(mainQueue)
  -> drain globalWorkQueue
  -> extern / resurrection candidates 入 mainQueue
  -> ExecuteLikeDrainMainQueue 收尾
  -> STW 内切到 MS_SWEEP
  -> ClearAllWriteBarrierBuffers

PostMarkTermination
  -> ClearWeakRefBuffer
  -> ProcessFinalizers
  -> PrepareCycleRef / PostResolveCycleTask
  -> CollectLargeGarbage
```

这说明当前实现已经不是“SATB buffer 驱动尾收敛”的主模型，而更接近：

- 根扫描：`DoEnumeration + GcPhaseEnum`
- 并发增量：`WriteBarrierBuffer`
- 并发推进：`GC thread + Mutator assist`
- 收尾：`WB flush + drain mainQueue`

---

## 3. Step2.1 ~ Step2.4 对照复核

### 3.1 Step2.1 MarkSetup

#### 已实现部分

1. 上一轮 sweep 完成保障已经落地。
   - 代码中不是抽象的 `EnsureSweepDone()`，而是直接调用 `CentralRegionPool::WaitForSweepDone()` + `ForceFinishSweep()`。

2. 根枚举框架复用了现有 `DoEnumeration()` / `GcPhaseEnum()` 路径。
   - 这一点和设计一致。

3. 新对象在 mark 相关 phase 中会被标黑。
   - `MarkSweepCollector::MarkNewObject()` 与 `AllocBufferMS::Allocate()` 上的 mark-at-alloc 都体现了这个目标。

#### 与文档不一致的地方

1. **文档强调“无 STW 的 Handshake 方案”**，但当前代码实际引入了一个明确的短 STW。
   - STW 内完成 `FlushAllAllocBuffers`、`VisitAllRegions` 清 bitmap、LOS/Tier1 大对象 mark 清理，以及 `TransitionToGCPhase(MS_MARK_SETUP)`。

2. 文档把 TL Region bitmap 清理和 flush 放在 Mutator 的 `HandleGCPhase(MS_MARK_SETUP)` 内完成。
   - 当前代码没有这么做。
   - `Mutator::HandleGCPhase(GC_PHASE_MS_MARK_SETUP)` 只做 `GcPhaseEnum(newPhase)`。

3. 文档里的“两阶段清 bitmap”模型已不成立。
   - 当前实现是“STW 下先 flush 全部 alloc buffer，再由 GC 线程遍历 central pool 统一清理”。

#### 当前实现的优点

1. 工程上更稳妥，避免了 TLS 型 `AllocBufferMS` 与 per-Mutator handshake 不匹配的问题。
2. 通过 STW 把 `FlushAllAllocBuffers + ClearMarkBitmaps + phase 切换` 合成一个原子窗口，规避了 stale mark bits 和 barrier phase 编码窗口问题。

#### 当前实现的代价

1. Step2.1 已不再是文档宣称的“近乎 0 暂停”的 handshake-only 方案。
2. flush 全部 alloc buffer 使每轮 mark 前都要付出一次小型集中暂停。

### 3.2 Step2.2 ConcurrentMark

#### 已实现部分

1. 并发标记主循环已经存在。
2. 存在 `GC thread + Mutator assist` 的协同推进模型。
3. 有全局 work queue 与本地 workStack 的共享机制。
4. 有简单的收敛判定：本地空 + 全局空，连续 N 轮稳定后退出。

#### 与文档不一致的地方

1. 文档强调 SATB 删除屏障与 `SatbBuffer` 作为并发增量来源。
   - 当前 MS 代码的核心增量来源已经换成了 `WriteBarrierBuffer`。
   - `MarkTermination` 里收尾的也是 `FlushAllWriteBarrierBuffers()`，而不是 `MarkSatbBuffer()`。

2. 文档中的 `ControlAssistAndBackpressure()`、assist ratio 动态调参、窗口统计等控制逻辑没有实现。
   - 当前 assist 是固定预算、固定阈值的 MVP 版本。

3. 文档有较强的 “assist 主导” 表述。
   - 当前实现更准确地说是“GC 线程主循环 + assist 补充吞吐”。

#### 当前实现的优点

1. 结构简单，容易调试。
2. 避免直接复用 `TracingCollector::MarkSatbBuffer()` 那套 CopyGC phase 语义。
3. `globalWorkQueue_ + ShareWorkToGlobal + PopFromGlobal` 的闭环比较清晰。

#### 当前实现的短板

1. assist/backpressure 仍然比较粗糙，没有 backlog、alloc pressure、队列时延的联动控制。
2. convergence 只看“空队列 + 连续轮次”，没有更细粒度的 in-flight work 统计。
3. `WeakRef` 在 assist 路径上被直接回灌全局队列，说明这条路径仍偏保守。

### 3.3 Step2.3 MarkTermination

#### 已实现部分

1. STW 收尾窗口已经存在。
2. 会统一收集并 drain 剩余灰对象。
3. 会处理 extern / resurrection candidates。
4. 最终确实通过 Execute-like 循环完成尾收敛。

#### 与文档不一致的地方

1. 文档主语义还是 SATB 收敛。
   - 当前代码主语义已经切到 **WB buffer + globalWorkQueue** 收敛。

2. 文档里 `CollectUnmarkedToMainQueue()` 还保留了 `CollectSatbCandidatesToMain()`。
   - 当前实现没有这一步。

3. 文档把 phase 切到 `MS_SWEEP` 放在逻辑结束处描述，但没有强调必须在 STW 内完成。
   - 当前代码明确把 `TransitionToGCPhase(GC_PHASE_MS_SWEEP, false)` 放在 STW 内，原因是避免 mutator 在 MarkTermination 与 PostMarkTermination 之间继续触发旧屏障语义。

#### 当前实现的优点

1. STW 边界收得很紧，语义上更安全。
2. 明确把 `activeCollector_` 清空、phase 切换和清 WB buffer 都放在 STW 中，消除了 phase gap。

#### 当前实现的短板

1. Step2 文档没有及时反映“SATB 收尾已经让位给 WB 收尾”，读者容易误判当前一致性模型。
2. `CollectUnmarkedToMainQueue()` 这个接口名还保留旧设计味道，但里面不再做 SATB 收集，命名有滞后。

### 3.4 Step2.4 PostMarkTermination

#### 已实现部分

1. `ClearWeakRefBuffer()` 已做。
2. `ProcessFinalizers()` 已做。
3. `PrepareCycleRef()` / `PostResolveCycleTask()` 已做。
4. `CollectLargeGarbage()` 已做。

#### 与文档不一致的地方

1. 文档里还保留了 `ClearSatbBuffer()` 作为任务项。
   - 当前 `PostMarkTermination()` 并没有显式做 SATB 清理。
   - 这与前面“MS 当前主收敛路径已转成 WriteBarrierBuffer”是一致的。

2. 文档把 Step2.4 描述为“屏障已切换到 Sweep 模式后的一般清理阶段”。
   - 当前代码确实如此，但关键点是：**phase 切换发生在 Step2.3 STW 内，而不是 Step2.4 中**。

#### 当前实现的优点

1. 与当前 barrier 生命周期一致，没有把 phase 切换拖到非 STW 阶段。
2. LOS eager collect 和 weakref/finalizer 清理都在一个明确的 post-mark 窗口内。

---

## 4. 一张表看清：设计 vs 当前实现

| 阶段 | Step2 文档主张 | 当前代码实现 | 结论 |
|------|----------------|--------------|------|
| Step2.1 | Handshake-only，无 STW | `ForceFinishSweep` + 短 STW flush/clear + handshake enum | **已落地，但实现已偏离原设计** |
| Step2.2 | SATB + assist 主导并发标记 | GC 线程本地栈 + `globalWorkQueue_` + `WriteBarrierBuffer` + assist | **已落地，但增量来源已变化** |
| Step2.3 | STW 内收 SATB / finalizer / weakref 尾巴 | STW 内 flush WB buffers + drain global queue + resurrection/extern | **已落地，但收敛机制已变化** |
| Step2.4 | 弱引用、SATB、Finalizer、CycleRef、LOS 清理 | 弱引用、Finalizer、CycleRef、LOS 清理；无显式 SATB clear | **大体落地，任务边界已重划** |

---

## 5. 当前版本的主要优化点

下面的优化点按优先级排序，不是按实现难度排序。

### O1. 先把 Step2 文档和当前代码口径统一

这是优先级最高的一项。

当前最大的成本不是代码功能缺失，而是**设计文档和实现语义已经分叉**，典型表现包括：

1. 2.1 还在强调 handshake-only，但代码已有短 STW。
2. 2.2/2.3 还在用 SATB 作为主收敛叙事，但代码已经转成 WB buffer 主模型。
3. 2.4 还保留 `ClearSatbBuffer()` 任务项，但代码没有对应实现。

建议：

1. 明确把 Step2 文档标成“设计稿”或“当前实现复核版”。
2. 单独补一张“当前代码路径图”，避免读者继续按旧设计推导问题。

### O2. 给 ConcurrentMark 增加更强的运行时统计

当前并发标记已有基础统计，但还不够支撑调优：

1. 缺少 `globalWorkQueue_` 高水位、平均积压、空转轮次统计。
2. 缺少 WB overflow 次数、每轮 flush entry 数分布。
3. 缺少 assist 与 GC 线程的实际吞吐对比。

建议增加：

1. `global queue peak size`
2. `assist pop / push / marked / weakref requeue`
3. `gc-thread drained objects / round`
4. `convergence stable rounds` 命中次数和总轮次

没有这些统计，后面想判断 assist 是否过弱、WB overflow 是否过多，会一直停留在经验判断层面。

### O3. 明确 barrier 语义，减少“SATB”和“WB buffer”双重叙事

当前代码里仍然同时保留了大量 SATB 历史语汇，但 MS 主路径已经更偏向 write-barrier grey queue。

这会带来两个问题：

1. 读代码时很难一眼看出当前真正主收敛来源。
2. 后续如果还复用 `TracingCollector` 某些接口，容易误把 CopyGC 的 SATB 生命周期带回 MS 路径。

建议：

1. 在 `MarkSweepCollector` 代码注释和文档里，明确写“MS 当前以 WriteBarrierBuffer 为主增量来源”。
2. 把 `CollectUnmarkedToMainQueue` 这类偏旧设计命名改成更贴近实际语义的名字。

### O4. 优化 Step2.1 的 STW 成本，而不是执着回到 handshake-only

从当前实现看，短 STW 的引入是有明确工程理由的，简单回退到 handshake-only 不是稳妥方向。

更现实的优化方向应是：

1. 统计 `FlushAllAllocBuffers()` 的耗时占比。
2. 统计 `VisitAllRegions` 清 bitmap 的耗时占比。
3. 评估能否把“无 bitmap 的空 region”跳过清理。
4. 评估是否可以只清本轮可见 region，而不是无差别全量遍历。

也就是说，当前更值得做的是 **缩短这个 STW 窗口**，而不是重新把整套方案改回纯 handshake。

### O5. 加强 MarkTermination 的 in-flight work 收敛判定

现在的 MarkTermination 已经做了很多正确的收尾工作，但收敛判定仍然偏经验式：

1. ConcurrentMark 只看 local/global queue 是否空。
2. 没有显式统计 in-flight assist 正在处理但尚未回吐的对象数量。

建议：

1. 引入 assist in-flight 计数，收敛时一起纳入判断。
2. 让 MarkTermination 日志输出“进入 STW 前最后一轮 global/local queue 规模”。

这能显著降低“为什么这轮 STW 收尾偏重”的诊断成本。

### O6. 把 Step2.4 的任务边界写死在代码注释里

当前 `PostMarkTermination()` 做的是对的，但文档口径已经漂移。

建议直接在代码注释中固定这件事：

1. phase 已在 MarkTermination STW 内切到 `MS_SWEEP`
2. Step2.4 不再承担 barrier 卸载责任
3. Step2.4 不再承担 SATB 主收尾责任

这样后面重构 Step2 文档时，会更容易以代码为准回收旧叙事。

---

## 6. 建议的后续动作

### P0：先做文档收口

1. 给 Step2 主文档加“当前实现复核摘要”。
2. 把 2.1 到 2.4 中明显过时的 SATB / handshake-only 表述标出来。

### P1：再做统计增强

1. 并发 mark 队列统计
2. WB overflow 统计
3. Step2.1 短 STW 分项耗时

### P2：最后做行为优化

1. 减少 Step2.1 的 bitmap clear 范围
2. 优化 assist/backpressure
3. 增强 convergence 判定

---

## 7. 最终结论

Step2 的 2.1 到 2.4 在当前代码里已经不是“未实现的设计”，而是“**基本已实现，但实现路线已经工程化重写**”。

最关键的判断是：

1. **主流程已落地**。
2. **Step2.1 采用了短 STW + handshake 的混合方案**。
3. **Step2.2/2.3 当前主要依赖 WriteBarrierBuffer，而不是文档里强调的 SATB 主路径**。
4. **当前最该优化的不是盲目补功能，而是统一设计文档口径、补并发标记统计、缩短 Step2.1 的 STW 窗口。**