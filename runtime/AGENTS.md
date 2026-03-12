# Cangjie Runtime - 项目上下文指南

## 项目概述

Cangjie Runtime（仓颉运行时）是仓颉编程语言的核心运行时环境，提供内存管理、垃圾回收、线程调度、异常处理、动态库加载等基础运行时服务。该项目使用 C/C++ 编写，基于 CMake 构建系统，支持多平台交叉编译。

### 主要技术栈
- **构建系统**: CMake (3.16.3+)
- **编译器**: Clang/Clang++
- **语言标准**: C99, C++14 (GNU++14 on Linux, C++14 on macOS/iOS)
- **目标平台**:
  - Linux (x86_64, aarch64, arm)
  - macOS (x86_64, aarch64)
  - Windows (x86_64)
  - OpenHarmony (aarch64, x86_64, arm)
  - Android (aarch64, x86_64)
  - iOS (aarch64, simulator)

### 项目架构
运行时采用模块化架构，主要模块包括：
- **Base**: 基础工具类（字符串、内存、时间、日志等）
- **Common**: 通用组件（对象管理、线程缓存、页缓存等）
- **Heap**: 内存管理和垃圾回收（分配器、收集器、屏障等）
- **ObjectModel**: 对象模型实现
- **Concurrency**: 并发原语和同步机制
- **Loader**: 动态库加载器
- **Exception**: 异常处理系统
- **Signal**: 信号处理（仅 Unix/Linux）
- **Sync**: 同步原语
- **CJThread**: 仓颉协程/轻量级线程调度器
- **Inspector**: 运行时检查和调试支持
- **UnwindStack**: 栈展开支持
- **Demangler**: 符号反混淆
- **Sanitizer**: 内存错误检测支持（ASAN, TSAN, HWASAN）
- **CpuProfiler**: CPU 性能分析

### 关键特性
- **垃圾回收**: 采用复制式垃圾回收（Copy GC）
- **协程调度**: 内置 CJThread 轻量级线程调度器
- **跨平台**: 支持 Linux、macOS、Windows、OpenHarmony、Android、iOS
- **内存安全**: 支持 ASAN、TSAN、HWASAN 等内存错误检测工具
- **动态库加载**: 支持运行时加载和初始化仓颉动态库

## 构建和运行

### 构建系统
项目使用 `build.py` Python 脚本作为主要构建入口，该脚本会调用 CMake 生成构建配置。

### 构建命令

```bash
# 基本构建（native 平台）
python3 build.py build --target native --build-type release -v 0.0.1

# 构建 Windows 交叉编译版本
python3 build.py build --target windows-x86_64 --build-type release -v 0.0.1 --target-toolchain /path/to/mingw-w64

# 构建 OpenHarmony 版本
python3 build.py build --target ohos-aarch64 --build-type release -v 0.0.1 --target-toolchain /path/to/ohos/toolchain

# 构建 Android 版本
python3 build.py build --target android-aarch64 --build-type release -v 0.0.1 --target-toolchain /path/to/android/ndk

# 构建调试版本
python3 build.py build --target native --build-type debug -v 0.0.1

# 启用 HWASAN
python3 build.py build --target native --build-type release -v 0.0.1 -hwasan

# 启用 Sanitizer 支持
python3 build.py build --target native --build-type release -v 0.0.1 --cjlib-sanitizer-support asan
```

### 构建目标选项
- `native`: 本地平台编译（Linux/macOS）
- `windows-x86_64`: Linux 到 Windows x86_64 交叉编译
- `ohos-aarch64`: Linux 到 OpenHarmony aarch64 交叉编译
- `ohos-x86_64`: Linux 到 OpenHarmony x86_64 交叉编译
- `ohos-arm`: Linux 到 OpenHarmony arm 交叉编译
- `android-aarch64`: Android aarch64 构建
- `android26-aarch64`: Android API 26 aarch64 构建
- `android31-aarch64`: Android API 31 aarch64 构建
- `android-x86_64`: Android x86_64 构建
- `ios-aarch64`: iOS aarch64 构建
- `ios-simulator-aarch64`: iOS 模拟器 aarch64 构建
- `ios-simulator-x86_64`: iOS 模拟器 x86_64 构建

### 构建类型
- `release`: 发布版本（默认，-O2 优化）
- `debug`: 调试版本（-O0 无优化）
- `relwithdebinfo`: 带调试信息的发布版本（-O2 优化）

### 安装命令
```bash
# 安装到默认路径（output/common）
python3 build.py install

# 安装到指定路径
python3 build.py install --prefix /custom/install/path
```

### 清理命令
```bash
python3 build.py clean
```

### 直接使用 CMake 构建
```bash
# 生成构建配置
cmake -DCMAKE_INSTALL_PREFIX=output/common_linux_x86_64_cjnative \
      -DCMAKE_BUILD_TYPE=Release \
      -DCOPYGC_FLAG=1 \
      -DDOPRA_FLAG=1 \
      -DOHOS_FLAG=0 \
      -DANDROID_FLAG=0 \
      -DIOS_FLAG=0 \
      -DMACOS_FLAG=1 \
      -DRUNTIME_TRACE_FLAG=1 \
      -DCJ_SDK_VERSION=0.0.1 \
      -S . -B CMakebuild

# 编译
cd CMakebuild
make cangjie-runtime -j32

# 安装
make install
```

### 构建产物
构建产物默认安装在 `output/common` 目录下：
```
output/common/
└── <target_platform>_<build_type>_<target_arch>/
    ├── bin/          # 可执行文件（如 cjfilt）
    ├── include/      # 头文件（Cangjie.h, RuntimeConfig.h）
    ├── lib/          # 静态库
    └── runtime/      # 动态库（libcangjie-runtime.so/dylib/dll）
```

临时文件生成在 `output/temp` 目录下。

## 开发约定

### 代码风格
- **C 语言**: 遵循 C99 标准
- **C++ 语言**: 遵循 C++14 标准（Linux 使用 GNU++14）
- **编译警告**: 启用 `-Wall -Wextra -Werror`，将警告视为错误
- **代码格式**: 遵循项目现有代码的格式风格
- **命名约定**:
  - 函数: 大驼峰命名法（如 `InitCJRuntime`）
  - 变量/参数: 小驼峰命名法（如 `heapParam`）
  - 宏定义: 全大写（如 `MRT_EXPORT`）
  - 类型定义: 大驼峰命名法（如 `CJThreadHandle`）

### 编译标志
- **C 标志**: `-std=c99 -fPIC -Werror -Wall -Wextra -fstack-protector-strong`
- **C++ 标志**: `-std=c++14 -fPIC -Werror -Wall -Wextra -fstack-protector-strong -fno-exceptions -fno-rtti`
- **优化级别**:
  - Release: `-O2 -D_FORTIFY_SOURCE=2 -gdwarf-4`
  - Debug: `-O0 -gdwarf-4 -DMRT_DEBUG=1`

### 平台相关代码
- 平台特定代码放在 `src/arch/<platform>/` 目录下
- 操作系统特定代码放在 `src/os/<os>/` 目录下
- 使用编译宏控制平台相关代码（如 `__OHOS__`, `__ANDROID__`, `__IOS__`, `WINDOWS_FLAG`）

### API 设计
- **导出符号**: 使用 `MRT_EXPORT` 宏标记需要导出的公共 API
- **C 兼容性**: 公共 API 使用 `extern "C"` 确保 C 兼容性
- **错误处理**: 使用 `RTErrorCode` 枚举返回错误码
- **文档注释**: 使用 Doxygen 风格注释记录 API

### 内存管理
- 运行时内置垃圾回收器，管理仓颉对象的内存
- 原生代码使用 `malloc/free` 或 C++ `new/delete` 管理内存
- 注意区分仓颉对象和原生对象的生命周期

### 线程和并发
- 使用 CJThread 实现轻量级协程调度
- 支持多种线程模型（通过 DOPRA_FLAG 控制）
- 注意线程安全和同步原语的使用

### 测试
- 目前项目使用覆盖率测试（COV 标志）
- 建议在修改核心组件后进行完整的构建和测试

### 第三方依赖
- **bounds_checking_function**: OpenHarmony 的边界检查函数库（从 gitcode.com 自动克隆）
- **CJThread**: 仓颉线程库（独立构建）

## 项目目录结构

```
runtime/
├── src/                          # 源代码目录
│   ├── Base/                     # 基础工具类
│   ├── Common/                   # 通用组件
│   ├── Heap/                     # 内存管理和 GC
│   ├── ObjectModel/              # 对象模型
│   ├── Concurrency/              # 并发原语
│   ├── Loader/                   # 动态库加载器
│   ├── Exception/                # 异常处理
│   ├── Signal/                   # 信号处理（Unix/Linux）
│   ├── Sync/                     # 同步原语
│   ├── CJThread/                 # 仓颉协程调度器
│   ├── Inspector/                # 运行时检查
│   ├── UnwindStack/              # 栈展开
│   ├── Demangler/                # 符号反混淆
│   ├── Sanitizer/                # 内存错误检测
│   ├── CpuProfiler/              # CPU 性能分析
│   ├── Utils/                    # 工具函数
│   ├── StackMap/                 # 栈映射
│   ├── arch/                     # 平台特定汇编代码
│   │   ├── aarch64_linux/
│   │   ├── x86_64_linux/
│   │   ├── aarch64_macos/
│   │   ├── x86_64_macos/
│   │   ├── aarch64_ios/
│   │   ├── x86_64_ios/
│   │   ├── arm_linux/
│   │   └── x86_64_windows/
│   ├── os/                       # 操作系统特定代码
│   │   ├── Linux/
│   │   ├── Macos/
│   │   └── Windows/
│   ├── Cangjie.h                 # 公共 API 头文件
│   ├── CangjieRuntime.cpp/h      # 运行时核心实现
│   ├── CangjieRuntimeApi.cpp     # 运行时 API 实现
│   ├── CompilerCalls.cpp/h       # 编译器调用接口
│   ├── ExceptionManager.cpp/h    # 异常管理器
│   ├── HeapManager.cpp/h         # 堆管理器
│   ├── LoaderManager.cpp/h       # 加载器管理器
│   ├── SignalManager.cpp/h       # 信号管理器
│   ├── TypeInfoManager.cpp/h     # 类型信息管理器
│   ├── LogManager.cpp/h          # 日志管理器
│   ├── StackManager.cpp/h        # 栈管理器
│   ├── ObjectManager.h           # 对象管理器（内联）
│   ├── CjScheduler.cpp/h         # CJThread 调度器
│   ├── CjTimer.cpp/h             # 定时器
│   └── RuntimeConfig.h           # 运行时配置
├── build/                        # 构建脚本和工具
│   ├── build_cjthread.sh         # CJThread 构建脚本
│   ├── build_cjthread_windows.bat
│   ├── lds/                      # 链接脚本
│   └── scripts/                  # 辅助脚本
├── third_party/                  # 第三方依赖
│   └── third_party_bounds_checking_function/
├── CMakeLists.txt                # 主 CMake 配置
├── config.cmake                  # CMake 配置
├── SetupAr.cmake                 # AR 工具设置
├── build.py                      # Python 构建脚本
├── build_runtime.md              # 英文构建文档
├── build_runtime_zh.md           # 中文构建文档
└── output/                       # 构建输出目录
    ├── common/                   # 最终安装路径
    └── temp/                     # 临时构建文件
```

## 关键编译宏

- `__OHOS__`: OpenHarmony 平台
- `__ANDROID__`: Android 平台
- `__IOS__`: iOS 平台
- `MRT_DEBUG`: 调试模式
- `MRT_USE_CJTHREAD`: 使用 CJThread
- `MRT_USE_CJTHREAD_RENAME`: 使用 CJThread 重命名
- `MRT_USE_COPYGC`: 使用复制式 GC
- `MRT_DUMP_ADDRESS`: 在日志中转储地址
- `COV_SIGNALHANDLE`: 覆盖率测试信号处理
- `CANGJIE_SANITIZER_SUPPORT`: 运行时 Sanitizer 支持
- `ENABLE_FORWARD_PTRAUTH_CFI`: 启用前向指针认证 CFI
- `ENABLE_BACKWARD_PTRAUTH_CFI`: 启用后向指针认证 CFI

## 常见问题

### 交叉编译问题
交叉编译需要指定正确的工具链路径：
- Windows: `--target-toolchain /path/to/mingw-w64`
- OpenHarmony: `--target-toolchain /path/to/ohos/toolchain`
- Android: `--target-toolchain /path/to/android/ndk`

### 平台限制
- Windows 平台不支持信号处理模块（使用 sigaction）
- GWPASAN 仅支持 Linux 和 OpenHarmony
- HWASAN 在 Windows 上不可用
- iOS/macOS 使用 `-target` 而不是 `--target` 标志

### 构建失败排查
1. 检查编译器版本（需要 Clang）
2. 检查 CMake 版本（需要 3.16.3+）
3. 检查工具链路径是否正确
4. 检查依赖是否正确克隆（bounds_checking_function）
5. 查看详细构建日志（使用 `VERBOSE=1`）

## 参考资料

- [仓颉 SDK 集成构建指导书](https://gitcode.com/Cangjie/cangjie_build/blob/dev/README_zh.md)
- [Cangjie License](https://cangjie-lang.cn/pages/LICENSE)
- 项目 Git 仓库: https://gitcode.com/Cangjie/cangjie_runtime.git

## 知识库（Skills）

项目维护了两套结构化知识文档，位于 `skills/` 目录下。AI 在处理相关任务时**必须先按需加载对应知识**，而非凭记忆猜测。

### 加载原则

1. **按需加载**：不要一次性读取所有文件，先读 `SKILL.md` 确定索引，再根据关键词进入对应子文档。
2. **逐层深入**：`SKILL.md` → 分类 `README.md` → 具体专题文档。
3. **知识回写**：工作过程中发现文档未覆盖或与代码不一致的内容，**必须更新到对应 references 文档中**。
4. **以代码为准**：文档与源码冲突时，以源码为准，同时修正文档。

---

### A. Cangjie 语言语法（cangjie-skills）

**用途**：编写 Cangjie 代码、调试语法错误、理解语言特性时使用。

**入口文件**：`skills/cangjie-skills/SKILL.md`

**加载顺序**：
1. 读 `skills/cangjie-skills/SKILL.md` — 获取总览与使用规则
2. 读 `skills/cangjie-skills/references/README.md` — 确定主题分支
3. 按问题类型进入 `skills/cangjie-skills/references/语法/README.md` — 7 大语法主题索引
4. 查规则：`skills/cangjie-skills/references/语法/语法速查.md` — 常见语法模板与示例
5. 写代码前后自检：`skills/cangjie-skills/references/语法/代码生成清单.md`

**目录结构**：
```
skills/cangjie-skills/
├── SKILL.md                          # 总入口（语法主题地图 + AI 生成原则）
└── references/
    ├── README.md                     # 阅读顺序指南
    └── 语法/
        ├── README.md                 # 7 大语法主题索引
        ├── 语法速查.md               # 变量/函数/类型/泛型/并发等速查模板
        └── 代码生成清单.md            # 写前准备 + 硬性规则 + 生成后自检
```

**触发关键词**：`Cangjie 代码`、`cjc 编译`、`语法`、`spawn`、`struct`、`enum`、`class`、`interface`、`match`、`泛型`、`Lambda`、`异常处理`、`import`、`package`

---

### B. Cangjie Runtime 知识（cangjie-runtime-skills）

**用途**：理解/调试 Runtime 内部机制（GC、协程、元信息、链接加载）、排障、构建验证时使用。

**入口文件**：`skills/cangjie-runtime-skills/SKILL.md`

**加载顺序**：
1. 读 `skills/cangjie-runtime-skills/SKILL.md` — 获取模块地图 + References 索引 + AI 执行规则
2. 根据关键词命中以下四大分支之一，先读 `README.md`，再进入主文档：

| 分类 | 入口 | 命中关键词 |
|------|------|-----------|
| **GC** | `references/GC/README.md` | Allocate、RegionSpace、write barrier、safepoint、Collector、Forward、OOM |
| **协程** | `references/协程/README.md` | CJThread、ProcessorSchedule、unwind、stack grow、netpoll、fd wait |
| **元信息** | `references/元信息/README.md` | TypeInfo、MTable、StackMap、反射、对象模型、泛型实例化 |
| **问题解决** | `references/问题解决/README.md` | build失败、OOM、GC停顿、调度卡住、反射失败、复盘 |

**GC 子目录详情**：
```
references/GC/
├── README.md
├── Allocator内存分配/    # Allocator.md、内存分配.md
├── CJBarrier/           # CJBarrier.md（读写屏障）
├── 内存管理/             # 基本概念.md、RegionList.md、GhostRegion.md、指针标记机制.md、safepoint.md 等
└── 回收器与任务/         # collector.md（主文档：Enum/Trace/Preforward/Forward/SATB/GCReason）
```

**协程子目录详情**：
```
references/协程/
├── README.md
├── 协程/       # 协程调度.md、其他调度器.md、FFI.md
├── 回栈/       # 回栈.md、异常.md、栈增长.md、信号.md
├── 网络/       # 网络调度与netpoll.md、fd生命周期与等待模型.md、socket调用路径.md
└── 其他/       # 析构函数.md
```

**元信息子目录详情**：
```
references/元信息/
├── README.md
├── 元数据/     # metadata.md、mtable表.md、stackmap概念.md、反射信息.md、对象模型.md
├── 泛型反射/   # 泛型.md、反射.md、虚函数.md
└── 链接加载/   # runtime启动流程.md
```

**问题解决子目录详情**：
```
references/问题解决/
├── README.md
├── 构建与验证问题.md
├── GC_问题排查.md
├── 协程_问题排查.md
├── 元信息_问题排查.md
└── 问题记录模板.md
```

**构建/验证脚本**：
```bash
# 统一脚本入口
python3 skills/cangjie-runtime-skills/scripts/build_copy_verify.py \
    --runtime-root <runtime路径> \
    --cangjie-root <cangjie包路径> \
    --platform <macos-aarch64|linux-x86_64>
```

---

### 知识更新规则

在工作过程中，以下情况**必须**更新对应 references 文档：

1. **发现新机制**：源码中发现文档未记录的重要机制或流程 → 补充到对应分类的专题文档中
2. **修正错误**：文档描述与代码实际行为不一致 → 以代码为准修正文档
3. **问题沉淀**：完成一次问题排查后 → 按 `references/问题解决/问题记录模板.md` 格式记录
4. **新语法发现**：发现 Cangjie 语法文档未覆盖的写法或陷阱 → 补充到 `语法速查.md` 或新建专题
5. **更新粒度**：每次只更新与当前任务相关的最小文档，不做大范围重构

## AI 工作规范

### 任务确认流程（必须遵守）

收到用户的任务指令后，**必须先确认理解再执行**，流程如下：

1. **理解阶段**：用自己的语言复述对任务的理解，包括：
   - 要做什么（目标）
   - 怎么做（方案/步骤）
   - 会影响哪些文件或模块
2. **提问阶段**：列出所有有歧义或不确定的点，向用户提问
3. **等待确认**：用户回复确认（如"可以"、"开始"）后，才能开始执行
4. **绝对不要**在未得到用户确认前就开始修改代码或文件

### 创建长文档的策略

为避免网络超时导致文件内容丢失，创建较长文档时**必须分步进行**：

1. **第一步：先用 `create_file` 创建带大纲的骨架文件**
   - 只写标题、章节标题、每节一句话摘要
   - 确保文件成功落盘
2. **第二步：逐节填充内容**
   - 每次用 `replace_string_in_file` 只填充 1-2 个章节
   - 每次替换完成后确认成功再继续下一节
3. **绝对不要**一次性用 `create_file` 写入超过 200 行的内容

## 本地开发验证流程（macOS aarch64）

### 1. 编译 Runtime

```bash
cd /Users/lipeisheng/code/cangjie/cangjie/cangjie_runtime/runtime
python3 build.py build
```

构建产物位于：
```
output/temp/lib/aarch64_Release/libcangjie-runtime.dylib
```

> **注意**: `output/common/` 是 `build.py install` 的安装路径，构建后的直接产物在 `output/temp/` 中。推荐使用 `gc/run.sh` 一键完成编译、替换、验证。

### 2. 替换 SDK 中的 dylib 并验证

SDK 路径：
```
/Users/lipeisheng/code/cangjie/cangjie-pkg/self/dev260313/cangjie/runtime/lib/darwin_aarch64_cjnative/
```

该目录下的文件说明：
- `libcangjie-runtime.dylib` — 当前使用的运行时（替换此文件）
- `libcangjie-runtime.dylib.origin` — 原始未修改版本（用于对比/回退）

替换命令：
```bash
cp /Users/lipeisheng/code/cangjie/cangjie/cangjie_runtime/runtime/output/temp/lib/aarch64_Release/libcangjie-runtime.dylib \
   /Users/lipeisheng/code/cangjie/cangjie-pkg/self/dev260313/cangjie/runtime/lib/darwin_aarch64_cjnative/libcangjie-runtime.dylib
```

回退到原始版本：
```bash
cp /Users/lipeisheng/code/cangjie/cangjie-pkg/self/dev260313/cangjie/runtime/lib/darwin_aarch64_cjnative/libcangjie-runtime.dylib.origin \
   /Users/lipeisheng/code/cangjie/cangjie-pkg/self/dev260313/cangjie/runtime/lib/darwin_aarch64_cjnative/libcangjie-runtime.dylib
```

### 3. 编译并运行测试用例

测试用例位于 `gc/` 目录下，主要文件为 `gc.cj`。

```bash
# 初始化仓颉编译环境
source /Users/lipeisheng/code/cangjie/cangjie-pkg/self/dev260313/cangjie/envsetup.sh

# 编译测试用例
cd /Users/lipeisheng/code/cangjie/cangjie/cangjie_runtime/runtime/gc
cjc gc.cj

# 运行可执行文件
./main
```

### 4. 完整一键流程

```bash
# 推荐方式：使用 run.sh 一键流程
cd /Users/lipeisheng/code/cangjie/cangjie/cangjie_runtime/runtime/gc
bash run.sh          # all: 编译 + 替换 + 测试
bash run.sh test     # 仅编译测试 + 运行（需已替换）
bash run.sh replace  # 仅替换 SDK dylib
bash run.sh restore  # 恢复原始 runtime
```

手动流程（备用）：
```bash
# 编译 → 替换 → 测试
cd /Users/lipeisheng/code/cangjie/cangjie/cangjie_runtime/runtime && \
python3 build.py build && \
cp output/temp/lib/aarch64_Release/libcangjie-runtime.dylib \
   /Users/lipeisheng/code/cangjie/cangjie-pkg/self/dev260313/cangjie/runtime/lib/darwin_aarch64_cjnative/libcangjie-runtime.dylib && \
source /Users/lipeisheng/code/cangjie/cangjie-pkg/self/dev260313/cangjie/envsetup.sh && \
cd gc && cjc gc.cj && ./main
```