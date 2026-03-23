

> 设计文档: `doc/Step1_分配器改造详细设计.md`

### Step 1.1: MSRegionInfo 元数据

> 设计文档: `doc/Step1.1_MS_RegionInfo元数据改造设计.md`
>
> **目标**: 用 64 字节 `MSRegionInfo` 替代 CopyGC 的 ~120 字节 `UnitMetadata`，提供 O(1) 地址→元数据查找。

#### 新增文件

| 文件 | 行数 | 说明 |
|------|------|------|
| `src/Heap/Allocator/MSRegionInfo.h` | 252 | `MSRegionInfo` 结构体 + `MSRegionInfoArray` 镜像数组 |
| `src/Heap/Allocator/MSRegionInfo.cpp` | 383 | 生命周期 / 分配 / 标记 / 清扫全流程实现 |

#### Region 与 MSRegionInfo 的关系

##### 1. 内存布局与映射

**Region** 是堆上连续的 32KB 物理内存块（`MS_REGION_SIZE = 32768`），用于存放仓颉对象。

**MSRegionInfo** 是 64 字节的元数据结构（恰好一个缓存行），独立 `malloc` 分配为一个**平坦数组**（`MSRegionInfoArray::array_`），与堆上的 Region **一一镜像**：

```
MSRegionInfoArray (malloc'd)         Heap Region Space
┌─────────────────┐                  ┌──────────────────┐
│ array_[0]  64B  │  ◄────────────►  │ Region 0   32KB  │ heapStart
│ array_[1]  64B  │  ◄────────────►  │ Region 1   32KB  │ heapStart + 32KB
│ array_[2]  64B  │  ◄────────────►  │ Region 2   32KB  │ heapStart + 64KB
│ ...             │                  │ ...              │
│ array_[N-1]     │  ◄────────────►  │ Region N-1       │
└─────────────────┘                  └──────────────────┘
```

寻址公式（O(1) 双向映射）：

| 方向 | 公式 | 代码 |
|------|------|------|
| 地址 → MSRegionInfo | `idx = (addr - heapStart) / 32KB` | `MSRegionInfoArray::GetRegionInfo(addr)` |
| 索引 → Region 起始地址 | `addr = heapStart + idx * 32KB` | `MSRegionInfoArray::GetRegionAddress(idx)` |
| MSRegionInfo → 索引 | `idx = info - array_` | `MSRegionInfoArray::GetRegionIdx(info)` |
| MSRegionInfo → Region 起始 | `regionEnd - MS_REGION_SIZE` | `info->RegionStart()` （仅限小/中对象） |

##### 2. 小/中对象（≤16KB）生命周期

小/中对象使用**单个 Region**（32KB），MSRegionInfo 与 Region 一一对应。

```
┌─────────────────────────────────────────────────────────────────┐
│ 创建 (TakeRegion → InitForSizeClass)                           │
│ ┌───────────┐     ┌────────────────────────────────────┐       │
│ │ FREE      │────►│ BUMP                               │       │
│ │ 空闲池    │     │ allocPtr=RegionStart, bitmap 已分配 │       │
│ └───────────┘     └───────────┬─────────────────────────┘       │
│                               │ bump-pointer 分配用尽           │
│                               ▼                                 │
│                   ┌────────────────────────────────────┐       │
│                   │ FULL   (freeSlotCount == 0)        │       │
│                   └───────────┬─────────────────────────┘       │
│                               │ SweepRegion()                   │
│                               ▼                                 │
│                   ┌────────────────────────────────────┐       │
│                   │ PARTIAL (有 freeSlot, bitmap 分配)  │       │
│                   └──────┬────────────┬─────────────────┘       │
│                          │分配用尽    │ 全部空闲                │
│                          ▼            ▼                         │
│                        FULL      FREE → 回收到空闲池            │
│                        (循环)    (Reset + ReleaseBitmaps)       │
└─────────────────────────────────────────────────────────────────┘
```

关键操作：

| 阶段 | Region 变化 | MSRegionInfo 变化 |
|------|------------|-------------------|
| **创建** | `RegionManager::TakeRegion(1)` 分配 32KB | `InitForSizeClass(cls)` ：设置 type=BUMP、分配 allocBitmap/markBitmap |
| **分配** | 对象写入 Region 内存 | `Alloc()` 移动 allocPtr 或扫描 allocBitmap |
| **标记** | 无变化 | `SetMarkBit(objAddr)` / `TestAndSetMarkBit(objAddr)` 写 markBitmap |
| **清扫** | 无变化（原地清扫） | `SweepRegion()` ：allocBitmap = ~markBitmap，type→PARTIAL |
| **回收** | 物理内存释放回 RegionManager | `Reset()` 清零 + `ReleaseBitmaps()` munmap 释放 bitmap |
| **复用** | 再次 `TakeRegion(1)` | 再次 `InitForSizeClass(cls)` |

##### 3. 大对象 Tier1（16KB ~ 4MB）—— 堆内多 Region

大对象跨越 N 个连续 Region，在 MSRegionInfoArray 中对应 **1 个 base entry + (N−1) 个 span entry**：

```
MSRegionInfoArray                      Heap (连续 N 个 Region)
┌──────────────────────────────────┐   ┌──────────────────────────┐
│ array_[baseIdx]   (base entry)   │──►│ Region[0]  ┐             │
│   regionType = LARGE             │   │            │ 对象实际数据 │
│   allocPtr   = objStart          │   │            │             │
│   regionEnd  = objStart + N*32KB │   ├────────────┤             │
│   reserved   = baseIdx (自身)    │   │ Region[1]  │             │
├──────────────────────────────────┤   │            │             │
│ array_[baseIdx+1] (span entry)   │──►├────────────┤             │
│   regionType = LARGE             │   │ Region[2]  │             │
│   regionEnd  = 0  ← 注意!       │   │            ┘             │
│   allocPtr   = 未设置            │   └──────────────────────────┘
│   reserved   = baseIdx (回指)    │
├──────────────────────────────────┤
│ array_[baseIdx+2] (span entry)   │──►
│   ...同上...                     │
└──────────────────────────────────┘
```

**注意事项**：

- `RegionStart()` 返回 `regionEnd - MS_REGION_SIZE`，仅对小/中对象正确。对 LARGE base entry 而言，真实起始地址应使用 `allocPtr`。
- Span entry 的 `regionEnd = 0`、`allocPtr` 未设置，不可调用 `RegionStart()`。
- Span entry 的 `reserved` 字段存储 baseIdx，可回溯到 base entry。
- `GetRegionInfoValidated()` 通过判断 `regionEnd == 0` 识别 span entry。

生命周期：

| 阶段 | Region 变化 | MSRegionInfo 变化 |
|------|------------|-------------------|
| **创建** | `TakeRegion(N)` 分配 N×32KB | `RegisterLargeObjectMS` ：base `InitForLargeObject(start,end)` + N−1 个 span 设 type=LARGE+reserved=baseIdx |
| **标记** | 无变化 | base entry `isMarked = 1` |
| **清扫** | 无变化 / 回收释放 | `SweepLargeRegions()` ：遍历 span 清零 + base `Reset()` |
| **回收** | 物理内存释放回 RegionManager | 从 `LargeRegionList` 移除 |

##### 4. 大对象 Tier2（> 4MB）—— 独立 mmap

超过 `kHugeObjectThreshold`（4MB）的对象完全绕过堆和 MSRegionInfo，使用独立的 `LargeObjectSpace` 管理。

```
LargeObjectSpace (独立链表)                    mmap 区域 (堆外)
┌──────────────────────────────┐              ┌───────────────────┐
│ LargeObjectInfo              │              │                   │
│   baseAddr   = mmap_start    │─────────────►│  对象数据 (>4MB)  │
│   mappedSize = mmap_size     │              │                   │
│   objectSize = alloc_size    │              └───────────────────┘
│   gcMark     = false         │
│   prev ◄─── next ───►       │  (侵入式双向链表)
└──────────────────────────────┘
```

**与 MSRegionInfo 的区别**：

| 对比项 | Tier1（MSRegionInfo） | Tier2（LargeObjectSpace） |
|--------|----------------------|--------------------------|
| 内存来源 | 堆内 `TakeRegion(N)` | 独立 `mmap` |
| 元数据 | `MSRegionInfo` 数组 entry | `LargeObjectInfo` 链表节点 |
| 地址范围 | `IsHeapAddress() == true` | `IsHeapAddress() == false` |
| 标记方式 | `baseInfo->isMarked = 1` | `loInfo->gcMark = true` |
| 清扫方式 | `SweepLargeRegions()` 遍历链表 | `SweepUnmarkedObjects()` + `munmap` |
| 查找复杂度 | O(1) 数组索引 | O(n) 链表遍历 |

---

#### 关键类型与函数

**`MSRegionType` 枚举** (MSRegionInfo.h L32-38):

> CopyGC 有 15+ 种 RegionType，MS-GC 精简为 6 种状态。状态转换图：
>
> ```
> FREE --InitForSizeClass()--> BUMP --槽位耗尽--> FULL --SweepRegion()--> PARTIAL --分配耗尽--> FULL
>                                                                        PARTIAL --全部死亡--> FREE
> FREE --InitForLargeObject()--> LARGE --GC未标记--> FREE
> FREE --跨度初始化--> SPAN （未实际使用）
> ```

| 枚举值 | 值 | 含义 | 设置位置 | 使用/检查位置 |
|--------|---:|------|----------|--------------|
| `MS_REGION_FREE` | 0 | 未分配，在 RegionManager 空闲池中。所有字段已清零，bitmap 已释放。 | `MSRegionInfo::Reset()` (MSRegionInfo.cpp L135)；`MSRegionInfoArray::Init()` 初始化所有条目 (MSRegionInfo.cpp L361) | `ReleaseRegionInternal()` 调用 `Reset()` 将 Region 归还空闲池 (CentralRegionPool.cpp L192) |
| `MS_REGION_BUMP` | 1 | 首次填充阶段，使用 bump-pointer 分配（`allocPtr` 单调递增直到 `regionEnd`）。分配速度最快。 | `MSRegionInfo::InitForSizeClass(cls)` (MSRegionInfo.cpp L28) | `AllocBufferMS::FastAlloc()` 判断 BUMP→走 `Alloc()` bump 路径 (AllocBufferMS.cpp L76)；`SlowAlloc()` 同理 (AllocBufferMS.cpp L114) |
| `MS_REGION_PARTIAL` | 2 | 已清扫，有空闲槽位（`freeSlotCount > 0`）。使用 bitmap 分配（CTZ 扫描）。 | `CentralRegionPool::GetRegion()` — lazy sweep 后设置 (CentralRegionPool.cpp L40, L67, L96)；`ReturnPartialRegion()` (CentralRegionPool.cpp L158) | `AllocBufferMS::FastAlloc()` 非 BUMP 分支→走 `AllocFromBitmap()` (AllocBufferMS.cpp L82)；`CentralRegionPool` 四链表分类依据 |
| `MS_REGION_FULL` | 3 | 无空闲槽位（`freeSlotCount == 0`）。等待下轮 GC 清扫后可能变为 PARTIAL 或 FREE。 | `CentralRegionPool::ReturnFullRegion()` (CentralRegionPool.cpp L171) | `CentralRegionPool::GetRegion()` 优先级 3 — fullUnswept 链表 sweep 后检查是否变 PARTIAL (CentralRegionPool.cpp L80-100) |
| `MS_REGION_LARGE` | 4 | 大对象 Region（单个对象占一个或多个 Region）。不经过 SizeClass/bitmap 分配，单独管理生命周期。 | `MSRegionInfo::InitForLargeObject()` (MSRegionInfo.cpp L96)；`RegionSpace::RegisterLargeObjectMS()` 跨度 span 条目也设为 LARGE (RegionSpace.cpp L283) | `MarkSweepCollector::MarkObject()` — regionType==LARGE 时回溯 baseIdx (MarkSweepCollector.cpp L212)；`RegionSpace::MarkObjectMS()` 同理 (RegionSpace.cpp L153)；`RegionSpace::IsMarkedObjectMS()` (RegionSpace.cpp L247) |
| `MS_REGION_SPAN` | 5 | 多 Region 大对象的非首条目（`reserved` 字段存 primaryIdx）。⚠️ **已定义但未使用**——当前跨度条目直接设为 `MS_REGION_LARGE` 并通过 `reserved=baseIdx` 回溯。| 无（枚举保留但无代码赋值） | 无实际检查点。可考虑清理。 |

**`MSSweepState` 枚举** (MSRegionInfo.h L44-48):

> Lazy Sweep 协议的三态 CAS 状态机，配合 `sweepGen` 代际计数使用。

| 枚举值 | 值 | 含义 | 设置位置 | 检查位置 |
|--------|---:|------|----------|----------|
| `UNSWEPT` | 0 | 未清扫。新分配或 GC 代际翻转后的初始状态。 | `InitForSizeClass()` (MSRegionInfo.cpp L27)；`InitForLargeObject()` (L95)；`Reset()` (L134) | `Alloc()` — 非 SWEPT 走 bump-pointer (MSRegionInfo.cpp L151)；`HasFreeSpace()` — 非 SWEPT 走 bump 尾部判断 (L307) |
| `SWEEPING` | 1 | 正在清扫中。某线程已通过 `CasSweepGen(sg-2, sg-1)` 获取所有权。 | `CasSweepGen()` CAS 成功时隐式进入（由 sweepGen 值表示） | `CentralRegionPool::GetRegion()` — CAS 失败说明其他线程正在 SWEEPING，跳过该 Region (CentralRegionPool.cpp L61, L82) |
| `SWEPT` | 2 | 清扫完成。`allocBitmap` 已更新为可分配位图，`freeSlotCount` 已刷新。 | `SweepRegion()` 末尾 (MSRegionInfo.cpp L238) | `Alloc()` — SWEPT 走 `AllocFromBitmap()` (MSRegionInfo.cpp L151)；`HasFreeSpace()` — SWEPT 检查 `freeSlotCount > 0` (L307) |

> **注**: 实际 lazy sweep 协议同时依赖 `sweepGen`（代际计数）+ `CasSweepGen()` CAS 操作，`MSSweepState` 枚举主要用于 Region 内部分配/查询的快速判断。

---

**`MSRegionInfo` 结构体** (MSRegionInfo.h L56-145, 64 字节 = 1 cache line):

> 替代 CopyGC 的 `UnitMetadata`（~120 字节），用更紧凑的 64 字节结构管理 Region 的分配、标记和清扫。`allocPtr` 强制在 offset 0，满足编译器快速路径 ABI 约束。

**属性列表**:

| 字段 | 偏移 | 类型 | 说明 | 主要使用位置 |
|------|-----:|------|------|-------------|
| `allocPtr` | +0 | `uintptr_t` | Bump-pointer 当前位置。BUMP 状态下单调递增；SWEPT 状态下不再使用（bitmap 分配）。**编译器快速路径 ABI 约束：必须在 offset 0。** | **写**: `MSRegionInfo::Alloc()` bump 递增；`CentralRegionPool::GetFreshRegion()` 初始化 (L138)。**读**: `CentralRegionPool::ReclaimLargeRegions()` 取大对象起始地址 (L437)；`GetRegionInfoValidated()` LARGE 范围校验。 |
| `regionEnd` | +8 | `uintptr_t` | Region 结束地址（不含）。`RegionStart() = regionEnd - MS_REGION_SIZE`。 | **写**: `CentralRegionPool::GetFreshRegion()` (L139)；`InitForLargeObject()`。**读**: `Alloc()` bump 边界检查；`GetRegionInfoValidated()` 范围校验；`LargeObjectSpace::Sweep()` 计算 objSize (L72)。 |
| `nextRegionIdx` | +16 | `int32_t` | 侵入式链表下一个节点索引。`INVALID_IDX = 0xFFFFFFFF` 表示链表尾。 | **读/写**: `MSRegionList::PushFront/PopFront/Remove/ForEach()` (CentralRegionPool.h L59-122)；`LargeRegionList::Add/Remove/Sweep()` (LargeObjectSpace.cpp L29-65)。 |
| `prevRegionIdx` | +20 | `int32_t` | 侵入式链表上一个节点索引。用于 `MSRegionList` 双向链表 O(1) 删除。 | 同 `nextRegionIdx`，在 `MSRegionList` 和 `LargeRegionList` 的链表操作中成对使用。 |
| `allocBitmap` | +24 | `uint64_t*` | 分配位图指针。`1` = 空闲可分配，`0` = 已占用。大小按 `(totalSlotCount+63)/64` 个 word。 | **内部**: `AllocFromBitmap()` CTZ 扫描分配；`SweepRegion()` 中 `allocBitmap[i] = ~markBitmap[i]`。**外部写**: `RegionSpace::AllocateLargeMS()` 大对象 span 设 `nullptr` (L285)。 |
| `markBitmap` | +32 | `uint64_t*` | 标记位图指针。`1` = 存活已标记，`0` = 死亡。两个 bitmap 通过单次 `mmap` 连续分配。 | **内部**: `TestAndSetMarkBit/SetMarkBit/GetMarkBit()`。**外部读**: `MarkSweepCollector::ResetMarkBitmaps()` 判空+清零 (L128-129)；`RegionSpace::MarkObjectMS/IsMarkedObjectMS()` 判空检查 (L159, L259)；`MarkSweepCollector::MarkObject()` 有效性检查 (L222)。 |
| `totalSlotCount` | +40 | `uint16_t` | Region 总槽位数 = `MS_REGION_SIZE / slotSize`。 | **内部**: `IsCompletelyFree()` 判断；`SweepRegion()` 计算 wordCount。**外部读**: `MarkSweepCollector::ResetMarkBitmaps()` 计算 bitmap words (L127)；`RegionSpace` 联合判空 (L159, L259)。**外部写**: 大对象 span 设 0 (RegionSpace.cpp L286)。 |
| `freeSlotCount` | +42 | `uint16_t` | 当前空闲槽位数。分配时递减，清扫时重算。 | **内部**: `Alloc/AllocFromBitmap()` 递减；`SweepRegion()` 重算；`HasFreeSpace()` 判断；`IsCompletelyFree()` 判断。**外部写**: 大对象 span 设 0 (RegionSpace.cpp L287)。 |
| `liveSlotCount` | +44 | `uint16_t` | 上轮 GC 后的存活槽位数（by `popcount(markBitmap)`）。 | **完全封装**，仅在 `MSRegionInfo.cpp` 内部：`SweepRegion()` 写入、`GetLiveBytes()` 读取。无外部直接访问。 |
| `rawPointerObjCount` | +46 | `uint16_t` | 弱引用/原始指针对象计数（预留）。 | **完全封装**，仅在 `InitForSizeClass/InitForLargeObject/Reset()` 中清零。当前未实际使用。 |
| `sizeClass` | +48 | `uint8_t` | SizeClass 索引（0-35）。大对象 Region 为 0。 | **完全封装**，外部通过方法间接访问：`GetSlotSize()` → `ClassToSize(sizeClass)`；`InitForSizeClass(cls)` 由 `CentralRegionPool::GetFreshRegion()` 调用 (L135)。 |
| `sweepState` | +49 | `MSSweepState` | 清扫状态（UNSWEPT / SWEEPING / SWEPT）。 | **完全封装**，仅在 `MSRegionInfo.cpp` 内部：`Alloc()` / `HasFreeSpace()` 检查 SWEPT；`SweepRegion()` 设为 SWEPT；`Init*/Reset()` 设为 UNSWEPT。 |
| `regionType` | +50 | `MSRegionType` | Region 类型（FREE / BUMP / PARTIAL / FULL / LARGE / SPAN）。 | **外部写**: `CentralRegionPool` 设 PARTIAL/FULL (L40, L67, L96, L158, L171)；`RegionSpace::AllocateLargeMS()` span 设 LARGE (L283)。**外部读**: `AllocBufferMS::FastAlloc/SlowAlloc` 判 BUMP (L76, L114)；`RegionSpace/MarkSweepCollector` 判 LARGE (L153, L247, L212)。详见 MSRegionType 表格。 |
| 标志位域 | +51 | `uint8_t` | `isMarked:1` — GC 标记标志；`isEnqueued:1` — 弱引用入队标志；`isResurrected:1` — Finalizer 复活标志；`flagsPadding:5` — 保留。 | **`isMarked`** 是外部访问最广泛的字段：`RegionSpace::MarkObjectMS()` 读/写 (L251, L254)；`IsMarkedObjectMS()` 读 (L156)；`MarkSweepCollector::MarkObject()` 读/写 (L215-216)；`CentralRegionPool::PrepareForSweep()` 重置 (L407)；`LargeObjectSpace::Sweep()` 判活/重置 (L67, L69)；分配时 alloc-black 设 1 (RegionSpace.cpp L298)。 |
| `sweepGen` | +52 | `uint32_t` | 清扫代际编号。配合 `CasSweepGen()` 实现线程安全的 lazy sweep。 | **无外部直接访问**，全部通过方法接口：`CasSweepGen()` — `CentralRegionPool::GetRegion()` lazy sweep 抢占 (L57, L85)；`SetSweepGen()` — sweep 完成后设当前代 (L59, L87)；`GetSweepGen()` — `ReturnPartial/FullRegion()` 判断归入 swept 还是 unswept 链 (L160, L173)。 |
| `reserved` | +56 | `uint64_t` | 多用途：小对象时低 32 位存 `searchHint`（bitmap 搜索起始 word）；大对象时存 `baseIdx`（跨度回溯到首条目）。 | **内部**: `AllocFromBitmap()` 读/写 searchHint；`SweepRegion()` 重置 searchHint。**外部**: `RegionSpace::AllocateLargeMS()` 写 baseIdx (L277, L290)；`RegionSpace::MarkObjectMS/IsMarkedObjectMS()` 读 baseIdx 回溯 base region (L155, L250)；`MarkSweepCollector::MarkObject()` 读 baseIdx (L214)。 |

> `static_assert(sizeof(MSRegionInfo) == 64)` — 编译期保证恰好一个 cache line。

**方法列表**:

| 方法 | 说明 | 主要使用位置 |
|------|------|-------------|
| `RegionStart()` | 返回 `regionEnd - MS_REGION_SIZE`。O(1) 地址计算。 | `GetRegionInfoValidated()` 范围校验；`Alloc()` / `AllocFromBitmap()` 计算槽位偏移；`TestAndSetMarkBit()` 边界检查。 |
| `InitForSizeClass(cls)` | 初始化为 BUMP 状态：设 sizeClass、`mmap` 分配双 bitmap、allocBitmap 全 1（全空闲）、markBitmap 全 0。 | `CentralRegionPool::GetFreshRegion()` (CentralRegionPool.cpp L135) — 新 Region 首次初始化。 |
| `InitForLargeObject(start, end)` | 初始化为 LARGE 状态：设 allocPtr=start, regionEnd=end，不分配 bitmap（大对象用 `isMarked` 单标志位）。 | `RegionSpace::RegisterLargeObjectMS()` (RegionSpace.cpp L272) — 大对象 Region 注册。 |
| `ReleaseBitmaps()` | `munmap` 释放双 bitmap 内存（alloc + mark 连续分配，一次释放）。 | `Reset()` 内部调用；`CentralRegionPool::ReleaseRegionInternal()` 间接触发 (L192)。 |
| `Reset()` | 调用 `ReleaseBitmaps()` 后清零所有字段，`regionType` 设为 `FREE`。 | `CentralRegionPool::ReleaseRegionInternal()` (CentralRegionPool.cpp L192) — Region 归还空闲池。 |
| `Alloc()` | 统一分配入口：SWEPT 状态→`AllocFromBitmap()`；否则→bump-pointer 递增 `allocPtr`，同步更新 allocBitmap。 | `AllocBufferMS::FastAlloc()` (AllocBufferMS.cpp L78) — BUMP Region 分配热路径。 |
| `AllocFromBitmap()` | Bitmap 分配：从 `searchHint` 起 CTZ 扫描 allocBitmap 找空闲位，清除该位，`freeSlotCount--`。支持 wrap-around。 | `AllocBufferMS::FastAlloc()` (AllocBufferMS.cpp L82) — PARTIAL Region 分配；`Alloc()` 内 SWEPT 分支转发。 |
| `SweepRegion()` | 核心清扫：`allocBitmap[i] = ~markBitmap[i]` + mask trailing bits + `popcount` 统计存活 + `memset` 清零 markBitmap + 设 `SWEPT`。返回释放字节数。 | `CentralRegionPool::GetRegion()` lazy sweep (CentralRegionPool.cpp L58, L86)；`CentralRegionPool::SweepAndClassify()` 后台 sweep；`EnsureSwept()` 强制 sweep。 |
| `SetMarkBit(objAddr)` | 无条件原子 OR 设置 markBitmap 对应位。用于 mark-at-alloc。 | `RegionSpace::MarkObjectMS()` (RegionSpace.cpp L260) — 新分配对象标灰；`MarkSweepCollector::MarkNewObject()` mark-at-alloc。 |
| `GetMarkBit(objAddr)` | 原子 load 读取 markBitmap 对应位。用于判活查询。 | `RegionSpace::IsMarkedObjectMS()` (RegionSpace.cpp L160) — Finalizer/弱引用判活。 |
| `TestAndSetMarkBit(objAddr)` | ⭐ CAS 原子标记：含 slotSize==0 / 越界检查 / slotIdx 越界检查。返回 `true` 表示之前已标记（无需入队）。 | `MarkSweepCollector::MarkObject()` (MarkSweepCollector.cpp L220) — 并发标记核心路径（最高频调用）。 |
| `GetSweepGen()` | 返回 `sweepGen`。 | `CentralRegionPool::ReturnPartialRegion/ReturnFullRegion()` (CentralRegionPool.cpp L160, L173) — 判断归入 swept 还是 unswept 链表。 |
| `SetSweepGen(gen)` | 设置 `sweepGen`。 | `CentralRegionPool::GetRegion()` sweep 完成后 (L59, L87)；`GetFreshRegion()` 新 Region 初始化 (L142)。 |
| `CasSweepGen(expected, desired)` | `compare_exchange_strong` on `sweepGen`。用于 lazy sweep 所有权争夺。 | `CentralRegionPool::GetRegion()` lazy sweep 抢占 (L57, L85)；`EnsureSwept()` 强制 sweep (L246)。 |
| `IsCompletelyFree()` | `freeSlotCount == totalSlotCount`。全部死亡时释放 Region。 | `CentralRegionPool::GetRegion()` sweep 后判断释放 (L62, L90)；`SweepAndClassify()` 分类。 |
| `HasFreeSpace()` | SWEPT→检查 `freeSlotCount > 0`；非 SWEPT→额外检查 bump 尾部空间。 | `AllocBufferMS::ReturnRegion()` (AllocBufferMS.cpp L133) — 归还时分类路由；`CentralRegionPool::GetRegion()` sweep 后判断 (L95)。 |
| `GetSlotSize()` | `SizeClassInfo::ClassToSize(sizeClass)`。 | 内部方法调用（`GetAllocatedBytes/GetLiveBytes`）。 |
| `GetAllocatedBytes()` | `(totalSlotCount - freeSlotCount) * slotSize`。 | GC 统计信息收集。 |
| `GetLiveBytes()` | `liveSlotCount * slotSize`。 | GC 统计信息收集。 |

---

**`MSRegionInfoArray` 类** (MSRegionInfo.h L150-245):

> 堆前镜像数组管理器。`MSRegionInfo[i]` 对应 `heapStart + i * MS_REGION_SIZE` 处的 Region。数组通过 `malloc` 独立分配（避免与 CopyGC 的 UnitInfo 元数据重叠）。

**静态属性**:

| 属性 | 类型 | 说明 |
|------|------|------|
| `array_` | `MSRegionInfo*` | 镜像数组指针（`malloc` 分配）。 |
| `heapStartAddress_` | `uintptr_t` | 堆起始地址（第一个 Region 的起始）。 |
| `heapEndAddress_` | `uintptr_t` | 堆结束地址 = `heapStart + maxRegionCount * MS_REGION_SIZE`。 |
| `maxRegionCount_` | `size_t` | 最大 Region 数量。 |

**静态方法**:

| 方法 | 说明 | 主要使用位置 |
|------|------|-------------|
| `Init(heapStart, maxRegionCount)` | `malloc` 分配 `maxRegionCount * 64` 字节数组，`memset` 清零，所有条目初始化为 `MS_REGION_FREE` + `INVALID_IDX`。 | `RegionSpace::InitMS()` (RegionSpace.cpp L210) — 堆初始化时一次性调用。 |
| `GetRegionInfo(addr)` | ⭐ **O(1) 快速查找**：`array_[(addr - heapStart) / MS_REGION_SIZE]`。无分支，用于热路径。 | `CentralRegionPool::GetFreshRegion()` (L136) — 新 Region 地址→元数据映射。 |
| `GetRegionInfoValidated(addr)` | ⭐ **含对齐校正的安全查找**：先尝试直接索引，验证 `addr ∈ [RegionStart(), regionEnd)`；失败时对 LARGE 类型检查 `[allocPtr, regionEnd)` 和 span 条目；最后尝试相邻 idx±1（处理 UNIT_SIZE < MS_REGION_SIZE 对齐不匹配）。返回 `nullptr` 表示无法解析。 | `MarkSweepCollector::MarkObject()` (MarkSweepCollector.cpp L203) — 并发标记核心路径；`RegionSpace::MarkObjectMS()` (RegionSpace.cpp L240)；`RegionSpace::IsMarkedObjectMS()` (RegionSpace.cpp L147)。 |
| `GetRegionInfoByIdx(idx)` | O(1) 索引→MSRegionInfo。`return &array_[idx]`。 | `MSRegionList::PushFront/PopFront/Remove/ForEach()` (CentralRegionPool.h) — 链表操作通过 idx 访问节点；`MarkSweepCollector::MarkObject()` 大对象 baseIdx 回溯 (L213)。 |
| `GetRegionIdx(info)` | O(1) MSRegionInfo→索引。`return info - array_`。 | `MSRegionList::PushFront/Remove()` (CentralRegionPool.h) — 链表节点插入/删除时获取索引；`RegionSpace::RegisterLargeObjectMS()` 计算 baseIdx (L275)。 |
| `GetRegionAddress(idx)` | O(1) 索引→Region 起始地址。`return heapStart + idx * MS_REGION_SIZE`。 | `CentralRegionPool::ReleaseRegionInternal()` — 通过索引回溯 Region 物理地址（间接使用 `RegionStart()`）。 |
| `IsHeapAddress(addr)` | 检查地址是否在堆范围内。`addr ∈ [heapStart, heapEnd)`。 | `MarkSweepCollector::MarkObject()` (MarkSweepCollector.cpp L201) — 标记前快速过滤非堆地址。 |
| `GetMaxRegionCount()` | 返回 `maxRegionCount_`。 | `MarkSweepCollector::ResetMarkBitmaps()` — 遍历所有 Region 清零 markBitmap (L124)；`CentralRegionPool::PrepareForSweep()` 遍历分类 (L270)。 |
| `GetHeapStartAddress()` | 返回 `heapStartAddress_`。 | 调试日志输出。 |
| `Cleanup()` | `free(array_)` 释放整个镜像数组。 | 运行时销毁时调用（与 `Init` 配对）。 |

**MSRegionInfoArray 生命周期与使用路径**:

```
  Runtime启动                          运行时(热路径)                              Runtime销毁
  ──────────                          ──────────────                              ──────────
  Heap::Init()                        分配 / 标记 / 清扫 / 回收                    Cleanup()
      │                                   │                                          │
      v                                   v                                          v
  RegionSpace::InitMS()         ┌── GetRegionInfo(addr)           ──┐          free(array_)
      │                         │   GetRegionInfoValidated(addr)    │
      v                         │   GetRegionInfoByIdx(idx)         │
  MSRegionInfoArray::Init()     │   GetRegionIdx(info)              │
      │                         │   IsHeapAddress(addr)             │
      ├─ malloc(N * 64B)        │   GetMaxRegionCount()             │
      ├─ memset 清零             └──────────────────────────────────┘
      └─ 所有条目 → FREE
```

| 阶段 | 调用链 | 说明 |
|------|--------|------|
| **初始化** | `Heap::Init()` → `RegionSpace::InitMS()` (RegionSpace.cpp L210) → `MSRegionInfoArray::Init(heapStart, maxRegionCount)` | 运行时启动时**一次性调用**。`malloc` 分配 `maxRegionCount * 64` 字节，`memset` 清零后逐条目设 `regionType=FREE`, `nextRegionIdx=prevRegionIdx=INVALID_IDX`。此后 `array_` 指针和 `heapStartAddress_` 等静态成员在整个运行时生命周期内不变。 |
| **分配路径（地址→元数据）** | `CentralRegionPool::GetFreshRegion()` (L136) → `GetRegionInfo(regionStart)` | 从 RegionManager 获得新 Region 物理地址后，通过 `(addr - heapStart) / MS_REGION_SIZE` 快速定位到对应 MSRegionInfo 条目，随后调 `InitForSizeClass()` 初始化。**O(1) 无分支**，是分配路径的入口映射。 |
| **标记路径（对象→元数据，含校正）** | `MarkSweepCollector::MarkObject(obj)` (L203) → `GetRegionInfoValidated(addr)` → `TestAndSetMarkBit()` | 并发标记的**最高频路径**。先 `IsHeapAddress()` 过滤非堆地址，再 `GetRegionInfoValidated()` 获取 MSRegionInfo（含对齐校正）。对 LARGE 类型还通过 `GetRegionInfoByIdx(reserved)` 回溯到 base 条目。 |
| **标记路径（Barrier 侧）** | `RegionSpace::MarkObjectMS(obj)` (L240) → `GetRegionInfoValidated(addr)` | 写屏障 `MSWriteBarrier::MarkAndEnqueue()` 标灰旧/新引用时走此路径。逻辑与 Collector 侧相同，但在 Mutator 线程上执行。 |
| **判活路径** | `RegionSpace::IsMarkedObjectMS(obj)` (L147) → `GetRegionInfoValidated(addr)` → `GetMarkBit()` | Finalizer 判活、弱引用存活性检查时使用。LARGE 类型检查 `isMarked` 位，小对象检查 `markBitmap` 对应位。 |
| **清扫路径（遍历全部 Region）** | `MarkSweepCollector::ResetMarkBitmaps()` (L124) → `GetMaxRegionCount()` + `GetRegionInfoByIdx(i)` 循环 | GC MarkSetup 阶段遍历所有 Region 清零 markBitmap。通过 `maxRegionCount` 做上界，逐索引访问。 |
| **清扫路径（分类）** | `CentralRegionPool::PrepareForSweep()` (L270) → `GetMaxRegionCount()` + `GetRegionInfoByIdx(i)` | GC 代际翻转时遍历所有活跃 Region，按 `HasFreeSpace()` 分类到 partialUnswept / fullUnswept 链表。 |
| **链表操作（idx 互转）** | `MSRegionList::PushFront/PopFront/Remove/ForEach()` → `GetRegionInfoByIdx(idx)` + `GetRegionIdx(info)` | CentralRegionPool 四链表和 LargeRegionList 的所有操作都通过 idx↔MSRegionInfo* 互转。`PushFront` 时 `GetRegionIdx()` 获取新节点索引写入 `nextRegionIdx/prevRegionIdx`；`PopFront` 时 `GetRegionInfoByIdx()` 通过索引取节点。 |
| **回收路径（元数据→物理地址）** | `CentralRegionPool::ReleaseRegionInternal()` (L192) → `region->RegionStart()` → `RegionInfo::GetRegionInfoAt(regionStart)` | Region 全部死亡时，通过 MSRegionInfo 的 `RegionStart()` 回算物理地址，再映射到 CopyGC 层的 `RegionInfo` 归还 RegionManager。`GetRegionAddress(idx)` 提供同等能力但当前未直接使用。 |
| **销毁** | Runtime 析构 → `MSRegionInfoArray::Cleanup()` | `free(array_)` 释放整个镜像数组，`maxRegionCount_` 归零。与 `Init()` 配对。 |

---

**MSRegionInfo 生命周期**:

一个 MSRegionInfo 条目经历以下完整循环：

```
  ┌─────────────────────────────────────────────────────────────────────────────────┐
  │                             MSRegionInfo 生命周期                                │
  │                                                                                 │
  │  ① Init                ② 分配                 ③ GC标记              ④ 清扫       │
  │  ─────────────────     ──────────────         ──────────────       ──────────── │
  │  GetFreshRegion()      Alloc() bump           TestAndSetMarkBit   SweepRegion() │
  │  InitForSizeClass()    AllocFromBitmap()      SetMarkBit          HasFreeSpace  │
  │       │                     │                      │                   │        │
  │       v                     v                      v                   v        │
  │  [FREE] ──→ [BUMP] ──→ [FULL] ──→ (GC mark) ──→ [sweep] ──→ [PARTIAL] ──→ ... │
  │    ^                                                              │             │
  │    │                    ⑤ 回收                                     │             │
  │    │                    ──────────────                             │             │
  │    └── IsCompletelyFree() → Reset() → ReleaseRegionInternal() ←──┘             │
  └─────────────────────────────────────────────────────────────────────────────────┘
```

| 阶段 | 函数 | 行号 | 详细说明 |
|------|------|------|----------|
| **① 初始化** | `InitForSizeClass(cls)` | MSRegionInfo.cpp L24 | **触发**: `CentralRegionPool::GetFreshRegion()` 从 RegionManager 获取空闲 Region 后调用。**操作**: (1) 设 `regionType=BUMP`, `sizeClass=cls`；(2) 计算 `totalSlotCount = MS_REGION_SIZE / slotSize`；(3) `mmap` 分配 `allocBitmap + markBitmap` 连续内存（`bitmapWords * 8 * 2` 字节）；(4) `allocBitmap` 全置 1（全空闲），`markBitmap` 全置 0；(5) 若 `totalSlotCount % 64 != 0`，mask 末尾无效位。 |
| **① 初始化（大对象）** | `InitForLargeObject(start, end)` | MSRegionInfo.cpp L79 | **触发**: `RegionSpace::RegisterLargeObjectMS()` 注册堆内大对象时调用。**操作**: (1) 设 `allocPtr=start`, `regionEnd=end`, `regionType=LARGE`；(2) 不分配 bitmap（大对象用 `isMarked` 单标志位）；(3) `totalSlotCount=1`, `freeSlotCount=0`（已被大对象占满）。 |
| **② 分配（bump）** | `Alloc()` | MSRegionInfo.cpp L148 | **首轮（BUMP 状态）**: `allocPtr` 单调递增，每次推进 `slotSize` 字节。同步清 allocBitmap 对应位（`0`=已占）。`freeSlotCount--`。当 `allocPtr + slotSize > regionEnd` 时返回 0（Region 满）。**注意**: bump 阶段 allocBitmap 也同步更新，确保后续 sweep 后 bitmap 语义一致。 |
| **② 分配（bitmap）** | `AllocFromBitmap()` | MSRegionInfo.cpp L173 | **清扫后（SWEPT/PARTIAL 状态）**: 从 `reserved[低32位]`（searchHint）指示的 word 开始，用 `__builtin_ctzll()` 找首个为 1 的位（空闲槽），清除该位（= 已占），`freeSlotCount--`。若当前 word 后续无空闲则递增 hint。搜到 wordCount 末尾后 **wrap-around** 从 word 0 重扫（避免漏扫前段回收的槽位）。 |
| **③ 标记（CAS）** | `TestAndSetMarkBit(objAddr)` | MSRegionInfo.cpp L271 | **并发标记核心**。**操作**: (1) 计算 `slotIdx = (objAddr - RegionStart()) / slotSize`；(2) 含 3 级防护：`slotSize==0` 检查、`addr ∉ [regionStart, regionEnd)` 检查、`slotIdx >= totalSlotCount` 检查——任一失败记 error log 返回 false；(3) `__atomic_fetch_or(&markBitmap[wordIdx], bit, RELAXED)` 原子设位；(4) 返回旧值 `(old & bit) != 0`——true 表示之前已标记（重复标记，调用方跳过入队）。 |
| **③ 标记（无条件）** | `SetMarkBit(objAddr)` | MSRegionInfo.cpp L251 | **mark-at-alloc 专用**。无边界检查，直接 `__atomic_or_fetch` 设位。用于 GC 期间新分配对象标灰（alloc-black），因调用方已保证地址合法。 |
| **③ 标记（查询）** | `GetMarkBit(objAddr)` | MSRegionInfo.cpp L261 | `__atomic_load_n` 读取 markBitmap 对应位。用于 `IsMarkedObjectMS()` 判活（Finalizer/弱引用存活性检查）。 |
| **④ 清扫** | `SweepRegion()` | MSRegionInfo.cpp L216 | **核心清扫算法**: (1) 遍历每个 bitmap word：`allocBitmap[i] = ~markBitmap[i]`（标记了=存活=不可分配，未标记=死亡=可分配）；(2) `popcount(markBitmap[i])` 累加存活计数；(3) mask 末尾无效位；(4) `memset(markBitmap, 0, ...)` 为下轮 GC 清零 markBitmap；(5) 更新 `liveSlotCount`, `freeSlotCount = total - live`；(6) 设 `sweepState = SWEPT`；(7) 重置 searchHint=0。**返回**: 释放的字节数 `freeSlotCount * slotSize`。 |
| **④ 查询** | `HasFreeSpace()` | MSRegionInfo.cpp L304 | **SWEPT 状态**: 直接检查 `freeSlotCount > 0`（bitmap 清扫后已准确）。**非 SWEPT（BUMP）状态**: 还检查 bump 尾部 `allocPtr + slotSize <= regionEnd`（bump 未耗尽）。用于 `AllocBufferMS::ReturnRegion()` 分类路由和 `CentralRegionPool::GetRegion()` lazy sweep 后判断。 |
| **⑤ 回收** | `ReleaseBitmaps()` | MSRegionInfo.cpp L106 | `munmap(allocBitmap, bitmapBytes * 2)` 一次释放双 bitmap（alloc+mark 连续分配）。置 `allocBitmap = markBitmap = nullptr`。 |
| **⑤ 回收** | `Reset()` | MSRegionInfo.cpp L122 | 完整重置：调 `ReleaseBitmaps()` → 清零所有字段（allocPtr, regionEnd, idx, counts, flags） → `regionType = FREE`。**触发**: `CentralRegionPool::ReleaseRegionInternal()` — 当 `IsCompletelyFree()` 为 true（全部死亡）或外部主动释放时，调 `Reset()` 后将物理 Region 归还 `RegionManager::ReclaimRegion()`。 |

#### allocBitmap 与 markBitmap 的关系及编译器 Fast-Path 影响

**为什么需要两个 bitmap？**

`allocBitmap` 和 `markBitmap` 在语义上完全独立，不可合并：

| | `allocBitmap` | `markBitmap` |
|---|---|---|
| **含义** | `1`=空闲可分配, `0`=已占用 | `1`=GC 标记存活, `0`=未标记 |
| **谁写** | 分配器（分配时清 bit） | GC 标记器（标记时设 bit） |
| **何时写** | mutator 运行期间 | GC 标记阶段 |
| **何时读** | `AllocFromBitmap()` 找空闲 slot | `SweepRegion()` 计算存活数 |

核心原因在于**两次 GC 之间有新分配**。`SweepRegion()` 以 `allocBitmap[i] = ~markBitmap[i]` **无条件覆写** allocBitmap，完全由 markBitmap 推导。但清扫完成后 mutator 继续分配新对象（清 allocBitmap 位），这些新分配反映在 allocBitmap 而 **不在 markBitmap** 中。若合并为一个 bitmap，下次 GC 的 `~markBitmap` 覆写会丢失新分配信息，导致已分配 slot 被误判为空闲。

**BUMP 阶段 allocBitmap 同步是防御性编程，非正确性必需**

当前 `Alloc()` 在 BUMP 阶段同步更新 allocBitmap（L160-163），但这不是正确性要求。因为：
1. BUMP 阶段结束后，Region 进入 FULL 状态等待 GC
2. GC 清扫时 `SweepRegion()` 执行 `allocBitmap[i] = ~markBitmap[i]` 无条件覆写，不依赖旧的 allocBitmap
3. 清扫后 Region 进入 PARTIAL 状态，后续分配走 `AllocFromBitmap()`，此时 allocBitmap 已被 sweep 正确重建

因此即使 BUMP 阶段完全不碰 allocBitmap，整个流程仍然正确。

**对编译器 Fast-Path 的影响**

这意味着 Phase 2 编译器内联 BUMP 分配可以**只操作 `allocPtr` 和 `regionEnd`**（offset 0 和 offset 8），无需碰 bitmap、freeSlotCount 或任何其他字段：

```asm
; r0 = &MSRegionInfo (allocPtr at +0, regionEnd at +8)
ldr  x1, [r0]           ; x1 = allocPtr
add  x2, x1, slotSize   ; x2 = allocPtr + slotSize
ldr  x3, [r0, #8]       ; x3 = regionEnd
cmp  x2, x3
b.gt slow_path           ; 满了 → runtime 慢路径
str  x2, [r0]            ; allocPtr = new value
; x1 = 已分配地址
```

| Region 状态 | 编译器能内联？ | 需要碰 allocBitmap？ | 说明 |
|-------------|------------|-----------------|------|
| **BUMP** | ✅ 可以 | ❌ 不需要 | sweep 会从 markBitmap 重建 |
| **PARTIAL/SWEPT** | 理论可以（CTZ 内联），目前未实现 | ✅ 必须 | allocBitmap 是唯一的空闲 slot 来源 |

> 这也解释了 `allocPtr` 必须在 offset 0 的设计——编译器内联只需知道结构体前 16 字节（allocPtr + regionEnd），与其余字段完全解耦。

#### Review 重点
- ⚠️ `GetRegionInfoValidated`: 地址对齐校正启发式——当 `UNIT_SIZE(16KB) < MS_REGION_SIZE(32KB)` 时，奇数 unit 分配的 Region 未对齐到 32KB 边界，对象可能映射到错误的 MSRegionInfo 条目。当前通过检查 `addr ∈ [RegionStart(), regionEnd)` 并尝试相邻条目(idx±1)修正，但极端情况下仍可能误判。
- ⚠️ `MS_REGION_SPAN(5)` 枚举值已定义但未使用（之前 SPAN 方案失败后保留）

#### RegionManager 依赖分析与精简评估

> **背景**: MS 模式目前复用 CopyGC 的 `RegionManager` + `RegionInfo` 做物理 region 分配/回收。`MSRegionInfoArray` 被 `malloc` 而非放入 mmap 前段，根本原因是 mmap 前段已被 `RegionInfo[]` 占据。本节评估是否可以用轻量级 MS 专用分配器替代 `RegionManager`。

##### 一、MS 模式实际使用的接口（5 + 4 = 9 个）

**RegionManager 接口（5 个）**

| 接口 | 调用位置 | 用途 |
|------|----------|------|
| `Initialize(unitNum, metadata)` | `RegionSpace.cpp:197` | 初始化 mmap 布局，设置 `regionInfoStart` / `regionHeapStart` |
| `GetRegionHeapStart()` | `RegionSpace.cpp:198` | 获取堆起始地址 |
| `TakeRegion(n, UnitRole, expectPhysicalMem)` | `CentralRegionPool.cpp:122`, `RegionSpace.cpp:39` | **核心分配原语** — 从空闲池取 n 个 unit |
| `ReclaimRegion(RegionInfo*)` | `CentralRegionPool.cpp:197`, `CentralRegionPool.cpp:456` | **核心回收原语** — 归还 region 到空闲池 |
| `GetLargeObjectThreshold()` | `RegionSpace.cpp:32` | Tier1 大对象阈值（16KB） |

**RegionInfo 接口（4 个）**

| 接口 | 调用位置 | 用途 |
|------|----------|------|
| `RegionInfo::GetRegionInfoAt(addr)` | `CentralRegionPool.cpp:198`, `CentralRegionPool.cpp:455` | 地址 → RegionInfo* 反查（ReclaimRegion 前） |
| `rawRegion->GetRegionStart()` | `CentralRegionPool.cpp:130` | TakeRegion 后获取 region 物理起始地址 |
| `rawRegion->Alloc(size)` | `RegionSpace.cpp:48` | Tier1 大对象在 RegionInfo 上 bump 分配 |
| `RegionInfo::UNIT_SIZE` | `CentralRegionPool.cpp:120` | 常量 16KB，计算 `unitsPerMSRegion = 32KB / 16KB = 2` |

**灰色地带接口（未 ifdef 保护，MS Collector 当前未调用但虚函数存在）**

| 接口 | 位置 | 说明 |
|------|------|------|
| `ReclaimGarbageMemory()` | `RegionSpace.h:109` | 委托 `regionManager.ReclaimGarbageRegions()`，MS Collector 未调用 |
| `ForEachObj()` | `RegionSpace.h:144` | 委托 `regionManager.ForEachObj*()`，MS 模式需要自己的遍历实现 |
| `GetAllocatedSize()` | `RegionSpace.cpp:142` | MS 模式已覆盖为 `msCentralPool_->GetTotalMSRegionBytes()` |

##### 二、RegionManager 内部实现复杂度

`TakeRegion()` 内部做了非常多的事：

1. **启发式 GC 触发** — 检查 `allocated >= threshold` 触发 HeuGC
2. **分配限速** — `RequestForRegion()` 根据余弦函数计算 sleep 时间，避免分配速度超过回收速度
3. **优先复用垃圾 region** — 从 `garbageRegionList` 取（避免物理内存增长）
4. **FreeRegionManager** — 用 **CartesianTree（笛卡尔树）** 管理空闲 unit，分 dirty/released 两棵树
5. **inactiveZone 扩展** — 最后从未使用区域 `fetch_add` 扩展

`ReclaimRegion()` 相对简单 — 清理 RegionInfo + `freeRegionManager.AddGarbageUnits(idx, num)`。

`RegionInfo` 本身是 ~120 字节的结构体，大量字段（routeState, liveInfo, markBitmap, rawPointerCount, ghostRegion 等）全是 CopyGC 特有的，MS 模式只用到 `allocPtr`, `regionEnd`, `unitIdx ↔ addr` 映射。

##### 三、精简方案对比

**方案 A：`MSPageAllocator` 完全替代 RegionManager（大改）**

```
MSPageAllocator {
    mmap 一块大内存
    前段放 MSRegionInfo[]（替代独立 malloc）
    后段是堆空间
    用简单 bitmap/freelist 管理 32KB region 的分配/释放
}
```

| 优点 | 代价 |
|------|------|
| 消除 RegionInfo 120B × N 的元数据浪费 | 需要改 `RegionSpace::Init` 的 mmap 布局计算 |
| MSRegionInfoArray 可嵌入 mmap 前段 | 需从 TakeRegion 中提取 HeuGC 触发 + 限速逻辑 |
| 代码干净，无 CopyGC 包袱 | 需实现 region 分配器（CartesianTree 或 bitmap） |
| | ~400-600 行新代码 + ~200 行改动 |

**方案 B：`MSRegionManager` 薄包装（小改）**

```cpp
class MSRegionManager {
    RegionManager& rm_;
public:
    uintptr_t TakeRegion32K() {
        RegionInfo* ri = rm_.TakeRegion(2, SMALL_SIZED_UNITS, true);
        return ri ? ri->GetRegionStart() : 0;
    }
    void ReclaimRegion(uintptr_t addr) {
        RegionInfo* ri = RegionInfo::GetRegionInfoAt(addr);
        rm_.ReclaimRegion(ri);
    }
};
```

| 优点 | 缺点 |
|------|------|
| 改动量极小（~50 行） | RegionInfo 120B 浪费仍在 |
| CentralRegionPool 只依赖薄接口 | MSRegionInfoArray 仍 malloc |
| 风险低 | |

**方案 C：当前状态 + ifdef 隔离（最小改动）**

给 `ReclaimGarbageMemory()` / `ForEachObj()` 加 `#ifdef MRT_USE_MARK_SWEEP` 分支。

##### 四、结论

| 维度 | 方案 A | 方案 B | 方案 C |
|------|--------|--------|--------|
| 代码干净度 | ★★★★★ | ★★★ | ★★ |
| 改动量 | **大**（~800 行） | **小**（~50 行） | **极小** |
| 风险 | **高** | **低** | **无** |
| 消除 RegionInfo 浪费 | ✅ | ❌ | ❌ |
| MSRegionInfoArray 入 mmap | ✅ | ❌ | ❌ |

**建议路径：C → B → A 渐进式**。当前阶段（MS GC 验证期）用方案 C，确保功能正确。稳定后抽取方案 B 隔离接口。最终性能调优阶段再考虑方案 A。

核心原因：MS 模式实际只需要 5 个 RegionManager 方法，但 `TakeRegion` 内部的 HeuGC 触发 + 限速 + CartesianTree 空闲管理不是几十行能复刻的，直接替换风险大。

### Step 1.2: SizeClass 与线程本地分配

> 设计文档: `doc/Step1.2_SizeClass与线程本地分配设计.md`
>
> **目标**: 36 级 SizeClass 表 + 线程本地 `AllocBufferMS` + 全局 `CentralRegionPool` 四链表设计。

#### 新增文件

| 文件 | 行数 | 说明 |
|------|------|------|
| `src/Heap/Allocator/SizeClass.h` | 164 | 36 级 SizeClass 定义（8B→16KB），常量表 `kSizeClasses[]` |
| `src/Heap/Allocator/SizeClass.cpp` | 77 | `SizeClassInfo::Init()` — 构建 size→class 查表 + 验证 |
| `src/Heap/Allocator/AllocBufferMS.h` | 87 | `AllocBufferMS` 类：per-thread per-sizeclass 活跃 Region 缓存 |
| `src/Heap/Allocator/AllocBufferMS.cpp` | 181 | 分配热路径实现 |
| `src/Heap/Allocator/CentralRegionPool.h` | 281 | `MSRegionList` 侵入式链表 + `SizeClassBucket` + `CentralRegionPool` 主类 |
| `src/Heap/Allocator/CentralRegionPool.cpp` | 457 | 全局 Region 池管理 + lazy sweep + 大对象管理 |

#### 修改文件

| 文件 | 修改行 | 改动要点 |
|------|--------|----------|
| `src/Heap/Allocator/RegionSpace.h` L27-30, L192-203, L238-268 | `#ifdef` 块: 声明 `AllocBufferMS*` 成员、`InitMS()`、MS 分配/标记接口 |
| `src/Heap/Allocator/RegionSpace.cpp` L18-20, L29-62, L201-227 | `#ifdef` 块: `InitMS()` 初始化 MSRegionInfoArray + CentralRegionPool + AllocBufferMS；`Allocate()` 中增加 MS 分支 |
| `src/Heap/Heap.cpp` L158(等) | `#ifdef` 块: 调用 `SizeClassInfo::Init()` + `RegionSpace::InitMS()` |

---

#### 1. SizeClassInfo 类（SizeClass.h / SizeClass.cpp）

> 对应 Go runtime `msize.go` 的 size-class 表。提供 36 级 SizeClass 常量 + O(1) 双表查询。

**全局常量**:

| 常量 | 值 | 说明 | 主要使用位置 |
|------|---|------|-------------|
| `MS_REGION_SIZE` | `32 * 1024` (32KB) | MS 模式 Region 固定大小 | MSRegionInfo.h:102,157,163; CentralRegionPool.cpp:120,139,145; RegionSpace.cpp:208,280 |
| `kNumSizeClasses` | `36` | SizeClass 级数 | AllocBufferMS.h:75; CentralRegionPool.h:255,268; CentralRegionPool.cpp:210,275,295 |
| `kSizeClasses[36]` | `{8,16,24,...,16384}` | 索引→slot 大小映射表 | SizeClass.cpp:24(Init 内循环); SizeClassInfo::ClassToSize() 内联引用 |
| `kLargeObjectThreshold` | `16384` (16KB) | 大对象阈值，超过走 Tier1/Tier2 | RegionSpace.cpp:40(分配判断) |
| `kHugeObjectThreshold` | `4 * 1024 * 1024` (4MB) | 超大对象阈值，超过走 Tier2 mmap | RegionSpace.cpp:40(分配判断) |
| `kSmallLookupBound` | `1024` | 小对象查表上界 | SizeToClass() 内分支判断 |
| `kSmallGranularity` | `8` | 小对象查表粒度（8B 对齐） | SizeToClass() 内索引计算 |
| `kLargeGranularity` | `256` | 大对象查表粒度 | SizeToClass() 内索引计算 |
| `kSmallLookupEntries` | `129` | 小对象查表数组长度 | smallLookup_[] 定义 |
| `kLargeLookupEntries` | `~61` | 大对象查表数组长度 | largeLookup_[] 定义 |

**SizeClassInfo 静态属性**:

| 属性 | 类型 | 说明 | 主要使用位置 |
|------|------|------|-------------|
| `smallLookup_[129]` | `uint8_t[]` | size/8 → class 快速查表（size ≤ 1024） | SizeToClass() 内联读取 |
| `largeLookup_[~61]` | `uint8_t[]` | (size-1024)/256 → class 快速查表（1024 < size ≤ 16384） | SizeToClass() 内联读取 |
| `slotsPerRegion_[36]` | `size_t[]` | 每级 SizeClass 在一个 Region 中可容纳的 slot 数 | SlotsPerRegion() / BitmapWordsPerRegion() 内联读取; MSRegionInfo::InitForSizeClass() |

**SizeClassInfo 静态方法**:

| 方法 | 说明 | 主要使用位置 |
|------|------|-------------|
| `Init()` | 构建 smallLookup_ / largeLookup_ / slotsPerRegion_，Debug 模式下做全范围验证 | RegionSpace.cpp:204 (`InitMS` 中调用) |
| `SizeToClass(size)` | O(1) size→class 查表：size≤1024 走 smallLookup_，否则走 largeLookup_ | AllocBufferMS.cpp:51 (`Allocate` 入口) |
| `ClassToSize(cls)` | O(1) class→slot 大小：直接索引 `kSizeClasses[cls]` | MSRegionInfo.h:139(`GetSlotSize`); MSRegionInfo.cpp:30,156,175,243,253,263,273,314,320,326 |
| `SlotsPerRegion(cls)` | O(1) 返回 `slotsPerRegion_[cls]` = `MS_REGION_SIZE / ClassToSize(cls)` | MSRegionInfo.cpp (`InitForSizeClass` 中初始化 totalSlots) |
| `BitmapWordsPerRegion(cls)` | 计算 `(SlotsPerRegion(cls) + 63) / 64`，返回 bitmap 所需 uint64_t 字数 | MSRegionInfo.cpp (`InitForSizeClass` 中分配 bitmap) |

**SizeClassInfo 生命周期与使用路径**:

```
①Init(启动)                    ②分配热路径                    ③Region初始化
Heap.cpp:InitMS()              AllocBufferMS::Allocate()     MSRegionInfo::InitForSizeClass()
  │                              │                              │
  └→ SizeClassInfo::Init()       └→ SizeToClass(size) → cls     ├→ SlotsPerRegion(cls) → totalSlots
       ├→ 填充 slotsPerRegion_[]      (O(1) 双表查询)           └→ BitmapWordsPerRegion(cls) → bitmap大小
       ├→ 填充 smallLookup_[]
       ├→ 填充 largeLookup_[]    ③Region/对象信息查询
       └→ Debug 全范围验证        MSRegionInfo::GetSlotSize()
                                   └→ ClassToSize(cls)
```

| 阶段 | 函数 | 详细说明 |
|------|------|---------|
| ①启动初始化 | `SizeClassInfo::Init()` | Runtime 启动时由 `Heap::InitMS()` → `RegionSpace::InitMS()` 最先调用。遍历 36 级填充 `slotsPerRegion_[]`，构建双级查表 `smallLookup_[129]` + `largeLookup_[~61]`。Debug 模式下验证 `ClassToSize(SizeToClass(n)) >= n` 对所有 n∈[1,16384] 成立 |
| ②分配查表 | `SizeToClass(size)` | 每次对象分配的第一步。`AllocBufferMS::Allocate(size)` 调用，O(1) 返回 size 对应的 class 索引 |
| ③Region 初始化 | `SlotsPerRegion(cls)` / `BitmapWordsPerRegion(cls)` | 新 Region 从 `CentralRegionPool::GetFreshRegion()` 分配时，`MSRegionInfo::InitForSizeClass(cls)` 调用这两个方法确定 slot 数和 bitmap 大小 |
| ④信息查询 | `ClassToSize(cls)` | 贯穿整个生命周期：分配时计算 slot 偏移、标记时验证对象边界、清扫时统计回收字节 |

> **注意**: `SizeClassInfo` 是纯静态工具类，无实例状态，Init 后所有查表数组在整个 runtime 生命周期内只读不变。

---

#### 2. AllocBufferMS 类（AllocBufferMS.h / AllocBufferMS.cpp）

> 对应 Go runtime `mcache` — 每个 P（Processor）持有一个，每级 SizeClass 缓存一个活跃 Region。Phase 1 为全局单实例，Phase 2 需改为 per-Processor TLS。

**AllocBufferMS 属性**:

| 属性 | 类型 | 说明 | 主要使用位置 |
|------|------|------|-------------|
| `activeRegions_[36]` | `MSRegionInfo*[]` | 每级 SizeClass 的当前活跃 Region（nullptr = 无缓存） | FastAlloc():读取当前 Region; SlowAlloc():设置新 Region; ReturnRegion():归还并置 nullptr; FlushAll():遍历归还 |
| `central_` | `CentralRegionPool*` | 反向指针，指向全局 CentralRegionPool | RefillFromCentral():调 GetRegion/GetFreshRegion; ReturnRegion():调 ReturnPartial/Full/Release |
| `allocCount_` | `size_t` | 分配计数器（用于周期性 trace 日志） | Allocate():每次+1 |

**AllocBufferMS 方法**:

| 方法 | 说明 | 主要使用位置 |
|------|------|-------------|
| `Init(central)` | 初始化：绑定 CentralRegionPool，清空 activeRegions_ | RegionSpace.cpp:218 (`InitMS` 中调用) |
| `Fini()` | 析构前清理：调 FlushAll() 归还所有缓存 Region，置 central_=nullptr | ~AllocBufferMS() 析构函数调用 |
| `Allocate(size)` | **分配入口**：SizeToClass → FastAlloc → (mark-at-alloc) | RegionSpace.cpp:64 (`Allocate` MS 分支) |
| `FastAlloc(cls)` | **快速路径**：尝试在 activeRegions_[cls] 上 bump/bitmap 分配；失败则调 SlowAlloc | Allocate() 内部调用 |
| `SlowAlloc(cls)` | **慢速路径**：ReturnRegion(旧) → RefillFromCentral(新) → 分配 | FastAlloc() 失败时调用 |
| `RefillFromCentral(cls)` | 从 CentralRegionPool 获取 Region：先 GetRegion(四链表)，再 GetFreshRegion(新 Region) | SlowAlloc() 内部调用 |
| `ReturnRegion(cls)` | 归还当前 Region：完全空闲→Release，部分空闲→Partial，已满→Full | SlowAlloc()/FlushAll() 调用 |
| `FlushAll()` | GC STW 时归还所有 36 级 activeRegions_ | MarkSweepCollector.cpp:118 |
| `GetActiveRegion(cls)` | 返回指定 SizeClass 的当前活跃 Region（用于 GC 迭代） | 内联方法，供 GC 遍历 |

**AllocBufferMS 生命周期与使用路径**:

```
①创建(InitMS)         ②分配热路径(Allocate)              ③GC STW(FlushAll)      ④销毁(Fini)
RegionSpace::InitMS()  RegionSpace::Allocate(size)        MarkSweepCollector      ~AllocBufferMS()
  │                      │                                  ::MarkSetup()            │
  ├→ new AllocBufferMS() └→ msAllocBuffer_->Allocate(size)   │                      └→ Fini()
  └→ Init(centralPool)      │                               └→ FlushAll()               ├→ FlushAll()
       └→ central_=pool     ├→ SizeToClass(size) → cls          └→ 36次                 └→ central_=null
          activeRegions_=0  ├→ FastAlloc(cls)                       ReturnRegion(cls)
                            │   ├→ BUMP: Alloc()
                            │   └→ PARTIAL: AllocFromBitmap()
                            ├→ miss → SlowAlloc(cls)
                            │   ├→ ReturnRegion(cls旧)
                            │   │   ├→ 完全空闲→ ReleaseRegion
                            │   │   ├→ 部分空闲→ ReturnPartial
                            │   │   └→ 已满    → ReturnFull
                            │   └→ RefillFromCentral(cls)
                            │       ├→ central_->GetRegion(cls)
                            │       └→ central_->GetFreshRegion(cls)
                            └→ mark-at-alloc (GC标记阶段)
                                └→ RegionSpace::MarkObject(obj)
```

| 阶段 | 函数 | 详细说明 |
|------|------|---------|
| ①创建 | `new AllocBufferMS()` + `Init(central)` | `RegionSpace::InitMS()` 中创建。绑定 CentralRegionPool 指针，36 个 activeRegions_ 全置 nullptr |
| ②快速分配 | `FastAlloc(cls)` | 检查 activeRegions_[cls] 是否存在，BUMP Region 调 `Alloc()` bump-pointer，PARTIAL Region 调 `AllocFromBitmap()` CTZ 扫描 |
| ②慢速分配 | `SlowAlloc(cls)` | 当前 Region 耗尽。先 ReturnRegion 归还旧 Region 到 CentralRegionPool 四链表，再 RefillFromCentral 获取新 Region |
| ②mark-at-alloc | `Allocate()` 尾部 | GC 并发标记阶段（MARK_SETUP / CONCURRENT_MARK / MARK_TERMINATION），新分配对象标黑以确保本轮存活 |
| ③GC 刷新 | `FlushAll()` | MarkSweepCollector GC 启动的 MarkSetup 阶段调用 `FlushAllWriteBarrierBuffers()` → `FlushAll()`。遍历 36 级，每级 ReturnRegion 归还到 CentralRegionPool |
| ④销毁 | `~AllocBufferMS()` → `Fini()` | Runtime 关闭时析构。先 FlushAll 归还所有 Region，再清空 central_ 指针 |

---

#### 3. CentralRegionPool 及辅助类（CentralRegionPool.h / CentralRegionPool.cpp）

> 对应 Go runtime `mcentral` — 全局 Region 池，per-SizeClass 四链表管理 + Lazy Sweep + 后台清扫。

##### 3.1 辅助类: MSRegionList

> 侵入式双向链表，利用 MSRegionInfo 的 `prevRegionIdx` / `nextRegionIdx` 字段串联。调用方需持有外层 bucket.lock，链表自身无锁。

**MSRegionList 属性**:

| 属性 | 类型 | 说明 |
|------|------|------|
| `headIdx_` | `int32_t` | 链表头节点的 MSRegionInfoArray 索引（INVALID_IDX = -1 表示空） |
| `count_` | `size_t` | 当前链表中 Region 数量 |

**MSRegionList 方法**:

| 方法 | 说明 | 主要使用位置 |
|------|------|-------------|
| `Prepend(region)` | 头插：O(1) 将 Region 插入链表头部 | CentralRegionPool 各 Return/Sweep 路径中归还 Region 到四链表 |
| `TakeHead()` | 取头：O(1) 弹出链表头部 Region 并返回（空返回 nullptr） | GetRegion() 取 partialSwept/partialUnswept/fullUnswept 头部; ForceFinishSweep(); SweepUnsweptList() |
| `Remove(region)` | 按节点删除：O(1) 从链表中摘除指定 Region | SweepLargeRegions() 中移除死亡大对象 Region |
| `IsEmpty()` | 判空：`headIdx_ == INVALID_IDX` | 内部逻辑判断 |
| `Count()` | 返回当前 Region 数量 | DumpPoolStats(), GetTotalPartialCount(), GetTotalFullCount() |
| `Swap(other)` | O(1) 交换两个链表的全部内容（headIdx_ + count_） | PrepareForSweep() 中 swept ↔ unswept 代际翻转 |
| `ForEach(visitor)` | 遍历链表，对每个 Region 调用 visitor 回调。**遍历中不可修改链表** | VisitAllRegions() GC 根枚举; ClearLargeMarks(); SweepLargeRegions() 收集死亡 Region |

##### 3.2 辅助结构: MSSweepStats

> 清扫统计信息，跨一个 GC 周期累积，下一轮 PrepareForSweep() 时重置。

| 字段 | 类型 | 说明 | 主要使用位置 |
|------|------|------|-------------|
| `bgSweptRegions` | `atomic<size_t>` | 后台清扫线程清扫的 Region 数 | BackgroundSweep() 中累加; MarkSweepCollector.cpp:727 读取打印 |
| `lazySweptRegions` | `atomic<size_t>` | Lazy Sweep（分配时清扫）的 Region 数 | GetRegion() 中累加; MarkSweepCollector.cpp:727 读取打印 |
| `forceSweptRegions` | `atomic<size_t>` | ForceFinishSweep 强制清扫的 Region 数 | SweepAndClassify() 中累加; MarkSweepCollector.cpp:727 读取打印 |
| `sweptFreedBytes` | `atomic<size_t>` | 小对象清扫释放的总字节数 | 清扫路径累加; MarkSweepCollector 读取 |
| `releasedRegions` | `atomic<size_t>` | 完全空闲后释放回 OS 的 Region 数 | 各清扫路径中 IsCompletelyFree() 后累加 |
| `Reset()` | 方法 | 重置所有计数器为 0 | PrepareForSweep() 开头调用 |
| `TotalSweptRegions()` | 方法 | 返回三种清扫数之和 | 统计报告使用 |

##### 3.3 辅助结构: SizeClassBucket（CentralRegionPool 内部）

> 每个 SizeClass 一个 bucket，包含互斥锁 + 四链表。共 36 个 bucket 数组。

| 字段 | 类型 | 说明 |
|------|------|------|
| `lock` | `std::mutex` | 保护本 bucket 四链表的并发访问 |
| `partialSwept` | `MSRegionList` | 有空闲槽位、已完成清扫的 Region（分配首选） |
| `fullSwept` | `MSRegionList` | 无空闲槽位、已完成清扫的 Region |
| `partialUnswept` | `MSRegionList` | 有空闲槽位、未清扫的 Region（需 lazy sweep） |
| `fullUnswept` | `MSRegionList` | 无空闲槽位、未清扫的 Region（需 lazy sweep） |

##### 3.4 CentralRegionPool 属性

| 属性 | 类型 | 说明 | 主要使用位置 |
|------|------|------|-------------|
| `globalSweepGen_` | `static atomic<uint32_t>` | 全局清扫代计数器（初始值 2）。每轮 GC +2 实现 O(1) 代际翻转 | PrepareForSweep() +2; GetRegion() 读取判 swept/unswept; MarkSweepCollector.cpp:711,773 读取 |
| `sweepDone_` | `atomic<bool>` | 后台清扫是否完成标志 | BackgroundSweep() 尾部置 true; ForceFinishSweep() 尾部置 true; PrepareForSweep() 置 false |
| `sweepStats_` | `MSSweepStats` | 本轮 GC 清扫统计（见 3.2） | 各清扫路径累加; MarkSweepCollector.cpp:727 读取打印 |
| `totalMSRegionBytes_` | `atomic<size_t>` | 当前 MS Region 总占用字节（用于 GC 触发阈值） | GetFreshRegion() +MS_REGION_SIZE; ReleaseRegionInternal() -MS_REGION_SIZE; AddLargeRegionBytes/SubLargeRegionBytes; RegionSpace.cpp:139 读取 |
| `buckets_[36]` | `SizeClassBucket[]` | 36 级 SizeClass 的四链表 bucket 数组 | GetRegion/Return*/ForceFinishSweep/BackgroundSweep/PrepareForSweep 等全部操作 |
| `regionManager_` | `RegionManager*` | 指向底层 RegionManager（管理物理 Region 分配/回收） | GetFreshRegion() 调 TakeRegion(); ReleaseRegionInternal() 调 ReclaimRegion() |
| `largeLock_` | `std::mutex` | 保护大对象 Region 链表的并发访问 | RegisterLargeRegion/ClearLargeMarks/SweepLargeRegions |
| `largeRegionList_` | `MSRegionList` | Tier1 堆内大对象 Region 链表 | RegisterLargeRegion() 添加; SweepLargeRegions() 遍历+移除死亡; ClearLargeMarks() 清标记 |

##### 3.5 CentralRegionPool 方法

| 方法 | 说明 | 主要使用位置 |
|------|------|-------------|
| `CentralRegionPool(regionManager)` | 构造：绑定 RegionManager 指针 | RegionSpace.cpp:212 (`InitMS` 中创建) |
| **Region 获取/归还** | | |
| `GetRegion(cls)` | 四级优先级获取：①partialSwept → ②partialUnswept(CAS lazy sweep) → ③fullUnswept(CAS lazy sweep) → ④返回 nullptr | AllocBufferMS.cpp:159 (`RefillFromCentral`) |
| `GetFreshRegion(cls)` | 从 RegionManager 分配全新 Region → InitForSizeClass → 设 sweepGen | AllocBufferMS.cpp:165 (`RefillFromCentral` 备选) |
| `ReturnPartialRegion(region, cls)` | 有空闲 Region 归还：按 sweepGen 放入 partialSwept 或 partialUnswept | AllocBufferMS.cpp:140 (`ReturnRegion` 部分空闲分支) |
| `ReturnFullRegion(region, cls)` | 已满 Region 归还：按 sweepGen 放入 fullSwept 或 fullUnswept | AllocBufferMS.cpp:143 (`ReturnRegion` 已满分支) |
| `ReleaseRegion(region, cls)` | 完全空闲 Region 释放：调 ReleaseRegionInternal 回收 | AllocBufferMS.cpp:137 (`ReturnRegion` 完全空闲分支) |
| **GC 操作** | | |
| `ForceFinishSweep()` | 强制完成所有未清扫 Region（Drain partialUnswept + fullUnswept） | MarkSweepCollector.cpp:108 (GC 启动前) |
| `PrepareForSweep()` | O(1) 代际翻转：Reset 统计 → Swap swept↔unswept → globalSweepGen_+2 | MarkSweepCollector.cpp:635 (MarkTerminate 后) |
| `BackgroundSweep()` | 后台清扫：遍历 36 级，SweepUnsweptList 清扫 unswept 链表 | FinalizerProcessor.cpp:314 (gc-helper 线程) |
| `IsSweepDone()` | 检查后台清扫是否完成 | 内部调度判断 |
| `VisitAllRegions(visitor)` | 遍历 36 级×4 链表，对每个 Region 调用 visitor（用于 GC 清标记等） | MarkSweepCollector.cpp:126 (清 markBitmap) |
| **大对象管理** | | |
| `RegisterLargeRegion(region)` | Tier1 大对象 Region 注册到 largeRegionList_ | RegionSpace.cpp:303 (`RegisterLargeObjectMS`) |
| `ClearLargeMarks()` | 遍历 largeRegionList_ 清除所有 isMarked 标记 | MarkSweepCollector.cpp:148 (MarkSetup) |
| `SweepLargeRegions()` | 两阶段清扫：收集未标记大 Region → 释放回 RegionManager，返回释放字节数 | MarkSweepCollector.cpp:616 (PostMarkTermination) |
| `AddLargeRegionBytes(bytes)` / `SubLargeRegionBytes(bytes)` | 调整 totalMSRegionBytes_ 计数 | RegionSpace.cpp:304; SweepLargeRegions() 内部 |
| **监控统计** | | |
| `GetTotalPartialCount()` | 返回所有级别 partial（swept+unswept）Region 总数 | 监控/调试 |
| `GetTotalFullCount()` | 返回所有级别 full（swept+unswept）Region 总数 | 监控/调试 |
| `DumpPoolStats()` | 打印四链表聚合统计 + 大对象 Region 数（VLOG REPORT） | MarkSweepCollector.cpp:788 |
| `GetGlobalSweepGen()` | 返回当前全局 sweepGen 值 | MarkSweepCollector.cpp:711,773 |
| `GetSweepStats()` | 返回 MSSweepStats 引用 | MarkSweepCollector.cpp:727 |
| `GetTotalMSRegionBytes()` | 返回当前 MS Region 总占用字节（用于 GC 触发阈值） | RegionSpace.cpp:139 |
| **内部方法** | | |
| `ReleaseRegionInternal(region)` | 释放 Region：ReleaseBitmaps → Reset → totalMSRegionBytes_减 → ReclaimRegion | GetRegion() 中 IsCompletelyFree 后; ReleaseRegion() 转发 |
| `SweepAndClassify(region, sg, bucket)` | CAS 清扫+分类：sweep → 完全空闲 Release / 部分空闲 partialSwept / 仍满 fullSwept | ForceFinishSweep() 清扫循环 |
| `SweepUnsweptList(bucket, list, sg)` | 遍历指定 unswept 链表，CAS 清扫并分类归还 | BackgroundSweep() 遍历 partialUnswept/fullUnswept |

**CentralRegionPool 生命周期与使用路径**:

```
①创建                 ②分配路径              ③GC 清扫周期              ④后台清扫
RegionSpace::InitMS()  AllocBufferMS          MarkSweepCollector         FinalizerProcessor
  │                    ::RefillFromCentral()    │                          │
  └→ new CentralPool     │                     ├→ ForceFinishSweep()      └→ BackgroundSweep()
     (regionManager)     ├→ GetRegion(cls)     │   └→ drain unswept          └→ 36级×2链表
                         │   ├→ partialSwept   │       SweepAndClassify       SweepUnsweptList
                         │   ├→ partialUnswept │                                    │
                         │   │  (CAS sweep)    ├→ PrepareForSweep()          sweepDone_=true
                         │   └→ fullUnswept    │   ├→ Reset stats
                         │      (CAS sweep)    │   ├→ Swap swept↔unswept
                         │                     │   └→ sweepGen += 2
                         └→ GetFreshRegion(cls)│
                            └→ TakeRegion      ├→ ClearLargeMarks()
                            └→ InitForSizeClass└→ SweepLargeRegions()
                                                   └→ 回收未标记大Region
```

| 阶段 | 函数 | 详细说明 |
|------|------|---------|
| ①创建 | `CentralRegionPool(regionManager)` | `RegionSpace::InitMS()` L212 创建实例，绑定底层 RegionManager 指针 |
| ②分配-首选 | `GetRegion(cls)` | AllocBufferMS 缓存 miss 时调用。**四级优先级**：①partialSwept 直接取（最快）→②partialUnswept CAS(sg-2→sg-1) lazy sweep →③fullUnswept CAS lazy sweep（sweep 后可能产生空闲）→④返回 nullptr。全空闲 Region 直接 Release |
| ②分配-新建 | `GetFreshRegion(cls)` | GetRegion 返回 nullptr 后调用。从 RegionManager TakeRegion 分配全新 Region → InitForSizeClass → 设当前 sweepGen → totalMSRegionBytes_+=MS_REGION_SIZE |
| ②归还-部分 | `ReturnPartialRegion(region, cls)` | AllocBufferMS::ReturnRegion 有空闲分支。按 sweepGen 放入 partialSwept 或 partialUnswept |
| ②归还-已满 | `ReturnFullRegion(region, cls)` | AllocBufferMS::ReturnRegion 已满分支。按 sweepGen 放入 fullSwept 或 fullUnswept |
| ②归还-释放 | `ReleaseRegion(region, cls)` | AllocBufferMS::ReturnRegion 完全空闲分支。调 ReleaseRegionInternal：ReleaseBitmaps → Reset → totalMSRegionBytes_减 → ReclaimRegion 回收到 FreeRegionManager |
| ③GC-强制清扫 | `ForceFinishSweep()` | GC 启动前确保上轮清扫完成。遍历 36 级 bucket，Drain partialUnswept + fullUnswept，每个 Region 经 SweepAndClassify 清扫后分类 |
| ③GC-代际翻转 | `PrepareForSweep()` | MarkTerminate 后调用。Reset 统计 → 36 级 Swap(partialSwept↔partialUnswept, fullSwept↔fullUnswept) → globalSweepGen_+=2 → sweepDone_=false。O(1) 完成，无需移动 Region |
| ③GC-大对象 | `ClearLargeMarks()` + `SweepLargeRegions()` | MarkSetup 清标记 → PostMarkTermination 清扫。两阶段：先 ForEach 收集未标记 Region，再逐个释放回 RegionManager |
| ④后台清扫 | `BackgroundSweep()` | FinalizerProcessor gc-helper 线程调用。遍历 36 级，SweepUnsweptList 清扫 partialUnswept + fullUnswept。每 6 级 yield 一次避免饥饿。完成后 sweepDone_=true |
| ⑤监控 | `DumpPoolStats()` / `GetSweepStats()` | GC 周期结束时 MarkSweepCollector 调用，打印四链表聚合统计、清扫来源占比 |

**RegionSpace 入口** (RegionSpace.cpp L61-62):
```cpp
#ifdef MRT_USE_MARK_SWEEP
    // Mark-Sweep path: route through AllocBufferMS
    return msAllocBuffer_->Allocate(size);
```

**InitMS** (RegionSpace.cpp L201-227):
```
SizeClassInfo::Init()
→ MSRegionInfoArray::Init(heapStart, maxRegionCount)
→ new CentralRegionPool(&regionManager)
→ new AllocBufferMS() → Init(centralPool)
```

#### Review 重点
- 当前 Phase 1 设计为单 `AllocBufferMS` 实例（非 per-thread TLS）。Phase 2 需改为 per-Processor TLS 绑定。
- `GetRegion(cls)` 的 lazy sweep 在分配热路径上——如果 unswept 积压较多，分配延迟可能上升。
- `SizeClassBucket` 使用 `std::mutex` 加锁——高并发场景下可能成为瓶颈，Phase 2 考虑换 spinlock 或 CAS 链表。
- `BackgroundSweep()` 每 6 级 yield 一次——调度粒度可根据实际负载调优。

### Step 1.3: 大对象分配

> 设计文档: `doc/Step1.3_大对象分配与堆增长设计.md`
>
> **目标**: 三级大对象分配策略 + 多 Region 跨度管理 + 堆增长控制。

#### 新增文件

| 文件 | 行数 | 说明 |
|------|------|------|
| `src/Heap/Allocator/LargeObjectSpace.h` | 122 | `LargeRegionList`（Tier1 侵入式链表）+ `LargeObjectSpace`（Tier2 mmap） |
| `src/Heap/Allocator/LargeObjectSpace.cpp` | 271 | 大对象分配/释放/标记/清扫实现 |

#### 修改文件

| 文件 | 修改行 | 改动要点 |
|------|--------|----------|
| `src/Heap/Allocator/RegionSpace.h` L247-250 | `#ifdef` 块: 声明 `RegisterLargeObjectMS()` |
| `src/Heap/Allocator/RegionSpace.cpp` L54, L229-301 | MS 大对象分派 + `RegisterLargeObjectMS()` 实现（多 Region 跨度初始化） |
| `src/Heap/Allocator/RegionManager.h` L131 | `#ifdef` 块: MS 专用 `AllocLarge` 重载 |
| `src/Heap/Allocator/RegionManager.cpp` L17, L640-652, L1165 | `#ifdef` 块: MS 大对象分配、Region 释放回收 |
| `src/Heap/Allocator/CartesianTree.h` L125-164, L607-636 | `#ifdef` 块: `TakeRegionAligned()` 按 MS_REGION_SIZE 对齐 |
| `src/Heap/Allocator/FreeRegionManager.h` L109-155 | `#ifdef` 块: `TakeRegionMS()` 封装对齐分配 |

#### 三级分配策略

| 层级 | 大小范围 | 分配方式 | 管理方式 |
|------|---------|----------|----------|
| 常规 | ≤16KB | SizeClass + AllocBufferMS | CentralRegionPool 四链表 |
| Tier1（堆内大对象） | 16KB ~ 4MB | `RegionManager::AllocLarge()` → 多 Region | `LargeRegionList` + `CentralRegionPool::RegisterLargeRegion()` |
| Tier2（超大对象） | >4MB | `LargeObjectSpace::Allocate()` → 独立 `mmap` | `LargeObjectSpace` 链表，Eager Sweep |

#### 大对象分配入口

大对象分配统一由 `RegionSpace::TryAllocateOnce()` (RegionSpace.cpp L37-63) 根据 size 分派：

```
RegionSpace::Allocate(size)
  └→ TryAllocateOnce(allocSize)
       │
       ├─ size > 4MB (kHugeObjectThreshold)
       │   └→ Tier2: msLargeObjectSpace_->Allocate(size)  ← 独立 mmap
       │
       ├─ size > 16KB (largeObjectThreshold) && size ≤ 4MB
       │   └→ Tier1: RegionManager::TakeRegion(unitCount, LARGE_SIZED_UNITS)
       │       └→ rawRegion->Alloc(size) → RegisterLargeObjectMS(addr, regionBytes)
       │
       └─ size ≤ 16KB (kLargeObjectThreshold)
           └→ 常规: msAllocBuffer_->Allocate(size)  ← SizeClass + AllocBufferMS
```

#### 关键函数详细说明

##### Tier1 大对象（16KB ~ 4MB，堆内多 Region）

| 函数 | 位置 | 说明 | 调用者/使用位置 |
|------|------|------|----------------|
| `RegionSpace::TryAllocateOnce()` L37-63 | RegionSpace.cpp | 大对象分派入口：`allocSize > largeObjectThreshold` 进入 Tier1 路径 | `RegionSpace::Allocate()` 重试循环调用 |
| `RegionManager::TakeRegion(unitCount, LARGE_SIZED_UNITS)` | RegionManager.cpp | 分配连续 `unitCount` 个 UNIT_SIZE 单元的物理 Region | TryAllocateOnce() Tier1 分支 |
| `rawRegion->Alloc(allocSize)` | RegionInfo | 在分配到的 Region 上 bump 出 `allocSize` 字节，返回地址 | TryAllocateOnce() Tier1 分支 |
| `RegionSpace::RegisterLargeObjectMS(addr, regionBytes)` L267-301 | RegionSpace.cpp | **核心**：初始化大对象的 MSRegionInfo 元数据 + 跨度信息 + GC 注册。详见下方展开 | TryAllocateOnce() Tier1 分支 |
| `MSRegionInfo::InitForLargeObject(start, end)` | MSRegionInfo.cpp | 初始化 base MSRegionInfo：type=LARGE, allocPtr=start, regionEnd=end, isMarked=0 | RegisterLargeObjectMS() 调用 |
| `CentralRegionPool::RegisterLargeRegion(baseInfo)` | CentralRegionPool.cpp | 将 base MSRegionInfo 注册到 `largeRegionList_` 链表 | RegisterLargeObjectMS() 尾部 |
| `CentralRegionPool::AddLargeRegionBytes(regionBytes)` | CentralRegionPool.cpp | `totalMSRegionBytes_ += regionBytes`，用于 GC 触发阈值 | RegisterLargeObjectMS() 尾部 |

**RegisterLargeObjectMS 展开**:
1. `baseInfo = MSRegionInfoArray::GetRegionInfo(start)` → 获取首个 MSRegionInfo
2. `baseInfo->InitForLargeObject(start, end)` → 初始化 base
3. `baseInfo->reserved = baseIdx` → 自指（base 指向自己）
4. **跨度初始化**: `for i in 1..spanCount`: `spanInfo->regionType = MS_REGION_LARGE`, `spanInfo->reserved = baseIdx`（回溯指针）
5. **mark-at-alloc**: GC 标记阶段中分配的大对象，`baseInfo->isMarked = 1` 确保本轮存活
6. `centralPool->RegisterLargeRegion(baseInfo)` + `AddLargeRegionBytes(regionBytes)`

##### Tier2 超大对象（>4MB，独立 mmap）

| 函数 | 位置 | 说明 | 调用者/使用位置 |
|------|------|------|----------------|
| `LargeObjectSpace::Allocate(size)` L110 | LargeObjectSpace.cpp | `mmap` 分配页对齐内存 + 创建 `LargeObjectInfo` 元数据 + 链入链表 | TryAllocateOnce() Tier2 分支 |
| `LargeObjectSpace::Free(addr)` L151 | LargeObjectSpace.cpp | 从链表摘除 + `munmap` 立即释放 + 更新计数器 | 手动释放（当前未在 GC 路径直接调用） |
| `LargeObjectSpace::Contains(addr)` | LargeObjectSpace.cpp | O(n) 线性扫描判断地址是否属于 LOS | GC 标记时需要时调用 |
| `LargeObjectSpace::MarkObject(addr)` L200 | LargeObjectSpace.cpp | `FindInfo(addr)->gcMark = 1` 设置标记位 | MarkSweepCollector 标记阶段 |
| `LargeObjectSpace::SweepUnmarkedObjects()` L209 | LargeObjectSpace.cpp | 遍历链表，未标记的 `munmap` 释放 + 摘链 | MarkSweepCollector PostMarkTermination |
| `LargeObjectSpace::SweepUnmarkedObjectsWithStats()` | LargeObjectSpace.cpp | 带统计版本的清扫，返回 freedCount + freedBytes | DFX 日志路径 |
| `LargeObjectSpace::ClearAllMarks()` | LargeObjectSpace.cpp | 遍历链表清除所有 gcMark（用于 MarkSetup） | MarkSweepCollector MarkSetup 阶段 |
| `LargeObjectSpace::ForEachObject(visitor)` | LargeObjectSpace.cpp | 遍历所有对象调用 visitor（用于根枚举/标记） | GC 遍历路径 |

**LargeObjectInfo 结构体**:

| 字段 | 类型 | 说明 |
|------|------|------|
| `baseAddr` | `uintptr_t` | mmap 基地址（页对齐） |
| `mappedSize` | `size_t` | mmap 区域大小（页对齐） |
| `objectSize` | `size_t` | 实际对象大小 |
| `gcMark` | `uint8_t` | GC 标记位（0=死，1=活） |
| `isTraceAllocated` | `bool` | 是否在 GC trace 阶段分配 |
| `next` / `prev` | `LargeObjectInfo*` | 双向链表指针 |

##### LargeRegionList（Tier1 链表管理）

| 函数 | 说明 | 调用者/使用位置 |
|------|------|----------------|
| `Add(region)` | 头插 Region 到链表（自带 mutex 加锁） | CentralRegionPool::RegisterLargeRegion() 转发 |
| `Remove(region)` | 从链表摘除指定 Region（需外部持锁或 STW） | SweepAndReclaim() 内部 |
| `SweepAndReclaim()` | 遍历链表：存活→清标记，死亡→Remove+Reset+返回字节数 | CentralRegionPool::SweepLargeRegions() 可选路径 |
| `ForEach(visitor)` | 模板遍历（当前为占位，实际 Tier1 清扫走 CentralRegionPool 路径） | — |
| `Count()` | 返回链表 Region 数量 | DumpPoolStats() 统计 |

##### Tier1 大对象 GC 清扫路径

| 函数 | 位置 | 说明 |
|------|------|------|
| `CentralRegionPool::ClearLargeMarks()` | CentralRegionPool.cpp | MarkSetup：遍历 largeRegionList_ 清 isMarked=0 |
| `CentralRegionPool::SweepLargeRegions()` | CentralRegionPool.cpp | PostMarkTermination：两阶段——ForEach 收集未标记 Region → 逐个 Reset + ReclaimRegion + SubLargeRegionBytes |

#### Review 重点
- ⚠️ `RegisterLargeObjectMS` 的跨度初始化：所有 span MSRegionInfo 的 `reserved` 字段存储 `baseIdx`，用于 `GetRegionInfoValidated` 在大对象内部地址时回溯到 base。需确认跨度释放时所有 span 条目都正确 Reset。
- Tier1 大对象走 `CentralRegionPool::SweepLargeRegions()` L411（Eager Sweep），Tier2 走 `LargeObjectSpace::SweepUnmarkedObjects()`

---

### Step 1.4: 对象分配与 Region 管理全流程

> 本节将 Step 1.1 ~ 1.3 涉及的所有组件串联，描述两条完整流程。

#### 流程一：对象分配流程

**入口**: `RegionSpace::Allocate(size, allocType)` (RegionSpace.cpp L106)

```
用户代码分配对象
  │
  └→ RegionSpace::Allocate(size, allocType)  ← 最外层入口，带重试循环
       │
       └→ TryAllocateOnce(allocSize, allocType)  ← 单次分配尝试
            │
            ├─ [1] size > 4MB (kHugeObjectThreshold)
            │   └→ msLargeObjectSpace_->Allocate(size)
            │       ├→ mmap(size_页对齐)
            │       ├→ new LargeObjectInfo{baseAddr, mappedSize, ...}
            │       ├→ 链入 LargeObjectSpace 双向链表
            │       └→ 返回 mmap 基地址
            │
            ├─ [2] 16KB < size ≤ 4MB (Tier1 大对象)
            │   ├→ unitCount = ceil(size / UNIT_SIZE)
            │   ├→ regionManager.TakeRegion(unitCount, LARGE_SIZED_UNITS)
            │   │   └→ FreeRegionManager 从笛卡尔树分配连续 units
            │   ├→ rawRegion->Alloc(allocSize)  ← RegionInfo bump 分配
            │   └→ RegisterLargeObjectMS(addr, regionBytes)
            │       ├→ baseInfo = MSRegionInfoArray::GetRegionInfo(start)
            │       ├→ baseInfo->InitForLargeObject(start, end)
            │       ├→ 跨度 span 初始化 (type=LARGE, reserved=baseIdx)
            │       ├→ mark-at-alloc (GC 标记阶段→isMarked=1)
            │       ├→ centralPool->RegisterLargeRegion(baseInfo)
            │       └→ centralPool->AddLargeRegionBytes(regionBytes)
            │
            └─ [3] size ≤ 16KB (常规小对象)
                └→ msAllocBuffer_->Allocate(size)
                    ├→ SizeToClass(size) → cls
                    ├→ FastAlloc(cls)
                    │   ├─ activeRegions_[cls] 存在:
                    │   │   ├─ BUMP Region → Alloc() bump-pointer
                    │   │   └─ PARTIAL Region → AllocFromBitmap() CTZ
                    │   └─ 无活跃 Region 或已满 → SlowAlloc(cls)
                    │       ├→ ReturnRegion(cls)  ← 归还旧 Region
                    │       │   ├─ 完全空闲 → central_->ReleaseRegion()
                    │       │   ├─ 部分空闲 → central_->ReturnPartialRegion()
                    │       │   └─ 已满     → central_->ReturnFullRegion()
                    │       ├→ RefillFromCentral(cls)
                    │       │   ├→ central_->GetRegion(cls)  ← 四链表优先级
                    │       │   │   ├→ partialSwept 直接取
                    │       │   │   ├→ partialUnswept CAS lazy sweep
                    │       │   │   └→ fullUnswept CAS lazy sweep
                    │       │   └→ central_->GetFreshRegion(cls)  ← 全新 Region
                    │       │       ├→ TakeRegion(unitsPerMSRegion)
                    │       │       └→ InitForSizeClass(cls)
                    │       └→ 在新 Region 上分配
                    └→ mark-at-alloc (GC 标记阶段→MarkObject 标黑)
```

**分配失败重试逻辑** (`RegionSpace::Allocate` 重试循环):

| 重试次数 | 动作 | 函数 |
|---------|------|------|
| 1-2 次 | 协程让步 | `CJThreadResched()` |
| 3-5 次 | 触发 GC 或等待 GC 完成 | `RequestGC(GC_REASON_HEU)` / `WaitForGCFinish()` |
| 6 次 | 触发 OOM GC | `RequestGC(GC_REASON_OOM)` |
| 超限 | 抛出 OutOfMemory 异常 | `ExceptionManager::OutOfMemory()` |

**各层对应函数汇总**:

| 层级 | 入口函数 | 分配函数 | 元数据初始化 | GC 注册 |
|------|---------|---------|------------|---------|
| Tier2 (>4MB) | `TryAllocateOnce` | `LargeObjectSpace::Allocate()` (mmap) | `LargeObjectInfo` new | 链入 LargeObjectSpace 链表 |
| Tier1 (16KB~4MB) | `TryAllocateOnce` | `TakeRegion()` + `rawRegion->Alloc()` | `RegisterLargeObjectMS()` → `InitForLargeObject()` + 跨度初始化 | `RegisterLargeRegion()` + `AddLargeRegionBytes()` |
| 常规 (≤16KB) | `msAllocBuffer_->Allocate()` | `FastAlloc()`→bump/bitmap | `InitForSizeClass()` (首次填充) | CentralRegionPool 四链表管理 |

#### 流程二：Region 获取与归还流程

常规小对象（≤16KB）的 Region 在 `AllocBufferMS` ↔ `CentralRegionPool` ↔ `RegionManager` 三层之间流转：

```
                    RegionManager (物理 Region)
                    ┌─────────────────────────────┐
                    │ FreeRegionManager 笛卡尔树   │
                    │   空闲 Region 池             │
                    └──────┬───────────────┬──────┘
                  TakeRegion│             │ReclaimRegion
                   (获取新Region)      (完全空闲回收)
                           │             │
                    ┌──────▼─────────────▼──────┐
                    │     CentralRegionPool      │
                    │  per-SizeClass 四链表 ×36   │
                    │                            │
                    │  partialSwept  ◄── 分配首选 │
                    │  partialUnswept ← lazy sweep│
                    │  fullSwept                  │
                    │  fullUnswept   ← lazy sweep │
                    └──┬──────────────────┬──────┘
            GetRegion/ │                  │ ReturnPartial/
          GetFreshRegion              ReturnFull/Release
                       │                  │
                    ┌──▼──────────────────▼──────┐
                    │       AllocBufferMS         │
                    │  activeRegions_[36]          │
                    │  每级 1 个活跃 Region         │
                    └─────────────────────────────┘
                              ▲
                              │ Allocate(size)
                          用户分配请求
```

**Region 获取流程** (`AllocBufferMS::RefillFromCentral` → `CentralRegionPool::GetRegion/GetFreshRegion`):

```
AllocBufferMS 缓存 miss (activeRegions_[cls] 耗尽)
  │
  └→ RefillFromCentral(cls)
       │
       ├→ [优先级1] central_->GetRegion(cls)
       │   │
       │   ├→ [1.1] partialSwept.TakeHead()
       │   │   └→ 有: 直接返回（最快，已清扫有空闲）
       │   │
       │   ├→ [1.2] partialUnswept.TakeHead() + CAS lazy sweep
       │   │   ├→ CasSweepGen(sg-2 → sg-1)  ← 抢占清扫权
       │   │   ├→ SweepRegion()               ← 就地清扫
       │   │   ├→ SetSweepGen(sg)             ← 标记已清扫
       │   │   ├→ IsCompletelyFree? → Release + continue
       │   │   └→ 返回（有空闲 Region）
       │   │
       │   ├→ [1.3] fullUnswept.TakeHead() + CAS lazy sweep
       │   │   ├→ CasSweepGen + SweepRegion + SetSweepGen
       │   │   ├→ IsCompletelyFree? → Release + continue
       │   │   ├→ HasFreeSpace? → 返回（sweep 后产生空闲）
       │   │   └→ 仍满 → 放入 fullSwept + continue
       │   │
       │   └→ 全部链表为空 → 返回 nullptr
       │
       └→ [优先级2] central_->GetFreshRegion(cls)
            ├→ regionManager_->TakeRegion(unitsPerMSRegion, SMALL_SIZED_UNITS)
            │   └→ FreeRegionManager 笛卡尔树分配
            ├→ MSRegionInfoArray::GetRegionInfo(regionStart) → msInfo
            ├→ msInfo->InitForSizeClass(cls)
            │   ├→ 设置 sizeClass, totalSlotCount, freeSlotCount
            │   ├→ 分配 markBitmap + allocBitmap
            │   └→ regionType = MS_REGION_BUMP
            ├→ msInfo->allocPtr = regionStart, regionEnd = start + MS_REGION_SIZE
            ├→ SetSweepGen(当前 sweepGen)
            ├→ totalMSRegionBytes_ += MS_REGION_SIZE
            └→ 返回 msInfo (BUMP 模式，首次 bump-pointer 分配)
```

**Region 归还流程** (`AllocBufferMS::ReturnRegion` → `CentralRegionPool`):

```
AllocBufferMS 归还 Region (SlowAlloc 换出 / FlushAll GC 刷新)
  │
  └→ ReturnRegion(cls)
       │
       ├→ region->IsCompletelyFree()?
       │   YES → central_->ReleaseRegion(region, cls)
       │          └→ ReleaseRegionInternal(region)
       │              ├→ region->ReleaseBitmaps()    ← 释放 bitmap 内存
       │              ├→ region->Reset()             ← 清空所有字段
       │              ├→ totalMSRegionBytes_ -= MS_REGION_SIZE
       │              └→ regionManager_->ReclaimRegion(rawRegion)
       │                  └→ 归还物理 Region 到 FreeRegionManager 笛卡尔树
       │
       ├→ region->HasFreeSpace()?
       │   YES → central_->ReturnPartialRegion(region, cls)
       │          └→ 加锁 bucket[cls].lock
       │              ├→ sweepGen == 当前? → partialSwept.Prepend(region)
       │              └→ sweepGen 过期?   → partialUnswept.Prepend(region)
       │
       └→ 已满
           └→ central_->ReturnFullRegion(region, cls)
                └→ 加锁 bucket[cls].lock
                    ├→ sweepGen == 当前? → fullSwept.Prepend(region)
                    └→ sweepGen 过期?   → fullUnswept.Prepend(region)
```

**Region 状态流转汇总**:

| 状态 | 所在位置 | 进入条件 | 离开条件 |
|------|---------|---------|---------|
| 新建 (BUMP) | AllocBufferMS.activeRegions_ | GetFreshRegion() 创建并返回 | bump 耗尽 → ReturnRegion |
| 部分已用 (PARTIAL) | CentralRegionPool.partialSwept/Unswept | ReturnPartialRegion() 归还 | GetRegion() 取出重新分配 |
| 已满 (FULL) | CentralRegionPool.fullSwept/Unswept | ReturnFullRegion() 归还 | GetRegion() lazy sweep 后可能产生空闲 |
| 待清扫 (Unswept) | CentralRegionPool.*Unswept 链表 | PrepareForSweep() 代际翻转 | lazy sweep / BackgroundSweep / ForceFinishSweep |
| 已清扫 (Swept) | CentralRegionPool.*Swept 链表 | sweep 完成 + SetSweepGen(sg) | GetRegion() 取出 / PrepareForSweep() 翻转为 Unswept |
| 完全空闲 | 释放回 FreeRegionManager | IsCompletelyFree() 判定 | 被 TakeRegion 重新分配 |

---