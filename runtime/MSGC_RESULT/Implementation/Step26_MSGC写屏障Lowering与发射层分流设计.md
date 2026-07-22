# Step26：MSGC 写屏障 Lowering 与发射层分流设计

> 2026-06-19
> 目标：把 Step25 中“compiler fast path + 最小 ABI 固化”的待办，进一步细化成可直接实施的 Step26 设计。
> 结论先行：**保留原 `llvm.cj.gcwrite_*` 接口，不改前端；在 MSGC 下由 lowering 分流到 MSGC 专用 wrapper，再由发射层针对这些 wrapper 做 fast emission。**

---

## 目录

1. [背景与问题定义](#1-背景与问题定义)
2. [核心结论](#2-核心结论)
3. [当前代码基础](#3-当前代码基础)
4. [方案对比与推荐](#4-方案对比与推荐)
5. [推荐方案详细设计](#5-推荐方案详细设计)
6. [第一阶段范围控制](#6-第一阶段范围控制)
7. [文件级改动清单](#7-文件级改动清单)
8. [风险点与回退策略](#8-风险点与回退策略)
9. [验证口径](#9-验证口径)
10. [实施顺序](#10-实施顺序)

---

## 1. 背景与问题定义

Step25 已经明确了一件事：

> MSGC runtime 侧“写时只追加日志，flush 时再做 discover / queue transfer”这一步已经完成，真正未完成的是 compiler fast path 和最小 ABI 固化。

因此 Step26 不再讨论 runtime 语义是否要继续从写时 mark 迁移，因为这件事已经迁移完了。Step26 真正要解决的是下面两个问题：

1. **能不能不改前端接口，直接把现有写屏障调用变成 MSGC 专用 fast path。**
2. **如果能，分流点应该放在哪一层，才能既吃到性能收益，又不把 compiler 和 collector 内部结构绑死。**

这一步如果走错方向，容易出现两类坏结果：

1. 过早改前端 intrinsic，导致改动面很大，且对现有 SDK/IR 兼容性不友好。
2. 让 compiler 直接知道 `DiscoverObject`、`PerPGCWork`、`globalFull_` 之类 collector 内部结构，导致后续 runtime 演进困难。

所以 Step26 的目标不是“再发明一套新的写屏障接口”，而是：

> **保留原 `llvm.cj.gcwrite_*` 入口，只在 MSGC 路径下把它们 lower 成 MSGC 专用 wrapper，再由目标后端对这些 wrapper 发射真正的薄 fast path。**

## 2. 核心结论

Step26 的核心结论有 6 条：

1. **前端 intrinsic 不改。**
   继续保留：
   - `cj_gcwrite_ref`
   - `cj_gcwrite_struct`
   - `cj_gcwrite_static_ref`
   - `cj_gcwrite_static_struct`
   - `cj_array_copy_ref`
   - `cj_array_copy_struct`

2. **MSGC 专用分流点放在 `CJBarrierLowering.cpp`，而不是放在前端。**

3. **non-mark 期继续沿用现有 `EnableGCPhase` 逻辑，直接退化为普通 store / memcpy。**

4. **mark 期不再直接落到通用 `CJ_MCC_WriteRefField`，而是改成 MSGC 专用 wrapper。**

5. **AArch64 发射层当前识别 6 个 MSGC wrapper，并发射“load old + store new/copy + append old + ABI 不满足时 slow fallback”的薄路径。**

6. **当前已经完成 `gcwrite_ref / gcwrite_static_ref / gcwrite_static_struct / array_copy_ref / gcwrite_struct / array_copy_struct`；剩余 `generic / atomic / X86` 仍保留在后续阶段。**

一句话总结：

> **Step26 要做的不是换前门，而是在原前门后面加一条 MSGC 专用后厨通道。**

## 3. 当前代码基础

本节只盘点当前已经具备的代码条件，包括：

1. compiler 仍然发原始 `llvm.cj.gcwrite_*`
2. lowering 当前仍映射到通用 `CJ_MCC_Write*`
3. runtime 的 MSGC 写屏障已经是 old-only + log-only
4. `wbBuf` 已经在 `AllocBufferMS`

结合当前代码，可以确定 Step26 已经具备 4 个直接前提：

### 3.1 compiler 仍然发原始 `llvm.cj.gcwrite_*`

当前 `CJNativeIRBuilder.cpp` 仍然通过 `CallGCWrite()` / `CallGCWriteStaticRef()` 发：

- `llvm.cj.gcwrite.ref`
- `llvm.cj.gcwrite.static.ref`

这说明：

1. 前端接口目前是稳定的
2. 不需要为了 MSGC 再加一套新的 frontend intrinsic

### 3.2 lowering 当前仍直接落到通用 runtime 符号

当前 `CJBarrierLowering.cpp` 中 `IntrinsicMap` 仍然是：

- `cj_gcwrite_ref -> CJ_MCC_WriteRefField`
- `cj_gcwrite_static_ref -> CJ_MCC_WriteStaticRef`

也就是说，**真正的改造入口就是这里**。只要在这里加 MSGC 分流，就不需要动前端 CHIR/IR 生成层。

### 3.3 MSGC runtime 的写屏障已经是 old-only + log-only

当前 `MSMarkBarrier::EnqueueRefFast()` 已经只做：

1. 过滤空指针 / 非堆对象
2. 取 `AllocBufferMS`
3. 满了则 `FlushWriteBarrierOverflow`
4. `wbBuf.Push(old)`

没有写时 `DiscoverObject`，这意味着 compiler fast path 只要把 old 追加进当前 owner 的 `wbBuf`，语义上就是和 runtime 现状对齐的。

### 3.4 `wbBuf` 与 `gcWork_` 已经在 `AllocBufferMS`

当前 `WriteBarrierBuffer wbBuf_` 和 `PerPGCWork gcWork_` 已经都挂在 `AllocBufferMS` 上。

这点很关键，因为它意味着 Step26 不需要再做“先把 wbBuf 从 Mutator 搬走”这类前置工程，可以直接进入 fast path 固化。

## 4. 方案对比与推荐

本节对比三种路线：

1. 新增前端 intrinsic
2. 保留原 intrinsic，lowering 分流到 MSGC wrapper
3. 直接让 compiler 知道 collector 内部 ABI

并给出推荐方案。

### 方案 A：新增前端 intrinsic

例如新增：

- `llvm.cj.msgc.gcwrite.ref`
- `llvm.cj.msgc.gcwrite.static.ref`

优点：

1. 语义隔离干净
2. 不会影响 CopyGC 现有 lowering

缺点：

1. 前端、IRBuilder、intrinsic 定义、优化 pass、lowering 全都要动
2. 破坏现有接口稳定性
3. 为了一个后端分流问题，把改动面扩展到了前端

结论：**不推荐作为第一步。**

### 方案 B：保留原 intrinsic，在 lowering 中分流到 MSGC wrapper

优点：

1. 改动集中在 `CJBarrierLowering` 与目标后端
2. 保留现有前端与 IR 接口
3. 可以和当前 MSGC alloc fast path 的 wrapper / emission 模式保持一致

缺点：

1. 需要在 lowering 中新增一层 MSGC wrapper 生成逻辑
2. 需要固化一小块 runtime/compiler 共享 ABI

结论：**这是推荐方案。**

### 方案 C：让 compiler 直接知道 collector 内部 ABI

例如让发射层直接调用或理解：

- `DiscoverObject`
- `TryEnqueueGreyObject`
- `PerPGCWork`
- `globalFull_`

优点：

1. 表面上 runtime call 可能更少

缺点：

1. compiler 与 collector 内部协议耦合过深
2. runtime 内部结构一改，compiler 也要联动
3. 很难做最小 ABI 封装

结论：**明确不建议。**

## 5. 推荐方案详细设计

本节拆解完整流程：

1. 前端 / IR 层保持不变
2. `CJBarrierLowering` 中增加 MSGC 写屏障分流
3. MSGC wrapper 的符号与职责
4. AArch64 发射层如何对 wrapper 做 fast emission
5. X86 如何跟进
6. 最小 ABI 如何固化

### 5.1 总体控制流

推荐方案的完整控制流如下：

```text
frontend / IRBuilder
    emit llvm.cj.gcwrite_ref / llvm.cj.gcwrite_static_ref
        |
        v
CJBarrierLowering
    non-mark path -> direct store
    mark path     -> rewrite to CJ_MCC_WriteRefFieldFast_MSGC / CJ_MCC_WriteStaticRefFast_MSGC
        |
        v
target emission (AArch64 first)
    emit thin fast path:
      load old
      store new
      append old to wbBuf
      if full / fallback -> slow helper
        |
        v
runtime slow path
    current MCC_WriteRefField / MCC_WriteStaticRef
    or future dedicated overflow slow helper
```

这里最关键的一点是：

> `GetGCPhase` 的判断继续留在 lowering，不放到 wrapper，也不放到 runtime。

这样 non-mark 热路径仍然能直接变普通 store，不会多出无意义的 wrapper call。

### 5.2 前端 / IR 层保持不变

Step26 第一阶段不改：

- `CJNativeIRBuilder.cpp`
- `Intrinsics.td`
- `CJIntrinsics.h` 的写屏障接口形状

这样做的原因很简单：

1. 当前接口已经足够表达 `ref + baseObj + fieldPtr`
2. Step26 的问题不是“IR 表达不出来”，而是“lowering 后落到了太厚的通用 runtime 接口”
3. 先不动前端，能最大限度减少影响范围

### 5.3 `CJBarrierLowering` 中增加 MSGC 写屏障分流

这一层是 Step26 的主入口。

建议增加一个独立开关，例如：

- `EnableMSGCWriteBarrierLowering`

语义与现有 `EnableMSGCReadBarrierLowering` 对齐。

在 `BarrierMaker::replaceInstWithBarrier()` 中，对下面两类 intrinsic 做 MSGC 特判：

1. `cj_gcwrite_ref`
2. `cj_gcwrite_static_ref`

当满足：

1. 当前启用 MSGC 写屏障 lowering
2. barrier 仍位于 mark path

则不再映射到：

- `CJ_MCC_WriteRefField`
- `CJ_MCC_WriteStaticRef`

而是映射到：

- `CJ_MCC_WriteRefFieldFast_MSGC`
- `CJ_MCC_WriteStaticRefFast_MSGC`

注意，这里的 `Fast` 不是说“永不回退”，而是说：

> **它是一个给目标后端识别和发射的薄 wrapper 入口。**

full、layout 不满足、owner 获取失败等情况，仍然可以回落 slow path。

### 5.4 wrapper 的职责边界

这两个 wrapper 不应该承载完整 collector 语义，它们的职责应当只到：

1. load old
2. store new
3. 若 `old == nullptr` 则直接返回
4. 取当前 `AllocBufferMS`
5. 取 `wbBuf`
6. 若 `wbBuf` 未满，则 `append(old)`
7. 否则进入 slow helper

它们**不应该**直接做：

- `DiscoverObject`
- `TryEnqueueGreyObject`
- `WorkBuf` 组装
- `globalFull_` 操作

这些仍然留在 runtime flush / slow path。

### 5.5 AArch64 发射层的具体做法

第一阶段建议只在 `AArch64AsmPrinter.cpp` 做发射层支持。

原因：

1. 当前主验证环境就是 darwin aarch64
2. 现有 MSGC alloc fast path 也已经在 AArch64 侧有成熟模式

建议做法：

1. AsmPrinter 识别 `CJ_MCC_WriteRefFieldFast_MSGC`
2. 发射出一段固定模板汇编
3. 模板内部复用现有 MSGC fast path 的 TLS / thread-local 获取方式

对 `WriteRefFieldFast_MSGC(ref, obj, field)`，发射逻辑建议为：

```text
old = *field
*field = ref
if old == null:
    return
if obj 不是 heap obj:
    return
allocBuf = current TLS / current owner
if allocBuf == null:
    call MCC_WriteRefField(ref, obj, field) slow
    return
wb = allocBuf->wbBuf_
if wb.count == wb.capacity:
    call MCC_WriteRefField(ref, obj, field) slow
    return
wb.buf[wb.count] = old
wb.count++
return
```

对 `WriteStaticRefFast_MSGC(ref, field)`，去掉 `obj` 检查即可：

```text
old = *field
*field = ref
if old == null:
    return
allocBuf = current TLS / current owner
...
```

这里有一个细节要特别说明：

`MCC_WriteRefField()` 现在是“先做 heap 检查，再调 barrier”。所以如果发射层无法静态保证 `obj` 一定是 heap object，就要在 fast path 中保守保留 `obj` heapness fallback。

实现落地时还有一个需要记住的点：

> **AArch64 当前是在“调用 `CJ_MCC_Write*Fast_MSGC` wrapper 的 callsite”直接展开 fast emission，wrapper 自身仍保留为 thin slow body。**

也就是说，wrapper 在现实现状里更像“供 lowering/AsmPrinter 识别的标记入口 + slow fallback 承载体”，而不是“把 fast path 汇编写在 wrapper 函数体内部”。

### 5.6 X86 跟进策略

第一阶段不要求 X86 同步完成，但设计上要预留同样的 wrapper 名字与 ABI。

后续在 `X86MCInstLower.cpp` 中按同一符号复刻发射逻辑即可。这样可以保证：

1. lowering 逻辑跨平台一致
2. wrapper ABI 一次设计，多后端共享

### 5.7 最小 ABI 固化方式

Step26 需要固化的 ABI 只限于下面几项：

1. 如何从当前线程上下文拿到 `AllocBufferMS`
2. `AllocBufferMS` 到 `wbBuf_` 的稳定偏移
3. `WriteBarrierBuffer` 的稳定布局
4. slow helper 的稳定符号名

建议第一阶段采用和现有 alloc fast path 一样的思路：

1. compiler / AsmPrinter 硬编码一小块 ABI 假设
2. runtime 侧通过 `static_assert` 和注释把这些假设钉死

至少需要固化：

1. `AllocBufferMS::wbBuf_` offset
2. `WriteBarrierBuffer::buf_` offset
3. `WriteBarrierBuffer::count_` offset
4. `WriteBarrierBuffer::kCapacity`

明确不暴露给 compiler 的内容包括：

- `DiscoverObject`
- `TryEnqueueGreyObject`
- `PerPGCWork` 内部结构
- `WorkBufPool`
- `globalFull_`

### 5.8 写侧 callsite 总表与当前分工

这里的“写屏障 callsite”有两个口径，先分清楚：

1. **狭义 Step26 callsite**：指 AArch64 发射层当前真正识别并展开的 MSGC wrapper callsite。
2. **广义写侧入口**：指 `CJBarrierLowering` 当前会识别并最终 lower 到 runtime/helper 的所有“写侧 / 覆盖旧引用侧” intrinsic。

先给出狭义 Step26 口径：

| 口径 | 数量 | 明细 | 当前状态 |
|------|------|------|----------|
| AArch64 MSGC wrapper callsite | 6 | `CJ_MCC_WriteRefFieldFast_MSGC`、`CJ_MCC_WriteStaticRefFast_MSGC`、`CJ_MCC_WriteStaticStructFast_MSGC`、`CJ_MCC_ArrayCopyRefFast_MSGC`、`CJ_MCC_WriteStructFieldFast_MSGC`、`CJ_MCC_ArrayCopyStructFast_MSGC` | **都已展开** |

其中最基础的两类 callsite 行为如下：

1. `WriteRefFieldFast_MSGC(ref, obj, field)`
   - non-mark path：仍由现有 `EnableGCPhase` 路径直接退化成普通 store
   - mark path：在 callsite 展开 `load old -> store new -> old 过滤 -> append old to wbBuf`
   - 失败条件：`obj` 非 heap、`old == null`、buffer 满、TLS/owner 不可用等情况下回落 slow helper

2. `WriteStaticRefFast_MSGC(ref, field)`
   - non-mark path：直接普通 store
   - mark path：在 callsite 展开 `load old -> store new -> old 过滤 -> append old to wbBuf`
   - 失败条件：buffer 满、TLS/owner 不可用等情况下回落 slow helper

再给出广义写侧入口口径。当前 `CJBarrierLowering` 视角下，一共有 14 类“写侧 / 覆盖旧引用侧”入口：

| 类别 | intrinsic / lowering 入口 | 第一阶段是否做发射层展开 | 当前操作 |
|------|---------------------------|--------------------------|----------|
| 单 slot 引用写 | `cj_gcwrite_ref -> CJ_MCC_WriteRefFieldFast_MSGC / CJ_MCC_WriteRefField` | **是** | mark 期走 callsite fast emission；non-mark 期直接 store；失败则回 `MCC_WriteRefField` |
| 单 slot 静态引用写 | `cj_gcwrite_static_ref -> CJ_MCC_WriteStaticRefFast_MSGC / CJ_MCC_WriteStaticRef` | **是** | mark 期走 callsite fast emission；non-mark 期直接 store；失败则回 `MCC_WriteStaticRef` |
| 对象内 struct 覆盖写 | `cj_gcwrite_struct -> CJ_MCC_WriteStructFieldFast_MSGC / CJ_MCC_WriteStructField` | **是** | mark 期走 callsite fast emission；通过 `obj->StateWord->TypeInfo->GCTib` 动态取 short GCTib；要求 `dst/size` 8-byte 对齐，否则回 slow |
| 静态 struct 覆盖写 | `cj_gcwrite_static_struct -> CJ_MCC_WriteStaticStructFast_MSGC / CJ_MCC_WriteStaticStruct` | **是** | mark 期走 callsite fast emission；使用 lowering 传下来的 `AggType` short GCTib；非 short bitmap 或容量不足时回 slow |
| struct 字段拷贝 | `cj_copy_struct_field -> CJ_MCC_CopyStructField` | 否 | 直接走 runtime，最终复用 `WriteStruct` 语义 |
| ref 数组拷贝 | `cj_array_copy_ref -> CJ_MCC_ArrayCopyRefFast_MSGC / CJ_MCC_ArrayCopyRef` | **是** | mark 期走 callsite fast emission；按 `copyLen / 8` 扫描目标区旧引用，再 memmove |
| struct 数组拷贝 | `cj_array_copy_struct -> CJ_MCC_ArrayCopyStructFast_MSGC / CJ_MCC_ArrayCopyStruct` | **是** | mark 期走 callsite fast emission；通过 `dstObj->StateWord->TypeInfo->componentTypeInfo->{gctib,componentSize}` 动态取 short GCTib；仅扫描完整元素，ABI 不满足时回 slow |
| generic 数组拷贝 | `cj_array_copy_generic -> CJ_MCC_ArrayCopyGeneric` | 否 | 直接走 runtime，按元素类型分发到 `CopyRefArray` / `CopyStructArray` / `memmove` |
| 原子 store | `cj_atomic_store -> CJ_MCC_AtomicWriteReference` | 否 | non-mark 期直接 atomic store；mark 期走 runtime，先 log(old) 再 atomic store |
| 原子 swap | `cj_atomic_swap -> CJ_MCC_AtomicSwapReference` | 否 | 直接走 runtime，做 exchange 后再 log 被换出的 old |
| 原子 CAS | `cj_atomic_compare_swap -> CJ_MCC_AtomicCompareAndSwapReference` | 否 | 直接走 runtime，仅在 CAS 成功时 log old |
| generic 写 | `cj_gcwrite_generic -> CJ_MCC_WriteGeneric` | 否 | 直接走 runtime，按 `obj/src/typeInfo` 关系分发到 `WriteStruct` / `ReadStruct` / `memcpy` |
| generic 赋值 | `cj_assign_generic -> CJ_MCC_AssignGeneric` | 否 | 直接走 runtime；无 ref field 时 memcpy，有 ref field 时转 `WriteGeneric` |
| generic payload 写 | `cj_gcwrite_generic_payload -> CJ_MCC_WriteGenericPayload` | 否 | 直接走 runtime；无 ref field 时 memcpy，有 ref field 时转 `WriteStruct` |

为什么实现上会先从 `ref/static_ref` 起步，再逐步扩展到现在这 6 类：

1. `ref/static_ref` 是最简单的 **single-slot old-edge**，fast path 只需要 `load old + append old + store new`
2. 4 个 bulk 路径虽然也能进 fastpath，但都不是简单的“push 一个 oldRef”，而是要先扫描目标覆盖区里的旧引用
3. `generic / atomic` 到现在仍然不适合照搬这套模式：
   - `generic` 需要运行时按 `typeInfo` 分发
   - `atomic swap / cas` 需要保住原子语义、返回值和成功位

因此 Step26 当前阶段的分工可以概括成一句话：

> **AArch64 callsite fast emission 现在已经覆盖 `ref/static_ref + 4 个 bulk struct/array_copy` 共 6 类；剩余 `generic / atomic / copy_struct_field / X86` 仍由 lowering 做 phase 分流或直接落回 runtime。**

### 5.9 bulk barrier fastpath 落地结果与设计要点

截至 2026-06-19，下面 4 类 bulk barrier 已经落地，并且在 LLVM `shouldLowerMSGCWriteBarrier()` 中默认开启：

1. `cj_gcwrite_struct`
2. `cj_gcwrite_static_struct`
3. `cj_array_copy_ref`
4. `cj_array_copy_struct`

这 4 类路径共同遵守的前提是：

> **它们都不是“push 一个 oldRef”问题，而是“先扫描目标覆盖区里的旧引用，再做 memcpy / memmove”问题。**

也就是说，这几类路径都必须遵守同一个顺序：

```text
scan overwritten old refs in dst range
    ->
append/log old refs
    ->
do memcpy / memmove
```

不能反过来先 copy 再补 log，否则旧边已经丢失。

#### 5.9.1 这 4 类路径的实际推进顺序

实际采用的推进顺序基本如下，而不是四类一起做：

1. **`cj_gcwrite_static_struct` 先做**
2. **`cj_array_copy_ref` 第二个做**
3. `cj_gcwrite_struct` 先补布局信息，再做
4. `cj_array_copy_struct` 最后做

原因：

1. `gcwrite_static_struct` 当前已经自带 `AggType` metadata，lowering 可以直接派生 GCTib / bitmap
2. `array_copy_ref` 的 ref layout 是固定的，扫描逻辑最简单
3. `gcwrite_struct` / `array_copy_struct` 编译器侧目前缺少足够的静态布局信息，不适合第一步就把 `TypeInfo/GCTib` 动态布局 ABI 钉死在后端

#### 5.9.2 `cj_gcwrite_static_struct` 的 fastpath 形态

推荐新增 wrapper：

- `CJ_MCC_WriteStaticStructFast_MSGC`

控制流：

```text
non-mark path:
    direct memcpy

mark path:
    use AggType-derived bitmap
    need = popcount(bitmap)   ; conservative upper bound
    if wbBuf remaining < need:
        slow fallback
    for each ref slot in dst selected by bitmap:
        old = *(dst + slot)
        if old != null && heap(old):
            wbBuf.push(old)
    memcpy(dst, src, size)
```

实现建议：

1. 第一版只支持 `AggType` 对应的小 bitmap 形态
2. bitmap 过大、布局不满足、buffer 空间不足时，统一 slow fallback 到现有 `MCC_WriteStaticStruct`
3. 这里的 `need = popcount(bitmap)` 只是保守上界，不要求精确统计 heap oldRef 数量

#### 5.9.3 `cj_array_copy_ref` 的 fastpath 形态

推荐新增 wrapper：

- `CJ_MCC_ArrayCopyRefFast_MSGC`

控制流：

```text
non-mark path:
    direct memmove

mark path:
    copyLen = min(dstSize, srcSize)
    slotCount = copyLen / sizeof(void*)
    if wbBuf remaining < slotCount:
        slow fallback
    for i in [0, slotCount):
        old = dst[i]
        if old != null && heap(old):
            wbBuf.push(old)
    memmove(dst, src, copyLen)
```

这里直接按 `slotCount` 做容量上界判断即可，优点是：

1. 不需要先精确统计 heap oldRef 个数
2. 容量不足时立即 slow fallback，保证不会出现“扫描到一半 wbBuf 满了”的半完成状态
3. 与当前 runtime 语义一致，因为 runtime 本质也是“扫描目标覆盖区旧引用，再做 memmove”

#### 5.9.4 `cj_gcwrite_struct` 的 direct-ABI fastpath 形态

当前实现已经采用 direct-ABI 路线，但把边界收得很窄，只固化对 AArch64 MSGC 必需的对象模型读取：

1. `obj` 头部 `stateWord` 位于 offset 0
2. 通过 acquire load 读取 `stateWord`
3. 低 48 bit 还原 `TypeInfo*`
4. 从 `TypeInfo + 16` 读取 `gctib`

对应 wrapper：

- `CJ_MCC_WriteStructFieldFast_MSGC`

控制流：

```text
non-mark path:
    direct memcpy

mark path:
    if obj 不是 heap object:
        memcpy
        return
    stateWord = ldar [obj]
    typeInfo = stateWord & low48
    gctib = *(typeInfo + 16)
    if gctib 不是 short bitmap:
        slow fallback
    if dst/size 不是 8-byte 对齐:
        slow fallback
    slotUpperBound = size / 8
    if wbBuf remaining < slotUpperBound:
        slow fallback
    bitmap >>= ((dst - (obj + 8)) / 8)
    for each set bit while ptr < dst + size:
        old = *ptr
        if old != null && heap(old):
            wbBuf.push(old)
        ptr += 8
    memcpy(dst, src, size)
```

这里保留 direct-ABI 的原因是：

1. compiler 栈里本来就已经存在 `GetTypeInfoFromObject()`、`GetPayloadFromObject()` 一类对象模型 ABI 假设
2. `gcwrite_struct` 没有 `AggType` metadata，想进入 fastpath只能走动态 `TypeInfo/GCTib`
3. 通过“仅支持 short GCTib + 8-byte 对齐范围 + 统一 slow fallback”可以把风险控制在最小闭包

#### 5.9.5 `cj_array_copy_struct` 的 direct-ABI fastpath 形态

当前实现对应 wrapper：

- `CJ_MCC_ArrayCopyStructFast_MSGC`

控制流：

```text
non-mark path:
    direct memmove

mark path:
    copyLen = min(dstSize, srcSize)
    if copyLen == 0:
        memmove
        return
    if dstObj 不是 heap object:
        memmove
        return
    stateWord = ldar [dstObj]
    typeInfo = stateWord & low48
    componentTypeInfo = *(typeInfo + 64)
    gctib = *(componentTypeInfo + 16)
    componentSize = *(u32*)(componentTypeInfo + 12)
    if gctib 不是 short bitmap:
        slow fallback
    if dst/copyLen/componentSize 不满足 8-byte 对齐:
        slow fallback
    slotUpperBound = copyLen / 8
    if wbBuf remaining < slotUpperBound:
        slow fallback
    elemCount = copyLen / componentSize
    for each complete destination element:
        for each set bit in component bitmap:
            old = *(elementBase + slot*8)
            if old != null && heap(old):
                wbBuf.push(old)
    memmove(dst, src, srcSize)
```

这里刻意只扫描完整元素，和 runtime `MArray::ForEachRefFieldInRange()` 的 `size / elementSize` 行为保持一致；尾部不足一个元素的字节仍会被 `memmove` 覆盖，但不会进入 ref 扫描。

#### 5.9.6 当前落地顺序与状态

当前 Step26 的实际增量顺序已经变成：

1. `gcwrite_ref / gcwrite_static_ref`
2. `gcwrite_static_struct`
3. `array_copy_ref`
4. `gcwrite_struct`
5. `array_copy_struct`

一句话总结：

> **bulk barrier fastpath 的核心仍然是“先把 dst 覆盖区的 old refs 安全扫出来，再做 copy”；只是 `gcwrite_struct / array_copy_struct` 因为缺少静态布局信息，最终采用了受限 direct-ABI 版本，并把非 short GCTib、非 8-byte 对齐等情况统一回 slow。**

## 6. 当前实现范围控制

截至 2026-06-19，Step26 当前实现范围如下：

1. 已做：
   - `cj_gcwrite_ref`
   - `cj_gcwrite_static_ref`
   - `cj_gcwrite_static_struct`
   - `cj_array_copy_ref`
   - `cj_gcwrite_struct`
   - `cj_array_copy_struct`
   - AArch64 callsite fast emission
   - old-only logging 语义保持不变

2. 未做：
   - `atomic_store / atomic_swap / cas` 下沉到发射层
   - `generic` 系列写屏障下沉到发射层
   - X86 后端对应 emission
   - short GCTib 之外的 `std`/大 bitmap 形态
   - 更激进的精确容量估算（当前 bulk fastpath 统一使用 `copyLen / 8` 或 `size / 8` 上界）

## 7. 文件级改动清单

本节列出 runtime 与 compiler 两边的目标修改文件和各自职责。

### 7.1 runtime 侧

1. `src/Heap/MBarrier/MSMarkBarrier.cpp`
   - 语义不改
   - 继续保持 old-only + log-only

2. `src/CompilerCalls.cpp`
   - 保留当前 `MCC_WriteRefField` / `MCC_WriteStaticRef` 作为 slow fallback
   - 如后续需要，再单独加更薄的 overflow helper

3. `src/Heap/MAllocator/MSAllocBuffer.h`
   - 为 `wbBuf_` 布局补 ABI 注释
   - 补必要的 layout assert

4. `src/Heap/MBarrier/WriteBarrierBuffer.h`
   - 为 `buf_ / count_ / capacity` 补 ABI 注释
   - 补必要的 layout assert

### 7.2 compiler / llvm 侧

1. `cangjie_compiler/third_party/llvm-project/llvm/lib/CodeGen/CJBarrierLowering.cpp`
   - 新增 `EnableMSGCWriteBarrierLowering`
   - 在 mark path 将 `gcwrite_ref / gcwrite_struct / gcwrite_static_ref / gcwrite_static_struct / array_copy_ref / array_copy_struct` 改写为 MSGC wrapper

2. `cangjie_compiler/third_party/llvm-project/llvm/lib/Target/AArch64/AArch64AsmPrinter.cpp`
   - 识别 6 个 MSGC wrapper callsite
   - `gcwrite_struct / array_copy_struct` 额外读取 `stateWord -> TypeInfo -> GCTib/componentTypeInfo/componentSize`
   - 发射薄 fast path

3. `cangjie_compiler/third_party/llvm-project/llvm/lib/Target/X86/X86MCInstLower.cpp`
   - 仍留待后续第二平台跟进

4. `cangjie_compiler/third_party/llvm-project/llvm/test/...`
   - 补 wrapper 生成与 emission 检查测试

### 7.3 前端层

`CJNativeIRBuilder.cpp`、`Intrinsics.td`、`CJIntrinsics.h` 当前仍不改。

## 8. 风险点与回退策略

本节分析 correctness、布局 ABI、平台覆盖等风险，并给出回退开关。

### 8.1 correctness 风险

主要风险有 3 类：

1. `obj` 并不总能静态保证为 heap object
2. `wbBuf` owner 获取失败时不能丢日志
3. layout 假设错了会直接写坏内存

对应策略：

1. `obj` 不能证明安全时，保守 fallback 到现有 `MCC_WriteRefField`
2. owner 取不到、buffer 满、ABI 条件不满足，统一 slow fallback
3. 必须补 runtime 侧 layout assert

### 8.2 平台覆盖风险

第一阶段只做 AArch64，意味着：

1. X86 暂时仍走旧路径
2. wrapper 名字必须跨平台统一，避免后续再改 ABI

### 8.3 回退策略

建议新增一个独立开关：

- `EnableMSGCWriteBarrierLowering`

关闭后恢复到：

1. 原 `cj_gcwrite_*`
2. 原 `CJ_MCC_Write*` lowering
3. 不进入 MSGC wrapper emission

这样可以把 Step26 的回退范围控制在 compiler 侧，不需要回滚 runtime 语义。

## 9. 验证口径

本节定义功能正确性、性能收益、以及回归验证的方法。

验证要分 3 层：

### 9.1 正确性

1. case27 连续跑多轮，无 `SIGSEGV`
2. 无 json parse fail / failed request
3. 无新增 verify 失败
4. `CJ_MS_VERIFY_MARK=1` 下保持稳定

### 9.2 编译与发射

1. IR 中仍看到原 `llvm.cj.gcwrite_*`
2. lowering 后 mark path 调到 `CJ_MCC_WriteRefFieldFast_MSGC` / `CJ_MCC_WriteStaticRefFast_MSGC`
3. AArch64 汇编中能看到对应 wrapper 被展开为 fast emission，而不是普通 call

### 9.3 性能

1. case27 `c8 / 10K`
2. case27 `c8 / 100K`
3. 对比 CPU 利用率
4. 对比吞吐与初始高负载爬升速度

判断标准不是只看单轮最好值，而是看：

1. 多轮稳定性
2. CPU 利用率是否更接近 CopyGC
3. barrier 热路径是否变薄

### 9.4 修改 LLVM 后的完整验证链路

这一点需要单独强调：

> **如果 Step26 改动落在 `CJBarrierLowering.cpp`、`AArch64AsmPrinter.cpp`、`X86MCInstLower.cpp` 这类 compiler / LLVM 代码里，那么验证时不能只替换 runtime dylib。**

原因是：

1. case27 的 `benchmark.cj` 是由 `cjc` 现编译出来的
2. `cjc` 自身来自 compiler 构建产物
3. `std` 需要用新的 `cjc + runtime` 重新构建，才能和新的 ABI / emission 对齐
4. `stdx` 又依赖新的 `CANGJIE_HOME`，会继续消费新的 `cjc`、`third_party/llvm`、`runtime/lib` 和 `lib`

所以 Step26 的正确验证链路是：

```text
改 llvm/compiler
  -> 重建 compiler
  -> 重建 runtime
  -> 用新 cjc + 新 runtime 重建 std
  -> 把 compiler + runtime + std 组合成 out SDK
  -> 用 out SDK 重建 stdx
  -> 用 out SDK + stdx target 去编译/执行 case27
```

#### 9.4.1 先组一个 `out` SDK 根

参考 [build_cangjie_compiler_one_shot_zh.md](../../build_cangjie_compiler_one_shot_zh.md) 的链路，推荐把：

```text
$COMPILER_ROOT/output
```

作为组合后的 SDK 根，记作：

```bash
export OUT_SDK_ROOT=$COMPILER_ROOT/output
```

这个 `OUT_SDK_ROOT` 内应至少包含：

1. 新的 `bin/cjc`
2. 新的 `third_party/llvm`
3. 新的 `runtime/lib/.../libcangjie-runtime.dylib`
4. 新的 `lib/...`
5. 新的 `envsetup.sh`

对于 Step26 这种 barrier lowering / emission 改动，`OUT_SDK_ROOT` 才是后续所有验证的唯一可信 SDK 根。

#### 9.4.2 compiler、runtime、std 的组合顺序

建议顺序如下：

1. **重建 compiler**
   - 产出新的 `cjc`
   - 安装到 `cangjie_compiler/output`

2. **重建 runtime**
   - **必须显式使用 `--gc-mode marksweep`**
   - 一次 install 到 `runtime/output/common/...`，供 stdlib 构建时 `--target-lib` 使用
   - 一次 install 到 `cangjie_compiler/output`，把 runtime 合并进 `OUT_SDK_ROOT`

3. **重建 stdlib**
   - `source "$OUT_SDK_ROOT/envsetup.sh"`
   - 用新 `cjc` 构建 stdlib
   - 再把 stdlib install 产物合并回 `OUT_SDK_ROOT`

这一步完成后：

> `OUT_SDK_ROOT = compiler + runtime + std`

它应被视为 Step26 的组合 SDK。

这里有一个 2026-06-19 实际踩到的坑，必须单独记住：

1. `runtime/build.py` 的默认 `--gc-mode` 是 `copygc`
2. 如果按默认命令直接 `build + install`，装进 `OUT_SDK_ROOT` 的就不是 MSGC runtime
3. 当时 `out` SDK 上的 `case27` 首轮 GC 后直接 `Abort trap: 6`
4. 保持 `out` 的 compiler / std / stdx 不变，只把 `runtime/lib/.../libcangjie-runtime.dylib` 临时换成稳定 `dev260509` 的 MSGC runtime 后，同一套 `out` 立即通过 `case27`
5. 这说明那次 `out` 失败的根因不是 LLVM lowering 本身，而是 SDK 组装阶段把 runtime GC 模式装错了
6. compiler 侧的 LLVM 是通过 `ExternalProject(cjnative)` 单独维护的；直接改 `third_party/llvm-project` 后，顶层 `ninja install` / `build.py install` 不一定会自动重编 `llc`
7. 这次实际生效的增量路径是进入 `cangjie_compiler/build/build/third_party/cjnative-prefix/src/cjnative-build` 执行 `ninja llc`，再把新的 `bin/llc` 与 `lib/libLLVM.dylib` 同步回 `compiler/output` 与 `out`
8. 用这份新 `llc` 跑现有 LLVM 用例 `llvm/test/CodeGen/AArch64/CangjieGC/msgc-writebarrier-lowering.ll` 和 `llvm/test/CodeGen/AArch64/cangjie-writebarrier-msgc-fastpath.ll` 已验证通过

另外，当前 runtime 源码里还修过一个会直接挡住 `marksweep` 构建的问题：

1. `MSAllocBuffer.cpp` 里原先直接对 `WriteBarrierBuffer` 私有字段做 `offsetof`
2. `marksweep` 真正参与编译时，这会在 Clang 下报 private member 访问错误
3. 现在已改成 `WriteBarrierBufferLayoutAccess` 这种 friend layout accessor 形式，既保留 ABI 断言，又能让 MSGC runtime 正常出包

#### 9.4.3 `stdx` 必须吃这套新的 `OUT_SDK_ROOT`

`stdx` 构建不是独立于 SDK 的。

从 `cangjie_stdx` 的构建逻辑看，它会通过 `$CANGJIE_HOME` / `envsetup.sh` 使用：

1. `$CANGJIE_HOME/bin/cjc`
2. `$CANGJIE_HOME/third_party/llvm`
3. `$CANGJIE_HOME/runtime/lib/...`
4. `$CANGJIE_HOME/lib/...`

因此在 Step26 场景里，`stdx` 必须在下面这个环境里构建：

```bash
source "$OUT_SDK_ROOT/envsetup.sh"
export PATH="$OUT_SDK_ROOT/bin:$PATH"
```

然后执行：

```bash
cd "$STDX_ROOT"
python3 build.py clean
python3 build.py build -t release --target-lib=<openssl lib 路径>
python3 build.py install
```

`stdx` install 后，验证使用的根目录应取：

```bash
export OUT_STDX_ROOT=$STDX_ROOT/target/darwin_aarch64_cjnative
```

注意这里是：

```text
target/darwin_aarch64_cjnative
```

而不是直接指到 `dynamic/stdx`。因为 case27 的脚本会自动拼：

```text
$MSGC_STDX_ROOT/dynamic/stdx
```

#### 9.4.4 case27 运行时的组合关系

Step26 下 case27 的编译/运行组合应明确分成两部分：

1. **SDK 根**
   - `MSGC_SDK_ROOT=$OUT_SDK_ROOT`

2. **STDX 平台根**
   - `MSGC_STDX_ROOT=$OUT_STDX_ROOT`

也就是说：

```bash
MSGC_SDK_ROOT="$OUT_SDK_ROOT" \
MSGC_STDX_ROOT="$OUT_STDX_ROOT" \
COPYGC_SDK_ROOT=... \
COPYGC_STDX_ROOT=... \
CASE27_PAYLOAD=100K CASE27_CONCURRENCY=8 CASE27_DURATION_MS=60000 \
./run_compare.sh
```

`run_compare.sh` 内部会：

1. `source "$MSGC_SDK_ROOT/envsetup.sh"`
2. 额外把 `DYLD_LIBRARY_PATH` 前缀到 `"$MSGC_STDX_ROOT/dynamic/stdx"`
3. 用新的 `cjc` 重新编译 `benchmark.cj`
4. 用 `MSGC_STDX_ROOT/dynamic/stdx` 作为 `--import-path` 和 `-L`
5. 链接 `libstdx.net.http.dylib`、`libstdx.encoding.json.dylib`
6. 最终在新的 compiler + runtime + std + stdx 组合上执行 case27

注意：`OUT_SDK_ROOT/envsetup.sh` 只会自动注入 `runtime/lib/...` 和 `tools/lib`，
不会自动把 `stdx` 的 `dynamic/stdx` 加进运行时动态库搜索路径。
因此凡是直接运行手工编译出来的 `stdx` benchmark，都需要像 `run_compare.sh`
一样显式补：

```bash
export DYLD_LIBRARY_PATH="$MSGC_STDX_ROOT/dynamic/stdx:${DYLD_LIBRARY_PATH:-}"
```

这也是 Step26 唯一能真实覆盖“barrier lowering + emission 改动是否真的进入业务 benchmark”的验证方法。

## 10. 实施顺序

本节给出当前这轮实际执行下来的落地顺序，便于后续继续扩展。

推荐顺序如下：

### Step 26A：补文档与 ABI 注释

1. 固化 Step26 文档
2. 在 runtime 中为 `AllocBufferMS::wbBuf_`、`WriteBarrierBuffer` 布局补注释和 assert

### Step 26B：在 `CJBarrierLowering` 中生成 MSGC wrapper

1. 增加 `EnableMSGCWriteBarrierLowering`
2. 先改写 `gcwrite_ref/static_ref`
3. 再扩展到 `gcwrite_static_struct / array_copy_ref / gcwrite_struct / array_copy_struct`
4. non-mark path 保持 direct store

### Step 26C：逐个补 AArch64 wrapper emission

1. 识别 `CJ_MCC_WriteRefFieldFast_MSGC`
2. 识别 `CJ_MCC_WriteStaticRefFast_MSGC`
3. 再补 `CJ_MCC_WriteStaticStructFast_MSGC`、`CJ_MCC_ArrayCopyRefFast_MSGC`
4. 最后补 `CJ_MCC_WriteStructFieldFast_MSGC`、`CJ_MCC_ArrayCopyStructFast_MSGC`
5. full / ABI 条件不满足时统一回现有 slow ABI

### Step 26D：重建 compiler、runtime、std 并组合 `out` SDK

1. 按 `build_cangjie_compiler_one_shot_zh.md` 的链路重建 compiler
2. 把 compiler、runtime、std 都并入 `OUT_SDK_ROOT`
3. runtime 这一步必须确认是 `--gc-mode marksweep`，不能吃默认 `copygc`
4. 不要在 LLVM 改动后只替换 runtime dylib 就开始跑 case27

### Step 26E：用组合 SDK 重建 stdx

1. `source "$OUT_SDK_ROOT/envsetup.sh"`
2. 用新的 `cjc + runtime + std` 构建 `stdx`
3. 使用 `target/darwin_aarch64_cjnative` 作为 `MSGC_STDX_ROOT`

### Step 26F：跑 case27 正确性与性能验证

1. 用 `MSGC_SDK_ROOT=$OUT_SDK_ROOT`
2. 用 `MSGC_STDX_ROOT=$OUT_STDX_ROOT`
3. 先看稳定性
4. 再看吞吐
5. 再看 CPU

### Step 26G：当前未完成项

当前 bulk fastpath 已经扩展到 struct / array，但下面几项仍然留在后续阶段：

1. `atomic_store / atomic_swap / cas`
2. `generic` 系列
3. X86 emission
4. 非 short GCTib 形态

最后再强调一次 Step26 的边界：

> **Step26 的目标不是更改 MSGC 的屏障语义，而是把已经确定好的 old-only logging barrier 语义，通过更薄的 lowering 与发射层真正落到 mutator 热路径上。**
