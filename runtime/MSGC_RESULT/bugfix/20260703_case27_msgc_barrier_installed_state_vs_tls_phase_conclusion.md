# case27 MSGC barrier-installed 状态与 TLS phase 判定结论

## 基本信息

- 标题：AArch64 MSGC inline write barrier 应看 barrier-installed 状态，而不是仅看 TLS phase 数值窗口
- 日期：2026-07-03
- 平台：macOS aarch64
- 验证用例：`MSGC_RESULT/Result/case27_http_complex_json`
- 验证参数：`payload=100K`，`concurrency=1000`，`duration=60000ms`，`cjHeapSize=4gb`

## 结论摘要

- 当前实验结果强烈支持：`case27` 这轮进程级崩溃的关键问题，不是“phase 读取一定要改成全局”本身，而是 **inline write barrier 的 gate 语义与 runtime 实际安装的 barrier 语义不一致**。
- 把 AArch64 inline write barrier 的判定依据从“`phase in [20, 22]`”改成“runtime 当前是否实际安装了 `msMarkBarrier`”之后，`case27` 由此前多次 `138/139` 崩溃变成 `3/3 exit=0` 且都跑到 `[DONE]`。
- 因此，对 write barrier 来说，最终判据更适合是一个 **barrier-installed state**，而不是 mutator TLS 里的 `encodedGCPhase` 数值窗口。
- 但 2026-07-03 下午继续把这个语义“下沉成 mutator TLS `gcFastState.bit0`”之后，`case27` 又在 warmup 期稳定复现 `139(SIGSEGV)`。
- 这说明：**语义本身要看 barrier-installed state 没错，但如果这个状态重新退化成每线程 TLS 影子副本，仍可能把 stale publish / per-mutator lag 风险重新带回来。**

## 下午跟进实验：TLS `gcFastState.bit0`

### 改动摘要

- runtime：
  - 复用 `ThreadLocalData::gcFastState`
  - `bit0 = WB enabled`
  - `bit1 = assist 预留`
  - 在 `MutatorManager::TransitionAllMutatorsToGCPhase()` / `StartLightSync()` 中，把 runtime 当前 barrier-installed 状态发布到各 mutator TLS
- compiler：
  - AArch64 inline write barrier 不再读全局 `g_msgcWBEnabled`
  - 改成直接读 TLS `gcFastState.bit0`

### 验证方式

- 重新编译并安装：
  - LLVM / runtime / std / stdx
- 验证目录：
  - `MSGC_RESULT/Result/case27_http_complex_json/result/20260703_135151_compiler_output_sdk_tls_gcfaststate_wbbit_c1000_p100K_validate_detached/`
- 验证参数：
  - `payload=100K`
  - `concurrency=1000`
  - `duration=60000ms`
  - `cjHeapSize=4gb`
- 同时跑了一个 dyld/probe 小回路：
  - `payload=1K`
  - `concurrency=1`
  - `duration=1ms`

### 结果

- `PROBE_RC=139`
- `RUN_RC=139`
- probe 和正式跑都不是超时，而是都很快在 warmup 阶段崩溃
- `stderr.txt` 栈顶：
  - `SIGSEGV`
  - `std/convert/parsable.cj::__CNai12parseCompInt...`
  - 上层调用链在 `stdx.encoding.json::parseJsonNumber`

### 当前解释

- “看真实 barrier-installed 语义”这条方向仍然成立。
- 但把这个真状态变成 **每个 mutator 一份 TLS shadow copy** 后，compiler inline 读到的又不是全局唯一 truth source，而是“某个线程最近一次被发布到的值”。
- 因而当前更像是：
  - 全局 truth source 方案证明了“phase 数值窗口不是正确 gate”
  - TLS bit 方案又证明了“如果 publish 链路本身可能滞后，单纯把 truth source 复制到 TLS 仍不够”
- 也就是说，剩下要继续盯的重点已经从“判据语义”收敛到：
  - **per-mutator barrier state publish 是否存在滞后/漏发**
  - **新建 mutator / runtime mutator / 线程切入切出路径是否拿到了旧 bit**
  - **某些 barrier 执行线程是否在切换窗口内没有及时观察到新的 TLS bit**

## 本次改动

### 1. runtime 侧新增全局 barrier 状态字节

- 在 `src/Heap/Collector/Collector.cpp` 中新增：
  - `extern "C" MRT_EXPORT std::atomic<uint8_t> g_msgcWBEnabled{ 0 };`
- 含义：
  - `1`：当前安装的是 `msMarkBarrier`
  - `0`：当前安装的是 `msIdleBarrier`

### 2. runtime 在 `InstallBarrier()` 中发布真实 barrier 状态

- `src/Heap/Heap.cpp::HeapImpl::InstallBarrier(const GCPhase phase)`
- 行为：
  - `GC_PHASE_MS_MARK_SETUP`
  - `GC_PHASE_MS_CONCURRENT_MARK`
  - `GC_PHASE_MS_MARK_TERMINATION`
    - 安装 `msMarkBarrier`
    - `g_msgcWBEnabled.store(1, release)`
  - `GC_PHASE_IDLE`
  - `GC_PHASE_FINISH`
  - `GC_PHASE_RECLAIM_SATB_NODE`
  - `GC_PHASE_MS_SWEEP`
    - 安装 `msIdleBarrier`
    - `g_msgcWBEnabled.store(0, release)`

### 3. compiler 侧 inline write barrier 改读 barrier-installed 状态

- `llvm/lib/Target/AArch64/AArch64AsmPrinter.cpp`
- 原逻辑：
  - 读 TLS `encodedGCPhase`
  - `phase == 0` -> `Slow`
  - `phase < 20 || phase > 22` -> `StoreOnly`
  - `phase in {20,21,22}` -> old-edge log fastpath
- 新逻辑：
  - `LDARB` 读全局 `g_msgcWBEnabled`
  - `0` -> `StoreOnly`
  - `1` -> old-edge log fastpath
  - 其他异常值 -> `Slow`

## 验证结果

### 对照组 1：只把 phase 改成全局镜像，但仍按 `20..22` 判定

- 结果目录：
  - `MSGC_RESULT/Result/case27_http_complex_json/result/20260703_111219_compiler_output_global_phase_mirror_acquire_c1000_p100K_runs3/`
- 结果：
  - `run01 exit=138`
  - `run02 exit=139`
  - `run03 exit=139`
- 结论：
  - 仅把“读 TLS phase”换成“读全局 phase 镜像”并不能解决问题。

### 对照组 2：改成读 barrier-installed 全局字节

- 结果目录：
  - `MSGC_RESULT/Result/case27_http_complex_json/result/20260703_114128_compiler_output_wbenabled_gate_c1000_p100K_runs3/`
- 结果：
  - `run01 exit=0`，有 `[DONE]`
  - `run02 exit=0`，有 `[DONE]`
  - `run03 exit=0`，有 `[DONE]`
- 请求级统计：
  - `run01 fail=711`
  - `run02 fail=1312`
  - `run03 fail=566`
- 结论：
  - **进程级 crash 被压住了**
  - 但 benchmark 的 `fail=` 仍非零，因此当前结论是“解决了进程崩溃路径”，不是“所有业务层异常都已消失”

## 为什么这次不崩了

- 原来的 inline write barrier 看的是“phase 数字像不像 mark phase”。
- 但真正决定语义的是 runtime 当前装的是哪一个 barrier。
- phase 切换边界上，即使 `phase` 已经/尚未落到 `20..22`，也不代表当前 mutator 执行路径上看到的 barrier 语义已经严格同步。
- 当 inline fastpath 的 gate 与 runtime 实际 barrier 安装窗口不一致时，就可能：
  - 在不该记 old edge 时误走 barrier 路径
  - 或在该记 old edge 时误走 `StoreOnly`
  - 进而制造 SATB 漏边或对象图不一致
- 这次改成直接读取“barrier-installed 真状态”后，compiler inline 与 runtime barrier 的语义源头一致，因此这个窗口不一致问题被明显收敛。

## 这个标记能不能放到 TLS

- **能放，但不建议作为最终修复方案。**

### 原因

- `wbEnabled` 表示的是“runtime 当前实际安装了哪种 barrier”，这天然是一个 **全局真状态**。
- 如果把它放到每个 mutator TLS 里，就会变成每线程一份影子副本。
- 那么 runtime 仍然需要在 phase/barrier 切换时把这个状态逐个发布到每个线程。
- 这样会重新引入与当前问题同类的风险：
  - 某个 mutator 的 TLS 副本刷新晚于真实 barrier 安装
  - inline gate 仍可能读到滞后的影子值
- 因此：
  - **TLS 更适合做快速提示值**
  - **全局原子更适合做“当前 barrier 真状态”的最终判据**

### 如果只做实验，TLS 最小改法

- 可以复用 `src/Mutator/ThreadLocal.h` 里的 `reserved0`
- 不需要改变 `ThreadLocalData` 总大小
- 但这只适合做对照实验，不适合作为当前问题的首选收敛方案

## 原来的 phase 在哪里

当前与 phase 相关的状态有三层：

### 1. Collector 全局 phase

- `src/Heap/Collector/Collector.h`
- 字段：
  - `std::atomic<GCPhase> gcPhase`
- 含义：
  - 整个 collector 的全局 GC phase

### 2. Mutator 正式 phase

- `src/Mutator/Mutator.h`
- 字段：
  - `std::atomic<GCPhase> mutatorPhase`
- 含义：
  - 某个 mutator 当前完成握手后的正式 phase

### 3. 编译器 fastpath 直接读的 TLS phase 字节

- `src/Mutator/ThreadLocal.h`
- 字段：
  - `uint8_t encodedGCPhase`
- 写入逻辑：
  - `src/Mutator/Mutator.h::SetThreadLocalGCPhase()`
  - `src/Mutator/ThreadLocal.cpp::SetMutator()`
- 当前编码方式：
  - 直接 `static_cast<uint8_t>(phase)`
- 在 64 位下 offset 为：
  - `78`
- 对应校验：
  - `src/Mutator/ThreadLocal.cpp` 中有 `static_assert(offsetof(ThreadLocalData, encodedGCPhase) == sizeof(void*) * 9 + 6, ...)`
  - `llvm/lib/CodeGen/AsmPrinter/AsmPrinter.cpp` 中 `EncodedGCPhaseOffsetInCJTLS = 78`

## phase 在编译器中有什么作用

### 1. `EnableGCPhase` 写屏障 fastpath 分流

- `llvm/lib/CodeGen/CJBarrierLowering.cpp`
- 编译器会插入 `GetGCPhase()` 检查，把 barrier 分成：
  - fast path
  - slow path
  - 或直接 `store/memcpy`
- 这个 `GetGCPhase()` 在后端不会真的 call，而会被汇编阶段展开成 TLS 读取。

### 2. AArch64 `emitGcStateCheck()`

- `llvm/lib/Target/AArch64/AArch64AsmPrinter.cpp`
- 对 `GetGCPhase()` 的 lowering 是：
  - `ldrb w9, [x28, #EncodedGCPhaseOffsetInCJTLS]`
- 也就是直接从 CJ TLS 里读 `encodedGCPhase`

### 3. MSGC new-array fastpath 也依赖 phase

- `llvm/lib/Target/AArch64/AArch64AsmPrinter.cpp::emitCJNewArrayForMSGC()`
- 当前逻辑：
  - `phase == IDLE(1)` -> fast alloc
  - `phase == MS_SWEEP(23)` -> fast alloc
  - 其他 -> `Slow`
- 这说明 phase 目前不只服务于 write barrier，也参与对象/数组分配侧 fastpath 判定

## 用全局变量是否会损失性能

- **会有一定损失，但更像“每次 barrier 入口多了几条指令”，不是严重的全局争用瓶颈。**

### 当前两种读法的 AArch64 指令差异

- 旧 TLS phase 读法：
  - `ldrb w9, [x28, #EncodedGCPhaseOffsetInCJTLS]`
  - 只需要一条 TLS 偏移加载
- 现在的全局 `g_msgcWBEnabled` 读法：
  - `adrp + add + ldarb`
  - 先物化全局地址，再做 acquire byte load

### 性能判断

- 相比 TLS 直接偏移读取，全局字节方案确实更重：
  - 多了地址物化
  - 多了 `acquire` 语义
- 但它通常不会形成很强的 cache ping-pong：
  - `g_msgcWBEnabled` 只在 phase/barrier 切换时写
  - 平时是大量只读
- 真正的性能代价主要来自：
  - write barrier 热路径上的固定额外指令数
  - 写密集场景下每次进入 inline gate 都要先读这个全局字节

### 当前判断

- 作为“先验证根因、先保证语义正确”的实验修复，全局字节是合理的。
- 作为最终长期版本，它不是最优性能形态。

## 更好的办法

- **更好的最终方案是：保留“barrier-installed state”这层语义，但把它下沉成 TLS 独立 bit 给 compiler 读取。**

### 原始推荐方向

- 不再让 compiler 从 `encodedGCPhase` 的数值范围推导“是否需要 write barrier”
- 改成由 runtime 在 barrier 安装/phase 切换时，直接向每个 mutator TLS 发布一个独立的 `WB enabled` 状态位
- compiler inline fastpath 只读取这个 bit：
  - `0` -> `StoreOnly`
  - `1` -> old-edge log fastpath

### 为什么最初认为它可能比全局字节更好

- 保留了这次实验已经证明有效的语义：
  - gate 跟随 **真实 barrier 状态**
  - 不再把 phase 数值窗口当作最终真相
- 同时恢复了 TLS fastpath 的低成本读取特性：
  - 可以重新回到类似 `ldrb [x28, #imm]` 的单条加载
  - 不需要每次都做 `adrp/add`

### 落点实现

- 首选：复用 `ThreadLocalData::gcFastState`
  - 让它变成真正的 fast-state bitfield
  - 例如：
    - bit0 = `WB enabled`
    - bit1 = `enum/mark assist`
- 备选：复用 `ThreadLocalData::reserved0`
  - 适合快速做实验
  - 但语义上不如 `gcFastState` 自然

### 关键注意点

- 这个 TLS bit **不能再通过 `phase -> if phase in [20,22]` 这种逻辑推导出来**
- 它必须在 runtime 的 barrier 安装点直接生成，并在 mutator phase/barrier 切换链路中发布
- 否则只是把旧问题换个字段名重新放回 TLS

### 这部分建议已被新实验修正

- 已验证：
  - 全局 `g_msgcWBEnabled` 作为 truth source 可以稳定压住进程级 crash
  - 第一版 TLS `gcFastState.bit0` 实现不能稳定压住 crash
- 因此当前不应直接把 “TLS bit 一定优于全局字节” 当成既定结论。
- 现阶段更稳妥的判断是：
  - **全局 truth source 已被实验验证有效**
  - **TLS shadow copy 版本还需要继续排查 publish 时序，不能直接作为最终收敛版本**
- 仍然明确不建议：
  - 回到 `encodedGCPhase` 数值窗口
  - 或把“全局 phase 镜像”继续当作 write barrier 最终判据

## 当前收敛判断

- 对 write barrier 而言，当前最强结论是：
  - **不应该再把 TLS `encodedGCPhase` 当成最终真相**
  - **应该让 inline gate 直接跟随 runtime 当前实际安装的 barrier 状态**
- 对分配 fastpath 而言，是否也要完全改成 barrier-installed/state-installed 风格，还需要单独审视；它与 write barrier 的语义并不完全相同，不能机械照搬。

## 当前剩余事项

- 本轮只说明“进程级 crash 明显收敛到 barrier-installed gate 方向”
- `case27` 的请求级 `fail=` 仍非零，后续仍需区分：
  - 是否还有独立于 crash 的业务层失败
  - 是否仍存在其它 MSGC 时序/可见性问题

## 2026-07-04 跟进：只移除 `CJ_MCC_ArrayCopyStructFast_MSGC` 仍会崩溃

### 本轮实验目的

- 在前述 write barrier gate 已经收敛到 “barrier-installed state 比 TLS phase 数值窗口更可靠” 之后，继续排查 `case27` 剩余进程级崩溃来源。
- 本轮先只移除 compiler 侧 `CJ_MCC_ArrayCopyStructFast_MSGC` 的 inline/fastpath 分发，让 `CJ_MCC_ArrayCopyRefFast_MSGC` 继续保留，观察是否仍会复现。

### 本轮构建与验证来源

- compiler：
  - 基于移除 `CJ_MCC_ArrayCopyStructFast_MSGC` 后的 LLVM 做增量重编
  - 安装到 `cangjie_compiler/output`
- std / stdx：
  - 均用新的 `cangjie_compiler/output/bin/cjc` 重编并重新安装到 `output`
- runtime：
  - 使用 `cangjie_compiler/output/runtime/lib/darwin_aarch64_cjnative/libcangjie-runtime.dylib`

### 验证配置

- 用例：
  - `MSGC_RESULT/Result/case27_http_complex_json`
- 参数：
  - `payload=100K`
  - `concurrency=1000`
  - `duration=60000ms`
  - `cjHeapSize=4gb`
- 结果目录：
  - `MSGC_RESULT/Result/case27_http_complex_json/result/20260704_000544_case27_c1000_p100K_60s_heap4gb_3runs_arraycopystruct_tryemit_removed/`

### 验证结果

- `dyld probe` 已确认运行时实际加载的是：
  - `cangjie_compiler/output/runtime/lib/darwin_aarch64_cjnative/libcangjie-runtime.dylib`
  - `cangjie_compiler/output/darwin_aarch64_cjnative/dynamic/stdx/libstdx.net.http.dylib`
  - `cangjie_compiler/output/darwin_aarch64_cjnative/dynamic/stdx/libstdx.encoding.json.dylib`
- 正式 3 轮验证没有全部跑完：
  - `run1` 在约 `48s` 左右提前崩溃
  - `exit_code=138`
- 崩溃栈：
  - `SIGBUS (signal 10)`
  - `std/core/array_common.cj::__CNat14CPointerHandleIG_E6<init>HPG_RNat5ArrayIG_E`
  - 上层在 `stdx.encoding.json::WriteBuffer.appendEscape`
  - 再往上是 `jsonWriteWithoutFormat -> JsonObject.toString -> processRequest`

### 这轮实验的直接结论

- **只移除 `CJ_MCC_ArrayCopyStructFast_MSGC` 并不能解决 `case27` 的进程级崩溃。**
- 这说明：
  - `ArrayCopyStruct` 不是当前这条 crash 路径的充分根因
  - 即使 write barrier gate 已经改成 barrier-installed state，仍然还有独立的 array-copy fastpath 风险没有消失
- 因为本轮保留下来的主要差异项就是 `CJ_MCC_ArrayCopyRefFast_MSGC`，所以当前下一步的主排查对象应收敛为：
  - **`CJ_MCC_ArrayCopyRefFast_MSGC`**

### 当前收敛判断（更新）

- write barrier 方向：
  - “看 barrier-installed state，而不是看 TLS `encodedGCPhase` 数值窗口” 这一条结论仍成立。
- 但对 `case27` 剩余 crash 而言：
  - 问题并没有因为 `ArrayCopyStruct` fastpath 单独下线而消失。
  - 当前更像是：**`CJ_MCC_ArrayCopyRefFast_MSGC` 仍然可能在并发 MSGC 下制造对象图/引用拷贝不一致。**
- 因此，后续主线应从“只盯 write barrier gate”进一步转向：
  - **重点分析 `CJ_MCC_ArrayCopyRefFast_MSGC` 的 inline 实现、phase/gate 条件、old/new ref 处理语义，以及它与慢路径 `CJ_MCC_ArrayCopyRef` 的行为差异。**
