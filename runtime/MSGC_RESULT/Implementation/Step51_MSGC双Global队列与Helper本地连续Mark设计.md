# Step51：MSGC 双 Global 队列与 Helper 本地连续 Mark 设计

## 0. 结论摘要

当前 `globalFull_` 实际混放 full/partial WorkBuf，GC Main、Helper 和
Mark Assist 围绕同一把队列锁交换工作。Helper 每处理一个 buf 都退出
`workersInFlight`、发布 credit，并重新进入控制循环；TLS Assist 产生的
partial `putCur_` 则长期留在 TLS，直到填满或 MarkTermination 才收口。

本次改造采用以下模型：

1. Global 拆成 `globalFull_` 和 `globalPartial_`：
   - Full 只保存 `len == WorkBuf::kCapacity`；
   - Partial 只保存 `0 < len < WorkBuf::kCapacity`。
2. Helper 在一个 active burst 内连续处理多个 buf，不再逐 buf 进出
   `workersInFlight`，也不生成 `bgScanCredit_`。
3. Helper 的本地 `putCur_` 达到 128 个对象后优先转成自己的下一个
   `getCur_`；不足 128 时，先消费 Global，再回头消费自己的小尾巴。
4. TLS 每次 Assist 结束都将非空 partial `putCur_` 发布到
   `globalPartial_`；下次只有成功获取 Full 输入后，才从 Partial 队列
   获取一个 buf 作为新的输出 `putCur_`。
5. GC Main 继续承担当前 backlog、Helper 扩展、Assist ratio、时间预算、
   收敛和 quiesce 职责；其工作获取和收敛判断扩展到双队列。

方案取消跨线程直接窃取 TLS 活动 `putCur_`，从结构上避免 TLS 生命周期、
active buffer 数据竞争和 steal slot 管理问题。

## 1. 目标、范围与非目标

### 1.1 目标

- 减少 Helper 每处理一个 WorkBuf 后的共享原子、credit 发布和控制循环开销。
- 让对象图的局部闭包优先留在同一 Helper，提高 TypeInfo、对象页和 mark
  bitmap 的访问局部性。
- 在高分支对象图产生 full buf 时继续快速向 Global 暴露并行工作。
- 将 TLS Assist 的 partial 尾部及时提供给 GC Main/Helper，降低并发 Mark
  尾部隐藏工作量。
- 让 Helper 获取顺序确定且容易验证：本地大尾巴、Global Full、Global
  Partial、本地小尾巴、等待。
- 保持现有 mark winner、弱引用延后处理、MarkTermination fixed-point 和
  GC phase 语义。

### 1.2 范围

修改范围限于 MSGC：

- `WorkBuf` 全局队列组织和 `PerPGCWork` 获取/发布接口；
- `MarkSweepCollector` 的 ConcurrentMark、Helper、Assist、quiesce 和
  MarkTermination 队列收口；
- Helper 周期统计；
- 与上述行为对应的单元测试和契约测试。

### 1.3 非目标

- 不修改 CopyGC。
- 不改变 `WorkBuf::kCapacity == 1024` 及编译器可见 WorkBuf 布局。
- 不增加 Helper 之间的直接 work stealing。
- 不增加 Helper 对 GC Main 私有 `putCur_` 的直接访问。
- 不增加 Helper 对 TLS 活动 `putCur_` 的直接访问。
- 不调整 GC Main 的动态 Helper 策略、Assist ratio 公式或 Mark 时间预算。
- 不在本步骤优化 mark bitmap 原子操作、对象字段扫描和 WeakRef 队列。

## 2. 当前实现与问题

### 2.1 单队列混放

当前 `LFStack` 同时保存 full 和 partial WorkBuf，通过 `count_` 与
`fullCount_` 统计数量。`Pop()` 取栈顶任意 buf，`PopFull()` 为避开 partial
需要沿链表搜索第一个 full buf。两个操作及 `GetCounts()` 都竞争同一 mutex。

这使两个不同用途绑定在一起：

- Helper/GC Main 可以处理任意非空 buf；
- Mark Assist 只能获取 full buf。

### 2.2 Helper 每 buf 协调

当前 Helper 每轮都执行：

```text
TryEnterWorker
  -> workersInFlight +1
  -> DrainOneBufRound
  -> RecordScanned / PublishPendingCredit
  -> workersInFlight -1
  -> 无工作时 backoff 50us
```

即使本轮扫描刚产生了大量本地 `putCur_`，Helper 也不会直接把它作为下一轮
输入；partial 输出继续隐藏在本地，full 输出进入 Global。每个输入 buf 都
承担两次共享 in-flight 原子更新和统计 credit 协议。

### 2.3 TLS partial 隐藏

Mark Assist 当前只消费一个 Full Global buf。扫描产生的 partial 输出保留在
TLS `PerPGCWork::putCur_`，只有填满时才进入 Global。TLS 不再触发 Assist
或者对象图尾部较窄时，这些灰对象会等到 MarkTermination 才被统一 Dispose。

### 2.4 主要约束

`getCur_`、`putCur_`、`WorkBuf::len` 和 `objs[]` 都是单所有者数据，不能让
Helper 直接跨线程窃取活动 `putCur_`。安全共享必须发生在 Owner 放弃所有权
并将完整 WorkBuf 指针 Push 到 Global 之后。

## 3. WorkBuf 生命周期与双 Global 队列

### 3.1 队列定义

引入两个独立队列：

```text
globalFull_:
  invariant: 每个节点 len == 1024

globalPartial_:
  invariant: 每个节点 0 < len < 1024
```

两个队列分别持有自己的 mutex/count，共享同一个 `markWorkEpoch_`。向任一
队列发布 WorkBuf 都通知该 epoch，避免等待 Helper 丢失唤醒。

新增一个小型 `MSMarkGlobalWork`，内部直接管理 Full/Partial 两条 lane。每条
lane 拥有独立的链表、mutex 和 count；`GetCounts()` 使用 `std::lock` 同时
锁定两条 lane，得到一致快照。该类型统一提供：

- `Push(WorkBuf*)`：按 `Full()` 路由，拒绝空 buf；
- `PopFull()`；
- `PopPartial()`；
- `PopForMark()`：Full 优先，其次 Partial；
- `GetCounts()`：返回 full/partial/total 的一致快照语义；
- `DrainAll(...)`：MarkTermination 收口两条队列。

所有调用点都通过该类型路由，禁止直接操作 lane 链表，避免 invariant 漂移。

### 3.2 生命周期

WorkBuf 只能按以下状态流转：

```text
POOL
  -> ACTIVE_PUT（一个生产者独占）
  -> GLOBAL_FULL 或 GLOBAL_PARTIAL（Global 独占）
  -> ACTIVE_GET（一个消费者独占）
  -> POOL
```

也允许 Owner 在未发布时执行本地移交：

```text
ACTIVE_PUT
  -> 同一 Helper 的 ACTIVE_GET
```

本地移交只是指针交换，不复制对象。完成移交后必须为新的输出方向取得另一个
空 WorkBuf，保证同一 buf 不同时作为输入和输出。

### 3.3 发布规则

- 写入后 `len == 1024`：立即 Push 到 Full。
- TLS Assist 结束时 `0 < len < 1024`：Push 到 Partial。
- Helper/GC Main 的私有 partial：按各自策略本地继续使用；只有 Dispose 或
  明确发布点才进入 Partial。
- 空 buf：返回 `WorkBufPool`，不得进入任一 Global 队列。

### 3.4 获取规则

- GC Main/Helper 的普通 Mark 输入：Full 优先，然后 Partial。
- Mark Assist 输入：只允许 Full。
- TLS Assist 输出：只有成功取得 Full 输入后，才从 Partial 取得；没有
  Partial 时从 Pool 取得空 buf。
- 从 Partial 取得的输出 buf 中原有灰对象继续保留，TLS 只在其尾部追加；
  再发布后由后续消费者统一扫描。

## 4. Helper 本地连续 Mark

### 4.1 Active burst

`workersInFlight` 的作用域从“一个 buf”扩大为“一段连续工作 burst”：

```text
WaitUntilEligible
  -> TryEnterWorker（一次）
  -> 连续选择并 drain 多个 buf
  -> 没有任何可执行工作
  -> ExitWorker（一次）
  -> BackoffWorker，最多 50us
```

quiesce 或 target helper 变化必须在 buf 边界检查。Helper 不得在扫描一个
对象引用列表的中间放弃所有权。

### 4.2 每轮选择顺序

处理完一个输入 buf 后，根据本地 `putCur_` 长度选择下一项：

```text
if putCur.len >= 128:
    本地提升 putCur -> getCur
else:
    尝试 globalFull_
    若为空，尝试 globalPartial_
    若仍为空，且 putCur 非空：
        本地提升小 putCur -> getCur
    若仍无工作：
        结束 active burst
```

因此：

- `128..1023` 的本地输出立即连续处理，保留局部性；
- `<128` 的本地输出允许 Global 已发布工作插队；
- Global 为空时仍会处理自己的小尾巴，不会仅因阈值把本地闭包留给 STW；
- full 输出在写满瞬间进入 Full 队列，不参与本地提升。

### 4.3 热路径统计

Helper 不再：

- 维护 `pendingCredit_`；
- 发布 `publishedCredit_`；
- 为 `bgScanCredit_` 提供输入；
- 每个 buf 进出 `workersInFlight`。

每个 slot 只维护单线程写入的普通 `totalProcessed_`。Helper task 结束且
`WaitFinish()` 返回后，GC Main 才读取并汇总该计数，避免运行期原子更新。

### 4.4 等待和唤醒

只有以下条件同时成立时 Helper 才结束 burst：

- 本地没有可提升的 `putCur_`；
- Full 为空；
- Partial 为空。

退出 `workersInFlight` 后，通过 `markWorkEpoch_.WaitForChange()` 最多等待
50us。Full/Partial 的 Push、Helper target 变化和 quiesce/stop 都会递增
WorkEpoch，从而提前唤醒等待线程。

## 5. GC Main 保持协调者语义

GC Main 继续执行当前每轮协调工作：

- 累计自身处理量；
- 读取 Global backlog；
- 通过 `MSMarkWorkerPolicy` 只升不降地扩展 Helper；
- 根据 Full backlog 更新 Assist ratio EMA；
- 根据自身扫描量注入 `bgScanCredit_`；
- 检查 workers/assists in-flight；
- 检查 Mark 时间预算；
- 进入 quiesce 或等待 WorkEpoch。

需要做的适配仅包括：

1. 普通输入改为 Full 优先、Partial 次之。
2. `publishedBacklog` 使用 `full + partial`，用于 Helper 扩展和收敛判断。
3. `fullBacklog` 只使用 Full 数量，保持 Assist backpressure 的原语义。
4. 删除 `TakePublishedWorkerCredit()` 及 Helper credit 对
   `backgroundScanned` 的贡献；GC Main 自身扫描量仍可按现有衰减策略产生
   `bgScanCredit_`。
5. 收敛候选要求 Full 和 Partial 均为空。
6. quiesce residual 日志同时报告 Full、Partial 和 total。

“GC Main 基于当前实现不变”指其协调职责和策略不变，不代表继续读取已经删除
的 Helper credit，或在收敛时忽略新增加的 Partial 队列。

## 6. TLS Mark Assist

### 6.1 单次调用顺序

TLS `MSMarkAssist()` 按以下顺序执行：

```text
TryEnterAssist
  -> 先领取 GC Main 已生成的 bgScanCredit_（保留现有 credit-first）
  -> 剩余 debt 足够一个 WorkBuf 时尝试 PopFull
  -> PopFull 失败：ExitAssist，不触碰 Partial
  -> PopFull 成功：
       PopPartial 作为 putCur
       Partial 为空则从 Pool 获取空 putCur
       drain Full 输入
       输出写满时 Push Full，继续取得输出 buf
       调用结束时将非空未满输出 Push Partial
       空输出返回 Pool
  -> ExitAssist
```

Helper 不再产生 `bgScanCredit_`，但 GC Main 自身的 credit-first 机制不在本步骤
删除。Assist 的 debt 支付仍只按实际扫描的 Full 输入计算，作为输出 builder
取得的 Partial 原有对象不计入本次已扫描量。

### 6.2 Partial 合并

从 Partial 取出的 buf 可能已有 N 个待扫描灰对象。TLS 对它拥有独占写权限，
只执行尾部追加：

```text
原有 N 个 + 本次新发现 M 个
  -> N + M == 1024：Push Full
  -> 0 < N + M < 1024：Assist 结束时 Push Partial
```

原有 N 个对象并未在本次 Assist 中扫描，因此不能增加
`MSMarkAssistStats.processed`，也不能用于偿还本次 debt。

### 6.3 输出 buf 补充

当输出在扫描过程中填满后：

1. 满 buf 立即进入 Full；
2. 从 Pool 获取一个空 buf；
3. 继续扫描尚未处理完的输入 Full；
4. 调用结束时把最后一个非空未满输出发布到 Partial。

每次 Assist 最多从 Partial 领取一次初始输出 builder，不在同一次 Assist 中
连续搬走多个 Partial，避免 Mutator 暂时隐藏过多待扫描工作。

### 6.4 TLS 退出

Assist 返回前不再保留非空 `gcWork_.putCur_`。因此 TLS `Fini()` 不需要新增
steal slot 清理；现有 active write-barrier log 和其它 per-P cache 生命周期
保持不变。

## 7. 收敛、Quiesce 与 MarkTermination

### 7.1 收敛条件

GC Main 只有在以下条件同时满足时才能开始 quiesce：

```text
GC Main 当前没有输入
globalFull_ 为空
globalPartial_ 为空
workersInFlight == 0
assistsInFlight == 0
```

获取双队列计数时，`MSMarkGlobalWork::GetCounts()` 使用 `std::lock` 同时
持有 Full/Partial 两把 mutex，不能先后读取后把瞬时组合误判为空。

### 7.2 Quiesce

quiesce 顺序保持：

1. phase 切到 `QUIESCING`，阻止新的 worker/assist claim；
2. 等待 `workersInFlight == 0 && assistsInFlight == 0`；
3. Stop worker control；
4. `WaitFinish()`；
5. 汇总每个 Helper 的只读退出统计。

若 quiesce 在 Helper buf 边界到达，Helper 停止继续 claim，把仍属私有的
`getCur_`/`putCur_` 在 task 退出的 `Dispose()` 中按 Full/Partial 路由发布。
这些 residual 交给 MarkTermination，不得在 quiesce 中重新启动 Helper。

### 7.3 MarkTermination

STW fixed-point drain 必须在每轮：

1. flush 所有 write-barrier active log；
2. Dispose GC Main、Helper 和所有 TLS `PerPGCWork`；
3. drain Full；
4. drain Partial；
5. drain deferred WeakRef queue；
6. 扫描 main queue，直到所有来源都为空。

Full 与 Partial 的 drain 顺序不影响正确性；固定采用 Full 后 Partial，便于日志
和测试稳定。任何从 private/Global 移入 main queue 的对象都已经是 trusted
grey，不能重新以 Discover 结果决定是否扫描其字段。

### 7.4 不变量

- 一个 WorkBuf 任意时刻只有一个 Owner。
- 进入 Global 后，原 Owner 不再访问。
- Full 队列中不存在 partial/empty。
- Partial 队列中不存在 full/empty。
- Assist 只有在取得 Full 输入后才持有 Partial 输出。
- 收敛判断覆盖两条 Global 队列和所有 in-flight participant。
- MarkTermination 能收口所有 private 与 Global 工作。

## 8. 代码结构与修改点

### 8.1 `src/Common/LFStack.h`

- 保留 `WorkBuf` 布局和容量。
- 将 `PerPGCWork` 从单一 `globalFull_` 指针改为
  `MSMarkGlobalWork` 指针。
- 拆分普通输入、Full 输入、Partial 输出 builder 的获取接口。
- 新增按 Full/Partial invariant 路由的发布接口。
- 新增 Helper 本地 `putCur_ -> getCur_` 移交 helper。
- `Dispose()` 按长度发布到正确队列；`Reset()` 仍只归还 Pool。

若 `MSMarkGlobalWork` 直接放在 `LFStack.h`，应保持它只负责 WorkBuf 交换，
不依赖 Collector 策略，避免 Common 层反向依赖 Heap。

### 8.2 `src/Heap/MCollector/MarkSweepCollector.h/.cpp`

- Collector 持有 Full/Partial 两个 Global lane。
- `TransferRootsToGCWork()` 按长度路由根 WorkBuf。
- write-barrier full log 路由到 Full；Owner 退出时发布的 partial active log
  路由到 Partial。
- `RunMarkWorker()` 改为 active burst 循环。
- `DrainOneBufRound()` 拆分为“获取策略”和“只 drain 当前 getCur”两个层次，
  避免 Helper 为本地移交重新访问 Global。
- GC Main 获取、backlog、convergence、quiesce residual 适配双队列。
- `MSMarkAssist()` 实现 Full 输入 + Partial builder。
- `DrainGlobalLFStackToMainQueue()` 同时 drain 两条 lane。
- 删除 Helper credit 汇总路径，保留退出后 processed 日志。

### 8.3 `src/Heap/MCollector/MSMarkLocalStats.h`

- `MSMarkWorkerLocalStats` 简化为单线程普通累计。
- 删除 `pendingCredit_`、`pendingBuffers_`、`publishedCredit_` 以及对应原子
  Publish/Take API。
- Assist 自己的 `MSMarkAssistStats` 保持。

### 8.4 `src/Heap/MCollector/MSMarkWorkerControl.h`

保持 phase/target/in-flight/Assist 协议，并新增无副作用的
`CanContinueWorker(workerId)` 原子只读接口：

- Helper 可以在一次 `TryEnterWorker()` 后连续处理整个 burst；
- 每个 buf 边界能观察 quiesce/stop；
- 退出 burst 后才执行一次 `ExitWorker()`；
- 50us backoff 改由 WorkEpoch 等待，可被任一 Global lane 通知提前结束；
- 不能通过重复 TryEnter/Exit 模拟 buf 边界检查。

### 8.5 测试文件

优先扩展或新增：

- `test/MSGCTailDrainContractTest.sh`
- `test/MSGCDisableMarkAssistContractTest.sh`
- `test/MSMarkWorkerPolicyTest.cpp`
- 新增双 Global lane/PerPGCWork 单元测试；
- 新增 Helper burst 与无 worker credit 契约测试；
- 新增 Assist Partial 合并和 debt 计数测试。

## 9. 诊断与统计

周期日志至少保留：

```text
[mark-workers]
configured=<GC main + helper 上限>
processed=<Helper 退出后汇总对象数>
mainProcessed=<GC main 对象数>

[mark-quiesce]
duration
residualFull
residualPartial
residualTotal
```

计数原则：

- `processed` 使用 Helper task 结束后的线程私有值，不在运行期原子发布。
- 不恢复 `publishedCredit_` 或 `bgScanCredit_` 的 Helper 贡献。
- 不在生产热路径新增逐 Push/Pop、逐 promote 的共享统计；Full/Partial 当前
  数量直接来自 lane 自身 count。
- `assistProcessed` 只记录实际 drain 的 Full 输入对象，不能包含作为
  `putCur_` 合并的 Partial 原有对象。

## 10. 测试与验收

### 10.1 单元测试

至少覆盖：

1. `len == 1024` 只能进入 Full。
2. `1..1023` 只能进入 Partial。
3. empty 只能回 Pool。
4. 普通 Pop 顺序为 Full 后 Partial。
5. Assist Pop 只能取得 Full。
6. TLS 只有成功取得 Full 后才取得 Partial builder。
7. Partial builder 追加后：
   - 未满重新进入 Partial；
   - 填满进入 Full。
8. Helper `len == 127/128/1023/1024` 的优先级边界。
9. Helper 连续多个 buf 只进出一次 in-flight。
10. Helper processed 只在 task 结束后汇总，不生成 background credit。

### 10.2 并发与终止测试

- Full/Partial 并发 Push/Pop 不重复、不丢失 WorkBuf。
- quiesce 与 Helper 本地提升竞争时，所有 private 工作最终进入
  MarkTermination。
- quiesce 与 Assist Partial 发布竞争时，不漏标；允许进入 residual，但必须
  被 STW fixed-point drain。
- TLS 退出、Runtime shutdown 和 worker pool STOPPING 不发生悬空 WorkBuf。
- WeakRef 仍只在主线程终止阶段分类。
- `CJ_MS_DISABLE_ASSIST=1` 契约不受双队列影响。

### 10.3 构建与正确性

实施后先执行：

- 相关 C++ 单元测试和 shell contract tests；
- MSGC Runtime 增量构建；
- 小对象、链表、宽对象图、WeakRef 和持续写屏障用例；
- `cjMSVerifyMark=1` 的 mark completeness 验证。

本步骤不自动执行完整 SDK/stdlib/stdx 全量构建，除非用户在实施阶段另行要求。

### 10.4 性能验收

使用 Step50 相同口径，对比修改前后：

- 单线程、Assist 关闭；
- 4 Mark 线程、Assist 关闭；
- 4 Mark 线程、Assist 开启；
- case27 P160（稳定性问题允许时）。

必须同时报告：

- ConcurrentMark 与 MarkTermination；
- `mainProcessed + helperProcessed`；
- quiesce 时 Full/Partial residual；
- HTTP 成功吞吐、P99、CPU、RSS；
- 退出码和 Runtime 身份。

性能目标不是只降低某个计数，而是：

- 4 线程相对单线程 Mark 加速比高于 Step50 的约 1.78x，且结果可重复；
- Helper 每 buf 的 in-flight/credit 原子操作消失；
- MarkTermination residual 不因 Partial 合并明显上升；
- Assist 开启时业务吞吐不因取消 Helper `bgScanCredit_` 明显回退；
- 无 mark completeness、WeakRef、OOM 或 shutdown 回归。

## 11. 已确认决策

1. 本地连续处理阈值固定为 `WorkBuf::kCapacity / 8 == 128`。
2. Helper full 输出进入 Global Full。
3. Helper `putCur_ >= 128` 时优先本地提升。
4. Helper `putCur_ < 128` 时按 Full、Partial、本地小尾巴的顺序处理。
5. Helper 不生成 `bgScanCredit_`。
6. Helper processed 只在退出后用于 GC 日志。
7. GC Main 保持现有协调职责和策略。
8. 不实现 Helper 对 Helper/GC Main/TLS 活动 `putCur_` 的直接窃取。
9. Global 拆成 Full 与 Partial 两条 lane。
10. TLS Assist 结束时发布所有非空 partial 输出。
11. TLS 下次 Assist 必须先取得 Full 输入，之后才能取得 Partial 作为输出。
12. Helper 等待上限保持 50us，并可被新 Global 工作或 stop/quiesce 提前唤醒。
