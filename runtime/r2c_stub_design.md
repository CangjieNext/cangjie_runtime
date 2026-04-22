# `R2C` 边界设计说明

本文讨论一个更正式的方案：为 `ApplyCangjieMethodStub` 这类 `runtime -> cj` 调用链单独定义 `R2C` 边界，而不是继续复用 `N2C` 语义。

## 1. 背景

当前 `x86_64` 原型为了尽快验证机制，已经把 `ApplyCangjieMethodStub` 临时接成了 `N2C` 风格边界：

- 进入 stub 前保存 top-context
- 进入内层 `cj` 后把 TLS 顶层上下文切到 managed
- unwind 命中 `ApplyCangjieMethodStub` 时按 slot 恢复 caller

这个原型已经证明机制可行：

- `ApplyCJMethodImpl`
- `ApplyCJMethod`
- `MCC_ApplyCJStaticMethod`

这段 runtime C++ 调用链可以被整体绕过，不再继续依赖 `IsRuntimeFrame()`。

但这只是机制复用，不是语义上最合适的长期方案。

## 2. 为什么复用 `N2C` 不够理想

`N2C` 的本意是：

- native 侧调用 Cangjie 代码

而 `ApplyCangjieMethodStub` 的真实语义是：

- runtime 侧通过反射/方法描述调用 Cangjie 代码

两者虽然在“进入 managed”这件事上相似，但来源不同：

- `N2C` 的 caller 是普通 native
- `R2C` 的 caller 是 runtime

这会带来几个问题。

### 2.1 语义不清晰

如果把 `ApplyCangjieMethodStub` 归到 `N2C_STUB`：

- 代码层面看起来像“外部 native 调 cj”
- 实际上 caller 是 runtime 自己

这种混用在原型阶段可以接受，但长期会让边界语义变模糊。

### 2.2 后续维护容易误判

当别人看到 `FrameType::N2C_STUB` 时，很容易默认：

- 这个边界来自 runtime 外部 native 代码

但 `ApplyCangjieMethodStub` 并不满足这个假设。

后续如果在：

- EH
- GC
- signal unwind
- 栈打印
- profile

这些模块里继续把 `N2C` 当“外部 native 边界”处理，就会把 `runtime -> cj` 这条链路也误纳进去。

### 2.3 Android 扩平台时不够干净

当前 Android 问题的关键是：

- 普通 runtime 帧不稳定地依赖 `IsRuntimeFrame()`

`ApplyCangjieMethodStub` 这条链的目标其实是：

- 把一段 runtime 调用链压缩成独立边界

如果继续借用 `N2C`，虽然能工作，但设计上看不出“这是 runtime 自己的边界压缩机制”。

## 3. 为什么新增 `R2C` 更合理

新增 `R2C` 的核心价值是：**把 `runtime -> cj` 从 `native -> cj` 中拆出来，成为语义独立的一类边界。**

### 3.1 边界语义准确

`R2C` 明确表示：

- caller 在 runtime
- callee 在 managed Cangjie

这和：

- `C2R`
- `C2N`
- `N2C`

形成一套更对称的桥接边界模型。

### 3.2 更容易做平台一致实现

`ApplyCangjieMethodStub` 在：

- `x86_64_linux`
- `aarch64_linux`
- `arm_linux`
- Apple 平台
- Windows

本来就都有对应 stub。

如果定义独立 `R2C` 语义，后续每个平台都可以按统一思路实现：

- 特殊 stub 识别
- slot 保存 caller context
- unwind 命中边界时恢复 caller

而不是每个平台都去理解“为什么这个 stub 要伪装成 `N2C`”。

### 3.3 更利于 Android 闭环

Android 真正要解决的是：

- `cj -> runtime -> cj` 这条链里，中间 runtime C++ 帧不能再继续依赖 `IsRuntimeFrame()`

把 `ApplyCangjieMethodStub` 升级为 `R2C` 边界后，这条链会变成：

```text
inner cj -> R2C boundary -> outer C2R boundary -> outer cj
```

这比“把 `ApplyCangjieMethodStub` 伪装成 `N2C`”更直观，也更适合作为 Android 上的正式修复机制之一。

## 4. 建议的 `R2C` 设计

## 4.1 新增独立帧类型

在 `FrameType` 中新增：

- `R2C_STUB`

作用：

- 栈分类时不再把 `ApplyCangjieMethodStub` 混入 `N2C_STUB`
- 后续各模块可以针对 `R2C` 做独立逻辑

## 4.2 新增 `R2CSlotData`

新增数据结构：

- `pc`
- `fa`
- `status`

用途和 `N2CSlotData` 类似，但语义不同：

- 它保存的是“runtime 调入 managed 之前的 caller context”

## 4.3 新增 `R2CFrame`

和 `N2CFrame` / `C2RFrame` 类似，负责：

- 按当前架构的 stub frame layout 定位 `R2CSlotData`

这样 `UnwindContext` 在命中 `R2C_STUB` 时，可以直接取到 slot。

## 4.4 新增保存/恢复接口

建议新增：

- `MRT_SaveTopManagedContextToR2CStub`
- `MRT_RestoreTopManagedContextFromR2CStub`

进入 `ApplyCangjieMethodStub` 前：

- 保存当前 TLS 顶层上下文到 `R2CSlotData`
- 把 TLS 顶层切到 managed

从内层 `cj` 返回 runtime 后：

- 再从 `R2CSlotData` 恢复 TLS 顶层上下文

## 4.5 新增专用 unwind 逻辑

在 `UnwindContext` 中新增：

- `UnwindToCallerContextFromR2CStub()`

语义：

- unwind 命中 `R2C_STUB` 时，直接用 `R2CSlotData` 恢复 caller

这样：

- `ApplyCJMethodImpl`
- `ApplyCJMethod`
- `MCC_ApplyCJStaticMethod`

这段 runtime C++ 调用链就会被整体跳过。

## 4.6 `ApplyCangjieMethodStub` 按 `R2C_STUB` 识别

`StackInfo::AnalyseAndSetFrameType()` 里：

- 先识别 `ApplyCangjieMethodStub`
- 再把它标成 `FrameType::R2C_STUB`

而不是：

- `RUNTIME`
- 或 `N2C_STUB`

## 5. 与当前 `N2C` 复用方案的对比

### 5.1 复用 `N2C`

优点：

- 实现快
- 可以快速做 `x86_64` 原型
- 能直接复用现有 slot 和 unwind 逻辑

缺点：

- 语义借用，不够准确
- 后续 EH/GC/signal/print 容易把 `R2C` 和真正的 `N2C` 混为一类
- 扩平台时可读性和可维护性一般

### 5.2 新增 `R2C`

优点：

- 语义准确
- 和 `C2R/C2N/N2C` 形成更完整的桥接模型
- 更适合作为正式机制推广到 Android / aarch64 / 其它平台

缺点：

- 需要补新的 frame type / slot / API / unwind 逻辑
- 实现成本明显高于复用 `N2C`

## 6. 可行性

技术上可行，而且当前 `x86_64` 原型已经证明了最关键的一点：

- `ApplyCangjieMethodStub` 这条链确实可以通过 slot-based caller 恢复，把中间 runtime C++ 帧整体跳过

所以新增 `R2C` 不是从零开始设计，而是：

- 把已经验证过的原型机制从“借用 `N2C`”收敛成“独立 `R2C`”

## 7. 工作量

如果只做 `linux x86_64` 首版，工作量中等，主要包括：

1. `FrameType` 增加 `R2C_STUB`
2. 增加 `R2CSlotData` / `R2CFrame`
3. 增加 `MRT_SaveTopManagedContextToR2CStub`
4. 增加 `MRT_RestoreTopManagedContextFromR2CStub`
5. 增加 `UnwindToCallerContextFromR2CStub`
6. 修改 `ApplyCangjieMethodStub`
7. 补回归和文档

如果要扩到全平台，工作量会上升到：

- 多架构汇编适配
- 不同平台 unwind 机制适配
- 回归验证

## 8. 建议推进顺序

建议顺序如下：

1. 保留当前 `x86_64` 的 `N2C` 复用原型，作为可工作的验证版本
2. 在此基础上设计并实现独立 `R2C`
3. 先在 `linux x86_64` 上把 `R2C` 跑通
4. 再迁到 `aarch64_linux/android`
5. 最后再评估是否扩到其它平台

## 9. 结论

从“快速验证”角度看：

- 复用 `N2C` 是务实的

从“正式设计”角度看：

- 新增 `R2C` 更合理

因为 `ApplyCangjieMethodStub` 的真实语义不是普通 `N2C`，而是一个独立的：

- `runtime -> cj`

桥接边界。

如果后续准备把这套机制作为 Android / aarch64 的正式方案推进，建议最终收敛到独立 `R2C`，而不是长期保留“用 `N2C` 伪装 `R2C`”的实现。
