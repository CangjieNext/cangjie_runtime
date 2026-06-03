# 通过源码一次性构建可用的 Cangjie Compiler（macOS aarch64）

## 目标

这份文档只保留一条已经在本机走通的最短链路，用来在新会话里从源码重新构建一套可用的 Cangjie compiler 包。

这里的“可用”指的是：

- `cangjie_compiler/output/bin/cjc` 可以直接编译 `hello.cj`
- `hello` 可以正常运行，不再缺 `runtime` / `stdlib`
- 如果当前源码里还保留了这轮加的 marker，还能额外看到 compiler / LLVM / stdlib 的验证日志

本文默认：

- 不构建 `cjdb`
- 不跑 compiler tests
- 重新构建 compiler、runtime、stdlib，并把产物汇总到 `cangjie_compiler/output`

## 适用范围

- 平台：macOS arm64
- 验证时间：2026-05-05
- 已验证仓库：
	- `cangjie_compiler`
	- `cangjie_runtime_MSGC/runtime`
	- `cangjie_runtime_MSGC/stdlib`

## 固定路径

以下路径是本次实际验证使用的路径，后续新会话可以直接复用：

```bash
export COMPILER_ROOT=/Users/lipeisheng/code/cangjie/cangjie/cangjie_compiler
export RUNTIME_ROOT=/Users/lipeisheng/code/cangjie/cangjie/cangjie_runtime_MSGC/runtime
export STDLIB_ROOT=/Users/lipeisheng/code/cangjie/cangjie/cangjie_runtime_MSGC/stdlib
export TARGET_SDK=/Users/lipeisheng/code/cangjie/cangjie-pkg/dev/20260421/cangjie

export COMPILER_PY=/Users/lipeisheng/code/cangjie/cangjie/cangjie_compiler/.venv/bin/python
export RUNTIME_PY=/Users/lipeisheng/code/cangjie/cangjie/cangjie_runtime_MSGC/runtime/.venv/bin/python
export STDLIB_PY=/Users/lipeisheng/code/cangjie/cangjie/cangjie_runtime_MSGC/stdlib/.venv/bin/python
```

## 前置依赖

### 1. Homebrew 依赖

最少保证这些工具已经可用：

```bash
brew install cmake ninja llvm@16 m4 bison googletest gnu-tar
```

`swig` 不是这条最短链路的必需依赖，因为这里明确不构建 `cjdb`。

### 2. PATH

建议在新 shell 里先执行：

```bash
export PATH=/opt/homebrew/opt/llvm@16/bin:/opt/homebrew/opt/m4/bin:$PATH
```

### 3. 保底环境变量

这轮成功构建中，下面这些变量建议显式设置，避免新会话环境漂移：

```bash
export ARCH=aarch64
export SDK_NAME=darwin
export CANGJIE_VERSION=0.0.1
```

当前这条最短链路不构建 `stdx`，因此不依赖 `STDX_VERSION`。

## 一次性成功的构建流程

### Step 0：统一初始化

```bash
export PATH=/opt/homebrew/opt/llvm@16/bin:/opt/homebrew/opt/m4/bin:$PATH
export ARCH=aarch64
export SDK_NAME=darwin
export CANGJIE_VERSION=0.0.1

export COMPILER_ROOT=/Users/lipeisheng/code/cangjie/cangjie/cangjie_compiler
export RUNTIME_ROOT=/Users/lipeisheng/code/cangjie/cangjie/cangjie_runtime_MSGC/runtime
export STDLIB_ROOT=/Users/lipeisheng/code/cangjie/cangjie/cangjie_runtime_MSGC/stdlib

export COMPILER_PY=/Users/lipeisheng/code/cangjie/cangjie/cangjie_compiler/.venv/bin/python
export RUNTIME_PY=/Users/lipeisheng/code/cangjie/cangjie/cangjie_runtime_MSGC/runtime/.venv/bin/python
export STDLIB_PY=/Users/lipeisheng/code/cangjie/cangjie/cangjie_runtime_MSGC/stdlib/.venv/bin/python
```

### Step 1：重建 compiler

这是当前实际验证过的最短成功命令。不要带 `--build-cjdb`。

```bash
cd "$COMPILER_ROOT"
"$COMPILER_PY" build.py clean
"$COMPILER_PY" build.py build -t release --no-tests
"$COMPILER_PY" build.py install
```

完成后至少要有这些产物：

```bash
ls "$COMPILER_ROOT/output/bin/cjc"
ls "$COMPILER_ROOT/output/envsetup.sh"
ls "$COMPILER_ROOT/output/third_party/llvm/lib/libLLVM.dylib"
```

### Step 2：重建 runtime

这里要执行两次 install，目的不同：

- 第一次默认 `install`：把产物放到 `runtime/output/common/...`，给 stdlib 的 `--target-lib` 使用
- 第二次 `install --prefix`：把 runtime 直接安装进 `compiler/output`

```bash
cd "$RUNTIME_ROOT"
"$RUNTIME_PY" build.py clean
"$RUNTIME_PY" build.py build
"$RUNTIME_PY" build.py install
"$RUNTIME_PY" build.py install --prefix "$COMPILER_ROOT/output"
```

完成后检查这两组目录：

```bash
ls "$RUNTIME_ROOT/output/common/darwin_release_aarch64/lib/darwin_aarch64_cjnative"
ls "$RUNTIME_ROOT/output/common/darwin_release_aarch64/runtime/lib/darwin_aarch64_cjnative"

ls "$COMPILER_ROOT/output/lib/darwin_aarch64_cjnative"
ls "$COMPILER_ROOT/output/runtime/lib/darwin_aarch64_cjnative"
```

`stdlib` 构建最关键的是这两个文件必须存在：

```bash
ls "$RUNTIME_ROOT/output/common/darwin_release_aarch64/lib/darwin_aarch64_cjnative/libcangjie-aio.a"
ls "$RUNTIME_ROOT/output/common/darwin_release_aarch64/runtime/lib/darwin_aarch64_cjnative/libcangjie-runtime.dylib"
```

### Step 3：重建 stdlib

进入 `stdlib` 之前，必须先 source compiler 的新环境。否则 `stdlib` 配置阶段可能找不到 `cjc`。

为了避免新 shell 里 `PATH` 没带上 `cjc`，建议在执行 `build.py` 前再显式补一遍：

```bash
export PATH="$COMPILER_ROOT/output/bin:$PATH"
```

```bash
source "$COMPILER_ROOT/output/envsetup.sh"
export PATH="$COMPILER_ROOT/output/bin:$PATH"

cd "$STDLIB_ROOT"
"$STDLIB_PY" build.py clean
"$STDLIB_PY" build.py build -t release \
	--target-lib="$RUNTIME_ROOT/output" \
	--build-args=-Woff --build-args=deprecated \
	--build-args=-Woff --build-args=unused
"$STDLIB_PY" build.py install
```

注意：

- `stdlib build.py build` 可以带 compiler 的 `DYLD_LIBRARY_PATH`
- 但 `stdlib build.py install` 不要额外手工带上指向 `compiler/output/third_party/llvm/lib` 的 `DYLD_LIBRARY_PATH`

原因是 install 阶段会调用宿主机工具链里的 `llvm-ar`；如果它误加载了仓内的 `libLLVM.dylib`，会出现符号不匹配报错

```text
dyld: Symbol not found: _LLVMInitializeAMDGPUAsmParser
```

### Step 4：把 stdlib 合并进 compiler/output

```bash
rsync -a "$STDLIB_ROOT/output/" "$COMPILER_ROOT/output/"

find "$COMPILER_ROOT/output/runtime" "$COMPILER_ROOT/output/lib" \
	-type f -name '*.dylib' -exec codesign -f -s - {} \;
```

在 macOS arm64 上，复制 dylib 之后重新签名不是可选项，而是必须项。

### Step 5：最终冒烟验证

如果仓库里已经有 `hello.cj`，直接复用即可。当前路径是：

```bash
ls "$COMPILER_ROOT/hello.cj"
```

验证命令：

```bash
source "$COMPILER_ROOT/output/envsetup.sh"
cd "$COMPILER_ROOT"
rm -f hello_rebuilt
cjc hello.cj -o hello_rebuilt
./hello_rebuilt
```

当前这轮源码保留 marker 时，期望输出类似：

```text
[cj-compiler-rebuilt] exe=/Users/lipeisheng/code/cangjie/cangjie/cangjie_compiler/output/bin/cjc input=hello.cj
[cj-llvm-rebuilt]
[cj-stdlib-rebuilt]
Hello, Cangjie
```

如果后续有人把 marker 改掉了，那么最小成功标准仍然是最后能打印：

```text
Hello, Cangjie
```

## 实际踩坑与规避

### 1. compiler 带 `--build-cjdb` 会失败

现象：

- `Could NOT find SWIG`
- `Could NOT find PythonAndSwig`
- `FAILED: third_party/lldb-prefix/src/lldb-stamp/lldb-configure`

规避：

- 最短链路直接使用：

```bash
"$COMPILER_PY" build.py build -t release --no-tests
```

- 不要再带 `--build-cjdb`

### 2. stdlib 直接构建会报 `NOT FOUND CJC`

根因：

- `stdlib` 依赖 `CANGJIE_HOME` / `cjc`，但裸 shell 下这两个环境不一定存在

规避：

```bash
source "$COMPILER_ROOT/output/envsetup.sh"
```

然后再进入 `stdlib` 执行 `build.py build`。

### 3. stdlib 会被 warning 卡死

现象：

- `array_common.cj` 的 deprecated warning
- `libc.cj` 的 unused variable warning
- 最终 `ninja: build stopped: subcommand failed`

规避：

```bash
--build-args=-Woff --build-args=deprecated \
--build-args=-Woff --build-args=unused
```

### 4. runtime 只 build 不 install，stdlib 会链接失败

现象：

- `ld: library 'cangjie-runtime' not found`
- `search path '.../runtime/lib/darwin_aarch64_cjnative' not found`

根因：

- `stdlib` 不是只看 `runtime/output/temp/...`
- 它需要 `runtime/output/common/darwin_release_aarch64/{lib,runtime/lib}/...` 这两层目录都存在

规避：

- `runtime` build 后必须先跑一次默认 `install`
- 不要跳过：

```bash
"$RUNTIME_PY" build.py install
```

### 5. macOS arm64 上复制 dylib 后必须重新签名

现象：

- 编译能过，但运行二进制会被直接杀掉
- 常见表现是 `killed` / `exit 137` / dyld 提前退出

规避：

```bash
codesign -f -s - /path/to/copied.dylib
```

如果是批量复制，直接对目标目录下的所有 `.dylib` 重新签名。

### 6. 同步到外部 SDK 时必须使用绝对路径

这次实际踩过一次坑：从错误的当前目录执行相对路径 `rsync`，会静默写到错误的目录树里，看起来像“同步成功”，实际目标 SDK 没被替换。

规避：

- `rsync` 的目标路径必须写绝对路径
- 不要依赖当前工作目录猜相对路径

### 7. runtime install 可能出现一条 `strip` 抱怨，但不一定是致命错误

本次见到过类似信息：

```text
strip: can't open file: .../runtime/output/common/darwin_release_aarch64/lib/libcangjie-runtime.a
```

实际结果里 install 仍然完成了，关键 dylib 和静态库都正确落盘。

处理方式：

- 不要只看这一条报错就判定失败
- 先检查 Step 2 里要求的几个关键文件是否存在

### 8. stdlib 构建时，`source envsetup.sh` 之后最好仍显式补一遍 `PATH`

现象：

- `stdlib/build.py build` 一上来就在 `Generating cangjieCJNATIVECore` 失败
- 日志里只看到一条很短的 `no such file or directory`
- 实际失败点是 CMake 子命令里调用 `cjc`，但当前 shell 的 `PATH` 里没有它

规避：

```bash
source "$COMPILER_ROOT/output/envsetup.sh"
export PATH="$COMPILER_ROOT/output/bin:$PATH"
```

然后再执行 `stdlib/build.py build`。

### 9. 如果单独在 LLVM build tree 里做定点 `ninja` 验证，`build.py install` 后仍要确认 `output/third_party/llvm/lib/libLLVM.dylib` 真正更新了

现象：

- 在 `cangjie_compiler/build/build/third_party/cjnative-prefix/src/cjnative-build` 里单独 `ninja lib/libLLVM.dylib bin/llc` 后
- 用 build tree 里的 `./bin/llc` 跑单点 repro 已经通过
- 但 `compiler/output/third_party/llvm/bin/llc` 仍然复现旧问题

本次实际根因：

- `output/third_party/llvm/bin/llc` 已经是新的
- 但它运行时加载的 `output/third_party/llvm/lib/libLLVM.dylib` 还是旧的

规避：

- 如果只是标准的一次性全量 `build.py clean/build/install`，通常不用手工干预
- 如果是做 LLVM 局部定点重编并想立刻验证，建议额外检查：

```bash
ls "$COMPILER_ROOT/build/build/third_party/cjnative-prefix/src/cjnative-build/lib/libLLVM.dylib"
ls "$COMPILER_ROOT/output/third_party/llvm/lib/libLLVM.dylib"
```

- 一旦怀疑 `output` 里的 `libLLVM.dylib` 没同步，直接手工覆盖并重签名：

```bash
cp "$COMPILER_ROOT/build/build/third_party/cjnative-prefix/src/cjnative-build/lib/libLLVM.dylib" \
	"$COMPILER_ROOT/output/third_party/llvm/lib/libLLVM.dylib"
codesign -f -s - "$COMPILER_ROOT/output/third_party/llvm/lib/libLLVM.dylib"
```

### 10. 不要把 `cangjie_compiler/output` 直接当成“完整 SDK 拷贝源”

现象：

- 把 `cangjie_compiler/output/` 直接 `rsync` 到一个新目录后，`bin/cjc --help` 能正常启动
- 但真正编译 hello 之类的最小程序时会在链接阶段失败
- 典型报错包括：

```text
ld64.lld: error: cannot open .../lib/darwin_aarch64_cjnative/section.o: No such file or directory
ld64.lld: error: cannot open .../lib/darwin_aarch64_cjnative/cjstart.o: No such file or directory
ld64.lld: error: library not found for -lcangjie-runtime
```

根因：

- `cangjie_compiler/output` 里主要是 compiler 自身安装结果
- 如果没有先把 runtime/stdlib 的安装产物完整合并进去，它并不等价于一份可直接分发的完整 SDK
- 因此“编译器可执行文件存在”不代表“整套链接物已经齐全”

规避：

- 如果是第一次做一份新的可用 SDK，优先复制一份已经验证可编译 hello 的完整 SDK 目录
- 或者先按 Step 2、Step 3、Step 4 把 runtime 和 stdlib 全量合并进 `compiler/output`，再把它复制到新位置
- 复制后至少做一次最小冒烟验证：

```bash
source "$TARGET_SDK/envsetup.sh"
"$TARGET_SDK/bin/cjc" "$COMPILER_ROOT/hello.cj" -o /tmp/hello_sdk_check
/tmp/hello_sdk_check
```

- 只有这一步能输出 `Hello, Cangjie`，才能把目标目录视为完整可用 SDK

## 最终验证标准

最小检查集如下：

```bash
test -f "$COMPILER_ROOT/output/bin/cjc"
test -f "$COMPILER_ROOT/output/envsetup.sh"
test -f "$COMPILER_ROOT/output/third_party/llvm/lib/libLLVM.dylib"
test -f "$COMPILER_ROOT/output/runtime/lib/darwin_aarch64_cjnative/libcangjie-runtime.dylib"
test -f "$COMPILER_ROOT/output/runtime/lib/darwin_aarch64_cjnative/libcangjie-std-core.dylib"
```

如果当前源码还带 LLVM marker，也可以额外检查：

```bash
strings "$COMPILER_ROOT/output/third_party/llvm/lib/libLLVM.dylib" | grep '\[cj-llvm-rebuilt\]'
```

最终仍以 `hello.cj` 能编译、能运行作为成功判据。

## 可选：同步到外部 SDK

如果只需要本地 `compiler/output`，到这里就可以停。

如果要把这套已验证产物同步到外部 SDK，再执行下面这组命令。这里必须使用绝对路径。

```bash
export TARGET_SDK=/Users/lipeisheng/code/cangjie/cangjie-pkg/dev/20260421/cangjie

rsync -a "$COMPILER_ROOT/output/" "$TARGET_SDK/"

find "$TARGET_SDK/runtime" "$TARGET_SDK/lib" "$TARGET_SDK/third_party/llvm/lib" "$TARGET_SDK/tools/lib" \
	-type f -name '*.dylib' -exec codesign -f -s - {} \;

codesign -f -s - "$TARGET_SDK/bin/cjc"
codesign -f -s - "$TARGET_SDK/bin/cjfilt"
```

替换后再验证一次：

```bash
source "$TARGET_SDK/envsetup.sh"
cd "$COMPILER_ROOT"
rm -f hello_pkg
cjc hello.cj -o hello_pkg
./hello_pkg
```

当前这轮实际验证里，目标 SDK 环境下也成功打印了：

```text
[cj-compiler-rebuilt] exe=/Users/lipeisheng/code/cangjie/cangjie-pkg/dev/20260421/cangjie/bin/cjc input=hello.cj
[cj-llvm-rebuilt]
[cj-stdlib-rebuilt]
Hello, Cangjie
```
