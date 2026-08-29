# Runtime HLT Linux AArch64 失败用例分析

## 1. 分析范围

本文件分析以下 Runtime HLT 结果：

- 主结果：`run-20260828_211048/02-runtime-hlt`
- 对照结果：`run-20260828_204647/02-runtime-hlt`
- 测试仓库：`/home/lipeisheng/cangjie/cangjie_test`
- 目标：`aarch64-unknown-linux-gnu`
- 配置：`testsuites/HLT/configs/cjnative/linux_aarch64-linux_aarch64/basic.cfg`
- GC 模式：`marksweep`
- SDK：`/home/lipeisheng/cangjie/cangjie_compiler_CMSGC/output/msgc`

本次只做结果和源码分析，没有在本文件生成后重新编译或重跑 HLT。结论分为两种：

- `已确认`：日志、测试断言或源码已经能直接证明原因。
- `待专项定位`：已经确认失败位置和现象，但还不能仅凭现有日志确定唯一的 runtime 根因。

## 2. 结果概况

主结果原始统计如下：

| 状态 | 数量 | 说明 |
| --- | ---: | --- |
| PASS | 1322 | 正常通过 |
| FAIL | 56 | 进入了有效测试命令，但执行、返回码或输出断言失败 |
| UNRESOLVED | 84 | 框架没有找到有效命令，按约定归一为 `UNSUPPORTED`，不计入 FAIL |
| 合计 | 1462 | 原始框架统计 |

对照结果为 PASS 1323、FAIL 55、UNRESOLVED 84。两次结果的差异只有 `cjnative/runtime/GC/multi_coroutine_park_02.cj`：第一次通过，第二次在 GC 线程中崩溃。因此该用例具有非确定性，不能按稳定通过处理。

## 3. 分类汇总

以下分类互斥，合计 56 个 FAIL；`UNSUPPORTED` 单独统计，不计入 FAIL。

| 分类 | 数量 | 结果性质 | 处理优先级 |
| --- | ---: | --- | --- |
| A. `UNSUPPORTED`：无有效命令 | 84 | 测试框架/平台编排问题 | 先清理测试入口 |
| B. GC export：MSGC 与旧日志契约不一致 | 31 | 测试断言不匹配 | 高 |
| C. AArch64 GC 寄存器根记录异常 | 2 | runtime 缺陷候选 | 最高 |
| D. Finalizer 并发回调崩溃 | 1 | runtime 缺陷候选 | 高 |
| E. 协程停放与 GC 区域任务崩溃 | 1 | runtime 缺陷候选，且非确定 | 最高 |
| F. Native signal 日志拼写不一致 | 6 | runtime/test 输出契约问题 | 中 |
| G. SpawnMain 参数/ABI 问题，部分叠加线程资源不足 | 6 | 测试输入或环境问题候选 | 高 |
| H. exclusiveScope 缺少 JNI 头文件 | 2 | 测试环境依赖缺失 | 中 |
| I. SIGFPE 测试没有触发信号 | 1 | 测试构造/编译语义问题候选 | 中 |
| J. 静态 runtime 栈增长/栈溢出 | 4 | 静态 runtime 缺陷候选 | 高 |
| K. 静态 runtime C2N/N2C 异常路径 | 1 | 静态 runtime 缺陷候选 | 高 |
| L. 静态 runtime OOM 异常路径 | 1 | 行为或 GC 根扫描缺陷候选 | 高 |
| **FAIL 合计** | **56** |  |  |

## 4. 各类详细分析

### A. `UNSUPPORTED`：框架没有找到有效命令（84）

全部 84 个用例的原因都是 `no_valid_command`。框架日志对应 `No valid command statement was found.`，没有编译、运行或比较动作，因此不应作为 runtime FAIL。

按路径统计：

- `Concurrency/...`：9 个
- `Exception/...`：13 个
- `cjnative/...`：62 个

完整清单见：`run-20260828_211048/02-runtime-hlt/unsupported_cases.txt`。

建议：

1. 能补齐 AArch64 Linux 命令的用例，增加有效的 `EXEC`、`RUN-EXEC` 或 `RUN-EXEC-PIPE`。
2. 依赖其他目标平台、旧宏或不适用于当前框架的用例，加入平台 safe list，结果记录为 `UNSUPPORTED`。
3. 框架汇总时保留原始 `UNRESOLVED`，另输出归一化的 `UNSUPPORTED`，避免把不可执行用例混入 FAIL。

### B. GC export：MSGC 与旧日志契约不一致（31）

#### 用例

`gc_env_macro` 下的 11 个纯输出比较失败：

```text
gc_export_001.info  gc_export_003.info  gc_export_005.info
gc_export_006.info  gc_export_007.info  gc_export_008.info
gc_export_009.info  gc_export_010.info  gc_export_012.info
gc_export_013.info  gc_export_025.info
```

普通 `gc_export` 下的 20 个：

```text
gc_export_001.info  gc_export_002.info  gc_export_003.info
gc_export_004.info  gc_export_005.info  gc_export_006.info
gc_export_007.info  gc_export_008.info  gc_export_009.info
gc_export_010.info  gc_export_011.info  gc_export_012.info
gc_export_013.info  gc_export_020.info  gc_export_021.info
gc_export_022.info  gc_export_023.info  gc_export_024.info
gc_export_025.info  gc_export_029.cj
```

#### 证据和原因

当前 MSGC runtime 输出的是类似下面的格式：

```text
[MS-GC] heap initialized: committedRegions=...
[MS-GC] config: heap=64.00MB region=... gcThreads=... P=...
```

而这些 HLT `.info` 文件仍然断言旧 GC 的文本格式，例如：

```text
Heap size: 4096(KB)
Region size: 64(KB)
Heap growth: 1.15
GC Threshold: 20(KB)
Processor number: 24
Cangjie stack size: 4096(KB)
```

源码中 `/home/lipeisheng/cangjie/cangjie_runtime_CMSGC/runtime/src/CangjieRuntimeApi.cpp` 已体现 MSGC 的当前策略：

- 约第 267 行：默认初始堆为 `64 * 1024KB`。
- 约第 303 行：MSGC 的 heap growth 默认值为 `1.0`。
- 约第 317 行：`gcThreshold` 根据初始堆计算，不再接受旧的外部阈值语义。
- 约第 327 行：MSGC 的 GC 线程数按新的解析逻辑计算。

具体表现：

- `gc_export_020` 至 `gc_export_024` 的脚本 grep 旧的 `Processor number:`，得到空文件；当前日志中使用的是 `P=...`。
- `gc_export_029.cj` 调用 `SetGCThreshold(234234234)`，当前输出为忽略请求或阈值由当前堆派生，而测试仍期待 `gcThreshold is set to 234234234 KB.`。
- 其余用例主要是期望的旧字段缺失或数值/格式差异。

#### 解决方案

应先在测试契约上做选择：

1. MSGC 专用测试改为断言稳定的 `[MS-GC]` 配置字段和 MSGC 语义。
2. 需要验证旧 `Heap size:`、`Processor number:` 等输出的用例，继续放入 CopyGC/旧 GC 专用测试集，不要用 MSGC 运行。
3. 不建议为了通过旧 `.info` 而在 MSGC 中伪造旧日志或恢复已经废弃的 `SetGCThreshold` 语义。
4. 修改测试后重新运行全部 GC export，并单独确认 `gc_export_004`、`gc_export_011` 的寄存器根异常，因为它们不属于本类，而归入 C 类。

### C. AArch64 GC 寄存器根记录异常（2）

#### 用例

```text
cjnative/runtime/GC/gc_export/gc_env_macro/gc_export_004.info
cjnative/runtime/GC/gc_export/gc_env_macro/gc_export_011.info
```

#### 证据

两者的输出都包含：

```text
register x19 is not recorded
wrong reg info, start ip: ... frame pc: ...
gc-main-thread ... SIGABRT
```

这不是普通日志格式比较失败，而是 GC 根扫描的寄存器元数据校验主动失败。`gc_export_004` 使用 4MB heap、4KB region 等压力配置；`gc_export_011` 使用 4MB heap 和异常的 GC threshold 配置，均会更早触发 GC。

该现象与之前定位的 AArch64 `CJThreadContext`/stackmap 寄存器根记录问题属于同一故障族，但本次结果使用的是已经生成的 SDK runtime，不能据此证明当前源码中的修复已经生效。必须重建并刷新 `/home/lipeisheng/cangjie/cangjie_compiler_CMSGC/output/msgc` 后复验。

#### 解决方案

1. 确认 compiler 生成的 AArch64 stackmap 寄存器编号与 runtime `CJThreadContext` 保存/读取编号一致，重点检查 x19-x30 的保存顺序、偏移和有效位。
2. 用包含当前寄存器根修复的 runtime 重新构建 SDK，先单独运行这两个用例，再运行 Runtime LLT 中原有的寄存器根回归用例。
3. 开启 `MRT_REPORT`、debug 检查和 core/backtrace，确认修复后不再出现 `register x19 is not recorded`、`wrong reg info`。
4. 只有复验通过后，才可以将这两个用例从 runtime 缺陷候选改为 PASS；在此之前不能按 GC export 的输出契约问题处理。

### D. Finalizer 并发回调崩溃（1）

#### 用例

```text
cjnative/runtime/Finalizer/complex_1.cj
```

日志表现为：

```text
Thread "gc-helper" catched unhandled SIGSEGV (Segmentation fault) from runtime frame
signal pc: ... addr: 0x164
```

测试源码中的触发条件很明确：`C.~init()` 直接调用 `callBack`；回调先等待 barrier，再获取 semaphore；同一个 spawn 中的主逻辑在触发 `gc(heavy: true)` 前持有该 semaphore。测试注释还说明，只有“析构函数 + spawn”组合才稳定暴露问题。

runtime 的 `/home/lipeisheng/cangjie/cangjie_runtime_CMSGC/runtime/src/Heap/Collector/FinalizerWorkList.cpp` 约第 77-116 行显示，FinalizerProcessor 在线程进入 `ScopedObjectAccess` 后直接通过 `ExecuteCangjieStub` 执行用户 finalizer；工作对象在执行期间由 `workingFinalizables` 保持引用，执行结束后再从工作列表删除。

当前证据能确认 Finalizer/GC helper 在用户回调同步等待期间崩溃，但不能仅凭 `addr: 0x164` 确认是对象释放、线程栈、异常上下文还是调度状态损坏。因此根因状态为 `待专项定位`，不是 `UNSUPPORTED`。

#### 解决方案

1. 用 debug/ASAN runtime 重复执行该用例，断在 `ProcessFinalizableList`、`ExecuteCangjieStub` 和 signal handler，取得崩溃前的对象地址、finalizer 函数地址、当前 mutator 和栈边界。
2. 确认 FinalizerProcessor 执行用户代码时没有持有 GC/分配器内部锁，且 `workingFinalizables` 的根在整个回调和异常清理期间有效。
3. 检查 finalizer 线程的 managed context、`CJThreadContext` 和协程切换状态，避免在同步阻塞期间使用失效的栈或寄存器根信息。
4. 若设计上不允许 finalizer 回调直接执行阻塞式用户同步操作，应把该约束固化为测试/文档；若允许，则 runtime 必须保证阻塞、GC 和 finalizer 生命周期安全。
5. 增加该用例的循环压力回归，不能只以单次通过判定修复完成。

### E. 协程停放与 GC 区域任务崩溃（1）

#### 用例

```text
cjnative/runtime/GC/multi_coroutine_park_02.cj
```

测试每轮创建 100 个 coroutine，执行 sleep 和对象分配，主逻辑周期性调用 `GC()`，共循环 100 轮。第二次运行中：

```text
Thread "gc-main-thread" catched unhandled SIGSEGV ... addr: 0x8
#0 ... MarkSweepCollector::RunRegionTask(...)
```

第一次运行该用例通过，第二次失败，说明至少存在调度时序或资源状态相关的非确定因素。

当前 `/home/lipeisheng/cangjie/cangjie_runtime_CMSGC/runtime/src/Heap/MCollector/MarkSweepCollector.cpp` 约第 1199-1315 行的 `RunRegionTask` 会依次访问 region lease、mark bitmap、scan bitmap、slot 对象和 TypeInfo，然后完成 region 状态转换。该路径的任何 lease 生命周期、位图指针、对象槽位或并发发布错误，都可能在这里表现为小地址崩溃。

#### 解决方案

1. 用包含符号的 debug/ASAN runtime 单独循环执行该用例，至少保留首次失败的 core、`MRT_REPORT` 和完整线程日志。
2. 对 `MSTaskLease`、`MSRegionInfo`、bitmap、`regionStart`、`slotSize`、`TypeInfo` 增加失败前诊断，重点检查 region owner 状态和 pending mark words 的发布顺序。
3. 检查 `CompleteRegionWork`、`CasScanState(OWNED, IDLE)` 和重新发布 pending summary 之间的竞态，确认任务不会在 region 生命周期结束后继续访问。
4. 将该用例和 C 类寄存器根用例放入同一轮 AArch64 MSGC 回归，但保留为两个结果分类，避免没有证据时把所有 GC 崩溃合并成一个根因。

### F. Native signal 日志拼写不一致（6）

#### 用例

```text
cjnative/runtime/Native/case26-unhandledSigabrt/case26-unhandledSigabrt.cj
cjnative/runtime/Native/case27-nativeflow/case27-nativeflow.cj
cjnative/runtime/Native/case28-unhandledSigsegv/case28-unhandledSigsegv.cj
cjnative/runtime/Native/case29-n2cUnhandledSigabrt/case29-n2cUnhandledSigabrt.cj
cjnative/runtime/Native/case30-n2cUnhandledSigsegv/case30-n2cUnhandledSigsegv.cj
cjnative/runtime/Native/case32-n2cexcept9/case32-nn2cexcept9.cj
```

测试期待：

```text
CJNative Handle signal: ...
```

runtime 实际输出：

```text
CJNatvie Handle signal: ...
```

拼写错误位于 `/home/lipeisheng/cangjie/cangjie_runtime_CMSGC/runtime/src/Signal/SignalStack.cpp:101`。这些用例的实际 SIGABRT/SIGSEGV 触发、线程类型和返回码均已出现，比较失败的直接原因是日志字符串不一致；`case32` 还存在测试期望不带句号、runtime 输出带句号的格式差异。

#### 解决方案

优先修正 runtime 输出为 `CJNative`，再统一测试断言是否带句号。修改后重新运行这 6 个用例，确认 signal handler 的其余内容和返回码不变。不要仅通过修改测试把 `CJNatvie` 固化下来。

### G. SpawnMain 参数/ABI 问题，部分叠加线程资源不足（6）

#### 用例

```text
cjnative/runtime/SpawnMain/direct_call_sync/user.cj
cjnative/runtime/SpawnMain/spawn_main_create/user.cj
cjnative/runtime/SpawnMain/spawn_main_create_with_arg/user.cj
cjnative/runtime/SpawnMain/spawn_main_ffi/user.cj
cjnative/runtime/SpawnMain/spawn_main_ffi_with_arg/user.cj
cjnative/runtime/SpawnMain/spawn_main_multi_create/user.cj
```

它们依赖的 C 文件都设置：

```c
.thStackSize = 2 * 1024,
.coStackSize = 2 * 1024,
.processorNum = 8,
```

`coStackSize` 的单位是 KB，不是字节；测试使用的 2048 表示 2MB，按 API 文档的 `[64KB, 1GB]` 范围应当合法。现有源码 `/home/lipeisheng/cangjie/cangjie_runtime_CMSGC/runtime/src/CangjieRuntimeApi.cpp:93-98` 也按 KB 数值比较，2048 应通过检查。

本次日志中确实出现了 `RuntimeParam.coParam.coStackSize must be in range [64KB, 1GB].`，这说明测试实际传入值或使用的头文件/ABI 与当前 runtime 源码不一致，不能简单根据源码片段判定“2*1024 一定非法”。后续必须核对编译时 `Cangjie.h`、链接到的 runtime 和结构体布局。

错误链路随后表现为 runtime 未初始化、`LoadCJLibrary fail` 和 `runtime != nullptr` 检查失败。`spawn_main_ffi` 和 `spawn_main_ffi_with_arg` 还出现 `pthread_create ... Resource temporarily unavailable`，这可能是 HLT 并发运行多个 `processorNum=8` runtime 后的线程资源压力。

#### 解决方案

1. 对每个 SpawnMain 测试打印 `sizeof(RuntimeParam)`、`offsetof(coStackSize)`、头文件版本和实际加载的 runtime，确认 header/runtime 没有混用。
2. 用明确合法且统一的配置重跑，例如 `coStackSize = 64` 或 `128`（KB），并让 `thStackSize` 保持不小于系统要求。
3. SpawnMain 用例串行运行，运行前清理残留进程，检查 `ulimit -u`、线程数和 cgroup 限制。
4. 参数和 ABI 对齐后，若仍出现 runtime 初始化失败，再把问题升级为 SpawnMain runtime 缺陷；当前不应把初始化失败的后续检查当作独立 FAIL。

### H. exclusiveScope 缺少 JNI 头文件（2）

#### 用例

```text
cjnative/runtime/exclusiveScope/testcase0/test.info
cjnative/runtime/exclusiveScope/testcase1/test.info
```

编译失败的直接原因：

```text
fatal error: 'jni.h' file not found
```

用例命令通过 `${JAVA_HOME}/include` 和 `${JAVA_HOME}/include/linux` 查找 JNI 头文件；当前执行环境没有可用的 `JAVA_HOME` 或其下没有 `include/jni.h`。

#### 解决方案

1. 设置与目标架构匹配的 JDK `JAVA_HOME`。
2. 执行测试前检查 `$JAVA_HOME/include/jni.h` 和 `$JAVA_HOME/include/linux/jni_md.h` 存在且可读。
3. 如果当前 Linux AArch64 环境不提供匹配 JDK，则将这两个用例记为 `UNSUPPORTED`，不计入 FAIL；不要修改 runtime 来绕过缺失的 JNI 依赖。

### I. SIGFPE 测试没有触发信号（1）

#### 用例

```text
cjnative/signal/signal_10003.cj
```

测试为非 Mac、非 iOS、非 AArch32 目标期待返回 136，并期待 native SIGFPE。其依赖的 `sigfpe.c` 实现为：

```c
int a = 0;
int b = 1;
int res = b / a;
```

AArch64 Linux 本次实际返回 0 并输出 `res = 0`，没有进入 runtime signal handler。runtime 的 `/home/lipeisheng/cangjie/cangjie_runtime_CMSGC/runtime/src/SignalManager.cpp` 已安装 SIGFPE handler，故当前现象首先说明 C 端除零没有按测试预期产生硬件信号，不能直接归因于 runtime signal handler。

#### 解决方案

1. 如果测试目标是验证 runtime 的 SIGFPE handler，C 用例应使用显式、可移植的 `raise(SIGFPE)`，或采用不会被编译器按未定义行为优化掉的信号触发方式。
2. 如果必须保留整数除零，则固定编译选项并确认 AArch64 编译器没有将未定义行为折叠为普通返回；同时重新确认返回码 136 的契约。
3. 修正测试后再验证 runtime 是否输出预期的 native SIGFPE 信息。

### J. 静态 runtime 栈增长/栈溢出（4）

#### 用例

```text
cjnative/static_runtime/StackGrow01.cj
cjnative/static_runtime/stack001.cj
cjnative/static_runtime/stack213.cj
cjnative/static_runtime/stack416.cj
```

这些用例的编译命令均带 `--static`，运行时执行 `unset LD_LIBRARY_PATH`，因此使用的是编译时内嵌的 static runtime，不能由替换动态 `libcangjie-runtime.so` 直接修复。

前三个用例预期捕获 `StackOverflowError` 并打印栈。实际均为 runtime frame SIGSEGV，地址为 `0xfffffffffffffffc`，没有进入预期异常断言。`stack416` 创建大量 coroutine 并使用 64KB coroutine stack，多个 worker 同时在 runtime frame SIGSEGV，故还有栈增长与并发调度叠加因素。

#### 解决方案

1. 先为 static runtime 构建带符号版本，确认 AArch64 stack guard、stack grow stub、栈边界和 16-byte 对齐。
2. 对 `CJ_SOF_SIZE`、64KB stack 和 stack grow 入口增加边界检查，确认触发 StackOverflowError 前不会使用低于栈底的地址。
3. 检查异常展开和栈回溯使用的 frame address，特别是无效 frame 时不能继续解引用；历史上 `fa[-1]` 类的未保护访问会把栈损坏表现为 runtime SIGSEGV。
4. `stack416` 需要单独进行 coroutine 数量和 stack size 的分层压力测试，区分单 coroutine 栈增长问题与大量 worker 调度问题。

### K. 静态 runtime C2N/N2C 异常路径（1）

#### 用例

```text
cjnative/static_runtime/case25-n2cexcept8/case25-n2cexcept8.cj
```

该用例在 C 和 Cangjie 之间多次往返调用，并在 Cangjie 函数中抛出 `Exception`。静态可执行文件返回 -11，runtime frame SIGSEGV，故没有输出预期的 C2N/N2C 调用链和 `Exception`。

#### 解决方案

1. 对 static runtime 的 C2N、N2C stub、异常上下文保存和恢复分别加符号和寄存器转储。
2. 核对 AArch64 C ABI 下浮点参数、返回地址、callee-saved registers 和异常跳转上下文的布局。
3. 检查从 C 调用 Cangjie 后抛出异常时，是否错误地沿 C 栈或失效的 managed frame 展开。
4. 先让最小化的 C -> CJ -> throw -> C caller reproducer 稳定通过，再恢复该 HLT 的多层递归调用。

### L. 静态 runtime OOM 异常路径（1）

#### 用例

```text
cjnative/static_runtime/exception_0006.cj
```

用例设置 4MB heap，持续分配对象，预期捕获并输出 `OutOfMemoryError`。实际在 `gc-main-thread` 中 SIGABRT，并伴随：

```text
register x19 is not recorded
wrong reg info, start ip: ... frame pc: ...
```

因此它同时暴露出静态 runtime OOM 路径和 AArch64 寄存器根记录问题。分类上单独保留为 L 类，避免把“静态 OOM 语义”与 C 类的动态 GC export 配置压力混在一起。

#### 解决方案

1. 先用修复后的 AArch64 寄存器根记录重新构建 static runtime，确认 OOM 触发时不再因 root metadata 校验 abort。
2. 若仍不能捕获 `OutOfMemoryError`，继续检查 static MSGC 的 heap limit、GC 失败到异常构造的转换路径。
3. 验证抛出 OOM 前用于构造异常的 runtime 对象和异常上下文已经被正确保留。

## 5. 建议处理顺序

1. 修复 HLT 框架/平台清单：将 84 个无有效命令用例明确记录为 `UNSUPPORTED`，或补齐 Linux AArch64 命令。
2. 修正 `/runtime/src/Signal/SignalStack.cpp:101` 的 `CJNatvie` 拼写，并统一 6 个 Native signal 用例的断言格式。
3. 核对 SpawnMain 的 header/runtime ABI 和参数值，串行复验，排除线程资源不足。
4. 将 GC export 旧日志断言迁移到 MSGC 语义，保留 CopyGC 旧格式测试；之后复验 31 个输出类用例。
5. 重建并刷新 SDK/runtime，优先复验 `gc_env_macro/gc_export_004`、`gc_env_macro/gc_export_011` 和静态 `exception_0006`，确认寄存器根修复是否覆盖 HLT。
6. 对 `multi_coroutine_park_02` 做循环、debug/ASAN 和 core 分析，定位 `RunRegionTask` 的并发状态问题。
7. 对 `Finalizer/complex_1` 做循环和 debug/ASAN 分析，确认 finalizer 阻塞回调期间的对象、线程栈和 managed context 生命周期。
8. 修正 SIGFPE 测试构造后再复验 signal handler。
9. 最后重建 static runtime，处理四个栈增长用例和 C2N/N2C 异常用例。

## 6. 当前结论

- 84 个 `UNSUPPORTED` 是框架没有有效命令，不计入 FAIL。
- 31 个 GC export 主要是 MSGC 输出/语义与旧 `.info` 契约不一致，不应通过伪造旧日志解决。
- 6 个 Native signal 用例的主要失败原因是 runtime 日志拼写 `CJNatvie`，signal 行为本身已发生。
- 2 个 exclusiveScope 用例是 JNI 环境缺失，建议补环境或归入 `UNSUPPORTED`。
- SpawnMain 的失败需要先核对实际编译头文件、runtime 二进制和 `RuntimeParam` ABI，当前不能仅凭源码中的 `2 * 1024` 下结论。
- `gc_env_macro_004`、`gc_env_macro_011`、`multi_coroutine_park_02`、Finalizer 以及静态 runtime 的 6 个用例仍属于 runtime 缺陷候选，需要使用重建后的目标 runtime 进行专项复验。
