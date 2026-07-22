# Step36：MSGC 屏障热路径优化方案

> 记录时间：2026-07-08
> 目标：把当前 MSGC 写屏障 inline 发射链路上「GC state / heap range 取值过重、SATB 语义分阶段、work buffer 写入与 flush 放大」这三类热点，细化成可直接实施的分步优化方案。
> 口径：**以当前代码为准**。所有指令级描述均基于 `AArch64AsmPrinter.cpp::emitMccWriteBarrierFastPath` 现状。
> 背景数据：活对象从个位数增长到百万级别时，case27 吞吐差距被放大到 MSGC ~4000 vs CopyGC ~10000。原因是百万活对象下 mark 阶段几乎常驻，**写屏障 = 常驻热路径**，屏障单次成本被直接放大到吞吐上。
> 最新诊断补充：在临时“直接丢弃三条 `wbuf` flush 路径”的实验版 runtime 中，case27 吞吐从 **~4000 req/s 直接抬到 ~9600 req/s**。这说明当前真正贵的不是 `wbBuf.Push()` 本身，而是 flush 路径把 mutator 线程拖入 `DiscoverObject + WorkBuf 打包 + 局部/全局队列外发` 的同步 collector 工作。
> 第二条诊断补充：恢复 flush、但把 mutator `assist mark` 整体关闭后，`case27 c10/p10K/heap4gb` 连续两次都会在约 12 秒内以 `Regular allocation cannot allocate memory ... OutOfMemory` 失败。这说明 `assist` 在当前 MSGC 里不只是“性能负担”，还是并发标记与分配 pacing 的活性支撑，不能直接整个拿掉。

---

## 目录

1. [结论先行](#1-结论先行)
2. [当前写屏障热路径逐指令拆解](#2-当前写屏障热路径逐指令拆解)
3. [优化域一：GC state 与 heap range 的编译器侧获取](#3-优化域一gc-state-与-heap-range-的编译器侧获取)
4. [优化域二：SATB 转混合屏障](#4-优化域二satb-转混合屏障)
5. [优化域三：work buffer 写入与 flush](#5-优化域三work-buffer-写入与-flush)
6. [落地顺序与依赖](#6-落地顺序与依赖)
7. [风险与回退](#7-风险与回退)
8. [验证口径](#8-验证口径)

---

## 1. 结论先行

1. 当前 MSGC 写屏障在 mark 阶段单次 ref 写要发射约 **33 条指令**，其中包含 **3 次 GOT 间接全局 load + 1 次 acquire load + 2 组双边界堆检查**，远不是一个「薄屏障」。
2. **收益最大、风险最低的是优化域一**：把 `inlineFastPathGCPhase / heapReservedStartAddr / heapReservedEndAddr` 三个全局从「跨 image GOT 间接」改成「X28 CJTLS 直读」，并把 phase 的 acquire load 降为 plain load、双边界堆检查降为单边界。这一域纯发射层改动，可用现有开关回退。
3. **优化域二（SATB → hybrid）价值不在单次更便宜，而在结构简化 + STW 缩短**：统一单一屏障形态贯穿整个 mark 周期，去掉 `GC_PHASE_MS_ENUM` 的 old+new 特判与 mark termination 的 STW 栈重扫。属于语义变更，放最后做。
4. **新增实验结论把优化域三的重要性大幅抬高**：临时关闭 `FlushWriteBarrierOverflow / FlushExitedWriteBarrierBuffer / FlushAllWriteBarrierBuffers` 后，吞吐从 ~4000 直接升到 ~9600，证明当前性能瓶颈已经不只是「写屏障发射太重」，而是 **overflow/final flush 把 mutator 拉进了重 collector 工作**。
5. 因此，**优化域三的优先级要调整为：先把 overflow flush 从 mutator 热路径摘掉，再优化 bulk 旧边枚举与重复日志**。如果保留正确性前提下仍无法接近实验上限，再回头评估 hybrid barrier 等更大语义改动。
6. `assist mark` 的方向不是“直接关闭”，而是**保活前提下轻量化**：当前实验已经证明，全关 assist 会把性能问题升级成 OOM/活性问题；真正可行的方向应是降低 mutator 参与强度、改进 credit/debt 触发方式，或补上 regular allocation 的 retry/wait 兜底。

一句话总结：

> **Step36 的主战场已经从“只削 33 条指令的发射层”扩展成“两线并行”：一条是把写时固定开销削到 20 条以内，另一条是把 flush 的 discover / enqueue / forwarding 工作尽量从 mutator 线程上摘走。**

预期效果（分域独立可测）：

| 域 | 手段 | 预期 |
| --- | --- | --- |
| 一 | TLS 化 + 去 acquire + 单边界 + heap-obj 跳过 | ref 屏障 ~33 → ~18-20 指令，去掉全部 GOT 间接 |
| 二 | hybrid 统一屏障 | 去掉 ENUM 双语义窗口 + STW 栈重扫，phase 判断退化为单布尔 |
| 三 | overflow 异步化 + bulk fast path + 重复 ref 过滤 | 把 mutator 侧同步 flush 成本移出热路径，flush 频率与原子 CAS 次数下降 |

---

## 2. 当前写屏障热路径逐指令拆解

以最基础的 `WriteRefFieldFast_MSGC(ref, obj, field)` 在 mark 阶段为例，发射逻辑见
[llvm/lib/Target/AArch64/AArch64AsmPrinter.cpp:2662](../../../../cangjie_compiler/third_party/llvm-project/llvm/lib/Target/AArch64/AArch64AsmPrinter.cpp)
起（`emitMccWriteBarrierFastPath`）。

### 2.1 三个全局是「跨 image GOT 间接」

符号定义在
[AArch64AsmPrinter.cpp:92](../../../../cangjie_compiler/third_party/llvm-project/llvm/lib/Target/AArch64/AArch64AsmPrinter.cpp)：

```cpp
InlineFastPathGCPhaseSymName  = "__ZN12MapleRuntime4Heap21inlineFastPathGCPhaseE";
HeapReservedStartAddrSymName  = "__ZN12MapleRuntime4Heap21heapReservedStartAddrE";
HeapReservedEndAddrSymName    = "__ZN12MapleRuntime4Heap19heapReservedEndAddrE";
```

它们是 runtime dylib 内的全局（[src/Heap/Heap.cpp:39](../../../src/Heap/Heap.cpp)、[Heap.h:131](../../../src/Heap/Heap.h)）。应用码与 runtime 分属不同 image，取址只能走 GOT：

```asm
ADRP  Xd, sym@GOTPAGE
LDR   Xd, [Xd, sym@GOTPAGEOFF]   ; 取到全局地址（GOT 间接）
LDR/LDARB Xd, [Xd]               ; 再取值
```

即每个全局 **3 条指令、2 次 load**，其中一次是 GOT 间接。这三个值进程级几乎不变，却在**每屏障、每 callsite** 重新加载。

### 2.2 逐指令清单（ref 写、mark 阶段、非 static）

| # | 段落 | 指令 | 备注 |
| --- | --- | --- | --- |
| 1 | 读 phase | `ADRP + LDR[GOT] + LDARB` | GOT 间接 + **acquire**，[:2662](../../../../cangjie_compiler/third_party/llvm-project/llvm/lib/Target/AArch64/AArch64AsmPrinter.cpp) |
| 2 | phase==0? | `SUBS + b.eq StoreOnly` | [:2663](../../../../cangjie_compiler/third_party/llvm-project/llvm/lib/Target/AArch64/AArch64AsmPrinter.cpp) |
| 3 | 读 heapStart | `ADRP + LDR[GOT] + LDR` → X16 | [:2673](../../../../cangjie_compiler/third_party/llvm-project/llvm/lib/Target/AArch64/AArch64AsmPrinter.cpp) |
| 4 | 读 heapEnd | `ADRP + LDR[GOT] + LDR` → X17 | [:2674](../../../../cangjie_compiler/third_party/llvm-project/llvm/lib/Target/AArch64/AArch64AsmPrinter.cpp) |
| 5 | obj 堆检查 | `SUBS/b.lo/SUBS/b.hs` | 双边界 4 指令，[:2677](../../../../cangjie_compiler/third_party/llvm-project/llvm/lib/Target/AArch64/AArch64AsmPrinter.cpp) |
| 6 | 读 old | `LDR X10,[X2]` | [:2679](../../../../cangjie_compiler/third_party/llvm-project/llvm/lib/Target/AArch64/AArch64AsmPrinter.cpp) |
| 7 | old==0? | `CBZ StoreOnly` | [:2685](../../../../cangjie_compiler/third_party/llvm-project/llvm/lib/Target/AArch64/AArch64AsmPrinter.cpp) |
| 8 | old 堆检查 | `SUBS/b.lo/SUBS/b.hs` | 双边界 4 指令，[:2687](../../../../cangjie_compiler/third_party/llvm-project/llvm/lib/Target/AArch64/AArch64AsmPrinter.cpp) |
| 9 | 取 allocBuf | `LDR X11,[X28,#off] + CBZ slow` | 已经是 TLS 直读 |
| 10 | 查满 | `LDR count + SUBS + b.hs slow` | [:2696](../../../../cangjie_compiler/third_party/llvm-project/llvm/lib/Target/AArch64/AArch64AsmPrinter.cpp) |
| 11 | 写入 | `ADD base + STR[base,count,lsl3]` | [:2709](../../../../cangjie_compiler/third_party/llvm-project/llvm/lib/Target/AArch64/AArch64AsmPrinter.cpp) |
| 12 | 更新 count | `ADD + STR` | [:2722](../../../../cangjie_compiler/third_party/llvm-project/llvm/lib/Target/AArch64/AArch64AsmPrinter.cpp) |
| 13 | store field | `STR X0,[field]` | [:2729](../../../../cangjie_compiler/third_party/llvm-project/llvm/lib/Target/AArch64/AArch64AsmPrinter.cpp) |

合计约 **33 条指令、6 次访存（其中 3 次 GOT 间接）、1 次 acquire、2 组双边界检查**。

对照 Go 写屏障快路径（`writeBarrier.enabled` 固定偏移 plain load + `CBZ done`），当前实现明显偏重，尤其是「域一」这几项与 old-ref 检查无关的固定开销。

### 2.3 结论

> 屏障成本里真正跟「记录 old 边」相关的只有第 6-12 步；第 1、3、4 步（三个全局 GOT 间接取值）和第 5 步（obj 堆检查）都是可优化掉的固定间接开销。这就是优化域一的靶点。

---

## 3. 优化域一：GC state 与 heap range 的编译器侧获取

本域全部为发射层（`AArch64AsmPrinter.cpp`）+ 少量 runtime TLS 布局改动，语义不变，收益最大、可回退，**建议第一批落地**。

### 3.1 子项 A：三个全局搬进 X28 CJTLS，消除 GOT 间接

**现状**：phase / heapStart / heapEnd 走 GOT 间接（§2.1）。
**目标**：把它们放进 X28 指向的 CJTLS 块，取值降为单条 `LDRB/LDR [X28,#imm]`。

屏障已经在用 X28 读 `msAllocBuffer`（[:2689](../../../../cangjie_compiler/third_party/llvm-project/llvm/lib/Target/AArch64/AArch64AsmPrinter.cpp) `LDR X11,[X28,#getMSAllocBufferOffsetInCJTLS/8]`），说明 CJTLS 块可用且已被消费。

改动点：

1. runtime 在 CJTLS 结构里新增 3 个字段：`tlGCPhase(u8)`、`tlHeapStart(u64)`、`tlHeapEnd(u64)`（或 `tlHeapSize`，见子项 C）。
2. 刷新时机：
   - `tlGCPhase`：mutator 进入托管态 / 跨 safepoint 时随 `encodedGCPhase` 一并刷新（与现有 `inlineFastPathGCPhase` 同源）。
   - `tlHeapStart/End`：堆保留区在 heap init 后固定（[Heap.h:119](../../../src/Heap/Heap.h)），线程创建时写一次即可；保守起见也可在 mark 开始的 handshake 里统一写各线程 TLS。
3. 发射层把 `EmitLoadGlobalU8Acquire(...phase...)` 与两处 `EmitLoadGlobalU64(...heapStart/End...)` 换成 `LDR [X28,#imm]`。

收益：每屏障省约 **6 条指令 + 3 次 GOT 依赖 load**（3 个全局各从 3 指令降到 1 指令）。

> 注意：Step35 §3.4 曾**声称**「从 X28 CJTLS 直接读单字节 gcFastState」，但当前发射代码读的仍是全局符号。本子项就是把这个「文档已规划、代码未落地」的差补齐。

### 3.2 子项 B：phase 的 `LDARB` acquire 降为 plain `LDRB`

**现状**：[:2662](../../../../cangjie_compiler/third_party/llvm-project/llvm/lib/Target/AArch64/AArch64AsmPrinter.cpp) 用 `EmitLoadGlobalU8Acquire`（`LDARB`）。
**依据**：phase 只在 STW / handshake safepoint 切换（[Heap.cpp:218](../../../src/Heap/Heap.cpp) 起 `__ATOMIC_RELEASE`），mutator 跨 safepoint 已经历全屏障，屏障内部再做 per-write acquire 是多余排序。Go 读 `writeBarrier.enabled` 也是 plain load。
**改动**：TLS 化之后直接 `LDRB [X28,#off]`，天然去掉 acquire。

风险：需确认没有「不经过 safepoint 就期望观察到最新 phase」的路径。当前 phase 发布依赖 STW/handshake，满足。

### 3.3 子项 C：双边界堆检查降为单边界

**现状**：`EmitHeapRangeCheckTo`（[:2605](../../../../cangjie_compiler/third_party/llvm-project/llvm/lib/Target/AArch64/AArch64AsmPrinter.cpp)）对每个待检查值做
`(v>=start) && (v<end)` = 两次 `SUBS` + 两次 `Bcc`（4 指令）。
**目标**：无符号减法技巧
`(v - start) <u heapSize` = `SUB + SUBS(cmp) + b.hs`（3 指令、1 次分支）。

配合子项 A，在 TLS 里直接存 `tlHeapStart` 与 `tlHeapSize = end - start`，屏障里 `SUB tmp,v,start; CMP tmp,heapSize; b.hs skip`。

收益：每个堆检查省 1 指令 + 1 个分支预测点。ref 写屏障有 obj 与 old 两处堆检查（子项 D 若命中可去掉 obj 那处）。

### 3.4 子项 D：对可静态证明的 heap base 跳过 obj 堆检查

**现状**：`WriteRefFieldFast_MSGC` 无条件对 `obj` 做堆检查（[:2677](../../../../cangjie_compiler/third_party/llvm-project/llvm/lib/Target/AArch64/AArch64AsmPrinter.cpp)），因为发射层无法确认 obj 一定在堆里（Step26 §5.5 也承认这点）。
**观察**：绝大多数 `WriteRefField` 的 `obj` 是类实例字段写的接收者，前端 CHIR 阶段知道它是堆对象。
**改动**：

1. lowering 在能证明 base 为堆对象时，给 MSGC wrapper callsite 打属性 `cj-heap-obj`（类似现有 `cj-msgc-sizeclass` 等属性机制）。
2. 发射层见到该属性时，整段 obj 堆检查（4 指令 + 对 heapStart/End 的依赖）删除，只保留 old-ref 检查。
3. 证明不成立时保持现状保守 fallback。

收益：命中时每屏障再省约 4 指令。属于「需前端/lowering 配合」的一项，排在纯发射层子项之后。

### 3.5 域一合计

TLS 化（A）+ 去 acquire（B）+ 单边界（C）后，ref 写屏障从 ~33 降到 **~22-24 指令**且无 GOT 间接；再叠加 heap-obj 跳过（D）可到 **~18-20 指令**。这是把 4000 往 CopyGC 拉近的最直接杠杆。

---

## 4. 优化域二：SATB 转混合屏障

### 4.1 现状

- 写侧纯 old-only deletion（Yuasa SATB），[src/Heap/MBarrier/MSMarkBarrier.cpp:134](../../../src/Heap/MBarrier/MSMarkBarrier.cpp)。
- 另有独立 `GC_PHASE_MS_ENUM` 窗口，对 `WriteRefField / WriteStaticRef` 做 old+new（Step35 §2.6、§3.5）。
- 读侧本就是零成本 direct load（[MSMarkBarrier.cpp:97](../../../src/Heap/MBarrier/MSMarkBarrier.cpp)）。

### 4.2 为什么转 hybrid（Go 1.8 风格）

价值**不在**单次写更便宜——hybrid 需要在栈未扫描时对 new 也 shade，单次反而多做一次。价值在结构与 STW：

1. **统一单一屏障形态贯穿整个 mark 周期**：不再需要 ENUM 阶段切换屏障语义。发射层 phase 判断从「多阶段 + 可能的 bit2 enum 特判」退化为**单布尔 marking on/off**，与域一叠加后快路径分支进一步收缩。
2. **消除 mark termination 的 STW 栈重扫**：hybrid 不变式（`shade(*slot) + shade(new if 源栈未扫描)`）保证不会出现 black→white 逃逸，可去掉 STW 重扫栈这一步。百万活对象时这是 STW 主要成本之一。
3. **读侧继续零屏障**：hybrid 天然允许无读屏障，MSGC 读侧不需要任何改动，恰好对齐「读写屏障混合」目标。

### 4.3 落地要点

1. runtime：将 `MSEnumBarrier` 的 old+new 语义并入统一 `MSMarkBarrier`，全程 `shade(old) + 条件 shade(new)`；栈扫描状态通过 per-mutator 标志判定「源栈是否已扫描」。
2. 去掉 `GC_PHASE_MS_ENUM` 作为独立屏障切换点（保留其作为 root enumeration 时间窗即可，但屏障语义不再随之切换）。
3. 发射层：phase 判断简化为 `marking on/off` 单布尔（配合域一的 TLS 单字节）。
4. mark termination 去掉 STW 栈重扫路径。

### 4.4 风险

语义正确性是硬门槛，必须过 `VerifyMarkCompleteness` + case27 稳定性。Step25 §5.3 已把「old-only vs old/new vs hybrid」列为待定的 collector invariant 选择，本域即对该选择拍板为 hybrid，需以数据为准，**排在域一之后**。

---

## 5. 优化域三：work buffer 写入与 flush

### 5.0 当前 flush 逻辑总览（以现有代码为准）

在细化优化点之前，先把当前这套 flush 链路完整串一遍，作为后续所有子项的公共基线。

#### 5.0.1 数据结构

| 结构 | 定义 | 角色 |
| --- | --- | --- |
| `WriteBarrierBuffer`（wbBuf） | [WriteBarrierBuffer.h:40](../../../src/Heap/MBarrier/WriteBarrierBuffer.h) | per-owner 日志缓冲：`BaseObject* buf_[1024] + count_`，屏障单写者，无锁 Push |
| `WorkBuf` | [LFStack.h:24](../../../src/Common/LFStack.h) | 全局工作交换单元：`next + len + objs[1024]`，64B 对齐 |
| `WorkBufPool` | [LFStack.h:111](../../../src/Common/LFStack.h) | WorkBuf 空闲池，底层 `LFStack`（**mutex**，非 lock-free） |
| `LFStack`（`globalFull_`） | [LFStack.h:49](../../../src/Common/LFStack.h) | 全局满 buf 栈，**mutex 保护** |
| `PerPGCWork`（gcw） | [LFStack.h:206](../../../src/Common/LFStack.h) | per-P 本地队列：`list1_`(消费就绪) / `list2_`(标记输出) / `getCur_` / `putCur_`，`list1_` 最多 4 节点 |

wbBuf 挂在每个 `AllocBufferMS`（per-mutator/P）上，创建时 `RegisterAllocBufferMS` 注册进 `CentralRegionPool` 的 owner registry（[MSAllocBuffer.cpp:111](../../../src/Heap/MAllocator/MSAllocBuffer.cpp)）。

#### 5.0.2 写入侧（Push）

- **inline asm 快路径**（域二拆解的第 6–12 步）：old-ref 经「非空 + 双边界堆检查」后，直接 `STR` 进 `[wbBuf.buf_ + count]` 并自增 `count_`，全程无调用、无锁。
- **C++ bulk 路径**：`WriteStruct` / `CopyRefArray` / `CopyStructArray` 线性枚举 dst 范围内旧 ref，逐个走 `EnqueueRefFast`（[MSMarkBarrier.cpp:25](../../../src/Heap/MBarrier/MSMarkBarrier.cpp)）：`IsHeapAddress` 过滤 → `GetOrCreateAllocBufferMS` 取 wbBuf → 满则先 flush → `Push`。

#### 5.0.3 三条 flush 触发路径

1. **overflow flush**：wbBuf 满（`count_ == 1024`）时，`EnqueueRefFast` / asm slow stub 调 `FlushWriteBarrierOverflow`（[MarkSweepCollector.cpp:968](../../../src/Heap/MCollector/MarkSweepCollector.cpp)）。**mutator 自己同步执行**。
2. **owner-exit flush**：线程 / P 退出时 `AllocBufferMS::Fini` 调 `FlushExitedWriteBarrierBuffer`（[MSAllocBuffer.cpp:120](../../../src/Heap/MAllocator/MSAllocBuffer.cpp)）。
3. **final STW flush**：`MarkTermination`（STW）里 `FlushAllWriteBarrierBuffers`（[MarkSweepCollector.cpp:1328](../../../src/Heap/MCollector/MarkSweepCollector.cpp)），`VisitAllocBufferMS` 遍历**全部** owner，逐个把残留 wbBuf 排入 `mainQueue`。

#### 5.0.4 单条 flush 做的工作

三条路径的循环体一致：对 wbBuf 每个条目执行 `DiscoverObject`（[:495](../../../src/Heap/MCollector/MarkSweepCollector.cpp)）=

1. `IsTraceableObjectAddress`（`IsManagedAddress` + `GetRegionInfoValidated` + LARGE base 解析）
2. `obj->IsValidObject()`
3. `MarkObject` → `MarkObjectInHeap` → `TestAndSetMarkBit`（**原子 `fetch_or` 到 markBitmap**，含一次除法 + size-class 表查）

返回「新灰」的对象被 repack 进一个从 `workBufPool_.Get()` 取出的 `WorkBuf`；装满一个就路由出去。

#### 5.0.5 路由与消费

- overflow flush 走 **local-first**：新灰 WorkBuf 经 `gcw->PushWBBuf` 优先进本 P 的 `list1_`，`list1_` 满（>4 节点）才 `globalFull_->Push`（[LFStack.h:241](../../../src/Common/LFStack.h)）。
- owner-exit / final STW 目前**不走 local-first**，直接进 `globalFull_` / `mainQueue`。
- 消费侧：mutator `assist` 与 GC 主线程 drain 从 `getCur_ → list1_ → list2_ → putCur_ → globalFull_` 依次取 WorkBuf 扫描（`DrainGCWork` / `DrainOneBufRound`，[:1172](../../../src/Heap/MCollector/MarkSweepCollector.cpp) 起）。
- final STW 收口：`collectPendingMarkWork` 依次 `FlushAllWriteBarrierBuffers → DisposeAllPerPGCWork → DrainGlobalLFStackToMainQueue → DrainDeferredMainThreadQueueToMainQueue`（[:1326](../../../src/Heap/MCollector/MarkSweepCollector.cpp)），保证 mark 完备后再进 sweep。

#### 5.0.6 一句话概括

> 当前 flush = **mutator 在 wbBuf 满 / 线程退出 / final STW 三个时机，同步地对每条日志做 discover + 原子 mark + repack 进 WorkBuf + 路由到本地或全局队列**。§5.2 的诊断实验（丢弃三条 flush → 4000 升到 9600）正说明：这套「同步摊到业务线程上的 collector 工作」才是当前主瓶颈，后续 §5.3 起的所有子项都是围绕「把这套工作从 mutator 热路径上移走 / 变薄」展开。

### 5.1 现状回顾

- 屏障把 old-ref `Push` 进 per-owner `WriteBarrierBuffer`（1024 槽固定数组），[src/Heap/MBarrier/WriteBarrierBuffer.h:56](../../../src/Heap/MBarrier/WriteBarrierBuffer.h)。
- 满时走 `FlushWriteBarrierOverflow`（[src/Heap/MCollector/MarkSweepCollector.cpp:968](../../../src/Heap/MCollector/MarkSweepCollector.cpp)），每条走 `DiscoverObject`（[:495](../../../src/Heap/MCollector/MarkSweepCollector.cpp)）= `IsTraceableObjectAddress + IsValidObject + 原子 CAS MarkObject`。
- flush 已本地优先：新灰对象优先进本 owner 的 `PerPGCWork.list1_`，退化才走 `globalFull_` 互斥（[:975](../../../src/Heap/MCollector/MarkSweepCollector.cpp)）。方向正确。

### 5.2 最新诊断结论：真正贵的是“mutator 同步做 flush”

临时实验中，把 `overflow flush / owner-exit flush / final STW flush` 三条路径统一改成“直接丢弃并记账”后，case27 吞吐从 **~4000 req/s 抬到 ~9600 req/s**。这个数据有两个很强的结论：

1. 热点不在 `wbBuf.Push()` 这一写一加，而在 **buffer 满了以后 mutator 自己去做 discover / mark / 打包 / 外发**。
2. 当前 flush 设计把 collector 工作同步摊到了业务线程上；只要 mark 阶段常驻，overflow 一多，吞吐就会被同步 flush 直接拖垮。

这也解释了为什么单纯“减少几条发射指令”不够：域一很重要，但它解决的是每次写的固定税；而诊断实验表明，**吞吐翻倍的真正空间在 flush 架构本身**。

但 2026-07-08 的第二轮隔离实验也给了一个边界条件：保持 flush 正常、仅关闭 `AllocBufferMS::AccumulatePacingDebt()` 里的 `MSMarkAssist()` 后，`case27 c10/p10K/heap4gb` 会在约 12 秒内失败，典型现象是：

1. assist 关闭后，`gc.log` 中 `Assist enabled` 仍会打印 ratio，但 `[assist] calls=0`
2. `ConcurrentMark` 个别轮次会被 GC 主线程单独拉长到 `225,016us / 3,265,822 objects`
3. 随后 mutator 在 `RegionSpaceMS::AllocateRegularObject()` 的单次失败路径直接抛 `OutOfMemory`

这说明两个事实要同时成立：

1. flush 架构仍然是最大的吞吐瓶颈之一
2. 但当前 MSGC 的并发标记/pacing 也仍然依赖 mutator assist 提供最低限度的供给与追赶能力

因此，优化方向不能简单演化成“把 assist 删掉”，而应改成“**把 mutator assist 变轻，而不是变成 0**”。

### 5.3 子项 A：overflow flush 改成 swap-and-publish / 异步 discover（最高优先级）

当前 `FlushWriteBarrierOverflow()` 的问题不是“逻辑错”，而是它发生在 mutator 热路径里，并且逐条做以下工作：

1. 扫描 `wbBuf` 全部条目
2. 对每条执行 `DiscoverObject`
3. 将成功的新灰对象重新装入 `WorkBuf`
4. 再路由到 `PerPGCWork.list1_` 或 `globalFull_`

保留正确性的前提下，最值得做的不是继续微调这里的几条分支，而是把它改成 **swap-and-publish**：

1. `wbBuf` 满时，mutator 只负责“切换到一个空 buffer / 空批次”
2. 满 buffer 作为整体挂到 per-P pending list 或全局 pending queue
3. 后续由 GC 线程或 assist 线程异步执行 `DiscoverObject + WorkBuf 打包`

这样做的好处：

1. mutator 热路径从“同步 flush”退化为“常数级甩锅”
2. `PerPGCWork::PushWBBuf()` 这类整块路由结构可以真正发挥作用，而不是先逐对象 repack 再整块转发
3. 更接近实验版 9600 req/s 暴露出的上界

注意：这里的“异步化 flush”并不等价于“顺便把 assist 一起关掉”。从 2026-07-08 的 case27 结果看，当前 runtime 下 **flush 可以重构，assist 不能直接归零**；否则会触发 allocation pacing 失效，最终在 regular alloc 的单次失败路径上放大成 OOM。

### 5.4 子项 B：bulk 写路径单独做 fast path（第二优先级）

`WriteStruct`、`CopyRefArray`、`CopyStructArray` 当前都不是“写一个 ref”，而是**先线性枚举 destination 范围内所有旧 ref，再逐个 `EnqueueRefFast`**：

1. `WriteStruct`：[MSMarkBarrier.cpp:166](../../../src/Heap/MBarrier/MSMarkBarrier.cpp)
2. `CopyRefArray`：[MSMarkBarrier.cpp:203](../../../src/Heap/MBarrier/MSMarkBarrier.cpp)
3. `CopyStructArray`：[MSMarkBarrier.cpp:223](../../../src/Heap/MBarrier/MSMarkBarrier.cpp)

在容器 grow、rehash、数组拷贝频繁的负载下，这类 bulk 路径很可能比单字段 `WriteReference` 更伤。优化方向应单独拉出来，不要和普通 ref 写混在一起：

1. 对 fresh dst / 已知全零 dst / 新分配对象字段初始化直接走 no-old-edge fast path
2. 对可静态证明“不含 ref”的 slice 保持纯 `memcpy/memmove`
3. 对大范围 copy，优先按 card / chunk 批量记录，再异步处理旧边，而不是同步逐字段入 `wbBuf`

### 5.5 子项 C：重复 old-ref 是隐性放大器（低成本高收益）

`Push` 无去重。同一字段在循环里反复覆写，会把同一 old-ref 反复塞进 1024 槽 → 提前触发 flush → flush 里每条都走原子 CAS（重复项直到 CAS `MarkObject` 返回 already-marked 才被丢弃）。这是把「少量真实 old 边」放大成「大量重复入队 + 大量原子操作」。

**低成本缓解**：屏障内维护一个「上次 logged ref」单槽（1 个寄存器 / 1 个 TLS 字段），与当前 old 相等则直接跳过 Push。对「循环内反复写同一字段」的模式（正是百万对象基准的典型）非常有效，且**不引入任何原子操作**。

发射层实现：在 §2.2 第 7 步（`old==0?`）之后、写入之前，加一条 `CMP old, tlLastLoggedRef; b.eq StoreOnly`；写入成功后 `STR old, tlLastLoggedRef`。成本 2-3 指令，换取 flush 频率与原子 CAS 数下降。

### 5.6 子项 D：overflow 与 final STW 分开优化，不再共享同一种回放策略

overflow 与 final STW 的目标其实不同：

1. **overflow**：目标是别阻塞 mutator，优先“快速甩锅”
2. **final STW**：目标是 pause 可控，优先“批量收口”

因此两者不应该继续共用“逐条 replay + 逐条 discover”这一思路。更合理的分工是：

1. overflow 走异步 publish / whole-buffer handoff
2. final STW 直接消费 pending buffer 列表，按块批量 discover
3. owner-exit flush 也走与 overflow 相同的 publish 语义，避免线程退出时做同步 collector 工作

### 5.7 子项 E：修正容量/布局注释与 count 热点访存（收尾项）

- [WriteBarrierBuffer.h:30](../../../src/Heap/MBarrier/WriteBarrierBuffer.h) 注释仍写「256 entries = 2 KB」，实际 `kCapacity = 1024`（8 KB）。先纠正误导。
- 当前 `count_` 在 `buf_[1024]` 之后（偏移 8192），与刚写入的槽不在同一 cache line。每屏障都要 `load count → cmp → store count`。可评估把 `count_` 放到 buffer 头部（与热点写入区同 line）减少跨行访存。注意此改动会动 ABI 偏移常量 `MSGCWBBufferCountOffset`，需 runtime 与发射层同步（见 §7）。

### 5.8 子项 F：flush 内 workBufPool 交互批量化（次级整理项）

`FlushWriteBarrierOverflow` 每写满一个 `WorkBuf` 就 `workBufPool_.Get()` / `Put()` 一次（[:996](../../../src/Heap/MCollector/MarkSweepCollector.cpp) 起）。在大批量 flush 时可一次性预取 / 归还多个 WorkBuf，减少 pool 抖动。`DiscoverObject` 内的原子 CAS 是 flush 主成本，百万对象下不可避免，但 pool 交互可摊薄。属于收尾整理项，非 blocker。

### 5.9 子项 G：如果以上都做完 still 不够，再评估 barrier 算法切换

诊断实验已经说明“把 flush 从热路径上移开”是巨大收益点。因此在做完：

1. 域一的发射层瘦身
2. 域三的 overflow 异步化
3. bulk 路径 fast path
4. 低成本去重

之前，不宜过早把问题上升为“必须立即切 hybrid barrier”。更合理的顺序是先把当前 old-only deletion barrier 的工程实现做到足够薄，再看是否还有必要用更大语义改动换进一步收益。

### 5.10 子项 H：flush 内部固定开销（对照当前代码新增，零/低语义风险）

前面 §5.3–§5.9 讲的是 flush 的**架构级**改造（异步 publish、bulk fast path、去重、STW 分治）。但对照当前 `MarkSweepCollector.cpp` / `MSRegionInfo.h` / `WriteBarrierBuffer.h` 的实际实现，flush 里还有几处**每条都在的固定开销**，不改架构就能吃到，且不动 ABI 偏移、不改屏障语义，适合和域一并行落地。

#### H-1：`DiscoverObject` 每条 flush 重复解析 region（最实、零风险）

`FlushWriteBarrierOverflow` 每条走 `DiscoverObject`（[MarkSweepCollector.cpp:495](../../../src/Heap/MCollector/MarkSweepCollector.cpp)），当前逐条做了**两遍地址解析**：

1. `IsTraceableObjectAddress(obj)`（[:239](../../../src/Heap/MCollector/MarkSweepCollector.cpp)）内部调 `GetRegionInfoValidated(addr)`，并对 LARGE region 做一次 base 解析。
2. 紧接着的 `MarkObject(obj)` → `MSRegionInfoArray::MarkObjectInHeap(addr)`（[MSRegionInfo.h:392](../../../src/Heap/MAllocator/MSRegionInfo.h)）**又调一次** `GetRegionInfoValidated(addr)` + **又做一次** LARGE base 解析。

即每条 flush 对同一地址算了两遍 region 查表 + 两遍大对象 base 解析。百万对象 flush 下是纯浪费。

**改法**：给 flush 路径开一个 `DiscoverObjectKnownHeap(msInfo, obj)`，`IsTraceableObjectAddress` 把已解析出的 `msInfo` 透传给 `MarkObjectInHeap`，一次查表贯穿 discover + mark。§5.5 只覆盖了「去重减少 CAS」，没覆盖这条**每条都在**的固定重复查表。

#### H-2：`markedObjectCount` 每对象一次全局原子（跨 mutator 竞争）

`DiscoverObject` 每标记成功一个对象就 `markedObjectCount.fetch_add(1, relaxed)`（[MarkSweepCollector.cpp:507](../../../src/Heap/MCollector/MarkSweepCollector.cpp)）。多个 mutator 并发 overflow flush 时，这是共享 cache line 上的高频 RMW 竞争点。

**改法**：flush 循环里本就有本地 `forwarded` 计数，循环结束后一次性 `fetch_add(forwarded)` 即可。与 §5.8 里 `globalShareCount_` 已做的批量化同类手法（那个已批量、这个还没）。

#### H-3：Exited / FinalSTW flush 未走 local-first 路由

只有 `FlushWriteBarrierOverflow` 用了 `gcw->PushWBBuf`（list1_ 本地优先，[:990](../../../src/Heap/MCollector/MarkSweepCollector.cpp)）。而：

1. `FlushExitedWriteBarrierBuffer` 直接 `globalFull_.Push`（[:1049](../../../src/Heap/MCollector/MarkSweepCollector.cpp)）。
2. `FlushAllWriteBarrierBuffers` 全部塞单一 `mainQueue`（[:1431](../../../src/Heap/MCollector/MarkSweepCollector.cpp)）。

§5.6 把「owner-exit 走 publish 语义」归到了未来的 swap-and-publish 大重构里，但**在现有结构下就能立刻改**：exited 路径也走 `PushWBBuf`，避免线程退出时抢 `globalFull_` 锁。

#### H-4：FinalSTW flush 单线程串行（STW pause 放大）

`FlushAllWriteBarrierBuffers`（[:1413](../../../src/Heap/MCollector/MarkSweepCollector.cpp)）用 `VisitAllocBufferMS` 在**一个 GC 线程上顺序**遍历所有 mutator 的 wbBuf，逐条 discover 进单一 `mainQueue`。百万活对象、多 mutator、每 buffer 满 1024 条时，这段 STW 纯串行。可按 buffer 分片到 GC worker 并行 discover 到各自本地队列。这是 §5.6「final STW 批量收口」没点破的**并行化**维度，直接砍 mark termination pause。

#### H-5：flush 循环里的 per-entry 判空是死分支（收尾项）

三条 flush 循环都有 `if (obj == nullptr) continue;`（[:1001](../../../src/Heap/MCollector/MarkSweepCollector.cpp)、[:1041](../../../src/Heap/MCollector/MarkSweepCollector.cpp)、[:1428](../../../src/Heap/MCollector/MarkSweepCollector.cpp)）。但入队端 `EnqueueRefFast` 在 Push 前已过滤 null（[MSMarkBarrier.cpp:27](../../../src/Heap/MBarrier/MSMarkBarrier.cpp)），asm 快路径也在 `old==0` 时跳过 Push。即 buffer 里不会有 null，这 1024 次/buffer 的分支是死的。价值低，收尾时可去。

#### H-6：`WriteBarrierBuffer::Flush()` 模板疑似死代码（收尾项）

[WriteBarrierBuffer.h:68](../../../src/Heap/MBarrier/WriteBarrierBuffer.h) 的 `Flush(Container&)` 模板——现在 flush 全走 collector 里用 `Data()/Count()` 的手写循环，这个模板方法应已无调用者。与 §5.7 修注释归一类收尾清理。

> 小结：H-1 / H-2 是**纯收益、零语义风险、不动 ABI**，可与域一发射层瘦身并行做，属于 §5.3 大重构之前就能吃到的低垂果实；H-4 价值大但要动 GC worker 调度，配合 §5.6 一起做；H-3 / H-5 / H-6 是低成本收尾。

### 5.11 子项 I：`LFStack` 是 mutex 而非 lock-free —— 隐藏的全局串行点

再往数据结构层挖一层，`globalFull_` 与 `workBufPool_` 底层的 `LFStack` **名不副实**：[LFStack.h:47](../../../src/Common/LFStack.h) 注释自己承认 “a plain Treiber stack is not safe here … Use a small mutex”，即 `Push/Pop/PopFull` 全部走**一把全局 `std::mutex`**（[:53](../../../src/Common/LFStack.h)、[:61](../../../src/Common/LFStack.h)）。这暴露出 §5.8 低估的问题：

#### I-1：overflow flush 的「local-first」路径其实也在抢全局锁（最重）

`FlushWriteBarrierOverflow` 即使路由到本地 `list1_`，每装满一个批次仍要 `workBufPool_.Get()`（[MarkSweepCollector.cpp:996](../../../src/Heap/MCollector/MarkSweepCollector.cpp)、[:1011](../../../src/Heap/MCollector/MarkSweepCollector.cpp)），而 `WorkBufPool::Get/Put` 走的正是 mutex-backed 的 `freeList_`（[LFStack.h:123](../../../src/Common/LFStack.h)、[:135](../../../src/Common/LFStack.h)）。

代码注释（[MarkSweepCollector.cpp:977](../../../src/Heap/MCollector/MarkSweepCollector.cpp)）说 local-first「不跨 globalFull_ 互斥」——**只对了一半**：它仍然每个 WorkBuf 撞一次 pool 的全局互斥。多 mutator 并发 overflow 时，所有人序列化在这把锁上。§5.8 F 把它描述成「pool 抖动」，低估了本质：这是**进程级全局锁**，不是抖动。

**改法**：`WorkBufPool` 分片到 per-P（每个 AllocBufferMS 挂一个小 freelist），或换真正的 lock-free freelist（带 ABA 保护 / tagged pointer）。这是让 §5.3 的 local-first 真正「无锁甩锅」的前提。

#### I-2：`list1_` 只有 4 个节点，重 overflow 下快速退化成全局锁

`WorkBufList::kMaxNodes = 4`（[LFStack.h:155](../../../src/Common/LFStack.h)），即 local-first 最多缓存 4 个满 WorkBuf（4096 对象）就溢出到 `globalFull_->Push`（全局 mutex，[:246](../../../src/Common/LFStack.h)）。而百万活对象持续 overflow 恰恰是 list1_ 最容易填满的场景——**在最需要 local-first 的负载下，它反而最快退化成全局锁**。

**改法**：加深 list1_，或让 assist 更积极消费 list1_（与 §6 第 7 条 assist 轻量化联动：assist 应优先把本 P 的 list1_ 排空，而不是去 global 拉）。

#### I-3：swap-and-publish 应让 wbBuf 直接「成为」一个可发布的 WorkBuf（零拷贝前提）

`WorkBuf`（[LFStack.h:24](../../../src/Common/LFStack.h)：`next + len + objs[1024]`）与 `WriteBarrierBuffer`（`buf_[1024] + count_`）都是 `BaseObject*[1024] + 计数`，但**字段顺序不同**，不能直接 reinterpret。§5.3 讲了 swap-and-publish 思路，但没点破关键前提：

**只要把 `WriteBarrierBuffer` 改成与 `WorkBuf` 同布局（带 `next` 链、`len` 在前）**，满 buffer 就能作为一个 WorkBuf 节点**零拷贝**挂到 per-P pending list / `globalFull_`，彻底省掉 flush 里「逐条 `DiscoverObject` 后 repack 进新 WorkBuf」那一步，也顺带省掉 I-1 的 pool `Get`。这一步把 §5.3 从「重构设想」变成「廉价可实现」，但会动屏障发射层的 wbBuf 布局偏移（`MSGCWBBufferOffsetInAllocBuffer` / `MSGCWBBufferCountOffset`），需与发射层同步（见 §7.1）。

> 小结：I-1 是被「local-first」措辞掩盖的**真实全局串行点**，优先级应等同 §5.3；I-2 是 local-first 的深度不足，低成本可调；I-3 是 §5.3 zero-copy 落地的结构前提，需连带改 ABI 偏移。

### 5.12 子项 J：为 non-empty `wbBuf` 维护 dirty owner 列表，避免 final STW 扫全量 owner

当前 `FlushAllWriteBarrierBuffers()`（[MarkSweepCollector.cpp:1413](../../../src/Heap/MCollector/MarkSweepCollector.cpp)）通过 `CentralRegionPool::VisitAllocBufferMS()`（[MSCentralRegionPool.h:826](../../../src/Heap/MAllocator/MSCentralRegionPool.h)）在 STW 下遍历**所有**已注册 `AllocBufferMS`，再逐个看 `wbBuf.Count()`。这有两个隐藏成本：

1. 即使绝大多数 owner 的 `wbBuf` 为空，也要在 STW 下做一次全量 registry 遍历。
2. `VisitAllocBufferMS` 还持有 `msAllocBufferLock_`，意味着 “扫 owner registry” 本身也被串行化到 STW 关键路径里。

在 mutator / Processor 数远大于真实写流量线程数的场景下，这部分空转会放大 final STW pause。

**改法**：给 `WriteBarrierBuffer` 或 `AllocBufferMS` 增加一个 `wbDirty` 位与 intrusive / vector dirty-owner registry：

1. 当 `wbBuf.count_` 从 `0 -> 1` 时，把 owner 挂入 dirty list。
2. overflow flush / exited-owner flush / final STW flush 清空 buffer 时，把 owner 从 dirty list 摘掉。
3. `FlushAllWriteBarrierBuffers()` 只遍历 dirty owner，而不是全量 `msAllocBuffers_`。

这条优化与 §5.6 的“overflow 与 final STW 分治”是互补关系：§5.6 讲的是**怎么 flush**，子项 J 讲的是**STW 根本该扫谁**。

### 5.13 子项 K：flush 批次内的局部 dedup，补齐单槽去重抓不到的非相邻重复

§5.5 的 `tlLastLoggedRef` 单槽过滤只能拦住**相邻重复**。但在 rehash / arraycopy / struct copy 这类 bulk 场景里，同一个 old-ref 很可能在一个 `wbBuf` 里**多次出现但彼此不相邻**。这些重复项目前都会继续走：

1. `IsTraceableObjectAddress`
2. `IsValidObject`
3. `MarkObjectInHeap`
4. `markedObjectCount` / `forwarded` 统计

即使最终在 mark CAS 上因为 already-marked 被丢掉，前面的固定开销还是都付了。

**改法**：在 flush worker 的局部栈上维护一个极小的“本批次最近已见对象”缓存，例如 32 或 64 槽 direct-mapped pointer table：

1. 命中同槽且指针相等：直接跳过本条，连 `DiscoverObject` 都不进。
2. 冲突：覆盖或保守退化为正常 `DiscoverObject`，**绝不允许因为 hash 冲突错丢真实对象**。

它和 §5.5 的关系是：

1. §5.5 解决**屏障端**最便宜的相邻重复。
2. 子项 K 解决**flush 端**剩余的非相邻重复。

这样可以在不增加屏障快路径指令数的前提下，继续压缩 overflow / exited / final STW 三条 flush 路径里的无效 discover 成本。

### 5.14 子项 L：overflow flush 改成 owner-aware API，去掉二次 TLS / GCWork 解析

当前 `EnqueueRefFast()`（[MSMarkBarrier.cpp:30](../../../src/Heap/MBarrier/MSMarkBarrier.cpp)）在进入 overflow 前已经拿到了：

1. `AllocBufferMS* allocBuf`
2. `WriteBarrierBuffer& wbBuf`

但 `FlushWriteBarrierOverflow()`（[MarkSweepCollector.cpp:980](../../../src/Heap/MCollector/MarkSweepCollector.cpp)）内部又重新：

1. `ThreadLocal::GetAllocBufferMS()`
2. 判空后取 `gcw = &allocBuf->GetGCWork()`
3. 再做一次 `gcw->Init()`

这相当于在 overflow 热点上重复做了一次 owner 解析与 fallback 分支。若 future 要把 overflow 做成 async publish，这一层还会继续留在临界路径里。

**改法**：把接口改成：

1. `FlushWriteBarrierOverflow(AllocBufferMS& owner, WriteBarrierBuffer& wbBuf)`，或
2. `FlushWriteBarrierOverflow(PerPGCWork* gcw, WriteBarrierBuffer& wbBuf)`

由 `EnqueueRefFast()` 直接把已知 owner 传入。这样可以去掉 flush 内部的二次 TLS lookup / owner 判空 / `GetGCWork()` 解引用，把 overflow 入口进一步压平。收益不如 §5.3 大，但属于**零语义风险、非常低成本**的固定开销优化。

### 5.15 子项 M：加高水位预排空，避免“只在满 1024 时才爆一次大 flush”

当前 `WriteBarrierBuffer` 只有在 `IsFull()`（[WriteBarrierBuffer.h:89](../../../src/Heap/MBarrier/WriteBarrierBuffer.h)）时才触发 overflow flush，即 mutator 会遇到一种很硬的模式：

1. 前 1023 次 Push 基本都很便宜
2. 第 1024 次突然同步执行一整批 discover / pack / route

这会把 flush 成本表现成明显的尾延迟尖刺，而不是平摊成本。

**改法**：引入高水位预排空机制，例如：

1. `count >= 768` 时只打一个 `wbPreflushPending` 位
2. helper / GC 线程看到 pending 后，优先异步排空这类 buffer
3. mutator 只有在 active / standby 都耗尽时才真正自己做同步 flush

这条和 §5.3 swap-and-publish 是同方向，但比“直接改双 buffer + pending queue”更保守，适合作为中间态验证：

1. 先验证“提前排空”本身能否显著降低 overflow 尖刺
2. 再决定是否继续上 zero-copy publish

### 5.16 子项 N：给 owner 准备本地 flush scratch `WorkBuf`，先绕开 pool 全局锁

§5.11 已指出 `WorkBufPool::Get/Put` 背后其实是 mutex-backed `LFStack`。即便 overflow 已经 local-first，`FlushWriteBarrierOverflow()` 仍会在每个 `WorkBuf` 边界上碰一次全局 pool 锁（[MarkSweepCollector.cpp:996](../../../src/Heap/MCollector/MarkSweepCollector.cpp)、[:1011](../../../src/Heap/MCollector/MarkSweepCollector.cpp)）。

在彻底做 per-P pool / lock-free freelist 之前，还有一个更低成本的过渡方案：

1. 在 `AllocBufferMS` 上预留 1-2 个 owner-local flush scratch `WorkBuf`
2. overflow flush 优先消费这几个 scratch
3. 只有 scratch 不够时才退化到 `workBufPool_.Get()`

这样能先把**常见的小到中等 overflow** 从全局 pool 互斥里摘出来，不必等整个 pool 架构重做完才见收益。它和 §5.11 I-1 不冲突：I-1 是长期结构问题，子项 N 是当前实现上的低成本缓解。

### 5.17 子项 O：`VisitAllocBufferMS` 改成 snapshot-then-iterate，缩短 registry 锁占用

§5.12 子项 J 已经指出 final STW 不该扫全量 owner；但即使在 dirty-list 尚未落地前，当前 `VisitAllocBufferMS()`（[MSCentralRegionPool.h:828](../../../src/Heap/MAllocator/MSCentralRegionPool.h)）还有一个实现问题：

1. 在 `msAllocBufferLock_` 下直接遍历 `msAllocBuffers_`
2. 并在持锁期间执行 visitor

这意味着：

1. flush / drain 路径不仅“要扫 registry”
2. 还把“整个扫的过程”都绑定在 registry 锁上

**改法**：改成两阶段：

1. 锁内只拷贝一份 `AllocBufferMS*` snapshot
2. 锁外遍历 snapshot 执行 visitor

这样即使还没做 dirty-owner list，也能把 registry 锁持有窗口压到很短；做了 dirty-owner list 之后，这个模式也仍然更适合作为统一的 owner 遍历基础设施。

### 5.18 子项 P：`WriteBarrierBuffer` 容量 sweep / 两段式 buffer（实验项）

当前 `WriteBarrierBuffer::kCapacity = 1024`（[WriteBarrierBuffer.h:42](../../../src/Heap/MBarrier/WriteBarrierBuffer.h)）更像结构常量，而不是已经被实测证明的最优点。既然 flush 成本远高于 `Push()`，就值得把“buffer 多大最合适”当成实验参数而不是定值信仰。

有两个可行方向：

1. **容量 sweep**：做 `512 / 1024 / 2048 / 4096` 的 case27 对比
2. **两段式 buffer**：保留一个小 inline primary，再接一个 secondary spill 段

取舍：

1. 小 buffer：更早暴露 overflow，但单次 flush 工作量小
2. 大 buffer：减少 flush 次数，但单次尖刺更大、cache footprint 更重
3. 两段式：试图兼顾前两者，但结构会更复杂

这条属于**实验项**而非第一批默认优化，因为它会直接影响 [MSAllocBuffer.cpp:84](../../../src/Heap/MAllocator/MSAllocBuffer.cpp) 那组 AArch64 fast-path ABI 偏移约束，必须与发射层联动验证。

### 5.19 子项 Q：flush worker 做“最近 region/base”微缓存，吃掉局部性红利

§5.10 H-1 解决的是“同一对象 discover 时做两遍 region 解析”；但对 bulk 场景还可以再往前走一步：**不同对象之间**也常常有很强的 region 局部性。

例如在数组 copy / rehash / struct copy 中，一批 old-ref 很可能：

1. 来自同一个 small-object region，或
2. 落在同一个 large object base

这时即使 H-1 已经把“单对象双查表”合并了，flush worker 仍然可以维护一个极小的最近缓存：

1. 最近一个 `MSRegionInfo*`
2. 最近一个 LARGE base
3. 对应 `slotSize / markBitmap / regionStart` 等派生信息

命中时可直接跳过一部分 `GetRegionInfoValidated()` / base 解析 / slot 边界派生逻辑。它不改变语义，也不增加屏障快路径指令，只是更充分利用 flush 批次内部的地址局部性。优先级低于 H-1，但在 bulk 负载上可能有额外收益。

### 5.20 子项 R：屏障入队与 mark bit 的每对象固定税（bulk TLS 查找 + 除法）

前面 H–Q 大多在优化 flush 的**调度/结构**；本子项补两处**紧贴热路径的每对象算术/查找固定税**——一处在屏障入队侧（bulk 路径），一处在 mark bit 落位侧。都是纯 runtime、零语义风险、不动 ABI。

#### R-1：bulk 屏障每个 ref 都重复做 TLS 查找（接 §5.4 / §5.14）

`EnqueueRefFast()` 每次调用都 `AllocBufferMS::GetOrCreateAllocBufferMS()`（[MSMarkBarrier.cpp:30](../../../src/Heap/MBarrier/MSMarkBarrier.cpp)），内部是 `ThreadLocal::GetThreadLocalData()` + null 检查 + 可能的 region-space cast（[MSAllocBuffer.cpp:784](../../../src/Heap/MAllocator/MSAllocBuffer.cpp)）。而 `WriteStruct` / `CopyRefArray` / `CopyStructArray` 是在 `ForEachRefInStruct` / `ForEachRefFieldInRange` 的 lambda 里**逐 ref** 调 `EnqueueRefFast()`（[MSMarkBarrier.cpp:48](../../../src/Heap/MBarrier/MSMarkBarrier.cpp)、[:70](../../../src/Heap/MBarrier/MSMarkBarrier.cpp)）。

即一次 N 个 ref 的 bulk copy = **N 次 TLS 查找 + N 次 null 检查**，而这一批 ref 的 `allocBuf` / `wbBuf` 是同一个。

**改法**：把 `GetOrCreateAllocBufferMS()` 与 `wbBuf` 提到 bulk 循环外一次取好，lambda 只做 `IsHeapAddress + Push`（满则 flush）。§5.14 子项 L 讲的是 flush **内部**别再二次解析 owner；R-1 是屏障**入队侧**别在 bulk lambda 里逐 ref 解析 owner，两者互补，都是把「同一 owner 解析」从 per-ref 降到 per-bulk。

#### R-2：`TestAndSetMarkBit` 每对象一次除法 + size-class 表查

flush 里每个新灰对象最终落到 `MSRegionInfo::TestAndSetMarkBit`（[MSRegionInfo.cpp:337](../../../src/Heap/MAllocator/MSRegionInfo.cpp)），每次都：

1. `SizeClassInfo::ClassToSize(sizeClass)` 一次表查；
2. `slotIdx = (objAddr - regionStart) / slotSize` —— **一次整数除法**，slotSize 多数 size class 非 2 的幂；
3. `__atomic_fetch_or(&markBitmap[wordIdx], bit, RELAXED)`（这条不可省）。

百万对象 mark 下，除法 + 表查是每对象固定成本。可在 `MSRegionInfo` 里预存 `slotSizeReciprocal`（Granlund–Montgomery 魔数乘法）或对 2 幂 class 走移位，把除法换成乘加。属于 mark 通用热路径，flush、assist、GC 主线程 drain 都吃这份成本；中等收益、纯 runtime、零语义风险。

> 小结：R-1 与 §5.4 / §5.14 同族（把 owner 解析从 per-ref 提到 per-bulk），可立即单独落地；R-2 是 mark 通用热路径的每对象税，收益覆盖所有 discover 来源，不止 flush。

### 5.21 优先级收口与「不必再继续列点」的边界

到 §5.20 为止，flush 侧的**大石头已基本捞完**，可归为三类：

1. **架构级**：§5.3 overflow 异步 publish、§5.11 I-1 + §5.16 pool 全局 mutex、§5.6 + §5.12 + §5.17 的 STW 分治 / dirty-owner registry / snapshot 遍历。
2. **每对象 / 每 ref 固定税**：§5.10 H-1 重复 region 查表、H-2 原子计数、§5.14 owner 二次解析、§5.19 region 微缓存、§5.20 R-1 bulk TLS 查找 / R-2 除法。
3. **收尾 / 实验级**：H-5 判空死分支、H-6 死模板、§5.7 注释/count 布局、§5.8 pool 批量化、§5.13 flush 端 dedup、§5.15 预排空、§5.18 容量 sweep。

再往下多是收尾级或已被更大项覆盖，边际收益递减。**下一步不应继续列点，而是先做一版最小组合实测**：

> **I-1/§5.16（pool 去全局锁）+ H-1（去重复查表）+ R-1（bulk TLS 提取）** —— 三者都零语义风险、不动 ABI，直接验证能否把 case27 从 ~4000 往实验上界 ~9600 推进，再据结果决定是否上 §5.3 的 swap-and-publish 大重构。

---

## 6. 落地顺序与依赖

按收益 / 风险比排序：

1. **域一 A+B+C**（TLS 化 + 去 acquire + 单边界）
   - 纯发射层 + runtime TLS 布局；`EnableMSGCWriteBarrierLowering` 可整体回退。
   - 先做，单独可测。
   - **可与 §5.10 H-1（去重复 region 查表）+ H-2（markedObjectCount 批量原子）并行**：纯 runtime 收益、零语义风险、不动 ABI，属于域三大重构前就能吃的低垂果实。
2. **域三 A**（overflow swap-and-publish / 异步 discover）
   - 一并落地 §5.10 H-3（exited 路径改走 local-first `PushWBBuf`）。
   - 这是最贴近 4000 → 9600 诊断结论的一步，应尽早做。
3. **域三 B**（bulk 写路径 fast path）
   - 容器 grow / copy / rehash 场景收益很可能仅次于 overflow 异步化。
4. **域三 C**（重复 ref 单槽过滤）
   - runtime/发射层小改，直接砍 flush 频率与原子 CAS。
5. **域三 L/M/N/O**（owner-aware API + 高水位预排空 + owner-local scratch `WorkBuf` + snapshot registry）
   - 都是当前实现上能逐步落地的“固定开销 / 锁占用 / 尖刺平滑”优化，不要求先改 barrier 算法。
   - 其中 L/N 最适合和 §5.11 I-1 一起做，M/O 则更偏 flush 行为与 STW 收口路径。
6. **域三 J/K**（dirty owner 列表 + flush 批次内局部 dedup）
   - 一个砍 final STW 的全量空扫，一个砍 flush 内剩余的非相邻重复 discover。
   - 都不要求先完成 hybrid barrier，且与 §5.10 / §5.11 / 本轮新增 L/M/N/O 可叠加。
7. **域一 D**（heap-obj 属性跳过 obj 检查）
   - 需前端/lowering 传属性，改动面稍大。
8. **域三 D/E/F/Q**（overflow/final STW 分治、count 布局、flush 批量化、region 微缓存）
   - B 涉及 ABI 偏移同步，谨慎。
9. **域三 P**（wbBuf 容量 sweep / 两段式 buffer）
   - 更像参数 / 结构实验，收益可能有，但会直接碰 ABI 和 cache footprint，放后面数据驱动。
10. **assist 轻量化，而不是 assist 删除**
   - 候选方向：提高 debt trigger、限制单次 assist 工作量、优先消费 GC thread 注入 credit、仅在 backlog 持续高位时才真正下沉到 mutator 扫描。
   - 若仍出现“局部拿不到 region 就直接 OOM”，需要同时评估 regular allocation 增加 retry/wait/触发同步 GC 的兜底。
11. **域二 hybrid**
   - 语义变更，最后做，数据驱动决策。

每一步都必须走 Step26 §9.4 的完整验证链：
改 llvm/compiler → 重建 compiler → 重建 runtime（**必须 `--gc-mode marksweep`**）→ 用新 cjc 重建 std → 组 out SDK → 重建 stdx → 跑 case27。**不能只替换 runtime dylib**。

---

## 7. 风险与回退

### 7.1 ABI 偏移强绑定

发射层硬编码大量 runtime layout offset（Step30 §12.3）：

```text
MSGCActiveRegionsOffsetInAllocBuffer = 8
MSGCWBBufferOffsetInAllocBuffer      = 896
MSGCWBBufferCountOffset              = 8192
```

本方案涉及：

- 域一 A：新增 CJTLS 字段偏移，发射层与 runtime 必须同步，并在 runtime 侧补 `static_assert`。
- 域三 B：改 `count_` 偏移会动 `MSGCWBBufferCountOffset`，两侧必须一起改。

**要求**：所有偏移改动都必须在 runtime 侧用 `static_assert` / friend layout accessor（参考现有 `WriteBarrierBufferLayoutAccess`）钉死，并在发射层常量处加注释指回 runtime 定义。

### 7.2 语义正确性

- 域一 B（去 acquire）：需确认 phase 发布路径全部经过 STW/handshake。
- 域二（hybrid）：collector invariant 变更，必须 `CJ_MS_VERIFY_MARK=1` 稳定。
- 域三 A（去重单槽）：单槽只过滤「连续相同」old，语义仍是 old-only 超集（漏过滤只是多记，不会漏记），correctness 安全。

### 7.3 回退

- 域一 / 域三发射层部分：`EnableMSGCWriteBarrierLowering` 关闭即回到通用 `CJ_MCC_Write*` slow path。
- 域二：需单独 gate，保留 old-only 与 hybrid 两条语义分支，出问题可切回 old-only。

---

## 8. 验证口径

### 8.1 正确性

1. `CJ_MS_VERIFY_MARK=1` 下 `VerifyMarkCompleteness` 不报错。
2. case27 连续多轮无 `SIGSEGV` / `Abort trap: 6`。
3. 无 json parse fail / failed request / invalid object / stale pointer。

### 8.2 发射与命中

1. IR 中仍是原 `llvm.cj.gcwrite_*`。
2. 反汇编中：
   - 域一 A 后，屏障内**不再出现** `heapReservedStartAddr/EndAddr` 的 GOT 加载与 phase 的 `LDARB`，改为 `LDR/LDRB [X28,#imm]`。
   - 域一 D 命中时，`cj-heap-obj` callsite 不再发射 obj 堆检查。
3. 现有用例回归：
   - `llvm/test/CodeGen/AArch64/cangjie-writebarrier-msgc-fastpath.ll`
   - `llvm/test/CodeGen/AArch64/CangjieGC/msgc-writebarrier-lowering.ll`
   - 需按新发射形态更新 CHECK。

### 8.3 性能

1. case27 `c8 / 10K`、`c8 / 100K`。
2. 对比项：吞吐、CPU 利用率（是否更接近 CopyGC）、mark 阶段总时长、`[BARRIER_VOL]` 与 wbFlush 统计（flush 次数 / forwarded 数）。
3. 判断标准看多轮稳定性，而非单轮最好值。

### 8.4 分域预期

| 域 | 主要看的指标 |
| --- | --- |
| 一 | 屏障指令数下降、GOT load 消失、单线程吞吐上升 |
| 二 | STW 时长下降、无 ENUM 双语义窗口、verify 稳定 |
| 三 | `wbFlushStats_` 的 flush 次数与 forwarded 比值下降、原子 CAS 次数下降 |

---

## 结语

Step36 是 Step25/26 的直接续作：runtime 侧 logging barrier、wbBuf 迁移、compiler fast wrapper 都已落地，剩下的瓶颈集中在**屏障发射出来的那 33 条指令本身**。

- 优化域一把「与 old 边无关的固定间接开销」（GOT 全局、acquire、双边界）削掉，是当前 ROI 最高、最该先做的一批。
- 优化域三用极低成本手段砍掉 flush 放大。
- 优化域二是更深的语义演进，用 STW 缩短和结构统一换取长期收益，放最后并以数据拍板。

> **先把写屏障从 ~33 指令削到 ~20 指令、去掉全部 GOT 间接与 acquire，再谈语义级的 hybrid 演进。**
