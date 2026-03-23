# CopyGC vs MSGC 时序图

## 1. 文档目标

本文只按当前源码描述 `CopyGC` 与 `MSGC` 的真实运行时序，不按早期设计稿推演。

范围覆盖：
- 构建期开关如何切到两套 GC
- 运行时如何安装 collector / barrier
- 一轮 GC 的主阶段顺序
- `CopyGC` 与 `MSGC` 在 barrier、对象移动、回收方式上的真实差异

主要源码入口：
- `build.py`
- `config.cmake`
- `CMakeLists.txt`
- `src/Heap/Heap.cpp`
- `src/Heap/Collector/CollectorProxy.cpp`
- `src/Heap/WCollector/WCollector.cpp`
- `src/Heap/Collector/MarkSweepCollector.cpp`
- `src/Heap/Barrier/MSSatbBarrier.cpp`
- `src/Heap/Allocator/CentralRegionPool.cpp`

## 2. 结论摘要

当前仓库里的两套 GC 可以概括成下面两句话：

- `CopyGC` 是典型的移动式 regional copying collector，核心阶段为 `ENUM -> TRACE -> POST_TRACE -> PREFORWARD -> FORWARD -> IDLE`，依赖 tagged pointer、`FindToVersion()`、Forward 和 6 套 barrier。
- `MSGC` 是非移动式 mark-sweep collector，核心阶段为 `MARK_SETUP -> CONCURRENT_MARK -> MARK_TERMINATION -> POST_MARK_TERMINATION -> MS_SWEEP`，当前实际生效的写屏障是 `MSSatbBarrier`，不是设计文档中大量描述的 `MSWriteBarrier` 主路径。

也就是说，当前源码里的 `MSGC` 已经不是最早设计稿里的 “hybrid barrier + WriteBarrierBuffer 主收敛” 版本，而是更保守的：

- `MSIdleBarrier + MSSatbBarrier`
- `SatbBuffer + globalWorkQueue`
- `mark-at-alloc`
- `CentralRegionPool` 的 lazy/background/force sweep 三路协作

## 3. 统一入口

### 3.1 构建期开关

构建脚本通过 `--gc-mode` 在两套 GC 间切换：

- `copygc` -> `-DCOPYGC_FLAG=1 -DMARKSWEEP_FLAG=0`
- `marksweep` -> `-DCOPYGC_FLAG=0 -DMARKSWEEP_FLAG=1`

对应源码：
- `build.py:112-117`
- `config.cmake:29-49`
- `CMakeLists.txt:208-213`

### 3.2 Collector 安装

`CollectorProxy` 在编译期决定当前进程里到底装哪一个 collector：

- `MRT_USE_MARK_SWEEP` 下装 `MarkSweepCollector`
- 否则装 `WCollector`

对应源码：
- `src/Heap/Collector/CollectorProxy.cpp:11-24`
- `src/Heap/Collector/CollectorProxy.cpp:35-56`

### 3.3 Barrier 安装

`HeapImpl` 也是按编译期开关装 barrier：

- `CopyGC`: `IdleBarrier / EnumBarrier / TraceBarrier / PostTraceBarrier / PreforwardBarrier / ForwardBarrier`
- `MSGC`: `MSIdleBarrier / MSSatbBarrier`

关键事实：

- `src/Heap/Heap.cpp:65-69` 构造函数里，MS 分支实例化的是 `msIdleBarrier` 和 `msSatbBarrier`
- `src/Heap/Heap.cpp:159-161` 成员字段也是这两套
- `src/Heap/Barrier/CMakeLists.txt:11-13` 在 MS 模式下只把 `MSSatbBarrier.cpp` 编进 `Barrier` 库

因此，当前产物里 `MSWriteBarrier.cpp` 不是主路径的一部分。

## 4. CopyGC 时序图

### 4.1 时序总览

```text
应用线程/分配路径
  -> RegionSpace::Allocate
  -> AllocBuffer / RegionManager
  -> 达到阈值后 RequestGC

GC 主线程
  -> CollectorProxy::RunGarbageCollection
  -> WCollector::RunGarbageCollection
  -> WCollector::DoGarbageCollection
       1. TraceHeap
          1.1 TransitionToGCPhase(ENUM)
          1.2 DoEnumeration
          1.3 TransitionToGCPhase(TRACE)
          1.4 PrepareTrace
          1.5 DoTracing
          1.6 ProcessFinalizers
       2. PostTrace
          2.1 TransitionToGCPhase(POST_TRACE)
          2.2 HandleTraceRegions
          2.3 ClearWeakRefBuffer
          2.4 Clear SatbBuffer
          2.5 CollectLargeGarbage / CollectPinnedGarbage
          2.6 RefineFromSpace
          2.7 PrepareForwardTable
       3. Preforward
       4. ForwardFromSpace
       5. TransitionToGCPhase(IDLE)
       6. FlipTagID
       7. CollectSmallSpace
```

对应源码：
- `src/Heap/WCollector/WCollector.cpp:370-410`
- `src/Heap/WCollector/WCollector.cpp:527-545`

### 4.2 CopyGC 的关键运行语义

1. 根枚举和 trace 都要处理 tagged pointer。
2. 读 barrier 不只是“读”，还会做 untag、fixup，甚至在后期触发 forward。
3. GC 后半段存在真实的对象移动：
   - `Preforward`
   - `ForwardFromSpace`
4. GC 收尾后会 `FlipTagID()`，下一轮再用另一套 tag 区分新旧引用。

### 4.3 CopyGC 的 barrier 时序

```text
IDLE
  -> IdleBarrier

ENUM
  -> EnumBarrier
  -> 读时解析 tag / old pointer
  -> 写时 SATB(old + new) + tag 编码

TRACE
  -> TraceBarrier
  -> 读时解析 tag / FindToVersion
  -> 写时 SATB(old) + tag 编码

POST_TRACE
  -> PostTraceBarrier

PREFORWARD
  -> PreforwardBarrier

FORWARD
  -> ForwardBarrier
  -> 读时可能真正触发对象 forward
```

这一套复杂度来自一个根因：`CopyGC` 会移动对象，所以任何阶段都必须回答“这个指针指的是旧版本还是新版本”。

## 5. MSGC 时序图

### 5.1 时序总览

```text
应用线程/分配路径
  -> RegionSpace::Allocate
  -> TryAllocateOnceMS
     -> 小中对象: AllocBufferMS -> CentralRegionPool
     -> 大对象: MSRegionManager::TakeRegionForLarge
  -> 达到阈值后 RequestGC

GC 主线程
  -> CollectorProxy::RunGarbageCollection
  -> MarkSweepCollector::RunGarbageCollection
  -> MarkSweepCollector::DoGarbageCollection
       1. MarkSetup
          1.1 SatbBuffer::Init
          1.2 WaitForSweepDone
          1.3 STW: FlushAllAllocBuffers + ClearLargeMarks + reset marked counter
          1.4 TransitionToGCPhase(MS_MARK_SETUP)
          1.5 EnumAndMarkRoots
       2. ConcurrentMark
          2.1 TransitionToGCPhase(MS_CONCURRENT_MARK)
          2.2 开启 assist
          2.3 RunConcurrentMarkLoop
       3. MarkTermination
          3.1 STW
          3.2 DrainSatbBufferToMainQueue
          3.3 drain globalWorkQueue
          3.4 extern / resurrection candidates 入队
          3.5 ExecuteLikeDrainMainQueue
          3.6 STW 内切到 MS_SWEEP
          3.7 Clear SatbBuffer
       4. PostMarkTermination
          4.1 ClearWeakRefBuffer
          4.2 ProcessFinalizers
          4.3 CollectLargeGarbage
       5. ConcurrentSweep
          5.1 STW: FlushAllAllocBuffers + PrepareForSweep
          5.2 NotifyToSweep
          5.3 NotifyToReclaimGarbage
```

对应源码：
- `src/Heap/Collector/MarkSweepCollector.cpp:68-115`
- `src/Heap/Collector/MarkSweepCollector.cpp:126-210`
- `src/Heap/Collector/MarkSweepCollector.cpp:522-590`
- `src/Heap/Collector/MarkSweepCollector.cpp:808-846`

### 5.2 MSGC 的关键运行语义

1. 对象不移动，因此：
   - 没有 `Forward`
   - 没有 `FindToVersion` 主路径
   - 读操作基本都是直接 load
2. `MarkSetup` 不是纯 handshake，而是：
   - 先等 sweep 收敛
   - 再做一次短 STW，把所有 `AllocBufferMS` 归还到 `CentralRegionPool`
   - 然后再进入 `MS_MARK_SETUP`
3. `MarkTermination` 在 STW 内已经把 phase 切到 `MS_SWEEP`，这是当前代码的关键安全边界。
4. `ConcurrentSweep` 已经接了后台 sweep 唤醒，不再是旧 review 文档里说的“未接线”状态。

### 5.3 MSGC 的 barrier 时序

```text
IDLE / MS_SWEEP
  -> MSIdleBarrier
  -> 读写都直接访问，不做 tag/fixup

MS_MARK_SETUP / MS_CONCURRENT_MARK / MS_MARK_TERMINATION
  -> MSSatbBarrier
  -> 读操作仍然是直接 load
  -> 写操作只做 shade(old) + store
  -> old value 进入 SatbBuffer
```

对应源码：
- `src/Heap/Barrier/MSSatbBarrier.cpp:25-92`
- `src/Heap/Allocator/RegionSpace.h:320-326`
- `src/Mutator/SatbBuffer.cpp:18-23`

这里要特别强调：

- 当前源码主路径是 `MSSatbBarrier`
- 不是 `MSWriteBarrier`
- `WriteBarrierBuffer` 和 `MSWriteBarrier` 代码还在，但对当前 MS 产物来说属于保留/遗留实现

## 6. 关键差异对照

| 维度 | CopyGC | MSGC |
|------|--------|------|
| 对象是否移动 | 会移动 | 不移动 |
| 主 collector | `WCollector` | `MarkSweepCollector` |
| 主要阶段 | `ENUM -> TRACE -> POST_TRACE -> PREFORWARD -> FORWARD` | `MARK_SETUP -> CONCURRENT_MARK -> MARK_TERMINATION -> POST_MARK -> MS_SWEEP` |
| 读屏障 | 全流程存在 | 当前主路径无读屏障 |
| 写屏障 | SATB + tag 编码 | 当前主路径是纯 SATB deletion barrier |
| 指针语义 | tagged pointer + `FindToVersion` | 原始指针 |
| 小对象分配 | thread-local region bump | `AllocBufferMS + SizeClass + CentralRegionPool` |
| 回收方式 | 复制 / compact / from-space 回收 | mark + sweep，依赖 `PrepareForSweep` 和 lazy/background/force sweep |
| 大对象 | CopyGC 自己的 large/pinned 路径 | `>=16KB` 直接走 `MSRegionManager` 的 large region |
| 复杂度来源 | forward / tagged pointer / 多 barrier | size class / bitmap / sweep 协调 |

### 6.1 CopyGC 的时序优势

- 回收后空间释放直接
- 碎片少
- 重压和大堆场景下更稳定

### 6.2 MSGC 的时序优势

- 没有 forward
- 读路径更轻
- GC 阶段数量更少
- 在合适工作负载下可以用并发 mark/sweep 降低停顿和 RSS

## 7. 当前源码与设计文档的主要偏差

### 7.1 Barrier 偏差最大

很多 `doc/Step3*`、`doc/Step7*`、`doc/MS_CodeReview*` 文档把当前 MS 描述成：

- `MSIdleBarrier + MSWriteBarrier`
- `WriteBarrierBuffer`
- hybrid barrier

但当前源码事实是：

1. `Heap.cpp` 装的是 `MSSatbBarrier`
2. `Barrier/CMakeLists.txt` 只编了 `MSSatbBarrier.cpp`
3. `MarkSweepCollector` 里与 `WriteBarrierBuffer` 相关的几个入口已经变成 legacy stub

### 7.2 MarkSetup 边界比设计更保守

设计稿里很多地方强调 “纯 handshake / 近乎零 STW”，当前实现则明确采用：

- `WaitForSweepDone`
- 短 STW flush alloc buffers
- STW 下处理 large region mark 清理

这是一个明显的工程化收敛。

### 7.3 Sweep 接线状态比部分 review 文档更新

有些 review 文档记录过 “`NotifyToSweep()` 未接线”，但当前源码里：

- `MarkSweepCollector::ConcurrentSweep()` 已调用 `fp.NotifyToSweep()`
- `FinalizerProcessor::Run()` 已在 wake loop 里执行 `BackgroundSweepLoop()`

因此“后台 sweep 已实现但未接线”已经不是当前状态。

### 7.4 大对象路由与很多文档描述不一致

当前源码真实分界是：

- `allocSize >= kLargeObjectThreshold(16KB)` 就进入 large path
- 不是文档里常见的“只有 >4MB 才切出主堆”

`LargeObjectSpace` 仍然存在并被 mark path 兼容，但我在当前主分配入口里没有看到它作为 `TryAllocateOnceMS()` 的常规分配终点。

## 8. 结语

如果只想用一句话记住当前源码：

```text
CopyGC = 移动式 + tagged pointer + forward + 6 barriers
MSGC   = 非移动式 + size class/bitmap + MSSatbBarrier + sweepGen 协调清扫
```

理解当前实现时，应优先信源码，其次再参考 `doc/benchmark/` 和较新的复核文档；早期 Step 设计稿更适合当“演进背景”，不适合直接当“现状说明”。
