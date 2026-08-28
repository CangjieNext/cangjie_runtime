# Region-scan MSGC LLT / compiler 失败分析

> 状态更新（2026-08-05）：本文的 15,879 项/64.13% 是修复前的全量历史基线。移除
> `[cj-compiler-rebuilt]` 并兼容 `--common-part-chir` 后，已验证的 LLT 定向回归为 4,613 项、
> 4,119 项通过、494 项失败（89.29%）；合并 HLT 对应回归后的当前可复核集合为 5,008 项、
> 4,512 项通过、496 项失败（90.10%）。完整的当前口径、失败清单和未验证本地修改见
> [`2026-08-04_msgc_compiler_test_inventory.md`](2026-08-04_msgc_compiler_test_inventory.md) 的
> “当前有效结论”。

## 历史结论摘要

本次以 macOS/aarch64 Region-scan MSGC SDK 运行 `LLT/compiler`，所有经测试框架下发的 Cangjie 编译命令均
显式包含 `--gc-mode=marksweep`。最终结果：15,879 项，PASS 10,184，FAIL 5,695，通过率 64.13%。

5,695 个失败不能等同于 5,695 个 Compiler/Runtime 缺陷。失败首先被三个前置问题放大：

1. 当前 Compiler 分支不支持 CJMP common/platform 编译参数；
2. `cjc` 无条件输出内部重建标记，破坏严格的诊断/输出比较；
3. SDK 未提供 Java、Objective-C、Python FFI 等专项包。

清除上述前置问题前，不应按剩余 FAIL 总数评估 Region-scan 正确性。已从日志中分离出 **9 个需要优先解决的
真实崩溃/IR 校验候选**：4 个 Fast3 `musttail` IR 校验失败、4 个运行时 SIGSEGV、1 个 metadata funcTable
空指针 SIGABRT。

## 执行范围与证据

- SDK：`cangjie_compiler_region_mark/output/msgc`
- 测试：`cangjie_test/testsuites/LLT/compiler`
- 最终框架日志：`/private/tmp/msgc-compiler-llt2-20260804/Maple_Test.log`
- 单用例日志：`/private/tmp/msgc-compiler-llt2-20260804/*.log`
- 最终汇总：`Total: 15,879, PASS: 10,184, FAIL: 5,695`

以下分类按 5,695 个框架判定 FAIL 的日志去重统计。同一用例可能同时命中“输出标记”和“诊断基线”等类别，
因此各行计数不能相加。

## 失败分类

| 分类 | 命中数 | 直接证据 | 判断 | 是否需要在当前 MSGC 分支解决 |
| --- | ---: | --- | --- | --- |
| CJMP 功能/参数缺失 | 2,181 用例 | 2,532 次 `invalid option: '--common-part-chir'`；另有 `--enable-interop-cjmapping` 84 次、`--debug-annotations` 18 次等。 | 当前 `cjc` 不接受 CJMP common/platform 相关选项，CJMP 大量用例无法开始语义验证。 | 否，除非本分支目标包含 CJMP；应合入对应 Compiler 特性或将该组列为未支持。 |
| 编译器内部标记污染输出 | 2,940 个 Compare Fail | 2,968 个 Compare Fail 中 2,940 个含 `[cj-compiler-rebuilt]`。该文本在 `cangjie_compiler_region_mark/src/main.cpp:106` 无条件写入 stderr。 | 严格诊断比对把内部调试文本当成用户输出，造成大面积假失败。 | 是，P0 测试可用性问题；删除或改为显式调试开关后需重跑比较类用例。 |
| SDK/可选语言包未交付 | 562 | `java.lang` 383、`objc.lang` 335、`ffi_python` 142、`std.reflect` 35 等（同一日志可出现多个包）。 | Java、Objective-C、Python FFI 与部分标准包不在当前 MSGC SDK 覆盖内。 | 否，除非本轮交付目标包含这些组件；应补包或条件排除。 |
| 测试配置占位符缺失 | 27 | `%enableSwitchOpt` 25、`%disableCHIROpt` 2 被原样传给 `cjc`。 | macOS/aarch64 LLT 配置未定义测试需要的优化宏。 | 是，P1；补齐测试配置宏后重跑 CHIROpt 分组。 |
| 静态库/链接参数问题 | 19 | `library not found for -llibcangjie-std-coreFFI.a` 等。SDK 文件树中实际存在 `libcangjie-std-coreFFI.a` 与 `libcangjie-std-posixFFI.a`。 | 参数把完整文件名错误地作为 `-l` 名传给链接器，或 static-link 宏未适配 macOS。 | 是，P1；修正 Driver/测试宏的静态库传参后重跑。 |
| 诊断文本与规格基线漂移 | 2,968 Compare Fail（与标记污染重叠） | Sema/Diagnose/Parser/Macro 等用例的编译错误数常正确，但注解拼写、macro note、JSON 文本或行列格式与旧期望不一致。 | 其中大部分先受内部标记影响；去掉标记后才能量化真正的语言规格/诊断回归。 | 待定；先重跑再逐项更新规格基线或修复诊断。 |
| MS-GC 生命周期日志污染 | 53 个 Compare Fail | 诊断用例输出末尾出现 `MarkSweepCollector::Fini()`、`Cangjie runtime shutdown.` 等行。 | 严格输出用例不应被 Runtime 调试日志污染。 | 是，P1；检查测试启动环境和 Runtime 日志等级。 |
| 超时 | 13 | 框架超时/timeout 相关日志。 | 多为专项编译、外部工具或运行时序，尚未逐项复现。 | 待定；按单例串行复核。 |

## 需要优先解决的真实问题候选

以下项目具有明确的编译器 IR 校验失败、运行时崩溃或 metadata 不变量失败证据，和 SDK 缺包、测试文本差异不同。
它们当前均为“已观察到、待最小复现”的候选，尚未完成源码级根因闭环。

| 优先级 | 数量 | 用例 | 观察到的证据 | 初步归属与下一步 |
| --- | ---: | --- | --- | --- |
| P0 | 4 | `Mangle/Mangle_obf/byte_array2`、`BurstHeapMemoryAllocatorBenchmark_4KB`、`BurstHeapMemoryAllocatorBenchmark_4MB`、`Driver/options/obfuscation/test_layout/test10.info` | LLVM verifier：`musttail call must precede a ret with an optional bitcast`，目标为 `CJ_MCC_NewArrayGenericFast3_MSGC`；随后 `LLVM ERROR: Broken module found`。 | Compiler/LLVM lowering 与 Fast3 调用约定不一致。该签名与 MSGC Fast3 trampoline 改动路径直接相关，应先建立最小 IR/源码复现。 |
| P0 | 1 | `CodegenCJNative/storeElementRef.cj` | 编译成功、运行时 SIGSEGV；栈顶为 `Inner.greet`，由捕获 `this.inner` 的闭包调用触发。 | Codegen/closure capture 或引用存储路径候选；需最小化到闭包捕获结构字段的程序。 |
| P0 | 1 | `CHIR/ClosureConversion/testClosureConversion10.cj` | 编译成功、运行时 SIGSEGV，PC 为 `0x0`；路径为泛型约束接口扩展 + lambda。 | CHIR closure conversion / 泛型调用代码生成候选；需查看生成 CHIR/LLVM 与空目标调用来源。 |
| P0 | 2 | `Driver/options/trim_path/trim_path4.cj`、`trim_path5.cj` | 用例预期抛出 `Exception` 并退出 1；实际运行时 SIGSEGV，栈在 `default.main`。 | 异常抛出/栈展开或相关代码生成候选，和 trimpath 参数本身无直接因果证据。 |
| P0 | 1 | `Sema/box/option_box_assign_01.cj` | `funcTable is nullptr, ti: std.core:Option<std.core:String>, itf: stdx.encoding.json.stream:JsonDeserializable<...>` 后 SIGABRT。 | metadata/interface table 构建或 stdx 链接组合候选；需在不附带无关 stdx 库的最小程序中复现。 |

## 按子系统的失败分布

| 子系统 | FAIL |
| --- | ---: |
| CJMP | 2,634 |
| Sema | 1,103 |
| Diagnose | 781 |
| FFI | 260 |
| CHIR | 191 |
| Parser | 189 |
| Macro | 189 |
| CHIROpt | 82 |
| Driver | 71 |
| CodegenCJNative | 58 |
| IncrementalCompilation | 48 |
| 其余（Modules、ConditionalCompilation、CodegenCommon、Mangle 等） | 90 |

CJMP 的数量主要由 `--common-part-chir` 未支持造成；Sema/Diagnose/Parser/Macro 的比较失败主要先受
`[cj-compiler-rebuilt]` 输出污染影响，不能在当前基线上直接判定语言语义错误。

## 建议处置顺序

1. **先移除或门控 `EmitCompilerRebuildMarker` 的 stderr 输出**，再重跑 Compare Fail 分组；这是消除 2,940
   个假失败的前提。
2. **修复 Fast3 的 `musttail` IR 约束**，它会直接阻断 `--fobf-all` 与相关后端路径。
3. **分别最小复现 4 个 SIGSEGV 与 1 个 funcTable SIGABRT**；确认是 Region-scan codegen/Runtime 回归后再改源码。
4. 补齐 `enableSwitchOpt`/`disableCHIROpt` 宏和 static-link 参数，恢复 CHIROpt、LTO、VFE、增量编译的有效覆盖。
5. 将 CJMP、Java、Objective-C、Python FFI 明确标为“当前 SDK/分支未支持”；只有相应特性合入或 SDK 补包后再纳入通过率。

## 结论边界

本报告未修改 Compiler、LLVM、Runtime 或测试源码。崩溃和 IR verifier 项仅完成日志级聚类与调用路径初判；
修复前必须先在当前 SDK/源码版本上建立单例最小复现，并确认具体生成 IR、metadata 或 Runtime 调用链。
