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

## 八、灰度记录（iOS 先行，现已推广到 macOS）

第一版以 `__IOS__` 宏隔离（`config.cmake` 仅为 iOS/模拟器构建定义），macOS 保持
旧行为。macOS 拓展（当前状态）已将 marker 机制推广到全部 Apple 平台：

| 平台 | 区间来源 | 构建机制 |
|---|---|---|
| iOS（真机/模拟器） | archive 首尾 marker | 不执行 rename；`cjtext_start/end` 与 `cjthread_text_*` 作为 archive 首尾成员 |
| macOS | archive 首尾 marker（同 iOS） | 同 iOS；`apple_rename_section.sh` 已删除，macOS stub 汇编恢复 `.text` |

macOS 拓展的具体差异（相对第一版）：

1. `Section.cpp` 与 `StackManager.cpp` 的 `__IOS__` 守卫改为 `__APPLE__`，旧
   `getsectiondata(__cjrt_text)` 分支删除。
2. macOS 的 4 处 stub `.section __TEXT,__cjrt_text,regular`（aarch64_macos 的
   C2NStub.S 一处、N2CStub.S 三处）恢复为 `.text`。x86_64_macos 的 stub 原本
   就在 `.text`，无需改动。
3. 两个 CMakeLists 的 rename 调用删除，`apple_rename_section.sh` 删除。
4. install 阶段针对 Apple 静态库的 `objcopy --rename-section .text=...` 加了
   Apple 排除。该步原本依赖 `objcopy` 在 macOS 上缺失而静默跳过；若构建机装有
   binutils 的 objcopy，rename 会真实执行并破坏 marker 区间。现在显式排除。
5. macOS 动态（`COMPILE_DYNAMIC`）帧分类改走 `dladdr` 镜像判定（与 iOS 动态
   一致）。共享库的链接顺序是 SRC_LIST 对象在前、marker 经 target_link_libraries
   追加在后，marker 区间不夹住共享库全部代码，因此动态路径不消费区间。

注意：`check_cj_text_range.sh` 现在同时适用于 iOS 与 macOS 产物。

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

## 十、archive 成员序保证的源码与实测论证

本节回应"成员按 .a 内物理序落位"的前提属于哪一级保证。证据分三级：
**契约**（源码注释/man page 明文承诺）、**源码佐证**（实现行为，可引用 file:line）、
**实测**（本机冒烟测试跑通）。以下 ld64 源码引用均指
github.com/apple-oss-distributions/ld64 tag `ld64-957.1`（经典 ld64 最后一个开源
版本）；lld 引用指本机 llvm-project 仓库。

### 10.1 结论表

| 链接器 | 成员序→布局保证 | 等级 | 证据 |
|---|---|---|---|
| ld64 经典（开源 ≤957.1；本机 ld_classic=ld64-951.9） | 成员按 (archive 链接行位置, 成员物理序) 落位 | 契约 + 源码佐证 + 实测 | 契约：ld.hpp:391-403 注释明文（"Input files extracted from archives are ordered using the ordinal of the archive itself plus the index of the object file within the archive"）。源码：passes/order.cpp:657-703 对每个 section 无条件排序，比较器 :252-259 按 file ordinal（即 archiveOrdinalWithMemberIndex，archive_file.cpp:421、ld.hpp:450）。实测：ld_classic 默认模式通过，why_load 证明提取序≠落位序（10.4） |
| Apple 新 ld（Xcode 15.4 = ld-1053.12；Xcode 26.6 = ld-1267） | 同上 | 实测（未开源，无法源码验证） | 10.4 全模式通过；补拉成员落 archive 位置（10.3 Q2） |
| lld-MachO | 不成立：布局 = 符号解析拉取序 | 源码佐证 | lld InputFiles.cpp:104 `SetVector inputFiles`；ArchiveFile::fetch :2215-2240 拉取即 :2237 插入；Writer.cpp:877-919 无 order file/PGO 时不重排，Writer.cpp:949-956 按 inputSections 顺序落位 → 布局序 = 拉取序 ≠ 成员序。本机无 lld 二进制，未实测 |
| 任何链接器 + LTO（archive 成员为 bitcode） | 失效（classic 源码级）/ 实测侥幸保持 | 见 10.3 Q6 | classic：LTO 输出单对象按 LTOPartition 落位（lto_file.cpp:1084、:1433-1464、ld.hpp:402），函数序由 LLVM 决定。实测：三种 ld 的 LTO（-O0/-O2）括号均保持，但依赖 LLVM 行为，非契约 |

### 10.2 Q1：经典 ld64 的完整实现路径

提取与布局是两条独立路径，靠 ordinal 汇合：

1. 成员枚举与索引。archive_file.cpp:297 定位首个成员（TOC）；:330-344 非 lazy
   模式下按物理序线性遍历并 eager parse 全部成员；:374-404 `makeObjectFileForMember`
   按物理序推导 `memberIndex`；:421 成员 ordinal = `archiveOrdinalWithMemberIndex
   (memberIndex)`，编码为 (partition=argList, majorIndex=链接行参数位, minorIndex=
   filelist 位, counter=成员索引)（ld.hpp:416-418、:448-450）。
2. 提取（决定哪些成员进链接，不决定位置）。
   - forceLoad（-all_load/-force_load）：ctor eager parse（archive_file.cpp:327-345），
     forEachAtom 在 archive 的链接行位置按物理序加载（archive_file.cpp:468-490，
     由 InputFiles.cpp:1349 调用）。
   - lazy（默认，本方案路径）：resolveAllUndefines 多轮循环（Resolver.cpp:990-1026，
     外层 :995、内层 :1002-1005）→ resolveCurrentUndefines（:918-965，:941）→
     InputFiles::searchLibraries（InputFiles.cpp:1417-1460，archive JIT 查找 :1450）→
     justInTimeforEachAtom（archive_file.cpp:580-597，TOC 哈希 :650-667 反向构建，
     重名时取 TOC 最先）。**提取顺序 = 未定义符号名字典序**（SymbolTable.cpp:565-575，
     :574 显式 sort）。注意：`__CJ_Text_End` 字典序在 `__CJ_Text_Start` 之前，
     实测 why_load 确认 End.o 先被拉取 —— 提取序与成员序相反。
3. 原子收集与初始布局。Resolver::doAtom 依次 push（Resolver.cpp:721）→
   fillInInternalState 按该顺序入 section（:1949-1954）→ InternalState::addAtom
   顺序 append（ld.cpp:733、:900/905）。此时代码顺序 = 提取顺序。
4. **布局重排（关键一环）**。ld.cpp:1544 无条件执行 `ld::passes::order::doPass`
   （注释："must run after code dedup"）。order.cpp:637-716 对除 stub/指针类外的
   每个 section 排序，比较器优先级：order_file 命中 → section$end → tentative →
   cold → **按 .o 文件 ordinal（:252-259，注释 "sort by .o order"）** → 对象内
   原子地址（:265-271）。
5. 因此最终落位 = (archive 在链接行的位置, 成员物理索引)，与提取顺序无关。
   dead-strip（Resolver.cpp:1351-1425）用 stable_partition（:1411）只删不排；
   -u 符号是 dead-strip root（:1291-1299）。

### 10.3 Q2/Q3/Q6：多轮补拉与边界情况

**Q2 补拉落位。** 链接行靠后的 .o 引用靠前 archive 的成员时，补拉发生在
resolveAllUndefines 的某一轮，但成员 ordinal 不变（archive 的链接行位 + 成员索引），
order pass 把它排到 archive 的位置。实测（10.4 late_pull 模式）：libaux.a 在 user.o
之前，user.o 引用 aux 成员；nm -n 显示 `_cj_aux_fn` 落在 `_user_fn` 之前
（ld_classic 3e50 < 3e78；ld-1053.12 3e40 < 3e68）—— 补拉成员落在 archive 的
链接行位置，而非提取时刻的末尾。

**Q3 边界情况。**

| 开关 | 提取影响 | 落位影响 | 证据 |
|---|---|---|---|
| -all_load（Options.cpp:2775） | 全成员 eager parse + 物理序加载 | archive 位置，成员序（order pass 同样按 ordinal） | Options.cpp:930-932；archive_file.cpp:471-490；实测通过 |
| -force_load <path>（Options.cpp:2785） | 同上，单 archive | 同上 | 实测通过 |
| -ObjC（Options.cpp:2781，:934-935） | 类成员按 TOC 哈希序加载（archive_file.cpp:493），类别成员物理扫描（:507-539）—— 提取序非成员序 | 布局仍由 order pass 按 ordinal 重排，成员序保持 | 实测通过 |
| weak 定义成员 | 无未定义引用不提取；被 -u/引用拉入后照常落位 | 成员序保持 | 实测（weak end marker）通过。风险：弱符号可与他处强定义共存不报错，marker 必须用专名（已在实施时采用） |
| -dead_strip | 不提取额外成员 | stable_partition 只删不排（Resolver.cpp:1411）；-u 符号是 root（:1291-1299） | 实测通过 |

**Q6 LTO。** classic 源码级结论：成员序 → 布局映射在 LTO 下失效。bitcode 成员
parse 时 ordinal 仍带成员索引（archive_file.cpp:431-433），合并前按 ordinal 排序
（lto_file.cpp:1502-1503，注释 "The order that files are merged must match command
line order"），但 codegen 输出是单个临时对象、以 LTOOrdinal（partition 2，最后）
落位（lto_file.cpp:1084、:1433-1464；ld.hpp:402 注释 "The LTO object file is last"），
对象内函数顺序由 LLVM 决定，ld64 无契约。embedded bitcode（-fembed-bitcode）成员
仍是含原生码的 Mach-O，走普通布局路径，成员序保持；__LLVM bundle 按 ordinal 排序
（bitcode_bundle.cpp:737-741）。实测：ld-1053.12 / ld-1267 / ld_classic 的
全 LTO archive（-O0 与 -O2）括号均保持 —— 属 LLVM 当前行为的实测级证据，非契约。
本方案的构建不开 LTO；若未来开启，成员序前提需重新评估。

### 10.4 Q4：新 ld 开源状态与实测记录

apple-oss-distributions/ld64 的 tag 列表（`git ls-remote --tags` 核实）最新为
`ld64-957.1`。Xcode 15.4 的 `ld-1053.12` 与 Xcode 26.6 的 `ld-1267` 均未开源，
**无法源码验证**，其保证等级只到实测。

冒烟测试（`build/cj_marker_order_smoke_test.sh`）记录：

| 工具链 | ld 版本串 | default | all_load | force_load | objc | dead_strip | weak | lto | late_pull |
|---|---|---|---|---|---|---|---|---|---|
| Xcode 15.4 (15F5021i) | `PROJECT:ld-1053.12` | OK+run PASS | OK | OK | OK | OK | OK | OK | OK |
| Xcode 26.6 (17F113) | `PROJECT:ld-1267` | OK+run PASS | OK | OK | OK | OK | OK | OK | OK |
| Xcode 15.4 `-ld_classic` | `PROJECT:ld64-951.9` | OK | — | — | — | — | — | — | — |

附加实测：ld_classic `-why_load` 显示提取序为符号名字典序（`__CJ_Text_End` 先于
`__CJ_Text_Start`），但 `nm -n` 布局仍为 Start<rt1<rt2<End —— 直接证明
"提取序 ≠ 落位序，落位序 = 成员序"。binary 内自检（section.o 比较四个 marker 与
rt 符号地址）在可运行模式全部 PASS。测试目标 arm64-apple-macos12.0；iOS 真机目标
（arm64-apple-ios12.0.0，iphoneos SDK）的 default/late_pull/dead_strip 三模式
nm 校验亦全部通过（无法本机运行自检）。

补充论证（2026-10-14）：
1. 演示所用的 ld_classic 版本串为 `ld64-951.9`，该版本同样开源。与其后 957.1 逐文件
   diff：`passes/order.cpp` 与 `SymbolTable.cpp` **完全一致**；`archive_file.cpp`
   仅 riscv 宏命名差异。order pass 调用点 951.9 在 `ld.cpp:1565`、957.1 在 `:1544`，
   "sort by .o order" 比较器两者均在 `order.cpp:252-259`。即本节机制描述对实测
   版本逐行成立。
2. 新 ld（1053.12/1267，未开源）实测同一场景：`-why_load` 输出与 ld_classic 逐字
   一致（提取序 = 字母序，trace 中 `[n]` 即成员物理索引，end.o=[5] 最先拉取），
   `nm -n` 落位 = 成员序。二进制 `strings` 可见 ordinal 体系仍在
   （`File::Ordinal::Partition::lto`、`setSymbolTableOrdinal`、`ordinal < numAtoms`）。
   外部契约相同；内部实现无源码可证，证据等级为二进制 strings + 实测。

### 10.5 复跑方法

```bash
# 两个工具链全模式（Xcode 升级后重跑即可；支持任意多个 --xcode）
runtime/build/cj_marker_order_smoke_test.sh \
    --xcode /Applications/Xcode.app \
    --xcode /Applications/Xcode26.app --keep

# 仅默认模式；iOS 目标（nm-only，不运行自检）
runtime/build/cj_marker_order_smoke_test.sh \
    --xcode /Applications/Xcode.app --modes default --target ios

# 选项：--workdir DIR 保留产物；--modes 逗号分隔
#   default,all_load,force_load,objc,dead_strip,weak,lto,late_pull,ld_classic
```

脚本自包含（源码内嵌、按成员序 ar cr + ranlib 建档、链接行模拟本方案：
section.o 在前引用 runtime marker、-u 强制 thread marker、libaux.a 验证补拉落位），
退出码非 0 即存在括号破坏。结论表任何一行的 file:line 引用对应
ld64-957.1 源码树（见 10.1 注）。
