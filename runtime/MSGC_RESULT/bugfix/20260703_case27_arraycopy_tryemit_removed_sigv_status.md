# case27 ArrayCopy fastpath 从 `tryEmitCangjieSpecificCallByMOSym` 移除后的 SIGV 状态

## 变更定义

本条记录对应的是一组**实验配置**，不是当前工作区的长期状态。

实验改动为：

- 在 LLVM `AsmPrinter::tryEmitCangjieSpecificCallByMOSym()` 中，先把下面两个符号从 `emitMccWriteBarrierFastPath()` 的分发名单里移除：
  - `CJ_MCC_ArrayCopyRefFast_MSGC`
  - `CJ_MCC_ArrayCopyStructFast_MSGC`

这样做之后，这两个 `ArrayCopy` 路径不再走当前 AArch64 的 MSGC inline fastpath。

补充说明：

- 实验完成后，这两个名字由你加回了当前工作区。
- 本文档记录的是“移除后做验证”的结果，不代表当前源码仍处于移除状态。

## 验证前提

- 平台：`macOS aarch64`
- 用例：`MSGC_RESULT/Result/case27_http_complex_json`
- 配置：`payload=100K`，`concurrency=1000`，`duration=60s`，`cjHeapSize=4gb`
- SDK：`/Users/lipeisheng/code/cangjie/cangjie/cangjie_compiler/output`
- 验证方式：
  - 用 fresh output SDK 重新编译 `benchmark.cj`
  - 运行新编译出的二进制
  - 观察 `rc`、`stderr`、`[RESULT]` 与请求级 `fail`

## 验证结果

### 1. 先做的单轮样本并不稳定

目录：

- `MSGC_RESULT/Result/case27_http_complex_json/result/20260703_230130_case27_c1000_p100K_60s_heap4gb_1run_arraycopy_tryemit_removed_rebuilt_sdk`

结果：

- `rc=137`
- `gc_starts=0`
- `result_lines=0`

说明：

- 仅凭这一轮，**不能**直接下结论说“移除后已经完全修好”。

### 2. 后续 3 轮批量验证未再复现进程级崩溃

目录：

- `MSGC_RESULT/Result/case27_http_complex_json/result/20260703_231615_case27_c1000_p100K_60s_heap4gb_3runs_arraycopy_tryemit_removed`

3 轮结果：

1. `run1`: `rc=0`，`fail=1`，`throughput=522 req/s`
2. `run2`: `rc=0`，`fail=1`，`throughput=524 req/s`
3. `run3`: `rc=0`，`fail=1`，`throughput=539 req/s`

共同特征：

- 都执行到 `[DONE]`
- `stderr_lines=0`
- 未观察到 `SIGSEGV` / `139`
- 但每轮仍有少量请求级 `fail`

### 3. 后续 5 轮批量验证同样未再复现 SIGV

目录：

- `MSGC_RESULT/Result/case27_http_complex_json/result/20260703_232719_case27_c1000_p100K_60s_heap4gb_5runs_arraycopy_tryemit_removed`

5 轮结果：

1. `run1`: `rc=0`，`fail=51`，`throughput=521 req/s`
2. `run2`: `rc=0`，`fail=0`，`throughput=529 req/s`
3. `run3`: `rc=0`，`fail=0`，`throughput=522 req/s`
4. `run4`: `rc=0`，`fail=1`，`throughput=524 req/s`
5. `run5`: `rc=0`，`fail=0`，`throughput=523 req/s`

共同特征：

- 5/5 都是 `rc=0`
- `stderr_lines=0`
- 未复现 `SIGSEGV` / `SIGBUS` / 进程级 crash
- 但请求级 `fail` 仍然存在波动，尤其 `run1 fail=51`

## 当前可收敛结论

基于这组结果，当前可以谨慎收敛为：

1. 把 `CJ_MCC_ArrayCopyRefFast_MSGC` 与 `CJ_MCC_ArrayCopyStructFast_MSGC` 从 `tryEmitCangjieSpecificCallByMOSym()` 的 fastpath 分发里移除后，**此前 case27 中反复出现的 SIGV / 进程级崩溃症状，在后续 3 轮 + 5 轮重复验证中都没有再复现**。
2. 这强烈说明：**case27 当前这类 SIGV 现象，与 `ArrayCopyRef/ArrayCopyStruct` 走 AArch64 MSGC inline fastpath 有明显相关性。**
3. 但这还**不能**表述成“问题完全修复”，因为：
   - 该实验族里更早还有过 `rc=137` 的单轮样本
   - 即使后续 8 轮都没有进程级 crash，请求级 `fail` 仍未清零

## 建议口径

> 当前实验结果表明：在 `tryEmitCangjieSpecificCallByMOSym()` 中移除 `CJ_MCC_ArrayCopyRefFast_MSGC` 和 `CJ_MCC_ArrayCopyStructFast_MSGC` 后，case27 `c1000 + 100K + 60s + heap=4gb` 的后续 3 轮和 5 轮重复验证均未再复现 SIGV / 进程级崩溃。这说明当前崩溃现象与 `ArrayCopy` 的 MSGC inline fastpath 高度相关。但请求级 `fail` 仍存在，因此这一定义更准确地说是“显著压住了 SIGV 症状”，还不能直接等价成“问题完全修复”。
