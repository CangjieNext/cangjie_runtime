# MSGC Case2 当前分析状态

- 日期：2026-05-02
- 用例：`MSGC_RESULT/Result/case2_50x_pressure` / `F5_HashMap` / `seed5 + 4gb + marksweep`
- 目的：把当前对 case2 的分析结论单独收敛成一份状态文档，便于后续继续追踪

## 1. 问题现状

`case2` 的 `F5_HashMap` 目前已经可以拆成两层问题来看：

1. **第一层问题：并发 sweep 与 mark/Drain 交叠**
2. **第二层问题：HashMapEntry.value 槽位上的 stale edge / 坏对象传播**

这两层问题不是同一个修复点，但它们会在同一个 `F5_HashMap` 压测里先后显形。

## 2. 已经确认的结论

### 2.1 Barrier 的 zero-dst fast path 的确有问题

旧 `MSMarkBarrier::WriteStruct()` 中，“dst 看起来全零就跳过 shade-old”的 fast path 会在并发写交错下漏掉 old ref。

当前代码里，这条 fast path 已经从主控制流删除：

- `WriteStruct()` 现在只要 `obj != nullptr && obj->GetTypeInfo() != nullptr`，就无条件执行 `shade(old)`。
- `IsZeroedStructDestination()` / `ZERO_DST_FAST_PATH_MAX_BYTES` 已删除。

这个修改是必要的，因为它解释了早期 `value mismatch` 的一部分来源：本来应被 SATB 记录的旧引用在 transient zero 瞬间被跳过，后续 target 被错误回收。

### 2.2 “并发 sweep 交叠”是一个独立且已经坐实的问题

在 `CJ_MS_STW_MARK=1` 已排除并发 mark 竞态后，再把 sweep 串行为“`PreEnumRoots()` 单线程 force-finish unswept backlog”，`case2_50x_pressure + seed5 + 4gb` 可以稳定通过。

2026-05-02 最新源码已不再依赖 `CJ_MS_STW_SWEEP` gate，而是在默认路径中收口该问题：

1. `PreEnumRoots()` 先通过 `StopAndWaitForAllSweep()` 停止并等待 background/lazy sweep 静止；
2. 如果 central `partialUnswept/fullUnswept` 仍有 backlog，GC 线程同步执行 `ForceFinishAllUnswept()`，保证这些 region 在 `ClearAllMarkBitmaps()` 前完成物理 sweep；
3. background helper 对 GC stop 或已被 GC 线程扫空的陈旧请求直接安静返回，避免继续产生 `BackgroundSweep: stop requested at cls=0` 和 `BackgroundSweep slice: swept=0`。

验证结果：`case2_50x_pressure + seed5 + 4gb + marksweep` 单次运行目录 `20260502_112258_copilot_case2_seed5_bg_sweep_quiet_exit_marksweep` 中 Functional `5/5 passed`。随后 5 次复验（`copilot_case2_seed5_verify5_run1..5`）显示 background sweep 判据稳定干净：`BackgroundSweep: stop requested` 计数均为 0，`BackgroundSweep slice: swept=0` 计数均为 0，`ReclassifyUnsweptToSwept total>0` 计数均为 0；但 Functional 结果为 `4/5, 5/5, 5/5, 4/5, 5/5`，失败均为 `F5_HashMap`。因此当前只能认为 background sweep 交叠问题已收口，case2 整体仍存在 `HashMapEntry.value` 相关的间歇性 stale edge / 坏对象传播问题。

这说明第一类问题的根因不在 root marking 主路径，而在：

- 对象或边已经被 discover，但尚未完成 Drain；
- sweep / lazy sweep 仍在同一批 region 上做物理清扫；
- 后续访问这些对象时读到坏 `stateWord`、坏 `typeInfo` 或 stale edge。

也就是说，**mark 和 sweep 的并发窗口本身就能单独制造错误**。

### 2.3 当前坏边已经缩到 `HashMapEntry.value`

围绕 `case11_f5_hashmap_min_repro` 做的增量探针虽然是更小复现，但它对 `case2` 的分析是有价值的：

1. `TraceObjectRefFieldsToGCWork()` 命中的坏边 owner 是：

```text
RawArray<std.collection:HashMapEntry<Int64,std.collection:ArrayList<Int64>>>
```

2. `fieldOffset=40` 可以映射到 `HashMapEntry.value`。

3. 也就是说，当前 stale edge 的落点已经收敛到：

```text
RawArray<HashMapEntry<Int64, ArrayList<Int64>>>[i].value
```

这个结论对于回看 `case2` 很重要，因为它把“坏边发生在哪里”从泛泛的 HashMap 场景，缩成了非常具体的 entry.value 槽位。

### 2.4 常规 value 写路径不像完全绕过 barrier

当前静态和动态证据都表明，常规路径下：

- `entries[i] = HashMapEntry(..., value)` 这类 heap-to-heap 写入会走 `WriteGeneric -> WriteStruct -> shade(new)`；
- `HashMapEntry(..., value)` 带参构造里，ref 型 `value` 字段也会走 `_CJ_MCC_WriteRefField`，不是显眼的 plain raw store。

因此，当前更像是：

1. 某条**非常规写入路径**在更后面把同一个槽位写坏；
2. 或者对象已经被正常 discover / enqueue，但后续又被覆盖、复用或损坏；
3. 而不是“主路径完全没进 barrier”。

## 3. 当前对 case2 的整体判断

截至目前，`case2` 不应再被理解成“单一的 WriteStruct bug”。更准确的分层判断是：

### 3.1 已经基本坐实的部分

- `WriteStruct` 的 zero-dst fast path 是错的，已经修掉。
- 并发 sweep 会与 mark/Drain 交叠，已经被实验稳定证明。

### 3.2 还未完全打穿的部分

- 为什么在常规 `WriteStruct shade(new)` 已存在的前提下，`HashMapEntry.value` 仍会出现 stale edge。
- `DrainGCWork: FATAL invalid typeInfo obj=...` 这种坏对象，究竟是从哪个具体 `fieldAddr` / 哪次 enqueue 进入 GC work 的。

换句话说，现在最大的未知点已经不是“屏障主路径有没有执行”，而是：

```text
坏对象是在什么时候、通过哪条非典型路径，最终进入了 GC work 或留在了 entry.value 槽位里。
```

## 4. 当前 workspace 中与分析对应的改动

### 4.1 已经用于验证的修复/实验代码

- `src/Heap/MBarrier/MSMarkBarrier.cpp`
  - 删除 zero-dst fast path 及其辅助函数
- `src/Heap/MAllocator/MSCentralRegionPool.cpp`
  - `StopAndWaitForAllSweep()` 默认路径中同步 `ForceFinishAllUnswept()`，在 bitmap 清理前物理扫完 central unswept backlog
  - `BackgroundSweep()` 在 stop 或无 backlog 时直接返回；由 stop 导致的中断不再作为 pending work 继续唤醒 helper

补充说明：`ClearAllMarkBitmaps` 中每轮仍可能出现 `unsweptPromoted=~50`。这不是 central backlog 残留；当前 `MarkTermination()` 明确采用“thread-local regions out of the current sweep generation”的策略，TL active/cache region 后续通过 `ReturnRegionToCentral()` 按当前 `globalSweepGen` 盖章，因此 `unsweptPromoted` 主要是 TL region 的 generation 对齐信号。

### 4.2 已经用于定位第二层问题的定向 probe

- `src/CompilerCalls.cpp`
  - 记录 `fieldAddr` / `valueFieldAddr`
- `src/Heap/MBarrier/MSMarkBarrier.cpp`
  - 记录 `WriteStruct shade-new` 的 `fieldAddr`
- `src/Heap/MCollector/MarkSweepCollector.cpp`
  - 记录 fatal 时的 `fieldAddr`
  - 记录 value 槽位 enqueue 的对象地址

这些 probe 的目的不是修复，而是把：

```text
最后一次把对象写进 HashMapEntry.value 的动作
```

和：

```text
后续 DrainGCWork / TraceObjectRefFieldsToGCWork 里坏掉的对象
```

在同一次运行里精确对应起来。

## 5. 下一步最值得做的事

当前对 case2 的后续工作，不应该再回到泛泛的 barrier 猜测，而应该继续盯这个具体闭环：

1. 用 `fieldAddr` 把 `HashMapEntry.value` 最后一次写入和后续 fatal 对象地址直接对上；
2. 判断坏对象是在“写后被别的路径写坏”，还是“已正常入队但在 GC 后续阶段被损坏”；
3. 如果能稳定对到同一个 `fieldAddr`，再往上追该地址对应的 source/asm lowering 路径。

## 6. 当前结论一句话版

`case2` 现在已经不是“只有一个 barrier bug”的理解方式了；当前更准确的结论是：

- **Bug 1**：并发 sweep 交叠，已经坐实；
- **Bug 2**：`HashMapEntry.value` 上仍有 stale edge / 坏对象传播链，但常规 `WriteStruct` 主路径不像完全漏掉 barrier，真正缺口还在继续收敛。