# Step42 MSGC Global 任意 WorkBuf 并发消费与收敛

## 目标

取消 GC main/helper 对 full buffer 的优先语义：Global 中已经发布的 full/partial `WorkBuf` 一视同仁，
每次 claim 一个 buffer 并完整扫描。mutator assist 继续只获取 full buffer，避免改变积分偿债粒度。

## 实现

- `LFStack::GetCounts()` 在一次锁内返回 Global 的 total/full 数量。
- `PerPGCWork::AcquireOneBuffer()` 使用 `Pop()` 获取任意已发布 buffer，供 GC main/helper 使用。
- `PerPGCWork::AcquireOneFullBuffer()` 保留，供 mutator assist 使用。
- worker 动态扩缩容按 Global total backlog 计算；assist backlog 放大仍按 full backlog 计算。
- 并发收敛候选由“Global 无满块”改为“Global 完全为空”。
- 首次出现完整空闲候选后进入不可逆的 tail drain：helper 固定为配置上限，assist ratio 提升到
  `baseRatio * maxMultiplier`，并立即执行 one-way quiesce，不再提供 write barrier refill 窗口。
- one-way quiesce 关闭 worker/assist claim 并等待 in-flight 归零；即使此时 Global 出现 late work 也不
  恢复并发标记，直接交给 MarkTermination STW 固定点处理。
- quiesce 不发布 GC main/helper 的私有 partial `putCur_`；这些有界尾数同样由 MarkTermination STW
  统一 dispose/drain。
- 不在并发阶段发布 mutator 私有 partial。mutator 退出会执行 `AllocBufferMS::Fini()/Reset()`，并发遍历
  它们会引入重复发布和生命周期竞争；这部分仍由 MarkTermination STW 统一处理。

## 作用场景

- Global 中存在已发布 partial（例如 root/exited mutator flush），旧 `PopFull()` 会跳过它并留到 STW。

该改动只把已经发布到 Global 的 partial 工作前移到并发 mark；GC main/helper 自己新产生的 partial
尾数仍留给 MarkTermination。它不减少总扫描对象数，也不改变 mark bitmap、写屏障或 mutator assist
的语义。

## 收敛不变式

并发收敛需要同时满足：GC main 当前输入为空、Global 为空、worker in-flight 为 0、assist in-flight
为 0；第一次满足就立即进入 one-way quiesce，不再等待额外稳定轮次。GC main/helper/mutator 的私有
partial 与 mutator `activeLog_` 不属于并发
收敛输入，由 STW termination 兜底。这样避免持续写屏障负载下 partial 尾部反复发布形成正反馈。

2026-07-12 增加尾段闭合策略：第一次满足完整空闲条件后，worker policy 进入 `TAIL_DRAIN`，固定全部
helper 并将 assist 提升至最大配置倍率，然后立即 quiesce。`c10/p10K` 实测表明保留 3 轮稳定空闲会让
94.75% 的 ConcurrentMark 时间落在 tail，70.7% 周期被 refill，因此删除 3 轮计数和两个 50us 等待。
late Global、mutator `activeLog_` 和各线程 private partial 由紧随其后的 MarkTermination 统一扫描。
该策略以少量潜在 STW residual work 换取 ConcurrentMark 尾部不再反复点燃。

新增 `[mark-tail]` 日志：`entered`、`duration`、`refills`、`assistRatioScaled`、
`quiesceResidualBuffers`。`refills` 为兼容旧结果保留，新直接闭合实现中应恒为 0。

## 验证

- `MSWorkBufInvariantTest` 覆盖 GC 可取 Global partial、assist 不取 partial、私有 partial 等待 termination、
  total/full 计数一致性，并分别验证 Release/Debug trusted-grey 模式。
- `MSMarkWorkerPolicyTest`、`MSMarkWorkerControlTest` 验证动态 worker 与 quiesce 控制。
- `MSAssistPacingTest` 验证 tail assist 最大倍率及溢出饱和；`MSGCTailDrainContractTest.sh` 验证源码中
  不再存在 restart/resume quiesce 路径。
- runtime Release 增量构建通过。
- 初版曾在 quiesce 发布 GC main/helper 私有 partial 并恢复并发闭包；`c1000/p100K/5s` 短测未暴露
  问题，但 `c10/p10K/60s` 会把 ConcurrentMark 拉长到 2.49s、首轮 marked objects 放大到 7700 万，
  随后 OOM。该行为已撤销。
- 修复后用户原命令 `python3 build/scripts/run_case27_c10_p10k.py --run-timeout-sec 100` 完整通过：
  6658 req/s、400392 成功、0 失败；首轮 ConcurrentMark 602us、marked objects 43181、threshold 20MB。

结果目录：
`MSGC_RESULT/Result/case27_http_complex_json/result/20260712_013018_case27_c10_p10k_validate_c10_p10K_60s_heap4gb_1run`
