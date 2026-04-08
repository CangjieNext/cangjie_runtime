# Step 3.2：WriteBarrierBuffer 详细设计

> 本文档是 [Step3 混合写屏障详细设计](Step3_混合写屏障详细设计.md) 的子文档。
> 2026-04-05 按当前源码校准：`WriteBarrierBuffer` 已经是运行中的 `MSMarkBarrier` 组成部分，但它的真实行为不是“优先写 per-P localWorkBuf”，而是“Mutator 本地缓存 + overflow 刷入 collector 全局队列 + STW 刷入主队列”。

---

## 目录

1. [当前职责](#1-当前职责)
2. [真实数据结构](#2-真实数据结构)
3. [当前调用路径](#3-当前调用路径)
4. [生命周期与 flush 语义](#4-生命周期与-flush-语义)
5. [与并发标记/assist 的关系](#5-与并发标记assist-的关系)
6. [与 SatbBuffer 的当前关系](#6-与-satbbuffer-的当前关系)
7. [性能与约束](#7-性能与约束)

---

## 1. 当前职责

`WriteBarrierBuffer` 的职责非常收敛：

1. 作为 **每个 Mutator 私有的固定容量灰对象缓存**。
2. 避免每次写屏障都直接竞争 `MarkSweepCollector::globalWorkQueue_`。
3. 在 `MarkTermination` 进入 STW 后，把各个 Mutator 尚未外发的灰对象统一回灌到主 `WorkStack`。

当前实现不是“buffer 满了自动选择本地队列或全局队列”。真实语义是：

- `Push()` 只向本地数组写入。
- overflow 由 `MSMarkBarrier::EnqueueRef()` 检测，并调用 `activeCollector->FlushWriteBarrierOverflow(wbBuf)`。
- `Flush(container)` 只把条目逐个 `push_back` 到调用者提供的容器。

因此，`WriteBarrierBuffer` 自身并不知道 `Processor`、`localWorkBuf` 或 `global queue` 的概念；这些策略都在 collector / barrier 层实现。

---

## 2. 真实数据结构

当前代码位于 `src/Heap/MBarrier/WriteBarrierBuffer.h`：

```cpp
class WriteBarrierBuffer {
public:
    static constexpr size_t kCapacity = 256;

    WriteBarrierBuffer() : count_(0) { ... }

    inline void Push(BaseObject* obj)
    {
        size_t idx = count_;
        if (LIKELY(idx < kCapacity)) {
            buf_[idx] = obj;
            count_ = idx + 1;
        }
    }

    template<typename Container>
    inline void Flush(Container& workStack)
    {
        size_t n = count_;
        for (size_t i = 0; i < n; ++i) {
            if (buf_[i] != nullptr) {
                workStack.push_back(buf_[i]);
            }
        }
        count_ = 0;
    }

    inline void Clear() { count_ = 0; }

    size_t Count() const { return count_; }
    bool IsEmpty() const { return count_ == 0; }
    bool IsFull() const { return count_ >= kCapacity; }
};
```

关键点：

- 容量固定为 `256`，在 64 位平台上约 `2KB`。
- `Push()` **不会自动 flush**，满了以后只是拒绝继续写入，因此调用方必须先检查 `IsFull()`。
- `Flush()` 是模板函数，目标容器只需要支持 `push_back(BaseObject*)`。
- buffer 没有内部锁；它依赖“单 Mutator 单写者”这个并发前提。

---

## 3. 当前调用路径

### 3.1 `MSMarkBarrier::EnqueueRef()` 才是真实的策略入口

当前 marksweep 写屏障不是 old/new 分流到两个不同结构，而是 old/new 都走同一条 `EnqueueRef()` 路径：

```text
WriteReference / WriteStaticRef / WriteStruct
  -> EnqueueRef(ref)
     -> collector.MarkObject(ref)
     -> 如果是 newly marked:
          1. 取当前 Mutator 的 WriteBarrierBuffer
          2. 若已满，先 FlushWriteBarrierOverflow(wbBuf)
          3. wbBuf.Push(ref)
```

源码行为要点：

1. `MarkObject(ref)` 先做真正的标记位设置。
2. 只有“本次新标记成功”的对象才会进入 `WriteBarrierBuffer`。
3. 这样 `WriteBarrierBuffer` 中存放的是“需要后续 trace children 的灰对象”，而不是单纯的写入历史。

### 3.2 overflow 的真实去向

当本地 buffer 满时，当前实现调用：

```cpp
activeCollector->FlushWriteBarrierOverflow(wbBuf);
```

而 `FlushWriteBarrierOverflow()` 的行为是：

```cpp
std::lock_guard<std::mutex> lock(globalWorkMutex_);
wbBuf.Flush(globalWorkQueue_);
```

也就是说，**overflow 当前直接刷入 `MarkSweepCollector::globalWorkQueue_`**，并不是 per-P 本地工作队列。

---

## 4. 生命周期与 flush 语义

### 4.1 所属位置

`WriteBarrierBuffer` 当前已经常驻在 `Mutator` 中：

```cpp
WriteBarrierBuffer wbBuf_;
WriteBarrierBuffer& GetWriteBarrierBuffer() { return wbBuf_; }
```

这不是未来设计，而是当前布局的一部分。

### 4.2 运行期生命周期

| 时机 | 当前行为 |
|------|----------|
| Mutator 创建 | `wbBuf_` 构造，`count_=0` |
| 并发标记期间写屏障 | `EnqueueRef()` 在 newly-marked 时调用 `Push()` |
| buffer 满 | collector 在 barrier 路径上把旧条目刷到 `globalWorkQueue_` |
| `MarkTermination` STW | `FlushAllWriteBarrierBuffers(mainQueue)` 把所有 Mutator 的 WBBuf 刷到主队列 |
| GC 结束后 | 当前主流程不依赖专门的 `ClearAllWriteBarrierBuffers()` 阶段；`Flush()` 已把残留条目清空 |

### 4.3 `MarkTermination` 中的真实角色

`MarkSweepCollector::MarkTermination()` 当前把 `WriteBarrierBuffer` 视为“直接 mutator/barrier source”：

```text
1. FlushAllWriteBarrierBuffers(mainQueue)
2. FlushAllAssistLocalWorks(mainQueue)
3. DrainGlobalWorkQueueToMainQueue(mainQueue)
4. CollectResurrectionCandidatesToMain(mainQueue)
5. ExecuteLikeDrainMainQueue(mainQueue)
```

其中第 1 步就是把所有 `WriteBarrierBuffer` 条目统一转移到 STW 下的 `mainQueue`。

---

## 5. 与并发标记/assist 的关系

`WriteBarrierBuffer` 和 assist-local work 是两套不同结构：

| 结构 | 来源 | 内容 | 何时消费 |
|------|------|------|----------|
| `WriteBarrierBuffer` | 写屏障 `EnqueueRef()` | newly-marked 灰对象 | overflow 时入全局；STW 时统一 flush |
| `assistLocalWork` | `MSMarkAssist()` | Mutator assist 正在/待继续 drain 的工作 | 并发期继续消费；STW 时统一并入主队列 |

因此当前关系是：

1. `WriteBarrierBuffer` 负责把屏障新产生的灰对象延迟批量外发。
2. `MSMarkAssist()` 负责消费 `globalWorkQueue_` 中的工作，并把自己未做完的局部工作保存在 `assistLocalWork_`。
3. `MarkTermination` 统一回收这两类残留。

当前并不存在“WBBuf flush 到 per-P localWorkBuf，再由 assist 就地消费”的实现。

---

## 6. 与 SatbBuffer 的当前关系

`SatbBuffer` 仍然在 runtime 中存在，但它已经不是当前 MSGC 写屏障直接主路径的核心组成部分。

当前差异应这样理解：

| 维度 | SatbBuffer | WriteBarrierBuffer |
|------|------------|-------------------|
| 当前在 MSGC 中的地位 | 保留基础设施，部分 tracing/兼容逻辑仍在 | `MSMarkBarrier` 当前活跃路径的一部分 |
| 记录内容 | 历史上偏向 old-value/SATB 语义 | 当前记录 newly-marked 灰对象 |
| 收敛方式 | 旧设计主要在 termination 统一遍历 | 当前 overflow 实时进全局队列，STW 再做最终 flush |
| 所属位置 | arena + node 管理 | `Mutator` 内嵌固定数组 |

因此，本文档不再把 `WriteBarrierBuffer` 叙述为“将来可能替换 SatbBuffer 的方案”；它已经是当前 marksweep barrier 的真实组件。

---

## 7. 性能与约束

### 7.1 当前收益

1. 热路径上的本地写入只有数组赋值和计数递增。
2. 大部分写屏障不会直接碰 `globalWorkMutex_`。
3. STW 时的 flush 只是把残留对象批量并入 `mainQueue`，逻辑简单、确定性高。

### 7.2 当前约束

1. `Push()` 不负责溢出处理，调用方必须在 `IsFull()` 后先 flush。
2. `Flush()` 只负责“把已有条目倒到外部容器”，不会做 dedup、mark 或路由判断。
3. 该结构默认单写者；不能把它误写成多线程共享队列。

### 7.3 文档结论

当前代码中，`WriteBarrierBuffer` 的准确定位是：

> `MSMarkBarrier` 下的每 Mutator 灰对象暂存区，overflow 走 collector 全局工作队列，`MarkTermination` 再把所有残留统一并入主队列完成收敛。
