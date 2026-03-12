# Step5 DFX 日志与可观测性设计

> **所属**：[Go 风格 GC 迁移方案](Go风格GC迁移方案.md) — Step 5 DFX 子文档
> **上游依赖**：Step1（分配器）· Step2（三色标记）· Step3（混合写屏障）· Step4（并发清扫）

## 1. 设计目标

1. **保持 6 块日志结构不变**：入口 → 标记计时 → 后标记计时 → 回收统计 → Region 统计 → GC 汇总，便于与 CopyGC 日志横向对比
2. **替换 CopyGC 专有概念**：删除 tagID / Forward / Ghost / exempt from-space / SATB re-marking，替换为 MS-GC 对应概念
3. **新增 MS-GC 独有指标**：bitmap sweep 效率、sweepGen 代数、lazy sweep 触发率、WBBuf flush 次数、Mark Assist 字节数
4. **兼容现有 `VLOG(REPORT)` + `TRACE_COUNT` 框架**：不引入新日志基础设施，仅改内容
5. **可用 `grep` 单行提取关键指标**：每行以 `[MS-GC]` 前缀标识，便于脚本分析

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
allocated size: 111,580,480, heap size: 111,542,272, heap utilization: 100.03%
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

### 3.1 块① GC 入口（2 行）

```
[MS-GC] === GC START cycle=12 === sweepGen=26 reason=3(heuristic)
[MS-GC] allocated 68,157,440 B, threshold 67,108,864 B, phase IDLE→MARK_SETUP
```

| 字段 | 说明 |
|------|------|
| `cycle=N` | GC 轮次计数（全局递增，与 CopyGC 共用计数器） |
| `sweepGen=N` | 当前清扫代数（每次 GC +2，Step4.1） |
| `reason=N(text)` | 触发原因：1=oom / 3=heuristic / 4=explicit |
| `allocated` | 当前已分配字节数（来自 `GcStats`） |
| `threshold` | 当前 GC 触发阈值（Pacer 计算） |
| `phase` | 状态机转换（`MS_PHASE_IDLE → MS_PHASE_MARK_SETUP`） |

**与 CopyGC 差异**：删除 `tagID`，新增 `sweepGen` 和 `phase` 状态转换。

### 3.2 块② MarkSetup + ConcurrentMark 计时（~10 行）

```
[MS-GC] --- MarkSetup (STW) ---
[MS-GC] ForceFinishSweep: swept 3 remaining regions, 45us
[MS-GC] ClearBitmaps: cleared 512 regions, 120us
[MS-GC] InstallWriteBarrier: switched to MSWriteBarrier, 2us
[MS-GC] EnumRoots: 2 roots (exclude stack roots), 28us
[MS-GC] MarkSetup total: 195us, phase MARK_SETUP→MARK
[MS-GC] --- ConcurrentMark ---
[MS-GC] ConcurrentMark with 2 threads, initial workStack: 2
[MS-GC] ConcurrentMark time: 85us, marked 598 objects
[MS-GC] FlushWriteBarrierBuffers: 12 buffers, 156 entries, 8us
[MS-GC] MarkAssist: 3 mutators assisted, 32768 B marked
```

| 字段 | 说明 |
|------|------|
| `ForceFinishSweep` | MarkSetup 开始时强制完成上一轮残留 lazy sweep（Step4.2） |
| `ClearBitmaps` | 清零所有活跃 Region 的 markBitmap（Step2.1） |
| `InstallWriteBarrier` | 安装混合写屏障 `MSWriteBarrier`（Step3） |
| `EnumRoots` | 枚举根集（无 old pointer 更新） |
| `MarkSetup total` | STW 阶段总时间 + 状态转换 |
| `ConcurrentMark threads/time/marked` | 并发标记线程数、耗时、标记对象数 |
| `FlushWriteBarrierBuffers` | 刷新 WBBuf：buf 数量、总条目数、耗时（Step3.3） |
| `MarkAssist` | Mutator 辅助标记统计（Step2.3 GC Assist） |

**与 CopyGC 差异**：
- 新增 `ForceFinishSweep`、`ClearBitmaps`、`InstallWriteBarrier` 三行
- `MarkSatbBuffer` → `FlushWriteBarrierBuffers`
- 新增 `MarkAssist` 行
- 删除 `trace live objects & update old pointers` 行

### 3.3 块③ MarkTermination + PostMark 计时（~8 行）

```
[MS-GC] --- MarkTermination (STW) ---
[MS-GC] DrainMarkQueue: drained 24 grey objects, 12us
[MS-GC] FlushWriteBarrierBuffers (final): 2 buffers, 8 entries, 3us
[MS-GC] MarkTermination total: 15us, phase MARK→MARK_TERMINATION
[MS-GC] --- PostMark ---
[MS-GC] ProcessWeakRefs: cleared 5 weak refs, 8us
[MS-GC] EnqueueFinalizers: enqueued 2 objects, 3us
[MS-GC] CollectLargeGarbage: freed 3 large objects (1,114,112 B), 5us
[MS-GC] PrepareForSweep: advanced sweepGen 24→26, prepared 286 regions, 15us
[MS-GC] InstallIdleBarrier: switched to MSIdleBarrier, 1us
[MS-GC] PostMark total: 32us, phase MARK_TERMINATION→POST_MARK→IDLE
```

| 字段 | 说明 |
|------|------|
| `DrainMarkQueue` | 终止阶段排空灰色对象队列 |
| `FlushWriteBarrierBuffers (final)` | 最终一次刷新（STW 下确保无遗漏） |
| `MarkTermination total` | STW 终止阶段耗时 |
| `ProcessWeakRefs` | 弱引用处理（清除未标记的弱引用） |
| `EnqueueFinalizers` | 析构函数入队（Step2.4） |
| `CollectLargeGarbage` | 大对象垃圾直接释放（Step2.4） |
| `PrepareForSweep` | 推进 sweepGen 并准备待清扫 Region 列表（Step4.1） |
| `InstallIdleBarrier` | 切换回 `MSIdleBarrier`，恢复 mutator 快速路径 |
| `PostMark total` | 后标记阶段总时间 + 完整状态转换链 |

**与 CopyGC 差异**：
- 整块替换：CopyGC 的 PostTrace/Preforward/ForwardFromRegions/CollectFromSpace 全部删除
- 新增 `PrepareForSweep`（推进 sweepGen）和 `InstallIdleBarrier`
- 大对象处理简化：直接释放而非迁移

### 3.4 块④ 回收与 Sweep 统计（~5 行）

> **说明**：这一块在 PostMark 结束后异步输出，因为并发清扫是在 mutator 恢复后逐步执行的。
> 最终结果在下一次 GC 入口时汇总输出（或由 `ForceFinishSweep` 触发时输出）。

```
[MS-GC] --- Sweep Summary (since cycle=11) ---
[MS-GC] swept small: 274 regions, freed 18,743,296 B (bitmap CTZ sweep)
[MS-GC] swept large: 3 objects, freed 1,114,112 B
[MS-GC] released regions: 48 (returned to OS via madvise)
[MS-GC] sweep breakdown: background 198 regions (72%), lazy 76 regions (28%)
[MS-GC] garbage ratio: 15.16%
```

| 字段 | 说明 |
|------|------|
| `swept small` | 小对象 Region 清扫数量 + 释放字节数 |
| `swept large` | 大对象清扫数 + 释放字节数（已在 PostMark 完成） |
| `released regions` | 完全空闲 Region 退回 OS（`madvise DONTNEED`） |
| `sweep breakdown` | 后台清扫 vs 懒惰清扫的比例（Step4.2 / Step4.3） |
| `garbage ratio` | 垃圾率 = freed / allocated_before_gc |

**与 CopyGC 差异**：
- CopyGC 的 `collect ... B: old small ... old pinned ... old large` 完全重写
- 新增 `sweep breakdown` 显示后台/懒惰清扫比例
- 新增 `released regions`（CopyGC 的 released 在 Region 统计块中，这里单独提取）

### 3.5 块⑤ Region 统计（CentralRegionPool 视角，~15 行）

```
[MS-GC] region statistics when gc ends (sweepGen=26)
[MS-GC]   total units: 8192 (134,217,728 B)
[MS-GC]   active units: 6400 (104,857,600 B)
[MS-GC]   tl-regions (in-use): 3 regions, 12 units
[MS-GC]   CentralPool small (36 classes aggregated):
[MS-GC]     partialSwept: 45 regions  fullSwept: 180 regions
[MS-GC]     partialUnswept: 12 regions  fullUnswept: 49 regions
[MS-GC]     total small: 286 regions, 1144 units
[MS-GC]   large-object regions: 240 regions, 6784 units
[MS-GC]   free regions: 1560 regions, 6240 units
[MS-GC]   released (to OS): 48 regions, 192 units
[MS-GC]   FRAG-INT: bitmapUtil 78.32%, internalWaste 8,126,464 B
[MS-GC]   FRAG-EXT: index 0.7200, maxContiguousFree 32 units
[MS-GC]   FRAG-LARGE: largeInMainRatio 82.89%
[MS-GC]   MAIN: [0x10a958000, 0x112958000) inactiveZone 0x112938000
```

| 字段 | 说明 |
|------|------|
| `tl-regions (in-use)` | 当前绑定在 Mutator 线程上的 ThreadLocal Region |
| `CentralPool small` | 按四链表输出（Step1.3 CentralRegionPool） |
| `partialSwept` | 已清扫且不满的 Region（可立即分配） |
| `partialUnswept` | 未清扫且不满的 Region（lazy sweep 候选） |
| `fullSwept` / `fullUnswept` | 已满的 Region（待下次 GC 重新评估） |
| `large-object regions` | 大对象占用的 Region 数 |
| `free regions` | 空闲可用 Region |
| `released` | 已退回 OS 的 Region |
| `FRAG-INT` | 内部碎片：bitmap 利用率 + 浪费字节数 |
| `FRAG-EXT` / `FRAG-LARGE` / `MAIN` | 与 CopyGC 相同格式，保持对比性 |

**与 CopyGC 差异**：
- 删除 `from-regions`、`recent-full`、`garbage regions`、`pinned regions`、`recent-pinned`、`rawPointer-pinned`、`dirty regions`
- 新增 CentralPool 四链表视图（`partialSwept / fullSwept / partialUnswept / fullUnswept`）
- `FRAG-INT` 新增 `bitmapUtil`（bitmap 位利用率，比 CopyGC 的 `heapUtil` 更精确）

### 3.6 块⑥ GC 汇总（3 行）

```
[MS-GC] allocated size: 111,580,480, heap size: 111,542,272, heap utilization: 100.03%
[MS-GC] live bytes 111,580,480 (survived 111,449,408, recent-allocated 131,072), update gc threshold 131,195,167 -> 131,533,373
[MS-GC] total gc time: 242 us (MarkSetup 195 + MarkTerm 15 + PostMark 32), sweep pending
```

| 字段 | 说明 |
|------|------|
| `allocated size` / `heap size` / `heap utilization` | 与 CopyGC 相同（来自 `GcStats::Dump()`） |
| `live bytes` / `survived` / `recent-allocated` | 与 CopyGC 相同（survived 基于 bitmap 统计） |
| `update gc threshold` | Pacer 计算的新阈值 |
| `total gc time` | STW + 并发标记总时间（**不含 Sweep**） |
| `(MarkSetup + MarkTerm + PostMark)` | 拆分各阶段耗时，方便定位瓶颈 |
| `sweep pending` | 提示 Sweep 尚未完成（异步进行中） |

**与 CopyGC 差异**：
- `total gc time` 拆分为三个子阶段，CopyGC 只有一个总数
- 新增 `sweep pending` 标记，表明 Sweep 是异步的
- `collection rate` 在 sweep 完成后才输出（在下一次 GC 入口的 Sweep Summary 中）

### 3.7 完整示例输出（一个 GC Cycle ~35 行）

```
[MS-GC] === GC START cycle=12 === sweepGen=26 reason=3(heuristic)
[MS-GC] allocated 68,157,440 B, threshold 67,108,864 B, phase IDLE→MARK_SETUP
[MS-GC] --- MarkSetup (STW) ---
[MS-GC] ForceFinishSweep: swept 3 remaining regions, 45us
[MS-GC] ClearBitmaps: cleared 512 regions, 120us
[MS-GC] InstallWriteBarrier: switched to MSWriteBarrier, 2us
[MS-GC] EnumRoots: 2 roots (exclude stack roots), 28us
[MS-GC] MarkSetup total: 195us, phase MARK_SETUP→MARK
[MS-GC] --- ConcurrentMark ---
[MS-GC] ConcurrentMark with 2 threads, initial workStack: 2
[MS-GC] ConcurrentMark time: 85us, marked 598 objects
[MS-GC] FlushWriteBarrierBuffers: 12 buffers, 156 entries, 8us
[MS-GC] MarkAssist: 3 mutators assisted, 32768 B marked
[MS-GC] --- MarkTermination (STW) ---
[MS-GC] DrainMarkQueue: drained 24 grey objects, 12us
[MS-GC] FlushWriteBarrierBuffers (final): 2 buffers, 8 entries, 3us
[MS-GC] MarkTermination total: 15us, phase MARK→MARK_TERMINATION
[MS-GC] --- PostMark ---
[MS-GC] ProcessWeakRefs: cleared 5 weak refs, 8us
[MS-GC] EnqueueFinalizers: enqueued 2 objects, 3us
[MS-GC] CollectLargeGarbage: freed 3 large objects (1,114,112 B), 5us
[MS-GC] PrepareForSweep: advanced sweepGen 24→26, prepared 286 regions, 15us
[MS-GC] InstallIdleBarrier: switched to MSIdleBarrier, 1us
[MS-GC] PostMark total: 32us, phase MARK_TERMINATION→POST_MARK→IDLE
[MS-GC] --- Sweep Summary (since cycle=11) ---
[MS-GC] swept small: 274 regions, freed 18,743,296 B (bitmap CTZ sweep)
[MS-GC] swept large: 3 objects, freed 1,114,112 B
[MS-GC] released regions: 48 (returned to OS via madvise)
[MS-GC] sweep breakdown: background 198 regions (72%), lazy 76 regions (28%)
[MS-GC] garbage ratio: 15.16%
[MS-GC] region statistics when gc ends (sweepGen=26)
[MS-GC]   total units: 8192 (134,217,728 B)
[MS-GC]   active units: 6400 (104,857,600 B)
[MS-GC]   tl-regions (in-use): 3 regions, 12 units
[MS-GC]   CentralPool small (36 classes aggregated):
[MS-GC]     partialSwept: 45 regions  fullSwept: 180 regions
[MS-GC]     partialUnswept: 12 regions  fullUnswept: 49 regions
[MS-GC]     total small: 286 regions, 1144 units
[MS-GC]   large-object regions: 240 regions, 6784 units
[MS-GC]   free regions: 1560 regions, 6240 units
[MS-GC]   released (to OS): 48 regions, 192 units
[MS-GC]   FRAG-INT: bitmapUtil 78.32%, internalWaste 8,126,464 B
[MS-GC]   FRAG-EXT: index 0.7200, maxContiguousFree 32 units
[MS-GC]   FRAG-LARGE: largeInMainRatio 82.89%
[MS-GC]   MAIN: [0x10a958000, 0x112958000) inactiveZone 0x112938000
[MS-GC] allocated size: 111,580,480, heap size: 111,542,272, heap utilization: 100.03%
[MS-GC] live bytes 111,580,480 (survived 111,449,408, recent-allocated 131,072), update gc threshold 131,195,167 -> 131,533,373
[MS-GC] total gc time: 242 us (MarkSetup 195 + MarkTerm 15 + PostMark 32), sweep pending
```

## 4. TRACE_COUNT 可观测指标集合

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
| 6 | — | `[MS-GC] InstallWriteBarrier: switched to MSWriteBarrier, Xus` | **新增** |
| 7 | `Total roots: N (exclude stack roots)` | `[MS-GC] EnumRoots: N roots (exclude stack roots), Xus` | **修改**：合并为一行 |
| 8 | `enum roots & update old pointers within time: Xus` | *(合并到上一行)* | **删除** |
| 9 | — | `[MS-GC] MarkSetup total: Xus, phase MARK_SETUP→MARK` | **新增** |
| 10 | `Concurrent mark with T threads, workStack: N` | `[MS-GC] ConcurrentMark with T threads, initial workStack: N` | **修改**：加前缀 |
| 11 | `Concurrent marking time: Xus` | `[MS-GC] ConcurrentMark time: Xus, marked M objects` | **修改**：合并 mark count |
| 12 | `MarkSatbBuffer time: Xus` | `[MS-GC] FlushWriteBarrierBuffers: B buffers, E entries, Xus` | **重写** |
| 13 | `Concurrent re-marking time: Xus` | `[MS-GC] DrainMarkQueue: drained G grey objects, Xus` | **重写** |
| 14 | — | `[MS-GC] MarkAssist: M mutators assisted, B B marked` | **新增** |
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
| 42 | `allocated size: ..., heap size: ..., heap utilization: ...` | `[MS-GC] allocated size: ..., heap size: ..., heap utilization: ...` | **保留**：加前缀 |
| 43 | `live bytes M (...), update gc threshold ...` | `[MS-GC] live bytes M (...), update gc threshold ...` | **保留**：加前缀 |
| 44 | `total gc time: X us, collection rate Y MB/s` | `[MS-GC] total gc time: X us (MarkSetup+MarkTerm+PostMark), sweep pending` | **修改**：拆分 + sweep 标记 |

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
