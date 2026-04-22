# `R2C` 实施方案

本文给出 `R2C` 的具体实施方案。目标是把当前 `ApplyCangjieMethodStub` 对 `N2C` 的临时复用，收敛成语义独立的 `runtime -> cj` 边界机制。

## 1. 目标

要解决的问题是：

- `cj -> runtime -> cj` 场景下
- `ApplyCJMethodImpl / ApplyCJMethod / MCC_ApplyCJStaticMethod`

这段 runtime C++ 调用链不应再依赖 `IsRuntimeFrame()` 识别。

目标形态是：

```text
inner cj -> R2C boundary -> outer C2R boundary -> outer cj
```

而不是：

```text
inner cj -> ApplyCangjieMethodStub -> runtime C++ chain -> outer C2R -> outer cj
```

## 2. 实施原则

实施时遵循以下原则：

1. 不直接推翻当前 `x86_64` 已验证通过的原型
2. 先在 `linux x86_64` 上把 `R2C` 跑通
3. 再迁移到 `aarch64_linux/android`
4. 不把 `R2C` 和现有 `N2C` 语义混在一起
5. 每一步都保留可回滚状态

## 3. 分阶段实施

## 3.1 第一阶段：抽离独立语义

这一阶段只做“模型层”的抽离，不先改所有架构。

改动点：

1. 在 `src/Common/StackType.h` 中新增：
   - `FrameType::R2C_STUB`
   - `R2CSlotData`
   - `R2CFrame`

2. `R2CSlotData` 建议先保持和 `N2CSlotData` 一致：
   - `pc`
   - `fa`
   - `status`

3. `R2CFrame::GetSlotData()` 先只实现 `linux x86_64`

阶段目标：

- 代码里已经能表达“这是 `R2C`，不是 `N2C`”
- 不再靠命名约定区分

## 3.2 第二阶段：补齐 `R2C` 保存/恢复接口

在 `src/UnwindStack/UnwindCApi.h/.cpp` 中新增：

- `MRT_SaveTopManagedContextToR2CStub`
- `MRT_RestoreTopManagedContextFromR2CStub`

接口行为：

### `MRT_SaveTopManagedContextToR2CStub`

进入 `ApplyCangjieMethodStub` 前：

- 读取 TLS `UnwindContext`
- 保存 `pc/fa/status` 到 `R2CSlotData`
- 将 TLS 顶层上下文切到 managed

### `MRT_RestoreTopManagedContextFromR2CStub`

从内层 `cj` 返回 runtime 后：

- 从 `R2CSlotData` 恢复 TLS `UnwindContext`

阶段目标：

- `R2C` 的 slot 生命周期独立于 `N2C`
- `ApplyCangjieMethodStub` 不再调用 `MRT_SaveTopManagedContextToN2CStub`

## 3.3 第三阶段：补齐 `R2C` 的专用 unwind

在 `src/UnwindStack/UnwindContext.cpp` 中新增：

- `UnwindToCallerContextFromR2CStub()`

行为：

- unwind 命中 `R2C_STUB` 时
- 直接从 `R2CSlotData` 恢复 caller `pc/fa/status`

同时修改 `UnwindToCallerContext()`：

- `N2C_STUB` 走 `UnwindToCallerContextFromN2CStub()`
- `R2C_STUB` 走 `UnwindToCallerContextFromR2CStub()`
- 其它帧继续走普通机器回栈

阶段目标：

- `R2C` 成为独立边界协议
- `ApplyCJMethod*` 那段 runtime 调用链整体被跳过

## 3.4 第四阶段：接入 `x86_64` 的 `ApplyCangjieMethodStub`

在 `src/arch/x86_64_linux/N2CStub.S` 中，把当前临时复用 `N2C` 的逻辑替换成真正的 `R2C`：

1. 进入内层 `cj` 前：
   - 调 `MRT_SaveTopManagedContextToR2CStub`

2. 返回 runtime 后：
   - 调 `MRT_RestoreTopManagedContextFromR2CStub`

3. `ApplyCangjieMethodStub` 继续保留 `unwindPCForApplyCangjieMethodStubStart/End`

4. 在 `StackInfo::AnalyseAndSetFrameType()` 中：
   - `IsApplyCangjieMethodStubFrame()` 命中后标记为 `FrameType::R2C_STUB`

阶段目标：

- `x86_64` 不再借用 `N2C_STUB`
- 栈分类、TLS 语义、slot 使用都收敛为独立 `R2C`

## 3.5 第五阶段：补 `aarch64_linux/android`

在 `aarch64_linux` 上做同样迁移：

- 定义 `R2CFrame` 的 slot 偏移
- 在 `ApplyCangjieMethodStub` 接入 `MRT_SaveTopManagedContextToR2CStub`
- 接入 `MRT_RestoreTopManagedContextFromR2CStub`
- 将 `ApplyCangjieMethodStub` 标成 `R2C_STUB`

这一步是 Android 真正相关的关键阶段。

阶段目标：

- `cj -> runtime -> cj` 不再依赖中间 runtime C++ 帧的地址范围识别

## 3.6 第六阶段：决定是否扩全平台

确认 `x86_64 + aarch64` 稳定后，再决定是否扩到：

- `arm_linux`
- Apple 平台
- Windows

不建议一开始就全平台并行推进。

## 4. 具体文件改动清单

首版 `x86_64` 预计会涉及：

- `src/Common/StackType.h`
- `src/UnwindStack/UnwindCApi.h`
- `src/UnwindStack/UnwindCApi.cpp`
- `src/UnwindStack/UnwindContext.cpp`
- `src/UnwindStack/StackInfo.cpp`
- `src/UnwindStack/MachineFrame.cpp`
- `src/arch/x86_64_linux/N2CStub.S`

迁到 `aarch64` 后还会涉及：

- `src/arch/aarch64_linux/N2CStub.S`

如果扩到更多平台，再补对应平台的 `N2CStub.S` 或平台特定 unwind 文件。

## 5. 验证方案

## 5.1 现有用例回归

至少回归：

- `stack214`
- `stack215`

验证：

- 单层 reflect 嵌套
- 双层 reflect 嵌套

预期：

- 栈中保留内层 managed 帧
- 中间 `ApplyCJMethod*` runtime C++ 链不需要参与分类

## 5.2 自造探针验证

建议保留一个最小探针，专门验证：

```text
outer cj -> C2R -> runtime -> R2C -> inner cj -> throw/getStackTrace
```

验证点：

- `R2C` 命中后是否恢复到外层 `C2R`
- 是否丢失 `leaf/entry`
- 是否错误跳过内层 managed 帧

## 5.3 EH 验证

验证场景：

- 内层 `cj` 抛异常并由内层 `catch`
- 内层 `cj` 抛异常向外传播

重点看：

- 是否错误从外层 runtime top-context 起步
- 是否破坏现有 `Exception.cpp` 中针对 `C2R` 的规避逻辑

## 5.4 栈打印验证

验证：

- `getStackTrace()`
- `fillInStackTrace()`
- `printStackTrace()`

重点看：

- 输出中是否仍保留预期的 `apply`
- 是否把内层 managed 帧压掉
- 是否出现新的 demangle/分类异常

## 5.5 Android 场景验证

迁到 `aarch64_android` 后重点验证：

- `extractNativeLibs=false`
- `ApplyCangjieMethodStub` 反射链
- 中间 runtime C++ 帧不再触发 `IsRuntimeFrame()` 依赖

## 6. 风险点

主要风险如下：

### 6.1 `R2C` 与 `N2C` 混用残留

如果改动不彻底，容易出现：

- 一部分路径已经标成 `R2C_STUB`
- 另一部分路径还在按 `N2C_STUB` 处理

这会让行为不一致。

### 6.2 slot 偏移错误

不同架构的 stub frame layout 不同。

如果 `R2CFrame::GetSlotData()` 偏移错了，会直接导致：

- 恢复错误 caller
- 栈丢帧
- crash

### 6.3 EH 路径被打坏

之前已经出现过：

- 使用错误 top-context 会把内层 managed 帧整体跳掉

所以 `R2C` 引入后，EH 必须重点验证。

### 6.4 观测行为变化

即使功能正确，栈形态也会变化：

- 某些 runtime C++ 帧不再出现
- 调试时看到的链会更短

这属于预期变化，但需要在文档中提前说明。

## 7. 回滚策略

建议每个阶段都独立提交，便于回退：

1. 先提交 `R2C` 数据结构和接口框架
2. 再提交 `x86_64` 接入
3. 再提交 `aarch64` 接入
4. 最后提交 Android 验证和文档

如果某一步打坏嵌套场景，只回退该阶段即可。

## 8. 预计工作量

如果只做 `linux x86_64`：

- 约 `3-5` 个工程日

如果再做 `aarch64_linux/android`：

- 整体约 `1-1.5` 周

如果扩全平台：

- 约 `2-4` 周

## 9. 建议推进顺序

建议按以下顺序推进：

1. `x86_64` 上把 `R2C` 从设计层抽离出来
2. `x86_64` 上替换当前对 `N2C` 的复用
3. 回归 `stack214/215` 和异常场景
4. 迁移到 `aarch64_linux/android`
5. 验证 Android `extractNativeLibs=false`
6. 再评估是否扩平台

## 10. 结论

`R2C` 的实施不复杂到不可控，但也不是一个小补丁。

更准确地说，它是：

- 在已有 `x86_64` 原型基础上
- 把“借用 `N2C`”收敛成“独立 `R2C` 语义”

从实施角度看，最稳的路线是：

- 先收口 `x86_64`
- 再迁 `aarch64/android`
- 最后再谈全平台
