# C2R 完整回栈机制评审短结论

## 1. 结论

`C2R` 完整回栈机制可以做，技术上可行，但它是一个中高成本的底层增强项，不适合作为 Android `extractNativeLibs=false` 问题的第一优先修复。

更准确地说：

- 作为 runtime 回栈鲁棒性增强：有价值，值得评估推进
- 作为当前 Android crash 的主修复手段：不建议优先采用

## 2. 要解决的问题

目标不是单纯“让 `C2RStub` 多一个 slot”，而是让：

- runtime helper 内部普通 runtime 帧不再参与 managed 元数据解析
- 回栈在 `C2R` 边界上稳定收敛
- `GC / EH / signal / print` 在 runtime helper 内部触发时，都能从稳定边界开始展开

## 3. 方案核心

完整方案需要同时具备四部分：

1. `C2RSlotData`
   在 `C2RStub` 中保存 caller `pc/fa/status`

2. `C2RStub` 自定义 unwind
   回栈到 `C2R_STUB` 时，不再只按机器帧回退，而是按 slot 恢复 caller

3. TLS 顶层上下文切换
   进入 runtime helper 期间，把 TLS `UnwindContext` 切到 `C2R` 边界

4. 返回时清理/恢复上下文
   防止留下 stale top context

一句话概括：

- `N2C` 解决“边界处如何恢复 caller”
- `C2N` 解决“helper 内部触发回栈时从哪开始”
- `C2R` 要真正可用，必须同时具备这两类能力

## 4. 可行性判断

技术上可行。

原因：

- 现有 runtime 已有两套成熟范式可参考
  - `N2C` 的 slot-based unwind
  - `C2N` 的 TLS risky top-context
- `GC / EH` 当前本来也更依赖 stub 边界，而不是普通 runtime 帧

所以从模型上看，`C2R` 完整机制是顺着现有设计体系扩展，而不是另起炉灶。

## 5. 主要价值

主要价值有三点：

1. 提升 runtime helper 区段回栈鲁棒性
2. 降低普通 runtime 帧误判成 managed 的风险
3. 让 `C2R / C2N / N2C` 三类边界机制更对称，更容易维护

它的定位更适合作为：

- 底层回栈增强
- 边界协议补全
- 风险隔离层

而不是单独替代 runtime 地址范围识别。

## 6. 主要风险

风险主要集中在三类：

1. `C2RStub` 栈布局改动
   会影响 GC stub root 扫描、寄存器偏移、CFI 信息

2. 跨路径联动
   需要验证 `C2R/N2C/C2N/Safepoint/StackGrow/Signal` 的嵌套场景

3. 多平台汇编差异
   Linux、Apple、Windows 的实现和验证成本差异明显

因此它不是小补丁，而是一个新的边界上下文协议。

## 7. 兼容性影响

兼容性影响需要单独评估，不能默认“只是内部增强，因此天然兼容”。

可以分四层理解：

1. 对外接口兼容性
   如果不改 `CJ_MCC_*` / `MRT_*` 的符号、参数和调用约定，对外 API 兼容性影响较小。

2. runtime 内部 ABI 兼容性
   如果 `C2RStub` 增加 slot，stub frame layout、寄存器偏移、`CalleeSavedStubFrameSize` 都会变化。
   这会直接影响 GC 对 stub frame 的偏移读取逻辑，因此内部 ABI 风险较大。

3. 新旧版本混用兼容性
   “旧编译产物 + 新 runtime” 有机会兼容，但不能默认成立，需要专项验证。

4. 调试与观测兼容性
   栈打印、signal 栈、profile 调用链形态都会变化，属于行为兼容性变化。

评审上可以把结论收敛为：

- 对外接口兼容性：较小
- runtime 内部 ABI 兼容性：较大
- 混用兼容性：需要专项验证
- 调试观测兼容性：一定会变化

## 8. 工作量评估

如果只做：

- `aarch64_linux`
- Android / Linux 主线
- 原型级验证

工作量大致为：

- `1 ~ 2 周`

如果做成正式全平台版本，预计为：

- `2 ~ 4 周`

甚至更长，主要取决于：

- 多架构汇编改造
- 回归验证
- 平台测试环境准备

## 9. 平台建议

建议推进顺序：

1. `aarch64_linux`
2. `x86_64_linux`
3. Android 主线验证
4. 其它 Linux 变体
5. Apple 平台
6. Windows

不建议一开始就全平台同时展开。

## 10. 对当前 Android crash 的建议

当前 Android `extractNativeLibs=false` crash 的根因，仍然是：

- runtime 地址范围识别失败
- 普通 runtime 帧被误判成 managed

因此最优先修复仍应是：

- 修正 Android runtime 地址范围识别逻辑

`C2R` 完整机制更适合作为第二阶段增强项，而不是第一阶段止血方案。

## 11. 建议决策

如果当前目标是：

- 快速止 Android crash

建议：

- 先修 runtime 地址范围识别

如果当前目标是：

- 系统性增强 runtime 回栈鲁棒性

建议：

- 立项做 `C2R` 完整机制
- 先做 `aarch64_linux/android` 原型
- 验证通过后再决定是否扩成全平台正式机制

## 12. 一句话结论

`C2R` 完整回栈机制值得做，但它属于“增强型基础设施项目”；  
当前 Android crash 应优先修根因，`C2R` 方案适合作为下一阶段的体系化增强。

## 13. 当前 `linux x86_64` 进展补充

当前 `linux x86_64` 已经有一版可运行原型，状态可以概括为：

- `C2RSlotData + TLS 顶层上下文切换` 已打通
- runtime helper 内部发起回栈时，起点会直接收敛到 `C2R` 边界
- `cj -> runtime -> cj` 嵌套边界已经有正式用例覆盖

但也要明确：

- 目前还没有 `UnwindToCallerContextFromC2RStub`
- `C2R_STUB` 往上仍然靠普通机器回栈
- 因此这版是“可运行原型”，不是“完整 custom unwind 成品”

当前 `release` 形态下已完成一轮非 `StackGrow` 回归：

- HLT `stack001-006`
- HLT `stack201-214`
- HLT `stack401-416`

并新增了 `stack214` 用于验证：

- `cj -> runtime(std.reflect::StaticFunctionInfo::apply) -> cj`

所以对评审更准确的说法应是：

- `x86_64_linux` 原型已经可用，且已有一轮关键验证
- 后续是否补完整 `C2R` custom unwind，以及是否继续扩到 `StackGrow/aarch64/Android`，再进入下一阶段决策
