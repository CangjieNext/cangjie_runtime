# Runtime HLT Linux x86 失败用例分析

## 1. 范围与结果

本次分析对象是 Runtime HLT 在 Linux x86_64 CJNative 下使用 MSGC SDK 的结果。

- 结果目录：`/home/lipeisheng/cangjie/cangjie_runtime_CMSGC/runtime/MSGC_RESULT/Tests/Test-linux-x86/Runtime-HLT`
- 测试仓库：`/home/lipeisheng/cangjie/cangjie_test`
- 测试框架：`/home/lipeisheng/cangjie/cangjie_test_framework`
- SDK/compiler：`/home/lipeisheng/cangjie/cangjie_compiler_CMSGC/output/msgc`
- stdx：`/home/lipeisheng/cangjie/cangjie_compiler_CMSGC/output/msgc/linux_x86_64_cjnative`
- 配置：`configs/hlt_basic_linux_x86.cfg`

本次结果有两个统计口径：

- `summary.txt`：1378 个有执行结果的用例，PASS 1322，FAIL 56。
- `results.json`/`results.xml`：1462 条记录，即 PASS 1322 + FAIL 56 + UNRESOLVED 84。

84 条 `UNRESOLVED` 的框架输出均为 `No valid command statement was found.`，并非测试程序执行后返回字面值 `UNSUPPORTED`。按要求，本文件将这 84 条在 Linux x86 本次配置下归入 `UNSUPPORTED`，并单独说明其实际原因。

## 2. 分类总览

| 分类 | 数量 | 主要原因 | 建议 |
| --- | ---: | --- | --- |
| GC export 配置/报告契约不匹配 | 24 | 用例仍按 CopyGC 旧报告字段和值校验，MSGC 输出了不同的有效配置 | 增加 MSGC 专用期望，或将旧报告用例与 MSGC 分开 |
| GC 非法参数触发信号处理异常 | 2 | 非法环境参数进入信号/栈展开异常路径，进程以 134 退出 | 修复非法参数的受控处理和信号展开路径 |
| GC processor 数量契约不匹配 | 6 | 用例将有效 processor 数量固定等同于 `/proc/cpuinfo` | 按 MSGC 的解析、上限和回退规则改写断言 |
| `SetGCThreshold` 语义变更 | 1 | MSGC 按当前堆大小维护阈值，忽略外部设置值 | 更新用例契约，或明确恢复可设置语义 |
| SpawnMain `RuntimeParam` ABI 不匹配 | 6 | 测试自带旧版 `Cangjie.h`，结构体偏移与 MSGC runtime 不一致 | 使用与 runtime 配套的公共头文件并重编 |
| Native 信号/异常输出契约不匹配 | 6 | 信号处理路径被触发，但诊断文本拼写/格式与期望不一致 | 统一 runtime 输出与用例断言，信号退出码单独断言 |
| 协程与 GC 压力下 SIGSEGV | 1 | 10000 个延迟协程叠加显式 GC 时发生 SIGSEGV | 缩小复现并定位调度、根扫描、屏障和 safepoint 交互 |
| PEA 压力用例超时 | 2 | 超大循环/分配量超过普通 HLT 超时边界 | 移入压力性能套件或降低规模 |
| 静态运行时栈/异常路径异常 | 6 | 静态 MSGC 下栈增长、栈溢出和 C/CJ 异常互操作进入 SIGSEGV/SIGABRT | 单独修复静态 runtime 的栈和异常路径 |
| Java/JNI 前置环境缺失 | 2 | 编译 C++ helper 时找不到 `jni.h` | 配置 JDK/JAVA_HOME，或在无 JNI 环境中标记 UNSUPPORTED |
| UNSUPPORTED（框架 UNRESOLVED） | 84 | 当前 Linux x86 HLT 配置未生成有效命令 | 按平台、优化级别、依赖文件和条件名修正测试清单 |

下面的 FAIL 分类合计 56，UNSUPPORTED 分类另计 84。

## 3. FAIL 用例分析

### 3.1 GC export 配置/报告契约不匹配（24）

受影响用例：

- `Runtime/cjnative/runtime/GC/gc_export/gc_env_macro/gc_export_001.info`
- `Runtime/cjnative/runtime/GC/gc_export/gc_env_macro/gc_export_003.info`
- `Runtime/cjnative/runtime/GC/gc_export/gc_env_macro/gc_export_005.info`
- `Runtime/cjnative/runtime/GC/gc_export/gc_env_macro/gc_export_006.info`
- `Runtime/cjnative/runtime/GC/gc_export/gc_env_macro/gc_export_007.info`
- `Runtime/cjnative/runtime/GC/gc_export/gc_env_macro/gc_export_008.info`
- `Runtime/cjnative/runtime/GC/gc_export/gc_env_macro/gc_export_009.info`
- `Runtime/cjnative/runtime/GC/gc_export/gc_env_macro/gc_export_010.info`
- `Runtime/cjnative/runtime/GC/gc_export/gc_env_macro/gc_export_012.info`
- `Runtime/cjnative/runtime/GC/gc_export/gc_env_macro/gc_export_013.info`
- `Runtime/cjnative/runtime/GC/gc_export/gc_env_macro/gc_export_025.info`
- `Runtime/cjnative/runtime/GC/gc_export/gc_export_001.info` 至 `gc_export_013.info`

这些用例可以编译并启动，失败发生在输出比较阶段。用例期望的是旧的 CopyGC 报告，例如 `Heap size`、`Region size`、`Heap growth`、`GC Threshold` 等固定字段和值；当前 MSGC 输出的是 `[MS-GC]` 配置和实际内存信息，例如：

```text
heap initialized: committedRegions=2048 committed=64.00MB ...
[MS-GC] config: heap=64.00MB region=0.06MB gcThreads=... P=...
MemoryInfo.maxHeapSize = ...
MemoryInfo.allocatedHeapSize = 65536
```

这不是普通对象分配或 GC 崩溃，而是“runtime 已按 MSGC 规则运行，测试仍比较 CopyGC 旧报告”的契约不一致。源码也明确区分两种模式：`src/CjScheduler.cpp:446-465` 说明 MSGC 独立计算 shard、初始 Segment 和软堆限制，`src/CjScheduler.cpp:529-534` 说明阈值由当前堆大小维护。

解决方式：

1. 为 MSGC 增加单独的期望报告和环境变量断言，只断言 MSGC 对外承诺的有效字段，避免复用 CopyGC 的固定数值。
2. 如果这些用例的目的就是验证 CopyGC 报告，则在测试配置中排除 MSGC，放回 CopyGC 专用任务。
3. 不建议为了通过旧比较而把 MSGC 的堆初始化、报告字段或阈值语义改回旧实现。

### 3.2 GC 非法参数触发信号处理异常（2）

受影响用例：

- `Runtime/cjnative/runtime/GC/gc_export/gc_env_macro/gc_export_004.info`
- `Runtime/cjnative/runtime/GC/gc_export/gc_env_macro/gc_export_011.info`

两例都能编译，但运行以 134 退出。日志包含：

```text
register rbx is not recorded
wrong reg info, start ip ...
CJNative Handle signal: 6
Thread "gc-main-thread" catched unhandled SIGABRT
Internal Compiler Error: Interrupt signal (6) received.
```

用例设置了非法 GC 环境参数，例如负的 `cjGarbageThreshold` 或负的 `cjGCThreshold`。当前实现的非法参数路径没有稳定地回退到默认值或返回受控错误，而是进入了信号/栈展开失败路径。

解决方式：

1. 为每个非法参数建立最小复现，先确认参数解析应当是默认回退、报错返回还是终止进程。
2. 修复非法参数分支的边界检查和信号/寄存器信息展开，不能让配置错误转化成内部编译器错误。
3. 修复前将这两例从普通 HLT 结果中单独隔离，修复后重新编译并验证，不要把它们与普通报告比较失败合并处理。

### 3.3 GC processor 数量契约不匹配（6）

受影响用例：

- `Runtime/cjnative/runtime/GC/gc_export/gc_export_020.info`
- `Runtime/cjnative/runtime/GC/gc_export/gc_export_021.info`
- `Runtime/cjnative/runtime/GC/gc_export/gc_export_022.info`
- `Runtime/cjnative/runtime/GC/gc_export/gc_export_023.info`
- `Runtime/cjnative/runtime/GC/gc_export/gc_export_024.info`
- `Runtime/cjnative/runtime/GC/gc_export/gc_export_025.info`

用例从 runtime 报告中取出 `Processor number:`，再与 `/proc/cpuinfo` 的 CPU 数量直接 `diff`，因此命令返回 1。当前实现 `src/CjScheduler.cpp:177-195` 对 MSGC 和 CopyGC 使用不同上限：MSGC 允许到 CPU 核数的 20 倍，且会根据 `cjProcessorNum` 做解析、截断或回退；有效 processor 数量不保证等于物理 CPU 数量。

解决方式：

1. 断言 MSGC 的“请求值、有效值、上限截断值、非法值回退值”，不要固定断言 `/proc/cpuinfo` 数量。
2. 把非法输入和默认值分别做成独立断言，以便判断是解析问题还是预期值问题。
3. 若该组只验证旧 CopyGC 行为，则对 MSGC 标记不适用，不应继续当作 runtime 功能 FAIL。

### 3.4 SetGCThreshold 语义变更（1）

受影响用例：`Runtime/cjnative/runtime/GC/gc_export/gc_export_029.cj`。

用例调用 `SetGCThreshold(234234234.0)`，期望扫描到对应的阈值。进程返回 0，但比较失败；runtime 日志为：

```text
Ignore requested gcThreshold=234234234 KB; gcThreshold is derived from current heap size.
```

这是当前 MSGC 的明确语义，见 `src/CangjieRuntime.cpp:171-181`；公共头文件也说明 `gcThreshold` 是由当前堆大小维护的值，见 `src/Cangjie.h:245-252`。

解决方式：优先将测试改为断言“请求值被忽略、阈值与当前堆大小同步”的新契约；只有在 MSGC 产品需求仍要求外部设置阈值时，才恢复 setter 语义并同步修改 runtime、头文件和全部相关用例。

### 3.5 SpawnMain RuntimeParam ABI 不匹配（6）

受影响用例：

- `Runtime/cjnative/runtime/SpawnMain/direct_call_sync/user.cj`
- `Runtime/cjnative/runtime/SpawnMain/spawn_main_create/user.cj`
- `Runtime/cjnative/runtime/SpawnMain/spawn_main_create_with_arg/user.cj`
- `Runtime/cjnative/runtime/SpawnMain/spawn_main_ffi/user.cj`
- `Runtime/cjnative/runtime/SpawnMain/spawn_main_ffi_with_arg/user.cj`
- `Runtime/cjnative/runtime/SpawnMain/spawn_main_multi_create/user.cj`

编译和链接成功，但运行以 134 退出，日志主要包含：

```text
RuntimeParam.coParam.coStackSize must be in range [64KB, 1GB].
Cangjie runtime should be initialized when initialize a sub scheduler.
runtime has NOT been initialized.
LoadCJLibrary fail. as InitCJLibrary return false
Check failed: runtime != nullptr
```

每个用例的 `main.c` 使用测试目录自带的 `SpawnMain/Cangjie.h`。该头文件的 `GCParam` 在 `gcThreads` 后结束，`LogParam`、`ConcurrencyParam` 从约 `:284-328` 开始；当前 runtime 的公共头文件在 `src/Cangjie.h:245-447` 还包含 MSGC 字段。因此 `RuntimeParam` 后续成员偏移不一致，runtime 读取到错误的 `coStackSize`，不是简单的栈大小取值错误。

解决方式：

1. 删除测试侧重复 ABI 定义，使用与所链接 `libcangjie-runtime.so` 配套的 SDK `Cangjie.h`。
2. 若必须保留测试头文件，则逐字节同步 `RuntimeParam` 及全部子结构，并增加结构体大小/偏移校验。
3. 头文件或 runtime 更新后，重新编译 C helper、CJ 动态库和最终用例；不能复用旧二进制。
4. 不要只把 `coStackSize` 改成某个值来规避问题，旧结构布局仍会继续破坏后续字段。

### 3.6 Native 信号/异常输出契约不匹配（6）

受影响用例：

- `Runtime/cjnative/runtime/Native/case26-unhandledSigabrt/case26-unhandledSigabrt.cj`
- `Runtime/cjnative/runtime/Native/case27-nativeflow/case27-nativeflow.cj`
- `Runtime/cjnative/runtime/Native/case28-unhandledSigsegv/case28-unhandledSigsegv.cj`
- `Runtime/cjnative/runtime/Native/case29-n2cUnhandledSigabrt/case29-n2cUnhandledSigabrt.cj`
- `Runtime/cjnative/runtime/Native/case30-n2cUnhandledSigsegv/case30-n2cUnhandledSigsegv.cj`
- `Runtime/cjnative/runtime/Native/case32-n2cexcept9/case32-nn2cexcept9.cj`

这些是 Linux 上真实执行的 C/CJ 信号和异常互操作用例，并非没有执行。运行时已经进入了预期信号路径：case26/29 为 SIGABRT，case28/30 为 SIGSEGV，case27 还输出了 `unhandled SIGSEGV from unmanaged stack overflow!`，case32 输出了 `Two pending exceptions ... Program will abort.`。

主要失败原因是输出契约：用例断言 `CJNative Handle signal: ...`，当前日志实际输出为拼写错误的 `CJNatvie Handle signal: ...`，并伴随时间戳、线程号、标点和退出码差异。例如 `case26...log` 和 `case32...log` 的 compare 记录明确显示该 scan 为 false，而 native frame 的正则断言可以匹配。case27 还需要单独确认 unmanaged stack overflow 的 signal 路径是否应以 139 结束。

解决方式：

1. 统一 runtime 的诊断文本拼写和格式，至少修正 `CJNatvie` 为 `CJNative`，并确定稳定的标点和字段格式。
2. 用例对故意触发的崩溃分别断言信号/退出码和稳定诊断关键词，不要把时间戳、地址和线程名作为固定字符串。
3. 对 case27 保留独立的 native stack overflow 回归检查，确认信号处理后不会误判为普通 SIGSEGV。

### 3.7 协程与 GC 压力下 SIGSEGV（1）

受影响用例：`Runtime/cjnative/runtime/GC/multi_coroutine_park_02.cj`。

用例创建 100 轮、每轮 100 个 coroutine，每个 coroutine 延迟后分配对象并更新全局值；主线程每轮显式调用 `GC()`，最后等待全部 Future。编译成功，运行返回 -11（SIGSEGV），日志显示：

```text
CJNatvie Handle signal: 11.
Thread "gc-main-thread" catched unhandled SIGSEGV ... addr: 0x8
... MarkSweepCollector::RunRegionTask(...) ... libcangjie-runtime.so
```

故障发生在 MSGC collector 的 `RunRegionTask`，不是编译或比较脚本问题。该用例应视为协程 park/resume、根扫描、写屏障、safepoint 与并发 GC 交互的真实压力回归。

解决方式：先用更小的轮数和 coroutine 数量做确定性复现，保留 core/backtrace；再检查 parked coroutine 的根注册/恢复、GC worker 的 region task 生命周期、对象引用更新和并发 sweep/mark 的同步。修复后保留一个缩小版回归用例，再恢复完整压力规模。

### 3.8 PEA 压力用例超时（2）

受影响用例：

- `Runtime/cjnative/runtime/pea/complex2.cj`
- `Runtime/cjnative/runtime/pea/complex3.cj`

两例都能编译，但执行约 119.2 秒后返回框架超时码 3，日志为 `TimeOut`。源码中的循环上限为 `80000000000`，每轮还创建包含嵌套对象的 `ComplexObject`，属于超大计算/分配压力，而非普通 HLT 功能验证。

解决方式：

1. 将两例移到独立性能/压力套件，设置明确的资源上限和超时基线。
2. 若要验证 PEA 功能本身，降低循环规模并保留可重复的功能断言。
3. 不建议直接提高普通 HLT 的全局超时时间，否则会掩盖真正的卡住问题。

### 3.9 静态运行时栈/异常路径异常（6）

受影响用例：

- `Runtime/cjnative/static_runtime/StackGrow01.cj`
- `Runtime/cjnative/static_runtime/case25-n2cexcept8/case25-n2cexcept8.cj`
- `Runtime/cjnative/static_runtime/exception_0006.cj`
- `Runtime/cjnative/static_runtime/stack001.cj`
- `Runtime/cjnative/static_runtime/stack213.cj`
- `Runtime/cjnative/static_runtime/stack416.cj`

这些用例明确通过静态方式编译，并在运行时 `unset LD_LIBRARY_PATH`，因此不能用注入动态 runtime 的方式规避问题。失败表现如下：

- `StackGrow01`、`stack001`、`stack213`、`stack416`：设置 64 KB 左右的 CJ 栈并触发栈增长/溢出，静态程序返回 139（SIGSEGV），而对应的动态栈用例可以产生 `StackOverflowError` 或通过比较。
- `case25-n2cexcept8`：C/CJ 异常互操作路径在地址 `0x18` 触发 SIGSEGV。
- `exception_0006`：4 MB 堆的 OOM 用例未得到期望的 `OutOfMemoryError`，而是出现 `register rbx is not recorded`、`wrong reg info` 后 SIGABRT。

解决方式：

1. 将静态栈增长、静态栈溢出、静态 C/CJ 异常互操作、静态 OOM 分成独立回归组，分别定位。
2. 优先检查静态 MSGC 的信号处理、栈边界/增长、异常 safepoint 和 unwind 元数据；用 core/backtrace 复现，不以“出现 SIGSEGV”代替期望异常。
3. 对 `exception_0006` 修复受控 OOM 异常传播，不能把 fatal signal 当成 OOM 结果。
4. 保持测试的 `unset LD_LIBRARY_PATH` 语义；修复后使用同一 MSGC SDK 重新编译静态测试和依赖。

### 3.10 Java/JNI 前置环境缺失（2）

受影响用例：

- `Runtime/cjnative/runtime/exclusiveScope/testcase0/test.info`
- `Runtime/cjnative/runtime/exclusiveScope/testcase1/test.info`

两例在编译 `cpp/call_cangjie.cpp` 时失败，错误为 `fatal error: 'jni.h' file not found`。测试指令要求 `${JAVA_HOME}/include`、`${JAVA_HOME}/include/linux`、`javac` 和 `java`；当前执行环境没有可用的 JNI 头文件。

解决方式：选择与 SDK 兼容的 JDK，执行测试前设置正确的 `JAVA_HOME`，并确认 `jni.h`、`javac`、`java` 均可用；如果 Linux HLT 环境明确不提供 Java/JNI，则把两例标记为 UNSUPPORTED，而不是归因于 runtime。

## 4. UNSUPPORTED 用例分析

### 4.1 O2 专用 IR 检查（9）

受影响用例为：

- `Runtime/Concurrency/Thread/thread009/thread00901.cj`
- `Runtime/Concurrency/Thread/thread009/thread00902.cj`
- `Runtime/Concurrency/Thread/thread009/thread00903.cj`
- `Runtime/Concurrency/Thread/thread009/thread00904.cj`
- `Runtime/Concurrency/Thread/thread009/thread00905.cj`
- `Runtime/Concurrency/Thread/thread009/thread00906.cj`
- `Runtime/Concurrency/Thread/thread009/thread00907.cj`
- `Runtime/Concurrency/Thread/thread009/thread00908.cj`
- `Runtime/Concurrency/Thread/thread009/thread00909.cj`

这些文件的 `EXEC`/`RUN` 都带有 `(not Windows and O2)` 条件，并检查 Future 对象的 LLVM IR。当前 HLT 配置的 condition 只有 `target_linux`、`CJNATIVE` 等，没有 `O2`，所以命令被条件过滤，框架没有生成命令。

解决方式：使用带 `O2` condition 的配置执行这组 IR 检查；或者将它们移入 O2/编译器优化任务。不能把当前未执行结果当成 runtime 功能失败。

### 4.2 异常/资源依赖源码片段（13）

受影响源码为：

- `Runtime/Exception/Resources/Bnf/002/test_20029.cj`
- `Runtime/Exception/Resources/Bnf/003/test_30029.cj`
- `Runtime/Exception/Throwable/008/use_80001.cj` 至 `use_80007.cj`
- `Runtime/Exception/Throwable/010/test.cj`
- `Runtime/Exception/Throwable/011/test_a.cj`
- `Runtime/Exception/Throwable/011/test_b.cj`
- `Runtime/Exception/try/005/use_50001.cj`

这些文件是由同目录的主测试通过 `EXEC` 引入的依赖、辅助实现或片段，不是独立入口。它们自身没有被当前框架选出有效的 standalone 执行命令，因此统一返回 `No valid command statement was found.`。

解决方式：保留为依赖文件，并从 standalone 扫描清单中排除；如果确实希望单独测试，则为其补充入口、完整编译命令、运行命令和断言。

### 4.3 OHOS 专用 Demangle（22）

受影响用例为 `Runtime/cjnative/runtime/DFX/Demangle/demangle_case1.cj` 至 `demangle_case22.cj`。

代表性指令见 `demangle_case1.cj:10-12`：

```text
(OHOS and not Android) EXEC: ... -lcangjie-demangle ...
(OHOS and not Android) RUN-EXEC: ...
```

当前目标是 Linux x86_64，不满足 `OHOS` 条件，且该组依赖 OHOS 的 demangle 链接环境。框架因此没有生成命令。

解决方式：在 OHOS 目标和相应 runtime/toolchain 中执行；Linux x86 HLT 清单中保持排除。

### 4.4 Finalizer 依赖源码片段（1）

受影响文件：`Runtime/cjnative/runtime/Finalizer/the_class.cj`。

该文件只有 `C` 类和析构函数定义，没有独立的 `EXEC`、`RUN-EXEC` 或断言，属于由其他 finalizer 主测试引用的类定义。当前框架没有生成 standalone 命令。

解决方式：作为依赖文件保留；由引用它的 finalizer 主测试验证。如果需要独立执行，补充主入口和完整指令。

### 4.5 GC 依赖/宏源码片段或指令格式问题（5）

受影响文件：

- `Runtime/cjnative/runtime/GC/check_array.cj`
- `Runtime/cjnative/runtime/GC/gc_export/gc_env_macro/define.cj`
- `Runtime/cjnative/runtime/GC/gc_export/gc_env_macro/test.cj`
- `Runtime/cjnative/runtime/GC/gc_export/gc_export_code.cj`
- `Runtime/cjnative/runtime/GC/gc_export/gc_export_026.cj`

前四个文件是被其他用例引用的类、宏或辅助代码，没有独立执行命令。`gc_export_026.cj` 虽有类似执行指令，但条件中使用了全角右括号，例如 `(not Android）`，与框架要求的 ASCII 条件语法不一致，导致该条件/命令无法被正常识别。

解决方式：

1. 将辅助文件只作为依赖，避免按单文件扫描成测试。
2. 将 `gc_export_026.cj` 的条件标记改为 ASCII，例如 `(not Android)`，并用框架做单例回归。
3. 修复后确认生成的命令和实际执行条件，再判断 runtime 结果。

### 4.6 PointerAuth/CFI 平台专用用例（26）

受影响用例包括：

- `Runtime/cjnative/runtime/PointerAuth/backwardCFI/exploit-wrapper.cj`
- `Runtime/cjnative/runtime/PointerAuth/backwardCFI/exploit.cj`
- `Runtime/cjnative/runtime/PointerAuth/backwardCFI/scenarios/` 下 22 个场景：`arith-exception*` 4 个、`exception-with-wrapper*` 2 个、`execute-cangjie-stub-return*` 2 个、`func-return-address*` 2 个、`oom-exception*` 4 个、`sof-exception*` 4 个、`user-exception*` 4 个
- `Runtime/cjnative/runtime/PointerAuth/forwardCFI/C2NStubAttack/test.cj`
- `Runtime/cjnative/runtime/PointerAuth/forwardCFI/N2CStubAttack/test.cj`

这些指令都受 `(OHOS and not Android)` 保护，部分还要求 `-march=armv8+pauth`、OHOS sysroot 和真实设备。Linux x86_64 不具备 ARM Pointer Authentication/CFI 运行条件，所以归入 UNSUPPORTED。

解决方式：在支持 Pointer Authentication 的 OHOS ARM 设备和对应 sysroot/toolchain 上执行；Linux x86 HLT 保持排除，不要通过修改为 x86 命令来伪造该安全特性测试。

### 4.7 exclusiveScope 平台/依赖源码（6）

受影响文件分两组：

- iOS/Objective-C 专用 4 个：`Runtime/cjnative/runtime/exclusiveScope/objc_interop_0/entry.cj`、`objc_interop_0/test.info`、`objcffi_0/src/cjworld/A.cj`、`objcffi_0/test.info`。其中 `test.info` 的执行指令受 `target_ios`/`iOSNative` 保护。
- Java 测试依赖 2 个：`Runtime/cjnative/runtime/exclusiveScope/testcase0/cj/javaCallcj.cj`、`testcase1/cj/javaCallcj.cj`。它们由 testcase 的 `test.info` 编译，不是 standalone 用例；对应主测试的 JNI 前置失败已在 3.10 单独记录。

解决方式：iOS 组放入 iOSNative/Objective-C 任务；Java helper 只作为 testcase 依赖，待 JDK 环境具备后从主 `test.info` 验证。

### 4.8 LTO 条件名称不匹配（2）

受影响用例：

- `Runtime/cjnative/runtime/lto/lto_test6_17.cj`
- `Runtime/cjnative/runtime/lto/lto_test8_1.cj`

两例的指令使用 `(Linux) EXEC`/`RUN-EXEC`，但当前配置的条件集合使用的是 `target_linux`，没有名为 `Linux` 的条件。框架的条件解析是按名称精确匹配的，见 `cangjie_test_framework/maple_test/utils.py:268-301`；因此这两例虽然源码中写了 Linux 指令，当前运行仍没有有效命令。

解决方式：把指令条件统一成当前框架配置使用的 `(target_linux)`，或在所有 Linux 配置中正式增加同名 `Linux` 条件并保持一致；修改后确认 `--test`/`--no-progress` 参数被正确替换，再单独运行 LTO 用例。

## 5. 建议处理顺序

1. 先修复或最小化复现真实 runtime 崩溃：SpawnMain ABI、协程+GC、静态 runtime 栈/异常、GC 非法参数。
2. 再统一 MSGC 的测试契约：GC export 报告、processor 数量、`SetGCThreshold` 语义和 Native 诊断文本。
3. 配置 Java/JNI 环境后单独重跑 exclusiveScope 的 testcase0/1；没有 JDK 时保持 UNSUPPORTED。
4. 将 PEA 两例移入压力/性能任务，避免普通 HLT 超时影响其他结果。
5. 清理测试清单：把依赖文件从 standalone 扫描中排除，修正 `O2`、`target_linux` 和全角标点等条件/指令问题，再在对应平台或配置中重跑。
6. 每次 runtime 或 SDK 变化后，使用当前 SDK 重新编译受影响的 `.cj`、C helper 和静态测试，不复用旧二进制。

## 6. 证据文件

- 结果统计：`Runtime-HLT/summary.txt`
- 结构化结果：`Runtime-HLT/results.json`、`Runtime-HLT/results.xml`
- 框架日志：`Runtime-HLT/framework-console.log`
- 单用例日志：`Runtime-HLT/log/*.log`
- 本次配置：`configs/hlt_basic_linux_x86.cfg`
- 测试源码：`/home/lipeisheng/cangjie/cangjie_test/testsuites/HLT/Runtime`
- 当前 runtime 参数/GC 实现：`src/CjScheduler.cpp`、`src/CangjieRuntime.cpp`、`src/Cangjie.h`
- 框架条件解析：`/home/lipeisheng/cangjie/cangjie_test_framework/maple_test/utils.py:243-330`

本文件只记录分析结果，未修改 runtime 源码、测试仓库或已有原始结果文件。
