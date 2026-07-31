# Step45 MSGC 吞吐率与 GC 速度优化点分析

> 状态：部分实施。第 3 章“Mark Worker 控制锁与 GC Main 空转”已于 2026-07-19 落地并完成正确性验证；第 9 章 Sweep 完成状态与第 9.7 节的批量/无锁 unswept 改造已于 2026-07-20 落地并完成构建与契约验证；其余优化点仍是静态热路径审计和后续方案，不代表已经通过 profiler 定量验证。
>
> 目标：记录仍会影响 Mutator 吞吐、Concurrent Mark 收敛、Sweep 清理速度和 Scavenge 页归还效率的实现点，并给出可实施的优化方案、并发不变式和验证方法。
>
> 范围：本 Step 仅设计 Runtime 优化。16KB small/large 边界涉及 LLVM ABI，本文只记录联动要求，不在本 Step 直接修改 LLVM。

## 1. 审计结论与优先级

当前 MSGC 已经将大量逐对象统计改成本地化或周期汇总，因此下一阶段的主要瓶颈不再是简单计数器，而是：

1. Concurrent Mark 中围绕 WorkBuf 流转的全局 mutex 和 Worker 控制 mutex。
2. Sweep 中频繁的 size-class pressure 排序、完成判定和 2ms 短任务调度。
3. Scavenge 在 shard 锁内执行 OS 页操作，以及 macOS 路径的重复写零。
4. Barrier 和对象图遍历中的过度扫描、重复地址校验和已标记对象的无效检查。
5. Region 级慢路径上仍然存在的全局快照扫描和失效的批量补货阈值。

### 1.1 总优先级

| 优先级 | 优化点 | 主要影响 | 发生频率 | 修改风险 |
| --- | --- | --- | --- | --- |
| P0 | Mark Worker 控制锁与 GC Main 空转 | Concurrent Mark CPU、锁竞争、收敛速度 | 每个 WorkBuf/空队列轮次 | 中高 |
| P0 | Global WorkBuf 单 mutex 与 `PopFull()` 锁内扫描 | GC Main/Helper/Assist/Barrier 共享竞争 | 每次 publish/pop/pool get-put | 高 |
| P0 | Sweep class 排序与 2ms 任务调度 | Background/Parallel/Assist Sweep 有效工作占比 | 每个 slice、每个 Helper | 中 |
| P0 | Scavenge 锁内页释放 | 阻塞同 shard Region 分配 | 内存压力下每个释放 run | 高 |
| P0 | Bulk Barrier 过度扫描与大栈对象 | Mutator bulk write 吞吐、额外 Mark 工作 | 每次 struct/ref-array copy | 中 |
| P1 | `DiscoverObject()` 重复校验 | 对象图每条引用边的 Mark 成本 | Concurrent Mark 全程 | 高 |
| P1 | fresh Region 前后的 Mark/Scavenge 快照 | 分配慢路径吞吐 | 约每 32KB Region | 低 |
| P1 | Per-P empty cache 补货阈值 | 高 P 数下 Global empty 锁竞争 | 每次 Region refill | 中 |
| P1 | `SweepRegion()` 逐 slot 清零 | Sweep CPU 和内存带宽 | 每个部分存活 Region | 中 |
| P1 | 旧 Sweep completion probe 扫描 72 个 counter | Lazy/Assist/Helper 小批次尾部成本 | 每次 Sweep 执行方退出 | 中 |
| P1 | 16KB Small/Large 边界 | 16KB 对象落入全局 Large 路径 | 精确 16KB 对象负载 | 高，涉及 LLVM ABI |
| P2 | Background Scavenge 重复快照 | gc-helper CPU 和响应速度 | 每个 Scavenge 轮次 | 低 |
| P2 | Large hot cache 全局锁和周期清空 | Large churn 吞吐和跨周期复用 | Large-heavy 负载 | 中 |
| P2 | `ComputeMarkedStats()` 额外 bitmap 遍历 | PostMark CPU，Sweep 又遍历一次 | 每个 GC cycle | 高 |
| P2 | OHOS `VLOG(REPORT)` 无条件输出 | 日志 IO 和格式化成本 | OHOS 上每个 GC/Sweep slice | 低中 |

### 1.2 如何理解 P0/P1/P2

- **P0** 表示路径位于默认启用的 Concurrent Mark、Background/Parallel Sweep 或 Background Scavenge，且会引入共享锁、忙等待或 OS 调用。
- **P1** 表示发生频率较高，但单次成本较小，或者优化会触及 stale edge、mark bitmap 等正确性边界。
- **P2** 表示负载特定、平台特定，或实施收益需要先用 profiler 证明。

P0 并不意味着必须一次性全部修改。Mark WorkBuf 和 Scavenge 锁状态都是正确性敏感区，实施时仍要按第 18 节拆分成小步。

## 2. 优化边界与基本原则

### 2.1 以正确性不变式为边界

MSGC 的热路径同时承担并发正确性。任何性能优化都不能仅以“少一次检查”为目标，而必须明确检查被哪个更便宜的不变式代替。例如：

- `DiscoverObject()` 中的 `IsValidObject()` 用于拒绝已 Sweep 的 stale slot，不能直接删除。
- `LFStack` 使用 mutex 是为了规避 WorkBuf 复用时的 ABA，换成 Treiber stack 时必须同时提供 tagged pointer/hazard 机制。
- Scavenge 释放页移到锁外后，Region 必须在整个 OS 调用窗口内不可被 allocator 重新取走。
- Sweep completion 不能仅根据 unswept count 置 done，仍需确认 admission 已关闭且所有 in-flight sweep 都为 0。

### 2.2 只让快路径承担必要工作

本 Step 把路径分为三级：

| 级别 | 示例 | 优化原则 |
| --- | --- | --- |
| 逐对象/逐引用边 | Barrier、`DiscoverObject()`、slot allocation | 只保留常数时间检查；不遍历 class/shard；避免 mutex |
| 逐 Region/WorkBuf | Region refill、SweepRegion、WorkBuf publish | 可以批量化和使用本地统计；避免扫描全局状态 |
| 逐 cycle/slice | pressure 快照、live bitmap 统计、日志 | 可遍历全局，但要生成可复用快照，不让每个 Worker 重复遍历 |

### 2.3 用批量和快照代替重复全局访问

本文中的多个问题具有同一模式：全局真实状态由 36 个 class、N 个 shard 或 N 个 Worker 组成，但热路径每次都重新遍历它们。优化时应优先选择：

1. slice/cycle 边界生成一次 pressure/retained/in-flight 快照。
2. 事件发生时增量维护一个全局总数，只在总数达到边界时做完整校验。
3. 一次获取/归还一条 WorkBuf/Region 链，而不是在循环中重复获取同一把锁。

### 2.4 默认开关与审计假设

2026-07-19 当前默认值为：

- `cjMSBackgroundSweep=true`。
- `cjMSParallelSweep=true`。
- `cjGCBGSweepMaxTime=2ms`。
- `cjMSBackgroundScavenge=true`。
- `cjMSBackgroundShrink=false`。
- `cjMSVerifyMark=false`。
- `cjGCInterval=0`，默认不使用 HEU cooldown gate。

因此 Background/Parallel Sweep 和 Scavenge 的问题属于默认路径；`VerifyMarkCompleteness()` 则属于明确的诊断路径，本 Step 不把它列为常规吞吐瓶颈。

### 2.5 需要实测，不能把静态审计当成性能结论

文中的优先级是根据发生频率、锁/扫描复杂度和默认启用条件推断的。实施前后必须使用同一 SDK、同一 `.cj` 重编译流程和隔离环境测试，否则不得宣称优化有效。

## 3. 优化点一：Mark Worker 控制锁与 GC Main 空转

### 3.1 优化前实现

Mark Helper 的单轮路径在 `MarkSweepCollector::RunMarkWorker()`：

```text
WaitUntilEligible(workerId)       // lock MSMarkWorkerControl::mutex_
    -> TryEnterWorker(workerId)   // 再 lock 一次，workersInFlight_++
    -> DrainOneBufRound()
       -> globalFull_.Pop()       // lock LFStack::mutex_
       -> 扫描一个 WorkBuf
       -> WorkBufPool::Put()      // lock freeList_.mutex_
    -> ExitWorker()               // 第三次 lock，workersInFlight_--，notify_all
    -> scanned == 0 ? BackoffWorker(50us)
```

`WaitUntilEligible()` 的 predicate 在 Worker 已经 eligible 时会立即返回，但仍需要获取控制 mutex。因此 Helper 每处理一个 WorkBuf 至少进入三次 `MSMarkWorkerControl::mutex_`，还不包括 Global WorkBuf 和 WorkBufPool 的锁。

Mutator Mark Assist 调用 `TryEnterAssist()/ExitAssist()`，与 Helper 争用同一把控制 mutex。GC Main 的 `RunConcurrentMarkLoop()` 在每个循环轮次中又会分别调用：

- `globalFull_.GetCounts()`。
- `globalFull_.Empty()`。
- `GetWorkersInFlight()`。
- `GetAssistsInFlight()`。
- `TakePublishedWorkerCredit()`，遍历所有 Worker slot 做 atomic exchange。

当 Global 队列刚好为空、但某个 Helper 仍在扫描对象图时，收敛条件因 `workersInFlight != 0` 不成立。GC Main 没有 yield/backoff，会继续循环执行上述锁和原子操作，与真正工作的 Helper 争抢 CPU 和 cache line。

### 3.2 优化目标

1. Worker 处理连续 WorkBuf 时不进入控制 mutex。
2. Assist 入口只做常数次 atomic 操作。
3. mutex/CV 仅用于 Worker 真正休眠、目标 Helper 数变化和 quiesce 等待。
4. GC Main 在“无全局工作但有 in-flight”时不忙等待，同时不影响 Barrier 新 publish 的响应速度。

### 3.3 已实施方案（2026-07-19）

#### 第一步：合并只读状态快照

新增一次返回 `phase/targetHelpers/workersInFlight/assistsInFlight` 的 `GetSnapshot()` 接口，GC Main 用一份快照完成本轮收敛判断，不再连续获取两次控制锁。同时：

- `assistWorkPerByteScaled_` 仅在新值与已发布值不同时 store，避免每轮写入 Mutator 会读取的 cache line。
- Worker credit 改成 GC Main 本轮实际处理了对象、backlog 变化或每 8 轮之一时收集，不再每次空轮询都扫描全部 Worker slot。

这一层已落地在 `RunConcurrentMarkLoop()`。

#### 第二步：原子化 phase/target/in-flight 快路径

控制面已拆成：

```text
atomic<Phase> phase_
atomic<size_t> targetHelpers_
atomic<size_t> workersInFlight_
atomic<size_t> assistsInFlight_
mutex + condition_variable       // 只处理休眠和 quiesce
```

Worker 进入协议：

```text
load phase/target
if phase != RUNNING || workerId >= target: 进入等待慢路径
workersInFlight.fetch_add(1, acquire)
recheck phase/target
if 已经 QUIESCING/STOPPED:
    workersInFlight.fetch_sub(1, release)
    return false
return true
```

`BeginQuiesce()` 先用 release store 发布 `QUIESCING`，然后等待两个 in-flight counter 归零。Worker/Assist 的正常进入、退出只做 atomic；quiesce 期间每个 claimant 退出都会通过控制 CV 唤醒 `WaitForNoInFlight()`。这里不依赖两个独立 atomic 恰好同时读到 0，从而规避跨计数快照陈旧造成的极低概率漏唤醒；真正是否归零仍由 CV predicate 判断。

#### 第三步：GC Main 的有界等待

当满足：

```text
global queue empty
gc main private input empty
workersInFlight + assistsInFlight > 0
```

GC Main 先做 8 轮 `yield`，仍然满足“Global 为空但有 in-flight”时，在 `WorkEpoch` 上最多等待 50us。`globalFull_.Push()`、Worker/Assist 总 in-flight 归零、target 和 phase 变化都递增 epoch 并唤醒 GC Main。

这不是固定 sleep：每次等待前先捕获 epoch，任何并发 publish 都会使 predicate 成立并立即返回；50us 只是丢唤醒防护上限。`WorkEpoch::Notify()` 在无 waiter 时只做 atomic，不获取 CV mutex。

#### 第四步：MARK 恢复共享线程池宽度

Sweep slice 会用 `SetMaxActiveThreadNum(helperCount)` 降低共享池宽度。`StartMarkWorkers()` 现在在持有 MARK lease、提交长生命 task 之前恢复：

```text
pool.maxActiveThreadNum = maxHelpers
```

这里不能使用 `initialHelpers`。物理池宽度要允许所有潜在 slot 启动，当前真正可扫描的 slot 仍由 `targetHelpers_` 控制。不做按需提交 task。

### 3.4 风险与验证

- 必须验证 `BeginQuiesce()` 与“Worker 读到 RUNNING 后尚未 in-flight++”的竞争，不能让 MarkTermination 早于 Worker 退出。
- 动态 Helper 只允许周期内扩容，现有“不在 cycle 内缩容”的收敛假设不能被破坏。
- 记录每周期 `control-lock acquisitions` 可以作为临时实验计数，但不能以共享热原子的形式长期保留。
- 压测必须包含：空队列尾部、高 fan-out 对象图、高 Mutator Assist 比率和强制 GC 中断 Mark 四类竞争。
- 已新增 `MSGCMarkControlFastPathContractTest.sh`，并扩展 `MSMarkWorkerControlTest.cpp`、`MSWorkBufInvariantTest.cpp`，覆盖 snapshot、target 唤醒、quiesce 等待和 epoch 唤醒。
- 2026-07-19 已通过 macOS aarch64 MarkSweep Release Runtime 构建。高扇出对象图用例在 `cjMSParallelSweepMaxHelpers=1` 下连续触发 GC：最终产物观察到 4 次 `parallel-sweep ... activeHelpers=1`，随后 Mark Helper 单周期处理约 14.7 万至 81.5 万对象，证明 Sweep 留下的单线程池宽度不会再限制下一轮 Mark；用例最终 `PASS`。
- 同方案前一轮 HTTP 并发回归用例完成 4066/4066 次请求、失败数为 0；最终 quiesce 唤醒收口后的产物则由上述高扇出、重复 GC 用例验证。上述结果验证的是并发协议与 Helper 可参与性；吞吐、GC CPU 和 Mark 尾延迟收益仍需在同 SDK/同用例条件下做优化前后定量对比，不能仅根据静态实现宣称提升。

## 4. 优化点二：Global WorkBuf 队列与 WorkBufPool 批量化

### 4.1 优化前实现

`src/Common/LFStack.h` 中的 `LFStack` 名字保留了 lock-free 历史含义，但当前 `Push()`、`Pop()`、`PopFull()`、`Empty()` 和 `GetCounts()` 全部使用同一把 `std::mutex`。这是一个有意的正确性选择：WorkBuf 被积极复用，没有 ABA 防护的普通 Treiber stack 不安全。

当前 Global 结构中可以同时存在：

- Barrier/PerPGCWork publish 的满 WorkBuf。
- root tail、Mutator exit 等路径 publish 的 partial WorkBuf。

Mutator Assist 只能消费满 WorkBuf，因此 `PopFull()` 会持锁从链表头开始跳过 partial buffer，直到找到第一个 `len == 1024` 的节点。partial tail 越多，每次 Assist 在全局锁内遍历的链越长。

WorkBufPool 的 `GetN(out, 8)` 看似批量获取，但内部循环调用八次 `freeList_.Pop()`，即获取和释放同一把 mutex 八次。`DrainLogCacheToPool()` 也会逐个 `Put()`。

### 4.2 已实施方案（2026-07-19）

#### 阶段 A：保留 mutex，实现真正的批量操作

`LFStack` 新增：

```text
PopN(out, maxCount)
PushChain(head)
```

两个操作都只获取一次 mutex。`PushChain()` 在持锁前遍历调用方独占的链，按 full/partial 拆分并自行计算 total/full 数量；调用方不传 count，避免错误元数据破坏全局计数。

`WorkBufPool::GetN()` 现在通过一次 `freeList_.PopN()` 摘下最多 8 个空 buffer，只对不足部分执行 `new`。`AllocBufferMS::DrainLogCacheToPool()` 先脱离整个 P-local `logCache_`，再由 `WorkBufPool::PutChain()` 重置并用一次 `PushChain()` 归还，不再逐节点调用 `Put()`。

批量 Global publish 最多递增一次 `WorkEpoch`；当前批量归还只用于不绑定 epoch 的 Pool free list。

#### 阶段 B：单 mutex 下分离 full-ready 和 partial-ready

`LFStack` 保留一把 mutex，但内部拆成两个逻辑 head：

```text
fullTop_       // len == kCapacity
partialTop_    // root tail / exit / STW handoff，以及 Pool 中的 empty buffer
```

`Push()` 根据 `WorkBuf::Full()` 自动选 lane，所以 Collector、Barrier、`PerPGCWork` 的 publish 调用点不需要传额外类别：

- Barrier activeLog 和 `PerPGCWork::putCur_` 满时进入 `fullTop_`。
- root tail、Mutator exit log、Dispose tail 进入 `partialTop_`。
- Mutator Assist 的 `PopFull()` 直接摘 `fullTop_`，由 O(partial 数量)扫描降为 O(1)。
- GC Main/Helper 的 `Pop()` 优先 full，再消费 partial。
- `Empty()` 和 `GetCounts()` 在同一把锁下同时读取两个 lane，保留一致的收敛/backlog 快照。
- MarkTermination 现有 `Pop()` 固定点循环自然排空两个 lane，不需要新的 drain 路径。

这一实现消除了 Assist 的链内线性搜索，但没有消除 Global publish/pop 对同一 mutex 的竞争。

#### 阶段 C：本轮明确不实施

不引入 tagged pointer、Treiber stack 或双 mutex Global。WorkBuf 会在 Global 与 Pool 间快速复用，保留 mutex 可以继续规避 ABA；单 mutex 也使 total/full 快照保持一致。剩余锁竞争是否值得进一步修改，必须先通过 profiler 证明。

### 4.3 ABI 约束

`WorkBuf::objs` 当前保持在 offset 16，AArch64 MSGC 写屏障 fastpath 使用固定偏移访问 `len` 和 `objs`。队列拆分可以改变队列容器，但不能在未同步修改 LLVM lowering 和 ABI 测试时改变 WorkBuf 前两个字段的布局。

### 4.4 验证重点

- `MSWorkBufInvariantTest.cpp` 覆盖 full 优先、partial 保留、Assist 只取 full、批量 Pop、Pool 整链归还/重取、Dispose tail 和一次 batch epoch 通知。
- `MSGCWorkBufBatchDualLaneContractTest.sh` 检查双 head、Pool 批量接线、旧逐节点 drain 已移除，并确认 activeLog/WorkBuf 四项 ABI assertion 保留。
- 本轮按要求只执行聚焦单元测试和源码契约，不做 Runtime 整体构建或业务 benchmark；因此这里验证的是语义、计数和 ABI 源码约束，不宣称已有端到端吞吐收益。

## 5. 优化点三：Bulk Barrier 扫描范围与批量标记器

### 5.1 当前实现

> 2026-07-31 更新：本节早期记录的 `dstLen/dstSize` 过扫描和栈上
> `BulkRefMarker` 已被后续实现替换。

Mark 阶段的 bulk write 在覆盖目标内存之前保留旧引用。当前 `MSMarkBarrier.cpp`
先检查 source 长度不超过 destination 容量，再按实际覆盖长度扫描：

| 操作 | 旧引用扫描范围 | 实际覆盖范围 |
| --- | --- | --- |
| `WriteStruct()` | `srcLen` | `srcLen` |
| `CopyRefArray()` | `srcSize` | `srcSize` |
| `CopyStructArray()` | `srcSize` | `srcSize` |

`dstLen/dstSize` 只作为 `memcpy_s/memmove_s` 的目标容量。旧实现对未覆盖尾部的
额外扫描已经删除，TSAN range 也统一使用 `srcLen/srcSize`。

当前 bulk 与 scalar 共用 non-owning `BarrierMarkContext` 和持久
`MSBarrierMarkBuffer`，不再构造约 1KB 的栈上 `BulkRefMarker`。bulk 构造 context 时
只判断一次 phase/collector，循环内使用 `AddOldRefActive()`。

### 5.2 扫描范围优化

当前实现不再通过 `min` 掩盖非法参数，而是在扫描旧引用前检查：

```cpp
CHECK_DETAIL(srcLen <= dstLen, ...);
```

随后只扫描 `[dst, dst + srcLen)`。`CopyRefArray/CopyStructArray` 使用相同的
`srcSize <= dstSize` 契约；尾部旧引用仍留在目标中，不因本次 copy 被额外 discover。

### 5.3 小批量与大批量分层

> 历史备选，当前不再采用。持久 `MSBarrierMarkBuffer` 已统一 scalar/bulk，
> 不再按引用数量构造不同的栈上 marker。

建议把 bulk marker 拆成两条路径：

```text
refCount <= smallBulkThreshold
    -> 逐 ref 调用已知 owner 的 ShadeAndAppend
    -> 不构造 1KB BulkRefMarker

refCount > smallBulkThreshold
    -> BulkRefMarker 按 bitmap word 合并 atomic OR
```

`smallBulkThreshold` 不应凭经验写死，可从 8/16 两个候选值做 microbenchmark。比较指标包括 1、2、4、8、16、64、256 个 ref 的 copy 时间、mark CAS 数、activeLog publish 数和栈占用。

大批量路径可进一步将 `refs[64]` 改成紧凑的“置位引用列表”：

```text
Entry {
    MSRegionInfo* info;
    uint16_t wordIdx;
    uint64_t mask;
    BaseObject* refs[kActualRefs];
    uint8_t bitIndexes[kActualRefs];
}
```

但这会增加 Add/Flush 的索引处理，只能在证明 1KB 初始化仍是瓶颈后实施。第一阶段优先实现扫描范围修正和 small-bulk 分层。

### 5.4 Atomic Barrier 的同值写入

`AtomicSwapReference()` 能够获得 exchange 真正替换出的 `oldRef`；如果 `oldRef == newRef`，没有删除任何边，可跳过 shade。`CompareAndSwapReference()` 成功且 `oldRef == newRef` 时也可跳过。

`AtomicWriteReference()` 不能直接使用“先 load，如果 old==new 就不记录”的优化：在 load 和 store 之间，其他线程可能写入另一个引用，本次 store 真正删除的将不是 load 观察到的值。如果要获得精确 SATB 旧值，Mark barrier 下应评估用 exchange 取代 load+store，并单独评估语义和性能；不能只为了少一次 shade 而引入丢边竞争。

### 5.5 正确性和验证

- 先 shade old ref，再覆盖目标内存的顺序不能改变。
- Bulk bitmap OR 仅能将 white 转 grey，只有 `MarkBitmapWord()` 返回的 new bits 能进 activeLog。
- 要覆盖 overlap array copy、struct 内嵌 ref bitmap、RawArray struct、static struct 和 atomic 并发写。
- 需要将 floating-garbage/liveBytes 一并纳入观测，不只比较 barrier 本身耗时。

## 6. 优化点四：DiscoverObject 重复地址校验与已标记快路径

### 6.1 优化前调用链

普通对象图扫描中，每个 ref field 经过：

```text
TraceObjectRefFieldsToGCWork()
    -> target != nullptr
    -> IsManagedAddress(target)           // Heap::IsHeapAddress
    -> DiscoverObject(target)
       -> IsTraceableObjectAddressEx()
          -> IsManagedAddress(target)     // 再次 heap-range check
          -> GetRegionInfoValidated()
          -> Region type / large base
          -> Region 范围 / slotSize / slot alignment / slotIdx
          -> bump allocPtr 边界
       -> target->IsValidObject()         // stateWord/typeInfo 校验
       -> MarkObjectInHeapKnown()         // mark bit test-and-set
```

Runtime 单引用 barrier fallback 还有一层重复：

```text
EnqueueRefFast()
    -> Heap::IsHeapAddress
    -> EnqueueRefInto()
       -> Heap::IsHeapAddress             // 第二次
       -> ShadeRefForBarrier()
          -> DiscoverObject()
             -> IsManagedAddress          // 第三次
```

LLVM inline barrier 会承担部分快路径，因此这一问题的 Mutator 影响取决于 runtime fallback 比例；但 `TraceObjectRefFieldsToGCWork()` 是 GC 对象图遍历的常规路径，对高连接度对象图会直接影响 Mark 速度。

### 6.2 为什么不能直接删除 `IsValidObject()`

MSGC 已经出现过 stale edge 指向已 Sweep/清零 slot 的问题。这类地址可能：

- 仍位于合法 heap range。
- 仍按当前 size class slotSize 对齐。
- 所在 Region 仍然是 PARTIAL/FULL，而不是 FREE。
- 但 slot 内 stateWord 已经清零，不再是合法对象。

因此仅凭范围和对齐不足以保证可跟踪。优化必须保留对 stale free slot 的拒绝能力。

### 6.3 已实现：显式区分已知前置条件的入口

当前实现增加了 Runtime 内部的 known-managed helper，并由单一安全入口统一普通扫描：

```text
DiscoverObject(obj)                  // 公共安全入口，什么都不假设
DiscoverManagedObject(obj)           // 调用者已验证 Heap::IsHeapAddress
ShadeManagedRefForBarrier(obj)       // Barrier 已验证 heap range
```

`TraceObjectRefFieldsToGCWork()`、`EnumRefFieldRoot()` 和 `EnumAndTagRawRoot()` 已直接调用公共
`DiscoverObject()`，不再在调用前重复 `IsManagedAddress()`。Barrier 仍在 `EnqueueRefFast()` 最外层保留一次
heap-range gate，以免非堆引用继续获取 collector/TLS/AllocBufferMS；进入 `EnqueueRefInto()` 后使用
`ShadeManagedRefForBarrier()`，不再重复 heap-range 检查。

这类 helper 应保持 `static inline`/私有，不应扩张成外部 ABI，否则不可验证的前置条件会扩散到整个 Runtime。

### 6.4 已实现：一次解析与已标记快路径

优化前顺序是“先 `IsValidObject()`，再 test-and-set mark bit”。高入度对象在第一次标记后，后续每条指向它的边仍会读 stateWord/typeInfo。

当前实现使用私有 `ResolvedMarkTarget`，Small object 保存 `MSRegionInfo* + wordIdx + bitMask`，Large object
保存 canonical base entry。解析阶段一次完成 Region、slot 和 bitmap 坐标计算，后续只复用解析结果：

```text
1. 验证 heap range + Region + slot 对齐/范围
2. BUMP Region 使用 allocPtr 拒绝尚未分配的尾部 slot
3. relaxed load 测试 mark bit / Large marked flag
   - 已 marked：直接返回 false
   - 未 marked：继续
4. IsValidObject()
5. 使用已解析的 wordIdx/bitMask 执行 atomic fetch-or
```

这里明确不读取 `allocBitmap`：PARTIAL Region 的 `AllocFromBitmapFast()` 当前对 allocation bitmap 使用普通读写，
Concurrent Mark 新增并发读取需要额外同步协议。已 marked 候选直接返回 `false`，调用方不会执行
`HasRefField()` 或加入 work queue；white 候选仍必须通过 `IsValidObject()`，所以被 Sweep 清零的 stale slot
不能赢得 mark bit。每轮 Mark 前又会先停止 Sweep 并清空全部 mark bitmap/large flag，因此 marked probe
属于当前 cycle。

Small 的最终更新复用 `MSRegionInfo::MarkBitmapWord()`，不再进入按地址重新计算 slot 的
`TestAndSetMarkBit()`；Large 使用 canonical base entry 的 `IsMarked()` 与 `AtomicTestAndSetMarked()`。
多个线程同时看到 white 仍然安全，只有 atomic OR winner 返回 newly-marked。

### 6.5 优化方案 C：STW activeLog 的重复验证

Barrier Route-A 在写入 activeLog 前已经完成 shade，log 按设计只包含 trusted grey object。Concurrent consumer 在 Release 版本中不重校验，但 `FlushAllWriteBarrierBuffers()` 在 MarkTermination STW 中仍对每个 activeLog entry 执行 `IsTraceableObjectAddress()+IsValidObject()`。

这一部分只处理每个 P 未 publish 的 partial tail，不是 Concurrent Mark 主成本，但会增加 STW 尾部。可在充分验证 Route-A invariant 后：

- Release 版本直接 `PushMovedGrey()`。
- Debug/诊断版本保留重校验与 fatal/report。

由于历史上存在 invalid stateWord/stale edge 问题，这项应放在地址检查分层和长时间并发回归之后，不建议作为第一批改动。

### 6.6 验证指标

- 每个 scanned ref 的 `IsHeapAddress/GetRegionInfoValidated/IsValidObject` 调用数。
- 已 marked 快路径命中率，按对象图入度分类。
- `invalid stateWord`、mark completeness、stale managed target 诊断不得出现新增。
- case27/case28 等历史 barrier/stale-edge 用例需要多轮、高并发回归，不能只跑 hello/gc.cj。

## 7. 优化点五：Region 获取路径上的 Mark/Scavenge 压力统计

### 7.1 优化前实现

`MSRegionManager::TakeRegions()` 在每次调用 FreeManager 前执行 `MaybeNotifyMark()`，在成功取得 Region/span 后执行 `MaybeNotifyScavenge()`：

```text
TakeRegions(num)
    -> MaybeNotifyMark()
    -> freeManager_.TryTakeRegions()
    -> SUCCESS
    -> MaybeNotifyScavenge()
```

这不是逐对象路径，而是逐 fresh Region/span 路径。小对象通常每消耗一个 32KB Region 进入一次，Large 分配则每个 span 进入一次。

`MaybeNotifyMark()` 原先每次先读取纳秒时钟和 `lastHeuRequestNs_`。但 `cjGCInterval` 默认为 0，表示不启用 interval/cooldown gate，所以默认配置下会继续调用 `GetSweptHeapBytes()`，遍历 36 个 class 的 partial/full swept count 及 large live span count。也就是说，优化前默认情况下 Mark threshold probe 是每个 fresh Region 一次全局 class 扫描，而不是低频路径。

`MaybeNotifyScavenge()` 原先随后调用 `RegionSpaceMS::GetHeapRetainedBytes()`：

```text
active      = GetActiveMSRegionCount()       // 36 class × 4 类 list count
empty       = GetEmptySweptRegionCount()
largeCache  = GetLargeHotCacheBytes()
hotFree     = GetHotFreeBytes()              // 汇总全部 FreeManager shard
```

`GetActiveMSRegionCount()` 单次包含约 145 个 atomic load，之后还要遍历 shard hot-free 状态。高分配速率下，一次 fresh Region 获取会先执行 Mark swept 快照，成功后又执行 Scavenge retained 快照。这些“为了决定是否唤醒后台工作”的全局扫描直接处于 Mutator Region 慢路径。

### 7.2 优化方案 A：Region 级采样门槛

最低风险的做法是在 `MSRegionManager` 中维护本地/分片分配进度，仅每 N 个 Region 或每 M 字节做一次精确 Mark/Scavenge 检查。例如：

```text
allocatedRegionSample += num
if allocatedRegionSample < 32:
    return
allocatedRegionSample = 0
exactRetained = GetHeapRetainedBytes()
```

N 必须同时与 heap threshold 和 scavenge threshold 的最小越界容忍相匹配。若 N=32，最多延迟约 1MB 的小 Region 分配后才提交 HEU/唤醒 Scavenge，通常远小于 threshold，但必须在小堆配置下单独验证。Large span 一次可消耗很多 Region，应按 `num` 累加并在单次越过采样字节阈值时立即 probe，不能把一个大 span 当成“一次分配”。

为避免多 P 同时越界后全部计算快照，可使用一个 coalescing atomic epoch：只有抢到本次 probe 权的 P 进行完整计算，其他 P 继续分配。

### 7.3 已实现：增量 retained 快照

当前已在 CentralRegionPool/FreeManager 状态迁移时维护聚合 counter：

- `centralSmallActiveRegions_`：四条 small Central 链的 Region 总数。
- `centralSmallSweptRegions_`：partial/full swept 两条链的 Region 总数。
- `totalHotFreeRegions_`：所有 FreeManager shard 的 hot-free Region 总数。
- empty swept、large live/cache 继续复用已有 `emptySweptCount_`、`largeLiveSpanCount_`、
  `largeHotCacheBytes_`。

Mutator 只读取少量聚合 atomic：`GetSweptMSRegionCount()` 读取 small swept aggregate 与 large live，
`GetActiveMSRegionCount()` 读取 small active aggregate 与 large live，`GetHotFreeBytes()` 读取全局 hot-free
aggregate。原来的 36 class/全部 shard 遍历分别保留为 `*Reference()` API。

SizeClassBucket 的 Push/Take/Claim 负责单 Region 增减；epoch flip 和 reclassify 比较操作前后的 swept list
count，只按 delta 批量调整 `centralSmallSweptRegions_`，因为 Region 仍留在 Central，active aggregate 不变。
FreeManager 在 shard lock 下刷新 `snapHotFreeRegions_` 时通过 `exchange()` 获得旧值，再将差值汇入
`totalHotFreeRegions_`；Grow、Take、Reclaim、Scavenge、Shrink 已统一经过这一发布点。

`MaybeNotifyMark()` 与 `MaybeNotifyScavenge()` 先读 fast aggregate；只有 fast 值越过 threshold 时才执行
reference scan 确认，避免跨多个 atomic 的瞬时高估触发多余 GC/Scavenge。Background Scavenge 在计算
释放目标和可释放 hot-free 时直接使用 reference snapshot。Mark 与 Scavenge 各有一个 CAS probe-owner
gate，同一时刻只有一个 P 执行 reference scan，其他 P 直接继续分配。

本轮保持原有 retained 口径：`GetActiveMSRegionCount()` 仍只包含 Central 四条链和 large live spans，
不包含 P active Region。这样本轮是读取方式优化，不同时改变 GC/Scavenge threshold 语义；若后续要将
P active 纳入 retained，应作为独立策略变更验证。

### 7.4 验证

- 对比优化前后每 GB 分配的 `GetHeapRetainedBytes()` 调用数。
- 对比每 GB 分配的 `GetSweptHeapBytes()` 调用数；默认 `cjGCInterval=0` 时也必须能通过采样/聚合快照降频。
- 记录 heap/scavenge threshold 首次越界到 `RequestGC/NotifyToScavenge()` 的延迟字节和时间。
- 在 1P/8P/64P 下比较 Region allocation slowpath 时间。
- 验证快照误差不会使 Scavenge 永久不被唤醒，也不会在 threshold 以下持续空唤醒。

## 8. 优化点六：Per-P Empty Region Cache 补货阈值

### 8.1 当前实现

P 的 `AllocBufferMS` 保留 `localEmptyRegionCache_`，容量上限为 36。当本地 cache 为空时，`RefillLocalEmptyRegionsFromGlobal()` 计算：

```text
globalCount = central_->GetEmptySweptGlobalCount()
refillThreshold = max(1, allocBufferCount) * 36
if globalCount <= refillThreshold:
    不批量补货
else:
    refillCount = min(36, globalCount - refillThreshold)
```

这等价于“只有 Global 先保留足够给每个 P 填满 36 个 Region 之后，当前 P 才允许批量拿取”。例如 64P 时阈值为 2304 个 Region，约 72MB。在大多数堆配置下 Global empty 很难超过该阈值，批量路径实际上被关闭。

之后 `GetCacheRegion()` 会退化为 `TakeGlobalEmptyRegion()`，每个 32KB Region 单独访问 Global empty 结构。另外，每次 refill 还会：

- 通过 `GetAllocBufferCount()` 获取 `msAllocBufferLock_`。
- 在栈上创建一个 `std::vector<MSRegionInfo*>`，批量成功时可能分配内存。

### 8.2 优化目标

1. 本地 cache 从空到有的 refill 至少能拿到一个小批次。
2. Global 仍保留最低公平供应，不允许活跃 P 将全部 empty Region 长期囤积在本地。
3. 不在每次 refill 中读取需要 mutex 保护的 P 总数。
4. 避免临时 vector 分配。

### 8.3 建议策略

将“每 P 满配保留”改成“Global 最低保留 + 当前 P 低水位补货”：

```text
localLowWatermark = 0
localRefillTarget = 8 or 16
globalReserve = max(activePCount, fixedMinimum)

available = globalCount > globalReserve ? globalCount - globalReserve : 0
take = min(localRefillTarget, available)
if take == 0 && globalCount > 0:
    take = 1
```

`localRefillTarget` 不等于 cache 最大容量 36。一次填满 36 个 Region 会增加本地囤积和 Scavenge 回收难度，8/16 更适合作为初始候选值。

P 数应通过已有 registry 发布的 atomic snapshot 获得，或者只在 P 注册/注销时更新，不在 refill 时获取 `msAllocBufferLock_`。

批量容器可使用 `std::array<MSRegionInfo*, kLocalRefillMax>` 加 count，或让 Central 直接返回 Region 链，避免 vector heap allocation。

### 8.4 公平性与回收

本地 cache 是 Per-P 优化，但后台 Shrink/Scavenge 需要能够 drain。实施时要保留：

- P 退出时将本地 empty Region 返回 Central。
- 内存压力下 Central 能访问或请求 drain P cache。
- `emptySweptCount_` 在 Global/Local 之间移动时不重复计数，只在离开整个 empty-cache 体系时减少。

### 8.5 验证

- 记录 `batchRefill/singleGlobalTake/freshFreeManagerTake` 比率，计数使用 P-local 周期汇总。
- 在 1P/8P/64P 下测试小对象纯分配吞吐。
- 使用非均匀负载：少数 P 高速分配，其他 P 空闲，确认不会为空闲 P 保留大量 Global supply。
- 确认 Scavenge/Shrink 仍能在达到 threshold 后回收本地 cache 中的 Region。

## 9. 优化点七：Sweep Size Class 排序与完成发布

### 9.1 当前 class order 生成

`BuildBackgroundSweepClassOrder()` 为 36 个 size class 创建数组并调用 `std::sort`。比较器每次对 lhs/rhs 调用 `GetBucketPressure()`，每个 pressure 又读取：

```text
partialSwept
fullSwept
partialUnswept
fullUnswept
```

因此一次比较需要约 8 个 atomic load，36 个元素的排序会产生约千级 atomic load。更重要的是：

- `BackgroundSweep()` 生成一次 order。
- 每个 `ParallelSweep()` Helper 各自生成一次 order。
- Mutator `AssistSweep()` 即使最多只 Sweep 4 个 Region，也会先排序全部 36 class。

排序期间各 count 仍可并发变化，比较器读到的 lhs/rhs 不一定来自同一时刻，所以额外 atomic load 也没有换来严格一致的顺序。

### 9.2 方案 A：pressure 快照后排序

在 slice/assist 入口一次性读取：

```cpp
struct ClassSweepSnapshot {
    uint8_t cls;
    size_t ready;
    size_t unswept;
    bool preferred;
};
std::array<ClassSweepSnapshot, 36> snapshot;
```

`std::sort` 只比较 snapshot 中的普通数值，不再在 comparator 中访问共享 atomic。这不仅降低成本，还使排序基于同一次逻辑快照。

Parallel Sweep 可由协调者生成一份快照/order，Helper 通过不同起始下标轮转访问，避免 N 个 Helper 重复排序。需要使用一个 slice-owned immutable 对象，保证 Helper 结束前生命期有效。

### 9.3 方案 B：O(36) 分组而非全排序

Sweep 实际不需要 36 个 class 的完全有序关系，核心优先级是：

1. preferred class。
2. 有 unswept 工作且 ready supply 少的 class。
3. 其他有 unswept 的 class。
4. 没有 unswept 的 class 可直接跳过。

可以 O(36) 生成 urgent/normal 两组 class，组内使用 round-robin cursor，无需全排序。这一方案更快，但会改变 pressure policy 的精确顺序，建议先实施 snapshot-sort，再用数据判断是否需要分组。

### 9.4 优化前 Sweep 完成发布

旧实现的 `TryPublishSweepDone()` 先调用 `GetTotalUnsweptCount()`，读取 36 个 class 的 partial/full unswept count，共 72 个 atomic load，然后再检查：

```text
bgSweepRunning
activeLazySweepCount
activeAssistSweepCount
activeHelperSweepCount
```

该函数在 Background/Parallel/Assist/Lazy 等多个退出点调用。Mutator Lazy/Assist 可能只 Sweep 1～4 个 Region，却在尾部遍历 72 个 count。

### 9.5 已实现：全局 unswept 剩余计数

当前 `CentralRegionPool` 已在 `centralSmallActiveRegions_`/`centralSmallSweptRegions_` 旁边增加
`centralSmallUnsweptRegions_`。它统计 36 个 size class 的 `partialUnswept + fullUnswept`，
不包含 large Region。`GetTotalUnsweptCount()` 只执行一次 acquire atomic load；原来的
72 次 count 重算保留为 `GetTotalUnsweptCountReference()`。

记账规则如下：

- `PrepareForSweep()` 在完成 swept→unswept 链切换后，用同一轮计算的 `totalUnswept`
  一次性 release 发布精确总数。
- `PushPartialUnswept()`/`PushFullUnswept()` 在链表节点可见前先递增。这覆盖 P 归还
  sweep generation 不匹配 Region 的路径，不能只依赖 `PrepareForSweep()` 初始化。
- `ClaimUnsweptHeadForSweep()` 只在 Region 已从 unswept 链唯一摘除后递减；CAS 失败
  不递减。`headSweepGen == sg` 的 stale current-generation head 也已移出链表，
  因此同样递减。
- STW `ReclassifyUnsweptToSwept()` 完成全部搬迁后显式将 aggregate 校正为 0。

旧实现的完成探测快路径曾变为：

```text
if centralSmallUnsweptRegions != 0: return false
if any in-flight != 0: return false
if forceFinishSweepRunning: return false
if GetTotalUnsweptCountReference() != 0: return false
publish sweepDone
```

最后一次完整重算是 Release 中保留的 correctness fence，仅 fast aggregate 已经为 0
且所有执行方均静止的候选完成时才支付 72 次 atomic load。

`StopAndWaitForAllSweep()` 的 GC 主线程 force-finish 不属于 Background/Lazy/Assist/Helper
计数。现在使用 `forceFinishSweepRunning_` 覆盖“首次并发 drain → 关闭入口并等待
执行方 → 最终串行 drain”的完整窗口。这避免 GC 主线程摘走最后一个 Region 后，
其他线程在该 Region 仍处于 `SweepRegion()` 时提前发布 `sweepDone_`。

### 9.6 验证

- Debug 下在 slice 边界校验 `centralSmallUnsweptRegions == sum(72 counters)`。
- 检查所有 claim 入口：Background、Parallel、Assist、Lazy、ForceFinish，确保只对成功 ownership transfer 递减一次。
- 测试 `StopAndWaitForAllSweep()` 与最后一个 claim 并发时不会早发布 done。
- 对比优化前后 AssistSweep(1/4 Region) 的平均和 p99 时间。

### 9.7 已实施：Sweep 批量 claim/publish 与无锁 unswept 队列

2026-07-20 已按以下边界落地：

- Lazy 保持单 Region claim，避免在 mutator 分配路径引入额外尾延迟。
- Assist 按本次 `targetRegions` 计算 batch 上限，一次 claim 并 sweep 该额度。
- Background、Parallel Helper、GC 主线程 force-finish 统一以最多 64 个 Region 为一个 batch。
- batch 在锁外执行 `SweepRegion()`，随后按 empty/partial/full 分类；每一类只执行一次队列发布。
- empty Region 使用 `PushEmptySweptBatch()`，Assist 的 local empty cache 也使用批量入队。
- `partialUnswept` 和 `fullUnswept` 从 intrusive mutex list 改为带 32-bit tag 的 `LockFreeRegionStack`，claim 使用 `PopBatch()`，发布使用 `PushBatch()`。
- bucket mutex 只保留给 `fullSwept` 发布、cycle 边界 flip/reclassify 和 STW 诊断遍历；普通 unswept claim 不再获取该 mutex。
- `sweepAdmission_`、GC 线程最终发布 `sweepDone_`、OOM 不发布完成状态的协议保持不变。

批量 claim 后 Region 已从 unswept aggregate 中一次性扣除，但仍由统一 Sweep admission 计入 in-flight；因此 `sweepDone_` 不会在 batch 仍执行 `SweepRegion()` 时提前发布。cycle 边界的 full swept → full unswept 转移在 STW 下按 64 个 Region 分批完成，concurrent sweep 热路径不承担该锁和链表转换成本。

## 10. 优化点八：SweepRegion Bitmap 处理与 Dead Slot 清零

### 10.1 当前 `SweepRegion()` 流程

Small Region 被唯一 claim 后，`MSRegionInfo::SweepRegion()` 执行：

```text
1. 在栈上创建 markWords[64] = {0}             // 512B
2. 将 markBitmap[0..wordCount) 拷贝到 markWords
3. popcount 计算 liveCount
4. liveCount == 0:
       memset(整个 32KB Region)
   else:
       allocBitmap[i] = ~markWords[i]
       遍历每个 free bit
       对每个 dead slot 单独 memset(slotSize)
5. 发布 freeSlotCount/sweepState/summary
```

mark bitmap 在 Mark 结束后稳定，Region 又已经由 sweepgen CAS/list claim 取得唯一 ownership，直到 `SweepRegion()` 结束前不会归还 Mutator。因此 512B 临时副本不是为了防止并发修改，主要是实现上的中间存储。

对 8B/16B 等小 slot Region，如果存活率为 50%，可能执行上千次小 `memset`。函数调用、分支和小写操作的成本往往高于对连续内存做少量大 `memset`。

### 10.2 去除 markWords 副本

**实施状态：已完成。** `SweepRegion()` 现在直接两遍读取稳定的 `markBitmap`，
Pass 1 和 Pass 2 都会先对最后一个 word 的 trailing bits 做 mask。

保留两遍 bitmap 读取，但不保存副本：

```text
pass 1: 直接 popcount(markBitmap[i])
if liveCount == 0: 整 Region 清零并返回
pass 2: allocBitmap[i] = ~markBitmap[i]
```

两遍读取总量与当前基本相同，但删除了每 Region 512B 栈清零和 bitmap 副本写入。mark bitmap 的 cache line 在 pass 1 后大概率仍在 cache 中。

不建议为了只遍历一次而立即清零 dead slot：如果最终 `liveCount == 0`，整 Region 一次 memset 是最快路径，不应在之前做逐 run 清零。

### 10.3 连续 dead run 清零

**实施状态：已完成。** `runStartSlot` 会跨 bitmap word 保留，并在扫描结束时
关闭未结束的 run；所有 dead run 都在 Region 发布到 swept list 前清零。

对非全死 Region，将 allocBitmap 中连续的 free bits 合并为 slot run：

```text
runStartSlot = first free bit
runEndSlot   = first following allocated bit
memset(base + runStartSlot * slotSize,
       0,
       (runEndSlot - runStartSlot) * slotSize)
```

run 需要能跨 64-bit word 边界合并。最后一个 bitmap word 的 trailing bits 必须先按 `totalSlotCount` mask，不得将 Region 尾部之外的伪 free bit 合并进 memset。

可保留一个小阈值：单个 run 只有 1 个小 slot 时直接清零，连续 run 再进入大 memset。是否需要该分支由 microbenchmark 决定。

### 10.4 Slice 内的时钟和 yield

**实施状态：已完成。** `SweepUnsweptList()` 现在在 slice 入口由各 Sweep 执行方
检查 admission，slice 内不再逐 Region 检查 stop request；每次最多 claim 64 个 Region，
deadline 在入口及每个 batch 完成后采样。逐 Region 的无条件 `std::this_thread::yield()` 已删除。
Mutator Assist 按本次 target 批量 claim，不走 deadline/yield 路径。

### 10.5 验证

- 按 slot size 和存活率建立 Sweep microbenchmark：8B/64B/512B/4KB，0%/10%/50%/90%/100% live。
- 比较每 Region memset 调用数、写入字节、Sweep ns/Region。
- Sweep 结束后所有 free slot 必须为零，allocBitmap/freeSlotCount/reserved summary 必须一致。
- 下一次分配仍依赖“Sweep 已清零”，不能将清零延迟到 allocation fastpath。

## 11. 优化点九：Parallel Sweep 的 2ms 任务调度

### 11.1 当前实现

Parallel Sweep 默认开启，base slice budget 默认为 2ms。`BackgroundProcessor::ParallelSweepLoop()` 的顺序是：

```text
startUs = now
deadlineUs = startUs + pressure-adjusted budget
计算 swept/unswept/threshold
获取 GCThreadPool SWEEP lease
vector<unique_ptr<HeapWork>> stagedTasks
for each helper:
    new LambdaWork(capture deadlineUs)
SetMaxActiveThreadNum
AddWork
Start
background lane 也执行 Sweep
WaitFinish
Release lease
```

budget 在任务分配、入队和线程唤醒之前已经开始计时。在 2ms 窗口中，协调成本会直接挤占真正 Sweep 的时间；如果线程池唤醒较慢，Helper 进入 `ParallelSweep()` 时 deadline 可能已接近或超过。

BackgroundProcessor 一次 wake burst 最多 4 轮/12ms，因此高 backlog 下可能在一次 wake 中重复四次创建和启动任务。

### 11.2 低风险优化：工作 deadline 不包含 staging

创建一个 slice-owned context：

```text
SweepSliceContext {
    atomic<uint64_t> deadlineUs;
    immutable classOrder;
    pressureMode;
}
```

Helper task 捕获 context 指针，线程池任务全部 staged 后再发布 `deadlineUs = now + budget`并 Start。context 生命期持续到 `WaitFinish()` 结束。

这会使 2ms 变成更接近“有效 Sweep 预算”，但整个 BackgroundProcessor 轮次的 wall time 会增加 staging 成本。因此仍要使用 12ms burst 上限控制总 CPU 窗口。

### 11.3 中期优化：一个 lease 中执行多个 slice/quantum

当 Helper 已经被唤醒，不应为每 2ms 任务重新归还和获取 WorkerPool lease。可在一个 wake burst 内：

```text
Acquire SWEEP lease
Stage persistent-for-burst helper tasks
Start once
each helper:
    while burst not expired and GC not started and backlog exists:
        sweep bounded region quantum
WaitFinish once
Release lease
```

每个 Helper 的单次 quantum 可使用 Region 数而不是每 Region 读时钟，协调者使用共享 burst deadline 负责总上限。

这一设计不建议变成新的常驻 native Sweep 线程。当前复用 GCThreadPool 的主要价值就是不增加常驻线程，并且 Mark/Sweep 通过 lease 互斥使用同一组物理线程。

### 11.4 停止和所有权约束

- GC 开始时 `stopSweepRequested_` 必须使所有 Helper 在有界 Region 时间内退出。
- SWEEP lease 必须在 Mark 准备使用 WorkerPool 前归还，不能将 Mark/Sweep `HeapWork` 混入同一队列。
- context/class order 的生命期必须长于所有 Helper，不能捕获 BackgroundProcessor 栈上已退出对象的引用。
- 工作预算扩大后不能恶化 Mutator p99，LOW pressure 仍需保持较短 quantum。

### 11.5 验证

- 分别测量 stagingUs、workerWakeUs、effectiveSweepUs、WaitFinishUs。
- 记录“Helper 进入时 deadline 已过期”的轮次数，计数仅在实验版本使用本地汇总。
- 验证 Mark 与 Sweep 反复交接时 WorkerPool owner 不混用、`WaitFinish()` 不卡死。
- 对比单线程 Sweep、当前 2ms parallel slice、burst-persistent 三种方式的 regions/s 和 Mutator throughput。

## 12. 优化点十：Scavenge 锁内页释放与 macOS 重复清零

> 实施状态（2026-07-20）：D1-D4 已完成。`ReleaseUnits()` 已返回成功/失败，普通 Scavenge 和 `ReleaseCompletelyFreeSegment()` 已采用锁内 claim、锁外 release、锁内提交/回滚；D5 的 macOS `MemorySet` 删除和 D6 的零页来源优化本次未做。

### 12.1 修改前锁范围

`FreeManager::ScavengeFreeRegionsByRegionBudget()` 对每个 shard 使用 `try_lock(shardMutex_)`，成功后在持锁状态下调用 `ScavengeOwnedFreeRegionsLocked()`。该函数扫描 hot-free run，并在 `flushPending()` 内直接：

```text
ReleaseUnits(run)             // OS page operation
MarkScavengedLocked(run)
```

`ReleaseCompletelyFreeSegment()` 也在持有 owner shard mutex 时调用 `ReleaseUnits()`。与此同时，Region/span 分配的 `TryTakeRegions()` 需要获取同一把 shard mutex。因此后台 Scavenge 虽然用 `try_lock` 避免等待 Mutator，但一旦它抢到锁，后来的 Mutator 会等待整个 OS 页释放完成。

### 12.2 平台路径

| 平台 | `ReleaseUnits()` 行为 | 锁内风险 |
| --- | --- | --- |
| Windows | `VirtualFree(..., MEM_DECOMMIT)` | 内核操作可阻塞 |
| macOS | 先 `MemorySet(0)`，再 `MAP_FIXED` anonymous `mmap` | 先写完整 run，随即替换映射，内存带宽和页错误成本最高 |
| Linux/Unix | `madvise(..., MADV_DONTNEED)` | 内核页回收在锁内 |

macOS 的 anonymous `MAP_FIXED mmap` 会用新的 demand-zero 映射替换原区间，之前的整段 `MemorySet` 通常会被立即丢弃。这是一个很强的 macOS 优化候选，但需要让 `ReleaseUnits()` 返回成功/失败，不能在 mmap 失败后仍然将 Region 标记为 scavenged。

### 12.3 两阶段 Scavenge 状态机

页释放移到锁外的前提是：这批 Region 在锁外窗口中不可被分配、Shrink 或第二个 Scavenge 执行方重复处理。当前实现已为 `OwnedSegmentView` 增加 `scavengingBits_`，并使用以下状态机：

```text
阶段 A（持 shard lock）
    -> 查找 !alloc && !scavenged run
    -> 同时置 alloc 与 scavenging bits
    -> 从 allocator 可见 free candidate 中排除
    -> 生成本地 RegionSpan 列表
    -> unlock

阶段 B（不持 shard lock）
    -> ReleaseUnits(span) -> success/failure

阶段 C（再次持 shard lock）
    -> success: scavenging -> scavenged
    -> failure: clear scavenging，恢复 hot-free
    -> 刷新 chunk summary/cursor/snapshot
```

`TryTakeOneRegionFastLocked()`、free-run 查找、steal/borrow、Shrink 和 segment release 都通过 `allocBits=1` 自动将该状态视为“不可分配”。claim 同步减少 `freeRegionCount_`、增加 `scavengingRegionCount_` 并重建 summary；完成时恢复 free count，成功再增加 scavenged count。

一次只保留有界的 run 数和字节数，避免锁外挂起大量 free Region 导致 allocator 短时供应不足。

### 12.4 macOS 路径优化

在两阶段协议或单独平台实验中，可去除 mmap 之前的 `MemorySet`：

```text
mmap(addr, size,
     PROT_READ | PROT_WRITE,
     MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED,
     -1, 0)
```

成功后映射具有零页语义，`ReuseUnits()` 在 macOS 上也不需要额外 commit。验证必须包含：

- 重用后对象内存全零。
- RSS/physical footprint 确实下降，不能只是替换了虚拟映射。
- ASAN/TSAN/GWPASAN 构建下的映射语义。
- mmap 失败注入时 Region 恢复 hot-free，不被误标 scavenged。

### 12.5 验证指标

- Mutator 等待 `shardMutex_` 的 p50/p99/max，特别是 Scavenge 活跃窗口。
- 页释放的 lock-held time 应从 OS 调用整段降为两次短 metadata 更新。
- macOS 的写入字节、page fault、RSS 下降时间和 allocation throughput。
- 反复 Scavenge/reuse 不得出现同一 Region 同时 free、scavenging、allocated 中的两种状态。

## 13. 优化点十一：Background Scavenge 重复快照

### 13.1 当前重复读取

`BackgroundScavengeLoop()` 一轮中先读取 `GetHeapRetainedBytes()`，随后调用 `HasPendingScavengeWork()`，后者再次读取 retained；通过检查后又读一次 retained/threshold。同一轮中 `GetHotFreeBytes()` 也在 flush 前、计算 gap 时和 flush 后多次遍历全部 shard snapshot。

这些操作位于 gc-helper，不直接阻塞 Mutator，但会：

- 占用本可用于 Sweep/Scavenge 的后台 CPU。
- 延迟真正的 cache flush 和 OS 页释放。
- 在 shard 数较多时不必要地读取大量共享 cache line。

### 13.2 统一 `ScavengeSnapshot`

在每轮入口生成：

```text
ScavengeSnapshot {
    retainedBytes;
    thresholdBytes;
    hotFreeBytes;
    emptySweptBytes;
    largeHotCacheBytes;
    totalUnsweptRegions;
}
```

本轮的 pending 判断、target gap 和 flush 优先级都使用同一快照。只有执行了会改变快照的操作后才局部刷新：

- flush large hot cache 后更新 large/hot-free。
- flush empty swept 后更新 empty/hot-free。
- `BackgroundScavenge()` 后更新 hot-free/retained，或直接用返回的 released bytes 扣减。

`HasPendingScavengeWork()` 可接收 snapshot，或者只用在外部无快照调用者；`BackgroundScavengeLoop()` 不应在已经计算 retained 后又调用一个内部重算的 predicate。

### 13.3 与增量 retained 计数的关系

第 7 节如果实施了聚合 retained counter，`ScavengeSnapshot` 仍有价值：它保证同一决策轮次使用逻辑一致的数据，而不是在每个分支重新读 atomic 得到不同时刻的组合。

不要为了快照引入全局 mutex。这些值本来就是压力决策用的近似快照，通过 acquire/relaxed atomic 组合即可，真正的 Region ownership 仍由 FreeManager/Central 各自的锁和 CAS 保证。

### 13.4 验证

- 单轮 `GetHeapRetainedBytes/GetHotFreeBytes` 调用数应收敛为入口一次加必要刷新。
- 优化前后的 Scavenge 目标字节和释放结果应一致，不得因快照过时越过 threshold 大量释放。
- 没有 hot-free 但仍有 unswept 时，仍能正确唤醒 Sweep，不能提前关闭 Scavenge 工作位。

## 14. 优化点十二：Large Object 全局锁、元数据初始化与 Hot Cache

### 14.1 当前 Large 分配路径

`RegionSpaceMS::TryAllocateLargeObjectOnce()` 先按 `regionBytes` 查找 Central large hot cache：

```text
TryTakeLargeHotCache(regionBytes)       // largeHotCacheLock_ + std::map
    -> hit: memset(整个 span)
            RegisterLargeObjectMS()
            RegisterLargeRegion()
    -> miss: FreeManager::TakeRegionForLarge()
             memset(整个 span)
             RegisterLargeObjectMS()
             RegisterLargeRegion()
```

Hot cache 使用 `std::map<regionCount, vector<LargeHotEntry>>`，所有 P 的 Large 分配都获取同一把 `largeHotCacheLock_`。新注册的 live Large Region 又通过 `largeLock_` 进入 `largeRegionList_`。因此 Large-heavy 负载在 FreeManager shard 之上仍存在两个 Central 全局串行点。

当前路径还重新通过 `ThreadLocal::GetAllocBufferMS()` 获得 `msBuffer2` 来选 home shard，而函数参数已经传入/创建了 `msBuffer`。这是可以直接统一的重复 TLS 读取：使用已解析的 `msBuffer->GetCachedHomeShardId()`。

### 14.2 Span 元数据的两遍访问

`RegisterLargeObjectMS()` 先遍历 span 的全部 `MSRegionInfo` entry，检查每个 entry 只能是 FREE 或 LARGE；然后初始化 base entry，并再遍历一次所有 follower entry 写入 LARGE/reserved/base ownership。

可将 follower 检查和初始化合并为一遍：

```text
for each follower entry:
    CHECK current type is FREE or reusable LARGE
    initialize LARGE follower metadata
```

base entry 仍单独检查后调用 `InitForLargeObject()`。不建议在第一步将类型检查改成 Debug-only：Large span 覆盖一个已使用 Region 是严重的 allocator 破坏，合并遍历已能去掉一半 metadata pass。

### 14.3 利用零页来源避免无条件 memset

Large array 依赖未初始化内容为零，因此不能简单删除 span 清零。但 FreeManager 可以向调用者返回页来源：

```text
HOT_FREE_REUSED       // 物理页仍有旧内容，必须 memset
SCAVENGED_ZERO        // mmap/madvise/decommit 后重用，平台保证零页
FRESH_GROWN_ZERO      // 新激活虚拟堆区间，需按平台确认零页
LARGE_HOT_CACHE       // 必须 memset
```

只对明确保证 zero-fill-on-demand 的来源跳过 memset。这一优化与第 12 节的 Scavenge 成功状态紧密相关，不应在页来源不明确时单独实施。

### 14.4 Hot cache 的跨周期复用问题

`SweepLargeRegions()` 开始时首先无条件 `ReclaimLargeHotCache(max)`，把上一轮没有用掉的 hot span 全部返回 FreeManager；然后将本轮新死的 Large span 放入 hot cache。

这使 hot cache 只保留一个 GC 周期。如果同一规格的 Large 分配间隔跨越一个周期，旧 span 会在分配之前被释放，之后又从 FreeManager 重新获取并触发页错误。

建议改成 byte/age/pressure 联合策略：

- hot cache 总字节不超过 `min(fixedCap, heapThreshold * ratio)`。
- entry 带 cycle age，允许保留 1～2 个周期，超龄后清理。
- Scavenge threshold 超限时仍可优先 flush large hot cache。
- 优先清理最大、最老或长期未命中的 span bucket。

### 14.5 降低 Large 全局锁竞争

可按收益/复杂度分三步：

1. 保留 map，但将 hit/miss 统计本地化，先证明锁竞争比例。
2. 对常见小 span count 使用固定 bucket 数组，超大 span 再使用 map，降低树操作成本。
3. 如果多 P Large churn 仍争用严重，再按 span-size hash 分片锁；不建议默认给每个 P 保留大容量 Large cache，这会显著增加 retained/RSS。

live large list 的 `largeLock_` 可以通过批量注册降低获锁次数，但对单个巨大对象负载意义不大，应先用 contention profiler 判断是 hot-cache lock 还是 live-list lock 为主。

### 14.6 验证

- 按 16KB、32KB、64KB、1MB 和混合 size 测试 Large allocation throughput。
- 统计 hot-cache hit/miss/evict age，但必须使用本地或锁内顺带更新，不新增热原子。
- 对比 cache cap 前后的 RSS、page fault、Scavenge bytes 和吞吐。
- 所有 follower entry 的 `reserved=baseIdx`、location/type 必须在分配、回收和 hot-cache 重用后保持正确。

## 15. 优化点十三：16KB Small/Large 边界与 Runtime/LLVM ABI

### 15.1 当前不一致

`MSSizeClass.h` 定义 36 个 class，class 35 的 slot size 为 16384B，注释同时写明：

- class 35 是 maximum small object。
- 对象 **大于** `kLargeObjectThreshold` 才走 Large。

但 `RegionSpaceMS::Allocate()` 当前使用：

```cpp
if (allocSize >= kLargeObjectThreshold) {
    return AllocateLargeObject(...);
}
```

因此经过 Runtime 通用分配入口时，精确 16384B 对象会走 Large，class 35 不可达。该对象将：

- 至少占用一个 32KB Region/span。
- 获取 large hot-cache/global lock。
- 注册 Large span metadata/live list。
- 清零整个 32KB span，而不是使用两个 16KB slot 的 small Region。

LLVM allocation inline fastpath 如果使用 `size <= 16384` 作为 small 条件，则精确 16KB 对象会因“是否内联/是否退化 Runtime”选择不同 allocator 协议。本 Step 没有重新审计 LLVM 工作区，所以这一点必须在修改前从 `CJBarrierLowering`/分配 lowering 和生成 IR/汇编中确认，不得仅根据 Runtime 注释判断。

### 15.2 两种可选 ABI

#### 方案 A：16KB 属于 Small

```text
small: allocSize <= 16384
large: allocSize > 16384
```

优点：保留 class 35，与 size-class table/“maximum small”注释一致；两个 16KB 对象可共享一个 32KB Region，避免 Large 全局锁和 span metadata。

缺点：每个 Region 只有两个 slot，bitmap/list 管理的相对收益较低；如果现有 LLVM 明确将 16KB 当 Large，需要联动修改编译器。

#### 方案 B：16KB 属于 Large

```text
small: allocSize < 16384
large: allocSize >= 16384
```

优点：与当前 Runtime 条件一致，大 slot 不进入 small Region 链。

缺点：class 35 应被删除或明确保留为 ABI 占位，否则 size-class table 和 lookup 存在死边界；16KB-heavy 负载需承担 Large 锁和 2x 内部空间。

### 15.3 建议决策方式

不先凭注释选方案，而是按以下顺序：

1. 审计 LLVM 静态/动态 size fastpath 的精确比较条件。
2. 用 16376/16384/16392B 对象导出 IR/汇编，确认三个边界各自路径。
3. 对 16KB-only 和混合负载比较 Small/Large 的 throughput、RSS、Sweep 和 lock contention。
4. 确定唯一 ABI 后，同一提交系列修改 Runtime、LLVM、注释和边界测试。

若 LLVM 当前已稳定使用 `<=16384` small fastpath，短期更倾向方案 A：将 Runtime 改为 `>` 并使 class 35 可达；但这一建议以 LLVM 实际代码和 benchmark 结果为前提。

### 15.4 ABI 回归要求

- Runtime `SizeToClass(16384)` 、LLVM size check 和 allocator slowpath 必须返回同一类别。
- 静态常量分配、动态 size 分配和 Runtime fallback 三条路径必须一致。
- CopyGC/MSGC 切换 Runtime 后必须重编译 `.cj` 用例，不能用旧 binary 比较边界行为。
- WorkBuf/AllocBufferMS 等 LLVM 读取的结构布局不属于本边界改动，不应附带修改。

## 16. 优化点十四：Mark 结束后的 Bitmap 统计遍历

### 16.1 当前实现

`PostMarkTermination()` 在发布 small Region Sweep 之前调用 `CentralRegionPool::ComputeMarkedStats()`。它遍历：

1. 36 个 class 的 Central partial/full swept/unswept Region。
2. 所有 P `AllocBufferMS` 的 36 个 active Region。
3. large live list。

对每个 small Region 遍历 mark bitmap 并 popcount，计算 objectCount 和 trueLiveBytes。随后 Background/Lazy/Assist Sweep 会再遍历同一 bitmap 生成 allocBitmap、freeSlotCount 并清零 dead slot。

这一额外遍历并非只为日志服务：`trueLiveBytes` 用于更新下一轮 heap/scavenge threshold，`objectCount` 用于下一轮 Mark Worker/assist 策略。因此不能像普通诊断计数一样直接删除。

### 16.2 方案 A：Mark CAS 成功时本地记账

每个对象只有一个线程能成功完成 white→grey mark bit 转换。该线程已经解析 Region/size class，理论上可在成功 CAS 后记录：

```text
localMarkedObjects += 1
localLiveBytes += slotSize or largeObjectSize
```

然后在 GC Main、Mark Helper、Mutator Assist、Barrier P-local 和 root enumeration 的生命周期边界汇总。这能删除 `ComputeMarkedStats()` 的全 bitmap pass，但难点在于 white→grey 发生在多种上下文：

- GC Main/Helper 有明确 worker-local stats。
- Mutator Barrier 需要 P-local cycle stats。
- Mutator 退出时需要将本地统计转移给 collector。
- Enum root 和 finalizer/resurrection 路径需要 GC-main local stats。
- Large object 标记使用 base flag，字节数不是 slotSize。

任何遗漏都会使 heap threshold 失真。不能用一个全局 `markedBytes.fetch_add()` 替代，否则会重新引入比 bitmap pass 更严重的逐对象热原子。

### 16.3 方案 B：与 Sweep 融合

Sweep 本来就要 popcount/读取 mark bitmap，可在每个 Region Sweep 时返回 liveBytes/objectCount，最终汇总。该方案不增加 Mark 热路径记账，但存在发布时序问题：

- PostMarkTermination 需要及时更新 threshold，而并发 Sweep 可能在很久以后才全部完成。
- P active/current-generation Region 不一定参加本轮 Sweep。
- 如果等到 Sweep done 才更新 threshold，会改变 HEU/Scavenge 的时序和可能的下一轮 GC 触发点。

因此该方案不能仅作局部优化，它需要重新设计 threshold publication，优先级低于 Mark/WorkBuf/Sweep/Scavenge 的明确热点。

### 16.4 建议

1. 保留当前 bitmap 统计作为权威结果。
2. 在实验分支中同时计算 worker/P-local 结果，与 bitmap 权威值做 cycle 级对比。
3. 只有在连续多轮完全相等、Mutator exit/finalizer/large 等路径全覆盖后，才考虑删除 bitmap pass。
4. 如 profiler 表明 `ComputeMarkedStats()` 占比很低，则不实施本项，避免为减少一次顺序 bitmap 读取而扩散统计协议。

## 17. 优化点十五：OpenHarmony REPORT 日志默认开销

### 17.1 平台差异

`src/Base/LogFile.h` 中，非 OHOS 平台的 `VLOG(type, ...)` 先调用 `LogFile::LogIsEnabled(type)`，日志关闭时参数不会被格式化。

OHOS 上 `VLOG(REPORT, ...)` 却直接映射为 `LOG(RTLOG_INFO, ...)`。因此 MSGC 中的：

- parallel-sweep start/done。
- GC Mark 每阶段时间。
- HEU/Sweep/Scavenge 状态。
- `Pretty()` 数值格式化。

在 OHOS 上可能成为实际 INFO 日志 IO，而不是关闭时的近似零成本路径。对 2ms Sweep slice 而言，每 slice 两条 INFO 就可能显著改变后台工作节奏。

### 17.2 优化方案

不建议全局改变 OHOS `VLOG(REPORT)` 语义，因为其他 Runtime 模块可能依赖 REPORT→INFO。优先在 MSGC 缩减高频日志：

- GC cycle start/end、六阶段总耗时继续保留 REPORT。
- Background/Parallel Sweep 每 slice start/done 改为 Debug 或受明确的 MSGC verbose 开关控制。
- 将多个 slice 汇总成每 wake burst 一条：regions、bytes、有效工作时间、lease miss、remaining。
- 严格区分“用户需要的 GC 周期日志”和“开发者调优日志”。

### 17.3 验证

- OHOS Release 构建下分别比较 INFO/REPORT 开启和关闭的 GC/Sweep throughput。
- 确认默认日志仍能还原一次 GC cycle 的触发原因、各阶段耗时、live/threshold 和 Sweep 发布结果。
- 日志删减不得影响 `MRT_REPORT` 用于正式性能分析的核心字段。

## 18. 分批实施顺序

不建议将本文所有点放入一个大补丁。一旦吞吐回归或并发错误出现，大补丁很难判断是 Barrier、Mark、Sweep 还是 Scavenge 导致。建议按以下批次实施，每批都可独立构建、回归和回退。

### 18.1 Batch A：低风险局部改动

| 子项 | 主要文件 | 修改 | 验收 |
| --- | --- | --- | --- |
| A1 Bulk 覆盖范围 | `MSMarkBarrier.cpp` | `WriteStruct/CopyRefArray` 只扫描 copied length | dst capacity 大于 copy length 时尾部不 shade，Mark 完整 |
| A2 WorkBufPool 批量链 | `LFStack.h`、`MSAllocBuffer.h` | `PopN/PushChain`，P cache 一次 refill/drain | 节点不丢失、count 一致、锁次数下降 |
| A3 Sweep pressure 快照 | `MSCentralRegionPool.cpp/.h` | comparator 不读 atomic，Parallel 共享 order | 顺序 policy 与现有快照规则一致 |
| A4 deadline/yield 采样 | `MSCentralRegionPool.cpp` | 每 4/8 Region 读时钟，HIGH 下不强制 yield | stop 响应有界，Sweep regions/s 提升 |
| A5 ScavengeSnapshot | `BackgroundProcessor.cpp` | 单轮共享 retained/hot-free 快照 | 决策结果不变，重复调用下降 |
| A6 Large TLS 去重 | `MSRegionSpace.cpp` | 使用已解析 `msBuffer` 选 shard | home shard 不变 |
| A7 Large span 单遍初始化 | `MSRegionSpace.cpp` | 合并 follower 检查和初始化 | 保留 fatal invariant，metadata 一致 |

Batch A 中 A1、A2、A3 应各自独立提交。它们分别涉及 Barrier 语义、WorkBuf ownership 和 Sweep policy，即使都是局部改动，也不应混在一个 commit 中。

### 18.2 Batch B：Sweep 内核与计数协议

| 子项 | 主要文件 | 修改 | 关键风险 |
| --- | --- | --- | --- |
| B1 `SweepRegion()` 去副本 | `MSRegionInfo.cpp` | 直接两遍读 mark bitmap | mark bitmap 稳定窗口 |
| B2 dead run 清零 | `MSRegionInfo.cpp` | 跨 bitmap word 合并连续 free slots | trailing mask、Region 边界 |
| B3 total unswept counter | `MSCentralRegionPool.cpp/.h` | Prepare 发布，claim 递减，done 边界校验 | 多执行方不得重复递减 |
| B4 Region trigger 采样 | `MSRegionManager.cpp/.h`、`MSRegionSpace.cpp` | 每 N Region 做 retained/scavenge probe | 唤醒延迟不得无界 |
| B5 empty cache 补货 | `MSAllocBuffer.cpp/.h`、`MSCentralRegionPool.*` | low-water refill + Global reserve + fixed batch | 公平性、cache 囤积、count 语义 |

B3 需要为 Background/Lazy/Assist/Helper/ForceFinish 的每个 claim 入口写单元测试，不能只用完整 benchmark 间接覆盖。

### 18.3 Batch C：Mark WorkBuf 并发结构

Batch C 按以下顺序：

1. C1：合并 in-flight snapshot，减少 GC Main 重复锁。
2. C2：仅当 assist ratio 变化时 publish，Worker credit 降频收集。
3. C3：full/partial Global 队列分离，消除 `PopFull()` 锁内扫描。
4. C4：phase/target/in-flight atomic fastpath，mutex/CV 仅休眠/quiesce。
5. C5：GC Main 引入 work epoch 和有界等待，消除 Helper in-flight 时忙等待。
6. C6：若 mutex profiler 仍证明 Global 队列是主瓶颈，再评估 tagged lock-free stack。

C3/C4/C5 不应在同一个提交中首次出现，否则 Mark 收敛卡死时无法判断是队列 ownership、in-flight 竞争还是 wakeup 丢失。

### 18.4 Batch D：Scavenge 状态机

1. D1（已完成）：让 `ReleaseUnits()` 返回 bool/status，补充失败注入测试。
2. D2（已完成）：为 OwnedSegmentView 增加 scavenging 中间状态和不变式校验。
3. D3（已完成）：常规 Scavenge 改为锁内保留、锁外释放、锁内提交/回滚。
4. D4（已完成）：`ReleaseCompletelyFreeSegment()` 使用同一协议。
5. D5：macOS 移除 mmap 前 `MemorySet`，验证 zero/RSS/failure semantics。
6. D6：Large allocation 利用 SCAVENGED_ZERO 页来源跳过整 span memset。

D5 看似是单行删除，但不应在 `ReleaseUnits()` 仍然是 void、失败后仍标记 scavenged 的情况下作为独立“完成优化”。

### 18.5 Batch E：Parallel Sweep 持续任务

1. E1：slice context 在 task staging 后发布 deadline，共享 class order。
2. E2：将单次 wake burst 内的多轮 Start/WaitFinish 合并为一个 SWEEP lease epoch。
3. E3：Helper 用 Region quantum，协调者用 burst deadline 限制总时间。
4. E4：在 LOW/NORMAL/HIGH 压力下分别校准 quantum 和 Helper 数。

### 18.6 Batch F：Runtime/LLVM ABI 联动

1. F1：审计 LLVM 精确 16KB 分支和 size-class lookup。
2. F2：为 16376/16384/16392 生成 IR/汇编 contract test。
3. F3：比较 16KB small/large 两种协议的吞吐和 RSS。
4. F4：在同一提交系列修改 Runtime 和 LLVM，重编译全部 `.cj` 用例。

### 18.7 Batch G：只在 profiler 证明后实施

- G1：Mark 成功 CAS 的 P/Worker-local live stats，用来取代 `ComputeMarkedStats()`。
- G2：Large hot cache 分片锁/固定 bucket/age-cap。
- G3：OHOS 高频 REPORT 分级和 burst summary。
- G4：Global WorkBuf tagged lock-free stack。

这些改动的协议扩散范围较大，若在 profiler 中占比不高，应明确选择不实施。

## 19. 验证矩阵与性能指标

### 19.1 单元和 Contract Test

| 优化组 | 现有测试入口 | 应新增的覆盖 |
| --- | --- | --- |
| WorkBuf/Mark control | `MSWorkBufInvariantTest.cpp`、`MSMarkWorkerControlTest.cpp`、`MSGCTailDrainContractTest.sh` | full/partial 分队列、batch chain count、quiesce 竞争、丢 wakeup |
| Mark policy/assist | `MSMarkWorkerPolicyTest.cpp`、`MSAssistPacingTest.cpp`、`MSMarkLocalStatsTest.cpp` | credit 降频后债务不失真、尾部能收敛 |
| Barrier | `MSRegionInfoBatchMarkTest.cpp`、`MSGCSatbHookDecoupleContractTest.sh`、`MSGCDeadWriteBarrierBufferCleanupContractTest.sh` | dst>src 尾部、small-bulk 阈值、atomic same-value/race、stale slot |
| Sweep | `MSParallelSweepPolicyTest.cpp`、`MSSweepAssistPacingTest.cpp`、`MSGCPLevelSweepDebtContractTest.sh` | pressure snapshot、totalUnswept 递减、deadline sampling、done 竞争 |
| Region Sweep | `MSRegionInfoEmptyRegionFastPathTest.cpp` | 跨 word dead run、trailing bits、各存活率下的全零验证 |
| FreeManager | `MSFreeManagerFastPathTest.cpp`、`MSFreeManagerShardLocalTest.cpp`、`MSFreeManagerBatchReclaimTest.cpp`、`MSFreeManagerShrinkLockOrderTest.cpp` | scavenging 中间态、OS 失败回滚、allocation/scavenge 竞争 |
| Large/metadata | `MSFreeManagerLargeStealTest.cpp`、`MSFreeManagerSegmentMetadataTest.cpp`、`MSRegionOwnershipStateTest.cpp` | follower 单遍初始化、hot-cache age/cap、zero-page source |
| Threshold/stats | `MSMarkedStatsTest.cpp`、`MSHeapThresholdPolicyTest.cpp` | retained 采样延迟、P active 口径、local stats 与 bitmap 权威值对比 |

Contract test 只能保证某些语义/代码结构没有被意外删除，不能代替并发运行测试。对 Mark/Scavenge 状态机，必须有可控制的竞争注入。

### 19.2 必须覆盖的 benchmark 负载

| 负载 | 主要验证项 |
| --- | --- |
| SmallObj 纯分配，1P/8P/64P | Region notify 采样、empty cache refill、FreeManager shard lock |
| 高 fan-out/高入度对象图 | `DiscoverObject()` 已 marked 快路径、Mark Worker/Global WorkBuf |
| 50% live/half-alive | `SweepRegion()` dead run 清零和 Sweep regions/s |
| 大量 struct/ref-array copy | 精确覆盖范围、持久 buffer threshold/set-conflict |
| 16KB 边界和 Large churn | Runtime/LLVM ABI、hot-cache lock、span memset/page fault |
| 超 scavenge threshold 的波峰分配 | shard lock p99、两阶段页释放、RSS 回落 |
| case27 HTTP complex JSON 高并发 | Barrier、Mark tail、Mutator throughput、历史 stale-edge 稳定性 |
| case28/case3 历史稳定性用例 | invalid stateWord、false OOM、Sweep/Mark 交接 |
| 10GB/长时间运行 | hot cache age、Scavenge/Shrink、计数漂移和 RSS |

### 19.3 核心指标

Mutator 指标：

- objects/s、allocated bytes/s、端到端 requests/s。
- allocation slowpath ns/Region，Global empty single/batch/fresh 比率。
- Mark Assist/Sweep Assist 的调用数、工作量和 p99 时间。
- FreeManager shard mutex wait p50/p99/max。

GC 指标：

- Concurrent Mark wall time、GC Main/Helper/Assist processed objects 占比。
- global queue pop/push/pool lock 获取次数及等待时间（临时 profiler，不永久留热计数）。
- Mark tail duration、quiesce residual buffers、force-converged 次数。
- Sweep regions/s、ns/Region、effectiveSweepUs/slice、force-finish backlog。
- Scavenge released bytes/s、OS release time、RSS 回落延迟。

正确性指标：

- mark completeness violation 必须为 0。
- invalid stateWord/stale target/Region ownership mismatch 必须为 0。
- `totalUnswept == sum(bucket unswept)`、FreeManager free/scavenged/scavenging 集合不变式必须持续成立。
- 无 WorkBuf 丢失、重复归还或 Mark 永不收敛。

### 19.4 SDK 和运行环境

1. 每次替换 `libcangjie-runtime.dylib` 后，用当前 SDK 重新编译全部 `.cj` 用例。
2. MSGC 和 CopyGC/不同 MSGC SDK 必须在 `env -i` 隔离子 shell 中分别编译和运行。
3. 每次测试前用 `DYLD_PRINT_LIBRARIES=1` 验证实际加载的 runtime 路径。
4. 对比时明确 compiler、runtime、stdlib 三件套的来源。
5. `gc.log` 未生成、stdout 为空或 dyld 报错时，必须判定为启动环境错误，不能当成性能变慢。

### 19.5 性能验收规则

- 每个子项先做 microbenchmark 证明局部成本下降，再做完整 workload 验证端到端收益。
- 至少多轮运行并报告中位数和离散程度，不使用单次最佳数据。
- 同时比较 throughput、GC CPU、p99、RSS，不得用大幅 RSS/CPU 增长换取局部 throughput 后只报告一个指标。
- 若性能收益低于噪声，保留更简单的现有实现，不为了“理论上更快”扩展并发状态机。

## 20. 正确性不变式与回退开关

### 20.1 必须保持的不变式

| 子系统 | 不变式 |
| --- | --- |
| Barrier | 删除旧边前必须 shade/log old ref；只有 mark CAS/bitmap OR 的 white→grey 获胜者进 activeLog |
| Discover | heap/Region/slot 合法不等于对象有效；stale free slot 必须被拒绝 |
| WorkBuf | 任一 WorkBuf 同一时刻只能属于 P activeLog/logCache、PerPGCWork get/put、Global 某队列或 Pool 之一 |
| Assist | Mutator Mark Assist 仅消费 full WorkBuf，一个 full buffer 偿还的债务单位不受 partial 影响 |
| Mark quiesce | MarkTermination 开始处理尾部前，Worker/Assist in-flight 必须为 0，且所有可发布工作都可被统一 drain |
| Sweep | 每个 unswept Region 只能被一个执行方 claim；mark bitmap 在 Region Sweep 完成前不得清除 |
| Sweep done | unswept 总数为 0 且 background/lazy/assist/helper in-flight 全为 0 才能发布 done |
| Region zeroing | 任一 free slot 重新发给 Mutator 前必须为零；Large 只能在页来源保证 zero 时跳过 memset |
| Scavenge | free/scavenging/scavenged/allocated 互斥；OS 释放失败不得发布 scavenged |
| WorkerPool | MARK 和 SWEEP lease 互斥，一个 owner 的 Start/WaitFinish 周期内不得混入另一类 HeapWork |
| ABI | WorkBuf `objs` offset、AllocBufferMS LLVM 可见字段和 16KB size 分类在 Runtime/LLVM 间必须一致 |

### 20.2 回退与诊断策略

正式实施时，不建议为每个小优化永久增加环境变量，否则会迅速扩大配置矩阵。可按风险选择：

- Bulk copied-length、snapshot sort、batch pool 等局部优化：使用独立 commit 作为回退边界，不增加永久开关。
- Mark atomic control/full-partial queue：实验阶段可编译时 gate，稳定后删除旧路径和 gate，不长期保留两套并发协议。
- Scavenge 两阶段：在根因尚未验证前可保留单线程/小 quantum 诊断开关，但不能在两套状态机之间运行时随意切换。
- 16KB ABI：只能使用成对 Runtime/LLVM 构建回退，不允许只替换 Runtime dylib 验证另一个边界协议。

`CJ_MS_STW_SWEEP` 仍按项目规则保留为诊断开关，用于判断并发 Sweep 是否制造 stale edge/invalid stateWord。本 Step 不删除该开关。

## 21. 源码索引

### 21.1 Mark/WorkBuf

| 文件 | 关键符号 | 本 Step 关注点 |
| --- | --- | --- |
| `src/Common/LFStack.h` | `WorkBuf`、`LFStack`、`WorkBufPool`、`PerPGCWork` | full/partial 队列、batch get/put、ABI offset |
| `src/Heap/MCollector/MSMarkWorkerControl.h` | `WaitUntilEligible`、`TryEnterWorker`、`ExitWorker`、quiesce | mutex fastpath 原子化 |
| `src/Heap/MCollector/MarkSweepCollector.cpp` | `RunConcurrentMarkLoop`、`RunMarkWorker`、`MSMarkAssist`、`DrainOneBufRound` | GC Main 空转、Worker credit、收敛 |
| `src/Heap/MCollector/MSMarkLocalStats.h` | Worker local credit/stats | 降频发布和 cycle 汇总 |

### 21.2 Barrier/Discover

| 文件 | 关键符号 | 本 Step 关注点 |
| --- | --- | --- |
| `src/Heap/MBarrier/MSMarkBarrier.cpp` | `BarrierMarkContext`、`AddOldRefActive`、`WriteStruct`、`CopyRefArray`、atomic writes | 单次 phase gate、精确覆盖范围、TypeInfo/GCTib 复用 |
| `src/Heap/MCollector/MarkSweepCollector.cpp` | `IsTraceableObjectAddressEx`、`DiscoverObject`、`ResolveSmallRefForBarrier`、`TraceObjectRefFieldsToGCWork` | stale slot 校验、已 marked 快路径 |
| `src/Heap/MAllocator/MSRegionInfo.h/.cpp` | `MarkObjectInHeapKnown`、`TestAndSetMarkBit`、`MarkBitmapWord` | mark bit test/CAS、small/large 区分 |
| `src/CompilerCalls.cpp` | `CJ_MCC_ArrayCopyRef`、`CJ_MCC_CopyStructField` | dst/src length ABI 语义 |

### 21.3 Allocation/Central/Sweep

| 文件 | 关键符号 | 本 Step 关注点 |
| --- | --- | --- |
| `src/Heap/MAllocator/MSAllocBuffer.cpp/.h` | `GetCacheRegion`、`RefillLocalEmptyRegionsFromGlobal`、pacing debt | Per-P cache 补货和临时 vector |
| `src/Heap/MAllocator/MSCentralRegionPool.cpp/.h` | `BuildBackgroundSweepClassOrder`、`PublishSweepDoneAfterQuiescence`、batch `AssistSweep`、`ParallelSweep` | pressure 快照、total unswept、class order、batch claim/publish |
| `src/Heap/MAllocator/MSRegionInfo.cpp` | `SweepRegion` | bitmap 副本、dead run 清零 |
| `src/Heap/MCollector/BackgroundProcessor.cpp/.h` | `BackgroundSweepLoop`、`ParallelSweepLoop` | 2ms deadline、task staging、lease epoch |
| `src/Heap/MAllocator/MSRegionManager.cpp/.h` | `MaybeNotifyMark`、`MaybeNotifyScavenge`、`TakeRegions` | Region 级采样和 retained 快照 |
| `src/Heap/MAllocator/MSRegionSpace.cpp/.h` | `GetHeapRetainedBytes`、`GetSweptHeapBytes` | 统计口径和聚合 counter |

### 21.4 FreeManager/Scavenge/Large

| 文件 | 关键符号 | 本 Step 关注点 |
| --- | --- | --- |
| `src/Heap/MAllocator/MSFreeManager.cpp/.h` | `TryTakeRegions`、`ScavengeOwnedFreeRegionsLocked`、`ReleaseCompletelyFreeSegment`、`ReleaseUnits` | shard lock 范围、scavenging 中间态、OS 失败 |
| `src/Heap/MCollector/BackgroundProcessor.cpp` | `BackgroundScavengeLoop` | `ScavengeSnapshot`、重复 retained/hot-free 读取 |
| `src/Heap/MAllocator/MSRegionSpace.cpp` | `TryAllocateLargeObjectOnce`、`RegisterLargeObjectMS` | Large zero source、TLS 去重、span 单遍初始化 |
| `src/Heap/MAllocator/MSCentralRegionPool.cpp/.h` | large live list/hot cache/`SweepLargeRegions` | 全局锁、map bucket、cache age/cap |
| `src/Heap/MAllocator/MSSizeClass.h` | `kSizeClasses`、`kLargeObjectThreshold` | 16KB ABI 边界 |

### 21.5 配置与日志

| 文件 | 关键符号 | 本 Step 关注点 |
| --- | --- | --- |
| `src/CjScheduler.cpp` | `cjGCBGSweepMaxTime`、`cjMSBackgroundSweep`、`cjMSParallelSweep`、`cjMSBackgroundScavenge` | 默认开关和预算 |
| `src/Base/LogFile.h` | `VLOG` | OHOS REPORT→INFO 语义 |
| `src/Heap/MAllocator/MSCentralRegionPool.cpp` | `ComputeMarkedStats` | cycle 级 bitmap 统计 |

### 21.6 文档结论

本 Step 记录的 15 组优化点中，最先应通过数据验证的主线是：

```text
Mark Worker/Global WorkBuf 锁竞争
    + Sweep 排序/短任务的无效协调
    + Scavenge 锁内 OS 页释放
    + Bulk Barrier 过度扫描
```

这四条分别对应 Concurrent Mark、Sweep、堆页归还和 Mutator write hotpath，能覆盖 MSGC 当前最主要的 CPU/锁/内存带宽成本。但每条都必须以第 20 节的不变式为边界，优化结果也必须同时报告 throughput、GC 速度、p99、RSS 和正确性，不能只依赖单一日志计数或静态代码判断。
