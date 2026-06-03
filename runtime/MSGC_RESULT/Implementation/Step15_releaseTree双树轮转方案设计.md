# Step15 releaseTree 双树轮转方案设计

> 本文档记录基于堆增长 epoch 的 releaseTree 双树轮转方案，包括方案理解、可行性判断、改进点与落地建议。

---

## 1. 目标与背景

当前实现里，releaseTree 本质上是 FreeManager 内部的一棵全局 backend tree：

1. mutator 侧的 TakeRegions 会先尝试从这棵树取 span；失败再走 bump / grow。
2. reclaim 侧会把释放后的 region/span 插回同一棵树。
3. take 与 insert 共用同一把锁和同一份 treap / cartesian tree 结构。

从当前代码看，这套单树模型有三个天然问题：

1. producer 和 consumer 共用一棵树，容易形成锁竞争。
2. 新释放出来的 span 会立刻进入 take 热路径，mutator 很容易撞上冷页与树操作成本。
3. take 与 reclaim 的流量混在一起，难以按“堆增长阶段”做分层管理。

你提出的方案，本质上是在 releaseTree 上引入“epoch 轮转 + 生产消费分离”的 bank 设计：

1. 当前增长 epoch 内，新释放的 span 只进入“生产树”。
2. mutator 只从“消费树”拿 span。
3. 当堆再次增长时，旧 producer 被发布给 consumer，新 epoch 切一个新的 producer。

这个方向的核心价值不是“多一棵树”本身，而是把 reclaim 写入路径和 mutator 取用路径拆开。

## 2. 方案理解

按当前代码语义，我把你的方案理解成下面这组规则。

### 2.1 两棵树的角色

1. consumerTree：只负责给 TakeRegions 提供 released span。
2. producerTree：只负责接收当前 epoch 内新增的 released span。

注意这里的“producer”不是只指 BackgroundReclaim。
当前实现中，至少下面几类路径都会把 span 放回 released pool：

1. ReclaimRegion
2. ReclaimRegionLarge
3. ReclaimEmptyRegionToReleased
4. InlineReclaimForBalance

所以 Step15 里应把 producer 理解为“本 epoch 所有 released span 的写入入口”，而不是“只给 BackgroundReclaim 用的专属树”。

### 2.2 epoch 的含义

这里的 epoch 不是 GC phase，而是“堆增长轮次”：

1. 初始 committed heap 对应 epoch0。
2. 每次 GrowOneSegment 成功，epoch 加 1。
3. producerTree 在 epoch 切换时轮转。

### 2.3 取用策略

按你原始描述，第一次 grow 以后，TakeRegions 的顺序是：

1. 先从 consumerTree 取。
2. consumerTree 没有空间，再走 bump pointer。
3. bump 也不够，再 grow。

这是一个“优先回收旧 epoch released span，最新 epoch 仍允许 bump 扩展”的策略。

## 3. 运行状态机

可以把整个机制写成一个很明确的状态机。

### 3.1 初始化阶段

1. 创建 producerTree(epoch0)。
2. consumerTree 为空。
3. TakeRegions 不从 producerTree 取，只走 bump。
4. 所有 released span 都进入 producerTree(epoch0)。

这一步的含义是：初始堆空间先尽量线性 bump，producer 只积累“本轮释放回来的冷 span”。

### 3.2 第一次 grow 后

1. GrowOneSegment 成功，进入 epoch1。
2. 原 producerTree(epoch0) 发布为 consumerTree。
3. 为 epoch1 创建新的 producerTree(epoch1)。
4. TakeRegions 开始允许先查 consumerTree，再走 bump。

此时 consumerTree 里只包含旧 epoch 的 released span；producerTree(epoch1) 只接收本轮新释放的 span。

### 3.3 第二次 grow 后

1. GrowOneSegment 再次成功，进入 epoch2。
2. 将旧 consumerTree 与旧 producerTree(epoch1) 合并成新的 consumerTree。
3. 为 epoch2 创建新的 producerTree(epoch2)。
4. TakeRegions 继续只从合并后的 consumerTree 取。

这样 consumerTree 代表“所有已发布的历史 epoch released span”，producerTree 代表“当前 epoch 还未发布的 released span”。

### 3.4 后续循环

以后每次 grow 都重复下面动作：

1. 冻结当前 producerTree。
2. 旧 consumerTree 与冻结后的 producerTree 合并。
3. 合并结果替换为新的 consumerTree。
4. 新 epoch 创建一个空的 producerTree。

从抽象上看，这不是“永远只有两棵固定地址的树”，而是“永远维持两个 bank 角色”：

1. 一个 bank 负责写入。
2. 一个 bank 负责取用。

## 4. 可行性评估

结论先说在前面：

1. 这个方案**在架构上可行**。
2. 但它不是小修补，而是 FreeManager released pool 管理方式的一次中等规模重构。

### 4.1 为什么说可行

当前 releaseTree 的所有权比较集中，主要收口在 FreeManager：

1. Take 入口在 TryTakeRegions。
2. Insert 入口在 ReclaimRegionsReleased / InsertRegions。
3. grow 入口在 GrowOneSegment。

也就是说，producer / consumer 分离可以主要落在 FreeManager 内部，而不需要把大量逻辑散到各个 GC 组件里。

### 4.2 它解决了什么问题

如果 producerTree 与 consumerTree 分锁，那么至少能直接改善下面两点：

1. reclaim 插入不会再和 mutator 取用争同一把树锁。
2. 当前 epoch 刚释放出来的 span 不会立刻被 mutator 拿走，减少“刚 release 就被重新 fault-in”的热路径抖动。

### 4.3 它没有自动解决的问题

这个方案不会自动消掉所有 releasedTree 成本，至少还有三类成本仍然存在：

1. consumerTree 自身的树操作成本仍在。
2. 旧 span 真被拿来分配时，冷页重新建立成本仍在。
3. grow 时做树合并，会把一部分成本从 take/reclaim 热路径搬到 epoch 切换点。

所以它更像是“把热点形态改对”，而不是“树一拆二就一定更快”。

### 4.4 为什么不建议做“节点级并行查找”

从当前 CartesianTree 的实现看，不建议直接把 releasedTree 改成“多个协程并行查找节点，找到后只锁住单个节点使用，发现该节点已锁则继续找别的节点”的模型。

原因不是这个思路完全不可想象，而是它和当前数据结构不匹配：

1. 当前 take 不是纯只读查找，而是“查找 + 修改节点 index/count + 旋转下沉 + 可能删除节点”的组合操作。
2. 节点在一次 take 后可能被旋转到别的位置，甚至被删除回收，因此“先无锁看到一个节点，再稍后锁住它”并没有稳定语义。
3. 当前遍历依赖的是树指针结构本身，而不是不可变快照；如果并行线程一边查找、一边旋转或删除，另外的查找线程很容易读到变化中的子树。
4. 当前实现没有节点级生命周期保护，也没有版本号、hazard pointer、epoch reclamation 之类的并发内存安全机制。

所以，如果直接做节点级 try-lock，最终通常还是会回到下面这个现实：

1. 为了保证旋转、删除和父子指针更新安全，仍然需要更大范围的结构锁。
2. 一旦引入这些额外锁和校验，复杂度会接近“重写一套并发树”，不再是对当前 releaseTree 的增量优化。

更现实的替代方向是“树级并行”，不是“节点级并行”：

1. 把 consumerTree 再拆成多个 shard，每个 shard 一把锁。
2. take 先根据摘要信息挑可能满足请求的 shard 去 try_lock。
3. 某个 shard 锁忙就直接跳到下一个 shard，不等待。
4. reclaim 则按地址范围、segment 或 hash 规则把 span 插到对应 shard。

这样能达到“锁冲突时去找别的候选，不在一个热点点位上等锁”的目标，同时避免把当前 CartesianTree 直接改造成复杂的并发节点锁结构。

## 5. 需要补强的点

你的原始方案是成立的，但要真正能落地，至少要补齐下面这些边界。

### 5.1 不能把 producer 只理解成 BackgroundReclaim

这点是最重要的前提修正。

当前往 released pool 放 span 的并不只有 BackgroundReclaim。若 Step15 只让 BackgroundReclaim 写 producerTree，而把其它 reclaim 路径继续写旧 consumerTree，那么 producer / consumer 分离就会被立刻打破。

建议：

1. 所有 ReclaimRegionsReleased 的调用方统一写入 producerTree。
2. 不按“调用来源”分树，只按“当前 epoch 是否已发布”分树。

### 5.2 merge 不能长时间阻塞 take 热路径

你提到“合并的时候要加锁保证安全”，这点是对的，但还需要再细化：

1. 不能在 consumer 正在被大量 TryTake 的时候，拿着同一把锁做长时间 merge。
2. 否则会把原来的 lock contention 从 take-vs-insert 变成 take-vs-merge。

建议：

1. merge 在一个独立 publish / rotate 锁下触发。
2. 尽量离线构建 merged consumer。
3. 真正对外切换 consumer 指针时，只做一次短临界区 swap。

### 5.3 shrink / tail rewind 也要适配双树

当前 FreeManager 的 ShrinkHeap / RewindInactiveZoneUsingReleasedTail 会从 released pool 的尾部取 trailing span 回收 committed tail。

变成双树后，tail span 可能在：

1. consumerTree 里。
2. producerTree 里。

所以 shrink 逻辑必须能查看两边，不能默认只有一棵树。

### 5.4 统计与诊断要分 bank

如果引入双树，但统计仍只打一个 releasedPath/busyRetries，总结论会重新变模糊。

建议至少分出：

1. consumer take 次数、失败次数、锁等待。
2. producer insert 次数、merge 次数、merge 耗时。
3. publish / rotate 触发原因。

## 6. 建议的数据结构与接口改造

建议把当前单 backend 改成“两个 release bank + 一个 rotate 锁”的形式。

### 6.1 建议的数据结构

```cpp
struct ReleaseBank {
	std::unique_ptr<Backend> backend;
	std::mutex mutex;
	UnitCount totalUnits;
	uint64_t epoch;
	bool published;
};
```

FreeManager 内建议增加：

1. consumerBank_
2. producerBank_
3. rotateMutex_
4. 当前 epoch 号
5. rotate / merge 统计字段

如果后续验证发现 consumerBank 仍然是热点，可以继续演进成：

1. `consumerBanks_[N]`
2. 一个只读的 shard 摘要数组，例如每个 shard 的 `maxBlockCount`、`totalUnits`
3. 按 shard 维度统计 take 命中率和锁冲突情况

### 6.2 建议新增的核心接口

建议围绕当前接口拆出更明确的角色：

1. TryTakeRegionsFromConsumer
2. InsertRegionsToProducer
3. PublishProducerOnGrow
4. MergeBanksToConsumer
5. TryTakeTrailingRegionsFromBanks

如果演进到 consumer shard 版本，则可以进一步细化为：

1. TryTakeRegionsFromConsumerShard
2. SelectConsumerShardForTake
3. InsertRegionsToConsumerShard
4. CollectConsumerShardStats

### 6.3 并发语义建议

1. take 只碰 consumerBank_.mutex。
2. reclaim insert 只碰 producerBank_.mutex。
3. rotate / publish 只碰 rotateMutex_，并在必要时短暂进入 consumer / producer 的 swap 临界区。
4. take 若拿不到 consumer 锁，可直接 fallback 到 bump，不做长时间等待。

### 6.4 树级分片并行的具体做法

如果后续验证发现 consumerBank 仍然是热点，建议优先把它演进成“多棵 consumer shard tree”，而不是做节点级锁。

#### 6.4.1 分片对象

最直接的做法是只对 consumer 侧分片：

1. producer 仍保持单 bank，简化 reclaim 写入路径。
2. consumer 变成 `consumerBanks_[N]`，每个 shard 一棵 tree、一把锁。
3. mutator take 在 shard 间做 try-lock 轮询，不在单棵树上等待。

这样改的好处是：

1. 不破坏 Step15 当前“双树轮转”的主结构。
2. 真正的热路径并行化只发生在 consumer 读侧。
3. reclaim 侧不用同时承受“producer 分片 + consumer 分片”的复杂度。

#### 6.4.2 分片规则

建议优先使用“按地址范围分片”，而不是按 hash 分片。

推荐规则：

1. 把整个 heap 的 region index 空间按连续区间切成 N 段。
2. 每个 shard 只管理自己负责的 region index 范围。
3. 一个 span 属于哪个 shard，由它的起始 index 和结束 index 落在哪个区间决定。

优先按范围分片的原因是：

1. 与 heap grow 的 segment 连续布局天然一致。
2. 对 shrink / tail rewind 更友好，因为尾部 span 一定集中在高地址 shard。
3. 后续做 locality 分析时，更容易观察“哪个地址段的 released span 最热”。

#### 6.4.3 span 跨 shard 边界怎么处理

地址范围分片的一个必答题是：大 span 可能跨多个 shard。

建议规则是“按 shard 边界切分，再分别插入”：

1. reclaim 一个 span `[idx, idx + num)` 时，先判断它覆盖了哪些 shard 范围。
2. 如果 span 落在单个 shard 内，直接插入该 shard。
3. 如果 span 跨多个 shard，就按边界切成多个子 span，再分别插入对应 shard。

这样做的代价是多了一次切分，但好处是：

1. shard 内部不需要知道别的 shard 的边界。
2. 每个 shard 的 tree 仍然只维护自己的连续地址子区间。
3. tail rewind 和 per-shard 统计都更简单。

#### 6.4.4 shard 摘要信息

为了避免 take 每次盲扫所有 shard，建议给每个 shard 保留一个无锁读取的摘要结构：

```cpp
struct ConsumerShardSummary {
	std::atomic<UnitCount> totalUnits;
	std::atomic<UnitCount> maxBlockUnits;
	std::atomic<uint64_t> lastBusyTimestamp;
};
```

最低限度需要：

1. `totalUnits`：判断 shard 是否可能为空。
2. `maxBlockUnits`：判断 shard 是否可能满足本次请求大小。

这样 take 可以先无锁筛掉明显不可能命中的 shard，再去尝试 try-lock。

#### 6.4.5 take 侧的选择流程

建议 take 流程改成下面这样：

1. 读一遍所有 shard 的 `maxBlockUnits` 摘要，筛出满足 `>= num` 的候选 shard。
2. 按优先级排列候选 shard。
3. 依次对候选 shard 做 try-lock。
4. 某个 shard 锁忙，直接跳下一个。
5. 某个 shard 加锁成功但真实 tree 已经拿不到 span，则刷新摘要后继续下一个。
6. 所有 shard 都失败，再回退到 bump / grow。

这里最关键的点是：

1. `BUSY` 只在 shard 级别传播，不再是一把全局锁把所有线程都挡住。
2. 摘要可以是近似值，不要求完全强一致；真正结果以拿到 shard 锁后的 tree 为准。

#### 6.4.6 shard 优先级怎么排

建议至少支持两种优先级策略：

1. 低地址优先：更接近当前单树 low-address-first 的语义。
2. 本地轮转优先：每个 P 或线程记住上次命中的 shard，下次从邻近 shard 开始找，减少 herd effect。

实际落地时，可以先做最简单版本：

1. 从一个线程本地起点 shard 开始。
2. 按环形顺序轮询其余 shard。

这样即使所有线程都同时来取，也不会永远先撞同一个 shard。

#### 6.4.7 rotate / merge 时如何处理 shard

双树轮转和分片并不冲突，推荐的组合方式是：

1. producer 仍保持单 tree。
2. grow 时先冻结 producer。
3. 再把冻结 producer 中的 spans 按 shard 范围分发到各个 consumer shard。
4. 对每个 shard 分别做 merge insert。

实现上可以有两种方式：

1. 串行分发：简单、风险低，先作为第一版。
2. 后台分发：把 frozen producer 里的 spans 收集出来，分桶后并行 merge 到各个 shard。

不管哪种方式，都建议遵循一个原则：

1. rotate 的“对外可见切换”要短。
2. 真正重活是 span 收集、分桶、merge，这部分尽量放在切换前后离线处理。

#### 6.4.8 shrink / tail rewind 如何适配 shard

按地址范围分片后，tail rewind 反而更容易做：

1. 先定位 inactiveZone 对应的尾部 shard。
2. 优先只检查最高地址 shard 的 rightmost node。
3. 如果该 shard 没有匹配尾部的 span，再向前一个 shard 回退。

因为 shard 是按地址连续切分的，所以不需要在所有 shard 上无序扫描。

#### 6.4.9 这个方案的收益边界

树级分片并行最适合解决的是“全局 consumer 锁争用”。

它的直接收益主要是：

1. 让 try-lock 冲突从单点热点变成多个 shard 的分散热点。
2. 让 `BUSY -> 换别的地方找` 真正成立。
3. 降低单棵 tree 的 nodeCount 和遍历开销。

但它不直接解决：

1. 冷页重新建立成本。
2. 单个 shard 内部的 CartesianTree 旋转与删除成本。
3. grow rotate 时的总 merge 工作量。

所以它应该被理解为“降低全局锁争用”的手段，而不是 releasedTree 全部问题的总解。

## 7. 建议的分配与回收流程

### 7.1 建议的 take 流程

按你原方案，可以维持“consumer first”的主策略：

1. 优先尝试 consumerTree。
2. consumer 锁忙或 consumer 为空，直接走 bump。
3. bump 不够，尝试 grow。

这样可以满足你的“取锁失败则走 bump”。

### 7.2 建议的 reclaim 流程

所有 released 回收路径统一做：

1. ReleaseUnits
2. 进入 saferegion
3. InsertRegionsToProducer

不要区分是 BackgroundReclaim、InlineReclaim 还是 LargeObject reclaim。

### 7.3 建议的 grow / rotate 流程

推荐流程如下：

1. growMutex_ 成功后完成新 segment 提交。
2. 进入 rotateMutex_。
3. 冻结当前 producerBank，马上切一个新的空 producerBank 给新 epoch 使用。
4. 在后台或离线阶段，把“旧 consumer + 冻结 producer”合并成新的 merged consumer。
5. 用短临界区替换 consumerBank 指针。
6. 释放 rotateMutex_。

这比“拿住同一把树锁直接 merge 完再放开”更稳妥。

如果 consumer 已经演进成 shard 版本，则 grow / rotate 的细化流程建议是：

1. 冻结 producerBank。
2. 遍历 frozen producer，收集所有 spans。
3. 按 shard 地址范围把 spans 分桶。
4. 对每个 shard 依次或并行做 merge insert。
5. 更新每个 shard 的摘要字段。
6. 最后再对外发布新的 consumer 视图。

### 7.4 对分配顺序的一个改进建议

你原方案是“consumer first，再 bump”。这个方向可以保留，但我建议不要绝对固定，而是做成策略开关：

1. 低压力时：bump first，consumer second，保留热点局部性。
2. 高压力或 consumer 已积压较大时：consumer first，bump second。

原因是双树解决的是“争锁与阶段污染”，不代表所有场景都适合强制优先吃旧 released span。

## 8. 验证计划

建议把验证拆成三层。

### 8.1 正确性验证

1. 重复分配 / 回收压力测试，确认不会双重分配同一 span。
2. grow / shrink / rewind inactive tail 混合测试。
3. grow rotate 与 reclaim / take 并发交错时的锁安全验证。

### 8.2 性能验证

重点看下面几类指标是否下降：

1. take 路径 busy_retries
2. consumer lock wait
3. producer insert lock wait
4. merge 总耗时与单次峰值

### 8.3 基准用例验证

建议至少重跑：

1. P1-Single
2. benchmark_wide
3. 高 churn 小对象用例
4. large object 占比高的用例

并做下面几组对比：

1. 单树 vs 双树
2. consumer first vs bump first
3. 不同 merge / rotate 触发频率下的抖动差异
4. 不同 shard 数量下的吞吐与锁冲突变化

## 9. 结论

我的结论是：

1. 这个方案**可行**，而且方向是对的。
2. 它最大的价值在于把“当前 epoch 的释放写入”和“历史 epoch 的取用消费”拆开，而不是单纯把一棵树变两棵树。
3. 它最需要补强的地方，是 merge 的短临界区设计，以及 shrink / tail reclaim 对双树的兼容。

如果推进，我建议按下面节奏做：

1. 先做 FreeManager 内部 bank 抽象，不改太多策略。
2. 第一阶段只实现“consumer/prodcer 分锁 + grow 时 rotate”。
3. 第二阶段再补 consumer-first / bump-first 策略开关。
4. 第三阶段再看是否需要优化 merge 算法或替换 backend。

也就是说，Step15 推荐作为一个**值得实现的结构性方案**，但不建议一次性把“双树 + 新策略 + 新 merge 算法”全部一起落地。先把 bank 轮转做出来，再用数据决定下一步，技术风险最低。
