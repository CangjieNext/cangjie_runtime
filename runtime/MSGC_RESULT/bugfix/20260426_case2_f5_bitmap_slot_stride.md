# case2 F5_HashMap 与 bitmap slot stride 排查记录

## 基本信息

- 标题：MSGC case2 在 2-slot bitmap sidecar 下 F5_HashMap 失败，`kBitmapSlotBytes=*3` 可恢复通过
- 日期：2026-04-26
- 提交/分支：主观察对象为 `209bda2d4` 附近实现；验证工作主要在 `runtime_wt_209_patch1/runtime` 上进行
- 平台：macOS aarch64

## 现象与复现

- 现象：`MSGC_RESULT/Result/case2_50x_pressure/benchmark.cj` 在 MSGC 下执行到 `F5_HashMap` 时抛出
  `IndexOutOfBoundsException: index: 0, size: 0`
- 失败位置：`benchF5_HashMap()` 中 `v[0]` / `v[1]` 读取，说明 `HashMap<Int64, ArrayList<Int64>>` 中取出的 `ArrayList` 已变为空或内容被破坏
- 最小复现条件（当前稳定复现口径）：
  1. 使用 MarkSweep runtime
  2. `seed5.env`
  3. `cjHeapSize=4gb`
  4. 运行 `case2_50x_pressure/benchmark.cj`
- 实际结果：
  - 2-slot 实现下，`F1-F4` 通过，`F5_HashMap` 失败
  - 将 `MSBitmapArena::kBitmapSlotBytes` 从 `*2` 改回 `*3` 后，可恢复 `5/5 passed`
- 期望结果：2-slot 与 3-slot 都应通过，且 2-slot 不应依赖额外物理槽位才能正确运行

## 关键复现命令

### 编译 runtime

```bash
cd /Users/lipeisheng/code/cangjie/cangjie/cangjie_runtime_MSGC/runtime_wt_209_patch1/runtime
/Users/lipeisheng/code/cangjie/cangjie/cangjie_runtime_MSGC/runtime/.venv/bin/python build.py build --gc-mode marksweep
```

### 替换 SDK runtime 并重签名

```bash
cp output/temp/lib/aarch64_Release/libcangjie-runtime.dylib \
  /Users/lipeisheng/code/cangjie/cangjie-pkg/dev/20260421/cangjie/runtime/lib/darwin_aarch64_cjnative/libcangjie-runtime.dylib
codesign -f -s - \
  /Users/lipeisheng/code/cangjie/cangjie-pkg/dev/20260421/cangjie/runtime/lib/darwin_aarch64_cjnative/libcangjie-runtime.dylib
```

### 复现 case2

```bash
source /Users/lipeisheng/code/cangjie/cangjie-pkg/dev/20260421/cangjie/envsetup.sh
cd /Users/lipeisheng/code/cangjie/cangjie/cangjie_runtime_MSGC/runtime/MSGC_RESULT/Result/case2_50x_pressure
cjc benchmark.cj -o <result_dir>/benchmark
source /Users/lipeisheng/code/cangjie/cangjie/cangjie_runtime_MSGC/runtime/MSGC_RESULT/Result/seed/seed5.env
export MRT_REPORT=<result_dir>/gc.log
export cjHeapSize=4gb
python3 /Users/lipeisheng/code/cangjie/cangjie/cangjie_runtime_MSGC/runtime/MSGC/benchmark/rss_monitor.py \
  --binary <result_dir>/benchmark \
  --label MSGC \
  --interval 200 \
  --vmmap-interval 0 \
  --csv <result_dir>/rss.csv \
  --output <result_dir>/rss.png \
  --event-csv <result_dir>/bench_events.csv \
  --stdout-file <result_dir>/stdout.txt
```

## 已确认的关键结果

### 1. 旧的“第三槽位被直接访问”证明无效

- 早期曾出现一次“3-slot guard 未被破坏且用例通过”的结果，但该轮验证遗漏了 `cjHeapSize=4gb`
- 这会改变触发条件，因此该轮结果已作废，不能作为根因依据

### 2. 在正确 4GB 条件下，未观测到直接第三槽位访问

- 正确结果目录：
  - `MSGC_RESULT/Result/case2_50x_pressure/result/20260426_170000_probe_real_thirdslot_guard_4gb_exec2/`
- 结果：
  - `F1-F4` 通过
  - `F5_HashMap` 抛 `IndexOutOfBoundsException`
  - 未出现第三槽位 canary 损坏
  - 未出现 mark/alloc fast-path 越界探针命中
- 结论：`*3` 能修复问题，但当前证据不支持“存在明确的 direct third-slot read/write”

### 3. “heap shrink 后残留越界 region 指针”假设在当前复现中未获支持

- 已新增 STW 探针：在 `EnumRoots()` 后扫描 CentralPool / AllocBuffer 中是否存在 `regionIdx >= committedRegionCount` 的陈旧引用
- 结果目录：
  - `MSGC_RESULT/Result/case2_50x_pressure/20260426_170722_probe_committed_range_slot2/`
- 关键结果：
  - 仍在 `F5_HashMap` 失败
  - 未出现 `region beyond committed range`
  - `gc.log` 中 `committedRegions` 一直为 `16384`
- 结论：本轮失败不是由 heap shrink 后的 stale region 引用直接触发

### 4. sweep 诊断确认失败表象是可达图被欠标，随后被清扫

- 结果目录：
  - `MSGC_RESULT/Result/case2_50x_pressure/20260426_173424_sweep_diag_slot2/`
- 运行条件：2-slot、`seed5.env`、`cjHeapSize=4gb`、`CJ_MS_SWEEP_DIAG=1`
- 结果：
  - 仍在 `F5_HashMap` 失败
  - 异常前 sweep 连续清理大量仍带有效 `TypeInfo` 的 `RawArray<Int64>` 槽位
  - 失败点仍是 `ArrayList<...>::[](Int64)` 报 `index: 0, size: 0`
- 这说明症状不是读写第三槽位时直接崩溃，而是某些仍会被 F5 容器图访问到的对象/子对象没有在本轮标记闭包中留下 mark bit，最终被 `SweepRegion()` 当作 dead slot 清零。

### 5. 强制扫描已标对象后，2-slot 在同条件下通过

- 验证 patch：在 `MSMarkBarrier::EnqueueRef()` 中加入运行时开关 `CJ_MS_FORCE_ENQUEUE_MARKED=1`
  - 原行为：`TryEnqueueGreyObject(ref, sink)` 如果发现 `ref` 已经 marked，会直接返回 false，不再入队扫描
  - 验证行为：若 `ref` 已 marked 但对象仍有效，则仍推入写屏障 buffer，使其后续被扫描一次
- 结果目录：
  - `MSGC_RESULT/Result/case2_50x_pressure/20260426_force_enqueue_marked_slot2/`
- 运行条件：2-slot、`seed5.env`、`cjHeapSize=4gb`、`CJ_MS_FORCE_ENQUEUE_MARKED=1`
- 结果：
  - `F1_TreeIntegrity` PASS
  - `F2_Generational` PASS
  - `F3_LargeObject` PASS
  - `F4_LinkedList` PASS
  - `F5_HashMap` PASS
  - `Functional: 5/5 passed`
  - 未出现 `IndexOutOfBoundsException`
  - 未出现 canary / alloc fast-path / mark bit / committed-range 诊断
- 结论：当前失败的直接原因是 mark 协议把“已经 marked”误当成“已经完成扫描”，导致 marked-but-not-scanned 对象不会再次入队。打开“已标对象也扫描”后，同一 2-slot 物理布局不再失败，说明根因落在灰/黑状态区分缺失，而不是直接第三槽位访问。

## 当前定位结论

## 标准库写屏障排查补充

### 1. `ArrayList.add` 不是简单裸写漏屏障

对 SDK 中 `libcangjie-std-collection.dylib` 反汇编后，`ArrayList.add` 的关键写入是 aarch64 inline barrier fast path：

```asm
1eef4: lsr x9, x28, #56
1eef8: cmp w9, #0x9
1eefc: b.ge 0x1ef08
1ef00: str x20, [x21]
1ef08: mov x0, x20
1ef0c: mov x1, x23
1ef10: mov x2, x21
1ef14: bl 0x6b434
```

其中 `0x6b434` 对应 `_CJ_MCC_WriteRefField`。`x28` 高 8 位来自 `MRT_GetThreadLocalData()` 中的 `GCPhase << 56`。MSGC 的 marking phase 值为 20/21/22，均大于 `GC_PHASE_ENUM=9`，理论上会走慢路径屏障。

因此，不能仅凭 `str x20, [x21]` 断定 `ArrayList.add` 漏屏障。除非后续运行时验证发现 managed code 中 `x28 >> 56` 在 MSGC marking 期间仍为旧值，否则该路径不是当前最强根因。

### 2. `HashMap.add` 的常规 key/value 写入有屏障

对 `__CNac7HashMapIG_G0_E3addHG_G0_` 反汇编，stub 映射如下：

- `0x6b410` -> `_CJ_MCC_AssignGeneric`
- `0x6b434` -> `_CJ_MCC_WriteRefField`
- `0x6b4ac` -> `_CJ_MCC_WriteGeneric`

在 `HashMap.add` 范围内观察到：

- `_CJ_MCC_WriteRefField` 调用 2 次：约 `0x16024`、`0x16078`
- `_CJ_MCC_WriteGeneric` 调用 3 次：约 `0x15f2c`、`0x16010`、`0x16050`
- 未观察到 `_CJ_MCC_AssignGeneric` / ArrayCopy 类调用

关键片段：

```asm
15ee8: bl 0x6acf4        ; _CJ_MCC_NewObjectFast, 分配新 Entry
15ef4: mov x25, x0       ; x25 = new Entry
15ef8: str x9, [x8, #0x8]!
15f08: str x21, [x9, #0x8]

15f20: mov x0, x25
15f24: mov x1, x21
15f28: mov x2, x20
15f2c: bl 0x6b4ac        ; _CJ_MCC_WriteGeneric

15f34: lsr x9, x28, #56
15f38: cmp w9, #0x9
15f3c: b.ge 0x16018
15f40: str x20, [x21]    ; inline fast path，marking phase 应跳过

1604c: mov x2, x22
16050: bl 0x6b4ac        ; _CJ_MCC_WriteGeneric
16078: bl 0x6b434        ; _CJ_MCC_WriteRefField
```

这说明 `HashMapEntry` 的常规 key/value 写入并非完全绕过 barrier。尤其是 `HashMap<Int64, ArrayList<Int64>>` 的 value 写入路径已有 `_CJ_MCC_WriteGeneric` / `_CJ_MCC_WriteRefField`。

### 3. 当前查到的真正危险点：新对象初始化 raw store

反汇编中仍存在紧跟 `_CJ_MCC_NewObjectFast` 之后的 direct store：

```asm
15ee8: bl 0x6acf4        ; _CJ_MCC_NewObjectFast
15ef4: mov x25, x0       ; new Entry
15ef8: str x9, [x8, #0x8]!
15f08: str x21, [x9, #0x8]
```

这类 store 是编译器按“新对象尚未发布，初始化写可省屏障”的假设生成的。这个假设在 stop-the-world 或 precise initialized-before-visible 模型中通常成立，但和当前 MSGC 的 alloc-black/mark-only 实现存在冲突：

1. 新对象在 marking phase 由 `MarkNewObject()` 直接 `MarkObject(obj)`。
2. `MarkNewObject()` 不把对象入 grey queue，release 下也没有独立 scanned 状态参与控制流。
3. 后续 `TryEnqueueGreyObject()` 遇到 already marked 对象会直接 skip。
4. 如果 direct store 写入的是引用字段，例如新 `HashMapEntry` 的 `next`、或构造期间写入的 backing array / child object，且该 child 不是通过其他 barrier 或 root 已被 discover，本轮标记闭包就可能漏掉它。

所以目前能静态查到的“漏屏障”不是 `HashMap.add` 的普通 key/value 更新，而是 compiler new-object initialization write barrier elision 与 MSGC alloc-black 协议不兼容。换句话说，这些 raw store 在当前协议下不是天然安全的。

### 4. CopyGC 为什么没有同类问题

CopyGC 不是靠“新对象被 mark 后一定会扫描”来保证这类初始化写安全。源码里普通对象分配路径 `RegionSpace::Allocate()` 只是返回新地址，并不会调用 `WCollector::MarkNewObject()`；`WCollector::MarkNewObject()` 主要由 `BaseObject::OnFinalizerCreated()` 触发，用于 finalizer 对象登记时的特殊保活。

CopyGC 的安全边界在 region/collection-set 层：

1. GC 开始时 `AssembleGarbageCandidates()` 只把当时已经在 `recentFullRegionList` 等列表里的 region 合入 `fromRegionList`，这些才是本轮可能被搬移/回收的候选。
2. Mutator 当前还在分配的 `tlRegionList` 不进入本轮 `fromRegionList`；如果它在本轮 GC 过程中变满，会被放到 `recentFullRegionList`，注释明确说明这类 full region 会 escape current GC，留到下一轮处理。
3. Trace 期间新申请的 TL / large region 会带 `traceRegion` 标记，满后进入 `fullTraceRegions` / `largeTraceRegions`，`PostTrace::HandleTraceRegions()` 再把它们合回 recent list，并清掉 trace flag。也就是说，trace 期间新分配 region 不会成为当前轮 from-space。
4. `Allocator::ShouldEnqueue()` 对 `IsTraceRegion()` 直接返回 false，避免把 trace 期间新 region 的对象塞进 SATB，保证并发 tracing 可以收敛。

因此，CopyGC 下新对象初始化 raw store 写入的新分配子对象通常不需要当前轮扫描：父对象和子对象所在的新 region 本轮都不会被回收。若 raw store 写入的是旧对象引用，CopyGC 依赖 SATB 删除屏障维持 snapshot-at-the-beginning：旧对象要么已在 root/对象图快照中，要么在被覆盖时进入 SATB；TraceBarrier 的普通写只记 old ref 也正是这个模型。

MSGC 当前不同：它是对象粒度 sweep，mark bit 同时承担 live/queue 去重语义；alloc-black 把新对象直接设为 marked，但该对象仍在会被 sweep 观察的 MS heap 中。后续写屏障发现同一个 already-marked 对象时，`TryEnqueueGreyObject()` 会跳过扫描，于是“marked 但未扫描”的对象可能留下未闭合的子引用。这也是 `CJ_MS_FORCE_ENQUEUE_MARKED=1` 能绕过 case2 的协议层解释。

### 5. 仍需运行时验证的两个分支

1. 验证 `x28` phase 是否 stale：如果 marking 中 `x28 >> 56 < 9`，则 `ArrayList.add` / `HashMap.add` 的 inline fast path 会实际裸写，这是明确漏屏障。
2. 验证 new-object raw store 是否写 ref 且 child 未被其他路径 discover：重点看 `HashMapEntry.next`、`ArrayList` 构造时 backing array 字段、以及 `RawArray<HashMapEntry<...>>` 中 entry value 的 `WriteGeneric` 是否实际进入 `MSMarkBarrier::WriteStruct` 并 enqueue new ref。

目前已有的 `CJ_MS_FORCE_ENQUEUE_MARKED=1` 通过，说明即使没有唯一定位到某条机器指令，协议层面确实存在“已 marked 但仍需要扫描”的对象。上述 raw initialization store 是与该现象最吻合的静态候选。

### 已排除项

- `TraceRefField` 本身不是修复点
- “必须设置 `kBitmapSlotBytes=*3` 才能满足显式第三段访问”尚未被证实
- “heap shrink / committed range 收缩导致过期 region 指针”在当前 seed5 + 4GB 复现里未命中

### 当前最可靠的事实

- 2-slot 失败不是由 direct third-slot read/write、bitmap fast-path 越界或 heap shrink stale region 直接触发
- 2-slot 失败可以通过“已 marked 对象也入队扫描”恢复通过
- 因此当前根因不是“少了一块可随意浪费的 padding”，而是实现上缺少独立的灰/黑状态：
  - `markBitmap` 只能表示对象 live/marked
  - mark queue 需要知道对象是否已经 scanned
  - alloc-black 会把新对象直接设为 marked
  - 写屏障后续发现这个对象时，`TryEnqueueGreyObject()` 因 marked bit 已经存在而跳过入队
  - 如果这个 marked 对象还没有被扫描过，它的子引用不会进入本轮闭包，子对象最终可能被 sweep 清零

### 当前最可能的方向

1. 已实现新的验证方向：把本轮 sweep snapshot 前移到 MarkSetup，GC 开始后归还的 TL/fresh region 盖当前 `globalSweepGen` 后进入 swept 队列，绕过本轮 sweep。
2. 同步移除 new-time alloc-black：小对象分配不再调用 `MarkNewObject()`，large object 注册不再设置 `FLAG_MARKED`。
3. 该方案使用现有 sweep generation 表达“本轮候选集合”：当前 central pool 中的 swept region 在 MarkSetup 被翻入 unswept；之后归还/新建的 region 保持当前 generation，下一轮 MarkSetup 才会被翻入 unswept。
4. 仍需回归验证：如果 case2 仍失败，则说明问题不只是“新 region 被本轮 sweep 卷入”，还存在从新对象指向旧 sweep-candidate region 的漏标边，需要恢复“marked”和“scanned/black”的独立状态。

## 本轮新增探针与代码记录

### 已验证的探针

- `MSRegionInfo.cpp`
  - padding / real-third-slot canary
  - `SetMarkBit` / `GetMarkBit` / `TestAndSetMarkBit` 越界日志
- `MSCentralRegionPool.cpp`
  - `LogRegionsBeyondCommittedRange()`
- `MarkSweepCollector.cpp`
  - 在 `EnumRoots()` 后调用 committed-range 探针

### 已完成的新探针

- 在 `runtime_wt_209_patch1/runtime/src/Heap/MAllocator/MSRegionInfo.cpp` 中新增了一个局部 sweep 诊断：
  - 在 `SweepRegion()` 清零 dead slot 之前
  - 若槽位内容看起来仍像 `HashMap` / `HashMapEntry` / `ArrayList` / `RawArray`
  - 则输出一次诊断日志
- 验证结果：`20260426_173424_sweep_diag_slot2` 中命中大量 `RawArray<Int64>`，并最终在 `F5_HashMap` 失败

### 已完成的协议验证 patch

- 在 `runtime_wt_209_patch1/runtime/src/Heap/MBarrier/MSMarkBarrier.cpp` 中加入 `CJ_MS_FORCE_ENQUEUE_MARKED`
- 目的：验证“marked-but-not-scanned 被跳过”是否是漏标根因
- 验证结果：`20260426_force_enqueue_marked_slot2` 在 2-slot 下通过 `Functional: 5/5 passed`
- 注意：该 patch 只是诊断手段，不应作为正式修复直接合入；它会带来大量重复扫描，正确方向是恢复 scanned/black 状态或等价状态位

### 正式修复验证：region snapshot + no alloc-black

- 修复路径：本轮 sweep snapshot 前移到 MarkSetup；GC-active 期间归还的 TL/fresh region 盖当前 `globalSweepGen` 后进入 swept 队列，跳过本轮 sweep；同时移除小对象和 large object 的 new-time alloc-black。
- 验证目录：`MSGC_RESULT/Result/case2_50x_pressure/20260426_snapshot_no_allocblack_seed5`
- 验证环境：seed5，`cjHeapSize=4gb`，未设置 `CJ_MS_FORCE_ENQUEUE_MARKED`，未设置 sweep diag。
- 验证结果：`Functional: 5/5 passed`，`F5_HashMap PASS 8034 ms (1000 rounds)`，benchmark exit code 为 0，峰值 RSS 约 117.8 MB。
- 结论：case2/F5 的直接回归已通过；当前修复不依赖“已 marked 也强制入队”的诊断 patch。

## 关键函数/调用链

- bitmap sidecar / stride
  - `src/Heap/MAllocator/MSBitmapArena.h`
  - `src/Heap/MAllocator/MSBitmapArena.cpp`
- per-region bitmap 布局与分配/清扫
  - `src/Heap/MAllocator/MSRegionInfo.h`
  - `src/Heap/MAllocator/MSRegionInfo.cpp`
- 全局 mark bitmap 清理与 sweep 协调
  - `src/Heap/MAllocator/MSCentralRegionPool.cpp`
  - `src/Heap/MCollector/MarkSweepCollector.cpp`
- 失败业务点
  - `MSGC_RESULT/Result/case2_50x_pressure/benchmark.cj`
  - `benchF5_HashMap()`

## 代表性日志

### stdout / monitor

```text
[BENCH] F1_TreeIntegrity         PASS
[BENCH] F2_Generational          PASS
[BENCH] F3_LargeObject           PASS
[BENCH] F4_LinkedList            PASS
IndexOutOfBoundsException: index: 0, size: 0
    at default::benchF5_HashMap()
```

### gc.log（committed-range 探针那一轮）

```text
[MS-GC] ClearAllMarkBitmaps: ... committedRegions=16384
[MS-GC] ReclassifyUnsweptToSwept: partial=0 regions, full=0 regions, total=0
```

未出现：

```text
[MS-GC] region beyond committed range: ...
```

## 当前结论状态

- 根因：已通过验证收敛到 mark 协议缺少独立 scanned/black 状态
- 已确认：
  - 直接第三槽位访问不是当前最强解释
  - stale region after shrink 不是当前最强解释
  - F5 失败对应“仍会被访问的对象图没有完成标记闭包，随后被 sweep 清零”
  - 强制已 marked 对象也入队扫描后，同一 2-slot 布局通过 case2
- 后续修复方向：
  - 保留三份物理 bitmap 槽位并恢复 `scannedBitmap` 的协议语义；或
  - 在不使用第三物理槽位的前提下，为每个对象/slot 提供等价的 scanned/black 状态

## 验证与回归

- 已验证命令：见“关键复现命令”章节
- 已验证结果：
  - 2-slot：稳定在 `F5_HashMap` 失败
  - `kBitmapSlotBytes=*3`：可恢复通过
  - committed-range 探针：未命中 stale region
  - real-third-slot guard：在正确 4GB 条件下未命中 direct third-slot 访问
  - sweep 诊断：失败前命中大量 `RawArray<Int64>` 被当作 dead slot 清零
  - `CJ_MS_FORCE_ENQUEUE_MARKED=1`：2-slot 在同条件下恢复 `Functional: 5/5 passed`
- 回归风险：
  - 如果直接把“重复扫描所有已 marked 对象”当作正式修复，会引入明显性能风险
  - 如果只把 `*3` 理解为 padding，而不恢复 scanned/black 语义，后续优化 mark/sweep/alloc 路径时仍可能重新漏扫