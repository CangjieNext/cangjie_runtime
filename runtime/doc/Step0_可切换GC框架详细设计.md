# Step 0：前置准备详细设计 —— 构建可切换的 GC 框架

> 本文档是 [Go 风格 GC 迁移方案](Go风格GC迁移方案.md) Step 0 的独立详细设计文档。
> 目标：在不影响现有 Copy GC 的前提下，搭建 Mark-Sweep GC 的编译开关和骨架代码，使后续 Step 1-7 的改造有统一的基础设施。

---

## 目录

1. [现有构建系统与 GC 基础设施深度分析](#1-现有构建系统与-gc-基础设施深度分析)
2. [现有 Collector 继承体系深度分析](#2-现有-collector-继承体系深度分析)
3. [现有 Heap 初始化与 Barrier 安装深度分析](#3-现有-heap-初始化与-barrier-安装深度分析)
4. [编译宏与构建系统改造](#4-编译宏与构建系统改造)
5. [GCPhase 扩展设计](#5-gcphase-扩展设计)
6. [MarkSweepCollector 骨架设计](#6-markswepcollector-骨架设计)
7. [CollectorProxy 适配改造](#7-collectorproxy-适配改造)
8. [HeapImpl 条件编译改造](#8-heapimpl-条件编译改造)
9. [Barrier 基类与 MS Barrier 统一目录设计](#9-barrier-基类与-ms-barrier-统一目录设计)
10. [源码目录与 CMakeLists 组织](#10-源码目录与-cmakelists-组织)
11. [与现有基础设施的复用与改造清单](#11-与现有基础设施的复用与改造清单)
12. [验证方案](#12-验证方案)

---

## 1. 现有构建系统与 GC 基础设施深度分析

### 1.1 构建入口与标志传递

项目使用 `build.py` → CMake 两级构建。`build.py` 为每个目标平台硬编码了 CMake 变量：

```python
# build.py L118（native 目标）、L155（windows）、L205（ohos）、L240（android）、L281（ios）
# 所有平台统一硬编码：
"-DCOPYGC_FLAG=1",
"-DDOPRA_FLAG=1",
```

当前 **无法通过命令行参数选择 GC 策略**，`COPYGC_FLAG` 始终为 1。

### 1.2 CMake 变量消费路径

`config.cmake` 定义了所有编译标志的默认值和描述。`CMakeLists.txt` 中消费 `COPYGC_FLAG`：

```cmake
# CMakeLists.txt L208-L210
if (COPYGC_FLAG MATCHES 1)
    add_definitions(-DMRT_USE_COPYGC)
endif ()
```

**关键发现**：`MRT_USE_COPYGC` 宏虽然被定义，但**源代码中没有任何 `#ifdef MRT_USE_COPYGC` 条件编译**。该标志是历史遗留的空壳——当前代码是硬编码使用 CopyGC（WCollector）。

### 1.3 现有 CMake 标志全览

| 变量 | 默认值 | 用途 |
|------|--------|------|
| `COPYGC_FLAG` | 1（硬编码） | 定义 `MRT_USE_COPYGC`（未实际使用） |
| `DOPRA_FLAG` | 1 | 控制 CJThread（`MRT_USE_CJTHREAD` + `MRT_USE_CJTHREAD_RENAME`） |
| `OHOS_FLAG` | 0 | OpenHarmony 目标（1=aarch64, 2=x86_64, 3=arm） |
| `WINDOWS_FLAG` | 0 | Windows 交叉编译 |
| `MACOS_FLAG` | 自动检测 | macOS 构建 |
| `ANDROID_FLAG` | 0 | Android 构建 |
| `IOS_FLAG` | 0 | iOS 构建 |
| `ASAN_FLAG` | 0 | AddressSanitizer |
| `HWASAN_FLAG` | 0 | Hardware ASAN |
| `GWPASAN_SUPPORT_FLAG` | 自动检测 | GWP-ASAN 支持 |
| `SANITIZER_SUPPORT` | "" | `asan`/`tsan`/`hwasan` |
| `COV` | 0 | 覆盖率测试 |
| `RUNTIME_TRACE_FLAG` | 1 | CJThread trace |
| `DUMPADDRESS_FLAG` | 0 | `MRT_DUMP_ADDRESS` |
| `CJ_SDK_VERSION` | 由 `-v` 传入 | SDK 版本号 |

### 1.4 Heap 相关源码目录组织

```cmake
# src/Heap/CMakeLists.txt
set(SRC_LIST "Heap.cpp" "GcThreadPool.cpp")
add_subdirectory(Allocator)     # RegionSpace, RegionManager, MemMap, CartesianTree
add_subdirectory(Barrier)       # Barrier.cpp（仅基类）
add_subdirectory(Collector)     # Collector, TracingCollector, CopyCollector, CollectorProxy, FinalizerProcessor...
add_subdirectory(WCollector)    # WCollector + 6 个 GC-phase Barrier 实现
add_library(Heap STATIC ${SRC_LIST})
```

主 `CMakeLists.txt` 链接顺序：
```cmake
list(APPEND LINK_LIBS "boundscheck" "Base" "Common" ... "Heap" "Mutator" "ObjectModel"
    ... "Allocator" "Barrier" "Collector" "WCollector" ... )
```

**关键问题**：WCollector 目录**既包含收集器实现又包含所有 Barrier 实现**，Barrier 与 Collector 强耦合。新 GC 策略的 Barrier 必须放在独立目录中。

### 1.5 运行时初始化链

```
InitCJRuntime(RuntimeParam*)                [CangjieRuntimeApi.cpp L170]
  → CangjieRuntime::CreateAndInit()         [CangjieRuntime.cpp L106]
    → Init()
      → PagePool::Instance().Init()
      → MutatorManager::Init()
      → HeapManager::Init(heapParam)        [HeapManager.cpp]
        → Heap::GetHeap().Init(heapParam)   [Heap.cpp L167]
          → theSpace->Init(param)           // RegionSpace 初始化
          → collectorProxy.Init()           // → wCollector.Init()
          → collectorResources.Init()       // 启动 GC 主线程
      → TypeInfoManager::Init()
      → LoaderManager::Init()
      → ExceptionManager::Init()
```

### 1.6 公共 API 接口层分析

`Cangjie.h` 公共 API 与 GC 策略无关——良好的抽象层：

| API | 说明 | GC 策略相关性 |
|-----|------|---------------|
| `InitCJRuntime()` | 运行时初始化 | 无（通过 HeapParam 配置） |
| `FiniCJRuntime()` | 运行时终结 | 无 |
| `CJ_MRT_ForceFullGC()` | 强制 GC | 无（走 Collector 虚函数） |
| `MCC_NewObject()` | 对象分配 | 无（走 Allocator 虚函数） |
| `MCC_WriteRefField()` | 写引用 | 无（走 Barrier 虚函数） |
| `MCC_AtomicReadReference()` | 原子读 | 无（走 Barrier 虚函数） |

**结论**：公共 API 层（`Cangjie.h`、`CompilerCalls.h`）无需任何修改。

### 1.7 现有架构的耦合点总结

| 耦合点 | 位置 | 严重程度 | 说明 |
|--------|------|----------|------|
| HeapImpl 构造函数 | Heap.cpp L56-65 | **高** | 硬编码创建 7 个 WCollector-specific Barrier |
| CollectorProxy 成员 | CollectorProxy.h L81 | **高** | 硬编码 `WCollector wCollector` 成员 |
| InstallBarrier | Heap.cpp L189-205 | **高** | 硬编码 GCPhase→Barrier 映射 |
| Barrier→WCollector 转型 | 各 Barrier .cpp | **中** | `static_cast<WCollector&>(theCollector)` 向下转型 |
| GCPhase 枚举 | Collector.h L24-37 | **中** | 包含 CopyGC 专属 phase（PREFORWARD, FORWARD） |
| ForwardDataManager | TracingCollector.h | **低** | MS 模式不使用，已在 TracingCollector 层 |

**良好的抽象点**（无需改动）：

| 抽象点 | 位置 | 说明 |
|--------|------|------|
| Heap 接口层 | Heap.h | `Allocate`/`GetBarrier`/`InstallBarrier` 均为虚函数 |
| Collector 继承链 | Collector.h → TracingCollector.h | 基类提供通用标记/枚举能力 |
| Barrier 虚函数接口 | Barrier.h | 20+ 个虚方法，任何子类均可替换 |
| CompilerCalls 委托 | CompilerCalls.cpp | 全部通过 `Heap::GetBarrier().xxx()` 间接调用 |
| MutatorManager | MutatorManager.h | STW/LightSync 机制与 GC 策略无关 |
| CollectorResources | CollectorResources.h | GC 线程池/任务队列可在不同 Collector 间共享 |

---

## 2. 现有 Collector 继承体系深度分析

### 2.1 完整类继承链

```
Collector (抽象基类)                       ← src/Heap/Collector/Collector.h (121 行)
  ├── CollectorProxy : public Collector    ← src/Heap/Collector/CollectorProxy.h (85 行)
  │     (代理模式，内含 WCollector 实例，所有调用委托给 currentCollector)
  │
  └── TracingCollector : public Collector  ← src/Heap/Collector/TracingCollector.h (394 行)
        (抽象中间类，提供标记/枚举/追踪/终结器通用能力)
        │
        └── CopyCollector : public TracingCollector  ← src/Heap/Collector/CopyCollector.h
              (抽象类，添加对象复制/转发逻辑)
              │
              └── WCollector : public CopyCollector  ← src/Heap/WCollector/WCollector.h
                    (具体实现，Regional-Copying GC，"Smooth Collector")
```

### 2.2 Collector 基类（Collector.h）

```cpp
// 核心虚函数接口
class Collector {
public:
    virtual void Init() = 0;
    virtual void Fini() {}
    
    void RequestGC(GCReason reason, bool async);
    virtual GCPhase GetGCPhase() const;
    virtual void SetGCPhase(const GCPhase phase);
    
    virtual void MarkNewObject(BaseObject*) {}
    virtual void FixObject(BaseObject&) const {}
    virtual void RunGarbageCollection(uint64_t, GCReason) = 0;
    virtual bool ShouldIgnoreRequest(GCRequest& quest) = 0;
    virtual BaseObject* FindToVersion(BaseObject* obj) const = 0;
    
    // CopyGC 专属方法（MS 模式需要 override 为空操作）
    virtual BaseObject* ForwardObject(BaseObject*) { std::abort(); }
    virtual bool TryUpdateRefField(...) const { std::abort(); }
    virtual bool TryForwardRefField(...) const { std::abort(); }
    virtual bool TryUntagRefField(...) const { std::abort(); }
    virtual bool TryTagRefField(...) const { std::abort(); }
    virtual RefField<> GetAndTryTagRefField(BaseObject*) const { std::abort(); }
    virtual bool IsOldPointer(RefField<>&) const { std::abort(); }
    virtual bool IsCurrentPointer(RefField<>&) const { std::abort(); }
    
protected:
    CollectorType collectorType = CollectorType::NO_COLLECTOR;
    std::atomic<GCPhase> gcPhase = { GCPhase::GC_PHASE_IDLE };
};
```

**CollectorType 枚举**：
```cpp
enum CollectorType {
    NO_COLLECTOR = 0,
    PROXY_COLLECTOR,   // CollectorProxy
    COPY_COLLECTOR,    // CopyCollector
    SMOOTH_COLLECTOR,  // WCollector
    COLLECTOR_TYPE_COUNT,
};
```

**关键发现**：Collector 基类中有大量 CopyGC 专属方法（`ForwardObject`、`TryUpdateRefField`、`IsOldPointer` 等），默认实现为 `std::abort()`。MarkSweepCollector 必须 override 这些方法为安全的空操作。

### 2.3 TracingCollector（TracingCollector.h）

中间抽象类，提供**对两种 GC 策略都通用的标记能力**：

| 能力类别 | 关键方法 | MS 可复用 |
|----------|----------|-----------|
| **标记** | `MarkObject(BaseObject*)` — bitmap 标记 | ✅ 直接复用 |
| **根枚举** | `EnumAllCommonRoots()` / `EnumAllExportRoots()` | ✅ 直接复用 |
| **追踪** | `DoEnumeration()` / `DoTracing()` / `TracingImpl()` | ✅ 直接复用 |
| **Phase 转换** | `TransitionToGCPhase()` → MutatorManager | ✅ 直接复用 |
| **SATB 处理** | `MarkSatbBuffer()` | 🔄 MS 改为 WBBuf flush |
| **终结器** | `ProcessFinalizers()` / `DoResurrection()` | ✅ 直接复用 |
| **GC 统计** | `UpdateGCStats()` / `GCStats` | ✅ 直接复用 |
| **工作栈** | `WorkStack` / `RootSet` / `PrefetchQueue` | ✅ 直接复用 |
| **并发标记** | `AddConcurrentTracingWork()` | ✅ 直接复用 |
| **对象复活** | `ResurrectObject()` | ✅ 直接复用 |
| **Forward 专属** | `GetCurrentTagID()` / `EnumAndTagRawRoot()` | ❌ MS 不使用 |

关键成员：
```cpp
class TracingCollector : public Collector {
protected:
    Allocator& theAllocator;                    // 分配器引用
    CollectorResources& collectorResources;     // GC 线程/线程池/任务队列
    GCReason gcReason = GC_REASON_USER;         // 当前 GC 原因
    bool fixReferences = false;                 // 是否修正引用（CopyGC 用）
    std::atomic<size_t> markedObjectCount = {0};// 标记计数
};
```

### 2.4 CopyCollector（CopyCollector.h）

CopyGC 专属层——MarkSweepCollector **不继承此类**：

```cpp
class CopyCollector : public TracingCollector {
public:
    void RunGarbageCollection(uint64_t gcIndex, GCReason reason) override;
    void CopyObject(const BaseObject& fromObj, BaseObject& toObj, size_t size) const;
protected:
    virtual BaseObject* ForwardObjectExclusive(BaseObject* obj) = 0;
    virtual void ForwardFromSpace();            // 并行转发
    virtual void RefineFromSpace();             // 精炼 from-space
    virtual void DoGarbageCollection() = 0;     // 子类实现 GC 主循环
};
```

### 2.5 WCollector（WCollector.h）

最终具体实现，Regional-Copying GC（"Smooth Collector"）：

| 核心子过程 | 说明 | MS 对应 |
|-----------|------|---------|
| `DoGarbageCollection()` | GC 主循环 | 替换为 MS 主循环 |
| `TraceHeap()` | 并发标记 | 复用 TracingCollector |
| `PostTrace()` | 标记后处理 | MS 无此阶段 |
| `Preforward()` | 预转发 | MS 无此阶段 |
| `ForwardFromSpace()` | 并行对象复制 | MS 替换为 Sweep |
| `CollectSmallSpace()` | 回收 Region | MS 替换为并发 Sweep |
| `CollectLargeGarbage()` | 大对象回收 | MS 改为 mark-sweep 清扫 |

持有的关键状态：
```cpp
class WCollector : public CopyCollector {
    ForwardTable forwardTable;     // 转发表（MS 不使用）
    uint16_t currentTagID = 0;     // tag ID 翻转（MS 不使用）
    // 6 个 Barrier 实现在 WCollector/ 目录下，通过 HeapImpl 持有
};
```

### 2.6 CollectorProxy（CollectorProxy.h/cpp）

代理模式——所有 Heap 操作通过它分派到具体 Collector：

```cpp
class CollectorProxy : public Collector {
public:
    explicit CollectorProxy(Allocator& allocator, CollectorResources& resources)
        : wCollector(allocator, resources)                   // ← 硬编码 WCollector
    { collectorType = CollectorType::PROXY_COLLECTOR; }
    
    void Init() override {
        wCollector.Init();
        if (currentCollector == nullptr) {
            currentCollector = &wCollector;                   // ← 硬编码选择
        }
    }
    
    void RunGarbageCollection(uint64_t gcIndex, GCReason reason) override {
        switch (reason) {
            case GC_REASON_HEU:
            case GC_REASON_BACKUP:
            case GC_REASON_OOM:
            case GC_REASON_FORCE:
            default:
                currentCollector = &wCollector;               // ← 全部指向 wCollector
                break;
        }
        currentCollector->SetGcStarted(true);
        currentCollector->RunGarbageCollection(gcIndex, reason);
    }

private:
    TracingCollector* currentCollector = nullptr;
    WCollector wCollector;                                    // ← 具体实例
};
```

**关键发现**：`CollectorProxy` 已经具备**策略模式的雏形**——它持有 `TracingCollector*` 指针可以指向不同的 Collector 实例。只需将 `WCollector wCollector` 改为条件编译，即可支持 MarkSweepCollector。

### 2.7 继承体系改造方向

```
改造后的继承体系：

Collector (抽象基类)                        ← 不变
  ├── CollectorProxy                        ← 条件编译持有不同 Collector
  │
  └── TracingCollector                      ← 不变（通用标记能力）
        ├── CopyCollector                   ← CopyGC 专属（#ifdef MRT_USE_COPYGC）
        │     └── WCollector                ← CopyGC 具体实现
        │
        └── MarkSweepCollector              ← 新增（#ifdef MRT_USE_MARK_SWEEP）
              (直接继承 TracingCollector，跳过 CopyCollector)
```

MarkSweepCollector 直接继承 TracingCollector 的理由：
1. TracingCollector 提供了所有通用的标记/枚举/追踪能力
2. CopyCollector 层的 `ForwardFromSpace`/`CopyObject` 对 MS 无意义
3. 直接继承避免带入不需要的 Forward 逻辑

---

## 3. 现有 Heap 初始化与 Barrier 安装深度分析

### 3.1 HeapImpl 类结构

`HeapImpl` 是 `Heap` 抽象类的唯一具体实现，以全局静态单例存在：

```cpp
// Heap.cpp L54-156
class HeapImpl : public Heap {
public:
    HeapImpl()
        : theSpace(Allocator::NewAllocator()),          // RegionSpace*
          collectorResources(collectorProxy),
          collectorProxy(*theSpace, collectorResources), // CollectorProxy
          stwBarrier(collectorProxy),                    // STW 通用屏障
          idleBarrier(collectorProxy),                   // 以下 6 个 WCollector 专属
          enumBarrier(collectorProxy),
          traceBarrier(collectorProxy),
          postTraceBarrier(collectorProxy),
          preforwardBarrier(collectorProxy),
          forwardBarrier(collectorProxy)
    {
        currentBarrier = &stwBarrier;
        stwBarrierPtr = &stwBarrier;
        Heap::currentBarrierPtr = &currentBarrier;       // 供 Heap::GetBarrier() 使用
    }

private:
    Allocator* theSpace;                        // 分配器（RegionSpace）
    CollectorResources collectorResources;      // GC 线程池/任务队列
    CollectorProxy collectorProxy;              // 收集器代理
    
    ExportRootTable exportRootsTable;
    
    // ─── 7 个 Barrier 实例（全部硬编码为 WCollector 专属） ───
    Barrier stwBarrier;                         // STW 通用
    IdleBarrier idleBarrier;                    // GC 空闲
    EnumBarrier enumBarrier;                    // 枚举阶段
    TraceBarrier traceBarrier;                  // 追踪阶段（SATB）
    PostTraceBarrier postTraceBarrier;          // 追踪后
    PreforwardBarrier preforwardBarrier;        // 预转发
    ForwardBarrier forwardBarrier;              // 转发阶段
    
    Barrier* currentBarrier = nullptr;          // 当前活跃屏障
    StaticRootTable staticRootTable;
    std::atomic<bool> isGCEnabled = { true };
};

static ImmortalWrapper<HeapImpl> g_heapInstance;  // 全局单例
```

### 3.2 Barrier 二级指针机制

`Heap` 基类通过**二级指针**实现快速 Barrier 访问：

```cpp
// Heap.h L31-34
class Heap {
    static Barrier& GetBarrier() { return **currentBarrierPtr; }
    static bool UseBarrier() { return *currentBarrierPtr != stwBarrierPtr; }
    
    static Barrier** currentBarrierPtr;  // 指向 HeapImpl::currentBarrier 的地址
    static Barrier* stwBarrierPtr;       // STW barrier 的直接指针
};
```

调用链：
```
CompilerCalls 函数
  → Heap::GetBarrier()                    // 解引用二级指针，O(1)
    → **currentBarrierPtr                 // = HeapImpl::currentBarrier
      → 具体 Barrier 实例的虚函数
```

**优势**：切换 Barrier 只需修改 `HeapImpl::currentBarrier` 指针（单指针原子写），所有 mutator 下次调用自动获取新 Barrier。

### 3.3 InstallBarrier 硬编码映射

```cpp
// Heap.cpp L189-205
void HeapImpl::InstallBarrier(const GCPhase phase)
{
    if (phase == GCPhase::GC_PHASE_ENUM) {
        currentBarrier = &enumBarrier;
    } else if (phase == GCPhase::GC_PHASE_TRACE || phase == GCPhase::GC_PHASE_CLEAR_SATB_BUFFER) {
        currentBarrier = &traceBarrier;
    } else if (phase == GCPhase::GC_PHASE_PREFORWARD) {
        currentBarrier = &preforwardBarrier;
    } else if (phase == GCPhase::GC_PHASE_FORWARD) {
        currentBarrier = &forwardBarrier;
    } else if (phase == GCPhase::GC_PHASE_IDLE) {
        currentBarrier = &idleBarrier;
    } else if (phase == GCPhase::GC_PHASE_POST_TRACE) {
        currentBarrier = &postTraceBarrier;
    }
}
```

**问题**：每种 GC 策略需要不同的 Phase→Barrier 映射。当前硬编码无法扩展。

### 3.4 Heap 初始化流程

```cpp
// Heap.cpp L167-174
void HeapImpl::Init(const HeapParam& param) {
    theSpace->Init(param);                   // 1. RegionSpace 初始化
    Heap::GetHeap().EnableGC(InitEnabledGCParam());  // 2. 启用 GC
    collectorProxy.Init();                   // 3. → wCollector.Init()
    collectorResources.Init();               // 4. 启动 GC 主线程
}
```

### 3.5 Barrier→Collector 耦合分析

所有 WCollector 目录下的 Barrier 实现都持有对 `Collector&` 的引用，并在内部向下转型：

```cpp
// 以 TraceBarrier.cpp 为例
void TraceBarrier::WriteReference(BaseObject* obj, RefField<>& field, BaseObject* ref) const {
    WCollector& collector = static_cast<WCollector&>(theCollector);  // 向下转型
    // 调用 collector 的 IsOldPointer、TryUpdateRefField 等 WCollector 专属方法
    ...
}
```

这意味着 WCollector 的 Barrier **无法**用于 MarkSweepCollector。MS 模式必须有自己的 Barrier 实现。

### 3.6 HeapImpl 改造核心需求

| 改造点 | 当前 | 目标 |
|--------|------|------|
| Barrier 实例 | 7 个 WCollector 专属 | 条件编译：CopyGC 保持 7 个 / MS 模式 2 个 |
| CollectorProxy 成员 | `WCollector wCollector` | 条件编译：CopyGC 持有 WCollector / MS 持有 MarkSweepCollector |
| InstallBarrier | 硬编码 if-else | 条件编译：CopyGC 保持 / MS 使用新映射 |
| Heap 接口层 | 虚函数 | 不变（良好抽象） |

---

## 4. 编译宏与构建系统改造

### 4.1 设计原则

1. **编译期切换**：通过 `MRT_USE_MARK_SWEEP` vs `MRT_USE_COPYGC` 互斥宏，在编译时选择 GC 策略
2. **向后兼容**：不带新参数时，默认行为与改造前完全一致（CopyGC）
3. **互斥保护**：两个宏不能同时为 1，CMake 层面做 FATAL_ERROR 检查
4. **最小侵入**：尽量不修改现有 if/else 逻辑，用 `#ifdef` 分支隔离新代码

### 4.2 config.cmake 改造

```cmake
# ═══ 新增内容（在 COPYGC_FLAG 相邻位置） ═══

# Mark-Sweep GC 开关（与 COPYGC_FLAG 互斥）
if (NOT DEFINED MARKSWEEP_FLAG)
    set(MARKSWEEP_FLAG 0 CACHE STRING "marksweep default is false" FORCE)
endif()
message(STATUS "MARKSWEEP_FLAG : ${MARKSWEEP_FLAG}")

# 互斥校验
if (COPYGC_FLAG MATCHES 1 AND MARKSWEEP_FLAG MATCHES 1)
    message(FATAL_ERROR 
        "COPYGC_FLAG and MARKSWEEP_FLAG are mutually exclusive. "
        "Please set only one of them to 1.")
endif()

# 至少选一个
if (NOT COPYGC_FLAG MATCHES 1 AND NOT MARKSWEEP_FLAG MATCHES 1)
    message(FATAL_ERROR 
        "Either COPYGC_FLAG or MARKSWEEP_FLAG must be set to 1.")
endif()
```

### 4.3 CMakeLists.txt 改造

```cmake
# ═══ 替换现有 COPYGC_FLAG 消费逻辑（L208-L210） ═══

if (COPYGC_FLAG MATCHES 1)
    add_definitions(-DMRT_USE_COPYGC)
    message(STATUS "GC Mode: CopyGC (Regional-Copying)")
elseif (MARKSWEEP_FLAG MATCHES 1)
    add_definitions(-DMRT_USE_MARK_SWEEP)
    message(STATUS "GC Mode: MarkSweep (Go-style)")
endif()
```

### 4.4 build.py 改造

```python
# ═══ 新增命令行参数（在 b.add_argument 区域） ═══

b.add_argument(
    "--gc-mode",
    choices=["copygc", "marksweep"],
    default="copygc",
    help="Select GC algorithm: copygc (default, Regional-Copying) or marksweep (Go-style)"
)
```

CMake 调用处改造（以 native 目标为例）：

```python
# ═══ 替换硬编码的 -DCOPYGC_FLAG=1 ═══

gc_flags = {
    "copygc":    ["-DCOPYGC_FLAG=1", "-DMARKSWEEP_FLAG=0"],
    "marksweep": ["-DCOPYGC_FLAG=0", "-DMARKSWEEP_FLAG=1"],
}

cmake_command = [
    "cmake",
    "-DCMAKE_INSTALL_PREFIX={}_{}".format(install_prefix, target_arch),
    "-DCMAKE_BUILD_TYPE={}".format(mode),
    *gc_flags[args.gc_mode],         # ← 替换原来的 "-DCOPYGC_FLAG=1"
    "-DDOPRA_FLAG=1",
    # ... 其余参数不变 ...
]
```

需要在以下 5 处替换（build.py 中每个平台目标的 cmake_command）：
- L118（native）
- L155（windows-x86_64）
- L205（ohos 系列）
- L240（android 系列）
- L281（ios 系列）

### 4.5 构建命令示例

```bash
# 默认构建（CopyGC，与改造前一致）
python3 build.py build --target native --build-type release -v 0.0.1

# 构建 Mark-Sweep GC 版本
python3 build.py build --target native --build-type release -v 0.0.1 --gc-mode marksweep

# 构建 Mark-Sweep 调试版本
python3 build.py build --target native --build-type debug -v 0.0.1 --gc-mode marksweep
```

### 4.6 宏使用规范

| 场景 | 用法 | 示例 |
|------|------|------|
| 仅 CopyGC 代码 | `#ifdef MRT_USE_COPYGC` | Forward 相关逻辑 |
| 仅 MS 代码 | `#ifdef MRT_USE_MARK_SWEEP` | ConcurrentSweeper |
| 两者不同实现 | `#ifdef MRT_USE_MARK_SWEEP ... #else ... #endif` | InstallBarrier |
| 通用代码 | 不加宏 | MarkObject、根枚举 |

**编码规范**：
- `#ifdef` 块中不要嵌套过深，超过 2 层时应抽取为独立函数
- 每个 `#ifdef` 块必须有对应的注释说明用途
- MS Barrier 源文件统一放在 `Barrier/` 目录下，通过 CMakeLists.txt 条件编译控制

---

## 5. GCPhase 扩展设计

### 5.1 现有 GCPhase 枚举

```cpp
// Collector.h L24-37
enum GCPhase : uint8_t {
    GC_PHASE_UNDEF             = 0,
    GC_PHASE_IDLE              = 1,    // 空闲
    GC_PHASE_FINISH            = 2,    // GC 完成
    GC_PHASE_RECLAIM_SATB_NODE = 3,    // 回收 SATB 节点
    GC_PHASE_INIT              = 8,    // GC 初始化
    // ↓ 以下阶段需要 barrier（enum value > GC_PHASE_INIT）
    GC_PHASE_ENUM              = 9,    // 根枚举（CopyGC）
    GC_PHASE_TRACE             = 10,   // 并发标记（CopyGC）
    GC_PHASE_CLEAR_SATB_BUFFER = 11,   // 清除 SATB 缓冲（CopyGC）
    GC_PHASE_POST_TRACE        = 12,   // 追踪后处理（CopyGC）
    GC_PHASE_PREFORWARD        = 13,   // 预转发（CopyGC）
    GC_PHASE_FORWARD           = 14,   // 并发转发（CopyGC）
};
```

**设计约束**：
- `GC_PHASE_IDLE` (1) 和 `GC_PHASE_INIT` (8) 的语义对两种 GC 通用
- barrier 安装的判断条件是 `phase > GC_PHASE_INIT`，MS 的 phase 值也需要 > 8
- 枚举值不能与现有值冲突

### 5.2 扩展方案：追加 MS 专属 Phase

```cpp
// Collector.h — 扩展后的 GCPhase 枚举
enum GCPhase : uint8_t {
    // ═══ 通用 Phase ═══
    GC_PHASE_UNDEF             = 0,
    GC_PHASE_IDLE              = 1,
    GC_PHASE_FINISH            = 2,
    GC_PHASE_RECLAIM_SATB_NODE = 3,
    GC_PHASE_INIT              = 8,
    
    // ═══ CopyGC 专属 Phase（不变） ═══
    GC_PHASE_ENUM              = 9,
    GC_PHASE_TRACE             = 10,
    GC_PHASE_CLEAR_SATB_BUFFER = 11,
    GC_PHASE_POST_TRACE        = 12,
    GC_PHASE_PREFORWARD        = 13,
    GC_PHASE_FORWARD           = 14,
    
    // ═══ Mark-Sweep 专属 Phase（新增，从 20 开始避免冲突） ═══
    GC_PHASE_MS_MARK_SETUP       = 20,  // STW: 开启写屏障，扫描根
    GC_PHASE_MS_CONCURRENT_MARK  = 21,  // 并发标记
    GC_PHASE_MS_MARK_TERMINATION = 22,  // STW: 完成标记，关闭写屏障
    GC_PHASE_MS_SWEEP            = 23,  // 并发清扫（barrier 已关闭）
};
```

### 5.3 Phase 语义对比

| Go GC Phase | 对应 MS Phase | STW? | Barrier |
|-------------|---------------|------|---------|
| `_GCoff` | `GC_PHASE_IDLE` (1) | - | MSIdleBarrier |
| `_GCmark` (setup) | `GC_PHASE_MS_MARK_SETUP` (20) | STW | MSWriteBarrier |
| `_GCmark` (concurrent) | `GC_PHASE_MS_CONCURRENT_MARK` (21) | 并发 | MSWriteBarrier |
| `_GCmarktermination` | `GC_PHASE_MS_MARK_TERMINATION` (22) | STW | MSWriteBarrier |
| sweep (background) | `GC_PHASE_MS_SWEEP` (23) | 并发 | MSIdleBarrier |

### 5.4 Phase 状态机

```
CopyGC 状态机（现有，不变）：
  IDLE → INIT → ENUM → TRACE → CLEAR_SATB → POST_TRACE → PREFORWARD → FORWARD → IDLE

Mark-Sweep 状态机（新增）：
  IDLE → INIT → MS_MARK_SETUP(STW) → MS_CONCURRENT_MARK → MS_MARK_TERMINATION(STW) → MS_SWEEP → IDLE
```

### 5.5 CollectorType 扩展

```cpp
// Collector.h
enum CollectorType {
    NO_COLLECTOR = 0,
    PROXY_COLLECTOR,
    COPY_COLLECTOR,
    SMOOTH_COLLECTOR,
    MARK_SWEEP_COLLECTOR,      // ← 新增
    COLLECTOR_TYPE_COUNT,
};
```

### 5.6 GetGCPhaseName 扩展

```cpp
// Collector.cpp 中的 GetGCPhaseName 需要补充新 phase 的名称
const char* Collector::GetGCPhaseName(GCPhase phase) {
    switch (phase) {
        // ... 现有 case 保持 ...
        case GC_PHASE_MS_MARK_SETUP:       return "MS_MARK_SETUP";
        case GC_PHASE_MS_CONCURRENT_MARK:  return "MS_CONCURRENT_MARK";
        case GC_PHASE_MS_MARK_TERMINATION: return "MS_MARK_TERMINATION";
        case GC_PHASE_MS_SWEEP:            return "MS_SWEEP";
        default:                           return "UNKNOWN";
    }
}
```

---

## 6. MarkSweepCollector 骨架设计

### 6.1 设计目标

Step 0 阶段仅创建**骨架代码**——类定义、空的虚函数实现、正确的编译链接。GC 主循环的具体实现在后续 Step 中逐步填充。

### 6.2 继承关系

```
TracingCollector (现有，提供通用标记能力)
  └── MarkSweepCollector (新增)
        - 不经过 CopyCollector
        - 直接复用 TracingCollector 的 MarkObject、EnumRoots、DoTracing 等
        - 新增 ConcurrentSweep 等方法占位
```

### 6.3 头文件设计

```cpp
// 新建文件: src/Heap/Collector/MarkSweepCollector.h
#ifndef MRT_MARK_SWEEP_COLLECTOR_H
#define MRT_MARK_SWEEP_COLLECTOR_H

#ifdef MRT_USE_MARK_SWEEP

#include "TracingCollector.h"

namespace MapleRuntime {

class MarkSweepCollector : public TracingCollector {
public:
    explicit MarkSweepCollector(Allocator& allocator, CollectorResources& resources)
        : TracingCollector(allocator, resources)
    {
        collectorType = CollectorType::MARK_SWEEP_COLLECTOR;
    }

    ~MarkSweepCollector() override = default;

    // ═══ 生命周期 ═══
    void Init() override;
    void Fini() override;

    // ═══ GC 主循环 ═══
    void RunGarbageCollection(uint64_t gcIndex, GCReason reason) override;

    // ═══ CopyGC 专属方法 override 为安全空操作 ═══
    BaseObject* FindToVersion(BaseObject* obj) const override { return nullptr; }
    BaseObject* ForwardObject(BaseObject*) override { return nullptr; }
    bool TryUpdateRefField(BaseObject*, RefField<>&, BaseObject*&) const override { return false; }
    bool TryForwardRefField(BaseObject*, RefField<>&, BaseObject*&) const override { return false; }
    bool TryUntagRefField(BaseObject*, RefField<>&, BaseObject*&) const override { return false; }
    bool TryTagRefField(BaseObject*, RefField<>&, BaseObject*) const override { return false; }
    RefField<> GetAndTryTagRefField(BaseObject*) const override
    {
        RefField<> empty;
        return empty;
    }
    bool IsOldPointer(RefField<>&) const override { return false; }
    bool IsCurrentPointer(RefField<>&) const override { return false; }
    bool IsFromObject(BaseObject*) const override { return false; }
    bool IsGhostFromObject(BaseObject*) const override { return false; }
    bool IsUnmovableFromObject(BaseObject*) const override { return false; }

    // ═══ MS 专属方法（骨架，后续 Step 填充） ═══
    
    // Step 2 填充: 三色标记
    void MarkSetup();               // STW: 开启写屏障，扫描根
    void ConcurrentMark();          // 并发标记
    void MarkTermination();         // STW: 完成标记
    
    // Step 4 填充: 并发清扫
    void ConcurrentSweep();         // 并发清扫
    
    // Step 5 填充: GC Pacer
    void UpdateGCTrigger();         // 动态调整 GC 触发阈值

    // Step 3 填充: 写屏障相关
    void FlushAllWriteBarrierBuffers();  // 刷新所有 mutator 的 WBBuf

    // ═══ TracingCollector 虚方法 override ═══
    void TraceObjectRefFields(BaseObject* obj, WorkStack& workStack) override;
    void EnumRefFieldRoot(RefField<>& ref, RootSet& rootSet) const override;
    BaseObject* GetAndTryTagObj(BaseObject* obj, RefField<>& field) override;

private:
    // GC 主循环实现（Step 2 填充）
    void DoGarbageCollection();

    // 当前 MS GC phase
    MSGCPhase msPhase_ = MSGCPhase::OFF;
};

// MS GC Phase 定义（细粒度，内部使用）
enum class MSGCPhase : uint8_t {
    OFF = 0,              // 等价于 GC_PHASE_IDLE
    MARK_SETUP,           // STW: 安装写屏障，扫描根
    CONCURRENT_MARK,      // 并发标记
    MARK_TERMINATION,     // STW: 完成标记
    SWEEP,                // 并发清扫
};

} // namespace MapleRuntime

#endif // MRT_USE_MARK_SWEEP
#endif // MRT_MARK_SWEEP_COLLECTOR_H
```

### 6.4 实现文件骨架

```cpp
// 新建文件: src/Heap/Collector/MarkSweepCollector.cpp
#ifdef MRT_USE_MARK_SWEEP

#include "MarkSweepCollector.h"
#include "Base/Log.h"

namespace MapleRuntime {

void MarkSweepCollector::Init()
{
    VLOG(GC, "MarkSweepCollector::Init()");
    // Step 0: 初始化骨架
    // - 后续 Step 中将初始化 ConcurrentSweeper、GCPacer 等组件
}

void MarkSweepCollector::Fini()
{
    VLOG(GC, "MarkSweepCollector::Fini()");
}

void MarkSweepCollector::RunGarbageCollection(uint64_t gcIndex, GCReason reason)
{
    VLOG(GC, "MarkSweepCollector::RunGarbageCollection(gcIndex=%llu, reason=%d)",
         gcIndex, static_cast<int>(reason));
    
    SetGCReason(reason);
    PreGarbageCollection(/* isConcurrent */ true);
    
    DoGarbageCollection();
    
    PostGarbageCollection(gcIndex);
}

void MarkSweepCollector::DoGarbageCollection()
{
    // ═══ Step 0 骨架：记录 GC 流程，不执行实际回收 ═══
    // 后续 Step 逐步填充各阶段实现
    
    // Phase 1: Mark Setup (STW)
    // MarkSetup();
    
    // Phase 2: Concurrent Mark
    // ConcurrentMark();
    
    // Phase 3: Mark Termination (STW)
    // MarkTermination();
    
    // Phase 4: Concurrent Sweep
    // ConcurrentSweep();
    
    VLOG(GC, "MarkSweepCollector::DoGarbageCollection() — skeleton, no actual collection");
}

// ═══ MS 专属方法骨架（后续 Step 填充） ═══

void MarkSweepCollector::MarkSetup()
{
    // Step 2 填充
    VLOG(GC, "MarkSweepCollector::MarkSetup() — TODO");
}

void MarkSweepCollector::ConcurrentMark()
{
    // Step 2 填充
    VLOG(GC, "MarkSweepCollector::ConcurrentMark() — TODO");
}

void MarkSweepCollector::MarkTermination()
{
    // Step 2 填充
    VLOG(GC, "MarkSweepCollector::MarkTermination() — TODO");
}

void MarkSweepCollector::ConcurrentSweep()
{
    // Step 4 填充
    VLOG(GC, "MarkSweepCollector::ConcurrentSweep() — TODO");
}

void MarkSweepCollector::UpdateGCTrigger()
{
    // Step 5 填充
}

void MarkSweepCollector::FlushAllWriteBarrierBuffers()
{
    // Step 3 填充
}

// ═══ TracingCollector 虚方法 override ═══

void MarkSweepCollector::TraceObjectRefFields(BaseObject* obj, WorkStack& workStack)
{
    // Step 2 填充: 遍历对象引用字段，将未标记的入栈
    // 骨架阶段复用 WCollector 的逻辑模式，但不做 tag 处理
    VLOG(GC, "TraceObjectRefFields — TODO");
}

void MarkSweepCollector::EnumRefFieldRoot(RefField<>& ref, RootSet& rootSet) const
{
    // MS 模式：直接读取引用值（无 tag），加入根集
    BaseObject* target = ref.GetTargetObject();
    if (target != nullptr) {
        rootSet.PushBack(target);
    }
}

BaseObject* MarkSweepCollector::GetAndTryTagObj(BaseObject* obj, RefField<>& field)
{
    // MS 模式：不做 tag，直接返回对象
    return obj;
}

} // namespace MapleRuntime

#endif // MRT_USE_MARK_SWEEP
```

### 6.5 骨架代码的设计考量

| 决策 | 理由 |
|------|------|
| 继承 TracingCollector 而非 CopyCollector | TracingCollector 提供通用标记能力；CopyCollector 的 Forward 逻辑对 MS 无意义 |
| CopyGC 专属方法 override 为空操作 | Collector 基类默认实现为 `abort()`，不 override 会崩溃 |
| `FindToVersion` 返回 `nullptr` | MS 模式无转发表，对象永远在原地，`FindLatestVersion` 检查 null 后返回原对象 |
| `IsOldPointer` 返回 `false` | MS 模式无 tagged pointer，指针永远不是"旧指针" |
| `DoGarbageCollection` 全部注释 | Step 0 仅建骨架，不执行实际回收；确保切换到 MS 时运行时不崩溃 |
| `EnumRefFieldRoot` 直接读引用 | MS 模式无 tag，直接 `GetTargetObject()` 即可获取真实地址 |

---

## 7. CollectorProxy 适配改造

### 7.1 改造目标

将 `CollectorProxy` 从硬编码持有 `WCollector` 改为条件编译持有不同的 Collector 实例。

### 7.2 CollectorProxy.h 改造

```cpp
// src/Heap/Collector/CollectorProxy.h — 改造后
#ifndef MRT_COLLECTOR_PROXY_H
#define MRT_COLLECTOR_PROXY_H

#include "Base/Macros.h"
#include "Collector.h"
#include "CollectorResources.h"

#ifdef MRT_USE_MARK_SWEEP
#include "MarkSweepCollector.h"
#else
#include "WCollector/WCollector.h"
#endif

namespace MapleRuntime {

class CollectorProxy : public Collector {
public:
    explicit CollectorProxy(Allocator& allocator, CollectorResources& resources)
#ifdef MRT_USE_MARK_SWEEP
        : msCollector(allocator, resources)
#else
        : wCollector(allocator, resources)
#endif
    {
        collectorType = CollectorType::PROXY_COLLECTOR;
    }

    ~CollectorProxy() override = default;

    void Init() override;
    void Fini() override;

    GCPhase GetGCPhase() const override { return currentCollector->GetGCPhase(); }
    void SetGCPhase(const GCPhase phase) override { currentCollector->SetGCPhase(phase); }

    MRT_EXPORT void RunGarbageCollection(uint64_t gcIndex, GCReason reason) override;
    bool ShouldIgnoreRequest(GCRequest& request) override { return currentCollector->ShouldIgnoreRequest(request); }

    TracingCollector& GetCurrentCollector() const { return *currentCollector; }

    // ═══ CopyGC 专属方法委托（MS 模式下由 MarkSweepCollector 的空操作处理） ═══
    BaseObject* FindToVersion(BaseObject* obj) const override { return currentCollector->FindToVersion(obj); }
    bool IsOldPointer(RefField<>& ref) const override { return currentCollector->IsOldPointer(ref); }
    bool IsCurrentPointer(RefField<>& ref) const override { return currentCollector->IsCurrentPointer(ref); }
    bool IsFromObject(BaseObject* obj) const override { return currentCollector->IsFromObject(obj); }
    bool IsGhostFromObject(BaseObject* obj) const override { return currentCollector->IsGhostFromObject(obj); }
    bool IsUnmovableFromObject(BaseObject* obj) const override
    {
        return currentCollector->IsUnmovableFromObject(obj);
    }

    void AddRawPointerObject(BaseObject* obj) override { return currentCollector->AddRawPointerObject(obj); }
    void RemoveRawPointerObject(BaseObject* obj) override { return currentCollector->RemoveRawPointerObject(obj); }

    BaseObject* ForwardObject(BaseObject* obj) override { return currentCollector->ForwardObject(obj); }

    bool TryUpdateRefField(BaseObject* obj, RefField<>& field, BaseObject*& toVersion) const override
    {
        return currentCollector->TryUpdateRefField(obj, field, toVersion);
    }

    bool TryForwardRefField(BaseObject* obj, RefField<>& field, BaseObject*& toVersion) const override
    {
        return currentCollector->TryForwardRefField(obj, field, toVersion);
    }

    bool TryUntagRefField(BaseObject* obj, RefField<>& field, BaseObject*& target) const override
    {
        return currentCollector->TryUntagRefField(obj, field, target);
    }

    RefField<> GetAndTryTagRefField(BaseObject* obj) const override
    {
        return currentCollector->GetAndTryTagRefField(obj);
    }

private:
    TracingCollector* currentCollector = nullptr;

#ifdef MRT_USE_MARK_SWEEP
    MarkSweepCollector msCollector;
#else
    WCollector wCollector;
#endif
};

} // namespace MapleRuntime
#endif // MRT_COLLECTOR_PROXY_H
```

### 7.3 CollectorProxy.cpp 改造

```cpp
// src/Heap/Collector/CollectorProxy.cpp — 改造后
#include "CollectorProxy.h"

namespace MapleRuntime {

void CollectorProxy::Init()
{
#ifdef MRT_USE_MARK_SWEEP
    msCollector.Init();
    if (currentCollector == nullptr) {
        currentCollector = &msCollector;
    }
#else
    wCollector.Init();
    if (currentCollector == nullptr) {
        currentCollector = &wCollector;
    }
#endif
}

void CollectorProxy::Fini()
{
#ifdef MRT_USE_MARK_SWEEP
    msCollector.Fini();
#else
    wCollector.Fini();
#endif
}

void CollectorProxy::RunGarbageCollection(uint64_t gcIndex, GCReason reason)
{
#ifdef MRT_USE_MARK_SWEEP
    // MS 模式：所有 GC 原因使用同一收集器
    currentCollector = &msCollector;
#else
    // CopyGC 模式：保持现有逻辑不变
    switch (reason) {
        case GC_REASON_HEU:
        case GC_REASON_BACKUP:
        case GC_REASON_OOM:
        case GC_REASON_FORCE:
        default:
            currentCollector = &wCollector;
            break;
    }
#endif
    currentCollector->SetGcStarted(true);
    currentCollector->RunGarbageCollection(gcIndex, reason);
}

} // namespace MapleRuntime
```

### 7.4 改造要点

| 改造点 | 原始代码 | 改造后 |
|--------|----------|--------|
| 成员变量 | `WCollector wCollector` | `#ifdef` 选择 MS 或 W |
| `#include` | 总是 include WCollector.h | 条件 include |
| `Init()` | 总是 `wCollector.Init()` | 条件选择 |
| `RunGarbageCollection()` | switch 全指向 wCollector | MS 模式直接指向 msCollector |
| `Fini()` | `wCollector.Fini()` | 条件选择 |

### 7.5 向后兼容保证

- **不定义 `MRT_USE_MARK_SWEEP` 时**：`#else` 分支生效，代码与改造前完全一致
- **`currentCollector` 类型不变**：仍然是 `TracingCollector*`，MarkSweepCollector 继承自 TracingCollector
- **所有委托方法不变**：通过 `currentCollector->xxx()` 虚函数分派，不感知具体类型

---

## 8. HeapImpl 条件编译改造

### 8.1 改造思路

HeapImpl 是最大耦合点——构造函数中硬编码了 7 个 WCollector 专属 Barrier 实例。改造策略：

1. **Barrier 成员**：条件编译选择不同的 Barrier 集合
2. **InstallBarrier**：条件编译选择不同的 Phase→Barrier 映射
3. **其余成员**：Allocator、CollectorResources、CollectorProxy、StaticRootTable 等保持不变

### 8.2 HeapImpl 构造函数改造

```cpp
// Heap.cpp — HeapImpl 构造函数改造
class HeapImpl : public Heap {
public:
    HeapImpl()
        : theSpace(Allocator::NewAllocator()),
          collectorResources(collectorProxy),
          collectorProxy(*theSpace, collectorResources),
          stwBarrier(collectorProxy),
#ifdef MRT_USE_MARK_SWEEP
          // MS 模式：仅 2 个 Barrier（Step 0 先用基类 Barrier 占位）
          msIdleBarrier(collectorProxy),
          msWriteBarrier(collectorProxy)
#else
          // CopyGC 模式：6 个 Barrier（不变）
          idleBarrier(collectorProxy),
          enumBarrier(collectorProxy),
          traceBarrier(collectorProxy),
          postTraceBarrier(collectorProxy),
          preforwardBarrier(collectorProxy),
          forwardBarrier(collectorProxy)
#endif
    {
        currentBarrier = &stwBarrier;
        stwBarrierPtr = &stwBarrier;
        Heap::currentBarrierPtr = &currentBarrier;
        RunType::InitRunTypeMap();
    }

    // ... 其余方法不变 ...

private:
    Allocator* theSpace;
    CollectorResources collectorResources;
    CollectorProxy collectorProxy;
    ExportRootTable exportRootsTable;
    
    Barrier stwBarrier;                                // STW 通用（两种模式共享）

#ifdef MRT_USE_MARK_SWEEP
    // ─── MS 模式的 Barrier 实例 ───
    // Step 0 阶段：用基类 Barrier 占位
    // Step 3 阶段：替换为 MSIdleBarrier + MSWriteBarrier
    Barrier msIdleBarrier;                             // GC 空闲期
    Barrier msWriteBarrier;                            // GC Mark 期（Step 3 替换为 MSWriteBarrier）
#else
    // ─── CopyGC 模式的 Barrier 实例（不变） ───
    IdleBarrier idleBarrier;
    EnumBarrier enumBarrier;
    TraceBarrier traceBarrier;
    PostTraceBarrier postTraceBarrier;
    PreforwardBarrier preforwardBarrier;
    ForwardBarrier forwardBarrier;
#endif

    Barrier* currentBarrier = nullptr;
    StaticRootTable staticRootTable;
    std::atomic<bool> isGCEnabled = { true };
};
```

### 8.3 InstallBarrier 改造

```cpp
void HeapImpl::InstallBarrier(const GCPhase phase)
{
#ifdef MRT_USE_MARK_SWEEP
    // ═══ MS 模式：2 个 Barrier 的简化映射 ═══
    switch (phase) {
    case GCPhase::GC_PHASE_MS_MARK_SETUP:
    case GCPhase::GC_PHASE_MS_CONCURRENT_MARK:
    case GCPhase::GC_PHASE_MS_MARK_TERMINATION:
        currentBarrier = &msWriteBarrier;
        break;
    case GCPhase::GC_PHASE_IDLE:
    case GCPhase::GC_PHASE_MS_SWEEP:
    default:
        currentBarrier = &msIdleBarrier;
        break;
    }
#else
    // ═══ CopyGC 模式：6 个 Barrier 的映射（不变） ═══
    if (phase == GCPhase::GC_PHASE_ENUM) {
        currentBarrier = &enumBarrier;
    } else if (phase == GCPhase::GC_PHASE_TRACE || phase == GCPhase::GC_PHASE_CLEAR_SATB_BUFFER) {
        currentBarrier = &traceBarrier;
    } else if (phase == GCPhase::GC_PHASE_PREFORWARD) {
        currentBarrier = &preforwardBarrier;
    } else if (phase == GCPhase::GC_PHASE_FORWARD) {
        currentBarrier = &forwardBarrier;
    } else if (phase == GCPhase::GC_PHASE_IDLE) {
        currentBarrier = &idleBarrier;
    } else if (phase == GCPhase::GC_PHASE_POST_TRACE) {
        currentBarrier = &postTraceBarrier;
    }
#endif
    DLOG(GCPHASE, "install barrier for gc phase %u", phase);
}
```

### 8.4 Init/Fini 保持不变

```cpp
void HeapImpl::Init(const HeapParam& param)
{
    theSpace->Init(param);                       // 不变
    Heap::GetHeap().EnableGC(InitEnabledGCParam());  // 不变
    collectorProxy.Init();                       // 条件编译在 CollectorProxy 内部处理
    collectorResources.Init();                   // 不变
}

void HeapImpl::Fini()
{
    collectorResources.Fini();
    collectorProxy.Fini();                       // 条件编译在 CollectorProxy 内部处理
    if (theSpace != nullptr) {
        delete theSpace;
        theSpace = nullptr;
    }
}
```

### 8.5 Step 0 阶段的 Barrier 占位策略

Step 0 阶段，MS 模式的 `msIdleBarrier` 和 `msWriteBarrier` 使用 `Barrier` 基类占位：

| 基类 Barrier 的默认行为 | 效果 |
|--------------------------|------|
| `WriteReference()` | 直接 `field.SetTargetObject(ref)` |
| `ReadReference()` | 直接 `field.GetTargetObject()` |
| 其他方法 | 类似的直通实现 |

这意味着 Step 0 的 MS 模式下：
- 所有读写操作都是**直通**（无屏障逻辑）
- GC 不执行实际回收（骨架 DoGarbageCollection）
- 应用可以正常运行，只是不回收内存
- 这是预期行为，验证基础设施正确即可

### 8.6 其他需要条件编译的 HeapImpl 方法

```cpp
// IsSurvivedObject — 两种模式逻辑相同，不需要改造
bool IsSurvivedObject(const BaseObject* obj) const override
{
    return RegionSpace::IsMarkedObject(obj) || RegionSpace::IsResurrectedObject(obj);
}

// GetCollector — 通过 CollectorProxy 间接获取，不需要改造
Collector& HeapImpl::GetCollector() { return collectorProxy.GetCurrentCollector(); }

// Allocate — 通过 Allocator 虚函数，Step 0 不改造
MAddress HeapImpl::Allocate(size_t size, AllocType allocType) { return theSpace->Allocate(size, allocType); }
```

**结论**：HeapImpl 中仅构造函数和 `InstallBarrier` 需要条件编译，其余方法通过已有的虚函数/代理机制自动适配。

---

## 9. Barrier 基类与 MS Barrier 统一目录设计

### 9.1 Barrier 基类回顾

```cpp
// Barrier.h — 基类提供 20+ 个虚函数接口
class Barrier {
public:
    explicit Barrier(Collector& collector) : theCollector(collector) {}
    virtual ~Barrier() = default;

    // ─── 写操作 ───
    virtual void WriteReference(BaseObject* obj, RefField<false>& field, BaseObject* ref) const;
    virtual void WriteStaticRef(RefField<false>& field, BaseObject* ref) const;
    virtual void WriteStruct(BaseObject* obj, MAddress field, MAddress src, size_t size) const;
    virtual void WriteStaticStruct(MAddress field, MAddress src, size_t size) const;
    virtual void CopyRefArray(...) const;
    virtual void CopyStructArray(...) const;
    virtual void AtomicWriteReference(...) const;
    virtual void AtomicSwapReference(...) const;
    virtual void CompareAndSwapReference(...) const;
    virtual void WriteGeneric(...) const;

    // ─── 读操作 ───
    virtual BaseObject* ReadReference(BaseObject* obj, RefField<false>& field) const;
    virtual BaseObject* ReadStaticRef(RefField<false>& field) const;
    virtual BaseObject* ReadWeakRef(BaseObject* obj, RefField<false>& field) const;
    virtual void ReadStruct(...) const;
    virtual void ReadStaticStruct(...) const;
    virtual BaseObject* AtomicReadReference(...) const;
    virtual void ReadGeneric(...) const;

protected:
    Collector& theCollector;  // 持有对 Collector 的引用
};
```

**基类默认实现**：所有方法都是**直通操作**（直接读/写，不做额外处理）。这是 STW Barrier 和 Step 0 占位 Barrier 的行为。

### 9.2 Step 0 阶段的 MS Barrier 占位策略

Step 0 阶段，MS 模式使用 **Barrier 基类实例作为占位**：

```cpp
// HeapImpl 构造函数中
Barrier msIdleBarrier;    // 用基类 Barrier 占位
Barrier msMarkBarrier;    // 用基类 Barrier 占位
```

这样做的理由：
1. **Step 0 不需要 GC 逻辑**：骨架 MarkSweepCollector 不执行实际回收
2. **基类直通行为安全**：所有读写操作正常工作，只是不做标记
3. **后续 Step 3 再替换**：创建专门的 MSIdleBarrier 和 MSWriteBarrier

### 9.3 MS Barrier 统一到 Barrier 目录的设计决策

**设计决策**：MS Barrier 的源文件**不创建独立的 MSBarrier/ 子目录**，而是统一放在 `src/Heap/Barrier/` 目录下，通过 CMakeLists.txt 条件编译控制。

**理由**：
1. **Barrier 基类和子类天然同属一个模块**：MSIdleBarrier / MSWriteBarrier 继承自 Barrier，放在同目录更直观
2. **避免目录碎片化**：独立 MSBarrier/ 目录只有 2-3 个文件，不值得单独建目录
3. **与 WCollector 的区别**：WCollector/ 目录同时包含收集器实现（WCollector.h/cpp）和 6 个 Barrier，是完整的 CopyGC 模块；而 MS Barrier 仅是 Barrier 子类，不包含收集器逻辑
4. **CMake 链接简化**：不需要新增链接库名（MSBarrier），所有 Barrier 代码统一编入 `Barrier` 静态库

### 9.4 目录结构

```
src/Heap/
├── Barrier/                    # Barrier 基类 + MS Barrier（统一目录）
│   ├── Barrier.h               # Barrier 基类头文件
│   ├── Barrier.cpp             # Barrier 基类实现
│   ├── Barrier.inline.h        # Barrier 内联方法
│   ├── MSIdleBarrier.h         # Step 3 创建
│   ├── MSIdleBarrier.cpp       # Step 3 创建
│   ├── MSWriteBarrier.h        # Step 3 创建
│   ├── MSWriteBarrier.cpp      # Step 3 创建
│   ├── WriteBarrierBuffer.h    # Step 3 创建
│   └── CMakeLists.txt          # 条件编译 MS Barrier 源文件
├── WCollector/                 # CopyGC 模式：收集器 + 6 个 Barrier
│   ├── IdleBarrier.h/cpp
│   ├── EnumBarrier.h/cpp
│   ├── TraceBarrier.h/cpp
│   ├── PostTraceBarrier.h/cpp
│   ├── PreforwardBarrier.h/cpp
│   ├── ForwardBarrier.h/cpp
│   ├── WCollector.h/cpp
│   └── CMakeLists.txt
└── Collector/                  # 收集器（条件编译选择）
    ├── MarkSweepCollector.h    # Step 0 创建
    ├── MarkSweepCollector.cpp  # Step 0 创建
    ├── CopyCollector.h/cpp     # CopyGC 模式
    └── CMakeLists.txt          # 条件编译选择源文件
```

### 9.5 Barrier/CMakeLists.txt 条件编译

```cmake
# src/Heap/Barrier/CMakeLists.txt
set(SRC_LIST
    "Barrier.cpp"
)

# Mark-Sweep barrier sources (Step 3 will add MSIdleBarrier.cpp, MSWriteBarrier.cpp here)
if (MARKSWEEP_FLAG MATCHES 1)
    # list(APPEND SRC_LIST "MSIdleBarrier.cpp")
    # list(APPEND SRC_LIST "MSWriteBarrier.cpp")
endif()

include_directories(${CMAKE_CURRENT_SOURCE_DIR}/../)
add_library(Barrier STATIC ${SRC_LIST})
```

**说明**：Step 0 阶段注释掉 MS Barrier 源文件（尚未创建）。Step 3 取消注释并创建对应 .h/.cpp 文件即可。

### 9.6 Step 3 时的 MS Barrier 完整形态预览

Step 3 将在 `Barrier/` 目录下创建两个 Barrier 子类：

| Barrier | 安装阶段 | 写操作行为 | 读操作行为 |
|---------|----------|-----------|-----------|
| **MSIdleBarrier** | IDLE / SWEEP | 直接 store | 直接 load |
| **MSWriteBarrier** | MARK_SETUP / CONCURRENT_MARK / MARK_TERMINATION | shade(old) + shade(new) + store | 直接 load |

> **核心约束**（Step 3 已确认）：所有 CompilerCalls `extern "C"` 函数签名**完全不变**。MS Barrier 通过 `Barrier` 基类的虚函数分派机制工作，无需修改公共 API 层。

---

## 10. 源码目录与 CMakeLists 组织

### 10.1 Step 0 新增文件清单

| 文件路径 | 说明 | 内容 |
|---------|------|------|
| `src/Heap/Collector/MarkSweepCollector.h` | MS 收集器头文件 | 类声明 + 骨架方法 |
| `src/Heap/Collector/MarkSweepCollector.cpp` | MS 收集器实现 | 空壳方法体（LOG + abort/no-op） |

### 10.2 修改文件清单

| 文件路径 | 修改内容 |
|---------|----------|
| `config.cmake` | 新增 `MARKSWEEP_FLAG`，与 `COPYGC_FLAG` 互斥 |
| `build.py` | 新增 `--gc-mode` 参数，控制 `COPYGC_FLAG` / `MARKSWEEP_FLAG` |
| `CMakeLists.txt` (根) | 新增 `MARKSWEEP_FLAG → MRT_USE_MARK_SWEEP` 宏定义 |
| `src/Heap/Barrier/CMakeLists.txt` | 条件编译 MS Barrier 源文件（Step 3 取消注释） |
| `src/Heap/CMakeLists.txt` | 条件编译 WCollector（MS 模式时不编译） |
| `src/Heap/Collector/Collector.h` | `GCPhase` 新增 MS 阶段；`CollectorType` 新增 `MARK_SWEEP_COLLECTOR` |
| `src/Heap/Collector/CollectorProxy.h` | `#ifdef` 切换 WCollector / MarkSweepCollector 成员 |
| `src/Heap/Collector/CollectorProxy.cpp` | `#ifdef` 切换初始化和分派逻辑 |
| `src/Heap/Heap.cpp` | HeapImpl 构造函数和 `InstallBarrier` 条件编译 |

### 10.3 src/Heap/CMakeLists.txt 改造

**现有版本：**
```cmake
set(SRC_LIST
    "Heap.cpp"
    "GcThreadPool.cpp"
)
include_directories(${CMAKE_CURRENT_SOURCE_DIR})
add_subdirectory(Allocator)
add_subdirectory(Barrier)
add_subdirectory(Collector)
add_subdirectory(WCollector)
add_library(Heap STATIC ${SRC_LIST})
```

**改造后版本：**
```cmake
set(SRC_LIST
    "Heap.cpp"
    "GcThreadPool.cpp"
)
include_directories(${CMAKE_CURRENT_SOURCE_DIR})
add_subdirectory(Allocator)
add_subdirectory(Barrier)       # 基类 Barrier + MS Barrier（始终编译，内部条件编译）
add_subdirectory(Collector)     # Collector 基类/公共类（始终编译）

# ─── GC 模式条件编译 ───
if (NOT MARKSWEEP_FLAG MATCHES 1)
    add_subdirectory(WCollector)  # CopyGC 模式的 WCollector + CopyGC Barrier
endif()

add_library(Heap STATIC ${SRC_LIST})
```

> **注意**：`Collector` 子目录始终编译，因为其中包含公共基类（`Collector.h`、`TracingCollector.h`）和新增的 `MarkSweepCollector`。`MarkSweepCollector.cpp` 自身通过 `#ifdef MRT_USE_MARK_SWEEP` 保护。

### 10.4 根 CMakeLists.txt LINK_LIBS 改造

现有链接库列表（以非 Windows 为例）：
```cmake
list(APPEND LINK_LIBS "boundscheck" "Base" "Common" "Concurrency" "Loader"
    "Inspector" "Exception" "Heap" "Mutator" "ObjectModel" "UnwindStack" "Signal"
    "Allocator" "Barrier" "Collector" "WCollector" "BinaryFile" "CjFile"
    "CjFileLoader" "CjSemanticVersion" "Sync" "CpuProfiler" ${SANITIZER_LIB})
```

**改造后：**
```cmake
list(APPEND LINK_LIBS "boundscheck" "Base" "Common" "Concurrency" "Loader"
    "Inspector" "Exception" "Heap" "Mutator" "ObjectModel" "UnwindStack" "Signal"
    "Allocator" "Barrier" "Collector" "BinaryFile" "CjFile"
    "CjFileLoader" "CjSemanticVersion" "Sync" "CpuProfiler" ${SANITIZER_LIB})

# ─── GC 模式条件链接 ───
if (NOT MARKSWEEP_FLAG MATCHES 1)
    list(APPEND LINK_LIBS "WCollector")
endif()
# 注：MS Barrier 源文件编入 Barrier 库，无需额外链接
```

同理，静态库（`cangjie-runtime-static`）的 `target_sources` 中也需做相同改造：

```cmake
# 将硬编码的 $<TARGET_OBJECTS:WCollector> 改为条件引用
if (NOT MARKSWEEP_FLAG MATCHES 1)
    target_sources(cangjie-runtime-static PRIVATE $<TARGET_OBJECTS:WCollector>)
endif()
# 注：MS Barrier 对象文件已包含在 Barrier 库中
```

### 10.5 Collector 子目录 CMakeLists.txt 改造

如果 `src/Heap/Collector/CMakeLists.txt` 当前只编译 CopyCollector 相关源文件，需要条件添加 MarkSweepCollector：

```cmake
set(SRC_LIST
    "CollectorProxy.cpp"
    "CollectorResources.cpp"
    # 其他公共 Collector 文件...
)

if (MARKSWEEP_FLAG MATCHES 1)
    list(APPEND SRC_LIST "MarkSweepCollector.cpp")
else()
    list(APPEND SRC_LIST "CopyCollector.cpp")
endif()

add_library(Collector STATIC ${SRC_LIST})
```

### 10.6 目录结构总览

Step 0 完成后，`src/Heap/` 的目录结构为：

```
src/Heap/
├── CMakeLists.txt              # [修改] 条件编译 WCollector
├── Heap.cpp                    # [修改] HeapImpl 条件编译
├── GcThreadPool.cpp            # [不变]
├── Allocator/                  # [不变] 内存分配器
│   ├── CMakeLists.txt
│   ├── Allocator.h/cpp
│   └── RegionSpace.h/cpp
├── Barrier/                    # [修改] Barrier 基类 + MS Barrier 条件编译
│   ├── CMakeLists.txt          # [修改] 条件编译 MS Barrier 源文件
│   ├── Barrier.h
│   ├── Barrier.cpp
│   ├── MSIdleBarrier.h         # Step 3 创建
│   ├── MSIdleBarrier.cpp       # Step 3 创建
│   ├── MSWriteBarrier.h        # Step 3 创建
│   └── MSWriteBarrier.cpp      # Step 3 创建
├── Collector/                  # [修改] 新增 MarkSweepCollector
│   ├── CMakeLists.txt          # [修改] 条件编译
│   ├── Collector.h             # [修改] GCPhase/CollectorType 扩展
│   ├── TracingCollector.h/cpp  # [不变] 公共基类
│   ├── CopyCollector.h/cpp     # [CopyGC 模式编译]
│   ├── CollectorProxy.h/cpp    # [修改] #ifdef 切换
│   ├── MarkSweepCollector.h    # [新增] MS 收集器头文件
│   └── MarkSweepCollector.cpp  # [新增] MS 收集器骨架实现
├── WCollector/                 # [CopyGC 模式编译]
│   ├── CMakeLists.txt
│   ├── WCollector.h/cpp
│   ├── IdleBarrier.h/cpp
│   ├── EnumBarrier.h/cpp
│   ├── TraceBarrier.h/cpp
│   ├── PostTraceBarrier.h/cpp
│   ├── PreforwardBarrier.h/cpp
│   └── ForwardBarrier.h/cpp

```

---

## 11. 与现有基础设施的复用与改造清单

### 11.1 组件改造矩阵

| 组件 | 文件 | 改造方式 | 改造内容 | 风险等级 |
|------|------|---------|----------|---------|
| **build.py** | `build.py` | 🔄 修改 | 新增 `--gc-mode` 参数，映射为 `COPYGC_FLAG` / `MARKSWEEP_FLAG` | ⭐ 低 |
| **config.cmake** | `config.cmake` | 🔄 修改 | 新增 `MARKSWEEP_FLAG`，确保与 `COPYGC_FLAG` 互斥 | ⭐ 低 |
| **CMakeLists.txt（根）** | `CMakeLists.txt` | 🔄 修改 | 新增 `MRT_USE_MARK_SWEEP` 宏定义；条件链接库 | ⭐⭐ 中 |
| **Heap CMakeLists** | `src/Heap/CMakeLists.txt` | 🔄 修改 | 条件编译 `WCollector` 子目录 | ⭐ 低 |
| **Collector CMakeLists** | `src/Heap/Collector/CMakeLists.txt` | 🔄 修改 | 条件编译 `CopyCollector.cpp` / `MarkSweepCollector.cpp` | ⭐ 低 |
| **GCPhase** | `Collector.h` | 🔄 修改 | 新增 MS 阶段枚举值 (20-23)；新增 `MARK_SWEEP_COLLECTOR` | ⭐ 低 |
| **CollectorProxy** | `CollectorProxy.h/cpp` | 🔄 修改 | `#ifdef MRT_USE_MARK_SWEEP` 切换成员和初始化 | ⭐⭐ 中 |
| **HeapImpl** | `Heap.cpp` | 🔄 修改 | 构造函数和 `InstallBarrier` 条件编译 | ⭐⭐⭐ 高 |
| **MarkSweepCollector** | `Collector/MarkSweepCollector.h/cpp` | ✨ 新增 | 继承 `TracingCollector`，骨架方法 | ⭐⭐ 中 |
| **Barrier CMakeLists** | `Barrier/CMakeLists.txt` | 🔄 修改 | 条件编译 MS Barrier 源文件 | ⭐ 低 |
| **Collector 基类** | `Collector.h` | ✅ 直接复用 | 虚函数接口无需修改（CopyGC 方法默认 abort） | 无 |
| **TracingCollector** | `TracingCollector.h/cpp` | ✅ 直接复用 | 标记基础设施、根枚举、工作栈全部复用 | 无 |
| **Barrier 基类** | `Barrier.h/cpp` | ✅ 直接复用 | 虚函数 + 直通默认实现，无需修改 | 无 |
| **Allocator** | `Allocator/` | ✅ 直接复用 | Step 0 不改分配器（Step 1 改造） | 无 |
| **CompilerCalls** | `CompilerCalls.cpp/h` | ✅ 直接复用 | 所有 `extern "C"` 签名不变，通过 `GetBarrier()` 分派 | 无 |
| **Cangjie.h** | `src/Cangjie.h` | ✅ 直接复用 | 公共 API 与 GC 实现解耦 | 无 |
| **RuntimeConfig.h** | `src/RuntimeConfig.h` | ✅ 直接复用 | 运行时配置中无 GC 类型硬编码 | 无 |

### 11.2 改造依赖关系（拓扑排序）

```
  ┌──────────────────────────────────────────────────────┐
  │ 层 0：构建系统                                        │
  │   build.py → config.cmake → CMakeLists.txt (根)       │
  │   → src/Heap/CMakeLists.txt                           │
  │   → src/Heap/Collector/CMakeLists.txt                 │
  └──────────────────────┬───────────────────────────────┘
                         │
  ┌──────────────────────▼───────────────────────────────┐
  │ 层 1：GCPhase / CollectorType 扩展                    │
  │   Collector.h  — 新增枚举值                            │
  └──────────────────────┬───────────────────────────────┘
                         │
  ┌──────────────────────▼───────────────────────────────┐
  │ 层 2：MarkSweepCollector 骨架                          │
  │   MarkSweepCollector.h/cpp — 依赖层1的新 GCPhase/Type  │
  └──────────────────────┬───────────────────────────────┘
                         │
  ┌──────────────────────▼───────────────────────────────┐
  │ 层 3：CollectorProxy 适配                              │
  │   CollectorProxy.h/cpp — 依赖层2的 MarkSweepCollector  │
  └──────────────────────┬───────────────────────────────┘
                         │
  ┌──────────────────────▼───────────────────────────────┐
  │ 层 4：HeapImpl 条件编译                                │
  │   Heap.cpp — 依赖层3的 CollectorProxy 和层2的 GCPhase  │
  ├─────────────────────────────────────────────────────┘
  │
  ┌──────────────────────▼───────────────────────────────┐
  │ 层 5：Barrier 目录条件编译                               │
  │   Barrier/CMakeLists.txt — 条件编译 MS Barrier 源文件    │
  └─────────────────────────────────────────────────────┘
```

**实施建议**：按层级从 0 到 5 顺序实施，每层完成后编译验证。

### 11.3 复用比例分析

| 分类 | 文件数 | 占比 |
|------|-------|------|
| ✅ 直接复用 | 7 | 41% |
| 🔄 修改 | 9 | 53% |
| ✨ 新增 | 1 | 6% |
| **合计** | **17** | **100%** |

**核心要点**：
- Step 0 的修改都是**增量式**的，通过 `#ifdef` 条件编译保证 CopyGC 模式完全不受影响
- 所有对外接口（`Cangjie.h`、`CompilerCalls`、`RuntimeConfig.h`）零修改
- 最高风险在 `Heap.cpp`（HeapImpl），因为它是 Barrier 安装和 Collector 初始化的汇聚点
- `TracingCollector` 提供了丰富的可复用基础设施（根枚举、标记栈、阶段转换、终结器处理）

---

## 12. 验证方案

### 12.1 验证目标

Step 0 的验证必须同时满足两项要求：
1. **CopyGC 模式回归**：默认编译（`--gc-mode copygc` 或不指定）产物行为与改造前完全一致
2. **MS 模式编译链路**：`--gc-mode marksweep` 能成功编译、链接、加载，骨架 Collector 正确初始化

### 12.2 验证用例矩阵

| 用例 ID | 验证内容 | 构建命令 | 预期结果 |
|---------|----------|---------|----------|
| V0-01 | CopyGC 默认编译 | `python3 build.py build` | 编译成功，产物与改造前二进制一致 |
| V0-02 | CopyGC 显式编译 | `python3 build.py build --gc-mode copygc` | 编译成功，结果同 V0-01 |
| V0-03 | CopyGC 测试回归 | V0-01 产物 + `cd gc && cjc gc.cj && ./main` | 测试通过，输出与改造前一致 |
| V0-04 | MS 模式编译 | `python3 build.py build --gc-mode marksweep` | 编译成功，生成 libcangjie-runtime.dylib |
| V0-05 | MS 模式链接 | 检查 V0-04 产物符号表 | 包含 `MarkSweepCollector` 相关符号 |
| V0-06 | MS 模式加载 | 替换 dylib + 简单 Hello World 程序 | 加载成功，`InitCJRuntime` 完成初始化 |
| V0-07 | MS 模式 GC 触发 | 分配对象触发 GC | `MarkSweepCollector::RunGarbageCollection` 打印 LOG 后安全返回 |
| V0-08 | 互斥校验 | `--gc-mode copygc` 检查宏定义 | `MRT_USE_COPYGC=1`，无 `MRT_USE_MARK_SWEEP` |
| V0-09 | 互斥校验 | `--gc-mode marksweep` 检查宏定义 | `MRT_USE_MARK_SWEEP=1`，无 `MRT_USE_COPYGC` |

### 12.3 CopyGC 回归验证步骤

```bash
# 1. 编译（默认 CopyGC 模式）
cd /Users/lipeisheng/code/cangjie/cangjie/cangjie_runtime/runtime
python3 build.py build

# 2. 替换 SDK 中的 dylib
cp output/common/darwin_release_aarch64/runtime/lib/darwin_aarch64_cjnative/libcangjie-runtime.dylib \
   /Users/lipeisheng/code/cangjie/cangjie-pkg/self/dev260313/cangjie/runtime/lib/darwin_aarch64_cjnative/libcangjie-runtime.dylib

# 3. 运行测试
source /Users/lipeisheng/code/cangjie/cangjie-pkg/self/dev260313/cangjie/envsetup.sh
cd gc && cjc gc.cj && ./main

# 4. 验证产物一致性（可选：对比符号表）
nm -gU output/common/darwin_release_aarch64/runtime/lib/darwin_aarch64_cjnative/libcangjie-runtime.dylib | \
    grep -c "WCollector"  # 应 > 0

nm -gU output/common/darwin_release_aarch64/runtime/lib/darwin_aarch64_cjnative/libcangjie-runtime.dylib | \
    grep -c "MarkSweepCollector"  # 应 = 0
```

### 12.4 MS 模式编译验证步骤

```bash
# 1. 编译（MS 模式）
cd /Users/lipeisheng/code/cangjie/cangjie/cangjie_runtime/runtime
python3 build.py build --gc-mode marksweep

# 2. 检查符号表
nm -gU output/common/darwin_release_aarch64/runtime/lib/darwin_aarch64_cjnative/libcangjie-runtime.dylib | \
    grep "MarkSweepCollector"  # 应包含 Init/Fini/RunGarbageCollection 等符号

nm -gU output/common/darwin_release_aarch64/runtime/lib/darwin_aarch64_cjnative/libcangjie-runtime.dylib | \
    grep -c "WCollector"  # 应 = 0

# 3. 替换并运行简单程序
cp output/common/darwin_release_aarch64/runtime/lib/darwin_aarch64_cjnative/libcangjie-runtime.dylib \
   /Users/lipeisheng/code/cangjie/cangjie-pkg/self/dev260313/cangjie/runtime/lib/darwin_aarch64_cjnative/libcangjie-runtime.dylib

source /Users/lipeisheng/code/cangjie/cangjie-pkg/self/dev260313/cangjie/envsetup.sh

# 使用简单的 Hello World 验证初始化
cat > /tmp/test_ms_init.cj << 'EOF'
main() {
    println("MS mode init OK")
}
EOF
cd /tmp && cjc test_ms_init.cj && ./main
# 预期输出: "MS mode init OK"
```

### 12.5 骨架 Collector 行为验证

MS 模式下，GC 触发时骨架 `MarkSweepCollector` 应：
1. `RunGarbageCollection` 被调用时打印日志后直接返回（不执行实际回收）
2. 不崩溃、不死锁
3. 内存持续增长（因为没有实际回收）—— 这是预期行为

```bash
# 分配压力测试（验证骨架不崩溃）
cat > /tmp/test_ms_alloc.cj << 'EOF'
main() {
    var i = 0
    while (i < 10000) {
        let arr = Array<Int64>(100, item: 0)
        i++
    }
    println("MS alloc test OK (no crash)")
}
EOF
cd /tmp && cjc test_ms_alloc.cj && ./main
# 预期: 打印 "MS alloc test OK (no crash)"
# 日志中可见 MarkSweepCollector::RunGarbageCollection 被调用（如果启用 LOG）
```

### 12.6 常见失败排查

| 失败现象 | 可能原因 | 排查方法 |
|---------|---------|---------|
| MS 模式编译报 undefined symbol | CMakeLists 条件编译遗漏 | 检查 LINK_LIBS 和 target_sources 是否包含 MarkSweepCollector |
| MS 模式链接报 duplicate symbol | `#ifdef` 未正确隔离 CopyGC/MS 代码 | 检查 CopyCollector.cpp 是否被错误编译 |
| CopyGC 模式回归失败 | `#ifdef` 改动影响了默认路径 | diff 改造前后的预处理输出（`-E` 标志） |
| MS 模式启动崩溃 | HeapImpl::InstallBarrier 中 GCPhase 映射缺失 | 检查 MS 模式是否有对应的 Barrier 实例 |
| MS 模式 GC 触发后死锁 | 骨架方法未正确返回 | 确认 `RunGarbageCollection` 是 LOG + return，无等待逻辑 |

### 12.7 验证检查清单

- [ ] `python3 build.py build` — CopyGC 模式编译成功
- [ ] `python3 build.py build --gc-mode marksweep` — MS 模式编译成功
- [ ] CopyGC 模式 `gc.cj` 测试通过
- [ ] MS 模式简单程序运行不崩溃
- [ ] MS 模式分配压力测试不崩溃
- [ ] CopyGC 产物不含 `MarkSweepCollector` 符号
- [ ] MS 产物不含 `WCollector` 符号
- [ ] CopyGC 模式宏定义正确（`MRT_USE_COPYGC=1`）
- [ ] MS 模式宏定义正确（`MRT_USE_MARK_SWEEP=1`）
