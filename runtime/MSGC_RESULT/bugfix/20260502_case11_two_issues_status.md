# MSGC 两类问题阶段性记录

- 日期：2026-05-02
- 范围：MSGC 并发 sweep 路径、HashMap stale edge 路径
- 相关文件：
  - [src/Heap/MCollector/MarkSweepCollector.cpp](../../src/Heap/MCollector/MarkSweepCollector.cpp)
  - [src/Heap/MAllocator/MSCentralRegionPool.cpp](../../src/Heap/MAllocator/MSCentralRegionPool.cpp)
  - [src/Heap/MAllocator/MSRegionManager.cpp](../../src/Heap/MAllocator/MSRegionManager.cpp)
  - [src/CompilerCalls.cpp](../../src/CompilerCalls.cpp)
  - [src/Heap/MBarrier/MSMarkBarrier.cpp](../../src/Heap/MBarrier/MSMarkBarrier.cpp)

## 1. 问题一：并发 sweep 与 mark/Drain 交叠

### 1.1 现象

在 `case2_50x_pressure` / `F5_HashMap` 压测中，曾出现过以下异常：

- `DrainGCWork: FATAL invalid stateWord`
- 扫描过程中读到 stale heap pointer
- 在已 discover / 已入队但尚未完成 Drain 的对象上，后续观察到对象内容或元数据异常

### 1.2 根因判断

问题不在主写屏障协议本身，而在 **并发 sweep 线程仍可能与下一轮 mark/Drain 窗口交叠**：

1. 某些对象或边已经被 mark 线程 discover，准备后续 Drain；
2. 同一批 region 上仍残留 background sweep / lazy sweep；
3. sweep 物理清扫或清零发生在对象图尚未完全闭合之前；
4. 后续 Drain 或字段扫描再访问这些对象时，就会看到坏 `stateWord`、坏 typeinfo、或 stale edge。

### 1.3 修改方案

为了把 sweep 完全移出 mark 窗口，当前代码做了 4 个配套 gate：

1. 在 [src/Heap/MCollector/MarkSweepCollector.cpp](../../src/Heap/MCollector/MarkSweepCollector.cpp) 的 `PreEnumRoots()` 中固定执行：

```cpp
StopAndWaitForAllSweep();
ClearAllMarkBitmaps();
ReclassifyUnsweptToSwept();
PrepareForSweep();
```

2. 在 [src/Heap/MAllocator/MSCentralRegionPool.cpp](../../src/Heap/MAllocator/MSCentralRegionPool.cpp) 的 `StopAndWaitForAllSweep()` 中，当 `CJ_MS_STW_SWEEP=1` 时，等待 background sweep 和 lazy sweep 都 quiesce 后，直接调用 `ForceFinishAllUnswept()`，由 GC 线程在 `PreEnumRoots()` 内单线程物理清扫 backlog。

3. 在 [src/Heap/MCollector/MarkSweepCollector.cpp](../../src/Heap/MCollector/MarkSweepCollector.cpp) 的 `ConcurrentSweep()` 中，当 `CJ_MS_STW_SWEEP=1` 时跳过后台 sweep，日志为：

```text
[MS-GC] CJ_MS_STW_SWEEP=1: ConcurrentSweep skipped, sweep deferred to next PreEnumRoots
```

4. 关闭 sweep 的唤醒和重启路径：
  - [src/Heap/MAllocator/MSRegionManager.cpp](../../src/Heap/MAllocator/MSRegionManager.cpp) 的 `MaybeNotifySweep()` 在该 gate 下直接返回；
  - [src/Heap/MAllocator/MSCentralRegionPool.cpp](../../src/Heap/MAllocator/MSCentralRegionPool.cpp) 的 `ReenableLazySweep()` 在该 gate 下不重新打开 lazy sweep。

### 1.4 当前结论

在 `CJ_MS_STW_MARK=1` 已经排除并发 mark 竞态的前提下，再把 sweep 串行化为“`PreEnumRoots()` 单线程 force-finish unswept backlog”，`case2_50x_pressure + seed5 + 4gb` 可稳定通过。说明第一类问题的根因确实在 **并发 sweep 路径**。

## 2. 问题二：HashMapEntry.value 槽位 stale edge / 坏对象入队

### 2.1 现象

在 `case11_f5_hashmap_min_repro` 上，当前仍存在第二类问题，症状主要有两种：

- `TraceObjectRefFieldsToGCWork: FATAL invalid target`
- `DrainGCWork: FATAL invalid typeInfo`

最近一次定向运行中，stdout 里已经稳定抓到：

```text
DrainGCWork: FATAL invalid typeInfo obj=0x300b01560
```

### 2.2 已确认事实

当前已经确认以下几点：

1. 坏边位置落在 `RawArray<HashMapEntry<Int64, ArrayList<Int64>>>` 的 `fieldOffset=40`，对应 `HashMapEntry.value`。
2. 常规 entry 写路径并不像完全绕过 barrier：
  - [src/CompilerCalls.cpp](../../src/CompilerCalls.cpp) 的 `CJ_MCC_WriteGeneric` 已记录到对应 slot 写入；
  - [src/Heap/MBarrier/MSMarkBarrier.cpp](../../src/Heap/MBarrier/MSMarkBarrier.cpp) 的 `WriteStruct` 已记录到 `shade-new` 命中同一类槽位。
3. `HashMapEntry(..., value)` 这类带参构造，对 ref 型 `value` 字段也不是显眼的 plain raw store。

### 2.3 当前判断

因此，第二类问题当前更像：

- 某条非常规路径在后续把同一槽位写坏；
- 或者对象已经被正常 discover / enqueue，但在后续阶段又被覆盖、复用或损坏；
- 而不是常规 `WriteGeneric -> WriteStruct -> shade(new)` 主路径完全漏掉了 discover。

### 2.4 当前为定位它做的定向改动

为把“最后一次写入槽位”与“最终坏对象”串起来，当前 workspace 中已经加入了窄范围 probe：

- [src/CompilerCalls.cpp](../../src/CompilerCalls.cpp) 记录 `fieldAddr` / `valueFieldAddr`
- [src/Heap/MBarrier/MSMarkBarrier.cpp](../../src/Heap/MBarrier/MSMarkBarrier.cpp) 记录 `WriteStruct shade-new` 的 `fieldAddr`
- [src/Heap/MCollector/MarkSweepCollector.cpp](../../src/Heap/MCollector/MarkSweepCollector.cpp) 记录 fatal 时的 `fieldAddr`

下一步目标是把：

```text
Trace enqueue owner=... fieldAddr=... target=...
```

和：

```text
DrainGCWork: FATAL invalid typeInfo obj=...
```

在同一次运行里对到同一个对象地址，确认坏对象究竟是从哪一个 `HashMapEntry.value` 槽位被送进 GC work 的。

## 3. 当前状态

- 问题一（并发 sweep 交叠）：根因和抑制方案已经明确，关键 gate 已在当前代码里。
- 问题二（HashMap stale edge / invalid typeInfo）：还在继续收敛，目前已经排除“常规 value 写路径完全绕过 barrier”这一假设，但还没把最终坏对象和最后一次 enqueue 的字段地址彻底串起来。