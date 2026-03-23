# Step 4：并发清扫（Concurrent Sweep）详细设计

> **前置依赖**：[Step1 分配器改造](Step1_分配器改造详细设计.md) | [Step2 三色标记](Step2_三色标记与Sweep详细设计.md) | [Step3 混合写屏障](Step3_混合写屏障详细设计.md)
>
> **子文档**：
> - [Step4.1 SweepRegion 详细实现](Step4.1_SweepRegion详细实现.md) — bitmap NOT 清扫、sweepState 状态机、Region 命运判定
> - [Step4.2 Sweep 调度与 CentralRegionPool 集成](Step4.2_Sweep调度与CentralRegionPool集成.md) — sweepGen 机制、Lazy Sweep 路径、ForceFinishSweep
> - [Step4.3 后台 Sweep 与 FinalizerProcessor 集成](Step4.3_后台Sweep与FinalizerProcessor集成.md) — gc-helper 线程、Finalizer 顺序保证、物理回收
>
> 2026-03-23 当前代码状态修正：
> 1. 后台 sweep 已经接线，`MarkSweepCollector::ConcurrentSweep()` 会调用 `NotifyToSweep()`。
> 2. gc-helper 主循环入口是 `FinalizerProcessor::Run()`，不是早期文档里的 `RunMS()`。
> 3. `MS_SWEEP` phase 在 `MarkTermination` 的 STW 内就已经切好，`ConcurrentSweep()` 负责 flush alloc buffers、`PrepareForSweep()` 和通知后台清扫。

---

## 目录

1. [概述与设计目标](#1-概述与设计目标)
2. [Sweep 总体架构](#2-sweep-总体架构)
3. [Per-Region Bitmap NOT Sweep（概要）](#3-per-region-bitmap-not-sweep概要)
4. [Sweep 调度与分配路径集成（概要）](#4-sweep-调度与分配路径集成概要)
5. [后台 Sweep 与 FinalizerProcessor 集成（概要）](#5-后台-sweep-与-finalizerprocessor-集成概要)
6. [Phase 联动与 GC 周期衔接](#6-phase-联动与-gc-周期衔接)
7. [大对象清扫策略](#7-大对象清扫策略)
8. [复用与改造清单](#8-复用与改造清单)
9. [验证方案](#9-验证方案)

---

## 1. 概述与设计目标

### 1.1 Step4 的定位

Step4 是 Mark-Sweep GC 8 步迁移计划中**并发清扫**环节。在 Step2（并发标记）完成后，堆中每个 Region 的 `markBitmap` 记录了存活对象信息。Step4 负责将这些标记信息转化为可分配空间——即**将 markBitmap 翻转为 allocBitmap**，使分配器（Step1）可以在下一轮中复用已回收的 slot。

**核心等式**（[Step1.1 §3.5.4](Step1.1_MS_RegionInfo元数据改造设计.md)）：
```
allocBitmap[i] = ~markBitmap[i]    // 单次 NOT 操作 = 64 个 slot 的 sweep
```

### 1.2 设计目标

| 编号 | 目标 | 说明 |
|------|------|------|
| G1 | **零 STW 清扫** | Sweep 不在 STW 中执行，不增加标记停顿时间 |
| G2 | **三路协作** | 后台 Sweep（gc-helper）+ Lazy Sweep（分配触发）+ ForceFinishSweep（GC 前兜底）三路并行 |
| G3 | **bitmap NOT 而非 free-list 遍历** | 使用 `allocBitmap = ~markBitmap` 批量翻转，不触碰死对象内存（Step1.1 已定义） |
| G4 | **与 CentralRegionPool 四链表对齐** | Sweep 状态由 `sweepGen` 代数驱动（+2/GC），不引入新的 RegionType 枚举（Step1.2 已定义） |
| G5 | **不重复大对象回收** | 大对象回收由 [Step2.4 PostMarkTermination](Step2.4_PostMarkTermination_详细设计.md) 的 `CollectLargeGarbage()` 负责，Step4 仅引用 |
| G6 | **Sweep 阶段无需写屏障** | 当前代码在 `GC_PHASE_MS_SWEEP` 下安装的是 `MSIdleBarrier`，因此 sweep 路径不承担写屏障开销 |
| G7 | **CAS 无阻塞** | 遇到正在被他人 sweep 的 Region 直接跳过，不自旋等待（对标 Go `mcentral.cacheSpan`） |

### 1.3 与旧版 Step4 的关键差异

旧版 Step4 存在 **6 处与 Step1/Step2 设计的严重不一致**，本次全部修正：

| # | 旧版问题 | 本版修正 | 依据 |
|---|---------|---------|------|
| 1 | Free-list 遍历 sweep（FreeSlot 链表写入死对象） | `allocBitmap[i] = ~markBitmap[i]`（单 NOT，不触碰死对象） | Step1.1 §3.5.4 |
| 2 | 自定义 `AtomicRegionQueue`（全局+分桶+大对象+pinned 4 种队列） | 复用 CentralRegionPool 四链表（partialSwept/fullSwept/partialUnswept/fullUnswept） | Step1.2 §3 |
| 3 | `gcEpoch_` +1/GC | `globalSweepGen_` +2/GC（Go `mheap.sweepgen` 风格） | Step1.2 §3.1 |
| 4 | 新增 `MS_UNSWEPT_REGION` / `MS_SWEPT_PARTIAL_REGION` 等 RegionType | 使用 `MSRegionInfo.sweepState`（UNSWEPT/SWEEPING/SWEPT）+ sweepGen 代数 | Step1.1 §3.4 |
| 5 | `kNumSizeClasses = 68` | 36 个 size class（8B-16KB） | Step1.2 §1.2 |
| 6 | Step4 §8 重复实现大对象 sweep | 引用 Step2.4 `CollectLargeGarbage()`，不重复 | Step2.4 §4 |

### 1.4 编译约束

所有 Step4 新增代码位于 `#ifdef MRT_USE_MARK_SWEEP` 条件编译内，不影响 CopyGC 路径。

---

## 2. Sweep 总体架构

### 2.1 三路协作模型

Mark-Sweep 的 Sweep 阶段采用**三路协作**模型，对标 Go runtime 的 sweep 设计：

```
  ┌──────────────────────────────────────────────────────────────┐
  │                   GC 标记完成                                  │
  │   PostMarkTermination:                                        │
  │     ① CollectLargeGarbage()        // 大对象 Eager Sweep      │
  │     ② PrepareForSweep()            // globalSweepGen_ += 2    │
  │     ③ NotifyToSweep()              // 唤醒 gc-helper          │
  └────────────┬─────────────────────────┬────────────────────────┘
               │                         │
    ┌──────────▼──────────┐   ┌──────────▼──────────┐
    │ 路径 A: 后台 Sweep   │   │ 路径 B: Lazy Sweep   │
    │ (gc-helper 线程)     │   │ (分配线程按需触发)    │
    │                      │   │                      │
    │ 遍历 36 个 sizeClass │   │ GetRegion(cls):      │
    │ 的 Unswept 链表      │   │  partialSwept → 直接用│
    │ 逐个 CAS + sweep     │   │  partialUnswept →    │
    │                      │   │    CAS sweep → 用    │
    │ yield 策略:           │   │  fullUnswept →       │
    │ 每 N 个 Region 让步   │   │    CAS sweep → 用    │
    └──────────┬───────────┘   └──────────┬───────────┘
               │                          │
               │    路径 C: ForceFinishSweep                 
               │    (下轮 GC 开始前兜底)    │
               │    ┌──────────────────────┐
               └───▶│ 强制 sweep 所有残余    │◀──────────────┘
                    │ Unswept Region        │
                    │ (MarkSetup 阶段调用)  │
                    └──────────────────────┘
```

三路的关系：
- **路径 A（后台 Sweep）**：gc-helper 线程在 `FinalizerProcessor::Run()` 的主循环中检测 `shouldBackgroundSweep_`，随后执行 `BackgroundSweepLoop()`，主动遍历所有 size class 的 Unswept 链表，批量 sweep。这是 **主动** 清扫，降低 Lazy Sweep 被触发的概率。
- **路径 B（Lazy Sweep）**：当分配线程从 `CentralRegionPool::GetRegion(cls)` 获取 Region 时，如果 `partialSwept` 为空，会从 `partialUnswept`/`fullUnswept` 中取出并**原地 sweep**。这是 **被动** 清扫，将 sweep 开销分摊到分配路径。
- **路径 C（ForceFinishSweep）**：下轮 GC 的 `MarkSetup` 阶段调用，强制 sweep 所有尚未完成的 Unswept Region（冷 size class 等），确保 markBitmap 已全部清零，可安全开始新一轮标记。对标 Go `gcStart() → finishsweep_m()`。

### 2.2 Phase 转换与 Sweep 触发时机

```
MS_PHASE_MARK_TERMINATION (STW)
  │
  │  PostMarkTermination (非 STW):
  │    ├─ Task 1-5: 弱引用/Finalizer/循环引用/...(Step2.4)
  │    ├─ Task 6: CollectLargeGarbage()          // 大对象 Eager Sweep
  │
  │  ConcurrentSweep():
  │    ├─ STW: FlushAllAllocBuffers + PrepareForSweep()
  │    ├─ NotifyToSweep()
  │    └─ NotifyToReclaimGarbage()
  │
  ▼
GC_PHASE_MS_SWEEP
  │
  │  Barrier: MSIdleBarrier（无写屏障开销）
  │
  ├─ gc-helper: BackgroundSweepLoop()             // 路径 A 启动
  │
  ├─ Mutator: 正常分配，按需 Lazy Sweep           // 路径 B
  │
  └─ 下轮 GC MarkSetup: ForceFinishSweep()       // 路径 C 兜底
```

关键点：
1. **Sweep 发生在 `GC_PHASE_MS_SWEEP` 期间**，此时实际安装的是 `MSIdleBarrier`（直接 store，无标记开销）
2. **sweepGen 代际翻转是 O(1)**：`globalSweepGen_ += 2`，原来 partialSwept/fullSwept 中 `sweepGen == oldGen` 的 Region 自动变为 Unswept（无需移动链表节点）
3. **大对象已在 PostMarkTermination 中处理**，Step4 的三路协作仅针对小对象 Region

### 2.3 数据流概览

```
                     markBitmap               allocBitmap
                    (Step2 写入)              (Step4 产出)
                         │                        │
  ┌──── Region ──────────┤                        │
  │  slot 0:  alive  ──▶ │ bit=1 ──→ ~1=0 ──▶ │ bit=0 (occupied)
  │  slot 1:  dead   ──▶ │ bit=0 ──→ ~0=1 ──▶ │ bit=1 (free)     ← 可分配
  │  slot 2:  alive  ──▶ │ bit=1 ──→ ~1=0 ──▶ │ bit=0 (occupied)
  │  slot 3:  dead   ──▶ │ bit=0 ──→ ~0=1 ──▶ │ bit=1 (free)     ← 可分配
  │  ...                  │                     │
  └───────────────────────┘                     └──────────────────

  allocBitmap[i] = ~markBitmap[i]   // 1 条指令处理 64 个 slot
  memset(markBitmap, 0, bitmapSize) // 清零为下轮标记准备
```

### 2.4 与 Go runtime 的对应

| 概念 | Go runtime | Cangjie MS (Step4) |
|------|-----------|-------------------|
| 后台 sweep | `bgsweep()` goroutine | gc-helper `BackgroundSweepLoop()` |
| 分配触发 sweep | `mcentral.cacheSpan()` 内部 | `CentralRegionPool::GetRegion()` 内部 |
| GC 前强制完成 | `gcStart() → finishsweep_m()` | `MarkSetup → ForceFinishSweep()` |
| 代际计数器 | `mheap.sweepgen` (+2/GC) | `globalSweepGen_` (+2/GC) |
| sweep 所有权 | `CAS(span.sweepgen, sg-2, sg-1)` | `CAS(region.sweepGen, sg-2, sg-1)` |
| 四链表 | `mcentral.partial/full[swept/unswept]` | `CentralRegionPool.SizeClassBucket` 四链表 |

---

## 3. Per-Region Bitmap NOT Sweep（概要）

> **详细设计见 → [Step4.1 SweepRegion 详细实现](Step4.1_SweepRegion详细实现.md)**

### 3.1 核心算法

每个小对象 Region（≤16KB slot）的 sweep 由 `MSRegionInfo::SweepRegion()` 完成，算法核心是 **bitmap 按位取反**（[Step1.1 §3.11](Step1.1_MS_RegionInfo元数据改造设计.md)）：

```cpp
void MSRegionInfo::SweepRegion() {
    size_t wordCount = (totalSlotCount + 63) / 64;
    uint16_t live = 0;
    for (size_t i = 0; i < wordCount; i++) {
        allocBitmap[i] = ~markBitmap[i];           // 1 NOT = 64 slots
        live += __builtin_popcountll(markBitmap[i]); // 统计存活
    }
    memset(markBitmap, 0, wordCount * sizeof(uint64_t)); // 清零为下轮准备
    liveSlotCount = live;
    freeSlotCount = totalSlotCount - live;
    sweepState = MS_SWEEP_STATE_SWEPT;
}
```

**关键优势**：
- **不触碰死对象内存**：仅操作 bitmap，不写入 FreeSlot 链表到死对象空间
- **批量处理**：1 条 NOT 指令处理 64 个 slot，SIMD 优化潜力大
- **O(N/64) 复杂度**：N 为 slot 总数，远优于 O(N) 的逐对象遍历

### 3.2 sweepState 状态机

```
  UNSWEPT ──CAS──▶ SWEEPING ──完成──▶ SWEPT
    │                  │
    │  CAS 失败        │  异常中断
    │  (他人已抢占)     │  (不应发生)
    ▼                  ▼
  跳过此 Region     回滚/标记异常
```

- `MS_SWEEP_STATE_UNSWEPT (0)`：未清扫，markBitmap 有效
- `MS_SWEEP_STATE_SWEEPING (1)`：正在被某线程清扫（CAS 抢占中）
- `MS_SWEEP_STATE_SWEPT (2)`：已清扫，allocBitmap 已翻转，markBitmap 已清零

### 3.3 Region 命运判定

sweep 完成后根据 `freeSlotCount` 判定 Region 去向：

| 条件 | 去向 | 说明 |
|------|------|------|
| `freeSlotCount == totalSlotCount` | `ReleaseRegion()` → FreeRegionManager | 全死，物理回收 |
| `0 < freeSlotCount < totalSlotCount` | 返回 `partialSwept` 链表 | 有空洞，可分配 |
| `freeSlotCount == 0` | 返回 `fullSwept` 链表 | 全满，等下轮 GC |

---

## 4. Sweep 调度与分配路径集成（概要）

> **详细设计见 → [Step4.2 Sweep 调度与 CentralRegionPool 集成](Step4.2_Sweep调度与CentralRegionPool集成.md)**

### 4.1 CentralRegionPool 四链表回顾

[Step1.2 §3](Step1.2_SizeClass与线程本地分配设计.md) 定义了 `CentralRegionPool` 每个 size class 维护 4 条链表：

```cpp
struct SizeClassBucket {
    MutexLock lock;
    RegionList partialSwept;    // 有空洞 + 已 sweep（最快路径）
    RegionList fullSwept;       // 全满 + 已 sweep
    RegionList partialUnswept;  // 有空洞 + 未 sweep（上轮残留）
    RegionList fullUnswept;     // 全满 + 未 sweep（上轮残留）
};
```

**代际翻转**：每轮 GC 完成后 `globalSweepGen_ += 2`，原来的 swept 自动变为 unswept（sweepGen 不再匹配新代数）。**O(1)，无需移动链表节点**。

### 4.2 Lazy Sweep 四级获取优先级

分配线程通过 `CentralRegionPool::GetRegion(cls)` 获取 Region，优先级如下（对标 Go `mcentral.cacheSpan`）：

```
GetRegion(cls):
  ① partialSwept[cls] 非空 → pop → 直接用                ← 最快，无 sweep 开销
  ② partialUnswept[cls] 非空 → pop → CAS sweep → 用     ← Lazy Sweep 触发
  ③ fullUnswept[cls] 非空 → pop → CAS sweep              ← Lazy Sweep 触发
       ├─ 全死 → Release → FreeRegionManager → 重试
       ├─ 有空洞 → 用
       └─ 仍全满 → 放回 fullSwept → 重试
  ④ 全部为空 → GetFreshRegion(cls) → FreeRegionManager   ← 新 Region
```

### 4.3 sweepGen CAS 抢占机制

```
对标 Go: CAS(span.sweepgen, sg-2, sg-1)

  region.sweepGen == sg - 2  →  未 sweep（需要清扫）
  region.sweepGen == sg - 1  →  正在被某线程 sweep（跳过）
  region.sweepGen == sg      →  已 sweep（可直接使用）
```

- CAS 成功 → 当前线程获得 sweep 所有权 → 执行 `SweepRegion()` → `sweepGen = sg`
- CAS 失败 → 其他线程已抢占 → **跳过，不等待**（避免自旋阻塞）

### 4.4 ForceFinishSweep

下轮 GC 的 `MarkSetup` 阶段调用（对标 Go `finishsweep_m()`），强制 sweep 所有 36 个 size class 的残余 Unswept Region：

```cpp
void CentralRegionPool::ForceFinishSweep() {
    for (uint8_t cls = 0; cls < NUM_SIZE_CLASSES; cls++) {
        // drain partialUnswept[cls] + fullUnswept[cls]
        // sweep each → 按结果分入 partialSwept/fullSwept/Release
    }
    // 完成后：所有 markBitmap 已清零，可安全开始新一轮 Mark
}
```

---

## 5. 后台 Sweep 与 FinalizerProcessor 集成（概要）

> **详细设计见 → [Step4.3 后台 Sweep 与 FinalizerProcessor 集成](Step4.3_后台Sweep与FinalizerProcessor集成.md)**

### 5.1 gc-helper 线程职责扩展

现有 `FinalizerProcessor` 运行在 gc-helper 线程上。当前代码中后台 sweep 已通过其现有 `Run()` 主循环接线：

```
FinalizerProcessor::Run() 主循环:
  ① ProcessFinalizables()           // 最优先：执行 finalizer
  ② BackgroundSweepLoop()           // 新增：后台 sweep
  ③ FeedHungryBuffers()             // 预分配 region
  ④ ReclaimHeapGarbage()            // 物理回收
  → Wait()                          // 等待下一次唤醒
```

### 5.2 Finalizer 与 Sweep 的顺序保证

**关键约束**：Finalizer 必须在 Sweep 之前执行。

原因：`MarkTermination` 将未标记但有 finalizer 的对象标记为 resurrected（存活）。这些对象需要先执行 finalizer，然后在**下轮** GC 中才被真正回收。如果先 sweep 再执行 finalizer，可能访问已清零的对象。

```
MarkTermination: enqueue finalizable objects (mark as resurrected)
       │
       ▼
gc-helper 唤醒:
  ① ProcessFinalizables()    // 先执行 finalizer，resurrected 对象安全访问
  ② BackgroundSweepLoop()    // 再 sweep，resurrected 对象不会被回收
```

### 5.3 物理回收路径

Sweep 产出的完全空闲 Region 通过现有基础设施回收：

```
SweepRegion() → 全死(freeSlotCount == totalSlotCount)
  → ReleaseRegion()
    → FreeRegionManager (dirty 树)
      → ReleaseGarbageRegions()
        → madvise(MADV_DONTNEED) (released 树)
```

复用 [Step1.2 §3](Step1.2_SizeClass与线程本地分配设计.md) 的 `ReleaseRegion()` 和现有 `RegionManager` 的 `ReclaimGarbageMemory()` 三阶段物理回收，不新增回收路径。

---

## 6. Phase 联动与 GC 周期衔接

### 6.1 sweepGen 生命周期

`globalSweepGen_` 是一个 `std::atomic<uint32_t>` 全局计数器，每轮 GC 完成后 +2（对标 Go `mheap.sweepgen`）。

```
初始状态: globalSweepGen_ = 0

GC #1:
  MarkTermination → PostMarkTermination:
    PrepareForSweep(): globalSweepGen_ = 2
    所有 Region.sweepGen == 0 → 需要 sweep (0 < 2)
  Sweep 阶段:
    sweep Region R → R.sweepGen = 2 (已 sweep)
  下轮 GC 开始前:
    ForceFinishSweep(): 强制 sweep 所有 sweepGen < 2 的 Region

GC #2:
  MarkTermination → PostMarkTermination:
    PrepareForSweep(): globalSweepGen_ = 4
    上轮 swept Region.sweepGen == 2 → 变为 unswept (2 < 4)  // 隐式翻转
  ...
```

### 6.2 GC 周期时序图

```
时间 ──────────────────────────────────────────────────────▶

     GC #N                                    GC #N+1
  ┌────────┐                               ┌────────┐
  │ Mark   │                               │ Mark   │
  │ Setup  │  Concurrent   Mark    Post    │ Setup  │
  │(Hshake)│    Mark      Term.   Mark     │(Hshake)│
  └──┬─────┘              (STW)            └──┬─────┘
     │                      │                  │
     │  ForceFinishSweep()  │  PrepareForSweep │  ForceFinishSweep()
     │  (清零上轮残余)       │  (sweepGen += 2) │  (清零本轮残余)
     │                      │                  │
     │                      ▼                  │
     │            ┌─────────────────┐          │
     │            │   Sweep 阶段     │          │
     │            │                 │          │
     │            │ A: 后台 sweep    │          │
     │            │ B: Lazy sweep   │          │
     │            │                 │          │
     │            │ Barrier: Idle   │          │
     │            │ (无写屏障开销)   │          │
     │            └────────┬────────┘          │
     │                     │                   │
     │              C: ForceFinishSweep ───────┘
```

### 6.3 跨周期安全性保证

| 保证项 | 机制 | 说明 |
|--------|------|------|
| markBitmap 清零 | `SweepRegion()` 内 memset | 每个被 sweep 的 Region 的 markBitmap 立即清零 |
| 残余 sweep 完成 | `ForceFinishSweep()` | MarkSetup 前强制清完，确保新一轮标记安全 |
| 无重复 sweep | CAS sweepGen 抢占 | 同一 Region 不会被两个线程同时 sweep |
| 新分配对象安全 | `GetFreshRegion()` sweepGen=当前代 | 新 Region 不需要 sweep |
| 分配路径无阻塞 | CAS 失败即跳过 | 遇到正在被 sweep 的 Region 不等待 |

### 6.4 与 Step2 各阶段的交互

| Step2 阶段 | Step4 交互 | 说明 |
|-----------|-----------|------|
| MarkSetup (Handshake) | `ForceFinishSweep()` | 确保上轮 sweep 全部完成 |
| | `FlushAll()` → CentralRegionPool | 归还 activeRegion，使 GC 可见 |
| ConcurrentMark | 无 | 标记期间 Mutator 仍可分配（Lazy Sweep 正常工作） |
| MarkTermination (STW) | 无 | 标记完成，暂不触发 sweep |
| PostMarkTermination | `CollectLargeGarbage()` | 大对象 Eager Sweep（Step2.4 负责） |
| | `PrepareForSweep()` → sweepGen += 2 | 触发新一轮 sweep |
| | `NotifyBackgroundSweep()` | 唤醒 gc-helper 开始后台 sweep |

---

## 7. 大对象清扫策略

### 7.1 设计决策：不重复实现

大对象（>16KB）的清扫**不在 Step4 中实现**，而是在 [Step2.4 PostMarkTermination](Step2.4_PostMarkTermination_详细设计.md) 的 Task 6 `CollectLargeGarbage()` 中完成。这是一个**有意的设计决策**，原因如下：

1. **已有完整实现**：Step2.4 §4 已定义 `CollectLargeGarbage()` 的完整流程（遍历 LargeRegionList + LargeObjectSpace，检查 mark bit，按大小释放）
2. **Eager Sweep 而非 Lazy**：大对象使用 **Eager Sweep**（标记完成后立即回收），不使用 Lazy Sweep（[Step1.3 §4.2](Step1.3_大对象分配与堆增长设计.md)）
3. **避免重复代码**：旧版 Step4 §8 重复实现了 `SweepLargeRegion()` + `HandleLargeEmptyRegion()`，与 Step2.4 逻辑完全重叠

### 7.2 大对象回收流程（Step2.4 负责）

```
PostMarkTermination → CollectLargeGarbage():
  │
  ├─ 遍历 LargeRegionList (Tier1: 16KB-4MB):
  │    ├─ marked → ResetMarkBit(), 保留
  │    └─ unmarked → 从链表移除 → FreeRegionManager (dirty 树)
  │
  └─ 遍历 LargeObjectSpace (Tier2: >4MB):
       ├─ marked → ResetMarkBit(), 保留
       └─ unmarked → munmap 释放物理页
```

### 7.3 Step4 对大对象的唯一交互

Step4 不 sweep 大对象，但需要**确保 sweep 完成性检查包含大对象**：

```cpp
bool IsSweepComplete() {
    // 小对象 sweep 完成检查
    if (!centralPool_.AllSwept()) return false;
    // 大对象 sweep 在 PostMarkTermination 中同步完成，无需异步检查
    return true;
}
```

大对象回收是**同步的**（在 PostMarkTermination 的 `CollectLargeGarbage()` 中一次性完成），不涉及 Lazy Sweep 或后台 sweep。

---

## 8. 复用与改造清单

### 8.1 总览矩阵

| 组件 | 来源 | 复用方式 | 说明 |
|------|------|----------|------|
| **MSRegionInfo::SweepRegion()** | Step1.1 §3.11 | ✅ 直接复用 | bitmap NOT + popcount + memset 清零 |
| **CentralRegionPool** | Step1.2 §3 | ✅ 直接复用 | 四链表 + sweepGen + GetRegion/ForceFinishSweep |
| **FreeRegionManager** | 现有 | ✅ 直接复用 | dirty/released 树不变 |
| **CartesianTree** | 现有 | ✅ 直接复用 | MergeInsert/TakeUnits 不变 |
| **RegionList** | 现有 | ✅ 直接复用 | 双向链表操作不变 |
| **ReclaimGarbageMemory** | 现有 RegionSpace | ✅ 直接复用 | 三阶段物理回收不变 |
| **CollectLargeGarbage** | Step2.4 | ✅ 直接复用 | 大对象 Eager Sweep 不变 |
| **FinalizerProcessor** | 现有 | 🔄 修改 | 在现有 `Run()` 主循环中接入 `BackgroundSweepLoop()` + `NotifyToSweep()` |
| **RegionManager** | 现有 | 🔄 修改 | 新增 `fullRegionList`（MS 模式） |

### 8.2 Step4 不新增的组件

以下旧版 Step4 中的组件**不再需要**：

| 旧组件 | 原因 | 替代方案 |
|--------|------|---------|
| `AtomicRegionQueue` | 自定义队列与 Step1.2 CentralRegionPool 重复 | CentralRegionPool 四链表 |
| `ConcurrentSweeper` 独立类 | 过度抽象，sweep 逻辑已分布在 SweepRegion + CentralRegionPool + FinalizerProcessor | 各组件内联 |
| `SweepSmallRegion()` free-list 版 | 使用 FreeSlot 链表写入死对象 | `MSRegionInfo::SweepRegion()` bitmap NOT |
| `SweepLargeRegion()` | 与 Step2.4 `CollectLargeGarbage()` 重复 | 不实现，引用 Step2.4 |
| `gcEpoch_` (+1/GC) | 与 Step1.2 sweepGen (+2/GC) 不一致 | `globalSweepGen_` (+2/GC) |
| `MS_UNSWEPT_REGION` 等 RegionType | 与 Step1.1 MSRegionInfo.sweepState 不一致 | `sweepState` 字段 |

### 8.3 需修改的现有文件

| 文件 | 修改内容 |
|------|----------|
| `src/Heap/Collector/FinalizerProcessor.h/cpp` | 在现有 `Run()` 主循环中接入 `BackgroundSweepLoop()`、`NotifyToSweep()`、`shouldBackgroundSweep_` |
| `src/Heap/Collector/MarkSweepCollector.cpp` | `ConcurrentSweep()` 中调用 `PrepareForSweep()` + `NotifyToSweep()` + `NotifyToReclaimGarbage()` |
| `src/Heap/Collector/MarkSweepCollector.cpp` | `MarkSetup()` 中通过 `WaitForSweepDone()` / `ForceFinishSweep()` 保证上轮 sweep 收敛 |
| `CMakeLists.txt` | 无新增文件（Step4 不创建独立 Sweeper 目录）|

### 8.4 改造优先级

| 优先级 | 任务 | 前置依赖 | 预估 |
|--------|------|----------|------|
| P0 | `MSRegionInfo::SweepRegion()` 实现 | Step1.1 MSRegionInfo 完成 | 1 天 |
| P0 | `CentralRegionPool` sweepGen + Lazy Sweep | Step1.2 CentralRegionPool 完成 | 2 天 |
| P1 | 在 `FinalizerProcessor::Run()` 中接入 BackgroundSweepLoop | Step2 MarkSweepCollector + P0 | 2 天 |
| P1 | `ForceFinishSweep()` 集成到 MarkSetup | P0 + Step2.1 MarkSetup | 1 天 |
| P1 | `PrepareForSweep()` 集成到 PostMarkTermination | P0 + Step2.4 | 0.5 天 |
| P2 | 端到端集成测试 | 全部 P0/P1 | 2 天 |

---

## 9. 验证方案

### 9.1 单元测试

#### 9.1.1 SweepRegion bitmap NOT 正确性

```
测试名: TestSweepRegion_BitmapNOT
前置: sizeClass=4 (64B slot) 的 Region, totalSlotCount=512
      分配 100 个对象 (allocBitmap 前 100 bit 为 0, 表已分配)
      标记 60 个存活 (markBitmap 60 个 bit 为 1)
步骤:
  1. 调用 SweepRegion()
  2. 验证 allocBitmap[i] == ~markBitmap_original[i] (全部 word)
  3. 验证 liveSlotCount == 60
  4. 验证 freeSlotCount == 512 - 60 == 452
  5. 验证 markBitmap 已全部清零
  6. 验证 sweepState == MS_SWEEP_STATE_SWEPT
预期: bitmap 正确翻转，统计准确
```

```
测试名: TestSweepRegion_AllDead
前置: Region 有 200 个已分配对象，markBitmap 全 0（全死）
步骤:
  1. SweepRegion()
  2. 验证 allocBitmap 全 1（全部 slot 可分配）
  3. 验证 liveSlotCount == 0, freeSlotCount == totalSlotCount
预期: 全死 Region → IsCompletelyFree() == true
```

```
测试名: TestSweepRegion_AllAlive
前置: Region 全部对象标记存活
步骤:
  1. SweepRegion()
  2. 验证 allocBitmap 全 0（无可分配 slot）
  3. 验证 freeSlotCount == 0
预期: 全活 Region → 放回 fullSwept 链表
```

#### 9.1.2 sweepGen CAS 正确性

```
测试名: TestSweepGenCAS_ConcurrentClaim
线程数: 8
Region 数: 100, 全部 sweepGen = sg-2
步骤:
  1. 8 线程并发 CAS(region.sweepGen, sg-2, sg-1)
  2. CAS 成功者执行 SweepRegion() → sweepGen = sg
  3. 验证: 每个 Region 恰好被 sweep 1 次
  4. 验证: 所有 Region 最终 sweepGen == sg
预期: 无重复 sweep，无遗漏
```

#### 9.1.3 ForceFinishSweep 完成性

```
测试名: TestForceFinishSweep_DrainAll
前置: 36 个 sizeClass 各有 10 个 Unswept Region（360 总计）
步骤:
  1. 调用 ForceFinishSweep()
  2. 验证: 所有 partialUnswept/fullUnswept 链表为空
  3. 验证: 所有 Region 的 markBitmap 已清零
  4. 验证: 全死 Region 已进入 FreeRegionManager
  5. 验证: 存活 Region 在 partialSwept 或 fullSwept 中
预期: 无残余 Unswept Region
```

### 9.2 并发安全测试

#### 9.2.1 后台 Sweep + Lazy Sweep 并发

```
测试名: TestBackgroundAndLazySweep_Concurrent
线程: 1 gc-helper + 4 Mutator
Region: 200 个混合存活率 Region
步骤:
  1. PrepareForSweep(): sweep 启动
  2. gc-helper: BackgroundSweepLoop() 开始
  3. 4 Mutator 同时通过 GetRegion() 触发 Lazy Sweep
  4. 运行至 sweep 完成
  5. 验证:
     a. 无 Region 被 sweep 两次
     b. backgroundSweepCount + lazySweepCount == 200
     c. 所有 PARTIAL Region 的 allocBitmap 可用
     d. 所有 EMPTY Region 已进入 FreeRegionManager
预期: 无竞态，无重复 sweep
```

#### 9.2.2 Sweep + Allocate 交叉

```
测试名: TestSweepAllocInterleave
线程: 1 gc-helper + 8 Mutator
运行: 5 秒连续
步骤:
  1. Mutator 持续分配+解除引用
  2. 每秒触发 1 次 GC → 并发 sweep
  3. Mutator 分配过程中自动 Lazy Sweep
  4. 验证:
     a. 分配从不失败（除真正 OOM）
     b. 无 use-after-free（ASAN 检查）
     c. 分配到的 slot 不与存活对象重叠
预期: 分配和 sweep 安全交错
```

### 9.3 集成测试

#### 9.3.1 完整 GC 周期

```
测试名: TestMS_FullCycle_ConcurrentSweep
步骤:
  1. 分配 10000 小对象 + 100 大对象
  2. 随机引用图（50% 可达）
  3. 触发 GC: Mark → MarkTermination → PostMark (CollectLargeGarbage + PrepareForSweep)
  4. 并发 Sweep (后台 + Lazy)
  5. 验证: 可达对象存活，不可达被回收
  6. 再分配 5000 对象 (复用 sweep 后的 allocBitmap)
  7. 验证: 新分配不与存活对象冲突
预期: 完整 MS 周期正确
```

#### 9.3.2 多轮 GC 稳定性

```
测试名: TestMS_MultiCycle_100Rounds
运行: 100 轮 GC
每轮: 分配 1000 → 解引用 500 → GC → Sweep
验证:
  - sweepGen 单调递增 (每轮 +2)
  - 堆大小不持续增长（无泄漏）
  - ForceFinishSweep 每轮清空残余
预期: 100 轮后内存稳定
```

### 9.4 性能基准

| 测试项 | 指标 | 目标 |
|--------|------|------|
| STW 时间 | MarkTermination 停顿 | 不含 sweep 时间（vs 旧版 STW sweep） |
| Sweep 总耗时 | 后台+Lazy 完成延迟 | 持平或更快（bitmap NOT vs free-list） |
| 分配延迟 P99 | 分配路径 Lazy sweep 开销 | ≤ 10μs |
| Lazy sweep 频率 | 分配触发 sweep 比例 | < 20%（后台 sweep 兜底大部分） |

### 9.5 回归验证清单

| 验证项 | 方法 | 通过标准 |
|--------|------|----------|
| gc.cj 基准测试 | `cjc gc.cj && ./main` | 输出正确，无崩溃 |
| gc_large.cj 大对象 | `cjc gc_large.cj && ./main` | 大对象分配+回收正常 |
| gc_mixed.cj 混合 | `cjc gc_mixed.cj && ./main` | 混合场景正常 |
| Finalizer 顺序 | 自定义 finalizer 测试 | finalizer 在 sweep 前执行 |
| 多轮 GC | 连续 1000 轮 | 内存稳定，无泄漏 |
| ASAN | `-fsanitize=address` | 0 error |
| TSAN | `-fsanitize=thread` | 0 data race |
| 编译宏切换 | CopyGC / MS 分别编译 | 两种模式均通过 |
