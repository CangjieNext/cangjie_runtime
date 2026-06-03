# Step 10：Bitmap 现状与优化策略

> 目标：系统梳理 MSGC 中 `MSRegionInfo` bitmap 的当前实现、生命周期、性能语义与风险点，
> 在不破坏 Step 8 三色语义和 Step 9 empty cache 语义的前提下，给出可渐进落地的优化策略。

## 目录

1. [背景与目标](#1-背景与目标)
2. [当前实现总览](#2-当前实现总览)
3. [Bitmap 生命周期](#3-bitmap-生命周期)
4. [当前实现的收益与代价](#4-当前实现的收益与代价)
5. [当前问题与优化动机](#5-当前问题与优化动机)
6. [优化约束与设计原则](#6-优化约束与设计原则)
7. [方案 A：Bitmap Block Pool](#7-方案-a-bitmap-block-pool)
8. [方案 B：同 class 热复用扩展](#8-方案-b-同-class-热复用扩展)
9. [方案 C：清零与统计路径优化](#9-方案-c-清零与统计路径优化)
10. [推荐实施顺序](#10-推荐实施顺序)
11. [风险与验证](#11-风险与验证)
12. [与 Step 8 / Step 9 的关系](#12-与-step-8--step-9-的关系)

## 1. 背景与目标

在 Step 8 中，MSGC 已经把小对象 Region 的状态表达扩展为：

- `allocBitmap`：slot 是否可分配
- `markBitmap`：对象是否已被当前 GC 发现
- `scannedBitmap`：对象 children 是否已 trace 完

在 Step 9 中，又进一步引入了 `emptySweptCache`，把“完全空的小 Region”从 `MSRegionManager`
的旧 free-tree 语义中分离出来，转由 `CentralRegionPool` 统一管理。

这两步叠加之后，bitmap 已经不再只是一个局部实现细节，而是同时参与了：

1. 小对象分配状态表达
2. 并发标记三色状态表达
3. sweep 后可复用空间表达
4. empty cache 的 class 解绑与重新绑定

当前代码已经能正确工作，但 bitmap 这一层还存在几个明显问题：

- 生命周期分散在 `MSRegionInfo`、`CentralRegionPool`、`AllocBufferMS` 多处路径上
- “同 class 直接复用”和“empty cache 释放后重建”两套语义并存，容易混淆
- bitmap 本身内存很小，但当前分配/释放方式仍然以 `mmap/munmap` 为主，系统调用成本偏高
- 若后续继续做 reclaim / empty cache / region 热复用优化，不先把 bitmap 语义讲清楚，容易引入行为回退

因此 Step 10 的目标不是再定义一套新 bitmap 语义，而是：

1. 先把当前实现状态讲清楚
2. 明确哪些现状应该保留，哪些值得优化
3. 给出一条不破坏 Step 8 和 Step 9 语义边界的渐进式优化路径

## 2. 当前实现总览

### 2.1 参与对象与字段布局

当前 MSGC 中，bitmap 只服务于**小/中对象 region**；`MS_REGION_LARGE` 不使用这套 bitmap。

`MSRegionInfo` 当前只保留两个指针字段：

- `allocBitmap`
- `markBitmap`

`scannedBitmap` 不单独保存指针，而是通过 `markBitmap + bitmapWords` 推导。这样 `MSRegionInfo` 不需要再为第三个 bitmap 增加额外指针字段。

### 2.2 当前真实存储来源：`MSBitmapArena` sidecar

当前实现已经不是“每个 region 独立 `mmap/munmap` 一块 bitmap”。真实模型是：

1. `MSBitmapArena::Init(maxRegionCount, committedRegionCount)` 预留一整片 sidecar arena。
2. 每个 region index 都有一个**固定槽位**，槽位大小为 `kBitmapSlotBytes`。
3. `MSRegionInfo::InitForSizeClass(cls)` 通过：

```text
MSBitmapArena::GetSlotBase(regionIdx)
```

取得当前 region 的固定 slot base。

4. 槽位内部布局仍然是：

```text
[ allocBitmap | markBitmap | scannedBitmap ]
```

因此，“三段 bitmap 连续布局”仍然成立，但它现在绑定在 **固定 sidecar slot** 上，而不是临时分配的一小块独立映射。

### 2.3 当前内存生命周期

当前 sidecar arena 的生命周期由 `MSRegionManager` 驱动：

1. 堆增长时，`MSRegionManager::InitializeFreshRegions()` 调用 `MSBitmapArena::EnsureRegionRangeCommitted()` 激活对应 chunk。
2. 堆收缩时，`MSRegionManager::ShrinkHeap()` 调用 `MSBitmapArena::ShrinkToCommittedRegionCount()` 回收尾部 sidecar chunk。
3. `MSRegionManager::~MSRegionManager()` 在析构时调用 `MSBitmapArena::Cleanup()`。

也就是说，bitmap 的物理保留/回收粒度现在是 **arena chunk**，而不是单 region 的 `mmap/munmap`。

### 2.4 当前语义分工

三个 bitmap 的职责如下：

| 结构 | 语义 | 主要写入方 | 主要读取方 |
|------|------|-----------|-----------|
| `allocBitmap` | `1=free`, `0=allocated` | `InitForSizeClass()`、`ResetToBump()`、`SweepRegion()`、`AllocFromBitmapFast()` | 分配器 |
| `markBitmap` | `1=alive/discovered`, `0=dead/unmarked` | `MarkObjectInHeap()` | GC 标记、`ComputeMarkedBytes()` |
| `scannedBitmap` | `1=children traced` | `SetScannedBit()` | 并发 drain / tri-color 判定 |

当前实现中：

- BUMP 分配期主要不读 bitmap。
- sweep 会重建 `allocBitmap`。
- 下一轮 `MarkSetup` 的 `ClearAllMarkBitmaps()` 再统一清 `markBitmap + scannedBitmap`。

### 2.5 当前成本画像

bitmap 本体依然很小，问题已经不再是“单块 bitmap 太小导致单独 `mmap` 不划算”，因为当前代码已经切换到 sidecar arena。现在更值得关注的是：

1. sidecar slot 与 region index 强绑定后，逻辑上更稳定，但 chunk 级保留会带来新的常驻成本权衡；
2. `ResetToEmptyCache()` / `InitForSizeClass()` 只是逻辑解绑与重新初始化，不再伴随物理映射的即时释放；
3. 因此需要把“逻辑释放 bitmap”与“物理回收 sidecar chunk”严格区分。

## 3. Bitmap 生命周期

### 3.1 创建路径

当前只有两类路径会真正**绑定并初始化** bitmap 指针：

#### 路径 A：新 region 首次进入某个 class

```text
GetFreshRegion()
	-> MSRegionManager::TakeRegion()
	-> MSRegionInfo::InitForSizeClass(cls)
```

这条路径用于 brand-new region，bitmap 一定是新分配的。
这条“新分配”现在指的是：绑定到当前 region index 对应的 sidecar slot，并初始化内容，而不是重新 `mmap` 一块独立 bitmap block。

#### 路径 B：empty cache region 重新绑定 class

```text
TakeEmptySweptRegion(cls)
	-> InitForSizeClass(cls)
```

或者在 lazy sweep 路径中：

```text
GetRegion()
	-> RebindEmptyRegionForClass()
	-> ResetToEmptyCache()
	-> InitForSizeClass(cls)
```

这条路径用于“逻辑上仍在中央池，但已经脱离旧 class 的 region 壳”。当前重新绑定时会再次调用 `InitForSizeClass(cls)`，把同一 sidecar slot 重新初始化为目标 class 所需布局。

### 3.2 使用路径

#### 3.2.1 分配阶段

分配阶段分为两段：

1. `BUMP` 阶段：走 `allocPtr`，几乎不读 bitmap
2. `SWEPT/PARTIAL` 阶段：走 `AllocFromBitmapFast()`，此时 `allocBitmap` 成为分配真相来源

`AllocFromBitmapFast()` 还额外依赖 `reserved` 中缓存的 summary bit，先定位哪个 word 还有 free slot，
再进入 `allocBitmap[wordIdx]` 取最低位 free bit。

#### 3.2.2 标记阶段

小对象的并发标记走：

```text
MarkObjectInHeap(addr)
	-> TestAndSetMarkBit(addr)
```

并发 drain 完成对象 children 扫描后，再走：

```text
SetScannedObjectInHeap(addr)
	-> SetScannedBit(addr)
```

此外，`ComputeMarkedBytes()` 会在 marking 完成后、sweep 开始前遍历所有 `markBitmap` 统计 true live bytes。

#### 3.2.3 sweep 阶段

`SweepRegion()` 是 `allocBitmap` 的重建点：

```text
allocBitmap = ~markBitmap
```

同时它会：

- 统计 `liveSlotCount`
- 更新 `freeSlotCount`
- 重建 `reserved` summary word
- 把 dead slot 内容清零，避免后续 bitmap 复用时 tracing 到脏引用

当前实现已经不在 `SweepRegion()` 内清 `markBitmap`，而是推迟到下一轮 `MarkSetup` 的
`ClearAllMarkBitmaps()` 中统一清 `markBitmap + scannedBitmap`。

### 3.3 复用路径

当前有两类复用方式，但两者都共享同一个 sidecar slot 现实：

#### 路径 A：same-class 直接复用

```text
lazy sweep / central get
	-> region 完全空
	-> region->sizeClass == cls
	-> ResetToBump()
```

这条路径会**保留当前 slot 绑定关系并重置内容**：

- `allocBitmap` 重置为 all-free
- `markBitmap` 清零
- `scannedBitmap` 清零

这是当前最热的复用路径。

#### 路径 B：empty cache 语义复用

```text
PushEmptySweptRegion(region)
	-> ResetToEmptyCache()
	-> emptySweptCache_
	-> TakeEmptySweptRegion(cls)
	-> InitForSizeClass(cls)
```

这里需要特别更正文档口径：

1. `ResetToEmptyCache()` 会调用 `ReleaseBitmaps()`。
2. 但当前 `ReleaseBitmaps()` **只把 `allocBitmap/markBitmap` 指针置空**。
3. sidecar arena 中对应 slot 的物理存储并不会在这里立即 `munmap`。
4. 重新绑定时，`InitForSizeClass(cls)` 仍然会回到该 region index 的固定 slot 上重新初始化。

所以当前 empty cache 的真实语义是：

> 逻辑上解除 class 与 bitmap 指针绑定，物理上保留 sidecar slot，直到 arena chunk 缩容或 cleanup 时才真正回收。

### 3.4 释放路径

当前 bitmap 的释放主要发生在三处：

1. `ResetToEmptyCache()`
2. `Reset()`
3. `ReleaseRegionInternal()`

其中 `ReleaseRegionInternal()` 目前是：

```text
ReleaseBitmaps()
Reset()
```

而 `Reset()` 内部又会调用一次 `ReleaseBitmaps()`。因为第二次会发现 `allocBitmap == nullptr`，
所以不会二次释放，但这个路径仍然存在职责重复。

### 3.5 进程退出 / allocator 销毁阶段

这里有一个容易忽略的事实：

- `MSRegionManager::~MSRegionManager()` 不会遍历所有 region 释放 bitmap
- `MSRegionInfoArray::Cleanup()` 只处理镜像数组本身
- `RegionSpaceMS::~RegionSpaceMS()` 主要销毁总 heap `MemMap`

也就是说，运行期 bitmap 的回收闭环主要依赖 region 自己先走 `Reset()` / `ResetToEmptyCache()` /
`ReleaseRegionInternal()`。如果某些 region 在 runtime teardown 时仍然持有独立 bitmap block，
当前实现更接近“依赖进程级 OS 回收”而不是“显式遍历释放”。

## 4. 当前实现的收益与代价

### 4.1 当前实现已经获得的收益

#### 收益 1：没有把第三个 bitmap 指针塞进 `MSRegionInfo`

`scannedBitmap` 采用推导值而不是显式字段，使 `MSRegionInfo` 继续保持 64B cache line 约束。

#### 收益 2：bitmap 生命周期仍然是“一块内存”

不管是创建、复用还是释放，对外都只管理一块 block，避免了三段 bitmap 分别管理的复杂度。

#### 收益 3：same-class 快速复用已经存在

`ResetToBump()` 路径已经证明：对完全空 region，没有必要每次都释放再重建 bitmap。

#### 收益 4：empty cache 语义是干净的

进入 `emptySweptCache` 时释放旧 bitmap 并解绑旧 class，使 empty cache 真正成为“无 class 空壳”，
而不是“伪装成空 region 的旧 class region”。

### 4.2 当前实现的主要代价

#### 代价 1：bitmap block 很小，但系统调用和页粒度都很重

多数 class 的 bitmap block 只有几十到几百字节，最极端的小 class 也只有 1.5KB，
但当前仍以 `mmap/munmap` 或 `VirtualAlloc/VirtualFree` 管理它们。

在 macOS arm64 的 16KB 页上，这意味着一个 24B ~ 1536B 的 bitmap block 如果独立映射，
单块页利用率只有约 0.15% ~ 9.38%。所以这里的真实成本至少包含两层：

1. 频繁 `mmap/munmap` 带来的系统调用抖动
2. 一页只承载一块小 bitmap 带来的内部碎片和潜在 RSS 驻留

#### 代价 2：cross-class empty cache 复用必然重建 bitmap

这符合 Step 9 语义，但会把 empty cache 作为常态热点时的系统调用成本放大出来。

#### 代价 3：bitmap 相关清理触点较多

目前可能触碰 bitmap 内容的路径包括：

- `InitForSizeClass()`
- `ResetToBump()`
- `SweepRegion()`
- `ClearAllMarkBitmaps()`
- `ReleaseBitmaps()`

如果后续继续加 reclaim / cache / scavenger 逻辑，而没有把 ownership 统一，会更难维护。

#### 代价 4：部分职责边界仍有重复

最明显的是 `ReleaseRegionInternal()` 与 `Reset()` 都在触碰 bitmap 释放职责。

## 5. 当前问题与优化动机

综合当前代码，bitmap 相关问题的优先级可以这样排序：

### 问题 1：系统调用抖动和页级内部碎片都高于 bitmap 本身内存成本

这是最值得优先优化的点。

当前主要矛盾不是“bitmap 太大”，而是“bitmap 太小却频繁走 `mmap/munmap`”，并且在 16KB 页平台上，
哪怕只为几十字节的 bitmap 建一次独立映射，也会引入整页粒度的浪费。

第一版 bitmap pool 的对比结果也说明：

- `mmap/munmap` 次数可以小幅下降
- 但如果缓存策略过于保守，RSS 峰值、RSS 尾值甚至总耗时不一定同步变好

也就是说，“减少系统调用”本身还不够，必须把“减少页驻留浪费”一起纳入优化目标。

### 问题 2：empty cache 与 same-class 热复用之间存在两种复用语义

当前代码语义本身没错，但后续演进时很容易误把：

- same-class 快速复用
- empty cache 脱 class 复用

混成一种逻辑，导致 Step 9 语义被破坏。

### 问题 3：bitmap ownership 不够集中

bitmap 创建在 `MSRegionInfo`，策略决定在 `CentralRegionPool`，而 teardown 并没有一处完整的兜底回收路径。

### 问题 4：清零路径还有进一步压缩空间

`ResetToBump()`、`SweepRegion()`、`ClearAllMarkBitmaps()` 都会对 bitmap 或 region 数据进行内存写入。
这些路径各自合理，但仍有机会做更细的成本收敛。

## 6. 优化约束与设计原则

后续所有 bitmap 优化都应遵守下面几个边界：

### 6.1 不打破 `MSRegionInfo == 64B`

除非收益极其明确，否则不应为了 bitmap 优化把 `MSRegionInfo` 扩大出一个 cache line。

### 6.2 不打破 Step 8 的三色语义

必须继续保证：

- `markBitmap` 表达是否已发现
- `scannedBitmap` 表达是否已完成 children trace

优化可以改变 bitmap block 的来源，但不能改变三色状态语义。

### 6.3 不打破 Step 9 的 empty cache 语义

进入 `emptySweptCache` 以后，region 必须是**无 class 空壳**。即使未来不再真正 `munmap`，
语义上也必须等价于：

- 旧 class 绑定失效
- 旧 bitmap 不能再被视为当前 region 的有效分配状态

### 6.4 不把 bitmap 管理重新退化成 allocator 热路径负担

bitmap 优化的目标是减少热路径成本，而不是引入新的锁、全局哈希表或复杂同步，
把分配路径重新拖慢。

### 6.5 优先优化生命周期，而不是优先重做编码格式

在当前阶段，真正值得先做的是：

- 减少创建/释放抖动
- 统一 ownership
- 收敛清理路径

而不是先去做位图压缩、分层编码或 SIMD 花式实现。

## 7. 方案 A：Bitmap Block Pool

### 7.1 方案目标

首选方案不是重做 bitmap 编码格式，而是把 bitmap 从：

```text
每个 region 独立 mmap / munmap
```

改为：

```text
统一的 bitmap block acquire / release
```

也就是把“物理内存管理”与“逻辑语义解绑”分开：

- 逻辑上：empty cache 仍然表示无 class 空壳
- 物理上：bitmap block 不一定真的立刻归还给 OS，可以先归还给 pool

### 7.2 核心思想

引入独立的 `MSBitmapBlockPool`，对外只暴露两类接口：

```text
Acquire(bitmapWords) -> base
Release(base, bitmapWords)
```

内部仍按整块连续内存管理：

```text
[ allocBitmap | markBitmap | scannedBitmap ]
```

这样 `MSRegionInfo` 侧的逻辑可以基本保持不变：

- `InitForSizeClass()`：从 pool 获取 block，而不是直接 `mmap`
- `ReleaseBitmaps()`：归还给 pool，而不是直接 `munmap`

### 7.2.1 基于 RegionIndex 的平铺式 Bitmap Arena

除了按 block 做 pool 之外，还有一个更激进但也更直接的方向：

> 不再为单个 region 的 bitmap 做独立 acquire/release，而是按 RegionIndex 在 metadata 前部平铺一整块 sidecar arena。

推荐的布局不是把 `MSRegionInfo` 本体扩成大对象，而是在它旁边增加一块平行区域：

```text
[ MSRegionInfo[maxRegions] | BitmapSlot[maxRegions] | Heap ]
```

其中每个 `BitmapSlot` 对应一个 region index，地址可直接推导：

```text
slotBase(idx) = bitmapArenaBase + idx * kBitmapSlotBytes
```

这样做的关键点是：

1. `MSRegionInfo` 仍保持 64B，不破坏 cache line 约束
2. bitmap 不再按 region 独立 `mmap/munmap`
3. 每个 region 拥有一个稳定的 sidecar slot，empty cache 只做逻辑解绑，不做物理释放
4. page 内天然可以平铺多个 bitmap，而不是一块 bitmap 对应一页映射

### 7.2.2 固定 slot 大小

如果要保证 region 在不同 class 之间切换时不需要搬迁 bitmap，那么单 region 的 slot 必须按最坏情况预留：

- `kMaxBitmapWords = 64`
- `kBitmapSlotBytes = 3 * 64 * 8 = 1536B`

也就是每个 region 固定保留一段 1536B 的 bitmap sidecar 空间。

这样地址推导可以保持简单：

```text
allocBitmap   = slotBase(idx)
markBitmap    = slotBase(idx) + bitmapBytes
scannedBitmap = slotBase(idx) + bitmapBytes * 2
```

其中真正使用的有效长度仍然由当前 class 的 `bitmapWords` 决定，只清理和初始化前 `3 * bitmapBytes` 这一段。

### 7.2.3 生命周期语义

这个方案下，`ReleaseBitmaps()` 不再表示“归还物理内存”，而只表示“解除逻辑绑定”：

- `allocBitmap = nullptr`
- `markBitmap = nullptr`
- `sizeClass = INVALID_SIZE_CLASS`

物理 slot 仍然保留在 arena 中，等待该 region 下次重新绑定 class 时再次使用。

这意味着：

- same-class 复用路径自然成立
- empty cache 语义仍然成立，因为逻辑上该 region 已经不再绑定旧 class
- runtime teardown 时不需要遍历释放单独 bitmap block，OS 直接回收整个 arena 即可

### 7.2.4 与“一页多个 bitmap”的关系

在 macOS arm64 的 16KB 页上，如果采用 1536B 固定 slot：

- 一页最多可容纳 `floor(16384 / 1536) = 10` 个 full slot
- 页内剩余 `16384 - 10 * 1536 = 1024B`

这和当前“一个 bitmap block 一次独立 `mmap`”相比，会把页粒度成本从“每个 region 至少一页”降成“10 个 region 共享一页级 backing”。

换句话说：

- 当前独立 `mmap` 模式，最坏情况下每个 region 的 bitmap 背后都是 16KB 页
- 平铺 arena 模式，最坏情况下每个 region 只占 1536B 固定槽位，对应 heap 的 4.6875%

虽然对 24B / 192B 这种小 bitmap 来说仍然有 slot 内部浪费，但这个浪费已经从“按页”降低为“按 1536B 固定槽位”。

### 7.2.5 内存开销公式

设：

- `MS_REGION_SIZE = 32KB`
- `kBitmapSlotBytes = 1536B`
- `sizeof(MSRegionInfo) = 64B`
- `regionCount = heapBytes / 32KB`

则：

- bitmap arena 大小 = `regionCount * 1536B`
- metadata 大小 = `regionCount * 64B`
- metadata + bitmap sidecar 总大小 = `regionCount * 1600B`

折算到 heap 的比例分别是：

- bitmap arena：`1536 / 32768 = 4.6875%`
- MSRegionInfo metadata：`64 / 32768 = 0.1953125%`
- 二者合计：`1600 / 32768 = 4.8828125%`

### 7.2.6 量级估算

如果按 committed heap 规模估算，量级如下：

| committed heap | regionCount | bitmap arena | MSRegionInfo metadata | front 总开销 |
|---|---:|---:|---:|---:|
| 64MB | 2048 | 3MB | 128KB | 3.125MB |
| 256MB | 8192 | 12MB | 512KB | 12.5MB |
| 512MB | 16384 | 24MB | 1MB | 25MB |
| 1GB | 32768 | 48MB | 2MB | 50MB |
| 4GB | 131072 | 192MB | 8MB | 200MB |

如果按当前代码的 4GB max virtual cap 直接预留整个 arena，则需要额外预留 192MB virtual address space；
如果最终所有 region 的 slot 都被实际触达，那么 bitmap arena 的物理高水位上限也是 192MB。

### 7.2.7 这个方案的优点与代价

优点：

1. 完全消除 bitmap 的 per-region `mmap/munmap`
2. 没有 pool bucket、没有 acquire/release 锁竞争
3. region index 到 bitmap slot 的映射是 O(1)，实现直观
4. empty cache 只做逻辑解绑，不再和物理回收耦合

代价：

1. 固定 slot 采用最坏情况 1536B，对大 class 和 large path 来说会有明显的预留冗余
2. 如果 arena 一旦触达后长期不回收，RSS 会更接近一个稳定的高水位，而不是随 bitmap 生命周期回落
3. 这更像“用确定的 4.6875% heap overhead 换掉不确定的页级抖动与系统调用成本”

因此这个方案非常适合作为“先把 bitmap 从独立映射改成 sidecar arena”的版本；
但如果后续还要继续压低内存占用，就需要在此基础上再做 page 级 scavenging，或继续演进到按 bucket 的 slab arena。

### 7.2.8 其他可实现方案

除了“嵌入到 front layout 的固定 slot arena”之外，当前代码结构下至少还有下面几类可实现方案。

#### 方案 D：保留现有接口，改成按 `bitmapWords` 的 page-slab allocator

这条路径对现有代码最友好，因为可以保留当前接口不变：

```text
Acquire(bitmapWords)
Release(block, bitmapWords)
```

只是把 `MSBitmapManager` 内部从“缓存离散 block”改成“每个 bucket 管理若干 16KB slab 页”。

做法：

1. bucket key 仍然是 `bitmapWords`
2. 每个 bucket 维护若干 page slab
3. slab 内切分多个固定大小 slot，slot 大小为 `24 * bitmapWords`
4. `Release()` 只把 slot 放回 slab free list；当整页 slab 彻底空闲时，再决定是否 `munmap`

在 16KB 页上的单页容量示例：

| bitmapWords | slotBytes | 每页 slot 数 |
|---|---:|---:|
| 1 | 24B | 682 |
| 8 | 192B | 85 |
| 16 | 384B | 42 |
| 32 | 768B | 21 |
| 64 | 1536B | 10 |

优点：

1. 复用当前 `MSBitmapManager` API，改动面最小
2. 立刻解决“一页只装一个 bitmap block”的问题
3. 内存占用按 bucket 实际需求增长，不会像固定 slot arena 那样直接预留 4.6875% heap

代价：

1. 仍需要 bucket/slab 元数据和 free list
2. 要处理 slab scavenging，否则空页可能长期挂住
3. address 不是 region index 直推，调试直观性不如 sidecar arena

#### 方案 E：独立 `BitmapArena` 映射，保持当前 `[metadata | heap]` 布局不动

如果不希望改动 `MSRegionManager::GetVirtualReserveSize()` 和当前 front layout，可以把 sidecar arena 单独映射成第二块 `MemMap`：

```text
Map1: [ MSRegionInfo[maxRegions] | Heap ]
Map2: [ BitmapSlot[maxRegions] ]
```

这个方案的核心仍然是固定 1536B slot，只是把它从主 reserve layout 中拆出去。

优点：

1. 不需要改动现有 heap 起始地址和 metadata 前布局
2. 实现上比改 `GetMetadataSize()/GetVirtualReserveSize()` 更独立
3. 仍然保留 region index -> bitmap slot 的 O(1) 映射

代价：

1. 仍然有固定 slot 的 4.6875% heap 等价开销
2. 需要额外管理一块 sidecar `MemMap`
3. teardown 和 sanitizer 需要额外考虑第二块映射

#### 方案 F：按 committed segment / chunk 扩展的 chunked sidecar arena

这个方案介于固定 slot arena 和 slab allocator 之间。

思路是：

1. 不给所有 max regions 一次性准备一整块 arena
2. 把 region 按 256 或 512 个为一组切 chunk
3. 每个 chunk 对应一段固定 slot sidecar
4. 当 `MSRegionManager` 扩展 committed 区间时，同步激活对应 chunk 的 bitmap arena

以 256-region chunk 为例：

- chunk heap 覆盖：`256 * 32KB = 8MB`
- chunk bitmap arena：`256 * 1536B = 384KB`

优点：

1. 物理内存增长和 committed heap 更同步
2. 可以做 chunk 级 scavenging，比全局固定 arena 更容易回收冷区
3. 仍然保持 region index 到 slot 的规则映射

代价：

1. 实现复杂度高于固定 arena
2. 需要维护 chunk 激活、回收和地址推导逻辑
3. 和 `MSRegionManager` 的 grow/shrink 路径耦合更深

#### 方案 G：只固定 `allocBitmap`，把 `mark/scanned` 拆到 GC 周期 arena

这个方案利用了三类 bitmap 生命周期并不完全一样的事实：

- `allocBitmap` 是 allocator 常驻状态
- `markBitmap/scannedBitmap` 主要在 GC 周期内活跃

因此可以考虑：

1. `allocBitmap` 做固定 sidecar 或 slab 管理
2. `markBitmap + scannedBitmap` 改为按 GC 周期或按活跃 region 批量分配
3. 一轮 GC 结束后回收到 cycle arena，而不是常驻在每个 region 上

按最坏情况估算：

- 固定 `allocBitmap` 最大只要 `64 * 8 = 512B/region`
- 对应 heap 固定开销约 `512 / 32768 = 1.5625%`

优点：

1. steady-state 常驻内存明显低于完整 1536B fixed slot
2. allocator 热路径只依赖 `allocBitmap`

代价：

1. 需要把 `markBitmap` 生命周期从 region 常驻改成 GC 周期管理
2. `ClearAllMarkBitmaps()`、并发标记和 sweep 的时序都要重新梳理
3. 这是几种方案里改动风险最高的一种

### 7.2.9 工程可行性排序

如果按“先做出来、尽快验证收益”的角度排序，当前更推荐：

1. 方案 D：按 `bitmapWords` 的 page-slab allocator
2. 方案 E：独立 `BitmapArena` 映射的固定 slot arena
3. 方案 F：chunked sidecar arena
4. 方案 G：`allocBitmap` 与 `mark/scanned` 生命周期拆分

排序理由：

1. 方案 D 与现有 `MSBitmapManager` 接口完全兼容，代码侵入最小
2. 方案 E 虽然固定开销更大，但实现直观，便于先验证 sidecar 思路
3. 方案 F 更适合在确认 sidecar 思路有效后，继续把物理增长和回收做细
4. 方案 G 内存上最有吸引力，但需要动 GC 与 allocator 的边界，不适合作为第一落地方案

### 7.2.10 sidecar arena：一次性 reserve vs committed 增量扩展

如果确定走 sidecar bitmap arena，这里还有一个实现策略上的分叉：

1. 按 maxVirtualRegionCount 一次性 reserve 整个 arena
2. 跟着 committed heap 的 segment 增长，按段增量扩展 arena

这两个方向都能做，但适合的目标不同。

#### 方案 E1：按 maxVirtual 一次性 reserve

做法：

1. 在初始化时按 maxVirtualRegionCount 直接算出整个 bitmap arena 的虚拟大小
2. 布局变成 `[MSRegionInfo[maxVirtual] | BitmapSlot[maxVirtual] | Heap[maxVirtual]]`
3. region index 到 bitmap slot 继续用纯地址推导：`slotBase(idx) = bitmapArenaBase + idx * 1536`

在当前 4GB max virtual cap 下：

- bitmap arena 额外 virtual reserve = 192MB
- `MSRegionInfo` metadata = 8MB
- front 总保留 = 200MB

但这里有一个关键事实：

> 一次性 reserve 不等于一次性把 192MB 全部变成 RSS。

和当前 `MSRegionInfo` array 一样，整块 arena 可以先只保留虚拟地址；只要后续没有触达对应 page，物理页就不会真正落地。

优点：

1. 最符合当前 `MSRegionManager` 的模型：metadata 本来就是按 maxVirtual 一次性 reserve
2. 实现最简单，不需要改 `GrowOneSegment()` 回调协议
3. 地址推导最直观，region index 和 bitmap slot 一一对应
4. 第一版最容易验证 sidecar arena 本身是否值得做

代价：

1. upfront virtual address space 增加 192MB
2. 一旦很多 slot page 被触达，RSS 高水位可能逐步逼近 arena 实际使用上限
3. 如果后续想把冷 bitmap page 还给 OS，需要额外再做 page/chunk scavenging

#### 方案 E2：按 committed segment 增量扩展

做法：

1. 不一次性给所有 max regions 准备完整 arena
2. 每次 `GrowOneSegment()` 成功后，为新增的 region 范围准备对应 sidecar
3. shrink 时，如果尾部 segment 被回收，再尝试同步回收尾部 sidecar

当前 segment 大小范围是 64MB 到 512MB，因此 bitmap arena 的单次增量大致为：

| heap grow segment | 新增 region 数 | sidecar 增量 |
|---|---:|---:|
| 64MB | 2048 | 3MB |
| 128MB | 4096 | 6MB |
| 256MB | 8192 | 12MB |
| 512MB | 16384 | 24MB |

优点：

1. virtual 和 physical 增长都更贴近 committed heap，而不是 maxVirtual cap
2. 更适合把 sidecar 回收和 heap shrink 绑定起来
3. 长时间运行时，如果 committed heap 远小于 4GB，上限更紧

代价：

1. 实现复杂度明显更高
2. 需要把 sidecar 生命周期接进 `InitializeFreshRegions()` / `OnCommittedHeapExtended()` / shrink 路径
3. 如果不是做成连续预留的 chunked arena，地址推导会比一次性 reserve 更绕
4. 第一版更容易把问题从 bitmap 本身转移到 grow/shrink 协议细节

#### 当前实现下的实际区别

从当前代码结构看，这两者最重要的差别不是“是否立刻吃掉 192MB RSS”，而是：

1. 一次性 reserve 主要多吃的是虚拟地址空间，物理页仍然可以按触达懒提交
2. 增量扩展主要节省的是地址空间和回收边界管理，但会显著增加实现复杂度

换句话说，在 64 位平台上，192MB virtual reserve 本身并不算大问题；真正麻烦的是如果 arena 触达后没有回收策略，RSS 高水位可能回不来。

#### 建议结论

如果要在 sidecar arena 这条路上先做第一版，更建议：

1. 先做 maxVirtual 一次性 reserve 的版本
2. 先利用 OS 的 lazy commit，让物理页随着 slot 触达自然增长
3. 等 sidecar 方案本身验证有效后，再决定是否继续演进到 committed-segment 的 chunked arena

原因很直接：

1. 它和当前 `MSRegionInfo` 的 reserve 模型一致，接入最顺
2. 第一版可以把关注点放在“bitmap 独立映射是否值得消除”，而不是 heap grow/shrink 协议
3. 如果一开始就做 committed 增量扩展，调试复杂度会明显上升，收益却未必先于 page-slab 方案体现出来

### 7.3 分桶方式

推荐按 `bitmapWords` 或等价的 `totalBytes` 分桶，而不是按 `sizeClass` 分桶。

原因：

1. bitmap block 的物理大小只由 `totalSlotCount` 决定
2. 多个 class 会共享同样的 `bitmapWords`
3. block 重新初始化后，不再携带旧 class 语义

因此第一版最简单的桶 key 可以是：

```text
bucketKey = bitmapWords
```

### 7.4 推荐 ownership

推荐把 pool 作为独立组件存在，而不是塞进 `MSRegionInfo` 自己管理。

职责分工建议如下：

- `MSRegionInfo`：只负责描述 region 当前 bitmap 指针与内容
- `MSBitmapBlockPool`：负责 block 的 acquire / release / teardown
- `RegionSpaceMS` 或 MSGC allocator 初始化路径：负责 pool 生命周期

这样后续 runtime 销毁时，也可以由 pool 统一释放仍缓存的 bitmap block，补上当前 teardown 的兜底能力。

### 7.5 逻辑语义如何保持不变

这个方案的关键点是：

> `ReleaseBitmaps()` 可以从“归还 OS”变成“归还 bitmap pool”，但对 region 语义必须仍然等价于“bitmap 已失效”。

也就是说，在 `ResetToEmptyCache()` 之后，region 仍然必须满足：

- `allocBitmap == nullptr`
- `markBitmap == nullptr`
- `sizeClass == INVALID_SIZE_CLASS`
- 旧 class 的布局语义已经彻底解绑

至于底层 block 是进 pool 还是直接 `munmap`，不应影响上层语义。

### 7.6 第一版实现建议

第一版建议保守一些：

1. pool miss 仍然走原生 `mmap/VirtualAlloc`
2. pool hit 直接返回已有 block
3. pool release 先只做无锁/轻锁缓存，不做复杂 scavenger
4. 每个 bucket 设置高水位，超过后再真正 `munmap`

但这里还需要补一个针对页粒度问题的约束：

5. 第一版不要让所有 bucket 一视同仁地按固定深度缓存，尤其不能让 24B ~ 192B 的小 block 长时间各自占住整页

这样能先验证“减少系统调用抖动”是否已经足够带来收益。

更具体地，第一版落地时建议同时做三件事：

1. 增加 bucket hit/miss、缓存深度、按页折算 cached bytes 的统计
2. 为小 `bitmapWords` bucket 设置更低 cap，必要时直接禁用缓存
3. 增加全局 cached-page budget，而不是只靠 per-bucket 个数上限

### 7.7 预期收益

方案 A 的主要收益有四个：

1. 显著减少 empty cache rebind 带来的 `mmap/munmap` 往返
2. 保持 Step 9 的 empty cache 语义不变
3. 统一 bitmap 生命周期管理入口
4. 为 runtime teardown 提供明确的释放出口

## 8. 方案 B：同 class 热复用扩展

### 8.1 当前已有能力

当前实现其实已经有一个同 class 热复用优化：

```text
完全空 region + 请求 class 与旧 class 相同
	-> ResetToBump()
	-> 保留 bitmap block
```

这条路径非常有效，而且语义也干净。

### 8.2 为什么不建议一开始就大幅扩张

继续扩大同 class 热复用虽然看起来能进一步减少 bitmap 重建，但风险也更高：

1. 容易削弱 Step 9 的“global classless empty cache”语义
2. 容易把 allocator 行为重新耦合回某个 size class 的局部热度
3. 容易让 reclaim / scavenge 行为变得不稳定

因此不建议在没有 pool 的情况下，先去扩张热复用策略。

### 8.3 可选的第二阶段增强

如果方案 A 落地后仍然看到明显的 bitmap 重建热点，可以再考虑引入**很薄的一层同 class 热缓存**：

- 每个 class 保留极少量 recent-empty region
- 只作为短时 locality hint
- 溢出部分仍然进入全局 empty cache

这里要强调：

> 这只能是方案 A 之后的可选增强，不应反过来替代 empty cache 主语义。

### 8.4 建议结论

方案 B 不是当前第一优先级。

优先顺序应是：

```text
先统一 bitmap block 生命周期
再看是否还需要扩大同 class 热复用
```

## 9. 方案 C：清零与统计路径优化

### 9.1 `SweepRegion()` 的 dead slot 清零

当前 `SweepRegion()` 在得到 `allocBitmap = ~markBitmap` 之后，会对每个 free bit 对应的 slot 做一次清零。

这条路径的正确性价值很高，因为它避免了后续复用时 tracing 到脏引用。

但它仍有优化空间：

#### 方向 A：按连续 free run 合并清零

当前实现是“逐 free bit -> 单 slot memset”。

后续可考虑：

- 先在 word 内聚合连续 free run
- 再对一段连续 slot 做一次范围 memset

这样能降低小 slot class 下的大量短 memset 次数。

#### 方向 B：按 class 选择是否保留现状

对大 slot class，逐 slot memset 的成本本来就不高；对小 slot class 才可能更值得优化。

因此这类优化更适合做成启发式，而不是一刀切改所有 class。

### 9.2 `ClearAllMarkBitmaps()` 的清理成本

当前实现会在下一轮 `MarkSetup` 前统一清理：

- `markBitmap`
- `scannedBitmap`

这条路径本身是合理的，因为它把 sweep 与下一轮标记解耦了。

这里不建议过早做激进改动，原因是：

1. 这条路径对正确性边界敏感
2. 与 `StopAndWaitForAllSweep()`、`sweepGen`、TL flush 时序强耦合
3. 优化空间大概率小于方案 A 带来的收益

### 9.3 统计路径优化

当前 `ComputeMarkedBytes()` 通过遍历各 region 的 `markBitmap` 统计 true live bytes。

虽然可以考虑增量维护 live 统计，但这会把额外原子更新压到标记热路径上，不适合作为第一阶段优化。

因此推荐策略是：

- 第一阶段不动统计语义
- 如果后续发现 `ComputeMarkedBytes()` 成本显著，再单独评估是否做缓存或增量统计

### 9.4 本方案定位

方案 C 是**次级优化**。

它更适合在方案 A 落地并验证之后，再根据 profile 决定是否继续做。

## 10. 推荐实施顺序

推荐按下面的顺序推进：

### 第一阶段：统一 bitmap block 生命周期

目标：先解决“bitmap 很小，但生命周期管理过重”的核心矛盾。

动作：

1. 引入 `MSBitmapBlockPool`
2. 把 `InitForSizeClass()` 接入 `Acquire()`
3. 把 `ReleaseBitmaps()` 接入 `Release()`
4. 保持 `ResetToBump()`、`ResetToEmptyCache()`、`ClearAllMarkBitmaps()` 语义不变

### 第二阶段：收敛释放职责

目标：把 bitmap 释放职责进一步收敛，减少重复触点。

动作：

1. 整理 `ReleaseRegionInternal()` 与 `Reset()` 的职责边界
2. 明确 teardown 时由谁统一 drain bitmap pool

### 10.1 针对页粒度浪费的专项优化计划

为了把“bitmap 很小，但独立 `mmap` 至少占一页”的问题真正收敛掉，建议按下面几个阶段推进：

#### 阶段 0：先补观测

1. 记录各 bucket 的 hit/miss、缓存深度、缓存 block 数、按页折算的 cached bytes
2. benchmark 同时观察 `mmap/munmap` 次数、RSS 峰值、RSS 尾值、总耗时
3. 明确哪些 `bitmapWords` bucket 是主要驻留来源，而不是凭感觉调参

#### 阶段 1：先收紧当前 block cache 策略

1. 显著降低 `kMaxCachedBlocksPerBucket`
2. 对小 block bucket 设置更小上限，必要时直接不缓存
3. 增加全局 cached-page budget，超过预算立即回收给 OS
4. 先把 RSS 尾部压回去，再看 syscall 收益是否还能保住

#### 阶段 2：从按 block 缓存升级为按 page/slab 缓存

1. 一次 `mmap` 一页或多页 slab，而不是一个 bitmap block 映射一页
2. 在 slab 内切分多个同尺寸 bitmap block
3. bucket 管理对象从“单 block”变为“slab + free list”
4. 只有走到这一步，才可能同时降低 syscall 抖动和页内碎片

这里还有一个可并行评估的分支：

1. 不做按 bucket 的 slab，而是直接引入按 RegionIndex 平铺的固定 slot bitmap arena
2. 用固定 1536B sidecar slot 替换当前 acquire/release 机制
3. 用约 4.6875% 的 heap 固定开销，换掉 per-region bitmap 独立映射问题
4. 如果后续仍觉得 4.6875% 偏高，再在 arena 之上增加 page 级 scavenging

在工程落地时，建议把这一阶段再细分为两条并行路线：

1. 低侵入路线：先把现有 `MSBitmapManager` 改成 page-slab allocator
2. 结构性路线：验证固定 slot sidecar arena 是否值得用确定开销换简单语义

#### 阶段 3：增加冷页回收

1. 对长期未命中的 slab 做 scavenging
2. 空 slab 整页 `munmap` 或等价回收
3. 在低活跃期回收冷缓存，避免 idle RSS 被历史热点长期抬高

### 第三阶段：再评估热点增强

如果 profile 仍显示明显开销，再考虑：

1. 同 class 热缓存
2. `SweepRegion()` 连续 free run 清零
3. bitmap 统计路径优化

### 不建议的顺序

不建议一开始就：

1. 修改 `MSRegionInfo` 结构大小
2. 重做 bitmap 编码格式
3. 把 empty cache 重新退化成 per-class cache 主模型

## 11. 风险与验证

### 11.1 主要风险

#### 风险 1：旧 bitmap 语义泄漏到 empty cache

如果 pool 化后只是“物理上缓存 block”，但没有正确把 region 逻辑状态清空，
就会破坏 empty cache 的无 class 语义。

#### 风险 2：跨 class 复用时 bitmap 未完全重置

这会直接导致：

- `allocBitmap` 脏状态
- `markBitmap/scannedBitmap` 残留
- trailing bits 掩码错误

#### 风险 3：并发路径上的 ownership 不清晰

需要保证任何 block 被归还到 pool 之前，已经没有线程再读写该 block。

#### 风险 4：teardown 新增真正的资源管理复杂度

当前实现更依赖进程退出时 OS 回收；引入 pool 后，就需要明确 cache block 的最终 drain 逻辑。

### 11.2 建议验证点

#### 功能验证

1. 新 region 初始化后，三段 bitmap 内容正确
2. same-class `ResetToBump()` 后 bitmap 可继续使用
3. `ResetToEmptyCache()` 后 region 逻辑上不再绑定旧 class
4. empty cache rebind 后，新的 class 布局正确
5. `ClearAllMarkBitmaps()` 之后所有小对象 region 的 `mark/scanned` 均已清零

#### 回归验证

1. 标准 marksweep 构建通过
2. 现有 MSGC benchmark 全量通过
3. GC 过程不再出现 stale mark / stale scanned 导致的 trace 异常

#### 性能验证

重点观察：

1. bitmap 相关 `mmap/munmap` 次数是否显著下降
2. benchmark wall time 是否下降或至少不退化
3. RSS 与 active-region 曲线是否维持当前稳定形态
4. bitmap pool 的 cached pages / cached bytes 是否受控，没有把 syscall 收益换成长期驻留

### 11.3 建议的验收标准

第一阶段可以用下面三个标准判断是否成功：

1. 语义完全不变
2. benchmark 无功能回退
3. bitmap 系统调用显著减少

## 12. 与 Step 8 / Step 9 的关系

### 12.1 与 Step 8 的关系

Step 8 确立的是**三色语义表达**：

- `markBitmap` 表示已发现
- `scannedBitmap` 表示已扫描

Step 10 不改变这个语义，只优化这两段 bitmap 与 `allocBitmap` 的物理生命周期管理方式。

### 12.2 与 Step 9 的关系

Step 9 确立的是**empty cache 语义边界**：

- empty cache 是 classless shell
- 进入 empty cache 前，旧 class 语义必须解绑

Step 10 的所有优化都必须服从这个边界。即使后续不再真的 `munmap`，也必须做到语义等价。

### 12.3 本文档的定位

因此可以把 Step 10 看作：

> 在不改变 Step 8 逻辑语义、也不改变 Step 9 控制平面语义的前提下，对 bitmap 这一层做一次独立的资源管理与性能优化设计。

换句话说：

- Step 8 解决“bitmap 表达什么”
- Step 9 解决“empty region 如何流转”
- Step 10 解决“bitmap block 自己怎么更高效地活着和被复用”
