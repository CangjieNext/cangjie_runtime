# Step 2：三色标记详细设计 —— 替换 Forward 为 Mark-Sweep

> 本文档是 [Go 风格 GC 迁移方案](Go风格GC迁移方案.md) Step 2 的独立详细设计文档。
> 目标：用 Go 风格的并发三色标记替换现有的 Trace + Forward（复制式）回收流程。
> Sweep 阶段设计见 Step 3。

---

## 目录

1. [现有 GC 流程深度分析](#1-现有-gc-流程深度分析)
2. [三色标记抽象与实现映射](#2-三色标记抽象与实现映射)
3. [MarkSweepCollector 总体设计](#3-marksweecollector-总体设计)
4. [Phase 1: Mark Setup（Handshake）](#4-phase-1-mark-setuphandshake) → [Step 2.1](Step2.1_MarkSetup_STW详细设计.md)
5. [Phase 2: Concurrent Mark（并发标记）](#5-phase-2-concurrent-mark并发标记) → [Step 2.2](Step2.2_ConcurrentMark_详细设计.md)
6. [Phase 3: Mark Termination（STW）](#6-phase-3-mark-terminationstw) → [Step 2.3](Step2.3_MarkTermination_STW详细设计.md)
7. [Phase 4: Post Mark Termination](#7-phase-4-post-mark-termination) → [Step 2.4](Step2.4_PostMarkTermination_详细设计.md)
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
               GC 结束：仍为白色 → 垃圾 → 回收 ────────┘
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
  └── MSBarrier.h                 // Mark-Sweep 专用屏障
```

### 3.3 DoGarbageCollection 新流程

```cpp
void MarkSweepCollector::DoGarbageCollection() {
    // ===== Phase 1: Mark Setup (Handshake) =====
    MarkSetup();              // → Step 2.1
    
    // ===== Phase 2: Concurrent Mark (并发) =====
    ConcurrentMark();         // → Step 2.2
    
    // ===== Phase 3: Mark Termination (STW) =====
    MarkTermination();        // → Step 2.3
    
    // ===== Phase 4: Post Mark Termination =====
    PostMarkTermination();    // → Step 2.4
    
    // ===== 后续阶段（见 Step 3）=====
}
```

**与 WCollector 流程对比**：

| 阶段 | WCollector | MarkSweepCollector |
|------|------------|-------------------|
| 标记准备 | TraceHeap() (ENUM+TRACE) | MarkSetup() (Handshake) |
| 并发标记 | DoTracing() | ConcurrentMark() |
| 标记收尾 | PostTrace() (STW) | MarkTermination() (STW) |
| 标记后清理 | PostTrace() 后半 | PostMarkTermination() |
| 对象复制 | Preforward() + ForwardFromSpace() | **无（不移动对象）** |
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
    
    // === 标记统计 (新增) ===
    size_t bytesMarked_{0};                      // 本轮标记的字节数
    
    // === GC 触发相关 (新增) ===
    size_t gcTriggerBytes_{0};                   // 下次 GC 触发阈值
    double gogcPercent_{100.0};                  // GOGC 比例（默认 100%）
};
```

### 3.5 与 Go runtime 的对应关系

| Go 概念 | MarkSweepCollector 对应 |
|---------|------------------------|
| `gcStart()` | `DoGarbageCollection()` 入口 |
| `gcBgMarkStartWorkers` | 复用 `GCThreadPool` + `ConcurrentMarkingWork` |
| `gcMarkDone()` | `MarkTermination()` |
| `gcWork` (per-P 灰色队列) | `WorkStack` (split 实现工作窃取) |
| `gcController.trigger` | `gcTriggerBytes_` |
| `GOGC` 环境变量 | `gogcPercent_` 配置 |

---

## 4. Phase 1: Mark Setup（Handshake）

> **详细设计 → [Step 2.1：Mark Setup（Handshake）详细设计](Step2.1_MarkSetup_STW详细设计.md)**

**概述**：通过 Handshake 机制（复用 `TransitionToGCPhase`）完成标记初始化，每个 Mutator 在 safepoint 自行转换，无需全局暂停。核心步骤：

1. **确保上一轮回收完成** — 等待或协助完成
2. **RetireAllThreadLocalRegions** — 将所有线程私有 TL Region 回收到中央池（必须在 ClearBitmap 之前）
3. **ClearAllMarkBitmaps** — 两阶段清除：中央池 Bitmap 由 GC 线程批量清除，TL Bitmap 由各 Mutator 在 Handshake 中自行清除
4. **安装 SATB 删除屏障** — 通过 `TransitionToGCPhase(MS_MARK)` 完成
5. **EnumAndMarkRoots** — GC 线程枚举全局根 + Mutator 自扫描栈根
6. **EnableMarkNewObject** — 并发分配期间新对象标黑
7. **sweepGen + gcEpoch** — 递增代际编号，为后续阶段提供 Region 纪元判定

与现有 WCollector ENUM 阶段的关键差异：无 from-space 选择、Handshake 替代 STW、增加 TL Region 回收和 sweepGen 代际管理。

---

## 5. Phase 2: Concurrent Mark（并发标记）

> **详细设计 → [Step 2.2：Concurrent Mark（并发标记）详细设计](Step2.2_ConcurrentMark_详细设计.md)**

**概述**：并发标记阶段复用现有 `TracingCollector::DoTracing` 框架（并行任务、工作窃取、SATB 重标记），
在 `MarkSweepCollector` 中通过 override 相关 virtual 方法去除 CopyGC 的 tag/forward/fix-ref 逻辑：

1. **并发标记主循环**：复用 `ConcurrentMarkingWork::Execute`，GC 主线程 + 辅助线程共同推进
2. **热路径简化**：`TraceRefField` 去除 `FindLatestVersion` 和 CAS 更新引用
3. **Mark Assist**：Mutator 按分配债务（assist debt）参与标记，控制分配速率和内存增长
4. **新分配对象**：复用 `MarkNewObject`，标记期分配即标黑
5. **Region 纪元管理**：采用 `sweepGen + gcEpoch` 机制管理并发标记期 Region 获取
6. **收敛判定**：全局队列 + 所有局部队列为空，连续 N 轮稳定 → 进入 MarkTermination

---

## 6. Phase 3: Mark Termination（STW）

> **详细设计 → [Step 2.3：Mark Termination（STW）详细设计](Step2.3_MarkTermination_STW详细设计.md)**

**概述**：STW-first 收敛策略，在短 STW 窗口内将并发标记的"尾巴"收敛到一致状态：

1. **收集未标记候选到主队列**：SATB 残留、Extern 对象、Finalizer 复活链
2. **Execute 同构循环补标**：`MarkObject` + `TraceObjectRefFields` + WeakRef 分支
3. **切换到下一阶段**：`TransitionToGCPhase(MS_PHASE_POST_MARK)` 卸载标记屏障

`MarkSatbBuffer` 采用"核心逻辑复用 + MS 专用封装"，在 STW 内串行收敛，不依赖线程池。

---

## 7. Phase 4: Post Mark Termination

> **详细设计 → [Step 2.4：Post Mark Termination 详细设计](Step2.4_PostMarkTermination_详细设计.md)**

**概述**：MarkTermination（STW）结束后，mark bitmap 已稳定，mutator 已恢复运行。本阶段负责标记结束后的清理和准备工作，不需要 STW 保护：

| 序号 | 任务 | 说明 |
|------|------|------|
| 1 | `ClearWeakRefBuffer()` | 遍历 WeakRefBuffer，referent 未标记则置空 |
| 2 | `ClearSatbBuffer()` | 释放 SATB 缓冲区内存 |
| 3 | `ProcessFinalizers()` | 将未标记的 finalizer 对象入队，通知 Finalizer 线程执行析构 |
| 4 | `PrepareCycleRef()` | OHOS 专用：合并 discoveredExternObjects 到 cycleRefWorkStack |
| 5 | `PostResolveCycleTask()` | OHOS 专用：兜底触发 CJ_MRT_RolveCycleRef |
| 6 | `CollectLargeGarbage()` | 回收大对象 region：≤ 4MB 插入 freeManager，> 4MB 直接释放物理页 |

所有操作均基于已稳定的 mark bitmap，通过原子操作或独占访问保证并发安全。

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
    MS_PHASE_MARK_SETUP        = 21,   // Handshake: 安装屏障 + 根枚举
    MS_PHASE_MARK              = 22,   // 并发标记
    MS_PHASE_MARK_TERMINATION  = 23,   // STW: SATB 清理 + 弱引用 + Finalizer
    MS_PHASE_POST_MARK         = 24,   // 非 STW: 标记后清理
};
```

### 8.3 Phase 状态转换图

```
     ┌────────────────────────────────────────────────────────────┐
     │                                                            │
     ▼                                                            │
  MS_PHASE_IDLE ──[GC 触发]──→ MS_PHASE_MARK_SETUP (Handshake) │
                                     │                          │
                                     ▼                          │
                              MS_PHASE_MARK (并发)              │
                                     │                          │
                                     ▼                          │
                              MS_PHASE_MARK_TERMINATION (STW)   │
                                     │                          │
                                     ▼                          │
                              MS_PHASE_POST_MARK (非 STW)       │
                                     │                          │
                                     └──────────────────────────┘
```

### 8.4 各 Phase 对应的 Barrier 类

| Phase | Barrier 类 | 作用 |
|-------|-----------|------|
| `MS_PHASE_IDLE` | `IdleBarrier` | 无屏障，普通读写 |
| `MS_PHASE_MARK_SETUP` | `MSTraceBarrier` | SATB 删除屏障（MarkSetup 末尾安装） |
| `MS_PHASE_MARK` | `MSTraceBarrier` | SATB 删除屏障（写引用前保存旧值） |
| `MS_PHASE_MARK_TERMINATION` | `MSTraceBarrier` | 保持屏障直到 MarkTermination 完成 |
| `MS_PHASE_POST_MARK` | `IdleBarrier` | 卸载屏障（标记已完成） |

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
| `MS_PHASE_POST_MARK` | 普通分配（已卸载标记 hook） |

**关键**：在 MARK_SETUP / MARK / MARK_TERMINATION 三个阶段，新分配的对象**必须标记为黑色**，否则可能被误回收。

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
| `src/Heap/MarkSweepCollector/MarkSweepCollector.cpp` | GC 主流程 (MarkSetup + ConcurrentMark + MarkTermination + PostMarkTermination) |
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
| P0 | MSTraceBarrier | 低 | 简化版 TraceBarrier |
| P0 | MSConcurrentMarkingWork | 低 | 简化版 ConcurrentMarkingWork |
| P1 | EnumAllCommonRoots 改造 | 中 | 可能影响 WCollector 回归 |
| P1 | CollectorProxy 切换支持 | 低 | 添加新分支 |
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
   /Users/lipeisheng/code/cangjie/cangjie-pkg/self/dev260313/cangjie/runtime/lib/darwin_aarch64_cjnative/libcangjie-runtime.dylib && \
source /Users/lipeisheng/code/cangjie/cangjie-pkg/self/dev260313/cangjie/envsetup.sh && \
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
| 吞吐量 | 固定分配量，测量总时间 | 第一版可能略慢于 WCollector |
| 内存占用 | 测量 RSS | Mark-Sweep 无需 to-space，堆占用↓50% |
| GC 频率 | 统计 GC 次数 | GOGC 控制下应更合理 |
| 碎片率 | 测量 FreeList 碎片 | 长期运行后监控碎片趋势 |

### 10.6 Level 5: Sanitizer 验证

```bash
# ASAN：检测 use-after-free、buffer-overflow
python3 build.py build --target native --build-type debug -v 0.0.1 --cjlib-sanitizer-support asan

# TSAN：检测数据竞争（并发标记期间）
python3 build.py build --target native --build-type debug -v 0.0.1 --cjlib-sanitizer-support tsan
```

重点关注的并发安全点：
1. `RegionBitmap::MarkBits()` 的 CAS 操作 — 多线程并发标记
2. `WorkStack` 的 push/pop/split — GC 线程间共享
3. `SatbBuffer` 的 Push/Retire — Mutator 与 GC 线程交互

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
