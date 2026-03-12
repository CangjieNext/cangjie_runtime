# Step 3.1：MSBarrier 详细实现

> 本文档是 [Step3 混合写屏障详细设计](Step3_混合写屏障详细设计.md) 的子文档。
> 包含 MSIdleBarrier 和 MSWriteBarrier 的完整代码级设计。

---

## 目录

1. [MSIdleBarrier 设计](#1-msidlebarrier-设计)
2. [与 Barrier 基类的兼容](#2-与-barrier-基类的兼容)
3. [MSWriteBarrier 核心设计](#3-mswritebarrier-核心设计)
4. [MarkAndEnqueue 辅助方法](#4-markandenqueue-辅助方法)
5. [WriteReference 实现](#5-writereference-实现)
6. [WriteStaticRef 实现](#6-writestaticref-实现)
7. [WriteStruct 实现](#7-writestruct-实现)
8. [CopyRefArray 实现](#8-copyrefarray-实现)
9. [原子操作实现](#9-原子操作实现)
10. [ReadReference 和其他读操作](#10-readreference-和其他读操作)

---

## 1. MSIdleBarrier 设计

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

---

## 2. 与 Barrier 基类的兼容

MSIdleBarrier 和 MSWriteBarrier 都继承自同一个 `Barrier` 基类，虚函数接口完全兼容。这意味着：
- `Heap::GetBarrier()` 返回的仍然是 `Barrier&`
- 所有 CompilerCalls 中的调用路径不需要修改签名
- 通过 `InstallBarrier` 切换指针即可生效

## 3. MSWriteBarrier 核心设计

MSWriteBarrier 在 GC Mark 阶段（`MS_PHASE_MARK_SETUP` → `MS_PHASE_MARK` → `MS_PHASE_MARK_TERMINATION`）生效。它的核心职责：
- **写操作**：标灰旧值 + 标灰新值 → 写入
- **读操作**：直接 load，零开销（与 MSIdleBarrier 相同）

---

## 4. MarkAndEnqueue 辅助方法

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

---

## 5. WriteReference 实现

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

---

## 6. WriteStaticRef 实现

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

## 7. WriteStruct 实现

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

---

## 8. CopyRefArray 实现

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

---

## 9. 原子操作实现

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

---

## 10. ReadReference 和其他读操作

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
