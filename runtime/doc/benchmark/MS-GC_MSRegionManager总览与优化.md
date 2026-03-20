# MSRegionManager 总览与优化分析

> 基于当前源码 `src/Heap/Allocator/MSRegionManager.{h,cpp}`、`CentralRegionPool.cpp`、`RegionSpace.cpp` 复核。
> 日期：2026-03-19

---

## 1. 先说结论

这份文档对应的主题是 MSRegionManager，但按当前代码看，MSRegionManager 已经不再是“固定堆上的一个简单 region 管理器”，而是：

1. 基于大虚拟地址预留的 region 元数据 + 堆空间管理器。
2. 负责 dirty/released 双树管理、按 unit 分配、segment 增长和 tail shrink。
3. 与 `CentralRegionPool`、`AllocBufferMS`、large-region 路径共同组成 MS-GC 的实际分配系统。

因此，讨论性能时不能只盯着 `MSRegionManager::TakeRegion()` 一处。小中对象热点路径更多在 `AllocBufferMS + CentralRegionPool`，MSRegionManager 主要负责：

- fresh region 获取
- 大对象对齐分配
- 回收后空闲 unit 的树管理
- committed heap 的增长与尾部收缩

---

## 2. 当前实现的准确模型

### 2.1 内存布局

当前实现不是“按 heapSize 精确初始化的一次性固定堆”，而是“虚拟大预留 + 初始 committed segment”。

布局如下：

```text
[ MSRegionInfo[maxVirtualRegionCount] | Heap[maxVirtualHeapSize] ]
^mmapBase                              ^heapStart                ^maxHeapEnd
                                       ^inactiveZone            ^committedEnd
```

关键点：

- `MSRegionInfo[]` 位于 mmap 前端。
- `heapStart_` 指向真实堆起点。
- `committedEnd_` 是当前可用堆上界，会随 `GrowOneSegment()` 增长。
- `maxHeapEnd_` 是最大虚拟预留上界。
- `inactiveZone_` 是 bump 水位线，初始等于 `heapStart_`。

### 2.2 Initialize 的真实语义

当前真实签名是：

```cpp
void MSRegionManager::Initialize(size_t heapSize,
                                 uintptr_t mmapBase,
                                 size_t maxVirtualHeapSize)
```

它做的是两层初始化：

1. 用 `maxVirtualHeapSize` 计算最大 region/unit 数量。
2. 仅按 `heapSize` 初始化初始 committed segment。

因此，下面这些旧表述都不准确：

- “`heapEnd_ = heapStart_ + heapAligned`”
- “初始化时把所有 `MSRegionInfo` 都置成 FREE”
- “maxRegionCount_ / maxUnitCount_ 只对应初始 heapSize”

更准确的描述是：

- 只对初始 committed 范围内的 `MSRegionInfo` 做显式初始化。
- `dirtyTree_` 和 `releasedTree_` 按最大虚拟 unit 数初始化，以支持后续增长后的索引。
- `reservedEnd` / `heapEnd` 在 MS 模式下应理解为当前 committed end，而不是整个虚拟保留区末尾。

### 2.3 关键成员与职责

| 成员 | 含义 | 备注 |
|------|------|------|
| `array_` | mmap 前端的 `MSRegionInfo[]` | 非镜像结构 |
| `heapStart_` | 堆起点 | 不变 |
| `committedEnd_` | 当前 committed heap 上界 | 可增长 |
| `maxHeapEnd_` | 最大虚拟上界 | 不变 |
| `inactiveZone_` | bump 水位线 | 单调增加，tail shrink 时可回退 |
| `dirtyTree_` | 已回收但仍保留物理页的空闲 unit | 取出后需要清零 |
| `releasedTree_` | 已释放物理页的空闲 unit | 取出后通常无需 `ClearUnits` |
| `segmentSize_` | 增长粒度 | `max(heapSize, 64MB)` 后再按 region 对齐 |

### 2.4 地址到 MSRegionInfo 查找

当前 O(1) 查找结论成立：

```cpp
size_t regionIdx = (addr - heapStart_) / MS_REGION_SIZE;
return &array_[regionIdx];
```

但要补一句前提：调用方必须保证地址属于 MS heap 有效范围，否则这里没有额外边界防御。

---

## 3. 分配路径梳理

### 3.1 小中对象路径不是直接 `TakeRegion()` 热路径

当前小中对象分配路径应写成：

```text
Mutator
  -> AllocBufferMS::Allocate()
    -> AllocBufferMS::SlowAlloc()
      -> CentralRegionPool::GetRegion(cls)
         P1 partialSwept
         P2 partialUnswept (现场 sweep)
         P3 fullUnswept (现场 sweep)
         P4 GetFreshRegion(cls)
              -> MSRegionManager::TakeRegion()
```

因此：

- `MSRegionManager::TakeRegion()` 只在 CentralPool 没有可复用 region 时才进入。
- 小中对象的常态热点更多在 `CentralRegionPool` 四链表与 lazy sweep，而不是每次都进 MSRegionManager。

### 3.2 `TakeRegion()` 的当前行为

`TakeRegion()` 仍有两个行为值得明确写出：

1. 先做 heuristic GC trigger 检查。
2. 再调用 `TakeUnitsAligned(unitsPerMSRegion_, unitsPerMSRegion_, idx)` 取对齐 region。

这里的 heuristic trigger 是同步的：

```cpp
collector.RequestGC(GC_REASON_HEU, true);
```

也就是说，fresh region 路径会直接阻塞等待这次启发式 GC 完成。

### 3.3 `TakeUnitsAligned` 是当前唯一公开分配实现

旧文档里写 “TakeRegion -> TakeUnitsAligned -> TakeUnitsInternal 或 inactiveZone bump”，这个说法不再适用。

当前更准确的说法是：

1. 对外公开路径 `TakeRegion()` / `TakeRegionForLarge()` 都直接调用 `TakeUnitsAligned()`。
2. `TakeUnitsInternal()` 已从当前代码中删除，因为没有任何调用点。
3. `TakeUnitsAligned()` 现在自己承担完整的 dirty/released/inactiveZone 三级分配逻辑，并在 tree 路径中做 over-allocation + 对齐切分。

因此，对当前代码读者而言，不应再把 `TakeUnitsInternal()` 理解为现存的内部公共子步骤；当前真正生效的就是 `TakeUnitsAligned()`。

### 3.4 `TakeUnitsAligned()` 的当前语义

tree 路径：

- 优先 dirtyTree
- 再尝试 releasedTree
- 都失败后进入 inactiveZone bump

tree 路径的特点：

- 只有在从 dirtyTree / releasedTree 取块时，才会为满足对齐先多取 `num + alignment - 1`
- 对齐后只保留中间那段真正返回给调用方的区间，未使用的前后碎片通过 `ReclaimUnitsInternal()` 重新插回 dirtyTree
- dirtyTree 取出的块会 `ClearUnits()`
- releasedTree 取出的块不会 `ClearUnits()`

bump 路径的特点：

- 先把 bump 起点对齐到 `alignment * unitSize_`
- 空间不足会尝试 `GrowOneSegment()`
- 成功后用 CAS 推进 `inactiveZone_`
- bump 路径不会先多取一大段再“放回去”，而是直接占用对齐后的目标区间

### 3.5 关于 bump 对齐间隙

文档里指出 “inactiveZone 对齐会产生永久浪费”，这个方向基本成立，但表述需要更谨慎。

当前代码里，对齐前旧水位线和对齐后起点之间的间隙没有被显式回收到 dirty/released tree，因此这部分确实不会被再次走 tree 路径复用。更准确的说法应是：

- 这部分会形成 `inactiveZone_` 前方的未建模小空洞。
- 是否“永久浪费”取决于后续 shrink / heap 尾部回退是否能覆盖到它；对非尾部间隙来说，确实等价于泄漏式浪费。

### 3.6 大对象路径

当前大对象路径的关键点：

1. `TakeRegionForLarge()` 同样先做 heuristic trigger 检查。
2. `unitCount` 会按 `unitsPerMSRegion_` 向上对齐。
3. 起始地址也必须按 `unitsPerMSRegion_` 对齐。
4. 若分配失败，不在这里先同步 `ForceFinishSweep()`，而是由上层 `RegionSpace::TryAllocateOnceMS()` 触发 `RequestGC(GC_REASON_OOM, false)` 并等待 GC 完成后重试一次。

这一点和旧文档/旧分析里“大对象分配失败先强制扫尾”的说法不同，必须区分开。

---

## 4. 回收路径梳理

### 4.1 小中对象 region 回收

当前小中对象 region 只有在“完全空闲”时才会离开 `CentralRegionPool`：

```text
SweepRegion
  -> IsCompletelyFree()
    -> ReleaseRegionInternal(region)
         -> region->ReleaseBitmaps()
         -> region->Reset()
         -> totalMSRegionBytes_ -= MS_REGION_SIZE
         -> msRegionManager_->ReclaimRegion(region)
              -> ReclaimUnitsInternal(...)
              -> dirtyTree_.MergeInsert(...)
```

结论：

- 小中对象完全空闲 region 当前回到 `dirtyTree_`
- 不是回到 `releasedTree_`
- 部分占用 region 继续留在 central pool 四链表里

### 4.2 大对象回收

旧文档里最明显的一处错误是：把 `ReclaimRegionLarge()` 写成回到 dirtyTree。

当前真实实现是：

```cpp
ReclaimRegionLarge(...) -> ReclaimUnitsReleased(...)
```

也就是：

- large region 会先释放物理页
- 然后进入 `releasedTree_`
- 不是进入 dirtyTree

这点对后续优化判断非常关键，因为 releasedTree 本来就不是“完全没用上”。它至少已经承接了：

1. large region 回收
2. `ReleaseGarbageRegions()` 从 dirtyTree 迁移出的节点

### 4.3 `releasedTree` 的真实状态

因此，旧文档里“releasedTree 从未被写入，可激活”的表述已经不成立。

更准确的说法是：

- 在“小对象主导”场景下，releasedTree 可能利用率不高。
- 但在大对象回收和 dirty->released scavenging 场景下，它已经是活跃路径。

---

## 5. 并发与锁行为

### 5.1 锁模型

当前这个归纳基本成立：

- 分配侧对 tree 走 `try_lock`
- 回收侧对 tree 走阻塞锁

但要补一条更关键的行为：

- `CentralRegionPool` 的 P1 热路径其实是 `partialSwept` 的 lock-free pop。
- 真正进入 `dirtyTreeMutex_` / `releasedTreeMutex_` 的是 fresh region 或大对象分配路径，不是所有小对象分配都会碰到树锁。

如果不写这条，容易高估 MSRegionManager 锁竞争在整体分配中的占比。

### 5.2 `ScopedEnterSaferegion` 的描述

这里也需要收紧一点。更准确的描述应是：

- 在 tree 获取失败后的循环里进入 saferegion，主要是为了避免长时间占用运行态并允许 GC 协调。
- 在回收路径加锁前进入 saferegion，是为了让线程在这段可能阻塞的路径上可被 GC 协调。

“锁会被持有直到 STW 结束” 这种说法过于绝对，只有在特定时序下才会发生，不应写成普遍结论。

---

## 6. 当前代码下真正值得关注的性能点

下面把优化点按把握程度拆开。

### 6.1 高把握问题

#### O1: GC 阈值指标与触发比较指标不一致

这不是 MSRegionManager 独有问题，但它直接影响 `TakeRegion()` / `TakeRegionForLarge()` 的行为。

- `MarkSweepCollector::UpdateGCStats()` 使用 `trueLiveBytes`
- `MSRegionManager::TakeRegion*()` 使用 `AllocatedBytes() >= threshold`
- 而 `AllocatedBytes()` 在 MS 模式下实际来自 `totalMSRegionBytes_`

这意味着：

- 阈值是按真实 live 计算的
- 触发是按已登记 region 总量判断的

这组指标语义不一致，优先级应高于单纯调 tree 或 memset。

#### O2: 指标命名与职责不清

当前 `AllocatedBytes()` 这个名字在 MS 模式下很容易让读者误解成：

- committed heap bytes
- 真实 live bytes
- 或当前物理占用

实际它更接近“central pool + large region 已登记 MS region 总量”。

建议优先补齐并区分：

- live bytes
- registered region bytes
- committed bytes
- released/dirty cached bytes

#### O3: sweep 诊断信号仍偏弱

当前有 `ForceFinishSweep CAS FAIL` 等日志，但仍不足以让人快速判断：

- 是哪个 size class 出问题
- 问题来自 lazy / bg / force 哪个阶段
- large reclaim 与 small sweep 哪边是主因

这类诊断完善的价值通常高于继续做微观优化。

### 6.2 中把握优化点

#### O4: 减少 fresh-region 路径上的同步 heuristic GC 成本

当前 fresh region 路径上的 heuristic trigger 是同步的：

```cpp
RequestGC(GC_REASON_HEU, true)
```

这意味着一旦触发，调用方直接阻塞等 GC。可以考虑：

- 降低检查频率
- 改成增量门槛触发，而不是每次 `TakeRegion*()` 都判断
- 让某些场景先异步请求，再由更高层统一收敛

这是否值得做，要结合实际 profile 看 fresh path 占比。

#### O5: 降低 dirtyTree 锁竞争导致的过早 bump

当前分配侧 `try_lock` 失败后会继续往下走，最终可能 bump 或 grow segment。这个问题方向成立。

但推荐先做轻量实验，而不是直接大改数据结构：

- `try_lock` 失败时短暂 `yield` 后重试一次
- 对高频 class 做小型 fresh-region 局部缓存
- 记录 `try_lock fail -> bump fallback` 计数，先确认问题规模

比起直接引入无锁 tree 或读写锁，这条路径的性价比更高。

#### O6: `ClearUnits` 成本值得量化，但不要先下结论

dirtyTree 取出块会 `memset`，这当然是成本。但是否已经成为主瓶颈，当前文档里写得太肯定。

原因是：

- 小对象常态热路径优先走 `partialSwept`
- fresh region 获取不是每次对象分配都发生
- macOS 上 released 路径本身也要 `MemorySet + mmap(MAP_FIXED)`

因此更合理的建议是：

- 先在 benchmark 中记录 dirtyTree 命中率、releasedTree 命中率、fresh region 次数
- 再决定优化 `ClearUnits` 是否真有收益

### 6.3 候选实验，不建议直接写成默认优化

#### O7: 让小对象完全空闲 region 直接走 releasedTree

这是旧文档里最积极的一条建议，但当前不应写成默认推荐。

原因：

1. 方向上是合理的，确实可能减少后续 `ClearUnits()`。
2. 但在 macOS 上 `ReleaseUnits()` 不是廉价操作，而是：
   - `MemorySet`
   - `mmap(MAP_FIXED)`
3. 对 32KB 小 region 来说，回收-再申请频繁时，这条路径未必比保留 dirty cache 更划算。

所以更稳妥的写法应是：

- 作为可实验候选
- 只建议在高 RSS 压力或小 region 低复用场景下评估
- 最好设计成“按缓存压力/回收节奏自适应迁移 dirty->released”，而不是把 `ReclaimRegion()` 全量改掉

#### O8: 引入碎片整理/选择性 compaction

方向没问题，但这是系统级改造，已经超出 MSRegionManager 层面的局部优化。

可以保留为远期方向，但不应放在当前文档的前排建议里。

### 6.4 结构性重构候选：Region-Run Free Manager

如果目标不是继续在当前 unit-tree 上打补丁，而是从根上消除“对齐切分 + 碎片回插”这类复杂度，更稳妥的做法不是直接把现有 `CartesianTree` 粒度硬改掉，而是新增一层 region-run free manager，让 `MSRegionManager` 对外只按 region 计数管理，内部在需要 `ClearUnits()` / `ReleaseUnits()` 时再换算成 unit 范围。

#### 方案目标

1. 让 free manager 只管理“连续 region run”，不再管理任意 unit 区间。
2. 让 `TakeRegion()` / `TakeRegionForLarge()` 天然按 region 边界分配，不再需要 over-allocation + 对齐切分。
3. 保留底层清零和物理页释放的 unit/page 粒度实现，但把它们下沉为 region-run 的派生动作。

#### 拆分后的职责

建议把当前 `MSRegionManager` 中的自由空间管理职责拆成两层：

1. `RegionRunFreeManager`
  - 管理 free dirty region runs
  - 管理 free released region runs
  - 接口只接受 `regionIdx` 和 `regionCount`
  - 负责 lowest-address-first / merge / remove-root / tail reclaim

2. `MSRegionManager`
  - 保留 heapStart / committedEnd / inactiveZone / GrowOneSegment / ShrinkHeap
  - 对外仍暴露 `TakeRegion()`、`TakeRegionForLarge()`、`ReclaimRegion()`、`ReclaimRegionLarge()`
  - 在调用 free manager 后，把 region 范围转换成 unit 范围，再做 `ClearUnits()` 或 `ReleaseUnits()`

#### 建议接口

```cpp
class RegionRunFreeManager {
public:
   using RegionIndex = uint32_t;
   using RegionCount = uint32_t;

   bool TakeDirtyLowestAddr(RegionCount num, RegionIndex& idx);
   bool TakeReleasedLowestAddr(RegionCount num, RegionIndex& idx);

   bool ReclaimDirty(RegionIndex idx, RegionCount num);
   bool ReclaimReleased(RegionIndex idx, RegionCount num);

   bool RemoveLargestDirtyRun(RegionIndex& idx, RegionCount& num);
   bool TakeTailReleasedRunIfEndEquals(RegionIndex endIdx,
                           RegionIndex& idx,
                           RegionCount& num);

   uint32_t GetDirtyRegionCount() const;
   uint32_t GetReleasedRegionCount() const;
};
```

这里的关键不是具体名字，而是语义收缩：以后 free manager 不再处理 unit 索引，也不再承担对齐逻辑。

#### `MSRegionManager` 侧的改造方式

1. `TakeRegion()`
  - 直接申请 1 个 dirty/released/fresh region
  - 不再调用 `TakeUnitsAligned()`

2. `TakeRegionForLarge(size_t unitCount, ...)`
  - 先把 `unitCount` 向上换算成 `regionCount`
  - 再直接申请连续 `regionCount`
  - 返回时再由 `regionIdx -> address` 计算起始地址

3. `ReclaimRegion()`
  - 直接按 `regionIdx, 1` 回收到 dirty free manager

4. `ReclaimRegionLarge()`
  - 先把 `regionBytes` 换算成 `regionCount`
  - 释放物理页
  - 再按 `regionIdx, regionCount` 回收到 released free manager

5. `ReleaseGarbageRegions()`
  - 以 region run 为单位从 dirty free manager 抽取节点
  - 对整个 run 做 `ReleaseUnits()`
  - 再插入 released free manager

6. `ShrinkHeap()`
  - 以 region run 为单位找 heap 尾部 released run
  - 仅在 run 末端与 `inactiveZone_` 对齐时回退 inactiveZone
  - committed shrink 仍按 segment 粒度执行

#### 为什么这是“更稳妥”的改法

相比直接把 `CartesianTree` 的 unit 粒度硬改成 region 粒度，这个方案更稳妥，原因有三个：

1. 改动边界清晰
  - 现有 `MSRegionManager` 外部接口基本可以保持不变
  - `CentralRegionPool` 和 `RegionSpace` 无需感知内部粒度变化

2. unit/page 级副作用保留在原处
  - `ClearUnits()` 和 `ReleaseUnits()` 仍然是底层页操作
  - 只是它们的输入由 `regionCount * unitsPerMSRegion_` 推导得出

3. 便于分阶段落地
  - 可以先引入新 manager，但保留旧的 unit helpers 作为过渡
  - 等 `TakeRegion*()` / `ReclaimRegion*()` 全切换后，再清理旧逻辑

#### 预期收益

1. 消除 `TakeUnitsAligned()` 的 over-allocation + 对齐切分逻辑。
2. 消除“从 releasedTree 拿块，但切下来的头尾碎片又回到 dirtyTree”这种语义混杂。
3. 让 `dirty/released` 缓存统计天然按 region 计数，和 `MSRegionInfo` 模型一致。
4. 让文档、代码和调试日志都更直观，因为 free run 的粒度与外部对象模型一致。

#### 需要接受的代价

1. 粒度变粗
  - free manager 不再表达非 region 对齐碎片
  - 这等于主动放弃 unit-level 空闲块复用能力

2. `ShrinkHeap()` 与 scavenging 逻辑要重写一层 region-run 版本
  - 当前实现里 tail reclaim 直接基于 unit end 判断
  - 改造后要统一换成 region end 计算

3. `CartesianTree` 可能仍要保留，但换成 region index/count 语义
  - 也可以新写一个更简单的 region-run treap/list
  - 这一点要看你是想复用现有代码，还是顺便清掉 unit 时代的历史包袱

#### 推荐的分阶段迁移步骤

**Stage 0: 先补适配层，不改外部行为**

1. 新增 `RegionRunFreeManager`，内部先复用 `CartesianTree`，但统一把 index/count 解释成 region 维度。
2. 在 `MSRegionManager` 内增加换算 helper：
  - `RegionIdxToUnitIdx()`
  - `RegionCountToUnitCount()`
  - `AddrToRegionIdx()`

**Stage 1: 切换回收路径**

1. 先把 `ReclaimRegion()` 改成回收 1 个 dirty region run。
2. 再把 `ReclaimRegionLarge()` 改成回收 released region run。
3. 这个阶段先不动分配路径，先验证 free-list 统计和 merge 语义正确。

**Stage 2: 切换分配路径**

1. 用 `TakeDirtyLowestAddr(1)` / `TakeReleasedLowestAddr(1)` 重写 `TakeRegion()`。
2. 用 `TakeDirtyLowestAddr(regionCount)` / `TakeReleasedLowestAddr(regionCount)` 重写 `TakeRegionForLarge()`。
3. 删除 `TakeUnitsAligned()` 中与 tree 对齐切分相关的逻辑，仅保留 bump/grow fallback，或者直接把它替换成 region-run 版本。

**Stage 3: 切换 scavenging / shrink**

1. `ReleaseGarbageRegions()` 改为按 region run 搬运 dirty -> released。
2. `ShrinkHeap()` 改为按 region run 判断 heap 尾部连续 released 空间。
3. 验证 committed shrink 与 `inactiveZone_` 回退在 region 粒度下仍正确。

**Stage 4: 清理遗留 unit 路径**

1. 删除只为 tree 对齐切分服务的旧 helper。
2. 把统计项统一改成 region-run 口径，再按需要换算成 bytes 输出。

#### 结论

这条方案本质上是把当前 MSRegionManager 从“unit allocator with region alignment”重构成“region-run allocator with page-level side effects”。它不是小修，但如果目标是让实现和对象模型一致、减少对齐切分复杂度、让 `dirty/released` 两类缓存语义更清晰，这是一条比继续在 unit tree 上叠补丁更干净的中期方案。
#### ✅ 实施状态：已完成（2025-03-19）

**实施策略**：Stage 0-3 合并为一次原子性改造（所有路径紧密耦合，分阶段无法独立验证），Stage 4 无需执行（没有遗留 unit 路径需要清理）。

**未引入 `RegionRunFreeManager` 新类**：评估后选择就地改造 `MSRegionManager`，直接将 `CartesianTree` 的索引语义从 unit 切换为 region。理由：
- `CartesianTree` 的 `Index`/`Count` 是 `uint32_t` 抽象类型，天然支持语义切换
- 引入新类会增加间接层但不增加功能
- 外部接口（`TakeRegion`、`ReclaimRegion` 等）完全不变

**改动文件**：
1. `MSRegionManager.h`：
   - 新增 `RegionIndex`/`RegionCount` 类型别名
   - 新增 4 个 inline 转换函数：`RegionIdxToAddr`、`AddrToRegionIdx`、`RegionToUnitIdx`、`RegionCountToUnitCount`
   - `TakeUnitsAligned` → `TakeRegions`，`ReclaimUnitsInternal` → `ReclaimRegionsDirty`，`ReclaimUnitsReleased` → `ReclaimRegionsReleased`
   - `GetFreeUnitCount()` 内部乘以 `unitsPerMSRegion_` 保持外部兼容

2. `MSRegionManager.cpp`：
   - **Initialize**: tree Init 改为 `maxVirtualRegionCount_`
   - **TakeRegions**（原 `TakeUnitsAligned`）：消除 over-allocation（`num + alignment - 1`）、对齐切分（`goto align_and_return`）、碎片回插。三条路径（dirty/released/bump）均直接按 region 数量分配
   - **ReclaimRegionsDirty**（原 `ReclaimUnitsInternal`）：接受 RegionIndex/RegionCount
   - **TakeRegion**: 直接 `TakeRegions(1, regionIdx)`
   - **TakeRegionForLarge**: unitCount → regionCount 换算后调用 `TakeRegions`
   - **ReclaimRegion**: 直接 `ReclaimRegionsDirty(regionIdx, 1)`
   - **ReclaimRegionLarge**: `AddrToRegionIdx` + region 计数换算
   - **ReleaseGarbageRegions**: 按 `MS_REGION_SIZE` 计算 dirty bytes
   - **ShrinkHeap**: Phase 1 使用 `AddrToRegionIdx`/`RegionIdxToAddr` 转换

**关键设计洞察**：`heapStart_` 始终 MS_REGION_SIZE 对齐（因为 metadata = regionCount × 64 = heapSize/512，对 heapSize ≥ 16MB 必定是 MS_REGION_SIZE 的整数倍），故 `inactiveZone_` 始终 region 对齐，bump 路径无需对齐逻辑。

**验证结果**：编译通过（dylib 1289000 bytes），`run.sh ms-all` hello 测试通过。
---

## 7. 建议的优化优先级

### P0: 先解决“指标一致性”和“诊断可见性”

1. 统一 threshold 计算指标与 trigger 比较指标。
2. 明确区分 live / registered / committed / cached bytes。
3. 补 sweep 与 OOM 前后的关键统计。

### P1: 再优化 fresh-region 与 tree 路径

1. 统计 `try_lock` 失败后的 fallback 比例。
2. 评估一次重试或小缓存是否能明显降低 bump/grow。
3. 统计 dirtyTree / releasedTree 命中率与 `ClearUnits` 占比。

### P2: 最后再决定是否动 released 策略

1. 先做实验验证小 region 直接 release 在 macOS 上是否真的划算。
2. 如果收益不稳定，优先考虑批量 scavenging，而不是默认把小 region 回收路径改为 released。

### P3: 如果决定做结构性重构，再推进 region-run free manager

1. 先用适配层引入 region-run free manager，不要一开始就硬改所有 unit 路径。
2. 先切回收，再切分配，最后切 scavenging/shrink。
3. 只有在前面 P0-P2 的指标补齐后，才适合判断这类结构性改造是否值得进入主线。

---

## 8. 修正后的结论

1. MSRegionManager 当前已经是“虚拟大预留 + committed segment 增长 + tail shrink”的管理器，不是固定堆模型。
2. 小中对象分配的常态热点在 `CentralRegionPool`，MSRegionManager 主要承担 fresh region 与大对象对齐分配。
3. 小 region 完全释放后回到 dirtyTree；large region 回收后回到 releasedTree。旧文档把 large reclaim 写到 dirtyTree 是错误的。
4. releasedTree 并非未使用，只是在小对象主导场景下可能不是主路径。
5. 当前最该优先优化的，不是直接改 `ReclaimRegion()` 走 releasedTree，而是先统一 GC 压力指标、补强 sweep 诊断，并量化 fresh/tree/memset 这几类成本的真实占比。
| **O6-g** | 分配侧 try_lock 重试 | 中 | 低 | P3: 减少不必要的 bump |
| **O6-f** | 批量回收 | 中 | 中 | P3: 减少锁频率 |
| **O6-h** | Fresh region 预取缓存 | 中 | 中 | P4: 减少树操作频率 |
| **O6-a** | 延迟清零 | 中 | 中 | P1: 分配路径加速 |
| **O6-j** | 碎片整理 | 高 | 高 | P5: 减少碎片，提高堆利用率 |

> **建议优先做 O6-d**：工程量最小（修改 `ReclaimRegion` 调用已有的 `ReleaseUnits` + `ReclaimUnitsReleased`），同时解决 P1（免 memset）和 P2（小对象也用 releasedTree），预期性能提升显著。注意 macOS 的 `mmap(MAP_FIXED)` 开销比 Linux 的 `madvise` 高，可考虑仅对大 region（或累积超过一定数量后批量）执行 release。
