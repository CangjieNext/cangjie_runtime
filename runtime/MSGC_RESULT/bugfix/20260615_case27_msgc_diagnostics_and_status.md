# case27 MSGC 问题、修复状态与验证排查手段记录

日期：2026-06-15

## 背景

case27 是 HTTP + complex JSON 压力场景，当前主要用于验证 MSGC 在高并发、小对象密集、JSON 对象图复杂、网络协程调度频繁的组合负载下是否稳定。

本轮排查中主要现象包括：

- 早期出现 `shard[4]` / `shard[5]` 的 seg 数为 0。
- 运行末尾曾出现看似 heap 还有空间但 regular allocation OOM。
- 开启 case27 c8/p100K 后出现 SIGSEGV / SIGABRT / UE。
- `CJ_MS_VERIFY_MARK=1` 下出现 JSON 相关 missed-mark、stale edge。
- 后续 JSON missed-mark 压下去后，仍有偶发 JSON parse failure。
- 最近一次 dump 证明失败 JSON 字节本身合法，问题从“JSON 字节被写坏”收敛到“parser/root/live 状态偶发被 GC 影响”。

## 遇到过的问题

### 1. shrink 后某些 shard 没有 seg

现象：

- 日志中出现类似 `shard[5]: 0 seg`。
- P 设置为 6，但 shrink 后部分 shard 没有本地 segment。

原因判断：

- shrink 回收尾部 segment 时，只按全局 committed segment 数判断，没有保证每个 shard/P 至少保留一个 segment。
- 当尾部 segment 属于某个 shard 且该 shard 没有其他 segment 时，shrink 后该 shard 可能变成 0 seg。

处理：

- 修改 shrink 逻辑：回收 tail segment 前检查 owner shard，保证每个 shard 至少保留 1 个 segment。

状态：

- 已修复。

### 2. finalizer 路径 MarkNewObject 子图漏扫

现象：

- finalizer/resurrection 相关路径中新对象被标记后，其子对象没有稳定进入本轮扫描队列。
- 可能导致对象本身黑了，但子图没有在本轮 mark 中继续展开。

原因判断：

- `MarkNewObject` 中只 `MarkObject(obj)` 不够。
- 对 finalizer/resurrection 路径，需要把对象推入 deferred main thread 队列，保证其子图被扫描。

处理：

- 在 `MarkNewObject` 中保留：

```cpp
MarkObject(obj);
PushSingleToDeferredMainThread(obj);
```

注意：

- 不应把 `MarkNewObject` 补到普通 allocation fast/common path。
- 当前结论是：普通分配路径的新对象本轮不会被 sweep，不需要用 `MarkNewObject` 遮盖问题。

状态：

- finalizer 路径已修复。
- 通用分配路径不加 `MarkNewObject`。

### 3. MSMarkBarrier new-edge 漏入队

现象：

- `CJ_MS_VERIFY_MARK=1` 下出现 JSON 对象图 missed-mark。
- JSON / HashMapEntry / ArrayList 等对象链上有 target 没被标记。

原因判断：

- 写屏障或 copy barrier 部分路径只处理 old ref，未覆盖新写入的 new ref。
- struct copy、static struct copy、ref array copy、struct array copy 等批量写入路径，需要把目标范围内的新引用也入队。

处理：

- 在 `MSMarkBarrier` 中补 new-edge 入队。
- 对 struct/ref array/static struct/struct array copy 路径，扫描目标写入范围内的引用并 `EnqueueRefFast`。
- `CopyStructArray` 使用 `MArray::ForEachRefFieldInRange` 这类范围扫描，避免按错误步长或布局遗漏引用字段。

状态：

- 已修复，JSON 类 missed-mark 明显收敛。

### 4. mutator 退出时 WBB 丢失

现象：

- 并发 mark 期间，mutator 退出后其 write barrier buffer 可能仍有未 flush 的灰对象。
- verifier 中出现 missed-mark，且问题和线程/协程生命周期相关。

原因判断：

- mutator exit / destroy / runtime destroy 时，如果处于 MS mark phase，WBB 中 pending mark work 不能直接随 mutator 消失。

处理：

- 在 mutator 退出相关路径补 `MaybeFlushPendingMSMarkWorkOnMutatorExit`。
- 当 phase 属于 MS mark setup / concurrent mark / mark termination 时，把退出 mutator 的 WBB flush 到 collector work buffer。

状态：

- 已修复。

### 5. empty-cache / region 状态异常

现象：

- 崩溃前出现 `EmptyCacheDiag reason=pop-without-in-cache-mark`。
- 后续出现 `TestAndSetMarkBit: slotIdx >= totalSlots`。
- SIGSEGV 现场地址落在 region 最后一个合法 slot 之后的边界地址，说明非法对象地址进入了 mark/drain 路径。

原因判断：

- empty-cache push/pop 配对或 region 状态转换存在异常。
- region 被当成 empty cache 取出时，可能已经不是 `MS_REGION_EMPTY_CACHE`，或者 cache 标记与实际链表状态不一致。

处理：

- 增加 `EmptyCacheDiag`，记录每个 region 的 last entry/exit op、线程、region type、size class、sweep gen。
- 在 push/pop、local cache、global cache、background sweep、force finish sweep、reclaim requeue 等路径补诊断记录。
- 对取出的 empty cache region 增加 region type 校验。
- region 重新用于小对象前清空 region payload，降低复用旧内容导致的误判和悬挂对象风险。

状态：

- 已加诊断与防护。
- 最近验证未再复现同类 SIGSEGV，但还不能宣称完全根治，需要默认 `CJ_MS_BG_SCAVENGE` 下多轮确认。
- 2026-07-07 新补充：
  - `GetFreshRegion` 新近复现过 `expected=FREE actual=CENTRAL_FULL` 的 region-state 断言；
  - 代码核对发现 `MSRegionManager::InitializeFreshRegions()` 之前只重置
    `regionType/next/prev`，没有同步重置 `MSRegionInfoArray` 的 location/owner side table；
  - 当 grow 复用曾经 shrink 过的旧虚拟 region 槽位时，这会留下 `CENTRAL_*` 残影；
  - 已修正为 grow 时对每个 fresh region 执行完整 `Reset()`。
- 2026-07-08 新补充：
  - 当前 `case27` 这次 `expected=FREE actual=CENTRAL_FULL` 更可疑的点在
    `FreeManager::TryStealSingleRegionBatchLocked()` 的 borrowed fastpath；
  - 2026-06-13 的优化把原先受 `receiverShard.shardMutex_` 保护的
    `borrowedSpans_` 队列，改成了裸 `borrowedIdx_/borrowedNum_` 二元组；
  - 消费侧 `TryTakeBorrowedRegionLocked()` 仍在 receiver lock 下读写这两个字段，
    但生产侧 publish borrowed batch 时已经不再拿 receiver lock，形成明确数据竞争；
  - borrowed 路径本身不会再走 bitmap free 校验，一旦读到“新的 `borrowedNum_` +
    旧的 `borrowedIdx_`”这类拼接状态，就可能把一个早已回到 `CENTRAL_FULL`
    或已被别处使用的 region index 当 fresh region 发出去；
  - 因此后续修复优先级应放在 borrowed publish/consume 协议，而不是只盯
    empty-cache 或 grow/reset 路径。
  - 当前树已先按保守方案修复：恢复 `borrowedSpans_` 队列，并把 publish/consume
    都收回 `receiverShard.shardMutex_` 保护下，避免 torn pair 和多批借用互相覆盖。

### 6. 合法 JSON 偶发 parse failure

现象：

- 最新 c8/p100K 验证中 MSGC 仍出现少量 JSON parse failure。
- dump 出来的 server body/client response 用 Python 可以正常解析。
- server-body fail 文件与 `payload.snapshot.json` byte-for-byte 一致。
- 典型报错是 `Unexpected character: '}'`，位置在 JSON 末尾。
- `Unexpected character: 'I'` 不是独立坏包，而是 server 先抛异常后 client 拿到
  `Internal Server Error` 文本。

原因判断：

- 这不像 HTTP 传输坏包，也不像 serializer 输出坏 JSON。
- 更像 `JsonValue.fromStr` 解析过程中，`JsonParser` / `cache` / `ArrayList` / `String.rawData()` 等 live 状态偶发被 GC 影响。
- 当前 mark verifier 只能检查“已标记对象指向未标记对象”等情况；如果某个本应作为 root 的 live parser 对象没有被枚举，verifier 不一定能直接发现。

状态：

- 未完全解决。
- 下一步应沿 stack/root 枚举、safepoint/saferegion、`JsonParser` live object 保护继续排查。
- 补充：结合后续 allocator/UAF 排查记录，以上方向现已降为**次优先级**；当前应优先沿
  `20260615_case27_uaf_allocator_investigation.md` 中的“同 region 双 owner / 同槽双分配”
  路径继续定位，只有当 allocator 方向被证伪后再回到 root/safepoint。
- 2026-06-15 晚新增补充：
  - `result/until_stall_loop/run_20260615_225903/` 这轮虽然跑满了 `60s` 且没有直接崩溃，
    但仍有 `fail=17`；
  - 之后按源码核对发现：当前树里已找不到 `CJ_MS_STW_SWEEP` 的实际判断分支，
    `StopAndWaitForAllSweep() + ForceFinishAllUnswept()` 已经是默认路径。
  → 因此这轮结果**不能再归因**到 `CJ_MS_STW_SWEEP=1`，只能作为一次“未崩但仍有 parse failure”
    的时序观测。

## 加过的排查手段

### 1. `CJ_MS_VERIFY_MARK=1`

用途：

- GC 标记完整性校验。
- 检查 `missed-mark`、`staleManaged`、`staleNonTraceable`、`staleInvalid`。

使用方式：

```bash
CJ_MS_VERIFY_MARK=1 ./run_compare.sh
```

分析方式：

```bash
rg "missed-mark|staleManaged|staleNonTraceable|staleInvalid|mark-completeness" msgc/gc.log
```

结论作用：

- 帮助定位 write barrier new-edge 漏入队。
- 帮助定位 mutator exit WBB flush 问题。
- 帮助确认 JSON 类 missed-mark 是否被压下去。

### 2. `EmptyCacheDiag`

用途：

- 追踪 empty region cache 的 push/pop 配对。
- 判断 region 是否被重复 push、错误 pop、状态类型不匹配。

典型日志：

```text
[MS-GC] EmptyCacheDiag reason=pop-without-in-cache-mark ...
```

分析方式：

```bash
rg "EmptyCacheDiag|pop-without-in-cache-mark|push-while-marked-in-cache|pop-bad-region-type" msgc/gc.log
```

结论作用：

- 把 SIGSEGV 中非法对象地址问题收敛到 empty-cache / region 状态损坏方向。
- 辅助区分是 mark 漏扫还是 region 元数据/复用路径异常。

### 3. case27 body / graph 诊断

用途：

- 判断 JSON 是否真的被传输或序列化写坏。
- 在 `toString` 前遍历 JSON 对象图，判断对象图是否可达、是否存在明显断链。

使用方式：

```bash
CASE27_BODY_DIAG=1 \
CASE27_DUMP_BAD_BODY=1 \
CASE27_GRAPH_DIAG=1 \
CASE27_GRAPH_DIAG_RATE=128 \
./run_compare.sh
```

分析方式：

- 查看 `msgc/stdout.txt` 中 `server-body-fail`、`client-response-fail`、`json-walk-done`。
- 查看 result 目录下 dump 出来的 `case27_*fail*.json`。
- 用 Python 校验 JSON 是否合法，必要时和 `payload.snapshot.json` 做 byte diff。

结论作用：

- 证明部分失败 JSON 字节本身合法。
- 将问题从“HTTP/JSON 输出坏包”收敛为“parser/root/live 状态被 GC 影响”。

### 4. `CJ_MS_BG_SCAVENGE=0`

用途：

- 关闭后台 scavenge，隔离问题是否与 background reclaim/scavenge 相关。

使用方式：

```bash
CJ_MS_BG_SCAVENGE=0 ./run_compare.sh
```

结论作用：

- 如果关闭后问题消失，优先怀疑 reclaim/scavenge/empty-cache 并发路径。
- 如果关闭后仍存在，则继续看 mark/root/sweep 或 parser live 状态。

### 5. `CJ_MS_STW_SWEEP`

用途：

- 诊断并发 sweep 是否制造 stale edge / invalid stateWord / invalid typeInfo。
- 将 sweep 从并发路径切到下一轮 `PreEnumRoots()` 单线程 force-finish unswept backlog。

状态：

- 历史文档里这个开关曾经存在，但**当前源码树里已搜不到实际分支**。
- 当前实现中：
  - `MarkSweepCollector::PreEnumRoots()` 默认就会走
    `StopAndWaitForAllSweep() + ForceFinishAllUnswept()`；
  - case2 的历史总结文档也明确写了“默认路径已收口，不再依赖 `CJ_MS_STW_SWEEP` gate”。
- 因此：
  - 不应再把 `CJ_MS_STW_SWEEP=1` 当作受控实验开关；
  - `run_20260615_225903/` 这轮“60s 无崩溃但 fail=17”的结果，只能作为负证据，
    不能直接证明 sweep 串行化压住了崩溃。

### 5.1 allocator 方向新增负证据

- `result/until_stall_loop/run_20260615_225123/`：
  开 `CJ_MS_DETECT_DOUBLE_OWNER=1 + CJ_MS_DETECT_REUSE_LIVE=1`，仍先出现 managed SIGSEGV，
  但没有先打出 `DOUBLE-OWNER` / `REUSE-LIVE`。
- `result/until_stall_loop/run_20260615_225707/`：
  再加 `CJ_MS_DETECT_DOUBLE_ALLOC_SLOT=1` 后，benchmark 跑到 `46.6s / 11043 req / fail=0`，
  仍未打出 `DOUBLE-ALLOC-SLOT` / `DOUBLE-OWNER` / `REUSE-LIVE`，最终被外部 `SIGKILL(9)` 终止。
- 当前结论：
  - “同 region 双 owner / 同槽双分配”仍未被证实；
  - 但 slot 级追踪有明显时序/内存开销，这组负结果不能直接当作最终排除。

### 5.2 普通新对象保活链路需要单独复核

- 代码上能直接看到：
  - `src/ObjectModel/MObject.cpp` 的普通 `NewObject()` 只是 `Allocate + SetClassInfo`；
  - `src/ObjectModel/MArray.inline.h` 的 `NewKnownWidthArray()` 也是 `Allocate + SetClassInfo + SetLength`；
  - `src/Heap/MCollector/MarkSweepCollector.cpp` 虽然实现了 `MarkNewObject()`，
    但源码里能直接看到的显式调用点主要是 `BaseObject::OnFinalizerCreated()`。
- 这意味着一个新的高优先级怀疑点是：
  - 在 `GC_PHASE_MS_CONCURRENT_MARK(21)` 期间，parser/serializer 创建的普通局部对象
    如果只存在于栈和临时变量里，而没有及时通过 barrier / deferred queue 进入当前轮标记，
    就可能解释“输入字节合法，但 `JsonValue.fromStr` 偶发失败”的现象。
- 这条线当前应与 allocator 方向并行排查，而不是继续把 parse failure 解释成“HTTP body 被写坏”。

### 5.3 `CJ_MS_DIAG_NEW_ALLOC_FORCE_MARK` 诊断实验

用途：

- 验证“并发 mark 期间普通新对象默认不被及时 mark”是否真实存在；
- 观察如果诊断式地对普通 `NewObject/NewArray` 补 `MarkNewObject()`，case27 现象会怎么变化。

实现：

- 在 `src/Common/BaseObject.{h,cpp}` 增加：
  - `DiagnoseOrdinaryNewObject()`
  - `DumpMSNewObjectDiagSummary()`
- 在 `src/ObjectModel/MObject.cpp` / `src/ObjectModel/MArray.inline.h`
  的普通对象、普通数组、pinned 对象分配成功后打点；
- 通过环境变量控制：
  - `CJ_MS_DIAG_NEW_ALLOC_LOG=1`
  - `CJ_MS_DIAG_NEW_ALLOC_FORCE_MARK=1`
  - `CJ_MS_DIAG_NEW_ALLOC_LOG_LIMIT=<N>`

结果：

- `result/until_stall_loop/run_20260615_232758/`（c8/p100K/60s）在该 gate 下
  benchmark **跑满 60s 且未 stall**，结果：
  `total=39758 ok=39690 fail=68 throughput=660req/s`；
- 同轮 `gc.log` 末尾最近一次 summary：
  `total=958453 phase20=65927 phase21=892526 phase22=0 markedBefore=161312 markedAfter=958453 forced=797141 logged=128`。

结论：

- 绝大部分普通新对象确实发生在 `GC_PHASE_MS_CONCURRENT_MARK(21)`；
- 其中大多数默认是 `markedBefore=0`，说明“普通新对象并发 mark 期间未被立即 mark”是
  **真实现象**；
- 但 gate 只能把症状从“容易 stall/跑挂”压到“仍有少量 parse fail”，不能视为最终修复。

### 5.4 force-mark 后仍存在的硬崩溃

复现：

- `result/until_stall_loop/run_20260615_233034/`（c8/p100K/15s）在
  `CJ_MS_DIAG_NEW_ALLOC_FORCE_MARK=1` 基础上再开
  `CASE27_BODY_DIAG=1`、`CASE27_DUMP_BAD_BODY=1`。

现象：

- 进程仍会崩，栈为：
  `std/core/array_common.cj::__CNat14CPointerHandleIG_E6<init>HPG_RNat5ArrayIG_E`
  → `stdx.encoding.json:WriteBuffer.appendEscape`
  → `jsonWriteWithoutFormat`
  → `JsonObject.toString`
  → `benchmark.cj:processRequest`
- 日志关键字：
  `Thread "schd-worker" catched unhandled SIGUSR1 (Bus error: 10)`。

结论：

- 普通新对象 mark-at-alloc 缺口是一个真实因子；
- 但 serializer / `WriteBuffer` / `String` / backing-array 路径上仍有第二层问题，
  否则这个 Bus error 不应继续存在。

### 5.5 dump 再校验后的最新判断

本轮新抓到：

- `case27_server-body-fail_req651_slot-1.json`
- `case27_client-response-fail_req651_slot-1.json`
- `case27_client-response-fail_req397_slot-1.json`
- `case27_client-response-fail_req1645_slot-1.json`

校验结果：

- `server-body-fail_req651` 与 `payloads/100K.json` **byte-for-byte 一致**；
- `client-response-fail_req651` 的 body 就是 `Internal Server Error`；
- `client-response-fail_req397` / `req1645` 是 `100408/100409` 字节的
  **valid JSON**，Python 正常解析，但 Cangjie client 仍报
  `The json data is Non-standard, please check`。

当前判断：

- “server 解析 valid request body 偶发失败”仍然存在；
- “client 对 valid response 偶发 parse fail”仍然存在；
- “server 侧 `JsonObject.toString` / `WriteBuffer.appendEscape` 偶发 Bus error”
  仍然存在；
- 所以当前状态是**部分缓解，但未根治**。

### 5.6 `CJ_MS_DIAG_PREENUM_RETURN_ALL_REGIONS` 的 no-STW 复验

背景：

- 2026-06-16 早些时候的诊断实验表明，只要在 `PreEnumRoots()` 里把
  `ReturnAllAllocBufferRegionsToCentral()` 放进一个短 STW 窗口，small old-gen mismatch
  就能稳定归零，case27 连跑 5 轮也都 `fail=0`。
- 但这还不能区分“是 return-all 本身起作用”，还是“其实是 STW 把 flush/gen-flip 竞态压住了”。

因此本轮又做了一次更窄的实验：

- 保留 `CJ_MS_DIAG_PREENUM_RETURN_ALL_REGIONS=1`
- 保留新分配对象 sweep/gen 追踪
- **只去掉** `PreEnumRoots()` 诊断分支里的 `ScopedStopTheWorld`
- 在 `AllocBufferMS::ReturnRegionToCentral()` 中新增 central-return 诊断：
  region 回到 central 前，记录该 region 上是否存在“出生在 old-gen 上的新对象”

实现位置：

- [src/Heap/MCollector/MarkSweepCollector.cpp](/Users/lipeisheng/code/cangjie/cangjie/cangjie_runtime_MSGC/runtime/src/Heap/MCollector/MarkSweepCollector.cpp:472)
- [src/Heap/MAllocator/MSAllocBuffer.cpp](/Users/lipeisheng/code/cangjie/cangjie/cangjie_runtime_MSGC/runtime/src/Heap/MAllocator/MSAllocBuffer.cpp:867)
- [src/Common/BaseObject.cpp](/Users/lipeisheng/code/cangjie/cangjie/cangjie_runtime_MSGC/runtime/src/Common/BaseObject.cpp:557)
- [src/Common/BaseObject.h](/Users/lipeisheng/code/cangjie/cangjie/cangjie_runtime_MSGC/runtime/src/Common/BaseObject.h:95)

运行：

- 结果目录：
  `MSGC_RESULT/Result/case27_http_complex_json/result/preenum_return_all_no_stw_central_only_20260616/run_20260616_103118/`
- 参数：
  `100K / c8 / 15s / heap12gb / proc6`
- 开关：
  `CJ_MS_DIAG_PREENUM_RETURN_ALL_REGIONS=1`
  `CJ_MS_DIAG_NEW_ALLOC_TRACK_SWEEP=1`
  `CJ_MS_DIAG_NEW_ALLOC_TRACK_LIMIT=1048576`
  `CJ_MS_DIAG_NEW_ALLOC_LOG=1`
  `CJ_MS_DIAG_NEW_ALLOC_LOG_LIMIT=8`

业务结果：

- 这轮 benchmark 正常结束：
  `total=6816 ok=6816 fail=0 throughput=422req/s`

但 GC 诊断结果说明两个更重要的事实：

1. **central 中重新看到了 old-gen born new object**

`gc.log` 中出现多条：

```text
[MS-GC][NEW-ALLOC-DIAG][RETURN-CENTRAL] ...
allocObjectSweepGen=8 allocCurrentSweepGen=10 ...
returnRegionSweepGen=8 currentSweepGen=10
```

说明“出生在旧代 active region 上的新对象”并不只是留在 TL region 里，而是真的回到了 central。

2. **去掉 STW 后，small mismatch 统计会回潮**

同一轮 summary 中，多次出现：

```text
returnedCentralOldGen=2/5
returnedCentralPhase21OldGen=1/3
returnedCentralMarkedBeforeZeroOldGen=2/5
markedBeforeZeroGenMismatch=2/7/14
```

最终累计为：

```text
total=3047 phase20=242 phase21=2805
markedBefore=462 markedBeforeZero=2585
genMatch=2571 genMismatch=476
phase21GenMismatch=408
markedBeforeZeroGenMismatch=14
```

3. **`activeAfter` 非零，表明 mutator 确实在和 no-STW flush 并发**

`gc.log` 多轮都能看到：

```text
activeBefore=143 returned=143 activeAfter=3
activeBefore=145 returned=145 activeAfter=17
activeBefore=156 returned=155 activeAfter=20
```

这和带 STW 时 `activeAfter=0` 的结果不同，说明 no-STW return-all 并没有封住
“flush 之后、gen flip 之前”这段窗口。

当前结论更新为：

- 不应把 `CJ_MS_DIAG_PREENUM_RETURN_ALL_REGIONS=1` 的早期正结果简单解读成
  “只要 return-all 就修好了”；
- 更准确的理解是：
  **短 STW + full flush** 能压住 small old-gen mismatch；
  **full flush 单独存在** 还不够；
- 因此，这个 gate 目前仍然只是**诊断工具**，不能直接当成正式修复提交。

### 5.7 `STW-only / no-return-all` 反向对照

为了继续回答“是 STW 更关键，还是 return-all 更关键”，本轮又做了一个反向对照：

- 保留 `CJ_MS_DIAG_PREENUM_RETURN_ALL_REGIONS=1`
- 在 `PreEnumRoots()` 诊断分支里**重新加回短 STW**
- 但**不调用** `ReturnAllAllocBufferRegionsToCentral()`

也就是让 collector 在一个闭世界窗口里完成：

- `ClearAllMarkBitmaps()`
- `ReclassifyUnsweptToSwept()`
- `PrepareForSweep()`

但故意**不处理 alloc-buffer active/cached region**

实现位置：

- [src/Heap/MCollector/MarkSweepCollector.cpp](/Users/lipeisheng/code/cangjie/cangjie/cangjie_runtime_MSGC/runtime/src/Heap/MCollector/MarkSweepCollector.cpp:474)

运行：

- 结果目录：
  `MSGC_RESULT/Result/case27_http_complex_json/result/preenum_stw_only_no_return_all_20260616/run_20260616_111952/`
- 参数不变：
  `100K / c8 / 15s / heap12gb / proc6`

业务结果：

- benchmark 仍然正常结束：
  `total=7465 ok=7465 fail=0 throughput=461req/s`

但 GC 侧结论很明确：

1. **STW 没有把 active region 清掉**

多轮都能看到：

```text
PreEnumRoots: alloc-buffer STW-only diag activeBefore=169 cachedBefore=0 returned=0 activeAfter=169 cachedAfter=0
```

说明 STW 只是“停住了 mutator”，并没有改变 active region 跨代际保留这件事。

2. **small old-gen mismatch 大量回潮**

最终 summary：

```text
total=3067 phase20=164 phase21=2903
markedBefore=540 markedBeforeZero=2527
genMatch=2265 genMismatch=802
phase21GenMismatch=705
markedBeforeZeroGenMismatch=262
```

这和另外两组实验形成很清楚的对照：

- `full-flush + STW`：`markedBeforeZeroGenMismatch=0`
- `return-all + no-STW`：`markedBeforeZeroGenMismatch=14`
- `STW-only + no-return-all`：`markedBeforeZeroGenMismatch=262`

3. **后续运行中仍然会看到 old-gen born new object 回 central**

这一轮仍然记录到了 `65` 条
`[MS-GC][NEW-ALLOC-DIAG][RETURN-CENTRAL]`。

因此当前结论需要再更新一层：

- 不能说“STW 本身就是根因”；
- 更像是：
  - `return-all` 负责处理 active/cached region 的代际归属；
  - STW 负责收口 flush 与 gen-flip 之间的竞态窗口；
  - 两者缺一不可，单独使用都不能达到 `full-flush + STW` 的效果。

### 5.8 `Clear` 放到 STW 外、只保护 `Reclassify + Prepare` 仍然不行

又做了一轮更窄的边界实验：

- 不做 `DrainAllAllocBufferCachedRegions()`；
- `ClearAllMarkBitmaps()` 放在 STW 外；
- 短 STW 只包：
  `ReclassifyUnsweptToSwept() + PrepareForSweep()`；
- 日志关键字：
  `PreEnumRoots: reclassify/prepare STW-only diag ...`

结果目录：

- `MSGC_RESULT/Result/case27_http_complex_json/result/preenum_reclassify_prepare_stw_only_20260616/run_20260616_115210/`

负载：

- `100K / c8 / 15s / heap12gb / proc6`

业务结果：

- benchmark 正常结束：
  `total=7416 ok=7416 fail=0 throughput=459req/s`
- 没有 `SIGSEGV` / JSON fail。

GC 诊断结果：

1. **active region 没有被处理**

```text
PreEnumRoots: reclassify/prepare STW-only diag activeBefore=168 cachedBefore=0 activeAfter=168 cachedAfter=0
...
PreEnumRoots: reclassify/prepare STW-only diag activeBefore=175 cachedBefore=0 activeAfter=175 cachedAfter=0
```

2. **small-path mismatch 进一步恶化**

最终 summary：

```text
total=2753 phase20=213 phase21=2540
markedBefore=529 markedBeforeZero=2224
genMatch=1901 genMismatch=852
phase21GenMismatch=703
markedBeforeZeroGenMismatch=323
```

对比上一组 `STW-only + no-return-all` 的
`markedBeforeZeroGenMismatch=262`，这轮更差。

3. **central 里仍能看到 old-gen born new object 回流**

- `RETURN-CENTRAL` 共 `67` 条；
- 最终 summary 前 sweep-track 仍然有：

```text
tracked=215 trackedGenMismatch=60
trackedPhase21GenMismatch=27
trackedMarkedBeforeZeroGenMismatch=13
returnedCentralOldGen=12
returnedCentralPhase21OldGen=8
returnedCentralMarkedBeforeZeroOldGen=12
```

结论：

- **只**保护 `Reclassify + PrepareForSweep` 不够；
- 把 `ClearAllMarkBitmaps()` 移到 STW 外，至少在当前实现里没有看到收益，反而让 mismatch 更高；
- 如果继续走短 STW 方案，当前证据更支持把
  `ClearAllMarkBitmaps() + ReclassifyUnsweptToSwept() + PrepareForSweep()`
  放回同一个受保护窗口；
- 如果目标是把 small mismatch 真正压下去，仍然需要 `return-all` 或等价的 active-region retag/flush。

### 6. `CJ_MS_VERIFY_ROOTS`

用途：

- 后续用于检查 stack/root 枚举路径。
- 当前剩余 JSON parse failure 更适合用它继续看 `JsonParser`、`cache Array<JsonValue>`、`ArrayList<Byte>`、`String.rawData()` 是否被稳定枚举。

状态：

- 是下一步排查 parser/root 问题的入口。

## 编译、替换、验证流程

### 1. 编译并替换 MSGC runtime

推荐使用 runtime/gc 下已有脚本：

```bash
cd /Users/lipeisheng/code/cangjie/cangjie/cangjie_runtime_MSGC/runtime/gc
bash run.sh ms-replace
```

该步骤会：

- 编译 MarkSweep runtime。
- 将 `libcangjie-runtime.dylib` 替换到 `/Users/lipeisheng/code/cangjie/cangjie-pkg/self/dev260609/cangjie`。
- macOS 上重新 codesign，避免 dylib 替换后被系统 SIGKILL。

注意：

- 替换 runtime 后必须重新编译 `.cj` 用例。
- 不能复用旧 binary 做验证。

### 2. 按 case27 README 跑对比

进入 case27 目录：

```bash
cd /Users/lipeisheng/code/cangjie/cangjie/cangjie_runtime_MSGC/runtime/MSGC_RESULT/Result/case27_http_complex_json
```

典型验证命令：

```bash
MSGC_SDK_ROOT=/Users/lipeisheng/code/cangjie/cangjie-pkg/self/dev260609/cangjie \
MSGC_STDX_ROOT=/Users/lipeisheng/code/cangjie/cangjie-pkg/self/dev260609/darwin_aarch64_cjnative \
COPYGC_SDK_ROOT=/Users/lipeisheng/code/cangjie/cangjie-pkg/dev/20260507/cangjie \
COPYGC_STDX_ROOT=/Users/lipeisheng/code/cangjie/cangjie-pkg/dev/20260507/darwin_aarch64_cjnative \
CASE27_CONCURRENCY=8 \
CASE27_PAYLOAD=100K \
CASE27_DURATION_MS=60000 \
HEAP_SIZE=12gb \
PROCS=6 \
CJ_MS_VERIFY_MARK=1 \
CASE27_BODY_DIAG=1 \
CASE27_DUMP_BAD_BODY=1 \
CASE27_GRAPH_DIAG=1 \
CASE27_GRAPH_DIAG_RATE=128 \
TAG=verify_c8_p100K \
./run_compare.sh
```

要求：

- 使用 `run_compare.sh`，不要自己临时拼 runner。
- 每个 SDK 使用隔离环境，避免 `DYLD_LIBRARY_PATH` 污染。
- 确认 CopyGC SDK 不被替换。

## 结果分析 checklist

### 1. 先看 compare report

文件：

```text
result/<timestamp>_*/compare_report.md
```

检查：

- CopyGC 退出码是否为 0。
- MSGC 退出码是否为 0。
- MSGC 是否有 failed requests。
- QPS / latency 是否明显异常。

### 2. 再看 MSGC stdout

文件：

```text
result/<timestamp>_*/msgc/stdout.txt
```

检查：

```bash
rg "SIGSEGV|SIGABRT|Internal Server Error|server-body-fail|client-response-fail|json-walk-done|Parse Error" msgc/stdout.txt
```

判断：

- 如果有 SIGSEGV/SIGABRT，先看崩溃栈和崩前日志。
- 如果只有 JSON fail，继续检查 dump 文件是否是合法 JSON。

### 3. 再看 MSGC GC 日志

文件：

```text
result/<timestamp>_*/msgc/gc.log
```

检查：

```bash
rg "missed-mark|staleManaged|staleNonTraceable|staleInvalid|EmptyCacheDiag|TestAndSetMarkBit|OutOfMemory" msgc/gc.log
```

判断：

- `missed-mark` 非 0：优先看 barrier/root/exit WBB。
- `staleManaged` 非 0：优先看 stale edge、悬挂引用、region 复用。
- `EmptyCacheDiag` 异常：优先看 reclaim/scavenge/empty-cache push-pop。
- `TestAndSetMarkBit slotIdx >= totalSlots`：说明非法对象地址进入 mark，重点看 region 状态和对象边界。

### 4. 校验失败 JSON 是否真的坏

对 dump 出来的失败 JSON 使用 Python：

```bash
python3 - <<'PY'
import json
from pathlib import Path

for p in sorted(Path(".").glob("case27_*fail*.json")):
    try:
        json.loads(p.read_text())
        print("OK", p, p.stat().st_size)
    except Exception as e:
        print("BAD", p, e)
PY
```

如果 server body fail，需要和 payload snapshot 对比：

```bash
cmp -l case27_server-body-fail_xxx.json payload.snapshot.json | head
```

判断：

- 如果 Python 解析失败，说明字节确实坏，需要看 HTTP/serializer/内存写坏。
- 如果 Python 解析成功且和 payload 一致，说明不是 JSON 字节坏，而是 MSGC 下 parser/runtime 状态问题。

## 当前总体状态

已解决或基本收敛：

- shrink 导致 shard 0 seg。
- finalizer `MarkNewObject` 子图未进入本轮扫描。
- write barrier new-edge 漏入队。
- mutator exit 时 WBB pending work 丢失。
- JSON 对象图大面积 missed-mark。
- empty-cache 问题已加诊断和防护，近期未再复现同类边界地址 SIGSEGV。
- 普通新对象 `force-mark` 实验证明了并发 mark 期间默认 `markedBefore=0` 的现象，
  并能明显降低“60s 内直接 stall/跑挂”的概率。

仍待解决：

- 默认开启 `CJ_MS_BG_SCAVENGE` 时仍需多轮验证是否完全稳定。
- 偶发合法 JSON 被 `JsonValue.fromStr` 解析失败（server request / client response 两侧都还在）。
- `JsonObject.toString` / `WriteBuffer.appendEscape` 仍可在 force-mark 条件下 Bus error。
- verifier 仍曾出现非 JSON 的 `std.sync:Timer -> closure` missed-mark，说明调度/timer/root/new-edge 相关路径可能还有小缺口。
- 需要继续检查 `JsonParser` / `WriteBuffer` 期间的 stack/root 枚举、safepoint/saferegion 交互，以及 backing `String/Array` 是否仍可能在 GC 中被遗漏或被覆写。

## 2026-06-16 新增诊断结论：没有抓到 same-cycle sweep，但确认存在 old-gen allocation

本轮新增了两类诊断：

1. 在普通 `NewObject / NewPinnedObject / NewKnownWidthArray` 之后记录：
   - `objectSweepGen`
   - `currentSweepGen`
   - `genMatch`
2. 在 `SweepRegion()` / `SweepLargeRegions()` 真正 free slot 或 free large object 时，
   反查该地址是不是“本轮 mark-phase 新分配对象”。

运行：

- 目录：
  `MSGC_RESULT/Result/case27_http_complex_json/result/diag_new_alloc_gen_20260616_001927/`
- 负载：
  `100K / c8 / 15s`
- 开关：
  `CJ_MS_DIAG_NEW_ALLOC_TRACK_SWEEP=1`
  `CJ_MS_DIAG_NEW_ALLOC_TRACK_LIMIT=1048576`
  `CJ_MS_DIAG_NEW_ALLOC_LOG=1`
  `CJ_MS_DIAG_NEW_ALLOC_LOG_LIMIT=8`
- **未开** `CJ_MS_DIAG_NEW_ALLOC_FORCE_MARK`

结果：

- benchmark 正常退出：
  `total=8113 ok=8112 fail=1 throughput=502req/s`
- `gc.log` 里**没有任何**
  `[MS-GC][NEW-ALLOC-DIAG][SWEPT]`
- 每轮 `sweep-track` 汇总也都是：
  `swept=0 sweptGenMatch=0 sweptGenMismatch=0`

因此：

- **这轮没有抓到“本轮新分配对象被 same-cycle sweep”的直接证据。**

但 `genMismatch` 已经大量命中。最终累计 summary：

```text
total=9674 phase20=901 phase21=8773
markedBefore=1843 markedBeforeZero=7831
genMatch=6892 genMismatch=2782
phase21GenMismatch=2311
markedBeforeZeroGenMismatch=939
```

进一步分桶后：

- `1843` 条是 `regionType=4` large object / large array，`objectSweepGen=0`
  ，这部分更像 large-region 元数据未参与 small-region `sweepGen` 协议；
- **剩余 `939` 条是真正值得关注的 non-large mismatch**：
  - 全部在 `regionType=1/2`；
  - 全部 `markedBefore=0`；
  - 全部满足 `objectSweepGen = currentSweepGen - 2`。

也就是说：

- **small / pinned 普通新分配对象确实在 old gen region 上分配出来了。**

## 新的第一优先级怀疑点：active alloc-buffer region 跨过了 gen flip

这轮 `gc.log` 的 `preGCMark` snapshot 多次显示：

```text
alloc-buffer regions: total=129/131/154... active=同值 cached=0
```

而当前 `PreEnumRoots()` 只做了：

```cpp
centralPool->DrainAllAllocBufferCachedRegions();
centralPool->ReclassifyUnsweptToSwept();
centralPool->PrepareForSweep();
```

也就是只 drain cached，没有处理 `activeRegions_`。

同时：

- `CentralRegionPool::PrepareForSweep()` 的注释明确写着
  `FlushAllAllocBuffers returns TL regions to swept`；
- `AllocBufferMS::ReturnAllRegionsToCentral()` 也已经存在，会把 active/cached region
  一起回收到 central，并在返回 central 时打当前 `sweepGen`；
- 但当前实际调用链里，并没有在 `PreEnumRoots()` 触发它。

所以目前最强怀疑是：

1. 上一轮 active TL region 保持 `oldSweepGen`；
2. `PrepareForSweep()` 把全局 `sweepGen` 翻到了 `newSweepGen`；
3. mutator 在 phase20/21 继续从这些 old-gen active region 上分配；
4. 于是命中了大批
   `objectSweepGen = currentSweepGen - 2`。

这条链已经从“猜测”升级为**有运行证据 + 有源码落点**的主线。

## 2026-06-16 新增诊断结论：full alloc-buffer flush 实验已把 small old-gen mismatch 清零

本轮做了一个更直接的诊断实验：

- 在 `PreEnumRoots()` 增加临时开关
  `CJ_MS_DIAG_PREENUM_RETURN_ALL_REGIONS=1`
- 在这个分支里把
  `ClearAllMarkBitmaps() -> ReturnAllAllocBufferRegionsToCentral() -> PrepareForSweep()`
  放进同一个短 STW 窗口
- 目的不是提交最终修复，而是验证：
  **如果把 alloc-buffer 手里的 active/cached region 都在 gen flip 前收回，939 条 small old-gen mismatch 会不会直接消失。**

运行：

- 结果目录：
  `MSGC_RESULT/Result/case27_http_complex_json/result/preenum_return_all_20260616_003801/`
- 负载：
  `100K / c8 / 15s`
- 开关：
  `CJ_MS_DIAG_PREENUM_RETURN_ALL_REGIONS=1`
  `CJ_MS_DIAG_NEW_ALLOC_TRACK_SWEEP=1`
  `CJ_MS_DIAG_NEW_ALLOC_TRACK_LIMIT=1048576`
  `CJ_MS_DIAG_NEW_ALLOC_LOG=1`
  `CJ_MS_DIAG_NEW_ALLOC_LOG_LIMIT=8`

关键日志：

```text
PreEnumRoots: full alloc-buffer flush activeBefore=101 cachedBefore=0 returned=101 activeAfter=0 cachedAfter=0
...
PreEnumRoots: full alloc-buffer flush activeBefore=164 cachedBefore=0 returned=164 activeAfter=0 cachedAfter=0
```

说明这个实验路径是**真的把 active/cached 都清到 0** 了，不是只改了统计。

最终对比：

- baseline：

```text
genMismatch=2782
phase21GenMismatch=2311
markedBeforeZeroGenMismatch=939
```

- full-flush：

```text
genMismatch=1295
phase21GenMismatch=1144
markedBeforeZeroGenMismatch=0
```

进一步按 regionType 分桶：

- `small/pinned non-large mismatch: 939 -> 0`
- 剩余 `1295` 条 mismatch **全部是 `regionType=4` large object**
- `[NEW-ALLOC-DIAG][SWEPT]` 仍然是 `0`

同时 benchmark 结果也改善为：

```text
total=8380 ok=8380 fail=0 throughput=508req/s
```

对比 baseline 的：

```text
total=8113 ok=8112 fail=1 throughput=502req/s
```

可以得到一个已经相当强的结论：

1. **small old-gen mismatch 的根因链已经被坐实**：
   active alloc-buffer region 确实跨过了 gen flip。
2. **这 939 条不是观测噪声**：
   把它们清掉以后，短跑下用户态失败也同步从 `1` 降到了 `0`。
3. **remaining mismatch 不再混杂**：
   现在可以把后续排查重点从 “small + large 混在一起” 收敛成两条独立线：
   - small path：进入正式修复设计；
   - large path：单独确认 `objectSweepGen=0` 的语义和风险。
4. **这个分支仍然只是诊断开关**：
   当前依赖短 STW，不应直接当作最终默认修复提交。

## 2026-06-16 新增验证：5 轮复查没有再复现 small-path 问题

为了确认上面的 full-flush 结果不是单次“概率没撞到”，继续直接跑了 5 轮同配置短跑：

- 目录：
  `MSGC_RESULT/Result/case27_http_complex_json/result/preenum_return_all_5runs_20260616_093233/`
- 配置：
  `100K / c8 / 15s / heap12gb / proc6`
- 开关：
  `CJ_MS_DIAG_PREENUM_RETURN_ALL_REGIONS=1`
  `CJ_MS_DIAG_NEW_ALLOC_TRACK_SWEEP=1`
  `CJ_MS_DIAG_NEW_ALLOC_TRACK_LIMIT=1048576`
  `CJ_MS_DIAG_NEW_ALLOC_LOG=1`
  `CJ_MS_DIAG_NEW_ALLOC_LOG_LIMIT=8`

结果汇总：

- `run_01`: `total=8262 ok=8262 fail=0 throughput=504req/s`
- `run_02`: `total=10208 ok=10208 fail=0 throughput=627req/s`
- `run_03`: `total=9924 ok=9924 fail=0 throughput=615req/s`
- `run_04`: `total=9658 ok=9658 fail=0 throughput=593req/s`
- `run_05`: `total=9310 ok=9310 fail=0 throughput=576req/s`

对应的 GC summary 在 5 轮里都满足：

- `markedBeforeZeroGenMismatch=0`
- `sweptHits=0`
- `genMismatch` 仍非 0，但范围稳定在 `1429~1669`
- `phase21GenMismatch` 仍非 0，但范围稳定在 `1286~1569`

因此这里可以再往前收敛一步：

1. **“把 alloc-buffer active/cached region 在 gen flip 前全部收回”**，
   对 small-object old-gen mismatch 的抑制不是一次性偶然，而是已经有 5 轮重复证据。
2. **case27 用户态异常在这 5 轮里也没有再出现**：
   `fail` 全部为 0，至少说明 small-path 这条链和业务异常具有稳定相关性。
3. **large-object 支线被单独暴露出来了**：
   small-path 归零后，剩余 mismatch 还在，说明后续要把 large region / `objectSweepGen=0`
   单独当成下一阶段问题，而不是再把 small/large 混在一起看。

## 2026-06-16 新增验证：`PreEnumRoots` 分段计时（`Reclassify+Prepare` 一并放入 STW）

- 有效目录：
  `MSGC_RESULT/Result/case27_http_complex_json/result/preenum_reclassify_prepare_stw_timing_20260616/run_20260616_145046/`
- 同组首跑
  `run_20260616_144736`
  的 `stage-times` 日志格式串写错，把 `Pretty(...).Str()` 按 `%zu` 打印，数值无效，已作废。
- 配置：
  `100K / c8 / 15s / heap12gb / proc6`
- 开关：
  不设置 `CJ_MS_DIAG_PREENUM_RETURN_ALL_REGIONS`
  保留
  `CJ_MS_DIAG_NEW_ALLOC_TRACK_SWEEP=1`
  `CJ_MS_DIAG_NEW_ALLOC_TRACK_LIMIT=1048576`
  `CJ_MS_DIAG_NEW_ALLOC_LOG=1`
  `CJ_MS_DIAG_NEW_ALLOC_LOG_LIMIT=8`

业务结果：

```text
total=8063 ok=8063 fail=0 throughput=499req/s
```

最后一轮 GC 的关键分段时间：

```text
PreEnumRoots: stage-times stopSweep=24,756,543us clearBitmaps=10,387,425us stw=249us
PreEnumRoots: 35,144,231us
total gc time: 35,442,837 us
```

前面多轮也能看到同样趋势，`stw` 基本都在微秒级到几百微秒量级，例如：

```text
stopSweep=8us clearBitmaps=71us stw=13us
stopSweep=36,402us clearBitmaps=223us stw=22us
stopSweep=1,130,304us clearBitmaps=13,136us stw=24us
```

这轮可以直接排除一个误判：

1. **`ReclassifyUnsweptToSwept() + PrepareForSweep()` 这段 STW 本身不重**，
   它不是 `PreEnumRoots` 长尾的主要来源。
2. **当前 `PreEnumRoots` 的大头已经非常清楚**：
   尾轮主要耗时来自
   `StopAndWaitForAllSweep()` 和 `ClearAllMarkBitmaps()`，
   尤其最后一轮分别约 `24.8s` 和 `10.4s`。
3. **因此下一步若继续收敛 `PreEnumRoots` 长尾，优先级应转向**：
   - 为什么 `StopAndWaitForAllSweep()` 会在尾轮等待到 `24.8s`
   - 为什么 `ClearAllMarkBitmaps()` 会在尾轮膨胀到 `10.4s`
   - 而不是优先怀疑短 STW 本身太重
