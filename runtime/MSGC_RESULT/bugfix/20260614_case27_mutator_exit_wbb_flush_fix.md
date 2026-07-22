# case27 mutator 退出时 WBB 丢失问题修复记录

## 基本信息

- 日期：2026-06-14
- 场景：`MSGC_RESULT/Result/case27_http_complex_json`
- 用例口径：`CASE27_CONCURRENCY=8`，`CASE27_PAYLOAD=100K`，`CJ_MS_VERIFY_MARK=1`
- 问题类型：MSGC 并发标记期间，退出中的 mutator 丢失 write barrier 新边
- 相关文件：
  - `src/Mutator/MutatorManager.cpp`
  - `src/Heap/MCollector/MarkSweepCollector.h`
  - `src/Heap/MCollector/MarkSweepCollector.cpp`

说明：

- 本文只记录这次 `mutator exit + WBB` 丢边问题。
- 它和前面已经单独记录的 `finalizer / MarkNewObject deferred scan` 不是同一个问题。

## 1. 分析方式

### 1.1 复现与观测

首先严格按 `case27_http_complex_json/README.md` 中规定的方法跑对比，不自己改运行方式：

```bash
MSGC_SDK_ROOT=/Users/lipeisheng/code/cangjie/cangjie-pkg/self/dev260609/cangjie \
MSGC_STDX_ROOT=/Users/lipeisheng/code/cangjie/cangjie-pkg/self/dev260609/darwin_aarch64_cjnative \
COPYGC_SDK_ROOT=/Users/lipeisheng/code/cangjie/cangjie-pkg/dev/20260507/cangjie \
COPYGC_STDX_ROOT=/Users/lipeisheng/code/cangjie/cangjie-pkg/dev/20260507/darwin_aarch64_cjnative \
CASE27_PAYLOAD=100K CASE27_CONCURRENCY=8 CASE27_DURATION_MS=60000 \
HEAP_SIZE=12gb PROCS=6 CJ_MS_VERIFY_MARK=1 \
./run_compare.sh
```

关键现象来自修复前的结果目录：

- `result/20260614_225333_msgc_vs_copygc_c8_p100K_compare`

在这轮里：

- `CopyGC rc=0`
- MSGC 侧历史上出现过 `SIGSEGV` / 坏 JSON / verifier `missed-mark`
- verifier 输出里除了大量 JSON 容器项，还能看到明显的 `Thread` / `Timer` 相关 referrer：
  - `RawArray<std.sync:Timer>`
  - `std.core:Thread`
  - `std.sync:Timer`

### 1.2 缩小范围

这一步的做法是把“真实漏标”和“verifier 对 JSON 联合体/内联 payload 的噪声”分开。

具体做法：

1. 先看 verifier 的 referrer 类型分布，而不是只看总数。
2. 把 `Thread` / `Timer` 这类明显不应该出现在 JSON 扫描噪声里的项单独拎出来。
3. 再顺调用链看这些对象为什么会在并发标记期间变白。

顺着代码看到了两个关键点：

1. `MarkTermination()` 里的 `FlushAllWriteBarrierBuffers()` 只会遍历“当前还能被 GC 枚举到的 mutator”。
2. `ScheduleAllCJThreadListRemove()` 会先把 `CJThread` 从 `allCJThreadList` 里摘掉，再调用析构 hook。

对应代码位置：

- `src/Heap/MCollector/MarkSweepCollector.cpp`
- `src/CJThread/src/runtime/schedule/src/schedule.cpp`

这意味着如果一个 `CJThread` 在并发标记期间退出：

- 它对应的 mutator 可能在 `MarkTermination` 之前就从可枚举列表里消失；
- 但它自己的 `wbBuf_` 里仍然可能留着尚未被 STW drain 的灰对象。

另外又补看了 `Mutator::ResetMutator()`：

- `src/Mutator/Mutator.h`

这里会直接：

- `wbBuf_.Clear();`
- `assistLocalWork_.clear();`

所以一旦走到退出路径，未刷出的 WBB 新边会被直接清空。

### 1.3 根因确认

为了把怀疑变成证据，临时加了一个退出诊断日志：

- 在 `MutatorManager` 里打印：当 mutator 在 MS mark phase 退出时，`wbBuf_` 里还剩多少条。

修复前重跑后，在 `gc.log/stdout` 中能稳定看到类似日志：

```text
[MS-GC] MutatorExitPendingMarkWork where=transit phase=21 wbBuf=5 ...
```

而且这些日志就出现在：

- `ConcurrentMark` 结束前后
- `MarkTermination` 之前

同时，同一轮 verifier 会继续报 `missed-mark`。

这说明根因不是“对象压根没被 barrier 发现”，而是：

- **barrier 已经把对象放进退出 mutator 自己的 `wbBuf_` 了；**
- **但 mutator 在 STW flush 之前退出，这些条目没有进入全局 mark work，就被清掉了。**

## 2. 结果

### 2.1 根因结论

本次确认的根因是：

- **MSGC 并发标记期间，退出 mutator 的 `WriteBarrierBuffer` 中仍有待处理新边；**
- **这些新边在 `MarkTermination` 的统一 flush 之前丢失；**
- **随后对应 target 没有进入本轮 mark 闭包，造成真实漏标。**

更具体地说，问题链条是：

1. mutator 在 mark phase 内通过 barrier 产生新灰对象；
2. 新灰对象先进入该 mutator 的 `wbBuf_`；
3. 该 mutator 提前退出，并从 `CJThread` 全局列表中移除；
4. `FlushAllWriteBarrierBuffers()` 已经看不到它；
5. `ResetMutator()` 又把 `wbBuf_` 清空；
6. 最终这批新边丢失。

### 2.2 修复后现象

修复后重新编译、替换 runtime，并再次按 README 方法跑：

- 结果目录：`result/20260614_230843_msgc_vs_copygc_c8_p100K_compare`

这轮的直接结果：

- `CopyGC rc=0`
- `MSGC rc=0`
- 没有再出现这条问题链引发的 `SIGSEGV`

同时日志里能看到两类新证据：

1. 退出日志仍然存在，但都变成了：

```text
MutatorExitPendingMarkWork ... flushed=1
```

2. 新增的 exit flush 日志明确表明这些 WBB 条目已经被转移：

```text
[MS-GC] ExitFlush source=WBB mutatorTid=0 buffered=556 queued=556 dropped=0 workBufs=1
```

另外，对比修复前后的 verifier referrer 分布：

- 修复前：
  - `RawArray<std.sync:Timer>` 1515 条
  - `std.core:Thread` 160 条
  - `std.sync:Timer` 91 条
- 修复后：
  - 这些 `Thread/Timer` 类项已经消失
  - 剩余 referrer 全部收敛到 JSON 相关容器和值类型

这说明：

- **“退出 mutator 丢 WBB 新边”这一类真实漏标已经被修到。**

### 2.3 剩余现象

这次修复后 verifier 还没有清零。

修复后这一轮统计：

- `missed-mark = 4013`
- `mark-completeness = 1566`

但剩余条目已经明显不同：

- referrer 只剩：
  - `RawArray<stdx.encoding.json:JsonValue>`
  - `RawArray<std.collection:HashMapEntry<...JsonValue>>`
  - `ArrayList<JsonValue>`
  - `JsonObject / JsonString / JsonArray / Option<JsonValue>`
- target 全是 `<?>`
- 很多记录的 `traceable=0`
- 部分 `stateWord` 看起来像 ASCII 数据或内联内容，不像正常对象头

因此当前结论是：

- 这次修掉的是一条**真实 GC 丢边问题**；
- verifier 剩余未清零的部分，已经更像是 **JSON / `JsonValue` 扫描路径或 verifier 精度问题**，需要单独继续分析。

## 3. 怎么修复的

### 3.1 修复思路

修复思路很直接：

- 不再依赖“等到 `MarkTermination` 再从 mutator 列表里把这个退出线程枚举到”。
- 而是在 mutator 退出/销毁前，如果当前正处于 MS mark phase，并且 `wbBuf_` 非空，就**当场把这些条目转移到 collector 的 `globalFull_`**。

这样做的效果是：

- 这些灰对象即使所属 mutator 立刻退出，也不会丢；
- 后续 `ConcurrentMark` / `MarkTermination` 仍然能从全局 work 中继续处理它们。

### 3.2 代码修改点

1. 在 `MarkSweepCollector` 中新增专门的退出 flush 接口：

- `src/Heap/MCollector/MarkSweepCollector.h`
- `src/Heap/MCollector/MarkSweepCollector.cpp`

新增方法：

- `FlushExitedMutatorWriteBarrierBuffer(WriteBarrierBuffer& wbBuf, uint32_t mutatorTid)`

作用：

- 遍历退出 mutator 的 `wbBuf_`
- 过滤无效项
- 直接打包成 `WorkBuf`
- 推到 `globalFull_`
- 最后 `wbBuf.Clear()`

2. 在 `MutatorManager` 中统一接入退出前 flush：

- `src/Mutator/MutatorManager.cpp`

新增辅助函数：

- `MaybeFlushPendingMSMarkWorkOnMutatorExit(...)`

接入点：

- `TransitMutatorToExit()`
- `DestroyMutator()`
- `DestroyRuntimeMutator()`

也就是说，不管是普通 `CJThread` 退出，还是 runtime thread 销毁，只要命中 MS mark phase，都会先尝试把待处理 WBB 新边刷走。

### 3.3 验证方式

验证链路如下：

1. 重编 MSGC runtime：

```bash
python3 build.py build --gc-mode marksweep
```

2. 替换 `dev260609` 中的 runtime 并 `codesign`
3. 重新按 README 方式运行：

```bash
CASE27_PAYLOAD=100K CASE27_CONCURRENCY=8 CJ_MS_VERIFY_MARK=1 ./run_compare.sh
```

4. 对比修复前后两个目录：

- 修复前：`result/20260614_225333_msgc_vs_copygc_c8_p100K_compare`
- 修复后：`result/20260614_230843_msgc_vs_copygc_c8_p100K_compare`

本次验证结论：

- 主问题已修：退出 mutator 的 WBB 新边不再丢失，`Thread/Timer` 类 verifier 漏标消失。
- 结果改善：MSGC 本轮能稳定跑完，`rc=0`。
- 尚未结束：verifier 仍有 JSON 相关剩余项，后续需要继续沿 `JsonValue` / 容器扫描路径排查。
