# Runtime HLT `multi_coroutine_park_02` SIGSEGV 修复记录

## 1. 问题概述

用例：`/home/lipeisheng/cangjie/cangjie_test/testsuites/HLT/Runtime/cjnative/runtime/GC/multi_coroutine_park_02.cj`。

现象是在协程频繁 park、唤醒和大量 GC 触发的压力下，MSGC release runtime 在 `MarkSweepCollector::RunRegionTask()` 中解引用空 `TypeInfo`，进程收到 `SIGSEGV`，故障地址为 `0x8`。该用例是当前 Runtime HLT 中复现该问题的目标用例。

本次修复的目标是：

- 恢复 outside-set trusted mark 目标的发布有效性校验；
- 覆盖实际的小对象 trusted 快路径；
- 保留扫描端的 release 防护，避免异常 mark 位再次造成 runtime 崩溃；
- 用与 SDK MSGC 模式一致的 compiler/runtime 组合重新编译并验证用例。

## 2. 复现基线

原始结果来自 `MSGC_RESULT/Tests/Test-linux-x86/Runtime-HLT`。原始二进制在 8 次默认运行中 6 次以 `rc=139` 失败、2 次通过；设置 `cjProcessorNum=1` 时原始问题可稳定复现，基线批次为 3/3 失败。失败栈指向：

```text
MarkSweepCollector::RunRegionTask
TypeInfo::IsWeakRefType
SIGSEGV, address 0x8
```

失败前的反汇编显示：`obj->GetTypeInfo()` 返回空指针，随后 `IsWeakRefType()` 读取 `TypeInfo + 0x8`。该现象与协程根扫描或并发 mark 产生的 aligned stale slot 相符。

基线排查还记录了以下结果：

- `cjMSParallelSweep=false` 不能消除故障；
- `CJ_MS_STW_SWEEP=1` 不能消除故障；
- `cjProcessorNum=1` 是最稳定的触发条件；
- `cjProcessorNum=4`、`cjGCThreads=1` 的部分批次通过，但不能证明问题已修复。

验证环境：compiler 使用 `/home/lipeisheng/cangjie/cangjie_compiler_CMSGC/output/msgc/bin/cjc`，版本为 `Cangjie 1.0.0 (cjnative)`；stdlib/runtime 使用同一 SDK 的 `linux_x86_64_cjnative` 目录。修复前 SDK runtime 备份为 `libcangjie-runtime.so.backup-20260829-033422`，Build ID 为 `656cc02a518c67e4ecb67f9894d5415e3dbe7c9a`。

## 3. 原因分析

### 3.1 直接故障链路

MSGC 将已封存的 Region 划入本轮 `collect-set`，而仍由 mutator 持有的 active Region 属于 `outside-set`。在 `collect-set` 中，ALLOC bitmap 是稳定快照，可以过滤未分配 slot；在 `outside-set` 中，ALLOC bitmap 可能还未物化并且会继续变化，因此不能用它判断 slot 是否有效。

原实现的 trusted 小对象路径在 `AddRefToMarkBatch()` 中直接根据地址计算 Region、word 和 bit，然后发布 mark 位。该路径没有调用 `AddResolvedRefToMarkBatchImpl()`，所以 outside-set 的 `TypeInfo` 有效性检查没有覆盖它。一个已经被 sweep 清空、但地址仍满足对齐和 slot 计算条件的旧协程根，就可能把对应 stale slot 发布到 mark bitmap。

随后 `RunRegionTask()` 根据 mark bitmap 扫描该 slot。release 代码只在 debug 下检查 `TypeInfo != nullptr`，检查在 release 中被编译掉，但后续无条件调用 `typeInfo->IsWeakRefType()`，最终在 `TypeInfo + 0x8` 处触发 `SIGSEGV`。

### 3.2 `25e4b1d7` 删除校验的背景与问题

`25e4b1d7` 的优化将 trusted 引用视为已经满足“位于 managed heap、地址是合法对象”的强前提，并删除了 outside-set 路径的 `TypeInfo` 读取，以减少每个 trusted 引用的冷内存访问。这个假设对稳定的 collect-set 分配快照基本成立，但对 outside-set 不成立：active Region 的分配状态仍在变化，协程生命周期又会让旧栈值或旧边被并发观察到。

删除校验的原因是性能优化，而不是因为 `TypeInfo` 校验在语义上不需要。问题在于优化没有把校验范围限制为 collect-set，且实际小对象 trusted 快路径没有保留 outside-set 的兜底检查，导致“trusted”被当成了“永远不会 stale”。本修复恢复的是 outside-set 的边界检查，不会在 collect-set 的稳定 alloc bitmap 路径上为每个引用增加 `TypeInfo` 访问。

### 3.3 为什么只有该用例触发

`multi_coroutine_park_02` 同时具备大量短生命周期协程、频繁 park/wakeup、协程栈根扫描和高频主动 GC。它更容易让旧的协程栈值、回收后的 aligned slot 以及 mark worker 的扫描时序重叠。普通 Runtime HLT 用例通常没有同时满足这些条件，因此未稳定触发相同窗口；这不是该用例本身违反了对象模型，而是它把 runtime 的 stale-slot 防护缺口放大了。

## 4. 修复内容

修改文件：`src/Heap/MCollector/MarkSweepCollector.cpp`。

1. `ValidatePublishedMarkTarget(obj, MarkSourceTag<true>)` 在 release 中也返回 `obj != nullptr && obj->GetTypeInfo() != nullptr`。debug 仍保留原有的 fatal 检查。
2. 在 trusted 小对象直达路径中，仅当 `info->collectEpoch != batch.GetMarkEpoch()` 时执行上述校验；collect-set 继续由稳定 ALLOC bitmap 过滤。
3. 在已解析的非 small trusted 路径中，同样对 outside-set 执行校验，覆盖 LARGE 等路径。
4. `RunRegionTask()` 和 `RunObjectTask()` 增加 release 防护：发现空 `TypeInfo` 时记录错误、跳过对象并正确完成 task/lease；debug 构建仍立即报错，便于继续发现上游发布问题。
5. 没有修改 `CJThread` 源码。`src/CJThread/.../cjthread.cpp` 不属于当前 CMake runtime target，实际链接的是外部 `libcangjie-thread`，因此不把未生效的试探性改动带入最终补丁。

## 5. 构建与验证

### 5.1 构建

使用已有 `CMakebuild` 配置，确认 `MARKSWEEP_FLAG=1`、`COPYGC_FLAG=0`，执行：

```bash
cmake --build CMakebuild --target cangjie-runtime --parallel 32
cmake --install CMakebuild --prefix /home/lipeisheng/cangjie/cangjie_compiler_CMSGC/output/msgc
```

构建和安装退出码均为 `0`。新的 SDK runtime 为：

```text
/home/lipeisheng/cangjie/cangjie_compiler_CMSGC/output/msgc/runtime/lib/linux_x86_64_cjnative/libcangjie-runtime.so
Build ID: 186b491770e8cef3d619a195bbf13b8fea801f93
```

安装时 CMake 对未生成的默认 `output/common/linux_release_x86_64` 下 `cjfilt` 和静态 runtime 执行 `strip` 时打印了历史路径 warning，但目标 SDK runtime 已成功安装，未影响本次验证。

### 5.2 重新编译用例

替换 runtime 后，按项目规则重新编译 `.cj`，没有复用旧 binary：

```bash
source /home/lipeisheng/cangjie/cangjie_compiler_CMSGC/output/msgc/envsetup.sh
cjc --gc-mode marksweep --dy-std --int-overflow=wrapping \
  /home/lipeisheng/cangjie/cangjie_test/testsuites/HLT/Runtime/cjnative/runtime/GC/multi_coroutine_park_02.cj \
  -o MSGC_RESULT/bugfix/2026-08-29_Runtime-HLT_multi_coroutine_park_02_SIGSEGV_validation/multi_coroutine_park_02.minimal.out
```

编译退出码为 `0`。`nm` 确认生成 binary 包含 `CJ_MCC_NewObjectFast_MSGC_*`；`ldd` 确认运行时加载的是上述 SDK runtime。compiler 来源为 `/home/lipeisheng/cangjie/cangjie_compiler_CMSGC/output/msgc/bin/cjc`，stdlib/stdx 使用该 SDK 的 `/home/lipeisheng/cangjie/cangjie_compiler_CMSGC/output/msgc/linux_x86_64_cjnative` 配置及其运行库。

### 5.3 回归结果

所有运行均在同一 SDK 环境中串行执行，结果文件位于：
`MSGC_RESULT/bugfix/2026-08-29_Runtime-HLT_multi_coroutine_park_02_SIGSEGV_validation/`。

| 配置 | 次数 | 结果 | stderr | GC report |
| --- | ---: | --- | ---: | ---: |
| 默认处理器数 | 3 | 3/3 `rc=0` | 0 行 | 每次 100 个 GC cycle |
| `cjProcessorNum=1` | 3 | 3/3 `rc=0` | 0 行 | 每次 100 个 GC cycle |

6 次运行 stdout 均为 `1\n`；stderr 中 `SIGSEGV`、`Segmentation fault` 和 `invalid marked object` 均为 0 次。单处理器配置覆盖了原始基线最稳定的复现条件。

## 6. 结果与残余风险

修复结果：原始 `RunRegionTask -> TypeInfo::IsWeakRefType` 空指针崩溃已在目标用例和原始单处理器触发条件下消失，且最终验证没有产生 invalid-mark 防护日志。最终生效的代码改动仅为 `MarkSweepCollector.cpp`，没有改动测试源文件、compiler 或 stdlib。

残余风险：扫描端 release 防护是最后一道保险，不应替代上游 mark 发布校验；如果未来仍看到 `invalid marked object`，应继续定位对应的 root/barrier 来源，而不是把日志静默视为正常。当前验证覆盖目标 Runtime HLT 用例的 release MSGC 回归，不等同于重新执行完整 Runtime HLT/LLT 套件；仓库当前也没有可直接运行的历史 trusted-mark contract 测试文件，因此未声称该独立 contract 已执行。

SDK 原 runtime 已在替换前备份为：
`/home/lipeisheng/cangjie/cangjie_compiler_CMSGC/output/msgc/runtime/lib/linux_x86_64_cjnative/libcangjie-runtime.so.backup-20260829-033422`。
