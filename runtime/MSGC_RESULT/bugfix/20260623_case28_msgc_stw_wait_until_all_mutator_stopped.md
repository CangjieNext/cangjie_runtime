# case28 MSGC 在 `PreEnumRoots -> WaitUntilAllMutatorStopped` 卡住记录

## 基本信息

- 标题：case28 MSGC 侧首轮 GC 进入 STW 后无法完成 mutator stop，benchmark 超时不退出
- 日期：2026-06-23
- 平台：macOS aarch64
- 关联修复：`phase 3` gate / post-GC `IDLE` 回切已落地，本问题为后续端到端新 blocker

## 现象

- 用新的 MSGC SDK 跑 `case28_http_complex_json`：
  - `CASE28_PAYLOAD=10K`
  - `CASE28_CONCURRENCY=8`
  - `CASE28_DURATION_MS=60000`
  - `HEAP_SIZE=12gb`
  - `MSGC_HEAP_GROWTH=1`
  - `MSGC_SCAVENGE_GROWTH=10`
- `CopyGC` 正常完成，结果：
  - `throughput=4794 req/s`
  - `total=288264 ok=288264 fail=0`
- `MSGC` 侧没有打印任何 `[PROGRESS]` / `[RESULT]`，在 60s 固定时长下运行超过 2 分钟仍未退出，最终手工 `kill`。
- compare report 将 `MSGC` 统计为：
  - `throughput=0`
  - `total=0`
  - `GC cycles=0`

## 关键证据

### 1. `stdout.txt`

- 只走到：
  - server ready
  - payload loaded
  - warmup done
  - `Started 8 workers in sync mode; running for 60000ms...`
- 之后仅出现一条：
  - `HttpEngineConn1#readRequest exception: ConnectionException: Socket is closed.`
- 没有任何 2s 周期的 `[PROGRESS]` 行

### 2. `gc.log`

- 首轮 GC 在 `2026-06-23 00:32:52` 触发：
  - `=== GC START cycle=1 ===`
  - `--- PreEnumRoots ---`
  - `ForceFinishAllUnswept...`
  - `StopAndWaitForAllSweep...`
- 日志最后停在：
  - `feed done: slices=2 buffers=9 classes=44 regions=44 elapsed=445us`
- 没有出现本轮 GC 完成日志，也没有后续 cycle

### 3. `sample`

采样命令：

```bash
sample <pid> 3 1
```

主要栈签名：

- `gc-main-thread`
  - `MarkSweepCollector::RunGarbageCollection`
  - `MarkSweepCollector::PreEnumRoots`
  - `MutatorManager::StopTheWorld`
  - `MutatorManager::WaitUntilAllMutatorStopped`

- 多个 mutator / schd-worker / main-thread
  - `CJ_MCC_HandleSafepoint`
  - `MapleRuntime::Mutator::HandleSuspensionRequest`
  - `std::condition_variable::wait`

- 一个 HTTP worker 的阻塞点更具体：
  - `HttpServer::process`
  - `HttpEngineConn1::readRequest`
  - `BufferedReader::fill`
  - `DopraTcpSocketImpl::readImpl`
  - `.L_copy_end`
  - `MRT_LeaveSaferegion`
  - `Mutator::HandleSuspensionRequest`

## 当前判断

- 这轮问题已经不是 AArch64 phase gate 把 `phase 3` 错送 slow path。
- 也不是 SDK / stdx / dylib 混用问题：
  - `run_compare.sh` 用 `env -i` 隔离环境
  - `CopyGC` 同脚本同参数可稳定跑完
  - MSGC 侧确实加载了新 rebuild 的 runtime
- 当前更像是：
  - MSGC 首轮 GC 在 `PreEnumRoots()` 做停世界时，
  - 某些 mutator 已进入 safepoint/saferegion 相关等待，
  - 但 `WaitUntilAllMutatorStopped()` 仍认为“并未全部停止”，从而一直自旋等待

## 初步收敛方向

优先检查以下路径的一致性：

1. `MutatorManager::StopTheWorld()` / `WaitUntilAllMutatorStopped()`
2. `Mutator::EnterSaferegion()` / `LeaveSaferegion()` / `HandleSuspensionRequest()`
3. 阻塞 I/O 路径的 saferegion 包装：
   - `DopraTcpSocketImpl::readImpl`
   - `.L_copy_end -> MRT_LeaveSaferegion`
4. MSGC 在 `PreEnumRoots()` 之前是否已切换到需要所有 mutator ack 的 phase，但某类线程没有正确记账

## 结论

- `phase 3` / post-GC `IDLE` 问题已经完成代码修复，但还没有把 case28 拉回“可正常结束并可比较吞吐”的状态。
- 当前 case28 的主 blocker 是：
  - `MarkSweepCollector::PreEnumRoots()`
  - `MutatorManager::WaitUntilAllMutatorStopped()`
  - 与 mutator safepoint/saferegion 停止记账相关的 STW 卡住问题。
