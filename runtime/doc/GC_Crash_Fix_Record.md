# GC 崩溃问题分析与修复记录

日期: 2026-02-21

## 问题现象

在 64MB 堆压力测试（40 轮 × 4×256KB 分配 + 树结构）下，LOS 修改后的 runtime 出现多种崩溃：

- `CartesianTree.cpp:57: merge insertion failed` — CartesianTree 中出现重叠索引范围的双重插入
- `RegionManager.cpp:237: Delete Region next is not null` — 链表损坏
- `IsFromRegion()` 断言失败 — region 类型不匹配

所有崩溃均在 `gc-helper` 线程（即 FinalizerProcessor 线程）上发生，发生在第 0 轮（第一次 GC 周期）。

---

## 问题一：FinalizerProcessor 与 GC 主线程竞态

### 分析

FinalizerProcessor（FP）线程异步执行 `ReclaimGarbageRegions`，与 GC 主线程的 PostTrace 阶段操作（`CollectLargeGarbage`、`MigrateLargeObjectsToLOS` 等）存在竞态。两者并发修改 `garbageRegionList` 和 region 元数据。

原始实现使用 `gcActive`/`reclaimInProgress` 两个 `atomic<bool>` 的 Dekker 风格双检查模式进行互斥。

**根因**：Dekker 协议要求 `seq_cst` 内存序才能保证正确性。在 ARM（Apple Silicon）上，两个线程执行"store X, load Y"时使用 `acquire/release` 语义，可能都看到对方的旧值，导致互斥失败。

### 修复

用 `std::mutex gcReclaimMutex` 替代 Dekker 原子标志：

| 文件 | 修改 |
|------|------|
| `FinalizerProcessor.h` | 添加 `std::mutex gcReclaimMutex`；新增 `SuppressGarbageReclaim()`（lock）和 `UnsuppressGarbageReclaim()`（unlock）|
| `FinalizerProcessor.cpp` | `ReclaimHeapGarbage()` 开头加 `std::lock_guard<std::mutex> lock(gcReclaimMutex)` |
| `CopyCollector.cpp` | GC 周期开始时调用 `SuppressGarbageReclaim()`，结束时调用 `UnsuppressGarbageReclaim()` |

### 涉及文件

- `src/Heap/Collector/FinalizerProcessor.h`
- `src/Heap/Collector/FinalizerProcessor.cpp`
- `src/Heap/Collector/CopyCollector.cpp`

---

## 问题二：ShrinkInactiveZone 与 Mutator 并发竞态导致 CartesianTree 双重插入

### 分析

修复问题一后仍然 4/5 崩溃，全部为 `CartesianTree.cpp:57: merge insertion failed`。

通过二分法定位：

| 配置 | 结果 |
|------|------|
| lowAddrFirst=OFF, ShrinkInactive=OFF, ReturnBorrowed=OFF | 5/5 通过 |
| lowAddrFirst=ON, ShrinkInactive=OFF, ReturnBorrowed=OFF | 5/5 通过 |
| **lowAddrFirst=ON, ShrinkInactive=ON, ReturnBorrowed=OFF** | **4/5 崩溃** |
| 原始 SDK runtime（无 LOS 修改）| 5/5 通过 |

**根因**：`ShrinkInactiveZone()` 原本被设计为在 STW（Stop-The-World）期间执行，但实际上 **WCollector 的 GC 没有 STW**。

在 `CopyCollector::RunGarbageCollection()` 中，`ScopedStopTheWorld stw;` 是**被注释掉的**（第 43 行）。整个 GC 周期是**并发**的——`TransitionToGCPhase()` 只切换 mutator 的 barrier 类型，**不暂停 mutator**。

`ShrinkInactiveZone` 执行 `ClearTrees()` + 重新扫描添加 free region 的过程中，mutator 的 `TakeRegion → ReclaimRegion → AddGarbageUnits` 路径并发执行：

```
GC 线程                          Mutator 线程
─────────                        ─────────
ClearTrees()                     
                                 TakeRegion → 取到 garbage region R
                                 ReclaimRegion(R):
                                   InitFreeUnits(R)  ← R 变 free
扫描发现 R, IsFreeRegion=true   
AddGarbageUnits(R.idx, R.num)    
                                   AddGarbageUnits(R.idx, R.num)  ← 重复插入！
                                   → CartesianTree CRASH
```

### 修复

在 `WCollector::DoGarbageCollection()` 中**禁用** `ShrinkInactiveZone()` 调用，并添加注释说明原因。

如果后续需要此优化，应该用 `ScopedStopTheWorld` 包裹整个 `ShrinkInactiveZone` 执行过程，确保 mutator 暂停后再操作 `freeRegionManager` 的树结构。

### 涉及文件

- `src/Heap/WCollector/WCollector.cpp` — 注释掉 `ShrinkInactiveZone()` 调用
- `src/Heap/Allocator/RegionManager.cpp` — `ShrinkInactiveZone()` 实现保留，但当前不被调用

---

## 回退的调试代码

| 文件 | 内容 | 原因 |
|------|------|------|
| `RegionInfo.h` | `PrepareForwardableRegion` 中 FATAL 日志 | 调试用，原 `CHECK(IsFromRegion())` 已足够 |
| `FinalizerProcessor.cpp` | `ReclaimHeapGarbage` 中两行 VLOG | 调试日志 |
| `WCollector.cpp` | `DumpMemoryMap()` 调用 | 每次 GC 结束都调用太重 |

---

## 验证结果

修复后使用 debug 版本测试 15/15 全部通过：

```
cjHeapSize=64mb ./main_crash_repro   → 15/15 PASS (0 crash)
```

对比原始 SDK runtime 也是 5/5 通过，确认修复没有引入新问题。

---

## 关键经验

1. **ARM 内存模型**：Dekker 互斥协议在 ARM 上需要 `seq_cst` 而非 `acquire/release`。更安全的做法是直接使用 `std::mutex`。
2. **并发 GC ≠ STW GC**：WCollector 虽然有 `TransitionToGCPhase` 但没有真正的 STW。任何操作 freeRegionManager 树结构（ClearTrees/AddGarbageUnits）的代码必须考虑 mutator 并发。
3. **二分法定位**：通过逐个开关功能快速定位 `ShrinkInactiveZone` 是唯一有问题的功能点。
