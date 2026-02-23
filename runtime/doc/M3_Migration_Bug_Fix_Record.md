# M3 迁移数据损坏 Bug 排查与修复记录

**日期**：2026年2月22日

---

## 1. 问题概述

`BuildLargeObjectMigrationRoutes` 开启后（`cjEnableLOSMigration=1`），测试程序 `gc_los_migrate_verify.cj` 中所有参与迁移的大对象出现数据损坏（checksum 校验失败），而关闭迁移功能时全部通过。

**复现环境**：macOS aarch64, Release 构建, `cjHeapSize=64mb`

**症状**：
```
[Phase 5] Triggering migration GC rounds...
  GC round 0: FAILED - 21 corrupted!
    corrupted object id=40
    corrupted object id=1000 ~ 1019
```

---

## 2. 排查过程

### 2.1 第一轮分析：引用未标记（已在本次修复前解决）

**原始 Bug**：`BuildLargeObjectMigrationRoutes` 在 PostTrace 阶段才选择迁移候选并设置 `FROM_REGION` 类型，此时 Enum/Trace 阶段已经结束，导致：

1. `GetAndTryTagRefField()` → `IsFromObject(target)` → `IsFromRegion()` — 只在 Enum/Trace 期间检查
2. 候选 region 在 Enum/Trace 期间仍为 `LARGE_REGION` 类型 → `IsFromRegion()` 返回 `false`
3. 引用未被 tagged → `ForwardBarrier::ReadReference` 快速路径直接返回旧指针
4. MAIN 旧 region 被 `CollectRegion` 回收后，旧地址变成垃圾数据

**修复**：两阶段迁移设计（详见 2.3 节最终方案）。

### 2.2 第二轮分析：修复后仍然崩溃

两阶段修复后重新编译测试，迁移启用时依然出现完全相同的 21 个对象损坏。通过二分诊断法逐步缩小问题范围：

#### 诊断实验 1：全部标记为 UNMOVABLE（不回收、不迁移）

将 `BuildLargeObjectMigrationRoutes` 中的循环体替换为：

```cpp
for (RegionInfo* region : candidates) {
    region->SetRegionType(RegionInfo::RegionType::UNMOVABLE_FROM_REGION);
    losFullCount++;
}
```

**结果：ALL TESTS PASSED** ✓

结论：FROM_REGION → tag → UNMOVABLE_FROM_REGION → untag 路径正确。问题在死对象回收或实际迁移路径。

#### 诊断实验 2：恢复死对象回收 + UNMOVABLE（不实际迁移）

```cpp
for (RegionInfo* region : candidates) {
    if (region->GetLiveByteCount() == 0) {
        migrationList.DeleteRegion(region);
        rm.CollectRegion(region);
        deadCount++;
        continue;
    }
    region->SetRegionType(RegionInfo::RegionType::UNMOVABLE_FROM_REGION);
    losFullCount++;
}
```

**结果：FAILED — 21 corrupted!** ✗

结论：`GetLiveByteCount() == 0` 判断有误，活对象被当成死对象回收。

### 2.3 根因定位：`isMarked` 标志未重置

大对象 region 的标记机制与小对象不同：

| 对象类型 | 标记机制 | 存活判断 |
|----------|----------|----------|
| 小对象 | `markBitmap`（需 `liveInfo` 指针） | 逐对象 bitmap 检查 |
| 大对象 | `metadata.isMarked` 标志（每 region 一个 flag） | `isMarked == 1` |

`MarkObject` 对大对象的关键代码（`RegionInfo.h`）：

```cpp
bool MarkObject(const BaseObject* obj) {
    if (IsLargeRegion()) {           // 检查 unitRole，不检查 regionType
        if (metadata.isMarked != 1) {
            SetMarkedRegionFlag(1);  // 首次标记
            return false;            // → 触发 AddLiveByteCount(objectSize)
        }
        return true;                 // 已标记 → 跳过 AddLiveByteCount
    }
    // 小对象路径...
}
```

**问题链**：

```
上一轮 GC trace → 对象被标记 → isMarked = 1
上一轮 GC CollectLargeGarbage → 遍历 oldLargeRegionList → ResetMarkBit()
                                                            ↑ 对象仍在此 list 中，isMarked → 0 ✓

本轮 GC:
  AssembleMigrationCandidates() →
    从 oldLargeRegionList 移除 → 加入 migrationCandidateList
    ClearLiveInfo() → liveByteCount = 0
    【缺少 ResetMarkBit()】 → isMarked 仍为 1 !!

  Trace 阶段 → 对象可达 → MarkObject(obj)
    → isMarked 已经是 1 → return true（"已标记"）
    → 不调用 AddLiveByteCount
    → liveByteCount 保持 0 !!

  BuildLargeObjectMigrationRoutes →
    region->GetLiveByteCount() == 0 → 误判为死对象 → CollectRegion() !!
    → 活对象的 region 被回收 → 数据被覆盖 → 损坏 !!
```

**关键点**：`ClearLiveInfo()` 只清除 `liveInfo` 指针和 `liveByteCount`，**不清除 `isMarked`**。而 `ResetMarkBit()` 由 `CollectLargeGarbage` 在遍历 `oldLargeRegionList` 时调用。候选被移出 `oldLargeRegionList` 后，`ResetMarkBit` 不再覆盖到它们 → `isMarked` 残留为 1 → trace 误判为"已标记" → liveByteCount 永远为 0。

---

## 3. 修复方案

在 `AssembleMigrationCandidates()` 中，紧跟 `ClearLiveInfo()` 之后添加 `ResetMarkBit()` 调用：

### 修改文件：`src/Heap/Allocator/RegionManager.cpp`

```diff
         oldLargeRegionList.DeleteRegion(region);
         migrationCandidateList.PrependRegion(region, RegionInfo::RegionType::FROM_REGION);
         region->ClearLiveInfo();
+        region->ResetMarkBit();  // M3-fix: reset isMarked so trace re-marks and counts live bytes

         region = next;
```

**修复效果**：`ResetMarkBit()` → `SetMarkedRegionFlag(0)` → 下一轮 trace 的 `MarkObject` 看到 `isMarked == 0` → 正确设置标志并调用 `AddLiveByteCount` → liveByteCount 反映真实存活状态 → 不再误判为死对象。

---

## 4. 修改文件清单（两阶段迁移 + isMarked 修复）

| 文件 | 修改说明 |
|------|----------|
| `src/Heap/Allocator/RegionManager.h` | 新增 `migrationCandidateList` 字段、构造函数初始化、`AssembleMigrationCandidates()` / `CleanupMigrationCandidates()` / `GetMigrationCandidateList()` 声明 |
| `src/Heap/Allocator/RegionManager.cpp` | 实现 `AssembleMigrationCandidates()`（含 `ResetMarkBit` 修复）和 `CleanupMigrationCandidates()` |
| `src/Heap/Allocator/RegionSpace.h` | `AssembleGarbageCandidates()` 最前面调用 `AssembleMigrationCandidates()` |
| `src/Heap/WCollector/WCollector.cpp` | 重写 `BuildLargeObjectMigrationRoutes()` 基于 `migrationCandidateList`；`DoGarbageCollection()` 末尾添加 `CleanupMigrationCandidates()` 调用 |

---

## 5. 两阶段迁移设计

### 5.1 阶段一：Trace 前选候选（`AssembleMigrationCandidates`）

在 `AssembleGarbageCandidates()` 最前面执行（先于小对象和 LOS 的 garbage 装配）：

1. 检查环境变量 `cjEnableLOSMigration=1`
2. 遍历 `oldLargeRegionList`，过滤不可迁移的 region（有 rawPointer、在 RESERVE 区域）
3. 将候选移入 `migrationCandidateList`，类型设为 `FROM_REGION`
4. 调用 `ClearLiveInfo()` + **`ResetMarkBit()`** — 确保 trace 重新标记
5. 候选的 `IsFromRegion() == true` → Enum/Trace 阶段引用会被 tagged

### 5.2 阶段二：Trace 后路由（`BuildLargeObjectMigrationRoutes`）

在 PostTrace 中执行（trace 完成后已知存活情况）：

对 `migrationCandidateList` 中每个候选：
- **liveByteCount == 0**（死对象）→ 直接 `CollectRegion` 回收
- **活对象 + LOS 可分配** → 分配 LOS 空间，`PrepareForwardableLargeRegion` 设置 ghost route，移入 `fromRegionList`
- **活对象 + LOS 满** → 设置 `UNMOVABLE_FROM_REGION`，留在 `migrationCandidateList`

### 5.3 GC 结束后清理（`CleanupMigrationCandidates`）

在 `CollectSmallSpace()` 之后，将未迁移的候选（`UNMOVABLE_FROM_REGION`）merge 回 `oldLargeRegionList`（类型恢复为 `LARGE_REGION`）。

### 5.4 ForwardBarrier 兼容性

| 引用目标状态 | `IsFromRegion()` | `IsUnmovableFromRegion()` | ForwardBarrier 行为 |
|-------------|-------------------|---------------------------|---------------------|
| 已迁移到 LOS | true | false | → `TryForwardRefField` → 通过 ghost route 转发到 LOS 新地址 |
| LOS 满未迁移 | false | true | → `TryUntagRefField` → 去掉 tag bit，返回原地址 |
| 未参与迁移 | false | false | → 引用未 tagged，快速路径直接返回 |

---

## 6. 验证结果

### 6.1 启用迁移功能

```bash
cjEnableLOSMigration=1 MRT_REPORT=./gc_migrate_fixed.log cjHeapSize=64mb ./main_migrate_verify
```

```
[Phase 5] Triggering migration GC rounds...
  GC round 0: 40/40 objects intact - PASSED
  GC round 1: 40/40 objects intact - PASSED
  GC round 2: 40/40 objects intact - PASSED
  GC round 3: 40/40 objects intact - PASSED
  GC round 4: 40/40 objects intact - PASSED

[Phase 6] Extended verification (post-ghost lifecycle)...
  Extended round 0: all 20 objects PASSED
  Extended round 3: all 20 objects PASSED
  Extended round 6: all 20 objects PASSED
  Extended round 9: all 20 objects PASSED

==============================================
   ALL TESTS PASSED
==============================================
```

GC 日志确认迁移实际发生：
```
LOS migration: 12 scheduled, 0 dead, 9 LOS-full
LOS migration: region 0x... @[...] size=524304 -> LOS 0x...  (×12)
```

### 6.2 未启用迁移（回归验证）

```bash
MRT_REPORT=./gc_migrate_nolos.log cjHeapSize=64mb ./main_migrate_verify
```

```
ALL TESTS PASSED
```

无回归。

---

## 7. 经验总结

### 7.1 大对象标记的隐含假设

GC 的大对象标记依赖 `isMarked` 标志（而非 bitmap）。正常流程中 `ResetMarkBit()` 由 `CollectLargeGarbage` 遍历 `oldLargeRegionList` 调用。**将 region 移出 `oldLargeRegionList` 后，必须手动 `ResetMarkBit()`**，否则 `isMarked` 残留为 1，导致 trace 跳过 `AddLiveByteCount`。

### 7.2 `ClearLiveInfo()` ≠ 完整的标记状态重置

`ClearLiveInfo()` 只清除 `liveInfo` 指针和 `liveByteCount`，**不清除** `isMarked`。对大对象 region 而言，正确的重置序列是：

```cpp
region->ClearLiveInfo();   // liveByteCount → 0
region->ResetMarkBit();    // isMarked → 0
```

### 7.3 二分诊断法的有效性

通过将 `BuildLargeObjectMigrationRoutes` 的循环体逐步简化，快速定位到死对象回收路径（`GetLiveByteCount() == 0` 分支）是问题所在：

| 实验 | 内容 | 结果 | 结论 |
|------|------|------|------|
| 控制组 | 不启用迁移 | PASS | 基准正常 |
| 诊断1 | 全部标 UNMOVABLE | PASS | tag→untag 路径正确 |
| 诊断2 | 恢复死对象回收 + UNMOVABLE | FAIL | 死对象判断有误 |
| 修复后 | 添加 `ResetMarkBit()` | PASS | 根因确认 |
