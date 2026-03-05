# 从 Copy GC 迁移到 Go 风格并发标记-清扫 GC：分步实施方案

## 文档目标

基于当前 Cangjie Runtime 的区域复制式 GC（Regional Copying GC），按步骤改造为类似 Go 的**并发三色标记-清扫（Concurrent Tri-color Mark-and-Sweep）** GC，以实现亚毫秒级 STW 停顿和更简洁的指针语义。

---

## 第一部分：架构对比与迁移动机

### 1.1 当前 Cangjie GC 架构（Regional Copying GC）

```
分配: RegionSpace (bump-pointer per-thread AllocBuffer)
          │
标记: Enum → Trace (并发, SATB写屏障, bitmap-on-region)
          │
搬移: Preforward → Forward (并行复制, CAS锁定, tagged-pointer自愈)
          │
回收: CollectSmallSpace → garbageRegionList → dirtyUnitTree → releasedUnitTree
```

**核心特征**：
- **移动式**：存活对象从 from-space 复制到 to-space，消除碎片
- **Tagged Pointer**：指针低位编码 tag ID（0/1 交替），用于区分"已转发"/"未转发"
- **自愈读写屏障**：读/写遇到 from-space 指针时触发延迟转发
- **6 套屏障实现**：Idle / Enum / Trace / PostTrace / Preforward / Forward
- **Phase 数量**：8 个（IDLE → INIT → ENUM → TRACE → CLEAR_SATB → POST_TRACE → PREFORWARD → FORWARD）

### 1.2 Go GC 架构（Concurrent Tri-color Mark-and-Sweep）

```
分配: TCMalloc风格 (mcache → mcentral → mheap, size-class)
          │
标记: Mark Setup (STW) → Concurrent Mark (并发, 混合写屏障) → Mark Termination (STW)
          │
清扫: Concurrent Sweep (并发, lazy sweep per-span)
```

**核心特征**：
- **非移动式**：对象分配后永不移动，通过清扫回收死亡对象的空间
- **无 tagged pointer**：不需要指针修正，无读屏障开销
- **仅写屏障**：混合写屏障（Dijkstra 插入 + Yuasa 删除），**无读屏障**
- **3 个 Phase**：Off → Mark → Mark Termination (→ Sweep 是并发不算 phase)
- **亚毫秒 STW**：仅 Mark Setup 和 Mark Termination 两个极短 STW

### 1.3 关键差异对比

| 维度 | 当前 Cangjie GC | Go GC | 迁移影响 |
|------|-----------------|-------|----------|
| **对象移动** | 移动（Copy） | 不移动 | 删除 Forward 阶段，增加 Sweep |
| **碎片处理** | 复制压缩消除 | 靠 size-class 和 span 回收缓解 | 需改造分配器 |
| **指针语义** | Tagged Pointer + 自愈 | 原始指针 | 删除 tag 机制，大幅简化 |
| **读屏障** | 有（Forward 阶段修正） | 无 | 删除读屏障，提升 mutator 吞吐 |
| **写屏障** | SATB（Trace 阶段） | 混合写屏障（Mark 阶段） | 改造写屏障实现 |
| **STW 次数** | 多点（Enum/Preforward 有同步窗口） | 2 次极短 STW | 简化同步 |
| **GC Phase** | 8 个 | 3 个 | 大幅简化状态机 |
| **大对象** | 独立 Region, 标记清除 | 独立 span, 标记清除 | 基本一致 |

### 1.4 迁移动机

1. **降低 STW 延迟**：消除 Forward 阶段的同步窗口，仅保留两个亚毫秒级 STW
2. **移除读屏障**：Go 无读屏障，mutator 热路径零额外开销
3. **简化指针语义**：去掉 tagged pointer 和 forwarding table，降低调试和维护复杂度
4. **减少 Phase 数量**：从 8 个减到 3 个，状态机更简洁
5. **提升吞吐量**：非移动式 GC 省去了大量对象复制开销（尤其大堆场景）

**代价**：
- 无内存压缩，可能产生碎片（通过 size-class 分配器缓解）
- 清扫阶段可能延迟内存释放（通过 lazy sweep 和后台 sweep 缓解）

---

## 第二部分：迁移总体策略

### 2.1 核心原则

1. **渐进式迁移**：通过编译宏 `MRT_USE_MARK_SWEEP`（与现有 `MRT_USE_COPYGC` 对偶）控制新旧 GC 切换，确保任何一步都可回退
2. **复用现有框架**：保留 `RegionSpace`/`RegionManager` 内存管理底座，保留 `GCThreadPool` 并行框架，保留 `MutatorManager` 同步机制
3. **自底向上**：先改分配器，再改标记，再加清扫，最后删老代码
4. **验证驱动**：每一步都必须通过现有测试用例 + 新增专项测试

### 2.2 迁移路线图

```
Step 0: 构建可切换 GC 框架         ← 基础设施
    │
Step 1: 改造分配器（free-list模式）  ← 为非移动式 GC 做准备
    │
Step 2: 实现三色标记 + Sweep          ← 核心算法替换
    │
Step 3: 实现 Go 风格混合写屏障      ← 屏障替换
    │
Step 4: 实现并发清扫                   ← 并行回收
    │
Step 5: 实现 GC Pacer                   ← 智能触发控制
    │
Step 6: 移除 Forward 相关代码        ← 清理旧路径
    │
Step 7: 性能调优与压力验证           ← 收尾
```

### 2.3 关键文件变更范围预估

| 模块 | 变更类型 | 关键文件 |
|------|----------|----------|
| Collector 框架 | 新增 | `src/Heap/Collector/MarkSweepCollector.h/cpp` |
| Barrier | 新增 + 改造 | `src/Heap/MSBarrier/` 目录（新建） |
| Allocator | 改造 | `src/Heap/Allocator/RegionSpace.cpp`, `RegionManager.cpp` |
| Heap 入口 | 改造 | `src/Heap/Heap.cpp`, `src/Heap/Heap.h` |
| CompilerCalls | 改造 | `src/CompilerCalls.cpp`（写屏障入口） |
| TypeDef | 改造 | `src/Heap/TypeDef.h`（新增 Phase 枚举） |
| CMake | 改造 | `CMakeLists.txt`, `config.cmake` |

### 2.4 兼容性承诺

- 新旧 GC 通过编译宏完全隔离，构建时选择
- 公共 API（`Cangjie.h`）保持不变
- 编译器侧 intrinsic 接口（`llvm.cj.*`）保持不变
- 对象模型（`GCTib`、`TypeInfo`）保持不变

---

## 第三部分：分步实施计划

### Step 0：前置准备——构建可切换的 GC 框架

#### 目标
在不影响现有 Copy GC 的前提下，搭建 Mark-Sweep GC 的编译开关和骨架代码。

#### 0.1 新增编译宏

**文件**: `config.cmake`

```cmake
# 新增 Mark-Sweep GC 开关（与 COPYGC_FLAG 互斥）
option(MARKSWEEP_FLAG "Enable Mark-Sweep GC (Go-style)" OFF)

if(MARKSWEEP_FLAG AND COPYGC_FLAG)
    message(FATAL_ERROR "COPYGC_FLAG and MARKSWEEP_FLAG are mutually exclusive")
endif()

if(MARKSWEEP_FLAG)
    add_definitions(-DMRT_USE_MARK_SWEEP=1)
endif()
```

**文件**: `build.py`

```python
# 新增命令行参数
parser.add_argument('--gc-mode', choices=['copygc', 'marksweep'], default='copygc',
                    help='Select GC algorithm')
```

#### 0.2 新建 Collector 骨架

**新建文件**: `src/Heap/Collector/MarkSweepCollector.h`

```cpp
#ifndef CANGJIE_RUNTIME_MARK_SWEEP_COLLECTOR_H
#define CANGJIE_RUNTIME_MARK_SWEEP_COLLECTOR_H

#include "TracingCollector.h"

namespace cj::gc {

// Go 风格 GC 的 Phase 定义
enum class MSGCPhase : uint8_t {
    OFF = 0,         // GC 未运行，等价于 IDLE
    MARK_SETUP,      // STW: 开启写屏障，扫描根
    MARK,            // 并发标记
    MARK_TERMINATION,// STW: 完成标记，关闭写屏障
    SWEEP,           // 并发清扫
};

class MarkSweepCollector : public TracingCollector {
public:
    void DoGarbageCollection() override;

private:
    // Phase 1: Mark Setup (STW)
    void MarkSetup();
    
    // Phase 2: Concurrent Mark
    void ConcurrentMark();
    
    // Phase 3: Mark Termination (STW)
    void MarkTermination();
    
    // Phase 4: Concurrent Sweep
    void ConcurrentSweep();
    
    // GC Pacer
    void UpdateGCTrigger();
    
    MSGCPhase currentPhase_ = MSGCPhase::OFF;
};

} // namespace cj::gc

#endif
```

#### 0.3 CollectorProxy 适配

**文件**: `src/Heap/Collector/CollectorProxy.h/cpp`

在 `CollectorProxy` 初始化时根据宏选择 collector：

```cpp
void CollectorProxy::Initialize() {
#if defined(MRT_USE_MARK_SWEEP)
    currentCollector_ = new MarkSweepCollector();
#else
    currentCollector_ = new WCollector();  // 现有 Copy GC
#endif
}
```

#### 0.4 验证标准
- `MRT_USE_MARK_SWEEP=0` 时，行为与改造前完全一致
- `MRT_USE_MARK_SWEEP=1` 时，可编译通过（骨架代码，GC 暂不执行实际回收）
- 现有全部测试用例通过

### Step 1：改造分配器——从 bump-pointer 到 free-list 混合模式

#### 目标
在非移动式 GC 下，对象分配后不再搬迁。需要改造 Region 内部分配策略，支持对象原地回收后空间复用。

#### 1.1 设计思路：Go 的 TCMalloc vs Cangjie 的 Region

Go 使用 TCMalloc 风格的 size-class 分配器（67 个 size class，8B ~ 32KB）。直接引入 TCMalloc 改动过大。我们采用折中方案：

**保留 Region 框架 + Region 内部引入 free-list**

```
┌─ Region (128KB) ──────────────────────────────┐
│ [obj1][obj2][free][obj3][free][free][obj4]...  │  ← Mark-Sweep 后有空洞
│         ↓                                       │
│ free-list: free→free→free                      │  ← 串联空闲块
└────────────────────────────────────────────────┘
```

#### 1.2 Size Class 引入

参考 Go 的做法，将小对象按 size class 分类（简化版）：

```cpp
// 新增文件: src/Heap/Allocator/SizeClass.h
static constexpr size_t kNumSizeClasses = 32;
static constexpr size_t kSizeClasses[] = {
    8, 16, 24, 32, 48, 64, 80, 96,
    112, 128, 160, 192, 224, 256, 320, 384,
    448, 512, 640, 768, 896, 1024, 1280, 1536,
    1792, 2048, 2560, 3072, 3584, 4096, 5120, 6144,
    // > 6144 走 Large 路径（与现有大对象路径合并）
};

// 对象大小 → size class 映射（查表 O(1)）
inline uint8_t SizeToClass(size_t size);
// size class → 对象大小
inline size_t ClassToSize(uint8_t cls);
```

#### 1.3 Region 内部 free-list

当前 Region 使用 bump-pointer 分配（`RegionInfo::Alloc` 仅递增 `allocPosition`）。Mark-Sweep 后需要回收 Region 内部空洞。

**改造 `RegionInfo`**：

```cpp
class RegionInfo {
    // 现有字段保留
    std::atomic<uintptr_t> allocPosition;  // bump-pointer 顶端
    
    // 新增: Mark-Sweep 模式下的 free-list
#ifdef MRT_USE_MARK_SWEEP
    FreeSlot* freeList_;       // 已回收空间的链表头
    size_t    freeBytes_;      // 当前 Region 可用空闲字节数
    uint8_t   sizeClass_;      // 此 Region 服务的 size class
#endif
};

struct FreeSlot {
    FreeSlot* next;  // 利用死亡对象的前 8 字节存储链表指针
};
```

#### 1.4 分配路径改造

```
TryAllocateOnce(size):
  ├── PINNED_OBJECT → AllocPinned (不变)
  ├── size >= largeObjectThreshold → AllocLarge (不变)
  └── MOVEABLE_OBJECT:
      ├── [Copy GC] AllocBuffer::Allocate → bump-pointer (现有路径)
      └── [Mark-Sweep] AllocBuffer::AllocateMS:
            1. cls = SizeToClass(size)
            2. region = tlRegionPerClass_[cls]  // 每 size class 一个本地 Region
            3. if (region && region->freeList_):
                   从 free-list 摘取一个 slot → 返回
            4. elif (region && region->allocPosition 有空间):
                   bump-pointer 分配 → 返回
            5. else:
                   获取新 Region (现有 TakeRegion 逻辑) → 设定 sizeClass
                   bump-pointer 分配 → 返回
```

#### 1.5 线程本地缓存（对应 Go 的 mcache）

```cpp
class AllocBufferMS {
    // 每个 size class 维护一个当前活跃 Region
    RegionInfo* tlRegionPerClass_[kNumSizeClasses];
    
    // 分配入口
    uintptr_t Allocate(size_t size);
    
    // Sweep 后将 Region 归还到全局 central 池
    void ReturnFullRegions();
};
```

#### 1.6 Central 池（对应 Go 的 mcentral）

```cpp
class CentralRegionPool {
    // 每个 size class 两个列表:
    //   - partial: 有空闲 slot 的 Region（sweep 后可复用）
    //   - full:    已满的 Region（等待下次 sweep）
    struct SizeClassBucket {
        MutexLock lock;
        RegionList partialRegions;  // 含空闲 slot
        RegionList fullRegions;     // 已满
    };
    
    SizeClassBucket buckets_[kNumSizeClasses];
    
    // 线程本地 Region 用完时，从 partial 取；没有则从 RegionManager 新建
    RegionInfo* GetRegion(uint8_t cls);
    // 线程归还 Region
    void ReturnRegion(RegionInfo* region, uint8_t cls);
};
```

#### 1.7 关键设计决策

| 决策 | 选择 | 理由 |
|------|------|------|
| size class 数量 | 32 | 平衡内部碎片（≤12.5%）与复杂度 |
| 大对象阈值 | 保持现有（默认 32KB） | 与现有大对象路径兼容 |
| Region 大小 | 保持 128KB | 不改变底层内存管理 |
| Pinned 对象 | 保持不变 | Mark-Sweep 下所有对象都不移动，pinned 语义自然满足 |

#### 1.8 验证标准

- Mark-Sweep 模式下对象分配路径正确（包括小对象、大对象、Pinned）
- 分配性能基准：单线程分配吞吐 ≥ 现有 Copy GC 的 90%
- free-list 回收后再分配功能正确（本步暂用手动标记测试）

### Step 2：实现三色标记——替换 Forward 为 Sweep

#### 目标
实现 Go 风格的并发三色标记算法，用 Sweep 替代 Forward 阶段完成内存回收。

#### 2.1 三色抽象

```
白色 (White): 未访问，GC 结束后视为垃圾
灰色 (Gray):  已发现但子引用未全部扫描（在工作队列中）
黑色 (Black): 本身及直接子引用都已处理
```

在实现层面，颜色映射到 Region bitmap：

```cpp
// 复用现有 RegionInfo 的 mark bitmap
// 未标记 = 白色
// 已标记 + 在 workStack 中 = 灰色
// 已标记 + 不在 workStack 中 = 黑色
```

#### 2.2 GC 主流程（替换 `DoGarbageCollection`）

```cpp
void MarkSweepCollector::DoGarbageCollection() {
    // ============ Phase 1: Mark Setup (STW) ============
    MarkSetup();
    
    // ============ Phase 2: Concurrent Mark ============
    ConcurrentMark();
    
    // ============ Phase 3: Mark Termination (STW) ============
    MarkTermination();
    
    // ============ Phase 4: Concurrent Sweep ============
    ConcurrentSweep();
    
    // ============ 更新 GC 触发阈值 ============
    UpdateGCTrigger();
}
```

#### 2.3 Mark Setup（STW，目标 < 100μs）

```cpp
void MarkSweepCollector::MarkSetup() {
    // 1. 进入 STW
    ScopedStopTheWorld stw;
    
    // 2. 切换 Phase
    TransitionToGCPhase(MS_PHASE_MARK_SETUP);
    
    // 3. 安装混合写屏障（详见 Step 3）
    InstallWriteBarrier();
    
    // 4. 清除上一轮的 mark bitmap
    //    不需要全部清零，使用 "mark bit flip" 技巧：
    //    每轮 GC 使用不同的 mark bit 值（0/1 交替），
    //    这样无需清除 bitmap，只需翻转期望值
    FlipMarkBit();
    
    // 5. 扫描所有 Goroutine/CJThread 的栈（收集根）
    //    这是 STW 的主要耗时。Go 通过预编译 stack map 加速。
    //    复用现有 EnumAllCommonRoots 逻辑
    EnumRoots();
    
    // 6. 根入队到全局 workStack（灰色队列）
    //    所有根引用的对象标为灰色
    
    // 7. 退出 STW，进入并发标记
    TransitionToGCPhase(MS_PHASE_MARK);
}
```

**关键复用**：
- 根扫描直接复用 `TracingCollector::EnumAllCommonRoots()`
- 栈扫描复用现有 safepoint + `GcPhaseEnum` 机制
- bitmap 使用现有 `RegionInfo::MarkObject()` 基础设施

#### 2.4 Concurrent Mark（并发，复用现有标记框架）

```cpp
void MarkSweepCollector::ConcurrentMark() {
    // 复用现有并行标记框架
    // TracingImpl() 已经是并行工作窃取式标记
    
    // 1. GC 线程 + GCThreadPool 并行从 workStack 取对象
    // 2. 对每个灰色对象:
    //    a. 遍历其 ref 字段
    //    b. 若目标对象未标记(白色) → 标记(灰→黑) + 入队
    // 3. workStack 为空 → 标记完成
    
    DoTracing();  // 复用现有 TracingImpl
    
    // 处理 SATB/写屏障 buffer（由混合写屏障产生）
    ProcessWriteBarrierBuffer();
}
```

**与现有 Trace 阶段的关键区别**：
- 不需要标记新分配对象（Go 策略：Mark 期间新分配的对象默认标记为黑色）
- 写屏障产生的 buffer 处理逻辑不同（详见 Step 3）

#### 2.5 Mark Termination（STW，目标 < 100μs）

```cpp
void MarkSweepCollector::MarkTermination() {
    // 1. 进入 STW
    ScopedStopTheWorld stw;
    
    TransitionToGCPhase(MS_PHASE_MARK_TERMINATION);
    
    // 2. 排空写屏障 buffer（re-scan 残留灰色对象）
    DrainWriteBarrierBuffer();
    
    // 3. 处理弱引用
    //    复用现有 WeakRefBuffer::ClearWeakRefBuffer()
    ClearWeakRefs();
    
    // 4. 处理 Finalizer
    //    复用现有 DoResurrection() + FinalizerProcessor
    ProcessFinalizers();
    
    // 5. 关闭写屏障
    UninstallWriteBarrier();
    
    // 6. 退出 STW，进入并发清扫
    TransitionToGCPhase(MS_PHASE_SWEEP);
}
```

#### 2.6 Sweep 实现（本步先实现 STW Sweep，Step 4 改为并发）

```cpp
void MarkSweepCollector::SweepRegion(RegionInfo* region) {
    uint8_t cls = region->sizeClass_;
    size_t  slotSize = ClassToSize(cls);
    
    uintptr_t cursor = region->GetDataStart();
    uintptr_t end    = region->GetAllocPosition();
    
    FreeSlot* newFreeList = nullptr;
    size_t freeBytes = 0;
    size_t liveBytes = 0;
    
    while (cursor < end) {
        if (region->IsMarked(cursor)) {
            // 存活对象: 什么都不做
            liveBytes += slotSize;
        } else {
            // 死亡对象: 加入 free-list
            FreeSlot* slot = reinterpret_cast<FreeSlot*>(cursor);
            slot->next = newFreeList;
            newFreeList = slot;
            freeBytes += slotSize;
        }
        cursor += slotSize;
    }
    
    region->freeList_ = newFreeList;
    region->freeBytes_ = freeBytes;
    
    if (liveBytes == 0) {
        // 整个 Region 无存活 → 回收为空 Region
        ReturnToGarbageRegionList(region);
    } else {
        // 有存活 + 有空闲 → 放入 central partial list
        centralPool_->ReturnRegion(region, cls);
    }
}
```

#### 2.7 大对象回收

大对象（> largeObjectThreshold）独占 Region，sweep 逻辑更简单：

```cpp
void MarkSweepCollector::SweepLargeRegions() {
    // 遍历 oldLargeRegionList
    for (auto* region : oldLargeRegionList) {
        if (region->IsLargeObjectMarked()) {
            // 存活 → 保留
            recentLargeRegionList.Add(region);
        } else {
            // 死亡 → 整个 Region 回收
            ReturnToGarbageRegionList(region);
        }
    }
}
```

这与现有 `CollectLargeGarbage` 逻辑高度一致，可直接复用。

#### 2.8 标记阶段新分配对象策略

Go 的策略：**Mark 期间新分配的对象直接标记为黑色**（不会被回收）。

这比当前 `MarkNewObject` 策略更简洁：

```cpp
// 在 Mark-Sweep 模式下，Allocate 路径中:
uintptr_t RegionSpace::Allocate(size_t size, ...) {
    uintptr_t addr = TryAllocateOnce(size, ...);
    if (addr && IsInMarkPhase()) {
        MarkObject(addr);  // 新对象直接标为黑色
    }
    return addr;
}
```

#### 2.9 此步骤涉及的源码变更清单

| 文件 | 变更说明 |
|------|----------|
| `src/Heap/Collector/MarkSweepCollector.h/cpp` | 新建：完整 GC 主流程 |
| `src/Heap/Allocator/RegionInfo.h` | 改造：新增 `freeList_`、`freeBytes_`、`sizeClass_` |
| `src/Heap/Allocator/SizeClass.h` | 新建：size class 定义和映射 |
| `src/Heap/Allocator/RegionSpace.cpp` | 改造：Mark 期间新分配对象标黑 |
| `src/Heap/Collector/TracingCollector.cpp` | 条件编译复用（标记主循环） |

#### 2.10 验证标准

- 基础 GC 正确性：分配 → 标记 → 清扫 → 再分配，无泄漏无误回收
- 弱引用和 Finalizer 行为正确
- 大对象回收正确
- 现有 `gc/gc.cj` 测试通过（切换到 Mark-Sweep 模式下）

### Step 3：实现 Go 风格混合写屏障

#### 目标
用 Go 的混合写屏障（Hybrid Write Barrier）替换当前的 SATB 写屏障 + 读屏障体系，实现**无读屏障**的并发标记。

#### 3.1 Go 混合写屏障原理

Go 1.8 引入的混合写屏障结合了两种经典屏障：

```
writePointer(slot, ptr):
    shade(*slot)    // Yuasa 删除屏障: 旧值标灰（防止漏标已断开的引用）
    if current goroutine's stack has been scanned:
        shade(ptr)  // Dijkstra 插入屏障: 新值也标灰
    *slot = ptr     // 实际写入
```

关键优势：**栈上写操作不需要屏障**。因为：
- 栈在 Mark Setup 阶段已被完整扫描
- 栈上新写入的指针要么来自堆（堆写有屏障保护），要么来自栈（栈对象在本轮 GC 前已扫描）

#### 3.2 与当前 Cangjie 屏障系统的对比

| 维度 | 当前 Cangjie | Go 混合写屏障 | 变化 |
|------|-------------|-------------|------|
| 写屏障 | SATB（仅记录旧值） | 混合（旧值 + 条件新值） | 改造 |
| 读屏障 | 有（Forward 自愈） | 无 | **完全删除** |
| 屏障数量 | 6 套 | 2 套（Off + On） | 大幅简化 |
| 栈写操作 | 需要屏障 | 不需要 | **编译器可优化** |
| 编译器配合 | `llvm.cj.gcwrite/gcread.*` | 仅 `llvm.cj.gcwrite.*`（保留），`gcread.*` 降级为普通 load | 减少 intrinsic |

#### 3.3 实现方案

**新建文件**: `src/Heap/MSBarrier/MSWriteBarrier.h/cpp`

```cpp
class MSWriteBarrier : public Barrier {
public:
    // ==== 写屏障 ====
    
    // 对象字段 ref 写入
    void WriteReference(uintptr_t obj, uintptr_t* slot, uintptr_t newVal) override {
        uintptr_t oldVal = *slot;
        
        // 1. shade(oldVal) — 旧值标灰
        if (oldVal != 0 && !IsMarked(oldVal)) {
            MarkAndPush(oldVal);  // 标记 + 入 workStack
        }
        
        // 2. shade(newVal) — 新值标灰（仅当写入来自已扫描的栈/堆）
        //    简化处理: 对堆上写一律对新值也标灰
        //    （Go 对栈写跳过，但栈写本身不走此路径）
        if (newVal != 0 && !IsMarked(newVal)) {
            MarkAndPush(newVal);
        }
        
        // 3. 实际写入
        *slot = newVal;
    }
    
    // 对象字段 struct 写入
    void WriteStruct(uintptr_t dst, uintptr_t src, size_t size,
                     const RefFieldOffsets* offsets) override {
        // 先对 dst 中旧 ref 字段执行 shade(old)
        for (auto offset : *offsets) {
            uintptr_t oldRef = *(uintptr_t*)(dst + offset);
            if (oldRef != 0 && !IsMarked(oldRef)) {
                MarkAndPush(oldRef);
            }
        }
        // 执行 memcpy
        memmove_s(dst, size, src, size);
        // 对新 ref 字段执行 shade(new)
        for (auto offset : *offsets) {
            uintptr_t newRef = *(uintptr_t*)(dst + offset);
            if (newRef != 0 && !IsMarked(newRef)) {
                MarkAndPush(newRef);
            }
        }
    }
    
    // ==== 读屏障: 完全去除 ====
    
    uintptr_t ReadReference(uintptr_t obj, uintptr_t* slot) override {
        return *slot;  // 直接返回，零开销！
    }
    
    uintptr_t ReadStruct(uintptr_t dst, uintptr_t src, size_t size) override {
        memmove_s(dst, size, src, size);  // 直接 memcpy
        return dst;
    }
    
private:
    // 标记对象并推入工作栈
    void MarkAndPush(uintptr_t objAddr);
};

class MSIdleBarrier : public Barrier {
    // GC 不活跃时的屏障: 所有操作都是直接读写，零开销
    void WriteReference(uintptr_t obj, uintptr_t* slot, uintptr_t newVal) override {
        *slot = newVal;
    }
    uintptr_t ReadReference(uintptr_t obj, uintptr_t* slot) override {
        return *slot;
    }
    // ... 其他方法同理
};
```

#### 3.4 写屏障 Buffer 优化

Go 使用 per-P（per-Processor）的写屏障 buffer 来批量处理，避免每次写操作都竞争全局 workStack：

```cpp
class WriteBarrierBuffer {
    static constexpr size_t kBufferSize = 256;
    
    uintptr_t buf_[kBufferSize];
    size_t    pos_ = 0;
    
    void Push(uintptr_t obj) {
        buf_[pos_++] = obj;
        if (pos_ >= kBufferSize) {
            Flush();  // 批量转移到全局 workStack
        }
    }
    
    void Flush();
};

// 每个 CJThread/Mutator 持有一个 WriteBarrierBuffer
thread_local WriteBarrierBuffer* tlWriteBarrierBuffer;
```

#### 3.5 CompilerCalls 改造

**文件**: `src/CompilerCalls.cpp`

```cpp
// Mark-Sweep 模式下，读操作不需要屏障
#ifdef MRT_USE_MARK_SWEEP

uintptr_t CJ_MCC_ReadRefField(uintptr_t obj, uintptr_t* slot) {
    return *slot;  // 直接返回，无屏障
}

uintptr_t CJ_MCC_ReadStaticRef(uintptr_t* slot) {
    return *slot;  // 直接返回，无屏障
}

#endif
```

写操作保持走屏障路径，但简化为只需一套实现（Mark On/Off）。

#### 3.6 屏障安装/卸载

```cpp
void HeapImpl::InstallBarrierMS(MSGCPhase phase) {
    switch (phase) {
    case MSGCPhase::MARK:
    case MSGCPhase::MARK_SETUP:
    case MSGCPhase::MARK_TERMINATION:
        currentBarrier_ = &msWriteBarrier_;  // 混合写屏障
        break;
    default:
        currentBarrier_ = &msIdleBarrier_;   // 无屏障
        break;
    }
}
```

对比现有 6 套屏障切换，大幅简化为 2 套（On/Off）。

#### 3.7 Tagged Pointer 移除

Mark-Sweep 下不需要 tagged pointer：
- 删除 `FlipTagID()` 调用
- 删除 `RefField::IsTagged()` / `GetTagID()` 检查
- `ReadReference` 不需要 `TryUpdateRefField` / `TryUntagRefField`
- `ForwardObjectImpl` 完全删除

```cpp
// 条件编译: Mark-Sweep 模式下跳过所有 tag 相关逻辑
#ifdef MRT_USE_MARK_SWEEP
    // 指针就是原始地址，不含任何 tag
    #define REF_TO_ADDR(ref) (ref)
    #define ADDR_TO_REF(addr) (addr)
#else
    // 现有 Copy GC 的 tag 逻辑保留
    #define REF_TO_ADDR(ref) RemoveTag(ref)
    #define ADDR_TO_REF(addr) AddTag(addr)
#endif
```

#### 3.8 验证标准

- 并发标记期间写屏障正确：不遗漏活跃对象
- 读操作零额外开销（直接 load）
- 写屏障 buffer 批量刷新无 race condition
- 多线程压力测试通过（高并发分配 + 写 + GC）

### Step 4：实现并发清扫（Concurrent Sweep）

#### 目标
将 Step 2 中的 STW Sweep 改为并发执行，Sweep 工作由后台线程 + 分配时 lazy sweep 协同完成，进一步降低停顿。

#### 4.1 Go 的 Sweep 策略

Go 的 sweep 是完全并发的，采用两种机制协同：
1. **Background Sweep**：后台 goroutine 在 GC 结束后异步清扫
2. **Lazy Sweep**：分配时如果需要新 span，先清扫一个 span 再使用

这保证了：
- Sweep 不会造成任何 STW
- 分配不会被 sweep 阻塞太久（一次只 sweep 一个 span/region）

#### 4.2 实现方案

```cpp
class ConcurrentSweeper {
public:
    // ====== 后台 Sweep ======
    
    // GC Mark 完成后启动后台 sweep
    void StartBackgroundSweep() {
        sweepDone_.store(false, std::memory_order_release);
        // 将所有需要 sweep 的 Region 加入 sweepQueue
        CollectSweepableRegions();
        // 唤醒后台 sweep 线程
        NotifySweepThread();
    }
    
    // 后台线程主循环
    void BackgroundSweepLoop() {
        while (true) {
            WaitForSweepRequest();
            
            RegionInfo* region;
            while ((region = TakeSweepRegion()) != nullptr) {
                SweepOneRegion(region);  // sweep 一个 Region
            }
            
            sweepDone_.store(true, std::memory_order_release);
        }
    }
    
    // ====== Lazy Sweep（分配时触发） ======
    
    // 分配器需要新 Region 时调用
    RegionInfo* SweepAndGetRegion(uint8_t sizeClass) {
        // 从 sweepQueue 中取一个同 sizeClass 的 Region
        RegionInfo* region = TakeSweepRegionForClass(sizeClass);
        if (region == nullptr) return nullptr;
        
        // 原地 sweep
        SweepOneRegion(region);
        
        // 若有空闲 slot，直接返回给分配器
        if (region->freeBytes_ > 0) {
            return region;
        }
        // 全部存活 → 放入 full list，继续找下一个
        centralPool_->ReturnFullRegion(region, sizeClass);
        return nullptr;
    }
    
    // ====== 单 Region Sweep ======
    
    void SweepOneRegion(RegionInfo* region) {
        uint8_t cls = region->sizeClass_;
        size_t slotSize = ClassToSize(cls);
        
        uintptr_t cursor = region->GetDataStart();
        uintptr_t end    = region->GetAllocPosition();
        
        FreeSlot* freeHead = nullptr;
        size_t freeBytes = 0;
        size_t liveCount = 0;
        
        while (cursor < end) {
            if (IsMarkedForCurrentCycle(cursor)) {
                // 存活
                liveCount++;
            } else {
                // 死亡 → 串入 free-list
                // 可选: 调用析构函数（如果有 finalizer，应已在 Mark Termination 处理）
                FreeSlot* slot = reinterpret_cast<FreeSlot*>(cursor);
                slot->next = freeHead;
                freeHead = slot;
                freeBytes += slotSize;
            }
            cursor += slotSize;
        }
        
        region->freeList_ = freeHead;
        region->freeBytes_ = freeBytes;
        
        if (liveCount == 0) {
            // 整个 Region 空 → 归还到 garbageRegionList
            ReturnEmptyRegion(region);
        }
    }
    
    bool IsSweepDone() const {
        return sweepDone_.load(std::memory_order_acquire);
    }

private:
    std::atomic<bool> sweepDone_{true};
    
    // 无锁队列: 待 sweep 的 Region
    // 使用 atomic TakeHead 保证后台线程和分配线程不冲突
    AtomicRegionQueue sweepQueue_;
    
    // 按 sizeClass 分桶的 sweep 队列（用于 lazy sweep 快速匹配）
    AtomicRegionQueue sweepQueuePerClass_[kNumSizeClasses];
    
    void CollectSweepableRegions() {
        // 将 recentFullRegionList、fullRegionList 等中的 Region
        // 按 sizeClass 放入对应 sweepQueuePerClass
    }
};
```

#### 4.3 分配路径与 Sweep 的集成

```
AllocateMS(size):
  cls = SizeToClass(size)
  1. 尝试 tlRegionPerClass_[cls] 的 free-list
  2. 尝试 tlRegionPerClass_[cls] 的 bump-pointer
  3. 从 CentralRegionPool::GetRegion(cls):
     3a. 先查 partialRegions（已 sweep 有空闲）
     3b. 若无 → sweeper.SweepAndGetRegion(cls)  // lazy sweep
     3c. 若无 → RegionManager::TakeRegion()       // 新建
  4. 若仍失败 → 触发同步 GC
```

#### 4.4 与现有后台回收的兼容

当前 `FinalizerProcessor` 负责后台回收（`ReclaimGarbageMemory`）。在 Mark-Sweep 模式下：

```
FinalizerProcessor::Run():
  1. 执行 Finalizer（不变）
  2. 若 GC 刚完成 → 启动 BackgroundSweep（替代 ReclaimGarbageMemory）
  3. Sweep 产生的空 Region → garbageRegionList → dirtyUnitTree → releasedUnitTree
     （复用现有三层回收逻辑）
```

#### 4.5 大对象的并发 Sweep

大对象独占 Region，sweep 更简单（检查一个 mark bit 即可），可嵌入后台 sweep 流程中。

#### 4.6 验证标准

- 后台 sweep 和 lazy sweep 并行执行无 data race
- 分配不被 sweep 长时间阻塞（每次 lazy sweep 仅处理一个 Region）
- 内存及时释放：sweep 后空 Region 正确归还
- 多线程分配 + 并发 GC 无泄漏无误回收

### Step 5：实现 GC Pacer（触发控制器）

#### 目标
实现类似 Go 的 GC Pacer，智能决定何时触发下一次 GC，平衡内存使用和 GC 频率。

#### 5.1 Go 的 Pacer 设计

Go 的 GC 触发由两个机制控制：

1. **GOGC**（默认 100）：堆增长到上次 GC 后存活量的 `(1 + GOGC/100)` 倍时触发
   - 例如上次 GC 后存活 100MB，GOGC=100 → 堆到 200MB 时触发
2. **GOMEMLIMIT**（Go 1.19+）：硬性内存上限，接近上限时更积极地 GC

Pacer 还有一个"GC CPU 目标"（默认 25%），会动态调节 mark assist 的工作量，使 GC 占用的 CPU 不超过目标值。

#### 5.2 实现方案

```cpp
class GCPacer {
public:
    // ====== 配置 ======
    
    // 对应 GOGC，默认 100（堆翻倍时触发）
    // 设为 -1 表示禁用自动 GC
    int32_t gcPercent_ = 100;
    
    // 对应 GOMEMLIMIT，默认 MaxInt64（无限制）
    size_t memoryLimit_ = SIZE_MAX;
    
    // GC CPU 目标占比（0.0~1.0），默认 0.25
    double gcCPUFraction_ = 0.25;
    
    // ====== 状态 ======
    
    // 上次 GC 后的存活字节数
    size_t lastLiveBytes_ = 0;
    
    // 触发阈值（字节）
    size_t triggerBytes_ = 0;
    
    // 当前已分配字节数
    std::atomic<size_t> allocatedBytes_{0};
    
    // ====== 核心逻辑 ======
    
    // GC 完成后调用，更新触发阈值
    void UpdateAfterGC(size_t liveBytes, size_t totalAllocated) {
        lastLiveBytes_ = liveBytes;
        
        // 基于 GOGC 的触发点
        size_t gcPercentTrigger = 0;
        if (gcPercent_ > 0) {
            gcPercentTrigger = liveBytes + liveBytes * gcPercent_ / 100;
        }
        
        // 基于 GOMEMLIMIT 的触发点（预留 sweep 和分配缓冲）
        size_t memLimitTrigger = memoryLimit_;
        if (memoryLimit_ != SIZE_MAX) {
            // 在 limit 的 95% 处触发，留出缓冲
            memLimitTrigger = memoryLimit_ * 95 / 100;
        }
        
        // 取较小值作为实际触发点
        triggerBytes_ = std::min(gcPercentTrigger, memLimitTrigger);
        
        // 重置分配计数器
        allocatedBytes_.store(0, std::memory_order_relaxed);
    }
    
    // 分配路径中调用，检查是否需要触发 GC
    bool ShouldTriggerGC(size_t allocSize) {
        size_t total = allocatedBytes_.fetch_add(allocSize, std::memory_order_relaxed) + allocSize;
        return total + lastLiveBytes_ >= triggerBytes_;
    }
    
    // 获取环境变量配置
    void InitFromEnv() {
        // 读取 CJ_GC_PERCENT（对应 GOGC）
        const char* gcPercent = getenv("CJ_GC_PERCENT");
        if (gcPercent) {
            gcPercent_ = atoi(gcPercent);
        }
        
        // 读取 CJ_MEM_LIMIT（对应 GOMEMLIMIT）
        const char* memLimit = getenv("CJ_MEM_LIMIT");
        if (memLimit) {
            memoryLimit_ = ParseSize(memLimit);
        }
    }
};
```

#### 5.3 与现有触发机制的关系

| 现有机制 | Pacer 中对应 | 处理方式 |
|---------|-------------|---------|
| `GC_REASON_HEU`（启发式） | `ShouldTriggerGC()` | 替换为 Pacer 逻辑 |
| `GC_REASON_OOM`（内存不足） | 保留 | 作为 fallback |
| `GC_REASON_USER`（手动触发） | 保留 | 用户显式 GC |
| `GC_REASON_BACKUP`（兜底） | 可保留 | 长时间无 GC 的安全网 |
| `GC_REASON_FORCE`（强制） | 保留 | Runtime 内部强制 GC |

#### 5.4 Mark Assist（标记辅助）

Go 的一个重要机制：当 mutator 分配速度过快时，分配线程会被"征召"参与标记工作（mark assist），实现背压控制。

```cpp
// 在分配路径中:
uintptr_t Allocate(size_t size) {
    if (IsInMarkPhase() && pacer_.NeedAssist(size)) {
        // 此线程需要帮助完成一些标记工作，再允许分配
        DoMarkAssist(size);
    }
    return DoAllocate(size);
}

void DoMarkAssist(size_t debtBytes) {
    // 从全局 workStack 取灰色对象并标记
    // 直到"偿还"了 debtBytes 对应的标记工作量
    size_t scanned = 0;
    while (scanned < debtBytes) {
        uintptr_t obj = workStack_.TryPop();
        if (obj == 0) break;
        MarkObject(obj);
        scanned += GetObjectSize(obj);
    }
}
```

这防止了"分配比标记快"导致的堆无限膨胀。

#### 5.5 配置环境变量

```bash
# 设置 GC 触发比例（等价于 GOGC）
export CJ_GC_PERCENT=100    # 默认: 堆翻倍触发
export CJ_GC_PERCENT=200    # 堆三倍触发（更少 GC，更多内存）
export CJ_GC_PERCENT=50     # 堆 1.5 倍触发（更频繁 GC，更少内存）

# 设置内存上限（等价于 GOMEMLIMIT）
export CJ_MEM_LIMIT=1G      # 限制堆不超过 1GB
export CJ_MEM_LIMIT=512M    # 限制堆不超过 512MB
```

#### 5.6 验证标准

- 不同 `CJ_GC_PERCENT` 值下 GC 频率符合预期
- `CJ_MEM_LIMIT` 生效时堆大小不超限
- Mark Assist 能有效控制堆膨胀
- 高并发分配压力下不 OOM（Pacer 能及时触发 GC）

### Step 6：移除 Forward 相关代码路径

#### 目标
在 Mark-Sweep GC 稳定之后，清理 Copy GC 遗留的 Forward 相关代码（在 `MRT_USE_MARK_SWEEP` 宏保护下）。

#### 6.1 可删除的模块/机制

| 模块 | 说明 | 影响文件 |
|------|------|----------|
| **ForwardTable** | 对象路由映射表 | `src/Heap/Collector/ForwardData.h/cpp` |
| **GhostRegion** | from→to Region 视图 | `src/Heap/Allocator/GhostRegion.h/cpp` |
| **Tagged Pointer** | 指针低位 tag 编码 | `src/Heap/Allocator/RefField.h`, `TypeDef.h` |
| **ForwardBarrier** | Forward 阶段自愈屏障 | `src/Heap/WCollector/ForwardBarrier.cpp` |
| **PreforwardBarrier** | Preforward 阶段屏障 | `src/Heap/WCollector/PreforwardBarrier.cpp` |
| **CopyObject** | 对象复制逻辑 | `src/Heap/WCollector/WCollector.cpp` |
| **RouteState 状态机** | Region 搬迁状态 | `src/Heap/Allocator/RegionInfo.h` |
| **fromRegionList / unmovable** | 搬迁相关 Region 列表 | `src/Heap/Allocator/RegionManager.cpp` |
| **FlipTagID** | Tag 交替翻转 | `src/Heap/Collector/TracingCollector.cpp` |

#### 6.2 不可删除（Mark-Sweep 仍需复用）

| 模块 | 原因 |
|------|------|
| `RegionInfo::MarkObject` | 标记 bitmap 仍需使用 |
| `TracingImpl` 标记主循环 | 并发标记复用 |
| `EnumAllCommonRoots` | 根扫描复用 |
| `WeakRefBuffer` | 弱引用处理复用 |
| `FinalizerProcessor` | Finalizer 处理复用 |
| `SatbBuffer` → 改为 `WriteBarrierBuffer` | 缓冲思路复用，实现替换 |
| `GCThreadPool` | 并行框架复用 |
| `MutatorManager` | 同步框架复用 |

#### 6.3 GC Phase 简化

```cpp
// 删除（Copy GC 专有）:
//   GC_PHASE_INIT
//   GC_PHASE_CLEAR_SATB_BUFFER
//   GC_PHASE_POST_TRACE
//   GC_PHASE_PREFORWARD
//   GC_PHASE_FORWARD
//   GC_PHASE_RECLAIM_SATB_NODE

// Mark-Sweep 仅需:
enum class MSGCPhase : uint8_t {
    OFF = 0,
    MARK_SETUP = 1,
    MARK = 2,
    MARK_TERMINATION = 3,
    SWEEP = 4,
};
```

#### 6.4 屏障简化

```
现有 6 套屏障:
  IdleBarrier → EnumBarrier → TraceBarrier → PostTraceBarrier → PreforwardBarrier → ForwardBarrier

Mark-Sweep 仅需 2 套:
  MSIdleBarrier (GC Off)
  MSWriteBarrier (GC Mark)
```

#### 6.5 执行策略

1. 使用 `#ifdef MRT_USE_MARK_SWEEP` 条件编译隔离
2. 保留 Copy GC 代码直到 Mark-Sweep 完全验证通过
3. 最终可通过删除 `MRT_USE_COPYGC` 代码段彻底清理

#### 6.6 验证标准

- 删除后编译无 warning/error
- 二进制体积减小（预期减少约 15-20%）
- 所有测试用例在 Mark-Sweep 模式下通过

### Step 7：性能调优与验证

#### 目标
完成全面的性能评估、压力测试和调优，确保 Mark-Sweep GC 满足生产要求。

#### 7.1 性能基准测试矩阵

| 测试维度 | 测试场景 | 关键指标 |
|---------|---------|----------|
| **分配吞吐** | 单线程/多线程大量小对象分配 | ops/sec |
| **GC 停顿** | 各种堆大小下的 STW 时间 | p50/p99/max 停顿 |
| **GC 吞吐** | GC 占用 CPU 比例 | GC CPU% |
| **内存效率** | 峰值 RSS vs 存活对象大小 | 内存放大系数 |
| **碎片率** | 长时间运行后的内存碎片 | 碎片比例 |
| **尾延迟** | 应用级 p99 延迟 | μs |
| **大对象** | 大量大对象分配回收 | 回收及时性 |

#### 7.2 关键调优参数

```bash
# GC 触发比例（越小越频繁，内存越紧凑）
export CJ_GC_PERCENT=100   # 默认

# 内存上限
export CJ_MEM_LIMIT=1G

# Size class 配置（可调整）
export CJ_SIZE_CLASS_CONFIG=default

# 并行 mark 线程数
export CJ_GC_THREADS=4

# 后台 sweep 线程数
export CJ_SWEEP_THREADS=1
```

#### 7.3 与 Copy GC 性能对比（预期）

| 指标 | Copy GC | Mark-Sweep GC | 说明 |
|------|---------|---------------|------|
| STW 停顿 | ~1-10ms | **< 500μs** | 大幅降低 |
| 读屏障开销 | 有 | **无** | 热路径提速 |
| 写屏障开销 | SATB | 混合（略高） | 可接受 |
| 内存碎片 | 无（压缩） | **有** | 需以 size class 缓解 |
| 大堆 GC 吞吐 | 差（复制量大） | **好** | 不复制 |
| 小堆 GC 吞吐 | 好（快速复制） | 一般 | 看具体场景 |

#### 7.4 碎片缓解策略

1. **Size Class**：减少内部碎片（浪费 ≤ 12.5%）
2. **Region 回收策略**：空 Region 及时归还 OS
3. **CJ_GC_PERCENT 调小**：更频繁 GC，减少碎片累积
4. **紧急压缩**（可选 future work）：碎片率过高时，对部分 Region 做局部压缩

#### 7.5 压力测试用例

```
gc/gc.cj                    — 综合用例
gc/gc_large.cj              — 大对象
gc/gc_mixed.cj              — 混合大小
gc/gc_quick.cj              — 快速分配
gc/gc_tl_region_leak.cj     — 内存泄漏检测
gc/gc_shrink_verify.cj      — 堆收缩验证
gc/test_los_functional.cj   — LOS 功能验证
gc/test_los_perf.cj         — LOS 性能测试
```

每个用例需在 Mark-Sweep 模式下验证通过，并与 Copy GC 做性能对比。

#### 7.6 监控与可观测性

复用现有 `MRT_REPORT` 日志框架，新增以下指标：

```
[MS-GC] cycle=N live=100MB allocated=95MB trigger=200MB
[MS-GC] mark_setup_stw=80us mark_time=15ms mark_term_stw=50us sweep_time=8ms
[MS-GC] regions: total=1024 swept=800 freed=300 fragmentation=12%
[MS-GC] pacer: gc_percent=100 cpu_fraction=0.18 assist_bytes=5MB
```

---

## 第四部分：风险评估与回退方案

### 4.1 主要风险

| 风险 | 影响 | 缓解措施 |
|------|------|----------|
| **碎片导致 OOM** | 堆有空闲空间但无法分配 | Size class + Region 级回收 + 紧急压缩兜底 |
| **STW 不够短** | 根集过大导致 Mark Setup 超时 | 增量根扫描 + 栈预编译 |
| **并发标记漏标** | 对象被误回收 → crash | 写屏障形式化验证 + Fuzzing 测试 |
| **Sweep 延迟过大** | 内存释放不及时 | 增加后台 sweep 线程 + Lazy sweep 加速 |
| **OHOS 跨语言兼容** | Export Root 机制未适配 | 保留 Export Root 完整逻辑 |
| **编译器 IR 不兼容** | `llvm.cj.*` intrinsic 语义变化 | 保持接口不变，仅改运行时实现 |

### 4.2 回退策略

1. **编译时回退**：`MRT_USE_MARK_SWEEP=0`，完全回到 Copy GC，零风险
2. **运行时切换**（未来可选）：通过环境变量 `CJ_GC_MODE=copygc|marksweep` 运行时选择
3. **混合模式**（未来可选）：大部分对象走 Mark-Sweep，碎片率过高时对部分 Region 做 compaction

### 4.3 分步回退点

| 步骤 | 回退条件 | 回退动作 |
|------|---------|----------|
| Step 1 (分配器) | 性能退化 > 30% | 回退到 bump-pointer |
| Step 2 (三色标记) | 正确性问题 | 回退到 Copy GC 全路径 |
| Step 3 (混合写屏障) | 漏标问题 | 回退到 SATB |
| Step 4 (并发 Sweep) | 碎片严重 | 加紧急压缩机制 |
| Step 5 (Pacer) | 触发不合理 | 回退到现有启发式 |

### 4.4 测试覆盖要求

每一步交付前：
1. 现有所有 `gc/*.cj` 测试通过
2. 新增对应 Step 的专项测试
3. 24 小时稳定性压力测试通过
4. ASAN/TSAN 检查通过

---

## 附录

### A. Go GC 源码参考

| 文件 | 内容 | 参考价值 |
|------|------|----------|
| `runtime/mgc.go` | GC 主控制流 | Mark Setup/Termination 流程 |
| `runtime/mgcmark.go` | 并发标记 | 工作窃取、mark assist |
| `runtime/mgcsweep.go` | 并发清扫 | Lazy sweep、后台 sweep |
| `runtime/mgcpacer.go` | Pacer | 触发阈值计算 |
| `runtime/mbarrier.go` | 写屏障 | 混合写屏障实现 |
| `runtime/mheap.go` | 堆管理 | Span/arena 管理 |
| `runtime/mcache.go` | 线程本地缓存 | Per-P 分配 |
| `runtime/mcentral.go` | Central 缓存 | Size class 分发 |
| `runtime/sizeclasses.go` | Size class 定义 | 67 个 class 的具体值 |

### B. 当前 Cangjie GC 源码锚点

| 文件 | 内容 |
|------|------|
| `src/Heap/Heap.cpp` | 堆入口 + 屏障安装 |
| `src/Heap/Collector/TracingCollector.cpp` | 标记主循环（可复用） |
| `src/Heap/WCollector/WCollector.cpp` | `DoGarbageCollection` 主流程 |
| `src/Heap/Allocator/RegionSpace.cpp` | 分配主流程 |
| `src/Heap/Allocator/RegionManager.cpp` | Region 管理 + TakeRegion |
| `src/Heap/Allocator/RegionInfo.h` | Region 元数据 + mark bitmap |
| `src/CompilerCalls.cpp` | 编译器屏障入口 |
| `src/Heap/WCollector/ForwardBarrier.cpp` | Forward 屏障（待删除） |

### C. 名词对照表

| Go 概念 | Cangjie 对应 | 说明 |
|---------|-------------|------|
| goroutine | CJThread | 轻量级线程 |
| P (Processor) | Processor/Monitor | 调度实体 |
| span | Region | 内存管理单元 |
| arena | 堆虚拟空间 | mmap 的大块地址 |
| mcache | AllocBuffer/AllocBufferMS | 线程本地缓存 |
| mcentral | CentralRegionPool | 全局按 size class 分发 |
| mheap | RegionManager | 顶层堆管理 |
| GOGC | CJ_GC_PERCENT | GC 触发比例 |
| GOMEMLIMIT | CJ_MEM_LIMIT | 内存上限 |
| write barrier buffer | WriteBarrierBuffer | 写屏障缓冲 |
| mark bit | RegionInfo mark bitmap | 标记位 |
| gcWork | WorkStack | 灰色对象工作队列 |
| mark assist | DoMarkAssist | 分配时辅助标记 |

### D. 预估工作量

| 步骤 | 预估工期 | 复杂度 |
|------|---------|--------|
| Step 0: GC 框架 | 1 周 | 低 |
| Step 1: 分配器改造 | 2-3 周 | 中 |
| Step 2: 三色标记 + Sweep | 3-4 周 | 高 |
| Step 3: 混合写屏障 | 2-3 周 | 高 |
| Step 4: 并发 Sweep | 2 周 | 中 |
| Step 5: GC Pacer | 1-2 周 | 中 |
| Step 6: 清理旧代码 | 1 周 | 低 |
| Step 7: 性能调优 | 2-3 周 | 中 |
| **总计** | **14-19 周** | |
