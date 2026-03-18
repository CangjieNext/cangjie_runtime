# MSGC Sweep 实现与文档差异

## 1. 目的

本文档专门记录 MSGC 的 sweep 现阶段实现，与 Step4 系列设计文档之间的差异。

目标不是否定原设计，而是把当前代码状态说清楚，避免后续阅读文档时把“设计目标”“已实现能力”“尚未接通的链路”混在一起。

相关设计文档：

- [doc/Step4_并发清扫详细设计.md](doc/Step4_%E5%B9%B6%E5%8F%91%E6%B8%85%E6%89%AB%E8%AF%A6%E7%BB%86%E8%AE%BE%E8%AE%A1.md)
- [doc/Step4.1_SweepRegion详细实现.md](doc/Step4.1_SweepRegion%E8%AF%A6%E7%BB%86%E5%AE%9E%E7%8E%B0.md)
- [doc/Step4.2_Sweep调度与CentralRegionPool集成.md](doc/Step4.2_Sweep%E8%B0%83%E5%BA%A6%E4%B8%8ECentralRegionPool%E9%9B%86%E6%88%90.md)
- [doc/Step4.3_后台Sweep与FinalizerProcessor集成.md](doc/Step4.3_%E5%90%8E%E5%8F%B0Sweep%E4%B8%8EFinalizerProcessor%E9%9B%86%E6%88%90.md)

核心实现位置：

- [src/Heap/Allocator/MSRegionInfo.cpp](src/Heap/Allocator/MSRegionInfo.cpp#L211)
- [src/Heap/Allocator/CentralRegionPool.cpp](src/Heap/Allocator/CentralRegionPool.cpp#L21)
- [src/Heap/Collector/MarkSweepCollector.cpp](src/Heap/Collector/MarkSweepCollector.cpp#L125)
- [src/Heap/Collector/FinalizerProcessor.cpp](src/Heap/Collector/FinalizerProcessor.cpp#L79)

---

## 2. 先给结论

当前 sweep 实现与文档的关系，可以概括成三句话：

1. **核心算法是对的**：单 Region 的 bitmap NOT sweep 已经真实落地。
2. **主路径比文档更保守**：当前主要依赖 lazy sweep 和下一轮 GC 开始前的 force sweep。
3. **后台 sweep 代码已存在，但启动链路没有完全按文档接通**。

也就是说，Step4 的骨架已经形成，但“后台 sweep 成为主力路径”这件事，在当前代码里还不能算完全成立。

---

## 3. 已经与文档一致的部分

### 3.1 SweepRegion 的核心算法已经落地

设计文档在 [doc/Step4.1_SweepRegion详细实现.md](doc/Step4.1_SweepRegion%E8%AF%A6%E7%BB%86%E5%AE%9E%E7%8E%B0.md#L87) 中把核心算法定义为：

`allocBitmap = ~markBitmap`

当前代码实现位置：

- [src/Heap/Allocator/MSRegionInfo.cpp](src/Heap/Allocator/MSRegionInfo.cpp#L211)

真实逻辑包括：

- 按 word 遍历 `markBitmap`
- 执行 `allocBitmap[i] = ~markBitmap[i]`
- 统计 `liveCount`
- 修正尾 word 的无效 bit
- 清空 `markBitmap`
- 更新 `liveSlotCount/freeSlotCount`
- 将 `sweepState` 设为 `SWEPT`

这部分和文档基本一致。

### 3.2 lazy sweep 的位置与文档一致

文档在 [doc/Step4.2_Sweep调度与CentralRegionPool集成.md](doc/Step4.2_Sweep%E8%B0%83%E5%BA%A6%E4%B8%8ECentralRegionPool%E9%9B%86%E6%88%90.md#L130) 中描述：

- 分配线程在 `GetRegion(cls)` 过程中
- 从 `partialUnswept/fullUnswept` 取 region
- CAS 抢 sweep 权
- sweep 完后直接返回可用 region

当前代码实现位置：

- [src/Heap/Allocator/CentralRegionPool.cpp](src/Heap/Allocator/CentralRegionPool.cpp#L21)

这里的优先级和文档一致：

1. `partialSwept`
2. `partialUnswept`
3. `fullUnswept`
4. fresh region

第二、第三优先级里确实会直接执行 `region->SweepRegion()`。

### 3.3 ForceFinishSweep 的职责与文档一致

文档在 [doc/Step4.2_Sweep调度与CentralRegionPool集成.md](doc/Step4.2_Sweep%E8%B0%83%E5%BA%A6%E4%B8%8ECentralRegionPool%E9%9B%86%E6%88%90.md#L224) 中把 `ForceFinishSweep()` 定义为“下一轮 GC 之前的兜底清扫”。

当前代码实现位置：

- [src/Heap/Allocator/CentralRegionPool.cpp](src/Heap/Allocator/CentralRegionPool.cpp#L233)
- [src/Heap/Collector/MarkSweepCollector.cpp](src/Heap/Collector/MarkSweepCollector.cpp#L140)

collector 在 `MarkSetup` 一开始就调用它，这一点和设计目标一致。

### 3.4 generation flip 机制已落地

文档把 `globalSweepGen += 2` 作为 Step4 的关键机制，当前代码也确实这么做了：

- [src/Heap/Allocator/CentralRegionPool.cpp](src/Heap/Allocator/CentralRegionPool.cpp#L18)
- [src/Heap/Allocator/CentralRegionPool.cpp](src/Heap/Allocator/CentralRegionPool.cpp#L427)

因此，“用 sweep generation 让 Region 在不同 GC 周期之间切换 swept/unswept 身份”的总体思路是已经落地的。

---

## 4. 主要出入点

## 4.1 出入一：后台 sweep 不是当前的稳定主路径

### 文档表述

Step4 和 Step4.3 文档都把 background sweep 描述为三路协作里的主力路径：

- [doc/Step4_并发清扫详细设计.md](doc/Step4_%E5%B9%B6%E5%8F%91%E6%B8%85%E6%89%AB%E8%AF%A6%E7%BB%86%E8%AE%BE%E8%AE%A1.md#L111)
- [doc/Step4.3_后台Sweep与FinalizerProcessor集成.md](doc/Step4.3_%E5%90%8E%E5%8F%B0Sweep%E4%B8%8EFinalizerProcessor%E9%9B%86%E6%88%90.md#L25)

文档预期是：

- `PrepareForSweep()` 之后
- collector 会通知 gc-helper
- gc-helper 主动遍历 unswept lists 做 background sweep

### 当前实现

后台 sweep 的执行函数本身确实存在：

- [src/Heap/Collector/FinalizerProcessor.cpp](src/Heap/Collector/FinalizerProcessor.cpp#L308)
- [src/Heap/Allocator/CentralRegionPool.cpp](src/Heap/Allocator/CentralRegionPool.cpp#L435)

但是 collector 在 `ConcurrentSweep()` 中并没有真正发出 `NotifyToSweep()`：

- [src/Heap/Collector/MarkSweepCollector.cpp](src/Heap/Collector/MarkSweepCollector.cpp#L868)

可以看到原本的 sweep 通知被注释掉了：

- `fp.NotifyToSweep();` 被注释

当前真正调用的是：

- [src/Heap/Collector/MarkSweepCollector.cpp](src/Heap/Collector/MarkSweepCollector.cpp#L877)

也就是 `NotifyToReclaimGarbage()`。

### 结论

当前代码里：

- background sweep 逻辑存在
- finalizer 线程也支持接收 `shouldBackgroundSweep_`
- 但 collector 默认没有把它作为常规 sweep 启动路径接通

因此这部分和文档的差异应描述为：

> 后台 sweep 已实现函数体，但尚未形成文档描述的“GC 结束后自动启动的稳定主路径”。

---

## 4.2 出入二：并发仲裁主要靠 sweepGen，不是 sweepState

### 文档表述

Step4.1 文档在 [doc/Step4.1_SweepRegion详细实现.md](doc/Step4.1_SweepRegion%E8%AF%A6%E7%BB%86%E5%AE%9E%E7%8E%B0.md#L164) 里多次强调：

- `UNSWEPT → SWEEPING → SWEPT`
- 通过 `sweepState` CAS 保证唯一 sweep 所有权

### 当前实现

真实代码里，抢 sweep 所有权的关键 CAS 是：

- [src/Heap/Allocator/MSRegionInfo.h](src/Heap/Allocator/MSRegionInfo.h#L173)
- [src/Heap/Allocator/CentralRegionPool.cpp](src/Heap/Allocator/CentralRegionPool.cpp#L67)
- [src/Heap/Allocator/CentralRegionPool.cpp](src/Heap/Allocator/CentralRegionPool.cpp#L253)
- [src/Heap/Allocator/CentralRegionPool.cpp](src/Heap/Allocator/CentralRegionPool.cpp#L472)

也就是：

- `CasSweepGen(sg - 2, sg - 1)` 抢占
- sweep 完成后 `SetSweepGen(sg)`

`sweepState` 确实存在，见：

- [src/Heap/Allocator/MSRegionInfo.h](src/Heap/Allocator/MSRegionInfo.h#L42)

但它更像辅助状态和调试信息。`MSRegionInfo::SweepRegion()` 本身并没有文档里那种“assert 当前必须是 SWEEPING 再执行”的完整状态机约束。

### 结论

更准确的现状应该表述为：

> 真实的并发仲裁器是 `sweepGen`，`sweepState` 主要用于表示当前状态，而不是决定所有权的唯一同步原语。

---

## 4.3 出入三：PrepareForSweep 不只是 O(1) 的纯逻辑翻转

### 文档表述

Step4.2 文档在 [doc/Step4.2_Sweep调度与CentralRegionPool集成.md](doc/Step4.2_Sweep%E8%B0%83%E5%BA%A6%E4%B8%8ECentralRegionPool%E9%9B%86%E6%88%90.md#L280) 更强调 `globalSweepGen += 2` 的逻辑效果，容易让人理解成“不动链表”。

### 当前实现

真实代码在：

- [src/Heap/Allocator/CentralRegionPool.cpp](src/Heap/Allocator/CentralRegionPool.cpp#L366)

这里除了 `globalSweepGen_.fetch_add(2)` 之外，还做了两件事：

1. `partialSwept.DrainTo(partialUnswept)`
2. `fullSwept.Swap(fullUnswept)`

所以当前实现不是“单纯通过 generation 逻辑翻转而不碰链表”，而是：

- generation flip
- 加上列表重分类

### 结论

更准确的描述应为：

> `PrepareForSweep()` 是“代际翻转 + 少量链表调整”的复合操作，而不是完全纯粹的 O(1) 逻辑翻转。

---

## 4.4 出入四：大对象 sweep 在 ForceFinishSweep 里又兜底做了一次

### 文档表述

Step4 总文档在 [doc/Step4_并发清扫详细设计.md](doc/Step4_%E5%B9%B6%E5%8F%91%E6%B8%85%E6%89%AB%E8%AF%A6%E7%BB%86%E8%AE%BE%E8%AE%A1.md#L47) 写的是：

- 大对象回收由 `PostMarkTermination::CollectLargeGarbage()` 负责
- Step4 不重复实现

### 当前实现

collector 确实在 PostMarkTermination 做了大对象回收：

- [src/Heap/Collector/MarkSweepCollector.cpp](src/Heap/Collector/MarkSweepCollector.cpp#L831)

但 `ForceFinishSweep()` 尾部又显式调用了一次：

- [src/Heap/Allocator/CentralRegionPool.cpp](src/Heap/Allocator/CentralRegionPool.cpp#L324)

也就是 `SweepLargeRegions()`。

### 结论

这一点与文档不一致，但从代码语义上看，是合理兜底：

> 当前实现不仅在 PostMarkTermination 回收 large region，还在 ForceFinishSweep 中再兜底一次，以避免 large allocation 因连续页没有及时归还而失败。

---

## 4.5 出入五：Step4.3 里的通知模型比代码实现更理想化

### 文档表述

Step4.3 文档给出了一套近似独立的 MS 版 finalizer/sweep 运行框架，包括：

- `RunMS()`
- `WaitForMarkDone()`
- `NotifyMarkDone()`

参考位置：

- [doc/Step4.3_后台Sweep与FinalizerProcessor集成.md](doc/Step4.3_%E5%90%8E%E5%8F%B0Sweep%E4%B8%8EFinalizerProcessor%E9%9B%86%E6%88%90.md#L88)
- [doc/Step4.3_后台Sweep与FinalizerProcessor集成.md](doc/Step4.3_%E5%90%8E%E5%8F%B0Sweep%E4%B8%8EFinalizerProcessor%E9%9B%86%E6%88%90.md#L154)

### 当前实现

真实代码没有独立的 `RunMS()`，而是继续沿用通用 `Run()` 主循环：

- [src/Heap/Collector/FinalizerProcessor.cpp](src/Heap/Collector/FinalizerProcessor.cpp#L79)

主循环中只是在既有框架上增加了：

- `shouldBackgroundSweep_`
- `BackgroundSweepLoop()`

对应定义：

- [src/Heap/Collector/FinalizerProcessor.h](src/Heap/Collector/FinalizerProcessor.h#L99)

### 结论

更准确的说法是：

> 后台 sweep 是“嵌入现有 FinalizerProcessor 事件循环”的，而不是已经形成了一套文档中那样独立、专门面向 MS 的调度主循环。

---

## 4.6 出入六：EnsureSweepDone 设计已经被 ForceFinishSweep 直接替代

### 文档表述

Step4.3 文档把 `EnsureSweepDone()` 作为一个协调机制重点描述：

- [doc/Step4.3_后台Sweep与FinalizerProcessor集成.md](doc/Step4.3_%E5%90%8E%E5%8F%B0Sweep%E4%B8%8EFinalizerProcessor%E9%9B%86%E6%88%90.md#L432)

### 当前实现

在 collector 中：

- [src/Heap/Collector/MarkSweepCollector.cpp](src/Heap/Collector/MarkSweepCollector.cpp#L240)

`EnsureSweepDone()` 已经变成 no-op，注释也明确写了：

- 实际 sweep completion 由 `MarkSetup -> ForceFinishSweep()` 保证

### 结论

这部分应视为文档过时：

> 当前实现里真正有效的机制不是 `EnsureSweepDone()`，而是每轮 GC 开始时直接执行 `ForceFinishSweep()`。

---

## 5. 次要差异与实现细节

### 5.1 SweepRegion 返回值与文档示例略有差异

Step4.1 文档示例把 `SweepRegion()` 写成返回 `freedSlots` 或类似计数；当前真实实现在：

- [src/Heap/Allocator/MSRegionInfo.cpp](src/Heap/Allocator/MSRegionInfo.cpp#L211)

返回值实际上是：

- `freeSlotCount * slotSize`

也就是**释放字节数**，而不是释放 slot 数。

### 5.2 `sweepState` 初始化和使用比文档更弱

当前 `InitForSizeClass`、`Reset`、`ResetToBump` 确实都会操作 `sweepState`：

- [src/Heap/Allocator/MSRegionInfo.cpp](src/Heap/Allocator/MSRegionInfo.cpp#L12)
- [src/Heap/Allocator/MSRegionInfo.cpp](src/Heap/Allocator/MSRegionInfo.cpp#L52)
- [src/Heap/Allocator/MSRegionInfo.cpp](src/Heap/Allocator/MSRegionInfo.cpp#L76)

但 sweep 调度代码几乎不依赖它做逻辑判断，更多依赖：

- `sweepGen`
- `freeSlotCount`
- `HasFreeSpace()`
- `IsCompletelyFree()`

### 5.3 force sweep 的日志与诊断能力强于文档

当前 `ForceFinishSweep()` 有比较重的错误日志和统计输出：

- [src/Heap/Allocator/CentralRegionPool.cpp](src/Heap/Allocator/CentralRegionPool.cpp#L273)
- [src/Heap/Allocator/CentralRegionPool.cpp](src/Heap/Allocator/CentralRegionPool.cpp#L320)
- [src/Heap/Allocator/CentralRegionPool.cpp](src/Heap/Allocator/CentralRegionPool.cpp#L330)

这说明在现实运行中，force sweep 已经不仅是“冷门兜底路径”，还是一个被频繁观测和调试的重要路径。

---

## 6. 现阶段更准确的 sweep 描述

结合代码现状，当前 MSGC 的 sweep 更准确的描述应该是：

1. 单 Region sweep 已经完整实现，算法是 bitmap NOT。
2. 分配路径上的 lazy sweep 是当前最可靠、最核心的在线 sweep 路径。
3. 下一轮 GC 开始前的 `ForceFinishSweep()` 是保证正确性的最终兜底。
4. background sweep 的函数体与线程集成代码已经具备，但 collector 默认启动链路尚未完全接通。
5. 并发协调以 `sweepGen` CAS 为核心，`sweepState` 主要是状态表达和诊断辅助。

如果压缩成一句话：

> 当前 Step4 已经实现了“能工作”的 sweep，但它还没有完全达到设计文档中“后台 sweep 成为主力回收者”的阶段，现状更像“lazy + force 为主，background 为预留或半接通能力”。

---

## 7. 建议如何修正文档认知

如果后续要回写或修正文档，我建议这样处理：

1. 在 Step4 总文档中，把“后台 sweep 是主力路径”改成“目标路径，当前代码默认仍以 lazy + force 为主”。
2. 在 Step4.1 中把 `sweepState` 的表述降级，改成“辅助状态字段”，明确真实仲裁靠 `sweepGen`。
3. 在 Step4.2 中把 `PrepareForSweep()` 改写为“generation flip + list reshaping”。
4. 在 Step4.3 中标注 `RunMS/WaitForMarkDone/NotifyMarkDone` 为理想化方案，说明当前实现仍复用通用 `FinalizerProcessor::Run()`。
5. 补充一条“当前代码在 ForceFinishSweep 中也会 sweep large region 作为兜底”的说明。

---

## 8. 总结

从 sweep 角度看，当前文档和代码不是“完全偏离”，而是“主干一致，落地策略更保守”。

真正需要防止的误解只有两个：

1. 不要把 background sweep 当成当前已经稳定启用的主路径。
2. 不要把 `sweepState` 当成真实的并发控制核心。

除此之外，Step4 的大方向，尤其是：

- bitmap NOT
- 三路 sweep 架构
- generation flip
- force sweep 兜底

这些核心思想都已经真实进入代码。