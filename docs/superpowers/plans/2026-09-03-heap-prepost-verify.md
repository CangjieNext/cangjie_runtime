# GC 堆 Pre/Post 校验 实现计划

> 状态：已执行完毕（2026-09）。所实现的 FP 握手版已被 ADR-12 walker 方案（`specs/2026-09-10-heap-verify-walker-design.md`）取代；本文档保留作历史执行记录。

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 按 spec `docs/superpowers/specs/2026-09-02-heap-prepost-verify-design.md` 实现 GC 周期 pre/post 堆校验（HeapVerifier 类 + FP 活动证据 + cjPreVerify/cjPostVerify 环境变量开关）。

**Architecture:** 新增无状态 `HeapVerifier` 类（`ForEachObj` 全堆扫描 + `ForEachRefField` 一跳引用检查 + 4 类根集检查），在 `TracingCollector::PreGarbageCollection` / `PostGarbageCollection` 两个点以 `ScopedStopTheWorld` 包裹调用；`FinalizerProcessor` 增加 3 个原子计数器供失败归因。只读校验，失败统计后 fatal。

**Tech Stack:** C++17（runtime 既有代码风格）；构建 `runtime/build.py`（native release）；端到端验证用 `cjc` 编译仓颉程序 + 环境变量。

---

## 重要约定

1. **git 提交**：本仓库提交**仅在用户明确指示时执行**。每个任务以"检查点"结尾（构建/测试通过即完成），执行代理不得自主 commit。
2. **TDD 适配**：runtime 无 C++ 单测基建（已勘察确认），每个任务的"测试"= 构建成功 + Task 5-8 的端到端验证。故障注入采用临时代码（Task 7 给出注入与撤销的完整 diff）。
3. **构建前提**：`$WORKSPACE/cangjie_compiler/output/envsetup.sh` 与 `modules/` 必须已存在（Task 0 检查）。环境变量：`WORKSPACE=/home/zhangbin/cangjie/code/opencode_workspace/cj`，`CANGJIE_VERSION=1.0.0`，`ARCH=x86_64`。
4. 所有行号为写作时锚点，以当前代码实际内容为准定位插入点。
5. include 根有两个：`runtime/src/`（如 `Common/Runtime.h`）和 `runtime/src/Heap/`（如 `Allocator/RegionInfo.h`、`Collector/...`）。

## 已验证的关键事实（实现依据）

- `syncMutex` 是 `std::recursive_mutex`（`MutatorManager.h:252`）：GC 线程持 `ScopedSTWLock` 时再进 `ScopedStopTheWorld` 是重入，**无死锁**（`DumpBeforeGC` 同模式先例）。
- `RegionManager::ForEachObjSafe` 内部自带 STW（`RegionManager.cpp:532-537`），verify **必须**用 `ForEachObj(visitor, false)`（照抄 `DumpHeap`，`TracingCollector.cpp:589-595`）。
- 引用字段读取必须用 `RefField<>::GetTargetObject()`（剥 tag 位，48 位地址）；FP/export 根以可能带 tag 的原始值存于 `ObjectRef.object`，读取需包 `RefField<>(reinterpret_cast<MAddress>(ref.object)).GetTargetObject()`（`FinalizerProcessor.cpp:200-201` 同模式）。
- 四类根的访问接口全在 `Heap`/`Runtime` 公共 API：`Heap::GetHeap().VisitStaticRoots(v)`（Heap.h:93）、`VisitAllExportRoots(v)`（Heap.h:96）、`GetFinalizerProcessor().VisitGCRoots(v)`（Heap.h:106）、`Runtime::Current().GetConcurrencyModel().VisitGCRoots(&v)`（TracingCollector.cpp:295，include `Common/Runtime.h`）。
- 状态码：`obj->GetStateWord().GetStateCode()`，枚举 `ObjectState::ObjectStateCode{NORMAL=0,LOCKED=1,FORWARDING=2,FORWARDED=3}`（StateWord.h:21-30）。ghost 判定：`RegionInfo::InGhostFromRegion(obj)`（RegionInfo.h:516，静态，任意堆内地址安全）。
- `cjHeapSize` 支持 `KB/MB/GB` 后缀，Linux 最小 4MB（`CjScheduler.cpp:85-108`）——压力测试用小堆强制高频 GC。
- 仓颉语法（已从现存代码确认）：finalizer `~ClassName() {}`；数组 `Array<Byte>(8, repeat: 0)`；可选 `?Node = None`。

---

### Task 0: 环境预检

**Files:** 无修改

- [ ] **Step 0.1: 检查构建前提**

```bash
WORKSPACE=/home/zhangbin/cangjie/code/opencode_workspace/cj
[ -f "$WORKSPACE/cangjie_compiler/output/envsetup.sh" ] || { echo "FAIL: compiler not built"; exit 1; }
[ -d "$WORKSPACE/cangjie_compiler/output/modules" ] || { echo "FAIL: stdlib missing"; exit 1; }
[ -d "$WORKSPACE/cangjie_runtime_zb/runtime" ] || { echo "FAIL: runtime source missing"; exit 1; }
echo "OK: prerequisites verified"
```

期望输出：`OK: prerequisites verified`。失败则停止，报告用户（需先跑完整 toolchain 构建，不在本计划范围）。

---

### Task 1: FinalizerProcessor 活动计数器

**Files:**
- Modify: `runtime/src/Heap/Collector/FinalizerProcessor.h`
- Modify: `runtime/src/Heap/Collector/FinalizerProcessor.cpp`

- [ ] **Step 1.1: FinalizerProcessor.h —— 新增 3 个原子字段与 getter**

在私有段 `std::atomic<bool> shouldFeedHungryBuffers;`（:125）之后插入：

```cpp
    // Heap verification (pre/post-GC) diagnostics: cumulative counters.
    // Relaxed order is sufficient: diagnostic reporting only.
    std::atomic<uint64_t> finalizedCount {0};
    std::atomic<uint64_t> enqueuedCount {0};
    enum class FpState : uint8_t { IDLE, PROCESSING };
    std::atomic<FpState> fpState {FpState::IDLE};
```

在公有段 `bool IsRunning() const { return running; }`（:82）之前插入：

```cpp
    // Heap verification diagnostics accessors.
    uint64_t GetFinalizedCount() const { return finalizedCount.load(std::memory_order_relaxed); }
    uint64_t GetEnqueuedCount() const { return enqueuedCount.load(std::memory_order_relaxed); }
    bool IsProcessing() const { return fpState.load(std::memory_order_relaxed) == FpState::PROCESSING; }
```

- [ ] **Step 1.2: FinalizerProcessor.h —— 构造函数初始化**

构造函数 `FinalizerProcessor::FinalizerProcessor()`（:68-79）体内、`shouldFeedHungryBuffers.store(...)` 之后追加（匹配既有风格）：

```cpp
    finalizedCount.store(0, std::memory_order_relaxed);
    enqueuedCount.store(0, std::memory_order_relaxed);
    fpState.store(FpState::IDLE, std::memory_order_relaxed);
```

- [ ] **Step 1.3: FinalizerProcessor.cpp —— EnqueueFinalizables 计数**

`EnqueueFinalizables`（:195-214）中 `finalizables.push_back(reinterpret_cast<BaseObject*>(tmpField.GetFieldValue()));`（:204）之后、`it = finalizers.erase(it);`（:205）之前插入：

```cpp
            enqueuedCount.fetch_add(1, std::memory_order_relaxed);
```

- [ ] **Step 1.4: FinalizerProcessor.cpp —— ProcessFinalizables 状态标记**

`ProcessFinalizables`（:259-275）：`MRT_PHASE_TIMER("Finalizer", FINALIZE);`（:261）之后插入：

```cpp
    fpState.store(FpState::PROCESSING, std::memory_order_relaxed);
```

函数末尾（`hasFinalizableJob.store(...)` 块之后、右花括号之前）插入：

```cpp
    fpState.store(FpState::IDLE, std::memory_order_relaxed);
```

- [ ] **Step 1.5: FinalizerProcessor.cpp —— ProcessFinalizableList 完成计数**

`ProcessFinalizableList`（:221-257）的 erase 块：

```cpp
        {
            std::lock_guard<std::mutex> l(listLock);
            itor = workingFinalizables.erase(itor);
        }
```

改为：

```cpp
        {
            std::lock_guard<std::mutex> l(listLock);
            itor = workingFinalizables.erase(itor);
            finalizedCount.fetch_add(1, std::memory_order_relaxed);
        }
```

- [ ] **Step 1.6: 增量构建验证**

```bash
cd "$WORKSPACE/cangjie_runtime_zb/runtime"
python3 build.py build -t release -v 1.0.0 2>&1 | tail -5
```

期望：退出码 0，无 `error:`。失败→按构建日志第一个 error 定位修复后再继续。

**检查点**：Task 1 完成。不提交。

---

### Task 2: HeapVerifier 类（完整实现）

**Files:**
- Create: `runtime/src/Heap/HeapVerifier.h`
- Create: `runtime/src/Heap/HeapVerifier.cpp`
- Modify: `runtime/src/Heap/CMakeLists.txt`

- [ ] **Step 2.1: 创建 HeapVerifier.h**

```cpp
// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_HEAP_VERIFIER_H
#define MRT_HEAP_VERIFIER_H

#include "Common/BaseObject.h"
#include "Common/StateWord.h"

namespace MapleRuntime {

// GC pre/post-cycle heap verification.
// Spec: docs/superpowers/specs/2026-09-02-heap-prepost-verify-design.md
// Enabled via cjPreVerify / cjPostVerify ("1"/"true"/"on", case-insensitive).
// Read-only: reports and fatals on failure, never mutates the heap.
class HeapVerifier {
public:
    // FP activity counters snapshotted at GC cycle start; used to attribute
    // verification failures (spec section 8.2).
    struct FpSnapshot {
        uint64_t finalizedCount = 0;
        uint64_t enqueuedCount = 0;
    };

    static bool IsPreVerifyEnabled();
    static bool IsPostVerifyEnabled();

    HeapVerifier(const char* tag, const FpSnapshot& fpSnapshot) : tag_(tag), fpSnapshot_(fpSnapshot) {}
    ~HeapVerifier() = default;

    // Walks all heap objects and root sets (spec section 4.3).
    // Requires STW held by the caller (same contract as TracingCollector::DumpHeap).
    void Verify();

private:
    void VerifyObject(BaseObject* obj);
    void VerifyReference(BaseObject* holder, RefField<>& field);
    // Core one-hop target check shared by ref-fields and roots (spec section 6.2).
    void CheckTarget(const char* kind, BaseObject* holder, size_t offset, BaseObject* ref);
    // State code rule (spec section 6.3): NORMAL/LOCKED always legal;
    // FORWARDING/FORWARDED only inside ghost from-regions.
    static bool CheckStateCode(BaseObject* obj);
    void VerifyRoots();
    void RecordFailure(const char* fmt, ...);

    const char* const tag_;
    const FpSnapshot fpSnapshot_;
    size_t failCount_ = 0;
    size_t objCount_ = 0;
    size_t rootCount_ = 0;
    uint64_t startTime_ = 0;
};
} // namespace MapleRuntime
#endif // MRT_HEAP_VERIFIER_H
```

- [ ] **Step 2.2: 创建 HeapVerifier.cpp**

```cpp
// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "HeapVerifier.h"

#include <cstdarg>
#include <cstring>

#include "Allocator/Allocator.h"
#include "Allocator/RegionInfo.h"
#include "Base/Log.h"
#include "Base/Panic.h"
#include "Base/TimeUtils.h"
#include "Common/Runtime.h"
#include "Heap.h"
#include "Mutator/MutatorManager.h"

namespace MapleRuntime {

namespace {
// "1"/"true"/"on" (case-insensitive) => true; unset/anything else => false.
bool IsTruthyEnv(const char* name)
{
    const char* val = std::getenv(name);
    if (val == nullptr) {
        return false;
    }
    auto equalsIgnoreCase = [val](const char* literal) {
        size_t i = 0;
        for (; literal[i] != '\0'; ++i) {
            char c = val[i];
            if (c >= 'A' && c <= 'Z') {
                c = static_cast<char>(c - 'A' + 'a');
            }
            if (c != literal[i]) {
                return false;
            }
        }
        return val[i] == '\0';
    };
    return equalsIgnoreCase("1") || equalsIgnoreCase("true") || equalsIgnoreCase("on");
}
} // namespace

bool HeapVerifier::IsPreVerifyEnabled()
{
    static const bool enabled = IsTruthyEnv("cjPreVerify");
    return enabled;
}

bool HeapVerifier::IsPostVerifyEnabled()
{
    static const bool enabled = IsTruthyEnv("cjPostVerify");
    return enabled;
}

void HeapVerifier::Verify()
{
    MRT_ASSERT(MutatorManager::Instance().WorldStopped(), "HeapVerifier::Verify() requires STW");
    startTime_ = TimeUtil::NanoSeconds();
    Heap& heap = Heap::GetHeap();
    // safe=false is REQUIRED: ForEachObjSafe takes STW internally and would
    // deadlock against the caller's ScopedStopTheWorld (DumpHeap precedent).
    heap.ForEachObj([this](BaseObject* obj) { VerifyObject(obj); }, false);
    VerifyRoots();
    if (failCount_ > 0) {
        FinalizerProcessor& fp = heap.GetFinalizerProcessor();
        const uint64_t finalizedNow = fp.GetFinalizedCount();
        const uint64_t enqueuedNow = fp.GetEnqueuedCount();
        LOG(RTLOG_ERROR,
            "FP activity this cycle: finalized %llu (from %llu to %llu), enqueued %llu, FP mid-run: %s",
            static_cast<unsigned long long>(finalizedNow - fpSnapshot_.finalizedCount),
            static_cast<unsigned long long>(fpSnapshot_.finalizedCount),
            static_cast<unsigned long long>(finalizedNow),
            static_cast<unsigned long long>(enqueuedNow - fpSnapshot_.enqueuedCount),
            fp.IsProcessing() ? "yes" : "no");
        if (std::strcmp(tag_, "pre-GC") == 0) {
            LOG(RTLOG_ERROR, "NOTE: heap was already corrupted BEFORE this GC cycle started");
        }
        CHECK_DETAIL(false, "%s heap verify failed with %zu failures", tag_, failCount_);
    }
    VLOG(REPORT, "%s heap verify passed: %zu objects, %zu roots, %llu us", tag_, objCount_, rootCount_,
         static_cast<unsigned long long>((TimeUtil::NanoSeconds() - startTime_) / 1000));
}

void HeapVerifier::VerifyObject(BaseObject* obj)
{
    ++objCount_;
    if (!Heap::IsHeapAddress(reinterpret_cast<MAddress>(obj))) {
        RecordFailure("object %p from ForEachObj is outside heap range", obj);
        return;
    }
    if (!obj->IsValidObject()) {
        RecordFailure("object %p has invalid header (null or corrupted TypeInfo)", obj);
        return; // header untrusted: skip ref-field traversal (spec 6.1)
    }
    if (!CheckStateCode(obj)) {
        RecordFailure("object %p<%s> has unexpected state %d outside ghost from-region", obj,
                      obj->GetTypeInfo()->GetName(), static_cast<int>(obj->GetStateWord().GetStateCode()));
        // state failure does not preclude ref traversal (spec 6.1)
    }
    if (obj->GetTypeInfo()->HasRefField()) {
        obj->ForEachRefField([this, obj](RefField<>& field) { VerifyReference(obj, field); });
    }
}

void HeapVerifier::VerifyReference(BaseObject* holder, RefField<>& field)
{
    CheckTarget("ref-field", holder,
                static_cast<size_t>(reinterpret_cast<MAddress>(&field) - reinterpret_cast<MAddress>(holder)),
                field.GetTargetObject());
}

void HeapVerifier::CheckTarget(const char* kind, BaseObject* holder, size_t offset, BaseObject* ref)
{
    if (ref == nullptr) {
        return; // legal null
    }
    if (!Heap::IsHeapAddress(reinterpret_cast<MAddress>(ref))) {
        return; // legal non-heap target (e.g. constant strings), collector does the same
    }
    if (!Heap::GetHeap().GetAllocator().IsHeapObject(reinterpret_cast<MAddress>(ref))) {
        RecordFailure("%s: %p<%s> offset %zu targets %p which is not a heap object", kind, holder,
                      (holder != nullptr) ? holder->GetTypeInfo()->GetName() : "root", offset, ref);
        return;
    }
    if (!ref->IsValidObject()) {
        RecordFailure("%s: %p<%s> offset %zu targets %p with invalid header", kind, holder,
                      (holder != nullptr) ? holder->GetTypeInfo()->GetName() : "root", offset, ref);
        return;
    }
    if (!CheckStateCode(ref)) {
        RecordFailure("%s: %p<%s> offset %zu targets %p<%s> in state %d outside ghost from-region", kind, holder,
                      (holder != nullptr) ? holder->GetTypeInfo()->GetName() : "root", offset, ref,
                      ref->GetTypeInfo()->GetName(), static_cast<int>(ref->GetStateWord().GetStateCode()));
    }
}

bool HeapVerifier::CheckStateCode(BaseObject* obj)
{
    switch (obj->GetStateWord().GetStateCode()) {
        case ObjectState::NORMAL:
        case ObjectState::LOCKED:
            return true;
        case ObjectState::FORWARDING:
        case ObjectState::FORWARDED:
            // Legal only as forwarding residue inside a ghost from-region.
            return RegionInfo::InGhostFromRegion(obj);
        default:
            return false;
    }
}

void HeapVerifier::VerifyRoots()
{
    Heap& heap = Heap::GetHeap();
    // Static roots: RefField-based slots.
    RefFieldVisitor staticVisitor = [this](RefField<>& field) {
        ++rootCount_;
        CheckTarget("static-root", nullptr, 0, field.GetTargetObject());
    };
    heap.VisitStaticRoots(staticVisitor);

    // ObjectRef-based roots may carry tag bits in the raw value (same storage
    // as FinalizerProcessor lists); wrap through RefField to strip them.
    auto refVisitor = [this](ObjectRef& ref) {
        ++rootCount_;
        BaseObject* target =
            RefField<>(reinterpret_cast<MAddress>(ref.object)).GetTargetObject();
        CheckTarget("root", nullptr, 0, target);
    };
    RootVisitor rootVisitor = refVisitor;

    heap.VisitAllExportRoots(rootVisitor);                                   // export roots
    Runtime::Current().GetConcurrencyModel().VisitGCRoots(&rootVisitor);     // concurrency roots
    heap.GetFinalizerProcessor().VisitGCRoots(rootVisitor);                  // FP roots
}

void HeapVerifier::RecordFailure(const char* fmt, ...)
{
    ++failCount_;
    char buf[512];
    va_list args;
    va_start(args, fmt);
    (void)vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    LOG(RTLOG_ERROR, "%s", buf);
}
} // namespace MapleRuntime
```

（`FormatLog` 为 printf 风格变参，`Log.h:394`，无 va_list 版本，故用 `vsnprintf` 转接。）

- [ ] **Step 2.3: 注册到 CMake**

`runtime/src/Heap/CMakeLists.txt` 的 `SRC_LIST` 改为：

```cmake
set(SRC_LIST
    "Heap.cpp"
    "HeapVerifier.cpp"
    "GcThreadPool.cpp"
)
```

- [ ] **Step 2.4: 构建验证**

```bash
cd "$WORKSPACE/cangjie_runtime_zb/runtime"
python3 build.py build -t release -v 1.0.0 2>&1 | tail -5
```

期望：退出码 0。若 include 路径报错（如 `Base/TimeUtils.h` 找不到），用 `ls runtime/src/Base/` 核对实际文件名（TimeUtils.h/TimeUtil.h）后修正 include。

**检查点**：Task 2 完成。不提交。

---

### Task 3: TracingCollector 接线（FP 快照 + 两个 hook）

**Files:**
- Modify: `runtime/src/Heap/Collector/TracingCollector.h`
- Modify: `runtime/src/Heap/Collector/TracingCollector.cpp`

- [ ] **Step 3.1: TracingCollector.h —— include 与成员**

在 include 区（跟随现有 include）加：

```cpp
#include "HeapVerifier.h"
```

在 `TracingCollector` 类私有成员区（`Allocator& theAllocator;` :318 附近）加：

```cpp
    // FP activity counters snapshotted at cycle start; shared by pre/post verifiers.
    HeapVerifier::FpSnapshot fpSnapshot {};
```

- [ ] **Step 3.2: TracingCollector.cpp —— PreGarbageCollection 插入快照与 pre-verify**

`PreGarbageCollection`（:653-677），在函数开头 VLOG（:655-657）之后、`SatbBuffer::Instance().Init();`（:660）之前插入：

```cpp
    // FP activity snapshot must precede any verification/GC work (spec 7.2).
    FinalizerProcessor& fp = Heap::GetHeap().GetFinalizerProcessor();
    fpSnapshot.finalizedCount = fp.GetFinalizedCount();
    fpSnapshot.enqueuedCount = fp.GetEnqueuedCount();
```

在 `#endif`（DumpBeforeGC 块，:675）之后、`TRACE_COUNT(...)`（:676）之前插入：

```cpp
    if (HeapVerifier::IsPreVerifyEnabled()) {
        if (MutatorManager::Instance().WorldStopped()) {
            HeapVerifier("pre-GC", fpSnapshot).Verify();
        } else {
            ScopedStopTheWorld stw("pre-verify");
            HeapVerifier("pre-GC", fpSnapshot).Verify();
        }
    }
```

- [ ] **Step 3.3: TracingCollector.cpp —— PostGarbageCollection 插入 post-verify**

`PostGarbageCollection`（:679-690），在 `PagePool::Instance().Trim();`（:684）之后、`collectorResources.NotifyGCFinished(gcIndex);`（:685）之前插入：

```cpp
    if (HeapVerifier::IsPostVerifyEnabled()) {
        if (MutatorManager::Instance().WorldStopped()) {
            HeapVerifier("post-GC", fpSnapshot).Verify();
        } else {
            ScopedStopTheWorld stw("post-verify");
            HeapVerifier("post-GC", fpSnapshot).Verify();
        }
    }
```

- [ ] **Step 3.4: 构建验证**

```bash
cd "$WORKSPACE/cangjie_runtime_zb/runtime"
python3 build.py build -t release -v 1.0.0 2>&1 | tail -5
```

期望：退出码 0。

**检查点**：Task 3 完成。不提交。

---

### Task 4: 安装 runtime + helloworld 冒烟

- [ ] **Step 4.1: 安装并部署到编译器输出树**

```bash
cd "$WORKSPACE/cangjie_runtime_zb/runtime"
python3 build.py install 2>&1 | tail -3
cp -R output/common/linux_release_x86_64/lib "$WORKSPACE/cangjie_compiler/output/"
cp -R output/common/linux_release_x86_64/runtime "$WORKSPACE/cangjie_compiler/output/"
```

期望：install 退出码 0；`ls "$WORKSPACE/cangjie_compiler/output/runtime"` 可见运行时库。

- [ ] **Step 4.2: helloworld 冒烟（双开）**

```bash
source "$WORKSPACE/cangjie_compiler/output/envsetup.sh"
cd "$WORKSPACE/helloworld"
rm -f main default.cjo
cjc hello.cj
cjPreVerify=1 cjPostVerify=1 ./main
```

期望：stdout 末行为 `helloworld`；无 fatal/segfault。注：helloworld 不一定触发 GC——"verify passed" 日志可能出现 0 次（也合法），关键是**不崩溃**。REPORT 日志是否输出取决于 `MRT_LOG_LEVEL`，不作为本步判据。

- [ ] **Step 4.3: 开关默认关闭验证**

```bash
./main
```

期望：输出 `helloworld`，行为与改动前的 runtime 一致（默认全关，零影响）。

**检查点**：Task 4 完成。不提交。

---

### Task 5: 仓颉压力程序验证（正向）

- [ ] **Step 5.1: 创建压力程序**

```bash
mkdir -p "$WORKSPACE/verify_stress"
cat > "$WORKSPACE/verify_stress/stress.cj" << 'EOF'
class Fin {
    var id: Int64
    public init(id: Int64) {
        this.id = id
    }
    ~Fin() {
        let arr = Array<Int64>(64, repeat: id)
        if (arr[0] < 0) {
            println("impossible")
        }
    }
}

class Node {
    var next: ?Node
    var data: Array<Int64>
    public init() {
        next = None
        data = Array<Int64>(32, repeat: 1)
    }
}

main() {
    var head: ?Node = None
    for (i in 0..300000) {
        let n = Node()
        n.next = head
        head = n
        if (i % 3 == 0) {
            Fin(i) // dropped immediately => finalizable garbage
        }
        if (i % 1000 == 0) {
            head = None // drop the whole chain
        }
    }
    println("stress done")
}
EOF
```

- [ ] **Step 5.2: 编译并运行（双开 + 小堆强制高频 GC）**

```bash
source "$WORKSPACE/cangjie_compiler/output/envsetup.sh"
cd "$WORKSPACE/verify_stress"
cjc stress.cj
cjHeapSize=16MB cjPreVerify=1 cjPostVerify=1 MRT_LOG_LEVEL=v ./main; echo "exit=$?"
```

期望：
- **硬判据**：退出码 0，stdout 末行 `stress done`，无任何 `heap verify failed` / fatal / segfault
- 软判据（尽力）：日志出现 `pre-GC heap verify passed: N objects...` / `post-GC heap verify passed: ...`（`MRT_LOG_LEVEL` 合法值为单字符 `v/d/i/w/e/f/s`，LogFile.cpp:272-296；passed 行经 VLOG(REPORT) 输出，受类别开关影响，若无输出不算失败）

若出现 verify failed：**停止**，收集失败明细日志（含对象地址/偏移/原因 + FP activity 行），报告用户——这可能是 verify 误报（规则 bug）或真实 GC bug，由用户判断。

- [ ] **Step 5.3: 单开关独立性与关闭态**

```bash
cjHeapSize=16MB cjPreVerify=1 ./main          # 期望：只有 pre-GC passed 日志，无 post
cjHeapSize=16MB cjPostVerify=1 ./main         # 期望：只有 post-GC passed 日志，无 pre
cjHeapSize=16MB ./main                        # 期望：无任何 verify 日志，正常完成
```

**检查点**：Task 5 完成。不提交。

---

### Task 6: 阴性测试（故障注入 → 检测 → 撤销）

- [ ] **Step 6.1: 临时注入（写完即删）**

`HeapVerifier.cpp` 的 `Verify()` 开头（`startTime_ = ...` 之后）临时插入：

```cpp
    // TEMPORARY fault injection for negative testing. REMOVE AFTER TASK 6.
    if (std::getenv("cjVerifyFaultTest") != nullptr) {
        bool injected = false;
        Heap::GetHeap().ForEachObj([&injected](BaseObject* obj) {
            if (injected || !obj->IsValidObject() || !obj->GetTypeInfo()->HasRefField()) {
                return;
            }
            obj->ForEachRefField([](RefField<>& field) {
                field.SetFieldValue(Heap::GetHeap().GetStartAddress() + 8); // mid-heap bogus target
            });
            injected = true;
        }, false);
    }
```

（注入第一个含引用字段的对象，避免"第 N 个对象恰好无引用字段"导致测试静默通过。）

并重新构建+安装（Task 4.1 的命令）。

- [ ] **Step 6.2: 验证检测与报告**

（用压力程序 + 小堆保证 GC 一定发生——helloworld 可能不触发 GC，见 Task 4.2 注。）

```bash
source "$WORKSPACE/cangjie_compiler/output/envsetup.sh"
cd "$WORKSPACE/verify_stress"
cjHeapSize=16MB cjVerifyFaultTest=1 cjPostVerify=1 ./main; echo "exit=$?"
```

期望：
- 进程以 fatal 中止（非 0 退出），日志含：
  - `post-GC heap verify failed with N failures`（N≥1）
  - 至少一条 `ref-field: <holder> ... targets <addr> which is not a heap object`（或 invalid header 变体）明细
  - `FP activity this cycle: finalized ...` 证据行

- [ ] **Step 6.3: 验证 pre 路径与 NOTE 行**

```bash
cjHeapSize=16MB cjVerifyFaultTest=1 cjPreVerify=1 ./main; echo "exit=$?"
```

期望：fatal，且日志含 `NOTE: heap was already corrupted BEFORE this GC cycle started`。

- [ ] **Step 6.4: 撤销注入（必须完成）**

删除 Step 6.1 插入的全部临时代码，重新构建+安装（Task 4.1 命令），并复跑 Task 5.2 确认恢复正常。

- [ ] **Step 6.5: FP 证据数值验证**

```bash
cjHeapSize=16MB cjVerifyFaultTest=1 cjPostVerify=1 ./main 2>&1 | grep "FP activity"
```

期望：fatal 报告中 `enqueued` ≥ 1（finalizer 密集程序，本轮必有入队）；`finalized` 数值 ≤ 实际创建的 Fin 数量级。记录数值供报告。

**检查点**：Task 6 完成，注入已撤销。不提交。

---

### Task 7: 性能记录（informational）

- [ ] **Step 7.1: 记录 verify 开销**

```bash
cd "$WORKSPACE/verify_stress"
cjHeapSize=64MB cjPreVerify=1 cjPostVerify=1 MRT_LOG_LEVEL=REPORT ./main 2>&1 \
  | grep "heap verify passed" | tail -4
```

记录 pre/post 各自的 objects 数与耗时（µs），写入最终报告（无门限，仅记录）。

**检查点**：Task 7 完成。

---

## 完成标准（对齐 spec §9）

| # | 项目 | 对应任务 |
|---|---|---|
| 1 | 冒烟无 fatal | Task 4.2 |
| 2 | 压力程序无 fatal | Task 5.2 |
| 3 | 注入损坏 → fatal + 明细正确 | Task 6.2/6.3 |
| 4 | FP 证据数值正确 | Task 6.5 |
| 5 | 开关独立 | Task 5.3 |
| 6 | 性能记录 | Task 7 |

全部通过后向用户汇报结果（含 Task 7 数据），等待用户指示是否提交。

## 已知偏差（需向用户披露）

- spec §9 第 2 项提到"弱引用"：stdlib 未找到公开弱引用 API，压力程序以 finalizer + 链表 + 数组替代。若用户知晓弱引用 API，可补充测试。
