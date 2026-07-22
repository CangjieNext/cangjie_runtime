# Step28：MSGC `cj_gcread_generic` fastpath 设计

> 2026-06-22
> 目标：整理 `cj_gcread_generic` 在 MSGC 下的语义、热点来源与可落地 fastpath 方案。

> 当前前提：本文直接以当前 `llvm-project` 分支“整体服务 MSGC”作为默认前提，不额外引入新的 MSGC 编译器开关。

---

## 目录

1. [背景与问题定义](#1-背景与问题定义)
2. [当前代码事实](#2-当前代码事实)
3. [核心结论](#3-核心结论)
4. [推荐方案](#4-推荐方案)
5. [实施顺序](#5-实施顺序)
6. [风险与回退](#6-风险与回退)
7. [验证口径](#7-验证口径)

---

## 1. 背景与问题定义

在 Step26 之后，MSGC 下已经有一批读屏障被直接 lower 掉：

- `cj_gcread_ref`
- `cj_gcread_weakref`
- `cj_gcread_struct`
- `cj_gcread_static_ref`
- `cj_gcread_static_struct`

但 `cj_gcread_generic` 仍然保留为 runtime helper 调用。

这不是遗漏，而是因为它和前面几类“纯读”不一样：

1. 它的语义不是“读一个 ref”。
2. 它的语义也不只是“把一段 struct 拷到外部临时缓冲区”。
3. 当 `dst` 落在 heap 上时，它在 runtime 中会转成 `WriteStruct`，本质上已经带了 **heap 写语义**。

因此，Step28 要解决的问题不是：

> 能不能像 `gcread_ref` 一样，直接把 `cj_gcread_generic` 一刀切降成普通 `load` / `memcpy`。

而是：

> **如何把 `cj_gcread_generic` 按 `dst` 的实际语义分流：能纯拷贝的路径直接纯拷贝，必须保留 old-edge 语义的路径复用现有 struct-write fastpath。**

---

## 2. 当前代码事实

### 2.1 compiler 侧 `cj_gcread_generic` 的主要产生点

当前前端/IRBuilder 发 `cj_gcread_generic` 的主要场景有 4 类。

#### 2.1.1 泛型 load，且运行时 `T` 为非引用类型

在 `cangjie_compiler/src/CodeGen/CJNative/CJNativeIRBuilder.cpp` 中，generic load 会先按“运行时是不是 ref”分两支：

1. `handleRefBB` 走 `cj_gcread_ref`
2. `handleNonRefBB` 先 `cj_alloca_generic`，再 `cj_gcread_generic`

关键形态如下：

- `valueVal = CallIntrinsicAllocaGeneric({ti, tiSize})`
- `CallGCReadGeneric({valueVal, base, addr, tiSize})`

这说明这里的 `cj_gcread_generic` 本质是：

> 从 `addr` 指向的 generic payload 中，按 `size` 拷一份值，放进一个 generic result object 里。

#### 2.1.2 未知静态大小的 generic element load

同一文件里，`CGType::GetSize() == 0` 的 generic element load 也走同样的模式：

1. 根据运行时 `TypeInfo` 求 size
2. `cj_alloca_generic`
3. `cj_gcread_generic`

这类路径和 2.1.1 的区别只是 size 来源不同，本质仍然是 “copy payload into generic box”。

#### 2.1.3 enum 关联值的 non-ref 提取

在同一文件的 enum associated value 路径中，非引用关联值会：

1. 先为结果分配一个 generic object
2. 再从 enum payload 区把关联值读出来

关键调用形态仍然是：

- `nonRefResult = CallIntrinsicAllocaGeneric(...)`
- `CallGCReadGeneric({nonRefResult, enumValueWithTI, associatedValPtr, sizeOfAssociatedValueType})`

#### 2.1.4 boxing / generic copy 转换路径

在 `cangjie_compiler/src/CodeGen/Base/ExprDispatcher/OtherExprDispatcher.cpp` 中，box non-ref value 时也会：

1. `cj_alloca_generic`
2. `cj_gcread_generic`

这一类说明 `cj_gcread_generic` 不只出现在 field load，还会出现在 generic boxing / copy 组合路径里。

### 2.2 `CallGCReadGeneric` 本身只是一个薄 intrinsic 封装

`cangjie_compiler/src/CodeGen/CJNative/CJNativeIntrinsicsCall.cpp` 中，`CallGCReadGeneric()` 只是对
`llvm.cj.gcread.generic` 的一层薄包装：

- 参数 0：`dst`
- 参数 1：`base obj`
- 参数 2：`field ptr`
- 参数 3：`size`

它本身不做任何额外优化，也不区分 heap / non-heap。

### 2.3 `cj_alloca_generic` 说明很多 `dst` 是“fresh generic 容器”

`cj_gcread_generic` 的 `dst` 常常来自 `cj_alloca_generic`。

而 `llvm/lib/Transforms/Scalar/CJRuntimeLowering.cpp` 中，`replaceAllocaGeneric()` 的逻辑是：

1. 先尝试 `replaceWithAlloca(CI, ES)`
2. 否则回退到 runtime `CJ_MCC_NewObject`

因此这里有两个重要结论：

1. `dst` 经常是 **fresh** 的 generic 容器。
2. 这个 fresh 容器后续可能 lower 成 **栈对象**，也可能 lower 成 **堆对象**。

也就是说，我们真正应该抓住的性质不是“它一定在 heap 上”，而是：

> **它往往是一个刚分配出来、payload 还没有承载旧 ref 的目标对象。**

### 2.3.1 真正落地时看到的已不是原始 `cj_alloca_generic`

这里还要再补一个实现上的关键细节：

1. `CJRuntimeLowering` 会先处理 `cj_alloca_generic`
2. `CJGenericIntrinsicOpt` 是在它之后跑的
3. `CJBarrierLowering` 则更晚，属于 codegen 期 lowering

因此，如果 Phase 1 选择落在 `CJGenericIntrinsicOpt.cpp`：

> 它真正看到的往往不是原始 `cj_alloca_generic` intrinsic，而是它 lower 之后的结果。

具体来说：

1. 若 `replaceWithAlloca()` 成功，`dst` 会变成一个真实 `alloca` 的 addrspacecast
2. 若回退到 runtime 分配，callsite 会被改成 `CJ_MCC_NewObject` 一类 runtime call，并打上 `!new_gen` / `NoAlias` 信息

所以 Phase 1 的“fresh-dst”识别，不能只按“找到一个 `cj_alloca_generic` 调用点”来做，而要按：

> **lower 后的 fresh provenance（栈 alloca 或带 `new_gen` 语义的 fresh heap object）来做。**

### 2.3.2 stack fresh 与 heap fresh 的可信度并不完全相同

虽然 `dst` 都可以被称为“fresh generic 容器”，但第一版实现里要区分两类：

1. **stack-lowered fresh dst**
   - 这是最硬的子集
   - `replaceWithAlloca()` 会直接在栈上构造 generic box
   - 对非 primitive payload 还会显式 `memset 0`
2. **heap-lowered fresh dst**
   - 语义上也 fresh，但实现上依赖 `new_gen` provenance 与 MSGC 分配器“新对象/回收空槽为零”的运行时不变量
   - 更适合作为 Phase 1 的第二小步，而不是和 stack case 一起一次性首发

### 2.4 runtime 中 `CJ_MCC_ReadGeneric` 的真实语义

`runtime/src/CompilerCalls.cpp` 中，`CJ_MCC_ReadGeneric()` 本身只做一件事：

- `Heap::GetBarrier().ReadGeneric(dstPtr, obj, fieldPtr, size)`

真正的语义在 `runtime/src/Heap/Barrier/Barrier.cpp` 的 `Barrier::ReadGeneric()` 里。

它按 `dstObj` / `obj` 是否在 heap 上分 3 条路：

1. `dst` 非 heap，`src` 非 heap
   - 直接 `memcpy(dst + TYPEINFO_PTR_SIZE, fieldPtr, size)`
2. `dst` 非 heap，`src` 在 heap
   - `ReadStruct(dst + TYPEINFO_PTR_SIZE, obj, fieldPtr, size)`
3. `dst` 在 heap
   - `WriteStruct(dstObj, dst + TYPEINFO_PTR_SIZE, size, fieldPtr, size)`

这条 3 分支非常关键，它说明：

> `cj_gcread_generic` 不是单纯的 read helper，而是一个“generic payload copy helper”；当 `dst` 在 heap 上时，它语义上已经跨成写路径。

### 2.5 MSGC 下真正的读侧成本已经很薄

在 MSGC 中：

- `src/Heap/MBarrier/MSIdleBarrier.h` 的 `ReadStruct()` 是直接 `memcpy_s`
- `src/Heap/MBarrier/MSMarkBarrier.cpp` 的 `ReadStruct()` 也是直接 `memcpy_s`

这意味着：

1. `dst` 非 heap 时，`cj_gcread_generic` 基本上只是“helper + memcpy”
2. 它剩余的真正语义复杂度，集中在 `dst` 在 heap 时落到 `WriteStruct()`

### 2.6 MSGC 下 heap-dst 分支最终会落到 old-edge 保护

`src/Heap/MBarrier/MSMarkBarrier.cpp` 中，`WriteStruct()` 的核心语义是：

1. `EnqueueStructRefsInRange(obj, dst, dstLen)`，先保留目标区域旧 ref
2. 再 `memcpy(dst, src, size)`

所以 heap-dst 路径里真正不能随便删除的是：

> **覆盖写之前对旧引用集合的保留语义。**

不是“读动作本身”。

### 2.7 当前 LLVM MSGC 读屏障 lowering 明确排除了 `cj_gcread_generic`

`llvm/lib/CodeGen/CJBarrierLowering.cpp` 中：

1. `EnableMSGCReadBarrierLowering` 默认开启
2. `shouldLowerMSGCReadBarrier()` 当前只包含：
   - `cj_gcread_ref`
   - `cj_gcread_weakref`
   - `cj_gcread_struct`
   - `cj_gcread_static_ref`
   - `cj_gcread_static_struct`

`cj_gcread_generic` 不在 safe list 里。

而 `cangjie_compiler/third_party/llvm-project/MSGC_RESULT/Plan/MSGC_读屏障安全优化计划.md`
也已经明确写过：

> `cj_gcread_generic` 暂不并入“安全读屏障移除”，因为 `dst` 在 heap 时会转成 `WriteStruct`，不再是纯读。

### 2.8 `CJGenericIntrinsicOpt` 已经在处理一部分 generic copy 薄化

`llvm/lib/Transforms/Scalar/CJGenericIntrinsicOpt.cpp` 已经有多处 `cj_gcread_generic` 专门逻辑：

1. 可以把部分 `cj_gcread_generic` / `cj_assign_generic` / `cj_gcwrite_generic` 链改写成更薄的形式
2. 其中一类直接会把 `cj_gcread_generic` 降成 `memcpy(dst + 8, src, size)`
3. 现有测试已经有：
   - `llvm/test/Transforms/CJGenericIntrinsicOpt/copy_generic_opt_non_phi.ll`
   - `llvm/test/Transforms/CJGenericIntrinsicOpt/copy_generic_opt_phi.ll`

这说明后续优化不应该只盯着“单个 helper 怎么更快”，还应该继续利用已有 generic opt pass，减少 helper 本身的出现次数。

但也要注意：

1. 现有 `CJGenericIntrinsicOpt` 更偏向“stack-to-stack / no-ref typeinfo / 已知等价 copy 链”这类薄化
2. 它目前**还没有**专门理解“fresh heap generic box + `cj_gcread_generic`”这一子问题
3. 因此 Step28 不是推翻它，而是要在现有 `MemorySSA + lowerToMemcpy()` 基础上再补一层针对 fresh-dst 的 MSGC 定向识别

### 2.9 当前 `write_struct` fastpath 已经比 `read_struct` 更适合承接剩余 generic 路径

还需要补一条容易忽略的事实：

1. `CJ_MCC_ReadStructField()` 在 runtime debug 里明确假设 `dstPtr` **不在 heap**
2. `MCC_WriteStructField()` 则天然支持：
   - `obj` 不在 heap 时直接 `memcpy`
   - `obj` 在 heap 时走 `WriteStruct`
3. AArch64 的 `CJ_MCC_WriteStructFieldFast_MSGC` callsite fast emission 也已经具备：
   - `obj` 不在 heap -> `StoreOnly` -> 直接 `memcpy`
   - `obj` 在 heap -> old-ref 枚举 + `memcpy`

因此，剩余 `cj_gcread_generic` 若要在 MSGC 下复用现有 fastpath：

> **最自然的承接点是 `WriteStructFieldFast_MSGC`，不是 `ReadStructField`。**

---

## 3. 核心结论

基于上面的代码事实，Step28 的核心结论有 7 条：

1. **`cj_gcread_generic` 不是普通读屏障。**
   它本质是“generic payload copy”，其中一部分场景会退化成 heap 写。

2. **MSGC 读侧本身已经很薄。**
   真正剩下的成本，一部分是 runtime helper call 本身，另一部分是 heap-dst 时的 `WriteStruct` 语义。

3. **优化的关键不是“统一删掉 helper”，而是按 `dst` 分类。**
   `dst` 是否 fresh、最终是否承载 old ref，决定了能不能直接 `memcpy`。

4. **最值得先做的不是 runtime-only 判断，而是 compiler-side 的 fresh-dst 薄化。**
   这能直接减少 helper 数量，而且不需要把 MSGC collector 细节暴露给 runtime 通用入口。

5. **Phase 1 真正该识别的是 lower 之后的 fresh provenance，而不是原始 `cj_alloca_generic` intrinsic。**
   `CJGenericIntrinsicOpt` 跑的时候，很多 callsite 已经变成 alloca 或 `CJ_MCC_NewObject + !new_gen` 了。

6. **剩余必须保留 heap 写语义的场景，应优先复用 `CJ_MCC_WriteStructFieldFast_MSGC`。**
   不建议再单独发明一套 `ReadGeneric` 专用 AArch64 发射协议。

7. **不建议再走“在 runtime helper 入口读取 `TypeInfo` / `HasRefField()` 决定是否 memcpy”的路线。**
   Step7 已经做过这一类 runtime-only 试验，case1 上是明显回退，不值得继续沿同一切口投入。

---

## 4. 推荐方案

推荐把 `cj_gcread_generic` 明确拆成 3 个子集来处理，而不是继续把它当成一个统一 helper。

### 4.1 先按语义把 `cj_gcread_generic` 分成 3 类

#### A 类：`dst` 明确不需要 old-edge 保护

这类包括：

1. `dst` 最终是非 heap
2. `dst` 虽然可能最终在 heap，但当前能静态证明它是 fresh generic 容器，payload 还未承载任何旧 ref

这类路径的共同点是：

> **可以直接把 `fieldPtr -> dst + TYPEINFO_PTR_SIZE` 做成一次 plain memcpy。**

#### B 类：`dst` 在 heap，且不能证明 fresh

这类路径不能直接删 helper，因为覆盖写之前必须保留旧 ref。

它们应该复用的语义不是 `ReadStruct`，而是：

> **`WriteStruct(obj, dstPayload, size, src, size)` 这一套 old-edge + memcpy 语义。**

#### C 类：当前仍然难以证明的复杂 generic copy 链

比如：

1. `dst` 来源不够直接
2. 中间有 `phi`
3. `dst` payload 在 `gcread_generic` 前已经发生过可能的 clobber
4. size / ptr 关系过于复杂

这类先保守回退，不强行 thin。

### 4.2 Phase 1：先做 compiler-side 的 fresh-dst 直接 `memcpy`

这是最推荐的第一步，原因有 4 个：

1. 命中率高，当前多数 callsite 的 `dst` 最初都来自 `cj_alloca_generic`
2. 语义清楚，抓住的是“fresh payload”而不是 runtime 时刻去猜测内存内容
3. 它可以自然拆成“先 stack、后 heap”的两小步推进
4. 实现位置可以复用现有 `CJGenericIntrinsicOpt` 的 generic copy 识别逻辑

#### 4.2.0 当前仓实现不额外引入 MSGC 开关

这里直接采用当前仓的现实前提：

> 整个 `llvm-project` 分支都按 MSGC 服务来处理。

因此 Phase 1 不再额外设计新的 MSGC 编译期开关或 module flag。

但这不改变另一个要求：

> 仍然要按 **fresh provenance** 做严格识别，不能把“MSGC-only”偷换成“可以随便 thin”。

#### 4.2.1 推荐识别条件

建议第一版只吃最稳的子集：

1. `dst` 直接或去 cast 后，来自 `CJRuntimeLowering` 之后仍可证明 fresh 的来源：
   - `replaceWithAlloca()` 生成的栈 generic box
   - 或 runtime fresh allocation call（带 `new_gen` / `NoAlias` 语义）
2. `dst` 的 payload 区在当前 `cj_gcread_generic` 之前没有发生过：
   - `cj_assign_generic`
   - `cj_gcwrite_generic`
   - `memcpy`/`memmove`
   - 未知 call clobber
3. `size` 与这次 copy 的 payload 大小保持一致，或至少是同一 `TypeInfo` 推导出来的 size

这里要强调：

> 依赖的是 SSA/provenance 上的“fresh”，而不是 runtime 里去检查“dst 当前是不是全零”。

前者是编译期可证明的语义；后者是运行期猜测，之前在 struct write 上已经证明风险很高。

#### 4.2.1.1 推荐按 1a / 1b 两步首发

为降低第一版风险，建议再拆成两个小步：

1. **Phase 1a：先吃 stack-lowered fresh dst**
   - 命中 `replaceWithAlloca()` 产物
   - 这是最稳的首发子集
2. **Phase 1b：再放开 heap-lowered fresh dst**
   - 要求 fresh object provenance 仍然清晰
   - 例如仍能追到 `new_gen` runtime alloc 且 payload 无 clobber
   - 这一步才开始覆盖“fresh heap object 也直接 memcpy”

#### 4.2.2 推荐改写形态

把：

```llvm
call void @llvm.cj.gcread.generic(dst, base, srcPtr, size)
```

改成：

```llvm
%dst.payload = getelementptr i8, ptr %dst, i32 8
call void @llvm.memcpy(..., %dst.payload, srcPtr, size, ...)
```

也就是说：

1. 保留 generic box 自身的 `TypeInfo*`
2. 只把 payload 部分拷进去
3. 完全跳过 `CJ_MCC_ReadGeneric`

#### 4.2.3 推荐落点

第一优先级建议放在：

- `llvm/lib/Transforms/Scalar/CJGenericIntrinsicOpt.cpp`

原因：

1. 这个 pass 已经理解 `cj_gcread_generic` / `cj_assign_generic` / `cj_gcwrite_generic`
2. 它已经具备把部分 generic helper 链薄化为 `memcpy` 的现有基础
3. 这里做 dataflow / use-def / clobber 识别，比在 `CJBarrierLowering` 晚期临时补分析更自然
4. 它运行时机正好在 `CJRuntimeLowering` 之后，因此可以直接吃 lower 后的 fresh provenance

#### 4.2.4 实现时不要再按“匹配原始 `cj_alloca_generic`”去写

这点单独强调一次：

1. `CJGenericIntrinsicOpt` 看到的 IR，很多时候已没有原始 `cj_alloca_generic`
2. 所以实现应优先匹配：
   - alloca-lowered generic box 形态
   - 或 fresh runtime alloc call + `new_gen` 语义
3. 若未来 `CJRuntimeLowering` 继续演进，这层识别也应继续以“fresh provenance”而非单一 intrinsic 名字为准

### 4.3 Phase 2：剩余 heap-dst 路径复用 `gcwrite_struct` fastpath

Phase 1 做完以后，剩下的 `cj_gcread_generic` 基本就是“不能证明 fresh”的那部分。

这部分在 MSGC 下不应该新发明一套“ReadGeneric 专用发射协议”，而应该：

> **直接复用已经落地的 `CJ_MCC_WriteStructFieldFast_MSGC` 那套 struct-copy fastpath。**

#### 4.3.1 语义上为什么能复用

对于 heap-dst 的 `cj_gcread_generic`，runtime 当前本来就是：

```text
dstPayload = dstObj + TYPEINFO_PTR_SIZE
WriteStruct(dstObj, dstPayload, size, fieldPtr, size)
```

也就是说，它和 `gcwrite_struct` 的差别主要只有两个：

1. 目标地址固定是 `dstObj` payload 起点
2. 源地址来自 `fieldPtr`

old-edge 保护语义完全一致。

另外还有一条实现层面的现实优势：

1. `CJ_MCC_ReadStructField()` 调试语义假设 `dstPtr` 不在 heap
2. `MCC_WriteStructField()` 的 slow body 则天然兼容：
   - `obj` 不在 heap -> 直接 `memcpy`
   - `obj` 在 heap -> `WriteStruct`
3. AArch64 `WriteStructFieldFast_MSGC` 的 callsite fast emission也天然兼容这两类分流

所以“把剩余 `gcread_generic` 统一改写到 `WriteStructFieldFast_MSGC`”比“改成 `ReadStructField` 再补特殊分支”更顺。

#### 4.3.2 推荐实现方式

推荐在 MSGC 路径下把剩余的 `cj_gcread_generic` 重写成等价的 struct-write 形态，优先顺序如下：

1. **优先方案**：在 `CJBarrierLowering.cpp` 里直接改写成现有 `CJ_MCC_WriteStructFieldFast_MSGC` 可识别的 wrapper 调用形态
2. **不建议首发**：新增一个极薄的 `CJ_MCC_ReadGenericHeapDstFast_MSGC` wrapper，再让 AArch64 去复用 `WriteStructFast_MSGC` 展开逻辑

无论哪种方式，原则都一样：

- 不重新设计一套独立的 old-ref 枚举 ABI
- 不让后端再理解第二份 generic 专用 barrier 协议
- 直接复用当前 struct-copy fastpath 对 `obj->TypeInfo->GCTib` 的既有 ABI 假设

也就是：

> 如果需要用 `obj->TypeInfo->GCTib` 去枚举目标 payload 里的 ref bitmap，那就沿用现在 `gcwrite_struct` / `array_copy_struct` 已经在走的那套后端/runtime 对象模型 ABI。

而且这里还有一个可以简化实现的点：

> **Phase 2 不一定非要先静态证明 `dst` 在 heap。**

原因是：

1. 若 `dstObj` 不在 heap，`MCC_WriteStructField()` slow body 本来就直接 `memcpy`
2. 若 AArch64 fast emission 发现 `obj` 不在 heap，也会走 `StoreOnly` -> `memcpy`
3. 若 `dstObj` 在 heap，才进入 old-edge 枚举路径

所以更实用的做法是：

> **把“Phase 1 没吃掉的 residual `cj_gcread_generic`”统一 canonicalize 成 `WriteStructFieldFast_MSGC` 形态。**

这样：

1. 不需要在 Phase 2 再做一份“dst 是否 heap”的静态分类
2. 运行时 / 发射层自然完成最终分流
3. `CJ_MCC_ReadGeneric` 的残留量会更低

#### 4.3.3 为什么这比单独做 `ReadGeneric` 发射更好

因为单独做 `ReadGeneric` 发射，会重复实现下面这些东西：

1. 如何从 `obj` 取 `TypeInfo`
2. 如何从 `TypeInfo` 取 `GCTib`
3. 如何按 bitmap 枚举旧 ref
4. ABI 条件不满足时如何 slow fallback

而这些事情，`WriteStructFieldFast_MSGC` 的 MSGC fastpath 已经在做。

更新（2026-07-04）：AArch64 AsmPrinter 之外，`CJBarrierLowering` 源码侧现已把 `cj_array_copy_ref` / `cj_array_copy_struct` 一并补入 MSGC write-barrier lowering allowlist；后续是否真正体现在实际用例的最终 codegen 中，仍需以重编后的 IR/汇编验证。

因此 Step28 的即时落地策略应该是：

> **先锚定 `WriteStructFieldFast_MSGC` 这条已经完整打通的主路径；`ArrayCopyStructFast_MSGC` 可作为后续扩展，不作为本步骤前提。**

### 4.4 Phase 3：继续减少 `cj_gcread_generic` 本身的出现次数

即使 Phase 1 + Phase 2 都做完，generic helper 的动态数量仍可能偏高。

因此第三阶段应继续扩展 `CJGenericIntrinsicOpt`，重点关注 3 类链路：

1. `cj_alloca_generic -> cj_gcread_generic -> cj_assign_generic`
2. `cj_alloca_generic -> cj_gcread_generic -> cj_gcwrite_generic`
3. enum / container 路径里“先取 payload，再立刻搬运”的临时 generic box

很多时候，减少一次 helper 调用，比把单次 helper 再优化 5% 更有价值。

### 4.5 明确不推荐的方向

Step28 明确不推荐 3 条路线：

1. 在 `Barrier::ReadGeneric()` 入口再加一层 `GetTypeInfo()/HasRefField()` 判断
2. 把 `cj_gcread_generic` 直接并入当前 MSGC safe read list，一刀切改成 `memcpy`
3. 重新引入任何 runtime “zero-dst 看起来像 fresh” 的猜测式 fastpath

---

## 5. 实施顺序

推荐按下面顺序推进：

1. **先做 Phase 1a：stack-lowered fresh-dst `memcpy`**
   - 文件优先改 `CJGenericIntrinsicOpt.cpp`
   - 先吃最稳的 lowered alloca / fresh stack generic box
   - 目标是先显著减少 `CJ_MCC_ReadGeneric` 调用次数

2. **再做 Phase 1a 的 LLVM 测试**
   - 补 positive case：fresh stack generic box 被改成 `memcpy`
   - 补 negative case：`dst` 在前面被写过，不允许薄化
   - 复用/扩展现有 `copy_generic_opt_non_phi.ll`、`copy_generic_opt_phi.ll`

3. **再做 Phase 1b：heap-lowered fresh-dst `memcpy`**
   - 继续改 `CJGenericIntrinsicOpt.cpp`
   - 识别 `new_gen` / fresh runtime alloc provenance
   - 只在 provenance 足够硬时放开

4. **再做 Phase 2：residual `cj_gcread_generic` 统一改写到 `WriteStructFieldFast_MSGC`**
   - 主要改 `CJBarrierLowering.cpp`
   - 首选不新增新 wrapper
   - 直接复用现有 `CJ_MCC_WriteStructFieldFast_MSGC` + AArch64 发射逻辑

5. **最后做 Phase 2 的 codegen/asm 验证**
   - 证明 MSGC 下 residual `cj_gcread_generic` 不再统一落到 `CJ_MCC_ReadGeneric`
   - 而是进入 `WriteStructFieldFast_MSGC` fast emission 或 slow fallback

6. **Phase 3 放在最后**
   - 先看前两阶段收益够不够
   - 不够再继续压 generic helper 数量

---

## 6. 风险与回退

### 6.1 最大风险：误判 fresh，漏掉 old-edge 保护

这是这件事里唯一不能犯错的点。

因此第一版必须遵守两个约束：

1. 只相信 lower 后的 fresh provenance 与 dominator/clobber 事实
2. 不相信“当前内存看起来是零”这种运行期现象

只要条件稍微不够硬，就直接不 thin，走原 helper。

### 6.2 第二个风险：把“generic read”错误当成“纯读”

如果有人未来直接把 `cj_gcread_generic` 塞回 `shouldLowerMSGCReadBarrier()` 的 safe list：

1. 非 fresh heap-dst 场景会绕过 `WriteStruct`
2. old ref 会在覆盖写时丢失保留语义

所以 Step28 文档要把这条边界写死：

> `cj_gcread_generic` 只有其“可证明无 old-edge”的子集能直接薄化；它整体不能按纯读屏障对待。

### 6.3 ABI 风险

如果 Phase 2 复用 `gcwrite_struct` fastpath，就必须继续满足现有后端/runtime ABI 约束，例如：

1. 对象 payload 起点仍然是 `TYPEINFO_PTR_SIZE`
2. `obj->TypeInfo->GCTib` 的读取方式不变
3. 目标布局必须和当前 struct-copy fast emission 已支持的对象模型一致

一旦这些条件不满足，就直接 fallback 到原 helper / slow wrapper。

额外再补一条：

4. 不要把 residual `cj_gcread_generic` 改写成 `CJ_MCC_ReadStructField`
   - 它的 debug 语义假设 `dstPtr` 不在 heap
   - 不是承接 heap/non-heap 混合 generic dst 的最佳目标

### 6.4 回退策略

这项工作天然适合分阶段回退：

1. 如果 Phase 1 收益好、Phase 2 风险高，可以只保留 Phase 1
2. 如果 Phase 2 某些 ABI 子集不稳，可以只对满足条件的对象开启
3. 任一阶段都应保持 CopyGC 与非 MSGC 默认行为不变

---

## 7. 验证口径

### 7.1 IR / codegen 验证

至少验证 4 类 case：

1. fresh stack generic box + `cj_gcread_generic` 被直接改成 `memcpy`
2. fresh heap generic box + `cj_gcread_generic` 在 provenance 足够硬时被直接改成 `memcpy`
3. `dst` 发生过先写，不能误改
4. enum associated value 的 non-ref 提取仍然正确
5. boxing / generic copy 路径仍然正确

### 7.2 动态调用次数验证

要验证的不只是功能，还要看 helper 数量是否真的降下去。

建议新增或复用计数口径，至少看：

1. `CJ_MCC_ReadGeneric` 的动态调用次数
2. 进入 `CJ_MCC_WriteStructFieldFast_MSGC` 的次数
3. slow fallback 的次数

Phase 1 成功后，理论上应该先看到：

> `CJ_MCC_ReadGeneric` 的动态调用数明显下降。

### 7.3 benchmark 验证

优先看 generic copy 压力明显的 case，例如：

1. case27 / case28 这类对象构造 + 序列化/搬运场景
2. 之前 Step7 已经暴露 generic helper 热点的 baseline case

关注指标：

1. 吞吐量
2. `CJ_MCC_ReadGeneric` / struct-write fallback 次数
3. GC 总时间
4. RSS 是否异常增加

### 7.4 正确性验证重点

重点不是“程序还能跑”，而是下面两件事：

1. fresh-dst 直拷是否只命中了真正 fresh 的子集
2. 非 fresh heap-dst 是否仍然保留了 old-edge 保护

只要这两件事守住，`cj_gcread_generic` 的 fastpath 才算真正可落地。

---

## 8. case28 实测数据（2026-06-22）

### 8.1 度量方法

在 `runtime/src/Heap/Barrier/Barrier.cpp` 的 `Barrier::ReadGeneric()` 三分支各加一个原子计数器，进程退出（`atexit`）时把三个分支的动态调用次数打到 stderr（`[READGENERIC_STATS]` 前缀）。

说明：MSGC 的 `MSIdleBarrier` / `MSMarkBarrier` 不 override `ReadGeneric`，走的就是基类 `Barrier::ReadGeneric`，只是其中的 `ReadStruct` / `WriteStruct` 被 MS barrier override。因此在基类入口打点对 MSGC 完全有效。

- runtime：`--gc-mode marksweep` 自构建，替换进 `self/dev260621` SDK。
- 用例：case28 `benchmark.cj`，MSGC 单跑。
- 参数：`CASE28_PAYLOAD=10K`、`CASE28_CONCURRENCY=8`、`CASE28_DURATION_MS=60000`、`cjHeapGrowth=1`、`cjScavengeGrowth=10`。
- 运行结果：`ok=438981, fail=0, throughput=7300 req/s`（干净跑完）。

### 8.2 三分支动态计数结果

| 分支 | 语义 | 次数 | 占比 |
| --- | --- | --- | --- |
| ① `!heap(dst) && !heap(src)` | 纯 memcpy | 0 | 0% |
| ② `!heap(dst) && heap(src)` | ReadStruct | 0 | 0% |
| ③ `heap(dst)` | **WriteStruct（带 old-edge 保护）** | 112,821,002 | **100%** |
| | 总计 | 112,821,002 | |

平均约 **每个请求 ~257 次 `cj_gcread_generic` 调用**，是明确热点。

### 8.3 对本设计阶段优先级的修正

**case28 上 100% 的 `cj_gcread_generic` 落在 heap-dst 的 WriteStruct 分支，纯 memcpy / ReadStruct 分支一次都没命中。** 这比 §2.5 原先假设的"dst 非 heap 时基本只是 helper+memcpy"更极端——case28 根本没有非 heap-dst 流量。

由此对 §4 / §5 的阶段优先级做如下修正：

1. **Phase 2（heap-dst 复用 `gcwrite_struct` inline fastpath）是 case28 上唯一覆盖 100% 流量的方案，应优先评估。** 当前这 1.128 亿次每次都要走一次 runtime call → `EnqueueStructRefsInRange`（遍历 GCTib）+ memcpy；Phase 2 换成 AArch64 内联 GCTib bitmap 枚举 + memcpy，省的就是这条满载路径的 helper-call 开销。

2. **Phase 1（fresh-dst 直接 memcpy）在 case28 上的收益上限，完全取决于这 1.128 亿次 heap-dst 里有多少是可证明 fresh 的 `cj_alloca_generic` box。** runtime 计数看不到"fresh"这一维（fresh 是编译期 SSA 属性）。因此 **Phase 1 值不值得做，必须先用编译期静态计数**（统计 `cj_gcread_generic` 中 dst 去 cast 后来自本函数 fresh `cj_alloca_generic` 且无中途 clobber 的 callsite 占比）来定。文档原先把 Phase 1 排在前面的理由（"命中率高"）在 case28 不成立。

3. 非 heap-dst 那条 Phase 1 早被现有 `CJGenericIntrinsicOpt`（无 ref 字段 → memcpy）覆盖的路径，在 case28 占比为 0，无增量收益。

### 8.4 现场与产物

- 插桩改动：`Barrier.cpp` 三分支原子计数 + `atexit` 打印。
- 产物目录：`MSGC_RESULT/Result/case28_http_complex_json/result/readgeneric_20260622_113547/`（`stdout.txt` / `stderr.txt` / `gc.log`）。
- 插桩 dylib 已替换进 `self/dev260621`；原 dylib 备份为 `libcangjie-runtime.dylib.bak-before-readgeneric`。正式对比前需 restore 回非插桩版本，避免 stderr 计数行污染后续 run。
