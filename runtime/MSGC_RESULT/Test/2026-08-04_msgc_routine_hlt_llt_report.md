# Region-scan MSGC Runtime 用例当前结果

## 测试范围与口径

测试环境为 macOS/aarch64，使用已安装的 Region-scan MSGC SDK：
`/Users/lipeisheng/code/cangjie/cangjie/cangjie_compiler_region_mark/output/msgc`。
所有 Cangjie 用例均显式使用 `--gc-mode=marksweep`。

本文只描述当前结果，不保留已被修复项的历史失败基线。统计分为两部分：

- **常规回归**：使用 `-j2` 执行的 HLT/LLT Runtime 用例。
- **压力桶**：OOM、大对象/大数组、持续 GC 和大量协程等资源敏感用例；固定单例 `-j1`，不计入常规回归总数。

## 当前汇总

| 范围 | 用例总数 | 通过 | 当前确认失败 | 未解析/待重跑 | 结论 |
| --- | ---: | ---: | ---: | ---: | --- |
| HLT / Runtime 常规回归 | 1475 | 1294 | 39 | 142 | 失败集中于 Finalizer 崩溃、CopyGC 专属期望和诊断文本。 |
| LLT / Runtime 常规回归 | 251 | 232 | 1 | 18 | 唯一确认失败是负下标诊断全文匹配差异。 |
| 常规回归合计 | 1726 | 1526 | 40 | 160 | 已验证通过率 **88.41%**。160 项在当前条件下没有有效命令分支，不计为运行失败。 |
| 压力桶（独立统计） | — | 69 | 5 | 3 按要求跳过；`oom_env_11` 不执行 | 33 个待重跑项中，已串行完成 30 个：25 通过、5 失败；失败均非 Fast3 的 `x19` stackmap 崩溃。 |

其中压力桶的 44 个已验证通过项包括 GC 并发、高频 GC、持续 GC、超大页、大数组、忙等，以及
`Throwable/013` 的 `exception_0001`、`exception_0002`、`exception_0004`。

本轮串行复验后，压力桶新增 25 个通过项：

- `OutOfMemoryError` 环境用例 11/13 通过；
- `Throwable/012` 8/8 通过；
- `Throwable/013` 复验项 6/7 通过，加上此前已验证的 3 项，该组当前为 9/10 通过；
- 失败与跳过项的具体原因见下表。

## 已通过的关键修复验证

| 用例/类别 | 结果 | 说明 |
| --- | --- | --- |
| HLT `Runtime/cjnative/static_runtime/synchronized.cj` | PASS | Region-scan MSGC 静态 Runtime 链接产物可用。 |
| LLT Runtime API 5 项 | 5/5 PASS | 使用 SDK `Cangjie.h` 后，Runtime API ABI 一致。 |
| LLT `Runtime/CJNative/Signal/signal001.cj` | PASS | C 接口声明、POSIX 头文件和信号回调签名已正确。 |
| HLT `Throwable/013/exception_0001.cj` | PASS | `cjHeapSize=4MB` 下输出预期 `Out of memory`，无 `register x19 is not recorded`、`wrong reg info` 或 `SIGABRT`。 |
| HLT `Throwable/013/exception_0002.cj` | PASS | 受限堆并发/锁场景正常完成并输出预期 OOM。 |
| HLT `Throwable/013/exception_0004.cj` | PASS | 捕获 `OutOfMemoryError` 后可执行 `list.clear()` 和显式 `GC()`。 |
| HLT `Throwable/012` | 8/8 PASS | 析构/异常压力路径在单例 `-j1` 下完成。 |
| HLT `Throwable/013/exception_0003,0005,0006,0007,0009,0010.cj` | 6/6 PASS | 受限堆 OOM、清理与 GC 路径均完成，无寄存器记录错误或 SIGABRT。 |

`Throwable/013` 的 OOM 问题已通过为 `CJ_MCC_NewArrayGenericFast3_MSGC` 增加
callee-saved trampoline 修复：慢分配进入 GC 前会保存 `x19`–`x28`，不再因 stackmap
根枚举失败而 SIGABRT。

## 当前确认失败：40 项

| 分类 | 数量 | 用例范围 | 原因 | 是否需要修改 |
| --- | ---: | --- | --- | --- |
| Finalizer / GC-helper 崩溃 | 12 | `Finalizer/{barrier_sync,barrier_sync_1,complex_2,finalizer_mutex_01,monitor_sync,mutex_sync,read_write_lock_sync,read_write_lock_sync_1,semaphore_sync,semaphore_sync_1,sync_counter,dump_thread}.cj` | `gc-helper` SIGSEGV（故障地址 `0x164`）或 `mutator->IsVaildCJThread()` 检查失败。 | **需要修改 Runtime，P0。** |
| Finalizer 时序 | 1 | `Finalizer/sleep.cj` | 程序可返回，但 Finalizer 时序/输出不满足断言。 | Finalizer 崩溃修复后复测；暂不修改规格。 |
| CopyGC 与 MSGC 规格差异 | 20 | `GC/gc_export/gc_env_macro/gc_export_{001,003,004,005,006,010,011,025}.info`；`GC/gc_export/gc_export_{001,002,003,004,005,006,007,010,011,012,025}.info`；`gc_export_029.cj` | 用例断言 CopyGC 的堆大小、region、阈值和日志；MSGC 使用独立的 MarkSweep 参数与日志语义。 | **修改/拆分 MSGC 专用测试期望。** |
| 诊断文本差异 | 7 | Native 信号/异常 6 项；LLT `Runtime/CJNative/array/array11.cj` | Native 诊断前缀为 `CJNatvie` 而非 `CJNative`；负下标用例只因完整诊断文本变化而失败。 | 修正 Runtime 拼写或将测试改为匹配稳定关键字。 |

失败总数：12 + 1 + 20 + 7 = **40**。

## 压力桶复验失败与不执行项

压力桶的 5 个失败不计入上面的常规回归 40 项：

| 用例 | 结果 | 直接证据 | 分类与处理建议 |
| --- | --- | --- | --- |
| HLT `OutOfMemoryError/oom_dump.cj` | FAIL | `cjHeapSize=4mb` 下 100 个后台 `spawn` 在主线程 `sleep(2s)` 结束前没有稳定触发 OOM，未出现转储提示。 | **压力时序不稳定**；不需要 Runtime 修复。应将用例改为等待/同步实际 OOM，或改为单线程确定性分配。 |
| HLT `OutOfMemoryError/oom_env_08.cj` | FAIL | 已输出 `Out of memory`，并生成 `cj_OOM_pid*.dat`；最后的 `RUN-EXEC-1: ls ...` 返回 0，而标注期望退出码 1。 | **用例期望与当前 MSGC 转储行为不符**；修改 MSGC 条件分支，使检查转储文件存在并期望 0。 |
| HLT `Throwable/013/exception_0008.cj` | FAIL（超时） | 4 MB 堆下启动 1001 个无限分配协程，90 秒超时；无 `register x19 is not recorded`、`wrong reg info` 或 SIGABRT。 | **高并发压力/调度待分析**；先缩小协程数或采用受控栅栏形成稳定回归，再判断是否需要 Runtime 调度优化。 |
| LLT `Exception32_OutOfMemoryError.cj` | FAIL | 约 71.5 秒后成功分配 10 亿个 `Int64`，输出 `10`，而用例断言 OOM 分支输出 `20`。 | **MSGC 默认可用堆/规格差异**；为 MSGC 用例设置可控 `cjHeapSize` 强制 OOM，或拆分 MSGC 期望。 |
| LLT `Expection42_NewObject.cj` | FAIL | 分配 100 万个对象后正常以 0 退出；`RUN-ERRCHECK` 期望非 0 和 `Out of memory`。 | **MSGC 默认可用堆/规格差异**；为 MSGC 设置堆上限或调整用例为确定性 OOM。 |

## 不执行项

以下项目当前不计为失败：

| 状态 | 数量 | 范围 | 原因与处理方式 |
| --- | ---: | --- | --- |
| 按要求跳过 | 3 | LLT `Expection38_NewArray8.cj`、`Expection39_NewArray16.cj`、`Expection40_NewArray32.cj` | 在当前 MSGC 默认可用堆下长时间不 OOM；本轮不运行。 |
| 明确不执行 | 1 | `OutOfMemoryError/oom_env_11.cj` | 用例内部会后台启动 4 个运行时进程，框架 `-j1` 无法限制该并发，可能导致开发机卡死；仅在有资源门禁的专用机器执行。 |
| 常规未解析 | 160 | 142 HLT、18 LLT | 测试框架均记录 `No valid command statement was found.`，且 `commands` 为空：用例的条件化 `EXEC`/`RUN-EXEC` 没有匹配当前 macOS/aarch64 + CJNative 配置。它们没有被编译或运行，不计为失败。 |

当前优先级：先修复 Finalizer/`gc-helper` 崩溃；随后将 `Throwable/013/0008` 收敛为可控压力
回归；最后处理 MSGC 专用 OOM 堆上限、转储检查和诊断匹配。

## 可复现证据

- HLT 结果 JSON：`/Users/lipeisheng/code/cangjie/cangjie/cangjie_test/output/msgc-runtime-routine-hlt.engLCE/results.json`
- LLT 结果 JSON：`/Users/lipeisheng/code/cangjie/cangjie/cangjie_test/output/msgc-runtime-routine-llt.cJ7p7p/results.json`
- HLT 常规清单：[2026-08-04_msgc_hlt_runtime_safe_testlist.txt](2026-08-04_msgc_hlt_runtime_safe_testlist.txt)
- LLT 常规清单：[2026-08-04_msgc_llt_runtime_safe_testlist.txt](2026-08-04_msgc_llt_runtime_safe_testlist.txt)
- 压力项筛选依据：[2026-08-04_msgc_runtime_suite_resource_triage.md](2026-08-04_msgc_runtime_suite_resource_triage.md)
