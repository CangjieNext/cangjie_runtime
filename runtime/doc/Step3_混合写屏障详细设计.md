# Step 3：混合写屏障详细设计 —— 从 SATB+读屏障到 Go 风格 Hybrid Write Barrier

> 本文档是 [Go 风格 GC 迁移方案](Go风格GC迁移方案.md) Step 3 的独立详细设计文档。
> 目标：用 Go 的混合写屏障（Hybrid Write Barrier）替换当前的 SATB 写屏障 + 6 套读写屏障体系，实现**无读屏障**的并发标记。

---

## 目录

1. [现有屏障系统深度分析](#1-现有屏障系统深度分析)
2. [Go 混合写屏障原理](#2-go-混合写屏障原理)
3. [混合写屏障与 SATB 的理论对比](#3-混合写屏障与-satb-的理论对比)
4. [MSBarrier 总体设计](#4-msbarrier-总体设计)
5. [MSWriteBarrier 详细实现](#5-mswritebarrier-详细实现)
6. [写屏障 Buffer 优化（Per-Thread WBBuf）](#6-写屏障-buffer-优化per-thread-wbbuf)
7. [读屏障完全移除方案](#7-读屏障完全移除方案)
8. [Tagged Pointer 移除方案](#8-tagged-pointer-移除方案)
9. [CompilerCalls 改造](#9-compilercalls-改造)
10. [屏障安装与 Phase 联动](#10-屏障安装与-phase-联动)
11. [与现有基础设施的复用与改造清单](#11-与现有基础设施的复用与改造清单)
12. [验证方案](#12-验证方案)

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

### 4.4 MSIdleBarrier 设计

GC 不活跃时使用，所有操作都是直接内存访问，**零开销**：

```cpp
class MSIdleBarrier : public Barrier {
public:
    explicit MSIdleBarrier(Collector& collector) : Barrier(collector) {}
    
    // ====== 读操作：直接返回 ======
    BaseObject* ReadReference(BaseObject* obj, RefField<false>& field) const override {
        return field.GetTargetObject();  // 无 tag，直接返回地址
    }
    
    BaseObject* ReadStaticRef(RefField<false>& field) const override {
        return field.GetTargetObject();
    }
    
    BaseObject* ReadWeakRef(BaseObject* obj, RefField<false>& field) const override {
        return field.GetTargetObject();
    }
    
    void ReadStruct(MAddress dst, BaseObject* obj, MAddress src, size_t size) const override {
        memmove_s(dst, size, src, size);  // 直接 memcpy
    }
    
    void ReadStaticStruct(MAddress dst, MAddress src, size_t size, const GCTib gctib) const override {
        memmove_s(dst, size, src, size);
    }
    
    // ====== 写操作：直接写入 ======
    void WriteReference(BaseObject* obj, RefField<false>& field, BaseObject* ref) const override {
        field.SetTargetObject(ref);  // 无 tag，直接写入
    }
    
    void WriteStaticRef(RefField<false>& field, BaseObject* ref) const override {
        field.SetTargetObject(ref);
    }
    
    void WriteStruct(BaseObject* obj, MAddress dst, size_t dstLen, 
                     MAddress src, size_t srcLen) const override {
        memmove_s(dst, dstLen, src, srcLen);
    }
    
    void WriteStaticStruct(MAddress dst, size_t dstLen, MAddress src, size_t srcLen,
                           const GCTib gctib) const override {
        memmove_s(dst, dstLen, src, srcLen);
    }
    
    // ====== 数组拷贝：直接 memcpy ======
    void CopyRefArray(BaseObject* dstObj, MAddress dstField, MIndex dstSize,
                      BaseObject* srcObj, MAddress srcField, MIndex srcSize) const override {
        size_t copySize = std::min(dstSize, srcSize) * sizeof(uintptr_t);
        memmove_s(dstField, copySize, srcField, copySize);
    }
    
    // ====== 原子操作：直接原子读写 ======
    BaseObject* AtomicReadReference(BaseObject* obj, RefField<true>& field,
                                    MemoryOrder order) const override {
        return field.GetTargetObject(order);
    }
    
    void AtomicWriteReference(BaseObject* obj, RefField<true>& field, BaseObject* ref,
                              MemoryOrder order) const override {
        field.SetTargetObject(ref, order);
    }
    
    BaseObject* AtomicSwapReference(BaseObject* obj, RefField<true>& field, BaseObject* ref,
                                    MemoryOrder order) const override {
        return field.Exchange(ref, order);
    }
    
    bool CompareAndSwapReference(BaseObject* obj, RefField<true>& field,
                                 BaseObject* oldRef, BaseObject* newRef,
                                 MemoryOrder succOrder, MemoryOrder failOrder) const override {
        return field.CompareExchange(oldRef, newRef, succOrder, failOrder);
    }
};
```

**与现有 IdleBarrier 的关键区别**：
- 现有 IdleBarrier 读操作需要 `IsTagged()` 检查 + 可能的 forward
- MSIdleBarrier 直接返回，不做任何额外操作
- 开销从 ~5-10 条指令降到 1 条 load 指令

### 4.5 与 Barrier 基类的兼容

MSIdleBarrier 和 MSWriteBarrier 都继承自同一个 `Barrier` 基类，虚函数接口完全兼容。这意味着：
- `Heap::GetBarrier()` 返回的仍然是 `Barrier&`
- 所有 CompilerCalls 中的调用路径不需要修改签名
- 通过 `InstallBarrier` 切换指针即可生效

---

## 5. MSWriteBarrier 详细实现

### 5.1 核心设计

MSWriteBarrier 在 GC Mark 阶段（Mark Setup → Concurrent Mark → Mark Termination）生效。它的核心职责：
- **写操作**：标灰旧值 + 标灰新值 → 写入
- **读操作**：直接 load，零开销（与 MSIdleBarrier 相同）

### 5.2 MarkAndEnqueue 辅助方法

所有 shade 操作最终都归结到 `MarkAndEnqueue`：

```cpp
// 标记对象并推入写屏障缓冲区（如果是首次标记）
inline void MSWriteBarrier::MarkAndEnqueue(BaseObject* obj) const {
    if (obj == nullptr) {
        return;
    }
    if (!Heap::IsHeapAddress(reinterpret_cast<MAddress>(obj))) {
        return;  // 非堆对象（栈/全局），跳过
    }
    // 尝试标记（原子操作，bitmap 中的 set-bit）
    if (RegionSpace::MarkObject(obj)) {
        return;  // 已经被标记过，无需入队
    }
    // 首次标记 → 推入 per-thread WBBuf
    Mutator* mutator = Mutator::GetMutator();
    if (mutator != nullptr) {
        mutator->GetWriteBarrierBuffer().Push(obj);
    }
}
```

**与现有 SATB 的关键区别**：
- SATB 记断开的引用，GC 后统一标记
- 混合写屏障**立即原子标记**（`MarkObject`），然后将灰色对象入队待扫描
- 减少了 GC 线程后续处理 SATB buffer 的工作量

### 5.3 WriteReference 实现

```cpp
void MSWriteBarrier::WriteReference(BaseObject* obj, RefField<false>& field, 
                                     BaseObject* ref) const {
    // 1. shade(old) — 旧值标灰
    BaseObject* oldRef = field.GetTargetObject();  // 直接读取，无 tag
    MarkAndEnqueue(oldRef);
    
    // 2. shade(new) — 新值标灰
    MarkAndEnqueue(ref);
    
    // 3. 实际写入（release 语义确保 shade 先于写入可见）
    std::atomic_thread_fence(std::memory_order_release);
    field.SetTargetObject(ref);
}
```

**对比现有 TraceBarrier::WriteReference**：
| 步骤 | 现有 TraceBarrier | 新 MSWriteBarrier |
|------|------------------|-------------------|
| 读旧值 | IsOldPointer → TryUpdateRefField（含 CAS） | `field.GetTargetObject()`（直接 load） |
| SATB 记录 | `RememberObjectInSatbBuffer(old)` | `MarkAndEnqueue(old)` + `MarkAndEnqueue(new)` |
| 写入 | `GetAndTryTagRefField` → `SetFieldValue`（打 tag） | `field.SetTargetObject(ref)`（直接写） |
| fence | `seq_cst` | `release`（更轻量） |
| 总指令数 | ~20-30 | ~10-15 |

### 5.4 WriteStaticRef 实现

```cpp
void MSWriteBarrier::WriteStaticRef(RefField<false>& field, BaseObject* ref) const {
    // 静态字段同样需要 shade old + shade new
    BaseObject* oldRef = field.GetTargetObject();
    MarkAndEnqueue(oldRef);
    MarkAndEnqueue(ref);
    
    std::atomic_thread_fence(std::memory_order_release);
    field.SetTargetObject(ref);
}
```

**注意**：现有 `TraceBarrier::WriteStaticRef` 不记录 SATB（L125），这是不安全的（在 Copy GC 中静态引用由根枚举保证）。在 Mark-Sweep 中，静态引用的覆盖写必须 shade old，否则旧的引用目标可能被漏标。

### 5.5 WriteStruct 实现

结构体写入涉及多个引用字段：

```cpp
void MSWriteBarrier::WriteStruct(BaseObject* obj, MAddress dst, size_t dstLen,
                                  MAddress src, size_t srcLen) const {
    if (obj != nullptr) {
        // 1. shade all old refs in dst
        obj->ForEachRefInStruct(
            [this](RefField<>& refField) {
                BaseObject* oldRef = refField.GetTargetObject();
                MarkAndEnqueue(oldRef);
            },
            dst, dst + dstLen);
    }
    
    // 2. memcpy（实际写入结构体数据）
    std::atomic_thread_fence(std::memory_order_release);
    memmove_s(dst, dstLen, src, srcLen);
    
    if (obj != nullptr) {
        // 3. shade all new refs in dst
        obj->ForEachRefInStruct(
            [this](RefField<>& refField) {
                BaseObject* newRef = refField.GetTargetObject();
                MarkAndEnqueue(newRef);
            },
            dst, dst + dstLen);
    }
}
```

**对比现有 TraceBarrier::WriteStruct**：
- 现有实现在 memcpy 后还需要 `GetAndTryTagRefField` + `CompareExchange` 对每个引用字段打 tag
- 新实现省去了所有 tag 操作，更简洁高效

### 5.6 CopyRefArray 实现

```cpp
void MSWriteBarrier::CopyRefArray(BaseObject* dstObj, MAddress dstField, MIndex dstSize,
                                   BaseObject* srcObj, MAddress srcField, MIndex srcSize) const {
    MIndex count = std::min(dstSize, srcSize);
    auto* dstSlots = reinterpret_cast<BaseObject**>(dstField);
    auto* srcSlots = reinterpret_cast<BaseObject**>(srcField);
    
    for (MIndex i = 0; i < count; ++i) {
        // shade old
        MarkAndEnqueue(dstSlots[i]);
        // shade new (from src)
        MarkAndEnqueue(srcSlots[i]);
    }
    
    // bulk copy
    std::atomic_thread_fence(std::memory_order_release);
    memmove_s(dstField, count * sizeof(uintptr_t), srcField, count * sizeof(uintptr_t));
}
```

### 5.7 原子操作实现

```cpp
void MSWriteBarrier::AtomicWriteReference(BaseObject* obj, RefField<true>& field,
                                           BaseObject* ref, MemoryOrder order) const {
    // 原子读旧值
    BaseObject* oldRef = field.GetTargetObject(std::memory_order_relaxed);
    MarkAndEnqueue(oldRef);
    MarkAndEnqueue(ref);
    
    // 原子写入
    field.SetTargetObject(ref, order);
}

BaseObject* MSWriteBarrier::AtomicSwapReference(BaseObject* obj, RefField<true>& field,
                                                 BaseObject* ref, MemoryOrder order) const {
    MarkAndEnqueue(ref);  // shade new
    
    // 原子交换
    BaseObject* oldRef = field.Exchange(ref, order);
    MarkAndEnqueue(oldRef);  // shade old (交换返回的旧值)
    
    return oldRef;
}

bool MSWriteBarrier::CompareAndSwapReference(BaseObject* obj, RefField<true>& field,
                                              BaseObject* oldRef, BaseObject* newRef,
                                              MemoryOrder succOrder, MemoryOrder failOrder) const {
    // shade both old and new regardless of CAS result
    MarkAndEnqueue(oldRef);
    MarkAndEnqueue(newRef);
    
    return field.CompareExchange(oldRef, newRef, succOrder, failOrder);
}
```

**AtomicSwapReference 的特殊处理**：先 shade(new)，再执行 exchange，再 shade 返回的旧值。这确保了无论并发如何交错，旧值和新值都被保护。

### 5.8 ReadReference 和其他读操作

MSWriteBarrier 的读操作与 MSIdleBarrier 完全相同——**直接返回**：

```cpp
BaseObject* MSWriteBarrier::ReadReference(BaseObject* obj, RefField<false>& field) const {
    return field.GetTargetObject();  // 零开销
}

BaseObject* MSWriteBarrier::ReadStaticRef(RefField<false>& field) const {
    return field.GetTargetObject();
}

BaseObject* MSWriteBarrier::ReadWeakRef(BaseObject* obj, RefField<false>& field) const {
    return field.GetTargetObject();
}

void MSWriteBarrier::ReadStruct(MAddress dst, BaseObject* obj, MAddress src, size_t size) const {
    memmove_s(dst, size, src, size);
}
```

**这是本次改造最大的性能收益点**——即使在 GC Mark 阶段，读操作也完全无开销。

---

## 6. 写屏障 Buffer 优化（Per-Thread WBBuf）

### 6.1 为什么需要 Buffer

`MarkAndEnqueue` 中的 `MarkObject` 是原子操作（bitmap CAS），但将灰色对象推入全局 workStack 需要加锁。为避免每次写屏障都竞争全局锁，使用 per-thread 的写屏障缓冲区：

```
写屏障 → MarkAndEnqueue(obj)
           → MarkObject(obj)           // 原子 bitmap CAS（无锁）
           → wbBuf.Push(obj)           // 写入线程本地 buffer（无锁）
           → if full: wbBuf.Flush()    // 满时批量转移到全局 workStack
```

### 6.2 WriteBarrierBuffer 设计

```cpp
// src/Heap/MSBarrier/WriteBarrierBuffer.h

class WriteBarrierBuffer {
public:
    static constexpr size_t kCapacity = 256;  // 256 × 8 = 2KB
    
    WriteBarrierBuffer() : pos_(0) {}
    
    // 推入一个灰色对象（快速路径：无锁）
    void Push(BaseObject* obj) {
        buf_[pos_++] = obj;
        if (UNLIKELY(pos_ >= kCapacity)) {
            Flush();
        }
    }
    
    // 批量转移到全局 workStack
    void Flush() {
        if (pos_ == 0) return;
        
        // 获取 GC 线程的 workStack
        WorkStack& globalStack = GetGlobalWorkStack();
        globalStack.PushBatch(buf_, pos_);
        
        pos_ = 0;
    }
    
    // GC 结束时清空（无需 flush，因为对象已被标记）
    void Clear() {
        pos_ = 0;
    }
    
    bool IsEmpty() const { return pos_ == 0; }
    size_t Size() const { return pos_; }

private:
    BaseObject* buf_[kCapacity];
    size_t pos_;
};
```

### 6.3 与现有 SatbBuffer::Node 的对比

| 维度 | SatbBuffer::Node | WriteBarrierBuffer |
|------|-----------------|-------------------|
| 存储位置 | 从全局 arena 动态分配 | **内嵌在 Mutator 中**（栈式 buffer，无动态分配） |
| 容量 | 64~69 entries | 256 entries（更大，减少 flush 频率） |
| 满时行为 | retire 到全局 retiredNodes 链表 | 批量 push 到全局 WorkStack |
| 对象处理时机 | Mark Termination 时统一遍历 | 随 flush 实时进入 WorkStack → GC 线程实时处理 |
| 内存管理 | 需要 arena/page 管理 | 无额外内存管理（生命周期跟随 Mutator） |

### 6.4 Buffer 嵌入位置

在 Mutator 类中增加 WBBuf 成员：

```cpp
// src/Mutator/Mutator.h（修改）
class Mutator {
    // ... 现有成员 ...
    
#ifdef MRT_USE_MARK_SWEEP
    WriteBarrierBuffer wbBuf_;
    
public:
    WriteBarrierBuffer& GetWriteBarrierBuffer() { return wbBuf_; }
#endif
};
```

### 6.5 Buffer 生命周期管理

| 时机 | 操作 |
|------|------|
| Mutator 创建 | WBBuf 自动初始化（pos=0） |
| 每次写屏障 | `Push(obj)` → 可能触发 `Flush()` |
| Mark Termination STW | `FlushAll()`：遍历所有 Mutator，强制 flush 所有 WBBuf |
| GC 结束（进入 IDLE） | `ClearAll()`：清空所有 WBBuf |
| Mutator 退出 | 析构时自动 flush 残留对象 |

```cpp
// Mark Termination 中的 FlushAll
void MarkSweepCollector::FlushAllWriteBarrierBuffers() {
    MutatorManager::Instance().VisitAllMutators([](Mutator& mutator) {
        mutator.GetWriteBarrierBuffer().Flush();
    });
}
```

### 6.6 与 GC 并发标记的集成

WBBuf flush 产生的工作会进入 GC 线程的 WorkStack，而并发标记线程（ConcurrentMarkingWork）的 Execute 循环会从 WorkStack 取出对象继续标记。这形成了一个高效的生产者-消费者流水线：

```
Mutator 写屏障 → WBBuf[线程本地]
                     ↓ (满时 flush)
                 WorkStack[全局]
                     ↓ (GC 线程取出)
               ConcurrentMarkingWork::Execute()
                     ↓ (遍历引用字段)
                 MarkObject → 回到 WorkStack
```

### 6.7 性能考量

- **Push 路径**：1 次数组写 + 1 次比较，约 2 条指令
- **Flush 路径**：批量 memcpy + 锁操作，均摊到每次 push 约 0.5 条指令
- **对比 SatbBuffer**：SatbBuffer::Push 需要 1 次加锁（Node 的 mutex），WriteBarrierBuffer::Push 无锁
- **整体**：写屏障热路径从 ~15 条指令降到 ~5 条指令

---

## 7. 读屏障完全移除方案

### 7.1 当前读屏障存在的所有位置

在现有 Copy GC 中，以下位置都有读屏障逻辑：

| 调用入口 | 文件 | 行为 |
|----------|------|------|
| `CJ_MCC_ReadRefField` | CompilerCalls.cpp L1550 | `GetBarrier().ReadReference(obj, field)` |
| `CJ_MCC_ReadWeakRef` | CompilerCalls.cpp L1555 | `GetBarrier().ReadWeakRef(obj, field)` |
| `CJ_MCC_ReadStructField` | CompilerCalls.cpp L1559 | `GetBarrier().ReadStruct(dst, obj, src, size)` |
| `CJ_MCC_ReadStaticRef` | CompilerCalls.cpp L1571 | `GetBarrier().ReadStaticRef(field)` |
| `CJ_MCC_ReadStaticStruct` | CompilerCalls.cpp L1575 | `GetBarrier().ReadStaticStruct(...)` |
| `CJ_MCC_ReadGeneric` | CompilerCalls.cpp L1705 | `GetBarrier().ReadGeneric(...)` |
| `MCC_AtomicReadReference` | CompilerCalls.cpp L360 | `GetBarrier().AtomicReadReference(...)` |

### 7.2 为什么 Mark-Sweep 不需要读屏障

Copy GC 需要读屏障的根本原因是**对象会移动**：
- Forward 阶段，from-space 中的对象被拷贝到 to-space
- 读屏障负责在读取 from-space 指针时"自愈"（查转发表，返回 to-space 地址）

Mark-Sweep 中**对象不移动**：
- 对象分配后地址永远不变
- 引用字段中的指针始终是有效地址
- 不存在"旧地址"/"新地址"的概念
- 因此读操作直接 load 即可

### 7.3 Mark-Sweep 模式下的读操作实现

```cpp
#ifdef MRT_USE_MARK_SWEEP

// 所有读操作降级为直接 load（编译器可内联）
static inline BaseObject* MSReadRefField(BaseObject* obj, RefField<false>& field) {
    return field.GetTargetObject();
}

static inline BaseObject* MSReadStaticRef(RefField<false>& field) {
    return field.GetTargetObject();
}

static inline BaseObject* MSReadWeakRef(BaseObject* obj, RefField<false>& field) {
    return field.GetTargetObject();
}

static inline void MSReadStruct(MAddress dst, BaseObject* obj, MAddress src, size_t size) {
    memmove_s(dst, size, src, size);
}

#endif
```

### 7.4 CompilerCalls 中的读操作改造（可选优化）

> **注意**：以下改造是**可选的性能优化**，不影响正确性。不做改造时，读操作通过虚函数分派到 `MSIdleBarrier::ReadReference()` → `field.GetTargetObject()`，功能完全正确。`#ifdef` 方案仅为绕过一次虚函数调用开销。**所有函数 `extern "C"` 签名保持不变。**

**方案 A：条件编译（推荐初期使用）**

```cpp
extern "C" MRT_EXPORT BaseObject* CJ_MCC_ReadRefField(BaseObject* obj, RefField<false>& field) {
#ifdef MRT_USE_MARK_SWEEP
    return field.GetTargetObject();  // 直接返回
#else
    return Heap::GetBarrier().ReadReference(obj, field);  // 走 barrier 虚函数
#endif
}
```

**方案 B：快速路径判断（Copy/MS 共存期使用）**

```cpp
extern "C" MRT_EXPORT BaseObject* CJ_MCC_ReadRefField(BaseObject* obj, RefField<false>& field) {
    if (LIKELY(!Heap::UseBarrier())) {
        return field.GetTargetObject();  // 多数情况：GC 空闲
    }
    return Heap::GetBarrier().ReadReference(obj, field);
}
```

在 MS 模式下，`GetBarrier().ReadReference()` 本身也是直接 `field.GetTargetObject()`，所以即使走虚函数也没有实质开销，但避免虚函数调用可以帮助编译器更好地内联。

### 7.5 WeakRef 的特殊处理

弱引用在 Mark-Sweep 中需要在 Sweep 阶段清理（如果 referent 未被标记），但**读操作本身不需要屏障**：

```cpp
BaseObject* MSWriteBarrier::ReadWeakRef(BaseObject* obj, RefField<false>& field) const {
    BaseObject* referent = field.GetTargetObject();
    // 注意：不需要 RememberObjectInSatbBuffer（混合写屏障已覆盖方向）
    // 弱引用的 referent 如果在标记结束时未被标记，会在 clearWeakRef 阶段被清 null
    return referent;
}
```

### 7.6 编译器优化机会

移除读屏障后，编译器可以做更多优化：

1. **内联**：`CJ_MCC_ReadRefField` → `field.GetTargetObject()` → 单条 load 指令
2. **别名分析**：读操作不再有副作用（无 CAS/写入），编译器可以做更好的别名分析
3. **循环优化**：循环中的重复读引用可以被 LICM（Loop Invariant Code Motion）提出
4. **LLVM intrinsic 降级**：`llvm.cj.gcread.*` 系列 intrinsic 可降级为普通 load
5. **减少 safepoint**：读操作不再是潜在的 safepoint（不修改任何状态）

---

## 8. Tagged Pointer 移除方案

### 8.1 当前 Tagged Pointer 机制

**RefField 内存布局**（64 位平台，RefField.h L127-L133）：
```
[63:50] padding (14 bits)
[49]    tagID   (1 bit)    — 标记 ID（0/1 交替，区分当前/旧 GC 周期）
[48]    isTagged (1 bit)   — 是否被 GC 打了 tag
[47:0]  address  (48 bits) — 对象地址
```

**Tagged Pointer 的用途**：
1. **Forward 阶段**：GC 标记已转发的引用（`isTagged=1`），读屏障检测到后执行自愈
2. **Tag ID 翻转**：每轮 GC 后 `currentTagID ^= 1`，区分本轮和上轮的 tag
3. **IsOldPointer / IsCurrentPointer**：判断引用是否指向 from-space（需要 forward）

**涉及的核心方法**（WCollector.h L80-L170）：
| 方法 | 说明 |
|------|------|
| `FlipTagID()` | `currentTagID ^= 1`，GC 结束时翻转 |
| `IsOldPointer(field)` | `field.IsTagged() && field.GetTagID() != currentTagID` |
| `IsCurrentPointer(field)` | `field.IsTagged() && field.GetTagID() == currentTagID` |
| `GetAndTryTagRefField(ref)` | 将引用打上当前 tag |
| `TryUpdateRefField(...)` | 解析 tagged 引用 → forward → 写回新地址 |
| `TryUntagRefField(...)` | 去掉 tag，恢复原始地址 |
| `TryForwardRefField(...)` | Forward 阶段：查转发表 → 拷贝对象 → 更新引用 |

### 8.2 为什么 Mark-Sweep 不需要 Tagged Pointer

Mark-Sweep 中对象不移动，不存在 from-space/to-space 的概念：
- 不需要区分"已转发"/"未转发"引用
- 不需要自愈式读屏障来修正旧地址
- 不需要 tag ID 翻转来区分 GC 周期

因此，**整个 Tagged Pointer 机制可以完全移除**。指针就是地址，不含任何元数据。

### 8.3 代码层面的移除策略

#### 方案：条件编译

```cpp
// src/ObjectModel/RefField.h
#ifdef MRT_USE_MARK_SWEEP

// Mark-Sweep 模式：指针就是地址
template<bool isAtomic = false>
class RefField {
public:
    BaseObject* GetTargetObject() const {
        return reinterpret_cast<BaseObject*>(value_.load(std::memory_order_relaxed));
    }
    
    void SetTargetObject(BaseObject* obj) {
        value_.store(reinterpret_cast<uintptr_t>(obj), std::memory_order_relaxed);
    }
    
    uintptr_t GetFieldValue() const {
        return value_.load(std::memory_order_relaxed);
    }
    
    // 不再需要以下方法：
    // IsTagged(), GetTagID(), GetAddress()
    // 它们在 MS 模式下无意义
    
    // CAS 等原子操作保持不变
    bool CompareExchange(BaseObject*& expected, BaseObject* desired,
                         std::memory_order success = std::memory_order_seq_cst,
                         std::memory_order failure = std::memory_order_relaxed) {
        uintptr_t exp = reinterpret_cast<uintptr_t>(expected);
        uintptr_t des = reinterpret_cast<uintptr_t>(desired);
        bool result = value_.compare_exchange_strong(exp, des, success, failure);
        expected = reinterpret_cast<BaseObject*>(exp);
        return result;
    }
    
private:
    std::conditional_t<isAtomic, std::atomic<uintptr_t>, uintptr_t> value_;
};

#else
// 现有 Copy GC 的 Tagged RefField 保留
// ... existing RefField implementation ...
#endif
```

### 8.4 需要清理的 Tagged Pointer 使用点

通过全代码搜索，以下位置使用了 Tagged Pointer 相关逻辑：

| 文件 | 使用点 | MS 模式行动 |
|------|--------|------------|
| `WCollector.h/cpp` | `FlipTagID`, `IsOldPointer`, `IsCurrentPointer`, `GetAndTryTagRefField` | 条件编译跳过 |
| `IdleBarrier.cpp` | `IsTagged()` 检查 → forward | MSIdleBarrier 不检查 |
| `EnumBarrier.cpp` | `GetAndTryTagRefField` 打 tag | MSWriteBarrier 不打 tag |
| `TraceBarrier.cpp` | `IsOldPointer` → `TryUpdateRefField` | MSWriteBarrier 直接读 |
| `ForwardBarrier.cpp` | `TryForwardRefField` 循环 | MS 模式无此 phase |
| `CompilerCalls.cpp` | `MCC_WriteRefField` 中 tag 处理 | 条件编译跳过 |
| `RefField.inline.h` | `SetTargetObject` 中构造 tagged value | MS 模式直接存地址 |
| `Barrier.cpp` | 基类 `ReadReference` 中 `TryUpdateRefField` | `MSIdleBarrier` override |
| `ForwardDataManager` | 转发表管理 | MS 模式不使用 |

### 8.5 便捷宏定义

```cpp
// 统一的指针转换宏——MS 模式下为恒等变换
#ifdef MRT_USE_MARK_SWEEP
    #define REF_TO_ADDR(ref) (ref)                          // 指针即地址
    #define ADDR_TO_REF(addr) (addr)                        // 地址即指针
    #define IS_TAGGED_REF(field) (false)                    // 永远不 tagged
    #define NEED_FORWARD(field) (false)                     // 永远不需要 forward
#else
    #define REF_TO_ADDR(ref) RemoveTag(ref)
    #define ADDR_TO_REF(addr) AddTag(addr, currentTagID)
    #define IS_TAGGED_REF(field) ((field).IsTagged())
    #define NEED_FORWARD(field) (IsOldPointer(field))
#endif
```

### 8.6 风险评估

| 风险 | 说明 | 缓解 |
|------|------|------|
| 遗漏 tag 使用点 | 代码中可能有隐式的 tag 假设 | 搜索 `IsTagged|GetTagID|tagID|isTagged` 全部标注 |
| RefField 二进制兼容 | 修改 RefField 布局可能影响序列化 | 运行时不序列化 RefField，无影响 |
| 编译器生成的 tag 代码 | cjc 编译器可能生成依赖 tag 的代码 | 编译器需配合修改 intrinsic 降级 |

---

## 9. CompilerCalls 改造

> **核心约束：所有 CompilerCalls `extern "C"` 函数签名完全不变，无需修改对外接口。**
> 
> MSWriteBarrier 完全通过 `Barrier` 基类的虚函数分派机制工作，CompilerCalls 层面的调用链路（`MCC_WriteRefField → Heap::GetBarrier().WriteReference()`）保持不变。下文涉及的 `#ifdef MRT_USE_MARK_SWEEP` 条件编译均为**可选的性能优化**（绕过虚函数调用开销），不是正确性必需的。即使完全不修改 CompilerCalls.cpp，设计也能正确运行。

### 9.1 当前 CompilerCalls 架构

CompilerCalls（`src/CompilerCalls.cpp`）是**编译器生成代码与运行时的接口层**。cjc 编译器为每个引用读写操作生成对 `CJ_MCC_*` / `MCC_*` 函数的调用：

```
仓颉源码: obj.field = value
    ↓ (cjc 编译)
LLVM IR: call @MCC_WriteRefField(ref, obj, &field)
    ↓ (链接)
运行时: MCC_WriteRefField() → Heap::GetBarrier().WriteReference(obj, field, ref)
    ↓ (虚函数分派)
具体 Barrier 实现
```

### 9.2 需要改造的 CompilerCalls 函数

> **以下所有函数的 `extern "C"` 签名（函数名、参数列表、返回类型）完全不变。** "MS 模式行为"列描述的是通过虚函数分派自动生效的内部行为变化，不涉及函数签名修改。

| 函数 | 行号 | 当前行为 | MS 模式行为 |
|------|------|----------|------------|
| **写操作** | | | |
| `MCC_WriteRefField` | L282 | `GetBarrier().WriteReference()` | 不变（虚函数分派到 MSWriteBarrier） |
| `MCC_WriteStructField` | L291 | `GetBarrier().WriteStruct()` | 不变 |
| `MCC_WriteStaticRef` | L302 | `GetBarrier().WriteStaticRef()` | 不变 |
| `MCC_WriteStaticStruct` | L307 | `GetBarrier().WriteStaticStruct()` | 不变 |
| `MCC_AtomicWriteReference` | L355 | `GetBarrier().AtomicWriteReference()` | 不变 |
| `MCC_AtomicSwapReference` | L365 | `GetBarrier().AtomicSwapReference()` | 不变 |
| `MCC_AtomicCompareSwapReference` | L371 | `GetBarrier().CompareAndSwapReference()` | 不变 |
| **读操作** | | | |
| `CJ_MCC_ReadRefField` | L1550 | `GetBarrier().ReadReference()` | **直接返回**（条件编译/快速路径） |
| `CJ_MCC_ReadWeakRef` | L1555 | `GetBarrier().ReadWeakRef()` | **直接返回** |
| `CJ_MCC_ReadStructField` | L1559 | `GetBarrier().ReadStruct()` | **直接 memcpy** |
| `CJ_MCC_ReadStaticRef` | L1571 | `GetBarrier().ReadStaticRef()` | **直接返回** |
| `CJ_MCC_ReadStaticStruct` | L1575 | `GetBarrier().ReadStaticStruct()` | **直接 memcpy** |
| `CJ_MCC_ReadGeneric` | L1705 | `GetBarrier().ReadGeneric()` | **直接 memcpy** |
| `MCC_AtomicReadReference` | L360 | `GetBarrier().AtomicReadReference()` | **直接原子 load** |

### 9.3 写操作改造

写操作走 barrier 虚函数的架构不变，因为写屏障在 GC Mark 阶段确实需要额外逻辑。

> **写操作无需任何改造即可工作**：`MCC_WriteRefField` 函数签名和函数体均不变（仍然 `GetBarrier().WriteReference(...)`），差异完全由虚函数分派到 `MSWriteBarrier::WriteReference()` 内部消化。以下 `#ifdef` 示例仅为**可选的微优化**（在明确不需要屏障的场景提前短路），不是正确性前提。

**可选优化 1：MCC_WriteRefField 中的非堆对象短路**

```cpp
extern "C" MRT_EXPORT void MCC_WriteRefField(BaseObject* ref, BaseObject* obj, RefField<false>* fieldAddr) {
    RefField<false>& field = *fieldAddr;
    
#ifdef MRT_USE_MARK_SWEEP
    // MS 模式：非堆对象直接写，堆对象走 barrier
    if (LIKELY(obj == nullptr || !Heap::IsHeapObject(obj))) {
        field.SetTargetObject(ref);  // 栈/全局写：不需要屏障
        return;
    }
    Heap::GetBarrier().WriteReference(obj, field, ref);
#else
    // 现有逻辑保持不变
    if (LIKELY(obj == nullptr || !Heap::IsHeapObject(obj))) {
        field.SetTargetObject(ref);
        return;
    }
    Heap::GetBarrier().WriteReference(obj, field, ref);
#endif
}
```

**可选优化 2：CJ_MCC_ArrayCopyRef**

```cpp
extern "C" MRT_EXPORT void CJ_MCC_ArrayCopyRef(...) {
#ifdef MRT_USE_MARK_SWEEP
    // MS 模式：走 MSWriteBarrier::CopyRefArray（shade old+new + memcpy）
    Heap::GetBarrier().CopyRefArray(dstObj, dstField, dstSize, srcObj, srcField, srcSize);
#else
    // 现有逻辑
    Heap::GetBarrier().CopyRefArray(dstObj, dstField, dstSize, srcObj, srcField, srcSize);
#endif
}
```

实际上写操作的 CompilerCalls **签名和调用方式完全不变**，差异由 barrier 虚函数内部实现承担。即使不做上述可选优化，写操作也能通过虚函数分派正确工作。

### 9.4 读操作改造（可选优化）

> **重要**：读操作改造是**可选的性能优化**，不是正确性必需。不做任何改造时，读操作路径为：`CJ_MCC_ReadRefField → GetBarrier().ReadReference() → MSIdleBarrier::ReadReference() → field.GetTargetObject()`，通过虚函数分派自动生效，功能完全正确。以下 `#ifdef` 方案仅为绕过虚函数调用开销，在 LTO 环境下收益有限。

读操作是**可选**改造点——从虚函数调用变为**直接 load**：

```cpp
// CompilerCalls.cpp 中的读操作改造

extern "C" MRT_EXPORT BaseObject* CJ_MCC_ReadRefField(BaseObject* obj, RefField<false>& field) {
#ifdef MRT_USE_MARK_SWEEP
    return field.GetTargetObject();  // 直接返回，可被内联
#else
    return Heap::GetBarrier().ReadReference(obj, field);
#endif
}

extern "C" MRT_EXPORT BaseObject* CJ_MCC_ReadStaticRef(RefField<false>& field) {
#ifdef MRT_USE_MARK_SWEEP
    return field.GetTargetObject();
#else
    return Heap::GetBarrier().ReadStaticRef(field);
#endif
}

extern "C" MRT_EXPORT BaseObject* CJ_MCC_ReadWeakRef(BaseObject* obj, RefField<false>& field) {
#ifdef MRT_USE_MARK_SWEEP
    return field.GetTargetObject();
#else
    return Heap::GetBarrier().ReadWeakRef(obj, field);
#endif
}

extern "C" MRT_EXPORT void CJ_MCC_ReadStructField(MAddress dst, BaseObject* obj, 
                                                     MAddress src, size_t size) {
#ifdef MRT_USE_MARK_SWEEP
    memmove_s(dst, size, src, size);
#else
    Heap::GetBarrier().ReadStruct(dst, obj, src, size);
#endif
}

extern "C" MRT_EXPORT BaseObject* MCC_AtomicReadReference(BaseObject* obj, 
                                                            RefField<true>& field,
                                                            MemoryOrder order) {
#ifdef MRT_USE_MARK_SWEEP
    return field.GetTargetObject(order);
#else
    return Heap::GetBarrier().AtomicReadReference(obj, field, order);
#endif
}
```

### 9.5 编译器 Intrinsic 配合

当 `MRT_USE_MARK_SWEEP` 启用后，编译器（cjc）侧也应配合优化：

| Intrinsic | 当前 | MS 模式建议 |
|-----------|------|------------|
| `llvm.cj.gcread.ref` | 生成 `call CJ_MCC_ReadRefField` | 降级为普通 `load` 指令 |
| `llvm.cj.gcread.static` | 生成 `call CJ_MCC_ReadStaticRef` | 降级为普通 `load` 指令 |
| `llvm.cj.gcwrite.ref` | 生成 `call MCC_WriteRefField` | **保持**（写屏障仍需） |
| `llvm.cj.gcwrite.static` | 生成 `call MCC_WriteStaticRef` | **保持** |

**注意**：编译器优化是后续工作。初期阶段即使仍生成函数调用，运行时层面的实现也确保了正确性和较好的性能（因为 MS 模式下读操作函数体只有 1 条 load 指令，LTO 可以内联）。

---

## 10. 屏障安装与 Phase 联动

### 10.1 现有的屏障安装

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

### 10.2 新的屏障安装（Mark-Sweep 模式）

MS 模式只需 2 套屏障，安装逻辑大幅简化：

```cpp
void HeapImpl::InstallBarrierMS(const GCPhase phase) {
    switch (phase) {
    // 标记相关的三个阶段 → 安装混合写屏障
    case GCPhase::GC_PHASE_MS_MARK_SETUP:
    case GCPhase::GC_PHASE_MS_CONCURRENT_MARK:
    case GCPhase::GC_PHASE_MS_MARK_TERMINATION:
        currentBarrier = &msWriteBarrier_;
        break;
    
    // 其他所有阶段（IDLE、SWEEP 等）→ 空闲屏障
    default:
        currentBarrier = &msIdleBarrier_;
        break;
    }
}
```

### 10.3 Phase 状态与 Barrier 对应关系

```
┌─────────────────┐    ┌──────────────────┐
│   GC_PHASE_IDLE │───→│  MSIdleBarrier   │
│  (Off / Sweep)  │    │  (零开销读写)     │
└────────┬────────┘    └──────────────────┘
         │ Mark Setup STW
         ▼
┌─────────────────┐    ┌──────────────────┐
│ MS_MARK_SETUP   │───→│  MSWriteBarrier  │
│  (STW)          │    │  (shade old+new) │
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

### 10.4 HeapImpl 持有的 Barrier 实例

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

### 10.5 屏障切换的触发点

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

### 10.6 Barrier 生效时序保证

**问题**：切换 barrier 指针后，正在执行 barrier 的 mutator 如何安全过渡？

**保证**：
1. **STW 切换**：所有 mutator 已暂停，切换后才恢复 → 不存在并发问题
2. **非 STW 切换**：`InstallBarrierMS` 修改 `currentBarrier` 指针（单指针原子写），mutator 下一次 `GetBarrier()` 即获取新 barrier → 可能有 1-2 次旧 barrier 的尾部调用

**MS 模式下的安全性**：
- Off → On：某个 mutator 可能用 MSIdleBarrier 执行了一次没有 shade 的写 → 不影响，因为 Mark Setup 是 STW，此时 mutator 已暂停
- On → Off：某个 mutator 可能用 MSWriteBarrier 多做了一次 shade → 无害（多标不少标）

**结论**：2 套 barrier 的切换比 6 套更安全，因为 Off→On 总是在 STW 中发生，On→Off 的多余 shade 是无害的。

---

## 11. 与现有基础设施的复用与改造清单

### 11.1 总览矩阵

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

### 11.2 新增文件

| 文件 | 说明 |
|------|------|
| `src/Heap/MSBarrier/MSIdleBarrier.h` | GC 空闲期屏障声明 |
| `src/Heap/MSBarrier/MSIdleBarrier.cpp` | GC 空闲期屏障实现 |
| `src/Heap/MSBarrier/MSWriteBarrier.h` | 混合写屏障声明 |
| `src/Heap/MSBarrier/MSWriteBarrier.cpp` | 混合写屏障实现 |
| `src/Heap/MSBarrier/WriteBarrierBuffer.h` | Per-thread 写屏障缓冲区 |

### 11.3 需修改的现有文件

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

### 11.4 可完全删除的代码（MS 模式启用后）

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

### 11.5 改造优先级

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

## 12. 验证方案

### 12.1 单元测试

#### 12.1.1 MSWriteBarrier 正确性

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

#### 12.1.2 WriteBarrierBuffer 测试

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

#### 12.1.3 RefField 简化验证

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

### 12.2 并发安全测试

#### 12.2.1 多线程写屏障压力测试

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

#### 12.2.2 屏障切换竞态测试

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

### 12.3 GC 完整性测试

#### 12.3.1 混合写屏障 + 并发标记集成测试

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

#### 12.3.2 长时间运行稳定性测试

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

### 12.4 性能基准测试

| 测试项 | 指标 | 对比基线 | 目标 |
|--------|------|----------|------|
| 读操作延迟 | 单次 RefField 读取耗时 | Copy GC（带 tag 解码） | 降低 15-30% |
| 写操作延迟 | 单次 WriteReference 耗时 | Copy GC TraceBarrier | 持平或 +10% 以内 |
| WBBuf flush 频率 | 每秒 flush 次数 | SatbBuffer retire 频率 | 持平或更低 |
| 屏障切换耗时 | InstallBarrierMS 单次耗时 | InstallBarrier 当前实现 | 持平 |
| 整体吞吐量 | 应用 QPS / 事务/秒 | Copy GC 完整运行 | 持平或提升 |

### 12.5 回归验证清单

| 验证项 | 方法 | 通过标准 |
|--------|------|----------|
| gc.cj 基准测试 | `cjc gc.cj && ./main` | 输出正确，无崩溃 |
| gc_large.cj 大对象 | `cjc gc_large.cj && ./main` | 大对象正常回收 |
| gc_mixed.cj 混合 | `cjc gc_mixed.cj && ./main` | 小对象+大对象混合正常 |
| WeakRef 语义 | 自定义 WeakRef 测试 | GC 后弱引用正确清理 |
| Finalizer 执行 | 带 finalize() 的对象 | 回收时 finalizer 正确执行 |
| ASAN/TSAN 检查 | 编译带 sanitizer 运行 | 0 error，0 warning |
| 编译宏切换 | 分别编译 CopyGC / MS | 两种模式均编译通过 |
