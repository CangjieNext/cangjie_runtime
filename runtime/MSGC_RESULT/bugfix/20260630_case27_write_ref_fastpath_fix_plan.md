# 2026-06-30 case27 `WriteRef*Fast_MSGC` 修复方案

## 1. 问题现象

`case27_http_complex_json` 在 MSGC 下跑高压参数：

- `CASE27_CONCURRENCY=1000`
- `CASE27_PAYLOAD=100K`
- `CASE27_DURATION_MS=60000`

会在运行约 `22s` 左右随机崩溃，典型栈为：

- `std/core/array_common.cj::__CNat14CPointerHandle...`
- `stdx/encoding/json/write_buffer.cj::__CN18stdx.encoding.json11WriteBuffer12appendEscape...`

表现为：

- `SIGBUS` / `SIGUSR1`
- 崩溃点落在 JSON 序列化阶段
- 但真正损坏通常发生在更早的对象引用写入路径

## 2. 已经排除和已经确认的点

### 2.1 `WriteStructFieldFast_MSGC` 不是主因

只禁用 `CJ_MCC_WriteStructFieldFast_MSGC` 后，`case27` 仍然会崩：

- 编译目录：
  - `MSGC_RESULT/Result/case27_http_complex_json/result/20260630_structfast_probe_manual`
- 运行目录：
  - `MSGC_RESULT/Result/case27_http_complex_json/result/20260630_structfast_probe_manual_run_c1000_p100K`

因此这次 case27 不应继续优先怀疑 `WriteStructFieldFast_MSGC`。

### 2.2 禁用 `WriteRefFieldFast/WriteStaticRefFast` 后，问题消失

把 `cj_gcwrite_ref` / `cj_gcwrite_static_ref` 改成绕开 AArch64 fast emission 的 wrapper 后：

- `MSGC_RESULT/Result/case27_http_complex_json/result/20260630_231007_refslowprobe_manual_compile`

重新编译 case27，并两次跑同样的 `c1000 + p100K`：

- `20260630_231217_refslowprobe_manual_run2_c1000_p100K`
  - `exit=0`
  - `throughput=282 req/s`
  - `stderr.txt` = 0 行
- `20260630_231954_refslowprobe_manual_run3_c1000_p100K`
  - `exit=0`
  - `throughput=265 req/s`
  - `stderr.txt` = 0 行

这说明：

- 崩溃与 `CJ_MCC_WriteRefFieldFast_MSGC`
- `CJ_MCC_WriteStaticRefFast_MSGC`

高度相关，已经不是泛化的“runtime 整体不稳定”问题。

## 3. 根因判断

当前高置信根因不是 runtime 慢路径本身，而是：

- `llvm/lib/Target/AArch64/AArch64AsmPrinter.cpp`
  - `AArch64AsmPrinter::emitMccWriteBarrierFastPath`

里面针对：

- `CJ_MCC_WriteRefFieldFast_MSGC`
- `CJ_MCC_WriteStaticRefFast_MSGC`

的 **AArch64 callsite fast emission**。

### 3.1 慢路径语义

慢路径走：

- `runtime/src/CompilerCalls.cpp`
  - `MCC_WriteRefField`
  - `MCC_WriteStaticRef`
- `runtime/src/Heap/MBarrier/MSMarkBarrier.cpp`
  - `MSMarkBarrier::WriteReference`
  - `MSMarkBarrier::WriteStaticRef`

核心语义是：

1. 读出 `oldRef = field.GetTargetObject()`
2. `EnqueueRefFast(oldRef)`
3. `field.SetTargetObject(ref)`

### 3.2 当前 ref fast path 的做法

`AArch64AsmPrinter.cpp` 中 `IsRef / IsStaticRef` 分支现在会直接：

1. `ldr` 原字段值
2. 直接按 TLS + 固定 offset 写 `AllocBufferMS::wbBuf_`
3. 直接 `str` 新值回字段

代码位置见：

- `llvm/lib/CodeGen/AsmPrinter/AsmPrinter.cpp`
  - 特判 fast wrapper 名字
- `llvm/lib/Target/AArch64/AArch64AsmPrinter.cpp`
  - `emitMccWriteBarrierFastPath`
  - `2493+` 识别 `IsRef / IsStaticRef`
  - `2657+` 执行旧值读取、heap check、`wbBuf_` 直写、字段直写

### 3.3 为什么怀疑它

因为它和慢路径存在两类关键差异：

1. **直接操作 `RefField` 原始 64-bit 值**
   - 没有复用 `field.GetTargetObject()`
   - 没有复用 `field.SetTargetObject(ref)`
   - 等于绕开了 `RefField` 的统一封装语义

2. **直接写 `AllocBufferMS::wbBuf_`**
   - 依赖 TLS 里的 `msAllocBuffer`
   - 依赖固定 offset：
     - `MSGCWBBufferOffsetInAllocBuffer = 896`
     - `MSGCWBBufferCountOffset = 8192`
   - 没有复用 runtime helper 那套 `GetOrCreateAllocBufferMS()` / overflow flush / owner lifecycle 路径

这两个差异里，任一处和 runtime 实际语义不一致，都足以造成：

- old edge 漏记
- 已存活对象被误回收
- 后续在 JSON writer / String / Array 路径上表现成随机对象损坏

## 4. 应该怎么修

### 4.1 最小安全修复：直接禁用 ref/static-ref fast emission

这是当前最推荐的正式修复。

做法：

- 在 `llvm/lib/CodeGen/AsmPrinter/AsmPrinter.cpp` 里，不再把下面两个名字送进
  `emitMccWriteBarrierFastPath`：
  - `CJ_MCC_WriteRefFieldFast_MSGC`
  - `CJ_MCC_WriteStaticRefFast_MSGC`

保留其它仍可用的 fast path：

- `CJ_MCC_WriteStructFieldFast_MSGC`
- `CJ_MCC_WriteStaticStructFast_MSGC`
- `CJ_MCC_ArrayCopyRefFast_MSGC`
- `CJ_MCC_ArrayCopyStructFast_MSGC`

这样编译后的 callsite 会退化成：

- 普通 `bl CJ_MCC_WriteRefFieldFast_MSGC`
- 普通 `bl CJ_MCC_WriteStaticRefFast_MSGC`

而 wrapper 自身仍然只是 thin slow body，会再进入：

- `CJ_MCC_WriteRefField`
- `CJ_MCC_WriteStaticRef`

这相当于：

- **不改 runtime 行为**
- **只关掉 AArch64 ref/static-ref 的危险 inline fastpath**
- 改动面最小，定位最精准

### 4.2 当前验证用修法：改 wrapper 名字绕开 fastpath

这就是已经验证过的方式：

- `CJBarrierLowering.cpp`
  - `cj_gcwrite_ref -> CJ_MCC_WriteRefFieldSlowProbe_MSGC`
  - `cj_gcwrite_static_ref -> CJ_MCC_WriteStaticRefSlowProbe_MSGC`

优点：

- 最容易做 A/B 验证
- 已经证明能消掉 case27 崩溃

缺点：

- 更像诊断版 / probe 版修法
- 不如直接在 `AsmPrinter.cpp` 关掉这两个 fastpath 来得干净

因此：

- **验证阶段可以继续保留**
- **正式修复建议改成 4.1 那种“保留 wrapper 名字，但不做 fast emission”**

### 4.3 长期修复：重写 ref/static-ref fast path，而不是继续带病上线

如果后续一定要拿回这两个 fastpath 的性能，建议不要在当前实现上小修小补，而是：

1. 先保留 4.1 的禁用修复，确保 correctness
2. 另起一轮重写 `IsRef / IsStaticRef` 分支

长期修法建议二选一：

#### 方案 A：完全复用 runtime helper

让 AArch64 callsite fast emission 不再自己写 `wbBuf_`，而是只做少量快速判断后直接调用一个专用 runtime helper，例如：

- `MRT_MSGC_WriteRefFieldFastChecked(...)`
- `MRT_MSGC_WriteStaticRefFastChecked(...)`

helper 内部统一复用：

- `field.GetTargetObject()`
- `EnqueueRefFast(oldRef)`
- `field.SetTargetObject(ref)`

优点：

- 语义与 runtime 完全一致
- 不再手写 `wbBuf_` ABI

缺点：

- 性能可能不如纯 inline

#### 方案 B：保留 inline，但必须和 runtime 语义逐项对齐

如果坚持 inline，则至少要重新审计并证明下面几点：

1. 原始 `ldr/str` 与 `RefField` 封装语义完全一致
2. 直接访问 TLS `msAllocBuffer` 不会踩生命周期窗口
3. 直接写 `wbBuf_` 与 `EnqueueRefFast()` 的 overflow / owner / flush 语义完全一致
4. 对 `heapCurrentEnd` 这类全局状态的读取方式与 runtime 线程可见性要求一致

在这些点全部被代码和压测证明之前，不建议重新启用。

## 5. 推荐执行顺序

### 第一阶段：正式止血

1. 改 `AsmPrinter.cpp`，禁用：
   - `CJ_MCC_WriteRefFieldFast_MSGC`
   - `CJ_MCC_WriteStaticRefFast_MSGC`
2. 增量编译 LLVM
3. 用新 compiler 重新编 `stdlib` / `stdx`
4. 重新编 `case27`
5. 回归跑：

## 2026-06-30 补充：当前实现修正方向

上面的“直接禁用 `WriteRefFieldFast_MSGC` / `WriteStaticRefFast_MSGC` callsite fast emission”
是前一阶段的止血建议；当前更贴近最终形态的修法已经调整为：

1. **保留** `CJ_MCC_WriteRefFieldFast_MSGC` / `CJ_MCC_WriteStaticRefFast_MSGC`
2. 在 `AArch64AsmPrinter.cpp` 中把 `RefField` 语义补齐为：
   - `GetTargetObject()` = `load fieldRaw` 后做 `mask48`
   - `SetTargetObject(ref)` = `ref` 先做 `mask48` 再 `store`
3. `EnqueueRefFast(oldRef)` 的 common path 改为 inline：
   - `oldRef == null` / `oldRef` 非 heap -> 直接 `StoreOnly`
   - `allocBuf == nullptr` -> `Slow`
   - `wbBuf full` -> `Slow`
   - 其余情况走 inline `wbBuf.Push(oldRef)`
4. 其它会“扫描旧 ref 再入 wbBuf”的 MSGC bulk fastpath 也必须先做 `mask48`，包括：
   - `WriteStructFieldFast_MSGC`
   - `WriteStaticStructFast_MSGC`
   - `ArrayCopyRefFast_MSGC`
   - `ArrayCopyStructFast_MSGC`

原因是这些路径虽然不是单 slot `WriteRef*`，但它们枚举旧字段时本质上也在复用
`RefField::GetTargetObject()` 的语义；若直接把 64-bit slot 原值当作对象指针去做
`null/heap-check/push`，仍然会和 runtime 慢路径产生偏差。
   - `c1000 + p100K`
   - 至少 2 次

### 第二阶段：回归面扩展

再跑：

- `c10 + p10K`
- `c100 + p10K`
- `c1000 + p10K`
- `c1000 + p100K`

目标是确认：

- 崩溃消失
- 没引入新的明显性能回退
- `WriteStructFieldFast_MSGC` 相关 case 仍正常

### 第三阶段：如果要恢复性能，再单独重做 ref fastpath

先 correctness，后 performance。

当前阶段不建议把“修 crash”和“重新设计 ref fastpath”绑成一个 patch。

## 6. 一句话结论

当前最合理的修复不是继续查 `WriteStructFieldFast_MSGC`，而是：

- **正式禁用 AArch64 的 `WriteRefFieldFast_MSGC` / `WriteStaticRefFast_MSGC` callsite fast emission**
- 让它们回到已有的 wrapper slow path
- 等 correctness 稳定后，再单独重写 ref/static-ref fastpath

## 7. 2026-07-01 当前状态更新（以本节为准）

这一节记录的是当前代码树里已经实际落地的修复、做过的诊断，以及截至
`2026-07-01` 仍未解决的问题。若与前文阶段性推测冲突，以本节为准。

### 7.1 当前已经落地的修复 / 调整

1. **LLVM 侧 `wbBuf_.count_` 偏移已修正**

- 文件：
  - `llvm/lib/Target/AArch64/AArch64AsmPrinter.cpp`
- 现状：
  - `MSGCWBBufferCountOffset` 已从硬编码 `8192` 改成
    `MSGCWBBufferOffsetInAllocBuffer + MSGCWBBufferCapacity * 8`
  - 当前值实际为 `9088`
- 含义：
  - 已经修掉了 `wbBuf_` 基址 `896` 没加到 `count_` 上的 off-by-896 问题
  - 编译器 inline fast path 和 runtime `WriteBarrierBuffer::count_` 的 ABI
    不再错位

2. **此前的 wrapper 改名 probe 已经回退**

- 当前 `llvm` / `CJBarrierLowering` / `AsmPrinter` 仍使用：
  - `CJ_MCC_WriteRefFieldFast_MSGC`
  - `CJ_MCC_WriteStaticRefFast_MSGC`
- `CJ_MCC_WriteRefFieldSlowProbe_MSGC` /
  `CJ_MCC_WriteStaticRefSlowProbe_MSGC` 这类诊断 wrapper 不再是当前工作树方案

3. **runtime slow path 仍保留 `obj` 是否在 heap 的判断**

- 文件：
  - `runtime/src/CompilerCalls.cpp`
- 现状：
  - `MCC_WriteRefField()` 里仍然先做 `if (!Heap::IsHeapAddress(obj))`
  - 非 heap `obj` 直接 `field->SetTargetObject(ref)` 返回
- 同时：
  - `MSMarkBarrier::WriteReference()` / `WriteStaticRef()` 内部不再重复判断 `obj`
  - 这符合“入口判断一次，真正进入 barrier 后不再重复判断”的当前策略

4. **runtime flush 路径已经补了 old-ref 是否在 heap 的过滤**

- 文件：
  - `runtime/src/Heap/MCollector/MarkSweepCollector.cpp`
- 已补位置：
  - `FlushWriteBarrierOverflow()`
  - `FlushExitedWriteBarrierBuffer()`
  - `FlushAllWriteBarrierBuffers()`
- 现状：
  - 在真正 `DiscoverObject()` / `TryEnqueueGreyObject()` 之前，先过滤
    `!Heap::IsHeapAddress(obj)`
- 含义：
  - 即使 inline fast path 把非 heap old-ref 塞进了 `wbBuf_`，flush 时也不会继续把
    它送进下一跳标记队列

5. **做过一次 `MS_MARK_SETUP` 的 STW 诊断改动**

- 文件：
  - `runtime/src/Heap/MCollector/MarkSweepCollector.cpp`
- 改动：
  - `EnumRoots()` 里把
    `TransitionToGCPhase(GCPhase::GC_PHASE_MS_MARK_SETUP, true)`
    改成了一个真实的
    `ScopedStopTheWorld("MS MarkSetup", true, GCPhase::GC_PHASE_MS_MARK_SETUP)`
- 说明：
  - 这是诊断 TLS phase 同步窗口的临时实验，不是已经证明有效的正式修复

### 7.2 已经验证过、但不能解释当前 crash 的方向

1. **`WriteStructFieldFast_MSGC` 不是当前 case27 主因**

- 只关 `struct fastpath` 时，case27 仍可复现崩溃
- 这一点已在前文 `20260630_structfast_probe_manual*` 结果中记录

2. **“MSGC 使用指针高位 / tagged ref 语义”这条线不成立**

- 当前 MSGC 不走 CopyGC 那套 tagged-pointer 转移语义
- 因而如果假设“高 16 位本身不会被污染”，那么 `mask48` 对 MSGC ref 语义基本等价于
  no-op
- 这意味着：
  - 不能再把 case27 主因继续归到 `RefField tagged 语义不一致`

3. **仅把 `MS_MARK_SETUP` phase 切换包进 STW，不能修掉 case27**

- 已使用 `compiler/output` 这套 SDK 重编 runtime、替换 dylib、重新编 case27 后复测
- 结果目录：
  - `MSGC_RESULT/Result/case27_http_complex_json/result/20260701_164501_compiler_output_sdk_marksetup_stw_runtime_c1000_p100K`
- 结果：
  - `exit.code = 138`
  - 仍然崩溃
  - 崩溃时间约 `10s`，并未消失
- 含义：
  - “TLS phase 在 `MS_MARK_SETUP` 切换点滞后旧值，导致 inline fast path 漏记 old edge”
    这个方向，至少不是当前 case27 的主根因

4. **直接去掉 `phase < 20 || phase > 22 -> StoreOnly` gate，也不能修掉 case27，且会更早暴露损坏**

- 在 `llvm/lib/Target/AArch64/AArch64AsmPrinter.cpp` 中，临时注释了
  ref/static-ref inline fastpath 对 non-mark phase 的 `StoreOnly` gate，
  但保留原判断为注释，便于回滚。
- 基于这份 compiler 改动，重新增量编译 `compiler/output` 的 `llc/libLLVM`，
  再重编 runtime、std、stdx 并安装到同一套 SDK 后，用新 SDK 重新编译 case27。
- 结果目录：
  - `MSGC_RESULT/Result/case27_http_complex_json/result/20260702_095800_compiler_output_sdk_nophasegate_c1000_p100K`
- 结果：
  - 短探针 `c1 + p100K + 1s` 在 warmup 就先出现
    `Unexpected character: '\0'`
  - 正式 `c1000 + p100K` 仍然以 `exit.code = 139` 崩溃
  - 栈顶仍然落在 `MCC_AcquireRawData -> WriteBuffer.appendEscape -> JsonObject.toString`
- 含义：
  - “non-mark phase 被错误地 `StoreOnly`，所以去掉 phase gate 就能修好”
    这条解释不成立
  - 相反，这个 gate 更像是当前 inline fastpath 的一个必要保护带；彻底放开后，
    非 mark phase 也往 `_wbuf` 打 old edge，会更早把对象/JSON 状态打坏

### 7.3 当前仍然存在的关键问题

1. **case27 还没有被真正修好**

- 即使在当前代码树下，使用 `compiler/output` SDK 重新编译并运行
  `c1000 + p100K`，仍然会崩
- 因此现在不能把“count offset 修复”“flush heap 过滤”“MarkSetup STW 诊断”
  任何一项当成最终修复

2. **`WriteRefFieldFast_MSGC` / `WriteStaticRefFast_MSGC` 的 callsite inline fast path 仍在**

- `AsmPrinter.cpp` 仍会把这两个 wrapper inline 成 AArch64 fast emission
- 当前 ref/static-ref inline 路径仍然是：
  - 读字段旧值
  - 直接通过 TLS `msAllocBuffer` 写 `wbBuf_`
  - 直接写回字段
- 它仍然绕开了 runtime helper 的统一语义入口：
  - `MCC_WriteRefField`
  - `MCC_WriteStaticRef`
  - `MSMarkBarrier::WriteReference`
  - `MSMarkBarrier::WriteStaticRef`

3. **当前 ref/static-ref inline fast path 自身不做 old-ref heap-check**

- 从 `llvm/lib/Target/AArch64/AArch64AsmPrinter.cpp` 当前实现看：
  - `oldRef == null` 会走 `StoreOnly`
  - `allocBuf == null` 会走 `Slow`
  - `wbBuf full` 会走 `Slow`
  - 但 ref/static-ref 单 slot fastpath 本身不会在 push 前对 `oldRef`
    做 heap-range check
- 这也是为什么 runtime flush 侧现在必须补一层
  `Heap::IsHeapAddress(obj)` 过滤

4. **TLS phase 方向不能彻底排除，但 `MarkSetup` 这个切点已明显降级**

- 当前只能说：
  - “`MS_MARK_SETUP` 这一跳切换窗口”不是最强嫌疑
- 还不能说：
  - 所有 TLS / phase 可见性问题都被完全排除

### 7.4 当前更合理的工作结论

1. **已经确认修掉的是真问题**

- `MSGCWBBufferCountOffset = 8192` 的 ABI 错位，确实已经被修正
- runtime flush 缺少 `Heap::IsHeapAddress()` 过滤，也已经补上

2. **但这些修复还不足以解释或消除 case27**

- 当前 crash 依旧存在
- 说明还有至少一个更核心的问题没有打中

3. **后续排查应该继续聚焦 ref/static-ref inline fast path 本身**

- 重点不是 `struct fastpath`
- 重点也不是 CopyGC tagged-pointer 语义
- 更应继续审计的是：
  - inline fast path 与 runtime helper 之间是否还有语义偏差
  - old-edge 记录时机是否仍存在漏窗
  - TLS phase / TLS allocBuf / flush 之间是否还有未覆盖的一致性问题
