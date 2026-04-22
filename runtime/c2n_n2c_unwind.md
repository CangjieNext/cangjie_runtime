# runtime 中 C2N / N2C 回栈实现分析

本文分析 `runtime` 里 `C2N`、`N2C` 两类桥接调用在“回栈”场景下是如何工作的。这里的“回栈”不只指打印栈，也包括 GC 扫栈、signal 栈打印、异常栈展开。源码以 `aarch64_linux` 为主线，其他架构基本沿用同样的思路，只是 stub 汇编细节不同。

## 1. 两个方向分别是什么

- `C2N`：Cangjie/managed 代码进入 native，入口 stub 是 `CJ_MCC_C2NStub`。
- `N2C`：native 代码进入 Cangjie/managed，入口 stub 是 `CJ_MCC_N2CStub`。

对应实现和入口符号：

- `src/arch/aarch64_linux/C2NStub.S`
- `src/arch/aarch64_linux/N2CStub.S`
- `src/UnwindStack/UnwindCApi.cpp`
- `src/UnwindStack/UnwindContext.cpp`
- `src/UnwindStack/StackInfo.cpp`
- `src/UnwindStack/MachineFrame.cpp`
- `src/Exception/Exception.cpp`

## 2. 核心结论：C2N 和 N2C 用了两套不同的“回栈锚点”

这是整个实现里最重要的点。

### 2.1 C2N：把“可回栈上下文”放进 TLS 的 `UnwindContext`

当线程从 managed 进入 native 后，native 段上面可能再套很多普通 C/C++ 调用帧。这些帧对 runtime 来说通常是“不可靠的上半段”，不能指望只靠它们恢复到最后一个 managed caller。

所以 `C2N` 的策略是：

1. 在进入 native 之前，把当前桥接点保存到 TLS 里的 `Mutator::uwContext`
2. 回栈时如果发现当前 `uwContext.status != RELIABLE`，就直接从这个保存点开始，而不是从当前机器栈顶盲目往上爬

这部分代码在：

- `src/UnwindStack/UnwindCApi.cpp`
  - `MRT_SaveC2NContext`
  - `MRT_DeleteC2NContext`
- `src/UnwindStack/UnwindContext.cpp`
  - `UnwindContext::GoIntoNativeCode`
  - `UnwindContext::GoIntoManagedCode`

### 2.2 N2C：把“上一层上下文”放进 N2C stub 自己的 slot

`N2C` 的问题不一样。它是在 native 里创建一个桥接帧，再进入 managed。此时回栈是从 managed 往外走，等走到 `N2CStub` 这一层时，需要知道“桥接外面应该回到哪去”。

所以 `N2C` 的策略是：

1. 在 `N2CStub` 帧里预留 `N2CSlotData`
2. 进入 managed 前，把“外层上下文”写进这个 slot
3. 回栈走到 `N2CStub` 时，不一定按普通机器栈走，而是优先读取 slot 里保存的 `pc/fa/status`

这部分代码在：

- `src/Common/StackType.h`
  - `struct N2CSlotData`
  - `class N2CFrame`
- `src/UnwindStack/UnwindCApi.cpp`
  - `MRT_SaveTopManagedContextToN2CStub`
  - `MRT_RestoreTopManagedContextFromN2CStub`
- `src/UnwindStack/UnwindContext.cpp`
  - `UnwindContext::UnwindToCallerContextFromN2CStub`

## 3. C2N 回栈是怎么实现的

## 3.1 进入 native 之前先把桥接点保存到 TLS

`src/arch/aarch64_linux/C2NStub.S` 里，`CJ_MCC_C2NStub` 先建立一个标准 stub frame，保存寄存器，之后在真正调用 native 前执行：

- 设置 `x1 = x29`，也就是当前 stub 的 frame address
- 设置 `x0 = unwindPCForC2NStub`，也就是这个 stub 专门留给回栈识别的 PC
- 调用 `MRT_SaveC2NContext`

`MRT_SaveC2NContext` 最终会调用：

- `UnwindContext::GoIntoNativeCode(pc, fa)`

效果是把 TLS 中的 `uwContext` 设成：

- `ip = unwindPCForC2NStub`
- `fa = 当前 C2N stub 的 frame address`
- `status = RISKY`

也就是说，线程一旦进入 native，runtime 就不再认为“当前栈顶是完全可靠的 managed/runtime 栈”，而是显式记住“最后一个可信桥接点”。

## 3.2 回栈起点不一定取当前机器栈，而是先看 TLS 的 `uwContext`

所有栈遍历最终都会走到 `StackInfo::CheckTopUnwindContextAndInit`，在：

- `src/UnwindStack/StackInfo.cpp`

逻辑是：

- 如果当前 `Mutator::uwContext.status == RELIABLE`，说明线程现在处在普通 managed/runtime 可直接回栈的状态，从当前 caller frame 开始回栈即可
- 如果状态不是 `RELIABLE`，说明线程正处在 native 或其他风险区，直接把 TLS 里保存的 `uwContext` 当作回栈起点

这就是 `C2N` 能跨过 native 段的关键。runtime 不去“解析 native 里面发生了什么”，而是直接从进入 native 前保存下来的桥接点继续往 managed caller 方向回退。

## 3.3 C2N stub 本身仍然是标准机器帧

`MachineFrame::IsC2NStubFrame()` 在：

- `src/UnwindStack/MachineFrame.cpp`

它通过 `ip == unwindPCForC2NStub` 识别当前帧是不是 `C2NStub`。

一旦识别成 `C2N_STUB`，后续从这个 stub 回到它的 managed caller，仍然走普通机器回栈：

- `MachineFrame::UnwindToCallerMachineFrame`

原因是 `C2NStub` 自己是 runtime 生成的标准 frame，`fp/lr` 链是完整的，真正“不透明”的是 stub 之上的 native 代码，而那一段已经被 TLS `uwContext` 绕过去了。

## 3.4 native 返回后要及时删掉这份 TLS 上下文

在 `src/arch/aarch64_linux/C2NStub.S` 里，native 返回后会先恢复 `sp`，然后调用：

- `MRT_GetThreadLocalData`
- `MRT_DeleteC2NContext`

`MRT_DeleteC2NContext` 会把 `uwContext` 清回 managed 状态，也就是：

- `ip = nullptr`
- `fa = nullptr`
- `status = RELIABLE`

这里删得很早，源码里也写了注释：要在离开 saferegion 前清掉 risky 的 C2N context，避免 GC 在这个时间窗里看见一个已经失效的“顶层 C2N 帧”。

## 4. N2C 回栈是怎么实现的

## 4.1 N2C stub 里预留了一个 `N2CSlotData`

`src/arch/aarch64_linux/N2CStub.S` 的栈布局注释里可以看到，stub frame 里专门放了一段 unwind context：

- `Context PC`
- `Context FP`
- `Context LR`
- `UC Status`
- `shadowframe`
- `direct call`

其中 runtime 真正会直接使用的是 `N2CSlotData` 对应的：

- `pc`
- `fa`
- `status`

它的访问方式在 `src/Common/StackType.h`：

- `N2CFrame::GetSlotData`

在 aarch64/arm 下，slot 紧挨着 `fa` 后面，所以是 `fa + 1`。

## 4.2 进入 managed 前，把“外层上下文”写进 slot

`src/arch/aarch64_linux/N2CStub.S` 在调用目标 Cangjie 函数前会执行：

- `MRT_SaveTopManagedContextToN2CStub(x29)`

对应 C++ 实现见 `src/UnwindStack/UnwindCApi.cpp`。

它做了两件事：

1. 把当前 TLS `uwContext.frameInfo` 和 `status` 拷贝到当前 `N2CStub` 的 `N2CSlotData`
2. 调用 `uwContext.GoIntoManagedCode()`，把 TLS 顶层上下文清回“managed 内部可直接回栈”的状态

这一步的意义是：

- managed 里正常回栈时，不需要一直盯着 TLS
- 只有当回栈真的走到 `N2CStub` 这层时，才从 slot 里知道外层应该接到哪里

## 4.3 从 managed 回到 N2CStub 后，是否继续按机器帧回退，取决于 `slot.status`

核心逻辑在：

- `src/UnwindStack/UnwindContext.cpp`
  - `UnwindContext::UnwindToCallerContextFromN2CStub`

逻辑分两种：

### 4.3.1 `status` 是 `RISKY / SIGNAL_STATUS / UNKNOWN`

这时不相信普通机器回栈，而是直接用 slot 里保存的：

- `caller.fa = slot.fa`
- `caller.ip = slot.pc`

也就是“跳回保存下来的上层上下文”。

这对应的典型场景是：

- 外层 native 并不是一个可以稳定按 `fp/lr` 继续走的纯普通调用链
- 或者当前桥接点本身就是从外层 `C2N` 再回进来的

此时 `N2CStub` 本质上充当了一个“显式跳板帧”。

### 4.3.2 `status` 是 `RELIABLE`

这时说明外层 caller 本身就是可信的普通机器帧链，可以直接：

- `frameInfo.mFrame.UnwindToCallerMachineFrame(...)`

也就是说，`N2CStub` 不强制跳 slot，它只是保留了“必要时覆盖普通回栈”的能力。

## 4.4 从 managed 返回 native 时，再把 slot 恢复回 TLS

`src/arch/aarch64_linux/N2CStub.S` 在目标 Cangjie 函数返回后，会调用：

- `MRT_RestoreTopManagedContextFromN2CStub(x29)`

它把 `slot.pc / slot.fa / slot.status` 再写回 TLS 里的 `uwContext`。

这样做的效果是：

- 如果这个 `N2C` 是嵌套在某个 `C2N` native 段里，返回 native 后，外层那份 risky 上下文会重新生效
- 后续若再发生 GC、signal 打印、异常处理，runtime 仍然知道“native 外面最近的可信 managed/runtme 桥接点”在哪里

## 5. 栈遍历器如何识别这些桥接帧

桥接帧识别集中在：

- `src/UnwindStack/MachineFrame.cpp`

其中最关键的是：

- `MachineFrame::IsN2CStubFrame()`
- `MachineFrame::IsC2NStubFrame()`

识别方式不是靠符号表做复杂解析，而是直接比较当前 `ip` 是否等于 runtime 在汇编里显式暴露出来的几个特殊 label：

- `unwindPCForN2CStub`
- `unwindPCForC2NStub`
- 以及 safepoint / stackgrow / exclusive 等 stub 的专用 label

识别完后，`StackInfo::AnalyseAndSetFrameType()` 会给当前帧打上：

- `FrameType::N2C_STUB`
- `FrameType::C2N_STUB`
- `FrameType::RUNTIME`
- `FrameType::MANAGED`

这样 GC、打印栈、异常栈三种逻辑可以共用同一套底层回栈机制，只是筛选规则不同。

## 6. 为什么异常回栈会在 N2C 边界停下来

异常相关逻辑在：

- `src/UnwindStack/EhStackInfo.cpp`
- `src/Exception/Exception.cpp`

`EHStackInfo::FillInStackTrace()` 的循环里有一段特殊判断：

- 如果 `n2cCount == 1`，直接停止

这个行为说明 runtime 的异常栈展开只处理“当前 managed 岛”里的异常传播，不继续跨过第一个 `N2C` 边界往更外层 native 搜。

原因也很直接：

- native 外层不是 Cangjie 异常模型的一部分
- runtime 只需要在当前 managed 栈里找到 catch/landing pad
- 跨过 native 再往外找，对当前异常恢复没有意义，反而会把 native 段混进来

## 7. 异常真正恢复控制流时，C2N 是怎么接上的

这部分是 `C2N/N2C` 回栈和异常恢复结合得最紧的一段。

### 7.1 先收集 managed EH frame

`ExceptionHandling::BuildEHFrameInfo()` 会：

1. 通过 `EHStackInfo` 拿到当前异常相关栈
2. 只把 `FrameType::MANAGED` 的帧转成 `EHFrameInfo`
3. 同时记住最后一个非 managed 帧到 `lastRuntimeFrame`

这个 `lastRuntimeFrame` 通常就是 managed 栈外侧最近的 runtime/C2N 桥接帧。

### 7.2 再把这个 runtime 帧的返回地址改写成 `RestoreContextForEH`

`ExceptionHandling::ChangeEHStackInfoLR()` 会直接改：

- `lastRuntimeFrame.mFrame.GetFA()->returnAddress = RestoreContextForEH`

也就是说，后续当控制流准备从 runtime/C2N 桥接帧返回时，不再回原来的地方，而是先进入：

- `src/arch/aarch64_linux/RestoreContextForEH.S`

### 7.3 `RestoreContextForEH` 再恢复寄存器并跳到 landing pad

`RestoreContextForEH.S` 做的事非常直接：

1. 把当前 callee-saved 寄存器和 `sp/fp` 保存成一个 context
2. 调 `MRT_RestoreContext`
3. `MRT_RestoreContext` 根据前面构造好的 `EHFrameInfo` 恢复异常目标帧的寄存器状态，并返回目标 landing pad
4. 汇编最后 `br x0` 跳到 landing pad

所以从控制流角度看，异常不是“让 native 帧逐层执行语言级析构再回去”，而是：

- 先通过 `EHStackInfo` 算出 managed 异常应该落到哪里
- 再通过改写 runtime/C2N 桥接帧的返回地址，把返回路径劫持到 `RestoreContextForEH`
- 最后一次性把寄存器恢复到目标 managed 帧，跳到 landing pad

## 8. 把整个机制串起来看

可以把它理解成下面这张图：

```text
managed A
  -> C2NStub
       保存 {ip=unwindPCForC2NStub, fa=stub_fp, status=RISKY} 到 TLS
  -> native ...
       若 native 内再回调 managed:
         native -> N2CStub
             把当前 TLS uwContext 存入 N2CSlotData
             清 TLS 为 managed 状态
         -> managed B
```

从 `managed B` 回栈时：

1. 在 managed 段内部，按普通机器帧回退
2. 走到 `N2CStub` 时，检查 `N2CSlotData.status`
3. 如果不可靠，直接跳回 slot 里保存的 `{pc, fa}`
4. 如果 slot 指向的是某个 `C2NStub`，继续把这个 `C2NStub` 当作桥接点往外退
5. `C2N` 之上的 native 段不会被硬解析，而是由 TLS 保存的 top context 直接跨过去

所以这套设计本质上不是“runtime 能完全解析所有 native 栈帧”，而是：

- 对 managed/runtime 自己生成的桥接帧，保证 frame layout 可识别
- 对不透明的 native 段，显式保存足够的恢复点
- 在跨边界时用 TLS 或 stub slot 把 managed 世界重新接起来

## 9. 简短总结

`C2N` 和 `N2C` 的回栈实现是非对称的：

- `C2N` 解决的是“进入 native 后，上层 native 栈不透明”，所以把桥接点存进 TLS `UnwindContext`
- `N2C` 解决的是“从 managed 回到桥接帧时，外层 caller 可能不能直接靠机器帧恢复”，所以把外层上下文存进 `N2CStub` 自己的 slot

统一的消费方是 `UnwindContext` / `StackInfo` / `MachineFrame`：

- 先识别当前是不是特殊 stub
- 如果是 `N2CStub`，优先看 slot
- 如果当前线程处在 `C2N` native 段，优先看 TLS 保存的 risky context
- 异常路径再在此基础上改写 runtime/C2N 桥接帧返回地址，把控制流导向 `RestoreContextForEH`

这就是 runtime 里 `C2N / N2C` 回栈能工作的核心实现。
