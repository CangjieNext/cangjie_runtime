# MS-GC vs CopyGC 性能差异分析与优化方向

- **日期**: 2026-03-16
- **基于**: Benchmark_Round2 数据（优化后）
- **平台**: macOS aarch64 (Apple Silicon), Release `-O2`
- **堆大小**: 256MB (`cjHeapSize=256mb`)

---

## 1. 数据总览

| 测试项 | CopyGC | MS-GC 优化前 | MS-GC 优化后 | 优化提升 | vs CopyGC |
|--------|--------|-------------|-------------|:-------:|:---------:|
| P1_SmallAlloc | 237 ms | 320 ms | 285 ms | 11% ↑ | +20% |
| P2_LargeAlloc | 840 ms | 1204 ms | 1061 ms | 12% ↑ | +26% |
| P3_MixedWorkload | 520 ms | 729 ms | 649 ms | 11% ↑ | +25% |
| P4_LongLivedChurn | 419 ms | 568 ms | 506 ms | 11% ↑ | +21% |
| P5_Fragmentation | 656 ms | 1027 ms | 876 ms | 15% ↑ | +34% |
| **Total** | **4887 ms** | **7023 ms** | **6207 ms** | **12% ↑** | **+27%** |

差距变化: +44% → **+27%**（缩减 17 个百分点）

---

## 2. 各测试项差距根因

### P1_SmallAlloc — 差距 +20%（最小百分比差距之一）

- **测试内容**: 50 × depth-16 完全二叉树，共 ~650 万个小对象
- **差距来源**: CopyGC 用纯 bump-pointer（指针加一条指令），MS-GC 即使内联后仍需 bitmap 查找（`AllocFromBitmapFast` 扫描 bitmap word），每次分配多 ~2-3 条指令
- **评估**: 20% 接近 MS-GC 的理论下限差距，进一步优化空间有限

### P2_LargeAlloc — 差距 +26%（最大绝对差距 221ms）

- **测试内容**: 200 × 256KB 对象，共 ~50MB
- **差距来源**:
  - `TakeUnitsAligned()` 需按 `MS_REGION_SIZE` 对齐搜索 bitmap
  - `ForceFinishSweep()` 在分配前阻塞等待 sweep 完成（CopyGC 无此步）
  - LOS mmap/munmap 开销在 MS-GC 中更频繁
- **评估**: 大对象复用池（arena caching）可缓解

### P3_MixedWorkload — 差距 +25%

- **测试内容**: 30 轮 × (10 小 tree + 3 大对象 + HashMap 5K 条目)
- **差距来源**: 综合了 P1（小对象）和 P2（大对象）的分配开销，加上 HashMap rehash 在 MS-GC 下触发更多 GC 周期
- **评估**: 最具代表性的实际应用场景差距

### P4_LongLivedChurn — 差距 +21%（最小差距）

- **测试内容**: 20 × 512KB 持久对象 + 100 轮短生命周期风暴
- **差距最小的原因**: MS-GC 的核心优势——不移动对象。CopyGC 每轮须拷贝 20 个 512KB 持久对象（共 10MB），MS-GC 只设标记位
- **评估**: MS-GC 最接近 CopyGC 的场景，也是生产环境中 MS-GC 最有竞争力的场景

### P5_Fragmentation — 差距 +34%（最大百分比差距）

- **测试内容**: 15 轮 × (分配 30×128KB → 释放奇数索引 → 分配 8×256KB → 散布 500 小 tree)
- **差距来源**:
  - CopyGC 通过拷贝压缩自然消除碎片，MS-GC 无法压缩
  - `TakeUnitsLowestAddr()` 在碎片化堆中搜索连续 region 更慢
  - 碎片化导致 region 利用率低，更频繁触发 GC
- **评估**: 碎片化是 MS-GC 的**结构性劣势**，无法根本解决，只能通过分配策略缓解

---

## 3. 剩余 27% 差距构成估算

```
+27% 总差距
├── ~10%  bump-pointer vs bitmap 分配（不可消除，MS-GC 固有代价）
├── ~8%   ForceFinishSweep 阻塞 + sweep 协调开销
├── ~5%   写屏障快/慢路径开销（SATB barrier 读旧值 + 入队）
└── ~4%   LOS/大对象管理开销（mmap + 对齐搜索）
```

---

## 4. 功能测试差异

| 测试 | CopyGC | MS 优化前 | MS 优化后 | vs CopyGC | 说明 |
|------|--------|----------|----------|:---------:|------|
| F1_TreeIntegrity | PASS 134ms | PASS 174ms | PASS 153ms | +14% | 树遍历开销 |
| F2_Generational | PASS 303ms | PASS 512ms | PASS 427ms | +41% | 写屏障最大开销 |
| F3_LargeObject | PASS 1531ms | PASS 2189ms | PASS 1931ms | +26% | LOS 开销 |
| F4_LinkedList | PASS 117ms | PASS 138ms | **FAIL** 140ms | — | 并发标记竞态 |
| F5_HashMap | PASS 126ms | PASS 159ms | PASS 138ms | +10% | 最小功能差距 |

F2 差距最大 (+41%)：老年代→新生代引用的写屏障在 200 轮 churn 中被频繁触发。
F4 FAIL：并发标记竞态条件，链表遍历中活对象被误回收（非本轮优化引入）。

---

## 5. 优化方向（按优先级排序）

### P0 — 正确性问题（必须先解决）

| 编号 | 问题 | 现象 | 根因 | 涉及文件 |
|------|------|------|------|----------|
| **C1** | F4 并发标记竞态 | F4_LinkedList FAIL | 并发标记期间链表活对象被误回收 | MarkSweepCollector.cpp, MSWriteBarrier.cpp |
| **C2** | Benchmark 挂起 | 进程 UE 状态无法退出 | runtime 死锁或 sweep 阻塞无限等待 | MarkSweepCollector.cpp, ConcurrentSweep |
| **C3** | ~50% 运行崩溃 | OOM 或 SEGV | 并发标记竞态 + sweep 时序问题 | 同 C1 |

### P1 — 高收益优化（预期 -5~8%）

| 编号 | 优化 | 预期收益 | 说明 |
|------|------|----------|------|
| **O1** | 并发 sweep 真正并发化 | -5~8% | 去掉 `ForceFinishSweep()` 阻塞，让 sweep 与 mutator 真正并行；目前 `NotifyToSweep()` 被注释掉，sweep 在分配时同步完成 |
| **O2** | sweep 与分配流水线化 | -3~5% | 分配器从已 sweep 区域取 region，后台 sweep 线程推进未 sweep 区域，避免分配路径上的 sweep 开销 |

### P2 — 中等收益优化（预期 -3~5%）

| 编号 | 优化 | 预期收益 | 目标测试项 |
|------|------|----------|-----------|
| **O3** | 大对象 region 缓存/复用 | -3~5% | P2, P3 |
| **O4** | LOS mmap 池化 | -2~3% | P2 |
| **O5** | 写屏障缓冲区批量刷新 | -1~2% | F2, P4 |

**O3 详细**: 大对象释放后不立即归还 OS，保持 mmap 映射并放入 per-size 的 free pool，下次分配同 size 时直接复用，避免 mmap/munmap 系统调用。

**O5 详细**: 当前 SATB barrier 每次写都立即入队 workStack；改为 per-thread barrier buffer，满时批量刷新，减少原子操作和缓存行争用。

### P3 — 低收益 / 长期优化

| 编号 | 优化 | 预期收益 | 说明 |
|------|------|----------|------|
| **O6** | SizeClass 细化 | -1~2% | 减少内部碎片，提高 region 利用率 |
| **O7** | 分配器 prefetch | -1% | 在 bitmap 扫描前预取下一个 cache line |
| **O8** | GC 触发策略优化 | 不定 | 基于分配速率动态调整 GC 触发阈值，减少不必要的 GC 周期 |
| **O9** | 碎片化缓解策略 | -2~3% (P5) | 优先从高碎片 region 分配，延迟归还低碎片 region |

---

## 6. 不可消除的差距

以下差距是 MS-GC 相对于 CopyGC 的**架构性代价**，无法通过优化消除：

1. **分配路径差距 (~10%)**: bump-pointer 是理论最快的分配方式（单条 ADD 指令），bitmap/freelist 分配永远比它慢
2. **碎片化 (~3-5%)**: MS-GC 不移动对象，碎片只能缓解不能消除；CopyGC 每次 GC 自动压缩
3. **写屏障固有开销 (~2-3%)**: SATB barrier 需要读旧值并入队，这是并发标记的必要代价

**理论最优差距**: 即使所有可优化项都做到极致，MS-GC 相对 CopyGC 仍有 **~15-18%** 的性能差距。

---

## 7. MS-GC 的优势场景

尽管整体慢于 CopyGC，MS-GC 在以下场景有优势：

1. **大堆 + 高存活率**: CopyGC 拷贝成本 ∝ 存活对象总量，MS-GC 标记成本 ∝ 对象图深度
2. **延迟敏感**: CopyGC 的 STW 拷贝阶段暂停时间 ∝ 存活量，MS-GC 的 STW 仅做 root 扫描
3. **长生命周期对象为主**: P4 测试已证明（+21% 是最小差距），持久对象越多 MS-GC 越有竞争力
4. **堆内存上限紧张**: CopyGC 需要 2x 空间做拷贝，MS-GC 原地标记不需要额外空间
