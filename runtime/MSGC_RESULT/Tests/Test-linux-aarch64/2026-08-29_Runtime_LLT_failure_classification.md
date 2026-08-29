# Runtime LLT FAIL/UNSUPPORTED 原因分类与处理建议

## 1. 范围与结果

- 测试批次：`run-20260828_211048`
- 测试阶段：`01-runtime-llt`
- 平台：Linux aarch64，目标 `aarch64-unknown-linux-gnu`
- SDK：`/home/lipeisheng/cangjie/cangjie_compiler_CMSGC/output/msgc`
- stdx：`/home/lipeisheng/cangjie/cangjie_compiler_CMSGC/output/msgc/linux_aarch64_cjnative/dynamic/stdx`
- 测试仓库：`/home/lipeisheng/cangjie/cangjie_test`
- 测试框架 commit：`2a6330f10d2dbb27ea8d0c898713ae1689eb275a`
- GC 模式：`marksweep`
- 框架并发：`-j2`
- 单用例超时：120 秒
- 结果目录：`/home/lipeisheng/cangjie/cangjie_runtime_CMSGC/runtime/MSGC_RESULT/Tests/Test-linux-aarch64/run-20260828_211048/01-runtime-llt`

原始结果为 `PASS 226`、`FAIL 5`、`UNRESOLVED 17`，归一化后为 `PASS 226`、`FAIL 5`、`UNSUPPORTED 17`，没有 `ENV-ERROR`。汇总见 [`summary.txt`](<./run-20260828_211048/01-runtime-llt/summary.txt:1>)。框架普通文本汇总只显示 231 项，是因为默认隐藏了 17 个 `UNRESOLVED`；本记录以归一化后的 248 项为准。

分类依据是：先看框架返回码和单用例日志，再对照用例源码、Runtime 头文件和 Runtime stackmap 代码。`UNRESOLVED` 因当前 Linux 条件没有有效执行命令而归为 `UNSUPPORTED`，不计入 `FAIL`。

## 2. FAIL 分类

### 2.1 Runtime API 测试头文件与 RuntimeParam ABI 不一致：3 项

用例：

- `CJNative/RuntimeApi/runtime_api_followup.cj`
- `CJNative/RuntimeApi/runtime_api_task_loader.cj`
- `CJNative/RuntimeApi/runtime_api_validation.cj`

证据：

- `runtime_api_followup` 在第一次 `InitCJRuntime` 时报告 `coStackSize` 非法并以 134 退出，见 [`runtime_api_followup` 日志](<./run-20260828_211048/01-runtime-llt/log/Runtime/CJNative/RuntimeApi/runtime_api_followup_cj_1787922649.log:37>)。
- `runtime_api_task_loader` 在初始化 Runtime 时报告 `pthread_create ... Resource temporarily unavailable return 11`，见 [`runtime_api_task_loader` 日志](<./run-20260828_211048/01-runtime-llt/log/Runtime/CJNative/RuntimeApi/runtime_api_task_loader_cj_1787922649.log:30>)。
- `runtime_api_validation` 的编译成功，但最终有效初始化同样因 `pthread_create` 返回 11 而 abort，见 [`runtime_api_validation` 日志](<./run-20260828_211048/01-runtime-llt/log/Runtime/CJNative/RuntimeApi/runtime_api_validation_cj_1787922648.log:27>)。
- 三个测试都包含本地复制的旧版 `RuntimeParam` 定义；其 `GCParam` 只有 5 个字段，见 [`runtime_api_test.h`](</home/lipeisheng/cangjie/cangjie_test/testsuites/LLT/Runtime/CJNative/RuntimeApi/runtime_api_test.h:51>)。
- 当前 Runtime 的 `GCParam` 已包含 MSGC 配置字段，直到 `msAdaptiveGrowth`，见 [`Cangjie.h`](</home/lipeisheng/cangjie/cangjie_runtime_CMSGC/runtime/src/Cangjie.h:245>)；当前 ABI 约束为 `sizeof(GCParam) == 160`、配置结构为 248 字节，见 [`InvokeConfig.h`](</home/lipeisheng/cangjie/cangjie_compiler_CMSGC/output/msgc/include/cangjie/Macro/InvokeConfig.h:157>)。
- Runtime API 会按新版结构布局访问 `param->coParam`，见 [`CangjieRuntimeApi.cpp`](</home/lipeisheng/cangjie/cangjie_runtime_CMSGC/runtime/src/CangjieRuntimeApi.cpp:363>)。旧测试头文件传入的对象尺寸和 `coParam` 偏移不匹配，因此 Runtime 读取到了错误位置的参数；`coStackSize` 非法和 `pthread_create(EAGAIN)` 是该 ABI 失配的不同表现。

判断：这是测试侧 C API 头文件与当前 Runtime/SDK ABI 不同步导致的测试失真，暂不把 3 项计为 GC 或调度器真实行为缺陷。`runtime_api_validation` 中初始化前的一系列错误日志是用例主动验证的预期分支，真正失败点是最后一次有效初始化。

处理建议：

1. 让 Runtime API 用例直接包含与被测库同版本的 `Cangjie.h`，删除 `runtime_api_test.h` 中重复的 `HeapParam`、`GCParam`、`RuntimeParam` 定义。
2. 将当前 `Cangjie.h` 纳入 SDK 的公开 include 产物；当前 SDK include 树中未发现该头文件，不能继续依赖测试仓库内的手工副本。
3. 在 API 测试编译阶段增加 `sizeof` 和 `offsetof` 的 ABI 检查，至少校验 `GCParam`、`RuntimeParam` 及 `coParam` 偏移。
4. 更新头文件后以 `-j1` 串行重跑这 3 项；若仍有 `EAGAIN`，再单独检查 Runtime 创建的线程数和宿主资源限制。

### 2.2 AArch64 GC stackmap 寄存器根记录异常：1 项

用例：

- `CJNative/GC/gc_async_allocation_finalizer.cj`

原始结果：前三个 `cjEnableAsyncAllocation` 变体通过，第四个 `cjEnableAsyncAllocation=xx` 变体以 134 退出，日志为 `register x21 is not recorded`、`wrong reg info`、`gc-main-thread`、`SIGABRT`，见 [`gc_async_allocation_finalizer` 日志](<./run-20260828_211048/01-runtime-llt/log/Runtime/CJNative/GC/gc_async_allocation_finalizer_cj_1787922648.log:89>)。相同错误签名还出现在 Runtime HLT、stdlib HLT 和 compiler LLT，说明它不是单一测试断言问题。

#### 2.2.1 结论

目前已经把责任边界收敛到：

> LLVM 生成的 AArch64 stackmap 中 `x21` 的位编号和 Runtime 解码是一致的；失败发生在 Runtime 扫描栈帧时，当前寄存器根映射要求消费 `x21`，但 `RegSlotsMap` 在此时没有 `x21` 的可用保存槽。

因此，当前证据不支持“LLVM 把 x21 编成了错误寄存器编号”或“`.cjmetadata` 中 x21 位图损坏”这两个判断。更准确的首要怀疑是：GC 栈扫描入口没有获得、没有识别，或在扫描顺序中丢失了当前寄存器的全量快照；需要继续在 Runtime 的 unwind context/frame type 边界上修复和验证。

`cjEnableAsyncAllocation=xx` 是触发该问题的变体，不是当前源码中已知的配置项。当前 Runtime 没有读取这个变量，且直接设置 `cjStackGrow=1` 仍可复现，所以不能把 `xx` 本身作为根因。

#### 2.2.2 LLVM 生成端核对

LLVM 侧有两套不同的寄存器编号，当前代码恰好保持一致：

1. 普通寄存器根位图使用 DWARF register number。AArch64 `AArch64Bit2Reg` 按 `x0 ... x31` 顺序排列，因此 bit 21 就是 `x21`，见 [`StackMaps.cpp`](</home/lipeisheng/cangjie/cangjie_compiler_CMSGC/third_party/llvm-project/llvm/lib/CodeGen/StackMaps.cpp:134>)。
2. 序言保存寄存器使用独立的 ordinal。`AArch64CalleeSavedReg` 将 `x19 ... x30` 映射为 prologue bit 0 ... 11，将 `d8 ... d15` 映射为 bit 12 ... 19；Runtime 的 `ResolveCalleeSaved` 也按同一顺序解析，见 [`StackMaps.cpp`](</home/lipeisheng/cangjie/cangjie_compiler_CMSGC/third_party/llvm-project/llvm/lib/CodeGen/StackMaps.cpp:94>) 和 [`RegisterAarch64.h`](</home/lipeisheng/cangjie/cangjie_runtime_CMSGC/runtime/src/Common/RegisterAarch64.h:35>)。

生成流程也没有发现编号错位：

- [`updateOrInsertFnInfo`](</home/lipeisheng/cangjie/cangjie_compiler_CMSGC/third_party/llvm-project/llvm/lib/CodeGen/StackMaps.cpp:737>) 从 MachineFrameInfo 获取实际 callee-save 栈偏移。
- [`addItemInfo`](</home/lipeisheng/cangjie/cangjie_compiler_CMSGC/third_party/llvm-project/llvm/lib/CodeGen/StackMaps.cpp:1401>) 对引用寄存器设置 `1 << Loc.Reg`；如果寄存器属于当前函数的 callee-save 集合，也会被保留在 stackmap 中。
- [`setPrologueInfo`](</home/lipeisheng/cangjie/cangjie_compiler_CMSGC/third_party/llvm-project/llvm/lib/CodeGen/StackMaps.cpp:1267>) 输出 callee-save bitmap 和对应偏移。
- [`emitStackMapItem`](</home/lipeisheng/cangjie/cangjie_compiler_CMSGC/third_party/llvm-project/llvm/lib/CodeGen/StackMaps.cpp:1658>) 以位表形式输出每个 PC 行和寄存器表。

ELF 入口也匹配 Runtime 的读取方式：[`AsmPrinter.cpp`](</home/lipeisheng/cangjie/cangjie_compiler_CMSGC/third_party/llvm-project/llvm/lib/CodeGen/AsmPrinter/AsmPrinter.cpp:1036>) 在非 leaf Cangjie 函数入口前写入 4 字节的相对 method descriptor，Runtime 从 `startPC - 4` 读取它，见 [`MFuncdesc.inline.h`](</home/lipeisheng/cangjie/cangjie_runtime_CMSGC/runtime/src/ObjectModel/MFuncdesc.inline.h:47>)。这条 descriptor/stackmap 入口在失败样本中可以正常解析。

#### 2.2.3 Runtime 消费端核对

Runtime 的数据流如下：

```text
frame startPC/framePC
  -> MFuncDesc::GetFuncDesc(startPC)
  -> CompressedStackMapHead::GetStackMapHead()
  -> StackMapTable::GetIdxSet(startPC, framePC)
  -> RegTable::GetActiveRegBits()
  -> RegRoot::VisitGCRoots()
  -> RegSlotsMap::VisitSingleSlotsRoot()
```

关键代码含义：

- [`CompressedStackMap.h`](</home/lipeisheng/cangjie/cangjie_runtime_CMSGC/runtime/src/StackMap/CompressedStackMap.h:46>) 根据当前 PC 行取出寄存器 bit map。
- [`StackMapAarch64.h`](</home/lipeisheng/cangjie/cangjie_runtime_CMSGC/runtime/src/StackMap/StackMapAarch64.h:64>) 从最低位开始遍历，bit 21 直接传给 `VisitSingleSlotsRoot`，没有额外偏移。
- [`StackMapTypeDef.h`](</home/lipeisheng/cangjie_runtime_CMSGC/runtime/src/StackMap/StackMapTypeDef.h:44>) 中的 `RegSlotsMap` 不是寄存器值表，而是“某个寄存器当前可以从哪个栈槽读取”的来源表。访问成功后 [`Erase`](</home/lipeisheng/cangjie_runtime_CMSGC/runtime/src/StackMap/StackMapTypeDef.h:54>)，因此它是一条按栈帧传递并消费的来源链。
- [`StackMapTable.h`](</home/lipeisheng/cangjie_runtime_CMSGC/runtime/src/StackMap/StackMapTable.h:196>) 将 prologue ordinal 映射为 `x19 ... x30`，AArch64 Linux 每个 offset 乘 8。
- [`TracingCollector.cpp`](</home/lipeisheng/cangjie_runtime_CMSGC/runtime/src/Heap/Collector/TracingCollector.cpp:225>) 先枚举当前 managed frame 的 slot/root 和寄存器 root；枚举失败后在 260 行输出 `wrong reg info`，最后才在 267 行记录该 frame 的 callee-save 槽。

这里的最后一个顺序是设计要求，不宜直接调换：

```text
SAFEPOINT/STACKGROW stub 保存的当前寄存器
  -> 当前 managed frame 的寄存器根（消费并 Erase）
  -> 当前 managed frame 序言保存的 callee-save 槽
  -> caller managed frame 的寄存器根
```

如果把当前 frame 的 `RecordCalleeSaved` 提前，它会把当前函数为 caller 保存的旧值误当成当前函数正在使用的寄存器值，可能造成错误保活。因此已确认的修复方向不是简单调整 `TracingCollector.cpp:267` 的前后顺序，而是补齐最上游的当前寄存器来源。

#### 2.2.4 失败二进制的精确验证

对失败用例保存的可执行文件执行 ELF section、符号、反汇编和 `.cjmetadata` 解析：

```text
.../gc_async_allocation_finalizer.out
```

验证结果如下。下面地址是 ELF 链接地址，运行时实际地址还要加 PIE load base：

1. `_CNat5ArrayIG_E6<init>HlG_` 的函数入口为 `0x2b4944`。
2. 失败时捕获到的 frame PC 为 `0x2b49d0`，相对入口偏移为 `0x8c`。
3. 该函数 `+0x8c` 的 stackmap 行为 `RegIdxPlusOne=2`；寄存器表第 2 行为 `0x600000`，即 bit 21 和 bit 22，也就是 `x21/x22`。
4. 该函数序言实际保存了 `x22/x21`：`stp x22, x21, [sp, #80]`。函数 FP 为 `sp+16`，因此 `x21` 保存槽为 `fp+72`。
5. 同一 stackmap 的 callee-save bitmap 为 `0xdff`，其中 prologue ordinal 2 对应 `x21`，offset 为 9；Runtime 计算 `base + 9 * 8 = fp + 72`，与反汇编完全一致。

这证明失败 frame 的 stackmap 行是有效的，并且编译器已经为 `x21` 生成了正确的保存位置。

另外，AArch64 两个 stub 的布局也一致：

- [`HandleSafepointStub.S`](</home/lipeisheng/cangjie/cangjie_runtime_CMSGC/runtime/src/arch/aarch64_linux/HandleSafepointStub.S:32>) 保存 `x0 ... x28`，`x21` 位于 `stubFP + 0xb8`。
- [`StackGrowStub.S`](</home/lipeisheng/cangjie/cangjie_runtime_CMSGC/runtime/src/arch/aarch64_linux/StackGrowStub.S:33>) 使用同样的保存布局。
- [`StackMapAarch64.h`](</home/lipeisheng/cangjie/cangjie_runtime_CMSGC/runtime/src/StackMap/StackMapAarch64.h:54>) 从 `fp + 0x10` 开始按 8 字节记录 `x0 ... x28`，计算出的 `x21` 地址也是 `fp + 0xb8`。

所以当前没有证据表明 AArch64 stub 的 `x21` 保存槽偏移写错。

#### 2.2.5 GDB 复现得到的直接证据

在同一个失败二进制上以 `cjStackGrow=1` 运行，并在 [`StackMapTypeDef.h:60`](</home/lipeisheng/cangjie/cangjie_runtime_CMSGC/runtime/src/StackMap/StackMapTypeDef.h:60>) 停止，得到：

- 当前访问寄存器为 `reg=21`。
- 调用栈为 `RegSlotsMap::VisitSingleSlotsRoot -> RegRoot::VisitGCRoots -> RootMap::VisitRegRoots -> TracingCollector::VisitStackRoots -> GCStackInfo::VisitStackRoots`。
- 失败瞬间 `RegSlotsMap` 只有 `x19`、`x20` 为 `isRecorded=true`，`x21` 及后续寄存器均为 false。
- 当前 `Array.init +0x8c` 的 stackmap 明确要求 `x21/x22`。

这把问题从“解析出的寄存器是否为 x21”进一步定位为“解析正确，但扫描时没有 x21 的来源”。`register x21 is not recorded` 是 Runtime 对来源链不完整的保护性 abort，并不表示 x21 这个寄存器在二进制 metadata 中没有记录。

#### 2.2.6 原因分类

**A. 已确认：Runtime 寄存器来源链缺少当前寄存器快照。**

`GCStackInfo::VisitStackRoots` 对栈帧逐个处理，只有遇到 `SAFEPOINT` 或 `STACKGROW` frame 时，才调用 `RecordStubAllRegister` 把 `x0 ... x28` 加入 `RegSlotsMap`，见 [`GcStackInfo.cpp`](</home/lipeisheng/cangjie/cangjie_runtime_CMSGC/runtime/src/UnwindStack/GcStackInfo.cpp:38>)。如果当前扫描入口没有先经过一个被识别的全寄存器 stub，managed frame 的寄存器根就只能依赖来源链中已有的寄存器槽；当链中没有 `x21` 时就会在 `VisitSingleSlotsRoot` 失败。

**B. 高概率触发边界：unwind context 或 stub frame 分类没有把全寄存器快照接入扫描链。**

AArch64 safepoint stub 在 [`HandleSafepointStub.S`](</home/lipeisheng/cangjie/cangjie_runtime_CMSGC/runtime/src/arch/aarch64_linux/HandleSafepointStub.S:113>) 将 stub FP、特殊 unwind PC 和 `SIGNAL_STATUS` 传给 `MRT_UpdateUwContext`；Runtime 再由 [`MachineFrame.cpp`](</home/lipeisheng/cangjie/cangjie_runtime_CMSGC/runtime/src/UnwindStack/MachineFrame.cpp:103>) 通过特殊 PC 精确识别 `SAFEPOINT`。stack grow 使用同样的模式，见 [`StackGrowStub.S`](</home/lipeisheng/cangjie/cangjie_runtime_CMSGC/runtime/src/arch/aarch64_linux/StackGrowStub.S:115>) 和 [`MachineFrame.cpp`](</home/lipeisheng/cangjie/cangjie_runtime_CMSGC/runtime/src/UnwindStack/MachineFrame.cpp:69>)。

因此需要重点检查：

- `MRT_UpdateUwContext` 写入的 `pc/fa/status` 是否在 GC 开始前仍然有效；
- `FrameInfo::AnalyseAndSetFrameType` 是否将该 frame 判成 `SAFEPOINT`/`STACKGROW`，而不是普通 managed/runtime frame；
- unwind 从 stub 到 managed caller 时是否跳过了 stub，或者从一个不含全量寄存器的 `RELIABLE` context 直接开始扫描；
- stack grow 调整栈后，保存快照的 FP 与新的 managed frame FP 是否仍然落在同一条来源链上。

错误具有时序相关性也符合这个判断：直接运行 `cjStackGrow=1` 时多次运行大部分通过，少数运行在 GC 线程枚举根时失败；变体名称改变了分配/GC 时序，但没有改变 stackmap 编码。

**C. 当前不支持作为首要原因：LLVM AArch64 寄存器编号或 bit endian 错误。**

LLVM 的 bit 21、Runtime 的 bit 21、失败二进制的 `x21/x22` 表项、Array.init 的实际序言保存位置均一致。除非修复 Runtime 来源链后仍出现“有效快照已存在但 `x21` 仍缺失”，否则不应先修改 LLVM 的 `AArch64Bit2Reg`、`AArch64CalleeSavedReg` 或压缩位表编码。

**D. 次要防护缺口：metadata 解析缺少边界校验。**

[`MFuncdesc.inline.h`](</home/lipeisheng/cangjie/cangjie_runtime_CMSGC/runtime/src/ObjectModel/MFuncdesc.inline.h:47>) 和 [`CompressedStackMap.h`](</home/lipeisheng/cangjie/cangjie_runtime_CMSGC/runtime/src/StackMap/CompressedStackMap.h:106>) 对 descriptor、stackmap header 和各表项基本不做范围/版本校验。它会让坏 PC 或损坏 metadata 更难诊断，但本次失败样本已经成功解析到合法的 Array.init stackmap 行，所以这不是当前主因。建议作为诊断增强项处理。

#### 2.2.7 解决方案和验证顺序

1. **先修 Runtime 的来源链入口。** 保证所有可被 GC 枚举的运行中 mutator，在进入 `VisitStackRoots` 前都有一个可追溯的当前寄存器快照；优先验证 `SAFEPOINT` 和 `STACKGROW` frame 的 `pc/fa/status`、分类结果及 `RecordStubAllRegister` 调用顺序。不要先放宽 `VisitSingleSlotsRoot`，也不要把缺失寄存器当作空根。
2. **增加来源链诊断。** 给 `RegSlotsMap` 增加仅 DEBUG 使用的来源信息，至少记录寄存器、槽地址、来源 frame 类型/FA，并在 `Insert`、`Erase`、`VisitStackRoots` 前后打印 32 位寄存器 availability mask。`wrong reg info` 日志应同时打印 frame type、startPC、framePC、`framePC-startPC`、原始 reg bitmap、callee-save bitmap 和当前 availability mask。
3. **单独验证 safepoint/stackgrow 路径。** 对失败用例固定 `cjStackGrow=1`、`-j1`，连续运行不少于 100 次；统计首次失败前的 frame 序列。重点确认失败样本是否缺少 `SAFEPOINT`/`STACKGROW`，以及出现该 frame 时 `fa + 0x10 + 21 * 8` 是否等于实际 `x21` 保存槽。
4. **再决定是否需要修改 LLVM。** 只有当 Runtime 日志证明全量快照已正确插入、但 LLVM 生成的当前 PC 行仍要求不存在的寄存器根时，才继续检查 `recordCJStackMap`、statepoint operand 收集和 `addItemInfo` 的 live-reference 集合。当前二进制证据尚未指向这里。
5. **补 metadata 防护。** 在 descriptor、header、record count、bit length、callee-save offset 数量之间增加一致性校验，并把非法 `startPC/framePC` 转换为带上下文的诊断失败，避免后续将 metadata 损坏误报为寄存器缺失。

当前处理结论：该项继续保留为真实 `FAIL`，不归入 `UNSUPPORTED`。原因已经从“组件未知”收敛为“Runtime AArch64 GC 栈扫描的寄存器来源链/上下文边界问题”，但本轮没有修改 Runtime 或 LLVM 源码；修复前应以来源链 instrumentation 得到具体缺失的 frame 路径后再提交代码变更。

### 2.3 超大数组压力用例未形成确定性 OOM：1 项

用例：

- `CJNative/Exception/Expection41_NewArray64.cj`

证据：用例申请 `Array<Int64>(2147483648)`，源码见 [`Expection41_NewArray64.cj`](</home/lipeisheng/cangjie/cangjie_test/testsuites/LLT/Runtime/CJNative/Exception/Expection41_NewArray64.cj:14>)。编译成功，运行阶段 119.44 秒后返回框架超时码 3；用例原本期望返回 1 并输出 `Out of memory`，见 [`Expection41` 日志](<./run-20260828_211048/01-runtime-llt/log/Runtime/CJNative/Exception/Expection41_NewArray64_cj_1787922649.log:25>)。

判断：这是约 16 GiB 数组分配压力没有在当前 MSGC 默认堆策略下及时转化为确定性 OOM，属于压力用例调度/资源配置问题，当前结果不能证明 Runtime 分配路径错误。该项没有设置受控 `cjHeapSize`，也没有列入本次安全 LLT 排除列表。

处理建议：

1. 从常规 Runtime LLT 安全清单中移出，放入单独的压力/OOM 清单，固定 `-j1` 执行。
2. 为该用例设置可控的小堆上限，例如 `cjHeapSize=4mb`，让期望的 `Out of memory` 和退出码稳定可达；具体值需通过一次单用例复验确定。
3. 若该用例的目标是验证 64 位数组长度而非 OOM，应改为不需要实际申请 16 GiB 的检查方式，或拆出专门的虚拟地址/边界测试。

## 3. UNSUPPORTED 分类

17 项均为框架原始 `UNRESOLVED`，统一原因是当前条件集合为 `Unix/Linux/arm/CJNATIVE`，没有有效的 `EXEC`/`RUN` 命令。框架记录的原文是 `No valid command statement was found.`，归一化清单见 [`unsupported_cases.txt`](<./run-20260828_211048/01-runtime-llt/unsupported_cases.txt:1>)。

### 3.1 Android/iOS 专用用例：3 项

- `CJNative/DFX/excepetion_handle_on_android.cj`
- `CJNative/DFX/hilog_cjrt_test.cj`
- `CJNative/DFX/trace_cjrt_test.cj`

这些文件只有 Android 或 iOS 条件命令，没有 Linux 命令，例如 [`hilog_cjrt_test.cj`](</home/lipeisheng/cangjie/cangjie_test/testsuites/LLT/Runtime/CJNative/DFX/hilog_cjrt_test.cj:7>) 和 [`trace_cjrt_test.cj`](</home/lipeisheng/cangjie/cangjie_test/testsuites/LLT/Runtime/CJNative/DFX/trace_cjrt_test.cj:7>)。

处理：继续在 Linux 批次记为 `UNSUPPORTED`；在 Android/iOS 专用流水线中提供 `logcat`、`atrace`、`xcrun` 等平台工具后执行。只有存在等价 Linux 语义时才新增 Linux 条件命令。

### 3.2 DEBUG/IR 专用用例：6 项

- `CJNative/debuginfo_array_type.cj`
- `CJNative/debuginfo_c_func_call.cj`
- `CJNative/debuginfo_class_this.cj`
- `CJNative/debuginfo_global_init_mangleName.cj`
- `CJNative/fast-math-option.cj`
- `CJNative/internal_variable.cj`

这些用例使用 `DEBUG_OPTION` 下的前端/IR 输出命令，例如 [`debuginfo_array_type.cj`](</home/lipeisheng/cangjie/cangjie_test/testsuites/LLT/Runtime/CJNative/debuginfo_array_type.cj:7>)。本批只启用了 `CJNATIVE`，没有启用 `DEBUG_OPTION`。

处理：建立独立的 DEBUG/IR 测试批次，定义 `%frontendCompiler`、`%dump-ir-to-screen`、调试选项及比较器；如果确认 aarch64 Linux 支持这些检查，再将对应条件加入专用配置。不要把 IR 文本检查混入普通 Runtime LLT。

### 3.3 没有完整测试指令的源码片段：2 项

- `CJNative/global_init/test2.cj`
- `CJNative/global_init/test3.cj`

文件只有全局变量源码和说明注释，没有可执行的 `EXEC`、`RUN-EXEC` 或断言指令，因此框架无法构造测试步骤。

处理：若它们是被其他测试引用的源码片段，继续保持 `UNSUPPORTED`；若应当作为独立用例，补齐编译、运行和结果断言后再纳入测试清单。

### 3.4 仅 LLVM 条件的互操作用例：6 项

- `CJNative/interop/interop_advance_01.cj`
- `CJNative/interop/interop_advance_02.cj`
- `CJNative/interop/interop_advance_03.cj`
- `CJNative/interop/interop_advance_04.cj`
- `CJNative/interop/interop_advance_05.cj`
- `CJNative/interop/interop_advance_06.cj`

这些用例的命令条件是 `Linux and LLVM`，例如 [`interop_advance_01.cj`](</home/lipeisheng/cangjie/cangjie_test/testsuites/LLT/Runtime/CJNative/interop/interop_advance_01.cj:7>)。本批条件集合包含 Linux，但使用的是 `CJNATIVE`，不包含 `LLVM`，所以命令未被选中。

处理：在 LLVM 后端专用配置中启用 `LLVM` 条件并提供匹配的编译器/运行选项；CJNATIVE Runtime LLT 中继续标记 `UNSUPPORTED`，不要强行改成普通 FAIL。

## 4. 建议处理顺序

1. 先修正 Runtime API 测试头文件/SDK 公开头文件的 ABI 同步问题，串行回归 3 个 Runtime API 用例。
2. 单独处理 `Expection41_NewArray64.cj` 的 OOM 压力配置，避免常规批次被 16 GiB 分配拖到超时。
3. 对 AArch64 stackmap 问题建立最小复现，核对编译器 metadata 与 Runtime 寄存器映射；该类问题修复前继续保留为真实 `FAIL`。
4. 为 Android/iOS、DEBUG/IR、LLVM backend 分别建立匹配配置；在当前 Linux CJNative 批次中保持 17 项 `UNSUPPORTED`，不计入 FAIL。

## 5. 证据索引

- 阶段汇总：[`summary.txt`](<./run-20260828_211048/01-runtime-llt/summary.txt:1>)
- FAIL 清单：[`failed_cases.txt`](<./run-20260828_211048/01-runtime-llt/failed_cases.txt:1>)
- UNSUPPORTED 清单：[`unsupported_cases.txt`](<./run-20260828_211048/01-runtime-llt/unsupported_cases.txt:1>)
- 原始框架日志：[`Maple_Test.log`](<./run-20260828_211048/01-runtime-llt/log/Maple_Test.log:81>)
- 框架执行命令：[`command.sh`](<./run-20260828_211048/01-runtime-llt/command.sh:1>)
- 环境快照：[`environment.txt`](<./run-20260828_211048/environment.txt:1>)
