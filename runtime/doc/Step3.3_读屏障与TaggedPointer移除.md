# Step 3.3：读屏障、Tagged Pointer 移除与 CompilerCalls 改造

> 本文档是 [Step3 混合写屏障详细设计](Step3_混合写屏障详细设计.md) 的子文档。
> 包含读屏障完全移除方案、Tagged Pointer 移除方案，以及 CompilerCalls 层面的改造。

---

## 目录

1. [读屏障完全移除方案](#1-读屏障完全移除方案)
   - 1.1 [当前读屏障存在的所有位置](#11-当前读屏障存在的所有位置)
   - 1.2 [为什么 Mark-Sweep 不需要读屏障](#12-为什么-mark-sweep-不需要读屏障)
   - 1.3 [Mark-Sweep 模式下的读操作实现](#13-mark-sweep-模式下的读操作实现)
   - 1.4 [WeakRef 的特殊处理](#14-weakref-的特殊处理)
   - 1.5 [编译器优化机会](#15-编译器优化机会)
2. [Tagged Pointer 移除方案](#2-tagged-pointer-移除方案)
   - 2.1 [当前 Tagged Pointer 机制](#21-当前-tagged-pointer-机制)
   - 2.2 [为什么 Mark-Sweep 不需要 Tagged Pointer](#22-为什么-mark-sweep-不需要-tagged-pointer)
   - 2.3 [代码层面的移除策略](#23-代码层面的移除策略)
   - 2.4 [需要清理的使用点](#24-需要清理的使用点)
   - 2.5 [便捷宏定义](#25-便捷宏定义)
   - 2.6 [风险评估](#26-风险评估)
3. [CompilerCalls 改造](#3-compilercalls-改造)
   - 3.1 [当前架构](#31-当前架构)
   - 3.2 [改造函数清单](#32-改造函数清单)
   - 3.3 [写操作改造](#33-写操作改造)
   - 3.4 [读操作改造（可选优化）](#34-读操作改造可选优化)
   - 3.5 [编译器 Intrinsic 配合](#35-编译器-intrinsic-配合)

---

## 1. 读屏障完全移除方案

### 1.1 当前读屏障存在的所有位置

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

### 1.2 为什么 Mark-Sweep 不需要读屏障

Copy GC 需要读屏障的根本原因是**对象会移动**：
- Forward 阶段，from-space 中的对象被拷贝到 to-space
- 读屏障负责在读取 from-space 指针时"自愈"（查转发表，返回 to-space 地址）

Mark-Sweep 中**对象不移动**：
- 对象分配后地址永远不变
- 引用字段中的指针始终是有效地址
- 不存在"旧地址"/"新地址"的概念
- 因此读操作直接 load 即可

### 1.3 Mark-Sweep 模式下的读操作实现

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

### 1.4 WeakRef 的特殊处理

弱引用在 Mark-Sweep 中需要在 Sweep 阶段清理（如果 referent 未被标记），但**读操作本身不需要屏障**：

```cpp
BaseObject* MSWriteBarrier::ReadWeakRef(BaseObject* obj, RefField<false>& field) const {
    BaseObject* referent = field.GetTargetObject();
    // 注意：不需要 RememberObjectInSatbBuffer（混合写屏障已覆盖方向）
    // 弱引用的 referent 如果在标记结束时未被标记，会在 clearWeakRef 阶段被清 null
    return referent;
}
```

### 1.5 编译器优化机会

移除读屏障后，编译器可以做更多优化：

1. **内联**：`CJ_MCC_ReadRefField` → `field.GetTargetObject()` → 单条 load 指令
2. **别名分析**：读操作不再有副作用（无 CAS/写入），编译器可以做更好的别名分析
3. **循环优化**：循环中的重复读引用可以被 LICM（Loop Invariant Code Motion）提出
4. **LLVM intrinsic 降级**：`llvm.cj.gcread.*` 系列 intrinsic 可降级为普通 load
5. **减少 safepoint**：读操作不再是潜在的 safepoint（不修改任何状态）

---

## 2. Tagged Pointer 移除方案

### 2.1 当前 Tagged Pointer 机制

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

### 2.2 为什么 Mark-Sweep 不需要 Tagged Pointer

Mark-Sweep 中对象不移动，不存在 from-space/to-space 的概念：
- 不需要区分"已转发"/"未转发"引用
- 不需要自愈式读屏障来修正旧地址
- 不需要 tag ID 翻转来区分 GC 周期

因此，**整个 Tagged Pointer 机制可以完全移除**。指针就是地址，不含任何元数据。

### 2.3 代码层面的移除策略

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

### 2.4 需要清理的使用点

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

### 2.5 便捷宏定义

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

### 2.6 风险评估

| 风险 | 说明 | 缓解 |
|------|------|------|
| 遗漏 tag 使用点 | 代码中可能有隐式的 tag 假设 | 搜索 `IsTagged|GetTagID|tagID|isTagged` 全部标注 |
| RefField 二进制兼容 | 修改 RefField 布局可能影响序列化 | 运行时不序列化 RefField，无影响 |
| 编译器生成的 tag 代码 | cjc 编译器可能生成依赖 tag 的代码 | 编译器需配合修改 intrinsic 降级 |

---

## 3. CompilerCalls 改造

> **核心约束：所有 CompilerCalls `extern "C"` 函数签名完全不变，无需修改对外接口。**
> 
> MSWriteBarrier 完全通过 `Barrier` 基类的虚函数分派机制工作，CompilerCalls 层面的调用链路（`MCC_WriteRefField → Heap::GetBarrier().WriteReference()`）保持不变。下文涉及的 `#ifdef MRT_USE_MARK_SWEEP` 条件编译均为**可选的性能优化**（绕过虚函数调用开销），不是正确性必需的。即使完全不修改 CompilerCalls.cpp，设计也能正确运行。

### 3.1 当前架构

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

### 3.2 改造函数清单

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

### 3.3 写操作改造

写操作走 barrier 虚函数的架构不变，因为写屏障在 GC Mark 阶段确实需要额外逻辑。

> **写操作无需任何改造即可工作**：`MCC_WriteRefField` 函数签名和函数体均不变（仍然 `GetBarrier().WriteReference(...)`），差异完全由虚函数分派到 `MSWriteBarrier::WriteReference()` 内部消化。以下 `#ifdef` 示例仅为**可选的微优化**（在明确不需要屏障的场景提前短路），不是正确性前提。

**可选优化：MCC_WriteRefField 中的非堆对象短路**

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

### 3.4 读操作改造（可选优化）

> **重要**：读操作改造是**可选的性能优化**，不是正确性必需。不做任何改造时，读操作路径为：`CJ_MCC_ReadRefField → GetBarrier().ReadReference() → MSIdleBarrier::ReadReference() → field.GetTargetObject()`，通过虚函数分派自动生效，功能完全正确。以下 `#ifdef` 方案仅为绕过虚函数调用开销，在 LTO 环境下收益有限。

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

**其他读操作改造**（均为可选优化，模式相同）：

```cpp
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

### 3.5 编译器 Intrinsic 配合

当 `MRT_USE_MARK_SWEEP` 启用后，编译器（cjc）侧也应配合优化：

| Intrinsic | 当前 | MS 模式建议 |
|-----------|------|------------|
| `llvm.cj.gcread.ref` | 生成 `call CJ_MCC_ReadRefField` | 降级为普通 `load` 指令 |
| `llvm.cj.gcread.static` | 生成 `call CJ_MCC_ReadStaticRef` | 降级为普通 `load` 指令 |
| `llvm.cj.gcwrite.ref` | 生成 `call MCC_WriteRefField` | **保持**（写屏障仍需） |
| `llvm.cj.gcwrite.static` | 生成 `call MCC_WriteStaticRef` | **保持** |

**注意**：编译器优化是后续工作。初期阶段即使仍生成函数调用，运行时层面的实现也确保了正确性和较好的性能（因为 MS 模式下读操作函数体只有 1 条 load 指令，LTO 可以内联）。
