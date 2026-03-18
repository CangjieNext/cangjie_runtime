# MarkSweep F1 新引用未入队导致对象图漏扫修复

## 1. 背景

本文记录 `gc_benchmark.cj` / `gc_benchmark_f1_*` 在 MarkSweep 模式下的定位过程、关键结论、代码修改与当前验证状态。

## 2. 现象

### 2.1 原始失败现象

MarkSweep 模式下，`doc/benchmark/gc_benchmark.cj` 无法稳定通过功能测试：

- 早期失败表现为 `F1_TreeIntegrity` 直接 `FAIL`
- 常见错误是 `treeChecksum(...)` 深递归后触发 `StackOverflowError`
- 另一些版本表现为 `countNodes()` 返回值变小或变大，出现 `node mismatch`

CopyGC 基线下同一份 benchmark 可以完整通过，因此问题明确位于 MarkSweep runtime，而不是 benchmark 本身。

### 2.2 最小复现

为了把完整 benchmark 的噪声去掉，先后构造了两个最小用例：

- `doc/benchmark/gc_benchmark_f1_rooted_build.cj`
  - 构造中的每个 `TreeNode` 都放进全局 `ArrayList<TreeNode>`
  - 用于验证“对象已经被全局根持有”时是否仍会损坏
- `doc/benchmark/gc_benchmark_f1_unrooted.cj`
  - 基本等价于 benchmark 里的 `F1_TreeIntegrity`
  - 不额外 pin 节点，更接近真实场景

在修复前，这两个最小用例都会在第 3/4 轮附近开始出现：

- `node mismatch`
- 或 `treeChecksum` 中的 `StackOverflowError`

### 2.3 关键排除项

本轮定位中先后确认了以下事实：

- `TreeNode.left/right` 的元信息正确，`ForEachRefField` 能稳定看到 `refs=2`
- `ArrayList<default:TreeNode>` 与底层 `RawArray<default:TreeNode>` 也确实能被枚举
- `nullRegion/staleBase` 计数最终为 0，说明当前主问题已经不是
  - `msInfo == nullptr`
  - stale large base
  这条分支

这意味着：根集和对象枚举本身并没有直接缺失，真正的问题在于并发标记期间“新形成的对象图边”没有形成完整的 trace 闭包。

## 3. 根因分析

### 3.1 现象与写屏障统计的对应关系

在 `MSWriteBarrier.cpp` 中增加写屏障统计后，得到如下关键现象：

- `phaseSkip = 0`
  - 说明写屏障并没有因为 phase 错误而被整体跳过
- `alreadyMarked` 很高，而 `enqueued` 极低
  - 说明大量新引用对象在写屏障执行时已经是 marked 状态
  - 但它们并没有被真正送入 trace 队列

这与当前 MarkSweep 的分配语义是吻合的：

1. `AllocBufferMS::Allocate()` 在标记期调用 `MarkNewObject()`
2. 新对象在分配时就被置为“已标记”
3. 后续 `MCC_WriteRefField -> MSWriteBarrier::WriteReference()` 写入 `newRef` 时
4. `RegionSpace::MarkObject(newRef)` 会返回 “already marked”
5. 结果 `newRef` 不再进入 `WriteBarrierBuffer / mark queue`

问题在于：

**“对象已被标黑”不等于“对象已经被 trace 过”。**

如果一个对象在分配时被 mark-at-alloc 标黑，但在它的字段真正初始化完成之后，从未被放入 trace 队列，那么它内部后来建立起来的对象图边仍然可能完全漏扫。

### 3.2 对应到 F1 的具体时序

以 `buildTree(depth)` 为例，真实时序是：

1. 分配父节点 `node`
2. `MarkNewObject(node)` 把它设为 marked
3. 递归构造子树
4. 回来后执行：
   - `node.left = Some(...)`
   - `node.right = Some(...)`
5. 写屏障此时如果只做 SATB `shade(old)`，或者对 `newRef` 看到 “already marked” 就跳过
6. 那么这些新子树未必会被完整 trace

这正是之前 `F1` 出现：

- 节点数变少
- 校验和变化
- 最终形成环或异常共享，导致 `treeChecksum` 栈溢出

### 3.3 为什么文档里那两处修复不够

此前还修过两处独立问题：

- `MarkSweepCollector::MarkObject()` 中 `msInfo == nullptr` 时错误返回 `false`
- LARGE 分支缺少 stale base 守卫

它们确实能修掉一部分 `DrainSlice invalid TypeInfo / stale pointer` 问题，但对当前主问题不足：

- 在新的统计中，`nullRegion = 0`
- `staleBase = 0`

说明当前剩余错误不是因为对象地址解析失败，而是因为 **写屏障与 mark-at-alloc 的配合语义不完整**。

## 4. 代码修改

### 4.1 已保留的修改

以下修改已经确认是合理且需要保留的：

1. `src/Heap/Allocator/AllocBufferMS.cpp`
   - 小对象复用后对对象体做 `memset`
   - 避免复用 slot 残留旧引用

2. `src/Mutator/Mutator.cpp`
3. `src/Heap/Allocator/AllocBufferMS.h`
4. `src/Heap/Allocator/CentralRegionPool.h`
5. `src/Heap/Collector/TracingCollector.cpp`
   - MS 下将栈根暂存到 `AllocBufferMS`
   - `MergeMutatorRoots()` 从 `AllocBufferMS` 合并

6. `src/Heap/Collector/MarkSweepCollector.cpp`
7. `src/Heap/Allocator/RegionSpace.cpp`
   - `msInfo == nullptr` 不再按 “newly marked” 处理
   - LARGE 分支增加 stale base 守卫
   - 这部分主要修 stale pointer / DrainSlice 分支

### 4.2 本轮关键修复

真正命中 F1 主问题的是 `src/Heap/Barrier/MSWriteBarrier.cpp`。

修复思路经历了三步收敛：

1. `shade(new)` 直接对 `newRef` 调 `MarkAndEnqueue`
   - 能明显改善 F1
   - 但很多 `newRef` 因为 mark-at-alloc 已经是 marked，仍不会真正入队

2. 粗暴版：对所有 `newRef` 保守入队
   - 最小 F1 通过
   - 但完整 benchmark 入队过多，副作用明显

3. 当前收敛版：对象字段写入时 enqueue 容器对象本身以触发重扫
   - 对普通对象字段写入：
     - 保留 `oldRef` 的 SATB 处理
     - 对 `newRef` 不再直接依赖 “是否 already marked”
     - 改为把容器对象 `obj` 放入 `WriteBarrierBuffer`
   - 对静态根写入：
     - 没有容器对象可重扫，仍直接 enqueue `newRef`
   - 对 struct/array bulk copy 路径：
     - 暂时不采用粗暴保守入队，避免完整 benchmark 过度放大工作量

可以把这版语义理解为：

> “当并发标记期间一个对象获得了新引用，不要求新引用对象一定从白变灰；但要求承载这个新边的对象后续一定会被重扫一次。”

这比“无条件 enqueue newRef”更接近需要的闭包语义，也更容易控制副作用。

## 5. 验证结果

### 5.1 最小用例

当前版本下：

- `doc/benchmark/gc_benchmark_f1_rooted_build.cj`：通过
- `doc/benchmark/gc_benchmark_f1_unrooted.cj`：通过

说明原始的 F1 树图损坏问题已经被修掉。

### 5.2 完整 benchmark

当前版本下完整 `doc/benchmark/gc_benchmark.cj` 的功能测试结果已经推进到：

- `F1_TreeIntegrity PASS`
- `F2_Generational PASS`
- `F3_LargeObject PASS`
- `F4_LinkedList PASS`

也就是说，原先最核心的树结构损坏问题已经不再阻塞前四项功能测试。

### 5.3 尚未完全通过

完整 benchmark 仍然没有 100% 通过，剩余失败项还需要继续定位。

当前可以确认的是：

- 问题已经不再停留在原始 F1 根因上
- 剩余问题已经转移到 benchmark 的后续阶段
- 下一步需要明确是 `F5_HashMap`，还是后续性能阶段中的某项失败

## 6. 当前结论

截至当前版本，可以给出以下结论：

1. 原始主问题不是元信息缺失，也不是 `TreeNode`/`ArrayList` 没被枚举。
2. 原始主问题的关键根因是：
   - mark-at-alloc 把新对象提前标黑
   - 写屏障又没有保证这些“已标黑但尚未 trace”的对象重新进入 trace 闭包
3. 文档里 stale pointer / `msInfo==nullptr` 的修复是对的，但只覆盖了旁支问题，不是主根因。
4. 当前“对象字段写入时重扫容器对象”的版本已经实证修掉：
   - `F1_TreeIntegrity`
   - `F2_Generational`
   - `F3_LargeObject`
   - `F4_LinkedList`

因此，这一版修复已经具备合并价值，但在正式收口前还需要把完整 benchmark 的剩余失败项处理掉。

## 7. 后续工作

1. 继续读取完整 benchmark 在 `F4` 之后的失败项，确认是否落在 `F5_HashMap`
2. 若是 `HashMap` 路径失败：
   - 再抽一个对应的最小复现
   - 检查 `ArrayList` / `HashMap` 内部容器写入是否仍需更细粒度重扫
3. 若是性能阶段失败：
   - 区分 correctness 与性能问题
   - correctness 优先，性能优化另开问题单
