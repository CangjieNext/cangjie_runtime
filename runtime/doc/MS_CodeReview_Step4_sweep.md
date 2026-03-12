## Step 4: 并发清扫

> 设计文档: `doc/Step4_并发清扫详细设计.md`
>
> **目标**: 三路协作（Lazy Sweep + Background Sweep + ForceFinishSweep），bitmap NOT 翻转算法，sweepGen 代际翻转 O(1) 切换。
>
> **核心等式**: `allocBitmap[i] = ~markBitmap[i]`（markBitmap 按位取反 → 存活位变 0、死亡位变 1 → allocBitmap 中 1 = 空闲槽位）

#### 并发清扫总体时序

```
GC Cycle N 结束：
  PostMarkTermination()
    → ProcessFinalizers() → fp.Notify()        // 唤醒 gc-helper 处理 Finalizer
    → CollectLargeGarbage()                     // Eager Sweep 大对象 (Tier1+Tier2)
  ConcurrentSweep()
    → PrepareForSweep()                         // globalSweepGen_ += 2, Swap(swept↔unswept)
    → (设计文档: NotifyToSweep() 唤醒后台 Sweep, **实际未接线，见下 ⚠**)
    ── GC 结束, Mutator 恢复运行 ──

Mutator 运行期间 (GC Cycle N+1 之前)：
  分配路径触发 Lazy Sweep：
    AllocBufferMS::Refill() → CentralRegionPool::GetRegion(cls)
      → Priority 2/3: 从 unswept 链表取 Region → CAS 抢占 → SweepRegion() → 分类归队
  gc-helper 线程触发 Background Sweep (如果 shouldBackgroundSweep_ == true)：
    BackgroundSweep() → 遍历 36 个 SizeClass 的 unswept 链表逐个清扫

GC Cycle N+1 开始：
  MarkSetup()
    → ForceFinishSweep()                        // 兜底：强制清扫所有残留 unswept Region
```

#### 三路协作模型详细说明

| 路径 | 入口函数 | 触发时机 | 做了什么 | 退出条件 |
|------|---------|---------|----------|---------|
| **Lazy Sweep** | `CentralRegionPool::GetRegion(cls)` 中 Priority 2/3 分支 | Mutator 分配时需要 Region，partialSwept 链表为空，必须从 unswept 链表获取 | 从 `partialUnswept` / `fullUnswept` 链表 TakeHead 一个 Region → `CasSweepGen(sg-2, sg-1)` 抢占清扫权 → `SweepRegion()` 执行 bitmap NOT → `SetSweepGen(sg)` → 分类归队（completely free → Release / has free → return 给调用者 / still full → fullSwept） | 成功获取一个有空闲槽位的 Region（或所有 unswept 都已耗尽，转 GetFreshRegion） |
| **Background Sweep** | `CentralRegionPool::BackgroundSweep()` 由 `FinalizerProcessor::BackgroundSweepLoop()` 调用 | gc-helper 线程主循环检测到 `shouldBackgroundSweep_ == true` 时执行 | 遍历全部 36 个 SizeClass 的 `partialUnswept` + `fullUnswept` 链表，逐个 TakeHead → CAS 抢占 → SweepRegion → 分类归队；每处理 6 个 SizeClass 调用 `yield()` 让出 CPU | 所有 36 个 SizeClass 的 unswept 链表全部清空，`sweepDone_ = true` |
| **ForceFinishSweep** | `CentralRegionPool::ForceFinishSweep()` | 下轮 GC `MarkSetup()` 的第一步调用 | 同步遍历全部 36 个 SizeClass，drain 两个 unswept 链表（partialUnswept + fullUnswept），调用 `SweepAndClassify()` 完成清扫和分类 | 所有 unswept 链表为空，`sweepDone_ = true` |

> **⚠ 设计 vs 实际差异**：设计文档 (`Step4_并发清扫详细设计.md` L488-489, `Go风格GC迁移方案.md` L457) 明确描述 `ConcurrentSweep()` 中应调用 `NotifyToSweep()` 唤醒 gc-helper 线程进行后台 Sweep。`NotifyToSweep()` 方法已在 `FinalizerProcessor.h` L99 中实现（设置 `shouldBackgroundSweep_ = true` + `Notify()`），但在 `MarkSweepCollector::ConcurrentSweep()` (L621-643) 中**未被调用**。实际代码只执行了 `PrepareForSweep()`。这意味着当前实现中后台 Sweep **不会自动触发**，清扫完全依赖 Lazy Sweep（分配路径）和 ForceFinishSweep（下轮 GC 兜底）。这是一个待修复的接线遗漏。

### Step 4.1: SweepRegion

> 设计文档: `doc/Step4.1_SweepRegion详细实现.md`
>
> **目标**: 单 Region 的 bitmap NOT 清扫算法 + sweepState 三态 CAS 状态机。

#### MSRegionInfo 结构总览

`MSRegionInfo` 被精确控制在 **64 字节 = 1 个缓存行**，通过 `static_assert` 编译期校验。它是每个 MS Region 的核心元数据，存储在堆前方的镜像数组 `MSRegionInfoArray` 中。

```cpp
struct MSRegionInfo {                    // 共 64 字节, offset 标注见源文件
    uintptr_t   allocPtr;               // +0   当前 bump-pointer 位置 (编译器快速路径 ABI 要求 offset 0)
    uintptr_t   regionEnd;              // +8   Region 结束地址

    int32_t     nextRegionIdx;          // +16  链表 next 索引
    int32_t     prevRegionIdx;          // +20  链表 prev 索引

    uint64_t*   allocBitmap;            // +24  分配位图: 1=空闲, 0=已占用
    uint64_t*   markBitmap;             // +32  标记位图: 1=存活, 0=死亡

    uint16_t    totalSlotCount;         // +40  Region 中的总槽位数
    uint16_t    freeSlotCount;          // +42  当前空闲槽位数
    uint16_t    liveSlotCount;          // +44  上次 GC 后存活槽位数
    uint16_t    rawPointerObjCount;     // +46  弱引用引用计数

    uint8_t      sizeClass;             // +48  Size Class 索引 (0~35)
    MSSweepState sweepState;            // +49  清扫状态
    MSRegionType regionType;            // +50  Region 类型

    uint8_t     isMarked     : 1;       // +51.0  GC 标记标志 (大对象用)
    uint8_t     isEnqueued   : 1;       // +51.1  弱引用队列标志
    uint8_t     isResurrected: 1;       // +51.2  Finalizer 复活标志
    uint8_t     flagsPadding : 5;       // +51.3  保留位

    uint32_t    sweepGen;               // +52  清扫代际 (用于 Lazy Sweep CAS 协议)
    uint64_t    reserved;               // +56  扩展字段 (低 32 位 = searchHint, 高 32 位保留)
};
static_assert(sizeof(MSRegionInfo) == 64);
static_assert(offsetof(MSRegionInfo, allocPtr) == 0);
```

**关键类型枚举**：

| 枚举 | 值 | 说明 |
|------|---|------|
| `MSSweepState::UNSWEPT` | 0 | 未清扫 (GC 标记完成后的初始状态) |
| `MSSweepState::SWEEPING` | 1 | 正在清扫 (CAS 抢占后的中间态) |
| `MSSweepState::SWEPT` | 2 | 已清扫 (bitmap NOT 完成, 可用于分配) |

#### 实现位置

| 函数 | 文件 | 行号 | 说明 |
|------|------|------|------|
| `MSRegionInfo::SweepRegion()` | `MSRegionInfo.cpp` | L216-248 | 核心清扫算法：bitmap NOT + 统计 |
| `MSRegionInfo::HasFreeSpace()` | `MSRegionInfo.cpp` | L304-317 | 按 sweepState 判断是否有空闲槽位 |
| `MSRegionInfo::IsCompletelyFree()` | `MSRegionInfo.h` | 内联 | `freeSlotCount == totalSlotCount` |
| `MSRegionInfo::CasSweepGen()` | `MSRegionInfo.h` | 内联 | `reinterpret_cast<atomic<uint32_t>*>(&sweepGen)->compare_exchange_strong` |

#### SweepRegion 算法详解

核心思想：**markBitmap 按位取反 → allocBitmap**，一次 NOT 操作就完成了 64 个槽位的存活/空闲状态翻转。

```cpp
// MSRegionInfo.cpp L216-248  —— 按实际代码逐行注释
size_t MSRegionInfo::SweepRegion()
{
    // 1. 计算 bitmap word 数量 (每个 word 覆盖 64 个槽位)
    size_t wordCount = (totalSlotCount + 63) / 64;
    int liveCount = 0;

    // 2. 核心循环: 每次处理 64 个槽位
    for (size_t i = 0; i < wordCount; i++) {
        // 核心等式: markBitmap[1=alive] 取反 → allocBitmap[1=free]
        allocBitmap[i] = ~markBitmap[i];
        // popcount 统计存活位数, 编译器映射到硬件 POPCNT 指令
        liveCount += __builtin_popcountll(markBitmap[i]);
    }

    // 3. 尾 word mask 处理：最后一个 word 可能不满 64 位
    //    例如 totalSlotCount=100 → 最后 word 只有 100%64=36 个有效位
    //    高 28 位必须 mask 为 0, 否则分配时会把无效位误判为空闲槽位
    size_t trailingBits = totalSlotCount % 64;
    if (trailingBits != 0) {
        allocBitmap[wordCount - 1] &= (1ULL << trailingBits) - 1;
    }

    // 4. 清零 markBitmap (为下轮 GC Concurrent Mark 准备干净的 bitmap)
    memset(markBitmap, 0, wordCount * sizeof(uint64_t));

    // 5. 更新统计信息
    liveSlotCount = static_cast<uint16_t>(liveCount);
    freeSlotCount = totalSlotCount - liveSlotCount;

    // 6. 状态转移: → SWEPT
    sweepState = MSSweepState::SWEPT;

    // 7. 重置 searchHint (reserved 低 32 位), AllocFromBitmap 从 word 0 开始搜索
    reserved = (reserved & 0xFFFFFFFF00000000ULL);

    // 8. 返回释放的字节数 (freeSlotCount × slotSize)
    size_t slotSize = SizeClassInfo::ClassToSize(sizeClass);
    return static_cast<size_t>(freeSlotCount) * slotSize;
}
```

**时间复杂度**: $O(n/64)$，其中 $n$ = `totalSlotCount`。每个 bitmap word 只需一次 NOT + 一次 POPCNT。

#### sweepGen CAS 协议

`sweepGen` 是三路 Sweep 协作的核心同步机制。每个 Region 的 `sweepGen` 字段表示其相对于 `globalSweepGen_` 的清扫进度：

```
单个 Region 的 sweepGen 状态机 (sg = 当前 globalSweepGen_):

  sweepGen == sg-2        sweepGen == sg-1        sweepGen == sg
  ┌───────────┐   CAS     ┌───────────┐  store   ┌──────────┐
  │  UNSWEPT  │──(抢占)──→│  SWEEPING  │──(完成)──→│  SWEPT   │
  └───────────┘           └───────────┘           └──────────┘
                                                       │
                     PrepareForSweep()                 │
                     globalSweepGen_ += 2              │
                     sg' = sg + 2                      │
                     region.sweepGen == sg              │
                     = sg' - 2  → 变回 UNSWEPT         │
                     ←─────────────────────────────────┘
```

**CAS 操作** (`CasSweepGen`, MSRegionInfo.h 内联):
```cpp
bool CasSweepGen(uint32_t expected, uint32_t desired) {
    return reinterpret_cast<std::atomic<uint32_t>*>(&sweepGen)
        ->compare_exchange_strong(expected, desired, std::memory_order_acq_rel);
}
```

**三路使用方式统一**：不论是 Lazy Sweep、Background Sweep 还是 ForceFinishSweep，清扫一个 Region 的标准流程都是：
1. `CasSweepGen(sg-2, sg-1)` — 原子抢占清扫权（失败则跳过，已被其他线程抢走）
2. `SweepRegion()` — 执行 bitmap NOT
3. `SetSweepGen(sg)` — 标记为已清扫

这保证了同一个 Region **只会被清扫一次**，即使三条路径并发竞争。

#### HasFreeSpace 判断逻辑

```cpp
// MSRegionInfo.cpp L304-317
bool MSRegionInfo::HasFreeSpace() const {
    // Case 1: 已清扫 → 看 freeSlotCount (bitmap 数据有效)
    if (sweepState == MSSweepState::SWEPT) {
        return freeSlotCount > 0;
    }
    // Case 2: 未清扫但 freeSlotCount > 0 (之前分配未用尽)
    if (freeSlotCount > 0) {
        return true;
    }
    // Case 3: 初次填充的 BUMP Region, 检查 bump-pointer 尾部空间
    //         (仅在首次 Sweep 前有效, Sweep 后应走 Case 1)
    size_t slotSize = SizeClassInfo::ClassToSize(sizeClass);
    return allocPtr + slotSize <= regionEnd;
}
```

**用途**：在 `GetRegion()` 的 Priority 3 (fullUnswept) 路径中，Sweep 完一个 Region 后调用 `HasFreeSpace()` 判断是否产生了空闲槽位。如果有则直接返回给调用者，否则归入 `fullSwept`。

#### Review 重点

- **尾 word mask**: `SweepRegion` 最后一个 bitmap word 可能不满 64 位，`(1ULL << trailingBits) - 1` 确保无效位被清零。验证 `totalSlotCount % 64 == 0` 时不进入 mask 分支（避免 `1ULL << 64` UB）
- **popcount 准确性**: `__builtin_popcountll` 编译器映射到硬件 POPCNT 指令（x86_64/aarch64 均有硬件支持）
- **markBitmap 清零时机**: `SweepRegion()` 内部 `memset(markBitmap, 0, ...)` 清零。但注意 `MarkSetup()` 中也会 `VisitAllRegions` 再次 `memset` 清零 —— 两处清零存在冗余，是否是有意为之（防御性编程）需要确认
- **CAS 内存序**: `CasSweepGen` 使用 `memory_order_acq_rel`，保证 CAS 成功后后续的 `SweepRegion()` 看到正确的 bitmap 数据
- **sweepState vs sweepGen**: `sweepState` 是 Region 本地的 enum（UNSWEPT/SWEEPING/SWEPT），`sweepGen` 是全局代际对比用的整数。当前代码中 `SweepRegion()` 只设置 `sweepState = SWEPT`，但 `sweepGen` 的 CAS/Store 在调用者侧完成 —— 两者的一致性需要 Review

#### 小结

**做了什么**: 实现单 Region 粒度的清扫算法。核心是 `SweepRegion()` 的 bitmap NOT 翻转——将 markBitmap 按位取反写入 allocBitmap，一次 NOT 处理 64 个槽位，配合 POPCNT 统计存活数。同时实现 sweepGen CAS 三态协议（UNSWEPT → SWEEPING → SWEPT），使三路 Sweep 可以无锁并发竞争同一 Region 的清扫权。

**效果**:
- 清扫时间复杂度 $O(n/64)$，每个 bitmap word 仅需一次 NOT + 一次 POPCNT，远优于逐对象遍历
- sweepGen CAS 协议保证同一 Region 只被清扫一次，三路 Sweep 并发安全无需额外锁
- `MSRegionInfo` 精确控制在 64 字节（1 个缓存行），元数据访问无缓存行分裂

**改进点**:
1. **markBitmap 双重清零**: `SweepRegion()` 内部 `memset(markBitmap, 0, ...)` 清零，`MarkSetup()` 中 `VisitAllRegions` 又清零一次。两处冗余，应确认是防御性编程（保留）还是遗漏（移除一处）
2. **sweepState 与 sweepGen 分离**: `SweepRegion()` 设置 `sweepState = SWEPT`，但 `sweepGen` 的 CAS/Store 在调用者完成。两个状态表示分离增加了出错风险——若调用者忘记 `SetSweepGen(sg)`，Region 的 sweepState 已是 SWEPT 但 sweepGen 仍为 sg-1，可能导致下轮被误判。建议将 `SetSweepGen(sg)` 移入 `SweepRegion()` 内部，或添加断言校验
3. **尾 word UB 风险**: `totalSlotCount % 64 == 0` 时 `trailingBits = 0`，不进入 mask 分支。但如果错误地进入，`1ULL << 64` 是未定义行为。建议添加 `assert(trailingBits < 64)` 防御

### Step 4.2: Sweep 调度与 CentralRegionPool 集成

> 设计文档: `doc/Step4.2_Sweep调度与CentralRegionPool集成.md`
>
> **目标**: sweepGen 代际机制 + Lazy Sweep 嵌入分配路径 + ForceFinishSweep 兜底。CentralRegionPool 是三路 Sweep 的调度中心，对应 Go runtime 的 `mcentral`。

#### CentralRegionPool 核心数据结构

```cpp
// CentralRegionPool.h — 核心字段
class CentralRegionPool {
    static std::atomic<uint32_t> globalSweepGen_;  // 全局清扫代际, 初始值=2, 每轮 GC +=2
    std::atomic<bool> sweepDone_{true};             // 本轮清扫是否完成
    MSSweepStats sweepStats_;                       // 清扫统计 (forceSwept/bgSwept/lazySwept/released)
    std::atomic<size_t> totalMSRegionBytes_{0};     // MS Region 总字节数 (GC 触发阈值用)

    // 每个 SizeClass 一个桶, 共 36 个 (kNumSizeClasses=36)
    SizeClassBucket buckets_[kNumSizeClasses];

    RegionManager* regionManager_;                  // 底层物理 Region 管理器

    // 大对象链表 (Tier 1: 堆内大对象, MS_REGION_LARGE)
    std::mutex largeLock_;
    MSRegionList largeRegionList_;
};
```

**SizeClassBucket 四链表结构**（对应 Go mcentral 的 partial/full × swept/unswept）：

```cpp
struct SizeClassBucket {
    std::mutex lock;              // 保护下面四个链表的并发访问
    MSRegionList partialSwept;    // 有空闲槽位 + 已清扫 → 分配最快路径
    MSRegionList fullSwept;       // 无空闲槽位 + 已清扫
    MSRegionList partialUnswept;  // 有空闲槽位 + 未清扫 → Lazy Sweep 优先目标
    MSRegionList fullUnswept;     // 无空闲槽位 + 未清扫 → Lazy Sweep 备选
};
```

#### 实现位置

| 函数 | 文件 | 行号 | 说明 | 统计累计字段 |
|------|------|------|------|-------------|
| `GetRegion(cls)` | `CentralRegionPool.cpp` | L30-112 | 四级优先级分配 + 内嵌 Lazy Sweep | `lazySweptRegions` |
| `GetFreshRegion(cls)` | `CentralRegionPool.cpp` | L115-142 | 从 RegionManager 分配全新 Region | — |
| `ReturnPartialRegion()` | `CentralRegionPool.cpp` | L155-165 | TL 归还部分空闲 Region | — |
| `ReturnFullRegion()` | `CentralRegionPool.cpp` | L167-178 | TL 归还已满 Region | — |
| `ReleaseRegionInternal()` | `CentralRegionPool.cpp` | L185-200 | 释放 Region 回 RegionManager (物理回收) | — |
| `ForceFinishSweep()` | `CentralRegionPool.cpp` | L206-240 | 遍历 36 个 SC 的 unswept, 同步全部清扫 | `forceSweptRegions` |
| `SweepAndClassify()` | `CentralRegionPool.cpp` | L243-265 | 单 Region: CAS→Sweep→分类归队 | `forceSweptRegions` |
| `PrepareForSweep()` | `CentralRegionPool.cpp` | L266-290 | O(1) 代际翻转: Swap + globalSweepGen_+=2 | — |
| `BackgroundSweep()` | `CentralRegionPool.cpp` | L296-312 | 后台遍历 36 个 SC 的 unswept 链表 | `bgSweptRegions` |
| `SweepUnsweptList()` | `CentralRegionPool.cpp` | L314-348 | 批量清扫一个 unswept 链表 | `bgSweptRegions` |
| `SweepLargeRegions()` | `CentralRegionPool.cpp` | L411-450 | Tier1 大对象 Eager Sweep (两遍: 收集+释放) | — |

#### sweepGen 代际机制

`globalSweepGen_` 是一个全局原子计数器，初始值 = 2，每轮 GC 递增 2。每个 Region 的 `sweepGen` 字段记录其"被清扫时的代际值"。通过比较 `region.sweepGen` 与 `globalSweepGen_`，可以在 O(1) 时间内判断 Region 是否需要清扫，**不需要遍历链表修改状态**。

```
globalSweepGen_ 生命周期:

  初始化: globalSweepGen_ = 2
  GC #1: PrepareForSweep() → globalSweepGen_ = 4
  GC #2: PrepareForSweep() → globalSweepGen_ = 6
  GC #3: PrepareForSweep() → globalSweepGen_ = 8
  ...

单个 Region 的 sweepGen 对比 (sg = 当前 globalSweepGen_):
  region.sweepGen == sg - 2  → 未清扫 (上轮 GC 结束时产生, 等待清扫)
  region.sweepGen == sg - 1  → 正在清扫 (某线程 CAS 抢占成功, 中间态)
  region.sweepGen == sg      → 已清扫 (或新分配的 Fresh Region)
```

**为什么初始值是 2 而非 0**：因为 CAS 使用 `sg-2` 作为 expected 值。初始值 = 2 确保第一轮 GC 时 `sg-2 = 0`，恰好匹配新 Region 的默认 `sweepGen = 0`。

#### GetRegion 四级优先级 (Lazy Sweep 入口)

`GetRegion(cls)` 是 Mutator 分配路径的核心，也是 Lazy Sweep 的触发入口。当 `AllocBufferMS` 的 TL Region 耗尽时，调用此函数获取新 Region。

```cpp
// CentralRegionPool.cpp L30-112  —— 按实际代码注释
MSRegionInfo* CentralRegionPool::GetRegion(uint8_t cls) {
    SizeClassBucket& bucket = buckets_[cls];
    uint32_t sg = globalSweepGen_.load(memory_order_acquire);

    // ---- Priority 1: partialSwept (最快: 已清扫 + 有空闲, 无需 Sweep) ----
    {
        lock_guard lg(bucket.lock);
        MSRegionInfo* region = bucket.partialSwept.TakeHead();
        if (region) {
            region->regionType = MS_REGION_PARTIAL;
            return region;   // 直接返回, 零清扫开销
        }
    }

    // ---- Priority 2: partialUnswept (Lazy Sweep: 大概率有空闲) ----
    for (;;) {
        MSRegionInfo* region;
        { lock_guard lg(bucket.lock); region = bucket.partialUnswept.TakeHead(); }
        if (!region) break;

        if (region->CasSweepGen(sg - 2, sg - 1)) {    // CAS 抢占清扫权
            region->SweepRegion();                      // bitmap NOT
            region->SetSweepGen(sg);                    // 标记已清扫
            sweepStats_.lazySweptRegions++;

            if (region->IsCompletelyFree()) {           // 全空 → 物理回收
                sweepStats_.releasedRegions++;
                ReleaseRegionInternal(region);
                continue;  // 回收后继续取下一个
            }
            region->regionType = MS_REGION_PARTIAL;
            return region;  // 有空闲槽位, 返回
        }
        // CAS 失败: 另一线程正在清扫此 Region, 跳过
    }

    // ---- Priority 3: fullUnswept (Lazy Sweep: Sweep 后可能变 partial) ----
    for (;;) {
        MSRegionInfo* region;
        { lock_guard lg(bucket.lock); region = bucket.fullUnswept.TakeHead(); }
        if (!region) break;

        if (region->CasSweepGen(sg - 2, sg - 1)) {
            region->SweepRegion();
            region->SetSweepGen(sg);
            sweepStats_.lazySweptRegions++;

            if (region->IsCompletelyFree()) {
                sweepStats_.releasedRegions++;
                ReleaseRegionInternal(region);
                continue;
            }
            if (region->HasFreeSpace()) {               // Sweep 后产生了空闲
                region->regionType = MS_REGION_PARTIAL;
                return region;
            }
            // 仍然全满 → 归入 fullSwept
            { lock_guard lg(bucket.lock); bucket.fullSwept.Prepend(region); }
            continue;
        }
    }

    // ---- Priority 4: 返回 nullptr, 调用者应调用 GetFreshRegion() ----
    return nullptr;
}
```

**关键行为总结**：
- Priority 1 无清扫开销，是热路径（GC 清扫进度领先于分配速度时走此路径）
- Priority 2/3 在分配路径上内嵌 Sweep（Lazy Sweep），**分配线程承担清扫成本**
- 全空 Region 立即 `ReleaseRegionInternal` 物理回收给 `RegionManager`，减少内存占用
- CAS 失败 = 其他线程（Background Sweep / ForceFinish / 另一个 Lazy Sweep）抢先处理，安全跳过

#### GetFreshRegion (分配全新 Region)

当 `GetRegion()` 返回 nullptr（所有 unswept 链表耗尽），调用此函数：

```cpp
// CentralRegionPool.cpp L115-142
MSRegionInfo* CentralRegionPool::GetFreshRegion(uint8_t cls) {
    size_t unitsPerMSRegion = MS_REGION_SIZE / RegionInfo::UNIT_SIZE;
    RegionInfo* rawRegion = regionManager_->TakeRegion(
        unitsPerMSRegion, SMALL_SIZED_UNITS, true);       // 从 RegionManager 分配物理 Region
    if (!rawRegion) return nullptr;                         // OOM

    uintptr_t regionStart = rawRegion->GetRegionStart();
    MSRegionInfo* msInfo = MSRegionInfoArray::GetRegionInfo(regionStart);  // 查镜像数组
    msInfo->InitForSizeClass(cls);                          // 初始化 bitmap、统计、状态
    msInfo->allocPtr = regionStart;
    msInfo->regionEnd = regionStart + MS_REGION_SIZE;
    msInfo->SetSweepGen(globalSweepGen_.load());            // 标记为"已清扫" (新 Region 无需 Sweep)
    totalMSRegionBytes_ += MS_REGION_SIZE;                  // GC 触发阈值计数
    return msInfo;
}
```

**状态**: 返回的 Region 已设置 `sweepGen = globalSweepGen_`（当前代际），被视为 SWEPT 状态，可直接用于 bump-pointer 分配。

#### ReturnPartialRegion / ReturnFullRegion (TL 归还)

当 `AllocBufferMS` 的 TL Region 被换出时，归还给中心池：

```cpp
// CentralRegionPool.cpp L155-178  —— 根据 sweepGen 归入正确链表
void ReturnPartialRegion(MSRegionInfo* region, uint8_t cls) {
    SizeClassBucket& bucket = buckets_[cls];
    lock_guard lg(bucket.lock);
    region->regionType = MS_REGION_PARTIAL;
    uint32_t sg = globalSweepGen_.load();
    if (region->GetSweepGen() == sg)
        bucket.partialSwept.Prepend(region);      // 已清扫 → partialSwept
    else
        bucket.partialUnswept.Prepend(region);    // 未清扫 → partialUnswept
}

void ReturnFullRegion(MSRegionInfo* region, uint8_t cls) {
    // 同理, 归入 fullSwept 或 fullUnswept
}
```

**关键**: 归还时通过比较 `region.sweepGen == globalSweepGen_` 判断链表归属。如果 Mutator 持有 Region 期间发生了 GC（globalSweepGen 递增），该 Region 会被归入 unswept 链表。

#### SweepAndClassify 分类规则

`SweepAndClassify` 是 `ForceFinishSweep` 的内部辅助函数（Lazy Sweep 路径在 `GetRegion` 中内联了类似逻辑）：

```cpp
// CentralRegionPool.cpp L243-265
void SweepAndClassify(MSRegionInfo* region, uint32_t sg, SizeClassBucket& bucket) {
    // 1. CAS 抢占清扫权 (与 Lazy Sweep / Background Sweep 竞争)
    if (!region->CasSweepGen(sg - 2, sg - 1)) {
        return;  // 另一线程正在处理, 安全放弃
    }

    // 2. 执行 bitmap NOT 清扫
    region->SweepRegion();
    region->SetSweepGen(sg);
    sweepStats_.forceSweptRegions++;       // ForceFinish 路径统计

    // 3. 三路分类归队
    if (region->IsCompletelyFree()) {       // 全空 → 物理回收
        sweepStats_.releasedRegions++;
        ReleaseRegionInternal(region);
    } else if (region->HasFreeSpace()) {    // 有空闲 → partialSwept
        lock_guard lg(bucket.lock);
        bucket.partialSwept.Prepend(region);
    } else {                                // 全满 → fullSwept
        lock_guard lg(bucket.lock);
        bucket.fullSwept.Prepend(region);
    }
}
```

**分类归队后的状态**: Region 必定位于 `*Swept` 链表中（或已物理回收），不会残留在 unswept 链表。

#### PrepareForSweep 代际翻转 (O(1) 核心)

```cpp
// CentralRegionPool.cpp L266-290
void PrepareForSweep() {
    sweepStats_.Reset();                      // 重置上轮统计

    // O(1) 代际翻转: 把 swept 链表直接 swap 成 unswept 链表
    // 前提: ForceFinishSweep 保证 unswept 链表已空, 所以 swap 安全
    for (uint8_t cls = 0; cls < kNumSizeClasses; cls++) {
        SizeClassBucket& bucket = buckets_[cls];
        lock_guard lg(bucket.lock);
        bucket.partialSwept.Swap(bucket.partialUnswept);   // partial: swept↔unswept
        bucket.fullSwept.Swap(bucket.fullUnswept);         // full:    swept↔unswept
    }

    globalSweepGen_.fetch_add(2, memory_order_release);    // 代际 +2: 使所有 Region 逻辑变为"未清扫"
    sweepDone_.store(false, memory_order_release);         // 标记本轮清扫未完成
}
```

**调用时机**: `MarkSweepCollector::ConcurrentSweep()` (L621) — GC 标记阶段结束后的最后一步。

**Swap 语义**:
- Swap 前: `partialSwept` 有刚 Sweep 完的 Region, `partialUnswept` 为空 (被 ForceFinish 清空了)
- Swap 后: `partialSwept` 为空, `partialUnswept` 装满了上轮 swept Region
- 配合 `globalSweepGen_ += 2`: 这些 Region 的 `sweepGen` 现在 = `sg - 2`，被三路 Sweep 视为 UNSWEPT

**核心优势**: 不需要遍历所有 Region 修改状态，O(36) 链表指针交换 + 一次原子加法。

#### ForceFinishSweep (兜底保障)

```cpp
// CentralRegionPool.cpp L206-240
void ForceFinishSweep() {
    uint32_t sg = globalSweepGen_.load();

    for (uint8_t cls = 0; cls < kNumSizeClasses; cls++) {
        SizeClassBucket& bucket = buckets_[cls];

        // Drain partialUnswept
        for (;;) {
            MSRegionInfo* region;
            { lock_guard lg(bucket.lock); region = bucket.partialUnswept.TakeHead(); }
            if (!region) break;
            SweepAndClassify(region, sg, bucket);
        }

        // Drain fullUnswept
        for (;;) {
            MSRegionInfo* region;
            { lock_guard lg(bucket.lock); region = bucket.fullUnswept.TakeHead(); }
            if (!region) break;
            SweepAndClassify(region, sg, bucket);
        }
    }

    sweepDone_.store(true, memory_order_release);
}
```

**调用时机**: `MarkSweepCollector::MarkSetup()` 的**第一步** (L100)。

**为什么需要**: 确保上轮 GC 的所有 Region 都已清扫完毕，才能安全地开始新一轮标记。如果 Lazy Sweep / Background Sweep 在两轮 GC 之间完成了所有清扫，则 `ForceFinishSweep` 的循环体不会执行（unswept 链表已为空），开销 ≈ 0。

**退出条件**: 36 个 SizeClass 的 `partialUnswept` + `fullUnswept` 链表全部 drain 为空。

#### SweepLargeRegions (大对象 Eager Sweep)

大对象 (Tier 1: 堆内 `MS_REGION_LARGE` Region) 不走 Lazy Sweep，而是在 `PostMarkTermination` 中立即清扫：

```cpp
// CentralRegionPool.cpp L411-450  —— 两遍法: 收集死亡 → 释放
size_t SweepLargeRegions() {
    vector<MSRegionInfo*> deadRegions;
    size_t freedBytes = 0;

    // Pass 1 (持锁): 遍历 largeRegionList_, 收集未标记的死亡 Region
    {
        lock_guard lg(largeLock_);
        largeRegionList_.ForEach([&](MSRegionInfo* region) {
            if (region->isMarked == 0) toRemove.push_back(region);
        });
        for (auto* r : toRemove) {
            largeRegionList_.Remove(r);
            deadRegions.push_back(r);
        }
    }

    // Pass 2 (无锁): 逐个释放死亡 Region 回 RegionManager
    for (MSRegionInfo* msInfo : deadRegions) {
        uintptr_t regionStart = msInfo->allocPtr;
        size_t regionBytes = msInfo->regionEnd - regionStart;
        freedBytes += regionBytes;
        msInfo->Reset();
        RegionInfo* rawRegion = RegionInfo::GetRegionInfoAt(regionStart);
        regionManager_->ReclaimRegion(rawRegion);
        totalMSRegionBytes_ -= regionBytes;
    }
    return freedBytes;
}
```

**调用时机**: `MarkSweepCollector::CollectLargeGarbage()` (L608)，在 `PostMarkTermination` 中调用。

**为什么 Eager 而非 Lazy**: 大对象 Region 不参与 SizeClass 桶管理，没有 `sweepGen` CAS 协议。标记完成后即可判定存活性 (`isMarked`)，立即回收释放内存。

#### Review 重点

- **PrepareForSweep 前置条件**: Swap 要求 unswept 链表为空。这通过 `MarkSetup → ForceFinishSweep()` 保证。如果 `ForceFinishSweep` 被跳过（代码路径变更），Swap 会导致 Region 丢失
- **锁粒度**: `GetRegion()` 中每次 TakeHead 都取锁放锁（乐观尝试），而非持锁遍历。这降低了锁竞争但增加了 lock/unlock 次数。在 Region 数量多时是否成为瓶颈需要关注
- **统计分离**: `lazySweptRegions` (GetRegion路径) vs `forceSweptRegions` (ForceFinish路径) vs `bgSweptRegions` (Background路径) 分别统计，可用于 DFX 诊断清扫策略是否均衡
- **ReleaseRegionInternal 路径**: `ReleaseBitmaps()` → `Reset()` → `totalMSRegionBytes_ -= MS_REGION_SIZE` → `regionManager_->ReclaimRegion()`。确认 bitmap 内存被正确释放（避免泄漏）
- **SweepLargeRegions 两遍法**: Pass 1 持 `largeLock_` 遍历 + Remove，Pass 2 无锁释放。这避免了在持锁期间调用 `ReclaimRegion`（可能触发其他锁），降低死锁风险

#### 小结

**做了什么**: 将 sweepGen 代际机制和三路 Sweep 调度集成到 CentralRegionPool。`PrepareForSweep()` 通过 O(1) 链表 Swap + `globalSweepGen_ += 2` 完成代际翻转；`GetRegion()` 四级优先级内嵌 Lazy Sweep；`ForceFinishSweep()` 在下轮 GC 前兜底清扫所有残留 Region；`SweepLargeRegions()` 两遍法 Eager Sweep 大对象。

**效果**:
- O(1) 代际翻转：不遍历 Region，36 次链表指针交换 + 一次原子加法即完成全堆状态切换
- Lazy Sweep 嵌入分配路径：清扫成本分摊到 Mutator 分配中，GC 停顿仅包含 `PrepareForSweep()`（微秒级）
- 三路分类归队（completely free → 物理回收 / has free → partialSwept / still full → fullSwept）自动维护链表一致性
- ForceFinishSweep 保证下轮 GC 开始时 unswept 链表为空，是 PrepareForSweep Swap 的前置条件

**改进点**:
1. **锁粒度**: `GetRegion()` 每次 TakeHead 都取锁放锁（乐观策略），在 Region 数量大时 lock/unlock 次数多。可考虑批量取出多个 Region 减少锁操作，或改用 lock-free 链表
2. **Lazy Sweep 的分配延迟**: Priority 2/3 路径中 Mutator 线程同步执行 `SweepRegion()`，bitmap NOT 的耗时直接加到分配延迟上。如果 Region 槽位多（小 SizeClass），单次清扫可能耗时数微秒。Background Sweep 接线后可减轻此压力
3. **全空 Region 回收策略**: Lazy Sweep 中 `IsCompletelyFree()` 的 Region 立即物理回收给 RegionManager。在内存压力大时这是好的，但在分配密集场景下可能导致频繁 TakeRegion/ReclaimRegion 震荡。可考虑保留少量空 Region 作为缓冲池
4. **大对象 Eager Sweep 的 STW 影响**: `SweepLargeRegions()` 在 PostMarkTermination 阶段同步执行。大对象数量多时，两遍遍历的耗时会延长 PostMark 阶段（虽非严格 STW，但 Handshake 期间会阻塞未响应的 Mutator）

### Step 4.3: 后台 Sweep 与 FinalizerProcessor 集成

> 设计文档: `doc/Step4.3_后台Sweep与FinalizerProcessor集成.md`
>
> **目标**: 将后台 Sweep 集成到 FinalizerProcessor（gc-helper 线程），保证 Finalizer 先于 Sweep。gc-helper 是 Runtime 唯一的后台辅助线程，复用它可避免额外的线程开销。

#### 集成位置与修改点

| 文件 | 修改行 | 改动要点 |
|------|--------|----------|
| `FinalizerProcessor.h` L99-103 | 新增 `NotifyToSweep()`: 设置 `shouldBackgroundSweep_=true` + `Notify()` 唤醒 |
| `FinalizerProcessor.h` L112 | 新增 `BackgroundSweepLoop()` 私有方法声明 |
| `FinalizerProcessor.h` L137 | 新增 `std::atomic<bool> shouldBackgroundSweep_` 原子标志 |
| `FinalizerProcessor.cpp` L82 | 构造函数中 `shouldBackgroundSweep_.store(false)` 初始化 |
| `FinalizerProcessor.cpp` L99-100 | `Run()` 主循环: 在 ProcessFinalizables 之后检查 `shouldBackgroundSweep_` |
| `FinalizerProcessor.cpp` L118 | Wait 循环: 增加 `shouldBackgroundSweep_` 作为唤醒条件之一 |
| `FinalizerProcessor.cpp` L308-320 | 新增 `BackgroundSweepLoop()` 实现 (带 `#ifdef MRT_USE_MARK_SWEEP`) |

#### gc-helper 线程主循环 (Run)

`FinalizerProcessor::Run()` 是 gc-helper 线程的主循环，按**严格优先级顺序**处理四类任务：

```cpp
// FinalizerProcessor.cpp L86-125  —— 按实际代码注释
void FinalizerProcessor::Run() {
    Init();
    NotifyStarted();
    while (running) {
        // ① Finalizer 处理 (最高优先级)
        //    必须先于 Sweep: 确保 Finalizer 引用的对象在清扫前被正确处理
        if (hasFinalizableJob.load(relaxed)) {
            ProcessFinalizables();
        }

        // ② 后台 Sweep (第二优先级, MS GC 专用)
        //    在 Finalizer 之后执行, 确保不会清扫 Finalizer 还需要的内存
        if (shouldBackgroundSweep_.load(relaxed)) {
            BackgroundSweepLoop();
        }

        // ③ 喂饱饥饿的 AllocBuffer (第三优先级)
        if (shouldFeedHungryBuffers.load(relaxed)) {
            FeedHungryBuffers();
        }

        // ④ 回收堆垃圾 (最低优先级)
        if (shouldReclaimHeapGarbage.load(relaxed)) {
            ReclaimHeapGarbage();
        }

        // Wait: 等待任意标志变为 true 或超时
        while (running) {
            Wait(iterationWaitTime);           // condvar wait_for (默认超时)
            if (hasFinalizableJob || shouldReclaimHeapGarbage ||
                shouldFeedHungryBuffers || shouldBackgroundSweep_) {
                break;  // 有任务, 退出等待
            }
        }
    }
    Fini();
}
```

**排序约束**: **Finalizer → Sweep → Feed → Reclaim**。Finalizer 必须在 Sweep 之前执行，防止析构函数访问已被清扫回收的内存。

#### NotifyToSweep 入口

```cpp
// FinalizerProcessor.h L99-103
void NotifyToSweep() {
    shouldBackgroundSweep_.store(true, memory_order_release);
    Notify();  // condvar notify_one → 唤醒 gc-helper 的 Wait
}
```

**设计预期调用位置**: `MarkSweepCollector::ConcurrentSweep()` 中，在 `PrepareForSweep()` 之后调用，唤醒 gc-helper 线程开始后台清扫。

> **⚠ 接线遗漏 (同 Step 4 概述)**: `NotifyToSweep()` 方法已实现，但 `ConcurrentSweep()` 中**未调用**。当前 `shouldBackgroundSweep_` 永远不会被设置为 `true`，后台 Sweep 路径实际**不会触发**。

#### BackgroundSweepLoop 实现

```cpp
// FinalizerProcessor.cpp L308-320
void FinalizerProcessor::BackgroundSweepLoop() {
#ifdef MRT_USE_MARK_SWEEP
    RegionSpace& space = reinterpret_cast<RegionSpace&>(Heap::GetHeap().GetAllocator());
    CentralRegionPool* centralPool = space.GetCentralPool();
    if (centralPool != nullptr) {
        centralPool->BackgroundSweep();     // 遍历 36 个 SizeClass 的 unswept 链表
    }
#endif
    shouldBackgroundSweep_.store(false, memory_order_relaxed);  // 完成, 清除标志
}
```

`BackgroundSweepLoop` 的实际工作委托给 `CentralRegionPool::BackgroundSweep()`：

```cpp
// CentralRegionPool.cpp L296-312
void CentralRegionPool::BackgroundSweep() {
    uint32_t sg = globalSweepGen_.load();

    for (uint8_t cls = 0; cls < kNumSizeClasses; cls++) {
        SizeClassBucket& bucket = buckets_[cls];
        SweepUnsweptList(bucket, bucket.partialUnswept, sg);  // 清扫 partial
        SweepUnsweptList(bucket, bucket.fullUnswept, sg);     // 清扫 full

        if (cls % 6 == 5)                                     // 每 6 个 SC yield 一次
            std::this_thread::yield();                         // 避免独占 CPU
    }

    sweepDone_.store(true, memory_order_release);              // 全部完成
}
```

`SweepUnsweptList` 逐个 TakeHead + CAS 清扫 + 分类归队（逻辑与 `GetRegion` 的 Lazy Sweep 路径和 `SweepAndClassify` 完全一致）:

```cpp
// CentralRegionPool.cpp L314-348
void SweepUnsweptList(SizeClassBucket& bucket, MSRegionList& list, uint32_t sg) {
    for (;;) {
        MSRegionInfo* region;
        { lock_guard lg(bucket.lock); region = list.TakeHead(); }
        if (!region) break;

        if (region->CasSweepGen(sg - 2, sg - 1)) {
            region->SweepRegion();
            region->SetSweepGen(sg);
            sweepStats_.bgSweptRegions++;         // Background 路径统计

            if (region->IsCompletelyFree()) {
                sweepStats_.releasedRegions++;
                ReleaseRegionInternal(region);
            } else if (region->HasFreeSpace()) {
                lock_guard lg(bucket.lock);
                bucket.partialSwept.Prepend(region);
            } else {
                lock_guard lg(bucket.lock);
                bucket.fullSwept.Prepend(region);
            }
        }
        // CAS 失败: Lazy Sweep 或 ForceFinish 已经处理, 跳过
    }
}
```

#### 三路 Sweep 与 GC 生命周期完整对照

| 路径 | 触发时机 | 入口函数链 | 统计字段 | 退出条件/完成状态 |
|------|---------|-----------|---------|-----------------|
| **Lazy Sweep** | Mutator 分配时 `AllocBufferMS::Refill()` | `GetRegion(cls)` → Priority 2/3 内嵌 CAS + SweepRegion | `lazySweptRegions` | 获取到有空闲的 Region（或 unswept 耗尽转 GetFreshRegion） |
| **Background Sweep** | `ConcurrentSweep()` 后 (设计: `NotifyToSweep()`) | `NotifyToSweep()` → gc-helper `Run()` → `BackgroundSweepLoop()` → `BackgroundSweep()` → `SweepUnsweptList()` | `bgSweptRegions` | 36 个 SizeClass 的 unswept 链表全部 drain，`sweepDone_ = true` |
| **ForceFinishSweep** | 下轮 GC `MarkSetup()` 第一步 | `MarkSetup()` → `ForceFinishSweep()` → `SweepAndClassify()` | `forceSweptRegions` | 36 个 SizeClass 的 unswept 链表全部 drain，`sweepDone_ = true` |

#### ConcurrentSweep 完整流程 (GC 侧入口)

```cpp
// MarkSweepCollector.cpp L621-643  —— 按实际代码
void MarkSweepCollector::ConcurrentSweep() {
    CentralRegionPool* centralPool = space.GetCentralPool();
    if (centralPool != nullptr) {
        centralPool->PrepareForSweep();       // O(1) 代际翻转
        // 设计文档: 此处应调用 fp.NotifyToSweep() 触发后台 Sweep
        // 实际代码: 未调用 —— 后台 Sweep 不会启动
    }
    // GC 结束, Mutator 恢复
    // 清扫将由 Lazy Sweep (分配路径) 和 ForceFinishSweep (下轮 GC) 完成
}
```

#### 小结

**做了什么**: 将后台 Sweep 集成到 FinalizerProcessor（gc-helper 线程）。新增 `NotifyToSweep()` 唤醒接口、`BackgroundSweepLoop()` 调度入口、`BackgroundSweep()` 遍历 36 个 SizeClass 的 unswept 链表逐个清扫。gc-helper 主循环按严格优先级：Finalizer → Sweep → Feed → Reclaim。

**效果**:
- 复用 gc-helper 线程，无额外线程开销。后台 Sweep 与 Finalizer 处理共享同一线程，排序约束天然满足
- `BackgroundSweep()` 每 6 个 SizeClass yield 一次，避免独占 CPU 导致 Mutator 饥饿
- sweepGen CAS 协议保证 Background Sweep 与 Lazy Sweep / ForceFinishSweep 并发安全——CAS 失败即跳过，无重复清扫
- 清扫统计分三路独立计数（`lazySweptRegions` / `bgSweptRegions` / `forceSweptRegions`），便于 DFX 诊断

**改进点**:
1. **接线遗漏 (Critical)**: `NotifyToSweep()` 已实现但 `ConcurrentSweep()` 中未调用。后台 Sweep 实际不会触发，所有清扫压力集中在 Lazy Sweep（增加分配延迟）和 ForceFinishSweep（增加下轮 GC 停顿）。修复方法：在 `ConcurrentSweep()` 的 `PrepareForSweep()` 之后添加 `fp.NotifyToSweep()` 调用
2. **yield 粒度**: 每 6 个 SizeClass yield 一次（共 36 个 SC，yield 6 次）。但每个 `SweepUnsweptList` 可能遍历大量 Region，单次 yield 间隔可能很长。建议在 `SweepUnsweptList` 内部也按 Region 数量定期 yield
3. **`shouldBackgroundSweep_` 内存序**: `NotifyToSweep` 用 `release`，`Run` 循环检查用 `relaxed`。单生产者单消费者场景下安全，但如果后续多线程检查此标志需加强为 `acquire`
4. **`BackgroundSweepLoop` 的 #ifdef 位置**: `shouldBackgroundSweep_.store(false)` 在 `#endif` 之后，即非 MS 模式也会清除标志。功能正确但语义不清，建议移入 `#ifdef` 块内