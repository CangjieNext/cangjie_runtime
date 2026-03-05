# Step 2：三色标记与 Sweep 详细设计 —— 替换 Forward 为 Mark-Sweep

> 本文档是 [Go 风格 GC 迁移方案](Go风格GC迁移方案.md) Step 2 的独立详细设计文档。
> 目标：用 Go 风格的并发三色标记 + Sweep 替换现有的 Trace + Forward（复制式）回收流程。

---

## 目录

1. [现有 GC 流程深度分析](#1-现有-gc-流程深度分析)
2. [三色标记抽象与实现映射](#2-三色标记抽象与实现映射)
3. [MarkSweepCollector 总体设计](#3-marksweecollector-总体设计)
4. [Phase 1: Mark Setup（STW）](#4-phase-1-mark-setupstw)
5. [Phase 2: Concurrent Mark（并发标记）](#5-phase-2-concurrent-mark并发标记)
6. [Phase 3: Mark Termination（STW）](#6-phase-3-mark-terminationstw)
7. [Phase 4: Sweep 实现](#7-phase-4-sweep-实现)
8. [GC Phase 状态机改造](#8-gc-phase-状态机改造)
9. [与现有基础设施的复用与改造清单](#9-与现有基础设施的复用与改造清单)
10. [验证方案](#10-验证方案)

---

## 1. 现有 GC 流程深度分析

### 1.1 Collector 类层次

```
Collector (抽象基类, src/Heap/Collector/Collector.h)
  └── TracingCollector (标记追踪框架, TracingCollector.h/cpp)
        └── CopyCollector (复制式 GC 框架, CopyCollector.h/cpp)
              └── WCollector (当前生产 Collector, WCollector.h/cpp)

CollectorProxy (代理, 持有 WCollector，委托所有调用)
```

**Step 2 需要在 TracingCollector 同级** 新建 `MarkSweepCollector`，复用 `TracingCollector` 的标记基础设施，但**不继承 CopyCollector**（不需要 Forward）。

### 1.2 WCollector::DoGarbageCollection 完整流程

```
WCollector::DoGarbageCollection()          // WCollector.cpp L527
  │
  ├── TraceHeap()                          // WCollector.cpp L440
  │     ├── AssembleGarbageCandidates()    // 收集 from-space 候选
  │     ├── ENUM → DoEnumeration()         // STW: 根枚举，标记引用指针
  │     ├── TRACE → PrepareTrace()         // 准备标记
  │     │        → DoTracing()             // 并发标记（workStack 窃取式）
  │     │        → ProcessFinalizers()     // 终结器入队
  │     └── (退出 TRACE)
  │
  ├── PostTrace()                          // WCollector.cpp L445
  │     ├── POST_TRACE phase
  │     ├── HandleTraceRegions()           // 处理 trace 阶段分配的 Region
  │     ├── ClearWeakRefBuffer()           // 清除弱引用
  │     ├── ClearSatbBuffer()              // 清除 SATB 缓冲
  │     ├── CollectLargeGarbage()          // 回收大对象（未标记 → garbage）
  │     ├── CollectPinnedGarbage()         // 回收 pinned 对象
  │     ├── RefineFromSpace()              // 筛选 from-space（剔除高存活率 Region）
  │     └── PrepareForwardTable()          // 创建 from→to 路由表
  │
  ├── Preforward()                         // WCollector.cpp L451
  │     └── LightSync + 并行修复各类根指针
  │
  ├── ForwardFromSpace()                   // 并发复制存活对象到 to-space
  │
  ├── IDLE phase
  │     ├── FlipTagID()                    // 翻转指针 tag
  │     └── CollectSmallSpace()            // 回收 from-space garbage Region
  │
  └── ReclaimGarbageMemory()               // 释放物理页

关键 Phase 顺序:
  IDLE → ENUM(STW) → TRACE(并发) → CLEAR_SATB → POST_TRACE(STW)
       → PREFORWARD(LightSync) → FORWARD(并发) → IDLE
```

### 1.3 需要**保留**的基础设施

| 组件 | 文件 | 复用方式 |
|------|------|----------|
| `MarkStack<T>` (WorkStack) | `Common/MarkWorkStack.h` | 直接复用，灰色对象队列 |
| `PrefetchQueue` | `TracingCollector.h` | 直接复用，标记预取优化 |
| `RegionBitmap` (mark bitmap) | `Heap/Collector/LiveInfo.h` | 直接复用，per-Region 标记位图 |
| `RegionInfo::MarkObject()` | `Heap/Allocator/RegionInfo.h` | 直接复用，设置标记位 |
| `SatbBuffer` | `Mutator/SatbBuffer.h` | 复用结构，适配新语义 |
| `WeakRefBuffer` | `Mutator/SatbBuffer.h` | 直接复用 |
| `FinalizerProcessor` | `Heap/Collector/FinalizerProcessor.h` | 直接复用 |
| `EnumAllCommonRoots()` | `TracingCollector.cpp` | 需改造（去除指针 tag） |
| `ConcurrentMarkingWork` | `TracingCollector.cpp` | 需改造（去除 Forward 相关逻辑） |
| `StaticRootTable` / `ExportRootTable` | `TracingCollector.h` | 直接复用 |
| `ScopedStopTheWorld` | `Mutator/MutatorManager.h` | 直接复用 |
| `GCThreadPool` | `Common/GCThreadPool.h` | 直接复用 |

### 1.4 需要**废弃**的组件

| 组件 | 原因 |
|------|------|
| `ForwardObject` / `ForwardObjectImpl` | Mark-Sweep 不移动对象 |
| `ForwardDataManager` / `ForwardTable` | 不需要 from→to 路由 |
| `TagID` / `FlipTagID` / 指针 tagging | 不需要区分新旧指针 |
| `RefineFromSpace` / `ExemptFromRegions` | 不需要选择 from-space |
| `Preforward*` 系列方法 | 不需要修复根指针 |
| `GhostFromRegion` 机制 | 不需要保留旧 Region 镜像 |
| `RouteInfo` / `RouteState` | 不需要对象路由 |
| `CopyCollector` 基类 | 不需要复制框架 |

### 1.5 现有标记机制详解

#### Mark Bitmap

```
每个 Region 按需分配一个 RegionBitmap:
  - 1 bit 对应 8 字节 (kMarkedBytesPerBit = 8)
  - 使用 atomic<uint64_t> markWords[] 数组
  - CAS 操作实现并发安全标记
  
RegionInfo::MarkObject(obj, objSize):
  if (IsLargeRegion):
    设置 metadata.isMarked 标志（整个 Region = 1 对象）
  else:
    offset = GetAddressOffset(obj)
    markBitmap->MarkBits(offset, objSize, regionSize)
    → 返回 true 表示已标记（跳过），false 表示新标记

RegionInfo::IsMarkedObject(obj):
  markBitmap->IsMarked(offset) → 检查单个位
```

#### WorkStack（灰色队列）

```
MarkStack<BaseObject*>:
  - 分段链表：MarkStackBuf<T> 每段 64 个元素
  - push_back / pop_back: O(1)
  - split(n): 拆分给线程池工作窃取
  - insert(other): 合并两个栈

ConcurrentMarkingWork::Execute():
  loop:
    obj = workStack.pop()
    wasMarked = MarkObject(obj)
    if (!wasMarked):
      if obj.IsWeakRef → 特殊处理 referent，记录到 WeakRefBuffer
      else → TraceObjectRefFields(obj, workStack)  // 子引用入队
    TryForkTask()  // 工作栈过大时分裂给 threadPool
```

#### SATB Buffer

```
SatbBuffer:
  - per-Mutator Node（64 个槽位），写满后 retire
  - TraceBarrier::WriteReference 中：写引用前将旧值 push 到 SATB Node
  - GC 标记后处理：MarkSatbBuffer() 遍历所有 retired Nodes
    → 未标记对象加入 workStack → 继续标记
    → 循环直到 workStack 为空
```

---

## 2. 三色标记抽象与实现映射

### 2.1 三色抽象定义

| 颜色 | 语义 | Cangjie Runtime 实现 |
|------|------|---------------------|
| **白色 (White)** | 未被标记，GC 结束后仍为白色则是垃圾 | `RegionBitmap::IsMarked(offset) == false` |
| **灰色 (Gray)** | 已被发现（标记位 = 1）但子引用尚未扫描 | 标记位 = 1 **且** 仍在 `WorkStack` 中 |
| **黑色 (Black)** | 已标记且所有子引用已扫描 | 标记位 = 1 **且** 已出队处理完毕 |

> **实现关键点**：灰色和黑色在 bitmap 中无法区分（都是 marked），区分完全依赖 WorkStack 的出入队状态。这与 Go 一致 —— `gcWork` 队列承载灰色对象。

### 2.2 标记位的翻转（Mark Bit Flip）

**现状**：WCollector 在每次 GC 开始时重置 RegionBitmap（`ClearBitmap()`），然后从 0 标记到 1。

**改造方向**：保持现有方式不变。Go 使用指针最高位作为 mark bit 并做 flip，但 Cangjie 使用独立的 RegionBitmap，不需要引入额外机制。每个 GC 周期开始时 `ClearBitmap()` 即可将所有对象变为白色。

### 2.3 写屏障与三色不变性

三色标记的核心约束 —— **强三色不变性**（无黑色对象直接引用白色对象）或**弱三色不变性**（白色对象可达性通过灰色对象保护）：

| Go 的方案 | 当前 Cangjie 的方案 | Step 2 方案 |
|-----------|---------------------|-------------|
| 混合写屏障（Dijkstra 插入 + Yuasa 删除） | Yuasa SATB（仅删除屏障） | **保持 SATB 删除屏障**，新分配对象标记为黑色 |

**SATB 写屏障维持弱三色不变性的原理**：
```
TraceBarrier::WriteReference(obj, offset, newValue):
  oldValue = obj->GetField(offset)
  if (oldValue != nullptr):
    satbBuffer->Push(oldValue)     // 保存旧引用，确保旧引用不被遗漏
  obj->SetField(offset, newValue)  // 执行赋值
```

- **删除屏障**：当黑色对象 A 的一个字段从指向白色对象 B 改为指向其他对象时，SATB 会记录旧值 B → B 被 remark 阶段重新标记 → B 不会丢失
- **新分配黑色**：并发标记期间分配的对象**直接标记为黑色**（已实现：`WCollector::MarkNewObject` 在 ENUM/TRACE/CLEAR_SATB 阶段对新分配对象调用 `MarkObject`）
- **Go 的混合屏障**额外添加了 Dijkstra 插入屏障（写入新值也记录），可以避免栈重扫描。Cangjie 的 SATB + MarkTermination STW 重扫描方案同样正确，且简单可靠。

### 2.4 颜色转换状态机

```
                    ┌──────────────────────────────┐
                    │                              │
                    ▼                              │
               ┌─────────┐                        │
    All objs → │  白色    │ ←── GC 开始时 ClearBitmap │
               │ (White)  │                        │
               └────┬─────┘                        │
                    │ EnumRoots / TraceRefField     │
                    │ → MarkObject + Push           │
                    ▼                              │
               ┌─────────┐                        │
               │  灰色    │ ← bitmap=1 + 在 WorkStack │
               │ (Gray)   │                        │
               └────┬─────┘                        │
                    │ Pop + TraceObjectRefFields    │
                    ▼                              │
               ┌─────────┐                        │
               │  黑色    │ ← bitmap=1 + 已处理      │
               │ (Black)  │                        │
               └──────────┘                        │
                                                   │
               GC 结束：仍为白色 → 垃圾 → Sweep ───────┘
```

### 2.5 与 Go 三色实现的对比

| 维度 | Go | Cangjie (Step 2) |
|------|----|--------------------|
| 标记位存储 | 堆 arena 的 bitmap（2 bit/指针槽） | RegionBitmap（1 bit/8 bytes），per-Region |
| 灰色队列 | `gcWork`（per-P 本地队列 + 全局队列） | `MarkStack<BaseObject*>`（分段链表 + split 工作窃取） |
| 写屏障 | 混合屏障（插入+删除） | SATB 删除屏障 |
| 新分配着色 | 标记为黑色（`gcmarknewobject`） | 标记为黑色（`MarkNewObject`，已有实现） |
| Mark Termination | STW + drain 灰色队列 | STW + MarkSatbBuffer + drain WorkStack |
| 栈扫描 | 并发（栈使用 stack barrier 或抢占式扫描） | STW EnumRoots（保守方案，后续可优化） |

---

## 3. MarkSweepCollector 总体设计

### 3.1 类继承关系

```
Collector (抽象基类)
  └── TracingCollector
        ├── CopyCollector → WCollector      ← 现有的复制式 GC（保留，不修改）
        └── MarkSweepCollector [NEW]        ← 新增 Mark-Sweep GC
```

**设计决策**：`MarkSweepCollector` 直接继承 `TracingCollector`，复用其标记框架（`DoEnumeration`、`DoTracing`、`ConcurrentMarkingWork`、`MarkSatbBuffer`），但**不继承 CopyCollector**（不需要 Forward/Copy 能力）。

### 3.2 新文件结构

```
src/Heap/MarkSweepCollector/
  ├── MarkSweepCollector.h        // 类声明
  ├── MarkSweepCollector.cpp      // 核心 GC 流程
  ├── SweepWorker.h               // Sweep 工作单元
  ├── SweepWorker.cpp             // Region 级 Sweep 实现
  └── MSBarrier.h                 // Mark-Sweep 专用屏障
```

### 3.3 DoGarbageCollection 新流程

```cpp
void MarkSweepCollector::DoGarbageCollection() {
    // ===== Phase 1: Mark Setup (STW) =====
    MarkSetup();
    
    // ===== Phase 2: Concurrent Mark (并发) =====
    ConcurrentMark();
    
    // ===== Phase 3: Mark Termination (STW) =====
    MarkTermination();
    
    // ===== Phase 4: Sweep (并发) =====
    Sweep();
    
    // ===== Phase 5: GC 完成 =====
    UpdateGCTrigger();
    SetPhase(MS_PHASE_IDLE);
}
```

**与 WCollector 流程对比**：

| 阶段 | WCollector | MarkSweepCollector |
|------|------------|-------------------|
| 根枚举 + 标记准备 | TraceHeap() (ENUM+TRACE) | MarkSetup() (STW) |
| 并发标记 | DoTracing() | ConcurrentMark() |
| 标记后处理 | PostTrace() (STW) | MarkTermination() (STW) |
| 对象复制 | Preforward() + ForwardFromSpace() | **无（不移动对象）** |
| 垃圾回收 | CollectSmallSpace() | Sweep()（逐 Region 回收死亡对象→空闲链表） |
| 空间翻转 | FlipTagID() | **无** |

### 3.4 关键数据成员

```cpp
class MarkSweepCollector : public TracingCollector {
private:
    // === 标记相关 (继承自 TracingCollector) ===
    // WorkStack workStack_;          // 灰色对象队列
    // PrefetchQueue prefetchQueue_; // 预取优化
    // StaticRootTable staticRoots_; // 静态根
    // ExportRootTable exportRoots_; // 导出根
    
    // === Sweep 相关 (新增) ===
    std::vector<RegionInfo*> sweepCandidates_;   // 待 sweep 的 Region 列表
    std::atomic<size_t> sweepIndex_{0};          // 并发 sweep 的工作窃取索引
    size_t bytesReclaimed_{0};                   // 本轮回收的字节数
    size_t bytesMarked_{0};                      // 本轮标记的字节数
    
    // === GC 触发相关 (新增) ===
    size_t gcTriggerBytes_{0};                   // 下次 GC 触发阈值
    double gogcPercent_{100.0};                  // GOGC 比例（默认 100%）
    
    // === Sweep 状态 (新增) ===
    std::atomic<bool> sweepDone_{true};          // sweep 是否完成
};
```

### 3.5 与 Go runtime 的对应关系

| Go 概念 | MarkSweepCollector 对应 |
|---------|------------------------|
| `gcStart()` | `DoGarbageCollection()` 入口 |
| `gcBgMarkStartWorkers` | 复用 `GCThreadPool` + `ConcurrentMarkingWork` |
| `gcMarkDone()` | `MarkTermination()` |
| `gcSweep()` | `Sweep()` |
| `gcWork` (per-P 灰色队列) | `WorkStack` (split 实现工作窃取) |
| `mheap.sweepgen` | `sweepDone_` + `sweepIndex_` |
| `gcController.trigger` | `gcTriggerBytes_` |
| `GOGC` 环境变量 | `gogcPercent_` 配置 |

---

## 4. Phase 1: Mark Setup（STW）

### 4.1 目标

在**极短的 STW 窗口**内完成以下工作：
1. 清空上一轮标记位（所有对象变白色）
2. 安装写屏障（SATB 删除屏障）
3. 枚举根集，将根引用的对象标记为灰色
4. 启动并发标记阶段

### 4.2 伪代码

```cpp
void MarkSweepCollector::MarkSetup() {
    // 确保上一轮 Sweep 已完成（Go 也有此检查）
    WaitForSweepCompletion();
    
    // ---- STW 开始 ----
    ScopedStopTheWorld stw(mutatorManager_);
    
    SetPhase(MS_PHASE_MARK_SETUP);
    
    // 0. 回收所有线程的 Thread-Local Region（详见 §4.7）
    //    将 TL Region 从线程私有变为中央池管辖，确保 Sweep 可达
    RetireAllThreadLocalRegions();
    
    // 1. 清除所有 Region 的标记位图
    ClearAllMarkBitmaps();
    
    // 2. 安装 SATB 写屏障
    //    切换 Barrier 为 MSTraceBarrier（Mark-Sweep 版本的 TraceBarrier）
    InstallBarrier(new MSTraceBarrier(this));
    
    // 3. 重置灰色队列
    workStack_.clear();
    
    // 4. 根枚举 → 灰色对象入队
    //    ** 复用 TracingCollector::EnumAllCommonRoots() **
    //    但需改造：去除指针 tag 处理逻辑（详见 §9.5）
    EnumAllCommonRoots();
    //    输出：rootSet_ 已填充所有根引用
    
    // 5. 将根集对象标记 + 入队
    for (auto* obj : rootSet_) {
        if (MarkObject(obj)) {                // 设置 bitmap 位 → 变灰
            workStack_.push_back(obj);         // 入灰色队列
        }
    }
    
    // 6. 标记新分配对象为黑色的 hook
    EnableMarkNewObject();   // 并发分配时调用 MarkNewObject
    
    // 7. 快照当前所有已分配 Region 到 sweepCandidates_（详见 §7.2a）
    //    此后新分配的 Region 不纳入本轮 Sweep
    SnapshotSweepCandidates();
    
    // 8. 递增全局 GC epoch（详见 §5.9）
    globalGCEpoch_.fetch_add(2, std::memory_order_release);
    
    SetPhase(MS_PHASE_MARK);
    // ---- STW 结束 ----
}
```

### 4.3 ClearAllMarkBitmaps 实现

```cpp
void MarkSweepCollector::ClearAllMarkBitmaps() {
    // 遍历所有已分配的 Region，清除其 LiveInfo 中的 markBitmap
    auto& regionManager = heapManager_->GetRegionManager();
    regionManager.ForEachRegion([](RegionInfo* region) {
        if (region->HasMarkBitmap()) {
            region->GetLiveInfo()->ClearMarkBitmap();
        }
    });
    
    // 也需要清除大对象 Region 的 isMarked 标志
    regionManager.ForEachLargeRegion([](RegionInfo* region) {
        region->GetMetadata().isMarked = false;
    });
}
```

### 4.4 与现有 WCollector ENUM 阶段的对比

| 维度 | WCollector ENUM | MarkSweep MarkSetup |
|------|----------------|---------------------|
| STW 方式 | `ScopedStopTheWorld` | 相同 |
| TL Region 处理 | — | `RetireAllThreadLocalRegions()`（§4.7） |
| Bitmap 清除 | `ClearBitmap()` per-Region | `ClearAllMarkBitmaps()` 批量 |
| 根枚举 | `EnumAllCommonRoots()` | 复用框架，Override 2 个 visitor（§9.5） |
| 屏障安装 | `EnumBarrier` → `TraceBarrier` | 直接 `MSTraceBarrier` |
| from-space 选择 | `AssembleGarbageCandidates()` | **无**（不需要选择 from-space） |
| Sweep 快照 | — | `SnapshotSweepCandidates()` + gcEpoch（§7.2a） |
| 输出 | rootSet_ 填充 | 同上 + workStack 填充 |

### 4.5 STW 时间预估

Mark Setup STW 涉及的工作量：
- `RetireAllThreadLocalRegions()`：O(线程数)，每线程处理 1~2 个 Region ≈ 几微秒
- `ClearAllMarkBitmaps()`：O(Region 数量)，每个 Region 一次 `memset`。1000 个 Region ≈ 几十微秒
- `EnumAllCommonRoots()`：O(线程数 × 栈帧数 + 静态根数)。现有实现已并行化，通常 < 1ms
- `SnapshotSweepCandidates()`：O(Region 数量)，遍历收集引用 ≈ 几十微秒
- 总计目标：< 1ms（参考 Go 的 Mark Setup STW 通常 < 50μs）

### 4.6 确保上一轮 Sweep 完成

```cpp
void MarkSweepCollector::WaitForSweepCompletion() {
    // 如果 Sweep 尚未完成，需要同步等待或协助完成
    // Go runtime 中：STW 前会检查 sweepone，如有残留 sweep 会先完成
    if (!sweepDone_.load(std::memory_order_acquire)) {
        // 选项 A: 同步等待 sweep 完成
        // 选项 B: 在当前线程帮助 sweep（推荐，减少等待时间）
        HelpSweep();  // 窃取 sweepCandidates_ 中的剩余 Region 进行 sweep
    }
}
```

### 4.7 Thread-Local Region 回收（RetireAllThreadLocalRegions）

**问题**：MarkSetup 开始时，每个线程的 AllocBuffer 中可能持有一个正在使用的 `tlRegion`（通过 bump-pointer 分配）。这些 Region 处于线程私有状态，不在中央池的任何 RegionList 中。**必须在 MarkSetup STW 阶段将它们全部回收到中央池**。

#### 4.7.1 必须回收的三个原因

| # | 原因 | 详细说明 |
|---|------|---------|
| 1 | **Sweep / Mutator 竞态** | 如果 TL Region 不回收，Sweep 阶段无法感知这些 Region。Sweep 建立的 FreeList 和 Mutator 的 bump-pointer 分配会同时操作同一个 Region，产生**数据竞争**。 |
| 2 | **分配模型切换** | 从 bump-pointer → free-list 分配是 Mark-Sweep 的核心改造（Step 1）。回收 TL Region 让 Sweep 有机会扫描其内部，构建 FreeList；后续分配从 FreeList 获取空位，而非继续 bump-pointer。 |
| 3 | **ClearBitmap 正确性** | `ClearAllMarkBitmaps()` 需要遍历**所有已分配 Region** 并清零 bitmap。如果 TL Region 不在中央池的视野内，可能遗漏清除，导致上一轮的残留标记位**误判对象为存活**。 |

**Go 的对应机制**：Go 在 STW 的 `gcStart()` 中调用 `forEachP(func(pp *p) { pp.mcache.prepareForSweep(sweepgen) })`，将每个 P 的 mcache 中的 mspan 全部 flush 回 mcentral/mheap，与此处的设计完全一致。

#### 4.7.2 实现

```cpp
void MarkSweepCollector::RetireAllThreadLocalRegions() {
    // 遍历所有 AllocBuffer（每个 Mutator 线程一个）
    allocBufferManager_->VisitAllocBuffers([this](AllocBuffer* buffer) {
        RegionInfo* tlRegion = buffer->GetTLRegion();
        if (tlRegion != nullptr) {
            // 1. 设置 Region 的水印线（当前 bump-pointer 位置）
            //    水印线之后的区域是未分配的，Sweep 可将其加入 FreeList
            tlRegion->SetAllocWatermark(buffer->GetAllocPosition());
            
            // 2. 将 Region 归还中央池
            centralPool_->ReturnPartialRegion(tlRegion);
            
            // 3. 清空 AllocBuffer 的 tlRegion 引用
            buffer->SetTLRegion(nullptr);
            buffer->ResetAllocPosition();
        }
        
        // 同样处理 preparedRegion（如有）
        RegionInfo* prepared = buffer->GetPreparedRegion();
        if (prepared != nullptr) {
            centralPool_->ReturnPartialRegion(prepared);
            buffer->SetPreparedRegion(nullptr);
        }
    });
}
```

#### 4.7.3 时序位置

`RetireAllThreadLocalRegions()` **必须在 `ClearAllMarkBitmaps()` 之前**执行。原因：
1. 回收后 TL Region 进入中央池 → `ClearAllMarkBitmaps()` 能遍历到它们
2. 回收后设置水印线 → Sweep 知道哪些区域有对象、哪些是空闲的

```
MarkSetup STW 执行顺序:
  RetireAllThreadLocalRegions()    ← 先回收 TL Region
  ClearAllMarkBitmaps()            ← 再清标记位
  InstallBarrier()
  EnumAllCommonRoots()
  SnapshotSweepCandidates()
  globalGCEpoch_ += 2
```

---

## 5. Phase 2: Concurrent Mark（并发标记）

### 5.1 目标

与 Mutator 并发执行，追踪所有可达对象并标记为黑色。这是 GC 周期中耗时最长的阶段。

### 5.2 伪代码

```cpp
void MarkSweepCollector::ConcurrentMark() {
    // Phase 已在 MarkSetup 末尾设为 MS_PHASE_MARK
    
    // 1. 并发标记 — 复用 TracingCollector::DoTracing() 核心逻辑
    //    启动 ConcurrentMarkingWork 任务组，从 workStack 弹出灰色对象，
    //    扫描其引用字段（变黑），将发现的白色对象标记并入队（变灰）
    DoConcurrentMarking();
    
    // 2. 并发标记完成后，workStack 为空
    //    但 Mutator 可能在此期间产生了新的 SATB 记录
    //    → 这些在 MarkTermination 中处理
}
```

### 5.3 DoConcurrentMarking 详细流程

```cpp
void MarkSweepCollector::DoConcurrentMarking() {
    // 复用现有 ConcurrentMarkingWork 任务（TracingCollector.cpp L107-143）
    // 需要改造的部分用 [改造] 标记
    
    // 1. 创建并发标记任务
    auto task = new MSConcurrentMarkingWork(this, &workStack_);
    gcThreadPool_->Submit(task);
    
    // 2. 当前 GC 线程也参与标记（类似 Go 的 dedicated mark worker）
    MSMarkLoop();
    
    // 3. 等待所有并发标记任务完成
    gcThreadPool_->WaitForCompletion();
}
```

### 5.4 标记循环（核心热路径）

```cpp
// 改造自 ConcurrentMarkingWork::Execute()
void MSConcurrentMarkingWork::Execute() {
    while (true) {
        BaseObject* obj = workStack_->pop_back();  // 弹出灰色对象
        if (obj == nullptr) break;                 // 无灰色对象 → 结束
        
        // [改造] 不需要处理 TaggedPointer / FindLatestVersion
        //        Mark-Sweep 下对象地址永远不变
        
        bool wasMarked = collector_->MarkObject(obj);
        if (wasMarked) continue;  // 已标记，跳过
        
        // 特殊处理：WeakReference
        if (obj->IsWeakRef()) {
            // 不追踪 referent 字段（弱引用语义）
            // 将 WeakRef 记录到 WeakRefBuffer，MarkTermination 时处理
            weakRefBuffer_->Insert(obj);
            // 只扫描 WeakRef 自身的非 referent 字段
            TraceWeakRefNonReferentFields(obj);
            continue;
        }
        
        // 扫描对象的所有引用字段 → 子引用标记并入灰色队列
        TraceObjectRefFields(obj, *workStack_);
        
        // 工作窃取：workStack 过大时，split 一半给 threadPool 新任务
        TryForkTask();
    }
}
```

### 5.5 TraceRefField 改造

```cpp
// [改造] 简化版的 TraceRefField — 去除 tag/forward 相关逻辑
void MarkSweepCollector::TraceRefField(BaseObject** fieldAddr, WorkStack& stack) {
    BaseObject* ref = *fieldAddr;
    if (ref == nullptr) return;
    
    // [去除] 不需要 IsTaggedPointer / FindLatestVersion / UpdateField
    //        Mark-Sweep 下对象不会移动，引用永远有效
    
    // 检查是否在堆范围内
    if (!heapManager_->IsInHeap(ref)) return;
    
    // 标记对象（CAS 设置 bitmap 位）
    bool newlyMarked = MarkObject(ref);
    if (newlyMarked) {
        stack.push_back(ref);  // 新发现的白色对象 → 入灰色队列
    }
}
```

**与现有 `WCollector::TraceRefField()` 的对比**：

| 现有逻辑（WCollector.cpp L256-290） | Mark-Sweep 版本 |
|-------------------------------------|-----------------|
| `IsTaggedPointer(ref)` → 旧指针处理 | **去除**：无指针 tagging |
| `FindLatestVersion(ref)` → 查找转发后地址 | **去除**：对象不移动 |
| `UpdateRefField(fieldAddr, latest)` | **去除**：无需更新引用 |
| `MarkObject(ref)` + `push_back` | **保留**：核心标记逻辑完全相同 |

### 5.6 新分配对象的处理

并发标记期间，Mutator 继续分配新对象。这些对象必须标记为**黑色**（已标记），否则可能被误回收：

```cpp
// 复用现有 WCollector::MarkNewObject() 机制
// 在分配路径中（Allocator::Allocate）：
void MarkSweepCollector::MarkNewObject(BaseObject* obj, size_t size) {
    GCPhase phase = GetPhase();
    if (phase == MS_PHASE_MARK || phase == MS_PHASE_MARK_SETUP) {
        // 新分配对象直接标记为黑色（已标记但不入灰色队列）
        RegionInfo* region = RegionInfo::GetRegionInfoAt(obj);
        region->MarkObject(obj, size);
        // 注意：不 push 到 workStack_（黑色 = 已处理）
    }
}
```

### 5.7 并发标记的终止条件

并发标记阶段在以下条件下终止：
1. `workStack_` 为空（所有灰色对象已处理完毕）
2. 所有并发标记线程已退出
3. **注意**：此时 SATB buffer 中可能有残留记录，需要在 MarkTermination 中处理

### 5.8 标记期间的辅助标记（Mark Assist）

Go 的 Mark Assist 机制：分配内存时，如果分配速度超过标记速度，Mutator 需要帮助标记。

**Step 2 简化方案**：暂不实现 Mark Assist，依靠 GC 线程池完成所有标记工作。后续可作为优化项：

```cpp
// 未来优化：在 Allocator::Allocate 中添加
if (gcRunning && allocDebt > 0) {
    // Mutator 帮助消耗 workStack 中的灰色对象
    MSMarkAssist(allocDebt);
    allocDebt = 0;
}
```

### 5.9 并发标记期间的 Region 获取策略（gcEpoch 机制）

**问题**：并发标记期间 Mutator 继续分配内存，需要从中央池获取 Region。但中央池中混杂着不同 GC 周期的 Region，如何判断一个 Region 是否可以安全使用？

#### 5.9.1 gcEpoch 概念

引入 `gcEpoch`（GC 纪元）机制，类似 Go 的 `sweepgen`：

```cpp
// 全局 GC 纪元，每轮 GC 递增 2
std::atomic<uint64_t> globalGCEpoch_;  // 初始值 0，每次 GC +2

// 每个 Region 记录自己的 GC 纪元
struct RegionInfo {
    uint64_t gcEpoch;  // 该 Region 最后一次参与 GC 时的 epoch
    // ...
};
```

**纪元含义**：

| Region.gcEpoch 值 | 含义 | 可否在标记期间获取 |
|-------------------|------|-------------------|
| `globalGCEpoch_ - 2` | 上一轮 GC 的 Region，**需要 Sweep** | ✅ **可以**，先 lazy sweep 再使用 |
| `globalGCEpoch_` | 当前轮 GC 的 Region（MarkSetup 后分配） | ❌ **不可 Sweep**（本轮标记未完成） |
| `globalGCEpoch_ + 2` | 未来轮次（不应出现） | N/A |

#### 5.9.2 标记期间的 Region 获取来源

Mutator 在并发标记期间需要 Region 时，按优先级尝试以下来源：

```
1. 部分已用 Region（已有 FreeList）
   条件：Region 已被 Sweep（gcEpoch == globalGCEpoch_ - 2 且 swept 标志为 true）
   操作：直接从 FreeList 分配
   ✅ 推荐来源（复用已有空间、零延迟）

2. 上一轮的满 Region（lazy sweep）
   条件：gcEpoch == globalGCEpoch_ - 2 且 swept == false
   操作：先 Sweep 该 Region → 构建 FreeList → 分配
   ✅ 安全（上一轮标记结果已确定）

3. 全新 Region（从 FreeRegionManager 获取空白 Region）
   条件：FreeRegionManager 有空闲 Region 可分配
   操作：分配空白 Region，设 gcEpoch = globalGCEpoch_
   ✅ 安全（新 Region 无需 Sweep）

4. 当前轮已满的 Region
   条件：gcEpoch == globalGCEpoch_ 且已满
   操作：❌ 不可 Sweep（本轮标记未完成，不知道哪些对象存活）
   ❌ 必须等到下一轮 GC 才能 Sweep
```

#### 5.9.3 与 Go sweepgen 的对比

| 维度 | Go sweepgen | Cangjie gcEpoch |
|------|-------------|-----------------|
| 递增方式 | `mheap_.sweepgen += 2` | `globalGCEpoch_ += 2` |
| 存储位置 | `mspan.sweepgen` | `RegionInfo.gcEpoch` |
| 判断逻辑 | `sweepgen-2`=需 sweep, `sweepgen-1`=正在 sweep, `sweepgen`=已 sweep | 相同 |
| lazy sweep | `sweepone()` in `mallocgc()` | 第一版急切 sweep，后续可改为 lazy |

#### 5.9.4 gcEpoch 在 MarkSetup 中的更新

GC 周期开始时，在 MarkSetup STW 中递增 `globalGCEpoch_`（见 §4.2 伪代码步骤 8）。此后所有新分配的 Region 自动获得新 epoch 值，不会被本轮及旧轮的 Sweep 误清理。

### 5.10 并发标记框架复用分析与性能对比

基于对 `TracingCollector.cpp` 和 `WCollector.cpp` 的源码分析，现有并发标记分为**框架层（TracingCollector）**和**算法层（WCollector）**两层，框架层与 GC 算法完全解耦，可直接复用。

#### 5.10.1 框架层 — 直接复用

| 组件 | 所在文件 | 职责 | 是否含 CopyGC 逻辑 |
|------|---------|------|-------------------|
| **ConcurrentMarkingWork** | TracingCollector.cpp L62-145 | 标记主循环 + 工作窃取（TryForkTask） | **无**——内部全部通过 virtual 调用注入算法差异 |
| **TracingImpl** | TracingCollector.cpp L323-345 | 启动并行标记：split workStack → 多线程 Execute | ❌ 无 |
| **AddConcurrentTracingWork** | TracingCollector.cpp L350-365 | 按线程数拆分初始根集任务 | ❌ 无 |
| **DoTracing** | TracingCollector.cpp L384-440 | 总调度：线程数/优先级 → TracingImpl → ConcurrentReMark → FindUselessExtern → DoResurrection | 部分有（DoResurrection 用 tag） |
| **ConcurrentReMark** | TracingCollector.cpp L499 | 委托 MarkSatbBuffer | ❌ 无 |
| **MarkSatbBuffer** | TracingCollector.cpp L441-497 | 循环收集 SATB + 重标记 + 超时退化 STW | ❌ 无 |
| **TryForkTask** | ConcurrentMarkingWork 内 | 工作窃取：栈过大时 split 给线程池 | ❌ 无 |
| **PrefetchQueue** | TracingCollector.h L40-70 | 环形缓冲 + `__builtin_prefetch` | ❌ 无 |
| **WorkStack (MarkStack)** | Common/MarkWorkStack.h | 可 split 的工作栈 | ❌ 无 |

**关键发现**：`ConcurrentMarkingWork::Execute()` 热循环中所有算法差异都通过 3 个 virtual 调用注入：

```cpp
// ConcurrentMarkingWork::Execute() 核心循环（TracingCollector.cpp L103-143）
for (;;) {
    BaseObject* obj = workStack.back();
    workStack.pop_back();
    bool wasMarked = collector.MarkObject(obj);      // ← virtual
    if (!wasMarked) {
        if (!obj->HasRefField()) continue;
        if (UNLIKELY(obj->IsWeakRef())) {
            auto* referent = collector.GetAndTryTagObj(obj, *referentField);  // ← virtual
            collector.TraceObjectRefFields(referent, workStack);              // ← virtual
            WeakRefBuffer::Instance().Insert(obj);
        } else {
            collector.TraceObjectRefFields(obj, workStack);  // ← virtual
        }
    }
    if (threadPool) TryForkTask();
}
```

因此 **`ConcurrentMarkingWork` 类无需新建 MSConcurrentMarkingWork**，只需 MarkSweepCollector override 3 个 virtual 方法即可。

#### 5.10.2 算法层 — 需 Override 的 virtual 方法

| virtual 方法 | WCollector 实现 | MarkSweepCollector 实现 |
|-------------|----------------|------------------------|
| `TraceRefField` | 6 步骤：IsCurrentPointer → IsOldPointer → FindLatestVersion → GetAndTryTagRefField → CAS → push（~40 行） | 3 步骤：GetTargetObject → IsHeapAddress → push（~5 行） |
| `TraceObjectRefFields` | 包装 TraceRefField | 包装简化版 TraceRefField（不变） |
| `GetAndTryTagObj` | tag 检查 + FindLatest + CAS（~25 行） | 直接 GetTargetObject + IsHeapAddress（~5 行） |
| `MarkObject` | `region->MarkObject(obj, size)` + AddLiveByteCount | 基类版本功能等价，可直接复用 |
| `DoResurrection` | GetAndTryTagRefField + tag/CAS | 需 Override：去除 tag/CAS |

#### 5.10.3 标记热路径性能对比

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

#### 5.10.4 定量估算

假设典型对象有 4 个引用字段，标记 100 万个对象：

```
WCollector:  100万 × 4字段 × (2 tag check + 1 FindLatest + 1 CAS) = 400万次额外操作
MarkSweep:   100万 × 4字段 × (1 load + 1 check) = 基础操作

CAS 在现代 CPU 上约 10-20ns，400万次 CAS ≈ 40-80ms 额外开销
Cache miss (FindLatest) 约 50-100ns，假设 10% 命中 ≈ 40万次 × 75ns ≈ 30ms

预估标记阶段性能提升：10~30%（取决于对象图结构和旧指针比例）
```

#### 5.10.5 Trade-off 总结

| 维度 | CopyGC (WCollector) | MarkSweep |
|------|--------------------|-----------|
| **标记热路径效率** | 慢（额外 tag/CAS/ForwardTable） | **快**（纯标记，零 CAS） |
| **Sweep/回收阶段** | **快**（复制后 from-space 整体回收，无 Sweep） | 慢（需逐 Region 遍历 Sweep） |
| **STW 暂停** | 较长（Enum + Trace + Forward 均需 STW 配合） | **更短**（仅 MarkSetup + MarkTermination 短暂 STW） |
| **碎片** | **无**（复制式自动紧缩） | 有（原地回收，需 FreeList 管理） |
| **总 CPU 开销** | 复制大对象成本高 | Sweep 并发分摊，但增加总 CPU |
| **适用场景** | 短生命周期对象多、堆较小 | **大堆、长生命周期对象多** |

---

## 6. Phase 3: Mark Termination（STW）

### 6.1 目标

在**短暂 STW** 内完成标记收尾工作：
1. 处理 SATB buffer 残留记录（确保弱三色不变性完整）
2. 清理弱引用
3. 处理终结器（Finalizer）
4. 卸载写屏障
5. 准备 Sweep 所需的 Region 列表

### 6.2 伪代码

```cpp
void MarkSweepCollector::MarkTermination() {
    // ---- STW 开始 ----
    ScopedStopTheWorld stw(mutatorManager_);
    
    SetPhase(MS_PHASE_MARK_TERMINATION);
    
    // 1. 处理 SATB buffer 残留
    //    ** 复用 TracingCollector::MarkSatbBuffer() **
    //    遍历所有 Mutator 的 SATB Node，将未标记对象加入 workStack
    //    循环直到 workStack 为空
    MarkSatbBuffer();
    
    // 2. 处理 Finalizer
    //    ** 复用 FinalizerProcessor::EnqueueFinalizables() **
    //    遍历已注册的 Finalizer，未标记（白色）的对象入 Finalize 队列
    //    可终结对象会被复活（标记为灰色），需要再次追踪
    ProcessFinalizers();
    DrainWorkStack();   // 复活对象的引用链也需要标记
    
    // 3. 处理弱引用
    //    ** 复用 WeakRefBuffer::ClearWeakRefBuffer() **
    //    遍历 WeakRefBuffer，清除指向白色对象的 referent
    ClearWeakRefBuffer();
    
    // 4. 处理外部弱引用（ExternObjectManager）
    //    ** 复用 FindUselessExternObjects() **
    FindUselessExternObjects();
    
    // 5. 统计标记结果
    bytesMarked_ = CalculateTotalMarkedBytes();
    
    // 6. sweepCandidates_ 已在 MarkSetup 中快照（§7.2a）
    //    此处可做可选的验证性检查
    // [已移至 MarkSetup] PrepareSweepCandidates();
    
    // 7. 卸载 SATB 写屏障，安装 idle 屏障
    //    从此刻起 Mutator 写引用不再记录到 SATB
    InstallBarrier(new IdleBarrier());
    DisableMarkNewObject();
    
    // 8. 设置 Sweep 阶段
    sweepDone_.store(false, std::memory_order_release);
    sweepIndex_.store(0, std::memory_order_release);
    SetPhase(MS_PHASE_SWEEP);
    
    // ---- STW 结束 ----
}
```

### 6.3 MarkSatbBuffer 处理

```
复用 TracingCollector::MarkSatbBuffer()（TracingCollector.cpp L450-505）:

loop:
  for each mutator:
    node = mutator.satbBuffer.RetireCurrentNode()
    for each ref in retiredNodes:
      if ref != nullptr && !IsMarked(ref):
        MarkObject(ref)      // 白 → 灰
        workStack.push(ref)
  
  DrainWorkStack()  // 将新灰色对象处理完
  
  if workStack.empty() && 没有新 retiredNodes:
    break

改造点: 无需改造，SATB 处理逻辑与标记方式无关。
```

### 6.4 ProcessFinalizers 处理

```
复用 FinalizerProcessor（TracingCollector.cpp + FinalizerProcessor.h）:

1. EnqueueFinalizables(pred = !IsMarked):
   遍历已注册的 Finalizer 对象
   如果对象未标记（白色）→ 入队终结队列
   
2. 复活（Resurrection）:
   入队的对象需要被复活（标记 + 入灰色队列）
   因为 Finalizer 执行时需要访问对象
   
3. DrainWorkStack():
   复活对象的引用链也全部标记
   
4. Notify FP thread:
   唤醒 Finalizer 执行线程

改造点: 
  - 去除 Forward 相关的 Finalizer 处理
  - 复活时直接 MarkObject（不需要复制对象）
```

### 6.5 PrepareSweepCandidates（已前移至 MarkSetup）

> **设计更新**：经分析，`sweepCandidates_` 的快照需要在 MarkSetup STW 中完成（见 §7.2a `SnapshotSweepCandidates()`），以确保：
> 1. 快照发生在 STW 中，不会与并发标记期间新分配的 Region 混淆
> 2. 通过 gcEpoch 机制区分不同 GC 周期的 Region
> 
> MarkTermination 中不再重复收集 sweepCandidates_，原 `PrepareSweepCandidates()` 代码由 §7.2a 的 `SnapshotSweepCandidates()` 取代。
> MarkTermination 中对应的步骤 6 (`PrepareSweepCandidates()`) 改为验证性检查（可选）。

### 6.6 与 WCollector PostTrace 的对比

| WCollector PostTrace | MarkSweep MarkTermination |
|----------------------|--------------------------|
| `HandleTraceRegions()` — 处理标记期间分配的 Region | **不需要** — 新分配的对象已标记为黑色 |
| `ClearWeakRefBuffer()` | **复用** |
| `ClearSatbBuffer()` — 只清空不处理 | `MarkSatbBuffer()` — **处理后清空** |
| `CollectLargeGarbage()` / `CollectPinnedGarbage()` | → 移至 Sweep 阶段统一处理 |
| `RefineFromSpace()` — 选择 from/to-space | **去除** |
| `PrepareForwardTable()` — 创建路由表 | **去除** |

### 6.7 STW 时间预估

- `MarkSatbBuffer()`：取决于 SATB buffer 积累量，通常 O(百~千个对象)
- `ProcessFinalizers()`：取决于注册的 Finalizer 数量，通常很少
- `ClearWeakRefBuffer()`：O(弱引用数量)
- `PrepareSweepCandidates()`：O(Region 数量)
- 总计目标：< 500μs

---

## 7. Phase 4: Sweep 实现

### 7.1 目标

遍历所有已分配 Region，将**未标记（白色）的对象**回收，将空间归还给分配器的空闲链表。Sweep 与 Mutator **并发执行**。

### 7.2 Sweep 总体架构

```
Sweep 工作分为三类:

1. 普通 Region Sweep (< 4MB 对象所在的 Region)
   → 遍历 Region bitmap，未标记对象 → 释放到 Region 内 FreeList
   → 全空 Region → 归还 FreeRegionManager
   
2. 大对象 Region Sweep (Region = 单个大对象, ≤ 4MB)
   → 检查 metadata.isMarked
   → 未标记 → 整个 Region 归还 FreeRegionManager
   
3. 超大对象 Sweep (> 4MB, 独立 mmap)
   → 检查 metadata.isMarked
   → 未标记 → munmap 释放
```

### 7.2a Sweep 范围详细说明

**Sweep 不只清理"中央池里的 Region"**。Sweep 的目标是 MarkSetup STW 阶段快照的 **`sweepCandidates_`**，即 GC 开始时**所有已分配的 Region**，具体包括：

| Region 类别 | 来源 | 是否 Sweep | 说明 |
|-------------|------|-----------|------|
| **退役的 TL Region** | MarkSetup 中 `RetireAllThreadLocalRegions()` 回收 | ✅ 是 | 线程曾在使用、未满的 Region |
| **已满 Region** | 分配过程中已满并归还中央池的 Region | ✅ 是 | 相当于现有的 recentFullRegionList |
| **大对象 Region** (≤4MB) | 占 1~N 个连续 Region 的大对象 | ✅ 是 | 整个 Region 粒度判定存活/死亡 |
| **超大对象** (>4MB) | 独立 mmap 分配 | ✅ 是 | munmap 释放 |
| **Mark 期间新分配的 Region** | 并发标记阶段从 FreeRegionManager 获取 | ❌ 否 | `gcEpoch == globalGCEpoch_`，对象全黑，无垃圾 |
| **空闲 Region** | FreeRegionManager 中的未分配 Region | ❌ 否 | 没有对象，无需 Sweep |

**快照时机**：在 MarkSetup STW 中调用 `SnapshotSweepCandidates()`（见 §4.2 伪代码步骤 7），将当前时刻所有已分配 Region 的引用记录到 `sweepCandidates_` 列表。此后新分配的 Region 不在此列表中，自然被排除。

```cpp
void MarkSweepCollector::SnapshotSweepCandidates() {
    sweepCandidates_.clear();
    auto& regionManager = heapManager_->GetRegionManager();
    
    // 所有普通 Region（包括刚回收的 TL Region 和已满 Region）
    regionManager.ForEachRegion([this](RegionInfo* region) {
        if (region->IsAllocated()) {
            region->gcEpoch = globalGCEpoch_.load() - 2;  // 标记为"需 Sweep"
            sweepCandidates_.push_back(region);
        }
    });
    
    // 大对象 Region
    regionManager.ForEachLargeRegion([this](RegionInfo* region) {
        if (region->IsAllocated()) {
            region->gcEpoch = globalGCEpoch_.load() - 2;
            sweepCandidates_.push_back(region);
        }
    });
}
```

> **注意**：§6.5 中原有的 `PrepareSweepCandidates()` 逻辑已前移至 MarkSetup 阶段（此处的 `SnapshotSweepCandidates`），以确保快照发生在 STW 中，避免与并发标记期间新分配的 Region 混淆。MarkTermination 中不再重复收集。

### 7.2b 浮动垃圾（Floating Garbage）

**问题**：并发标记期间，Mutator 可能从中央池获取一个部分使用的 Region（有空位）放入 TLS。此后本轮 GC 将该 Region 中的部分对象标记为垃圾。但由于该 Region 已经在 TLS 中（不在 `sweepCandidates_` 里），本轮 Sweep **无法清除这些垃圾对象**。

**这就是浮动垃圾（Floating Garbage）**——由设计带来的一轮延迟回收，Go 也有同样的行为。

#### 7.2b.1 影响分析

| 维度 | 分析 |
|------|------|
| **正确性** | ✅ 不会误回收存活对象。垃圾只是**多存活一轮**，下一轮 GC 的 MarkSetup 会 `RetireAllThreadLocalRegions()` 并将其纳入 `sweepCandidates_`，此时这些垃圾就会被清除 |
| **内存浪费上界** | 线程数 × 每线程持有的 SizeClass 数 × Region 大小 × 死亡率。假设 32 线程 × 1 Region × 256KB × 50% 死亡率 ≈ **4MB**。实际场景中通常更小 |
| **Go 的态度** | Go 接受浮动垃圾。mcache 中的 mspan 在下一轮 GC 的 `prepareForSweep()` 才被 flush |

#### 7.2b.2 可选优化

**在 MarkTermination STW 中重新回收 TLS Region**：

```cpp
// 可选优化（v1 不实现）
void MarkSweepCollector::MarkTermination() {
    ScopedStopTheWorld stw(mutatorManager_);
    // ... 其他 MarkTermination 逻辑 ...
    
    // [可选] 将 Mark 期间被 TLS 持有的 Region 也纳入 sweepCandidates_
    // 这会消除浮动垃圾，但增加 STW 时间
    RetireAllThreadLocalRegions();
    AppendNewSweepCandidates();
}
```

**推荐**：v1 接受浮动垃圾，与 Go 行为一致；如果后续监控发现浮动垃圾影响显著，再添加此优化。

### 7.3 并发 Sweep 入口

```cpp
void MarkSweepCollector::Sweep() {
    // Sweep 与 Mutator 并发执行（STW 已在 MarkTermination 末尾结束）
    
    // 1. 启动并发 sweep 工作线程
    size_t numWorkers = std::min(gcThreadPool_->GetThreadCount(), 
                                  sweepCandidates_.size());
    for (size_t i = 0; i < numWorkers; i++) {
        gcThreadPool_->Submit(new SweepWorker(this));
    }
    
    // 2. 当前线程也参与 sweep
    SweepLoop();
    
    // 3. 等待所有 sweep 完成
    gcThreadPool_->WaitForCompletion();
    
    // 4. 超大对象 sweep（独立 mmap 的 > 4MB 对象）
    SweepHugeObjects();
    
    // 5. 向操作系统归还物理页
    ReleaseUnusedPages();
    
    // 6. 标记 sweep 完成
    sweepDone_.store(true, std::memory_order_release);
}
```

### 7.4 SweepLoop — 工作窃取

```cpp
void MarkSweepCollector::SweepLoop() {
    while (true) {
        // 原子递增 sweepIndex_，窃取下一个待 sweep 的 Region
        size_t idx = sweepIndex_.fetch_add(1, std::memory_order_relaxed);
        if (idx >= sweepCandidates_.size()) break;
        
        RegionInfo* region = sweepCandidates_[idx];
        
        if (region->IsLargeRegion()) {
            SweepLargeRegion(region);
        } else {
            SweepNormalRegion(region);
        }
    }
}
```

### 7.5 SweepNormalRegion — 普通 Region Sweep

这是 Sweep 的核心实现，需要与 Step 1 的空闲链表分配器配合：

```cpp
void MarkSweepCollector::SweepNormalRegion(RegionInfo* region) {
    RegionBitmap* bitmap = region->GetLiveInfo()->GetMarkBitmap();
    uintptr_t regionStart = region->GetRegionStart();
    uintptr_t regionEnd = region->GetRegionEnd();
    
    size_t totalFreeBytes = 0;
    size_t totalLiveBytes = 0;
    FreeList localFreeList;  // Region 局部空闲链表
    
    uintptr_t cursor = regionStart;
    uintptr_t freeStart = 0;
    bool inFreeSpan = false;
    
    // 遍历 Region 中的每个对象槽位
    while (cursor < regionEnd) {
        BaseObject* obj = reinterpret_cast<BaseObject*>(cursor);
        size_t objSize = obj->GetObjectSize();
        
        if (objSize == 0) break;  // 到达分配水印线（未分配区域）
        
        bool isMarked = bitmap->IsMarked(cursor - regionStart);
        
        if (isMarked) {
            // === 存活对象 ===
            totalLiveBytes += objSize;
            
            if (inFreeSpan) {
                // 结束一段空闲区间 → 加入 FreeList
                size_t freeSize = cursor - freeStart;
                localFreeList.Add(freeStart, freeSize);
                totalFreeBytes += freeSize;
                inFreeSpan = false;
            }
        } else {
            // === 死亡对象 ===
            // [可选] 调用析构（如有 C++ raw destructor）
            // [可选] 清零内存（debug 模式）
            
            if (!inFreeSpan) {
                freeStart = cursor;
                inFreeSpan = true;
            }
            // 连续死亡对象合并为一个大 free span
        }
        
        cursor += AlignUp(objSize, kObjectAlignment);
    }
    
    // 处理 Region 尾部的空闲区间
    if (inFreeSpan) {
        size_t freeSize = cursor - freeStart;
        localFreeList.Add(freeStart, freeSize);
        totalFreeBytes += freeSize;
    }
    
    // 未分配区域（水印线到 Region 末尾）也是空闲的
    if (cursor < regionEnd) {
        // allocWatermark 之后的区域加入空闲链表
        localFreeList.Add(cursor, regionEnd - cursor);
        totalFreeBytes += (regionEnd - cursor);
    }
    
    // === 判断 Region 状态 ===
    if (totalLiveBytes == 0) {
        // 整个 Region 为空 → 归还给 FreeRegionManager
        region->SetState(RegionState::FREE);
        regionManager_->ReturnRegion(region);
        bytesReclaimed_ += region->GetRegionSize();
    } else {
        // Region 有存活对象 → 更新其 FreeList
        region->SetFreeList(std::move(localFreeList));
        region->SetLiveBytes(totalLiveBytes);
        bytesReclaimed_ += totalFreeBytes;
        
        // 将 Region 归还 CentralRegionPool，供后续分配
        centralPool_->ReturnPartialRegion(region);
    }
}
```

### 7.6 SweepLargeRegion — 大对象 Region Sweep

```cpp
void MarkSweepCollector::SweepLargeRegion(RegionInfo* region) {
    // 大对象 Region = 1 个对象占 1~N 个连续 Region
    if (region->GetMetadata().isMarked) {
        // 存活 → 保留
        return;
    }
    
    // 死亡 → 归还所有连续 Region 给 FreeRegionManager
    size_t regionCount = region->GetSpanningRegionCount();
    for (size_t i = 0; i < regionCount; i++) {
        RegionInfo* span = region + i;
        span->SetState(RegionState::FREE);
        regionManager_->ReturnRegion(span);
    }
    bytesReclaimed_ += region->GetRegionSize() * regionCount;
}
```

### 7.7 SweepHugeObjects — 超大对象 Sweep (>4MB)

```cpp
void MarkSweepCollector::SweepHugeObjects() {
    // > 4MB 对象使用独立 mmap，不在 Region 体系中
    auto& hugeObjectList = heapManager_->GetHugeObjectList();
    
    auto it = hugeObjectList.begin();
    while (it != hugeObjectList.end()) {
        HugeObjectInfo& info = *it;
        if (info.isMarked) {
            info.isMarked = false;  // 重置标记，为下一轮 GC 准备
            ++it;
        } else {
            // 未标记 → munmap 释放
            munmap(info.addr, info.size);
            bytesReclaimed_ += info.size;
            it = hugeObjectList.erase(it);
        }
    }
}
```

### 7.8 ReleaseUnusedPages — 物理页归还

```cpp
void MarkSweepCollector::ReleaseUnusedPages() {
    // 对归还给 FreeRegionManager 的 Region，调用 madvise 释放物理页
    // 复用 Step 1 中 FreeRegionManager::ReleaseUnits() 的实现
    regionManager_->ReleaseUnusedPhysicalPages();
    
    // 效果：
    //   Linux: madvise(MADV_DONTNEED) — 立即释放物理页
    //   macOS: madvise(MADV_FREE) — 标记为可回收
    //   保留虚拟地址空间，不影响后续重新分配
}
```

### 7.9 Lazy Sweep 优化（后续迭代）

Go 使用 lazy sweep：不在 GC 后立即 sweep 所有 Region，而是在分配时按需 sweep。

**Step 2 第一版**采用**急切 sweep**（上述实现），所有 Region 在 GC 后立即 sweep。后续可优化为 lazy sweep：

```
Lazy Sweep 方案（优化项）:
  1. GC 完成后，不立即 sweep，只设置 sweepCandidates_
  2. Mutator 分配内存时:
     - 先检查有无待 sweep 的 Region
     - 有 → sweep 该 Region，从其 FreeList 分配
     - 无 → 从已 sweep 的 Region 分配
  3. 优势: sweep 开销分摊到分配路径，减少 GC 暂停后的 CPU 峰值
  4. Go 实现: sweepone() 在 mallocgc 中调用
```

### 7.10 与 Go Sweep 的对比

| 维度 | Go | Cangjie Mark-Sweep (Step 2) |
|------|----|-----------------------------|
| Sweep 时机 | Lazy — 分配时按需 | 第一版：急切；后续：lazy |
| Sweep 粒度 | per-mspan（8KB~1MB） | per-Region（256KB~4MB） |
| 空闲管理 | mspan.freeindex + allocBits/gcmarkBits | Region 内 FreeList |
| 大对象 | manualFreeList → munmap | 大 Region → FreeRegionManager |
| 超大对象 | mheap_.allocManual | 独立 mmap → munmap |
| 页归还 | scavenger goroutine (madvise) | ReleaseUnusedPages (madvise) |
| 并发 | 分配路径隐式并发 | 显式并发 sweep workers |

---

## 8. GC Phase 状态机改造

### 8.1 现有 GCPhase 枚举

```cpp
// src/Heap/Collector/Collector.h
enum GCPhase {
    UNDEF                = 0,
    IDLE                 = 1,
    FINISH               = 2,
    RECLAIM_SATB_NODE    = 3,
    INIT                 = 8,
    ENUM                 = 9,    // STW 根枚举
    TRACE                = 10,   // 并发标记
    CLEAR_SATB_BUFFER    = 11,
    POST_TRACE           = 12,   // STW 标记后处理
    PREFORWARD           = 13,   // LightSync 修复根
    FORWARD              = 14,   // 并发复制
};
```

### 8.2 新增 Mark-Sweep Phase 值

```cpp
// 在 GCPhase 枚举中新增（使用高位避免冲突）：
enum GCPhase {
    // ... 保留现有值（WCollector 使用） ...
    
    // === Mark-Sweep GC Phases ===
    MS_PHASE_IDLE              = 20,   // 空闲（等待触发）
    MS_PHASE_MARK_SETUP        = 21,   // STW: 安装屏障 + 根枚举
    MS_PHASE_MARK              = 22,   // 并发标记
    MS_PHASE_MARK_TERMINATION  = 23,   // STW: SATB 清理 + 弱引用 + Finalizer
    MS_PHASE_SWEEP             = 24,   // 并发 Sweep
};
```

### 8.3 Phase 状态转换图

```
     ┌────────────────────────────────────────────────────────┐
     │                                                        │
     ▼                                                        │
  MS_PHASE_IDLE ──[GC 触发]──→ MS_PHASE_MARK_SETUP (STW)     │
                                     │                        │
                                     ▼                        │
                              MS_PHASE_MARK (并发)            │
                                     │                        │
                                     ▼                        │
                              MS_PHASE_MARK_TERMINATION (STW) │
                                     │                        │
                                     ▼                        │
                              MS_PHASE_SWEEP (并发)           │
                                     │                        │
                                     └────────────────────────┘
```

### 8.4 各 Phase 对应的 Barrier 类

| Phase | Barrier 类 | 作用 |
|-------|-----------|------|
| `MS_PHASE_IDLE` | `IdleBarrier` | 无屏障，普通读写 |
| `MS_PHASE_MARK_SETUP` | `MSTraceBarrier` | SATB 删除屏障（MarkSetup 末尾安装） |
| `MS_PHASE_MARK` | `MSTraceBarrier` | SATB 删除屏障（写引用前保存旧值） |
| `MS_PHASE_MARK_TERMINATION` | `MSTraceBarrier` | 保持屏障直到 MarkTermination 完成 |
| `MS_PHASE_SWEEP` | `IdleBarrier` | 卸载屏障（Sweep 无需屏障） |

### 8.5 MSTraceBarrier 设计

```cpp
// src/Heap/MarkSweepCollector/MSBarrier.h
class MSTraceBarrier : public Barrier {
public:
    MSTraceBarrier(MarkSweepCollector* collector) : collector_(collector) {}
    
    void WriteReference(BaseObject* obj, size_t offset, BaseObject* newValue) override {
        // SATB 删除屏障
        BaseObject* oldValue = obj->GetRefField(offset);
        if (oldValue != nullptr) {
            // 将旧值记录到当前 Mutator 的 SATB buffer
            GetCurrentMutator()->GetSatbBuffer()->Push(oldValue);
        }
        // 执行实际写入
        obj->SetRefField(offset, newValue);
    }
    
    // [与现有 TraceBarrier 的区别]
    // 1. 去除指针 tag 检查（IsTaggedPointer）
    // 2. 去除 Forward 相关的指针更新
    // 3. 只保留纯 SATB 语义
    
private:
    MarkSweepCollector* collector_;
};
```

### 8.6 Phase 切换对分配器的影响

| Phase | 新分配对象行为 |
|-------|--------------|
| `MS_PHASE_IDLE` | 普通分配，不标记 |
| `MS_PHASE_MARK_SETUP` | 分配 + 标记为黑色（`MarkNewObject`） |
| `MS_PHASE_MARK` | 分配 + 标记为黑色（`MarkNewObject`） |
| `MS_PHASE_MARK_TERMINATION` | 分配 + 标记为黑色 |
| `MS_PHASE_SWEEP` | 普通分配（已卸载标记 hook） |

**关键**：在 MARK_SETUP / MARK / MARK_TERMINATION 三个阶段，新分配的对象**必须标记为黑色**，否则可能被当轮 Sweep 误回收。

### 8.7 Phase 查询的线程安全

```cpp
// Phase 值通过 atomic<GCPhase> 存储
// 各处查询使用 memory_order_acquire，设置使用 memory_order_release
// 确保 Phase 切换对所有线程可见

void SetPhase(GCPhase phase) {
    phase_.store(phase, std::memory_order_release);
}

GCPhase GetPhase() const {
    return phase_.load(std::memory_order_acquire);
}
```

---

## 9. 与现有基础设施的复用与改造清单

### 9.1 总览矩阵

| 组件 | 所在文件 | 处理方式 | 改造内容 |
|------|---------|----------|---------|
| **MarkStack (WorkStack)** | `Common/MarkWorkStack.h` | ✅ 直接复用 | 无 |
| **PrefetchQueue** | `TracingCollector.h` | ✅ 直接复用 | 无 |
| **RegionBitmap (mark bitmap)** | `Heap/Collector/LiveInfo.h` | ✅ 直接复用 | 无 |
| **RegionInfo::MarkObject** | `Heap/Allocator/RegionInfo.h` | ✅ 直接复用 | 无 |
| **SatbBuffer** | `Mutator/SatbBuffer.h` | ✅ 直接复用 | 无 |
| **WeakRefBuffer** | `Mutator/SatbBuffer.h` | ✅ 直接复用 | 无 |
| **FinalizerProcessor** | `Heap/Collector/FinalizerProcessor.h` | ✅ 直接复用 | 无 |
| **ScopedStopTheWorld** | `Mutator/MutatorManager.h` | ✅ 直接复用 | 无 |
| **ScopedLightSync** | `Mutator/MutatorManager.h` | ✅ 直接复用 | 无 |
| **GCThreadPool** | `Common/GCThreadPool.h` | ✅ 直接复用 | 无 |
| **StaticRootTable** | `TracingCollector.h` | ✅ 直接复用 | 无 |
| **ExportRootTable** | `TracingCollector.h` | ✅ 直接复用 | 无 |
| **EnumAllCommonRoots** | `TracingCollector.cpp` | ✅ 直接复用 | 框架与 GC 算法解耦，详见 §9.5 |
| **DoEnumeration** | `TracingCollector.cpp` | ✅ 直接复用 | 枚举调度逻辑，与标记/复制无关 |
| **MergeMutatorRoots** | `TracingCollector.cpp` | ✅ 直接复用 | 收集栈根，无 tag/forward 逻辑 |
| **EnumAllSurrectedExportRoots** | `TracingCollector.cpp` | ✅ 直接复用 | 直接 push_back，无 tag 处理 |
| **EnumRefFieldRoot** (virtual) | `WCollector.cpp` | 🔧 Override | 去除 tag/forward/CAS（~5 行），详见 §9.5.2 |
| **EnumAndTagRawRoot** (virtual) | `WCollector.cpp` | 🔧 Override | 去除 tag/CAS（~5 行），详见 §9.5.2 |
| **ConcurrentMarkingWork** | `TracingCollector.cpp` | ✅ 直接复用 | 内部全通过 virtual 调用注入算法差异，无需新建 MSConcurrentMarkingWork，详见 §5.10.1 |
| **TracingImpl** | `TracingCollector.cpp` | ✅ 直接复用 | 启动并行标记 + split workStack，与 GC 算法无关 |
| **AddConcurrentTracingWork** | `TracingCollector.cpp` | ✅ 直接复用 | 按线程数拆分初始任务，与 GC 算法无关 |
| **TryForkTask** | `ConcurrentMarkingWork` 内 | ✅ 直接复用 | 工作窃取逻辑，与 GC 算法无关 |
| **TraceRefField** (virtual) | `WCollector.cpp` | 🔧 Override | 去除 tag/Forward/CAS（~40 行→~5 行），详见 §5.10.2 |
| **TraceObjectRefFields** (virtual) | `WCollector.cpp` | 🔧 Override | 包装简化版 TraceRefField |
| **GetAndTryTagObj** (virtual) | `WCollector.cpp` | 🔧 Override | 去除 tag/CAS（~25 行→~5 行），详见 §5.10.2 |
| **MarkSatbBuffer** | `TracingCollector.cpp` | ✅ 直接复用 | SATB 处理逻辑与标记方式无关 |
| **ConcurrentReMark** | `TracingCollector.cpp` | ✅ 直接复用 | 委托 MarkSatbBuffer |
| **DoTracing** | `TracingCollector.cpp` | 🔧 小幅改造 | DoResurrection 需 Override 去除 tag/CAS，其余直接复用 |
| **ProcessFinalizers** | `TracingCollector.cpp` | 🔧 改造 | 去除 Forward 相关的复活逻辑 |
| **GCPhase 枚举** | `Heap/Collector/Collector.h` | 🆕 扩展 | 添加 MS_PHASE_* 值 |
| **Barrier 类** | `Mutator/Barrier.h` | 🆕 新增 | `MSTraceBarrier` |
| **CollectorProxy** | `Heap/Collector/CollectorProxy.h` | 🔧 改造 | 支持切换到 MarkSweepCollector |
| **ForwardTable / ForwardData** | `WCollector/` | ❌ 不使用 | Mark-Sweep 无需 |
| **TagID / FlipTagID** | `WCollector.cpp` | ❌ 不使用 | Mark-Sweep 无需 |
| **GhostFromRegion** | 多处 | ❌ 不使用 | Mark-Sweep 无需 |

### 9.2 新增文件清单

| 文件 | 职责 |
|------|------|
| `src/Heap/MarkSweepCollector/MarkSweepCollector.h` | 类声明 |
| `src/Heap/MarkSweepCollector/MarkSweepCollector.cpp` | GC 主流程 (MarkSetup + ConcurrentMark + MarkTermination + Sweep) |
| `src/Heap/MarkSweepCollector/SweepWorker.h` | Sweep 工作线程声明 |
| `src/Heap/MarkSweepCollector/SweepWorker.cpp` | SweepNormalRegion / SweepLargeRegion / SweepHugeObjects |
| `src/Heap/MarkSweepCollector/MSBarrier.h` | MSTraceBarrier 实现 |

> **注意**：原计划的 `MSMarkWork.h/cpp`（MSConcurrentMarkingWork）**不再需要**。经源码分析（§5.10.1），`ConcurrentMarkingWork` 内部全部通过 virtual 调用注入算法差异，MarkSweepCollector 只需 override `TraceRefField` / `TraceObjectRefFields` / `GetAndTryTagObj` 即可复用整个标记框架。

### 9.3 需修改的现有文件

| 文件 | 修改内容 |
|------|---------|
| `src/Heap/Collector/Collector.h` | 添加 `MS_PHASE_*` 枚举值 |
| `src/Heap/Collector/TracingCollector.h` | 将 `EnumAllCommonRoots()` 提取为 virtual 方法，便于 override |
| `src/Heap/Collector/TracingCollector.cpp` | 提取可复用逻辑为 protected 方法 |
| `src/HeapManager.h/cpp` | 支持创建和切换 MarkSweepCollector |
| `src/Heap/Collector/CollectorProxy.h/cpp` | 添加 MarkSweepCollector 委托 |
| `CMakeLists.txt` | 添加新源文件编译 |

### 9.4 改造优先级和风险评估

| 优先级 | 改造项 | 风险 | 说明 |
|--------|--------|------|------|
| P0 | MarkSweepCollector 骨架 + Phase 定义 | 低 | 新增代码，不影响现有 |
| P0 | SweepNormalRegion | 中 | 核心新功能，需要充分测试 |
| P0 | MSTraceBarrier | 低 | 简化版 TraceBarrier |
| P0 | MSConcurrentMarkingWork | 低 | 简化版 ConcurrentMarkingWork |
| P1 | EnumAllCommonRoots 改造 | 中 | 可能影响 WCollector 回归 |
| P1 | CollectorProxy 切换支持 | 低 | 添加新分支 |
| P2 | Lazy Sweep 优化 | 中 | 与分配器路径耦合 |
| P2 | Mark Assist | 中 | 影响分配热路径 |

### 9.5 根枚举（Enum）复用详细分析

基于对 `TracingCollector.cpp` 和 `WCollector.cpp` 的源码分析，以下是根枚举体系各组件的复用结论。

#### 9.5.1 可直接复用的组件

| 组件 | 所在文件 | 调用关系 | 复用原因 |
|------|---------|---------|---------|
| **EnumAllCommonRoots** | TracingCollector.cpp L747 | 顶层框架 | 并行启动 4 类根枚举任务 + MergeMutatorRoots，逻辑与 GC 算法无关 |
| **DoEnumeration** | TracingCollector.cpp L307 | 调用 EnumAllCommonRoots + EnumAllExportRoots | 枚举调度逻辑，与标记/复制无关 |
| **MergeMutatorRoots** | TracingCollector.cpp L290 | AllocBuffer.MergeRoots() | 收集各线程的栈根，无任何 tag/forward 逻辑 |
| **EnumAllSurrectedExportRoots** | TracingCollector.cpp L565 | 直接 push_back 到 rootSet | 无 tag/forward 处理，直接复用 |

**EnumAllCommonRoots 框架结构**（TracingCollector.cpp L747-830）：

```
EnumAllCommonRoots():
  ├── Task 1: EnumStaticRoots()              → 调用 EnumRefFieldRoot (visitor)
  ├── Task 2: EnumConcurrencyModelRoots()    → 调用 EnumAndTagRawRoot (visitor)
  ├── Task 3: EnumFinalizerProcessorRoots()  → 调用 EnumAndTagRawRoot (visitor)
  ├── Task 4: EnumAllSurrectedExportRoots()  → 直接 push_back (无 visitor)
  └── MergeMutatorRoots()                    → AllocBuffer.MergeRoots()
  最后合并各线程 rootSet → rootSet_
```

这个框架**与 GC 算法完全解耦**，只负责"从哪些来源收集根"以及"并行化执行"。GC 算法的差异仅体现在两个虚方法 visitor 中。

#### 9.5.2 需要 Override 的两个虚方法

这两个方法是 `TracingCollector` 的虚方法，由 `WCollector` 实现时**深度耦合了 Copy-GC 的指针 tag 机制**。`MarkSweepCollector` 必须 override 为简化版本。

##### EnumRefFieldRoot（被 EnumStaticRoots 调用）

**WCollector 现有实现**（WCollector.cpp L130-180）— 6 步骤，其中 4 步为 tag 专用：

```cpp
// WCollector 版本（Copy-GC）
void WCollector::EnumRefFieldRoot(RefField<BaseObject>& field, vector<BaseObject*>& rootSet) {
    BaseObject* ref = field.GetTargetObject();
    if (ref == nullptr) return;
    
    if (IsCurrentPointer(ref))  return;      // ① tag 检查：当前版本 → 跳过
    if (IsOldPointer(ref)) {                 // ② tag 检查：旧版本
        ref = FindLatestVersion(ref);        // ③ 查找最新复制后地址
    }
    if (!IsHeapAddress(ref)) return;
    
    bool tagged = GetAndTryTagRefField(field, ref);  // ④ CAS 标记指针 tag
    if (!tagged) return;                              // 已被其他线程标记
    
    rootSet.push_back(ref);
}
```

**MarkSweepCollector 简化版本** — 核心逻辑仅 ~5 行：

```cpp
// MarkSweep 版本（无指针移动，无 tag）
void MarkSweepCollector::EnumRefFieldRoot(RefField<BaseObject>& field, vector<BaseObject*>& rootSet) {
    BaseObject* ref = field.GetTargetObject();
    if (ref == nullptr) return;
    if (!IsHeapAddress(ref)) return;
    rootSet.push_back(ref);
}
```

**去除的逻辑**：`IsCurrentPointer` / `IsOldPointer` / `FindLatestVersion` / `GetAndTryTagRefField` / CAS — 这些全部是 Copy-GC 的指针 tag 机制，Mark-Sweep 下对象不移动、指针不变化，完全不需要。

##### EnumAndTagRawRoot（被 EnumConcurrencyModelRoots / EnumFinalizerProcessorRoots 调用）

**WCollector 现有实现**（WCollector.cpp L190-230）— 类似的 tag 逻辑：

```cpp
// WCollector 版本
void WCollector::EnumAndTagRawRoot(BaseObject*& rawRef, vector<BaseObject*>& rootSet) {
    if (rawRef == nullptr) return;
    CHECK(!IsOldPointer(rawRef));  // 断言：raw root 不应指向旧版本
    
    if (IsCurrentPointer(rawRef)) return;  // tag 检查
    BaseObject* ref = GetTargetObject(rawRef);
    if (!IsHeapAddress(ref)) return;
    
    bool tagged = GetAndTryTagRefField(rawRef, ref);  // CAS tag
    if (!tagged) return;
    
    rootSet.push_back(ref);
}
```

**MarkSweepCollector 简化版本**：

```cpp
// MarkSweep 版本
void MarkSweepCollector::EnumAndTagRawRoot(BaseObject*& rawRef, vector<BaseObject*>& rootSet) {
    if (rawRef == nullptr) return;
    if (!IsHeapAddress(rawRef)) return;
    rootSet.push_back(rawRef);
}
```

#### 9.5.3 实现方案

**推荐方案**：`MarkSweepCollector` 继承 `TracingCollector`，override `EnumRefFieldRoot` 和 `EnumAndTagRawRoot` 两个虚方法：

```cpp
class MarkSweepCollector : public TracingCollector {
protected:
    // Override: 去除 Copy-GC 的 tag/forward 逻辑
    void EnumRefFieldRoot(RefField<BaseObject>& field, 
                          std::vector<BaseObject*>& rootSet) override;
    void EnumAndTagRawRoot(BaseObject*& rawRef, 
                           std::vector<BaseObject*>& rootSet) override;
    
    // 其余根枚举方法（EnumAllCommonRoots, DoEnumeration, MergeMutatorRoots 等）
    // 全部从 TracingCollector 继承，无需 override
};
```

**改造量评估**：仅需 override 2 个方法，每个方法 ~5 行，总改造量 < 15 行。框架层（EnumAllCommonRoots 及其调用的所有子方法）完全复用，无需任何修改。

---

## 10. 验证方案

### 10.1 验证策略总览

```
Level 1: 单元测试 — 验证各组件独立正确性
Level 2: 集成测试 — 验证完整 GC 周期
Level 3: 压力测试 — 验证并发安全和内存正确性
Level 4: 对比测试 — 与 WCollector 对比性能
Level 5: Sanitizer — ASAN/TSAN 检测内存/并发错误
```

### 10.2 Level 1: 单元测试

| 测试项 | 验证内容 | 方法 |
|--------|---------|------|
| ClearAllMarkBitmaps | 所有 Region bitmap 清零 | 创建 Region → 标记若干位 → Clear → 验证全 0 |
| SweepNormalRegion | 死亡对象→FreeList，存活对象不受影响 | 创建 Region → 分配对象 → 标记部分 → Sweep → 验证 FreeList |
| SweepLargeRegion | 未标记大对象 Region 归还 | 创建大 Region → 不标记 → Sweep → 验证归还 |
| MSTraceBarrier | SATB 记录旧值 | 安装屏障 → 写引用 → 检查 SATB buffer 是否记录旧值 |
| MarkNewObject | 标记期间新分配对象为黑色 | 设 Phase=MARK → Allocate → 验证 bitmap 位为 1 |
| WorkStack split | 工作窃取正确性 | 填充 WorkStack → split → 验证两半之和 = 原始 |

### 10.3 Level 2: 集成测试

使用 `gc/gc.cj` 测试用例验证完整 GC 周期：

```bash
# 编译 Runtime → 替换 → 编译测试 → 运行
cd /Users/lipeisheng/code/cangjie/cangjie/cangjie_runtime/runtime && \
python3 build.py build && \
cp output/common/darwin_release_aarch64/runtime/lib/darwin_aarch64_cjnative/libcangjie-runtime.dylib \
   /Users/lipeisheng/code/cangjie/cangjie-pkg/dev/20260220/cangjie/runtime/lib/darwin_aarch64_cjnative/libcangjie-runtime.dylib && \
source /Users/lipeisheng/code/cangjie/cangjie-pkg/dev/20260220/cangjie/envsetup.sh && \
cd gc && cjc gc.cj && ./main
```

**集成测试场景**：

| 场景 | 测试文件 | 验证要点 |
|------|---------|---------|
| 基本分配+回收 | `gc_minimal.cj` | 分配→GC→验证对象存活/死亡正确 |
| 大量分配 | `gc_large.cj` | 多次 GC 周期，内存不泄漏 |
| 混合对象大小 | `gc_mixed.cj` | 小对象+大对象+超大对象混合 |
| 弱引用 | 新增测试 | WeakRef 在 referent 死亡后被清除 |
| Finalizer | 新增测试 | 不可达对象的 Finalizer 被调用 |
| 并发分配+GC | 新增测试 | 多线程同时分配 + GC，无崩溃 |

### 10.4 Level 3: 压力测试

```cj
// 并发压力测试伪代码 (cangjie)
main() {
    // 启动 N 个并发协程，持续分配和释放
    for (i in 0..16) {
        spawn {
            for (j in 0..100000) {
                let arr = Array<Int64>(Random.next(10, 10000), item: 0)
                // 偶尔保持长期引用
                if (j % 1000 == 0) {
                    longLived.add(arr)
                }
            }
        }
    }
    // 等待完成 → 验证 longLived 对象未被误回收
}
```

### 10.5 Level 4: 对比测试

| 指标 | 测试方法 | 预期 |
|------|---------|------|
| GC 暂停时间 | 测量 STW 阶段耗时 | Mark-Sweep STW < WCollector 总 STW |
| 吞吐量 | 固定分配量，测量总时间 | 第一版可能略慢于 WCollector（Sweep 开销） |
| 内存占用 | 测量 RSS | Mark-Sweep 无需 to-space，堆占用↓50% |
| GC 频率 | 统计 GC 次数 | GOGC 控制下应更合理 |
| 碎片率 | 测量 FreeList 碎片 | 长期运行后监控碎片趋势 |

### 10.6 Level 5: Sanitizer 验证

```bash
# ASAN：检测 use-after-free、buffer-overflow
python3 build.py build --target native --build-type debug -v 0.0.1 --cjlib-sanitizer-support asan

# TSAN：检测数据竞争（并发标记/Sweep 期间）
python3 build.py build --target native --build-type debug -v 0.0.1 --cjlib-sanitizer-support tsan
```

重点关注的并发安全点：
1. `RegionBitmap::MarkBits()` 的 CAS 操作 — 多线程并发标记
2. `WorkStack` 的 push/pop/split — GC 线程间共享
3. `SatbBuffer` 的 Push/Retire — Mutator 与 GC 线程交互
4. `sweepIndex_` 的 fetch_add — Sweep 工作窃取
5. `FreeList` 的并发访问 — Sweep 归还 + Mutator 分配

### 10.7 回归测试

在 Mark-Sweep 开发期间，WCollector **必须保持可用**：

```cpp
// 通过配置切换 GC 策略
// RuntimeConfig.h 或环境变量
enum GCStrategy {
    GC_COPY = 0,       // 现有 WCollector（默认）
    GC_MARK_SWEEP = 1, // 新 MarkSweepCollector
};

// HeapManager 根据配置创建对应 Collector
if (config.gcStrategy == GC_MARK_SWEEP) {
    collector_ = new MarkSweepCollector(...);
} else {
    collector_ = new WCollector(...);
}
```

确保每次提交都通过 WCollector 的完整回归测试，不引入回归。

### 10.8 验证里程碑

| 阶段 | 里程碑 | 验证标准 |
|------|--------|---------|
| M1 | MarkSweepCollector 编译通过 | `python3 build.py build` 成功 |
| M2 | 单个 GC 周期正确 | `gc_minimal.cj` 运行通过 |
| M3 | 多轮 GC 周期正确 | `gc_large.cj` + `gc_mixed.cj` 通过 |
| M4 | 并发安全 | TSAN 无数据竞争报告 |
| M5 | 内存安全 | ASAN 无 use-after-free 报告 |
| M6 | 性能达标 | 对比测试各指标在可接受范围内 |
