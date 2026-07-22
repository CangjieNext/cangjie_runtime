# Step25：Go 风格写屏障对齐设计

> 2026-06-18 初稿，2026-06-19 按当前代码状态修订
> 目标：对照 Go 风格“薄写屏障 + flush 做 collector 语义”的方向，盘点当前 MSGC 已经落地的部分，以及还没有落地的部分。
> 口径：**以当前代码为准**。这篇文档不再把“已经做完的 runtime 改造”继续写成待办。

---

## 目录

1. [结论先行](#1-结论先行)
2. [本文采用的 Go 风格口径](#2-本文采用的-go-风格口径)
3. [当前 MSGC 写屏障的实际实现](#3-当前-msgc-写屏障的实际实现)
4. [已经实现的部分](#4-已经实现的部分)
5. [还需要实现的部分](#5-还需要实现的部分)
6. [不建议做的点](#6-不建议做的点)
7. [推荐落地顺序](#7-推荐落地顺序)
8. [验证口径](#8-验证口径)

---

## 1. 结论先行

这轮对照当前代码后，可以先下 4 个结论：

1. **runtime 侧“写时只记日志、flush 时再做 discover/mark/queue transfer”这一步已经做完了。**
2. **wbBuf 已经不在 `Mutator` 上，而是在 `AllocBufferMS` 上，且与 `PerPGCWork` 放在同一个 owner 上。**
3. **当前 MSGC 的实际语义不是 old/new 双记录，而是 old-only deletion-style barrier。**
4. **真正还没做的是 compiler/llvm 侧的 MSGC 专用 fast path、最小 ABI 固化，以及是否要继续向 Go 的语义组合进一步靠拢。**

可以把当前状态概括成下面这张表：

| 项目 | 当前状态 | 结论 |
| --- | --- | --- |
| 写时是否立即 `DiscoverObject` | 否 | 已改成 logging barrier |
| `DiscoverObject / MarkObject` 在哪做 | overflow / owner-exit / STW flush | 已迁到 flush 慢路径 |
| wbBuf 挂载点 | `AllocBufferMS::wbBuf_` | 已从 Mutator 迁出 |
| barrier 记录内容 | old-only | 与文档旧版本不同，需要单独说明 |
| struct/array/atomic runtime barrier | 已有 old-only logging 版本 | runtime 侧已覆盖 |
| compiler MSGC 专用 fast wrapper | 还没有 | 仍待实现 |
| 最小 ABI 固化 | 还没有 | 仍待实现 |
| 完整 per-P/Processor 化 | 还没有完全做完 | 仍待实现 |

一句话总结：

> Step25 最关键的 runtime 重构其实已经落地了；现在真正的主战场已经从“把 mark 挪出写时路径”切换成“把 compiler fast path 和 runtime 的最小接口做薄并固化”。

---

## 2. 本文采用的 Go 风格口径

这里先统一一个口径，避免被“Go 到底 shade old、shade new，还是 hybrid”这个细节带偏。

本文说的“与 Go 风格对齐”，核心不是逐条复刻 Go 某个具体版本的 barrier 细节，而是对齐下面这条主线：

1. **编译器尽量把不需要 barrier 的情况在前面裁掉。**
2. **mark 期快路径尽量薄，只做日志记录，不直接跑完整 collector 逻辑。**
3. **真正的 collector 语义放到 flush 慢路径统一做。**
4. **compiler 只知道最小 ABI，不直接依赖 runtime 的内部 grey/work queue 结构。**

所以这里更关心的是：

```text
写时：append/log
flush：discover / mark / queue transfer
```

而不是在这篇文档里强行把 Go 的 old/new 组合和当前 MSGC 一一对应。

当前 MSGC 是否要继续从 **old-only** 往 **old/new** 或 **hybrid** 靠，需要结合 collector invariant 另行决定；这件事属于“还需要实现的部分”，不是当前代码已经完成的部分。

---

## 3. 当前 MSGC 写屏障的实际实现

这一节只写“当前代码现在是什么样”，不写历史状态。

### 3.1 compiler 侧仍然是 `llvm.cj.gcwrite_*` intrinsic 路线

从当前 compiler 代码看：

- `cj_gcwrite_ref`
- `cj_gcwrite_static_ref`
- `cj_gcwrite_struct`
- `cj_gcwrite_static_struct`

这些 intrinsic 仍然存在，入口在：

- `../../cangjie_compiler/src/CodeGen/CJNative/CJNativeIRBuilder.cpp`

也就是说，**compiler 侧还没有出现专门为 MSGC 定制的极薄 wrapper ABI**，例如文档旧版本里提到的：

- `CJ_MCC_WriteRefFieldFast_MSGC`
- `CJ_MCC_WriteStaticRefFast_MSGC`
- `CJ_MCC_WBFlushSlow_MSGC`

这部分还属于待实现。

### 3.2 runtime 写时路径已经是 log-only，不再立即 mark

当前 `MSMarkBarrier::EnqueueRefFast()` 在：

- `src/Heap/MBarrier/MSMarkBarrier.cpp`

它现在做的是：

1. 过滤 `nullptr` / 非堆对象
2. 取当前 `AllocBufferMS`
3. 拿到 `AllocBufferMS::wbBuf_`
4. buffer 满时调用 `FlushWriteBarrierOverflow(wbBuf)`
5. 把 ref 直接 `wbBuf.Push(ref)`

这里**已经没有**写时的：

- `TryEnqueueGreyObject`
- `DiscoverObject`
- `MarkObject`

所以这一步已经不是“写时立即 discover”的老实现，而是一个真正的 logging barrier。

### 3.3 当前语义是 old-only，不是 old/new

当前 `MSMarkBarrier.cpp` 的各类写路径都是 old-only：

- `WriteReference`
- `WriteStaticRef`
- `WriteStruct`
- `WriteStaticStruct`
- `CopyRefArray`
- `CopyStructArray`
- `AtomicWriteReference`
- `AtomicSwapReference`
- `CompareAndSwapReference`

它们做的事情统一可以概括成：

```text
log(old 或 dst 中将被覆盖的旧引用)
store / memcpy / memmove / atomic store
```

当前代码**不会**在写时把 new 再追加到 wbBuf。

这点和文档旧版本不同，也和 `MSMarkBarrier.h` 里的旧注释不同。头文件里仍然写着 old/new 双记录，但实际实现已经是 old-only。

### 3.4 wbBuf 已经挂在 `AllocBufferMS` 上

当前 owner 不是 `Mutator`，而是：

- `AllocBufferMS::wbBuf_`
- `AllocBufferMS::gcWork_`

入口在：

- `src/Heap/MAllocator/MSAllocBuffer.h`
- `src/Heap/MAllocator/MSAllocBuffer.cpp`

当前结构更准确地说是：

```text
ThreadLocal -> AllocBufferMS
                     |- wbBuf_
                     |- gcWork_
```

这说明：

1. wbBuf 已经和分配/局部 GC work 放在同一个 owner 上
2. 方向上已经比“挂在 Mutator 私有状态里”更接近 Go 的 per-P 设计
3. 但它还不等于 Go 那种严格意义上的 P 级别统一宿主

### 3.5 flush 慢路径已经是 collector 语义中心

当前真正做 `DiscoverObject + queue transfer` 的是 3 类 flush：

1. `FlushWriteBarrierOverflow`
2. `FlushExitedWriteBarrierBuffer`
3. `FlushAllWriteBarrierBuffers`

其中：

- overflow 路径会把新灰对象装进 `WorkBuf`，优先进本地 `PerPGCWork`，退化时进 `globalFull_`
- owner-exit 路径会直接刷进 `globalFull_`
- STW 终止阶段会遍历所有 `AllocBufferMS`，把剩余 wbBuf 条目 `TryEnqueueGreyObject(..., mainQueue)`

也就是说：

> 当前 runtime 已经把“collector 语义中心”从写时路径迁到了 flush 路径。

### 3.6 bulk barrier 的 runtime 版本已经跟上了 logging 语义

旧文档里曾把 bulk barrier 归为“先不做”。

但按当前代码看，runtime 侧其实已经有了这些版本：

- `WriteStruct`
- `WriteStaticStruct`
- `CopyRefArray`
- `CopyStructArray`

它们做法不是写时 mark，而是：

1. 遍历 dst 范围里的旧 ref
2. 对旧 ref 做 `EnqueueRefFast`
3. 再执行 `memcpy/memmove`

所以正确表述应当是：

- **runtime 侧 bulk barrier 已经进入 logging barrier 语义**
- **未完成的是 compiler 侧是否要为这些 bulk 场景做专门 fast path**

---

## 4. 已经实现的部分

这一节只列已经落到代码里的改造。

### 4.1 runtime 已完成“写时只 log，flush 时再 discover”

这是 Step25 最关键的一步，而且现在已经不是待办。

已完成内容：

1. `EnqueueRefFast()` 不再直接 `DiscoverObject`
2. wbBuf 满时由 collector flush 路径统一处理
3. `DiscoverObject / TryEnqueueGreyObject / WorkBuf/globalFull_ transfer` 已经收敛到 flush 路径

结论：

> “把写时 mark 挪到 flush 时”这一步，已经实现。

### 4.2 wbBuf 已从 Mutator 迁到 `AllocBufferMS`

已完成内容：

1. 当前写屏障通过 `AllocBufferMS::GetOrCreateAllocBufferMS()` 取 owner
2. `WriteBarrierBuffer` 实体在 `AllocBufferMS::wbBuf_`
3. 线程退出时在 `AllocBufferMS::Fini()` 中做 `FlushExitedWriteBarrierBuffer`

结论：

> “先把 wbBuf 从 Mutator 收到更像 Go 的 per-P 落点”这一步，已经部分完成，而且代码已经切过去了。

### 4.3 wbBuf 与本地 `gcWork_` 已经同宿主

这一点很重要，因为它意味着 barrier logging 和 assist/local mark work 已经不再完全割裂。

当前 `AllocBufferMS` 同时持有：

1. `wbBuf_`
2. `gcWork_`

overflow flush 也优先把新灰对象推给当前 owner 的本地 `PerPGCWork`。

结论：

> 这一步虽然还不是完整 Go per-P，但方向已经是“本地 buffer + 本地 GC work 合流”。

### 4.4 runtime 侧 old-only barrier 已经稳定存在

当前实际实现已经明确是 old-only：

1. 单 slot 写
2. static 写
3. struct 覆盖
4. array copy
5. atomic write/swap/CAS

这说明当前项目已经做了一个明确的语义选择：

> **先跑 old-only deletion-style barrier**，而不是 old/new 双记录。

这件事不是“没实现”，而是“已经实现成了现在这个版本”。

### 4.5 bulk barrier 的 runtime logging 版本已经落地

旧文档把它写成“暂不扩展 bulk barrier”已经不准确了。

更准确的状态是：

1. runtime 侧 bulk barrier 已实现
2. 它们已经遵循当前 old-only logging 语义
3. 还没做的是 compiler 侧是否要把这些 bulk barrier 做薄/做快

---

## 5. 还需要实现的部分

现在真正的待办，已经从 runtime 语义迁移到了 compiler ABI、语义目标和更强的一致性上。

### 5.1 compiler 侧 MSGC 专用 fast wrapper 还没有

当前 compiler 仍然走 `llvm.cj.gcwrite_*` intrinsic 路线。

还需要做的点：

1. 为 `gcwrite_ref / gcwrite_static_ref` 增加 MSGC 专用极薄 fast wrapper
2. mark 期 fast path 只做：
   - load old
   - store new
   - append old 到 wbBuf
   - full 时跳 slow helper
3. non-mark phase 尽量直接退化为普通 store

这一步是后续真正能继续降 barrier 热路径成本的关键点。

### 5.2 最小 ABI 还没有固化

如果要让 compiler 真正吃到 Go 风格 fast path 的收益，还要把双方接口收缩到最小 ABI。

当前还没有系统化固化的内容包括：

1. 如何从 TLS 取到 `AllocBufferMS`
2. `WriteBarrierBuffer` 的稳定布局约束
3. full 分支 slow helper 的稳定符号
4. 双边 `static_assert` / layout guard

建议未来只暴露这些最小信息，不要暴露：

- `MSRegionInfoArray`
- `WorkBufPool`
- `globalFull_`
- `PerPGCWork` 内部协议

### 5.3 是否继续向 Go 的 old/new 或 hybrid 语义靠拢，还没有定

当前代码已经是 old-only。

如果后续要“更像 Go”，还需要单独决策：

1. 继续维持 old-only deletion barrier
2. 改成 old/new 双记录
3. 改成更接近 Go 的 hybrid 语义

这不是简单的代码搬运问题，而是 collector invariant 选择问题。

它需要一起评估：

1. `VerifyMarkCompleteness`
2. case27 稳定性
3. mark backlog / assist 压力
4. 写屏障热路径成本

所以这一步仍然是待实现的设计选择，不是已完成项。

### 5.4 更严格的 per-P / Processor 化还没有完全做完

虽然 wbBuf 已经进了 `AllocBufferMS`，但如果要继续向 Go 对齐，后面还可以做得更彻底：

1. 明确 `AllocBufferMS` 与 runtime Processor/P 的绑定关系
2. 明确 owner dispose / flush / convergence 协议
3. 让 barrier / assist / bg mark 在同一层 owner 上有更一致的生命周期

当前状态是“方向正确但还没完全走完”。

### 5.5 flush 核心还可以继续收口

目前 flush 分成：

1. overflow flush
2. owner-exit flush
3. STW flush

三者都已经在做 collector 语义，但实现上还不是一个完全统一的公共核心。

后续仍可以做：

1. 抽公共的 wb entry drain / discover / queue transfer 核心
2. 只让不同场景决定“目标去向”
3. 保证 overflow / exit / STW 三条路径语义完全一致

这不是 correctness blocker，但属于还可以继续完成的整理项。

### 5.6 代码注释还有一批陈旧信息没清掉

这次对照代码时，至少看到两类明显陈旧注释：

1. `MSMarkBarrier.h` 仍然写着 old/new 双记录
2. `WriteBarrierBuffer.h` 的注释仍然保留旧容量口径

这些不会改变运行行为，但会直接误导后续阅读者。

这部分建议尽快清理。

---

## 6. 不建议做的点

### 6.1 不建议把完整 collector ABI 直接暴露给 compiler

不建议让 compiler 直接知道：

- `DiscoverObject`
- `MSRegionInfoArray`
- `WorkBufPool`
- `globalFull_`

理由不变：

1. compiler 和 runtime 耦合面会迅速膨胀
2. runtime 内部结构一改，compiler 也要跟着改
3. 这和“薄 barrier + 厚 flush”的目标相反

### 6.2 不建议把“runtime 已完成的事情”继续当下一阶段主任务

例如下面两件事，现在都不应该再被写成主待办：

1. “把写时 discover 挪到 flush”
2. “把 wbBuf 从 Mutator 挪到 AllocBufferMS”

因为它们已经完成了。

下一阶段如果还把精力放在这里，会偏离真正的瓶颈。

### 6.3 不建议在 compiler 侧第一步就去做 bulk barrier 完整内联

当前 runtime 侧 bulk barrier 已经有 logging 版本。

所以 compiler 侧第一阶段建议仍然聚焦：

1. `gcwrite_ref`
2. `gcwrite_static_ref`

等单 slot fast path 成熟后，再评估 bulk barrier 是否值得进一步下沉。

---

## 7. 推荐落地顺序

按当前状态，推荐顺序应该更新为：

### Step A：先把文档和代码注释口径统一

这一步很小，但很必要：

1. 明确当前已经是 old-only logging barrier
2. 明确 wbBuf 当前在 `AllocBufferMS`
3. 删除文档里“runtime 还没拆语义”的旧表述

### Step B：做 compiler 侧 `gcwrite_ref/static_ref` 的 MSGC fast path

目标是：

1. non-mark 期直接走普通 store
2. mark 期只做极薄 append
3. full 时跳 slow helper

### Step C：固化最小 ABI

需要一起完成：

1. TLS -> `AllocBufferMS` 取法
2. `WriteBarrierBuffer` 布局约束
3. slow helper 符号和调用约定
4. 必要的双边 layout assert

### Step D：再决定最终语义目标

在 compiler fast path 跑起来后，再决定：

1. 保持 old-only
2. 引入 old/new
3. 引入 hybrid

这一步要以 correctness 和 benchmark 数据为准，不要先拍脑袋。

### Step E：最后再评估更彻底的 per-P 化

等前面几步稳定之后，再看是否需要继续把 owner 模型向 Go 收得更紧。

---

## 8. 验证口径

### 8.1 正确性

1. `VerifyMarkCompleteness` 不报错
2. 无 `SIGSEGV`
3. 无 json fail / failed request
4. 无 invalid object / stale pointer 问题

### 8.2 屏障行为

当前已满足的检查项：

1. 写时路径不再直接 `DiscoverObject`
2. wbBuf 中保存的是原始 ref 日志
3. flush 时才做 `DiscoverObject / queue transfer`

后续要继续验证的检查项：

1. compiler fast path 是否真的只做 append
2. full 分支是否只跳薄 helper
3. old-only / old+new / hybrid 语义切换后 verify 是否仍稳定

### 8.3 性能

重点看：

1. `case27_http_complex_json`
2. `c8 / p10K`
3. `c8 / p100K`

建议对比：

1. 吞吐量
2. CPU 占用
3. barrier 热点
4. mark 阶段总时长

### 8.4 当前阶段的预期收益

由于 runtime 侧 logging barrier 已经做完，后续收益重点不再来自“把 mark 挪到 flush”，而主要来自：

1. compiler fast path 变薄
2. runtime/llvm ABI 变稳
3. barrier 调用/分支成本下降

---

## 结语

按当前代码状态，Step25 应当这样理解：

1. **runtime 侧 logging barrier 改造已经完成**
2. **当前实际语义是 old-only deletion barrier**
3. **后续主任务是 compiler fast path、最小 ABI 和最终语义目标选择**

所以接下来不该再重复做“把写时 mark 挪出 barrier”这件事，而应该把精力放到：

> **让 compiler 只知道极薄的 wb append ABI，让 runtime 继续托底 flush/discover/queue transfer。**

