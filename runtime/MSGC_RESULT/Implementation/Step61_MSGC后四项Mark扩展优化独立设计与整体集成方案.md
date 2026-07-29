# Step61：MSGC 后四项 Mark 扩展优化独立设计与整体集成方案

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 分别定义大数组 RangeQueue、统一任务调度与终止、WorkUnit Pacer/Assist、Old/Immutable Region 四项优化，再与 Step60 的 ScanClass、三平面 Bitmap、Region 扫描和 MarkBatch 组合成唯一最终 MSGC 架构。

**Architecture:** 大引用数组拆成可窃取的连续 RangeTask；Object、Region、Range 和 remembered-card 工作由统一 Scheduler/WorkLedger 管理；Pacer 以 Q16 WorkUnit 而非对象/WorkBuf 数计量；长期稳定 Region 晋升 OLD，minor GC 只扫描双 epoch remembered cards，major GC 仍执行完整闭包。

**Tech Stack:** C++14、CMake、Cangjie Runtime Mark-Sweep GC、Worker-local deque、Q16 定点 Pacer、AArch64/X86-64 MSGC write-barrier emitter、独立 C++/shell/LLVM lit 测试。

---

## 1. 文档范围、基线与最终约束

### 1.1 本文范围

本文只设计以下四项：

5. 大数组 `RangeQueue`；
6. 统一任务调度与终止；
7. `WorkUnit Pacer/Assist`；
8. `Old/Immutable Region`。

其中 Old Region 为了在 minor GC 中安全跳过对象，必须包含 old→young remembered
set；因此双 epoch CardTable 是优化八的必要组成，不是额外的第五项。

Step61 以 Step60 的最终结构为前置条件：

- small Region 已按 `NO_REF/HAS_REF` 分离；
- alloc/mark/scan 三平面已经落地；
- small HAS_REF 使用 Region owner；
- GC Worker 已使用 MarkWordBatch；
- Runtime/LLVM 已切到唯一 v2 ABI。

本文不重新设计 Sweep/Scavenge，也不删除 `CJ_MS_STW_SWEEP` 诊断开关。Step61
对 Range、任务调度、Pacer 和 Old/Immutable 的定义优先于 Step59 的同名章节。

### 1.2 当前源码基线

截至 2026-07-27，当前源码仍是：

| 项目 | 当前实现 |
|---|---|
| 大数组扫描 | `ForEachElementInArrayGCKnownType()` 在领取对象的单个线程中串行遍历全部元素 |
| 并发工作 | `WorkBuf` 经 `globalFull_` 的 Full/Partial lane 交换 |
| Helper/Assist | 各自维护 `PerPGCWork` 和独立 drain 循环 |
| 自然终止 | 主要检查 Global WorkBuf、GC main 输入、worker/assist in-flight |
| 提前 STW | `debt-stalled` 连续 3 个窗口或固定 time budget |
| Assist 单位 | processed object / WorkBuf，`gcAssistDebtScaled_` 仍是对象单位 |
| Assist 比率 | 上轮 marked objects 与 allocated bytes 推导 |
| Region 年龄 | 没有 generational YOUNG/OLD/IMMUTABLE 状态 |
| Remembered set | 没有 MSGC old→young CardTable |
| GC 范围 | 每轮 Mark/Sweep 都覆盖本轮纳入的完整 small/large 集合 |

因此 Step60 即使把 413.7 万个 `Array<Byte>` 的叶子发现路径变快，2 GiB case26
仍可能受三个结构性问题限制：

1. 保存数百万引用的超大外层数组仍由单线程枚举；
2. Region、Range、特殊对象和 barrier 尾工作没有同一个完成账本；
3. 长期稳定的 2 GiB payload 仍会在每个完整 GC 周期重复参与 Mark。

### 1.3 Step61 的最终目标

最终形成两类 GC：

```text
minor:
  young roots
  + sealed remembered-card ranges
  -> Step60 young Region/MarkBatch/RangeQueue
  -> 只 Sweep young

major:
  全部 roots
  -> Step60 全堆 Region/MarkBatch/RangeQueue
  -> 更新 Region age
  -> Sweep young + old
```

所有 GC main、helper 和 assist 共用：

```text
MSUnifiedMarkScheduler
  -> Object lane
  -> Region lane
  -> Range lane
  -> main-only lane

MSWorkLedger
  -> outstanding by kind
  -> active producer/worker
  -> publishSeq
```

Pacer 只使用 `WorkUnitQ16`，不再把对象数或 WorkBuf 数当作唯一工作量。最终 SDK
不保留：

- 串行扫描超大引用数组的常驻 fallback；
- helper、assist、GC main 三套不同 drain 语义；
- 仅凭队列瞬时为空或 debt 不下降决定普通终止；
- object-count 和 WorkUnit 两套 Assist debt；
- 扫描后直接清空 remembered card 的实现；
- old Region 空洞重新分配 young 对象的混代路径；
- minor/major 以运行时 feature flag 拼装的多种组合。

## 2. 优化五：大数组 RangeQueue

### 2.1 要解决的问题

case26 的 2 GiB 持久 payload 主要是数百万个 `Array<Byte>` 叶子，但这些叶子由一个
或少数长期存活的引用容器持有。假设外层数组含 413.7 万个 8B 引用，其引用 payload
约为 31.6 MiB。

当前一个 Worker 领取该数组后会串行：

```text
读取 413.7 万个元素
  -> 解析 413.7 万个目标
  -> 提交 mark bitmap
```

即使其他 Worker 已经空闲，也不能协助这个对象。其影响是：

- 单个任务 wall time 成为 Mark critical path；
- WorkBuf debt 可能很小，但真实剩余扫描字节很大；
- Helper 扩容看不到对象内部并行度；
- MarkTermination 可能接收一个尚未完成的大尾巴；
- Pacer 按“1 个对象”严重低估工作。

### 2.2 可拆分对象与不可拆分对象

唯一拆分条件：

```text
typeInfo 是 RawArray
component TypeInfo 含引用
reference payload bytes >= 128 KiB
对象不属于 WeakRef/Finalizer 专用语义
```

分类结果：

| 对象 | 处理 |
|---|---|
| `Array<Byte>` 等 NO_REF 数组 | mark winner 结束，不创建 RangeTask |
| 引用 payload < 128 KiB | 当前 Region/Object owner 内联扫描 |
| 引用 payload >= 128 KiB | 展开为连续 RangeTask |
| 含引用 struct array | 按完整 element stride 拆分 |
| 普通非数组对象 | Region owner 或 ObjectTask |
| WeakRef/Finalizer | main-only ObjectTask |
| remembered old-array card | 生成 `YOUNG_FILTER` RangeTask |

Step60 的 small/LARGE 边界约为 16 KiB，而 Range 阈值是 128 KiB。因此正常
`FULL_OBJECT` RangeTask 的父对象一定是 LARGE；它使用 LARGE base entry 的
`LARGE_MARKED` 做唯一发现，不使用 small scan plane。只有 remembered-card range
可能只覆盖 LARGE 数组的一小段。

### 2.3 RangeTask 与 ArrayScanTicket

最终结构：

```cpp
enum class MSRangeMode : uint8_t {
    FULL_OBJECT,
    YOUNG_FILTER,
};

struct MSArrayScanTicket {
    BaseObject* array;
    const TypeInfo* arrayType;
    uint64_t logicalLength;
    uint32_t regionIdx;
    uint16_t regionGeneration;
    uint16_t workEpoch;
    std::atomic<uint32_t> remainingRanges;
    std::atomic<uint8_t> state;
    MSRangeMode mode;
};

struct MSRangeTask {
    uint32_t ticketIdx;
    uint32_t beginElement;
    uint32_t endElement;
    uint32_t estimatedWorkUnitQ16;
};
```

`state` 只有：

```text
RESERVED -> PUBLISHED -> COMPLETE
```

ticket 和 task node 都来自 GC 初始化时按 committed heap 上限预留、按需 commit 的
索引池。队列中只保存索引，不保存会因 vector 扩容失效的地址。MSGC 不移动对象，
但 Worker 仍校验 `workEpoch + regionGeneration + LARGE base`，拒绝旧周期或错误
SPAN token。

### 2.4 拆分与持久发布

固定目标 payload：

```text
kMSRangeTargetBytes = 128 KiB
```

任务边界：

```cpp
elementsPerRange =
    max<uint32_t>(1, kMSRangeTargetBytes / elementStride);

[0, length) ->
  [0, elementsPerRange)
  [elementsPerRange, 2 * elementsPerRange)
  ...
```

每个边界按完整元素对齐；引用数组一个 task 最多 16,384 个 8B 引用。最后一个
task 只覆盖真实尾部，不允许读取 padding。

发布顺序：

```text
1. 计算 rangeCount，并一次 reserve 1 ticket + rangeCount task nodes
2. 初始化 ticket、全部 task node 和 remainingRanges
3. ledger.Add(RANGE, rangeCount)
4. release store ticket.state = PUBLISHED
5. 对每个 range release push task，并更新 publishSeq
6. 完成父 ObjectTask
```

如果 task pool 无法一次 reserve 全部范围，禁止发布半个 ticket。当前 Worker 直接用
同一个 MarkContext 分段内联扫描全部范围，仍按每 128 KiB flush/yield；正确性不依赖
内存分配成功。

由于 `FULL_OBJECT` 父对象是 LARGE，没有 small scan bit。其“扫描职责已持久转交”
由 `LARGE_MARKED + ticket.PUBLISHED + rangeOutstanding` 表达。未来若 Range 阈值低于
small/LARGE 边界，small 父对象只能在所有 range 成功发布后置 scan bit。

### 2.5 Range worker 与 work stealing

Worker 从本地 Range lane 尾部消费，其他 Worker 从头部 steal。每个 task：

```text
校验 ticket/workEpoch/generation
  -> 获取 array component ScanDescriptor
  -> 顺序扫描 [beginElement, endElement)
  -> child refs 进入 Step60 MarkWordBatch
  -> 每 4 KiB 检查 safepoint/yield budget
  -> FlushAll()
  -> ticket.remainingRanges--
  -> ledger.Complete(RANGE)
  -> 最后一个 range: ticket COMPLETE 并归还 ticket
```

同一 range 内复用 `RegionLookupCache` 和 `ScanDescriptorCache`。steal 只改变 task
owner，不改变元素区间；不得用多个 Worker 共享一个可变 cursor。

为避免长 task 破坏 Assist 延迟上限，Assist 在 128 KiB task 内以 16 KiB sub-slice
执行。未完成尾部修改当前 task 的 `beginElement` 后重新发布，先增加新 outstanding
再完成旧 task。

### 2.6 LARGE、small array 与 remembered-card range

LARGE 唯一发现路径：

```text
LARGE_MARKED winner
  -> NO_REF: 完成
  -> HAS_REF 非大数组: ObjectTask
  -> HAS_REF 大数组: ArrayScanTicket + FULL_OBJECT RangeTask
  -> WeakRef/Finalizer: main-only ObjectTask
```

SPAN 只解析到 base，不独立标记或创建 ticket。

minor GC 的 remembered card 使用同一 Range worker，但 mode 为 `YOUNG_FILTER`：

```text
old array card byte interval
  -> 对齐到完整 element 区间
  -> RangeTask(YOUNG_FILTER)
  -> 只把 young target 交给 MarkBatch
  -> 若仍存在 old->young edge，把 card 续写到新 card epoch
```

普通 small array 小于 128 KiB，由 Step60 Region owner 直接扫描，不进入 RangeQueue。
非数组 old card 由 Object/Region scanner 按 card 交集扫描。

### 2.7 正确性、收益、代价和指标

必须保持：

```text
所有 FULL_OBJECT ranges 的并集 == [0, arrayLength)
任意两个 ranges 不重叠
每个边界按 elementStride 对齐
ticket.remainingRanges == 未完成 range 数
ticket COMPLETE => rangeOutstanding 中不存在该 ticket
父 ObjectTask 完成前 ticket 已 PUBLISHED 或已同步扫描完成
stale generation/epoch task 不访问对象
```

直接收益：

- 单个 31.6 MiB 引用数组可由约 253 个 128 KiB task 并行处理；
- Mark critical path 从整个数组缩短为最长 range 加调度尾部；
- steal 可以利用空闲 helper；
- Pacer 可以按 range bytes 看到真实工作；
- remembered old array 与 major full array 共用扫描内核。

代价：

- 每个大数组增加 ticket 和 task node；
- task 太小会增加 ledger/steal 开销，太大则降低并行度；
- 多 Worker 同时发现相邻叶子可能增加 mark cache-line 竞争，依赖 Step60 batch 缓解；
- task pool exhaustion 需要同步 fallback。

必须记录：

```text
rangeTickets
rangeTasksPublished / Completed / Stolen / Resliced
rangeFullObjectBytes / rangeYoungFilterBytes
rangePoolReserveFailures
rangeInlineFallbackBytes
rangeWorkerCpuUs
maxRangeTaskWallUs
arrayParallelWidth
```

## 3. 优化六：统一任务调度与终止

### 3.1 要解决的问题

Step60 之后至少同时存在：

```text
RegionScanToken
RangeTask
LARGE/WeakRef/Finalizer ObjectTask
Worker-local MarkBatch
mutator barrier producer
root/finalizer producer
```

如果每种工作各自判断“队列为空”，会出现：

- task 已被 pop、正在执行，队列看起来为空；
- parent 尚未完成但准备发布 child，outstanding 瞬时为零；
- mark bit 已置位，Region dirty/token 尚未发布；
- range 已从 ObjectTask 展开，但局部 deque 尚未公开；
- mutator barrier 在终止检查过程中产生新工作；
- helper、assist、STW 各用一套 drain 规则，尾部语义不一致。

当前 `debt-stalled` 又以 WorkBuf/object backlog 判断无进展，无法看到一个正在扫描的
31.6 MiB RangeTask。因此统一调度和统一终止必须同时设计。

### 3.2 最终任务模型

逻辑任务：

```cpp
enum class MSMarkTaskKind : uint8_t {
    REGION,
    RANGE,
    OBJECT,
    MAIN_ONLY,
};

struct MSObjectTask {
    BaseObject* object;
    uint32_t regionIdx;
    uint16_t regionGeneration;
    uint16_t workEpoch;
    uint8_t specialKind;
};

struct MSMarkTask {
    MSMarkTaskKind kind;
    uint8_t flags;
    uint16_t workEpoch;
    uint32_t estimatedWorkUnitQ16;
    union {
        MSRegionScanToken region;
        MSRangeTask range;
        MSObjectTask object;
    } payload;
};
```

任务类型只决定 payload 和消费者能力，不决定另一套生命周期：

| kind | 普通 Worker | Assist | GC main |
|---|---:|---:|---:|
| REGION | 是 | 是 | 是 |
| RANGE | 是 | 是 | 是 |
| OBJECT | 是 | 是，排除 special | 是 |
| MAIN_ONLY | 否 | 否 | 是 |

WeakRef 登记、Finalizer 分类和必须串行的 Runtime root 使用 MAIN_ONLY。LARGE 普通对象
是 OBJECT；大引用数组是 RANGE；small HAS_REF 是 REGION。

### 3.3 Scheduler 队列结构

`MSUnifiedMarkScheduler` 内部保持逻辑 lane，而不是把不同任务重新塞进旧 WorkBuf：

```text
每个 GC Worker/Assist context:
  localRegionDeque
  localRangeDeque
  localObjectDeque

全局:
  injectRegion
  injectRange
  injectObject
  mainOnlyQueue
```

所有 deque/task node 预分配；本地 owner 从尾部 pop，stealer 从头部 steal。外部
mutator/root producer 只进入 global injection，不操作其他 Worker 的 deque。

选择任务使用 WorkUnit deficit round-robin：

```text
quantum:
  REGION = 4096 WU
  RANGE  = 4096 WU
  OBJECT = 1024 WU

每轮为非空 lane 增加 quantum
estimatedWU <= deficit 时领取并扣除
只有一个 lane 非空时允许领取一个超 deficit task
连续 32 个本地 task 后强制检查 global injection 和 steal
```

这比固定 `8:4:1` task 数更稳定：一个 128 KiB RangeTask 不能与一个小 ObjectTask
被当作相同成本。GC main 每轮先检查 MAIN_ONLY，再参加普通 DRR；helper/assist 永不
领取 MAIN_ONLY。

### 3.4 WorkLedger

```cpp
struct MSWorkLedger {
    std::atomic<uint64_t> publishSeq;
    std::atomic<uint64_t> outstanding[4];
    std::atomic<uint32_t> activeProducers;
    std::atomic<uint32_t> activeWorkers;
    std::atomic<uint8_t> phase;
};
```

`outstanding[kind]` 表示：

```text
queued + local deque + 已领取但尚未完成
```

因此 pop 不减 outstanding。只有 task 的扫描、MarkBatch flush 和全部 child 发布完成
后才减。

提供两个不可绕过的 RAII 对象：

```cpp
class MSProducerGuard;
class MSTaskLease;
```

- mutator barrier、root 枚举、Region owner、Range worker 在产生新工作前持有
  `MSProducerGuard`；
- scheduler 成功 pop 后返回 `MSTaskLease`；
- lease 析构前若未 `Complete()`，Debug 直接 fatal，Release 同步完成/回滚；
- MarkContext 不允许跨 lease 保存未 flush 的 mark bits。

### 3.5 发布、领取、派生和完成

唯一发布顺序：

```text
ProducerGuard active
  -> outstanding[kind].fetch_add(1, relaxed)
  -> release push task
  -> publishSeq.fetch_add(1, release)
```

任务池已经 reserve 后，queue push 不允许失败。异常/OOM fallback 在增加 outstanding
之前决定同步执行。

唯一消费顺序：

```text
acquire pop
  -> activeWorkers++
  -> 执行 task
  -> child task: 先 Add + push + publishSeq
  -> FlushAll MarkBatch
  -> task-specific completion:
       Region 释放 OWNED
       Range 递减 ticket.remaining
       Object 完成特殊状态
  -> outstanding[kind]--
  -> activeWorkers--
```

父子原则：

```text
Add(child) happens-before Complete(parent)
```

因此 total outstanding 不会在任务图仍有边时瞬时变成零。worker 把未完成任务
reslice/yield 时也必须先发布 continuation，再完成当前 lease。

### 3.6 自然终止

GC main 只有在快速检查满足以下条件时才进入完整终止确认：

```text
所有 local/global 普通 lane 为空
mainOnlyQueue 为空
outstanding[all] == 0
activeProducers == 0
activeWorkers == 0
Step60 dirtyRegionSummary == 0
不存在未发布 ArrayScanTicket
所有 MarkContext batch 为空
```

完整确认：

```text
seq0 = acquire load publishSeq
acquire fence
读取全部 queue generation/head-tail snapshot
读取 ledger、dirty summary、ticket summary
seq1 = acquire load publishSeq

seq0 == seq1 且所有状态仍为空 -> natural candidate
否则继续并发 Mark
```

natural candidate 只请求 STW，不提前关闭 Mark barrier。`ScopedStopTheWorld` 停住
mutator 后等待 `activeProducers==0`，再 flush barrier/root 尾部。这样终止检查与真正
STW 之间发生的写入仍由 barrier 捕获。

普通周期删除“连续三个 WorkBuf debt 不下降就视为完成”的语义。固定 time budget
保留为显式 emergency fallback，OOM/FullGC 可以直接请求 emergency STW，但日志必须
与 `natural` 分开，不能把 emergency 说成自然收敛。

### 3.7 MarkTermination

STW 顺序固定为：

```text
1. 停止 mutator，等待 activeProducers/activeWorkers 稳定
2. flush 全部 barrier producer、local deque 和 MarkBatch
3. 枚举最终线程栈、全局根和 runtime roots
4. 用同一个 Scheduler drain 普通 Object/Region/Range 到 ledger 全零
5. snapshot Finalizer，发布 resurrection roots
6. 再用同一个 Scheduler drain 到 ledger 全零
7. 按最终强闭包处理 WeakRef
8. 检查 dirty Region、ticket、card-scan batch、ledger 全空
9. minor/major 分别发布 Sweep 参与集合
10. 在 STW 内切换 IDLE barrier，再恢复 mutator
```

Finalizer 产生的 Region/Range/Object 工作仍经同一个 ledger；不能建立
`resurrection-only` 私有 drain。WeakRef 只在第二次强闭包后清 referent。

### 3.8 正确性、收益、代价和指标

全局不变量：

```text
queued/in-flight task => corresponding outstanding > 0
outstanding == 0 => 不存在该 kind 的 queue/local/in-flight task
active producer 内的未发布工作阻止 natural termination
child Add 先于 parent Complete
TaskLease 完成前 MarkBatch 为空
natural fixed point 需要 publishSeq 双快照稳定
```

收益：

- GC main/helper/assist 共享同一执行内核；
- Range/Region/Object 任一工作都能阻止假终止；
- WorkUnit-aware DRR 避免大数组或热点 Region 长期占满；
- STW 不再重新解释旧 WorkBuf 的“已标但未扫描”语义；
- debt-stalled 不再承担正确性终止职责。

代价：

- task envelope 比单一对象指针大；
- outstanding/publishSeq 有共享原子更新；
- per-worker 三 lane 增加固定内存；
- main-only fairness 和 emergency STW 需要单独测试。

必须记录：

```text
published/completed/outstanding[4]
activeProducersPeak / activeWorkersPeak
publishSeqRetries
localPops / globalPops / steals[3]
drrForcedFairness
taskReslices
naturalCandidates / naturalRetries
emergencyTimeBudgetEntries / oomEntries
terminationDrainTasks[4]
```

## 4. 优化七：WorkUnit Pacer/Assist

### 4.1 要解决的问题

以下任务都可能被当前统计记作“1 个对象”：

```text
NO_REF small leaf:
  只解析引用并置一个 mark bit

HAS_REF small object:
  解析 + bitmap + 扫描几十个字段

31.6 MiB 引用数组:
  解析 + 扫描 413.7 万个引用

old card:
  扫描 512B 并筛选 young target
```

按 object/WorkBuf 计债会导致：

- 数组对象工作被低估数百万倍；
- `Array<Byte>` 叶子数量又可能让简单对象被高估；
- 不同 WorkBuf 填充度造成 Assist 还债跳变；
- Region/Range/remembered-card 无法使用同一 Pacer；
- “backlog 没下降”不等于 CPU 没有完成有效工作。

### 4.2 WorkUnit 唯一口径

使用无符号 Q16：

```cpp
using MSWorkUnitQ16 = uint64_t;
static constexpr MSWorkUnitQ16 kWUOne = 1ULL << 16;
```

初始权重：

| 事件 | Q16 | 逻辑 WorkUnit |
|---|---:|---:|
| 解析一个引用候选 | 65,536 | 1.0 |
| 执行一次 mark word RMW | 524,288 | 8.0 |
| 扫描一个对象/数组 payload byte | 1,024 | 1/64 |
| dispatch/complete 一个 task | 262,144 | 4.0 |
| 扫描一个 remembered-card byte | 512 | 1/128 |

任务实际完成量：

```text
completedWU =
    resolvedRefs       * refCost
  + markWordRmw        * bitmapCost
  + scanBytes          * scanByteCost
  + taskCount          * taskCost
  + rememberedBytes    * cardByteCost
```

所有乘加使用 saturating arithmetic。统计来自 Step60 `MSMarkContext` 的本地计数，
只在 task 完成、Assist slice 结束或 Worker 退出时合并，不在每个引用上执行全局
atomic。

权重是 ABI 无关的 Pacer 参数，不影响正确性。第一版固定上述权重；只更新总
`workUnitsPerCpuUs` EWMA，不在在线热路径用一次 GC 的噪声重新拟合五个系数。权重
调整必须通过离线 profile 和独立基准进入后续版本。

### 4.3 minor/major 工作预测

Pacer 保存两套历史：

```cpp
struct MSGCWorkHistory {
    uint64_t resolvedRefsEWMA;
    uint64_t markRmwEWMA;
    uint64_t scanBytesEWMA;
    uint64_t rememberedBytesEWMA;
    uint64_t totalWorkUnitEWMA;
    double workUnitsPerCpuUsEWMA;
    uint64_t liveBytesAtSample;
    bool initialized;
};

MSGCWorkHistory minorHistory;
MSGCWorkHistory majorHistory;
```

预测：

```text
minorExpectedWU =
    scale(minorHistory, currentYoungBytes / previousYoungBytes)
  + sealedRememberedBytes * cardByteCost

majorExpectedWU =
    scale(majorHistory, currentParticipatingBytes / previousMajorBytes)
```

历史为空时使用保守静态估计：

```text
young/major participating allocated bytes / 64
+ root estimate
+ sealed remembered bytes / 128
```

EWMA 使用 `old * 7/8 + sample * 1/8`。minor 和 major 绝不共用同一历史，否则一次
major 会把后续 minor Assist ratio 拉高数倍。OOM/显式诊断周期不更新普通历史。

### 4.4 Assist ratio、debt 和 global credit

MarkSetup 计算：

```text
availableAllocBytes = max(heapGoal - markStartInUseBytes, 1)
allocRateBytesPerUs = max(recentAllocationRateEWMA, 1)
runwayUs = availableAllocBytes / allocRateBytesPerUs

backgroundCapacityWU =
    activeBackgroundWorkers
  * workUnitsPerCpuUsEWMA
  * runwayUs

requiredAssistWU =
    max(expectedWorkWU - backgroundCapacityWU, 0)

assistWorkPerByteQ16 =
    requiredAssistWU / availableAllocBytes
```

每次分配：

```text
charge = allocatedBytes * assistWorkPerByteQ16
claim = min(charge, globalAssistCreditQ16)
localDebtQ16 += charge - claim
```

后台 Worker 只有在实际进度超过本轮目标斜率时才存入 credit：

```text
creditDeposit =
  min(backgroundAheadWU, expectedWorkWU / 8)
```

credit 使用 CAS claim，不能重复抵扣；上限防止 Mark 前半程的瞬时领先让后半程完全
失去 Assist。GC 周期结束清零 credit。守恒关系：

```text
chargedWU
  = assistCompletedWU
  + creditClaimedWU
  + remainingLocalDebtWU
  + bounded rounding error
```

普通 background 完成量降低 `remainingWorkWU`；只有 ahead 部分形成 credit，避免
同一工作既减少 remaining 又无限冲销未来 debt。

### 4.5 Assist slice

当前 P debt 为正时进入：

```text
deadline = now + 200 us
budgetWU = min(localDebtWU, 4096 WU)
```

Assist：

1. `TryEnterAssist()`；
2. 从统一 Scheduler 的 REGION/RANGE/OBJECT lane 领取 task；
3. MAIN_ONLY 永不领取；
4. task 超出 wall/WU budget 时用 continuation reslice；
5. flush MarkBatch 和 local task；
6. 用本 slice 实际 `completedWU` 抵扣 local debt；
7. 到达 200µs 或 4096WU 任一上限立即返回；
8. 未还债留到下一次分配。

如果没有可领取任务：

- 不伪造 completedWU；
- 尝试 claim 已有 global credit；
- 仍有 debt 时保留，不在分配慢路径自旋等待；
- 通知 background Worker 重新评估目标宽度。

LARGE 分配同样按实际 bytes 收费，不能绕过 WorkUnit debt。

### 4.6 Worker 自动伸缩

每 1 ms 采样：

```text
requiredRateWUPerUs = remainingWorkWU / max(runwayUsRemaining, 1)
perWorkerRate = workUnitsPerCpuUsEWMA
desiredWorkers = ceil(requiredRateWUPerUs / perWorkerRate)
```

限制：

```text
1 <= totalMarkWorkers <=
  min(activeProcessors, logicalCpuCount, configuredMaxWorkers)
```

规则：

- 连续 2 个窗口 `desired > active` 才扩容；
- 连续 4 个窗口 `desired < active` 且 steal/queue pressure 低才缩容；
- 一次普通窗口最多增减 1；
- OOM/FullGC 可立即拉到上限；
- Scheduler outstanding 增加或 Range steal 压力突升时立即唤醒评估；
- worker 数量不改变 Ledger、task ownership 或 Region owner 规则。

删除按 Full WorkBuf 个数和单调 pressure level 作为唯一 helper 目标的接口。

### 4.7 正确性、收益、代价和指标

正确性边界：

```text
WorkUnit 只影响速度，不影响 task 是否必须完成
completedWU 只能来自已经完成并 flush 的工作
credit 只能 claim 一次
minor/major 历史分离
Assist 返回前无本地未发布 task/mark bit
Pacer 估计错误不能触发提前 logical termination
```

收益：

- 数组、Region、小叶子和 card 工作可以比较；
- Pacer 能提前看到大 Range 的实际剩余量；
- Assist 延迟由 200µs/4096WU 双重限制；
- helper 扩缩容依赖实测吞吐和 runway；
- 替代 `debt-stalled` 的对象/WorkBuf 假进展信号。

代价：

- task 结束要汇总更多本地计数；
- 需要 minor/major 两套历史；
- 固定权重需要后续 profile 校准；
- global credit CAS 有竞争，但只在 allocation assist 慢路径和 task 边界发生。

必须记录：

```text
expected/remaining/completed WorkUnit
completedWU by main/helper/assist
charged/paid/creditDeposited/creditClaimed/debtEnd
assistRatioQ16
assistCalls / assistNoWork / assistReslices
assistWallP50/P95/Max
minor/major history inputs
workUnitsPerCpuUs
desired/activeWorkers
scaleUp/scaleDown reasons
```

## 5. 优化八：Old/Immutable Region

### 5.1 要解决的问题

2 GiB case26 中约 413.7 万个 `Array<Byte>` 长期存活且几乎不再修改。即使 Step60
把每个叶子的发现成本降到 Region/alloc/mark 元数据，完整 GC 仍要：

```text
扫描外层引用数组
发现 413.7 万个叶子
更新对应 mark bitmap
在 Sweep 中重新保留这些 Region
```

如果请求产生的短命对象只有几百 MiB，让 2 GiB 稳定 payload 每轮参与会使 GC 成本
由长期存活集而不是新垃圾决定。

MSGC 不移动对象，适合以完整 Region 建立年龄。目标不是把 old 对象永久视为 live，
而是：

- minor 只回收 young，允许 old floating garbage 留到 major；
- old→young 引用通过 remembered set 成为 minor roots；
- major 仍完整判断 young/old/immutable 的可达性；
- 整 Region 晋升，避免 slot 级年龄和混代分配。

### 5.2 Region age 与参与集合

逻辑状态：

```cpp
enum class MSRegionAge : uint8_t {
    YOUNG,
    OLD,
    IMMUTABLE,
    SPECIAL,
};
```

- `YOUNG`：参与每次 minor/major，可继续按 Step60 规则分配；
- `OLD`：minor 不进入普通 Mark/Sweep，但 remembered cards 可能被扫描；
- `IMMUTABLE`：OLD 的子状态，且确认没有 old→young 边；minor 连 CardTable 都不访问；
- `SPECIAL`：含 WeakRef、Finalizer、untracked native/unsafe 写语义，每轮参与，不晋升。

Step60 `MSRegionInfo.control` 的保留位定义：

```text
bit 4 OLD
bit 5 IMMUTABLE
bit 6 HAS_SPECIAL
bit 7 HAS_UNTRACKED_WRITE
```

历史统计放入冷 side table，不扩大 64B RegionInfo：

```cpp
struct MSRegionAgeInfo {
    uint32_t lastMajorEpoch;
    uint16_t lastLiveRatioQ16;
    uint16_t lastOccupancyQ16;
    uint8_t consecutiveMajorSurvivals;
    uint8_t mutationEpoch;
    uint8_t flags;
    uint8_t reserved;
    uint32_t lastDirtyCardCount;
};
```

`MSRegionAgeTable[regionIdx]` 只在 major 结束、card 汇总和诊断读取。LARGE/SPAN 的年龄
归 LARGE base；SPAN lookup 必须先解析 base。

### 5.3 晋升与降级

small Region 在 major 结束时同时满足以下条件才晋升 OLD：

```text
连续 2 次 major 的 liveBytes 均大于 0
两次 liveBytes / allocatedBytesAtMarkStart >= 95%
当前 allocatedBytes / RegionSize >= 80%
不含 WeakRef/Finalizer
无 untracked native/unsafe heap-reference write
已从 AllocBuffer 归还并封存
```

LARGE object 以 base entry 为单位，连续两次 major 被标活且没有特殊语义即可 OLD，
不使用 80% Region occupancy 条件。

OLD 的规则：

- small Region 空洞永不重新分配 young；
- OLD Region 非空就保持原 sizeClass/ScanClass/age；
- major Sweep 后整个 small Region 为空才回 classless FREE；
- LARGE OLD 死亡时整 span 在 major 回收；
- minor 不改变 major survival count。

IMMUTABLE 必须已经 OLD，并在一次 major 中同时满足：

```text
Region 所有 live object 的 TypeInfo 都有 MODIFIER_IMMUTABLE
本 Region 自上次 major 后 mutationEpoch 未变化
没有 WeakRef/Finalizer/native untracked write
扫描全部 outgoing ref 后不存在 young target
```

IMMUTABLE 不是 immortal。它在 major 中仍必须由 root/引用发现并参与完整 Mark。

任意引用写以目标字段所在 Region 为准：

```text
if destination IMMUTABLE:
  atomic clear IMMUTABLE
  mutationEpoch++
  再执行 card/SATB/insertion barrier
```

必须先降级再发布新引用。反射、atomic、array copy 和 Runtime 内部引用写使用同一
barrier；无法保证经过 barrier 的 native/unsafe 对象在分配时标
`HAS_UNTRACKED_WRITE`，永不进入 IMMUTABLE。

### 5.4 双 epoch CardTable

单 CardTable 在扫描后直接清零是错误的：

```text
old.field -> youngObject
minor N 扫描 card 后清零
引用没有再次写入
minor N+1 看不到该 old->young edge
youngObject 被错误回收
```

最终采用两个 1-byte/card 平面：

```text
card size = 512 B heap
cardPlane[0], cardPlane[1]
cardSummary[0], cardSummary[1]

cardIdx = (fieldAddress - heapStart) >> 9
summaryIdx = cardIdx >> 6       // 64 cards = 32 KiB heap
summaryBit = 1 << (cardIdx & 63)
```

每个 minor MarkSetup 在 STW：

```text
scanEpoch = activeCardEpoch
activeCardEpoch ^= 1
clear 新 active plane
seal scanEpoch plane
```

恢复 mutator 后：

- 新 old→young 写只写新 active plane；
- scanner 只读 sealed scan plane；
- sealed card 扫描后如果仍存在 old→young edge，把对应 card 写入新 active plane；
- sealed plane 全部 task 完成后再清零；
- persistent edge 因此每轮自动续存；
- scan 与 mutator 不会并发清写同一个 plane。

写入发布：

```text
release store activeCard[cardIdx] = DIRTY
release OR activeSummary[summaryIdx], summaryBit
```

summary 只用于跳过全空 32 KiB chunk。CardTable 按虚拟地址预留、按 OLD Region
延迟 commit：

```text
两个 card plane 逻辑上限 = heap * 2 / 512 = 0.390625%
```

major 扫描 OLD 对象时顺便建立下一 minor 的 active card baseline。新晋升 Region
至少执行一次 outgoing edge seed；若仍有 young target，不能只因为本轮完整 Mark
已经访问过就省略 card。

### 5.5 minor GC

minor 参与集合在 MarkSetup STW 封存：

```text
participating:
  所有 YOUNG small/LARGE
  所有 SPECIAL
  sealed remembered cards

excluded:
  OLD/IMMUTABLE 普通对象本体
```

流程：

```text
1. root ref 指向 YOUNG/SPECIAL -> Step60 Discover
2. root ref 指向 OLD/IMMUTABLE -> 视为本轮已存活，不写 young mark
3. sealed card -> Object/Range task 扫描与 card 相交的引用字段
4. card target 是 YOUNG -> 作为 root 进入 MarkBatch
5. card 仍含 old->young -> 续写新 active plane
6. Region/Range/Object 统一收敛
7. WeakRef/Finalizer 按 SPECIAL 语义闭包
8. 只 Sweep participating YOUNG/SPECIAL
```

并发 minor 写屏障是混合屏障：

```text
SATB pre-barrier:
  overwritten old value 是 YOUNG -> shade old value

old->young post-barrier:
  标 active card
  若 concurrent minor 正在运行 -> 立即 shade new young value
```

只记录 card 供下一轮使用不足以保护当前轮新插入的 young target，因此 post-barrier
立即 shade 是硬性正确性要求。

扫描 512B card 时：

- small Region 根据 sizeClass/alloc plane 找出与 card 相交的已分配 slot；
- 普通对象只枚举地址落在 card 区间的字段；
- array 转换成 element-aligned `YOUNG_FILTER` RangeTask；
- LARGE/SPAN 先解析 base，再裁剪到真实对象/数组范围；
- NO_REF OLD Region 不 commit CardTable，也不产生扫描任务。

### 5.6 major GC

major 参与全部：

```text
YOUNG + OLD + IMMUTABLE + SPECIAL
```

全部复用 Step60/RangeQueue：

- NO_REF small/large 只发现，不扫描 payload；
- HAS_REF small 使用 Region owner；
- 超大引用数组使用 FULL_OBJECT RangeTask；
- WeakRef/Finalizer 使用 main-only 闭包；
- immutable 只表示“上轮没有合法写入”，不跳过 major reachability。

major MarkTermination 在 STW 中：

1. 从完整 live bitmap 更新 Region live ratio、occupancy 和 survival count；
2. 计算并封存本轮 age transition；
3. 先把全部晋升候选纳入 OLD 集合，再按最终 age 建立 active card baseline；
4. 对没有 young edge 且满足类型/写入约束的 OLD 发布 IMMUTABLE；
5. release 发布 age epoch、`lastMajorOldLiveBytes` 和 major WorkUnit history。

随后 major Sweep：

1. 按已封存的 age snapshot 回收不可达 young/old；
2. 冻结仍非空的 OLD small Region 空洞；
3. 整 Region 为空时回 classless FREE，并清理两个 card plane 对应范围；
4. 不执行新的 age transition，不修改本轮 participation snapshot。

触发 major 的任一条件：

```text
minorCyclesSinceMajor >= 8
oldAllocatedBytes >= lastMajorOldLiveBytes * (1 + gcPercent/100)
sealedRememberedCards >= totalOldCards / 8
estimatedMinorCardWU >= estimatedMajorWU / 2
young heap goal 无法在 total heap limit 内提供 minRunwayBytes
显式 FullGC 或 OOM
```

card 扫描已经接近完整 major 一半成本时提前 major，避免 remembered set 退化成另一种
全堆扫描。

### 5.7 WeakRef、Finalizer、LARGE 与 unsafe 写

固定规则：

- 含 WeakRef/Finalizer 的 small Region 设置 `HAS_SPECIAL`，年龄为 SPECIAL；
- LARGE WeakRef/Finalizer base 也是 SPECIAL；
- SPECIAL 每次 minor/major 都参与；
- Finalizer resurrection 产生的 Region/Range/Object task 进入同一 WorkLedger；
- WeakRef 仍在第二次强闭包之后处理；
- `HAS_UNTRACKED_WRITE` Region 不晋升 IMMUTABLE；
- native/unsafe 能写引用但无法 card/SATB 的对象不得晋升 OLD；
- LARGE old array 的 remembered card 由 `YOUNG_FILTER` RangeTask 扫描；
- SPAN 不持有独立年龄或 card owner，全部解析到 LARGE base。

### 5.8 正确性、收益、代价和指标

正确性不变量：

```text
minor Sweep 不处理 OLD/IMMUTABLE
OLD small Region 不接收新分配
每个 old->young edge:
  当前 minor 由 post-barrier/scan 可见
  下一 minor 存在 active card
sealed card plane 不被 mutator 写
IMMUTABLE => OLD 且无 old->young edge
SPECIAL 不晋升
major 仍验证所有 age 的可达性
```

收益：

- 2 GiB 稳定 payload 晋升后退出多数 minor；
- NO_REF OLD Region minor 不读对象、Bitmap 或 CardTable；
- 大 old 引用数组只在 dirty card 或 major 时扫描；
- minor 工作与 young allocation/dirty edge 更相关；
- WorkUnit Pacer 可分别预测 minor/major。

代价：

- old floating garbage 延迟到 major；
- OLD partial Region 空洞冻结，增加碎片；
- 双 card plane 逻辑地址上限约 0.390625% heap；
- 写屏障增加 destination/source age 判断；
- minor/major、promotion 和 card baseline 增加状态机复杂度。

必须记录：

```text
young/old/immutable/special Regions and bytes
promoted/demoted Regions
oldFrozenFreeBytes
minor/major count and reason
cardCommittedPages[2]
cardDirty/Scanned/Retained/Cleared
cardSummaryWordsVisited
rememberedScanBytes / rememberedYoungRefs
oldSkippedBytes
immutableSkippedBytes / immutableDeopts
minorFloatingGarbageEstimate
```

## 6. 四项独立收益、依赖关系与冲突闭合

### 6.1 独立职责

| 优化 | 自己解决 | 自己不解决 |
|---|---|---|
| RangeQueue | 单个超大引用数组的内部并行和长任务尾部 | 不决定全局终止，不降低稳定叶子每轮参与次数 |
| 统一调度/终止 | 所有 task 的公平执行、记账和 fixed point | 不预测应投入多少 CPU，不减少完整堆工作 |
| WorkUnit Pacer/Assist | 异构工作计量、Assist 收费和 Worker 宽度 | 不定义对象/Region 是否参与 GC |
| Old/Immutable | stable Region 退出 minor，card 保证 old→young | major 仍需完整 Mark，不加速首次两轮 major |

### 6.2 与 Step60 的依赖图

```text
Step60:
  ScanClass + alloc/mark/scan + Region owner + MarkBatch
                │
                ├──────────────┐
                v              v
        RangeQueue         Old/Immutable
        child -> batch     minor target filtering
        card -> range      card -> child batch
                │              │
                └──────┬───────┘
                       v
             Unified Scheduler/Ledger
                       │
                       v
              WorkUnit Pacer/Assist
```

RangeQueue 依赖 Step60 MarkBatch；Old/Immutable 依赖 ScanClass 和稳定 alloc plane；
Pacer 依赖 Scheduler 提供统一 completed WorkUnit；自然终止依赖所有 producer 使用
Ledger。

### 6.3 冲突闭合

| 表面冲突 | 最终闭合 |
|---|---|
| Range 发布后父对象何时算扫描完成 | 128 KiB range 父对象实际是 LARGE；用 `LARGE_MARKED + ticket + rangeOutstanding`，不伪用 small scan bit |
| task pool reserve 失败 | 发布前一次 reserve；失败则同步分段扫描，不发布半 ticket |
| task 空但 batch/producer 尚有工作 | TaskLease 完成前 flush；activeProducer 和 outstanding 阻止终止 |
| Pacer 预测错是否会漏 Mark | Pacer 只调 CPU/Assist，不参与 logical fixed point |
| card 扫描后清理与持久 edge | 双 epoch；仍指向 young 的 card 续写到新 active plane |
| minor 中新 old→young edge | post-barrier 立即 shade 新 young target，同时为下一轮记 card |
| OLD 空洞与 young 分配 | OLD 非空不重新分配，整 Region 空才回 FREE |
| IMMUTABLE 与反射/atomic 写 | 先清 IMMUTABLE，再走统一 card/SATB/insertion barrier |
| minor 跳过 old 与 Finalizer/WeakRef | SPECIAL 永不晋升，每轮参与 |
| emergency STW 与自然终止 | emergency 单独 reason；STW 后仍用同一 Ledger drain 到真实 fixed point |

### 6.4 最终实施依赖顺序

```text
1. 锁定最终 MSMarkTask、Scheduler、WorkLedger 接口
2. 把 Step60 Region/Object 工作迁到统一 Scheduler
3. 实现 ArrayScanTicket/RangeQueue
4. 建立 WorkUnit 计数，再替换 Pacer/Assist/Worker policy
5. 实现 RegionAgeTable 和双 epoch CardTable
6. 同步修改 Runtime/LLVM write barrier
7. 接入 minor/major、Finalizer/WeakRef 和 Sweep 参与集合
8. 联合 SDK 构建后一次发布
```

任务可以分阶段做组件测试，但 Task 1～7 不生成可交付 SDK。最终 Runtime 不保留旧
WorkBuf drain、object debt、单 card plane 或 non-generational 常驻分支。

## 7. Step60 + Step61 最终整体架构

### 7.1 最终元数据

最终新增结构：

```text
MSRegionInfo.control:
  Step60 QUEUED/OWNED/DIRTY/LARGE_MARKED
  + OLD/IMMUTABLE/HAS_SPECIAL/HAS_UNTRACKED_WRITE

MSRegionAgeTable:
  16B / Region cold history

MSCardTable:
  2 x 1 byte / 512B heap
  2 x summary bitmap

MSUnifiedMarkScheduler:
  per-context Region/Range/Object deques
  global injection lanes
  main-only lane

MSWorkLedger:
  publishSeq + outstanding[4]
  activeProducers + activeWorkers

MSPacerState:
  minorHistory + majorHistory
  remainingWU + assistRatio + globalCredit
```

Step60 的 64B RegionInfo 和 FastPathState 大小不再改变。Age/Card base 若编译器
inline barrier 需要，则在 v2 FastPathState 的后续 ABI 扩展区统一加入，Runtime/LLVM
contract 同步更新；不在 emitter 内引用 C++ 全局符号。

### 7.2 分配与写屏障

所有新 Region 为 YOUNG。分配仍按 `(sizeClass, ScanClass)`，Mark 中 newEpoch
alloc-black。

最终引用写：

```text
读取 oldValue
解析 destination age 和 old/new target age

destination IMMUTABLE:
  clear IMMUTABLE
  mutationEpoch++

concurrent mark SATB:
  shade oldValue（按本轮参与集合过滤）

destination OLD && newValue YOUNG:
  mark active card
  concurrent minor 时立即 shade newValue

执行引用 store
```

atomic CAS/swap、struct、static、generic、array-copy 必须保持等价语义。Static root
没有 destination Region，由最终 root 枚举和 SATB 处理。

### 7.3 minor Mark

```text
MarkSetup:
  seal young alloc plane
  flip card epoch
  snapshot young/special participating set
  seed scheduler with roots + sealed cards

Concurrent:
  root/card young ref -> Step60 Discover/MarkBatch
  HAS_REF small -> Region task
  large array/card -> Range task
  all task -> WorkLedger
  WorkUnit -> Pacer/Assist

Termination:
  两次统一 fixed point
  Weak/Finalizer
  verify young mark/scan and card carry-forward

Sweep:
  young/special only
```

### 7.4 major Mark

major 使用相同 Scheduler，没有第二套 Mark：

```text
participating filter = ALL
roots -> Step60 Discover
small HAS_REF -> Region
large arrays -> Range
special -> main-only
termination -> same Ledger
MarkTermination -> age transition + card baseline
Sweep -> ALL using sealed age snapshot
```

差别只在参与集合、Pacer history 和 major-termination age transition。

### 7.5 统一 Worker 循环

```cpp
MSRunResult RunMarkSlice(
    MSMarkContext& context,
    MSConsumerCapability capability,
    MSWorkBudget budget);
```

GC main、helper、assist 只传不同 capability/budget：

| 调用者 | MAIN_ONLY | wall budget | WU budget |
|---|---:|---:|---:|
| GC main | 是 | 无固定 slice | fixed point |
| helper | 否 | 1 ms 后重评估 | 动态 |
| assist | 否 | 200 µs | 4096 WU |
| STW main | 是 | 无 | ledger 全零 |

task 执行、child 发布、batch flush 和 lease completion 只有一份代码。

### 7.6 统一终止与 Sweep

自然终止只由 Ledger/queue/producer/dirty/ticket fixed point决定。Pacer 的
`remainingWU==0` 只是调度提示，不能替代。

普通闭包与 resurrection 闭包共用 Scheduler。结束后：

```text
minor:
  assert young mark/scan complete
  assert sealed card tasks complete
  publish young/special Sweep

major:
  assert all participating mark/scan complete
  publish age/card baseline
  publish all Sweep
```

### 7.7 case26 中四项优化的实际作用

```text
第一次/晋升前 major:
  RangeQueue 并行扫描约 31.6 MiB 外层引用
  Step60 快速发现 413.7 万个 NO_REF leaves
  Ledger 避免 Range 尾部落入 STW
  WorkUnit 正确估计数百万引用

晋升后 minor:
  2 GiB Array<Byte> Regions 为 OLD NO_REF，直接跳过
  未变化且无 old->young 的 immutable Region 连 CardTable 都不读
  只有请求新建 young、dirty cards 和 roots 参与

周期性 major:
  再次用 RangeQueue + Step60 完成全堆验证
  回收 old floating garbage并更新年龄
```

因此四项分别压缩：单对象 critical path、调度尾部、Assist 失配和重复 full-live
工作；不能把所有提升归因于某一个开关。

## 8. 并发状态机与内存序

### 8.1 Task/Ledger 状态

`WorkLedger` 不是统计模块，而是 Mark fixed point 的一部分。所有能产生后继工作的
执行体都必须先注册 producer：

```text
Producer:
  INACTIVE -> ACTIVE -> INACTIVE

Task:
  RESERVED -> PUBLISHED -> CLAIMED -> COMPLETED
```

发布一个 task 的顺序固定为：

```text
1. outstanding[kind].fetch_add(1, relaxed)
2. queue.push(task, release)
3. publishSeq.fetch_add(1, release)
```

领取方用 acquire pop；`CLAIMED` 不减少 outstanding。执行者必须先发布全部子 task、
flush 私有 MarkBatch，再：

```text
outstanding[kind].fetch_sub(1, release)
```

`MSProducerGuard` 构造时对 `activeProducers` 执行 acq_rel 增加，析构前完成所有发布，
再 release 减少。一个 task 派生子 task 时，顺序必须是：

```text
Add(child) -> Publish(child) -> Flush(child batch) -> Complete(parent)
```

终止观察者使用 acquire 读取 `publishSeq/outstanding/activeProducers/activeWorkers`。
自然终止必须在两次相同的 `publishSeq` 快照之间同时看到所有队列为空和 Ledger 为零；
不能用一次 relaxed load 判空。

### 8.2 ArrayScanTicket 状态

Ticket 状态只允许：

```text
FREE -> RESERVED -> PUBLISHED -> COMPLETE -> FREE
```

生产者先初始化 ticket 的 array、TypeInfo、Region generation、range 总数和
`remainingRanges`，再为所有 range 预留 task 槽。只有全部预留成功后才：

```text
ledger.Add(RANGE, count)
ticket.state.store(PUBLISHED, release)
release-push all ranges
```

`PUBLISHED` 表示全部 ticket/task 内容已经初始化且对应 outstanding 已登记；它必须
先于任何 task 入队，避免 worker 先 pop 到 RESERVED ticket。range worker acquire pop
后读取 ticket。每个 range 在发布后继引用并
flush MarkBatch 后：

```text
if (remainingRanges.fetch_sub(1, acq_rel) == 1) {
    ticket.state.store(COMPLETE, release);
}
ledger.Complete(RANGE);
```

最后一个 range 只负责 ticket 完成；ticket 槽回收到 epoch-local free list，必须等
Ledger 已观察不到该 task，避免 ABA。`FULL_OBJECT` 的父对象是 LARGE，完成条件由
ticket 管理，不借用 small Region 的 scan plane。若将来允许 small array 进入
RangeQueue，必须在 `PUBLISHED` 后才设置 scan 位，不能反过来。

### 8.3 Card epoch 状态

Card plane 的生命周期是：

```text
CLEAR -> ACTIVE -> SEALED -> DRAINED -> CLEAR
```

minor MarkSetup 在 STW 中：

1. acquire 读取当前 `activeCardEpoch`，把它记为 `scanEpoch`；
2. 清理下一 active plane 和 summary；
3. release 发布新的 `activeCardEpoch`；
4. 把旧 plane 标为 SEALED，并按 summary 生成 remembered task；
5. 这些 task 全部进入 Ledger 后才恢复 mutator。

mutator 的写屏障 acquire 读取 `activeCardEpoch`，先 release 写 card，再 release OR
summary。scanner acquire 读取 sealed summary/card；若扫描后仍有 old→young edge，
将 card release 写入当前 active plane。

只有 `sealedCardOutstanding == 0` 且没有 card producer 后，sealed plane 才能进入
DRAINED/CLEAR。Region 回收、LARGE span 复用或 Region generation 改变时，必须同时
清理两个 plane 对应范围；card task 需携带 Region generation，过期 task只能安全
丢弃，不能扫描新对象。

### 8.4 Region age 状态

合法转换为：

```text
YOUNG --major termination--> OLD --major termination--> IMMUTABLE
IMMUTABLE --any tracked write--> OLD
YOUNG/OLD/IMMUTABLE --special or untracked capability--> SPECIAL
OLD/IMMUTABLE --whole Region reclaimed--> FREE/YOUNG
```

晋升只在 major 的 MarkTermination STW 内发布：此时完整 live bitmap、Mark 开始时的
allocation snapshot 和 Region 属性均稳定。先建立 outgoing young-edge card baseline，
再用 release CAS 设置 OLD；只有确认 baseline 为空且类型/写入条件成立，才发布
IMMUTABLE。

IMMUTABLE 写入路径用 acq_rel CAS 清除 IMMUTABLE，并递增 `mutationEpoch`，然后执行
普通写屏障，最后才提交用户引用。读屏障和 scanner acquire 读取 age 位。并发 Sweep
不得改变 age；它只按 MarkTermination 已封存的 age/participation snapshot 回收。

### 8.5 Pacer 状态

Pacer 的预测数据和正确性状态严格分离：

- `totalWU/completedWU/assistCreditWU` 使用饱和无符号原子；
- worker 每个 slice 在本地累积，再一次 relaxed `fetch_add`，避免每个引用更新全局；
- `assistDebtWU` 归 mutator 本地所有，只在 safepoint/assist 入口汇总；
- `targetWorkers` 每 1 ms 最多更新一次，用 release 发布，worker acquire 读取；
- minor/major EWMA 只在正常 GC 完成后的单线程 PostGC 更新；
- 溢出时钳位到 `UINT64_MAX` 并强制保守 assist，不能回绕成零；
- Pacer 任何读到旧值的结果只允许“多做/少做一小段”，不得改变 task 的发布、完成和
  终止条件。

### 8.6 全局不变量

实现和评审必须逐条维持：

```text
published task => outstanding > 0
parent complete => all children already published or synchronously completed
private MarkBatch non-empty => owner task/producer still active
natural termination => every queue empty && Ledger zero && publishSeq stable
SEALED card plane is immutable to mutators
old->young edge => current minor可见 && next minor active card存在
IMMUTABLE => OLD && no special/untracked write && no old->young edge
minor Sweep target ∩ OLD/IMMUTABLE = empty
major participating set = all allocated Regions
Pacer decision cannot make logical work disappear
```

Debug 构建在 MarkTermination 对这些不变量做全堆抽样；压力测试构建提供全量检查。

## 9. 按最终依赖顺序实施

下面是实现顺序，不是可长期保留的中间架构。每个 Task 都先写测试，再实现；Task 6
通过前，不能发布只包含部分结构的 Runtime SDK。

### Task 1：统一 Task、Scheduler 与 WorkLedger

**Files**

- Create: `src/Heap/MCollector/MSMarkTask.h`
- Create: `src/Heap/MCollector/MSWorkLedger.h`
- Create: `src/Heap/MCollector/MSUnifiedMarkScheduler.h`
- Create: `src/Heap/MCollector/MSUnifiedMarkScheduler.cpp`
- Modify: `src/Heap/MCollector/MarkSweepCollector.h`
- Modify: `src/Heap/MCollector/MarkSweepCollector.cpp`
- Modify: `src/Heap/MCollector/MSMarkWorkerControl.h`
- Modify: `src/Heap/MCollector/MSMarkTerminationPolicy.h`
- Modify: `src/Heap/MCollector/CMakeLists.txt`
- Test: `test/MSUnifiedMarkSchedulerTest.cpp`
- Test: `test/MSWorkLedgerTerminationTest.cpp`
- Test: `test/MSGCUnifiedTerminationContractTest.sh`

**实现**

1. 定义四种 task kind、capability mask 和固定大小 `MSMarkTask`；
2. 实现 per-context deque、global injection、steal 和 WU deficit round-robin；
3. 实现 `MSProducerGuard/MSTaskLease`，禁止直接 push queue；
4. 把 root、write-barrier log、Region owner、普通 object 和 main-only closure 全部迁移；
5. 把 helper、assist、GC main 改成调用同一 `RunMarkSlice()`；
6. 用 Ledger 双快照替换 `DEBT_STALLED` 三窗口逻辑；
7. 保留 `TIME_BUDGET/OOM/FULL_GC` 为显式 emergency reason，不伪装成自然终止。

**先失败的测试**

```text
producer 在 queue empty 快照后发布 task，不能误终止
claimed parent 派生 child，outstanding 不能出现瞬时 0
private batch 未 flush，不能进入 MarkTermination
MAIN_ONLY 不能被 helper/assist 执行
Region/Range/Object 持续互相派生时不能饿死
```

**验证**

```bash
cmake --build CMakebuild --target MSUnifiedMarkSchedulerTest MSWorkLedgerTerminationTest -j8
ctest --test-dir CMakebuild -R 'MS(UnifiedMarkScheduler|WorkLedgerTermination)' --output-on-failure
bash test/MSGCUnifiedTerminationContractTest.sh
```

### Task 2：RangeQueue 与 ArrayScanTicket

**Files**

- Create: `src/Heap/MCollector/MSRangeTaskPool.h`
- Create: `src/Heap/MCollector/MSRangeTaskPool.cpp`
- Modify: `src/Heap/MCollector/MSMarkTask.h`
- Modify: `src/Heap/MCollector/MSUnifiedMarkScheduler.h`
- Modify: `src/Heap/MCollector/MarkSweepCollector.h`
- Modify: `src/Heap/MCollector/MarkSweepCollector.cpp`
- Modify: `src/Heap/Barrier/GCRefField.inline.h`
- Modify: `src/Heap/MAllocator/MSRegionInfo.h`
- Modify: `src/Heap/MCollector/CMakeLists.txt`
- Test: `test/MSRangeTaskPoolTest.cpp`
- Test: `test/MSGCArrayRangeCoverageContractTest.sh`
- Test: `test/MSGCRangeQueueFallbackContractTest.sh`

**实现**

1. 实现 epoch-local ticket/task pool 和 reserve-all API；
2. 在已知 TypeInfo 的 array 扫描入口判断 `payload >= 128 KiB`；
3. 生成 element-aligned `[begin,end)`，默认每片约 128 KiB；
4. `FULL_OBJECT` 与 `YOUNG_FILTER` 共用 task 格式，但使用不同 target filter；
5. range 执行复用 Step60 `MarkContext/MarkBatch`；
6. reserve 失败走同步、分段、无发布的 fallback；
7. 把 ticket 和 range outstanding 接入 Task 1 Ledger。

**先失败的测试**

```text
长度 0/1、非整除边界、128 KiB 前后覆盖恰好一次
32 MiB 引用数组多 worker 扫描且无漏项/重复 task
pool 用尽只走完整 fallback，不留下半发布 ticket
Region generation 变化后旧 range 不扫描新对象
NO_REF []byte 永不进入 RangeQueue
```

**验证**

```bash
cmake --build CMakebuild --target MSRangeTaskPoolTest -j8
ctest --test-dir CMakebuild -R MSRangeTaskPool --output-on-failure
bash test/MSGCArrayRangeCoverageContractTest.sh
bash test/MSGCRangeQueueFallbackContractTest.sh
```

### Task 3：WorkUnit、Pacer、Assist 与 Worker 伸缩

**Files**

- Create: `src/Heap/MCollector/MSWorkUnit.h`
- Create: `src/Heap/MCollector/MSWorkPacer.h`
- Modify: `src/Heap/MCollector/MSMarkLocalStats.h`
- Modify: `src/Heap/MCollector/MSMarkWorkerPolicy.h`
- Modify: `src/Heap/MCollector/MSMarkWorkerControl.h`
- Modify: `src/Heap/MCollector/MSAssistPacing.h`
- Modify: `src/Heap/MCollector/MSWorkBufScalePolicy.h`
- Modify: `src/Heap/MCollector/MarkSweepCollector.h`
- Modify: `src/Heap/MCollector/MarkSweepCollector.cpp`
- Modify: `src/Heap/MAllocator/MSAllocBuffer.h`
- Modify: `src/Heap/MAllocator/MSAllocBuffer.cpp`
- Modify: `src/Heap/MAllocator/MSRegionSpace.cpp`
- Test: `test/MSWorkUnitTest.cpp`
- Test: `test/MSWorkPacerTest.cpp`
- Test: `test/MSGCWorkUnitAssistContractTest.sh`

**实现**

1. 落地 Q16 固定权重、饱和加法和 slice-local counter；
2. 分开保存 minor/major history；
3. 用 predicted WU、allocation rate、runway 和 background capacity 计算 assist ratio；
4. 将 `gcAssistDebtScaled_` 从“对象数债务”迁移为 WU 债务；
5. assist 改为 `200 us || 4096 WU` slice，并从统一 Scheduler 领任务；
6. 每 1 ms 更新 `targetWorkers`，实现 2 次扩容/4 次缩容迟滞；
7. 旧参数只作为一个版本的兼容读取，稳定后删除 object-count pacing 分支。

**先失败的测试**

```text
相同对象数、不同引用密度得到不同 WU
饱和计算不回绕
background credit 不能超过 predicted/8
200 us assist 被大 RangeTask 内部分片约束
Pacer 预测为 0 时仍由 Ledger 完成所有逻辑任务
worker 目标在抖动负载下不每窗口反转
```

**验证**

```bash
cmake --build CMakebuild --target MSWorkUnitTest MSWorkPacerTest -j8
ctest --test-dir CMakebuild -R 'MS(WorkUnit|WorkPacer)' --output-on-failure
bash test/MSGCWorkUnitAssistContractTest.sh
```

### Task 4：Old/Immutable、双 epoch CardTable 与 minor/major

**Files**

- Create: `src/Heap/MAllocator/MSRegionAgeTable.h`
- Create: `src/Heap/MAllocator/MSRegionAgeTable.cpp`
- Create: `src/Heap/MAllocator/MSCardTable.h`
- Create: `src/Heap/MAllocator/MSCardTable.cpp`
- Modify: `src/Heap/MAllocator/MSRegionInfo.h`
- Modify: `src/Heap/MAllocator/MSRegionInfo.cpp`
- Modify: `src/Heap/MAllocator/MSRegionManager.h`
- Modify: `src/Heap/MAllocator/MSRegionManager.cpp`
- Modify: `src/Heap/MAllocator/MSRegionSpace.h`
- Modify: `src/Heap/MAllocator/MSRegionSpace.cpp`
- Modify: `src/Heap/MAllocator/MSCentralRegionPool.h`
- Modify: `src/Heap/MAllocator/MSCentralRegionPool.cpp`
- Modify: `src/Heap/MBarrier/MSMarkBarrier.h`
- Modify: `src/Heap/MBarrier/MSMarkBarrier.cpp`
- Modify: `src/Heap/MBarrier/MSRefField.inline.h`
- Modify: `src/Heap/MCollector/MarkSweepCollector.h`
- Modify: `src/Heap/MCollector/MarkSweepCollector.cpp`
- Modify: `src/Heap/MSGCFastPath.h`
- Modify: `src/Heap/MAllocator/CMakeLists.txt`
- Test: `test/MSRegionAgeTableTest.cpp`
- Test: `test/MSCardTableEpochTest.cpp`
- Test: `test/MSGCOldRegionMinorContractTest.sh`
- Test: `test/MSGCOldYoungBarrierContractTest.sh`

**实现**

1. 在 Step60 control 保留位中实现 YOUNG/OLD/IMMUTABLE/SPECIAL；
2. 建立不扩大 64B `MSRegionInfo` 的 cold age side table；
3. 实现两个 512B/card plane、64-card summary 和延迟 commit；
4. 实现 minor STW epoch flip、sealed card task 和 persistent-card 续写；
5. 实现 concurrent minor 的 SATB pre-barrier 与 old→young insertion barrier；
6. 将 card array segment 转成 Task 2 的 `YOUNG_FILTER` RangeTask；
7. 在 major termination 建 baseline，再发布晋升；冻结 OLD small holes；
8. minor 仅 Sweep YOUNG/SPECIAL，major 扫描并 Sweep 全部 age；
9. 加入 major 触发条件和 minor/major 独立 history。

**先失败的测试**

```text
无后续写入的 persistent old->young edge 连续跨 3 个 minor 仍存活
minor 标记进行中新增 old->young edge，本轮与下一轮都可见
sealed plane 不接收 mutator 写；active plane 不被 scanner 清除
OLD partial Region 不再分配 young
IMMUTABLE 写前降级，写后目标可达
WeakRef/Finalizer/untracked Region 永不晋升
major 能回收 OLD floating garbage
LARGE/SPAN card owner 始终解析到 base
```

**验证**

```bash
cmake --build CMakebuild --target MSRegionAgeTableTest MSCardTableEpochTest -j8
ctest --test-dir CMakebuild -R 'MS(RegionAgeTable|CardTableEpoch)' --output-on-failure
bash test/MSGCOldRegionMinorContractTest.sh
bash test/MSGCOldYoungBarrierContractTest.sh
```

### Task 5：LLVM/Runtime 屏障 ABI 一次性切换

Old/Immutable 不是只改 Runtime 的优化。编译器 fast path 必须知道 destination Region age、
active card plane 和当前 minor phase；因此按 Step60 v2 ABI 做一次性、同步切换。

**Runtime Files**

- Modify: `src/Heap/MSGCFastPath.h`
- Modify: `src/Heap/MAllocator/MSAllocBuffer.h`
- Modify: `src/Heap/MAllocator/MSAllocBuffer.cpp`
- Modify: `src/Heap/MBarrier/MSRefField.inline.h`
- Modify: `src/Heap/MBarrier/MSMarkBarrier.cpp`
- Modify: `test/MSGCFastPathStateABIContractTest.cpp`
- Modify: `test/MSGCFastPathStateABIContractTest.sh`

**LLVM Files**

- Modify: `llvm/include/llvm/CodeGen/CJMSGCABI.h`
- Modify: `llvm/lib/CodeGen/CJBarrierLowering.cpp`
- Modify: `llvm/lib/Target/AArch64/AArch64MSGCEmitter.cpp`
- Modify: `llvm/lib/Target/AArch64/AArch64CangjieCallSize.h`
- Modify: `llvm/lib/Target/X86/X86MSGCEmitter.cpp`
- Modify: `llvm/lib/Target/X86/X86MCInstLower.cpp`
- Test: `llvm/test/CodeGen/AArch64/CangjieGC/msgc-writebarrier-old-young.ll`
- Test: `llvm/test/CodeGen/X86/cangjie-msgc-writebarrier-old-young.ll`

**最终 fast path 顺序**

```text
load oldValue
resolve destination Region age
if IMMUTABLE -> slow path deopt
if mark phase and oldValue is participating young -> SATB log
if destination old and newValue young:
  dirty active card
  if concurrent minor -> shade newValue
store newValue
```

Runtime v2 state append card/age 元数据基址和 card epoch；旧字段 offset 不重排。AArch64、
X86 和 Runtime 用相同常量并各自 `static_assert`。fast path 不支持的 special、SPAN、
untracked 情况统一进入 slow path，不能静默跳过。

**验证**

```bash
bash test/MSGCFastPathStateABIContractTest.sh
cmake --build CMakebuild --target MSGCFastPathStateABIContractTest -j8
ctest --test-dir CMakebuild -R MSGCFastPathStateABIContract --output-on-failure
cd /Users/lipeisheng/code/cangjie/cangjie/cangjie_compiler/build/build/third_party/cjnative-prefix/src/cjnative-build
bin/llvm-lit -sv \
  llvm/test/CodeGen/AArch64/CangjieGC/msgc-writebarrier-old-young.ll \
  llvm/test/CodeGen/X86/cangjie-msgc-writebarrier-old-young.ll
```

### Task 6：全量集成、构建与 case26 回归

**实现**

1. 删除旧 `WorkBuf`/`PerPGCWork` 对象数调度和正常 `DEBT_STALLED` 终止路径；
2. 删除单 card、只记下一轮而不保护当前轮的任何临时路径；
3. 确认 Step60 64B RegionInfo、三平面 Bitmap 和 Region owner 不被重复实现；
4. 打开全部 invariant、task/card/WU/age 指标；
5. 完整构建 Runtime、替换 MSGC SDK dylib、重签名；
6. 用替换后的 SDK 重新编译 case26 `.cj`，不复用旧 binary；
7. 在完全隔离的 SDK 环境验证 runtime dylib 来源；
8. 分别运行 128 MiB 和 2 GiB persistent-set case。

**Runtime 构建**

```bash
cd /Users/lipeisheng/code/cangjie/cangjie/cangjie_runtime_MSGC/runtime
python3 build.py build --gc-mode marksweep
```

**SDK 三件套**

```text
compiler = /Users/lipeisheng/code/cangjie/cangjie/cangjie_compiler/output/msgc
stdlib   = /Users/lipeisheng/code/cangjie/cangjie/cangjie_compiler/output/msgc
runtime  = 本 Task 新构建并替换、codesign 后的 libcangjie-runtime.dylib
```

替换 runtime 后必须按项目规则重新编译用例，并在 `env -i` 子 shell 中做
`DYLD_PRINT_LIBRARIES=1` 探针。CopyGC 基线只使用固定
`/Users/lipeisheng/code/cangjie/cangjie-pkg/dev/20260507/cangjie`，不替换其 runtime。

## 10. 测试、观测指标与验收

### 10.1 正确性测试矩阵

| 维度 | 必测组合 |
|---|---|
| GC 类型 | 首次 major、连续 minor、周期 major、FullGC、OOM emergency |
| 对象 | small NO_REF/HAS_REF、LARGE、SPAN、引用数组、`Array<Byte>` |
| age | YOUNG、OLD、IMMUTABLE、SPECIAL、写后 immutable deopt |
| 边 | young→young、young→old、old→young、old→old、persistent old→young |
| 并发 | 1/2/8/全部 worker，assist on/off，mutator 持续 array copy/atomic write |
| task pool | 正常、ticket 耗尽、range task 耗尽、global injection 饱和 |
| card | epoch flip 前写、flip 后写、扫描同时写、Region 回收复用 |
| 终止 | producer 延迟发布、child 派生、private batch、finalizer resurrection |

每个组合至少检查：

```text
无漏标、无错误回收、无重复 Sweep
Ledger/queue/ticket/card outstanding 全归零
minor 不 Sweep OLD/IMMUTABLE
major 后 full-heap verifier 与 mark bitmap 一致
ASAN/TSAN 构建无越界、UAF 和数据竞争
```

### 10.2 必须新增的观测指标

RangeQueue：

```text
rangeTickets/rangeTasks/rangeBytes/rangeRefs
rangeSteals/rangeFallbacks/rangePoolHighWater
rangeMaxSliceUs/rangeParallelWidth/rangeContinuationCount
```

Scheduler/Ledger：

```text
taskPublished/Claimed/Completed[kind]
taskSteals[kind]/laneStarvationUs[kind]
outstandingHighWater[kind]/producerHighWater
terminationProbeCount/terminationProbeRetry
terminationResidualWU/emergencyReason
```

Pacer：

```text
predicted/actual/completed WU
refsSeen/markRMW/scanBytes/rememberedBytes
WUPerCpuUs/assistRatio/assistDebt/assistCredit
assistSliceUs/assistSliceWU/targetWorkers/activeWorkers
runwayBytes/runwayUs/allocationRate
```

Old/Immutable：

```text
young/old/immutable/special Regions and bytes
promoted/demoted Regions/oldFrozenFreeBytes
minor/major count and trigger
cardDirty/Scanned/Retained/Cleared
cardCommittedPages/cardSummaryWordsVisited
oldSkippedBytes/immutableSkippedBytes/immutableDeopts
rememberedScanWU/majorWU/minorFloatingGarbageEstimate
```

所有速率同时输出 wall 和 CPU 口径。minor 与 major 必须分栏，不能把“minor 跳过
old 后对象数变少”误报成单位对象速率下降。

### 10.3 case26 基线

验收固定使用：

```text
128 MiB:
MSGC_RESULT/Result/case26_http_complex_json/result/
20260727_095319_output_msgc_vs_copygc_go_java_c1000_p100K_compare

2 GiB:
MSGC_RESULT/Result/case26_http_complex_json/result/
20260727_095905_output_msgc_vs_copygc_go_java_c1000_p100K_compare
```

现有基线：

| 指标 | 128 MiB | 2 GiB |
|---|---:|---:|
| 端到端有效对象速率 | 68.201 Mobj/s | 16.033 Mobj/s |
| natural 有效对象速率 | 76.062 Mobj/s | 23.464 Mobj/s |
| Mark wall P95 | 56.437 ms | 3676.664 ms |
| `debt-stalled` 周期 | 16.59% | 73.68% |
| MarkTermination / Mark wall | 23.88% | 41.59% |

实现后的数据必须按 warm-up、promotion 前 major、稳定 minor、周期 major 分组。不能用
稳定 minor 跳过 old 的低工作量掩盖 major 退化。

### 10.4 最终验收门槛

正确性为硬门槛；任一 full-heap verifier、card continuity 或 Ledger invariant 失败均
直接拒绝。性能门槛：

1. 32 MiB 以上引用数组的 range 执行并行宽度达到
   `min(4, availableWorkers)`，且 `rangeFallbacks == 0`（故障注入除外）；
2. stable set 达到晋升条件后，普通 minor 的
   `oldSkippedBytes / eligibleStableBytes >= 95%`；
3. 2 GiB major 的 natural 有效对象速率不低于 `40 Mobj/s`；
4. 2 GiB 稳态 minor 的 Mark P95 不高于 `200 ms`，所有 Mark 混合 P95 不高于
   `750 ms`；
5. 正常周期不再产生 `DEBT_STALLED`；`TIME_BUDGET/OOM/FULL_GC` emergency 周期占比
   不高于 `5%`；
6. 2 GiB MarkTermination 占 Mark wall 不高于 `10%`，且 normal termination
   residual WU 为 0；
7. `actualWU / predictedWU` 的 P50 在 `[0.8, 1.25]`，P95 在 `[0.5, 2.0]`；
8. remembered scan WU 连续达到预计 major WU 的 `1/2` 时，下一次必须触发 major；
9. 128 MiB 的请求吞吐、P99 latency 和 RSS 相对原基线回退均不超过 `5%`；
10. 双 card plane 的 committed RSS 按实际 OLD 覆盖增长，空 Region 不得预提交；
11. AArch64/X86 ABI lit、Runtime ABI contract、全部 MSGC 单测和压力测试通过；
12. case26 结果必须保存 compiler/runtime/stdlib 来源、dylib probe、编译/运行退出码和
    原始 GC 日志。

其中第 3 项验证 Step60 + RangeQueue 的 major 单位工作效率；第 4 项主要验证
Old/Immutable 对稳态延迟的收益，二者不可互相替代。

## 11. 文件影响清单

### 11.1 Runtime 新增文件

```text
src/Heap/MCollector/MSMarkTask.h
src/Heap/MCollector/MSWorkLedger.h
src/Heap/MCollector/MSUnifiedMarkScheduler.h/.cpp
src/Heap/MCollector/MSRangeTaskPool.h/.cpp
src/Heap/MCollector/MSWorkUnit.h
src/Heap/MCollector/MSWorkPacer.h
src/Heap/MAllocator/MSRegionAgeTable.h/.cpp
src/Heap/MAllocator/MSCardTable.h/.cpp
```

### 11.2 Runtime 主要修改文件

```text
src/Heap/MCollector/MarkSweepCollector.h/.cpp
src/Heap/MCollector/MSMarkWorkerControl.h
src/Heap/MCollector/MSMarkWorkerPolicy.h
src/Heap/MCollector/MSMarkLocalStats.h
src/Heap/MCollector/MSAssistPacing.h
src/Heap/MCollector/MSMarkTerminationPolicy.h
src/Heap/MAllocator/MSRegionInfo.h/.cpp
src/Heap/MAllocator/MSRegionManager.h/.cpp
src/Heap/MAllocator/MSRegionSpace.h/.cpp
src/Heap/MAllocator/MSCentralRegionPool.h/.cpp
src/Heap/MAllocator/MSAllocBuffer.h/.cpp
src/Heap/MBarrier/MSMarkBarrier.h/.cpp
src/Heap/MBarrier/MSRefField.inline.h
src/Heap/Barrier/GCRefField.inline.h
src/Heap/MSGCFastPath.h
src/Heap/MCollector/CMakeLists.txt
src/Heap/MAllocator/CMakeLists.txt
```

### 11.3 LLVM 修改文件

```text
llvm/include/llvm/CodeGen/CJMSGCABI.h
llvm/lib/CodeGen/CJBarrierLowering.cpp
llvm/lib/Target/AArch64/AArch64MSGCEmitter.cpp
llvm/lib/Target/AArch64/AArch64CangjieCallSize.h
llvm/lib/Target/X86/X86MSGCEmitter.cpp
llvm/lib/Target/X86/X86MCInstLower.cpp
```

### 11.4 测试与结果

新增的 Runtime 单元/contract test 和 LLVM lit 文件以 Task 1～5 清单为准。case26 新结果
必须新建时间戳目录，保留原始 128 MiB/2 GiB 基线，不覆盖
`20260727_095319` 和 `20260727_095905`。

本 Step 只新增本文档，没有修改 Runtime、LLVM 或测试源码。后续实现必须按 Task 1～6
整体落地，最终只保留本文定义的 Scheduler/Ledger、Range ticket、WorkUnit pacer 和
双 epoch Old/Immutable 架构。
