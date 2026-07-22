# case27 当前验证结论归档（2026-07-02）

## 本轮结论要点

在 `runtime = a4cb33f6e1f16cf2988d49d1495f4416e7691829` 固定不变的前提下，基于 `case27_http_complex_json` 的 `c1000 + 100K` 对照验证，当前可以收敛成下面 3 条：

1. **只留 `CJ_MCC_WriteRefFieldFast_MSGC` / `CJ_MCC_WriteStaticRefFast_MSGC` 不行**
2. **只留 `CJ_MCC_WriteStructFieldFast_MSGC` / `CJ_MCC_WriteStaticStructFast_MSGC` 也不行**
3. **必须把这 4 个 MSGC 写屏障 fastpath 全部从 `emitMccWriteBarrierFastPath` 的分发名单里移除，case27 才能执行到结束**

这里的“执行到结束”当前应严格理解为：

- `bench_exit=0`
- `watchdog=normal`
- 未复现 crash / hang / timeout / dylib 加载错误

**不能**把它直接表述成“业务结果完全正常”，因为“4 个都移除”的这轮虽然进程级执行成功，但请求级仍有 `fail`。

## 验证前提

- `runtime`：commit `a4cb33f6e1f16cf2988d49d1495f4416e7691829`
  - 用户口径短写：`a4cb33f6e1f16cf2988d`
- 用例：`case27_http_complex_json`
- 配置：`c1000 + 100K`
- 平台：`macOS aarch64`
- 每轮都使用 fresh SDK 重新编译 `benchmark.cj`，并先做 dylib 加载探针

## 三组对照结果

### 1. 只留 `WriteRefFast` 两个，不行

对应配置：

- 保留：
  - `CJ_MCC_WriteRefFieldFast_MSGC`
  - `CJ_MCC_WriteStaticRefFast_MSGC`
- 移除：
  - `CJ_MCC_WriteStructFieldFast_MSGC`
  - `CJ_MCC_WriteStaticStructFast_MSGC`

代表结果：

- 目录：`20260702_154349_compiler_output_msgc_ref_on_struct_off_c1000_p100K`
- 结果：
  - `bench_exit=138`
  - `watchdog=normal`
  - MSGC runtime 加载正确
  - 进程崩溃，不能通过 case27 验证

结论：

- **只保留 `WriteRefFieldFast_MSGC` / `WriteStaticRefFast_MSGC` 仍然不行**

### 2. 只留 `WriteStructFast` 两个，也不行

对应配置：

- 保留：
  - `CJ_MCC_WriteStructFieldFast_MSGC`
  - `CJ_MCC_WriteStaticStructFast_MSGC`
- 移除：
  - `CJ_MCC_WriteRefFieldFast_MSGC`
  - `CJ_MCC_WriteStaticRefFast_MSGC`

代表结果：

- 目录：`20260702_152000_compiler_output_disable_writeref_staticref_fast_c1000_p100K_rerun1`
- 结果：
  - `bench_exit=139`
  - `watchdog=normal`
  - 实际加载的是 `compiler/output` 下的 fresh SDK
  - 进程崩溃，不能通过 case27 验证

结论：

- **只保留 `WriteStructFieldFast_MSGC` / `WriteStaticStructFast_MSGC` 也不行**

### 3. 4 个都移除，case27 才能跑完

对应配置：

- 从 `emitMccWriteBarrierFastPath` 分发名单里全部移除：
  - `CJ_MCC_WriteRefFieldFast_MSGC`
  - `CJ_MCC_WriteStructFieldFast_MSGC`
  - `CJ_MCC_WriteStaticRefFast_MSGC`
  - `CJ_MCC_WriteStaticStructFast_MSGC`

代表结果：

- 目录：`20260702_160417_compiler_output_msgc_all_fastpath_off_c1000_p100K`
- 结果：
  - `bench_exit=0`
  - `watchdog=normal`
  - `runtime_load.txt` 确认加载的是 `compiler/output` 下的 runtime/stdx
  - `gc.log` 头部确认是 `[MS-GC]`
  - `[RESULT] total=17694 ok=17027 fail=667`

结论：

- **只有把这 4 个 MSGC 写屏障 fastpath 全部移除，case27 才能稳定执行到结束**
- 但当前这个“成功”仍然只是**进程级成功**
- **请求级仍有 fail**，所以不能表述成“业务完全无异常”

## 可直接复用的口径

> 当前在 `runtime = a4cb33f6e1f16cf2988d` 固定不变的前提下，case27 `c1000 + 100K` 的对照验证表明：只保留 `CJ_MCC_WriteRefFieldFast_MSGC` / `CJ_MCC_WriteStaticRefFast_MSGC` 不行，只保留 `CJ_MCC_WriteStructFieldFast_MSGC` / `CJ_MCC_WriteStaticStructFast_MSGC` 也不行；必须把这 4 个 MSGC 写屏障 fastpath 全部从 AArch64 `emitMccWriteBarrierFastPath` 分发名单中移除，case27 才能执行到结束（`bench_exit=0`）。但这一结论当前仍限于“进程级执行成功”，请求级结果仍有 `fail`，暂不能表述成业务完全正常。

## 2026-07-02 运行时工作区临时加桩与结果更新

### runtime / compiler 侧临时加了什么

1. 在 `runtime/src/CompilerCalls.h/.cpp` 增加了：
   - `MCC_MSGC_DebugStoreOnlyPhase(uint32_t inlineTlsPhase)`
2. 在 AArch64 `StoreOnly` inline 快路径里增加了一次 debug 慢调用：
   - `str x0, [x1]`
   - `mov w0, w9`
   - `bl _MCC_MSGC_DebugStoreOnlyPhase`
3. 另外曾临时在 runtime 中加过 3 个 helper：
   - `MCC_MSGC_EnqueueOldRefFast`
   - `MCC_MSGC_EnqueueStructOldRefsFast`
   - `MCC_MSGC_EnqueueStaticStructOldRefsFast`

补充说明：

- 上面第 3 组 `MCC_MSGC_Enqueue*` helper 只是为“后续若要做 old-edge 手动入队试验”预留 runtime 接口。
- 它们在当前 case27 `benchmark` 二进制中**没有被实际引用**，后续已从 runtime 工作区移除并重新编译安装。

### 这些临时改动的作用是什么

- `MCC_MSGC_DebugStoreOnlyPhase` 的目的不是修复，而是验证一条特定怀疑路线：
  - 当 inline `StoreOnly` 走“只 store、不入队”分支时，inline 看到的 `TLS encodedGCPhase` 是否可能已经滞后于 runtime 重新读取到的 TLS phase / 全局 `GetGCPhase()`
- 只有当同时满足下面两个条件时，才打印一条 `[MSGC][StoreOnlyPhaseMismatch]`：
  - `inlineTlsPhase < 20` 或 `inlineTlsPhase > 22`
  - `globalGCPhase == GC_PHASE_MS_CONCURRENT_MARK`
- 样本数限制为最多 `64` 条，避免刷爆日志。

需要特别强调：

- 这不是“零扰动诊断”。
- 因为当前 AArch64 `StoreOnly` 快路径每次命中后，都会额外执行一次 `bl _MCC_MSGC_DebugStoreOnlyPhase`，所以它会改变 fastpath 时序。
- 因此“加桩后某一轮没复现 / 退出码为 0”**不能直接当成修复证据**。

### 实际结果

#### A. 带 `StoreOnlyPhase` probe 的短回路验证

- 目录：`20260702_165914_storeonly_inlinew9_probe_c1000_p100K`
- 配置：`case27 / 100K / c1000 / 12s / heap=4gb`
- 结果：
  - `run1`: `rc=0`
  - `run2`: `rc=0`
  - `run3`: `rc=0`
  - `ConcurrentMark` 次数：`48 / 46 / 43`
  - `StoreOnlyPhaseMismatch` 样本：`0 / 0 / 0`

结论：

- probe 链路本身已经生效，但这轮**没有抓到**“inline 看到非 mark、全局其实是 concurrent mark”的不一致样本。

#### B. 保留 probe，且 runtime 中仍带着 3 个 `MCC_MSGC_Enqueue*` helper 时的 60s x 5 次验证

- 目录：`20260702_171030_case27_c1000_p100K_60s_heap4gb_5runs`
- 配置：`case27 / 100K / c1000 / 60s / heap=4gb`
- 结果：
  - `run1`: `rc=0`，`fail=1067`
  - `run2`: `rc=0`，`fail=807`
  - `run3`: `rc=0`，`fail=1502`
  - `run4`: `rc=139`，发生 crash
  - `run5`: `rc=0`，`fail=631`
  - 5 次 `StoreOnlyPhaseMismatch` 样本都为 `0`

`run4` crash 指纹：

- 栈顶位于：
  - `stdx.log.Logger.enabled`
  - `stdx.net.http.ConnNode.close`
- fault addr：`0xffffffffffffffff`

结论：

- 在“probe 已生效”的前提下，问题依然可能复现为 crash。
- 因而不能把“probe build 下某些轮次 exit=0”误解成问题已经解决。

#### C. 移除 3 个 `MCC_MSGC_Enqueue*` helper、只保留 `MCC_MSGC_DebugStoreOnlyPhase` 后的 60s x 3 次验证

- 目录：`20260702_173212_case27_c1000_p100K_60s_heap4gb_3runs_runtime_no_enqueue`
- 配置：`case27 / 100K / c1000 / 60s / heap=4gb`
- 验证前确认：
  - 新编译的 `benchmark` 仅引用 `_MCC_MSGC_DebugStoreOnlyPhase`
  - 不再引用 3 个 `MCC_MSGC_Enqueue*`
  - `dyld probe` 确认实际加载的是 `compiler/output/runtime/lib/darwin_aarch64_cjnative/libcangjie-runtime.dylib`
- 结果：
  - `run1`: `rc=0`，`fail=1053`，`ConcurrentMark=145`
  - `run2`: `rc=0`，`fail=2561`，`ConcurrentMark=115`
  - `run3`: `rc=0`，`fail=1395`，`ConcurrentMark=136`
  - 3 次 `StoreOnlyPhaseMismatch` 样本都为 `0`

结论：

- 这 3 个 `MCC_MSGC_Enqueue*` helper 本身并不在当前 case27 的实际执行路径上。
- 把它们从 runtime 中移除后，进程仍可执行到结束，但请求级 `fail` 依旧存在。
- 因此它们不是当前 case27 现象的决定因素，也不能作为修复点。

### 当前能收敛出的判断

1. `MCC_MSGC_DebugStoreOnlyPhase` 这条 runtime / compiler 联合诊断链路已经确认生效。
2. 截至当前，它**没有抓到**“inline 看到 `<20` 或 `>22`，但全局其实是 `ConcurrentMark`”的样本，因此还**不能**据此确认“TLS phase 滞后”这条路线。
3. 那 3 个 `MCC_MSGC_Enqueue*` helper 没有进入当前 case27 实际执行路径；移除前后，请求级 `fail` 现象都存在，所以它们不是根因。
4. “4 个写屏障 fastpath 全移除”目前依然只是**进程级**最稳定配置；请求级 `fail` 仍然存在，根因还没有闭环。

## 2026-07-02 A 方向（heap-range / `heapCurrentEnd` 发布时序）无扰动验证更新

### 这次实际改了什么

本轮按“A 方向”做了最小修正，并把前面的诊断扰动项全部移除：

1. runtime 侧把 `Heap::heapCurrentEnd` 改成原子发布/读取：
   - 扩堆时 `store(..., release)`
   - `IsHeapAddress()` 判堆范围时 `load(..., acquire)`
   - `CangjieRuntime::SetGCThreshold()` 也改成 `acquire` 读取
2. AArch64 `emitMccWriteBarrierFastPath` 侧：
   - fastpath 里的 `heapCurrentEnd` 读取改成 acquire load
3. 移除扰动项：
   - 删掉 `MSGCDebugStoreOnlyPhaseHelperStr`
   - 删掉 `MCC_MSGC_DebugStoreOnlyPhase`
   - 当前验证使用的是**不带 debug 慢调用**的版本

这轮验证的目标是只验证 “heap-range check 用非 acquire 读 `heapCurrentEnd`” 这条路线，避免 probe 改变 fastpath 时序。

### 验证结果

#### A1. 首轮尝试无效，属于验证环境问题

- 目录：`20260702_190015_case27_c1000_p100K_60s_heap4gb_3runs_candidateA_no_probe`
- 问题：
  - 运行时漏加了 stdx 的 `DYLD_LIBRARY_PATH`
  - `dyld_probe.stderr` 报 `Library not loaded: @rpath/libstdx.net.http.dylib`

结论：

- 这轮应判定为**环境错误**，不能算产品失败，也不能用于判断 A 方向是否生效。

#### A2. 修正 stdx 路径后，真实验证仍失败

- 目录：`20260702_190710_case27_c1000_p100K_60s_heap4gb_3runs_candidateA_no_probe_fixed_stdxpath`
- 配置：`case27 / 100K / c1000 / 60s / heap=4gb`
- `build_env.txt` / `runtime_load` 确认使用的是 fresh `compiler/output` SDK
- `nm` 确认不再引用 `MCC_MSGC_DebugStoreOnlyPhase`

代表结果：

- `run1`: `rc=138`
  - `run1.stderr.txt` 顶栈：
    - `stdx.encoding.json::WriteBuffer::appendEscape`
    - `stdx.encoding.json::jsonWriteWithoutFormat`
    - `JsonObject::toString`
    - `benchmark.cj:processRequest`
  - 信号：`SIGUSR1 (Bus error: 10)`
- 同一构建产物直接复跑：
  - `rerun.exitcode=139`
  - `rerun.stderr.txt` 顶栈：
    - `MCC_AcquireRawData`
    - `CJ_MCC_AcquireRawData`
    - `stdx.encoding.json::WriteBuffer::appendEscape`
  - 信号：`SIGSEGV (Segmentation fault: 11)`

补充说明：

- `run_summary.txt` 里虽然显示 `gc_starts=0`，但这是统计脚本 grep 关键字写错导致的误计数。
- 实际 `gc_run1.log` 中可以看到多轮 `=== GC START cycle=... ===`，说明失败不是“没进 GC”。

### 当前结论

1. A 方向修正已经真实落地：
   - runtime 与 AArch64 fastpath 都改成了 `heapCurrentEnd` 的 release/acquire 配对
   - `MSGCDebugStoreOnlyPhase` 相关扰动项已全部移除
2. 在**无 probe 扰动**的前提下，case27 仍然会复现 crash。
3. 因此：
   - **A 方向不是充分修复**
   - “并发扩堆导致 heap-range check 误判非堆、从而漏记 old edge”至多只是候选因素之一，当前不能解释全部现象
4. 这轮新的失败栈已经进一步收敛到：
   - `MCC_AcquireRawData`
   - `CJ_MCC_AcquireRawData`
   - `stdx.encoding.json::WriteBuffer::appendEscape`
5. 后续若继续保留 fastpath 排查，优先级应转向：
   - `MCC_AcquireRawData` / 字符串 raw-data 访问链路
   - 或继续验证 `SATB/log ordering` 这条 B 路线

## 2026-07-02 std / stdx 全量重编安装后的排除项更新

### 这次验证的目标

排除下面这条嫌疑：

- **问题是否来自 SDK 里的 `std` / `stdx` 仍混有旧产物，或 `benchmark` 仍链接到了未完整重编安装的标准库动态库**

### 这次实际做了什么

1. 基于当前 `compiler/output` 这套 SDK，完整重编并安装：
   - `stdlib`
   - `stdx`
2. 落盘时间确认：
   - `libcangjie-std.dylib`: `2026-07-02 19:59:03`
   - `libstdx.net.http.dylib`: `2026-07-02 19:59:02`
   - `libstdx.encoding.json.dylib`: `2026-07-02 19:59:02`
3. 然后在隔离环境里基于这套 fresh SDK 重新编译 `case27/benchmark.cj`，再跑一轮：
   - 配置：`case27 / 100K / c1000 / 60s / heap=4gb`
   - 结果目录：`20260702_200122_case27_c1000_p100K_60s_heap4gb_1run_rebuilt_std_stdx`

### 实际结果

- 编译成功，`benchmark` 使用的是这套 fresh SDK 里的 runtime / stdx
- warmup 正常
- 进入正式压测后约 4s 崩溃退出：
  - `rc=138`
  - `result_lines=0`
  - `gc_starts=0`
- 崩溃签名仍与前面主线现象一致：
  - 信号：`SIGUSR1 (Bus error: 10)`
  - 顶栈：
    - `std/core/array_common.cj::__CNat14CPointerHandleIG_E6<init>HPG_RNat5ArrayIG_E`
    - `stdx.encoding.json::WriteBuffer::appendEscape`
    - `stdx.encoding.json::jsonWriteWithoutFormat`
    - `JsonObject::toString`
    - `benchmark.cj:processRequest`

### 结论

1. `std` / `stdx` 已经做过**完整重编 + 安装 + 基于该 SDK 重新编译用例**的闭环验证。
2. 在这个前提下，case27 仍然复现与之前同类的 crash。
3. 因此可以排除：
   - **“旧 `std` / `stdx` 产物残留”是当前问题根因**
   - **“仅因为没有完整重编安装 `std` / `stdx` 才导致现象成立”**
4. 当前问题仍应继续收敛在：
   - 写屏障 fastpath 本身
   - `MCC_AcquireRawData` / 字符串 raw-data 访问链路
   - 或 `SATB/log ordering` 等并发时序问题

## 2026-07-02 联合实验更新：`2GB per-P` + `disable shrink` + `wbBuf 800/500`

### 这次实验想验证什么

同时验证两类怀疑：

1. **heap grow / shrink churn 是否是关键触发条件**
   - 做法：把 MSGC 每个 P 的 shard segment 直接提到 `2GB`
   - 并关闭 background shrink，尽量避免运行过程中的 committed heap 边界频繁变化
2. **wbBuf 接近满载时的 fastpath / slowpath 切换时机是否是关键因素**
   - 做法：
     - AArch64 inline fastpath：`count >= 800` 就提前走 slow
     - runtime slow path：`count > 500` 就先 flush

### 这次实际改动

源码侧临时改动共 4 处：

1. `runtime/src/Heap/MAllocator/MSShardRouting.h`
   - `kMSShardSegmentBytes: 64MB -> 2GB`
2. `runtime/src/Heap/MCollector/BackgroundProcessor.cpp`
   - `IsBackgroundShrinkEnabled()` 直接返回 `false`
3. `runtime/src/Heap/MBarrier/MSMarkBarrier.cpp`
   - `EnqueueRefFast()` 改成 `count > 500` 就先 flush
4. `llvm/lib/Target/AArch64/AArch64AsmPrinter.cpp`
   - MSGC 写屏障 inline fastpath 的 wbBuf 阈值从 `1024` 提前到 `800`

补充说明：

- 这轮使用的是 fresh rebuild SDK：
  - compiler：`compiler/output`
  - runtime：本地修改后重新 build/install 到 `compiler/output`
  - std / stdx：也都完整重编安装
- 用例重新基于这套 SDK 编译。

### 验证方式

- 用例：`case27_http_complex_json`
- 配置：`100K / c1000 / 60s / heap=4gb`
- 结果目录：
  - `20260702_223220_heap2g_perP_noshrink_wb800_500_case27_100K_c1000_60s`
- 运行前 `DYLD_PRINT_LIBRARIES` 已确认实际加载的是：
  - `compiler/output/runtime/lib/darwin_aarch64_cjnative/libcangjie-runtime.dylib`
  - `compiler/output/darwin_aarch64_cjnative/dynamic/stdx/libstdx.net.http.dylib`
  - `compiler/output/darwin_aarch64_cjnative/dynamic/stdx/libstdx.encoding.json.dylib`

### 实际结果

3 轮都失败，而且失败更早、更稳定：

- `run1`
  - `rc=138`
  - 大约在启动正式压测后 `~10s` 崩溃
  - 顶栈：
    - `std/core/array_common.cj::__CNat14CPointerHandleIG_E6<init>HPG_RNat5ArrayIG_E`
    - `stdx.encoding.json::WriteBuffer::appendEscape`
    - `stdx.encoding.json::jsonWriteWithoutFormat`
    - `JsonObject::toString`
- `run2`
  - `rc=139`
  - 大约在 `~7s` 崩溃
  - 顶栈：
    - `MCC_AcquireRawData`
    - `CJ_MCC_AcquireRawData`
    - `stdx.encoding.json::WriteBuffer::appendEscape`
- `run3`
  - `rc=139`
  - 大约在 `~4s` 崩溃
  - 顶栈：
    - `MCC_AcquireRawData`
    - `CJ_MCC_AcquireRawData`
    - `stdx.encoding.json::WriteBuffer::appendEscape`

共同特征：

- 3 轮都收敛到 `stdx.encoding.json` 的 JSON 序列化路径
- 崩点集中在：
  - `WriteBuffer::appendEscape`
  - `jsonWriteWithoutFormat`
  - `JsonObject::toString`
- 其中 2 轮已直接落到 `MCC_AcquireRawData` / `CJ_MCC_AcquireRawData`

### 结论

1. **“每个 P 预留更大 heap + 关闭 shrink” 没有解决问题**
   - 因而 `heap grow / shrink churn` 不是当前 case27 crash 的充分根因
2. **“inline 提前 slow、runtime 提前 flush wbBuf” 也没有缓解问题**
   - 单纯降低 wbBuf 水位或提前 flush 不是当前问题的修复方向
3. 这轮组合实验不仅没修复，反而让失败更早、更稳定，说明：
   - 根因更可能仍在写屏障 fastpath 正确性本身
   - 或在 `MCC_AcquireRawData` / 字符串 raw-data 访问链路
   - 而不是单纯的 heap 边界抖动或 wbBuf 满载时机问题

## 2026-07-03 全局 phase 镜像 acquire 读取验证更新

### 这次实验想验证什么

验证下面这条具体怀疑是否成立：

- **AArch64 inline write-barrier fastpath 读到的 `TLS encodedGCPhase` 可能滞后，于是把本应处于 mark 期的 barrier 误判成 `StoreOnly`**

这次不是去改 gate 语义本身，而是只替换 phase 的读取来源：

- 不再从 TLS 读 `encodedGCPhase`
- 改成读一个由 runtime 在 `SetGCPhase()` 时同步发布的**全局 phase 镜像**
- 发布/读取使用 `release / acquire`

如果这样改完后 case27 仍然照样失败，就可以把“**TLS phase 字节本身读旧**”这条子假设基本排掉。

### 这次实际改了什么

1. runtime 侧新增全局镜像：
   - `g_msgcGlobalGCPhase`
2. `Collector::SetGCPhase()` 中：
   - 原有 `gcPhase.store(..., release)` 保留
   - 同时对 `g_msgcGlobalGCPhase` 做 `store(..., release)`
3. AArch64 `emitMccWriteBarrierFastPath` 中：
   - 原先 phase gate 前的 TLS `LDRB` 读取
   - 改成对 `g_msgcGlobalGCPhase` 的 `LDARB` acquire 字节读
4. gate 判断保持不变：
   - 仍按 `20..22` 视为 MSGC mark 相关阶段
   - 小于 `20` 或大于 `22` 仍走 `StoreOnly`

补充说明：

- 这轮是**真实 rebuild + install + 重新编译 case27**，不是手工替换一两个对象文件。
- compiler、runtime、`stdlib`、`stdx` 都重新编译并安装到了 `compiler/output`。

### 验证方式

- 用例：`case27_http_complex_json`
- 配置：`100K / c1000 / 60s / heap=4gb`
- 结果目录：
  - `20260703_111219_compiler_output_global_phase_mirror_acquire_c1000_p100K_runs3`

验证前确认：

- `DYLD_PRINT_LIBRARIES` 探针退出码：`0`
- 实际加载的是 fresh SDK：
  - `compiler/output/runtime/lib/darwin_aarch64_cjnative/libcangjie-runtime.dylib`
  - `compiler/output/darwin_aarch64_cjnative/dynamic/stdx/libstdx.net.http.dylib`
  - `compiler/output/darwin_aarch64_cjnative/dynamic/stdx/libstdx.encoding.json.dylib`

### 实际结果

3 轮全部失败：

- `run1`
  - `exit=138`
  - 大约在 `~6s` 左右崩溃
  - 顶栈：
    - `std/core/array_common.cj::__CNat14CPointerHandleIG_E6<init>HPG_RNat5ArrayIG_E`
    - `stdx.encoding.json::WriteBuffer::appendEscape`
    - `stdx.encoding.json::jsonWriteWithoutFormat`
    - `JsonObject::toString`
- `run2`
  - `exit=139`
  - 大约在 `~20s` 左右崩溃
  - 顶栈：
    - `MCC_AcquireRawData`
    - `CJ_MCC_AcquireRawData`
    - `stdx.encoding.json::WriteBuffer::appendEscape`
- `run3`
  - `exit=139`
  - 大约在 `~12s` 左右崩溃
  - 顶栈：
    - `MCC_AcquireRawData`
    - `CJ_MCC_AcquireRawData`
    - `stdx.encoding.json::WriteBuffer::appendEscape`

共同特征：

- 失败指纹与前面主线现象一致，没有出现新的“phase-only”型特征
- 仍然持续收敛到：
  - `stdx.encoding.json::WriteBuffer::appendEscape`
  - `jsonWriteWithoutFormat`
  - `JsonObject::toString`
  - `MCC_AcquireRawData` / `CJ_MCC_AcquireRawData`

### 结论

1. 这次实验已经把 inline barrier 的 phase 读取来源从 **TLS phase** 换成了 **全局 release/acquire phase 镜像**。
2. 在这个前提下，case27 仍然稳定复现 crash。
3. 因此可以**基本排除**下面这条子假设：
   - **“根因就是 inline fastpath 读到滞后的 TLS `encodedGCPhase` 字节”**
4. 更准确地说，当前被排除的是：
   - **phase 载体是 TLS，所以读旧**
   - **只要把 phase 改成全局 acquire 读就能修复**
5. 这并不等于“所有 phase 相关问题都排除”：
   - 如果后续还要继续沿 phase 路线深挖，更应该怀疑
   - `20..22` gate 语义本身是否完整
   - 或 phase 与其它并发状态发布之间是否仍有不一致
   - 而不是单纯怀疑 TLS phase 字节滞后
6. 当前主线仍应继续收敛在：
   - 写屏障 fastpath 正确性本身
   - `MCC_AcquireRawData` / 字符串 raw-data 访问链路
   - 或其它与 SATB / old-edge 正确性相关、但不等价于“TLS phase 读旧”的问题
