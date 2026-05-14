# C2R Custom Unwind 当前阻塞点分析

本文记录 `linux x86_64` 上尝试补 `UnwindToCallerContextFromC2RStub()` 的实际结果，说明为什么当前实现不能直接上线，以及后续如果继续推进，需要先补什么信息。

## 1. 当前结论

当前不能直接为 `C2R_STUB` 增加类似 `N2C_STUB` 的 slot-based caller 恢复逻辑。

原因不是“代码还没写完”，而是：

- 现有 `C2RSlotData + TLS top-context` 足以支持“在线程位于 `C2RStub` 调用的 runtime 侧辅助函数/调用链内部时，从 `C2R` 边界起步”
- 但现有信息不足以区分：
  - 当前正在处理的是“TLS 顶层 risky C2R 边界”
  - 还是“普通栈遍历过程中遇到的一个中间 `C2R` 帧”

一旦把两者混用，`cj -> runtime -> cj` 这种嵌套场景就会把内层 managed 帧直接跳掉。

## 2. 这次尝试做了什么

本轮尝试是在 `UnwindContext::UnwindToCallerContext()` 中对 `FrameType::C2R_STUB` 增加专门处理：

- 读取 `C2RSlotData`
- 在 `slot.status` 为 `RISKY / SIGNAL / UNKNOWN` 时，直接按 slot 恢复 caller
- 否则回退到普通机器回栈

从表面上看，这和 `N2C_STUB` 的处理方式相似。

## 3. 实际失败现象

### 3.1 单层 reflect 嵌套被压扁

原本正常的链路：

- `ReflectBridge::leaf`
- `ReflectBridge::entry`
- `StaticFunctionInfo::apply`
- `main`

在错误版本里会塌成：

- `StaticFunctionInfo::apply`
- `main`

也就是 `leaf` 和 `entry` 两层 managed 帧直接丢失。

### 3.2 双层 reflect 嵌套同样失败

原本正常的链路：

- `NestedReflectInner::leaf`
- `NestedReflectInner::entry`
- `StaticFunctionInfo::apply`
- `NestedReflectOuter::entry`
- `StaticFunctionInfo::apply`
- `main`

错误版本中同样会退化成只剩最外层 `apply -> main`。

### 3.3 StackGrow 未受影响

`StackGrow07` 等 case 仍然通过，说明问题不在通用机器回栈，也不在 `StackGrow` 本身，而是集中在：

- `C2R / N2C`
- `cj -> runtime -> cj`
- 反射 callback 这类嵌套边界

## 4. 根因分析

### 4.1 `N2C` 和 `C2R` 的 slot 语义并不对称

`N2CSlotData` 的语义是：

- 这个 `N2C_STUB` 本身就是一个“从 native 回到 managed”的显式边界
- 当 unwind 走到它时，slot 里保存的是“应当恢复到的 caller managed context”

所以在 `N2C_STUB` 处按 slot 恢复 caller，语义天然成立。

### 4.2 `C2RSlotData` 当前保存的是“进入 `C2RStub` 调用链前的顶层上下文”

当前 `C2RSlotData` 的用途其实是：

- 进入 `C2RStub` 调用链前，保存当前 TLS 顶层 `UnwindContext`
- 从 `C2RStub` 调用链返回后，恢复 TLS 顶层 `UnwindContext`
- 当线程位于这段调用链内部发起回栈时，起点可以直接落在 `C2R` 边界

也就是说，它首先是一个“top-context 保存槽”，而不是“任意 `C2R` 帧都可用来恢复 caller 的通用 unwind 槽”。

### 4.3 普通栈里的 `C2R` 帧会遇到“历史 top-context”

在 `cj -> runtime -> cj` 场景里：

1. 外层 `cj -> runtime`
   - 进入 `C2RStub`
   - `C2RSlotData` 保存的是外层 top-context
2. 中间 `runtime -> cj`
   - 通过 `N2CStub` 回调到内层 `cj`
3. 在内层 `cj` 里抛异常 / 打印栈
   - 栈遍历过程中会遇到这个外层 `C2R_STUB`

这时如果把这个 `C2R_STUB` 也当成“当前应该按 slot 恢复 caller 的边界”，就会直接恢复到它当年保存的 top-context，而不是继续按机器栈经过：

- `leaf`
- `entry`
- `apply`

结果就是中间 managed 帧被整体绕掉。

## 5. 为什么仅靠 `slot.status` 不够

这次尝试已经验证过，仅靠：

- `slot.status`
- 当前 frame type 是不是 `C2R_STUB`

无法可靠区分以下两种情况：

1. 顶层 risky context 正好指向当前 `C2R_STUB`
   - 这种情况下，按 slot 恢复是合理的
2. 普通栈遍历过程中遇到一个历史 `C2R_STUB`
   - 这种情况下，应该继续机器回栈，不能直接用 slot 跳

原因是：

- 两种情况都可能看到 `FrameType::C2R_STUB`
- slot 里都可能是 `RISKY`
- 但它们的语义完全不同

所以当前缺的不是“分支代码”，而是“区分这两种语义所需的额外信息”。

## 6. 当前可工作的边界

当前已经验证可工作的实现边界是：

- 保留 `C2RSlotData`
- 保留 `进入 C2RStub 调用链前保存 top-context`
- 保留 `在线程位于 C2RStub 调用链内部时，把 TLS 顶层切到 C2R 边界`
- 保留 `从 C2RStub 调用链返回后恢复 TLS 顶层`
- 不引入 `UnwindToCallerContextFromC2RStub()`

这套实现可以稳定支持：

- 在线程位于 `C2RStub` 调用的 runtime 侧辅助函数/调用链内部发起回栈时，从 `C2R` 边界起步
- 避免这段 runtime 调用链中的普通 runtime 帧误判成 managed
- `cj -> runtime -> cj`
- 双层 reflect 嵌套
- 非 `StackGrow` 和 `StackGrow` 的现有验证集

## 7. 如果后续要继续推进，需要先补什么

要继续做 `C2R custom unwind`，建议先补一个能明确标识“这个 `C2R` 是当前 top risky 边界”的额外判定信息，再决定是否让 `UnwindToCallerContextFromC2RStub()` 生效。

### 7.1 可选方向一：在 `C2RSlotData` 增加专用 flag

例如增加类似：

- `isTopRiskyBoundary`
- `isTopContextSnapshot`

语义明确区分：

- 只是保存 TLS top-context
- 还是当前允许按 slot 恢复 caller 的边界

优点：

- 信息贴近 stub 本身
- 对 `UnwindContext` 侵入较小

缺点：

- 需要非常清楚这个 flag 在嵌套 `C2R/N2C` 场景下如何维护

### 7.2 可选方向二：扩展 TLS `UnwindContextStatus`

例如在现有：

- `RELIABLE`
- `RISKY`
- `UNKNOWN`
- `SIGNAL_STATUS`

之外，再增加一个更细的状态，表示：

- 当前 unwind 起点是“由 `C2R` top-context 人工切进来的”

这样只有当“当前上下文本身就是 `C2R top-context`”时，才允许在 `C2R_STUB` 处走 slot 恢复。

优点：

- 语义集中在 unwind 入口

缺点：

- 需要检查 GC / EH / signal / print / thread snapshot 等所有依赖 `UnwindContextStatus` 的路径

### 7.3 可选方向三：引入 top-context 与 ordinary-frame 的显式绑定

例如：

- 在 TLS 中额外记录一个“当前 top risky C2R 的 frame address”
- 只有当正在 unwind 的 `C2R_STUB` 恰好等于这条记录时，才允许按 slot 恢复

优点：

- 语义最直接
- 能从根上区分“当前 top risky C2R”与“普通栈内 C2R”

缺点：

- 需要额外 TLS 状态
- 生命周期维护必须非常严格

## 8. 下一步建议

建议顺序如下：

1. 不再继续直接修改 `UnwindToCallerContextFromC2RStub()`
2. 先把“为什么当前不能直接补 `C2R custom unwind`”固化为设计约束
3. 选择一种“top risky C2R 标识”方案
4. 先做最小原型
5. 先验证以下 case，再决定是否继续扩：
   - `stack214`
   - `stack215`
   - `cj -> runtime -> cj` 本地脚本
   - `StackGrow`

## 9. 一句话结论

当前 `C2R custom unwind` 之所以不能直接补，不是因为 slot 不存在，而是因为：

- 现有 `C2RSlotData` 保存的是“top-context 快照”
- 不是“所有 `C2R_STUB` 都能安全使用的 caller 恢复槽”

如果后续要继续推进，必须先补一个额外标记，能区分：

- 顶层 risky `C2R`
- 普通栈内 `C2R`

否则 `cj -> runtime -> cj` 这种嵌套场景会持续被误跳。
