# Step30 LLVM 侧 MSGC 优化实现盘点

## 1. 背景

本文记录对以下目录的源码审阅结果：

```text
/Users/lipeisheng/code/cangjie/cangjie/cangjie_compiler/third_party/llvm-project
```

目标是梳理编译器/LLVM 侧已经为 MSGC（Mark-Sweep GC）实现了哪些优化，以及这些优化和 runtime 侧 MSGC barrier / allocator 的对应关系。

结论：当前 LLVM 分支中已经合入多类 MSGC 专项优化，不只是设计文档。核心落点包括：

1. MSGC 读屏障消除；
2. MSGC 写屏障 lowering 到专用 fast wrapper；
3. AArch64 写屏障 inline fast path 发码；
4. MSGC 对象分配 fast path；
5. MSGC 数组分配 fast path；
6. generic read/write/array 热点消解。

主要源码位置：

```text
llvm/lib/CodeGen/CJBarrierLowering.cpp
llvm/lib/Target/AArch64/AArch64AsmPrinter.cpp
llvm/lib/CodeGen/AsmPrinter/AsmPrinter.cpp
llvm/lib/Transforms/Scalar/CJRuntimeLowering.cpp
llvm/lib/Transforms/Scalar/CJGenericIntrinsicOpt.cpp
```

相关测试位置：

```text
llvm/test/CodeGen/AArch64/CangjieGC/msgc-writebarrier-lowering.ll
llvm/test/CodeGen/AArch64/CangjieGC/msgc-gcread-generic-lowering.ll
llvm/test/CodeGen/AArch64/CangjieGC/msgc-gcwrite-generic-lowering.ll
llvm/test/CodeGen/AArch64/cangjie-writebarrier-msgc-fastpath.ll
llvm/test/CodeGen/AArch64/cangjie-newarray-msgc-dynamic-fastpath.ll
llvm/test/CodeGen/AArch64/cangjie-newarray-msgc-dynamic-concrete-fastpath.ll
llvm/test/Transforms/CJRuntimeLowering/new_array_generic_msgc.ll
llvm/test/Transforms/CJRuntimeLowering/new_array_concrete_dynamic_msgc.ll
llvm/test/Transforms/CJGenericIntrinsicOpt/msgc_gcread_generic_fresh.ll
```

---

## 2. MSGC 读屏障消除

实现位置：

```text
llvm/lib/CodeGen/CJBarrierLowering.cpp
```

控制开关：

```cpp
static cl::opt<bool> EnableMSGCReadBarrierLowering(
  "enable-msgc-read-barrier-lowering", cl::init(true), cl::Hidden);
```

判断函数 `shouldLowerMSGCReadBarrier()` 支持以下 intrinsic：

```cpp
cj_gcread_ref
cj_gcread_weakref
cj_gcread_struct
cj_gcread_generic
cj_gcread_static_ref
cj_gcread_static_struct
```

MSGC 下这些 read barrier 会被特殊 lowering：

| intrinsic | MSGC lowering |
|---|---|
| `cj_gcread_ref` | direct load |
| `cj_gcread_weakref` | direct load |
| `cj_gcread_static_ref` | direct load |
| `cj_gcread_struct` | `llvm.memcpy` |
| `cj_gcread_static_struct` | `llvm.memcpy` |
| `cj_gcread_generic` | 改写为 struct write/copy fast path |

这和 runtime 侧 `MSMarkBarrier` 的语义一致：MSGC 当前没有真正的 read barrier，读路径本质是 direct load / memcpy。

---

## 3. MSGC 写屏障 lowering 到专用 fast wrapper

实现位置：

```text
llvm/lib/CodeGen/CJBarrierLowering.cpp
```

控制开关：

```cpp
static cl::opt<bool> EnableMSGCWriteBarrierLowering(
  "enable-msgc-write-barrier-lowering", cl::init(true), cl::Hidden);
```

当前写屏障 lowering 只对 AArch64 开启：

```cpp
const Triple TT(M.getTargetTriple());
if (!TT.isAArch64()) {
  return false;
}
```

`shouldLowerMSGCWriteBarrier()` 支持：

```cpp
cj_gcwrite_ref
cj_gcwrite_static_ref
cj_gcwrite_struct
cj_gcwrite_static_struct
cj_gcwrite_generic
cj_array_copy_ref
```

MSGC 专用 wrapper 名称：

```text
CJ_MCC_WriteRefFieldFast_MSGC
CJ_MCC_WriteStaticRefFast_MSGC
CJ_MCC_WriteStructFieldFast_MSGC
CJ_MCC_WriteStaticStructFast_MSGC
CJ_MCC_ArrayCopyRefFast_MSGC
CJ_MCC_ArrayCopyStructFast_MSGC
```

这些 wrapper 本身是 internal noinline 函数，body 默认调用原 runtime slow helper。真正的优化发生在 AArch64 后端：后端识别这些 wrapper 名称，并改为发射手写 inline fast path。

更新（2026-07-04）：`CJBarrierLowering::shouldLowerMSGCWriteBarrier()` 源码侧已经补上 `cj_array_copy_ref` 与 `cj_array_copy_struct`。因此 array-copy-ref / struct 现在都应通过 MSGC write-barrier lowering 进入对应 fast wrapper；是否在最终工具链中生效，仍需以重编后的 IR/测试结果为准。

---

## 4. AArch64 写屏障 inline fast path

实现位置：

```text
llvm/lib/CodeGen/AsmPrinter/AsmPrinter.cpp
llvm/lib/Target/AArch64/AArch64AsmPrinter.cpp
```

`AsmPrinter.cpp` 会识别 MSGC wrapper：

```cpp
CJ_MCC_WriteRefFieldFast_MSGC
CJ_MCC_WriteStructFieldFast_MSGC
CJ_MCC_WriteStaticRefFast_MSGC
CJ_MCC_WriteStaticStructFast_MSGC
CJ_MCC_ArrayCopyRefFast_MSGC
CJ_MCC_ArrayCopyStructFast_MSGC
```

然后调用：

```cpp
emitMccWriteBarrierFastPath(...)
```

### 4.1 phase check

fast path 会从 TLS 读取 GC phase：

```asm
LDRB W9, [X28 + gcPhaseOffset]
```

逻辑大致是：

```text
phase == 0                       -> slow path
phase < MSGC_MARK_SETUP          -> StoreOnly
phase > MSGC_MARK_TERMINATION    -> StoreOnly
MSGC mark setup/concurrent/term   -> 执行 old-edge logging
```

也就是说只有 MSGC mark 阶段执行写屏障记录；idle/sweep 等阶段直接 store/memcpy/memmove。

### 4.2 ref/static ref fast path

对 `WriteRefFieldFast_MSGC` / `WriteStaticRefFast_MSGC`：

1. 检查 phase；
2. 读取旧引用 old；
3. old 为 null 则 StoreOnly；
4. old 不在 heap range 则 StoreOnly；
5. 从 TLS 读取 `msAllocBuffer`；
6. 检查 `WriteBarrierBuffer` 是否有空间；
7. 将 old ref 写入 `WriteBarrierBuffer`；
8. 更新 buffer count；
9. store new ref。

关键常量包括：

```cpp
MSGCWBBufferOffsetInAllocBuffer = 896
MSGCWBBufferCountOffset = 8192
MSGCWBBufferCapacity = 1024
```

这和 runtime 侧 `AllocBufferMS::wbBuf_` / `WriteBarrierBuffer::kCapacity` 对齐。

### 4.3 struct/static struct fast path

对 struct 写：

1. 检查 phase；
2. 检查对象是否在 heap；
3. 读取 TypeInfo / GCTib；
4. 如果可解析 ref bitmap，则扫描 dst 覆盖范围内的旧引用；
5. 对非空且在 heap 中的 old ref 写入 `WriteBarrierBuffer`；
6. 最后 inline `memcpy`；
7. 复杂场景 fallback 到 runtime slow helper。

这对应 runtime：

```cpp
MSMarkBarrier::WriteStruct()
  EnqueueStructRefsInRange(...)
  memcpy_s(...)
```

### 4.4 array copy ref fast path

对 `ArrayCopyRefFast_MSGC`：

1. 根据 size 计算 ref count；
2. 检查 wb buffer 容量；
3. 遍历 dst 旧引用；
4. old 非空且在 heap 内则 push 到 wb buffer；
5. 最后执行 `memmove`。

这对应 runtime：

```cpp
CopyRefArray()
  EnqueueRefArrayRange(dstField, dstSize)
  memmove_s(...)
```

---

## 5. `cj_gcwrite_generic` 优化

实现位置：

```text
llvm/lib/CodeGen/CJBarrierLowering.cpp
```

关键函数：

```cpp
replaceWithMSGCWriteGenericAsStruct(...)
```

代码注释说明其目标：

```cpp
// Step29 Phase 3: rewrite cj_gcwrite_generic into the existing MSGC
// struct-write fastpath instead of the CJ_MCC_WriteGeneric helper.
```

原路径：

```text
cj_gcwrite_generic -> CJ_MCC_WriteGeneric
```

MSGC 优化后：

```text
cj_gcwrite_generic -> CJ_MCC_WriteStructField -> CJ_MCC_WriteStructFieldFast_MSGC
```

语义转换：

```text
gcwrite.generic(obj=baseObj, dst=fieldPtr, src=box, size)
  -> WriteStruct(obj, fieldPtr, size, box + 8, size)
```

其中 generic box 的 payload 位于 `box + 8`。

代码还保留了 `src == nullptr` 语义：如果 source box 是 null，则将 size 改为 0，使 copy 变为 no-op，避免破坏 runtime 原语义。

这个优化直接针对 `CJ_MCC_WriteGeneric` helper 热点。

---

## 6. `cj_gcread_generic` 优化

实现位置：

```text
llvm/lib/CodeGen/CJBarrierLowering.cpp
```

在 MSGC read barrier lowering 中：

```cpp
case Intrinsic::cj_gcread_generic:
  replaceWithMSGCWriteStruct(II);
```

转换逻辑：

```text
gcread.generic(dstBox, baseObj, srcField, size)
  -> WriteStruct(dstBox, dstBox + 8, size, srcField, size)
```

AArch64 下进一步命中：

```text
CJ_MCC_WriteStructFieldFast_MSGC
```

测试中有明确检查：

```text
CHECK: bl _CJ_MCC_WriteStructFieldFast_MSGC
CHECK-NOT: _CJ_MCC_ReadGeneric
```

说明该优化已经有测试覆盖。

---

## 7. MSGC NewObject fast path

实现位置：

```text
llvm/lib/CodeGen/CJBarrierLowering.cpp
llvm/lib/Target/AArch64/AArch64AsmPrinter.cpp
```

### 7.1 静态尺寸对象

`doNewFastPath()` 会处理：

```text
CJ_MCC_NewObject
CJ_MCC_NewFinalizer
```

对常量 size，`getMSGCFastPathConfig()` 会根据 MSGC size class 表计算：

```text
size class
slot size
```

小于等于 `16384` 的对象可走 MSGC fast path：

```cpp
static constexpr uint32_t MSGCLargeObjectThreshold = 16384;
```

wrapper 会带属性：

```cpp
cj-msgc-sizeclass
cj-msgc-slotsize
```

### 7.2 动态尺寸对象

非 constant size 会生成动态 wrapper：

```text
CJ_MCC_NewObjectFast_MSGC_dynamic
CJ_MCC_NewFinalizerFast_MSGC_dynamic
```

并带属性：

```cpp
cj-msgc-dynamic
```

### 7.3 AArch64 发码

`AArch64AsmPrinter.cpp` 中：

```cpp
emitMccNewObjectFastPath(...)
```

会分流：

```text
有 cj-msgc-sizeclass/cj-msgc-slotsize -> emitMccNewObjectForMSGC
有 cj-msgc-dynamic                  -> emitMccNewObjectForMSGCDynamic
否则                                -> CopyGC fast path 或 slow fallback
```

静态 MSGC 分配 fast path 会内联：

1. safepoint 检查；
2. 从 TLS 读取 `msAllocBuffer`；
3. 从 `AllocBufferMS.activeRegions_[cls]` 取 region；
4. 判断 region 类型：bump / partial；
5. bump region 走 alloc ptr bump；
6. partial region 走 bitmap 查空 slot；
7. 初始化 object header；
8. 失败 fallback slow path。

这说明 LLVM 已经内联了 MSGC 小对象分配的核心路径。

### 7.4 动态尺寸分配表

动态尺寸 fast path 会发射 size class 表：

```text
lCJMSGCSizeClassTable
lCJMSGCSlotSizeTable
```

由运行时 size 查表得到 size class 和 slot size，再走 MSGC fast allocation。

---

## 8. MSGC NewArray fast path

实现位置：

```text
llvm/lib/Transforms/Scalar/CJRuntimeLowering.cpp
llvm/lib/Target/AArch64/AArch64AsmPrinter.cpp
```

### 8.1 concrete array 动态长度 fast path

`replaceNewArray()` 中，如果：

```text
length 是动态值
元素大小 elemSize 是常量
```

则会计算动态 object size，并创建 MSGC dynamic array wrapper：

```cpp
getOrCreateMSGCDynamicConcreteArrayWrapper(...)
```

对应文档：

```text
06_AArch64_MSGC动态长度ConcreteArray快速路径实现方案.md
```

该文档状态写明：

```text
状态：已实现，LLVM/SDK 验证已完成，待 stdx/case27 回归验证
```

### 8.2 generic array dynamic dispatch

`replaceNewArrayGeneric()` / `lowerDynamicNewArrayGeneric()` 中有以下 wrapper：

```text
CJ_MCC_NewArrayGenericFast3_MSGC
CJ_MCC_NewArrayGenericFast3_MSGC_dispatch
CJ_MCC_NewArrayGenericFast3_MSGC_dyn_ref
CJ_MCC_NewArrayGenericFast3_MSGC_dyn_struct
CJ_MCC_NewArrayGenericFast3_MSGC_dyn_w1
CJ_MCC_NewArrayGenericFast3_MSGC_dyn_w2
CJ_MCC_NewArrayGenericFast3_MSGC_dyn_w4
CJ_MCC_NewArrayGenericFast3_MSGC_dyn_w8
```

动态 generic array 会根据 element kind 分流：

| element kind | 目标 wrapper/runtime |
|---|---|
| reference-like | `dyn_ref` / `CJ_MCC_NewObjArray` |
| struct-like | `dyn_struct` / `CJ_MCC_NewArray` |
| primitive width 1 | `dyn_w1` / `CJ_MCC_NewArray8` |
| primitive width 2 | `dyn_w2` / `CJ_MCC_NewArray16` |
| primitive width 4 | `dyn_w4` / `CJ_MCC_NewArray32` |
| primitive width 8 | `dyn_w8` / `CJ_MCC_NewArray64` |

---

## 9. Generic intrinsic 优化

实现位置：

```text
llvm/lib/Transforms/Scalar/CJGenericIntrinsicOpt.cpp
```

这个 pass 不是 MSGC 专属，但对 MSGC generic helper 热点有明显作用。相关测试包括：

```text
msgc_gcread_generic_fresh.ll
```

### 9.1 generic barrier 转 memcpy

对以下 intrinsic：

```text
cj_assign_generic
cj_gcread_generic
cj_gcwrite_generic
```

如果判断复制内容不含 ref field，或者 source/destination 都不是 GC heap pointer，则可转为 `llvm.memcpy`。

例如：

```text
gcread_generic -> memcpy(dst + 8, src, size)
gcwrite_generic -> memcpy(dst, src + 8, size)
```

### 9.2 fresh object 场景优化

`processFreshGcreadGeneric()` 会识别：

1. dst 是 alloca；
2. dst 来自当前 block 内 fresh heap allocation；
3. fresh allocation 带：

```text
metadata: new_gen
return attr: noalias
```

并且中间没有 clobber payload，则可以直接 memcpy。

这减少了 fresh destination 上 `gcread.generic` 的 barrier/helper 调用。当前没有对应的
`processFreshGcwriteGeneric()`；`gcwrite.generic` 只能命中通用的 stack/non-ref copy 规则。

---

## 10. bugfix 对应实现

相关文档：

```text
MSGC_RESULT/bugfix/01_MSGC_FastPath_encodedGCPhase_mismatch.md
MSGC_RESULT/bugfix/02_MSGC_FastPath_InMSGCMode_missing_for_regular_functions.md
```

其中第二个问题在 `AArch64AsmPrinter.cpp` 中能看到对应兜底：如果当前函数没有 MSGC wrapper attribute，但 module 中存在：

```text
CJ_MCC_NewObjectFast_MSGC_dynamic
CJ_MCC_NewFinalizerFast_MSGC_dynamic
```

则认为当前处于 MSGC mode，regular Cangjie function 也走 MSGC dynamic fast path。

代码注释说明，如果 MSGC mode 下误走 CopyGC inline fast path，会读取 runtime 中为兼容保留的 `tlRegion_=NullRegion()`，导致一直 fallback slow path，在 O0 下可能出现大量 slow allocation calls。

---

## 11. 当前已实现优化清单

| 类别 | 状态 | 关键文件 |
|---|---:|---|
| `cj_gcread_ref` direct load | 已实现 | `CJBarrierLowering.cpp` |
| `cj_gcread_weakref` direct load | 已实现 | `CJBarrierLowering.cpp` |
| `cj_gcread_static_ref` direct load | 已实现 | `CJBarrierLowering.cpp` |
| `cj_gcread_struct` -> memcpy | 已实现 | `CJBarrierLowering.cpp` |
| `cj_gcread_static_struct` -> memcpy | 已实现 | `CJBarrierLowering.cpp` |
| `cj_gcread_generic` -> struct fast path | 已实现 | `CJBarrierLowering.cpp` |
| `cj_gcwrite_ref` MSGC wrapper | 已实现 | `CJBarrierLowering.cpp` |
| `cj_gcwrite_static_ref` MSGC wrapper | 已实现 | `CJBarrierLowering.cpp` |
| `cj_gcwrite_struct` MSGC wrapper | 已实现 | `CJBarrierLowering.cpp` |
| `cj_gcwrite_static_struct` MSGC wrapper | 已实现 | `CJBarrierLowering.cpp` |
| `cj_gcwrite_generic` -> struct fast path | 已实现 | `CJBarrierLowering.cpp` |
| `cj_array_copy_ref` MSGC wrapper | 已实现 | `CJBarrierLowering.cpp` |
| `cj_array_copy_struct` AArch64 发码能力 | 有实现，lowering 默认入口需再确认 | `AArch64AsmPrinter.cpp` |
| AArch64 ref write barrier inline old logging | 已实现 | `AArch64AsmPrinter.cpp` |
| AArch64 struct write barrier inline old refs logging | 已实现 | `AArch64AsmPrinter.cpp` |
| AArch64 array ref copy inline old refs logging | 已实现 | `AArch64AsmPrinter.cpp` |
| AArch64 StoreOnly fast path | 已实现 | `AArch64AsmPrinter.cpp` |
| MSGC NewObject 静态 size class fast path | 已实现 | `CJBarrierLowering.cpp`, `AArch64AsmPrinter.cpp` |
| MSGC NewObject 动态 size fast path | 已实现 | `CJBarrierLowering.cpp`, `AArch64AsmPrinter.cpp` |
| MSGC dynamic size class table | 已实现 | `AArch64AsmPrinter.cpp` |
| MSGC concrete array dynamic length fast path | 已实现 | `CJRuntimeLowering.cpp`, `AArch64AsmPrinter.cpp` |
| MSGC generic array dynamic dispatch fast path | 已实现 | `CJRuntimeLowering.cpp` |
| generic barrier -> memcpy | 已实现 | `CJGenericIntrinsicOpt.cpp` |
| fresh object generic read 优化 | 已实现 | `CJGenericIntrinsicOpt.cpp` |
| fresh object generic write 优化 | 未实现，残留测试已删除 | — |

---

## 12. 需要注意的限制和风险

### 12.1 主要面向 AArch64

写屏障 lowering 明确限制：

```cpp
if (!TT.isAArch64()) {
  return false;
}
```

对象/数组 fast path 主要实现在：

```text
llvm/lib/Target/AArch64/AArch64AsmPrinter.cpp
```

因此 x86_64 上不一定具备同等 MSGC fast path。

### 12.2 MSGC mode 判断不是显式 flag

AArch64 后端通过 module 中是否存在：

```text
CJ_MCC_NewObjectFast_MSGC_dynamic
CJ_MCC_NewFinalizerFast_MSGC_dynamic
```

推断是否处于 MSGC mode。这是有效的工程兜底，但长期看不如显式 module flag 稳定。

### 12.3 LLVM fast path 和 runtime layout 强绑定

AArch64 fast path 中硬编码了大量 runtime layout offset，例如：

```cpp
MSGCActiveRegionsOffsetInAllocBuffer = 8
MSGCWBBufferOffsetInAllocBuffer = 896
MSGCWBBufferCountOffset = 8192
MSRegionInfoAllocBitmapOffset = 24
MSRegionInfoFreeSlotCountOffset = 42
MSRegionInfoRegionTypeOffset = 50
```

这意味着 runtime 结构体布局变更时，LLVM 后端必须同步更新 offset，否则会出现错误。

### 12.4 `cj_array_copy_struct` 已补入 lowering allowlist，需继续做工具链验证

后端有 `CJ_MCC_ArrayCopyStructFast_MSGC` 发码能力；截至 2026-07-04，`shouldLowerMSGCWriteBarrier()` 源码侧也已补上 `cj_array_copy_struct`（同时补上 `cj_array_copy_ref`）。后续重点不再是“是否在 allowlist 中”，而是确认重编后的工具链是否真的把 case27 等实际用例 lowering 到对应 fast wrapper。

---

## 13. 总结

LLVM 侧 MSGC 优化已经覆盖 barrier、allocation、array allocation、generic helper 消解等多个热点：

1. 读屏障：MSGC 下直接 load/memcpy，消除 `CJ_MCC_Read*` helper；
2. 写屏障：AArch64 下按 phase 分流，mark 阶段 inline old-edge logging，非 mark 阶段 StoreOnly；
3. generic：`gcread_generic` / `gcwrite_generic` 尽量转 struct fast path 或 memcpy，减少 `CJ_MCC_ReadGeneric` / `CJ_MCC_WriteGeneric`；
4. 分配：小对象静态/动态 size class fast path 已接入；
5. 数组：concrete dynamic array 和 generic dynamic array dispatch 已接入；
6. 后端：AArch64 AsmPrinter 已直接内联 MSGC allocator 和 barrier fast path。

因此当前 MSGC 不是只依赖 runtime 慢路径运行。AArch64 上已经形成了一套较完整的 MSGC compiler fast path：

```text
IR intrinsic lowering
  -> MSGC wrapper
  -> AArch64 wrapper symbol recognition
  -> inline allocation / barrier fast path
  -> fallback runtime slow helper
```

后续需要重点关注的是：

1. `cj_array_copy_struct` lowering 入口是否完整接入；
2. runtime layout offset 与 LLVM 后端常量的同步机制；
3. x86_64 或其他架构是否需要 MSGC fast path；
4. MSGC mode 是否应从 wrapper-name 推断改为显式 module flag；
5. 配合 runtime `[BARRIER_VOL]` 与 allocator stats 继续确认优化命中率。
