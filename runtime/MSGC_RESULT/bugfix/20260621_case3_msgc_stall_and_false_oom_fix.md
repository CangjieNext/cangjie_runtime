# case3 MSGC stall 与 large alloc 假 OOM 修复记录

## 基本信息

- 日期：2026-06-21
- 场景：`MSGC_RESULT/Result/case3_half_alive`
- SDK：`/Users/lipeisheng/code/cangjie/cangjie-pkg/self/dev260619/cangjie`
- 相关结果目录：
  - 卡住现场采样：`MSGC_RESULT/Result/case3_half_alive/result/debug_20260620_223539_msgc_single`
  - 卡住问题修复后、暴露 large alloc 假 OOM：`MSGC_RESULT/Result/case3_half_alive/result/verify_20260620_case3_dev260619_msgc_shrink_notify_fix`
  - 最终干净验证：`MSGC_RESULT/Result/case3_half_alive/result/verify_20260621_case3_dev260619_msgc_large_run_fix_clean`
- 本次修改文件：
  - `src/Heap/MAllocator/MSFreeManager.cpp`
  - `src/Heap/MAllocator/MSFreeManager.h`
  - `src/Heap/MAllocator/MSRegionManager.cpp`
  - `test/MSFreeManagerLargeStealTest.cpp`

## 问题 1：大对象增长阶段卡住

### 现象

- `case3` 在 MSGC 下会出现“进程没退出、也没继续前进”的卡住现象。
- 现场采样文件 `debug_20260620_223539_msgc_single/sample.txt` 显示：
  - 主线程卡在大对象分配路径：
    `RegionSpaceMS::AllocateLargeObject`
    -> `TryAllocateLargeObjectOnce`
    -> `MSRegionManager::TakeRegionForLarge`
    -> `MSRegionManager::TakeRegions`
    -> `FreeManager::GrowForRegions`
    -> `FreeManager::GrowOneSegmentLocked`
  - `gc-helper` 同时卡在后台 shrink 路径：
    `BackgroundProcessor::BackgroundShrinkLoop`
    -> `MSRegionManager::ShrinkOneIdleSegment`
    -> `FreeManager::GetSegmentFreeSnapshot`
- 从用户视角看，就是 benchmark 很早就不再推进，`stdout.txt` 常常还是空的，或者长期停留在刚启动阶段。

### 关键调用链

- mutator 大对象增长路径：
  `RegionSpaceMS::TryAllocateLargeObjectOnce`
  -> `MSRegionManager::TakeRegionForLarge`
  -> `MSRegionManager::TakeRegions`
  -> `FreeManager::GrowForRegions`
  -> `FreeManager::GrowOneSegmentLocked`
- 后台 shrink 路径：
  `BackgroundProcessor::BackgroundShrinkLoop`
  -> `MSRegionManager::ShrinkOneIdleSegment`
  -> `FreeManager::GetSegmentFreeSnapshot`
  -> `FreeManager::ReleaseCompletelyFreeSegment`

### 根因

- **根因 1A：grow 路径和 shrink 路径存在锁顺序反转。**
- 修复前：
  - `GrowOneSegmentLocked()` 先拿 `owner shard` 的 `shardMutex_`，再拿全局 `pageHeapMutex_`。
  - `GetSegmentFreeSnapshot()` / `ReleaseCompletelyFreeSegment()` 先拿 `pageHeapMutex_`，再拿 `shardMutex_`。
- 这两条路径一旦并发命中同一个 owner shard，就会形成典型的 ABBA 反转：
  - mutator 手里拿着 `shardMutex_`，等待 `pageHeapMutex_`
  - helper 手里拿着 `pageHeapMutex_`，等待 `shardMutex_`
- 结果不一定每次都表现成严格意义上的“永久死锁”，但至少会表现成非常长时间的互相等待，足以把 `case3` 卡住。

- **根因 1B：mutator 热路径上的 shrink 探测把碰撞窗口放大了。**
- 修复前，`MSRegionManager::TakeRegions()` 每次 grow 成功后都会走 `MaybeNotifyShrink()`。
- 而旧的 `MaybeNotifyShrink()` 不是简单发一个后台通知，而是会在 mutator 线程上同步做 `HasPendingShrinkWork(pool)` 这类重探测。
- `case3` 的 `PHASE1` 是连续的大对象突发分配，这会让 grow/shrink 探测高频交织，进一步放大上面的锁竞争。

### 修复方式

- **修复 1A：重排 grow 路径的拿锁方式，避免“拿着一个锁等另一个锁”。**
- `GrowOneSegmentLocked()` 改成：
  1. 先拿 `pageHeapMutex_`
  2. 再 `try_lock` 对应 shard 的 `shardMutex_`
  3. 如果 shard 正忙，立刻释放 `pageHeapMutex_`，`yield` 后重试
- 这样 mutator 不会再“拿着 shard 锁去等 page 锁”。

- **修复 1B：让 shrink 路径不要拿着全局 page 锁去等 shard 锁。**
- `GetSegmentFreeSnapshot()` 和 `ReleaseCompletelyFreeSegment()` 改成：
  1. 在 `pageHeapMutex_` 保护下只读取最小必要的全局元数据快照
  2. 立即释放 `pageHeapMutex_`
  3. 再对 shard 做 `try_lock`
  4. 如果 shard 正忙，则直接返回 empty / 0，让后台下次再试
- 这样 helper 不会把全局 `pageHeapMutex_` 长时间钉住。

- **修复 1C：把 `MaybeNotifyShrink()` 改成纯唤醒，不在 mutator 热路径做重探测。**
- 现在它只检查 `CentralRegionPool` 是否存在，然后调用 `BackgroundProcessor::NotifyToShrink()`。
- 真正的 shrink-work 判断留给 helper 线程自己做，mutator 不再同步扫 segment / central pool cache。

### 修复后的行为变化

- 修完这组问题后，`case3` 不再卡在早期增长阶段。
- 实际观察到的现象是：benchmark 从“卡住不动”变成了“能继续走，但随后在 large alloc 上报错”。
- 这说明第一类问题确实被打穿了，后面暴露出来的是第二类独立问题，而不是原来的卡住还在。

## 问题 2：large allocation 假 OOM

### 现象

- 在第一类卡住问题修掉后，`case3` 不再早期挂住，但会在 large alloc 上报错。
- 关键日志来自：
  - `verify_20260620_case3_dev260619_msgc_shrink_notify_fix/gc_msgc_manual.log`
  - `verify_20260620_case3_dev260619_msgc_shrink_notify_fix/stderr.txt`
- 直接证据：
  - `TryAllocateOnceMS LARGE ALLOC FAILED: allocSize=8388624, unitCount=513, ... msRegionManager freeUnits=33550996`
  - `Large allocation cannot allocate memory of 8388624(B), throw an OutOfMemory exception`
  - 同时 GC 日志又显示：
    - `current heap: 549,755,813,888 B`
    - `shard[4]: ... 16765258 free (hot=1849095, cold=14916163)`
- 也就是说：**不是全堆没空间，而是 allocator 没把“全堆其实有空间”转化成这次 large alloc 可用的连续 region run。**

- `stderr.txt` 里后面跟着一串 `SIGSEGV` 打印。
- 这不是 primary root cause。primary root cause 是上面的 large alloc 失败；后面的 `SIGSEGV` 是 OOM/异常打印路径在 signal-stack 打印时再次踩进 runtime frame 的次生噪音，不要把它误判成第一现场。

### 关键调用链

- large object 分配路径：
  `RegionSpaceMS::AllocateLargeObject`
  -> `TryAllocateLargeObjectOnce`
  -> `MSRegionManager::TakeRegionForLarge`
  -> `MSRegionManager::TakeRegions`
  -> `FreeManager::TryTakeRegions`

### 根因

- **根因 2A：多 region 请求只会在本地 shard 找，不会像单 region 一样去 donor shard 借连续 run。**
- 修复前，`FreeManager::TryTakeRegions()` 的逻辑是：
  1. 先在 `homeShard` 本地找连续 free run
  2. 如果失败：
     - `num == 1` 时，允许走 `TryStealSingleRegionBatchLocked()`
     - `num > 1` 时，没有 donor-steal 回退，直接结束本地尝试
- 这意味着：
  - 小对象的“拿一个 region”有跨 shard 借用能力
  - 但 large object / multi-region 请求没有
- 所以即使全堆别的 shard 还很空，只要当前 `homeShard` 找不到足够长的连续 run，这次 large alloc 仍然会失败。
- 这正好和现场日志一致：global `freeUnits` 很高，某些 shard 还有海量 `free/hotFree/coldFree`，但当前 large alloc 仍然报 OOM。

- **根因 2B：同一条路径里还存在一个“跨 chunk 连续 run”搜索盲点。**
- `FindFreeRunInViewLocked()` 会用 `chunkSummaries_[chunkIdx].max` 做跳跃优化。
- 这个 `max` 只描述“单个 64-region summary chunk 内的最长空闲 run”，不能完整代表“跨多个 chunk 拼起来的连续 run”。
- 因此，对很大的连续 run 请求，旧逻辑可能会因为单 chunk 的 `max < num` 就把候选 chunk 整段跳过去，漏掉实际上存在的跨 chunk fit。
- 这个盲点不是这次 `8388624 B` OOM 的唯一必需条件，但它确实是同一路径上的算法漏洞，后续大 run 场景会踩中，所以一起修掉了。

### 修复方式

- **修复 2A：为 `num > 1` 增加跨 shard 的连续 run donor-steal。**
- 新增 `TryStealContiguousRegionRunLocked()`，并在 `TryTakeRegions()` 中于 `num > 1` 时调用。
- 它的策略是：
  1. 扫描 donor shard
  2. 找出能提供满足请求的连续 free run 的 donor
  3. 优先选择 free run 更大、free region 更多的 donor
  4. 直接在 donor 的 owned segment 上 mark allocate，并推进 donor 的 cursor
- 这样 large object 的多 region 请求就不再被困死在本地 shard。

- **修复 2B：限制 chunk-summary skip 的使用范围。**
- `FindFreeRunInViewLocked()` 现在只有在 `num <= kChunkBits` 时才使用 chunk-summary skip。
- 对于更大的请求，直接退回到更保守但正确的候选扫描 + `PreciseFitInViewFromLocked()` 校验。
- 这保证了“跨多个 chunk 才形成的连续 free span”不会被错误跳过。

- **附带回归测试：**
  - `TestContiguousRunStealsFromDonorOwnedSegment()`
  - `TestLargeRunLargerThanChunkFitsInSingleFreeSegment()`
- 前者覆盖“多 region 请求要能从 donor shard 拿连续 run”。
- 后者覆盖“单个 free segment 内存在跨 chunk 的大连续 run，不应被搜索逻辑漏掉”。

### 为什么修复后能避免“明明 free 很多却 OOM”

- 修复前，large alloc 的回退链路大致是：
  - 本地 shard 找不到
  - `num > 1` 又不能 donor-steal
  - 于是只能 grow；grow 再失败就直接抛 OOM
- 这会把“局部找不到”误判成“全局没空间”。

- 修复后，回退链路变成：
  - 本地 shard 找
  - 本地找不到时，`num > 1` 也能去 donor shard 借连续 run
  - donor 仍不满足时才继续 grow
- 这样 allocator 的决策终于和 GC 日志里的全局 free 状态一致，不会再出现“明明 free 很多却直接 OOM”的假象。

- 同时，对很大的连续 run，请求不会因为 chunk-summary 的局部视图而漏掉合法 fit，避免同一路径再次出现搜索型假失败。

## 代码改动摘要

- `src/Heap/MAllocator/MSFreeManager.cpp`
  - 修正 `GrowOneSegmentLocked()` / `GetSegmentFreeSnapshot()` / `ReleaseCompletelyFreeSegment()` 的锁交互
  - 为 `num > 1` 增加 `TryStealContiguousRegionRunLocked()`
  - 修正 `FindFreeRunInViewLocked()` 的 chunk-summary skip 条件
- `src/Heap/MAllocator/MSFreeManager.h`
  - 增加 `TryStealContiguousRegionRunLocked()` 声明
- `src/Heap/MAllocator/MSRegionManager.cpp`
  - `MaybeNotifyShrink()` 改为纯唤醒 helper，不在 mutator 热路径做重型 shrink probe
- `test/MSFreeManagerLargeStealTest.cpp`
  - 增加 multi-region donor-steal 与 large-run 搜索回归测试
- 额外收尾：
  - 移除了 `TakeRegions` 的诊断统计打印，避免验证结果里的 `stderr` 混入热路径统计噪音

## 验证

### 白盒验证

```bash
clang++ -std=c++17 -pthread -I. -Isrc test/MSFreeManagerLargeStealTest.cpp \
  -o /tmp/MSFreeManagerLargeStealTest && /tmp/MSFreeManagerLargeStealTest
```

- 结果：通过

### case3 端到端验证

1. 重编 MSGC runtime

```bash
python3 build.py build --gc-mode marksweep -t release -v 0.0.1
```

2. 替换到 `dev260619` 并重签

```bash
cp output/temp/lib/aarch64_Release/libcangjie-runtime.dylib \
  /Users/lipeisheng/code/cangjie/cangjie-pkg/self/dev260619/cangjie/runtime/lib/darwin_aarch64_cjnative/libcangjie-runtime.dylib
codesign -f -s - \
  /Users/lipeisheng/code/cangjie/cangjie-pkg/self/dev260619/cangjie/runtime/lib/darwin_aarch64_cjnative/libcangjie-runtime.dylib
```

3. 用同一套 SDK 重新编译 `case3`

```bash
env -i HOME=$HOME TERM=$TERM PATH=/usr/bin:/bin:/usr/sbin:/sbin:/opt/homebrew/bin \
  bash -c '
    source /Users/lipeisheng/code/cangjie/cangjie-pkg/self/dev260619/cangjie/envsetup.sh >/dev/null 2>&1
    cd MSGC_RESULT/Result/case3_half_alive
    cjc benchmark.cj -O1 -o result/verify_20260621_case3_dev260619_msgc_large_run_fix_clean/bench_msgc_o1
  '
```

4. 运行 benchmark

```bash
env -i HOME=$HOME TERM=$TERM PATH=/usr/bin:/bin:/usr/sbin:/sbin:/opt/homebrew/bin \
  bash -c '
    source /Users/lipeisheng/code/cangjie/cangjie-pkg/self/dev260619/cangjie/envsetup.sh >/dev/null 2>&1
    cd MSGC_RESULT/Result/case3_half_alive/result/verify_20260621_case3_dev260619_msgc_large_run_fix_clean
    MRT_REPORT="$PWD/gc_msgc_manual.log" \
      cjProcessorNum=6 cjHeapSize=2gb cjHeapGrowth=1 cjScavengeGrowth=2 \
      ./bench_msgc_o1 > stdout.txt 2> stderr.txt
  '
```

5. 结果

- `stdout.txt` 完整通过：
  - `PHASE1 Done`
  - `PHASE2 Round 1..10`
  - `PHASE3 Size check PASS`
  - `PHASE3 Checksum PASS`
  - `[BENCH] SUMMARY`
- `exit_code.txt = 0`
- `stderr.txt` 为空
- 没有再出现：
  - early stall
  - `Large allocation cannot allocate memory ...`
  - `SIGSEGV` / `SIGABRT` 噪音
- 最终 wall time 约 `38119 ms`

### 补充：grow + background shrink 长时间 stress 验证

- 时间：2026-06-21
- 结果目录：
  - `MSGC_RESULT/Result/case3_half_alive/result/stress_20260621_case3_dev260619_msgc_grow_shrink`
- 口径：
  - 重新用 `dev260619` SDK 编译 `case3`
  - 干净 `env -i` 环境连续执行 8 轮
  - 每轮参数保持：
    - `cjProcessorNum=6`
    - `cjHeapSize=2gb`
    - `cjHeapGrowth=1`
    - `cjScavengeGrowth=2`
  - 每轮保存 `stdout.txt` / `stderr.txt` / `gc.log` / `exit_code.txt`
  - 另生成汇总表：`stress_summary.tsv`
- 8 轮汇总结果：
  - `run1`: `exit=0`, `39.916s`, `gc_cycles=1554`
  - `run2`: `exit=0`, `39.893s`, `gc_cycles=1548`
  - `run3`: `exit=0`, `40.302s`, `gc_cycles=1560`
  - `run4`: `exit=0`, `40.488s`, `gc_cycles=1557`
  - `run5`: `exit=0`, `40.594s`, `gc_cycles=1557`
  - `run6`: `exit=0`, `40.625s`, `gc_cycles=1569`
  - `run7`: `exit=0`, `40.527s`, `gc_cycles=1558`
  - `run8`: `exit=0`, `40.514s`, `gc_cycles=1555`
- 统一判据检查：
  - 8/8 轮都完整跑到 `PHASE1 Done`、`PHASE2 Round 10/10`、`PHASE3 PASS`、`[BENCH] SUMMARY`
  - 8/8 轮 `stderr.txt` 为空
  - 8/8 轮未出现：
    - `Large allocation cannot allocate memory`
    - `LARGE ALLOC FAILED`
    - `SIGSEGV`
    - `SIGABRT`
    - timeout / 卡住
- 结论：
  - 以 `case3` 这条“大对象 grow + background shrink 反复交织”的压力路径来看，当前修复后的 MSGC runtime 已经通过了连续多轮 stress 验证，没有复现之前的卡住和 false OOM。

## 结论

- 这次 `case3` 实际上暴露的是两层独立问题：
  1. **grow/shrink 路径的锁交互错误**，导致大对象增长阶段卡住
  2. **multi-region large alloc 的回退链路不完整**，把“局部找不到连续 run”误判成“全局 OOM”
- 第一层修掉后，benchmark 不再早期卡住；第二层修掉后，large alloc 能正确利用 donor shard / 合法连续 run，端到端 benchmark 恢复正常。
- 当前 `verify_20260621_case3_dev260619_msgc_large_run_fix_clean` 已经给出完整通过结果，可以作为这次修复的最终验证基线。
