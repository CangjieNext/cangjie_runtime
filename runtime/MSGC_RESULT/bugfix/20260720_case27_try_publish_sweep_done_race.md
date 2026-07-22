# Case27 `TryPublishSweepDone()` handoff 竞态排查

## 基本信息

- 标题：Case27 MSGC 在 `PreEnumRoots()` 中触发 `CHECK(TryPublishSweepDone())`
- 日期：2026-07-20
- 提交/分支：`588affbf12` → `b6817f505a` → `e3e864ad09` → `ae76037bcc` → `1d1e8b7ec1`
- 平台：macOS aarch64

## 现象与复现

- 现象：case27 在约 20 秒、cycle 264 的 `PreEnumRoots()` 中 abort，日志为 `Check failed: TryPublishSweepDone()`。
- 实际结果：MSGC 进程收到 SIGABRT；CopyGC 对照正常完成。
- 期望结果：关闭所有 sweep admission、等待已进入的 sweep 完成后，正常发布 `sweepDone_`。

## 定位过程

- `TryPublishSweepDone()` 首次加入于 `588affbf12cb865df7a1ffdf254ece20a58483b1`（2026-07-15 23:30:50，多线程 sweep），不是 `e3e864ad09` 首次加入。
- `e3e864ad09` 增加了 `forceFinishSweepRunning_` 检查、`GetTotalUnsweptCountReference()` 复核，并把 `GetTotalUnsweptCount()` 切换为 aggregate counter。
- `ae76037bcc` 把 `StopAndWaitForAllSweep()` 中原先忽略的 `(void)TryPublishSweepDone()` 改为 `CHECK(TryPublishSweepDone())`，将返回 false 从可恢复状态变成进程 abort。
- 关键日志：cycle 264 中 parallel sweep 报 `remaining=1434`；GC 主线程随后 force-finish `516` 个 region，第二次 drain 为 `0`，紧接着触发 CHECK，没有打印 `StopAndWaitForAllSweep` 完成日志。

## 根因与修复

- 当前最可信根因：sweep handoff 的 admission 关闭与完成发布不是一个原子协议。`StopAndWaitForAllSweep()` 只做一次 gate/counter 快照等待；`TakeUnsweptRegion()` 在早期 gate 检查和 counter 增加之间存在延迟窗口，`BackgroundSweep()` 也可能先设置 `bgSweepRunning_` 再观察 stop gate。`TryPublishSweepDone()` 随后对多个独立状态做快照检查并直接 store `sweepDone_`，不能保证全局静默。
- 结论边界：本次 abort 的直接触发点是 `ae76037bcc` 的强制 CHECK；潜在竞态基础来自更早的多线程 sweep/完成发布设计，`e3e864ad09` 扩展了发布条件但没有封闭 admission race。
- 修复方式：使用一个原子 admission word：最高位关闭新 sweep 入场，低位记录所有已入场 sweep。GC 通过一次原子 RMW 关闭 admission，等待低位计数归零后再做最终 drain，并由 `PublishSweepDoneAfterQuiescence()` 唯一发布 `sweepDone_`。lazy/background/helper/assist worker 和 OOM force-finish 路径均不发布。
- OOM 语义：OOM force-finish 只清理当前分配所需的 sweep work，不代表正常 sweep epoch 完成，因此不设置 `sweepDone_`；同时清理 forced-HIGH，并在 `HasPendingSweepWork()` 为 false 时清理 P-local sweep debt。
- 影响范围：MSGC 并发 sweep 与下一轮 `PreEnumRoots()` 的交接；高并发分配/HTTP 场景更容易命中。

## 验证与回归

- 验证材料：`MSGC_RESULT/Result/case27_http_complex_json/result/20260720_165542_output_msgc_vs_copygc_go_c1000_p100K_compare/msgc/stdout.txt` 与同目录 `msgc/gc.log`。
- 当前验证结果：admission、incremental snapshot、dynamic helper、P-level debt、class-order、PostMark publication、IDLE-after-MarkTermination 契约测试通过；MarkSweep Debug 构建已编译通过 `MSCentralRegionPool.cpp`、`MSAllocBuffer.cpp` 和 MCollector 目标。完整 runtime 链接被现有 `src/arch/aarch64_macos/N2CStub.S` 的 CFI 错误阻断，尚未重新跑 benchmark。
- 回归风险：如果仅移除 `CHECK`，可能留下 `sweepDone_` 长时间为 false 或在新 sweep epoch 上错误发布，仍可能导致后续 GC/sweep 状态异常。
