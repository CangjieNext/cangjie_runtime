# Step 1.1 — MS RegionInfo 元数据改造设计

> 本文档从 [Step1_分配器改造详细设计.md](Step1_分配器改造详细设计.md) §3 拆分而来。
>
> 2026-04-05 当前代码状态校准：
> 1. 当前 `MSRegionInfo` 已经接入 `MSBitmapArena` sidecar 方案，region 初始化时会绑定 `[allocBitmap | markBitmap | scannedBitmap]` 三个位图槽。
> 2. `ReleaseBitmaps()` / `ResetToEmptyCache()` 当前主要做位图指针解绑和状态重置，不等于每次状态迁移都立即物理 `munmap`。
> 3. 文中如果仍以“独立 mmap bitmap、region 销毁时直接 unmap”为默认现状，应以当前 sidecar arena 实现为准。

---

## 3. MS RegionInfo 元数据改造设计

> **核心问题**：现有 `UnitMetadata`（~120 bytes/Unit）为 Copy GC 量身定做，包含路由信息、Ghost Region、读写锁等 MS 模式下无用的字段。本节设计一套面向 Mark-Sweep 的精简元数据 `MSRegionInfo`，同时将 Region 内空闲空间管理从 free-list 改为 bitmap。

<!-- PLACEHOLDER: 3.1 现有 UnitInfo 的角色分析 -->

### 3.1 现有 UnitInfo 的角色分析

当前 `UnitInfo`（即 `UnitMetadata`，定义于 `RegionInfo.h:991-1055`）在 Copy GC 中承担三个核心角色：

| # | 角色 | 关键字段 | 为什么 MS 不需要 |
|---|------|---------|----------------|
| 1 | **地址 → Region 反查** | 镜像数组 + `SUBORDINATE_UNIT` 的 `ownerRegion` | MS 不做对象搬移，不需要在 barrier 中频繁反查 Region；可改为简单数组索引 |
| 2 | **Ghost Region 追踪** | `inGhostFromRegion`, `regionEnd0`, `liveInfo0`, `ownerRegion0`, `unitRole0`, `nextRegionIdx0` | Ghost Region 是 Copy GC 特有的"转发后旧空间"概念，MS 不搬移对象，完全不需要 |
| 3 | **可变大小 Region（多 Unit 连续体）** | `SUBORDINATE_UNIT` 角色、`ownerRegion` 指针 | MS 仅需固定大小 Region（32KB）+ 单独的 LOS 路径，不需要运行时拼接多 Unit |

**结论**：在 MS 模式下，`UnitInfo` 的三大角色全部退化或消失。我们 **不再需要 per-Unit 元数据**，改为 **per-Region 元数据**（`MSRegionInfo`），元数据粒度从 256KB/Unit 提升到 32KB/Region（一对一）。

<!-- PLACEHOLDER: 3.2 堆前方镜像布局分析 -->

### 3.2 堆前方镜像布局分析：是否保留？

#### 3.2.1 现有布局

```
低地址 ──────────────────────────────────────────────── 高地址
       ┌──────────────────┬──────────────────────────┐
       │  UnitInfo[] 镜像  │     Heap Region 空间      │
       │  (向低地址增长)    │  (heapStartAddress →)    │
       └──────────────────┴──────────────────────────┘
                          ↑
                   heapStartAddress
```

- `UnitInfo[i]` 与 `Region[i]` 一一对应，`sizeof(UnitInfo) == sizeof(RegionInfo)` ≈ 120 bytes
- 查找公式：`unitInfo = heapStart - (regionIdx + 1) * sizeof(UnitInfo)`
- 每个 Unit 256KB（CopyGC），所以 256MB 堆 = 1024 个 Unit → 1024 × 120B = **120KB 镜像区**

#### 3.2.2 保留镜像的好处

| 优势 | 说明 |
|------|------|
| **O(1) 地址查找** | `addr → regionIdx = (addr - heapStart) / REGION_SIZE`，然后直接索引镜像数组，无需哈希/搜索 |
| **Cache 友好** | Region 元数据紧凑排列，遍历时顺序访问，对 prefetch 友好 |
| **无额外分配** | 元数据在堆 mmap 时一次性分配，不需要 `malloc` 管理生命周期 |
| **与编译器约束兼容** | `allocPtr` 在 `UnitInfo` 偏移 0 处的约束不变，编译器生成的 fast-path 无需修改 |
| **调试友好** | 从任意堆地址可以算出 Region 索引，方便 GDB 调试 |

#### 3.2.3 MS 模式下的变化

**保留镜像布局，但改变粒度**：

- 粒度：`per-Unit` → `per-Region`（MS 模式下 1 Region = 32KB，不再等于 CopyGC 的 Unit 256KB）
- 元素大小：`sizeof(UnitMetadata)` ≈ 120B → `sizeof(MSRegionInfo)` ≈ 64B（见 §3.4）
- 元素数量：堆大小 / 32KB（256MB 堆 = 8192 个 Region）

**内存对比**（256MB 堆，8192 个 Region）：

| 方案 | 每元素大小 | 总镜像大小 |
|------|-----------|-----------|
| 现有 CopyGC UnitInfo | ~120 B | ~120 KB（1024 Unit × 120B） |
| MS MSRegionInfo | ~64 B | **~512 KB**（8192 × 64B） |

**结论**：**保留镜像布局**。它的 O(1) 查找、cache 友好、零额外分配等优势在 MS 模式下同样有价值。精简后的 `MSRegionInfo` 只有 ~64 bytes，8192 Region × 64B = 512KB 镜像区，与 CopyGC 的 120KB 相比略大但完全可接受。

<!-- PLACEHOLDER: 3.3 MS 模式可删除/简化的字段 -->

### 3.3 MS 模式可删除/简化的字段

对现有 `UnitMetadata` 逐字段分析（参考 `RegionInfo.h:991-1055`）：

| 字段 | 大小(B) | CopyGC 用途 | MS 处置 | 理由 |
|------|---------|------------|---------|------|
| `allocPtr` | 8 | bump-pointer 当前位置 | **保留** | MS 新 Region 首次分配仍用 bump-pointer |
| `regionEnd` | 8 | Region 结束地址 | **保留** | 边界检查必需 |
| `nextRegionIdx` | 4 | Region 链表前驱 | **保留** | CentralRegionPool 链表管理 |
| `prevRegionIdx` | 4 | Region 链表后驱 | **保留** | CentralRegionPool 链表管理 |
| `liveByteCount` | 4 | 存活字节数 | **改名保留** → `liveSlotCount` | 按 slot 计数更精确 |
| `rawPointerObjectCount` | 4 | weak ref 追踪 | **保留** | MS 也需 weak ref 处理 |
| `liveInfo` / `ownerRegion` | 8 (union) | LiveInfo 指针 / 从属 Unit 指回主 Region | **删除 union, 改为 `markBitmap*`** | MS 不需要 `ownerRegion`；LiveInfo 被 bitmap 替代 |
| `liveInfo0` / `ownerRegion0` | 8 (union) | Ghost Region 备份 | **删除** | Ghost 是 CopyGC 专有 |
| `regionEnd0` | 8 | Ghost Region 备份 | **删除** | Ghost 是 CopyGC 专有 |
| `routeInfo` | 16 | 转发目标地址 | **删除** | MS 不搬移对象 |
| `nextRegionIdx0` | 4 | Ghost Region 备份 | **删除** | Ghost 是 CopyGC 专有 |
| `unitRole` | 4 bit | 角色标记 | **删除** | MS 只有一种 Region 角色 |
| `unitRole0` | 4 bit | Ghost Region 备份 | **删除** | Ghost 是 CopyGC 专有 |
| `regionType` | 4 bit | Region 类型 | **保留并简化** | 见 §3.9 |
| `isTraceRegion` | 1 bit | trace 标记 | **删除** | MS 不分 trace/non-trace |
| `inGhostFromRegion` | 1 bit | Ghost 标记 | **删除** | Ghost 是 CopyGC 专有 |
| `isMarked` | 1 bit | GC 标记 | **保留** | sweep 时判断 Region 是否已标记 |
| `isEnqueued` | 1 bit | 队列标记 | **保留** | weak ref 队列管理 |
| `isResurrected` | 1 bit | 复活标记 | **保留** | finalize 后复活检测 |
| `routeState` | 1 | 路由状态 | **删除** | MS 不搬移对象 |
| `rwLock` | ≥4 | 读写锁 | **删除** | MS 不需要 Region 级读写锁（并发标记用不同同步策略） |

**总结**：可删除约 **56 bytes** 的 CopyGC 专有字段，新增 bitmap 指针等约 16 bytes。净减约 40 bytes（120B → ~64B, 减少 47%）。

<!-- PLACEHOLDER: 3.4 MSRegionInfo 结构设计 -->

### 3.4 MSRegionInfo 结构设计

```cpp
// 编译约束：allocPtr 必须在偏移 0
static_assert(offsetof(MSRegionInfo, allocPtr) == 0);

struct MSRegionInfo {
    // ========== 分配核心（偏移 0，编译器 fast-path 依赖） ==========
    uint8_t*    allocPtr;           // +0   bump-pointer / 当前分配位置 (8B)
    uint8_t*    regionEnd;          // +8   Region 结束地址 (8B)

    // ========== Region 链表管理 ==========
    int32_t     nextRegionIdx;      // +16  链表后继索引 (4B)
    int32_t     prevRegionIdx;      // +20  链表前驱索引 (4B)

    // ========== Bitmap 指针 ==========
    uint64_t*   allocBitmap;        // +24  分配位图指针 (8B)  → 1=空闲, 0=已分配
    uint64_t*   markBitmap;         // +32  标记位图指针 (8B)  → 1=存活, 0=死亡

    // ========== 统计信息 ==========
    uint16_t    totalSlotCount;     // +40  Region 内总 slot 数 (2B)
    uint16_t    freeSlotCount;      // +42  当前空闲 slot 数 (2B)
    uint16_t    liveSlotCount;      // +44  上次 GC 后存活 slot 数 (2B)
    uint16_t    rawPointerObjCount; // +46  weak ref 引用计数 (2B)

    // ========== 分类信息 ==========
    uint8_t     sizeClass;          // +48  Size Class 编号 (1B, 见 §4)
    uint8_t     sweepState;         // +49  sweep 状态 (1B): UNSWEPT / SWEEPING / SWEPT
    uint8_t     regionType;         // +50  Region 类型 (1B, 见 §3.9)

    // ========== 标志位（位域） ==========
    uint8_t     isMarked     : 1;   // +51.0  GC 标记
    uint8_t     isEnqueued   : 1;   // +51.1  weak ref 队列标记
    uint8_t     isResurrected: 1;   // +51.2  finalizer 复活标记
    uint8_t     flagsPadding : 5;   // +51.3  保留

    // ========== 对齐填充 ==========
    uint32_t    padding;            // +52  对齐到 8 字节边界 (4B)

    // ========== 预留扩展 ==========
    uint64_t    reserved;           // +56  未来扩展 (8B)
};                                  // 总计 64 bytes

static_assert(sizeof(MSRegionInfo) == 64, "MSRegionInfo must be exactly 64 bytes");
```

**设计要点**：

1. **`allocPtr` 在偏移 0**：与现有编译器 fast-path 兼容，`AllocBuffer::tlRegion` 直接指向 `MSRegionInfo`
2. **bitmap 用指针而非内联**：bitmap 大小随 sizeClass 变化（见 §3.5），不适合固定在结构体内
3. **slot 计数用 `uint16_t`**：最小 sizeClass=8B 时，32KB Region 最多 4096 个 slot，`uint16_t` (0~65535) 足够
4. **64 bytes 对齐**：恰好是一个 cache line，遍历时 cache 效率最优
5. **预留 8B `reserved`**：为后续增量式 sweep、并发标记等扩展留空间

**与现有 `UnitMetadata` 的对应关系**：

```
UnitMetadata (~120B)          →  MSRegionInfo (64B)
──────────────────────────────────────────────────
allocPtr                      →  allocPtr         (保留)
regionEnd                     →  regionEnd        (保留)
nextRegionIdx / prevRegionIdx →  nextRegionIdx / prevRegionIdx (保留)
liveByteCount                 →  liveSlotCount    (字节→slot)
rawPointerObjectCount         →  rawPointerObjCount (保留)
liveInfo (→LiveInfo→bitmap)   →  markBitmap       (直接指针，省一层间接)
ownerRegion                   →  (删除，MS 无从属 Unit)
liveInfo0/ownerRegion0        →  (删除，Ghost 专有)
regionEnd0                    →  (删除，Ghost 专有)
routeInfo (16B)               →  (删除，MS 不搬移)
unitRole / unitRole0          →  (删除)
routeState                    →  (删除)
rwLock                        →  (删除)
(无)                          →  allocBitmap      (新增，bitmap 分配)
(无)                          →  sizeClass        (新增)
(无)                          →  sweepState       (新增)
(无)                          →  freeSlotCount    (新增)
(无)                          →  totalSlotCount   (新增)
```

<!-- PLACEHOLDER: 3.5 AllocBitmap 设计 -->

### 3.5 AllocBitmap 设计：bitmap 替代 free-list

#### 3.5.1 核心思路

每个 Region 维护两个 bitmap：

| Bitmap | 语义 | 谁写 | 谁读 |
|--------|------|------|------|
| `allocBitmap` | 1=空闲 slot, 0=已分配 slot | Allocator (分配时清 bit), Sweeper (回收时设 bit) | Allocator (找空闲 slot) |
| `markBitmap` | 1=存活 slot, 0=死亡 slot | GC Marker (标记阶段设 bit) | Sweeper (回收阶段读) |

#### 3.5.2 Bitmap 大小计算

每个 bit 对应一个 slot。对于 sizeClass = S 的 Region：

```
slotCount = REGION_SIZE / S = 32KB / S
bitmapWords = ceil(slotCount / 64)    // 每个 uint64_t 覆盖 64 个 slot
bitmapBytes = bitmapWords * 8
```

各 Size Class 的 bitmap 大小：

| Size Class (bytes) | Slots/Region | Bitmap Words | Bitmap Bytes | 两个 Bitmap 合计 |
|--------------------|-------------|-------------|-------------|----------------|
| 8 | 4096 | 64 | 512 | 1024 |
| 16 | 2048 | 32 | 256 | 512 |
| 32 | 1024 | 16 | 128 | 256 |
| 48 | 682 | 11 | 88 | 176 |
| 64 | 512 | 8 | 64 | 128 |
| 128 | 256 | 4 | 32 | 64 |
| 256 | 128 | 2 | 16 | 32 |
| 512 | 64 | 1 | 8 | 16 |
| 1024 | 32 | 1 | 8 | 16 |

最大开销：sizeClass=8 时，两个 bitmap 合计 1024B，仅占 Region 大小的 **3.1%**。

#### 3.5.3 分配算法（Bitmap Alloc）

```cpp
// 在 Region 的 allocBitmap 中找一个空闲 slot
inline void* AllocFromBitmap(MSRegionInfo* region, uint8_t sizeClass) {
    uint64_t* bitmap = region->allocBitmap;
    int wordCount = region->totalSlotCount / 64;

    for (int i = 0; i < wordCount; i++) {
        if (bitmap[i] != 0) {
            // CTZ: Count Trailing Zeros, 找最低位的 1（空闲 slot）
            int bit = __builtin_ctzll(bitmap[i]);
            bitmap[i] &= ~(1ULL << bit);     // 清除该 bit（标记为已分配）
            region->freeSlotCount--;

            // 计算 slot 地址
            int slotIndex = i * 64 + bit;
            return region->regionStart() + slotIndex * sizeClass;
        }
    }
    return nullptr;  // Region 已满
}
```

**性能分析**：
- `CTZ` 指令在 x86 (`tzcnt`) 和 ARM (`rbit + clz`) 上都是单周期指令
- 最坏情况遍历 64 个 word（sizeClass=8），但实际中 `freeSlotCount > 0` 时很快命中
- 可进一步优化：维护 `searchHint`（上次成功分配的 word 索引），减少扫描范围

#### 3.5.4 Sweep 算法（Bitmap Sweep）

```cpp
// Sweep: 根据 markBitmap 更新 allocBitmap
inline void SweepRegion(MSRegionInfo* region) {
    uint64_t* alloc = region->allocBitmap;
    uint64_t* mark  = region->markBitmap;
    int wordCount = region->totalSlotCount / 64;
    int liveCount = 0;

    for (int i = 0; i < wordCount; i++) {
        // 存活对象：markBitmap 中为 1 的 slot → 在 allocBitmap 中标记为已分配（0）
        // 死亡对象：markBitmap 中为 0 的 slot → 在 allocBitmap 中标记为空闲（1）
        alloc[i] = ~mark[i];
        liveCount += __builtin_popcountll(mark[i]);
    }

    // 清除 markBitmap 为下次 GC 做准备
    memset(mark, 0, wordCount * sizeof(uint64_t));

    region->liveSlotCount = liveCount;
    region->freeSlotCount = region->totalSlotCount - liveCount;
    region->sweepState = SWEPT;
}
```

**核心优化点**：`allocBitmap = ~markBitmap`，一条 NOT 指令完成整个 word（64 个 slot）的 sweep，无需遍历每个对象。

#### 3.5.5 Bitmap vs Free-list 对比

| 维度 | Free-list | Bitmap |
|------|-----------|--------|
| **分配速度** | O(1) 链表头弹出 | O(1) CTZ 找空闲 bit |
| **Sweep 速度** | O(dead) 逐个链入 | O(words) 批量 NOT，**SIMD 友好** |
| **空间开销** | 在死对象头部写 next 指针 | 额外 bitmap（最大 1KB/Region） |
| **Cache 友好** | 随机访问死对象（cache miss） | 顺序访问 bitmap word（cache 友好） |
| **死对象写入** | 必须写入死对象的 header | **不碰死对象内存**，对 COW/换页友好 |
| **并发安全** | 需要锁保护链表头 | 可用 CAS 操作 bitmap word |
| **调试友好** | 需遍历链表 | 一个 bitmap dump 就能看到全部空闲信息 |
| **碎片可见性** | 需统计链表长度 | `popcount` 一算就知道空闲率 |

**结论**：Bitmap 在 sweep 速度、cache 友好度、死对象零写入、调试便利性上全面优于 free-list。额外的 bitmap 空间开销（最大 3.1%）完全可接受。

<!-- PLACEHOLDER: 3.6 Bitmap 存储位置 -->

### 3.6 Bitmap 存储位置

#### 方案对比

| 方案 | 做法 | 优点 | 缺点 |
|------|------|------|------|
| A. Region 头部内嵌 | 从 Region 开头划出固定空间放 bitmap | 访问快（同一 page），无额外分配 | 浪费空间（必须按最大 sizeClass 预留），破坏 slot 对齐 |
| B. 镜像区扩展 | 在堆前方镜像区为每个 Region 分配 bitmap | 与 MSRegionInfo 相邻，cache 友好 | 镜像区大小需预分配，不同 sizeClass 大小不同 |
| **C. 独立 mmap** ★ | 每个 Region 初始化时 `mmap` 独立 bitmap 区域 | 大小精确匹配 sizeClass，按需分配 | 多一次 mmap 开销，指针间接访问 |
| D. 全局 bitmap pool | 预分配一大块 bitmap pool，按需切片 | 无 mmap 碎片 | 需要管理 pool 分配/释放 |

#### 推荐：方案 C（独立 mmap）+ 延迟分配

```
Region 初次分配给某 sizeClass 时：
  1. 计算 bitmapBytes = ceil(REGION_SIZE / sizeClass / 64) * 8
  2. mmap(bitmapBytes * 2)   // allocBitmap + markBitmap 连续分配
  3. allocBitmap = mmap_addr
  4. markBitmap  = mmap_addr + bitmapBytes

Region 释放回 CentralRegionPool 时：
  5. munmap(allocBitmap, bitmapBytes * 2)
  6. allocBitmap = markBitmap = nullptr
```

**理由**：
- 不浪费 Region 内部空间（slot 地址对齐不受影响）
- Bitmap 大小精确匹配 sizeClass（8B slot 需要 512B bitmap，1024B slot 只需要 8B bitmap）
- Region 回收后 bitmap 内存立即释放，不产生内存泄漏
- mmap 开销在 Region 生命周期内只发生一次，可接受

<!-- PLACEHOLDER: 3.7 地址到 MSRegionInfo 的查找 -->

### 3.7 地址到 MSRegionInfo 的查找

#### 现有 CopyGC 查找（复杂，需处理 SUBORDINATE_UNIT）

```cpp
// 现有：addr → RegionInfo（需处理多 Unit Region 和从属 Unit）
RegionInfo* GetRegionInfo(void* addr) {
    int idx = (addr - heapStartAddress) / UNIT_SIZE;
    UnitInfo* ui = GetUnitInfo(idx);
    if (ui->unitRole == SUBORDINATE_UNIT) {
        return ui->ownerRegion;  // 从属 Unit → 跳转到主 Region
    }
    return (RegionInfo*)ui;
}
```

#### MS 模式查找（简化，O(1) 直接索引）

```cpp
// MS 模式：addr → MSRegionInfo（始终 O(1)，无需 ownerRegion 跳转）
inline MSRegionInfo* GetMSRegionInfo(void* addr) {
    int regionIdx = ((uintptr_t)addr - heapStartAddress) / REGION_SIZE;
    // 镜像数组在 heapStartAddress 之前
    return &msRegionInfoArray[regionIdx];
}

// 其中 msRegionInfoArray = (MSRegionInfo*)(heapStartAddress - maxRegionCount * sizeof(MSRegionInfo))
```

**简化点**：
- 不再需要 `SUBORDINATE_UNIT` 判断（MS Region 都是固定 32KB，不拼接）
- 不再需要 `ownerRegion` 指针跳转
- 查找总是 O(1)，无分支

#### 地址 → slot 索引（用于 bitmap 操作）

```cpp
inline int GetSlotIndex(MSRegionInfo* region, void* addr) {
    uintptr_t regionStart = (uintptr_t)region->regionEnd - REGION_SIZE;
    return ((uintptr_t)addr - regionStart) / region->sizeClass;
}

inline void SetMarkBit(MSRegionInfo* region, void* obj) {
    int slotIdx = GetSlotIndex(region, obj);
    int wordIdx = slotIdx / 64;
    int bitIdx  = slotIdx % 64;
    region->markBitmap[wordIdx] |= (1ULL << bitIdx);
}
```

<!-- PLACEHOLDER: 3.8 大对象 Region 处理 -->

### 3.8 大对象 Region 处理

大对象（> REGION_SIZE/2 = 16KB）不适合 slot+bitmap 分配，走独立的 LOS（Large Object Space）路径。

| 维度 | 小对象 Region | 大对象 Region |
|------|-------------|-------------|
| sizeClass | 8B ~ 2048B | N/A |
| 分配方式 | bitmap 找空闲 slot | mmap 独立区域 或 多 Region 连续分配 |
| allocBitmap | 需要 | 不需要（整个 Region 就是一个对象） |
| markBitmap | 需要 | 只需 1 bit（Region 级别标记） |
| MSRegionInfo.regionType | `ACTIVE` / `PARTIAL` / `FULL` | `LARGE_OBJECT` |
| sweep | bitmap NOT 操作 | 直接 munmap / 释放整个 Region |

**在 MSRegionInfo 中的表示**：大对象 Region 的 `allocBitmap = nullptr`, `markBitmap = nullptr`，仅用 `isMarked` 位表示存活状态。详细设计见 §12（大对象管理改造设计）。

<!-- PLACEHOLDER: 3.9 MSRegionType 简化枚举 -->

### 3.9 MSRegionType 简化枚举

#### 现有 CopyGC RegionType（15+ 种）

```cpp
// 现有：类型繁多，包含大量 CopyGC 专有状态
enum RegionType {
    FREE_REGION, EDEN_REGION, SURVIVOR_REGION, PROMOTE_REGION,
    OLD_REGION, TRACE_REGION, GHOST_REGION, LARGE_REGION,
    HUMONGOUS_START, HUMONGOUS_CONTINUE, PIN_REGION, ...
};
```

#### MS 模式 MSRegionType（5 种）

```cpp
enum MSRegionType : uint8_t {
    MS_REGION_FREE    = 0,  // 未分配，在 CentralRegionPool.freeList 中
    MS_REGION_BUMP    = 1,  // 首次分配中，使用 bump-pointer（allocPtr < regionEnd）
    MS_REGION_PARTIAL = 2,  // 已 sweep，有空闲 slot（freeSlotCount > 0）
    MS_REGION_FULL    = 3,  // 已满，无空闲 slot（freeSlotCount == 0）
    MS_REGION_LARGE   = 4,  // 大对象 Region（整个 Region 存放单个对象）
};
```

**状态转换**：

```
                ┌──────────────────────────────────────────┐
                │                                          │
                ▼                                          │
  FREE ──alloc──→ BUMP ──满了──→ FULL ──sweep──→ PARTIAL ──→ FULL
                   │                              │    ↑
                   │                              │    │
                   └──────── sweep ───────────────┘    │
                                                       │
                              alloc slot ──────────────┘
                              (freeSlotCount→0)
                  
  PARTIAL ──allFreeAfterSweep──→ FREE (归还 CentralRegionPool)
  
  FREE ──allocLargeObject──→ LARGE
  LARGE ──gcSweepDead──→ FREE
```

<!-- PLACEHOLDER: 3.10 元数据内存开销对比 -->

### 3.10 元数据内存开销对比

#### 假设：256MB 堆，8192 个 Region（32KB/Region）

**现有 CopyGC 方案**：

| 元数据 | 每 Region 开销 | 1024 Region 合计 |
|--------|---------------|-----------------|
| UnitMetadata 镜像 | ~120 B | 120 KB（1024 Unit × 120B） |
| LiveInfo + RegionBitmap（mark bitmap） | ~2 KB（sizeClass=16 时） | ~2 MB |
| RouteInfo | 16 B | 16 KB |
| **合计** | | **~2.1 MB** |

**MS MSRegionInfo + Bitmap 方案**：

| 元数据 | 每 Region 开销 | 1024 Region 合计 |
|--------|---------------|-----------------|
| MSRegionInfo 镜像 | 64 B | 512 KB |
| allocBitmap + markBitmap | ≤1 KB（sizeClass=8 时） | ≤8 MB（极端） |
| **合计（极端 sizeClass=8）** | | **≤8.5 MB** |
| **合计（典型 sizeClass=64）** | | **~1.5 MB** |
| **合计（混合 sizeClass）** | | **~2 MB**（估算） |

> **注**：bitmap 开销与 sizeClass 正相关。实际应用中小对象居多但不会全是 16B，典型混合场景下 bitmap 总开销约 1-2MB。

#### 综合对比

| 维度 | CopyGC UnitInfo | MS MSRegionInfo |
|------|-----------------|-----------------|
| 结构体大小 | ~120 B | 64 B (**-47%**) |
| 镜像区大小 | 120 KB（1024 Unit） | 512 KB（8192 Region） |
| per-Region mark 元数据 | LiveInfo+RouteInfo（~2KB + 16B） | bitmap 指针（就在结构体中） |
| 总元数据开销 | ~2.1 MB | ~2 MB（bitmap 随 sizeClass 而异） |
| 查找复杂度 | O(1) + SUBORDINATE 分支 | O(1) 无分支 |
| Ghost Region 支持 | 支持（~40B/Region 开销） | 不需要（**-40B/Region**） |
| 多 Unit Region 支持 | 支持（ownerRegion 跳转） | 不需要（固定 32KB） |

---

### 3.11 MSRegionInfo Bitmap 方法实现

基于 §3.4 结构体和 §3.5 bitmap 算法，以下是 `MSRegionInfo` 类完整的方法接口。

```cpp
#ifdef MRT_USE_MARK_SWEEP

    // ========== 初始化 ==========

    // 初始化 Region 为指定 size class，分配 bitmap（§3.6 方案 C：独立 mmap）
    void InitForSizeClass(uint8_t cls) {
        sizeClass = cls;
        freeSlotCount = 0;
        sweepState = UNSWEPT;
        size_t slotSize = SizeClassInfo::ClassToSize(cls);
        totalSlotCount = static_cast<uint16_t>(REGION_SIZE / slotSize);

        // 分配 allocBitmap + markBitmap 连续内存
        size_t bitmapWords = (totalSlotCount + 63) / 64;
        size_t bitmapBytes = bitmapWords * sizeof(uint64_t);
        void* mem = mmap(nullptr, bitmapBytes * 2, PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        allocBitmap = reinterpret_cast<uint64_t*>(mem);
        markBitmap  = reinterpret_cast<uint64_t*>(
            reinterpret_cast<uint8_t*>(mem) + bitmapBytes);

        // 初始化：所有 slot 空闲（allocBitmap 全 1），标记位清零
        memset(allocBitmap, 0xFF, bitmapBytes);
        memset(markBitmap, 0, bitmapBytes);
        freeSlotCount = totalSlotCount;
    }

    // ========== 分配 ==========

    // 从 allocBitmap 分配一个空闲 slot（O(1) CTZ，线程本地调用无需同步）
    // 算法参见 §3.5.3
    uintptr_t AllocFromBitmap() {
        int wordCount = (totalSlotCount + 63) / 64;
        size_t slotSize = SizeClassInfo::ClassToSize(sizeClass);

        for (int i = 0; i < wordCount; i++) {
            if (allocBitmap[i] != 0) {
                int bit = __builtin_ctzll(allocBitmap[i]);
                allocBitmap[i] &= ~(1ULL << bit);    // 清除 bit → 标记为已分配
                freeSlotCount--;
                size_t slotIndex = i * 64 + bit;
                return regionStart() + slotIndex * slotSize;
            }
        }
        return 0;  // Region 已满
    }

    // ========== Sweep ==========

    // 根据 markBitmap 更新 allocBitmap（§3.5.4 算法）
    // 核心：allocBitmap[i] = ~markBitmap[i]，64 个 slot 一条 NOT 指令
    // 返回本次回收释放的字节数
    size_t SweepRegion() {
        int wordCount = (totalSlotCount + 63) / 64;
        int liveCount = 0;

        for (int i = 0; i < wordCount; i++) {
            allocBitmap[i] = ~markBitmap[i];
            liveCount += __builtin_popcountll(markBitmap[i]);
        }

        // 清除 markBitmap 为下次 GC 做准备
        memset(markBitmap, 0, wordCount * sizeof(uint64_t));

        liveSlotCount = static_cast<uint16_t>(liveCount);
        freeSlotCount = totalSlotCount - liveSlotCount;
        sweepState = SWEPT;

        return freeSlotCount * SizeClassInfo::ClassToSize(sizeClass);
    }

    // ========== 生命周期 ==========

    // 释放 bitmap 内存，Region 归还 FreeRegionManager 时调用
    void ReleaseBitmaps() {
        if (allocBitmap != nullptr) {
            size_t bitmapWords = (totalSlotCount + 63) / 64;
            size_t bitmapBytes = bitmapWords * sizeof(uint64_t);
            munmap(allocBitmap, bitmapBytes * 2);  // alloc + mark 连续分配，一次释放
            allocBitmap = nullptr;
            markBitmap = nullptr;
        }
    }

    // ========== 查询 ==========

    // 所有 slot 空闲 → 可整体释放回 FreeRegionManager
    bool IsCompletelyFree() const {
        return freeSlotCount == totalSlotCount;
    }

    // 是否还有可分配空间（bitmap 空闲 slot 或 bump-pointer 尾部）
    bool HasFreeSpace() const {
        return freeSlotCount > 0 ||
               allocPtr + SizeClassInfo::ClassToSize(sizeClass) <= regionEnd;
    }

    uint8_t  GetSizeClass()     const { return sizeClass; }
    uint16_t GetFreeSlotCount() const { return freeSlotCount; }

#endif // MRT_USE_MARK_SWEEP
```

**性能优化点**：可利用 §3.4 中 `reserved` 字段存储 `searchHint`（上次成功分配的 word 索引），避免每次从 word 0 开始扫描。在 sweep 后重置 `searchHint = 0`。

---

### 3.12 分配优先级：Bitmap + Bump-Pointer 双阶段

Region 生命周期中存在两个分配阶段：

#### 阶段一：BUMP（新 Region 首次填充）

Region 刚从 `CentralRegionPool` 获取、尚未经历 GC sweep 时，使用 bump-pointer 快速顺序填充：

```
allocPtr += slotSize    ← 极快，与 CopyGC 相同
同步将 allocBitmap 对应 bit 清零（标记为已分配）
```

此阶段 bitmap 只做被动记录，不参与查找。分配速度与 CopyGC bump-pointer 相同。

#### 阶段二：SWEPT（sweep 后的 Region）

Region 经过 GC sweep 后，`allocBitmap` 反映了空闲 slot 分布（§3.5.4），使用 bitmap 分配：

```
AllocFromBitmap() → CTZ 找空闲 bit → 返回 slot 地址    ← O(1)
```

#### 统一入口

```cpp
uintptr_t MSRegionInfo::AllocMS(size_t size) {
    // 阶段二：sweep 后使用 bitmap 分配
    if (sweepState == SWEPT) {
        return AllocFromBitmap();
    }

    // 阶段一：bump-pointer（新 Region 尾部空间）
    size_t slotSize = SizeClassInfo::ClassToSize(sizeClass);
    if (allocPtr + slotSize <= regionEnd) {
        uintptr_t addr = reinterpret_cast<uintptr_t>(allocPtr);
        allocPtr += slotSize;
        freeSlotCount--;

        // 同步更新 allocBitmap
        size_t slotIdx = (addr - regionStart()) / slotSize;
        allocBitmap[slotIdx / 64] &= ~(1ULL << (slotIdx % 64));

        return addr;
    }

    return 0;  // Region 已满 → 上层需获取新 Region
}
```

**关键细节**：即使 bump-pointer 耗尽，如果 Region 后来被 sweep，仍可通过 bitmap 复用已死亡对象的 slot。

---

### 3.13 与现有 Pinned 机制的关系

当前 `AllocPinned` 使用 slot-based 分配（`FreePinnedSlotLists`），维护不同大小的 free-list：

```cpp
// 现有: RegionManager::AllocPinned
MAddress AllocPinned(size_t size) {
    // 1. 尝试从 recentPinnedRegionList 头部 Region 分配
    // 2. 遍历 freePinnedSlotLists 找合适大小的 slot
    // 3. 都失败 → 新建 Region
}
```

在 Mark-Sweep 模式下：

1. **所有对象都不移动**：`PINNED_OBJECT` 和 `MOVEABLE_OBJECT` 的区分不再有实际意义
2. **bitmap 取代 slot-list**：`allocBitmap` 天然支持任意 slot 的分配/释放，无需维护额外链表
3. **API 兼容**：保留 `allocType` 参数，仅在行为上统一走 Mark-Sweep bitmap 路径

**bitmap 相比 Pinned slot-list 的优势**：

| 维度 | Pinned slot-list | Bitmap |
|------|------------------|--------|
| slot 大小 | 不固定（动态适配） | size-class 固定（更简单） |
| 查找空闲 slot | O(n) 遍历链表 | O(1) CTZ |
| 死对象写入 | 写入 next 指针 | 不碰死对象 |
| 碎片可见性 | 需遍历统计 | `popcount` O(1) |

