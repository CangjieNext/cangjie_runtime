# M4a 实现记录：LOS 滑动整理（Sliding Compaction）

## 1. 目标

在 GC 清扫（M3）之后对 LOS 空间执行条件性滑动整理，消除碎片、回收空间并归还给 RESERVE。

核心目标：
- **消除碎片**：将可移动对象向高地址（losEnd）滑动紧凑排列
- **重建空闲管理**：整理后完全重建 losFreeMgr 的两棵 CartesianTree
- **归还 RESERVE**：整理释放的低地址空间立即归还给 RESERVE

## 2. 修改文件清单

| 文件 | 改动类型 | 说明 |
|------|----------|------|
| `src/Heap/Allocator/FreeRegionManager.h` | 新增方法 | `ClearTrees()` — 清空 dirtyTree + releasedTree |
| `src/Heap/Allocator/LOSManager.h` | 新增声明 | `CompactLOS()`, `NeedsCompaction()`, `GetFragmentationRatio()`, compaction 统计字段, `RebuildFreeRegionManager()`, `ShrinkToReserve()` |
| `src/Heap/Allocator/LOSManager.cpp` | 新增实现 | 5 个 M4a 函数实现 + 修改 `MigrateLargeObjectsFromMain` 为追加模式 |
| `src/Heap/WCollector/WCollector.h` | 新增声明 | `CompactLOSIfNeeded()` |
| `src/Heap/WCollector/WCollector.cpp` | 新增实现 | `CompactLOSIfNeeded()` 实现 + PostTrace 流程插入 + 修改 `MigrateLargeObjectsToLOS` 处理合并转发表 |

## 3. 设计决策

### 3.1 插入位置

PostTrace 中的执行顺序：

```
PostTrace()
  → HandleTraceRegions()
  → ClearWeakRefBuffer / ClearBuffer
  → PrepareCycleRef()
  → CollectLargeGarbage()         // 清扫 MAIN + LOS 死对象
  → CompactLOSIfNeeded()          // ★ M4a: 条件性 LOS 整理
  → MigrateLargeObjectsToLOS()    // M3: MAIN → LOS 迁移
  → CollectPinnedGarbage()
  → RefineFromSpace()
  → PrepareForwardTable()
```

**理由**：清扫后碎片信息最准确；整理后 LOS 有更大连续空间，利于后续迁移。

### 3.2 触发条件

条件性触发（不是每次 GC 都整理）：

1. **碎片率阈值**：`(dirtyUnits + releasedUnits) / totalActiveUnits > 0.3`
2. **分配失败计数**：自上次整理以来有新的 LOS `AllocLarge` 失败

阈值常量：`LOS_COMPACTION_FRAG_THRESHOLD = 0.3`

### 3.3 整理策略

**滑动整理向高地址（losEnd）靠拢，策略 A（Pinned 屏障）**：

- 找到最高地址的 pinned 对象（`rawPointerCount > 0`）的 `regionEnd` 作为屏障
- 只整理屏障以上的可移动对象
- 从 losEnd 向低地址分配目标位置，对象紧凑排列

### 3.4 Forwarding Map 合并

M4a compaction 和 M3 migration 共享同一个 `migrationMap`：

- `CompactLOS()` 先执行，添加 compaction 转发条目并立即更新引用
- `MigrateLargeObjectsFromMain()` 追加 migration 条目（不再清空 map）
- `MigrateLargeObjectsToLOS()` 最后统一调用 `UpdateMigratedReferences()` + `ClearMigrationState()`

### 3.5 losFreeMgr 重建

整理后完全重建 `losFreeMgr`：

1. 调用 `ClearTrees()` 清空 dirtyTree + releasedTree
2. 遍历排序后的存活 region 列表，计算相邻间隙
3. 对每个间隙：`InitRegionAt` + `InitFreeUnits` + `AddGarbageUnits`

### 3.6 RESERVE 归还

整理释放的低地址空间立即通过 `ReserveManager::ReturnFromLOS()` 归还。

## 4. GC 流程中的 LOS 整理

```
                ┌─────────────────────┐
                │ CollectLargeGarbage  │   清扫 MAIN + LOS 死对象
                └──────────┬──────────┘
                           │
                           ▼
                ┌─────────────────────┐
                │ CompactLOSIfNeeded  │   检查碎片率 / 分配失败
                └──────────┬──────────┘
                           │
              NeedsCompaction()?
              ┌────────────┴────────────┐
              │ YES                     │ NO
              ▼                         ▼
   ┌──────────────────┐          (skip)
   │   CompactLOS()   │
   ├──────────────────┤
   │ Phase1: 收集     │  分离 movable / pinned
   │ Phase2: 计算     │  从 losEnd 向下分配目标地址
   │ Phase3: 转发表   │  构建 forwarding map
   │ Phase4: 引用更新 │  UpdateMigratedReferences()
   │ Phase5: 移动     │  memmove_s + 重建 RegionInfo
   │ Phase5b: 重建列表│  clear + prepend all live regions
   │ Phase6: 重建FRM  │  ClearTrees + insert gaps
   │ Phase7: 归还     │  ShrinkToReserve()
   └──────────────────┘
              │
              ▼
   ┌──────────────────────────┐
   │ MigrateLargeObjectsToLOS │  MAIN → LOS 迁移 (追加 forwarding)
   └──────────────────────────┘
```

### 整理前后内存布局

```
整理前：
losStart                                            losInactiveZone    losEnd
    |     [obj1]  [   ]  [obj2]  [PIN]  [obj3]  [   ]  [obj4]     |         |
    v                                                              v         v
    低地址 ──────────────────────────────────────────────────> 高地址

整理后：
losStart      newLosInactiveZone                                   losEnd
    |    [空闲区域]  [PIN]  [obj1] [obj2] [obj3] [obj4]            |         |
    v                 ^                                             v         v
    低地址 ───────────│────────────────────────────────────> 高地址
                      │
               PIN 对象不动，可移动对象向高地址紧凑排列
               低地址空闲空间归还 RESERVE
```

## 5. 新增 API 说明

### LOSManager

| 方法 | 说明 |
|------|------|
| `NeedsCompaction()` | 检查是否需要整理：碎片率 > 30% 或有新的分配失败 |
| `CompactLOS()` | 执行 7 阶段滑动整理算法，返回释放字节数 |
| `GetFragmentationRatio()` | 计算碎片率 = freeUnits / activeUnits |
| `RebuildFreeRegionManager()` | 清空并重建 losFreeMgr 的两棵树 |
| `ShrinkToReserve()` | 归还低地址空闲空间给 RESERVE |

### FreeRegionManager

| 方法 | 说明 |
|------|------|
| `ClearTrees()` | 清空 dirtyTree + releasedTree（线程安全） |

### WCollector

| 方法 | 说明 |
|------|------|
| `CompactLOSIfNeeded()` | PostTrace 中调用，条件性执行 LOS 整理 |

### 统计字段（LOSManager 新增）

| 字段 | 含义 |
|------|------|
| `losCompactCount` | 整理执行次数 |
| `losCompactMovedBytes` | 上次整理移动字节数 |
| `losCompactFreedBytes` | 上次整理释放字节数 |
| `losCompactShrunkBytes` | 上次整理归还 RESERVE 字节数 |
| `lastCompactAllocFailCount` | 上次整理时的 allocFailCount（冷却计数） |

## 6. 异步整理（Async Compaction）

### 6.1 动机

同步整理（上述 7 阶段 `CompactLOS()`）需要在 STW 期间由 GC 线程触发。然而，在某些场景下 LOS 碎片化严重但常规 GC 未被触发（例如堆阈值未达到），此时 LOS 分配频繁失败并降级到 MAIN，导致 MAIN 空间压力增大。

**异步整理**通过 `FinalizerProcessor` 线程（gc-helper）检测到 LOS 碎片化后主动请求一次同步 GC，在该 GC 的 STW PostTrace 阶段执行整理。

### 6.2 触发路径

```
AllocLarge()                        // RegionManager.h
  → losManager->AllocLarge() 失败
  → AllocLargeInMain() 成功（MAIN 降级）
  → if (!IsCompacting() && !IsCompactionRequested())
      → SignalLOSCompaction()       // RegionManager.cpp
          → losManager->SetCompactionRequested(true)
          → FinalizerProcessor::NotifyToCompactLOS()
              → shouldCompactLOS = true
              → Notify() 唤醒 FP 线程
```

### 6.3 FinalizerProcessor 主循环集成

`FinalizerProcessor::Run()` 主循环新增检查：

```cpp
// M4a-async: Check if async LOS compaction was requested
if (shouldCompactLOS.load(std::memory_order_relaxed)) {
    PerformLOSCompaction();
}
```

等待条件也包含 `shouldCompactLOS`，确保信号能唤醒休眠的 FP 线程。

### 6.4 PerformLOSCompaction() 流程

```
FinalizerProcessor::PerformLOSCompaction()
  │
  ├─ 获取 LOSManager（通过 RegionSpace → RegionManager）
  ├─ 双重检查：IsCompactionRequested() || NeedsCompaction()
  │   └─ 如果都不满足 → 清除 shouldCompactLOS, return
  │
  ├─ Step 1: losMgr->SetCompacting(true)
  │   └─ 阻止所有 LOS 新分配（强制走 MAIN 降级）
  │
  ├─ Step 2: Collector::RequestGC(GC_REASON_HEU_SYNC, false)
  │   └─ 阻塞等待 GC 完成
  │   └─ 在 STW PostTrace 中：
  │       CompactLOSIfNeeded()
  │         → losManager->NeedsCompaction() || IsCompactionRequested()
  │         → SetCompacting(true)  // 同步路径的 guard
  │         → CompactLOS()          // 执行 7 阶段滑动整理
  │         → SetCompactionRequested(false)
  │         → SetCompacting(false)
  │
  ├─ Step 3: losMgr->SetCompacting(false)
  │   └─ 恢复 LOS 分配
  │
  └─ shouldCompactLOS = false
```

**关键安全性**：FinalizerProcessor 线程在 `Init()` 中调用了 `EnterSaferegion()`，因此请求同步 GC 是安全的——GC 线程可以立即进入 STW，不会因等待 FP 线程到达安全点而死锁。

### 6.5 状态标志说明

| 标志 | 位置 | 含义 |
|------|------|------|
| `LOSManager::isCompacting` | `std::atomic<bool>` | 整理进行中（同步或异步），阻止所有 LOS 新分配 |
| `LOSManager::compactionRequested` | `std::atomic<bool>` | 分配路径设置，PostTrace 中 `CompactLOSIfNeeded()` 检查 |
| `FinalizerProcessor::shouldCompactLOS` | `std::atomic<bool>` | FP 线程内部信号，控制是否执行 `PerformLOSCompaction()` |

### 6.6 同步 vs 异步整理对比

| 维度 | 同步整理 | 异步整理 |
|------|----------|----------|
| 触发方 | GC 线程（PostTrace 中 `NeedsCompaction()` 返回 true） | 分配线程（LOS 分配失败 → `SignalLOSCompaction()`） |
| 执行位置 | 当前 GC 周期的 PostTrace | 下一次 GC（由 FP 请求的 sync GC）的 PostTrace |
| 实际整理代码 | 相同：`CompactLOSIfNeeded()` → `CompactLOS()` | 相同 |
| 前置条件 | 碎片率>30% 或有新分配失败 | `compactionRequested=true`（分配失败降级时设置） |
| 分配阻塞 | STW 期间自然阻塞 | `isCompacting=true` 显式阻塞 LOS 分配 |

### 6.7 修改文件清单（异步部分）

| 文件 | 改动类型 | 说明 |
|------|----------|------|
| `src/Heap/Allocator/LOSManager.h` | 新增字段 | `isCompacting`, `compactionRequested` 原子标志 + accessor |
| `src/Heap/Allocator/RegionManager.h` | 修改 `AllocLarge()` | 添加 `IsCompacting()` 检查跳过 LOS + `IsCompactionRequested()` 防重复信号 |
| `src/Heap/Allocator/RegionManager.cpp` | 新增 `SignalLOSCompaction()` | 设置 `compactionRequested` + 通知 FP |
| `src/Heap/Collector/FinalizerProcessor.h` | 新增字段+方法 | `shouldCompactLOS` 原子标志, `NotifyToCompactLOS()`, `PerformLOSCompaction()` 声明 |
| `src/Heap/Collector/FinalizerProcessor.cpp` | 新增实现+修改 `Run()` | `PerformLOSCompaction()` 实现, 主循环+等待条件添加检查 |
| `src/Heap/WCollector/WCollector.cpp` | 修改 `CompactLOSIfNeeded()` | 检查 `IsCompactionRequested()` 作为触发条件 + 整理后清除标志 |

## 7. 编译验证

```
Build and preinstall completed successfully.
```

macOS aarch64 Release 构建成功，无错误。
