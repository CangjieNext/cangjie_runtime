# LOS Compaction 设计文档

> 创建日期: 2026-02-21
> 状态: 设计阶段

---

## 1. 问题回顾

`CompactLOSIfNeeded` 当前被禁用，原因是 WCollector 为并发收集器（`ScopedStopTheWorld` 在 `CopyCollector::RunGarbageCollection()` 中被注释掉），`CompactLOS()` 的以下操作在 mutator 并发运行时不安全：

1. `memmove_s` 移动存活对象 → mutator 并发读写被移动对象 → SIGSEGV / 数据撕裂
2. `UpdateMigratedReferences` 扫描全堆更新引用 → mutator 读取旧地址 → 悬空引用
3. `RebuildFreeRegionManager` + region 列表重建 → 与并发 `AllocLarge` 竞态

---

## 2. 候选方案对比

### 方案 A：复用 WCollector Forwarding 机制（**选定方案**）

利用 WCollector 已有的并发对象移动框架：`StateWord`（NORMAL → FORWARDING → FORWARDED）+ tagged pointer + `ForwardBarrier` 读屏障。LOS 压缩期间，将待移动的 LOS region 标记为 ghost-from-region，复用 `Preforward` + `ForwardFromSpace` 阶段完成并发拷贝和引用更新。

**核心改进**：结合用户建议，采用 **"冻结 LOS 写入 + GC 线程压缩 + 解冻"** 策略，避免在压缩期间 LOS 接收新分配请求，消除分配器竞态。

| 维度 | 评估 |
|------|------|
| STW 需求 | 无 |
| 并发安全 | 高（复用已验证的 forward 机制） |
| 实现复杂度 | 中 |
| 内存开销 | 压缩期间新旧副本共存，峰值 ×2 |
| 适用性 | 正式方案 |

### 方案 B：mprotect + SIGSEGV handler

移动对象前用 `mprotect` 把旧地址设为不可访问，mutator 访问时触发 SIGSEGV，由信号处理器重定向。

| 维度 | 评估 |
|------|------|
| STW 需求 | 无 |
| 并发安全 | 低 |
| 实现复杂度 | 高 |
| 内存开销 | 无额外 |
| 缺陷 | 信号处理慢（内核态切换）；macOS page 16KB 与 region 不对齐；无法处理寄存器中缓存的旧地址；平台兼容性差（iOS 信号受限） |
| 结论 | **不推荐** |

### 方案 C：Copy-on-Read（Two-Phase 渐进压缩）

GC 拷贝对象到新地址 + 在旧对象头安装 forwarding pointer；mutator 读取时自发现并修正。

| 维度 | 评估 |
|------|------|
| STW 需求 | 无 |
| 并发安全 | 中 |
| 实现复杂度 | 高 |
| 内存开销 | 峰值 ×2 |
| 缺陷 | 需要改 `BaseObject` 布局存 forwarding pointer（与 `TypeInfo` 冲突）；mutator 写入旧对象需额外 write barrier 同步到新副本；本质上重造一套 forward 机制 |
| 结论 | 不如方案 A 直接复用已有设施 |

### 方案 D：Free-list 合并（不移动对象）

放弃对象移动，改为在 `losFreeMgr` 中合并相邻空闲 region。

| 维度 | 评估 |
|------|------|
| STW 需求 | 无 |
| 并发安全 | 高 |
| 实现复杂度 | 低 |
| 内存开销 | 无 |
| 缺陷 | 交错存活的碎片无法合并；无法收缩 `losInactiveZone`；只能缓解不能根治 |
| 结论 | 可作为补充，但不能替代压缩 |

---

## 3. 方案 A 详细设计

### 3.1 总体思路

```
  ┌─────────┐     ┌──────────────┐     ┌──────────────┐     ┌─────────┐
  │ 冻结 LOS │ ──→ │ 规划 + 拷贝   │ ──→ │ 引用更新 +   │ ──→ │ 解冻 LOS │
  │ (禁止写入) │     │ (GC 线程)     │     │ 树重建       │     │ (恢复写入) │
  └─────────┘     └──────────────┘     └──────────────┘     └─────────┘
       ↑                                                          │
       │    冻结期间大对象分配 fallback 到 MAIN                      │
       │    解冻后将 MAIN 中的大对象迁移回 LOS（可选, 下次 GC 自然迁移）│
       └──────────────────────────────────────────────────────────┘
```

### 3.2 关键设计决策

**冻结 LOS**：设置 `losCompactionFreeze` 原子标志为 `true`，`AllocLarge()` 检查该标志，如果已冻结则直接返回 0（触发 MAIN fallback）。冻结期间大对象在 MAIN 中分配，功能不受影响，仅分配位置临时改变。

**为什么不需要 STW**：
- 对象移动通过 WCollector 的 forwarding 机制完成：GC 线程先拷贝对象到新地址，转发 barrier 确保 mutator 总能读到正确版本
- 引用更新通过 `ForwardBarrier::ReadReference()` 的 tagged pointer 机制在 mutator 的下次读取时自动完成
- LOS 冻结保证了压缩期间不会有新的 LOS 分配干扰 `losFreeMgr` 和 region 列表

**重要澄清（覆盖范围）**：
- `ForwardBarrier::ReadReference()` 是“按访问触发”，不会主动遍历 LOS 全部对象。
- 仅靠读屏障不能保证 compactPlan 中所有对象都完成搬移。
- 因此必须由 GC 线程在 `ForwardFromSpace` 阶段主动遍历 compactPlan，对每个 LOS 对象执行一次 forward（LOS 为 1 region = 1 object，遍历成本可控）。
- 只有在“GC 主动全量 forward 完成”后，才能安全回收旧 region 并重建 `losFreeMgr`。

**并发搬移顺序约束（新增）**：
- 在并发模式下，不能把整批对象一次性安装 route 后再“随缘 forward”，否则会出现“目标区间覆盖未搬移源区间”的风险。
- 对于对象 A（`old(A) -> new(A)`），仅当 `target(A)` 与任何“未完成对象 B 的 `source(B)`”不相交时，A 才允许进入本轮安装/搬移。
- 形式化条件：
    - `safe(A) = ∀B(unresolved): [new(A), new(A)+size(A)) ∩ [old(B), old(B)+size(B)) = ∅`
- 对“向高地址滑动”场景，这等价于按依赖链推进：通常要先处理更高地址侧的一串前驱对象；pinned 对象作为段边界，段内独立应用同一规则。
- 本轮实现采用“一次性全量整理”：
    - 单轮安装全部 compact plan；
    - GC 线程在本轮强制 forward 全量 LOS 待搬移对象；
    - 单轮 finalize 完成后一次性清空 plan 与迁移表；
    - 不保留未完成计划到下一轮。

**barrier 前驱推进（新增实现）**：
- 若 mutator 访问到“尚未标记 ghost-from 的压缩对象”，read barrier 不直接返回失败；
- barrier 会调用 `EnsureCompactionRouteForObject(addr)`，按依赖链先安装其前驱条目，再安装当前条目；
- 安装完成后对象进入 ghost-from 路径，再复用既有 forwarding 流程。
- 这样可保证“中间对象被访问时，先推进会覆盖它源区间的前驱对象”，避免覆盖风险。

### 3.3 实施步骤

#### Step 1：添加冻结标志和 AllocLarge 检查

**文件**：`src/Heap/Allocator/LOSManager.h`, `src/Heap/Allocator/LOSManager.cpp`

**改动内容**：

1. 在 `LOSManager` 中添加：
   ```cpp
   // 压缩冻结标志：true 时 AllocLarge 返回 0，强制 fallback 到 MAIN
   std::atomic<bool> losCompactionFreeze{false};

   void FreezeLOS() { losCompactionFreeze.store(true, std::memory_order_release); }
   void UnfreezeLOS() { losCompactionFreeze.store(false, std::memory_order_release); }
   bool IsFrozen() const { return losCompactionFreeze.load(std::memory_order_acquire); }
   ```

2. 修改 `AllocLarge()` 入口，在现有 `isCompacting` 检查后追加：
   ```cpp
   if (losCompactionFreeze.load(std::memory_order_acquire)) {
       losAllocFailCount.fetch_add(1, std::memory_order_relaxed);
       return 0;  // → MAIN fallback
   }
   ```

**验证**：冻结后所有大对象分配走 MAIN，解冻后恢复 LOS 分配。功能正确性不受影响。

---

#### Step 2：重构 CompactLOS —— 仅规划，不移动

**文件**：`src/Heap/Allocator/LOSManager.cpp`

**改动内容**：

将当前 `CompactLOS()` 拆分为两个函数：

- `PlanCompaction()` — 计算 compactPlan（哪些对象移到哪里），建立 `migrationMap`，**不执行 memmove**
- 移动和引用更新交给 WCollector 的 forwarding 阶段

```cpp
// 返回 compactPlan（供 WCollector 使用）
bool LOSManager::PlanCompaction(std::vector<CompactEntry>& compactPlan)
{
    // Phase 1: 收集 movable vs pinned regions（与现有逻辑相同）
    // Phase 2: 计算目标地址（与现有逻辑相同）
    // Phase 3: 在新地址预分配空间（从 losFreeMgr 或 inactiveZone）
    //          这一步在冻结期间执行，无并发分配竞态
    // Phase 4: 建立 migrationMap（old → new）
    // 不执行 memmove，不重建 region 列表
    return !compactPlan.empty();
}
```

`CompactEntry` 结构：
```cpp
struct CompactEntry {
    RegionInfo* oldRegion;
    uintptr_t   oldStart;
    uintptr_t   newStart;
    size_t      regionSize;
    size_t      unitCount;
};
```

---

#### Step 3：将 LOS compactPlan 注入 Forwarding 流程

**文件**：`src/Heap/WCollector/WCollector.cpp`, `src/Heap/Allocator/RegionManager.h`

**改动内容**：

在 `PostTrace()` 中，`CompactLOSIfNeeded()` 改为：

```cpp
void WCollector::CompactLOSIfNeeded()
{
    LOSManager* losMgr = ...;
    if (!losMgr->NeedsCompaction()) return;

    // 1. 冻结 LOS（新分配走 MAIN）
    losMgr->FreezeLOS();

    // 2. 规划压缩
    std::vector<LOSManager::CompactEntry> compactPlan;
    if (!losMgr->PlanCompaction(compactPlan)) {
        losMgr->UnfreezeLOS();
        return;
    }

    // 3. 为每个待移动的 LOS region:
    //    a) 在新地址初始化 region 并拷贝对象
    //    b) 将旧 region 标记为 ghost-from-region（复用现有机制）
    //    c) 设置 routeInfo 使 GetRoute(fromObj) 返回新地址
    for (auto& entry : compactPlan) {
        // 拷贝对象到新地址
        CopyObjectToNewLocation(entry);
        // 标记旧 region 为 ghost-from（使 ForwardBarrier 能识别）
        PrepareOldRegionAsGhostFrom(entry);
    }

    // 4. forwarding 阶段会自动处理引用更新
    //    （在 Preforward + ForwardFromSpace 期间）

    // 注：解冻和树重建在 ForwardFromSpace 完成后执行
    //    见 Step 5
}
```

**关键复用**：
- `RegionInfo::PrepareForwardableRegion()` — 将 LOS old region 标记为 ghost-from
- `RegionInfo::SetRouteInfo(newAddr)` — 设置转发地址
- `ForwardBarrier::ReadReference()` — mutator 读取时自动看到 ghost-from → 触发 forward（补充更新）
- GC 线程主动遍历 compactPlan 全量 forward（保证收敛）

---

#### Step 4：LOS 大对象的 Forwarding 适配

**文件**：`src/Heap/Allocator/RegionInfo.h`, `src/Heap/Allocator/RegionManager.h`

**问题**：现有 `PrepareForwardableRegion()` 有 `CHECK(UnitRole == SMALL_SIZED_UNITS)` 限制，LOS 使用 `LARGE_SIZED_UNITS`。

**改动内容**：

1. 新增 `PrepareForwardableLargeRegion()`:
   ```cpp
   void PrepareForwardableLargeRegion(uintptr_t newAddr)
   {
       CHECK(IsLargeRegion());
       metadata.routeState = FORWARDABLE;
       // 大对象 region 只有 1 个对象，直接设置转发地址
       // preLiveBytes 始终为 0，所以 routeInfo.to1 = newAddr 即可
       metadata.routeInfo.SetRouteInfo(newAddr, GetRegionAllocatedSize());
       // 标记为 ghost-from
       SetInGhostRegion(1);
       size_t nUnit = GetUnitCount();
       UnitInfo* unit = reinterpret_cast<UnitInfo*>(this);
       UnitInfo::UnitInfoArray array = UnitInfo::UnitInfoArray(unit, nUnit);
       for (size_t i = 1; i < nUnit; i++) {
           array[i].SetInGhostRegion(1);
       }
   }
   ```

2. `GetRoute()` 对大对象自动适配：大对象 region 中只有 1 个对象（offset=0），`preLiveBytes=0`，`GetRoute(0)` 直接返回 `routeInfo.to1` = 新地址。无需修改。

3. 在 `RegionManager` 中新增方法，将 LOS ghost-from region 注册到 `ghostFromRegionList`，使 `ForwardFromSpace` 能遍历到它们：
   ```cpp
   void RegisterLOSGhostFromRegions(const std::vector<RegionInfo*>& oldRegions);
   ```

---

#### Step 5：解冻 LOS + 树重建

**文件**：`src/Heap/WCollector/WCollector.cpp`, `src/Heap/Allocator/LOSManager.cpp`

**改动内容**：

在 `DoGarbageCollection()` 的 `ForwardFromSpace()` 完成后（所有引用已更新）添加：

```cpp
// ForwardFromSpace 已完成，所有 mutator 的引用已指向新地址
if (losMgr->IsCompacting()) {
    // 1. 回收旧 region（ghost-from regions → InitFreeUnits → losFreeMgr）
    losMgr->ReclaimCompactedOldRegions(compactPlan);

    // 2. 重建 losFreeMgr（重新计算空闲区间）
    losMgr->RebuildFreeRegionManager(allLiveRegions);

    // 3. 更新 losInactiveZone（缩小到最低存活对象边界）
    losMgr->UpdateInactiveZone();

    // 4. 解冻 LOS（恢复 AllocLarge 正常分配）
    losMgr->UnfreezeLOS();
    losMgr->SetCompacting(false);
}
```

在进入 `FinishLOSCompaction()` 之前，增加一步：

```cpp
// 强制收敛：GC 主动完成 LOS compactPlan 全量 forward
for (auto& entry : compactPlan) {
    BaseObject* oldObj = reinterpret_cast<BaseObject*>(entry.oldStart + RegionSpace::HEADER_SIZE);
    (void)ForwardObject(oldObj);
}
```

说明：即使 mutator 未访问到某些 LOS 对象，上述主动遍历也会确保它们搬移完成，避免“残留旧对象导致无法重建树”。

**时序保证**：
- `ForwardFromSpace` 完成后，所有 mutator 持有的引用都已从旧地址 CAS 更新到新地址
- 旧 region 的 ghost-from bit 被 `DispelGhostFromRegion()` 清除
- 此时回收旧 region 和重建 losFreeMgr 是安全的：没有任何线程持有旧地址引用
- 最后解冻 LOS，新的 `AllocLarge` 请求可以使用压缩后的连续空间

---

#### Step 6：处理冻结期间 MAIN 中积累的大对象

**设计决策**：**不需要主动迁移回 LOS**。

理由：
- 冻结期间分配到 MAIN 的大对象，会在后续的 GC 周期中作为 MAIN 的 large region 存在
- `MigrateLargeObjectsToLOS`（清单中的第 2 个禁用功能）后续恢复后，会自然地在下一次 GC 中检测到 MAIN 中的大对象并迁移到 LOS
- 即使不迁移，这些对象也能正常工作，只是暂时占用 MAIN 空间

冻结时间很短（仅一次 GC 周期），积累的 MAIN 大对象数量有限，影响可忽略。

---

### 3.4 数据流时序

```
DoGarbageCollection() {
    TraceHeap();                    // 标记存活对象

    PostTrace() {
        HandleTraceRegions();
        CollectLargeGarbage();      // 回收 LOS 死对象

        CompactLOSIfNeeded() {      // ★ 修改后
            FreezeLOS();              // 冻结 LOS 分配
            PlanCompaction();         // 规划移动目标
            CopyObjectsToNewAddr();   // GC 线程拷贝对象
            PrepareGhostFromRegions(); // 标记旧 region 为 ghost-from
        }

        // MigrateLargeObjectsToLOS——保持禁用（第 2 个功能）
        CollectPinnedGarbage();
        RefineFromSpace();
        fwdTable.PrepareForwardTable();
    }

    Preforward();                   // mutator 开始看到 tagged pointer
                                    // ForwardBarrier 遇到 LOS ghost-from → 读新地址

    ForwardFromSpace();             // GC 线程 + mutator 并发完成所有 forward
                                    // LOS 对象的旧→新引用更新在此完成

    FinishLOSCompaction() {         // ★ 新增
        ReclaimCompactedOldRegions(); // 回收旧 region
        RebuildFreeRegionManager();   // 重建 LOS 空闲树
        UpdateInactiveZone();         // 缩小 inactive zone
        UnfreezeLOS();                // 解冻 LOS 分配
    }

    CollectSmallSpace();            // 回收小对象 from-space
    ReturnBorrowedToReserve();      // 返还 RESERVE 空间
}
```

### 3.5 并发安全分析

| 操作 | 并发线程 | 安全保证 |
|------|----------|----------|
| `FreezeLOS()` | GC 线程写，mutator 读 | `std::atomic<bool>` + acquire/release |
| `PlanCompaction()` | GC 线程独占 | LOS 已冻结，无并发 AllocLarge |
| `CopyObjectsToNewAddr()` | GC 线程独占 | 拷贝到新地址（尚无人引用），旧对象未被修改 |
| `PrepareGhostFromRegions()` | GC 线程 | 设置 ghost-from bit + routeInfo（在 PrepareForwardTable 之前） |
| `ForwardBarrier::ReadReference()` | 所有 mutator | 已有机制：tagged pointer + CAS + 自旋 |
| `ForwardFromSpace()` | GC + mutator | 已有机制：`TryLockObject` + `ForwardObjectExclusive` |
| `ReclaimCompactedOldRegions()` | GC 线程 | ForwardFromSpace 完成后，ghost-from 已清除，无人持有旧引用 |
| `RebuildFreeRegionManager()` | GC 线程 | LOS 仍冻结中，无并发分配 |
| `UnfreezeLOS()` | GC 线程写，mutator 读 | `std::atomic<bool>` |
| barrier 前驱推进 | mutator/GC 并发 | `compactionPlanMutex` 保护 plan 安装与迁移表写入，避免并发安装竞态 |

### 3.6 并发风险控制（barrier assist）

为避免 barrier 介入后引入新的并发风险，实施以下约束：

1. **互斥保护 plan 生命周期**：`pendingCompactionPlan` 的设置、快照、安装、finalize 统一受 `compactionPlanMutex` 保护。
2. **安装幂等**：同一 entry 若已是 ghost-from，重复安装直接返回成功，不重复写 route。
3. **递归环检测**：前驱递归安装使用 `visiting` 集合，检测异常环依赖并中止，避免死循环。
4. **仅压缩期启用 assist**：只有 `IsCompacting()==true` 且存在 pending plan 时才走 assist 逻辑，不影响常规 barrier 快路径。

### 3.6 LOS 大对象 Forwarding 的特殊处理

现有 forwarding 为小对象设计（多个小对象共享一个 region，通过 `preLiveBytes` 偏移计算新地址）。LOS 大对象有简化条件：

- **1 region = 1 object**：`preLiveBytes` 恒为 0
- **`GetRoute(fromObj)` 直接返回 `to1` 地址**：无需偏移计算
- **`ForwardObjectExclusive` 中的 `CopyObject`**：对大对象可能较慢（几十 KB），但只执行一次（GC 线程已预拷贝）

**优化**：GC 线程在 `CompactLOSIfNeeded` 中已完成对象拷贝，`ForwardObjectExclusive` 被调用时只需要设置 forwarded 状态即可。可以通过将 LOS ghost-from region 的 RouteState 直接设为 `ROUTED`（跳过 RouteOrCompactRegionImpl），使 `TryForwardObject` 中：
```cpp
if (fwdTable.RouteRegion(region)) {       // → ROUTED, 直接 true
    BaseObject* toVersion = ForwardObjectImpl(obj, region);  // 获取新地址
    ...
}
```
`ForwardObjectImpl` 中，`ForwardObjectExclusive` 调用 `CopyObject` 会再拷贝一次。这里有两种选择：

**选择 1（简单）**：允许 `CopyObject` 重复拷贝——大对象只有几个，开销可接受。

**选择 2（优化）**：跳过已预拷贝的对象。在 `ForwardObjectExclusive` 中判断目标地址已有有效对象（NORMAL 状态），直接设置 forwarded 而不拷贝。

**建议先用选择 1**，验证正确性后再优化。

---

### 3.7 风险与缓解

| 风险 | 影响 | 缓解措施 |
|------|------|----------|
| 冻结期间 MAIN 空间不足 | 大对象分配 OOM | 冻结时间仅一次 GC 周期（毫秒级）；MAIN 有 RESERVE 借用机制 |
| 大对象 CopyObject 缓慢 | ForwardFromSpace 阶段 mutator 自旋等待 | GC 线程已预拷贝，mutator 只需设置 forwarded 状态 |
| ghost-from region 数量较多 | 增加 ForwardFromSpace 遍历开销 | LOS 对象数量通常很少（几个到几十个），影响可忽略 |
| `PrepareForwardableRegion` 的 CHECK 限制 | 编译错误 | 新增 `PrepareForwardableLargeRegion` 绕过小对象 CHECK |

### 3.8 可靠性结论（并发一次性方案）

结论：该方案**可执行**，但其可靠性更依赖“单轮内全量收敛”是否成立：

1. **全量安装**：compact plan 必须完整安装为 ghost-from route；
2. **全量强制 forward**：本轮 GC 必须覆盖所有 LOS 待搬移对象；
3. **全量 finalize**：只在 forward 收敛后回收旧 region，并清空 plan/migration 状态；
4. **冻结窗口闭环**：finalize 完成后再解冻 LOS。

若单轮内无法完成全量收敛，或存在目标覆盖未迁移源区间的执行路径，并发下仍可能出现崩溃（SIGSEGV）或进程被 kill（137）。因此该版本是“工程试验可运行”，需通过压力回归判定稳定性。

---

## 4. 实施计划

| 步骤 | 描述 | 文件 | 预计改动量 |
|------|------|------|-----------|
| **Step 1** | 添加冻结标志 + AllocLarge 检查 | LOSManager.h/cpp | ~10 行 |
| **Step 2** | 拆分 CompactLOS → PlanCompaction | LOSManager.cpp | ~80 行重构 |
| **Step 3** | WCollector 集成：冻结 → 规划 → 拷贝 → ghost-from | WCollector.cpp | ~40 行 |
| **Step 4** | 大对象 forwarding 适配 | RegionInfo.h, RegionManager.h | ~30 行 |
| **Step 5** | 解冻 + 树重建 + 旧 region 回收 | WCollector.cpp, LOSManager.cpp | ~30 行 |
| **Step 6** | 测试验证 | — | 构建 + 60x 压力测试 |

总计改动约 **190 行**，核心新增逻辑 ~120 行，其余为重构现有 CompactLOS。

---

## 5. 测试计划

1. **冻结验证**：`cjHeapSize=64mb` + 高大对象分配压力，观察冻结期间大对象 fallback 到 MAIN
2. **压缩正确性**：DLOG 对比压缩前后 region 地址和对象内容
3. **引用完整性**：压缩后遍历所有堆对象，验证引用指向有效对象
4. **压力测试**：release 模式 60x 循环，默认 6:3:1 配比
5. **碎片化场景**：交替分配/释放不同大小的大对象，触发压缩，验证空间回收

---

## 6. 当前代码进度（2026-02-21）

- 已落地：`LOS` 冻结状态与 `AllocLarge()` 冻结检查（冻结期间 fallback 到 MAIN）
- 已落地：`LOSManager::BuildCompactionPlan()`（仅规划，不直接 memmove）
- 已落地：并发对象搬移主路径
    - `InstallPendingCompactionForForwarding()`：安装 ghost-from route；
    - `TryForwardObject()`：barrier assist 触发前驱安装后进入 forward；
    - `ForceForwardPendingLOSObjects()`：GC 线程主动收敛，覆盖未被 mutator 触达对象。
- 已落地：finalize 收敛闭环
    - `FinalizePendingCompaction()` 仅回收已 `IsForwarded()` 的条目；
    - 未完成条目保留在 pending plan，下一轮继续收敛；
    - 仅当 pending 为空时才解冻 LOS 并结束 compacting 状态。

### 6.1 当前调试结论

- 已排除部分 `exit=137` 的非算法因素（macOS AMFI 代码签名拦截）；
- 当前主要功能性失败点为运行期断言：`failed to add dirty units [...]`，说明 free tree 插入仍存在重复区间路径；
- 下一步重点：在 LOS finalize/回收链路中进一步消除重复 `AddGarbageUnits` 路径，并补充最小化诊断日志定位重复来源。

---

## 7. 2026-02-22 策略切换草案（待确认后执行）

### 7.1 新策略理解（与你的口径对齐）

1. **触发策略调整**：
    - 不再因为 “LOS 插入失败/碎片条件” 触发本轮 LOS compact；
    - LOS 插入失败时，**直接 fallback 到 MAIN**，不中断，不触发 compact。

2. **compact 执行时机调整**：
    - LOS compact 仅在 **OOM 原因 GC** 中执行；
    - 且 compact 整理阶段放在 **STW** 内执行。

3. **大对象迁移策略**：
    - 在 compact 整理完成后，将 MAIN 中符合条件的大对象迁回 LOS；
    - 该迁移过程同样放在 **STW** 内执行。

4. **并发方案保留**：
    - 现有“gc-helper 线程并发整理”路径保留代码；
    - 但默认 **禁用**（不参与当前默认执行路径）。

### 7.2 预期收益

- 将并发整理链路从默认路径剥离，优先保证正确性与可诊断性；
- 将整理与迁移收敛到 OOM-GC 的 STW 窗口，降低并发竞态复杂度；
- 保留并发实现作为后续可选能力（feature flag / 配置开关）。

### 7.3 待确认问题（确认后开始代码改造）

1. **OOM 范围定义**：
    - “仅 OOM GC 做 compact”是否严格指 `GCReason::oom`，不包含 `heuristic_sync`？

2. **STW 包裹边界**：
    - 是否要求 `LOS compact + MAIN→LOS 迁移` 全部在同一个 STW 区间内完成，
      还是允许“同轮 OOM GC 内的两个 STW 子阶段”？

3. **并发路径禁用粒度**：
    - 是“默认关闭但可配置开启”，还是“先在代码里硬禁用（仅保留实现，不暴露开关）”？

4. **MAIN→LOS 迁移对象范围**：
    - 仅迁移“因 LOS 插入失败 fallback 到 MAIN 的大对象”，
      还是迁移“当前 MAIN 中所有满足 LOS 条件的大对象”？

5. **与现有冻结逻辑关系**：
    - OOM STW 模式下是否仍保留 `isLOSAllocFrozen` 机制（用于防御性一致性），
      还是在该路径中不再需要显式冻结标记？

> 注：本节仅为策略对齐文档，代码暂不改动；待上述问题确认后再进入实现阶段。

### 7.4 已确认口径（2026-02-22）

- OOM 范围：仅 `GCReason::oom` 执行该策略；
- STW 边界：`LOS compact + MAIN→LOS 迁移` 在同一段 STW 内完成；
- 并发逻辑：gc-helper 并发整理路径保留实现，但当前硬禁用、不暴露开关；
- 迁移范围：仅迁移“LOS 插入失败后 fallback 到 MAIN”的记录对象。
