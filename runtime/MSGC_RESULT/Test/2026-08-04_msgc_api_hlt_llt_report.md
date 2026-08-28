# Region-scan MSGC API 用例执行报告

## 测试范围与执行口径

测试环境为 macOS/aarch64，SDK 为：
`/Users/lipeisheng/code/cangjie/cangjie/cangjie_compiler_region_mark/output/msgc`。
所有 Cangjie 编译命令均显式传入 `--gc-mode=marksweep`，运行时库和 stdx 均来自该 SDK。

本报告将 HLT/API 与 LLT/API 分开统计。用例数采用“包含 `EXEC` 编译指令的 `.cj` 测试源”口径；
目录中的其余 `.cj` 文件可能是依赖源码、测试输入或资源，不能直接作为独立执行用例计数。

| 套件 | API `.cj` 文件 | 含 `EXEC` 指令的测试源 | 主要覆盖范围 |
| --- | ---: | ---: | --- |
| HLT / API | 10,718 | 9,079 | `std`、syntax、net、crypto、encoding、actors、OpenSSL 等高层 API 组合验证。 |
| LLT / API | 4,008 | 3,509 | `std` 与 `stdx` 的模块级 API、unittest、libast、CHIR、网络和语法工具。 |
| 合计 | 14,726 | 12,588 | — |

### 重点模块分布

- HLT/API：`std` 6,992，syntax 732，net 617，crypto 275，encoding 263，Regression 98，actors 26。
  `std` 内部以 ast（2,552）、collection（604）、overflow（492）、FFI（394）、I/O（374）、
  reflect（372）、unittest（340）、sync（274）为主。
- LLT/API：`std` 2,409，`stdx` 1,100。`std` 以 unittest（905）、libast（700）、socket（133）、
  sync（116）为主；`stdx` 以 net（453）、chir（253）、syntax（128）、encoding（110）为主。

## 执行方法

- 测试框架：`/Users/lipeisheng/code/cangjie/cangjie/.coverage-runtime-macos/cangjie_test_framework/main.py`
- HLT 配置：`testsuites/HLT/configs/cjnative/mac_aarch64-mac_aarch64/basic.cfg`
- LLT 配置：`testsuites/LLT/configs/cjnative/cjnative_test_darwin_aarch64.cfg`
- 并发：`-j2`；HLT 与 LLT 不并行启动。
- 单用例超时：120 秒。失败后继续执行，以获得完整分类结果。
- API 测试实际使用 `darwin_aarch64_cjnative/dynamic/stdx` 作为链接/导入目录。SDK 当前未提供
  `stdx.chir`、`stdx.syntax`、`stdx.syntaxFFI`，因此通用链接列表会剔除这三项；真正依赖这些包的
  测试将归类为“SDK 缺包”，而不是误记为 Runtime 失败。

首轮 HLT/API 在父目录 `darwin_aarch64_cjnative` 作为 stdx 搜索路径时产生批量
`package_search_error`，已在约 400 个已创建日志后停止，**不纳入结果统计**。修正为动态 stdx
目录且使用与 SDK 实际产物一致的链接列表后，`HLT/API/net/HttpStatusCode/HttpStatusCode_let_37.cj`
冒烟通过。

## 最终执行结果（2026-08-04）

两套 API 已完成。框架按目录和条件实际调度的数量与前述 `EXEC` 源文件静态计数不同，以下以框架的
最终汇总为准：

| 套件 | 实际调度 | PASS | FAIL | 通过率 | 结论 |
| --- | ---: | ---: | ---: | ---: | --- |
| HLT / API | 9,595 | 8,437 | 1,158 | 87.93% | 通过主体可运行；失败以 SDK 组件/测试环境配置为主。 |
| LLT / API | 3,728 | 2,766 | 962 | 74.19% | 模块级 API 覆盖完成；失败集中在 stdx 子组件、OpenSSL 和静态链接配置。 |
| 合计 | 13,323 | 11,203 | 2,120 | 84.09% | 未见本轮已修复的 MSGC OOM 栈图寄存器问题复发。 |

框架最终汇总只报告 PASS/FAIL，未报告 UNRESOLVED 项。首轮 HLT 的错误 stdx 搜索路径、以及首轮
LLT 未将 `dynamic/stdx` 加入运行库搜索路径的结果均已停止，**不纳入上表**。

`std/unittest/GC/test_gccount_03.cj` 会自行设置 `cjHeapSize=4gb` 并持续占满一个 CPU 核；本轮已按
压力用例安全规则中断。框架总数中可能仍保留其失败记录，但它不作为 Runtime 行为结论，后续只能单独
以 `-j1` 执行。

### 失败分类与原因

下表的“日志命中”用于说明主因，类别之间可能重叠，不能与最终 FAIL 数直接相加。

| 套件 | 类别 | 日志命中 | 已确认原因 | 是否属于 MSGC Runtime 真实问题 | 建议处理 |
| --- | --- | ---: | --- | --- | --- |
| HLT | SDK/API 包或宏配置 | 773 缺包日志；363 个 `%link-python` 未展开 | 缺少 `stdx.syntax`（730）、`std.log`（41）、`std.ffi.python`（41）等包；Python 用例将 `%link-python` 原样传给 `cjc`，报“invalid file or directory”。 | 否 | 补齐 SDK 包并在测试配置定义 Python 链接宏后重跑。 |
| HLT | 网络/外部服务时序 | 少量 | 代理类用例出现 `Connection reset by peer`、读超时；依赖本机 socket 调度。 | 否，需复跑确认 | 单独串行复跑，稳定后再判断用例期望。 |
| HLT | 断言/诊断文本差异 | 16 | 例如 `WeakRef_17` 的断言失败；另有编译诊断文字期望与当前编译器输出不一致。 | 待确认 | 逐项比对预期输出，区分语言规格更新与回归。 |
| HLT | OpenSSL 静/动态库冲突 | 1 | `static_link_openssl_no_extract` 运行时提示不安全加载 `libcrypto`，随后在 crypto key 初始化中 SIGABRT。 | 否 | 统一静态 OpenSSL 与 stdx 依赖的链接策略；该问题不能归因于 GC。 |
| LLT | SDK 缺少 stdx 子组件 | 316 | `stdx.chir`（265）、`std.reflect`（41）、`stdx.syntax`（13）、plugin manager 等无法导入；SDK 中确无 chir/syntax 产物。 | 否 | 补齐对应 SDK 子包，或将该分组从当前 SDK 覆盖中排除。 |
| LLT | 测试宿主工具缺失 | 115 | `stdx/syntax/testVisitor` 用例调用 `dos2unix`，macOS 测试机未安装该工具，统一返回 127。 | 否 | 安装 `dos2unix` 或改用跨平台换行符处理。 |
| LLT | OpenSSL 静/动态库冲突 | 177 | HTTP/TLS 用例均出现“loading libcrypto in an unsafe way”，随后 `SIGABRT`；栈在 stdx crypto key 解码路径。 | 否 | 修复/隔离 OpenSSL 链接方式后统一重跑 TLS/HTTP 分组。 |
| LLT | 静态 Runtime 链接配置 | 约 88 | 静态 API 用例链接 `libcangjie-runtime.a` 时缺少 C++ 标准库符号（如 `std::__1::mutex`）；另有约 43 项未在宏中加入已有的 `-lstdx.effect`。 | 否 | 补全 static/cffi 链接宏（`-lc++`）和 `stdx.effect` 链接库。 |
| LLT | 编译诊断或结果期望差异 | 48 | 例如 `accessMutableVarInTry` 已报 3 个预期错误，但期望的诊断文本已变化，比较器失败。 | 非 GC 问题；需逐项判定 | 更新规格基线或修正编译器诊断兼容性。 |

因此，本轮 2,120 个 FAIL 不能直接视为 Region-scan MSGC Runtime 缺陷：占主导的是 SDK 发布内容、
测试配置、外部工具及 OpenSSL 装载方式。需要优先补全/修正这些前置条件后，再从余下的断言或行为差异中
提取可复现的真实 Runtime/编译器问题。

### 根因复核记录

以下为从失败命令、编译器/链接器诊断和 SDK 文件树交叉核验后的结论；“已确认”仅说明失败发生的直接原因，
并不等同于已修复。

| 分类 | 直接证据 | 根因判断 | 对 Runtime 结论的影响 |
| --- | --- | --- | --- |
| SDK 发布内容缺失 | SDK 文件树中不存在 `stdx.chir.cjo`、`stdx.syntax.cjo`、`std.log`、`std.ffi.python`、`std.reflect`、`stdx.plugin.manager`；对应日志报 `can not find package`。 | 当前 MSGC SDK 未交付这些 API 包或子组件。 | 不能用于判断 GC/Runtime 正确性；待补包后重跑。 |
| 测试宏未展开 | HLT Python 失败命令把 `%link-python` 原样传入 `cjc`，编译器明确报 `invalid file or directory '%link-python'`。 | 框架配置没有定义该占位符，而非 Cangjie 编译或运行期失败。 | 修正宏后重跑该组。 |
| 测试机工具缺失 | 115 个 LLT 日志均为 `/bin/bash: dos2unix: command not found`，退出码 127。 | 宿主机缺少用例前置工具。 | 安装工具或改用跨平台预处理后重跑；不计 Runtime 问题。 |
| OpenSSL 装载冲突 | 177 个 LLT 和 1 个 HLT 运行日志先报告 `loading libcrypto in an unsafe way`，再在 stdx crypto key/TLS 初始化中收到 SIGABRT。 | 同一进程混用了静态 OpenSSL 和被 stdx 依赖的动态 `libcrypto`；这是链接/装载边界冲突。 | 信号由 Runtime 的 native-signal 输出路径记录，但触发点在 OpenSSL 装载，不是 Region-scan GC 崩溃。 |
| 静态 Runtime C++ 依赖遗漏 | 约 88 个 LLT static 链接日志在 `libcangjie-runtime.a` 报 `std::__1::mutex`、`recursive_mutex`、`operator new/delete` 未定义；实际链接命令未含 `-lc++`。 | static/cffi 用例的链接宏未带 macOS C++ 标准库。 | 补充 `-lc++` 后再评价该分组。 |
| 已交付库未被链接 | SDK 中存在 `dynamic/stdx/libstdx.effect.dylib`，但约 43 个日志的链接命令未带 `-lstdx.effect`，随后报 effect 符号未定义。 | `import-cangjie-stdx` 的通用链接列表缺少 effect 库。 | 是测试/SDK 链接配置缺项，不是 SDK 缺包或 Runtime 行为错误。 |
| 编译诊断/输出期望漂移 | 48 个 LLT 使用 `compare.py` 失败；例如 `accessMutableVarInTry` 已产生预期的 3 个编译错误，但旧断言文本与当前诊断文本不一致。 | 需要逐例确认语言规格/诊断格式是否已更新。 | 暂不归为 Runtime 问题；更新基线前保留为待判定。 |
| 网络和时序依赖 | 代理测试记录 `Connection reset by peer`、`SocketTimeoutException`。 | 测试依赖本机 socket 调度、端口和超时窗口，尚未在串行环境复现确认。 | 暂列环境/时序待复核，不能据此定性 Runtime。 |

本轮未在已抽样和聚类的失败中发现 `register x19 is not recorded`、`wrong reg info`、`SIGSEGV`，或可直接归因于
Region-scan mark 路径的崩溃。其余未被上述高频签名覆盖的失败，主要是比较器期望差异或需逐例复核的行为项；
在前置 SDK/链接问题清除前，不建议把它们批量认定为 Runtime 缺陷。

## 分类口径

执行结束后按以下口径归类，不将条件跳过、无有效命令分支或缺失外部依赖直接判定为 MSGC Runtime 缺陷：

| 分类 | 判定依据 | 处理方式 |
| --- | --- | --- |
| Runtime/编译器真实问题 | 崩溃、错误结果、堆栈或可稳定复现的行为异常 | 提取最小复现与日志，定位 Runtime/LLVM/编译器。 |
| MSGC 与 CopyGC 规格差异 | 断言依赖 CopyGC 堆参数、日志、OOM 阈值或 GC 行为 | 拆分 MSGC 专用期望或修改用例。 |
| 平台或外部依赖 | macOS 条件、OpenSSL、证书、网络、工具缺失等 | 配置依赖、加条件跳过或限定平台。 |
| 用例/框架问题 | 超时、退出码、诊断全文或脆弱时序断言 | 使断言稳定、修正预期或单独压力化。 |
| 未解析 | 无匹配 `EXEC`/`RUN` 条件分支 | 记录为未执行，不计失败。 |

## 证据位置

- HLT 最终日志：`/private/tmp/msgc-api-hlt-rerun-20260804/Maple_Test.log`
- LLT 最终日志：`/private/tmp/msgc-api-llt-valid-20260804/Maple_Test.log`
- HLT 框架结果目录：`/Users/lipeisheng/code/cangjie/cangjie/cangjie_test/output/msgc-api-hlt-rerun-20260804`
- LLT 框架结果目录：`/Users/lipeisheng/code/cangjie/cangjie/cangjie_test/output/msgc-api-llt-valid-20260804`
- LLT 使用的本地 API 测试列表：`MSGC_RESULT/Test/2026-08-04_msgc_llt_api_testlist.txt`
