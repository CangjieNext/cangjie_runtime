# Step 8：AllocationRoots 退场与三色并发 Drain 收敛设计

> 目标：在 **不先引入 newborn region 机制** 的前提下，优先消除 `allocationRoots_` 在 `MarkTermination` STW 阶段形成的集中收敛开销，将“新对象 + old/new 引用变化”的追踪工作尽可能前移到并发阶段。  
> Step 8 采用 **方案A：显式三色状态（white / grey / black）**，而不是沿用当前单一 `mark bit` 同时表示“已发现”和“已扫描”的语义。
>
> **当前实现状态（本轮落地后）**：
> 1. 小对象已引入 `markBitmap + scannedBitmap` 连续分配的三色状态表达。
> 2. 大对象已补 `isScanned`，与小对象三色语义对齐。
> 3. collector 的 drain 语义已从“marked 即跳过”改为“grey -> black”。
> 4. `allocationRoots_` 已从主路径移除，并已从代码中彻底删除。
> 5. `MarkTermination` 的 direct source 已只剩 `WriteBarrierBuffer`；MSGC 主路径不再依赖 `SatbBuffer`。

---

## 目录

1. [背景](#1-背景)
2. [当前实现回顾](#2-当前实现回顾)
3. [Step 8 的核心思路](#3-step-8-的核心思路)
4. [三色状态语义](#4-三色状态语义)
5. [为什么先不碰 newborn region](#5-为什么先不碰-newborn-region)
6. [最小实现方案](#6-最小实现方案)
7. [正确性要求](#7-正确性要求)
8. [预期收益](#8-预期收益)
9. [风险](#9-风险)
10. [验证标准](#10-验证标准)
11. [与后续工作的关系](#11-与后续工作的关系)

---

## 1. 背景

当前 MSGC 的长 STW 尾巴已经通过日志确认，其主因不是 `SatbBuffer`，也不是 `WriteBarrierBuffer`，而是 `allocationRoots_`。

典型现象：

- 长 GC 周期中，`MarkTermination collect/direct breakdown` 显示：
  - `satb = 0`
  - `wb = 4`
  - `allocRoots = 1048576`
- 同一周期中：
  - `MarkTermination total ≈ 6.3s`
  - `ConcurrentMark ≈ 7.3s`
  - `total gc time ≈ 14s`

这说明当前实现的主要问题是：

1. 并发标记期间，新分配对象没有被及时纳入当前周期工作队列。
2. 这些对象被保守地堆积到 `allocationRoots_`。
3. 到 `MarkTermination` 时统一 merge，并在 STW 中做一次巨大的 `DrainMainQueue`。

---

## 2. 当前实现回顾

### 2.1 当前三条路径

当前实现中，“当前周期可能新变为可达的对象”来自三条不同通路：

1. `oldRef -> SatbBuffer`
2. `newRef -> WriteBarrierBuffer`
3. `newObject -> allocationRoots_`

对应语义：

- `oldRef` 保 snapshot-at-the-beginning
- `newRef` 补并发期间新建立的 heap 边
- `newObject` 解决“新对象尚未完全初始化 / 尚未 publish 到 heap 字段”时的保守存活问题

### 2.2 为什么会有 `allocationRoots_`

历史上单独依赖 `mark-at-alloc` 被证明确有正确性风险：

1. 对象在分配时就被标为“已发现”
2. 但它的字段初始化发生在后面
3. 如果 collector 后续把“已发现”误认为“已完成 trace”
4. 那么这个对象后来建立起来的新对象图边就可能漏扫

因此现版本采用了更保守的策略：

- 小对象在标记期分配时不再只靠 `mark-at-alloc`
- 而是记入 `allocationRoots_`
- 到 `MarkTermination` 再统一 merge

这解决了正确性，但将大量本可并发处理的工作压入了 STW。

---

## 3. Step 8 的核心思路

Step 8 的最小实现不先引入 newborn region，而是先将三路来源收敛为两路：

1. `newObject -> alloc-black`
2. `oldRef/newRef -> 同一个并发可 drain 的 grey 队列`

这里的关键不只是“恢复 mark-at-alloc”，而是：

- 不再让单个 `mark bit` 同时表达“已发现”和“已完成扫描”
- 引入显式三色状态：
  - `white`：未发现
  - `grey`：已发现、待 trace children
  - `black`：children 已 trace 完
- 所有新工作来源都统一进入同一个 **per-mutator 可并发 drain 的 grey 队列**
- collector 在并发阶段持续 drain，最后 STW 只做最终 flush + fixed-point

也就是说，Step 8 的本质是：

> 用显式三色状态重建当前周期闭包，使 `allocationRoots_` 不再承担“新对象对象图尚未闭合”的保守兜底职责。

---

## 4. 三色状态语义

### 4.1 对象状态

Step 8 中，对象的 collector 语义至少要区分以下三态：

| 状态 | 含义 |
|------|------|
| `white` | 尚未被当前 GC 发现 |
| `grey` | 已发现，已入队或待 trace，children 尚未完全扫描 |
| `black` | children 已经 trace 完，不应再因“已标记”而被误跳过 |

当前实现的主要语义缺陷正是：

- `mark bit` 只能表达“已发现”
- collector 侧很多路径却把它当成“已完成扫描”

Step 8 通过显式三色状态修复这个语义混淆。

### 4.2 写引用统一语义

对所有引用写操作，统一采用下面的抽象语义：

```text
enqueue(oldRef)
store(newRef)
enqueue(newRef)
```

其中 `enqueue(x)` 的职责不是“把对象直接变黑”，而是：

1. 过滤 `null` / 非堆对象
2. 检查对象当前状态
3. 若对象为 `white`，则原子转换为 `grey`
4. 只有首次变为 `grey` 的对象才推进本地并发可 drain 队列

这里要特别强调：

- `enqueue(x)` **不能**因为对象“已经被标记”就把它视为“已扫描”
- `black` 的设置只能发生在 collector 从队列中取出对象、完成 `TraceObjectRefFields(...)` 之后

### 4.3 新对象语义

Step 8 对标记期新分配对象支持两种语义路线：

#### 方案 A：`newObject -> black`（**优先推荐**）

对标记期新分配对象：

1. 分配完成后不再进入 `allocationRoots_`
2. 对象直接从 `white -> black`
3. 对象本身**不必**再进入本地并发可 drain 队列
4. 后续对象图建立完全依赖 `old/new ref enqueue`

这一路线的成立前提是：

1. 新对象后续所有引用写入都必须经过 barrier
2. barrier 必须保证 `newRef` 都能进入当前周期 grey 队列
3. 不允许存在绕过 barrier 的初始化写入路径

这一路线的优势是：

- 队列流量更小
- 不需要 collector 再主动扫一遍新对象自身
- 更接近 `allocationRoots_` 的彻底退场目标

#### 方案 B：`newObject -> grey`（保守兜底）

对标记期新分配对象：

1. 分配完成后不再进入 `allocationRoots_`
2. 对象直接从 `white -> grey`
3. 立即进入本地并发可 drain 队列
4. collector 后续在并发 drain 中 trace 其 children，再转为 `black`

这一路线的优势是：

- 对 barrier 覆盖率要求更低
- 即使个别初始化写入路径没有完整进入 barrier，也仍有一次 collector 主动扫描的兜底机会

代价是：

- 新对象本身也会进入队列
- 并发 drain 压力更大

在 Step 8 中，这里的 “mark-at-alloc” 更准确应理解为：

> “对象在当前周期被立即纳入 collector 语义”，而不是当前实现里 `allocationRoots_` 那种“拖到 STW 再统一 merge”。

换句话说，当前周期的闭包依赖：

- `newObject -> black` 或 `newObject -> grey queue`
- `enqueue(newRef)`：保证新建立的对象图边被追踪
- `enqueue(oldRef)`：保证 snapshot 语义

**Step 8 优先推荐方案 A（`newObject -> black`）**，理由是：

1. 其目标更直接：优先减少并发队列体积与 `MarkTermination` 尾部残留
2. 如果 barrier 覆盖率被证明完整，它比 `newObject -> grey` 更接近最终优化目标
3. `newObject -> grey` 可作为保守 fallback，而不是默认主路径

---

## 5. 为什么先不碰 newborn region

Step 8 故意不先引入 newborn region，原因有三点：

1. `allocationRoots_` 已经被证明是当前 STW 尾巴的主因，优先级最高。
2. newborn region 涉及 `CentralRegionPool` 四链表、`sweepGen`、`PrepareForSweep()` 等多处语义联动，风险更高。
3. 即使未来要做 newborn region，它也应该建立在“对象级并发收敛模型已经正确”的前提之上，而不是用它代替整个对象级收敛机制。

因此 Step 8 是：

> 先解决对象级三色收敛模型，再考虑 region 级 epoch 隔离。

---

## 6. 最小实现方案

### 6.1 阶段一：引入显式三色状态

当前 `mark bit` 只能表达“已发现”，无法区分：

- 已发现但未 trace（grey）
- 已 trace 完（black）

因此 Step 8 第一阶段必须先补三色状态表达。

#### 6.1.1 推荐实现：`markBitmap + scannedBitmap`

为了把改动压到最小，Step 8 推荐的第一版实现不是改对象头，也不是引入新的对象级元数据结构，而是：

1. **保留现有 `markBitmap` 语义**
   - `mark = 0` → `white`
   - `mark = 1` 且 `scanned = 0` → `grey`
   - `mark = 1` 且 `scanned = 1` → `black`

2. **在现有小对象 region 元数据旁新增一个 `scannedBitmap`**
   - `markBitmap` 继续表达“对象是否已被当前 GC 发现”
   - `scannedBitmap` 只表达“对象 children 是否已经 trace 完”
   - **实现策略**：不在 `MSRegionInfo` 中新增 `scannedBitmap*` 指针，而是在 bitmap 内存布局上采用连续分配：
     ```text
     [ allocBitmap | markBitmap | scannedBitmap ]
     ```
   - 也就是说：
     - `allocBitmap` 仍然保持现有指针字段
     - `markBitmap` 仍然保持现有指针字段
     - `scannedBitmap` 通过 `markBitmap + wordCount` 推导得到
   - 这样可以避免打破 `MSRegionInfo == 64B` 的结构约束，也不需要额外增加对象级或 region 级指针字段

3. **大对象单独处理**
   - 大对象当前已经有 `isMarked`
   - 最小改动版本可为 LARGE 路径单独补一个 `isScanned`
   - 不要求一开始就和小对象完全共享同一套 bitmap 实现

这样三色状态就能表达为：

| 状态 | `markBitmap` | `scannedBitmap` |
|------|--------------|-----------------|
| `white` | 0 | 0 |
| `grey`  | 1 | 0 |
| `black` | 1 | 1 |

#### 6.1.2 为什么这是最小改动方案

相比“重做对象头”或“引入全新状态机对象表”，`markBitmap + scannedBitmap` 的优势是：

1. **不改对象头布局**
   - 不影响 ABI
   - 不影响编译器 fast-path 约定

2. **不重写 `MarkObject()` 的核心职责**
   - `MarkObject()` 仍然只负责 `white -> grey`
   - 现有大量调用点可以继续复用

3. **不改变小对象元数据的归属关系**
   - 仍然由 `MSRegionInfo` / region bitmap 管理
   - 不需要在对象级维护额外哈希表或 side table

4. **collector 改动集中**
   - 主要改 queue pop 后的 `grey -> black`
   - 而不是全量重写 enqueue 逻辑

5. **不需要在 `MSRegionInfo` 中硬塞新的 bitmap 指针**
   - 当前 `MSRegionInfo` 已经卡在 64B cache line
   - 结构中的字段没有足够干净的 8B 冗余位可以直接容纳 `scannedBitmap*`
   - 采用连续分配 + 推导关系，可以在不扩大结构体的前提下表达显式三色状态

#### 6.1.3 collector 侧的最小行为变化

引入 `scannedBitmap` 后，collector 的关键语义变为：

1. **enqueue 阶段**
   - 只尝试把对象从 `white -> grey`
   - 不负责转 `black`

2. **drain 阶段**
   - 如果对象是 `grey`，必须执行 `TraceObjectRefFields(...)`
   - 完成后再把对象从 `grey -> black`

3. **不能继续沿用旧语义**
   - 旧语义常常是“`MarkObject(obj)` 返回已标记就直接跳过”
   - 在 Step 8 里，这会把 `grey` 错当成 `black`

#### 6.1.4 GC 周期起点的清理要求

当前 `ClearAllMarkBitmaps()` 只清 `markBitmap`。

引入显式三色状态后，GC 周期开始时必须同时：

1. 清 `markBitmap`
2. 清 `scannedBitmap`

否则上一轮残留的 `black` 状态会污染本轮 collector 语义。

#### 6.1.5 大对象路径的最小落地建议

小对象推荐使用 `markBitmap + scannedBitmap`。

对于 LARGE 路径，第一版可以采用更简单的配套实现：

1. 保留当前 `isMarked`
2. 额外补一个 `isScanned`

这样不要求一开始就把 large object 也纳入 bitmap 方案，能把 Step 8 的改动范围控制住。

#### 6.1.6 本阶段的最小目标

本阶段唯一要求是：

> collector 必须能够区分“对象是否首次被发现”和“对象是否已经完成 children trace”，且这个区分以最小代价嵌入现有 region/bitmap 体系中。

#### 6.1.7 连续分配方案的具体落地

为了让 `scannedBitmap` 不新增指针字段，推荐按下面的方式落地：

1. **`InitForSizeClass()`**
   - 现在 `MSRegionInfo::InitForSizeClass()` 一次性分配：
     ```text
     totalBytes = bitmapBytes * 2
     [ allocBitmap | markBitmap ]
     ```
   - Step 8 改为：
     ```text
     totalBytes = bitmapBytes * 3
     [ allocBitmap | markBitmap | scannedBitmap ]
     ```
   - 指针关系：
     - `allocBitmap = base`
     - `markBitmap = base + bitmapBytes`
     - `scannedBitmap = markBitmap + bitmapWords`
   - 其中 `scannedBitmap` 不存入 `MSRegionInfo` 字段，而通过 helper 计算：
     ```cpp
     uint64_t* GetScannedBitmap()
     {
         size_t bitmapWords = (totalSlotCount + 63) / 64;
         return markBitmap + bitmapWords;
     }
     ```

2. **`ReleaseBitmaps()`**
   - 释放时仍然只需要 `munmap/VirtualFree(allocBitmap, totalBytes)`
   - 但 `totalBytes` 要从 `bitmapBytes * 2` 改成 `bitmapBytes * 3`
   - 这保证三段 bitmap 仍然作为一整块内存管理

3. **`Reset()` / `ResetToBump()`**
   - 继续只保留 `allocBitmap = nullptr; markBitmap = nullptr;`
   - `scannedBitmap` 不需要单独置空，因为它是推导值
   - `ResetToBump()` 除了清 `markBitmap` 之外，还要额外清 `scannedBitmap`

4. **`ClearAllMarkBitmaps()`**
   - 现在只清 `markBitmap`
   - Step 8 中要改为同时：
     - 清 `markBitmap`
     - 清 `scannedBitmap`
   - 对 LARGE 路径则改成：
     - 清 `isMarked`
     - 清 `isScanned`

5. **对象状态 helper**
   - 推荐新增最小 helper，而不是在调用点手写 bitmap 下标逻辑：
     - `bool IsScannedBit(uintptr_t objAddr) const`
     - `void SetScannedBit(uintptr_t objAddr)`
     - `void ClearScannedBitmap()`
   - 这样 collector 从 `grey -> black` 的转变能集中封装，避免逻辑分散

#### 6.1.8 为什么不用单独指针字段

这个方案的价值就在于：

1. `MSRegionInfo` 继续保持 64B，不打破现有 cache-line 假设
2. 分配/释放生命周期仍然只有“一块 bitmap 内存”，不会引入双重管理
3. `scannedBitmap` 的存在对外是显式状态，对内是隐式推导，改动面最小

### 6.2 阶段二：统一 old/new grey 队列

保留现有 `WriteBarrierBuffer` 这个容器，但语义从“newRef 专用”升级为“old/new 共用的 MutatorGreyQueue”。

此阶段改动：

- `MSMarkBarrier`
  - 不再维持“old -> SatbBuffer，new -> WBBuf”两套主路径
  - 逐步过渡到“old/new 都尝试做 `white -> grey` 并进入同一个并发可 drain 队列”
- `FlushWriteBarrierOverflow`
  - 保持现有 overflow -> global queue 行为

### 6.3 阶段三：恢复小对象 alloc-discover（默认 `newObject -> black`）

在 `AllocBufferMS::Allocate()` 中恢复标记期小对象的“分配即纳入当前周期 collector 语义”，但不再写入 `allocationRoots_`。

此阶段改动：

- `AllocBufferMS`
  - 删除 `PushAllocatedObject()` 主路径调用
  - **默认主路径**：新分配对象直接从 `white -> black`
  - **保守 fallback**：只有当 barrier 覆盖率无法证明完整时，才退回 `white -> grey + 本地队列`
  - `allocationRoots_` 保留一段过渡期，但默认不再增长

这一步的核心原则是：

> 新对象本身不再依赖 `allocationRoots_` 保活，而是依赖 “分配后立即进入 collector 语义 + 后续所有新边都被 barrier 推入 grey 队列”。

### 6.4 阶段四：collector drain 改为 grey -> black

这是 Step 8 正确性最关键的一步。

collector 侧不能再沿用“对象已 marked 就跳过 trace”的旧语义，而必须改成：

1. 从队列中取到 `grey` 对象
2. 一定执行 `TraceObjectRefFields(obj, ...)`
3. trace 完 children 后再将对象转为 `black`

只有这样，前面 enqueue 阶段提前“发现”对象才不会导致后续 children 被跳过。

需要特别说明：

- 对于默认主路径 `newObject -> black`，collector **不负责**主动重新扫描新对象自身
- 它依赖的是：
  - 新对象本身已经 black
  - 后续 `old/new ref` 写入会把相关对象推进 grey 队列
- 因此 drain 逻辑的核心任务是处理 `old/new ref enqueue` 产生的 grey 对象，而不是“兜底重扫所有新对象”

### 6.5 阶段五：MarkTermination 去掉 allocationRoots merge

当阶段二、三、四验证正确后：

- `MarkTermination()`
  - 删除 `MergeAllAllocationRoots()`
  - direct sources 只保留：
    - mutator grey queue flush
    - 可能保留的 old-value snapshot 队列
    - pending global/local work

最终目标：

- `allocationRoots_` 彻底退场
- `MarkTermination collect/direct breakdown` 中 `allocRoots -> 0`

---

## 7. 正确性要求

Step 8 能否成立，取决于以下不变式：

1. 所有引用写路径都必须经过统一的 `white -> grey` enqueue 逻辑
   - 普通写
   - 静态写
   - array copy
   - struct/bulk copy
   - atomic / CAS 写

2. 新分配对象采用的默认策略是 `white -> black`
   - 只有在 barrier 覆盖率不完整的过渡阶段，才允许退回 `white -> grey`

3. queue pop 后必须执行 children trace，再转为 `black`

4. 并发阶段必须持续 drain 本地/全局队列
   - 否则只是把 `allocationRoots_` 的压力换到另一个终止期队列

5. 最终仍保留一次 STW 兜底
   - flush 所有 mutator 本地队列
   - drain 至 fixed-point

---

## 8. 预期收益

如果 Step 8 生效，预期应看到：

1. `MarkTermination collect/direct breakdown`
   - `allocRoots` 快速下降，最终接近 0

2. `MarkTermination total`
   - 从秒级明显回落到毫秒级或亚秒级

3. `ConcurrentMark`
   - 并发阶段对象处理量更多来自 `old/new ref` 推进的 grey 对象
   - assist 调用可能上升，但 STW 尾巴缩短

4. RSS 峰值
   - 不一定立刻显著下降
   - 但峰值前后的“堆住不收敛”窗口应缩短

---

## 9. 风险

### 9.1 正确性风险

如果有任一引用写路径没有进入统一的 `white -> grey` enqueue 逻辑，旧问题会回归：

- 黑对象在后续初始化中建立的新边漏扫

如果默认主路径采用 `newObject -> black`，那么这条风险更强：

- 只要有任何初始化写入绕过 barrier，`newObject -> black` 就会直接暴露漏边问题

### 9.2 状态机风险

如果三色状态实现不完整，会出现两类致命错误：

1. **过早 black**
   - 对象 children 尚未 trace 就被视为 black
   - 新边漏扫

2. **grey/black 去重错误**
   - 对象被重复推进队列
   - work queue 爆炸，拖慢并发收敛

### 9.3 性能风险

如果 `oldRef` 和 `newRef` 都统一入队，但并发 drain 能力不足，则会出现：

- work queue 流量变大
- 只是把 `allocationRoots_` 的压力转移为另一个大队列

### 9.4 编译器路径风险

必须重新核查编译器 fast path / bulk write / runtime helper 是否存在绕开 barrier 的情况。

### 9.5 策略分流风险

如果实现中既支持：

- 默认 `newObject -> black`
- fallback `newObject -> grey`

那么必须明确：

- 哪些分配路径走 black
- 哪些路径强制降级 grey

否则 collector 行为会在不同对象类型间失去一致性，增加诊断难度。

---

## 10. 验证标准

Step 8 的验收不看理论，只看日志与 benchmark：

1. `gc_benchmark_10x.cj` 全部功能测试稳定通过
2. 长 GC 周期中：
   - `allocRoots` 不再成为 direct source 主体
3. `MarkTermination total` 显著下降
4. `F1/F5` 等历史高风险用例不再出现漏扫或随机失败
5. 默认 `newObject -> black` 下，bulk write / HashMap resize / array copy 路径经过验证不会绕过 barrier

---

## 11. 与后续工作的关系

Step 8 完成后，下一步才适合继续评估：

1. newborn region / next-epoch swept 机制
2. old/new 是否仍需严格区分缓冲语义
3. 是否进一步做多轮并发收敛 + 最终短 STW 兜底

也就是说：

> Step 8 不是最终形态，而是以显式三色状态为基础，把当前 `allocationRoots_` 主导的 STW 尾巴先降下来，为后续 region-epoch 级优化创造条件。
