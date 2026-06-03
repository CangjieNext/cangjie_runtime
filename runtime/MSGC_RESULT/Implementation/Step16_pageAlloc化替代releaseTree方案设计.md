# Step16 pageAlloc 化替代 releaseTree 方案设计

> 本文档给出一个比 Step15 更进一步的方向：不再继续围绕 releasedTree 做 producer/consumer/tree-shard 演进，而是引入 Go `pageAlloc` 风格的页堆，把“free span 管理”和“归还 OS 的 scavenged span 管理”统一到一个 bitmap + summary 的分配器里。

---

## 1. 目标与边界

本文目标不是“把 Go runtime 的 mheap 原样搬过来”，而是把其中最有价值的那一层移植到当前 MSGC：

1. 用 **pageAlloc 风格页堆** 替代当前 `FreeManager` 里的 releasedTree backend。
2. 把“可再分配的 free span”和“已归还 OS 的 scavenged span”统一管理。
3. 让 `TakeRegions` 不再依赖 CartesianTree 的节点遍历、旋转、publish merge。
4. 为后续的 background scavenger、tail shrink、局部缓存打下统一底座。

本文明确**不**包含下面这些内容：

1. 不改 `CentralRegionPool` 的四链表 + lazy sweep 主体逻辑。
2. 不改 small object 在 steady-state 下优先吃 `partialSwept` / `partialUnswept` 的热路径。
3. 不要求第一阶段就做成 Go 那样完整的 `mcache + mcentral + mheap + pageAlloc` 全套复制。

换句话说，Step16 的合理目标是：

1. 保留当前 MSGC 的 `CentralRegionPool + MSRegionManager` 上层行为。
2. 只把 `FreeManager` 下面的 released/free 空间管理层替换成 pageAlloc 式页堆。

---

## 2. 当前实现的问题归纳

### 2.1 当前 FreeManager 的真实形态

当前代码已经不是 Step14 最早分析时的“单树单锁”，而是：

1. `producerBank_` 接收 `ReclaimRegionsReleased()` 的新释放 span。
2. `consumerBank_` 供 `TryTakeRegions()` 取用。
3. `GrowOneSegment()` 或 consumer miss 时，会把 producer 发布到 consumer。

但当前实现仍有几个结构性问题：

1. consumer 侧仍是**单 bank 单锁**。
2. publish 不是 O(1) 指针交换，而是 `CollectUnitSpans -> 逐段重新插入 consumer`。
3. `ReclaimRegionsReleased()` 先 `ReleaseUnits()` 再入树，导致 released pool 里的 span 天然是冷页。
4. `ShrinkHeap()` 仍依赖 CartesianTree 的“最右节点刚好顶到 tail”语义。

### 2.2 为什么 Step15 继续演进不是最优路径

Step15 解决的是“单棵树不好用，所以拆树”。

这条线能继续走，但有两个根本限制：

1. **树结构本身仍然保留**。
	`TakeUnits` 仍然要做遍历、更新节点、下沉、删除或合并。
2. **free 与 scavenged 仍没有解耦**。
	当前 releasedTree 本身就代表“已 release 给 OS 的 span 池”，不是一个普通 free heap。

这意味着即使 Step15 完整落地：

1. tree 操作成本还在；
2. 冷页复用成本还在；
3. grow/publish 的批量搬运成本还在。

所以从长期看，更合理的方向不是“把 tree 做得更复杂”，而是直接切到底层模型：

1. free 页通过 bitmap 管；
2. scavenged 页通过独立 bitmap 管；
3. 用 summary 快速找连续空闲区间；
4. scavenger 独立决定何时把 free 页归还 OS。

---

## 3. 方案总览

### 3.1 核心结论

建议在 `FreeManager` 内引入一个新的 `MSPageHeap`，最终替代：

1. `Backend`
2. `producerBank_`
3. `consumerBank_`
4. `PublishProducerToConsumers()`
5. `CollectUnitSpans()/DistributeFrozenSpansToConsumers()` 这一整条发布链

新的目标形态如下：

1. **一个统一页堆** 负责管理 committed heap 内所有 free range。
2. free range 只是 free，不再默认等于“已 release 给 OS”。
3. scavenged 只是 free range 的一个子状态，由另一个 bitmap 记录。
4. allocation 直接从页堆找连续 run；free 直接回页堆；scavenge 由后台或同步路径单独做。
5. 保留当前 `empty region/empty-cache` 作为 region 层与页堆之间的缓冲层，由后台 `BackgroundReclaim` 统一负责“归还 empty region 到页堆”以及后续 scavenging。

### 3.2 与 Go pageAlloc 的对应关系

建议借鉴 Go 的四个关键思想，而不是逐行翻译实现：

1. **位图是唯一真实来源**：某页是否 allocated / free，由 bitmap 决定。
2. **summary 是索引**：每层 summary 记录一段地址空间的 start/max/end free-run。
3. **search cursor**：优先从低地址开始分配，把高地址更容易留给 shrink/scavenge。
4. **free 与 scavenged 分离**：free page 可以是 hot free，也可以是 scavenged free；是否归还 OS 不再影响“它是不是 free”。

### 3.3 建议的最终分层

建议把当前堆管理分成下面三层：

1. `CentralRegionPool`
	继续负责小对象 region 的 swept/unswept 生命周期。
2. `MSRegionManager`
	继续负责 region 级编排、grow/shrink 协调和地址到 `MSRegionInfo` 的映射。
3. `MSPageHeap`
	负责 committed heap 内 free/scavenged range 的统一管理。

其中 `MSPageHeap` 成为新的“底层 free-space allocator”，相当于 Go runtime 中 `mheap.pages` 的角色。

### 3.4 MSPageHeap 的职责边界

这里需要明确区分两种“地址管理”，避免把 Step16 误读成“所有分配状态都塞进页堆”。

建议的边界是：

1. `MSPageHeap` 统一管理 **committed heap 的全局地址状态**。
2. `CentralRegionPool/MSRegionInfo` 继续管理 **单个 region 内部的小对象分配状态**。

更具体地说，下面这些职责应该统一进 `MSPageHeap`：

1. 哪些 unit/page 当前是 allocated。
2. 哪些 unit/page 当前是 free-hot。
3. 哪些 unit/page 当前是 free-scavenged。
4. 全局连续 free range 的查找。
5. grow 后新 committed 区间接入 free space。
6. shrink 时尾部 segment 是否可整体回收。

而下面这些职责**不应该**进 `MSPageHeap`：

1. `MSRegionInfo::allocPtr` 在 region 内的推进。
2. region 内 bitmap / mark / sweep 生命周期。
3. `CentralRegionPool` 的 partial/full swept/unswept 四链表。
4. size-class 级的 region 复用策略。

也就是说：

1. **全局 bump pointer 语义** 最终可以被 `MSPageHeap` 吸收。
2. **region 内 allocPtr 前移** 仍然保留在 `MSRegionInfo/CentralRegionPool`。

因此 Step16 的最终目标不是“页堆接管所有分配行为”，而是：

1. 页堆接管“整个 committed heap 上哪些地址可分配”。
2. region allocator 接管“拿到一个 region 后，内部对象怎么继续分配”。

进一步说，Step16 还建议把当前“普通 fresh region / 后台 feed fresh region”这两条分裂路径统一掉：

1. 后台 feed 不再保留 `TakeRegionForFeed()` 这种单独入口。
2. 普通小对象与后台 feed 都先走同一个 `CentralRegionPool::GetRegion(cls)`。
3. 只有在中央池 miss 时，二者才共同进入同一个 fresh-region 获取入口。
4. 大对象仍然不走中央池，但底层连续地址分配原语与小对象 fresh-region 路径共用同一个 `MSPageHeap::Alloc()`。

这也是它和 Go 的对应关系：

1. `MSPageHeap` 更接近 Go 的 `mheap.pages/pageAlloc`。
2. `CentralRegionPool` 更接近 Go 的 `mcentral`。
3. `MSRegionInfo` 内的对象 bump/slot 管理，更接近 span 内部对象分配逻辑，而不是 page heap 本身。

---

## 4. 核心数据结构

### 4.1 建议的数据结构

建议新引入：

```cpp
struct MSPageHeap {
	 std::mutex lock;

	 // 已提交地址空间范围。
	 uintptr_t heapStart;
	 uintptr_t committedEnd;
	 uintptr_t maxHeapEnd;

	 // 分配粒度仍以 Unit 为底层单位，但外部 API 保持 region 对齐。
	 size_t unitSize;
	 size_t unitsPerRegion;

	 // 全局位图块索引。
	 std::vector<PageChunk*> chunks;

	 // 多层 summary。每层记录 start/max/end contiguous free units。
	 std::vector<SummaryLevel> summaryLevels;

	 // 低地址优先搜索起点。
	 UnitIndex searchIdx;

	 // 供 scavenger 快速找到“可 release 的 free 区域”。
	 ScavengeIndex scavIndex;
};

struct PageChunk {
	 Bitset allocBits;      // 1 = in use, 0 = free
	 Bitset scavengedBits;  // 1 = free but already released to OS
};
```

### 4.2 粒度建议

推荐底层仍以 `Unit` 为 bitmap 粒度，而不是直接退回 region 粒度。

原因：

1. `FreeManager` 本来就保存 `unitSize_` 和 `unitsPerRegion_`。
2. 当前 API 虽然基本是 region 对齐，但 future-proof 更好。
3. shrink/scavenge 天然按页或多页单位工作，更接近 OS 语义。

同时，**对外接口仍保持 region 对齐**：

1. `TakeRegions(num)` 仍然返回 region 对齐的范围。
2. `ReclaimRegionsReleased(idx, num)` 仍然按 region 归还。
3. `TakeRegionForLarge()` 仍把大对象 round-up 成 region 数。

也就是说：

1. 外部还是 region allocator；
2. 内部页堆是 unit/page allocator。

### 4.3 summary 设计

建议直接采用 Go pageAlloc 那套 summary 语义：

1. `start`：这一段开头连续 free 单位数。
2. `max`：这一段内部最大连续 free 单位数。
3. `end`：这一段结尾连续 free 单位数。

这样有三个直接收益：

1. allocation 查找可以跳过大量不可能满足请求的区间。
2. free 后的 summary 更新是局部的，不依赖树节点重排。
3. tail shrink 很容易判断“高地址尾部是否整段 free”。

---

## 5. 分配与回收路径

### 5.1 allocation 路径

建议最终把 `TakeRegions` 改成下面这个模型：

1. 先向 `MSPageHeap::Alloc(unitCount)` 请求连续 range。
2. 找到则直接返回。
3. 找不到则 `GrowOneSegment()`。
4. grow 成功后调用 `MSPageHeap::Grow(base, size)`，把新 committed 区间作为 free range 加入页堆。
5. 再次重试 allocation。

这样做以后：

1. `TryIncreaseInactiveZone()` 不再是主路径。
2. 当前全局 `inactiveZone` bump pointer 的语义退化成“低地址优先 first-fit 的特殊情形”。
3. 整个 committed heap 内的 free range 都由一个统一分配器管理。

在这个底座之上，建议把上层获取路径也统一成两类，而不是现在三套入口：

1. **小对象/后台 feed**：先查 `CentralRegionPool::GetRegion(cls)`。
2. **中央池 miss 后的小对象/后台 feed**：走同一个 `GetFreshRegion(cls)`。
3. **大对象**：直接按 `regionCount > 1` 走 `MSPageHeap::Alloc()`，不经过中央池。

也就是说：

1. 小对象和后台 feed 的差别，只应该体现在“谁触发补货、谁消费缓存”，不应该体现在“底层拿 fresh region 走两条不同的 `TakeRegion*` 路径”。
2. 大对象由于不属于 size-class region cache，仍保留独立的上层入口，但其底层连续地址获取不再特殊化。

这里再强调一次：

1. 被替代的是 **全局堆尾部 bump**。
2. 不是 `MSRegionInfo` 内部的小对象 `allocPtr`。

所以 `TakeRegions()` 统一走 `MSPageHeap::Alloc()` 之后，返回值仍然只是“给上层一个 region 范围”；
真正把对象放进某个 region 里，仍然由 `CentralRegionPool` 和 `MSRegionInfo` 继续完成。

### 5.1.1 建议的接口收敛

结合当前代码，建议把下面这三条路径收敛：

1. `MSRegionManager::TakeRegion()`
2. `MSRegionManager::TakeRegionForFeed()`
3. `MSRegionManager::TakeRegionForLarge()`

收敛方式不是把它们都变成一个完全相同的公开 API，而是：

1. 删除 `TakeRegionForFeed()` 这种“只因调用来源不同而存在”的入口。
2. 保留一个统一的小对象 fresh-region 入口，例如 `TakeRegion()` 或 `TakeFreshRegion()`。
3. 保留一个大对象入口，例如 `TakeLargeRegionRange()`，因为它天然需要 `regionCount > 1` 和 large-object 特有统计/回收策略。
4. 在更底层，只保留一个统一原语：`MSPageHeap::Alloc(unitCount)`。

推荐的最终调用链：

1. 普通小对象：`CentralRegionPool::GetRegion(cls)` -> miss -> `GetFreshRegion(cls)` -> `MSRegionManager::TakeRegion()` -> `MSPageHeap::Alloc(1 region)`。
2. 后台 feed：`CentralRegionPool::GetRegion(cls)` -> miss -> `GetFreshRegion(cls)` -> `MSRegionManager::TakeRegion()` -> `MSPageHeap::Alloc(1 region)`。
3. 大对象：`MSRegionManager::TakeLargeRegionRange(regionCount)` -> `MSPageHeap::Alloc(regionCount)`。

这样做的核心收益是：

1. 后台 feed 和普通小对象不再因为来源不同而分裂 fresh-region 路径。
2. 中央池成为小对象 region 的唯一第一入口。
3. page heap 成为所有连续地址范围的唯一底层入口。

### 5.2 free 路径

建议把当前 `ReclaimRegionsReleased()` 拆成两步：

1. `MSPageHeap::Free(range)`
	只把该 range 重新标成 free，更新 alloc bitmap 与 summary。
2. `MSPageHeap::MaybeScavenge(range or budget)`
	根据策略决定是否、何时把这段 free range release 给 OS。

这和当前实现最大的不同是：

1. **free 不再等于 release-to-OS**。
2. 刚释放出来的 region 先作为 hot free 留在页堆里。
3. scavenger 再把真正长期不用的高地址 free 页转换成 scavenged。

这里建议**保留**当前的 `empty region/empty-cache` 层，但改变它的职责：

1. region 在 sweep 后变成 completely free 时，先进入 emptyRegion/empty-cache。
2. 普通小对象与后台 feed 仍然可以优先复用这批 emptyRegion。
3. `BackgroundReclaim` 在后台异步把积压的 emptyRegion 归还到 `MSPageHeap::Free(region range)`。
4. 归还进页堆后，再由 scavenging 逻辑决定是否继续 `SysUnused` 归还 OS。

这样做的含义是：

1. `CentralRegionPool` 除了 partial/full region 以外，仍然保留 emptyRegion 这一层作为热复用缓冲。
2. completely free region 不再直接碰 OS，而是先进入 emptyRegion。
3. “何时把 emptyRegion 交还页堆”与“何时进一步归还 OS”统一交给后台 `BackgroundReclaim` 决定。

### 5.3 scavenged 页的再次使用

页堆分配命中 scavenged range 时：

1. allocBits 置 1；
2. scavengedBits 清 0；
3. 统计上记为“从 scavenged free 复用”；
4. 真正页面恢复由平台层保证按需 fault-in，必要时补 `sysUsed` 类处理。

关键点在于：

1. 页面是否 scavenged 只影响“代价”和“统计”；
2. 不影响它参与统一 free-space 分配。

这也意味着当前 `BackgroundReclaim` 的语义应调整但不必删除：

1. 第一阶段仍负责“把 emptyRegion 从中央池归还到页堆”。
2. 第二阶段继续在页堆的 free-hot 范围上工作。
3. 它的职责扩展为两段：
	- drain emptyRegion -> `MSPageHeap::Free(region range)`
	- scavenge free-hot -> free-scavenged -> `SysUnused(range)`

### 5.4 grow 路径

现在的 grow 是：

1. 扩 committedEnd；
2. 初始化 `MSRegionInfo`；
3. bump pointer 可以继续向后长。

改成 page heap 以后应变成：

1. 扩 committedEnd；
2. 初始化新增 region 的 `MSRegionInfo`；
3. 调 `MSPageHeap::Grow(oldCommittedEnd, growBytes)`；
4. 新增区间作为 free range 暴露给统一页堆。

这时不再需要“producer 发布给 consumer”的概念。

---

## 6. 与现有模块的职责划分

### 6.1 MSRegionManager

建议保留：

1. 小对象 fresh-region 与大对象 range 分配的调用编排。
2. `MSRegionInfo` 初始化与地址转换。
3. grow/shrink 对上层 GC 和 Heap 的通知。
4. region allocator 与 page heap 之间的桥接。

建议删除或收敛：

1. `TakeRegionForFeed()` 这种仅因调用来源不同而存在的入口。
2. “普通小对象 fresh region”和“后台 feed fresh region”两套几乎相同的初始化代码。

建议下沉给 `MSPageHeap`：

1. released/free range 查找。
2. free range 合并。
3. free/scavenged 状态转换。
4. trailing free tail 查找。
5. 全局 bump/inactiveZone 语义。

### 6.2 CentralRegionPool

建议保持不变：

1. partial/full swept/unswept 四链表。
2. lazy sweep 的获取优先级。
3. region 内部对象分配与 `allocPtr` 推进。
4. emptyRegion/empty-cache 作为 completely free region 的暂存层。

建议增强：

1. 后台 feed 与普通路径统一先走 `GetRegion(cls)`。
2. 只有 `GetRegion(cls)` miss 时，才进入统一的 `GetFreshRegion(cls)`。
3. 删除 `GetFreshRegionForFeed()` 这种只因调用方不同而存在的包装层。
4. emptyRegion 继续保留，但其回收出口统一变成 `BackgroundReclaim -> MSPageHeap::Free()`。

只是 `ReclaimEmptyRegionToReleased()` 应进一步收敛为普通 free：

1. `region->Reset()`；
2. 先进入 emptyRegion/empty-cache；
3. `BackgroundReclaim` 异步把 emptyRegion 归还到 `MSPageHeap::Free(region range)`；
4. 之后再决定是否把该 free-hot range 继续转成 free-scavenged 并 release 给 OS。

换句话说，Step16 之后，中央池仍然保留 emptyRegion 的短期归属权，但 emptyRegion 的最终去向由 `BackgroundReclaim + MSPageHeap` 统一决定。

### 6.3 FreeManager

建议重构为更薄的一层协调器，或者直接更名为：

1. `MSPageHeapManager`
2. 或 `MSPageHeap`

当前 `Backend` 抽象届时可以移除，因为目标不再是“可替换的树 backend”，而是“固定的 page heap 元数据层”。

---

## 7. 并发策略

### 7.1 第一阶段建议

第一阶段**不追求一步到位复制 Go 的 per-P pageCache**。

理由：

1. 当前 MSGC 的小对象热路径主要在 `CentralRegionPool`，并不频繁触达 `FreeManager`。
2. 先把 tree/publish 冷热点拿掉，收益就已经很明显。
3. bitmap + summary 的统一结构先跑通，比一开始就加本地缓存更稳妥。

因此第一阶段建议：

1. `MSPageHeap` 只有一把全局锁；
2. 查找和更新都在这把锁下完成；
3. 但查找复杂度从“树遍历 + 旋转 + 合并”变成“summary 跳转 + chunk 位图搜索”；
4. 不再需要 `BUSY -> saferegion retry` 这种行为模式。

### 7.2 第二阶段建议

在 page heap 跑稳后，再考虑加入 Go 风格的局部缓存：

1. 对 `TakeRegionForFeed` 和大对象小范围请求，加入 per-processor page cache。
2. cache 只缓存小块连续 free units。
3. cache 用尽时从 `MSPageHeap` 批量 refill。

这一步相当于把 Go `pageCache` 的思想嫁接到 MSGC，但应该作为第二阶段优化，而不是第一阶段阻塞项。

### 7.3 第三阶段建议

如果页堆锁仍然成为新热点，再做 shard：

1. 按地址范围把 committed heap 切成多个 shard。
2. 每个 shard 有独立 chunk bitmap + summary 根。
3. `searchIdx` 先选候选 shard，再进 shard 内分配。

这比“继续把 CartesianTree 分片”更自然，因为 summary/bitmap 天然适合按地址段拆分。

---

## 8. Shrink 与归还 OS

### 8.1 shrink 方案

当前 shrink 依赖“releasedTree 最右节点刚好顶住 inactiveZone tail”。

改成 page heap 以后，建议改成：

1. 从 `committedEnd` 对齐到 segment 边界向下扫描；
2. 检查这些 segment 覆盖的 units 是否全部 free；
3. 若整段 free，则直接下调 committedEnd；
4. 直到遇到第一个非全空 segment 为止。

这比树上拿 rightmost node 更稳定，因为：

1. 逻辑只依赖 bitmap 真相；
2. 不依赖某个节点是否刚好合并成尾段；
3. 与 summary/bitmap 天然一致。

### 8.2 scavenger 方案

建议单独引入 `MSScavenger`：

1. 后台慢速扫高地址 free range；
2. 优先 release 高地址、长时间未复用的 free pages；
3. 对 memory pressure 或 grow 后超额 RSS，允许同步 scavenge assist。

这一步等价于把 Go 里“free”和“return memory to OS”拆成两条路径。

同时，建议保留当前 `BackgroundReclaim` 线程名或职责入口，但把内部工作拆成两段：

1. 第一段：从中央池 drain emptyRegion，并归还到 `MSPageHeap`。
2. 第二段：从 `MSPageHeap` 中挑选 free-hot 区间，调用 `SysUnused` 归还 OS。
3. 也就是说，`BackgroundReclaim` 变成“empty-region reclaim + scavenging”的统一后台线程。

### 8.3 平台语义建议

建议把当前 `ReleaseUnits()` 的平台处理迁移成：

1. `SysUnused(range)`：把 free range release 给 OS。
2. `SysUsed(range)`：仅在平台确有需要时恢复 accounting。
3. `scavengedBits`：记录哪些 free range 已走过 `SysUnused`。

这样 `MSPageHeap` 对外只表达统一状态机：

1. allocated
2. free-hot
3. free-scavenged

---

## 9. 一次性整体落地方案与优化项

既然这里不打算分阶段过渡，那么 Step16 的实现目标就不应再是“先保留旧壳子，再慢慢替换内部细节”，而应定义为一次性把当前 releasedTree 模型切换到统一页堆模型，并把配套优化项一起收口。

这里的“一次性落地”不是说所有代码在一个大函数里重写，而是说**同一轮方案设计与代码改造必须同时完成下面这些结构变化**，不再允许保留一半 tree 语义、一半 page heap 语义的中间态。

### 9.1 一次性落地的核心改造范围

建议把下面这些内容视为一个不可拆开的整体：

1. 引入 `MSPageHeap`，作为 committed heap 内唯一的全局 free/scavenged range 管理器。
2. 删除 `FreeManager` 对 `Backend/producerBank_/consumerBank_` 的依赖，不再保留发布树、冻结树、consumer tree 这些中间结构。
3. 把 `TryTakeRegions()`、`GrowOneSegment()`、`ShrinkHeap()` 的底层地址管理统一改成 `MSPageHeap::Alloc/Free/Grow/ShrinkTailCandidate` 一套原语。
4. 移除 `TryIncreaseInactiveZone()` 以及它所代表的“全局 bump 区”视角，让 grow 后的新空间直接进入页堆 free-hot。
5. 把 `ReclaimRegionsReleased()` 改造成普通 free 语义，不再在主 free 路径里直接 `ReleaseUnits()`。
6. 保留 emptyRegion/empty-cache，但把它的最终出口统一成 `BackgroundReclaim -> MSPageHeap::Free() -> scavenging`。
7. 收敛 `TakeRegionForFeed()`、`GetFreshRegionForFeed()` 这类仅因调用来源不同而存在的入口，让普通路径与后台 feed 共用 fresh-region 获取路径。

换句话说，这次改造的完成态应该直接是：

1. 小对象仍先经 `CentralRegionPool`。
2. fresh region 统一从 `MSRegionManager -> MSPageHeap` 获取。
3. completely free region 先进入 emptyRegion，再由 `BackgroundReclaim` 异步归还页堆。
4. free-hot 与 free-scavenged 由 `MSPageHeap` 统一表达。
5. shrink 与 scavenging 都只依赖 bitmap/summary 真相，而不再依赖 CartesianTree 节点形态。

### 9.2 必须同时改完的接口与语义

如果决定一次性落地，那么下面这些接口语义也不应该保留旧名字掩盖旧模型，而应该一起收敛：

1. `MSRegionManager::TakeRegionForFeed()` 删除，后台 feed 与普通小对象统一走 `TakeRegion()` 或新的 `TakeFreshRegion()`。
2. `MSRegionManager::TakeRegionForLarge()` 可以保留大对象独立入口，但底层必须统一改成 `MSPageHeap::Alloc(regionCount)`。
3. `FreeManager::TryTakeRegions()` 的实现不再访问 CartesianTree，而是直接调用 `MSPageHeap::Alloc()`。
4. `FreeManager::ReclaimRegionsReleased()` 应重命名或语义重定义为普通 free 操作，例如 `FreeRegions()`，至少内部逻辑必须先 `MSPageHeap::Free()`，再由后台决定是否 scavenging。
5. `CentralRegionPool::GetFreshRegionForFeed()` 这类包装层应删除，避免“同一种 fresh region 获取逻辑维护两份”。
6. `ReclaimEmptyRegionToReleased()` 的语义应同步调整为“emptyRegion 入队”，不再暗示它会直接流向 releasedTree。

一次性改完这些接口的价值在于：

1. 调用栈能准确反映新模型，而不是表面 page heap、实际内部仍残留 releasedTree 心智模型。
2. 后续调试统计更清楚，不会出现“名字叫 released，实际只是 free-hot”的语义混乱。

### 9.3 必须同步落地的优化项

这次既然不做阶段式过渡，建议把下列优化项与页堆主体一起纳入同一轮实现，而不是留作“以后再补”：

1. **summary + bitmap 搜索链路**：不能只落 allocBits，而不落 summary；否则只是在用位图重写线性扫描。
2. **searchIdx 低地址优先策略**：必须和 `Alloc()` 一起落地，否则 shrink 保持高地址空洞的收益拿不到。
3. **scavengedBits 与 free-hot 分离**：必须在第一版就建好状态机，否则 free 与 release-to-OS 仍会重新耦合。
4. **BackgroundReclaim 双阶段循环**：同一轮里就要改成“drain emptyRegion -> page heap free，再按 budget/scav index 做 scavenging”。
5. **tail shrink bitmap 判定**：同一轮里就改成按 chunk/summary 判断尾段是否全 free，不能继续依赖 rightmost tail tree 语义。
6. **统一统计口径**：至少同步切成 allocated bytes、free-hot bytes、free-scavenged bytes、emptyRegion bytes 几类，否则新旧模型无法对比验证。

如果这些优化项不和主体一起落地，会出现两个问题：

1. 架构上已经切到页堆，但性能和可观测性仍停留在旧模型。
2. 代码里会留下大量“暂时兼容旧语义”的分支，后面反而更难清理。

### 9.4 一次性落地后的职责稳定形态

一次性改造完成后，建议直接把职责固定成下面这个长期形态：

1. `CentralRegionPool`
	负责 size-class region 的热路径、lazy sweep、partial/full/empty 三类缓存管理。
2. `MSRegionManager`
	负责 region 级编排、地址到 `MSRegionInfo` 的映射、grow/shrink 协调，以及上层到页堆的桥接。
3. `MSPageHeap`
	负责 committed heap 上的 alloc/free/scavenged 真相、连续 free range 查找、grow 后纳管、tail shrink 判定。
4. `BackgroundReclaim`
	负责 emptyRegion 回页堆和页堆 scavenging 两段后台工作，而不是 releasedTree 的搬运线程。

这里要特别强调：

1. `MSPageHeap` 接管的是“全局地址范围怎么分配/回收”。
2. `MSRegionInfo::allocPtr` 接管的仍然是“region 内对象怎么发出去”。
3. emptyRegion 仍是 `CentralRegionPool` 的短期缓存，不是页堆直接管理的小对象热路径结构。

### 9.5 为什么不建议保留中间态

这次如果已经明确不走分阶段路线，那么最需要避免的就是“名义上整体替换，实际上留下半套旧模型”。

不建议保留中间态的原因是：

1. 如果页堆已经接管 alloc/free，但 `inactiveZone` 还保留，全局地址视角就会分裂。
2. 如果 free-hot/free-scavenged 还没拆开，旧的 `ReleaseUnits()` 语义会继续污染分配路径。
3. 如果后台 feed 还保留独立 fresh-region 路径，中央池与页堆之间仍会有两套补货逻辑。
4. 如果 shrink 还依赖 tree tail，新的 bitmap/summary 就无法成为唯一真相来源。
5. 如果统计口径还沿用 releasedTree 时代的数据，新方案上线后也很难判断收益来自哪一层。

因此，“一次性落地”真正要表达的是：

1. 同一轮把模型边界切干净。
2. 同一轮把关键优化项一起并入。
3. 同一轮把接口、统计和后台线程职责同步改到位。

---

## 10. 预期收益与风险

### 10.1 预期收益

1. 去掉 `CollectUnitSpans -> DistributeFrozenSpansToConsumers` 这条发布链。
2. 去掉 tree 节点遍历、旋转、合并、rightmost-tail 特殊逻辑。
3. free 与 scavenged 分离后，能独立分析“碎片问题”和“冷页复用问题”。
4. 低地址优先分配有利于把高地址留给 shrink/scavenge。
5. 后续加 page cache 或 shard 的基础更统一。

### 10.2 主要风险

1. summary/bitmap 实现正确性要求很高，尤其是 merge 和跨 chunk 更新。
2. 当前 512GB 虚拟保留空间下，元数据设计必须是稀疏映射，不能直接平铺大数组。
3. 需要重新定义统计项，原有 releasedTree 诊断口径会失效。
4. 部分现有启发式是基于“released bytes”写的，需要迁移到“free bytes / scavenged bytes / hot free bytes”三套指标。

### 10.3 最终建议

如果目标是长期替代 releaseTree，推荐的主线不是继续做 Step15 的“多树版本”，也不是再拆成多个过渡 phase，而是直接按 Step16 的最终模型一次性完成整体替换：

1. 以 `MSPageHeap` 统一接管 committed heap 的 free/scavenged 状态。
2. 同时收敛 fresh-region 路径、emptyRegion 回收路径、BackgroundReclaim 职责与 shrink/scavenge 判定逻辑。
3. 同时把 summary/bitmap、searchIdx、scavengedBits、tail shrink 判定、统计口径这批关键优化项一起落地。

这样做的好处是：

1. 先把模型边界一次切干净，避免长期维护过渡态。
2. 不会在 CartesianTree 这条技术路线上继续投入过多复杂度。
3. 最终得到的是一个更接近 Go `pageAlloc` 的统一页堆，而不是一个越来越复杂的 releasedTree 家族。
