## Step 2: 三色标记

> 设计文档: `doc/Step2_三色标记与Sweep详细设计.md`
>
> **目标**: 并发三色标记替换 CopyGC 的 Trace+Forward，四阶段 GC 周期。

### 新增文件

| 文件 | 行数 | 说明 |
|------|------|------|
| `src/Heap/Collector/MarkSweepCollector.h` | 187 | `MSGCCycleStats` + `MarkSweepCollector` 类声明 |
| `src/Heap/Collector/MarkSweepCollector.cpp` | 829 | GC 完整周期实现 |

### GC 完整周期

```
RunGarbageCollection(gcIndex, reason)
  → DoGarbageCollection(gcIndex)
      → MarkSetup()           [STW]     ← Step 2.1
      → ConcurrentMark()      [并发]    ← Step 2.2
      → MarkTermination()     [STW]     ← Step 2.3
      → PostMarkTermination() [非STW]   ← Step 2.4
      → ConcurrentSweep()     [并发]    ← Step 4
```

### MarkObject / IsMarkedObject 对齐校正（跨 Step 公共逻辑）

以下 3 处均使用 `GetRegionInfoValidated(addr)` 进行地址对齐校正:

| 函数 | 文件 | 行号 | 调用场景 |
|------|------|------|----------|
| `MarkSweepCollector::MarkObject()` | `MarkSweepCollector.cpp` | L203 | 并发标记期间 |
| `RegionSpace::MarkObjectMS()` (MarkObject override) | `RegionSpace.cpp` | L147 | Barrier 标灰时 |
| `RegionSpace::IsMarkedObjectMS()` | `RegionSpace.cpp` | L229 | Finalizer/弱引用判活 |

### Step 2.1: MarkSetup (Handshake, 非 STW)

> 设计文档: `doc/Step2.1_MarkSetup_STW详细设计.md`
>
> **注意**: 代码注释 (L92) 明确写为 `"Handshake, non-STW"`，日志 (L98) 遗留 `"(STW)"` 字样是历史残留。实际机制是 **Handshake**——GC 线程通过 `TransitionToGCPhase()` → `TransitionAllMutatorsToGCPhase()` 广播 phase 切换信号，每个 Mutator **在自己的 safepoint 独立转换**，不需要同时暂停所有线程。
>
> **目标**: 完成标记前的所有准备——确保上轮 sweep 完成、清零 markBitmap、刷回 TL Region、安装写屏障、扫描根集——然后进入并发标记。

#### 完整执行流程

```
MarkSetup() [GC 线程, Handshake]  (MarkSweepCollector.cpp L95)
  │
  ├─ ① ForceFinishSweep                    ← 确保上轮 sweep 完成
  │   └→ CentralRegionPool::ForceFinishSweep()
  │       └→ drain 所有 36 级 partialUnswept + fullUnswept
  │
  ├─ ② FlushAll TL Regions                 ← 归还线程本地 Region 缓存
  │   └→ AllocBufferMS::FlushAll()
  │       └→ 36 级 ReturnRegion → CentralRegionPool
  │
  ├─ ③ Clear markBitmaps                   ← 清零所有小对象 Region 的 markBitmap
  │   └→ CentralRegionPool::VisitAllRegions(visitor)
  │       └→ 每个 Region: memset(markBitmap, 0, wordCount * 8)
  │
  ├─ ④ Clear LOS marks                     ← 清零 Tier2 超大对象标记
  │   └→ LargeObjectSpace::ClearAllMarks()
  │       └→ 遍历链表: gcMark = 0
  │
  ├─ ④.5 Clear Tier1 large region marks    ← 清零 Tier1 堆内大对象标记
  │   └→ CentralRegionPool::ClearLargeMarks()
  │       └→ 遍历 largeRegionList_: isMarked = 0
  │
  ├─ ⑤ Reset markedObjectCount             ← 统计计数器归零
  │   └→ markedObjectCount.store(0)
  │
  ├─ ⑥ Handshake → GC_PHASE_MS_MARK_SETUP ← 安装写屏障 + Mutator 扫描栈根
  │   └→ TransitionToGCPhase(MS_MARK_SETUP, true)
  │       └→ MutatorManager::TransitionAllMutatorsToGCPhase()
  │           ├→ InstallBarrier(MS_MARK_SETUP) → currentBarrier = &msWriteBarrier
  │           ├→ SetGCPhase(MS_MARK_SETUP)
  │           └→ 广播 safepoint 信号, 等待每个 Mutator 到达 safepoint
  │               └→ 每个 Mutator 执行 GcPhaseEnum() 扫描自己的栈根
  │
  ├─ ⑦ EnumAndMarkRoots                    ← GC 线程汇总根集并标记
  │   └→ EnumAndMarkRoots(workStack, foreignStack)
  │       ├→ DoEnumeration() — 枚举静态根/协程根/Finalizer根/合并Mutator栈根
  │       └→ 遍历根集: MarkObject(obj) → TestAndSetMarkBit
  │           └→ 新标记的根推入 workStack
  │
  ├─ ⑧ Handshake → GC_PHASE_MS_CONCURRENT_MARK ← 进入并发标记阶段
  │   └→ TransitionToGCPhase(MS_CONCURRENT_MARK, true)
  │       └→ Barrier 保持 msWriteBarrier（不变）
  │
  ├─ ⑨ Merge foreignStack → workStack      ← 合并外部引用根
  │
  └─ ⑩ RunConcurrentMarkLoop(workStack)    ← 启动并发标记循环
       └→ (进入 Step 2.2 ConcurrentMark)
```

#### 函数详表

| 函数 | 文件:行号 | 作用 | 调用者 / 使用位置 |
|------|----------|------|-------------------|
| `MarkSetup()` | MarkSweepCollector.cpp L95 | MarkSetup 主入口，编排步骤①~⑩ | `DoGarbageCollection()` L84 |
| `CentralRegionPool::ForceFinishSweep()` | CentralRegionPool.cpp L206 | 强制清扫所有残余 unswept Region（drain partialUnswept + fullUnswept × 36 级），确保上轮 GC 的 sweep 彻底完成 | MarkSetup 步骤① |
| `AllocBufferMS::FlushAll()` | AllocBufferMS.cpp L172 | 归还所有 36 个 `activeRegions_[cls]` 到 CentralRegionPool 四链表。归还后 activeRegions_ 全为 nullptr | MarkSetup 步骤② |
| `CentralRegionPool::VisitAllRegions(visitor)` | CentralRegionPool.cpp | 遍历 36 级 × 4 链表中所有 Region，执行 visitor 回调（此处用于 memset 清零 markBitmap） | MarkSetup 步骤③ |
| `LargeObjectSpace::ClearAllMarks()` | LargeObjectSpace.cpp | 遍历 Tier2 超大对象链表，清除所有 `gcMark` 标志 | MarkSetup 步骤④ |
| `CentralRegionPool::ClearLargeMarks()` | CentralRegionPool.cpp | 遍历 Tier1 `largeRegionList_`，清除所有 `isMarked` 标志 | MarkSetup 步骤④.5 |
| `TransitionToGCPhase(phase, true)` | TracingCollector.h L298 | 转发到 `MutatorManager::TransitionAllMutatorsToGCPhase(phase)` | MarkSetup 步骤⑥⑧ |
| `MutatorManager::TransitionAllMutatorsToGCPhase()` | MutatorManager.cpp L476 | **Handshake 核心**：(1) `InstallBarrier(phase)` 安装屏障；(2) `SetGCPhase(phase)` 设全局 phase；(3) 广播 safepoint 信号给所有 Mutator；(4) 等待每个 Mutator 到达 safepoint 并执行 phase 回调 | TransitionToGCPhase 内部 |
| `Heap::InstallBarrier(phase)` | Heap.cpp L209 | 根据 phase 切换 barrier：`MS_MARK_SETUP/CONCURRENT_MARK/MARK_TERMINATION` → `msWriteBarrier`；`IDLE/MS_SWEEP` → `msIdleBarrier` | TransitionAllMutatorsToGCPhase 内部 |
| `Mutator::GcPhaseEnum(newPhase)` | Mutator.cpp L567 | Mutator 在 safepoint 扫描自己的栈根，推入 AllocBuffer（后续由 GC 线程 DoEnumeration 合并） | 每个 Mutator 到达 safepoint 时执行 |
| `EnumAndMarkRoots(workStack, foreignStack)` | MarkSweepCollector.cpp L245 | GC 线程汇总根集：(1) `DoEnumeration()` 枚举静态根/协程根/Finalizer 根/合并 Mutator 栈根；(2) 遍历根集调 `MarkObject()` 标记，新标记的推入 workStack | MarkSetup 步骤⑦ |
| `DoEnumeration(workStack, foreignStack)` | TracingCollector（继承） | 复用 CopyGC 的根枚举框架，收集所有 GC 根（静态根、栈根、外部引用根） | EnumAndMarkRoots 步骤 A |
| `MarkObject(obj)` | MarkSweepCollector.cpp L203 | 核心标记：addr → GetRegionInfoValidated → TestAndSetMarkBit。返回 true = 已标记（跳过），false = 新标记 | EnumAndMarkRoots 步骤 B |
| `RunConcurrentMarkLoop(workStack)` | MarkSweepCollector.cpp L300 | 并发标记主循环，drain workStack 直到收敛 | MarkSetup 步骤⑩ → 进入 Step 2.2 |

#### Mutator Safepoint 行为 (Mutator.cpp L566-573)

当 Mutator 在 safepoint 收到 `GC_PHASE_MS_MARK_SETUP` 信号时：

```cpp
else if (newPhase == GCPhase::GC_PHASE_MS_MARK_SETUP) {
    // 1. 扫描自己的栈根（复用 GcPhaseEnum）
    GcPhaseEnum(newPhase);
    // Note: TL Region flush 和 bitmap 清理由 GC 线程集中完成
}
```

| Mutator 动作 | 说明 |
|-------------|------|
| `GcPhaseEnum(newPhase)` | 扫描本线程栈帧中的引用，推入 per-Mutator 的 AllocBuffer，后续由 GC 线程 `DoEnumeration()` 合并 |
| 写屏障切换 | `InstallBarrier(MS_MARK_SETUP)` 已在 `TransitionAllMutatorsToGCPhase` 中全局执行，此后所有 Mutator 的写操作经过 `MSWriteBarrier`：shade(old) + shade(new) |
| mark-at-alloc 生效 | 进入 `MS_MARK_SETUP` phase 后，`MarkNewObject()` (L648) 对新分配对象调 `MarkObject()` 标黑，确保本轮 GC 存活 |

#### 阶段总结

| 维度 | 说明 |
|------|------|
| **同步机制** | Handshake（非 STW）——每个 Mutator 在自己的 safepoint 独立转换，不需全部同时暂停 |
| **Barrier 切换** | `msIdleBarrier` → `msWriteBarrier`（在步骤⑥ Handshake 中由 `InstallBarrier` 完成） |
| **产出** | ① 所有 markBitmap / isMarked / gcMark 已清零；② 写屏障已安装（后续写操作自动标灰）；③ 根集已枚举并标记到 workStack；④ markedObjectCount 归零 |
| **为后续步骤提供** | workStack 中包含所有已标记的 GC 根对象 → **Step 2.2 ConcurrentMark** 从 workStack 开始递归追踪标记所有可达对象 |
| **关键不变式** | MarkSetup 完成后：(1) 所有以前的标记信息已清除；(2) MSWriteBarrier 确保并发标记期间 Mutator 写操作不会丢失引用；(3) mark-at-alloc 确保标记期间新分配对象存活 |

### Step 2.2: ConcurrentMark (与 Mutator 并发)

> 设计文档: `doc/Step2.2_ConcurrentMark_详细设计.md`
>
> **目标**: GC 线程与 Mutator **并发**执行，从 workStack 中的根集出发，递归追踪标记所有可达对象。标记期间 Mutator 正常运行，写屏障（SATB）保证不丢失引用。

#### 完整执行流程

```
RunConcurrentMarkLoop(workStack)  [GC 线程, 与 Mutator 并发]
(MarkSweepCollector.cpp L300)
  │
  │  convergenceStableRounds_ = 0
  │
  ├─ while (!MarkConverged(workStack)):    ← 收敛条件: 连续 3 轮 workStack 为空
  │   │
  │   ├─ ① RunCollectorDrainSlice(workStack, 256)
  │   │   │  (L325, 每批最多处理 256 个灰色对象)
  │   │   │
  │   │   └─ while (!workStack.empty() && processed < 256):
  │   │       │
  │   │       ├─ obj = workStack.pop_back()
  │   │       │
  │   │       ├─ 跳过: obj==null || !IsHeapAddress(obj)
  │   │       │
  │   │       ├─ 无引用字段? → continue (叶子对象, 已标黑)
  │   │       │
  │   │       ├─ WeakRef 特殊处理:
  │   │       │   └→ 不标记 referent, 推入 WeakRefBuffer 延后处理
  │   │       │
  │   │       └─ ② TraceObjectRefFields(obj, workStack)  (L679)
  │   │           │  obj.ForEachRefField → TraceRefField
  │   │           │
  │   │           └─ ③ TraceRefField(obj, field, workStack)  (L360)
  │   │               │  target = field.GetTargetObject()
  │   │               │
  │   │               └─ ④ MarkObject(target)  (L203)
  │   │                   │  addr → MSRegionInfo → TestAndSetMarkBit
  │   │                   │  返回 true=已标记(跳过), false=新标记
  │   │                   │
  │   │                   └─ 新标记 → markedObjectCount++ → workStack.push_back(target)
  │   │
  │   └─ workStack.empty() ? convergenceStableRounds_++ : reset(0)
  │
  └─ 收敛完成 → 记录 cycleStats_.concurrentMarkUs
```

**并发期间 Mutator 侧同时发生**:
```
Mutator 写操作 → MSWriteBarrier (SATB 删除屏障)
  │
  ├─ shade(old): MarkAndEnqueue(oldRef, wbBuf)
  │   └→ 若 oldRef 未标记 → RegionSpace::MarkObject → wbBuf.Push
  │
  └─ store: field.SetTargetObject(newRef)
  
Mutator 分配对象 → MarkNewObject(obj)  (L648)
  └→ 标记期间新分配的对象直接标黑 (mark-at-alloc)
```

#### 函数详表

| 函数 | 文件:行号 | 作用 | 调用者 / 使用位置 |
|------|----------|------|-------------------|
| `ConcurrentMark()` | MarkSweepCollector.cpp L292 | Phase 边界标记函数（实际逻辑由 `RunConcurrentMarkLoop` 在 MarkSetup 末尾驱动） | `DoGarbageCollection()` L85 |
| `RunConcurrentMarkLoop(workStack)` | MarkSweepCollector.cpp L300 | 并发标记主循环：分片 drain workStack，检查收敛（连续 3 轮空栈）。记录耗时和标记对象数 | MarkSetup 步骤⑩ |
| `RunCollectorDrainSlice(workStack, max)` | MarkSweepCollector.cpp L325 | 单批次处理：从 workStack 弹出最多 `kSliceMaxObjects=256` 个灰色对象，逐个 Trace 子引用 | RunConcurrentMarkLoop 内部 |
| `TraceObjectRefFields(obj, workStack)` | MarkSweepCollector.cpp L679 | 遍历 obj 的所有引用字段，对每个字段调 `TraceRefField` | RunCollectorDrainSlice 内部 |
| `TraceRefField(obj, field, workStack)` | MarkSweepCollector.cpp L360 | 取 `field.GetTargetObject()` → `MarkObject(target)`，新标记则推入 workStack | TraceObjectRefFields 回调 |
| `MarkObject(obj)` | MarkSweepCollector.cpp L203 | **核心标记**：addr → `MSRegionInfoArray::GetRegionInfoValidated` → (大对象回溯到 base) → `TestAndSetMarkBit`。返回 true=已标记，false=新标记 | TraceRefField / EnumAndMarkRoots / MarkNewObject |
| `MarkConverged(workStack)` | MarkSweepCollector.cpp L375 | 收敛判断：`workStack.empty() && convergenceStableRounds_ >= 3` | RunConcurrentMarkLoop 循环条件 |
| `MarkNewObject(obj)` | MarkSweepCollector.cpp L648 | **mark-at-alloc**：标记期间（MARK_SETUP / CONCURRENT_MARK / MARK_TERMINATION）新分配的对象直接调 `MarkObject` 标黑 | 分配路径（AllocBufferMS::Allocate 末尾） |
| `MSWriteBarrier::WriteReference()` | MSWriteBarrier.cpp L96 | SATB 删除屏障：`shade(old)` + store。确保被覆盖的引用不丢失 | 每次 Mutator 写引用字段时触发 |
| `MSWriteBarrier::MarkAndEnqueue()` | MSWriteBarrier.cpp L26 | shade 操作核心：`RegionSpace::MarkObject(obj)` → 新标记则 `wbBuf.Push(obj)` | WriteReference / WriteStaticRef / Atomic 操作 |
| `WriteBarrierBuffer::Push(obj)` | WriteBarrierBuffer.h L56 | per-Mutator 缓冲区（256 条目），满时静默丢弃（对象已 mark，MarkTermination 会兜底扫描） | MarkAndEnqueue 内部 |
| `WeakRefBuffer::Insert(obj)` | WeakRefBuffer | WeakRef 对象的 referent 不在并发标记阶段追踪，推入缓冲区延后到 MarkTermination 处理 | RunCollectorDrainSlice 中 WeakRef 分支 |

#### MarkObject 实现细节 (L203-243)

```
MarkObject(obj):
  addr = reinterpret_cast<uintptr_t>(obj)
  msInfo = MSRegionInfoArray::GetRegionInfoValidated(addr)  ← 含对齐校正
  if (MS_REGION_LARGE):
    若 reserved(baseIdx) != 本条目索引 → 回溯到 base
  return msInfo->TestAndSetMarkBit(addr)
```

#### Mutator Safepoint 行为 (Mutator.cpp L580-583)

并发标记阶段 Mutator **不需要在 safepoint 执行额外操作**：

```cpp
else if (newPhase == GCPhase::GC_PHASE_MS_CONCURRENT_MARK) {
    // Transition to concurrent mark: no special Mutator action needed.
    // SATB write barrier is already installed by barrier switch.
}
```

| Mutator 动作 | 说明 |
|-------------|------|
| Safepoint 回调 | **无动作** — 写屏障已在 Step 2.1 的 Handshake 中安装完毕 |
| 写屏障 | `MSWriteBarrier`（SATB 删除屏障）：每次写引用 → `shade(old)` 标记旧值并推入 per-Mutator `WriteBarrierBuffer` |
| 新对象分配 | `MarkNewObject(obj)` 在 CONCURRENT_MARK phase 生效，新分配对象直接标黑 |
| WBBuf 满时 | 静默丢弃（对象已 mark）。GC 线程在 MarkTermination STW 阶段会 `FlushAllWriteBarrierBuffers` 兜底 |

#### 阶段总结

| 维度 | 说明 |
|------|------|
| **同步机制** | 完全并发 — GC 线程 drain workStack，Mutator 正常运行，通过 SATB 写屏障协调 |
| **关键常量** | `kSliceMaxObjects = 256`（每批处理上限）；`kConvergenceRequiredRounds = 3`（收敛需连续3轮空栈） |
| **产出** | 绝大多数可达对象已被标记（markBitmap 中 bit 置1 / 大对象 isMarked=1）；`markedObjectCount` 记录了标记总数 |
| **未处理** | ① Mutator 的 WriteBarrierBuffer 中可能还有未 drain 的灰色对象；② WeakRef 的 referent 未追踪（在 WeakRefBuffer 中暂存） |
| **为后续步骤提供** | → **Step 2.3 MarkTermination (STW)**: Flush 所有 WBBuf、drain 残余灰对象、处理 WeakRef/Finalizer 复活链，完成标记闭合 |
| **关键不变式** | (1) SATB 屏障保证：并发期间被覆盖的旧引用一定被 shade（不丢失可达白色对象）；(2) mark-at-alloc 保证：标记期间新分配的对象一定存活；(3) 收敛后 workStack 为空，但 WBBuf 中可能有残余 |

### Step 2.3: MarkTermination (STW)

> 设计文档: `doc/Step2.3_MarkTermination_STW详细设计.md`
>
> **目标**: **真正的 STW**（`ScopedStopTheWorld`）——暂停所有 Mutator，flush 写屏障缓冲区、收集外部引用和 Finalizer 复活候选、drain 残余灰色对象，完成标记闭合。

#### 完整执行流程

```
MarkTermination()  [STW, GC 线程独占]  (MarkSweepCollector.cpp L385)
  │
  ├─ ScopedStopTheWorld stw("MS MarkTermination", GC_PHASE_MS_MARK_TERMINATION)
  │   └→ 暂停所有 Mutator + InstallBarrier(MARK_TERMINATION) → msWriteBarrier 保持
  │
  ├─ mainQueue = NewWorkStack()
  │
  ├─ ① FlushAllWriteBarrierBuffers(mainQueue)        ← 收集所有 Mutator 的 WBBuf 残留
  │   └→ MutatorManager::VisitAllMutators → 每个 mutator.GetWriteBarrierBuffer().Flush(mainQueue)
  │   └→ 记录 cycleStats_.wbBufFlushUs / wbBufEntriesTotal
  │
  ├─ ② CollectExternCandidatesToMain(mainQueue)       ← 外部引用候选
  │   └→ 遍历 discoveredExternObjects: 未标记的推入 mainQueue
  │
  ├─ ③ CollectResurrectionCandidatesToMain(mainQueue)  ← Finalizer 复活
  │   └→ VisitFinalizerRoots: finalizerObj 未标记 → 推入 mainQueue（让其存活以便 Finalizer 执行）
  │
  ├─ ④ ExecuteLikeDrainMainQueue(mainQueue)            ← 标记所有残余灰色对象
  │   └→ while (!mainQueue.empty()):
  │       obj = mainQueue.pop_back()
  │       MarkObject(obj) → 新标记? → markedObjectCount++ → TraceObjectRefFields → 子引用入 mainQueue
  │       WeakRef 特殊: TraceObjectRefFields(referent) + WeakRefBuffer.Insert(obj)
  │
  ├─ 记录 cycleStats_.markTerminationUs / drainedGreyObjects
  │
  └─ ~ScopedStopTheWorld() → StartTheWorld()
      └→ Mutator 恢复运行（Barrier 仍为 msWriteBarrier，phase = MARK_TERMINATION）
```

#### 函数详表

| 函数 | 文件:行号 | 作用 | 调用者 / 使用位置 |
|------|----------|------|-------------------|
| `MarkTermination()` | MarkSweepCollector.cpp L385 | MarkTermination 主入口：STW → flush WBBuf → 收集候选 → drain → 统计 | `DoGarbageCollection()` L86 |
| `ScopedStopTheWorld` | (RAII 对象) L391 | 暂停所有 Mutator，析构时自动 `StartTheWorld()` | MarkTermination 开头 |
| `FlushAllWriteBarrierBuffers(mainQueue)` | MarkSweepCollector.cpp L445 | 遍历所有 Mutator，调 `mutator.GetWriteBarrierBuffer().Flush(mainQueue)`，将缓冲的灰色对象合并到 mainQueue | MarkTermination 步骤① |
| `CollectExternCandidatesToMain(mainQueue)` | MarkSweepCollector.cpp L464 | 遍历 `discoveredExternObjects` map，未标记的对象推入 mainQueue | MarkTermination 步骤② |
| `CollectResurrectionCandidatesToMain(mainQueue)` | MarkSweepCollector.cpp L475 | 通过 `VisitFinalizerRoots` 遍历 Finalizer 注册表，未标记的 finalizerObj 推入 mainQueue（让其复活以便 Finalizer 执行） | MarkTermination 步骤③ |
| `ExecuteLikeDrainMainQueue(mainQueue)` | MarkSweepCollector.cpp L485 | 与并发标记相同的 drain 循环：`pop → MarkObject → TraceObjectRefFields → push 子引用`，直到 mainQueue 清空 | MarkTermination 步骤④ |
| `CollectUnmarkedToMainQueue(mainQueue)` | MarkSweepCollector.cpp L433 | 便捷封装：依次调 FlushAllWBBuf + CollectExtern + CollectResurrection | 未在 MarkTermination 直接使用（独立可用） |
| `MarkObject(obj)` | MarkSweepCollector.cpp L203 | 核心标记（复用 Step 2.2 的实现） | ExecuteLikeDrainMainQueue 内部 |
| `WeakRefBuffer::Insert(obj)` | WeakRefBuffer | MarkTermination 的 drain 中遇到 WeakRef → 追踪 referent 但将 WeakRef 本身推入缓冲区，延后到 PostMark 处理 | ExecuteLikeDrainMainQueue 中 WeakRef 分支 |

#### Mutator Safepoint 行为 (Mutator.cpp L575-578)

MarkTermination 是 **真 STW**（`ScopedStopTheWorld`），所有 Mutator 被冻结，**没有 safepoint 回调**：

```cpp
else if (newPhase == GCPhase::GC_PHASE_MS_MARK_TERMINATION) {
    // MarkTermination STW: flush write barrier buffer so GC thread can drain all grey objects.
    // The GC thread will call FlushAllWriteBarrierBuffers() to collect them.
    // No action needed here — flush is done centrally by the collector under STW.
}
```

| Mutator 动作 | 说明 |
|-------------|------|
| Safepoint 回调 | **无动作** — STW 由 `ScopedStopTheWorld` 冻结所有线程，WBBuf flush 由 GC 线程集中完成 |
| 写屏障 | Barrier 保持 `msWriteBarrier`（但 Mutator 已暂停，不会触发） |
| mark-at-alloc | `MarkNewObject` 在 MARK_TERMINATION phase 仍生效（但 Mutator 已暂停） |

#### 阶段总结

| 维度 | 说明 |
|------|------|
| **同步机制** | STW（`ScopedStopTheWorld`）— 所有 Mutator 暂停，GC 线程独占执行 |
| **产出** | ① 所有灰色对象已完全标记（workStack + WBBuf 全部 drain 完毕）；② Finalizer 复活链已标记存活；③ 外部引用候选已处理；④ WeakRef 推入 WeakRefBuffer 待处理 |
| **与 CopyGC 差异** | 无 SATB buffer 处理（替换为 WBBuf flush）；无 Forward/CAS 操作；复活链补标后无需 re-scan（SATB 屏障已保证完整性） |
| **为后续步骤提供** | → **Step 2.4 PostMarkTermination**: 弱引用清理（WeakRefBuffer）、Finalizer 入队、大对象 Eager Sweep、切换到 sweep phase |
| **关键不变式** | MarkTermination 完成后：(1) 所有可达对象（含 Finalizer 复活链）均已标记；(2) 标记阶段正式结束——后续不再有新的标记操作 |

### Step 2.4: PostMarkTermination (Handshake, 非 STW)

> 设计文档: `doc/Step2.4_PostMarkTermination_详细设计.md`
>
> **目标**: 标记结束后的清理——切换到 sweep phase（卸载标记屏障）、弱引用清理、WBBuf 清空、Finalizer 入队、大对象 Eager Sweep、OHOS 循环引用处理。

#### 完整执行流程

```
PostMarkTermination()  [GC 线程, 非 STW]  (MarkSweepCollector.cpp L526)
  │
  ├─ ① TransitionToGCPhase(GC_PHASE_MS_SWEEP, true)  ← Handshake 切换到 Sweep phase
  │   └→ InstallBarrier(MS_SWEEP) → currentBarrier = &msIdleBarrier
  │   └→ 每个 Mutator 在 safepoint 确认 phase 切换（无额外动作）
  │
  ├─ ② WeakRefBuffer::ClearWeakRefBuffer()           ← 弱引用清理
  │   └→ referent 未标记 → 置 null（弱引用失效）
  │   └→ 记录 weakRefUs
  │
  ├─ ③ ClearAllWriteBarrierBuffers()                  ← 清空所有 Mutator 的 WBBuf
  │   └→ MutatorManager::VisitAllMutators → 每个 mutator.GetWriteBarrierBuffer().Clear()
  │
  ├─ ④ ProcessFinalizers()                            ← Finalizer 入队
  │   └→ FinalizerProcessor::EnqueueFinalizables(predicate: !IsMarkedObject)
  │   └→ fp.Notify() — 唤醒 Finalizer 线程执行析构
  │   └→ 记录 snapshotFinalizerNum
  │
  ├─ ⑤ PrepareCycleRef()                              ← OHOS 循环引用准备（条件编译）
  │   └→ cycleRefWorkStack.insert(discoveredExternObjects)
  │   └→ discoveredExternObjects.clear()
  │
  ├─ ⑥ PostResolveCycleTask()                         ← OHOS 循环引用解析（仅 __OHOS__）
  │   └→ CJ_MRT_RolveCycleRef()
  │
  ├─ ⑦ CollectLargeGarbage()                          ← 大对象 Eager Sweep
  │   ├→ Tier2: LargeObjectSpace::SweepUnmarkedObjectsWithStats()
  │   │   └→ 遍历 mmap 链表，释放未标记的超大对象（>4MB）
  │   └→ Tier1: CentralRegionPool::SweepLargeRegions()
  │       └→ 遍历 largeRegionList_，释放未标记的大 Region
  │   └→ 记录 losFreedCount / losFreedBytes
  │
  └─ 记录 cycleStats_.postMarkUs
```

#### 函数详表

| 函数 | 文件:行号 | 作用 | 调用者 / 使用位置 |
|------|----------|------|-------------------|
| `PostMarkTermination()` | MarkSweepCollector.cpp L526 | PostMark 主入口：编排 7 项清理任务 | `DoGarbageCollection()` L87 |
| `TransitionToGCPhase(MS_SWEEP, true)` | TracingCollector.h L298 | Handshake 切换到 Sweep phase：`InstallBarrier(MS_SWEEP)` → `msIdleBarrier`（卸载标记屏障），全局 phase 切到 MS_SWEEP | PostMark 步骤① |
| `WeakRefBuffer::ClearWeakRefBuffer()` | WeakRefBuffer | 遍历 WeakRefBuffer 中暂存的 WeakRef 对象：referent 未标记 → 置 null（弱引用失效） | PostMark 步骤② |
| `ClearAllWriteBarrierBuffers()` | MarkSweepCollector.cpp L456 | 遍历所有 Mutator，调 `GetWriteBarrierBuffer().Clear()`。标记已结束，WBBuf 不再需要 | PostMark 步骤③ |
| `ProcessFinalizers()` | MarkSweepCollector.cpp L576 | `FinalizerProcessor::EnqueueFinalizables(!IsMarkedObject)` — 收集未标记的 Finalizer 对象入队 + `fp.Notify()` 唤醒 Finalizer 线程 | PostMark 步骤④ |
| `PrepareCycleRef()` | MarkSweepCollector.cpp L586 | 将 `discoveredExternObjects` 合并到 `cycleRefWorkStack`，清空 discovered（OHOS 跨语言循环引用准备） | PostMark 步骤⑤ |
| `PostResolveCycleTask()` | MarkSweepCollector.cpp L593 | 仅 `__OHOS__`：调 `CJ_MRT_RolveCycleRef()` 通知宿主侧解析循环引用 | PostMark 步骤⑥ |
| `CollectLargeGarbage()` | MarkSweepCollector.cpp L603 | Eager Sweep 大对象：Tier2 `LargeObjectSpace::SweepUnmarkedObjectsWithStats()` + Tier1 `CentralRegionPool::SweepLargeRegions()` | PostMark 步骤⑦ |
| `LargeObjectSpace::SweepUnmarkedObjectsWithStats()` | LargeObjectSpace.cpp | 遍历 mmap 超大对象链表，`munmap` 释放未标记对象，记录 freedCount/freedBytes | CollectLargeGarbage Tier2 |
| `CentralRegionPool::SweepLargeRegions()` | CentralRegionPool.cpp | 遍历 `largeRegionList_`，回收未标记的大 Region（归还底层页），返回释放字节数 | CollectLargeGarbage Tier1 |

#### Mutator Safepoint 行为 (Mutator.cpp L583-586)

PostMarkTermination 通过 Handshake 切换到 `GC_PHASE_MS_SWEEP`，Mutator **无额外动作**：

```cpp
else if (newPhase == GCPhase::GC_PHASE_MS_SWEEP) {
    // Transition to sweep: no special Mutator action needed.
    // Barrier switched to idle by InstallBarrier.
}
```

| Mutator 动作 | 说明 |
|-------------|------|
| Safepoint 回调 | **无动作** — 仅确认 phase 切换 |
| 写屏障切换 | `InstallBarrier(MS_SWEEP)` → `msIdleBarrier`（零开销直通读写，不再 shade） |
| mark-at-alloc | `MarkNewObject` 在 MS_SWEEP phase **不生效**（新对象不再标记） |
| 分配路径 | 进入 sweep phase 后，分配时按需触发 lazy sweep（`SweepAndClassify`） |

#### 阶段总结

| 维度 | 说明 |
|------|------|
| **同步机制** | Handshake（非 STW）— 切换到 sweep phase；后续操作由 GC 线程串行执行 |
| **产出** | ① 弱引用已清理（失效的 referent 置 null）；② WBBuf 已清空；③ Finalizer 已入队并唤醒 Finalizer 线程；④ 大对象（Tier1+Tier2）已 Eager Sweep 释放 |
| **执行顺序约束** | `CollectLargeGarbage` **必须在 `ProcessFinalizers` 之后** — 避免回收有 Finalizer 的大对象（它们在步骤④已被标记为需要析构） |
| **为后续步骤提供** | → **Step 2.5 ConcurrentSweep**: 小对象的 lazy sweep 已通过 `PrepareForSweep()` 的 generation flip 准备就绪，分配时按需清扫 |
| **关键不变式** | PostMarkTermination 完成后：(1) 标记屏障已卸载（msIdleBarrier）；(2) 大对象内存已同步回收；(3) 小对象 sweep 进入 lazy 模式；(4) GC 周期即将结束 |

---