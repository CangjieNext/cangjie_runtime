# Step 9：PreEnum 冻结 Sweep 快照复盘

> 2026-05-09 基于 case1_baseline + seed5 + marksweep 的实现与验证记录
>
> 平台：macOS aarch64
>
> 目标：从 `PreEnumRoots()` 开始冻结本轮 sweep snapshot，使 mutator 在 PreEnum 之后归还到 central pool 的 region 不参与本轮 sweep。

---

## 目录

1. [问题背景](#1-问题背景)
2. [本次实现改动](#2-本次实现改动)
3. [验证过程](#3-验证过程)
4. [性能结果](#4-性能结果)
5. [结论与影响](#5-结论与影响)
6. [后续优化方向](#6-后续优化方向)

---

## 1. 问题背景

原先的 sweep snapshot 在 `MarkTermination()` 期间通过 `PrepareForSweep()` 冻结。这样会产生一个语义窗口：

1. `PreEnumRoots()` 已经开始本轮 GC。
2. mutator 仍可能把 thread-local region 归还到 central pool。
3. 如果这些 region 在 `PrepareForSweep()` 前进入 central pool，则可能进入本轮 sweep snapshot。

本次需求是：从 `PreEnumRoots()` 开始就确定“后续归还的 region 不参加本轮 sweep”。

这相当于把 sweep generation flip 提前到 PreEnum 阶段，使本轮 sweep 只处理 PreEnum 时刻已经在 central pool 中的 swept lists。

---

## 2. 本次实现改动

### 2.1 MarkSweepCollector：PrepareForSweep 前移

修改文件：

- `src/Heap/MCollector/MarkSweepCollector.cpp`

关键变化：

- `PreEnumRoots()` 中执行：
  - `StopAndWaitForAllSweep()`
  - `ClearAllMarkBitmaps()`
  - `ReclassifyUnsweptToSwept()`
  - `PrepareForSweep()`
- `MarkTermination()` 中不再执行 `PrepareForSweep()`。
- `MarkTermination()` 在进入 `GC_PHASE_MS_SWEEP` 后调用 `ReenableLazySweep()`。

语义变化：

```text
旧流程：
PreEnumRoots()
  stop sweep / clear bitmap / reclassify
MarkTermination()
  FlushAllAllocBuffers()
  PrepareForSweep()
  TransitionToGCPhase(MS_SWEEP)

新流程：
PreEnumRoots()
  stop sweep / clear bitmap / reclassify
  PrepareForSweep()              # 提前冻结本轮 sweep snapshot
MarkTermination()
  FlushAllAllocBuffers()
  TransitionToGCPhase(MS_SWEEP)
  ReenableLazySweep()
```

这样，PreEnum 之后 mutator 归还的 region 会被 stamp 到新的 sweep generation，不再被本轮 sweep 处理。

### 2.2 去掉热路径诊断统计

修改文件：

- `src/CompilerCalls.cpp`

删除内容：

- `FastPathDiag#N` 首 5 次打印。
- `FastPathGate` phase/no-buffer/no-region 等原子统计。
- `AllocStats` small/large/array 原子计数。

保留内容：

- `MCC_PrintAllocStats()` 符号保留为空实现，避免潜在链接或 ABI 影响。

原因：

debug/O0 验证时发现诊断统计严重污染性能数据。case1 中 runtime 小对象分配可达 1.78 亿次，热路径上的 TLS/sizeclass/regionType 判断和原子计数会显著放大耗时。

### 2.3 C++14 constexpr 链接补丁

修改文件：

- `src/Heap/MCollector/MarkSweepCollector.cpp`
- `src/Heap/MAllocator/MSAllocBuffer.cpp`

补充：

```cpp
constexpr size_t MarkSweepCollector::kAssistMaxObjectsPerCall;
constexpr uint8_t AllocBufferMS::kCachedRegionSlots;
```

原因：

debug/O0 构建时暴露 C++14 下 `static constexpr` 被 ODR-use 后需要 out-of-line definition 的链接问题。release/O2 下通常会被优化掉，但补充定义更稳。

---

## 3. 验证过程

### 3.1 debug/O0 验证中的误判

第一次用 debug/O0 runtime 验证时，case1 seed5 从 origin 的约 5.3s 退化到约 25.4s。

后续确认这不能直接归因于 PreEnum 改动，主要混入了两个变量：

- debug/O0：构建参数为 `-O0 -gdwarf-4 -DMRT_DEBUG=1`。
- 热路径诊断：`FastPathGate phase_miss=178,794,434`，说明大量 runtime 分配路径还额外做了诊断统计。

因此后续重新编译 release/O2，并移除诊断统计，再做同条件对比。

### 3.2 release/O2 单次对比

构建命令：

```bash
python3 build.py build --target native --build-type release -v 0.0.1 --gc-mode marksweep
```

运行条件：

- SDK：`/Users/lipeisheng/code/cangjie/cangjie-pkg/self/dev260507/cangjie`
- heap：`cjHeapSize=256mb`
- processor：`cjProcessorNum=6`
- seed：`seed5`
- case：`MSGC_RESULT/Result/case1_baseline/benchmark.cj`
- 每次替换 runtime 后均重新编译 `.cj`

结果目录：

- 新 runtime：[20260509_000749_preenum_snapshot_release_o2_no_diag_seed5_marksweep](../Result/case1_baseline/result/20260509_000749_preenum_snapshot_release_o2_no_diag_seed5_marksweep)
- origin：[20260509_000912_origin_seed5_compare_release_o2_marksweep](../Result/case1_baseline/result/20260509_000912_origin_seed5_compare_release_o2_marksweep)

单次结果：

| runtime | Functional | wall time | monitor time | RSS avg/peak | CPU avg/peak |
|---|---:|---:|---:|---:|---:|
| PreEnum snapshot release/O2 no-diagnostics | 5/5 | 5665 ms | 5692 ms | 26.19 / 47.11 MB | 106.70% / 133.00% |
| origin | 5/5 | 5284 ms | 5503 ms | 25.67 / 43.98 MB | 106.61% / 133.30% |

### 3.3 release/O2 五次稳定性验证

结果目录：

- 新 runtime 五跑：[20260509_001901_preenum_release_o2_no_diag_seed5_5runs_stability](../Result/case1_baseline/result/20260509_001901_preenum_release_o2_no_diag_seed5_5runs_stability)
- origin 五跑：[20260509_001516_origin_seed5_5runs_stability](../Result/case1_baseline/result/20260509_001516_origin_seed5_5runs_stability)

新 runtime 五跑结果：

| Run | 状态 | Functional | wall time | P1 | P2 | P3 | P4 | P5 | RSS avg/peak |
|---:|---|---:|---:|---:|---:|---:|---:|---:|---:|
| 1 | PASS | 5/5 | 5864 ms | 282 | 976 | 597 | 511 | 896 | 30.11 / 48.30 MB |
| 2 | PASS | 5/5 | 5830 ms | 269 | 967 | 580 | 502 | 846 | 25.81 / 48.72 MB |
| 3 | PASS | 5/5 | 5783 ms | 268 | 973 | 596 | 511 | 856 | 28.59 / 47.45 MB |
| 4 | PASS | 5/5 | 5762 ms | 267 | 958 | 588 | 508 | 864 | 29.61 / 45.30 MB |
| 5 | PASS | 5/5 | 5867 ms | 279 | 990 | 587 | 515 | 856 | 28.40 / 48.75 MB |

新 runtime 汇总：

- 通过率：5/5
- wall time 平均值：5821.2 ms
- wall time min/max：5762 / 5867 ms
- wall time 标准差：47.4 ms

origin 五跑结果：

| Run | 状态 | Functional | wall time | P1 | P2 | P3 | P4 | P5 | RSS avg/peak |
|---:|---|---:|---:|---:|---:|---:|---:|---:|---:|
| 1 | PASS | 5/5 | 5336 ms | 266 | 877 | 540 | 477 | 769 | 25.74 / 48.44 MB |
| 2 | PASS | 5/5 | 5312 ms | 259 | 889 | 541 | 472 | 771 | 28.34 / 51.03 MB |
| 3 | PASS | 5/5 | 5349 ms | 264 | 931 | 536 | 482 | 768 | 27.80 / 44.23 MB |
| 4 | PASS | 5/5 | 5275 ms | 258 | 871 | 536 | 478 | 778 | 28.37 / 49.56 MB |
| 5 | PASS | 5/5 | 5257 ms | 262 | 885 | 533 | 472 | 773 | 26.58 / 47.69 MB |

origin 汇总：

- 通过率：5/5
- wall time 平均值：5305.8 ms
- wall time min/max：5257 / 5349 ms
- wall time 标准差：39.2 ms

---

## 4. 性能结果

五跑均值对比：

| 指标 | origin | PreEnum snapshot | 差值 |
|---|---:|---:|---:|
| wall time | 5305.8 ms | 5821.2 ms | +515.4 ms (+9.7%) |
| P1_SmallAlloc | 261.8 ms | 273.0 ms | +11.2 ms |
| P2_LargeAlloc | 890.6 ms | 972.8 ms | +82.2 ms |
| P3_MixedWorkload | 537.2 ms | 589.6 ms | +52.4 ms |
| P4_LongLivedChurn | 476.2 ms | 509.4 ms | +33.2 ms |
| P5_Fragmentation | 771.8 ms | 863.6 ms | +91.8 ms |

观察：

1. 劣化稳定存在，不是单次抖动。
2. 劣化主要集中在 P2、P3、P5。
3. 这些阶段都更依赖 region 周转、large allocation、fragmentation 与 refill/sweep 协同。
4. 五次都没有出现新的 abort、FAIL 或 UE。

---

## 5. 结论与影响

### 5.1 正确性收益

新流程达到了目标语义：

- 从 `PreEnumRoots()` 开始冻结本轮 sweep snapshot。
- PreEnum 之后 mutator 归还到 central pool 的 region 不进入本轮 sweep。
- lazy sweep 在 mark bitmap 尚未最终稳定前保持关闭，直到 `MarkTermination()` 后进入 `MS_SWEEP` 再打开。

这降低了“本轮标记期间新归还 region 被本轮 sweep 消费”的风险。

### 5.2 性能代价

代价是本轮内 region 复用及时性下降：

```text
PreEnum 提前 PrepareForSweep
  -> sweep generation 提前翻转
  -> PreEnum 后归还的 region 进入下一代 sweep
  -> 本轮 sweep 可处理的 ready/unswept supply 变少
  -> mutator 更容易走 fresh / refill / 后台补货压力路径
  -> P2/P3/P5 这类 region 周转敏感阶段变慢
```

因此，这次改动本质上是在“snapshot 隔离性”与“本轮回收及时性”之间做取舍。

在 case1 + seed5 上，这个取舍的可观测成本约为：

- wall time +9.7%
- RSS 峰值大致同量级，没有出现灾难性上涨
- 稳定性五跑通过

### 5.3 debug/O0 数据不应作为性能结论

debug/O0 + 诊断统计曾把 case1 拉到约 25s，但那次主要是测量污染：

- O0 关闭优化
- `MRT_DEBUG=1`
- 热路径原子统计和 TLS/region 检查

最终性能结论以 release/O2 no-diagnostics 五跑为准。

---

## 6. 后续优化方向

### 6.1 保留语义，降低 supply 损失

当前实现通过全局提前 `PrepareForSweep()` 达成隔离语义，但这会让 PreEnum 后归还的所有 region 都错过本轮 sweep。

后续可以探索更细粒度策略：

1. 不提前全局 flip，而是在 region 归还 central pool 时根据 GC epoch 打 next-cycle 标记。
2. 本轮 sweep 只处理 PreEnum snapshot 中已有 region。
3. PreEnum 后归还 region 可进入独立 next-cycle list，不污染本轮 list。

这样可以保留“不参与本轮 sweep”的语义，同时减少对 existing swept supply 的全局扰动。

### 6.2 MarkTermination 后补偿式 reclaim

如果必须保留当前提前 flip 策略，可考虑在 `MarkTermination()` 后做有限补偿：

- 只处理已确认安全、且属于 next generation 的 empty/fully-dead region。
- 严格限制 budget，避免重新引入并发 sweep stale edge 风险。
- 优先服务 P2/P5 这类大分配和碎片化压力。

### 6.3 增加供给侧计数

为了进一步定位 +9.7% 的来源，下一轮建议补低开销计数：

- PreEnum 后归还 central pool 的 region 数。
- 因 next generation 而跳过本轮 sweep 的 region 数。
- fresh region 分配次数。
- foreground refill / background fill 的 fresh 与 reusable 来源分布。

注意：计数必须避免重新污染热路径，优先用 per-thread/per-class 批量汇总或只在 GC phase 边界输出。

---

## 附：当前遗留状态

- SDK 在验证结束后已恢复到 `.origin`。
- 历史 debug/O0 run2 留下 `PID 5069 UE`，`kill -9` 无法清理；本次 release/O2 五跑没有新增 UE 残留。
- 当前源码仍包含本 Step 的实现改动，尚未提交。
