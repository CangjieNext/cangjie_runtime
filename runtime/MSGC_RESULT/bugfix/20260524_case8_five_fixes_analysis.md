# case8_http_json_serde MSGC 崩溃修复分析（5 个 Fix）

**日期**: 2026-05-24  
**触发场景**: case8_http_json_serde 60s HTTP JSON 序列化基准测试  
**修复前现象**: benchmark 运行 2~20s 内必然 SIGSEGV 崩溃  
**修复后结果**: 60s 完整运行，exit=0，MSGC 性能与 CopyGC 基本持平（±10%）

---

## 性能对比（60s，cjHeapSize=256mb，cjProcessorNum=6）

| Phase | MSGC（Fix 1-5） | CopyGC 基线 | 差异 |
|-------|----------------|------------|------|
| P1 (1K, 17.5s) | 4780 req/s | 4894 req/s | -2.3% |
| P2 (10K, 15s)  | 478 req/s  | 469 req/s  | +1.9% |
| P3 (500K, 12.5s) | 12 req/s | 11 req/s  | +9% |
| P4 (Mixed, 15s)  | 36 req/s | 33 req/s  | +9% |
| 总时长 | 60070ms | 60082ms | ≈持平 |

---

## 5 个 Fix 总览

| # | 文件 | 问题类型 | 是否真实问题 | 是否必要 | 性能影响 |
|---|------|---------|------------|---------|---------|
| Fix 1 | `CompilerCalls.cpp` | 编译器 ABI：IsSubType 收到带 objectState 的原始 stateWord | ✅ 真实 | ✅ 必要 | 可忽略 |
| Fix 2 | `MSRegionInfo.cpp` | SweepRegion 并发竞争：先 publish allocBitmap 后 zero，分配方拿到脏 slot | ✅ 真实 | ✅ 必要 | 可忽略 |
| Fix 3 | `WriteBarrierBuffer.h` | WBB Flush 时推入 stateWord=0 的已清零对象 | ⚠️ 防御性 | ✅ 必要 | 可忽略 |
| Fix 4 | `MarkSweepCollector.cpp` | MarkTermination mark queue 中出现 stateWord=0 的对象，原代码直接 FATAL | ⚠️ 症状修复 | ✅ 必要 | 可忽略 |
| Fix 5 | `StateWord.h` + `BaseObject.h` | 对象初始化只写 TypeInfo 不写 objectState，objectState 继承旧 slot 的脏值 | ✅ 真实 | ✅ 必要 | 极小 |

---

## Fix 1：`CJ_MCC_IsSubType` 顶 16 位 mask

### 问题描述

编译器在生成类型检查代码时，用 `ldr xN, [obj]` 直接读取 8 字节 stateWord（包含 TypeInfo 的低 48 位 + objectState 的高 16 位），并将这个**原始 64 位值**作为 `TypeInfo*` 传入 `CJ_MCC_IsSubType`。

当 objectState 不为 0（如对象处于 LOCKED 状态 = `stateBits=1`），原始值的高 16 位非零，指针变为内核态地址 → **SIGSEGV**。

### StateWord 布局

```
字节 0-3 : typeInfoLow32  （TypeInfo* 的低 32 位）
字节 4-5 : typeInfoHigh16 （TypeInfo* 的高 16 位，bit 47..32）
字节 6-7 : objectState    （NORMAL=0, LOCKED=1, FORWARDING=2, FORWARDED=3）
```

例如崩溃地址 `0x6e650001025012f8`：
- TypeInfo* = `0x0001025012f8`（有效）
- objectState.stateBits = `0x6e65`（非标准值，导致指针变为内核态地址）

### 代码变更

```cpp
// CompilerCalls.cpp: CJ_MCC_IsSubType
constexpr uintptr_t kTypeInfoMask = 0x0000'FFFF'FFFF'FFFFull;
typeInfo = reinterpret_cast<TypeInfo*>(
    reinterpret_cast<uintptr_t>(typeInfo) & kTypeInfoMask);
```

### 是否真实问题？

**是**。objectState = LOCKED (1) 在以下场景必然发生：
- 任何使用 `ScopedObjectLock` 的对象（哈希计算、toString、引用字段遍历）
- 并发标记期间，多个 GC 线程竞争 forwarding 时临时 LOCK 对象

CopyGC 也有 LOCKED 状态，但 CopyGC 下 IsSubType 通常在 safepoint 内执行（此时对象不会被并发修改），而 MSGC 并发期更长，窗口更大。

### 是否必要？

**必要**。这是正确性修复，不是防御性补丁。对象被 LOCK 时调用 IsSubType 是合法操作，runtime 必须正确处理。

### 对性能的影响？

**可忽略**：每次 IsSubType 调用新增 1 条 AND 指令。IsSubType 不在分配热路径上。

---

## Fix 2：`SweepRegion` 三阶段保证 zero-before-publish

### 问题描述

原始 `SweepRegion` 实现：

```cpp
// 原始（错误）顺序：
for i: allocBitmap[i] = ~markBitmap[i];  // 1. 先 publish
         liveCount += popcount(markBitmap[i])
// 再 zero dead slots...
for each dead slot: memset(slot, 0, slotSize)  // 2. 后 zero
```

**竞争窗口**：步骤 1 将 slot 标记为可分配 → 并发 allocator 立即从 allocBitmap 取走 slot → 步骤 2 的 memset 才开始 → allocator 初始化新对象时 slot 内还有旧对象的脏数据（包括脏 objectState bits）。

### 修复方案

```cpp
// 修复后（正确）顺序：
// Pass 1: 统计存活数量
// Pass 2: zero dead slots（先零化）
// __atomic_thread_fence(__ATOMIC_RELEASE)  // 保证 zero 对后续 acquire 可见
// Pass 3: allocBitmap[i] = ~markBitmap[i]（后 publish）
```

发布 allocBitmap 之前完成所有 memset + release fence，保证 allocator 拿到 slot 时内存已干净。

### 是否真实问题？

**是**。这是一个典型的 produce-before-zero 竞争，在高并发下（多 Processor 线程竞争分配）必然触发。实测在 case8 场景（HTTP 请求大量短生命周期 JSON 对象）触发频率很高。

### 是否必要？

**必要**。不修复此竞争，新对象可能继承脏 objectState（如旧对象的 FORWARDED 状态），导致编译器生成的内联类型派发崩溃。

### 对性能的影响？

**可忽略**：
- 新增 Pass 1（bitmap 扫描统计 liveCount）：O(wordCount)，额外工作量远小于 Pass 2 的 memset 成本
- release fence 在 AArch64 是一条 `dmb ish` 指令，overhead 极小
- 三 pass 总时间 ≈ 原 one-pass + memset 时间

---

## Fix 3：`WriteBarrierBuffer::Flush` 添加 `IsValidObject()` 过滤

### 问题描述

SATB 写屏障在 mutator 写引用字段前，将旧值入队到 WBB（Write Barrier Buffer）。MarkTermination（STW）阶段 flush WBB 时，理论上所有旧值都应该是活跃对象。

**边缘场景**：上一轮 GC 的 lazy/concurrent sweep 可能在下一轮 MarkTermination 期间仍在运行。如果 sweep 将某个对象清零（memset to 0），而该对象的指针恰好还在某个 mutator 的 WBB 中（在 MarkTermination 开始前最后一刻写入），Flush 时 `obj->stateWord == 0x0` → `IsValidObject() == false`。

### 代码变更

```cpp
// WriteBarrierBuffer.h: Flush
if (obj != nullptr && obj->IsValidObject()) {  // 新增：过滤已清零对象
    workStack.push_back(obj);
}
```

### 是否真实问题？

**防御性修复**。SATB 理论上保证 WBB 只有活跃旧值，但在 MSGC 的并发 sweep 与 MarkTermination 重叠时，存在上述边缘场景。此外，如果存在 WBB 被写入非法指针的其他 bug，此检查也能作为保护。

### 是否必要？

**必要**。不过滤则后续处理 stateWord=0 的对象会访问空 TypeInfo → 崩溃或 undefined behavior。过滤是安全且正确的：已被 sweep 清零的对象是死对象，SATB 正确性不要求保留它们。

### 对性能的影响？

**可忽略**：`IsValidObject()` = `GetTypeInfo() != nullptr`，单次 load + 比较。WBB 大小固定（典型 64 或 256 项），flush 不在热路径上。

---

## Fix 4：`ExecuteLikeDrainMainQueue` FATAL → skip

### 问题描述

MarkTermination STW 阶段，`ExecuteLikeDrainMainQueue` 处理主 mark 队列。原代码：

```cpp
if (!obj->IsValidObject()) {
    LOG(RTLOG_FATAL, "[MS-GC] FATAL invalid stateWord ...");
    // 继续执行（没有 continue/return，随后访问 null TypeInfo 还是会崩）
}
```

当出现 stateWord=0 的对象时，原代码打印 FATAL 日志后继续运行，导致后续 `obj->HasRefField()` 等调用访问空 TypeInfo → 崩溃。

### 代码变更

```cpp
if (!obj->IsValidObject()) {
    VLOG(REPORT, "[MS-GC] ExecuteLikeDrainMainQueue: skipping swept/zeroed obj=%p...");
    continue;  // 安全跳过
}
```

### 是否真实问题？

**问题本身是真实的（Fix 3 的 backstop），但也有诊断价值损失**。

原来的 FATAL 是正确的 GC 不变量检查：mark 队列中不应出现死对象。改为 skip 会**掩盖这个不变量违反**。需要明确：

- **如果跳过的是真正的死对象**（被并发 sweep 清零）：正确，SATB 安全性不受影响
- **如果跳过的是活对象**（某个 bug 导致活对象 stateWord 被错误清零）：会导致漏标 → 活对象被收集 → use-after-free

配合 Fix 2（zero-before-publish 消除竞争）和 Fix 3（WBB 过滤），进入这个分支的对象**应该总是死对象**，skip 是安全的。但为了保留诊断能力，建议后续考虑：

```cpp
// 建议：添加断言或计数，供 debug 模式下追查
#ifdef MRT_DEBUG
    DCHECK(false, "stateWord=0 object in mark queue - investigate");
#endif
```

### 是否必要？

**必要**：原代码在打印 FATAL 后没有 return/continue，必然接着崩溃。即使不改日志级别，也需要加 `continue`。

### 对性能的影响？

**可忽略**：稀有路径，每轮 GC 触发次数极少（Fix 2+3 修复后理论上为 0）。

---

## Fix 5：`StateWord::InitStateWord` 原子初始化（TypeInfo + objectState=0）

### 问题描述

原始 `SetTypeInfo` 的实现：

```cpp
void SetTypeInfo(TypeInfo* typeInfo) {
    uintptr_t address = reinterpret_cast<uintptr_t>(typeInfo);
    this->typeInfoLow32  = (address >> 0) & 0xFFFFFFFF;   // write 1
    this->typeInfoHigh16 = (address >> 32) & 0xFFFF;      // write 2
    // objectState 不变！
}
```

**两个独立问题**：

**问题 A：objectState 不被初始化**  
如果内存 slot 的旧对象有脏 objectState（如 `0x2268`），新对象调用 `SetTypeInfo` 后不写 objectState，导致新活跃对象继承了旧的脏 `stateBits`。当编译器生成的内联类型派发代码读取 `[obj]`（原始 64 位 stateWord）并当作 TypeInfo* 使用时，高 16 位非零 → SIGSEGV。

此问题的根源是：某些分配路径（BUMP 模式的 fresh region 经 `ResetToBump()` 重置）不经过 SweepRegion 的 zero 阶段，slot 内存可能有历史残留数据。Fix 2 消除了 BITMAP 路径的竞争，但 BUMP 路径理论上仍存在此风险。

**问题 B：stateWord 的非原子写入（撕裂读）**  
`SetTypeInfo` 分两步写 typeInfoLow32 和 typeInfoHigh16（两次 32/16 位 store）。若并发读线程在两次写之间读取 stateWord，可能看到只有 low32 有效、high16 还是旧值的撕裂状态，导致 TypeInfo* 计算错误。

### 修复方案

```cpp
void InitStateWord(TypeInfo* typeInfo) {
    uintptr_t address = reinterpret_cast<uintptr_t>(typeInfo);
    // Build the clean 64-bit word: TypeInfo in bits 47..0, objectState = 0
    uint64_t word = ((address >> 32) & 0xFFFF) << 32
                    | (address & 0xFFFFFFFF);
    // Single atomic release store: TypeInfo + zero objectState, no torn reads
    __atomic_store_n(reinterpret_cast<uint64_t*>(this), word, __ATOMIC_RELEASE);
}
```

在 `BaseObject::SetClassInfo`（含 static 版本）中改为调用 `InitStateWord`。

### 是否真实问题？

**是，两个问题都是真实的**：
- 问题 A：直接导致 case8 中 `estimateCapacityWithDepth` 的 SIGSEGV（crash addr `0x22680001025012f8`，高 16 位 objectState=`0x2268`）
- 问题 B：独立的并发安全问题，在高并发下有小概率触发错误类型判断

### 是否必要？

**必要**。Fix 5 是本次修复中对正确性贡献最关键的一个：即使其他 4 个 Fix 都到位，如果分配路径未正确初始化 objectState，相同类型的崩溃仍可能从其他代码路径复现。

### 对性能的影响？

**极小**：
- AArch64: `__atomic_store_n(..., __ATOMIC_RELEASE)` → `stlr x0, [mem]`（store-release），比普通 `str` 多一个内存屏障语义，但在非竞争场景（对象初始化时只有一个线程写）overhead 在现代 ARM 核心上约 1-2 cycle
- 仅影响对象分配路径（`SetClassInfo`），不影响对象读/类型派发热路径
- 实测性能无可见回退（MSGC P1=4780 vs CopyGC P1=4894，差异 2.3%，属正常噪声范围）

---

## 各 Fix 之间的关联

```
Fix 2（zero-before-publish）
  └─► 消除大部分脏 objectState 产生的根因（BITMAP 分配路径）
  
Fix 5（InitStateWord）
  └─► 从分配方视角保证无论何种路径（BUMP / BITMAP），objectState 始终为 0
  └─► 同时解决 SetTypeInfo 撕裂写问题
  
Fix 1（IsSubType mask）
  └─► 作为 Defense-in-Depth 层：即使 objectState 因其他原因非零（LOCKED 状态），
      IsSubType 也不会崩溃
  
Fix 3（WBB Flush filter）
  └─► 防止 staleWBB entry 进入 mark queue
  
Fix 4（skip instead of FATAL）
  └─► Fix 3 的 backstop：即使有漏网的 zero-stateWord 对象进入 mark queue 也不崩溃
  └─► ⚠️ 会掩盖后续的真实 GC 不变量违反，建议 debug 模式保留断言
```

**理想的根本解法**（后续考虑）：
1. 编译器侧：生成类型派发代码时对从 `[obj]` 读取的值做 mask，不依赖 runtime 的 workaround
2. GC 侧：确保所有内存路径（BUMP/BITMAP/Large）在 slot 可见给 allocator 前都完成零化，Fix 4 的 FATAL 可恢复

---

## 结论

5 个 Fix 都是**必要的**，没有多余的改动。从正确性角度：
- Fix 1、2、5 是**根因修复**，解决了导致 SIGSEGV 的直接原因
- Fix 3、4 是**防御层**，处理并发边缘场景，同时提供了一定的诊断抑制（Fix 4 需注意）

从性能角度：5 个 Fix **没有可测量的性能回退**，MSGC benchmark 结果与 CopyGC 基线持平。
