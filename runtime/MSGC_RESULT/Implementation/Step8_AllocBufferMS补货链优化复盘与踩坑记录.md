# Step 8：AllocBufferMS 补货链优化复盘与踩坑记录

> 2026-05-03 基于 case1_baseline + seed5 + marksweep 的连续实验记录
>
> 平台：macOS aarch64
>
> 目标：继续降低 mutator 在 AllocBufferMS::SlowAlloc / RefillFromCentral 路径上的前台补货次数，优先压低 cached_miss、slow_fresh，并提高 cached_hit

---

## 目录

1. [问题背景](#1-问题背景)
2. [本轮实验的稳定观察](#2-本轮实验的稳定观察)
3. [已验证有效的优化](#3-已验证有效的优化)
4. [已验证无效或需要回退的优化](#4-已验证无效或需要回退的优化)
5. [这几轮踩过的坑](#5-这几轮踩过的坑)
6. [当前结论与后续方向](#6-当前结论与后续方向)

---

## 1. 问题背景

在 Step 6 已经确认：case1 的主要额外成本不在 STW，而在 mutator 的 refill 路径。

进入 2026-05-03 这轮实验后，进一步确认了一个更具体的事实：

- 当前主问题已经不是 mutator 还在大量命中 unswept lazy sweep
- 真正该压的是前台 direct refill 次数，也就是 `cached_miss`、`slow_fresh`
- 同时还要控制后台 `bg_fresh`，避免后台为了补 spare region 过度走 fresh

以 hotcache_adaptive 基线为例：

- 结果目录：[20260503_205025_hotcache_adaptive_20260503_rerun_marksweep](../Result/case1_baseline/result/20260503_205025_hotcache_adaptive_20260503_rerun_marksweep/report.md)
- wall time：5457 ms
- GC 次数：185
- GC 总时间：433.09 ms
- `cached_hit/cached_miss/slow_fresh/bg_fresh = 46136/69379/62794/45146`
- `slow_partial_unswept = 0`，`slow_full_unswept = 0`
- `bg_partial_unswept = 0`，`bg_full_unswept = 0`

这意味着：

1. unswept sweep 已基本不是当前瓶颈
2. 继续优化应把目标放在“减少前台自己下场补货”和“让后台 spare 更高效”上

---

## 2. 本轮实验的稳定观察

### 2.1 当前 allocator 路径真正慢点

从基线计数可以直接看出：

- `cached_miss` 仍然很高，说明 mutator 频繁拿不到本地 cached region
- `slow_fresh` 很高，说明前台 miss 后经常直接落到 fresh region
- `bg_fresh` 也很高，说明后台 fill 也在大量消耗 fresh supply

因此，这一轮的优化重点是：

1. 提高 hottest classes 的 `cached_hit`
2. 降低 `cached_miss`
3. 减少前台 `slow_fresh`
4. 控制后台 `bg_fresh` 的代价

### 2.2 唤醒风暴不是当前第一主因

之后检查到 `BackgroundProcessor::NotifyToFeedAllocBuffers()` 走的是全局 `flag.exchange(true)` 去重，因此“重复 notify 太多”并不是单独的主矛盾。

更值得关注的是：

- 后台线程醒来之后是否在做过于粗粒度的全量扫描
- 后台 fill 的供给是否落到了不值得优先处理的 class 上

---

## 3. 已验证有效的优化

### 3.1 低风险优化：去掉 SlowAlloc 的 pre-sweep notify，并允许后台 fill fresh fallback

- 基线：[20260503_195845_slowalloc_miss_baseline_20260503_marksweep](../Result/case1_baseline/result/20260503_195845_slowalloc_miss_baseline_20260503_marksweep/report.md)
  - 5748 ms，254 GC，GC 总时间 730.03 ms
- 对比：[20260503_200038_slowalloc_lowrisk_compare_20260503_marksweep](../Result/case1_baseline/result/20260503_200038_slowalloc_lowrisk_compare_20260503_marksweep/report.md)
  - 5655 ms，224 GC，GC 总时间 608.24 ms

关键收益：

- `slow_sweep_notify: 113279 -> 0`
- `cached_hit: 2328 -> 44407`
- `slow_fresh: 103288 -> 61446`
- `bg_fresh: 0 -> 42764`

结论：当前 workload 下，SlowAlloc 的 pre-notify 是纯噪声；后台 fill 允许 fresh fallback 是真实收益点。

### 3.2 hot-class adaptive refill

- 结果：[20260503_205025_hotcache_adaptive_20260503_rerun_marksweep](../Result/case1_baseline/result/20260503_205025_hotcache_adaptive_20260503_rerun_marksweep/report.md)
- 5457 ms，185 GC，GC 总时间 433.09 ms

结论：相比 low-risk 版本，按热度给少数 class 扩容 cached slots 是有效的，能继续降低 front-end refill 压力。

### 3.3 hard pending gate 的 3 次平均优于 soft throttle

hard gate 三跑结果：

- [run1](../Result/case1_baseline/result/20260503_212358_revert_soft_to_hardgate_20260503_run1_marksweep/report.md)
- [run2](../Result/case1_baseline/result/20260503_212406_revert_soft_to_hardgate_20260503_run2_marksweep/report.md)
- [run3](../Result/case1_baseline/result/20260503_212413_revert_soft_to_hardgate_20260503_run3_marksweep/report.md)

soft throttle 参考运行：

- [run1](../Result/case1_baseline/result/20260503_212051_soft_spare_throttle_20260503_rerun1_marksweep/report.md)
- [run2](../Result/case1_baseline/result/20260503_212100_soft_spare_throttle_20260503_rerun2_marksweep/report.md)

汇总结论：

- hard gate 三跑均值：5435.67 ms，183.00 GC，GC 总时间 460.48 ms
- soft throttle 三跑均值：5610.33 ms，199.67 GC，GC 总时间 507.69 ms

结论：虽然 soft throttle 的一些计数看起来更“漂亮”，但三跑均值上 hard gate 更好。

---

## 4. 已验证无效或需要回退的优化

### 4.1 只保留 hottest 1 到 2 个 class 的扩容

- 结果：[20260503_215836_top2_focus_only_marksweep](../Result/case1_baseline/result/20260503_215836_top2_focus_only_marksweep/report.md)
- 5558 ms，183 GC，GC 总时间 454.12 ms
- `cached_hit/cached_miss/slow_fresh/bg_fresh = 42798/72692/63710/42007`

结论：把扩容范围从原始 top2/top4 收窄到 top1/top2，会明显伤害 `cached_hit`，不该保留。

### 4.2 extra spare 只允许吃 reusable supply

- 结果：[20260503_215345_top2_reusable_extra_marksweep](../Result/case1_baseline/result/20260503_215345_top2_reusable_extra_marksweep/report.md)
- 5417 ms，190 GC，GC 总时间 443.30 ms
- `cached_hit/cached_miss/slow_fresh/bg_fresh/bg_failed = 38431/77089/69284/35691/79550`

结论：虽然 `bg_fresh` 降了，但 hot spare conversion 被饿死，`cached_miss` 和 `slow_fresh` 明显恶化。

### 4.3 只给 hottest class 的 slot3 放开 fresh fallback

- 结果：[20260503_215647_top2_reusable_extra_hot0_slot3_marksweep](../Result/case1_baseline/result/20260503_215647_top2_reusable_extra_hot0_slot3_marksweep/report.md)
- 5474 ms，189 GC，GC 总时间 451.18 ms
- `cached_hit/cached_miss/slow_fresh/bg_fresh/bg_failed = 38468/77063/69234/35734/70433`

结论：相比完全 reusable-only 只是略微缓和，但仍然属于错误方向。

### 4.4 关闭 `MS-ALLOC-MISS` 这批统计

- 结果：[20260503_220635_no_alloc_miss_stats_marksweep](../Result/case1_baseline/result/20260503_220635_no_alloc_miss_stats_marksweep/report.md)
- 5584 ms，191 GC，GC 总时间 439.30 ms

结论：统计开销不是当前主矛盾，关掉计数并没有带来 wall time 收益。

### 4.5 `BackgroundFillCache` 快照化去重计算

三次运行：

- [run1](../Result/case1_baseline/result/20260503_221912_bgfill_snapshot_once_marksweep/report.md)
- [run2](../Result/case1_baseline/result/20260503_222206_bgfill_snapshot_once_rerun2_marksweep/report.md)
- [run3](../Result/case1_baseline/result/20260503_222214_bgfill_snapshot_once_rerun3_marksweep/report.md)

三跑结果：

- wall time：5600 / 5645 / 5582 ms，均值 5609.00 ms
- GC 总时间：423.05 / 420.64 / 411.90 ms，均值 418.53 ms

结论：这条实现层优化确实把 GC total 压低了，但三跑 wall time 稳定回退，不应保留。

---

## 5. 这几轮踩过的坑

### 5.1 单次结果很容易误导，必须看 3 次平均

最典型的例子就是 hard gate 与 soft throttle：

- 单次看，soft throttle 的计数更好看
- 三跑均值看，hard gate 明显更优

因此后续 allocator/refill 调优，凡是结论要落地，都必须至少三跑。

### 5.2 计数变漂亮，不等于端到端更快

这轮里多次出现：

- `bg_fresh` 降了
- `GC total` 降了
- 但 wall time 没降，甚至更差

说明 case1+seed5 下，最终目标仍然应该优先看 benchmark wall time，而不能只盯 allocator 内部计数。

### 5.3 不能再把重心放回 unswept sweep

这轮基线已经反复证明：

- `slow_partial_unswept = 0`
- `slow_full_unswept = 0`
- `bg_partial_unswept = 0`
- `bg_full_unswept = 0`

继续围绕“前台 lazy sweep 还很多”做优化，会偏离真正的问题。

### 5.4 extra spare 的 fresh gating 非常危险

一旦对 extra spare 的 fresh fallback 动手过重，就会直接把 hottest class 的 spare conversion 饿死，表现为：

- `cached_hit` 下降
- `cached_miss` 上升
- `slow_fresh` 上升
- 甚至 `bg_failed` 暴涨

这条坑已经验证过，不应重复进入。

### 5.5 实现层减计算，不一定能换来更好的吞吐

`BackgroundFillCache` 快照化说明了一件事：

- 背景 fill 代码里的重复计算确实能被去掉
- 但 current workload 下，减少这些计算并没有自动转成更好的端到端时间

所以后续优化应优先减少“做错事/做多事”，而不只是“同样的事做得更快”。

---

## 6. 当前结论与后续方向

截至 2026-05-03，这轮 allocator/refill 调优可以收敛为以下结论：

1. 当前真正的主问题是 `cached_miss`、`slow_fresh`、`bg_fresh`，不是 unswept sweep
2. 原始 top2/top4 hotcache_adaptive 仍然是当前更稳的基线
3. 对 hottest class 的扩容收窄，以及对 extra spare 的 fresh gating，都已经验证为错误方向
4. 单纯关统计或压实现层重复计算，也没有带来 wall time 改善

更合理的下一步方向应是：

- 不是继续改 refill source 的宏观策略
- 而是只让后台处理真正“脏/饿”的 alloc buffer 或 size class，避免醒来后做全量粗扫

换句话说，下一阶段更该做的是“缩小后台 fill 的作用范围”，而不是继续挤压当前这条 top2/top4 refill 策略本身。