# Step35：CopyGC 与 MSGC 屏障语义及 Inline 实现对比

> 记录时间：2026-07-05
> 目的：把近期排查中反复问到的几个对比点先沉淀下来，避免后续把 CopyGC 和 MSGC 的屏障语义、以及 MSGC 的 read/write inline 链路混为一谈。

---

## 1. 先给结论

1. CopyGC 和 MSGC 的写屏障不是“同一语义的两种实现”。
2. `MCC_WriteRefField / MCC_WriteStructField / MCC_WriteStaticRef / MCC_WriteStaticStruct` 这一层入口 precheck，两者基本一致；真正不同的是进 barrier 之后做什么。
3. `WriteGeneric` 只有“源和目标都不在堆里时直接 memcpy”这一个大分支看起来相似，完整逻辑并不相同。
4. MSGC 的 read inline 和 write inline 也不是一条链路：
   - read 侧主要在 `CJBarrierLowering.cpp` 里直接改成 load/memcpy；
   - write 侧则是 `CJBarrierLowering.cpp` 先改成 wrapper，再由 `AArch64AsmPrinter.cpp` 在 callsite 发射 inline fastpath。

---

## 2. CopyGC 与 MSGC 的 runtime 屏障语义差异

### 2.1 phase 模型不同

CopyGC 在 [src/Heap/Heap.cpp:253](../../../src/Heap/Heap.cpp) 按 phase 切多套 barrier：

- `GC_PHASE_ENUM -> EnumBarrier`
- `GC_PHASE_TRACE / GC_PHASE_CLEAR_SATB_BUFFER -> TraceBarrier`
- `GC_PHASE_POST_TRACE -> PostTraceBarrier`
- `GC_PHASE_IDLE -> IdleBarrier`
- `GC_PHASE_PREFORWARD / GC_PHASE_FORWARD -> PreforwardBarrier / ForwardBarrier`

MSGC 在 [src/Heap/Heap.cpp:231](../../../src/Heap/Heap.cpp) 只区分两大类：

- idle / finish / reclaim / sweep -> `msIdleBarrier`
- mark setup / concurrent mark / mark termination -> `msMarkBarrier`

也就是说：

- CopyGC 的 barrier 同时承担了 SATB、tagged pointer 修正、moving collector 读写协作；
- MSGC 当前 barrier 重点只有一件事：并发标记期保住 old edge。

### 2.2 为什么 CopyGC `EnumBarrier` 会记 old + new，而 `TraceBarrier` 只记 old

代码事实：

- `EnumBarrier::WriteReference()` 会 `RememberObjectInSatbBuffer(old)`，也会在 `ref != nullptr` 时记录 `new`，[src/Heap/WCollector/EnumBarrier.cpp:107](../../../src/Heap/WCollector/EnumBarrier.cpp)
- `TraceBarrier::WriteReference()` 只记录 old，[src/Heap/WCollector/TraceBarrier.cpp:102](../../../src/Heap/WCollector/TraceBarrier.cpp)
- `WCollector::TraceHeap()` 的 phase 顺序是先 `ENUM`，再 `TRACE`，[src/Heap/WCollector/WCollector.cpp:370](../../../src/Heap/WCollector/WCollector.cpp)

基于这些源码，可以做一个明确标注为“推断”的结论：

> 推断：CopyGC 在 `ENUM` 这个 root enumeration 窗口采用了更保守的策略，既保旧边也保新边；进入 `TRACE` 后，snapshot 已立住，才退回到更标准的 SATB old-only 语义。

MSGC 没有这套 `ENUM/TRACE` 双语义窗口。`MSMarkBarrier` 从头到尾都是 deletion-style 的 `log(old) + store`，[src/Heap/MBarrier/MSMarkBarrier.cpp:134](../../../src/Heap/MBarrier/MSMarkBarrier.cpp)。

### 2.3 普通字段写入口：两者 precheck 基本一致

入口在共享的 [src/CompilerCalls.cpp:283](../../../src/CompilerCalls.cpp)：

- `MCC_WriteRefField`：只看 `obj` 是否在堆里；不在堆里就直接 `SetTargetObject`
- `MCC_WriteStructField`：只看 `obj` 是否在堆里；不在堆里就直接 `memcpy`
- `MCC_WriteStaticRef / MCC_WriteStaticStruct`：不做这个 precheck，直接进 barrier

所以这层结论是：

- CopyGC 和 MSGC 在 `MCC_Write*` 入口 precheck 上是一致的；
- static 写之所以两边都直接进 barrier，是因为 static slot 本身是 root，不是因为它“在堆里”。

### 2.4 真正的差异在 barrier 内部

CopyGC：

- `TraceBarrier::WriteReference()`：记 old，然后 tag/store new
- `EnumBarrier::WriteReference()`：记 old + new，然后 tag/store new
- `PostTraceBarrier::WriteReference()`：不做 SATB remember，主要是 tag/store

MSGC：

- `MSMarkBarrier::WriteReference()`：取 old，`EnqueueRefFast(old)`，然后直接 store new，[src/Heap/MBarrier/MSMarkBarrier.cpp:134](../../../src/Heap/MBarrier/MSMarkBarrier.cpp)
- `MSMarkBarrier::WriteStaticRef()`：同样是 old-only，[src/Heap/MBarrier/MSMarkBarrier.cpp:147](../../../src/Heap/MBarrier/MSMarkBarrier.cpp)

### 2.5 `WriteGeneric`：只有一部分像，完整逻辑不一样

CopyGC：

- `TraceBarrier::WriteGeneric()` / `EnumBarrier::WriteGeneric()` 都有专门 override，[src/Heap/WCollector/TraceBarrier.cpp:307](../../../src/Heap/WCollector/TraceBarrier.cpp)、[src/Heap/WCollector/EnumBarrier.cpp:333](../../../src/Heap/WCollector/EnumBarrier.cpp)
- 它们会按 `(obj 是否在堆里, src 是否在堆里)` 做四路分流：
  - 两边都非堆：`memcpy`
  - dst 非堆、src 在堆：`ReadStruct`
  - dst 在堆、src 非堆：`WriteStruct`
  - 两边都在堆：先 `ReadStruct` 再 `WriteStruct`

MSGC：

- 没有单独的 `MSMarkBarrier::WriteGeneric()` override，走的是基类 [src/Heap/Barrier/Barrier.cpp:209](../../../src/Heap/Barrier/Barrier.cpp)
- 同样保留了“两边都非堆 -> memcpy”这个大分支
- 但在 `obj` 在堆里的情况下，不再像 CopyGC 那样先做 source-side 的 read/fixup，而是统一收敛到 `WriteStruct`

因此更准确的说法是：

> `WriteGeneric` 只有“纯非堆 memcpy”这一支和 MSGC 看起来一致；只要牵涉堆对象，CopyGC 和 MSGC 的处理模型就已经不同了。

### 2.6 `ArrayCopy` 也不能直接类比

CopyGC：

- `CopyRefArray()` 的实现写在 `IdleBarrier`，[src/Heap/WCollector/IdleBarrier.cpp:164](../../../src/Heap/WCollector/IdleBarrier.cpp)
- 但里面调用的是虚函数 `WriteReference()`，[src/Heap/Barrier/Barrier.h:37](../../../src/Heap/Barrier/Barrier.h)
- 所以在 `TraceBarrier / EnumBarrier` phase 下，数组逐元素写仍会动态分派到对应 phase 的 `WriteReference()`

MSGC：

- `MSMarkBarrier::CopyRefArray()` / `CopyStructArray()` 都是“先枚举并记录 dst 被覆盖掉的 old refs，再 memmove”，[src/Heap/MBarrier/MSMarkBarrier.cpp:195](../../../src/Heap/MBarrier/MSMarkBarrier.cpp)

这也是后面排查 `cj_array_copy_ref / struct` inline 问题时，不能直接用 CopyGC 直觉套 MSGC 的原因。

---

## 3. MSGC 读/写屏障 inline 实现对比

## 3.1 总览表

| 维度 | MSGC 读侧 | MSGC 写侧 |
|---|---|---|
| 入口开关 | `EnableMSGCReadBarrierLowering` | `EnableMSGCWriteBarrierLowering` |
| 主要文件 | `llvm/lib/CodeGen/CJBarrierLowering.cpp` | `CJBarrierLowering.cpp` + `AsmPrinter.cpp` + `AArch64AsmPrinter.cpp` |
| 是否先生成 wrapper | 否 | 普通写路径会；arraycopy 相关代码路径当前处于“实现已在代码中，但未完全激活”状态 |
| 是否依赖 AArch64 发射层 | 否 | 是 |
| 当前本质 | 直接消 barrier，改成 load/memcpy | 仍保留 barrier 语义，但尽量在 callsite inline |
| 是否读 GC/tag 状态 | 不读 `g_msgcWBEnabled` | 会读 TLS `gcFastState`（由 runtime 根据 `g_msgcWBEnabled + encodedGCPhase` 刷新） |
| 当前 arraycopy 是否走 inline | 不涉及 | 当前工作区里不应视为已生效：`CJBarrierLowering` 里有 arraycopy wrapper 代码，但 `shouldLowerMSGCWriteBarrier()` 当前未把 arraycopy 放进触发名单，`AsmPrinter` 分发名单里也不含它们 |

## 3.2 MSGC 读侧 inline：本质是 lowering 期直接消掉 barrier

实现入口：

- `shouldLowerMSGCReadBarrier()`：[llvm/lib/CodeGen/CJBarrierLowering.cpp:258](../../../../cangjie_compiler/third_party/llvm-project/llvm/lib/CodeGen/CJBarrierLowering.cpp)
- `replaceInstWithMSGCReadAccess()`：[llvm/lib/CodeGen/CJBarrierLowering.cpp:453](../../../../cangjie_compiler/third_party/llvm-project/llvm/lib/CodeGen/CJBarrierLowering.cpp)

当前行为：

- `cj_gcread_ref / weakref / static_ref` -> direct load
- `cj_gcread_struct / static_struct` -> `llvm.memcpy`
- `cj_gcread_generic` -> 借用 struct-copy helper 做 lowering，[llvm/lib/CodeGen/CJBarrierLowering.cpp:499](../../../../cangjie_compiler/third_party/llvm-project/llvm/lib/CodeGen/CJBarrierLowering.cpp)

这条链路的关键点是：

1. 没有 MSGC 专用 read wrapper；
2. 没有 `AsmPrinter` callsite 特判；
3. 没有 AArch64 专属手写汇编路径；
4. 目标就是把 MSGC 下本来“不需要 read barrier”的地方尽量提早变成普通 IR load/memcpy。

## 3.3 非 MSGC 的通用 read fastpath，不等于 MSGC read inline

`CJBarrierLowering::readBarrierFastPath()` 里的 tagged-pointer fastpath 是另一套机制，[llvm/lib/CodeGen/CJBarrierLowering.cpp:1368](../../../../cangjie_compiler/third_party/llvm-project/llvm/lib/CodeGen/CJBarrierLowering.cpp)。

它做的是：

- 先 load 指针
- 看高位 tag
- 未标记则直接走 fast 分支
- 否则回到 slow gcread call

这套是 CopyGC / tagged pointer 模型的读快路径。MSGC 的读侧优化不是“也走这套”，而是更早在 lowering 阶段把很多 gcread 直接删掉。

## 3.4 MSGC 写侧 inline：wrapper + AArch64 发射层

第一步，`CJBarrierLowering.cpp` 为 MSGC 写侧准备了这些 wrapper 名称与替换逻辑：

- `CJ_MCC_WriteRefFieldFast_MSGC`
- `CJ_MCC_WriteStaticRefFast_MSGC`
- `CJ_MCC_WriteStructFieldFast_MSGC`
- `CJ_MCC_WriteStaticStructFast_MSGC`
- `CJ_MCC_ArrayCopyRefFast_MSGC`
- `CJ_MCC_ArrayCopyStructFast_MSGC`

源码位置：[llvm/lib/CodeGen/CJBarrierLowering.cpp:298](../../../../cangjie_compiler/third_party/llvm-project/llvm/lib/CodeGen/CJBarrierLowering.cpp)

但要特别区分“实现存在”和“当前会被触发”：

- 当前 `shouldLowerMSGCWriteBarrier()` 只对 `cj_gcwrite_ref / static_ref / struct / static_struct / generic` 返回 true，[llvm/lib/CodeGen/CJBarrierLowering.cpp:276](../../../../cangjie_compiler/third_party/llvm-project/llvm/lib/CodeGen/CJBarrierLowering.cpp)
- 也就是说，arraycopy 的 wrapper 分支虽然保留在代码里，但在这版工作区里不能默认认为已经进入 MSGC write-barrier lowering 主路径

第二步，`AsmPrinter::tryEmitCangjieSpecificCallByMOSym()` 只对进入分发名单的 wrapper 调 `emitMccWriteBarrierFastPath()`，[llvm/lib/CodeGen/AsmPrinter/AsmPrinter.cpp:4144](../../../../cangjie_compiler/third_party/llvm-project/llvm/lib/CodeGen/AsmPrinter/AsmPrinter.cpp)

注意当前工作区状态：

- 当前分发名单只有 4 个普通写 wrapper：
  - `WriteRefFieldFast_MSGC`
  - `WriteStructFieldFast_MSGC`
  - `WriteStaticRefFast_MSGC`
  - `WriteStaticStructFast_MSGC`
- `ArrayCopyRefFast_MSGC / ArrayCopyStructFast_MSGC` 当前不在这个 dispatch 名单里

这意味着：

> 就当前工作区而言，arraycopy 不应被视为“已经在走 MSGC callsite inline emission”。即使相关实现代码还在，真正是否生效仍取决于 lowering 触发名单和 `AsmPrinter` dispatch 名单是否同时放开。

第三步，真正的 AArch64 inline 发射在 [llvm/lib/Target/AArch64/AArch64AsmPrinter.cpp:2493](../../../../cangjie_compiler/third_party/llvm-project/llvm/lib/Target/AArch64/AArch64AsmPrinter.cpp)

它的核心结构是：

1. 从 `X28` 指向的 CJTLS 直接读单字节 `gcFastState`
2. `bit0 == 0` 时直接 `StoreOnly`
3. `ref/staticref` 分支里，`bit2 == 0` 走旧的 old-only 记录；`bit2 == 1` 进入 enum 语义
4. 对 heap object 写会做 heap-range check
5. enum 语义下：
   - `old != null && new != null` 时，按 `old + new` 两条记录追加到 `wbBuf`
   - 只有一边非空时，只追加那一边
6. `struct/static struct` 这几条 inline 目前仍保持旧逻辑，不读取 `bit2`
7. 最后再执行 store / memcpy / memmove

## 3.5 当前 MSGC 写 inline 还依赖哪些运行时事实

当前发射层隐含依赖：

- inline 本身只消费 TLS `gcFastState`：
  - `bit0` = MSGC 并发写屏障已安装
  - `bit2` = `GC_PHASE_MS_ENUM` 的 ref/staticref old+new 语义
- `gcFastState` 由 runtime 在 mutator 进入托管态时，根据全局 `g_msgcWBEnabled` 与 TLS `encodedGCPhase` 刷新
- `X28` 可拿到当前线程 TLS
- TLS 中能取到 `AllocBufferMS*`
- `wbBuf` 布局与 `MSGCWBBufferOffsetInAllocBuffer / CountOffset / Capacity` 对齐
- heap-range 判断使用 runtime 发布的保留堆边界

另外，runtime 当前已经把原 `GC_PHASE_MS_MARK_SETUP` 重命名为 `GC_PHASE_MS_ENUM`，并拆成两类屏障：

- `GC_PHASE_MS_ENUM` -> `MSEnumBarrier`
  - 当前只在 `WriteRefField / WriteStaticRef` 上执行 `old + new`
- `GC_PHASE_MS_CONCURRENT_MARK / GC_PHASE_MS_MARK_TERMINATION` -> `MSMarkBarrier`
  - 继续保持 `old-only`

所以 MSGC 写 inline 问题，很多时候不是 `MCC_Write*` 本身错了，而是：

- 全局 gate 错了
- heap-range 判断错了
- TLS / `AllocBufferMS` 取值错了
- 或者 arraycopy wrapper 根本没进入当前 dispatch

---

## 4. 本文对后续排查的直接意义

1. 不要把 CopyGC `EnumBarrier` 的 `old + new` 逻辑整体投射到 MSGC；当前 MSGC 只有 `GC_PHASE_MS_ENUM` 下的 `WriteRefField / WriteStaticRef` 是 old+new，其余写路径仍不是这套语义。
2. `MCC_Write*` 入口 precheck 一样，不代表 barrier 语义一样。
3. `WriteGeneric` 不能只看“非堆 memcpy”那一支；只要 source/destination 里有堆对象，CopyGC 与 MSGC 的路径已经分叉。
4. MSGC read inline 验证通过，不代表 write inline 也没问题；两者代码路径完全不同。
5. 排查 `cj_array_copy_ref / struct` 时，先确认当前工具链里它们究竟是：
   - 仅 lowering 成 wrapper；
   - 还是已经进入 `AsmPrinter` dispatch 并发射了 inline。
