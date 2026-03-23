# Step 7：CopyGC 与 MS GC 阶段对比分析

> 本文档对比 CopyGC（WCollector）和 MS GC（MarkSweepCollector）在 Enum、Mark、SATB/写屏障三个阶段的实现差异，
> 并深入对比两套屏障体系的架构与具体实现。

---

## 目录

1. [Enum（根枚举）阶段对比](#1-enum根枚举阶段对比)
2. [Mark/Trace（并发标记）阶段对比](#2-marktrace并发标记阶段对比)
3. [SATB / 写屏障缓冲区对比](#3-satb--写屏障缓冲区对比)
4. [Barrier 体系架构对比](#4-barrier-体系架构对比)
5. [各 Barrier 实现逐项对比](#5-各-barrier-实现逐项对比)
6. [Barrier 方法级行为矩阵](#6-barrier-方法级行为矩阵)
7. [架构差异根源总结](#7-架构差异根源总结)

---

## 1. Enum（根枚举）阶段对比

### 1.1 CopyGC (WCollector)

**流程**：`DoGarbageCollection()` → `TraceHeap()` → STW `DoEnumeration()` @ `GC_PHASE_ENUM`

**核心逻辑**（WCollector.cpp）：
- `EnumRefFieldRoot()` 检查引用是否已带 `currentTagID`
- 若是旧 Tag 或未 Tag → 调用 `FindLatestVersion()` 找到对象最新版本（可能经过 Forward）
- 用 **CAS** 给引用字段打上新 Tag：`field.CompareExchange()`
- 最终将对象加入 `rootSet`

**屏障**：使用 `EnumBarrier`，读操作需解析 Tagged Pointer，写操作记 SATB（新旧值都记）

### 1.2 MS GC (MarkSweepCollector)

**流程**：`RunGarbageCollection()` → `MarkSetup()` 内分 4 步：
1. **Phase A**：等前一轮 Sweep 完成（`ForceFinishSweep`）
2. **Phase B**：短暂 STW → Flush 线程本地分配缓冲 + 清空 Mark Bitmap
3. **Phase C**：Handshake 切换到 `GC_PHASE_MS_MARK_SETUP`
4. **Phase D**：`EnumAndMarkRoots()` 枚举并标记根

**核心逻辑**（MarkSweepCollector.cpp）：
- `EnumRefFieldRoot()` 直接 `field.GetTargetObject()` 拿到对象指针，**无需 Tag 处理**
- 枚举完毕后对每个根调用 `MarkObject(obj)` → 在 `MSRegionInfo::markBitmap` 中原子 CAS 置位

**屏障**：此阶段用 `MSIdleBarrier`，读写都是直接操作，**零开销**

### 1.3 关键差异

| 维度 | CopyGC | MS GC |
|------|--------|-------|
| 指针处理 | 查最新版本 + CAS 打 Tag | 直接读指针，无 Tag |
| 标记方式 | Tag 编码在指针位中 | 独立 mark bitmap |
| 屏障开销 | EnumBarrier（解 Tag + SATB） | MSIdleBarrier（零开销） |
| Forward 处理 | 需要 `FindLatestVersion()` | 不移动对象，无需 Forward |
| STW 范围 | `DoEnumeration()` 整体 STW | 仅 Phase B 短暂 STW，Phase D 非 STW |

---

## 2. Mark/Trace（并发标记）阶段对比

### 2.1 CopyGC (WCollector)

**流程**：`TraceHeap()` → 切换到 `GC_PHASE_TRACE` → `DoTracing()`

**核心逻辑**：
- `TraceRefField()` 对每个引用字段：
  1. 检查是否是 `currentTagID` → 已处理则跳过
  2. 若是 `oldTagID` → `FindLatestVersion()` 找到 Forward 后的地址
  3. **CAS 更新字段**为新 Tag 版本
  4. 未标记则入 workStack
- **单线程执行**，无 Mutator Assist
- 收敛是隐式的（单线程 drain 完 workStack 即完成）

**屏障**：`TraceBarrier`，读操作需解析 Tagged Pointer，写操作执行 SATB 记旧值 + 对新值打 Tag

### 2.2 MS GC (MarkSweepCollector)

**流程**：`ConcurrentMark()` → 切换到 `GC_PHASE_MS_CONCURRENT_MARK` → `RunConcurrentMarkLoop()`

**核心逻辑**：
- `TraceRefField()` 极其简洁：
  ```cpp
  BaseObject* target = field.GetTargetObject();  // 直接 load，无 barrier
  if (target == nullptr || !IsManagedAddress(target)) return;
  if (!MarkObject(target)) {  // bitmap CAS，返回是否已标记
      workStack.push_back(target);  // 新标记才入栈
  }
  ```
- **GC 线程 + Mutator Assist 双通道**：
  - GC 线程：`RunCollectorDrainSlice()` 每次处理 `kSliceMaxObjects`(256) 个对象
  - GC 线程超过阈值时 `ShareWorkToGlobal()` 分一半给全局队列
  - Mutator 分配时调用 `MSMarkAssist()` 从全局队列取工作，限制 64 对象/次
- **收敛检测**：全局队列和本地栈连续 3 轮为空才结束

**屏障**：`MSWriteBarrier`（混合写屏障），读操作直接 load **零开销**，写操作 shade(old) + shade(new)

### 2.3 关键差异

| 维度 | CopyGC | MS GC |
|------|--------|-------|
| 并发模型 | **单线程**顺序标记 | GC 线程 + Mutator Assist **并行** |
| TraceRefField | 解 Tag + FindLatest + CAS 更新 | 直接 load + bitmap CAS |
| 工作队列 | 本地 workStack | 全局 `globalWorkQueue_` + 本地栈 |
| 收敛机制 | 隐式（drain 完即止） | 显式（3 轮稳定检测） |
| 读屏障 | 有（Tag 检查 + 可能 Forward） | **无**（直接 load） |
| 写屏障 | SATB 记旧值 + Tag 编码 | 混合写屏障（shade old + new） |
| 每对象标记开销 | Tag CAS + LiveInfo 位图 | 仅 mark bitmap CAS |

---

## 3. SATB / 写屏障缓冲区对比

### 3.1 CopyGC — SatbBuffer

**架构**（`src/Mutator/SatbBuffer.h`）：
- **全局单例** `SatbBuffer::Instance()`
- 每个 Node 固定 64 个槽位存对象指针
- 三个链表：`freeNodes`（空闲）、`retiredNodes`（已满待处理）、`arena`（内存页池）
- 每个 Mutator 持有一个 `satbNode`

**写入路径**：
```
WriteBarrier 触发 → mutator->RememberObjectInSatbBuffer(oldVal)
  → 检查 IsHeapAddress
  → ShouldEnqueue?
  → EnsureGoodNode(node)  // 满则 retire 到全局 retiredNodes
  → node->Push(obj)        // 写入线程本地 Node
```

**处理时机**：
- `MarkTermination` STW 内：`SatbBuffer::GetRetiredObjects()` → 统一遍历标记
- `PostTrace()` 调用 `SatbBuffer::ClearBuffer()` 清理

**特点**：
- 仅记 **旧值**（Trace 阶段）或 **旧+新值**（Enum 阶段）
- Node 满了才 retire → **延迟处理**（STW 才集中消费）
- 需要全局锁操作 `LockedList`

### 3.2 MS GC — WriteBarrierBuffer

**架构**（`src/Heap/MSBarrier/WriteBarrierBuffer.h`）：
- **Per-Mutator TLS**，固定 256 entries（2KB），内嵌在 Mutator 中
- 纯数组 + 计数器，**无锁**（单写者只有自己的线程写）

**写入路径**：
```
MSWriteBarrier::WriteReference 触发
  → shade(old): MarkAndEnqueue(oldRef, wbBuf)
     → bitmap CAS 标记
     → 新标记的才 Push 到 wbBuf
  → store: field.SetTargetObject(ref)  // 直接写，无 Tag
  → shade(new): EnqueueForRescan(obj, wbBuf)
     → 无条件 Push（保证新值的子对象被扫描）
```

**溢出处理**：
```
wbBuf 满 → FlushWriteBarrierOverflow()
  → 单次加锁 globalWorkMutex_
  → 批量转移到 globalWorkQueue_
  → wbBuf.Clear()
```

**处理时机**：
- **并发标记期间实时注入**：溢出时立即 flush 到全局工作队列，GC 线程/Assist 实时消费
- `MarkTermination` STW：`FlushAllWriteBarrierBuffers()` 扫描所有 Mutator 的 buffer 做最终兜底

### 3.3 关键差异

| 维度 | CopyGC SatbBuffer | MS GC WriteBarrierBuffer |
|------|-------------------|--------------------------|
| 作用域 | 全局单例 + per-Mutator Node | Per-Mutator TLS（内嵌） |
| 容量 | 64 pointers / Node | 256 entries (2KB) |
| 锁开销 | retire 时需 `LockedList` 全局锁 | Push 无锁，flush 时单次 mutex |
| 记录内容 | 仅旧值（Trace）或旧+新值（Enum） | 旧值 + 新值（始终） |
| 消费时机 | **延迟**：MarkTermination STW 统一处理 | **实时**：溢出即 flush 到工作队列 |
| 与标记交互 | GC 线程 STW 后批量扫描 | 并发标记期间 GC/Assist 实时消费 |
| 内存分配 | 动态（arena 页池分配 Node） | 静态（编译时固定在 Mutator 内） |
| 标记位置 | Buffer 不做标记，只记录 → STW 后标记 | **Push 前就做 bitmap CAS 标记** |

---

## 4. Barrier 体系架构对比

### 4.1 CopyGC：6 套 Barrier 体系

CopyGC 因对象会移动（Copying），需要在不同 GC Phase 使用不同屏障处理 Tagged Pointer 和 Forward 逻辑：

```
Barrier (基类, src/Heap/Barrier/Barrier.h)
  └── IdleBarrier           (GC_PHASE_IDLE)        — 自愈读屏障 + 直接写
        ├── EnumBarrier       (GC_PHASE_ENUM)        — SATB 记旧+新值 + Tag 编码
        ├── TraceBarrier      (GC_PHASE_TRACE)       — SATB 记旧值 + Tag 编码
        ├── PostTraceBarrier  (GC_PHASE_POST_TRACE)  — 弱引用清理 + Untag
        ├── PreforwardBarrier (GC_PHASE_PREFORWARD)  — 提前 Forward + CAS
        └── ForwardBarrier    (GC_PHASE_FORWARD)     — 并发复制 Forward
```

**关键特征**：
- **所有 Phase 都有读屏障**（至少 Tag 检查），Forward 阶段读屏障最重（触发对象复制 + CAS）
- 写屏障在 Enum/Trace 阶段使用 SATB，写入时需要 `seq_cst` fence + Tag 编码
- 屏障安装通过 `HeapImpl::InstallBarrier(phase)` 根据 6 个 Phase 切换
- 每个 Phase 的屏障行为差异较大，逻辑分散在 6 个 `.cpp` 文件中

### 4.2 MS GC：2 套 Barrier 体系

MS GC 因对象不移动，极大简化了屏障体系：

```
Barrier (基类)
  ├── MSIdleBarrier     (GC 空闲 / Sweep 阶段)  — 所有操作直接读写，零开销
  └── MSWriteBarrier    (GC Mark 阶段)          — 写操作 shade(old)+shade(new)，读操作直接返回
```

**关键特征**：
- **读操作全程零开销**：所有 Phase 的 `Read*` 方法都是直接 `field.GetTargetObject()`
- 写屏障仅在标记期间激活，使用 Go 风格混合写屏障（Hybrid Write Barrier）
- 屏障安装通过 `HeapImpl::InstallBarrierMS(phase)` 仅需 2 分支
- 无 Tagged Pointer、无 Forward、无 `seq_cst` fence

### 4.3 屏障安装机制对比

**CopyGC**：
```cpp
void HeapImpl::InstallBarrier(const GCPhase phase) {
    if      (phase == GC_PHASE_ENUM)        → enumBarrier
    else if (phase == GC_PHASE_TRACE)       → traceBarrier
    else if (phase == GC_PHASE_CLEAR_SATB)  → traceBarrier
    else if (phase == GC_PHASE_POST_TRACE)  → postTraceBarrier
    else if (phase == GC_PHASE_PREFORWARD)  → preforwardBarrier
    else if (phase == GC_PHASE_FORWARD)     → forwardBarrier
    else if (phase == GC_PHASE_IDLE)        → idleBarrier
}
```

**MS GC**：
```cpp
void HeapImpl::InstallBarrierMS(const GCPhase phase) {
    switch (phase) {
    case MS_PHASE_MARK_SETUP:
    case MS_PHASE_MARK:
    case MS_PHASE_MARK_TERMINATION:
        currentBarrier = &msWriteBarrier_;
        break;
    default:  // IDLE、SWEEP 等
        currentBarrier = &msIdleBarrier_;
        break;
    }
}
```

### 4.4 屏障切换安全性

| 维度 | CopyGC | MS GC |
|------|--------|-------|
| 切换次数/GC 周期 | 6 次（IDLE→ENUM→TRACE→POST→PRE→FWD→IDLE） | 2 次（IDLE→MARK→IDLE） |
| STW 切换 | 大部分 Phase 需要 STW | 仅 MarkTermination STW |
| 非 STW 安全性 | 复杂（6 套屏障交叉） | 简单（Off→On 通过 Handshake，On→Off 多余 shade 无害） |

---

## 5. 各 Barrier 实现逐项对比

### 5.1 Idle 阶段屏障

#### CopyGC — IdleBarrier (`src/Heap/WCollector/IdleBarrier.cpp`)

即使 GC 空闲，**读操作也需要处理 Tagged Pointer**：

```cpp
BaseObject* IdleBarrier::ReadReference(BaseObject* obj, RefField<false>& field) const {
    while (true) {
        RefField<> tmpField(field);
        if (!tmpField.IsTagged()) {
            return tmpField.GetTargetObject();          // 无 tag → 直接返回
        }
        if (theCollector.IsCurrentPointer(tmpField)) {  // 当前周期 tag → untag
            if (theCollector.TryUntagRefField(obj, field, tmpField)) {
                return tmpField.GetAddress();
            }
        } else {                                         // 旧周期 tag → forward
            BaseObject* toVersion = nullptr;
            if (theCollector.TryUpdateRefField(obj, tmpField, toVersion)) {
                return toVersion;
            }
        }
    }
}
```

- 每次读至少 1 次 `IsTagged()` 检查
- 可能触发 `TryUpdateRefField()`（CAS 更新为 Forward 后地址）
- 写操作直接 store（无 SATB）

#### MS GC — MSIdleBarrier (`src/Heap/MSBarrier/MSIdleBarrier.h`)

所有操作都是**直接内存访问**，完全无开销：

```cpp
BaseObject* MSIdleBarrier::ReadReference(BaseObject*, RefField<false>& field) const {
    return field.GetTargetObject();  // 直接返回
}

void MSIdleBarrier::WriteReference(BaseObject*, RefField<false>& field, BaseObject* ref) const {
    field.SetTargetObject(ref);      // 直接写入
}
```

**差异总结**：CopyGC Idle 读有 Tag 检查 + 可能 CAS；MS GC Idle 读写完全透明。

---

### 5.2 标记阶段屏障

#### CopyGC — TraceBarrier (`src/Heap/WCollector/TraceBarrier.cpp`)

**读操作**：需要区分 Current/Old Pointer 并做 Forward：
```cpp
BaseObject* TraceBarrier::ReadReference(BaseObject* obj, RefField<false>& field) const {
    RefField<> tmpField(field);
    if (theCollector.IsCurrentPointer(tmpField)) {
        return tmpField.GetTargetObject();
    }
    if (theCollector.IsOldPointer(tmpField)) {
        return theCollector.FindToVersion(tmpField.GetTargetObject());  // Forward
    }
    return tmpField.GetTargetObject();
}
```

**写操作**：SATB 记旧值 + seq_cst fence + Tag 编码：
```cpp
void TraceBarrier::WriteReference(BaseObject* obj, RefField<false>& field, BaseObject* ref) const {
    // 1. 读旧值（可能需 Forward）
    RefField<> oldField(field);
    BaseObject* rememberedObject = ...;  // 解析旧值
    
    // 2. SATB: 记录旧值
    mutator->RememberObjectInSatbBuffer(rememberedObject);
    
    // 3. seq_cst fence + Tag 编码 + 写入
    std::atomic_thread_fence(std::memory_order_seq_cst);
    RefField<> newField = theCollector.GetAndTryTagRefField(ref);
    field.SetFieldValue(newField.GetFieldValue());
}
```

#### CopyGC — EnumBarrier (`src/Heap/WCollector/EnumBarrier.cpp`)

与 TraceBarrier 类似，**但同时记录新旧值**到 SATB：
```cpp
void EnumBarrier::WriteReference(...) const {
    // SATB 记录旧值
    mutator->RememberObjectInSatbBuffer(rememberedObject);
    // SATB 同时记录新值（比 TraceBarrier 多这一步）
    if (ref != nullptr) {
        mutator->RememberObjectInSatbBuffer(ref);
    }
    // Tag + store
    ...
}
```

#### MS GC — MSWriteBarrier (`src/Heap/MSBarrier/MSWriteBarrier.cpp`)

**读操作**：直接返回，零开销
```cpp
BaseObject* MSWriteBarrier::ReadReference(BaseObject*, RefField<false>& field) const {
    return field.GetTargetObject();  // 直接 load
}
```

**写操作**：混合写屏障（shade old + shade new）：
```cpp
void MSWriteBarrier::WriteReference(BaseObject* obj, RefField<false>& field, BaseObject* ref) const {
    WriteBarrierBuffer& wbBuf = GetWBBuf();
    
    // shade(old): 删除屏障 — 防止旧引用目标被漏标
    BaseObject* oldRef = field.GetTargetObject();
    MarkAndEnqueue(oldRef, wbBuf);
    
    // 直接写入（无 Tag、无 fence）
    field.SetTargetObject(ref);
    
    // shade(new): 插入屏障 — 防止新引用目标被漏标
    if (obj != nullptr) {
        EnqueueForRescan(obj, wbBuf);   // 容器对象：确保子对象被扫描
    } else {
        MarkAndEnqueue(ref, wbBuf);     // 静态引用：直接标记新值
    }
}
```

**核心辅助方法 — MarkAndEnqueue**：
```cpp
void MSWriteBarrier::MarkAndEnqueue(BaseObject* obj, WriteBarrierBuffer& wbBuf) {
    if (obj == nullptr || !IsHeapAddress(obj)) return;
    // Phase 门控：仅标记阶段生效
    GCPhase phase = Mutator::GetMutator()->GetMutatorPhase();
    if (phase != MARK_SETUP && phase != CONCURRENT_MARK && phase != MARK_TERMINATION) return;
    // Bitmap CAS 标记 + 入缓冲
    if (!RegionSpace::MarkObject(obj)) {  // 新标记
        if (wbBuf.IsFull()) FlushWriteBarrierOverflow(wbBuf);
        wbBuf.Push(obj);
    }
}
```

**差异总结**：

| 维度 | CopyGC TraceBarrier | CopyGC EnumBarrier | MS GC MSWriteBarrier |
|------|--------------------|--------------------|---------------------|
| 读操作 | Tag 检查 + Forward | Tag 检查 + Forward | **直接 load** |
| 写操作—旧值 | SATB 记录 | SATB 记录 | bitmap CAS + WBBuf |
| 写操作—新值 | Tag 编码写入 | SATB 记录 + Tag 编码 | EnqueueForRescan / MarkAndEnqueue |
| 内存序 | `seq_cst` fence | `seq_cst` fence | **无 fence** |
| 指针处理 | Tag + Forward | Tag + Forward | 原始指针 |

---

### 5.3 Forward / 复制阶段屏障（CopyGC 独有）

MS GC **不存在此阶段**（对象不移动），以下为 CopyGC 独有的 3 套屏障：

#### ForwardBarrier (`src/Heap/WCollector/ForwardBarrier.cpp`)

**用于**：`GC_PHASE_FORWARD`，并发复制阶段

**读操作**（最重的热路径）：
```cpp
BaseObject* ForwardBarrier::ReadReference(BaseObject* obj, RefField<false>& field) const {
    while (true) {
        RefField<> tmpField(field);
        if (!tmpField.IsTagged()) return tmpField.GetTargetObject();
        if (theCollector.IsUnmovable(tmpField)) {
            if (theCollector.TryUntagRefField(obj, field, tmpField))
                return tmpField.GetAddress();         // 不可移动 → only untag
        } else {
            if (theCollector.TryForwardRefField(obj, field, tmpField))
                return tmpField.GetAddress();         // 可移动 → 触发对象复制 + CAS
        }
    }
}
```

- `TryForwardRefField` 包含：查找 ForwardTable → 如未转发则 CopyObject → CAS 更新字段
- 这是 **CopyGC 最大的 mutator 性能瓶颈**

#### PreforwardBarrier (`src/Heap/WCollector/PreforwardBarrier.cpp`)

**用于**：`GC_PHASE_PREFORWARD`，预转发根修正

读操作类似 ForwardBarrier，但包含手动 `ForwardObject()` + CAS 回退逻辑

#### PostTraceBarrier (`src/Heap/WCollector/PostTraceBarrier.cpp`)

**用于**：`GC_PHASE_POST_TRACE`，标记后弱引用处理

特殊方法 `ReadWeakRef()`：清理未标记的弱引用 referent
```cpp
BaseObject* PostTraceBarrier::ReadWeakRef(BaseObject* obj, RefField<false>& field) const {
    RefField<> tmpField(field);
    BaseObject* referent = tmpField.GetTargetObject();
    if (!theCollector.IsMarkedObject(referent)) {
        field.SetTargetObject(nullptr);  // 清理不可达的弱引用
        return nullptr;
    }
    return referent;
}
```

---

### 5.4 MS GC 弱引用处理对比

MS GC 的弱引用在 `PostMarkTermination()` 阶段统一处理，不需要专门的 Barrier：
- Sweep 阶段通过 bitmap 判断对象是否存活
- WeakRef referent 未标记 → 在 `ProcessWeakReferences()` 中清空
- 无需 PostTraceBarrier 这样的专用屏障

---

## 6. Barrier 方法级行为矩阵

### 6.1 读操作（Read）

| 方法 | CopyGC IdleBarrier | CopyGC TraceBarrier | CopyGC ForwardBarrier | MS MSIdleBarrier | MS MSWriteBarrier |
|------|-------------------|--------------------|-----------------------|-----------------|------------------|
| `ReadReference` | Tag 检查 + Untag/Forward loop | Current/Old 判断 + Forward | **对象复制** + CAS Forward loop | 直接 load | 直接 load |
| `ReadStaticRef` | Tag 检查 + Untag/Forward | 同上 | 同上 | 直接 load | 直接 load |
| `ReadWeakRef` | Tag 检查 + Untag/Forward | 同上 | 同上 | 直接 load | 直接 load |
| `ReadStruct` | ForEach ref: ReadRef → memcpy | 同 Idle | 同 Idle | memcpy | memcpy |
| `ReadStaticStruct` | 同上 | 同上 | 同上 | memcpy | memcpy |
| `AtomicReadReference` | Atomic get + Untag/Forward | Atomic + Forward | Atomic + Forward + CAS | Atomic get | Atomic get |
| `ReadGeneric` | 复合：heap 检查 + ReadRef | 复合 + Forward | 复合 + Forward | memcpy | memcpy |

**核心差异**：CopyGC 所有 Phase 读操作都需 Tag 检查（最轻 1 次分支，最重循环 CAS + 对象复制）；MS GC 读操作**始终是直接 load，零额外指令**。

### 6.2 写操作（Write）

| 方法 | CopyGC EnumBarrier | CopyGC TraceBarrier | MS MSIdleBarrier | MS MSWriteBarrier |
|------|-------------------|--------------------|-----------------|-----------------|
| `WriteReference` | SATB(old+new) + fence + Tag | SATB(old) + fence + Tag | 直接 store | shade(old) + store + shade(new) |
| `WriteStaticRef` | fence + Tag + store | fence + Tag + store | 直接 store | shade(old) + store + MarkAndEnqueue(new) |
| `WriteStruct` | ForEach: SATB(old+new) + memcpy + Tag | ForEach: SATB(old) + memcpy + Tag | memmove | ForEach: shade(old) → memmove → shade(new) |
| `WriteStaticStruct` | 同上（bitmap walk） | 同上 | memmove | bitmap walk: shade(old) → memmove → shade(new) |
| `CopyRefArray` | 逐元素: SATB + Tag | 逐元素: SATB + Tag | memmove | Loop: shade(old) all → memmove → shade(new) all |
| `CopyStructArray` | 逐 struct ref: SATB + Tag | 逐 struct ref: SATB + Tag | memmove | ForEach ref in range: shade(old) → memmove → shade(new) |
| `AtomicWriteReference` | SATB(old+new) + fence + Tag + atomic store | SATB(old) + fence + Tag + atomic store | Atomic store | shade(old) + atomic store + EnqueueForRescan |
| `AtomicSwapReference` | Exchange + SATB(old+new) + Tag | Exchange + SATB(old) | Atomic exchange | Atomic exchange → shade(old swapped) |
| `CompareAndSwapReference` | CAS + SATB(old+new) | CAS + SATB(old) | CAS | CAS success → shade(old) + EnqueueForRescan |
| `WriteGeneric` | 复合：heap 检查 + WriteRef | 复合 + SATB + Tag | memcpy | 复合 + shade(old+new) |

**核心差异**：
- CopyGC 写操作需要 `seq_cst` fence（每次写！）+ Tagged Pointer 编码，Enum 阶段还要额外记新值到 SATB
- MS GC 写操作无 fence、无 Tag，仅在标记期间 shade(old+new)，且 shade 本身只需一次 bitmap CAS

### 6.3 各 Barrier 使用阶段映射

```
CopyGC GC 周期:
  IDLE ──→ ENUM ──→ TRACE ──→ CLEAR_SATB ──→ POST_TRACE ──→ PREFORWARD ──→ FORWARD ──→ IDLE
  Idle      Enum     Trace      Trace         PostTrace      Preforward     Forward     Idle

MS GC 周期:
  IDLE ──→ MARK_SETUP ──→ CONCURRENT_MARK ──→ MARK_TERMINATION ──→ SWEEP ──→ IDLE
  Idle      MSWrite         MSWrite              MSWrite            Idle      Idle
```

---

## 7. 架构差异根源总结

### 7.1 根本差异：对象是否移动

CopyGC 和 MS GC 所有实现差异都源于一个根本事实：

| | CopyGC | MS GC |
|--|--------|-------|
| **对象移动** | ✅ 对象从 from-space 复制到 to-space | ❌ 对象分配后地址不变 |

由此派生出以下所有差异：

### 7.2 派生差异链

```
对象移动（Copy）                          对象不移动（Mark-Sweep）
  │                                       │
  ├─ 需要 Tagged Pointer 区分新旧版本       ├─ 指针就是地址，无需编码
  │    └─ 读屏障必须检查 Tag                 │    └─ 读操作零开销
  │    └─ 写屏障需要 Tag 编码 + fence        │    └─ 写操作无 fence
  │                                       │
  ├─ Forward 阶段需要对象复制               ├─ 无 Forward，无对象复制
  │    └─ ForwardBarrier 读屏障极重          │    └─ 无此阶段
  │    └─ 需要 ForwardTable 和 CopyObject   │
  │                                       │
  ├─ 6 个 GC Phase → 6 套 Barrier          ├─ 2 个 GC 状态 → 2 套 Barrier
  │    └─ 大量代码重复和切换逻辑             │    └─ 架构简洁，易维护
  │                                       │
  ├─ 单线程标记（Trace 期间更新指针）        ├─ 并发标记 + Mutator Assist
  │    └─ 标记和 Forward 强耦合             │    └─ 标记独立于 Sweep
  │                                       │
  ├─ SATB Buffer 延迟处理（STW 后统一）      ├─ WBBuf 实时处理（溢出即 flush）
  │    └─ 全局锁 LockedList                 │    └─ TLS 无锁 + 单次 mutex flush
  │                                       │
  └─ 内存紧凑（无碎片）                     └─ 可能产生碎片（需 Sweep 回收空闲 Region）
```

### 7.3 性能影响对比

| 指标 | CopyGC | MS GC | 优势方 |
|------|--------|-------|--------|
| **Mutator 读开销** | Tag 检查（~1-3 指令），Forward 阶段极重 | 零额外指令 | **MS GC** |
| **Mutator 写开销** | SATB + seq_cst fence + Tag 编码 | shade(old+new) bitmap CAS | **MS GC** |
| **GC 标记吞吐** | 单线程 | GC 线程 + Mutator Assist | **MS GC** |
| **STW 停顿** | 多个 STW 阶段（Enum, PostTrace, Preforward） | 主要 2 次 STW（MarkSetup 短, MarkTermination） | **MS GC** |
| **内存紧凑性** | 复制后无碎片 | 可能碎片化 | **CopyGC** |
| **大对象处理** | 大对象不移动（pin） | 统一处理 | 持平 |
| **屏障代码量** | ~1950 行（6 套） | ~800 行（2 套） | **MS GC** |
| **浮动垃圾** | SATB 仅记旧值，较少 | shade(old+new) 稍多 | **CopyGC** |

### 7.4 代码复杂度对比

| 维度 | CopyGC | MS GC |
|------|--------|-------|
| Barrier 类数量 | 6 个 | 2 个 |
| 虚方法覆盖总数 | ~90 个（6×15） | ~30 个（2×15） |
| Tagged Pointer 相关代码 | ~300 行分散 | 0 |
| Forward 相关代码 | ~750 行 | 0 |
| 屏障切换分支 | 7 个 phase 分支 | 2 个 phase 分支 |
| 总净代码量 | ~2750 行 | ~800 行 |
