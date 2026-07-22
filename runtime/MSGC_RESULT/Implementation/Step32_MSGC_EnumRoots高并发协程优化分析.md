# Step32：MSGC EnumRoots 高并发协程优化分析

> 2026-06-26
> 目标：分析 `case27_http_complex_json` 在 `c1000 / p10K` 下，`EnumRoots` 耗时上升到几十毫秒的原因，并给出适合当前 MSGC 代码结构的优化方向。
> 口径：以当前 runtime 源码和现有 benchmark 日志为准，不讨论脱离现状的大重构。

---

## 目录

1. [结论先行](#1-结论先行)
2. [问题现象](#2-问题现象)
3. [当前 EnumRoots 实现路径](#3-当前-enumroots-实现路径)
4. [瓶颈判断](#4-瓶颈判断)
5. [最值得优先做的优化](#5-最值得优先做的优化)
6. [次优先级优化项](#6-次优先级优化项)
7. [暂时不建议先做的方向](#7-暂时不建议先做的方向)
8. [建议的落地顺序](#8-建议的落地顺序)
9. [验证口径](#9-验证口径)
10. [一句话总结](#10-一句话总结)

---

## 1. 结论先行

当前 `MSGC` 在 `c1000` 下 `EnumRoots` 变慢，**主要不是因为 root 数量多**，而是因为：

1. `EnumRoots` 当前仍是一个“**mutator handshake + 每个 mutator / coroutine 各自扫栈 + GC 线程串行汇总**”的流程；
2. 对于 1000 个协程场景，真正被放大的成本是：
   - 大量协程/线程进入 `MS_MARK_SETUP` 的握手成本；
   - 每个 mutator 本地 `GcPhaseEnum()` 的栈展开与栈图遍历成本；
   - 并发模型根 `EnumConcurrencyModelRoots()` 对 `allCJThreadList` 的串行遍历成本；
3. 从日志看，`roots` 只有几百个，但 `EnumRoots` 已经到 `20ms~30ms+`，说明瓶颈在“**枚举过程本身**”，不是 root push 或 work stack 大小。

所以优化主线不应该先盯着“怎么再压 root 数量”，而应该优先做：

1. **把 parked coroutine / concurrency roots 的枚举从串行改成可并行；**
2. **把 mutator 栈枚举中的重容器和重复分配成本降下来；**
3. **把 `EnumRoots` 进一步拆 timer，先明确到底是 handshake 慢、栈展开慢、还是并发模型根遍历慢。**

---

## 2. 问题现象

这次主要参考下面两轮日志：

1. `result/20260626_000418_msgc_vs_copygc_go_c1000_p10K_compare/msgc/gc.log`
2. `result/20260626_001134_msgc_vs_copygc_go_c1000_p10K_compare/msgc/gc.log`

在第二轮中，`EnumRoots` 的典型耗时已经进入几十毫秒：

- `EnumRoots: 24,601us, roots=404`
- `EnumRoots: 28,578us, roots=453`
- `EnumRoots: 29,748us, roots=543`

而 `roots` 数量只是在 `228 ~ 543` 之间增长，并没有到“几万 root”的量级。

这说明：

> **`EnumRoots` 的 wall time 并不是被最终根集合大小主导，而是被 root 枚举路径的固定开销和 per-mutator / per-coroutine 开销主导。**

顺带一提，同一批日志里 `PreEnumRoots` 也常常很大，后半段甚至会到 `20ms~50ms`。那是另外一条问题线，主要是 `StopAndWaitForAllSweep()`。但这里先只聚焦 `EnumRoots`。

---

## 3. 当前 EnumRoots 实现路径

### 3.1 GC 主流程入口

`src/Heap/MCollector/MarkSweepCollector.cpp`

当前 `EnumRoots()` 的主体很短：

1. `TransitionToGCPhase(GC_PHASE_MS_MARK_SETUP, true)`
2. `markWorkStack_.clear()`
3. `EnumAndMarkRoots(markWorkStack_.UnsafeRaw())`

也就是：

```text
先做 mutator phase handshake
→ 再由 GC 线程做 root 汇总与初始 discover
```

### 3.2 mutator 侧实际做了什么

`GC_PHASE_MS_MARK_SETUP` 最终会让每个 mutator 执行 `Mutator::GcPhaseEnum()`。

`src/Mutator/Mutator.cpp`

当前 `GcPhaseEnum()` 的特点是：

1. 每次进入都新建：
   - `std::set<BaseObject*> rootSet`
   - `std::stack<BaseObject*> rootStack`
2. 通过 `VisitMutatorRoots(visitor)` 进入栈枚举；
3. 对于 stack-to-stack 的情况，还会递归地 `ForEachRefField()` 做局部闭包展开；
4. 最终不是直接 mark，而是把发现到的 heap root 用 `Allocator::PushRootToBuffer(obj)` 先写进每个 alloc buffer 的 root buffer。

这意味着 mutator 枚举阶段本身就包含：

- 栈展开；
- stack map 解码；
- root 去重；
- 部分对象图局部展开；
- root buffer 写入。

### 3.3 栈枚举路径

`Mutator::VisitStackRoots()` → `StackManager::VisitStackRoots()` → `GCStackInfo::FillInStackTrace()` → `GCStackInfo::VisitStackRoots()`

对应文件：

- `src/Mutator/Mutator.cpp`
- `src/StackManager.cpp`
- `src/UnwindStack/GcStackInfo.cpp`

这里的关键成本点是：

1. 每次都要先 `FillInStackTrace()` 把 frame 链完整走一遍；
2. 再逐帧解析 stack map；
3. 再访问 slot roots / reg roots。

对于 1000 协程场景，这个 per-mutator / per-cjthread 成本会被线性放大。

### 3.4 GC 线程串行汇总什么

`MarkSweepCollector::EnumAndMarkRoots()` 当前直接走：

```text
EnumAllCommonRootsSerial(workStack)
EnumAllExportRoots(workStack)
```

`EnumAllCommonRootsSerial()` 当前顺序是：

1. `MergeMutatorRoots(rootSet)`
2. `EnumStaticRoots(rootSet)`
3. `EnumConcurrencyModelRoots(rootSet)`
4. `EnumFinalizerProcessorRoots(rootSet)`
5. `EnumAllSurrectedExportRoots(rootSet)`

其中最值得注意的是第 3 项。

### 3.5 并发模型根路径

`EnumConcurrencyModelRoots()` 最终会走到：

- `CJThreadModel::VisitGCRoots()`
- `ScheduleAllCJThreadVisit()`
- `ScheduleAllCJThreadVisitImpl(...)`

这一段会拿着 `allCJThreadListLock` 串行遍历 `allCJThreadList`，并通过 `MRT_VisitorCaller()` 访问每个 `CJThread` 的 `argStart` 里的 `LWTData`。

当前 `MRT_VisitorCaller()` 实际访问的是：

1. `LWTData::obj`
2. `LWTData::threadObject`

也就是说：

> **当前 coroutine 级根枚举，是按全局 cjthread 链表串行扫一遍的。**

当协程数上来以后，这一段哪怕每个 cjthread 只看两个字段，也会成为稳定线性成本。

---

## 4. 瓶颈判断

### 4.1 不是 root 数量本身

从日志看：

- roots 只有几百；
- EnumRoots 却要二十多毫秒。

如果瓶颈在 `markWorkStack_.size()` 或 `DiscoverAndPushObject()`，通常会看到 root 数量更大，或者 root 数量与耗时更强相关。但现在更像是：

```text
协程 / mutator 数量 ↑
→ 握手次数 / 栈展开次数 / coroutine root visit 次数 ↑
→ EnumRoots 时间 ↑
```

### 4.2 最像“握手 + 栈展开 + 串行 cjthread root visit”的组合开销

当前实现里，`EnumRoots` 这段 wall time 实际包住了两类成本：

1. `TransitionToGCPhase(GC_PHASE_MS_MARK_SETUP, true)` 的 handshake 成本；
2. `EnumAndMarkRoots()` 的 root 汇总成本。

其中第一类成本在日志里还没有单独拆开，所以看起来都被算进了 `EnumRoots`。

但从代码看，第二类成本里又包含两个明显的线性路径：

1. 每个 mutator 的 `GcPhaseEnum()`；
2. `EnumConcurrencyModelRoots()` 对所有 `CJThread` 的遍历。

### 4.3 当前实现还有不少“常数项偏重”的地方

例如：

1. `GcPhaseEnum()` 每次都用 `std::set` 去重；
2. `GCStackInfo::FillInStackTrace()` 每次都重新构 frame vector；
3. `MergeMutatorRoots()` 在 GC 线程侧还要拿 `MutatorManagementWLock()` 去扫 alloc buffer；
4. `EnumAllCommonRootsSerial()` 完全串行。

这些在低并发下可能不显眼，但在 `c1000` 下就会被放大。

---

## 5. 最值得优先做的优化

### 5.1 优先级 1：改成 P 协助版的 cjThread roots 并行 visit

这是我现在最推荐的主优化方向。

这里说的不是“让 P 去帮忙扫别人的 mutator 栈”，而是：

> **让每个 P 在完成自己的 `GcPhaseEnum()` 之后，不立即结束 `MS_MARK_SETUP`，而是继续协助 visit 一部分 parked cjThread 的 scheduler roots。**

也就是把当前 `EnumRoots` 拆成两个子阶段：

1. **阶段 A：每个 mutator / P 完成自己的本地栈枚举**
2. **阶段 B：所有 P 进入一个共享的 cjThread-roots assist 阶段，并行扫全局 `allCJThreadList`**

这样做的目标是：

1. 保留当前“每个 mutator 自扫自己栈”的正确性边界；
2. 不把 parked coroutine roots 继续留给 GC 线程单线程串行做；
3. 直接把 `P` 这一批已经停下来的执行资源利用起来。

#### 5.1.1 为什么 P 协助版是合理的

当前 `EnumConcurrencyModelRoots()` 最终只是在 visit 每个 `CJThread` 上挂着的 `LWTData` root：

1. `LWTData::obj`
2. `LWTData::threadObject`

这条路径本身不依赖“当前运行栈的 UnwindContext”，也不要求“必须由 CJThread 所属 mutator 自己来扫”。

所以它和 `GcPhaseEnum()` 那种“只能自己扫自己的栈”不同，天然适合拆成共享任务。

换句话说：

- **mutator 栈 root**：必须 self-scan
- **cjThread 挂载 root**：可以由任意已停下来的 P 协助 visit

这正好给了我们一个很清晰的分工边界。

#### 5.1.2 建议的新阶段划分

建议把当前 `EnumRoots` 的逻辑从：

```text
MS_MARK_SETUP handshake
→ 每个 mutator 扫自己栈
→ GC 线程串行 EnumAndMarkRoots
```

改成：

```text
MS_MARK_SETUP handshake / phase A
→ 每个 mutator 扫自己栈，推 root 到 alloc buffer
→ mutator 上报“self enum done”

MS_MARK_SETUP assist / phase B
→ GC 线程 snapshot cjThread root entries
→ 所有 P 从共享任务队列中领取 cjThread visit 分片
→ 每个 P 把 visit 到的 root 推到本地 root buffer
→ 所有 P 上报“cjThread assist done”

GC 线程收尾
→ merge mutator roots
→ static/finalizer/export roots
→ 进入 concurrent mark
```

这里最关键的是：

> **P 协助版不是把 `GcPhaseEnum()` 并行化，而是在 `GcPhaseEnum()` 结束之后再追加一个“全局 cjThread root 协助阶段”。**

这样可以最大限度降低对现有栈枚举正确性的影响。

#### 5.1.3 具体任务模型

建议新增一个 `MSMarkSetupAssistState` 一类的共享状态，至少包含：

1. `std::vector<void*> cjThreadArgStarts`
   - GC 线程在拿 `allCJThreadListLock` 时 snapshot 下来
2. `std::atomic<size_t> nextIndex`
   - 所有 P 通过 fetch_add 领取 task chunk
3. `size_t totalCount`
4. `size_t chunkSize`
   - 例如每次处理 16 / 32 / 64 个 cjThread
5. `std::atomic<uint32_t> doneCount`
   - 统计完成 assist 的 P 数
6. `epoch / active` 标记
   - 防止复用状态时读到上一轮残留

分发逻辑建议非常简单：

```text
GC 线程：
  snapshot cjThread argStart[]
  nextIndex = 0
  publish active=true

每个已进入 safepoint 的 P：
  while true:
    begin = fetch_add(chunkSize)
    if begin >= totalCount: break
    end = min(begin + chunkSize, totalCount)
    for i in [begin, end):
      visit cjThreadArgStarts[i]
      push roots to local buffer
  doneCount++
```

这里不建议一开始就做复杂 work-stealing。

因为当前单个 cjThread visit 的工作量很轻，简单的原子递增分片通常就够了。

#### 5.1.4 P 在 assist 阶段 visit 什么

第一版建议只 visit 当前 `MRT_VisitorCaller()` 已经在做的那两个 root：

1. `LWTData::obj`
2. `LWTData::threadObject`

不要在第一版里顺手扩展语义，比如：

- 顺便扫更多 runtime side field
- 顺便做 deeper object traversal
- 顺便碰 coroutine stack / saved context

第一版目标应该非常收敛：

> **只把原本 GC 线程串行做的 cjThread root visit，等价地搬成多 P 并行做。**

这样验证起来最干净，也最容易和现有实现做一一对比。

#### 5.1.5 P 协助版与当前 `GcPhaseEnum()` 的关系

这里有一个边界一定要守住：

**不能让 P 在还没完成自己的 `GcPhaseEnum()` 时，就去协助全局 cjThread visit。**

否则会出现两个问题：

1. mutator 栈 root 还没稳定写完；
2. phase completion 的判定条件会混乱。

所以建议协议上明确分成两个完成标记：

1. `selfEnumDone`
2. `cjThreadAssistDone`

只有当：

```text
所有 mutator selfEnumDone == true
且
所有参与 assist 的 P 都 cjThreadAssistDone == true
```

GC 线程才允许继续执行 `EnumAndMarkRoots()` 的剩余串行部分或直接进入 root merge。

#### 5.1.6 根汇总方式

P 协助 visit 到的 root，建议仍然走“推到本地 alloc buffer root staging”这条旧路径，而不是直接并发 push 到 `markWorkStack_`。

原因：

1. 这样可以复用当前 `MergeMutatorRoots()` 语义；
2. 不需要在 `MS_MARK_SETUP` 阶段额外引入一个并发的 grey queue；
3. 能保持“EnumRoots 结束时，再统一 discover/mark 根”的现有结构。

也就是说，P assist 阶段只是把更多 root 放进 staging buffer，而不是直接改掉 `EnumAndMarkRoots()` 的后半段。

#### 5.1.7 需要的代码挂点

如果按当前结构落，比较合理的挂点是：

1. `Mutator::GcPhaseEnum()`
   - self stack enum 做完后，不直接返回 phase finish
   - 进入一个新的 `MS_MARK_SETUP_ASSIST` 分支
2. `MutatorManager::TransitionAllMutatorsToGCPhase()`
   - `EnsurePhaseTransition()` 的判定要支持“双阶段完成”
3. `MarkSweepCollector::EnumRoots()`
   - 在 `TransitionToGCPhase(GC_PHASE_MS_MARK_SETUP, true)` 之前或之中初始化 assist state
   - 在所有 P assist 完成后再继续 root merge
4. `CJThreadModel` / scheduler
   - 新增“snapshot allCJThreadList into array”的只读接口
   - 尽量缩短 `allCJThreadListLock` 持有时间

#### 5.1.8 正确性约束

P 协助版必须满足下面几个约束：

1. **cjThread root snapshot 必须先于 assist 开始完成**
   - assist 期间不能一边扫一边修改底层列表
2. **assist 期间不能依赖 cjThread 当前运行上下文**
   - 只读 `argStart/LWTData` 这类稳定挂载 root
3. **mutator 栈扫描与 cjThread assist 必须阶段隔离**
   - 不能在 self-enum 未完成时混做
4. **root staging 容器必须是线程私有或分片私有**
   - 避免多 P 直接争用同一 work stack
5. **GC 线程继续后必须只看到完整结果**
   - 所有 assist root 必须在 merge 前已经发布完成

#### 5.1.9 为什么我现在更倾向这个版本

相比“GC thread pool 并行版”，P 协助版有两个好处：

1. **更贴近当前问题本质**
   - 现在慢点本来就发生在 `MS_MARK_SETUP`
   - 直接把已停下来的 P 利用起来，最符合这个阶段的资源形态
2. **少一层额外 worker 调度**
   - 不需要再借助后台 GC worker 去扫 cjThread root
   - 已经停下的 mutator/P 自己就是最自然的 helper

当然，它的代价也更明显：

1. phase 协议会比“GC thread pool 版”复杂；
2. 需要把 `MS_MARK_SETUP` 从单阶段完成改成双阶段完成；
3. 第一版实现难度更高。

但如果目标就是解决 `c1000` 下 `EnumRoots` 明显被协程数放大，那么：

> **P 协助版是更激进、也更对症的方案。**

### 5.2 优先级 2：把 `EnumRoots` timer 再拆细

在真正改并行之前，建议先把 `EnumRoots` 至少拆成下面几段：

1. `markSetupHandshakeUs`
2. `mergeMutatorRootsUs`
3. `enumConcurrencyRootsUs`
4. `enumStaticRootsUs`
5. `enumFinalizerRootsUs`
6. `exportRootsUs`

原因很简单：

现在日志只有一个总的 `EnumRoots`，无法直接判断：

- 是握手慢；
- 还是 coroutine roots 慢；
- 还是 mutator roots merge 慢。

没有这层数据，后面的优化方向只能靠推断。

### 5.3 优先级 3：把 `GcPhaseEnum()` 的容器改轻

当前 `GcPhaseEnum()` 用 `std::set<BaseObject*>` 去重，这是一个明显偏重的实现。

建议方向：

1. 改成 mutator-local 可复用容器；
2. 去重结构改成 flat / open-addressing 风格的小集合；
3. 给“小 root 数量常见路径”做 small-buffer fast path。

理由：

1. `std::set` 每次插入都要节点分配/指针跳转；
2. `c1000` 下这一段会被重复做很多次；
3. 这类优化不改语义，回报比较稳定。

---

## 6. 次优先级优化项

### 6.1 给栈展开路径做缓存/复用

可考虑的点：

1. 复用 `GCStackInfo::stack` 的底层 buffer；
2. 复用 `RegSlotsMap` 之类临时结构；
3. 对 parked coroutine 的 frame 边界信息做更轻量缓存。

这一项有价值，但通常工程复杂度会比“并行化 concurrency roots”更高。

### 6.2 将 `EnumAllCommonRootsSerial()` 部分并行化

除了 concurrency roots，下面这些理论上也能并行：

1. static roots
2. finalizer roots
3. resurrected export roots

不过这些大概率不是 `c1000` 场景的主因，所以我不建议一上来先动它们。

### 6.3 缩小 `MergeMutatorRoots()` 持锁范围

当前 `MergeMutatorRoots()` 直接在 `MutatorManagementWLock()` 下调用 `MergeRootsFromAllocBuffers()`。

如果后面确认这段在大并发下也占明显时间，可以考虑：

1. 先 snapshot alloc buffer 指针；
2. 再释放管理锁；
3. 在锁外做 root merge。

但这需要额外确认生命周期正确性，不适合作为第一步。

---

## 7. 暂时不建议先做的方向

### 7.1 先压缩 root 数量

当前问题的主矛盾不是 root 数量太大，而是枚举过程太重。

所以先去做：

- root 合并规则改造；
- root set 压缩；
- markWorkStack 初始容量调优；

这些大概率不会显著改变现在的几十毫秒问题。

### 7.2 先改 `DiscoverAndPushObject()` / `MarkObject()`

`EnumRoots` 阶段 root 数只有几百，这块不是热点中心。

### 7.3 先做激进的“跳过 coroutine root 枚举”

虽然从调度语义看，有些 parked cjthread 的 root 可能具有阶段性稳定性，但这属于正确性风险很高的区域。

在没有严格证明某类 coroutine 可以安全跳过之前，不建议直接从语义上裁根。

---

## 8. 建议的落地顺序

建议按下面顺序推进：

1. **先加细粒度计时**
   - 把 `EnumRoots` 拆成 handshake / mutator merge / concurrency roots / static roots 等子项
2. **再做轻量优化**
   - `GcPhaseEnum()` 改轻量容器
   - 临时 buffer 复用
3. **然后做主优化**
   - concurrency roots snapshot + 并行 visit
4. **最后再看更深层次**
   - parked coroutine 栈扫描是否也能分离并行化
   - mutator roots merge 是否还需要进一步拆锁

这个顺序的好处是：

1. 每一步都能独立测收益；
2. 不会一上来就把 root 枚举路径改得太大；
3. 可以先用 profiling/timer 证明主因，再决定是否继续深挖。

---

## 9. 验证口径

后续如果要验证 `EnumRoots` 优化是否有效，建议至少看这几项：

1. `EnumRoots` 平均耗时 / P95 / 最大值
2. `EnumRoots` 内部各子阶段耗时
3. `c1000 / p10K` 吞吐变化
4. GC 总次数变化
5. `PreEnumRoots` 是否因为 sweep 干扰掩盖了 `EnumRoots` 优化收益

推荐至少对比下面两组：

1. `c1000 / p10K`
2. `c100 / p10K`

因为前者能放大协程数量问题，后者能帮助确认优化是不是只在极端高协程数下才有效。

---

## 10. 一句话总结

当前 `MSGC` 的 `EnumRoots` 在 `c1000` 下变慢，主因不是 root 多，而是：

> **“每个 mutator/coroutine 各自扫栈 + GC 线程串行扫并发模型根”这条枚举路径在高协程数下被线性放大了。**

最值得优先做的不是先压 root 数量，而是：

1. **把 `EnumRoots` 内部拆 timer；**
2. **把 `EnumConcurrencyModelRoots` 从串行全表扫描改成 snapshot 后并行 visit；**
3. **把 `GcPhaseEnum()` 的重容器和临时分配改轻。**
