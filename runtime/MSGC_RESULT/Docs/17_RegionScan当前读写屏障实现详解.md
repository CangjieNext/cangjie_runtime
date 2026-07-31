# Region-scan 当前读写屏障实现详解

> 代码基线：2026-07-31 本工作区的 Region-scan Runtime 与 LLVM 源码；已包含
> outside-set Small 复用 LLVM TLS fast-add 的修改。本文只描述
> `cangjie_runtime_region_mark/runtime` 和
> `cangjie_compiler_region_mark/third_party/llvm-project`，不包含 Wbuf 实现。
>
> 本文以代码为准。`06_MSGC_Barrier架构与实现详解.md` 说明总体架构，
> `15_Wbuf与RegionScan读写屏障实现对比.md` 说明两套实现的差异；本文聚焦当前
> Region-scan 的完整读/写 barrier 清单、LLVM lowering/内联边界，以及 Runtime 的实际语义。

## 1. 结论与术语

Region-scan 使用 non-moving mark-sweep：对象地址不会因 GC 转发或搬迁而改变。因此，普通
**读屏障不是 forwarding/self-healing barrier**，在 MSGC lowering 开启时会降成直接 `load`
或 `memcpy`。仍须保留 statepoint、safepoint、原子内存序和对象表示等 GC 协议；“无普通读
屏障”不等于“无 GC 约束”。

并发标记期间的写屏障是 deletion-style SATB pre-write barrier。对会覆盖引用槽位的写入，
必须先保护被删除的旧边，再执行写入：

```text
old = load(destination)
mark / record old                 # old 为 heap 引用时
store(destination, new)           # 或 memcpy / memmove
```

这里的 `mark / record` 在当前 Region-scan 中不是把对象直接压入 mutator WorkBuf：

1. Small 对象先按 `(regionIdx, bitmap-word)` 合并到当前 owner 的 `MSBarrierMarkBuffer`；
2. buffer flush 时对 mark plane 做原子 OR；
3. 新置位且 Region 的 scan class 为 `HAS_REF` 时，累积 `pendingWordSummary` 并发布 REGION task；
4. Large/Span 对象使用 canonical marked flag；标记 winner 且有引用字段时发布 OBJECT task；
5. `NO_REF` 对象仍会置 mark，但不发布扫描任务。

这个设计使密集写入可合并同一 mark word 的多个 bit，扫描工作的基本单位是 Region/bitmap
word，而不是每个对象一个 WorkBuf entry。

## 2. Phase 与 Runtime 入口

`HeapImpl::InstallBarrier()` 对 MSGC 安装两类有效 barrier：

| GC phase | `Heap::GetBarrier()` | 语义 |
|---|---|---|
| `IDLE`、`FINISH`、`RECLAIM_SATB_NODE` | `MSIdleBarrier` | 直接读写/复制；没有标记工作 |
| `MS_MARK_SETUP`、`MS_CONCURRENT_MARK`、`MS_MARK_TERMINATION` | `MSMarkBarrier` | 读仍直接访问；写保护被覆盖的旧引用 |
| 其他未审计 phase | `stwBarrier` | 保守回退 |

常用的 `MCC_Write*`、`CJ_MCC_ArrayCopy*` 与 `MCC_Atomic*` 在 `MRT_USE_MARK_SWEEP`
分支直接调用 `MSBarrierOps`。`MSBarrierOps` 从当前线程
`ThreadLocalData::encodedGCPhase` 判断三个连续的 Mark phase；因此这些核心写入口不依赖
虚函数分派。仍经 `Heap::GetBarrier()` 的路径（例如 generic、`CopyStructField` 和 runtime
read API）会得到上表对应的 `MSIdleBarrier`/`MSMarkBarrier` 行为。

## 3. 全部读侧操作

| LLVM intrinsic | 语义 | MSGC LLVM lowering（默认） | Runtime slow/API 行为 |
|---|---|---|---|
| `llvm.cj.gcread.ref` | 读取对象引用字段 | 直接 `load field` | `CJ_MCC_ReadRefField` → `MSIdleBarrier::ReadReference`，raw load |
| `llvm.cj.gcread.weakref` | 读取 weak-ref 字段 | 直接 `load field` | `CJ_MCC_ReadWeakRef` → raw load；不执行 forwarding |
| `llvm.cj.gcread.static.ref` | 读取静态引用字段 | 直接 `load field` | `CJ_MCC_ReadStaticRef` → raw load |
| `llvm.cj.gcread.struct` | 将对象内 struct 拷至目标 | `llvm.memcpy(dst, src, size)` | `CJ_MCC_ReadStructField` → `MSIdleBarrier::ReadStruct`，`memcpy_s` |
| `llvm.cj.gcread.static.struct` | 将静态 struct 拷至目标 | `llvm.memcpy(dst, src, size)` | `CJ_MCC_ReadStaticStruct` → `MSIdleBarrier::ReadStaticStruct`，`memcpy_s` |
| `llvm.cj.gcread.generic` | 读取 generic 值到目标 box | 不是普通 read fast path：改写为对目标 box payload 的 `WriteStruct` | 未优化时 `CJ_MCC_ReadGeneric` → `Barrier::ReadGeneric`；在 MSGC lowering 后，因目标 payload 会被覆盖，需要写侧旧边保护 |
| `llvm.cj.atomic.load` | 带 memory order 的原子引用读 | 保留 Runtime call | `MCC_AtomicReadReference` → `MSLoadRawReference(field, order)`；保持原子内存序 |

`CJBarrierLowering` 的 `-enable-msgc-read-barrier-lowering` 默认值为 `true`。它只在 Module
`Cangjie_GCMode=MSGC` 时应用直接读 lowering；关闭该选项或不满足 MSGC 条件时，intrinsic
保留并下沉到表中的 Runtime API。`gcread.generic` 的例外很重要：源端没有 forwarding
需求，但目标 box 的旧 payload 可能包含最后一条可达边，所以它必须复用写屏障。

## 4. 全部写侧操作

### 4.1 可进入六类 MSGC wrapper 的写操作

以下操作在 64-bit MSGC Module 中由 `CJBarrierLowering` 改写为内部 wrapper。wrapper 自身是
thin slow body：调用原 `CJ_MCC_*` Runtime helper；目标后端不能内联时仍可正确执行 slow
call。目标内联在调用点发生，而不是在 wrapper 函数体中发生。

| LLVM intrinsic | 被覆盖的内容与 Runtime slow helper | MSGC wrapper | Idle target fast path | Mark target fast path / 回退 |
|---|---|---|---|---|
| `cj_gcwrite_ref` | 对象 ref slot；`CJ_MCC_WriteRefField` | `CJ_MCC_WriteRefFieldFast_MSGC` | raw store | 读取 old；托管堆内的 Small（collect-set 与 outside-set）共用坐标计算并合并一 bit 至 TLS buffer；Large/Span、阈值或冲突回 wrapper |
| `cj_gcwrite_static_ref` | 静态 ref slot；`CJ_MCC_WriteStaticRef` | `CJ_MCC_WriteStaticRefFast_MSGC` | raw store | 与 ref 相同，但无 base object |
| `cj_gcwrite_struct` | 对象内 struct；`CJ_MCC_WriteStructField` | `CJ_MCC_WriteStructFieldFast_MSGC` | `memcpy` | 仅 short GCTib 可内联枚举旧 ref，并用 leaf thunk 复用 fast-add；RawArray/复杂 GCTib/校验失败回 wrapper |
| `cj_gcwrite_static_struct` | 静态 struct；`CJ_MCC_WriteStaticStruct` | `CJ_MCC_WriteStaticStructFast_MSGC` | `memcpy` | 同 struct；GCTib 驱动被覆盖范围的旧 ref 枚举 |
| `cj_array_copy_ref` | 引用数组目的区；`CJ_MCC_ArrayCopyRef` | `CJ_MCC_ArrayCopyRefFast_MSGC` | `memmove` | Mark 路径目前保留 wrapper，由 Runtime 在覆盖前逐 slot 处理 |
| `cj_array_copy_struct` | struct 数组目的区；`CJ_MCC_ArrayCopyStruct` | `CJ_MCC_ArrayCopyStructFast_MSGC` | `memmove` | Mark 路径目前保留 wrapper，由 Runtime 枚举目的区旧 ref |

### 4.2 generic、copy 与 atomic 写操作

| LLVM intrinsic | 当前 LLVM 处理 | Runtime 实现与写屏障语义 |
|---|---|---|
| `cj_gcwrite_generic` | 将 source box 的 `+8` payload 改写为 `WriteStruct`；64-bit 下复用 `CJ_MCC_WriteStructFieldFast_MSGC`。`src == null` 时将 copy size 置 0。 | 原 helper `CJ_MCC_WriteGeneric` 调 `Barrier::WriteGeneric`；本质上是可能覆盖 heap generic payload 的 struct 写 |
| `cj_gcwrite_generic_payload` | 保留 `CJ_MCC_WriteGenericPayload` 调用，不进入六类 target inline | 无 ref type 直接 `memcpy_s`；含 ref type 调 `Barrier::WriteStruct` |
| `cj_assign_generic` | 保留 `CJ_MCC_AssignGeneric` 调用 | 无 ref type 直接复制；含 ref type 经 `Barrier::WriteGeneric` |
| `cj_copy_struct_field` | 保留 `CJ_MCC_CopyStructField` 调用 | heap 目的对象经 `Barrier::WriteStruct`；非 heap 目的地直接 `memcpy_s` |
| `cj_array_copy_generic` | 保留 `CJ_MCC_ArrayCopyGeneric` 调用 | 按 component type 分流：reference 类别 → `CopyRefArray`；tuple/struct/enum → `CopyStructArray`；无引用 primitive → `memmove_s` |
| `cj_atomic.store` | 保留 `CJ_MCC_AtomicWriteReference` 调用 | Mark 下使用 CAS retry loop，确保每个即将删除的 old edge 在成功更新前已处理 |
| `cj_atomic.swap` | 保留 `CJ_MCC_AtomicSwapReference` 调用 | Mark 下使用 CAS retry loop；返回成功替换的 old value |
| `cj_atomic.compare.swap` | 保留 `CJ_MCC_AtomicCompareAndSwapReference` 调用 | `old != new` 且 Mark 时在 CAS 前保守处理 expected old edge；失败 CAS 可能多做标记工作，但成功 CAS 没有未保护窗口 |

原子写没有接入六类 MSGC target inline。这样 Runtime 可以维持 `MemoryOrder` 语义，并将
`release`/`acq_rel` 对应的失败 memory order 正规化后用于 CAS retry。

## 5. Runtime：旧边发现、合并与发布

### 5.1 `MSIdleBarrier`

`MSIdleBarrier` 是无并发 Mark 时的直接访问实现：

| 操作 | 实现 |
|---|---|
| ref/weak/static ref read | `MSLoadRawReference` |
| struct/static struct read | `memcpy_s` |
| ref/static ref write | `MSStoreRawReference` |
| struct/static struct write | `memcpy_s` |
| ref/struct array copy | `memmove_s` |
| atomic load/store/swap/CAS | `MS*RawReference` 原子封装，保留传入 memory order |

`MSMarkBarrier` 继承该类的所有读操作；也就是说，Mark 期间普通读依旧是 direct load/
memcpy。

### 5.2 `MSMarkBarrier` 与 `MSBarrierOps`

`MSBarrierOps::WriteReference/WriteStaticRef` 的实际顺序是：仅在 TLS phase 为 Mark 时读取
old，若 `old != new` 则调用 `BarrierMarkContext::AddOldRefActive(old)`，最后 raw store。
struct 和 array copy 先枚举**目的区将被覆盖的旧引用**，然后再 `memcpy_s`/`memmove_s`：

| 操作 | 覆盖前的旧引用枚举 |
|---|---|
| `WriteStruct` | 普通对象使用对象 TypeInfo/GCTib 的目标 range；RawArray 使用 component TypeInfo/GCTib |
| `WriteStaticStruct` | 传入的 GCTib，限制为实际覆盖 range |
| `CopyRefArray` | 目的数组范围的每个 ref slot |
| `CopyStructArray` | 普通对象 GCTib 或 RawArray component GCTib，遍历被覆盖的元素 |

`BarrierMarkContext` 先过滤 `null` 和非 heap 引用；直到遇到第一个 managed old ref 才延迟解析
active `MarkSweepCollector` 与当前 owner 的 `AllocBufferMS`。没有 `AllocBufferMS` 时使用
`PublishManagedRefForBarrierImmediately()`，立即走 mark batch flush，不会丢弃旧边。

### 5.3 TLS bitmap batch ABI

`AllocBufferMS` 内嵌的 `MSBarrierMarkBuffer` 是 compiler/runtime 共享 ABI。当前布局如下：

| 字段/参数 | 值 | 用途 |
|---|---:|---|
| `barrierMarkBuffer_` 在 `AllocBufferMS` 的 offset | 576 | AArch64/X86 直接寻址的 buffer 起点 |
| entry | 32 × 16B | 每项为 `wordKey + mask` |
| set/way | 8 set × 4 way | `(regionIdx ^ wordIdx * 5) & 7` 选择 set |
| `occupancy` / `pendingBitCount` / `lastEntry` | offset 512 / 516 / 520 | occupied way、pending bit 预算、MRU entry |
| flush threshold | 128 pending-add budget | 达到阈值或 set 冲突时先 flush，再重试插入；当前 LLVM 计数是保守值 |
| `wordKey` | `(regionIdx << 6) | wordIdx` | 连接 Region 与其 64-bit mark word |

Runtime 插入路径检查重复 pending bit，因此 Runtime 中的 `pendingBitCount` 精确统计新增 bit。
当前 LLVM inline 路径不做该检查：重复 bit 的 OR 仍然幂等，但会继续增加
`pendingBitCount`，所以这是一个保守的 pending-add 预算，可能比 entry mask 的真实 unique bit
数量更早达到阈值。二者在达到阈值或 set 冲突时都退回 wrapper flush。

### 5.4 flush 与生命周期

`MarkSweepCollector::FlushBarrierMarkBuffer()` 遍历已占用 entry，并执行。alloc plane 中
`1` 表示空闲 slot、`0` 表示已分配 slot；只有本轮 collect-set 的 alloc plane 是冻结快照，
outside-set 仍可能处于 BUMP/bitmap 分配状态，因此不能读取它：

```text
mask = buffered mask
若 region.collectEpoch == currentMarkEpoch：
    mask &= ~allocPlane[wordKey] & validTailMask
    # 去除空闲 slot 与无效 tail bit
outside-set：跳过 alloc-plane 过滤
mask &= ~markPlane[wordKey]       # 去除全局已 mark bit，避免无效原子 RMW
old = atomic_fetch_or(markPlane[wordKey], mask)
newBits = mask & ~old             # 解决 probe 与 RMW 之间的并发竞争
若 newBits 非空且 Region 是 HAS_REF：
    Region::PublishPendingMarkWords(word bit)
    scheduler.TryPublishRegion(region)
clear local buffer
```

Large 对象不占用该 buffer：Runtime 竞争 `flags_.marked`，winner 且 `HAS_REF` 时调用
`markScheduler_.PublishObject()`。buffer 的三条必须覆盖的 drain 路径如下：

| 时机 | 动作 |
|---|---|
| `pendingBitCount >= 128` 或四路 set 满 | 当前写操作前 flush，随后重试插入 |
| owner/`AllocBufferMS` 退出 | `NotifyAllocBufferExit()` flush 对应 buffer |
| MarkTermination STW | `FlushAllBarrierMarkBuffersUnderSTW()` 遍历所有 `AllocBufferMS` 并 flush |
| 下一 mark epoch 的准备 STW | `PrepareBarrierMarkBuffersForNewEpochUnderSTW()` reset；未带 epoch tag 的残留不能带入下一轮 mark plane |

## 6. LLVM：优化、lowering 与 target emission

### 6.1 pass 与 lowering 顺序

```text
Cangjie frontend 的 llvm.cj.* intrinsic
  → CJBarrierOpt             # 栈对象/冗余 barrier 等可证明安全的化简
  → CJBarrierSplit           # 常量大小的 struct barrier 按 GCTib 拆为 ref 操作 + 普通 copy
  → statepoint / safepoint 处理
  → CJBarrierLowering        # 读取 Cangjie_GCMode，完成 MSGC read/write lowering
  → AArch64 或 X86 callsite emission
  → 不支持/校验失败时执行 wrapper 的 Runtime slow helper
```

`CJBarrierOpt` 会识别栈上的读写/复制等可安全操作；`CJBarrierSplit` 对常量大小的
`gcread/gcwrite struct` 和 static struct 按 GCTib 把引用字段分离出来。它们是通用的 IR
优化，不能替代 MSGC 写屏障语义：任何保留下来的 heap reference overwrite 仍必须进入后续
MSGC wrapper 或 Runtime 路径。

`CJBarrierLowering` 从 Module 的 `Cangjie_GCMode` 读取模式。MSGC 会跳过旧 CopyGC 的
`GetGCPhase`/tagged-pointer fast path；它不使用 CopyGC 的 phase shortcut。读侧直接降为
load/memcpy，六类写侧变为 wrapper，其余操作按表 4.2 保留 Runtime ABI。

### 6.2 target callsite fast path

`AArch64MSGCEmitter.cpp` 与 `X86MSGCEmitter.cpp` 在 wrapper callsite 尝试展开实现；
`AArch64CangjieCallSize.cpp` 也使用同一决策，以免指令尺寸估算与最终 emission 漂移。

两套 emitter 都从 current mutator 的 `encodedGCPhase` 进行两个 unsigned range check：

| TLS phase 范围 | emitter 动作 |
|---|---|
| `IDLE(1)` 到 `RECLAIM_SATB_NODE(3)` | 直接 store/memcpy/memmove |
| `MS_MARK_SETUP(20)` 到 `MS_MARK_TERMINATION(22)` | 尝试旧边发现 + TLS bitmap buffer fast-add |
| 其他值 | 调用 wrapper 的 Runtime slow helper |

Mark inline fast path 对托管堆内的 SMALL old-ref 使用统一坐标计算，不再读取或比较
`region.collectEpoch/currentMarkEpoch`。因此 collect-set Small 与 outside-set Small 都可直接
进入同一个 TLS 4-way fast-add；LLVM 依赖“编译器产生的非空 oldRef 是已发布对象起始引用”和
“SMALL Region 的 sizeClass 合法”两个 Runtime/编译器不变量，不重复执行 StateWord、精确
slot 起点或 sizeClass 越界检查。

null 和非 heap old-ref 不需要标记，直接完成原写入；Large/Span、非 SMALL Region、TLS buffer
达到阈值、目标 set 的 4 个 way 均无法容纳当前 `wordKey`、复杂 struct/array 情形，仍会在
**写入前**跳往 slow helper。Runtime 的 `ResolveManagedMarkTarget()`、large OBJECT publication、
bulk 枚举与 buffer flush 继续作为正确性兜底。flush 通过 `collectEpoch` 区分：collect-set 应用
冻结的 alloc plane 和 valid-tail mask，outside-set 跳过 alloc plane；两者随后共用全局 mark
plane 原子置位和 REGION task 发布。

编译器与 Runtime 共享稳定 C 数据符号 `CJ_MRT_MSGCFastPathState`（64B），而非引用 Runtime
C++ mangled data symbol。`CJMSGCABI.h` 同时镜像 object/TypeInfo、RegionInfo、mark plane、
TLS buffer 和 GC phase 的布局常量；变更这些值必须同步修改两侧并整体重编。

### 6.3 已支持与明确不内联的边界

| 类别 | AArch64/X86 当前做法 |
|---|---|
| Ref / StaticRef | Idle 直接 store；Mark 可内联 small old-ref 到 TLS buffer |
| Struct / StaticStruct | Idle 直接 `memcpy`；Mark 对 short GCTib 走内联枚举和共享 leaf thunk，其他情形回 Runtime |
| ArrayCopyRef / ArrayCopyStruct | Idle 直接 `memmove`；Mark 保持 Runtime wrapper |
| generic、generic payload、assign generic、array copy generic、copy struct field | 不属于六类 wrapper 的 target inline；由 Runtime 或间接 `WriteStruct` 保障语义 |
| atomic store/load/swap/CAS | 保持 Runtime call；不进入当前 MSGC target write emitter |
| 非 64-bit wrapper ABI、非 C calling convention、未支持 target/call opcode | 不内联，执行 wrapper slow body |

## 7. 源码索引

### Region-scan Runtime

| 文件 | 当前职责 |
|---|---|
| `runtime/src/CompilerCalls.cpp` | `MCC_*`/`CJ_MCC_*` ABI 入口；MSGC 分支将主写、数组与原子操作交给 `MSBarrierOps` |
| `runtime/src/Heap/Heap.cpp` | `InstallBarrier()` 安装 `MSIdleBarrier`、`MSMarkBarrier` 或保守 `stwBarrier` |
| `runtime/src/Heap/MBarrier/MSIdleBarrier.h` | direct load/store/memcpy/memmove 与原子直接访问 |
| `runtime/src/Heap/MBarrier/MSMarkBarrier.{h,cpp}` | old-before-overwrite、bulk old-ref 枚举和原子 CAS retry |
| `runtime/src/Heap/MBarrier/MSBarrierMarkBuffer.h` | 32-entry、8×4-way TLS bitmap batch 定义与插入逻辑 |
| `runtime/src/Heap/MAllocator/MSAllocBuffer.{h,cpp}` | buffer 嵌入、初始化与生命周期 |
| `runtime/src/Heap/MCollector/MarkSweepCollector.cpp` | buffer add/flush、Region/OBJECT publication、退出与 STW drain |
| `runtime/src/Heap/MSGCFastPath.h` | `CJ_MRT_MSGCFastPathState` 稳定 C ABI |

### Region-scan LLVM

| 文件 | 当前职责 |
|---|---|
| `llvm/include/llvm/IR/Intrinsics.td` | `llvm.cj.gcread_*`、`gcwrite_*`、array、atomic、generic intrinsic 声明 |
| `llvm/lib/Transforms/Scalar/CJBarrierOpt.cpp` | barrier IR 化简与冗余/栈对象优化 |
| `llvm/lib/Transforms/Scalar/CJBarrierSplit.cpp` | 常量大小 aggregate barrier 的 GCTib 拆分 |
| `llvm/lib/CodeGen/CJBarrierLowering.cpp` | 按 `Cangjie_GCMode` 完成 direct read、六类 wrapper 与 generic 重写 |
| `llvm/include/llvm/CodeGen/CJMSGCABI.h` | compiler/runtime 共享的 MSGC ABI 与 wrapper 分类 |
| `llvm/lib/Target/AArch64/AArch64MSGCEmitter.cpp` | AArch64 wrapper-call inline emission |
| `llvm/lib/Target/AArch64/AArch64CangjieCallSize.cpp` | AArch64 emission 的对应 size decision |
| `llvm/lib/Target/X86/X86MSGCEmitter.cpp` | X86-64 wrapper-call inline emission |
| `llvm/lib/CodeGen/TargetPassConfig.cpp` | 将 `CJBarrierLowering` 接入 Cangjie pipeline |

## 8. 关键正确性约束

1. 任何 heap old reference 必须在目的 slot 被覆盖前进入 mark 路径；bulk copy 也不能先复制再扫描。
2. Runtime buffer 可以跨多个 barrier 调用合并，但不能跨 mark epoch；owner 退出与 MarkTermination 必须 drain。
3. collect-set Small 与 outside-set Small 共用 LLVM 坐标计算/TLS fast-add，但 flush 的 alloc-plane 语义不同；Large/Span 仍必须回 Runtime。
4. `NO_REF` 可不发布扫描任务，但不能跳过对象存活标记。
5. generic read 的目标 box 写入仍是一条写边；不能因其名称为 read 而省略旧边保护。
6. atomic 写路径必须同时满足 SATB 的 old-before-mutation 和原本的 memory-order 语义。
7. `CJMSGCABI.h`、`MSGCFastPath.h`、`MSBarrierMarkBuffer.h` 与 `MSAllocBuffer` 的固定 offset 是隐藏 ABI；修改必须同步 compiler/runtime 并用脚本化 SDK 构建后的产物验证。

## 9. 完整 SDK 验证与当前 Runtime 回退量

### 9.1 当前实现、验证口径与数据位置

`enable-msgc-inline-write-barrier` 位于
`llvm/lib/CodeGen/AsmPrinter/AsmPrinter.cpp`，当前默认值为 `true`。完整 SDK 必须同时重编
compiler/LLVM、Runtime、stdlib 和 stdx；只重编 case27 主模块不能代表 stdlib/stdx 内部生成
的屏障代码。

本节区分两版实现：

| 名称 | LLVM Small fast-add 范围 | 用途 |
|---|---|---|
| 旧 ON | 仅 `region.collectEpoch == currentMarkEpoch` 的 collect-set Small | 初始 ON/OFF A/B |
| 当前 ON | 托管堆内所有 SMALL；不再因 epoch 不匹配回 Runtime | 本文当前实现与最新计数 |

当前计数使用重新构建安装在
`cangjie_compiler_region_mark/output/msgc` 的 SDK，重新编译 case27 后运行。测试负载为：

| 配置 | 值 |
|---|---:|
| case | `case27_http_complex_json` |
| payload | `10K`（实际 9572B） |
| concurrency | 10 |
| duration | 10000ms |
| heap | `cjHeapSize=4gb`、`cjMSMaxVirtualHeapSize=4gb` |
| minimum GC threshold | `cjMSMinGCThreshold=20mb` |
| 重复次数 | 3 |

三轮计数原始结果位于：

```text
MSGC_RESULT/Result/case27_http_complex_json/result/
  20260731_outside_small_inline_count_1TmAhq/
```

三轮均 `fail=0`。这里选择稳定的 10 并发负载，用于确认屏障动态调用量；不替代高并发专项
验证。

### 9.2 Runtime 诊断计数含义

Runtime 保留两组由 `CJ_MSGC_BARRIER_COUNT=1` 控制的临时计数；环境变量未设置时不执行原子
累加：

1. `CompilerCalls.cpp` 统计六类 wrapper 的实际 Runtime 入口次数：
   `ref/static_ref/struct/static_struct/array_ref/array_struct`；
2. `MarkSweepCollector::AddManagedRefToBarrierBuffer()` 及相关路径统计 buffer request、
   resolve reject、Large、already pending、普通 add、threshold flush、set-conflict flush
   和 immediate publish。

三个层级不能混用：

```text
Runtime wrapper 入口
  → GCPhase/old-ref/类型等过滤或 bulk 枚举后，可能形成 old-ref buffer request
    → threshold/4-way conflict 时才真正 flush
```

LLVM 直接加入 TLS buffer 的正常 bit 不经过 `AddManagedRefToBarrierBuffer()`，因此不会增加
`old_ref_buffer_request`；但这些 bit 达到 threshold 或 4-way conflict 后仍会回 Runtime
flush。

### 9.3 当前 outside-set Small inline 三轮数据

| 轮次 | 完成业务请求 | Runtime wrapper | wrapper/业务请求 | buffer request | buffer/业务请求 | threshold flush | set-conflict flush | 吞吐 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| 1 | 109728 | 509048 | 4.639 | 294087 | 2.680 | 251439 | 30711 | 10850 req/s |
| 2 | 112363 | 554325 | 4.933 | 316785 | 2.819 | 269688 | 33833 | 11107 req/s |
| 3 | 110763 | 495858 | 4.477 | 287116 | 2.592 | 244933 | 30186 | 10951 req/s |
| 合计/归一化 | 332854 | 1559231 | 4.684 | 897988 | 2.698 | 766060 | 94730 | 平均 10969 req/s |

与相同负载下旧 ON（仅 collect-set Small inline）的三轮数据比较：

| 指标 | 旧 ON | 当前 ON | 结果 |
|---|---:|---:|---|
| 完成业务请求 | 343834 | 332854 | 当前轮次总量低 3.2%，因此同时看归一化值 |
| Runtime wrapper | 9494807 | 1559231 | 绝对数量减少 83.58% |
| wrapper/业务请求 | 27.615 | 4.684 | 减少 83.04% |
| old-ref buffer request | 9429227 | 897988 | 绝对数量减少 90.48% |
| buffer request/业务请求 | 27.424 | 2.698 | 减少 90.16% |

这说明此前由 epoch 不匹配引入的 outside-set Small 普通回退已经大幅消除。下降没有等于旧数据
中 outside-set 成功 add 的 95.49%，原因是：LLVM 接管正常 add 后，更多真实 bit 在 TLS 中
累积，达到 threshold/冲突时仍必须回 Runtime flush；Array/bulk 与 Large 路径也仍保留 Runtime。

### 9.4 剩余 Runtime 请求构成

当前三轮 1559231 次 Runtime wrapper 的类型拆分为：

| wrapper | 数量 | 占比 | 说明 |
|---|---:|---:|---|
| `ref` | 924590 | 59.30% | 主要为 threshold、4-way conflict、Large 等 scalar slow edge |
| `array_ref` | 282059 | 18.09% | Mark bulk 当前不做 target inline |
| `array_struct` | 352582 | 22.61% | Mark bulk 当前不做 target inline |
| `static_ref/struct/static_struct` | 0 | 0% | 本负载未形成 Runtime 动态入口 |

ArrayRef 与 ArrayStruct 合计占剩余 wrapper 的 40.70%，属于当前明确保留的 Runtime 边界。

当前三轮 897988 次 buffer request 中：

| Runtime 动作/分类 | 数量 | 占 buffer request |
|---|---:|---:|
| threshold flush | 766060 | 85.31% |
| 4-way set-conflict flush | 94730 | 10.55% |
| 二者合计 | 860790 | 95.86% |
| Large | 35845 | 3.99% |
| resolve rejected | 0 | 0% |
| already pending | 563 | 0.063% |

现在绝大多数 old-ref buffer request 都对应真正需要 flush 的 slow edge，而不是“outside-set
正常 add 因 epoch 不匹配进入 Runtime”。

诊断字段不是互斥分类。例如一次 threshold 请求会先 flush，再由 Runtime 把当前 oldRef 插入
清空后的 buffer，所以同一次请求还会增加 `old_ref_added_without_flush_outside_set`。当前三轮
该字段为 766149，不能据此判断 LLVM 没有接管 outside-set；它主要表示“触发 flush 后的当前
bit 由 Runtime 完成重插入”。set-conflict 同样会在 flush 后重试。因此不能把所有字段相加
反推 request 总数。

当前三轮计数运行吞吐为 `10850 / 11107 / 10951 req/s`，平均 `10969 req/s`，但
`CJ_MSGC_BARRIER_COUNT=1` 在 Runtime 热路径执行原子计数，因此这组数据只用于确认运行健康
和动态请求量，不作为最终性能结论。当前实现还需要独立、无计数且能稳定完成的三轮复测后，
才能量化 outside-set 扩展的新增吞吐收益。

### 9.5 历史完整 SDK Fastpath ON/OFF A/B

2026-07-31 在 outside-set Small 扩展进入 LLVM fast-add 之前，对
`enable-msgc-inline-write-barrier` 做了一次完整 SDK A/B，目的是回答两个问题：

1. target callsite fastpath 是否真正减少了进入 Runtime wrapper 的请求；
2. 在不启用热路径计数时，fastpath 是否能带来可观察的吞吐收益。

这个开关在 `llvm/lib/CodeGen/AsmPrinter/AsmPrinter.cpp` 中定义，当前默认值为 `true`。A/B
不是只重编 case27 主模块，而是分别生成两套完整 SDK：

| 变体 | LLVM 默认值 | 构建内容 |
|---|---:|---|
| 旧 ON | `cl::init(true)` | compiler/LLVM、Runtime、stdlib、stdx、case27 主模块 |
| Fastpath OFF | 临时改为 `cl::init(false)` | compiler/LLVM、Runtime、stdlib、stdx、case27 主模块 |
| 最终恢复旧 ON | 恢复 `cl::init(true)` | 再次完整重编并安装 compiler/LLVM、Runtime、stdlib、stdx |

三次均使用 Region-scan 固定脚本：

```text
runtime/build/scripts/incremental_rebuild_msgc_sdk.py --jobs 8
```

脚本对 Runtime、stdlib、stdx 执行 clean/build/install，并同步到
`cangjie_compiler_region_mark/output/msgc`。因此 A/B 中 stdlib 与 stdx 自身产生的屏障代码也
分别受 ON/OFF 控制；仅重编 benchmark 主模块不能代表完整效果。

测试负载统一为：

| 配置 | 值 |
|---|---:|
| case | `case27_http_complex_json` |
| payload | `10K`（实际 9572B） |
| concurrency | 10 |
| duration | 10000ms |
| heap | `cjHeapSize=4gb`、`cjMSMaxVirtualHeapSize=4gb` |
| minimum GC threshold | `cjMSMinGCThreshold=20mb` |
| 重复次数 | 3 |

这里选择稳定的 10 并发负载，是为了避免把 1000 并发下已有的 TaskLedger 终止检查问题混入
fastpath A/B。该结果用于确认屏障调用量和稳定负载下的吞吐效果，不替代后续高并发专项验证。

### 9.6 历史 A/B Runtime 诊断计数

为测量动态回退量，Runtime 临时增加了两组由 `CJ_MSGC_BARRIER_COUNT=1` 控制的计数。不开
该环境变量时不执行原子累加：

1. `CompilerCalls.cpp` 统计六类 wrapper 的实际 Runtime 入口次数：
   `ref/static_ref/struct/static_struct/array_ref/array_struct`；
2. `MarkSweepCollector::AddManagedRefToBarrierBuffer()` 及相关路径统计 buffer request、
   resolve reject、Large、already pending、普通 add、threshold flush、set-conflict flush
   和 immediate publish。

因此三个数字必须分开理解：

```text
Runtime wrapper 入口
  → GCPhase/old-ref/类型等过滤后，可能形成 old-ref buffer request
    → buffer 达到阈值或 4-way set 冲突时，才触发 flush
```

“进入 Runtime wrapper”不等于“进入 buffer”，更不等于“每次都 flush”。

三轮动态计数如下：

| Fastpath | 轮次 | 完成业务请求 | Runtime wrapper | wrapper/业务请求 | buffer request | flush | flush/buffer |
|---|---:|---:|---:|---:|---:|---:|---:|
| 旧 ON | 1 | 114533 | 3022482 | 26.390 | 3001639 | 22901 | 0.7629% |
| 旧 ON | 2 | 114645 | 3221621 | 28.101 | 3193278 | 24374 | 0.7633% |
| 旧 ON | 3 | 114656 | 3250704 | 28.352 | 3234310 | 24701 | 0.7637% |
| OFF | 1 | 76919 | 3004537710 | 39061.060 | 2338483 | 17864 | 0.7639% |
| OFF | 2 | 79168 | 3092366249 | 39060.811 | 2187051 | 16643 | 0.7610% |
| OFF | 3 | 79244 | 3095334752 | 39060.809 | 2043458 | 15478 | 0.7574% |

汇总后：

| 指标 | Fastpath OFF | 旧 ON | 结果 |
|---|---:|---:|---:|
| Runtime wrapper/业务请求 | 39060.9 | 27.6 | 减少 99.929%，约 1415 倍 |
| 三轮 buffer request | 6568992 | 9429227 | 旧 ON 完成的业务请求更多，绝对值不可直接横比 |
| buffer request/业务请求 | 约 27.9 | 约 27.4 | 实际 Mark 旧边工作量基本一致 |
| 三轮 flush | 49985 | 71976 | 随实际 buffer request 数量变化 |
| flush/buffer request | 0.7609% | 0.7633% | 两侧比例一致 |

OFF 的 wrapper 次数非常大，但 buffer request/业务请求与旧 ON 基本相同，原因是：

1. OFF 的每次写都会进入 Runtime wrapper，再由 Runtime 的
   `ThreadLocalData::encodedGCPhase` 判断是否处于 Mark；
2. 大部分写发生在非 Mark phase，只产生 wrapper 调用和 Runtime phase filter，不形成
   old-ref buffer request；
3. 旧 ON 在 callsite 先检查相同的 `encodedGCPhase`，Idle/Finish 等 phase 直接完成
   store/memcpy/memmove，避免跨 Runtime 调用；
4. Mark phase 下仍必须完成同等的 SATB old-edge 保护，所以归一化后的 buffer request
   没有被错误消除。

六类 wrapper 的动态拆分进一步说明 fastpath 确实执行：

| 类型 | Fastpath OFF（每轮约） | 旧 ON（每轮约） | 说明 |
|---|---:|---:|---|
| `ref` | 17.0～17.5 亿 | 300～324 万 | 旧 ON 主要只剩 Mark 阶段的 slow/fallback 请求 |
| `struct` | 13.0～13.4 亿 | 0 | 本负载中的 Struct 被 target inline/前序拆分处理 |
| `static_ref` | 217 | 0 | callsite fastpath 已处理 |
| `static_struct` | 0 | 0 | 本负载未形成动态请求 |
| `array_ref` | 61～63 万 | 0.59～0.67 万 | Idle 直接 memmove；Mark bulk 保留 Runtime |
| `array_struct` | 76～79 万 | 0.76～0.85 万 | Idle 直接 memmove；Mark bulk 保留 Runtime |

### 9.7 历史 A/B flush 比例

旧 ON 三轮共有 9429227 次 old-ref buffer request，真正的 threshold/set-conflict
flush 共 71976 次，只占 0.7633%。其余请求通常是：

- 合并到已有 `(regionWord, mask)` entry；
- 占用目标 set 的空 way；
- 命中已 pending bit；
- Large 与 outside-set Small 等情况按当时的 Runtime 语义处理。

threshold flush 与本次 bit 插入可能发生在同一个 buffer request 中：Runtime 会先在
`pendingBitCount >= 128` 时 flush，再继续处理当前 old ref。set-conflict flush 则表示目标
set 的 4 个 way 都不能容纳当前 `wordKey`，flush 后重新查找并插入。因此 flush 计数不是
与普通 add 完全互斥的分类，不能把所有诊断字段简单相加来反推 request 总数。

从历史 A/B 看，旧 ON/OFF 的 flush/buffer 比例都约为 0.76%，说明 fastpath 大幅减少的是无意义的
Runtime 边界调用，并没有通过跳过必要 flush 或 Mark 工作来换取性能。

### 9.8 历史无计数吞吐

计数开启时，OFF 会为数十亿次 Runtime wrapper 执行额外的 relaxed atomic increment，
对 OFF 的扰动远大于 ON。因此计数运行只用于统计调用量，吞吐结论使用
`CJ_MSGC_BARRIER_COUNT` 未设置的独立三轮结果。

| Fastpath | 第 1 轮 | 第 2 轮 | 第 3 轮 | 平均 | 中位数 |
|---|---:|---:|---:|---:|---:|
| OFF | 8311 req/s | 8321 req/s | 8279 req/s | 8303.7 req/s | 8311 req/s |
| 旧 ON | 11074 req/s | 10844 req/s | 10005 req/s | 10641.0 req/s | 10844 req/s |

对比结果：

- 平均吞吐提升：`(10641.0 / 8303.7 - 1) = 28.1%`；
- 中位数吞吐提升：`(10844 / 8311 - 1) = 30.5%`；
- 旧 ON 第三轮存在一定系统/GC 波动，但三轮均高于 OFF；
- 无计数运行仍编译了环境变量 gate 的预测不命中分支，因此这是保留临时诊断代码时的
  A/B；原子计数本身没有参与吞吐结果。

在该历史实现和稳定 case27 配置下，完整 SDK 开启 callsite fastpath 的收益可判定为明显：
Runtime wrapper 归一化请求减少 99.929%，无计数吞吐平均提升约 28%。这组吞吐数据证明
fastpath 开关整体有效，但不能用来量化后来加入 outside-set Small inline 的新增收益。

### 9.9 历史静态代码生成核验

case27 主模块同时使用 `--save-temps` 保存汇编。统计结果为：

| 变体 | `LMSGCWBSlow` 标记 | mark-ref helper 标记 | wrapper callsite |
|---|---:|---:|---:|
| 旧 ON | 240 | 60 | 30 |
| OFF | 0 | 0 | 30 |
| 最终恢复旧 ON | 240 | 60 | 30 |

旧 ON 与 OFF 都能看到 30 个 wrapper callsite，不表示旧 ON 没有内联：旧 ON 的 call 指令被保留为
fastpath 失败时的 slow branch；额外出现的 slow label 和 mark-ref helper 才是 target
emitter 展开的证据。历史 A/B 中动态 Runtime 入口从约 39061 次/业务请求下降到约
27.6 次/业务请求，证明当时绝大多数执行没有走到这些 slow call。

验证结束后，源码默认值与安装在
`cangjie_compiler_region_mark/output/msgc` 的最终 SDK 均恢复为 fastpath ON，stdlib 与
stdx 也已随最终 ON SDK 再次完整重编。此后 SDK 又随 outside-set Small 扩展重新构建；
9.1～9.4 的当前计数基于后一次构建。

## 10. 当前数据指向的后续优化

### 10.1 A/B 完成：LLVM pending bit 去重（当前已移除）

曾在 AArch64/X86 inline fast-add 的 `KeyFound` 路径加入一次
`entry.mask & bitMask` test：已 pending 时直接完成当前写入，不执行 OR、不增加
`pendingBitCount`，只有新增 bit 才检查 threshold 并更新 mask。为判断额外 test/branch 是否值得，
使用完整重编的 compiler、Runtime、stdlib、stdx 和重新编译的 case27 做了相邻三轮 A/B。
两侧都开启 Runtime 热路径计数，配置均为 10K、并发 10、10 秒、4GB virtual heap：

| LLVM pending 去重 | 业务请求 | GC cycle | threshold flush | set-conflict flush | 总 flush | 平均吞吐 |
|---|---:|---:|---:|---:|---:|---:|
| 开启 | 319684 | 647 | 480918 | 61096 | 542014 | 10536 req/s |
| 移除 | 318936 | 648 | 548556 | 66186 | 614742 | 10515 req/s |

移除去重后，业务请求只变化 -0.23%，但总 flush 增加 13.42%；按业务请求归一化增加
13.68%，按 GC cycle 归一化增加 13.24%。平均吞吐只变化 -0.20%，远小于三轮间波动，不能判定
去重带来吞吐收益。它确实降低 flush，但 `KeyFound` 热路径增加的 test/branch 与减少的 slow edge
在该负载下基本抵消。

因此当前实现按本次实验要求移除了 LLVM pending-bit 去重：重复 bit 继续执行幂等 OR 并消耗
保守的 `pendingBitCount` 预算，Runtime 自身的去重保持不变。此前与更早历史结果比较得到的约
34% flush 降幅混入了运行时序和其他环境差异；评估该单项优化应以这里的相邻 A/B 为准。

### 10.2 第二优先级：为 threshold/conflict 提供专用 slow helper

当前 LLVM 已经算出 `buffer + wordKey + bitMask`，但 threshold 或 4-way conflict 时仍回到完整
wrapper。Runtime 随后重新执行 phase 判断、读取 oldRef、解析 Region/slot，再 flush 和插入
当前 bit。本次无去重三轮共有 614742 次这类请求，值得避免重复工作。

可增加内部 leaf slow helper，直接接收已验证的 buffer、`wordKey`、`bitMask` 和冲突原因：

1. Runtime flush；
2. 将当前 bit 插入清空后的 buffer；
3. 返回 emitter，由 emitter 完成原 store；
4. Large、非 SMALL、无 AllocBuffer、复杂 bulk 等仍走原 wrapper。

该方案不能消除 Runtime call，但能消除 flush slow edge 上的重复 barrier 入口、oldRef load 和
Region 坐标解析。helper 仍必须保持为非 safepoint、owner-local buffer 操作，并在 AArch64/X86
上保存 ABI 要求的原写入参数。

### 10.3 第二优先级：缩小 Array bulk 的 Runtime 边界

当前剩余 1559231 次 wrapper 中，`array_ref + array_struct` 为 634641 次，占 40.70%。不宜把
任意长度 bulk loop 全部展开到每个 callsite；这会显著增加代码尺寸和寄存器压力。更合适的是：

- LLVM 只内联长度为编译期常量且很小的 ArrayRef copy，展开少量 old-ref load 并复用已有
  leaf fast-add；
- ArrayStruct 仅在 component GCTib 和元素数均可静态确定、总 ref slot 数较小时展开；
- 动态长度、RawArray 动态 TypeInfo 和较大 copy 继续回 Runtime；
- Runtime bulk visitor 可按 `(wordKey, mask)` 小批量合并后再写 TLS buffer，减少逐 oldRef
  的函数调用和重复 lookup。

Runtime 已经缓存 collector/buffer/epoch，并去除连续相同 oldRef；后续优化应以减少
`AddManagedRefToBarrierBuffer()` 的逐引用调用为目标，而不是再次增加 phase 或 owner 查询。

### 10.4 第三优先级：处理 4-way conflict

本次无去重三轮的 4-way conflict 为 66186 次。当前任意一个 set 冲突都会 flush
整个 32-entry buffer。建议先按 set 记录冲突分布、flush 时 occupancy 和 unique-bit 数，再选择：

- 改善 `(regionIdx, wordIdx)` 的 hash，降低热点 key 聚集；
- 只 flush 冲突 set 或一个 victim entry，并正确扣减其 unique `pendingBitCount`；
- 若仍需要调整 set/way 几何，必须同步 compiler/runtime 隐藏 ABI，并评估额外 lookup 指令。

不建议在没有分布数据时直接扩大 way 数或 buffer：这可能用更长的每次 fast-add 查询换取较少
冲突，也会延长旧边从 TLS buffer 发布到全局 mark plane 的时间。

### 10.5 Runtime 热路径清理与读屏障结论

- `CJ_MSGC_BARRIER_COUNT` 相关代码是临时诊断设施。即使环境变量未设置，当前热路径仍保留
  gate/function-call；设置后还会执行原子累加。完成上述定向测量后应整体删除，再做无计数吞吐
  A/B。
- 在“oldRef 永远是合法、已发布对象起始引用”的工程不变量下，Runtime outside-set 新 bit
  路径的额外 TypeInfo 非空检查可以移到 debug；`BarrierMarkContext` 为该分类缓存
  `currentMarkEpoch_` 的 acquire load 也可随临时 collect/outside 统计一起移出逐 barrier 路径。
- Runtime flush 的 collect-set alloc-plane 过滤、全局 mark-plane 过滤和原子
  `fetch_or/newBits` 仍承担正确性或跨线程去重职责，不应根据本次 wrapper 数直接删除。
- 普通 Ref/Struct 读在 LLVM 已是 direct load/memcpy，当前数据没有显示普通读屏障还有主要
  优化空间。Atomic load 仍走 Runtime；只有新增调用计数证明其为热点时，才值得在 LLVM 保留
  memory-order 语义地直接 lowering。

综合优先顺序调整为：**threshold/conflict 专用 helper → 小型常量 bulk inline/Runtime bulk
合并 → set-conflict 局部化**。LLVM pending-bit 去重可降低约 13% 的 flush，但本次 A/B 没有
观测到吞吐收益，因此当前不保留。也不建议仅靠提高 128 阈值掩盖重复计数；这会增加 SATB
旧边在 mutator TLS 中滞留的时间。
