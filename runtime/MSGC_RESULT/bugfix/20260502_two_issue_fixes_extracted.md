# MSGC 两个问题的独立修改摘录

- 日期：2026-05-02
- 目的：把当前代码里已经落下去的两类关键修复单独摘出来，便于后续 review / cherry-pick / 回归验证

## 1. 问题一：Barrier 的 zero-dst 快速路径要删除

### 1.1 问题现象

`MSMarkBarrier::WriteStruct()` 旧实现里存在一个 zero-dst fast path：如果目标 struct 区域看起来是全零，就跳过 `shade(old)`。

这个假设在并发场景下不成立：`dst` 可能只是短暂变成 0，但里面原本承载的 old ref 仍然需要被 SATB 记录。跳过 `shade(old)` 后，old ref 可能被漏标，后续表现为 HashMap / ArrayList 场景下的 value mismatch、stale edge，甚至下游崩溃。

### 1.2 修改点

文件：`src/Heap/MBarrier/MSMarkBarrier.cpp`

当前行为：

1. `WriteStruct()` 中不再根据 zero-dst 判断跳过 `shade(old)`。
2. 只要 `obj != nullptr && obj->GetTypeInfo() != nullptr`，就无条件执行：

```cpp
obj->ForEachRefInStruct(
    [](RefField<false>& refField) {
        BaseObject* oldRef = refField.GetTargetObject();
        MSMarkBarrier::EnqueueRef(oldRef);
    },
    dst, dst + dstLen);
```

3. `memcpy` 后继续对新内容做 `shade(new)`。

### 1.3 当前代码状态

- 修复逻辑已经在代码里。
- `skipOldRefScan` 这类旧控制流已经不存在。
- `IsZeroedStructDestination()` / `ZERO_DST_FAST_PATH_MAX_BYTES` 已删除，不再保留 dead code。

## 2. 问题二：Mark 与 Sweep 并发交叠

### 2.1 问题现象

MSGC 中曾出现一类更底层的问题：对象或边已经被 mark 线程 discover，但并发 sweep / lazy sweep 仍可能在同一批 region 上进行物理清扫。这样会导致：

- `DrainGCWork: FATAL invalid stateWord`
- 扫描阶段读到 stale heap pointer
- 对象还没完成 Drain，内容就被 sweep 改写或清零

### 2.2 修复目标

目标不是简单“暂停后台线程”，而是把 sweep 从 mark 窗口里完整挪出去：

1. 下一轮 mark 开始前，先等所有 in-flight sweep quiesce；
2. 由 GC 线程在 `PreEnumRoots()` 内单线程 `ForceFinishAllUnswept()`，物理扫完 central unswept backlog；
3. 然后再 `ClearAllMarkBitmaps()`、`ReclassifyUnsweptToSwept()`；本轮 `MarkTermination()` 再执行 `PrepareForSweep()` 翻转下一轮 sweep generation；
4. background sweep 对 GC stop 或已被 force-finish 清空的陈旧请求直接安静返回。

### 2.3 修改点

#### A. PreEnumRoots 里先停 sweep，再冻结本轮 snapshot

文件：`src/Heap/MCollector/MarkSweepCollector.cpp`

当前顺序：

```cpp
centralPool->StopAndWaitForAllSweep();
centralPool->ClearAllMarkBitmaps();
centralPool->ReclassifyUnsweptToSwept();
```

这保证 mark 开始前，旧 sweep 不再与 Drain 并发交叠。

#### B. StopAndWaitForAllSweep 里真正 force-finish backlog

文件：`src/Heap/MAllocator/MSCentralRegionPool.cpp`

当前逻辑：

1. `stopSweepRequested_ = true`
2. `skipLazySweep_ = true`
3. 等 `bgSweepRunning_ == false`
4. 等 `activeLazySweepCount_ == 0`
5. 如果 `GetTotalUnsweptCount() > 0`，执行：

```cpp
ForceFinishAllUnswept();
```

这一步是关键，表示 backlog 不是“留着不扫”，而是改为 GC 线程在 `PreEnumRoots()` 里一次性物理扫完。

#### C. Background sweep 对 stop/空 backlog 安静退出

文件：`src/Heap/MAllocator/MSCentralRegionPool.cpp`

当前逻辑：

1. `BackgroundSweep()` 入口先设置 `bgSweepRunning_`，随后如果 `stopSweepRequested_ == true`，直接返回 `false`。
2. 如果 `GetTotalUnsweptCount() == 0`，说明陈旧的 helper 请求已经没有实际 backlog，也直接返回 `false`，不再打印空 slice。
3. `SweepUnsweptList()` 因 stop 返回 `false` 时，不再把这次中断当成 `pendingWork=true`，因为下一轮 `PreEnumRoots()` 的 GC 线程会 force-finish backlog。

这解决了高频 GC 下 helper 的陈旧 `shouldBackgroundSweep_` 请求继续制造：

```text
BackgroundSweep: stop requested at cls=0
BackgroundSweep slice: swept=0 regions ... pending=no
```

的问题。

## 3. 当前状态结论

这两个问题对应的关键修改当前都已经在代码里：

1. barrier 的 zero-dst fast path 已经从 `WriteStruct()` 主控制流中删除；
2. mark/sweep 并发交叠通过默认路径的 `StopAndWaitForAllSweep() + ForceFinishAllUnswept()` 收口，不再依赖 `CJ_MS_STW_SWEEP` 诊断 gate；
3. background helper 对 stop/空 backlog 请求安静退出，避免陈旧唤醒继续产生空 sweep slice。

如果后续要做最小化 cherry-pick，优先保留以下文件中的相关改动：

- `src/Heap/MBarrier/MSMarkBarrier.cpp`
- `src/Heap/MAllocator/MSCentralRegionPool.cpp`

验证结果：`case2_50x_pressure + seed5 + 4gb + marksweep` 单次运行 `20260502_112258_copilot_case2_seed5_bg_sweep_quiet_exit_marksweep` 曾 Functional `5/5 passed`；随后 5 次复验（`copilot_case2_seed5_verify5_run1..5`）中，background sweep 判据全部干净：`BackgroundSweep: stop requested = 0`，`BackgroundSweep slice: swept=0 = 0`，`ReclassifyUnsweptToSwept total>0 = 0`。但 Functional 并未稳定通过，5 轮结果为 `4/5, 5/5, 5/5, 4/5, 5/5`，失败均落在 `F5_HashMap`。因此本节修复只能说明 mark/sweep 交叠与 helper 陈旧唤醒问题已收口，不能说明 case2 整体已经无问题；`HashMapEntry.value` 的 stale edge / 坏对象传播链仍需继续定位。