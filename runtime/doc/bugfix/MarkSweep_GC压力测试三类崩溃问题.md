# Mark-Sweep GC 压力测试三类崩溃问题

## 1. 问题概述

60 秒压力测试（pinned 对象 + 大对象分配 + spawn 并发）下，Mark-Sweep GC 在运行约 2-3 秒后必然崩溃。共发现 **3 个独立 Bug**，均为时序敏感的竞态条件——此前被诊断日志代码（原子计数器 + LOG 输出）的"减速效应"掩盖。

| # | Bug | 症状 | 崩溃类型 | 严重度 |
|---|-----|------|----------|--------|
| 1 | **Sweep 不释放内存** | `ForceFinishSweep stats: released=0` → OOM | SIGABRT (OOM) | P0 |
| 2 | **MSRegionManager 回收失败** | `failed to reclaim units [1156+2, 1158)` | SIGABRT (fatal) | P0 |
| 3 | **Pinned/Mutex 对象状态损坏** | `Mutex is not locked by current thread` / SIGSEGV at pc=0x0 | SIGSEGV / Exception | P0 |

**根因**：三个 Bug 实为**同一根因**——`RegisterLargeObjectMS()` 中 `isMarked` 设置条件错误，IDLE 阶段分配的大对象 `isMarked=0`，被下一轮 GC 的 `ForceFinishSweep→SweepLargeRegions` 误杀，引发连锁故障。

## 2. 环境信息

| 项目 | 值 |
|------|-----|
| 平台 | macOS aarch64（Apple Silicon） |
| GC 模式 | Mark-Sweep（`--gc-mode marksweep`） |
| 测试用例 | `doc/test/gc_stress_pinned.cj`（60 秒压力测试） |
| 对比基准 | CopyGC 同测试 60 秒通过✅，RSS 均值 105MB |
| Runtime 源码 | `cangjie_runtime/runtime/` |
| SDK 路径 | `cangjie-pkg/self/dev260313/cangjie/` |
| 构建命令 | `python3 build.py build --gc-mode marksweep` |

## 3. 问题复现

```bash
# 编译 MarkSweep runtime（干净版本，无诊断代码）
cd cangjie_runtime/runtime
python3 build.py build --gc-mode marksweep

# 替换 SDK + 重签名
bash gc/run.sh ms-replace

# 编译运行压力测试
source <SDK_PATH>/envsetup.sh
cd doc/test && cjc gc_stress_pinned.cj -o main && ./main
# → 约 2-3 秒后崩溃（每次崩溃模式可能不同）
```

### CopyGC 对比（通过）
```bash
bash gc/run.sh replace   # CopyGC runtime
cd doc/test && cjc gc_stress_pinned.cj -o main && ./main
# → 60 秒完成，2624 rounds，RSS 均值 105MB ✅
```

## 4. Bug #1 — Sweep 完成但不释放任何 Region

### 4.1 症状

GC 执行 `ForceFinishSweep()` 时，遍历所有 unswept Region 并执行 `SweepRegion()`，但最终 `released=0`——没有任何 Region 被判定为"完全空闲"并归还给 `MSRegionManager`。堆空间持续增长，最终 OOM 崩溃。

在 `gc_benchmark.cj`（无诊断代码版本）中同样复现：
```
ForceFinishSweep stats: bg=0, lazy=0, force=12, released=0, largeFreed=0
[OOM] Out of memory
```

### 4.2 崩溃日志

```
2026-03-15 21:04:19.199010 E [MS-GC] ForceFinishSweep stats: bg=0, lazy=0, force=0, released=0, largeFreed=2949120
```

说明：
- `bg=0, lazy=0`：后台扫描和惰性扫描均未执行（正常——首个 GC 周期无前序 sweep）
- `force=0`：`ForceFinishSweep` 本身也未扫描任何 Region（所有 CAS 失败或 unswept 列表为空）
- `released=0`：没有 Region 被释放回自由池

### 4.3 代码位置

| 文件 | 行号 | 函数 |
|------|------|------|
| `CentralRegionPool.cpp` | 232 | `ForceFinishSweep()` — 遍历 unswept 列表 |
| `CentralRegionPool.cpp` | 260 | `CasSweepGen(sg-2, sg-1)` — CAS 竞争 |
| `CentralRegionPool.cpp` | 263 | `IsCompletelyFree()` 判定 → `ReleaseRegionInternal()` |
| `CentralRegionPool.cpp` | 326 | stats 日志输出 |
| `RegionSpace.cpp` | 61 | OOM 时调 `ForceFinishSweep()` |
| `RegionSpace.cpp` | 73 | 若仍不足则请求 Full GC |

### 4.4 根因分析

**确认根因**：`RegionSpace::RegisterLargeObjectMS()` 中，`isMarked` 只在 GC 活跃阶段设置：

```cpp
// 原代码（BUG）
GCPhase phase = Heap::GetHeap().GetCollector().GetGCPhase();
if (phase == GCPhase::GC_PHASE_MS_MARK_SETUP ||
    phase == GCPhase::GC_PHASE_MS_CONCURRENT_MARK ||
    phase == GCPhase::GC_PHASE_MS_MARK_TERMINATION ||
    phase == GCPhase::GC_PHASE_MS_SWEEP) {
    baseInfo->isMarked = 1;
}
```

当 GC 处于 **IDLE** 阶段时，大对象 `isMarked = 0`（未初始化默认值）。

**致命时序**：
1. IDLE 阶段：mutator 分配大对象 → `isMarked = 0`
2. GC 触发 → `MarkSetup()` → `ForceFinishSweep()` → `SweepLargeRegions()`
3. `SweepLargeRegions()` 检查 `isMarked == 0` → 判定为死对象 → 释放！
4. 但此时还没到 `ClearLargeMarks()`（清除标记位），也没到 mark 阶段（重新标记）
5. 活跃大对象的内存被释放回 MSRegionManager

**连锁效应**：
- 释放的 unit 范围与后续分配/释放重叠 → CartesianTree `MergeInsert` 失败（Bug #2）
- 大对象内存被复用为新 Region → 原大对象的内部状态（如 Mutex）被覆盖（Bug #3）
- OOM：释放实际上在使用的内存后堆地址空间出错（Bug #1）

### 4.5 修复方案

**已修复** — 无条件设置 `isMarked = 1`：

```cpp
// 修复后
baseInfo->isMarked = 1;
```

**原理**：
- 新分配的大对象必然是活的，无论当前 GC 阶段
- `ClearLargeMarks()` 会在每轮 GC 的 STW 阶段将所有大对象的 `isMarked` 重置为 0
- Mark 阶段重新标记存活对象 → `isMarked = 1`
- Sweep 阶段 `isMarked == 0` 的才是真正的死对象

**修改文件**：`src/Heap/Allocator/RegionSpace.cpp`（`RegisterLargeObjectMS` 函数）

## 5. Bug #2 — MSRegionManager CartesianTree 回收失败

### 5.1 症状

`MSRegionManager::ReclaimUnitsInternal()` 在将释放的 Region units 归还给 CartesianTree（dirty tree）时，`MergeInsert()` 返回 `false`，触发 `LOG(RTLOG_FATAL)` 导致 SIGABRT。

这意味着 CartesianTree 无法将被释放的 unit 范围合并插入——通常因为该范围与树中已有节点重叠（double-free）或状态不一致。

### 5.2 崩溃日志

```
2026-03-15 21:04:19.200998 F [MS-GC] MSRegionManager: failed to reclaim units [1156+2, 1158)
37150648 E CJNatvie Handle signal: 6.
37150648 E Thread "gc-main-thread" catched unhandled SIGABRT (Abort trap: 6) from native frame.
signal pc: 0x19e852388
  #0  0x19e852388 in __pthread_kill from /usr/lib/system/libsystem_kernel.dylib
```

时序：发生在 `ForceFinishSweep` 之后的 `PrepareForSweep` 阶段，GC 主线程尝试释放完全空闲的 Region。

### 5.3 代码位置

| 文件 | 行号 | 函数 |
|------|------|------|
| `MSRegionManager.cpp` | 228 | `ReclaimUnitsInternal()` — 归还 units 到 dirty tree |
| `MSRegionManager.cpp` | 233 | `MergeInsert()` 失败 → `LOG(RTLOG_FATAL)` |
| `CentralRegionPool.cpp` | 226 | `ReleaseRegionInternal()` — 调用 `ReclaimRegion()` |
| `CentralRegionPool.cpp` | 263 | `ForceFinishSweep` 中 `IsCompletelyFree()` → `ReleaseRegionInternal()` |

### 5.4 根因分析

**初步判断**：Region 被 double-free——同一个 unit 范围被释放了两次。

可能路径：
1. **ForceFinishSweep 与惰性 sweep 竞态**：`ForceFinishSweep` 和分配路径的 `GetRegion()` 都可能 sweep Region 并释放完全空闲的 Region。虽然有 CAS 保护，但如果 `ReleaseRegionInternal` 被调用两次（CAS 成功但状态清理不完整），则 `ReclaimUnitsInternal` 收到重复的 unit 范围。
2. **TakeUnitsLowestAddr 修改的副作用**：之前将 4 处 `TakeUnits()` 改为 `TakeUnitsLowestAddr()`，低地址优先分配改变了 tree 的遍历模式，可能暴露了 `MergeInsert` 的边界条件。
3. **Region metadata 未正确重置**：`ReleaseRegionInternal` 释放 Region 后，如果 `MSRegionInfo` 未标记为 `MS_REGION_FREE`，后续代码可能再次尝试释放。

**需验证**：在 `ReclaimUnitsInternal` 中打印传入的 idx/num，同时在 `ReleaseRegionInternal` 入口打印 Region 地址，确认是否存在 double-free。

### 5.5 修复方案

**待定** —— 需与 Bug #1 一起调查：
1. 在 `ReleaseRegionInternal` 入口添加防重入检查（检查 regionType 是否已为 FREE）
2. 检查 `MergeInsert` 的边界条件——特别是相邻 unit 的合并逻辑
3. 回顾 `TakeUnitsLowestAddr` 对 CartesianTree 状态的影响

## 6. Bug #3 — Pinned/Mutex 对象被 Sweep 损坏

### 6.1 症状

在压力测试中使用 `Mutex` 保护 pinned 对象的操作时，两种崩溃模式：

**模式 A — Mutex 状态异常**：
```
IllegalSynchronizationStateException: Mutex is not locked by current thread
  → at PinnedHolder::doWork()  (gc_stress_pinned.cj)
```
Mutex 内部状态被 GC sweep 损坏，`unlock()` 时发现当前线程不持有锁。

**模式 B — SIGSEGV at pc=0x0**：
使用 `spawn` 在并发标记期间创建新协程时，引用写入触发空指针解引用：
```
SIGSEGV at pc=0x0
Thread "schd-worker" catched unhandled SIGSEGV from managed frame
```

### 6.2 崩溃日志

模式 A（Mutex 异常）来自手动运行时观察到的异常信息（非 stderr 日志）。

模式 B（SIGSEGV）来自压力测试的第二次运行：
```
37119xxx E Thread "schd-worker" catched unhandled SIGSEGV (Segmentation fault: 11) from managed frame.
signal pc: 0x0
```
崩溃发生在运行约 2.8 秒时。

### 6.3 代码位置

| 文件 | 函数 | 说明 |
|------|------|------|
| `MSRegionInfo.h` | `SweepRegion()` | 扫描 Region 中的 slot，释放未标记对象 |
| `CentralRegionPool.cpp` | `ForceFinishSweep()` | 强制扫描所有 unswept Regions |
| `MSWriteBarrier.cpp` | `MarkAndEnqueue()` | SATB 写屏障——并发标记期间记录旧值 |
| `MarkSweepCollector.cpp` | `ConcurrentMark()` | 并发标记阶段 |

### 6.4 根因分析

**初步判断**：并发标记 + sweep 对 pinned 对象的处理存在问题。

1. **Pinned 对象的 Mutex 内部字段**：Cangjie `Mutex` 内部可能包含堆引用（如 owner thread 引用）。若 sweep 错误地将 Mutex 内部的活跃引用视为死对象并释放其所在 slot，则 Mutex 状态损坏。
2. **Spawn 创建的协程引用未被 SATB 屏障捕获**：`spawn` 在 managed 栈上写入新协程/闭包引用，若写屏障未触发（GC phase 数值导致屏障跳过），则新引用对 GC 不可见，被 sweep 释放后形成悬挂指针 → SIGSEGV。
3. **与 Bug #1 的关联**：如果 sweep 逻辑本身有误（bitmap 对比错误），不仅无法释放空闲 Region，还可能错误释放活跃对象的 slot。

**需验证**：
- 检查 `SweepRegion()` 是否正确跳过 pinned Region
- 确认 Mutex 对象所在 Region 的 mark bitmap 状态

### 6.5 修复方案

**待定** —— 与 Bug #1 高度关联，修复 sweep 逻辑后可能同时解决：
1. 确认 pinned Region（`regionType == MS_REGION_PINNED`）是否参与 sweep
2. 若 Mutex 使用内部堆引用，确保写屏障涵盖这些引用的更新
3. 检查 `spawn` 路径上的 SATB 屏障触发条件

## 7. 诊断日志掩盖效应分析

之前 `MSWriteBarrier.cpp` 和 `MarkSweepCollector.cpp` 中包含大量诊断代码：

```cpp
// MSWriteBarrier.cpp 中的原子计数器（已删除）
static std::atomic<uint64_t> wbCallCount{0};
static std::atomic<uint64_t> wbNullOrNonHeap{0};
static std::atomic<uint64_t> wbPhaseSkip{0};
static std::atomic<uint64_t> wbAlreadyMarked{0};
static std::atomic<uint64_t> wbEnqueued{0};
// 每次写屏障调用中 fetch_add
```

这些原子操作在每次写屏障调用时执行 `fetch_add`，形成内存屏障，降低了 mutator 线程的执行速度约 10-20%。同时 `MarkSweepCollector.cpp` 中的 pre-mark bitmap 遍历和 post-mark root dump 各消耗数百微秒，延长了 STW 时间。

**掩盖机制**：
1. mutator 减速 → GC 有更多时间完成标记 → 并发标记期间未标记对象更少
2. STW 延长 → mutator 无法修改引用 → SATB 屏障遗漏的窗口缩小
3. 内存屏障效应 → 处理器缓存一致性更强 → 竞态窗口收窄

**验证**：将 `wbCallCount.fetch_add(1)` 单独加回后，崩溃再次被掩盖（gc_benchmark 5/5 PASS）。移除后立即崩溃。

## 8. 测试验证计划

修复全部 3 个 Bug 后，需通过以下测试：

1. **gc_benchmark.cj**：`gc_hello.cj` 冒烟 + 5 项功能测试全部 PASS
2. **gc_stress_pinned.cj**：60 秒压力测试完整通过（含 Mutex + spawn）
3. **CopyGC 回归**：切回 CopyGC 模式确认无回归
4. **重复执行**：MS 模式下 gc_benchmark 连续运行 5 次均 PASS
5. **内存稳定性**：60 秒压力测试 RSS 无持续增长（目标 < 200MB 均值）

```bash
# 验证命令
bash gc/run.sh ms-all          # 基础冒烟
cd doc/test && cjc gc_stress_pinned.cj -o main && ./main  # 压力测试
bash gc/run.sh restore && bash gc/run.sh test  # CopyGC 回归
```

## 9. 附录

### 9.1 压力测试对比数据

| 指标 | CopyGC (60s) | MarkSweep |
|------|-------------|-----------|
| 状态 | PASS ✅ | CRASH ❌ (~3s) |
| RSS 均值 | 105.0 MB | 202.3 MB (崩溃前) |
| RSS 峰值 | 126.2 MB | 332.7 MB |
| CPU 均值 | 83.6% | 63.2% |
| 完成轮次 | 2,624 | ~73 |
| 分配总量 | 4,163 MB | ~115 MB |

### 9.2 相关文件

- 压力测试代码: `doc/test/gc_stress_pinned.cj`
- 监控脚本: `doc/test/monitor_stress.py`
- 可视化脚本: `doc/test/plot_stress.py`
- CopyGC 数据: `doc/test/copygc_stress.csv`
- MS 数据: `doc/test/ms_stress.csv`
- MS 崩溃日志: `doc/test/ms_stress_stderr.txt`
- 对比图表: `doc/test/stress_comparison.png`
