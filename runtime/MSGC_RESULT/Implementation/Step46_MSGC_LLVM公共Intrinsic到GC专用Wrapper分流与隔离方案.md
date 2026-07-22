# Step46 MSGC LLVM 公共 Intrinsic 到 GC 专用 Wrapper 分流与隔离方案

> 状态：2026-07-21 已完成 GCMode 契约、CopyGC/MSGC lowering 互斥分流、后端启发式删除与文件拆分、统一 allocation emission/size decision，以及 Runtime MSGC 专用数组符号宏隔离。LLVM/cjc 本地构建、lit、bitcode flag 和 CopyGC/MSGC Runtime 双构建/符号检查已通过；SDK compiler/runtime/stdlib 三件套组装与 `.cj` 端到端运行按约定留待后续。
>
> 目标：保留仓颉前端公共 `llvm.cj.*` intrinsic，通过 Module 级 GC 模式在 LLVM 优化与 lowering 边界区分 CopyGC/MSGC，避免 MSGC 默认开关污染 CopyGC 代码生成。
>
> 范围：前 1—16 节保留设计与验收基线；17—19 节记录实际实施、验证结果和仍未执行的 SDK 阶段。

## 1. 背景与审计结论

### 1.1 当前没有新增 MSGC intrinsic

当前仓颉前端仍生成公共的 `llvm.cj.*` intrinsic。重点入口包括：

- `cangjie_compiler/src/CodeGen/CJNative/CJNativeIRBuilder.cpp`：生成 ref/struct/static 读写屏障，以及具体数组分配 intrinsic。
- `cangjie_compiler/src/CodeGen/CJNative/CJNativeIntrinsicsCall.cpp`：生成对象分配、generic 读写屏障等 intrinsic。
- `llvm/include/llvm/IR/Intrinsics.td`：统一定义 `cj_gcread_*`、`cj_gcwrite_*`、`cj_array_copy_*`、`cj_malloc_object`、`cj_malloc_array` 和 `cj_malloc_array_generic`。

对 `Intrinsics.td`、`CJIntrinsics.h` 和 `CJIntrinsics.cpp` 的审计没有发现 `msgc` 专用 intrinsic。MSGC 的差异当前发生在后续优化、runtime lowering、barrier lowering 和 AArch64 发射阶段，而不是前端 intrinsic 生成阶段。

这说明没有必要为了隔离 CopyGC/MSGC 再复制一套 `llvm.cj.msgc.*` 定义。公共 intrinsic 应继续表达语言层的语义操作，GC 算法差异由后端选择具体实现。

### 1.2 当前 MSGC LLVM 实现覆盖的层次

当前 MSGC 相关实现主要分布在以下文件：

| 层次 | 文件 | 当前 MSGC 行为 |
| --- | --- | --- |
| ABI 公共定义 | `llvm/include/llvm/CodeGen/CJMSGCABI.h` | 集中定义 wrapper 名称、属性、36 档 size class 和 16KB 阈值 |
| generic 优化 | `llvm/lib/Transforms/Scalar/CJGenericIntrinsicOpt.cpp` | 已撤销 fresh/仅栈目标的 `gcread_generic -> memcpy`；保留原有栈到栈、已知无引用等 GC 无关优化 |
| barrier 优化 | `llvm/lib/Transforms/Scalar/CJBarrierOpt.cpp` | 已撤销 fresh-init barrier 消除和单引用 struct 标量化；恢复 `dev` 基线逻辑 |
| runtime lowering | `llvm/lib/Transforms/Scalar/CJRuntimeLowering.cpp` | 对象/数组 intrinsic 映射到 runtime 函数，部分路径直接使用 MSGC 名称 |
| barrier lowering | `llvm/lib/CodeGen/CJBarrierLowering.cpp` | MSGC 读屏障消除、写屏障改写为 `*_Fast_MSGC` wrapper、statepoint 后分配 wrapper 特化 |
| callsite dispatch | `llvm/lib/CodeGen/AsmPrinter/AsmPrinter.cpp` | 识别 MSGC wrapper，并决定是否进入目标后端 inline 发射 |
| AArch64 发射 | `llvm/lib/Target/AArch64/AArch64AsmPrinter.cpp` | 发射 MSGC 分配和写屏障 fast path |
| 指令尺寸估算 | `llvm/lib/Target/AArch64/AArch64InstrInfo.cpp` | 为 inline 展开返回保守尺寸，保证分支松弛和 jump table 正确 |
| X86 发射 | `llvm/lib/Target/X86/X86MCInstLower.cpp` | 当前只有 Cangjie 公共发射与 CopyGC object/array fast path，没有 MSGC emitter |

### 1.3 当前隔离缺口

当前代码并没有一个显式、统一的“本 Module 使用 MSGC”判定，因而存在以下风险：

1. `CJBarrierLowering.cpp` 中 `enable-msgc-read-barrier-lowering` 和 `enable-msgc-write-barrier-lowering` 当前默认开启。读屏障路径没有 GC 模式判断；写屏障只额外检查 AArch64。
2. `shouldSkipLegacyGCPhaseFastPath()` 只依据 MSGC 写屏障开关和目标架构，会让 CopyGC AArch64 也跳过原有 phase fast path。
3. `doNewFastPath()` 处理已经重写成 `GCStatepointInst` 的分配调用时，直接生成 `_MSGC_cls*` 或 `_MSGC_dynamic` wrapper，没有 Module 模式判定。
4. `CJRuntimeLowering.cpp` 的 `cj_malloc_array_generic` 当前固定映射到 `CJ_MCC_NewArrayGenericFast3_MSGC`，同样没有模式隔离。
5. `CJGenericIntrinsicOpt.cpp` 和 `CJBarrierOpt.cpp` 都早于最终 wrapper，因而依赖 MSGC 语义的新变换原本需要 GCMode 保护。当前两个 Pass 中本轮新增的 fresh/栈目标 generic 折叠、fresh-init 消除和单引用 struct 标量化都已撤销；以后若再引入 GC 语义相关优化，仍需在对应 Pass 读取 GCMode。
6. `AArch64AsmPrinter.cpp` 会通过 Module 中是否存在 `CJ_MCC_NewObjectFast_MSGC_dynamic` 推断 `InMSGCMode`。这是基于偶然符号存在性的启发式判断：Module 没有动态分配时可能漏判，链接或优化残留该符号时也可能误判。
7. `AArch64InstrInfo.cpp` 与 `AsmPrinter.cpp` 必须采用完全相同的 inline 判定；否则指令尺寸估算与实际展开不一致，可能破坏分支松弛或 jump table。

### 1.4 审计结论

“公共 intrinsic 到 wrapper 时再区分”是正确方向，但必须把“wrapper 边界”理解为一组 lowering 边界，而不只是 `AsmPrinter` 的最终调用发射：

```text
公共 llvm.cj.* intrinsic
        │
        ├─ 依赖 GC 语义的早期优化：先查 Module GCMode
        │
        ├─ CJRuntimeLowering / CJBarrierLowering：按 GCMode 生成不同调用或 wrapper
        │
        ├─ statepoint 后分配 lowering：继续读取同一个 GCMode
        │
        └─ AsmPrinter / AArch64：只消费已经分流后的 wrapper/属性
```

因此需要新增的不是 MSGC intrinsic，而是一个能够从前端贯穿 LTO、IR Pass、CodeGen 和目标后端的显式 GC 模式标记。

## 2. 设计目标与非目标

### 2.1 设计目标

1. 前端继续生成同一套 `llvm.cj.*` intrinsic，不让语言 IR 与具体 GC 算法绑定。
2. 每个 Cangjie Module 显式携带 CopyGC/MSGC 模式，所有依赖 GC 语义的 Pass 使用同一查询接口。
3. CopyGC Module 只能走现有 CopyGC lowering；MSGC Module 才能产生 `_MSGC` wrapper、MSGC runtime 符号和 MSGC 专用 inline 代码。
4. 优化开关只控制某项优化是否启用，不能再兼任 GC 模式开关。例如 `enable-msgc-inline-write-barrier=false` 表示 MSGC 走 wrapper slow path，不表示退回 CopyGC。
5. LTO 合并不同 GC 模式的 bitcode 时必须失败，避免生成“部分 CopyGC、部分 MSGC”的不一致二进制。
6. 旧 bitcode 没有 GCMode 时保持兼容，按 CopyGC 处理；新 MSGC 编译链必须显式写入 MSGC 标记。
7. 后端发射与指令尺寸估算使用相同的 wrapper/属性分类函数，不进行第二次 GC 模式猜测。

### 2.2 非目标

- 本方案不修改 MSGC 或 CopyGC 的回收算法、对象模型和 Runtime 调度策略。
- 本方案不支持在同一个最终 Module/可执行文件内混合两种堆对象 ABI。
- 本方案不为每个 intrinsic 新增 `cj_msgc_*` 变体。
- 本方案不根据 runtime dylib 名称、环境变量或某个函数是否存在来自动推断 GC 模式。
- 本方案不把 AArch64 MSGC fast path 直接推广到 X86、Linux ELF 或其他尚未验证的平台；这些平台先走正确的 wrapper slow path。
- 本方案不改变“替换 runtime 后必须重新编译 `.cj` 用例”的 SDK 验证规则。

## 3. 方案对比与结论

### 3.1 方案 A：公共 intrinsic + Module GCMode + 专用 wrapper（推荐）

前端保持当前 intrinsic；在 Module 中写入 GC 模式，所有依赖 GC 语义的 Pass 先读取模式，lowering 再生成 CopyGC runtime call 或 MSGC wrapper。

优点：

- intrinsic 数量和前端生成逻辑不膨胀。
- GC 算法选择是 Module 的显式契约，能够贯穿普通编译和 LTO。
- CopyGC 与 MSGC 可以复用绝大多数与 GC 算法无关的 IR 优化。
- wrapper 继续作为 LLVM IR/后端之间的内部 ABI，AArch64 fast path 不需要识别公共 intrinsic。
- 现有 MSGC 实现已经采用 wrapper/属性体系，改动可以渐进完成。

代价：

- 每一个依赖 GC 语义的早期 Pass 都必须完成一次审计，不能只修改 `CJBarrierLowering`。
- 必须定义缺失标记和混合 LTO 的严格规则。

### 3.2 方案 B：新增一套 MSGC intrinsic

例如新增 `llvm.cj.msgc.gcread_ref`、`llvm.cj.msgc.gcwrite_ref` 和 `llvm.cj.msgc.malloc_object`，让前端在生成 IR 时直接选择。

优点是单条指令的算法身份非常直观。缺点是 intrinsic 定义、参数索引、Verifier、分析、优化、测试和前端调用点都要复制或增加分支；generic、atomic、arraycopy、statepoint 等组合会持续扩大维护面。更重要的是，LTO Module 是否能够和另一 GC 模式链接、某些普通函数应采用哪种分配 ABI，仍需要 Module 级信息，不能仅靠 intrinsic 完整解决。

因此该方案改动最大，却不能消除 Module 契约的需要，不采用。

### 3.3 方案 C：编译宏隔离或维护两套 LLVM 构建

通过 `#ifdef MSGC` 或 CMake 宏让一套 LLVM 只支持 CopyGC，另一套只支持 MSGC。

优点是编译产物层面隔离直接。缺点是需要长期维护两套编译器，容易出现 Pass、修复和测试漂移；bitcode 仍可能被错误的 `llc`/LTO 工具处理；宏也无法表达输入 Module 的实际 ABI。

编译宏可以用于控制 MSGC 后端代码是否编入某个发行包，但不能替代 IR 中的 GCMode。

### 3.4 结论

采用方案 A。准确表述不是“只在生成 intrinsic 时选择 MSGC intrinsic 或 CopyGC intrinsic”，而是：

> 前端只生成公共 intrinsic，并写入 Module GCMode；在所有依赖 GC 语义的优化/lowering 点按 GCMode 分流，最终用不同 wrapper 或 runtime call 固化选择。

wrapper 是分流结果和后端 ABI，不是 GC 模式的唯一信息源。

## 4. 推荐架构与完整数据流

### 4.1 总体数据流

```text
cjc/SDK 构建配置
        │
        ├─ 选择 CopyGC 或 MSGC
        ▼
CGModule 创建 LLVM Module
        ├─ !llvm.module.flags: Cangjie_GCMode = 0/1
        └─ 仍生成公共 llvm.cj.* intrinsic
        │
        ▼
依赖 GC 语义的 IR Pass
        ├─ CopyGC：保持/执行 CopyGC 可证明的优化
        └─ MSGC：允许不移动 GC 专用的消除和改写
        │
        ▼
CJRuntimeLowering / CJBarrierLowering
        ├─ CopyGC：CJ_MCC_* 通用/CopyGC 调用
        └─ MSGC：*_MSGC runtime 调用或内部 wrapper + MSGC 属性
        │
        ▼
statepoint / Machine IR
        │
        ▼
AsmPrinter 与目标后端
        ├─ 普通符号：正常 call
        └─ 已分类 MSGC wrapper：在支持的目标上 inline，否则调用 wrapper slow body
        │
        ▼
链接与对应 GC 模式的 libcangjie-runtime
```

### 4.2 各层职责

| 层次 | 负责 | 不负责 |
| --- | --- | --- |
| cjc/CGModule | 决定并记录 GCMode；生成公共 intrinsic | 不生成目标架构 fast path |
| IR 优化 Pass | 依据 GCMode 使用合法的语义证明 | 不通过函数名猜测 GCMode |
| Runtime/Barrier Lowering | 把公共 intrinsic 固化为该 GC 的调用/wrapper | 不依赖 SDK 环境变量 |
| `CJMSGCABI.h` | 统一 MSGC wrapper 名称、属性和布局常量 | 不决定当前 Module 是否为 MSGC |
| AsmPrinter/AArch64 | 消费 wrapper/属性并发射已支持 fast path | 不重新推断或改变 GC 选择 |
| Runtime | 提供所选 GC 的符号与 ABI，实现 slow path | 不修正编译器生成的错误 GC 模式代码 |

### 4.3 为什么必须在 wrapper 之前也能判定模式

最终写屏障 wrapper 的确能把两种后端发射分开，但以下路径早于 wrapper：

- `CJGenericIntrinsicOpt` 仍会基于栈到栈、已知无引用等 GC 无关证明生成 memcpy；新增的 fresh/仅栈目标专项折叠已撤销。
- `CJBarrierOpt` 的 fresh-init barrier 消除和单引用 struct 标量化已撤销，当前仅保留原有公共 barrier 优化。
- `CJRuntimeLowering` 会先选择数组和对象的 runtime callee。
- `CJRewriteStatepoint` 之后，`CJBarrierLowering::doNewFastPath()` 看到的是 statepoint 中的实际 callee，不再是最初的 `cj_malloc_object` intrinsic。

这些步骤中依赖 GC 语义的变换必须读取 Module GCMode。等到 `AsmPrinter` 才区分，已经无法撤销错误的 IR 变换或恢复被选错的 runtime callee。

### 4.4 wrapper 的定位

MSGC wrapper 应满足以下契约：

1. 名称和函数属性由 `CJMSGCABI.h` 统一定义。
2. wrapper 只允许在 MSGC Module 中创建。
3. wrapper 必须拥有可执行的 slow body；目标或开关不支持 inline 时，普通调用仍然正确。
4. 后端以 wrapper/属性判断“该调用已经被 lowering 选成 MSGC”，而不是判断整个 Module 的 GC 模式。
5. wrapper 内部调用的 runtime 符号必须属于 MSGC Runtime 可用 ABI；CopyGC Module 中不得出现。

这样，Module GCMode 负责“选择”，wrapper 负责“承载已经完成的选择”，两者职责清晰且不会互相替代。

## 5. GCMode 表示与公共判定接口

### 5.1 Module flag 定义

建议新增整数 Module flag：

```llvm
!llvm.module.flags = !{!0}
!0 = !{i32 1, !"Cangjie_GCMode", i32 1}
```

其中行为值 `1` 对应 `Module::Error`，GC 模式值定义为：

| 值 | 枚举 | 含义 |
| --- | --- | --- |
| `0` | `CJGCMode::CopyGC` | 当前 CopyGC ABI 和 lowering |
| `1` | `CJGCMode::MSGC` | Mark-Sweep GC ABI 和 lowering |

使用 `Module::Error` 的目的，是让两个均由新编译器生成、但 GCMode 不同的 Module 在 IR Link/LTO 合并时直接报错，而不是任意选择一侧。

### 5.2 公共接口

建议新增 `llvm/include/llvm/IR/CJGCMode.h`，避免 Scalar、CodeGen、AsmPrinter 和 Target 各自解析 metadata：

```cpp
namespace llvm {
enum class CJGCMode : uint32_t {
  CopyGC = 0,
  MSGC = 1,
};

CJGCMode getCangjieGCMode(const Module &M);
bool isCangjieMSGC(const Module &M);
bool isCangjieCopyGC(const Module &M);
} // namespace llvm
```

接口实现必须统一完成：

1. 提取 `ConstantAsMetadata/ConstantInt`。
2. 缺失 flag 时返回 CopyGC，兼容旧 bitcode 和未更新的手写测试。
3. 遇到非整数或未知值时立即诊断，不能静默回落。
4. 仅对 `CJBC`/Cangjie GC Module 使用该模式；普通 LLVM Module 不应受到影响。

如果实现放在 `.cpp` 中，需要同步加入对应 CMake；如果采用小型 inline helper，则必须保证错误处理和解析逻辑仍只有一份。

### 5.3 缺省与严格模式

兼容策略分两层：

- 普通 LLVM Pass 独立处理旧 Module：缺失 `Cangjie_GCMode` 时按 CopyGC。这保证旧 `.bc` 和大量现有 lit 测试不会一次性失效。
- 新版 cjc：无论 CopyGC 还是 MSGC，都必须显式写 `0` 或 `1`。MSGC SDK 不允许依赖“缺失即 MSGC”。

对于 LTO，`Module::Error` 只能检测“显式 0 与显式 1”冲突，无法证明缺失标记的旧 Cangjie bitcode属于哪种 GC。因此建议在 Cangjie LTO 输入检查处增加严格规则：只要本次链接目标是 MSGC，所有 `CJBC` 输入都必须显式为 `MSGC`；缺失、CopyGC 或未知值都报错。CopyGC 链接可继续接受缺失标记的旧 bitcode。

### 5.4 模式与优化开关分离

判定应统一写成：

```text
IsMSGC = isCangjieMSGC(Module)
UseFeature = IsMSGC && EnableSpecificOptimization && TargetSupported
```

不能写成：

```text
IsMSGC = EnableMSGCReadBarrierLowering
IsMSGC = Module 中存在某个 _MSGC wrapper
IsMSGC = 当前链接到了某个 runtime dylib
```

关闭某个 MSGC fast path 后仍然是 MSGC，只是必须回到 MSGC 正确的 slow path；绝不能退到 CopyGC 的对象布局或 barrier 语义。

## 6. 公共 Intrinsic 的 CopyGC/MSGC 分流矩阵

下表描述推荐的语义分流。具体 wrapper 名称继续以 `CJMSGCABI.h` 为准。

| 公共 intrinsic/类别 | CopyGC Module | MSGC Module |
| --- | --- | --- |
| `cj_gcread_ref/weakref/static_ref` | 保留 CopyGC tagged/forwarding fast path或降到 `CJ_MCC_Read*` | 普通 load；仅在明确满足 MSGC 不移动语义时消除 helper |
| `cj_gcread_struct/static_struct` | 保留 CopyGC 读屏障语义 | 按现有 MSGC 规则降为 memcpy |
| `cj_gcread_generic` | 保留 CopyGC generic 语义；只有栈到栈、已知无引用等与 GC 无关的证明才能 memcpy | 同样保留 generic/struct barrier；fresh 或仅目标为栈不再单独作为 memcpy 条件 |
| `cj_gcwrite_ref/static_ref` | 现有 `CJ_MCC_Write*` 与 CopyGC phase fast path | AArch64 改为 `CJ_MCC_Write*Fast_MSGC` wrapper；未支持目标调用 MSGC runtime slow helper |
| `cj_gcwrite_struct/static_struct` | 现有 CopyGC struct barrier | AArch64 改为 MSGC struct wrapper，复杂布局或不支持目标回 slow helper |
| `cj_gcwrite_generic` | `CJ_MCC_WriteGeneric` 或 CopyGC 合法优化 | 可证明时消除；否则转换为保持 old-edge 语义的 MSGC generic/struct 路径 |
| `cj_array_copy_ref/struct` | 现有 CopyGC arraycopy barrier | MSGC wrapper；idle 可 inline copy，mark 阶段按当前正确性约束回 runtime |
| `cj_array_copy_generic` | `CJ_MCC_ArrayCopyGeneric` | 仅在类型/布局可证明时特化，否则保守调用 runtime |
| `cj_malloc_object` | `CJ_MCC_NewObject`，沿用 CopyGC fast allocation | statepoint 后按 size class 生成 `_MSGC_cls*`/`_MSGC_dynamic` wrapper，大对象保留 slow path |
| `cj_malloc_array` | 现有 `CJ_MCC_NewArray`/CopyGC 路径 | 具体类型、常量或动态大小按 MSGC wrapper/属性分流 |
| `cj_malloc_array_generic` | 恢复/保留 `CJ_MCC_NewArrayGeneric` | 才允许生成 `CJ_MCC_NewArrayGenericFast3_MSGC*` dispatch/wrapper |
| atomic 引用操作 | 保持 CopyGC runtime/atomic barrier 语义 | 在完成专用设计前保持 runtime slow helper，不因其他 MSGC wrapper 自动 inline |

### 6.1 分流的两条硬规则

1. CopyGC 输出中不得包含 `_MSGC` 符号、`cj-msgc-*` 属性，也不得直接访问 `msAllocBuffer`、MSRegion bitmap 或 MSGC 导出全局量。
2. MSGC 输出中不得误用依赖 CopyGC forwarding/tagged-pointer/TLAB 布局的 fast path；未支持的场景必须回到 MSGC Runtime 的公共 slow helper。

### 6.2 可以共享的部分

以下变换如果证明完全不依赖对象是否移动、barrier 算法和堆布局，可以继续共用：

- 对无引用 POD 的 memcpy/memmove 合并。
- 纯栈对象且不会逃逸到 GC 堆的拷贝优化。
- 与 GC 算法无关的常量折叠、死代码消除和类型布局计算。

文档或代码评审中不能因为测试文件名带 `msgc` 就默认该优化只能用于 MSGC；应以它依赖的语义证明为准。无法证明为公共优化时，先用 GCMode 隔离。

## 7. 编译器前端修改点

### 7.1 增加编译选项中的 GC 模式

在 cjc 的全局编译选项中新增强类型枚举，例如：

```text
GCMode::COPY_GC
GCMode::MARK_SWEEP
```

建议提供内部/驱动选项 `--gc-mode=copygc|marksweep`，默认值为 `copygc`。正式 SDK 可以由构建系统固定传入，用户显式覆盖时必须保证所选 runtime 和 stdlib 与之匹配。

预计涉及：

- `include/cangjie/Option/Option.h`：新增枚举和 `GlobalOptions` 字段。
- `include/cangjie/Option/Options.inc`：登记命令行选项和值。
- `src/Option/Option.cpp` 或现有选项解析入口：校验字符串并设置枚举。
- SDK/编译器构建脚本：构建 MSGC SDK 时默认传 `marksweep`，CopyGC SDK 保持 `copygc`。

GC 模式还必须进入增量编译、bitcode/对象缓存或编译配置摘要的 cache key。否则切换模式后可能复用另一种 GC 的旧 `.bc`/`.o`。

### 7.2 在 CGModule 创建时写入一次 Module flag

`cangjie_compiler/src/CodeGen/CGModule.cpp` 已在 Module 创建时写入 `CJBC`、`Cangjie_OPT` 等 flag。应在同一位置增加：

```cpp
module->addModuleFlag(llvm::Module::Error, "Cangjie_GCMode",
                      options.gcMode == GCMode::MARK_SWEEP ? 1U : 0U);
```

要求：

1. 每个由新版 cjc 产生的 Cangjie Module 都写入，不能只给包含分配或 barrier 的 Module 写入。
2. 写入发生在优化和 bitcode 输出之前，使 `-emit-llvm`、LTO 和离线 `llc` 都能看到。
3. 同一个编译任务的所有 package/submodule 使用相同模式。
4. 模式值必须与 LLVM 公共枚举共享契约，禁止前端和 LLVM 分别定义不同数字。

### 7.3 intrinsic 生成点保持不变

以下代码继续调用原有 `llvm::Intrinsic::cj_*` ID，不新增 `if (MSGC)`：

- `CJNativeIRBuilder.cpp` 中的 `CreateGCMalloc`、读写屏障和数组相关生成逻辑。
- `CJNativeIntrinsicsCall.cpp` 中的 `cj_malloc_object`、`cj_gcwrite_generic`、`cj_gcread_generic` 等生成逻辑。
- `CGUtils.h::SetGCCangjie()` 继续把 managed 函数标记为 `gc "cangjie"`。

`gc "cangjie"` 表示该函数由仓颉 GC 管理，不足以区分 CopyGC/MSGC，因此不能替代 Module flag。

### 7.4 前端输出验证

同一份 `.cj` 分别用两种模式执行 `-emit-llvm -S`，预期：

- 函数体中的公共 intrinsic 名称和参数保持一致。
- `!llvm.module.flags` 中 `Cangjie_GCMode` 分别为 `0` 和 `1`。
- MSGC 模式在前端初始 IR 阶段尚不出现 `_MSGC` wrapper；wrapper 必须由后续 lowering 创建。

## 8. LLVM IR/CodeGen 公共设施修改点

### 8.1 新增 `CJGCMode` 公共设施

建议新增：

- `llvm/include/llvm/IR/CJGCMode.h`
- 如采用非 inline 实现，再增加 `llvm/lib/IR/CJGCMode.cpp` 和 `llvm/lib/IR/CMakeLists.txt` 条目。

该设施只负责 Module 模式的定义、读取和校验。MSGC wrapper 名称、函数属性、size class 与布局常量继续放在 `llvm/include/llvm/CodeGen/CJMSGCABI.h`。二者不要合并：前者属于 IR 级模式契约，后者属于 MSGC CodeGen ABI。

### 8.2 增加统一校验

可在 `CJIRVerifier` 或 Cangjie pipeline 的早期校验点增加以下不变式：

| 检查 | 失败条件 |
| --- | --- |
| flag 格式 | 值不是 `i32 0/1` |
| MSGC wrapper 所属模式 | CopyGC/缺失模式 Module 中已出现 `_MSGC` wrapper 或 `cj-msgc-*` 属性 |
| CopyGC fast ABI 所属模式 | MSGC Module 被改写到明确依赖 CopyGC TLAB/forwarding 的专用 wrapper |
| Module 一致性 | 同一 Module 内出现互斥的 GC ABI 标记 |

Verifier 负责尽早暴露错误，不能用“修复”方式静默删除 wrapper 或改写模式。

### 8.3 LTO/IR Link 处理

需要覆盖三种组合：

| 输入 | 结果 |
| --- | --- |
| 显式 CopyGC + 显式 CopyGC | 允许 |
| 显式 MSGC + 显式 MSGC | 允许 |
| 显式 CopyGC + 显式 MSGC | `Module::Error` 合并失败 |
| 旧 bitcode（缺失）+ CopyGC | 兼容，按 CopyGC |
| 旧 bitcode（缺失）+ MSGC | 严格 LTO 输入检查失败，要求重新编译 |

`llvm/lib/LTO/LTO.cpp` 已识别 `CJBC`，可在 Cangjie bitcode 输入尚未丢失单 Module 边界时增加 GCMode 一致性检查。不能只依赖最终 CombinedModule，因为缺失标记的输入在合并后无法再可靠追溯。

### 8.4 Pass 使用规范

所有 Pass 遵循以下规则：

1. 在 Module/Function 入口查询一次模式并缓存为局部只读值，避免在逐指令热循环重复解析 metadata。
2. 模式只决定语义路径；`cl::opt` 仅决定该路径内是否启用某项优化。
3. 公共优化可以不检查模式，但代码注释或测试要说明其证明为什么同时适用于两种 GC。
4. 新增任何 `_MSGC` 符号或 `cj-msgc-*` 属性时，创建点必须被 `isCangjieMSGC()` 支配。
5. 后续新增 X86/ELF fast path 时仍复用同一个 Module 模式，不新增架构专用 GCMode。

### 8.5 测试 IR 的迁移规则

现有手写 `.ll` 若测试公共/CopyGC 行为，可以暂时缺省并按 CopyGC；新增测试必须显式写 `Cangjie_GCMode=0`。所有 MSGC lit 测试必须显式写 `Cangjie_GCMode=1`，防止测试只因默认开启的 `cl::opt` 偶然通过。

当前工作区中已有 17 个文件名包含 `msgc` 的 LLVM 测试，但仍处于未跟踪状态。实施时应先纳入版本管理并补齐 Module flag，再把对应 CopyGC 负向用例成对加入。

## 9. LLVM 各 Pass 的具体修改点

### 9.1 `CJGenericIntrinsicOpt.cpp`

2026-07-20 已直接撤销 `processFreshGcreadGeneric()` 及其 fresh heap/栈目标证明辅助函数，`cj_gcread_generic` 恢复统一进入 `processGenericBarrier()`。实施后的边界为：

1. “fresh heap 目标”或“目标是 alloca”本身不再导致 `gcread_generic -> memcpy`。
2. 保留原有公共优化：栈到栈拷贝、TypeInfo/布局已证明无引用时仍可降为 memcpy，`GenericCopyOpt` 的等价源回溯也仍保留。
3. 不为已撤销的优化增加 GCMode gate；负向回归测试要求 fresh heap、fresh 栈目标和未 memset 的栈目标都保留 `gcread_generic`。

撤销原因是：fresh 只证明目标区域没有需要保留的 old edge，不证明从源区域读取的引用可跳过 GC read/generic 语义。仅目标为栈也只能证明写入位置，不能单独证明源 payload 是无引用的纯字节拷贝。

### 9.2 `CJBarrierOpt.cpp`

2026-07-20 已撤销 `preprocessBarriers()`、`optFreshInitBarrier()`、
`scalarizeOneRefStructBarrier()` 及它们的区间/类型分析辅助函数，并恢复
`barriersCheckFail()` 对 GC/global 目标 store 的原始保守判断。实施后的边界为：

1. fresh 新对象的首次 `cj_gcwrite_ref`/`cj_gcwrite_struct` 不再因 fresh provenance 直接改成 store/memcpy。
2. 含恰好一个直接引用字段的 `cj_gcwrite_struct` 不再拆成 `cj_gcwrite_ref + memcpy`，原 struct barrier 保留。
3. 原有 `optStructBarrier()`、`optStackBarrier()` 等公共逻辑保留；不为已撤销的两项优化增加 GCMode gate。
4. 负向回归测试分别检查 fresh 首次/连续写仍保留 barrier，以及单引用 struct 不生成 ref barrier 或 memcpy。

这两项实验在当时 case27 口径下效果基本与优化前一致，不值得为一个未显示收益的变换继续扩大 CopyGC/MSGC 语义隔离面。

### 9.3 `CJRuntimeLowering.cpp`

这是数组分配最直接的污染点。当前 `RuntimeMap` 把 `cj_malloc_array_generic` 固定映射到 `CJ_MCC_NewArrayGenericFast3_MSGC`，必须改为按模式选择。

具体修改：

1. 通用 `RuntimeMap` 恢复 `cj_malloc_array_generic -> CJ_MCC_NewArrayGeneric`，不要在静态全局表中保存模式相关选择。
2. `RuntimeLowering` 构造时保存 `CJGCMode`，或让 `replaceNewArray()`、`replaceNewArrayGeneric()` 显式接收模式。
3. CopyGC：沿用原 `CJ_MCC_NewArray`/`CJ_MCC_NewArrayGeneric` lowering，不创建任何 `_MSGC` dispatch/wrapper。
4. MSGC：才调用当前 concrete/dynamic/generic array fast lowering，创建 `CJ_MCC_NewArrayGenericFast3_MSGC_dispatch`、`*_dyn_*` 等内部函数与 `cj-msgc-*` 属性。
5. `EnableCJNewArrayFast` 只在 `IsMSGC && EnableCJNewArrayFast` 时控制 MSGC fast path；关闭时回到 MSGC 正确的通用 array runtime call。
6. 所有 fallback callee 的函数类型、calling convention、`noalias`、statepoint/leaf 属性必须与修改前路径逐项核对。

### 9.4 `CJBarrierLowering.cpp`：barrier 分流

`BarrierMaker` 已持有 `Module &M`，可以直接缓存 `IsMSGC`。建议修改为：

```text
shouldLowerMSGCReadBarrier = IsMSGC
                              && EnableMSGCReadBarrierLowering
                              && intrinsic supported

shouldLowerMSGCWriteBarrier = IsMSGC
                               && EnableMSGCWriteBarrierLowering
                               && AArch64
                               && intrinsic supported
```

同时：

- `replaceInstWithBarrier()` 保持“MSGC 专用分支优先、否则公共 runtime lowering”的结构，但 MSGC 分支必须被 `IsMSGC` 支配。
- `shouldSkipLegacyGCPhaseFastPath()` 改为接收 Module/`IsMSGC`，只有 `IsMSGC && EnableMSGCWriteBarrierLowering && AArch64` 才跳过 CopyGC legacy phase fast path。
- `readBarrierFastPath()` 在 CopyGC Module 中继续按原有 `EnableTaggedPointer` 逻辑运行；MSGC 读屏障已直接消除时不再重复处理。
- `writeBarrierFastPath()` 在 CopyGC Module 中继续按原有 `EnableGCPhase` 逻辑运行；MSGC wrapper 路径不应被 CopyGC phase shortcut 提前绕开。
- 非 AArch64 MSGC 暂不生成 AArch64 专用 wrapper，直接 lowering 到当前 Runtime 公共 barrier helper，保证正确而不冒用目标布局。

### 9.5 `CJBarrierLowering.cpp`：statepoint 后分配分流

当前 `doNewFastPath()` 已把原 CopyGC 的 `CJ_MCC_NewObjectFast`/`CJ_MCC_NewFinalizerFast` 逻辑替换成 MSGC size-class wrapper 逻辑。应拆成两个明确函数：

```text
doCopyGCNewFastPath()
    CJ_MCC_NewObject       -> CJ_MCC_NewObjectFast
    CJ_MCC_NewFinalizer    -> CJ_MCC_NewFinalizerFast

doMSGCNewFastPath()
    static small object    -> *_MSGC_cls<sizeclass>_sz<slotsize>
    dynamic small object   -> *_MSGC_dynamic
    large/unsupported      -> 原 MSGC runtime slow callee
```

`runOnFunction()` 在收集完 `GCStatepointInst` 后按父 Module 的 GCMode 调用其中一个。这里不能尝试从 `GCStatepointInst` 的 callee 名称反推模式，因为 statepoint 重写前的公共 callee 可能相同。

还需确保：

- CopyGC 的 `EnableGCFastPath` 行为恢复到 MSGC 修改前基线。
- MSGC size-class 属性只加在 MSGC wrapper 上。
- finalizer、动态大小、大对象和 JIT 分支分别保留自己的 fallback。
- `Changed` 返回值按实际改写累计，不能因为进入函数就无条件为 true。

### 9.6 相关 Pass 的补充审计

虽然当前 MSGC 修改集中在上述文件，实施时仍应对下列 Pass 做“是否含 GC 算法假设”的静态审计：

| Pass | 审计重点 |
| --- | --- |
| `CJBarrierSplit` | 拆分后的 pre/post barrier 是否对两种 GC 等价 |
| `CJGCInstrReplace/Restore` | intrinsic ID 和恢复路径是否会丢失模式信息 |
| `CJRewriteStatepoint` | Module flag 是否完整保留；实际 callee 改写后是否仍能正确分流 |
| `CJPartialEscapeAnalysis` | heap-to-stack 后 barrier 消除证明是否与移动 GC 相关 |
| `CJIRVerifier` | 是否能够在 CodeGen 前拒绝交叉模式 wrapper |

这些文件不一定需要修改；只有发现模式相关假设时才增加 gate。审计结果应通过注释或测试记录，而不是为了“看起来完整”无条件加入宏。

### 9.7 开关默认值处理

可以暂时保留 MSGC 各优化开关当前默认值，但生效条件必须改成：

| 开关 | 新含义 |
| --- | --- |
| `enable-msgc-read-barrier-lowering` | 仅在 MSGC Module 内启用读屏障消除 |
| `enable-msgc-write-barrier-lowering` | 仅在 MSGC+AArch64 内生成写屏障 wrapper |
| `enable-msgc-inline-write-barrier` | 已生成 MSGC wrapper 后，是否在支持平台 inline；关闭则调用 wrapper slow body |
| `enable-cangjie-new-array-fastpath` | 在对应 GCMode 已选择正确 array ABI 后，是否启用该模式的 fast path |
| `enable-gc-fast-path` | 允许分配 fast path；实际使用 CopyGC 或 MSGC 实现由 GCMode 决定 |

这样即使某个 `cl::opt` 默认开启，也不会跨越 GC 模式边界。

## 10. AsmPrinter、AArch64 与 X86 后端修改点

### 10.1 `AsmPrinter.cpp`

当前 `tryEmitCangjieSpecificCallByMOSym()` 直接枚举六个 MSGC 写屏障 wrapper 名称，还在目标无关文件中判断 Mach-O 和 `EnableMSGCInlineWriteBarrier`。这使公共调用分派同时承担了 wrapper 分类、目标能力判断和具体发射策略三项职责。

建议把公共层收敛为“识别请求并询问目标是否完成发射”：

1. 使用 `CJMSGCABI.h::classifyMSGCWriteBarrier()` 统一分类，删除本文件的重复字符串判断。
2. 引入目标无关的 `CJGCEmissionKind` 或等价请求结构，只表达 NewObject/NewArray/MSGC barrier 等语义类别，不包含 AArch64 opcode、寄存器或重定位规则。
3. 把当前空 `void` hook 改成返回实际处理结果的接口，例如：

```cpp
virtual bool tryEmitCJGCSpecificCall(
    CJGCEmissionKind Kind,
    const MachineInstr *MI,
    const MachineOperand &MOSym,
    unsigned Opcode) {
  return false;
}
```

4. 只有目标 hook 返回 `true` 时，公共 `AsmPrinter` 才认为原 call 已被替换；返回 `false` 时必须继续发射普通 call，执行 wrapper 的 slow body。
5. Mach-O、AArch64、inline 开关和具体 ABI 检查全部放到 AArch64 目标层。公共 `AsmPrinter.cpp` 不直接决定某个 MSGC 序列能否展开。
6. 新增防御性校验：被识别的 MSGC wrapper 所在 Module 必须为 MSGC。该校验只报告交叉模式错误，不能用于选择 GC，也不能在失败时改走 CopyGC emitter。

返回 `bool` 是必要的接口收紧。当前 `emitMccWriteBarrierFastPath()` 在 `AsmPrinter.h` 中是默认空实现；如果未来某个未实现 MSGC 发射的目标收到了 MSGC wrapper，公共层不能出现“识别成功、返回成功、实际一条指令也没发射”的行为。

`EnableMSGCInlineWriteBarrier` 继续只控制“已选中 MSGC wrapper 后是否展开”，不控制“当前是哪种 GC”。该开关定义和查询可以移动到 AArch64 Cangjie/MSGC 私有 helper，使发射器与尺寸估算使用同一个 accessor。当前源码注释写“默认关闭”，而实际 `cl::init(true)`，实施迁移时应同步修正注释或默认值，避免配置说明继续漂移。

### 10.2 `AArch64AsmPrinter.cpp` 分配路径

必须删除当前以下启发式：

```text
Module 中存在 CJ_MCC_NewObjectFast_MSGC_dynamic
    => InMSGCMode = true
```

改成完全由当前 wrapper 函数属性分派：

| wrapper 属性 | 发射行为 |
| --- | --- |
| `cj-msgc-sizeclass` + `cj-msgc-slotsize` | MSGC 静态 size-class emitter |
| `cj-msgc-dynamic` | MSGC 动态 emitter |
| CopyGC fast-wrapper 标记 | CopyGC emitter |
| 没有任何 fast-wrapper 标记 | 正常调用 runtime slow callee，不做 inline |

为避免把“无属性”继续解释为 CopyGC，建议给 `doCopyGCNewFastPath()` 创建的 wrapper 增加明确属性，例如 `cj-copygc-fast-alloc`，或至少要求已有 `cj-fast-new-obj` 且不带任何 MSGC 属性时才认作兼容的 CopyGC wrapper。长期方案优先使用显式的互斥属性。

这能解决当前启发式的两个错误边界：

- MSGC Module 不含动态 wrapper，但含静态分配时不再漏判。
- CopyGC Module 因链接/死代码残留了某个 MSGC 名称时不再被整体误判。

### 10.3 写屏障发射

`emitMccWriteBarrierFastPath()` 继续按 `MSGCWriteBarrierKind` 区分 ref、struct、static 和 arraycopy。修改重点不是重写发射算法，而是保证其入口只能来自已经分流的 MSGC wrapper。

Mach-O、AArch64、runtime global 取址、LARGE object mark 和 WorkBuf ABI 等现有约束保持不变。MSGC RefField 使用完整 raw 64-bit 指针；公共 CopyGC RefField 的 48-bit tagged-pointer 布局不变。不支持的 object format 必须调用 wrapper slow body。

### 10.4 `AArch64InstrInfo.cpp`

`getCangjieSpecificCallInstSizeInBytes()` 的判断必须与 AArch64 发射器使用同一个分类函数和同一组目标/开关条件：

```text
是否会 inline = wrapper kind 非 None
                && EnableMSGCInlineWriteBarrier
                && Mach-O
```

ref、struct、arraycopy 的保守尺寸可以保持现值，但统一 helper 不能只覆盖写屏障，还必须覆盖会在 AsmPrinter 阶段扩大代码体积的对象/数组分配路径：

- CopyGC object/array fast path。
- MSGC 静态 size-class object/array fast path。
- MSGC dynamic object/array fast path。
- 普通 call 与 `STATEPOINT` 两种入口。

建议提供一个 AArch64 私有的判定结果结构，由实际发射和大小估算共同消费：

```cpp
struct AArch64CJEmissionDecision {
  CJGCEmissionKind Kind;
  bool CanInline;
  unsigned MaxEmittedBytes;
};

AArch64CJEmissionDecision
classifyAArch64CJEmission(const MachineInstr &MI);
```

不得出现以下情况：

- 尺寸估算认为是 4B call，AsmPrinter 实际展开数百条指令。
- 尺寸估算认为会展开，AsmPrinter 却因另一套模式判断保留 call。

当前 array/object 的 static、dynamic 和普通 call 判定散落在 `AArch64AsmPrinter.cpp` 与 `AArch64InstrInfo.cpp` 中，覆盖范围并不完全相同。文件拆分时必须先统一这个 decision helper，再搬运发射函数；只移动代码而保留两套判断会继续留下 jump table、BranchRelaxation 和远分支风险。

### 10.5 `AsmPrinter.h` 与属性传播

`llvm/include/llvm/CodeGen/AsmPrinter.h` 继续声明 Cangjie GC 发射所需的目标 hook，但接口应采用“请求 + `bool` 处理结果”，不再提供会被公共层无条件当成成功的空 `void` 实现。接口名称也应从具体的 `emitMccWriteBarrierFastPath()` 收敛为目标无关的 `tryEmitCJGCSpecificCall()`，具体 MSGC emitter 只在 AArch64 私有实现中暴露。

这些声明不需要使用编译宏删除。LLVM 同一个构建可以同时具备 CopyGC/MSGC codegen 能力；真正隔离由输入 Module 的 GCMode、lowering 生成的互斥 wrapper 属性和目标能力完成。编译宏只适合裁剪某个发行包是否编入实现，不能替代输入 IR 的 GC 身份。

属性必须从 IR Function 保留到 `MachineFunction` 可查询阶段。新增 CopyGC/MSGC wrapper 属性时，应加入 IR round-trip、bitcode round-trip 和 CodeGen 测试，避免 LTO/internalize 后丢失。

### 10.6 当前后端文件混合审计

除 lowering 外，当前后端存在两类不同的“混合”，不能笼统理解为所有目标都同时实现了 CopyGC/MSGC：

| 位置 | 当前内容 | 混合性质 |
| --- | --- | --- |
| `llvm/lib/CodeGen/AsmPrinter/AsmPrinter.cpp` | 通用 Cangjie 调用分派、六个 MSGC wrapper 字符串、Mach-O gate、MSGC inline 开关 | 目标无关层混入 MSGC 与 AArch64/Mach-O 策略 |
| `llvm/lib/Target/AArch64/AArch64AsmPrinter.cpp` | 标准 AArch64 发射、Cangjie safepoint/stub/TLS、CopyGC 分配、MSGC 分配、MSGC 写屏障 | 目标通用、语言扩展和两种 GC 实现混在同一文件 |
| `llvm/lib/Target/AArch64/AArch64InstrInfo.cpp` | 标准 AArch64 指令信息和 MSGC barrier 展开尺寸上界 | 通用目标分析混入 MSGC code-size 策略 |
| `llvm/lib/Target/X86/X86MCInstLower.cpp` | 标准 X86 MI→MCInst lowering、Cangjie safepoint/stub、CopyGC 分配/数组 fast path | 没有 MSGC emitter，但 Cangjie/CopyGC 与标准 X86 lowering 混合 |
| `llvm/include/llvm/CodeGen/CJMSGCABI.h` | MSGC wrapper 名称、函数属性、size class、barrier kind 分类 | 合理的共享 ABI，不属于应消除的实现混合 |

写屏障需要单独说明：当前 `emitMccWriteBarrierFastPath()` 本身是纯 MSGC 实现，内部没有选择 CopyGC；CopyGC 写屏障也没有与它复用同一套 MC 指令序列。混合发生在它所在的 `AArch64AsmPrinter.cpp` 以及公共 `AsmPrinter.cpp` 的入口策略，而不是该函数内部同时实现两种 GC。

X86 与 AArch64 在目录层面本来已经分别位于 `Target/X86` 和 `Target/AArch64`，因此“拆开 X86/AArch64”不是把一个共同文件一分为二，而是继续把各目标目录中的 Cangjie 发射从标准后端大文件中抽出。当前 X86 只有 CopyGC 发射，不能因为目录设计对称就提前创建空的 X86 MSGC 实现。

### 10.7 拆分原则：目标架构为一级，GC 为二级

MC 发射层的首要差异是指令集、寄存器、调用约定、重定位和 object format，因此一级边界必须是目标架构；在单个目标内部，再按 Cangjie 公共逻辑、CopyGC 和 MSGC 做二级拆分：

```text
目标无关 CodeGen
├─ GCMode、wrapper/属性分类、共享 ABI
└─ 不包含目标 opcode、寄存器、Mach-O relocation

Target/AArch64
├─ 标准 AArch64 AsmPrinter
├─ AArch64 Cangjie 公共发射
├─ AArch64 CopyGC emitter
└─ AArch64 MSGC emitter

Target/X86
├─ 标准 X86 MC lowering/AsmPrinter
├─ X86 Cangjie 公共发射
└─ X86 CopyGC emitter
```

不建议创建一个跨目标的 `MSGCAsmPrinter.cpp`，再在其中同时实现 AArch64/X86。那会把目标寄存器、指令编码、ABI 和重定位重新混到一起。能够共享的是分类和 ABI 契约，不能共享的是实际 MC 指令序列。

推荐目录结构如下，具体文件名可以遵循目标目录现有命名习惯调整，但职责边界应保持：

```text
llvm/include/llvm/CodeGen/
├─ CJMSGCABI.h
└─ CJGCEmission.h                 # 可选：目标无关请求/分类

llvm/lib/Target/AArch64/
├─ AArch64AsmPrinter.cpp          # 标准发射与薄调度
├─ AArch64CangjieAsmPrinter.h     # 目标私有类声明/共享上下文
├─ AArch64CangjieAsmPrinter.cpp   # safepoint、stack、stub、metadata
├─ AArch64CopyGCEmitter.cpp       # CopyGC object/array fast path
├─ AArch64MSGCEmitter.cpp         # MSGC alloc/barrier/module tables
└─ AArch64CangjieCallSize.cpp     # 发射分类与保守尺寸

llvm/lib/Target/X86/
├─ X86MCInstLower.cpp             # 标准 MI→MCInst lowering
├─ X86CangjieAsmPrinter.cpp       # safepoint、stack、stub、metadata
└─ X86CopyGCEmitter.cpp           # CopyGC object/array fast path
```

### 10.8 公共 ABI、目标 ABI 与实现代码边界

拆文件后仍应保留三层清晰边界：

| 层次 | 可以包含 | 不应包含 |
| --- | --- | --- |
| `CJGCMode`/公共 emission 分类 | Module GC 模式、目标无关 emission kind、合法性校验 | AArch64/X86 opcode、runtime 布局偏移 |
| `CJMSGCABI.h` | wrapper 名称、互斥属性、size class、MSGC barrier kind、ABI version | `MCInstBuilder`、寄存器选择、Mach-O PAGE/PAGEOFF |
| `AArch64MSGCEmitter` 私有 ABI | 当前已验证的 runtime symbol、TLS/MSRegion/WorkBuf/TypeInfo 偏移、Mach-O relocation 约束 | X86 指令或假定其他目标可复用的通用实现 |

当前大量 MSGC runtime 布局偏移直接定义在 `AArch64AsmPrinter.cpp`。第一阶段可以把它们随 emitter 移入 `AArch64MSGCEmitter.cpp` 或私有 `AArch64MSGCLayout.h`；长期应通过 ABI version、生成头文件或编译器/Runtime 配对测试约束同步。不能仅靠“Runtime 侧有 static_assert”就认为编译器中的另一份数值会自动同步。

### 10.9 AArch64 具体迁移清单

`AArch64AsmPrinter` 当前完整类定义位于 `AArch64AsmPrinter.cpp` 的匿名 namespace。若要把成员函数定义放到多个 `.cpp`，不能只剪切函数体；需要先建立目标私有头文件，或者改成持有窄接口的 helper class。

推荐采用低风险的两阶段方式：

1. 先建立 `AArch64CangjieAsmPrinter.h`，把 `AArch64AsmPrinter` 的目标私有类声明和 Cangjie 发射所需小型参数结构移入该文件；成员函数仍直接访问现有 `OutStreamer`、`OutContext`、`MCInstLowering`、`SM` 和 `MF`，本阶段只改变编译单元边界。
2. 文件稳定后，再评估是否将 MSGC emitter 收敛为只接收 streamer/context/stackmap/target-info 的 helper class。首轮不强行重写所有成员访问，避免“拆文件”同时演变成大范围行为重构。

建议迁移如下：

| 目标文件 | 迁移内容 |
| --- | --- |
| `AArch64CangjieAsmPrinter.cpp` | `emitCangjieCallStubInstImpl`、throw、metadata、thread-id、stack check、TLS/GC phase、safepoint outline/inline 等两种 GC 共用的 Cangjie 发射 |
| `AArch64CopyGCEmitter.cpp` | `emitMccNewObjectForCopyGC`；`emitCJNewArrayFastPath` 中读取 CopyGC AllocBuffer/TLAB 的 object/array 序列 |
| `AArch64MSGCEmitter.cpp` | `emitMccNewObjectForMSGC`、`emitMccNewObjectForMSGCDynamic`、`emitMccWriteBarrierFastPath`、`emitCJNewArrayForMSGC`、`emitCJNewArrayForMSGCDynamic`、`emitMSGCSizeClassTables`、全部 MSGC layout/symbol 常量 |
| `AArch64CangjieCallSize.cpp` | `getCangjieSpecificCallInstSizeInBytes` 中的 Cangjie/GC 分类和展开尺寸；与 emitter 共用 decision helper |
| `AArch64AsmPrinter.cpp` | 标准 AArch64 发射、`emitInstruction` 薄调度、生命周期 hook 和对上述组件的调用 |

`emitMccNewObjectFastPath()` 与 `emitCJNewArrayFastPath()` 应保留为薄分派器，只根据已经固化在 wrapper 上的互斥属性选择 CopyGC emitter、MSGC emitter 或 slow call。它们不得再检查 Module 中是否“碰巧存在”某个 `_MSGC_dynamic` 名称。

MSGC 动态分配还包含 module 级状态 `MSGCDynamicTablesNeeded`，并在 `doFinalization()` 中发射 size-class/slot-size 表。以下内容必须作为一个整体迁移：

```text
dynamic object/array emitter
        │
        ├─ 标记 MSGCDynamicTablesNeeded
        └─ doFinalization → emitMSGCSizeClassTables
```

可以让 `doFinalization()` 留在主文件，但它只能调用 `MSGCEmitter.emitModuleDataIfNeeded()`；不能把状态留在主文件、表实现移到另一个文件后再形成双向依赖。

### 10.10 X86 具体迁移清单与约束

X86 当前没有 `CJMSGCABI.h` 依赖，也没有 MSGC barrier/object/array emitter。现有 GC 专用代码是 CopyGC：

- `emitMccNewObjectForCopyGC()`。
- `emitMccNewObjectFastPath()`。
- `emitCJNewArrayFastPath()`。

这些函数可迁入 `X86CopyGCEmitter.cpp`；stack check、safepoint、GC phase、throw、native stub 等迁入 `X86CangjieAsmPrinter.cpp`。`LowerSTATEPOINT()` 与普通 X86 指令分派仍可留在 `X86MCInstLower.cpp`，只调用拆出的 Cangjie helper。

实际拆分有一个机械障碍：`X86MCInstLower` 当前定义在 `X86MCInstLower.cpp` 的匿名 namespace，而 `X86AsmPrinter::ParamForEmitNewObj` 保存了它的引用，CopyGC emitter 和 native stub 也直接调用其 operand lowering。实施时二选一：

1. 把 `X86MCInstLower` 的声明移动到目标私有头文件，使新的编译单元能够使用；
2. 推荐方案：在调用 emitter 前先完成 MachineOperand→MCOperand/MCInst lowering，让 `X86CopyGCEmitter` 只接收已经构造好的 slow-call、符号或更窄的回调，删除参数结构对匿名 lowerer 的依赖。

第二种方案依赖面更小，也能避免为了拆 CopyGC 发射而公开整个 X86 lowering 工具。无论采用哪种方案，都不能通过 `#include "X86MCInstLower.cpp"` 或 `.inc` 拼接实现伪拆分。

目前不创建空的 `X86MSGCEmitter.cpp`。将来支持 X86 MSGC 时，必须先定义 X86 对应的 runtime ABI、寄存器约定、object format 和尺寸估算，再新增实现；不能直接复用 AArch64 的布局/MC 序列。

### 10.11 CMake 与编译隔离

新 `.cpp` 文件分别加入 `llvm/lib/Target/AArch64/CMakeLists.txt` 和 `llvm/lib/Target/X86/CMakeLists.txt` 的目标源文件列表。默认同一 LLVM target library 同时编入其 CopyGC/MSGC 能力，不使用 `#ifdef MSGC` 在编译期二选一。

构建层应满足：

- AArch64 target library 可以同时链接 CopyGC 和 MSGC emitter，但单个输入 Module 只能通过 GCMode/wrapper 属性选择其中一个。
- X86 target library 当前只链接公共 Cangjie 与 CopyGC emitter；收到不支持 inline 的 MSGC wrapper 时保留 slow call。
- `CJMSGCABI.h` 不反向 include 任何 Target 目录头文件。
- AArch64/X86 私有头文件不进入 LLVM 对外安装 API。
- 文件移动阶段不改变 command-line 开关默认值、wrapper 名称、寄存器选择和发射顺序。

### 10.12 后端拆分实施顺序

后端拆分属于后文“阶段 4：后端去启发式、文件拆分与 ABI 收口”，建议在该阶段内部按以下顺序执行：

1. 先增加互斥 wrapper 属性和统一 emission decision helper，使用测试锁定现有行为。
2. 把公共 `AsmPrinter` hook 改成返回 `bool`，确保不支持目标能够可靠保留 slow call。
3. 抽出 `AArch64MSGCEmitter.cpp`，同时迁移 layout 常量、dynamic table 状态和尺寸判定。
4. 抽出 `AArch64CopyGCEmitter.cpp`，让 object/array 分派只消费 wrapper 属性。
5. 抽出 AArch64 公共 Cangjie 发射，最后清理主 `AArch64AsmPrinter.cpp`。
6. 整理 X86 公共 Cangjie 与 CopyGC 发射；由于当前没有 X86 MSGC，不把它设为 AArch64 MSGC 隔离完成的前置条件。

每一步都应是行为保持型提交：文件拆分前后的 IR、汇编、object 反汇编和符号引用必须一致。若某一步需要改变 fast-path 语义或指令序列，应独立成后续优化提交，不与移动文件混合评审。

## 11. Runtime 与 ABI 联动要求

### 11.1 Runtime 宏隔离的边界

LLVM 与 Runtime 的隔离方式不同：

- LLVM 同一个构建可以同时包含 CopyGC/MSGC lowering 代码，使用 Module GCMode 选择，不需要把所有 MSGC C++ 代码包进构建宏。
- Runtime 构建只选择一个 GC，实现层使用 `MRT_USE_COPYGC`/`MRT_USE_MARK_SWEEP`；MSGC 专用类型、全局变量和导出符号应受 `MRT_USE_MARK_SWEEP` 隔离。
- `CJ_MCC_WriteRefField`、`CJ_MCC_WriteStructField`、arraycopy 等公共 ABI 可以保留同名，在函数内部由 Runtime 编译宏选择具体 barrier。这类公共入口不应为了形式上的隔离而复制名称。

### 11.2 当前需要补充检查的 MSGC 专用符号

当前 `Heap::inlineFastPathGCPhase` 的声明和定义已经位于 `MRT_USE_MARK_SWEEP` 下，这是正确做法。

但 `CJ_MCC_NewArrayGenericFast3_MSGC`/`MCC_NewArrayGenericFast3_MSGC` 当前可在以下位置看到未完整宏隔离的声明、定义或 alias：

- `src/CompilerCalls.h`
- `src/CompilerCalls.cpp`
- `src/CommonAlias.h`
- `src/MacAlias.h`

实施时应把该 MSGC 专用导出链统一置于 `MRT_USE_MARK_SWEEP`，并验证 CopyGC Runtime 不再导出该符号。前提是 LLVM CopyGC lowering 已恢复 `CJ_MCC_NewArrayGeneric`，否则会把编译器污染问题变成链接失败。

`Heap::heapReservedStartAddr/heapReservedEndAddr` 虽被 MSGC inline barrier 使用，但当前也是 Heap 的公共稳定范围信息；是否宏隔离应依据其他 Runtime 路径是否使用，而不能仅因 MSGC 后端引用就删除。宏隔离审计要按“符号是否专用”判断，不按文件名批量包裹。

### 11.3 wrapper slow path 必须可用

LLVM 创建的内部 `_MSGC` wrapper 必须调用 MSGC Runtime 中真实存在的公共或专用 slow helper。以下场景都依赖 slow body：

- 非 Mach-O 或非 AArch64。
- `enable-msgc-inline-write-barrier=false`。
- ABI/参数/布局不满足 fast path。
- safepointState、Region、WorkBuf、size-class 或大对象检查失败。

因此“wrapper 被后端识别”不等于可以省略 Runtime helper。

### 11.4 布局 ABI 与版本

MSGC AArch64 inline 路径直接依赖 `ThreadLocalData`、`AllocBufferMS`、`MSRegionInfo`、WorkBuf 和 size-class 表的偏移/常量。现有 Runtime `static_assert` 必须保留，并建议在 `CJMSGCABI.h` 增加一个 ABI 版本属性或常量，例如 `cj-msgc-abi-version`。

如果布局或 barrier 协议变化：

1. 同时提升 LLVM 与 Runtime ABI 版本。
2. 重新构建 compiler、runtime 和 stdlib。
3. 旧 wrapper/bitcode 不得静默链接到新 Runtime。

GCMode 解决“选择哪个 GC”，ABI version 解决“同一种 GC 的编译器与 Runtime 布局是否匹配”，两者不能混为一个标记。

### 11.5 三件套一致性

最终 SDK 中 compiler、runtime、stdlib 必须来自相同 GC 模式：

| 组件 | 必须满足 |
| --- | --- |
| compiler | 生成正确 `Cangjie_GCMode` 并执行对应 lowering |
| runtime | 由对应 `MRT_USE_*` 宏构建，导出该模式所需 ABI |
| stdlib | 使用同一 compiler/runtime 模式重新编译 |

仅替换 runtime dylib 不能切换由 compiler 固化在对象文件中的 GC lowering。替换后继续使用旧 `.cj` binary 会造成 ABI 不匹配，必须重新编译用例。

## 12. 测试与验证矩阵

### 12.1 测试原则

每项 MSGC 正向测试都应有一个 CopyGC 负向测试。不能只检查“MSGC wrapper 出现了”，还要检查“CopyGC 中没有出现”。建议使用前缀：

```text
CHECK-COPY
CHECK-MSGC
CHECK-COPY-NOT
CHECK-MSGC-NOT
```

手写 `.ll` 必须显式写 GCMode，避免测试依赖缺省行为。

### 12.2 Module flag 与链接测试

| 用例 | 预期 |
| --- | --- |
| `Cangjie_GCMode=0` | helper 返回 CopyGC |
| `Cangjie_GCMode=1` | helper 返回 MSGC |
| 缺失 flag | 普通 Pass 按 CopyGC 兼容 |
| 非整数或值为 2 | 明确诊断 |
| `llvm-link`：0 + 0 | 成功 |
| `llvm-link`：1 + 1 | 成功 |
| `llvm-link`：0 + 1 | 因 `Module::Error` 失败 |
| MSGC LTO + 缺失 flag 的 CJBC | 严格输入检查失败，要求重编译 |

### 12.3 Pass 级测试

| 文件/Pass | CopyGC 检查 | MSGC 检查 |
| --- | --- | --- |
| `CJGenericIntrinsicOpt` | fresh/仅栈目标不被错误 memcpy；栈到栈/无引用公共证明仍生效 | 与 CopyGC 相同；不再为 fresh/栈目标启用专项折叠 |
| `CJBarrierOpt` | fresh-init 仍保留 barrier；单引用 struct 仍保留 struct barrier | 与 CopyGC 相同；不再启用 fresh-init 消除或单引用 struct 标量化 |
| `CJRuntimeLowering` | generic array 调用 `CJ_MCC_NewArrayGeneric`；无 `_MSGC` | 生成 `Fast3_MSGC` dispatch/wrapper |
| `CJBarrierLowering` 读侧 | 保留 CopyGC read/tagged path | ref load、struct memcpy |
| `CJBarrierLowering` 写侧 | 无 `Write*Fast_MSGC` | AArch64 生成正确 MSGC wrapper |
| statepoint 后对象分配 | `CJ_MCC_NewObjectFast` CopyGC wrapper | `_MSGC_cls*`/`_MSGC_dynamic` wrapper |
| 非 AArch64 MSGC | 不生成 AArch64 inline ABI | 调用正确 slow helper |

现有 17 个 MSGC lit 文件应先补 flag，再增加对应 CopyGC 文件或在同一文件使用双 `RUN`。重点覆盖当前已有的 gcread/gcwrite generic、fresh barrier、new array、Mach-O global gate、calling convention 和 inline/no-inline 用例。

### 12.4 CodeGen 与 object 级测试

不能只检查 LLVM IR 或文本汇编，还需要：

1. `llc -filetype=asm`：确认 wrapper 选择和符号名。
2. `llc -filetype=obj | llvm-objdump -d -`：确认真实 AArch64 编码，特别是 `tbz/tbnz` 位号和跳转目标。
3. Mach-O：确认外部 Runtime 数据符号使用正确 GOT/relocation 形式。
4. `AArch64InstrInfo`：分别覆盖 ref/struct/arraycopy、CopyGC object/array、MSGC static object/array、MSGC dynamic object/array，构造包含大量展开点和 jump table/远分支的用例，确认尺寸估算不会导致分支落入 inline 序列中部。
5. 关闭 `enable-msgc-inline-write-barrier`：汇编必须保留 wrapper call，不能出现 CopyGC inline 代码。
6. AArch64 CopyGC：拆文件前后 object/array fast path 的指令序列、stackmap 和 slow-call target 保持一致，且不引用 MSRegion/WorkBuf/size-class table。
7. AArch64 MSGC：静态和动态分配、六类 barrier、module table 各自只由 `AArch64MSGCEmitter` 发射；未使用 dynamic emitter 的 Module 不生成 size-class table。
8. X86 CopyGC：拆文件前后 Linux/Windows 调用约定、寄存器和 slow-call relocation 保持一致。
9. X86/其他未支持目标：人为输入 MSGC wrapper 时必须保留可执行 slow call；不得因基类空 hook 导致调用消失，也不得尝试发射 AArch64 序列。
10. 编译单元测试：AArch64/X86 target library 新增源文件后均能独立构建，且目标私有头文件没有进入公共安装接口。

### 12.5 Runtime 符号与宏测试

分别构建 CopyGC 和 MSGC Runtime，并使用 `nm`/`llvm-nm` 检查：

| 符号 | CopyGC | MSGC |
| --- | --- | --- |
| 公共 `CJ_MCC_Write*`/arraycopy | 存在 | 存在 |
| `Heap::inlineFastPathGCPhase` | 不存在 | 存在 |
| `CJ_MCC_NewArrayGenericFast3_MSGC` | 不存在 | 存在 |

再对 LLVM 产物做交叉检查：CopyGC object 不得 undefined-reference 任何 MSGC 专用符号；MSGC fast-path object 必须引用预期的 MSGC ABI。

### 12.6 端到端 SDK 矩阵

至少覆盖：

| 编译模式 | Runtime | 结果 |
| --- | --- | --- |
| CopyGC | CopyGC | 编译、链接、运行通过 |
| MSGC | MSGC | 编译、链接、运行通过 |
| CopyGC | MSGC | 不作为支持组合；应通过 ABI/version/流程检查尽早拒绝 |
| MSGC | CopyGC | 链接缺少 MSGC 专用符号或版本检查失败，不能静默运行 |

每种模式都测试 O0/O2、对象/数组/generic 分配、ref/struct/static/arraycopy barrier、强制 GC 和高频分配。MSGC 重点跑现有正确性/压力用例；CopyGC 重点确认性能与生成代码回到修改前基线。

多 SDK 验证必须使用隔离的 `env -i` 子 shell，先用 `DYLD_PRINT_LIBRARIES=1` 确认实际加载的 `libcangjie-runtime.dylib`。compiler、runtime、stdlib 三件套来源必须记录。每次替换 dylib 后都重新用对应 SDK 编译 `.cj`，不得复用旧 binary。

### 12.7 最小检查命令示例

```bash
# 前端公共 intrinsic 与 Module flag
cjc --gc-mode=copygc -emit-llvm -S test.cj -o copy.ll
cjc --gc-mode=marksweep -emit-llvm -S test.cj -o msgc.ll
rg 'Cangjie_GCMode|llvm.cj.(gcread|gcwrite|malloc)' copy.ll msgc.ll

# lowering 后符号隔离
llc -cangjie-pipeline copy.ll -o copy.s
llc -cangjie-pipeline msgc.ll -o msgc.s
rg '_MSGC|cj-msgc' copy.s msgc.s

# Runtime 导出符号隔离
nm -gU <copygc-runtime> | rg 'MSGC|inlineFastPathGCPhase'
nm -gU <msgc-runtime> | rg 'MSGC|inlineFastPathGCPhase'
```

实际选项和工具路径以实现后的构建系统为准；验证报告必须保存退出码和结果文件，不能只记录命令。

## 13. 分阶段实施计划

### 阶段 1：模式契约与前端传递

修改 cjc 选项、`CGModule.cpp` 和 LLVM `CJGCMode` helper，增加 Module flag/IR Link 单元测试。此阶段不改变任何 lowering 输出。

完成条件：同一 `.cj` 的两份初始 IR 仅 GCMode 不同，公共 intrinsic 保持一致；0+1 链接冲突测试通过。

### 阶段 2：先恢复 CopyGC 基线

在 `CJRuntimeLowering` 和 `CJBarrierLowering` 增加模式 gate。`CJGenericIntrinsicOpt` 的 fresh/栈目标专项折叠，以及 `CJBarrierOpt` 的 fresh-init 消除/单引用 struct 标量化，都已通过删除恢复到 `dev`/MSGC 修改前逻辑，无需再为它们增加 gate。CopyGC 路径优先恢复：

- `CJ_MCC_NewArrayGeneric` 映射。
- `CJ_MCC_NewObjectFast`/`CJ_MCC_NewFinalizerFast` wrapper。
- legacy read/write phase/tagged fast path。
- 不出现 `_MSGC` wrapper/属性。

完成条件：CopyGC 全部负向 lit 通过，并完成一次官方 CopyGC SDK 基线用例验证。

### 阶段 3：接回 MSGC lowering

在显式 `GCMode=MSGC` 分支中接回现有读屏障消除、写屏障 wrapper 和 object/array size-class wrapper。generic/barrier 优化只接入独立证明安全且确有收益的项，不恢复本次撤销的 fresh/栈目标 `gcread_generic` 折叠、fresh-init barrier 消除或单引用 struct 标量化。

完成条件：现有 17 个 MSGC 测试纳入版本管理、补齐 mode flag 并通过；关闭各 fast-path 开关时仍走 MSGC 正确 slow path。

### 阶段 4：后端去启发式、文件拆分与 ABI 收口

删除 `AArch64AsmPrinter` 的 `InMSGCMode` 符号存在性判断，增加显式 CopyGC/MSGC wrapper 属性；统一实际发射与 `AArch64InstrInfo` 的 emission decision/尺寸 predicate。公共 `AsmPrinter` hook 改为返回是否真正完成发射，目标不支持时保留 wrapper slow call。

在判定逻辑稳定后，按“目标架构一级、GC 二级”完成后端文件拆分：

- AArch64：公共 Cangjie、CopyGC emitter、MSGC emitter、Cangjie call size 分离。
- X86：公共 Cangjie 与 CopyGC emitter 从 `X86MCInstLower.cpp` 分离；不创建空 MSGC emitter。
- `MSGCDynamicTablesNeeded`、module size-class table、MSGC layout 常量与 MSGC emitter 同生命周期迁移。
- CMake 同时编入目标已支持的 emitter，不用编译宏代替输入 Module 的 GCMode。

完成条件：IR/bitcode 属性 round-trip、Mach-O 汇编、object 反汇编、jump table/分支距离测试通过；拆分前后 AArch64 CopyGC、AArch64 MSGC、X86 CopyGC 指令序列和 stackmap 行为保持一致；X86/未支持目标输入 MSGC wrapper 时保留 slow call。

### 阶段 5：Runtime 宏与 SDK 全链路验证

宏隔离 `NewArrayGenericFast3_MSGC` 的声明、定义和 alias，保留公共 Runtime ABI；分别构建两种 Runtime 并检查符号表。最后按隔离环境重新构建 compiler/runtime/stdlib 和测试用例。

完成条件：两套正确组合运行通过，两套错误组合能够尽早失败，且验证报告明确记录三件套来源。

### 预计修改文件清单

| 仓库 | 文件/区域 | 修改类型 |
| --- | --- | --- |
| cangjie compiler | `include/cangjie/Option/Option.h` | 新增 GCMode 枚举/字段 |
| cangjie compiler | `include/cangjie/Option/Options.inc`、选项解析实现 | 新增 `--gc-mode` 并校验 |
| cangjie compiler | `src/CodeGen/CGModule.cpp` | 写入 `Cangjie_GCMode` Module flag |
| cangjie compiler | 增量/bitcode cache 配置摘要 | 将 GCMode 纳入缓存键 |
| LLVM | `llvm/include/llvm/IR/CJGCMode.h`（及可选 `.cpp`） | 统一模式定义、读取和校验 |
| LLVM | `llvm/lib/LTO/LTO.cpp` | Cangjie 输入 Module 的严格混合模式检查 |
| LLVM | `llvm/lib/Transforms/Scalar/CJGenericIntrinsicOpt.cpp` | 已撤销 fresh/栈目标专项折叠；保留 GC 无关的公共证明，未来仅对新的 GC 相关优化做模式分流 |
| LLVM | `llvm/lib/Transforms/Scalar/CJBarrierOpt.cpp` | 已撤销 fresh-init 消除/单引用 struct 标量化；保留 `dev` 基线公共优化，未来仅对新的 GC 相关优化做模式分流 |
| LLVM | `llvm/lib/Transforms/Scalar/CJRuntimeLowering.cpp` | CopyGC/MSGC array/runtime callee 分流 |
| LLVM | `llvm/lib/CodeGen/CJBarrierLowering.cpp` | 读写屏障、legacy fast path、statepoint 分配分流 |
| LLVM | `llvm/include/llvm/CodeGen/CJMSGCABI.h` | 保持 wrapper 分类集中，补充必要 ABI 属性/版本 |
| LLVM | `llvm/include/llvm/CodeGen/AsmPrinter.h`、`llvm/lib/CodeGen/AsmPrinter/AsmPrinter.cpp` | 使用公共 wrapper 分类；目标 hook 返回实际处理结果；去除目标/Mach-O 策略和重复名称判断 |
| LLVM | `llvm/include/llvm/CodeGen/CJGCEmission.h`（可选） | 定义目标无关 emission kind/request，不包含目标 opcode |
| LLVM | `llvm/lib/Target/AArch64/AArch64AsmPrinter.cpp` | 删除符号存在性模式推断，保留标准发射与薄调度 |
| LLVM | `llvm/lib/Target/AArch64/AArch64CangjieAsmPrinter.h/.cpp`（新增） | 目标私有类声明与 GC 共用的 Cangjie 发射 |
| LLVM | `llvm/lib/Target/AArch64/AArch64CopyGCEmitter.cpp`（新增） | CopyGC object/array fast path |
| LLVM | `llvm/lib/Target/AArch64/AArch64MSGCEmitter.cpp`（新增） | MSGC alloc、barrier、layout 常量与 module table |
| LLVM | `llvm/lib/Target/AArch64/AArch64CangjieCallSize.cpp`（新增或并入私有 helper） | 与发射器统一 emission decision 和尺寸估算 |
| LLVM | `llvm/lib/Target/AArch64/AArch64InstrInfo.cpp` | 调用统一 helper，不再维护另一套 MSGC inline 判定 |
| LLVM | `llvm/lib/Target/X86/X86MCInstLower.cpp` | 保留标准 lowering 与薄调用，移出 Cangjie/CopyGC 大段发射 |
| LLVM | `llvm/lib/Target/X86/X86CangjieAsmPrinter.cpp`（新增） | X86 Cangjie stack/safepoint/stub 等公共发射 |
| LLVM | `llvm/lib/Target/X86/X86CopyGCEmitter.cpp`（新增） | X86 CopyGC object/array fast path |
| LLVM | `llvm/lib/Target/AArch64/CMakeLists.txt`、`llvm/lib/Target/X86/CMakeLists.txt` | 编入新增目标私有源文件，不按 GC 模式互斥构建 |
| Runtime | `src/CompilerCalls.h/.cpp` | 宏隔离 MSGC 专用 generic array 符号 |
| Runtime | `src/CommonAlias.h`、`src/MacAlias.h` | 宏隔离对应 alias |
| 测试 | `llvm/test/Transforms/*`、`llvm/test/CodeGen/AArch64/*` | 双模式 lit、LTO、汇编与 object 测试 |

这是设计阶段的预计清单。实施时若源码审计证明某个 Pass 完全不依赖 GC 算法，可以只补测试而不修改代码；反之发现新的模式相关创建点，必须加入清单和本文记录。

### 实施顺序约束

阶段 2 必须先让 CopyGC 路径可独立验证，再继续后端清理。Runtime 删除 MSGC 符号不能早于 LLVM CopyGC lowering 修复，否则会产生与设计无关的链接失败噪声。阶段 4 必须先统一 wrapper 属性、目标 hook 和 emission decision，再移动文件；否则很难区分行为回归来自判定变化还是编译单元拆分。每个阶段都应保持可回退，不把全部改动压成一次大提交。

## 14. 风险、回退与兼容策略

### 14.1 主要风险

| 风险 | 后果 | 防护 |
| --- | --- | --- |
| 新 cjc 忘记写 flag | MSGC 被当作 CopyGC | 新版 cjc 无条件写 flag；MSGC SDK/LTO 严格检查 |
| 旧无标记 bitcode 混入 MSGC LTO | 部分函数采用 CopyGC ABI | MSGC LTO 拒绝缺失标记的 CJBC |
| 某个早期 Pass 未检查模式 | wrapper 生成前已破坏语义 | Pass 清单审计 + 双模式负向测试 |
| 关闭 MSGC 优化后退到 CopyGC | 对象布局/barrier 错误 | 模式和 feature flag 分离；关闭只走 MSGC slow path |
| Module 中残留 MSGC 符号触发误判 | CopyGC 被错误 inline | 删除所有符号存在性 GC 推断 |
| wrapper 属性在 LTO 中丢失 | 误走普通或 CopyGC 发射 | bitcode/LTO round-trip 测试；无属性默认 slow call |
| AsmPrinter 与 InstrInfo 条件不同 | jump table/远分支控制流破坏 | 共享 predicate + 大函数 object 测试 |
| 公共 AsmPrinter 空 hook 被当作成功 | 未支持目标上的 wrapper call 被静默吞掉 | hook 返回 `bool`；只有目标确认完成发射才删除原 call |
| 拆文件时遗漏 MSGC module 状态 | dynamic 分配引用缺失的 size-class table，或无用 Module 误发表 | `MSGCDynamicTablesNeeded`、table emitter、`doFinalization` 作为一个组件迁移 |
| X86 emitter 继续依赖匿名 lowerer | 新编译单元无法合法访问类型，或通过 include `.cpp` 形成伪拆分 | 预先 lower operand/slow call，或建立目标私有头文件；禁止 include 实现文件 |
| 文件移动夹带指令语义修改 | 无法判断回归来自重构还是优化 | 拆分提交保持行为不变，优化序列另开提交并单独验证 |
| compiler/runtime 布局版本不一致 | 静默内存破坏 | ABI version、Runtime static_assert、SDK 三件套验证 |
| 非 Mach-O/非 AArch64 误用 inline | 重定位或布局错误 | target predicate；不支持平台保留 wrapper slow call |

### 14.2 缺省必须 fail-safe

所有无法确认模式或 wrapper ABI 的情况遵循：

- 旧/缺失 Module flag：按 CopyGC 兼容，但禁止产生新的 MSGC wrapper。
- 未知 flag：编译失败。
- MSGC wrapper 属性不完整：不 inline，调用其 slow body；Verifier 同时报错或警告升级为错误。
- 目标架构/对象格式不支持：不 inline，保持 MSGC slow call。
- ABI version 不匹配：失败，不尝试以 CopyGC emitter 兜底。

### 14.3 回退策略

1. Module flag 与公共 helper 可以独立保留，它们本身不改变代码生成。
2. 每个 MSGC 优化仍保留 feature flag，出现回归时可关闭该优化并走 MSGC slow path。
3. CopyGC 路径以 `dev`/修改前实现为回归基线，可单独回退某个 Pass 的模式分支。
4. 后端启发式一旦删除，不建议回退为符号猜测；若 wrapper 属性传播有问题，应临时禁用 inline，而不是恢复不可靠推断。
5. Runtime MSGC 专用符号宏隔离若导致预期外依赖，先恢复符号导出并定位引用方；不得为了链接通过让 CopyGC lowering 继续生成 MSGC callee。

## 15. 验收标准

### 15.1 代码结构

- [x] 仓颉前端没有新增 MSGC intrinsic，公共 intrinsic 生成点保持统一。
- [x] 新 cjc 对所有 Cangjie Module 显式写 `Cangjie_GCMode`。
- [x] LLVM 只有一个 GCMode 解析/校验实现。
- [x] 所有 `_MSGC` wrapper/属性创建点都被 Module GCMode 判定支配。
- [x] `AArch64AsmPrinter` 不再通过函数是否存在推断 GC 模式。
- [x] CopyGC 与 MSGC 分配 fast path 是显式的两个分支。
- [x] 公共 `AsmPrinter` 不再直接包含 Mach-O/AArch64 MSGC 发射策略，目标 hook 只有实际发射成功才返回 `true`。
- [x] AArch64 公共 Cangjie、CopyGC、MSGC 和尺寸分类已按目标私有文件拆分；MSGC layout/table 状态没有残留在主文件形成反向依赖。
- [x] X86 Cangjie/CopyGC 发射已从标准 MC lowering 大段实现中分离，且未创建无实现的 X86 MSGC emitter。
- [x] AArch64/X86 新增源文件通过目标 CMake 正常编入，未依赖 `#include` `.cpp`/`.inc` 的伪拆分。
- [x] feature flag 不再承担 GC 模式选择。

### 15.2 IR 与 CodeGen

- [x] 同一份初始 IR 除 Module flag 外使用相同公共 intrinsic。
- [x] CopyGC lowering 结果不含 `_MSGC`、`cj-msgc-*` 或 MSRegion/TLS 专用访问。
- [x] MSGC lowering 在支持场景生成预期 wrapper，在不支持场景走 MSGC slow helper。
- [x] CopyGC read/write legacy fast path 行为恢复到修改前基线。
- [x] O0/O2 都不会因 Pass 是否运行而改变 GC 模式选择。
- [x] AsmPrinter 实际展开与 AArch64InstrInfo 尺寸估算条件一致。
- [x] CopyGC object/array、MSGC static/dynamic object/array 和六类 barrier 的发射/尺寸判定均由同一 emission decision 覆盖。
- [x] X86/其他未支持目标遇到 MSGC wrapper 时保留 slow call，不吞调用、不发射 AArch64 序列。

### 15.3 链接与 Runtime

- [x] 显式 CopyGC/MSGC bitcode 混合链接失败。
- [x] MSGC LTO 拒绝无标记旧 CJBC。
- [x] CopyGC Runtime 不导出 MSGC 专用 generic array/phase 符号。
- [x] MSGC Runtime 导出 generic-array wrapper slow path 和 inline phase gate 所需符号。
- [x] 本次涉及的 compiler/runtime 布局常量由 Runtime `static_assert` 保护；本次未改变布局 ABI，无需提升 ABI version。

### 15.4 运行验证

- [ ] CopyGC SDK 重新编译并运行对象、数组、generic 和 barrier 用例。
- [ ] MSGC SDK 重新编译并运行相同用例及现有压力用例。
- [ ] 每次验证记录 compiler、runtime、stdlib 来源和实际 dylib 路径。
- [ ] CopyGC 性能与生成代码没有因 MSGC 默认开关退化。
- [ ] MSGC fast path 开启/关闭两种状态都正确，关闭后只退到 MSGC slow path。

只有以上项目全部满足，才能把本文状态从“设计方案”更新为“已实施并验证”。

## 16. 最终结论

针对“能否在公共 `cj` intrinsic 向 wrapper 切换时区分”的问题，结论是：**可以，而且这是推荐方向，但必须增加显式 Module GCMode，不能只靠 wrapper 名称或某个开关。**

最终边界应当是：

```text
前端语义层：公共 llvm.cj.* intrinsic
             + Cangjie_GCMode Module flag

LLVM 优化/lowering 层：先按 GCMode 判断合法变换
                      再生成 CopyGC call 或 MSGC wrapper

目标后端：只消费已经选定的 wrapper/属性
          不再猜测当前 GC

Runtime：公共 ABI 保持共享
         MSGC 专用实现和符号用 MRT_USE_MARK_SWEEP 隔离
```

因此不需要“生成 intrinsic 时生成 MSGC intrinsic 或 CopyGC intrinsic”。真正要修改的是 GC 模式的传递、所有依赖 GC 语义的早期优化、runtime/barrier lowering、statepoint 后分配分流，以及后端启发式判断。完成这些点后，一套 LLVM 可以安全处理两种公共 IR，并在 lowering 边界形成可验证、可回退的 CopyGC/MSGC 隔离。

后端物理结构在此基础上采用“目标架构一级、GC 实现二级”：AArch64 与 X86 保留各自独立的 MC 发射实现，AArch64 内再拆分公共 Cangjie、CopyGC、MSGC 和尺寸判定，X86 当前只拆公共 Cangjie 与 CopyGC。文件拆分不承担 GC 选择；它用于约束依赖和评审边界，真正的语义选择仍由 GCMode、互斥 wrapper 属性和目标 `bool` hook 完成。

## 17. 2026-07-21 第一批实施记录：AArch64 行为保持型物理拆分

本批已经完成 AArch64 后端的第一层物理隔离，但没有提前实施 GCMode、互斥 wrapper 属性、公共 `AsmPrinter` 的 `bool` hook 或 emission decision。目标是先把原有指令序列原样迁入独立编译单元，为后续语义分流缩小修改面。

### 17.1 已完成的文件调整

| 文件 | 本批结果 |
| --- | --- |
| `llvm/lib/Target/AArch64/AArch64AsmPrinter.h` | 新增目标私有头文件，承载完整 `AArch64AsmPrinter` 类声明、发射参数结构和当前 module 状态 |
| `llvm/lib/Target/AArch64/AArch64MSGCEmitter.cpp` | 独立承载 MSGC ABI/layout 常量、静态/动态对象分配、静态/动态数组分配、六类写屏障和 size-class table 发射 |
| `llvm/lib/Target/AArch64/AArch64CopyGCEmitter.cpp` | 独立承载 CopyGC 对象和数组 TLAB/AllocBuffer fast path |
| `llvm/lib/Target/AArch64/AArch64AsmPrinter.cpp` | 删除上述大段实现，保留标准 AArch64/Cangjie 发射、现有判定逻辑和薄分派器 |
| `llvm/lib/Target/AArch64/CMakeLists.txt` | 同时编入 CopyGC/MSGC 两个 emitter，不使用 GC 编译宏二选一 |

具体迁移结果：

- `emitMccNewObjectForCopyGC()`、新拆出的 `emitCJNewArrayForCopyGC()` 位于 `AArch64CopyGCEmitter.cpp`。
- `emitMccNewObjectForMSGC()`、`emitMccNewObjectForMSGCDynamic()`、`emitMccWriteBarrierFastPath()`、`emitCJNewArrayForMSGC()`、`emitCJNewArrayForMSGCDynamic()` 和 `emitMSGCSizeClassTables()` 位于 `AArch64MSGCEmitter.cpp`。
- `emitMccNewObjectFastPath()` 与 `emitCJNewArrayFastPath()` 仍位于主文件，继续执行拆分前完全相同的选择顺序；CopyGC 数组 fallback 只改为调用 `emitCJNewArrayForCopyGC()`。
- `MSGCDynamicTablesNeeded` 目前仍是 `AArch64AsmPrinter` 的目标私有成员，`doFinalization()` 仍在主文件中调用已经拆出的 `emitMSGCSizeClassTables()`。本批没有引入独立 emitter 对象或改变 module 生命周期。

### 17.2 本批明确没有修改的语义

以下内容均保持拆分前行为，留给 Step46 后续语义批次：

1. 没有新增 CopyGC/MSGC intrinsic，前端仍生成公共 `llvm.cj.*` intrinsic。
2. 没有新增或读取 `Cangjie_GCMode` Module flag。
3. 没有删除 `AArch64AsmPrinter` 通过 `_MSGC_dynamic` 符号存在性推断模式的兼容逻辑。
4. 没有增加 CopyGC/MSGC 互斥 wrapper 属性，也没有改变 wrapper 名称、调用约定或 runtime ABI。
5. 没有修改 `AsmPrinter.cpp` hook、`AArch64InstrInfo.cpp` 的尺寸判定、任何 fast-path 开关默认值或实际指令顺序。
6. 没有开始 X86 文件拆分；X86 仍只有现有 Cangjie/CopyGC 发射，没有创建空 MSGC emitter。

因此，本批完成的是“编译单元隔离”，不是“GC 模式选择隔离”。同一 AArch64 target library 仍同时包含两种 emitter，当前调用哪个实现仍由拆分前判定决定。

### 17.3 测试基线修正与新增覆盖

`cangjie-writebarrier-msgc-fastpath.ll` 原来的第三条 `RUN` 使用默认开关，却以 `DEFAULT-OFF` 命名并只检查 wrapper call。源码实际默认值为 `true`，而 inline slow 分支本身也含 wrapper call，导致该检查不能证明默认关闭。本批将其改成显式：

```text
-enable-msgc-inline-write-barrier=false
```

并使用 `NO-INLINE` 前缀检查六类 wrapper call。该修改只消除测试歧义，不改变默认值。

另外新增 `cangjie-copygc-fastpath-file-split.ll`，同时覆盖：

- CopyGC `CJ_MCC_NewObject` 的 object AllocBuffer fast path。
- `CJStatepointID::NewArrayFast` 的 CopyGC array AllocBuffer fast path。
- 两条路径的 slow-call target 仍分别为 `CJ_MCC_NewObject` 和 `CJ_MCC_NewArray`，生成代码不出现 MSGC 标识。

### 17.4 已完成验证

验证结果如下：

| 验证 | 结果 |
| --- | --- |
| `LLVMAArch64CodeGen` 与 `llc` 重新编译/链接 | 通过；两个新 `.cpp` 均作为独立 object 编译 |
| 全部 17 个现有 `*msgc*` lit + 新增 CopyGC 拆分测试 | `18/18 PASS` |
| `llvm/test/CodeGen/AArch64/CangjieGC` | `5 PASS + 1 XFAIL`；`atomic.ll` 仍为既有 expected failure |
| `python3 build.py build -t release --no-tests` | 通过 |
| `git diff --check` | 通过 |

这些验证说明新编译单元能够链接，现有 MSGC static/dynamic alloc、六类 barrier、dynamic table 以及 CopyGC object/array fast path 均仍可到达。由于本批没有改变 lowering、ABI 或 runtime，未替换 SDK runtime，也未运行 runtime 端到端用例。

### 17.5 后续批次边界

下一批不能把“文件已经拆开”等同于“CopyGC/MSGC 已经安全分流”，仍需按本文前述方案依次完成：

1. 增加 `Cangjie_GCMode` 与统一解析/校验，并让前端写入 Module flag。
2. 恢复 CopyGC lowering 基线，再在显式 MSGC 模式中创建 MSGC wrapper。
3. 增加互斥 wrapper 属性、统一 AArch64 emission decision 和尺寸估算，删除 `_MSGC_dynamic` 符号存在性推断。
4. 把公共 `AsmPrinter` hook 改为“实际发射成功才返回 `true`”，不支持目标保留 slow call。
5. 再拆 AArch64 公共 Cangjie 发射和尺寸判定；随后独立处理 X86 Cangjie/CopyGC 拆分。
6. 最后完成 Runtime 专用符号宏隔离和 CopyGC/MSGC 两套 SDK 的三件套验证。

第一批的可回退边界清晰：如果新编译单元出现问题，可以仅回退类声明、CMake 和函数位置移动，不需要同时回退任何 GC 语义或 ABI 修改。

## 18. 2026-07-21 第二批实施记录：后端 hook 收紧、公共发射拆分与剩余边界

本节记录第二批 Stage 4 后端实现，重点是实际处理结果可回退、AArch64/X86 编译单元隔离和后续阻塞项审计。

### 18.1 公共 AsmPrinter 与 AArch64 barrier decision

本批完成了写屏障 wrapper 入口的接口收紧：

1. `AsmPrinter.cpp` 不再手写枚举六个 MSGC wrapper 名称，统一调用 `CJMSGCABI.h::classifyMSGCWriteBarrier()`。
2. `AsmPrinter.h::emitMccWriteBarrierFastPath()` 从默认空实现的 `void` hook 改为返回 `bool`。公共层只有在目标 hook 返回 `true` 时才吞掉原 call；返回 `false` 时继续走正常 call 发射。
3. Mach-O、inline 开关和 AArch64 emitter 能力判断从公共 `AsmPrinter.cpp` 移到 AArch64 私有 decision helper。`EnableMSGCInlineWriteBarrier` 也移入 `AArch64CangjieCallSize.cpp`，默认值仍为 `true`，没有改变命令行开关语义。
4. AArch64 emitter 与 `AArch64InstrInfo` 共同调用 `classifyAArch64MSGCWriteBarrier(MI, CallTarget)`，共享 wrapper kind、Mach-O gate、inline gate 和 512/1536/64 字节上界。
5. helper 显式接收调用方已经解析好的 `MachineOperand &CallTarget`，不能自行假定 `MI.getOperand(0)` 是 callee。这样普通 call 和 `STATEPOINT` 使用同一个真实 call target，不会再次出现发射器与尺寸估算分叉。

这项修改包含一个有意的正确性修复，而不只是文件移动：旧代码在 X86 Mach-O 遇到手写或 LTO 残留的 MSGC wrapper 时，会调用基类空 `void` hook 并错误地认为 call 已发射，最终把调用删除；新接口会得到 `false` 并保留 wrapper slow call。正常 lowering 当前仍只在 AArch64 生成该 wrapper，但后端现在能正确处理异常输入。

本批统一的是 **MSGC 写屏障** 的实际发射 predicate 和尺寸 predicate。对象/数组分配的全部 emission decision 尚未统一，不能把当前 `AArch64MSGCWriteBarrierDecision` 误称为覆盖所有 Cangjie call 的统一 decision。

### 18.2 AArch64 与 X86 文件拆分结果

当前目标目录形成以下物理边界：

| 目标 | 文件 | 本批职责 |
| --- | --- | --- |
| AArch64 | `AArch64AsmPrinter.cpp` | 标准 AArch64 lowering/发射、`LowerSTATEPOINT`、`emitInstruction` 薄调用和 `doFinalization()` 生命周期 |
| AArch64 | `AArch64CangjieAsmPrinter.cpp` | stack check/grow、TLS/GC phase、safepoint、thread id、metadata、FFI stub、throw，以及 object/array 的跨 GC 薄分派 |
| AArch64 | `AArch64CangjieCallSize.h/.cpp` | 完整 `getCangjieSpecificCallInstSizeInBytes()` 实现、MSGC barrier capability/size decision、inline 开关所有权 |
| AArch64 | `AArch64CopyGCEmitter.cpp` | CopyGC object/array AllocBuffer fast path |
| AArch64 | `AArch64MSGCEmitter.cpp` | MSGC static/dynamic object/array、六类 barrier、ABI/layout 常量、size-class table 和 pending module data |
| X86 | `X86MCInstLower.cpp` | 标准 X86 MI→MCInst lowering、`LowerSTATEPOINT`/`emitInstruction` 及需要匿名 lowerer 的窄 bridge |
| X86 | `X86CangjieAsmPrinter.cpp` | stack check/grow、TLS/GC phase、safepoint、FP16 CJ ABI 适配、epilogue、thread id、metadata 和 FFI 栈布局 |
| X86 | `X86CopyGCEmitter.cpp` | Linux/Windows CopyGC object/finalizer/array AllocBuffer fast path |

X86 的 `X86MCInstLower` 仍位于匿名 namespace，因此没有为了拆文件而把整个 lowerer 暴露到目标私有头。处理方式是：

- 主文件先把 slow call、finalizer target 和 array callee lowering 成 `MCInst`；
- `X86CopyGCEmitter.cpp` 只消费已经 lowering 的 `MCInst`；
- `SM.recordCJStackMap()` 仍在主文件 bridge 中、完整 emitter 返回之后执行，次数和顺序不变；
- 没有使用 `#include "X86MCInstLower.cpp"` 或 `.inc` 伪拆分。

X86 仍然没有 MSGC emitter，也没有创建空的 `X86MSGCEmitter.cpp`。它只编入公共 Cangjie 发射和 CopyGC emitter；遇到 MSGC wrapper 时保留普通 slow call。

`MSGCDynamicTablesNeeded` 仍是 `AArch64AsmPrinter` 的目标私有状态，但其读取、置位和实际表发射已经全部收口在 `AArch64MSGCEmitter.cpp`：

- dynamic object/array emitter 负责置位；
- `emitPendingMSGCModuleData()` 负责读取状态并发射表；
- 主文件 `doFinalization()` 只调用该窄方法，再继续 `AsmPrinter::doFinalization(M)`。

这样既保留基类生命周期顺序，也避免主文件直接理解 MSGC size-class table 的生成条件和布局。AArch64/X86 的 `CMakeLists.txt` 均已编入新增 `.cpp`，没有按 GC 模式做编译期互斥。

### 18.3 新增测试与验证结果

本批先建立结构/行为基线，再修改实现：

- 结构 RED：`AArch64CangjieAsmPrinter.cpp`、`AArch64CangjieCallSize.cpp`、`X86CangjieAsmPrinter.cpp`、`X86CopyGCEmitter.cpp` 均不存在，公共写屏障 hook 仍为 `void`。
- 行为 RED：新增 `X86/cangjie-msgc-wrapper-slow-call.ll` 后，X86 Mach-O caller 中的 `CJ_MCC_WriteRefFieldFast_MSGC` call 被旧空 hook 删除；修改后 Mach-O/ELF 都保留 slow call。
- 新增 `AArch64/cangjie-msgc-writebarrier-statepoint.ll`，锁定 `STATEPOINT` 的真实 call target：inline on 时进入 AArch64 MSGC 序列，inline off 时保留 wrapper call。
- 扩展 `X86/cangjie-copygc-fastpath-file-split.ll`，覆盖 Linux SysV、Win64、object、array 和 finalizer；finalizer 的既有 `jmp CJ_MCC_OnFinalizerCreated` 行为保持不变。
- `cangjie-newarray-msgc-dynamic-fastpath.ll` 增加两张 dynamic table 定义各只出现一次的检查。

最终验证结果：

| 验证 | 结果 |
| --- | --- |
| `LLVMAArch64CodeGen`、`LLVMX86CodeGen`、`llc` 编译和链接 | 通过；所有新增 `.cpp` 均编译为独立 object |
| AArch64 相关 15 个 lit | `14 PASS + 1 XFAIL` |
| X86 选定 8 个公共/CopyGC/slow-call lit | `6 PASS + 1 XFAIL + 1 UNSUPPORTED` |
| `python3 build.py build -t release --no-tests` | 退出码 0 |
| `git diff --check` | 通过 |

AArch64 集合包含现有 CangjieGC、CopyGC object/array、MSGC static/dynamic array、六类 barrier、object 级反汇编和新增 statepoint 用例。X86 集合包含现有 GC phase/metadata 基线及新增 Linux/Win64 CopyGC 和 MSGC wrapper fallback。

`X86/CangjieGC/CFile/CFile-mac-package.ll` 在本批修改前后的全目录基线中都因本机错误文本为 `Abort trap: 6`、测试固定匹配 `Aborted` 而失败，属于已有平台文本差异，没有修改该无关用例。旧的 PID 19187 `llc` 进程一直处于 `UE` 状态，是此前无有效输入的手工探针残留；所有本批构建和 lit 都以新命令退出码为准，没有把该进程当作当前任务。

### 18.4 本批没有完成且不能提前强做的事项

后端物理拆分完成不等于 CopyGC/MSGC 语义分流完成。以下事项仍未完成：

1. **显式 GCMode 契约**：cjc 选项、`Cangjie_GCMode` Module flag、统一解析/校验、bitcode cache key 和 LTO 混合模式拒绝尚未实现。
2. **lowering 双模式分流**：`CJRuntimeLowering`、`CJBarrierLowering`、statepoint 后对象/数组分配仍需先恢复 CopyGC 基线，再只在显式 MSGC 模式生成 MSGC wrapper/属性。
3. **互斥 wrapper 属性**：CopyGC/MSGC object/array wrapper 还没有一套完整、互斥且可 round-trip 的属性契约。
4. **删除 `_MSGC_dynamic` 启发式**：`AArch64CangjieAsmPrinter.cpp::emitMccNewObjectFastPath()` 仍通过 Module 中存在 `CJ_MCC_NewObjectFast_MSGC_dynamic` 推断 MSGC。当前没有 GCMode 或等价显式属性时直接删除，会让普通 MSGC O0 object call 错走 CopyGC emitter，因此本批只把它移入公共 Cangjie 分派文件，没有伪装成已经解决。
5. **allocation emission/size 全统一**：写屏障已经共享 decision，但下列现有尺寸缺口仍在：NewObject/NewFinalizer 展开通常仍按普通 call 尺寸；direct MSGC static/dynamic array、caller-attributed dynamic `NewArrayFast`、metadata pre-init 等也没有全部共享实际 emitter predicate。修复这些项会改变 branch relaxation 输入，必须在 wrapper 属性稳定后单独做 correctness 批次和 jump-table/MIR 压力测试。
6. **Runtime 宏隔离**：`NewArrayGenericFast3_MSGC` 声明、定义和 alias 的 `MRT_USE_MARK_SWEEP` 隔离，以及 CopyGC/MSGC Runtime 符号表检查尚未执行。
7. **双 SDK 端到端验证**：本批没有替换 runtime dylib，也没有构建 compiler/runtime/stdlib 三件套矩阵；最终仍需按隔离 `env -i` 环境重新编译 `.cj` 用例后验证。

公共 hook 当前保留原名称 `emitMccWriteBarrierFastPath()`，但已具备“目标实际处理成功才返回 true”的关键语义。是否再引入完整 `CJGCEmissionKind/request` 属于后续统一 object/array/barrier 请求模型，不是本次写屏障接口正确性的阻塞项。

### 18.5 下一批推荐顺序

后续应回到语义契约，不再继续通过后端符号名补启发式：

1. 实现 `GCMode` 枚举、cjc 选项、Module flag、统一 helper 和链接/LTO 校验；先只传递信息，不改 lowering。
2. 为现有手写 `.ll` 补齐明确的 CopyGC/MSGC flag，并建立 0+0、1+1、0+1 和缺失 flag 的测试矩阵。
3. 在 `CJRuntimeLowering`、`CJBarrierLowering` 和 statepoint 分配路径先恢复 CopyGC 输出，再接回 MSGC wrapper；补互斥 wrapper 属性。
4. 让 AArch64 object/array 分派只消费 GCMode/显式属性，删除 `_MSGC_dynamic` 符号存在性判断。
5. 扩展 AArch64 私有 decision 到 object、finalizer、array、metadata 等所有 AsmPrinter 展开点，并增加 jump table/BranchRelaxation 压力测试；这一批与实际指令序列优化分开。
6. 宏隔离 Runtime 的 MSGC 专用 generic-array 符号，分别构建 CopyGC/MSGC Runtime 并用 `nm` 校验。
7. 最后构建两套 compiler/runtime/stdlib 组合，用各自 SDK 重新编译 `.cj` 用例，确认 dylib 来源后执行正确性与性能矩阵。

因此，Stage 4 的“后端 hook 安全性和物理文件隔离”现在已经完成；Stage 4 的“去分配启发式和完整尺寸统一”仍被 Stage 1—3 的显式模式/属性契约阻塞。下一步应先实现 GCMode，而不是继续在 AsmPrinter 中猜测 GC。

## 19. 2026-07-21 第三批实施记录：GCMode 契约、互斥 lowering 与 Runtime 宏隔离

本批完成了 18.4 中列出的源码阻塞项。现在的边界是：前端继续生成公共 `llvm.cj.*` intrinsic，Module flag 负责选择 GC，lowering 生成互斥 wrapper/属性，后端只消费已经固化的选择。

### 19.1 GCMode 与 cjc 传递

已新增 `llvm/include/llvm/IR/CJGCMode.h` 和 `llvm/lib/IR/CJGCMode.cpp`，定义：

- `CangjieGCMode::CopyGC = 0`。
- `CangjieGCMode::MSGC = 1`。
- Module flag 名称 `Cangjie_GCMode`。
- 无 flag 的旧 bitcode 兼容为 CopyGC；非 `i32 0/1` 立即报错。

cjc 已增加 `--gc-mode=copygc|marksweep`，默认为 `copygc`。`CGModule` 对每个新 Module 以 `Module::Error` 行为写入 flag，并将 `gcMode` 放入 `GlobalOptions::ToSerialized()`，防止增量编译/配置缓存跨 GC 模式复用。

`llvm/lib/LTO/LTO.cpp` 在 Cangjie LTO 仍保留单 Module 边界时检查输入：

- 显式 `0 + 1` 由 Module flag 冲突拒绝。
- MSGC 与无 flag 旧 CJBC 混合时拒绝，要求旧输入重新编译。
- `0 + 0` 和 `1 + 1` 均允许。
- RegularLTO 在原始 Module 完成 metadata materialize 后、按 `HasSummary` 分叉前累计 CJBC/GCMode；因此 FullLTO 的有 summary 和无 summary 输入都不会因提前合入 `CombinedModule` 而绕过检查。

### 19.2 Runtime/Barrier lowering 互斥分流

`CJRuntimeLowering` 的公共 runtime map 已恢复 `cj_malloc_array_generic -> CJ_MCC_NewArrayGeneric`，并在 lowering 对象中缓存 GCMode：

| 路径 | CopyGC | MSGC |
| --- | --- | --- |
| generic array | 直接使用 `CJ_MCC_NewArrayGeneric` | 允许生成 `Fast3_MSGC` dispatch/wrapper |
| concrete array | 添加 `cj-copygc-fast-alloc` 契约 | 根据 size class 生成 static/dynamic MSGC wrapper |
| dynamic array size | 保持 CopyGC 原路径 | 只在 MSGC 模式下生成 dynamic size fast path |

`CJBarrierLowering` 已按 Module GCMode 分开：

- CopyGC 保留 tagged/read barrier、GC phase 和 `CJ_MCC_NewObjectFast`/`CJ_MCC_NewFinalizerFast` 路径，wrapper 增加 `cj-copygc-fast-alloc`。
- MSGC 才允许读屏障消除、`Write*Fast_MSGC` wrapper 和 static/dynamic object wrapper，并且不继承 CopyGC GC-phase shortcut。
- 关闭 MSGC feature flag 时仍保持 MSGC slow helper/wrapper 语义，不会回退到 CopyGC emitter。
- `CJGenericIntrinsicOpt` 的 fresh/栈目标专项折叠、`CJBarrierOpt` 的 fresh-init 消除和单引用 struct 标量化仍保持删除，没有借 GCMode 重新接回。

### 19.3 后端去启发式与统一 emission decision

`CJMSGCABI.h` 现在集中定义互斥 allocation 属性和分类结果：

- CopyGC：`cj-copygc-fast-alloc`。
- MSGC static：`cj-msgc-sizeclass` + `cj-msgc-slotsize`。
- MSGC dynamic：`cj-msgc-dynamic`。
- 属性冲突或不完整时报错；CopyGC Module 中出现 MSGC ABI、MSGC Module 中出现 CopyGC ABI 时报错。

AArch64 已使用 `classifyAArch64CJEmission()` 作为 object、finalizer、array 和六类 MSGC write barrier 的唯一决策入口。同一结果同时供 AsmPrinter 实际发射和 `AArch64InstrInfo::getCangjieSpecificCallInstSizeInBytes()` 消费，包含：

- emission kind。
- 是否允许 inline。
- size class/slot size。
- 保守展开尺寸。

`AArch64CangjieAsmPrinter.cpp` 中通过 Module 是否存在 `CJ_MCC_NewObjectFast_MSGC_dynamic` 推断 `InMSGCMode` 的逻辑已删除。对新显式 flag Module，没有 fast-wrapper 属性就保留 runtime slow call；无 flag 旧 Module 仅对原 CopyGC object/NewArrayFast statepoint 保留兼容。

X86 只消费 CopyGC 属性或旧 CopyGC 兼容路径；MSGC wrapper 统一保留 slow call，没有引入空 MSGC emitter。公共 `AsmPrinter` 的 object、array 和 barrier hook 都只有在目标返回 `true` 时才吞掉原 call。

### 19.4 Runtime MSGC 符号宏隔离

`CJ_MCC_NewArrayGenericFast3_MSGC`/`MCC_NewArrayGenericFast3_MSGC` 的声明、定义和 alias 已统一放入 `MRT_USE_MARK_SWEEP`：

- `src/CompilerCalls.h`。
- `src/CompilerCalls.cpp`。
- `src/CommonAlias.h`。
- `src/MacAlias.h`。

双模式构建还暴露了 CopyGC 编译仍无条件初始化 `.ms*` Runtime 参数的隔离缺口。`CjScheduler.cpp` 和 `CangjieRuntimeApi.cpp` 中的 MSGC 常量、helper 和 `.ms*` designated initializer 现已受 `MRT_USE_MARK_SWEEP` 保护，而不是为 CopyGC 补伪默认值。

### 19.5 本轮本地验证结果

| 验证 | 结果 |
| --- | --- |
| LLVM `llc` 增量重编译/链接 | 退出码 0；AArch64/X86 新拆分源文件实际重编入库 |
| cjc 增量重编译/链接 | 退出码 0；CodeGen 对象文件重编后重新生成 `build/build/bin/cjc` |
| LLVM GCMode/lowering/backend lit | Step46 聚焦集 `33/33 PASS`；LTO 用例同时覆盖 ThinLTO、FullLTO 无 summary、FullLTO 有 summary 的 MSGC + legacy 拒绝；扩大集中另有既有 `atomic.ll` XFAIL，0 新 FAIL |
| FullLTO CJBC 边界回归 | `cj-gc-mode-mismatch.ll` 与 `cj-fulllto-other-bc.ll` 合计 `2/2 PASS`；后者兼容 macOS `Abort trap: 6` 与 Linux `Aborted` 终止文本 |
| cjc object-only 编译 | 默认、`copygc`、`marksweep` 三条路径均退出 0 |
| bitcode Module flag | 默认 `0`，显式 CopyGC `0`，MarkSweep `1` |
| 公共 intrinsic 一致性 | 同一 `hello.cj` 的 CopyGC/MSGC raw bitcode 中 `llvm.cj.*` 行无差异 |
| O0/O2 模式稳定性 | CopyGC 在 O0/O2 均为 `0`，MarkSweep 在 O0/O2 均为 `1` |
| cjc 非法选项 | `--gc-mode=invalid` 被拒绝，返回 invalid value 诊断 |
| Runtime CopyGC release 构建 | 退出码 0；`NewArrayGenericFast3_MSGC` 和 `inlineFastPathGCPhase` 均不在导出符号表 |
| Runtime MarkSweep release 构建 | 退出码 0；`_CJ_MCC_NewArrayGenericFast3_MSGC`、`_MCC_NewArrayGenericFast3_MSGC` 和 `Heap::inlineFastPathGCPhase` 均存在 |
| 静态隔离审计 | 无 `InMSGCMode`、无 `getFunction(...MSGC_dynamic)` 启发式；已撤销的三类优化入口均不存在 |
| 三个仓库 `git diff --check` | 全部退出 0 |
| 只读代码复审 | FullLTO 缺口修复后 Critical/Important 均为 0，可进入后续 SDK 验证 |

lit 中唯一的 XFAIL 为 `AArch64/CangjieGC/atomic.ll` 的既有 expected failure，不是本轮新失败。PID 19187 仍是历史手工探针留下的 `UE` 状态 `llc`；本轮构建与 lit 都以新命令的退出码和结果文件为准。

### 19.6 当前唯一剩余阶段：SDK 三件套端到端验证

源码修改项已完成，当前只保留第 15.4 节的 SDK 运行验证，本轮按约定不执行：

1. 分别产出 CopyGC 和 MSGC 的 compiler/runtime/stdlib 三件套，不只替换 runtime dylib。
2. 每个 SDK 都在干净 `env -i` 子 shell 中使用，先用 `DYLD_PRINT_LIBRARIES=1` 确认实际 dylib 来源。
3. 每次切换 runtime 后都用当前 SDK 重新编译 `.cj`，禁止复用旧 binary。
4. 两种模式分别跑 object/array/generic/barrier、O0/O2、fast-path on/off 和现有 MSGC 压力用例，并记录 compiler/runtime/stdlib 三者来源。
5. 最后再做 CopyGC 生成码/性能基线与 MSGC 正确性/性能对比。

SDK 阶段开始前不需要再改 intrinsic 或恢复已撤销的 generic/barrier 优化。如果端到端失败，应先按三件套来源、Module flag、wrapper 属性、实际 dylib 路径和 Runtime 导出符号的顺序定位，不回退到 `_MSGC_dynamic` 符号猜测。

### 19.7 本批仍未执行的边界

按用户“最后的 SDK 构建先等一下”的约定，本批没有：

1. 将任何 Runtime dylib 复制或签名到 SDK。
2. 组装新 compiler/runtime/stdlib 三件套 SDK。
3. 替换 SDK Runtime 后重新编译并执行 `.cj` 正确性/压力/性能用例。
4. 执行 CopyGC/MSGC O0/O2 的完整 SDK 矩阵和交叉模式失败验证。
5. 引入文档中长期建议的 MSGC layout ABI version；它不在本批五项剩余工作的实施范围内。

因此当前结论是：**Step46 本批要求的源码修改已完成，LLVM/cjc/Runtime 本地验证已通过；整体“已实施并完成 SDK 验收”状态仍需等三件套 SDK 组装与端到端矩阵完成后再确认。**
