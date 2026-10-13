# 仓颉 Runtime 代码区间标记方案（macOS/iOS）

本文描述 `StackManager::IsRuntimeFrame` 所需的 runtime 代码地址区间（`[start, end)`）
的获取方式。本方案用 archive 内嵌 marker 符号替代 `__TEXT,__cjrt_text` 自定义代码段。
跨段 `bl` 超 128MB 的问题随之消失。Xcode 集成方零新增配置。

术语约定：本文的"区间"一律指 `[start, end)` 形式的 PC 地址范围。"marker"指只提供
地址、不被执行的边界函数。

## 一、背景

### 1.1 需求

GC 精确扫栈时必须区分 runtime 帧与托管帧。只有托管帧携带 stack map，可以被精确
扫描。runtime 帧与 C2N/N2C 等 stub 帧没有 stack map，必须跳过。

帧分类入口是 `StackManager::IsRuntimeFrame(pc)`。该函数依赖 runtime 代码的 PC
地址区间。

### 1.2 旧实现

构建期对 runtime 的每个 `.o` 执行 `ld -r -rename_section __TEXT,__text,__TEXT,__cjrt_text`。
脚本是 `build/apple_rename_section.sh`（本方案已删除）。脚本挂在 `cangjie-runtime-static`
与 `cangjie-thread` 两个 target 的 PRE_BUILD 阶段。

链接器把同名段合并为连续输出段。加载时 `src/os/Macos/Section.cpp` 调用 `getsectiondata`
取段边界作为区间。

`ld -r` 只改段名。机器码、符号、重定位保持不变。segment 仍为 `__TEXT`。区间连续性
来自链接器的同名段合并性质。

### 1.3 问题

runtime 代码因此成为最终镜像中的第二个代码段。它与用户的 `__TEXT,__text` 的距离
可以超过 AArch64 `bl` 指令的 ±128MB 寻址范围。此时链接直接报错。

lld 的跨段 thunk 存在前向盲区。Apple ld 的 branch islands 也未覆盖该场景。

临时修复引入了 `code-model=large`。每个 runtime 入口调用点改为 GOT 间接调用，
增加 8 字节。该修复还需要维护一份手工白名单。这带来持续的 codesize 与维护成本。

## 二、核心思路

runtime 代码回归单一 `__TEXT,__text`。最终镜像只有一个代码段。跨段 `bl` 问题随之
消失。`code-model=large` 可以整体撤销。

区间端点改由两个 marker 函数的地址提供。"marker 夹住全部 runtime 代码"这一性质由
archive 成员序保证。Apple ld 按成员序落位拉取的 archive 成员。该性质已实测证实。
因此区间正确性由构建机器控制。接入方的链接行安排不承担任何责任。

核心原则：`IsRuntimeFrame` 需要的是一对地址，不是一个段。段只是获取这对地址的载体
之一。marker 符号是另一种载体。后者不产生布局连锁反应。

## 三、方案设计

### 3.1 每个静态库自括号

"自括号"指：每个 archive 用自己的首尾 marker 夹住自己的全部代码。

```
libcangjie-runtime.a                 libcangjie-thread.a
├─ CjTextStart.o (__CJ_Text_Start)   ├─ CjThreadTextStart.o (__CJ_Thread_Text_Start)
├─ runtime 全部成员 .o               ├─ CJThread 全部成员 .o
└─ CjTextEnd.o   (__CJ_Text_End)     └─ CjThreadTextEnd.o  (__CJ_Thread_Text_End)

链接后（Apple ld 按成员序落位）：
[__Text_Start] [runtime 代码] [Text_End] [Thread_Start] [thread 代码] [Thread_End] [用户/三方代码]
└—————————— runtime 区间 ——————————————┘ └—————————— cjthread 区间 ——————————————┘
```

设计要点：

1. 成员顺序由 CMake `target_sources` 的首尾位置写死。顺序由构建机器控制，不经由
   接入方之手。
2. 两个库的 marker 使用不同符号名。若同名，链接器只拉取第一个定义者。第二个
   archive 的 marker 永远进入不了二进制。区间会静默塌缩。
3. 两个库各自自括号。第三方库无论排在链接行哪个位置，都落在区间之外。
4. 附带收益：原先未改名的 `__StaticInit` 段现在也落在区间内。
5. marker 用 C 实现，不用逐架构汇编。`__attribute__((used))` 防止删除。函数体含
   互异的 volatile 常量，防止 ICF 折叠。

### 3.2 区间消费链路

`src/os/Macos/Section.cpp` 在加载时（constructor）取四个 marker 地址。每个区间经
`SetRuntimeRange` 校验。缺失或倒置时输出 stderr 并 abort。旧实现是静默降级，新实现
响式失败。校验通过后填入 `g_runtimeStaticStart/End` 与 `g_cjThreadStaticStart/End`。

`src/StackManager.cpp` 的 Apple 静态路径填充 `StackManager::rtStartAddr/rtEndAddr`
与 `cjThreadStartAddr/cjThreadEndAddr`。这与 Windows 路径的双区间模式对齐。

判定逻辑 `IsRuntimeFrame` 零改动。GC 扫根、栈回溯、信号处理等下游零改动。

### 3.3 动态模式

动态构建（`COMPILE_DYNAMIC`）下 runtime 与 thread 为 dylib。动态路径不消费区间：
macOS 动态按 dylib 文本范围判定。iOS 动态按 `dladdr` 判定。

但 `section.o` 仍引用 marker 符号。两个 SHARED 目标编入 marker 对象并导出，保证
链接可解析。此时区间值指向 dylib 内部，无副作用。

### 3.4 旧机制的移除

1. 删除 `build/apple_rename_section.sh`。
2. 移除 `cangjie-runtime-static` 与 `cangjie-thread` 的 PRE_BUILD rename 调用。
3. 8 个 stub 汇编文件（C2NStub/N2CStub/I2NStub 的 macos/ios 变体）中 12 处
   `.section __TEXT,__cjrt_text,regular` 恢复为 `.text`。
4. `Cjstart.S` 保持原样。cjstart.o 仍是首个链接输入。它承担 `__CJMETAHEADER`
   元数据头与 mod_init_func 职责。这些职责与文本区间无关。

## 四、正确性依赖与失败模式

本方案依赖一个链接器性质：Apple ld 将拉取的 archive 成员按成员序落位。

lld 不满足该性质。lld 的成员按符号解析的拉取序插入 `inputFiles`（见 lld
`InputFiles.cpp` 的 `ArchiveFile::fetch`）。同样的 archive 在 lld 下区间错乱。

| 失败场景 | 暴露方式 |
|---|---|
| archive 被重组、外部对象混入成员之间 | 校验脚本：区间内出现白名单外符号 |
| 用 lld 链接、成员序被破坏 | 校验脚本：archive 符号落在所有区间之外 |
| marker 对象缺失 | 链接期 undefined symbol |
| 区间倒置或为空 | init 时 stderr + abort |

校验脚本用法（Xcode Build Phase 或 CI）：

```
check_cj_text_range.sh <链接产物> cjstart.o section.o libcangjie-runtime.a libcangjie-thread.a
```

说明：来自 `.a` 的符号必须落在某个区间内。来自 glue 对象（cjstart.o、section.o）的
符号合法地落在区间外（它们链接在 archive 之前），仅进入白名单。

## 五、实施清单

| 类别 | 文件 | 内容 |
|---|---|---|
| 新增 | `src/os/Macos/CjTextStart.c` / `CjTextEnd.c` | runtime archive 首尾 marker |
| 新增 | `src/CJThread/src/base/CjThreadTextStart.c` / `CjThreadTextEnd.c` | thread archive 首尾 marker |
| 新增 | `build/check_cj_text_range.sh` | 链接产物双向校验 |
| 修改 | `src/os/Macos/Section.cpp` | 双区间填充 + `SetRuntimeRange` 校验 |
| 修改 | `src/StackManager.cpp` | Apple 静态路径补 cjThread 区间 |
| 修改 | `CMakeLists.txt` / `src/CJThread/src/CMakeLists.txt` | marker 以首/末成员接入两个静态库；SHARED 目标编入 marker；移除 rename 调用 |
| 修改 | 8 个 stub `.S`（macos/ios 的 C2NStub/N2CStub/I2NStub） | 12 处 `.section __cjrt_text` → `.text` |
| 删除 | `build/apple_rename_section.sh` | — |

不改动：`IsRuntimeFrame` 及全部下游（GC 扫根、回栈、信号处理）。Linux 保持链接脚本
边界符号机制（`cjld.lds`）。Windows 保持 PE 段改名机制。

## 六、验证结果

验证环境为 Xcode 26 链接器，端到端：

1. 双 archive 自括号落位正确。runtime 区间夹住全部 runtime 符号。cjthread 区间夹住
   全部 thread 符号。用户代码在所有区间之外。运行自检通过。
2. 模拟成员序破坏（lld 式）。校验脚本报出区间外符号。exit 1。
3. marker 缺失。链接期 undefined symbol。
4. 全部新增与修改源文件在 arm64 与 x86_64 编译通过。

## 七、收益

| 维度 | 旧方案（cjrtext） | 本方案 |
|---|---|---|
| Xcode 集成 | 无感知 | 无感知。零新增输入、参数、顺序契约 |
| 最终镜像 | 两个代码段，存在 bl 跨距风险 | 单一 `__TEXT,__text` |
| code-model=large | 必需。+8B/调用点 + 手工白名单 | 可整体撤销 |
| 区间正确性保证者 | 链接器的段合并机制 | 构建机的 archive 成员序 + 校验脚本 |
| 失败暴露 | 链接期报错（bl 超限） | 构建期或 CI 响式报错（脚本 + init abort） |

## 八、灰度隔离（iOS 先行，macOS 保持旧行为）

改动以 `__IOS__` 宏隔离（`config.cmake` 仅为 iOS/模拟器构建定义，macOS 不定义；
StackManager.cpp 已有同名用法先例）。

| 平台 | 区间来源 | 构建机制 |
|---|---|---|
| iOS（真机/模拟器） | archive 首尾 marker | 不执行 rename；`cjtext_start/end` 与 `cjthread_text_*` 作为 archive 首尾成员 |
| macOS | `getsectiondata(__TEXT, __cjrt_text)` | 保留 rename（`apple_rename_section.sh` 仅在 `MACOS_FLAG` 下挂载）；stub 汇编保留段名 |

具体守卫：`Section.cpp` 的 marker 声明、`g_cjThreadStaticStart/End`、`SetRuntimeRange`
及填充调用均在 `#if defined(__IOS__)` 内，`#else` 分支恢复旧 `getsectiondata` 逻辑；
`StackManager.cpp` 仅 iOS 填 cjThread 区间；两个 CMakeLists 的 marker 接线仅在 iOS
分支生效，macOS 分支恢复 rename 调用（`RENAME_PLATFORM=macos_cangjie`）。

注意：`check_cj_text_range.sh` 仅适用于 iOS 产物；对 macOS 产物（无 marker）会报
"range markers missing"，属预期。

文件编译说明：本改动只使用 C++11 以内特性（`__attribute__((used))`、volatile、
fprintf），在显式指定标准的条件下与 Apple clang 15（Xcode 15）及 clang 21（Xcode 26）
均兼容。runtime 构建通过 `config.cmake:373-376` 显式设置 `-std=c++14/gnu++14`，
不受个别工具链默认标准的影响。注意：本机 shell 的 `xcrun` 会解析到
`~/Downloads/Xcode-beta.app`（clang 15，默认标准低于 C++11），脱离构建系统做临时
编译验证时需显式加 `-std=c++17` 或固定 `DEVELOPER_DIR`。

## 九、后续事项

1. 重编 runtime SDK。用触发过 bl 超限的真实应用复测。
2. cjc 停止传递 `code-model=large`。回退 llvm 侧对应提交（GOT 间接调用分支与手工
   白名单）。
3. 将 `check_cj_text_range.sh` 接入 Xcode Build Phase 或 CI。
4. 重建 `build/cjthread_build/`。陈旧的生成文件仍引用已删除的 rename 脚本。
5. 追加 stackmap 优先分类：帧分类先查 PC 是否命中 `__cjstackmap`。命中即 MANAGED。
   依据是编译器保证的不变量：托管帧必有 stack map，runtime 帧必无。该不变量与链接
   布局无关。此改动使 GC 正确性独立于本方案的布局假设。最坏失败降级为打印噪音。
