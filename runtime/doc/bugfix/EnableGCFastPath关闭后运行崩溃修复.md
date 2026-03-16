# EnableGCFastPath 关闭后运行崩溃修复

## 1. 问题描述

LLVM 编译器后端中有一个编译选项 `EnableGCFastPath`（定义在 `llvm/lib/CodeGen/CJBarrierLowering.cpp`），默认值为 `false`。该选项控制对象分配的快速路径：开启时，编译器将 `CJ_MCC_NewObject` 调用替换为 `CJ_MCC_NewObjectFast`，后者内联 bump-pointer 分配逻辑，快速路径失败时再回退到 `CJ_MCC_NewObject` 慢路径。

**问题**：关闭 `EnableGCFastPath`（使用默认值 `false`）后，编译出的仓颉程序在运行时崩溃。调查过程中实际遇到了**两个独立的问题**：

1. **Bus Error（SIGBUS）**：运行时立即崩溃，PC 指向页对齐地址
2. **SIGKILL（exit 137）**：修复第一个问题后，程序被操作系统静默杀死

## 2. 环境信息

| 项目 | 值 |
|------|-----|
| 平台 | macOS aarch64（Apple Silicon） |
| 编译器 | Cangjie LLVM 编译器（`cjc`） |
| Runtime | cangjie-runtime（C/C++ 实现） |
| 测试 SDK（ON） | `cangjie-bak2`：EnableGCFastPath = true |
| 测试 SDK（OFF） | `cangjie`：EnableGCFastPath = false |
| 关键文件 | `llvm/lib/CodeGen/CJBarrierLowering.cpp` 第 47 行 |

`EnableGCFastPath` 定义：
```cpp
static cl::opt<bool> EnableGCFastPath("enable-gc-fast-path", cl::init(false), cl::Hidden);
```

相关编译选项：
| 开关 | 作用 | 默认值 |
|------|------|--------|
| `EnableGCFastPath` | 对象分配快速路径（NewObj → NewObjFast） | `false` |
| `EnableGCPhase` | 写屏障快速路径（检查 GC 阶段） | `true` |
| `EnableTaggedPointer` | 读屏障快速路径（检查 tagged pointer） | `true` |
| `EnableGCStateLoop` | 循环中写屏障优化 | `false` |

## 3. 问题复现

使用 EnableGCFastPath=OFF 的 SDK 编译并运行测试用例：

```bash
source cangjie/envsetup.sh  # OFF SDK
cjc gc_benchmark.cj
./main
# Bus error (signal 10)，PC = 0x1030dc000（页对齐地址）
```

使用 EnableGCFastPath=ON 的 SDK：
```bash
source cangjie-bak2/envsetup.sh  # ON SDK
cjc gc_benchmark.cj
./main
# 全部测试通过：5/5 functional, 所有 performance 正常
```

## 4. 分析过程

### 4.1 问题一：Bus Error（SIGBUS）

#### 观察

用 `lldb` 运行崩溃程序，得到 SIGUSR1（Bus error），PC 地址为 `0x1030dc000`（页对齐地址），这是典型的栈破坏后跳转到无效地址的表现。

#### 关键线索：`nm` 符号对比

```bash
# OFF 编译器产物：只有 CJ_MCC_NewObject（慢路径）
nm simple_off | grep NewObject
→ CJ_MCC_NewObject

# ON 编译器产物：有 CJ_MCC_NewObjectFast + CJ_MCC_NewObject
nm main_bak2 | grep NewObject
→ CJ_MCC_NewObjectFast
→ CJ_MCC_NewObject
```

EnableGCFastPath=ON 时编译器会生成 `CJ_MCC_NewObjectFast` 包装函数，内联 bump-pointer 分配，失败时 fall through 到 `CJ_MCC_NewObject`。

#### 关键线索：汇编 Stub 宏差异

Runtime 的 `CalleeSavedStub.S` 中定义了两种汇编入口宏：

**`CalleeSavedRegistersStub`**（标准版）：
```asm
.macro CalleeSavedRegistersStub func
    stp x29, x30, [sp, #-0x70]!   ; 保存 callee-saved 寄存器
    ...
    bl  _MCC_\func                  ; 调用 C++ 实现
    ...
    ldp x29, x30, [sp], #0x70      ; 恢复并返回
    ret
.endm
```

**`CalleeSavedRegistersStubNew`**（新版，为 FastPath 设计）：
```asm
.macro CalleeSavedRegistersStubNew func
    ldp x29, x30, [sp], #16       ; ★ 额外操作：弹出 fp/lr
    stp x29, x30, [sp, #-0x70]!   ; 保存 callee-saved 寄存器
    ...
    bl  _MCC_\func
    ...
    ldp x29, x30, [sp], #0x70
    ret
.endm
```

`StubNew` 入口处多了一个 `ldp x29, x30, [sp], #16`——它假定调用者已经在栈上 push 了 fp/lr。

#### 根因分析

当 `EnableGCFastPath` **开启**时，调用链为：

```
用户代码 → CJ_MCC_NewObjectFast（编译器生成）
           ├── stp x29, x30, [sp, #-0x10]!   ← push fp/lr
           ├── 内联 bump-pointer 分配
           ├── 成功 → 返回
           └── 失败 → fall through 到 CJ_MCC_NewObject
                       ├── ldp x29, x30, [sp], #16  ← pop fp/lr（与上面的 stp 配对）
                       ├── CalleeSavedRegistersStubNew 正常执行
                       └── 返回
```

`stp`（push）和 `ldp`（pop）正确配对，栈平衡。

当 `EnableGCFastPath` **关闭**时，编译器**不生成** `CJ_MCC_NewObjectFast`，直接调用 `CJ_MCC_NewObject`：

```
用户代码 → CJ_MCC_NewObject
           ├── ldp x29, x30, [sp], #16  ← ★ 没有对应的 stp！
           │   弹出了调用者栈帧中不相关的数据到 x29/x30
           ├── 栈指针上移 16 字节（栈破坏）
           ├── ... 后续操作用破坏的 fp/lr 执行 ...
           └── ret → 跳转到垃圾地址 → SIGBUS
```

**根因**：`CJ_MCC_NewObject` 使用了 `CalleeSavedRegistersStubNew` 宏，其入口的 `ldp x29,x30,[sp],#16` 与 `CJ_MCC_NewObjectFast` 的 `stp x29,x30,[sp,#-0x10]!` 配对。当没有 FastPath 时，这个 `ldp` 弹出了不属于它的栈数据，导致栈破坏。

### 4.2 问题二：SIGKILL（exit 137）

修复 Bus Error 后（将 `CalleeSavedRegistersStubNew` 改为 `CalleeSavedRegistersStub`），重新编译 runtime 并替换到 SDK 目录。程序不再 Bus Error，但被操作系统静默杀死（exit code 137 = SIGKILL）。

#### 排查过程

**1. 怀疑标准库 ABI 不兼容**

最初怀疑 cangjie SDK 的标准库 dylib 是用 OFF 编译器编译的，可能存在 ABI 不兼容。但 MD5 对比显示两个 SDK 的标准库**完全相同**：
```
cangjie/libcangjie-std-core.dylib  → MD5: e19556d19c95d354fb64cdb21e495f52
cangjie-bak2/libcangjie-std-core.dylib → MD5: e19556d19c95d354fb64cdb21e495f52
```

**2. 怀疑 runtime dylib 内容不同**

对比两个 SDK 中的 runtime dylib，MD5 也完全相同：
```
cangjie/libcangjie-runtime.dylib   → MD5: bdd449b299b2681fab1d8bcf7756a795
cangjie-bak2/libcangjie-runtime.dylib → MD5: bdd449b299b2681fab1d8bcf7756a795
build output/libcangjie-runtime.dylib → MD5: bdd449b299b2681fab1d8bcf7756a795
```

**3. 关键实验：DYLD_LIBRARY_PATH 对比**

```bash
# 指向 bak2 目录 → 成功
DYLD_LIBRARY_PATH=/path/to/cangjie-bak2/runtime/lib/... ./hello
# 输出: hello

# 指向 cangjie 目录 → 被杀
DYLD_LIBRARY_PATH=/path/to/cangjie/runtime/lib/... ./hello
# [1] killed (exit 137)
```

同一个可执行文件，dylib 文件内容完全一致（MD5 相同），仅仅因为指向不同目录就一个正常、一个被杀。

**4. 隔离实验：复制到 /tmp**

```bash
# 把 cangjie 的 dylib 复制到 /tmp 的全新目录
mkdir -p /tmp/cj_test && cp cangjie/.../libcangjie-runtime.dylib /tmp/cj_test/
cp cangjie/.../libboundscheck.dylib /tmp/cj_test/

DYLD_LIBRARY_PATH=/tmp/cj_test ./hello
# 输出: hello  ← 成功！
```

复制到新目录就能工作，说明问题出在 cangjie SDK 目录中的文件状态**而非内容**。

**5. DYLD_PRINT_LIBRARIES 分析**

```bash
# bak2 路径：正常加载所有库
DYLD_PRINT_LIBRARIES=1 DYLD_LIBRARY_PATH=.../cangjie-bak2/... ./hello
# dyld: <...> hello_bak2_compiled
# dyld: <...> libcangjie-runtime.dylib  ← 正常加载
# dyld: <...> libboundscheck.dylib
# ... 后续系统库 ...
# hello

# cangjie 路径：加载主程序后立即被杀
DYLD_PRINT_LIBRARIES=1 DYLD_LIBRARY_PATH=.../cangjie/... ./hello
# dyld: <...> hello_bak2_compiled
# [1] killed                           ← 连 runtime dylib 都没来得及加载！
```

进程在 dyld 刚加载完主程序、还没来得及加载 `libcangjie-runtime.dylib` 时就被 SIGKILL 了。

#### 根因

**macOS arm64 (Apple Silicon) 强制代码签名机制**。

在 Apple Silicon 上，所有 Mach-O 可执行文件和动态库必须有有效的代码签名。链接器输出的文件自带 **ad-hoc linker-signed** 签名。

之前调试过程中多次使用 `cp` 命令覆盖 cangjie SDK 目录中的 dylib 文件。`cp` 操作改变了文件的 inode 和元数据，但**没有更新嵌入的 ad-hoc 签名哈希**，导致签名失效。macOS 内核在加载签名无效的 dylib 时，直接 SIGKILL 进程，不产生任何错误消息。

验证：对被破坏签名的 dylib 重新签名后立即恢复正常：
```bash
codesign -f -s - /path/to/cangjie/.../libcangjie-runtime.dylib
# 重签后：
./hello
# 输出: hello  ← 成功
```

## 5. 修复方案

### 5.1 Bus Error 修复：汇编 Stub 宏替换

**改动文件**（3 个）：
- `src/arch/aarch64_macos/CalleeSavedStub.S`
- `src/arch/aarch64_ios/CalleeSavedStub.S`
- `src/arch/aarch64_linux/CalleeSavedStub.S`

**改动内容**：将 `CJ_MCC_NewObject` 和 `CJ_MCC_NewFinalizer` 的入口宏从 `CalleeSavedRegistersStubNew` 改为 `CalleeSavedRegistersStub`。

macOS/iOS 改动（两个文件相同）：
```diff
 _CJ_MCC_NewObject:
-    CalleeSavedRegistersStubNew MCC_NewObject
+    CalleeSavedRegistersStub MCC_NewObject

 _CJ_MCC_NewFinalizer:
-    CalleeSavedRegistersStubNew MCC_NewFinalizer
+    CalleeSavedRegistersStub MCC_NewFinalizer
```

Linux 改动（双参数宏语法）：
```diff
 unwindPCForC2RStubStart:
-    CalleeSavedRegistersStubNew MCC_NewObject CJ_MCC_NewObject
+    CalleeSavedRegistersStub MCC_NewObject CJ_MCC_NewObject
     CalleeSavedRegistersStub MCC_NewWeakRefObject CJ_MCC_NewWeakRefObject
     CalleeSavedRegistersStub MCC_NewObjArray CJ_MCC_NewObjArray
-    CalleeSavedRegistersStubNew MCC_NewFinalizer CJ_MCC_NewFinalizer
+    CalleeSavedRegistersStub MCC_NewFinalizer CJ_MCC_NewFinalizer
```

**为什么不影响 FastPath=ON 的场景**：

当 EnableGCFastPath=ON 时，编译器生成的 `CJ_MCC_NewObjectFast` 中已经完成了 fp/lr 的 push（`stp x29,x30,[sp,#-0x10]!`）。改为 `CalleeSavedRegistersStub` 后，`CJ_MCC_NewObject` 入口不再弹出多余的 fp/lr，而是直接保存 callee-saved 寄存器。`CJ_MCC_NewObjectFast` 的 stp 所 push 的 fp/lr 会在 `CJ_MCC_NewObjectFast` 自己的 epilogue 中被正确恢复——它跳到 `CJ_MCC_NewObject` 时，返回地址已通过 `bl` 指令更新为正确值。

实际验证：`cangjie-bak2` SDK 出厂的 runtime dylib 中 `CJ_MCC_NewObject` 入口就是没有 `ldp` 的版本（与我们的修复结果一致），说明上游已经做了同样的改动。

### 5.2 SIGKILL 修复：macOS arm64 代码签名

**规则**：在 macOS arm64 上，每次用 `cp` 替换 dylib 后必须重新签名：

```bash
cp <new_dylib> <target_path>/libcangjie-runtime.dylib
codesign -f -s - <target_path>/libcangjie-runtime.dylib
```

**已更新的文件**：
- `gc/run.sh`：`do_replace()` 和 `do_restore()` 中的 `cp` 操作后自动执行 `codesign -f -s -`
- `AGENTS.md`：所有手动替换命令已加上 `codesign` 步骤

**适用范围**：仅影响 macOS arm64 平台。Linux 上不需要重签名。

## 6. 验证结果

使用 EnableGCFastPath=OFF 的编译器 + 修复后 runtime + 重签名后的 SDK 环境：

```
========================================
  Cangjie GC Benchmark Suite
========================================

--- Functional Tests ---
[BENCH] F1_TreeIntegrity         PASS  142 ms
[BENCH] F2_Generational          PASS  307 ms
[BENCH] F3_LargeObject           PASS  1519 ms
[BENCH] F4_LinkedList            PASS  118 ms
[BENCH] F5_HashMap               PASS  127 ms

--- Performance Tests ---
[BENCH] P1_SmallAlloc            237 ms  6553550 nodes  27652 nodes/ms
[BENCH] P2_LargeAlloc            838 ms  50 MB  61 MB/s
[BENCH] P3_MixedWorkload         531 ms  30 rounds
[BENCH] P4_LongLivedChurn        416 ms  persistent_intact=20/20
[BENCH] P5_Fragmentation         660 ms  15 rounds

========================================
[BENCH] Functional: 5/5 passed
[BENCH] Total wall time:    4899 ms
========================================
```

同时验证 EnableGCFastPath=ON 的编译器 + 修复后 runtime 也能正常工作（向后兼容）。

## 7. 经验总结

### 调试经验

1. **区分多个叠加问题**：本次调试中遇到了两个独立的问题（ABI 不匹配 + 代码签名失效），它们的表现不同（SIGBUS vs SIGKILL），需要分别定位和修复。如果只关注其中一个，会误以为修复无效。

2. **`DYLD_PRINT_LIBRARIES` 是 macOS 上调试动态库加载的利器**：通过对比正常/异常场景下的库加载日志，可以快速确定问题发生在加载阶段还是运行阶段。

3. **MD5 对比不能代替签名验证**：文件内容相同（MD5 一致）不意味着文件在系统层面等价。macOS arm64 的代码签名是文件元数据的一部分，`cp` 操作可能破坏签名而不改变文件内容。

4. **控制变量法排查**：通过 `/tmp` 隔离实验（复制 dylib 到全新目录），排除了"文件内容"因素，锁定了"文件状态/签名"因素。

### 开发规范

1. **macOS arm64 替换 dylib 后必须重签名**：
   ```bash
   cp <source> <target>.dylib
   codesign -f -s - <target>.dylib   # 必须！
   ```

2. **汇编 stub 设计应与编译器选项解耦**：runtime 的汇编 stub 不应假定编译器一定会生成特定的包装函数。`CalleeSavedRegistersStubNew` 的设计依赖了 `CJ_MCC_NewObjectFast` 的存在，违反了这一原则。

3. **交叉验证**：修复后应同时验证 ON 和 OFF 两种配置都能正常工作。
