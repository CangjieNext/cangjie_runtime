# Step43 MSGC Mark 统计本地化与周期汇总

## 问题

helper 每扫描一个 WorkBuf 原来会连续更新 `slot.processed`、`helperProcessedCount_` 和
`pendingWorkerScanned_` 三个原子，其中前两项只用于周期末日志，且 `slot.processed` 没有读取点。
mutator assist 也同时更新多组全局诊断原子，多个 P 会争用相同 cache line。

## 实现

- 新增 `MSMarkWorkerLocalStats`：
  - `totalProcessed_` 由所属 helper 普通累加，worker 全部停止后由 GC main 汇总。
  - credit 使用每 worker 独立原子发布槽，不再写共享 `pendingWorkerScanned_`。
  - 连续工作时累计 4 个 WorkBuf 再发布；无工作或 quiesce 时发布尾数。
- 新增 `MSMarkAssistStats`，存放 calls、bgStolen、globalPaid、processed。
- 每个 `AllocBufferMS` 保存自己的 assist stats，assist 热路径只写 owner-local 普通字段。
- mutator 在 mark 中退出时，通过 `PublishExitedLog()` 将统计低频折叠到 collector；仍存活的 P 在
  MarkTermination STW 中统一汇总并 reset。
- `MSMarkAssist()` 直接接收调用者 `AllocBufferMS&`，删除一次 TLS 查找。
- 删除未使用的 `assistFastSkipCount_`、`assistLocalMarked_`。
- 合并重复口径：processed 使用一份，bg credit consumed 直接取 bgStolen。
- 删除仅用于旧日志的 `globalShareCount_`；写屏障条目量继续由 `MSGCWriteBarrierFlushStats` 统计。
- `bgCreditInjected_` 只有 GC main 读写，改为普通 `size_t`。

## 保留的同步状态

- `workersInFlight` / `assistsInFlight`：参与 quiesce 和收敛正确性。
- `bgScanCredit_`：GC worker 与 mutator assist 实时交换 pacing credit。
- `assistWorkPerByteScaled_`：GC main 动态发布、mutator 实时读取。

这些字段不是周期末诊断统计，不能下沉为无同步的 worker-local 数据。

## 统计口径

- calls：成功通过 `TryEnterAssist()` 的调用数，不再统计 quiesce 后被拒绝的尝试。
- bgStolen：从背景扫描 credit 抵扣的对象数，同时作为 consumed 口径。
- globalPaid：assist 取得 full WorkBuf 后承诺偿还的 debt，按 WorkBuf 容量计。
- processed：assist 实际扫描的对象数。

Assist base ratio 闭环新增两个周期口径：GC main 在自己的 `RunConcurrentMarkLoop()` 普通累加
`gcMainProcessedObjects`，helper 继续使用 worker-local `TotalProcessed()` 并在 worker 停止后写入
`helperProcessedObjects`。二者只在 GC main 线程汇总，不增加共享原子计数。

## 验证

- `MSMarkLocalStatsTest` 覆盖 4-buffer 批量发布、尾数 flush、assist merge/reset。
- runtime Release 增量构建验证完整接口和 `activeLog_` ABI 静态断言。
- 早期 `c1000/p100K/2s` smoke 没有覆盖 Step42 private-partial 并发闭包的长时正反馈，不能作为完整
  正确性结论。撤销该闭包行为后，保留本 Step 的全部统计本地化，再执行 `c10/p10K/60s`：
  6658 req/s、400392 成功、0 失败。

结果目录：
`MSGC_RESULT/Result/case27_http_complex_json/result/20260712_013018_case27_c10_p10k_validate_c10_p10K_60s_heap4gb_1run`
