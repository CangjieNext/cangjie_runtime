# C2R 完整回栈机制分析

本文分析一个增强方案：  
为 `C2R` 路径补齐类似 `N2C` / `C2N` 的完整边界上下文机制，使 runtime 在进入 `C2R` helper 后，可以把整段 runtime helper 调用链压缩成一个稳定边界，避免普通 runtime 帧被误判成 managed 帧。

分析重点按以下几个维度展开：

- 如何实现
- 可行性
- 价值
- 工作量
- 全平台适配

相关现状代码主要在：

- `src/arch/*/CalleeSavedStub.S`
- `src/UnwindStack/UnwindContext.cpp`
- `src/UnwindStack/UnwindCApi.cpp`
- `src/UnwindStack/StackInfo.cpp`
- `src/UnwindStack/GcStackInfo.cpp`
- `src/Exception/Exception.cpp`

## 1. 背景

当前 runtime 已经有两套成熟的跨边界回栈机制：

- `C2N`
  - 进入 native 前，把边界上下文写入 TLS `UnwindContext`
- `N2C`
  - 在 `N2CStub` frame 中保存 caller `pc/fa/status`
  - 回栈到 `N2C_STUB` 时，按 slot 直接恢复 caller，而不是继续沿机器帧硬回退

但 `C2R` 目前不是这个模型。

现有 `C2RStub` 本质上只是 callee-saved stub：

- 保存若干寄存器
- 调用 runtime helper
- 返回

它有稳定的 frame layout，适合 GC 在边界上扫描 callee-saved roots，但没有：

- `C2RSlotData`
- caller context save/restore
- `UnwindToCallerContextFromC2RStub`
- 进入 runtime helper 时的 TLS top-context 切换

因此当 unwind 起点落在 runtime helper 内部普通 runtime 帧时，系统仍然依赖“普通 runtime frame 识别”来判断它是不是 runtime。  
一旦这个识别失败，例如 Android `extractNativeLibs=false` 场景下 runtime 地址范围没识别出来，就可能把普通 runtime 帧误判成 managed 帧。

## 2. 如何实现

如果要做“完整的 `C2R` 回栈机制”，建议不是只加一个 slot，而是补齐下面这整套协议。

## 2.1 新增 `C2RSlotData`

参考 `N2CSlotData`，新增一个 `C2RSlotData`，最小字段建议包含：

- `const uint32_t* pc`
- `FrameAddress* fa`
- `UnwindContextStatus status`

可选字段视实现策略决定：

- `lr`
- `shadowframe`
- `directCall`
- 调试用标记位

建议位置：

- `src/Common/StackType.h`

同时新增类似：

- `class C2RFrame`
- `static C2RSlotData* GetSlotData(FrameAddress* fa)`

用于按架构读取 stub frame 中的 slot。

## 2.2 修改 `C2RStub` 栈布局

当前 `C2RStub` 定义在：

- `src/arch/*/CalleeSavedStub.S`

需要在 frame layout 中预留 `C2RSlotData` 区域。  
这一步必须谨慎，因为它会影响：

- callee-saved 寄存器偏移
- CFI 信息
- GC 根扫描对 stub layout 的隐含假设

实现时需要：

1. 为每个架构重新定义 `StubFrameContextSize`
2. 明确 slot 所在偏移
3. 调整保存/恢复寄存器的汇编偏移
4. 更新注释中的 frame layout

这一步是整个方案的基础。

## 2.3 进入 `C2RStub` 时保存 caller 上下文到 slot

在真正调用 runtime helper 前，需要像 `N2C` 一样，把当前 caller context 保存到 slot 中。

建议新增 C API：

- `MRT_SaveTopManagedContextToC2RStub`

作用：

1. 读取当前 TLS `UnwindContext`
2. 把 `{pc, fa, status}` 写入当前 `C2RSlotData`

注意这里的“当前上下文”不一定永远是纯 managed：

- 可能来自 managed caller
- 也可能来自更复杂的边界状态

因此 `status` 也必须一并保存。

## 2.4 进入 runtime helper 期间切换 TLS 顶层上下文

这是最关键的一步。

如果只保存 slot，而不改 TLS 顶层上下文，那么当线程在 runtime helper 内部触发：

- 打印栈
- signal 回栈
- EH 栈构建
- GC 扫栈

时，回栈入口仍会从“当前普通 runtime 帧”开始。  
这意味着系统仍然需要先正确识别普通 runtime frame，才能退回到 `C2R_STUB`。

所以还必须像 `C2N` 一样，在进入 runtime helper 期间，把 TLS `UnwindContext` 切成一个“风险边界状态”。

建议新增：

- `MRT_SaveC2RContext`
- `MRT_DeleteC2RContext`

语义可以参考 `C2N`：

- `GoIntoRuntimeCode(pc, fa)` 或复用现有 `GoIntoNativeCode` 语义
- `status = RISKY`
- `ip = unwindPCForC2RStub...`
- `fa = 当前 C2RStub frame`

这样做以后，任何发生在 runtime helper 内部的回栈请求，都可以直接从 `C2R` 边界开始，而不是从 helper 内部普通 runtime 帧开始。

## 2.5 回栈到 `C2R_STUB` 时按 slot 恢复 caller

在 `UnwindContext` 中新增专门逻辑：

- `UnwindToCallerContextFromC2RStub`

逻辑建议与 `N2C` 保持一致：

- 如果 `slot.status` 是 `RISKY / UNKNOWN / SIGNAL_STATUS`
  - 直接使用 `slot.pc / slot.fa`
- 如果 `slot.status` 是 `RELIABLE`
  - 可以按普通机器帧回退，或统一仍走 slot

这里有两种策略：

### 策略 A：完全参考 `N2C`

- `RELIABLE` 时继续走机器帧
- 非 `RELIABLE` 时走 slot

优点：

- 最大限度复用 `N2C` 现有语义

缺点：

- 行为相对复杂

### 策略 B：`C2R_STUB` 一律按 slot 恢复 caller

优点：

- 更简单
- 语义更稳定

缺点：

- 与 `N2C` 的“状态化分支”不完全一致

从工程实现角度看，我更倾向于策略 B。

## 2.6 离开 `C2RStub` 时恢复或清理 TLS 上下文

runtime helper 返回后，需要：

1. 恢复返回值
2. 恢复寄存器
3. 清理 runtime helper 期间写入的 TLS risky context

这一点和 `C2N` 是同类问题。  
否则 GC / signal / 栈打印可能看到 stale `C2R` top context。

## 2.7 栈遍历入口支持 `C2R` 顶层上下文

所有栈遍历最终都经过：

- `StackInfo::CheckTopUnwindContextAndInit`

如果引入了 `C2R` 顶层 risky context，那么这里不需要大改逻辑，但要保证：

- 当前 TLS `uwContext` 处于 `C2R` runtime helper 状态时
- unwind 起点直接落在 `C2R_STUB`

这一步实际上决定了方案能不能解决“普通 runtime 帧先误判再 crash”的问题。

## 2.8 `cj -> runtime -> cj` 嵌套场景如何处理

这个场景指的是：

- 外层 `cj` 代码通过 `C2RStub` 进入 runtime helper
- runtime 再通过 `N2CStub` 回调到内层 `cj`

完整机制下，回栈边界会形成两层嵌套：

1. 外层 `cj -> runtime`
   - 进入 `C2RStub` 时，把当前 TLS 顶层 `UnwindContext` 保存到 `C2RSlotData`
   - 同时把 TLS 顶层切到当前 `C2R` 边界
2. 中间 `runtime -> cj`
   - 如果回调路径经过 `N2CStub`，则 `N2CStub` 保存的 caller 上下文，不再是 runtime helper 内部普通帧
   - 而是前一步已经写入 TLS 的外层 `C2R` 边界
3. 内层 `cj` 发生回栈
   - 先按普通 managed 栈展开
4. 回栈跨过 `N2CStub`
   - 从 `N2C` slot 恢复到外层 `C2R` 边界
   - 不再展开中间那段 runtime helper 内部普通帧
5. 回栈继续跨过 `C2RStub`
   - 再恢复到外层 `cj` 调用者

这样，最终可见的调用链会被压缩成：

- `inner cj -> N2C boundary -> C2R boundary -> outer cj`

而不会保留为：

- `inner cj -> N2C -> runtime helper frames -> C2R -> outer cj`

这个行为有两个直接价值：

- 避免中间 runtime helper 普通帧被误判成 managed 帧
- 使 `cj -> runtime -> cj` 场景仍然能稳定闭环，而不是卡在普通 runtime 帧识别上

同时也要明确两个限制：

- 该语义依赖 `runtime -> cj` 的回调路径确实经过 `N2CStub`
- 中间 runtime helper 内部调用链会被有意压缩，不适合作为保留完整 runtime 调试栈的方案

对当前 `linux x86_64` 原型而言，这个场景是成立的。  
进入 `C2RStub` 时已经把 TLS 顶层切到了 `C2R` 边界，因此后续 `N2CStub` 保存下来的 caller 上下文，会自然指向外层 `C2R`，从而把中间 runtime helper 区段整体压缩掉。

## 3. 可行性

## 3.1 技术可行性

技术上是可行的。

原因是现有 runtime 已经有两套现成范式：

- `N2C`：slot-based caller 恢复
- `C2N`：TLS top-context 覆盖顶层回栈起点

`C2R` 方案本质上是把两者组合起来：

- 用 slot 解决“回到边界后怎么恢复 caller”
- 用 TLS 解决“helper 内部触发回栈时从哪开始”

所以从设计模型上，它并不是发明第三套全新机制，而是扩展现有体系。

## 3.2 语义可行性

从 GC / EH 当前设计看，也是可行的。

### GC

GC 当前已经显式把：

- `C2R_STUB`
- `C2N_STUB`
- `EXSLUSIVE`

当作边界帧处理，而不是依赖普通 runtime 帧。  
这说明 runtime 原本就把“stub 边界”视为根扫描的稳定载体。

### EH

异常路径最终只需要：

- managed EH 帧
- 最后一个 runtime 边界帧

只要 `C2R_STUB` 还作为最后 runtime 边界保留下来，语义上是能接住的。

## 3.3 主要风险

主要风险在实现细节而不是理论模型：

1. `C2RStub` frame layout 改动会牵动 GC root 扫描
2. CFI / ptrauth / unwind 兼容性需要逐架构验证
3. 信号栈、打印栈、CPU profile 的可观测性会变化
4. 嵌套边界场景更复杂

例如：

- managed -> C2R -> runtime -> N2C -> managed
- managed -> C2R -> runtime -> C2N -> native
- managed -> C2R -> runtime -> signal/safepoint/stackgrow

这些都要做专门回归。

## 4. 价值

## 4.1 直接价值

这套机制最大的直接价值是：

- 把 runtime helper 内部调用链压缩成稳定边界
- 减少普通 runtime 帧参与回栈分类
- 降低 runtime frame 识别失败后误判成 managed 的概率

在 Android `extractNativeLibs=false` 这类问题上，它能作为很强的防御机制。

## 4.2 架构价值

从 runtime 架构一致性看，它还有两个额外价值：

1. 让 `C2R`、`N2C`、`C2N` 三类边界的设计更加对称
2. 把“跨世界边界”的回栈规则集中在 stub / TLS 协议中，而不是散落在普通 frame 识别逻辑中

这对未来维护是有帮助的。

## 4.3 价值边界

但这套机制不能完全替代：

- runtime 地址范围识别
- 普通 runtime frame 分类

原因很简单：

- 不是所有 runtime 帧都处在 `C2R` helper 区段里
- 还有 `C2N/N2C/Safepoint/StackGrow/Exclusive` 等其它路径
- 某些 runtime 内部调用可能本来就不经过 `C2RStub`

因此它更适合作为：

- 边界机制增强
- 鲁棒性提升
- 风险隔离层

而不是“彻底替代 runtime frame 识别”的唯一方案。

## 5. 工作量

## 5.1 最小可用版本

如果目标是先做一个主线原型，只覆盖：

- `aarch64_linux`
- Android / Linux 主线
- `C2RStub + slot + TLS + unwind`
- 核心回归：GC / EH / print / signal

工作量大致是中等偏高。

粗略拆分：

1. 设计 `C2RSlotData` 与 `C2RFrame`
2. 修改一套汇编 stub layout
3. 增加 save/restore C API
4. 修改 unwind 主逻辑
5. 验证 GC / EH / signal / print
6. 补测试与文档

工程量大约在：

- `4 ~ 8` 个工程日

如果包含较充分联调与问题修复，现实上接近：

- `1 ~ 2` 周

## 5.2 全平台正式版本

如果要做成 runtime 的正式通用机制，至少要覆盖：

- `aarch64_linux`
- `x86_64_linux`
- `arm_linux`
- `aarch64_macos`
- `x86_64_macos`
- `aarch64_ios`
- `x86_64_ios`
- `x86_64_windows`

这时工作量会显著上升。

增加的成本主要在：

- 多架构汇编维护
- CFI / unwind 规则差异
- Windows SEH
- Apple ptrauth
- 各平台测试环境准备

整体更接近：

- `2 ~ 4` 周

如果测试覆盖要求高，可能更长。

## 6. 兼容性影响

## 6.1 对外接口兼容性

如果实现时不修改以下内容：

- `CJ_MCC_*` / `MRT_*` 的符号名
- 对外函数参数布局
- 调用约定
- 返回值约定

那么从外部接口角度看，兼容性影响较小。

这意味着：

- 业务代码源码兼容性通常不受影响
- 大多数调用方的链接兼容性也有机会保持稳定

因此该方案的主要兼容性风险不在“外部 API”，而在 runtime 内部边界协议。

## 6.2 runtime 内部 ABI 兼容性

内部兼容性影响较大。

原因是该方案一旦引入 `C2RSlotData`，就会直接改变：

- `C2RStub` frame layout
- callee-saved 寄存器保存区偏移
- `CalleeSavedStubFrameSize`
- CFI / unwind 语义

而当前 GC 对 stub frame 布局有显式偏移依赖，例如：

- `src/StackMap/StackMapAarch64.h`
- `src/StackMap/StackMapArm.h`
- `src/StackMap/StackMapX86.h`

这些实现会按固定偏移读取 stub 中保存的寄存器槽位。  
因此如果只改汇编布局，而不同时更新这些偏移规则，就可能直接导致：

- GC 读取错误寄存器槽位
- root 扫描错误
- 更严重时出现内存安全问题

所以这不是一个“局部可热插拔”的改动，而是必须整套同步升级的内部 ABI 变更。

## 6.3 新旧版本混用兼容性

这一点需要单独验证，不能默认安全。

如果方案满足：

1. `CJ_MCC_*` 对外调用 ABI 不变
2. 编译产物不依赖 `C2RStub` 的内部 slot 布局
3. 所有 stub 偏移逻辑都封装在 runtime 内部

那么：

- “旧编译产物 + 新 runtime” 有机会保持兼容

因为多数编译产物只调用 stub 符号，并不知道 stub 内部 frame layout。

但如果引入了以下变化，则混用兼容性会下降：

- 编译器需要理解新的 `C2R` slot 语义
- 某些 offset / 协议被固化到编译器或外部工具链
- TLS top-context 协议在新旧版本间不一致

此时可能出现：

- 新 runtime 不能可靠运行旧产物
- 旧 runtime 不能正确解释新产物

因此该方案一旦推进，建议把“跨版本混跑验证”作为明确测试项，而不是默认假设兼容。

## 6.4 调试与观测兼容性

该方案会改变回栈行为本身，因此调试与观测层面一定会有兼容性变化。

主要表现为：

- 栈打印结果可能变短
- signal 栈内容会变化
- CPU profile 的调用链形态会变化
- 某些依赖 runtime helper 详细栈形状的调试脚本会受到影响

这类问题不属于 ABI 破坏，但属于：

- 行为兼容性变化
- 观测结果兼容性变化

对于线上问题排查和性能分析工具，需要提前评估影响。

## 6.5 兼容性结论

可以把该方案的兼容性影响概括为：

- 对外接口兼容性：较小
- runtime 内部 ABI 兼容性：较大
- 新旧版本混用兼容性：需要专项验证
- 调试/观测兼容性：一定会变化

因此更稳妥的实施前提应当是：

1. 保持 `CJ_MCC_*` 的对外 ABI 不变
2. 同步修改所有依赖 `C2RStub` 布局的 runtime 内部模块
3. 在主平台上做新旧版本混跑验证

## 7. 全平台适配

## 7.1 Linux / Android / OHOS

这是最适合优先落地的平台。

原因：

- ELF / frame model 接近
- 当前问题本身也集中在 Android
- `aarch64_linux` 与 `x86_64_linux` 的实现路径最清晰

建议先做：

1. `aarch64_linux`
2. 再扩到 `x86_64_linux`
3. 视需要覆盖 `arm_linux`

## 7.2 Apple 平台

Apple 平台可做，但要额外关注：

- ptrauth
- 动态库符号与 unwind label 处理
- iOS / macOS 的差异

因为 `N2C/C2N` 在 Apple 上已有对应实现，所以思路不是问题，但验证成本更高。

## 7.3 Windows

Windows 平台工作量最大。

原因：

- 不是简单 DWARF/FP unwind
- SEH / unwind info 体系不同
- 现有 `N2C`、`C2R` 都有 Windows 专门实现

如果先做跨平台原型，Windows 适合后置。

## 7.4 推荐适配顺序

建议顺序：

1. `aarch64_linux`
2. `x86_64_linux`
3. Android 主线验证
4. `arm_linux`
5. Apple 平台
6. Windows

这是成本与收益最平衡的顺序。

## 8. 建议实施策略

如果决定推进，建议不要一口气做全平台统一提交，而是分阶段。

### 阶段 1：原型

目标：

- 只做 `aarch64_linux`
- 打通 `C2RSlotData + TLS + custom unwind`
- 证明能稳定压缩 runtime helper 区段

### 阶段 2：验证

重点验证：

- GC roots
- EH 栈展开
- signal 栈
- print stack
- 嵌套 `C2R/N2C/C2N`

### 阶段 3：扩平台

在原型稳定后再扩：

- `x86_64_linux`
- Android
- 其它平台

## 9. 总结

按照“完整机制”来做，`C2R` 回栈增强方案是可行的。

它的核心不是单独引入 `C2RSlotData`，而是补齐一整套边界协议：

- `C2RSlotData`
- `C2RStub` frame layout
- TLS top-context 切换
- `UnwindToCallerContextFromC2RStub`
- 返回时上下文恢复

从价值上看，它能显著提升 runtime helper 区段在回栈中的鲁棒性，并让跨边界回栈设计更对称。  
从工作量看，它不是小修，适合作为一个分阶段推进的底层增强项目。  
从平台策略看，最合理的路径是先做 `aarch64_linux/android` 原型，再决定是否扩成全平台正式机制。

## 10. 当前 `linux x86_64` 原型进展

当前 `linux x86_64` 已经做出了一版可运行原型，但还不是“完整最终形态”。

### 10.1 已实现部分

当前原型已经具备：

- `C2RSlotData`
- `C2RStub` 进入 helper 前保存 TLS 顶层 `UnwindContext`
- 进入 helper 期间把 TLS 顶层切到 `C2R` 边界
- helper 返回后恢复 TLS 顶层 `UnwindContext`

这意味着：

- 当线程在 runtime helper 内部触发打印栈、异常栈构建等回栈动作时
- unwind 起点会直接落在 `C2R` 边界
- 中间普通 runtime helper 帧不会再先被当成 managed 帧去做元数据解析

### 10.2 当前还没补齐的部分

当前原型还没有新增：

- `UnwindToCallerContextFromC2RStub`

也就是说，当前 `C2R` 的行为是：

- 起点通过 TLS 顶层上下文切换直接收敛到 `C2R_STUB`
- 回栈跨过 `C2R_STUB` 时，仍然走普通机器回栈

因此这版更准确的定义是：

- `C2R` 原型已打通
- 但还不是完全对齐 `N2C` 的 slot-based custom unwind 完整协议

### 10.3 为适配嵌套场景补过的修正

在 `cj -> runtime -> cj` 的嵌套反射场景里，原型阶段出现过一个问题：

- `C2R` 顶层上下文会让 EH 栈构建过早从外层 `C2R` 边界起步
- 导致内层 managed 抛异常时，抛出点和 catch 点之间的 managed 帧被绕掉

当前 `linux x86_64` 原型已经通过 EH 路径修正解决了这个问题：

- EH 构建时临时忽略 `C2R` 顶层上下文
- 仍从当前异常调用链本身展开 managed EH 栈

这说明：

- `C2R` 顶层上下文切换对打印栈/普通回栈是有效的
- 但异常路径需要区别对待，不能机械复用同一套起点语义

### 10.4 已完成验证

当前验证是在 `release` runtime 下完成的，并已替换编译器侧 `libcangjie-runtime.so`。

已经通过的用例包括：

- 运行时本地最小验证：`tests/c2r_unwind_exception_validation.cj`
- 运行时本地嵌套边界验证：`tests/c2r_nested_reflect_unwind_validation.cj`
- HLT `stack001-006`
- HLT `stack201-214`
- HLT `stack401-416`

其中：

- `stack214` 是新增的正式用例
- 它专门验证 `cj -> runtime(std.reflect::StaticFunctionInfo::apply) -> cj` 这条链路

当前这轮回归里，`StackGrow` 目录是显式跳过的，因此还不能说 `linux x86_64` 已经覆盖所有 stack 相关场景。

### 10.5 当前阶段结论

因此，当前 `linux x86_64` 的状态更适合表述为：

- 主路径实现已完成
- 非 `StackGrow` 的一轮关键回归已通过
- `cj -> runtime -> cj` 嵌套边界已有正式测试覆盖
- 但 `C2R` 仍不是完整 custom unwind 版本

如果继续推进，后续重点应放在两件事：

1. 评估是否真的需要补 `UnwindToCallerContextFromC2RStub`
2. 在 `StackGrow` 和后续 `aarch64/Android` 真实问题场景中继续验证
