# `ApplyCangjieMethodStub` 专用回栈边界设计

本文说明为什么“仅把 `ApplyCangjieMethodStub` 识别成特殊 stub”还不够，以及如果要闭环 `cj -> runtime -> cj` 场景，应该怎样把它升级成真正的回栈边界。

## 1. 问题背景

当前已经为 `ApplyCangjieMethodStub` 增加了显式 `unwindPC*` 标记，因此它本身不再依赖 `IsRuntimeFrame()` 才能识别为 runtime 相关帧。

但这只能解决 stub 自己的问题，不能解决整个反射调用链的问题。

典型现场如下：

```text
inner cj
  -> ApplyCangjieMethodStub
  -> MethodInfo::ApplyCJMethodImpl
  -> MethodInfo::ApplyCJMethod
  -> MCC_ApplyCJStaticMethod
  -> CJ_MCC_ApplyCJStaticMethod
  -> outer cj
```

其中：

- `ApplyCangjieMethodStub` 是固定汇编 stub
- `MethodInfo::ApplyCJMethodImpl`
- `MethodInfo::ApplyCJMethod`
- `MCC_ApplyCJStaticMethod`

都是普通 runtime C/C++ 帧

所以即使 `ApplyCangjieMethodStub` 自己已经能 special-case 识别，`cj -> runtime -> cj` 这条链路里仍然会出现一段普通 runtime 调用链。

在 Android `extractNativeLibs=false` 场景下，如果 runtime 地址范围初始化失败，这些普通 runtime 帧仍然会落到 `IsRuntimeFrame()`，问题依旧可能发生。

## 2. 当前实现为什么不够

当前 `StackInfo::AnalyseAndSetFrameType()` 的行为可以概括为：

1. 先识别特殊 stub
2. 再识别普通 runtime frame
3. 否则按 managed 等其它类型处理

因此仅把 `ApplyCangjieMethodStub` 识别成特殊 stub，只能保证：

- `ApplyCangjieMethodStub` 自己不误判成 managed

但不能保证：

- `MethodInfo::ApplyCJMethodImpl`
- `MethodInfo::ApplyCJMethod`
- `MCC_ApplyCJStaticMethod`

这些 runtime 帧不再依赖 `IsRuntimeFrame()`

换句话说，当前方案只修了一个点，没有把中间 runtime 调用链整体压掉。

## 3. 目标

目标不是“继续给更多 runtime C++ 函数补 special-case”，而是把这条链路收敛成一个真正的边界：

```text
inner cj -> ApplyCangjieMethodStub boundary -> outer C2R boundary -> outer cj
```

而不是：

```text
inner cj -> ApplyCangjieMethodStub -> MethodInfo::ApplyCJMethodImpl -> MethodInfo::ApplyCJMethod
        -> MCC_ApplyCJStaticMethod -> CJ_MCC_ApplyCJStaticMethod -> outer cj
```

这样做以后：

- `ApplyCJMethodImpl/ApplyCJMethod/MCC_ApplyCJStaticMethod` 不再参与回栈分类
- Android 场景不再依赖这些普通 runtime 帧能否命中 `IsRuntimeFrame()`

## 4. 建议方案

建议把 `ApplyCangjieMethodStub` 按“`runtime -> managed` 专用边界”实现成 `N2C` 风格协议。

### 4.1 进入 stub 前保存当前 top-context

在 `ApplyCangjieMethodStub` 进入内层 Cangjie 方法前：

- 取当前 TLS `UnwindContext`
- 把其中的 `pc/fa/status` 保存到 stub slot
- 然后把当前 TLS 上下文切到 managed

如果外层调用链是：

```text
outer cj -> C2R -> runtime -> ApplyCangjieMethodStub -> inner cj
```

那么此时 slot 中保存的通常就是外层 `C2R` 边界上下文。

### 4.2 内层 `cj` 执行期间按 managed 起点处理

内层 Cangjie 方法执行时，TLS 顶层上下文已经被切到 managed。

这意味着：

- 内层抛异常
- `getStackTrace()`
- `fillInStackTrace()`

都会先展开内层 managed 帧，而不是直接从 runtime 顶层上下文起步。

### 4.3 unwind 命中 `ApplyCangjieMethodStub` 时直接恢复 caller

当栈遍历走到 `ApplyCangjieMethodStub`：

- 不再把它仅当成 `RUNTIME`
- 而是把它按 `N2C` 风格边界处理
- 直接从 slot 恢复 caller `pc/fa/status`

如果 slot 里保存的是外层 `C2R`，那么 unwind 会直接跳到：

```text
ApplyCangjieMethodStub -> outer C2R boundary
```

于是 `MethodInfo::ApplyCJMethodImpl/ApplyCJMethod/MCC_ApplyCJStaticMethod` 这段 runtime 调用链会被整体跳过。

### 4.4 从内层 `cj` 返回 runtime 后恢复原 TLS top-context

内层方法正常返回后：

- 从 slot 恢复 TLS 上下文
- 线程继续回到外层 runtime 调用链

这样不会破坏运行时执行流程。

## 5. 为什么这个方案比“继续补 runtime frame 识别”更合理

原因有三点：

### 5.1 这条链路本来就是一个语言边界

`ApplyCangjieMethodStub` 的语义不是普通 runtime 函数，而是：

- runtime 侧通过反射/方法描述调用 Cangjie 代码

它天然更像 `N2C` 这类边界，而不是普通 runtime frame。

### 5.2 可以整体消掉对中间 runtime C++ 帧的依赖

如果只补 `ApplyCangjieMethodStub` 自身识别，中间那段 runtime C++ 调用链仍然存在。

如果把它做成真正的边界协议，这段链可以整体绕过。

### 5.3 更贴近 Android 问题的根因

Android 上真正不稳的是“普通 runtime 帧依赖地址范围识别”。

所以更有效的方向是：

- 尽量减少这类普通 runtime 帧出现在回栈分类路径里

而不是不断给单个函数补特判。

## 6. `x86_64` 原型建议

`linux x86_64` 上可以先做最小原型：

1. 在 `ApplyCangjieMethodStub` 中复用现有 `N2C` slot 布局
2. 进入内层方法前调用 `MRT_SaveTopManagedContextToN2CStub`
3. 返回后调用 `MRT_RestoreTopManagedContextFromN2CStub`
4. unwind 命中 `ApplyCangjieMethodStub` 时，将其按 `N2C_STUB` 处理

这样原型无需再新增一套新的 slot 结构，能先验证核心语义是否成立。

## 7. 预期结果

做完后，`cj -> runtime -> cj` 的预期回栈形态应为：

```text
inner cj -> ApplyCangjieMethodStub boundary -> outer C2R boundary -> outer cj
```

而不是把 `MethodInfo::ApplyCJMethodImpl/ApplyCJMethod/MCC_ApplyCJStaticMethod` 展开出来。

对于 Android 场景，这意味着：

- `ApplyCJMethod*` 这段 runtime 调用链不会再依赖 `IsRuntimeFrame()`
- 即使 runtime 地址范围识别不稳定，这条反射链路也更不容易误判 crash

## 8. 限制

这套方案解决的是：

- `ApplyCangjieMethodStub` 相关的 `runtime -> managed` 反射调用链

它不能替代：

- Android runtime 本体地址范围识别修复
- 其它普通 runtime frame 的识别修复

也就是说，它是“缩短一类问题链路”的机制增强，不是根因修复的替代品。
