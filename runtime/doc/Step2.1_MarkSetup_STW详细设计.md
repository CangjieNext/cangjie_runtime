# Step 2.1：Phase 1 — Mark Setup（Handshake）详细设计

> 本文档从 [Step 2 主文档](Step2_三色标记与Sweep详细设计.md) §4 拆分而来，详细描述 Mark-Sweep GC 的第一阶段。
> **核心变更**：从传统 STW 方案升级为 **Handshake（握手）** 方案——复用现有 `TransitionToGCPhase` 机制，每个 Mutator 在到达 safepoint 时自行完成转换动作，无需全局暂停。

---

## 目录

- [Step 2.1：Phase 1 — Mark Setup（Handshake）详细设计](#step-21phase-1--mark-setuphandshake详细设计)
  - [目录](#目录)
  - [1. 目标](#1-目标)
  - [2. 伪代码](#2-伪代码)
  - [3. Handshake 机制详解](#3-handshake-机制详解)
    - [3.1 TransitionToGCPhase 内部流程](#31-transitiontogcphase-内部流程)
    - [3.2 Mutator::HandleGCPhase 新增分支](#32-mutatorhandlegcphase-新增分支)
    - [3.3 Mutator::GcPhaseEnum — 栈根自扫描（直接复用）](#33-mutatorgcphaseenum--栈根自扫描直接复用)
    - [3.4 与 STW 方案的对比](#34-与-stw-方案的对比)
  - [4. ClearAllMarkBitmaps — 两阶段实现](#4-clearallmarkbitmaps--两阶段实现)
    - [4.1 阶段 ①：Handshake 前 — 清除中央池 Bitmap](#41-阶段-handshake-前--清除中央池-bitmap)
    - [4.2 阶段 ②：Handshake 中 — Mutator 清除自己的 TL Bitmap](#42-阶段-handshake-中--mutator-清除自己的-tl-bitmap)
    - [4.3 安全性论证](#43-安全性论证)
  - [5. 确保上一轮 Sweep 完成](#5-确保上一轮-sweep-完成)
    - [5.1 两阶段策略](#51-两阶段策略)
    - [5.2 Go 的做法](#52-go-的做法)
    - [5.3 为什么在 Handshake 之前](#53-为什么在-handshake-之前)
    - [5.4 实现](#54-实现)
  - [6. 全类型 Region 回收（Mutator 自行 Flush）](#6-全类型-region-回收mutator-自行-flush)
    - [6.1 各层 Region 状态](#61-各层-region-状态)
    - [6.2 小对象 TL Region（Mutator 自行 flush）](#62-小对象-tl-regionmutator-自行-flush)
    - [6.3 Tier 1 / Tier 2 大对象（无需 flush）](#63-tier-1--tier-2-大对象无需-flush)
    - [6.4 sweepGen 与惰性 Sweep](#64-sweepgen-与惰性-sweep)
      - [为什么不用 SnapshotSweepCandidates](#为什么不用-snapshotsweepcandidates)
      - [sweepGen 机制](#sweepgen-机制)
      - [惰性 Sweep：Mutator 取 Region 时按需 sweep](#惰性-sweepmutator-取-region-时按需-sweep)
      - [后台 Sweep 线程（减少 Mutator 分配延迟）](#后台-sweep-线程减少-mutator-分配延迟)
  - [7. EnumAndMarkRoots — 根枚举与标记](#7-enumandmarkroots--根枚举与标记)
    - [7.1 两阶段根枚举](#71-两阶段根枚举)
    - [7.2 复用判定](#72-复用判定)
    - [7.3 MS Override 的 virtual 函数](#73-ms-override-的-virtual-函数)
    - [7.4 GC 线程侧：EnumAndMarkRoots 实现](#74-gc-线程侧enumandmarkroots-实现)
  - [8. 安全性分析](#8-安全性分析)
    - [8.1 窗口 A：Mutator A 已转换，Mutator B 未转换](#81-窗口-amutator-a-已转换mutator-b-未转换)
    - [8.2 窗口 B：中央池 Bitmap 已清除，TL Bitmap 未清除](#82-窗口-b中央池-bitmap-已清除tl-bitmap-未清除)
    - [8.3 窗口 C：并发标记期间 Region 被 Mutator 取走](#83-窗口-c并发标记期间-region-被-mutator-取走)
    - [8.4 总结：Handshake + sweepGen 安全保证](#84-总结handshake--sweepgen-安全保证)
  - [9. 与现有 WCollector ENUM 阶段的对比](#9-与现有-wcollector-enum-阶段的对比)
  - [10. 延迟预估](#10-延迟预估)
    - [Handshake 模型 vs STW 模型](#handshake-模型-vs-stw-模型)
    - [单 Mutator 转换时间拆解](#单-mutator-转换时间拆解)
    - [对应用的影响](#对应用的影响)
    - [潜在风险](#潜在风险)

---

## 1. 目标

**无 STW** 地完成标记初始化，通过 Handshake 机制让每个 Mutator 自行完成转换：

1. 清空上一轮标记位（所有对象变白色）
2. 安装写屏障（SATB 删除屏障）
3. 每个 Mutator 自行 flush TL Region + 扫描栈根
4. GC 线程枚举全局根，将根引用的对象标灰
5. 启动并发标记阶段

**关键洞察**：现有 CopyGC 的 ENUM 阶段本身就不是 STW，而是通过 `TransitionToGCPhase(GC_PHASE_ENUM)` handshake 实现。MS-GC 完全复用这套机制，只需注册新的 `GC_PHASE_MS_MARK_SETUP` 分支。

## 2. 伪代码

```cpp
void MarkSweepCollector::MarkSetup() {
    // ▶ Handshake 之前：确保上一轮 sweep 完成（§5）
    EnsureSweepDone();
    
    // ▶ Handshake 之前：清除中央池所有 Region 的标记位（§4 阶段①）
    //   此时 TL Region 还在线程私有，不受影响
    //   中央池 Region 无人在分配，安全清除
    ClearCentralPoolMarkBitmaps();
    
    // ▶ Handshake（§3）
    //   内部执行顺序（复用 TransitionAllMutatorsToGCPhase 已有机制）：
    //   ┌─ GC 线程 ─────────────────────────────────────────────┐
    //   │ 1. InstallBarrier(MS_MARK_SETUP) → 安装 msMarkBarrier │
    //   │ 2. SetGCPhase(GC_PHASE_MS_MARK_SETUP)                │
    //   │ 3. 广播 safepoint 信号给所有 Mutator                  │
    //   │ 4. EnsurePhaseTransition — 等待所有 Mutator 完成转换   │
    //   └───────────────────────────────────────────────────────┘
    //   ┌─ 每个 Mutator 到达 safepoint 时 ─────────────────────┐
    //   │ 1. ClearTLRegionBitmap()  — 清除自己 TL Region bitmap │ ← §4 阶段②
    //   │ 2. FlushTLRegion()        — 归还中央池                │ ← §6
    //   │ 3. GcPhaseEnum()          — 扫描自己的栈根 → AllocBuffer│ ← §7
    //   └───────────────────────────────────────────────────────┘
    //   Mutator 恢复执行 → SATB 屏障已激活，MarkNewObject 生效
    TransitionToGCPhase(GCPhase::GC_PHASE_MS_MARK_SETUP, true);
    
    // ▶ Handshake 之后：所有 TL Region 已 flush，全部可见
    
    // 1. 递增 sweepGen（§6.4）
    //    sweepGen 是 Region 级别的代际标记，替代集中式快照列表
    //    旧 Region 的 sweepGen < globalSweepGen_ → 需要本轮 sweep
    //    新分配的 Region 的 sweepGen == globalSweepGen_ → 跳过
    globalSweepGen_.fetch_add(2, std::memory_order_release);
    
    // 2. 合并 Mutator 栈根 + 枚举全局根 + 设置 mark bit（§7）
    EnumAndMarkRoots(workStack_, foreignStack_);
    
    // 3. 递增全局 GC epoch（详见主文档 §5.9）
    globalGCEpoch_.fetch_add(2, std::memory_order_release);
    
    SetPhase(MS_PHASE_MARK);
    // → 进入 ConcurrentMark 阶段
}
```

**执行时序总览**：

```
GC 线程                          Mutator A              Mutator B
  │                                │                      │
  ├─ EnsureSweepDone()             │ (running)            │ (running)
  ├─ ClearCentralPoolBitmaps()     │                      │
  ├─ TransitionToGCPhase ─────────►│                      │
  │   InstallBarrier               │                      │
  │   SetGCPhase                   │                      │
  │   Broadcast safepoint          │                      │
  │   ┌────────────────────────────┤                      │
  │   │  (等待 A)                  ├─ safepoint:          │
  │   │                            │  ClearTLBitmap()     │
  │   │                            │  FlushTLRegion()     │
  │   │                            │  GcPhaseEnum()       │
  │   │                            │  ─── 恢复执行 ──►    │
  │   │  (等待 B)                  │                      ├─ safepoint:
  │   │                            │                      │  ClearTLBitmap()
  │   │                            │                      │  FlushTLRegion()
  │   │                            │                      │  GcPhaseEnum()
  │   │                            │                      │  ─── 恢复执行 ──►
  │   └── 全部完成 ◄───────────────┤                      │
  ├─ globalSweepGen_ += 2         │ (分配新对象,         │ (分配新对象,
  ├─ EnumAndMarkRoots()            │  SATB 屏障捕获,      │  MarkNewObject 标黑)
  ├─ gcEpoch += 2                  │  MarkNewObject 标黑) │
  ├─ SetPhase(MARK)                │                      │
  │                                │                      │
  ▼ ConcurrentMark ...             ▼                      ▼
```

## 3. Handshake 机制详解

### 3.1 TransitionToGCPhase 内部流程

现有 `TransitionToGCPhase` 定义在 `TracingCollector.h:298`：

```cpp
void TransitionToGCPhase(const GCPhase phase, const bool) {
    MutatorManager::Instance().TransitionAllMutatorsToGCPhase(phase);
}
```

`TransitionAllMutatorsToGCPhase`（`MutatorManager.cpp:476`）执行以下步骤：

```
1. AcquireMutatorManagementWLock()     // 冻结 Mutator 列表
2. InstallBarrier(phase)               // 安装对应写屏障（全局生效）
3. SetGCPhase(phase)                   // 设置全局 GC phase
4. 遍历所有 Mutator：
   - SetSuspensionFlag(SUSPENSION_FOR_GC_PHASE)
   - SetSafepointActive(true)          // 触发 safepoint 检查
5. EnsurePhaseTransition(phase)        // 等待所有 Mutator 完成转换
6. MutatorManagementWUnlock()
```

**关键特性**：这不是 STW——Mutator 不会被集体暂停。每个 Mutator 在自己到达 safepoint 时**独立执行转换**，完成后立即恢复运行。GC 线程只是等待所有 Mutator 都完成。

### 3.2 Mutator::HandleGCPhase 新增分支

每个 Mutator 到达 safepoint 时调用 `HandleGCPhase(newPhase)`。现有实现（`Mutator.cpp:546`）已有 `GC_PHASE_ENUM`、`GC_PHASE_PREFORWARD` 等分支。

MS-GC 需要**新增两个分支**：

```cpp
inline void Mutator::HandleGCPhase(GCPhase newPhase)
{
    // ... 现有分支 ...
    
    // ── MS-GC 新增 ──
    else if (newPhase == GCPhase::GC_PHASE_MS_MARK_SETUP) {
        // 1. 清除自己 TL Region 的 bitmap
        ClearTLRegionBitmap();
        // 2. 将 TL Region 归还中央池
        FlushTLRegion();
        // 3. 复用现有栈根自扫描（与 CopyGC ENUM 完全相同）
        GcPhaseEnum(newPhase);
    }
    else if (newPhase == GCPhase::GC_PHASE_MS_MARK_TERMINATION) {
        // drain 本地 SATB buffer
        std::lock_guard<std::mutex> lg(mutatorLock);
        if (satbNode != nullptr) {
            SatbBuffer::Instance().RetireNode(satbNode);
            satbNode = nullptr;
        }
    }
}
```

### 3.3 Mutator::GcPhaseEnum — 栈根自扫描（直接复用）

`GcPhaseEnum()` 是现有代码（`Mutator.cpp:459`），CopyGC 的 `GC_PHASE_ENUM` 阶段使用：

```cpp
inline void Mutator::GcPhaseEnum(GCPhase newPhase) {
    RootVisitor visitor = [this](ObjectRef& root) {
        BaseObject* obj = root.object;
        if (Heap::IsHeapAddress(obj)) {
            AllocBuffer::GetOrCreateAllocBuffer()->PushRoot(obj);
            // 放入 thread-local AllocBuffer，后续 GC 线程 MergeMutatorRoots 合并
        }
    };
    VisitMutatorRoots(visitor);  // 遍历当前线程的栈帧
}
```

**核心要点**：
- 没有 tag/CAS/fix-ref 操作——纯粹的"扫描栈，push 到 thread-local buffer"
- MS-GC **完全复用**这个函数，零改动
- 每个 Mutator 扫描自己的栈，无需锁，无竞争

### 3.4 与 STW 方案的对比

| 维度 | 旧方案（STW） | 新方案（Handshake） |
|------|-------------|-------------------|
| Mutator 暂停 | 全部暂停，集中处理 | **不暂停**，各自到 safepoint 转换 |
| 栈根扫描 | GC 线程遍历所有 Mutator 栈 | **Mutator 自扫描**（`GcPhaseEnum`） |
| TL Region flush | GC 线程遍历所有 AllocBuffer | **Mutator 自行 flush** |
| Bitmap 清除 | STW 内一次性清除 | **两阶段**：handshake 前清中央池，handshake 中 mutator 清 TL |
| 屏障安装 | STW 内 `InstallBarrier` | `TransitionToGCPhase` 内部自动安装 |
| 最大暂停时间 | ~1ms（所有工作累加） | **≈ 0**（每个 Mutator 只暂停自己的转换时间） |
| 代码复用 | 需新写 STW 逻辑 | **直接复用** `TransitionToGCPhase` + `GcPhaseEnum` |

## 4. ClearAllMarkBitmaps — 两阶段实现

由于不再使用 STW，bitmap 清除必须分两阶段执行，避免与 Mutator 分配产生竞态。

### 4.1 阶段 ①：Handshake 前 — 清除中央池 Bitmap

在 `TransitionToGCPhase` 之前，GC 线程清除**所有中央池管辖的 Region** bitmap：

```cpp
void MarkSweepCollector::ClearCentralPoolMarkBitmaps() {
    auto& regionManager = heapManager_->GetRegionManager();
    
    // ── 小对象 Region（中央池中的）：清除 markBitmap ──
    // 此时 TL Region 仍在 Mutator 私有，不在此遍历范围内
    regionManager.ForEachRegion([](RegionInfo* region) {
        if (region->HasMarkBitmap()) {
            region->GetLiveInfo()->ClearMarkBitmap();
        }
    });
    
    // ── Tier 1 大对象 Region（16KB~4MB）：清除 isMarked 标志 ──
    largeRegionList_->ForEach([](RegionInfo* region) {
        region->GetMetadata().isMarked = false;
    });
    
    // ── Tier 2 超大对象（> 4MB）：清除 gcMark 标志 ──
    largeObjectSpace_->ClearAllMarks();
}
```

**安全性**：此时 Mutator 仍在运行，但它们只在自己的 TL Region 上分配，不会触碰中央池 Region 的 bitmap。中央池 Region 的 bitmap 是上一轮标记的残留，本轮还未使用，清除安全。

### 4.2 阶段 ②：Handshake 中 — Mutator 清除自己的 TL Bitmap

每个 Mutator 在 `HandleGCPhase(MS_MARK_SETUP)` 中清除自己持有的 TL Region bitmap：

```cpp
void Mutator::ClearTLRegionBitmap() {
    AllocBuffer* buffer = AllocBuffer::GetOrCreateAllocBuffer();
    RegionInfo* tlRegion = buffer->GetTLRegion();
    if (tlRegion != nullptr && tlRegion->HasMarkBitmap()) {
        tlRegion->GetLiveInfo()->ClearMarkBitmap();
    }
    // preparedRegion 同理
    RegionInfo* prepared = buffer->GetPreparedRegion();
    if (prepared != nullptr && prepared->HasMarkBitmap()) {
        prepared->GetLiveInfo()->ClearMarkBitmap();
    }
}
```

**时序**：Mutator 先清 bitmap，然后 flush TL Region（§6），最后扫描栈根。确保 flush 到中央池的 Region bitmap 已经被清过。

### 4.3 安全性论证

| 时间窗口 | bitmap 状态 | 是否安全 |
|---------|------------|--------|
| 阶段①之后、handshake 之前 | 中央池 bitmap 已清；TL bitmap 未清 | ✅ Mutator 只在 TL Region 分配，不读中央池 bitmap |
| Mutator A 转换后、B 未转换 | A 的 TL bitmap 已清并 flush；B 的 TL bitmap 未清 | ✅ B 还在用自己的 TL Region（bitmap 对 B 无影响，B 在 bump-pointer 分配不读 bitmap） |
| A 恢复运行、从中央池取新 TL Region | 新 TL Region 的 bitmap 在阶段①已清 | ✅ 安全 |
| 全部转换完成后 | 所有 bitmap 已清：中央池（阶段①）+ 原 TL（阶段②已 flush 到中央池） | ✅ 完整 |

## 5. 确保上一轮 Sweep 完成

### 5.1 两阶段策略

不是"等待"或"帮助"二选一，而是**两者都做**：

| 阶段 | 动作 | 位置 | 说明 |
|------|------|------|------|
| ① 帮助 sweep | 当前线程主动窃取并 sweep 剩余 Region | **Handshake 之前** | 减少残留工作量 |
| ② 等待确认 | 等待后台 sweep 线程也结束 | **Handshake 之前** | 保证 FreeList / bitmap 状态一致 |

### 5.2 Go 的做法

Go 在 `gcStart()` 中调用 `finishsweep_m()`（在 STW 之前）：
```go
func gcStart(trigger gcTrigger) {
    systemstack(func() {
        finishsweep_m()  // 主动 sweep 所有剩余 mspan
    })
    // ... 然后才进入标记阶段
}
```
`finishsweep_m()` 循环调用 `sweepone()`，每次窃取一个未 sweep 的 mspan 并执行 sweep，直到全部完成。

### 5.3 为什么在 Handshake 之前

| 如果在 Handshake 后做 | 如果在 Handshake 前做 |
|----------------------|---------------------|
| ClearBitmaps 会清掉上轮残留 bitmap → sweep 失去引用依据 | ✅ sweep 完成后 bitmap 可安全清除 |
| Mutator 可能在未 sweep 的 Region 上分配（FreeList 未建立） | ✅ FreeList 已就绪 |

### 5.4 实现

```cpp
void MarkSweepCollector::EnsureSweepDone() {
    // 快速路径：上一轮 sweep 已完成
    if (sweepDone_.load(std::memory_order_acquire)) {
        return;
    }
    
    // ---- 阶段 ①：当前线程主动帮助 sweep ----
    while (true) {
        size_t idx = sweepIndex_.fetch_add(1, std::memory_order_relaxed);
        if (idx >= sweepCandidates_.size()) break;
        
        RegionInfo* region = sweepCandidates_[idx];
        if (region->IsLargeRegion()) {
            SweepLargeRegion(region);
        } else {
            SweepNormalRegion(region);
        }
    }
    
    // ---- 阶段 ②：等待后台 sweep 线程也完成 ----
    std::unique_lock<std::mutex> lock(sweepMutex_);
    sweepDoneCV_.wait(lock, [this]() {
        return sweepDone_.load(std::memory_order_acquire);
    });
}
```

> **注**：[Step 4 并发清扫详细设计](Step4_并发清扫详细设计.md) §6.3 中有完整版实现（含 per-SizeClass 队列、大对象队列的多路窃取），此处为简化版。

## 6. 全类型 Region 回收（Mutator 自行 Flush）

### 6.1 各层 Region 状态

| 层级 | 对象大小 | 线程本地缓存 | MarkSetup 前的状态 | 需要的操作 |
|------|---------|-------------|-------------------|----------|
| 小对象 | ≤ 16KB | ✅ AllocBuffer 持有 tlRegion | Region 在线程私有 | **Mutator 自行 flush**（handshake 中） |
| Tier 1 | 16KB~4MB | ❌ 无（直接 FreeRegionManager） | 已在 LargeRegionList 中 | 无需 flush |
| Tier 2 | > 4MB | ❌ 无（独立 mmap） | 已在 LargeObjectSpace 中 | 无需 flush |

**关键变更**：旧方案由 GC 线程在 STW 内统一遍历所有 AllocBuffer 并 flush。新方案改为**每个 Mutator 在 safepoint 转换时自行 flush 自己的 TL Region**。

### 6.2 小对象 TL Region（Mutator 自行 flush）

每个 Mutator 在 `HandleGCPhase(MS_MARK_SETUP)` 中执行：

```cpp
void Mutator::FlushTLRegion() {
    AllocBuffer* buffer = AllocBuffer::GetOrCreateAllocBuffer();
    
    RegionInfo* tlRegion = buffer->GetTLRegion();
    if (tlRegion != nullptr) {
        // 1. 设置 Region 的水印线（当前 bump-pointer 位置）
        //    水印线之后的区域是未分配的，Sweep 可加入 FreeList
        tlRegion->SetAllocWatermark(buffer->GetAllocPosition());
        
        // 2. 归还中央池
        centralPool_->ReturnPartialRegion(tlRegion);
        
        // 3. 清空 AllocBuffer 引用
        buffer->SetTLRegion(nullptr);
        buffer->ResetAllocPosition();
    }
    
    // 同样处理 preparedRegion
    RegionInfo* prepared = buffer->GetPreparedRegion();
    if (prepared != nullptr) {
        centralPool_->ReturnPartialRegion(prepared);
        buffer->SetPreparedRegion(nullptr);
    }
}
```

**必须 flush 的两个原因**：

| # | 原因 | 详细说明 |
|---|------|----------|
| 1 | **Bitmap 清除完整性** | 两阶段 ClearBitmaps 中，阶段①只清中央池，阶段②由 Mutator 清自己 TL 后立即 flush。flush 能保证归还中央池的 Region bitmap 已被清除。 |
| 2 | **标记期间一致性** | flush 后 Mutator 重新从中央池取 Region 时，会触发惰性 sweep（§6.4）。如果不 flush，Mutator 继续在旧 TL Region 上分配，sweep 和分配会竞态操作同一 Region。 |

**Go 的对应机制**：Go 在 `gcStart()` 中调用 `forEachP(func(pp *p) { pp.mcache.prepareForSweep(sweepgen) })`，将每个 P 的 mcache 中的 mspan 全部 flush 回 mcentral/mheap。

### 6.3 Tier 1 / Tier 2 大对象（无需 flush）

根据 [Step1.3](Step1.3_大对象分配与堆增长设计.md) §2.5.1 和 §5.2 的设计：

- **Tier 1**（16KB~4MB）：分配时直接从 FreeRegionManager 取连续 Unit，立即加入 `LargeRegionList`
- **Tier 2**（> 4MB）：独立 mmap，立即注册到 `LargeObjectSpace.objectMap_`

两者都**分配时已进入中央跟踪结构**，不存在“线程私有不可见”的问题。handshake 保证没有正在执行中的大对象分配（Mutator 已到 safepoint）。

后续步骤的覆盖：

| 后续步骤 | Tier 1 | Tier 2 |
|---------|--------|--------|
| ClearBitmaps（§4 阶段①） | `LargeRegionList.ForEach()` 清 `isMarked` | `LargeObjectSpace.ClearAllMarks()` |
| sweepGen 递增 | 全局 `globalSweepGen_ += 2`，影响所有 Region | 同左 |
| 根枚举 & 标记 | `MarkObject()` 设置 `isMarked` | `LargeObjectSpace.MarkObject()` |

### 6.4 sweepGen 与惰性 Sweep

#### 为什么不用 SnapshotSweepCandidates

早期设计中，handshake 完成后调用 `SnapshotSweepCandidates()` 拍摊所有 Region 列表作为 sweep 范围。但这存在一个问题：

```
MarkSetup:
  SnapshotSweepCandidates() → 快照了 Region #42（在中央池）

并发标记期间:
  Mutator 从中央池取走 Region #42 当 TL → 通过 FreeList 分配新对象
  ...标记结束...

Sweep 阶段:
  遍历快照 → 扫到 #42 → ❗ 但 #42 正在被 Mutator 当 TL 使用！
  Sweep 想回收未标记 slot → Mutator 同时在 FreeList 分配 → 竞态！
```

解决方案：用 **sweepGen（代际标记）** 替代集中式快照列表，配合**惰性 sweep**。

#### sweepGen 机制

每个 Region 携带一个 `sweepGen` 字段，全局维护一个 `globalSweepGen_`：

```cpp
// Region 上的 sweepGen 状态含义：
//   region->sweepGen == globalSweepGen_       → 已 sweep / 本轮新分配，无需处理
//   region->sweepGen <  globalSweepGen_       → 需要 sweep
//
// MarkSetup 中递增：
globalSweepGen_.fetch_add(2, std::memory_order_release);
// 此后所有旧 Region 的 sweepGen 自动“过期”，新分配的 Region 带当前 sweepGen
```

对标 Go 的 `mheap.sweepgen`：

```go
// Go runtime: mgc.go
func gcStart(trigger gcTrigger) {
    // ...
    atomic.Store(&sweep.active, 0)  // 重置 sweep 状态
    // mheap.sweepgen += 2  在 gcSweep 中执行
}

// mspan.sweepgen 含义：
// == sweepgen - 2 : 需要本轮 sweep
// == sweepgen - 1 : 正在被 sweep
// == sweepgen     : 已 sweep / 新分配
```

#### 惰性 Sweep：Mutator 取 Region 时按需 sweep

Mutator 从中央池取 Region 时，检查并按需 sweep：

```cpp
RegionInfo* CentralPool::AcquireRegion(SizeClass sc) {
    RegionInfo* region = TakeRegionFromFreeList(sc);
    
    // 惰性 sweep：如果这个 Region 还没被本轮 sweep → 先 sweep
    if (region->sweepGen < globalSweepGen_.load(std::memory_order_acquire)) {
        SweepRegion(region);                        // 遍历 bitmap，回收未标记 slot
        region->sweepGen = globalSweepGen_.load();   // 标记已 sweep
    }
    
    return region;  // 返回已 sweep 的 Region，FreeList 已更新
}
```

**这解决了“被取走的 Region 怎么 sweep”的问题**：谁取谁 sweep。

####  后台 Sweep 线程（减少 Mutator 分配延迟）

惰性 sweep 可能增加 Mutator 的分配延迟。因此同时运行后台 sweep 线程，主动遍历中央池中的 Region 进行 sweep：

```cpp
void MarkSweepCollector::BackgroundSweep() {
    auto& regionManager = heapManager_->GetRegionManager();
    uint64_t currentGen = globalSweepGen_.load(std::memory_order_acquire);
    
    regionManager.ForEachRegion([currentGen](RegionInfo* region) {
        // 只 sweep 未被任何线程持有为 TL 的 Region
        if (region->sweepGen < currentGen && !region->IsThreadLocal()) {
            SweepRegion(region);
            region->sweepGen = currentGen;
        }
        // 被 TL 持有的 Region → 跳过，等 Mutator 释放后自然被惰性 sweep
    });
}
```

| 路径 | 触发时机 | 处理的 Region |
|------|---------|------------|
| **惰性 sweep** | Mutator 取 Region 时 | 任何 Region（包括曾被 TL 持有的） |
| **后台 sweep** | Mark 结束后并发运行 | 中央池中未被 TL 持有的 Region |
| **EnsureSweepDone** | 下一轮 GC 开始前（§5） | 所有残余未 sweep 的 Region |

## 7. EnumAndMarkRoots — 根枚举与标记

### 7.1 两阶段根枚举

MS-GC 的根枚举分两部分完成：

| 阶段 | 执行者 | 做什么 | 复用现有代码 |
|------|--------|--------|------------|
| **Handshake 中** | 每个 Mutator | `GcPhaseEnum()` — 扫描自己的栈帧，将根对象 push 到 AllocBuffer | ✅ 直接复用`Mutator::GcPhaseEnum` |
| **Handshake 后** | GC 线程 | `DoEnumeration()` — 并行枚举静态根、协程根、Finalizer 根；`MergeMutatorRoots()` 合并栈根 | ✅ 直接复用 `TracingCollector::DoEnumeration` |

调用链：

```
Handshake 中（每个 Mutator）:
  HandleGCPhase(MS_MARK_SETUP)
    → ClearTLRegionBitmap()           // §4 阶段②
    → FlushTLRegion()                 // §6
    → GcPhaseEnum()                   // 扫描栈根 → AllocBuffer::stackRoots

Handshake 后（GC 线程）:
  EnumAndMarkRoots(workStack, foreignStack)
    → DoEnumeration(workStack, foreignStack)      // TracingCollector.cpp:307
    │   ├─ EnumAllCommonRoots(threadPool, workStack)  // TracingCollector.cpp:747
    │   │   ├─ [并行] EnumStaticRoots()             // → virtual EnumRefFieldRoot
    │   │   ├─ [并行] EnumConcurrencyModelRoots()   // → virtual EnumAndTagRawRoot
    │   │   ├─ [并行] EnumFinalizerProcessorRoots() // → virtual EnumAndTagRawRoot
    │   │   ├─ [并行] EnumAllSurrectedExportRoots()
    │   │   ├─ MergeMutatorRoots(workStack)   // 合并各 Mutator 的 AllocBuffer::stackRoots
    │   │   └─ 合并 rootSets → workStack
    │   └─ EnumAllExportRoots(foreignStack)
    → MarkRoots(workStack)                         // 遍历 workStack 设置 mark bit
    → MarkRoots(foreignStack)
```

### 7.2 复用判定

| 组件 | 复用方式 | 说明 |
|------|---------|------|
| `DoEnumeration()` | ✅ **直接复用** | 纯调度逻辑，无 GC 算法依赖 |
| `EnumAllCommonRoots()` | ✅ **直接复用** | 并行派发 + 合并 |
| `MergeMutatorRoots()` | ✅ **直接复用** | 从 AllocBuffer 收集栈根，纯数据搬运 |
| `EnumAllExportRoots()` | ✅ **直接复用** | 委托 `EnumAndTagRawRoot` |
| `EnumStaticRoots()` | ✅ **直接复用** | 委托 `EnumRefFieldRoot` |
| **`EnumRefFieldRoot()`** (virtual) | 🔧 **已 Override** | MS 版去除 tag/CAS |
| **`EnumAndTagRawRoot()`** (virtual) | 🔧 **需 Override** | MS 版去除 tag/CAS |
| `GcPhaseEnum()` | ✅ **直接复用** | Mutator 自扫描栈根，纯 push，无 tag |

### 7.3 MS Override 的 virtual 函数

已实现的 `EnumRefFieldRoot`（[MarkSweepCollector.cpp](../src/Heap/Collector/MarkSweepCollector.cpp#L67)）：

```cpp
// WCollector 版本：~30 行（tag 检查 + CAS + FindLatestVersion）
// MS 版本：~3 行
void MarkSweepCollector::EnumRefFieldRoot(RefField<>& ref, RootSet& rootSet) const
{
    BaseObject* target = ref.GetTargetObject();
    if (target != nullptr) {
        rootSet.push_back(target);
    }
}
```

需要新增的 `EnumAndTagRawRoot`：

```cpp
// WCollector 版本：~20 行（tag 检查 + CAS + IsCurrentPointer/IsOldPointer）
// MS 版本：~5 行
void MarkSweepCollector::EnumAndTagRawRoot(ObjectRef& ref, RootSet& rootSet) const
{
    BaseObject* root = reinterpret_cast<RefField<>&>(ref).GetTargetObject();
    if (Heap::IsHeapAddress(root)) {
        rootSet.push_back(root);
    }
}
```

### 7.4 GC 线程侧：EnumAndMarkRoots 实现

```cpp
void MarkSweepCollector::EnumAndMarkRoots(WorkStack& workStack, WorkStack& foreignStack) {
    // ── Step A: 根枚举（直接复用 TracingCollector::DoEnumeration）──
    //    内部并行枚举：静态根、协程根、Finalizer 根、复活导出根、栈根
    //    栈根已由各 Mutator 在 handshake 时自行扫描到 AllocBuffer
    //    MergeMutatorRoots 合并到 workStack
    DoEnumeration(workStack, foreignStack);
    
    // ── Step B: 将根集对象标灰 + 入 workStack ──
    WorkStack markedRoots = NewWorkStack();
    while (!workStack.empty()) {
        BaseObject* obj = workStack.back();
        workStack.pop_back();
        if (MarkObject(obj)) {            // 设置 bitmap 位 / isMarked / gcMark
            markedRoots.push_back(obj);    // 标记成功 → 灰色，入队待扫描
        }
    }
    workStack = std::move(markedRoots);
    
    // foreignStack 中的 Export 根也需标记
    WorkStack markedForeign = NewWorkStack();
    while (!foreignStack.empty()) {
        BaseObject* obj = foreignStack.back();
        foreignStack.pop_back();
        if (MarkObject(obj)) {
            markedForeign.push_back(obj);
        }
    }
    foreignStack = std::move(markedForeign);
}
```

**关键差异**：WCollector 通过 tag 机制“隐式标记”根对象。MS 没有 tag，需要在枚举完成后**显式遍历根集设置 mark bit**（Step B）。

**`MarkNewObject` 自动生效**：无需显式 `EnableMarkNewObject()`。当 Mutator 转换到 `GC_PHASE_MS_MARK_SETUP` 后，`MarkNewObject` 基于 `mutatorPhase` 检查自动激活：

```cpp
void MarkSweepCollector::MarkNewObject(BaseObject* obj) {
    GCPhase phase = Mutator::GetMutator()->GetMutatorPhase();
    if (phase == GC_PHASE_MS_MARK_SETUP ||
        phase == GC_PHASE_MS_CONCURRENT_MARK ||
        phase == GC_PHASE_MS_MARK_TERMINATION) {
        MarkObject(obj);
    }
}
```

## 8. 安全性分析

Handshake 模型与 STW 的核心差异：Mutator 不是同时停止的，而是**逐个转换**。转换期间存在三个时间窗口需要分析安全性。

### 8.1 窗口 A：Mutator A 已转换，Mutator B 未转换

```
GC 线程: 广播 handshake
        ├─ Mutator A: 已到 safepoint，执行 FlushTL + GcPhaseEnum → 恢复运行（带 Mark barrier）
        └─ Mutator B: 尚未到 safepoint，仍在执行用户代码（带旧 barrier）
```

| 场景 | 是否安全 | 分析 |
|------|---------|------|
| A 分配新对象 | ✅ | A 已进入 MS_MARK_SETUP，`MarkNewObject` 自动激活，新对象标黑 |
| A 覆写引用 | ✅ | A 已安装 `msMarkBarrier`，SATB 屏障捕获旧值 |
| B 分配新对象 | ✅ | B 尚未转换，`MarkNewObject` 不激活。但 B 的 TL Region bitmap 未被清除（还没执行 handshake），不影响正确性。B 转换后会对栈进行根枚举，所有可达对象会被枚举 |
| B 覆写引用 | ✅ | B 尚未转换，旧 barrier 无 SATB。但 B 最终会到 safepoint 进行栈根枚举，相当于“快照”了 B 的所有栈根。对于 B 覆写后丢弃的引用，如果该引用不在任何其他根中，将被正确回收（它们确实不可达） |
| A 将引用传给 B | ✅ | A 写入 B 的对象字段时，A 的 SATB barrier 捕获 B 字段的旧值。新值（A 传递的引用）已在 A 的栈根中被枚举或被 A 的 barrier 保护 |

### 8.2 窗口 B：中央池 Bitmap 已清除，TL Bitmap 未清除

两阶段清除的中间状态：

```
GC 线程: ClearCentralPoolMarkBitmaps() 完成
         TransitionToGCPhase(MS_MARK_SETUP) 开始…
         ├─ Mutator A: 已转换，TL bitmap 已清除 + flush 回中央池
         └─ Mutator B: 未转换，TL bitmap 仍有旧 mark bit
```

| 场景 | 是否安全 | 分析 |
|------|---------|------|
| 中央池 Region 的 bitmap 已清零 | ✅ | 中央池 Region 无 Mutator 并发读写 bitmap（没有 TL 绑定） |
| B 的 TL Region 上有旧 mark bit | ✅ | B 转换时会先清除自己的 TL bitmap，然后 flush。旧 mark bit 不会泄漏到新周期 |
| A flush 的 Region 已在中央池，bitmap 已清 | ✅ | A 在转换时先 clearBitmap 再 flush，顺序保证 |

### 8.3 窗口 C：并发标记期间 Region 被 Mutator 取走

```
标记期间:
  中央池有 Region #42（sweepGen < globalSweepGen_，尚未 sweep）
  Mutator 从中央池取 Region #42 当 TL
  → CentralPool::AcquireRegion 触发惰性 sweep
  → SweepRegion(#42)：遍历 bitmap，回收未标记 slot，更新 FreeList
  → region->sweepGen = globalSweepGen_
  → Mutator 开始在已 sweep 的 Region #42 上分配
```

| 场景 | 是否安全 | 分析 |
|------|---------|------|
| Mutator 取旧 Region 分配 | ✅ | 惰性 sweep 先执行，FreeList 已更新，不会分配到未回收的死对象 slot |
| Mutator 在已取走的 Region 上分配新对象 | ✅ | MarkNewObject 已激活，新对象标黑 |
| 后台 sweep 线程访问该 Region | ✅ | `sweepGen == globalSweepGen_`，后台线程跳过 |
| Mark 结束后该 Region 还在 TL | ✅ | 下一轮 MarkSetup 会再次 flush；本轮已被 sweep 过 |
| 新分配的 Region（本轮从 OS 获取） | ✅ | `sweepGen == globalSweepGen_`，无需 sweep；bitmap 全零 |

### 8.4 总结：Handshake + sweepGen 安全保证

| 性质 | 保证机制 |
|------|----------|
| **无漏标** | SATB barrier 捕获覆写的旧值；GcPhaseEnum 捕获栈根；MarkNewObject 标记新对象 |
| **无误回收** | 所有可达对象均被标灰/标黑；未达对象不可能被标记 |
| **bitmap 一致** | 两阶段清除保证新周期开始前 bitmap 全零 |
| **sweep 安全** | sweepGen 保证每个 Region 恰好被 sweep 一次；惰性 sweep 避免 TL Region 竞态 |
| **无全局停顿** | 每个 Mutator 只在自己的 safepoint 短暂执行转换动作 |

## 9. 与现有 WCollector ENUM 阶段的对比

| 维度 | WCollector（CopyGC） | MS-GC（新设计） |
|------|---------------------|------------------|
| **转换机制** | TransitionToGCPhase(ENUM) handshake | TransitionToGCPhase(MS_MARK_SETUP) handshake |
| **全局停顿** | 无（handshake，非 STW） | 无（相同机制） |
| **屏障安装** | `InstallBarrier(ENUM)` → Copy barrier | `InstallBarrier(MS_MARK_SETUP)` → SATB barrier |
| **Mutator 自扫描** | `GcPhaseEnum()` — 扫描栈根，PushRoot 到 AllocBuffer | `GcPhaseEnum()` — **完全相同** |
| **TL Region** | 无 flush（Copy GC 无此需求） | **新增** ClearTLBitmap + FlushTLRegion |
| **Bitmap 清除** | 无 bitmap（Copy GC 无 mark bitmap） | **新增** 两阶段清除 |
| **EnsureSweepDone** | 无（Copy GC 没有 Sweep 阶段） | **新增** 保证上一轮 Sweep 完成 |
| **Sweep 范围管理** | 无 | **新增** sweepGen 代际标记 + 惰性 sweep |
| **MarkNewObject** | 检查 mutatorPhase==ENUM/TRACE | 检查 mutatorPhase==MS_MARK_SETUP/CONCURRENT_MARK/TERMINATION |
| **根枚举** | DoEnumeration → EnumRefFieldRoot(tag+CAS) | DoEnumeration → EnumRefFieldRoot(直接push) |
| **GC 线程后续** | TransitionToGCPhase(TRACE) → DoTracing | 进入 MS_CONCURRENT_MARK → 并发标记 |

**复用率总结**：

- ✅ **直接复用**：TransitionToGCPhase 机制、GcPhaseEnum、DoEnumeration、EnumAllCommonRoots、MergeMutatorRoots、HandleGCPhase 派发框架
- 🔧 **Override**：EnumRefFieldRoot、EnumAndTagRawRoot、MarkNewObject、InstallBarrier 分支
- 🆕 **新增**：ClearBitmaps、FlushTLRegion、EnsureSweepDone、sweepGen + 惰性 sweep、HandleGCPhase(MS_MARK_SETUP) 分支

## 10. 延迟预估

### Handshake 模型 vs STW 模型

| 指标 | STW 方案（旧） | Handshake 方案（新） |
|------|-------------|----------------|
| **全局停顿时间** | 所有 Mutator 同时停顿，总时间 = max(Mutator 到达 safepoint) + Setup 操作 | **0** — 无全局停顿 |
| **单 Mutator 停顿** | 同上 | 仅在自己的 safepoint 停顿，执行转换动作 |
| **单 Mutator 停顿时间** | N/A | ClearTLBitmap + FlushTL + GcPhaseEnum |

### 单 Mutator 转换时间拆解

| 操作 | 预估时间 | 依据 |
|------|---------|------|
| `ClearTLRegionBitmap()` | ~1µs | 单 Region 32KB bitmap memset ≈ 数百 ns |
| `FlushTLRegion()` | ~0.5µs | 两个指针赋值 + 链表插入 |
| `GcPhaseEnum()` | ~5–50µs | 取决于栈深度，栈上根枚举为线性扫描 |
| **合计** | **~7–52µs** | 对比 STW 方案的毫秒级全局停顿 |

### 对应用的影响

- **延迟敏感应用**：无全局停顿，最坏情况是单个 Mutator 停顿 ~50µs
- **多线程应用**：各 Mutator 独立转换，不会因某个慢 Mutator 到达 safepoint 而阻塞其他 Mutator
- **单线程应用**：与 STW 无异（只有一个 Mutator）
- **Go 对比**：Go 1.22 的 STW 均值 ~12µs。本方案的单 Mutator 停顿在同一数量级，但避免了全局协调开销

### 潜在风险

| 风险 | 影响 | 缓解措施 |
|------|------|----------|
| Mutator 长时间不到 safepoint | GC 线程等待时间增加 | 原有 `preempt` 机制强制中断 |
| GcPhaseEnum 扫描深栈 | 单 Mutator 停顿时间较长 | 协程栈默认较浅（~KB 级），实际影响小 |
| 多 Mutator 同时 flush 竞争中央池锁 | 并发瓶颈 | 中央池使用 lock-free 或 per-size-class 锁，争用极小 |
