# Step24：Scavenge 关闭与 HeapGrowth 参数对吞吐影响分析

## 背景

在前序优化（Step18 GoStyle Scavenger、Step23 FreeManager Shard 化）完成后，case27（HTTP + JSON，10K payload，8并发，procs=6，heap=12gb）仍存在 MSGC 相对 CopyGC 吞吐偏低的问题。本步骤通过三组对比实验 + CPU 采样，定量分析 Scavenge 机制与 HeapGrowth 参数对 MSGC 吞吐的影响。

---

## 实验配置

| 参数 | 值 |
|---|---|
| benchmark | case27：HTTP + JSON 解析（10K payload，sync client，8并发） |
| heap 上限 | 12gb（`cjHeapSize=12gb`） |
| 处理器数 | 6（`cjProcessorNum=6`） |
| 采样工具 | macOS `sample`，15秒，1ms 间隔 |
| runtime | MSGC 模式编译（`--gc-mode marksweep`），含 `GetHotFreeRegionCount` 原子化优化 |

三组实验：

| 组别 | 关键配置 | 环境变量 |
|---|---|---|
| **no_scavenge** | 关闭后台 Scavenge | `CJ_MS_BG_SCAVENGE=0` |
| **grow1+scav** | Scavenge 开启，HeapGrowth=1 | `cjHeapGrowth=1` |
| **grow3+scav** | Scavenge 开启，HeapGrowth=3 | `cjHeapGrowth=3` |

对照基准：CopyGC（dev/20260507，相同 heap/procs 配置）

---

## 吞吐与内存结果

| 指标 | no_scavenge | grow1+scav | grow3+scav | CopyGC |
|---|---|---|---|---|
| **吞吐 (req/s)** | **9,008** | 4,552 | 7,443 | 5,732 |
| **vs CopyGC** | **+57%** | −21% | **+30%** | — |
| avg RSS (MB) | 2,883 | **182** | 671 | ~965 |
| peak RSS (MB) | 4,427 | **350** | 1,235 | ~1,789 |
| GC cycles / 60s | 162 | 1,530 | 506 | ~260 |
| avg GC time (ms) | 98.5 | 28.6 | 42.7 | — |
| scavenge 次数 | 0 | 507 | 317 | — |

---

## CPU 采样分析

### 线程阻塞率

三组 mutator 阻塞率均 ≤ 1%，无锁竞争瓶颈。`gc-pool-t1` 在三组中均几乎全程休眠（GC 并发度未饱和）。

### Mutator 活动占比（基于 ~71,000 样本）

| 活动 | no_scavenge | grow1+scav | grow3+scav |
|---|---|---|---|
| **TryEnqueueGreyObject**（写屏障入队） | 698（**0%**） | 3,101（**4%**） | 559（**0%**） |
| NewObject / NewArray | 5,067（7%） | 5,183（7%） | 5,546（7%） |
| SlowAlloc | 1,341（1%） | 1,323（1%） | 1,597（**2%**） |
| parseJson* | 3,090（4%） | 3,324（4%） | 2,891（4%） |
| WriteRefField + WriteGeneric | 963（1%） | 1,141（1%） | 863（1%） |
| **STW 暂停次数** | **129** | **406** | **128** |
| Scavenge（mutator 侧） | 0 | 1 | 1 |

### 全进程热点函数

| 函数 | no_scavenge | grow1+scav | grow3+scav |
|---|---|---|---|
| TryEnqueueGrey | 698 | **3,101** | 559 |
| NewObject | 1,353 | 1,389 | 1,462 |
| NewArray8 | 859 | 853 | 940 |
| SlowAlloc | 1,341 | 1,323 | 1,597 |
| parseJsonObject | 986 | 1,173 | 941 |
| AcquireRawData | 1,205 | 812 | 1,029 |
| mutex_wait | 515 | 446 | 661 |
| cv_wait | 118 | 363 | 129 |
| TraceObjRefFields | 82 | 377 | 83 |
| ScavengeOwnedFree | 0 | 15 | 16 |

---

## 核心结论

### 1. Scavenge 本身不是直接瓶颈

`ScavengeOwnedFreeRegionsLocked` 在样本中仅出现 15~16 次，madvise/munmap 路径本身占用 CPU 极少。

### 2. GC 频率才是主要瓶颈

关闭 Scavenge 后，hotFree regions 的物理页不再被归还给 OS，region 被复用时无需重新 page fault，**heap 保有量更高**，GC 阈值触发更晚，GC cycles 从 1,530 降至 162（**降低 89%**）。

GC 频率降低带来的连锁收益：
- `TryEnqueueGreyObject` 调用次数从 3,101 → 698（**降低 77%**）：并发标记阶段更短，mutator 执行写屏障灰对象入队的时间大幅减少
- STW 暂停次数从 406 → 129（**降低 68%**）：mutator 被 STW 打断的概率显著下降
- `cv_wait` 从 363 → 118：GC 协调等待减少

### 3. HeapGrowth 的作用机制

`cjHeapGrowth` 控制 GC 后阈值的增长倍率。增大此值使阈值更宽松，从而减少 GC 触发频率，效果与减少 Scavenge 类似——都是通过让 heap 保有更多内存来换取吞吐。

| HeapGrowth | GC cycles | TryEnqueueGrey | 吞吐 |
|---|---|---|---|
| 1（默认偏小） | 1,530 | 3,101（4%） | 4,552 req/s |
| 3 | 506 | 559（0%） | 7,443 req/s |
| 关闭 Scavenge | 162 | 698（0%） | 9,008 req/s |

grow3 已将 GC 频率和写屏障入队压到与 no_scavenge 相近的水平，但 Scavenge 开启后阈值仍会随 retain 压力而触发，所以 GC cycles 仍有 506 次（vs 162）。

### 4. 性能-内存权衡总结

| 方案 | 吞吐 vs CopyGC | peak RSS | 适用场景 |
|---|---|---|---|
| no_scavenge | **+57%** | 4.4 GB | 内存充裕、追求极限吞吐 |
| grow3+scav | **+30%** | 1.2 GB | 平衡方案 |
| grow1+scav | −21% | 350 MB | 内存极度受限 |
| CopyGC | 基准 | ~1.8 GB | — |

---

## 优化方向建议

1. **`CJ_MS_PIN_HOT_FREE` 机制（已实现）**：将 hotFree 排除出 retained 计算，让其不驱动 Scavenge 触发，同时 Step B 不释放 hotFree 物理页。效果预期介于 grow3+scav 和 no_scavenge 之间——保留 hotFree 物理页避免反复 fault，同时仍能对 emptySwept/largeCache 做 Scavenge。

2. **GC 阈值调优**：当前 HeapGrowth=1 偏小，建议将默认值调高（3~5）以减少不必要的 GC 频率，换取更好的吞吐，代价是内存占用适度增加。

3. **gc-pool-t1 利用率**：三组均显示 gc-pool-t1 几乎全程空闲，说明 GC 并发标记阶段持续时间短（或 worker 分配未饱和），并发 GC 的多线程潜力未被充分挖掘。

---

## 附：关键代码改动（本步骤）

### `CJ_MS_PIN_HOT_FREE` 环境变量实现

**`BackgroundProcessor.cpp`**：
- 新增 `IsHotFreePinned()`（读取 `CJ_MS_PIN_HOT_FREE`，默认 false）
- Step A gap 计算：pin 模式下将 hotFree 视为 0，确保 emptySwept/largeCache 仍被 flush
- Step B：pin 模式下跳过 `space.BackgroundScavenge(quantum)`，不释放 hotFree 物理页

**`MSRegionSpace.cpp`**：
- 新增本地 `IsMSHotFreePinned()`
- `GetHeapRetainedBytes()` 在 pin 模式下排除 hotFree，使其不计入 retained 压力，`MaybeNotifyScavenge`、`HasPendingScavengeWork` 均自动不受 hotFree 影响

---

## 细粒度指令统计（线程本地计数器）

运行时新增两组统计埋点，进程退出时汇总打印：

- **EnqueueRefFast**（写屏障灰对象入队）：`thread_local` 累加 → 析构时 flush 到全局 atomic，零开销
- **TakeRegions**（内存分配请求）：直接 `atomic fetch_add`，含时间戳

### 默认参数（grow1/scav，no_scavenge 对比基准）

| 配置 | heap | procs | concurrency | payload |
|---|---|---|---|---|
| 默认 MSGC（grow1+scav） | 12gb | 6 | 8 | 10K |

| 指标 | 值 |
|---|---|
| **吞吐** | 4,559 req/s |
| **EnqueueRefFast calls** | 241,197,102 |
| **TakeRegions calls** | 2,856,266 |
| TakeRegions totalTime | 25,772.817 ms |
| TakeRegions avgTime | 9.023 µs |
| NewObject | 1,118,077 |
| NewArray | 9,954,771 |

**EnqueueRefFast 均值**：241M 次 / 60s ≈ **4.0M 次/秒**，印证写屏障在高 GC 频率下对 mutator 的持续压力。

**TakeRegions avgTime 9 µs**：表明 FreeManager 分配路径（含 TryTakeRegions + GrowOneSegment）平均 9µs，整体不慢，但在高并发下累计开销（25.8s）显著。

### no_scavenge（CJ_MS_BG_SCAVENGE=0）

| 指标 | 值 |
|---|---|
| **吞吐** | 9,019 req/s |
| **EnqueueRefFast calls** | 16,288,886 |
| **TakeRegions calls** | 89,962 |
| TakeRegions totalTime | 1,067.753 ms |
| TakeRegions avgTime | 11.869 µs |
| NewObject | 2,178,777 |
| NewArray | 20,749,720 |

---

### 两组数据对比分析

| 指标 | 默认（grow1+scav） | no_scavenge | 倍率 |
|---|---|---|---|
| **吞吐** | 4,559 req/s | 9,019 req/s | **+98%** |
| **EnqueueRefFast calls** | 241,197,102 | 16,288,886 | **-93%** |
| **TakeRegions calls** | 2,856,266 | 89,962 | **-97%** |
| TakeRegions totalTime | 25,772 ms | 1,068 ms | **-96%** |
| TakeRegions avgTime | 9.0 µs | 11.9 µs | +32% |
| NewObject | 1,118,077 | 2,178,777 | +95% |
| NewArray | 9,954,771 | 20,749,720 | +108% |

**关键洞察：**

1. **EnqueueRefFast 降低 93%**：no_scavenge 使 GC 频率从 1,530 降至 162 次，并发标记时间大幅缩短，写屏障灰对象入队次数随之骤降。每次 GC 期间才会大量触发写屏障，GC 越少写屏障压力越小。

2. **TakeRegions 降低 97%**：GC 后 hotFree 不归还 OS，region 被保留在 FreeManager 中可直接复用，`TryTakeRegions` 本地命中率极高，几乎不需要 `GrowOneSegment`，TakeRegions 总调用次数因此暴降。TakeRegions avgTime 略升（9µs → 12µs）是因为热 region 的 fault 代价被摊入首次访问，但调用次数减少 97%，总开销从 25.8s 降至 1.1s。

3. **NewObject/NewArray 增加约 2 倍**：符合吞吐提升 2x 的预期——处理了更多请求，自然分配了更多对象。这说明 no_scavenge 下 GC 完全不是瓶颈，mutator 几乎全力跑业务逻辑。
