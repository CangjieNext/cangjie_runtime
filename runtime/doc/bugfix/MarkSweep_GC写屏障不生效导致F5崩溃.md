# Mark-Sweep GC 写屏障不生效导致 F5 测试崩溃

## 1. 问题描述

Mark-Sweep GC 实现中，gc.cj 基准测试的 F5 用例（`testArrayListStress`：创建 `ArrayList<Int64>` 填入 50 万元素、迭代求和、循环 200 轮）在第 0 轮完成后触发 **SIGSEGV**，崩溃点位于 `ArrayListIterator.next()` 的 `Barrier::WriteStruct` 调用——迭代器访问的底层 `Array<Int64>` backing array 已被 GC 错误回收。

调查过程中发现 **多个叠加问题**：

| # | 问题 | 症状 | 严重度 |
|---|------|------|--------|
| 1 | `UpdateGCStats()` 未被调用 | GC 阈值始终停留在 20MB，堆增长到 256MB | 已修复 |
| 2 | `ClearMarkBitmaps` 在 STW 外执行 | 可能遗漏 mutator 持有的 region，标记位残留 | 已修复 |
| 3 | 重复根对象跳过追踪 | 57 个根中 56 个被 `MarkObject` 误判为"已标记"后跳过入栈 | 已修复 |
| 4 | **写屏障从未触发** | 并发标记期间 SATB 删除屏障零调用，活跃对象被错误 sweep | **核心问题** |

F1-F4 在前三个修复后通过，F5 仍因问题 #4 崩溃。

## 2. 环境信息

| 项目 | 值 |
|------|-----|
| 平台 | macOS aarch64（Apple Silicon） |
| GC 模式 | Mark-Sweep（`--gc-mode marksweep`） |
| 编译器 | Cangjie LLVM 编译器（`cjc`） |
| Runtime 源码 | `cangjie_runtime/runtime/` |
| 编译器源码 | `cangjie_compiler/third_party/llvm-project/` |
| SDK 路径 | `cangjie-pkg/self/dev260313/cangjie/` |
| 关键编译宏 | `MRT_USE_MARK_SWEEP` |
| GCPhase 值域 | `MS_MARK_SETUP=20, MS_CONCURRENT_MARK=21, MS_MARK_TERMINATION=22, MS_SWEEP=23` |

## 3. 问题复现

```bash
# 编译 MarkSweep runtime
cd cangjie_runtime/runtime
python3 build.py build --gc-mode marksweep

# 替换 SDK + 重签名
cp output/temp/lib/aarch64_Release/libcangjie-runtime.dylib \
   <SDK_PATH>/runtime/lib/darwin_aarch64_cjnative/libcangjie-runtime.dylib
codesign -f -s - <SDK_PATH>/runtime/lib/darwin_aarch64_cjnative/libcangjie-runtime.dylib

# 编译运行测试
source <SDK_PATH>/envsetup.sh
cd gc && cjc gc.cj -o main_f5 && ./main_f5
# → F1-F4 PASS, F5 SIGSEGV after round 0
```

崩溃栈：
```
SIGSEGV at heap address 0x1247ea5e0
→ Barrier::WriteStruct
→ ArrayListIterator.next()
→ gc.cj:239 (for (v in list) { sum += v })
```

## 4. 分析过程

### 4.1 初步观察：写屏障缓冲区始终为空

在 `MarkTermination()` STW 阶段执行 `FlushAllWriteBarrierBuffers()` 时，所有 mutator 的写屏障缓冲区 entry 数量始终为 0。对于一个有大量指针写入的 F5 用例（ArrayList 内部频繁扩容 backing array、更新指针），这是极其反常的。

正常期望：并发标记期间 mutator 修改引用字段时，SATB 删除屏障应将 **旧值**（old reference）推入写屏障缓冲区，确保旧值不被遗漏标记。缓冲区为空意味着屏障从未被实际调用。

### 4.2 诊断插桩

在 `MSWriteBarrier.cpp` 中添加原子计数器，追踪屏障慢路径的调用情况：

```cpp
// 诊断计数器
static std::atomic<uint64_t> wbCallCount{0};     // MarkAndEnqueue 总调用次数
static std::atomic<uint64_t> wbNullOrNonHeap{0}; // 旧值为 null 或非堆地址
static std::atomic<uint64_t> wbPhaseSkip{0};     // 不在标记阶段，跳过
static std::atomic<uint64_t> wbAlreadyMarked{0}; // 旧值已被标记
static std::atomic<uint64_t> wbEnqueued{0};       // 成功入队

extern "C" void MSWriteBarrier_DumpStats();      // 输出统计
extern "C" void MSWriteBarrier_ResetStats();     // 重置统计
```

在 `MarkTermination()` 的 STW 内、Flush 写屏障缓冲区前后分别调用 `DumpStats` 和 `ResetStats`。

### 4.3 诊断数据分析

诊断输出显示了关键线索：

```
Cycle 1: MarkAndEnqueue calls: 2554, nullOrNonHeap: 2554, phaseSkip: 0, alreadyMarked: 0, enqueued: 0
Cycle 2: MarkAndEnqueue calls: 0
Cycle 3: MarkAndEnqueue calls: 0
...全部后续 cycle：0 调用
```

**分析**：
- **Cycle 1**：有 2554 次慢路径调用，说明编译器快速路径在第一个 GC cycle 中正确将控制权交给了慢路径。但所有旧值均为 null（首次构造树结构时旧值确实为 null），且没有 phaseSkip——说明 mutator 确实在标记阶段中。
- **Cycle 2+**：慢路径调用次数降为 **0**。这意味着编译器快速路径（inline 在生成代码中）**始终判定"不需要调用屏障"**，直接跳过了慢路径。

### 4.4 编译器写屏障快速路径分析

编译器在 `CJBarrierLowering.cpp` 中为每个引用写操作生成 **inline 快速路径**，避免每次都调用 runtime 慢路径。该快速路径的判断逻辑如下：

```
GCPhase = GetGCPhase()           // 从 x28 高 8 位提取
if (GCPhase <= 8) {              // 8 = GC_PHASE_INIT
    直接写入（跳过屏障）          // 快速路径
} else {
    调用 MCC_WriteRefField()      // 慢路径 → MSWriteBarrier::MarkAndEnqueue()
}
```

**IR 层实现**（`CJBarrierLowering.cpp` L383-390）：

```cpp
void createGCCheck(BasicBlock *InsertBB, BasicBlock *True, BasicBlock *False) {
    IRBuilder<> IRB(InsertBB);
    CallInst *GCState = IRB.CreateCall(GCStateCheckFunc);  // GetGCPhase()
    GCState->setCallingConv(CallingConv::CangjieGC);
    // 8: related and equal to enum GCPhase::kGCPhaseInit
    auto CheckResult = IRB.CreateICmpSLE(
        GCState, Constant::getIntegerValue(GCState->getType(), APInt(32, 8)));
    IRB.CreateCondBr(CheckResult, True, False);  // <= 8 跳过, > 8 走屏障
}
```

**AArch64 汇编层实现**（`AArch64AsmPrinter.cpp` L1315-1335）：

```asm
lsr  x9, x28, #56     ; 将 x28 右移 56 位 → 提取高 8 位 = GCPhase
cmp  x9, #8            ; 与 GC_PHASE_INIT(8) 比较
b.le skip_barrier      ; 如果 <= 8，跳过写屏障
```

**关键洞察**：快速路径的 GCPhase 来源是 **x28 寄存器的高 8 位**（利用 AArch64 TBI 特性），而非从内存读取。如果 x28 未被正确刷新为当前 GCPhase，快速路径会得到过时的值并错误跳过屏障。

### 4.5 x28 寄存器 GC Phase 编码机制

AArch64 的 TBI（Top Byte Ignore）特性允许在指针的高 8 位存放元数据而不影响寻址。x28 寄存器在仓颉 runtime 中用作 **ThreadLocalData 指针**，其高 8 位编码了当前 GC Phase：

$$\text{x28} = \underbrace{\text{GCPhase}}_{\text{bits [63:56]}} \mid \underbrace{\text{ThreadLocalData*}}_{\text{bits [55:0]}}$$

编码发生在 `MRT_GetThreadLocalData()` 中（`Mutator.cpp` L28-39）：

```cpp
extern "C" uintptr_t MRT_GetThreadLocalData()
{
    uintptr_t tlDataAddr = reinterpret_cast<uintptr_t>(ThreadLocal::GetThreadLocalData());
#if defined(__aarch64__)
    if (Heap::GetHeap().IsGcStarted()) {
        tlDataAddr = tlDataAddr | (static_cast<uint64_t>(Heap::GetHeap().GetGCPhase()) << 56);
    }
#endif
    return tlDataAddr;
}
```

**x28 被刷新的场景**：

| 场景 | 代码位置 | 触发方式 |
|------|---------|---------|
| Safepoint 处理 | `HandleSafepointStub.S` | `bl _MRT_GetThreadLocalData` → 刷新 x28 |
| C→Native 返回 | `C2NStub.S` | 从 native 调用返回时刷新 x28 |
| Callee-saved 恢复 | `CalleeSavedStub.S` | `bl _MRT_GetThreadLocalData` → `mov x28, x0` |
| CJThread 上下文切换 | `CJThreadSwitch.S` | 保存/恢复 x28 到 CJThread 上下文 |

### 4.6 x28 刷新时机分析

GC Phase 转换通过 `TransitionAllMutatorsToGCPhase()` 执行（`MutatorManager.cpp` L487-525）：

```
1. GC 线程设置全局 GCPhase（SetGCPhase）
2. GC 线程为每个 Mutator 设置 SUSPENSION_FOR_GC_PHASE 标志
3. 等待每个 Mutator 到达 safepoint 并执行本地转换
4. Mutator 的 TransitionToGCPhaseExclusive() 更新 mutatorPhase 原子变量
```

**关键问题**：`TransitionToGCPhaseExclusive()`（`Mutator.cpp` L590-597）只更新了 `mutatorPhase` 原子变量，**未直接修改 x28 寄存器**：

```cpp
void Mutator::TransitionToGCPhaseExclusive(GCPhase newPhase)
{
    HandleGCPhase(newPhase);
    SetSafepointActive(false);
    ClearSuspensionFlag(SUSPENSION_FOR_GC_PHASE);
    mutatorPhase.store(newPhase, std::memory_order_release);  // ← 只更新内存变量
    // x28 寄存器未被修改！
}
```

x28 的刷新依赖于后续的 safepoint 处理（`HandleSafepointStub.S`）调用 `MRT_GetThreadLocalData()`。

### 4.7 根因定位：x28 未刷新导致快速路径永远跳过屏障

综合以上分析，写屏障不生效的完整因果链为：

```
1. [GC 线程] MarkSetup() → 设置全局 GCPhase = MS_MARK_SETUP (20)
                         → 设置 IsGcStarted = true
                         → TransitionAllMutatorsToGCPhase(MS_MARK_SETUP)

2. [Mutator]  到达 safepoint → HandleSafepoint → TransitionToGCPhaseExclusive
              → mutatorPhase = 20（内存变量已更新）
              → HandleSafepointStub.S 末尾调用 MRT_GetThreadLocalData()
              → IsGcStarted() == true → x28 高 8 位 = 20
              → ✅ 此时 x28 正确，快速路径 20 > 8，会调用慢路径

3. [GC 线程] ConcurrentMark() → 设置全局 GCPhase = MS_CONCURRENT_MARK (21)
              → TransitionAllMutatorsToGCPhase(MS_CONCURRENT_MARK)

4. [Mutator]  再次到达 safepoint → x28 刷新为 21 → ✅ 正确

5. [GC 线程] MarkTermination() → STW → 设置 GCPhase = MS_MARK_TERMINATION (22)

6. [GC 线程] PostMarkTermination() → 设置 GCPhase = MS_SWEEP (23) 或 IDLE (1)
              → IsGcStarted 最终被设为 false

7. [Mutator]  恢复执行 → 到达 safepoint → MRT_GetThreadLocalData()
              → IsGcStarted() == false → x28 高 8 位 = 0 !!!
              → 快速路径 0 <= 8 → 跳过屏障 → ✅ 这次正确（不在 GC 中）

8. [下一个 GC cycle]
   [GC 线程] MarkSetup() → IsGcStarted = true, GCPhase = 20
              → 等待 Mutator 到达 safepoint

9. [Mutator]  如果在紧密循环中（如 ArrayList 迭代），可能长时间不触发 safepoint
              → x28 高 8 位仍为 0（上次 GC 结束时的值）
              → 快速路径 0 <= 8 → 永远跳过屏障 ← ❌ 根因！
```

**根因总结**：当 Mutator 在紧密循环中运行时（如 F5 的 `for (v in list)` 迭代），它可能在 GC Phase 已转换后长时间不命中 safepoint，导致 x28 寄存器的 GC Phase 编码过时（仍为前一个 cycle 结束时的 IDLE/0 值），编译器快速路径持续判定"不需要屏障"，SATB 删除屏障完全失效。

**这也解释了为什么 Cycle 1 有 2554 次调用而 Cycle 2+ 为 0**：
- Cycle 1 是程序启动后的首次 GC，Mutator 在此之前没有 GC phase 残留，safepoint 转换正常刷新了 x28
- Cycle 1 结束后 x28 被重置为 IDLE/0，Cycle 2 的 phase 转换 handshake 虽然完成（`mutatorPhase` 内存变量已更新），但 x28 寄存器可能未在所有写操作点前被刷新

## 5. 已实施的修复

### 5.1 修复 #1：补充缺失的 UpdateGCStats() 调用

**问题**：`MarkSweepCollector::RunGarbageCollection()` 在调用 `PostGarbageCollection()` 前未调用 `UpdateGCStats()`，导致 GC 阈值始终停留在初始值 20MB，堆在两次 GC 之间膨胀到 256MB 才触发下一次回收。

**对比**：CopyGC 的 `CopyCollector::RunGarbageCollection()` 正确调用了 `UpdateGCStats()`。

**修改文件**：`src/Heap/Collector/MarkSweepCollector.cpp`

```diff
 PreGarbageCollection(/* isConcurrent */ true);
 DoGarbageCollection(gcIndex);
+UpdateGCStats();
 PostGarbageCollection(gcIndex);
```

**效果**：GC 阈值随堆使用量动态调整，F1-F4 不再因 OOM/StackOverflow 退出。

### 5.2 修复 #2：ClearMarkBitmaps 移入 STW 内

**问题**：`ClearMarkBitmaps` 原本在 STW 结束后执行。在 STW 结束到 ClearMarkBitmaps 完成之间，Mutator 可能已获取新 region 并进行分配，这些 region 的标记位不会被清除，导致残留标记。

**修改文件**：`src/Heap/Collector/MarkSweepCollector.cpp`

```diff
 {
     ScopedStopTheWorld stw("MS FlushTLRegions + ClearBitmaps");
     FlushAllAllocBuffers();
+    // 清除所有 region 的 markBitmap（必须在 STW 内执行）
+    if (centralPool != nullptr) {
+        centralPool->VisitAllRegions([&clearedRegions](MSRegionInfo* region) {
+            size_t wordCount = (region->totalSlotCount + 63) / 64;
+            if (region->markBitmap != nullptr && wordCount > 0) {
+                memset(region->markBitmap, 0, wordCount * sizeof(uint64_t));
+            }
+            clearedRegions++;
+        });
+    }
 }
-// ClearMarkBitmaps 原在此处执行（STW 外）
```

**效果**：消除了 STW 结束到清除位图之间的竞态窗口。

### 5.3 修复 #3：根对象无条件入栈

**问题**：`EnumAndMarkRoots()` 中根据 `MarkObject()` 返回值决定是否将根对象加入工作栈。由于根枚举可能返回重复根（57 个唯一根中有 16 个重复），第一次出现时调用 `MarkObject()` 标记成功，之后的重复出现返回"已标记"后被跳过入栈——但更严重的是，GC handshake 阶段每个 Mutator 也会扫描自己的栈根并标记，导致 GC 线程的全局 `EnumAndMarkRoots()` 看到大部分根已被标记，进而跳过入栈。

**修改文件**：`src/Heap/Collector/MarkSweepCollector.cpp`

```diff
 for (BaseObject* obj : roots) {
     if (obj != nullptr && Heap::IsHeapAddress(reinterpret_cast<MAddress>(obj))) {
-        if (!MarkObject(obj)) {
-            workStack.push_back(obj);
-        }
+        // Always mark and always push — ensure all roots are traced
+        bool alreadyMarked = MarkObject(obj);
+        if (!alreadyMarked) {
+            markedObjectCount.fetch_add(1, std::memory_order_relaxed);
+        }
+        workStack.push_back(obj);  // 无条件入栈
     }
 }
```

**效果**：标记对象从 ~400 增加到 ~1558，根可达的对象图被完整追踪。

## 6. 待修复的核心问题：写屏障不生效

### 6.1 问题本质

编译器生成的写屏障快速路径依赖 x28 寄存器高 8 位中的 GC Phase 值。当值 ≤ 8 时，快速路径直接执行写入（跳过屏障）；当值 > 8 时，调用 runtime 慢路径执行 SATB 删除屏障。

x28 的 GC Phase 编码仅在以下代码路径中刷新：
1. **HandleSafepointStub.S** — Mutator 命中 safepoint 时
2. **C2NStub.S / N2CStub.S** — C→Native / Native→C 调用边界
3. **CJThreadSwitch.S** — CJThread 协程切换时保存/恢复

这些刷新点的共同特征是它们都会调用 `MRT_GetThreadLocalData()` 获取最新的 Phase 编码值。但 `MRT_GetThreadLocalData()` 内部有一个 `IsGcStarted()` 保护：

```cpp
if (Heap::GetHeap().IsGcStarted()) {
    tlDataAddr |= (GCPhase << 56);
}
```

如果 Mutator 在一个没有 safepoint 的紧密循环中运行（如 ArrayList 迭代），它可能在 GC 进入并发标记阶段后仍然使用旧的 x28 值，导致写屏障被永久跳过。

**结果**：并发标记期间 Mutator 修改引用字段时，旧值未被 SATB 屏障捕获。如果旧值指向的对象是并发标记的唯一到达路径，该对象将被错误标记为"未标记"，在 sweep 阶段被回收，导致悬挂指针和 SIGSEGV。

### 6.2 修复方向

**方向 A：确保 Phase 转换 handshake 同步刷新 x28**

在 `TransitionToGCPhaseExclusive()` 中，不仅更新 `mutatorPhase` 内存变量，还通过内联汇编或调用 `MRT_GetThreadLocalData()` 刷新 x28 寄存器：

```cpp
void Mutator::TransitionToGCPhaseExclusive(GCPhase newPhase)
{
    HandleGCPhase(newPhase);
    SetSafepointActive(false);
    ClearSuspensionFlag(SUSPENSION_FOR_GC_PHASE);
    mutatorPhase.store(newPhase, std::memory_order_release);
    // TODO: 在此处刷新 x28 高 8 位为 newPhase
}
```

但这需要注意：`TransitionToGCPhaseExclusive` 可能由 GC 线程代为执行（当 mutator 在 safe region 中时），此时修改 x28 无效（因为是 wrong thread）。

**方向 B：在 GC Phase 转换的 STW 恢复 (StartTheWorld) 之前刷新所有 Mutator 的 x28**

在 `ScopedStopTheWorld` 析构时（即 `StartTheWorld` 时），调用每个 Mutator 的 TLS 更新逻辑，确保它们恢复执行时 x28 已经包含最新 Phase。这需要在 Mutator 的 Safepoint suspension → resume 路径中确保 x28 刷新。

**方向 C：修改编译器快速路径策略**

对 MS GC 模式，不依赖 x28 高 8 位，改为从内存加载 GCPhase。这会增加每次写屏障检查的开销（一次额外内存读取），但消除了 x28 一致性问题。

**方向 D：确保 Mutator 在 GC Phase 转换后首次恢复时一定经过 safepoint**

这已经是 handshake 机制的设计意图。需要验证的是：handshake 完成后 Mutator 的首次恢复是否一定经过 `HandleSafepointStub.S`（它会调用 `MRT_GetThreadLocalData` 刷新 x28）。如果 handshake 通过 safe region 转换完成（GC 线程代执行），mutator 恢复后可能不经过 safepoint stubb。

## 7. 验证结果

### 当前状态（修复 #1-#3 后）

| 测试用例 | 描述 | 结果 |
|---------|------|------|
| F1 TreeIntegrity | 二叉树构建/销毁 depth=18, 200轮 | ✅ PASS |
| F2 Generational | 多代对象创建 | ✅ PASS |
| F3 LargeObject | 大对象分配 | ✅ PASS |
| F4 LinkedList | 链表构建/遍历 | ✅ PASS |
| F5 ArrayListStress | ArrayList<Int64> 50万元素×200轮 | ❌ SIGSEGV (round 0 后) |

### 诊断数据（F5 崩溃运行）

写屏障诊断计数（跨所有 GC cycle）：

```
几乎所有 cycle: MarkAndEnqueue calls: 0（快速路径完全跳过慢路径）
仅 1 个 cycle: calls: 72, 全部 nullOrNonHeap
```

Sweep 统计：

```
每个 cycle 释放 3582 个 region（~243-262MB）
大对象 Tier 1 回收量 243-262MB/cycle
```

## 8. 经验总结

### 调试方法论

1. **分层定位**：当多个问题叠加时（GC 阈值、位图清除时序、根标记、写屏障），逐层修复并独立验证。每修复一个问题都重新运行全部测试，避免将不同问题的症状混淆。

2. **诊断插桩的价值**：通过在 `MSWriteBarrier::MarkAndEnqueue()` 中添加原子计数器，精确区分了"屏障被调用但旧值为 null"和"屏障完全未被调用"两种情况，直接指向了编译器快速路径问题。

3. **跨层分析**：问题的根因跨越了 runtime（x28 刷新机制）和编译器（快速路径条件判断）两层。单独看任一层都无法发现完整的因果链。

4. **TBI 机制的陷阱**：利用 AArch64 TBI 特性在寄存器中嵌入元数据是高效的优化，但引入了**寄存器值与全局状态不一致**的风险。任何改变全局 GC Phase 的操作都必须确保所有 Mutator 的 x28 被同步更新。

### 涉及的关键文件

| 文件 | 层级 | 作用 |
|------|------|------|
| `CJBarrierLowering.cpp` | 编译器 | 生成写屏障快速路径（比较 x28>>56 与 8） |
| `AArch64AsmPrinter.cpp` | 编译器 | 发射 `lsr x9, x28, #56` 汇编指令 |
| `Mutator.cpp` | Runtime | `MRT_GetThreadLocalData()` 编码 GCPhase 到 x28 |
| `MutatorManager.cpp` | Runtime | `TransitionAllMutatorsToGCPhase()` Phase 广播 |
| `Mutator.inline.h` | Runtime | `TransitionToGCPhaseExclusive()` 本地 Phase 更新 |
| `HandleSafepointStub.S` | Runtime/ASM | safepoint 刷新 x28 |
| `MSWriteBarrier.cpp` | Runtime | SATB 删除屏障慢路径 |
| `MarkSweepCollector.cpp` | Runtime | MS GC 主流程 |
| `CollectorResources.h` | Runtime | `IsGcStarted()` 原子标志 |
| `CollectorProxy.cpp` | Runtime | `SetGcStarted(true)` 触发点 |

## 附录 A：GCPhase 枚举定义

```cpp
enum GCPhase : uint8_t {
    GC_PHASE_UNDEF = 0,
    GC_PHASE_IDLE = 1,
    GC_PHASE_FINISH = 2,
    GC_PHASE_RECLAIM_SATB_NODE = 3,
    GC_PHASE_INIT = 8,

    // CopyGC phases (enum value > 8 需要屏障)
    GC_PHASE_ENUM = 9,
    GC_PHASE_TRACE = 10,
    GC_PHASE_CLEAR_SATB_BUFFER = 11,
    GC_PHASE_POST_TRACE = 12,
    GC_PHASE_PREFORWARD = 13,
    GC_PHASE_FORWARD = 14,

    // Mark-Sweep GC phases (>= 20，避免与 CopyGC 冲突)
    GC_PHASE_MS_MARK_SETUP       = 20,  // STW: 启用写屏障，扫描根
    GC_PHASE_MS_CONCURRENT_MARK  = 21,  // 并发标记
    GC_PHASE_MS_MARK_TERMINATION = 22,  // STW: 完成标记，刷新屏障缓冲区
    GC_PHASE_MS_SWEEP            = 23,  // 并发 sweep
};
```

编译器快速路径阈值：**8**（`GC_PHASE_INIT`）。所有 MS Phase 值 ≥ 20，满足 `> 8` 条件——前提是 x28 高 8 位包含正确的值。

## 附录 B：编译器写屏障条件编译选项

| 开关 | 作用 | 默认值 | 定义位置 |
|------|------|--------|---------|
| `EnableGCPhase` | 写屏障快速路径（检查 x28 高 8 位 GC Phase） | `true` | `CJBarrierLowering.cpp` L46 |
| `EnableTaggedPointer` | 读屏障快速路径（检查 tagged pointer） | `true` | `CJBarrierLowering.cpp` L45 |
| `EnableGCFastPath` | 对象分配快速路径（NewObj → NewObjFast） | `false` | `CJBarrierLowering.cpp` L47 |
| `EnableGCStateLoop` | 循环中写屏障优化 | `false` | `CJBarrierLowering.cpp` L48 |
