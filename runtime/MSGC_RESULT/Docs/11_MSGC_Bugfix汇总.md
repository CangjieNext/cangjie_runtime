# MSGC Bugfix 汇总：当前代码口径

> 当前代码基线：`MSGC-bugfix-regionScan` / `24ca097ace`。历史 bug 记录用于追踪根因；当前行为只以源码和新结果为准。

## 1. 最大架构修复：移除对象 WorkBuf Mark

提交 `24ca097ace` 将普通 Mark 从对象 buffer 生命周期改为：

```text
small object:
  MARK plane + SCAN plane + pendingWordSummary + REGION task

large object:
  large marked flag + OBJECT task

closure:
  task ledger (queued + leased) + STW reconciliation
```

这不是单个性能优化，而是对下列历史风险的结构性收敛：

- mutator 私有尾块在并发终点不可见；
- worker 私有 partial 的发布/回收顺序；
- 全局队列短暂为空但 worker 仍持有工作；
- quiesce 重新发布 work 导致 closure feedback/OOM；
- barrier winner 已标记但未能追加日志；
- mutator 退出清理日志时的竞态。

当前要验证的新风险是：

- mark word 与 pending summary 的发布顺序；
- `OWNED → IDLE` release race；
- budget replacement 与旧 lease completion 顺序；
- ledger 与两条 ready lane 的一致性；
- sealed collect-set 的 ALLOC free-slot过滤；
- large OBJECT task publication。

## 2. Barrier/old-edge 问题

历史容器、rehash、array copy 错误常表现为：

- HashMap/ArrayList 元素丢失；
- value mismatch；
- JSON 树中对象提前回收；
- SIGSEGV。

共同检查点是 deletion barrier 必须执行：

```text
load old → publish and flush old → overwrite destination
```

当前标量和 struct/array bulk barrier 共用 `AllocBufferMS` 中持久的
`MSBarrierMarkBuffer`。每个 old edge 在 memcpy/memmove 前完成本地 capture；buffer
由 threshold、4-way 冲突、owner 退出或 MarkTermination STW 统一 flush，而不是在
wrapper 返回时析构 flush。

仍需回归：

- zero-destination struct；
- overlapping array copy；
- static struct；
- atomic CAS 成功/失败；
- old ref 指向 small、large、SPAN。

## 3. marked-but-not-scanned

旧方案可能出现 mark winner 已置位，却没有可消费的对象日志条目。当前小对象中：

```text
mark=1, scan=0
```

本身就是显式待扫描状态。producer 再发布 `pendingWordSummary`，scheduler只负责给该 Region
一个 owner。

Verifier 应检查：

- `MARK & ~SCAN` 中是否仍有必须扫描的旧对象；
- pending summary 是否 stranded；
- Region state 是否非 IDLE；
- ledger 为 0 时 ready lane 是否非空；
- reachable marked object 是否指向 unmarked target。

## 4. Sweep/Mark 周期交叠

下一轮 Mark 清 bitmap 前，上一轮所有 Sweep 必须退出并 Force-Finish residual backlog。
当前 `PreEnumRoots()` 的关键边界仍是：

```text
stop/wait Sweep
→ force finish unswept
→ reset current Mark work state/planes
→ establish collect epoch
→ enumerate roots
```

SCAN plane 的加入没有取消这个边界；它增加了“旧 Mark task owner 必须静默”的要求。

## 5. 新对象与 collect-set

普通 small object 不统一 mark-at-alloc。当前 Region owner 对 collect-set 应用：

```text
candidates &= ~allocBitmap & validMask
```

collect-set 已在 Mark 前从分配供给中封存；`~allocBitmap` 的作用是排除 free slots，不是
区分同一 Region 内的“新旧对象”。因此需要验证：

- `collectEpoch` 正确；
- ALLOC bit 的 `1=free` 语义与 BUMP materialization一致；
- BUMP materialization 正确；
- finalizer-created object 仍经 `MarkNewObject()`/deferred root进入闭包；
- large object marked flag 与注册时机正确。

## 6. TypeInfo 与对象解析

历史 Case8/复杂 JSON fault 暴露过 TypeInfo 高位、stale phase、非法对象解释等问题。
Region scan owner 根据 slot 地址直接读取 retained TypeInfo，因而以下防线仍重要：

- managed reference range/Region kind检查；
- SPAN 解析到 large primary；
- TypeInfo 非空与归一化；
- `HAS_REF/NO_REF` 与实际 TypeInfo一致；
- 对象初始化完成前不能被当作旧 snapshot candidate。

debug build 在 Region/OBJECT task 中保留对应检查。

## 7. 分配器与 page heap

历史修复类别：

- Grow/Shrink 与 shard 锁顺序；
- large连续 Region donor steal；
- 跨 chunk free-run搜索；
- empty cache ownership；
- scavenged Region reuse；
- heap range/committed end 发布。

Region-scan Mark 增加的分配交界：

- active Region改为 `[sizeClass][scanClass]`；
- Mark 期间按 WU/byte累计 Assist debt；
- collect-set sealing 与 ALLOC free-slot过滤；
- Region reset时清理 pending/task state；
- `NO_REF` Region不提交 SCAN plane。

## 8. 并发调度与终止

当前 closure 的核心断言：

1. 每个 admitted task在 ledger中有一份 outstanding。
2. claim 不减少 outstanding，只把 queued 变为 leased。
3. local batch flush后才能 complete lease。
4. owner预算耗尽时先发布 replacement，再 complete旧 lease。
5. STW drain 看到 ledger非零却 claim不到 ready task时直接报错。
6. ledger归零后两条 ready lane必须为空。

这组不变量替代了旧的“全局对象队列 + 私有尾块 + in-flight counter”终止条件。

## 9. Case25–31 的当前定位

| 用例 | 主要覆盖面 |
|---|---|
| case25 | HTTP复杂 JSON、同步/锁与基础稳定性 |
| case26 | Enum Assist与复杂 JSON对比 |
| case27 | 分配、barrier、Mark/Sweep、HTTP/JSON综合压力 |
| case27 high-P | 多 P、helper/Assist、shard与调度扩展性 |
| case28 exception/FFI | 异常跨 FFI边界 |
| case28 HTTP | bulk barrier/STW/复杂 JSON |
| case29 | 外部 Java client与高并发服务端 |
| case30 | 外部 Go client |
| case31 | 外部 Cangjie MSGC client |

旧结果若不是由 `24ca097ace` 对应 SDK生成，只能证明旧版本行为。Region-scan 合入后必须
重新编译并生成新 result directory。

## 10. 当前验证优先级

### P0 正确性

- `cjMSVerifyMark`；
- case25–31 短矩阵；
- bulk/atomic barrier定向测试；
- ledger/pending/scan-state debug checks；
- ASan/TSan可用平台。

### P1 收敛

- outstanding task峰值；
- REGION/OBJECT完成数；
- termination residual WU；
- Assist paid/debt；
- high-P 下 helper target与空转。

### P2 性能

- concurrent/termination wall time；
- batch hit/eviction与 mark-word RMW；
- RSS/retained；
- Sweep backlog；
- throughput/失败率。

## 11. 状态记录规则

Bugfix 文档应明确区分：

- 已在当前源码看到；
- 已编译；
- 已通过定向测试；
- 已通过长压；
- 仅历史版本通过；
- 仍未闭环。

不能把“代码存在”写成“case 已通过”，也不能把旧架构压测数字写成 Region-scan 版本效果。

## 12. 源码索引

- [`MarkSweepCollector.cpp`](../../src/Heap/MCollector/MarkSweepCollector.cpp)
- [`MSMarkScheduler.cpp`](../../src/Heap/MCollector/MSMarkScheduler.cpp)
- [`MSMarkBatch.h`](../../src/Heap/MCollector/MSMarkBatch.h)
- [`MSWorkUnit.h`](../../src/Heap/MCollector/MSWorkUnit.h)
- [`MSMarkBarrier.cpp`](../../src/Heap/MBarrier/MSMarkBarrier.cpp)
- [`MSRegionInfo.h`](../../src/Heap/MAllocator/MSRegionInfo.h)
- [`MSAllocBuffer.cpp`](../../src/Heap/MAllocator/MSAllocBuffer.cpp)
- [`MSFreeManager.cpp`](../../src/Heap/MAllocator/MSFreeManager.cpp)

历史原始记录仍位于 [`MSGC_RESULT/bugfix`](../bugfix/)。
