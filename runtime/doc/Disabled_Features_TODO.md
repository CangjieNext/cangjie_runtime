# LOS 实现 — 当前禁用功能清单

> 创建日期: 2026-02-21
> 状态: 所有功能禁用后, release 模式 60/60 测试通过 (64MB 默认 6:3:1 配比)

---

## 背景

WCollector 是并发垃圾收集器 — `ScopedStopTheWorld` 在 `CopyCollector::RunGarbageCollection()` 中被注释掉 (src/Heap/Collector/CopyCollector.cpp:43)。这意味着 mutator 线程在整个 GC 周期中持续运行。以下功能因与并发 mutator 存在竞态而被禁用。

---

## 禁用功能列表

### 1. CompactLOSIfNeeded (LOS 滑动压缩)

| 项目 | 详情 |
|------|------|
| **文件** | `src/Heap/WCollector/WCollector.cpp` ~L418 |
| **禁用方式** | 函数入口处 `return;` |
| **根因** | `CompactLOS()` 通过 `memmove_s` 滑动移动存活的大对象，重建 region 列表。在 `PostTrace()` 中执行，此时 mutator 并发读写 LOS 对象 → SIGSEGV |
| **影响** | LOS 空间会随时间碎片化，无法通过压缩回收碎片空间。退化为仅依赖 `losFreeMgr` 的 dirty/release 树复用已回收 region |
| **恢复方案** | 用 `ScopedStopTheWorld` 包裹 `CompactLOS()` 的对象移动 + `UpdateMigratedReferences()` 引用更新部分 |
| **优先级** | 高 — 长时间运行的程序 LOS 碎片化会导致分配失败 |

### 2. MigrateLargeObjectsToLOS (MAIN → LOS 大对象迁移)

| 项目 | 详情 |
|------|------|
| **文件** | `src/Heap/WCollector/WCollector.cpp` ~L459 |
| **禁用方式** | 函数入口处 `return;` |
| **根因** | `MigrateLargeObjectsFromMain()` 将大对象从 MAIN 复制到 LOS，然后 `UpdateMigratedReferences()` 扫描整个堆更新引用。没有 STW，mutator 并发读取旧引用 → SIGSEGV |
| **影响** | 大对象始终留在 MAIN 空间，占用 MAIN 的 region，无法享受 LOS 独立管理的优势。MAIN 空间压力增大 |
| **恢复方案** | 用 `ScopedStopTheWorld` 包裹 `MigrateLargeObjectsFromMain()` + `UpdateMigratedReferences()` |
| **优先级** | 高 — 与 CompactLOS 共享 `migrationMap` 和 `UpdateMigratedReferences()`，建议一起恢复 |

> 2026-02-22 新策略记录（非 STW，Post-GC 并发迁移）: `doc/LOS_PostGC_Concurrent_Migration_Strategy_2026-02-22.md`

### 3. ShrinkInactiveZone (MAIN 空闲区缩减)

| 项目 | 详情 |
|------|------|
| **文件** | `src/Heap/WCollector/WCollector.cpp` ~L603 |
| **禁用方式** | 调用被注释: `// reinterpret_cast<RegionSpace&>(theAllocator).GetRegionManager().ShrinkInactiveZone();` |
| **根因** | `ShrinkInactiveZone` 执行 `ClearTrees` + 重新扫描，与 mutator 的 `TakeRegion` → `ReclaimRegion`（`InitFreeUnits` 然后 `AddGarbageUnits`）产生竞态：mutator 标记 region 为 free 后尚未调用 `AddGarbageUnits`，重新扫描也将其添加 → CartesianTree 重复插入 |
| **影响** | 无法缩小 MAIN 的 inactive zone (高地址空闲区域)。不影响正确性，但在堆使用量下降后无法归还空间 |
| **恢复方案** | 用 `ScopedStopTheWorld` 包裹 `ShrinkInactiveZone()` 调用 |
| **优先级** | 中 — 不影响功能正确性，仅影响内存归还效率 |

### 4. lowAddressFirst = false (低地址优先分配策略)

| 项目 | 详情 |
|------|------|
| **文件** | `src/Heap/Allocator/RegionManager.cpp` ~L462-463 |
| **禁用方式** | `freeRegionManager.SetLowAddressFirst(false);`，标记 `[BISECT]` |
| **根因** | 低地址优先分配与 `ShrinkInactiveZone` 配合使用 — 通过将分配集中在低地址，使高地址的 inactive zone 可以安全缩小。当 `ShrinkInactiveZone` 被禁用时，`lowAddressFirst` 失去作用且 `FindLowestSuitablePtr` 路径可能引入额外开销 |
| **影响** | 分配不再优先使用低地址 region，region 分散在整个 MAIN 空间。不影响正确性 |
| **恢复方案** | 恢复 `ShrinkInactiveZone` 后，将 `SetLowAddressFirst(true)` 一同恢复 |
| **优先级** | 低 — 依赖 #3 的恢复，独立恢复无意义 |

---

## 已修复并启用的功能

以下功能之前被禁用，现已通过修复竞态问题重新启用：

### ✅ pendingReserveReturns (RESERVE 借用区归还)

| 项目 | 详情 |
|------|------|
| **文件** | `src/Heap/Allocator/RegionManager.h` (~L753), `RegionManager.cpp` (ReclaimRegion, ReleaseRegion, ReturnBorrowedToReserve) |
| **修复方式** | 添加 `std::mutex pendingReserveReturnsMutex`，在 `ReclaimRegion`、`ReleaseRegion`、`ReturnBorrowedToReserve` 中使用 `std::lock_guard` 保护 `pendingReserveReturns` vector |
| **原竞态** | mutator（通过 ReclaimRegion）、GC 线程、FP 线程并发对 `std::vector` 执行 `push_back` |

### ✅ FP-GC Dekker 竞态 (FinalizerProcessor 与 GC 竞态)

| 项目 | 详情 |
|------|------|
| **文件** | `src/Heap/Collector/CopyCollector.cpp`, `src/Heap/FinalizerProcessor.h/cpp` |
| **修复方式** | 添加 `std::mutex gcReclaimMutex`，GC 期间 `SuppressGarbageReclaim` / `UnsuppressGarbageReclaim`，FP 的 `ReclaimHeapGarbage` 获取锁 |

---

## 建议恢复顺序

1. **CompactLOSIfNeeded + MigrateLargeObjectsToLOS** — 这两个共享 `migrationMap` 和 `UpdateMigratedReferences()`，建议一起恢复。核心方案：在 WCollector.cpp 中用 `ScopedStopTheWorld` 包裹相关调用。需验证 STW 对延迟的影响。
2. **ShrinkInactiveZone** — 独立恢复，同样用 `ScopedStopTheWorld` 包裹。
3. **lowAddressFirst = true** — 在 #2 恢复后简单设回 `true` 即可。

---

## 关键技术约束

- `ScopedStopTheWorld` 已存在于代码中（`CopyCollector.cpp:43` 被注释），说明框架支持 STW
- STW 会暂停所有 mutator 线程，对延迟敏感的应用影响较大
- 可考虑的替代方案：
  - **增量压缩**: 每次 GC 只移动少量对象，分摊 STW 时间
  - **读屏障 (read barrier)**: Brooks pointer 或 forwarding pointer 方案，避免 STW
  - **写时复制 (copy-on-write)**: 利用 mprotect 在移动期间捕获 mutator 访问
- 当前建议先用 STW 方案验证正确性，后续根据性能数据决定是否优化
