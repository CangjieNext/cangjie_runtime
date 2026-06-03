# MSGC Case2 F5_HashMap 双重 SATB 缺陷修复

- 日期：2026-05-01
- 范围：MS-GC 写屏障 `MSMarkBarrier::WriteStruct` / `CopyStructArray`
- 用例：`MSGC_RESULT/Result/case2_50x_pressure`，`F5_HashMap`，`seed5 + 4gb heap + marksweep`
- 文件：[src/Heap/MBarrier/MSMarkBarrier.cpp](../../src/Heap/MBarrier/MSMarkBarrier.cpp)

## 1. 现象与触发条件

`benchF5_HashMap` 在 1000 轮压测中以两种不同方式失败：

| 失败模式 | 现象 | 复现频率 |
|---|---|---|
| **A. value mismatch** | `HashMap<Int64, ArrayList<Int64>>` 的 `vals[1]` 读到与 `i*2` 期望值不符的小整数（典型 `8676 vs 4338*2`） | 频繁 |
| **B. SIGSEGV** | `Barrier::WriteStruct → ForEachRefInStruct` 解引用野 typeinfo（faulting addr=0x1688） | 间歇（5 次中 1 次） |

调用栈共同特征：
```
Barrier::WriteStruct
  ArrayList[]= (std/collection/array_list.cj)
    benchF5_HashMap (benchmark.cj:307)
```

## 2. 根因分析

实质是同一类问题在 SATB 屏障里有两条 unsafe 路径：

### 2.0 2026-05-02 增量收敛：坏边落在 `HashMapEntry.value`，常规 `WriteStruct` 路径本身不像缺 discover

围绕 `case11_f5_hashmap_min_repro + CJ_MS_STW_MARK=1 + CJ_MS_STW_SWEEP=1` 做了更细的定向探针，当前新增事实如下：

1. 在 [src/Heap/MCollector/MarkSweepCollector.cpp](../../src/Heap/MCollector/MarkSweepCollector.cpp) 的 `TraceObjectRefFieldsToGCWork()` 命中：

```text
TraceObjectRefFieldsToGCWork: FATAL invalid target
ownerType=RawArray<std.collection:HashMapEntry<Int64,std.collection:ArrayList<Int64>>>
fieldOffset=40
targetHeap=1
targetTypeInfo=(nil)
```

2. 这说明坏边不是更晚在 `DrainGCWork()` 队列里才损坏，而是**扫描 `RawArray<HashMapEntry<...>>` 元素字段时当场读出来**的 stale heap pointer。

3. 结合 [src/ObjectModel/MArray.inline.h](../../src/ObjectModel/MArray.inline.h) 可知 `MArray::GetContentOffset() = sizeof(MArray)`；在 64-bit 下 `MArray` 头是 16B，因此 `fieldOffset=40` 对应元素内偏移 `24B`。

4. 再结合 [stdlib/libs/std/collection/hash_map.cj](../../stdlib/libs/std/collection/hash_map.cj) 中 `HashMapEntry<K, V>` 的字段顺序：

```text
hash: Int64   @ 0
next: Int64   @ 8
key:  K       @ 16   (这里 K = Int64)
value: V      @ 24   (这里 V = ArrayList<Int64>)
```

因此 `fieldOffset=40 = 16 + 24`，可以直接对上 **`HashMapEntry.value`**。

5. 再看 runtime 写路径：
  - [src/Heap/Barrier/Barrier.cpp](../../src/Heap/Barrier/Barrier.cpp) 中 `WriteGeneric()` 在 heap-to-heap 场景固定落到 `WriteStruct(obj, dst, size, src, size)`；
  - [src/Heap/MBarrier/MSMarkBarrier.cpp](../../src/Heap/MBarrier/MSMarkBarrier.cpp) 中 `WriteStruct()` 在 `memcpy` 后会无条件 `ForEachRefInStruct(... shade new ...)`。

6. 标准库源里 `HashMap.add()` / `putWithoutResize()` 对 entry 槽位的更新也是显式：

```text
entries[entryIndex] = HashMapEntry<K, V>(..., value)
```

对应位置见 [stdlib/libs/std/collection/hash_map.cj](../../stdlib/libs/std/collection/hash_map.cj)。

7. 再核对 [stdlib/build/build/modules/darwin_aarch64_cjnative/std/std.collection-temp-files/std.collection.s](../../stdlib/build/build/modules/darwin_aarch64_cjnative/std/std.collection-temp-files/std.collection.s) 里的 `HashMapEntry` 构造函数：
  - `__CNac12HashMapEntryIG_G0_E6<init>HllG_G0_`（带参构造）里，`hash/next` 两个 `Int64` 字段是 plain `str`；
  - 但 generic `key/value` 字段不会统一走 raw store：非 ref generic 走 `_CJ_MCC_WriteGeneric`，ref generic 走 `_CJ_MCC_WriteRefField`；
  - 也就是说，至少从 `HashMapEntry(..., value)` 构造函数本身看，`value` 这个 ref 槽位在 GC running 分支不是 plain store。

**当前更强结论**：

- `RawArray<HashMapEntry<..., ArrayList<Int64>>>` 上的坏边已经定位到 `value` 槽位；
- 常规 `entries[i] = HashMapEntry(..., value)` 这条 heap-to-heap 写路径按 runtime 代码应当经过 `WriteGeneric -> WriteStruct -> shade(new)`；
- `HashMapEntry(..., value)` 临时值构造本身，对 ref 型 `value` 也会走 `_CJ_MCC_WriteRefField`，不是显眼的 constructor raw store 漏洞；
- 因此当前更像是**某条绕过 `WriteStruct` 的路径**把 `value` 写进了 entry，或者是在 **new-object / temporary struct initialization raw store** 阶段把新 ref 写进去但没有被当前轮 discover，而不是 `MSMarkBarrier::WriteStruct()` 本身缺少 `discover(new)`。

下一步应继续盯两类路径：

1. `HashMapEntry<K,V>` 临时值 / 新对象初始化时是否存在 direct store；
2. 编译器对 `HashMapEntry(..., value)` 这类构造 + array slot store 的 lowering，是否在某个阶段把 ref 写入拆成了 raw store + 后续 plain copy。

### 2.1 Bug A — Zero-dst Fast Path

旧 `WriteStruct` 有「目标 8B 全零 ⇒ 跳过 shade-old」的快路径：

```cpp
bool skipOldRefScan = obj && obj->GetTypeInfo() && IsZeroedStructDestination(dst, dstLen);
if (obj && obj->GetTypeInfo() && !skipOldRefScan) {
    obj->ForEachRefInStruct(... shade old ...);
}
```

假设：「dst 8B 全零 ⇒ 该字段没有 old ref ⇒ 不需要 shade」。

实际：在并发 mark 期间，HashMap rehash / ArrayList 扩容会**短暂**地把某条 ref 字段写为 0（move 语义：先清 dst，再 memcpy src）。这一瞬间 mutator 上别的写屏障也可能击中同一槽位。Fast path 把这条仍然存活的旧 ref **判定为不需要 shade**，old ref 缺席 SATB → 后续被并发 mark 漏标 → sweep 阶段被释放 → 同地址被新对象重用 → HashMap 读取 entry.value 时拿到的是新对象的字段（典型表现为数值变成 2x 期望，因为新对象恰好是另一个 ArrayList）。

**修复**：直接移除 fast path，无条件 `ForEachRefInStruct(shade old)`。

### 2.2 Bug B — 缺失 Poisoned-Object Guard

修完 Bug A 后，5 次稳定性测试里 RUN 3 改为段错误。

原因：A 修复让所有 `WriteStruct` 都强制走 `ForEachRefInStruct`。而 `obj` 本身在并发场景下**可能已经被回收并重用**——此时 `obj->GetTypeInfo()` 拿到的是垃圾指针；`ForEachRefInStruct` 内部 `typeInfo->HasRefField()` 解引用就崩。

旧判定条件 `obj != nullptr && obj->GetTypeInfo() != nullptr` 不够：
- 没检查 `Heap::IsHeapAddress(obj)` ⇒ obj 可能根本不在堆里
- typeinfo 字段可能写过又被改回非空，但仍是垃圾值
- 没检查 `obj->HasRefField()`，无 ref 字段的对象不该走结构体屏障

**修复**：参照默认的 `Barrier::WriteGeneric` 已有的 guard，加 `IsObjSafeForStructBarrier(obj)`：

```cpp
static inline bool IsObjSafeForStructBarrier(const BaseObject* obj) {
    if (obj == nullptr) return false;
    if (!Heap::IsHeapAddress(const_cast<BaseObject*>(obj))) return false;
    if (obj->GetTypeInfo() == nullptr) return false;
    if (!obj->HasRefField()) return false;
    return true;
}
```

不安全则跳过 SATB shade，仅 memcpy/memmove。

## 3. 修改清单（最终版）

文件：[src/Heap/MBarrier/MSMarkBarrier.cpp](../../src/Heap/MBarrier/MSMarkBarrier.cpp)

**主修复**（已合入 workspace）：
1. **`WriteStruct`** 删除 zero-dst fast-path：移除 `IsZeroedStructDestination` 调用与 `skipOldRefScan` 判断，无条件执行 `ForEachRefInStruct(shade old)`。
2. `IsZeroedStructDestination` 函数体保留为 dead code（后续清理）。

**未并入主修复（diff 见 stash `20260501_full_workspace_with_bugAB_diag` 与 [MSGC_RESULT/temp/20260501_workspace_full.patch](../temp/20260501_workspace_full.patch)）**：
- `IsObjSafeForStructBarrier` guard：见 §5 评估，**不应并入**。
- 写屏障诊断 instrumentation（`CJ_MS_WB_DIAG`、`CJ_MS_HASHMAP_STRUCT_TRACE`、overflow drop 计数）：仅用于诊断，不要进生产。
- `CopyStructArray` 的 dstObj guard：同 `IsObjSafeForStructBarrier`，不并入。

## 4. 风险与残留问题

### 治标性质
Bug B 的 guard 是**下游兜底**，不是根因修复。
- 根因：obj 之所以会进入 poisoned 状态，意味着 GC 已经在错误时刻把活对象当死对象 sweep 掉过；guard 只是不让进程崩。
- 可能仍在别处显形：扩容路径、大对象、长链 HashMap、其它 struct 字段等。

### 后续观察项
| 指标 | 期望 | 含义 |
|---|---|---|
| 5x guard_run 全部 PASS | 是 | 段错误不再阻塞 |
| `g_structGuardSkipCount` = 0 | 是 | 没有 race，根因可能已随 Bug A 修复一起消失 |
| `g_structGuardSkipCount` > 0 但功能 PASS | 接受 | guard 在兜底，需进一步追根因 |
| `g_structGuardSkipCount` > 0 且 mismatch | **不接受** | guard 不够，需更深修复 |

### 已知风险
1. guard 命中时纯 memcpy，若该对象其实**仍然存活**且 dst 真有 old ref，则 SATB 漏掉一条边——后续这条边对应的 target 可能被错误回收。但前提是 obj 自己也被错误回收，所以对象图整体已经损坏，guard 命中说明系统已处于不一致状态。
2. 性能损失：每次 `WriteStruct` 多 1 次 `IsHeapAddress` + 几次 null 检查。HashMap 写入热点上有可测开销，与默认 Barrier 量级相当。

## 5. 验证

### 关键转折：Bug A 修复必要但不充分
做了一组**只应用 Bug A 修复、剥离所有诊断代码和 Bug B guard** 的对照验证（保存原状到 [MSGC_RESULT/temp/20260501_workspace_full.patch](../temp/20260501_workspace_full.patch) + `git stash` 后再做最小修复）。结果：

| Tag | F1 | F2 | F3 | F4 | F5_HashMap | Functional |
|---|---|---|---|---|---|---|
| `bugA_only_run1` (085101) | PASS | PASS | PASS | PASS | **PASS** 17174ms | 5/5 |
| `bugA_only_run1` (090128) | PASS | PASS | PASS | PASS | **PASS** 16554ms | 5/5 |
| `bugA_only_run2` (090604) | PASS | PASS | PASS | PASS | **PASS** 16651ms | 5/5 |
| `bugA_only_run3` (091038) | PASS | PASS | PASS | PASS | **FAIL** 16477ms | 4/5 |
| `bugA_only_run4` (091517) | PASS 7670ms | PASS 18874ms | PASS 145s | PASS **983s** | — | 🔥 **GC HANG** |
| `bugA_only_run5` (160136) | 进行中 | — | — | — | — | — |

→ Bug A 修复**必要但不充分**：观察到至少 **2 类残留问题**（3 PASS / 1 FAIL / 1 HANG）。

#### RUN 4 GC livelock（新发现的独立 bug）
- benchmark 进程在 F5 启动前进入 macOS `UE+`（kernel-level 不可中断），`kill -9` 无效；
- F4 跑了 **983 秒**（正常 ~7s），系统已在退化状态；
- gc.log 在 `09:18:17` 后 33 分钟无任何 GC 活动；
- 末尾出现 **HEU trigger 风暴**（每 ~1ms 一次，`effectiveAlloc=3568976 threshold=3145728 reason=threshold`），但 `GC START` 始终未出现；
- 表明 `RequestGC(GC_REASON_HEU)` 的请求未被 GC 主线程接受 / 未启动 ConcurrentMark；
- 怀疑路径：上一轮 GC cycle 中某个状态变量（`activeCollector_` 之外）未复位、STW handshake 卡某 mutator、或 assist debt 异常。
- 与 SATB 漏标是**独立问题**，需单独排查。


### Bug B 重新定位
之前 RUN 3 SIGSEGV 的真实成因 **是 Bug A 漏 shade 的下游表现**，并非独立缺陷：
- Bug A 让活对象被错误回收
- 槽位被新对象重用，但 mutator 仍按旧引用图解引用
- 在 `obj->GetTypeInfo()` 或 `ForEachRefInStruct` 内部踩到野 typeinfo → SIGSEGV

修掉 Bug A 后，被错误回收的链路消失，崩溃也随之消失。

### Bug B 的 guard 价值评估
`IsObjSafeForStructBarrier` 这个 guard：
- **副作用**：guard 命中时只 memcpy 不 shade，**会进一步掩盖** SATB 的 race（可能让 mismatch 变得更隐蔽）。
- **必要性**：仅作为防止 SIGSEGV 的兜底，无法修复 mismatch 问题。
- **结论**：**不应该并入主修复**，仅保留为诊断手段（`CJ_MS_WB_DIAG=1` 时打开）。

### 残留问题：仍有漏 shade 路径
RUN 3 出现 value mismatch（无 SIGSEGV）说明仍有 SATB shade-old 漏掉的路径。可疑残留：

1. **`WriteStruct` 的 `obj == nullptr` 短路**：当编译器在某些匿名 struct 拷贝场景没传 enclosing obj 时，整段 shade-old 被跳过。
2. **`WriteStruct` 的 `obj->GetTypeInfo() == nullptr` 短路**：obj 在并发瞬态下 typeinfo 可能短暂为空（刚分配未完成初始化），此时本应有的 shade 被跳过。
3. **`CopyStructArray` 的 `dstObj == nullptr` 短路**：同 #1。

下一步排查方向：
- 加 `CJ_MS_WB_NULLOBJ_TRACE` 计数，统计三个短路点的命中次数和上下文（GC phase/dstLen/调用栈）
- 在 obj==nullptr 路径，改用 `gctib` 枚举 ref 字段（类似 `WriteStaticStruct` 的做法），实现无 obj 的 shade-old

### 最终修复 = 1 行（5 行含注释）
```cpp
// before
bool skipOldRefScan = obj && obj->GetTypeInfo() && IsZeroedStructDestination(dst, dstLen);
if (obj && obj->GetTypeInfo() && !skipOldRefScan) { ... }
// after
if (obj && obj->GetTypeInfo()) { ... }
```

### 单次 + 5 次稳定性（历史）
- 单次：`fix_zero_fastpath_off` tag，F1-F5 5/5 PASS（F5_HashMap 16983ms）。
- 5 次（仅 Bug A 修复 + 完整诊断 instrumentation）：RUN 1/2 PASS、RUN 3 SIGSEGV → 误判为 Bug B。
- 5 次（Bug A + 错误的 "Bug B" guard + diag）：guard_run1 PASS, guard_run2 SIGSEGV (fault 0x23f5)。
- 5 次（**仅 Bug A，剥离所有诊断和 guard**）：进行中，1-2 已 PASS。

### 复现命令
```bash
cd <runtime>
python build.py build --target native -t release -v 0.0.1 --gc-mode marksweep
cp output/temp/lib/aarch64_Release/libcangjie-runtime.dylib \
   <SDK>/runtime/lib/darwin_aarch64_cjnative/libcangjie-runtime.dylib
codesign -f -s - <SDK>/runtime/lib/darwin_aarch64_cjnative/libcangjie-runtime.dylib

for i in 1 2 3 4 5; do
  bash MSGC_RESULT/Result/script/run_case.sh \
7. 进一步核对 [stdlib/build/build/modules/darwin_aarch64_cjnative/std/std.collection-temp-files/std.collection.s](../../stdlib/build/build/modules/darwin_aarch64_cjnative/std/std.collection-temp-files/std.collection.s) 里的 `HashMapEntry` 构造函数：
  - `__CNac12HashMapEntryIG_G0_E6<init>HllG_G0_`（带参构造）里，前两个 `Int64` 字段 `hash/next` 是 plain `str`；
  - generic `key/value` 字段不会统一走 raw store：非 ref generic 走 `_CJ_MCC_WriteGeneric`，ref generic 走 `_CJ_MCC_WriteRefField`；
  - 也就是说，至少从 `HashMapEntry(..., value)` 构造函数本身看，**`value` 这个 ref 槽位在 GC running 分支不是 plain store**。
    --case case2_50x_pressure --gc-mode marksweep --skip-build \
    --seed seed5 --heap-size 4gb --tag guard_run$i \
    --env CJ_MS_WB_DIAG=1
done
```
- `HashMapEntry(..., value)` 临时值构造本身，对 ref 型 `value` 也会走 `_CJ_MCC_WriteRefField`，不是显眼的 raw store 漏洞；

### 2026-05-01 B3 实验：`CJ_MS_STW_SWEEP=1` 失败且不可用于判因

为验证“是否是并发 sweep 导致 F5_HashMap 错误”，做了一个最小改动实验：

- [src/Heap/MCollector/MarkSweepCollector.cpp](../../src/Heap/MCollector/MarkSweepCollector.cpp) 的 `ConcurrentSweep()` 在 `CJ_MS_STW_SWEEP=1` 时直接跳过 `MaybeNotifySweep()`；
- [src/Heap/MAllocator/MSRegionManager.cpp](../../src/Heap/MAllocator/MSRegionManager.cpp) 的 `MaybeNotifySweep()` 入口直接短路，避免 mutator alloc 路径唤醒后台 sweep；
- [src/Heap/MAllocator/MSCentralRegionPool.cpp](../../src/Heap/MAllocator/MSCentralRegionPool.cpp) 的 `ReenableLazySweep()` 在 `CJ_MS_STW_SWEEP=1` 时保持 `skipLazySweep_=true` / `stopSweepRequested_=true`，不重新启用 lazy sweep。

表面目标是“把 sweep 推迟到下一轮 `PreEnumRoots()`”；但由于 `PreEnumRoots()` 里的 `ReclassifyUnsweptToSwept()` **只做队列重分类，不物理清扫 dead slots**，这个实验的真实语义变成了：**完全关闭 sweep**。

验证命令：

```bash
for i in 1 2 3 4 5; do
  bash MSGC_RESULT/Result/script/run_case.sh \
    --case case2_50x_pressure --gc-mode marksweep --skip-build \
    --seed seed5 --heap-size 4gb \
    --env CJ_MS_STW_MARK=1 \
    --env CJ_MS_STW_SWEEP=1 \
    --tag stwMarkSweep_run$i
done
```

5 次结果一致：

| Tag | 结果 |
|---|---|
| `stwMarkSweep_run1` | `F1_TreeIntegrity PASS 8877 ms` 后 **OOM** |
| `stwMarkSweep_run2` | `F1_TreeIntegrity PASS 8921 ms` 后 **OOM** |
| `stwMarkSweep_run3` | `F1_TreeIntegrity PASS 8763 ms` 后 **OOM** |
| `stwMarkSweep_run4` | `F1_TreeIntegrity PASS` 后 **OOM** |
| `stwMarkSweep_run5` | `F1_TreeIntegrity PASS` 后 **OOM** |

典型 stdout：

```text
--- Functional Tests (50x) ---
[BENCH] F1_TreeIntegrity         PASS  8877 ms  (1000 rounds)
An exception has occurred:
    Out of memory
```

gc.log 关键信号：

- 每轮都打印 `CJ_MS_STW_SWEEP=1: ConcurrentSweep skipped, sweep deferred to next PreEnumRoots`；
- `Previous-Cycle Sweep Summary` 持续是 `sweep breakdown: no regions swept`；
- `fullUnswept` backlog 快速累积，例如 `30 -> 386 -> 655 -> 667`；
- HEU trigger 原因从 `threshold` 演变为 `sweep-lag`，但因为无任何物理 sweep，ready region 供给无法恢复。

**结论**：B3 不是“STW sweep”，而是“禁用 sweep”。它会稳定导致早期 OOM，因此**不能用于判断 F5_HashMap 是否由并发 sweep 引起**。如果要继续验证 sweep 根因，下一步必须实现“仍然物理清扫 dead slots，但不让后台线程并发参与”的实验，例如：

1. GC 线程同步 drain `BackgroundSweep()` / `SweepUnsweptList()`；或
2. 真正的 STW 内单线程 sweep。

### 2026-05-01 Plan B 修正版：`PreEnumRoots` force-finish unswept backlog

随后把 `CJ_MS_STW_SWEEP=1` 改成真正的“下一轮 `PreEnumRoots()` 物理 sweep”模式：

- 保持 [src/Heap/MCollector/MarkSweepCollector.cpp](../../src/Heap/MCollector/MarkSweepCollector.cpp) 的 `ConcurrentSweep()` 短路，不再启动后台 sweep；
- 保持 [src/Heap/MAllocator/MSRegionManager.cpp](../../src/Heap/MAllocator/MSRegionManager.cpp) 的 `MaybeNotifySweep()` 短路，不让 mutator alloc 路径唤醒后台 sweep；
- 保持 [src/Heap/MAllocator/MSCentralRegionPool.cpp](../../src/Heap/MAllocator/MSCentralRegionPool.cpp) 的 `ReenableLazySweep()` 在该 gate 下不重新启用 lazy sweep；
- **新增关键修正**：在 [src/Heap/MAllocator/MSCentralRegionPool.cpp](../../src/Heap/MAllocator/MSCentralRegionPool.cpp) 的 `StopAndWaitForAllSweep()` 中，当 `CJ_MS_STW_SWEEP=1` 时，在 `bgSweepRunning_ == false && activeLazySweepCount_ == 0` 之后直接调用现成的 `ForceFinishAllUnswept()`，由 GC 线程在 `PreEnumRoots()` 内一次性物理清扫 backlog。

这时实验语义变成：

1. 当前 GC 结束时不做 `ConcurrentSweep`；
2. mutator 周期内没有 background sweep，也没有 lazy sweep；
3. 下一轮 GC 一开始，在 `PreEnumRoots()` 先停掉所有 sweeper，再由 GC 线程单线程 `ForceFinishAllUnswept()`；
4. 然后才 `ClearAllMarkBitmaps() -> ReclassifyUnsweptToSwept() -> EnumRoots()`。

这才是真正可用于判定“并发 sweep 是否为根因”的 Plan B。

验证命令：

```bash
for i in 1 2 3 4 5; do
  bash MSGC_RESULT/Result/script/run_case.sh \
    --case case2_50x_pressure --gc-mode marksweep --skip-build \
    --seed seed5 --heap-size 4gb \
    --env CJ_MS_STW_MARK=1 \
    --env CJ_MS_STW_SWEEP=1 \
    --tag stwMarkSweep_forcefinish_run$i
done
```

5 次结果：

| Tag | F5_HashMap | Functional |
|---|---|---|
| `stwMarkSweep_forcefinish_run1` | **PASS** 18443ms | **5/5 passed** |
| `stwMarkSweep_forcefinish_run2` | **PASS** 18373ms | **5/5 passed** |
| `stwMarkSweep_forcefinish_run3` | **PASS** 18778ms | **5/5 passed** |
| `stwMarkSweep_forcefinish_run4` | **PASS** 18221ms | **5/5 passed** |
| `stwMarkSweep_forcefinish_run5` | **PASS** 18607ms | **5/5 passed** |

第 1 次完整 stdout 摘要：

```text
[BENCH] F1_TreeIntegrity         PASS  7603 ms  (1000 rounds)
[BENCH] F2_Generational          PASS  16919 ms  (10000 temp trees)
[BENCH] F3_LargeObject           PASS  88439 ms  (2500 rounds)
[BENCH] F4_LinkedList            PASS  6548 ms  (1500 rounds)
[BENCH] F5_HashMap               PASS  18443 ms  (1000 rounds)
[BENCH] Functional: 5/5 passed
```

gc.log 明确显示 gate 已按预期工作：

```text
[MS-GC] CJ_MS_STW_SWEEP=1: force-finish unswept backlog in PreEnumRoots
[MS-GC] ForceFinishAllUnswept: swept=369 regions, emptied=356, retained=13, freed=11,950,864 B
[MS-GC] CJ_MS_STW_SWEEP=1: ConcurrentSweep skipped, sweep deferred to next PreEnumRoots
```

**结论**：在 `CJ_MS_STW_MARK=1` 已排除并发 mark 竞态的前提下，再把 sweep 串行化为“`PreEnumRoots()` 单线程 force-finish”，`case2_50x_pressure + seed5 + 4gb` 可以 **稳定 5/5 通过**。这强烈说明当前残留问题确实在 **并发 sweep 路径**，而不是 root marking / write barrier 主路径。

## 6. 经验教训

1. **SATB shade-old 的剪枝必须考虑并发瞬态**。任何形如「dst/src 处于某个特殊形态时跳过 shade」的优化，都要回答「在并发 mutator 写交错下这个特殊形态会不会被错误地观测到？」
2. **失败模式变化往往是同一根因的下游表现，而不是新 bug**。本案中：value mismatch（漏 shade）→ SIGSEGV（漏 shade 后槽位被复用，访问野 typeinfo）。第一直觉「加 guard 兜底」错误地引导我们追了一个伪 Bug B；正确方法是定位漏 shade 的源头并修复，下游崩溃自然消失。
3. **诊断代码会污染验证**。本案中诊断 instrumentation（`fprintf` 内部 deref `obj->GetTypeInfo()`）本身就是 SIGSEGV 触发点之一，掩盖了 Bug A 的纯净修复效果。验证最终修复时必须**剥离所有诊断**，跑最小补丁。
4. **结构体写屏障的 obj guard 是一把双刃剑**。`IsObjSafeForStructBarrier` 这种 guard 命中时跳过 shade，会进一步掩盖 SATB race，可能让 mismatch 类失败更隐蔽。只在 GC 已不一致的兜底路径用得上，**不应作为主修复方案**。
5. **`git stash` + 最小补丁验证** 是定位"哪一行修复真的有效"的高效方法。
