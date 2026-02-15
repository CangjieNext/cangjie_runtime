# 仓颉 Runtime GC 与大对象处理（源码视角）

## 1. 总览

当前仓颉 Runtime 的 GC 由 `CollectorProxy` 统一调度，实际执行器是 `WCollector`（平滑并发复制 GC）。

大对象（Large Object）不是独立虚拟地址空间的 LOS，而是在统一 region 堆中以大对象 region 列表管理：

- `oldLargeRegionList`
- `recentLargeRegionList`
- `largeTraceRegions`

## 2. 关键模块

- `src/Heap/Collector/CollectorProxy.cpp`
  - 所有 GC reason 目前都路由到 `WCollector`
- `src/Heap/WCollector/WCollector.cpp`
  - 主流程：`TraceHeap -> PostTrace -> Preforward -> ForwardFromSpace -> CollectSmallSpace`
- `src/Heap/Allocator/RegionSpace.cpp`
  - 分配入口，决定是否走大对象分配路径
- `src/Heap/Allocator/RegionManager.h/.cpp`
  - 大对象分配、装配候选、回收、统计
- `src/Heap/Allocator/RegionInfo.h`
  - 大对象 region 的类型、标记、遍历语义

## 3. 大对象判定与分配

### 3.1 判定条件

`RegionSpace::TryAllocateOnce()` 中，若：

- `allocSize >= regionManager.GetLargeObjectThreshold()`

则调用 `RegionManager::AllocLarge(allocSize)`。

### 3.2 阈值来源

`RegionManager::SetLargeObjectThreshold()`：

- 环境变量：`cjLargeThresholdSize`
- 默认：`32KB`
- 最终阈值会被 `regionSize` 上限截断

### 3.3 分配动作

`AllocLarge(size)` 的核心行为：

1. 计算需要的 unit 数（按 page-size 对齐）
2. 通过 `TakeRegion(..., LARGE_SIZED_UNITS)` 申请连续 region
3. 在该 region 上直接 `Alloc(size)`
4. 根据 GC 阶段：
   - tracing 期优先进入 `largeTraceRegions`
   - 否则进入 `recentLargeRegionList`

## 4. GC 时的大对象生命周期

### 4.1 候选装配

`AssembleGarbageCandidates()` 会调用 `AssembleLargeGarbageCandidates()`：

- 将 `recentLargeRegionList` 合并进 `oldLargeRegionList`
- 清空 live 信息，准备本轮重新标记

### 4.2 标记语义

在 `RegionInfo` 中：

- 大对象 region 只视作一个对象（offset=0）
- `VisitAllObjects()` 对大对象只访问 region 起始对象
- `MarkObject()` 对大对象用 region 级标记位，不走小对象位图细粒度扫描

### 4.3 回收语义（重点）

`WCollector::PostTrace()` 中会立即执行 `CollectLargeGarbage()`。

`RegionManager::CollectLargeGarbage()` 逻辑：

- 遍历 `oldLargeRegionList`
- 若对象未存活（`!IsSurvivedObject(0)`）则回收该 region
- 回收分两档：
  - `regionSize > LARGE_OBJECT_RELEASE_THRESHOLD(4MB)`：`ReleaseRegion()`，倾向归还物理内存
  - 否则：`CollectRegion()`，进入垃圾列表复用
- 若对象存活：仅 `ResetMarkBit()`，不搬迁

同时会遍历 `recentLargeRegionList` 做 mark 位复位。

## 5. 与小对象复制路径的关系

- 小对象主要走 from-region 路由/复制/转发
- 大对象不参与该转发链路，避免高成本复制
- 因此当前大对象策略可概括为：
  - **分配时独立归类**
  - **回收时标记清理 region**
  - **超大 region 优先释放给 OS**

## 6. 观测与调优

### 6.1 可观测统计

`GCStats` 中有专门指标：

- `largeSpaceSize`
- `largeGarbageSize`
- `collectedBytes`

可结合 GC 日志和 `DumpRegionStats()` 观察大对象压力。

### 6.2 可调参数

- `cjLargeThresholdSize`：大对象阈值
- `cjPinnedRegionSize`：影响 pinned 区竞争，间接影响 region 可用性

## 7. 阅读建议（从入口到细节）

1. `CollectorProxy.cpp`：确认 collector 选择
2. `WCollector.cpp`：看 GC 阶段与 `PostTrace` 回收顺序
3. `RegionSpace.cpp`：看分配分流
4. `RegionManager.h/.cpp`：看大对象全链路
5. `RegionInfo.h`：看大对象标记/遍历基本语义

## 8. 小结

当前实现并非“独立 LOS 堆段”，而是“统一堆 + 大对象 region 列表管理”。

优势是实现与并发复制 GC 的协作成本低；风险点在于大对象对连续 units 的需求会放大碎片敏感性，因此阈值与回收策略的观测和调优非常关键。
