# MSGC Scavenge Hot-Word 查找与物理页归还优化设计

## 1. 状态与目标

状态：设计已于 2026-07-26 经用户确认，等待书面规格复核后进入实现计划。

平台优先级：

1. Linux AArch64 高核生产场景是性能验收主平台。
2. macOS AArch64 只要求构建、功能和并发语义回归。

目标：

- 让 hot-free 查找成本跟实际含 hot-free 的 Segment/bitmap word 接近，而不是跟全部
  committed Region 接近。
- 避免默认 8 个 Scavenge quantum 反复扫描已经变成 cold-free 的尾部 bitmap。
- 把 claim/complete 从逐 Region 更新改为按 bitmap word 批量更新，缩短 shard lock 持有时间。
- 生成最大连续地址 span，确保 Linux `madvise` 调用数不超过安全可合并区间数。
- 分开观测查找、claim、OS release 和 complete 成本。

非目标：

- 不修改 Scavenge threshold、resident reserve、release target 和 `cjMSScavengeRounds`。
- 不引入并行 Scavenge，不与 Mark/Sweep 抢占更多线程和内存带宽。
- 不改变 allocator 对 hot-free/cold-free Region 的复用顺序和 cold-reuse 统计语义。
- 本轮不删除 macOS `MemorySet + MAP_FIXED mmap`；只补齐分阶段计时，为后续独立验证提供依据。

## 2. 当前流程与根因

当前流程：

```text
PostMarkTermination NotifyToScavenge / 每约 2 秒周期检查
  -> GetScavengeReleaseTargetBytes()
  -> large hot cache、empty-swept 按缺口转入 FreeManager hot-free
  -> target 按 cjMSScavengeRounds（默认 8）计算 quantum
  -> ScavengeFreeRegionsByRegionBudget()
  -> 从 nextScavengeShard_ 开始轮转 shard，并 try_lock
  -> ownedViews_ 反向遍历，每个 view 的 bitmap word 反向遍历
  -> 在 shard lock 内 claim hot-free span
  -> 锁外调用 ReleaseUnits()
  -> 重新加锁发布 cold-free，失败则恢复 hot-free
```

Region 状态语义：

| 状态 | `alloc` | `scavenged` | `scavenging` |
|---|---:|---:|---:|
| hot-free | 0 | 0 | 0 |
| OS release in-flight | 1 | 0 | 1 |
| cold-free | 0 | 1 | 0 |
| allocated | 1 | 0 | 0 |

当前 hot-free word 通过
`(~allocBits_[wi]) & (~scavengedBits_[wi]) & validMask` 现场计算。`alloc=1` 的
in-flight Region 不会被分配器或另一个 Scavenge slice 再次领取。

主要规模问题：

- 只有 `nextScavengeShard_`，没有 view/word cursor。每个 quantum 都从 shard 的最后一个
  view、最后一个 word 重新开始。
- `ChunkSummary` 只描述 alloc/free 连续区间，不描述 free Region 是否已经 scavenged；全 cold
  Segment 仍需逐 word 扫描。
- 找到 span 后再次调用 `MarkScavengingLocked()` 逐 Region claim；complete 也逐 Region 更新。
- 每个 span 都可能触发 `UpdateChunkSummaryRangeLocked()` 和整 view 的 `maxFreeRun_` 重算。
- `BackgroundScavengeLoop()` 一次 slice 内多次取得 hot-free/retained 快照；其中部分是 `O(P)`
  聚合。这不是 committed scan，但在 P=128 下需通过统计单独识别。

关键源码入口：`BackgroundProcessor::BackgroundScavengeLoop()`、
`RegionSpaceMS::GetScavengeReleaseTargetBytes()`、
`FreeManager::{ScavengeFreeRegionsByRegionBudget,ClaimScavengeSpansLocked,
ClaimScavengeSpansInViewLocked,MarkScavengingLocked,CompleteScavengingLocked,ReleaseUnits}`。

## 3. 方案比较与选择

| 方案 | 优点 | 缺点 |
|---|---|---|
| 仅增加 view/word cursor | 改动小，避免每轮从尾部重启 | 完整环绕时仍扫描所有 cold word |
| hot-word summary + cursor | O(1) 跳过 cold Segment/word，状态由现有 shard lock 保护 | 所有状态转换点都必须维护 summary |
| hot-word 候选队列 | steady state 接近直接消费候选 | 需要处理去重、stale entry、grow/shrink 和队列生命周期 |

选择第二种方案。生产配置中：

```text
64MiB / 32KiB = 2048 Regions / Segment
2048 / 64 = 32 bitmap words / Segment
```

因此一个 `uint64_t` 即可作为 per-view hot-word summary，内存和维护成本远低于候选队列。
cursor 解决跨 slice 进度和公平性，summary 解决 cold metadata 跳过。

## 4. 数据结构与不变式

新增私有字段：

```cpp
struct OwnedSegmentView {
    ...
    uint64_t hotWordMask_ = 0;
};

struct FreeManagerShard {
    ...
    size_t scavengeViewCursor_ = 0;
    size_t scavengeWordCursor_ = 0;
};
```

`BuildOwnedSegmentView()` 检查 `bitmapWordCount_ <= 64`。当前生产配置为 32；测试中的较大
Region 对应 word 更少。若未来 Segment/Region 配置突破 64 words，应把 summary 扩展为多级 bitset，
而不是静默截断。

统一维护函数：

```cpp
uint64_t GetValidRegionMask(const OwnedSegmentView& view, size_t wordIdx);
uint64_t GetHotBits(const OwnedSegmentView& view, size_t wordIdx);
void RefreshHotWordSummaryLocked(OwnedSegmentView& view, size_t wordIdx);
```

必须在以下路径维护对应 word：

- view 初始化和扩堆 attach；
- 单 Region、连续 Region 分配；
- Region reclaim/free；
- Scavenge claim；
- Scavenge success/failure complete；
- Region unavailable；
- Segment detach 和 cursor clamp。

核心不变式：

```text
((hotWordMask_ >> wi) & 1) == 1
  <=> ((~allocBits_[wi]) & (~scavengedBits_[wi]) & validMask) != 0
```

`hotWordMask_` 和 cursor 只在所属 `shardMutex_` 下读写，不新增原子操作。claim 必须先在锁内把
Region 转成 `alloc=1, scavenging=1`，才允许锁外执行 OS release。complete 必须验证该状态仍然
存在；成功发布 cold-free，失败恢复 hot-free。

`ownedViews_` grow 只追加，已有 cursor 保持有效；tail detach 后在同一锁域内把 cursor clamp
到剩余 view/word 范围。空 shard 的两个 cursor 归零。

## 5. 查找与 Claim 算法

每个 shard 从保存的 `(viewCursor, wordCursor)` 开始反向环形扫描，最多完整环绕一次：

1. `view.hotWordMask_ == 0` 时 O(1) 跳过整个 view。
2. 对 cursor 前方的 summary bits 使用 `clz` 定位下一个 hot word，无需读取 cold word 的三套 bitmap。
3. 读取精确 hot bits；按反向地址顺序选择不超过剩余 budget 的 bits。
4. 扫描时直接完成 word-mask claim，不再生成 span 后逐 Region 调用 `MarkScavengingLocked()`。
5. budget 用完时保存当前 view/word；word 中仍有 hot bits 时，下轮仍从该 word 继续。
6. 完整环绕未找到可 claim Region时返回；try-lock 失败的 shard 由现有 shard 轮转机制下轮重试。

word claim：

```cpp
view.allocBits_[wi] |= selectedMask;
view.scavengingBits_[wi] |= selectedMask;
size_t selected = PopCount(selectedMask);
shard.freeRegionCount_ -= selected;
shard.scavengingRegionCount_ += selected;
```

claim 后刷新该 word 的 hot summary 和 alloc `ChunkSummary`。同一 view 在本批所有 word 处理完成后
只计算一次 `maxFreeRun_`。整个 shard claim 完成后只调用一次
`RefreshRegionSnapshotsLocked()`。

该算法不承诺优先选择最低或最高地址，只保持反向轮转和不饥饿。Scavenge 的目标是物理页归还，
地址顺序不能作为 allocator 正确性契约。

## 6. Span 合并、OS Release 与 Complete

选中的 hot bits 通过统一 `AppendScavengeSpan()` 转成连续地址 span。反向扫描时若
`new.end == previous.start`，合并为一个更大的 span。合并覆盖 word 边界，也允许地址连续时跨
view；不得跨 allocated/cold/in-flight Region。

两阶段发布保持不变：

```text
claim（持 shard lock）
  -> 解锁
  -> 对每个最大连续 span 执行 ReleaseUnits()
  -> 重新加锁
  -> complete success/failure
```

Linux AArch64 继续使用 `madvise(MADV_DONTNEED)`，一个最终 span 对应一次系统调用。不引入
Linux 特有的批量新 syscall，不改变页重新访问时的零页语义。非连续 span 不能安全合并，因为中间
可能包含活对象页面。

complete 按 span 覆盖的 bitmap word 批量执行：

```cpp
view.allocBits_[wi] &= ~completedMask;
view.scavengingBits_[wi] &= ~completedMask;
if (released) {
    view.scavengedBits_[wi] |= completedMask;
}
```

成功时计入 `scavengedRegionCount_`；失败时不设置 scavenged bit，Region 自动恢复为 hot-free。
两者都减少 `scavengingRegionCount_` 并恢复 `freeRegionCount_`。随后刷新 touched word 的
hot summary/`ChunkSummary`，每个 touched view 只重算一次 `maxFreeRun_`，最后只刷新一次 shard
快照。

macOS 复用同一查找、claim、complete 和统计，但保留现有 `MemorySet + MAP_FIXED mmap`。
是否删除 `MemorySet` 必须依据独立计时和平台验证另立设计，不能混入本轮 Linux 优化。

## 7. 可观测性

保留现有字节返回值，通过可选输出参数透传 slice-local 统计：

```cpp
struct ScavengeSliceStats {
    size_t requestedBytes = 0;
    size_t claimedRegions = 0;
    size_t releasedRegions = 0;
    size_t visitedShards = 0;
    size_t lockMissShards = 0;
    size_t visitedViews = 0;
    size_t skippedColdViews = 0;
    size_t visitedHotWords = 0;
    size_t cursorWraps = 0;
    size_t candidateRuns = 0;
    size_t releaseCalls = 0;
    size_t releaseFailures = 0;
    uint64_t scanClaimUs = 0;
    uint64_t releaseUs = 0;
    uint64_t completeUs = 0;
};

size_t ScavengeFreeRegionsUpTo(size_t maxBytes, ScavengeSliceStats* stats = nullptr);
```

统计由调用方提供的栈对象承载，不引入全局原子热计数。`stats == nullptr` 时跳过可选计时和
诊断计数；`BackgroundScavengeLoop()` 在 REPORT 开启时传入 stats，每个实际 release slice 最多
输出一条日志，并同时打印上层的总 target 与本轮 quantum。

必须区分：

- `candidateRuns`：合并前发现的连续 run；
- `releaseCalls`：最终最大连续 span 数；
- `scanClaimUs`：summary 查找和锁内 claim；
- `releaseUs`：纯 OS release；
- `completeUs`：重新加锁后的状态发布。

这样才能判断瓶颈是 metadata scan、锁竞争、碎片导致的 syscall 数，还是内核物理页回收。

## 8. 修改范围

生产源码：

- `src/Heap/MAllocator/MSFreeManager.h/.cpp`
  - summary/cursor、word claim/complete、span 合并、slice stats。
- `src/Heap/MAllocator/MSRegionManager.h/.cpp`
  - 向 FreeManager 透传可选 stats。
- `src/Heap/MAllocator/MSRegionSpace.h/.cpp`
  - 向后台线程透传可选 stats。
- `src/Heap/MCollector/BackgroundProcessor.cpp`
  - 输出单条 slice REPORT。

测试：

- `test/MSFreeManagerShardStatsTest.cpp`
  - 状态、游标、summary、span、并发、失败回滚和 reference 对拍。
- 如现有测试文件过大，可新增一个只访问 FreeManager 私有测试接口的专用测试文件，并接入原有
  Runtime test target；不为测试暴露生产 public API。

知识文档：

- 更新 Cangjie Runtime skill 的 GC collector 文档，记录落地后的真实结构和复杂度。

明确不修改：

- `MSRegionInfo` 和 compiler/runtime ABI；
- Scavenge threshold policy；
- Mark/Sweep phase 和 helper 数；
- CopyGC。

## 9. 测试与性能验收

单元测试至少覆盖：

1. fresh/partial view 的 `hotWordMask_` 和 valid mask。
2. allocate、reclaim、Scavenge success/failure、cold reuse 后 summary 与完整 bitmap scan 一致。
3. 连续 Region 跨 word 时只生成一个 release span。
4. 碎片 Region 不跨 allocated Region 错误合并。
5. 多个小 quantum 延续 cursor，不从尾部重新扫描。
6. 全 cold view 被 O(1) 跳过。
7. grow、tail detach 后 cursor 初始化/clamp。
8. shard try-lock 跳过与 shard 轮转保持原语义。
9. release failure 完整恢复 hot-free。
10. OS release in-flight Region 不可被 allocator 取得。

增加固定 seed 的随机状态机测试，混合 allocate、reclaim、Scavenge success/failure；每一步将
summary、free/scavenged/scavenging 计数和优化查找结果与逐 Region reference scan 对拍。

保留并重新运行现有 FreeManager 测试，尤其是：

- release 在 shard lock 外；
- failure rollback；
- claimed Region 不可并发分配；
- cold reuse 只计数一次；
- owner shard 公平轮转。

Linux AArch64 性能矩阵使用相同 compiler/runtime/stdlib、相同 case27 配置，对
P=8/32/64/128 各运行至少 3 次。替换 Runtime 后必须重新编译 `.cj` 用例，并在干净 SDK 环境中
核对实际加载的 dylib/so。

验收门槛：

- P128 稀疏 hot-free 微基准的 visited bitmap words 至少下降 90%。
- case27 `scanClaimUs / releasedGiB` 中位数至少下降 50%。
- `releaseCalls` 等于本轮 selected Region 集合中的安全最大连续空闲区间数，且不高于基线。
- `releaseUs / releasedGiB` 不回退超过 5%。
- HTTP 成功吞吐不下降超过 3%，p99 不恶化超过 5%。
- RSS 峰值不增加超过 3%，结束 RSS 持平或下降。
- GC/OOM 次数、released bytes、cold-free 和 cold reuse 趋势不出现稳定回退。

macOS AArch64 只执行构建、FreeManager 测试和基础 `.cj` GC 回归，不以 page-release 吞吐作为
本轮验收条件。

## 10. 风险、回退与实施顺序

主要风险：

- 某个状态转换点漏刷 `hotWordMask_`，导致 hot-free 暂时不可见或重复 claim。
- partial word 的无效 bit 被误判为 hot。
- tail detach 后 cursor 越界。
- 合并 span 跨过 allocated Region，造成活页被丢弃。
- success/failure 混合 complete 时计数或 summary 漂移。
- summary 维护增加前台 allocator 的 shard-lock 时间，抵消后台收益。

实现使用测试驱动顺序：

1. 先补 reference helpers、summary/cursor 单测和扫描量统计，确认旧实现无法满足新断言。
2. 实现 `hotWordMask_` 及所有状态转换维护，先通过 summary 对拍。
3. 实现环形 cursor 和 summary-guided scan。
4. 将 claim/complete 改为 word 批处理，并实现最终 span 合并。
5. 透传 slice stats，补充单条 REPORT。
6. 完成 FreeManager 测试、Runtime 构建和 macOS 功能验证。
7. 在 Linux AArch64 做 P 梯度基线/优化对比。
8. 源码行为稳定后更新 Runtime GC 知识文档。

出现以下任一情况即停止默认启用并回退到现有扫描：

- summary/reference 不一致；
- release in-flight Region 被重新分配；
- failure rollback 丢失 Region；
- 前台分配锁等待显著增加；
- RSS、cold reuse、吞吐或延迟超过验收回退线。

设计刻意不引入运行时双路径开关；reference scan 仅用于测试/Debug 对拍。生产回退以独立提交为
边界，避免长期维护两套 Scavenge 状态机。
