# Step5 DFX 日志与可观测性设计

> **所属**：[Go 风格 GC 迁移方案](Go风格GC迁移方案.md) — Step 5 DFX 子文档
> **上游依赖**：Step1（分配器）· Step2（三色标记）· Step3（混合写屏障）· Step4（并发清扫）

## 1. 设计目标

1. **以当前 `MarkSweepCollector.cpp` 的真实输出为准**：文档样例必须能对应到现有 `VLOG(REPORT)` 语句，不能再保留未实现的 `ForceFinishSweep`、`InstallWriteBarrier`、`PrepareForSweep` 等设计态日志。
2. **保留 CopyGC 对照价值**：继续用块状结构解释日志，但块的边界以当前 MS collector 的真实实现为准。
3. **覆盖当前已落地指标**：`sweepGen`、previous-cycle sweep summary、assist/global queue 统计、`active-region bytes`、`true live`、`releasedTree` 等。
4. **兼容现有 `VLOG(REPORT)` + `TRACE_COUNT` 框架**：不引入新日志基础设施，仅校正文档口径。
5. **便于脚本抓取**：继续保持 `[MS-GC]` 前缀和单行关键摘要。

## 2. 当前 CopyGC 日志结构分析

一个完整 GC Cycle（以 cycle=60 为例）约 40 行日志，分为 6 大块：

### 2.1 块① GC 入口（2 行）

```
=== GC START cycle=60 === tagID=0 reason=1
GC TraceHeap: tagID=0 reason=1
```

| 字段 | 含义 | MS-GC 是否保留 |
|------|------|---------------|
| `cycle=N` | GC 轮次计数 | ✅ 保留 |
| `tagID=N` | CopyGC 指针标记 ID（0/1 交替） | ❌ 删除（MS 无 tag） |
| `reason=N` | GC 触发原因（1=oom, 3=heuristic） | ✅ 保留 |

### 2.2 块② Trace 阶段计时（~10 行）

```
Total roots: 2(exclude stack roots)
enum roots & update old pointers within time: 32us
roots size: 2
Concurrent mark with 2 threads, workStack: 2
Concurrent marking time: 85us
MarkSatbBuffer time: 0us
Concurrent re-marking time: 3us
identify useless extern ref time: 0us
resurrected objects 0
concurrent resurrection time: 2us
mark 598 objects
DoTracing time: 110us
trace live objects & update old pointers in ref-fields time: 115us
```

| 字段 | MS-GC 变化 |
|------|-----------|
| `enum roots & update old pointers` | → `enum roots`（无 old pointer 更新） |
| `MarkSatbBuffer time` | → `FlushWriteBarrierBuffers time`（Step3） |
| `Concurrent re-marking time` | → `MarkTermination drain time` |
| `identify useless extern ref time` | ✅ 保留 |
| `trace live objects & update old pointers in ref-fields time` | → `ConcurrentMark time`（无 old pointer 更新） |

### 2.3 块③ PostTrace + Forward 计时（~8 行）

```
GC PostTrace: tagID=0 reason=1
Collect large garbage time: 5us
CompactLOSIfNeeded time: 0us
exempt from-space: 18743296 B - 0 B -> 18743296 B, 0 B floating garbage, 0 B to forward
ExemptFromRegions time: 5us
PrepareFromRegionList: dispelled 274 ghosts, prepared 286 new from-regions
BuildLargeObjectMigrationRoutes time: 0us
PostTrace time: 704us
Preforward time: 22us
ForwardFromRegions time: 214us
CollectFromSpaceGarbage time: 0us
```

| 字段 | MS-GC 变化 |
|------|-----------|
| `Collect large garbage time` | ✅ 保留（Step2.4 CollectLargeGarbage） |
| `CompactLOSIfNeeded` | ❌ 删除（MS 无 LOS compact） |
| `exempt from-space` / `ExemptFromRegions` | ❌ 整行删除（MS 无 from-space） |
| `PrepareFromRegionList` / `ghosts` | ❌ 整行删除（MS 无 Ghost） |
| `BuildLargeObjectMigrationRoutes` | ❌ 删除（MS 无 migration） |
| `Preforward` / `ForwardFromRegions` | ❌ 整行删除（MS 无 Forward） |
| `CollectFromSpaceGarbage` | ❌ 删除 |

**→ 此块在 MS-GC 中完全重写为 PostMarkTermination + Sweep 触发日志。**

### 2.4 块④ 回收统计（2 行）

```
collect 19857408 B: old small 18743296 - 18743296 B, old pinned 0 - 0 B, old large 112263168 - 1114112 B. garbage ratio 15.16%
start to release heap garbage memory
```

| 字段 | MS-GC 变化 |
|------|-----------|
| `old small ... - ... B` | → `swept small: N regions, freed M B` |
| `old pinned` | ❌ 删除（MS 下所有对象不移动） |
| `old large ... - ... B` | → `swept large: N objects, freed M B` |
| `garbage ratio` | ✅ 保留，含义变为 `(freed / total_before_gc)` |

### 2.5 块⑤ Region 统计（~17 行）

```
region statistics when gc ends
    total units: 8192 (134217728 B)
    active units: 8184 (134086656 B)
    tl-regions 3: 12 units
    from-regions 0: 0 units           ← CopyGC 独有
    recent-full regions 2: 8 units    ← CopyGC 独有
    garbage regions 291: 1200 units   ← CopyGC 独有
    pinned regions 0: 0 units
    large-object regions 240: 6784 units
    ...
    FRAG-EXT: index 0.6800, maxContiguousFree 32 units, freeFragments 5
    FRAG-INT: internalWaste 14258016 B, heapUtil 72.55%
    FRAG-LARGE: largeInMainRatio 82.89%
    MAIN: [0x10a958000, 0x112958000) inactiveZone 0x112938000
```

**→ MS-GC 中 Region 分类需完全重写为 CentralRegionPool 四链表视角。**

### 2.6 块⑥ GC 汇总（3 行）

```
committed size: 111,580,480, heap size: 111,542,272, true-live utilization: 0.12%
live bytes 111580480 (survived 111449408, recent-allocated 131072), update gc threshold 131195167 -> 131533373
total gc time: 2,262 us, collection rate 8372.016 MB/s
```

| 字段 | MS-GC 变化 |
|------|-----------|
| `allocated size` / `heap size` | ✅ 保留 |
| `heap utilization` | ✅ 保留 |
| `live bytes` / `survived` / `recent-allocated` | ✅ 保留 |
| `update gc threshold` | ✅ 保留（Step5 Pacer 计算） |
| `total gc time` | ✅ 保留，但含义变为 MarkSetup→PostMark（不含 Sweep） |
| `collection rate` | ✅ 保留 |

## 3. MS-GC 新日志格式设计

### 3.1 块① GC 入口（当前真实 2 行）

```text
[MS-GC] === GC START cycle=12 === sweepGen=26 reason=3(heuristic)
[MS-GC] active-region bytes 68,157,440 B, threshold 67,108,864 B, gcIndex=12
```

当前字段含义：

| 字段 | 说明 |
|------|------|
| `cycle` | 当前 GC 轮次 |
| `sweepGen` | `CentralRegionPool::globalSweepGen_` 当前值 |
| `reason` | `GCReason` 数值与名称 |
| `active-region bytes` | `Heap::GetHeap().GetAllocatedSize()` |
| `threshold` | `GetGCStats().GetThreshold()` |
| `gcIndex` | collector 内部本轮索引 |

注意：当前真实日志里没有 `phase IDLE→MARK_SETUP` 这一行。

### 3.2 块② Previous-Cycle Sweep Summary（从第 2 轮 GC 开始可见）

`LogSweepSummary()` 现在在每轮 GC 入口时输出上一轮 sweep 的汇总：

```text
[MS-GC] --- Previous-Cycle Sweep Summary (cycle=11) ---
[MS-GC] swept small: 274 regions, freed 18,743,296 B (empty=48 -> swept cache, retained=226)
[MS-GC] large reclaimed in previous cycle: 3 objects / 35 regions, freed 1,114,112 B
[MS-GC] free-cache tree: releasedTree=48 regions (1,572,864 B)
[MS-GC] sweep breakdown: background 198 (72%), lazy 76 (28%)
```

这里的重点是：

1. 小对象 sweep 汇总来自 `CentralRegionPool::sweepStats_`。
2. 大对象统计来自上一轮 `CollectLargeGarbage()` 的快照。
3. `releasedTree` 已单独作为 free-cache tree 指标输出。

### 3.3 块③ MarkSetup / Root 阶段

当前 `MarkSweepCollector.cpp` 实际拆成两段：

```text
[MS-GC] --- PreEnumRoots ---
[MS-GC] PreEnumRoots: 18us
[MS-GC] --- EnumRoots ---
[MS-GC] EnumRoots: 177us, roots=2
```

与此同时，`CentralRegionPool` 在 `PreEnumRoots()` 内部还会额外输出：

```text
[MS-GC] ClearAllMarkBitmaps: ...
[MS-GC] ReclassifyUnsweptToSwept: ...
```

所以当前实现不是“MarkSetup total + ForceFinishSweep + InstallWriteBarrier”这套设计态格式，而是：

1. `PreEnumRoots` 只统计 collector 侧总时间。
2. 具体的 sweep-stop / bitmap-clear / reclassify 细节由 CentralRegionPool 各自打印。
3. `EnumRoots` 单独给出耗时和 root 数。

### 3.4 块④ ConcurrentMark

当前真实输出是：

```text
[MS-GC] --- ConcurrentMark (GC thread + Mutator assist) ---
[MS-GC] ConcurrentMark initial workStack bufs: 2
[MS-GC] ConcurrentMark time: 85us, marked 598 objects (assist: 3 calls, 16 marked, 64 processed, 40 traced, 1 fast-skip; global: 2 push/3 bufs, pop hit 3 miss 1)
```

当前已经落地的字段：

- assist 调用次数、标记数、处理数、trace 数、fast-skip 数
- global share 次数、共享 buf 数、pop hit/miss

当前没有单独的 `FlushWriteBarrierBuffers: ...` 并发期日志行；overflow 是通过 `globalWorkQueue_` 正常收敛的。

### 3.5 块⑤ MarkTermination

当前真实输出是：

```text
[MS-GC] --- MarkTermination (STW) ---
[MS-GC] MarkTermination total: 15us, flushed=164 entries, resurrection=2 entries, processed=24 objects, newlyMarked=3
```

它直接对应当前 `MarkTermination()` 的真实汇总口径：

- `flushed` = `WriteBarrierBuffer + assistLocalWork + globalWorkQueue_` 并入主队列的条目数
- `resurrection` = 复活候选条目数
- `processed` / `newlyMarked` = `ExecuteLikeDrainMainQueue()` 的处理结果

当前没有单独的 `DrainMarkQueue` 或 `FlushWriteBarrierBuffers (final)` 独立日志行。

### 3.6 块⑥ PostMark

当前 `PostMarkTermination()` 的真实输出是：

```text
[MS-GC] --- PostMark ---
[MS-GC] ProcessWeakRefs: 8us
[MS-GC] EnqueueFinalizers: enqueued 2 objects, 3us
[MS-GC] CollectLargeGarbage: freed 3 large objects / 35 regions (1,114,112 B), 5us
[MS-GC] PostMark total: 32us
```

注意：当前日志里没有单独的 `PrepareForSweep` 或 `InstallIdleBarrier` 行。它们对应的逻辑已经发生，但没有被单独打印成一行摘要。

### 3.7 块⑦ Region Statistics

当前 collector 会输出：

```text
[MS-GC] region statistics when gc ends (sweepGen=26)
[MS-GC]   total capacity: 134,217,728 B
[MS-GC]   active-region bytes: 104,857,600 B, true live: 12,345,678 B
[MS-GC]   free-cache tree: releasedTree=1,572,864 B
[MS-GC]   region-accounting: active=3200 regions, releasedTree=48 regions, virgin=256 regions, committed=4096 regions
...
```

随后 `CentralRegionPool::DumpPoolStats()` 还会补充当前 pool/bucket 统计，最后再输出：

```text
[MS-GC]   scheduler P: 8, tl-active regions: 6, alloc buffers: 8
```

因此，这一块当前不再是文档里那套手写的“四链表固定样例”，而是 `DumpRegionStatsMS()` + `DumpPoolStats()` 的组合输出。

### 3.8 块⑧ GC Summary

当前真实输出是：

```text
[MS-GC] active-region bytes: 104,857,600, true live: 12,345,678 B, true-live utilization: 11.78%, slack: 88.22%, marked 598 objects
[MS-GC] total gc time: 242 us (PreEnum 18 + EnumRoots 177 + ConcMark 0 + MarkTerm 15 + PostMark 32 + ConcSweep 0 + PostGC 0 + Other 0)
```

此外还会额外打一条 `LOG(RTLOG_INFO)` 兼容摘要。

### 3.9 当前实现与旧样例的主要差异

当前文档必须明确以下漂移点：

1. 没有 `ForceFinishSweep` 日志行。
2. 没有 `InstallWriteBarrier` / `InstallIdleBarrier` 日志行。
3. 没有 `PrepareForSweep` 单独摘要行。
4. `Previous-Cycle Sweep Summary` 是在下一轮 GC 开头输出，而不是在本轮 `PostMark` 之后立即输出。
5. `Region statistics` 当前由 `DumpRegionStatsMS()` 和 `DumpPoolStats()` 联合输出，字段比旧样例更贴近真实运行时状态。

## 4. TRACE_COUNT 可观测指标集合

> 说明：本节及后续对照表里仍保留了一些“建议新增”指标/日志项，用于说明可观测性缺口。
> 凡与第 3 节当前真实日志格式不一致的条目，都应理解为**待补齐的 backlog**，而不是当前已经落地的输出。

### 4.1 保留指标（与 CopyGC 共用）

| TRACE_COUNT 名称 | 含义 |
|-----------------|------|
| `CJRT_GC_CYCLE` | GC 轮次计数 |
| `CJRT_GC_REASON` | 触发原因 |
| `CJRT_GC_ALLOCATED_BYTES` | 当前已分配字节数 |
| `CJRT_GC_THRESHOLD` | GC 触发阈值 |
| `CJRT_GC_LIVE_BYTES` | 存活字节数 |
| `CJRT_GC_HEAP_SIZE` | 堆大小 |
| `CJRT_GC_HEAP_UTIL` | 堆利用率 (%) |
| `CJRT_GC_TOTAL_TIME_US` | GC 总耗时 (us) |
| `CJRT_GC_ROOTS_COUNT` | 根集数量 |
| `CJRT_GC_MARK_COUNT` | 标记对象数 |
| `CJRT_GC_CONCURRENT_MARK_TIME_US` | 并发标记耗时 |
| `CJRT_GC_WEAK_REF_CLEARED` | 清除的弱引用数 |
| `CJRT_GC_FINALIZER_ENQUEUED` | 入队的析构函数数 |
| `CJRT_GC_TL_REGIONS` | ThreadLocal Region 数 |
| `CJRT_GC_LARGE_REGIONS` | 大对象 Region 数 |
| `CJRT_GC_FREE_REGIONS` | 空闲 Region 数 |
| `CJRT_GC_RELEASED_REGIONS` | 退回 OS 的 Region 数 |
| `CJRT_GC_FRAG_EXT_INDEX` | 外部碎片指数 |
| `CJRT_GC_FRAG_INT_WASTE` | 内部碎片浪费字节 |
| `CJRT_GC_FRAG_LARGE_RATIO` | 大对象在 MAIN 中占比 |

### 4.2 新增 MS-GC 专有指标

| TRACE_COUNT 名称 | 含义 | 来源 |
|-----------------|------|------|
| `CJRT_MSGC_SWEEP_GEN` | 当前 sweepGen 代数 | Step4.1 |
| `CJRT_MSGC_MARK_SETUP_US` | MarkSetup STW 耗时 (us) | 块② |
| `CJRT_MSGC_MARK_TERM_US` | MarkTermination STW 耗时 (us) | 块③ |
| `CJRT_MSGC_POST_MARK_US` | PostMark 耗时 (us) | 块③ |
| `CJRT_MSGC_CLEAR_BITMAP_US` | ClearBitmaps 耗时 (us) | Step2.1 |
| `CJRT_MSGC_CLEAR_BITMAP_REGIONS` | 清零 bitmap 的 Region 数 | Step2.1 |
| `CJRT_MSGC_WBBUF_FLUSH_COUNT` | WBBuf flush 次数 | Step3.3 |
| `CJRT_MSGC_WBBUF_ENTRIES_TOTAL` | WBBuf 总条目数 | Step3.3 |
| `CJRT_MSGC_MARK_ASSIST_BYTES` | Mutator 辅助标记字节数 | Step2.3 |
| `CJRT_MSGC_MARK_ASSIST_MUTATORS` | 参与辅助标记的 Mutator 数 | Step2.3 |
| `CJRT_MSGC_SWEPT_SMALL_REGIONS` | 清扫的小对象 Region 数 | Step4.2-4.3 |
| `CJRT_MSGC_SWEPT_SMALL_FREED` | 小对象清扫释放字节数 | Step4.2-4.3 |
| `CJRT_MSGC_SWEPT_LARGE_COUNT` | 清扫的大对象数 | Step2.4 |
| `CJRT_MSGC_SWEPT_LARGE_FREED` | 大对象清扫释放字节数 | Step2.4 |
| `CJRT_MSGC_BG_SWEEP_REGIONS` | 后台清扫 Region 数 | Step4.2 |
| `CJRT_MSGC_LAZY_SWEEP_REGIONS` | 懒惰清扫 Region 数 | Step4.3 |
| `CJRT_MSGC_FORCE_FINISH_SWEEP_US` | ForceFinishSweep 耗时 (us) | Step4.2 |
| `CJRT_MSGC_BITMAP_UTIL` | bitmap 位利用率 (%) | 块⑤ |
| `CJRT_MSGC_POOL_PARTIAL_SWEPT` | CentralPool partialSwept Region 数 | Step1.3 |
| `CJRT_MSGC_POOL_FULL_SWEPT` | CentralPool fullSwept Region 数 | Step1.3 |
| `CJRT_MSGC_POOL_PARTIAL_UNSWEPT` | CentralPool partialUnswept Region 数 | Step1.3 |
| `CJRT_MSGC_POOL_FULL_UNSWEPT` | CentralPool fullUnswept Region 数 | Step1.3 |
| `CJRT_MSGC_GARBAGE_RATIO` | 垃圾率 (%) | 块④ |

### 4.3 删除的 CopyGC 指标

| 原 TRACE_COUNT 名称 | 删除原因 |
|---------------------|---------|
| `CJRT_GC_TAG_ID` | MS 无 tag 指针标记 |
| `CJRT_GC_FROM_REGIONS` | MS 无 from-space |
| `CJRT_GC_GHOST_REGIONS` | MS 无 Ghost Region |
| `CJRT_GC_FORWARD_TIME_US` | MS 无 Forward 阶段 |
| `CJRT_GC_PREFORWARD_TIME_US` | MS 无 Preforward |
| `CJRT_GC_EXEMPT_BYTES` | MS 无 exempt 机制 |
| `CJRT_GC_PINNED_REGIONS` | MS 下所有对象不移动，无需区分 pinned |
| `CJRT_GC_GARBAGE_REGIONS` | MS 的 Region 分类不同 |
| `CJRT_GC_DIRTY_REGIONS` | MS 无 dirty 概念 |
| `CJRT_GC_RECENT_FULL_REGIONS` | MS 无 recent-full 分类 |

## 5. CopyGC vs MS-GC 日志逐行对照表

| # | CopyGC 日志行 | MS-GC 日志行 | 变化类型 |
|---|--------------|-------------|---------|
| 1 | `=== GC START cycle=N === tagID=T reason=R` | `[MS-GC] === GC START cycle=N === sweepGen=S reason=R(text)` | **修改**：tagID→sweepGen |
| 2 | `GC TraceHeap: tagID=T reason=R` | `[MS-GC] allocated M B, threshold T B, phase IDLE→MARK_SETUP` | **重写** |
| 3 | — | `[MS-GC] --- MarkSetup (STW) ---` | **新增** |
| 4 | — | `[MS-GC] ForceFinishSweep: swept N remaining regions, Xus` | **新增** |
| 5 | — | `[MS-GC] ClearBitmaps: cleared N regions, Xus` | **新增** |
| 6 | — | `[MS-GC] InstallWriteBarrier: switched to MSMarkBarrier, Xus` | **新增** |
| 7 | `Total roots: N (exclude stack roots)` | `[MS-GC] EnumRoots: N roots (exclude stack roots), Xus` | **修改**：合并为一行 |
| 8 | `enum roots & update old pointers within time: Xus` | *(合并到上一行)* | **删除** |
| 9 | — | `[MS-GC] MarkSetup total: Xus, phase MARK_SETUP→MARK` | **新增** |
| 10 | `Concurrent mark with T threads, workStack: N` | `[MS-GC] ConcurrentMark with T threads, initial workStack: N` | **修改**：加前缀 |
| 11 | `Concurrent marking time: Xus` | `[MS-GC] ConcurrentMark time: Xus, marked M objects (assist: C calls, K marked, P processed, T traced, F fast-skip; global: G push/B bufs, pop hit H miss M)` | **修改**：合并 mark count，并补 assist/global queue 实际工作量 |
| 12 | `MarkSatbBuffer time: Xus` | `[MS-GC] FlushWriteBarrierBuffers: B buffers, E entries, Xus` | **重写** |
| 13 | `Concurrent re-marking time: Xus` | `[MS-GC] DrainMarkQueue: drained G grey objects, Xus` | **重写** |
| 14 | — | *(合并到 #11 的 assist 子字段)* | **新增后再合并** |
| 15 | `identify useless extern ref time: Xus` | *(保留在 PostMark 中 ProcessWeakRefs)* | **移动** |
| 16 | `resurrected objects N` / `concurrent resurrection time` | `[MS-GC] EnqueueFinalizers: enqueued N objects, Xus` | **重写** |
| 17 | `mark N objects` | *(合并到 #11)* | **删除** |
| 18 | `DoTracing time: Xus` | *(拆分为 MarkSetup + ConcMark + MarkTerm)* | **删除** |
| 19 | `trace live objects & update old pointers...` | *(删除，MS 无 old pointer 更新)* | **删除** |
| 20 | `GC PostTrace: tagID=T reason=R` | `[MS-GC] --- PostMark ---` | **重写** |
| 21 | `Collect large garbage time: Xus` | `[MS-GC] CollectLargeGarbage: freed N large objects (M B), Xus` | **修改**：增加详情 |
| 22 | `CompactLOSIfNeeded time: Xus` | *(删除)* | **删除** |
| 23 | `exempt from-space: ...` | *(删除)* | **删除** |
| 24 | `ExemptFromRegions time: Xus` | *(删除)* | **删除** |
| 25 | `PrepareFromRegionList: dispelled N ghosts...` | `[MS-GC] PrepareForSweep: advanced sweepGen→N, prepared M regions, Xus` | **重写** |
| 26 | `BuildLargeObjectMigrationRoutes time: Xus` | *(删除)* | **删除** |
| 27 | `PostTrace time: Xus` | `[MS-GC] PostMark total: Xus, phase ...→IDLE` | **重写** |
| 28 | `Preforward time: Xus` | *(删除)* | **删除** |
| 29 | `ForwardFromRegions time: Xus` | *(删除)* | **删除** |
| 30 | `CollectFromSpaceGarbage time: Xus` | *(删除)* | **删除** |
| 31 | `collect M B: old small ... old pinned ... old large ...` | `[MS-GC] swept small: N regions, freed M B` | **重写** |
| 32 | `start to release heap garbage memory` | `[MS-GC] sweep breakdown: background N (X%), lazy M (Y%)` | **重写** |
| 33 | `region statistics when gc ends` | `[MS-GC] region statistics when gc ends (sweepGen=N)` | **修改**：加 sweepGen |
| 34 | `tl-regions N: M units` | `[MS-GC]   tl-regions (in-use): N regions, M units` | **修改** |
| 35 | `from-regions N: M units` | *(删除)* | **删除** |
| 36 | `recent-full regions N: M units` | *(删除)* | **删除** |
| 37 | `garbage regions N: M units` | *(删除)* | **删除** |
| 38 | `pinned regions N: M units` | *(删除)* | **删除** |
| 39 | — | `[MS-GC]   CentralPool small (36 classes aggregated):` | **新增** |
| 40 | — | `[MS-GC]     partialSwept/fullSwept/partialUnswept/fullUnswept: ...` | **新增** |
| 41 | `FRAG-INT: internalWaste M B, heapUtil X%` | `[MS-GC]   FRAG-INT: bitmapUtil X%, internalWaste M B` | **修改**：新增 bitmapUtil |
| 42 | `allocated size: ..., heap size: ..., heap utilization: ...` | `[MS-GC] committed size: ..., heap size: ..., true-live utilization: ...` | **修改**：按 MS 真实语义重命名 |
| 43 | `live bytes M (...), update gc threshold ...` | `[MS-GC] live bytes M (...), update gc threshold ...` | **保留**：加前缀 |
| 44 | `total gc time: X us, collection rate Y MB/s` | `[MS-GC] total gc time: X us (PreEnum+EnumRoots+ConcMark+MarkTerm+PostMark+ConcSweep+PostGC+Other)` | **修改**：完整拆分并与总数对齐 |

**统计**：保留 ~8 行，修改 ~12 行，新增 ~12 行，删除 ~12 行。

## 6. 实现要点

### 6.1 代码组织

| 功能 | 实现位置 | 说明 |
|------|---------|------|
| GC 入口日志（块①） | `MSCollector::GarbageCollection()` | 新文件，对标 `CopyCollector.cpp` |
| MarkSetup 日志（块②前半） | `MSCollector::MarkSetup()` | 新函数，含 `MRT_PHASE_TIMER` |
| ConcurrentMark 日志（块②后半） | `TracingCollector::DoTracing()` | **复用**现有框架，`#ifdef MRT_USE_MARK_SWEEP` 分支改输出文案 |
| MarkTermination 日志（块③前半） | `MSCollector::MarkTermination()` | 新函数 |
| PostMark 日志（块③后半） | `MSCollector::PostMark()` | 新函数，对标 `WCollector::DoGarbageCollection()` 后半段 |
| Sweep 统计（块④） | `MSSweeper::DumpSweepStats()` | 新函数，在下次 GC 入口或 ForceFinishSweep 时输出 |
| Region 统计（块⑤） | `RegionManager::DumpRegionStatsMS()` | 新函数，对标 `DumpRegionStats()` |
| GC 汇总（块⑥） | `GcStats::DumpMS()` | 新函数或 `#ifdef` 分支 |
| TRACE_COUNT 指标 | 各函数内部 | 使用现有 `TRACE_COUNT(name, value)` 宏 |

### 6.2 条件编译策略

```cpp
// 在 RegionManager.cpp 中
void RegionManager::DumpRegionStats() {
#ifdef MRT_USE_MARK_SWEEP
    DumpRegionStatsMS();  // MS-GC 视角
#else
    DumpRegionStatsCopy(); // 原 CopyGC 逻辑（重命名）
#endif
}
```

所有日志差异点均通过 `#ifdef MRT_USE_MARK_SWEEP` 切换，确保 CopyGC 日志不受影响。

### 6.3 日志级别

| 级别 | 使用场景 |
|------|---------|
| `VLOG(REPORT, ...)` | 块①-⑥ 所有 GC 日志（与 CopyGC 保持一致） |
| `LOG(RTLOG_INFO, ...)` | GC 汇总行（始终输出） |
| `TRACE_COUNT(name, value)` | 所有可观测指标（供外部监控采集） |
| `MRT_PHASE_TIMER(name)` | 阶段计时（自动输出 `name time: Xus`） |

### 6.4 `[MS-GC]` 前缀实现

```cpp
// 定义统一前缀宏
#ifdef MRT_USE_MARK_SWEEP
#define GC_LOG_PREFIX "[MS-GC] "
#else
#define GC_LOG_PREFIX ""
#endif

// 使用示例
VLOG(REPORT, GC_LOG_PREFIX "=== GC START cycle=%u === sweepGen=%u reason=%d(%s)",
     gcCycle_, sweepGen_, reason, ReasonToString(reason));
```

### 6.5 实现顺序建议

1. **Phase 1**：实现块①（入口）+ 块⑥（汇总），最小可验证日志
2. **Phase 2**：实现块②③（标记阶段计时），依赖 Step2 代码
3. **Phase 3**：实现块④（Sweep 统计），依赖 Step4 代码
4. **Phase 4**：实现块⑤（Region 统计），依赖 Step1.3 CentralRegionPool
5. **Phase 5**：补全所有 TRACE_COUNT 指标，进行日志对比验证
