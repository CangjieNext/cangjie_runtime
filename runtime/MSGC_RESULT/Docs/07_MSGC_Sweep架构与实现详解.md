# MSGC Sweep 架构与实现详解

> 当前代码基线：`MSGC-bugfix-regionScan` / `24ca097ace`。当前 Sweep 是三 head membership 模型，不存在 Region-local `sweepGen/sweepState`。

## 1. Mark 到 Sweep 的交接

Region-scan Mark 先完成：

```text
task ledger == 0
REGION/OBJECT ready lanes empty
pendingWordSummary == 0
HAS_REF small Region state == IDLE
required MARK & ~SCAN candidates empty
```

之后：

- `ComputeMarkedStats()` 从稳定 MARK plane 统计 live；
- large object 由 primary large marked flag 判活；
- `PublishCollectSetForSweep()` 将 GC-private collect-set 发布到 unswept head；
- SCAN plane、pending summary 与 Mark task state 不参与 Sweep 判活。

## 2. 三 head 数据模型

每个 `[sizeClass][scanClass]` bucket 当前只有：

| Head | 语义 |
|---|---|
| `partialSwept` | 已 Sweep，仍有可分配 slots |
| `fullSwept` | 已 Sweep，无可分配 slot |
| `unswept` | Post-Mark 发布、等待 Sweep |

状态由 membership 表达。`MSRegionInfo` 中没有旧版 `sweepGen`、`sweepState`、location 或
owner token字段。

- `partialSwept`/`fullSwept` 使用带 ABA tag 的 lock-free Region stack。
- `unswept` 同样是唯一 claim source。
- intrusive link 是 `centralNextRegionIdx`。
- `ClaimUnswept()` 的成功 Pop 就是 Region 的唯一 Sweep ownership。

## 3. Collect-set 生命周期

### 3.1 PreEnum

`BuildCollectSet()` 在 STW 中：

1. 从每个 bucket steal swept heads；
2. 组成 GC-private `collectSet_[36][2]`；
3. 给 member 设置新的 `collectEpoch`；
4. 交换 large `nextSupply` 与 `largeCollectSet`；
5. Mark 期间新 supply 位于 collect-set 外。

### 3.2 Post-Mark

`PublishCollectSetForSweep()`：

1. 把每条 collect-set chain发布到 bucket `unswept`；
2. 设置 `centralSmallUnsweptRegions_`；
3. 重置 pressure/credit；
4. 根据 unswept 与 runway计算 Sweep Assist ratio；
5. 增加 `sweepCycleId_`；
6. 根据 unswept 数发布 `sweepDone_`。

## 4. `SweepRegion()`

对唯一 claim 的 small Region：

1. popcount有效 MARK bits，得到 live count；
2. 若 live 为 0，清零整块 32KB payload并设为全空；
3. 否则逐 word：
   - `validMarkBits = markWord & validMask`
   - `freeBits = ~validMarkBits & validMask`
   - 写入 ALLOC plane
   - 更新 `allocSummary`
   - 合并并清零 dead runs
4. 直接构造 `freeSlots = totalSlots - liveCount` 和对应的 Central route；
5. alloc mode设为 BITMAP，并返回 `MSCentralReturnDecision`。

`live == 0` 是特例：为避免无意义构造 ALLOC plane，`allocSummary` 保持 0，ALLOC plane
可以仍是旧内容；EMPTY 判定直接来自 `liveCount`，不能在此后 popcount ALLOC plane。

MARK plane 不在这里清空，而在下一轮 MarkSetup 的
`ClearAllMarkBitmaps()`/`ResetSmallMarkWorkState()` 中清理。

SCAN plane完全不参与 dead/live 判断。

## 5. Sweep 后重新分类

`SweepUnsweptList()` 或 Assist 单 Region路径在 `SweepRegion()` 后：

```text
collectEpoch = NO_COLLECT_EPOCH
decision = SweepRegion()  // kind + freeSlots
if decision.kind == EMPTY_CACHE:
    PushEmptySweptRegion
else if decision.kind == PARTIAL:
    PushPartialSwept
else:
    PushFullSwept
```

三条 Sweep 消费路径都使用同一个返回结果，不再通过持久化计数或 `HasFreeSpace()` 二次
分类。Force-Finish 的 freed bytes 由 `decision.freeSlots * slotSize` 计算。

空 Region离开 class-local supply，进入全局 classless empty cache；后续可重绑其他
size/scan class或 reclaim 给 FreeManager。

## 6. 五类执行者

| 执行方式 | 入口 | 边界 |
|---|---|---|
| Background | `BackgroundSweep` | deadline slice |
| Parallel helper | `ParallelSweep` | shared pool + deadline |
| Mutator Assist | `MSSweepAssist` / `AssistSweep` | debt/credit + max batch |
| Lazy/refill | central缺货路径 | 有限 claim，制造供给 |
| Force-Finish | `ForceFinishAllUnswept` | GC main，无普通时间片限制 |

所有执行者最终都从同一 `unswept` head claim，因此不会同时 Sweep 同一 Region。

## 7. Background 与 Parallel

Background：

- 先 `TryEnterSweep()`；
- 设置 `bgSweepRunning_`；
- 按 pressure生成 class顺序；
- 扫到 deadline；
- 将完成 Region记录为 background credit；
- `LeaveSweep()`。

Parallel helper：

- 进入统一 admission；
- 增加 `activeHelperSweepCount_`；
- 检查 skip/stop；
- 按 cyclic class order处理 `[cls][scan]`；
- 发布 helper credit；
- 退出 active/admission。

MARK 与 SWEEP 复用 GC worker pool时必须使用互斥 epoch/lease，不能混合消费。

## 8. Mutator Sweep Assist

分配成功按 Region数形成 P-local debt。`MSSweepAssist()`：

1. 先从 `sweepCredit_` 一次性领取 background/lazy/helper 已完成工作；
2. 对剩余 debt按配置判断是否达到 direct batch；
3. 调用 `AssistSweep()`；
4. `AssistSweep()` 在统一 admission下最多 Sweep有限 Regions；
5. 只按实际 paid regions扣减本地 debt。

pressure放大 debt形成速度，不提高单次偿还上限。

## 9. 完成发布

`sweepDone_` 不是“unswept count 瞬时为 0”：

- 所有普通 sweep入口受 `sweepAdmission_` 控制；
- 高位关闭 admission，低位统计 admitted active operations；
- completion 还核对 force-finish与各 active source；
- 最后一个完成者才可发布 done。

独立的 active counter保留诊断/分 lane状态，但统一 admission word避免
“close 后迟到线程才递增 active”的竞态。

## 10. 下一轮 GC 前停止

`StopAndWaitForAllSweep()`：

1. 标记 force-finish running；
2. GC main先 drain当前 unswept；
3. 发布 stop并关闭 Lazy/Assist/Helper fast gate；
4. `CloseSweepAdmissionAndWait()` 阻止新 entrant并等待 admitted source；
5. 再次串行 drain竞态残余；
6. 断言 unswept计数为 0；
7. 发布 Sweep done。

只有之后，PreEnum STW 才能重置 MARK/SCAN/task work state。

## 11. Large object

Large object不进入 small bucket `unswept`。Post-Mark 根据 large collect-set 和 primary marked
flag识别死亡 span：

- live large保留；
- dead large先进入/经过 large hot cache策略；
- 后续可 reclaim 到 FreeManager；
- Scavenge再处理物理页。

## 12. 与 Scavenge

Sweep 产生：

- partial/full swept供给；
- classless empty cache；
- large hot cache。

Scavenge前可先把 central cache物化为 FreeManager hot-free。Sweep负责对象与逻辑复用，
Scavenge负责 OS物理驻留，二者不能合并成一个状态。

## 13. 正确性不变式

1. collect-set 在 Mark 期间是 GC-private sealed chain。
2. Post-Mark 只在 Mark closure 完成后发布 unswept。
3. 一个成功 `ClaimUnswept()` 对应唯一 owner。
4. dead payload在 Region发布回 swept head前已清零。
5. ALLOC plane完整重建后才允许 allocator获取 Region。
6. MARK plane在本轮 Sweep完成前不能清。
7. SCAN plane不参与 Sweep判活。
8. admission关闭后不能出现新 SweepRegion。
9. Force-Finish结束时 unswept reference count也必须为 0。

## 14. 诊断

| 现象 | 首查 |
|---|---|
| 同 Region重复 Sweep | tagged unswept stack、intrusive next |
| unswept为0但未完成 | admission active、force-finish、active counters |
| dead对象未清零 | `SweepRegion` dead-run合并 |
| Region有空间却无法分配 | ALLOC plane、allocSummary、partialSwept |
| 下一轮清 bitmap卡住 | stop/admission/Force-Finish |
| RSS不降 | empty/large cache是否 reclaim、Scavenge阈值 |

## 15. 源码索引

- [`MSCentralRegionPool.h`](../../src/Heap/MAllocator/MSCentralRegionPool.h)
- [`MSCentralRegionPool.cpp`](../../src/Heap/MAllocator/MSCentralRegionPool.cpp)
- [`MSRegionInfo.cpp`](../../src/Heap/MAllocator/MSRegionInfo.cpp)
- [`MSSweepDebtPolicy.h`](../../src/Heap/MAllocator/MSSweepDebtPolicy.h)
- [`MSParallelSweepPolicy.h`](../../src/Heap/MAllocator/MSParallelSweepPolicy.h)
- [`BackgroundProcessor.cpp`](../../src/Heap/MCollector/BackgroundProcessor.cpp)
- [`MarkSweepCollector.cpp`](../../src/Heap/MCollector/MarkSweepCollector.cpp)

## 16. 历史口径

旧文档的 four-list、Region-local sweep generation/state、location/owner side table 与
`ClaimUnsweptHeadForSweep(kind, gen)` 已不属于当前实现。当前是
`partialSwept/fullSwept/unswept` 三 head，membership即状态。
