# MSGC Case2 F5 最终根因、修复点与 5 轮验证结论

- 日期：2026-05-02
- 用例：`MSGC_RESULT/Result/case2_50x_pressure`
- 参数：`marksweep + seed5 + 4gb`
- 关注点：`F5_HashMap` 间歇性 `value mismatch`
- 本文目的：把这次问题的最终闭环单独收敛成一份结论文档，覆盖根因、修复点和验证结果

> 这份文档对应本轮最终结论，补齐并收口此前
> `20260502_case2_current_analysis.md` 中仍未完全落地的部分。

## 1. 现象回顾

在前两轮修复之后：

1. `MSMarkBarrier::WriteStruct()` 的 zero-dst fast path 已删除；
2. background/lazy sweep 与 mark 的交叠窗口已通过 `StopAndWaitForAllSweep() + ForceFinishAllUnswept()` 收口；

但 `case2_50x_pressure + seed5 + 4gb + marksweep` 仍然会间歇性失败，且失败点持续集中在：

```text
F5_HashMap
```

表现不是 missing key，而是 value mismatch：

```text
key 仍在，HashMapEntry 仍在，取出的 value 数据却属于后面别的 key
```

这说明问题已经不是“HashMap entry 整体丢失”，而是：

```text
entry.value 指向的对象图里，有子对象在本轮 GC 中被错误回收并复用。
```

## 2. 最终根因

最终根因不是单一的 `WriteStruct` 主路径漏 barrier，而是：

```text
MSGC 在并发标记期间对新分配对象仍采用 alloc-black 语义：对象一分配就被标 live，
但没有保证它在初始化完成后被扫描一次。
```

这会带来一个关键问题：

1. 新对象本身不会被 sweep 掉；
2. 但构造/初始化期间写进它内部的子引用，如果没有被某条 barrier 路径显式 discover，就可能在当前 GC 周期中保持“未被发现”状态；
3. 一旦这些子对象所在 region 进入 sweep，它们就可能被回收并复用；
4. 之后再从父对象取值时，就会看到 stale data / value mismatch。

本次 `F5_HashMap` 的具体触发链是：

1. `HashMapEntry.value` 指向 `ArrayList<Int64>`；
2. `ArrayList` 内部再持有 `Array<Int64>` / `RawArray<Int64>`；
3. `ArrayList` 自身可能已被标 live，但 `myData.rawptr` 对应的 child `RawArray<Int64>` 没有被本轮 discover；
4. child 被 sweep 后地址复用，于是读取时表现为“value 内容串到了后面的 key”。

换句话说，`HashMapEntry.value` 是最终症状落点，真正的根因是：

```text
新对象在初始化完成前被“只标记不扫描”，导致其内部对象图闭包不完整。
```

## 3. 为什么前两类修复仍然是必要的

这次最终根因并不推翻前两轮修复，三类问题是叠加关系：

### 3.1 Barrier zero-dst fast path 确实是错的

文件：`src/Heap/MBarrier/MSMarkBarrier.cpp`

旧逻辑中，如果目标 struct 区域看起来全零，就跳过 `shade(old)`；在并发写交错下，这会漏掉 old ref。

当前代码已删除这条 fast path，`WriteStruct()` / `CopyRefArray()` / `CopyStructArray()` 都会在覆盖前保留 old refs，在写入后 discover new refs。

### 3.2 并发 sweep 交叠也确实是独立问题

文件：`src/Heap/MAllocator/MSCentralRegionPool.cpp`

当前默认路径中：

1. `PreEnumRoots()` 前先 `StopAndWaitForAllSweep()`；
2. 等 background sweep / lazy sweep quiesce；
3. 如果 central unswept backlog 仍存在，则同步 `ForceFinishAllUnswept()`；
4. `BackgroundSweep()` 对 stop 或空 backlog 的陈旧请求直接安静返回。

这部分修复把 mark/sweep 并发交叠窗口收口，否则即使新对象问题修掉，也可能仍被 sweep 物理清扫污染。

## 4. 最终修复点

最终修复的关键原则是：

```text
不要在 allocator 刚拿到裸地址时就把对象标黑；
而是在对象/数组 header 初始化完成后，再把它标 live，并确保它在 MarkTermination 被扫描一次。
```

### 4.1 停止在 allocator 里提前标新对象

文件：`src/Heap/MAllocator/MSAllocBuffer.cpp`

当前 `AllocBufferMS::Allocate()`：

1. 小对象 fast alloc 成功后，不再直接 `collector->MarkObject(obj)`；
2. allocator 只保留 assist / pacing 相关逻辑；
3. 新对象标记从“裸地址分配时刻”后移到对象模型初始化完成之后。

这一步避免了在 `TypeInfo` / 数组长度等 header 尚未建立时，把对象放入“已 live”状态却无法安全扫描。

### 4.2 普通对象在 `SetClassInfo()` 后标记为新对象

文件：`src/ObjectModel/MObject.cpp`

当前逻辑：

1. `MObject::NewObject()` 在 `SetClassInfo(addr, ti)` 之后调用 `MarkNewObject()`；
2. `MObject::NewPinnedObject()` 也在 `SetClassInfo()` 之后调用 `MarkNewObject()`。

这样可以确保 GC 看到的是一个已经具备 `TypeInfo` 的对象，而不是半初始化裸地址。

### 4.3 数组在 `SetClassInfo() + SetLength()` 后标记为新对象

文件：`src/ObjectModel/MArray.inline.h`

当前逻辑：

1. `MArray` 先完成 `SetClassInfo()`；
2. 再完成 `SetLength()`；
3. 然后调用 `MarkNewObject(newArray)`。

这保证数组扫描时元数据和长度都已就绪。

### 4.4 `MarkNewObject()` 从“只标黑”改为“标记 + 延迟扫描”

文件：`src/Heap/MCollector/MarkSweepCollector.cpp`

当前逻辑：

1. 如果当前 phase 属于 `MS_MARK_SETUP` / `MS_CONCURRENT_MARK` / `MS_MARK_TERMINATION`；
2. 则执行 `MarkObject(obj)`；
3. 随后执行 `PushSingleToDeferredMainThread(obj)`。

也就是说，新对象不再只是 alloc-black，而是：

```text
先保证它自己是 live 的，再把它加入 deferred 队列，
在 MarkTermination 时至少完整扫描一次。
```

这一步是本次问题真正的收口点。它允许构造/初始化期间某些内部写入即使没有被 barrier 单独 discover，最终也能在 sweep 前通过“新对象延迟扫描”补齐闭包。

## 5. 修复后语义

修复后的新对象处理语义可以概括为：

1. allocator 负责分配地址，不负责把半初始化对象标黑；
2. 对象模型层在 header 初始化完成后调用 `MarkNewObject()`；
3. collector 在并发标记期间把这个新对象：
   - 立即标 live；
   - 延迟到 MarkTermination 再扫描一次；
4. 因而对象图闭包不再完全依赖构造期每一条内部写入都必须走 barrier。

## 6. 验证方法

验证命令：

```bash
bash MSGC_RESULT/Result/script/run_case.sh \
  --case case2_50x_pressure \
  --gc-mode marksweep \
  --skip-build \
  --seed seed5 \
  --heap-size 4gb \
  --tag <tag>
```

重点观测指标：

1. `stdout.txt` 中 `F5_HashMap` 是否 PASS；
2. `stdout.txt` 中 `Functional` 是否 `5/5 passed`；
3. `F5-TRACE` 中是否仍有 `mismatch` / `missing`；
4. `gc.log` 中 background sweep 判据是否仍干净：
   - `BackgroundSweep: stop requested = 0`
   - `BackgroundSweep slice: swept=0 = 0`
   - `ReclassifyUnsweptToSwept total>0 = 0`

## 7. 5 轮稳定性验证结果

### 7.1 验证目录

1. `20260502_130749_copilot_case2_seed5_stable5_20260502_run1_marksweep`
2. `20260502_131348_copilot_case2_seed5_stable5_20260502_run2_marksweep`
3. `20260502_131925_copilot_case2_seed5_stable5_20260502_run3_marksweep`
4. `20260502_132502_copilot_case2_seed5_stable5_20260502_run4_marksweep`
5. `20260502_133039_copilot_case2_seed5_stable5_20260502_run5_marksweep`

### 7.2 结果表

| Run | F5_HashMap | Functional | Total wall time (ms) | F5 trace issues | bg stop | bg swept=0 | reclass positive |
| --- | --- | --- | ---: | ---: | ---: | ---: | ---: |
| 1 | PASS (17820 ms) | 5/5 passed | 326857 | 0 | 0 | 0 | 0 |
| 2 | PASS (17971 ms) | 5/5 passed | 333315 | 0 | 0 | 0 | 0 |
| 3 | PASS (18022 ms) | 5/5 passed | 333359 | 0 | 0 | 0 | 0 |
| 4 | PASS (18113 ms) | 5/5 passed | 333241 | 0 | 0 | 0 | 0 |
| 5 | PASS (18220 ms) | 5/5 passed | 334066 | 0 | 0 | 0 | 0 |

### 7.3 结论

这 5 轮里：

1. `F5_HashMap` 全部 PASS；
2. `Functional` 全部 `5/5 passed`；
3. `F5-TRACE` 没有再出现 mismatch / missing；
4. background sweep 相关判据继续保持干净。

按当前这组压力参数和 5 轮样本看：

```text
case2 + seed5 下此前剩余的 F5 间歇性失败，当前已经不再复现。
```

## 8. 最终一句话结论

本次 case2/F5 的最终根因不是单纯的 `WriteStruct` 漏 barrier，而是：

```text
并发标记期间的新对象仍采用“只标记不扫描”的 alloc-black 语义，
导致初始化阶段写入的子对象图闭包不完整。
```

修复方式是把新对象标记时机后移到 header 初始化完成之后，并在 `MarkTermination` 前强制对新对象补一次扫描；结合前面已经落地的 barrier 和 sweep 收口修复后，`case2_50x_pressure + seed5 + 4gb + marksweep` 已完成 5/5 稳定通过验证。