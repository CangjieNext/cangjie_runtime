# Step58 MSGC 高 P 并发 Mark 数据局部性、Bitmap 竞争与任务分区优化方案

## 1. 结论摘要

高 P 大堆下，MSGC 的并发 Mark 不能只靠增加 Helper 解决。case27 当前实测中，
P5 的总 Mark 速率为 **30.167 Mobj/s**，P50/P100/P150 分别只有
**12.683/11.390/11.158 Mobj/s**；只统计自然收敛周期，高 P 也只有
**11.532～13.223 Mobj/s**。Helper 采样负载均衡、Global WorkBuf 锁等待已降到约
1%～2%，而历史 4 个无 Assist 后台线程相对单线程仅有 **1.779x** 加速。因此当前首要矛盾是：

1. 大对象图使对象、TypeInfo、RegionInfo 和 mark bitmap 访问范围显著扩大；
2. 每条白引用都独立执行 bitmap 原子 `fetch_or`，同 word 竞争和相邻 word false sharing
   限制多线程扩展；
3. 已发布灰对象按通用 WorkBuf 流转，缺少显式地址亲和分区，Worker 会在大堆地址空间中跳转。

推荐按以下顺序实施，而不是一次完成所有结构性改造：

1. 建立唯一的 Mark bitmap 逻辑 word 访问层，保持连续布局，先消除物理寻址散点；
2. 在 GC 字段扫描中引入固定容量 `MarkWordBatcher`，把同一 word 的多个 bit 合成一次原子提交；
3. 引入 Region-affinity `MarkLaneSet`，让 Worker 优先连续处理相近堆地址的灰对象；
4. 在前三步数据证明相邻 word ownership 迁移仍是瓶颈后，再同步切换 Runtime/LLVM
   到 8-lane striped bitmap。

其中第 2 步直接减少原子 RMW 次数，第 3 步改善对象和元数据局部性，第 4 步降低相邻
bitmap word 的 cache-line false sharing。三者作用不同，不能互相替代。

## 2. 背景与现状

### 2.1 高 P Mark 实测

| 指标 | P5 | P50 | P100 | P150 |
|---|---:|---:|---:|---:|
| GC 周期数 | 561 | 22 | 23 | 21 |
| Mark 总速率（Mobj/s） | 30.167 | 12.683 | 11.390 | 11.158 |
| Natural Mark（Mobj/s） | 30.164 | 12.683 | 11.532 | 13.223 |
| Assist 工作占比 | 14.28% | 76.32% | 72.45% | 44.75% |

高 P 单轮标记对象数约为 P5 的 16.8～22.3 倍。P5 平均约 182B/live object，
高 P 约 103～105B/live object，即相同存活字节中需要解析和标记的对象更多。

同一个 P50 进程中，小轮与大轮也表现出明显差异：

- cycle 7：约 233 万对象，22.58 Mobj/s；
- cycle 11：约 2313 万对象，9.50 Mobj/s；
- cycle 12：约 2034 万对象，9.97 Mobj/s；
- cycle 9：约 2427 万对象，11.70 Mobj/s。

该对照保持 P、Runtime、线程策略和进程一致，说明工作集放大本身就会降低单位对象扫描速率。
完整数据见：

- `MSGC_RESULT/Implementation/Step52_MSGC并发Mark效率瓶颈采样分析与优化优先级.md`
- `MSGC_RESULT/Implementation/Step56_MSGC_P5_P50_P100_P150_Mark_Sweep_Scavenge速率对比分析.md`

### 2.2 当前每条引用的 Mark 热路径

当前 `TraceObjectRefFieldsToGCWork()` 对每个引用直接调用 `DiscoverObject()`：

```text
读取 RefField
  -> heap-range gate
  -> GetRegionInfoDirect
  -> Region type/slot/alignment/BUMP allocPtr 校验
  -> 计算 bitmap word 与 bit mask
  -> relaxed probe mark bit
  -> 白 slot acquire-load stateWord/TypeInfo
  -> atomic fetch_or 竞争 mark ownership
  -> winner 且含引用字段时进入 Worker 私有 putCur_
```

这条路径已经具有以下优化：

- `ResolvedMarkTarget` 缓存 Region、word 和 mask；
- 已标记者在读取 stateWord 前返回；
- trusted-grey WorkBuf 消费端不重复执行完整 Region/slot 校验；
- Worker 有私有 `getCur_`/`putCur_` 和少量 empty WorkBuf cache；
- Global Full/Partial lane 的 mutex 等待在稳态采样中已不是主热点。

尚未解决的是：

- 同一对象或相邻对象被多个字段引用时，仍可能重复 probe、校验和 RMW；
- 同一 64-bit mark word 的 64 个 slot 竞争同一个原子 word；
- 相邻逻辑 word 连续存放，会共享同一 64B cache line；
- Global WorkBuf 不携带 Region affinity，Worker 的输入可能横跨大量 Region。

### 2.3 当前 Bitmap 布局事实

以 2026-07-26 当前源码为准，Runtime 和当前
`cangjie_compiler/third_party/llvm-project` 都仍是连续布局：

```text
MSBitmapArena Region slot
  + 0                             allocBitmap[bitmapWords]
  + bitmapWords * sizeof(uint64)  markBitmap[bitmapWords]
```

Runtime 的 `IsMarkBitSetResolved()`/`MarkBitmapWordResolved()` 直接访问连续
`markBitmap[wordIdx]`。AArch64 emitter 和 X86 emitter 都从 `MSRegionInfo::markBitmap`
取出指针，再按 `logicalWord * 8` 计算原子操作地址。

现有 `docs/superpowers/plans/2026-07-25-msgc-mark-bitmap-phase-bcd.md`
描述的是尚未执行的 Phase B/C/D 计划，不能把 8-lane striped layout 当作当前行为。

## 3. 优化目标与非目标

### 3.1 性能目标

以同一 SDK、同一 benchmark、相近单轮 marked objects/live bytes 为比较前提：

1. P50/P100/P150 的 Natural Mark 总速率达到 P5 基线的至少 80%，目标为不低于
   **24 Mobj/s**；
2. 关闭 Mark Assist 后，GC main + 3 Helper 相对单 GC main 的扩展从 1.779x
   提高到至少 **2.8x**；
3. P5 Mark 速率回退不超过 5%；
4. bitmap 原子提交次数相对通过校验的白对象候选数显著下降，并能用周期日志定量解释；
5. Worker 工作量保持均衡，不以单个 lane 或单个 Worker 长期饥饿换取局部性。

上述数值是验收目标，不是未实施前的收益承诺。若对象图缺少地址聚集性，Batch 合并率和
Region-affinity 收益可能低于预期，必须由新增的 Worker-local 指标决定是否继续 Phase D。

### 3.2 正确性与兼容目标

- 不改变 SATB、alloc-black、WeakRef、Finalizer resurrection 和 MarkTermination 语义；
- 每个对象仍由 mark bit 决定唯一 mark winner；
- stale edge 的 Region/slot/stateWord 校验保持在原子提交之前；
- 不改变 `MSRegionInfo` 字段偏移、`WorkBuf` 布局和 `AllocBufferMS::activeLog_` ABI；
- striped layout 切换时 Runtime、AArch64、X86 必须使用同一逻辑到物理映射；
- CopyGC 路径不受影响。

### 3.3 非目标

本方案不处理以下问题：

- P 与初始 committed heap、GC 触发阈值的耦合；
- Scavenge 与 ConcurrentMark 的调度重叠；
- Sweep backlog、`stopSweep` closure 或 FreeManager 释放速率；
- 通过增加更多 Helper 掩盖单线程扫描效率问题；
- 删除 white-slot 合法性校验或把并发 Mark 改成不安全的非原子标记。

## 4. 候选方案对比

| 方案 | 主要动作 | 优点 | 局限/风险 | 结论 |
|---|---|---|---|---|
| A：只做 striped bitmap | 相邻逻辑 word 分散到不同 cache line | 直接降低相邻 word false sharing | 不减少同 word RMW，不改善对象扫描局部性；需要跨 Runtime/LLVM ABI | 不单独采用 |
| B：Batch + Region-affinity + 条件式 striped | 合并同 word bit、按 Region 分流灰对象、最后改变布局 | 同时覆盖原子次数、对象局部性和 false sharing；可逐步 A/B | 数据结构和收敛协议需要扩展 | **推荐** |
| C：严格 Region owner-computes | raw candidate 先路由到 Region owner，由 owner 标记 | 理论上可大幅减少跨线程 bitmap 写 | Barrier/Assist/SATB 都要异步路由；未标记重复候选可能爆炸；收敛复杂 | 当前不采用 |

方案 C 只有在方案 B 仍无法突破内存一致性瓶颈、且硬件计数证明 bitmap ownership
是绝对主因时才值得重新评估。当前直接引入会把性能优化扩大成并发标记协议重写。

## 5. 推荐总体架构

推荐架构保留“生产端完成校验并赢得 mark，WorkBuf 只承载 trusted-grey”的现有契约，
在生产端与 Global 队列之间增加两个有界层：

```text
灰对象字段扫描
  -> ResolveMarkTarget + stale-slot 校验
  -> MarkWordBatcher 按 (Region, logicalWord) 聚合
  -> 一次 atomic fetch_or 提交 combinedMask
  -> 根据 newBits 选出真正 mark winners
  -> 按 winner 所在 Region 计算 affinity lane
  -> Worker-local lane builder
  -> Full/冲突/退出时发布到对应 Global Mark lane
  -> Worker 优先消费 home lane，空闲时跨 lane stealing
```

四个 Phase 的职责边界如下：

| Phase | 行为变化 | 能否独立回退 |
|---|---|---|
| A：逻辑访问层 | 连续布局不变，统一所有 Mark word 访问 | 可以 |
| B：MarkWordBatcher | GC 字段扫描合并同 word 原子提交 | 可以 |
| C：MarkLaneSet | 灰对象按 Region affinity 分区 | 可以 |
| D：Striped bitmap | Runtime/LLVM 同步改变物理映射 | 需要使用匹配 SDK 整体回退 |

Phase B 和 C 都是 Runtime 内部实现，不改变编译器生成代码；Phase D 才是隐藏 ABI 切换。
因此应先测量 B/C 的独立收益，再决定是否承担 D 的全 SDK 重建与兼容风险。

## 6. Phase A：统一 Mark Bitmap 逻辑访问层

### 6.1 唯一访问接口

由 `MSRegionInfo` 负责逻辑 word 到物理 word 的转换，并提供以下 Runtime 私有接口：

```cpp
static constexpr size_t MarkBitmapPhysicalIndex(size_t logicalWord);

uint64_t* MarkWordAddressResolved(size_t logicalWord);
const uint64_t* MarkWordAddressResolved(size_t logicalWord) const;
uint64_t LoadMarkWordResolved(size_t logicalWord) const;
void StoreMarkWordResolved(size_t logicalWord, uint64_t value);
uint64_t CommitMarkWordResolved(size_t logicalWord, uint64_t mask);
```

Phase A 使用恒等映射：

```cpp
MarkBitmapPhysicalIndex(logicalWord) == logicalWord;
```

`CommitMarkWordResolved()` 使用 relaxed atomic `fetch_or`，返回本次真正新设置的 bit：

```cpp
uint64_t old = atomic_fetch_or(address, mask);
return mask & ~old;
```

调用方不能依赖 `fetch_or` 的原始 old word，也不能各自复制逻辑到物理转换公式。

### 6.2 必须迁移的消费者

以下路径必须统一使用逻辑 word API：

- `GetMarkBit()`、`IsMarkBitSetResolved()`、`TestAndSetMarkBit()`；
- `ResolvedMarkTarget::IsMarked()`/`TryMark()`；
- small `SweepRegion()` 的两次 bitmap 遍历；
- `ComputeMarkedStats()` 和 marked-object enumeration；
- `ClearMarkBitmap()`；
- Runtime `BulkRefMarker`；
- 后续 `MarkWordBatcher`。

Phase A 完成后，对以下目录执行直接访问审计：

```text
src/Heap/MAllocator
src/Heap/MCollector
src/Heap/MBarrier
```

除访问器实现本身外，不应再出现直接 `markBitmap[logicalWord]` 或
`memset(markBitmap, ...)`。Phase A 不修改 LLVM，不改变 mark bitmap 指针值和物理布局，
其性能应与基线等价。

## 7. Phase B：GC 扫描 MarkWordBatcher

### 7.1 数据结构

每次 `DrainCurrentBuf()` 在栈上创建一个固定容量 batcher，不进行堆分配、不加锁，也不把
未提交候选跨 WorkBuf 保留：

```cpp
struct MarkCandidate {
    BaseObject* object;
    TypeInfo* typeInfo;
    uint64_t bitMask;
};

struct MarkWordEntry {
    MSRegionInfo* region;
    uint16_t logicalWord;
    uint16_t candidateCount;
    uint64_t combinedMask;
    MarkCandidate candidates[16];
};

class MSMarkWordBatcher {
    MarkWordEntry entries[4];
};
```

Key 为 `(MSRegionInfo*, logicalWord)`。同一 entry 中如果已经存在相同 bit，直接视为
batch-local duplicate，不重复保存 candidate。单 entry 超过 16 个唯一 bit，或 4 个 entry
全部占用且出现新 key 时，先 flush 一个 entry，再复用其槽位。

第一版使用固定 round-robin eviction 即可，不引入 LRU 时间戳。bitmap word 本身最多 64 个 bit，
候选数组只是控制栈空间和 flush 粒度，不改变 mark bit 表达能力。

### 7.2 引用处理顺序

把当前 `DiscoverObject()` 的 small-object 路径拆为三个内部步骤：

```text
ResolveSmallMarkTarget()
ValidateWhiteTarget()
CommitOrBatch()
```

GC 字段扫描执行：

1. heap range、Region type、slot boundary/alignment 和 BUMP `allocPtr` 校验；
2. relaxed probe mark bit，已标记者立即返回；
3. 对白 slot acquire-load stateWord，确认 TypeInfo 合法；
4. 保存校验得到的 `TypeInfo*`，将 candidate 加入 batch；
5. entry flush 时合并执行一次 `CommitMarkWordResolved()`；
6. `newBits` 中存在 candidate bit 的对象才是本次真正 winner；
7. winner 且 `TypeInfo::HasRefField()` 时进入灰对象输出。

伪代码：

```cpp
uint64_t newBits = entry.region->CommitMarkWordResolved(
    entry.logicalWord, entry.combinedMask);
for (const MarkCandidate& candidate : entry.candidates) {
    if ((newBits & candidate.bitMask) != 0 &&
        candidate.typeInfo->HasRefField()) {
        greySink.Put(candidate.object, entry.region);
    }
}
```

两个线程可以分别聚合同一个 word，最终仍由原子 `fetch_or` 返回的 `newBits` 决定唯一 winner。
失败线程可能完成了候选校验，但不会重复把对象加入灰队列。

### 7.3 使用范围

第一版只用于 GC main、Mark Helper 和 Mutator Mark Assist 共用的 `DrainCurrentBuf()` 字段扫描：

- ordinary object、RawArray、Struct/StructArray 的 ref field 可以进入 batch；
- Large object 保持 byte flag 立即原子标记；
- root、单引用 Runtime/inline barrier、`MarkNewObject()` 保持立即提交；
- Runtime bulk copy barrier 可以复用同样的 logical-word commit 语义，但保留独立的小容量 batcher；
- WeakRef 对象本身仍按现有 deferred 语义处理。

### 7.4 Flush 边界

必须在以下边界显式清空 batch：

- `DrainCurrentBuf()` 正常返回前；
- 输入 WorkBuf 被回收到本地 cache/Pool 前；
- Worker 退出本次 in-flight claim 前；
- Assist 发布 builder 并退出前；
- Quiesce 和 MarkTermination 观察收敛状态前。

实现上 batcher 生命周期限制在一次 `DrainCurrentBuf()`，正常路径显式 `FlushAll()`；
Debug 构建的析构函数只断言 entry 已空，不依赖析构函数完成正确性提交。

### 7.5 指标

以下计数全部保存在 Worker/Assist 本地，退出或周期结束时汇总，禁止在每条引用上更新全局原子：

- `visitedEdges`
- `markedProbeHits`
- `validatedWhiteCandidates`
- `batchLocalDuplicates`
- `batchFlushes`
- `atomicWordCommits`
- `committedNewBits`
- `lostBits`

核心比率：

```text
RMW reduction = 1 - atomicWordCommits / validatedWhiteCandidates
batch coalesce = validatedWhiteCandidates / atomicWordCommits
winner ratio = committedNewBits / validatedWhiteCandidates
```

## 8. Phase C：Region-affinity MarkLaneSet

### 8.1 Lane 划分

将单个逻辑 Global Mark queue 扩展为一组 lane。每个 lane 仍保留现有 Full/Partial 两条
WorkBuf head 和一把 mutex；分片首先用于地址亲和，不把“消除队列锁”作为主要收益。

建议初始策略：

```text
laneCount = nextPowerOfTwo(totalMarkThreads)，clamp 到 [1, 16]
affinityGroup = regionIndex / 64
laneId = affinityGroup & (laneCount - 1)
```

MS Region 为 32KiB，64 个 Region 对应约 2MiB 地址范围。同一 2MiB 范围进入同一 lane，
既能保留对象/元数据/TLB 局部性，也比“整个 64MiB Segment 一个 lane”更不容易产生严重倾斜。

`laneCount`、group 大小和 burst quantum 首版使用编译期常量，并输出日志；在没有数据前不增加
一组公开调优参数。

### 8.2 Worker-local lane builder

单个 Worker 不能为每个 lane 永久持有一个 WorkBuf，否则 `workers × lanes` 会隐藏过多工作。
每个 `PerPGCWork` 只维护 4 个 direct-mapped builder：

```cpp
struct MarkLaneBuilder {
    uint16_t laneId;
    WorkBuf* buffer;
};
```

灰对象输出时已经持有 `ResolvedMarkTarget::info`，不需要重新查询 RegionInfo：

```text
target Region -> affinity lane
  -> 命中本地 builder：append
  -> builder full：发布到 lane Full
  -> builder 槽发生 lane 冲突：先发布旧 Partial，再绑定新 lane
```

这不改变 `WorkBuf` 的 `next/len/objs` 布局，也不在 WorkBuf 内添加 lane 字段；lane 由其所在队列
表达。Worker 退出活跃 burst 前必须发布全部非空 Partial builder。

### 8.3 Worker 消费与 stealing

每个 GC main/Helper 有一个 home lane：

```text
homeLane = workerId % laneCount
```

领取顺序：

1. 当前输入 WorkBuf；
2. home lane Full；
3. home lane Partial；
4. 其他 lane Full round-robin steal；
5. 其他 lane Partial round-robin steal；
6. flush 本地 builder 后再进行一次精确检查。

为了防止一个大 lane 独占 Worker，单次 home-lane burst 最多连续处理 8 个 WorkBuf；
达到 quantum 后刷新 lane backlog，若其他 lane 有工作则轮转。steal cursor 保存在 Worker 本地，
避免所有 Helper 每次都从 lane 0 开始。

### 8.4 Root、Barrier 与 Assist

- GC root 在 `DiscoverObject()` 赢得 mark 后，按目标 Region 直接进入对应 lane；
- GC main/Helper 的字段扫描使用 4-entry lane builder；
- Mutator Mark Assist 领取任意 lane 的 Full WorkBuf，使用较小的本地 builder，并在每次 Assist
  返回前发布全部 Partial；
- mutator `activeLog_` 是编译器 ABI WorkBuf，可能混合多个 Region。首版不在 mutator 写屏障热路径
  逐对象分桶，而是按 home shard/processor 进入 ingress lane；该 WorkBuf 扫描产生的下一层灰对象
  再按 Region 正常分流；
- MarkTermination 可以逐 lane 使用现有 Full-first `Pop()` 语义固定点清空。

### 8.5 局部性与均衡指标

每个 Worker 本地记录：

- `regionGroupSwitches`
- `homeLaneBuffers`
- `stolenLaneBuffers`
- `partialBuilderFlushes`
- `builderConflicts`
- `maxObservedLaneDebt`
- `processedObjects`

重点观察：

```text
Region switches per 1K scanned objects
steal ratio = stolen buffers / total buffers
worker imbalance = max(processed) / average(processed)
lane imbalance = max lane processed / average lane processed
```

若 steal ratio 长期接近 100%，说明 Region affinity 没有形成；若 worker/lane imbalance 过高，
应先缩小 affinity group 或 burst quantum，而不是继续增加 lane 数。

## 9. Phase D：8-lane Striped Mark Bitmap

### 9.1 布局

Small Region 最多有 4096 个 8B slot，对应最多 64 个 mark bitmap word。使用 8 条
64B lane，保持 mark bitmap 最大物理空间为 512B：

```cpp
constexpr size_t kMarkBitmapLaneCount = 8;
constexpr size_t kMarkBitmapWordsPerLane = 8;

physicalWord =
    (logicalWord & 7) * 8 +
    (logicalWord >> 3);
```

示例：

| logical | physical | cache line |
|---:|---:|---:|
| 0 | 0 | 0 |
| 1 | 8 | 1 |
| 2 | 16 | 2 |
| 7 | 56 | 7 |
| 8 | 1 | 0 |
| 63 | 63 | 7 |

这样相邻逻辑 word 不再位于同一 cache line；同一 cache line 保存间隔为 8 的逻辑 word。
它只减少相邻 word false sharing，不能消除多个线程对同一 word 的竞争，因此必须放在
MarkWordBatcher 之后评估。

### 9.2 Runtime 存储

新增 `MSMarkBitmapLayout.h` 作为 Runtime 的唯一布局定义。`MSBitmapArena` 每个 Region
已有 1024B sidecar，可固定划分为：

```text
slotBase +   0B：alloc bitmap 最大保留区 512B
slotBase + 512B：mark bitmap striped 物理区 512B
```

这不会增加 sidecar 虚拟地址空间，也不改变 `MSRegionInfo::markBitmap` 字段偏移；
`markBitmap` 指针值会统一指向 `slotBase + 512B`。

Sweep、统计、对象枚举和 mark probe/commit 都按逻辑 word 遍历，由
`MarkBitmapPhysicalIndex()` 转换。清理阶段只清有效的逻辑 word，不默认无条件写满 512B，
避免大 size class 触碰不必要的物理 cache line。

### 9.3 LLVM 同步

`llvm/include/llvm/CodeGen/CJMSGCABI.h` 镜像相同常量和 constexpr 映射。以下发射点必须同步：

- AArch64 scalar Ref/StaticRef small mark；
- AArch64 short-GCTib pair/single helper；
- X86 scalar `lock btsq`；
- X86 Struct/StaticStruct leaf helper；
- 任何内联 bulk/descriptor mark 地址计算。

原来的：

```text
markBitmap + logicalWord * 8
```

改为：

```text
lane = logicalWord & 7
slotInLane = logicalWord >> 3
markBitmap + lane * 64 + slotInLane * 8
```

Runtime bulk barrier 不复制公式，继续调用 `MSRegionInfo` 的 logical-word API。

### 9.4 Phase D 启用条件

满足以下条件才进入 Phase D：

1. Phase B 的 `batch coalesce` 已证明原子合并有效，但 4 线程扩展仍显著低于目标；
2. Phase C 后 Worker 负载均衡正常，Region-group switch 已下降；
3. 采样或硬件计数仍指向 mark bitmap cache-line ownership/false sharing；
4. 已准备匹配的 compiler、runtime、stdlib、stdx 全量构建和回退 SDK。

如果 Phase B/C 已达到 Mark 目标，则可以暂缓 Phase D，避免为有限收益引入隐藏 ABI 切换。

## 10. 并发收敛与正确性约束

### 10.1 Mark winner 不变量

所有 small-object candidate 必须满足：

```text
地址/Region/slot合法
  AND 当前mark bit为0
  AND stateWord/TypeInfo合法
  BEFORE candidate进入batch
```

`CommitMarkWordResolved()` 返回的 `newBits` 是唯一 mark winner authority。batch-local duplicate、
其他 Worker 先提交、Runtime barrier 先提交或 Assist 先提交，都只能使当前 candidate 不出现在
`newBits` 中，不能重复入灰队列。

在当前并发 Sweep/stale edge 根因没有完全消除前，禁止以下优化：

- 把 marked probe 与 atomic `fetch_or` 合为一步并在之后验证；
- 用 alloc bitmap 替代 white-slot stateWord 校验；
- 因为 batch 已持有 TypeInfo 就跨 GC cycle 保存 candidate；
- 对 Large、SPAN 或非法 Region 套用 small bitmap batch。

### 10.2 WorkBuf producer 不变量

任何进入 Mark lane 的对象必须继续满足 trusted-grey 契约：

```text
valid object + mark winner + has reference field
```

WorkBuf 消费端不重新 Discover；Release 构建直接扫描，Debug 构建可以断言 mark bit、
对象合法性和 `HasRefField()`。lane 分区只改变灰对象放到哪个队列，不改变 WorkBuf 内容语义。

### 10.3 并发可见工作集合

Natural convergence 的完整条件扩展为：

```text
GC main 当前输入为空
AND 所有 Global Mark lane Full/Partial 均为空
AND 所有 Worker lane builder 为空
AND 所有 in-flight MarkWordBatcher 为空
AND workersInFlight == 0
AND assistsInFlight == 0
```

不能只对 lane 的 Global count 求和而忽略 Worker 私有 builder。建议协议为：

1. Worker 没有可领取输入时，先 `FlushAllBatch()`；
2. 再发布全部非空 lane builder；
3. 退出 `workersInFlight`；
4. GC main 读取精确 lane count；
5. 首次完整为空后进入现有 one-way quiesce；
6. Quiesce 等待 in-flight 归零后再次读取精确 lane count；
7. late published work 留给 MarkTermination 固定点，不能恢复旧 epoch Helper。

Mutator Assist 必须在 `ExitAssist()` 之前完成 batch flush 和所有 Partial builder 发布。
mutator 尚未满的 `activeLog_` 仍按现有协议由 STW termination flush。

### 10.4 MarkTermination

MarkTermination 在 STW 下：

1. flush 所有 mutator active logs；
2. dispose/发布 GC main 与 Helper 的残余 PerPGCWork；
3. 逐 lane Full-first drain 到固定点；
4. 处理 deferred new object/WeakRef；
5. snapshot Finalizer，闭合 resurrection 子图；
6. 再次逐 lane drain，直到所有 batch/builder/lane 均为空；
7. 最后按最终 mark bit 处理 WeakRef referent。

`MarkWordBatcher` 不允许存活到 STW termination 之外。若 Debug 析构发现非空 entry，应直接暴露
协议错误，而不是静默丢弃。

### 10.5 Sweep 与 Bitmap 生命周期

PreEnumRoots 仍必须先 `StopAndWaitForAllSweep()`，确认上一轮 unswept backlog 物理完成，再清
mark bitmap。striped layout 不能替代 Sweep quiesce 或 sweep generation CAS。

Clear、Mark、ComputeMarkedStats 和 Sweep 必须使用同一逻辑 word 视图；不能出现 Mark 使用 striped、
Sweep 仍按连续 word 解释的混合状态。

## 11. Runtime/LLVM 隐藏 ABI 与兼容策略

### 11.1 唯一常量与跨仓契约

Runtime：

```text
src/Heap/MAllocator/MSMarkBitmapLayout.h
```

LLVM：

```text
llvm/include/llvm/CodeGen/CJMSGCABI.h
```

两侧必须具有相同的：

- layout version；
- lane count；
- words per lane；
- cache-line stride；
- maximum logical words；
- logical-to-physical mapping examples。

新增跨仓 source-contract 测试，至少比较：

```text
0 -> 0
1 -> 8
7 -> 56
8 -> 1
63 -> 63
```

并拒绝 emitter 中恢复旧的直接 `logicalWord * 8` 地址序列。

### 11.2 FastPath layout version

`CJ_MRT_MSGCFastPathState` 当前 phase 位于 offset 0，heap start/end 和 RegionInfo array
分别位于 8/16/24，结构大小 32B。可以利用 offset 1 的现有 padding 增加：

```cpp
uint8_t markBitmapLayoutVersion;
```

新 AArch64/X86 emitter 进入 inline MARK fast path 前校验版本；版本不匹配时回
Runtime wrapper slow path。这样可以保护：

```text
新 compiler 生成代码 + 旧 Runtime
```

但无法保护：

```text
旧 compiler 生成代码 + 新 striped Runtime
```

因为旧代码没有版本检查，仍会按连续布局写错误 word。因此 Phase D 不提供旧二进制兼容承诺。

### 11.3 SDK 切换规则

Phase D 必须整体生成一套新 SDK，明确记录：

- compiler：含新 AArch64/X86 emitter；
- runtime：含相同 bitmap layout；
- stdlib/stdx：用该 compiler/runtime 组合重新构建；
- benchmark：替换 Runtime 后用该 SDK 重新编译 `.cj`；
- macOS Runtime dylib：复制后重新 `codesign -f -s -`。

多 SDK 对比必须使用隔离的 `env -i` 子 shell，并通过 `DYLD_PRINT_LIBRARIES=1` 验证实际加载路径。
旧 compiler、旧 stdlib binary 或旧 benchmark 不允许与 striped Runtime 混用。

### 11.4 回退

Phase A/B/C 都应保留编译期开关或短期诊断 gate，可单独回退到连续 bitmap + 单 Global queue。
Phase D 的回退单位是整套 SDK，不能只替换 Runtime dylib。

## 12. 分阶段实施顺序

### Stage 0：补齐无扰动诊断

先增加 Worker-local 计数并在周期结束汇总：

- edge/probe/white/winner/lost；
- bitmap word commit 和 batch 合并；
- Region-group switch；
- lane home/steal/builder conflict；
- 每个 Worker 活跃时间和 processed。

这些计数默认只在 REPORT/诊断模式启用，热路径不更新全局原子。Stage 0 本身必须先证明
Mark 速率没有统计扰动。

### Stage 1：Phase A Identity Accessor

1. 为 logical mark word API 写失败单测；
2. 实现恒等映射访问器；
3. 迁移 Mark、Sweep、Stats、Clear、Enumeration、Runtime bulk barrier；
4. 运行直接访问审计和全部 bitmap 生命周期测试；
5. 构建 Runtime，确认行为、速率和 bitmap 统计与基线一致。

进入下一阶段条件：同一 run 的 marked object/live bytes、Sweep 回收结果完全一致，
Mark 速率波动在噪声范围内。

### Stage 2：Phase B MarkWordBatcher

1. 单测同 word 合并、重复 bit、entry eviction、winner filtering；
2. 拆分 small resolve/validate/commit 内部接口；
3. 只接入 `DrainCurrentBuf()`，Large/root/single barrier 保持立即提交；
4. 增加所有 flush 边界契约；
5. 对比单线程、4线程无 Assist，以及正常 Assist case27。

进入下一阶段条件：

- 正确性用例全部通过；
- `atomicWordCommits < validatedWhiteCandidates`，且周期数据可以解释收益；
- P5 不回退超过 5%；
- 高 P Mark 有稳定改善，或数据明确说明对象地址缺少同 word 聚集。

### Stage 3：Phase C MarkLaneSet

1. 先实现独立 lane queue 和精确 count 测试；
2. 实现 Worker-local 4-entry builder；
3. 接入 root、GC main、Helper、Assist；
4. 接入 home-lane burst 和 round-robin stealing；
5. 扩展 quiesce/termination 契约和 stress 测试；
6. 评估 Region-group switch、steal ratio 和 Worker imbalance。

进入下一阶段条件：

- lane/private builder 不会造成提前收敛；
- Worker 最大工作量不超过平均值的合理范围，且没有永久饥饿 lane；
- Region-group switch 明显下降；
- 相同对象图的 Mark 速率继续改善。

### Stage 4：决定是否进入 Phase D

使用 Stage 2/3 指标做决策：

```text
若已达到 >=24 Mobj/s 或 4线程 >=2.8x：
    暂缓 striped，保留连续 bitmap
否则若 bitmap ownership 仍是主要热点：
    进入 Stage 5
否则：
    转向对象字段/TypeInfo/RegionInfo 访问延迟优化
```

### Stage 5：Phase D Runtime/LLVM Striped ABI

1. Runtime 单测先要求 striped mapping，验证旧实现失败；
2. Runtime 切换 physical mapping，完成 Sweep/Stats/Clear 回归；
3. LLVM AArch64/X86 测试先要求 lane/slot 地址计算；
4. 修改 emitters 和 `CJMSGCABI.h`；
5. 增加跨仓 layout ABI contract；
6. 构建 compiler/runtime/stdlib/stdx 完整 SDK；
7. 重新编译所有 `.cj` benchmark；
8. 使用独立 SDK 做连续与 striped A/B。

## 13. 观测指标与验收标准

### 13.1 Mark 核心指标

| 指标 | 口径 | 目标 |
|---|---|---|
| Natural Mark 总速率 | `(main + helper + assist) / ConcurrentMark wall` | 高 P ≥24 Mobj/s |
| 无 Assist 单线程速率 | `gcMain / wall` | 不低于现有基线 |
| 无 Assist 4线程扩展 | `4-thread rate / 1-thread rate` | ≥2.8x |
| P5 回退 | 相同配置 Natural rate | ≤5% |
| Batch合并率 | candidates / atomic commits | 稳定大于1，并按对象类型分布解释 |
| Winner率 | newBits / candidates | 用于区分重复引用与真实新对象 |
| Region切换 | switches / 1K scanned objects | 相比单Global显著下降 |
| Steal率 | stolen / total buffers | 不长期接近0%或100% |
| Worker不均衡 | max processed / average | 不因affinity显著恶化 |

`objects/s` 只统计被消费的 trusted-grey 对象，不等于字段边访问率，因此必须同时报告
`visitedEdges/s` 和 `atomicWordCommits/s`。

### 13.2 隔离变量

Mark 扫描器 A/B 必须：

- 使用同一 P、heap、payload、并发度和 Runtime其他参数；
- 选择相近 marked objects/live bytes 的周期；
- 分别报告 Natural、debt-stalled、time-budget；
- 至少增加一组 `CJ_MS_DISABLE_ASSIST=1` 的1线程/4线程实验；
- 单独记录 Scavenge 与 Mark 重叠，必要时用诊断配置减少重叠，以免把内存释放带宽误算为
  MarkLane/bitmap 效果；
- 固定 compiler/runtime/stdlib/stdx 来源并记录 dylib MD5。

### 13.3 正确性验收

必须覆盖：

- ordinary object、RefArray、StructArray、RawArray；
- small/Large/SPAN；
- duplicate edge 和同 word 多 bit；
- SATB old-ref、mutator active log、Assist partial builder；
- alloc-black/new object；
- WeakRef、Finalizer resurrection；
- natural convergence、one-way quiesce、MarkTermination fixed point；
- Sweep、ComputeMarkedStats、VisitMarkedObjects 对同一 logical bitmap 的解释；
- P5/P50/P100/P150 case27 完整退出和 `[RESULT]`；
- 现有 case2/case3 GC 正确性回归。

### 13.4 业务与资源指标

即使 Mark objects/s 达标，也必须同时检查：

- benchmark throughput；
- GC CPU 与进程总 CPU；
- p95/p99；
- Assist share 和 mutator CPU；
- RSS、峰值 committed/in-use；
- MarkTermination/STW；
- Sweep backlog 与下一轮 `stopSweep`。

若 Mark 提速但业务吞吐下降，优先检查 lane 分桶和 batch flush 是否增加 mutator/Assist 工作，
不能只按 GC 内部 objects/s 判定成功。

## 14. 预计修改文件与测试

### 14.1 Runtime

新建：

- `src/Heap/MAllocator/MSMarkBitmapLayout.h`
- `src/Heap/MCollector/MSMarkWordBatcher.h`
- `src/Heap/MCollector/MSMarkLaneSet.h`（若实现不适合全部内联，再增加 `.cpp`）

修改：

- `src/Heap/MAllocator/MSBitmapArena.h/.cpp`
- `src/Heap/MAllocator/MSRegionInfo.h/.cpp`
- `src/Heap/MAllocator/MSCentralRegionPool.cpp`
- `src/Heap/MCollector/MarkSweepCollector.h/.cpp`
- `src/Heap/MBarrier/MSMarkBarrier.cpp`
- `src/Heap/MSGCFastPath.h`
- `src/Heap/Heap.cpp`（FastPath state 定义/初始化）
- `src/Common/LFStack.h`（只增加 lane 集合需要的组合接口，不改变 WorkBuf ABI）

可能修改：

- `src/Heap/MCollector/MSMarkWorkerControl.h`
- `src/Heap/MCollector/MSMarkLocalStats.h`
- `src/Heap/MAllocator/MSAllocBuffer.cpp`

### 14.2 LLVM

修改：

- `llvm/include/llvm/CodeGen/CJMSGCABI.h`
- `llvm/lib/Target/AArch64/AArch64MSGCEmitter.cpp`
- `llvm/lib/Target/X86/X86MSGCEmitter.cpp`
- AArch64/X86 与 MSGC write-barrier emission size 共享的估算代码（若实际地址序列长度变化）

不得在 `CJBarrierLowering` 或前端新增 striped 专用 intrinsic。布局属于 Runtime/target-emitter
隐藏 ABI，原始 Cangjie IR 和 MSGC wrapper 名称保持不变。

### 14.3 Runtime 测试

建议新建或扩展：

- `test/MSRegionInfoBatchMarkTest.cpp`
- `test/MSMarkWordBatcherTest.cpp`
- `test/MSMarkLaneSetTest.cpp`
- `test/MSGCMarkWordBatcherContractTest.sh`
- `test/MSGCMarkLaneConvergenceContractTest.sh`
- `test/MSGCMarkBitmapLayoutABIContractTest.sh`
- `test/MSGCSweepRegionBitmapContractTest.sh`
- `test/MSGCPostMarkSemanticClosureContractTest.sh`
- `test/MSGCTailDrainContractTest.sh`
- `test/MSGCWorkBufBatchDualLaneContractTest.sh`

### 14.4 LLVM 测试

- `llvm/test/CodeGen/AArch64/cangjie-writebarrier-msgc-fastpath.ll`
- AArch64 no-LSE/LSE object-level disassembly测试；
- `llvm/test/CodeGen/X86/cangjie-msgc-writebarrier-mark-ref.ll`
- X86 Struct/StaticStruct helper测试；
- MinGW COFF relocation/addressing测试；
- Runtime/LLVM layout常量跨仓契约。

### 14.5 性能结果

继续使用：

- `MSGC_RESULT/Result/case27_http_complex_json_msgc_high_p/run_compare.sh`
- P5/P50/P100/P150、100K、60s 的固定参数矩阵；
- 独立的1线程/4线程无Assist Mark扩展实验。

每个阶段生成独立结果目录和Runtime MD5，不能覆盖Stage间数据。

## 15. 风险、回退与决策建议

### 15.1 MarkWordBatcher 风险

- 对象引用分散时合并率低，却增加candidate暂存和flush分支；
- 固定entry过少会频繁evict，过多会增加Worker栈和cache footprint；
- TypeInfo读取与commit之间存在时间窗口，但在Mark期间对象不移动，且candidate已完成合法性校验；
- flush遗漏会直接造成漏标，因此必须用源码契约和Debug析构断言双重约束。

回退：关闭Batch接入，保留Phase A访问器。

### 15.2 MarkLaneSet 风险

- live graph集中在少数Region group时lane倾斜；
- 私有builder过多会隐藏工作、推迟Helper扩容或导致提前收敛；
- burst过大损害公平性，过小又失去局部性；
- barrier ingress是混合WorkBuf，第一代对象不能完全按Region分区。

回退：所有对象路由到lane 0，即恢复现有单Global语义；保持其他Phase不变。

### 15.3 Striped Bitmap 风险

- 对bitmapWords很少的大size class，相邻逻辑word被分散后可能增加Clear/Sweep/Stats触碰的cache line；
- emitter地址序列变长，inline barrier指令数和I-cache占用上升；
- Runtime/LLVM任一侧公式不一致都会产生GC正确性错误；
- 旧compiler二进制配新Runtime无法由layout version完全保护。

回退：整体切回连续bitmap SDK，不允许只替换Runtime。

### 15.4 最终建议

推荐批准以下实施边界：

1. **必须先做 Phase A**，没有统一逻辑访问层不进入任何布局修改；
2. **优先做 Phase B**，用最小协议变化直接减少同 word 原子RMW；
3. **随后做 Phase C**，用有界lane builder改善大堆扫描局部性；
4. **Phase D由数据触发**，不是默认必须上线的步骤；
5. 不实施严格Region owner-computes，也不删除stale-slot校验；
6. 每个Phase独立构建、独立A/B、独立回退，禁止一次提交后只测最终组合。

该顺序可以先获得低风险收益，并用中间数据判断高 P Mark 的剩余瓶颈究竟是同 word 竞争、
地址局部性，还是striped布局才能解决的相邻word false sharing。
