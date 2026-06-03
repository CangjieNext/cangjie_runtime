# Step1 分配器实现详解

> 位置：MSGC Step1 的实现说明文档。
> 目的：结合 Plan 文档和当前 runtime 源码，说明 MSGC 分配器现在是如何设计、如何分层、以及关键路径实际上是怎么跑起来的。
> 口径：**以当前代码为准**，不是重复一版设计稿。

---

## 目录

1. [总览](#1-总览)
2. [文档与代码映射](#2-文档与代码映射)
3. [内存布局与元数据](#3-内存布局与元数据)
4. [中央池的设计](#4-中央池的设计)
5. [大对象的分配与释放路径](#5-大对象的分配与释放路径)
6. [堆增长与收缩](#6-堆增长与收缩)
7. [分配器与 GC 的耦合点](#7-分配器与-gc-的耦合点)
8. [当前还能继续优化的点](#8-当前还能继续优化的点)

---

## 1. 总览

这一节不再先下结论，而是先把 allocator 的演进顺序说清楚：**老路径是 CopyGC 的 region-copy allocator，Step1 落地后切换成了 MSGC 的 size-class + central-pool allocator。** CopyGC 仍然保留 `RegionSpace::Allocate()` 式的“失败后触发 GC / 等待 GC / OOM 前最后重试”骨架；MSGC 小对象路径也沿用这套框架，但 `>= 16KB` 的 large path 已经收紧成“单次尝试，失败直接 OOM”。

可以先把两边的主链路并排记成下面这样：

```text
CopyGC:
RegionSpace
  -> AllocBuffer
    -> RegionManager
      -> garbageRegionList / freeRegionManager / inactiveZone

MSGC:
RegionSpaceMS
  -> AllocBufferMS
    -> CentralRegionPool
      -> MSRegionManager
        -> MSRegionInfo / MSBitmapArena / MSFreeManager
```

最大的结构变化有三点：

1. CopyGC 的小对象主路径本质上是“线程本地 region bump 分配”，MSGC 则变成“size class -> active region -> central pool -> lazy sweep / bitmap reuse”。
2. CopyGC 把“大对象 / pinned 对象 / raw pointer 对象”分成多条旁路；MSGC 则把普通小对象统一收敛到 `AllocBufferMS`，把 `>= 16KB` 的对象统一收敛到 in-heap large region path。
3. CopyGC 的 mutator 分配热路径基本不碰 sweep；MSGC 的 mutator 在缺货时会直接卷入 `CentralRegionPool` 的 lazy sweep、empty cache 复用和 mark-assist 协作。

### 1.1 CopyGC 分配流程总览

CopyGC 当前的入口在 `RegionSpace::TryAllocateOnce()`，主分流规则是：

1. `allocType == PINNED_OBJECT` 时走 `RegionManager::AllocPinned()`。
2. `allocSize >= regionManager.GetLargeObjectThreshold()` 时走 `RegionManager::AllocLarge()`。
3. 其他普通对象走 `AllocBuffer::Allocate()`。

其中大对象阈值来自 `RegionManager::SetLargeObjectThreshold()`，默认是 `32KB`，可以被 `cjLargeThresholdSize` 覆盖，但最终不会超过 region size。

### 1.2 CopyGC 详细流程图

```text
[用户请求分配]
    |
    v
RegionSpace::Allocate(size, allocType)
    |
    |-- ToAllocSize(size)
    |-- TryAllocateOnce(allocSize, allocType)
            |
            |-- allocType == PINNED_OBJECT ?
            |       |
            |       `--> RegionManager::AllocPinned(size)
            |              |-- recentPinnedRegionList 头 region 直接 Alloc
            |              |-- 不够则尝试 freePinnedSlotLists
            |              `-- 还不够则 TakeRegion(...) 新建 pinned region
            |
            |-- allocSize >= largeObjectThreshold ?
            |       |
            |       `--> RegionManager::AllocLarge(size)
            |              |-- regionCount = ceil(size / UNIT_SIZE)
            |              |-- TakeRegion(regionCount, LARGE_SIZED_UNITS)
            |              `-- 放入 recentLargeRegionList / largeTraceRegions
            |
            `--> AllocBuffer::Allocate(totalSize, allocType)
                    |-- tlRegion 非空 ?
                    |      `-- tlRegion->Alloc(totalSize) 成功则直接返回
                    |
                    `-- AllocateImpl(totalSize, allocType)
                           |-- 当前 tlRegion 满了
                           |      |-- RemoveThreadLocalRegion(tlRegion)
                           |      `-- EnlistFullThreadLocalRegion(tlRegion)
                           |
                           |-- preparedRegion 有货 ?
                           |      |-- 取走 preparedRegion
                           |      |-- 设成新的 tlRegion
                           |      `-- 通知 finalizer/gc-helper 继续喂下一块
                           |
                           `-- 否则 RegionManager::AllocateThreadLocalRegion()
                                  `-- TakeRegion(maxUnitCountPerRegion, SMALL_SIZED_UNITS)
                                         |-- 先尝试 garbageRegionList
                                         |-- 再尝试 freeRegionManager
                                         `-- 最后从 inactiveZone bump 切新 region
```

如果上面任何一条路返回 `0`，外层 `RegionSpace::Allocate()` 会进入统一重试框架：要么请求 HEU/OOM GC，要么等待正在进行的 GC 结束，再重新走一次 `TryAllocateOnce()`。

### 1.3 从 CopyGC 切到 MSGC 之后，分配器怎么变了

切到 MSGC 后，小对象外层重试骨架几乎没变，但 large path 已经单独收紧成“单次尝试，失败直接 OOM”；与此同时，内层分配模型从“拿一整个 thread-local region 然后线性 bump”为主，改成了“按 size class 取合适 region，并在中央池里做 sweep 后复用”。

当前 MSGC 的两个关键分流点是：

1. `RegionSpaceMS::TryAllocateOnceMS()` 用 `allocSize >= kLargeObjectThreshold(16KB)` 决定 large path。
2. 小对象进入 `AllocBufferMS::Allocate()` 后，会先做 `SizeClassInfo::SizeToClass(size)`，再按 class 找 active region。

### 1.4 MSGC 详细流程图

```text
[用户请求分配]
    |
    v
RegionSpaceMS::Allocate(size, allocType)
    |
    |-- ToAllocSize(size)
    `-- TryAllocateOnceMS(allocSize)
            |
            |-- allocSize >= 16KB ?
            |       |
            |       `--> large path
            |              |-- unitCount = ceil(allocSize / pageSize)
            |              |-- MSRegionManager::TakeRegionForLarge(unitCount, regionStart)
            |              |-- 失败则直接返回分配失败，外层立刻抛 OOM
            |              `-- RegisterLargeObjectMS(addr, regionBytes)
            |                     |-- base entry 初始化为 LARGE
            |                     `-- 后续 span entry 当前仍标成 LARGE，并在 reserved 中回指 base idx
            |
            `--> small path
                    `-- AllocBufferMS::Allocate(allocSize)
                           |-- cls = SizeClassInfo::SizeToClass(size)
                           |-- FastAlloc(cls)
                           |      |-- activeRegions_[cls] 为 BUMP
                           |      |      `-- region->AllocBump()
                           |      `-- activeRegions_[cls] 为 PARTIAL
                           |             `-- region->AllocFromBitmapFast()
                           |
                           `-- FastAlloc 失败 -> SlowAlloc(cls)
                                  |-- ReturnRegion(cls)
                                  |-- cachedRegion_[cls] 有货 ? 直接接管
                                  `-- 否则 RefillFromCentral(cls)
                                         `-- CentralRegionPool::GetRegion(cls)
                                                |-- P1 partialSwept
                                                |-- P2 partialUnswept -> 原地 lazy sweep
                                                |-- P3 fullUnswept -> 原地 lazy sweep
                                                |-- P4 emptySweptCache -> Reset/Rebind size class
                                                `-- P5 fresh region
                                                       `-- MSRegionManager::TakeRegions(...)
                                                              |-- 先尝试 released tree
                                                              |-- 再尝试 inactive zone bump
                                                              `-- 不够则 GrowOneSegment
```

MSGC 还有三层 CopyGC 没有的耦合点：

1. `AllocBufferMS::Allocate()` 在 marking phase 下会 `MarkNewObject()`，新对象直接黑化。
2. mutator 在 `SlowAlloc()` 缺货时可能顺手触发 `CentralRegionPool` 的 lazy sweep。
3. `gcAssistBytes_` 超过阈值后，mutator 还会进入 `MSMarkAssist()`，把一部分标记工作摊回分配路径。

### 1.5 CopyGC 与 MSGC 的优缺点

#### CopyGC 的优点

1. 小对象热路径非常短，核心就是 `tlRegion->Alloc()` 的线性 bump，吞吐友好。
2. mutator 分配路径基本不承担 sweep/bitmap 维护，热路径更干净，更容易稳定压低单次分配开销。
3. 对可移动对象来说，复制回收天然能整理空间，碎片压力通常比 mark-sweep 小。
4. `preparedRegion + hungry buffer` 的预取机制很直接，线程本地缺货时的恢复路径相对简单。

#### CopyGC 的缺点

1. 大对象、pinned 对象、raw pointer 对象都是旁路，分配路径分叉较多，维护成本不低。
2. 回收依赖复制/转发，GC 周期里的内存带宽成本更高，分配成功也受 evacuation 容量影响。
3. region 复用粒度偏粗，很多优化点集中在 region 级别，难以像 bitmap allocator 那样细粒度复用 slot。
4. 对 pinned / unmovable 对象较多的场景，CopyGC 的“移动整理”优势会被明显削弱。

#### MSGC 的优点

1. 小对象通过 size class + bitmap 复用 slot，不需要等整块 region 重新成为 from/to-space 才能再利用。
2. `CentralRegionPool` 把 `partialSwept / unswept / empty cache / fresh` 串起来，复用链路比 CopyGC 更细。
3. large object 主路径直接走 in-heap large region，和小对象元数据体系是一致的，口径更统一。
4. mark-at-alloc、assist debt、background sweep、empty cache scavenging 已经接到 allocator 上，便于把 GC 成本摊薄到平时。
5. empty cache 和 shrink 机制可以更主动地平滑 RSS，而不是只在 GC 大周期里集中处理。

#### MSGC 的缺点

1. 热路径比 CopyGC 复杂得多，不再是单一 bump allocator，而是 class lookup、state 判断、central refill、bitmap 分配的组合。
2. mutator 在缺货时会被卷入 lazy sweep 和 assist，延迟稳定性通常不如纯 bump 路径。
3. `MSRegionInfo / MSBitmapArena / CentralRegionPool / MSRegionManager` 的状态协作更复杂，正确性风险也更高。
4. mark-sweep 天生更依赖元数据和启发式参数，empty cache、水位、后台 sweep slice 等参数需要持续调优。
5. 不移动对象意味着碎片控制更多依赖 size class、bitmap summary 和 reclaim 策略，而不是复制整理天然解决。

## 2. 文档与代码映射

Step1 在 Plan 里分成三个子题，当前实现基本对应到下面这些文件：

| Plan 文档 | 当前实现文件 | 当前职责 |
| --- | --- | --- |
| Step1 主文档 | `src/Heap/MAllocator/MSRegionSpace.h/.cpp` | 统一分配入口，决定小对象还是大对象路径 |
| Step1.1 元数据与 bitmap | `src/Heap/MAllocator/MSRegionInfo.h/.cpp` | 每个 MS region 的元数据、bitmap 分配、sweep 行为 |
| Step1.1 bitmap 生命周期 | `src/Heap/MAllocator/MSBitmapArena.h/.cpp` | bitmap sidecar 大预留、按区间 commit / shrink |
| Step1.2 size class | `src/Heap/MAllocator/MSSizeClass.h/.cpp` | 36 级 size class 表、查表、slot 数计算 |
| Step1.2 线程本地分配 | `src/Heap/MAllocator/MSAllocBuffer.h/.cpp` | 每 mutator 的活动 region、缓存 region、assist debt |
| Step1.2 中央池 | `src/Heap/MAllocator/MSCentralRegionPool.h/.cpp` | partial/full swept/unswept 管理、lazy sweep、empty cache |
| Step1.3 堆增长与 large path | `src/Heap/MAllocator/MSRegionManager.h/.cpp` | 虚拟大预留、released/bump 供给、large region、shrink |

建议阅读顺序也是这个顺序：先看 `MSRegionSpace`，再看 `MSAllocBuffer`，然后看 `MSCentralRegionPool`、`MSRegionInfo`、`MSBitmapArena`、`MSRegionManager`。

## 3. 内存布局与元数据

这一节不再讲入口调度，而是只回答两个问题：

1. CopyGC 和 MSGC 的堆与元数据在内存里到底怎么排布。
2. `MSRegionInfo`、bitmap、region 生命周期切换时，元数据到底怎么变。

### 3.1 CopyGC 的内存布局

CopyGC 当前的 `RegionManager::Initialize()` 也是“元数据在前、对象堆在后”的布局，但它的元数据组织方式和 MSGC 很不一样。它不是一个简单的 `RegionInfo[]`，而是一个**按 unit 建模的镜像区**：每个 heap unit 在堆前面都有一个对应的 `UnitInfo`。

源码里的心智模型可以直接写成：

```text
... | UI(n+2) | UI(n+1) | UI(n) | ...metadata... | heapStart | U(n) | U(n+1) | U(n+2) | ...
```

这里有几个关键事实：

1. CopyGC 的最小管理粒度是 `UNIT_SIZE`，也就是系统页大小；region 只是若干个 unit 的聚合。
2. `UnitInfo` 存放在 `heapStartAddress` 之前，并且是**反向镜像**，`GetUnitInfo(idx)` 本质上是 `heapStart - (idx + 1) * sizeof(UnitInfo)`。
3. 一个 region 的主元数据只放在它的首 unit 上，这个首 unit 被当成 `RegionInfo` 使用；后续 subordinate unit 只通过 `ownerRegion` 指回主 region。
4. 所以 CopyGC 的元数据地址换算是“先按 unit 找镜像，再看当前 unit 是主 unit 还是 subordinate”。

这套布局和 CopyGC 的工作方式强相关，因为它不仅要管分配，还要管：

1. `FROM/TO` region 迁移。
2. ghost region 和记 route。
3. pinned / raw pointer / large / thread-local 等多种 region list。
4. moving GC 需要的 `LiveInfo / RouteInfo / RwLock / RouteState`。

### 3.2 MSGC 的内存布局

MSGC 在 `MSRegionManager::Initialize()` 里仍然保持“前元数据、后堆区”的大布局，但元数据形态被大幅简化了：

```text
[ MSRegionInfo[maxVirtualRegionCount] | Heap[maxVirtualHeap] ]
^ mmapBase                           ^ heapStart             ^ maxHeapEnd
```

和 CopyGC 相比，这里有四个决定性的变化：

1. 元数据不再按 unit 建模，而是直接按 `MS_REGION_SIZE = 32KB` 建模，一个 region 对应一个 `MSRegionInfo`。
2. `MSRegionInfo` 数组与 heap 中的 region 按 region index 一一对应；当前实现采用的是正向平铺数组，`GetRegionAddress(idx)` 直接按 `heapStart + idx * MS_REGION_SIZE` 计算，`GetRegionInfoByIdx(idx)` 直接返回 `&array_[idx]`，并不是 CopyGC 那种“与 region 区反向镜像排列”的布局。
3. `MSRegionInfo` 本体固定为 64B，一个 cache line，专门压缩成 allocator / mark-sweep 关心的字段。
4. bitmap 不内嵌在 `MSRegionInfo`，也不在对象区旁边零散分配，而是统一放到独立的 `MSBitmapArena` sidecar mapping。

这里再补一个容易混淆的点：如果只看“名义映射关系”，确实就是 `idx <-> heapStart + idx * MS_REGION_SIZE` 的正向对应；但对象地址反查时，代码仍保留了 `GetRegionInfoValidated()` 的相邻 entry 校验逻辑，用来兜住底层返回地址只有 `UNIT_SIZE` 对齐、未必严格 `MS_REGION_SIZE` 对齐的情况。

MSGC 的完整布局实际上是两层：

```text
主保留区:
[ MSRegionInfo array | heap space ]

sidecar:
[ bitmap slot 0 | bitmap slot 1 | bitmap slot 2 | ... ]
每个 slot = [ allocBitmap | markBitmap | scannedBitmap ]
```

也就是说，MSGC 把“region 元数据”和“slot 级 bitmap 元数据”拆成了两套不同的映射：

1. `MSRegionInfo[]` 负责 region 级状态。
2. `MSBitmapArena` 负责 slot 级分配 / 标记 / scanned 状态。

从堆增长角度看，当前实现也不是靠“反向镜像增长”来扩元数据。初始化时已经一次性为最大虚拟堆保留了整段 `MSRegionInfo[maxVirtualRegionCount]` 区间；后续堆增长时，只需要对新增 committed region 对应的那段 entry 做 `InitializeFreshRegions()` 初始化，并同步让 `MSBitmapArena::EnsureRegionRangeCommitted()` 激活相应 sidecar chunk。

### 3.3 CopyGC 与 MSGC 的布局对比

如果把两边的差异压缩成一句话，就是：**CopyGC 是 unit-mirror 驱动的 moving-GC 元数据模型，MSGC 是 region-array + sidecar bitmap 驱动的 mark-sweep 元数据模型。**

具体对比可以落到下面几条：

1. CopyGC 以 unit 为基本元数据单元，MSGC 以 32KB region 为基本元数据单元。
2. CopyGC 的 `RegionInfo` 是从 `UnitInfo` 里“长出来”的，subordinate unit 通过 `ownerRegion` 反指；MSGC 则是平铺的 `MSRegionInfo[]`。
3. CopyGC 的 metadata 内容很重，里面带有 forwarding、ghost、route、rwlock、LiveInfo 等 moving GC 结构；MSGC 则把字段收敛到分配、sweep 和并发标记真正需要的最小集合。
4. CopyGC 的 mark/resurrect/enqueue bitmap 是通过 `LiveInfo` 按需惰性分配的；MSGC 的 bitmap 则对每个 region index 都有稳定 sidecar slot，只是按 committed region 范围分 chunk 激活。
5. CopyGC 的地址换算是“heap 前反向镜像”；MSGC 的地址换算是“heapStart + regionIdx * 32KB”的正向数组索引。

### 3.4 MSRegionInfo 的设计

`MSRegionInfo` 是 MSGC 里最核心的 region 级元数据，当前设计目标很明确：**用一个 cache line 放下分配、回收和并发标记真正需要的状态，并保持 `allocPtr` 在偏移 0 以兼容编译器 fast path ABI。**

当前字段可以按职责拆成六组：

1. 分配核心字段：`allocPtr`、`regionEnd`
2. region 链表字段：`nextRegionIdx`、`prevRegionIdx`
3. bitmap 指针：`allocBitmap`、`markBitmap`
4. 统计字段：`totalSlotCount`、`freeSlotCount`、`liveSlotCount`、`rawPointerObjCount`
5. 分类字段：`sizeClass`、`sweepState`、`regionType`
6. 协议字段：`isMarked / isEnqueued / isResurrected / isScanned`、`sweepGen`、`reserved`

其中最值得单独记住的是三点：

1. `allocPtr` 必须在偏移 0，这和 `AllocBufferMS` 兼容旧 fast path 的 ABI 假设直接相关。
2. `sweepGen` 不是普通统计字段，而是 lazy sweep 的所有权协议字段，用来做 `sg-2 -> sg-1 -> sg` 的 CAS 认领。
3. `reserved` 不是闲置位，它当前一物两用：
   - 小对象 `PARTIAL` region 下，它是 bitmap summary，`bit[i] = 1` 表示 `allocBitmap[i]` 这一整 word 里还有空槽。
   - large span 的 secondary entry 下，它保存 primary/base region idx。

### 3.5 Bitmap 的设计

MSGC 当前的 bitmap 设计不是“每个 region 自己 new 一套 bitmap”，而是**每个 region index 在 `MSBitmapArena` 里绑定一个稳定 slot**。

当前一个 slot 的布局固定为：

```text
[ allocBitmap | markBitmap | scannedBitmap ]
```

它的设计要点是：

1. `allocBitmap` 表示槽位分配状态，当前语义是 `1 = free, 0 = allocated`。
2. `markBitmap` 表示对象存活状态，语义是 `1 = alive, 0 = dead`。
3. `scannedBitmap` 不单独挂指针，而是通过 `markBitmap + bitmapWords` 推出来，表示对象子字段是否已经扫描。
4. `MSBitmapArena` 不是按 region 一个一个 commit，而是按 `kRegionsPerChunk = 256` 的 chunk 激活 / 回收。
5. `MSBitmapArena::GetSlotBase(regionIdx)` 保证 region idx 到 bitmap slot 是稳定映射，这样 bitmap 生命周期就和 region index 绑定，而不是和某次临时分配绑定。

这套设计和 CopyGC 的最大差别在于：CopyGC 的 mark/resurrect/enqueue bitmap 是 `LiveInfo` 里的惰性对象，而 MSGC 当前 allocator 常驻的位图只保留 `[allocBitmap | markBitmap]`。也就是说，小对象 region 现在不再显式保存 `scannedBitmap`，`grey/black` 的区别由 collector 的队列协议隐式表达，而不是由第三张位图表达。

当前代码里的状态迁移可以直接记成：

1. 新一轮标记开始前，`CentralRegionPool::ClearAllMarkBitmaps()` 统一清 `markBitmap`，把对象状态重置回白色起点。
2. root 枚举、并发 barrier 或 drain 首次发现对象时，会先通过 `TestAndSetMarkBit()` 把对象从 `white -> grey`；如果旧值已经是 1，则说明它此前已经被发现，不需要重复入队。
3. collector 真正取出 grey 对象后，默认把它当作“已发现但尚未完成 trace”的工作项，直接执行 `TraceObjectRefFields(...)`；是否已经 trace 完由 queue membership 协议保证，而不再依赖对象上的 `scanned` 位。
4. 标记期新分配对象走 `MarkSweepCollector::MarkNewObject()`，当前实现只做 `MarkObject(obj)`，也就是按 alloc-black 语义直接把对象视为 live，但不再写额外的 `scanned` 状态。

所以当前 MSGC 的关键点不再是“`mark + scanned` 两个显式状态位”，而是“`mark` 负责 white->grey，grey/black 的边界由 `TryEnqueueGreyObject + GreyWorkQueue` 协议隐式维护”。

### 3.6 Region 生命周期下，MSRegionInfo 怎么变化

当前 region 生命周期可以按元数据变化来读，而不是只按“名字”读。

#### 3.6.1 FREE -> BUMP

当 `GetFreshRegion()` 或 empty-cache rebind 选中一个 region 后，会调用 `MSRegionInfo::InitForSizeClass(cls)`，这一刻的关键元数据变化是：

1. `sizeClass = cls`
2. `regionType = MS_REGION_BUMP`
3. `sweepState = UNSWEPT`
4. `totalSlotCount/freeSlotCount/liveSlotCount` 根据 size class 重建
5. 绑定 `allocBitmap/markBitmap/scannedBitmap`
6. `allocBitmap` 置全 1，`mark/scanned` 清零
7. `reserved` 初始化成“所有 bitmap word 都可能有空位”的 summary
8. 调用方再补上 `allocPtr = regionStart`、`regionEnd = regionStart + MS_REGION_SIZE`

这一步本质上是在把一个 free shell 变成“可 bump 填充的新 region”。

#### 3.6.2 BUMP -> FULL / 返回中央池

在初次填充阶段，分配优先走 `AllocBump()`。随着 `allocPtr` 向前推进：

1. 每次成功 bump，`allocPtr` 前移一个 slot，`freeSlotCount--`
2. 当 mutator 归还 region 时，如果它还有空位，`ReturnPartialRegion()` 会把它放回 `partialSwept` 或 `partialUnswept`，并且**保留当前类型**，不强制改成 `PARTIAL`
3. 如果它已经没有空位，`ReturnFullRegion()` 会把 `regionType` 显式改成 `MS_REGION_FULL`

这也是为什么当前实现里“partial region”不完全等于“已经 bitmap sweep 过的 region”，因为 BUMP region 也可能以 partial 供应的身份回到中央池。

#### 3.6.3 UNSWEPT/FULL -> SWEEP -> PARTIAL/FULL/EMPTY_CACHE

当 `CentralRegionPool::GetRegion()` 或后台 sweep 线程认领一个 unswept region 时，会通过 `CasSweepGen()` 抢到 sweep 所有权，然后进入 `SweepRegion()`。这一阶段元数据变化最关键：

1. `allocBitmap = ~markBitmap`
2. 重新统计 `liveSlotCount` 和 `freeSlotCount`
3. 重新构建 `reserved` summary
4. `sweepState = SWEPT`
5. 清零所有 dead slot 的对象内容

然后根据 sweep 结果分叉：

1. 如果 `freeSlotCount == 0`，该 region 会被当作 `FULL`
2. 如果 `0 < freeSlotCount < totalSlotCount`，该 region 会被标成 `PARTIAL`
3. 如果 `freeSlotCount == totalSlotCount`，说明整个 region 已空，会进入 `PushEmptySweptRegion()`，随后执行 `ResetToEmptyCache()`

#### 3.6.4 EMPTY_CACHE -> 重新绑定 size class

`ResetToEmptyCache()` 不是把 region 还给底层，而是把它变成一个“空壳缓存”：

1. `ReleaseBitmaps()` 只把 `allocBitmap/markBitmap` 指针置空
2. `sizeClass = INVALID_SIZE_CLASS`
3. `regionType = MS_REGION_EMPTY_CACHE`
4. `sweepState = SWEPT`
5. `allocPtr/regionEnd/slotCount/live/free` 这些对象布局字段全部清空

注意这里最容易误解的一点：**empty cache 并不释放物理 bitmap 存储。** `ReleaseBitmaps()` 只是解绑指针，真正的 sidecar 物理内存仍由 `MSBitmapArena` 保留，直到 shrink 或 cleanup。

之后如果这个 empty shell 被再次拿来用，`TakeEmptySweptRegion()` 会重新：

1. `InitForSizeClass(cls)`
2. 绑定回该 region index 对应的固定 bitmap slot
3. 重建 `allocPtr/regionEnd`
4. 回到 `BUMP` 初始态

#### 3.6.5 LARGE / large span 从属 entry

large object 的元数据又是另一条路：

1. base region 用 `InitForLargeObject(start, end)` 初始化，`regionType = LARGE`
2. base entry 不使用 bitmap，`allocBitmap = nullptr`、`markBitmap = nullptr`
3. `totalSlotCount = 1`、`liveSlotCount = 1`、`freeSlotCount = 0`
4. 后续 span entry 继续使用 `LARGE`，并在 `reserved` 中写 base region idx

所以 large region 的元数据重点不在 slot bitmap，而在“这个 span 属于哪个 primary region”。

### 3.7 Bitmap 是怎么使用的

当前 bitmap 在 MSGC 里不是单点使用，而是同时服务分配、标记和 sweep 三个阶段。

#### 3.7.1 分配阶段

分配时只看 `allocBitmap + reserved summary`：

1. `AllocFromBitmapFast()` 先读 `reserved`
2. 用 `ctz(summary)` 找到“还有空位”的 bitmap word
3. 再在 `allocBitmap[wordIdx]` 里找第一个 `1` bit
4. 找到后把该 bit 清零，表示该 slot 已分配
5. 如果某个 word 被清空，就把 `reserved` 对应 bit 也清零

这就是它为什么快的原因：不是每次从头扫整个 bitmap，而是先用 `reserved` 过滤掉全满的 word。

#### 3.7.2 标记阶段

标记阶段现在主要看 `markBitmap`：

1. `SetMarkBit / TestAndSetMarkBit` 在 `markBitmap` 上打活对象位
2. `mark-at-alloc` 会让新对象在分配后立刻进入 `markBitmap`
3. 是否已经完成 trace 不再落在 region bitmap 上，而是由 grey queue 的生产/消费协议隐式表达

这意味着 MSGC 下“对象是否已经被当前 GC 发现”继续由 `markBitmap` 表达，而“对象是否仍在 grey work 系统中”改由 queue protocol 表达，不再额外引入 slot 级扫描完成位。

#### 3.7.3 Sweep 阶段

sweep 时，`markBitmap` 被当作这一轮活对象的最终真值：

1. `allocBitmap = ~markBitmap`
2. 据此得出可复用槽位
3. 重建 `reserved` summary
4. 把 dead slot 清零

这里还有一个容易写错的点：`markBitmap` **不是在 `SweepRegion()` 里清零的**，而是下一轮 `MarkSetup` 前统一 `ClearAllMarkBitmaps()`。当前代码故意把“清 mark”推迟到了下一轮，避免 sweep 过程中每个 region 再多做一次冗余 memset。

### 3.8 这一节最需要记住的结论

如果只保留三个结论，可以记这三个：

1. CopyGC 的核心元数据是“heap 前反向 unit mirror + 重型 RegionInfo/LiveInfo”，MSGC 的核心元数据是“正向 `MSRegionInfo[]` + sidecar bitmap slot”。
2. `MSRegionInfo` 负责 region 级生命周期，bitmap 负责 slot 级状态，两者明确分层。
3. empty cache 只解绑 bitmap 指针，不释放 sidecar 物理存储；真正的 bitmap 物理生命周期由 `MSBitmapArena` 的 chunk commit/shrink/cleanup 控制。

## 4. 中央池的设计

### 4.1 设计总览：中央池是后端，TLS buffer 是前端

当前小对象路径不是“mutator 直接向 `MSRegionManager` 要 region”，而是一个明确分层的前后端结构：

```text
RegionSpaceMS::TryAllocateOnceMS
  -> AllocBufferMS::Allocate(size)
    -> FastAlloc(cls) / SlowAlloc(cls)
      -> CentralRegionPool::GetRegion(cls)
        -> MSRegionManager::TakeRegion(...)
```

这层结构里，各自职责已经比较稳定：

1. `AllocBufferMS` 是 TLS 前端，只保留“当前正在用的 region”和“一个备用 region cache”。
2. `CentralRegionPool` 是真正的中枢，负责按 size class 组织 region、执行 lazy sweep、empty cache 复用、以及 fresh region 的统一入口。
3. `MSRegionManager` 退到最底层，只负责 committed region 的供给、released tree 复用、堆增长和 reclaim。

按当前代码，中央池在小对象路径上一共维护 **36 个 size class bucket**，也就是 `kNumSizeClasses = 36`，slot 大小覆盖 `8B ~ 16384B`。但要注意，**顶层普通分配分流条件**是在 `RegionSpaceMS::TryAllocateOnceMS()` 中直接判断：

1. `allocSize < 16384`：进入小对象路径，走 `AllocBufferMS + CentralRegionPool`。
2. `allocSize >= 16384`：直接进入 large path，走 `MSRegionManager::TakeRegionForLarge()`。

也就是说，以“中央池是否真的参与本次分配”为准，当前代码的边界是 **`>= 16KB` 就转 large path**。size class 表仍然保留了 `16384B` 这一档，原因是 lookup 表和 class 表本身是按“小对象上界”定义的；但在当前实现里，这一档主要承接的是“向上归并到 16KB slot、但顶层还没在上游被更早切走”的边界语义，而不是说所有 `16KB` 整点对象都会真的进入中央池。

当前这样切分，核心原因有三条：

1. **8B 对齐约束**：对象分配以 8B 对齐为基础，所以最小 class 从 8B 开始。
2. **控制 class 数量与碎片率的平衡**：相邻 class 增长率大致控制在 `12.5% ~ 25%`，避免 class 太少导致浪费太大，也避免 class 太多导致查表、管理和缓存元数据过重。
3. **32KB region 的半区边界**：`MS_REGION_SIZE = 32KB`，当对象达到 `16KB` 时，一个 region 最多只能容纳 2 个 slot；再往上继续走 size-class/bitmap 复用的收益就迅速下降，所以当前代码把 `>= 16KB` 直接切到 large path。

如果把中央池的 size class 分布压成一个表，可以直接写成下面这样：

| class | slot 大小 | Slots/Region | 前一级 | 最坏内部碎片率 |
| --- | ---: | ---: | ---: | ---: |
| 0 | 8 | 4096 | - | 0.0% |
| 1 | 16 | 2048 | 8 | 50.0% |
| 2 | 24 | 1365 | 16 | 33.3% |
| 3 | 32 | 1024 | 24 | 25.0% |
| 4 | 48 | 682 | 32 | 33.3% |
| 5 | 64 | 512 | 48 | 25.0% |
| 6 | 80 | 409 | 64 | 20.0% |
| 7 | 96 | 341 | 80 | 16.7% |
| 8 | 112 | 292 | 96 | 14.3% |
| 9 | 128 | 256 | 112 | 12.5% |
| 10 | 160 | 204 | 128 | 20.0% |
| 11 | 192 | 170 | 160 | 16.7% |
| 12 | 224 | 146 | 192 | 14.3% |
| 13 | 256 | 128 | 224 | 12.5% |
| 14 | 320 | 102 | 256 | 20.0% |
| 15 | 384 | 85 | 320 | 16.7% |
| 16 | 448 | 73 | 384 | 14.3% |
| 17 | 512 | 64 | 448 | 12.5% |
| 18 | 640 | 51 | 512 | 20.0% |
| 19 | 768 | 42 | 640 | 16.7% |
| 20 | 896 | 36 | 768 | 14.3% |
| 21 | 1024 | 32 | 896 | 12.5% |
| 22 | 1280 | 25 | 1024 | 20.0% |
| 23 | 1536 | 21 | 1280 | 16.7% |
| 24 | 1792 | 18 | 1536 | 14.3% |
| 25 | 2048 | 16 | 1792 | 12.5% |
| 26 | 2560 | 12 | 2048 | 20.0% |
| 27 | 3072 | 10 | 2560 | 16.7% |
| 28 | 3584 | 9 | 3072 | 14.3% |
| 29 | 4096 | 8 | 3584 | 12.5% |
| 30 | 5120 | 6 | 4096 | 20.0% |
| 31 | 6144 | 5 | 5120 | 16.7% |
| 32 | 8192 | 4 | 6144 | 25.0% |
| 33 | 10240 | 3 | 8192 | 20.0% |
| 34 | 12288 | 2 | 10240 | 16.7% |
| 35 | 16384 | 2 | 12288 | 25.0% |

上表里的“最坏内部碎片率”是按当前 size class 表的**相邻 class 理论上限**来估算的，口径是：

```text
(当前 class 大小 - 前一级 class 大小) / 当前 class 大小
```

它表达的是“某个请求刚好跨过前一级上界时，向上归并到当前 class 之后，最坏会浪费多少 slot 空间”。所以它是 **size-class 设计上限**，不是 workload 运行时的真实平均碎片率。按现有设计文档的经验口径，整体加权平均内部碎片通常在 **12% ~ 15%** 左右。

所以当前 allocator 的核心不是单一 bump allocator，也不是单一 bitmap allocator，而是：**TLS 前端先吃本地热缓存，中央池负责统一供货与回收，底层 region manager 只做容量管理。**

### 4.2 中央池的数据结构是怎么设计的

`CentralRegionPool` 当前是“每个 size class 一个 bucket，再加一个全局 empty cache”的结构。

每个 `SizeClassBucket` 里有四条链：

1. `partialSwept`：已经 sweep，且还有空槽，是最热的可分配来源。
2. `partialUnswept`：还有空槽，但还没 sweep；取出来前要先原地 lazy sweep。
3. `fullSwept`：已经 sweep，但没有空槽；本轮不能直接供分配，只是保留状态。
4. `fullUnswept`：没有空槽且还没 sweep；lazy sweep 后可能重新出现空槽。

除此之外，中央池还维护一个全局 `emptySweptCache_`：

1. 这里放的是“已经完全空掉、解绑了 size class 的 region shell”。
2. 它不是按 class 分桶，而是 classless 的全局缓存。
3. 后续任何 class 缺 region 时，都可以把 empty shell 重新 `InitForSizeClass(cls)` 后再拿来用。

并发协议上，当前实现做了两层区分：

1. `partialSwept` 用 `LockFreeRegionStack`，因为它是最热的取区路径，要求 `Pop/Push` 尽量无锁。
2. `fullSwept / partialUnswept / fullUnswept` 放在 bucket 锁下，因为它们会伴随分类调整和 lazy sweep 认领。

再往下还有一个关键协议字段：`globalSweepGen + region->sweepGen`。它们配合 `CasSweepGen(sg - 2, sg - 1)` 实现“谁拿到 unswept region，谁负责这一轮 sweep”的所有权认领，所以中央池本身不只是 list 容器，它还是 lazy sweep 的并发协调层。

### 4.3 小对象是怎么申请的

当前小对象申请链路可以按下面的真实代码路径来理解：

```text
用户请求 size
  -> RegionSpaceMS::TryAllocateOnceMS()
  -> AllocBufferMS::Allocate(size)
  -> cls = SizeClassInfo::SizeToClass(size)
  -> FastAlloc(cls)
     -> activeRegions_[cls] 命中则直接分配
     -> miss 则进入 SlowAlloc(cls)
  -> SlowAlloc(cls)
     -> 归还旧 active region
     -> 先尝试 cachedRegion_[cls]
     -> 再尝试 CentralRegionPool::GetRegion(cls)
     -> 还没有则 CentralRegionPool::GetFreshRegion(cls)
     -> 设成新的 active region
     -> 立即在新 region 上做一次实际分配
```

这里最重要的现实是：**mutator 并不会自己判断 region 该去 partial、full、empty 哪条链，也不会直接做堆增长。** 这些动作都被收敛进 `ReturnRegion()` 和 `CentralRegionPool::GetRegion()` 这一进一出两个门。

具体来说：

1. `ReturnRegion(cls)` 会根据 `IsCompletelyFree()` 和 `HasFreeSpace()` 把当前 active region 送回 `ReturnEmptyRegion`、`ReturnPartialRegion` 或 `ReturnFullRegion`。
2. `RefillFromCentral(cls)` 先调 `central_->GetRegion(cls)`，只有中央池没有 ready/可 sweep/可 rebind 的 region 时，才会退到 `GetFreshRegion(cls)`。
3. `GetFreshRegion(cls)` 再往下才真正调用 `MSRegionManager::TakeRegion(cls, *this)`，也就是把 fresh region 申请这一层也纳入中央池统一管理。

这说明当前 MSGC 的小对象分配不是“每次缺货直接 grow heap”，而是优先榨干：

1. 本地 active
2. 本地 backup cache
3. 中央池已就绪 region
4. 中央池可 lazy sweep region
5. 中央池 empty cache rebind
6. fresh region

### 4.4 快速路径到底是什么

如果按真正的热度来看，当前实现其实有三层快路径，而不止一层。

#### 4.4.1 第一层：`activeRegions_[cls]` 命中

这是最热的 mutator 本地路径，逻辑就在 `AllocBufferMS::FastAlloc(cls)`：

1. 先取 `activeRegions_[cls]`
2. 如果 region 是 `MS_REGION_BUMP`，就走 `AllocBump()`
3. 否则走 `AllocFromBitmapFast()`
4. 成功就直接返回地址，不碰中央池

这条路的关键价值是：**无锁、无中央池交互、无额外分类动作**。因此它就是当前小对象路径的真正 fast path。

#### 4.4.2 第二层：`cachedRegion_[cls]` 命中

当 active region 用完后，`SlowAlloc(cls)` 不会立刻进中央池，而是先看每个 class 自己的 `cachedRegion_[cls]`：

1. 命中时直接原子读出并清空缓存槽
2. 把它设成新的 `activeRegions_[cls]`
3. 然后立即继续分配

这条路虽然已经进入 slow path 函数，但本质上还是一个“本地 warm cache hit”，因为它避开了 bucket list 操作和 fresh region 申请。

#### 4.4.3 第三层：中央池的 `partialSwept`

即便进了中央池，也还有一条最热的 refill 路径：`GetRegion(cls)` 优先从 `partialSwept.Pop()` 取 region。

这条路之所以重要，是因为：

1. region 已经 sweep 完，可直接复用
2. region 已经按 class 绑定好 bitmap
3. `partialSwept` 本身是 CAS 栈，取区不需要拿大锁

所以从实现上看，当前 allocator 的设计目标并不是“所有路径都无锁”，而是把最常见的命中面压缩到：

1. mutator 本地 active region
2. mutator 本地 backup cache
3. central bucket 的 lock-free `partialSwept`

### 4.5 中央池是怎么给小对象供货的

`CentralRegionPool::GetRegion(cls)` 的真实优先级现在已经非常清楚：

```text
P1 partialSwept
P2 partialUnswept   -> lazy sweep 后返回
P3 fullUnswept      -> lazy sweep 后若扫出空位则返回
P4 emptySweptCache  -> 重新绑定 size class
P5 fresh region     -> 由调用方再走 GetFreshRegion
```

每一级对应的实现意图也不同：

1. `partialSwept` 代表“现成可用”，追求最短 refill 延迟。
2. `partialUnswept` 代表“最可能 sweep 后马上有空位”，优先级高于 full。
3. `fullUnswept` 代表“虽然当前满，但这一轮活对象未必填满整个 region”，sweep 后仍可能转成 partial。
4. `emptySweptCache` 代表“复用空壳 region，不必马上向底层申请新 region”。
5. fresh region 是最后兜底。

这里还要注意两个容易被设计稿写错的点：

1. `fullSwept` 确实存在，但它不在当前 GetRegion 热路径里，因为它已经 sweep 过且确认没有空槽，本轮不能直接供分配。
2. 当 bucket ready supply 太低、但 unswept backlog 还很多时，`GetRegion(cls)` 一开始就会调用 `msRegionManager_->MaybeNotifySweep(this, cls)`，主动通知后台 sweep 线程补货，而不是等 mutator 一路跌到 fresh region 才反应。

### 4.6 TLS buffer 与 cache 是怎么设计实现的

`AllocBufferMS` 当前就是 MSGC 的 TLS 前端缓存层，它的设计要点有四个。

1. 一个线程或协程 Processor 对应一个 `AllocBufferMS`。
2. 每个 `AllocBufferMS` 里有 `activeRegions_[kNumSizeClasses]`，也就是每个 size class 一个当前活跃 region。
3. 每个 `AllocBufferMS` 里还有 `cachedRegion_[kNumSizeClasses]`，也就是每个 size class 一个原子备用 region 槽。
4. 每个 `AllocBufferMS` 还维护 `cacheMiss_[kNumSizeClasses]`，用来告诉后台线程哪些 class 最近“经常缺货”。

这套 TLS buffer/cache 当前是这样协作的：

1. mutator 正常只用 `activeRegions_[cls]`。
2. active 用完后，先尝试 `cachedRegion_[cls]`。
3. 如果 `cachedRegion_[cls]` 也空，就累加 `cacheMiss_[cls]`。
4. 当 miss 达到 `kCacheFillThreshold = 2`，会通知后台 `NotifyToFeedAllocBuffers()`。
5. gc-helper 随后通过 `CentralRegionPool::BackgroundFillAllocCaches()` 遍历所有已注册的 `AllocBufferMS`，调用它们的 `BackgroundFillCache()`。
6. `BackgroundFillCache()` 再从 `central_->GetRegion(cls)` 取一个 region，并用 CAS 填进 `cachedRegion_[cls]`。

除此之外，中央池还维护了一个 `msAllocBuffers_` 注册表：

1. `AllocBufferMS::Init()` 会向中央池注册自己。
2. `Fini()` 会反注册。
3. STW 时 `FlushAllAllocBuffers()` 会把所有 buffer 的 active region 和 cached region 统一归还给中央池。
4. 后台填 cache 时也靠这个注册表遍历所有线程本地 buffer。

最后还有一个兼容性细节值得保留：`AllocBufferMS` 的偏移 0 仍然是 `tlRegion_`，并初始化成 `RegionInfo::NullRegion()`。这不是因为 MSGC 还在真的依赖旧 fast path，而是为了让历史上已经带 LLVM fast-path 的预编译代码安全 fail-open 到当前运行时慢路径。

### 4.7 分配路径已经和 GC 协作接上了

虽然这一节聚焦中央池，但当前小对象路径已经不是“只管拿内存”的 allocator 了。

`AllocBufferMS::Allocate()` 在 marking phase 下还会做两件额外的事：

1. 调用 `MarkNewObject()`，让新对象按当前实现直接走 alloc-black。
2. 按分配字节数累积 `gcAssistBytes_`，超过 64KB 且 collector 仍有待做工作时，触发 `MSMarkAssist()`。

所以中央池和 TLS buffer 负责的是“小对象供货面”，但 mutator 真正走过的整条路径已经把 Step1 分配、Step2 标记和后台补货联在了一起。

## 5. 大对象的分配与释放路径

### 5.1 入口分流：当前 `>= 16KB` 直接进入 large path

当前大对象并不是先进入中央池，再由中央池决定是否拆出大 span；而是在 `RegionSpaceMS::TryAllocateOnceMS()` 里直接分流：

```text
allocSize >= kLargeObjectThreshold(16KB)
  -> unitCount = ceil(allocSize / MRT_PAGE_SIZE)
  -> regionBytes = round_up(unitCount * pageSize, MS_REGION_SIZE)
  -> MSRegionManager::TakeRegionForLarge(unitCount, regionStart)
  -> RegisterLargeObjectMS(addr, regionBytes)
```

如果 `TakeRegionForLarge()` 失败，当前实现会直接把这次 large allocation 判成失败：

1. `TryAllocateOnceMS()` 不会为 large path 主动请求 GC，也不会等待当前 GC 结束。
2. `RegionSpaceMS::Allocate()` 对 large path 不再走小对象那套通用重试骨架。
3. 非 GC 线程会立刻抛 OOM；GC 线程仍维持返回 `0` 的旧约定。

所以大对象路径虽然不经过中央池四-list，现在也不再复用 allocator 的“失败触发 GC / 等 GC / 再试一次”骨架；它的语义已经收敛为单次尝试、失败直接 OOM。

### 5.2 `TakeRegionForLarge` 是怎么拿到连续 span 的

`MSRegionManager::TakeRegionForLarge(unitCount, regionStart)` 本身不直接碰 `MSRegionInfo` 状态机，而是先把“页数请求”换算成“region span 请求”：

1. 先用 `regionCount = ceil(unitCount / unitsPerMSRegion_)` 把 unit 数换算成 region 数。
2. 然后统一走 `TakeRegions(regionCount, regionIdx, &source)`。
3. 成功后把 `regionStart = RegionIdxToAddr(regionIdx)`。
4. 再调用 `MaybeNotifyReclaim()` 和 `MaybeNotifyMark(LARGE_ALLOC, ...)`，把大对象分配压力反馈给回收和 GC 触发逻辑。
5. 最后只返回 `&array_[regionIdx]`，也就是这段 large span 的 base entry。

和小对象 `TakeRegion()` 相比，这里有两个重要差异：

1. 它一次取的是多个连续 region，而不是单个 32KB region。
2. 它不做 size class 初始化，也不经过中央池的 empty cache / partialSwept / lazy sweep 体系。

### 5.3 `RegisterLargeObjectMS` 是怎么写元数据的

真正把这段连续 span 变成“大对象”的动作，不在 `TakeRegionForLarge()` 内，而是在 `RegionSpaceMS::RegisterLargeObjectMS()` 里完成。

这一步当前做了四件事：

1. 根据 `addr` 找到 base entry，并计算这次 large span 覆盖了多少个 region。
2. 先校验这段 span 覆盖到的所有 `MSRegionInfo` entry 没有脏状态。
3. 对 base entry 调 `InitForLargeObject(start, end)`，建立 `allocPtr/regionEnd` 等主状态。
4. 把 `reserved` 写成 `baseIdx`，让后续所有 entry 都能回指 base entry。

然后对后续 span entry，当前代码的实际做法是：

1. `regionType = MS_REGION_LARGE`
2. `markBitmap = nullptr`
3. `allocBitmap = nullptr`
4. `totalSlotCount = 0`
5. `freeSlotCount = 0`
6. `reserved = baseIdx`

这里最容易和旧设计稿搞混的一点是：**当前 large span 的 secondary entry 也继续标成 `LARGE`，只靠 `regionEnd == 0` 和 `reserved == baseIdx` 来表达“我是 large span 的从属 entry”。**

最后，当前实现还会把新分配的大对象直接按 alloc-black 处理：

1. `baseInfo->isMarked = 1`
2. `baseInfo->isScanned = 1`
3. 若中央池存在，则把 base entry 注册进 `largeRegionList_`
4. 同时把 `regionBytes` 加到 `totalMSRegionBytes_`

### 5.4 大对象是怎么被标记和保活的

小对象依赖 `markBitmap + scannedBitmap`，大对象则不用 bitmap，而是把标记态集中放在 base entry 上。

当前生命周期是这样的：

1. 新一轮开始前，`CentralRegionPool::ClearAllMarkBitmaps()` 会遍历 committed 区，把所有 `MS_REGION_LARGE` entry 的 `isMarked/isScanned` 清零。
2. 之后，如果 GC 发现某个 large 对象，`MSRegionInfoArray::MarkObjectInHeap(addr)` 会先经过 `GetRegionInfoValidated(addr)`。
3. 如果命中的是 large span 的从属 entry，则会通过 `reserved` 回到 base entry。
4. 真正的存活位只记在 base entry 的 `isMarked` 上；是否已经扫描完成，则记在 base entry 的 `isScanned` 上。

所以 large path 的真实语义是：**span entry 只是“地址路由元数据”，大对象真正的标记状态都挂在 base entry 上。**

### 5.5 释放路径：当前是 eager reclaim，不进 empty cache

大对象的释放不是在 mutator 分配时 lazy sweep 出来的，而是在标记完成后集中做 eager reclaim。

当前链路是：

```text
MarkSweepCollector::CollectLargeGarbage()
  -> CentralRegionPool::SweepLargeRegions()
  -> MSRegionManager::ReclaimRegionLarge(regionStart, regionBytes)
  -> FreeManager::ReclaimRegionsReleased(regionIdx, regionCount)
```

`SweepLargeRegions()` 当前会：

1. 在 `largeLock_` 下遍历 `largeRegionList_`。
2. 把 `isMarked == 0` 的 large base entry 从链表里摘出来。
3. 对每个死亡 large object，先把后续 span entry 全部 `Reset()`。
4. 再把 base entry 自己 `Reset()`。
5. 最后调用 `ReclaimRegionLarge(regionStart, regionBytes)`，把整个 span 一次性还给 releasedTree。

这意味着大对象当前不会进入：

1. `partialSwept`
2. `emptySweptCache`
3. 小对象 lazy sweep 再利用路径

它的口径就是：**死了就整段 eager 释放回 releasedTree。**


## 6. 堆增长与收缩

### 6.1 当前底层供给面：`TakeRegions` 才是真正的统一入口

不管上层是小对象 `TakeRegion()` 还是大对象 `TakeRegionForLarge()`，最终都会落到 `MSRegionManager::TakeRegions(num, idx, source)`。

当前 `TakeRegions()` 的真实顺序很稳定：

```text
1. 先尝试 releasedTree
2. releasedTree 不够时，再尝试 inactive zone bump
3. bump 不够时，GrowOneSegment()
4. grow 成功后回到 bump 路径继续试
```

也就是说，底层供给层不是“先 grow，再看能不能复用”，而是严格的：**released-first -> bump -> grow**。

其中 releasedTree 路径还做了一个细节分流：

1. `TryTakeRegions()` 成功，则 `source = RELEASED`。
2. 如果返回 `EMPTY`，说明 releasedTree 确实没货了，才会停止尝试。
3. 若是竞争性失败，则进入 saferegion 后继续重试。

### 6.2 `TakeRegion`：小对象如何拿到一个 32KB region

小对象用的 `MSRegionManager::TakeRegion(cls, centralPool)` 本质上只是 `TakeRegions(1, ...)` 的包装，但包装里补了 allocator 真正关心的附加协议。

当前流程是：

1. 先调用 `TakeRegions(1, regionIdx, &source)` 取一个 32KB region。
2. 如果这次来源是 `BUMP`，并且打开了 inline reclaim，就立即 `InlineReclaimForBalance(centralPool)`，同步释放一个 empty-cache region，尽量把 committed bytes 压平。
3. 之后调用 `MaybeNotifySweep(&centralPool, cls)`、`MaybeNotifyReclaim()`、`MaybeNotifyMark(SMALL_ALLOC, ...)`。
4. 最后只返回 `&array_[regionIdx]` 这个仍处于 `FREE` 态的 entry。

也就是说，`TakeRegion()` 并不负责做 `InitForSizeClass(cls)`；真正把这块 fresh region 变成 BUMP region 的动作，还在 `CentralRegionPool::GetFreshRegion()` 里。

### 6.3 `TakeRegionForLarge`：大对象如何把请求转换成连续 span

`TakeRegionForLarge()` 和 `TakeRegion()` 共用同一套底层供给逻辑，但语义不同：

1. `TakeRegion()` 固定取 1 个 region，服务 size class。
2. `TakeRegionForLarge()` 先把页数换成 region 数，再一次取整段连续 span。

所以它的关键实现点不是“多了一套 allocator”，而是：

1. `unitCount -> regionCount` 的上取整换算。
2. 仍然复用 `TakeRegions(regionCount, ...)`。
3. 不参与中央池 bucket 分类。
4. 只返回 base entry 和 regionStart，后续状态初始化交给 `RegisterLargeObjectMS()`。

### 6.4 `GrowOneSegment` 是怎么增长 committed heap 的

当前堆增长不是重新 mmap 一块新堆，而是在初始化时的大虚拟保留区里，逐 segment 扩张 committed 窗口。

`MSRegionManager::GrowOneSegment()` 自己只是转调 `freeManager_.GrowOneSegment(*this)`；真正逻辑在 `FreeManager::GrowOneSegment()` 里：

1. 先拿 `growMutex_`，避免并发 grow/shrink 互撞。
2. 如果 `committedEnd_ >= maxHeapEnd_`，说明已经到虚拟上限，增长失败。
3. 正常情况下按 `segmentSize_` 增长；如果尾部剩余不够一个完整 segment，就截断到剩余字节数。
4. 计算这次新增了多少 `growRegions` 和 `growUnits`。
5. 调 `handler.InitializeFreshRegions(oldRegionCount, growRegions)`，把新 committed 这一段对应的 `MSRegionInfo` entry 初始化出来。
6. 更新 `committedRegionCount_`、`committedUnitCount_`、`committedEnd_`。
7. 最后调用 `handler.OnCommittedHeapExtended(committedEnd_)`，把“堆顶已经前移”的事实同步出去。

所以当前 grow 的本质是：**大虚拟保留区不变，只把 committed 窗口向后推，并补齐新增那一段 region metadata。**

### 6.5 `ShrinkHeap` 是怎么收缩 committed heap 的

堆收缩的上层入口在 `RegionSpaceMS::ReclaimGarbageMemory(releaseAll)`：

1. 先让中央池执行 `HandleBackgroundScavenge(releaseAll)`，把 empty cache 里可回收的 region 释放回 releasedTree。
2. 然后调用 `msRegionManager_->ShrinkHeap()`，尝试把 committed heap 的尾部往回收。

`MSRegionManager::ShrinkHeap()` 自己也只是一个包装：

1. 先记住收缩前的 `committedRegionCount`。
2. 调 `freeManager_.ShrinkHeap()` 实际收缩 committed window。
3. 如果 committed region 数真的变少了，再调用 `MSBitmapArena::ShrinkToCommittedRegionCount(newCount)`，让 sidecar bitmap 也同步跟着缩。

而 `FreeManager::ShrinkHeap()` 当前的真实策略是：

1. 先在锁内执行 `RewindInactiveZoneUsingReleasedTail()`，尽量把尾部已经 release 掉的 span 重新并回 inactive zone。
2. 只允许按完整 `segmentSize_` 为单位收缩。
3. 只收缩尾部连续的 committed 区，不会在中间打洞。
4. 至少保留一个 segment，不会把 committed heap 缩到 0。

所以当前 shrink 的本质不是“任意释放空洞”，而是：**先把尾部 released span 回卷，再把 committed window 做 tail-only 收缩。**

### 6.6 当前实现里，grow 和 reclaim 是怎么联动的

最后把这一节收成一句话：

1. `TakeRegion()` / `TakeRegionForLarge()` 负责提出 region 或 span 请求。
2. `TakeRegions()` 统一把请求落到 releasedTree、inactive zone 和 grow 路径。
3. `HandleBackgroundScavenge()` + `ShrinkHeap()` 再把已经空出来的尾部 committed 区收回去。

因此当前 MSGC 的容量管理已经形成闭环：**分配时先复用 released，再 bump，再 grow；回收时先把 empty cache 释放回 released，再做 tail shrink。**

## 7. 分配器与 GC 的耦合点

虽然 Step1 是 allocator，但当前实现已经和 GC 强绑定，主要体现在：

1. `AllocBufferMS` 在 marking phase 调 `MarkNewObject()`
2. `CentralRegionPool::PrepareForSweep()` 由 GC 周期驱动
3. `PreEnumRoots()` 在下一轮 MarkSetup 前负责 stop sweep / clear bitmap / reclassify unswept
4. `RegionSpaceMS::ReclaimGarbageMemory()` 实际委托给 `CentralRegionPool::HandleBackgroundScavenge()` 和 `MSRegionManager::ShrinkHeap()`
5. `MSRegionManager::MaybeNotifySweep/MaybeNotifyMark/MaybeNotifyReclaim` 让 allocator 侧的供给压力能反向触发 GC 背景动作

这意味着当前 Step1 实现已经不是“单纯把对象分出去”的模块，而是 **分配、复用、回收、RSS 管理、HEU 触发** 的统一控制面。

当前 Step1 allocator 的本质可以概括为：

**上层由 `RegionSpaceMS` 统一分流，小对象通过 `AllocBufferMS + CentralRegionPool` 做“线程本地热缓存 + 中央池 lazy sweep + empty cache 复用”，底层由 `MSRegionManager + MSFreeManager + MSBitmapArena` 提供“虚拟大预留、released/bump 供给、bitmap sidecar 和 large region 管理”，并且整个过程已经和 GC 的 mark-at-alloc、background sweep、reclaim 水位控制紧密耦合。**

## 8. 当前还能继续优化的点

当前实现已经能稳定跑通 `gc_benchmark` 的功能项，最近一次结果里 `F1~F5` 全部通过，`F3_LargeObject` 也已经 PASS；但如果从 allocator 视角继续往前看，Step1 仍然有几块值得优先优化的区域。

### 8.1 large path 仍然是最值得继续压的热点

当前 `>= 16KB` 的对象会直接离开 size-class 小对象路径，统一走：

```text
TryAllocateOnceMS
  -> TakeRegionForLarge
  -> RegisterLargeObjectMS
  -> 标记期按 alloc-black 建立初始状态
  -> GC 后走 eager reclaim
```

这条路的优点是语义简单、元数据口径统一，但对“反复创建 48KB / 64KB 一类对象”的 workload 来说，当前开销仍然偏重：

1. 每次分配都要做 `unitCount -> regionCount` 的换算和连续 span 获取。
2. 成功路径会额外带上 `MaybeNotifyReclaim()` 和 `MaybeNotifyMark(LARGE_ALLOC, ...)` 的联动开销。
3. 回收侧采用 eager reclaim，短命大对象会反复经历“建 span -> 标记清零 -> 整段回收”的完整周期。

所以 large path 后续最值得尝试的优化方向是：

1. 给 `16KB ~ 128KB` 这一类“中等偏大对象”补一层更轻量的复用策略，而不是所有对象都走一次完整 large-span 生命周期。
2. 把 `MaybeNotifyReclaim/MaybeNotifyMark` 从“几乎按次触发”收敛成更批量化的压力反馈，避免 burst large alloc 把通知面放大。
3. 在不改动“失败直接 OOM”语义的前提下，只优化 large path 的成功路径，不再把复杂度重新引回分配失败分支。

### 8.2 小对象路径的主要空间在“补货更平滑”，不是改 GetRegion 顺序

当前小对象热路径已经比较清晰：`activeRegions_[cls] -> cachedRegion_[cls] -> partialSwept -> lazy sweep -> empty cache -> fresh region`。

从已有调参和 benchmark 结果看，真正敏感的不是“有没有 cache”，而是**缺货后补货和回收的节奏够不够平滑**：

1. `ready watermark`、`empty cache floor/cap`、`pressure sweep slice` 对 RSS 和 active-region 波动都非常敏感。
2. `cacheMiss_[cls] + BackgroundFillAllocCaches()` 这条链路已经存在，但现在还是偏静态阈值驱动。
3. 把 `emptySweptCache` 提前到 lazy sweep 之前这个方向，实验上已经证明会明显恶化 RSS/active-region，不应该再当成优化目标。

因此这里更合理的优化点是：

1. 把补货策略做成按 class 的动态水位，而不是继续依赖统一常量阈值。
2. 让 `BackgroundFillCache()` 更偏向热点 class，减少冷门 class 抢 fresh region。
3. 继续坚持“partial/unswept 优先于 empty cache”的主顺序，只优化每一级内部的命中率和批量度。

### 8.3 reclaim / shrink 现在能工作，但目标仍然偏静态

当前第三层回收链路已经形成闭环：`HandleBackgroundScavenge()` 负责把 empty cache 释放回 releasedTree，`ShrinkHeap()` 负责回卷尾部 committed window。

这套机制已经能把系统跑通，但从已有参数实验看，还存在一个结构性问题：**回收目标更多是围绕当前阈值和水位在调，而不是直接围绕 true live / committed slack 在调。**

后续最值得继续优化的地方是：

1. 让 empty cache 保留量、单次 scavenging budget、tail shrink 触发条件更多参考 true live 和 committed slack，而不是只看固定阈值。
2. 让 reclaim 的“快/慢”模式跟当前 unswept backlog、ready supply 和 allocation slope 绑定，而不是只看单一压力点。
3. 继续保留现在这种分段 shrink 和 released-first 设计，不要再回到更激进的 shrink/reclaim 时序，之前的实验已经证明那类改法容易引入稳定性回归。

### 8.4 mutator 侧的分配扰动还可以继续收敛

当前 Step1 allocator 已经和 GC 深度耦合，小对象路径会在 marking phase 下走 `MarkNewObject()`，缺货时又可能顺手触发 lazy sweep、assist 和 reclaim 通知。

这类设计有助于把 GC 成本摊平，但副作用也很明确：**mutator 的分配慢路径容易被放大成“分配 + sweep + assist + 通知”的组合延迟。**

因此后续可以继续优化：

1. 把 `MaybeNotifySweep/MaybeNotifyMark/MaybeNotifyReclaim` 的触发做得更去抖，避免短时间内重复放大背景动作。
2. 让 assist debt、cache miss 和 bucket backlog 的关系更线性，减少某几个时刻的突刺式慢路径。
3. 尽量把真正重的工作继续留在后台线程，mutator 侧只保留必要的压力信号和少量快速认领逻辑。

### 8.5 还需要继续加强 allocator 自己的可观测性

现在已经有不少 GC/region 统计，但如果目标是继续压吞吐尾延迟和 RSS，allocator 自身还需要更细的路径级计数。

最有价值的新增统计大概有几类：

1. 小对象实际命中分布：`active/cached/partialSwept/partialUnswept/fullUnswept/emptyCache/fresh` 各自命中次数和字节数。
2. large path churn：large alloc 次数、平均 regionCount、eager reclaim 次数、large span 平均寿命。
3. grow/shrink 触发原因：到底是 releasedTree 见底、inactive zone 不足，还是 empty cache 过多、tail released 足够导致的回收。

这些数据的价值不在“多看几行日志”，而在于能把后续优化从“凭感觉调参数”变成“按路径占比直接找瓶颈”。

### 8.6 当前已经验证过的不优先方向

为了避免后面重复踩坑，把目前已经有结论的几条方向也记一下：

1. 不要把 `empty cache` 直接提前到 lazy sweep 之前。现有实验已经表明这会明显放大 active-region 和 RSS。
2. 不要简单通过禁用 releasedTree 复用来追求更低 RSS。这样通常会换来更大的 committed 扩张和更频繁的 GC。
3. 不要只靠把 `cjGCInterval` 拉长来“优化性能”。这类改法很多时候只是用更少 GC 次数换更高的 active-region / RSS，并没有真正改善 allocator 本身。

如果把优先级压成一句话，当前 Step1 后续最值得优先做的仍然是：**先压 large path 的 churn 和通知放大，再把小对象补货/回收水位做得更动态、更平滑，最后补齐 allocator 级 observability，让后续调优有更稳定的数据抓手。**
