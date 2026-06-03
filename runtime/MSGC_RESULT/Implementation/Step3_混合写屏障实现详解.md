# Step3 混合写屏障实现详解

> 位置：MSGC Step3 的实现说明文档。
> 目的：结合当前 runtime 源码，说明写屏障系统现在到底是怎么工作的。
> 口径：**以当前代码为准**，对照 Plan 文档（`MSGC/Step3_*.md`）标注设计-实现差异。

---

## 目录

1. [总览](#1-总览)
2. [MSIdleBarrier — 零开销空闲屏障](#2-msidlebarrier--零开销空闲屏障)
3. [MSMarkBarrier — 混合写屏障](#3-msmarkbarrier--混合写屏障)
4. [WriteBarrierBuffer — Per-Mutator 灰对象暂存区](#4-writebarrierbuffer--per-mutator-灰对象暂存区)
5. [屏障安装与 Phase 联动](#5-屏障安装与-phase-联动)
6. [CompilerCalls 集成](#6-compilercalls-集成)
7. [结论](#7-结论)

---

## 1. 总览

Step3 的核心目标是将 CopyGC 的 6 套屏障（Idle/Enum/Trace/PostTrace/Preforward/Forward）+ 读屏障 + Tagged Pointer 体系，替换为 MS-GC 下仅 **2 套屏障**、**零读屏障**、**无 Tagged Pointer** 的全新架构。

当前代码已完整落地，主路径如下：

| GC 阶段 | 安装的 Barrier | 读行为 | 写行为 |
|---------|---------------|--------|--------|
| `IDLE` / `MS_SWEEP` | `MSIdleBarrier` | 直接 load | 直接 store |
| `MS_MARK_SETUP` / `MS_CONCURRENT_MARK` / `MS_MARK_TERMINATION` | `MSMarkBarrier` | 直接 load | `enqueue(old)` + store + `enqueue(new)` |

关键文件：

| 文件 | 职责 |
|------|------|
| `src/Heap/MBarrier/MSIdleBarrier.h` | 空闲屏障，全部 inline，零开销 |
| `src/Heap/MBarrier/MSMarkBarrier.h/.cpp` | 混合写屏障，old/new 双向 shade |
| `src/Heap/MBarrier/WriteBarrierBuffer.h` | Per-Mutator 固定容量灰对象暂存区（256 entries） |
| `src/Heap/Heap.cpp` | 持有两个 barrier 实例，`InstallBarrier()` 做 phase 切换 |
| `src/CompilerCalls.cpp` | 编译器调用入口，通过虚函数分派，无 `#ifdef MRT_USE_MARK_SWEEP` |
| `src/Mutator/Mutator.h/.cpp` | 持有 `wbBuf_`，`HandleGCPhase()` 处理 phase 转换 |

### 1.1 数据流全貌

```text
编译器生成代码 → MCC_WriteRefField()
                  → Heap::GetBarrier().WriteReference()    // 虚函数分派
                     ┌─ MSIdleBarrier: field.SetTargetObject(ref)
                     └─ MSMarkBarrier:
                          EnqueueRef(old)              // shade(old)
                          field.SetTargetObject(ref)    // store
                          EnqueueRef(new)              // shade(new)
                            → collector.MarkObject(ref) // bitmap CAS
                            → wbBuf_.Push(ref)          // 灰对象入本地 buffer
                               ├─ 满时 → FlushWriteBarrierOverflow → globalWorkQueue_
                               └─ STW  → FlushAllWriteBarrierBuffers → mainQueue → drain
```

### 1.2 与 CopyGC 屏障体系的对比

| 维度 | CopyGC (6 套) | MSGC (2 套) |
|------|--------------|-------------|
| 屏障套数 | 6（Idle/Enum/Trace/PostTrace/Preforward/Forward） | 2（MSIdleBarrier / MSMarkBarrier） |
| 读屏障 | 每次读都检查 `IsTagged()` + 可能 CAS forward | **零开销**，直接 load |
| 写屏障 | SATB 记旧值 + Tagged Pointer encode | shade(old) + shade(new)，无 tag |
| Tagged Pointer | 需要（高位 bit 编码 tag） | **不需要**，指针即地址 |
| 内存 fence | `seq_cst`（每次写屏障） | 无显式 fence（仅 bitmap CAS 自带 acquire/release） |
| Forward 阶段 | 极重的读屏障（对象拷贝 + CAS） | **不存在**（MS 不移动对象） |

---

## 2. MSIdleBarrier — 零开销空闲屏障

**文件**：`src/Heap/MBarrier/MSIdleBarrier.h`（header-only，全部 inline）

**生效阶段**：`GC_PHASE_IDLE`、`GC_PHASE_MS_SWEEP`

### 2.1 设计原理

CopyGC 的 `IdleBarrier` 即使在 GC 空闲期，读操作也需要检查 `IsTagged()` 并可能执行 `TryUpdateRefField`（处理上一轮遗留的 tagged pointer）。在 MS-GC 中对象不移动、不存在 from-space/to-space、不存在 tagged pointer，因此所有操作都可以降级为直接内存访问。

### 2.2 当前实现

所有方法都是 inline 的 direct load/store，零运行时开销：

| 操作类型 | 方法 | 实现 |
|---------|------|------|
| 读引用 | `ReadReference` | `field.GetTargetObject()` |
| 读静态引用 | `ReadStaticRef` | `field.GetTargetObject()` |
| 读弱引用 | `ReadWeakRef` | `field.GetTargetObject()` |
| 读 struct | `ReadStruct` / `ReadStaticStruct` | `memcpy_s(dst, size, src, size)` |
| 写引用 | `WriteReference` | `field.SetTargetObject(ref)` |
| 写静态引用 | `WriteStaticRef` | `field.SetTargetObject(ref)` |
| 写 struct | `WriteStruct` / `WriteStaticStruct` | `memcpy_s(dst, dstLen, src, srcLen)` |
| 数组拷贝 | `CopyRefArray` / `CopyStructArray` | `memmove_s(...)` |
| 原子读 | `AtomicReadReference` | `field.GetFieldValue(order)` → `GetTargetObject()` |
| 原子写 | `AtomicWriteReference` | `field.SetTargetObject(ref, order)` |
| 原子交换 | `AtomicSwapReference` | `field.Exchange(newRef, order)` |
| CAS | `CompareAndSwapReference` | `field.CompareExchange(...)` |

---

## 3. MSMarkBarrier — 混合写屏障

**文件**：`src/Heap/MBarrier/MSMarkBarrier.h`（声明）、`src/Heap/MBarrier/MSMarkBarrier.cpp`（实现）

**生效阶段**：`GC_PHASE_MS_MARK_SETUP`、`GC_PHASE_MS_CONCURRENT_MARK`、`GC_PHASE_MS_MARK_TERMINATION`

### 3.1 核心语义

MSMarkBarrier 实现的是 Go 1.8 风格的混合写屏障，对堆上每次引用写入同时 shade 旧值和新值：

```text
Write(slot, new):
    enqueue(old = *slot)    // discover 被覆盖的引用目标
    *slot = new             // store
    enqueue(new)            // discover 新链接的引用目标
```

**为什么不需要栈写屏障**：MarkSetup Handshake 阶段会扫描所有 Mutator 的栈根，栈上新写入的指针来源只有两种——来自堆（有写屏障保护）或来自栈上已有引用（MarkSetup 已标灰）。因此栈写操作不可能引入未被保护的白色对象。

### 3.2 EnqueueRef — 核心 shade 路径

所有 shade 操作都汇聚到静态方法 `EnqueueRef()`，这是整个写屏障的核心热路径：

```cpp
void MSMarkBarrier::EnqueueRef(BaseObject* ref)
{
    if (ref == nullptr || !Heap::IsHeapAddress(ref)) {
        return;                                    // 非堆对象（栈/全局）跳过
    }
    MarkSweepCollector& collector = ...GetCollector();
    if (collector.MarkObject(ref)) {
        return;                                    // 已标记 → 跳过
    }
    // 首次标记成功 → 推入 per-Mutator WriteBarrierBuffer
    Mutator* mutator = Mutator::GetMutator();
    if (mutator == nullptr) { return; }
    WriteBarrierBuffer& wbBuf = mutator->GetWriteBarrierBuffer();
    if (UNLIKELY(wbBuf.IsFull())) {
        MarkSweepCollector* collector = MarkSweepCollector::GetActiveCollector();
        if (collector != nullptr) {
            collector->FlushWriteBarrierOverflow(wbBuf);   // 满时刷入 globalWorkQueue_
        } else {
            wbBuf.Clear();                                 // GC 已结束则丢弃
        }
    }
    wbBuf.Push(ref);
}
```

**关键行为**：
1. `MarkObject(ref)` 是原子 bitmap CAS，因此**标记在写屏障路径上立即发生**（不是延迟到 MarkTermination）
2. 只有**首次标记成功**的对象才会进入 WBBuf（已标记对象被过滤掉）
3. WBBuf 满时通过 `FlushWriteBarrierOverflow` 加锁刷入 `globalWorkQueue_`
4. 如果 GC 周期已结束（`activeCollector_ == nullptr`），直接 `Clear()` 丢弃

### 3.3 EnqueueContainer — Bulk Struct 诊断路径

用于将容器对象（如 struct 宿主）直接推入 collector 的全局工作队列，确保 re-scan：

```cpp
void MSMarkBarrier::EnqueueContainer(BaseObject* obj)
{
    if (obj == nullptr || !Heap::IsHeapAddress(obj)) { return; }
    MarkSweepCollector* collector = MarkSweepCollector::GetActiveCollector();
    if (collector != nullptr) {
        collector->EnqueueGreyObject(obj);
    }
}
```

当前代码中 `EnqueueContainer` 在 `WriteStruct` 和 `CopyStructArray` 中**被注释掉**（`// EnqueueContainer(obj)`），因为 per-field discover(new) 已经足够精确，re-enqueue 容器只会增加冗余的队列流量。

### 3.4 各写操作实现

**WriteReference / WriteStaticRef**（引用字段写入）：

```text
1. 读 old = field.GetTargetObject()
2. EnqueueRef(old)                    // shade(old)
3. field.SetTargetObject(ref)          // store
4. EnqueueRef(ref)                    // shade(new)
```

**WriteStruct / WriteStaticStruct**（结构体写入）：

```text
1. ForEachRefInStruct(dst) → EnqueueRef(oldRef)    // shade all old refs
2. memcpy_s(dst, src)                               // 整块覆写
3. ForEachRefInStruct(dst) → EnqueueRef(newRef)    // shade all new refs
```

其中 `WriteStruct` 使用 `obj->ForEachRefInStruct()` 遍历对象内的引用字段，`WriteStaticStruct` 使用 `gctib.ForEachBitmapWord()` 遍历静态结构体的 GCTib bitmap。两者都有 `obj->GetTypeInfo() != nullptr` 的 null guard。

**CopyRefArray / CopyStructArray**（数组拷贝）：

```text
1. 遍历 dst 范围引用 → EnqueueRef(oldRef)     // shade all old refs
2. memmove_s(dst, src)                         // 整块拷贝
3. 遍历 dst 范围引用 → EnqueueRef(newRef)     // shade all new refs
```

### 3.5 原子操作实现

| 方法 | shade 策略 | 说明 |
|------|-----------|------|
| `AtomicReadReference` | 无 shade | 直接原子 load，零开销 |
| `AtomicWriteReference` | shade(old) + shade(new) | 先 relaxed 读旧值，shade 后原子 store |
| `AtomicSwapReference` | shade(old) + shade(new) | 原子 exchange，对换出的旧值和传入的新值都 shade |
| `CompareAndSwapReference` | CAS 成功时 shade(old) + shade(new) | CAS 失败时不 shade（字段未变化） |

**与设计文档的差异**：设计文档中 `CompareAndSwapReference` 为"无论 CAS 是否成功都 shade old 和 new"，当前实现仅在 **CAS 成功时** shade，更精确地避免了不必要的标记流量。

### 3.6 读操作实现

所有读操作与 `MSIdleBarrier` 完全一致——直接 load，零开销：

| 方法 | 实现 |
|------|------|
| `ReadReference` | `field.GetTargetObject()` |
| `ReadStaticRef` | `field.GetTargetObject()` |
| `ReadWeakRef` | `field.GetTargetObject()` |
| `ReadStruct` / `ReadStaticStruct` | `memcpy_s(...)` |
| `AtomicReadReference` | `field.GetFieldValue(order)` → `GetTargetObject()` |

这意味着即使在 GC 标记期间，读操作也没有任何额外开销——这是 MS-GC 相对 CopyGC 最大的性能优势之一。

### 3.7 诊断计数器

MSMarkBarrier.cpp 内置了一套可选的 barrier 调用计数器，由环境变量 `CJ_MS_HASHMAP_COUNTERS` 控制开关：

| 计数器 | 含义 |
|--------|------|
| `g_oldShadeCount` | shade(old) 被调用的次数 |
| `g_newEnqueueCount` | shade(new) EnqueueRef 被调用的总次数 |
| `g_newWriteRefCount` | `WriteReference` 中 shade(new) 次数 |
| `g_newWriteStaticRefCount` | `WriteStaticRef` 中 shade(new) 次数 |
| `g_newWriteStructCount` | `WriteStruct` 中 shade(new) 次数 |
| `g_containerEnqueueCount` | `EnqueueContainer` 调用次数 |
| ... | 其余 Atomic/CAS/Array 类似 |

计数器在 `MarkTermination()` 末尾通过 `MSMarkBarrier_DumpAndResetCounters()` 输出并重置，方便在 benchmark 中观察各类 barrier 的调用频率分布。

---

## 4. WriteBarrierBuffer — Per-Mutator 灰对象暂存区

**文件**：`src/Heap/MBarrier/WriteBarrierBuffer.h`（header-only）

### 4.1 数据结构

```cpp
class WriteBarrierBuffer {
    static constexpr size_t kCapacity = 256;   // 固定容量
    BaseObject* buf_[kCapacity];               // inline 数组
    size_t count_;                             // 当前条目数
};
```

- 容量固定 256 个指针（64 位平台上 2KB），内嵌在 `Mutator` 对象中
- 无动态内存分配，无锁（单写者——所属 Mutator 线程）
- 构造时 `count_ = 0`，全部 slot 置 `nullptr`

### 4.2 核心方法

| 方法 | 行为 | 调用者 |
|------|------|--------|
| `Push(obj)` | `buf_[count_++] = obj`，不检查溢出 | `MSMarkBarrier::EnqueueRef()`（调用前先检查 `IsFull()`） |
| `Flush(container)` | 遍历 `buf_[0..count_-1]`，非 null 条目 `push_back` 到 container，然后 `count_ = 0` | STW 时 `FlushAllWriteBarrierBuffers` / overflow 时 `FlushWriteBarrierOverflow` |
| `Clear()` | `count_ = 0`（不清理数组内容） | GC 结束后 / `activeCollector_ == nullptr` 时 |
| `IsFull()` | `count_ >= kCapacity` | `EnqueueRef()` 在 Push 前检查 |

### 4.3 生命周期

| 时机 | 行为 |
|------|------|
| Mutator 创建 | `wbBuf_` 构造，`count_ = 0` |
| 并发标记期间 | `EnqueueRef()` 调用 `Push()`；满时 `FlushWriteBarrierOverflow()` |
| `MarkTermination` STW | `FlushAllWriteBarrierBuffers(mainQueue)` 统一 flush |
| GC 周期结束后 | barrier 切换到 `MSIdleBarrier`，不再有 Push；残留已被 STW flush 清空 |
| Mutator 退出 | 析构时隐式清理 |

### 4.4 Overflow 路径

当 `wbBuf_.IsFull()` 时，`EnqueueRef()` 调用：

```cpp
void MarkSweepCollector::FlushWriteBarrierOverflow(WriteBarrierBuffer& wbBuf)
{
    std::lock_guard<std::mutex> lock(globalWorkMutex_);
    wbBuf.Flush(globalWorkQueue_);
    if (!globalWorkQueue_.empty()) {
        globalWorkAvailable_.store(true, std::memory_order_release);
    }
}
```

- 加锁 `globalWorkMutex_` 刷入 `globalWorkQueue_`
- 设置 `globalWorkAvailable_` flag，通知 GC 线程 / assist 有新工作
- 这是并发阶段唯一需要竞争全局锁的路径（且仅在 buffer 满时触发）

### 4.5 STW Flush 路径

`MarkTermination()` 在 STW 下调用：

```cpp
void MarkSweepCollector::FlushAllWriteBarrierBuffers(WorkStack& mainQueue)
{
    auto func = [&mainQueue](Mutator& mutator) {
        mutator.GetWriteBarrierBuffer().Flush(mainQueue);
    };
    MutatorManager::Instance().VisitAllMutators(func);
}
```

- 遍历所有 Mutator，将每个 WBBuf 残留条目 flush 到 `mainQueue`
- STW 下执行，不需要额外同步
- flush 后由 `ExecuteLikeDrainMainQueue()` 统一 drain

---

## 5. 屏障安装与 Phase 联动

**文件**：`src/Heap/Heap.cpp`

### 5.1 HeapImpl 持有的 Barrier 实例

```cpp
#ifdef MRT_USE_MARK_SWEEP
    MSIdleBarrier msIdleBarrier;    // GC 空闲期 / sweep
    MSMarkBarrier msMarkBarrier;    // GC mark 期
#else
    // CopyGC: 6 个独立 Barrier 实例
    IdleBarrier idleBarrier;
    EnumBarrier enumBarrier;
    TraceBarrier traceBarrier;
    PostTraceBarrier postTraceBarrier;
    PreforwardBarrier preforwardBarrier;
    ForwardBarrier forwardBarrier;
#endif
    Barrier* currentBarrier;        // 当前活跃的 barrier 指针
```

初始化时 `currentBarrier = &msIdleBarrier`。

### 5.2 InstallBarrier 映射

```cpp
void HeapImpl::InstallBarrier(const GCPhase phase)
{
#ifdef MRT_USE_MARK_SWEEP
    switch (phase) {
        case GCPhase::GC_PHASE_IDLE:
        case GCPhase::GC_PHASE_MS_SWEEP:
            currentBarrier = &msIdleBarrier;      // 零开销
            break;
        case GCPhase::GC_PHASE_MS_MARK_SETUP:
        case GCPhase::GC_PHASE_MS_CONCURRENT_MARK:
        case GCPhase::GC_PHASE_MS_MARK_TERMINATION:
            currentBarrier = &msMarkBarrier;      // 混合写屏障
            break;
        default:
            currentBarrier = &stwBarrier;         // fallback
            break;
    }
#else
    // CopyGC 的多阶段 barrier 切换 ...
#endif
}
```

### 5.3 Phase 状态 → Barrier 映射图

```text
IDLE ────────→ MSIdleBarrier (零开销)
  │
  │ TransitionToGCPhase(MS_MARK_SETUP)
  ▼
MARK_SETUP ──→ MSMarkBarrier (shade old+new)
  │
  │ TransitionToGCPhase(MS_CONCURRENT_MARK)
  ▼
CONCURRENT_MARK → MSMarkBarrier
  │
  │ ScopedStopTheWorld(MS_MARK_TERMINATION)
  ▼
MARK_TERMINATION → MSMarkBarrier
  │
  │ TransitionToGCPhase(MS_SWEEP)  [在 STW 内切换]
  ▼
SWEEP ───────→ MSIdleBarrier (零开销)
  │
  │ → IDLE
  ▼
(回到起点)
```

**关键设计**：`MarkTermination()` 在 STW 内部执行 `TransitionToGCPhase(MS_SWEEP)`，确保 mutator 恢复运行时已经看到 `MSIdleBarrier`，不会在 drain 完成后仍然有 barrier 往 WBBuf 写入。

### 5.4 屏障切换的触发机制

通过二级指针实现 O(1) 查找：

```cpp
static Barrier& GetBarrier() { return **currentBarrierPtr; }
// currentBarrierPtr → HeapImpl::currentBarrier → 具体 Barrier 实例
```

`InstallBarrier()` 修改 `currentBarrier` 指针即可生效，无需通知各 Mutator 更新本地缓存。

---

## 6. CompilerCalls 集成

**文件**：`src/CompilerCalls.cpp`

### 6.1 当前集成方式：纯虚函数分派

CompilerCalls 层面**没有** `#ifdef MRT_USE_MARK_SWEEP` 条件编译。所有 `MCC_*` / `CJ_MCC_*` 入口函数通过 `Heap::GetBarrier()` 返回的虚函数引用自动分派到当前安装的 barrier 实例。

| 函数 | 分发调用 |
|------|---------|
| `MCC_WriteRefField` | `Heap::GetBarrier().WriteReference(obj, *field, ref)` |
| `MCC_WriteStructField` | `Heap::GetBarrier().WriteStruct(obj, dst, ...)` |
| `MCC_WriteStaticRef` | `Heap::GetBarrier().WriteStaticRef(*field, ref)` |
| `MCC_WriteStaticStruct` | `Heap::GetBarrier().WriteStaticStruct(...)` |
| `MCC_AtomicWriteReference` | `Heap::GetBarrier().AtomicWriteReference(obj, *field, ref, order)` |
| `MCC_AtomicSwapReference` | `Heap::GetBarrier().AtomicSwapReference(obj, *field, newRef, order)` |
| `MCC_AtomicCompareSwapReference` | `Heap::GetBarrier().CompareAndSwapReference(...)` |
| `CJ_MCC_ReadRefField` | `Heap::GetBarrier().ReadReference(obj, *field)` |
| `CJ_MCC_ReadWeakRef` | `Heap::GetBarrier().ReadWeakRef(obj, *field)` |
| `CJ_MCC_ReadStructField` | `Heap::GetBarrier().ReadStruct(...)` |
| `CJ_MCC_ReadStaticRef` | `Heap::GetBarrier().ReadStaticRef(*field)` |
| `MCC_AtomicReadReference` | `Heap::GetBarrier().AtomicReadReference(...)` |

### 6.2 为什么不需要条件编译

Step3.3 设计文档中提出了两个可选优化方案：
- **方案 A**：`#ifdef MRT_USE_MARK_SWEEP` 条件编译，读操作直接返回
- **方案 B**：`if (LIKELY(!Heap::UseBarrier()))` 快速路径判断

当前实现**两个都没有采用**。原因：
1. 虚函数分派已经足够高效（一次间接调用）
2. LTO 环境下编译器可能自动 devirtualize
3. 不改 CompilerCalls = 零维护成本，CopyGC 和 MSGC 共用同一份代码

---

### 7 结论

#### 7.1 正确性论证 — 所有存活对象都一定被标记

混合写屏障的正确性依赖以下 6 个环节共同保障。任何一个环节缺失都可能导致漏标（live object 被误回收）。当前实现对每个环节都有完整覆盖：

**① 栈根完备**

`MarkSetup` 通过 `TransitionToGCPhase(MS_MARK_SETUP, handshake=true)` 发起全局 handshake，每个 Mutator 到达 safepoint 后执行 `GcPhaseEnum()` → `VisitMutatorRoots()`，扫描自己栈帧上的所有引用并推入根集合。GC 线程**等待所有 Mutator 完成**才继续，保证在并发标记开始前，所有栈根已被标灰。

之后写屏障保护所有堆写操作，而栈写不需要屏障——栈上新写入的指针只可能来自堆（有屏障保护）或栈上已有引用（已在 MarkSetup 中标灰），不可能引入未保护的白色对象。

**② 全局/静态根完备**

handshake 完成后，GC 线程在 `EnumAndMarkRoots()` 中串行枚举所有非栈根：

| 根类别 | 枚举方法 |
|--------|---------|
| Mutator 根 | `MergeMutatorRoots()` — 收集 AllocBuffer 中的根 |
| 静态/全局引用 | `EnumStaticRoots()` |
| 并发模型根 | `EnumConcurrencyModelRoots()` |
| Finalizer 队列根 | `EnumFinalizerProcessorRoots()` |
| 复活 export 根 | `EnumAllSurrectedExportRoots()` + `EnumAllExportRoots()` |

覆盖了所有 root 类别。

**③ 新分配对象标黑（allocBlack）**

标记期间新分配的对象通过 `MarkNewObject()` 直接按 **alloc-black** 语义处理：运行时只打 `mark`，不入 grey queue。是否已经完成 trace 由 queue protocol 隐式表达，而不是再额外写 `scanned` 位：

```cpp
void MarkSweepCollector::MarkNewObject(BaseObject* obj)
{
    GCPhase phase = Mutator::GetMutator()->GetMutatorPhase();
    if (phase == MS_MARK_SETUP || phase == MS_CONCURRENT_MARK || phase == MS_MARK_TERMINATION) {
        MarkObject(obj);       // 设 markBitmap bit（alloc-black）
    }
}
```

新对象的引用字段初始为 null；后续赋值走写屏障的 `enqueue(new)` 路径，因此 allocBlack 对象指向的所有目标都会被发现。

**④ 写屏障完备 — 所有堆写路径无遗漏**

全部 10 种编译器生成的堆写操作都经过 `Heap::GetBarrier()` 虚分派到 `MSMarkBarrier`：

| 路径 | shade 行为 |
|------|-----------|
| `WriteReference` / `WriteStaticRef` | enqueue(old) + store + enqueue(new) |
| `WriteStruct` / `WriteStaticStruct` | ForEachRef shade(old) + memcpy + ForEachRef shade(new) |
| `CopyRefArray` / `CopyStructArray` | 遍历 dst shade(old) + memmove + 遍历 dst shade(new) |
| `AtomicWriteReference` | enqueue(old) + atomic store + enqueue(new) |
| `AtomicSwapReference` | exchange + enqueue(old) + enqueue(new) |
| `CompareAndSwapReference` | CAS 成功时 enqueue(old) + enqueue(new) |

混合屏障同时覆盖了 deletion barrier（shade old，防止 SATB 语义丢失）和 insertion barrier（shade new，防止新引用链指向白色对象），保证并发标记期间不会因引用图变异而漏标。

**⑤ MarkTermination 收敛 — STW 闭合 drain**

`MarkTermination()` 在 STW 下执行以下严格有序的步骤：

```text
STW → activeCollector_ = nullptr（禁止新 assist 入队）
    → FlushAllWriteBarrierBuffers(mainQueue)      // 收集所有 Mutator 的 WBBuf 残留
    → FlushAllAssistLocalWorks(mainQueue)          // 收集所有 assist 线程的本地工作
    → DrainGlobalWorkQueueToMainQueue(mainQueue)   // 排空 globalWorkQueue
    → CollectResurrectionCandidates(mainQueue)     // finalizer 复活候选
    → ExecuteLikeDrainMainQueue(mainQueue)         // 闭合 drain：mark+scan 直到空
    → TransitionToGCPhase(MS_SWEEP)                // 在 STW 内切到 SWEEP
```

收敛性证明：
1. STW 后没有 Mutator 运行 → 不会有新的 WBBuf 写入
2. `activeCollector_ = nullptr` → 不会有新 assist 工作入队
3. 三次 flush 把**所有分散的队列**汇聚到 `mainQueue`
4. `ExecuteLikeDrainMainQueue` 循环 drain 直到 `mainQueue` **严格为空**
5. 相变在 STW 内完成 → Mutator resume 后使用 `MSIdleBarrier`，不再产生灰对象

因此 MarkTermination 结束时，所有可达对象都已阅标记为 black，无灰色对象残留。

**⑥ WeakRef 处理**

弱引用不参与可达性传播。标记阶段遇到 WeakRef 对象时：
- 只标记 WeakRef 对象本身为 black，**不标记 referent**
- WeakRef 对象加入 `WeakRefBuffer`
- MarkTermination 后的 `PostMarkTermination()` 中调用 `WeakRefBuffer::ClearWeakRefBuffer()`：未标记的 referent → 置 null

此时已切到 `MS_SWEEP` 阶段，Mutator 使用 idle barrier，不会访问已被判定为不可达的 referent。

**正确性总结**

| 检查项 | 保障机制 | 状态 |
|--------|---------|------|
| 栈根完备 | Handshake + 每个 Mutator 自扫栈根 | ✅ |
| 全局根完备 | GC 线程串行枚举 5 类非栈根 | ✅ |
| allocBlack | `MarkNewObject` 设 mark+scanned | ✅ |
| 写屏障完备 | 10 种写路径全部 shade(old)+shade(new) | ✅ |
| MarkTermination 收敛 | STW + 3 次 flush + 闭合 drain + STW 内相变 | ✅ |
| WeakRef | 不传播 referent + PostMark 清零 | ✅ |

这 6 个环节形成闭合的标记正确性链：**根集合完备** → **并发期写屏障不漏** → **新分配标黑** → **终止期闭合 drain** → 所有存活对象一定被标记。

#### 7.2 落地总评

Step3 的核心设计目标——**2 套屏障 + 零读屏障 + 无 Tagged Pointer + 混合 old/new shade**——已经**完整落地**并通过 benchmark 验证。未落地的部分主要是性能微优化（per-P 队列、编译器 intrinsic 降级）和代码瘦身（RefField 条件编译），不影响正确性和基本性能。
