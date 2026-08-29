# MSGC Windows Runtime LLT 失败用例根因分类与处置建议

## 范围与结论状态

本文分析 2026-08-28 Runtime LLT 的 13 个失败用例；仅基于本轮 `results.json`、测试源码与当前 Runtime 源码做静态交叉核验，未修改代码或重跑用例。

- 结果输入：[results.json](artifacts/01-runtime-llt-corrected/results.json)，汇总：[summary.txt](artifacts/01-runtime-llt-corrected/summary.txt)。
- 执行条件：Windows x86_64 cjnative；`cjc.exe --gc-mode marksweep -j1`；框架并行度 `-j 12`；单用例超时 180 秒。
- Runtime 源码：`MSGC-bugfix-regionScan`，`1a7484484f578c003628d5782528379c140b61d9`；测试源码：`cangjie_test/main`，`767c38b25541a201d38f9d53297faacee5517ff5`；SDK compiler：`0.0.1 (cjnative)`。
- SDK 中 runtime DLL 的构建提交未嵌入产物，无法证明它与上述 Runtime HEAD 完全一致。因此，源码级结论标记为“已由当前源码解释”，尚不等同于“已在事故 DLL 上闭环验证”。
- 排障模式库没有命中可复用的 Windows/MSGC 案例；其中命中的 macOS 页大小案例与本次平台不相关，未作为结论依据。

## 分类总览

| 类别 | 用例数 | 用例 | 结论强度 | 是否 Runtime 代码问题 |
| --- | ---: | --- | --- | --- |
| A. Windows 测试环境/命令不兼容 | 3 | `heap_snapshot_coverage`、`debuginfo_ffic_expr`、`runtime_api_validation` | 已证实 | 否 |
| B. 编译器进程启动失败 | 2 | `class_static_04`、`Exception30_TryCatchReturnValueException` | 直接错误已证实；诱因待复验 | 当前无证据 |
| C. OOM 语义未稳定落到 `OutOfMemoryError` | 6 | `Exception32`、`Expection38`~`42` | 两个致命路径已证实，其余需复验 | 是，优先修复 |
| D. GC/分配压力用例 | 2 | `wahStackmap_01`、`cj_innerArray_100000_0to8_GC_Int16` | 前者致命路径已证实；后者仅超时 | 前者是；后者待定 |

本轮最重要的结论是：Windows 下物理页提交失败没有被当作“可恢复的分配失败”上报给上层，而是被 `CHECK` 直接终止。这会绕过 `MSRegionSpace` 已有的 OOM 转换逻辑，导致本应输出或捕获 `OutOfMemoryError` 的测试变成进程崩溃或超时。

## A. Windows 测试环境/命令不兼容（3 个）

### 直接证据与根因

| 用例 | 首个失败命令/输出 | 根因 |
| --- | --- | --- |
| `CJNative/debuginfo_ffic_expr.cj` | `clang.exe ...`；`'clang.exe' is not recognized...` | C/FFI 编译器不在 PATH。 |
| `CJNative/RuntimeApi/runtime_api_validation.cj` | `clang.exe ...`；同样报未识别 | 同上，C Runtime API 验证根本未编译/运行。 |
| `CJNative/DFX/heap_snapshot_coverage.cj` | `env DumpStackDepth=64 heap_snapshot_coverage.exe`；`'env' is not recognized...` | 测试源文件无条件使用 POSIX `env`，Windows `cmd` 没有此命令。 |

`cjnative_test_win.cfg` 把 `%clang` 定义为 `clang.exe --target=x86_64-windows-gnu`。本机 MSGC SDK 的 `third_party/llvm/bin` 中仅发现 `llvm-ar.exe`，未发现 `clang.exe`；因此该定义依赖外部 PATH，但本轮环境未提供该工具。

### 处置方案

1. 为 Windows C/FFI 用例提供与 `x86_64-windows-gnu` ABI 匹配的 `clang.exe` 及其运行依赖；在启动框架前做 `clang.exe --version` 预检。
2. 将 Windows 配置中的 `clang` 指向已验证的工具链绝对路径，或把完整工具链加入 PATH；不要只复制单个 `clang.exe`，否则链接时仍会缺少 linker、sysroot 或 MinGW 库。
3. 修正 `heap_snapshot_coverage.cj` 的指令：保留非 Windows 的 `env DumpStackDepth=64`，增加 Windows 条件指令，例如 `(Windows) RUN-EXEC-PIPE: (set DumpStackDepth=64) && %run ... | compare %f`。仓库已有 Windows 用例采用 `(set cjHeapSize=...) &&` 的写法，可作为模板。
4. 先单独回归这三个用例；它们通过前，不应把失败计入 Runtime/GC 回归率。

## B. 编译器进程启动失败（2 个）

### 用例与证据

- `CJNative/Class/Define/class_static_04.cj`
- `CJNative/Exception/Exception30_TryCatchReturnValueException.cj`

两者都是执行首条 `cjc.exe` 命令时返回 `The system cannot execute the specified program.`，没有生成运行时输出、GC 日志、崩溃栈或断言信息。两个源文件分别是普通静态字段初始化和异常/finally 语义检查，本身没有大内存分配。

### 判断与处置

这是编译器或宿主进程启动阶段失败，不能据此归因 Runtime。由于该轮框架以 `-j 12` 并行执行，并且同批存在多个超大分配/OOM 用例，**资源紧张导致子进程创建失败是合理假设，但当前结果没有逐用例时间线，尚未证实**。

1. 先完成类别 A 的 clang/toolchain 修复。
2. 将 OOM/超大分配用例从普通 LLT 批次中拆出，再以 `-j 1` 单独重跑这两个用例。
3. 若仍出现同一错误，收集 `cjc` 的完整 stderr、Windows 事件日志/CreateProcess 错误码以及 SDK 依赖检查结果，转入 compiler/toolchain 排障；若单跑通过，则把本轮失败标记为批次资源隔离问题。

## C. OOM 语义未稳定落到 `OutOfMemoryError`（6 个）

### 用例结果

| 用例 | 期望 | 本轮实际结果 | 当前判断 |
| --- | --- | --- | --- |
| `Exception32_OutOfMemoryError` | 捕获 `OutOfMemoryError` 后输出 `20` | 180 秒超时 | 需受控内存复验 |
| `Expection38_NewArray8` | 非 O2 输出 `Out of memory` | 180 秒超时 | 需受控内存复验 |
| `Expection39_NewArray16` | 非 O2 输出 `Out of memory` | `VirtualAlloc failed in FreeManager::ReuseUnits` 后异常退出 | 已证实：Windows 提交失败走致命路径 |
| `Expection40_NewArray32` | 非 O2 输出 `Out of memory` | 180 秒超时 | 需受控内存复验 |
| `Expection41_NewArray64` | 非 O2 输出 `Out of memory` | 与 `Expection39` 相同的 `ReuseUnits` 致命失败 | 已证实：同一缺陷 |
| `Expection42_NewObject` | stderr 含 `Out of memory` | 进程退出码 0，未产生期望文本 | 未触发 OOM，测试预算不确定 |

`Expection39` 和 `Expection41` 的输出直接给出：

```text
Check failed: ret != nullptr
VirtualAlloc failed in FreeManager::ReuseUnits
```

当前源码中，`src/Heap/MAllocator/MSFreeManager.cpp:2505-2511` 的 `FreeManager::ReuseUnits()` 在 Windows 调用 `VirtualAlloc(..., MEM_COMMIT, ...)` 后使用 `CHECK_DETAIL(ret != nullptr, ...)`。而 `src/Heap/MAllocator/MSRegionSpace.cpp:73-93` 与 `101-121` 已明确设计为：分配尝试最终返回 0 时，请求一次 OOM GC，再通过 `ExceptionManager::OutOfMemory()` 抛出语言级异常。

因此 Windows commit 失败在进入上述上层逻辑之前被进程终止，形成了明确的错误处理断层。

### Runtime 修复方案

1. 将 `FreeManager::ReuseUnits()` 从 `void` 改为可报告失败的返回值；把 `VirtualAlloc` 失败传播到 `TryTakeRegions()`、`GrowOneSegmentLocked()`、`GrowAndTakeRegionsLocked()` 与 `TryTakeOrGrowRegions()`，使其最终表现为普通“无法分配 Region”。
2. 传播失败时必须保持 FreeManager 元数据一致：若 Region 已从空闲结构取出但 commit 失败，需要回滚归属/空闲位图/已提交边界；若发生在 grow 路径，不能提前执行 `InitializeFreshRegions()` 或推进 `committedEnd_`。
3. 让 `RegionSpaceMS::AllocateLargeObject()` / `AllocateRegularObject()` 的既有 OOM 重试与 `ExceptionManager::OutOfMemory()` 接管该失败。这样超大数组测试应走语言级 OOM，而非 `CHECK` 终止。
4. 同时审计同类 Windows commit 入口：`src/Heap/Allocator/MemMap.cpp:69-74` 的 `MemMap::CommitMemory()`、`src/Common/PagePool.h:56-82` 的 `PagePool::GetPage()`。其中与用户对象/GC 工作区按需分配有关的失败也应可返回；仅 Runtime 启动时确实无法继续的保留区初始化才保留 fail-fast。
5. 增加日志：失败地址、请求字节数、`GetLastError()`、调用场景（reuse/grow/page-pool），并增加一个受控 OOM 回归，防止再次把系统提交失败误报成崩溃。

### 测试用例侧处置

这些用例用 2GB~8GB 级数组或大量对象依赖宿主可提交内存，结果会随物理内存、页面文件和并发度变化。建议把它们改为受控预算用例：

- Windows 下以单独进程运行，使用 `-j 1`；
- 显式设置 `cjHeapSize`（MSGC 的 soft limit）和 `cjMSMaxVirtualHeapSize`（虚拟堆上限，最小 Segment 为 64MB）；
- 用足以启动 Runtime、但低于测试目标分配大小的固定预算，例如先从 `cjHeapSize=64MB`、`cjMSMaxVirtualHeapSize=256MB` 校准；
- 断言语言级 `OutOfMemoryError` 或标准错误文本，不再依赖“把 Windows 页面文件耗尽”。

## D. GC/分配压力用例（2 个）

### `wahStackmap_01`

该用例创建大对象栈值、再分配 1000 万个对象。stderr 为：

```text
VirtualAlloc commit failed in GetPage, errno: 1455
```

`src/Heap/Allocator/MemMap.cpp:69-74` 与 `src/Common/PagePool.h:66-78` 都会在 Windows `VirtualAlloc(..., MEM_COMMIT, ...)` 失败后 `CHECK_E`。因此它与类别 C 共享“低层提交失败致命化”的 Runtime 缺陷；错误码 1455 表示系统提交资源不足。修复方式是复用类别 C 的可传播失败设计，并把该压力用例放入低并发/受控堆预算组。

### `cj_innerArray_100000_0to8_GC_Int16`

该用例启动一个持续调用 `GC()` 的任务，同时连续执行 9 组 10 万元素数组计算；本轮只有 `TimeOut`，没有运行时日志、栈或断言输出。因此目前不能将其判定为死锁或数值错误。

建议先在完成类别 C 的隔离后以 `-j 1` 重跑，并设置 `MRT_REPORT` 保存 GC 阶段日志：

- 单跑通过：归类为原批次资源竞争/并发调度造成的超时，将该类压力用例改为低并发组；
- 单跑仍超时：采集 `MRT_REPORT`、进程线程栈和 CPU 使用率，重点检查 `GC()` 请求是否持续完成、mutator 是否恢复、`finished` 标志是否被观察到；再决定是否进入 MarkSweepCollector/CJThread 的代码级排障。

## 逐用例归类

| 用例 | 类别 | 本轮首要原因 | 建议动作 |
| --- | --- | --- | --- |
| `class_static_04` | B | `cjc` 启动失败，无 Runtime 证据 | OOM 用例隔离后单跑；必要时查 compiler 启动链。 |
| `heap_snapshot_coverage` | A | POSIX `env` 在 Windows 不存在 | 增加 Windows 条件运行指令。 |
| `Exception30_TryCatchReturnValueException` | B | `cjc` 启动失败，无 Runtime 证据 | 同 `class_static_04`。 |
| `Exception32_OutOfMemoryError` | C | 超时，未产生受控 OOM | 固定堆预算后复验；修复 commit-failure 传播。 |
| `Expection38_NewArray8` | C | 超时，未产生受控 OOM | 同上。 |
| `Expection39_NewArray16` | C | `ReuseUnits` 将 commit 失败致命化 | 修 Runtime 错误传播并回归。 |
| `Expection40_NewArray32` | C | 超时，未产生受控 OOM | 同上。 |
| `Expection41_NewArray64` | C | `ReuseUnits` 将 commit 失败致命化 | 修 Runtime 错误传播并回归。 |
| `Expection42_NewObject` | C | 当前堆预算下未发生 OOM | 采用固定 `cjHeapSize`/`cjMSMaxVirtualHeapSize`。 |
| `runtime_api_validation` | A | 未找到 `clang.exe` | 补全/显式配置 Windows GNU clang 工具链。 |
| `wahStackmap_01` | D | `GetPage` commit 失败被致命化 | 修低层 commit 失败传播；低并发回归。 |
| `debuginfo_ffic_expr` | A | 未找到 `clang.exe` | 同 `runtime_api_validation`。 |
| `cj_innerArray_100000_0to8_GC_Int16` | D | 超时，缺少 GC 现场 | `-j1` + `MRT_REPORT` 最小复现。 |

## 修复优先级与验证

1. **P0：修复测试环境与平台指令。** 补齐 clang、改正 `env` 指令后，单跑类别 A 的三个用例；预期三个均能进入真正的编译/运行阶段。
2. **P1：修复 MSGC Windows commit-failure 传播。** 以 `Expection39_NewArray16`、`Expection41_NewArray64` 和 `wahStackmap_01` 做最小回归。预期不再出现 `CHECK`/访问违规，OOM 用例应得到语言级 OOM。
3. **P2：建立 OOM 压力用例隔离策略。** 将 6 个 OOM 用例和 `wahStackmap_01` 设为串行、受控堆上限组；普通 Runtime LLT 保持并行。这样避免超大分配干扰无关编译或普通语义用例。
4. **P3：复验待定项。** 类别 B 两项和向量化压力项在 P0/P2 后分别 `-j1` 重跑；只有仍复现时才继续追查 compiler 启动失败或 GC liveness。

完成 P0~P3 后重新执行 Runtime LLT，并将“环境失败”“已验证 Runtime 缺陷”“待复验性能/并发问题”分别统计；不要再把它们合并成单一 Runtime FAIL 数字。
