# signal-stack 打印重入导致 runtime-frame 次生噪音修复记录

## 基本信息

- 日期：2026-06-21
- 场景：`MSGC_RESULT/Result/case28_exception_ffi_boundary/native_explicit_rethrow.cj`
- SDK：`/Users/lipeisheng/code/cangjie/cangjie-pkg/self/dev260619/cangjie`
- 本次修改文件：
  - `src/SignalManager.cpp`
  - `test/SignalRuntimeNoiseRegression.sh`

## 现象

- `native_explicit_rethrow.cj` 本来就是一个“异常边界错误用法”的负向用例：`Cangjie -> C -> @C callback`，callback 在仓颉侧抛异常，native helper 再直接调用 `MRT_ThrowPendingException(nullptr)`，最终会崩溃。
- 这个 primary failure 之外，stderr 里还会出现一串额外的 signal 噪音：
  - 多次 `CJNatvie Handle signal: 11.`
  - 多次 `catched unhandled SIGSEGV`
  - 多次 `from runtime frame`
  - 还可能夹杂 `invalid index in stringpool`
- 这类输出很容易把人带偏，好像“runtime 自己又坏了一层”，但它不是第一现场，而是 crash dump 路径自己再次触发 fault 的次生现象。

## 关键调用链

- primary crash 进入：
  - `SignalManager::HandleUnexpectedSignal`
  - `PrintSignalHandlerStack`
  - `StackManager::PrintSignalStackTrace`
- 次生 fault 常落在 dump/unwind 期间：
  - `FrameInfo::ResolveProcInfo`
  - `SigHandlerFrameinfo::PrintFrameInfo`
  - 以及相关 metadata / stringpool / stackmap 访问路径

## 根因

- `PrintSignalHandlerStack()` 在 fatal signal 上会尝试继续做完整的栈打印。
- 但在异常/OOM/崩溃现场里，unwind 与 frame 解析会继续访问 runtime 元数据。
- 一旦这些访问再次触发 `SIGSEGV`，信号处理器会在 signal-stack 上重入。
- 旧逻辑没有区分“第一层 fatal signal”和“dump 过程中又炸出来的第二层 fatal signal”，于是会再次走完整打印路径。
- 当第二层 handler 返回后，执行又回到原来那条已经 fault 的 dump 指令，于是继续触发下一次 `SIGSEGV`，形成反复的 runtime-frame 噪音。

## 修复方式

- 在 `src/SignalManager.cpp` 中给 `PrintSignalHandlerStack()` 增加线程局部重入深度保护：
  - `g_fatalSignalDumpDepth`
  - `ScopedFatalSignalDump`
- 第一层 fatal signal 仍按原逻辑打印。
- 如果是在 dump 过程中再次进到 `PrintSignalHandlerStack()`：
  - 不再做完整 unwind/metadata 解析
  - 只打印一条最小错误信息：
    - `nested fatal signal while dumping previous crash: ...`
  - 直接 `_exit(128 + sig)` 退出
- 这样可以保证：
  - primary crash 仍然可见
  - nested crash 不再反复踩 runtime frame
  - 进程不会从 nested handler 返回后又回到原来那条已 fault 的 dump 指令继续循环

## 为什么这样修是对的

- 这个修复不是“把原始崩溃吞掉”，因为第一层 fatal signal 仍然会按旧逻辑打印。
- 它只处理“dump 过程中再次崩”的重入场景。
- 对这类场景，继续尝试完整打印已经没有收益，反而会制造更多噪音并污染第一现场。
- 直接降级为单条日志 + 立即退出，能最大程度保留真正有用的第一现场信息。

## 代码改动摘要

- `src/SignalManager.cpp`
  - 新增 signal dump 重入深度保护
  - 新增 nested fatal signal 的最小日志与 `_exit` 降级退出路径
- `test/SignalRuntimeNoiseRegression.sh`
  - 新增最小回归脚本
  - 重新编译 `native_explicit_rethrow.cj`
  - 断言：
    - 用例必须失败
    - `catched unhandled SIGSEGV` 行数不能失控
    - `from runtime frame` 只能保留一次

## 验证

### 1. 重编 MSGC runtime

```bash
python3 build.py build --target native --build-type release -v 0.0.1 --gc-mode marksweep
```

- 结果：通过

### 2. 替换到 `dev260619` 并重签

```bash
cp output/temp/lib/aarch64_Release/libcangjie-runtime.dylib \
  /Users/lipeisheng/code/cangjie/cangjie-pkg/self/dev260619/cangjie/runtime/lib/darwin_aarch64_cjnative/libcangjie-runtime.dylib
codesign -f -s - \
  /Users/lipeisheng/code/cangjie/cangjie-pkg/self/dev260619/cangjie/runtime/lib/darwin_aarch64_cjnative/libcangjie-runtime.dylib
```

- 结果：通过

### 3. 运行最小回归脚本

```bash
./test/SignalRuntimeNoiseRegression.sh
```

- 结果：通过
- 输出摘要：
  - `exit_code=139`
  - `sigsegv_lines=1`
  - `runtime_frame_lines=1`

### 4. 修复后的 stderr 关键片段

```text
An exception has occurred:
Exception: native-explicit-11
...
CJNatvie Handle signal: 11.
Thread "" catched unhandled SIGSEGV ... from managed frame ...
CJNatvie Handle signal: 11.
nested fatal signal while dumping previous crash: SIGSEGV ... from runtime frame ...
```

## 结论

- 这次修复解决的是“signal-stack 打印路径在 dump 过程中再次 fault，导致 runtime-frame 噪音反复出现”的问题。
- primary failure 仍然存在，因为 `native_explicit_rethrow.cj` 本身就是负向场景，目的就是验证错误边界。
- 修复后的行为是：
  - 保留第一现场
  - 对 nested fatal signal 做最小降级退出
  - 不再出现成串重复的 runtime-frame 次生噪音
