# Android / OHOS / Linux 动态链接场景下 runtime 栈帧识别

本文只分析一个问题：在 `runtime` 以动态库形式加载时，回栈过程中是如何判断“当前帧是不是 runtime 栈帧”的。

相关代码主要在：

- `src/UnwindStack/MachineFrame.cpp`
- `src/UnwindStack/StackInfo.cpp`
- `src/StackManager.cpp`
- `build/lds/*/cjnative_runtime.lds`

## 1. 总体结论

runtime 栈帧识别分两层：

1. 先识别“特殊 runtime stub 帧”
2. 如果不是特殊 stub，再判断它是不是“普通 runtime frame”

这里的“普通 runtime frame”并不是靠符号名逐帧解析，而是靠 `pc` 是否落在 runtime 代码地址范围内来判断。

## 2. 识别顺序

回栈时真正做分类的是 `StackInfo::AnalyseAndSetFrameType()`，见：

- `src/UnwindStack/StackInfo.cpp`

判断顺序是：

1. `IsN2CStubFrame()`
2. `IsSafepointHandlerStubFrame()`
3. `IsC2RStubFrame()`
4. `IsC2NStubFrame()`
5. `IsStackGrowStubFrame()`
6. `IsExclusiveStubFrame()`
7. `IsRuntimeFrame()`
8. 否则按 managed 或其他情况处理

也就是说，`N2C/C2N/C2R/StackGrow/Safepoint/Exclusive` 这些桥接桩不会落到“普通 runtime frame”判断里，而是优先按专用规则识别。

## 3. 特殊 stub 是怎么识别的

`MachineFrame.cpp` 里这几类 stub 都是按固定 label 直接比对 `ip`：

- `unwindPCForN2CStub`
- `unwindPCForC2NStub`
- `unwindPCForC2RStubStart / unwindPCForC2RStubEnd`
- `unwindPCForStackGrowStub`
- `unwindPCForSafepointHandlerStub`
- `unwindPCForExclusiveStub`

对应实现见：

- `src/UnwindStack/MachineFrame.cpp`

这类识别非常直接，属于“精确命中特定 runtime 桥接点”。

因此动态链接与否，对这些特殊 stub 的识别影响不大，因为它们本来就不是靠模块范围判断的。

## 4. 普通 runtime frame 的识别入口

普通 runtime frame 走：

- `MachineFrame::IsRuntimeFrame()`
- `StackManager::IsRuntimeFrame(Uptr pc)`

实现见：

- `src/UnwindStack/MachineFrame.cpp`
- `src/StackManager.cpp`

`IsRuntimeFrame(pc)` 在比较前会先做两件事：

1. 如果开了指针认证，先 `PtrauthStripInstPointer(pc)`
2. 如果开了 HWASAN，先 `Sanitizer::UntagAddr(pc)`

然后再判断 `pc` 是否落在已记录的地址区间内。

最终比较条件是：

- `rtStartAddr ~ rtEndAddr`
- `cjThreadStartAddr ~ cjThreadEndAddr`
- `cjcSoStartAddr ~ cjcSoEndAddr`
- `traceSoStartAddr ~ traceSoEndAddr`

也就是说，runtime 不只把 `libcangjie-runtime` 自己当作 runtime frame，还把一部分 runtime 相关组件一起纳入识别范围。

## 5. Android / OHOS 动态链接：靠 `/proc/self/maps`

在 `StackManager::InitAddressScope()` 里，Android / OHOS 的动态链接分支是：

- `#elif defined(__OHOS__) || defined(__ANDROID__)`

这里不会用链接脚本边界，而是：

1. 打开 `/proc/self/maps`
2. 逐行扫描当前进程映射
3. 只保留带执行权限的映射
4. 取映射 basename
5. 匹配 `libcangjie-runtime.so`
6. 把所有命中的执行段合并成一个总范围，写入 `rtStartAddr/rtEndAddr`

实现见：

- `GetEachSoAddrScope()` in `src/StackManager.cpp`
- `GetSoAddrScope()` in `src/StackManager.cpp`

这里的关键点是：

- 读取的是运行时真实映射地址
- 所以已经天然包含动态装载和 ASLR 之后的重定位结果
- 最终 `pc` 只要落在这些真实执行映射区间里，就会被当作 runtime frame

同一个文件里，`cjc`、`cjthread-trace` 也是类似方式识别的。

## 6. Linux 动态链接：靠链接脚本边界符号

Linux 动态链接分支和 Android / OHOS 不同。

在 `StackManager::InitAddressScope()` 里：

- `#if defined(COMPILE_DYNAMIC)`
- `#else // Linux`

Linux 动态链接场景不是去扫 `/proc/self/maps` 找 `libcangjie-runtime.so`，而是直接使用两个边界符号：

- `g_runtimeDynamicStart`
- `g_runtimeDynamicEnd`

代码是：

```cpp
StackManager::rtStartAddr = reinterpret_cast<Uptr>(&g_runtimeDynamicStart);
StackManager::rtEndAddr = reinterpret_cast<Uptr>(&g_runtimeDynamicEnd);
```

这两个符号来自 Linux 的链接脚本：

- `build/lds/aarch64_linux/cjnative_runtime.lds`
- `build/lds/x86_64_linux/cjnative_runtime.lds`

链接脚本里会把：

- `.cjnative.start` 放在 `.text` 前
- `.cjnative.end` 放在 `.text` 后

并在这两个 section 位置分别定义：

- `g_runtimeDynamicStart = .;`
- `g_runtimeDynamicEnd = .;`

因此 Linux 动态库加载后，`&g_runtimeDynamicStart` 和 `&g_runtimeDynamicEnd` 就构成了 runtime 代码段边界。

## 7. Linux 动态链接边界是怎么落到最终 so 里的

空锚点 section 由架构相关汇编文件提供，例如：

- `src/arch/aarch64_linux/CjNativeRuntimeStartAndEnd.S`
- `src/arch/x86_64_linux/CjNativeRuntimeStartAndEnd.S`

里面只做了两件事：

- 定义 `.cjnative.start`
- 定义 `.cjnative.end`

链接时，`cjnative_runtime.lds` 把这两个 section 固定包在 `.text` 前后，于是形成完整的 runtime text 范围。

所以 Linux 动态链接场景本质上是：

1. 运行库自身通过链接脚本导出 text 边界
2. 运行时直接拿边界符号地址作为 `rtStartAddr/rtEndAddr`
3. 回栈时用 `pc in [rtStartAddr, rtEndAddr]` 判定是否是 runtime frame

这里虽然没有读 `/proc/self/maps`，但仍然是可靠的，因为动态库装载后，符号地址本身就是重定位后的真实虚拟地址。

## 8. 为什么 Android / OHOS 和 Linux 的策略不同

从代码看，差异是刻意设计的：

- Android / OHOS：通过 `/proc/self/maps` 找 `libcangjie-runtime.so`
- Linux：通过链接脚本边界符号直接给出 runtime text 范围

可以把它理解为两种不同的“模块归属判定方式”：

- Android / OHOS 更偏向“按动态库映射名识别”
- Linux 更偏向“按 runtime 自己声明的 text 区间识别”

但两者最后都落到同一个接口：

- `StackManager::IsRuntimeFrame(pc)`

所以对上层回栈逻辑来说，它们是统一的。

## 9. 和特殊 stub 识别之间的关系

需要注意，`IsRuntimeFrame()` 不是第一层判断。

例如 `C2NStub` 自己的 `pc` 当然也在 runtime 地址范围里，但在实际分类时，`StackInfo::AnalyseAndSetFrameType()` 会先命中：

- `IsC2NStubFrame()`

于是它会被归为：

- `FrameType::C2N_STUB`

而不是普通的：

- `FrameType::RUNTIME`

同理 `N2CStub`、`C2RStub`、`Safepoint`、`StackGrow` 也都是这样。

这说明“runtime frame 识别”有两个精度层次：

- 特殊 stub：按专用 label 精确识别
- 普通 runtime：按地址范围粗识别

## 10. 总结

在 Android / OHOS / Linux 的动态链接场景下，runtime 栈帧识别的核心都是“看 `pc` 属不属于 runtime”，但初始化这个“runtime 地址范围”的方式不同：

- Android / OHOS：扫描 `/proc/self/maps`，按 `libcangjie-runtime.so` 的可执行映射建立范围
- Linux：使用链接脚本导出的 `g_runtimeDynamicStart/g_runtimeDynamicEnd` 作为 runtime text 边界

然后回栈分类时统一走：

1. 先按固定 label 识别特殊 runtime stub
2. 再用 `StackManager::IsRuntimeFrame(pc)` 判定普通 runtime frame

所以动态链接场景下的 runtime 栈帧识别，本质上依赖的是：

- 特殊 stub 的固定 `unwindPC*` 标记
- runtime 模块代码区间的正确初始化

而不是逐帧做复杂的符号解析。
