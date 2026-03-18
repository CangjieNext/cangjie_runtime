# Round 5 — MS-GC vs Origin (CopyGC) 综合 Benchmark 对比

- **日期**: 2026-03-17
- **平台**: macOS aarch64 (Apple Silicon)
- **构建类型**: Release (`-O2`)
- **堆大小**: 512MB (`cjHeapSize=512mb`)
- **测试集**: `gc_benchmark.cj`（F1-F5 功能测试 + P1-P5 性能测试）
- **运行次数**: 1 次
- **基线**: Origin SDK 自带 CopyGC runtime（未修改）

---

## 1. 测试用例说明

### 功能测试（正确性验证）

| 编号 | 测试名 | 内容 | 压力特征 |
|------|--------|------|----------|
| F1 | TreeIntegrity | depth=14 完全二叉树 ×80 轮，建树→checksum→countNodes，验证一致性 | 小对象密集分配（32767 节点/轮） |
| F2 | Generational | 长期树(depth=14) + 短命树(depth=12) ×500 轮，验证长期对象存活 | **已禁用**：MS-GC SIGSEGV |
| F3 | LargeObject | 64KB~1MB 大对象 ×50 轮，验证 checksum | 大对象分配+校验 |
| F4 | LinkedList | 500 节点×8KB 链表 ×30 轮，验证链表求和 | 链式引用+载荷 |
| F5 | HashMap | 10000 KV HashMap ×20 轮，验证读写 | 集合类+内部扩容 |

### 性能测试

| 编号 | 测试名 | 内容 | 主要考察 |
|------|--------|------|----------|
| P1 | SmallAlloc | depth=14 完全二叉树 ×200 轮 | 小对象分配吞吐 |
| P2 | LargeAlloc | 256KB 大对象 ×200 轮 | 大对象分配吞吐 |
| P3 | MixedWorkload | 小树+大对象+HashMap ×30 轮 | 混合负载 |
| P4 | LongLivedChurn | 20×512KB 长期对象 + 短命翻搅 ×100 轮 | 分代存活+GC 压力 |
| P5 | Fragmentation | 交替分配/释放 128KB→256KB ×15 轮 | 碎片化抗性 |

> **注意**: 树遍历函数（treeChecksum、countNodes、sumChain）均改为迭代实现，避免深递归触发 MS-GC 栈溢出。

## 2. 功能测试结果

| 测试 | Origin (CopyGC) | MS-GC | 耗时差 | 备注 |
|------|-----------------|-------|--------|------|
| F1 TreeIntegrity | **PASS** 413ms | **FAIL** 482ms | +17% | MS-GC checksum 不一致（GC 标记/清扫破坏存活对象数据） |
| F2 Generational | — | — | — | 已禁用：MS-GC 触发 SIGSEGV（长期+短命树对象混合时标记追踪错误） |
| F3 LargeObject | **PASS** 1468ms | **PASS** 1801ms | +23% | 大对象分配+校验均正确 |
| F4 LinkedList | **PASS** 122ms | **PASS** 130ms | +7% | |
| F5 HashMap | **PASS** 126ms | **PASS** 141ms | +12% | |
| **汇总** | **4/4 PASS** | **3/4 PASS** | — | MS-GC F1 正确性失败 |

### F1 FAIL 分析

F1 在 80 轮 depth=14 完全二叉树的建树+遍历中，MS-GC 返回的 checksum 在某些轮次与预期不一致。说明：
- **存活对象数据被破坏**：GC 在并发标记/清扫过程中，错误地回收或覆盖了 TreeNode 的 `val`/`left`/`right` 字段
- 疑似同一根因导致 F2 的 SIGSEGV（访问已释放内存 addr=0x100000109）
- 与之前 bugfix 文档中记录的 **写屏障不生效** 问题一致：mutator 在紧密循环中 x28 寄存器未及时刷新 GC Phase

## 3. 性能测试结果

| 测试项 | Origin (ms) | MS-GC (ms) | MS/Origin | 差距 |
|--------|------------|------------|-----------|------|
| **P1 SmallAlloc** | 280 | 335 | 1.20× | +20% |
| **P2 LargeAlloc** | 809 | 1061 | 1.31× | +31% |
| **P3 MixedWorkload** | 731 | 932 | 1.27× | +27% |
| **P4 LongLivedChurn** | 393 | 501 | 1.27× | +27% |
| **P5 Fragmentation** | 645 | 894 | 1.39× | +39% |
| **Total wall time** | **4990** | **6281** | **1.26×** | **+26%** |

### 逐项分析

**P1 SmallAlloc (+20%)**:
- CopyGC bump-pointer 分配仅需 ~3 条指令，MS-GC bitmap 扫描需 ~8-15 条指令
- 这 ~20% 差距接近理论下限，为架构性代价
- 吞吐: 23405 nodes/ms (Origin) vs 19561 nodes/ms (MS-GC)

**P2 LargeAlloc (+31%)**:
- 大对象经 MSRegionManager 分配需查找合适的连续 Region、设置 span 元数据
- 相对 CopyGC 简单的 Region 分割，路径更长
- 48 MB/s (MS-GC) vs 64 MB/s (Origin)

**P3 MixedWorkload (+27%)**:
- 混合小对象+大对象+HashMap，综合反映分配器差异
- 中等 GC 压力下 MS-GC 的 sweep 延迟开始显现

**P4 LongLivedChurn (+27%)**:
- 20 个 512KB 长期存活对象 + 短命翻搅
- `persistent_intact=20/20` 说明长期对象未丢失
- 差距主要来自短命对象的分配与 GC 回收效率

**P5 Fragmentation (+39%，差距最大)**:
- 交替分配/释放制造大量空洞
- MS-GC 的 FreeList 分配器在高碎片化环境下效率显著下降
- CopyGC 通过拷贝天然整理碎片，不受此影响

## 4. 历史轮次对比

| 轮次 | 日期 | 测试集 | CopyGC Total | MS-GC Total | MS/Copy | 核心变更 |
|------|------|--------|-------------|-------------|---------|----------|
| **R1** | 2026-03 | gc.cj (12项) | 20823ms | 21024ms | **1.01×** | MSRegionManager 迁移后首测（存活率>80%，未触发实际拷贝） |
| **R2** | 2026-03 | gc_abc_bench | 4869ms | 7023ms | **1.44×** | 基础分配器+标记修复（10项变更） |
| **R3** | 2026-03 | gc_abc_bench | 5813ms | 9291ms | **1.60×** | 更大压力场景暴露 sweep 不及时问题 |
| **R4** | 2026-03-17 | gc_abc_bench | 5749ms | 6614ms | **1.15×** | heapGrowth 0.15→0.5 + 简化阈值公式 |
| **R5** | 2026-03-17 | gc_benchmark | 4990ms | 6281ms | **1.26×** | 新综合 benchmark（功能+性能），512MB 堆 |

### 趋势分析

- R1 的 1.01× 是因为测试集恰好不触发 CopyGC 拷贝，不具参考性
- **R2→R3 退化** (1.44→1.60)：更大压力暴露了 sweep 不及时问题
- **R3→R4 大幅改善** (1.60→1.15)：GC 触发策略优化效果显著
- **R5 的 1.26×** 使用了完全不同的测试集（综合性更强），不宜直接与 R4 对比
- 跨轮次可比的核心洞察：**GC 触发频率和 sweep 及时性是影响 MS-GC 性能的最大因素**

## 5. 已知正确性问题

### 5.1 写屏障不生效（核心问题，未修复）

**症状**: F1 checksum 错误、F2 SIGSEGV、GC 日志中 `DrainSlice: obj has invalid TypeInfo=(nil)`

**根因**: 编译器快速路径通过 x28 寄存器高 8 位判断 GC Phase（ARM TBI 特性）。x28 仅在 safepoint/C-Native 边界/协程切换时刷新。Mutator 在紧密循环中长时间不命中 safepoint → x28 保持 IDLE/0 → **写屏障被永久跳过** → 并发标记阶段的对象引用变更丢失 → 存活对象被误判为垃圾并回收

**影响**: 任何包含 GC 并发标记阶段 + 密集对象引用修改的场景都可能触发

**参考**: [MarkSweep_GC写屏障不生效导致F5崩溃.md](../bugfix/MarkSweep_GC写屏障不生效导致F5崩溃.md)

### 5.2 Region 链表计数不一致

**症状**: `PrepareForSweep: fullUnswept COUNT MISMATCH! count_=N, actual traversal=M`

**影响**: 可能导致 sweep 遗漏或重复处理 Region，间接触发 OOM

### 5.3 大对象 isMarked 竞态（已修复）

**状态**: 已在 Round 2 修复（无条件 `baseInfo->isMarked = 1`）

**参考**: [MarkSweep_GC压力测试三类崩溃问题.md](../bugfix/MarkSweep_GC压力测试三类崩溃问题.md)

## 6. 性能差距根因分析

### 26% 总差距的构成（估算）

| 成分 | 贡献 | 可否优化 | 说明 |
|------|------|----------|------|
| Bump-pointer vs Bitmap 分配 | ~10% | **不可消除** | MS-GC 架构性代价，bitmap CTZ 扫描天然比 bump 递增慢 |
| ForceFinishSweep 阻塞 | ~5-8% | 可优化 | sweep 在 GC 开始前必须完成，当前实现阻塞分配线程 |
| 大对象分配路径 | ~4% | 可优化 | MSRegionManager TakeUnitsLowestAddr O(log N) 查找 |
| 碎片化累积 | ~3-5% | 部分可优化 | sweep 后 Region 碎片化导致 FreeList 效率下降 |
| SATB 写屏障开销 | ~2-3% | 可优化 | 每次对象引用写入额外检查+缓冲区入队（当前实际未生效，生效后可能增加） |
| Region 清零 (memset 32KB) | ~1-2% | 可优化 | 每个新 Region 使用前全量清零 |

### 理论最优差距

即使所有可优化项做到极致，MS-GC 仍有 **~10-15%** 的结构性差距（bitmap 分配 + 原地 sweep 的 metadata 开销）。但在 **高存活率、大堆、长生命周期对象** 场景下，MS-GC 可能反超 CopyGC（因为不需要复制）。

## 7. 优化方向与路线图

### P0 — 正确性修复（必须先于性能优化）

| 编号 | 问题 | 方案 | 预期效果 |
|------|------|------|----------|
| **C1** | **写屏障不生效** | 方案 A: `TransitionToGCPhaseExclusive` 中同步刷新所有 mutator 的 x28；方案 B: `StartTheWorld` 前刷新；方案 C: MS-GC 改为从内存加载 GCPhase 而非 x28 TBI | 修复 F1 FAIL 和 F2 SIGSEGV |
| **C2** | Region 链表计数不一致 | 审查 `PrepareForSweep` 中 swap 前后的链表操作，确保并发安全 | 消除 COUNT MISMATCH 警告 |

### P1 — 高收益优化（预期 -8~15%）

| 编号 | 优化 | 预期收益 | 说明 |
|------|------|----------|------|
| **O1** | Sweep 真正并发化 | -5~8% | 目前 ForceFinishSweep 在 GC 开始时阻塞。改为后台线程持续 sweep，GC 开始时仅等待少量未完成 Region |
| **O2** | Sweep 与分配流水线化 | -3~5% | 分配器在取 Region 时按需 lazy sweep，避免集中 sweep 的延迟 |
| **O3** | `AllocatedBytes()` 改为真实存活量 | -2~3% | 当前含未清扫垃圾，导致 GC 阈值计算偏大，过早触发 GC |

### P2 — 中收益优化（预期 -3~8%）

| 编号 | 优化 | 预期收益 | 说明 |
|------|------|----------|------|
| **O4** | MSRegionManager 双树激活 | -2~3% | releasedTree 当前永远为空，ReclaimRegion 改用 madvise+releasedTree，省去 memset 32KB |
| **O5** | 大对象 Region 缓存 | -1~2% | 大对象分配频繁时避免重复 TakeUnitsLowestAddr |
| **O6** | 写屏障缓冲区批量刷新 | -1~2% | 减少逐条处理的 cache miss |
| **O7** | Thread-local Region 缓存复用 | -1~2% | 当前线程释放 Region 后需重新申请，可复用热 Region |

### P3 — 长期优化（预期 -2~5%）

| 编号 | 优化 | 预期收益 | 说明 |
|------|------|----------|------|
| **O8** | SizeClass 细化 | -1~2% | 减少内部碎片，提高 Region 利用率 |
| **O9** | 选择性 Compaction | -1~3% | 碎片化严重的 Region 执行拷贝整理，其余原地 sweep |
| **O10** | GC 触发自适应策略 | -1~2% | 根据分配速率、存活率动态调整 heapGrowth 参数 |
| **O11** | ShrinkHeap / Region 归还 OS | 内存节省 | CentralRegionPool 中长期空闲 Region 通过 madvise 释放物理页 |

### 优化优先级时间线

```
Phase 1 (正确性):  C1 → C2           [必须优先]
Phase 2 (高收益):  O1 → O2 → O3     [预期将差距从 26% 缩至 ~15%]
Phase 3 (中收益):  O4 → O5 → O6     [预期将差距从 ~15% 缩至 ~10%]
Phase 4 (长期):    O8 → O9 → O10    [逐步逼近理论最优]
```

### MS-GC 适用场景建议

即使存在 ~10-15% 的结构性性能差距，MS-GC 在以下场景可能优于 CopyGC：
- **大堆（>1GB）**：CopyGC 需要 2× 堆空间做拷贝，MS-GC 不需要
- **高存活率（>80%）**：CopyGC 拷贝量大，MS-GC 原地 sweep 几乎零开销
- **延迟敏感**：MS-GC STW 仅 root 扫描，不含对象拷贝
- **长生命周期对象为主**：减少 GC 频率，MS-GC 分配器差距被摊薄
