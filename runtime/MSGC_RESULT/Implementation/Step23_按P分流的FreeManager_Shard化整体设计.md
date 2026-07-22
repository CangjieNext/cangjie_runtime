# Step 23：按 P 分流的 FreeManager Shard 化整体设计

> 2026-06-11 设计稿（实现前评审用）
> 目标：将 `FreeManager` 从“单全局 page-heap”升级为“按 P 分流、本地 shard 优先、缺货时直接 steal 其他 shard free span”的多 shard 架构，并将 `regular alloc`、`large alloc`、`grow/shrink`、`scavenge/reclaim` 一起改造成 shard-aware 版本；同时约定第一版 `GC` 日志里的 shard snapshot 字段。更细的统计口径仍留待后续单独讨论。

---

## 目录

- [Step 23：按 P 分流的 FreeManager Shard 化整体设计](#step-23按-p-分流的-freemanager-shard-化整体设计)
  - [目录](#目录)
  - [1. 背景与目标](#1-背景与目标)
  - [2. 核心设计决策](#2-核心设计决策)
  - [3. 总体架构](#3-总体架构)
  - [4. 数据结构与元数据改造](#4-数据结构与元数据改造)
  - [5. 分配路径重构](#5-分配路径重构)
  - [6. 跨 Shard Steal 协议](#6-跨-shard-steal-协议)
  - [7. Grow / Shrink 重构](#7-grow--shrink-重构)
  - [8. Scavenge / Reclaim 重构](#8-scavenge--reclaim-重构)
  - [9. 与现有模块的接口改造](#9-与现有模块的接口改造)
  - [10. 统计口径（暂缓）](#10-统计口径暂缓)
  - [11. 推荐落地顺序](#11-推荐落地顺序)
  - [12. 风险、取舍与非目标](#12-风险取舍与非目标)

---

## 1. 背景与目标

`Step22` 已经确认，当前瓶颈不仅在 `FreeManager::TryTakeRegions()` 的单全局 `pageHeapMutex_`，
也在于 large alloc、empty-region reclaim、scavenge、grow/shrink、统计读取都和这套单实例 page-heap 元数据耦合在一起。

当前实现的核心问题有四个：

1. `FreeManager` 只有一套全局 `allocBits_ / scavengedBits_ / chunkSummaries_ / searchIdx_`，所有 mutator 最终都会会合到同一把锁上。
2. `MSRegionManager::TakeRegion()` 和 `TakeRegionForLarge()` 都直接使用这套全局 page-heap，因此 small / large 两条路径会相互挤占。
3. `ReclaimRegionsReleased()`、`ScavengeFreeRegions*()`、`GrowOneSegment()`、`ShrinkHeap()` 与热路径共享同一元数据中心，后台工作会放大前台尾延迟。
4. 当前统计口径以“单全局 page-heap”视角组织，无法直接回答“是哪个 P / shard 出现缺货、偷取、grow、scavenge 压力”。

本 Step 的目标不是只把 `TryTakeRegions()` 改快一点，而是做一次完整的 shard-aware 重构：

1. `regular alloc` 按 P 命中本地 shard，缺货时直接 steal 其他 shard 的 free span。
2. `large alloc` 不再走单全局 page-heap，而是也先查本地 shard，再 steal，再 grow。
3. `grow / shrink / scavenge / reclaim / stats` 全部转成 shard-aware 语义。
4. `shard` 数量**固定由 P 数量决定**，不再引入独立的 shard 数环境变量。

最终希望把并发模型从：

```text
many mutators / large alloc / reclaim / scavenger
  -> 1 FreeManager
  -> 1 pageHeapMutex_
```

改成：

```text
mutator on P_i
  -> home shard_i
  -> local try-take
  -> remote steal if needed
  -> global grow/shrink only as last resort
```

## 2. 核心设计决策

### 2.1 shard 数量 = P 数量

本设计固定：

```text
shardCount = max(1, cjProcessorNum)
```

也就是说：

- shard 数量和运行时 P 数量一一对应；
- 第一版不额外提供 `CJ_MS_SHARD_COUNT` 一类独立开关；
- `P id` 和 `home shard id` 采用相同编号空间。

这样做的原因是：

1. 调度局部性最自然。哪个 mutator 跑在哪个 P 上，就优先命中哪个 shard。
2. 调参与观测口径最简单。`P3` 慢就能直接看 `shard3` 的 free run / steal / grow / scavenge 状况。
3. 可以避免“P 数和 shard 数不一致”带来的额外映射与负载不均衡判断。

### 2.2 初始 committed heap = P * 64M

当前初始堆口径约为 `256M`。本 Step 明确改成：

```text
initialCommittedHeapBytes = shardCount * 64M
```

也就是说：

- 每个 `P id / shard id` 在初始化阶段就拿到一块固定的 `64M` 初始堆地址空间；
- 初始化后 heap 视角不再是“一整片共享初始 committed 区”，而是每个 shard 各自拥有一块独立初始 segment；
- 第一版里 `64M` 同时作为：
  - 初始化时每个 shard 的保底 committed 容量；
  - grow 时的基础扩容粒度；
  - shrink 时的基础回收粒度。

这样做的原因是：

1. shard 从一开始就有明确的本地供给，不需要所有 P 先争抢同一片初始 256M。
2. 初始布局和后续 grow 粒度统一，owner 语义最干净。
3. `64M` 足够形成稳定的本地 free span 池，同时不会像更大粒度那样过早抬高每个 shard 的保底 RSS。

第一版这里不再额外引入“上限钳制”或“按小堆配置自动降级”的分支：

- `initialCommittedHeapBytes` 就固定等于 `shardCount * 64M`
- 不做 `min(heapLimit, shardCount * 64M)` 一类折中
- 不做“P 太多时自动缩小单 shard 初始 segment”的降级策略

### 2.3 按 P 分流，但 free span owner 固定不转移

虽然入口按 P 分流，但 free span 不能仅凭“当前在哪个 P 上”判断归属。

因此需要引入明确的 owner 语义：

- 第一版不为每个 stolen span 动态维护 `ownerShardId`；
- owner 只记录在 `segmentOwnerShard_` 上，某个 free span / region 的 owner 由它所在的 `64M segment` 决定；
- 正常本地分配时，只消费 `home shard` 自己持有的 free span；
- remote steal 的语义不是“owner 转移”，而是“borrow”：
  - donor 借出 free span，但 owner 仍然归 donor shard；
  - borrower 只把这批 region 挂到自己本地的 `borrowed region list` 中消费；
  - 后续 reclaim / scavenge 仍按原 owner shard 回去。

这里的关键不是“请求方拿到 span 就获得归属”，而是：

- 分配入口按 P 分流；
- free span 的 owner 固定；
- steal 只是借用，不改 owner；
- reclaim / scavenge 永远按 owner shard 组织。

这里顺便明确一件事：第一版**不需要**“每个 free span / region 都有一份可变 `ownerShardId` 元数据”。

原因是：

1. `owner` 只用于决定“未来 free / reclaim / scavenge 回到哪个 shard”，这件事按 `segmentOwnerShard_` 反查已经足够；
2. stolen span 如果还要逐 region 改 owner，会把 steal、归还、reclaim 三条路径都变成高频元数据更新点；
3. 这类动态 owner 元数据还会和 `borrowed region list` 重复表达同一件事，收益小于复杂度。

### 2.4 steal 连续失败 2 次后立即 grow

本 Step 不采用“把所有 shard 都扫空后再 grow”的纯保守策略，而是引入固定的 grow 升级门槛：

```text
local shard miss
  -> remote steal attempt #1
  -> remote steal attempt #2
  -> if both fail: grow one 64M segment for requester shard
```

这里的“失败 2 次”是指：

- 两次 donor 选择与尝试之后，仍然没有拿到满足请求的 free span；
- 对 `num == 1` 与 `num > 1` 使用不同 donor 选择逻辑，但共享同一个“连续失败 2 次即 grow”的升级规则。

第一版里，下列情况都算一次 steal 失败：

1. donor snapshot 观察下来就不满足本次请求；
2. 选中 donor 后加锁失败；
3. 加锁成功，但精确查找仍然失败；
4. donor 命中后实际摘取结果仍不足以满足本次请求。

这样做的原因是：

1. 避免在高并发下把时间花在过长的 donor 扫描链上。
2. 让缺货 shard 更快恢复到本地供给状态，而不是持续依赖 remote steal。
3. 让 grow 成为“本地供给恢复手段”，而不是全局供给彻底耗尽后的最后悲观分支。

### 2.5 grow / shrink 仍保留全局协调层

虽然热路径要去中心化，但 `grow` 和 `shrink` 不适合完全放给单个 shard 各自独立做。

因此：

- `grow` 仍然通过一个全局协调器串行推进；
- `shrink` 也由全局协调层统一检查“堆尾 segment 是否可收缩”；
- shard 只负责自己持有的 free span 元数据，不直接改变整个保留区边界。

### 2.6 shard 内仍保持单锁，不继续拆成 3 把锁

本 Step 明确不把每个 shard 再拆成：

- `allocLock`
- `reclaimLock`
- `scavengeLock`

这样的多锁模型。

第一版仍保持：

```text
one shard -> one shardMutex_
```

即：

- `TakeRegion`
- `Reclaim`
- `Scavenge` 内部元数据修改

最终仍围绕同一套 shard 主元数据和同一把 `shardMutex_` 组织。

这样做的原因是：

1. `allocBits_ / scavengedBits_ / chunkSummaries_ / free counters / takeCursor_ / reclaimCursor_` 的一致性边界最清晰。
2. 先用 shard 化把“全局一把锁”打散成“每个 shard 一把锁”，已经能吃到最大一波收益。
3. 在这个基础上继续加“Reclaim 批量化、Scavenge try-lock、Steal 批量偷”，比直接引入多锁模型风险更低。

因此本 Step 的思路不是“再拆更多锁”，而是：

- 保持 shard 内单锁；
- 通过**减少持锁次数**和**减少持锁时长**来继续降竞争。

### 2.7 CentralRegionPool 不做彻底 shard 化

本 Step 聚焦的是 `FreeManager` 的 shard 化和全链路接口升级。

因此这里直接定边界：

- `CentralRegionPool` 仍保留单逻辑实例；
- `empty cache` / `bucket` 继续保持中央共享结构，不作为本 Step 的 shard 化对象；
- 所有需要 fresh-region / reclaim / empty-region flush 的接口，都必须带上 `shardHint` 或可反查 `owner shard` 的地址信息；
- 后续默认也不把 `CentralRegionPool` 的深度 shard 化列为必做项；除非 profile 明确证明中央池本身重新成为主要瓶颈，否则保持现状。

这样可以先把最大的 page-heap 热点拆掉，而不一次性改动过多上层结构。

### 2.8 reclaim / scavenge 都按 owner shard 组织，并由后台线程专职执行

本 Step 明确采用下面这条执行边界：

- `reclaim`：按 `owner shard` 组织
- `scavenge`：按 `owner shard` 组织
- `sweep / scavenge` 沿用当前 `BackgroundProcessor::Run()` 的单后台线程事件循环
- `Scavenge` 内部继续保留“先 reclaim 到 hotFree，再 scavenge 物理页”的两段式逻辑
- mutator 不直接拿 `shardMutex_` 做 reclaim；后台唤醒沿用当前已有的 `NotifyToScavenge / RequestWork / Notify` 机制
- allocation 热路径不再做同步 reclaim

也就是说，第一版不新增一条独立的 “direct released spans -> page-heap” 新路径，而是继续沿用当前两类来源：

```text
emptySwept
  -> BackgroundScavengeLoop Step A
  -> batch reclaim to owner shard hotFree

largeHotCache
  -> BackgroundScavengeLoop Step A
  -> batch reclaim to owner shard hotFree
```

真正的：

- reclaim：把 `emptySwept` / `largeHotCache` 中的 region 按 owner shard 批量回到 `hotFree`
- scavenge：把 `hotFree` 对应的物理页批量释放给 OS

都只在后台线程中执行。

这里的 `reclaim` 不是新的 GC phase，而是当前 `BackgroundScavengeLoop()` 内部的 Step A：

```text
emptySwept / largeHotCache
  -> batch reclaim back into owner shard hotFree
  -> hotFree becomes visible to Step B
```

它之所以单独存在，是因为 Step23 已经明确：

1. mutator 不再同步拿 `shardMutex_` 做 reclaim；
2. reclaim 需要按 owner shard 路由，但仍希望保留当前 `Scavenge` 主体逻辑；
3. batch reclaim 能显著降低位图更新、summary 更新和锁竞争次数。

因此第一版里：

- `sweep` 负责“清扫 region，并把结果放回中央池、empty list、large cache 等现有结构”
- `reclaim` 负责“把 `emptySwept` / `largeHotCache` 中的 region 按 owner shard 批量并回 owner shard 的 `hotFree`”
- `scavenge` 负责“按当前 `BackgroundScavengeLoop()` 的主体逻辑，消费 `hotFree` 并把对应的物理页释放给 OS”

沿用当前 [BackgroundProcessor.cpp](/Users/lipeisheng/code/cangjie/cangjie/cangjie_runtime_MSGC/runtime/src/Heap/MCollector/BackgroundProcessor.cpp:179) 的单后台线程事件循环时，推荐处理顺序为：

```text
sweep
  -> reclaim
  -> scavenge
```

原因是：

1. `sweep` 先产出新的可回收 span；
2. `reclaim` 再把 `emptySwept` / `largeHotCache` 转成 owner shard 可见的 `hotFree`；
3. `scavenge` 最后基于更新后的 `hotFree` 再决定释放多少物理页。

### 2.9 继承 `TakeRegion / free` 的双端推进语义，避免刚 free 又立即被取回

当前设计里还有一条需要显式继承的行为约束：`TakeRegion` 与 `free/reclaim` 不应机械地共用同一个“立刻回退”的前沿，否则某个 region 刚 free，下一次本地 take 就可能马上又把它拿走。

因此，Step23 第一版在 shard 内继续保留下面这条语义：

1. `TakeRegion` 优先沿着自己的 `take-side frontier` 向前推进；
2. `free / reclaim` 合并回本 shard 时，优先进入 `reclaim-side frontier`；
3. 新近 free 的 span 不应因为一次 `free` 就立刻把 `take-side frontier` 拉回去；
4. 只有当当前 `take-side` 供给耗尽，或显式切换到 reclaim 侧时，allocator 才重新消费这批较新的 free span。

也就是说，这里继承的是“**避免刚 free 又立即被本地再次取回**”的策略目标，而不是简单继承当前单全局实现里的：

```cpp
searchIdx_ = std::min(searchIdx_, idx);
```

在 shard 化之后，若仍然无条件把本地 take 游标直接回退到最新 free 的位置，会削弱：

- shard 内局部性；
- batch steal / batch reclaim 带来的摊薄效果；
- 对“刚释放对象热页”的冷却窗口。

因此第一版建议把 shard 内 free 供给至少分成两个逻辑前沿：

- `take-side`
  - 当前优先被分配消费的一侧
- `reclaim-side`
  - 新近 free / reclaim 回来的 span 先进入这一侧

第一版这里直接写死成“两套 cursor”实现，即 `takeCursor_ + reclaimCursor_`；不再保留其他等价机制分支。语义上必须满足：**不能让新 free span 在同一轮本地分配中立刻回到最热前沿。**

## 3. 总体架构

改造后的层次建议如下：

```text
AllocBufferMS / mutator on P_i
  -> home shard id = P_i
  -> CentralRegionPool (logical single instance, shard-aware interfaces)
  -> MSRegionManager
  -> FreeManagerCoordinator
       ├─ FreeManagerShard[0]
       ├─ FreeManagerShard[1]
       ├─ ...
       └─ FreeManagerShard[P-1]
```

其中：

- `FreeManagerCoordinator` 是新的薄协调层，负责：
  - shard 数组生命周期；
  - grow / shrink 全局边界；
  - shard 级 steal 调度；
  - 全局统计汇总。
- `FreeManagerShard` 负责：
  - 本 shard 的 `allocBits_ / scavengedBits_ / chunkSummaries_`；
  - 本 shard 的 `takeCursor_ / reclaimCursor_ / freeRegionCount_ / scavengedRegionCount_`；
  - 本 shard 的 `TryTakeRegionsLocal()`、`ReclaimRegionsLocal()`、`ScavengeLocal()`。
- `MSRegionManager` 负责：
  - 将上层的 `TakeRegion / TakeRegionForLarge / Reclaim* / Grow / Shrink` 请求路由到正确 shard；
  - 维护全局 `segmentOwnerShard_` 元数据；
  - 处理“当前线程没有绑定 P”时的 fallback home shard 选择。

### 3.1 home shard 选择规则

正常 mutator：

```text
homeShardId = currentProcessorId
```

无 P 绑定的线程（如 background/helper/foreign thread）：

```text
homeShardId = tidHash % shardCount
```

但要注意：

- `free span` 的真正归属以 `owner shard` 为准；
- fallback home shard 只用于“当前请求优先查哪个 shard”，不改变已有 span 的 owner。

### 3.2 初始化布局规则

初始化阶段按 `shardCount = P 数量` 一次性切出 `P * 64M` 的 committed heap：

```text
initial segment size = 64M
initial segment count = shardCount
initial committed heap = shardCount * 64M
```

每个 shard 在启动时持有一个独占初始 segment：

- `segmentOwnerShard_[i] = i`
- shard 内部直接为这 64M 建立本地 `SegmentMeta`

这样 grow 前的供给模型就是完全本地化的，不存在“所有 P 共享同一片初始 256M”。

### 3.3 span 归属与 borrow 的基本原则

本方案里 owner 不随 steal 迁移，因此第一版不要求对每个 stolen region 动态改 `owner` 元数据。

更准确地说：

- grow 出来的 `64M segment` 在创建时就绑定 owner shard；
- 之后即使其中一小段 free span 被别的 shard 借走，这段 span 的 owner 仍然不变；
- reclaim / scavenge / shrink 时，按地址先找到所属 segment，再由 `segmentOwnerShard_` 得到 owner shard。

也就是说，Step23 第一版采用的是：

```text
owner 固定在 segment 级
steal = borrow
borrow 不改 owner
```

进一步说，stolen span 在第一版里只有两种需要长期维护的状态：

1. `owner`
   - 由地址落到哪个 `segment` 决定，再由 `segmentOwnerShard_` 反查；
2. `borrowed-free`
   - 表示这段 free region 已经从 donor 的 free 结构中摘掉，只能由 borrower 消费

这里不再额外引入全局 `stolen bit` / `borrowed bit` bitmap；`borrowed-free` 状态直接由：

- donor 侧“已经不在 free 结构里”
- borrower 侧“已经挂在 `borrowed region list` 里”

这两件事共同表达。

## 4. 数据结构与元数据改造

### 4.1 新增 `FreeManagerShard`

建议把当前 [MSFreeManager.h](/Users/lipeisheng/code/cangjie/cangjie/cangjie_runtime_MSGC/runtime/src/Heap/MAllocator/MSFreeManager.h:21) 中的大部分热路径状态下沉到 `FreeManagerShard`：

```cpp
class FreeManagerShard {
    std::mutex shardMutex_;
    uint64_t* allocBits_;
    uint64_t* scavengedBits_;
    ChunkSummary* chunkSummaries_;
    size_t bitmapWordCountInUse_;
    size_t summaryChunkCountInUse_;
    size_t freeRegionCount_;
    size_t scavengedRegionCount_;
    RegionIndex takeCursor_;
    RegionIndex reclaimCursor_;
    ShardSegmentSet ownedSegments_;
    BorrowedRegionList borrowedRegions_;
};
```

其中 `ownedSegments_` 表示“本 shard 当前管理哪些 segment / region span”。
`borrowedRegions_` 表示“本 shard 从其他 owner shard 借来的 free region / free span，本地只消费，不取得 owner”。
`takeCursor_ / reclaimCursor_` 共同表达 shard 内的双端推进语义：分配优先沿 `take-side` 前进，新近 free/reclaim 的 span 先落在 `reclaim-side`，避免刚 free 又被立即取回。

建议 `BorrowedRegionList` 内部至少保存：

```cpp
struct BorrowedSpan {
    uint16_t ownerShard;
    RegionIndex start;
    RegionCount len;
};
```

这份 `BorrowedSpan` 元数据的意义，不是改变 owner，而是表达：

1. 这段 region 当前已经不能再被 donor shard 使用；
2. 这段 region 当前只允许 borrower shard 本地消费；
3. 保留 owner / 起点 / 长度信息，供后续真正 free 后按 owner shard 回流使用；第一版不据此设计“未消费 borrowed 主动退回”机制。

### 4.2 `FreeManager` 改为协调器

旧 `FreeManager` 保留类型名，但职责变成协调层：

```cpp
class FreeManager {
    std::vector<FreeManagerShard> shards_;
    std::atomic<uint32_t> shardCount_;
    std::mutex growMutex_;
    std::vector<uint16_t> segmentOwnerShard_;
    static constexpr size_t kShardSegmentBytes = 64ULL * 1024ULL * 1024ULL;
};
```

第一版只要求一张 owner 表：

1. `segmentOwnerShard_`
   - 记录某个 `64M segment` 当前归属于哪个 shard；
   - grow 时设置；
   - reclaim / scavenge / shrink 时通过地址反查 owner。

因为 Step23 第一版采用的是“owner 固定在 segment 级、steal 只是 borrow”，所以不再要求为 stolen free span 动态维护 `regionOwnerShard_`。

### 4.3 当前单全局位图如何拆

当前实现的：

- `allocBits_`
- `scavengedBits_`
- `chunkSummaries_`

都是基于整个 committed heap 的单全局数组。

改造后建议：

- 每个 shard 只为自己持有的 segment 建立本地位图视图；
- 不要求每个 shard 的 region index 连续，但要求 segment 内 region 连续；
- shard 内部以“segment 为单位管理多个局部位图块”。

也就是说，第一版不要再强行维持“全局一个大 bitmap”的抽象，而是转成：

```text
FreeManagerShard
  -> SegmentMeta[owned]
       -> allocBits block
       -> scavengedBits block
       -> chunk summaries block
```

这样最符合“按 P 分流 + grow/shrink 以 segment 为边界”的结构。

### 4.4 shard 内部 hot path 微优化（`num == 1`）

`FreeManager` 做完 shard 化之后，热点会从“单全局锁竞争”进一步收敛到“单个 shard 持锁后的局部操作”。

因此，当前在单全局实现里已经值得做的两个 `num == 1` 优化，在新设计里不仅仍然成立，而且更应该保留为 `FreeManagerShard` 的固定热路径。

#### 优化 1：`FindFreeRunLocked(num == 1)` 快速路径

建议在 `FreeManagerShard` 中提供专门路径：

```cpp
bool TryTakeOneRegionFastLocked(RegionIndex& idx);
```

原理：

- `num == 1` 是最常见场景，regular alloc 的 fresh-region / refill 基本都落在这里；
- 原通用逻辑要走 `ChunkSummary + probe + PreciseFitFromLocked()`，会有多级间接跳转和多次缓存行加载；
- 快速路径直接读取 `allocBits_[wordIdx]`，取反后用 `__builtin_ctzll` 找第一个 free bit；
- 对首 word 做 cursor mask，对尾 word 做 owned/committed mask，即可在 O(1) word 操作里得到结果。

在 shard 化后，这条优化收益更高，因为：

1. 每个 shard 初始只管理自己的 `64M` segment，局部 bitmap 范围明显更小；
2. local allocate 和 remote donor steal 都会频繁复用这条路径；
3. 它直接缩短了 shard 锁持有时间，因此能放大 shard 化本身的收益。

#### 优化 2：`MarkAllocatedLocked(num == 1)` 快速路径

建议对应提供：

```cpp
void MarkAllocatedOneLocked(RegionIndex idx, bool& reusedScavenged);
```

原理：

- 当前通用实现会对同一批 region 做 4 类工作：
  - `CountScavengedInRangeLocked`
  - `CountFreeInRangeLocked`
  - 逐 bit `SetBit/ClearBit`
  - `UpdateChunkSummaryRangeLocked`
- `num == 1` 时没必要做这 4 趟独立扫描；
- 只需定位一个 `wordIdx + bitMask`，读取一次 `allocBits_` 和 `scavengedBits_`，更新：
  - `freeRegionCount_`
  - `scavengedRegionCount_`
  - 单 bit 分配状态
  - 当前 chunk summary

也就是说，`num == 1` 场景要从“按 range 做通用处理”退化为“单 region O(1) 更新”。

#### 新设计中的落点

这两条优化都不应放在 `FreeManagerCoordinator` 层，而应作为 `FreeManagerShard` 内部实现细节：

- local shard 命中：走 `TryTakeOneRegionFastLocked + MarkAllocatedOneLocked`
- remote donor steal：donor shard 也走同一套 fast path
- `num > 1` 时才退回通用 `FindFreeRunLocked / MarkAllocatedLocked`

这样才能确保新架构在“减少锁碰撞次数”之外，也同步把“每次持锁的局部成本”压到最低。

#### 与批量 steal / 批量 reclaim 的关系

这两条 `num == 1` 快速路径不是孤立优化，而是 shard 内单锁模型的基础：

- `TakeRegion` 热路径尽量用 `ctz + 单 bit 更新` 缩短单次持锁时间；
- 这样 remote donor 被 batch steal 命中时，也能更快完成 donor 侧摘取；
- 批量 reclaim 合并回 shard 时，同样可以把单 region 的局部更新压成 O(1) fast path，再批量拼起来做。

## 5. 分配路径重构

### 5.1 regular alloc：本地 shard 优先

regular alloc 慢路径改造成：

```text
AllocBufferMS::SlowAlloc(cls)
  -> CentralRegionPool::GetFreshRegion(cls, shardHint=P_i)
  -> MSRegionManager::TakeRegion(cls, shardHint=P_i)
  -> FreeManager::TryTakeRegions(num=1, homeShard=P_i)
       1. receiver borrowed cache on shard_i
       2. local owned free on shard_i
       3. remote steal from other shards
       4. after 2 steal failures, grow one 64M segment for shard_i
```

行为要求：

1. `num == 1` 的请求绝不先走全局扫描。
2. 若 `borrowedRegions_` 非空，优先先消费 borrowed cache，再查本 shard owned free。
3. 查本 shard owned free 时，优先沿 `take-side frontier` 推进，不因一次新的 `free/reclaim` 立即回退到最新 free 的位置。
4. steal 时只锁 donor shard，不需要先锁 home shard 再锁 donor shard。
5. 连续 2 次 steal 失败后即触发 grow，不再继续拉长 donor 扫描链。

### 5.2 large alloc：也必须 shard-aware

large alloc 不能继续保留“单全局 page-heap 另一路”的旧模式，否则 high P 时 small / large 仍会在底层会合。

建议改成：

```text
RegionSpaceMS::TryAllocateLargeObjectOnce
  -> MSRegionManager::TakeRegionForLarge(unitCount, shardHint=P_i)
  -> FreeManager::TryTakeRegionsLarge(regionCount, homeShard=P_i)
       1. local shard_i contiguous run
       2. remote shard contiguous run steal
       3. after 2 steal failures, enter grow under one growMutex_ critical section
       4. if request > single-segment run limit: reserve requester-owned contiguous N segments or fail
```

这里 large alloc 和 regular alloc 共享同一套 shard 调度原则，但 large path 的查找条件是“连续 run”，不是“任意单 region”。

这里再明确一条边界：

- `borrowedRegions_` 只服务于 regular alloc / 单 region 批量偷回来的本地 borrowed cache；
- large alloc **不走 borrowed list**，而是单独走 local owned run / remote steal / local grow 这条路径。
- 若单次请求已经大于单个 `64M segment` 可提供的上限，则只允许在 requester shard 自己名下、地址连续的 owned segments 上组合满足。
- 这类 grow 必须在**一次** `growMutex_` 临界区内完成所需连续 `N` 个 segment 的预留与 owner 建立；若找不到这样的连续空间，则本次大对象分配直接失败，不退化成“跨 donor / 跨 segment 拼接借用”。

### 5.3 local hit / steal / grow 的来源都要显式记录

`TakeRegion()` 和 `TakeRegionForLarge()` 不应只返回成功与否，还应返回来源：

```cpp
enum class AllocSource : uint8_t {
    LOCAL_SHARD,
    REMOTE_STEAL,
    GROW,
};
```

这样后续：

- DFX 统计能区分“本地供给不足”还是“总体供给不足”；
- `AllocBufferMS` 可以根据来源调节 refill 策略；
- case25 / case27 的 sample 能直接看到 tail latency 是不是被 steal / grow 放大。

## 6. 跨 Shard Steal 协议

### 6.1 steal 的基本规则

steal 只在本地 shard miss 后发生，且目标是“偷 free span”，不是“抢别人的活跃 region”。

规则建议如下：

1. 仅 donor shard 持有自己的 `shardMutex_`。
2. receiver shard 不参与 donor 锁保护，避免双锁死锁。
3. steal 以 batch 为单位搬运 free span，优先用一次 donor 锁获取换回多次后续分配命中。
4. `num == 1` 和 `num > 1` 使用不同 donor 选择策略。
5. 同一请求在连续 2 次 steal 失败后，必须立即升级到 grow。

### 6.2 `num == 1` 的 steal

对单 region 请求，优先目标是“最快拿到一块”。

建议策略：

- 先 round-robin 扫描其他 shard；
- 可以从“上一次 steal 成功的 donor”之后继续扫，避免每次都从 0 开始；
- 命中第一个 `maxFreeRun >= 1` 的 donor 后，不是只取 1 个 region，而是批量偷一个小批次；
- donor 本地仍使用自己的 `FindFreeRunLocked(num=1)` fast path。

建议第一版把 `num == 1` 的批量偷定义成固定小批次：

```text
stealBatchRegionsForSingle = 4
```

即：

- 当前请求只需要 1 个 region；
- 但 donor shard 一旦命中，就一次摘取 `4~8` 个 region 的小 span；
- 其中 1 个立即返回给当前请求；
- 其余挂到 receiver shard 的 `borrowed region list` 中，供后续同一 P 的 fresh-region 请求直接复用。

这里的关键约束是：

- donor 在借出时就把这批 region 从自己的 free 结构中摘掉；
- donor 的 free / hotFree 口径直接扣减；
- donor 计数上把这批 region 当作“已使用”，不再把它们视为自己当前可再次分配的 free 供给；
- borrower 只把它们当作“本地可消费的借入缓存”，不获得 owner。

这里的“被偷走标记”不建议单独再做一张全局 bitmap。

第一版里，“这批 region 已经被借走、donor 不能再用，只能 borrower 用”的语义，直接由两件事保证：

1. donor 已把它从自己的 free 结构中摘掉；
2. borrower 已把它记进自己的 `borrowed region list`

这就已经足够阻止 donor 再次分配它们。

这里不需要为了 `num == 1` 做全局最优 donor 选择，因为：

- 单 region 请求数量最多；
- donor 选择计算本身如果太重，会反过来吃掉 shard 化收益；
- 而“命中 donor 后批量偷一小段”正好可以摊薄 donor 锁竞争。

### 6.3 `num > 1` 的 steal

对 large alloc 或 batch refill，请优先找“大 run 最丰富”的 donor。

建议：

- 先读各 shard 的无锁 snapshot：`maxFreeRunRegions`、`freeRegionCount`；
- 按 `maxFreeRunRegions` 从大到小选择 donor；
- 只对满足 `maxFreeRunRegions >= num` 的 donor 尝试加锁和精确查找。

对于 `num > 1`，steal 的基本单位不再是“单 region 批次”，而是“满足本次请求的最小连续 span”。

第一版这里再明确一条边界：

- 连续 span **不允许跨多个 `64M segment` 拼接**
- donor 必须在单个 owned segment 内提供完整 run
- remote steal 不做跨 segment 的 run 拼接、borrowed span 拼接或 multi-segment steal merge

但对**单次请求已经大于单个 `64M segment` 可提供上限**的情况，第一版不直接失败，而是：

1. 不去远端借这么大的跨 segment run；
2. 由 requester shard 在 grow 后，使用自己名下地址连续的多个 owned segment 组合满足；
3. 这种组合只发生在 requester shard 自己的 owned segment 集合内，不发生在 remote donor 侧。

这样能显著减少“明知 donor 不够大还去拿锁”的无效竞争。

这里也顺带明确 donor 选择原则：**不是看“哪个 shard 当前被使用得最少”**。

原因是“被用得少”并不等于“最适合借”：

1. 它看不出连续 run 够不够大；
2. 它会被 borrowed 扣减口径干扰；
3. 它无法区分“总 free 多但很碎”和“总 free 不多但正好有大 run”。

因此第一版 donor 选择按下面两类做：

1. `num == 1`
   - 以便宜为先：round-robin 或基于上次成功 donor 的轻量轮转
   - 只要求 donor 快速满足 `maxFreeRun >= 1`
2. `num > 1`
   - 以连续供给能力为先：优先看 `maxFreeRunRegions`
   - 同等条件下再用 `ownedHotFree / ownedFree` 做次级 tie-break

### 6.4 steal 后 owner 不转移

Step23 第一版明确采用：

```text
steal = borrow
owner 不转移
```

也就是说：

- donor 借出 free span 后，只把它从自己的 free 结构里摘掉；
- donor 的 free / hotFree 统计直接扣减，并把这批 region 视作“已使用”；
- borrower 只在本地 `borrowed region list` 中维护它们，用于后续本地分配；
- borrower 在 `GC / DFX` 日志里要把这部分单独记成 `borrowed` 列，而不是并入 `free / hotFree`；
- 这些 region 未来再次 free 时，reclaim 仍按原 owner shard 路由。

`borrowed` 状态的移除时机也要写清楚：

1. borrower 真正消费某个 borrowed region 时
   - 从 `borrowed region list` 弹出
   - 进入正常“已分配 region”状态
2. 该 region 之后再次 free 时
   - small/empty 路径按 owner shard 回到 `emptySwept`
   - large 路径按 owner shard 回到 `largeHotCache`
   - 再由 `BackgroundScavengeLoop()` 的 Step A 批量回到 owner shard `hotFree`

也就是说，第一版**不额外设计“未消费 borrowed region 主动归还 owner”机制**。

batch steal 带回来的额外 borrowed-free region，如果暂时还没用掉，就继续留在 borrower 的本地 borrowed cache 中；只有当它真正被分配、后续又被释放时，才会自然按 owner shard 回流。

因此，“borrowed-first” 这条优先级只收紧在 regular alloc / 单 region borrowed cache 复用上，不扩展到 large object 路径。

这样做的目的不是追求最精确的实时统计，而是让：

1. owner 语义固定不变；
2. donor 不再把借出的 span 误当成自己仍可用的 free 供给；
3. borrower 可以在本地复用借来的 region，而不需要获得真正归属。
4. 新近 free 的 region 不会在同一 shard 内立刻回到最热 take 前沿，继续保留当前“分配/回收双端推进”的局部性收益。

## 7. Grow / Shrink 重构

### 7.1 grow：由触发请求的 shard 获得新的 64M segment

当某次请求在本地 shard miss 后连续 2 次 steal 失败时，触发全局 grow。

建议流程：

```text
Grow(requesterShard, neededRegions)
  -> lock growMutex_
  -> if neededRegions <= single-segment limit:
       commit one 64M segment for requesterShard
  -> else:
       reserve and commit requester-owned contiguous N * 64M segments
  -> segmentOwnerShard = requesterShard
  -> 初始化 requesterShard 本地 SegmentMeta
  -> requesterShard 再次本地 TryTake
```

grow 的最小单位始终是 `64M`。

这里需要和上面的 steal 边界一起看：

1. remote donor 不负责提供跨 segment 的大 run；
2. 但 requester shard 若因为单次大请求而在一次 grow 中连续拿到多个相邻 `64M segment`，可以在**自己名下**把这些相邻 owned segment 组合起来满足这次分配；
3. 这一步必须在同一把 `growMutex_` 保护下原子完成“找连续空间 -> commit -> 建 owner”，不能分批松锁拼出来；
4. 若当前保留区里不存在满足请求的连续空间，则本次 grow 失败，分配也随之失败。

这样 grow 后的新增供给直接回到缺货方，避免 grow 完还要再走一次 remote steal。

### 7.2 shrink：只收缩堆尾完整空闲 segment

shrink 必须比 grow 更保守。

建议规则：

1. 只检查 committed heap 尾部的完整 `64M segment`。
2. 尾 segment 必须全部 free 且全部 scavenged，才允许 shrink。
3. 若尾部存在部分已用 region，则立即停止，不尝试跨 shard 拼接。

这样虽然牺牲一部分激进回收能力，但可以确保：

- shrink 不需要重排中间 segment 的 owner；
- 不需要引入复杂的跨 shard compaction；
- 与当前 `ShrinkHeap()` 的“尾部回收”语义一致，只是从单全局改成带 owner 判断。

### 7.3 grow / shrink 的核心边界

`growMutex_` 仍然是全局的，但它只保护：

- `committedEnd_`
- `committedRegionCount_`
- `segment ownership table`
- `64M segment` 的 commit / uncommit 边界

不再保护普通 free-span 查找和回收。

这是 shard 化之后最重要的边界之一：**把“容量边界管理”与“日常供给管理”彻底分开。**

## 8. Scavenge / Reclaim 重构

### 8.1 reclaim：按 owner shard 回收

第一版里的 `reclaim`，不再定义成“direct released spans 直接打回 page-heap”。

而是继续沿用当前两条来源，只把它们改成 owner-shard 批量化：

```text
emptySwept
  -> drain global cache
  -> group by owner shard
  -> batch reclaim to owner shard hotFree

largeHotCache
  -> drain global cache
  -> group by owner shard
  -> batch reclaim to owner shard hotFree
```

这样 empty region 和 large cache 里的 region 都会回到正确 shard，但**mutator 本身不执行真正的 reclaim**。

reclaim 分两段：

1. submit 阶段
   - 生产者：`sweep` / `CentralRegionPool`
   - 动作：small empty region 进入 `emptySwept`，large region 进入 `largeHotCache`
   - 唤醒：沿用当前已有的 `NotifyToScavenge / RequestWork / Notify`

2. execute 阶段
   - 消费者：当前 `BackgroundProcessor` 的单后台线程
   - 动作：在 `BackgroundScavengeLoop()` Step A 中，先从全局 cache drain，再按 owner shard 分组，最后批量进入对应 shard 的 `hotFree`

这样一个 shard 只获取一次 `shardMutex_`，就在锁内批量：

- 更新 `allocBits_ / scavengedBits_`
- 合并相邻 free span
- 更新 chunk summaries
- 将这批新近 free span 挂入 `reclaim-side frontier`，而不是直接把 `take-side frontier` 回退到最新 free 位置

而不是“每回收 1 个 region 就单独拿一次锁”。

这也意味着 allocation 热路径里不再保留任何“顺手 reclaim 一把”的同步补救逻辑。

### 8.2 scavenge：每个 shard 独立做 hot-free -> scavenged

当前 `ScavengeFreeRegionsUpTo(maxBytes)` 是单全局扫描。

改造后建议：

- 后台线程用全局 retained threshold 决定“要不要 scavenge”；
- 第一版只保留**一个全局后台线程**负责轮询所有 shard，并沿用当前 `BackgroundProcessor::Run()` 中 sweep 与 scavenge 共线程处理的方案；
- 真正执行时按 shard round-robin 发预算：

```text
budget -> shard0
budget -> shard1
...
```

这里明确采用 **try-lock scavenging**，而不是让 scavenger 与分配热路径硬抢锁：

- 后台线程挑中目标 shard 后，先 `try_lock(shardMutex_)`；
- 若失败，说明该 shard 当前正忙于 `TakeRegion` / reclaim Step A；
- scavenger 立即跳过这个 shard，转去下一个候选 shard 或等待下一轮。

这样可以保证：

1. scavenger 不会因为等待 shard 锁而把后台线程卡成前台尾延迟放大器；
2. 分配热路径优先级天然高于物理页回收；
3. 全局后台线程实现简单，先把 owner-shard 语义与 try-lock 模型打通，再决定后续是否扩展为多 helper。

### 8.3 empty region / large hot cache 的 shard-aware 语义

这里再明确一次：`CentralRegionPool` 本身仍保持单逻辑实例，不做彻底 shard 化；但以下几条 owner-shard 语义必须改：

1. `ReclaimEmptyRegionToHotFree(region)` 必须通过地址反查 `segmentOwnerShard_`，把 empty region 回到对应 shard。
2. `ReclaimRegionLarge(regionStart, regionBytes)` 必须按 large span owner shard 归还。
3. 后台 scavenge 完成后，统计要能区分“是哪个 shard 释放了多少物理页”。

这三条不改，page-heap 已经 shard 化，上层 reclaim 仍会把结果重新打回单全局视角。

## 9. 与现有模块的接口改造

建议最少改下面几组接口：

1. [MSFreeManager.h](/Users/lipeisheng/code/cangjie/cangjie/cangjie_runtime_MSGC/runtime/src/Heap/MAllocator/MSFreeManager.h:76)
   - `TryTakeRegions(RegionCount num, RegionIndex& idx)`
   - 改成 `TryTakeRegions(RegionCount num, uint16_t homeShardId, RegionIndex& idx, AllocSource* source)`

2. [MSRegionManager.h](/Users/lipeisheng/code/cangjie/cangjie/cangjie_runtime_MSGC/runtime/src/Heap/MAllocator/MSRegionManager.h:87)
   - `TakeRegion(uint8_t cls, const CentralRegionPool& centralPool)`
   - `TakeRegionForLarge(size_t unitCount, uintptr_t& regionStart)`
   - 都增加 `shardHint`

3. [MSAllocBuffer.cpp](/Users/lipeisheng/code/cangjie/cangjie/cangjie_runtime_MSGC/runtime/src/Heap/MAllocator/MSAllocBuffer.cpp:355)
   - `SlowAlloc(cls)` 需要从当前 P 取得 `homeShardId`
   - refill 结果需要知道是 `LOCAL_SHARD / REMOTE_STEAL / GROW`

4. [MSCentralRegionPool.h](/Users/lipeisheng/code/cangjie/cangjie/cangjie_runtime_MSGC/runtime/src/Heap/MAllocator/MSCentralRegionPool.h:344)
   - `ReclaimEmptyRegion(size_t maxRegions)` 内部 reclaim 路由必须 shard-aware
   - `GetFreshRegion(cls)` 建议扩成 `GetFreshRegion(cls, shardHint)`

5. 背景线程路径
   - `BackgroundScavengeLoop()`
   - `MaybeNotifyScavenge()`
   - `GetHotFreeBytes()`
   - 这些接口需要升级为 shard-aware；具体统计字段后续单独讨论

## 10. 统计口径（暂缓）

本 Step 先不展开 `reclaim / scavenge / hotFree / steal / grow` 的新统计口径设计。

当前先固定四件事：

1. owner 路由以 `segmentOwnerShard_` 为准；`reclaim / scavenge / shrink` 都按 owner shard 组织。
2. `ownedFree / ownedHotFree / ownedScavenged / maxFreeRun` 只反映该 shard 主 free 结构中的供给，不把借入缓存并进去。
3. `borrowed` 单独成列，表示 receiver shard 当前 `borrowed region list` 中可直接消费的借入 region 数，不并入 `free / hotFree`。
4. donor 在借出时立即扣减自己的 `ownedFree / ownedHotFree`；第一版只要求这些计数足够用于压力判断和日志观测，不要求做全局精确守恒。

### 10.1 GC 日志最小落点

为了和当前 `GC` 日志主链对齐，shard 信息第一版至少接入三处：

1. `GC START` 快照
   - 只打印 `ownedFree / hotFree / borrowed` 至少有一项非 0 的 shard
   - 对命中的 shard 打印 `ownedFree / hotFree / scavenged / borrowed / maxFreeRun`
2. `region-accounting` / DFX 汇总
   - 在 `DumpRegionStatsMS()` 末尾追加同一份“仅非 0 shard” snapshot，便于对比 `GC` 前后变化
3. `BackgroundScavengeLoop()` 汇总
   - 除总 `released bytes` 外，再打印本轮 touched shard 的 `releasedBytes / hotFreeAfter / borrowed`

建议日志格式：

```text
[MS-GC]   shard[3]: ownedFree=12 regions, hotFree=8 regions, scavenged=4 regions, borrowed=6 regions, maxFreeRun=5
[MS-GC]   shard[3] scavenge: released=64MB hotFreeAfter=2 regions borrowed=6 regions
```

这里明确两条约束：

1. `borrowed` 必须单独成列，不能并入 `free / hotFree`，否则日志会把“本地 owned 供给”和“借入缓存”混成一个数。
2. 在 `FreeManagerShard` 真正落地前，不要从当前单全局 `FreeManager` 人工拆出伪 `shard0..N` 快照；日志必须直接基于真实 per-shard 元数据与 `borrowedRegions_` 输出。

## 11. 推荐落地顺序

### Phase 1：先把类型和接口改成 shard-aware

1. 引入 `FreeManagerShard`
2. 引入 `segmentOwnerShard_`
3. `shardCount = cjProcessorNum`
4. 初始化 committed heap 改为 `cjProcessorNum * 64M`
5. regular alloc 带 `homeShardId`

### Phase 2：先打通 regular alloc 的 local / steal / grow

1. `TryTakeRegions(num=1)` 本地 shard 化
2. donor steal
3. 连续 2 次 steal 失败后 grow
4. grow 后 `64M segment` 分配给 requester shard

### Phase 3：large alloc、reclaim、scavenge 一起并入 shard

1. `TakeRegionForLarge()` shard-aware
2. `ReclaimRegionsReleased()` 按 owner shard 路由
3. `ScavengeFreeRegionsUpTo()` 拆为 per-shard budget

### Phase 4：shrink 和统计口径收尾

1. 尾 segment shrink
2. `GetFinalStats()` / DFX 日志改成 shard 汇总
3. case25 / case27 / sample 验证 steal / grow / scavenge 的热点变化

## 12. 风险、取舍与非目标

### 12.1 主要风险

1. steal 成功率高时，remote borrow 会带来一定碎片重分布，需要后续观察 large alloc 的大 run 质量。
2. grow 绑定 requester shard 后，长期可能出现 shard 间容量倾斜，需要依赖 steal 和后续 rebalance 才能均衡。
3. shrink 只看堆尾完整空闲 segment，回收会比单全局理想模型更保守。

### 12.2 主要取舍

- 本设计优先解决 `64P+` 下的锁碰撞与吞吐问题，不优先追求最激进的全局最优碎片率。
- 第一版优先把 `FreeManager` 和其直接上下游做 shard-aware；`CentralRegionPool` 的 bucket / empty cache 保持单逻辑实例，不作为本 Step 或默认后续的必做 shard 化项。

### 12.3 非目标

本 Step 不包含：

1. moving / compaction 语义；
2. 基于 NUMA 的多层 shard 层次；
3. 中央池 full / partial bucket 的完全 lock-free 化；
4. steal 后的复杂后台 rebalance 守护线程。

本 Step 的终点是：**让 `FreeManager` 及其 large alloc / grow / shrink / scavenge / stats 主链条都变成 shard-aware，并且 shard 数量固定等于 P 数量。**
