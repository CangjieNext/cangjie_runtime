# Mark-Sweep GC 改造 — Code Review 指南

> 按 Step 0 → Step 4 顺序组织，每个 Step 列出：对应设计文档、涉及文件、新增/修改的关键函数，便于逐项检视。

---

## 目录

- [Mark-Sweep GC 改造 — Code Review 指南](#mark-sweep-gc-改造--code-review-指南)
  - [目录](#目录)
  - [Step 0: 可切换 GC 框架](#step-0-可切换-gc-框架)
    - [改动文件清单](#改动文件清单)
    - [核心设计点](#核心设计点)
  - [Step 1: 分配器改造](#step-1-分配器改造)
  - [Step 2: 三色标记](#step-2-三色标记)
  - [Step 3: 混合写屏障](#step-3-混合写屏障)
  - [Step 4: 并发清扫](#step-4-并发清扫)
  - [附录: 文件总览 \& 已知遗留问题](#附录-文件总览--已知遗留问题)
    - [全部 MS 相关文件列表](#全部-ms-相关文件列表)
      - [新增文件 (17 个, ~3,951 行)](#新增文件-17-个-3951-行)
      - [修改文件 (13 个, ~750 行 `#ifdef` 块)](#修改文件-13-个-750-行-ifdef-块)
    - [已知遗留问题](#已知遗留问题)

---

## Step 0: 可切换 GC 框架

> 设计文档: `doc/Step0_可切换GC框架详细设计.md`
>
> **目标**: 在不影响 CopyGC 的前提下搭建 MS-GC 编译开关和骨架，使 Step 1-4 有统一基础设施。

### 改动文件清单

| 文件 | 类型 | 改动要点 |
|------|------|----------|
| `build.py` L109-117, L409-412 | **修改** | 新增 `--gc-mode` 参数（`copygc` / `marksweep`），映射到 `-DCOPYGC_FLAG` / `-DMARKSWEEP_FLAG` |
| `config.cmake` L29-49 | **修改** | `COPYGC_FLAG` 与 `MARKSWEEP_FLAG` 互斥校验，默认 `COPYGC=1, MARKSWEEP=0` |
| `CMakeLists.txt` L211-212, L341, L616, L687 | **修改** | `MARKSWEEP_FLAG=1` 时定义 `-DMRT_USE_MARK_SWEEP`；条件链接（不链接 WCollector）；条件编译 MS 源文件 |
| `src/Heap/Collector/CollectorProxy.h` L15-16, L28, L93-94 | **修改** | `#ifdef MRT_USE_MARK_SWEEP` 块：include `MarkSweepCollector.h`，声明 `msCollector` 成员 |
| `src/Heap/Collector/CollectorProxy.cpp` L13, L28, L37 | **修改** | `#ifdef` 块：MS 模式实例化 `MarkSweepCollector`，`GetCollector()` 返回 `msCollector` |
| `src/Heap/Heap.cpp` L13, L65-75, L158-162, L211-215 | **修改** | MS 模式：barrier 初始化为 `msIdleBarrier/msWriteBarrier`、barrier 切换逻辑、GCPhase 映射 |

### 核心设计点

1. **编译宏隔离**: 全局 `MRT_USE_MARK_SWEEP`，CopyGC 路径用 `#ifndef` 排除
2. **CollectorProxy 分发**: MS 模式翻译为 `MarkSweepCollector`，CopyGC 保持 `CopyCollector`
3. **Barrier 切换**: MS 模式只有 2 个 Barrier（`MSIdleBarrier` + `MSWriteBarrier`），替代 CopyGC 的 6 个
4. **GCPhase 扩展**: 新增 `GC_PHASE_MS_MARK_SETUP` / `GC_PHASE_MS_CONCURRENT_MARK` / `GC_PHASE_MS_MARK_TERMINATION` / `GC_PHASE_MS_SWEEP`

---

## Step 1: 分配器改造

> 设计文档: `doc/Step1_分配器改造详细设计.md`
>
> **详细内容**: [MS_CodeReview_Step1_分配器改造.md](MS_CodeReview_Step1_分配器改造.md)

**概述**: 36 级 SizeClass + 线程本地 `AllocBufferMS` + 全局 `CentralRegionPool` 四链表 + 三级大对象分配，替代 CopyGC 的 bump-only 分配器。

| 子步骤 | 主题 | 核心组件 | 新增文件 |
|--------|------|---------|---------|
| **Step 1.1** | MSRegionInfo 元数据 | 64B `MSRegionInfo` 结构体 + `MSRegionInfoArray` O(1) 镜像数组 | `MSRegionInfo.h/cpp` |
| **Step 1.2** | SizeClass 与线程本地分配 | `SizeClassInfo` 36 级查表 + `AllocBufferMS` per-thread 缓存 + `CentralRegionPool` 四链表管理 | `SizeClass.h/cpp`, `AllocBufferMS.h/cpp`, `CentralRegionPool.h/cpp` |
| **Step 1.3** | 大对象分配 | Tier1 堆内多 Region (16KB~4MB) + Tier2 独立 mmap (>4MB) + `LargeObjectSpace` | `LargeObjectSpace.h/cpp` |
| **Step 1.4** | 全流程串联 | 对象分配三级分派流程 + Region 获取/归还/状态流转 | — (串联文档) |

**关键设计点**:
- **Region 三层流转**: `RegionManager` ↔ `CentralRegionPool` (partialSwept/Unswept + fullSwept/Unswept) ↔ `AllocBufferMS`
- **Lazy Sweep 嵌入分配**: `GetRegion()` 四级优先级中 partialUnswept/fullUnswept 触发就地清扫
- **mark-at-alloc**: GC 标记阶段新分配对象自动标黑，保证本轮存活

---

## Step 2: 三色标记

> 设计文档: `doc/Step2_三色标记与Sweep详细设计.md`
>
> **详细内容**: [MS_CodeReview_Step2_标记.md](MS_CodeReview_Step2_标记.md)

**概述**: 并发三色标记替换 CopyGC 的 Trace+Forward，四阶段 GC 周期（MarkSetup → ConcurrentMark → MarkTermination → PostMarkTermination）。

| 子步骤 | 主题 | 同步机制 | 核心函数 |
|--------|------|---------|---------|
| **Step 2.1** | MarkSetup | Handshake（非 STW） | `MarkSetup()` — 清 bitmap、flush TL Region、安装写屏障、扫描根集 |
| **Step 2.2** | ConcurrentMark | 与 Mutator 并发 | `RunConcurrentMarkLoop()` — 分片 drain workStack 递归标记可达对象 |
| **Step 2.3** | MarkTermination | STW（`ScopedStopTheWorld`） | `MarkTermination()` — flush WBBuf、收集 Finalizer 复活候选、drain 残余灰对象 |
| **Step 2.4** | PostMarkTermination | Handshake（非 STW） | `PostMarkTermination()` — 弱引用清理、Finalizer 入队、大对象 Eager Sweep、切换到 sweep phase |

**关键设计点**:
- **写屏障协调**: SATB 删除屏障（`MSWriteBarrier`）+ mark-at-alloc 保证并发标记期间不丢失引用
- **收敛策略**: 连续 3 轮空栈（`kConvergenceRequiredRounds = 3`）判定并发标记收敛
- **Finalizer 复活**: MarkTermination 中未标记的 Finalizer 对象被复活（推入 mainQueue 重新标记），确保析构函数可执行
- **大对象 Eager Sweep**: PostMark 阶段同步释放 Tier1+Tier2 大对象，不走 lazy sweep 路径

---

## Step 3: 混合写屏障

> 设计文档: `doc/Step3_混合写屏障详细设计.md`
>
> **详细内容**: [MS_CodeReview_Step3_barrier.md](MS_CodeReview_Step3_barrier.md)

**概述**: 替代 CopyGC 的 SATB + 6 套读写屏障，实现无读屏障并发标记。MS-GC 仅使用 `MSIdleBarrier` + `MSWriteBarrier` 两个 Barrier，按 GC Phase 切换。

| 子步骤 | 主题 | 核心组件 | 新增/修改文件 |
|--------|------|---------|-------------|
| **Step 3.1** | MSBarrier 实现 | `MSWriteBarrier`（SATB 删除屏障）+ `MSIdleBarrier`（零开销直通） | `MSWriteBarrier.h/cpp`, `MSIdleBarrier.h` |
| **Step 3.2** | WriteBarrierBuffer | 每 Mutator 的 WBBuf，缓冲旧引用供 MarkTermination flush | `WriteBarrierBuffer.h`, `Mutator.h` |
| **Step 3.3** | 读屏障移除 | MS-GC 无对象移动 → 读操作直接 load，CopyGC override stubs | `MarkSweepCollector.cpp` |
| **Step 3.4** | Step 2 子阶段改造 | Barrier 安装/卸载时机、WBBuf flush/clear、Phase 状态机 | `Heap.cpp`, `Mutator.cpp` |

**关键设计点**:
- **设计 vs 实现差异**: 头文件注释声明 Go 风格混合屏障（shade old + shade new），但实际实现是**纯 SATB 删除屏障**（仅 shade old），新引用由 mark-at-alloc 保障
- **2 vs 6 Barrier**: CopyGC 需要 6 个阶段性 Barrier（含读屏障），MS-GC 简化为 2 个（无读屏障）
- **Finalizer → Sweep 排序**: gc-helper 线程严格先执行 ProcessFinalizables 再执行 BackgroundSweep

---

## Step 4: 并发清扫

> 设计文档: `doc/Step4_并发清扫详细设计.md`
>
> **详细内容**: [MS_CodeReview_Step4_sweep.md](MS_CodeReview_Step4_sweep.md)

**概述**: 三路协作（Lazy Sweep + Background Sweep + ForceFinishSweep），bitmap NOT 翻转算法，sweepGen 代际翻转 O(1) 切换。核心等式 `allocBitmap[i] = ~markBitmap[i]`。

| 子步骤 | 主题 | 核心组件 | 文件 |
|--------|------|---------|------|
| **Step 4.1** | SweepRegion | 64B `MSRegionInfo` bitmap NOT 清扫算法 + sweepGen CAS 三态协议 | `MSRegionInfo.h/cpp` |
| **Step 4.2** | Sweep 调度与 CentralRegionPool 集成 | sweepGen 代际机制 + `GetRegion` 四级 Lazy Sweep + `PrepareForSweep` O(1) 翻转 + `ForceFinishSweep` 兜底 | `CentralRegionPool.h/cpp` |
| **Step 4.3** | 后台 Sweep 与 FinalizerProcessor 集成 | gc-helper 线程 `BackgroundSweepLoop` + `NotifyToSweep` 接口 | `FinalizerProcessor.h/cpp` |

**关键设计点**:
- **三路协作**: Lazy Sweep（分配路径按需清扫）+ Background Sweep（gc-helper 后台清扫）+ ForceFinishSweep（下轮 GC 兜底同步清扫）
- **sweepGen CAS 协议**: `CasSweepGen(sg-2, sg-1)` 原子抢占清扫权 → `SweepRegion()` → `SetSweepGen(sg)`，保证同一 Region 只被清扫一次
- **O(1) 代际翻转**: `PrepareForSweep()` 通过 Swap(swept↔unswept) + `globalSweepGen_ += 2` 完成，无需遍历 Region
- **⚠ 接线遗漏**: `NotifyToSweep()` 已实现但在 `ConcurrentSweep()` 中未被调用，后台 Sweep 实际不会自动触发

---

## 附录: 文件总览 & 已知遗留问题

### 全部 MS 相关文件列表

#### 新增文件 (17 个, ~3,951 行)

| 文件路径 | Step | 行数 |
|----------|------|------|
| `src/Heap/Allocator/SizeClass.h` | 1.2 | 164 |
| `src/Heap/Allocator/SizeClass.cpp` | 1.2 | 77 |
| `src/Heap/Allocator/MSRegionInfo.h` | 1.1 | 252 |
| `src/Heap/Allocator/MSRegionInfo.cpp` | 1.1 | 383 |
| `src/Heap/Allocator/AllocBufferMS.h` | 1.2 | 87 |
| `src/Heap/Allocator/AllocBufferMS.cpp` | 1.2 | 181 |
| `src/Heap/Allocator/CentralRegionPool.h` | 1.2 / 4.2 | 281 |
| `src/Heap/Allocator/CentralRegionPool.cpp` | 1.2 / 4.2 | 457 |
| `src/Heap/Allocator/LargeObjectSpace.h` | 1.3 | 122 |
| `src/Heap/Allocator/LargeObjectSpace.cpp` | 1.3 | 271 |
| `src/Heap/Collector/MarkSweepCollector.h` | 2 | 187 |
| `src/Heap/Collector/MarkSweepCollector.cpp` | 2 | 829 |
| `src/Heap/Barrier/MSWriteBarrier.h` | 3.1 | 84 |
| `src/Heap/Barrier/MSWriteBarrier.cpp` | 3.1 | 287 |
| `src/Heap/Barrier/MSIdleBarrier.h` | 3.1 | 175 |
| `src/Heap/Barrier/WriteBarrierBuffer.h` | 3.2 | 100 |
| `src/Heap/MSBarrier/CMakeLists.txt` | 0 | 14 |

#### 修改文件 (13 个, ~750 行 `#ifdef` 块)

| 文件路径 | Step | `#ifdef` 块数 |
|----------|------|:---:|
| `build.py` | 0 | 1 |
| `config.cmake` | 0 | 1 |
| `CMakeLists.txt` | 0 | 4 |
| `src/Heap/Allocator/RegionSpace.h` | 1.2 | 6 |
| `src/Heap/Allocator/RegionSpace.cpp` | 1.2 / 2 | 8 |
| `src/Heap/Allocator/RegionManager.h` | 1.3 | 3 |
| `src/Heap/Allocator/RegionManager.cpp` | 1.3 | 8 |
| `src/Heap/Allocator/CartesianTree.h` | 1.3 | 2 |
| `src/Heap/Allocator/FreeRegionManager.h` | 1.3 | 1 |
| `src/Heap/Collector/CollectorProxy.h` | 0 | 3 |
| `src/Heap/Collector/CollectorProxy.cpp` | 0 | 3 |
| `src/Heap/Collector/FinalizerProcessor.h` | 4.3 | — |
| `src/Heap/Collector/FinalizerProcessor.cpp` | 4.3 | 2 |
| `src/Heap/Heap.cpp` | 0 / 3.4 | 5 |
| `src/Mutator/Mutator.h` | 3.2 | — |
| `src/Mutator/Mutator.cpp` | 3.4 | 1 |

### 已知遗留问题

| # | 问题 | 影响 | 根因 | 建议修复 |
|---|------|------|------|----------|
| 1 | **Region 地址对齐** | 偶发数据损坏（~3% 轮次） | `MS_REGION_SIZE(32KB) > UNIT_SIZE(16KB)`，奇数 unit 分配的 Region 未对齐 32KB 边界，`GetRegionInfoValidated` 启发式校正偶尔失效 | 方案 A: 令 `MS_REGION_SIZE = UNIT_SIZE`；方案 B: `TakeRegion` 强制 32KB 对齐 |
| 2 | **`MS_REGION_SPAN` 枚举未使用** | 无功能影响（dead code） | 之前 SPAN 方案失败后保留 | 清理或备注 |
| 3 | **单 AllocBufferMS** | 并发分配瓶颈 | Phase 1 设计为单实例 | Phase 2 改为 per-Processor TLS |
| 4 | **CompilerCalls 读屏障** | 函数调用开销 | 当前读操作仍走 CompilerCalls 函数 | 编译器内联 load |
| 5 | **`NotifyToSweep()` 未接线** | 后台 Sweep 不会自动触发，清扫压力集中在 Lazy Sweep（增加分配延迟）和 ForceFinishSweep（增加下轮 GC 停顿） | `ConcurrentSweep()` 中 `PrepareForSweep()` 之后未调用 `fp.NotifyToSweep()` | 在 `ConcurrentSweep()` 的 `PrepareForSweep()` 之后添加 `fp.NotifyToSweep()` 调用 |
