# MSGC 实现理解

## 1. 总体判断

当前仓颉 Runtime 中的 MSGC，已经不是一套停留在设计稿里的骨架，而是一套可以完整跑通的编译期可切换 GC 实现。

它的整体结构可以概括为：

- **框架层**：通过编译宏在 CopyGC 和 MarkSweep 之间切换
- **分配层**：小中对象走 `AllocBufferMS + CentralRegionPool + MSRegionManager`
- **标记层**：在 `TracingCollector` 框架上实现并发三色标记
- **屏障层**：使用 MS 专属 barrier，但实现策略比设计稿更保守
- **清扫层**：通过 lazy/background/force 三路 sweep 协作回收空间
- **堆管理层**：`MSRegionInfo` 已经内嵌到 heap mmap 前段，且支持 committed heap 增长

如果用一句话总结：

> 当前 MSGC 的本质是“保留 TracingCollector 基础设施，替换 CopyGC 的 forward/copy 路径为 mark + sweep 路径”。

---

## 2. 编译期开关与总体入口

MSGC 不是运行时动态切换，而是**编译期选择**。

顶层 CMake 在下列位置定义宏：

- [CMakeLists.txt](CMakeLists.txt#L211)

关键逻辑：

- `COPYGC_FLAG=1` 时定义 `MRT_USE_COPYGC`
- `MARKSWEEP_FLAG=1` 时定义 `MRT_USE_MARK_SWEEP`

因此，MSGC 和 CopyGC 的差异主要通过 `#ifdef MRT_USE_MARK_SWEEP` 进入不同实现分支，而不是在运行时通过一个大 if-else 选择。

MSGC 的核心 collector 是：

- [src/Heap/Collector/MarkSweepCollector.h](src/Heap/Collector/MarkSweepCollector.h#L68)
- [src/Heap/Collector/MarkSweepCollector.cpp](src/Heap/Collector/MarkSweepCollector.cpp#L67)

主流程入口：

- `MarkSweepCollector::RunGarbageCollection`
- `MarkSweepCollector::DoGarbageCollection`

GC 主阶段顺序已经固定为：

1. `MarkSetup`
2. `ConcurrentMark`
3. `MarkTermination`
4. `PostMarkTermination`
5. `ConcurrentSweep`

对应代码位置：

- [src/Heap/Collector/MarkSweepCollector.cpp](src/Heap/Collector/MarkSweepCollector.cpp#L105)

---

## 3. 分配器主线

### 3.1 RegionSpace 是 MS 分配入口

MS 模式下，分配入口已经和 CopyGC 分叉。

关键函数：

- [src/Heap/Allocator/RegionSpace.cpp](src/Heap/Allocator/RegionSpace.cpp#L36)
- [src/Heap/Allocator/RegionSpace.cpp](src/Heap/Allocator/RegionSpace.cpp#L94)

`RegionSpace::TryAllocateOnce` 在 MS 模式下会直接走 `TryAllocateOnceMS`。

### 3.2 小中对象分配路径

小中对象路径是：

`RegionSpace::TryAllocateOnceMS`
→ `GetOrCreateAllocBufferMS`
→ `AllocBufferMS::Allocate`
→ 当前 active region 快速分配
→ 若失败则向 `CentralRegionPool` 补 region

关键代码：

- [src/Heap/Allocator/AllocBufferMS.cpp](src/Heap/Allocator/AllocBufferMS.cpp#L49)
- [src/Heap/Allocator/AllocBufferMS.cpp](src/Heap/Allocator/AllocBufferMS.cpp#L75)
- [src/Heap/Allocator/AllocBufferMS.cpp](src/Heap/Allocator/AllocBufferMS.cpp#L105)

这里有两个关键点：

1. **线程本地缓存是真实存在的**
   每个线程都有一个 `AllocBufferMS`，内部按 size class 维护 active region 和 cached region。

2. **分配并不总是 bitmap 分配**
   如果 region 处于 `MS_REGION_BUMP`，会优先走 bump；否则走 bitmap 分配。

这说明当前实现不是“纯 free-list allocator”，而是 bump 和 bitmap 混合。

### 3.3 CentralRegionPool 是小对象 region 调度中心

文档中 Step1 和 Step4 提到的四链表设计已经落地。

定义位置：

- [src/Heap/Allocator/CentralRegionPool.h](src/Heap/Allocator/CentralRegionPool.h#L200)

每个 size class 维护：

- `partialSwept`
- `fullSwept`
- `partialUnswept`
- `fullUnswept`

取 region 的主逻辑在：

- [src/Heap/Allocator/CentralRegionPool.cpp](src/Heap/Allocator/CentralRegionPool.cpp#L21)

优先级是：

1. `partialSwept`
2. `partialUnswept`
3. `fullUnswept`
4. `GetFreshRegion`

其中第 2 和第 3 步会直接触发 lazy sweep。

---

## 4. 大对象路径

### 4.1 当前主路径是堆内 large region

MS 模式下，大对象入口在：

- [src/Heap/Allocator/RegionSpace.cpp](src/Heap/Allocator/RegionSpace.cpp#L43)

当前逻辑是：

- 只要对象大小达到 `kLargeObjectThreshold`
- 就进入 `MSRegionManager::TakeRegionForLarge`

对应代码：

- [src/Heap/Allocator/MSRegionManager.cpp](src/Heap/Allocator/MSRegionManager.cpp#L370)

随后 `RegionSpace::RegisterLargeObjectMS` 会把这个 large object 对应的所有 span 都登记成 `MS_REGION_LARGE`：

- [src/Heap/Allocator/RegionSpace.cpp](src/Heap/Allocator/RegionSpace.cpp#L345)

### 4.2 LOS 仍然存在，但更像补充路径

工程里仍保留独立的 `LargeObjectSpace`：

- [src/Heap/Allocator/LargeObjectSpace.cpp](src/Heap/Allocator/LargeObjectSpace.cpp#L123)

collector 在标记和清扫阶段也会访问它：

- [src/Heap/Collector/MarkSweepCollector.cpp](src/Heap/Collector/MarkSweepCollector.cpp#L40)
- [src/Heap/Collector/MarkSweepCollector.cpp](src/Heap/Collector/MarkSweepCollector.cpp#L836)

因此当前更准确的说法是：

> MSGC 里存在“堆内 large region”和“独立 LOS”两套大对象承载机制，collector 同时兼容这两类对象。

这和一些设计文档里写的单一路由并不完全一致。

---

## 5. 标记阶段主线

### 5.1 MarkSetup

`MarkSetup` 在 MSGC 里承担四个关键职责：

1. 兜底完成上一轮未完成的 sweep
2. 在短 STW 中 flush 所有线程本地 alloc buffer
3. 清空所有 mark bitmap / large mark
4. 切换到 MS 标记 phase，随后做根枚举

关键函数：

- [src/Heap/Collector/MarkSweepCollector.cpp](src/Heap/Collector/MarkSweepCollector.cpp#L125)

这里有一个非常关键的实现点：

MS 并没有依赖纯 handshake 去刷新所有 TLS 分配状态，而是选择了一个**短 STW** 来统一 flush TL regions。这一点和文档里“尽量 handshake 化”的表述相比，更偏工程稳妥。

### 5.2 根枚举

根枚举在：

- [src/Heap/Collector/MarkSweepCollector.cpp](src/Heap/Collector/MarkSweepCollector.cpp#L292)

它复用了 `TracingCollector` 的 `DoEnumeration`，但对象标记逻辑已经换成 MSGC 自己的 `MarkObject`。

### 5.3 并发标记

并发标记在：

- [src/Heap/Collector/MarkSweepCollector.cpp](src/Heap/Collector/MarkSweepCollector.cpp#L342)
- [src/Heap/Collector/MarkSweepCollector.cpp](src/Heap/Collector/MarkSweepCollector.cpp#L368)

当前模型是：

- GC 线程维护本地 work stack
- 另有一个 global work queue
- Mutator assist 理论上可以参与

不过当前代码里 assist 的关键入口虽然存在：

- [src/Heap/Collector/MarkSweepCollector.cpp](src/Heap/Collector/MarkSweepCollector.cpp#L555)

但在分配路径里是被注释掉的：

- [src/Heap/Allocator/AllocBufferMS.cpp](src/Heap/Allocator/AllocBufferMS.cpp#L59)

所以现状应该理解为：

> 并发标记框架支持 assist，但生产路径里 assist 目前基本处于关闭状态。

### 5.4 MarkTermination

MarkTermination 是 STW 收敛阶段，核心工作是：

1. 停世界
2. flush 所有 write barrier buffer
3. 收集 extern/finalizer resurrection 候选
4. drain main queue，把剩余灰对象都追完

对应代码：

- [src/Heap/Collector/MarkSweepCollector.cpp](src/Heap/Collector/MarkSweepCollector.cpp#L613)
- [src/Heap/Collector/MarkSweepCollector.cpp](src/Heap/Collector/MarkSweepCollector.cpp#L673)

这一步非常关键，因为它决定并发标记结束时是否真正闭合了灰队列。

### 5.5 PostMarkTermination

后处理阶段在：

- [src/Heap/Collector/MarkSweepCollector.cpp](src/Heap/Collector/MarkSweepCollector.cpp#L754)

它主要负责：

1. phase 切换到 `MS_SWEEP`
2. 处理弱引用
3. 清空 WBBuf
4. 入队 finalizer
5. 处理 cycle reference
6. 回收 large garbage

这一步结束后，标记期就算完成，下一步进入 sweep 的代际翻转。

---

## 6. MarkObject 与 mark-at-alloc

### 6.1 MarkObject 真实写入点

MSGC 的对象标记已经不走 CopyGC 的 LiveInfo 语义，而是分两类：

- small/medium object：写 `MSRegionInfo::markBitmap`
- large object：写 base region 的 `isMarked`

实现位置：

- [src/Heap/Allocator/RegionSpace.cpp](src/Heap/Allocator/RegionSpace.cpp#L312)
- [src/Heap/Collector/MarkSweepCollector.cpp](src/Heap/Collector/MarkSweepCollector.cpp#L250)

### 6.2 新分配对象的处理

当前 MSGC 使用了明确的 **mark-at-alloc** 策略。

一处在正常分配路径：

- [src/Heap/Allocator/AllocBufferMS.cpp](src/Heap/Allocator/AllocBufferMS.cpp#L49)

另一处在 finalizer 创建：

- [src/Common/BaseObject.cpp](src/Common/BaseObject.cpp#L154)

这意味着，在标记期间新创建对象会直接被染黑，从而降低屏障必须做 insertion shading 的压力。

这也是理解当前写屏障实现为什么比设计稿更保守的关键前提。

---

## 7. 屏障实现：设计稿与现状的差别最大处

### 7.1 设计稿写的是 Hybrid Barrier

Step3 文档主张的是 Go 风格 hybrid barrier：

- `shade(old)`
- `shade(new)`
- 再执行写入

头文件仍保留这种表述：

- [src/Heap/Barrier/MSWriteBarrier.h](src/Heap/Barrier/MSWriteBarrier.h#L17)

### 7.2 实现现状更接近 SATB deletion barrier

真正的实现要看：

- [src/Heap/Barrier/MSWriteBarrier.cpp](src/Heap/Barrier/MSWriteBarrier.cpp#L55)
- [src/Heap/Barrier/MSWriteBarrier.cpp](src/Heap/Barrier/MSWriteBarrier.cpp#L132)

当前真实行为是：

- 普通 `WriteReference`：只 shade old
- `WriteStaticRef`：只 shade old
- `WriteStruct`：shade 被覆盖掉的旧引用，不 shade new
- `CopyRefArray`：shade old，不 shade new
- 原子写路径：主要也是 shade old

并且源码里多次直接写了：

- `No shade(new) for struct writes`
- `No shade(new) for array copy`

也就是说，现状更准确的理解是：

> MSGC 已经有了 MS 专属写屏障，但实现策略更偏 SATB deletion barrier，而不是完全兑现设计稿中的 hybrid insertion+deletion barrier。

### 7.3 WriteBarrierBuffer 的作用

WBBuf 定义在：

- [src/Heap/Barrier/WriteBarrierBuffer.h](src/Heap/Barrier/WriteBarrierBuffer.h#L18)

它的作用不是决定对象该不该标，而是：

- 把 barrier 已经新标黑的对象缓存下来
- 在 MarkTermination 下统一 flush 到 main queue

flush 调用点：

- [src/Heap/Collector/MarkSweepCollector.cpp](src/Heap/Collector/MarkSweepCollector.cpp#L673)

---

## 8. sweep 实现主线

### 8.1 三条 sweep 路径已经全部在代码里

Step4 设计的三路 sweep 已经在代码中形成闭环：

1. **lazy sweep**
   在 `GetRegion` 过程中，碰到 unswept region 时当场 sweep

2. **background sweep**
   gc-helper 线程后台跑 sweep

3. **force sweep**
   下一轮 GC 开始前强制兜底 sweep

关键代码：

- lazy: [src/Heap/Allocator/CentralRegionPool.cpp](src/Heap/Allocator/CentralRegionPool.cpp#L21)
- force: [src/Heap/Allocator/CentralRegionPool.cpp](src/Heap/Allocator/CentralRegionPool.cpp#L233)
- prepare: [src/Heap/Allocator/CentralRegionPool.cpp](src/Heap/Allocator/CentralRegionPool.cpp#L366)
- background: [src/Heap/Allocator/CentralRegionPool.cpp](src/Heap/Allocator/CentralRegionPool.cpp#L435)

后台触发来自：

- [src/Heap/Collector/FinalizerProcessor.cpp](src/Heap/Collector/FinalizerProcessor.cpp#L307)

### 8.2 sweepGen 是协调核心

设计文档里提到的 `globalSweepGen += 2` 机制已经落地。

实现位置：

- [src/Heap/Allocator/CentralRegionPool.cpp](src/Heap/Allocator/CentralRegionPool.cpp#L18)
- [src/Heap/Allocator/CentralRegionPool.cpp](src/Heap/Allocator/CentralRegionPool.cpp#L427)

它的作用是：

- 不需要显式重建所有链表状态
- 通过 generation flip 让“原先 swept 的 region”在新周期里逻辑上变成 unswept
- 不同 sweep 路径通过 CAS 抢占 `sg-2 → sg-1 → sg` 来避免重复 sweep

### 8.3 SweepRegion 是 per-region 原子单元

虽然本文件中没有展开 `MSRegionInfo::SweepRegion` 的具体实现，但从 `CentralRegionPool` 的调用方式可以确认：

- sweep 的最小原子单元就是一个 region
- 调度层只负责 CAS 获取清扫权和分类回链
- 真正的 bitmap 翻转发生在 `region->SweepRegion()` 内部

因此 Step4 文档关于“bitmap NOT sweep”的总体方向是可信的。

---

## 9. Finalizer 与 background sweep 的关系

文档里 Step4.3 讲了后台 sweep 复用 gc-helper 线程，这一点已经落地。

关键函数：

- [src/Heap/Collector/FinalizerProcessor.cpp](src/Heap/Collector/FinalizerProcessor.cpp#L307)

当前实现中，gc-helper 线程除了 finalizer 相关任务，还会调用 `CentralRegionPool::BackgroundSweep()`。

这使得 sweep 真正成为 collector 收尾阶段的一部分，而不是一个和 finalizer 完全平行的新线程系统。

---

## 10. MSRegionManager 的真实状态

### 10.1 Step6 并不是“未实现”

Step6 文档如果只看开头，容易误以为是未来方案；但代码里已经落了很大一部分。

最关键的一点：

`MSRegionInfo[]` 已经直接放在 heap mmap 前段。

对应位置：

- [src/Heap/Allocator/MSRegionManager.cpp](src/Heap/Allocator/MSRegionManager.cpp#L105)

这里直接做了：

- `array_ = reinterpret_cast<MSRegionInfo*>(mmapBase)`

这说明旧文档里“把 MSRegionInfo 放到 heap 前方元数据区”的核心设想，实际已经落地。

### 10.2 committed heap 增长也已经存在

文档里提出的 heap growth，在代码里不是以 `HeapGrowthManager` 这种独立类实现的，而是直接内嵌进 `MSRegionManager`。

关键函数：

- [src/Heap/Allocator/MSRegionManager.cpp](src/Heap/Allocator/MSRegionManager.cpp#L545)

调用点也已经分布在常规分配路径中：

- [src/Heap/Allocator/MSRegionManager.cpp](src/Heap/Allocator/MSRegionManager.cpp#L184)
- [src/Heap/Allocator/MSRegionManager.cpp](src/Heap/Allocator/MSRegionManager.cpp#L249)

因此更准确的表述应当是：

> Step6 的核心能力已经存在，但不是完全按设计稿里的抽象层次拆出来，而是直接融进了 MSRegionManager。

### 10.3 尚未完全按设计稿抽象化

没有明显看到完整独立出来的：

- `HeapGrowthManager`
- `HeapGoalController`
- 更细致的增长策略对象化封装

所以 Step6 当前是“核心机制已做，抽象层次未完全按文档分离”。

---

## 11. GC 触发策略与 benchmark 文档的对应关系

你当前打开的 benchmark 文档讨论了 Round4 的 GC 触发策略优化。

对应代码已经存在：

- [src/Heap/Collector/MarkSweepCollector.cpp](src/Heap/Collector/MarkSweepCollector.cpp#L1071)

当前 MSGC 的 `UpdateGCStats` 已经做了两件事：

1. 使用 `trueLiveBytes`，而不是简单的 committed bytes
2. 使用 Go 风格 `liveBytes × (1 + heapGrowth)` 更新阈值

这和 [doc/benchmark/Round4_GC触发优化效果.md](doc/benchmark/Round4_GC%E8%A7%A6%E5%8F%91%E4%BC%98%E5%8C%96%E6%95%88%E6%9E%9C.md) 里的描述是对得上的。

因此 Round4 文档可以视为“MSGC 当前阶段的真实性能观察”，而不是纯理论推导。

---

## 12. 文档与实现的主要差异

我认为最重要的差异有四个：

### 差异 1：屏障设计比实现更激进

文档倾向于完整 hybrid barrier。

代码实际更保守，更偏 SATB deletion barrier，加上 mark-at-alloc 补偿。

这是当前 MSGC 理解里最需要避免误判的地方。

### 差异 2：大对象模型没有完全收敛为单一路由

文档常把大对象路径描述成分层清晰的 Tier 模型。

代码里则表现为：

- 堆内 large region 主路径
- LOS 辅助路径仍保留

collector 两边都兼容。

### 差异 3：Step6 的核心机制已经落地，但抽象没有完全照设计稿拆

如果只看文档，会以为还在规划。

实际 `MSRegionInfo` 前置到 heap 元数据区、GrowOneSegment 增长 committed heap，这些核心点都已经落了。

### 差异 4：mutator assist 框架在，主路径默认偏关闭

文档把 assist 作为并发标记的重要组成。

代码里虽然实现了 `MSMarkAssist`，但在 `AllocBufferMS` 里相关触发逻辑当前被注释关闭。

---

## 13. 当前 MSGC 的一条真实调用链

如果从一次典型对象分配和一次 GC 周期来串，可以理解成：

1. `RegionSpace::Allocate` 进入 MS 路径
2. 小对象走 `AllocBufferMS`，大对象走 `MSRegionManager::TakeRegionForLarge`
3. 标记期内新分配对象直接 mark-at-alloc
4. 触发 GC 后，`MarkSweepCollector::RunGarbageCollection` 进入主循环
5. `MarkSetup` 先 `ForceFinishSweep`，再 STW flush TL regions 并清 bitmap
6. 根枚举后进入 `ConcurrentMark`
7. mutator 写屏障记录旧引用并把新染黑对象压进 WBBuf
8. `MarkTermination` 下统一 flush 所有 WBBuf 并 drain 灰队列
9. `PostMarkTermination` 处理 weak ref、finalizer、large garbage
10. `ConcurrentSweep` 中 `PrepareForSweep` 做 generation flip
11. 后台线程做 `BackgroundSweep`
12. 分配线程在缺 region 时做 lazy sweep
13. 如果下一轮 GC 到来而 sweep 未完成，则 `ForceFinishSweep` 兜底

这条链路已经是完整闭环。

---

## 14. 我对当前 MSGC 的最终理解

当前仓颉 Runtime 里的 MSGC，可以被描述为：

> 一套已经基本成型的、编译期切换的 Go 风格 Mark-Sweep GC。它在架构上成功复用了 TracingCollector 的标记基础设施，在实现上用 MSRegionManager、AllocBufferMS、CentralRegionPool 和 sweepGen 机制替代了 CopyGC 的 region forwarding 体系；但在 barrier 和 assist 等细节上，实际实现比设计稿更保守、更工程化。

换句话说：

- **不是 PPT 级设计**
- **也不是所有设计稿都 1:1 落地**
- **而是一套核心闭环已经打通、局部策略仍在迭代的 MSGC**

---

## 15. 建议的继续阅读顺序

如果后续要继续深入，我建议按下面顺序读：

1. [src/Heap/Collector/MarkSweepCollector.cpp](src/Heap/Collector/MarkSweepCollector.cpp#L67)
2. [src/Heap/Allocator/RegionSpace.cpp](src/Heap/Allocator/RegionSpace.cpp#L36)
3. [src/Heap/Allocator/AllocBufferMS.cpp](src/Heap/Allocator/AllocBufferMS.cpp#L49)
4. [src/Heap/Allocator/CentralRegionPool.cpp](src/Heap/Allocator/CentralRegionPool.cpp#L21)
5. [src/Heap/Barrier/MSWriteBarrier.cpp](src/Heap/Barrier/MSWriteBarrier.cpp#L132)
6. [src/Heap/Allocator/MSRegionManager.cpp](src/Heap/Allocator/MSRegionManager.cpp#L65)
7. [src/Heap/Collector/FinalizerProcessor.cpp](src/Heap/Collector/FinalizerProcessor.cpp#L307)

这七个文件基本就能把当前 MSGC 的真实实现串起来。