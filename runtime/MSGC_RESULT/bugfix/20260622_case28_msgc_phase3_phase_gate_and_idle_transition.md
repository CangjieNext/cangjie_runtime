# case28 MSGC `phase 3` 误判为 barrier slow path 与 GC 结束后未回切 `IDLE` 记录

## 基本信息

- 标题：case28 `GC_PHASE_RECLAIM_SATB_NODE(3)` phase gate 误判与 MSGC post-GC phase 停留问题
- 日期：2026-06-22
- 提交/分支：working tree（未提交）
- 平台：macOS aarch64

## 现象与复现

- 现象：
  - `case28` 在 MSGC 下吞吐没有随 `gcwrite/gcread generic` 的前置优化同步提升。
  - 深度采样显示绝大多数 write-time 落在 `phase 3`，并反复进入 `CJ_MCC_WriteStructField` / `CJ_MCC_WriteGeneric` slow path。
  - Instruments 采样与 slow-path 诊断都指向：AArch64 MSGC write fastpath wrapper 没有把 `GC_PHASE_RECLAIM_SATB_NODE(3)` 当作无屏障阶段。
- 最小复现步骤：
  1. 用 MSGC SDK 编译并运行 `MSGC_RESULT/Result/case28_http_complex_json/benchmark.cj`
  2. 打开 write barrier slow-path 诊断/采样
  3. 观察 `stdout.txt` / sample 输出中 `phase 3` 与 `CJ_MCC_WriteStructField` 的占比
- 实际结果：
  - `phase 3` 被 AArch64 wrapper 送入 `Slow`
  - MSGC runtime 在 `PostGarbageCollection()` 后进入 `GC_PHASE_RECLAIM_SATB_NODE(3)` 做 SATB/PagePool 收尾，但没有在收尾后显式回切 `IDLE`
  - `HeapImpl::InstallBarrier()` 在 MS 模式下也没有把 `phase 3` 映射到 `msIdleBarrier`
- 期望结果：
  - 仅 `MS_MARK_SETUP(20)` / `MS_CONCURRENT_MARK(21)` / `MS_MARK_TERMINATION(22)` 使用 MSGC 写屏障
  - `phase 3` 以及其他非 mark phase 应视作 `StoreOnly` / idle barrier
  - GC 收尾完成后 runtime 应重新回到 `GC_PHASE_IDLE`

## 定位过程

- 已排除项：
  - `gcread_generic` 不是当前主热点
  - `array GCTib` 长短格式不是这次 `phase 3` 慢路径占比异常的主因
  - 真实瓶颈不在 old-edge barrier 语义本身，而在“明明不需要 barrier 仍反复走 helper/slow path”
- 关键日志：
  - `case28` 采样中 `phase 3` 期间 write-time 占比接近全部
  - `CJ_MCC_WriteGeneric` / `CJ_MCC_WriteStructField` slow path 高占比
- 关键函数/调用链：
  - AArch64 write wrapper phase gate：
    - `llvm/lib/Target/AArch64/AArch64AsmPrinter.cpp`
  - MSGC runtime phase 定义：
    - `src/Heap/Collector/Collector.h`
  - MSGC barrier 安装：
    - `src/Heap/Heap.cpp`
  - post-GC 收尾：
    - `src/Heap/Collector/TracingCollector.cpp`
    - `src/Heap/MCollector/MarkSweepCollector.cpp`

## 根因与修复

### 问题 1：LLVM AArch64 MSGC write wrapper 的 phase gate 写窄了

- 根因：
  - 当前 gate 只把 `IDLE(1)` 与 `MS_SWEEP(23)` 当作 `StoreOnly`
  - 只把 `20..22` 当作 mark 区间
  - 所有其他 phase（包含 `GC_PHASE_RECLAIM_SATB_NODE(3)`）都会落到 `Slow`
  - 这与 runtime 总 phase 约定不一致：`GC_PHASE_INIT(8)` 之前的 phase 不需要 barrier
- 修复方式：代码
  - 把 AArch64 MSGC write fastpath 的 phase gate 改成：
    - `phase == 0` -> `Slow`（保守）
    - `phase in {20, 21, 22}` -> barrier path
    - 其他非零 phase -> `StoreOnly`
- 影响范围：
  - `CJ_MCC_WriteRefFieldFast_MSGC`
  - `CJ_MCC_WriteStaticRefFast_MSGC`
  - `CJ_MCC_WriteStructFieldFast_MSGC`
  - `CJ_MCC_WriteStaticStructFast_MSGC`
  - `CJ_MCC_ArrayCopyRefFast_MSGC`
  - 间接覆盖 `gcwrite.generic -> WriteStructFieldFast_MSGC`

### 问题 2：MSGC post-GC cleanup 进入 `phase 3` 后未显式回切 `IDLE`

- 根因：
  - `MarkSweepCollector::PostGarbageCollection()` 复用了 `TracingCollector::PostGarbageCollection()`
  - 基类会先切到 `GC_PHASE_RECLAIM_SATB_NODE(3)` 再执行 `SatbBuffer::ReclaimALLPages()` 与 `PagePool::Trim()`
  - 但 cleanup 结束后没有统一回切 `GC_PHASE_IDLE`
  - 同时 `HeapImpl::InstallBarrier()` 在 MS 模式下也未把 `phase 3` 映射到 `msIdleBarrier`
- 修复方式：代码
  - cleanup 完成后显式 `TransitionToGCPhase(GC_PHASE_IDLE, true)`
  - MS 模式下把 `GC_PHASE_RECLAIM_SATB_NODE(3)` 视作 idle barrier
- 影响范围：
  - MSGC GC 结束后的 barrier 选择
  - cleanup 窗口中的 write/struct-copy 路径
  - 可能连带改善 GC 结束后短窗口内的其他 fastpath 可达性

## 验证与回归

- 验证命令：
  - LLVM：
    - `llc ... cangjie-writebarrier-msgc-fastpath.ll | FileCheck ...`
    - `msgc-gcwrite-generic-lowering.ll` 仍有独立的 `CHECK-NOT` 误报，未作为本次 gate 修复阻塞项
  - 端到端：
    - 按 `AGENTS.md` 增量重编 `compiler -> runtime -> stdlib -> stdx`
    - 再跑 `MSGC_RESULT/Result/case28_http_complex_json/run_compare.sh`
- 验证结果：
  - 已完成代码落地：
    - LLVM AArch64 gate 仅保留 `20..22` 走 mark barrier，其他非零 phase 走 `StoreOnly`
    - runtime 在 `TracingCollector::PostGarbageCollection()` 收尾后显式回切 `GC_PHASE_IDLE`
    - `HeapImpl::InstallBarrier()` 将 `GC_PHASE_RECLAIM_SATB_NODE(3)` / `GC_PHASE_MS_SWEEP(23)` 映射到 idle barrier
  - 已完成构建链路：
    - `compiler` 增量重编并 install
    - `runtime` 用 clang16 重编并 install 到 `runtime/output` 与 `compiler/output`
    - `stdlib` / `stdx` 重新编译安装
  - LLVM 结果：
    - `cangjie-writebarrier-msgc-fastpath.ll` 已按新 gate 通过 FileCheck
  - case28 结果：
    - `CopyGC` 正常完成，`throughput=4794 req/s`
    - `MSGC` 不再出现“phase 3 helper 慢路径吞吐验证”这个旧问题作为主现象，但端到端仍未完成：在首轮 GC 的 `PreEnumRoots()` 进入 `StopTheWorld -> WaitUntilAllMutatorStopped()` 后卡住，`CASE28_DURATION_MS=60000` 超时两分钟仍未退出，最终人工终止
    - 结果目录：
      `MSGC_RESULT/Result/case28_http_complex_json/result/20260623_003140_llvm_phase_gate_runtime_idle_fix_c8_p10K_compare/`
  - 结论：
    - `phase 3` / `IDLE` gate 与 phase 回切问题已完成修复并集成到新 SDK
    - case28 当前剩余 blocker 已切换为新的 MSGC STW 停世界卡住问题，见同目录 2026-06-23 新问题单
- 回归风险：
  - 若 phase gate 放宽过头，可能把真正需要 barrier 的 mark phase 误判成 `StoreOnly`
  - 若只修 LLVM 不修 runtime phase 回切，则其他依赖 `encodedGCPhase` 的 fastpath 仍可能在 post-GC 窗口表现异常
