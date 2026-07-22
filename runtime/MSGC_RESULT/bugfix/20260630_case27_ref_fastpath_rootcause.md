# 2026-06-30 case27 `WriteRef*Fast_MSGC` 根因收敛记录

## 结论

`case27_http_complex_json` 的 `c1000 + p100K` 崩溃，当前已经从“泛化的 MSGC runtime 不稳定”收敛到：

- `CJ_MCC_WriteStructFieldFast_MSGC` 不是主因
- `CJ_MCC_WriteRefFieldFast_MSGC` / `CJ_MCC_WriteStaticRefFast_MSGC` 是高置信主嫌

更准确地说，问题集中在 **AArch64 后端对 MSGC ref/static-ref write barrier wrapper 的 fast emission**，而不是慢路径
`MCC_WriteRefField` / `MCC_WriteStaticRef` 本身。

## 证据链

### 1. 只关掉 `WriteStructFieldFast` 后，case27 仍然崩

- 编译目录：
  - `MSGC_RESULT/Result/case27_http_complex_json/result/20260630_structfast_probe_manual`
- 运行目录：
  - `MSGC_RESULT/Result/case27_http_complex_json/result/20260630_structfast_probe_manual_run_c1000_p100K`

现象：

- 约 `22s` 左右崩溃
- `stderr.txt` 栈顶稳定落在：
  - `std/core/array_common.cj::__CNat14CPointerHandle...`
  - `stdx/encoding/json/write_buffer.cj::__CN18stdx.encoding.json11WriteBuffer12appendEscape...`
- 说明单独排除 `WriteStructFieldFast` 后，问题依旧存在

### 2. 把 `WriteRefFieldFast` / `WriteStaticRefFast` 全部改名绕开 fast emission 后，二进制只剩 slow-probe

LLVM 改动：

- `llvm/lib/CodeGen/CJBarrierLowering.cpp`
  - `cj_gcwrite_ref -> CJ_MCC_WriteRefFieldSlowProbe_MSGC`
  - `cj_gcwrite_static_ref -> CJ_MCC_WriteStaticRefSlowProbe_MSGC`

重编译并同步：

- LLVM `llc/libLLVM`
- runtime
- stdlib
- stdx

新的 case27 编译目录：

- `MSGC_RESULT/Result/case27_http_complex_json/result/20260630_231007_refslowprobe_manual_compile`

静态验证：

- `temps/benchmark.s` 中只出现
  - `CJ_MCC_WriteRefFieldSlowProbe_MSGC`
  - `CJ_MCC_WriteStaticRefSlowProbe_MSGC`
  - `CJ_MCC_WriteStructFieldSlowProbe_MSGC`
- 不再出现任何 `...Fast_MSGC`

### 3. 同参数复现两次，均不再崩溃

运行 1：

- `MSGC_RESULT/Result/case27_http_complex_json/result/20260630_231217_refslowprobe_manual_run2_c1000_p100K`
- 结果：
  - `exit=0`
  - `throughput=282 req/s`
  - `stderr.txt` 为 `0` 行

运行 2：

- `MSGC_RESULT/Result/case27_http_complex_json/result/20260630_231954_refslowprobe_manual_run3_c1000_p100K`
- 结果：
  - `exit=0`
  - `throughput=265 req/s`
  - `stderr.txt` 为 `0` 行

这两次都跑满了 `60s`，且都跨过了此前最容易出问题的 `~22s` 窗口。

## 代码层面的可疑点

高概率问题点在：

- `llvm/lib/Target/AArch64/AArch64AsmPrinter.cpp`
  - `AArch64AsmPrinter::emitMccWriteBarrierFastPath`

这里的 `IsRef / IsStaticRef` fast path，会直接：

1. 从字段里 `ldr` 旧值
2. 直接把旧值塞进 `AllocBufferMS::wbBuf_`
3. 再 `str` 新值回字段

而慢路径实际走的是：

- `CompilerCalls.cpp`
  - `MCC_WriteRefField`
  - `MCC_WriteStaticRef`
- `MSMarkBarrier.cpp`
  - `MSMarkBarrier::WriteReference`
  - `MSMarkBarrier::WriteStaticRef`

也就是：

1. `field.GetTargetObject()`
2. `EnqueueRefFast(oldRef)`
3. `field.SetTargetObject(ref)`

当前最值得继续审计的差异有两类：

- fast path 直接操作 `RefField` 原始 64-bit 值，与 `GetTargetObject()/SetTargetObject()` 的真实语义不完全等价
- fast path 直接按 TLS + 固定 offset 写 `wbBuf_`，与 runtime helper 的 `GetOrCreateAllocBufferMS()` / overflow flush / owner lifecycle 路径不同

## 当前建议

在没有彻底审清 `emitMccWriteBarrierFastPath` 的 ref/static-ref 分支之前：

- 先保留 `WriteRefFieldFast_MSGC` / `WriteStaticRefFast_MSGC` 的 slow-probe 绕行
- `WriteStructFieldFast_MSGC` 不必背这次 case27 的锅
- 后续修复应优先围绕 `AArch64AsmPrinter.cpp` 的 ref/static-ref fast path 做最小收敛

## 2026-07-01 补充：发现并修复了一个独立的 runtime 堆增长竞态

在基于 `compiler/output` SDK 复测 case27 时，又暴露出一个和 `WriteRef*Fast_MSGC` 不同层面的独立问题：

- 运行目录：
  - `MSGC_RESULT/Result/case27_http_complex_json/result/20260701_122959_compiler_output_sdk_msgc_c1000_p100K`
- 典型报错：
  - `NewObject: Allocate returned non-heap addr=0x7098000000`

这个报错的根因不是 `WriteRef*Fast_MSGC`，而是 MSGC runtime 在堆增长时：

1. 先把新 segment 挂进 shard 的 `ownedViews_`
2. 锁外才调用 `Heap::OnHeapExtended()` 更新 `heapCurrentEnd`

这样 mutator 可能先拿到新 region 分配，再被 `Heap::IsHeapAddress()` 用旧的 `heapCurrentEnd` 误判成“非堆地址”。

对应修复已经落在 runtime：

- `runtime/src/Heap/MAllocator/MSFreeManager.cpp`
  - 在 `GrowOneSegmentLocked()` 里把 `handler.OnCommittedHeapExtended(committedEnd_)` 前移到新 segment 对 mutator 可见之前
- `runtime/src/Heap/Heap.h`
- `runtime/src/Heap/Heap.cpp`
- `runtime/src/CangjieRuntime.cpp`
  - `heapCurrentEnd` 改成原子读写，消掉 `IsHeapAddress()` / 阈值同步读取上的数据竞争

修复后再次复测：

- 运行目录：
  - `MSGC_RESULT/Result/case27_http_complex_json/result/20260701_132243_compiler_output_sdk_msgc_heap_publish_fix_c1000_p100K`

结果是：

- 原来的 `non-heap addr=0x7098000000` 已经不再出现
- 但 case27 仍会在 `stdx.encoding.json::WriteBuffer.appendEscape -> MCC_AcquireRawData` 路径崩溃

这说明：

- **runtime heap growth 发布顺序竞态是真 bug，并且已经修掉**
- **但它不是 2026-06-30 这条 case27 根因文档里 `WriteRef*Fast_MSGC` 问题的替代解释**
- 当前高压 case27 至少包含两个独立问题：
  - 一个是已经修掉的 runtime 堆边界发布竞态
  - 另一个仍然高置信落在 `WriteRefFieldFast_MSGC` / `WriteStaticRefFast_MSGC` 相关路径

## 2026-07-01 再补充：runtime 修了以后仍然会崩，是因为编译器内联 fast path 还在绕过 `Heap::IsHeapAddress()`

继续对 `20260701_132243_compiler_output_sdk_msgc_heap_publish_fix_c1000_p100K` 的生成汇编核对后，可以把“为什么 runtime 修了还会崩”说得更具体一些。

### 现象

运行时崩溃点已经变成：

- `stdx.encoding.json::WriteBuffer.appendEscape`
- `MCC_AcquireRawData`

这不是 `MCC_AcquireRawData` 自己制造了坏指针，而是它收到的 `ArrayRef` / `String.rawData()` 视图已经损坏，崩溃只是对象损坏的暴露点。

### 关键代码事实

`compiler/output` 重新编译出来的 case27 汇编里，`CJ_MCC_WriteRefFieldFast_MSGC` / `CJ_MCC_WriteStaticRefFast_MSGC` 的 wrapper 本体虽然只是薄壳慢调用，但**真正的 fast path 已经被内联在 callsite 前面**。例如：

1. 先读 TLS 相位字节 `x28+#78`
2. 只在 `[20, 22]` 这几个 GC phase 内走 MSGC write barrier inline path
3. 直接 `ldr` 全局 `Heap::heapStartAddr`
4. 直接 `ldr` 全局 `Heap::heapCurrentEnd`
5. 用这两个值做 heap-range check
6. 判定为堆对象后才把 old ref 直写到 `wbBuf_`
7. 否则直接 `StoreOnly`

这条路径没有调用 runtime 的 `Heap::IsHeapAddress()`，而是自己在 AArch64 汇编里重写了一份“是否在堆中”的判断逻辑。

### 为什么这会让 runtime 修复失效

前面的 runtime 修复做了两件事：

1. 把 `OnCommittedHeapExtended()` 提前到 segment 对 mutator 可见之前
2. 把 `Heap::heapCurrentEnd` 改成 `atomic + release/store + acquire/load`

这样 runtime 内部凡是通过 `Heap::IsHeapAddress()` / `GetHeapCurrentEnd()` 读堆边界的路径，都能看到一致的扩堆发布顺序。

但编译器内联的 write barrier fast path 仍然是：

- 自己 `ldr` `heapCurrentEnd`
- 自己做 heap-range check

它既没有复用 `Heap::IsHeapAddress()`，也没有 acquire 语义。

结果就是：

- **runtime 慢路径已经修好**
- **compiler inline fast path 仍可能看到旧的 `heapCurrentEnd`**

一旦 case27 在并发标记期间发生扩堆，落在新 committed 区间里的：

- `obj`
- 或 `oldRef`

就可能被 inline fast path 误判成“非堆对象”，直接跳到 `StoreOnly`，不再记录 old edge。

这会重新回到最初那类 SATB 语义错误：

- old edge 漏记
- 仍存活对象被误 sweep / 误回收
- 随后在 JSON / String / Array 路径上表现成随机对象损坏

### 为什么这个解释和现场证据对得上

1. `8192 -> 9088` 的 `wbBuf_ count_` 偏移修复已经真实进入当前生成汇编，因此“写到错 offset”不再能解释这次新崩溃。
2. 当前 case27 失败 run 的 `gc.log` 显示：
   - 初始 committed heap 是 `0x7040000000 ~ 0x7060000000`（512MB）
   - 崩溃前 `current heap` 已增长到 `1,275,068,416 B`
3. 也就是说，这个 run 期间确实发生了多轮扩堆；而 inline fast path 恰好还在直接读取 `heapCurrentEnd` 做堆范围判定。
4. 之前把 `WriteRefFieldFast_MSGC` / `WriteStaticRefFast_MSGC` 绕成 slow-probe 后，case27 可稳定跑过，和这个解释完全一致。

### 当前结论

case27 当前剩余的高置信根因不是 “mask48 不够”，也不是 “runtime heap publish fix 没生效”，而是：

- **AArch64 编译器内联的 `WriteRef*Fast_MSGC` callsite fast path 仍在绕过 runtime 的原子化堆边界判断**
- **在扩堆后的并发标记窗口里，它仍可能把新堆中的 `obj/oldRef` 误判成非堆对象，从而漏记 old edge**

因此，真正要修的是 compiler 侧 ref/static-ref callsite fast emission，而不是继续只改 runtime。

## 2026-07-01 再补充：把默认 `heapSize` 提到 2GB 以后，case27 仍然会在 `c1000 + p100K` 崩

为了验证“是不是因为初始堆太小、扩堆瞬间把 case27 打坏”，本地又做了一轮只改默认堆参数的实验。

### 实验内容

runtime 侧改动：

- `src/CangjieRuntimeApi.cpp`
  - `heapParam.heapSize == 0` 时的默认值改成 `2 * GB / KB`
- `src/CjScheduler.cpp`
  - `MRT_USE_MARK_SWEEP` 下默认 heap size 从 `256MB` 统一改成 `2GB`

重新编译后，把新的 runtime dylib 覆盖到：

- `/Users/lipeisheng/code/cangjie/cangjie/cangjie_compiler/output/runtime/lib/darwin_aarch64_cjnative/libcangjie-runtime.dylib`

并使用下面这套三件套做复测：

- compiler:
  - `/Users/lipeisheng/code/cangjie/cangjie/cangjie_compiler/output`
- runtime:
  - 同上 SDK 中替换后的 `libcangjie-runtime.dylib`
- stdx:
  - `/Users/lipeisheng/code/cangjie/cangjie/cangjie_compiler/output/darwin_aarch64_cjnative`

### 先验证默认值是否真的生效

短探针目录：

- `MSGC_RESULT/Result/case27_http_complex_json/result/20260701_194459_compiler_output_sdk_2gb_default_probe_c1_p100K`

结论：

- `DYLD_PRINT_LIBRARIES` 显示实际加载的是：
  - `.../cangjie_compiler/output/runtime/lib/darwin_aarch64_cjnative/libcangjie-runtime.dylib`
- `c1 + p100K` 短探针可正常结束：
  - `EXIT_CODE=0`
  - `[RESULT] payload=100K concurrency=1 duration=2104ms client_mode=sync`

说明：

- 新 runtime 已经真的被 SDK 加载
- “重新编译 + 替换 dylib + 重编 benchmark” 这条链路是有效的

### 正式复测结果

正式 run 目录：

- `MSGC_RESULT/Result/case27_http_complex_json/result/20260701_194536_compiler_output_sdk_2gb_default_c1000_p100K_run1`

运行参数：

- 不传 `cjHeapSize`
- 仅保留：
  - `cjHeapGrowth=1`
  - `cjScavengeGrowth=10`
  - `CASE27_PAYLOAD=100K`
  - `CASE27_CONCURRENCY=1000`
  - `CASE27_DURATION_MS=60000`

结果：

- `EXIT_CODE=138`
- 按照 Darwin 信号约定，相当于 `128 + SIGBUS`
- 崩溃前 `stdout.txt` 的最后进度大约停在：
  - `[PROGRESS] t=34589ms total=11350 ok=11209 fail=141 inflight=1000 +last2s=665`

`stderr.txt` 栈顶仍然落在：

- `std/core/array_common.cj::__CNat14CPointerHandle...`
- `stdx/encoding/json/write_buffer.cj::__CN18stdx.encoding.json11WriteBuffer12appendEscape...`
- `stdx/encoding/json/json_object.cj::__CN18stdx.encoding.json10JsonObject8toString...`
- `benchmark.cj:processRequest`

这和之前 case27 的对象损坏暴露点一致，没有变成“普通超时”或“单纯跑慢”。

### 这轮实验真正说明了什么

`gc.log` 里有两个非常关键的事实：

1. runtime 参数里，默认堆大小已经变成：
   - `Heap size: 2097152(KB)`（2GB）
2. 但 `MSRegionManager initialized` 里启动时的 committed heap 仍然只有：
   - `heapStart=0x7040000000`
   - `committedEnd=0x7060000000`
   - 也就是 **512MB**

并且在 run 过程中，`current heap` 确实继续长到了：

- `671,088,640 B`
- `939,524,096 B`
- `1,073,741,824 B`
- `1,409,286,144 B`
- 崩溃前最后一轮 GC 甚至已经到
  - `1,744,830,464 B`

因此这轮实验的边界要说清楚：

- **它证明了：单纯把 runtime 暴露出来的默认 `heapSize` 参数改到 2GB，并不能修好 case27。**
- **它也证明了：当前 MSGC 的“启动已提交堆”并不会因为这个参数改动直接变成 2GB，扩堆路径仍然会发生。**

所以：

- 这轮实验**不能**把“扩堆相关问题”彻底排除掉
- 但它已经足够说明：
  - “把默认 `heapSize` 从小值抬到 2GB”不是当前 case27 的直接解法
  - 当前更高优先级的主线仍然是前文已经收敛过的
    `WriteRefFieldFast_MSGC` / `WriteStaticRefFast_MSGC` callsite inline fast path

## 2026-07-01 再补充：把初始 committed heap 也固定到 2GB、彻底去掉扩堆路径后，case27 仍然会崩

上面那轮实验只能说明“默认 `heapSize=2GB` 不够”，还不能回答“如果一开始就把 committed heap 全部铺满、后续也不允许 shrink 回去，case27 还会不会崩”。

因此又做了一轮更强的实验：

- `src/Heap/MAllocator/MSRegionManager.cpp`
  - 启动 committed heap 不再按 `8 * 64MB = 512MB` 的 shard 初始段数来算
  - 改成直接按 `heapSize` 一次性 commit
- `src/Heap/MAllocator/MSFreeManager.cpp/.h`
  - 记录启动时的 `committedRegionCount`
  - `ShrinkHeap()` 不允许跌破这个初始 committed 水位

### 先验证“真的没有扩堆路径了”

短探针目录：

- `MSGC_RESULT/Result/case27_http_complex_json/result/20260701_195544_compiler_output_sdk_2gb_noexpand_probe_c1_p100K`

关键结论：

- `DYLD_PRINT_LIBRARIES` 显示实际加载的仍然是：
  - `/Users/lipeisheng/code/cangjie/cangjie/cangjie_compiler/output/runtime/lib/darwin_aarch64_cjnative/libcangjie-runtime.dylib`
- `gc.log` 启动即显示：
  - `Heap size: 2097152(KB)`
  - `heapStart=0x7040000000`
  - `committedEnd=0x70c0000000`
- 从地址差值可知，启动 committed heap 已经是完整 `2GB`
- 后续多轮 GC 中 `current heap` 反复保持：
  - `2,147,483,648 B`

也就是说，这轮探针已经不是“逻辑 heapSize=2GB，但实际还是 512MB 起步”的旧情况，而是：

- **启动即完整 commit 到 2GB**
- **运行中没有再走扩堆轨迹**

### 正式复测结果

正式 run 目录：

- `MSGC_RESULT/Result/case27_http_complex_json/result/20260701_195626_compiler_output_sdk_2gb_noexpand_c1000_p100K_run1`

运行参数仍然是：

- 不传 `cjHeapSize`
- `cjHeapGrowth=1`
- `cjScavengeGrowth=10`
- `CASE27_PAYLOAD=100K`
- `CASE27_CONCURRENCY=1000`
- `CASE27_DURATION_MS=60000`

结果：

- `EXIT_CODE=138`
- 崩溃前最后进度大约停在：
  - `[PROGRESS] t=10060ms total=3128 ok=3106 fail=22 inflight=1000 +last2s=740`

`stderr.txt` 栈顶仍然是同一类 JSON 序列化暴露点：

- `std/core/array_common.cj::__CNat14CPointerHandle...`
- `stdx/encoding/json/write_buffer.cj::__CN18stdx.encoding.json11WriteBuffer12appendEscape...`
- `stdx/encoding/json/json_object.cj::__CN18stdx.encoding.json10JsonObject8toString...`
- `benchmark.cj:processRequest`

### 这轮实验现在能得出的结论

这一次可以把边界说得更硬一点：

- **case27 的当前崩溃并不依赖“从 512MB 启动后继续扩堆”这条路径。**
- **即使 runtime 在启动时就完整 commit 2GB，并且整个 run 期间 `current heap` 始终固定在 2GB，case27 仍然会以相同类型的 `SIGBUS` 崩在相同的 JSON 序列化暴露点。**

因此，关于“是不是扩堆把对象/引用关系打坏”的怀疑，当前可以降为低优先级；
`case27` 的主因判断应继续收敛在前文已经反复验证过的：

- `CJ_MCC_WriteRefFieldFast_MSGC`
- `CJ_MCC_WriteStaticRefFast_MSGC`

对应的 AArch64 inline fast emission 语义偏差。

## 2026-07-04 验证补充：`compiler/output` SDK 的 `c1000 + p100K` 不再复现旧崩溃签名，但仍有高失败率和退出挂死

这次使用的是重新产出的 `compiler/output` 三件套：

- compiler:
  - `/Users/lipeisheng/code/cangjie/cangjie/cangjie_compiler/output/bin/cjc`
- runtime:
  - `/Users/lipeisheng/code/cangjie/cangjie/cangjie_compiler/output/runtime/lib/darwin_aarch64_cjnative/libcangjie-runtime.dylib`
- stdx:
  - `/Users/lipeisheng/code/cangjie/cangjie/cangjie_compiler/output/darwin_aarch64_cjnative/dynamic/stdx`

结果目录：

- `MSGC_RESULT/Result/case27_http_complex_json/result/20260704_104053_compiler_output_sdk_msgc_c1000_p100K_verify`

### 先确认加载来源

先用 `c1 + p100K + 2s` 探针跑了一次，`probe.stderr` 里 `DYLD_PRINT_LIBRARIES` 明确显示实际加载的是：

- `/Users/lipeisheng/code/cangjie/cangjie/cangjie_compiler/output/runtime/lib/darwin_aarch64_cjnative/libcangjie-runtime.dylib`

因此，这次不是误用了旧 SDK runtime。

### 正式结果

正式参数：

- `cjHeapGrowth=1`
- `cjScavengeGrowth=10`
- `CASE27_PAYLOAD=100K`
- `CASE27_CONCURRENCY=1000`
- `CASE27_DURATION_MS=60000`

`stdout.txt` 中的结果是：

- `[RESULT] payload=100K concurrency=1000 duration=67918ms client_mode=sync`
- `[RESULT] total=12752 ok=7011 fail=5741 inflight=42 throughput=187req/s`
- `[RESULT] bytes_sent=665MB bytes_recv=671MB`

过程中出现了大量：

- `Client1.1 read response timeout`
- `SocketException: Failed to write data 32: Broken pipe`
- `Connection reset by peer`

也就是说，这轮已经不是“稳定成功跑完 60s”的状态，而是：

- **失败请求占比很高**
- **吞吐明显掉下去**
- **duration 被拖到 67.9s 才打印结果**

### 这轮没有看到什么

这次没有再看到此前那类显式崩溃签名：

- `NewObject: Allocate returned non-heap addr=...`
- `SIGSEGV` / `SIGBUS`
- `WriteBuffer.appendEscape -> MCC_AcquireRawData` 暴露出的对象损坏崩溃

所以它和 2026-06-30 / 2026-07-01 那几轮“直接内存损坏崩掉”的形态并不完全相同。

### 新问题：打印 `[DONE]` 后进程没有真正退出

虽然 `stdout.txt` 已经打印了：

- `[DONE]`

但前台 `benchmark` 进程并没有退出，`ps` 里长时间保持异常状态，最后需要手工 `kill -9` 才清掉。

对现场进程做 `lldb -p` 后，关键栈是：

- 主线程：
  - `CJ_ProcessorThreadExit`
  - `CJ_ProcessorSchedule`
  - `CJ_CJThreadMpark`
- 退出路径线程：
  - `CurrentProcess.exit`
  - `FiniCJRuntime`
  - `CJ_ScheduleStopOutside`
  - `MRT_StopGCWork`
  - `CollectorResources::StopGCThreads`
- GC 主线程：
  - `MarkSweepCollector::PreEnumRoots`
  - `CentralRegionPool::StopAndWaitForAllSweep`
  - `CentralRegionPool::ForceFinishAllUnswept`
  - `MSRegionInfo::SweepRegion`

这说明当前这套 `compiler/output` SDK 在 `case27 c1000 + p100K` 上，除了高超时/高失败率之外，还存在：

- **runtime 退出阶段等待 GC sweep 收尾时卡住**

### 额外注意

这次还顺手确认了一个脚本层面的坑：

- `cangjie_compiler/output/envsetup.sh` 里对 `DYLD_LIBRARY_PATH` 是裸展开
- 因此在 `env -i` 的干净 shell 里，如果外层开了 `set -u`，`source envsetup.sh` 会直接失败

这不是 case27 根因，但会让验证脚本误以为 “SDK 不可用”。
