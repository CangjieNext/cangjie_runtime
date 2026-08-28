# MSGC Finalizer 暂不执行项与普通析构验证（2026-08-04）

## 暂不执行项

以下 HLT Finalizer 用例的析构回调依赖 CJThread-aware finalizer 执行模型（同步原语、调度、`sleep` 或线程快照）。该前置特性尚未合入当前 Region-scan MSGC 分支，因此已加入 `2026-08-04_msgc_hlt_runtime_safe_testlist.txt` 的 `[EXCLUDE-TEST-CASE]`，不纳入本分支的常规 Runtime 回归：

`barrier_sync`、`barrier_sync_1`、`complex_2`、`dump_thread`、`finalizer_mutex_01`、`monitor_sync`、`mutex_sync`、`read_write_lock_sync`、`read_write_lock_sync_1`、`semaphore_sync`、`semaphore_sync_1`、`sleep`、`sync_counter`。

这是一项当前分支能力边界的分类，不将这些用例的原始失败作为 Region-scan marksweep Runtime 回归缺陷统计。

## 普通析构验证

用例：`HLT/Runtime/cjnative/runtime/MM/Finalizer_01.cj`

- 析构函数仅执行 `count--`，不依赖 CJThread 调度、阻塞同步或线程快照。
- SDK：`cangjie_compiler_region_mark/output/msgc`。
- 编译选项：`--set-runtime-rpath --gc-mode=marksweep`。
- 结果：编译成功（仅 `GC()` 废弃告警），程序在 30 秒门限内以退出码 `0` 完成。

结论：当前 MSGC Runtime 能执行普通的 Cangjie 析构函数；受限的是析构回调中的 CJThread 依赖能力。
