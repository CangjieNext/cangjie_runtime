# src 当前工作区改动总结

## 范围
当前 `src` 目录共有 9 个文件有工作区改动：
- `src/Common/BaseObject.h`
- `src/Common/LFStack.h`
- `src/Common/StateWord.h`
- `src/CompilerCalls.cpp`
- `src/Heap/MAllocator/MSRegionInfo.cpp`
- `src/Heap/MAllocator/MSRegionInfo.h`
- `src/Heap/MBarrier/MSMarkBarrier.cpp`
- `src/Heap/MBarrier/WriteBarrierBuffer.h`
- `src/Heap/MCollector/MarkSweepCollector.cpp`

## 一、问题修复类改动

### 1. 修复运行时 TypeInfo 指针污染导致的接口分派/类型判断错误
涉及文件：`src/CompilerCalls.cpp`

改动内容：
- 在 `CJ_MCC_IsSubType` 中，对编译器直接从对象头读出来的 64 位 `stateWord` 参数做高 16 bit mask。
- 新增 `MaskRuntimeTypeInfo()`，并在 `CJ_MCC_GetMTable`、`CJ_MCC_GetMethodOuterTI` 中统一使用。

修复的问题：
- 修复 `funcTable is nullptr, ti: RawArray<UInt8>, itf: std.core:Resource` 这类错误。
- 根因是编译器侧把完整 `stateWord` 当作 `TypeInfo*` 传入，而 MSGC 下高 16 bit 可能带有 objectState，未 mask 时会形成非法指针。

### 2. 修复对象头 stateWord 发布/读取竞态
涉及文件：`src/Common/StateWord.h`、`src/Common/BaseObject.h`

改动内容：
- `StateWord::GetTypeInfo()` 从拆开的非原子读改为单次 64 位 atomic acquire load。
- 新增 `StateWord::InitStateWord()`，使用单次 64 位 release store 原子发布干净的 stateWord。
- `BaseObject::SetClassInfo()` 和静态版本都改成走 `InitStateWord()`。

修复的问题：
- 修复对象复用场景下读取到撕裂 header、脏 objectState 或垃圾 `TypeInfo*` 的问题。
- 这类问题会引出 `ExecuteLikeDrainMainQueue` 中的非法对象、随机 SIGSEGV，或对象刚分配出来就带着旧 header 的残留位。

### 3. 修复 small region sweep 与重新分配之间的发布顺序竞态
涉及文件：`src/Heap/MAllocator/MSRegionInfo.cpp`

改动内容：
- `SweepRegion()` 改成三段式：先统计 live，再先 zero dead slot，最后才发布 `allocBitmap`。
- `allocBitmap[i]` 和 `reserved` 的发布改成 release store。

修复的问题：
- 修复“free slot 先发布、slot 内容后清零”的竞态。
- 旧逻辑下，并发分配线程可能先抢到 slot，再读到上一个死对象残留的 header / 引用字段，导致新对象继承脏 stateWord 或旧引用。

### 4. 修复 bitmap 快路径分配的数据竞争
涉及文件：`src/Heap/MAllocator/MSRegionInfo.h`

改动内容：
- `AllocFromBitmapFast()` 从普通读写改成 acquire load + CAS 抢占 slot。
- 当某个 bitmap word 为空时，用原子方式修正 `reserved` summary。

修复的问题：
- 修复多个 mutator 并发从同一个 region bitmap 领 slot 时的竞争。
- 避免 double-claim、summary 过期、空 word 仍被反复选择等问题。

### 5. 修复/缓解 stale 对象进入 SATB / mark 队列导致的 fatal
涉及文件：`src/Heap/MBarrier/WriteBarrierBuffer.h`、`src/Heap/MCollector/MarkSweepCollector.cpp`

改动内容：
- `WriteBarrierBuffer::Flush()` 在回放时丢弃 `nullptr` 和 `!IsValidObject()` 的条目。
- `MarkSweepCollector::ExecuteLikeDrainMainQueue()` 对 `!IsTraceableObjectAddress(obj)` 或 `!obj->IsValidObject()` 的对象改为跳过，并记录报告日志，不再直接 fatal。

修复的问题：
- 修复并发 sweep / SATB buffer 中残留 stale entry 时，mark 队列消费到已清零或已回收对象导致的 `invalid stateWord` fatal。
- 这属于防御性修复，目的是把已死对象从标记队列里安全过滤掉。

## 二、优化类改动

### 1. 提高 LFStack work buffer 容量
涉及文件：`src/Common/LFStack.h`

改动内容：
- `WorkBuf::kCapacity` 从 256 提升到 1024。

优化效果：
- 降低灰队列 workbuf 频繁切换和链表操作开销。
- 对高对象量标记阶段更友好。

### 2. 提高 WriteBarrierBuffer 容量
涉及文件：`src/Heap/MBarrier/WriteBarrierBuffer.h`

改动内容：
- `WriteBarrierBuffer::kCapacity` 从 256 提升到 1024。

优化效果：
- 降低 SATB buffer flush 频率。
- 对高写入压力场景能减少 barrier buffer 管理开销。

### 3. 给写屏障增加已经标记对象的快速跳过
涉及文件：`src/Heap/MAllocator/MSRegionInfo.h`、`src/Heap/MBarrier/MSMarkBarrier.cpp`

改动内容：
- 新增 `MSRegionInfoArray::IsAlreadyMarked()` 作为只读快速判断。
- `MSMarkBarrier::EnqueueRef()` 先用这个快速路径跳过已标黑对象。

优化效果：
- 对 born-black 新对象或已标记对象，避免不必要地读 TLS mutator、进入 wb buffer 或灰队列。

### 4. 写屏障 null fast path
涉及文件：`src/Heap/MBarrier/MSMarkBarrier.cpp`

改动内容：
- `WriteReference()` 和 `WriteStaticRef()` 对 `oldRef` / `ref` 增加 inline null check。

优化效果：
- 降低对象初始化阶段大量 null 写入的函数调用和 barrier 开销。

### 5. 调整 assist 参数，降低过强的 mutator assist 干扰
涉及文件：`src/Heap/MCollector/MarkSweepCollector.cpp`

改动内容：
- `GetAssistRatioCoeff()` 默认值从 0.1 调整到 0.05。
- `GetAssistDebtTriggerObjects()` 默认值从 256 调整到 512。
- `GetAssistRatioMaxMultiplier()` 默认值从 10.0 调整到 3.0。

优化效果：
- 减轻 mutator assist 对吞吐的侵入。
- 避免 assist 过猛把业务线程拖慢，尤其是在高并发 benchmark 中更稳。

## 三、兼具修复与优化性质的改动

### 1. `IsAlreadyMarked()` 的引入
涉及文件：`src/Heap/MAllocator/MSRegionInfo.h`

说明：
- 这本质上是优化，但也减少了新对象 born-black 情况下重复入队带来的队列噪音和 stale 数据传播机会。

### 2. `ExecuteLikeDrainMainQueue()` 从 fatal 改为 skip-invalid
涉及文件：`src/Heap/MCollector/MarkSweepCollector.cpp`

说明：
- 这不是根因修复本身，而是让 GC 对无效对象更稳健。
- 它能帮助继续跑通 benchmark，并把问题从“直接崩溃”降级为“带日志跳过”，便于继续定位真正来源。

## 四、这批改动总体解决了什么

这批 `src` 改动主要围绕 MSGC 的三类核心问题：

1. **对象头 / TypeInfo 读取错误**
- 通过 `CompilerCalls.cpp` 的 mask 和 `StateWord.h` 的原子发布/读取，修复了对象头高位污染、撕裂读取、旧 header 残留导致的类型判断与接口分派错误。

2. **small object 重新分配竞态**
- 通过 `MSRegionInfo.cpp/.h` 的 zero-before-publish、release publish、CAS bitmap claim，修复了 sweep 与并发分配之间的 slot 发布竞态。

3. **并发标记 / SATB 队列中的 stale 对象问题**
- 通过 `WriteBarrierBuffer.h`、`MSMarkBarrier.cpp` 和 `MarkSweepCollector.cpp` 的修正，把 stale entry 对运行稳定性的影响显著降低，同时减少 barrier 热路径开销。

## 五、当前工作区里这些改动的定位结论

如果按性质拆分：

### 以“修 bug”为主的文件
- `src/Common/BaseObject.h`
- `src/Common/StateWord.h`
- `src/CompilerCalls.cpp`
- `src/Heap/MAllocator/MSRegionInfo.cpp`
- `src/Heap/MAllocator/MSRegionInfo.h`
- `src/Heap/MBarrier/WriteBarrierBuffer.h`
- `src/Heap/MCollector/MarkSweepCollector.cpp`

### 以“性能优化”为主的文件
- `src/Common/LFStack.h`
- `src/Heap/MBarrier/MSMarkBarrier.cpp`
- `src/Heap/MCollector/MarkSweepCollector.cpp`

### 同时承担“稳态修复 + 性能优化”的文件
- `src/Heap/MAllocator/MSRegionInfo.h`
- `src/Heap/MBarrier/MSMarkBarrier.cpp`
- `src/Heap/MCollector/MarkSweepCollector.cpp`

## 六、已生成的配套文件
- 本说明文件：`MSGC_RESULT/bugfix/src_current_bugfix_summary_20260525.md`
- 当前 `src` 完整 patch：`MSGC_RESULT/bugfix/src_current_bugfix_20260525.patch`
