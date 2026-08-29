# MSGC Windows 六阶段串行执行结果

## 执行信息

- 日期：2026-08-28
- 平台：Windows x86_64 cjnative
- compiler：`C:\cangjie\cangjie_compiler_CMSGC\output\msgc\bin\cjc.exe`
- runtime：`C:\cangjie\cangjie_compiler_CMSGC\output\msgc\runtime\lib\windows_x86_64_cjnative\`
- stdlib/stdx：当前 MSGC SDK 的 `windows_x86_64_cjnative` 与 `dynamic\stdx`
- cjpm：`C:\cangjie\cangjie_compiler_CMSGC\output\msgc\windows_x86_64_cjnative\cjpm`
- GC 模式：`--gc-mode marksweep`
- 阶段策略：Runtime LLT → Runtime HLT → stdlib LLT → stdlib HLT → compiler LLT → compiler HLT，阶段之间不并发
- 结果目录：本文件所在目录；每阶段原始 JSON、日志和临时文件位于对应 artifact 子目录

## 结果汇总

| 阶段 | 状态 | PASS | FAIL | UNSUPPORTED/ENV-ERROR | 退出码 | 原始结果 |
| --- | --- | ---: | ---: | ---: | ---: | --- |
| Runtime LLT | 完成（有失败） | 221 | 13 | 0 | 1 | [results.json](artifacts/01-runtime-llt-corrected/results.json) |
| Runtime HLT | 完成（有失败） | 1243 | 154 | 0 | 1 | [results.json](artifacts/02-runtime-hlt/results.json) |
| stdlib LLT | 完成（有失败） | 2238 | 340 | 0 | 1 | [results.json](artifacts/03-stdlib-llt/results.json) |
| stdlib HLT | 完成（有失败） | 7054 | 74 | 0 | 1 | [results.json](artifacts/04-stdlib-hlt/results.json) |
| compiler LLT | 完成（UTF-8 重跑，有失败） | 12168 | 2582 | 0 | 1 | [results.json](artifacts/05-compiler-llt-utf8/results.json) |
| compiler HLT | 完成（有失败） | 28029 | 769 | 0 | 1 | [results.json](artifacts/06-compiler-hlt/results.json) |

## 阶段记录

各阶段完成后在此追加实际命令、开始/结束时间、框架输出摘要和环境问题清单。未真正生成有效命令的用例记为 UNSUPPORTED，不计入 FAIL。

### Runtime LLT（阶段 1/6）

- 首次尝试：使用 Windows 专用清单 `cjnative_testlist_win`，该清单不含 `Runtime/`，框架执行 0 个用例并以退出码 0 结束；产物保留在 `artifacts/probe` 和 `artifacts/01-runtime-llt`，不计入统计。
- 修正后清单：`LLT/cjnative_testlist` + `LLT/configs/cjnative/exclude_cjnative_win`。
- 测试路径：`C:\cangjie\cangjie_test\testsuites\LLT\Runtime`
- 实际 compiler 覆盖：`cjc.exe --gc-mode marksweep -j1`；框架 worker：`-j 12`；单用例超时：180 秒；重试：0（未传 `--retry`，框架默认值）。
- 时间：2026-08-28T20:10 左右开始，2026-08-28T20:15:09 +08:00 结束。
- 框架摘要：234 个用例，PASS 221，FAIL 13，退出码 1。
- 失败用例（本轮只记录，不分析）：
  - `CJNative/Class/Define/class_static_04.cj`
  - `CJNative/DFX/heap_snapshot_coverage.cj`
  - `CJNative/Exception/Exception30_TryCatchReturnValueException.cj`
  - `CJNative/Exception/Exception32_OutOfMemoryError.cj`
  - `CJNative/Exception/Expection38_NewArray8.cj`
  - `CJNative/Exception/Expection39_NewArray16.cj`
  - `CJNative/Exception/Expection40_NewArray32.cj`
  - `CJNative/Exception/Expection41_NewArray64.cj`
  - `CJNative/Exception/Expection42_NewObject.cj`
  - `CJNative/RuntimeApi/runtime_api_validation.cj`
  - `CJNative/Stackmap/wahStackmap_01.cj`
  - `CJNative/debuginfo_ffic_expr.cj`
  - `CJNative/loop-vectorization/cj_innerArray_100000_0to8_GC_Int16.cj`
- 原始摘要：[summary.txt](artifacts/01-runtime-llt-corrected/summary.txt)；逐用例日志：[logs](artifacts/01-runtime-llt-corrected/logs)；退出信息：[exit.txt](artifacts/01-runtime-llt-corrected/exit.txt)。

### Runtime HLT（阶段 2/6）

- 测试清单：`HLT/testlist` + `HLT/configs/cjnative/exclude_common`。
- 测试路径：`C:\cangjie\cangjie_test\testsuites\HLT\Runtime`
- 实际 compiler 覆盖：`cjc.exe --gc-mode marksweep -j1`；框架 worker：`-j 12`；单用例超时：180 秒；重试：0。
- 时间：2026-08-28T20:16 左右开始，2026-08-28T20:39:34 +08:00 结束。
- 框架摘要：1,397 个用例，PASS 1,243，FAIL 154，退出码 1。
- 失败用例名称、期望/实际返回码及日志文件均保存在结果摘要中；本轮不分析原因。
- 原始摘要：[summary.txt](artifacts/02-runtime-hlt/summary.txt)；逐用例日志：[logs](artifacts/02-runtime-hlt/logs)；JSON：[results.json](artifacts/02-runtime-hlt/results.json)；退出信息：[exit.txt](artifacts/02-runtime-hlt/exit.txt)。

### stdlib LLT（阶段 3/6）

- 测试清单：`LLT/cjnative_testlist` + `LLT/configs/cjnative/exclude_cjnative_win`。
- 测试路径：`C:\cangjie\cangjie_test\testsuites\LLT\API\std`
- 实际 compiler 覆盖：`cjc.exe --gc-mode marksweep -j1`；框架 worker：`-j 12`；单用例超时：180 秒；重试：0。
- 时间：2026-08-28T20:40 左右开始，2026-08-28T21:04:02 +08:00 结束。
- 框架摘要：2,578 个用例，PASS 2,238，FAIL 340，退出码 1。
- 失败用例名称、期望/实际返回码及日志文件均保存在结果摘要中；本轮不分析原因。框架另记录了个别 `DEPENDENCE keyword error` 信息。
- 原始摘要：[summary.txt](artifacts/03-stdlib-llt/summary.txt)；逐用例日志：[logs](artifacts/03-stdlib-llt/logs)；JSON：[results.json](artifacts/03-stdlib-llt/results.json)；退出信息：[exit.txt](artifacts/03-stdlib-llt/exit.txt)。

### stdlib HLT（阶段 4/6）

- 测试清单：`HLT/testlist` + `HLT/configs/cjnative/exclude_common`。
- 测试路径：`C:\cangjie\cangjie_test\testsuites\HLT\API\std`
- 实际 compiler 覆盖：`cjc.exe --gc-mode marksweep -j1`；框架 worker：`-j 12`；单用例超时：180 秒；重试：0。
- 时间：2026-08-28T21:05 左右开始，2026-08-28T22:11:53 +08:00 结束。
- 框架摘要：7,128 个用例，PASS 7,054，FAIL 74，退出码 1。
- 框架另记录 1 个源码 UTF-8 解码警告；失败用例名称、返回码和日志文件均保存在摘要中，本轮不分析原因。
- 原始摘要：[summary.txt](artifacts/04-stdlib-hlt/summary.txt)；逐用例日志：[logs](artifacts/04-stdlib-hlt/logs)；JSON：[results.json](artifacts/04-stdlib-hlt/results.json)；退出信息：[exit.txt](artifacts/04-stdlib-hlt/exit.txt)。

### compiler LLT（阶段 5/6，首次收集）

- 测试路径：`C:\cangjie\cangjie_test\testsuites\LLT\compiler`；清单：`LLT/cjnative_testlist` + `LLT/configs/cjnative/exclude_cjnative_win`。
- 使用的 SDK 和 compiler 覆盖参数与前阶段相同：`cjc.exe --gc-mode marksweep -j1`，框架 worker `-j 12`。
- 结果：框架在收集依赖文件时因系统默认 GBK 解码触发 `UnicodeDecodeError`，未执行用例，退出码 1；错误摘要位于 [first-attempt.txt](artifacts/05-compiler-llt/first-attempt.txt)。
- 处理：下一次尝试设置 `PYTHONUTF8=1`，仅修正框架文本解码环境。

### compiler LLT（阶段 5/6，UTF-8 重跑）

- 处理：设置 `PYTHONUTF8=1` 并使用 `python -X utf8` 重跑，测试命令、SDK 与 compiler 参数不变。
- 时间：2026-08-28T22:30 左右开始，2026-08-28T23:36:04 +08:00 结束。
- 框架摘要：14,750 个用例，PASS 12,168，FAIL 2,582，退出码 1。
- 框架另记录 1 个非法 UTF-8 源文件被忽略；失败用例名称、返回码和日志文件均保存在摘要中，本轮不分析原因。
- 原始摘要：[summary.txt](artifacts/05-compiler-llt-utf8/summary.txt)；逐用例日志：[logs](artifacts/05-compiler-llt-utf8/logs)；JSON：[results.json](artifacts/05-compiler-llt-utf8/results.json)；退出信息：[exit.txt](artifacts/05-compiler-llt-utf8/exit.txt)。

### compiler HLT（阶段 6/6）

- 测试清单：`HLT/testlist` + `HLT/configs/cjnative/exclude_common`。
- 测试路径：`C:\cangjie\cangjie_test\testsuites\HLT\compiler`。
- 实际 compiler 覆盖：`cjc.exe --gc-mode marksweep -j1`；框架 worker：`-j 12`；单用例超时：180 秒；重试：0；使用 `PYTHONUTF8=1` 与 `python -X utf8`。
- 时间：2026-08-28T23:40 左右开始，2026-08-29T02:27:43 +08:00 结束。
- 框架摘要：28,798 个用例，PASS 28,029，FAIL 769，退出码 1。
- 框架另记录 5 个非法 UTF-8 源文件警告；失败用例名称、返回码和日志文件均保存在摘要中，本轮不分析原因。
- 原始摘要：[summary.txt](artifacts/06-compiler-hlt/summary.txt)；逐用例日志：[logs](artifacts/06-compiler-hlt/logs)；JSON：[results.json](artifacts/06-compiler-hlt/results.json)；退出信息：[exit.txt](artifacts/06-compiler-hlt/exit.txt)。
