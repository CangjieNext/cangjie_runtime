# Step49 MSGC 高核心规模扩展与内存吞吐优化方案

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 修复 MSGC 在 Linux AArch64 128 核高并发场景中的 Mark 吞吐失配、堆阈值震荡、物理页归还滞后和分配器规模随 Processor 线性膨胀问题，在控制 RSS 的同时恢复可扩展吞吐。

**Architecture:** 将 Scheduler Processor、MSGC Mark Worker、Sweep Helper、Allocator Shard、初始 Segment 和 Scavenge 水位拆成独立策略；后台 Mark 按 Processor 上限和实际 backlog 动态扩容，Heap/Scavenge 使用受最大堆约束的稳定水位，Allocator 只按实际并发需求创建有限 shard/Segment。所有新策略只在 `MRT_USE_MARK_SWEEP` 下生效，不改变 CopyGC。

**Tech Stack:** C++14、Cangjie Runtime MarkSweep GC、Linux AArch64、SATB/Mutator Assist、Region/Segment allocator、CMake Runtime tests、case27 HTTP benchmark。

---

## 1. 文档状态、范围与结论边界

> 状态：**Phase A（Mark/Sweep helper 策略）源码、构建和 SDK Runtime 端到端激活已验证；P160 压测仍在 OOM 后退出 139，性能验收未通过。Phase B–E 未实施。**
>
> 问题状态：`open（Phase A 已生效，规模问题未修复）`。当前证据足以确认高核 Mark 宽度和 Scavenge 水位是两个独立瓶颈；2026-07-24 已完成“仅扩大 MSGC Processor 上限”的实验 Runtime 重建、macOS 放大复现、Phase A 源码修改、SDK Runtime 安装和一次 60 秒请求回归。该回归约 12 秒即在 OOM 后退出 139，Phase B–E 和 LLT/HLT 仍未完成，因此不能标记为 `verified`。

本文只处理 MSGC 在高核心 Linux 服务器上的规模扩展，不修改 CopyGC 语义，也不把 AArch64 特有指令优化与 GC 算法调度混在一起。策略实现应能用于 Linux AArch64、Linux X86-64 等高核目标；OS 物理页释放仍由现有 `ReleaseUnits()` 平台实现负责。

本次源码审计基线：

- 本地 Runtime：`MSGC-bugfix`，`d1ea79c59bbdb06104f7d53e6f5f6301aa3cae78`。
- 远端 Linux AArch64 源码工作树：`MSGC-bugfix`，检查时 HEAD 为 `530ff16d8e76bdf1d2179244319d4e097f7b54e5`。
- 短时矩阵实际加载的 Runtime：`libcangjie-runtime.so` MD5 `e0e6e424ce0c36c18d7b617a67c8f4be`。
- 远端源码 HEAD 与已安装 `.so` 没有可验证的构建清单绑定，因此短测结论按 Runtime MD5 固定，不宣称该二进制必然由远端当前 HEAD 构建。

本文最初分析阶段没有修改源码、SDK 或已有结果；本轮 Phase A 只修改 Runtime 源码、聚焦测试和文档，未安装或重建 SDK。

## 2. 问题现象与触发条件

### 2.1 机器与拓扑

问题机器为 Linux AArch64 Kunpeng 920：

- 128 个逻辑 CPU。
- 2 个 socket。
- 4 个 NUMA node，每个 node 32 CPU。
- CPU 支持 AArch64 LSE `atomics`；LSE 是否启用属于发码性能问题，不是本文规模扩展根因。

### 2.2 主要用例

`case27_http_complex_json` 的问题参数：

```text
payload=100K
concurrency=1000
duration=60000ms
cjHeapSize=16gb
cjMSMaxVirtualHeapSize=16gb
cjHeapGrowth=1
cjScavengeGrowth=5
cjMSMinGCThreshold=20mb（脚本记录项；本地当前源码仍使用固定 20MB floor）
cjProcessorNum=auto（128）
cjGCThreads=auto（当前解析为 4）
```

60 秒结果目录：

```text
/home/lipeisheng/cangjie/cangjie_runtime/runtime/MSGC_RESULT/Result/
case27_http_complex_json/result/
20260724_111428_output_msgc_vs_copygc_go_c1000_p100K_compare
```

该轮已确认：

- `nofile` effective 为 `1048576`，不是文件描述符耗尽。
- CopyGC 吞吐约 `452 req/s`，MSGC 约 `376 req/s`。
- MSGC 平均 RSS 约 `15532.74MB`，峰值约 `16908.47MB`。
- MSGC 平均 CPU 约 `2825.92%`，峰值约 `5219%`，远未稳定转化为有效吞吐。
- MSGC 共 26 个 GC cycle，GC wall time 合计约 `61372ms`，平均约 `2360ms`，最大约 `5671ms`。
- 大量 `socket closed`/`broken pipe` 是服务端长 GC 和请求超时的结果，不是本轮根因。

### 2.3 典型 GC 震荡

日志呈现反复的 HEU/OOM 配对：

```text
HEU:
  mark-start active ≈ 278MB
  ConcurrentMark ≈ 4.36s
  mark-end active ≈ 16.87GB
  mark bitmap live ≈ 14.85GB

紧随其后的 OOM:
  ConcurrentMark ≈ 341ms
  mark bitmap live ≈ 603MB
  PostMark ≈ 3.80s
  active heap 降到约 1.12GB
```

后续多轮重复出现：

```text
HEU:  trueLive/active 冲到约 12.8～16.8GB
OOM:  trueLive 回到约 0.48～0.63GB
```

这说明业务真实稳定 live set 并没有接近 16GB；长时间并发标记期间产生的 SATB snapshot/floating garbage 放大了本轮 mark bitmap live，随后阈值控制器又把这个瞬时值当作下一轮容量基线。

## 3. 受控验证结果

所有短测使用同一个远端 MSGC SDK、相同 Runtime MD5、相同 payload/concurrency/heap，只改变目标变量。短测时长为 15 秒，绝对吞吐受预热影响，主要用于判断方向和 GC 形态。

### 3.1 Processor 数矩阵

固定默认 `cjGCThreads=4`：

| `cjProcessorNum` | MSGC 吞吐 | 成功 | 失败 | 平均 RSS | 峰值 RSS | GC 次数 |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 32 | 386 req/s | 9212 | 1006 | 14005.43MB | 16920.63MB | 17 |
| 64 | 706 req/s | 13396 | 115 | 13332.06MB | 16857.87MB | 27 |
| 128 | 584 req/s | 10332 | 414 | 15070.71MB | 16962.30MB | 20 |

结果目录：

```text
20260724_155609_diag_msgc_scale_p32_20260724_c1000_p100K_compare
20260724_155703_diag_msgc_scale_p64_20260724_c1000_p100K_compare
20260724_155750_diag_msgc_scale_p128_20260724_c1000_p100K_compare
```

结论：

1. 在当前策略下，P=128 已越过有效吞吐拐点；增加 Mutator 分配并发没有带来吞吐增长。
2. P=32 又不足以承载 concurrency=1000，因此不能把整个 Scheduler 简单固定为 32。
3. P=64 是本轮短测的较优工作点，但这只是临时运行参数候选，不是 Allocator shard 的最终默认值。

### 3.2 P=128 下 Mark 线程矩阵

复用同一个 P=128 MSGC 二进制，只调整 `cjGCThreads`：

| `cjGCThreads` | 吞吐 | 成功率 | ConcurrentMark 平均 | ConcurrentMark 最大 | 最大 trueLive | 最大 active heap | 最大 RSS |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 4 | 324 req/s | 83.5% | 633ms | 3.44s | 13.73GiB | 15.75GiB | 16.47GiB |
| 8 | 489 req/s | 94.8% | 222ms | 1.38s | 7.65GiB | 15.04GiB | 16.45GiB |
| 16 | 660 req/s | 96.9% | 168ms | 0.90s | 5.91GiB | 11.43GiB | 16.55GiB |

结果目录：

```text
/home/lipeisheng/cangjie/cangjie_runtime/runtime/MSGC_RESULT/Result/
case27_http_complex_json/result/20260724_diag_msgc_gcthreads_p128
```

从 4 增加到 16 个总 Mark 线程后：

- 吞吐提升约 `104%`。
- 最大 ConcurrentMark 时间下降约 `74%`。
- 最大 mark bitmap live 从 `13.73GiB` 降到 `5.91GiB`。
- 最大 active heap 从 `15.75GiB` 降到 `11.43GiB`。
- 最大 RSS 基本不变，仍约 `17GiB`。

因此必须把问题拆成两部分：

1. **Mark 线程扩展**解决 ConcurrentMark 追不上分配、Assist 侵占业务线程和吞吐下降。
2. **Scavenge/retained 水位**解决 active heap 已下降但物理页仍不归还的问题。

## 4. 完整因果链

当前实现的完整反馈链如下：

```text
128 Scheduler P
  ├─ 约 128 个分配 owner/TLS buffer 并行产生对象
  └─ Allocator shardCount 同步扩到 128
                │
                ▼
默认 Mark 总线程仍固定为 4
                │
                ▼
后台标记吞吐追不上分配和写屏障 publish
                │
                ├─ Mutator Assist 占比升高，业务 CPU 被用于标记
                └─ ConcurrentMark 从数百毫秒拉长到数秒
                │
                ▼
长 Mark 窗口累积 SATB snapshot/floating garbage
                │
                ▼
mark bitmap live 被放大到 12～15GB
                │
                ▼
live safety floor 把 heap threshold 推到接近 16GB
                │
                ▼
cjScavengeGrowth=5 产生 40～80GB Scavenge threshold
                │
                ├─ Scavenge 水位高于最大堆，物理页不及时释放
                └─ 堆继续增长直到 Region 获取触发 OOM GC
                │
                ▼
OOM 周期得到约 0.5GB 的新 snapshot live
                │
                ├─ threshold 立即跌回约 1GB
                ├─ force-finish 大量 Unswept/缓存
                └─ PostMark/Sweep 耗时 1.5～4.9s
                │
                ▼
再次进入 HEU 膨胀周期，形成吞吐、延迟和 RSS 震荡
```

需要避免把下游症状当成单点根因：

- OOM PostMark 很慢是堆先膨胀到极限后的结果。
- Assist 已启用，不是“没有 Assist”，而是后台 Mark 宽度不足后过度依赖 Assist。
- 初始 8GB committed/current capacity 不等于启动即有 8GB RSS，但扩大了 shard 拓扑和周期扫描范围。
- 单纯增加 Mark 线程不会自动释放已经驻留的空闲物理页。

## 5. 当前源码根因审计

### 5.1 实施前默认 Mark 总线程被硬限制为 4

文件：`src/Heap/MCollector/MSMarkWorkerPolicy.h`

```cpp
inline size_t ResolveMSMarkTotalThreads(size_t processorCount, size_t configuredThreads)
{
    size_t processors = std::max<size_t>(1, processorCount);
    if (configuredThreads != 0) {
        return std::max<size_t>(1, std::min(configuredThreads, processors));
    }
    return std::min<size_t>(4, std::max<size_t>(1, processors / 2));
}
```

实施前 P=8、32、64、128 最终都只得到 4 个总 Mark 线程。`RunConcurrentMarkLoop()` 会动态启用 `gcThreads - 1` 个 helper，但策略上限已经只有 3 个 helper，因此 backlog 再高也不能继续扩容。

Phase A 已把自动解析改为同时接收 Scheduler P 和 logical CPU，自动宽度以
`min(processorCount, logicalCpuCount)` 为基数并上限为 16；显式 `cjGCThreads`
仍优先并 clamp 到 Scheduler P。这样真实 128 核/P=128 默认得到 16 个总 Mark
线程，而 8 核 macOS 即使诊断性设置 P=160，自动值仍保持 4，避免把 Scheduler
规模误当成真实可用 CPU。

### 5.2 Mark 与 Sweep 共用同一个线程宽度参数

文件：

- `src/Heap/MCollector/MarkSweepCollector.cpp`
- `src/Heap/MCollector/BackgroundProcessor.cpp`
- `src/Heap/MAllocator/MSParallelSweepPolicy.h`

Mark 使用：

```cpp
maxHelpers = gcThreads > 1 ? gcThreads - 1 : 0;
```

Sweep 使用：

```cpp
size_t logicalMaxHelpers = gcThreads > 1 ? gcThreads - 1 : 0;
```

如果直接把默认 `gcThreads` 从 4 提到 16，HIGH pressure Sweep 也可能借用 15 个 helper。Mark backlog 与 Sweep Region backlog 的粒度、锁竞争和内存带宽特征不同，不能继续把两者默认宽度完全绑定。

### 5.3 Assist 已工作，但周期间上涨过慢

文件：`src/Heap/MCollector/MSAssistPacing.h`

Assist base ratio 上涨时被限制为：

```text
next <= previous + max(1, previous / 4)
```

在 60 秒问题日志中，负载突增时 raw base 已到 106，而已发布 base 只能从 13 增加到 16；后续按 `16→20→25→31→38→47→58→72` 逐周期追赶。大周期中 Assist 已处理约 20%～61% 的对象，部分短测周期达到 70%～86%。

这证明：

- 不能把问题描述成“Assist 没启用”。
- 先增加后台 Mark capacity，比继续强迫 Mutator Assist 更有利于业务吞吐。
- 后续仍应允许在 headroom 紧张时绕过普通 25% 周期限速，但它是第二层背压，不是第一修复。

### 5.4 Heap Threshold 把瞬时 snapshot live 当成稳定 live

文件：

- `src/Heap/MCollector/MSHeapThresholdPolicy.h`
- `src/Heap/MCollector/MarkSweepCollector.cpp::UpdateGCStats()`

当前规则：

```text
rawTarget       = live * (1 + heapGrowth)
riseLimit       = oldThreshold * 1.125
liveSafetyFloor = live * 1.25
allowed         = max(riseLimit, liveSafetyFloor)
```

正常上涨会被 12.5% 限制，但 `liveSafetyFloor` 会在本轮 mark bitmap live 突然变大时直接越过限速。P=128/4 Mark 线程短测的最大 trueLive 为 13.73GiB，因此阈值被推到 16GiB cap；下一轮 OOM 观察到较小 live 后又立即下降。

源码注释称“current marking phase allocations do not inflate trueLive”。从 mark bit 定义看，新分配对象本身不是简单按分配量全部计入；但 SATB 下在长并发窗口中曾经可达的对象会形成 floating garbage，因此该值仍不能等同于 PostSweep 稳定存活集。

### 5.5 Scavenge Threshold 没有最大堆约束

文件：

- `src/Heap/Collector/GcStats.cpp`
- `src/Heap/MCollector/MarkSweepCollector.cpp`

初始化和每轮更新各自保留了一份相同逻辑：

```cpp
scavengeThreshold = heapThreshold * scavengeGrowth;
```

只处理了 `size_t` 溢出，没有用 `RegionSpaceMS::GetMaxCapacity()` 做上限。`heapThreshold=16GiB`、`cjScavengeGrowth=5` 会得到 80GiB，永远高于 16GiB 最大堆。P=128/16 Mark 线程虽然把最大 active heap 降到 11.43GiB，但本轮 Scavenge threshold 仍最高达到 36.92GiB，因此最大 RSS 没有下降。

### 5.6 Shard 和初始 Segment 随 P 线性扩张

文件：

- `src/Heap/MAllocator/MSShardRouting.h`
- `src/Heap/MAllocator/MSRegionManager.cpp`
- `src/Heap/MAllocator/MSFreeManager.cpp`

当前关系：

```text
shardCount = processorNum
segmentSize = 64MB
initialCommitted = shardCount * 64MB
initialCommittedSegments = shardCount
```

| P | shard | 初始 Segment | 初始 current/committed capacity |
| ---: | ---: | ---: | ---: |
| 32 | 32 | 32 | 2GiB |
| 64 | 64 | 64 | 4GiB |
| 128 | 128 | 128 | 8GiB |

8GiB 是已纳入 allocator 拓扑的 current capacity，不等于立即产生 8GiB RSS。启动 RSS 约 43MB，说明 object pages 仍由 OS lazy commit；但初始化会触碰 262144 个 `MSRegionInfo` 中的初始 committed 范围，并建立 128 个 shard/owned Segment 视图。

`FreeManager` 已允许 shard 的 `ownedViews_` 为空，也支持请求方按 home shard grow；但单 Region steal 默认只尝试 2 个 donor。因此减少初始 Segment 时，必须同时验证空 shard 的首次 grow 和 donor 发现，不能只改乘法公式。

### 5.7 周期维护按 committed Region 全量扫描

文件：`src/Heap/MAllocator/MSCentralRegionPool.cpp::ClearAllMarkBitmaps()`

```cpp
size_t maxRegions = msRegionManager_->GetMaxRegionCount();
for (size_t i = 0; i < maxRegions; i++) {
    // 检查 Region type，再清 bitmap/large mark
}
```

`GetMaxRegionCount()` 当前返回 committed Region 数：

- 初始 P=128/8GiB：262144 个 Region。
- 扩展到 16GiB：524288 个 Region。

问题日志中堆扩展后 `PreEnumRoots` 可达到约 200～376ms。全 committed 扫描不是长 ConcurrentMark 的首要根因，但会放大高容量堆的 STW 基线。

## 6. 设计原则与策略边界

1. **CopyGC 行为不变。** 新策略头、参数默认值和调用只放在 `#ifdef MRT_USE_MARK_SWEEP` 分支。
2. **不把 Scheduler P 当成唯一规模旋钮。** Processor、Mark、Sweep、Allocator shard、initial Segment 必须分别解析。
3. **默认值是最大能力，不是持续占核数。** Mark/Sweep 仍按 backlog/pressure 动态激活 helper。
4. **先扩后台 Mark，再增强 Assist。** Assist 是分配背压和正确性保障，不应成为高核服务器的主要 collector。
5. **内存控制必须以最大堆为硬边界。** 任意派生阈值不得高于本模式可达到的最大堆/RSS 水位。
6. **瞬时 floating live 不能单独驱动长期水位。** 同时观察 mark-start、mark-end、allocation delta、PostSweep retained。
7. **正确性协议不因性能优化改变。** Mark CAS、trusted-grey WorkBuf、one-way quiesce、Sweep generation/claim 和 scavenged bit 不变式继续保留。
8. **每个 Phase 可独立构建、测试和回滚。** 禁止一次提交同时修改 Mark、Threshold、Shard 和 Region 索引。

## 7. Phase A：扩展 Mark 吞吐并拆分 Sweep 宽度

### 7.1 默认 Mark 线程策略

`ResolveMSMarkTotalThreads()` 已修改为：

```cpp
inline size_t ResolveMSMarkTotalThreads(size_t processorCount, size_t logicalCpuCount,
                                        size_t configuredThreads)
{
    size_t processors = std::max<size_t>(1, processorCount);
    if (configuredThreads != 0) {
        return std::max<size_t>(1, std::min(configuredThreads, processors));
    }
    size_t availableCpus = std::min(processors, std::max<size_t>(1, logicalCpuCount));
    if (availableCpus < 8) {
        return std::max<size_t>(1, availableCpus / 2);
    }
    size_t scaled = (availableCpus + 7) / 8;
    return std::min(availableCpus, std::min<size_t>(16, std::max<size_t>(4, scaled)));
}
```

预期默认值：

| `min(P, logical CPU)` | 实施前 | 新策略 |
| ---: | ---: | ---: |
| 1 | 1 | 1 |
| 4 | 2 | 2 |
| 8 | 4 | 4 |
| 32 | 4 | 4 |
| 64 | 4 | 8 |
| 128 | 4 | 16 |
| 256 | 4 | 16 |

显式 `cjGCThreads` 优先级保持不变，仍 clamp 到 `[1, processorNum]`。

`MSMarkWorkerPolicy` 不再包含 `kEmergencyBacklog`、增长采样或 `EnterTail()`。
ConcurrentMark 有可用 helper 时至少启用 1 个，每 8 个 published WorkBuf 对应
1 个目标 helper：

```text
desiredHelpers = max(1, ceil(publishedBacklog / 8))
activeHelpers = max(activeHelpers, min(maxHelpers, desiredHelpers))
```

因此它没有离散的 Mark LOW/MEDIUM/HIGH 状态，也不再逐次只增加一个 helper；
单周期仍只增不减，下一轮重新从基础宽度开始。

### 7.2 Sweep 默认上限独立

修改 `ComputeMSMaxSweepHelpers()`：

```cpp
constexpr size_t kMSDefaultMaxSweepHelpers = 8;

inline size_t ComputeMSMaxSweepHelpers(size_t gcThreads, size_t configuredMaxHelpers)
{
    size_t logicalMax = gcThreads > 1 ? gcThreads - 1 : 0;
    size_t policyCap = configuredMaxHelpers == 0
        ? kMSDefaultMaxSweepHelpers
        : configuredMaxHelpers;
    return std::min(logicalMax, policyCap);
}
```

含义：

- `cjMSParallelSweepMaxHelpers=0` 从“等于 `gcThreads-1`”改为“自动上限，默认最多 8”。
- 显式非零值仍允许用户覆盖，但不会超过 `gcThreads-1`。
- Mark 16 线程时 Sweep 不再自动拉到 15 helper。

需要同步更新 `src/Cangjie.h` 中参数注释和相关 Runtime 文档。

### 7.3 Phase A 验收条件

- P=1/4/8 默认值与现有行为一致。
- P=64/128 分别解析为 8/16。
- 显式配置仍覆盖默认值。
- `MSMarkWorkerPolicy` 按每 8 个 published WorkBuf 比例扩容，cycle 内只增不减。
- convergence candidate 不修改 helper/assist 目标，直接进入 one-way quiesce。
- Sweep HIGH pressure 的 active helper 不超过自动 8 或显式 cap。
- P=128 case27 下 Mark 最大时间、Assist share、成功率和吞吐相对 4 线程基线改善。

## 8. Phase B：建立受最大堆约束的 Scavenge 水位

> 2026-07-26 实施更新：最终没有采用本节原先的固定倍数与 80% max-heap cap 方案，也没有新增
> `MSScavengeThresholdPolicy.h`。MSGC 现在在现有 `MSHeapThresholdPolicy.h` 内维护最近 64 轮
> `markEndInUse`/`inUseBurst` 历史。FreeManager 在已 Scavenge Region 被重新分配时累计实际 cold
> Region 重用数，GC 取相邻快照差得到 `coldReuseBytes`。`residentReserve` 以 `recentBurst`
> 为下限：cold 重用时立即增加，连续 32 个无重用周期后每轮按剩余差值的 1/64 缓慢下降。
> 候选水线为 `min(recentPeakInUse + residentReserve, maxHeap)`。`inUse` 通过
> `committed - free - scavenging - emptySwept - largeHotCache` 计算，包含 TLS active Region。
> 后台释放缺口使用 `retained - max(scavengeThreshold, currentInUse)`。
> `cjScavengeGrowth` 对 MSGC 仅保留参数兼容性，不再参与计算。下文固定倍数/80% cap 内容保留为
> 历史备选方案，不代表当前实现。

### 8.1 提取统一纯策略

新增 `src/Heap/MCollector/MSScavengeThresholdPolicy.h`：

```cpp
namespace MapleRuntime {

constexpr size_t kMSScavengeMaxHeapPercent = 80;

struct MSScavengeThresholdDecision {
    size_t rawThreshold = 0;
    size_t maxHeapWatermark = 0;
    size_t threshold = 0;
    bool capped = false;
};

MSScavengeThresholdDecision EvaluateMSScavengeThreshold(
    size_t heapThreshold, double scavengeGrowth, size_t maxHeapBytes);

} // namespace MapleRuntime
```

策略：

```text
raw = saturatingMultiply(heapThreshold, max(scavengeGrowth, 1.0))
maxHeapWatermark = floor(maxHeapBytes * 80 / 100)
threshold = min(raw, maxHeapWatermark)
```

若 `maxHeapBytes==0`，仅使用 raw，避免初始化顺序不完整时产生 0 阈值。80% 是第一版内部安全水位，不新增公共 `GCParam` ABI 字段；后续只有正式数据证明需要用户配置时再设计独立兼容接口。

### 8.2 统一两个调用点

必须同时替换：

1. `src/Heap/Collector/GcStats.cpp::GCStats::Init()` 的初始 Scavenge threshold。
2. `src/Heap/MCollector/MarkSweepCollector.cpp::UpdateGCStats()` 的周期更新。

两个位置都使用实际 allocator 最大容量：

```text
maxHeapBytes = Heap::GetHeap().GetMaxCapacity()
```

不能用当前本地源码的 `GCParam::gcThreshold` 代替 max heap：该字段仍由初始 committed
heap 派生，Phase D 默认只保留一个 initial Segment 后它可能只有 64MiB。CopyGC 保留
原有 `GcStats.cpp` 计算；通过 `#ifdef MRT_USE_MARK_SWEEP` 选择新纯策略，不能把 MSGC
80% 水位无条件施加给 CopyGC。

### 8.3 诊断字段

`heapThreshold` REPORT 增加：

```text
rawScavengeThreshold
maxHeapScavengeWatermark
newScavengeThreshold
scavengeCapped
```

当 `rawScavengeThreshold > maxHeapBytes` 时至少输出一次 REPORT 警告，便于识别类似 `80GB threshold / 16GB heap` 的失效配置。

### 8.4 Phase B 验收条件

- 16GiB 最大堆下，任何 MSGC Scavenge threshold 不超过 12.8GiB。
- `cjScavengeGrowth=1.5` 且 heap threshold=4GiB 时保留 6GiB，不错误命中 cap。
- 乘法溢出时先饱和，再被最大堆水位 clamp。
- active heap 降低后，hot-free/cold-free 转换和 RSS 在有限时间内回落。
- Scavenge `try_lock`、`scavengingBits` 和锁外 `ReleaseUnits()` 协议不变。

## 9. Phase C：稳定 Heap Threshold 与高压收敛

Phase C 在 Phase A/B 稳定后实施，避免用阈值控制掩盖 Mark capacity 问题。

### 9.1 增加周期输入

在 `MSGCCycleStats` 增加：

```cpp
size_t markStartActiveBytes = 0;
size_t markEndActiveBytes = 0;
size_t allocationDuringMarkBytes = 0;
bool floatingLiveInflated = false;
```

采集位置：

- `PreGarbageCollection()`：通过 `RegionSpaceMS::GetHeapInUseBytes()` 记录 `markStartActiveBytes`。
- `UpdateGCStats()`：记录 `markEndActiveBytes`。
- `allocationDuringMarkBytes = max(markEndActiveBytes - markStartActiveBytes, 0)`。

### 9.2 floating-live 分类策略

在 `MSHeapThresholdPolicy.h` 新增纯函数：

```cpp
inline bool IsMSFloatingLiveInflated(size_t markStartActiveBytes,
                                     size_t markEndActiveBytes,
                                     size_t markBitmapLiveBytes,
                                     size_t maxHeapBytes)
{
    size_t start = std::max<size_t>(markStartActiveBytes, 1);
    bool grewFourTimes = markEndActiveBytes / start >= 4;
    bool consumedQuarterHeap =
        markEndActiveBytes > markStartActiveBytes &&
        markEndActiveBytes - markStartActiveBytes >= maxHeapBytes / 4;
    bool liveTracksGrowth = markBitmapLiveBytes > markStartActiveBytes * 2;
    return grewFourTimes && consumedQuarterHeap && liveTracksGrowth;
}
```

乘法比较在实际实现中必须改成防溢出的除法/饱和乘法。三个条件同时成立才分类，避免普通 live set 增长被错误抑制。

### 9.3 稳定阈值基线

`GCStats` 增加 `lastStableLiveBytes`。更新规则：

```text
普通周期:
  lastStableLive = markBitmapLive
  thresholdBase  = markBitmapLive

inflated 周期:
  thresholdBase = max(lastStableLive, markStartActiveBytes)
  不用本轮放大的 markBitmapLive 覆盖 lastStableLive
```

inflated 周期仍用现有 `EvaluateMSHeapThreshold()` 计算，但传入 `thresholdBase`。当前实现只稳定
threshold 基线，不额外安排 stabilization GC，避免在已经存在 pending/running 去重的异步 GC 队列上
再引入一套周期状态。

### 9.4 WorkBuf debt 驱动的并发 Mark 压力与 STW

不使用 Mark 期间分配速率、时间窗口、上一轮增长量乘二或 active heap 百分比推导 deadline。
GC main 只统计自己完整处理的 WorkBuf；每完成 100 个 WorkBuf 做一次 checkpoint：

```text
debt = Global Full WorkBuf + Global Partial WorkBuf
backlogLevel = ceil(debt / 100)

若本窗口 debt 没有下降:
  noProgressWindows += 1
  pressureLevel += 1
否则:
  noProgressWindows = 0

targetHelpers = min(pressureLevel, maxHelpers)
assistMultiplier = min(max(pressureLevel, 1), cjMSAssistMaxMultiplier)
```

Helper 宽度和 Assist ratio 只接受同一个单调不降的 `pressureLevel`，不再各自使用 backlog/EMA
判定。OOM cycle 从一开始就使用最大 Helper 宽度和最大 Assist multiplier。

以下条件沿已有 one-way quiesce 进入 STW `MarkTermination`：

- checkpoint 时 `debt <= 32`；
- 连续 3 个 checkpoint 的 debt 没有净下降；
- Small Region 在中央池、unswept 和 committed FreeManager 供给均未命中，即将 grow；
- Large allocation 的 committed contiguous Region 供给未命中，即将 grow。

Small/Large 由 `TakeRegion()` 与 `TakeRegionForLarge()` 的调用路径显式传递，不能按 Region 数量
猜测；一个只占 1 个 Region 的大对象仍属于 Large。grow 请求只通知 GC main 并唤醒其等待，
Mutator 随后继续原有 grow/retry，不同步等待 STW。

自然 fixed point 仍优先正常退出；原有 `cjMSMarkTimeBudget` 仅作为独立固定时间兜底。

### 9.5 Phase C 验收条件

- 正常稳定 live 增长仍遵守现有 12.5% rise limit 和 25% live headroom。
- 问题日志形态 `278MB→16GB/14GB live` 被分类为 inflated。
- 普通 `1GB→2GB/1.5GB live` 不因单一条件误分类。
- 每 100 个 GC-main WorkBuf 才更新一次 pressure，Helper 与 Assist 使用同一级别。
- `debt <= 32`、连续 3 个无净下降窗口以及 grow-before 条件都只复用现有
  `forceConverged`/MarkTermination 正确性路径。
- 不读取 active heap 百分比，不增加基于时间的分配速率采样。
- 60 秒日志不再出现 threshold 在约 1GiB 与 16GiB 间交替跳变。

## 10. Phase D：方案 A——拆分 Processor、Shard 与初始 Segment

### 10.1 P→Shard：三次根目标 + 整除约束

2026-07-26 确认以下正式设计目标：

- P=1/2/4/8 时，默认一个 P 对应一个 shard。
- P=16/128/1024 时，分别使用 8/16/32 个 shard。
- P 增长 8 倍时，目标 shard 数只增长 2 倍，即按三次根速度增长。
- `processorsPerShard = P / shardCount` 必须是整数；所有 shard 绑定完全相同数量的 P。
- Processor 到 shard 沿用取模路由：`processorId % shardCount`。因为
  `P % shardCount == 0`，每个 shard 恰好接收 `P / shardCount` 个 Processor。

先计算连续目标：

```text
P <= 8:
  targetShard = P

P > 8:
  targetShard = ceil(cuberoot(32 * P))
```

再把目标向下规整为 P 的因数：

```text
shardCount = max { d | 1 <= d <= targetShard 且 P % d == 0 }
processorsPerShard = P / shardCount
```

不能使用浮点 `std::cbrt()` 后直接取整；实现使用 `uint64_t` 整数立方根或二分查找，
避免立方边界因浮点误差选错档位。`32 * P` 也先提升到 `uint64_t`。

典型结果：

| P | 连续目标 | 最终 shard | P/shard |
| ---: | ---: | ---: | ---: |
| 1 | 1 | 1 | 1 |
| 2 | 2 | 2 | 1 |
| 4 | 4 | 4 | 1 |
| 8 | 8 | 8 | 1 |
| 16 | 8 | 8 | 2 |
| 32 | 11 | 8 | 4 |
| 64 | 13 | 8 | 8 |
| 128 | 16 | 16 | 8 |
| 256 | 21 | 16 | 16 |
| 512 | 26 | 16 | 32 |
| 1024 | 32 | 32 | 32 |
| 2048 | 41 | 32 | 64 |
| 4096 | 51 | 32 | 128 |
| 8192 | 64 | 64 | 128 |

严格整除存在一个不可回避的边界：P=127 这类质数在目标值以下只有因数 1，因此默认会
退化为 1 个 shard。这是“所有 shard 完全均分 P”优先于“接近三次根目标”的确定结果，
不能偷偷改成 15/16 个 shard。需要避免该退化时，运维可显式选择能整除的 P，或显式设置
`cjMSShardCount=P`。

### 10.2 策略函数与显式配置

在 `MSShardRouting.h` 增加：

```cpp
constexpr uint32_t kMSLowProcessorDirectShardLimit = 8;
constexpr uint32_t kMSDefaultInitialSegmentCount = 1;

uint32_t ComputeMSShardTarget(uint32_t processorCount);
uint32_t FindLargestMSShardDivisor(uint32_t processorCount,
                                  uint32_t targetShardCount);
uint32_t ResolveMSShardCount(uint32_t processorCount,
                            uint32_t configuredShardCount);

inline uint32_t ResolveMSInitialSegmentCount(uint32_t configured)
{
    return configured == 0
        ? kMSDefaultInitialSegmentCount
        : std::max<uint32_t>(1, configured);
}
```

`configuredShardCount=0` 使用默认曲线；非 0 时，先 clamp 到 `[1, P]`，再向下规整为
P 的最大因数。例：P=128、显式配置 24，最终得到 16，并在启动日志同时打印
`configured=24, resolved=16`，不能静默造成不均分。显式配置 P=128 可恢复 128 个 shard。
当前 shard ID 使用 `uint16_t`，且 `UINT16_MAX` 保留为 invalid ID，因此 resolver 必须把
候选 shard count 限制在可表示的有效数量内，再选择 P 的因数；显式配置不能截断后静默
产生重复 shard ID。

为避免修改公共 `GCParam` ABI，两个诊断值在 Runtime 参数解析阶段通过
`Base/EnvParam.h::ReadCountParameter()` 读取，并保存到 `CangjieRuntime` 的内部
MSGC scale config：

```text
cjMSShardCount
cjMSInitialSegmentCount
```

环境变量保留为诊断/回退开关。`MSAllocatorScaleConfig` 同时保存
`processorsPerShard`，启动时检查：

```text
processorCount >= 1
shardCount >= 1
processorCount % shardCount == 0
processorsPerShard == processorCount / shardCount
```

`MSRegionManager::ResolveHomeShardId()` 必须把 `freeManager_.GetShardCount()` 写入
`MSShardRouteContext::shardCount`，不能继续重新读取
`GetConcurrencyModel().GetProcessorNum()`。后者是 P，不是解析后的 shard 数；如果仍传 P，
`processorId % shardCount` 会退化回一 P 一 shard，并使 steal 接口收到越界 receiver ID。

### 10.3 初始只提交一个 Segment

当前 `CjScheduler.cpp` 和 `CangjieRuntimeApi.cpp` 的
`ComputeMSInitialHeapSizeKB(processorNum)` 仍返回 `processorNum * 64MB`，同时
`GCParam::gcThreshold` 也由这个 initial heap 派生。只修改 `MSRegionManager` 会导致
Runtime 参数、RegionManager committed 范围和 GC threshold 三者失配。

需要统一修改：

- `src/CjScheduler.cpp`
- `src/CangjieRuntimeApi.cpp`
- `src/CangjieRuntime.h`
- `src/CangjieRuntime.cpp`
- `src/Heap/MAllocator/MSRegionManager.cpp`

两个 Runtime 入口先解析一次 shard/initial Segment，写入内部配置：

```cpp
struct MSAllocatorScaleConfig {
    uint32_t shardCount;
    uint32_t processorsPerShard;
    uint32_t initialSegmentCount;
};
```

默认：

```text
initialSegmentCount = 1
heapParam.heapSize = 64MB
MSRegionManager initial committed = 64MB
GC threshold cap = configured max virtual heap
```

`MSRegionManager::Initialize()` 不再使用：

```text
committedHeap = shardCount * 64MB
```

改为读取已经由 Runtime 入口保存的 `MSAllocatorScaleConfig`：

```text
scale = CangjieRuntime::GetMSAllocatorScaleConfig()
logicalShardCount = scale.shardCount
initialSegments = scale.initialSegmentCount
committedHeap = initialSegments * 64MB
```

这样可避免同一环境变量被两处独立解析。`CjScheduler.cpp` 和
`CangjieRuntimeApi.cpp` 必须共享同一个 resolver，保证普通 `cjc` 入口和显式
`InitCJRuntime(param)` 入口行为一致。

本地当前源码的 `GCParam::gcThreshold` 仍等于 initial heap；Phase D 必须同时改成
MSGC max virtual heap，否则 initial=64MiB 后 Heap Threshold 会被错误限制在 64MiB。
这个修改仍位于 `MRT_USE_MARK_SWEEP` 分支，CopyGC 的 `gcThreshold` 推导不变。

### 10.4 所有 Shard 均匀共享已提交 Segment

默认只有第 0 个 shard 拥有初始 Segment；其余 shard 没有 `ownedViews_`。空 local shard
不等于 committed heap 无可用空间，更不能直接触发 Grow。

single-region 路径按以下顺序执行：

1. 从 home shard 的 `borrowedSpans_` 获取 Region。
2. 从 home shard 自有 Segment 获取 Region。
3. 从全局 `nonEmptyFreeShardBits` 选择 donor，并沿用批量借用，首版保持现有每批最多
   4 个 Region，避免每次分配都争用初始 owner 锁。
4. bitmap 候选必须在 donor shard 锁内重新确认；陈旧的置位只算 donor miss。
5. 只有确认所有 committed owner shard 都没有 free Region 后，才进入 Grow。

`nonEmptyFreeShardBits` 使用多 word 原子位图，不能假设 shard 永远不超过 64。free Region
从 0 变为非 0 时发布 bit；从非 0 变为 0 时在 shard 锁内清 bit。选 donor 时从 home
shard 后一位轮转，避免所有空 shard 固定打到 shard 0。

large/contiguous 路径不使用 single-region 的“两次 donor 尝试”上限，而是遍历 bitmap
中的全部候选 owner，寻找满足长度的连续 run。只有 committed 范围内不存在满足要求的
连续 run 时，才 Grow 足够数量的连续 Segment。

Grow 必须经过 `growMutex_` 并二次检查 committed supply：记录 miss 时的
`committedGeneration`，拿到锁后若 generation 已变化，则重新执行 donor/contiguous
查询；仍然失败才真正扩展。这样多个空 shard 同时首次分配时不会各自无意义地增长一个
64MiB Segment。

新 Segment 的固定 owner 是触发 Grow 的 home shard。Grow 完成后更新
`segmentOwnerShard_`、owner view、free snapshot、non-empty bitmap 和
`committedGeneration`。借用只转移 Region 的临时使用权，不修改 Segment owner；
Region 回收时仍按 `segmentOwnerShard_` 返回原 owner。

### 10.5 NUMA 边界

Phase D 第一版只控制逻辑规模，不声称完成 NUMA-aware allocation。后续若引入 NUMA node：

- shard 应在 node 内优先选择。
- Segment grow 应优先绑定当前 Processor 所在 node。
- donor 先 node-local，再跨 node。

没有 `mbind/numa_alloc` 和拓扑证据前，不把“4 node × N shard”写成正式算法。

### 10.6 Phase D 验收条件

- 纯策略覆盖 P=1/2/4/8/16/32/64/128/256/512/1024，以及 P=127 质数边界。
- 对每个测试输入都断言 `P % shardCount == 0`，每个 shard 的 Processor 数完全相同。
- P=16/128/1024 分别解析为 8/16/32 shard；P=128 的 `P/shard=8`。
- P=128 默认只初始化 1 个 Segment，即 64MiB committed heap，而不是 8GiB。
- 只有 shard 0 初始拥有 Segment 时，全部 16 个逻辑 shard 都能完成首次
  single/large allocation。
- donor miss 不导致无意义地连续 grow，也不出现跨 shard 双重分配。
- 并发首次分配只在全局 committed supply 确实不足时 Grow。
- `allocBits/scavengedBits/scavengingBits` 不变式和 grow/shrink lock order 测试通过。
- 显式 `cjMSShardCount=processorNum`、`cjMSInitialSegmentCount=processorNum` 能恢复旧拓扑。

## 11. Phase E：消除 O(committedRegions) 的周期维护

Phase E 只在 A～D 稳定后实施，属于 STW 基线优化。

### 11.1 活跃 Region 快照

在 `CentralRegionPool` 增加 `CollectMarkBitmapRegionSnapshot()`，在以下前置条件下调用：

```text
Mutator 已 StopTheWorld
StopAndWaitForAllSweep 已完成
没有 Lazy/Assist/Helper Sweep in-flight
```

快照来源：

- 所有注册 `AllocBufferMS::activeRegions_[36]`。
- 36 个 bucket 的 partial/full swept/unswept 容器。
- large active list。
- MarkTermination 后仍属于当前 committed 生命周期的其他 active Region 容器。

Region index 使用临时 vector 收集，再按 index 排序去重；每个 GC cycle 分配一次临时空间，不在分配快路径维护新的共享链表。

### 11.2 替换与校验

`ClearAllMarkBitmaps()` 改为遍历快照：

```text
SMALL/PARTIAL/FULL active Region -> ClearMarkBitmap()
LARGE primary -> ClearMarkedFlag()
FREE/EMPTY_CACHE/SPAN -> 不进入快照
```

Debug/诊断构建保留原 committed-range reference scan：

- 比较快照应清集合和 reference 集合。
- 任一遗漏立即 FATAL 并打印 Region index/type/owner/container。
- Release 默认只走快照路径。

### 11.3 Phase E 验收条件

- active snapshot 与 reference scan 在小对象、large、local active、central cache、unswept、P 退出场景完全一致。
- committed 16GiB、active 1GiB 时，清理复杂度随 active Region 而不是 524288 committed entries 增长。
- `PreEnumRoots` 的 bitmap clear 时间和总 STW P95 明显下降。
- 诊断 reference scan 不进入 Release 默认热路径。

## 12. DFX 与可观测性

新增诊断必须是周期级或低频采样，禁止重新引入逐对象全局热原子。

### 12.1 启动策略

```text
[MS-GC] [scale-policy]
processors=128
markThreads=16
sweepAutoCap=8
configuredShardCount=0
targetShardCount=16
shardCount=16
processorsPerShard=8
initialSegments=1
segmentSize=64MB
initialCommitted=64MB
maxHeap=16GB
```

### 12.2 Mark 压力

每个 cycle 汇总：

```text
markStartActiveBytes
markEndActiveBytes
allocationDuringMarkBytes
concurrentMarkUs
configured/activePeakHelpers
gcMain/helper/assist processed
assistShare
forceConverged
completedMarkBuffers
markDebtBuffers
markPressureLevel
markNoProgressWindows
markTerminationReason
markGrowRequestRegions
```

### 12.3 Heap/Scavenge 控制

```text
markBitmapLiveBytes
lastStableLiveBytes
thresholdBaseBytes
floatingLiveInflated
rawHeapThreshold
newHeapThreshold
rawScavengeThreshold
maxHeapScavengeWatermark
newScavengeThreshold
retainedBefore/After
releasedBytes
```

### 12.4 Shard/Segment

启动输出有效策略；周期只汇总：

```text
committedSegments
segmentsPerShard min/max
donorHit/donorMiss
growCount/growBytes
crossShardBorrowedRegions
committedGeneration
```

这些计数使用 shard-local 或已有锁内普通计数，在 REPORT 周期汇总；不能让每次 Region take 都争用同一个全局 atomic。

## 13. 文件修改地图

| 阶段 | 文件 | 动作 | 职责 |
| --- | --- | --- | --- |
| A | `src/Heap/MCollector/MSMarkWorkerPolicy.h` | 修改 | P→默认 Mark 总线程解析 |
| A | `src/Heap/MAllocator/MSParallelSweepPolicy.h` | 修改 | Sweep 自动 helper cap 独立于 Mark 最大宽度 |
| A | `src/Cangjie.h` | 修改注释 | 更新 `gcThreads` 和 `msParallelSweepMaxHelpers` 默认语义，不改变结构布局 |
| A | `test/MSMarkWorkerPolicyTest.cpp` | 修改 | 覆盖 1/4/8/32/64/128/256 P 和显式 override |
| A | `test/MSParallelSweepPolicyTest.cpp` | 修改 | 覆盖自动 8 helper cap 和显式 cap |
| B | `src/Heap/MCollector/MSScavengeThresholdPolicy.h` | 新增 | MSGC Scavenge raw/clamp 纯策略 |
| B | `src/Heap/Collector/GcStats.cpp` | 修改 | MSGC 初始化 threshold 使用统一策略，CopyGC 保持原逻辑 |
| B | `src/Heap/MCollector/MarkSweepCollector.cpp` | 修改 | 周期 Scavenge threshold 和 REPORT 使用统一策略 |
| B | `test/MSScavengeThresholdPolicyTest.cpp` | 新增 | 正常比例、80% cap、0 max heap、溢出测试 |
| C | `src/Heap/MCollector/MSHeapThresholdPolicy.h` | 修改 | floating-live 分类和稳定基线决策 |
| C | `src/Heap/MCollector/MSMarkTerminationPolicy.h` | 新增 | WorkBuf debt、统一 pressure 和 STW 决策 |
| C | `src/Heap/MCollector/MarkSweepCollector.h` | 修改 | 增加周期 pressure/termination 采样和 grow 请求状态 |
| C | `src/Heap/MCollector/MarkSweepCollector.cpp` | 修改 | 每 100 个 GC-main WorkBuf checkpoint 和 one-way quiesce |
| C | `src/Heap/MAllocator/MSRegionManager.h/.cpp` | 修改 | Small/Large committed supply miss 的 grow-before 请求 |
| C | `src/Heap/Collector/GcStats.h` | 修改 | 保存 `lastStableLiveBytes` |
| C | `test/MSHeapThresholdPolicyTest.cpp` | 修改 | floating 分类、稳定基线、溢出边界 |
| C | `test/MSMarkTerminationPolicyTest.cpp` | 新增 | backlog、无进展、debt budget、OOM、Assist 饱和 |
| D | `src/Heap/MAllocator/MSShardRouting.h` | 修改 | 三次根目标、P 因数规整、P/shard 与单初始 Segment 纯策略 |
| D | `src/CjScheduler.cpp` | 修改 | 环境入口解析 MSGC scale config，分离 initial heap 与 max heap threshold |
| D | `src/CangjieRuntimeApi.cpp` | 修改 | 显式 API 入口使用同一 MSGC scale resolver |
| D | `src/CangjieRuntime.h` | 修改 | 保存内部 `MSAllocatorScaleConfig`，不改变公共 RuntimeParam ABI |
| D | `src/CangjieRuntime.cpp` | 修改 | 定义内部 scale config 状态 |
| D | `src/Heap/MAllocator/MSRegionManager.cpp` | 修改 | 读取已解析 scale config，按 initial Segment 初始化并用实际 shardCount 路由 |
| D | `src/Heap/MAllocator/MSFreeManager.h` | 修改 | 非空 donor bitmap 和统计字段 |
| D | `src/Heap/MAllocator/MSFreeManager.cpp` | 修改 | 空 shard donor/grow、bitmap 发布和回收 |
| D | `test/MSShardRoutingTest.cpp` | 修改 | 默认 cap、显式 override、initial Segment |
| D | `test/MSFreeManagerShardLocalTest.cpp` | 修改 | 无 owned Segment 的首次分配 |
| D | `test/MSFreeManagerShardStealTest.cpp` | 修改 | 稀疏 owner 下 donor 命中与 grow fallback |
| D | `test/MSFreeManagerGrowLockOrderTest.cpp` | 修改 | 空 shard 并发 grow lock order |
| D | `test/MSFreeManagerOwnerTest.cpp` | 修改 | steal 后 owner 固定不变 |
| E | `src/Heap/MAllocator/MSCentralRegionPool.h` | 修改 | 声明 active Region 快照/诊断 reference 接口 |
| E | `src/Heap/MAllocator/MSCentralRegionPool.cpp` | 修改 | 快照收集、去重、bitmap clear |
| E | `src/Heap/MAllocator/MSAllocBuffer.h` | 修改 | 提供 STW 下 active Region 只读访问 |
| E | `test/MSGCIncrementalRegionSnapshotContractTest.sh` | 修改 | 保证 Release 不回退 committed 全扫 |
| E | `test/MSCentralMarkBitmapSnapshotTest.cpp` | 新增 | 快照与 reference 集合一致性 |
| 集成 | `MSGC_RESULT/Result/case27_http_complex_json/run_compare_linux.sh` | 修改 | 仅向 MSGC 子进程透传 GC线程/Shard/Segment 参数并记录 metadata |
| 文档 | `MSGC_RESULT/Implementation/Step49_MSGC高核心规模扩展与内存吞吐优化方案.md` | 更新 | 记录实施状态、提交、验证结果和最终参数 |

不应修改：

- LLVM MSGC emitter、intrinsic、wrapper lowering。
- CopyGC collector/allocator 策略。
- 公共 `GCParam` 字段顺序和大小。
- MSRegionInfo、AllocBufferMS、WorkBuf 的 compiler fast-path ABI 偏移。

## 14. 分阶段 TDD 实施任务

### Task 1：动态 Mark 默认宽度与独立 Sweep cap（已实施）

**Files:**

- Modify: `src/Heap/MCollector/MSMarkWorkerPolicy.h`
- Modify: `src/Heap/MAllocator/MSParallelSweepPolicy.h`
- Modify: `src/Cangjie.h`
- Modify: `test/MSMarkWorkerPolicyTest.cpp`
- Modify: `test/MSParallelSweepPolicyTest.cpp`

- [x] **Step 1：先修改测试形成 RED**

在 `TestThreadCountResolution()` 增加：

```cpp
ExpectEq(ResolveMSMarkTotalThreads(64, 128, 0), 8, "64P default mark threads");
ExpectEq(ResolveMSMarkTotalThreads(128, 128, 0), 16, "128P default mark threads");
ExpectEq(ResolveMSMarkTotalThreads(160, 8, 0), 4, "160P on 8 logical CPUs");
ExpectEq(ResolveMSMarkTotalThreads(160, 8, 12), 12, "explicit mark threads");
```

在 `MSParallelSweepPolicyTest.cpp` 增加：

```cpp
ExpectEq(ComputeMSMaxSweepHelpers(16, 0), 8,
         "automatic sweep cap must be independent from mark width");
ExpectEq(ComputeMSMaxSweepHelpers(16, 3), 3,
         "explicit sweep cap");
ExpectEq(ComputeMSMaxSweepHelpers(4, 0), 3,
         "low-width sweep behavior");
```

- [x] **Step 2：运行测试并确认旧实现失败**

```bash
mkdir -p .tmp/step49
clang++ -std=c++14 -pthread -I. -Isrc \
  test/MSMarkWorkerPolicyTest.cpp -o .tmp/step49/MSMarkWorkerPolicyTest
./.tmp/step49/MSMarkWorkerPolicyTest

clang++ -std=c++14 -pthread -I. -Isrc \
  test/MSParallelSweepPolicyTest.cpp -o .tmp/step49/MSParallelSweepPolicyTest
./.tmp/step49/MSParallelSweepPolicyTest
```

Expected：前者在 `64P default mark threads` 报 `expected=8 actual=4`；后者在自动 cap 报 `expected=8 actual=15`。

- [x] **Step 3：实现第 7.1、7.2 节纯策略**

Mark resolver 和 backlog 比例策略以第 7.1 节当前代码为准；两条 Runtime 参数
入口都传入 logical CPU。tail helper/assist escalation 已删除，完整空闲候选直接
进入 one-way quiesce。

Sweep resolver：

```cpp
constexpr size_t kMSDefaultMaxSweepHelpers = 8;
size_t logicalMaxHelpers = gcThreads > 1 ? gcThreads - 1 : 0;
size_t policyCap = configuredMaxHelpers == 0
    ? kMSDefaultMaxSweepHelpers
    : configuredMaxHelpers;
return std::min(logicalMaxHelpers, policyCap);
```

- [x] **Step 4：重跑策略与 quiesce 契约测试**

```bash
clang++ -std=c++14 -pthread -I. -Isrc \
  test/MSMarkWorkerPolicyTest.cpp -o .tmp/step49/MSMarkWorkerPolicyTest
clang++ -std=c++14 -pthread -I. -Isrc \
  test/MSParallelSweepPolicyTest.cpp -o .tmp/step49/MSParallelSweepPolicyTest
./.tmp/step49/MSMarkWorkerPolicyTest
./.tmp/step49/MSParallelSweepPolicyTest
```

结果：`MSMarkWorkerPolicyTest`、`MSParallelSweepPolicyTest`、
`MSAssistPacingTest`、`MSGCTailDrainContractTest.sh` 和
`MSGCMarkControlFastPathContractTest.sh` 均退出码 0。

- [x] **Step 5：Runtime 源码构建**

本轮按用户边界只执行：

```bash
cmake --build CMakebuild --target cangjie-runtime -j 8
```

结果：macOS AArch64 Release/MarkSweep 构建退出码 0，产物为
`output/temp/lib/aarch64_Release/libcangjie-runtime.dylib`。没有安装 SDK；
case27、60 秒矩阵和 LLT/HLT 留待后续验证。

### Task 2：Scavenge 最大堆水位

**Files:**

- Create: `src/Heap/MCollector/MSScavengeThresholdPolicy.h`
- Create: `test/MSScavengeThresholdPolicyTest.cpp`
- Modify: `src/Heap/Collector/GcStats.cpp`
- Modify: `src/Heap/MCollector/MarkSweepCollector.cpp`

- [ ] **Step 1：创建纯策略测试**

测试内容包含：

```cpp
constexpr size_t GiB = 1024ULL * 1024ULL * 1024ULL;

auto normal = EvaluateMSScavengeThreshold(4 * GiB, 1.5, 16 * GiB);
ExpectEq(normal.threshold, 6 * GiB, "normal scavenge threshold");
ExpectEq(normal.capped, false, "normal threshold not capped");

auto capped = EvaluateMSScavengeThreshold(16 * GiB, 5.0, 16 * GiB);
ExpectEq(capped.maxHeapWatermark, (16 * GiB / 100) * 80 +
         ((16 * GiB % 100) * 80) / 100, "80 percent watermark");
ExpectEq(capped.threshold, capped.maxHeapWatermark, "threshold capped by max heap");
ExpectEq(capped.capped, true, "capped diagnostic");

auto noCap = EvaluateMSScavengeThreshold(4 * GiB, 2.0, 0);
ExpectEq(noCap.threshold, 8 * GiB, "zero max heap keeps raw threshold");
```

- [ ] **Step 2：编译并确认头文件缺失**

```bash
clang++ -std=c++14 -pthread -I. -Isrc \
  test/MSScavengeThresholdPolicyTest.cpp \
  -o .tmp/step49/MSScavengeThresholdPolicyTest
```

Expected：FAIL，提示 `MSScavengeThresholdPolicy.h` 不存在。

- [ ] **Step 3：实现纯策略**

核心实现：

```cpp
long double multiplier = scavengeGrowth <= 0.0 ? 1.0L :
    static_cast<long double>(scavengeGrowth);
long double scaled = static_cast<long double>(heapThreshold) * multiplier;
decision.rawThreshold = scaled >= static_cast<long double>(
    std::numeric_limits<size_t>::max())
    ? std::numeric_limits<size_t>::max()
    : static_cast<size_t>(scaled);

if (maxHeapBytes == 0) {
    decision.threshold = decision.rawThreshold;
    return decision;
}
decision.maxHeapWatermark =
    (maxHeapBytes / 100) * kMSScavengeMaxHeapPercent +
    ((maxHeapBytes % 100) * kMSScavengeMaxHeapPercent) / 100;
decision.threshold = std::min(decision.rawThreshold,
                              decision.maxHeapWatermark);
decision.capped = decision.threshold < decision.rawThreshold;
```

- [ ] **Step 4：接入初始化与周期更新**

`GcStats.cpp` 只在 `MRT_USE_MARK_SWEEP` 下调用新策略；非 MSGC 分支保留原计算。`MarkSweepCollector.cpp` 删除本地重复 `ComputeScavengeThresholdBytes()`，两个位置统一传入 `Heap::GetHeap().GetMaxCapacity()` 作为 max heap。

- [ ] **Step 5：运行测试**

```bash
clang++ -std=c++14 -pthread -I. -Isrc \
  test/MSScavengeThresholdPolicyTest.cpp \
  -o .tmp/step49/MSScavengeThresholdPolicyTest
./.tmp/step49/MSScavengeThresholdPolicyTest
```

Expected：退出码 0。

- [ ] **Step 6：提交独立变更**

```bash
git add src/Heap/MCollector/MSScavengeThresholdPolicy.h \
  src/Heap/Collector/GcStats.cpp \
  src/Heap/MCollector/MarkSweepCollector.cpp \
  test/MSScavengeThresholdPolicyTest.cpp
git commit -m "opt: cap MSGC scavenge threshold by max heap"
```

### Task 3：floating-live 稳定阈值与并发 Mark 安全阀

**Files:**

- Modify: `src/Heap/MCollector/MSHeapThresholdPolicy.h`
- Create: `src/Heap/MCollector/MSMarkTerminationPolicy.h`
- Modify: `src/Heap/MCollector/MarkSweepCollector.h`
- Modify: `src/Heap/MCollector/MarkSweepCollector.cpp`
- Modify: `src/Heap/MAllocator/MSRegionManager.h`
- Modify: `src/Heap/MAllocator/MSRegionManager.cpp`
- Modify: `src/Heap/Collector/GcStats.h`
- Modify: `test/MSHeapThresholdPolicyTest.cpp`
- Create: `test/MSMarkTerminationPolicyTest.cpp`

- [ ] **Step 1：添加 floating 分类 RED 用例**

```cpp
constexpr size_t MiB = 1024ULL * 1024ULL;
constexpr size_t GiB = 1024ULL * MiB;

ExpectTrue(IsMSFloatingLiveInflated(
    278 * MiB, 16 * GiB, 14 * GiB, 16 * GiB),
    "case27 inflated cycle");
ExpectFalse(IsMSFloatingLiveInflated(
    1 * GiB, 2 * GiB, 1500 * MiB, 16 * GiB),
    "normal live growth");
ExpectFalse(IsMSFloatingLiveInflated(
    4 * GiB, 5 * GiB, 4500 * MiB, 16 * GiB),
    "small allocation delta");
```

- [ ] **Step 2：添加 Mark pressure RED 用例**

覆盖 debt 每 100 个 Global WorkBuf 提升一级、debt 未下降时额外提升一级、连续 3 个
无进展窗口请求 STW、`debt <= 32` 请求 STW、OOM 满 pressure，以及 Assist ratio
乘法饱和和 Small/Large grow 原因显式分类。

- [ ] **Step 3：运行并确认新符号不存在**

```bash
clang++ -std=c++14 -pthread -I. -Isrc \
  test/MSHeapThresholdPolicyTest.cpp -o .tmp/step49/MSHeapThresholdPolicyTest
clang++ -std=c++14 -pthread -I. -Isrc \
  test/MSMarkTerminationPolicyTest.cpp -o .tmp/step49/MSMarkTerminationPolicyTest
```

Expected：编译失败，提示新策略符号/头文件不存在。

- [ ] **Step 4：实现纯策略并接入周期状态**

实现以下确定调用点：

```cpp
// PreGarbageCollection()
cycleStats_.markStartActiveBytes =
    static_cast<RegionSpaceMS&>(theAllocator).GetHeapInUseBytes();

// UpdateGCStats()
cycleStats_.markEndActiveBytes = space.GetHeapInUseBytes();
cycleStats_.allocationDuringMarkBytes =
    cycleStats_.markEndActiveBytes > cycleStats_.markStartActiveBytes
    ? cycleStats_.markEndActiveBytes - cycleStats_.markStartActiveBytes
    : 0;
cycleStats_.floatingLiveInflated = IsMSFloatingLiveInflated(
    cycleStats_.markStartActiveBytes,
    cycleStats_.markEndActiveBytes,
    cycleStats_.trueLiveBytes,
    Heap::GetHeap().GetMaxCapacity());
size_t thresholdBase = cycleStats_.floatingLiveInflated
    ? std::max(gcStats.lastStableLiveBytes,
               cycleStats_.markStartActiveBytes)
    : cycleStats_.trueLiveBytes;
if (!cycleStats_.floatingLiveInflated) {
    gcStats.lastStableLiveBytes = cycleStats_.trueLiveBytes;
}
```

inflated 周期只调整 threshold 基线，不主动追加 stabilization GC。`RunConcurrentMarkLoop()`
每完成 100 个 GC-main WorkBuf 调用 `MSMarkPressurePolicy::Checkpoint()`；Helper 与 Assist
使用同一个 pressure level。debt budget、连续无进展或 grow-before 条件命中后，设置现有
`forceConverged=true` 并调用 `QuiesceMarkWorkers()`。

- [ ] **Step 5：运行策略和现有 Mark 控制测试**

```bash
clang++ -std=c++14 -pthread -I. -Isrc \
  test/MSHeapThresholdPolicyTest.cpp -o .tmp/step49/MSHeapThresholdPolicyTest
clang++ -std=c++14 -pthread -I. -Isrc \
  test/MSMarkTerminationPolicyTest.cpp -o .tmp/step49/MSMarkTerminationPolicyTest
./.tmp/step49/MSHeapThresholdPolicyTest
./.tmp/step49/MSMarkTerminationPolicyTest
./.tmp/step49/MSMarkWorkerPolicyTest
```

Expected：退出码均为 0。

- [ ] **Step 6：提交独立变更**

```bash
git add src/Heap/MCollector/MSHeapThresholdPolicy.h \
  src/Heap/MCollector/MSMarkTerminationPolicy.h \
  src/Heap/MCollector/MarkSweepCollector.h \
  src/Heap/MCollector/MarkSweepCollector.cpp \
  src/Heap/MAllocator/MSRegionManager.h \
  src/Heap/MAllocator/MSRegionManager.cpp \
  src/Heap/Collector/GcStats.h \
  test/MSHeapThresholdPolicyTest.cpp \
  test/MSMarkTerminationPolicyTest.cpp
git commit -m "opt: stabilize MSGC heap pacing under mark lag"
```

### Task 4：Shard 与初始 Segment 解耦

**Files:**

- Modify: `src/Heap/MAllocator/MSShardRouting.h`
- Modify: `src/CjScheduler.cpp`
- Modify: `src/CangjieRuntimeApi.cpp`
- Modify: `src/CangjieRuntime.h`
- Modify: `src/CangjieRuntime.cpp`
- Modify: `src/Heap/MAllocator/MSRegionManager.cpp`
- Modify: `src/Heap/MAllocator/MSFreeManager.h`
- Modify: `src/Heap/MAllocator/MSFreeManager.cpp`
- Modify: `test/MSShardRoutingTest.cpp`
- Modify: `test/MSFreeManagerShardLocalTest.cpp`
- Modify: `test/MSFreeManagerShardStealTest.cpp`
- Modify: `test/MSFreeManagerGrowLockOrderTest.cpp`
- Modify: `test/MSFreeManagerOwnerTest.cpp`

- [ ] **Step 1：先更新纯策略测试**

```cpp
ExpectEq(ResolveMSShardCount(8, 0), 8, "8P direct shards");
ExpectEq(ResolveMSShardCount(16, 0), 8, "16P default shards");
ExpectEq(ResolveMSShardCount(32, 0), 8, "32P divisor below cube-root target");
ExpectEq(ResolveMSShardCount(128, 0), 16, "128P default shards");
ExpectEq(ResolveMSShardCount(1024, 0), 32, "1024P default shards");
ExpectEq(ResolveMSShardCount(127, 0), 1, "prime P strict equal-share fallback");
ExpectEq(ResolveMSShardCount(128, 24), 16,
         "explicit non-divisor rounds down");
ExpectEq(ResolveMSShardCount(128, 128), 128,
         "explicit processor-sized rollback");
ExpectEq(ResolveMSInitialSegmentCount(0), 1,
         "default one initial segment");
ExpectEq(ResolveMSInitialSegmentCount(16), 16,
         "explicit initial segments");
```

再对表内全部 P 统一断言：

```cpp
uint32_t shards = ResolveMSShardCount(processors, 0);
ExpectEq(processors % shards, 0, "P must be evenly divisible by shards");
ExpectEq(processors / shards,
         ComputeMSProcessorsPerShard(processors, shards),
         "all shards have the same processor count");
```

- [ ] **Step 2：运行 RED**

```bash
clang++ -std=c++14 -pthread -I. -Isrc \
  test/MSShardRoutingTest.cpp -o .tmp/step49/MSShardRoutingTest
```

Expected：编译失败，提示 `ResolveMSShardCount`/`ResolveMSInitialSegmentCount` 不存在。

- [ ] **Step 3：实现解析和初始化拆分**

加入 `ResolveMSShardCount()`、`ResolveMSInitialSegmentCount()`，在 `CjScheduler.cpp` 与 `CangjieRuntimeApi.cpp` 解析：

```cpp
uint32_t shardCount = ResolveMSShardCount(processorCount, configuredShards);
uint32_t initialSegments =
    ResolveMSInitialSegmentCount(configuredInitialSegments);
uint32_t processorsPerShard = processorCount / shardCount;
CangjieRuntime::SetMSAllocatorScaleConfig(
    {shardCount, processorsPerShard, initialSegments});
size_t initHeapSizeKB =
    static_cast<size_t>(initialSegments) * kMSShardSegmentBytes / KB;
```

显式 initial Segment 还必须 clamp 到：

```text
[1, floor(maxVirtualHeapSize / segmentSize)]
```

不能允许配置值使初始化 `CHECK` 退出。

两个 Runtime 入口的 MSGC 分支都设置：

```cpp
.heapSize = initHeapSizeKB,
.gcThreshold = CangjieRuntime::GetMSMaxVirtualHeapSizeKB() * KB,
```

`MSRegionManager::Initialize()` 读取内部 config：

```cpp
MSAllocatorScaleConfig scale = CangjieRuntime::GetMSAllocatorScaleConfig();
uint32_t shardCount = scale.shardCount;
size_t committedHeapSize = RoundUp<size_t>(
    static_cast<size_t>(scale.initialSegmentCount) *
        kMSShardSegmentBytes,
    MS_REGION_SIZE);
```

- [ ] **Step 4：添加单初始 owner 的 FreeManager 测试**

创建 16 shard、1 committed Segment 的 fixture，模拟 P=128 默认拓扑，验证：

1. 无 owned view 的 shard 能从 non-empty donor 获取一个 Region。
2. 16 个 shard 并发首次分配时优先共享初始 Segment，不会各自 Grow。
3. committed donor 全空时只 Grow 一个 home Segment；等待者拿到 `growMutex_` 后二次检查。
4. 并发首次分配不会给同一 Region 双重所有权。
5. steal 后 `segmentOwnerShard_` 不改变，回收返回原 owner。
6. shardCount 超过 64 时，多 word non-empty bitmap 不丢失 donor。

- [ ] **Step 5：运行 FreeManager 定向测试**

```bash
for src in \
  test/MSFreeManagerShardLocalTest.cpp \
  test/MSFreeManagerShardStealTest.cpp \
  test/MSFreeManagerGrowLockOrderTest.cpp \
  test/MSFreeManagerOwnerTest.cpp; do
  name="$(basename "$src" .cpp)"
  clang++ -std=c++14 -pthread -I. -Isrc "$src" \
    -o ".tmp/step49/$name"
  ".tmp/step49/$name"
done
```

Expected：全部退出码 0。

- [ ] **Step 6：提交独立变更**

```bash
git add src/Heap/MAllocator/MSShardRouting.h \
  src/CjScheduler.cpp src/CangjieRuntimeApi.cpp \
  src/CangjieRuntime.h src/CangjieRuntime.cpp \
  src/Heap/MAllocator/MSRegionManager.cpp \
  src/Heap/MAllocator/MSFreeManager.h \
  src/Heap/MAllocator/MSFreeManager.cpp \
  test/MSShardRoutingTest.cpp \
  test/MSFreeManagerShardLocalTest.cpp \
  test/MSFreeManagerShardStealTest.cpp \
  test/MSFreeManagerGrowLockOrderTest.cpp \
  test/MSFreeManagerOwnerTest.cpp
git commit -m "opt: decouple MSGC shards from processor count"
```

### Task 5：Active Region bitmap clear

**Files:**

- Modify: `src/Heap/MAllocator/MSCentralRegionPool.h`
- Modify: `src/Heap/MAllocator/MSCentralRegionPool.cpp`
- Modify: `src/Heap/MAllocator/MSAllocBuffer.h`
- Create: `test/MSCentralMarkBitmapSnapshotTest.cpp`
- Modify: `test/MSGCIncrementalRegionSnapshotContractTest.sh`

- [ ] **Step 1：建立集合一致性测试**

fixture 必须同时放入：

```text
1 个 AllocBuffer active small Region
1 个 partialSwept Region
1 个 fullUnswept Region
1 个 large primary + followers
1 个 empty cache Region
1 个 free Region
```

断言快照只包含前三类 small Region和 large primary；不包含 follower、empty cache、free。

- [ ] **Step 2：运行 RED**

```bash
clang++ -std=c++14 -pthread -I. -Isrc \
  test/MSCentralMarkBitmapSnapshotTest.cpp \
  -o .tmp/step49/MSCentralMarkBitmapSnapshotTest
```

Expected：编译失败，提示 snapshot 接口不存在。

- [ ] **Step 3：实现 STW 快照和 reference 校验**

实现顺序：

1. `StopAndWaitForAllSweep()` 保持不变。
2. 在 STW 内获取注册 AllocBuffer/bucket/large 容器快照。
3. 按 Region index 排序、去重。
4. Release 按快照清 bit。
5. Debug 额外运行 committed reference scan，集合不同则 FATAL。

- [ ] **Step 4：运行定向测试和契约检查**

```bash
clang++ -std=c++14 -pthread -I. -Isrc \
  test/MSCentralMarkBitmapSnapshotTest.cpp \
  -o .tmp/step49/MSCentralMarkBitmapSnapshotTest
./.tmp/step49/MSCentralMarkBitmapSnapshotTest
bash test/MSGCIncrementalRegionSnapshotContractTest.sh
```

Expected：退出码均为 0；契约检查确认 Release 的默认 bitmap clear 不再调用 committed 全范围循环。

- [ ] **Step 5：提交独立变更**

```bash
git add src/Heap/MAllocator/MSCentralRegionPool.h \
  src/Heap/MAllocator/MSCentralRegionPool.cpp \
  src/Heap/MAllocator/MSAllocBuffer.h \
  test/MSCentralMarkBitmapSnapshotTest.cpp \
  test/MSGCIncrementalRegionSnapshotContractTest.sh
git commit -m "opt: clear MSGC mark bits from active region snapshots"
```

### Task 6：case27 参数透传与结果元数据

**Files:**

- Modify: `MSGC_RESULT/Result/case27_http_complex_json/run_compare_linux.sh`

- [ ] **Step 1：增加脚本参数**

```bash
MSGC_GC_THREADS="${MSGC_GC_THREADS:-}"
MSGC_SHARD_COUNT="${MSGC_SHARD_COUNT:-}"
MSGC_INITIAL_SEGMENT_COUNT="${MSGC_INITIAL_SEGMENT_COUNT:-}"
```

写入 MSGC `run_metadata.env`：

```text
cj_gc_threads
cj_ms_shard_count
cj_ms_initial_segment_count
```

- [ ] **Step 2：只向 MSGC 子进程透传**

在 `CJC_GC_MODE=marksweep` 分支：

```bash
if [[ -n "${MSGC_GC_THREADS:-}" ]]; then
    export cjGCThreads="$MSGC_GC_THREADS"
fi
if [[ -n "${MSGC_SHARD_COUNT:-}" ]]; then
    export cjMSShardCount="$MSGC_SHARD_COUNT"
fi
if [[ -n "${MSGC_INITIAL_SEGMENT_COUNT:-}" ]]; then
    export cjMSInitialSegmentCount="$MSGC_INITIAL_SEGMENT_COUNT"
fi
```

CopyGC 分支必须显式 unset 三个变量，不能改变 CopyGC 编译或运行环境。

- [ ] **Step 3：Shell 静态检查**

```bash
bash -n MSGC_RESULT/Result/case27_http_complex_json/run_compare_linux.sh
```

Expected：退出码 0。

- [ ] **Step 4：最小 smoke**

```bash
CASE27_PAYLOAD=1K CASE27_CONCURRENCY=2 CASE27_DURATION_MS=1000 \
PROCS=8 MSGC_GC_THREADS=4 GO_ENABLE=0 \
bash MSGC_RESULT/Result/case27_http_complex_json/run_compare_linux.sh
```

Expected：CopyGC/MSGC 均退出 0；MSGC metadata 记录 `cj_gc_threads=4`，CopyGC metadata 不设置对应 Runtime 环境。

- [ ] **Step 5：提交脚本**

```bash
git add MSGC_RESULT/Result/case27_http_complex_json/run_compare_linux.sh
git commit -m "test: expose MSGC high-core pacing controls"
```

## 15. 构建与验证矩阵

### 15.1 策略/契约测试

每个 Phase 首先运行其列出的 C++14 单测。合并前至少补跑：

```bash
bash test/MSGCMarkControlFastPathContractTest.sh
bash test/MSGCDynamicSweepHelperContractTest.sh
bash test/MSGCIncrementalRegionSnapshotContractTest.sh
bash test/MSGCPostMarkSweepPublishContractTest.sh
bash test/MSGCMinGCThresholdContractTest.sh
```

Expected：全部退出码 0。

### 15.2 Runtime 构建门禁

执行前必须重新确认三件套：

```text
compiler：实际使用的 MSGC cjc 路径和 commit
runtime：本次修改源码 commit 和安装后的 lib MD5
stdlib/stdx：实际 SDK 目录
```

使用用户指定的 Linux 增量脚本构建和安装，不能清理已构建 compiler：

```text
/home/lipeisheng/cangjie/cangjie_runtime/runtime/build/scripts/
incremental_rebuild_msgc_sdk.py
```

每分钟检查同一构建任务输出；没有产物、退出码和安装记录前不能宣称成功。

### 15.3 第一轮 15 秒诊断矩阵

固定：

```text
payload=100K
concurrency=1000
duration=15000ms
heap=16gb
runs=1
GO_ENABLE=0
```

矩阵：

| P | Mark threads | Sweep cap | Shard | Initial Segment | Scavenge growth |
| ---: | ---: | ---: | ---: | ---: | ---: |
| 64 | 8 | 8 | 32 | 8 | 1.5 |
| 64 | 8 | 8 | 64 | 8 | 1.5 |
| 128 | 8 | 8 | 64 | 8 | 1.5 |
| 128 | 16 | 8 | 64 | 8 | 1.5 |
| 128 | 16 | 8 | 32 | 8 | 1.5 |
| 128 | 16 | 8 | 64 | 16 | 1.5 |

先选择无 crash、无 mark verify 错误、成功率高且 active/RSS 最稳定的两组进入正式测试。

### 15.4 60 秒正式回归

候选配置每组至少运行 3 次，CopyGC 和 MSGC 都用各自 SDK 重新编译 benchmark。记录：

- throughput、success/fail、平均/峰值/P95 RSS、平均/峰值 CPU。
- GC count、ConcurrentMark avg/max/P95、MarkTermination、PostMark、PreEnum。
- Mark main/helper/assist processed 和 Assist share。
- threshold/scavenge threshold/floating classification。
- active/retained/hot-free/cold-free/released bytes。
- shard grow/steal/borrow 统计。

相对 P=128/4 线程同 SDK 基线的首轮验收目标：

```text
throughput 提升 >= 50%
失败率至少下降 50%
ConcurrentMark 最大值下降 >= 50%
平均 RSS 下降 >= 10%
峰值 RSS 下降 >= 5%
Scavenge threshold 始终 <= 80% max heap
不再出现约 1GiB <-> 16GiB threshold 周期震荡
```

绝对吞吐不能与不同 Runtime MD5、不同 benchmark 编译产物或不同客户端模式的历史结果直接比较。

### 15.5 低核与 CopyGC 隔离

低核至少覆盖 P=1/2/4/8，验证默认 Mark 宽度没有改变原行为。CopyGC 验证：

- `MRT_USE_MARK_SWEEP` 未定义时不编译/调用新策略。
- CopyGC `gcThreads=2` 默认不变。
- CopyGC Scavenge threshold 逻辑不使用 MSGC 80% cap。
- CopyGC benchmark 编译不添加 `--gc-mode`，并加载 CopyGC SDK 的 Runtime。

### 15.6 Runtime LLT/HLT

源码修改涉及 Runtime GC。原始 case27 验证通过后，必须根据 bugfix skill 的 Runtime regression gate 执行 LLT 和 HLT。开始前由用户提供/确认：

```text
cangjie_test（包含 testsuites/LLT 与 testsuites/HLT）
cangjie_test_framework
测试配置
```

缺少测试仓或框架时只能标记 `fixed-unverified`，不能标记 `verified`。

## 16. 上线顺序、停止条件与回滚

### 16.1 上线顺序

```text
Phase A Mark/Sweep 宽度
    ↓ 独立构建和 case27
Phase B Scavenge cap
    ↓ 独立构建和 RSS 验证
Phase C Threshold/headroom
    ↓ 观察稳定周期
Phase D Shard/Segment
    ↓ allocator 并发和 NUMA 观察
Phase E active Region clear
    ↓ STW P95 优化
```

不得跳过 A/B，直接依靠 Threshold 或强制 STW 掩盖 Mark capacity 问题。

### 16.2 停止条件

出现任一情况立即停止当前 Phase 并保留日志：

- `invalid stateWord/typeInfo`、mark completeness、WeakRef/Finalizer 错误。
- Region 双重分配、owner 不一致、bitmap/subset invariant 失败。
- ConcurrentMark 缩短但 MarkTermination/STW P95 明显恶化。
- Sweep helper 扩展造成 Mutator throughput 下降。
- Scavenge 频繁 release/reuse 形成页抖动。
- CopyGC 行为或公共 Runtime ABI 发生变化。

### 16.3 回滚

- Phase A 可显式设置 `cjGCThreads=4`、`cjMSParallelSweepMaxHelpers=3` 恢复旧宽度。
- Phase D 可设置 `cjMSShardCount=processorNum`、`cjMSInitialSegmentCount=processorNum` 恢复旧拓扑。
- Phase B/C/E 使用独立提交，出现回归时按 Phase commit 回滚，不用全局 reset，不影响用户其他工作树修改。
- benchmark 结果目录只新增不覆盖，SDK 安装按现有脚本保留备份。

## 17. 当前建议与非结论

### 17.1 当前已经证实

1. P=128 下固定 4 个 Mark 线程是吞吐瓶颈。
2. P=128/16 Mark 线程能显著缩短 ConcurrentMark、减少 floating live 和 active heap。
3. `cjScavengeGrowth=5` 产生高于最大堆的水位，导致 RSS 无法随 active heap 一起回落。
4. P、Mark、Sweep、Shard、initial Segment 不应继续使用同一个线性规模关系。

### 17.2 立即可用的临时运行建议

- 不改 Runtime 时，当前脚本可先用 `PROCS=64`，本轮短测比 P=128 更稳定。
- 如果脚本补充 `cjGCThreads` 透传，可在 P=128 下使用 `cjGCThreads=16` 做正式验证。
- `cjGCThreads=16 + cjScavengeGrowth=1.5` 是有依据的下一组实验配置，但组合效果尚未完成正式验证，不能写成最终默认值。

### 17.3 仍不是结论

- Phase D 已确认三次根目标、严格整除和单初始 Segment 的设计，但尚未实施或完成性能验证。
- P=127 等质数默认退化为单 shard 的代价需要单独验证；本文不把它描述成普遍最优。
- 80% Scavenge 水位仍是历史第一版候选，不代表当前实现。
- 增加 Mark 线程本身不能保证 RSS 下降。
- 降低 Scavenge threshold 本身不能解决 ConcurrentMark 追不上分配。
- 在没有三次 60 秒回归、低核验证和 LLT/HLT 前，不能宣称 128 核规模扩展问题已修复。

## 18. macOS P=CPU×20 复现实验（2026-07-24）

### 18.1 实验改动

为在 8 核 macOS 主机上放大 Linux 128 核场景，`CjScheduler.cpp` 的 Processor
上限改为按 GC 模式解析：

- MSGC：`logical_cpu_count * 20`
- CopyGC：保留原行为 `logical_cpu_count * 2`

默认 Processor 数仍为逻辑 CPU 数；只有显式设置较大的 `cjProcessorNum` 时才使用
扩大的 MSGC 上限。该改动是复现实验能力，不是 Step49 最终规模策略。

新增用例目录：

```text
MSGC_RESULT/Result/case27_http_complex_json_msgc_high_p
```

该目录从 case27 复制但不带旧 `result/`，默认入口只编译和运行 MSGC：

```bash
./run_compare.sh
```

在 8 核主机上默认 `PROCS=160`，并生成吞吐、CPU、RSS 和逐次 GC 图。所有 `.cj`
在干净 SDK 环境内重新编译，runtime dylib 来源和 MD5 写入每轮 metadata。

### 18.2 同机对照结果

两组均使用：

- compiler：`cangjie_compiler/output/msgc/bin/cjc`
- runtime：本次 marksweep runtime，安装到
  `cangjie_compiler/output/msgc/runtime/lib/darwin_aarch64_cjnative`
- stdlib/stdx：现有 `cangjie_compiler/output/msgc` 内容，本轮未重建
- payload：100K
- concurrency：1000
- requested duration：15 秒
- max heap：16GB

| 指标 | P=160 | P=8 |
| --- | ---: | ---: |
| shardCount | 160 | 8 |
| 初始 committed heap | 10GB | 512MB |
| 退出码 | 139 | 0 |
| 最终成功请求 | 无最终结果；崩溃前至少 1231 | 13859 |
| 吞吐 | 无稳态结果 | 827.7 req/s |
| 完整 GC cycle | 6 | 147 |
| GC max | 9167.05ms | 157.41ms |
| ConcurrentMark 最大观测 | 8755.05ms | 约 137.22ms |
| RSS peak | 2671.33MB（部分运行） | 2508.80MB |
| CPU avg | 188.71%（部分运行） | 449.23% |

P=160 日志中的关键状态：

```text
shardCount=160, segmentSize=64MB
Heap size: 10485760(KB)
ConcurrentMark time: 8,755,052us
active-region bytes: 16,581,525,504 B
scheduler P: 160, tl-active regions: 4054, alloc buffers: 162
=== GC START cycle=7 === ... reason=1(oom)
```

cycle 6 完成后仍有 1212 个 unswept region。下一轮 OOM GC 清理 backlog 后，在
`FrameInfo::ResolveProcInfo()` 所在的异常/栈处理路径收到 SIGSEGV。这个 SIGSEGV
需要单独定位，但它发生在堆已经达到 OOM 状态之后，不改变高 P 规模失配已经复现
的结论。

P=8 对照在相同请求压力下完成，零失败；其 active-region bytes 稳定在约
0.5–1.8GB，ConcurrentMark 通常为几十毫秒。因此这次实验支持 Step49 的原判断：

1. Processor 线性放大会同步放大 shard 和初始 Segment；160P 直接得到 10GB
   initial committed capacity。
2. Mutator/allocator 并发能力扩大后，当前固定/偏小的后台 Mark capacity 无法
   及时收敛，长 mark 窗口又放大 floating live。
3. 高 P 不是吞吐优化本身。真正的修复仍应拆分 Processor、Mark worker、Sweep
   helper、allocator shard 和 initial Segment，而不是把它们绑定到同一个 P。

结果目录：

```text
P160: case27_http_complex_json_msgc_high_p/result/
      20260724_165437_probe_c1000_msgc_high_p_p160_c1000_100K_15000ms
P8:   case27_http_complex_json_msgc_high_p/result/
      20260724_165631_control_p8_msgc_high_p_p8_c1000_100K_15000ms
```

### 18.3 当前状态

- 已完成：Processor 上限实验开关、MSGC-only case 副本、每轮重编译、CPU/RSS
  采样、吞吐和 GC 图、P160/P8 同机对照。
- 已证实：macOS 可复现高 P 下的初始堆/allocator 拓扑线性放大和 Mark
  capacity 失配。
- 未完成：Step49 Phase A–E 的产品化修复、三轮 60 秒回归、低核回归、
  LLT/HLT。
- 因 P=160 在约 11 秒即进入 OOM 并崩溃，本轮没有继续机械执行 60 秒；延长超时
  不会增加稳态数据，只会重复同一失败路径。

### 18.4 Phase A 安装后 P160 复验

2026-07-24 22:36 使用重新安装到 `cangjie_compiler/output/msgc` 的 Runtime
再次执行 `P=160 / c1000 / 100K / 60s / max heap 16GB`。用例由该 SDK 重新
编译，实际加载的 Runtime MD5 为 `85f001e1463e9c154db645debd143d3a`。

Phase A 已进入实际运行路径：

- 7 个已完成周期均输出 `[mark-quiesce]`，不再输出旧的 `[mark-tail]`。
- `[mark-workers] configured=4` 符合 `P=160、logical CPU=8` 自动解析为 4 个
  Mark 总线程；第 5～7 轮 helper 分别处理 `13,312`、`1,017,856`、
  `29,349,888` 个对象，证明 helper 路径实际参与。
- `quiesceResidualBuffers=0`，本轮未因 one-way quiesce 留下 Global residual。
- 日志没有 `parallel-sweep start`，下一轮 GC 直接在 `PreEnumRoots()` force
  drain；因此只能确认 Sweep cap 已编入 Runtime，不能宣称该次 workload 实际
  使用了动态 Sweep helper。

与修改前同参数结果
`20260724_172840_msgc_high_p_p160_c1000_100K_60000ms` 对比：

| 指标 | 修改前 | Phase A 安装后 | 变化 |
| --- | ---: | ---: | ---: |
| 已完成 GC 周期 | 7 | 7 | 相同 |
| ConcurrentMark 总计 | 8,379.686ms | 6,521.670ms | -22.17% |
| ConcurrentMark 最大值 | 8,184.279ms | 6,452.261ms | -21.16% |
| 第 6 轮 ConcurrentMark | 191.663ms | 67.634ms | -64.71% |
| 7 轮 total GC 总计 | 9,940.068ms | 7,017.211ms | -29.40% |
| 退出码 | 139 | 139 | 未修复 |

这只能证明单次失败运行中的 Mark 时间改善，不能作为吞吐验收：两轮实际扫描
对象量不完全相同，且都没有完成 60 秒。安装后运行在 4.083 秒时完成
`1584` 个成功请求，随后第 7 轮 Mark 把 active-region bytes 推到
`16,539,779,072B`、true live 推到 `14,389,523,056B`；大量分配失败后进入
cycle 8 OOM GC，最终在 `FrameInfo::ResolveProcInfo()` 收到 SIGSEGV。

因此 Phase A 缩短了 Mark，但没有改变 P160 的 `160 shard + 10GB initial
committed capacity`。Allocator shard、初始 Segment、Threshold/Scavenge 等
Phase B–E 仍是高 P 稳定运行的必要工作。

结果目录：

```text
case27_http_complex_json_msgc_high_p/result/
20260724_223606_after_mark_sweep_helper_policy_p160_c1000_100K_60000ms
```

### 18.5 单 Mark 线程且关闭 Mark Assist 的诊断结果

为测量 GC main 自身的对象扫描能力，2026-07-24 新增仅用于诊断的
`CJ_MS_DISABLE_ASSIST` 开关。该开关只在 MSGC `ConcurrentMark()` 建立本轮
Assist pacing 时生效：开启后 effective ratio 为 0，`AllocBufferMS` 不再累计
Mark Assist debt；不修改 `GCParam`、公共 Runtime ABI、Sweep Assist 或 Enum
Assist。case27 高 P 副本同时增加 `MSGC_GC_THREADS`，映射到 Runtime 的
`cjGCThreads`。

本轮配置：

```text
P=160
payload=100K
concurrency=1000
requested duration=60000ms
max heap=16GB
cjGCThreads=1
CJ_MS_DISABLE_ASSIST=1
```

三件套来源：

- compiler：`cangjie_compiler/output/msgc/bin/cjc`
- runtime：当前源码重新构建并安装到 `cangjie_compiler/output/msgc`，
  MD5 `90e9e94b0ed715ebf3b6494b5c0f6bb3`
- stdlib/stdx：`cangjie_compiler/output/msgc` 既有产物，本轮未重建

用例已重新用上述 SDK 和 `--gc-mode=marksweep` 编译。6 个完成周期均满足：

```text
Assist pacing: disabled=1 effectiveRatio=0
[mark-workers] configured=1 processed=0 ... mainProcessed=N
[assist] processed=0
```

因此这些周期的 `mainProcessed` 只来自 GC main 的 ConcurrentMark 扫描。逐周期
结果如下：

| cycle | ConcurrentMark | mainProcessed | GC main 扫描速度 |
| ---: | ---: | ---: | ---: |
| 1 | 34us | 173 | 508.82 万对象/s |
| 2 | 30us | 213 | 710.00 万对象/s |
| 3 | 43us | 269 | 625.58 万对象/s |
| 4 | 28us | 298 | 1064.29 万对象/s |
| 5 | 8.141ms | 105,111 | 1291.13 万对象/s |
| 6 | 12.129179s | 72,563,060 | 598.25 万对象/s |

6 轮合计扫描 `72,669,124` 个对象，ConcurrentMark 合计
`12.137455s`，汇总速度为 **598.72 万对象/s**。最长的第 6 轮受启动噪声影响最小，
其 **598.25 万对象/s** 可作为当前 macOS AArch64、该对象图和无 Helper/Assist
争用条件下的单 GC Mark 主线程能力估计，即约 **600 万对象/s**。

该指标只统计 ConcurrentMark 的 `mainProcessed`。MarkTermination 当前没有
processed 对象计数，不能并入该速度。本轮仍在 P160 的分配规模问题下退出 139：
进程在长 Mark 期间把 active heap 推近 16GB，出现大量分配失败，随后
`gc-main-thread` 在 `FrameInfo::ResolveProcInfo()` 收到 SIGSEGV。因此这是有效的
单线程 Mark 诊断数据，不是完整 60 秒吞吐结果，也不代表 P160 稳定性问题已修复。

结果目录：

```text
case27_http_complex_json_msgc_high_p/result/
20260724_230719_single_gc_thread_no_mark_assist_p160_c1000_100K_60000ms
```

### 18.6 只启用 Mark Helper、关闭 Mark Assist 的对照结果

使用与 18.5 相同的 Runtime MD5、compiler、stdlib/stdx、P160、c1000、100K 和
16GB max heap，只把 `cjGCThreads` 从 1 改为 4，继续保持
`CJ_MS_DISABLE_ASSIST=1`。用例再次由该 SDK 重新编译。

6 个完成周期均输出 `configured=4`、`[assist] processed=0` 和
`disabled=1 effectiveRatio=0`。Helper 在前 4 个微小周期没有取得工作，第 5
周期处理 `56,320` 个对象，第 6 周期处理 `39,219,200` 个对象，证明本轮只启用
GC main + 动态 Mark Helper，没有 Mutator Mark Assist。

| 口径 | 1 个总 Mark 线程，无 Assist | 4 个总 Mark 线程，无 Assist | 变化 |
| --- | ---: | ---: | ---: |
| 汇总对象数 | 72,669,124 | 52,063,382 | 对象图状态不同 |
| 汇总 ConcurrentMark | 12.137455s | 4.903980s | -59.60% |
| 汇总扫描速度 | 598.72 万对象/s | 1061.66 万对象/s | 1.773× |
| 最长周期扫描速度 | 598.25 万对象/s | 1064.13 万对象/s | 1.779× |

4 线程最长周期内部：

```text
ConcurrentMark = 4.885344s
mainProcessed = 12,767,076
helperProcessed = 39,219,200
totalProcessed = 51,986,276
```

按配置宽度 4 计算，最长周期相对单线程的并行效率约为 `1.779 / 4 = 44.47%`。
但 `configured=4` 是总宽度上限，当前日志没有记录每个 Helper 的 active
duration；动态策略会随 backlog 扩展，不能据此断言 3 个 Helper 在整个周期内
一直运行。两轮对象图和 floating live 也不同，因此 1.779× 是该 case 的实测
增益，不是严格的固定快照并行加速比。

本轮仍未完成 60 秒，在大量分配失败后进入 OOM cycle 7，并于
`FrameInfo::ResolveProcInfo()` 退出 139。这不影响已完成周期的 Mark 计数，但
不能把结果写成稳定业务吞吐。

结果目录：

```text
case27_http_complex_json_msgc_high_p/result/
20260724_232204_four_gc_threads_no_mark_assist_p160_c1000_100K_60000ms
```

### 18.7 OOM Mark/Sweep 全 Helper 与单次 Sweep 判定

2026-07-25 按 OOM 紧急路径补充以下策略：

- OOM ConcurrentMark 从本轮开始就启用全部已配置 Mark Helper，并在该轮内保持
  全宽；普通 heuristic GC 继续使用动态 Helper 策略。
- OOM PostMark 在普通 Lazy/Assist/Helper Sweep gate 保持关闭时，使用全部已配置
  Sweep Helper，并由 GC main 同时参与同一 sweep generation 的 Region claim。
- 所有 OOM Sweep task join 后只读取一次 residual unswept 数量用于日志；不重试、
  不断言必须为 0，也不发布 `sweepDone`。随后刷新 OOM cache 并重新开放普通 Sweep。
- `StopAndWaitForAllSweep()` 仍在下一轮 `PreEnumRoots()` 清 bitmap 前执行，原有跨轮
  安全边界不变；串行 `ForceFinishAllUnsweptForOOM()` 仅保留为诊断入口。

实现涉及：

```text
src/Heap/MAllocator/MSOOMEmergencyPolicy.h
src/Heap/MAllocator/MSCentralRegionPool.h
src/Heap/MAllocator/MSCentralRegionPool.cpp
src/Heap/MCollector/MarkSweepCollector.h
src/Heap/MCollector/MarkSweepCollector.cpp
test/MSOOMEmergencyPolicyTest.cpp
```

验证使用 `cangjie_compiler/output/msgc` 的 compiler、stdlib/stdx，并将本次
MarkSweep Runtime 构建产物替换到该 SDK 后重新编译用例。配置为 P128、c1000、
100K payload、16GB heap、`cjGCThreads=4`，执行一次 60 秒 case27：

```text
case27_http_complex_json_msgc_high_p/result/
20260725_211055_oom_parallel_sweep_once_p128_c1000_100K_60000ms
```

6 次 OOM cycle 均记录 `initialHelpers=3 forceAll=1`；Sweep 均使用 3 个 Helper
加 GC main，`allocationFallback=0`。本次 residual 恰好都为 0，但代码不依赖
这个结果。

| OOM 口径 | 修改前 P128 | 修改后 P128 | 变化 |
| --- | ---: | ---: | ---: |
| OOM cycle 数 | 8 | 6 | -25.00% |
| OOM GC 平均总时间 | 3788.72ms | 2381.27ms | -37.14% |
| OOM ConcurrentMark 平均 | 800.11ms | 224.13ms | -71.99% |
| OOM PostMark 平均 | 2681.96ms | 1854.68ms | -30.85% |
| OOM Sweep Region/PostMark 秒 | 约 8.91 万 | 约 14.22 万 | +59.72% |
| 同步 OOM 分配失败日志 | 895 | 768 | -14.19% |

OOM ConcurrentMark 的基线均值受一个 5311.114ms cycle 明显抬高，因此
`-71.99%` 不能单独作为稳定的 Mark 加速结论；而 Sweep 吞吐按实际处理 Region
总量归一化后仍有约 60% 提升，更能反映本次并行 Sweep 的直接收益。

全局结果中，成功吞吐从 216.90 增至 219.02 req/s（+0.98%），RSS 平均/峰值
分别下降 7.74%/8.98%；但失败率从 16.39% 升到 19.26%，CPU 平均上升 13.18%，
GC P95 从 6567.85ms 升到 7652.75ms。主要抵消项是普通 heuristic cycle 的
ConcurrentMark 合计从 32.407s 增至 41.966s；因此本轮确认 OOM Sweep 改造有效，
但总尾延迟的下一瓶颈仍是普通大对象图周期的 Mark/EnumRoots，而不是 OOM
PostMark。

### 18.8 Helper/Assist 统一压力与提前 STW（2026-07-26）

本轮将普通 ConcurrentMark 的 Helper 扩容、Assist 权重和提前 STW 统一到
`MSMarkPressurePolicy`：

- GC main 每完成 100 个 WorkBuf 执行一次 checkpoint，Helper/Assist 不更新该计数。
- Global Full+Partial debt 每 100 个 WorkBuf 对应一级 backlog pressure。
- 每个 debt 没有净下降的 checkpoint 再升一级；连续 3 个无净下降窗口请求 STW。
- checkpoint 时 debt 不超过 32 个 WorkBuf，直接把有界尾部交给 STW MarkTermination。
- OOM cycle 从启动即使用最大 Helper 与最大 Assist pressure。
- Small/Large committed Region 供给在 grow 前请求 STW；请求不阻塞分配线程，随后仍按原路径
  grow/retry。Large 类型由分配入口显式传递，因此单 Region 大对象不会误判为 Small。
- 删除 active heap 达到最大堆 90% 的提前 STW 判定，不引入 allocation-rate 或时间窗口。
  自然收敛和 `cjMSMarkTimeBudget` 固定兜底保持不变。

新增周期日志包括起始 pressure、每个 checkpoint 的 debt/debtDelta、终止原因、GC-main
完成的 WorkBuf 数、无进展窗口数和 grow 请求 Region 数。实现涉及：

```text
src/Heap/MCollector/MSMarkTerminationPolicy.h
src/Heap/MCollector/MarkSweepCollector.h
src/Heap/MCollector/MarkSweepCollector.cpp
src/Heap/MAllocator/MSRegionManager.h
src/Heap/MAllocator/MSRegionManager.cpp
test/MSMarkTerminationPolicyTest.cpp
```
