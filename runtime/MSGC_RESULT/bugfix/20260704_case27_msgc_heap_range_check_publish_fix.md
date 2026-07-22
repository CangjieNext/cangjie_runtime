# case27 MSGC heap 范围判断与发布语义问题记录

## 背景

- 平台：`macOS aarch64`
- 相关模块：
  - runtime：`src/Heap/Heap.h`、`src/Heap/MBarrier/MSMarkBarrier.cpp`
  - compiler：`llvm/lib/Target/AArch64/AArch64AsmPrinter.cpp`
- 相关 fastpath：
  - `CJ_MCC_WriteRefFieldFast_MSGC`
  - `CJ_MCC_WriteStructFieldFast_MSGC`
  - `CJ_MCC_WriteStaticRefFast_MSGC`
  - `CJ_MCC_WriteStaticStructFast_MSGC`
  - `CJ_MCC_ArrayCopyRefFast_MSGC`
  - `CJ_MCC_ArrayCopyStructFast_MSGC`

本条记录只讨论 **heap 范围判断** 这条问题线，不等价于 case27 所有问题已经闭环。

## 问题 1：producer 侧对 oldRef 的 heap 判断过早

### 现象

- runtime 的 `EnqueueRefFast()` 以前在 oldRef 入 `wbBuf` 之前，先做：
  - `ref == nullptr`
  - `Heap::IsHeapAddress(ref)`
- AArch64 inline write barrier / arraycopy fastpath 以前在 oldRef 入 `wbBuf` 之前，也会先用：
  - `heapStartAddr`
  - `heapCurrentEnd`
  做 heap-range check。

### 风险

- MSGC collector 真正消费 `wbBuf` 时，本来就还会做更强校验：
  - `DiscoverObject()`
  - `TryEnqueueGreyObject()`
  - `IsTraceableObjectAddress()`
- 因此，producer 侧对 oldRef 的 heap-range check 其实是 **冗余粗判**。
- 一旦 mutator 在线上读到的 `heapCurrentEnd` 偏旧，落在新 committed 区间内的 oldRef 可能被提前误判成 off-heap：
  - runtime：直接不入 `wbBuf`
  - compiler inline：走 `StoreOnly` / `skip`
- 这会把本该记录的 old edge 在 producer 侧直接丢掉。

## 问题 2：runtime 与 compiler 对 `heapCurrentEnd` 的发布/读取契约不一致

### runtime 当前语义

- `Heap::PublishHeapCurrentEnd(newEnd)`：
  - `store(..., std::memory_order_release)`
- `Heap::GetHeapCurrentEnd()`：
  - `load(..., std::memory_order_acquire)`
- 也就是 runtime 自己把 `heapCurrentEnd` 当成一个 **release/publish, acquire/read** 的并发边界。

### compiler 原先语义

- AArch64 inline fastpath 读取：
  - `heapStartAddr`：普通全局读
  - `heapCurrentEnd`：也用普通 `LDR`
- 这意味着 compiler fastpath 对 `heapCurrentEnd` 并没有遵守 runtime 的 acquire 读取契约。

### 风险

- 并发扩堆时，runtime grow 路径先发布新的 committed end，再把新 segment 暴露给 mutator。
- 如果 inline fastpath 仍用普通 `LDR` 读 `heapCurrentEnd`，它有机会看到偏旧的 end。
- 对仍然保留的 `obj/dst` heap guard 来说，这会导致：
  - 明明已经进入当前 committed heap 的对象
  - 在 inline 中被误判成“不在 heap 内”
  - 从而错误走 `StoreOnly` / `Slow` 分支

## 修复思路

### 1. oldRef 的 heap 校验下沉到 flush/drain 契约

- runtime：
  - `EnqueueRefFast()` 去掉 `Heap::IsHeapAddress(ref)` 预判
  - 只保留 `ref == nullptr` 快速返回
- compiler：
  - 对 6 个进入 `emitMccWriteBarrierFastPath()` 的 wrapper
  - 移除 “oldRef 入 `wbBuf` 前” 的 heap-range check

### 2. 保留 `obj/dst` 元数据访问保护

- `WriteStructFieldFast_MSGC` / `ArrayCopyStructFast_MSGC` 等路径
- 后面还要直接解 `TypeInfo/GCTib`
- 这些 `obj/dst` 的 heap-range check **不能删**
- 本轮只移除了 oldRef 的粗判，不移除为对象元数据访问服务的 guard

### 3. `heapCurrentEnd` 改成 acquire 读取

- AArch64 inline 新增专用读取助手：
  - `EmitLoadGlobalU64Acquire()`
- 仅对仍保留的 3 个 heap-range guard，把：
  - `heapCurrentEnd` 从普通 `LDR`
  - 改成 acquire `LDAR`
- `heapStartAddr` 仍保持普通读取，因为它初始化后不再变化。

## 实际改动

### runtime

- 文件：`src/Heap/MBarrier/MSMarkBarrier.cpp`
- 变更：
  - `EnqueueRefFast()` 从
    - `if (ref == nullptr || !Heap::IsHeapAddress(ref)) return;`
  - 改成
    - `if (ref == nullptr) return;`

### compiler

- 文件：`llvm/lib/Target/AArch64/AArch64AsmPrinter.cpp`
- 变更 1：
  - 移除 6 个 wrapper 里 oldRef 入 `wbBuf` 前的 `EmitHeapRangeCheckTo(...)`
- 变更 2：
  - 新增 `EmitLoadGlobalU64Acquire()`
  - 在剩余 3 个 `obj/dst` heap-range guard 处，对 `HeapCurrentEndSymName` 改用 `LDAR`

## 结论

1. oldRef 的 heap 判断不应在 producer 侧做最终裁决；producer 侧粗判既冗余，又会把并发扩堆窗口里的 stale `heapCurrentEnd` 问题放大成漏记 old edge。
2. runtime 已把 `heapCurrentEnd` 设计成 release/acquire 发布语义，compiler inline 对它也应做 acquire 读取，不能继续当普通常量边界读。
3. 这条修复只解决 **heap-range check / heapCurrentEnd publish** 这一类问题，不代表 case27 后续所有 crash 都已被解释。

## 补充说明

- 当前 runtime 内部更严格的 object 合法性判断，最终仍以：
  - `DiscoverObject()`
  - `TryEnqueueGreyObject()`
  - `IsTraceableObjectAddress()`
  为准。
- 因此这次改动的目标不是“把所有 heap 判定都删掉”，而是：
  - 删掉对 oldRef 的早期粗判
  - 保留真正用于对象元数据访问安全的 guard
  - 并把 `heapCurrentEnd` 的读取语义与 runtime 发布契约对齐
