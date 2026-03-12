## Step 3: 混合写屏障

> 设计文档: `doc/Step3_混合写屏障详细设计.md`
>
> **目标**: 替代 CopyGC 的 SATB + 6 套读写屏障，实现无读屏障并发标记。

### 屏障总览

MS-GC 仅使用 **2 个** Barrier（对比 CopyGC 6 个），按 GC Phase 切换：

| Barrier 类 | 活跃阶段 | 读操作 | 写操作 | 用途 |
|---|---|---|---|---|
| `MSIdleBarrier` | IDLE, MS_SWEEP | 直接 load（零开销） | 直接 store（零开销） | GC 非活跃期，无需任何屏障保护 |
| `MSWriteBarrier` | MS_MARK_SETUP, MS_CONCURRENT_MARK, MS_MARK_TERMINATION | 直接 load（零开销） | **shade(old) + store** | 并发标记期，SATB 删除屏障 |

**Phase → Barrier 映射**（`Heap.cpp` `InstallBarrier`）：

```
GC_PHASE_IDLE                    → msIdleBarrier
GC_PHASE_MS_MARK_SETUP           → msWriteBarrier
GC_PHASE_MS_CONCURRENT_MARK      → msWriteBarrier
GC_PHASE_MS_MARK_TERMINATION     → msWriteBarrier
GC_PHASE_MS_SWEEP                → msIdleBarrier
```

#### 设计 vs 实现差异（重要）

头文件注释声明的是 **Go 风格混合屏障**（shade(old) + shade(new)），但 **实际实现是纯 SATB 删除屏障**——仅 shade(old)，不 shade(new)。

- **实际行为**（`MSWriteBarrier.cpp` L90-93 注释）：
  > "Writes: pure SATB deletion barrier — shade(old) + store. New references are NOT shaded here; newly allocated objects are marked at allocation time (mark-at-alloc)."
- **原因**: 新对象在分配时已被标记（mark-at-alloc），无需插入屏障兜底。
- **影响**: 简化了屏障实现，降低了写路径开销，但依赖分配器在标记期正确标记新对象。

#### Review 重点

- 确认所有写路径都对 old 引用执行了 `MarkAndEnqueue`
- 确认 `MSIdleBarrier` 的所有方法确实是零开销（无多余分支）
- 注意 `CopyStructArray` 是保守实现——不做 shade，依赖 GC 线程 trace 发现

### Step 3.1: MSBarrier 实现

> 设计文档: `doc/Step3.1_MSBarrier详细实现.md`
>
> **目标**: 实现 `MSIdleBarrier`（零开销）和 `MSWriteBarrier`（SATB 删除屏障）。

#### 新增文件

| 文件 | 行数 | 说明 |
|------|------|------|
| `src/Heap/Barrier/MSIdleBarrier.h` | 175 | Header-only，GC 非活跃期的零开销 barrier — 全 inline 直接 load/store |
| `src/Heap/Barrier/MSWriteBarrier.h` | 84 | SATB 写屏障 API 声明，继承 `Barrier` 基类 |
| `src/Heap/Barrier/MSWriteBarrier.cpp` | 288 | 完整写屏障实现，覆盖所有的读/写/原子操作 |

#### MSWriteBarrier 类

**继承**: `MSWriteBarrier : public Barrier`（构造函数接收 `Collector&`）

**核心辅助方法**:

| 方法 | 可见性 | 说明 | 使用点 |
|------|--------|------|--------|
| `MarkAndEnqueue(obj, wbBuf)` | `private static` | ⭐ "shade" 操作：null 检查 → `IsHeapAddress` 检查 → phase 检查（仅 MARK_SETUP/CONCURRENT_MARK/MARK_TERMINATION 三个阶段活跃）→ `RegionSpace::MarkObject(obj)` 原子 CAS 标记 bitmap → 新标记? `wbBuf.Push(obj)` | 所有写操作的公共入口 |
| `GetWBBuf()` | `private static` | 返回当前 Mutator 的 `WriteBarrierBuffer&`，通过 `Mutator::GetMutator()->GetWriteBarrierBuffer()` | 所有写操作调用 |

**shade 流程**:
```
MarkAndEnqueue(obj, wbBuf):               MSWriteBarrier.cpp L27-45
  ├─ if obj == nullptr || !IsHeapAddress(obj) → return
  ├─ phase = Mutator::GetMutator()->GetMutatorPhase()
  ├─ if phase ∉ {MARK_SETUP, CONCURRENT_MARK, MARK_TERMINATION} → return
  ├─ newly_marked = !RegionSpace::MarkObject(obj)    // CAS bitmap, true=已标记
  └─ if newly_marked → wbBuf.Push(obj)               // 缓冲灰色对象
```

**读操作**（全部零开销直接 load）:

| 方法 | 行号 | 实现 |
|------|------|------|
| `ReadReference()` | L54 | `field.GetTargetObject()` 直接返回 |
| `ReadStaticRef()` | L60 | `field.GetTargetObject()` 直接返回 |
| `ReadWeakRef()` | L66 | `field.GetTargetObject()` 直接返回（MS-GC 无弱引用特殊处理） |
| `ReadStruct()` | L72 | `memcpy_s(dst, ...)` 直接拷贝 |
| `ReadStaticStruct()` | L80 | `memcpy_s(dst, ...)` 直接拷贝 |

**写操作**（SATB 删除屏障：shade(old) + store）:

| 方法 | 行号 | shade 策略 | 说明 |
|------|------|-----------|------|
| `WriteReference()` | L96 | `shade(old)` + store | 读取旧引用 → `MarkAndEnqueue(oldRef)` → `field.SetTargetObject(ref)` |
| `WriteStaticRef()` | L108 | `shade(old)` + store | 同上，面向静态字段 |
| `WriteStruct()` | L120 | `ForEachRefInStruct shade(old)` + memcpy | 遍历 struct 所有引用字段，逐个 shade → 整体 memcpy 覆写 |
| `WriteStaticStruct()` | L147 | `ForEachBitmapWord shade(old)` + memcpy | 通过 GCTib bitmap 遍历静态 struct 引用字段 → shade → memcpy |
| `CopyRefArray()` | L178 | 循环 `shade(old)` 所有 dst 引用 + memmove | 先遍历目标数组 shade 所有旧引用 → memmove 覆写 |
| `CopyStructArray()` | L206 | **无 shade**（保守） | 无 per-element GCTib，直接 memmove。依赖 GC 线程 trace 阶段发现引用 |
| `AtomicWriteReference()` | L237 | `shade(old)` + atomic store | 读旧值 → shade → `field.SetTargetObject(ref, order)` |
| `AtomicSwapReference()` | L251 | atomic exchange + `shade(old)` | 先 `field.Exchange(newRef)` → shade 被换出的旧引用 |
| `CompareAndSwapReference()` | L271 | CAS 成功后 `shade(old)` | `field.CompareExchange(...)` → 成功? shade 旧引用; 失败? 无操作 |

#### MSIdleBarrier 类

**继承**: `MSIdleBarrier : public Barrier`（构造函数接收 `Collector&`）

**特征**: Header-only（全 inline），全部方法零开销。在 `GC_PHASE_IDLE` 和 `GC_PHASE_MS_SWEEP` 阶段安装。

| 方法类别 | 实现方式 | 说明 |
|----------|---------|------|
| 所有读方法 (`ReadReference`, `ReadStaticRef`, `ReadWeakRef`, `ReadStruct`, `ReadStaticStruct`) | 直接 `field.GetTargetObject()` / `memcpy_s` | 零开销 |
| 所有写方法 (`WriteReference`, `WriteStaticRef`, `WriteStruct`, `WriteStaticStruct`) | 直接 `field.SetTargetObject(ref)` / `memcpy_s` | 零开销，无 shade |
| 数组拷贝 (`CopyRefArray`, `CopyStructArray`) | 直接 `memmove_s` | 零开销 |
| 原子操作 (`AtomicRead/Write/Swap/CAS`) | 直接原子操作 | 零开销 |
| TSAN 支持 | `Sanitizer::TsanWriteMemoryRange` / `TsanReadMemoryRange` | 在 `CANGJIE_TSAN_SUPPORT` 宏下额外调用 |

#### 小结

**做了什么**: 新增 `MSWriteBarrier`（SATB 删除屏障）和 `MSIdleBarrier`（零开销直通），替代 CopyGC 的 6 套 Barrier。`MSWriteBarrier` 对所有写操作执行 shade(old)——先读旧引用、原子标记 bitmap、推入 WBBuf；所有读操作零开销直接 load。`MSIdleBarrier` 全部方法均为 inline 直通。

**效果**:
- 写路径从 CopyGC 的「读屏障 + 写屏障 + Forward 检查」简化为「shade(old) + store」，去除了全部读屏障开销
- GC 非活跃期（IDLE/SWEEP）屏障零开销，对应用吞吐量无影响
- 两个 Barrier 类按 Phase 切换，代码路径清晰，易于 Review

**改进点**:
1. **`CopyStructArray` 无 shade**: 当前直接 memmove，不对 struct 数组元素的引用字段执行 shade。依赖 GC trace 阶段兜底发现，但如果 struct 元素在覆写后被覆盖（ABA 场景），理论上存在短暂的标记遗漏窗口。建议评估是否需要补 shade 或确认 trace 覆盖的完备性
2. **`MarkAndEnqueue` 的 phase 检查开销**: 每次 shade 都通过 `Mutator::GetMutator()->GetMutatorPhase()` 获取当前 phase，涉及 TLS 查找。可考虑将 phase 缓存在 Barrier 对象或局部变量中
3. **注释与实现不一致**: 头文件注释声明 Go 风格混合屏障（shade old + shade new），实际只有 shade(old)。应修正注释，避免误导 Reviewer

### Step 3.2: WriteBarrierBuffer

> 设计文档: `doc/Step3.2_WriteBarrierBuffer详细设计.md`
>
> **目标**: Per-Mutator 定长缓冲区，避免写屏障直接竞争 GC 线程的中心 WorkStack。

#### 新增文件

| 文件 | 行数 | 说明 |
|------|------|------|
| `src/Heap/Barrier/WriteBarrierBuffer.h` | 101 | Header-only `WriteBarrierBuffer` 类 |

#### 修改文件

| 文件 | 修改位置 | 改动要点 |
|------|----------|----------|
| `src/Mutator/Mutator.h` | L20, L398, L539 | `#include WriteBarrierBuffer.h`；添加 `wbBuf_` 成员（L539）+ `GetWriteBarrierBuffer()` 访问器（L398） |
| `src/Mutator/Mutator.cpp` | L566-588 | `#ifdef MRT_USE_MARK_SWEEP` 块: MS 各阶段的 safepoint 处理 |
| `src/Heap/Collector/MarkSweepCollector.cpp` | L445, L456 | `FlushAllWriteBarrierBuffers()` 和 `ClearAllWriteBarrierBuffers()` 实现 |

#### WriteBarrierBuffer 类

**定位**: Per-Mutator 的灰色对象缓冲区，减少 GC 线程 WorkStack 锁竞争。

**属性**:

| 属性 | 类型 | 说明 |
|------|------|------|
| `kCapacity` | `static constexpr size_t = 256` | 定长容量：256 slot = 2KB（64-bit） |
| `buf_[kCapacity]` | `BaseObject*[]` | 定长数组，存储写屏障 shade 出的灰色对象 |
| `count_` | `size_t` | 当前已使用 slot 数（0-256） |

**方法**:

| 方法 | 复杂度 | 说明 | 调用点 |
|------|--------|------|--------|
| `Push(obj)` | O(1) | 追加灰色对象。**满时静默丢弃**——对象已被 `MarkObject` 标记在 bitmap，GC 线程在 MarkTermination 的 bitmap 扫描中会发现它 | `MSWriteBarrier::MarkAndEnqueue()` 中新标记对象后调用 |
| `Flush(Container&)` | O(n) | 模板方法。遍历缓冲区，将非 null 对象 `push_back` 到调用方容器（通常是 `WorkStack`），然后重置 `count_=0` | `FlushAllWriteBarrierBuffers()` → `mutator.GetWriteBarrierBuffer().Flush(mainQueue)` |
| `Clear()` | O(1) | 仅重置 `count_=0`（不清零数组） | `ClearAllWriteBarrierBuffers()` |
| `Count()` | O(1) | 返回 `count_` | 调试/断言 |
| `IsEmpty()` | O(1) | 返回 `count_ == 0` | 调试/断言 |

**线程安全**:
- `Push`: 仅由拥有者 Mutator 线程调用（单写者），无需锁
- `Flush` / `Clear`: 在 STW 下调用（MarkTermination/PostMark），或由拥有者 Mutator 自行调用

#### 生命周期

```
Mutator 创建
  └─ wbBuf_ 随 Mutator 构造初始化（count_=0, buf_全 null）

GC 标记期（MARK_SETUP / CONCURRENT_MARK / MARK_TERMINATION）
  └─ 每次写操作触发 MSWriteBarrier
      └─ MarkAndEnqueue(oldRef, wbBuf_)
          └─ 新标记? → wbBuf_.Push(oldRef)      // 缓冲灰色对象

MarkTermination（STW）
  └─ MarkSweepCollector::RunMarkTermination()
      └─ FlushAllWriteBarrierBuffers(mainQueue)     // L398/L436
          └─ MutatorManager::VisitAllMutators(func)
              └─ 每个 mutator.GetWriteBarrierBuffer().Flush(mainQueue)
                  └─ 灰色对象全部汇入 GC 线程的 mainQueue

PostMarkTermination（Handshake）
  └─ MarkSweepCollector::RunPostMarkTermination()
      └─ ClearAllWriteBarrierBuffers()              // L543
          └─ MutatorManager::VisitAllMutators(func)
              └─ 每个 mutator.GetWriteBarrierBuffer().Clear()

Mutator 销毁
  └─ wbBuf_ 随 Mutator 析构自动销毁（栈对象，无堆分配）
```

#### 关键设计决策

1. **满时丢弃而非阻塞**: `Push` 满时不 flush 也不阻塞，因为对象已被 `MarkObject` 原子标记在 bitmap。GC 线程在 MarkTermination 的 `DrainWorkStack` 中通过 bitmap 扫描能发现所有已标记对象。丢弃只影响发现延迟，不影响正确性。
2. **集中 Flush 而非 Mutator 自行 Flush**: 设计文档曾提到 Mutator 在 safepoint 时自行 flush。但实际实现中，`Mutator.cpp` L575-578 的 `MS_MARK_TERMINATION` 分支注释明确说"flush is done centrally by the collector under STW"，Mutator 不做操作。
3. **固定大小**: 256 slot 是固定值，不可配置。实测中 2KB 的开销可接受，避免了动态扩容的复杂性。

#### 小结

**做了什么**: 为每个 Mutator 创建 256 slot 定长 `WriteBarrierBuffer`，写屏障 shade 出的灰色对象先缓冲在本地，在 MarkTermination STW 阶段集中 Flush 到 GC 线程的 WorkStack。

**效果**:
- 消除了写屏障与 GC 线程 WorkStack 之间的锁竞争——Mutator 写入 WBBuf 无锁（单写者）
- 满时静默丢弃保证了写屏障路径无阻塞，不影响 Mutator 吞吐量
- 正确性由 bitmap 保底——即使 WBBuf 丢弃，已标记对象在 MarkTermination 的 bitmap 扫描中仍可发现

**改进点**:
1. **满时丢弃的收敛影响**: WBBuf 满时丢弃意味着灰色对象不会被 push 到 WorkStack，只能在 MarkTermination 的 bitmap 全扫描时发现。如果写操作密集导致频繁溢出，会增加 MarkTermination 的 drain 工作量，延长 STW 时间。建议添加溢出计数器进行监控
2. **固定 256 容量**: 无法根据应用写操作频率调整。高频写场景下可能频繁溢出；低频写场景下浪费内存。建议支持运行时可配置或自适应扩容
3. **Flush 时机的设计偏差**: 设计文档建议 Mutator 在 safepoint 自行 Flush，实际由 Collector STW 集中 Flush。后者更简单但要求 STW 期间遍历所有 Mutator 的 WBBuf，在 Mutator 数量多时增加 STW 耗时

### Step 3.3: 读屏障移除

> 设计文档: `doc/Step3.3_读屏障与TaggedPointer移除.md`
>
> **目标**: MS-GC 下对象不移动，读屏障和 Tagged Pointer 均无需存在，全部降级为直接 load。

#### 为什么可以移除

CopyGC 需要读屏障的原因是对象可能被移动（FromSpace → ToSpace），读屏障负责"自愈"（self-healing）——将旧地址重定向到新地址。MS-GC 对象地址终生不变，因此：

- **无 FromSpace / ToSpace**: 不需要判断对象是否在 FromSpace
- **无 Forward Pointer**: 不需要转发指针
- **无 Tagged Pointer**: 不需要对象指针标记位

#### 移除后的读屏障实现

两个 Barrier 的所有读操作均为零开销直接 load：

| 操作 | MSWriteBarrier 实现 | MSIdleBarrier 实现 |
|------|---------------------|---------------------|
| `ReadReference()` | `field.GetTargetObject()` 直接返回 | 同左 |
| `ReadStaticRef()` | `field.GetTargetObject()` 直接返回 | 同左 |
| `ReadWeakRef()` | `field.GetTargetObject()` 直接返回 | 同左 |
| `ReadStruct()` | `memcpy_s` 直接拷贝 | 同左 |
| `ReadStaticStruct()` | `memcpy_s` 直接拷贝 | 同左 |
| `AtomicReadReference()` | `field.GetFieldValue(order)` 直接返回 | 同左 |

#### MarkSweepCollector 的 CopyGC Override Stubs

`MarkSweepCollector` 需要覆盖 `Collector` 基类中 CopyGC 专用的方法，全部返回 false/空操作：

```cpp
// MarkSweepCollector.h L92-101
bool IsFromObject(BaseObject*) const override { return false; }
bool IsGhostFromObject(BaseObject*) const override { return false; }
bool TryUpdateRefField(...) const override { return false; }
bool TryForwardRefField(...) const override { return false; }
bool TryUntagRefField(...) const override { return false; }
bool TryTagRefField(...) const override { return false; }
```

这些 stub 确保如果有任何残留的 CopyGC 调用路径意外触发，不会产生错误行为。

#### CompilerCalls 优化（设计预留，当前未改动）

以下 CompilerCalls 当前仍通过 Barrier 虚函数调用，未来编译器可直接内联 load 以消除函数调用开销：
- `CJ_MCC_ReadRefField` / `CJ_MCC_ReadWeakRef` / `MCC_AtomicReadReference`

#### Review 重点

- 确认 `MSWriteBarrier` 和 `MSIdleBarrier` 的所有 Read 方法均为直接 load，无分支判断
- 确认 `MarkSweepCollector` 的 Forward/Tag/Update 方法全部返回 false
- 注意 `ReadWeakRef` 当前与 `ReadReference` 实现相同——MS-GC 暂无弱引用特殊处理

#### 小结

**做了什么**: 移除 CopyGC 的读屏障和 Tagged Pointer 机制。两个 Barrier 的所有读操作降级为直接 load/memcpy。`MarkSweepCollector` 覆盖基类的 Forward/Tag/Update 等 CopyGC 专用方法，全部返回 false（安全 stub）。

**效果**:
- 读操作零额外开销——每次引用读取从「虚函数调用 + FromSpace 检查 + Forward 重定向」降级为单条 load 指令
- CopyGC 残留调用路径通过 stub 方法安全兜底，避免误触发对象搬迁逻辑
- 为编译器进一步优化（内联 load 取代 CompilerCalls 虚函数调用）留出空间

**改进点**:
1. **CompilerCalls 仍走虚函数**: `CJ_MCC_ReadRefField` 等读路径当前仍通过 Barrier 虚函数调用，有函数调用开销。编译器后续可直接生成 load 指令，彻底消除调用开销
2. **`ReadWeakRef` 无特殊处理**: 当前与 `ReadReference` 完全相同。如果后续需要弱引用清除语义（如 WeakRef 在 GC 后返回 null），需要在此处添加 `isMarked` 检查
3. **Stub 方法的防御性**: 6 个 override stub 只返回 false，不打日志或触发断言。建议在 Debug 构建下添加 `MRT_ASSERT(false, "unexpected CopyGC call path")` 以尽早发现残留调用

### Step 3.4: 对 Step 2 子阶段的改造

> 设计文档: `doc/Step3.4_对Step2子阶段的改造.md`
>
> **目标**: 在 Step 2 的四个子阶段中集成写屏障和 WriteBarrierBuffer。

#### 改造总览

| 子阶段 | 原方案 (SATB Buffer) | 现方案 (WBBuf) | 改造是否已实现 |
|--------|---------------------|----------------|--------------|
| **MarkSetup** | 安装 `MSTraceBarrier` + retire SATB buf | 安装 `MSWriteBarrier` | ✅ 已实现 |
| **ConcurrentMark** | SATB 缓冲区 → per-P 队列处理 | WBBuf.Push() 缓冲灰色对象 | ✅ 已实现 |
| **MarkTermination** | `CollectSatbCandidatesToMain()` drain | `FlushAllWriteBarrierBuffers()` drain | ✅ 已实现 |
| **PostMarkTermination** | 清空 SATB buffer | `ClearAllWriteBarrierBuffers()` | ✅ 已实现 |

#### 各子阶段详细改造

**Step 2.1 MarkSetup（Handshake）**:
- **Barrier 切换**: `Heap::InstallBarrier()` 将 `currentBarrier` 从 `msIdleBarrier` 切换到 `msWriteBarrier`
- **Mutator 侧**（`Mutator.cpp` L568-574）: 每个 Mutator 在 safepoint 执行 `GcPhaseEnum(newPhase)` 扫描栈根
- **效果**: Handshake 完成后，所有 Mutator 的写操作都受 SATB 删除屏障保护

**Step 2.2 ConcurrentMark（Mutator 并发运行）**:
- **Barrier 状态**: 保持 `msWriteBarrier`
- **Mutator 侧**（`Mutator.cpp` L581-583）: 无特殊操作。SATB 屏障已由 MarkSetup 安装
- **写屏障行为**: 每次引用字段覆写 → `shade(old)` → 新标记的对象进入 `wbBuf_`
- **GC 线程**: 持续从 `mainQueue` drain 灰色对象，trace 子引用

**Step 2.3 MarkTermination（STW）**:
- **Barrier 状态**: 保持 `msWriteBarrier`（STW 期间 Mutator 已暂停，屏障不触发）
- **Flush WBBuf**: `FlushAllWriteBarrierBuffers(mainQueue)`（`MarkSweepCollector.cpp` L398, L436）
  ```cpp
  void MarkSweepCollector::FlushAllWriteBarrierBuffers(WorkStack& mainQueue)
  {
      auto func = [&mainQueue](Mutator& mutator) {
          mutator.GetWriteBarrierBuffer().Flush(mainQueue);
      };
      MutatorManager::Instance().VisitAllMutators(func);
  }
  ```
- **调用两次**: L398（第一次 drain 前）和 L436（第二次 drain 前），确保 WBBuf 中延迟的灰色对象全部被处理
- **Mutator 侧**（`Mutator.cpp` L575-578）: 无操作。注释："flush is done centrally by the collector under STW"

**Step 2.4 PostMarkTermination（Handshake）**:
- **Barrier 切换**: `Heap::InstallBarrier()` 将 `currentBarrier` 切换回 `msIdleBarrier`
- **Clear WBBuf**: `ClearAllWriteBarrierBuffers()`（`MarkSweepCollector.cpp` L543）
  ```cpp
  void MarkSweepCollector::ClearAllWriteBarrierBuffers()
  {
      auto func = [](Mutator& mutator) {
          mutator.GetWriteBarrierBuffer().Clear();
      };
      MutatorManager::Instance().VisitAllMutators(func);
  }
  ```
- **Mutator 侧**（`Mutator.cpp` L585-587）: 无操作。Barrier 已切换到 idle，后续写操作零开销

#### Phase 状态机与 Barrier 切换时序

```
IDLE                        msIdleBarrier（零开销）
  │
  ▼ GC 触发
MS_MARK_SETUP ──────────── → msWriteBarrier（SATB 删除屏障）
  │  Handshake: 每个 Mutator 扫描栈根
  ▼
MS_CONCURRENT_MARK ─────── → msWriteBarrier（保持）
  │  Mutator 并发运行, shade(old) 写入 WBBuf
  ▼
MS_MARK_TERMINATION ────── → msWriteBarrier（保持，但 Mutator 已 STW）
  │  FlushAllWBBuf → DrainWorkStack → FlushAllWBBuf → DrainWorkStack
  ▼
MS_SWEEP ───────────────── → msIdleBarrier（零开销）
  │  ClearAllWBBuf; Barrier 退出标记模式
  ▼
IDLE                        msIdleBarrier（零开销）
```

#### Review 重点

- 确认 `FlushAllWriteBarrierBuffers` 在 MarkTermination 中被调用了两次（L398 + L436），覆盖了两轮 drain
- 确认 `ClearAllWriteBarrierBuffers` 在 PostMark 中调用（L543），而非在 MarkTermination 中——避免过早清空
- 注意 Mutator safepoint 中 `MS_MARK_TERMINATION` 分支不做 flush —— 与设计文档("Mutator flush WBBuf at safepoint") 有差异，实际由 Collector 集中处理

#### 小结

**做了什么**: 在 Step 2 的四个 GC 子阶段中集成写屏障和 WriteBarrierBuffer。MarkSetup 安装 `MSWriteBarrier`；ConcurrentMark 期间写屏障持续 shade(old) 写入 WBBuf；MarkTermination STW 下 Collector 集中 Flush WBBuf 两次（覆盖两轮 drain）；PostMark 切回 `MSIdleBarrier` 并清空 WBBuf。

**效果**:
- 写屏障生命周期与 GC Phase 精确对齐——仅在标记期活跃，其余时间零开销
- 两次 Flush（L398 + L436）确保 WBBuf 中的灰色对象不会遗漏，保证标记完备性
- Phase 状态机清晰：IDLE → MARK_SETUP(安装) → CONCURRENT_MARK(保持) → MARK_TERMINATION(flush) → SWEEP(卸载+clear) → IDLE

**改进点**:
1. **Flush 集中在 STW**: 所有 Mutator 的 WBBuf 在 STW 下由 Collector 遍历 Flush。Mutator 数量多时（数百个线程），`VisitAllMutators` 的遍历成本可感知。可考虑让 Mutator 在进入 safepoint 时自行 Flush（如设计文档所述），减少 STW 期间的集中开销
2. **两次 Flush 的效率**: MarkTermination 中 Flush → Drain → Flush → Drain，第二次 Flush 是为了捕获第一次 Drain 过程中可能发现的新 WBBuf 内容。但 STW 期间 Mutator 已暂停，第二次 Flush 理论上应为空。建议添加断言或统计验证
3. **设计文档同步**: 设计文档描述 Mutator 在 safepoint 自行 Flush，实际由 Collector 集中处理。两处不一致，应更新设计文档以反映实际实现

---
