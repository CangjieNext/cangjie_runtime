# Step56：MSGC P1/P5/P50/P100/P150 Mark、Sweep、Scavenge 速率对比分析

## 0. 结论摘要

本文现包含两组 runtime 数据：新补跑的 P1/P5 使用当前 runtime
`d326934e4672aa5a77d11b4a90f8e71b`，原 P50/P100/P150 使用历史 runtime
`c5adedd63ec68d4ea97cca267bf7cab0`。因此，P1↔P5 可以严格横向比较；P50/P100/P150
之间也可比较；两个 runtime 组之间只能作为方向性参考。结论如下：

1. **当前 runtime 下，P5 的 Mark 明显快于 P1。** P1、P5 总速率分别为 **17.241、40.620 Mobj/s**，P5 是 P1 的 **2.36x**；仅看 `reason=natural` 周期也分别为 **17.484、42.494 Mobj/s**，结论不依赖 `debt-stalled`。
2. **P1 是单 Processor 容量受限基线，不是“更低 P 就更快”的基线。** P1 没有 Helper，成功吞吐仅 **183.89 req/s**、失败率 **10.44%**；P5 有 1 个 Helper，成功吞吐 **801.80 req/s**、失败率为 0。
3. **当前 P5 的 Main+Helper 扫描能力扩展有效。** P1 Main 为 **17.163 Mobj/s**；P5 Main+Helper 为 **31.680 Mobj/s**，是 P1 的 **1.85x**。P5 另有 **22.01%** 的工作由 Assist 完成。
4. **当前 P1/P5 没有 OOM Sweep，Sweep closure 也很小。** P1/P5 分别只有 **2,463/43 个 Region** 留到 closure，全部 `stopSweep` 累计仅 **9.932/12.767 ms**。P1 因没有 Helper 而没有后台并行 Sweep；P5 后台 Sweep 为 **37.329 GB/s**。
5. **当前 P1/P5 的 Scavenge 活跃速率接近。** 两者分别为 **13.229、12.335 GB/s**，`releaseFailures` 都是 0；P1 略快约 7%，不足以说明 P 增大必然导致 Scavenge 退化。
6. **历史高 P 组仍表现出 Mark、Sweep、Scavenge 压力。** P50/P100/P150 的 Mark 为 **12.683、11.390、11.158 Mobj/s**，P50/P100 分别有 **197,326、232,069 个 Region** 留到 closure，Scavenge 仅 **0.441、0.764、1.080 GB/s**。但这些数值不能与当前 P5 做严格版本内回归。
7. **要确认当前 runtime 的高 P 改善幅度，必须继续用当前 dylib 重跑 P50/P100/P150。** 在此之前，不能把当前 P5 与历史高 P 的差值全部归因于 P/shard/工作集变化。

因此，本次新增数据支持的严格判断是：**同一当前 runtime 下，P5 比 P1 更能利用 GC Main+Helper 和业务 Processor，Mark 与请求吞吐均显著更高，同时 Sweep/Scavenge 仍然健康。** 历史高 P 组显示的大工作集、Sweep backlog 和 Scavenge 低速仍值得关注，但需要同版本复跑才能量化当前回归。

## 1. 测试目标与可比性

### 1.1 测试目标

在暂不讨论 STW 本身优化的前提下，对比 P1、P5、P50、P100、P150 时：

- Concurrent Mark 的总速率，以及 GC Main、Helper、Mutator Assist 各自贡献；
- 后台并行 Sweep、OOM 应急 Sweep、下轮 GC 前 Sweep closure 的速度和积压；
- Scavenge 将空闲物理页归还 OS 的速率；
- GC 行为对请求吞吐、失败率、CPU、RSS 的影响。

### 1.2 固定参数

| 参数 | 值 |
| --- | --- |
| 用例 | `case27_http_complex_json_msgc_high_p` |
| payload | `100K` |
| 客户端并发 | `1000` |
| 测试时长 | `60000 ms` |
| 最大虚拟堆 | `16gb` |
| `cjHeapGrowth` | `1` |
| `cjScavengeGrowth` | `5` |
| `cjMSMinGCThreshold` | `20mb` |
| `cjGCThreads` | 未显式设置，使用 runtime 自动策略 |
| 编译 GC 模式 | `cjc --gc-mode=marksweep` |
| 机器逻辑 CPU | `8` |

每档使用环境变量调用同一个入口，例如：

```bash
PROCS=1 TAG=step56_current_runtime \
  bash MSGC_RESULT/Result/case27_http_complex_json_msgc_high_p/run_compare.sh
```

### 1.3 SDK 三件套和 runtime 身份

- compiler：`cangjie_compiler/output/msgc/bin/cjc`
- runtime：`cangjie_compiler/output/msgc/runtime/lib/darwin_aarch64_cjnative/libcangjie-runtime.dylib`
- stdlib/stdx：`cangjie_compiler/output/msgc/darwin_aarch64_cjnative/dynamic/stdx`
- 当前 P1/P5 runtime MD5：`d326934e4672aa5a77d11b4a90f8e71b`
- 历史 P50/P100/P150 runtime MD5：`c5adedd63ec68d4ea97cca267bf7cab0`

五档 `compile.exitcode` 和 `run.exitcode` 均为 0，`dyld_runtime_path.txt` 均指向各自
`run_metadata.env` 记录的 MSGC runtime。脚本每轮都在隔离的 `env -i` 子 shell 中重新编译
`benchmark.cj`，不存在替换 runtime 后复用旧二进制的问题。

严格可比性分组如下：

| 分组 | P | runtime MD5 | 可比范围 |
| --- | --- | --- | --- |
| 当前 runtime | P1、P5 | `d326934e4672aa5a77d11b4a90f8e71b` | P1↔P5 严格可比 |
| 历史 runtime | P50、P100、P150 | `c5adedd63ec68d4ea97cca267bf7cab0` | 三档之间严格可比 |

跨分组比较同时包含 runtime 代码差异，只能说明“观察到的量级和方向”，不能作为纯 P 参数回归。

## 2. 指标定义与计算口径

### 2.1 Mark

日志提供每轮：

- `ConcurrentMark time`
- `[mark-workers] mainProcessed`
- `[mark-workers] processed`（Helper）
- `[assist] processed`（Mutator Assist）

本文使用：

```text
Mark 总速率 =
  Σ(mainProcessed + helperProcessed + assistProcessed)
  / Σ(ConcurrentMark wall time)
```

单位为 `Mobj/s`。这里的 object 是进入 GC Work、需要扫描引用字段的灰对象，不等同于“堆内所有对象”，也不是字节吞吐。

各参与者速率使用相同的 ConcurrentMark wall time 作分母，因此三者可相加为总速率。另单独统计 `reason=natural` 周期，避免 `debt-stalled` 提前收敛影响结论。

### 2.2 Sweep

Sweep 必须拆成三条路径：

1. **后台并行 Sweep**：从 `parallel-sweep start` 到对应 `parallel-sweep done`。
2. **OOM 应急 Sweep**：从 OOM 周期的 `Mark bitmap` 到 `OOM parallel sweep once` 完成日志。
3. **下轮 GC 前 closure**：`StopAndWaitForAllSweep: forceSwept=N->0` 与随后 `stopSweep=...`。

后台/closure 的逻辑扫描字节按 `Region 数 × 32768` 计算，因为 `MS_REGION_SIZE=32KB`。这反映 Region footprint 的处理速率，不是实际 DRAM 读带宽；空 Region、对象密度和 size class 不同都会改变单位 Region 的真实工作量。

OOM 路径日志没有独立的开始时间，本报告用紧邻调用前的 `Mark bitmap` 时间作为起点，所以这是包含调用及竞争的**观测 wall time**。它适合本轮相对比较，不应解读为纯 Sweep 内核微基准。

### 2.3 Scavenge

每个 `ScavengeSlice` 都记录 `releasedBytes`、`scanClaimUs`、`releaseUs`、`completeUs`。本文主口径为：

```text
Scavenge 活跃速率 =
  Σ(releasedBytes)
  / Σ(scanClaimUs + releaseUs + completeUs)
```

该口径排除了后台线程睡眠和两个 slice 之间的空档，衡量 FreeManager 真正执行查找、claim、OS release、完成 bookkeeping 时的效率。

`releasedBytes` 可大于最大堆，因为同一虚拟 Region 在不同 GC 周期中可以被重新分配、再次释放。

## 3. 五档结果总览

| 指标 | P1（当前） | P5（当前） | P50（历史） | P100（历史） | P150（历史） |
| --- | ---: | ---: | ---: | ---: | ---: |
| GC 周期数 | 695 | 730 | 22 | 23 | 21 |
| GC 总线程 / Helper | 1 / 0 | 2 / 1 | 4 / 3 | 4 / 3 | 4 / 3 |
| Mark 总速率 (Mobj/s) | **17.241** | **40.620** | **12.683** | **11.390** | **11.158** |
| Natural Mark 速率 (Mobj/s) | 17.484 | 42.494 | 12.683 | 11.532 | 13.223 |
| 后台 Sweep 速率 (GB/s) | 无 | 37.329 | 3.469 | 8.657 | 9.789 |
| OOM Sweep 观测速率 (GB/s) | 无 | 无 | 1.210 | 2.407 | 3.242 |
| Sweep closure 积压 Region | 2,463 | 43 | 197,326 | 232,069 | 42,234 |
| `stopSweep` 累计时间 (s) | 0.010 | 0.013 | 2.520 | 4.740 | 0.490 |
| Scavenge 活跃速率 (GB/s) | **13.229** | **12.335** | **0.441** | **0.764** | **1.080** |
| 成功吞吐 (req/s) | 183.89 | 801.80 | 207.12 | 184.15 | 200.95 |
| 请求失败率 | 10.44% | 0 | 27.02% | 25.60% | 22.41% |
| 平均 CPU | 132.90% | 457.88% | 307.26% | 297.22% | 310.11% |
| 平均 RSS (MB) | 573.38 | 1,289.02 | 3,554.63 | 3,330.74 | 4,181.19 |

同一当前 runtime 内，P5 相对 P1 同时提高了 Mark、业务吞吐和 Processor 利用率，且没有引入
OOM Sweep 或大 closure。历史 P50/P100/P150 仍不能只凭单个 Sweep/Scavenge GB/s 判断健康度：
三档请求完成量、OOM Sweep 次数和进入各路径的 Region 组成都不同。

## 4. Mark 速率分析

### 4.1 GC Main、Helper、Assist 分拆

| P | ConcurrentMark 总时间 (s) | Main 对象数 | Helper 对象数 | Assist 对象数 | 总对象数 | 总速率 (Mobj/s) |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 1（当前） | 9.192 | 157,769,191 | 0 | 718,848 | 158,488,039 | **17.241** |
| 5（当前） | 8.365 | 138,869,155 | 126,140,628 | 74,778,624 | 339,788,407 | **40.620** |
| 50 | 22.543 | 17,082,926 | 50,622,541 | 218,211,328 | 285,916,795 | **12.683** |
| 100 | 30.680 | 23,783,684 | 72,471,872 | 253,179,904 | 349,435,460 | **11.390** |
| 150 | 23.179 | 34,951,247 | 107,936,286 | 115,751,936 | 258,639,469 | **11.158** |

按参与者换算：

| P | Main (Mobj/s) | Helper (Mobj/s) | Assist (Mobj/s) | Main+Helper (Mobj/s) | Assist 占总工作 |
| ---: | ---: | ---: | ---: | ---: | ---: |
| 1（当前） | 17.163 | 0 | 0.078 | **17.163** | 0.45% |
| 5（当前） | 16.601 | 15.079 | 8.939 | **31.680** | 22.01% |
| 50 | 0.758 | 2.246 | 9.680 | **3.003** | 76.32% |
| 100 | 0.775 | 2.362 | 8.252 | **3.137** | 72.45% |
| 150 | 1.508 | 4.657 | 4.994 | **6.164** | 44.75% |

关键点：

- 当前 P1 没有 Helper，Main 单独达到 **17.163 Mobj/s**。
- 当前 P5 的 Main+Helper 达到 **31.680 Mobj/s**，是同 runtime P1 Main 的 **1.85x**，说明从 1 个 GC Main 扩到 Main+1 Helper 获得了有效增益，但不是理想 2x。
- P5 Assist 占 **22.01%**，仍低于 30% 建议线；P1 Assist 仅占 **0.45%**。
- 历史 P50/P100 即使有 3 个 Helper，Main+Helper 合计也只有约 **3 Mobj/s**，总 Mark 主要由 Mutator Assist 支撑。
- Assist 做掉 72%～76% 的 Mark 工作，会直接侵占业务线程时间；所以“总 Mark 仍有 11～13 Mobj/s”不能等同于后台 GC 性能健康。

历史 P150 的 Main+Helper 比 P50/P100 高，但总速率没有同步升高，因为 Assist 工作下降，而且本轮完成的总对象数和周期构成不同。单轮 P150 不能据此证明 Helper 已恢复线性扩展。

### 4.2 排除 `debt-stalled` 的影响

| P | Natural 周期 | Debt-stalled 周期 | Natural Mark 速率 (Mobj/s) | runtime 组 |
| ---: | ---: | ---: | ---: | --- |
| 1 | 610 | 85 | 17.484 | 当前 |
| 5 | 423 | 307 | 42.494 | 当前 |
| 50 | 22 | 0 | 12.683 | 历史 |
| 100 | 18 | 5 | 11.532 | 历史 |
| 150 | 15 | 6 | 13.223 | 历史 |

当前 P5 的 natural Mark 是同 runtime P1 的 **2.43x**；虽然 P5 有 307 个
`debt-stalled` 周期，总体 Mark 仍达到 40.620 Mobj/s，说明 P1↔P5 的结论不是提前收敛造成的。
历史 P50/P100/P150 排除 `debt-stalled` 后仍处在 11.5～13.2 Mobj/s，但不能直接除以当前
P5 得出纯 P 退化比例。

### 4.3 工作集随 P 放大

| P | 初始 committed heap | 平均 Mark 起始占用 | 平均最终 liveBytes | 平均标记对象数 | 平均字节/标记对象 |
| ---: | ---: | ---: | ---: | ---: | ---: |
| 1（当前） | 64 MiB | 0.396 GB | 0.173 GB | 0.443 M | 391.6 B |
| 5（当前） | 320 MiB | 0.724 GB | 0.257 GB | 1.240 M | 207.5 B |
| 50 | 3.125 GiB | 5.390 GB | 2.814 GB | 26.957 M | 104.4 B |
| 100 | 6.250 GiB | 5.722 GB | 3.647 GB | 34.866 M | 104.6 B |
| 150 | 9.375 GiB | 5.639 GB | 3.673 GB | 35.706 M | 102.9 B |

同一当前 runtime 内，P5 相对 P1：

- 平均 liveBytes 放大 **1.48x**；
- 平均标记对象数放大 **2.80x**；
- 平均字节/标记对象从 391.6 B 降到 207.5 B，对象图约密集 **1.89x**。

历史 P50/P100/P150 相对当前 P5 的工作集量级分别为：liveBytes **10.93x、14.17x、14.27x**，
标记对象数 **21.74x、28.12x、28.80x**，对象密度约 **1.98～2.02x**。这能解释旧高 P
数据的压力形态，但因为 runtime 不同，不能用这些比例精确归因 Mark 速率差。

## 5. Sweep 速率与 backlog 分析

### 5.1 后台并行 Sweep

| P | 后台 slice 数 | 逻辑处理量 (GB) | wall time (s) | 速率 (GB/s) | active Helper |
| ---: | ---: | ---: | ---: | ---: | ---: |
| 1（当前） | 0 | 0 | 0 | 无 | 0 |
| 5（当前） | 736 | 262.470 | 7.031 | **37.329** | 1 |
| 50 | 1 | 0.062 | 0.018 | **3.469** | 1 |
| 100 | 4 | 1.561 | 0.180 | **8.657** | 3 |
| 150 | 2 | 0.810 | 0.083 | **9.789** | 3 |

这里最重要的不是 P100/P150 单个 slice 的 GB/s 高于 P50，而是**后台 slice 的执行机会非常少**：

- 当前 P1 没有可用 Helper，因此没有后台并行 Sweep slice；
- 当前 P5 有 736 个后台 slice，持续消化了 262.470 GB 逻辑 Region footprint；
- P50/P100/P150 只有 1/4/2 个；
- 高 P 每轮产生大量 unswept Region，但主要由 lazy/assist/OOM/下轮 closure 等路径消化，后台处理没有形成持续稳定的流水线。

P5 的 37.329 GB/s 也不能视为真实内存带宽：其 Region 更小、更空，逻辑 Region footprint
除以时间会得到很高数值。不同 P 的对象密度不同，因此该指标主要用于判断同一路径有没有及时推进。

### 5.2 OOM 应急 Sweep

| P | OOM Sweep 次数 | 逻辑处理量 (GB) | 观测时间 (s) | 观测速率 (GB/s) | Helper |
| ---: | ---: | ---: | ---: | ---: | ---: |
| 1（当前） | 0 | 0 | 0 | 无 | - |
| 5（当前） | 0 | 0 | 0 | 无 | - |
| 50 | 2 | 11.330 | 9.365 | **1.210** | 3 |
| 100 | 9 | 48.363 | 20.093 | **2.407** | 3 |
| 150 | 11 | 51.674 | 15.937 | **3.242** | 3 |

高 P 下 OOM 应急 Sweep 成为主要路径。P100/P150 的观测速率高于 P50，说明 3 个 Helper 对大批量 Region claim 有作用，但健康度反而更差：

- OOM Sweep 次数从 P50 的 2 次增加到 P100/P150 的 9/11 次；
- 它发生在已经进入 OOM GC 的应急路径中；
- 更高的应急 Sweep GB/s 只能说明这批 Region 被更快处理，不能说明内存供应及时。

### 5.3 下一轮 GC 前 Sweep closure

`PreEnumRoots()` 在真正进入 STW 之前调用 `StopAndWaitForAllSweep()`：GC Main 先参与旧 sweep epoch 的 claim，关闭 background/lazy/assist/helper admission，等待所有已进入的 Sweep 退出，再串行收尾剩余 Region。该过程会延迟下一轮 GC 启动，虽然 `stopSweep` 本身不在 `ScopedStopTheWorld` 内。

| P | 有 backlog 的 closure 次数 | closure Region 总数 | 有 backlog 时长 (s) | 有效 closure 速率 (GB/s) | 全部 `stopSweep` (s) | 单次最大 |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 1（当前） | 6 | 2,463 | 0.003 | 31.185 | 0.010 | 0.954 ms |
| 5（当前） | 1 | 43 | 0.002 | 0.602 | 0.013 | 2.341 ms |
| 50 | 3 | 197,326 | 2.520 | 2.566 | 2.520 | 1.381 s |
| 100 | 4 | 232,069 | 4.650 | 1.635 | 4.740 | **2.579 s** |
| 150 | 1 | 42,234 | 0.473 | 2.928 | 0.490 | 472.694 ms |

这张表直接回答“并发 Sweep 是否阻塞释放或下一轮使用”：

- 当前 P1/P5 的 closure 都是毫秒级，P5 的 0.602 GB/s 由仅 43 个 Region 和固定开销计算而来，不能解读为 Sweep 内核变慢；
- 历史 P50/P100 确实有大批 unswept backlog 留到下一轮；
- closure 期间旧 sweep epoch 必须收敛，新的 Mark 不能开始；
- P100 最严重，4 次 closure 合计处理 232,069 个 Region，GC 启动前累计耗时 4.74 秒；
- P150 backlog 较少不代表机制上消失，它本轮有更多 OOM Sweep，提前消化了大量 Region。

因此，当前 P1/P5 没有出现 Sweep 供应失控；历史高 P 的核心问题不是 Helper 上限仍为 1：
HIGH 下已经用到 3 个 Helper，但**持续后台推进不足，工作转移到了 OOM 或下一轮同步 closure**。

## 6. Scavenge 速率分析

| P | Scavenge slice 数 | 累计 releasedBytes (GB) | 活跃时间 (s) | 活跃速率 (GB/s) | 仅 release 速率 (GB/s) | releaseFailures |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 1（当前） | 1,478 | 0.356 | 0.027 | **13.229** | 13.319 | 0 |
| 5（当前） | 842 | 0.829 | 0.067 | **12.335** | 12.419 | 0 |
| 50 | 1,453 | 25.146 | 57.034 | **0.441** | 0.443 | 0 |
| 100 | 422 | 51.141 | 66.896 | **0.764** | 0.768 | 0 |
| 150 | 675 | 46.644 | 43.170 | **1.080** | 1.085 | 0 |

同一当前 runtime 内，P1 只比 P5 快 **7.25%**，两者都处于 12 GB/s 以上。历史高 P
活跃速率明显更低，但不能与当前 P5 做严格版本内比例回归。

### 6.1 是否能正常释放

能。五档都满足：

- `releasedRegions == claimedRegions` 的累计关系正常；
- `releaseFailures=0`；
- 没有出现 `mmap fixed failed`、地址错误或 Scavenge 失败日志。

所以这是**成功但慢**，不是释放 API 失败。

### 6.2 为什么大堆下慢

本机是 macOS，`FreeManager::ReleaseUnits()` 的实现不是单个轻量 bookkeeping：

1. `MemorySet(addr, size, 0, size)` 清零；
2. 对同一地址执行 `MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED` 的 `mmap`；
3. 完成 FreeManager 中 candidate run 的 claim/complete 状态转换。

高 P 下每轮需要跨更多 shard/view/hot word 查找空闲 run，碎片会增加 `candidateRuns` 和 `releaseCalls`；同时大量实际内存需要清零并重新映射。数据也表明总耗时几乎全部在 `releaseUs`，不是 `scanClaimUs`。

P50 的 25.146 GB、P100 的 51.141 GB、P150 的 46.644 GB 是跨多个周期累计释放量，不代表进程同时持有这么多物理内存。它说明大堆压力下相同虚拟区域在“分配—回收—再次分配”之间反复震荡，Scavenge 正在消耗大量 CPU/内核时间。

## 7. 业务吞吐、CPU 与 RSS

| P | 总请求 | 成功 | 失败 | 成功吞吐 (req/s) | 失败率 | CPU 平均 / P95 | RSS 平均 / P95 / 最大 (MB) |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 1（当前） | 13,152 | 11,779 | 1,373 | **183.89** | 10.44% | 132.90% / 156.79% | 573 / 664 / 664 |
| 5（当前） | 49,136 | 49,136 | 0 | **801.80** | 0 | 457.88% / 513.85% | 1,289 / 1,558 / 1,558 |
| 50 | 17,958 | 13,105 | 4,853 | **207.12** | 27.02% | 307.26% / 429.10% | 3,555 / 5,177 / 5,572 |
| 100 | 18,157 | 13,509 | 4,648 | **184.15** | 25.60% | 297.22% / 431.60% | 3,331 / 5,444 / 6,194 |
| 150 | 16,215 | 12,582 | 3,633 | **200.95** | 22.41% | 310.11% / 467.70% | 4,181 / 6,513 / 6,925 |

当前 P1 只有一个业务 Processor，CPU、吞吐都显著低于 P5：

- P5 成功吞吐是 P1 的 **4.36x**，并把失败率从 10.44% 降到 0；
- P5 CPU 平均 457.88%，与 5 个 Processor 的可用并行度一致；P1 平均 132.90% 还包含 GC/后台线程，不能把它当作同吞吐下的“更省 CPU”；
- 历史 P50/P100/P150 的成功请求吞吐和完成量也不同，且 runtime 版本不同；
- P50/P100 的 Mark 主要由 Assist 完成，业务线程被 GC 工作占用；
- Sweep closure 存在长等待，Scavenge 的 `mmap` 路径也可能消耗内核时间；
- 指针追踪和 mark bitmap 更新可能受内存延迟而不是纯计算吞吐限制。

因此，当前 P1 是 Processor 容量不足；历史高 P 则是“完成更少工作、等待和 GC 协作更多”。
这两种低吞吐现象的机制不同，不能只看 CPU 百分比合并解释。

## 8. 压力判断与 Helper 启用是否生效

### 8.1 Mark 压力

| P | start level 1 | level 2 | level 3 | level 4 | 周期内最高 level | 最大 Helper |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 1（当前） | 5 | 26 | 26 | 638 | 4 | 0 |
| 5（当前） | 5 | 1 | 3 | 721 | 4 | 1 |
| 50 | 5 | 1 | 0 | 16 | 4 | 3 |
| 100 | 6 | 1 | 0 | 16 | 4 | 3 |
| 150 | 5 | 1 | 0 | 15 | 4 | 3 |

当前 runtime 的 Mark 压力策略以 WorkBuf backlog 为基础：

- 每 10 个 backlog buffer 增加一级；
- 每完成 10 个 buffer 做一次 checkpoint；
- backlog 不下降时单调提高 pressure；
- pressure 同时决定 Helper 目标数和 Assist multiplier；
- 连续 3 个无进展窗口后使用 `debt-stalled` 收敛。

对应当前源码为 `MSMarkTerminationPolicy.h` 的 `kBacklogBuffersPerLevel=10` 和
`kCheckpointBuffers=10`。P1 虽有 638 个周期从 level 4 开始，但因为总 GC 线程数为 1，
Helper 上限仍是 0；P5 有 721 个周期从 level 4 开始并启用 1 个 Helper。

历史 P50/P100/P150 日志来自旧 runtime，其压力粒度不能用当前常量反推；表中仅保留当时日志实际
记录的 start level 和 Helper 数。

### 8.2 Sweep 压力

后台并行 Sweep 日志的模式分布：

| P | MEDIUM (mode 1) | HIGH (mode 2) | target/active Helper | `forced=1` |
| ---: | ---: | ---: | ---: | ---: |
| 1（当前） | 0 | 0 | 0 / 0 | 0 |
| 5（当前） | 63 | 673 | 1 / 1 | 0 |
| 50 | 1 | 0 | 1 / 1 | 0 |
| 100 | 0 | 4 | 3 / 3 | 0 |
| 150 | 0 | 2 | 3 / 3 | 0 |

Sweep 的自然压力判断为：

```text
U = unswept bytes
S = swept bytes
T = heap threshold
R = max(T - S, 0)

U >= R      => HIGH
U >= R / 2  => MEDIUM
```

并带有 HIGH/ MEDIUM 降级迟滞。HIGH 的 slice budget 是 LOW 的 10 倍，并直接使用最大 Helper。

本机 `logical_cpus=8`：

- 当前 P1：`ResolveMSMarkTotalThreads()` 返回 1，只有 GC Main；
- 当前 P5：返回 2，总线程为 GC Main + 1 Helper；
- 历史 P50/P100/P150：当时日志均记录 4 个总线程，即 GC Main + 3 Helper；
- Sweep 的 `ComputeMSMaxSweepHelpers()` 使用 `gcThreads - 1`，因为 GC Main 本身也参与 Sweep；
- 未配置 `cjMSParallelSweepMaxHelpers` 时，不再存在独立的 8-Helper 上限。

当前 P1/P5 表明最大 Sweep Helper 会跟随 Mark 总线程策略：P1 没有后台并行 slice，P5 使用
1 个 Helper。历史高 P 日志则记录过 3 个 Helper，但需要当前 runtime 重跑后才能确认现版本在
P50/P100/P150 下的自动线程数和实际扩展效果。

## 9. 工作集放大与历史高 P Mark 低速

### 9.1 P 同时决定 shard 数和初始 committed heap

当前实现中：

- `shardCount = processorNum`；
- 每个 shard 的初始 segment 为 64 MiB；
- 初始 committed heap = `P × 64 MiB`。

因此 P1/P5/P50/P100/P150 一开始分别提交 64 MiB、320 MiB、3.125 GiB、6.25 GiB、9.375 GiB。
虽然 Mark 不会机械扫描每个 committed byte，但更大的分配跑道会让更多对象和 Region 在触发时
同时存活，扩大指针工作集。

对应源码：

- `src/Heap/MAllocator/MSShardRouting.h`
- `src/Heap/MAllocator/MSRegionManager.cpp`

### 9.2 对象图更大且更密

当前 P5 相对 P1 的平均 liveBytes 为 1.48x，标记对象数为 2.80x；历史 P50/P100/P150 的工作集
量级又远高于当前 P5。Mark 的核心工作是：

1. 从 WorkBuf 取灰对象；
2. 遍历引用字段；
3. 访问目标对象/TypeInfo；
4. 对目标 Region mark bitmap 做原子 `fetch_or`；
5. 首次标记且含引用字段时再次入队。

这是一条高度依赖缓存命中和内存延迟的指针追踪链。对象越密、地址分布越散，单位对象成本越高；单纯增加 Helper 很容易先碰到共享缓存和内存层次瓶颈。

对应源码：

- `src/Heap/MCollector/MarkSweepCollector.cpp` 的 `RunConcurrentMarkLoop()`、`TraceObjectRefFieldsToGCWork()`
- `src/Heap/MAllocator/MSRegionInfo.cpp` 的 `MarkBitmapWordResolved()`

### 9.3 Helper 增加了，但单位线程效率下降

当前 P5 的 Main+1 Helper 为 **31.680 Mobj/s**，相对 P1 单 Main 的 **17.163 Mobj/s**
获得 1.85x 增益，说明低 P 范围内 Helper 扩展有效。历史 P50/P100 的 Main+3 Helper 只有约
3 Mobj/s，提示旧高 P 路径还可能包括：

- 跨 Region、跨 shard 的随机访问导致缓存局部性变差；
- 所有参与者竞争/交换同一套 global WorkBuf 流；
- mark bitmap 原子更新和相邻 bitmap cache line 争用；
- WorkBuf 获取、发布、quiesce 与短任务调度开销；
- Sweep/Scavenge 与 Mark 同时争用 CPU、内存带宽或内核资源。

其中前三项需要增加 active-time、队列等待、每对象引用数、bitmap 原子命中/失败、region locality
等计数后才能进一步定量。当前数据只能确认 P1→P5 扩展有效；是否仍在当前 P50+ 退化，必须同
runtime 复跑。

### 9.4 Assist 掩盖了后台 Mark 退化

当前 P1/P5 的 Assist share 分别为 0.45%/22.01%；历史 P50/P100 则有 72%～76% 的 Mark
对象由 Mutator Assist 完成。Assist 保证了 GC 最终能推进，但会：

- 把 GC 成本转移到业务分配线程；
- 降低请求完成率；
- 使“总 Mark 速率”看起来明显高于 GC Main + Helper 的真实后台能力。

所以后续验收必须同时看总速率、Main+Helper 速率和 Assist share，不能只看总 Mobj/s。

## 10. 保持高 P Mark 速率的优化方向

在继续做高 P 优化前，第一步应使用当前 runtime 重跑 P50/P100/P150；若仍复现单位扫描速率
下降，再按下面顺序推进，而不是只增加 Helper。

### 优先级 1：建立固定工作集的 Mark 微基准和完整计数

先把“P 改变业务生成量”与“扫描器自身退化”分开：

- 固定 liveBytes、对象数、平均引用字段数和图结构；
- 仅改变 P/shard 数；
- 记录每个 Main/Helper/Assist 的 active time、wait time、处理对象数、处理引用数；
- 记录 WorkBuf pop miss、global publish/steal、quiesce wait；
- 记录 mark bitmap 原子尝试数、首次标记成功率和按 Region 的热点。

验收主指标应使用 `Mref/s`，`Mobj/s` 作为辅助，因为不同对象的引用字段数不同。

### 优先级 2：让 Mark work 保持 Region/shard 局部性

当前根对象和新发现对象最终进入共享 WorkBuf 流。可以设计：

- 每个 worker 优先消费本地 Region/shard 队列；
- 本地队列耗尽后再批量 steal；
- WorkBuf 按 Region 或地址范围聚类，而不是只按到达顺序；
- Helper 与 P home shard 建立稳定映射，减少跨 shard 抖动。

目标是减少 TypeInfo、对象 body 和 mark bitmap 的 cache miss，而不是追求更多并发消费者。

### 优先级 3：降低逐对象 bitmap 原子和发布成本

在不破坏并发标记正确性的前提下评估：

- 同一 Region/bitmap word 的小批量聚合；
- 对连续对象扫描的 bitmap word cache；
- WorkBuf 批量获取/批量发布；
- 对目标对象地址做轻量软件预取。

这一项必须用 bitmap 首次命中率和 cache miss 数据验证，不能仅靠微观代码减少几条指令判断。

### 优先级 4：解耦 P、shard 数和初始物理提交

保留每 P home shard 的路由语义，但将每 shard 64 MiB 改为按需提交或共享初始 segment，避免：

- P 增大立即把初始 committed heap 放大；
- 在同一业务负载下人为延后 GC，积累更大的对象图；
- 后续产生更大 Sweep/Scavenge 批次。

应同时观察分配 fast path、跨 shard fallback 和负载均衡，避免为了缩小堆破坏分配扩展性。

### 优先级 5：协调 Mark、Sweep、Scavenge 的带宽

高 Mark 压力期间：

- 限制大批量 Scavenge 的并发执行或设置内存带宽预算；
- 优先让 Sweep 生成可分配 Region，但避免无限制抢占 Mark worker pool；
- 以 `stopSweep` backlog 和 OOM 次数反馈后台 Sweep 调度，而不只看瞬时 `U/R`；
- 把 closure backlog 作为下一周期压力的输入，提前提高后台 Sweep duty cycle。

### 建议验收线

在固定对象图微基准中：

- 同一当前 runtime 下，P50/P100/P150 natural ConcurrentMark ≥ P5 的 **85%**；
- Main+Helper 速率 ≥ 总 Mark 速率的 **70%**；
- Assist share ≤ **30%**；
- `stopSweep` P95 < **10 ms**，不出现 >100 ms closure；
- Scavenge 不与 Mark 高压窗口形成长时间重叠；
- case27 成功吞吐和失败率不能因 GC 优化退化。

对于真实业务中 liveBytes 和对象数随 P 放大的场景，不能保证绝对 GC 周期时间与小堆相同；合理目标是保持单位引用扫描速率和后台承担比例，而不是承诺大 20 倍的对象图用相同 wall time 完成。

## 11. 数据限制

1. P1/P5 与 P50/P100/P150 使用不同 runtime MD5；跨组差异不能归因于 P 单一变量。
2. 每档只运行一轮 60 秒；P100 实际运行 73.359 秒，其他约 61～64 秒，受请求收尾和超时影响。建议做至少 3 轮并报告中位数/P95。
3. Mark 的 `processed` 是需要扫描引用的 WorkBuf 对象，不是所有对象，也没有按引用字段数归一化。
4. Sweep GB/s 是 `Region × 32KB / wall time` 的逻辑 footprint，不是 DRAM 实际带宽。
5. OOM Sweep 缺少独立 start 日志，本文观测时间包含紧邻调用窗口，适合相对比较。
6. 后台 Sweep 只覆盖 `parallel-sweep start/done`；lazy、allocation assist 等路径没有统一 wall-time 日志，所以用下一轮 closure backlog 补充判断是否及时完成。
7. Scavenge 使用活跃时间口径；它排除了 slice 间隔，适合看 FreeManager/OS release 效率，不代表从产生垃圾到 RSS 下降的端到端延迟。
8. 五档业务完成量不同，P1 与历史高 P 均有请求失败，因此 CPU/RSS 不能视为完全相同业务量下的等价资源对比。

## 12. 结果目录

### P1（当前 runtime）

- 目录：`MSGC_RESULT/Result/case27_http_complex_json_msgc_high_p/result/20260726_212722_step56_current_runtime_p1_c1000_100K_60000ms`
- GC 原始日志：`msgc/gc.log`
- 自动报告：`report.md`

### P5（当前 runtime）

- 目录：`MSGC_RESULT/Result/case27_http_complex_json_msgc_high_p/result/20260726_212847_step56_current_runtime_p5_c1000_100K_60000ms`
- GC 原始日志：`msgc/gc.log`
- 自动报告：`report.md`

### P50（历史 runtime）

- 目录：`MSGC_RESULT/Result/case27_http_complex_json_msgc_high_p/result/20260726_151254_step56_mark_sweep_scavenge_p50_c1000_100K_60000ms`
- GC 原始日志：`msgc/gc.log`
- 自动报告：`report.md`

### P100（历史 runtime）

- 目录：`MSGC_RESULT/Result/case27_http_complex_json_msgc_high_p/result/20260726_151425_step56_mark_sweep_scavenge_p100_c1000_100K_60000ms`
- GC 原始日志：`msgc/gc.log`
- 自动报告：`report.md`

### P150（历史 runtime）

- 目录：`MSGC_RESULT/Result/case27_http_complex_json_msgc_high_p/result/20260726_151558_step56_mark_sweep_scavenge_p150_c1000_100K_60000ms`
- GC 原始日志：`msgc/gc.log`
- 自动报告：`report.md`

每个 `msgc/` 子目录还保留：

- `run_metadata.env`：参数、SDK、runtime MD5；
- `compiler_identity.txt`：实际 cjc；
- `dyld_runtime_path.txt`：实际加载 runtime；
- `compile.exitcode`、`run.exitcode`；
- `rss.csv`、`stdout.txt`、`stderr.txt`。
