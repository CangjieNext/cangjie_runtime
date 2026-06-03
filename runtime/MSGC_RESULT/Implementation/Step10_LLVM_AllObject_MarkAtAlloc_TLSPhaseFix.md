# Step 10：LLVM 全对象 Mark-at-Alloc 与 TLS Phase 同步修复

> 2026-05-09 基于 case1_baseline + seed5 + output SDK 的根因、修复与验证记录
>
> 平台：macOS aarch64
>
> 目标：LLVM 在 MSGC mark 阶段允许任意对象走 NewObject fast path，并在编译器生成的 fast path 中直接标记新对象，不影响正确性。

---

## 1. 背景

LLVM AArch64 NewObject fast path 已扩展为所有 GC phase 都可以进入。为了保持 MSGC mark-at-alloc 语义，在以下阶段 fast path 分配成功后会直接设置对象所在 region 的 mark bitmap：

- `GC_PHASE_MS_MARK_SETUP`
- `GC_PHASE_MS_CONCURRENT_MARK`
- `GC_PHASE_MS_MARK_TERMINATION`

该 inline mark 的语义应与 runtime slow path `MarkSweepCollector::MarkNewObject()` 一致：只标记对象本身为 black，不负责把对象引用字段入队。引用字段的可达性由写屏障/SATB 语义保证。

---

## 2. 根因

问题不在 “是否需要 TryEnqueueGreyObject”。当前 runtime 的 `MarkNewObject()` 本身也只是调用 `MarkObject(obj)`，不会 enqueue；因此 LLVM inline mark 只置 mark bitmap 与 runtime 语义一致。

真正根因是 mutator 的 GC phase 存在两份状态：

- `Mutator::mutatorPhase`
- TLS 中给编译器 fast path 读取的 `ThreadLocalData::encodedGCPhase` / `gcFastState`

GC 线程在 `MutatorManager::EnsurePhaseTransition()` 中可能替 saferegion 内的 mutator 执行 `TransitionGCPhase(false)`。旧实现中 `Mutator::SetMutatorPhase()` 只有在当前线程就是该 mutator 时才刷新 TLS：

```cpp
if (ThreadLocal::GetMutator() == this) {
    ThreadLocal::SetGCFastState(...);
    ThreadLocal::SetEncodedGCPhase(...);
}
```

当 phase transition 由 GC 线程代劳时，只更新了 `mutatorPhase`，没有更新目标 mutator 自己 TLS 中的 `encodedGCPhase/gcFastState`。之后该 mutator 从 saferegion 恢复运行时，LLVM fast path 仍可能看到旧 phase，于是在 mark 阶段跳过 inline mark-at-alloc。runtime slow path 读取 collector/global phase，因此不受该问题影响。

这也解释了为什么该问题只在 LLVM inline fast path 暴露：编译器生成的分配 fast path 和部分 barrier fast path 都依赖 TLS phase，而不是每次调用 runtime 查询全局 phase。

---

## 3. 修复

修改文件：

- `src/Mutator/Mutator.h`
- `src/Mutator/MutatorManager.cpp`

修复点：

- 抽出 `SetThreadLocalGCPhase(ThreadLocalData*, GCPhase)`，统一写入 `gcFastState` 和 `encodedGCPhase`。
- 当前线程就是目标 mutator 时，保持原来的直接刷新 TLS 行为。
- GC 线程替 saferegion mutator transition phase 时，通过 `safepointStatePtr` 反推出目标 `ThreadLocalData`，并在 `targetLocal->mutator == this` 校验通过后刷新目标 TLS phase。
- mutator 重新 `PreparedToRun()` 时，再用 `GetMutatorPhase()` 刷新一次 TLS phase，覆盖 parked/unbound 场景。

这样 `mutatorPhase` 与编译器 fast path 使用的 TLS phase 在跨线程 phase transition 后保持一致。

### 3.1 二次修复：关闭 global phase 与 TLS phase 的切换窗口

case2 + seed5 后续验证中仍出现过一次 `F5_HashMap` 间歇失败：

```text
IndexOutOfBoundsException: The length of the array is 0, but the index is 0.
at default::benchF5_HashMap()
```

进一步分析发现，第一版修复只处理了“phase transition 完成后 TLS 没同步”的问题，但 `TransitionAllMutatorsToGCPhase()` 仍有一个更短的窗口：

```text
InstallBarrier(newPhase)
SetGCPhase(newPhase)
VisitAllMutators(SetSuspensionFlag / SetSafepointActive)
EnsurePhaseTransition(...)
```

在 `SetGCPhase(newPhase)` 之后、目标 mutator 完成 handshake 之前，运行中的 mutator 仍可能用旧 TLS phase 执行 LLVM fast path。这样写屏障 phase gate / NewObject mark-at-alloc 仍可能短暂误判。

第二段修复是在 MSGC 下把目标 phase 预发布到所有 mutator TLS：

```text
InstallBarrier(newPhase)
VisitAllMutators(SetMutatorPhase(newPhase))   # 只预发布 TLS / mutatorPhase
SetGCPhase(newPhase)
VisitAllMutators(SetSuspensionFlag / SetSafepointActive)
EnsurePhaseTransition(...)
```

关键点：

- `InstallBarrier()` 先执行，保证 TLS 预发布后如果 fast barrier 进入 runtime，runtime barrier 已经是新 phase 语义。
- `SetMutatorPhase(newPhase)` 只预发布 fast path 可见 phase，不设置 `FINISH_TRANSITION`。
- 随后的正常 handshake 仍会执行 `HandleGCPhase(newPhase)`，因此 `MS_MARK_SETUP` 的栈根枚举不会被跳过。
- `SetGCPhase(newPhase)` 之后，TLS 已经不再落后于 global phase，关闭了 LLVM fast path 误判窗口。

---

## 4. 验证

SDK 组合：

- compiler：`/Users/lipeisheng/code/cangjie/cangjie/cangjie_compiler/output/bin/cjc`
- stdlib：`/Users/lipeisheng/code/cangjie/cangjie/cangjie_compiler/output/libs`
- runtime：本次 release/O2 marksweep runtime，替换到 `/Users/lipeisheng/code/cangjie/cangjie/cangjie_compiler/output/runtime/lib/darwin_aarch64_cjnative/libcangjie-runtime.dylib`

执行要求：

- 替换 runtime 后重新编译 `benchmark.cj`。
- 使用隔离 `env -i` 子 shell。
- 运行时必须由 source 后的同一 shell 直接启动 `./benchmark`，避免 `/usr/bin/time` 或 Python 作为 launcher 时 macOS 影响 `DYLD_LIBRARY_PATH`。

产物目录：

```text
MSGC_RESULT/Result/case1_baseline/result/20260509_162606_tls_phase_sync_allmark_seed5_output_sdk
```

反汇编确认：

- NewObject fast path 入口存在 safepoint gate：读取 `x28 + 0x30`。
- fast path 分配后读取 TLS phase：`ldrb w8, [x28, #0x4e]`。
- phase 为 `0x14/0x15/0x16` 时设置 region mark bitmap：读取 region `+0x20`，使用 `ldxr/orr/stxr` 原子置位。

case1 + seed5 连续 5 次结果：

```text
run,exit_code,elapsed_s,peak_rss_kb,avg_cpu_pct
1,0,4.275280,56224,108.305
2,0,4.248420,53296,107.260
3,0,4.133710,54352,108.164
4,0,4.277530,60560,105.438
5,0,4.739120,56336,99.628
```

平均值：

```text
avg_elapsed_s=4.334812
avg_peak_rss_kb=56153.6
avg_cpu_pct=105.759
```

运行日志中未发现 `error/assert/invalid/panic/Exception` 等异常关键字。

二次修复后，case2 + seed5 连续 3 次结果：

```text
run,exit_code,elapsed_s,total_wall_ms,functional,peak_rss_kb,avg_cpu_pct
1,0,196.876160,196363,5/5,62336,115.206
2,0,198.568540,198093,5/5,62928,115.887
3,0,200.215450,200068,5/5,61712,116.484
```

平均值：

```text
avg_elapsed_s=198.553383
avg_total_wall_ms=198174.7
avg_peak_rss_kb=62325.3
avg_cpu_pct=115.859
```

产物目录：

```text
MSGC_RESULT/Result/case2_50x_pressure/result/20260509_171343_output_tls_prepublish_seed5_3runs
```

---

## 5. 结论

LLVM inline mark-at-alloc 本身可以对任意对象执行，不需要在编译器阶段按“是否含引用字段”过滤，也不需要额外 enqueue 新对象引用字段。

正确性前提是：编译器 fast path 读取的 TLS GC phase 必须与 runtime mutator phase transition 同步，并且不能落后于 global phase 的发布。旧实现有两层问题：

- GC 线程代替 saferegion mutator 切 phase 时漏刷目标 TLS。
- global phase 已切到新 phase、mutator TLS 尚未切到新 phase 的短窗口内，LLVM fast path 仍可能用旧 phase 误判。

修复这两层后，case1 + seed5 连续 5 次通过，case2 + seed5 连续 3 次通过。
