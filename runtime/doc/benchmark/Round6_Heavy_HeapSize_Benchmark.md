# Round 6 — MS-GC vs CopyGC 高压多堆尺寸 Benchmark 对比

- **日期**: 2026-03-19
- **平台**: macOS aarch64 (Apple Silicon)
- **构建类型**: Release (`-O2`), MS-GC 使用 `--gc-mode marksweep`
- **测试集**: `gc_benchmark_heavy.cj`（加压版：F1/F3/F4/F5 功能测试 + P1-P5 性能测试）
- **堆大小**: 256MB / 512MB / 1024MB 三组对比
- **资源采样**: `ps -p PID -o rss=,%cpu=` 每 0.3s 采样

---

## 1. 测试用例说明（HEAVY 加压版 vs 原版）

`gc_benchmark_heavy.cj` 在原版 `gc_benchmark.cj` 基础上将各项参数提高 2~3 倍，用于在更大堆下测试 GC 的极限表现。

### 功能测试（与原版相同，用于正确性验证）

| 编号 | 测试名 | 内容 |
|------|--------|------|
| F1 | TreeIntegrity | depth=14 完全二叉树 ×80 轮 |
| F3 | LargeObject | 64KB~1MB 大对象 ×50 轮 |
| F4 | LinkedList | 500 节点×8KB 链表 ×30 轮 |
| F5 | HashMap | 10000 KV HashMap ×20 轮 |

> F2（Generational）因 MS-GC SIGSEGV 已知问题继续禁用。

### 性能测试参数对比

| 编号 | 测试名 | 原版参数 | HEAVY 参数 | 倍率 |
|------|--------|---------|-----------|------|
| P1 | SmallAlloc | depth=14 × 200 轮 | depth=14 × 400 轮 | 2× |
| P2 | LargeAlloc | 256KB × 200 轮 | 512KB × 400 轮 | 4× |
| P3 | MixedWorkload | 10×depth=12 + 3×256KB + 5000 HM × 30 轮 | 15×depth=12 + 5×512KB + 10000 HM × 60 轮 | ~3× |
| P4 | LongLivedChurn | 20×512KB + 100 轮翻搅 | 30×1MB + 200 轮翻搅 | ~2.5× |
| P5 | Fragmentation | 30×128KB + 8×256KB × 15 轮 | 40×256KB + 10×512KB × 30 轮 | ~3× |

---

## 2. 256MB 堆（原版 benchmark，历史基线）

数据来源：Round5 及本轮之前的测试（原版 `gc_benchmark.cj`，`cjHeapSize=256mb`）。

### 性能对比

| 测试项 | CopyGC | MS-GC | MS/Copy |
|--------|--------|-------|---------|
| P1_SmallAlloc | 237 ms | 285 ms | 1.20× |
| P2_LargeAlloc | 840 ms | 1061 ms | 1.26× |
| P3_MixedWorkload | 520 ms | 649 ms | 1.25× |
| P4_LongLivedChurn | 419 ms | 506 ms | 1.21× |
| P5_Fragmentation | 656 ms | 876 ms | 1.34× |
| **Total** | **~5007 ms** | **~7043 ms** | **1.41×** |

### 资源使用

| 指标 | CopyGC | MS-GC |
|------|--------|-------|
| Peak RSS | 212-229 MB | 172-184 MB |
| Avg RSS | 120-133 MB | ~78 MB |
| Avg CPU | 83-93% | 100-101% |
| Max CPU | 103-105% | 114-115% |
| GC Cycles | — | ~46 |

> 256MB 堆下 MS-GC 慢 41%，但内存占用反而更低（峰值少 ~25%）。

---

## 3. 512MB 堆（HEAVY benchmark）

### CopyGC 结果

| 测试项 | 结果 | 耗时 |
|--------|------|------|
| F1_TreeIntegrity | PASS | 394 ms |
| F3_LargeObject | PASS | 1458 ms |
| F4_LinkedList | PASS | 112 ms |
| F5_HashMap | PASS | 182 ms |
| P1_SmallAlloc | 13.1M nodes | 570 ms (23K nodes/ms) |
| P2_LargeAlloc | 200 MB | 3213 ms (63 MB/s) |
| P3_MixedWorkload | 60 rounds | 3563 ms |
| P4_LongLivedChurn | intact=30/30 | 1447 ms |
| P5_Fragmentation | 30 rounds | 3255 ms |
| **Total** | **4/4 PASS** | **14,197 ms** |

| 资源指标 | 值 |
|----------|-----|
| Peak RSS | 314 MB |
| Avg RSS | 137 MB |
| Avg CPU | 95% |
| Max CPU | 106% |

### MS-GC 结果

**无法完成。** 进程运行 10+ 分钟仍未输出任何 benchmark 行（stdout 为空），GC 日志持续刷新。

- stderr 显示每 ~2 秒一次 GC cycle，每次 sweep 28,000~41,000 个 region
- RSS 仅 ~2.4 MB（堆几乎未分配即被 GC 回收）
- CPU 96%（全部用于 GC）
- **诊断：GC thrashing** — 512M 堆下 sweep 速度跟不上分配速度，导致 GC 无限循环，mutator 无法执行

---

## 4. 1024MB 堆（HEAVY benchmark）

### 性能对比

| 测试项 | CopyGC | MS-GC | MS/Copy |
|--------|--------|-------|---------|
| F1_TreeIntegrity | PASS 571 ms | PASS 515 ms | 0.90× ✓ |
| F3_LargeObject | PASS 1491 ms | PASS 2014 ms | 1.35× |
| F4_LinkedList | PASS 114 ms | PASS 144 ms | 1.26× |
| F5_HashMap | PASS 118 ms | PASS 161 ms | 1.36× |
| P1_SmallAlloc | 557 ms | 748 ms | 1.34× |
| P2_LargeAlloc | 3421 ms | 4468 ms | 1.31× |
| P3_MixedWorkload | 5582 ms | 4975 ms | **0.89× ✓** |
| P4_LongLivedChurn | 1622 ms | 1956 ms | 1.21× |
| P5_Fragmentation | 3383 ms | 6528 ms | **1.93×** |
| **Total** | **16,863 ms** | **21,513 ms** | **1.28×** |

> 功能测试：两者均 4/4 PASS。

### 资源使用

| 指标 | CopyGC | MS-GC | 差距 |
|------|--------|-------|------|
| Peak RSS | 276 MB | **1,594 MB** | MS 高 5.8× |
| Avg RSS | 130 MB | 354 MB | MS 高 2.7× |
| Avg CPU | 89% | 130% | MS 高 46% |
| Max CPU | 106% | 197% | MS 高 86% |
| 采样数 | 51 | 67 | — |

---

## 5. 多堆尺寸横向对比

### 总耗时

| 堆大小 | CopyGC | MS-GC | MS/Copy | 状态 |
|--------|--------|-------|---------|------|
| 256 MB | ~5,007 ms | ~7,043 ms | 1.41× | 两者均完成 |
| 512 MB | 14,197 ms | **DNF** | ∞ | MS-GC thrashing |
| 1024 MB | 16,863 ms | 21,513 ms | 1.28× | 两者均完成 |

> 256MB 数据为原版 benchmark，512MB/1024MB 为 HEAVY 加压版，绝对值不直接可比（参数不同），但 MS/Copy 比值有参考意义。

### Peak RSS

| 堆大小 | CopyGC Peak | MS-GC Peak | 备注 |
|--------|-------------|------------|------|
| 256 MB | 212-229 MB | 172-184 MB | MS 更省内存 |
| 512 MB | 314 MB | ~2.4 MB | MS 未能分配（thrashing） |
| 1024 MB | 276 MB | **1,594 MB** | MS 峰值超堆限 56% |

### CPU

| 堆大小 | CopyGC Avg | MS-GC Avg | MS-GC Max |
|--------|-----------|-----------|-----------|
| 256 MB | 83-93% | 100-101% | 114-115% |
| 512 MB | 95% | 96% (全 GC) | — |
| 1024 MB | 89% | 130% | 197% |

---

## 6. 关键发现与分析

### 6.1 MS-GC 对堆大小极度敏感

- 1024MB 堆：可运行，慢 28%
- 512MB 堆：**完全不可用**（GC thrashing，mutator 无法执行）
- CopyGC 在 512MB 和 1024MB 下均正常完成，稳定性远优于 MS-GC

**根因**：CopyGC 复制完成后整块 region 立即释放，回收延迟极低；MS-GC 的 lazy sweep 存在回收延迟，在堆空间紧张时来不及释放 region 给分配器，导致分配被 GC 频繁阻断。

### 6.2 P5 碎片化场景是 MS-GC 最大短板

1024MB 堆下 P5_Fragmentation 慢 93%（6528ms vs 3383ms），是所有子项中差距最大的。该场景特点是大量分配 → 释放一半 → 再分配更大对象，正好暴露 MS-GC 的碎片化问题：
- sweep 后的 region 含未释放的存活对象碎片
- 再分配大对象时需要连续空间，碎片化 region 无法满足
- 被迫 GC 更频繁，每次 sweep 量更大

### 6.3 P3 混合负载 MS-GC 有优势

1024MB 堆下 P3_MixedWorkload MS-GC 快 11%（4975ms vs 5582ms）。该场景小树+大对象+HashMap 混合，GC 频率低，MS-GC 的并发标记/sweep 可以与 mutator 并行，减少整体暂停时间。

### 6.4 内存使用模式反转

- 256MB 堆：MS-GC **更省内存**（Peak 172MB vs 229MB）
- 1024MB 堆：MS-GC **更耗内存**（Peak 1594MB vs 276MB，5.8×）

这说明 MS-GC 在小堆下 GC 频率足够高、回收及时，内存占用可控；在大堆下 GC 触发阈值高、sweep 延迟导致大量未回收 region 积压，内存占用飙升。

### 6.5 CPU 消耗分析

1024MB 堆下 MS-GC 平均 CPU 130%、峰值 197%，说明后台 sweep 线程与 mutator 线程同时高负荷运行。相比之下 CopyGC 平均 89%，CPU 效率更高。

---

## 7. 结论与优化建议

### 结论

1. **MS-GC 在 1024MB 大堆下可用**，总耗时慢 28%，功能全部通过
2. **MS-GC 在 512MB 堆下 HEAVY 负载完全不可用**（GC thrashing）
3. **核心瓶颈仍是 sweep 及时性**：
   - 小堆 → 回收不及时 → thrashing
   - 大堆 → region 积压 → 内存暴涨 5.8×
   - 碎片化场景 → 性能劣化近 2×
4. **MS-GC 在混合负载下有局部优势**（P3 快 11%），说明并发回收机制本身是有效的
5. CopyGC 在所有堆大小下均表现稳定，对堆大小不敏感

### 优化方向

| 优先级 | 方向 | 预期收益 |
|--------|------|---------|
| P0 | **提高 sweep 吞吐与及时性** — 增加后台 sweep 线程数、更积极的 sweep 调度 | 解决 512M thrashing，降低大堆内存溢出 |
| P0 | **GC 触发策略优化** — 根据堆剩余比例动态调整触发阈值，避免 GC 过晚触发导致 region 积压 | 降低 Peak RSS |
| P1 | **碎片整理（Compaction）** — 对碎片化严重的 region 进行存活对象搬迁 | 改善 P5 场景 |
| P1 | **分配器快速路径优化** — 大对象分配路径的 region 获取效率 | 改善 P2/F3 |
| P2 | **Lazy sweep → Eager sweep 混合策略** — 小堆用 eager，大堆用 lazy | 平衡不同堆大小表现 |
