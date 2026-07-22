# Step47 MSGC 跨目标 Lowering 与 Mach-O/ELF/COFF 发码扩展设计

> 状态：已实施并完成 LLVM 定向验证（2026-07-21）；SDK 构建按要求暂缓。
>
> 目标：让 MSGC 的 GCMode、intrinsic lowering、wrapper ABI 和语义分类保持目标无关，把 AArch64/X86、Mach-O/ELF/COFF 的差异收敛到最终机器码发射层。

## 1. 范围与平台矩阵

本次覆盖 64 位 Cangjie native 目标：

| 架构 | Object format / OS | Lowering | 本次最终发码能力 |
| --- | --- | --- | --- |
| AArch64 | Mach-O / macOS | 统一 MSGC wrapper | 保持现有 allocation 与 write-barrier fast path |
| AArch64 | ELF / Linux | 统一 MSGC wrapper | 在正确的 ELF 符号与重定位下开放 allocation；write barrier 仅在 LSE 可用时开放，否则 slow fallback |
| X86-64 | Mach-O / macOS | 统一 MSGC wrapper | 新增安全的 phase-gated write-barrier fast path；未实现的分配或 mark 子路径保留 slow fallback |
| X86-64 | ELF / Linux | 统一 MSGC wrapper | 同上，使用 ELF/GOT/PLT 规则 |
| X86-64 | COFF / Windows | 统一 MSGC wrapper | 安全保留 wrapper slow call；当前 Runtime phase 是 C++ ABI 符号，不生成不可移植的 COFF 数据导入 |

Windows AArch64、32 位 ARM/X86 和其他 object format 不在本次范围内。目标不在矩阵中或缺少必需 CPU feature 时，wrapper slow body 必须保持正确可执行。

X86 第一阶段不复制 AArch64 的完整 inline mark bitmap、WorkBuf append 和 MSGC allocation 实现。已在 Mach-O/ELF 上实现 `phase==idle -> store/memcpy/memmove`，`phase==mark -> wrapper slow path`。COFF 在稳定 C ABI phase 符号尚未提供时保留 wrapper，避免将 Itanium 符号错用到 MSVC ABI。

## 2. 当前问题

当前有三类边界混在一起：

1. `CJBarrierLowering.cpp` 在生成 MSGC 写屏障 wrapper 前检查 `Triple::isAArch64()`，导致 wrapper ABI 与目标架构绑定；X86 无法看到可供 emitter 分类的调用。
2. `AArch64CangjieCallSize.cpp` 使用 `isOSBinFormatMachO()` 同时控制 allocation 和 write-barrier inline。该判断既代表“当前只验证过 Mach-O”，又掩盖了 ELF 符号修饰、动态库外部数据重定位和 LSE feature 等独立条件。
3. `AArch64MSGCEmitter.cpp` 直接保存 Mach-O 修饰后的 C++ 符号名，并手工选择 `_memcpy`/`memcpy`。非 Mach-O 分支虽然存在，但仍会消费 Mach-O 双下划线符号，不能直接启用。

对象、数组分配 wrapper 和 `CJMSGCABI` 分类已经目标无关；公共 `AsmPrinter` 的 `bool` target hook 也已经能够在目标未处理时保留原 call。因此本次不引入新的 intrinsic，也不在公共层增加 `if AArch64 else X86` 分派。

## 3. 总体架构

数据流固定为：

```text
公共 llvm.cj.* intrinsic + Cangjie_GCMode=MSGC
        ↓
CJRuntimeLowering / CJBarrierLowering
        ↓ 只表达 MSGC 语义，不查询 AArch64/X86/Mach-O/ELF/COFF
内部 *_MSGC wrapper（始终带可执行 slow body）
        ↓
AsmPrinter 公共 wrapper 分类
        ↓ virtual bool target hook
        ├─ AArch64MSGCEmitter：AArch64 opcode、LSE、Mach-O/ELF relocation
        ├─ X86MSGCEmitter：X86 opcode、SysV ABI、Mach-O/ELF relocation；COFF 安全回退
        └─ false：普通发射原 wrapper call
```

公共层只共享：

- GCMode；
- wrapper 名称和函数属性；
- size class 与 barrier kind；
- wrapper 对应的 slow Runtime ABI；
- 后端 ABI 合法性校验。

目标层独占：

- 参数寄存器和 stack argument；
- 指令选择与原子指令能力；
- TLS 固定寄存器的读取；
- object-format 符号修饰和 relocation；
- inline 展开尺寸估算与 stackmap 记账。

不创建跨架构的共享 `MSGCEmitter.cpp`，也不让 AArch64/X86 emitter include 对方实现。

## 4. 目标无关 Lowering

`CJBarrierLowering` 的 MSGC 写侧选择只检查：

```text
GCMode == MSGC && EnableMSGCWriteBarrierLowering
```

删除三处 `isAArch64()` gate：普通写屏障选择、`gcread_generic` 转 struct helper 后的 wrapper 选择、`gcwrite_generic` 转 struct helper 后的 wrapper 选择。六类 wrapper 在所有范围内的 64 位目标上保持一致：

- `CJ_MCC_WriteRefFieldFast_MSGC`
- `CJ_MCC_WriteStaticRefFast_MSGC`
- `CJ_MCC_WriteStructFieldFast_MSGC`
- `CJ_MCC_WriteStaticStructFast_MSGC`
- `CJ_MCC_ArrayCopyRefFast_MSGC`
- `CJ_MCC_ArrayCopyStructFast_MSGC`

每个 wrapper 继续是 internal、noinline，并 tail-call 对应公共 Runtime helper。后端只有在完整发射 fast path 后才返回 `true`；否则原调用和 wrapper slow body都保留。

`gcwrite_generic -> WriteStructField` 的 `box + 8` payload 规则属于 64 位 Cangjie 对象 ABI，而不是 AArch64 指令规则。本次用共享常量或 DataLayout 约束表达，并对非 64 位目标回退到 `CJ_MCC_WriteGeneric`，避免把裸数字 `8` 错当成跨所有架构契约。

## 5. AArch64 发码

AArch64 继续由 `classifyAArch64CJEmission()` 同时服务实际 emitter 和 `AArch64InstrInfo` 尺寸估算，但把粗粒度 Mach-O gate 拆成两个 capability：

```text
supportsMSGCAllocationInline(Subtarget, Triple)
supportsMSGCWriteBarrierInline(Subtarget, Triple)
```

allocation 支持 AArch64 Mach-O 和 AArch64 ELF。write barrier 除 object format 外还要求 LSE；没有 LSE 时保留 wrapper slow call，不允许无条件发射 `LDSETALX/LDSETALB`。

Runtime C++ 数据符号以未加 object-format 前缀的 Itanium ABI 名称保存，例如 `_ZN12MapleRuntime4Heap21inlineFastPathGCPhaseE`。最终 `MCSymbol` 名称通过目标 mangler/统一 helper 生成：Mach-O 增加平台前缀，ELF 保持原名，避免在常量中写死双下划线。

外部数据地址按 object format 发射：

- Mach-O：`@GOTPAGE/@GOTPAGEOFF`；
- ELF PIC/PIE：`:got:`/`:got_lo12:` 后间接读取；
- 非 PIC ELF 只有在 LLVM relocation/code-model 明确允许时才可使用直接 page relocation；默认仍使用 GOT；
- 非 Mach-O/ELF 返回不支持，不进入 emitter。

`memcpy`、`memmove` 等 C 符号同样通过目标符号前缀接口生成，不在业务逻辑中手写 `_memcpy`。slow wrapper call继续复用正常 MachineOperand lowering。

## 6. X86-64 发码

新增 `X86MSGCEmitter.cpp`，并在 `X86AsmPrinter` 中 override `emitMccWriteBarrierFastPath()`。第一阶段只实现无需复制 AArch64 mark 算法的安全分流：

```text
acquire-load MSGC phase
    ├─ idle：ref/static-ref 直接 store 完整 64-bit RefField 表示
    │        struct/static-struct 调 memcpy
    │        array-copy 调 memmove
    └─ mark：调用原 *_Fast_MSGC wrapper slow body
```

X86 TSO 上普通 byte load 足以提供 acquire 所需的硬件顺序，但生成的 LLVM/MC 序列仍要明确保持该 load 不可跨写操作重排。mark phase 本次不在 X86 inline 操作 MSRegion bitmap 或 WorkBuf。

参数映射和支持范围为：

- SysV64（macOS/Linux）：`RDI, RSI, RDX, RCX, R8, R9`，本次六类 wrapper 均已实现；
- Win64：当前六类 wrapper 均回退 slow call，不读取 C++ ABI phase 数据符号，也不猜测第 5/6 参数的 stack offset；
- 任何无法证明 stack argument 位置正确的 call 形态返回 `false`。

外部 phase 数据取址按格式处理：

- Mach-O：GOTPCREL + Mach-O 符号前缀；
- ELF：GOTPCREL，函数 slow call继续由正常 PLT lowering处理；
- COFF：当前 Runtime 只导出 C++ ABI phase 数据，Itanium/MSVC 名字不可通用；因此 hook 在发射任何指令前返回 `false`，保留 wrapper slow call。后续若 Runtime 增加稳定 C ABI phase 符号，再开放 COFF import-address 形式。

MSGC object/array allocation wrapper 在 X86 第一阶段继续执行 slow body；这不是 lowering 特例，而是 X86 target hook 的能力结果。后续扩展 allocation 或 inline mark 时只增加 `X86MSGCEmitter`，不再修改公共 lowering。

## 7. 正确性与 fallback

所有 fast path 必须满足“先判定完整能力，再开始发射”。target hook在下列任一条件不满足时返回 `false`：

- 目标不是设计覆盖的 64 位架构；
- object format 未支持；
- AArch64 write barrier 缺少 LSE；
- wrapper ABI、calling convention 或参数位置不匹配；
- COFF 外部数据导入形式无法正确生成；
- 用户关闭相应 inline 开关。

返回 `false` 后由原有 AsmPrinter 正常发射 wrapper call。禁止在发出部分 fast-path 指令后再返回 `false`；否则会同时留下半段 inline 序列和原调用。

CopyGC 模块不能出现 MSGC wrapper，MSGC 模块不能进入 CopyGC emitter；现有 `verifyCangjieGCFunctionABI()` 继续在 AsmPrinter 初始化时执行。LTO 的 GCMode 混合校验不变。

X86 store-only 路径必须与 Runtime `RefField` 表示一致：和 AArch64 `STR X0`一样写入完整 64 位引用表示，不得丢失 tag 位。数组复制保持 overlap-safe 的 `memmove`，不能替换成 `memcpy`。

## 8. 测试与验收

测试遵循先 RED 后 GREEN：

1. 新增 target-neutral lowering 测试：同一份 MSGC intrinsic 分别用 AArch64、X86-64 triple，均生成相同 `_Fast_MSGC` internal wrapper；CopyGC 不生成。
2. AArch64 ELF allocation 测试：确认 static/dynamic object/array 展开并只生成 ELF 合法符号。
3. AArch64 ELF write-barrier 双测试：`+lse` 展开，`-lse` 保留 wrapper call；Mach-O 现有结果不变。
4. AArch64 ELF object relocation 测试：使用 `llvm-readobj -r`/`llvm-objdump` 检查 GOT relocation，不只检查文本汇编。
5. X86 Mach-O/ELF 测试：确认 phase load、idle store/copy 和 mark slow call 同时存在，且 C/Runtime 符号修饰正确。
6. X86 Win64 测试：先覆盖 ref/static-ref；struct/array 只有在 stack argument 与 shadow-space 断言成立时启用，否则明确检查 slow fallback。
7. 运行现有 AArch64 MSGC、AArch64 CopyGC、X86 CopyGC、X86 MSGC slow-call 和 GCMode/LTO 测试，确保没有回归。

本次验收不包含 SDK、stdlib 或 Runtime dylib 构建；LLVM 定向构建与 lit/FileCheck 是完成边界。

## 9. 文件修改范围

预计修改：

- `llvm/lib/CodeGen/CJBarrierLowering.cpp`：移除目标架构 gate，统一 wrapper lowering；
- `llvm/include/llvm/CodeGen/CJMSGCABI.h`：如有需要，补充目标无关 payload/slow-ABI 常量，不加入 opcode；
- `llvm/lib/Target/AArch64/AArch64CangjieCallSize.h/.cpp`：拆分 allocation/barrier capability，并加入 object-format/subtarget feature；
- `llvm/lib/Target/AArch64/AArch64MSGCEmitter.cpp`：统一符号修饰，补 ELF GOT 发射；
- `llvm/lib/Target/X86/X86AsmPrinter.h`：声明 MSGC write-barrier target hook；
- `llvm/lib/Target/X86/X86MCInstLower.cpp`：保持薄分派并准备 slow-call operand；
- `llvm/lib/Target/X86/X86MSGCEmitter.cpp`：新增 X86 MSGC phase-gated write barrier；
- `llvm/lib/Target/X86/CMakeLists.txt`：编入新 emitter；
- `llvm/test/CodeGen/AArch64/CangjieGC/`、`llvm/test/CodeGen/AArch64/`、`llvm/test/CodeGen/X86/`：新增跨 triple、ELF relocation、LSE 和 Win64 测试。

不修改前端 intrinsic，不复制 `llvm.cj.msgc.*`，不把 AArch64 指令或 Mach-O relocation 放入公共头文件，不改 CopyGC Runtime 语义。

## 10. 方案选择

评估过三种方式：

1. **目标无关 wrapper + 独立 target emitter（采用）**：语义边界清晰，未支持目标可安全 fallback，新增平台不再修改 lowering。
2. 在 `CJBarrierLowering` 持续枚举 AArch64/X86/OS：短期改动少，但每增加一个平台都要污染 IR 层，并容易让 lowering 与 emitter 能力漂移。
3. 在 LLVM IR 中直接生成完整 MSGC fast path：表面跨目标，实际会把 TLS、Runtime layout、原子能力和 relocation 泄漏到公共 IR，且难以与 call-size/stackmap 保持一致。

采用方案 1。AArch64 ELF 和 X86 的能力逐项开放；任何未完成项都以 slow wrapper 的正确性为先，不通过删除 target gate 来假装已经支持。

## 11. 实施结果

2026-07-21 实施后的实际边界：

- `CJBarrierLowering.cpp` 的 MSGC wrapper 选择已无 AArch64、X86 或 object-format 判断，仅保留 GCMode、feature 和 64 位 wrapper ABI 约束。
- `enable-msgc-inline-write-barrier` 已由公共 CodeGen 唯一定义，AArch64/X86 共用开关，避免目标库重复注册选项。
- AArch64 Mach-O 保持原快路径；AArch64 ELF allocation 已开放，write barrier 仅在 `+lse` 时展开，`-lse` 回退 wrapper。
- AArch64 外部 Runtime 数据使用目标 mangler 和 GOT；dynamic allocation 的 module-local size-class table 通过 `MachineOperand` lowering 生成 Mach-O/ELF 各自的 page relocation。
- X86-64 Mach-O/ELF 在 `phase==0` 时展开 ref store、struct `memcpy`、array `memmove`；非 idle 分支保留原 `_Fast_MSGC` wrapper call。
- X86 fast emitter 只接受默认 C/SysV64 calling convention 和普通 direct-call opcode；vectorcall、regcall、tail call 等 ABI/控制流形态在首条发码前回退。
- X86-64 COFF/Windows 和 X86-32 已验证为安全 slow fallback，对 phase 数据不生成错误重定位。
- `llvm-lit` 定向运行 28 个 AArch64/X86 MSGC 相关用例，28/28 通过；加上 CopyGC 文件拆分、GCMode/LTO、Runtime lowering 和 generic 公共优化后，扩展回归为 39/39 通过。
- X86 显式 statepoint barrier 在 target hook 展开后会主动记录 Cangjie stackmap；ELF 对象的 `.cjmetadata.stackmap` 非空。ELF、Mach-O、COFF 对象文件均使用 `llvm-readobj` 检查符号/重定位。

本轮未构建 SDK、Runtime dylib、stdlib 或 stdx。
