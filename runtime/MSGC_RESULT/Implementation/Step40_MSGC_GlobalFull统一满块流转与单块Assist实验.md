# Step40: MSGC GlobalFull 统一满块流转与单块 Assist 实验

## 目标

旁路但不删除 per-P `list1_/list2_`,让 GC 线程和 mutator assist 竞争同一份完整 WorkBuf,减少
write-barrier work 固定留给 owner mutator 的 local-first 偏置。

## 实现

- 满 `activeLog_` 直接发布到 `globalFull_`,不再进入 owner `list1_`。
- mark 扫描产生的 child 留在 `putCur_`;满 1024 后直接发布到 `globalFull_`,partial 保留到 STW。
- GC thread 和 mutator assist 每个 slice 只从 global 获取一个满 WorkBuf,完整消费后结束 slice。
- assist 的分配积分、background credit 与每个满块偿还 1024 object debt 的口径保持不变。
- `list1_/list2_` 字段和 legacy 方法保留,当前新路径不再写入。
- `PopFull()` 可跳过 global 中少量 partial buffer,避免 partial 栈顶挡住完整 work。
- 并发稳定轮数不变;判定只关注是否还有完整 global input。partial active/get/put work 由现有
  MarkTermination STW 的 flush、dispose、global drain 固定点闭合。

## 验证

- Release+MarkSweep 增量构建通过,安装并重签名到 compiler `output` SDK。
- `c8/p1K`,5 秒,`CJ_MS_VERIFY_MARK=1`:运行成功,无 FATAL/verifier 错误。
- `c100/p100K`,10 秒,`CJ_MS_VERIFY_MARK=1`:运行成功,无失败请求和标记错误。
- 压力短测 assist 累计占比 `80.68%`,原 60 秒基线为 `87.00%`;`assist local=0`,证明
  per-P local-first 已被旁路,但单 GC background loop 产能仍不足。
- 压力短测吞吐 `449 req/s`,与原 60 秒基线 `468 req/s` 同量级,暂未观察到吞吐提升。

## 结论

统一 global 流转只消除了 work ownership 偏置,没有增加后台 mark 算力。下一步应增加 2-3 个并行
background mark worker;否则 global 中的完整 work 仍主要被 mutator assist 抢走。partial 延后也会把
一部分闭合工作移到 MarkTermination,后续需结合 STW 数据决定是否增加提前发布阈值。

## 多 GC Mark Worker（已实现）

在统一 `globalFull_` 满块流转之上，已增加动态并发 mark worker：

- `GCParam.gcThreads` 现在表示包含 GC main 在内的总 mark 线程上限；`cjGCThreads` 可显式设置，
  默认值为 `min(4, max(1, processorNum / 2))`。
- 每个 helper 持有独立 `PerPGCWork`，从 `globalFull_` 获取一个 buffer 并完整扫描；扫描产生的满
  `putCur_` 重新发布到 Global，partial 保留到 worker 停止后交给 STW。
- `MSMarkWorkerPolicy` 根据 Global 满块压力动态调整 active helper：普通积压逐个增加，满块达到
  64 时直接拉满。helper 在单个 mark 周期内只增不减，直到 one-way quiesce 才停止；
  下一轮 GC 再从 0 active helper 重新按压力扩容。物理线程保持存活，worker 仅在 buffer 边界停放。
- `LFStack` 维护 `fullCount_`，压力采样和收敛检查不再每轮遍历链表。
- helper 扫描量计入 background credit，避免新增后台算力后 mutator 仍按旧口径重复偿还 debt。

## 新收敛条件（已实现）

并发收敛候选同时要求：

1. GC main 的当前输入为空。
2. Global 没有满 buffer。
3. `workersInFlight == 0`。
4. `assistsInFlight == 0`。
5. 上述条件连续稳定 3 轮。

进入最终握手时先把 worker/assist gate 从 `RUNNING` 切到 `QUIESCING`，禁止新领取任务，再等待
两个 in-flight 计数归零并复查 Global 满块。如果有 late work，则重新打开 gate 继续并发 mark；否则
停止 worker 并进入 MarkTermination。mutator 未满 `activeLog_` 不参与并发收敛，仍由 STW flush；
helper/mutator/GC main 的 partial `PerPGCWork` 也统一在 STW dispose 后做 fixed-point drain。

## 多线程验证结果（2026-07-11）

- 三组独立 C++14 测试通过：WorkBuf/full-count 生命周期、动态 worker 策略、worker/assist
  `RUNNING -> QUIESCING -> STOPPED` 与 in-flight 归零。
- Release MarkSweep 增量构建通过，安装并 codesign 到 compiler `output`；case27 在替换 runtime 后
  使用同一 output compiler/runtime/stdlib 重新编译，dyld 路径验证正确。
- `c1000/p100K/60s`、默认 assist 与默认 4 个总 GC 线程：`24009` 请求，`23786` 成功，
  `223` 个失败集中在结束阶段连接关闭，吞吐 `389 req/s`；136 个 GC 周期无 runtime fatal/OOM。
- 136 轮中 134 轮峰值使用 4 个线程；helper 累计扫描 `356,983,808` 对象。mutator assist 累计
  扫描 `623,717,376` 对象，占累计 marked 口径约 `55.75%`，明显低于单后台线程实验约 80%。
- ConcurrentMark 平均约 `353 ms`，P95 约 `666 ms`，最大约 `1.30 s`；没有触发 20 秒 mark budget。
- 当前每轮基于主循环采样，136 轮累计 `4635` 次扩容、`5071` 次缩容，说明 active helper
  存在较多抖动。这是修改前的问题证据；当前已删除周期内缩容，`scaleDown`
  日志字段保留为兼容/验证字段，新 runtime 中应始终为 `0`。

## Mark helper 周期内只增不减（2026-07-12）

`c1000/p100K` 重复运行中，退化样本在 live graph 放大后出现单轮十几次
`scaleUp/scaleDown`，ConcurrentMark 最大达到 `323.6ms`。旧策略的“连续 8 个低 backlog
样本”是按 GC main 循环次数而非墙钟时间计数，很容易在几十微秒内触发缩容。

当前改为：

1. 保留 backlog-per-thread、连续增长和 emergency backlog 三条扩容路径。
2. 删除 `STABLE_DOWN`、`lowSamples_` 与低水位缩容阈值。
3. collector 调用侧断言 `activeHelpers >= oldHelpers`，只处理 target helper 增长。
4. 进入 tail 时仍拉满 helper，quiesce 逻辑不变；新 GC 周期创建新 policy 后自然重置为 0。

该改动不改收敛条件和 mark 正确性，目标是用少量 helper idle/backoff 成本换取稳定的
并发 mark 产能，避免主机调度扰动被 helper 反复停放放大。

### Verifier A/B 结论

`CJ_MS_VERIFY_MARK=1` 在新 runtime 部分周期报告 `Future -> FutureOk` missed-mark。为判断归因，临时
恢复安装前 runtime、重新编译相同 `c1000/p100K` 用例后，4 个 verifier 周期中同样有 2 个非零，
累计 33 个 violation。因此该告警在本次多 worker 改动前已经存在，不能归因于并行 worker；它仍是
独立的 barrier/termination 正确性问题，需要后续单独定位。强制 `cjGCThreads=1` 在该压力下 mark
追不上分配并最终 OOM，也说明多后台 mark 算力对这个场景是必要的。

## Assist 调参环境变量

- 移除 `cjMSAssistRatioScale` 外部参数，定点精度在 collector 内固定为 `65536`。
- `cjMSAssistBaseRatioPercent`：基础 assist ratio 百分比，默认 `50`。
- `cjMSAssistBacklogStepBuffers`：Global 每积压多少个满 buffer 增加 `1x` 基础 ratio，默认 `16`。
- `cjMSAssistMaxMultiplier`：backlog 放大上限，默认 `4`。
- `cjMSAssistCreditDecayRounds`：background credit 线性衰减轮数，默认 `100`。
- 移除 `cjMSAssistDebtTrigger` 外部参数，触发阈值固定为 `WorkBuf::kCapacity`（当前 `1024`），
  与单次 assist 消费一个满 buffer 的粒度对齐。其余新参数均要求大于 `0`。
- assist ratio 内部定点精度提升到 `65536`，base percent 在截断前参与计算；backlog
  先做 `7/8` 历史值与 `1/8` 当前值的 EMA，再连续线性放大 ratio，消除整步跳变。

## Assist base ratio 周期闭环（2026-07-12）

- raw base 仍由 `markedObjects / allocatedBytes * cjMSAssistBaseRatioPercent` 计算。
- 首个有效周期直接采用 raw base；之后 raw 下降立即跟随，上涨单轮最多 `25%`。
- 上一轮 assist share 按 `assist / (assist + gcMain + helpers)` 计算，三者均为实际扫描 processed 口径。
- share 超过 `30%` 时，下一轮 feedback limit 为 `previousBase * 25% / actualShare`，
  直接向 `25%` 中心回调；share 低于 `20%` 不额外强推，仅允许 raw base 按上涨限速增长。
- 周期内原有 Global backlog EMA 和 `1x-4x` multiplier 保留，闭环只调整下一轮 base。

> **2026-07-17 现行策略更新**：上面的 `>30% -> 25%` 是本阶段最初落地的实验参数。
> 当前实现保留同一反馈公式和其他 Assist 限制，但触发条件调整为严格 `share > 90%`，
> 目标调整为 `80%`；`share == 90%` 不触发回压。

新日志 `[assist-pacing]` 输出 `rawBase/prevBase/nextBase/riseLimit/feedbackLimit/assistShare`
及 `riseLimited/shareLimited`。`[mark-workers]` 增加 `mainProcessed`，可与 helper `processed` 和
`[assist] processed` 复算 share。
