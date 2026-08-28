# Region-scan MSGC Compiler 用例清单与分类

## 范围与统计口径

测试仓库：`/Users/lipeisheng/code/cangjie/cangjie/cangjie_test`。

Compiler 用例位于：

- `testsuites/HLT/compiler`
- `testsuites/LLT/compiler`

目录中的 `.cj` 文件并不全部是独立测试：多包、增量编译和诊断用例会包含依赖源码、输入文件、期望文件或
旧版本基线。因此本文同时记录 `.cj` 文件数和含 `EXEC` 指令的源文件数；后者用于近似描述可由测试框架
调度的测试源数量，不等同于最终实际调度数（条件分支会影响实际执行）。

| 套件 | `.cj` 文件数 | 含 `EXEC` 指令的测试源 | 定位 |
| --- | ---: | ---: | --- |
| HLT / compiler | 41,700 | 20,832 | 高层语言特性、Cangjie Native 编译、链接和运行的端到端验证。 |
| LLT / compiler | 21,606 | 10,606 | 编译器前端、语义、IR、优化、代码生成和驱动的单元/模块回归。 |
| 合计 | 63,306 | 31,438 | — |

HLT 的默认 `testlist` 已包含 `compiler/`；LLT 的 API 执行场景需要专门的测试列表时，Compiler 同样应
显式填写 `compiler/`，避免目录选择差异影响统计。

## HLT / compiler

HLT 分为 `cjnative` 与 `cli`：前者编译并大多运行 Cangjie Native 程序，后者验证 `cjc` 驱动选项和
命令行行为。

| 分组 | 含 `EXEC` 的测试源 | 主要测试内容 |
| --- | ---: | --- |
| `stable_abi` | 4,389 | ABI 稳定性、跨模块和跨版本调用兼容性、符号/布局约束。 |
| `TypesAndVariables` | 3,273 | 类型系统、变量、数组、可变性、赋值与生命周期相关语言语义。 |
| `MetaProgramming` | 3,028 | 宏、quote、编译期元编程和生成代码的编译/运行行为。 |
| `Expressions` | 2,003 | 控制流、表达式求值、异常及模式匹配等语义。 |
| `Generics` | 1,917 | 泛型函数、泛型类型、约束、类型推导与实例化。 |
| `Class` | 1,455 | 类、继承、成员、构造与重写规则。 |
| `Packages` | 1,286 | 包导入、跨包可见性、多源文件和构建组织。 |
| `Extend` | 894 | 扩展定义、成员解析和跨包扩展。 |
| `Function` | 822 | 函数调用、闭包、参数、返回值和函数类型。 |
| `FFI` | 781 | C/Native FFI、链接、导出和互操作。 |
| `Chir` | 183 | CHIR 相关编译行为和 IR 路径。 |
| `OperatorOverloading` | 164 | 运算符重载及其语义/代码生成。 |
| `Property`、`TypeInference` 等 | 216 | 属性、类型推导、词法结构、访问控制、注解、枚举与诊断。 |
| `LTO` | 38 | 链接时优化与最终可执行产物。 |
| `cli/Option` | 297 | `cjc` 选项，例如 `--compile-as-exe`、优化和目标相关开关。 |
| `cli/command_injection` | 28 | 驱动命令拼接和安全边界。 |

典型用例形式是 `EXEC: %compiler ...` 后跟 `RUN-EXEC: %run ...`，因此能同时发现编译、链接、装载和
运行期问题。

## LLT / compiler

LLT 按编译器子系统组织，适合定位某个编译阶段或诊断变化。以下为含 `EXEC` 的测试源数量。

| 分组 | 测试源 | 主要测试内容 |
| --- | ---: | --- |
| `Sema` | 2,441 | 名称解析、类型检查、泛型约束、可见性和语义规则。 |
| `IncrementalCompilation` | 1,975 | 声明/类型/API 变更后的依赖失效、增量重编译和缓存一致性。 |
| `CHIR` | 1,715 | CHIR 构建、约束、包/继承语义和 IR 正确性。 |
| `ConstEval` | 714 | 常量求值、编译期异常、溢出和边界。 |
| `Macro` | 688 | 宏展开、quote、源位置信息和宏诊断。 |
| `Driver` | 440 | 编译参数、工具链、路径归一化、分离编译和链接驱动。 |
| `CodegenCJNative` | 435 | Native 代码生成、元数据、全局初始化、扩展定义及目标平台行为。 |
| `Parser` | 404 | 语法、歧义消解、字符串、包声明和错误恢复。 |
| `FFI` | 355 | C FFI 类型映射、导入/导出和链接。 |
| `Diagnose` | 291 | 错误/警告文本、位置、JSON/文本诊断和预期输出。 |
| `CodegenCommon` | 213 | 各后端共享的代码生成逻辑。 |
| `CHIROpt` | 213 | CHIR 优化、分析框架和变换正确性。 |
| `Modules` | 194 | 模块导入、包解析和跨模块符号。 |
| `Mangle` | 126 | 符号改名、泛型和跨包符号兼容；`DeMangle` 另有 31 项。 |
| `ConditionalCompilation` | 115 | 条件编译、平台宏和 quote 条件。 |
| `CJMP` | 79 | common/platform 源合并、平台成员与接口匹配。 |
| `CodegenCHIR` | 66 | CHIR 到后端代码生成的专项回归。 |
| `CJCov` | 63 | 覆盖率插桩和相关产物。 |
| `Lexer` | 15 | Token 化与词法边界。 |
| `Package`、`Plugins`、`Frontend`、`CHIROptTest` | 33 | 包专项、插件、前端回归和优化期望基线。 |

LLT 用例常用 `EXEC-PIPE-*`、`RUN-EXEC-PIPE-*` 与 `compare`/`ASSERT`，可分别验证：

- 编译应成功还是失败；
- 诊断文本、错误位置和错误数是否符合预期；
- 目标程序运行输出和退出码；
- 生成符号、IR、链接产物或增量编译结果。

## Region-scan MSGC 的优先执行范围

在使用 MSGC SDK 时，所有 Cangjie 编译命令必须显式带 `--gc-mode=marksweep`。按与当前 Region-scan
Runtime/LLVM 改动的相关程度，建议优先级如下：

| 优先级 | 分组 | 原因 |
| --- | --- | --- |
| P0 | `LLT/compiler/CodegenCJNative`、`CHIR`、`CHIROpt`、`CodegenCHIR` | 直接覆盖 Native 后端、IR、优化、元数据和可能的 GC 相关 lowering。 |
| P0 | `HLT/compiler/cjnative/stable_abi`、`FFI` | 覆盖 Runtime ABI、符号、C 边界与链接装载。 |
| P1 | `LLT/compiler/Driver`、`HLT/compiler/cli` | 验证 MSGC SDK 的参数传递、工具链、链接和产物安装。 |
| P1 | `HLT/compiler/cjnative/Packages`、`Extend`、`Generics` | 覆盖跨包元数据、泛型实例化和扩展成员的端到端路径。 |
| P2 | `Sema`、`Parser`、`Lexer`、`Diagnose`、`Macro` | 主要验证前端语言规则和诊断；仅在编译器前端有对应改动时优先。 |
| P2 | `IncrementalCompilation`、`CJMP`、`ConditionalCompilation` | 覆盖编译缓存、跨平台合并和条件编译，适合作为专项回归。 |

## 全量历史基线（2026-08-04 ～ 2026-08-05）

本节保留首次全量执行结果，作为覆盖范围和问题来源的历史基线。它是在修复
`--common-part-chir` 兼容参数及移除 `[cj-compiler-rebuilt]` 输出**之前**取得，不能作为当前 SDK 的
正确率；当前可复核的正确率和未解决项见后文“当前有效结论”。

两套用例均使用 Region-scan MSGC SDK，且框架实际下发的所有 Cangjie 编译命令均显式包含
`--gc-mode=marksweep`。下表的“总数”是测试框架最终调度数，不是前文含 `EXEC` 源文件数：一个源文件可按
平台条件、分阶段命令或 `.info` 指令生成多个任务。

| 套件 | 最终日志 | PASS | FAIL | 通过率 | 最终框架日志 |
| --- | ---: | ---: | ---: | ---: | --- |
| HLT / compiler | 29,052 | 27,944 | 1,108 | 96.19% | `/private/tmp/msgc-compiler-hlt2-20260804/Maple_Test.log` |
| LLT / compiler | 15,879 | 10,184 | 5,695 | 64.14% | `/private/tmp/msgc-compiler-llt2-20260804/Maple_Test.log` |
| 合计 | 44,931 | 38,128 | 6,803 | 84.86% | — |

### LLT / compiler：5,695 FAIL

LLT 的失败统计按日志签名归类；同一用例可同时命中多个类别，因此各行**不可相加**。完整的逐类证据、真实
崩溃候选及子系统分布见
[`2026-08-04_msgc_llt_compiler_failure_analysis.md`](2026-08-04_msgc_llt_compiler_failure_analysis.md)。

| 分类 | 命中数 | 日志证据与结论 | 当前处置 |
| --- | ---: | --- | --- |
| CJMP 接口/功能版本不匹配 | 2,181 | 主要为 `invalid option: '--common-part-chir'`；另含 `--enable-interop-cjmapping`、`--debug-annotations` 等。当前分支支持位置 `.chir` 输入和 `--common-part-cjo`，但不接受测试仓使用的新接口。 | 兼容别名或批量调整 CJMP 测试命令；未完成前不计为 MSGC Runtime 缺陷。 |
| 重建标记污染严格比较 | 2,940 Compare Fail | `cjc` 在 `src/main.cpp:106` 无条件输出 `[cj-compiler-rebuilt]`；2,968 个 Compare Fail 中 2,940 个命中该文本。 | P0：删除或改为显式调试开关后重跑比较类用例。 |
| SDK 可选包缺失 | 562 | `java.lang`、`objc.lang`、`ffi_python`、`std.reflect` 等包缺失。 | 按交付范围补包，或在 macOS MSGC 配置中条件排除。 |
| 测试配置/静态链接适配 | 46 | 未定义 `%enableSwitchOpt` / `%disableCHIROpt` 27 项；静态库 `-l` 参数拼接错误 19 项。 | P1：补测试宏并修正 macOS static-link 参数。 |
| Runtime 生命周期日志污染 | 53 Compare Fail | 输出含 `MarkSweepCollector::Fini()` 或 `Cangjie runtime shutdown.`。 | P1：降低 Runtime 测试日志等级后重跑。 |
| 超时 | 13 | 框架 timeout 日志。 | 单例串行复核；当前不直接归因于 MSGC。 |

除上述前置问题外，LLT 已发现 9 个需要优先核查的真实问题候选：4 个
`CJ_MCC_NewArrayGenericFast3_MSGC` 的 `musttail`/LLVM verifier 失败、4 个运行时 SIGSEGV、1 个
metadata `funcTable is nullptr` SIGABRT。它们均仅完成日志级定位，尚未完成最小复现和源码级根因闭环。

### HLT / compiler：1,108 FAIL

HLT 采用互斥的“首要失败原因”分类，以下计数**可相加为 1,108**。`[cj-compiler-rebuilt]` 在 953 个 HLT
失败日志中出现，但其中大多数同时有更直接的参数、缺包或测试前提错误，因此没有把 953 全部误算为标记问题。

| 首要分类 | FAIL | 直接证据 | 判断与建议 |
| --- | ---: | --- | --- |
| 未支持的测试接口参数 | 547 | 395 项使用 `--common-part-chir`；152 项使用 `--objc-interop-output-dir`。 | 分支/测试仓接口不同步。CJMP 参数可映射到现有位置 `.chir` 输入，或给 `cjc` 加兼容别名；Objective-C 组应在未交付对应互操作能力时跳过。 |
| SDK 未交付 `std.reflect` | 334 | `can not find package 'std.reflect'`；集中在 stable ABI 的 `add_virtual_function`（265）和 `package_name`（68）组。 | SDK 内容或测试配置问题，不是 Region-scan GC 结论。补包或条件排除。 |
| 仅受重建标记破坏的严格比较 | 48 | `Compare Fail` 且除内部 `[cj-compiler-rebuilt]` 外没有上述参数/缺包前置错误。 | P0 测试可用性问题；移除或门控该 stderr 标记后重跑。 |
| 条件编译 builtin 值差异 | 126 | `arch != "arm"` 被拒绝，诊断显示当前仅支持 `x86_64 aarch64`。 | 测试使用的 `arm` 别名与当前 Compiler 规格不一致；确认规格后兼容该别名或更新测试。 |
| CLI 行为/预期基线差异 | 36 | `debug_macro` 13 项为诊断文本比较差异；LTO 11 项、`output_type` 8 项、APC 2 项的预期退出码/产物与当前行为不同；另有 2 项包/CJMP 期望差异。 | 多数是 Driver/测试基线适配，先在移除重建标记后重新量化；不是 MSGC Runtime 崩溃。 |
| 外部工具未随 SDK 安装 | 14 | 12 个 obfuscation 用例找不到 `$CANGJIE_HOME/tools/bin/cjtrace-recover`；另 2 项同类 `No such file or directory` 前置失败。 | 补安装工具或将依赖该工具的用例条件排除。 |
| 运行时 SIGSEGV 候选 | 2 | `LTO/regression/sample5/index.cj` 与 `TypesAndVariables/.../list_00815.cj` 均编译成功、运行 exit 139；栈顶为 `CJ_MCC_ArrayCopyGeneric`，分别经 `ArrayList.grow`、`Array.clone` 到达。 | P0：疑似真实 MSGC Runtime 数组复制问题；需以单例建立最小复现后再修复。 |
| Compiler plugin 链接失败 | 1 | `Chir/CompileOption/plugin/runplugin/plugin5` 构建 dylib 时 `vtable for Plugin1` 未定义。 | P1：插件示例源码/SDK 插件 ABI 或链接约定问题，与 GC 无直接证据。 |

HLT 失败的主体是接口、SDK 包、外部工具和测试基线差异（1,106 项）；目前具有明确运行期崩溃证据的为上表 2 项
`CJ_MCC_ArrayCopyGeneric` 路径。该结论只代表本次 macOS/aarch64、当前 SDK 与当前测试仓组合；修复前必须
保留单例复现和修复后回归证据。

## `--common-part-chir` 兼容与重建标记回归（2026-08-05）

本轮修改并通过固定脚本
`runtime/build/scripts/incremental_rebuild_msgc_sdk.py` 重建、安装了 Region-scan MSGC SDK。编译器侧的改动为：

- 新增稳定兼容参数 `--common-part-chir=<path>`，复用 `.chir` 输入的路径校验和记录逻辑，并使其满足
  `--common-part-cjo` 的公共 CHIR 输入要求；
- 删除 `src/main.cpp` 中无条件输出的 `[cj-compiler-rebuilt]`，避免污染严格诊断/输出比较。

所有回归命令均显式使用 `--gc-mode=marksweep`。首次重跑发现 CJMP 的 dylib 阶段没有传入 macOS SDK，导致
`ld64.lld` 以 `-syslibroot /` 查找 `-lSystem`；这属于测试启动配置，不能计为编译器失败。修正为
`--sysroot $(xcrun --show-sdk-path)` 后，以下结果才是有效统计；最终目标日志中
`[cj-compiler-rebuilt]`、`invalid option: '--common-part-chir'` 和 `library not found for -lSystem` 均为 **0**。

| 回归范围 | 历史选择口径 | 总数 | PASS | FAIL | 结论 |
| --- | --- | ---: | ---: | ---: | --- |
| LLT：重建标记污染 | 历史 FAIL 日志同时含 `[cj-compiler-rebuilt]` 和 `[Compare Fail]` | 2,940 | 2,492 | 448 | 标记污染已消除；剩余失败为原有比较/功能差异。 |
| LLT：CJMP 兼容参数 | 历史日志可直接复建的 `invalid option: '--common-part-chir'` 用例 | 2,081 | 2,023 | 58 | 别名已被正确接受，绝大部分 CJMP 用例恢复通过。 |
| LLT：上述去重并集 | 两组交集 408 个 | 4,613 | 4,119 | 494 | 有效全量定向回归结果。 |
| HLT：CJMP 兼容参数 | 历史日志直接命中的 `--common-part-chir` 用例 | 395 | 393 | 2 | 参数兼容问题已消除。 |

旧统计中的“2,181 个 CJMP 接口/功能版本不匹配”是聚合类别，除 `--common-part-chir` 外还包括其他选项；保存的
LLT 历史失败日志可精确复建其中 2,081 个直接命中别名的用例。因此本轮以可追溯的 2,081 个 LLT 用例和 395 个
HLT 用例为准，没有把其余聚合项误标为该别名的回归结果。

### 剩余失败归类

LLT 的 494 个剩余失败全部位于这两个历史选择集内。其中 442 个仍是 `[Compare Fail]`，主要集中在
`CJMP/java`（73）、`Macro/sema`（45）、`IncrementalCompilation/DeclLevel`（40）、`CJMP/objc`（34）和
`CJMP/frontend`（29），属于诊断文本、语义规则、宏/增量编译或优化基线差异，不能归因于本次参数兼容改动。
另有 17 个日志含 compiler internal error，需后续单例核查；2 个出现 exit 139，分别是
`LLT/compiler/CJMP/compilation/verify_options_o.cj` 与
`LLT/compiler/CJMP/merging/static_and_top_level_variables/static_private_non_cs_variable.cj`，应作为独立崩溃问题处理。

HLT 剩余的 2 个失败为 `cjmp_04046`、`cjmp_11045` 的诊断基线不一致：实际诊断是
`common declaration must be defined in common package part`，而测试分别期待构造器 `common` 修饰符错误和声明重定义。
它们确认参数已经被解析并完成到 CJMP 语义检查，属于规格/诊断基线差异，不是兼容别名或链接配置问题。

## 当前有效结论（2026-08-05）

### 正确率口径

当前没有在最新源码上重新执行完整的 44,931 项 Compiler 套件，因此不能把修复前全量基线的 **84.86%**
写成当前正确率。当前唯一已使用重建安装后的 MSGC SDK、且完整可复核的结果，是针对“重建标记污染”和
`--common-part-chir` 的去重定向回归：LLT 4,613 项加 HLT 395 项，共 5,008 项。

| 口径 | 总数 | 通过 | 失败 | 正确率 | 说明 |
| --- | ---: | ---: | ---: | ---: | --- |
| 修复前全量历史基线 | 44,931 | 38,128 | 6,803 | 84.86% | 仅用于保留全量覆盖和历史问题规模；不是当前结果。 |
| 当前已验证的定向回归 | 5,008 | 4,512 | 496 | **90.10%** | 使用固定 SDK 重建脚本安装后的产物；LLT/HLT 两个选择集互不重叠。 |

已确认消除的前置失败为：`[cj-compiler-rebuilt]`、`invalid option: '--common-part-chir'` 和 dylib 阶段
`library not found for -lSystem` 均为 0。因而 90.10% 表示**已重跑的受影响用例集**的正确率，不能外推为
完整 Compiler 套件的当前正确率；完整正确率需要在当前源码重建 SDK 后再次全量运行。

### 当前失败用例与原因

定向回归中 LLT 有 494 个唯一失败、HLT 有 2 个唯一失败。下表的“命中数”是日志签名数，比较失败、ICE 和
崩溃可在同一测试中重叠，不能相加为 494。

| 套件/类别 | 命中或用例 | 当前原因 | 处置判断 |
| --- | ---: | --- | --- |
| LLT：诊断、语义与输出比较差异 | 442 Compare Fail | 主要为 `CJMP/java`（73）、`Macro/sema`（45）、`IncrementalCompilation/DeclLevel`（40）、`CJMP/objc`（34）、`CJMP/frontend`（29）；其余分散在旧诊断、语义、宏和增量编译基线。 | 多数是测试仓与当前 Compiler 的规格/诊断基线差异；需逐类确认，现不归因为 MSGC Runtime。 |
| LLT：重复 exportID ICE | 14 | `3step_enum`、`4step_function`、8 个 `merging_initializers*`、3 个 `verify_feature_subset*`、`verify_options_g` 在多父包初始化/feature-options 校验中报 `Found same exportID when export the package`。 | Compiler 问题。源码已有本地过滤修改，但尚未按要求重建 SDK，故仍计为未验证失败。 |
| LLT：CJMP 类型名空指针 ICE | 1：`CJMP/frontend/extend/positive/constraints/all_type_kind.cj` | `GetTypeNameFromTy` 对无关联声明的内建/复合类型调用 `GetDeclOfTy`，触发 `null pointer`。 | Compiler 崩溃；按当前决定暂不处理，仍计入失败。 |
| LLT：CJMP SIGSEGV（exit 139） | 2：`CJMP/compilation/verify_options_o.cj`、`CJMP/merging/static_and_top_level_variables/static_private_non_cs_variable.cj` | 前者在 `-O2` 编译阶段崩溃，后者在主包编译阶段崩溃；共同日志为 signal 11。 | Compiler 崩溃候选；按当前决定暂不处理，仍计入失败。 |
| LLT：CJMP 原生链接未定义符号 | 受相关选择集影响，未单独重新计数 | `ld64.lld` 缺少 `__CGP...` / `__CN...`，发生在 common/platform 扩展、导入和静态成员合并路径。 | Compiler/Driver 链接依赖传递问题。源码已有本地修改，未重建验证；不能从通过数扣除。 |
| HLT：CJMP 诊断基线 | 2：`cjmp_04046`、`cjmp_11045` | 实际为 `common declaration must be defined in common package part`，与用例期待的构造器修饰符/重定义诊断不同。 | 规格或诊断基线差异，不是参数兼容、链接配置或 MSGC Runtime 问题。 |

因此，当前已验证集合中的 496 个失败可归为两类：一类是 442 个为主的规格、诊断、宏或增量编译基线差异；
另一类是少量真实 Compiler 问题候选（重复 exportID、CJMP 类型名空指针、SIGSEGV 与原生链接依赖）。本轮明确
不处理的两个 SIGSEGV 和一个空指针 ICE 保留在失败统计中，不会被误写为通过。重复 exportID 与原生链接依赖
仅完成源码修改，尚未使用 `incremental_rebuild_msgc_sdk.py` 重建、安装和回归，因此同样不能计入已修复。

## 执行注意事项

- 使用 SDK：`/Users/lipeisheng/code/cangjie/cangjie/cangjie_compiler_region_mark/output/msgc`。
- 用 MSGC SDK 验证 Cangjie 程序时必须使用 `--gc-mode=marksweep`，避免默认 CopyGC 产物与 marksweep
  Runtime 混用。
- HLT 的 `compiler/` 默认已经在测试列表中；LLT 需要创建/选择测试列表时，显式放入 `compiler/`。
- 多包、增量编译、CLI 与 FFI 分组往往依赖工作目录、原生工具链或额外源文件；不能按单个 `.cj` 文件
  直接调用 `cjc` 替代框架执行。
