# Step 1：分配器改造详细设计 —— 从 Bump-Pointer 到 Bitmap/SizeClass 模式

> 本文档是 [Go 风格 GC 迁移方案](Go风格GC迁移方案.md) Step 1 的独立详细设计文档。
> 目标：在保留 Region 框架的前提下，改造分配器以支持 Mark-Sweep 模式下对象的原地回收与空间复用。
>
> 2026-03-23 当前代码状态修正：
> 1. `MSRegionManager` 已经是独立主路径，负责虚拟大预留、segment 增长和 mmap-front `MSRegionInfo[]`。
> 2. 当前大对象主分配路径是 `allocSize >= 16KB` 直接走 `MSRegionManager::TakeRegionForLarge()`，不是文档里描述的“主路径先到 `LargeObjectSpace`”。
> 3. `LargeObjectSpace` 及其 `Contains/MarkObject/SweepUnmarkedObjects` 辅助路径已经删除；当前 managed object 只保留主堆内的 small/large region。
> 4. `HeapGrowthManager`、`Scavenger`、`HeapGoalController` 并没有以独立类形态落地，相关能力已部分内联进 `MSRegionManager` 和现有回收/阈值逻辑。

---

## 目录

1. [现有分配器架构深度分析](#1-现有分配器架构深度分析)
2. [问题分析：为什么需要改造](#2-问题分析为什么需要改造)
3. [MS RegionInfo 元数据改造设计](#3-ms-regioninfo-元数据改造设计) → Step1.1
4. [Size Class、线程本地分配与 Sweep 设计](#4-size-class线程本地分配与-sweep-设计) → Step1.2
5. [大对象分配与堆增长](#5-大对象分配与堆增长) → Step1.3
6. [代码修改清单](#6-代码修改清单) → Step1.1 / 1.2 / 1.3
7. [验证方案](#7-验证方案) → Step1.1 / 1.2 / 1.3

---

## 1. 现有分配器架构深度分析

### 1.1 整体类层次

```
Allocator (抽象接口)
  └── RegionSpace (唯一实现)
        ├── RegionManager    - Region 生命周期管理（15+ 个 RegionList）
        ├── FreeRegionManager - 空闲 Unit 树（CartesianTree）
        ├── RegionInfo       - 单个 Region 的元数据 + bump-pointer 分配
        └── AllocBuffer      - 线程本地分配缓存（每线程一个 tlRegion）
```

核心源文件：

| 文件 | 职责 |
|------|------|
| `src/Heap/Allocator/RegionSpace.h/cpp` | 分配器入口，协调 RegionManager 与 AllocBuffer |
| `src/Heap/Allocator/RegionInfo.h` | Region 元数据：`allocPtr` bump-pointer、bitmap、RouteState FSM |
| `src/Heap/Allocator/AllocBuffer.h` | 线程本地：`tlRegion`、`preparedRegion`、raw-pointer regions |
| `src/Heap/Allocator/RegionManager.h` | 所有 RegionList 的增删查改、TakeRegion 三级查找 |
| `src/Heap/Allocator/FreeRegionManager.h` | 笛卡尔树管理 dirty/released Unit |
| `src/Heap/Allocator/RegionList.h` | 双向链表 + mutex 的基础数据结构 |

### 1.2 内存层次模型

```
             HeapSpace (虚拟地址预留区)
                │
     ┌──────────┼──────────┐
     │ Unit 0   │ Unit 1   │ Unit 2   │ Unit 3   │ ...
     │ (16KB)*  │ (16KB)*  │ (16KB)*  │ (16KB)*  │
     └──────────┴──────────┴──────────┴──────────┘
     │←────── Region (N Units) ──────→│

     * macOS: UNIT_SIZE = 16KB (PAGE_SIZE)
     * Linux: UNIT_SIZE = 4KB  (PAGE_SIZE)
     * 默认 Region = 128KB = 32 Units (Linux) 或 8 Units (macOS)
```

- **Unit**：最小物理内存管理单位，等于操作系统页大小
- **Region**：由连续 N 个 Unit 组成，是 GC 管理的基本单位
- **RegionInfo**：每个 Unit 有一个 `UnitInfo`，Region 首 Unit 的 `UnitInfo` 即 `RegionInfo`

### 1.3 RegionInfo 核心结构（`UnitMetadata`）

```cpp
// src/Heap/Allocator/RegionInfo.h:990
struct UnitMetadata {
    // ---- 快速分配区（位于结构体最前端） ----
    uintptr_t allocPtr;             // bump-pointer 当前位置
    uintptr_t regionEnd;            // Region 尾地址（allocPtr <= regionEnd）

    // ---- 链表索引 ----
    uint32_t  nextRegionIdx;
    uint32_t  prevRegionIdx;

    // ---- GC 统计 ----
    uint32_t  liveByteCount;        // 本轮 GC 中存活字节数
    int32_t   rawPointerObjectCount;

    // ---- GC 元信息 ----
    LiveInfo* liveInfo;             // 懒分配：markBitmap + liveBitmap
    uintptr_t regionEnd0;           // Ghost Region 备份
    RouteInfo routeInfo;            // Forward 路由信息

    // ---- 标志位 ----
    uint8_t unitRole  : 4;         // SMALL_SIZED_UNITS / LARGE_SIZED_UNITS
    uint8_t regionType: 4;         // FREE_REGION / THREAD_LOCAL_REGION / FULL_PINNED_REGION / ...
    uint8_t isTraceRegion: 1;
    uint8_t inGhostFromRegion: 1;
    // ...
};
```

**关键观察**：`allocPtr` 位于结构体偏移 0 处，是热路径上最频繁访问的字段。

### 1.4 Bump-Pointer 分配——当前快速路径

```cpp
// RegionInfo::Alloc（非原子版，线程本地 Region 使用）
uintptr_t Alloc(size_t size) {
    size_t limit = GetRegionEnd();
    if (metadata.allocPtr + size <= limit) {
        uintptr_t addr = metadata.allocPtr;
        metadata.allocPtr += size;     // 指针单调递增
        return addr;
    }
    return 0;  // Region 空间不足
}
```

这是经典的 bump-pointer 分配：**O(1)** 时间、无碎片、无锁（线程本地）。但**不支持 Region 内部空间回收**——一旦分配，中间释放的对象空间无法复用，只能整个 Region 在 Copy GC 后整体回收。

### 1.5 分配入口完整调用链

```
用户代码 new → CompilerCalls → RegionSpace::Allocate(size, allocType)
  │
  ├── ToAllocSize(size): objSize + HEADER_SIZE(0), 8-byte alignment
  │
  └── TryAllocateOnce(totalSize, allocType):
        │
        ├── allocType == PINNED_OBJECT?
        │     └── RegionManager::AllocPinned(totalSize)
        │           ├── 尝试 recentPinnedRegionList 头部空闲 slot
        │           ├── 遍历 freePinnedSlotLists 找合适 slot
        │           └── 新建 Region → 加入 recentPinnedRegionList
        │
        ├── totalSize >= largeObjectThreshold?
        │     └── RegionManager::AllocLarge(totalSize)
        │           ├── 计算需要的 Unit 数：ceil(size / UNIT_SIZE)
        │           ├── FreeRegionManager::TakeRegion(nUnit)
        │           └── 加入 recentLargeRegionList
        │
        └── MOVEABLE_OBJECT (小对象，最常见路径):
              └── AllocBuffer::Allocate(totalSize, allocType)
                    │
                    ├── 快速路径: tlRegion->Alloc(totalSize)  ← 上面的 bump-pointer
                    │     └── 成功 → 返回地址
                    │
                    └── 慢速路径: AllocateImpl(totalSize, allocType)
                          ├── tlRegion 已满 → EnlistFull(tlRegion)
                          ├── 尝试 preparedRegion → CAS 交换为 tlRegion
                          ├── TakeRegion → 从 garbageRegionList/FreeRegionManager/InactiveZone 获取
                          └── 分配成功 → 返回
```

### 1.6 Region 获取三级查找（`RegionManager::TakeRegion`）

```
TakeRegion(nUnit = 1):
  │
  ├── 第 1 级: garbageRegionList.TakeHeadRegion()
  │     └── 上轮 GC 标记为垃圾的 Region（已清理，可直接复用）
  │
  ├── 第 2 级: FreeRegionManager::TakeRegion(nUnit)
  │     ├── dirtyUnitTree（有内容但未释放的空闲 Unit，需 ClearUnits)
  │     └── releasedUnitTree（已 madvise 释放的 Unit，需 recommit）
  │
  └── 第 3 级: InactiveZone 扩展
        └── 从预留虚拟地址空间中 commit 新页
```

### 1.7 AllocBuffer —— 线程本地缓存

```cpp
class AllocBuffer {
    RegionInfo*             tlRegion;      // 当前活跃 Region（快速路径直接使用）
    std::atomic<RegionInfo*> preparedRegion;// 预备 Region（异步预分配）
    RegionList              tlRawPointerRegions;      // raw-pointer 对象
    RegionList              tlLargeRawPointerRegions;  // 大 raw-pointer 对象
    std::list<BaseObject*>  stackRoots;    // 并发枚举期间暂存根对象
};
```

**编译器约束**：`static_assert(offsetof(AllocBuffer, tlRegion) == 0)` —— 编译器生成的分配代码**硬编码**假设 `tlRegion` 在 `AllocBuffer` 偏移 0 处，JIT 直接通过 TLS 指针 + 0 偏移读取 `tlRegion`，再通过 `tlRegion + 0` 偏移读取 `allocPtr` 实现最快分配。

### 1.8 RegionList 管理总览

RegionManager 维护了 15+ 个 RegionList，各有不同生命周期意义：

| RegionList | 用途 | 生命周期 |
|------------|------|----------|
| `tlRegionList` | 当前正被线程使用的 Region | 分配中 → 满后 enlist |
| `recentFullRegionList` | 本轮刚填满的 Region | 满 → GC 开始时转 fromList |
| `fromRegionList` | 本轮 GC 的 from-space | GC 标记+转发 → 回收 |
| `garbageRegionList` | 整体垃圾 Region（无存活对象）| GC 后 → 被 TakeRegion 复用 |
| `recentPinnedRegionList` | Pinned 对象 Region | 分配中 → GC 转 oldPinned |
| `oldPinnedRegionList` | 历史 Pinned Region | 累积 |
| `recentLargeRegionList` | 本轮新分配的大对象 Region | 类似 recentFull |
| `oldLargeRegionList` | 历史大对象 Region | 累积 |
| `rawPointerPinnedRegionList` | raw-pointer pinned Region | 特殊管理 |
| `unmovableFromRegionList` | 不可移动的 from Region | Copy GC 特有 |

---

## 2. 问题分析：为什么需要改造

### 2.1 Copy GC 分配器的核心假设

当前分配器建立在 **Copy GC** 的核心假设之上：

1. **对象可移动**：存活对象在 Forward 阶段被复制到新 Region，旧 Region 整体释放
2. **Region 整体回收**：Region 要么全保留，要么全回收——不存在"Region 内部分空闲"
3. **Bump-pointer 足够**：因为不需要 Region 内部复用，线性分配是最优策略
4. **无需 sweep**：Copy GC 天然不需要 sweep 阶段——活对象已搬走，死对象的 Region 直接释放

### 2.2 Mark-Sweep 模式下的矛盾

在 Mark-Sweep GC 中，对象**不移动**。GC 标记完成后：

```
Region (128KB):
  [obj1:live][obj2:dead][obj3:live][obj4:dead][obj5:dead][obj6:live]...
              ↑ 空洞      ↑ 空洞          ↑ 空洞
```

如果继续使用 bump-pointer：
- **allocPtr 只能向前**：已分配区域中的空洞无法复用
- **内存利用率急剧下降**：每轮 GC 后大量 Region 处于"半满"状态，既不能回收也不能继续分配
- **碎片化**：不同大小对象混合在同一 Region 中，sweep 后的空洞大小不一

### 2.3 具体影响估算

假设平均对象存活率 50%，Region 大小 128KB：

| 场景 | Copy GC | Mark-Sweep (无改造) | Mark-Sweep (改造后) |
|------|---------|--------------------|--------------------|
| Region 满后可用空间 | 0（整体释放） | ~64KB 空洞不可用 | ~64KB 通过 bitmap 复用 |
| 内存利用率 | 约 50%（需要 to-space）| 约 50%（空洞浪费）| 约 85-95%（bitmap 复用）|
| 分配吞吐 | 极高（纯 bump-pointer）| 极高（但很快 OOM）| 高（bitmap 快速路径 + bump-pointer 兜底）|

### 2.4 改造目标

1. **Region 内部空间复用**：GC sweep 后，通过 allocBitmap 管理 Region 内的空闲 slot，供后续分配复用
2. **保留 Region 框架**：不改变 Region/Unit/RegionManager 的整体架构，最小化改动范围
3. **保留快速路径**：新 Region 第一次分配仍使用 bump-pointer（最快），bitmap 仅用于 sweep 后复用
4. **兼容编译器假设**：保持 `AllocBuffer::tlRegion` 在偏移 0 处、`RegionInfo::allocPtr` 在偏移 0 处的约束
5. **编译期可切换**：通过 `MRT_USE_MARK_SWEEP` 宏控制，与现有 Copy GC 代码共存

---

## 3. MS RegionInfo 元数据改造设计

> **详细设计文档**：[Step1.1_MS_RegionInfo元数据改造设计.md](Step1.1_MS_RegionInfo元数据改造设计.md)

本节设计面向 Mark-Sweep 的精简元数据 `MSRegionInfo`（64 bytes），替代 CopyGC 的 `UnitMetadata`（~120 bytes）。核心要点：

- **UnitInfo 不再需要**：CopyGC 的三大角色（地址反查、Ghost Region、多 Unit 拼接）在 MS 模式下全部消失
- **保留堆前方镜像布局**：O(1) 查找、cache 友好、零额外分配，粒度改为 per-Region（32KB）
- **MSRegionInfo 64 bytes**：删除 ~56B CopyGC 字段（routeInfo、Ghost 备份、rwLock 等），新增 bitmap 指针、sizeClass、sweepState
- **Bitmap 替代 free-list**：allocBitmap + markBitmap，1 bit/slot，sweep = `~markBitmap`，CTZ 分配 O(1)
- **Region 大小 32KB**：8192 Region/256MB 堆，markBitmap 512B/Region，大对象阈值 16KB
- **MSRegionType 简化**：5 种状态（FREE / BUMP / PARTIAL / FULL / LARGE）替代 CopyGC 15+ 种

---

## 4. Size Class、线程本地分配与 Sweep 设计

> **详细设计文档**：[Step1.2_SizeClass与线程本地分配设计.md](Step1.2_SizeClass与线程本地分配设计.md)

本节设计涵盖 Size Class、线程本地分配、分配路径改造和 Sweep 集成，已提取到独立文档。

### 4.1 三大核心组件

- **Size Class 设计**（Step1.2 §1）：36 级 size class 表（8B ~ 16KB），`SizeToClass` O(1) 查找，内部碎片 ≤ 25%
- **AllocBufferMS —— 线程本地分配缓存**（Step1.2 §2）：对标 Go mcache，每线程 36 个活跃 Region（per size class），`FastAlloc` 内联 bitmap 分配，`SlowAlloc` 从 CentralRegionPool 补充，`FlushAll` 在 GC STW 时归还
- **CentralRegionPool —— 全局 Region 池**（Step1.2 §3）：对标 Go mcentral，per-class partial/full 列表，Lazy Sweep 策略（按需 sweep full Region），per-class 锁粒度

### 4.2 分配路径改造（Step1.2 §2、§4）

- **编译期分派**：通过 `MRT_USE_MARK_SWEEP` 宏在 `TryAllocateOnce` 中切换 Copy GC / MS 路径
- **小对象路径**：`SizeToClass(size)` → `AllocBufferMS::FastAlloc(cls)` 内联 bitmap → 失败走 `SlowAlloc` 从 CentralRegionPool 补充
- **大对象路径**：当前 `allocSize >= 16KB` 即直接走 `MSRegionManager::TakeRegionForLarge()` 注册为堆内 large region；历史 `LargeObjectSpace`/Tier2 方案已删除，详见 [Step1.3](Step1.3_大对象分配与堆增长设计.md) §1-§2
- **Pinned 对象**：MS 模式下所有对象天然不移动，pinned 语义自动满足，统一走 `AllocBufferMS`
- **Raw-Pointer 对象**：仍使用独立 Region（不混入 size-class Region），复用现有逻辑
- **编译器快速路径兼容**：保持 `AllocBuffer::tlRegion` 偏移 0、`allocPtr` 偏移 0 的约束；新 Region 首次分配仍用 bump-pointer，bitmap 复用在运行时慢速路径处理
- **重试与 OOM**：`ShouldRetryAllocation` 适配 MS 模式 —— 失败 → 触发 `TriggerMarkSweepGC` → 重试 → OOM

### 4.3 Sweep 集成点（Step1.2 §3、§4；大对象见 [Step1.3](Step1.3_大对象分配与堆增长设计.md) §2.5、§5.5）

- **GC 阶段**：`IDLE → MARK_SETUP(STW) → CONCURRENT_MARK → MARK_TERM(STW) → CONCURRENT_SWEEP → IDLE`
- **Sweep 粒度**：Region 级，`SweepRegion()` 通过 `allocBitmap = ~markBitmap` 批量更新空闲状态
- **小对象 Lazy Sweep**（Go 做法）：分配线程从 CentralRegionPool full 列表取 Region → 就地 sweep → 变成 partial。零额外暂停，按需清理
- **大对象 Eager Sweep**：Tier 1 遍历 `LargeRegionList` 归还 FreeRegionManager；Tier 2 直接 `munmap`
- **Region 整体回收**：sweep 后 `IsCompletelyFree()` 的 Region 释放回 FreeRegionManager
- **快速跳过**：`liveByteCount == 0` → 全死直接释放；`== allocatedSize` → 全活跳过 sweep
- **Bitmap 清理**：sweep 时清理 markBitmap，详见 [Step1.1](Step1.1_MS_RegionInfo元数据改造设计.md) §3.5

---

## 5. 大对象分配与堆增长

> **详细设计文档**：[Step1.3_大对象分配与堆增长设计.md](Step1.3_大对象分配与堆增长设计.md)

本节涵盖大对象分配和动态堆管理两大主题，已提取到独立文档。核心要点：

- **当前主路径分类**：`<16KB` 走 size class；`>=16KB` 走 `MSRegionManager::TakeRegionForLarge()` 注册 large region
- **Tier 1 低地址优先**：CartesianTree `FindLowestAddr` 递归算法，减少堆碎片，Region 32KB 倍数对齐
- **Tier 1 GC 集成**：独立 `LargeRegionList` 跟踪大对象 Region，标记完成后 Eager Sweep 回收
- **当前地址识别**：`Heap::IsHeapAddress()` 仍是主堆范围检查；collector/allocator 不再维护 LOS 慢速路径，heap 外地址直接按非堆对象处理
- **堆增长**：64GB `MAP_NORESERVE` 虚拟预留 + 256MB Segment 分段提交，元数据向低地址增长
- **堆增长与触发**：`MSRegionManager` 已内联 segment 增长能力，GC 阈值与回收仍由现有 `GCStats` / `RegionSpace` / `FinalizerProcessor` 协调，而不是单独的 `HeapGoalController` / `Scavenger`
- **与 Go 对比**：Go 无 Tier 2 区分、Go 用 radix tree 而非 CartesianTree、Go 用 arena（64MB）而非 Segment（256MB）

---

## 6. 代码修改清单

> 各子文档包含独立的详细修改清单，本节为汇总索引。

### 6.1 按子文档分布

| 子文档 | 修改范围 | 链接 |
|--------|---------|------|
| **Step1.1** | `MSRegionInfo` 64B 结构体、bitmap 字段、`AllocFromBitmap`/`SweepRegion`/`InitForSizeClass` 方法、堆前方镜像布局适配 | [Step1.1 §3.4-3.7](Step1.1_MS_RegionInfo元数据改造设计.md) |
| **Step1.2** | `SizeClass.h/cpp`（新增）、`AllocBufferMS.h/cpp`（新增）、`CentralRegionPool.h/cpp`（新增）、`RegionSpace.cpp` 分派逻辑、Region 获取/释放全流程 | [Step1.2 §1-4](Step1.2_SizeClass与线程本地分配设计.md) |
| **Step1.3** | 大对象 large region、`MSRegionManager` segment 增长、历史 Tier2 方案校准 | [Step1.3 §7](Step1.3_大对象分配与堆增长设计.md) |

### 6.2 跨文档公共修改

| 文件 | 修改内容 | 来源 |
|------|---------|------|
| `CMakeLists.txt` | 新增源文件、`MRT_USE_MARK_SWEEP` 编译宏 | 各子文档 |
| `config.cmake` | `MARK_SWEEP_FLAG` 选项 | Step1.2 |
| `build.py` | `--gc-mode marksweep` 命令行参数 | Step1.2 |
| `src/Heap/MAllocator/MSRegionSpace.cpp` | large allocation 统一走 `TakeRegionForLarge()` + `RegisterLargeObjectMS()` | Step1.3 |
| `src/Heap/MCollector/MarkSweepCollector.cpp` | 非 heap 地址不再维护 LOS fallback，large reclaim 仅处理 in-heap large region | Step1.3 |

### 6.3 不修改的文件（确保兼容）

| 文件 | 原因 |
|------|------|
| `src/Heap/Allocator/AllocBuffer.h` | 保持编译器兼容性，不修改 `tlRegion` 偏移 |
| `src/Heap/Allocator/RegionList.h` | 基础数据结构不变 |
| `src/Heap/Collector/` | Collector 改造在 Step 2-4，本 Step 不涉及 |
| `src/Heap/CJBarrier/` | 屏障改造在 Step 4，本 Step 不涉及 |

### 6.4 实施顺序

```
第 1 阶段：基础设施（可独立编译测试）
  ├── [1a] SizeClass.h/cpp        (Step1.2)
  ├── [1b] MSRegionInfo + bitmap   (Step1.1)
  └── [1c] FreeRegionManager 改造  (Step1.3)

第 2 阶段：核心组件（依赖第 1 阶段）
  ├── [2a] CentralRegionPool       (Step1.2)
  ├── [2b] AllocBufferMS            (Step1.2)
  └── [2c] Large region 注册与回收   (Step1.3)

第 3 阶段：集成（依赖第 2 阶段）
  ├── [3a] RegionSpace 分派逻辑    (Step1.2)
  ├── [3b] large-region GC 集成     (Step1.3)
  ├── [3c] 堆增长/收缩集成          (Step1.3)
  └── [3d] 构建配置 + 编译验证
```

---

## 7. 验证方案

> 各子文档包含对应组件的详细测试用例，本节为汇总。

### 7.1 编译验证

```bash
# Copy GC 模式（默认，确保无回归）
python3 build.py build --target native --build-type debug -v 0.0.1

# Mark-Sweep 模式
python3 build.py build --target native --build-type debug -v 0.0.1 --gc-mode marksweep
```

### 7.2 单元测试按子文档分布

| 测试范围 | 子文档 | 关键测试点 |
|---------|--------|-----------|
| MSRegionInfo bitmap 操作 | [Step1.1](Step1.1_MS_RegionInfo元数据改造设计.md) | `AllocFromBitmap` CTZ、`SweepRegion` bitmap 翻转、Region 整体释放 |
| SizeClass 查找 | [Step1.2](Step1.2_SizeClass与线程本地分配设计.md) | `SizeToClass` 边界值、回环 `ClassToSize(SizeToClass(n)) >= n` |
| AllocBufferMS 线程本地 | [Step1.2](Step1.2_SizeClass与线程本地分配设计.md) | 不同 size class 使用不同 Region、`FlushAll` 归还 |
| CentralRegionPool Lazy Sweep | [Step1.2](Step1.2_SizeClass与线程本地分配设计.md) | full→sweep→partial 转换、全死 Region 释放 |
| 大对象分配/回收 | [Step1.3](Step1.3_大对象分配与堆增长设计.md) | large region 对齐、`RegisterLargeObjectMS()`、`CollectLargeGarbage()` 回收 |
| 堆增长/收缩 | [Step1.3](Step1.3_大对象分配与堆增长设计.md) | Segment 提交、`ShrinkHeap()` 尾部回卷、阈值联动 |

### 7.3 集成测试

使用 `gc/gc.cj` 进行 E2E 验证：编译 MS 版 runtime → 替换 SDK dylib → `cjc gc.cj && ./main`。

> **注意**：本 Step 仅改造分配器，完整 GC 流程在 Step 2/3。初期集成测试需禁用 GC 或使用手动标记+sweep harness。

### 7.4 性能基准

| 指标 | 合格标准 |
|------|----------|
| 单线程分配吞吐 | ≥ Copy GC 的 90% |
| 多线程（8 线程）分配吞吐 | ≥ Copy GC 的 85% |
| bitmap 分配 P99 延迟 | ≤ 200ns |
| sweep 后 Region 复用率 | 新增 Region 减少 ≥ 40% |

### 7.5 正确性检查清单

- [ ] `SizeToClass` 对 1B ~ 16KB 返回正确 class
- [ ] Region sweep 后 `freeSlotCount + liveSlotCount == totalSlotCount`
- [ ] `popcount(allocBitmap) == freeSlotCount` 恒成立
- [ ] 编译器快速路径分配的对象地址对齐到 8B
- [ ] `AllocBuffer::tlRegion` 偏移仍为 0（`static_assert`）
- [ ] MS 模式和 Copy GC 模式可独立编译且互不影响
- [ ] heap 外地址不会被误判为托管对象
- [ ] 大对象 large region 分配和 GC 回收路径正常

---

---

## 附录 A：关键数据结构对照表

| Go 概念 | Cangjie 现有概念 | Step 1 新增 | 子文档 |
|---------|-----------------|-------------|--------|
| mspan | Region(RegionInfo) | MSRegionInfo + size class | Step1.1 |
| mcache | AllocBuffer | AllocBufferMS | Step1.2 |
| mcentral | - (无直接对应) | CentralRegionPool | Step1.2 |
| mheap | RegionManager + FreeRegionManager | MSRegionManager（已内联增长/回收能力） | Step1.3 |
| mheap.alloc (large) | RegionManager::AllocLarge | `MSRegionManager::TakeRegionForLarge`（当前主路径） | Step1.3 |
| pageAlloc | FreeRegionManager (CartesianTree) | 低地址优先 FindLowestAddr | Step1.3 |
| arena | 固定堆 (Init 一次性 mmap) | Segment (256MB) | Step1.3 |
| bgscavenge | - (无) | 现有 `FinalizerProcessor + ReclaimGarbageMemory` 回收链 | Step1.3 |
| GOGC | - (无) | 现有 `GCStats` / threshold 更新逻辑 | Step1.3 |
| size class table | - (无) | SizeClassInfo (36 级) | Step1.2 |
| sweepgen | - (无) | sweepState | Step1.1 |
| freeindex | allocPtr | allocPtr + allocBitmap | Step1.1 |
| spanClass=0 (large) | recentLargeRegionList | large region（当前唯一大对象主路径） | Step1.3 |

## 附录 B：与后续 Step 的接口约定

本 Step 为后续 Step 预留以下接口：

| 接口 | 调用方 | 作用 | 子文档 |
|------|--------|------|--------|
| `MSRegionInfo::SweepRegion()` | Step 3 (MarkSweepCollector) | GC 结束后 sweep Region | Step1.1 |
| `MSRegionInfo::AllocFromBitmap()` | AllocBufferMS | sweep 后复用死亡对象 slot | Step1.1 |
| `CentralRegionPool::SweepAll()` | Step 3 (MarkSweepCollector) | 批量 sweep 所有 full Region | Step1.2 |
| `CentralRegionPool::VisitAllRegions()` | Step 2 (Marker) | 标记阶段遍历所有 Region 中的对象 | Step1.2 |
| `AllocBufferMS::FlushAll()` | Step 2 (STW MarkSetup) | STW 时归还所有线程本地 Region | Step1.2 |
| `SizeClassInfo::SizeToClass()` | Step 4 (WriteBarrier) | 屏障中判断对象大小 | Step1.2 |
| `MSRegionInfo::IsCompletelyFree()` | Step 3 (Collector) | 判断 Region 是否可整体释放 | Step1.1 |
| `LargeRegionList::SweepAndReclaim()` | Step 3 (Collector) | Tier 1 大对象 Eager Sweep | Step1.3 |
| `Heap::IsHeapAddress()` | Barrier / Collector | 主堆地址识别 | Step1.3 |
| `MSRegionManager::GrowOneSegment()` | `MSRegionManager::TakeRegion*` | 堆空间不足时动态扩展 | Step1.3 |
| `RegionSpace::ReclaimGarbageMemory()` | Step 4 / gc-helper | 回收与归还物理页 | Step1.3 |
| `GCStats` threshold 更新 | 分配路径 | 判断是否需要触发 GC | Step1.3 |
