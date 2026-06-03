# Step 17：Barrier / Assist / GC 线程 / GlobalBuf 整体设计说明

> 2026-05-19 整理  
> 涵盖：WriteBarrierBuffer → FlushWriteBarrierOverflow → globalFull_ 管道；  
> PerPGCWork（wbuf1_/wbuf2_ 分离）；TransferRootsToGCWork 批量推；  
> 以及本轮迭代过程中发现并修复的三个设计问题。

---

## 目录

1. [整体管道架构总览](#1-整体管道架构总览)
2. [WriteBarrierBuffer：第一级屏障缓冲](#2-writebarrierbuffer第一级屏障缓冲)
3. [FlushWriteBarrierOverflow：溢出批量推 globalFull_](#3-flushwritebarrieroverflow溢出批量推-globalfull_)
4. [Root 扫描路径：TransferRootsToGCWork](#4-root-扫描路径transferrootstogcwork)
5. [globalFull_ 与 workBufPool_：核心共享队列](#5-globalfull_-与-workbufpool_核心共享队列)
6. [PerPGCWork：get/put 分离的本地缓冲](#6-pergcworkgetput-分离的本地缓冲)
7. [GC 线程消费路径：DrainGCWork](#7-gc-线程消费路径draingcwork)
8. [Assist 路径：MSMarkAssist 与 per-mutator gcWork_](#8-assist-路径msmarkassist-与-per-mutator-gcwork_)
9. [三个设计问题与修复记录](#9-三个设计问题与修复记录)
10. [设计原则总结](#10-设计原则总结)

---

## 1. 整体管道架构总览

MSGC 的标记工作传递整体是一条**生产者→共享队列→消费者**管道：

```
生产侧（发现灰色对象）
  ├─ 写屏障（mutator 更新引用字段时触发）
  │     WriteBarrierBuffer[256]（per-mutator 内联数组）
  │         满 → FlushWriteBarrierOverflow → WorkBuf → globalFull_.Push()
  │
  └─ Root 扫描（ConcurrentMark 开始前一次性）
        markWorkStack_（std::vector，Root 枚举阶段填充）
            → TransferRootsToGCWork → WorkBuf → globalFull_.Push()

共享队列
  globalFull_（LFStack<WorkBuf*>，mutex保护的 LIFO Stack）
  workBufPool_（WorkBuf 的 free-list 复用池）

消费侧（标记子对象，再次写入 put-buf 后溢出回 globalFull_）
  ├─ GC 专用线程
  │     gcThreadWork_（PerPGCWork）
  │         TryGetObj → 取对象 → TraceObjectRefFieldsToGCWork → PutObj
  │
  └─ 各 Mutator Assist 线程（并发标记阶段，随分配触发）
        AllocBufferMS::gcWork_（PerPGCWork，per-mutator）
            TryGetObj → 取对象 → TraceObjectRefFieldsToGCWork → PutObj
```

**关键约定**：
- **所有**发现灰色对象的路径，最终都以"WorkBuf 整包"的形式推入 `globalFull_`，不存在"单个对象直接写入共享队列"的路径。
- **所有**消费路径都通过 `PerPGCWork.TryGetObj()` 取对象；本地 buf 耗尽后才向 `globalFull_` 补货，一次补一整包（256 个对象）。
- 子对象写入 `PerPGCWork.PutObj()` → put-buf（`wbuf2_`）；put-buf 满时整包推回 `globalFull_`。
- `globalFull_` 是 GC 线程与所有 assist 线程的**唯一**共享工作仓库，收敛检查 `gcThreadWork_.Empty() && globalFull_.Empty()` 即可覆盖全体工作。

---

## 2. WriteBarrierBuffer：第一级屏障缓冲

**文件**：`src/Heap/MBarrier/WriteBarrierBuffer.h`、`MSMarkBarrier.cpp`

WriteBarrierBuffer（WBB）是**每个 Mutator 私有**的第一级屏障缓冲，采用内联 256 个槽的定长数组（无锁，单写者）。

### 写屏障触发时（MSMarkBarrier::EnqueueRef）

```
EnqueueRef(ref)
  → 获取当前线程的 WBB
  → 如果 WBB 未满：直接写入 WBB[pos++]，返回（纯内存写，极快）
  → 如果 WBB 已满：调用 FlushWriteBarrierOverflow(WBB)
```

### WBB 的生命周期

- **STW MarkTermination** 阶段：`FlushAllWriteBarrierBuffers(mainQueue)` 强制清空所有 Mutator 的 WBB，把残余引用推入 STW `mainQueue` 做最终标记。
- 并发标记期间：WBB 满时自动溢出，STW 前不需要主动清空。

### 设计要点

WBB 之所以使用内联数组而非直接推队列，原因是：
1. 绝大多数写屏障不会导致 WBB 满（256 个槽足以吸收大量短期引用更新）。
2. 满时才做一次批量操作（push 一整包），攤销了入队的同步开销。

---

## 3. FlushWriteBarrierOverflow：溢出批量推 globalFull_

**文件**：`src/Heap/MCollector/MarkSweepCollector.cpp`  
**函数**：`MarkSweepCollector::FlushWriteBarrierOverflow(WriteBarrierBuffer& wbBuf)`

### 流程

```
wbBuf 已满（256 个引用）
  → 从 workBufPool_ 取一个空 WorkBuf
  → 遍历 wbBuf 的所有引用：
      if DiscoverObject(ref)（标记位图，首次才返回 true）
          WorkBuf.Push(ref)
          if WorkBuf.Full() → globalFull_.Push(WorkBuf)，取新 WorkBuf
  → 最终 partial WorkBuf → globalFull_.Push()（非空）或 workBufPool_.Put()（空）
  → 清空 wbBuf（pos = 0）
```

### 关键：只有"首次发现"的对象才推入 globalFull_

`DiscoverObject` 会原子地设置 markBitmap 对应 bit；已标记对象直接跳过，不重复入队。这保证了 globalFull_ 中不存在重复灰对象，消费端无需去重。

---

## 4. Root 扫描路径：TransferRootsToGCWork

**文件**：`src/Heap/MCollector/MarkSweepCollector.cpp`  
**函数**：`TransferRootsToGCWork()`，在 `RunConcurrentMarkLoop()` 开头调用。

### Root 枚举流程（EnumRoots 阶段，先于 ConcurrentMark）

```
TransitionToGCPhase(MS_MARK_SETUP, STW)
  → 每个 Mutator 在 safepoint 扫描自己的栈根 → stackRoots_[]（per-mutator）

GC 线程：EnumAndMarkRoots(markWorkStack_)
  → 全局/静态 Root → markWorkStack_（std::vector）

ConcurrentMark → MergeRootsFromAllocBuffers(markWorkStack_)
  → 归并所有 mutator 的 stackRoots_[] → markWorkStack_
```

此时 `markWorkStack_` 汇聚了**本轮 GC 所有 Root 对象**。

### TransferRootsToGCWork（当前实现，Step 17 改后）

```cpp
WorkBuf* buf = workBufPool_.Get();
while (!markWorkStack_.empty()) {
    BaseObject* obj = markWorkStack_.back(); markWorkStack_.pop_back();
    if (buf->Full()) { globalFull_.Push(buf); buf = workBufPool_.Get(); }
    buf->Push(obj);
}
if (!buf->Empty()) globalFull_.Push(buf);
else workBufPool_.Put(buf);
```

**与 FlushWriteBarrierOverflow 完全相同的模式**：批量打包 WorkBuf，整包推入 `globalFull_`。  
GC 线程通过 `TryGetObj()` 自然从 `globalFull_` 获取 Root，无需特殊"注入 GC 线程本地缓冲"的路径。

---

## 5. globalFull_ 与 workBufPool_：核心共享队列

**文件**：`src/Common/LFStack.h`

### WorkBuf

```cpp
struct WorkBuf {
    static constexpr size_t kCapacity = 256;
    alignas(64) BaseObject* objs[kCapacity];
    WorkBuf* next;  // LFStack 链表指针
    size_t len;

    bool Full() const  { return len >= kCapacity; }
    bool Empty() const { return len == 0; }
    void Push(BaseObject* obj) { objs[len++] = obj; }
    BaseObject* Pop()          { return objs[--len]; }
    void Reset()               { len = 0; next = nullptr; }
};
```

256 个对象一包，`alignas(64)` 避免 false sharing。

### LFStack（globalFull_）

```cpp
class LFStack {
    std::mutex mtx_;
    WorkBuf* top_ = nullptr;
public:
    void Push(WorkBuf* buf);  // 压栈（LIFO）
    WorkBuf* Pop();           // 弹栈（nullptr 表示空）
    bool Empty() const;
    size_t Count() const;
};
```

`globalFull_` 是 `MarkSweepCollector` 的成员，GC 线程和所有 assist 线程并发访问。使用 mutex 保护，Push/Pop 都在持有锁下完成（锁粒度 = 一个 WorkBuf 指针的链表操作，极快）。

### WorkBufPool（workBufPool_）

```cpp
class WorkBufPool {
    LFStack freeList_;
public:
    WorkBuf* Get();      // 从 freeList_ 弹出，或 new WorkBuf
    void Put(WorkBuf* buf);  // Reset 后压入 freeList_
};
```

WorkBuf 对象在整个 GC 生命周期内循环复用，避免频繁 malloc/free。

---

## 6. PerPGCWork：get/put 分离的本地缓冲

**文件**：`src/Common/LFStack.h`（struct 定义）

`PerPGCWork`（代码中结构体名为 `gcThreadWork_`）是**每个标记参与者**（GC 线程 + 每个 Mutator）的本地工作缓冲，核心是两个 WorkBuf 指针的严格分工：

| 字段 | 角色 | 操作 |
|------|------|------|
| `wbuf1_` | **get buffer**（取对象源） | 单调 drain；耗尽后晋升 wbuf2_ |
| `wbuf2_` | **put buffer**（放对象槽） | PutObj 写入；满了整包推 globalFull_ |

### PutObj（写入子对象）

```
PutObj(obj)
  → if wbuf2_->Full():
        globalFull_->Push(wbuf2_)
        wbuf2_ = pool->Get()
  → wbuf2_->Push(obj)
```

PutObj **始终写入 wbuf2_**，满时以整包推 globalFull_，永远不会写 wbuf1_。

### TryGetObj（取对象消费）

```
TryGetObj()
  1. if wbuf1_ 非空 → return wbuf1_->Pop()    ← 优先消费 get-buf
  2. if wbuf1_ 空：
       swap：pool->Put(wbuf1_)，wbuf1_ = wbuf2_，wbuf2_ = pool->Get()
       if wbuf1_ 非空 → return wbuf1_->Pop()  ← 消费刚晋升的 put-buf
  3. if 两者都空：
       wbuf1_ = globalFull_->Pop()             ← 向全局补货
       if wbuf1_ != nullptr → return wbuf1_->Pop()
  4. return nullptr  ← 全局也空，本轮无工作
```

### 为什么要 wbuf1_/wbuf2_ 分离？

**之前（未分离时的问题）**：wbuf1_ 同时作为 source 和 sink。消费端 Pop 一个对象，Trace 时把子对象 Push 回同一个 buf。若 trace 出的子对象很多，wbuf1_ 刚被 Pop 走一个就立刻重新填满，立刻被推回 globalFull_，反复在本地缓冲和全局队列之间"倒腾"，完全失去本地缓冲的摊销价值。

**分离后**：wbuf1_ 单调 drain（只出不进），wbuf2_ 单调累积（只进不出，满了才整包推出）。两者角色正交，吞吐最优。

### TryGetObjLocal（Assist 专用变体）

与 TryGetObj 相同，但跳过步骤 3（不向 globalFull_ 补货）。用于 assist 线程在完成 debt 后优雅退出，而不是把 GC 线程的工作"顺手"取走。

---

## 7. GC 线程消费路径：DrainGCWork

**文件**：`src/Heap/MCollector/MarkSweepCollector.cpp`

### RunConcurrentMarkLoop 骨架

```cpp
void MarkSweepCollector::RunConcurrentMarkLoop() {
    gcThreadWork_.Init(&globalFull_, &workBufPool_);  // 绑定全局队列
    TransferRootsToGCWork();                           // Root → globalFull_

    while (true) {
        size_t scannedCount = 0;
        DrainGCWork(gcThreadWork_, kSliceMaxObjects, scannedCount);  // 消费一片

        bgScanCredit_ += scannedCount;  // 积累 bgCredit 供 Assist 短路

        if (gcThreadWork_.Empty() && globalFull_.Empty()) {
            if (CheckConvergenceAndBreak(...)) break;  // 收敛检查
        }
    }
}
```

### DrainGCWork 内部

```cpp
void MarkSweepCollector::DrainGCWork(PerPGCWork& gcw, size_t max, size_t& count) {
    while (count < max) {
        BaseObject* obj = gcw.TryGetObj();  // 从 wbuf1_ / wbuf2_ / globalFull_ 取对象
        if (obj == nullptr) break;
        TraceObjectRefFieldsToGCWork(obj, gcw);  // 遍历 obj 的所有引用字段
        count++;
    }
}
```

### TraceObjectRefFieldsToGCWork

遍历 `obj` 的引用字段，对每个非 null 字段调用 `DiscoverObject`；首次发现则通过 `gcw.PutObj(field_obj)` 写入 wbuf2_（进而在满时溢出到 globalFull_）。这是**标记工作自我繁殖**的核心：取一个灰对象 → 生产多个新灰对象 → 最终回到 globalFull_ 供后续消费。

### 收敛条件

```
gcThreadWork_.Empty() && globalFull_.Empty()
```

两者皆空，意味着：
- GC 线程自己的 get-buf 和 put-buf 均为空（wbuf1_.Empty() && wbuf2_.Empty()）
- 全局共享队列也为空

此时所有 assist 线程的 put-buf 可能仍有残余（按设计，assist 通过 `Dispose()` 在 STW 时统一清空，并发阶段的残余通过 convergence 轮数 + per-P flush 解决）。

---

## 8. Assist 路径：MSMarkAssist 与 per-mutator gcWork_

**文件**：`src/Heap/MAllocator/MSAllocBuffer.h`（`AllocBufferMS::gcWork_`）  
**函数**：`MarkSweepCollector::MSMarkAssist(debtObjects)`

### 触发时机

每次 Mutator 分配新对象时，`AllocBufferMS::Allocate()` 检查 `gcAssistDebtScaled_`；超过阈值则调用 `MSMarkAssist`。  
debt 由 `assistWorkPerByteScaled_`（GC 线程动态更新的比率）乘以本次分配字节数计算。

### Assist 执行流程

```
MSMarkAssist(debtObjects)
  1. 尝试从 bgScanCredit_ 抵扣（原子 fetch_sub）
       → 若积分充足：直接返回，mutator 无需做额外工作  ← 短路快路径
  2. 初始化 gcWork_（若未初始化）
       gcWork_.Init(&collector->globalFull_, &collector->workBufPool_)
  3. DrainGCWork(gcWork_, debtObjects, scanned)  ← 使用 TryGetObjLocal
  4. 将 scanned 计数贡献给统计
  5. 若标记期间发现 WeakRef 等需主线程处理的对象 → PushDeferredMainThread
```

### 与 GC 线程的工作共享

Assist 线程的 `gcWork_` 和 GC 线程的 `gcThreadWork_` 使用**相同的 `globalFull_`**。Assist 消费 WorkBuf 时，TraceObjectRefFieldsToGCWork 产生的子对象 Push 到 `gcWork_.wbuf2_`，满时整包推回 `globalFull_`，GC 线程可继续消费。这是真正的**双向共享**：GC 线程和 Assist 互相生产、互相消费。

### STW MarkTermination 时的 Assist 残余清理

```
DisposeAllPerPGCWork()
  → 遍历所有 mutator 的 gcWork_
  → gcWork_.Dispose()：把 wbuf1_ 和 wbuf2_ 都推入 globalFull_
DrainGlobalLFStackToMainQueue(mainQueue)
  → 清空 globalFull_ → 推入 STW mainQueue 做最终标记
```

STW 保证了 Dispose 时没有并发写，所以此处直接把两个 buf 推 globalFull_ 是安全的。

---

## 9. 三个设计问题与修复记录

### 问题 1：WriteBarrierBuffer 溢出路由到错误队列（已修复，上一轮迭代）

**现象**：多线程并发 GC 测试（`bench_p1_multi`）出现 `anti=N`（N > 0），即存活对象被错误回收。

**根因**：`FlushWriteBarrierOverflow` 把溢出的引用推入了 `concurrentOverflowQueue_`（`std::mutex` 保护的 `std::vector`），而 GC 线程的主循环（`RunConcurrentMarkLoop`）只消费 `globalFull_`，从不读 `concurrentOverflowQueue_`。灰色对象被推入死角队列，GC 线程永远看不到，相关子图未标记 → 存活对象被 sweep 掉。

**修复**：`FlushWriteBarrierOverflow` 改为与 `TransferRootsToGCWork` 相同的路径：从 `workBufPool_` 取 WorkBuf，打包后整包推 `globalFull_`。

**验证**：连续 5 次运行 `bench_p1_multi`，均输出 `groups=2000 anti=0`。

---

### 问题 2：PerPGCWork wbuf1_/wbuf2_ 角色未分离，get buffer 既作源又作汇（已修复，本轮迭代）

**现象**：在高对象密度场景下，GC 线程和 Assist 线程的本地 WorkBuf 频繁与 `globalFull_` 之间来回"倒腾"，本地缓冲的摊销价值丧失，`globalFull_` 的 mutex 竞争增加。

**根因**：原设计 `wbuf1_` 同时作为"取对象"的源（Pop）和"放子对象"的汇（Push）。Pop 一个对象 → trace → 子对象 Push 回 wbuf1_ → 若子对象多则 wbuf1_ 立刻满 → 推回 `globalFull_` → 再 Pop → 循环。每次 trace 一个对象可能触发一次 push-to-global，mutex 竞争频率 = 每对象一次。

**修复**：
- `wbuf1_` 改为纯 **get buffer**：只做 Pop，不做 Push，单调 drain 至空。
- `wbuf2_` 改为纯 **put buffer**：只做 Push（即 `PutObj`），满时整包推 `globalFull_`，不做 Pop。
- `TryGetObj` 新逻辑：drain wbuf1_ → 空时把 wbuf2_ 晋升为 wbuf1_（swap 指针），取新空 wbuf2_ → 仍空则从 `globalFull_` 补货整包。
- 效果：每 256 个对象才触发一次 `globalFull_` 操作（mutex 持有时间极短），摊销比提升 256 倍。

**修改文件**：`src/Common/LFStack.h`（PutObj、TryGetObj、TryGetObjLocal 三个方法）。

---

### 问题 3：TransferRootsToGCWork 逐个 PutObj，走 wbuf2_ 的间接路径（已修复，本轮迭代）

**现象**：Root 对象通过 `gcThreadWork_.PutObj(obj)` 逐一注入 GC 线程本地 buf，而非直接推入 `globalFull_`，导致 Assist 线程在 ConcurrentMark 初期无法立即从 `globalFull_` 获取 Root，工作负载偏斜（Root 全在 GC 线程本地，Assist 线程只能空转等待）。

**根因**：`TransferRootsToGCWork` 的实现模式与 `FlushWriteBarrierOverflow` 不一致，走了"单对象 → wbuf2_ → 溢出才到 globalFull_"的间接路径。

**修复**：`TransferRootsToGCWork` 改为与 `FlushWriteBarrierOverflow` 完全相同的直接批量推模式：

```cpp
WorkBuf* buf = workBufPool_.Get();
while (!markWorkStack_.empty()) {
    BaseObject* obj = markWorkStack_.back(); markWorkStack_.pop_back();
    if (buf->Full()) { globalFull_.Push(buf); buf = workBufPool_.Get(); }
    buf->Push(obj);
}
if (!buf->Empty()) globalFull_.Push(buf);
else workBufPool_.Put(buf);
```

**效果**：Root 对象以 WorkBuf 整包的形式直接入 `globalFull_`，GC 线程和 Assist 线程从并发标记第一刻起就可以公平竞争 Root 工作，工作负载均衡改善。

**修改文件**：`src/Heap/MCollector/MarkSweepCollector.cpp`（`TransferRootsToGCWork` 函数）。

---

## 10. 设计原则总结

| 原则 | 体现 |
|------|------|
| **所有生产路径统一批量推 globalFull_** | WriteBarrierBuffer 溢出、TransferRootsToGCWork，都走 WorkBuf→globalFull_ |
| **所有消费路径统一通过 PerPGCWork.TryGetObj** | GC 线程、Assist 线程，接口完全相同 |
| **get/put 职责严格分离** | wbuf1_ 只 drain，wbuf2_ 只积累；角色正交，避免倒腾 |
| **256 对象 = 一次全局操作** | WorkBuf.kCapacity=256，摊销 mutex 成本 |
| **DiscoverObject 保证入队唯一性** | bitmap 原子 CAS，同一对象只入队一次 |
| **STW 时 Dispose 兜底** | DisposeAllPerPGCWork 确保并发阶段残余全部清空 |
| **收敛检查覆盖全体工作** | `gcThreadWork_.Empty() && globalFull_.Empty()` 是充分条件 |
