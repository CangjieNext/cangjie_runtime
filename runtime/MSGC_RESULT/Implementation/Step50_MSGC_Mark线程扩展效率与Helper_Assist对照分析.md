# Step50：MSGC Mark 线程扩展效率与 Helper/Assist 对照分析

## 0. 文档定位与结论摘要

本文是 Step49 中 Mark/Helper 扩展实验的独立分析文档，重点回答以下问题：

1. 关闭 Mark Assist 后，单个 GC Mark 线程每秒能扫描多少对象；
2. GC 主线程加 3 个 Helper 时，扫描速度是否随线程数线性增长；
3. 为什么不能用 `mainProcessed` 直接推导“单线程比多线程快 6 倍”；
4. Helper 和 mutator Mark Assist 分别承担了多少工作；
5. 下一轮需要怎样的受控实验，才能分离调度、队列竞争和内存带宽的影响。

本轮同一 Runtime 版本、同为关闭 Assist 的两组主要结果如下：

| 模式 | 代表性长周期 | 已完成周期累计 | 相对单线程加速 |
| --- | ---: | ---: | ---: |
| 1 个 Mark 线程，Assist 关闭 | 598.25 万对象/秒 | 598.72 万对象/秒 | 1.00x |
| 最多 4 个 Mark 线程，Assist 关闭 | 1,064.13 万对象/秒 | 1,061.66 万对象/秒 | 1.78x |

因此，当前数据支持的结论不是“单线程速度是多线程的 6 倍”，而是：

- 单线程长周期约处理 **598.25 万对象/秒**；
- 4 线程配置下，GC 主线程和 Helper 合计约处理 **1,064.13 万对象/秒**；
- 4 线程相对单线程约为 **1.779x**，按配置宽度折算约为 **44.47%**；
- `configured=4` 只表示本周期允许“1 个 GC 主线程 + 最多 3 个 Helper”，日志尚未统计每个 Helper 的实际活跃时长，不能把 44.47% 解释成 CPU 核利用率；
- 关闭 Assist 后，原本由 mutator Assist 承担的对象扫描转移给后台 Mark 线程，必须比较 `mainProcessed + helperProcessed`，不能只比较 `mainProcessed`。

这些数字只描述**已完成 ConcurrentMark 周期的对象扫描吞吐**。两次 case27 进程最终都因持续分配失败/OOM 后触发 SIGSEGV，退出码均为 139，没有完成 60 秒业务压测，也没有最终 `[RESULT]`。因此本文不把这些数字表述为 HTTP 吞吐或稳定运行结论。

## 1. 问题与实验目标

此前的 160P case27 结果中，`mainProcessed`、`helperProcessed` 和 `assistProcessed` 会随并发策略发生明显迁移。如果只观察其中一个计数，容易得出两个错误判断：

- 将 4 线程配置中的 `mainProcessed` 除以 Mark 时间，再与单线程结果相比，误判“GC 主线程变慢很多”；
- 将带 Assist 时的后台扫描量与不带 Assist 时的后台扫描量直接比较，忽略 mutator 已经分走了大量全局 Mark 工作。

本轮实验通过 `CJ_MS_DISABLE_ASSIST=1` 将 Assist 有效比例强制为 0，再分别设置：

- `cjGCThreads=1`：只有 GC 主线程扫描；
- `cjGCThreads=4`：GC 主线程加最多 3 个 Helper 扫描。

这样可以优先回答两个问题：

1. 在当前 case27 对象图和 macOS aarch64 环境中，单个后台 Mark 执行通道的实际量级是多少；
2. 将配置宽度从 1 增加到 4 后，整个后台 Mark 子系统的总处理能力提高了多少。

本轮实验并不能单独回答“4 个物理核心的纯扫描函数能否线性扩展”。原因是生产路径仍包含动态 Helper 启用、共享 WorkBuf、终止检测、线程唤醒和并发 mutator 等因素，而且两次进程并未运行到稳定结束。本文在第 9、10 节分别限定结论并给出下一轮受控方案。

## 2. `processed` 指标的源码语义

### 2.1 `mainProcessed`

`MarkSweepCollector::RunConcurrentMarkLoop()` 中，GC 主线程每轮调用：

```cpp
size_t scannedCount = DrainOneBufRound(gcThreadWork_, false);
gcMainProcessed += scannedCount;
```

因此 `mainProcessed` 是当前 ConcurrentMark 周期内，**GC 主线程从 Mark 工作缓冲区取出并扫描的对象数**。

### 2.2 `helperProcessed`

每个 Helper 在 `RunMarkWorker()` 中执行 `DrainOneBufRound()`，并通过
`MSMarkWorkerLocalStats::RecordScanned()` 累计扫描量。周期结束后，
`RunConcurrentMarkLoop()` 汇总所有 `markWorkerSlots_` 的
`TotalProcessed()`：

```cpp
size_t helperProcessed = 0;
for (const auto& slot : markWorkerSlots_) {
    helperProcessed += slot->stats.TotalProcessed();
}
```

日志中的：

```text
[mark-workers] configured=N processed=H ... mainProcessed=M
```

含义是：

- `configured=N`：本周期后台 Mark 的最大总宽度，即 GC 主线程加允许使用的 Helper 数；
- `processed=H`：所有 Helper 扫描对象数之和；
- `mainProcessed=M`：GC 主线程扫描对象数。

所以关闭 Assist 时，后台总扫描量应按下式计算：

```text
backgroundProcessed = mainProcessed + helperProcessed
```

`configured=4` 不代表 4 个线程从周期开始到结束始终同时运行。Helper 的目标数量由 `MSMarkWorkerPolicy` 根据已发布 WorkBuf backlog 动态提高，而且一个周期内只升不降：

```text
desiredHelpers = ceil(backlogBuffers / 8)
activeHelpers = max(previousActiveHelpers, desiredHelpers)
```

当前日志只记录配置上限和合计处理数，没有记录每个 Helper 的开始时间、退出时间和累计活跃时间。

### 2.3 `assistProcessed`

mutator 分配路径触发 `MSMarkAssist()` 时，也会从全局 Mark 工作中取出对象扫描。扫描量累计在各 `AllocBufferMS` 的 `MSMarkAssistStats.processed` 中，周期结束由 `SnapshotAssistStats()` 汇总并输出：

```text
[assist] processed=A
```

带 Assist 时，整个并发阶段的已统计扫描量应拆成两种口径：

```text
后台 GC 能力 = mainProcessed + helperProcessed
全部并发参与者 = mainProcessed + helperProcessed + assistProcessed
```

第二个口径包含 mutator 为偿还分配债务所做的 GC 工作，不能当作“GC 线程吞吐”；第一个口径又会受到 Assist 抢走工作量的影响，也不能脱离 Assist 占比单独评价 Helper。

### 2.4 指标不代表什么

`processed` 计数的是 Mark 工作缓冲区中被取出并扫描的对象，不是：

- 对象字节数；
- 分配对象数；
- HTTP 请求数；
- 单位时间内完成的业务量；
- 跨不同运行实例去重后的唯一对象数。

此外，当前 `MarkTermination` 没有同口径的 `processed` 计数，因此本文的速率只覆盖日志中已经完成的 `ConcurrentMark time`，不把终止阶段扫描工作加入分母或分子。

## 3. 实验环境与可比性控制

### 3.1 两组主要实验的共同条件

| 项目 | 值 |
| --- | --- |
| 平台 | macOS aarch64 |
| 逻辑 CPU | 8 |
| Cangjie P | 160，即逻辑 CPU 数的 20 倍 |
| payload | 100K |
| concurrency | 1000 |
| 请求时长 | 60000 ms |
| heap | 16 GB |
| `cjHeapGrowth` | 1 |
| `cjScavengeGrowth` | 5 |
| `CJ_MS_MIN_GC_THRESHOLD` | 20 MB |
| `CJ_MS_DISABLE_ASSIST` | 1 |
| SDK 根目录 | `/Users/lipeisheng/code/cangjie/cangjie/cangjie_compiler/output/msgc` |
| Runtime MD5 | `90e9e94b0ed715ebf3b6494b5c0f6bb3` |
| 编译模式 | `cjc ... --gc-mode=marksweep` |

SDK 三件套来源为：

- compiler：`output/msgc/bin/cjc`；
- runtime：本轮源码增量构建并安装到 `output/msgc` 的
  `libcangjie-runtime.dylib`；
- stdlib/stdx：`output/msgc` 中已经存在的产物，本轮没有重新构建。

脚本每次都重新用该 SDK 编译 `.cj` 用例，`compile.exitcode` 均为 0；运行时动态库探针也确认实际加载的是同一路径、同一 MD5 的 Runtime。因此单线程与 4 线程两组的主要代码差异只有 `cjGCThreads`。

### 3.2 唯一主要自变量

| 实验 | `cjGCThreads` | 后台 Mark 组成 |
| --- | ---: | --- |
| A | 1 | 1 个 GC 主线程 |
| B | 4 | 1 个 GC 主线程 + 最多 3 个动态 Helper |

两组日志每个周期都满足：

```text
Assist pacing: disabled=1 effectiveRatio=0
[assist] processed=0
```

这证明诊断开关已经生效，没有 mutator Mark Assist 分担扫描工作。

### 3.3 可比性边界

即使 Runtime 和配置一致，两次 case27 也不是固定对象图回放：

- mutator、网络请求和 GC 并发运行；
- 分配速率与 GC 完成时间互相影响；
- 两次长周期开始时的已分配量和可达对象规模不同；
- 两次最终都在持续分配失败/OOM 后退出 139。

所以本文同时给出：

- 每组最大的已完成 ConcurrentMark 周期，用于观察持续负载下的速度；
- 所有已完成周期的累计值，用于校验结论方向；
- 明确不使用未完成请求吞吐来证明 GC 策略优劣。

## 4. 实验 A：单 Mark 线程、关闭 Assist

结果目录：

```text
MSGC_RESULT/Result/case27_http_complex_json_msgc_high_p/result/
20260724_230719_single_gc_thread_no_mark_assist_p160_c1000_100K_60000ms
```

日志共记录 6 个已完成的 ConcurrentMark 周期：

| 周期 | Mark 时间 | GC 主线程 | Helper | Assist | 后台对象/秒 |
| ---: | ---: | ---: | ---: | ---: | ---: |
| 1 | 34 us | 173 | 0 | 0 | 508.82 万 |
| 2 | 30 us | 213 | 0 | 0 | 710.00 万 |
| 3 | 43 us | 269 | 0 | 0 | 625.58 万 |
| 4 | 28 us | 298 | 0 | 0 | 1,064.29 万 |
| 5 | 8,141 us | 105,111 | 0 | 0 | 1,291.13 万 |
| 6 | 12,129,179 us | 72,563,060 | 0 | 0 | 598.25 万 |

前 4 个周期只有数百个对象、几十微秒，计时取整和固定开销对比值影响很大；第 5 个周期仍只有约 8 ms。第 6 个周期持续 12.129 秒、扫描 7,256.306 万个对象，是本组最有代表性的持续扫描结果：

```text
72,563,060 / 12.129179 s = 5,982,520 objects/s
```

即约 **598.25 万对象/秒**。

6 个已完成周期累计：

```text
累计时间 = 12,137,455 us
累计对象 = 72,669,124
累计速率 = 5,987,179.685 objects/s
```

累计约 **598.72 万对象/秒**，与长周期结果接近，说明累计值没有被前面几个微小周期改变结论。

## 5. 实验 B：4 个 Mark 线程、关闭 Assist

结果目录：

```text
MSGC_RESULT/Result/case27_http_complex_json_msgc_high_p/result/
20260724_232204_four_gc_threads_no_mark_assist_p160_c1000_100K_60000ms
```

日志同样记录 6 个已完成的 ConcurrentMark 周期：

| 周期 | Mark 时间 | GC 主线程 | Helper 合计 | 后台合计 | Assist | 后台对象/秒 |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 1 | 66 us | 175 | 0 | 175 | 0 | 265.15 万 |
| 2 | 73 us | 215 | 0 | 215 | 0 | 294.52 万 |
| 3 | 68 us | 260 | 0 | 260 | 0 | 382.35 万 |
| 4 | 67 us | 305 | 0 | 305 | 0 | 455.22 万 |
| 5 | 18,362 us | 19,831 | 56,320 | 76,151 | 0 | 414.72 万 |
| 6 | 4,885,344 us | 12,767,076 | 39,219,200 | 51,986,276 | 0 | 1,064.13 万 |

第 1 至第 4 个周期虽然输出 `configured=4`，但 Helper 实际处理量为 0。这说明配置宽度不能等同于实际并发宽度：对象量很小时，GC 主线程已经完成工作，Helper 启动、唤醒或取得 WorkBuf 前周期就结束了。

第 5 个周期中 Helper 已经参与，但总计只有 76,151 个对象，而且 18,362 us 的 ConcurrentMark 中有 13,690 us 用于 `QuiesceMarkWorkers()`，小工作集下启动和收敛成本占主导，不适合用来评价持续并行扫描能力。

第 6 个周期持续 4.885 秒，Helper 承担了 75.44% 的后台扫描量：

```text
后台合计 = 12,767,076 + 39,219,200 = 51,986,276
Helper 占比 = 39,219,200 / 51,986,276 = 75.44%
后台速率 = 51,986,276 / 4.885344 s
         = 10,641,272.344 objects/s
```

即约 **1,064.13 万对象/秒**。

6 个已完成周期累计：

```text
累计时间 = 4,903,980 us
GC 主线程累计 = 12,787,862
Helper 累计 = 39,275,520
后台累计 = 52,063,382
累计速率 = 10,616,556.756 objects/s
```

累计约 **1,061.66 万对象/秒**，与长周期结果一致。

## 6. 单线程与 4 线程核心对比

### 6.1 代表性长周期

```text
单线程速率 = 5,982,520.334 objects/s
4 线程后台合计速率 = 10,641,272.344 objects/s

加速比 = 10,641,272.344 / 5,982,520.334
       = 1.7787x

配置宽度效率 = 1.7787 / 4
             = 44.47%
```

### 6.2 所有已完成周期累计

```text
单线程累计速率 = 5,987,179.685 objects/s
4 线程累计速率 = 10,616,556.756 objects/s

加速比 = 1.7732x
配置宽度效率 = 44.33%
```

长周期和累计口径都得到约 1.77 至 1.78 倍，结论方向一致。

### 6.3 为什么不能说“单线程是多线程的 6 倍”

4 线程长周期中：

```text
GC 主线程速率 = 12,767,076 / 4.885344 s
              = 2,613,342 objects/s

Helper 合计速率 = 39,219,200 / 4.885344 s
                = 8,027,930 objects/s
```

这里的 GC 主线程只处理了整个后台工作量的 24.56%，其余 75.44% 已被 Helper 取走。`mainProcessed` 下降首先代表**工作所有权发生转移**，不是同一个线程执行同一份工作后性能下降。

即使把单线程的 598.25 万对象/秒与 4 线程模式中的 GC 主线程 261.33 万对象/秒相除，也只有约 2.29 倍；而且这两个数字仍不能作为单线程内核效率的严格对照，因为 4 线程模式下有队列竞争、动态唤醒和不同对象图。

Helper 日志又只提供 3 个 Helper 的合计处理数，没有单 Helper 的活跃时间。把 802.79 万简单除以 3，会假设 3 个 Helper 从开始到结束都处于扫描状态，这与动态启用策略不符。因此当前数据既不支持“单线程快 6 倍”，也不能精确计算每个 Helper 的独立对象/秒。

正确对比单位是：

```text
单线程：mainProcessed
4 线程：mainProcessed + helperProcessed
带 Assist：还需单独列出 assistProcessed
```

### 6.4 44.47% 的准确含义

44.47% 是“相对于 4 倍理想线性加速的配置宽度效率”，只用于描述扩展差距：

```text
实际加速 1.7787x / 理想加速 4x = 44.47%
```

它不是：

- 4 个线程的 CPU 利用率；
- 3 个 Helper 的平均活跃比例；
- 机器物理核心利用率；
- Mark 算法中可并行部分的精确占比。

要得到这些指标，至少还需要每个 Helper 的活跃时间、等待时间、实际 CPU time 和线程调度信息。

## 7. 为什么 4 线程没有线性扩展

当前代码和日志已经能确认若干固定成本与串行点，但尚未采集锁等待和硬件计数器。因此以下分析分成“源码可确认”和“需要性能工具验证”两层。

### 7.1 动态 Helper 不是固定 4 路并行

`MSMarkWorkerPolicy::Update()` 按全局已发布 WorkBuf 数量决定目标 Helper：

```text
每 8 个 backlog WorkBuf 对应 1 个目标 Helper
至少 1 个，最多 maxHelpers
周期内只升不降
```

这带来两个直接结果：

1. `configured=4` 是上限，开始阶段不一定已经启用 3 个 Helper；
2. 对象图逐步展开时，足够大的全局 backlog 出现前，GC 主线程可能先串行执行一段时间。

前 4 个小周期 Helper 处理量为 0，已经直接证明“配置为 4”不等于“全周期 4 路并行”。长周期虽然有 3,921.92 万 Helper 扫描量，但没有每个 Helper 的时间戳，尚不能恢复实际并行宽度曲线。

### 7.2 启动、唤醒和收敛成本

Helper 通过线程池提交长生命周期任务，再由 `MSMarkWorkerControl` 控制：

- 等待当前 worker id 是否满足目标 Helper 数；
- 进入/退出 `workersInFlight`；
- 无工作时 backoff；
- 周期结束时 quiesce 并确认不再有在途 worker/assist。

这些控制路径使用 mutex/condition variable。对数秒级的大周期，固定成本通常可以摊薄；对几十微秒或十几毫秒的小周期，成本会主导结果。

实验 B 的第 5 个周期就是明确例子：

```text
ConcurrentMark = 18,362 us
mark-quiesce = 13,690 us
quiesce 占比 = 74.56%
```

因此不能根据这些小周期中的 265 至 455 万对象/秒判断持续扫描性能，也不能期望所有周期都获得并行加速。

### 7.3 全局 WorkBuf 交换存在 mutex 串行点

`globalFull_` 和 `WorkBufPool` 都基于 `LFStack`。虽然名称保留为 LFStack，当前实现为了避免 WorkBuf 激进复用导致 ABA 问题，在 `Push()`、`Pop()`、`PopFull()` 和 `GetCounts()` 中都使用同一个小 mutex。

每个执行通道拥有本地 `PerPGCWork` 的 `getCur_`/`putCur_`，且一个 WorkBuf 可容纳 1024 个对象，这已经对全局锁操作做了批量摊销；但并发线程仍会在以下时刻竞争共享结构：

- 获取新的全局 WorkBuf；
- 发布已填满的子对象 WorkBuf；
- 回收或重新取得空 WorkBuf；
- GC 主线程读取 backlog 以扩展 Helper、调节 Assist 和判断收敛。

线程数增加后，共享队列的串行部分和 cache-line 迁移不会随线程数缩短，是无法线性扩展的明确候选因素。当前没有锁等待时间计数，因此尚不能断言它是 1.78x 的第一根因。

### 7.4 对象图展开限制可用并行度

Mark 不是对一个已经切好分片的平坦数组做扫描，而是：

1. 从 WorkBuf 取得灰对象；
2. 读取对象类型和引用字段；
3. 对子对象执行 mark 判定/原子置位；
4. 将首次发现的子对象放入本地或全局 WorkBuf。

如果某一阶段只有少量根或对象图较窄，未来工作必须等当前对象被扫描后才会暴露，Helper 即使已经启动也没有足够 backlog。只有对象图展开到足够宽，多个执行通道才可能持续并行。这是并行图遍历本身的依赖，不是单纯提高线程上限就能消除的。

### 7.5 内存访问与原子更新可能成为共享瓶颈

对象扫描包含不规则指针追踪、TypeInfo/引用字段读取和 mark 状态原子更新。多线程同时运行时，潜在限制包括：

- last-level cache miss 和内存延迟；
- 可用内存带宽；
- mark bitmap/stateWord 所在 cache line 的竞争或伪共享；
- 多个线程访问相邻或相关对象时的 cache coherence 流量；
- GC 线程与 160P mutator 争用 CPU 和内存系统。

这些都是 Mark 扩展效率常见限制，而且与当前路径吻合，但本轮没有 `sample`/Instruments、线程 CPU time、cache miss 或带宽计数，必须标为待验证，不能仅凭吞吐比给出确定排序。

### 7.6 160P 调度过载会压缩 GC 线程可用算力

测试机器只有 8 个逻辑 CPU，却配置了 160 个 P，并以 concurrency=1000 持续分配。GC 主线程、最多 3 个 Helper、网络/调度线程和 mutator 都在争用有限的逻辑 CPU。

多开 Helper 不会凭空增加机器算力；它可能缩短 GC wall time，同时增加上下文切换、调度竞争以及对业务线程的抢占。当前结果证明总后台扫描速度提高到了 1.78x，但不能把未达到 4x 全部归咎于 Mark 实现，因为 case27 的超额并发也是实验环境的一部分。

### 7.7 当前根因优先级

基于现有证据，可按下面顺序继续验证：

1. **实际并行宽度不足**：日志已证明小周期 Helper 不参与，长周期仍缺少活跃时间；
2. **终止/收敛固定成本**：第 5 周期的 quiesce 占比已有直接证据；
3. **共享 LFStack 与 worker control 竞争**：源码存在 mutex 串行点，需补锁等待数据；
4. **对象图可用并行度**：由图遍历生成工作方式决定，需固定对象图实验；
5. **内存系统和 160P 调度竞争**：路径上合理但尚无 profiler/硬件计数器证明。

## 8. 与“4 线程 + Assist”历史结果的关系

历史结果目录：

```text
MSGC_RESULT/Result/case27_http_complex_json_msgc_high_p/result/
20260724_223606_after_mark_sweep_helper_policy_p160_c1000_100K_60000ms
```

该次运行同样是 macOS aarch64、P160、concurrency=1000、payload=100K、heap=16GB，日志显示 `configured=4`，但 Assist 保持启用。其 Runtime MD5 为：

```text
85f001e1463e9c154db645debd143d3a
```

与两组关闭 Assist 实验的
`90e9e94b0ed715ebf3b6494b5c0f6bb3` 不同。因此本节只能作为**历史方向性对照**，不能作为严格 A/B。

### 8.1 代表性长周期

```text
ConcurrentMark = 6.452261 s
mainProcessed = 9,975,073
helperProcessed = 29,349,888
assistProcessed = 30,590,976

后台扫描量 = 39,324,961
后台扫描速率 = 6,094,756.706 objects/s

全部扫描量 = 69,915,937
全部参与者速率 = 10,835,881.717 objects/s
Assist 工作占比 = 43.75%
```

如果只看 `mainProcessed + helperProcessed`，会认为 4 个后台 Mark 线程只有约 609.48 万对象/秒；但 Assist 已经从同一全局工作集合中处理了 3,059.10 万个对象，占全部扫描量的 43.75%。后台数值较低不代表这些线程本身慢，而是 mutator 抢先完成了相当一部分工作。

### 8.2 7 个已完成周期累计

```text
累计 ConcurrentMark = 6.521670 s
后台扫描量 = 40,584,222
Assist 扫描量 = 30,633,984
全部扫描量 = 71,218,206

后台速率 = 6,222,980.004 objects/s
全部参与者速率 = 10,920,240.675 objects/s
Assist 工作占比 = 43.01%
```

累计与长周期的 Assist 占比和总处理能力一致。

### 8.3 三组数据放在一起看

| 结果 | 后台扫描速度 | Assist 扫描占比 | 全部参与者速度 |
| --- | ---: | ---: | ---: |
| 单线程、Assist 关闭 | 598.25 万/秒 | 0% | 598.25 万/秒 |
| 最多 4 线程、Assist 关闭 | 1,064.13 万/秒 | 0% | 1,064.13 万/秒 |
| 历史最多 4 线程、Assist 开启 | 609.48 万/秒 | 43.75% | 1,083.59 万/秒 |

前两行来自同一 Runtime MD5，可以用于主要扩展对比；第三行 Runtime 不同，只看量级和工作迁移现象。

方向上可以看到：

- 关闭 Assist 后，后台线程承担了全部可用 Mark 工作，后台合计速度从带 Assist 历史结果的约 609 万/秒提高到约 1,064 万/秒；
- 带 Assist 时，将后台和 Assist 相加后，总处理能力约 1,084 万/秒，与关闭 Assist 后后台约 1,064 万/秒处在相近量级；
- 这更符合“Assist 改变工作分配和 GC/mutator 成本归属”，而不是“Assist 让某个 GC 线程突然快或慢数倍”。

由于 Runtime MD5、周期对象图和运行时状态不同，不能用 1,083.59 万与 1,064.13 万之间约 1.8% 的差值判断 Assist 的净性能收益。严格结论必须使用同一二进制做 A/B，并同时观察业务吞吐和 CPU 消耗。

## 9. 当前可以和不可以得出的结论

### 9.1 当前可以确认

1. `CJ_MS_DISABLE_ASSIST=1` 已真正生效：两个主实验每周期都是
   `disabled=1 effectiveRatio=0`，且 `assistProcessed=0`。
2. 同一 Runtime MD5 下，将 `cjGCThreads` 从 1 调到 4 后，代表性长周期后台对象扫描速度从约 598.25 万/秒提高到约 1,064.13 万/秒。
3. 对应加速为 1.7787x，不是 4x 线性扩展；按配置宽度折算为 44.47%。
4. 4 线程长周期中 Helper 承担 75.44% 的后台扫描量，Helper 路径确实被使用。
5. 小周期中 Helper 可能完全不处理对象，`configured=4` 只是最大宽度。
6. `mainProcessed`、`helperProcessed` 和 `assistProcessed` 之间存在工作迁移，评价整个 Mark 阶段必须按参与者口径求和。
7. 当前数据不支持“单线程处理速度是多线程的 6 倍”。

### 9.2 当前不能确认

1. **不能确认单个 Helper 的对象/秒。** 缺少每线程处理数和实际活跃时间。
2. **不能确认 4 个执行通道的物理核利用率。** 缺少线程 CPU time 和调度数据。
3. **不能确认共享 LFStack 是最大瓶颈。** 源码中存在 mutex，但没有锁竞争/等待占比。
4. **不能确认已经达到内存带宽上限。** 没有 cache miss、带宽或硬件性能计数器。
5. **不能确认 Assist 开关对业务吞吐的净影响。** 历史对照使用了不同 Runtime，主实验又未成功跑满 60 秒。
6. **不能把 598 万或 1,064 万对象/秒换算成固定 GB/s。** `processed` 没有记录每个对象扫描的字节数和引用字段数。
7. **不能认为 Mark 总停顿只等于 ConcurrentMark 时间。** 本文没有计入缺乏同口径对象计数的 MarkTermination。
8. **不能把这两次运行当作稳定性通过。** 两个进程最终都是退出码 139。

### 9.3 关于退出 139 的边界

两组运行都在 P160 的持续分配压力下出现大量 allocation failure/OOM，随后 `gc-main-thread` 在 `FrameInfo::ResolveProcInfo()` 附近以地址 `0x18` 崩溃，没有最终业务 `[RESULT]`。

这个崩溃需要作为独立稳定性问题处理，不能被本轮 Mark 扫描数据掩盖。另一方面，本文只使用崩溃前已经完整输出了 `ConcurrentMark time`、`mark-workers` 和 `assist` 的周期；这些周期的扫描计数仍可用于诊断 Mark 执行能力，但不能证明整个 Runtime 或 case27 已通过验证。

## 10. 下一步受控验证方案

下一步不应立即根据 1.78x 调整 Helper 数量。应先把“实际没有同时运行”“共享队列竞争”和“内存/调度饱和”分开测量。

### 10.1 第一层：固定对象图 Mark 微基准

目标是测纯 Mark 扩展能力，尽量去除 HTTP、持续分配和对象图漂移。

建议测试流程：

1. 使用同一个 SDK、同一个 Runtime MD5 和同一个 `.cj` 源文件；
2. 预先构造对象数量、对象类型、平均引用数和拓扑固定的可达对象图；
3. 完成预热后，在 Mark 测量窗口内停止继续扩大对象图；
4. 固定 `CJ_MS_DISABLE_ASSIST=1`；
5. 依次设置 `cjGCThreads=1/2/4`，条件允许时再加 8；
6. 每个配置至少重复 5 至 10 次，舍弃预热周期，报告中位数和 P90；
7. 同时报告 ConcurrentMark 和 MarkTermination，不能只看一段 wall time。

每组至少输出：

```text
mainProcessed
helperProcessed
backgroundProcessed
ConcurrentMark us
MarkTermination us
background objects/s
进程 CPU time
各 Helper 活跃时间
```

判断方式：

- 固定对象图仍只有约 1.8x：重点查队列/控制锁和内存系统；
- 固定对象图接近线性、case27 仍低：重点查动态工作暴露、160P 调度争用和 mutator 干扰；
- 实际活跃 Helper 长期少于配置值：先修 Helper 启用策略与可观测性，再讨论扫描内核。

### 10.2 第二层：同 Runtime 的 case27 A/B

在退出 139 的稳定性问题修复后，再运行应用级矩阵：

| 维度 | 固定值/取值 |
| --- | --- |
| Runtime | 同一构建产物及 MD5 |
| Assist | 先固定关闭；之后单独做开/关 A/B |
| GC threads | 1、2、4 |
| P | 先选能稳定跑满 60 秒的值；再单独测 P160 |
| payload/concurrency/heap | 100K / 1000 / 16GB |
| 重复次数 | 每组至少 3 次 |

每次运行前必须：

- 使用目标 SDK 重新编译用例并保留 compiler/runtime 身份；
- 在隔离环境中启动，确认实际加载的 Runtime 路径和 MD5；
- 保留 `gc.log`、请求结果、RSS/CPU 时间线和退出码；
- 只有退出码为 0 且存在最终 `[RESULT]` 时，才纳入业务吞吐对比。

报告中应分开画三组指标：

1. **后台 GC 扫描能力**：`main + helper`；
2. **总 Mark 工作能力**：`main + helper + assist`；
3. **业务代价**：成功请求吞吐、P99、mutator CPU 和峰值 RSS。

这样才能判断 Assist 是提高总吞吐，还是仅把 GC 成本从后台线程转移到业务线程。

### 10.3 补强 Helper 可观测性

建议为每个 Mark 周期增加下列诊断统计，默认可受 VLOG/诊断开关控制：

```text
[mark-worker-detail]
cycle=<id>
worker=<id>
firstActiveUs=<...>
lastActiveUs=<...>
activeUs=<...>
waitUs=<...>
backoffUs=<...>
drainCalls=<...>
nonEmptyDrains=<...>
processed=<...>
```

周期级再增加：

```text
peakActiveHelpers
timeWeightedActiveHelpers
helperScaleUpEvents
globalFullPush/Pop/GetCounts 次数
globalFullLockWaitUs
workBufPoolLockWaitUs
MarkTerminationProcessed
```

其中 `timeWeightedActiveHelpers` 是解释“configured=4 实际运行了几路”的关键指标。没有这个数据前，不应把配置宽度效率等同于线程执行效率。

### 10.4 根据证据选择后续优化

| 观测结果 | 优先优化方向 |
| --- | --- |
| backlog 很早变大，但 Helper 激活晚 | 调整启动/唤醒路径或 backlog 到 Helper 的映射 |
| `timeWeightedActiveHelpers` 明显低于目标 | 减少 worker gate、backoff 和调度延迟 |
| `globalFullLockWaitUs` 占比高 | 按 worker 分片队列、work stealing 或更大批量交换 |
| WorkBuf 锁低但 cache miss/带宽高 | 优化对象/元数据访问局部性，评估线程数上限 |
| P160 时 GC CPU 得不到调度 | 建立 GC 与 mutator 的 CPU 配额/优先级策略 |
| MarkTermination 残余工作量大 | 优先修正并发阶段收敛与工作发布，而不是增加尾部特例 |

当前阶段不为 tail drain 设置特殊“全部 Helper”策略，也不新增 HIGH pressure 分支。先用统一的动态 Helper 策略收集上述数据，再决定是否需要压力分级。

### 10.5 建议验收标准

Step50 后续性能实施可以采用以下阶段性门槛：

1. `cjGCThreads=2` 在固定对象图下相对单线程达到稳定、可重复的正加速；
2. `cjGCThreads=4` 的加速和 `timeWeightedActiveHelpers` 能互相解释，不再只看配置值；
3. 无 Assist 与有 Assist 的总扫描量口径完整，误差来源可追踪；
4. case27 能稳定跑满配置时长，退出码为 0；
5. 提升 Mark 速度时，成功请求吞吐不下降、峰值 RSS 不恶化；
6. macOS aarch64 结论在 Linux aarch64 高核心机器上用同一统计口径复核。

## 11. 源码与结果索引

### 11.1 源码

| 文件 | 与本文的关系 |
| --- | --- |
| `src/Heap/MCollector/MarkSweepCollector.cpp` | ConcurrentMark 主循环、Helper 启停、Assist 汇总及周期日志 |
| `src/Heap/MCollector/MarkSweepCollector.h` | 周期统计、Worker slot 和共享 Mark 状态 |
| `src/Heap/MCollector/MSMarkWorkerPolicy.h` | 总线程数解析和 backlog 驱动的 Helper 扩展策略 |
| `src/Heap/MCollector/MSMarkWorkerControl.h` | Helper/Assist 进入退出、等待、backoff 和 quiesce |
| `src/Heap/MCollector/MSMarkLocalStats.h` | Helper 与 Assist 的 `processed` 统计语义 |
| `src/Heap/MCollector/MSAssistPacing.h` | Assist 比例和诊断关闭 override |
| `src/Common/LFStack.h` | `globalFull_`、WorkBufPool 及 mutex 保护的共享 WorkBuf 栈 |
| `MSGC_RESULT/Result/case27_http_complex_json_msgc_high_p/run_msgc_high_p.sh` | P160 用例编译、环境隔离、GC 参数和结果采集 |

### 11.2 上游文档

- `MSGC_RESULT/Implementation/Step49_MSGC高核心规模扩展与内存吞吐优化方案.md`
  - 18.5：单 Mark 线程、关闭 Assist 的原始验证；
  - 18.6：4 个 Mark 线程、关闭 Assist 的原始验证。

本文对上述数据进行独立归纳，并补充指标语义、带 Assist 历史对照和后续受控验证设计。

### 11.3 原始结果

单线程、关闭 Assist：

```text
MSGC_RESULT/Result/case27_http_complex_json_msgc_high_p/result/
20260724_230719_single_gc_thread_no_mark_assist_p160_c1000_100K_60000ms
```

最多 4 线程、关闭 Assist：

```text
MSGC_RESULT/Result/case27_http_complex_json_msgc_high_p/result/
20260724_232204_four_gc_threads_no_mark_assist_p160_c1000_100K_60000ms
```

最多 4 线程、Assist 开启的历史方向性对照：

```text
MSGC_RESULT/Result/case27_http_complex_json_msgc_high_p/result/
20260724_223606_after_mark_sweep_helper_policy_p160_c1000_100K_60000ms
```

每个结果目录中的关键文件为：

- `msgc/run_metadata.env`：SDK、Runtime MD5 和运行参数；
- `msgc/gc.log`：ConcurrentMark、Worker 和 Assist 原始日志；
- `msgc/compile.exitcode`、`msgc/run.exitcode`：编译与运行退出状态；
- `msgc/dyld_runtime_path.txt`：实际加载的 Runtime；
- `report.md` 和 PNG：脚本生成的业务、CPU、RSS 图表；由于退出 139，仅作为故障现场，不作为完整 60 秒性能报告。
