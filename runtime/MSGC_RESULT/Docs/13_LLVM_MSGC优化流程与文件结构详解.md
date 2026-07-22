# LLVM MSGC 优化流程与文件结构详解

> 本文以当前 `cangjie_compiler` 与其 `third_party/llvm-project` 工作区源码为准，整理 MSGC 在 LLVM 中的优化、编译流程、CopyGC/MSGC 隔离方式、AArch64/X86 后端文件结构和当前能力边界。

## 1. 文档范围与核心结论

### 1.1 分析口径

本文整理的是**当前工作区实际存在的实现**，不是把历次 Step 文档中的候选方案全部视为已落地。源码范围包括：

- 前端：`cangjie_compiler/include/cangjie/Option`、`src/Option`、`src/CodeGen`；
- LLVM 公共层：`llvm/include/llvm/IR`、`llvm/include/llvm/CodeGen`、`llvm/lib/IR`；
- LLVM Pass：`CJRuntimeLowering`、`CJGenericIntrinsicOpt`、`CJBarrierOpt`、`CJBarrierLowering` 等；
- LLVM 后端：公共 `AsmPrinter`、AArch64 和 X86 目标目录；
- Runtime 只用于核对 wrapper、全局符号、TLS/Region/WorkBuf 布局等 ABI 边界。

旧设计文档、benchmark 和历史提交用于解释“为什么这样做”，但当前行为一律以源码、现有测试和 Step46 的最新实施记录为准。尤其是 Step30 中的 symbol-presence 模式推断、fresh `gcread_generic` 折叠等内容已经过时，不能继续当成现状。

### 1.2 当前实现的核心结论

1. **没有新增一套 MSGC intrinsic。** 前端在 CopyGC/MSGC 下继续生成同一套 `llvm.cj.*` intrinsic。
2. **GC 模式由 Module flag 明确携带。** `Cangjie_GCMode=0` 表示 CopyGC，`Cangjie_GCMode=1` 表示 MSGC；`cjc` 通过 `--gc-mode=copygc|marksweep` 选择，默认 CopyGC。
3. **真正的分流点是 lowering 到 wrapper/属性。** CopyGC 与 MSGC 不靠 intrinsic 名字区分，而由 `CJRuntimeLowering`、`CJBarrierLowering` 读取 GCMode 后生成互斥的 wrapper 或函数属性。
4. **MSGC 的主要 LLVM 优化有三组：** 消除不需要的读屏障、把写屏障落到可内联的专用 wrapper、把小对象/数组分配落到 size-class fast path。
5. **MSGC 读屏障消除不是简单的“所有 read 变 memcpy”。** ref/weak/static-ref 变 load，普通 struct 变 memcpy；`gcread_generic` 因目标可能在堆上，需要按 struct 写语义保护被覆盖旧边。
6. **MSGC 机器码能力在 target 最终发射层决定。** AArch64 支持 Mach-O，并已开放 ELF allocation 与 `+lse` write barrier；X86-64 Mach-O/ELF 支持 phase-gated idle write path；COFF/X86-32 保留可执行 wrapper slow call。
7. **CopyGC 与 MSGC 发射已经物理分文件。** AArch64 分为 `AArch64CopyGCEmitter.cpp` 与 `AArch64MSGCEmitter.cpp`；X86 分为 `X86CopyGCEmitter.cpp` 与 `X86MSGCEmitter.cpp`。
8. **AArch64 的“是否发射”和“调用最大尺寸”使用同一决策函数。** `classifyAArch64CJEmission()` 同时服务 AsmPrinter 和 `AArch64InstrInfo`，避免分支松弛/stackmap 尺寸与实际发码不一致。
9. **fresh/栈目标 `gcread_generic -> memcpy`、fresh-init barrier 消除、单引用 struct 标量化已经移除。** 它们不属于当前 MSGC 优化流程，也不应被重新描述为现有收益。
10. **当前源码级隔离和本地验证已完成，双 SDK 端到端验证属于后续交付阶段。** 本文描述实现能力，不把“源码通过”扩大成“完整 SDK 已验收”。

### 1.3 一句话架构

```text
公共 llvm.cj.* intrinsic
        + Cangjie_GCMode Module flag
        ↓
LLVM Pass 做共享优化，并在 lowering 阶段选择 CopyGC/MSGC wrapper 或属性
        ↓
目标后端只消费已固化的 ABI 契约；支持则内联，不支持则保留 slow call
```

## 2. 总体设计：公共 intrinsic，按 GCMode 分流

### 2.1 三层身份必须分开

当前设计把三个概念分成不同层次：

| 层次 | 表示方式 | 回答的问题 |
| --- | --- | --- |
| 语言/IR 语义 | 公共 `llvm.cj.gcread_*`、`gcwrite_*`、`malloc_*` | 这是读、写、分配还是 generic 操作？ |
| Module 模式 | `Cangjie_GCMode` Module flag | 这个 module 按哪一种 GC 语义编译？ |
| lowering/后端 ABI | CopyGC/MSGC wrapper 名称和函数属性 | 后端应展开哪一种 fast path，还是保留 slow call？ |

因此，“公共 intrinsic 向 wrapper 切换时区分”就是当前已经采用的方案。公共 intrinsic 不包含 collector 私有布局；GCMode 负责选择语义；wrapper/属性负责把选择稳定地传到 statepoint 之后和机器码发射阶段。

### 2.2 为什么不新增 MSGC intrinsic

如果复制一套 `llvm.cj.msgc.*` intrinsic，需要同步修改 Intrinsics 定义、前端生成点、Verifier、每个优化 Pass、statepoint、AArch64/X86 lowering 和所有测试。更重要的是，读/写/分配的语言级语义没有变化，变化的是 collector 的实现方式。

保留公共 intrinsic 有以下效果：

- 前端只生成一种稳定 IR，CopyGC/MSGC 的差异不扩散到所有 CodeGen 调用点；
- `CJBarrierSplit`、`CJGenericIntrinsicOpt`、escape analysis 等共享 Pass 可以继续处理公共语义；
- collector 专用知识集中在 GCMode-aware lowering 和后端 emitter；
- LTO 可以直接校验 module 模式，而不是从散落的 intrinsic 名字猜测模式；
- target emitter 未覆盖的架构、object format 或 CPU feature 组合仍可依靠 runtime slow helper 保证正确性。

### 2.3 wrapper 的准确定位

wrapper 不是新的 intrinsic，也不是只为改名而存在。它承担四项职责：

1. **固化模式选择。** 名字或属性明确表示 CopyGC、MSGC static、MSGC dynamic 或 MSGC write barrier。
2. **跨过 statepoint 边界。** 对象分配只有在 `CJRewriteStatepoint` 之后才能安全把实际 callee 换成 fast wrapper。
3. **给后端稳定识别点。** AsmPrinter 不重新分析高层 intrinsic，只识别经过校验的 wrapper/属性。
4. **提供可执行 slow body。** 目标不支持内联、开关关闭或 fast 条件失败时，wrapper 仍会调用公共 runtime helper。

当前 MSGC 写屏障 wrapper 包括：

```text
CJ_MCC_WriteRefFieldFast_MSGC
CJ_MCC_WriteStaticRefFast_MSGC
CJ_MCC_WriteStructFieldFast_MSGC
CJ_MCC_WriteStaticStructFast_MSGC
CJ_MCC_ArrayCopyRefFast_MSGC
CJ_MCC_ArrayCopyStructFast_MSGC
```

分配选择则主要通过属性表达：

```text
cj-copygc-fast-alloc              # CopyGC fast allocation
cj-msgc-sizeclass + cj-msgc-slotsize  # MSGC 静态 size-class
cj-msgc-dynamic                   # MSGC 动态尺寸
```

这些名字、属性、36 档 size class 和 16KB 编译器阈值集中在 `llvm/include/llvm/CodeGen/CJMSGCABI.h`，避免 lowering、发射和指令尺寸估算分别维护副本。

### 2.4 模式分流的硬规则

- CopyGC Module 不能出现 MSGC wrapper/属性；MSGC Module 不能出现 CopyGC wrapper/属性。
- 关闭某个 MSGC fast-path 开关，只能回到 MSGC slow helper，不能退回 CopyGC emitter。
- 缺少 Module flag 的旧 CJBC 只按历史 CopyGC 兼容，不能用 `_MSGC_dynamic` 等符号存在性猜测成 MSGC。
- 目标 hook 只有在确实发射完成时返回 `true`；返回 `false` 时公共 AsmPrinter 必须继续发普通 call。

## 3. 从 cjc 到机器码的完整优化流程

### 3.1 端到端流程图

```text
cjc 解析 --gc-mode
  │
  ├─ 前端生成公共 llvm.cj.* intrinsic
  └─ CGModule 写 Cangjie_GCMode = 0/1
        │
        ▼
Cangjie Module 前置 Pass
  CJIRVerifier
  → InsertCJTBAA（O2/O3）
  → CJFillMetadata
  → CJRuntimeLowering
  → CJBarrierSplit（按开关）
  → CJGCInstrReplace（O2、按开关）
  → CJSimpleOpt（O2/O3）
        │
        ▼
LLVM 标准优化管线及其中的 Cangjie 扩展
  内联 / SROA / EarlyCSE / InstCombine / GVN ...
  → CJPartialEscapeAnalysis（未关闭时）
  → CJGenericIntrinsicOpt（O2、按开关）
  → CJSimpleRangeAnalysis（O2）
        │
        ▼
Cangjie 后置 Pass
  CJGCInstrRestore（与 Replace 配对）
  → CJSpecificOpt
  → PlaceSafepoints
  → CJBarrierOpt
  → CJRewriteStatepoint
        │
        ▼
CodeGen 前
  CJBarrierLowering
  ├─ 读取 GCMode，做最终 barrier 分流
  └─ statepoint 后把对象分配 callee 换成 CopyGC/MSGC wrapper
        │
        ▼
Instruction Selection / MachineInstr
        │
        ▼
AsmPrinter
  ├─ 校验 Module 与函数 wrapper/属性一致
  ├─ AArch64：统一 decision → CopyGC/MSGC emitter 或普通 call
  └─ X86：CopyGC/MSGC emitter；COFF/X86-32 等未支持组合保留普通 call
```

该顺序的重要边界是：**共享 IR 优化在前，safepoint/statepoint 改写在中间，GC 专用机器码选择在后。** 后端不重新推断前端意图，只消费 Module flag 与 wrapper/属性组成的契约。

### 3.2 前端阶段

`cjc` 的 `GlobalOptions::GCMode` 有 `COPY_GC` 和 `MARK_SWEEP` 两个值。`CGModule` 创建 LLVM Module 时同时写入：

```llvm
!llvm.module.flags = !{..., !N}
!N = !{i32 1, !"Cangjie_GCMode", i32 0} ; CopyGC
; 或
!N = !{i32 1, !"Cangjie_GCMode", i32 1} ; MSGC
```

其中 `i32 1` 是 `Module::Error` behavior。GCMode 也进入 `GlobalOptions::ToSerialized()`，避免增量编译或配置缓存跨模式复用。前端 GC read/write builder 仍调用公共 intrinsic；struct 写补充 `AggType` metadata，供 LLVM 获取静态聚合类型信息。

### 3.3 IR 优化和 Runtime lowering 阶段

`CJRuntimeLowering` 负责把分配等公共 intrinsic 改成 runtime ABI，并且已经在构造时缓存 Module GCMode。此时处理数组分配最合适，因为元素类型、常量长度、元素大小等高层信息仍然存在。

`CJGenericIntrinsicOpt`、`CJBarrierSplit`、`CJBarrierOpt` 等 Pass 继续处理公共 intrinsic。它们的共享证明不依赖 MSGC；只有 `CJRuntimeLowering` 和后面的 `CJBarrierLowering` 执行 collector 分流。

### 3.4 safepoint/statepoint 与最终 barrier lowering

`PlaceSafepoints` 插入 safepoint，`CJRewriteStatepoint` 把 GC 相关调用改成 statepoint 形式。随后 `TargetPassConfig::addISelPasses()` 在 instruction selection 前运行 `CJBarrierLowering`。

这一步完成两类最终选择：

- barrier：CopyGC 保留 tagged-pointer/phase fast path；MSGC 消除读屏障或生成 MSGC 写 wrapper；
- object allocation：在 statepoint 上把 `CJ_MCC_NewObject`/`NewFinalizer` 换成带互斥属性的 CopyGC/MSGC wrapper。

因此不能只在 `CJRuntimeLowering` 里完成所有对象分配分流：对象调用经过 statepoint 改写后，最终 callee 必须在 `CJBarrierLowering` 阶段重新固化。

### 3.5 JIT 特殊路径

JIT 在 `TargetPassConfig` 中补跑 `CJFillMetadata`、`CJRuntimeLowering`、`CJSpecificOpt`、`PlaceSafepoints` 和 `CJRewriteStatepoint`，再统一进入 `CJBarrierLowering`。当前分配 fast wrapper 在 `CangjieJIT` 下关闭，避免 JIT 走依赖静态 module 数据或复杂后端 ABI 的路径。

## 4. GCMode、Module flag 与 LTO 隔离

### 4.1 公共判定接口

模式定义集中在：

```text
llvm/include/llvm/IR/CJGCMode.h
llvm/lib/IR/CJGCMode.cpp
```

提供两个层次的接口：

| 接口 | 缺 flag 行为 | 用途 |
| --- | --- | --- |
| `getExplicitCangjieGCMode()` | 返回 `None` | 判断是否为旧 bitcode、执行严格链接校验 |
| `getCangjieGCMode()` | 按 CopyGC 兼容 | 普通 Pass 和后端获取有效模式 |

flag 不是 `i32`、值不是 `0/1` 都会立即 `report_fatal_error`。这比在各 Pass 中各自判断字符串或某个 wrapper 是否存在更可靠。

### 4.2 IR Link 与 LTO 校验

新 Module 使用 `Module::Error` 写 flag，因此显式 CopyGC 和 MSGC module 在普通 IR link 时会因 flag 冲突失败。LTO 还处理一个普通 module flag 合并不容易覆盖的情况：**MSGC 与无 flag 旧 CJBC 混合**。

`llvm/lib/LTO/LTO.cpp` 记录三种输入状态：

```text
SawCangjieMSGC
SawCangjieLegacyOrCopyGC
SawNonCangjieModule
```

- RegularLTO 在原始 Module materialize metadata 后、summary 分叉前调用 `recordCangjieModule()`；
- ThinLTO 在 `checkCJBC()` 中解析每个 `ModuleMap` 输入并记录模式；
- Cangjie LTO 混入非 CJBC 会失败；
- 只要 MSGC 与 CopyGC/legacy 同时出现，就报错：`Cangjie MSGC LTO requires every CJBC input to carry Cangjie_GCMode=1`。

因此合法组合是 `0+0`、`1+1`；`0+1` 和 `1+legacy` 均被拒绝。旧 CJBC 可以单独继续按 CopyGC 使用，但不能被静默混进 MSGC LTO。

### 4.3 lowering 与后端的二次校验

Module flag 并不代替 wrapper/属性校验。`CJMSGCABI.h` 的 `classifyCangjieFastAllocation()` 会检查：

- CopyGC 属性不能和 MSGC static/dynamic 属性同时出现；
- `cj-msgc-sizeclass` 与 `cj-msgc-slotsize` 必须成对；
- static、dynamic 两类 MSGC 属性不能冲突。

`verifyCangjieGCFunctionABI()` 进一步检查 Module 模式与函数 ABI：CopyGC module 出现 `_MSGC` wrapper、MSGC module 出现 CopyGC fast wrapper都会报错。公共 `AsmPrinter::doInitialization()` 会先解析 Module GCMode，再遍历所有函数执行这项校验。

### 4.4 旧 bitcode 兼容边界

无 flag module 的有效模式仍是 CopyGC，但兼容范围是有意收紧的：

- AArch64 只为旧 CopyGC object 和 `NewArrayFast` statepoint 保留历史 fast-path 推断；
- 直接 array wrapper 没有明确属性时不做 legacy 推断；
- X86 也只允许 CopyGC 属性或 legacy CopyGC 路径；
- 任何 `_MSGC_dynamic` 符号存在性启发式都已删除。

这保证旧 CopyGC bitcode 可以继续工作，同时新 MSGC module 必须显式携带 `Cangjie_GCMode=1` 和匹配的 wrapper/属性。

## 5. LLVM Pass 分工与当前优化边界

### 5.1 Pass 分类

| Pass/组件 | 当前职责 | 是否 MSGC 专用 |
| --- | --- | --- |
| `CJIRVerifier` | 校验前端生成的 Cangjie IR | 否 |
| `CJFillMetadata` | 在后续结构优化前补齐 Klass/GC metadata | 否 |
| `CJRuntimeLowering` | lowering runtime intrinsic；按 GCMode 分流数组分配 | 部分 |
| `CJBarrierSplit` | 把少量引用字段的 struct barrier 拆成 ref barrier + 非引用 memcpy | 否 |
| `CJGenericIntrinsicOpt` | 栈到栈、已知无引用类型、MemorySSA copy forwarding | 否 |
| `CJPartialEscapeAnalysis` | 逃逸分析和 heap-to-stack 相关优化 | 否 |
| `CJBarrierOpt` | stack/no-ref barrier 消除和裸写 verifier | 否 |
| `CJRewriteStatepoint` | 固化 GC live reference、callee 和 stackmap 关系 | 否 |
| `CJBarrierLowering` | 按 GCMode 做最终 barrier/对象分配分流 | 是，含两种模式分支 |
| AArch64 MSGC emitter | 生成 MSGC 分配和写屏障机器码 | 是 |

“某个 Pass 对 MSGC workload 有收益”不等于“这个 Pass 只适用于 MSGC”。当前明确只对 MSGC 生效的变换，由 `getCangjieGCMode()` 支配；共享 Pass 的证明仍应同时满足 CopyGC 和 MSGC 语义。

### 5.2 `CJBarrierSplit` 的作用

对 constant-size struct/static-struct 读写，`CJBarrierSplit` 根据静态类型布局寻找引用槽：

- 引用槽生成细粒度 `gcread_ref`/`gcwrite_ref`；
- 引用槽之间的连续非引用字节生成 `memcpy`；
- 当前主要覆盖 1、2、3 个引用字段，更多引用保留 bulk barrier；
- 实现按 8 字节引用槽推进，依赖 64 位数据布局。

它的价值是缩小 barrier 覆盖范围，让后续 optimizer 能分别处理“需要 GC 语义的 ref”与“纯字节区域”。这是公共优化，不应被标成 MSGC-only。

### 5.3 `CJGenericIntrinsicOpt` 的作用

当前保留的通用优化包括：

1. source/destination 都可证明在栈上时，generic copy/read/write 可降为 `memcpy`；
2. TypeInfo 或静态布局证明不含 GC reference 时，可降为 `memcpy`；
3. 利用 MemorySSA、支配关系和别名分析做等价源回溯、copy forwarding 和冗余 generic 操作消除。

这些证明回答的是“复制内容和目标是否需要 GC barrier”，不是“当前是否为 MSGC”。因此 `CJGenericIntrinsicOpt` 仍是 CopyGC/MSGC 公共 Pass。

### 5.4 `CJBarrierOpt` 的作用

当前 `CJBarrierOpt` 兼具优化和 verifier 职责：

- 目标可证明是 `alloca` 时，stack barrier 可变为普通 load/store/memcpy/memmove；
- 静态布局证明复制区间没有 GC reference 时，消除不必要 barrier；
- 检查普通 store/memcpy/memmove 是否可能把 GC reference 裸写到 heap/global，无法证明安全时直接报错。

它同样是公共 Pass。MSGC 的 stable-address 特性不能被用来放松“heap 写必须保护旧边”这一规则。

### 5.5 已移除、不得计入当前优化的修改

当前 LLVM 分支 HEAD 已移除三类实验：

| 已移除修改 | 原目标 | 为什么不再保留 |
| --- | --- | --- |
| fresh heap/仅目标为栈的 `gcread_generic -> memcpy` | 消除 generic read helper | 只能证明目标，没有证明源 payload 的引用语义；恢复统一保守路径 |
| fresh-init barrier 消除 | 新对象首次写直接 store/memcpy | case27 A/B 收益基本一致，且扩大两种 GC 的证明面 |
| 单引用 struct 标量化 | struct barrier 拆为单 ref barrier + memcpy | 独立收益不明显，恢复原 struct barrier 更易维护 |

相应的 `processFreshGcreadGeneric()`、`optFreshInitBarrier()`、`scalarizeOneRefStructBarrier()` 等入口已经不在当前源码中。正确的文档口径是：保留原有 stack/no-ref/MemorySSA 公共优化，但不再利用 fresh provenance 做这三项专项折叠。

## 6. MSGC 读屏障优化

### 6.1 为什么 MSGC 可以消除普通读屏障

CopyGC 可能移动对象，读侧需要处理 tagged pointer、forwarding/version 等状态；MSGC 原地标记和清扫，对象在生命周期内地址稳定，不需要在普通读取时修正引用。因此 `CJBarrierLowering` 在 `CangjieGCMode::MSGC` 且 `EnableMSGCReadBarrierLowering` 打开时直接消除普通读 barrier。

这项优化**只适用于 MSGC 的 collector 语义**。`CJGenericIntrinsicOpt` 的 stack/no-ref memcpy 则是另一类共享证明，二者不能混为一谈。

### 6.2 当前 lowering 矩阵

| 公共 intrinsic | MSGC lowering | 作用 |
| --- | --- | --- |
| `cj_gcread_ref` | 从 field pointer 直接 load | 删除 runtime helper 和 tag/phase 判断 |
| `cj_gcread_weakref` | 直接 load | MSGC 不需要 forwarding 修正 |
| `cj_gcread_static_ref` | 从 static slot 直接 load | 同上 |
| `cj_gcread_struct` | `llvm.memcpy(dst, src, size)` | struct 地址稳定，按字节复制 |
| `cj_gcread_static_struct` | `llvm.memcpy(dst, src, size)` | 同上 |
| `cj_gcread_generic` | 转成 `CJ_MCC_WriteStructField` 语义 | 目标 box 可能在 heap，必须保护被覆盖 old edge |

普通 ref/struct 读从 `intrinsic → runtime helper` 缩短为 load/memcpy，是 MSGC 最直接、覆盖面最广的 LLVM 优化之一。

### 6.3 `gcread_generic` 为什么不能一律 memcpy

`gcread_generic(dstBox, base, srcField, size)` 的 destination 是 box 对象。lowering 实际写入 `dstBox + 8` payload。如果 box 在 GC heap，覆盖其原 payload 时就是一次 heap struct write；Mark 阶段必须保留被删除的旧引用边。

当前变换为：

```text
dstPayload = dstBox + 8
CJ_MCC_WriteStructField(dstBox, dstPayload, size, srcField, size)
```

在 64 位 wrapper ABI 且 MSGC 写 lowering 开启时，callee 可进一步换成 `CJ_MCC_WriteStructFieldFast_MSGC`；否则保留公共 runtime slow helper。是否把 wrapper 展开成机器码，由最终目标 emitter 决定。这个处理同时解释了为什么“destination fresh”或“destination 是 alloca”不能单独作为通用 memcpy 的新规则：它没有覆盖 source/generic 语义的全部证明条件。

### 6.4 与 safepoint 的关系

“没有读屏障”不等于“没有 safepoint”。MSGC 仍需要：

- `PlaceSafepoints` 协调线程进入 GC 安全状态；
- `CJRewriteStatepoint` 记录 live GC references；
- stackmap 支持 root enumeration；
- WeakRef、atomic 和 complex generic 操作仍按各自 runtime 语义处理。

读屏障消除只删除对象访问时不再需要的地址修正成本，不改变 GC 协作和根可见性协议。

## 7. MSGC 写屏障优化

### 7.1 lowering 到专用 wrapper

`CJBarrierLowering::shouldLowerMSGCWriteBarrier()` 同时要求：

```text
GCMode == MSGC
&& EnableMSGCWriteBarrierLowering
&& module 使用 64 位指针 ABI
&& intrinsic 在支持名单中
```

当前映射为：

| intrinsic | wrapper |
| --- | --- |
| `cj_gcwrite_ref` | `CJ_MCC_WriteRefFieldFast_MSGC` |
| `cj_gcwrite_static_ref` | `CJ_MCC_WriteStaticRefFast_MSGC` |
| `cj_gcwrite_struct` | `CJ_MCC_WriteStructFieldFast_MSGC` |
| `cj_gcwrite_static_struct` | `CJ_MCC_WriteStaticStructFast_MSGC` |
| `cj_array_copy_ref` | `CJ_MCC_ArrayCopyRefFast_MSGC` |
| `cj_array_copy_struct` | `CJ_MCC_ArrayCopyStructFast_MSGC` |
| `cj_gcwrite_generic` | 先改写成 struct-write ABI，再使用 struct wrapper |

每个 wrapper 都是 internal、noinline，并包含尾调用 runtime slow helper 的真实函数体。它既是后端识别锚点，也是任何不支持 inline 的情况下的正确性 fallback。

`cj_array_copy_generic` 不在专用名单中；atomic load/store/swap/CAS 也没有 MSGC 专用 emitter，继续走公共 runtime lowering。这里不能把“普通 field/struct 已内联”扩大成“所有 barrier 已内联”。

### 7.2 `gcwrite_generic` 的当前优化

generic write 的 source 是 box，实际 payload 从 `srcBox + 8` 开始。在 64 位 MSGC 专用 lowering 中，当前不再先调用 `CJ_MCC_WriteGeneric` 做动态分派，而是直接表达成：

```text
CJ_MCC_WriteStructField(obj, fieldPtr, size, srcBox + 8, size)
```

当 `srcBox == null` 时，lowering 用 `select` 把 size 变为 0，保持原 helper 的 early-out 语义，并避免额外控制流。64 位目标随后把该调用换成 struct wrapper；支持的 AArch64/X86 emitter 可在最终发射层继续内联，不支持的 object format 或架构执行 wrapper slow body。32 位目标不进入这项专用改写，仍按公共映射调用 `CJ_MCC_WriteGeneric`。

这项优化消除的是 generic helper 的固定分派层，但仍保留 heap write 的 old-edge 语义，不等于无条件 memcpy。

### 7.3 目标 phase gate 与 store-only 路径

AArch64 Mach-O、AArch64 ELF（需要 LSE）和 X86-64 Mach-O/ELF emitter 都会读取 Runtime 导出的：

```text
MapleRuntime::Heap::inlineFastPathGCPhase
```

gate 为 0 时直接进入 store-only：

- ref/static-ref：普通 store；
- struct/static-struct：`memcpy`；
- arraycopy：`memmove`。

当前 Idle 以及不需要 Mark barrier 的阶段共享这条短路径。它不再读取 CopyGC 的 `GetGCPhase`/tagged-pointer fast state；MSGC 与 CopyGC 的 phase 优化在 lowering 时已经互斥。

AArch64 使用 acquire load；X86-64 利用 TSO 下的普通 load 提供相应硬件有序性。gate 非 0 时，AArch64 继续尝试下述 Mark fast path，X86-64 当前回到 wrapper slow body。

### 7.4 ref/static-ref 的 Mark fast path

Mark gate 打开时，ref 写的主要步骤为：

1. 先读取 old ref；`old == null` 直接 store-only；
2. `old == new` 说明没有删除边，也直接 store-only；
3. 非 static 写检查 container 是否落在 `heapReservedStartAddr..heapReservedEndAddr`；非 heap container 无需追踪；
4. 从 X28 TLS 取 `msAllocBuffer`，再取固定偏移处的 `activeLog_` WorkBuf；为空或空间不足走 slow；
5. 根据 old ref 定位 `MSRegionInfoArray` 和 mark bitmap，校验 BUMP/PARTIAL/FULL/LARGE Region 状态；
6. 原子设置 mark bit，只有 mark winner 才考虑进入 log；
7. TypeInfo 证明对象含引用字段时，把 grey object 写入 `activeLog_->objs[len++]`；
8. 最后执行实际 store。

large object 会先解析 span primary Region，再校验对象 base 和 TypeInfo。异常、stale 或布局不确定情况都回 wrapper slow body，不在 inline 路径猜测。

### 7.5 struct/static-struct 的 Mark fast path

普通 struct 写根据 container TypeInfo/GCTib 识别被覆盖区间中的引用槽：

- short-form GCTib 按 8 字节槽扫描；
- 相邻两个引用槽可先做无副作用 descriptor 校验，再合并 mark bitmap word 的原子更新；
- 已标记、null 或无引用字段对象不会写入 active log；
- WorkBuf 容量在 mark 前预留，避免“赢得 mark bit 后却无法 enqueue”；
- array element 遇到 long-form GCTib 时读取 component TypeInfo；无引用 component 可直接 memcpy，含引用或无法证明则 slow；
- static-struct 使用传入 GCTib，复杂/长布局保守回 runtime。

无 old edge、非 heap container、size 为 0、GCTib 区间无引用等情况统一进入 inline memcpy。这个 store-only memcpy 是 barrier 语义分析后的结果，不是 `CJBarrierOpt` fresh-init 消除。

### 7.6 arraycopy 的当前边界

arraycopy wrapper 目前采用明确的两阶段策略：

- gate 为 0：inline `memmove`；
- Mark gate 打开：直接调用 runtime barrier。

旧的全内联 copy loop 曾把 raw old ref 直接追加到 `activeLog_`，没有先赢得 mark-CAS，会破坏“buffer 中只存 grey object”的 Route-A 不变式，因此已经撤销。当前不能描述为 Mark 阶段 arraycopy 已完全 inline。

### 7.7 正确性不变式

机器码优化必须保持以下顺序：

```text
读取 old edge
→ 校验对象/Region/TypeInfo
→ 确保 log 有容量
→ 原子争夺 mark winner
→ winner 且 HasRefField 才 enqueue
→ 执行 store/copy
```

其中“mark winner 后必须保证对象能被后续扫描”是核心不变式。任何 buffer 满、布局不明、Region 状态异常都应在改变 mark bit前回 slow，或由 runtime 完整接管。

### 7.8 优化作用

这组优化主要减少：

- 每次写进入虚 barrier/runtime helper 的调用成本；
- common case 中重复的 generic dispatch；
- Idle/Sweep 阶段不必要的 Mark 逻辑；
- null old、same-value 和非 heap container 的无效工作；
- struct 多引用槽逐个重复的 descriptor/bitmap 操作。

代价是 AArch64 emitter 与 Runtime 的 TLS、Region、bitmap、TypeInfo 和 WorkBuf 布局形成直接 ABI，维护时必须双边同步。

## 8. MSGC 对象与数组分配优化

### 8.1 两个分流时机

分配优化分成两部分：

- **数组**在 `CJRuntimeLowering` 中分流，因为此时仍能看到 array element type、length 和 element size；
- **对象/finalizer**在 `CJBarrierLowering` 中处理 statepoint，确保改写的是最终 actual callee。

两处都读取同一个 Module GCMode，并用 `CJMSGCABI.h` 中的属性传递选择，后端不再从函数名存在性猜模式。

### 8.2 size class 与静态小对象

编译器 ABI 定义 36 档 size class：

```text
8, 16, 24, 32, 48, ... , 12288, 16384
```

常量对象大小能映射到 class 时，`CJBarrierLowering` 生成类似：

```text
CJ_MCC_NewObjectFast_MSGC_cls3_sz32
```

并添加：

```text
cj-msgc-sizeclass="3"
cj-msgc-slotsize="32"
```

后端根据属性而不是解析 wrapper 名字获取 class/slot size。常量大对象超过编译器阈值时保留 `CJ_MCC_NewObject` slow call；finalizer 目前只为静态小对象生成 MSGC fast wrapper。

### 8.3 动态对象

对象大小不是常量时，普通 object 可生成共享 wrapper：

```text
CJ_MCC_NewObjectFast_MSGC_dynamic
cj-msgc-dynamic="1"
```

AArch64 emitter 在运行时执行：

1. safepoint check；
2. size 必须非 0 且不超过 16KB；
3. 用 `ceil(size/8)` 查询 module-local size-class table；
4. 用 class 查询 slot-size table；
5. 从 TLS `msAllocBuffer` 取对应 `activeRegions_[class]`；
6. 在 BUMP 或 PARTIAL Region 分配；
7. 任一条件失败调用 wrapper 内的 `CJ_MCC_NewObject` slow body。

动态表只在 module 实际使用动态 MSGC 分配时由 `emitPendingMSGCModuleData()` 写入只读 section，避免所有 module 无条件携带表数据。

### 8.4 BUMP/PARTIAL inline 分配

静态与动态对象/数组共享同一核心策略：

- 先检查 TLS safepoint state，GC 要求停顿时立即 slow；
- 读取 TLS `msAllocBuffer` 和当前 size class 的 active Region；
- BUMP Region 检查 `allocPtr + slotSize <= regionEnd` 后推进指针；
- PARTIAL Region 从 reserved summary 找非空 bitmap word，在 alloc bitmap 中取一个 free slot并更新 free count/summary；
- 写入 TypeInfo；数组还写入 length；
- 返回对象地址并记录 Cangjie stackmap；
- Region 类型、bitmap 或容量不满足时回 runtime。

当前 allocation emitter 主要依赖 safepoint gate，不读取 CopyGC encoded phase，也不在 fast path 中做历史方案中的 mark-at-alloc。

### 8.5 concrete array

`CJRuntimeLowering::replaceNewArray()` 尝试计算：

```text
alignedSize = align8(ArrayHeaderSize + length * elemSize)
```

- MSGC 常量总大小可映射 class：生成 static MSGC array wrapper；
- MSGC 动态 length、已知 elemSize：生成 `_MSGC_dynamic` wrapper并传动态总大小；
- CopyGC 常量 fast array：在实际 callee 上添加 `cj-copygc-fast-alloc`；
- 不能安全计算或过大：保留 runtime slow path。

### 8.6 generic array

CopyGC 和 MSGC 在这里完全分开：

| 模式 | 当前行为 |
| --- | --- |
| CopyGC | `cj_malloc_array_generic -> CJ_MCC_NewArrayGeneric`，不创建任何 `_MSGC` dispatch/wrapper |
| MSGC，element type 静态可知 | 按 reference/struct/primitive width 选择 `NewObjArray`、`NewArray`、`NewArray8/16/32/64`，常量总大小可继续特化 class |
| MSGC，element type 动态 | 生成 internal `CJ_MCC_NewArrayGenericFast3_MSGC_dispatch` |

动态 dispatch 读取 element TypeInfo/kind/size，分到：

```text
*_dyn_ref
*_dyn_struct
*_dyn_w1
*_dyn_w2
*_dyn_w4
*_dyn_w8
```

无法识别的类型进入 `CJ_MCC_NewArrayGenericFast3_MSGC` fallback。每个动态 wrapper都有可执行 slow body，因此未实现 MSGC inline 的目标也能正确运行。

### 8.7 边界与收益

编译器侧 `getMSGCFastSizeClass()` 接受 `<= 16384`，而当前 Runtime 顶层分配对齐后以 `>= 16384` 进入 large path。精确 16KB 是需要特别维护和验证的边界，不能把两层阈值简单写成完全等价。

分配 fast path 的收益来自把小对象/数组常见路径压缩为 TLS + per-class Region 本地操作，减少 runtime stub、全局池、size-class 计算和 slow allocator 固定税。动态路径比静态路径指令更多，但仍避免了大部分通用分派。

## 9. 目标机器码发射优化

### 9.1 公共 AsmPrinter 的 bool hook 协议

`llvm/lib/CodeGen/AsmPrinter/AsmPrinter.cpp` 负责通用调用识别，但不包含 MSGC 指令序列。它依次尝试：

- GC phase/safepoint/debug/metadata/Cangjie stub 等公共特殊调用；
- object/finalizer fast allocation hook；
- MSGC write-barrier hook；
- 其他 Cangjie target hook。

目标 hook 的返回值含义非常严格：

```text
true  = 已完整发射，公共层可以吞掉原 call
false = 没有发射，必须继续生成普通 call
```

因此 COFF/X86-32 收到 MSGC wrapper、AArch64 ELF 未提供 LSE、或 fast path 开关关闭时，都不会生成空代码，而是保留或回到 wrapper slow call。

### 9.2 统一 emission decision

`AArch64CangjieCallSize.cpp` 的 `classifyAArch64CJEmission()` 返回：

```text
Kind
CanInline
AllocConfig(sizeClass/slotSize)
WriteBarrierKind
MaxSize
```

`AArch64CangjieAsmPrinter.cpp` 用它选择 emitter；`AArch64InstrInfo::getCangjieSpecificCallInstSizeInBytes()` 用同一个结果给分支松弛、stackmap shadow 和指令布局提供保守尺寸。

当前 decision kind 为：

```text
None
CopyGCObject / CopyGCArray
MSGCStaticObject / MSGCDynamicObject
MSGCStaticArray / MSGCDynamicArray
MSGCWriteBarrier
```

这种共享不是单纯重构：如果发射端认为会展开 1KB，而 InstrInfo 仍按普通 call 的 4B/几字节估算，后续 branch relaxation 和 stackmap 位置都可能错误。

### 9.3 当前保守最大尺寸

| 类型 | `MaxSize` |
| --- | ---: |
| CopyGC object | 96 B |
| CopyGC array | 52 B |
| MSGC static/dynamic allocation | 4096 B |
| MSGC ref/static-ref barrier | 512 B |
| MSGC struct/static-struct barrier | 1536 B |
| MSGC arraycopy barrier | 64 B |

这些是布局用的保守上界，不是每个 callsite 的实际发码大小。修改 emitter 时必须同时检查 decision 的上界是否仍覆盖新指令序列。

### 9.4 statepoint 与普通 call 的发射

`AArch64AsmPrinter::LowerSTATEPOINT()` 对 `NewArrayFast` 先调用 array fast hook；失败后继续普通 statepoint call。其他 statepoint callee 通过 `tryEmitCangjieSpecificCallByMOSym()` 尝试 object/barrier 展开，成功后记录 stackmap。

普通 MachineInstr call 在 `emitInstruction()` 中先尝试直接 array fast path，再尝试公共 Cangjie specific call，失败后走标准 MC lowering。对象/数组 emitter 自己包含 slow call label，fast miss 仍保持原 runtime 调用和 stackmap 语义。

### 9.5 平台能力边界

当前最终发射能力如下：

| 目标 | MSGC allocation | MSGC write barrier |
| --- | --- | --- |
| AArch64 Mach-O | static/dynamic inline | inline，保留现有 Apple arm64 行为 |
| AArch64 ELF | static/dynamic inline | `+lse` inline，`-lse` wrapper slow body |
| X86-64 Mach-O/ELF | wrapper slow body | gate=0 时 inline store/memcpy/memmove；gate!=0 时 slow body |
| X86-64 COFF、X86-32 | wrapper slow body | wrapper slow body |

COFF 暂不读取 C++ ABI mangled 的 phase 数据符号；在 Runtime 提供稳定 C ABI phase accessor/data symbol 前，保留 slow call 才能同时兼容 Itanium 和 MSVC ABI。这是**发射能力限制**，不是 IR 语义限制：所有 64 位目标仍生成相同 MSGC wrapper，是否内联只由最终 emitter 判定。

### 9.6 module-local 动态表

动态对象/数组 emitter 首次使用时设置 `MSGCDynamicTablesNeeded`。`AArch64AsmPrinter::doFinalization()` 调用 `emitPendingMSGCModuleData()`，只在需要时输出：

- `lCJMSGCSizeClassTable[ceil(size/8)] -> class or 0xFF`；
- `lCJMSGCSlotSizeTable[class] -> uint16 slot size`。

表放在只读 section，属于当前 module 的代码生成数据，不是 Runtime 导出 ABI。

### 9.7 X86-64 MSGC 写屏障 emitter

`X86MSGCEmitter.cpp` 只负责最终机器码差异：Mach-O/ELF 通过目标符号 mangling 和 GOTPCREL 读取 phase，Idle 路径内联 ref/static-ref store、struct copy 和 array `memmove`；Mark 路径跳回原 wrapper。COFF 与 X86-32 在发射任何指令前返回 false，因此公共 AsmPrinter 完整保留原 call。

X86 fast emitter 还会先校验 calling convention 和 call opcode：只允许默认 C/SysV64 的普通 direct call；vectorcall、regcall、tail call 等形态保留其 ABI-aware wrapper call。X86-64 当前没有 MSGC allocation emitter，也没有把 AArch64 的 Region/bitmap/WorkBuf Mark fast path复制过去；这两类调用仍执行 wrapper slow body。

## 10. CopyGC/MSGC 与 AArch64/X86 文件结构

### 10.1 前端与 LLVM 公共结构

```text
cangjie_compiler/
├── include/cangjie/Option/
│   ├── Option.h                         # GlobalOptions::GCMode
│   └── Options.inc                      # --gc-mode
├── src/Option/
│   ├── OptionAction.cpp                 # copygc/marksweep 解析
│   └── Option.cpp                       # GCMode 序列化
├── src/CodeGen/
│   ├── CGModule.cpp                     # 写 Cangjie_GCMode Module flag
│   └── CJNative/CJNativeIRBuilder.cpp   # 仍生成公共 intrinsic；AggType metadata
└── third_party/llvm-project/llvm/
    ├── include/llvm/IR/CJGCMode.h       # GCMode 公共接口
    ├── lib/IR/CJGCMode.cpp
    ├── include/llvm/CodeGen/CJMSGCABI.h # wrapper/属性/size class/ABI 校验
    ├── lib/LTO/LTO.cpp                  # CJBC 与混合 GCMode 校验
    └── lib/Passes/
        ├── PassBuilder.cpp              # Cangjie 前置/后置 Pass 顺序
        └── PassBuilderPipelines.cpp     # 标准优化管线中的 Cangjie 扩展
```

`CJGCMode` 是公共模式契约，`CJMSGCABI` 是 lowering 与后端共享的 ABI 契约。它们应该保持小而稳定，不应把 AArch64 指令细节放进公共头文件。

### 10.2 IR Pass 与 CodeGen 结构

```text
llvm/lib/
├── Transforms/Scalar/
│   ├── CJRuntimeLowering.cpp       # runtime intrinsic；数组按 GCMode 分流
│   ├── CJGenericIntrinsicOpt.cpp   # generic 公共优化
│   ├── CJBarrierSplit.cpp          # struct barrier 公共拆分
│   ├── CJBarrierOpt.cpp            # stack/no-ref 消除 + verifier
│   ├── CJRewriteStatepoint.cpp     # statepoint 改写
│   └── ...                         # 其他 Cangjie 共享优化
├── Transforms/IPO/
│   └── CJPartialEscapeAnalysis.cpp # 共享 escape analysis
└── CodeGen/
    ├── CJBarrierLowering.cpp       # GCMode-aware barrier/object 分流
    └── AsmPrinter/AsmPrinter.cpp   # 公共 bool hook 与 ABI 初始化校验
```

这里有意没有把 `CJBarrierLowering.cpp` 拆成两个完全独立文件，因为它仍负责共享 intrinsic map、statepoint 收集、safepoint outline 和两种 GC 的最终选择。GC 专用的大段机器码才适合按 emitter 分文件。

### 10.3 AArch64 后端结构

```text
llvm/lib/Target/AArch64/
├── AArch64AsmPrinter.cpp          # 标准 printer、STATEPOINT、普通 MI lifecycle
├── AArch64AsmPrinter.h            # 共享类声明与 emitter 接口
├── AArch64CangjieAsmPrinter.cpp   # Cangjie 通用发射 + 薄分派
├── AArch64CangjieCallSize.h/.cpp  # 唯一 emission/call-size decision
├── AArch64CopyGCEmitter.cpp       # CopyGC object/array fast-path 指令序列
├── AArch64MSGCEmitter.cpp         # MSGC allocation/write barrier/动态表
└── AArch64InstrInfo.cpp           # 调用统一 decision 获取保守尺寸
```

当前拆分原则是：

```text
一级：Target（AArch64 / X86）
二级：GC（CopyGC / MSGC）
```

`AArch64AsmPrinter.cpp` 不再承载 CopyGC/MSGC 大段发码；`AArch64CangjieAsmPrinter.cpp` 只保留共享逻辑和薄 dispatcher；真正的 collector 指令序列分别进入 emitter 文件。

### 10.4 X86 后端结构

```text
llvm/lib/Target/X86/
├── X86AsmPrinter.cpp              # 标准 X86 printer
├── X86CangjieAsmPrinter.cpp       # Cangjie 通用栈检查/TLS/metadata 等
├── X86MCInstLower.cpp             # call/statepoint lowering 与 CopyGC/MSGC 薄分派
├── X86CopyGCEmitter.cpp           # CopyGC object/array 指令序列
├── X86MSGCEmitter.cpp             # MSGC phase-gated write barrier 指令序列
└── X86AsmPrinter.h                # 共享声明
```

X86 与 AArch64 本来就属于不同 target 目录，发射天然隔离。CopyGC 与 MSGC 的大段指令序列分别位于两个 emitter，`X86MCInstLower.cpp` 只识别 callee、lower slow target operand 并转交目标 emitter。

`X86MSGCEmitter.cpp` 当前只实现 X86-64 Mach-O/ELF 的 phase-gated Idle 写路径；COFF、X86-32、Mark 路径和 MSGC allocation 都保留 wrapper slow call。hook 只有在完整发射后才返回 true，不支持组合会在第一条指令前返回 false。

### 10.5 CMake 源文件归属

`AArch64/CMakeLists.txt` 已显式编译：

```text
AArch64CangjieAsmPrinter.cpp
AArch64CangjieCallSize.cpp
AArch64CopyGCEmitter.cpp
AArch64MSGCEmitter.cpp
```

`X86/CMakeLists.txt` 已显式编译：

```text
X86CangjieAsmPrinter.cpp
X86CopyGCEmitter.cpp
X86MSGCEmitter.cpp
```

这实现了物理文件拆分，但不是两套独立 LLVM 构建。CopyGC/MSGC 仍共用同一个 LLVM 二进制，通过 Module GCMode 和 wrapper/属性在单次编译中互斥选择。

## 11. 当前支持矩阵与回退规则

### 11.1 功能矩阵

| 操作 | CopyGC | MSGC AArch64 Mach-O | MSGC AArch64 ELF | MSGC X86-64 Mach-O/ELF | MSGC X86 COFF/32 位 |
| --- | --- | --- | --- | --- | --- |
| ref/weak/static read | CopyGC tagged/forwarding 路径 | 直接 load | 直接 load | 直接 load | 直接 load |
| struct/static read | CopyGC read barrier 语义 | memcpy | memcpy | memcpy | memcpy |
| `gcread_generic` | 公共 generic read | 转 struct wrapper，可 inline | `+lse` 可 inline，否则 slow | 转 struct wrapper；Idle inline、Mark slow | 32 位保留 generic helper；COFF slow |
| ref/static write | CopyGC phase/runtime 路径 | MSGC inline | `+lse` inline；`-lse` slow | Idle inline；Mark slow | wrapper slow body |
| struct/static write | CopyGC phase/runtime 路径 | inline/复杂布局 slow | `+lse` inline/复杂布局 slow | Idle memcpy；Mark slow | wrapper slow body |
| `gcwrite_generic` | 公共 generic write | 转 struct wrapper并可 inline | `+lse` 可 inline，否则 slow | 转 struct wrapper；Idle inline、Mark slow | 32 位 generic helper；COFF slow |
| ref/struct arraycopy | CopyGC runtime/既有路径 | Idle memmove；Mark slow | `+lse` Idle memmove；其他 slow | Idle memmove；Mark slow | wrapper slow body |
| generic arraycopy | 公共 runtime | 公共 runtime | 公共 runtime | 公共 runtime | 公共 runtime |
| atomic barrier | CopyGC 公共 atomic 路径 | 公共 runtime atomic | 公共 runtime atomic | 公共 runtime atomic | 公共 runtime atomic |
| object allocation | CopyGC fast wrapper/emitter | static/dynamic inline | static/dynamic inline | wrapper slow body | wrapper slow body |
| concrete/generic array | CopyGC array路径 | static/dynamic/dispatch inline | static/dynamic/dispatch inline | wrapper/dispatch slow body | wrapper/dispatch slow body |

表中的“slow body”不等于额外失败：wrapper 自身调用正确的 runtime helper，只是没有获得目标机器码内联收益。

### 11.2 feature flag 关闭时的规则

- `EnableMSGCReadBarrierLowering=false`：MSGC read 保留公共 runtime lowering，不会启用 CopyGC tagged fast path。
- `EnableMSGCWriteBarrierLowering=false`：MSGC 写不生成专用 wrapper，走 MSGC runtime helper，不会启用 CopyGC phase emitter。
- `EnableGCFastPath=false`：不做 statepoint 后 object fast wrapper替换，保留原 runtime callee。
- `enable-msgc-inline-write-barrier=false`：AArch64/X86 emitter 都不内联，原 wrapper call继续发射。

模式与优化开关是两个维度：GCMode 决定正确语义，flag 只决定是否采用 fast implementation。

### 11.3 必须保留 slow call 的情况

以下情况都应正常回 runtime，而不是强行内联：

- 目标架构/object format 不支持 emitter；
- wrapper/属性缺失或不完整；
- safepoint state 要求停顿；
- TLS `msAllocBuffer`/active Region/activeLog 为空；
- dynamic size 为 0、超过表范围或查到 `0xFF`；
- Region 类型、bitmap、TypeInfo、GCTib 或 large span 校验失败；
- WorkBuf 空间不足；
- Mark 阶段 arraycopy、复杂/长布局 struct、generic/atomic 未专门支持。

slow path 是 fast path 正确性的一部分，不是异常兜底代码。wrapper body 必须始终可执行，且调用约定、参数顺序和 statepoint 属性要与原 helper一致。

### 11.4 当前没有做的事情

- 没有维护两套 LLVM 源码或两个按宏裁剪的 LLVM 编译产物；
- 没有新增 `llvm.cj.msgc.*` intrinsic；
- 没有 X86 MSGC allocation emitter，也没有 X86 Mark barrier inline；
- 没有依赖 MSVC C++ mangling 的 COFF phase fast path；
- 没有把 atomic、generic arraycopy 和 Mark arraycopy 全部机器码内联；
- 没有恢复 fresh generic/barrier 实验；
- 没有完成本文范围外的双 SDK 端到端性能验收。

## 12. 测试、验证与维护约束

### 12.1 前端与 Module flag 测试

前端需要验证：

- 不传 `--gc-mode` 时默认 CopyGC；
- `--gc-mode=copygc` 生成 `Cangjie_GCMode=0`；
- `--gc-mode=marksweep` 生成 `Cangjie_GCMode=1`；
- 非法值被拒绝；
- O0/O2 和序列化配置都保持同一模式；
- 两种模式的 raw IR 仍使用相同 `llvm.cj.*` intrinsic。

现有 option 单测位于 `cangjie_compiler/unittests/Option/OptionTest.cpp`。

### 12.2 LLVM IR/LTO 测试

关键测试目录与用例：

```text
llvm/test/CJLTO/
  cj-gc-mode-mismatch.ll
  cj-fulllto-other-bc.ll

llvm/test/Transforms/CJRuntimeLowering/
  cangjie-gc-mode-invalid.ll
  new_array_copygc.ll
  new_array_generic_copygc.ll
  new_array_generic_msgc.ll
  new_array_concrete_dynamic_copygc.ll
  new_array_concrete_dynamic_msgc.ll

llvm/test/Transforms/CJGenericIntrinsicOpt/
  copy_generic_opt_non_phi.ll
  copy_generic_opt_phi.ll
```

`CJGenericIntrinsicOpt` 当前测试集中不再有 `msgc_gcread_generic_fresh.ll`；这是已移除专项优化的直接结构证据。

### 12.3 lowering 与后端测试

```text
llvm/test/CodeGen/AArch64/CangjieGC/
  gc-mode-backend-invalid.ll
  gc-mode-barrier-lowering-copygc.ll
  gc-mode-barrier-lowering.ll
  gc-mode-newobject-copygc.ll
  gc-mode-newobject-msgc.ll
  msgc-gcread-generic-lowering.ll
  msgc-gcwrite-generic-lowering.ll
  msgc-global-wb-gate-macho.ll
  msgc-unmarked-object-slow-call.ll
  msgc-unsupported-target-array-slow-call.ll
  msgc-writebarrier-target-independent-lowering.ll
  msgc-writebarrier-calling-conv.ll
  msgc-writebarrier-lowering.ll

llvm/test/CodeGen/AArch64/
  cangjie-msgc-elf-capability.ll
  cangjie-newarray-msgc-dynamic-concrete-fastpath.ll
  cangjie-newarray-msgc-dynamic-fastpath.ll
  cangjie-newarray-msgc-static-no-phase-fastpath.ll
  cangjie-writebarrier-msgc-fastpath.ll
  cangjie-writebarrier-msgc-*-no-inline.ll

llvm/test/CodeGen/X86/
  cangjie-msgc-writebarrier-fastpath.ll
  cangjie-msgc-writebarrier-copy-fastpath.ll
  cangjie-msgc-writebarrier-statepoint.ll
  cangjie-msgc-writebarrier-unsupported-cc.ll
  cangjie-msgc-writebarrier-win64.ll
  cangjie-msgc-wrapper-slow-call.ll
```

测试必须同时覆盖 positive inline 和 negative fallback。只检查 IR 中出现 `_MSGC` wrapper，无法证明后端真的生成 fast path；只检查汇编片段，又无法证明 CopyGC module 不会误用 MSGC ABI。

### 12.4 当前已记录的本地验证状态

Step46 最新实施记录给出的源码级结果为：

- LLVM/cjc 增量构建成功；
- GCMode/lowering/backend 聚焦 lit `33/33 PASS`；
- FullLTO/ThinLTO 混合模式用例通过；
- 默认、CopyGC、MarkSweep 的 cjc object-only 编译成功；
- Runtime CopyGC/MSGC 双模式 release 构建成功，导出符号隔离符合预期；
- `git diff --check` 通过。

这些结果是 Step46 当次验证记录。Step47 在 2026-07-21 重新完成 LLVM 增量构建，AArch64/X86 MSGC CodeGen 用例 28/28 通过；加入 CopyGC 文件拆分、GCMode/LTO、Runtime lowering 和 generic 公共优化后的扩展回归为 39/39 通过。X86 显式 statepoint 用例同时检查 `.cjmetadata.stackmap` 非空，unsupported-CC 用例检查 vectorcall 与 tail call 保留 ABI-aware fallback。本文没有重新执行 SDK 构建，也不把源码级结果等同于 SDK 三件套端到端验收。

### 12.5 SDK 端到端验证要求

后续验证必须分别产出 CopyGC/MSGC 的 compiler、runtime、stdlib 三件套，并遵守：

1. 每个 SDK 在干净 `env -i` 子 shell 中使用；
2. 用 `DYLD_PRINT_LIBRARIES=1` 核对实际加载的 runtime；
3. 切换/替换 runtime 后必须用当前 SDK 重编全部 `.cj` 用例；
4. 分别覆盖 O0/O2、object/array/generic/barrier、fast-path on/off；
5. 记录 compiler/runtime/stdlib 的来源，不复用跨 GC 模式 binary。

### 12.6 ABI 维护清单

修改以下任一项时，需要同步核对 LLVM、Runtime、测试和文档：

| ABI 项 | LLVM 依赖位置 | Runtime 对应事实 |
| --- | --- | --- |
| GCMode 数值/flag | `CJGCMode.*`、前端、LTO | SDK/bitcode 模式契约 |
| wrapper 名称/属性 | `CJMSGCABI.h`、lowering、AsmPrinter | helper/alias/export |
| size class/16KB 阈值 | `CJMSGCABI.h`、动态表、tests | allocator size class/large 判定 |
| TLS 偏移 | `AsmPrinter`/MSGC emitter | `ThreadLocalData` static_assert |
| `AllocBufferMS` 布局 | MSGC emitter | activeRegions/activeLog static_assert |
| `MSRegionInfo` 布局/类型值 | MSGC emitter | Region metadata static_assert |
| WorkBuf len/objs/capacity | write barrier emitter | `WorkBuf`/`AllocBufferMS` |
| TypeInfo/GCTib 字段 | struct barrier emitter | ObjectModel/TypeInfo |
| call 最大尺寸 | `AArch64CangjieCallSize.cpp` | 实际 emitter 指令序列 |

布局偏移变化必须通过编译期 static_assert 或聚焦测试暴露，不能依赖性能测试偶然发现静默内存破坏。

## 13. 与现有文档的关系及最终结论

### 13.1 文档分工

| 文档 | 建议用途 |
| --- | --- |
| [06_MSGC_Barrier架构与实现详解.md](06_MSGC_Barrier架构与实现详解.md) | Runtime barrier、Route-A、WorkBuf、phase 与 LLVM barrier 的完整语义 |
| [10_MSGC_优化实现与效果详解.md](10_MSGC_优化实现与效果详解.md) | LLVM + Runtime + Mark/Sweep/Allocator 的整体优化与效果 |
| 本文 | LLVM 中的 GCMode 契约、Pass 顺序、lowering、后端发射与文件结构 |
| [Step30 LLVM 侧 MSGC 优化实现盘点](../Implementation/Step30_LLVM_MSGC优化实现盘点.md) | 历史实现盘点，部分章节需结合 Step46/当前源码修正 |
| [Step46 公共 Intrinsic 到 GC 专用 Wrapper 分流与隔离方案](../Implementation/Step46_MSGC_LLVM公共Intrinsic到GC专用Wrapper分流与隔离方案.md) | 模式隔离方案、分文件实施过程和最新验证记录 |

### 13.2 历史描述的当前口径

| 历史描述 | 当前实现 |
| --- | --- |
| 通过 module 中存在 `_MSGC_dynamic` 推断 MSGC | 已删除；只看 `Cangjie_GCMode` 与显式属性 |
| `cj_malloc_array_generic` 公共映射直接指向 MSGC Fast3 | 已恢复为 `CJ_MCC_NewArrayGeneric`；仅 MSGC lowering 创建 Fast3 dispatch |
| fresh/栈目标 `gcread_generic` 可直接 memcpy | 已移除；只保留栈到栈、无引用 TypeInfo 等公共证明 |
| fresh-init barrier 消除 | 已移除 |
| 单引用 struct barrier 标量化 | 已移除 |
| arraycopy Mark 路径完全 inline | 当前 Mark 走 runtime，只有 gate=0 时 inline memmove |
| MSGC object allocation 按 encoded phase 做 mark-at-alloc | 当前 emitter 主要检查 safepoint state，不采用该历史路径 |
| X86 已有/应有 MSGC emitter | 已有 `X86MSGCEmitter.cpp`；Mach-O/ELF 内联 Idle 写路径，COFF/X86-32 与 Mark 路径保留 slow call |

### 13.3 推荐的源码阅读顺序

1. `llvm/include/llvm/IR/CJGCMode.h`、`llvm/lib/IR/CJGCMode.cpp`：先理解模式契约；
2. `llvm/include/llvm/CodeGen/CJMSGCABI.h`：理解 wrapper、属性、size class 和校验；
3. `llvm/lib/Passes/PassBuilder.cpp`、`PassBuilderPipelines.cpp`：确认 Pass 时序；
4. `llvm/lib/Transforms/Scalar/CJRuntimeLowering.cpp`：看 array 分流；
5. `llvm/lib/CodeGen/CJBarrierLowering.cpp`：看 read/write/object 的最终分流；
6. `llvm/lib/Target/AArch64/AArch64CangjieCallSize.cpp`：看是否允许 inline；
7. `AArch64CangjieAsmPrinter.cpp`、`AArch64CopyGCEmitter.cpp`、`AArch64MSGCEmitter.cpp`：看实际机器码；
8. `X86MCInstLower.cpp`、`X86CopyGCEmitter.cpp`、`X86MSGCEmitter.cpp`：看 X86 两种 GC 的薄分派、Idle fast path 和 slow fallback；
9. 对照 `llvm/test/CJLTO`、`CJRuntimeLowering`、`CodeGen/AArch64/CangjieGC` 以及 X86 fast/fallback 测试。

### 13.4 最终结论

当前 LLVM MSGC 实现已经形成清晰的分层结构：

```text
前端公共 intrinsic
  → Module GCMode 明确选择 collector
  → 共享 Pass 做 collector-independent 证明
  → Runtime/Barrier lowering 生成互斥 wrapper/属性
  → statepoint 后固化分配 callee
  → 后端统一 decision 校验并选择 emitter
  → AArch64 Mach-O/ELF 与 X86-64 Mach-O/ELF 按能力内联
  → 不支持的 ISA/object format 安全执行 wrapper slow body
```

当前真正属于 MSGC 的 LLVM 优化是：

- stable-address 前提下的普通读屏障消除；
- 目标无关的 MSGC 写屏障 wrapper，以及 AArch64/X86-64 的 inline fast path；
- static/dynamic size-class 对象和数组分配；
- generic array 类型/宽度 dispatch；
- AArch64 中与 Runtime 布局对齐的 Region/bitmap/WorkBuf fast emission；
- X86-64 Mach-O/ELF 中的 phase-gated Idle store/memcpy/memmove emission。

`CJGenericIntrinsicOpt`、`CJBarrierSplit`、`CJBarrierOpt` 的保留逻辑仍是公共优化；已经撤销的 fresh/栈目标、fresh-init 和单引用 struct 实验不再属于当前能力。CopyGC/MSGC 在单一 LLVM 中通过 GCMode 和 ABI 契约互斥，而不是通过宏编译两套后端。AArch64 与 X86 的 collector 发码均已按 GC 分文件；架构、object format 和指令集差异只在最终 target 发射层决定，公共 lowering 不再硬编码 AArch64 或 Mach-O。
