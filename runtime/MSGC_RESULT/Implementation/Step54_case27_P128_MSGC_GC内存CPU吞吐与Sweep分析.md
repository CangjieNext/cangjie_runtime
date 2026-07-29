# Step54：case27 P128 MSGC GC、内存、CPU、吞吐与 Sweep 分析

## 0. 文档定位与结论摘要

本文记录 case27 HTTP complex JSON 在 P=128 下的一次完整 MSGC 运行，重点分析：

1. HTTP 成功吞吐、失败率和失败类型；
2. RSS、堆提交量、active Region、true-live 和 CPU 使用；
3. GC 总时间、各阶段耗时和明确的 STW 时间；
4. ConcurrentMark、后台 Sweep、force-finish Sweep、OOM emergency Sweep 的速度；
5. Scavenge 的可观测净速度和当前日志口径限制；
6. P=128 下吞吐、OOM、Sweep backlog、Scavenge 阈值和内存峰值之间的关系。

本轮 benchmark、报告生成和编译均正常退出，实际加载的 Runtime 也与记录的 SDK
路径一致。最重要的结果如下：

| 指标 | 结果 |
| --- | ---: |
| 实际运行时间 | 65.967 s |
| 总请求 / 成功 / 失败 | 21,601 / 17,750 / 3,851 |
| 成功吞吐 | 269.07 req/s |
| 总请求速率（含失败） | 327 req/s |
| 失败率 | 17.83% |
| RSS 平均 / P95 / 峰值 | 4,287.83 / 7,098.03 / 7,953.55 MB |
| CPU 平均 / P95 / 峰值 | 324.20% / 463.78% / 538.40% |
| GC 次数 | 23 |
| OOM GC | 12，占 52.17% |
| GC 总时间 | 61.306645 s |
| GC 平均 / P95 / 最大 | 2.666 / 6.712 / 7.499 s |
| 明确 STW 累计 | 12.323130 s，占运行时间 18.68% |
| PostMark 累计 | 31.126508 s，占 GC 时间 50.77% |
| ConcurrentMark 聚合速度 | 906.07 万对象/s |
| 后台 parallel Sweep 聚合速度 | 3.29 GB/s |
| OOM emergency Sweep 聚合速度 | 2.18 GB/s |
| cold-free 净增长观测速率 | 0.823 GB/s，下界口径 |

本轮最主要的瓶颈不是“8 个 CPU 核已经被完全打满”，而是：

- P=128 创建 128 个 allocator shard，初始就提交 8 GiB heap；
- active Region 最高达到 16.212 GB，接近 16 GiB 最大虚拟堆；
- 23 个 GC 中有 12 个 OOM GC，并伴随 560 次普通分配失败和 159 次大对象分配失败；
- OOM PostMark emergency Sweep 的观测运行时间约 27.312 s，占 PostMark 的 87.74%；
- 明确 STW 中 MarkTermination 累计 11.467 s；
- Scavenge threshold 在第 9 个周期升到 16 GiB 上限，cold-free 随后被重新分配并在第 11 个周期降为 0。

因此，P=128 下的首要问题是大规模 shard/Region 供给、高分配压力、Sweep backlog
和 OOM emergency Sweep 共同造成的长 GC 与内存波动。CPU 平均只使用约 3.24 个核，
峰值约 5.38 个核，不能把本轮吞吐不足简单归因于物理 CPU 不够。

## 1. 实验目标与范围

本次实验使用现有
`MSGC_RESULT/Result/case27_http_complex_json_msgc_high_p/run_compare.sh`，
只显式覆盖 `PROCS=128`，其余参数沿用脚本当前默认值。

本次希望回答：

1. P=128 能否完整跑完 60 秒业务负载；
2. P=128 时 HTTP 成功吞吐、失败率、CPU 和 RSS 的量级；
3. GC 是否主要由 heuristic 还是 OOM 触发；
4. GC 时间主要消耗在哪个阶段；
5. 明确的 STW 时间是多少，哪些 GC 汇总时间不能解释成暂停；
6. Mark、Sweep、Scavenge 分别达到什么速度；
7. 内存增长、Sweep/Scavenge 和业务失败是否存在可观测关联。

本轮只运行 MSGC，不包含 CopyGC、Go 或 Java 对照。`run_compare.sh` 在该目录中只是
转发到 `run_msgc_high_p.sh`，不是多语言或多 GC 模式比较入口。因此本文可以描述
P=128 的绝对行为和内部瓶颈，但不能用单次结果证明：

- P=128 相对 P=8、P=128 其他 Runtime 版本或 CopyGC 的回归百分比；
- 某个优化动作能够提高多少吞吐；
- cold-free 快照变化等同于 Scavenge worker 的精确字节吞吐；
- `GC 总时间 / wall time` 等同于应用暂停比例。

上述比较需要相同 Runtime MD5、相同 SDK 三件套、相同主机状态下的额外 A/B 运行。

## 2. 运行环境与可比性控制

### 2.1 平台与 SDK 三件套

| 项目 | 值 |
| --- | --- |
| 平台 | macOS aarch64 |
| 逻辑 CPU | 8 |
| Cangjie P | 128 |
| Compiler | `cangjie_compiler/output/msgc/bin/cjc` |
| Compiler 版本 | Cangjie Compiler 1.0.0（cjnative） |
| Runtime | `cangjie_compiler/output/msgc/runtime/lib/darwin_aarch64_cjnative/libcangjie-runtime.dylib` |
| Runtime MD5 | `6163f423623ca89df12aef9812770407` |
| stdlib/stdx | `cangjie_compiler/output/msgc/darwin_aarch64_cjnative/dynamic/stdx` |
| 编译模式 | `--gc-mode=marksweep` |
| GC 总线程数 | 4，即 GC main + 最多 3 个 Helper |

`codesign --verify` 确认 Runtime 签名有效。运行期 `DYLD_PRINT_LIBRARIES=1` 的实际
加载路径与上表 Runtime 完全一致。本轮没有替换 SDK Runtime；脚本仍在运行前重新
使用该 SDK 编译 `benchmark.cj`，没有复用旧 binary。

### 2.2 benchmark 参数

| 参数 | 值 |
| --- | ---: |
| payload | 100K，实际 99,572 bytes |
| client concurrency | 1000 |
| client mode | sync |
| 配置运行时间 | 60,000 ms |
| 实际运行时间 | 65,967 ms |
| client timeout | 5,000 ms |
| heap 上限 | 16 GiB |
| `cjProcessorNum` | 128 |
| `cjHeapGrowth` | 1，Runtime 日志中 growth 为 2.00 |
| `cjScavengeGrowth` | 5 |
| `cjMSMinGCThreshold` | 20 MB；当前 Runtime 实际使用内部 20 MB floor |
| `cjGCThreads` | 未显式设置，由 Runtime 自动解析为总数 4 |
| `cjGCInterval` | 未显式设置，日志为 0 ms |

实际时间比配置时间多 5.967 s，原因是 60 秒发压结束后仍需等待 1000 个在途 worker
收敛到 `inflight=0`。吞吐计算使用最终 `[RESULT] duration=65967ms`，不是直接用
60,000 ms。

### 2.3 隔离与运行完整性

外层和脚本内部均使用干净环境，脚本在 `env -i` 子 shell 中：

1. source 当前 MSGC SDK 的 `envsetup.sh`；
2. 重新编译 `.cj` benchmark；
3. 设置 `MRT_REPORT`、`cjHeapSize` 和 MSGC 参数；
4. 使用 `DYLD_PRINT_LIBRARIES=1` 记录实际 Runtime；
5. 以 200 ms 间隔采集 RSS 和 CPU；
6. 等待 benchmark 与采样进程退出；
7. 校验最终 `[RESULT]` 并生成图表和 `report.md`。

完成后复核结果为：

```text
compile.exitcode = 0
run.exitcode     = 0
GC START         = 23
GC total summary = 23
OOM GC           = 12
```

## 3. 运行结果与业务吞吐

### 3.1 最终请求结果

最终输出为：

```text
[RESULT] payload=100K concurrency=1000 duration=65967ms client_mode=sync
[RESULT] total=21601 ok=17750 fail=3851 inflight=0 throughput=327req/s
[RESULT] bytes_sent=1685MB bytes_recv=1699MB
```

按实际运行时间计算：

```text
成功吞吐 = 17,750 / 65.967 s = 269.07 req/s
失败率   =  3,851 / 21,601   = 17.83%
发送速率 =  1,685 / 65.967   = 25.54 MB/s
接收速率 =  1,699 / 65.967   = 25.76 MB/s
```

总请求速率 327 req/s 包含失败请求，不能当作有效业务吞吐。本文后续以
269.07 req/s 作为成功吞吐口径。

### 3.2 失败类型

标准输出中的主要告警计数为：

| 告警 | 次数 | 含义 |
| --- | ---: | --- |
| `read response timeout` | 3,851 | 与最终失败请求数完全一致 |
| `Socket is closed` | 3,648 | 服务端或连接清理阶段观察到关闭 |
| `Broken pipe` | 704 | 对端超时/关闭后服务端继续写响应 |
| `No buffer space available` | 20 | macOS socket buffer/网络资源压力 |

这些日志之间会重叠，不能直接相加作为独立失败数。可确认的是，所有最终失败请求
都有 read-response timeout；socket closed 和 broken pipe 是同一批超时连接在服务端
及清理路径上的伴随现象。

GC 日志还记录：

```text
Regular allocation failed = 560
Large allocation failed   = 159
```

分配失败与网络超时不构成逐请求一一映射，但二者在长 GC/OOM 窗口中同时密集出现，
说明业务处理延迟已经超过 5 秒 client timeout。

### 3.3 吞吐时间序列

从相邻 `[PROGRESS]` 快照计算的成功吞吐窗口范围为：

| 指标 | 成功吞吐 |
| --- | ---: |
| 最低窗口 | 136.95 req/s |
| 平均窗口 | 274.25 req/s |
| 中位数 | 267.19 req/s |
| P95 | 395.97 req/s |
| 最高窗口 | 487.26 req/s |

`[PROGRESS]` 由并发计数器快照组成，部分行中的 `total` 与 `ok + fail` 存在读取时序差异，
所以它适合观察吞吐趋势，不用于替代最终 `[RESULT]`。多个 progress 间隔从约 2 秒拉长
到 8～9 秒，恰好出现在长 OOM GC 窗口，业务吞吐呈现明显锯齿。

## 4. CPU 与 RSS 内存

### 4.1 CPU

`rss.csv` 共包含 277 个约 200 ms 间隔样本：

| 指标 | CPU |
| --- | ---: |
| 平均 | 324.20% |
| P95 | 463.78% |
| 峰值 | 538.40% |
| 峰值出现时间 | 进程启动后约 8.17 s |

macOS `ps %cpu` 的 100% 约等于一个逻辑核。因此本轮平均约使用 3.24 个核，峰值约
5.38 个核；相对 8 个逻辑 CPU，平均约 40.5%，峰值约 67.3%。P=128 表示 Scheduler
Processor 数量，不代表存在 128 个物理执行核心，也不代表 GC 会自动使用 128 个线程。
Runtime 实际只配置 4 个 GC Mark 线程。

### 4.2 RSS

| 指标 | RSS |
| --- | ---: |
| 平均 | 4,287.83 MB |
| P95 | 7,098.03 MB |
| 峰值 | 7,953.55 MB |
| 峰值出现时间 | 进程启动后约 35.25 s |

最后一个 116.25 MB 样本位于 benchmark 退出和 Runtime 清理阶段，不能代表稳态工作集。
稳态判断应使用平均、P95、峰值和时间序列，而不是退出前最后一个样本。

### 4.3 MSGC heap 与 Region

初始化日志为：

```text
committedRegions   = 262144
maxVirtualRegions  = 524288
MS Region size     = 32 KiB
shardCount         = 128
segmentSize        = 64 MiB
```

由此得到：

```text
初始 committed heap = 262144 × 32 KiB = 8 GiB
最大 virtual heap   = 524288 × 32 KiB = 16 GiB
```

通用 Runtime 参数日志中的 `Region size: 64(KB)` 属于原有通用 Region 配置；MSGC
allocator 的 ABI Region 大小由 `MS_REGION_SIZE=32 KiB` 定义。Sweep/Scavenge 的
Region 字节折算必须使用 32 KiB。

各 GC 周期结束快照中的关键峰值：

| 指标 | 峰值 | 周期 |
| --- | ---: | ---: |
| active-region bytes | 16.212 GB | cycle 14 |
| true-live bytes | 5.833 GB | cycle 16 |
| RSS | 7.954 GB | 约 cycle 12～13 时间窗口 |
| 最大逻辑 heap | 17.180 GB，即 16 GiB | 运行中达到上限 |

cycle 14 的 active Region 已约占 16 GiB 最大虚拟堆的 94.37%，但 true-live 只有
5.708 GB，true-live utilization 为 35.21%。这说明大部分 Region footprint 当时
不是最终存活数据，而是待 Sweep、current sweep generation、分配缓存、碎片或尚未
归还的空闲 Region。

最后两个周期分别降到：

| 周期 | active Region | true-live | utilization |
| ---: | ---: | ---: | ---: |
| 22 | 473.73 MB | 32.17 MB | 6.79% |
| 23 | 480.44 MB | 34.83 MB | 7.25% |

但 `current heap` 仍为 16 GiB，说明逻辑提交容量没有随 true-live 下降而同步缩小。
物理 RSS 是否下降还取决于 cache flush、Scavenge 和 segment shrink，而不是只看 mark
后的存活字节。

## 5. GC 总体行为

### 5.1 总体统计

| 指标 | 结果 |
| --- | ---: |
| GC 次数 | 23 |
| heuristic GC | 11 |
| OOM GC | 12 |
| OOM 占比 | 52.17% |
| GC 总时间 | 61,306.645 ms |
| GC 平均 | 2,665.506 ms |
| GC P95 | 6,712.17 ms |
| GC 最大 | 7,498.922 ms |
| `GC 总时间 / wall time` | 92.93% |

92.93% 是 GC pipeline 的顺序 wall time 汇总，不是 STW 暂停率。ConcurrentMark 和
PostMark 允许 mutator 运行，后台 Sweep 还会跨 GC 周期执行；暂停时间需要按第 7 节
的源码边界单独计算。

GC 时间分布：

| 区间 | 次数 | 占比 |
| --- | ---: | ---: |
| 1～2 ms | 3 | 13.0% |
| 2～5 ms | 1 | 4.3% |
| 10～20 ms | 2 | 8.7% |
| 50～100 ms | 1 | 4.3% |
| 100～200 ms | 1 | 4.3% |
| 500 ms～1 s | 1 | 4.3% |
| 大于 1 s | 14 | 60.9% |

### 5.2 每周期总时间

| Cycle | 原因 | 总时间 | 主要特征 |
| ---: | --- | ---: | --- |
| 1 | heuristic | 2.860 ms | 冷启动小周期 |
| 2 | heuristic | 1.717 ms | 小周期 |
| 3 | heuristic | 1.542 ms | 小周期 |
| 4 | heuristic | 1.679 ms | 小周期 |
| 5 | heuristic | 18.615 ms | root/mark 开始放大 |
| 6 | heuristic | 83.344 ms | true-live 335 MB |
| 7 | heuristic | 2,745.562 ms | ConcurrentMark 1.477 s，MarkTermination 1.119 s |
| 8 | heuristic | 2,601.143 ms | EnumRoots 566 ms，MarkTermination 1.236 s |
| 9 | heuristic | 3,926.471 ms | MarkTermination 1.470 s，PostMark 763 ms |
| 10 | heuristic | 3,142.569 ms | active Region 10.773 GB |
| 11 | OOM | 4,472.318 ms | PreEnum force-finish 182,342 Regions |
| 12 | OOM | 7,498.922 ms | 本轮最大，PostMark 4.041 s |
| 13 | OOM | 2,034.655 ms | PostMark 1.683 s |
| 14 | OOM | 6,035.993 ms | active Region 峰值 16.212 GB |
| 15 | OOM | 3,376.350 ms | PostMark 2.641 s |
| 16 | OOM | 5,918.682 ms | true-live 峰值 5.833 GB |
| 17 | OOM | 4,079.705 ms | PostMark 2.593 s |
| 18 | OOM | 6,787.303 ms | PostMark 4.434 s |
| 19 | OOM | 5,366.393 ms | PostMark 4.499 s |
| 20 | OOM | 2,105.266 ms | PostMark 1.614 s |
| 21 | OOM | 989.715 ms | ConcurrentMark 678 ms |
| 22 | OOM | 104.685 ms | true-live 降到 32 MB |
| 23 | heuristic | 11.156 ms | 收尾周期 |

前 6 个 heuristic 周期主要建立初始 threshold。cycle 7 开始对象图和 active Region
快速放大；cycle 11 后进入连续 12 个 OOM GC，直到业务发压结束附近才回到 heuristic。

### 5.3 OOM 行为

12 个 OOM 周期都执行一次 PostMark emergency parallel sweep 和 cache flush。与此同时，
mutator 日志密集输出同步 OOM GC 请求。异步/同步任务去重保证 GC main 不会无限并行启动
多个 GC，但分配线程仍会持续经历失败、等待和重试。

因此，“进程最终退出码为 0”只表示 Runtime 没有崩溃且 benchmark 完成了清理，不表示
P=128 在业务层达到健康稳态。17.83% 请求失败和 52.17% OOM GC 已经说明该配置处于
明显的资源压力区。

## 6. GC 各阶段耗时

23 个完整 GC summary 的分阶段汇总如下：

| 阶段 | 累计 | 平均 | P95 | 最大 | GC 时间占比 |
| --- | ---: | ---: | ---: | ---: | ---: |
| PreEnum | 2,470.418 ms | 107.409 ms | 149.048 ms | 1,645.317 ms | 4.03% |
| EnumRoots | 3,459.366 ms | 150.407 ms | 538.229 ms | 694.228 ms | 5.64% |
| ConcurrentMark | 8,926.149 ms | 388.093 ms | 1,130.786 ms | 1,477.422 ms | 14.56% |
| MarkTermination | 11,467.353 ms | 498.581 ms | 1,463.854 ms | 1,693.928 ms | 18.70% |
| PostMark | 31,126.508 ms | 1,353.326 ms | 4,394.324 ms | 4,499.082 ms | 50.77% |
| PostGC | 3.906 ms | 0.170 ms | 0.229 ms | 2.277 ms | 0.01% |
| Other | 3,852.945 ms | 167.519 ms | 680.810 ms | 734.014 ms | 6.28% |

### 6.1 最大阶段：PostMark

`MarkSweepCollector::PostMarkTermination()` 在 mutator 恢复后执行，包含：

1. finalizer 通知；
2. large garbage 回收；
3. `ComputeMarkedStats()` bitmap popcount；
4. OOM 周期的一次 emergency parallel sweep；
5. OOM cache flush；
6. 通知后台 Sweep 和 Scavenge。

本轮 12 次 OOM emergency sweep 从 `Mark bitmap` 日志到
`OOM parallel sweep once` 日志的累计观测时间约 27.312 s：

```text
27.312 / 31.127 = 87.74%
```

所以 PostMark 不是单纯的统计开销；本轮绝大多数 PostMark 时间来自 OOM emergency
Sweep。该阶段虽然非 STW，但会与 mutator 争用 GC thread pool、CPU cache、内存带宽和
allocator 元数据，仍会显著影响业务延迟。

### 6.2 第二大阶段：MarkTermination

MarkTermination 累计 11.467 s，占 GC 时间 18.70%，且是完整 STW。长周期中需要：

- flush 所有 mutator write-barrier buffer；
- 回收 GC main/helper 的私有 WorkBuf；
- drain Global Full/Partial；
- drain deferred main-thread queue；
- 完成普通 mark fixed point；
- 处理 finalizer resurrection fixed point；
- 按最终 bitmap 处理 WeakRef；
- 重置 per-P GC work。

P=128 会增加 alloc buffer、scheduler root 和 per-P 状态数量；当并发 Mark 因 grow request、
debt budget 或 debt stalled 提前转入 STW 时，残余灰对象也会转移到 MarkTermination。

### 6.3 EnumRoots 与 PreEnum

EnumRoots 累计 3.459 s。它通过 GC phase handshake 让各 mutator 在 safepoint 扫描自己的
stack root，GC main 再串行枚举全局 root。高 P 和大量 HTTP coroutine 会放大 safepoint
收敛及 stack root 处理成本。

PreEnum 最大值 1.645 s 出现在 cycle 11，其中 `stopSweep=1.602 s`，是等待并强制完成上轮
182,342 个 unswept Region；它不是 1.645 s 的完整 STW。PreEnum 内明确 STW 的只是随后
clear bitmap、reclassify 和 prepare sweep snapshot 的短窗口。

## 7. STW 时间口径

### 7.1 源码边界

当前 MSGC 主流程为：

```text
PreEnumRoots
  -> EnumRoots
  -> ConcurrentMark
  -> MarkTermination
  -> PostMarkTermination
```

源码中的同步语义为：

| 阶段 | 同步语义 |
| --- | --- |
| PreEnum `StopAndWaitForAllSweep` | 停止/等待 Sweep，不是完整 STW |
| PreEnum clear/reclassify/prepare | `ScopedStopTheWorld`，完整 STW |
| EnumRoots | phase handshake，各 mutator 在 safepoint 扫描自身 stack root |
| ConcurrentMark | mutator 并发运行，Helper 和 Assist 可参与 |
| MarkTermination | `ScopedStopTheWorld`，完整 STW |
| PostMarkTermination | 源码明确标记 `non-STW, concurrent-safe` |

因此不能把 PreEnum、EnumRoots、ConcurrentMark 和 PostMark 的全部时间都加到暂停中。

### 7.2 明确 STW 汇总

| 明确 STW 来源 | 累计 |
| --- | ---: |
| PreEnum `stw=` 子阶段 | 855.777 ms |
| MarkTermination | 11,467.353 ms |
| 合计 | 12,323.130 ms |
| 占 65.967 s wall time | 18.68% |

PreEnum STW 平均 37.208 ms，最大 151.867 ms。MarkTermination 平均 498.581 ms，
最大 1,693.928 ms。

这里没有把 EnumRoots 的 3.459 s 计作“全局 STW”。但 handshake 期间每个 mutator
仍需要到达 safepoint 并扫描自身 root，业务线程会经历局部暂停和调度延迟。因此：

- 12.323 s 是源码明确完整 STW 的直接观测值；
- 它不是每个请求的精确暂停总和；
- 用户可感知延迟还包括 EnumRoots handshake、非 STW GC 资源争用、socket backlog 和
  OOM 分配等待。

### 7.3 GC summary 与暂停率的区别

```text
GC summary 累计 = 61.307 s，约占 wall 92.93%
明确 STW 累计   = 12.323 s，约占 wall 18.68%
```

两者相差的时间大多是应用可并发运行的 GC 阶段。虽然不是 STW，这些阶段依然会争用 CPU、
LLC、DRAM 和 allocator lock，所以业务吞吐仍可能在其间下降。报告中应同时保留两个口径，
不能只报其中一个。

## 8. ConcurrentMark 速度与工作分布

### 8.1 计数口径

每个周期日志包含：

```text
[mark-workers] configured=4 processed=<helper> ... mainProcessed=<main>
[assist] processed=<assist>
ConcurrentMark time: <us>
```

`processed` 计数从 WorkBuf 取出并扫描的对象数，不是扫描字节、引用边、分配对象或
HTTP 请求。MarkTermination 中处理的残余灰对象没有同口径 `processed` 计数，所以
本节速度只覆盖 ConcurrentMark。

### 8.2 累计工作分布

| 参与者 | 扫描对象 | 占比 |
| --- | ---: | ---: |
| GC main | 10,258,904 | 12.68% |
| 3 个 Helper 合计 | 35,156,544 | 43.47% |
| mutator Assist 合计 | 35,462,144 | 43.85% |
| 总计 | 80,877,592 | 100.00% |

23 个周期 ConcurrentMark 累计 8.926149 s：

```text
80,877,592 / 8.926149 s = 9,060,749 objects/s
```

即聚合约 **906.07 万对象/s**。按周期分别算速度后，平均为 1,066.02 万对象/s，
但小周期只有数百对象和几十微秒，固定开销及计时粒度会放大周期平均；评价整次运行应
优先使用累计对象除以累计时间。

### 8.3 Helper 和 Assist 的解释

Runtime 自动解析出总 GC Mark 线程数 4，即 GC main 加最多 3 个 Helper，而不是根据
P=128 启动 128 个 GC worker。Assist 占总对象数 43.85%，说明大量 GC 扫描成本已经
转移到业务 mutator：

```text
后台 GC 扫描 = main + helper = 45,415,448 objects
mutator Assist = 35,462,144 objects
```

Assist 可以帮助并发 Mark 收敛，但会直接占用执行请求的 mutator CPU，因此“全部参与者
对象/s”不等同于免费的后台 GC 能力。

### 8.4 收敛与 STW residual

23 个周期的 Mark STW 入口原因：

| 原因 | 次数 |
| --- | ---: |
| natural | 10 |
| small-needs-grow | 7 |
| large-needs-grow | 4 |
| debt-budget | 1 |
| debt-stalled | 1 |

13/23 个周期不是自然收敛，而是 grow request 或 debt policy 请求 one-way quiesce 后
把残余工作交给 MarkTermination。`[mark-quiesce]` 累计 1,059.275 ms，最大单次
326.656 ms。它解释了部分 ConcurrentMark 尾段时间，也会增加随后 STW fixed-point
drain 的输入规模。

## 9. Sweep 速度

### 9.1 速度口径

MSGC Region 大小固定为 32 KiB。日志中的：

```text
parallel-sweep start: U=<unsweptBytes> ...
parallel-sweep done: remaining=<regions> ...
```

可按下式计算一个 slice 的逻辑处理量：

```text
sweptBytes = U_start - remainingRegions × 32 KiB
speed      = sweptBytes / (doneTimestamp - startTimestamp)
```

这里的“字节”是完成 Sweep 分类的 Region footprint，不是逐字节读取或写入的 DRAM
流量。同一个 Region 可能在不同 GC generation 被再次 Sweep，所以跨周期累计值也不是
唯一物理内存量。

### 9.2 后台 parallel Sweep

| 指标 | 结果 |
| --- | ---: |
| slice 数 | 51 |
| 逻辑处理量 | 3.823206 GB |
| slice wall time 累计 | 1,161.837 ms |
| 聚合速度 | 3.290656 GB/s |
| lease miss | 0 |
| 因新 GC 中断的 slice | 1 |

按 `targetHelpers` 拆分：

| targetHelpers | slice | 处理量 | 时间 | 聚合速度 |
| ---: | ---: | ---: | ---: | ---: |
| 1 | 49 | 3.439231 GB | 1,144.866 ms | 3.004 GB/s |
| 2 | 1 | 0.027165 GB | 8.715 ms | 3.117 GB/s |
| 3 | 1 | 0.356811 GB | 8.256 ms | 43.218 GB/s |

49/51 个常规 slice 只使用 1 个目标 Helper。最后一个 3-Helper slice 处理大量已易于
分类/清空的 Region，按逻辑 footprint 得到 43.218 GB/s；这个数不能解释为内存带宽，
也不适合代表稳态。常规单 Helper 的 3.00 GB/s 和全部 slice 聚合 3.29 GB/s 更有参考
价值。

### 9.3 PreEnum force-finish Sweep

cycle 11 进入 GC 时，上轮后台 Sweep 仍有大量 backlog：

```text
StopAndWaitForAllSweep: forceSwept=182342->0 unswept regions
PreEnumRoots: stage-times stopSweep=1,601,775us
```

按 32 KiB Region 折算：

```text
处理量 = 182,342 × 32 KiB = 5.974983 GB
速度   = 5.974983 GB / 1.601775 s = 3.730 GB/s
```

这 1.602 s 发生在 PreEnum 的 stop/wait 阶段，尚未进入其明确
`ScopedStopTheWorld` 窗口；但 GC main 无法继续，且 Sweep 与业务争用资源，所以仍会拉长
整个 GC pipeline。

### 9.4 OOM emergency Sweep

每个 OOM PostMark 执行一次：

```text
GC main + 最多 3 个 helper
  -> ParallelSweepOnceForOOM()
  -> WaitFinish()
  -> 输出 unswept=<before>-><after>
```

从紧邻调用前的 `Mark bitmap` 时间戳到 `OOM parallel sweep once` 完成日志计算：

| Cycle | Regions | 观测时间 | 逻辑速度 |
| ---: | ---: | ---: | ---: |
| 11 | 103,094 | 1,986.000 ms | 1.701 GB/s |
| 12 | 164,939 | 3,189.575 ms | 1.694 GB/s |
| 13 | 238,392 | 1,590.013 ms | 4.913 GB/s |
| 14 | 31,845 | 3,207.633 ms | 0.325 GB/s |
| 15 | 246,179 | 2,570.766 ms | 3.138 GB/s |
| 16 | 93,552 | 2,351.728 ms | 1.304 GB/s |
| 17 | 199,519 | 2,467.246 ms | 2.650 GB/s |
| 18 | 170,908 | 3,850.759 ms | 1.454 GB/s |
| 19 | 223,209 | 4,406.614 ms | 1.660 GB/s |
| 20 | 243,406 | 1,531.271 ms | 5.209 GB/s |
| 21 | 32,021 | 87.234 ms | 12.028 GB/s |
| 22 | 74,082 | 73.252 ms | 33.139 GB/s |
| **合计** | **1,821,146** | **27,312.091 ms** | **2.185 GB/s** |

该时间窗口还包含 `RunOOMEmergencyParallelSweep()` 的任务准备、线程池同步及并发系统
干扰，因此是完整运行路径的 operational speed，而不是 `SweepRegion()` 纯函数速度。
后两个小 live-set 周期中很多 Region 很快归为空，逻辑速度特别高，同样不能解释成
物理带宽。

本轮 OOM emergency Sweep 的聚合速度 2.18 GB/s 低于常规后台/force-finish 的约
3.0～3.7 GB/s，而且一次要处理几十万 Region，最终累计占用了大部分 PostMark 时间。

## 10. Scavenge 行为与速度边界

### 10.1 源码执行路径

当前 Scavenge 由 `BackgroundProcessor::BackgroundScavengeLoop()` 在 `gc-helper`
线程中执行：

1. 计算 `GetScavengeReleaseTargetBytes()`；
2. 必要时先把 large hot cache 和 empty-swept cache flush 到 hot-free；
3. 按 `cjMSScavengeRounds` 计算本轮 quantum；
4. 调用 `BackgroundScavenge(quantum)`；
5. FreeManager 使用 owner-shard `try_lock` 领取 hot-free span；
6. 在 shard lock 外执行 macOS 页释放/重映射；
7. 重新加锁，将成功释放的 Region 标记为 cold-free。

源码目前写成：

```cpp
(void)space.BackgroundScavenge(quantum);
```

返回的 released bytes 没有写入 REPORT 日志，也没有记录 slice 开始/结束时间。因此本轮
不能从 `gc.log` 直接计算精确 Scavenge worker GB/s。

### 10.2 cold-free 快照

每个 GC 结束时都会输出 128 个 shard 的 `coldFree` 数量。汇总后的关键变化为：

| Cycle | cold-free | 相对前周期净变化 | Scavenge threshold |
| ---: | ---: | ---: | ---: |
| 1 | 0 GB | — | 初始阶段 |
| 2 | 0.185 GB | +0.185 GB | 0.014 GB |
| 3 | 0.318 GB | +0.133 GB | 0.015 GB |
| 4 | 0.518 GB | +0.200 GB | 0.016 GB |
| 5 | 0.775 GB | +0.257 GB | 0.119 GB |
| 6 | 1.196 GB | +0.420 GB | 0.811 GB |
| 7 | 2.414 GB | +1.219 GB | 13.534 GB |
| 8 | 1.230 GB | -1.184 GB | 15.574 GB |
| 9 | 0.999 GB | -0.231 GB | 17.180 GB |
| 10 | 0.134 GB | -0.865 GB | 17.180 GB |
| 11 | 0 GB | -0.134 GB | 17.180 GB |
| 12～23 | 0 GB | 0 | 17.180 GB |

cycle 1 到 cycle 7 的正向净增长合计 2.414281 GB，对应快照窗口累计
2.933113 s：

```text
2.414281 GB / 2.933113 s = 0.823112 GB/s
```

这只是 **cold-free 净增长下界**：

- 同一窗口中可能同时有新 Scavenge 和 cold Region 重用；
- 快照只在 GC 结束时采集，无法观察两个 GC 之间的细粒度波动；
- net delta 会把“释放”和“重新分配”互相抵消；
- 时间分母覆盖了 GC、mutator 和其他后台工作，不是纯 Scavenge CPU 时间。

### 10.3 本轮 Scavenge 结论

cycle 7 后 cold-free 持续下降，说明已 Scavenge Region 被高分配负载快速重用。cycle 9
起 Scavenge threshold 已达到 16 GiB 最大堆上限；后续 retained 不再高于一个更低的
释放目标，cold-free 在 cycle 11 归零后没有重新增长。

所以本轮不能说“Scavenge worker 的纯速度只有 0.823 GB/s”；能够直接支持的结论是：

- 早期观察到的 cold-free 正向净形成速度下界约 0.823 GB/s；
- 早期最多形成 2.414 GB cold-free；
- 这些 cold-free 在高 P 分配压力下被迅速重用；
- threshold 升到 16 GiB 后，Scavenge 没有持续形成新的 cold-free；
- 结束时 active/true-live 已很低，但 current heap 仍保持 16 GiB。

若要精确测 Scavenge，应增加 per-slice `targetBytes`、`releasedBytes`、`elapsedUs`、
`claimedRegions`、`lockMissShards` 和 `reusedColdRegions` 计数，再按 GC phase 标记它与
ConcurrentMark 的重叠窗口。

## 11. 吞吐、GC、内存和 CPU 的关联

### 11.1 从启动到连续 OOM

关键时间线如下：

```text
11:01:05  Runtime 启动，P=128，初始 committed heap=8 GiB
11:01:06  warmup 完成，1000 个 sync worker 开始
11:01:06  cycle 1～6 快速完成
11:01:06  cycle 7 开始，首次出现 1 s 以上 Mark/MarkTermination
11:01:09  cycle 8
11:01:13  cycle 9，Scavenge threshold 继续升高
11:01:19  cycle 10，active Region 已到 10.773 GB
11:01:22  cycle 11 首个 OOM GC，PreEnum 强制清 182,342 Region backlog
11:01:27  cycle 12 OOM GC
11:01:35  cycle 13 OOM GC
11:01:36  cycle 14 OOM GC，active Region 达 16.212 GB 峰值
11:01:43  cycle 15 起继续连续 OOM
11:02:12  cycle 22 OOM 后 live-set 降至 32 MB
11:02:12  cycle 23 heuristic 收尾
```

### 11.2 吞吐停顿与长 GC

`[PROGRESS]` 之间最明显的长间隔为：

| 进度区间 | 间隔 | 对应 GC 压力 |
| --- | ---: | --- |
| 20.133 s → 28.596 s | 8.463 s | cycle 10 + 首个 OOM cycle 11 |
| 28.596 s → 36.678 s | 8.082 s | cycle 12/13 |
| 40.724 s → 50.170 s | 9.446 s | cycle 14/15 |
| 54.727 s → 62.532 s | 7.805 s | cycle 17～20 |

这些窗口中既有完整 STW，也有非 STW PostMark emergency Sweep。即使 mutator 已恢复，
PostMark 仍占用 GC worker、CPU 和内存子系统；同时大量 allocation failure 会让请求处理
线程等待同步 OOM GC。最终表现为请求完成计数长时间不更新、超过 5 秒 read timeout。

### 11.3 RSS 峰值与 active Region 峰值

RSS 峰值出现在进程启动后约 35.25 s，对应 wall clock 约 11:01:40，落在 cycle 14
的长 OOM GC 窗口。该周期 active Region 最高达到 16.212 GB，而 true-live 为
5.708 GB。

RSS 7.954 GB 小于 active Region 16.212 GB 并不矛盾：

- active Region 是 Runtime 逻辑 Region footprint；
- cold/hot page、零页、尚未触碰页和 macOS 虚拟映射不会都形成等量 RSS；
- Scavenge/重映射和页复用会改变物理驻留；
- RSS 采样只观察进程物理工作集。

因此评估内存必须同时看 active、true-live、current heap、hot/cold-free 和 RSS，不能只看
其中一个。

### 11.4 CPU 没有打满不代表没有资源瓶颈

平均 CPU 324% 而成功吞吐只有 269 req/s，说明大量时间不是简单的可并行计算：

- socket timeout、closed/broken pipe 和网络 buffer 压力；
- safepoint/GC phase handshake；
- mutator 等待同步 OOM GC；
- Sweep/Scavenge 的 shard lock、thread-pool 同步和内存映射；
- bitmap、RegionInfo、JSON 对象图的 LLC/DRAM 访问；
- 128 个 Scheduler P 与 1000 个 HTTP worker 的调度开销。

所以直接增加 GC 线程数或继续提高 P，未必能提高吞吐，反而可能加剧 cache/DRAM 与调度竞争。

## 12. 核心结论

1. **P=128 可以完整退出，但不是健康稳态。**
   进程和报告均退出 0，但成功吞吐只有 269.07 req/s，失败率 17.83%，23 个 GC 中
   12 个由 OOM 触发。

2. **PostMark/OOM emergency Sweep 是最大 GC 时间来源。**
   PostMark 累计 31.127 s，占 GC 时间 50.77%；12 次 emergency Sweep 的 operational
   time 约 27.312 s，占 PostMark 87.74%。

3. **明确 STW 已足以显著影响 5 秒请求 timeout。**
   PreEnum 明确 STW 子阶段加 MarkTermination 累计 12.323 s；最大单次 MarkTermination
   1.694 s。再叠加 EnumRoots handshake、PostMark 资源竞争和分配等待，多个请求窗口超过
   5 秒。

4. **ConcurrentMark 不是唯一瓶颈。**
   ConcurrentMark 累计只占 GC 时间 14.56%，聚合速度约 906.07 万对象/s。Helper 与 Assist
   分别承担约 43.5% 和 43.8% 的扫描量；继续只优化 Mark 不能解决 31 秒 PostMark。

5. **Sweep 能力在高 backlog 下仍不足以避免 OOM force path。**
   常规后台 Sweep 约 3.29 GB/s，cycle 11 force-finish 约 3.73 GB/s，但仍积累了
   182,342 个 unswept Region。OOM emergency Sweep 聚合只有约 2.18 GB/s，并长期占据
   PostMark。

6. **Scavenge 没有形成持续可保留的 cold-free。**
   early cold-free 峰值 2.414 GB，净形成速度下界约 0.823 GB/s；随后全部被重用。cycle 9
   起 threshold 达 16 GiB，cycle 11 后 cold-free 一直为 0。

7. **P=128 的内存规模效应非常明显。**
   128 shard 使初始 committed heap 达 8 GiB；active Region 峰值 16.212 GB，而 true-live
   峰值只有 5.833 GB。大量逻辑 footprint 来自供给、backlog、缓存和碎片，而不是最终存活集。

8. **CPU 不是简单满核瓶颈。**
   平均约 3.24 核、峰值约 5.38 核。吞吐下降来自 GC/allocator/网络/内存层级的组合等待，
   不能通过“P=128 所以需要更多 CPU”解释。

综合判断：

```text
P=128 shard/初始提交规模
  + 1000 并发复杂 JSON 高分配
  + Sweep backlog 清理不及分配增长
  + Scavenge threshold 升到最大堆、cold-free 被快速重用
  -> committed/active footprint 冲到 16 GiB
  -> 连续同步 OOM GC
  -> PostMark emergency Sweep + MarkTermination 长耗时
  -> 请求超过 5 s timeout
  -> 成功吞吐下降、失败率升高
```

## 13. 后续优化与验证建议

### 13.1 P0：先补足精确观测

在改策略前，优先补充以下 REPORT 计数：

1. Scavenge 每 slice 的 target、claimed、released bytes、elapsed、shard lock miss；
2. Scavenge slice 与 GC phase/ConcurrentMark 的时间重叠；
3. OOM emergency Sweep 的独立 start/end、main/helper 分工和 size-class 分布；
4. PostMark 内 `ComputeMarkedStats`、large sweep、emergency small sweep、cache flush 的子阶段时间；
5. 每轮 Sweep backlog 的产生速率与消耗速率；
6. 每个 Helper 的首次启用、活跃时间和处理 Region 数；
7. mutator 同步 OOM 等待时间，而不只是 allocation failure 次数。

这可以把当前“从相邻日志时间戳估算”升级为 Runtime 直接计数，尤其能得到精确 Scavenge
速度和 PostMark 子阶段占比。

### 13.2 P0：建立相同 Runtime 的 P 梯度

使用同一 Runtime MD5、同一 compiler/runtime/stdlib，按以下矩阵至少重复 3 次：

```text
P = 8 / 16 / 32 / 64 / 128
payload = 100K
concurrency = 1000
duration = 60000 ms
heap = 16 GiB
```

每轮同时比较：

- 成功吞吐、失败率、p95/p99 请求延迟；
- RSS 平均/P95/峰值；
- initial committed heap 和 shard 数；
- OOM GC 次数；
- explicit STW；
- PostMark、MarkTermination、EnumRoots；
- Sweep/Scavenge bytes/s；
- regular/large allocation failure。

只有这组数据才能确定从哪个 P 开始出现非线性内存膨胀和吞吐拐点。

### 13.3 P1：限制 P 对初始提交和 shard footprint 的放大

当前 P=128 直接得到 128 shard 和 8 GiB initial committed heap。应评估：

- shard 数是否需要与 Scheduler P 一一对应；
- initial committed segment 是否应按逻辑 CPU、活跃 P 或渐进需求确定；
- idle shard 的 segment 是否可以延迟创建；
- shard hot-free 是否可以跨 shard 回收或在压力下重新平衡。

任何改动都必须同时验证分配 fast path、shard lock 竞争、large contiguous allocation 和
RSS，不能只追求更小初始 heap。

### 13.4 P1：让 Sweep 在 OOM 前消化 backlog

本轮常规后台 Sweep 大多只有 1 个 target Helper，而 OOM 后才使用 3 个 Helper 做 emergency
Sweep。建议验证：

1. 以 unswept bytes、supply miss 和分配增长速度联合决定 Sweep pressure；
2. 在 committed supply miss 前提前提升 helper；
3. 记录“本轮新增 unswept / 本轮已 swept”，按净 backlog 调整预算；
4. 避免下一轮 PreEnum 再强制清十几万个 Region；
5. 将 OOM emergency path 作为最后兜底，而不是主要稳态 Sweep 路径。

必须同步监控 ConcurrentMark 速度和业务 CPU，避免 Sweep helper 提前扩容后与 Mark/JSON
serde 争用更多内存带宽。

### 13.5 P1：重新审视 Scavenge threshold 饱和

本轮 threshold 很快升到 16 GiB 并保持不降，cold-free 无法持续形成。后续实验可验证：

- recent peak/burst reserve 的衰减窗口是否过长；
- `cjScavengeGrowth=5` 对高 P burst 是否过度保守；
- threshold 是否应区分短期 active burst 与稳定 true-live；
- Scavenge 是否应保留最小持续预算，而不是 threshold 达最大堆后完全依赖 retained gap；
- cold Region 重用率高时，是否应减少无收益的 release/recommit 往返。

不能直接永久关闭 Scavenge或强制激进释放；必须同时观察 RSS、page fault、mmap/bzero CPU、
cold reuse、OOM 和下一轮 Sweep backlog。

### 13.6 P2：降低 MarkTermination residual

13/23 周期通过 grow/debt 原因进入 STW。应按终止原因分别统计 residual WorkBuf 和 STW
扫描对象，验证：

- grow request 是否过早触发 one-way quiesce；
- debt-budget/debt-stalled 的阈值是否适合 P=128；
- Assist 是否能更早偿还 backlog，而不是把 43.85% 扫描成本压到业务线程后仍留下大 STW；
- root transfer 和 private partial 是否能在 STW 前更充分收敛。

目标应是降低 explicit STW 和请求 timeout，而不是只提高 ConcurrentMark 对象/s。

### 13.7 验证纪律

后续每次替换 Runtime 后必须：

1. 重签名 `libcangjie-runtime.dylib`；
2. 在干净 `env -i` 环境中确认实际加载路径；
3. 重新编译 `benchmark.cj`；
4. 明确 compiler/runtime/stdlib 三件套来源；
5. 等产物、退出码和最终 `[RESULT]` 落盘后再比较；
6. 将旧 `U/UE` 进程与本轮进程分开；
7. 保持 payload、concurrency、timeout、heap 和主机负载一致。

## 14. 证据与复现路径

### 14.1 结果目录

```text
MSGC_RESULT/Result/case27_http_complex_json_msgc_high_p/result/
20260726_110103_p128_gc_analysis_p128_c1000_100K_60000ms/
```

关键文件：

| 文件 | 内容 |
| --- | --- |
| `report.md` | 脚本生成的基础汇总报告 |
| `msgc/gc.log` | 23 个 GC 周期、Sweep、阈值和 Region 快照 |
| `msgc/stdout.txt` | benchmark 配置、progress、最终 result、网络告警 |
| `msgc/stderr.txt` | dyld 库加载记录 |
| `msgc/rss.csv` | 277 个 CPU/RSS 样本 |
| `msgc/run_metadata.env` | P、heap、SDK、Runtime MD5 和环境参数 |
| `msgc/compiler_identity.txt` | 实际 `cjc` 路径和版本 |
| `msgc/dyld_runtime_path.txt` | 实际加载的 Runtime |
| `msgc/compile.exitcode` | 编译退出码 0 |
| `msgc/run.exitcode` | 运行退出码 0 |

可直接查看：

- [基础报告](../Result/case27_http_complex_json_msgc_high_p/result/20260726_110103_p128_gc_analysis_p128_c1000_100K_60000ms/report.md)
- [GC 原始日志](../Result/case27_http_complex_json_msgc_high_p/result/20260726_110103_p128_gc_analysis_p128_c1000_100K_60000ms/msgc/gc.log)
- [benchmark 标准输出](../Result/case27_http_complex_json_msgc_high_p/result/20260726_110103_p128_gc_analysis_p128_c1000_100K_60000ms/msgc/stdout.txt)
- [RSS/CPU 数据](../Result/case27_http_complex_json_msgc_high_p/result/20260726_110103_p128_gc_analysis_p128_c1000_100K_60000ms/msgc/rss.csv)
- [运行元数据](../Result/case27_http_complex_json_msgc_high_p/result/20260726_110103_p128_gc_analysis_p128_c1000_100K_60000ms/msgc/run_metadata.env)
- [动态库探针](../Result/case27_http_complex_json_msgc_high_p/result/20260726_110103_p128_gc_analysis_p128_c1000_100K_60000ms/msgc/dyld_runtime_path.txt)

图表：

![P128 吞吐时间序列](../Result/case27_http_complex_json_msgc_high_p/result/20260726_110103_p128_gc_analysis_p128_c1000_100K_60000ms/msgc_throughput_timeline.png)

![P128 RSS 与 CPU](../Result/case27_http_complex_json_msgc_high_p/result/20260726_110103_p128_gc_analysis_p128_c1000_100K_60000ms/msgc_rss_cpu_timeline.png)

![P128 GC 单周期耗时](../Result/case27_http_complex_json_msgc_high_p/result/20260726_110103_p128_gc_analysis_p128_c1000_100K_60000ms/msgc_gc_per_cycle.png)

### 14.2 运行命令

```bash
cd /Users/lipeisheng/code/cangjie/cangjie/cangjie_runtime_MSGC/runtime

env -i HOME="$HOME" TERM="$TERM" \
  PATH=/usr/bin:/bin:/usr/sbin:/sbin:/opt/homebrew/bin \
  PROCS=128 \
  TAG=p128_gc_analysis \
  bash MSGC_RESULT/Result/case27_http_complex_json_msgc_high_p/run_compare.sh
```

脚本内部会再创建干净子环境、source MSGC SDK、重新编译 benchmark 并记录 dyld 路径。

### 14.3 本文计算口径复核

完成文档前已重新校验：

```text
compile_rc         = 0
run_rc             = 0
procs              = 128
GC START count     = 23
GC total count     = 23
OOM cycle count    = 12
sum(total gc time) = 61,306,645 us
```

Sweep 使用 32 KiB MSGC Region 折算；Scavenge 明确标记为 cold-free 净快照下界；
STW 只统计源码中明确 `ScopedStopTheWorld` 的 PreEnum 子阶段和 MarkTermination。
