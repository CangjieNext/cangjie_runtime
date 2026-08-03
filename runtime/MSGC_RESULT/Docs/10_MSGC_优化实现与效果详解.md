# MSGC 优化实现与效果详解

> 当前代码基线：`MSGC-bugfix-regionScan` / `24ca097ace`。本文区分“源码已实现的机制”和“必须重新实测的效果”，不把旧 WorkBuf 版本的数字归因给当前版本。

## 1. 优化总览

| 层次 | 当前机制 | 主要目标 |
|---|---|---|
| Mark 表示 | mark/scan 双位图 + pending word summary | 消除普通灰对象队列生命周期 |
| Mark 更新 | 16-entry `MSMarkWordBatch` | 合并同一 bitmap word 的原子 OR |
| 调度 | REGION/OBJECT 双 lane + task ledger | 统一 main/helper/Assist 与终止判定 |
| pacing | WU/byte、WU/μs、heap runway | 将分配速度与 Mark 进度匹配 |
| Barrier | 标量即时 flush，bulk 栈上合并 | 保持 SATB，同时减少 bulk RMW |
| 分配 | BUMP/BITMAP 双路径、size class、central cache | 降低小对象分配成本 |
| Sweep | background/lazy/Assist/parallel | 隐藏回收延迟并保证供给 |
| Scavenge | shard 扫描、hot/cold free、OS release | 降低物理驻留 |
| LLVM | GCMode 分流、读侧消除、写侧 wrapper/fast gate | 减少热路径 barrier 开销 |

## 2. Region-scan Mark 的优化点

### 2.1 灰状态内嵌到 Region

小对象的待扫描集合直接是 `markBitmap & ~scanBitmap`，不再为每个新发现对象追加一条
队列记录。收益来自：

- 状态与 Sweep 所需的 mark plane 共址；
- 重复发现由 bitmap OR 自然合并；
- 一个 Region task 可批量扫描多个对象；
- scan owner 对 scan plane 单写，降低对象级同步。

代价是 Region release race、pending summary 和 sealed collect-set 的 ALLOC 有效性过滤
必须严格正确。

### 2.2 Mark-word 合并

`MSMarkWordBatch` 为 8 set × 2 way：

- 相同 `(regionIdx, wordIdx)` 的多个 child 合并成一个 mask；
- 标量 barrier 为保证 old-before-store 会立即 flush，主要获益来自 tracing 与 bulk barrier；
- entry 只保存坐标，flush 时重新确认 Region 语义；
- 每个 batch 绑定 mark epoch，防止跨周期污染。

### 2.3 Region summary

`pendingWordSummary` 用一个 bit 表示一个 bitmap word 有新增/残余 work。owner 不需要每次
从 word 0 扫到末尾；`exchange(0)` 取得当前批次，并用 `ctz` 逐 bit 处理。

### 2.4 双 lane

REGION 与 OBJECT task 分开：

- 小对象获得 Region 聚合与切片；
- large object 保留对象级扫描语义；
- GC worker 在两 lane 间交替 preference；
- mutator Assist 禁止 OBJECT，控制分配线程尾延迟。

### 2.5 ledger-based closure

Task ledger 把 queued 与 leased 合并成单一 outstanding 数。它避免“最后一个任务已被
worker 弹出，ready lane 空，但 worker 还没发布 children”造成的假终点。

## 3. WU pacing 与动态并发

成本模型：

```text
resolved ref = 1 WU
mark word RMW = 8 WU
scan word = 1 WU
```

周期反馈包括：

- completed mark WU；
- Mark 期间 allocated bytes；
- concurrent mark wall time。

由此更新：

- density：WU/byte；
- throughput：WU/μs；
- Assist work-per-byte；
- main/helper/Assist slice WU budget。

Helper target 从 `ceil(outstanding/8)` 起步，在 HIGH/CRITICAL pressure 下提升。pressure
依据 heap runway，而不是只看 ready lane 的瞬时长度。

## 4. Barrier 优化

### 4.1 语义基础

当前 deletion barrier：

```text
load old → publish/flush old → store new
```

小对象 publication 进入 mark-word batch，large object 进入 OBJECT publication。

### 4.2 Bulk 合并

struct 与 array bulk barrier 在覆盖前遍历目标旧引用，通过同一个栈上 batch 合并。这对
同一 Region/word 中密集引用尤其有效。

### 4.3 Compiler fast path 边界

可优化：

- Idle phase store-only；
- MSGC 普通 read barrier 消除；
- wrapper 选择与 phase gate；
- target 支持时的保守 inline。

不可假定：

- TLS 中存在旧 grey-object log；
- barrier 目标一定是 small object；
- compiler 可直接操作 scheduler scan state/ledger；
- 未支持 calling convention 可跳过 wrapper。

## 5. 分配器优化

仍有效的主要机制：

- `MSRegionInfo::allocPtr` offset 0 支持 BUMP fast path；
- size-class 静态映射减少除法与对象大小判断；
- BITMAP 模式利用 `allocSummary` 快速定位有空位的 word；
- 空 Region 可 `ResetToBump()`，避免重建所有外部 plane；
- central pool 维护 partial/full/empty cache；
- large allocation 独立管理连续 Region；
- lazy/background/Assist Sweep 为分配补充可用 Region。

Mark 期间分配还承担两项新职责：

1. `AccountMarkAllocationBytes()` 更新周期分配量；
2. 按 work-per-byte 形成 WU Assist debt。

## 6. Bitmap 与元数据布局

`MSRegionInfo` 仍严格为 64 字节，当前字段包括：

- `centralNextRegionIdx` 与 `markTaskNextRegionIdx`；
- `collectEpoch`；
- `control` 中的 scan state/large marked；
- `allocSummary` 与 `pendingWordSummary`；
- `allocMode`、`sizeClass`、`layout`。

三套外部 plane：

- ALLOC；
- MARK；
- SCAN（`HAS_REF` small Region 才提交/访问）。

这让元数据保持单 cache line，但新增 fast-path state/plane base 时仍必须同步检查 ABI。

## 7. Sweep 与 Scavenge

Mark 切换没有改变 Sweep 的判活依据：Sweep 读取 mark bitmap 或 large marked flag。
scan bitmap 只表示“本轮是否完成引用扫描”，不决定对象是否回收。

Sweep 继续通过：

- background coordinator sweep；
- GCThreadPool parallel helper sweep；
- mutator sweep assist；
- OOM GC main/helper emergency sweep；
- 下一轮 GC 前 force-finish；

管理回收进度。Scavenge 再把已逻辑空闲的 Region/页转化为更低物理驻留，不能与 Sweep
混为一谈。当前代码没有 allocation-miss Lazy Sweep；Small Region 的上述执行者最终都调用
同一个 `MSRegionInfo::SweepRegion()`，Large Region 则由 PostMark 的 eager sweep 单独处理。

## 8. 效果口径

当前源码能证明“机制存在”，不能单凭代码证明 wall time、RSS 或吞吐提升。特别是提交
`24ca097ace` 改变了 Mark 数据结构，旧版本的：

- 对象扫描数；
- buffer publish/flush 计数；
- Mark Assist 次数；
- concurrent/STW Mark 时间；
- case27 吞吐与失败率；

不能直接作为当前效果。

当前版本至少应采集：

| 维度 | 建议指标 |
|---|---|
| 正确性 | case25–31 成功率、`cjMSVerifyMark`、ASan/TSan |
| Mark | completed WU、REGION/OBJECT tasks、ledger peak |
| pacing | WU/byte、WU/μs、Assist debt/paid WU、pressure |
| 延迟 | concurrent mark、termination、max Assist slice |
| 堆 | true live、floating live、threshold、GC 次数 |
| Sweep/OS | backlog、force-finish、retained/RSS、scavenged bytes |

## 9. 风险与回归点

1. pending summary 与 `OWNED → IDLE` 竞态会造成 stranded mark。
2. replacement 在旧 lease 完成后才发布会制造 ledger 假零。
3. collect-set 未正确 sealed，或 ALLOC free-bit 过滤错误，会扫描不存在的 slot或遗漏旧对象。
4. bulk barrier 在覆盖后 flush 会破坏 SATB。
5. large/span 解析错误会漏发 OBJECT task。
6. scan plane 被多个 owner 写会丢 bit。
7. 只看 ready lane 而忽略 leased task 会错误终止。
8. 把历史 benchmark 归因到新版本会得出错误优化结论。

## 10. 下一步优化建议

按当前结构，优先级应是：

1. 建立 Region task/pending/ledger 的 DFX 与 stress verifier。
2. 量化 batch hit/eviction 和 mark-word RMW 降幅。
3. 基于真实 WU throughput 调整三个 slice wall budget。
4. 检查 OBJECT lane 对 long large-object scan 的尾延迟。
5. 将 compiler fast path 约束收敛到稳定公开 ABI。
6. 用同一 SDK、同一 runtime dylib、同一参数重跑 case25–31。

## 11. 源码索引

- [`MSMarkBatch.h`](../../src/Heap/MCollector/MSMarkBatch.h)
- [`MSMarkScheduler.h`](../../src/Heap/MCollector/MSMarkScheduler.h)
- [`MSWorkUnit.h`](../../src/Heap/MCollector/MSWorkUnit.h)
- [`MarkSweepCollector.cpp`](../../src/Heap/MCollector/MarkSweepCollector.cpp)
- [`MSRegionInfo.h`](../../src/Heap/MAllocator/MSRegionInfo.h)
- [`MSAllocBuffer.cpp`](../../src/Heap/MAllocator/MSAllocBuffer.cpp)
- [`MSMarkBarrier.cpp`](../../src/Heap/MBarrier/MSMarkBarrier.cpp)

## 12. 已移除历史

Global WorkBuf、多 owner 私有 partial、active log 和 one-way buffer tail closure 属于旧
Mark 方案。历史 Implementation 文档仍可用于解释为什么改架构，但不能列为当前已启用的
优化或当前 DFX 指标。
