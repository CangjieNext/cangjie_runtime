# case25 Windows MSGC STW 停滞：RISKY unwind 状态遗漏 saferegion 修复

## 基本信息

- 日期：2026-08-27
- 平台：Windows x86_64
- GC：MSGC / MarkSweep
- 用例：`MSGC_RESULT/Result/case25_http_complex_json_stream`
- 压力模型：Go client 访问 MSGC HTTP server，`100K` payload、`c100`、`GOMAXPROCS=10`、`cjProcessorNum=10`、持久连接。

## 现象

在 Windows 本地构建的 MSGC SDK 上，服务端会在运行数千至一万余个请求后停止处理所有请求：

- 2 秒进度窗口变为 `+last2s=0`；
- 100 个客户端请求随后按 5 秒超时；
- Go client 访问 Java server 在同一负载下稳定完成，排除客户端和本机网络为主因；
- 简化 server 响应、跳过 JSON transform 后仍复现，排除 JSON 转换和响应体分配为主因。

## 证据与收敛过程

离线 dump 显示 GC 主线程停在：

```text
MarkSweepCollector::PreEnumRoots
  -> MutatorManager::StopTheWorld
  -> MutatorManager::WaitUntilAllMutatorStopped
```

同一时刻，实际运行的 CJThread 调度线程已经停在 `Mutator::SuspendForSync`。这说明不是某个 OS 线程继续执行用户代码，而是 STW 账本中有一个非运行 CJThread 的 mutator 没有进入 saferegion。

临时将 STW 诊断超时缩短后，首次超时日志稳定给出：

```text
not stopped: <tid>, total: 102, visited: 101/102
[STW-DETAIL] tid=<tid> cjid=1 state=1 unwind=2
```

其中：

- `state=1` 为 `CJTHREAD_READY`；
- `unwind=2` 为 `UnwindContextStatus::RISKY`；
- 该 CJThread 已不在运行态，却仍被 STW 作为未停止 mutator 等待。

## 根因

`src/Mutator/Mutator.cpp` 的 `Mutator::PreparedToPark()` 原先把以下两种情况合并为提前返回：

```cpp
if (UNLIKELY((uwContext.GetUnwindContextStatus() == UnwindContextStatus::RISKY) || InSaferegion())) {
    return;
}
```

当 CJThread 切换为 `READY`，且 unwind context 为 `RISKY` 时，该函数既不更新 unwind frame，也不会调用 `DoEnterSaferegion()`。前者是合理的，因为 RISKY 状态不能重新采样栈帧；后者不合理，因为 mutator 已不再运行，必须完成 STW saferegion 记账。

随后 GC 调用 `WaitUntilAllMutatorStopped()` 时，101 个 mutator 已停止，只有这个 READY CJThread 的 mutator 保持非 saferegion，造成无限等待直到 STW 超时。

## 修复

将 `RISKY` 从提前返回条件中移除：

```cpp
SetSafepointStatePtr(nullptr);
if (UNLIKELY(InSaferegion())) {
    return;
}

// RISKY 时保持跳过本平台的 unwind frame 更新。
// 无论 unwind 状态如何，park 的 mutator 都必须进入 saferegion。
DoEnterSaferegion();
```

实际代码仍保留各平台原有的：

```cpp
if (LIKELY(uwContext.GetUnwindContextStatus() != UnwindContextStatus::RISKY)) {
    // 更新 unwind frame
}
```

因此修复不改变 RISKY unwind 的栈帧语义，只补齐 CJThread park 时的 saferegion 状态迁移。

修改文件：

- `src/Mutator/Mutator.cpp`

排查时添加的 STW stderr 诊断、1 秒诊断超时、简化 HTTP 响应、连接强制关闭和线程池预热开关均已删除，未保留在最终修改中。

## 构建与验证

运行时通过以下命令重新构建并安装到本地 MSGC SDK：

```powershell
py build\scripts\rebuild_msgc_runtime_output.py --skip-clean
```

已确认构建产物与 SDK 中的 runtime DLL SHA-256 一致：

```text
123B4BB7F8EA5090E55493BD511F369585CF522F5813D11FA255870E04335B63
```

`benchmark.cj` 以 `-O2 --gc-mode=marksweep --dy-std` 重新编译成功。修复后的 Go client -> MSGC server 压测结果如下：

| 轮次 | 时长 | 成功/失败 | 吞吐量 |
| --- | ---: | ---: | ---: |
| 1 | 30 s | 41,137 / 0 | 1,368 req/s |
| 2 | 30 s | 39,516 / 0 | 1,314 req/s |

两轮均无停滞、无客户端超时，服务端 stderr 无 STW 超时记录。

## 影响范围

修复位于平台无关的 `Mutator::PreparedToPark()` 逻辑，对 Linux、macOS 和 Windows 都生效。当前已完成 Windows x86_64 的压力验证；其他平台未在本轮重复压测。RISKY 状态下的 unwind frame 更新路径保持原样。
