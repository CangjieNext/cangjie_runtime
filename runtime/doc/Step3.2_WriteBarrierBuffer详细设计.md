# Step 3.2：WriteBarrierBuffer 详细设计

> 本文档是 [Step3 混合写屏障详细设计](Step3_混合写屏障详细设计.md) 的子文档。
> 包含 Per-Thread WriteBarrierBuffer 设计及其与 [Step2.2 ConcurrentMark](Step2.2_ConcurrentMark_详细设计.md) 工作队列架构的集成方案。

---

## 目录

1. [为什么需要 Buffer](#1-为什么需要-buffer)
2. [WriteBarrierBuffer 设计](#2-writebarrierbuffer-设计)
3. [与现有 SatbBuffer::Node 的对比](#3-与现有-satbbuffernode-的对比)
4. [Buffer 嵌入位置](#4-buffer-嵌入位置)
5. [Buffer 生命周期管理](#5-buffer-生命周期管理)
6. [与 Step2.2 工作队列架构的集成](#6-与-step22-工作队列架构的集成)
7. [与 Mark Assist 的交互](#7-与-mark-assist-的交互)
8. [性能考量](#8-性能考量)

---

## 1. 为什么需要 Buffer

`MarkAndEnqueue` 中的 `MarkObject` 是原子操作（bitmap CAS），但将灰色对象推入全局工作队列需要加锁。为避免每次写屏障都竞争全局锁，使用 per-thread 的写屏障缓冲区：

```
写屏障 → MarkAndEnqueue(obj)
           → MarkObject(obj)           // 原子 bitmap CAS（无锁）
           → wbBuf.Push(obj)           // 写入线程本地 buffer（无锁）
           → if full: wbBuf.Flush()    // 满时批量转移到工作队列
```

---

## 2. WriteBarrierBuffer 设计

```cpp
// src/Heap/MSBarrier/WriteBarrierBuffer.h

class WriteBarrierBuffer {
public:
    static constexpr size_t kCapacity = 256;  // 256 × 8 = 2KB
    
    WriteBarrierBuffer() : pos_(0) {}
    
    // 推入一个灰色对象（快速路径：无锁）
    void Push(BaseObject* obj) {
        buf_[pos_++] = obj;
        if (UNLIKELY(pos_ >= kCapacity)) {
            Flush();
        }
    }
    
    // 批量转移到工作队列（详见 §6 集成设计）
    void Flush() {
        if (pos_ == 0) return;
        
        // 首先尝试写入 per-P 本地工作队列
        // 如果当前 Mutator 绑定了 P，则走 per-P 路径
        // 否则直接推入全局工作列表
        FlushToWorkQueue(buf_, pos_);
        
        pos_ = 0;
    }
    
    // GC 结束时清空（无需 flush，因为对象已被标记）
    void Clear() {
        pos_ = 0;
    }
    
    bool IsEmpty() const { return pos_ == 0; }
    size_t Size() const { return pos_; }

private:
    void FlushToWorkQueue(BaseObject** batch, size_t count);
    
    BaseObject* buf_[kCapacity];
    size_t pos_;
};
```

---

## 3. 与现有 SatbBuffer::Node 的对比

| 维度 | SatbBuffer::Node | WriteBarrierBuffer |
|------|-----------------|-------------------|
| 存储位置 | 从全局 arena 动态分配 | **内嵌在 Mutator 中**（栈式 buffer，无动态分配） |
| 容量 | 64~69 entries | 256 entries（更大，减少 flush 频率） |
| 满时行为 | retire 到全局 retiredNodes 链表 | 批量 push 到工作队列（per-P 或全局） |
| 对象处理时机 | Mark Termination 时统一遍历 | 随 flush 实时进入工作队列 → GC 线程实时处理 |
| 内存管理 | 需要 arena/page 管理 | 无额外内存管理（生命周期跟随 Mutator） |

---

## 4. Buffer 嵌入位置

在 Mutator 类中增加 WBBuf 成员：

```cpp
// src/Mutator/Mutator.h（修改）
class Mutator {
    // ... 现有成员 ...
    
#ifdef MRT_USE_MARK_SWEEP
    WriteBarrierBuffer wbBuf_;
    
public:
    WriteBarrierBuffer& GetWriteBarrierBuffer() { return wbBuf_; }
#endif
};
```

---

## 5. Buffer 生命周期管理

| 时机 | 操作 |
|------|------|
| Mutator 创建 | WBBuf 自动初始化（pos=0） |
| 每次写屏障 | `Push(obj)` → 可能触发 `Flush()` |
| Mark Termination STW | `FlushAll()`：遍历所有 Mutator，强制 flush 所有 WBBuf |
| GC 结束（进入 IDLE） | `ClearAll()`：清空所有 WBBuf |
| Mutator 退出 | 析构时自动 flush 残留对象 |

```cpp
// Mark Termination 中的 FlushAll
void MarkSweepCollector::FlushAllWriteBarrierBuffers() {
    MutatorManager::Instance().VisitAllMutators([](Mutator& mutator) {
        mutator.GetWriteBarrierBuffer().Flush();
    });
}
```

## 6. 与 Step2.2 工作队列架构的集成

### 6.1 Step2.2 的两级工作队列回顾

[Step2.2 ConcurrentMark](Step2.2_ConcurrentMark_详细设计.md) 定义了如下工作队列架构：

```
per-P 本地工作队列 (localWorkBuf)
  │
  ├── Mutator 标灰的对象 → 写入本地
  ├── GC assist drain 时 → 从本地取出
  │
  └── 满时 / drain 时 ──→ 全局工作列表 (globalWorkList)
                              │
                              ├── GC main thread → 从全局取出 drain
                              └── assist steal → 从全局窃取
```

### 6.2 WBBuf flush 目标的选择

WBBuf 的 `Flush()` 应优先写入当前 Mutator 所绑定的 **per-P 本地工作队列**，而非直接写入全局列表。理由：

1. **减少全局竞争**：per-P 队列无锁，全局列表有锁
2. **数据局部性**：Mutator 产生的灰对象很可能和后续 assist drain 中处理的对象有引用关系
3. **与 assist 机制一致**：Step2.2 的 Mark Assist 本身就从 per-P 队列取对象 drain

```cpp
void WriteBarrierBuffer::FlushToWorkQueue(BaseObject** batch, size_t count) {
    Mutator* m = Mutator::GetMutator();
    if (m == nullptr) {
        // 非 Mutator 上下文（如 finalizer 线程），走全局
        GlobalWorkList::PushBatch(batch, count);
        return;
    }
    
    // 优先写入 per-P 本地工作队列
    Processor* p = m->GetBoundProcessor();
    if (p != nullptr) {
        p->GetLocalWorkBuf().PushBatch(batch, count);
    } else {
        // Mutator 尚未绑定 P（极少数情况），走全局
        GlobalWorkList::PushBatch(batch, count);
    }
}
```

### 6.3 数据流全景

```
Mutator 写引用字段
  │
  ▼
MSWriteBarrier::WriteReference()
  │
  ├── MarkAndEnqueue(old) ──→ MarkObject(bitmap CAS) + WBBuf.Push(old)
  ├── MarkAndEnqueue(new) ──→ MarkObject(bitmap CAS) + WBBuf.Push(new)
  │
  ▼ (WBBuf 满 256 entries)
WriteBarrierBuffer::Flush()
  │
  ├── [有 bound P] → per-P localWorkBuf.PushBatch()
  │                    ↓
  │              GC assist drain / GC main thread 从此取出
  │
  └── [无 bound P] → GlobalWorkList.PushBatch()
                       ↓
                 GC main thread / other P steal 从此取出
```

### 6.4 MarkTermination 中的最终 flush

进入 `MS_PHASE_MARK_TERMINATION`（STW）时，所有 Mutator 已暂停：

```cpp
void MarkSweepCollector::DoMarkTermination() {
    // 1. 强制 flush 所有 WBBuf → 进入 globalWorkList
    //    注意：STW 期间 per-P 本地队列不可用（Mutator 已暂停），统一推全局
    FlushAllWriteBarrierBuffers();
    
    // 2. drain globalWorkList 至清空
    DrainGlobalWorkListToCompletion();
    
    // 3. 如果 drain 过程中没有新增灰对象 → 标记完成
}
```

这替代了 Step2.3 中原来的 `CollectSatbCandidatesToMain()`。

---

## 7. 与 Mark Assist 的交互

### 7.1 问题：WBBuf flush 产生的工作是否计入 assist 债务？

Step2.2 的 Mark Assist 机制中，每个 Mutator 累积分配债务 (`allocDebt`)，需要通过 drain 工作来偿还。

**设计决策**：WBBuf flush 产生的灰对象**不直接计入 assist 债务**。原因：

1. WBBuf flush 只是将**已标记为灰**的对象推入工作队列，**不是 drain 操作**（不递归扫描引用）
2. 实际的 drain 工作（从工作队列取出灰对象 → 扫描其引用字段 → 标记子对象）才应计入 assist 偿债
3. Mutator 的 WBBuf flush 是写屏障的固有开销，不应与 assist 混淆

### 7.2 间接关系

虽然 WBBuf flush 不直接计入 assist，但它**间接增加了工作队列中的待处理对象数量**。这意味着：

- 全局 `gcDebt` 会因为工作队列增长而隐式升高
- Mutator 在下次分配时检查 `allocDebt`，可能触发 assist
- Assist 会 drain per-P 队列中因 WBBuf flush 而新增的灰对象

```
WBBuf.Flush() → per-P 队列增长 → 不影响 allocDebt
                                        ↓
后续分配 → 检查 allocDebt → 触发 assist → drain per-P 队列（含 WBBuf 产生的灰对象）
```

### 7.3 shade(new) 对 assist 工作量的影响

混合写屏障比 SATB 多了 `shade(new)` 操作。这意味着：
- 并发标记期间会有更多对象被提前标灰
- MarkTermination STW 中需要 drain 的残留灰对象更少（因为已被实时处理）
- **整体效果：assist 工作分布更均匀，MarkTermination 停顿时间更短**

---

## 8. 性能考量

- **Push 路径**：1 次数组写 + 1 次比较，约 2 条指令
- **Flush 路径**：批量 memcpy + push（per-P 无锁 / 全局加锁），均摊到每次 push 约 0.5 条指令
- **对比 SatbBuffer**：SatbBuffer::Push 需要 1 次加锁（Node 的 mutex），WriteBarrierBuffer::Push 无锁
- **整体**：写屏障热路径从 ~15 条指令降到 ~5 条指令
- **与 per-P 队列集成**：flush 走 per-P 本地队列时完全无锁，只有极少数无 bound P 的情况才走全局加锁路径
