# M5 实现记录：MAIN ↔ RESERVE ↔ LOS 动态调度

**实现日期**：2026年2月19日

---

## 1. 实现目标

M5 阶段目标：建立 MAIN、RESERVE、LOS 三段空间之间的动态调度机制，使 GC 后能自动归还借用的 RESERVE 空间。

### 实际实现状态

| 编号 | 子目标 | 状态 |
|------|--------|------|
| #1 | RESERVE 借用空间归还（通过 bulk watermark reclaim） | ✅ 已实现（集成在 M4a/M4b 中） |
| #2 | ShrinkInactiveZone Phase B → ReclaimMainBorrowed | ✅ 已实现（M4b） |
| #3 | CompactLOS Phase 7 → ReclaimLOSBorrowed | ✅ 已实现（M4a） |
| #4 | NotifyGCCycle / GC 周期计数 | ✅ 已实现 |
| #5 | 冷却常量定义（BORROW_COOLDOWN_CYCLES） | ✅ 已定义（标记 "reserved for future use"） |
| #6 | 迟滞阈值（Hysteresis Thresholds） | ❌ 未实现 |
| #7 | 冷却检测 API（IsMainBorrowInCooldown 等） | ❌ 未实现 |
| #8 | pendingReserveReturns 暂存机制 | ❌ 未实现 |
| #9 | 逐 region 归还（ReturnFromMain/ReturnFromLOS） | ❌ 未实现 |
| #10 | Hole Bitmap 灵活归还 | ❌ 未实现 |

---

## 2. 已实现：Bulk Watermark Reclaim

### 2.1 实现方式

RESERVE 归还未使用独立的 M5 流程，而是集成在 M4a 和 M4b 的现有机制中：

**MAIN → RESERVE 归还**（ShrinkInactiveZone Phase B）：
- M4b 的 `ShrinkInactiveZone()` 在扫描 MAIN 活跃区后，Phase B 继续扫描 `[mainEnd, mainWm)` 的 RESERVE-borrowed 区域
- 找到最高活跃 borrowed region 后，整体推回 mainWatermark
- 调用 `ReserveManager::ReclaimMainBorrowed(newWatermark)` 一次性归还

**LOS → RESERVE 归还**（CompactLOS Phase 7）：
- M4a 的 `CompactLOS()` Phase 7 在整理完成后，将 losWatermark 推回到最低活跃 LOS region
- 调用 `ReserveManager::ReclaimLOSBorrowed(newWatermark)` 一次性归还

### 2.2 ReserveManager 已实现 API

```cpp
// Bulk reclaim MAIN-borrowed RESERVE space
// Directly pushes mainWatermark to newWatermark
size_t ReclaimMainBorrowed(uintptr_t newWatermark);

// Bulk reclaim LOS-borrowed RESERVE space
// Directly pushes losWatermark to newWatermark
size_t ReclaimLOSBorrowed(uintptr_t newWatermark);

// GC cycle counter
void NotifyGCCycle() { gcCycleCount++; }

// Statistics
size_t GetMainReturnBytes() const { return mainReturnBytes; }
size_t GetLOSReturnBytes() const { return losReturnBytes; }
```

### 2.3 已定义常量

| 常量 | 值 | 说明 |
|------|----|------|
| `BORROW_COOLDOWN_CYCLES` | 2 | 预留，标注 "reserved for future use" |
| `RETURN_COOLDOWN_CYCLES` | 2 | 预留，标注 "reserved for future use" |

---

## 3. GC 中的调用时序

```
DoGarbageCollection()
  ├─ ... Trace + Mark ...
  ├─ ForwardFromSpace()
  ├─ CompactLOSIfNeeded()          // M4a: CompactLOS Phase 7 → ReclaimLOSBorrowed
  ├─ ShrinkInactiveZone()          // M4b: Phase B → ReclaimMainBorrowed
  ├─ CleanupMigrationCandidates()
  ├─ FlipTagID()
  ├─ CollectSmallSpace()
  └─ NotifyGCCycle()               // M5: GC 周期计数 +1
```

防振荡由 M4b 的碎片率门控（>30%）间接实现，不依赖显式冷却机制。

---

## 4. 统计与日志

### 4.1 DumpRegionStats 输出

每次 GC 结束后，输出 RESERVE / LOS / MAIN 统计快照：

```
RESERVE: capacity X B, available X B, mainBorrowed X B, losBorrowed X B,
         mainReturned X B, losReturned X B
LOS: capacity X B, used X B, free X B, alloc X, fail X, fallback X, reserveBorrow X
MAIN: [0x..., 0x...) end 0x..., inactiveZone 0x..., reserveBorrow X
```

---

## 5. 未实现设计（保留参考）

以下为原始 M5 设计方案，**代码中未实现**，保留供后续参考。

### 5.1 pendingReserveReturns 暂存机制（未实现）

设计：在 `ReclaimRegion` / `ReleaseRegion` 阶段拦截 RESERVE-borrowed region 到独立队列，GC 后由 `ReturnBorrowedToReserve()` 集中处理归还。

实际：不存在 `pendingReserveReturns` 队列、`ReturnBorrowedToReserve()` 方法、`IsInReserveTerritory()` 检查。归还通过 bulk watermark reclaim 实现。

### 5.2 逐 region 归还（未实现）

设计：`ReturnFromMain(addr, size)` / `ReturnFromLOS(addr, size)` 逐个归还 RESERVE-borrowed region，仅水位边缘有效。

实际：不存在此 API。使用 `ReclaimMainBorrowed` / `ReclaimLOSBorrowed` 直接推移 watermark。

### 5.3 冷却检测 API（未实现）

设计：`IsMainBorrowInCooldown()` / `IsMainReturnInCooldown()` 等基于 GC 周期计数的冷却检查。

实际：仅定义了 `BORROW_COOLDOWN_CYCLES` / `RETURN_COOLDOWN_CYCLES` 常量和 `gcCycleCount` 计数器，无检查 API。

### 5.4 迟滞阈值（未实现）

设计：`RETURN_USAGE_THRESHOLD = 0.6`、`BORROW_USAGE_THRESHOLD = 0.8`、`LOS_RESERVE_RETURN_THRESHOLD = 0.4` 基于使用率的归还/借入门控。

实际：不存在这些常量。

### 5.5 Hole Bitmap（未实现）

设计：bit-per-unit bitmap 实现非 LIFO 灵活归还，支持任意位置归还 + watermark 自动合并。

实际：`Initialize()` 注释中提及 bitmap，但无 `returnBitmap`、`SetBitmapRange`、`CoalesceMainWatermark` 等实现。

---

## 6. 编译验证

```
Build and preinstall completed successfully.
```
