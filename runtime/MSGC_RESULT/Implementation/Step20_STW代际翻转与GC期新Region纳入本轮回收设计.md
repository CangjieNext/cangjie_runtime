# Step20：STW 代际翻转与 GC 期间新 Region 纳入本轮回收设计

> 目标：解决“MSGC 在本轮 GC 期间新产生的 region 不参与本轮 sweep，导致大量 GC 期新 region 只能等下一轮再复用/回收，推动 RSS 升高”的问题。
>
> 本方案同时给出 runtime 与 LLVM fast path 的协同改造设计。

---

## 1. 问题背景

当前实现沿用 Step9 的语义：

1. `PreEnumRoots()` 中执行：
   - `StopAndWaitForAllSweep()`
   - `ClearAllMarkBitmaps()`
   - `ReclassifyUnsweptToSwept()`
   - `PrepareForSweep()`
2. `PrepareForSweep()` 会把 `globalSweepGen_ += 2`，从而在 **PreEnum 阶段** 冻结本轮 sweep snapshot。
3. 之后 GC 期间新申请到的 region 会直接带上新的 `sweepGen`，因此逻辑上已经属于“本轮已 swept”对象。
4. 等到 `ConcurrentSweep()` / lazy sweep 开始时，这些 region 不会参与本轮 sweep，只能等下一轮 GC。

这条语义在 Step9 中是有意引入的，用来避免“PreEnum 之后 mutator 归还的 region 混入本轮 sweep snapshot”。

但副作用也很明显：

- GC 期间如果 mutator 继续大量分配，就会生成大量新 region
- 这些 region 即使其中对象在本轮 GC 结束前已经死亡，也不会在本轮 sweep 中被回收
- hotFree / emptySwept / scavenger 的供给被推迟一整轮 GC
- RSS 峰值被人为抬高，特别是在高分配压力 case 下更明显

用户提出的新目标是反过来做：

- **让 GC 期间新产生的 region 也参与本轮 GC 的 sweep**
- 但要保证 GC 期间新创建对象不会被误回收
- 因此需要在分配时给新对象做 mark-at-alloc
- 同时给这类新对象打一个“需要被 barrier 触发补扫”的标记
- 当 barrier 命中这种对象时，要把该对象本身入队，保证其子对象最终会被扫描到

---

## 2. 现状与根因

### 2.1 当前时序

当前关键路径如下：

```text
PreEnumRoots()
  StopAndWaitForAllSweep()
  ClearAllMarkBitmaps()
  ReclassifyUnsweptToSwept()
  PrepareForSweep()              # 提前 generation flip

EnumRoots()
ConcurrentMark()

MarkTermination()  # STW
  FlushAllWriteBarrierBuffers()
  DrainGlobalLFStackToMainQueue()
  ExecuteLikeDrainMainQueue()
  TransitionToGCPhase(MS_SWEEP)
  ReenableLazySweep()

ConcurrentSweep()
```

问题不在 `TransitionToGCPhase(MS_SWEEP)`，而在 `PrepareForSweep()` 已经在 `PreEnumRoots()` 发生。

### 2.2 为什么新 region 被排到下一轮

`CentralRegionPool::PrepareForSweep()` 的本质是：

- 把全局 `globalSweepGen_` 加 2
- 让原本 `swept` 的 region 在逻辑上全部变成“本轮 unswept”
- 从而冻结本轮要参与 sweep 的 region 集合

一旦这个动作发生在 `PreEnumRoots()`：

- GC 期间新分配的 region 会直接读取新的 `globalSweepGen_`
- 它们在本轮结束时天然被视为“已经 swept”
- 因此不会进入本轮的 eager sweep / lazy sweep / background sweep 路径

### 2.3 为什么单纯 mark-at-alloc 还不够

当前 `MarkSweepCollector::MarkNewObject()` 只做一件事：

```cpp
MarkObject(obj);
```

这只能保证“对象自己不白掉”，不能保证“对象的子对象一定被扫描到”。

原因是：

- 新对象在 mark 期被直接标黑
- 但对象字段往往是在分配之后逐步写入的
- 如果只标黑对象本身，而不保证其最终会有一次字段扫描，那么其后续挂上的子对象可能漏标

当前混合写屏障虽然会对 `old/new ref` 做 discover，但它并不等价于“把新对象本身作为一个待扫描容器入队一次”。

而且现有 `TryEnqueueGreyObject(obj, sink)` 依赖 `MarkObject(obj)` 的白转黑成功；对于已经被 mark-at-alloc 标黑的新对象，**直接调用它不会入队**。

所以本设计需要新增一条语义：

- 新对象分配时除了 mark，还要带一个“newborn-needs-rescan”标记
- barrier 命中该对象时，要把该对象本身以“已标黑但待补扫”的身份入队一次
- 入队成功后清掉该标记，避免重复入队

---

## 3. 设计目标

本方案的目标分四层：

1. 把 generation flip 从 `PreEnumRoots()` 挪到 `MarkTermination()` 的 STW 尾部
2. 让 GC 期间新产生的 region 自动落入本轮 sweep snapshot
3. 通过 mark-at-alloc 保证新对象自己不被 sweep
4. 通过 newborn 标记 + barrier 命中入队，保证新对象的子对象最终会被 trace 到

对应的最终语义是：

```text
本轮 GC 开始时不冻结 sweep snapshot
GC 期间所有新 region 先继续使用旧 sweepGen
MarkTermination(STW) 完成最后一次根/屏障 drain 后
再执行 PrepareForSweep() 冻结最终 snapshot
此时包括 GC 期间新分配/归还的 region 在内，都会进入本轮 sweep
```

---

## 4. 核心方案

### 4.1 时序改造：generation flip 挪到 MarkTermination STW 中

新时序建议改成：

```text
PreEnumRoots()
  StopAndWaitForAllSweep()
  ClearAllMarkBitmaps()
  ReclassifyUnsweptToSwept()
  # 不再 PrepareForSweep()

EnumRoots()
ConcurrentMark()

MarkTermination()  # STW
  FlushAllWriteBarrierBuffers()
  DrainGlobalLFStackToMainQueue()
  DrainDeferredMainThreadQueueToMainQueue()
  ExecuteLikeDrainMainQueue()
  PrepareForSweep()              # generation flip 改到这里
  TransitionToGCPhase(MS_SWEEP)
  ReenableLazySweep()

PostMarkTermination()
ConcurrentSweep()
```

这样会产生两个直接效果：

- GC 期间新申请到的 region 仍然带旧 `sweepGen`
- 直到 `MarkTermination()` STW 结束前才统一 flip，因而这些新 region 会被纳入本轮 unswept snapshot

### 4.2 正确性前提：新对象必须在分配时直接标黑

一旦新 region 进入本轮 sweep，GC 期间新对象就不再能依赖“region 不参加本轮回收”这层保护。

因此分配阶段必须满足：

- 对象一创建完成，就立即写 mark bitmap
- 这条语义必须覆盖：
  - runtime slow path
  - runtime inline/common allocation path
  - LLVM NewObject fast path

也就是：Step10 的 `mark-at-alloc` 要从“只解决对象不会白掉”扩展为“Step20 的 sweep 参与语义前提”。

### 4.3 newborn 标记：对象已黑，但尚未完成一次容器级扫描

本方案引入一个新的对象位语义：

- `NEWBORN_NEEDS_SCAN`

含义：

- 该对象是在本轮 mark phase 中分配出来的
- 对象本身已经 mark 过，不会被本轮 sweep 回收
- 但其字段可能仍在构造/初始化过程中
- 需要在 barrier 首次命中时把对象本身入队一次，做一次容器级 re-scan

### 4.4 标记位放在哪里

建议放在 `StateWord::objectState` 中，扩展为“2 位状态码 + 1 位 newborn flag”。

这里要特别澄清：

- **不是把 newborn 标记写进 `TypeInfo*` 指针值本身**
- 而是写进对象头 `StateWord` 的 `objectState` 子字段
- `StateWord` 本来就是一个 64-bit 对象头，包含：
  - `typeInfoLow32`
  - `typeInfoHigh16`
  - `objectState`

也就是说，newborn bit 与 TypeInfo 共存在同一个 64-bit state word 中，但不等于“篡改 TypeInfo 指针变量本身”。

原因：

- 当前 `StateWord` 注释已经说明：TypeInfo 地址至少 8 字节对齐，最低 3 bit 可复用
- 现在只用了 2 bit：`NORMAL / LOCKED / FORWARDING / FORWARDED`
- MSGC 不需要新增新的 forwarding 状态码，只需要额外 1 bit flag

建议改造方式：

- 保留低 2 bit 作为 `stateCode`
- 使用第 3 bit 作为 `newbornNeedsScan`

即：

```text
bit0..1 : stateCode
bit2    : newbornNeedsScan
```

按当前 runtime 的初始化顺序，对象一旦通过 `SetClassInfo()` / `InitStateWord()` 建立了有效对象头，就已经同时拥有：

- 有效的 TypeInfo bits
- 初始为 0 的 `objectState`

因此 Step20 中 newborn bit 的设置时机应当是：

1. 分配拿到裸地址
2. `SetClassInfo()` / `InitStateWord(klass)` 建立有效 state word
3. `MarkObject(obj)`
4. `SetNewbornNeedsScan(obj)`

也就是说，**newborn bit 必须在对象头初始化之后设置，而不是在“还没写 TypeInfo”时就写进去**。

对 runtime slow path，这一步可以自然挂在 `SetClassInfo()` 之后。

但对 LLVM NewObject fast path，需要额外澄清：

- AArch64 MSGC fast new object 并不是统一回调 runtime 的 `SetClassInfo()`
- 它是在 fast path wrapper 中直接把 klass/stateword 写到对象首字
- 同时直接操作 region 的 alloc bitmap / freeSlot / mark bitmap

因此 Step20 不能假设“newborn 标记都会走到 runtime `SetClassInfo()` 自动完成”。

结论是：

- slow path：可以在 runtime `SetClassInfo()` / `MarkNewObject()` 侧补 newborn 语义
- LLVM fast path：必须自己 inline，或显式调用一个 helper，单独设置 newborn bit

这样可以避免额外 side metadata，也更容易被 LLVM fast path 直接测试/设置。

### 4.5 为什么 barrier 需要“直接入队已标黑对象”

现有写屏障路径最终走到：

- `MSMarkBarrier::EnqueueRef()`
- `MarkSweepCollector::TryEnqueueGreyObject(ref, sink)`

而 `TryEnqueueGreyObject()` 的前提是：

- `MarkObject(obj)` 返回“首次标记成功”

这对 newborn object 不成立，因为 newborn object 已经在 alloc 时被 mark 了。

所以 Step20 需要新增一个与 `TryEnqueueGreyObject()` 不同的接口：

- `EnqueueKnownBlackObjectForRescan(obj)`

其语义是：

1. 对象必须已经 mark
2. barrier 看到对象带 `NEWBORN_NEEDS_SCAN` 标记
3. 通过 CAS 清掉该标记
4. 清标成功者把对象直接推入 WBB / globalFull / mainQueue 之一
5. 后续 drain 该对象时，只做字段 trace，不再依赖 mark-bit 变化

按当前版本的收口约束：

- newborn bit 只作为**本轮 mark 期间的 barrier hint** 使用
- barrier-hit 时可以把 holder 提前 promote 到 mark queue 并清标
- 如果本轮一直没有命中 barrier，则**不在 `MarkTermination()` 的 STW 阶段兜底 drain**
- 残留的 newborn bit 统一在**下一轮 GC 开始**、执行 `ClearAllMarkBitmaps()` 时同步清理

因此 Step20 第一版里 newborn bit 的语义是：

1. alloc 后给“已黑但可能尚未做过一次容器补扫”的对象打标
2. 仅在后续 barrier 命中 holder 时触发 promotion + 清标
3. 普通 trace 路径不主动检查 newborn bit
4. 未被 barrier 消费的残留 bit 由下一轮 GC 开始阶段批量清理

---

## 5. Runtime 侧详细设计

### 5.1 MarkSweepCollector 改动

#### 改动 A：`PreEnumRoots()` 不再调用 `PrepareForSweep()`

保留：

- `StopAndWaitForAllSweep()`
- `ClearAllMarkBitmaps()`
- `ReclassifyUnsweptToSwept()`

新增：

- 在 `ClearAllMarkBitmaps()` 这一步里，同步清理所有对象头上的 `NEWBORN_NEEDS_SCAN` 残留位
- 这一步是上一轮未命中 barrier 的 newborn bit 的统一回收点

移除：

- `PrepareForSweep()`

#### 改动 B：`MarkTermination()` 在 STW 尾部执行 `PrepareForSweep()`

放置位置建议：

- 在最后一次 `ExecuteLikeDrainMainQueue(mainQueue)` 之后
- 在 `TransitionToGCPhase(GC_PHASE_MS_SWEEP, false)` 之前

理由：

- 此时所有 mutator 已冻结
- WBB / globalFull / deferred main-thread work 已完成最后一次 drain
- newborn 对象若已通过 barrier 入队，也已被本轮消费干净
- 此时冻结最终 sweep snapshot 最安全

### 5.2 `MarkNewObject()` 扩展为两段式语义

当前：

```cpp
MarkObject(obj);
```

建议扩展为：

```text
if phase in {MS_MARK_SETUP, MS_CONCURRENT_MARK, MS_MARK_TERMINATION}:
  MarkObject(obj)
  if obj may contain references:
    SetNewbornNeedsScan(obj)
```

这里“may contain references”建议优先按以下策略：

- 对 class / ref-array / struct-array / 含 GCTib bitmap 的对象置位
- 对纯 primitive array 可跳过该位

如果实现成本偏高，也可以第一版先对所有 traceable heap object 统一置位，后续再做裁剪。

第一版里这里不再引入独立的 pending newborn 来源。

newborn bit 只承担两件事：

- 让 barrier 在命中 holder 时知道“这个对象需要做一次容器补扫”
- 让下一轮 GC 开始阶段能批量清掉上一轮遗留的 newborn bit

### 5.3 新增对象头 helper

建议在 `StateWord` / `BaseObject` 上新增 helper：

- `bool HasNewbornNeedsScan() const`
- `void SetNewbornNeedsScan()`
- `bool TryClearNewbornNeedsScan()`

要求：

- `TryClearNewbornNeedsScan()` 必须是 CAS 语义
- 只清 newborn bit，不能破坏低 2 bit 的 `stateCode`
- 兼容 `LOCKED/FORWARDING/FORWARDED` 的现有逻辑

### 5.4 Barrier 改动

新增一个统一 helper，建议放在 `MSMarkBarrier` 或 `MarkSweepCollector`：

- `MaybeEnqueueNewbornContainer(BaseObject* holder)`

调用时机：

- `WriteReference`
- `WriteStaticRef` 不需要，因为无 holder
- `WriteStruct`
- `CopyRefArray`
- `CopyStructArray`
- `AtomicWriteReference`
- `AtomicSwapReference`
- `CompareAndSwapReference` 成功路径

语义：

1. `holder == nullptr` 直接返回
2. 非 heap object / 无效 stateWord 返回
3. 若 holder 不带 `NEWBORN_NEEDS_SCAN`，返回
4. CAS 清标
5. 清标成功者调用 `EnqueueKnownBlackObjectForRescan(holder)`
6. 后续正常走字段 trace，不再依赖 newborn bit

注意：

- 这里 enqueue 的是 `holder` 自己，不是 `ref`
- old/new ref 的 discover 逻辑保留，不替代 Step3 现有混合写屏障
- Step20 是在 Step3 之上额外补一条“新生黑对象补扫”路径

### 5.5 队列与检查路径改动

新增接口：

- `PushKnownBlackToWBB(holder)` 或 `EnqueueKnownBlackObjectForRescan(holder)`

第一版不新增 pending newborn 队列，也不增加 STW drain 路径。

建议只保留两条现有路径：

1. **mark work queue**：继续复用现有 WorkBuf / WBB / globalFull，只承接 barrier-hit 后的 holder promotion
2. **GC start cleanup**：在下一轮 `PreEnumRoots()` 的 `ClearAllMarkBitmaps()` 阶段同步清理残留 newborn bit

推荐时序如下：

```text
alloc during mark
  -> MarkObject(obj)
  -> SetNewbornNeedsScan(obj)

barrier hit holder
  -> TryClearNewbornNeedsScan(holder)
  -> success: EnqueueKnownBlackObjectForRescan(holder)

next GC PreEnumRoots
  -> ClearAllMarkBitmaps()
  -> ClearAllNewbornBits()
```

也就是说：

- newborn 检查点只放在 barrier 命中 holder 的路径上
- 普通 trace 路径不额外判断 newborn bit
- 直接推入队列前，不调用 `TryEnqueueGreyObject()`，避免因为对象已黑而被过滤掉

---

## 6. LLVM Fast Path 侧设计

### 6.1 NewObject fast path

Step10 已经有“mark-at-alloc”的 fast path 经验，Step20 需要在此基础上再加一步：

1. fast path 分配成功后
2. 若 phase 属于 `MS_MARK_SETUP / MS_CONCURRENT_MARK / MS_MARK_TERMINATION`
3. 直接置该对象 mark bitmap
4. 再设置对象 stateWord 的 `NEWBORN_NEEDS_SCAN` 位

对应落点：

- `llvm/lib/CodeGen/CJBarrierLowering.cpp`
- AArch64 / X86 的 NewObject fast path 生成逻辑

建议：

- 保持“mark bitmap set”继续 inline
- “set newborn bit”也 inline 做
- 两者共同构成 Step20 的 alloc fast path 语义

这里不能把 newborn bit 的设置下沉到 runtime `SetClassInfo()`，因为当前 LLVM MSGC fast path 并不会统一回到那个 runtime 入口。

### 6.2 Barrier fast path

先按当前实现事实澄清一层：

- CJ LLVM 当前**确实存在**写屏障 fast path
- 但它本质上是 **phase-based barrier elision**，不是 mark phase 下完整内联 MSGC barrier
- 在 idle/sweep 等不需要写屏障的阶段，它把 barrier intrinsic 直接改写成原生 `store/memcpy/array copy`
- 到真正 mark phase 时，当前实现仍会保留 runtime barrier 调用

因此 Step20 对 barrier fast path 的改动要分两层看：

1. **正确性最小闭环**：如果 mark phase 仍然统一进入 runtime barrier，那么 runtime barrier 里补 newborn 逻辑就已经够了，不必先改 LLVM barrier fast path
2. **性能增强版**：如果后续要在 mark phase 也做更激进的 barrier fast path，再额外插入 newborn 测试/慢调用分支

也就是说，Step20 第一版不是必须改 LLVM barrier fast path 才能正确；它主要是一个后续优化位点。

写屏障 fast path 不建议把“清 newborn bit + WBB push”整套逻辑全内联展开到机器码里，因为：

- 需要访问 mutator TLS / WBB / overflow flush 路径
- 需要 CAS 清对象头 bit
- 需要兼容 overflow / activeCollector 为空等细节
- 机器码会迅速膨胀

如果后续要扩 LLVM barrier fast path，建议做法：

1. LLVM fast barrier 仍保留当前 phase gate / direct store 主逻辑
2. 仅额外插入一个廉价测试：`if (holder has NEWBORN_NEEDS_SCAN)`
3. 命中后调用一个小型 runtime helper，例如：
   - `CJ_MCC_MS_NewbornBarrierHit(obj)`
4. helper 内部完成：
   - CAS 清标
   - 直接把对象入 WBB 或 globalFull

这里也要澄清一点：

- 当前 CJ LLVM **确实存在写屏障 fast path**
- 但它不是“完整内联一套 MSGC 屏障算法”
- 而是先做 GC phase check，在非 mark phase 下把 barrier intrinsic 直接改写成原生 `store/memcpy/array copy`
- 只有需要真正屏障语义时，才继续保留 runtime barrier 调用

因此 Step20 对 LLVM barrier fast path 的扩展，本质也是：

- 在已有 phase-fastpath 基础上，再加一个 newborn-bit 检测/慢调用分支
- 而不是把整个 newborn enqueue 逻辑完全塞进机器码

### 6.3 是否需要新 intrinsic

有两种实现方式：

1. 不加新 intrinsic，直接在 `CJBarrierLowering.cpp` 扩现有 lowering 逻辑
2. 新增一个专用 intrinsic / runtime helper 语义点，例如：
   - `cj_ms_newborn_barrier_hit`

建议第一版优先不新增前端 intrinsic，而是在 CodeGen lowering 层直接扩现有 NewObject / WriteBarrier fast path。

理由：

- 这是 MSGC 专属语义，不一定值得污染前端 IR
- 当前需求更像后端 lowering 和 runtime helper 的协同问题
- 改动集中在 `CJBarrierLowering.cpp` 更符合现有仓颉 GC fast path 的实现风格

### 6.4 长期正确版：把 barrier fast path 的判定来源改成 live TLS fast-state

上面的 6.2/6.3 主要描述的是 Step20 第一版如何在现有 fast path 框架上补 newborn 语义。

但从长期维护和根因修复的角度看，**仅仅在现有 phase-based barrier fast path 上继续打补丁是不够的**。要保留 fast path，同时彻底修掉本次 case24 暴露出来的问题，必须先修正 barrier fast path 的**状态来源**。

#### 6.4.1 本次定位出的真正根因

当前 AArch64 下写屏障 fast path 的 phase check，实际并不是从 `ThreadLocalData` 内存中读取 live phase，而是走一条更激进的寄存器缓存路径：

- compiler 侧 `GetGCPhase` 在 AArch64 上会直接把 `x28` 的高 8 bit 取出来作为返回值
- runtime 侧 `MRT_GetThreadLocalData()` / `MRT_UpdateThreadLocalDataPhase()` 只有在“进入某些 runtime/stub 边界、主动重建 x28”时，才会把 `encodedGCPhase` 重新打包到返回值里
- GC 线程在切 phase 时，能够立即更新的是每个 mutator 对应 `ThreadLocalData` 里的 `gcFastState / encodedGCPhase`
- 但 GC 线程**不能同步改掉另一个正在执行托管代码的 mutator 手里的 x28 寄存器值**

这意味着当前设计天然存在一个 stale window：

```text
GC 已进入 mark phase
  -> TLS 内存中的 gcFastState / encodedGCPhase 已更新
  -> mutator 仍在纯托管代码里继续执行
  -> 它寄存器里的 x28 顶字节还是旧值（例如 idle）
  -> compiler fast path 误判“当前不需要 barrier”
  -> 本应走 runtime helper 的 ref write 被错误改写成 direct store
```

这次 case24 的根因就落在这里：

- 问题不在 Step20 的 newborn 设计本身
- 也不在 runtime 少了一轮 STW 补扫
- 而在于 **direct ref write barrier fast path 依赖了一个不具备跨线程实时一致性的 phase cache**

也就是说，当前 fast path 失败的不是“有没有 newborn test”，而是“进入 fast path 的门本身就用错了信号”。

#### 6.4.2 为什么“继续刷新 x28”不是长期解

一个很自然的思路是：既然 compiler 在看 `x28` 顶字节，那 runtime 能不能在 phase 切换时把所有 mutator 的 `x28` 也刷新掉？

长期看，这条路并不成立，原因有三层：

1. `x28` 是正在运行线程的寄存器态，不是普通共享内存
2. GC 线程无法远程原子修改另一个仍在执行托管代码的 mutator 当前寄存器值
3. 即便可以在 safepoint stub、N2C/C2N、异常恢复点等边界刷新 `x28`，也只能保证“下次过边界时变新”，不能保证“phase 切换瞬间立刻变新”

因此，只要 barrier fast path 仍然依赖 `x28` 顶字节，就永远会保留下面这个结构性风险：

- runtime 侧看到的是新 phase
- mutator 手里跑的是旧寄存器 cache
- compiler 却把旧 cache 当成 barrier 语义依据

这不是实现细节 bug，而是**状态载体选择错误**。

#### 6.4.3 长期正确语义：barrier fast path 只看 live TLS 的 `gcFastState`

长期正确版的核心原则是：

- **write barrier 是否允许 elide，应该由 live TLS 中的“屏障语义位”决定**
- 而不是由 phase 编号、也不是由 `x28` 顶字节缓存决定

runtime 里其实已经有这套语义化状态：

- `ThreadLocalData.gcFastState`
- `GC_FAST_STATE_MS_MARKING`
- `GC_FAST_STATE_MS_ASSIST`

它的含义比 `encodedGCPhase` 更贴近 barrier fast path 的真实需求：

- write barrier 关心的是“当前是否处于需要屏障的 marking 语义”
- 而不是“phase 数值是不是小于等于某个常量”

因此长期正确版建议把 barrier fast path 的判定统一改成：

```text
if (gcFastState & GC_FAST_STATE_MS_MARKING) != 0:
   必须走 runtime barrier helper
else:
   才允许走 direct store / memcpy / array copy fast path
```

这个判定有几个直接好处：

1. 语义与 runtime 对齐：需要 barrier 时一定落 slow path，不需要 barrier 时才允许 elide
2. 不再依赖 phase 编号的隐含约定，例如 `<= 8`、`idle == 1` 这类脆弱比较
3. 不再依赖 `x28` 顶字节 cache 的刷新时机
4. 后续即使 MSGC phase 枚举继续演进，也不需要同步修改 barrier fast path 的数值比较常量

#### 6.4.4 compiler 侧建议改造

长期正确版建议把 current `GetGCPhase` / `createCheck()` 这条链拆成两类用途，避免继续把“phase cache”混用到 barrier elision：

1. **barrier fast path**
  - 不再调用当前语义的 `GetGCPhase`
  - 改为直接读取 `ThreadLocalData.gcFastState`
  - 判定条件改成 `MS_MARKING` bit test

2. **保留给其他确实需要 phase byte 的路径**
  - 例如某些与 newobj/safepoint 相关、明确只依赖当前线程本地 cache 的场景
  - 这些路径可以继续保留 `encodedGCPhase`，但不能再把它当成 write barrier elision 的依据

对应到 compiler 改造点，建议至少包含：

- 在 llvm 侧新增 `GCFastStateOffsetInCJTLS` 常量，并与 runtime `ThreadLocalData` 对齐
- 在 runtime 侧补 `static_assert(offsetof(ThreadLocalData, gcFastState) == ...)`
- 在 `CJBarrierLowering.cpp` 中，把 write barrier / atomic barrier 的 `createGCCheck()` 与 `createCheck()` 改成生成“读 TLS fast-state + test marking bit”的 IR
- 在 AArch64 / X86 的特化发射路径中，优先把这条检查降低成 `ldrb + tst/and + branch` 一类的轻量序列

这样 barrier fast path 仍然是 fast path，只是它读的是**正确的 live state**，而不是 stale register cache。

#### 6.4.5 runtime 侧建议改造

runtime 这边不需要为了 barrier fast path 再引入新的复杂协议，但建议补齐三点约束：

1. `gcFastState` 作为 barrier fast path 的唯一语义源
  - 所有 mutator phase transition 都必须继续同步更新它
  - 不能出现“encodedGCPhase 已更新，但 gcFastState 还落后”的窗口

2. `encodedGCPhase` 保留，但降级为 phase byte cache
  - 可供需要 phase 数值的路径继续使用
  - 不再承担 write barrier 是否可 elide 的决策责任

3. 把 offset 同步做成显式 ABI 约束
  - 像 `encodedGCPhase`、`msAllocBuffer` 一样，为 `gcFastState` 增加 compile-time static_assert
  - 避免未来 `ThreadLocalData` 布局调整后，compiler 侧 silently 读错字段

#### 6.4.6 与 Step20 newborn 方案的关系

这条长期正确版不是替代 Step20 newborn 逻辑，而是给 Step20 提供一个**可靠的 barrier fast path 地基**。

两者的分工应该明确分开：

1. `gcFastState` 负责回答：
  - 这次写入能不能直接绕过 barrier

2. `NEWBORN_NEEDS_SCAN` 负责回答：
  - 如果这次写入已经进入 barrier 路径，holder 是否还需要做一次容器级补扫

所以长期正确版下的 barrier 路径应当是：

```text
先看 live TLS 的 gcFastState
  -> 非 marking：允许 direct store fast path
  -> marking：进入 barrier 路径
     -> 再判断 holder 是否带 NEWBORN_NEEDS_SCAN
     -> 命中则调用 newborn helper / enqueue known-black holder
     -> 同时保留 Step3 的 old/new ref discover 逻辑
```

这也意味着：

- Step20 的 newborn 语义仍然需要
- 但它应该建立在“barrier fast path 不会误把 marking 期写入放掉”这个前提之上
- 如果入口 gate 都错了，再完整的 newborn 逻辑也没有机会执行

#### 6.4.7 推荐落地顺序

如果后续要把这条长期正确版真正做成默认实现，建议按下面顺序推进：

1. 先修 barrier fast path 的状态来源
  - 从 `x28` 顶字节 / `encodedGCPhase` 比较
  - 改成 live TLS `gcFastState` bit test

2. 再恢复并验证 direct ref write barrier fast path
  - 确认 marking 期不会再误走 direct store

3. 在这个稳定地基上，再补 Step20 的 newborn-bit fast path 扩展
  - 命中 holder 时调用小型 runtime helper
  - 或按后续收益再决定是否进一步 inline

4. 最后才考虑更激进的 barrier-inline 优化
  - 例如减少 helper call、缩短 fast/slow path 跳转链
  - 但不再改变“live TLS fast-state 是唯一 gate”这条原则

这样可以把问题拆成两个正交层次：

- 第一层先修“会不会错误放行 barrier”
- 第二层再做“进入 barrier 以后 newborn 补扫怎么更便宜”

前者是 correctness root cause，后者才是 Step20 的性能增强。

---

## 7. 正确性论证

### 7.1 为什么新 region 可以安全参与本轮 sweep

因为在 `MarkTermination()` 执行 `PrepareForSweep()` 之前：

- 所有 mutator 已 STW 停止
- 所有 WBB / globalFull / deferred queue 已做最后一次 drain
- GC 期间新对象如果在本轮写屏障中命中过 holder，也已经通过 newborn-hit barrier 进入本轮队列

因此此时冻结 sweep snapshot，不会再遗漏本轮应处理的对象图。

### 7.2 为什么新对象不会被误回收

因为对象在分配时已经：

- 直接 `MarkObject(obj)`

所以即使对象所在 region 被纳入本轮 sweep，该对象本身也不会被当成白对象清掉。

### 7.3 为什么子对象不会漏扫

因为 Step20 新增两层保证：

1. 现有 Step3 混合写屏障仍然保留，对 `old/new ref` 继续 discover
2. newborn object 首次被 barrier 命中时，会把 holder 本身入队一次做容器级补扫

因此即便对象在构造期间通过多次字段写入、struct copy、array copy 才逐渐形成完整对象图，也能在本轮 mark 结束前被完整看见。

### 7.4 为什么需要“清标后只入队一次”

如果不清 `NEWBORN_NEEDS_SCAN`：

- 构造阶段对同一对象的每次字段写都会重复 enqueue holder
- 会显著放大 WBB / globalFull 压力

CAS 清标后只让第一次 barrier-hit 成功入队，可以把成本控制在“每个 newborn holder 最多一次补扫”。

---

## 8. 风险与边界

### 8.1 Step9 语义会被部分回退

Step9 的核心是假设：

- `PreEnumRoots()` 开始时就冻结 sweep snapshot

Step20 明确要反转这点，因此需要在 Step20 文档中注明：

- Step9 的“PreEnum 后归还 region 不参与本轮 sweep”不再成立
- 新语义变成“直到 MarkTermination STW 才冻结 sweep snapshot”

### 8.2 newborn bit 需要与 CopyGC 状态位兼容

`StateWord` 是运行时公共对象头结构，Step20 设计虽然主要服务 MSGC，但实现时要保证：

- CopyGC 仍然只看低 2 bit 状态码
- 不会把 newborn bit 误认为新的 forwarding 状态
- 所有 `IsForwardableState / IsLockedState / IsForwardedState` 都只基于低 2 bit 判断

### 8.3 直接入队已黑对象要避免破坏现有队列假设

当前队列里的对象大多是“刚从白转黑”的对象。Step20 会新增一种来源：

- “已经黑，但需要补扫的 newborn holder”

因此需要核对所有 drain 逻辑，保证它们不会假设“入队对象必然是首次 mark 成功者”。

---

## 9. 分阶段落地建议

### Phase 1：先改时序与 runtime slow path

1. `PrepareForSweep()` 从 `PreEnumRoots()` 挪到 `MarkTermination()`
2. `MarkNewObject()` 增加 newborn bit 置位
3. runtime barrier 增加 `MaybeEnqueueNewbornContainer(holder)`
4. 新增 `EnqueueKnownBlackObjectForRescan()`

先只走 runtime slow path，验证语义闭环。

### Phase 2：补 LLVM NewObject fast path

1. inline mark bitmap
2. inline newborn bit set

先确保 fast allocation 不掉语义。

### Phase 3：补 LLVM barrier fast path

1. inline test newborn bit
2. 命中时调用 runtime helper

把性能关键路径补齐。

---

## 10. 验证建议

### 10.1 功能验证

需要新增/打开以下统计：

- `newbornMarkedObjects`
- `newbornBarrierHits`
- `newbornRescanQueued`
- `newbornRescanProcessed`
- `gcPeriodNewRegionCount`
- `currentCycleSweptGcNewRegionCount`

预期：

- GC 期间新 region 数量大于 0
- 本轮 sweep 处理到其中一部分甚至全部
- newborn 入队数接近“含引用 newborn 对象数”而不是字段写次数

### 10.2 反汇编验证

对编译器产物做 `--save-temps` 与反汇编确认：

- NewObject fast path 中存在 newborn bit set
- barrier fast path 中存在 newborn bit test
- 命中后 call 到 runtime helper

### 10.3 性能验证

重点看以下 case：

- case24 这类 GC 期仍有强分配压力的 case
- 大量短命对象、但分配跨越 concurrent mark 的 case
- 大量 constructor / struct copy / array copy 的 case

重点指标：

- RSS peak
- GC 后 hotFree / emptySwept 供给
- 当前轮 sweep 回收字节数
- barrier queue 流量
- wall time

---

## 11. 结论

Step20 的本质是把 MSGC 从“PreEnum 冻结 sweep snapshot”切换到“MarkTermination STW 冻结最终 snapshot”。

这样 GC 期间新产生的 region 就能参与本轮 sweep，从而更早进入 hotFree / emptySwept / scavenger 链路，缓解 RSS 峰值。

为了保持正确性，必须同时补齐两件事：

1. **mark-at-alloc**：保证 GC 期间新对象本身不会被回收
2. **newborn-needs-scan + barrier hit enqueue**：保证这些新对象的子对象最终能被 trace 到

这两部分都需要同时覆盖：

- runtime slow path
- LLVM NewObject fast path
- LLVM barrier fast path

如果只做 generation flip 后移，而不补 newborn 标记与 barrier 入队，正确性是不闭合的。

如果只做 mark-at-alloc，而不让 GC 期新 region 进入本轮 sweep，则 RSS 问题也不会真正解决。

因此 Step20 应作为 Step3、Step9、Step10 之后的一次联合语义升级来实现。