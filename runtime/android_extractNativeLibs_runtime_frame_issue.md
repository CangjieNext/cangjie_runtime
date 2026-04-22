# Android `extractNativeLibs=false` 场景下 runtime 栈帧识别问题

本文说明一个 Android 动态链接场景下的 runtime 回栈问题：  
当 `android:extractNativeLibs=false` 时，`libcangjie-runtime.so` 可能不会以独立 `.so` 文件路径出现在 `/proc/self/maps` 中，导致 runtime 地址范围初始化失败，进而把 runtime 栈帧误判成 Cangjie 栈帧，最终在解析元数据时 crash。

## 1. 问题现象

Android 应用开启：

```xml
android:extractNativeLibs="false"
```

后，`libcangjie-runtime.so` 可能被直接压缩存放在 `base.apk` 内并由系统直接映射使用。

这时 `/proc/self/maps` 中的可执行映射路径不一定包含：

- `libcangjie-runtime.so`

而可能只表现为：

- `base.apk`
- 或 apk 内部映射路径

于是 runtime 启动阶段无法正确识别自身代码区间。

后续栈遍历时：

- `StackManager::IsRuntimeFrame(pc)` 返回 false
- runtime 普通帧被误判为 `MANAGED`
- 再走 `ResolveProcInfo()` / 元数据解析时，把 runtime 的 `pc/fa` 当成 Cangjie 函数处理
- 最终触发 crash

## 2. 当前实现为什么会失败

相关代码在：

- `src/StackManager.cpp`
- `src/UnwindStack/StackInfo.cpp`

当前 Android / OHOS 动态链接分支位于：

- `StackManager::InitAddressScope()`

逻辑是：

1. 打开 `/proc/self/maps`
2. 逐行扫描映射
3. 只保留带执行权限的映射
4. 取 basename
5. 匹配 `libcangjie-runtime.so`
6. 统计出 `rtStartAddr/rtEndAddr`

核心问题就在第 4、5 步：

- 当前实现依赖 `/proc/self/maps` 里能直接看到 `libcangjie-runtime.so`
- 但 `extractNativeLibs=false` 时，这个假设不成立

因此 `rtStartAddr/rtEndAddr` 可能保持默认值，或者没有正确覆盖 runtime 的真实代码段。

## 3. 错误传播链路

这个问题不是单点失败，而是会沿着回栈流程逐层放大。

### 3.1 runtime 地址范围初始化失败

由于没有从 `/proc/self/maps` 中匹配到 `libcangjie-runtime.so`：

- `rtStartAddr`
- `rtEndAddr`

没有被正确设置。

### 3.2 普通 runtime frame 无法识别

后续 `MachineFrame::IsRuntimeFrame()` 会调用：

- `StackManager::IsRuntimeFrame(pc)`

但此时 `pc` 不落在合法的 runtime 区间中，于是判断失败。

### 3.3 栈帧分类错误

`StackInfo::AnalyseAndSetFrameType()` 的顺序是：

1. 先识别特殊 stub
2. 再识别普通 runtime frame
3. 否则按 managed 等其它情况处理

因此对于“不属于特殊 stub 的普通 runtime 帧”，一旦 `IsRuntimeFrame()` 返回 false，就会掉到非 runtime 分支。

结果是：

- 原本应标成 `FrameType::RUNTIME`
- 实际被误标成 `FrameType::MANAGED`

### 3.4 后续元数据解析 crash

一旦 runtime 帧被当成 managed 帧，后续代码会尝试：

- 解析函数起始地址
- 找 `FuncDesc`
- 读取 stack map / method map / LSDA / 元数据

但 runtime 帧并不满足 Cangjie managed 函数的元数据约束，于是最终导致 crash。

## 4. 为什么特殊 stub 往往不先炸

`C2N/N2C/C2R/StackGrow/Safepoint/Exclusive` 这些特殊 runtime stub，通常优先通过固定 `unwindPC*` label 识别。

因此很多情况下，先出问题的不是这些 bridge stub，而是：

- runtime 内部的普通 C/C++ 函数帧
- 非专用 stub 的 runtime 帧

这些帧依赖的是：

- `StackManager::IsRuntimeFrame(pc)`

所以 runtime 地址范围初始化失败时，最容易误判的正是这类“普通 runtime frame”。

## 5. 根因总结

根因不是“回栈算法本身不会识别 runtime”，而是：

- Android `extractNativeLibs=false` 改变了 native library 在进程中的呈现方式
- 当前实现又把“runtime 模块识别”绑定到了 `/proc/self/maps` 的文件名匹配
- 一旦 maps 中看不到 `libcangjie-runtime.so` 这个 basename，runtime 地址范围就初始化失败

所以本质问题是：

**runtime 模块归属识别过度依赖 `maps` 中文件名，而不是依赖已加载 ELF 本身。**

## 6. 建议修复方案

建议把 Android / OHOS 上 runtime 本体的地址范围初始化改成：

- 优先按“当前 runtime 符号地址反查所属 ELF”
- `/proc/self/maps` 仅作为 fallback

### 6.1 推荐方案

做法：

1. 取一个确定属于 runtime 的代码地址，例如：
   - `&StackManager::IsRuntimeFrame`
   - 或某个 `MRT_*` / `CJ_MCC_*` 符号
2. 使用 `dl_iterate_phdr`
3. 遍历当前进程所有已加载 ELF 模块
4. 找到“其可执行 `PT_LOAD` 段包含该符号地址”的那个模块
5. 汇总这个模块所有带 `PF_X` 的 `PT_LOAD` 段
6. 得到 runtime 的真实执行地址范围
7. 用该范围初始化 `rtStartAddr/rtEndAddr`
8. 如果失败，再回退到当前 `/proc/self/maps` 逻辑

### 6.2 为什么这个方案正确

因为它不依赖：

- `/proc/self/maps` 里的 basename
- `.so` 是否被解压到文件系统
- `base.apk` 中路径长什么样

它只依赖一件事：

- “这个地址属于哪个已加载 ELF”

而这是 loader 在 `extractNativeLibs=false` 场景下依然能稳定提供的信息。

## 7. 不推荐的方案

### 7.1 继续修补 `/proc/self/maps` 字符串匹配

例如：

- 从 basename 匹配改成整行 `Find("libcangjie-runtime.so")`
- 尝试兼容 `apk!lib/...`

问题是：

- 某些场景下 maps 里可能根本没有 `libcangjie-runtime.so` 这个字符串
- 这类修补容易越补越碎

因此它最多只能做 fallback，不能作为主方案。

### 7.2 识别失败时把 unknown frame 一律当 runtime

这种方案风险很大：

- 会扩大误判范围
- 可能把真正的 native / managed 帧吞掉
- 会掩盖更深层问题

不适合回栈底层设施。

## 8. 建议代码改动点

建议改动：

- `src/StackManager.cpp`

建议新增：

1. Android / OHOS 专用 helper
   - 通过 `dl_iterate_phdr` + 目标符号地址反查模块
2. runtime 本体优先使用该 helper 初始化 `rtStartAddr/rtEndAddr`
3. 现有 `/proc/self/maps` 扫描逻辑保留为 fallback

可选增强：

- 当 Android / OHOS 上 `rtStartAddr/rtEndAddr` 初始化失败时打印显式错误日志
- 在 `IsRuntimeFrame()` 命中异常值时增加诊断日志，便于现场排查

## 9. 建议验证项

修复后至少验证以下场景：

1. Android，`extractNativeLibs=true`
2. Android，`extractNativeLibs=false`
3. runtime 普通函数帧能被识别为 `RUNTIME`
4. `C2N/N2C/C2R` stub 识别不受影响
5. 异常栈展开不再把 runtime 帧误判成 managed
6. GC 扫栈、signal 栈打印、普通栈打印行为正常

## 10. 一句话总结

这个 crash 的根因不是栈展开逻辑本身，而是 Android `extractNativeLibs=false` 改变了 runtime 动态库在 `/proc/self/maps` 中的呈现形式，导致当前基于库名匹配的 runtime 地址范围初始化失效。  

正确的修复方向应该是：

- **用 runtime 内部符号地址反查所属 ELF 的可执行段范围**
- 而不是继续依赖 `/proc/self/maps` 里的 `libcangjie-runtime.so` 文件名
