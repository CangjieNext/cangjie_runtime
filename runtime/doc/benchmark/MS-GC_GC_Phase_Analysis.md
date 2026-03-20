# MS-GC 阶段耗时分析报告

**日期**: 2026-03-19  
**测试**: gc_benchmark (5 功能 + 5 性能)  
**配置**: cjHeapSize=256mb, macOS aarch64  
**GC 周期数**: 38

## 1. STW 时间分布

MarkSetup STW 占总 STW 的 **97.4%**，是唯一值得优化的 STW 源。

| 阶段 | 平均 STW(us) | 占比 |
|---|---|---|
| **MarkSetup STW** | **1,848** | **97.4%** |
| MarkTerm STW | 17 | 0.9% |
| SweepPrep STW | 33 | 1.7% |
| **总 STW** | **1,898** | **100%** |

## 2. 各阶段耗时总览

| 阶段 | Avg(us) | Med(us) | P95(us) | Max(us) |
|---|---|---|---|---|
| MarkSetup(含STW) | 1,936 | 1,588 | 4,474 | 5,033 |
| ConcMark | 1,774 | 198 | 24,396 | 24,611 |
| MarkTerm | 10 | 6 | 40 | 41 |
| PostMark | 975 | 701 | 3,765 | 3,780 |
| ConcSweep | 338 | 230 | 984 | 1,061 |
| **总 GC 墙钟** | **5,033** | **3,094** | **28,579** | **28,690** |

STW 占比: avg=44.5%, med=43.8%, P95=77.0%, max=80.6%

## 3. 标记量

| 指标 | Avg | Med | Max |
|---|---|---|---|
| 标记对象数 | 36,010 | 108 | 549,771 |
| trueLive(B) | 2,786,676 | 646,336 | 28,334,888 |
| LOS freed(B) | 12,958,882 | — | 总计 492MB |
| WB buffer entries | 0.2 | 0 | 1 |
| Assist calls | 0 | 0 | 0 |

## 4. 后台清扫

| 指标 | Avg | Med | P95 | Max |
|---|---|---|---|---|
| 耗时(us) | 5,278 | 4,964 | 8,243 | 8,284 |
| Regions swept | 1,874 | — | — | — |
| Regions released | 3,012 | — | — | — |

总计: swept 71,209 regions, released 114,453 regions

## 5. Top-5 最长 STW 周期

| 排名 | 周期 | totalSTW(us) | MarkSetupSTW(us) | ConcMark(us) | 标记对象数 |
|---|---|---|---|---|---|
| 1 | #5 | 5,007 | 4,968 | 205 | 105 |
| 2 | #15 | 4,420 | 4,368 | 459 | 952 |
| 3 | #4 | 4,301 | 4,240 | 206 | 105 |
| 4 | #32 | 3,674 | 3,601 | 200 | 150 |
| 5 | #2 | 3,431 | 3,306 | 24,396 | 549,771 |

## 6. 关键发现

1. **MarkSetup STW 是唯一瓶颈** — 97.4% 的 STW 时间在这里，平均 1.8ms，P95=4.4ms
2. **ConcMark 方差极大** — 中位 198us vs P95=24ms，因为大部分周期活对象极少（med=108），偶尔高达 55 万
3. **WB buffer / Assist 几乎为零** — 并发标记期间 mutator 协助负载极低
4. **PostMark 耗时不小** — 平均 975us（非 STW），需检查内部逻辑
5. **BackgroundSweep 5ms** — 非阻塞，但如果下次 GC 前未完成，ForceFinishSweep 会进入 MarkSetup STW

## 7. MarkSetup STW 深入分析

MarkSetup 内部流程（代码参考 `MarkSweepCollector.cpp:125-240`）：

```
Phase A: ForceFinishSweep     ← 在 STW 之前，并发执行
Phase B: [STW 开始]
  B1. FlushAllAllocBuffers    ← 把各 Processor 的 TL region 刷回中心池
  B2. ClearMarkBitmaps        ← memset 清零每个 region 的 markBitmap
  B3. ClearLargeMarks         ← 清除 LOS + in-heap large region 标记
  B4. TransitionToGCPhase     ← 切换 GC Phase，扫描各 mutator 栈根
         [STW 结束]
Phase D: EnumAndMarkRoots     ← STW 后，GC 线程枚举全局根
```

### 7.1 子步骤耗时拆解（38 个周期）

| 子步骤 | Avg(us) | Med(us) | P95(us) | Max(us) | 占 STW% |
|---|---|---|---|---|---|
| **ClearMarkBitmaps** | **1,796** | **1,446** | **4,302** | **4,899** | **97.2%** |
| STW 其余部分 | 51 | 52 | 79 | 83 | 2.8% |
| ForceFinishSweep (STW外) | 20 | 18 | 49 | 49 | N/A |

**结论：ClearMarkBitmaps 占 STW 的 97.2%，是真正的瓶颈。**

### 7.2 每周期详细数据

| # | Total(us) | STW(us) | ClearBM(us) | Regions | STW-CB(us) | CB占STW |
|---|---|---|---|---|---|---|
| 1 | 644 | 514 | 476 | 648 | 38 | 92.6% |
| 2 | 3,377 | 3,306 | 3,252 | 2,726 | 54 | 98.4% |
| 3 | 3,394 | 3,335 | 3,269 | 3,940 | 66 | 98.0% |
| 4 | 4,295 | 4,240 | 4,171 | 4,581 | 69 | 98.4% |
| 5 | 5,033 | 4,968 | 4,899 | 4,178 | 69 | 98.6% |
| ... | ... | ... | ... | ... | ... | ... |
| **AVG** | **1,935** | **1,848** | **1,796** | **3,068** | **51** | **97.2%** |
| **P95** | — | **4,368** | **4,302** | **4,457** | **79** | — |
| **MAX** | — | **4,968** | **4,899** | **4,581** | **83** | — |

### 7.3 Region 数量 vs ClearBitmap 耗时（相关性分析）

| Region 范围 | 样本数 | 平均 Regions | 平均耗时(us) | us/region |
|---|---|---|---|---|
| 0-1000 | 1 | 648 | 476 | 0.73 |
| 1000-2500 | 6 | 2,421 | 1,039 | 0.43 |
| 2500-4000 | 28 | 3,150 | 1,721 | 0.55 |
| 4000-6000 | 3 | 4,405 | 4,457 | **1.01** |

Region 数量达 4000+ 时，us/region 从 ~0.5 飙升到 1.0（翻倍），说明可能存在 cache/TLB 压力。

### 7.4 根因分析

**ClearMarkBitmaps 实现**（在 STW 内）：
```cpp
centralPool->VisitAllRegions([](MSRegionInfo* region) {
    size_t wordCount = (region->totalSlotCount + 63) / 64;
    memset(region->markBitmap, 0, wordCount * sizeof(uint64_t));
});
```

问题：
1. **线性遍历 3000-4500 个 region**，每个做一次 `memset`
2. 每个 region 的 `markBitmap` 在不同内存地址，**完全随机访问**，cache 不友好
3. 必须在 STW 内执行（因为需要 FlushAllAllocBuffers 后才能遍历全部 region）
4. 平均清零 3,068 个 region，耗时 1.8ms

## 8. 优化方案

### P0: ClearMarkBitmaps 优化 ✅ **已实施**

**问题**：MarkSetup STW 里的 ClearMarkBitmaps（遍历所有 region 做 memset）占 STW 97.2%。

**发现**：`SweepRegion()` 已在 sweep 后 `memset(markBitmap, 0, ...)`，`InitForSizeClass()` 和 `ResetToBump()` 也初始化 markBitmap 为零。加上 `ForceFinishSweep()` 在 STW 前保证所有 region 已 swept，**STW 中的 ClearMarkBitmaps 是 100% 冗余的**。

**实施**：
- Release 模式：完全跳过 ClearMarkBitmaps 遍历，`clearBitmapUs=0`
- Debug 模式：保留验证遍历，检查所有 markBitmap 是否确实为零，发现脏 bitmap 则报错并清理

**实测结果** (40 GC cycles vs baseline 38 cycles):

| 指标 | 优化前(avg/med) | 优化后(avg/med) | 改善 |
|---|---|---|---|
| MarkSetup STW | 1,848 / 1,485 us | **22 / 17 us** | **98.8% ↓** |
| Total STW | 1,898 / 1,536 us | **75 / 65 us** | **95.8% ↓** |
| Total wall time | 10,664 ms | **6,585 ms** | **38.2% ↓** |

**修改文件**：`src/Heap/Collector/MarkSweepCollector.cpp` — MarkSetup Phase B

### P1: 检查 PostMark 耗时来源
平均 975us 非 STW 但不低，需确认是否有可优化空间。

### P2: ConcMark 高方差
个别周期标记量大是分配模式决定的，但可检查是否存在标记路径效率问题。
