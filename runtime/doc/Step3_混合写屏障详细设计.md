# Step 3：混合写屏障详细设计 —— 从 SATB+读屏障到 Go 风格 Hybrid Write Barrier

> 本文档是 [Go 风格 GC 迁移方案](Go风格GC迁移方案.md) Step 3 的独立详细设计文档。
> 目标：用 Go 的混合写屏障（Hybrid Write Barrier）替换当前的 SATB 写屏障 + 6 套读写屏障体系，实现**无读屏障**的并发标记。

---

## 目录

1. [现有屏障系统深度分析](#1-现有屏障系统深度分析)
2. [Go 混合写屏障原理](#2-go-混合写屏障原理)
3. [混合写屏障与 SATB 的理论对比](#3-混合写屏障与-satb-的理论对比)
4. [MSBarrier 总体设计](#4-msbarrier-总体设计)
5. [MSBarrier 详细实现 → Step3.1](#5-msbarrier-详细实现)
6. [WriteBarrierBuffer 设计 → Step3.2](#6-writebarrierbuffer-设计)
7. [读屏障、Tagged Pointer 移除与 CompilerCalls → Step3.3](#7-读屏障tagged-pointer-移除与-compilercalls)
8. [对 Step2 各子阶段的具体改造 → Step3.4](#8-对-step2-各子阶段的具体改造)
9. [屏障安装与 Phase 联动](#9-屏障安装与-phase-联动)
10. [与现有基础设施的复用与改造清单](#10-与现有基础设施的复用与改造清单)
11. [验证方案](#11-验证方案)

---

## 1. 现有屏障系统深度分析

### 1.1 Barrier 类层次

```
Barrier (基类, src/Heap/Barrier/Barrier.h L18)
  ├── 虚方法: ReadReference / ReadStaticRef / ReadWeakRef / ReadStruct / ReadStaticStruct
  ├── 虚方法: WriteReference / WriteStaticRef / WriteStruct / WriteStaticStruct
  ├── 虚方法: CopyRefArray / CopyStructArray
  ├── 虚方法: AtomicReadReference / AtomicWriteReference / AtomicSwapReference / CompareAndSwapReference
  └── 虚方法: WriteGeneric / ReadGeneric
```

**6 个具体 Barrier 实现**（全部位于 `src/Heap/WCollector/`）：

```
Barrier (STW 基类)
  └── IdleBarrier         (GC_PHASE_IDLE)
        ├── EnumBarrier       (GC_PHASE_ENUM)
        ├── TraceBarrier      (GC_PHASE_TRACE / GC_PHASE_CLEAR_SATB_BUFFER)
        ├── PostTraceBarrier  (GC_PHASE_POST_TRACE)
        ├── PreforwardBarrier (GC_PHASE_PREFORWARD)
        └── ForwardBarrier    (GC_PHASE_FORWARD)
```

### 1.2 各阶段屏障行为对照

| Phase | Barrier | 读屏障 | 写屏障 | 说明 |
|-------|---------|--------|--------|------|
| IDLE | IdleBarrier | TryUpdate/TryUntag（处理 tagged ptr） | 直接写（无 SATB） | GC 不活跃 |
| ENUM | EnumBarrier | 同 Idle + tag 处理 | **SATB 记旧值+新值** → GetAndTryTagRefField | 根枚举期间，新旧值都记录防漏标 |
| TRACE | TraceBarrier | 同 Idle + tag 处理 | **SATB 仅记旧值** → GetAndTryTagRefField | 经典 SATB 删除屏障 |
| CLEAR_SATB | TraceBarrier | 同上 | 同上 | SATB 清理期复用 TraceBarrier |
| POST_TRACE | PostTraceBarrier | 同 Idle + tag 处理 | SATB 记旧值 + tag 写入 | 标记后弱引用处理期 |
| PREFORWARD | PreforwardBarrier | forward + tag 处理 | forward 旧值 + 直接写 | 预转发根修正 |
| FORWARD | ForwardBarrier | **循环 TryForwardRefField**（重） | 直接写（无 SATB） | 并发复制核心开销 |

### 1.3 读屏障详细分析

**当前读屏障是所有 Phase 都存在的**（即使 IDLE 阶段也有 tag 处理开销）。

以 `IdleBarrier::ReadReference`（IdleBarrier.cpp L20-L43）为例：

```cpp
BaseObject* IdleBarrier::ReadReference(BaseObject* obj, RefField<false>& field) const
{
    while (true) {
        RefField<> tmpField(field);                      // 原子读取
        if (!tmpField.IsTagged()) {                      // 检查 tag 位
            return tmpField.GetTargetObject();            // 无 tag → 直接返回
        }
        if (theCollector.IsCurrentPointer(tmpField)) {   // 当前周期 tag → untag
            if (theCollector.TryUntagRefField(obj, field, tmpField)) {
                return tmpField.GetAddress();
            }
        } else {                                          // 旧周期 tag → forward
            BaseObject* toVersion = nullptr;
            if (theCollector.TryUpdateRefField(obj, tmpField, toVersion)) {
                return toVersion;
            }
        }
        // CAS 失败 → 重试
    }
}
```

**关键问题**：
- 每次读引用都需要检查 `IsTagged()`，即使 GC 空闲期也如此
- Forward 阶段的读屏障更重：循环 `TryForwardRefField`，包含对象复制和 CAS
- 这是 Copy GC 最主要的 mutator 开销来源

### 1.4 写屏障详细分析

以 `TraceBarrier::WriteReference`（TraceBarrier.cpp L107-L130）为例：

```cpp
void TraceBarrier::WriteReference(BaseObject* obj, RefField<false>& field, BaseObject* ref) const
{
    RefField<> tmpField(field);
    BaseObject* rememberedObject = nullptr;
    
    // 1. 读取旧值（可能需要 forward）
    if (theCollector.IsOldPointer(tmpField)) {
        BaseObject* toVersion = nullptr;
        if (theCollector.TryUpdateRefField(obj, tmpField, toVersion)) {
            rememberedObject = toVersion;
        } else {
            rememberedObject = field.GetTargetObject();
        }
    } else {
        rememberedObject = tmpField.GetTargetObject();
    }
    
    // 2. SATB: 记录旧值到 SatbBuffer
    Mutator* mutator = Mutator::GetMutator();
    if (rememberedObject != nullptr) {
        mutator->RememberObjectInSatbBuffer(rememberedObject);
    }
    
    // 3. 打 tag + 写入
    std::atomic_thread_fence(std::memory_order_seq_cst);
    RefField<> newField = theCollector.GetAndTryTagRefField(ref);
    field.SetFieldValue(newField.GetFieldValue());
}
```

**写屏障同样需要处理 tagged pointer**：先解 tag 拿旧值，再打 tag 写新值，额外增加了 `IsOldPointer`、`TryUpdateRefField`、`GetAndTryTagRefField` 等调用。

### 1.5 SatbBuffer 架构

```
SatbBuffer (全局单例, src/Mutator/SatbBuffer.h)
  ├── freeNodes: LockedList<Node>      — 空闲节点池
  ├── retiredNodes: LockedList<Node>   — 已满待扫描节点
  └── arena: LockedList<Page>          — 内存页池

每个 Mutator 持有一个 satbNode (SatbBuffer::Node*)
  └── Node: 固定大小 buffer (64~69 个对象指针)
```

**SATB 工作流**：
```
写屏障触发 → mutator->RememberObjectInSatbBuffer(oldVal)
  → Heap::IsHeapAddress(obj)?
  → SatbBuffer::ShouldEnqueue(obj)?
  → EnsureGoodNode(satbNode)   // node 满则 retire 到全局
  → satbNode->Push(obj)        // 写入线程本地 node
```

**GC 处理**：
```
MarkTermination → SatbBuffer::GetRetiredObjects(workStack)
               → 遍历所有记录的对象 → 标记
```

### 1.6 现有屏障系统的核心问题

| 问题 | 描述 | 影响 |
|------|------|------|
| **读屏障开销** | 所有 Phase 都有读屏障（tag 检查 + 可能的 CAS） | Mutator 吞吐下降 10-30% |
| **6 套屏障** | 每个 Phase 一套独立实现，逻辑重复 | 维护成本高，容易出 bug |
| **Tagged Pointer** | 指针低位编码 tag，所有操作都需解码 | 增加代码复杂度和 CPU 开销 |
| **seq_cst fence** | 写屏障中频繁使用 `atomic_thread_fence(seq_cst)` | 严重影响多核可扩展性 |
| **Forward 阶段** | ForwardBarrier 读屏障包含对象复制和 CAS | 极重的 hot path 开销 |

### 1.7 屏障安装机制

```cpp
// src/Heap/Heap.cpp L189
void HeapImpl::InstallBarrier(const GCPhase phase) {
    if      (phase == GC_PHASE_ENUM)                         → &enumBarrier
    else if (phase == GC_PHASE_TRACE || CLEAR_SATB_BUFFER)   → &traceBarrier
    else if (phase == GC_PHASE_PREFORWARD)                   → &preforwardBarrier
    else if (phase == GC_PHASE_FORWARD)                      → &forwardBarrier
    else if (phase == GC_PHASE_IDLE)                         → &idleBarrier
    else if (phase == GC_PHASE_POST_TRACE)                   → &postTraceBarrier
}
```

通过二级指针实现 O(1) 查找：
```cpp
// src/Heap/Heap.h
static Barrier& GetBarrier() { return **currentBarrierPtr; }
// currentBarrierPtr → HeapImpl::currentBarrier → 具体 Barrier 实例
```

---

## 2. Go 混合写屏障原理

### 2.1 经典写屏障回顾

在并发三色标记中，写屏障用于维护**三色不变性**（Tri-color Invariant），防止并发标记期间发生漏标。

两种经典写屏障：

**Dijkstra 插入屏障**（标灰新值）：
```
writePointer(slot, ptr):
    shade(ptr)    // 新值标灰：确保白色对象不被黑色对象引用而漏标
    *slot = ptr
```
- 优点：简单直接
- 缺点：栈上也需要屏障（栈写频繁，开销大），或者需要在 Mark Termination 时重扫所有栈

**Yuasa 删除屏障**（标灰旧值，SATB）：
```
writePointer(slot, ptr):
    shade(*slot)  // 旧值标灰：对象被删除引用时保活，防止被连接到黑色对象后漏标
    *slot = ptr
```
- 优点：不需要栈屏障（已有对象的断开引用会被保护）
- 缺点：更多浮动垃圾（已死对象被保护到下一轮）

### 2.2 Go 1.8 混合写屏障

Go 1.8 引入的混合写屏障（Hybrid Write Barrier）结合了两种经典屏障的优势：

```
writePointer(slot, ptr):
    shade(*slot)   // ① Yuasa 删除屏障：旧值标灰（防止已引用对象被漏标）
    if current stack has been scanned:
        shade(ptr) // ② Dijkstra 插入屏障：新值标灰（仅对已扫描的栈）
    *slot = ptr    // ③ 实际写入
```

### 2.3 为什么栈不需要屏障

这是混合写屏障最关键的优化：

1. **Mark Setup STW 阶段会扫描所有栈**：此时栈上所有可达对象都被标灰
2. **栈上新写入的指针来源只有两种**：
   - **来自堆**：堆上的写操作有写屏障保护
   - **来自栈上已有引用**：这些引用在栈扫描时已被标记
3. 因此，**栈上的写操作不可能引入未被保护的白色对象**

**Go 的进一步简化**：在 Cangjie Runtime 中，我们不区分"栈是否已扫描"（因为 Mark Setup STW 会一次性扫描所有栈），因此**堆上的写一律同时标灰旧值和新值**：

```
writePointer(slot, ptr):    // 仅对堆上的写
    shade(*slot)             // 旧值标灰
    shade(ptr)               // 新值标灰
    *slot = ptr
```

### 2.4 混合写屏障的正确性论证

**定理**：在以下条件下，混合写屏障保证不会漏标任何存活对象：

1. Mark Setup STW 期间，所有 goroutine/thread 的栈被完整扫描（根被标灰）
2. 堆上每次引用写入，旧值和新值都被标灰
3. 新分配的对象直接标为黑色（分配即标记）

**证明思路**：
- 假设存活对象 X 在 Mark Termination 时仍为白色（未标记）
- X 存活 → 从根可达 → 存在引用链 root → ... → Y → X
- Y 必已被标记（因为根已全部标灰，且标记会传播到所有可达对象）
- Y 持有对 X 的引用 → 这个引用要么在标记开始时就存在，要么在标记期间写入
  - 若标记开始时存在：Y 被标记时会遍历其引用字段，X 会被标灰 → 矛盾
  - 若标记期间写入：写屏障会将 X（新值）标灰 → 矛盾
- 因此假设不成立，不存在漏标 ∎

### 2.5 Go 混合写屏障在 Cangjie 中的适配

| 维度 | Go 原版 | Cangjie 适配 |
|------|---------|-------------|
| 栈扫描 | per-goroutine 逐个扫描 | Mark Setup STW 扫描所有线程栈 |
| 栈是否需要屏障 | 不需要 | **不需要**（同 Go，编译器可优化） |
| 堆写屏障 | shade(old) + 条件 shade(new) | shade(old) + **无条件 shade(new)** |
| 无条件 shade(new) 的代价 | 略多浮动垃圾 | 可接受：简化实现，避免 per-thread 扫描状态判断 |
| 新分配对象 | 标黑（allocBlack） | 标黑（与 Step2 §5.6 一致） |
| 写屏障开关 | `writeBarrier.enabled` 全局 flag | Phase-based barrier install |

---

## 3. 混合写屏障与 SATB 的理论对比

### 3.1 机制对比

| 维度 | 当前 SATB（Cangjie） | Go 混合写屏障（目标） |
|------|----------------------|----------------------|
| **屏障类型** | 写屏障（SATB 记旧值）+ 读屏障 | **仅写屏障**（记旧值+新值），无读屏障 |
| **读操作** | 需要 tag 检查 + 可能 CAS forward | **直接 load，零开销** |
| **写操作** | 记旧值 → 入 SatbBuffer → 打 tag 写入 | 标灰旧值 → 标灰新值 → 直接写入 |
| **buffer 机制** | SatbBuffer（per-thread Node，满退 retire） | WriteBarrierBuffer（per-thread，满时批量 flush） |
| **屏障套数** | 6 套（IDLE/ENUM/TRACE/POST/PRE/FWD） | **2 套**（Off / On） |
| **Tagged Pointer** | 需要（指针低位编码 tag） | **不需要**（原始指针） |
| **seq_cst fence** | 每次写屏障都有 | **不需要**（relaxed 或 release 即可） |
| **Forward 阶段开销** | 极重（读屏障触发对象拷贝） | **无**（MS 不移动对象） |

### 3.2 性能影响预估

**读操作**（热路径，频率远高于写）：
- 当前：每次 `ReadReference` 至少 1 次 `IsTagged()` 检查，Forward 阶段更重
- 目标：直接 load，**零额外指令**
- **预估提升**：mutator 读密集型工作负载吞吐提升 **15-30%**

**写操作**：
- 当前：SATB（record old） + tagged pointer encode/decode + seq_cst fence
- 目标：shade(old) + shade(new) → 写入
- **预估变化**：每次写操作多一次 shade(new)，但省去了 tag 处理和 seq_cst fence
- 净效果：写操作开销基本持平或略降

**整体**：
- 删除读屏障 → 大幅降低 mutator 热路径开销
- 删除 Forward 阶段 → 消除最重的 mutator 同步窗口
- 减少 barrier 套数 → 降低代码复杂度和 phase 切换开销

### 3.3 浮动垃圾影响

混合写屏障记录新值（shade(new)）会比纯 SATB 产生更多浮动垃圾：

- **SATB 仅 shade(old)**：只有在标记开始时已经存在引用的对象被保留
- **混合 shade(old)+shade(new)**：标记期间被写入引用的对象也被保留

**预估**：浮动垃圾增加 5-15%，但在下一轮 GC 中会被清理，整体内存占用影响可控。

### 3.4 SATB Buffer vs Write Barrier Buffer

| 维度 | 现有 SatbBuffer | 新 WriteBarrierBuffer |
|------|----------------|----------------------|
| **存储内容** | 旧值指针 | 旧值 + 新值指针 |
| **per-thread 大小** | ~512 bytes (64 pointers) | 2KB (256 pointers) |
| **满时行为** | retire 到全局 retiredNodes → GC 线程异步处理 | flush 到全局 work stack → GC 线程处理 |
| **分配方式** | SatbBuffer::Node（从 arena 分配） | 内嵌在 Mutator/AllocBuffer 中（栈式 buffer） |
| **清理时机** | MarkTermination STW 中统一清理 | MarkTermination STW 中统一清理 |

---

## 4. MSBarrier 总体设计

### 4.1 设计目标

1. **仅 2 套屏障**：Off（GC 空闲）和 On（GC 标记中），替代现有 6 套
2. **零读屏障**：读操作直接 load，不做任何检查
3. **无 Tagged Pointer**：指针就是地址，不含任何元数据
4. **GC 安全**：混合写屏障保证并发标记期间不漏标

### 4.2 新类继承关系

```
Barrier (基类, 不变)
  ├── MSIdleBarrier     (GC 空闲期 — 所有操作直接读写，零开销)
  └── MSWriteBarrier    (GC Mark 期 — 写操作标灰旧值+新值，读操作直接返回)
```

对比现有的 6 个 Barrier 子类，简化到 **2 个**。

### 4.3 新文件结构

```
src/Heap/MSBarrier/
├── MSIdleBarrier.h         — GC 空闲期屏障声明
├── MSIdleBarrier.cpp       — GC 空闲期屏障实现
├── MSWriteBarrier.h        — 混合写屏障声明
├── MSWriteBarrier.cpp      — 混合写屏障实现
└── WriteBarrierBuffer.h    — Per-thread 写屏障缓冲区
```

> **详细代码设计**（MSIdleBarrier 完整实现、Barrier 基类兼容性分析）见 → [Step3.1 MSBarrier 详细实现](Step3.1_MSBarrier详细实现.md) §1-§2

---

## 5. MSBarrier 详细实现

> **详细设计文档** → [Step3.1_MSBarrier详细实现.md](Step3.1_MSBarrier详细实现.md)

MSWriteBarrier 在 GC Mark 阶段（`MS_PHASE_MARK_SETUP` → `MS_PHASE_MARK` → `MS_PHASE_MARK_TERMINATION`）生效，核心是 `MarkAndEnqueue` 机制：

- **写操作**：shade(old) + shade(new) — 立即原子标记（bitmap CAS）+ 推入 per-thread WBBuf
- **读操作**：直接 load，零开销（与 MSIdleBarrier 相同）

主要方法：
| 方法 | 行为 |
|------|------|
| `WriteReference` | shade(old) + shade(new) → release fence → store |
| `WriteStaticRef` | 同上（注意：现有 TraceBarrier 不对静态引用记 SATB，MS 须修正） |
| `WriteStruct` | 遍历 old refs shade → memcpy → 遍历 new refs shade |
| `CopyRefArray` | 逐元素 shade old + shade new → bulk memcpy |
| `AtomicSwapReference` | shade(new) → exchange → shade(返回的 old) |
| `CompareAndSwapReference` | shade(old) + shade(new)（无论 CAS 结果） |
| 所有 Read* | `field.GetTargetObject()` — 直接返回 |

> **各方法完整伪代码**（MarkAndEnqueue、WriteReference、WriteStaticRef、WriteStruct、CopyRefArray、原子操作、读操作）详见 → [Step3.1 MSBarrier 详细实现](Step3.1_MSBarrier详细实现.md) §3-§10

---

## 6. WriteBarrierBuffer 设计

> **详细设计文档** → [Step3.2_WriteBarrierBuffer详细设计.md](Step3.2_WriteBarrierBuffer详细设计.md)

为避免每次写屏障都竞争全局 WorkStack 锁，使用 per-thread 的 `WriteBarrierBuffer`（WBBuf）：

- **容量**：256 entries（2KB），内嵌在 `Mutator` 中，无动态内存分配
- **Push 路径**：1 次数组写 + 1 次边界检查，约 2 条指令（无锁）
- **Flush 目标**：优先写入 per-P `localWorkBuf`（与 Step2.2 并发标记工作队列集成），无 Processor 绑定时 fallback 到全局 WorkStack
- **生命周期**：Mutator 创建时自动初始化 → 写屏障 Push → Mark Termination STW 强制 FlushAll → GC 结束 ClearAll → Mutator 退出时析构 flush

与现有 `SatbBuffer::Node` 的关键区别：SATB buffer 需要全局 arena 分配、retire 到链表、Mark Termination 统一遍历；WBBuf 内嵌 Mutator、flush 实时进入工作队列、GC 线程实时处理。

> Step3.2 还包含：与 Step2.2 per-P 工作队列的详细集成设计、Mark Assist 交互机制、性能对比分析。

---

## 7. 读屏障、Tagged Pointer 移除与 CompilerCalls

> **详细设计文档** → [Step3.3_读屏障与TaggedPointer移除.md](Step3.3_读屏障与TaggedPointer移除.md)

本节将三个紧密关联的"移除旧机制"任务合并设计：

### 7.1 读屏障完全移除

Mark-Sweep 中对象不移动，不存在 from-space / to-space 概念，因此所有读操作可降级为直接 load（零开销）。涉及 7 个 CompilerCalls 读入口（`CJ_MCC_ReadRefField` 等）。WeakRef 读操作同样不需要屏障，referent 清理由 Sweep 阶段负责。

### 7.2 Tagged Pointer 移除

现有 RefField 使用高位 bit（`tagID` + `isTagged`）标记 GC 转发状态。MS 模式下无需 forward，`RefField` 简化为裸指针：`GetTargetObject()` = 直接 load，`SetTargetObject()` = 直接 store。通过 `#ifdef MRT_USE_MARK_SWEEP` 条件编译，保留 Copy GC 兼容。需清理的使用点：`FlipTagID`、`IsOldPointer`、`IsCurrentPointer`、`GetAndTryTagRefField`、`TryUpdateRefField` 等。

### 7.3 CompilerCalls 改造

**核心约束：所有 `extern "C"` 函数签名完全不变。** MSWriteBarrier 通过虚函数分派自动生效，CompilerCalls 层面的写操作无需任何改造。读操作可选条件编译优化（绕过虚函数调用），但即使不做也能通过虚函数正确工作。后续编译器可配合将 `llvm.cj.gcread.*` intrinsic 降级为普通 `load`。

---

## 8. 对 Step2 各子阶段的具体改造

> **详细设计文档** → [Step3.4_对Step2子阶段的改造.md](Step3.4_对Step2子阶段的改造.md)

混合写屏障对 Step2 的 4 个子阶段产生如下影响：

| 子阶段 | 影响级别 | 关键变更 |
|--------|---------|---------|
| Step2.1 MarkSetup (Handshake) | 🟡 中 | Barrier 安装从 `MSTraceBarrier` 改为 `MSWriteBarrier`；`HandleGCPhase` 分支中 `SatbBuffer.RetireNode()` → `WBBuf.Flush()` |
| Step2.2 ConcurrentMark | 🔴 高 | WBBuf flush 实时写入 per-P `localWorkBuf`（非 STW 后批量处理）；灰色对象实时进入标记流水线 |
| Step2.3 MarkTermination (STW) | 🟡 中 | `CollectSatbCandidatesToMain()` → `FlushAllWriteBarrierBuffers()`；无需栈重扫——混合屏障已保护栈引用 |
| Step2.4 PostMarkTermination | 🟢 低 | `ClearSatbBuffer()` → `ClearWriteBarrierBuffers()`；其余 5 项任务不变 |

**最关键变化**：SATB 方案中灰色对象在 MarkTermination STW 才处理（延迟处理），混合屏障的 WBBuf 在并发标记期间实时注入工作队列（实时处理），减少 STW 停顿时间。

---

## 9. 屏障安装与 Phase 联动

### 9.1 现有的屏障安装

当前 `HeapImpl::InstallBarrier`（Heap.cpp L189）根据 6 个 GC Phase 切换 6 套屏障：

```cpp
void HeapImpl::InstallBarrier(const GCPhase phase) {
    if      (phase == GC_PHASE_ENUM)          → enumBarrier
    else if (phase == GC_PHASE_TRACE)         → traceBarrier
    else if (phase == GC_PHASE_CLEAR_SATB)    → traceBarrier  
    else if (phase == GC_PHASE_POST_TRACE)    → postTraceBarrier
    else if (phase == GC_PHASE_PREFORWARD)    → preforwardBarrier
    else if (phase == GC_PHASE_FORWARD)       → forwardBarrier
    else if (phase == GC_PHASE_IDLE)          → idleBarrier
}
```

### 9.2 新的屏障安装（Mark-Sweep 模式）

MS 模式只需 2 套屏障，安装逻辑大幅简化：

```cpp
void HeapImpl::InstallBarrierMS(const GCPhase phase) {
    switch (phase) {
    // 标记相关的三个阶段 → 安装混合写屏障
    case MS_PHASE_MARK_SETUP:
    case MS_PHASE_MARK:
    case MS_PHASE_MARK_TERMINATION:
        currentBarrier = &msWriteBarrier_;
        break;
    
    // 其他所有阶段（IDLE、SWEEP 等）→ 空闲屏障
    default:
        currentBarrier = &msIdleBarrier_;
        break;
    }
}
```

### 9.3 Phase 状态与 Barrier 对应关系

```
┌─────────────────┐    ┌──────────────────┐
│   GC_PHASE_IDLE │───→│  MSIdleBarrier   │
│  (Off / Sweep)  │    │  (零开销读写)     │
└────────┬────────┘    └──────────────────┘
         │ Mark Setup Handshake
         ▼
┌─────────────────┐    ┌──────────────────┐
│ MS_MARK_SETUP   │───→│  MSWriteBarrier  │
│  (Handshake)    │    │  (shade old+new) │
└────────┬────────┘    └──────────────────┘
         │ StartTheWorld
         ▼
┌─────────────────┐    ┌──────────────────┐
│ MS_CONCURRENT   │───→│  MSWriteBarrier  │
│ _MARK (并发)    │    │  (shade old+new) │
└────────┬────────┘    └──────────────────┘
         │ Mark Termination STW
         ▼
┌─────────────────┐    ┌──────────────────┐
│ MS_MARK_TERM    │───→│  MSWriteBarrier  │
│  (STW)          │    │  (shade old+new) │
└────────┬────────┘    └──────────────────┘
         │ Sweep 开始
         ▼
┌─────────────────┐    ┌──────────────────┐
│ GC_PHASE_IDLE   │───→│  MSIdleBarrier   │
│  (并发 Sweep)   │    │  (零开销读写)     │
└─────────────────┘    └──────────────────┘
```

**关键设计：Sweep 阶段不需要写屏障**。Sweep 是在标记完成后进行的，标记结果（bitmap）是确定性的快照。并发 Sweep 与 mutator 的交互通过 gcEpoch 和 per-region 锁保护（详见 Step4）。

### 9.4 HeapImpl 持有的 Barrier 实例

```cpp
// Heap.cpp / HeapImpl 成员

#ifdef MRT_USE_MARK_SWEEP
    // Mark-Sweep 模式：仅 2 个 Barrier 实例
    MSIdleBarrier   msIdleBarrier_;        // GC 空闲期
    MSWriteBarrier  msWriteBarrier_;       // GC Mark 期
    Barrier*        currentBarrier;        // 当前活跃
#else
    // 现有 Copy GC 模式：6 个 Barrier 实例
    Barrier         stwBarrier;
    IdleBarrier     idleBarrier;
    EnumBarrier     enumBarrier;
    TraceBarrier    traceBarrier;
    PostTraceBarrier postTraceBarrier;
    PreforwardBarrier preforwardBarrier;
    ForwardBarrier  forwardBarrier;
    Barrier*        currentBarrier;
#endif
```

### 9.5 屏障切换的触发点

屏障切换由 `MutatorManager` 在 Phase 转换时触发（与现有机制完全一致）：

```
Collector::TransitionToGCPhase(newPhase, isSTW)
  → MutatorManager::StopTheWorldTransition(newPhase)       // STW 切换
    → HeapImpl::InstallBarrierMS(newPhase)                  // 安装新 barrier
    → MutatorManager::TransitionAllMutatorsToGCPhase()      // 广播给所有 mutator
  
  → MutatorManager::TransitionToPhaseNonSTW(newPhase)       // 非 STW 切换
    → HeapImpl::InstallBarrierMS(newPhase)                  // 安装新 barrier
    → EnsurePhaseTransition()                               // 确保所有 mutator 看到
```

### 9.6 Barrier 生效时序保证

**问题**：切换 barrier 指针后，正在执行 barrier 的 mutator 如何安全过渡？

**保证**：
1. **STW 切换**：所有 mutator 已暂停，切换后才恢复 → 不存在并发问题
2. **非 STW 切换**：`InstallBarrierMS` 修改 `currentBarrier` 指针（单指针原子写），mutator 下一次 `GetBarrier()` 即获取新 barrier → 可能有 1-2 次旧 barrier 的尾部调用

**MS 模式下的安全性**：
- Off → On：某个 mutator 可能用 MSIdleBarrier 执行了一次没有 shade 的写 → 不影响，因为 Mark Setup 使用 Handshake 确保所有 Mutator 在进入并发标记前看到新 barrier
- On → Off：某个 mutator 可能用 MSWriteBarrier 多做了一次 shade → 无害（多标不少标）

**结论**：2 套 barrier 的切换比 6 套更安全，因为 Off→On 通过 Handshake 确保所有 Mutator 逐一切换，On→Off 的多余 shade 是无害的。

---

## 10. 与现有基础设施的复用与改造清单

### 10.1 总览矩阵

| 组件 | 文件 | 复用方式 | 说明 |
|------|------|----------|------|
| **Barrier 基类** | Barrier.h/cpp | ✅ 直接复用 | 虚函数接口不变 |
| **IdleBarrier** | WCollector/IdleBarrier.h/cpp | ❌ 替换为 MSIdleBarrier | 去除 tag 处理 |
| **EnumBarrier** | WCollector/EnumBarrier.h/cpp | ❌ 废弃 | MS 无 ENUM phase |
| **TraceBarrier** | WCollector/TraceBarrier.h/cpp | ❌ 替换为 MSWriteBarrier | SATB → 混合 |
| **PostTraceBarrier** | WCollector/PostTraceBarrier.h/cpp | ❌ 废弃 | MS 无此 phase |
| **PreforwardBarrier** | WCollector/PreforwardBarrier.h/cpp | ❌ 废弃 | MS 无 Forward |
| **ForwardBarrier** | WCollector/ForwardBarrier.h/cpp | ❌ 废弃 | MS 无 Forward |
| **SatbBuffer** | Mutator/SatbBuffer.h/cpp | 🔄 部分复用 | arena/page 管理可复用，Node 改为 WBBuf |
| **Heap::GetBarrier()** | Heap.h/cpp | ✅ 直接复用 | 二级指针机制不变 |
| **InstallBarrier** | Heap.cpp L189 | 🔄 修改 | 新增 `InstallBarrierMS` 分支 |
| **CompilerCalls** | CompilerCalls.cpp | ✅ 直接复用 / 🔄 可选优化 | **签名完全不变**；读操作可选条件编译直接返回（绕过虚函数） |
| **RefField** | ObjectModel/RefField.h | 🔄 修改 | MS 模式简化为裸指针 |
| **RefField.inline.h** | ObjectModel/RefField.inline.h | 🔄 修改 | MS 模式去除 tag 构造 |
| **MutatorManager** | Mutator/MutatorManager.h/cpp | ✅ 直接复用 | STW/LightSync 机制不变 |
| **RegionSpace::MarkObject** | RegionSpace.h L192 | ✅ 直接复用 | bitmap 标记不变 |
| **WorkStack** | TracingCollector 内部 | ✅ 直接复用 | WBBuf flush 目标 |

### 10.2 新增文件

| 文件 | 说明 |
|------|------|
| `src/Heap/MSBarrier/MSIdleBarrier.h` | GC 空闲期屏障声明 |
| `src/Heap/MSBarrier/MSIdleBarrier.cpp` | GC 空闲期屏障实现 |
| `src/Heap/MSBarrier/MSWriteBarrier.h` | 混合写屏障声明 |
| `src/Heap/MSBarrier/MSWriteBarrier.cpp` | 混合写屏障实现 |
| `src/Heap/MSBarrier/WriteBarrierBuffer.h` | Per-thread 写屏障缓冲区 |

### 10.3 需修改的现有文件

| 文件 | 修改内容 |
|------|----------|
| `src/Heap/Heap.h` | 增加 `InstallBarrierMS` 声明；条件编译 barrier 成员 |
| `src/Heap/Heap.cpp` | 增加 `InstallBarrierMS` 实现；初始化 MS barrier 实例 |
| `src/CompilerCalls.cpp` | 读操作可选优化（条件编译直接返回，绕过虚函数调用）；**所有函数签名不变** |
| `src/ObjectModel/RefField.h` | 条件编译：MS 模式简化为裸指针 |
| `src/ObjectModel/RefField.inline.h` | 条件编译：MS 模式去除 tag 构造 |
| `src/Mutator/Mutator.h` | 增加 `WriteBarrierBuffer wbBuf_` 成员 |
| `src/Mutator/Mutator.cpp` | Mutator 析构时 flush WBBuf |
| `CMakeLists.txt` | 增加 MSBarrier 目录源文件 |

### 10.4 可完全删除的代码（MS 模式启用后）

| 组件 | 代码量估计 | 说明 |
|------|-----------|------|
| EnumBarrier.h/cpp | ~400 行 | 枚举阶段屏障 |
| PostTraceBarrier.h/cpp | ~300 行 | 标记后屏障 |
| PreforwardBarrier.h/cpp | ~400 行 | 预转发屏障 |
| ForwardBarrier.h/cpp | ~350 行 | 转发阶段屏障 |
| ForwardDataManager | ~200 行 | 转发表管理 |
| Tagged Pointer 相关逻辑 | ~300 行分散 | FlipTagID/IsOldPointer 等 |
| 合计 | **~1950 行** | 净减少的代码量 |

新增代码约 ~800 行（MSIdleBarrier + MSWriteBarrier + WriteBarrierBuffer），**净减少约 1150 行**。

### 10.5 改造优先级

| 阶段 | 任务 | 前置依赖 |
|------|------|----------|
| P0 | MSIdleBarrier + MSWriteBarrier 核心实现 | Step2 MarkSweepCollector 框架 |
| P0 | WriteBarrierBuffer 实现 | MSWriteBarrier |
| P1 | CompilerCalls 读操作条件编译 | MSIdleBarrier |
| P1 | RefField 条件编译（去 tag） | MSIdleBarrier |
| P1 | InstallBarrierMS 集成 | Heap.h/cpp |
| P2 | 编译器 intrinsic 降级（cjc 侧） | P1 完成后配合 |
| P3 | 删除旧 Barrier 代码（6 套） | MS 模式稳定验证后 |

---

## 11. 验证方案

### 11.1 单元测试

#### 11.1.1 MSWriteBarrier 正确性

```
测试名: TestMSWriteBarrier_ShadeOldAndNew
前置条件: GC 处于 MARK phase，MSWriteBarrier 已安装
测试步骤:
  1. 创建对象 A、B、C，A.ref = B（老值）
  2. 执行 MSWriteBarrier::WriteReference(A, &A.ref, C)
  3. 验证 A.ref == C（写入成功）
  4. 验证 B 在 WBBuf 或已被 MarkObject 标灰（shade old）
  5. 验证 C 在 WBBuf 或已被 MarkObject 标灰（shade new）
预期: old 和 new 都被标记，引用更新成功
```

```
测试名: TestMSWriteBarrier_IdlePhaseNoop
前置条件: GC 处于 IDLE phase，MSIdleBarrier 已安装
测试步骤:
  1. 创建对象 A、B、C
  2. 执行 MSIdleBarrier::WriteReference(A, &A.ref, C)
  3. 验证 A.ref == C（写入成功）
  4. 验证 WBBuf 为空（无标记操作）
预期: Idle 期间写操作只做 store，无 shade
```

```
测试名: TestMSWriteBarrier_NullValues
前置条件: GC 处于 MARK phase
测试步骤:
  1. A.ref = B，执行 WriteReference(A, &A.ref, nullptr)
  2. 验证 B 被 shade（old 不为 null）
  3. 验证 nullptr 不触发 shade
  4. A.ref = nullptr，执行 WriteReference(A, &A.ref, C)
  5. 验证 C 被 shade（new 不为 null）
预期: null 值跳过 shade，非 null 值正常标记
```

#### 11.1.2 WriteBarrierBuffer 测试

```
测试名: TestWBBuf_FlushOnFull
前置条件: WBBuf 容量 = 256
测试步骤:
  1. 连续 Push 256 个对象引用
  2. 验证第 256 次 Push 触发 Flush
  3. Flush 后 WBBuf pos_ 回到 0
  4. 验证 256 个对象均被提交到全局 WorkStack
预期: 满时自动 flush，无丢失
```

```
测试名: TestWBBuf_ManualFlush
前置条件: WBBuf 中有 100 个对象
测试步骤:
  1. 调用 Flush()
  2. 验证 100 个对象提交到全局 WorkStack
  3. 验证 pos_ = 0
预期: 手动 flush 正常工作
```

#### 11.1.3 RefField 简化验证

```
测试名: TestRefField_MSMode_NoTag
前置条件: 编译宏 MRT_USE_MARK_SWEEP=1
测试步骤:
  1. 将 objPtr 写入 RefField
  2. 读回 RefField 值
  3. 验证读回值 == objPtr（无 tag bit 干扰）
  4. 验证 IsTagged() 始终返回 false
预期: MS 模式 RefField 为纯裸指针
```

### 11.2 并发安全测试

#### 11.2.1 多线程写屏障压力测试

```
测试名: TestConcurrentWriteBarrier_Stress
线程数: 8 个 Mutator + 1 个 GC 线程
运行时间: 10 秒
测试步骤:
  1. 创建共享数组 sharedArr[1024]，每元素指向一个对象
  2. 8 个 Mutator 线程随机执行:
     - sharedArr[random()] = newObj / nullptr
     - 使用 MSWriteBarrier::WriteReference
  3. GC 线程循环执行:
     - 安装 MSWriteBarrier（进入 MARK）
     - 并发标记
     - FlushAllWBBuf
     - 安装 MSIdleBarrier（回到 IDLE）
  4. 运行结束后验证:
     - 所有 sharedArr 中的非 null 引用都指向有效对象
     - 无 use-after-free
预期: 无崩溃，无内存错误
```

#### 11.2.2 屏障切换竞态测试

```
测试名: TestBarrierSwitch_Race
线程数: 4 个 Mutator + 1 个 GC 线程
运行时间: 5 秒
测试步骤:
  1. GC 线程快速循环 InstallBarrierMS(IDLE) ↔ InstallBarrierMS(MARK)
  2. 4 个 Mutator 线程连续执行 WriteReference
  3. 验证每次 WriteReference 执行的屏障逻辑与当前 phase 匹配
  4. 利用 phase 计数器验证无丢失的 shade 操作
预期: 切换过程无 SEGV，无竞态导致的漏标
```

### 11.3 GC 完整性测试

#### 11.3.1 混合写屏障 + 并发标记集成测试

```
测试名: TestMS_HybridBarrier_FullGC
测试步骤:
  1. 分配 10000 个对象，构建随机引用图
  2. 启动并发标记（安装 MSWriteBarrier）
  3. 标记期间 Mutator 执行 5000 次随机引用修改
  4. 标记完成后统计:
     a. 所有可达对象是否都被标记（无漏标）
     b. 标记对象中不可达的比例（浮动垃圾率）
  5. 执行 Sweep，回收未标记对象
  6. 再次遍历验证所有可达对象仍存活
预期:
  - 漏标率 = 0%（正确性保证）
  - 浮动垃圾率 < 20%（可接受的保守性）
```

#### 11.3.2 长时间运行稳定性测试

```
测试名: TestMS_LongRun_Stability
运行时间: 60 秒（或 1000 次 GC 循环）
测试步骤:
  1. 8 个 Mutator 线程持续分配+修改引用
  2. GC 线程循环执行完整 MS 回收
  3. 每 100 次 GC 后:
     a. 验证堆内存不持续增长（无泄漏）
     b. 验证对象数量在合理范围波动
     c. 验证 WBBuf 统计数据正常
预期: 内存稳定，无泄漏，无崩溃
```

### 11.4 性能基准测试

| 测试项 | 指标 | 对比基线 | 目标 |
|--------|------|----------|------|
| 读操作延迟 | 单次 RefField 读取耗时 | Copy GC（带 tag 解码） | 降低 15-30% |
| 写操作延迟 | 单次 WriteReference 耗时 | Copy GC TraceBarrier | 持平或 +10% 以内 |
| WBBuf flush 频率 | 每秒 flush 次数 | SatbBuffer retire 频率 | 持平或更低 |
| 屏障切换耗时 | InstallBarrierMS 单次耗时 | InstallBarrier 当前实现 | 持平 |
| 整体吞吐量 | 应用 QPS / 事务/秒 | Copy GC 完整运行 | 持平或提升 |

### 11.5 回归验证清单

| 验证项 | 方法 | 通过标准 |
|--------|------|----------|
| gc.cj 基准测试 | `cjc gc.cj && ./main` | 输出正确，无崩溃 |
| gc_large.cj 大对象 | `cjc gc_large.cj && ./main` | 大对象正常回收 |
| gc_mixed.cj 混合 | `cjc gc_mixed.cj && ./main` | 小对象+大对象混合正常 |
| WeakRef 语义 | 自定义 WeakRef 测试 | GC 后弱引用正确清理 |
| Finalizer 执行 | 带 finalize() 的对象 | 回收时 finalizer 正确执行 |
| ASAN/TSAN 检查 | 编译带 sanitizer 运行 | 0 error，0 warning |
| 编译宏切换 | 分别编译 CopyGC / MS | 两种模式均编译通过 |
