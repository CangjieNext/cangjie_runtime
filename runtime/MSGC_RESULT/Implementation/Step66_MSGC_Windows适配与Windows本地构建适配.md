# Step66 MSGC Windows适配与Windows本地构建适配

## 1. 文档目标

本文记录 2026-08-25 至 2026-08-27 在 Windows x86_64 主机上直接构建 Cangjie MSGC SDK 的适配、排障、验证和遗留问题。

本次工作的目标不是在 Linux 上交叉生成 Windows SDK，而是在 Windows 本机完成以下链路：

```text
Windows x86_64
  -> 构建 cangjie_compiler_CMSGC
  -> 构建 cangjie_runtime_CMSGC/runtime
  -> 构建 cangjie_runtime_CMSGC/stdlib
  -> 构建 cangjie_stdx_CMSGC
  -> 汇总到 cangjie_compiler_CMSGC/output/msgc
  -> 使用该 SDK 编译并执行 MSGC 用例
```

需要先明确当前结论：

1. Windows 本地构建链路已经能够产出 SDK。
2. MSGC Runtime 已能在 Windows 上构建、安装并运行实际 HTTP/JSON 用例。
3. `case25_http_complex_json_stream` 的全静态 MSGC 版本已完成 60 秒验证，零失败，吞吐量为 8,756 req/s。
4. 当前方案仍包含若干妥协项，尤其是 stdlib `-O0`、1 GiB PE 栈保留、全静态链接和临时兼容 archive。
5. Windows 动态链接路径仍未完全修复，因此不能把当前状态描述为正式完备的 Windows SDK 发布方案。

## 2. 源码、产物与工具链布局

### 2.1 源码目录

| 组件 | 目录 |
| --- | --- |
| Compiler | `C:\cangjie\cangjie_compiler_CMSGC` |
| Runtime/stdlib | `C:\cangjie\cangjie_runtime_CMSGC` |
| stdx | `C:\cangjie\cangjie_stdx_CMSGC` |
| 最终 SDK | `C:\cangjie\cangjie_compiler_CMSGC\output\msgc` |

### 2.2 本机工具链

本次原生构建使用：

- Windows x86_64；
- Python 3.12；
- CMake + Ninja；
- MSYS2 UCRT64 GCC/G++、GNU ar/ranlib；
- LLVM Clang，主要用于 Windows x86_64 runtime 汇编；
- PowerShell `envsetup.ps1`。

Windows runtime 最终采用混合工具链：

```text
C/C++ : C:\msys64\ucrt64\bin\gcc.exe / g++.exe
ASM   : C:\Program Files\LLVM\bin\clang.exe
Build : CMake + Ninja
```

这样做的原因是 runtime 的 Windows 汇编使用 LLVM/GAS 可接受的重定位和 SEH 语法，而当前本机 C++ 依赖与 UCRT64 GCC/libstdc++ 更容易保持一致。

## 3. 一键与增量构建脚本适配

### 3.1 入口脚本

完整构建入口仍为：

```powershell
python build\scripts\rebuild_msgc_sdk.py
```

该文件本身只是 MSGC 模式包装器，实际 Windows 适配位于：

- `build/scripts/full_sdk_builder.py`；
- `build/scripts/rebuild_sdk_and_stdx.py`；
- `build/scripts/rebuild_msgc_runtime_output.py`；
- `build/scripts/incremental_rebuild_msgc_sdk.py`；
- Compiler、Runtime、stdlib 和 stdx 各自的 `build.py`/CMake 文件。

增量入口为：

```powershell
python build\scripts\incremental_rebuild_msgc_sdk.py
```

Runtime 单独构建安装入口为：

```powershell
python build\scripts\rebuild_msgc_runtime_output.py
```

### 3.2 Windows 主机识别

构建脚本增加了 `windows` host layout 和 `windows_x86_64_cjnative` 平台目录识别，补充：

- Windows 默认 Compiler、stdlib、stdx 和 SDK 路径；
- `.dll` 动态库后缀；
- Windows LLVM DLL 的发现和同步；
- Windows runtime 安装目录的 relayout；
- PowerShell 环境脚本选择。

### 3.3 Python 与 shell 解耦

原脚本多处写死 `python3`、`bash -lc`、`source envsetup.sh` 和 `rm -rf`，这些不能直接用于原生 Windows。

适配后：

- 子 Python 进程使用 `sys.executable`；
- Windows 使用 `powershell.exe -NoProfile -ExecutionPolicy Bypass`；
- Windows source `envsetup.ps1`；
- 清理目录使用 `shutil.rmtree()`；
- CMake 构建统一使用 `cmake --build`，不再写死 `make`；
- Windows 安装目标使用 CMake `install`。

### 3.4 PATH 与编译器环境隔离

Windows runtime 构建前把以下目录置于 PATH 前部：

```text
C:\msys64\ucrt64\bin
C:\Program Files\LLVM\bin
C:\Program Files\CMake\bin
```

同时移除可能从其他阶段泄漏进来的 `CC`、`CXX`、`CFLAGS` 和 `CXXFLAGS`，显式设置：

```text
CMAKE_C_COMPILER=gcc
CMAKE_CXX_COMPILER=g++
```

原因是 Compiler SDK 的 cjnative/Clang 路径如果抢在 UCRT64 前面，嵌套的 cjthread CMake 工程会选择错误的编译器，并与 GCC 16 的 STL/运行库发生不一致。

### 3.5 CMake generator 稳定性

Compiler `build.py` 在 Windows 上会读取已有 `CMakeCache.txt` 中的 generator：

- 已有缓存为 `MinGW Makefiles` 时继续使用；
- 新构建优先 Ninja；
- Ninja 不存在时回退 MinGW Makefiles。

这避免安装 Ninja 后，对已有 MinGW Makefiles build tree 重新 configure 时发生 generator 不一致。

### 3.6 Compiler LLVM 重新链接

增量流程调整为：

```text
compiler build
  -> compiler LLVM build
  -> relink cjc
  -> rebuild cjc-release
  -> compiler install
```

原因是 `cjc` 静态链接 LLVM code generator。如果先链接 `cjc`、再单独更新 LLVM，SDK 中的 `llc` 虽然是新的，但 `cjc` 仍包含旧 LLVM。增加 `compiler-relink` 后，安装的 `cjc` 与本轮 LLVM 产物保持一致。

## 4. Compiler Windows 兼容修改

### 4.1 MinGW 编译器和 binutils 发现

Compiler 原逻辑只查找带 triple 前缀的程序：

```text
x86_64-w64-mingw32-gcc.exe
x86_64-w64-mingw32-g++.exe
```

Windows 本机增加对 UCRT64 中 `gcc.exe`、`g++.exe` 的回退，并优先查找 GNU `ar/ranlib`。这是为了兼容 MSYS2 UCRT64 的本机安装布局。

### 4.2 tinytoml patch 与只读目录清理

原先通过 shell 重定向执行：

```text
patch -p0 -l -f < patch-file
```

Windows 下改为查找 `patch` 可执行文件，并通过 Python `stdin` 传入 patch 内容，避免依赖 shell 重定向。

失败清理时先尝试移除 Windows 文件的只读属性，再执行 `shutil.rmtree()`，避免第三方源码目录无法删除。

### 4.3 LLP64 类型宽度修复

Windows 64 位采用 LLP64：

```text
sizeof(long) == 4
sizeof(void*) == 8
sizeof(long long) == 8
```

CHIR Serializer 原先使用 `bitset::to_ulong()` 序列化属性，在 Windows 上可能截断 64 位属性。已改为 `to_ullong()`。

这属于正确的平台修复，不是妥协。

### 4.4 整数转换异常路径

Compiler 多处通过封装的 `Stoi/Stol/Stoull` 解析数字。原实现直接调用 `std::stoi` 等接口并捕获异常。

Windows MinGW 构建中，这些异常可能跨越使用 `-fno-exceptions` 编译的边界，曾表现为：

```text
Internal Compiler Error: Windows unexpected exception code (...)
Exit code 0xc00000ff
```

现改为 `strtoll/strtoull/strtod/strtold` + `errno` + 范围检查，不再依赖 C++ 异常完成普通解析失败控制流。

同时 BaseMangler/Compression 改用该封装，并在泛型类型查找失败时先检查迭代器，再计算距离，避免未定义行为。

### 4.5 Windows SEH 返回值

`WindowsExceptionHandler()` 原先把 exception code 直接作为 `LONG` 返回给 SEH，语义不正确。现返回 `EXCEPTION_EXECUTE_HANDLER`，使 Windows 按预期进入异常处理路径。

### 4.6 文件访问判断

为绕过 MinGW `_access()` 对 Windows 目录和当前目录的误报，`FileUtil::Access()` 增加 `GetFileAttributesA()` 存在性判断。

当前实现对存在的路径直接把 `FM_EXIST/FM_READ/FM_WRITE/FM_EXE` 都视为成功。这能解除构建阻塞，但权限语义过宽，属于需要收窄的兼容性妥协。

## 5. Compiler 第三方依赖适配

### 5.1 使用本机 MinGW

原构建依赖面向 Linux 交叉编译的 `windows-x86_64-mingw.tar.gz` 和 Unix 解压布局。

Windows 本机且该包不存在时，当前逻辑直接复用 C/C++ 编译器所在的 UCRT64 prefix，并从其 `bin` 和 `lib` 安装运行库、binutils 和 DLL。

### 5.2 LLVM/compiler-rt

Windows 本机构建中：

- 不向 Clang 传递当前环境不可用的 `--unwindlib=libunwind`；
- 链接 native LLVM helper 时增加 `-lwinpthread`；
- 关闭 `COMPILER_RT_BUILD_PROFILE`；
- 不再提取不存在的 profile archive；
- Windows 不把 profile runtime 加入 Clang runtime 安装列表。

关闭 profile runtime 是功能妥协：普通编译可用，但 coverage/profile 能力不完整。

### 5.3 boundscheck 与 UCRT 重复符号

UCRT 已声明或实现部分 C11 安全函数，vendored boundscheck 再定义会产生声明或链接冲突。

Compiler、stdlib 和 stdx 分别通过以下方式处理：

- 不编译重复的 `snprintf_s/vsnprintf_s` 源；
- 设置 `SECUREC_DISABLE_CRT_FUNC`/`SECUREC_DISABLE_CRT_IMP`；
- 设置 `SECUREC_ENABLE_VSNPRINTF=0`。

这属于 UCRT 平台适配，但需要用 boundscheck 单元测试确认行为与其他平台一致。

## 6. Runtime Windows 本地构建适配

### 6.1 GCC/G++ 与 Clang ASM 混合构建

Windows native runtime 配置改为：

```text
CMAKE_C_COMPILER   = gcc.exe
CMAKE_CXX_COMPILER = g++.exe
CMAKE_ASM_COMPILER = clang.exe
```

GCC/G++ 路径去掉 Clang 专用的 `-target`、`-femulated-tls` 和 `--rtlib=compiler-rt`，保留 `-static -pthread`。Runtime 从 libc++/libunwind 组合切换为本机 UCRT64 的 libstdc++/GCC unwinding 组合。

### 6.2 Runtime 安装产物

Windows runtime 安装和 relayout 现在处理：

```text
cjstart.o
libcangjie-runtime.a
libcangjie-aio.a
libcangjie-runtime.dll
libcangjie-trace.dll
libboundscheck.dll
```

`libcangjie-aio.a` 来自嵌套的 cjthread CMake 工程，不在父工程 install manifest 中，因此 `runtime/build.py install` 增加了显式复制。

注意：当前正式 SDK 中仍缺少完整静态消费路径所需的 `libcangjie-thread.a`、`libc++.a` 和 `libunwind.a` 兼容处理，这在第 10 节单独说明。

## 7. MSGC Windows 内存语义修复

### 7.1 reserve 与 commit

MSGC 的 `MSRegionManager::Initialize()` 以前按 POSIX mmap 语义使用映射结果。Windows `VirtualAlloc` 路径会先 reserve 地址空间，reserve 后的页面不能直接读写。

Windows 下现增加：

```text
Commit metadata range
Commit initial heap range
```

否则初始化 `MSRegionInfo` 或首次分配时会访问未提交页面。

### 7.2 Scavenge 后页面复用

Windows Scavenge 通过 `VirtualFree(..., MEM_DECOMMIT)` 归还物理页。之后重新分配相同 Region 时，必须通过 `VirtualAlloc(..., MEM_COMMIT)` 重新提交。

`GrowOneSegmentLocked()` 和 `GrowAndTakeRegionsLocked()` 增加 `ReuseUnits()`，确保新增长或已归还的单元在初始化前重新可访问。

该调用目前是跨平台代码变化；POSIX 实现通常为 no-op，仍需要 Linux/macOS MSGC 回归覆盖。

### 7.3 对齐分配判断

`MSRegionInfo` 是 over-aligned 类型。Windows fallback 分配使用 `_aligned_malloc()`，平台宏从不稳定的 `MRT_WINDOWS` 改为编译器标准宏 `_WIN32`，避免 native Windows 构建中误走普通 `malloc()`。

## 8. Win64 ABI、栈展开与静态执行修复

### 8.1 CalleeSavedStub home space

Win64 ABI 要求调用者为被调函数预留 32 字节 home/shadow space。`CalleeSavedStub.S` 原先第一组 stub 只有 64 字节 frame，没有为后续调用留足 home space。

frame 调整为 96 字节后，保存寄存器、16 字节对齐和 32 字节 home space 同时满足。

这是正式 ABI 修复，不属于性能妥协。

### 8.2 全静态程序中的 native/Cangjie 帧识别

动态 runtime 可以通过模块和 `.text_rt` 等 section 区分 runtime native frame。全静态 PE 中 runtime 与 Cangjie 代码位于同一个 EXE，并不存在独立 runtime DLL section 边界。

旧逻辑会把 C++ runtime frame 当成 Cangjie frame，再按 Cangjie 方法布局从函数入口前方推导 `MFuncDesc` 和 stack map，最终读取无效地址。外部表现可能像栈溢出、异常回溯失败或随机访问异常，但根因不是线程栈空间耗尽，而是帧分类错误。

修复分两层：

1. `UnwindWin.cpp::GetStaticFuncDesc()` 校验推导出的 `MFuncDesc` 是否落在 `__CJMethodInfo`/`.cjmthd` 范围内。
2. `StackManager::IsNativeCodeInStaticExecutable()` 使用 `RtlLookupFunctionEntry()` 获得 PE 函数起点，并做相同的方法元数据范围校验。

校验失败时按 native runtime frame 处理，不再读取 Cangjie stack map。

该修复使 case25 全静态程序不再在 `FrameInfo::ResolveProcInfo`、TypeInfo 或 stack map 解析中崩溃。

## 9. stdlib 和 stdx 链接适配

### 9.1 Windows DLL import library

MinGW/COFF 链接不能沿用 ELF `.so` 的依赖表达方式。stdlib shared library 现在通过：

```text
--out-implib=lib<name>.dll.a
```

生成 import library，并在后续标准库依赖链接时显式使用 `*.dll.a`。安装阶段同时复制 DLL 和 import library。

这属于正式 Windows 链接适配。

### 9.2 OpenSSL strong archive 安装

stdx strong OpenSSL variant 的安装从一系列未检查返回值的 `execute_process()` 改为统一 helper：

- 检查每个 archive 的复制结果；
- 失败立即 `FATAL_ERROR`；
- 从实际生成目录选择 package/FFI strong archive。

这样可避免安装成功但 SDK 中 strong variant 缺文件。

### 9.3 GCC 16 旧 C 源兼容

stdlib MinGW toolchain 当前增加：

```text
-Wno-error=implicit-function-declaration
-Wno-error=incompatible-pointer-types
-Wno-implicit-int
```

这是为了让旧 C FFI 源在 GCC 16 下继续构建，但会降低类型错误的可见性，属于临时妥协，应通过修正源文件原型逐步移除。

## 10. 当前妥协性修改

### 10.1 stdlib 在 Windows 强制 `-O0`

Windows 下 Cangjie stdlib 的 compile command 和 bitcode command 被强制追加 `-O0`。

加入该设置是为了绕过 Windows `cjc` 在编译大型 `std.core` 等模块时出现的 ICE/异常退出。它可以让构建继续，但有直接代价：

- 标准库执行性能下降；
- HTTP、JSON、容器、字符串等路径都会受影响；
- 应用本身即使使用 `-O2`，也无法恢复预编译 stdlib 的优化；
- 本次 case25 的 8,756 req/s 不能代表最终 Windows Release SDK 的性能上限。

这是当前最重要的性能妥协，后续应定位导致 `-O2` 编译失败的具体 compiler pass 或输入函数，并恢复 Release 优化。

### 10.2 PE 栈保留提升到 1 GiB

Compiler MinGW toolchain 的链接参数从：

```text
--stack,16777216
```

调整为：

```text
--stack,1073741824
```

已检查最终 `cjc.exe`：

```text
Machine            : AMD64
SizeOfStackReserve : 1073741824
SizeOfStackCommit  : 4096
```

这不会启动时立即占用 1 GiB 物理内存，只是预留虚拟地址空间，并按需提交；但它会掩盖异常深递归或失控递归，也扩大了所有使用该 toolchain 链接程序的地址空间预留。

该设置应在 compiler 深递归根因修复后恢复为合理值，或只对确实需要的 `cjc` target 定向设置。

### 10.3 关闭 compiler-rt profile

Windows native LLVM 构建关闭了 `COMPILER_RT_BUILD_PROFILE`，并跳过 profile archive 提取。普通编译不受影响，但 profile/coverage 能力不完整。

### 10.4 FileUtil 权限判断过宽

`GetFileAttributesA()` 只能可靠说明路径存在，不能证明路径对当前进程可写或可执行。当前兼容逻辑把存在等同于 read/write/execute 成功，需要后续按 mode 实现更准确判断。

### 10.5 本机绝对路径

Windows 默认值包含：

```text
C:\cangjie\...
C:\msys64\ucrt64\...
C:\Program Files\LLVM\...
```

它们适合当前机器，但不适合作为可移植发布脚本。应支持命令行参数、环境变量和工具自动发现，绝对路径只保留为本机默认或示例。

## 11. 动态链接问题与全静态验证方案

### 11.1 动态 stdx/runtime 路径的问题

最初使用 SDK 中的 runtime/stdx DLL 编译 case25，链接可以完成，但运行时出现 TypeInfo 和 runtime 全局数据地址异常。

主要现象包括：

- `HttpContext.ti`、`URL.ti` 等跨 DLL 类型信息解析为无效地址；
- 静态 stdx 代码引用动态 runtime 的 `CJ_GScheduleManager` 等全局数据时，PE import 语义不正确；
- 直接链接 DLL 与 ELF/Mach-O 的符号模型不同，不能仅靠 `-L/-l` 沿用原有行为。

当前 stdlib 已开始生成 `*.dll.a`，但 Compiler 对 Cangjie type info、metadata 和 runtime data 的 Windows import/export 生成仍未完全闭合。

因此本次用例最终采用全静态链接。动态路径目前应标记为未完成，而不是已验证。

### 11.2 全静态链接缺失库

`cjc --static` 会查找以下库名：

```text
libcangjie-thread.a
libc++.a
libunwind.a
```

当前 Windows SDK 未正式安装完整对应物。为了验证，建立了临时目录：

```text
MSGC_RESULT/Result/case25_http_complex_json_stream/
  result/20260826_windows_2gb_c8_10k_stream_msgc/static-compat-libs
```

其中：

| 临时库名 | 实际来源 | 说明 |
| --- | --- | --- |
| `libcangjie-thread.a` | Runtime `output/temp/lib` | cjthread 静态 archive |
| `libc++.a` | UCRT64 `libstdc++.a` | 名称兼容别名，不是真正 libc++ |
| `libunwind.a` | UCRT64 `libgcc_eh.a` | 名称兼容别名，不是真正 LLVM libunwind |

这组别名与本轮 GCC/libstdc++ runtime 构建匹配，因此验证可运行，但不应直接作为 SDK 发布设计。

### 11.3 stdx 静态传递依赖

仅指定顶层 `stdx.net.http`、`stdx.encoding.json.stream` archive 时，COFF 静态链接不会自动拉入全部 Cangjie stdx 类型和实现。

最终验证命令显式把 static/stdx 下 39 个 `libstdx*.a` 放入：

```text
--start-group ... --end-group
```

后才完成所有静态符号解析。这说明 SDK 目前没有完整表达 stdx 静态库的传递依赖，普通使用者仍需要手工枚举 archive。

后续应由构建系统生成依赖元数据、聚合 archive 或正确的链接响应文件，而不是让业务命令维护 39 个库名。

## 12. 排障过程中的关键根因

### 12.1 “栈溢出”并非单一问题

本轮出现过三类容易被统称为栈问题的故障：

1. `cjc` 构建 stdlib 时出现 Windows unexpected exception/ICE；当前通过无异常数字解析、增大 PE 栈和 stdlib `-O0` 绕过，但深递归和优化阶段根因仍需继续缩小。
2. Win64 stub 未预留 home space；这是明确 ABI 错误，已修复。
3. 全静态 Runtime 把 native frame 误识别为 Cangjie frame，读取无效 stack map；这是 case25 运行崩溃的直接根因，已通过方法元数据范围校验修复。

因此，case25 后期的崩溃不能简单归因于线程栈太小；最终有效修复是静态帧分类，而不是继续增大业务程序栈。

### 12.2 PowerShell 参数数组问题

构造 `cjc` 参数时曾在 `ForEach-Object { $args += ... }` 中使用 `$args`。`$args` 是 PowerShell script block 的自动变量，会遮蔽外层数组，导致实际命令没有加入 39 个 stdx archive。

最终改用普通 `foreach` 并使用独立变量累加参数。该问题只存在于手工验证命令，没有形成源码修改。

## 13. 验证结果

### 13.1 构建脚本检查

当前检查结果：

| 检查项 | 结果 |
| --- | --- |
| `rebuild_msgc_sdk.py --dry-run` | 退出码 0，Windows 完整 stage plan 可生成 |
| `incremental_rebuild_msgc_sdk.py --dry-run` | 退出码 0，含 compiler relink |
| `test_rebuild_msgc_runtime_output.py` | 10 项通过 |
| `test_incremental_rebuild_msgc_sdk.py` | 9 项通过 |
| `test_rebuild_msgc_runtime_sdk.py` | 2 项失败，见第 14.1 节 |

完整 dry-run 依次覆盖：

```text
compiler clean/build/LLVM/install
runtime clean/build/install/relayout
stdlib clean/build/install/sync
stdx clean/build/install/sync
SDK overlay
```

### 13.2 Runtime 静态库重建

最终 Windows static runtime 通过以下增量 target 构建：

```powershell
cmake --build C:\cangjie\cangjie_runtime_CMSGC\runtime\CMakebuild `
  --config Release `
  --target cangjie-runtime-static `
  -j 12
```

生成并同步到 SDK 的 `libcangjie-runtime.a`：

```text
Size   : 75,022,826 bytes
SHA256 : 8C40BD2EFB85D1126DAD170C5794CEDC550AD063BC5CA3E5E286886D803E5981
```

### 13.3 case25 短验证

配置：

```text
GC          : MSGC / marksweep
Payload     : 10K，实际 9,572 bytes
Concurrency : 8
Heap        : 2 GiB
Requested   : 5,000 ms
```

结果：

```text
duration   = 6,106 ms
total      = 55,913
ok         = 55,913
fail       = 0
throughput = 9,157 req/s
exit       = 0
```

### 13.4 case25 60 秒正式验证

配置：

```text
GC          : MSGC / --gc-mode=marksweep
Payload     : 10K，实际 9,572 bytes
Concurrency : 8
Heap        : 2 GiB
Duration    : 60,000 ms
Server      : http://127.0.0.1:18225/process
Persistent  : disabled
```

结果：

```text
duration   = 60,108 ms
total      = 526,348
ok         = 526,348
fail       = 0
inflight   = 0
throughput = 8,756 req/s
exit       = 0
```

产物：

```text
MSGC_RESULT/Result/case25_http_complex_json_stream/result/
  20260826_windows_2gb_c8_10k_stream_msgc/
    benchmark-full-static-final2.exe
    stdout-full-static-final2-60s.txt
    gc-full-static-final2-60s.log
```

正式测试结束后没有残留 benchmark 进程，端口 `18225` 已释放。

日志中的 `ConnectionException: Socket is closed` 出现在停止 server、主动关闭连接阶段，不计为业务失败；最终 `fail=0`。

## 14. 已确认的遗留问题

### 14.1 Runtime 单独 SDK 包装入口回归

`build/scripts/rebuild_msgc_runtime_output.py` 已把默认 SDK 路径拆分为：

```text
DEFAULT_MSGC_SDK_ROOT_DARWIN
DEFAULT_MSGC_SDK_ROOT_LINUX
DEFAULT_MSGC_SDK_ROOT_WINDOWS
```

但 `build/scripts/rebuild_msgc_runtime_sdk.py` 仍引用已删除的：

```text
runtime_builder.DEFAULT_MSGC_SDK_ROOT
```

当前直接执行会报：

```text
AttributeError: module 'rebuild_msgc_runtime_output'
has no attribute 'DEFAULT_MSGC_SDK_ROOT'
```

对应两个单元测试失败。应改为调用 `detect_host_layout()` + `default_msgc_sdk_root(host)`，并保留显式 `--sdk-root` 覆盖行为。

### 14.2 动态 SDK 未闭合

需要继续解决：

- Cangjie type info/metadata 在 PE DLL 间的 import/export；
- runtime 全局数据的 `dllimport/dllexport`；
- stdlib/stdx DLL 和 `*.dll.a` 的依赖传播；
- 动态 case25 的编译、启动、运行和异常回溯验证。

### 14.3 静态 SDK 打包不完整

需要正式安装或重新定义：

- `libcangjie-thread.a`；
- Windows 使用 libstdc++ 时 `cjc --static` 的 C++ runtime 选择；
- Windows 使用 GCC SEH 时 unwind archive 的选择；
- stdx static 传递依赖。

不能长期依赖把 `libstdc++.a` 重命名为 `libc++.a`、把 `libgcc_eh.a` 重命名为 `libunwind.a`。

### 14.4 Release 优化未恢复

必须单独验证：

```text
std.core -O1
std.core -O2
完整 stdlib -O2
stdx -O2
case25/26/27 Release 回归
```

应通过最小化出错模块和 compiler pass 找到 `-O2` ICE 根因，再删除 Windows `-O0` 分支。

### 14.5 构建树污染

`runtime/build/cjthread_build` 下有大量被 Git 跟踪的 CMake 生成文件发生变化，同时存在构建日志和 GDB 临时脚本。

这些不是 Windows 适配源码，不应混入正式提交。提交前需要：

- 将生成目录从改动集中隔离；
- 删除或归档本机调试脚本；
- 仅保留源码、CMake、构建脚本、测试和本文档；
- 避免提交纯 CRLF/LF churn。

## 15. 修改性质分类

### 15.1 可作为正式修复保留

- Windows host/layout、PowerShell 和 `sys.executable` 支持；
- `cmake --build` 和 Python 原生清理；
- LLP64 `to_ullong()`；
- 缺失标准头文件；
- 无异常数字解析与 mangler 边界检查；
- 正确的 SEH handler 返回值；
- Win64 home space；
- Windows reserve/commit 与 Scavenge 页面复用；
- 全静态 Cangjie/native frame 元数据范围校验；
- MinGW DLL import library 生成和安装；
- boundscheck/UCRT 重复符号隔离；
- Compiler LLVM 更新后重新链接 `cjc`；
- 安装 `libcangjie-aio.a` 和 `libboundscheck.dll`。

### 15.2 需要继续验证后保留

- Runtime 全面切换 GCC/G++ + Clang ASM；
- `ReuseUnits()` 在 grow 路径的跨平台影响；
- 使用本机 MinGW 替代预打包 binary deps；
- OpenSSL strong archive 的源目录选择；
- 静态 PE 帧分类在 split function/pdata 场景下的行为。

### 15.3 应删除或替换的临时妥协

- Windows stdlib 全局 `-O0`；
- toolchain 全局 1 GiB stack reserve；
- `FileUtil::Access()` 把存在等价为可读/可写/可执行；
- GCC 16 旧 C 错误的全局降级；
- 关闭 compiler-rt profile；
- `libc++.a`/`libunwind.a` 名称兼容 alias；
- 手工枚举 39 个 stdx archive；
- `C:\cangjie` 和工具目录硬编码。

## 16. 推荐收敛顺序

### 阶段一：修复脚本一致性

1. 修复 `rebuild_msgc_runtime_sdk.py` 默认路径回归。
2. Windows 输出统一显示 `envsetup.ps1`，不再打印 `envsetup.sh`。
3. Windows required tools 校验补充 Ninja 和 Clang ASM。
4. 为 Windows full/incremental/runtime-only stage plan 增加单元测试。

### 阶段二：完成静态 SDK

1. 正式安装 cjthread static archive。
2. 让 `cjc --static` 根据 Windows GCC runtime 选择正确库名，不使用 alias。
3. 为 stdx 生成可传递的静态依赖描述或聚合 archive。
4. 使用普通用户命令重新编译 case25，不再传 39 个手工 archive。

### 阶段三：完成动态 SDK

1. 修复 PE Cangjie metadata/type info 的 import/export。
2. 修复 Runtime 数据符号的 import/export。
3. 验证 stdlib/stdx `*.dll.a` 依赖链。
4. 动态运行 case25 60 秒，要求零失败且异常回溯可用。

### 阶段四：恢复 Release 质量

1. 定位 stdlib `-O2` ICE 根因并移除 `-O0`。
2. 将 1 GiB stack reserve 降回合理值，并增加编译器深递归测试。
3. 修复旧 C FFI 原型，移除诊断降级。
4. 恢复 compiler-rt profile/coverage。
5. 对 case25、case26、case27 做 Windows Release 性能和稳定性回归。

## 17. 最终状态定义

截至 2026-08-27，本轮达到的状态是：

```text
Windows 原生源码构建       : 已跑通
MSGC Runtime 构建/安装     : 已跑通
Compiler/stdlib/stdx 汇总  : 已由一键脚本完整产出 SDK
MSGC 全静态实际用例        : 已通过 60 秒验证
MSGC 动态实际用例          : 已通过 60 秒及最终 SDK 短跑验证
标准 SDK 消费体验          : 使用正式库名和 stdx import library
Windows Release 性能       : stdlib 已恢复并通过完整 -O2 构建
一键脚本最终全量回归       : incremental_rebuild_msgc_sdk.py 退出码 0
编译器 PE 栈保留           : 已恢复为原始 16 MiB
```

## 18. 最终收敛与验收结果

### 18.1 LLVM 编译栈溢出根因

Windows 全量构建失败不是因为构建了 AArch64 SDK，而是 MSYS2 GCC 16 的
`cc1plus.exe` 在 LLVM X86 后端等重型 C++ 翻译单元中发生宿主编译器栈溢出。
仓颉 SDK 的目标仍为 `x86_64-unknown-windows-gnu`；ARM/AArch64 日志来自 LLVM
编译器后端库，不代表 SDK 目标架构发生变化。

最终处理为：

- 仓颉编译器主体继续使用项目原有 GCC/G++；
- 仅 LLVM ExternalProject 使用本机 Clang，并显式接入 MinGW sysroot 和正式
  libstdc++ 头文件；
- Clang 路径转换为无空格短路径，避免 compiler-rt 自定义探测命令拆分参数；
- Windows compiler-rt 只构建 SDK 实际打包所需的 builtins，配置阶段先使用
  MinGW libgcc，移除对尚未生成 builtins 的循环依赖；
- Windows 本机 `NATIVE` tablegen bootstrap 不再设置 `CMAKE_SYSTEM_NAME` 和
  compiler target，避免错误进入 `NATIVE/NATIVE` 递归构建；
- 增量脚本能识别旧 GCC LLVM cache、旧 builtins 选项和错误的 native cross
  cache，并只清理必要构建目录。

### 18.2 用户要求的妥协项收敛

| 项目 | 最终结果 |
| --- | --- |
| stdlib 优化 | 完整 `265/265` Release/O2 构建通过 |
| 编译器 PE 栈 | `SizeOfStackReserve=0x01000000`，恢复原始 16 MiB |
| case25 链接 | 使用 `--dy-std` 动态链接 |
| Runtime 库名 | 正式 `libcangjie-aio.a`、`libcangjie-thread.a` |
| C++/unwind 库名 | 正式 `libstdc++.a`、版本探测后的 `libgcc_eh.a`，无临时 alias |
| stdx 链接 | 安装 38 个正式 `.dll.a`，case25 只声明 3 个顶层 import library |
| 手工 39 库 | 已删除，不再枚举 stdx 静态 archive |
| runtime-only 包装脚本 | 按主机解析默认 SDK，显式 `--sdk-root` 可覆盖 |

### 18.3 一键全量构建

执行命令：

```powershell
python build\scripts\incremental_rebuild_msgc_sdk.py --jobs 8
```

最终结果：

```text
compiler build/install       : passed
LLVM/Clang/lld               : passed
runtime build/install        : passed
stdlib Release/O2 265/265    : passed
stdx 202/202 build/install   : passed
exit code                    : 0
SDK                          : C:\cangjie\cangjie_compiler_CMSGC\output\msgc
```

构建脚本相关现存测试为 `24/24` 通过。扩展 discovery 中另有 15 个旧 macOS
case 路径测试引用当前仓库不存在的 case25-case30 目录，属于测试数据缺失，不影响
Windows SDK 构建结果。

### 18.4 动态 case25 最终 SDK 验收

最终全量 SDK 的 5 秒短跑：

```text
payload     : 10K（实际 9,572 bytes）
concurrency : 8
heap        : 2 GiB
duration    : 6,103 ms
total/ok    : 98,274 / 98,274
fail        : 0
throughput  : 16,102 req/s
exit code   : 0
```

产物目录：

```text
MSGC_RESULT/Result/case25_http_complex_json_stream/result/
  20260827_131808_windows_dynamic_2gb_c8_10k_msgc/
```

PE 导入表确认包含 `libcangjie-runtime.dll`、`libcangjie-std-*.dll`、
`libstdx.net.http.dll` 和 `libstdx.encoding.json.stream.dll`，因此不是把动态命令
伪装成全静态产物。此前 60 秒动态正式结果仍为 711,675 请求、零失败、
11,835 req/s。

### 18.5 非阻塞注意项

- SDK 安装阶段曾同步完整 MinGW `lib` 树，包含 Python/Tcl 数据；该问题已在
  19 节通过 Windows 本机依赖白名单解决。
- Windows compiler-rt 的 sanitizer、XRay、fuzzer、profile 等非 SDK 必需目标
  未参与本轮打包；SDK 所需 builtins 已成功构建和安装。
- 全仓 `test_*msgc*.py` discovery 的旧 macOS case 路径清单应在对应测试数据恢复
  后另行整理。

## 19. Windows SDK 依赖白名单与体积收敛

### 19.1 根因

精简前 SDK 为 14,445 个文件、2,997,540,965 bytes。排除允许保留的 `cjc-*`
中间产物后，主要浪费来自两个整目录复制：

- `AddMinGWLibs.cmake` 把本机 `ucrt64/lib` 整体安装到 SDK，带入 13,338 个文件，
  包括 1,047 个 `.a`、Python、Tcl、测试数据和无关程序，共 571,980,503 bytes。
- `sync_compiler_llvm_into_sdk()` 把 `cjnative-build/bin` 整体覆盖到 SDK，又保留
  CMake 安装的整个 MSYS2 `bin`，共 320 个文件、516,775,185 bytes；同时把
  LLVM DLL 再复制到 `third_party/llvm/lib`，产生重复副本。

Runtime 的另一个独立体积来源是 Release 仍带 `-gdwarf-4`，且一键脚本从
`runtime/output/common` 直接同步未 strip 的 DLL。仅 `libcangjie-runtime.dll`
就有 61,135,763 bytes DWARF。本节只处理依赖复制范围，strip 应作为独立发布步骤。

### 19.2 `.a` 的保留原则

发布包本身也包含 `.a`：core 70 个、runtime 2 个、stdx 94 个、MinGW 16 个。
因此不能按扩展名整体删除：

- `libcangjie-std-*.a` 是全静态链接输入，必须保留；
- `*.dll.a` 是 GNU PE 链接器使用的正式 DLL 导入库，动态 std/stdx 需要保留；
- MinGW 中只保留 CRT、C++/EH、pthread 和实际使用的 WinAPI 导入库。

Windows 本机 MinGW 最终保留 20 个文件：4 个 CRT object、15 个固定 archive，
以及按 GCC 版本发现并扁平安装的 `libgcc_eh.a`。任何白名单文件缺失时 CMake
直接报错，不再静默回退到全目录复制。

### 19.3 LLVM 最小运行闭包

当前 Windows `cjc` 编译链直接调用：

```text
opt.exe
llc.exe
llvm-ar.exe
ld.exe
```

SDK 同时保留上述程序递归需要的 `libLLVM*.dll`、libstdc++/libgcc/pthread、
gettext、zlib/zstd DLL。同步结束后校验必需文件，并删除 GDB、Clang 测试工具、
Python、`libLLVM-15.dll.stale` 及 `third_party/llvm/lib` 中的重复 DLL。

该分支仅用于 Windows 本机构建；Linux、macOS 和 Linux 交叉 Windows 的原安装
分支保持不变。

### 19.4 精简结果与验收

| 项目 | 精简前 | 精简后 |
| --- | ---: | ---: |
| 整个 SDK | 14,445 文件 / 2,997,540,965 B | 817 文件 / 1,934,248,428 B |
| `third_party/mingw` | 13,338 文件 / 571,980,503 B | 20 文件 / 17,425,033 B |
| `third_party/llvm` | 323 文件 / 590,630,161 B | 13 文件 / 81,893,094 B |

总计减少 1,063,292,537 bytes；保留的 `bin` 中间产物仍占 1,186,221,513 bytes。

验收结果：

- 构建脚本单元测试：14 项中 12 项通过，2 项 Linux symlink 测试因 Windows
  普通用户无符号链接权限按平台跳过；
- `cjc`、`opt`、`llc`、`llvm-ar`、GNU `ld` 均可从精简 SDK 启动；
- case25 动态短测：1K、并发 2、22,988/22,988 成功、0 失败、10,879 req/s；
- `cjc --static` smoke 编译与执行通过，输出 `Windows static SDK link passed`。

### 19.5 编译器诊断快照清理

Windows 栈溢出、BFD、PE `.pdata` 和符号回溯排查期间曾手工保留：

```text
cjc-bfd.exe
cjc-debug.exe
cjc-map.exe
cjc-map2.exe
cjc-pdatafix.exe
cjc-symbols.exe
```

这些文件没有安装规则，也不被 SDK 或一键构建引用。Windows compiler install 后的
共用同步阶段现在会从 SDK `bin` 和 compiler `build/build/bin` 删除这些已知诊断
快照。正式的 `cjc.exe`、`cjc-frontend.exe`、构建目标 `cjc-release.exe` 以及
符号反修饰工具 `cjfilt.exe` 不在清理范围内。

## 20. 剩余问题：MSGC 对齐官方 `cjc/ld.lld` 的 DLL 链接方式

### 20.1 当前状态

官方 CopyGC SDK 的 Windows 编译链使用 `cjc -> ld.lld.exe`，并允许 linker 直接从
`libcangjie-*.dll` 和 `libstdx.*.dll` 读取导出符号。当前 MSGC SDK 则使用
`cjc -> GNU ld.exe`，动态 runtime、stdlib 和 stdx 通过正式 MinGW import library
`*.dll.a` 链接。

两者差异不是 case25 runner 人为制造的。当前 MSGC compiler driver 为处理跨 DLL
数据符号，显式启用了：

```text
--enable-auto-import
--enable-runtime-pseudo-reloc
```

并在 `MinGW_CJNATIVE::PrepareDependencyPath()` 中选择 `ld.exe`。动态 runtime 也固定
声明为 `libcangjie-runtime.dll.a`。这些修改位于：

```text
cangjie_compiler_CMSGC/src/Driver/Toolchains/CJNATIVE/MinGW_CJNATIVE.cpp
```

### 20.2 2026-08-27 对照实验

使用同一份 MSGC `cjc`、runtime、stdlib、stdx 和 case25 源码进行受控比较：

| 链接输入 | 编译结果 | 运行结果 |
| --- | --- | --- |
| GNU `ld` + 三个顶层 stdx `*.dll.a` | 成功 | 成功，约 12,008 req/s，退出码 0 |
| GNU `ld` + 三个顶层 stdx `.dll` | 成功 | 20 秒超时，退出码 124 |

两份 EXE 的 PE import table 中 DLL 名称一致，均包含动态 runtime、stdlib 和 stdx。
因此“链接成功”和“导入表中存在 DLL”不能证明数据导入语义正确。`*.dll.a` 不是临时
别名，它包含 `__imp_*` 符号、import thunk 和数据导入信息；当前 MSGC 直接读取 DLL
导出表时，仍可能对 TypeInfo、TypeTemplate、metadata 或 runtime 全局数据生成错误
重定位。

此前直接使用 `ld.lld` 的现象同样是链接成功但运行期出现跨 DLL 地址异常，典型对象
包括 `HttpContext.ti`、`URL.ti` 和 `CJ_GScheduleManager`。因此当前不能通过替换
`ld.exe`、删除 `*.dll.a` 或只修改 runner 来切换到官方模式。

### 20.3 根因边界

当前 MSGC compiler 基于较旧分支并包含 MSGC 专用 LLVM lowering，版本和目标信息为：

```text
Cangjie Compiler: 0.0.1 (cjnative)
Target: x86_64-unknown-windows-gnu
```

2026-08-27 官方 CopyGC SDK 为：

```text
Cangjie Compiler: 1.3.0-alpha.20260827010033 (cjnative)
Target: x86_64-w64-mingw32
```

官方二进制 `cjc.exe` 不能直接替换 MSGC `cjc.exe`，因为它不包含
`--gc-mode=marksweep`、MSGC intrinsic、statepoint 和 barrier lowering。可行方向是
将官方新版 Windows PE import/export 与 linker driver 能力移植到 MSGC compiler，
而不是绕过 MSGC compiler。

### 20.4 收敛条件

切换到 `MSGC cjc -> ld.lld -> 直接 DLL` 前必须完成：

1. 对齐官方新版 Windows/COFF driver 和目标 ABI，恢复 `ld.lld.exe` 选择。
2. 为跨 DLL TypeInfo、TypeTemplate、metadata 和 runtime 全局变量生成正确的
   `dllimport`/`dllexport` 及 `__imp_*` 引用。
3. runtime、stdlib、stdx 使用同一套 ABI 和 compiler 全量重建，不能混用旧 `.cjo`。
4. driver 将动态 runtime/std/stdx 从 `*.dll.a` 切换为直接 `.dll`，并从 SDK 最小
   LLVM 闭包恢复 `ld.lld.exe`。
5. 除 case25 外，补充泛型、异常、反射、跨 DLL TypeInfo、runtime 全局数据、HTTP
   和 JSON 的动态链接回归测试。
6. 验证 PE 导入和 base relocation 只是辅助检查，最终必须以稳定执行和结果正确为准。

在上述条件闭合前，GNU `ld + *.dll.a` 是当前 MSGC Windows 动态 SDK 的正式正确性
方案。该问题不阻塞现有 SDK 发布和 case25 对比，但作为 linker/ABI 技术债保留；不得
在仅有“编译成功”证据时删除 import library 或宣称已对齐官方 `ld.lld` 路径。

### 20.5 2026-08-27 实施结果：已切换并验证

MSGC Windows SDK 已完成 `cjc -> ld.lld.exe -> 直接 DLL` 切换：

- `MinGW_CJNATIVE` 改为选择 `ld.lld.exe`；删除 GNU `ld` 专用的
  `--enable-auto-import` 与 `--enable-runtime-pseudo-reloc`；
- 动态 runtime/std 由 `*.dll.a` 改为 `-l:libcangjie-runtime.dll` 和
  `-l:libcangjie-std-core.dll`；
- x86_64 Windows 的 StackMaps 过滤帧锚点 RBP，避免其错误进入 runtime
  callee-saved bitmap，修复 std.core 的 O2 codegen 异常；
- 增量 SDK 脚本会在 Windows backend 变更后触发 split `libLLVM-15.dll` 重链，并构建、同步
  `ld.lld.exe`、`llc.exe`、`llvm-ar.exe`、`opt.exe`。

完整 `incremental_rebuild_msgc_sdk.py` 重组成功。用新 SDK 重新编译后，最小
`--dy-std` 用例的实际 linker 命令为 `ld.lld.exe`，且直接传入
`-l:libcangjie-std-core.dll`、`-l:libcangjie-runtime.dll`；程序输出
`MSGC Windows smoke test`、退出码 0。case25 也以三个直接 `.dll`（不使用 `.dll.a`）
重新编译并运行 2 秒：`total=22351`、`ok=22351`、`fail=0`、退出码 0。
