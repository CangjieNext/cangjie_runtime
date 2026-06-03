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

### 切换 Runtime 后必须重新编译 .cj 文件（强制规则）

**规则**：每次替换 SDK 中的 `libcangjie-runtime.dylib`（如 CopyGC ↔ MS-GC 切换）后，**必须用 `cjc` 重新编译所有 `.cj` 测试文件**，不得复用旧 binary。

**原因**：`cjc` 编译时会链接当前 SDK 中的 runtime dylib，生成的二进制包含与特定 GC 模式匹配的初始化数据和符号引用。用 CopyGC SDK 编译的 binary 加载 MS-GC dylib（或反之）会导致进程在 dylib 加载阶段卡死（macOS 上表现为 UE 不可中断状态，`kill -9` 无效）。

**正确流程**：
```bash
# 1. 替换 runtime dylib
bash run.sh ms-replace    # 或 bash run.sh restore

# 2. 重新编译测试文件（必须！）
source /path/to/cangjie/envsetup.sh
cjc gc_benchmark.cj -o gc_benchmark

# 3. 运行测试
./gc_benchmark
```

**错误示例**（绝对禁止）：
```bash
# ✗ 用 CopyGC 编译 binary，然后换 MS-GC runtime 直接跑
cjc gc_benchmark.cj -o gc_benchmark   # CopyGC SDK
bash run.sh ms-replace                 # 换成 MS-GC
./gc_benchmark                         # ← 卡死！进程进入 UE 状态
```

### 多 SDK 对比测试的正确执行方式（必须遵守）

#### 问题根因

各 SDK 的 `envsetup.sh` 对 `DYLD_LIBRARY_PATH` 的处理方式不一致：
- SDK1/SDK2（自构建 MSGC）：`export DYLD_LIBRARY_PATH=<sdk>/runtime/lib/...:${DYLD_LIBRARY_PATH}` — **累加追加**
- SDK3（官方 CopyGC）：改用 `DYLD_FALLBACK_LIBRARY_PATH`，并清理旧路径

若在同一 shell 中先 `source SDK1/envsetup.sh` 再 `source SDK3/envsetup.sh`，`DYLD_LIBRARY_PATH` 中仍残留 SDK1 的 runtime 路径，优先级高于 SDK3 的 `DYLD_FALLBACK_LIBRARY_PATH`，导致 SDK3 程序静默加载了错误的 MSGC runtime，测试结果完全失真（例如 P2_LargeAlloc 从 420ms 变成 1087ms，SmallObj 分配次数从 5 万膨胀到 1.78 亿）。

#### 正确做法：每个 SDK 使用完全隔离的 `env -i` 子 shell

**绝对不要**在已有 `DYLD_LIBRARY_PATH` 的 shell 中直接 `source` 另一个 SDK 的 `envsetup.sh` 来切换 SDK。

**正确模板**：

```bash
CASE1=/path/to/case1_baseline

# SDK1 (MSGC 自构建)
env -i HOME=$HOME TERM=$TERM PATH=/usr/bin:/bin:/usr/sbin:/sbin \
  bash -c '
    source /Users/lipeisheng/code/cangjie/cangjie-pkg/self/dev260507/cangjie/envsetup.sh >/dev/null 2>&1
    MRT_REPORT="$CASE1/sdk1_runtime.log" cjHeapSize=256mb "$CASE1/bench_sdk1_run_new" 2>&1 | tee "$CASE1/sdk1_result.txt"
  '

# SDK2 (MSGC 仅屏障优化)
env -i HOME=$HOME TERM=$TERM PATH=/usr/bin:/bin:/usr/sbin:/sbin \
  bash -c '
    source /Users/lipeisheng/code/cangjie/cangjie-pkg/self/dev260505/envsetup.sh >/dev/null 2>&1
    MRT_REPORT="$CASE1/sdk2_runtime.log" cjHeapSize=256mb "$CASE1/bench_sdk2_run_new" 2>&1 | tee "$CASE1/sdk2_result.txt"
  '

# SDK3 (官方 CopyGC)
env -i HOME=$HOME TERM=$TERM PATH=/usr/bin:/bin:/usr/sbin:/sbin \
  bash -c '
    source /Users/lipeisheng/code/cangjie/cangjie-pkg/dev/20260507/cangjie/envsetup.sh >/dev/null 2>&1
    MRT_REPORT="$CASE1/sdk3_runtime.log" cjHeapSize=256mb "$CASE1/bench_sdk3_run_new" 2>&1 | tee "$CASE1/sdk3_result.txt"
  '
```

#### 运行前验证 dylib 来源（每次必做）

```bash
# 在对应的干净环境里确认加载的 runtime 路径
env -i HOME=$HOME TERM=$TERM PATH=/usr/bin:/bin:/usr/sbin:/sbin \
  bash -c '
    source /path/to/sdk/envsetup.sh >/dev/null 2>&1
    DYLD_PRINT_LIBRARIES=1 cjHeapSize=1m /path/to/bench 2>&1 | grep "cangjie-runtime" | head -3
  '
```

预期输出中，SDK1/SDK2 应该加载 `self/dev260507` 或 `self/dev260505` 下的 runtime，SDK3 应加载 `dev/20260507` 下的 runtime。若路径不匹配，立即停止测试排查原因。

#### macOS 上 `env` / `time` / 子 shell 组合的额外陷阱（必须遵守）

在 macOS 上，某些“看起来已经 `source envsetup.sh` 了”的写法，实际运行目标进程时**并没有正确继承 SDK 的 `DYLD_LIBRARY_PATH` / `DYLD_FALLBACK_LIBRARY_PATH`**，会导致二进制在 `dyld` 阶段直接挂住或报：

```text
Library not loaded: @rpath/libcangjie-runtime.dylib
Reason: no LC_RPATH's found
```

这类错误有几个典型特征：

- `run_stdout.txt` 为空
- `gc.log` 不生成
- `run_stderr.txt` / `time_stderr.txt` 里出现 `Library not loaded`
- 进程可能表现为 `UE` / 不可中断的异常状态

**规则**：

1. 不要在 `source envsetup.sh` 之后，再用额外一层会改变运行环境的包裹方式去执行目标程序，尤其要谨慎：
   - `env ... ./bench`
   - `/usr/bin/time -p env ... ./bench`
   - 多层 `bash -lc` / `bash -c` 混用

2. 最稳妥的运行模板是：

```bash
env -i HOME=$HOME TERM=$TERM PATH=/usr/bin:/bin:/usr/sbin:/sbin:/opt/homebrew/bin \
  bash -c '
    source /path/to/sdk/envsetup.sh >/dev/null 2>&1
    cd /path/to/case || exit 1
    cjc benchmark.cj -o bench
    DYLD_PRINT_LIBRARIES=1 cjHeapSize=1m ./bench 2>&1 | grep "cangjie-runtime" | head -3
    MRT_REPORT="$PWD/gc.log" cjHeapSize=256mb ./bench > run_stdout.txt 2> run_stderr.txt
  '
```

3. 如果要测纯执行时间，优先把计时命令放在**已经正确继承 SDK 环境的 shell 里面**，不要在外层再包一层会重置环境的 `env`。

4. 一旦运行时间明显超过正常值（例如本地已知应在 5s 左右），必须立即检查：
   - `run_stderr.txt`
   - `dyld_probe_stderr.txt`
   - `gc.log` 是否生成
   - `ps` 中进程状态是否为异常 `UE`

**绝对不要**在 `gc.log` 为 0 行、`run_stdout.txt` 为空、`dyld` 已经报错的情况下继续等待并误判为“只是跑得慢”。这不是性能问题，而是启动环境错误。

#### 各 SDK 期望的 GC 模式标志

| SDK | GC 模式 | runtime.log 特征 | AllocStats SmallObj 量级 |
|-----|--------|-----------------|--------------------------|
| SDK1（MSGC，IDLE+SWEEP phase check） | `[MS-GC]` | `MSRegionManager initialized` | ~1.7 亿 |
| SDK2（MSGC，仅屏障优化） | `[MS-GC]` | `MSRegionManager initialized` | ~1.7 亿 |
| SDK3（官方 CopyGC） | CopyGC | 无 `[MS-GC]`，有 `Region size:` | ~5 万（大量 inline fast path） |

若 SDK3 跑出的 SmallObj 分配量接近 1 亿量级，说明加载了错误的 MSGC runtime，数据无效。

---

### `CJ_MS_STW_SWEEP` 保留规则

- 当前阶段保留 `CJ_MS_STW_SWEEP` 作为 **诊断开关**，不要删除。
- 该开关的作用不是普通调优，而是把 sweep 从并发模式切到“下一轮 `PreEnumRoots()` 单线程 force-finish unswept backlog”的诊断路径，用来判断并发 sweep 是否仍在制造 stale edge / invalid stateWord / invalid typeInfo 问题。
- 在没有彻底修掉并发 sweep 根因之前，允许保留这个环境变量 gate；如果未来要删除，前提是已经证明默认并发 sweep 路径稳定，或已把这套行为改成正式默认策略。

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

### A. Cangjie 语言语法（cangjie-grammar-skill）

**用途**：编写 Cangjie 代码、调试语法错误、理解语言特性时使用。

**入口文件**：`skills/cangjie-grammar-skill/SKILL.md`

**加载顺序**：
1. 读 `skills/cangjie-grammar-skill/SKILL.md` — 获取总览与使用规则
2. 读 `skills/cangjie-grammar-skill/references/README.md` — 确定主题分支
3. 按问题类型进入 `skills/cangjie-grammar-skill/references/语法/README.md` — 7 大语法主题索引
4. 查规则：`skills/cangjie-grammar-skill/references/语法/语法速查.md` — 常见语法模板与示例
5. 写代码前后自检：`skills/cangjie-grammar-skill/references/语法/代码生成清单.md`

**目录结构**：
```
skills/cangjie-grammar-skill/
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

### B. Cangjie Runtime 知识（cangjie-runtime-skill）

**用途**：理解/调试 Runtime 内部机制（GC、协程、元信息、链接加载）、排障、构建验证时使用。

**入口文件**：`skills/cangjie-runtime-skill/SKILL.md`

**加载顺序**：
1. 读 `skills/cangjie-runtime-skill/SKILL.md` — 获取模块地图 + References 索引 + AI 执行规则
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
python3 skills/cangjie-runtime-skill/scripts/build_copy_verify.py \
    --runtime-root <runtime路径> \
    --cangjie-root <cangjie包路径> \
    --platform <macos-aarch64|linux-x86_64>
```

---

### C. Cangjie LLVM 后端知识（cangjie-llvm-skill）

**用途**：修改/调试仓颉 LLVM 后端、GC 屏障 lowering、statepoint/safepoint、Cangjie intrinsic、CJMetadata、目标后端代码生成，以及 runtime dylib 替换与 macOS codesign 问题时使用。

**入口文件**：`skills/cangjie-llvm-skill/SKILL.md`

**加载顺序**：
1. 读 `skills/cangjie-llvm-skill/SKILL.md` — 获取编译管道、常见任务与入口文件
2. 读 `skills/cangjie-llvm-skill/references/architecture.md` — 理解项目结构与模块划分
3. 读 `skills/cangjie-llvm-skill/references/gc-barriers.md` — 理解读写屏障、快速路径与 lowering 逻辑
4. 读 `skills/cangjie-llvm-skill/references/debugging.md` — 排查开关、Pass 顺序与验证流程
5. 查 `skills/cangjie-llvm-skill/references/file-index.md` — 做源码定位

**触发关键词**：`CJBarrierLowering`、`EnableGCFastPath`、`EnableGCPhase`、`EnableTaggedPointer`、`statepoint`、`safepoint`、`CJMetadata`、`intrinsic`、`AArch64AsmPrinter`、`X86MCInstLower`、`codesign`

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

### 执行长任务的失败复盘与规避（必须遵守）

以下规则用于避免“口头推进、实际未执行”“长命令超时后状态失真”“旧挂死进程干扰判断”等问题：

1. **先执行，后汇报**：用户要求“继续”“往下走”“开始构建/验证”后，必须先发起第一批真实工具调用，再汇报进度；禁止只回复“我现在去做某事”而没有实际执行。
2. **长链路必须分阶段**：像 runtime install、stdlib build/install、SDK 合并、benchmark 验证这类任务，必须拆成短阶段逐段执行；每一阶段结束后都要给出明确结果（成功、失败、卡住点），不能把整条链路当成一个大黑盒描述。
3. **超时后只能做两件事**：
   - 继续读取同一命令/同一任务的输出，确认它是否仍在推进；
   - 或明确宣布该命令已卡住/挂死。
   **禁止**在超时后跳去做无关检查，再把任务说成“还在执行”。
4. **先区分新任务与旧残留**：开始新的构建、编译或 benchmark 前，先检查是否存在历史残留的 `llc`、`benchmark`、`rss_monitor.py` 等进程。对长时间处于 `U` / `UE` / `UE+` 状态的进程，必须明确标记为“旧挂死进程”，不能当成当前任务仍在运行。
5. **不要用“当前活动终端最后一条命令”推断整体进度**：如果任务涉及多个终端或后台命令，必须以本轮实际启动的命令、产物文件、日志和退出码为准，而不是以任意一个终端最后停留的 `source .../activate`、`cd` 等命令为准。
6. **涉及 SDK 的验证必须说明三件套来源**：每次做 SDK 验证，都要明确写清楚 compiler、runtime、stdlib 分别来自哪里；如果脚本会在运行前替换 SDK 中的 `libcangjie-runtime.dylib`，必须显式说明，避免把“output compiler + output stdlib + 新 runtime dylib”的组合误说成“整套 output SDK 原样运行”。
7. **替换 runtime 后必须重新编译用例**：凡是替换了 SDK 中的 `libcangjie-runtime.dylib`，都必须重新用该 SDK 编译 `.cj` 测试文件后再运行；禁止复用旧 binary 做验证。
8. **不要在没有结果文件或退出码前宣称成功**：benchmark、编译、install、analysis 之类步骤，只有在产物、日志或退出码已经落地后，才能对外汇报“完成”。

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

### 当前 SDK 约定

| SDK 路径 | 类型 | 说明 |
|---------|------|------|
| `/Users/lipeisheng/code/cangjie/cangjie-pkg/self/dev260509/cangjie` | **自构建 MSGC SDK（最新）** | 从源码编译，包含**读屏障消除**
| `/Users/lipeisheng/code/cangjie/cangjie-pkg/dev/20260507/cangjie` | **Dev 原生 CopyGC SDK** | 当前 dev 分支官方发布的 CopyGC SDK，用于性能基线对比，**不要替换**其 `libcangjie-runtime.dylib` |

- **当前主力验证 SDK**：`self/dev260509/cangjie`（最新自构建 MSGC）
- **MSGC runtime 替换目录**：`/Users/lipeisheng/code/cangjie/cangjie-pkg/self/dev260509/cangjie/runtime/lib/darwin_aarch64_cjnative/`
- **稳定包备份**：各 SDK 目录下的 `libcangjie-runtime.dylib.origin` 用于回退

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
/Users/lipeisheng/code/cangjie/cangjie-pkg/self/dev260509/cangjie/runtime/lib/darwin_aarch64_cjnative/
```

该目录下的文件说明：
- `libcangjie-runtime.dylib` — 当前 MSGC SDK 正在使用的运行时（后续编译产物覆盖此文件）
- `libcangjie-runtime.dylib.origin` — 当前稳定包备份，用于回退或与新编译的 MSGC runtime 对比

替换命令：
```bash
cp /Users/lipeisheng/code/cangjie/cangjie/cangjie_runtime/runtime/output/temp/lib/aarch64_Release/libcangjie-runtime.dylib \
   /Users/lipeisheng/code/cangjie/cangjie-pkg/self/dev260509/cangjie/runtime/lib/darwin_aarch64_cjnative/libcangjie-runtime.dylib
# macOS arm64: cp 后必须重签名，否则 SIGKILL
codesign -f -s - /Users/lipeisheng/code/cangjie/cangjie-pkg/self/dev260509/cangjie/runtime/lib/darwin_aarch64_cjnative/libcangjie-runtime.dylib
```

回退到原始版本：
```bash
cp /Users/lipeisheng/code/cangjie/cangjie-pkg/self/dev260509/cangjie/runtime/lib/darwin_aarch64_cjnative/libcangjie-runtime.dylib.origin \
   /Users/lipeisheng/code/cangjie/cangjie-pkg/self/dev260509/cangjie/runtime/lib/darwin_aarch64_cjnative/libcangjie-runtime.dylib
codesign -f -s - /Users/lipeisheng/code/cangjie/cangjie-pkg/self/dev260509/cangjie/runtime/lib/darwin_aarch64_cjnative/libcangjie-runtime.dylib
```

### 3. 编译并运行测试用例

测试用例位于 `gc/` 目录下，主要文件为 `gc.cj`。

```bash
# 初始化仓颉编译环境
source /Users/lipeisheng/code/cangjie/cangjie-pkg/self/dev260509/cangjie/envsetup.sh

# 编译测试用例
cd /Users/lipeisheng/code/cangjie/cangjie/cangjie_runtime/runtime/gc
cjc gc.cj

# 运行可执行文件
./main
```

### 4. CopyGC 性能基线验证（不替换 runtime）

```bash
# CopyGC 基线只使用固定 SDK，不替换 runtime dylib
source /Users/lipeisheng/code/cangjie/cangjie-pkg/dev/20260507/cangjie/envsetup.sh
cd /Users/lipeisheng/code/cangjie/cangjie/cangjie_runtime_MSGC/runtime/gc
cjc gc.cj -o main_copygc
./main_copygc
```

### 5. MarkSweep GC 编译替换验证

`build.py` 通过 `--gc-mode` 参数选择 GC 算法，`copygc` 和 `marksweep` 互斥：
- `--gc-mode copygc`（默认）→ `-DCOPYGC_FLAG=1 -DMARKSWEEP_FLAG=0`
- `--gc-mode marksweep` → `-DCOPYGC_FLAG=0 -DMARKSWEEP_FLAG=1`

编译宏 `MRT_USE_MARK_SWEEP` 在 CMakeLists.txt 中由 `MARKSWEEP_FLAG=1` 触发。

#### 一键流程（推荐）

```bash
cd /Users/lipeisheng/code/cangjie/cangjie/cangjie_runtime/runtime/gc
bash run.sh ms-all     # 编译(MS) + 替换 + hello 测试
bash run.sh ms-build   # 仅编译 runtime (MarkSweep)
bash run.sh ms-replace # 编译(MS) + 替换 SDK dylib
bash run.sh ms-test    # 仅运行 hello 测试（需已替换）
bash run.sh restore    # 恢复原始 runtime
```

#### 手动流程

```bash
# 编译 MarkSweep runtime
cd /Users/lipeisheng/code/cangjie/cangjie/cangjie_runtime/runtime
python3 build.py build 

# 替换 + 重签名
cp output/temp/lib/aarch64_Release/libcangjie-runtime.dylib \
   /Users/lipeisheng/code/cangjie/cangjie-pkg/self/dev260509/cangjie/runtime/lib/darwin_aarch64_cjnative/libcangjie-runtime.dylib
codesign -f -s - /Users/lipeisheng/code/cangjie/cangjie-pkg/self/dev260509/cangjie/runtime/lib/darwin_aarch64_cjnative/libcangjie-runtime.dylib

# 编译测试 + 运行
source /Users/lipeisheng/code/cangjie/cangjie-pkg/self/dev260509/cangjie/envsetup.sh
cd gc && cjc gc_hello.cj -o main_hello && ./main_hello
```

#### 注意事项
- MSGC 验证统一替换 `self/dev260509/cangjie` 下的 `libcangjie-runtime.dylib`，`libcangjie-runtime.dylib.origin` 作为稳定回退包保留
- CopyGC 性能基线统一使用 `dev/20260507/cangjie`，禁止覆盖其 `libcangjie-runtime.dylib`
- MS runtime 使用 `gc_hello.cj` 做基础冒烟测试（而非 CopyGC 专用的 `gc.cj`）
- `gc.cj` 中可能包含 CopyGC 特定的内存布局假设，不适合直接用于 MS 验证
- 切换 GC 模式后务必 `restore` 再测另一种模式，避免 dylib 版本混淆

### 基准测试执行规范（M1 Pro 机器约定）

**必须加 `cjProcessorNum=6`，丢弃 Run 1，取 Run 2+ 的均值。**

**背景**：M1 Pro 有 6 个 P 性能核 + 2 个 E 效率核，共 8 核。CJThread 默认创建 `hardware_concurrency=8` 个 Processor 线程，其中约 2 个线程会被 macOS 调度到 E 核（频率约为 P 核的 1/3），导致批量协程任务（尤其是 P4_LongLivedChurn）出现 **100~200ms 级别的偶发高抖动**。macOS 不支持 `sched_setaffinity` 硬绑定核心，只能通过限制 Processor 数量来规避。

**验证数据（2026-05-08）**：
- 8 proc（默认）：P4 出现 191ms 异常值，Total 最高达 478ms
- 6 proc：P4 稳定在 61~84ms，Total 稳定在 294~323ms，191ms 异常完全消失

**标准运行命令**：
```bash
source /path/to/envsetup.sh >/dev/null 2>&1
# Run 1 为冷启动，丢弃；取 Run 2+ 均值作为基准
for i in 1 2 3 4 5; do
  echo "=== Run $i ==="
  CJ_MS_GC_DISABLE=1 cjProcessorNum=6 ./bench_nophase_O2 2>/dev/null | grep -E "P[1-5]_|Total"
done
```

**稳态基准（Run 3-5 均值，no-GC + 6 proc）**：
```
P1=49ms  P2=7ms  P3=25ms  P4=62ms  P5=17ms  Total=294ms
```
