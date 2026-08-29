# Windows MSGC Runtime HLT 失败用例根因分类与处置建议

## 范围与结论

基于 `artifacts/02-runtime-hlt/results.json` 的既有执行证据，对失败和未执行用例分类；本报告不把未复测的推断写成已修复结论。该结果生成于 2026-08-28，使用 Windows x86_64、`cjc --gc-mode marksweep`。

结论是：大头并不是 MSGC 用例实际失败。154 个 `FAIL` 中有 105 个首先失败在 Windows 测试命令本身；122 个 `UNRESOLVED` 完全没有运行命令。应先修复框架和平台选择，随后再以干净环境重跑，才能得到可信的 runtime 失败集。

## 总体统计与判定方法

| 结果 | 数量 | 判定 |
| --- | ---: | --- |
| PASS | 1243 | 已执行且比较通过 |
| FAIL | 154 | 有失败命令或比较失败；需按首个失败点分类 |
| UNRESOLVED | 122 | `commands` 为空，`output` 也没有返回码或错误文本；不能视为 runtime 失败 |
| 合计 | 1519 | Runtime HLT 清单总数 |

证据来自每个 case 的 `commands`、`output.return_code`、stdout/stderr，以及当前 runtime 源码的对应错误日志位置。一个用例若同时有多种问题，以下按**最先阻断结果的原因**归类；因此类别数量可直接相加为 154。

## A. 未执行：UNRESOLVED

122 个 `UNRESOLVED` 均没有实际 `commands`，三个空 output 条目也没有返回码。这只能证明框架没有把它们调度到编译/运行阶段，不能证明 source、compiler 或 runtime 有错。

主要目录分布如下：

| 目录类型 | 数量 | 可能缺失前置条件 |
| --- | ---: | --- |
| Native FFI/ABI | 29 | C/C++ 辅助编译、动态库、ABI 工具链 |
| DFX Demangle | 22 | demangle 测试依赖或专用 runner |
| 静态 runtime | 21 | 静态链接/特定构建模式 |
| Exception | 13 | 多文件依赖或 runner 规则 |
| `thread009` | 9 | 并发测试调度规则 |
| exclusiveScope（ObjC/Java） | 8 | Windows 不具备 ObjC/Java 互操作条件 |
| SpawnMain | 6 | 多入口/子进程构建规则 |
| GC 特殊用例 | 6 | 专用依赖或平台过滤 |
| signal | 4 | Windows 信号语义差异 |
| 其他（Finalizer/lto/ouroboros） | 4 | 各自的编译或平台前置条件 |

**处理建议：**先在测试框架中为 Windows 明确标记不支持的 ObjC、Unix signal、静态 runtime、专用 FFI 用例为 `SKIP`；对其余项检查用例 discovery、依赖文件复制、`test.info` 解析和生成命令。修复后只重跑这 122 项，并要求每项至少产出编译命令、运行命令和退出码。

## B. Windows 测试框架/外部工具不兼容

此类共 **105** 项，程序本体是否正确尚不可判定：后处理/比较命令先失败，遮蔽了用例输出。

| 首个阻断点 | 数量 | 原始证据 | 处理建议 |
| --- | ---: | --- | --- |
| `bash -c "dos2unix log.txt"` | 68 | Windows 调用 `bash` 后转到未安装发行版的 WSL | Windows runner 改用 PowerShell/原生文本读取；不要通过 WSL 做 CRLF 处理。若只为比较，比较器应直接支持 CRLF。 |
| `cat` 不存在 | 31 | `cat is not recognized...`；其中 30 项为 `gc_export`，另有 `thread01009` | 将 Windows 模板中的 `cat log.txt` 改为 `type log.txt`，或让 Python 比较器直接读文件。 |
| `grep` 不存在 | 5 | `grep is not recognized...`，均为 stack002 过滤日志 | 用 Python 正则过滤或 PowerShell `Select-String` 实现等价过滤，避免 `grep`。 |
| `clang` 不存在 | 1 | Native `case32-n2cexcept9` 辅助编译失败 | 配置 Windows Clang 并加入 PATH，或在测试配置中使用 SDK 自带 clang。 |

这 105 项应作为测试基础设施问题单独闭环；在基础设施修复前，不应把它们计入 MSGC 通过率或 runtime 回归数量。

## C. MSGC Runtime 确认问题

### C1. OOM 时 bitmap 提交失败被致命检查终止（3 项）

涉及 `oom_env_01`、`oom_env_10`、`gc_concurrency_09`。日志包含：

```text
[MS-GC] MSBitmapArena commit failed
F Check failed: RemapChunkRange(...)
```

根因是 `MSBitmapArena::EnsurePlaneRangeCommitted` 原先把 `VirtualAlloc`/`mmap` 映射失败升级为 fatal `CHECK`，绕过 `OutOfMemoryError`。

**当前状态：已修复但未重新跑完整 HLT。** 相关函数已改为返回失败，`MSRegionInfo`、CentralPool、AllocBuffer 向上回传并归还半初始化 region；详细记录见 `MSGC_RESULT/bugfix/20260829_msgc_cross_platform_oom_propagation_fix.md`。修复框架后应优先重跑这 3 项和全部 `OutOfMemoryError` 子目录。

### C2. 栈映射寄存器缺失导致异常展开/GC fatal（17 项）

涉及 9 个 OOM 环境用例、8 个 `Exception/Throwable/013` 或宏用例。共同日志为：

```text
E register rbx/rsi is not recorded
F wrong reg info, start ip: ... frame pc: ...
```

runtime 的检查点在 `src/StackMap/StackMapTypeDef.h`，fatal 发生在 `src/UnwindStack/StackGrowStackInfo.cpp`（以及 GC tracing 路径）。这说明对应 safepoint/异常展开帧要求的寄存器不在 stack map 中；runtime 不能忽略该错误，否则 GC 可能漏扫根并造成堆损坏。

**处置建议：**以一个 OOM case 和一个 Throwable case 各自执行 `cjc --save-temps`，对照生成 stackmap、LLVM statepoint/寄存器分配和 `StackMapTypeDef` 的寄存器编号；修复应落在 compiler/LLVM 的 Windows x86_64 stackmap 生成或 lowering，runtime 保留 fatal 校验。修复后重跑 17 项。

### C3. Finalizer 上调用 ThreadSnapshot 得到无效 Mutator（1 项）

`Finalizer/dump_thread.cj` 在 `ThreadSnapshot.dumpCurrentThread()` 后出现：

```text
F Check failed: mutator->IsVaildCJThread()
F Get invalid mutator
```

检查位于 `src/CompilerCalls.cpp:830-833`。该用例从 finalizer 相关上下文调用快照接口，当前调用上下文不是有效 CJThread mutator。

**处置建议：**明确 `dumpCurrentThread` 是否支持 finalizer/native 线程：若支持，接口需跳过 CJThread-only saferegion 操作或使用安全的 native-thread 快照路径；若不支持，std 接口应返回受控错误并为用例增加平台/线程上下文限制，不能保留 `CHECK` 终止。

## D. MSGC 与 CopyGC 用例契约差异

### D1. `cjStackSize=64kb` 不再满足当前下限（8 项）

`stack001`、`stack002`、`stack201`、`stack202`、`stack209`、`stack210`、`stack212`、`stack213` 均包含 `cjStackSize=64kb`。实际 runtime 报：

```text
Unsupported cjStackSize parameter. Valid range is >= 128KB and <= 1048576KB.
```

这些 case 随后仍会触发栈溢出，但额外参数错误改变了预期输出，导致比较失败。

**处理建议：**在 Windows HLT 配置中把 64KB 改为当前下限 128KB（单位统一为 `KB`/`MB`），然后核对可见栈帧数量；若 64KB 是 ABI/栈保护页边界的历史契约，应先恢复该契约并增加参数范围回归，而不是只修改断言。

### D2. CopyGC 参数导出用例与 MSGC 参数模型不匹配（30 项受框架错误遮蔽）

`gc_export_001..025.info` 及 `gc_env_macro` 组首先受 `cat` 缺失影响；日志同时显示多项旧参数被拒绝或钳制，例如 `cjHeapSize`、`cjRegionSize`、`cjGCInterval`、`cjGarbageThreshold`、`cjProcessorNum`。这些用例的断言使用 CopyGC 风格的日志和参数语义（如固定 heap/region/GC threshold），不能直接作为 MSGC 行为断言。

**处理建议：**先完成 B 类 Windows 文件读取修复；随后把此组拆成两套：

1. 参数校验测试：保留非法值，断言 MSGC 的拒绝/默认值行为；
2. MSGC 支持参数测试：仅设置已支持的参数，断言 `[MS-GC]` 结构化日志或查询 API。

不要为通过旧 CopyGC 文本断言而恢复不适合 MSGC 内存策略的参数语义。

### D3. `SetGCThreshold` 的语义已改变（1 项）

`gc_export_029.cj` 期望 `gcThreshold is set to 234234234 KB.`，而运行时输出：

```text
Ignore requested gcThreshold=234234234 KB; gcThreshold is derived from current heap size.
```

这是当前设计的明确行为：`CangjieRuntime::SetGCThreshold` 会调用 `SyncGCThresholdWithHeapSize`，`src/CjScheduler.cpp` 也说明 MSGC 的 threshold 由当前堆大小导出。

**处理建议：**把该 HLT 的 MSGC 分支改为断言“忽略显式阈值并按堆大小派生”；若产品要求 API 保持 CopyGC 兼容，则需要单独设计 MSGC 中外部阈值与自动策略的优先级，不能只改日志字符串。

## E. 超时与 Finalizer/CJThread 调度问题

### E1. 超时（18 项）

| 子集 | 数量 | 初步判断 |
| --- | ---: | --- |
| Finalizer 同步原语 | 11 | `Barrier`、mutex、semaphore、monitor、read-write lock 与 finalizer/GC 并发交互未完成；可能是通知遗漏、finalizer 未调度或 CJThread park/ready 卡住。 |
| OOM 环境 | 3 | `oom_env_04/05/14` 在 OOM/heap dump 组合下超时；需先应用 C1 修复后复测。 |
| Throwable | 3 | 异常抛出与收尾路径未结束；可能与 C2 栈映射问题相关，不能单独定性。 |
| Stack | 1 | `stack006` 使用 1GB 栈，需先检查资源限制与测试超时阈值。 |

**处理建议：**每类先选一个最小 case，设置 `MRT_REPORT`，采集主线程、finalizer、GC worker 的状态；同时记录 Barrier/锁等待者、CJThread 状态和最后一个 GC phase。Finalizer 用例必须在运行时间超过框架阈值时立即保存现场，不能只延长 timeout。

### E2. Finalizer sleep 功能不符合预期（1 项）

`Finalizer/sleep.cj` 未超时但测得 `sleep(Duration.second * 5)` 仅为 `7us400ns`。`MRT_Sleep` 调用 `TimerSleep`，后者通过 `CJThreadPark` 和 timer 回调恢复线程；现有证据表明实际等待没有发生，但尚不足以确定是 duration 传递、Timer 创建/释放，还是 finalizer 上下文调度问题。

**处理建议：**在 `TimerSleep`、`TimerSleepCoparkCallback` 和 `TimerSleepCoreadyCallback` 记录 duration、timer handle、`CJThreadPark` 返回码与回调时刻；对照普通 CJThread 与 finalizer 上下文各跑一次。只有确认 timer 未入队、被提前释放或 park 被错误唤醒后，再修改 CJThread timer 实现。

## 修复优先级与复测顺序

1. **测试框架**：Windows 原生替换 `bash/dos2unix`、`cat`、`grep`，补齐 clang；为不支持平台特性的用例标记 `SKIP`。这一步预计能消除或显露 227 项（105 FAIL + 122 UNRESOLVED）的真实状态。
2. **已修 OOM 分配链**：用最新 MSGC runtime 重跑 C1 的 3 项及所有 OOM 用例，确认不再有 `MSBitmapArena ... CHECK`。
3. **栈映射 correctness**：先复现 C2 的一个 OOM 和一个 Throwable case，保存 compiler 临时产物，修复 Windows x86_64 stackmap 后批量回归 17 项。
4. **参数契约**：分别维护 MSGC 参数测试与 CopyGC 兼容性测试，先处理 8 个 stack-size case 和 `gc_export_029`。
5. **Finalizer/CJThread**：对 11 个同步超时和 sleep case 做最小现场采集，再决定是 runtime 修复还是调整测试超时/平台限制。

## 附：分类明细索引

| 分类 | 数量 | 代表用例 | 原始证据 |
| --- | ---: | --- | --- |
| 未执行 | 122 | Demangle、Native、exclusiveScope | `commands=[]`、无返回码 |
| 框架/工具 | 105 | ouroboros、gc_export、stack002、Native case32 | WSL/bash、cat、grep、clang 缺失 |
| OOM bitmap fatal | 3 | `oom_env_01`、`oom_env_10`、`gc_concurrency_09` | `MSBitmapArena commit failed` |
| stackmap/展开 fatal | 17 | `oom_env_02`、`Throwable/013` | `register rbx/rsi is not recorded` |
| 无效 Mutator | 1 | `Finalizer/dump_thread` | `Get invalid mutator` |
| 栈参数不兼容 | 8 | `stack001`、`stack201` | `cjStackSize >= 128KB` |
| GC 参数/API 契约 | 31 | gc_export 系列、`gc_export_029` | 拒绝旧参数/忽略显式阈值 |
| 超时 | 18 | Finalizer 同步、OOM、Throwable | `TimeOut` |
| CJThread sleep 行为 | 1 | `Finalizer/sleep` | 5 秒请求实际仅数微秒 |

说明：`gc_export` 的 30 个参数用例当前首先属于 B 类 `cat` 框架失败；附表把它们同时作为 D2 的后续契约工作量统计，因此该行不能与其他行再次相加。
