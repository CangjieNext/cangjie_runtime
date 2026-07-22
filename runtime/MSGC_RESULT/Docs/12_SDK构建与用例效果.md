# MSGC SDK 构建、Runtime 替换与 case27 对比复现指南

> 本文用于在 macOS aarch64 上从源码构建 Cangjie SDK，构建并安装 MSGC runtime，
> 将 runtime 同步/替换到 SDK，然后复现 `case27_http_complex_json` 的 MSGC、CopyGC
> 和 Go 对比测试。
>
> 本文只整理可复现流程；本次编写没有重新执行构建、runtime 替换或 case27 压测。

## 1. 路径、版本与结果口径

### 1.1 源码目录与产物目录

本文以当前工作区的 macOS aarch64 布局为例。先定义几个后续命令会用到的变量：

```bash
export CJ_WORK=/Users/lipeisheng/code/cangjie/cangjie
export COMPILER_ROOT=$CJ_WORK/cangjie_compiler
export RUNTIME_ROOT=$CJ_WORK/cangjie_runtime_MSGC/runtime
export STDLIB_ROOT=$CJ_WORK/cangjie_runtime_MSGC/stdlib
export STDX_ROOT=$CJ_WORK/cangjie_stdx

# 完整 SDK 的构建输出；一键脚本会把 compiler、runtime、stdlib、stdx overlay 到这里。
export MSGC_SDK_ROOT=$COMPILER_ROOT/output
export MSGC_PLATFORM_ROOT=$MSGC_SDK_ROOT/darwin_aarch64_cjnative
export MSGC_STDX_ROOT=$MSGC_PLATFORM_ROOT
export MSGC_STDX=$MSGC_PLATFORM_ROOT/dynamic/stdx

export COPYGC_SDK_ROOT=/Users/lipeisheng/code/cangjie/cangjie-pkg/dev/20260507/cangjie
export COPYGC_PLATFORM_ROOT=/Users/lipeisheng/code/cangjie/cangjie-pkg/dev/20260507/darwin_aarch64_cjnative
```

`COMPILER_ROOT/output` 是 SDK 根目录，包含 `envsetup.sh`、`bin/cjc`、`lib`、`modules`、
`runtime` 和 `darwin_aarch64_cjnative`。Runtime 自身的直接构建产物在：

```text
$RUNTIME_ROOT/output/temp/lib/aarch64_Release/libcangjie-runtime.dylib
```

Runtime 安装后的公共目录通常为：

```text
$RUNTIME_ROOT/output/common/darwin_Release_aarch64/
├── lib/darwin_aarch64_cjnative/
└── runtime/lib/darwin_aarch64_cjnative/
```

### 1.2 MSGC SDK 与 CopyGC 基线

有两种 MSGC SDK 使用方式，不能混写：

| 用途 | SDK 根目录 | 说明 |
| --- | --- | --- |
| 从当前源码重新构建 | `$COMPILER_ROOT/output` | 推荐的可复现构建输出；由第 2 节脚本生成 |
| 使用已有自构建包 | `/Users/lipeisheng/code/cangjie/cangjie-pkg/self/dev260509/cangjie` | AGENTS.md 约定的现成 MSGC 包 |
| CopyGC 性能基线 | `$COPYGC_SDK_ROOT` | 官方 Dev CopyGC 包，只读使用，禁止替换其 runtime |

`MSGC_SDK_ROOT` 必须指向包含 `envsetup.sh` 的 SDK 根目录；`MSGC_STDX_ROOT` 必须指向
包含 `dynamic/stdx` 的平台目录。`run_compare.sh` 会在平台目录下自动拼接
`dynamic/stdx`，所以不要把 `.../dynamic/stdx` 本身填给 `MSGC_STDX_ROOT`。

AGENTS.md 中的 `self/dev260509` 是当前主力现成 MSGC 包约定；case27 最近一批已保存结果
使用的是 `$COMPILER_ROOT/output` 作为 MSGC SDK。重新构建时应记录实际使用的 compiler、
runtime、stdlib 和 stdx 路径，不能把“output compiler + output stdlib + 手工替换 runtime”
误称为某个发布 SDK 的原样运行。

### 1.3 历史结果的解释口径

本文的“已有结果”指仓库中已经落盘的结果，不是本次重新执行的结果。重点采用
`MSGC_RESULT/Result/case27_http_complex_json/result/20260712_*_output_msgc_vs_copygc_go_*_compare/`
下的 2026-07-12 结果；这些结果的 MSGC `sdk_root` 记录为
`/Users/lipeisheng/code/cangjie/cangjie/cangjie_compiler/output`。

所有对比都应同时记录：payload、concurrency、duration、heap size、GC 参数、SDK 根目录、
stdx 目录、runtime dylib 校验值和结果目录。不同 SDK、编译器或参数下的数字只能作为趋势
参考，不能直接拼成同一组基准。

## 2. 从源码构建完整 MSGC SDK

### 2.1 前置条件

需要准备 macOS aarch64 的 clang/clang++、CMake、Python 3、OpenSSL 3，以及已经完整检出
的 `cangjie_compiler`、`cangjie_runtime_MSGC`、`stdlib` 和 `cangjie_stdx` 源码。stdlib 和
stdx 构建需要 OpenSSL 的库目录；本机默认位置通常是：

```text
/opt/homebrew/opt/openssl@3/lib
```

构建前确认 SDK 输出目录属于本次构建，避免把旧 SDK 的 `lib`、`modules` 或 `runtime` 与
新 compiler 混用。若只想查看脚本将执行哪些命令，可以先使用 `--dry-run`；本次任务没有
执行构建或 dry-run。

### 2.2 推荐的一键构建命令

从 runtime 根目录执行。该脚本是当前工作区提供的统一入口，命令显式写出所有源码和产物
路径，便于以后直接复制复现：

```bash
cd "$RUNTIME_ROOT"

python3 build/scripts/rebuild_sdk_and_stdx.py \
  --compiler-root "$COMPILER_ROOT" \
  --runtime-root "$RUNTIME_ROOT" \
  --stdlib-root "$STDLIB_ROOT" \
  --stdx-root "$STDX_ROOT" \
  --sdk-root "$MSGC_SDK_ROOT" \
  --build-type release \
  --version 0.0.1 \
  --jobs 8 \
  --openssl-lib /opt/homebrew/opt/openssl@3/lib
```

脚本默认会在 macOS 上对输出 dylib 执行 ad-hoc codesign，并在最后检查 SDK 的
`envsetup.sh`、stdx 目录、runtime 目录和 `cjc -v`。如果实际 OpenSSL 路径不同，替换
`--openssl-lib`，不要把不存在的目录留在命令中。

### 2.3 一键脚本实际执行的阶段

脚本当前按以下顺序执行，顺序不要随意调整：

1. 构建 Cangjie compiler 的 `cjc`，再单独构建 cjnative 的 LLVM/llc/LTO。
2. 安装 compiler 到 `$MSGC_SDK_ROOT`，并同步 LLVM 运行库及 codesign。
3. 清理 runtime，执行 `build.py build --target native --build-type release --gc-mode marksweep`。
4. 将 MSGC runtime 安装到 `$RUNTIME_ROOT/output/common`，修复 runtime 安装目录布局，
   再同步 runtime 的 `lib`/`runtime` 到 `$MSGC_SDK_ROOT`。
5. 清理、构建、安装 stdlib；安装后先 overlay 到 SDK，因为 stdx 构建时会加载 std 宏插件
   并导入 SDK 中的 `.cjo`。
6. 清理、构建、安装 stdx 到 `$STDX_ROOT/target`，再把对应平台目录 overlay 到 SDK。
7. 对 SDK 中的 compiler LLVM、runtime、stdlib/stdx dylib 进行 ad-hoc codesign。

这里的 `--gc-mode marksweep` 是 MSGC 的关键开关；runtime `build.py` 默认是 `copygc`，
遗漏该选项会构建出 CopyGC runtime，不能当作 MSGC SDK。

### 2.4 构建产物检查

构建完成后只做文件和版本检查即可确认产物是否落盘：

```bash
test -f "$MSGC_SDK_ROOT/envsetup.sh"
test -x "$MSGC_SDK_ROOT/bin/cjc"
test -d "$MSGC_STDX"
test -f "$MSGC_PLATFORM_ROOT/dynamic/stdx/libstdx.net.http.dylib"
test -f "$MSGC_SDK_ROOT/runtime/lib/darwin_aarch64_cjnative/libcangjie-runtime.dylib"
```

正式验证时，在干净子 shell 中 `source "$MSGC_SDK_ROOT/envsetup.sh"` 后执行 `cjc -v`；
不要在同一个 shell 中先后 source MSGC 和 CopyGC 两套环境。

## 3. 单独构建、安装和替换 MSGC runtime

### 3.1 使用 Mark-Sweep 模式构建 runtime

如果 compiler/stdlib/stdx 不变，只更新 runtime，可单独执行：

```bash
cd "$RUNTIME_ROOT"
python3 build.py clean
python3 build.py build \
  --target native \
  --build-type release \
  --gc-mode marksweep \
  -v 0.0.1
```

`--gc-mode marksweep` 会设置 `COPYGC_FLAG=0`、`MARKSWEEP_FLAG=1`。不要省略该参数，也
不要用默认的 `copygc` 结果替换 MSGC SDK。

### 3.2 安装到 runtime 输出目录

构建成功后安装到 runtime 的公共输出目录：

```bash
python3 build.py install --prefix "$RUNTIME_ROOT/output/common"
```

两个常用来源都保留：

```text
直接构建产物：$RUNTIME_ROOT/output/temp/lib/aarch64_Release/libcangjie-runtime.dylib
安装产物：    $RUNTIME_ROOT/output/common/darwin_Release_aarch64/runtime/lib/darwin_aarch64_cjnative/libcangjie-runtime.dylib
```

### 3.3 替换 SDK 中的 dylib

完整 SDK 一键构建时，脚本会自动把安装好的 runtime overlay 到 `$MSGC_SDK_ROOT`；只更新
runtime 时，手工替换命令如下：

```bash
export SDK_RUNTIME_DIR="$MSGC_SDK_ROOT/runtime/lib/darwin_aarch64_cjnative"
export MSGC_RUNTIME_DYLIB="$RUNTIME_ROOT/output/temp/lib/aarch64_Release/libcangjie-runtime.dylib"

# 不覆盖已有的 libcangjie-runtime.dylib.origin；它是稳定回退副本。
cp "$MSGC_RUNTIME_DYLIB" "$SDK_RUNTIME_DIR/libcangjie-runtime.dylib"

# macOS arm64 替换 dylib 后必须重签名，否则运行时可能被 SIGKILL。
codesign -f -s - "$SDK_RUNTIME_DIR/libcangjie-runtime.dylib"
```

替换后可记录校验值：

```bash
md5 -q "$SDK_RUNTIME_DIR/libcangjie-runtime.dylib"
```

不要对 `$COPYGC_SDK_ROOT` 执行上述替换。CopyGC SDK 只作为固定基线使用。

### 3.4 恢复原始 runtime

恢复 MSGC SDK 的稳定包：

```bash
cp "$SDK_RUNTIME_DIR/libcangjie-runtime.dylib.origin" \
   "$SDK_RUNTIME_DIR/libcangjie-runtime.dylib"
codesign -f -s - "$SDK_RUNTIME_DIR/libcangjie-runtime.dylib"
```

如果 `.origin` 不存在，先停止操作并确认备份来源；不要用不确定的其他 SDK dylib 冒充回退
版本。

### 3.5 替换后的强制规则

替换 SDK 中的 `libcangjie-runtime.dylib` 后，必须用该 SDK 重新编译所有待运行的 `.cj`
文件，再运行新 binary。原因是 `.cj` 编译产物会携带与当前 runtime/GC 模式匹配的初始化
数据和符号引用；禁止复用“旧 SDK 编译 + 新 runtime 替换”得到的 binary。

case27 的 `run_compare.sh` 已在 CopyGC、MSGC 两个独立子 shell 内各自执行 `cjc`，所以
每次正式对比都应让脚本重新生成两侧的 `benchmark`，不要手工拿历史结果目录中的可执行
文件直接运行。

## 4. case27 用例说明

### 4.1 用例做什么

`case27_http_complex_json` 不是简单的 HTTP 连通性测试，而是一个单进程内的闭环压力
用例：同一个进程启动 HTTP Server 和多个 HTTP Client，Client 向
`127.0.0.1:<port>/process` 发送 POST，Server 完整解析复杂 JSON、修改多层字段并重新
序列化，Client 再解析响应并检查 Server 写入的 marker。

Server 每次请求主要执行以下 read-modify-write：

1. `counters.in`、`counters.ok` 各加 1；
2. 根对象增加 `processed_seq` 和 `server_ok`；
3. `events[i].seq` 加 1，`events[i].score` 乘 2；
4. 修改深层的 `events[i].user.attrs.credits`；
5. 向 `events[i].custom.product_ids` 追加整数，并增加 `server_marker`；
6. 整棵 JSON 树重新序列化，Client 再反序列化并断言 `server_ok == true`。

因此一次请求同时覆盖本地回环 HTTP、socket/连接关闭、JSON 字符串/数组/对象分配、深层
引用写入、批量 struct/array copy、请求响应两侧 serde，以及 MSGC 的分配、并发标记和
sweep/reclaim 路径。

### 4.2 关键负载与并发模型

payload 位于 `payloads/`，由外部 JSON 文件提供：

| 名称 | 约略大小 |
| --- | ---: |
| `1K.json` | 1.4 KB |
| `10K.json` | 9.3 KB |
| `100K.json` | 97 KB |
| `1M.json` | 999 KB |

Cangjie 和 Go 各有一份 benchmark：

- Cangjie：`benchmark.cj`，由 MSGC SDK 和 CopyGC SDK 分别编译；
- Go：`go_benchmark/`，用 `map[string]any`、`[]any` 构造通用 JSON 树，作为非 Cangjie
  GC 的参考，不替代 CopyGC 基线。

默认 `sync` 模式下，`CASE27_CONCURRENCY` 个 worker 按“发送请求 → 等响应 → 再发送”循环。
`async` 模式是单 sender 加 inflight 限流，真正的并发深度由 `CASE27_MAX_INFLIGHT` 控制，
适合观察持续发射请求时的 timeout/hang/stall；正式 MSGC/CopyGC 基准建议先使用 `sync`。

### 4.3 运行参数与结果文件

正式对比常用参数：

| 参数 | 默认/推荐值 | 作用 |
| --- | ---: | --- |
| `CASE27_PAYLOAD` | `10K` | 选择 payload 文件 |
| `CASE27_CONCURRENCY` | `8` | sync worker 数 |
| `CASE27_DURATION_MS` | `60000` | 单组运行时长 |
| `CASE27_CLIENT_MODE` | `sync` | `sync` 或 `async` |
| `CASE27_CLIENT_TIMEOUT_MS` | `5000` | 单请求读写超时 |
| `HEAP_SIZE` | `12gb` | 主要用于 CopyGC 固定堆 |
| `MSGC_HEAP_GROWTH` | `1` | MSGC `cjHeapGrowth` |
| `MSGC_SCAVENGE_GROWTH` | `10` | MSGC `cjScavengeGrowth` |
| `MSGC_MIN_GC_THRESHOLD` | 脚本默认 `5mb` | MSGC `cjMSMinGCThreshold` |

MSGC 使用 Mark-Sweep 自动增长堆；`run_compare.sh` 虽然会把 `HEAP_SIZE` 作为统一配置传给
两侧，但它不会像 CopyGC 那样预分配固定堆。要复现 2026-07-12 已保存矩阵，还需显式设置
`MSGC_MIN_GC_THRESHOLD=20mb`。

每个 side 的关键文件是 `stdout.txt`、`gc.log`、`rss.csv`、`run_metadata.env` 和重新
编译的 `benchmark`。总对比目录还包含 `compare_report.md`、RSS/CPU 图、吞吐图和 GC 单次
耗时图。

## 5. MSGC、CopyGC、Go 对比测试

### 5.1 推荐入口与参数

统一入口是：

```text
MSGC_RESULT/Result/case27_http_complex_json/run_compare.sh
```

它按 CopyGC → MSGC → Go 的顺序运行，并自动完成：

1. 在各自 SDK 环境中重新编译 Cangjie `benchmark.cj`；
2. 分别运行 CopyGC、MSGC 和 Go benchmark；
3. 用 `ps -o rss=,%cpu= -p <pid>` 采样 RSS/CPU；
4. 用 `script/compare_results.py` 解析吞吐、失败数、RSS/CPU 和 GC 日志；
5. 生成 Markdown 报告和图表。

脚本要求四个 SDK 变量都存在：`MSGC_SDK_ROOT`、`MSGC_STDX_ROOT`、`COPYGC_SDK_ROOT`、
`COPYGC_STDX_ROOT`。其中两个 `*_STDX_ROOT` 是平台根目录，不是 `dynamic/stdx` 目录。

### 5.2 SDK 隔离与 dylib 来源检查

不要在同一个交互 shell 中连续 source 两套 `envsetup.sh`。AGENTS.md 已记录：不同 SDK
对 `DYLD_LIBRARY_PATH`/`DYLD_FALLBACK_LIBRARY_PATH` 的处理不同，残留路径可能让 CopyGC
程序静默加载 MSGC runtime，导致结果失真。

`run_compare.sh` 的正确隔离方式是每一侧都使用 `env -i`，只传入 `HOME`、`TERM`、最小
`PATH`、对应 SDK 和对应 stdx，然后在子 shell 内 source 对应 SDK、设置：

```bash
export DYLD_LIBRARY_PATH="$STDX_DIR:${DYLD_LIBRARY_PATH:-}"
```

脚本每次运行都会重新执行两次 `cjc`，并在 `run_metadata.env` 保存 `sdk_root`、`stdx_dir`
和 `runtime_dylib_md5`。若需要按 AGENTS.md 做实际加载来源探针，应在干净子 shell 内编译
后运行一个很短的 benchmark，并检查 `DYLD_PRINT_LIBRARIES=1` 输出中加载的 runtime 路径；
不要用当前 shell 中已经污染过的 `DYLD_LIBRARY_PATH` 做探针。

最小探针的核心形式如下，`SDK_ROOT`/`STDX_DIR` 每次只填一套 SDK：

```bash
env -i HOME="$HOME" TERM="$TERM" \
  PATH=/usr/bin:/bin:/usr/sbin:/sbin:/opt/homebrew/bin \
  SDK_ROOT="$MSGC_SDK_ROOT" STDX_DIR="$MSGC_STDX" \
  CASE_DIR="$PWD" OUT_DIR="$PWD/probe_msgc" \
  bash -c '
    set -euo pipefail
    source "$SDK_ROOT/envsetup.sh" >/dev/null 2>&1
    export DYLD_LIBRARY_PATH="$STDX_DIR:${DYLD_LIBRARY_PATH:-}"
    mkdir -p "$OUT_DIR"
    cd "$OUT_DIR"
    cjc "$CASE_DIR/benchmark.cj" --import-path "$STDX_DIR" -L "$STDX_DIR" \
      --link-option "$STDX_DIR/libstdx.net.http.dylib" \
      --link-option "$STDX_DIR/libstdx.encoding.json.dylib" -o benchmark
    DYLD_PRINT_LIBRARIES=1 CASE27_PAYLOAD=1K CASE27_CONCURRENCY=1 \
      CASE27_DURATION_MS=1000 CASE27_PAYLOAD_DIR="$CASE_DIR/payloads" \
      ./benchmark 2>&1 | grep "cangjie-runtime"
  '
```

同样的探针要在 CopyGC 的全新 `env -i` 子 shell 中执行一次；预期路径必须分别落在对应
SDK 下。探针只用于确认加载来源，不作为正式性能结果。

### 5.3 执行对比测试

先执行一组默认的 10K/8 workers 对比：

```bash
export CASE27_DIR=$RUNTIME_ROOT/MSGC_RESULT/Result/case27_http_complex_json
cd "$CASE27_DIR"

MSGC_SDK_ROOT="$MSGC_SDK_ROOT" \
MSGC_STDX_ROOT="$MSGC_PLATFORM_ROOT" \
COPYGC_SDK_ROOT="$COPYGC_SDK_ROOT" \
COPYGC_STDX_ROOT="$COPYGC_PLATFORM_ROOT" \
CASE27_PAYLOAD=10K \
CASE27_CONCURRENCY=8 \
CASE27_DURATION_MS=60000 \
CASE27_CLIENT_MODE=sync \
HEAP_SIZE=12gb \
MSGC_HEAP_GROWTH=1 \
MSGC_SCAVENGE_GROWTH=10 \
GO_ENABLE=1 \
./run_compare.sh
```

要复现仓库中 2026-07-12 的 12 组 output-MSGC 矩阵，使用同一干净终端逐组执行以下循环。
每次循环仍然是一个独立的 case27 run，不能把多个 payload/concurrency 放进同一个进程：

```bash
for payload in 1K 10K 100K; do
  for concurrency in 1 10 100 1000; do
    MSGC_SDK_ROOT="$MSGC_SDK_ROOT" \
    MSGC_STDX_ROOT="$MSGC_PLATFORM_ROOT" \
    COPYGC_SDK_ROOT="$COPYGC_SDK_ROOT" \
    COPYGC_STDX_ROOT="$COPYGC_PLATFORM_ROOT" \
    CASE27_PAYLOAD="$payload" \
    CASE27_CONCURRENCY="$concurrency" \
    CASE27_DURATION_MS=60000 \
    CASE27_CLIENT_MODE=sync \
    HEAP_SIZE=12gb \
    MSGC_HEAP_GROWTH=1 \
    MSGC_SCAVENGE_GROWTH=10 \
    MSGC_MIN_GC_THRESHOLD=20mb \
    GO_ENABLE=1 \
    ./run_compare.sh
  done
done
```

`run_compare.sh` 的每一侧默认使用不同端口（CopyGC 18226、MSGC 18230、Go 18234），还会
在端口被占用时向后寻找空闲端口。正式运行前仍应检查并清理旧的 benchmark、`llc` 或监控
进程；处于 `U/UE/UE+` 的历史进程不能当作本轮仍在运行。

### 5.4 如何阅读结果

先检查运行是否有效，再读性能：

```text
copygc/stdout.txt、msgc/stdout.txt、go/stdout.txt：应有 [RESULT]
copygc/rss.csv、msgc/rss.csv、go/rss.csv：应非空
各 side/run_metadata.env：确认 SDK、stdx、参数和 runtime_dylib_md5
compare_report.md：最终汇总
```

`[RESULT]` 重点看 `total`、`ok`、`fail`、`throughput`、`bytes_sent/recv`。只有 `ok > 0`、
失败原因可解释且报告的 RSS/CPU 不是 `0.00`/`N/A` 时，才可以继续解读 GC 性能。`gc.log`
中 MSGC 应出现 `[MS-GC]`，CopyGC 应出现 CopyGC 的 `[GC] Start`，Go 应出现
`GODEBUG=gctrace=1` 格式；GC 模式不匹配时先停止分析。

`compare_results.py` 的 GC 指标是日志解析值：GC 次数、总时间、平均时间、最大时间和 P95；
它不等同于 wall time。RSS/CPU 来自独立采样，采样为空时不能把 0 当成真实低内存。

## 6. 已有 case27 结果

### 6.1 结果来源

本节只引用已经存在的结果文件，不在本次文档编写中重新运行。2026-07-12 的矩阵包含：

- payload：`1K`、`10K`、`100K`；
- concurrency：`1`、`10`、`100`、`1000`；
- 每组 60 秒，`sync` 模式，`HEAP_SIZE=12gb`；
- MSGC：`cjHeapGrowth=1`、`cjScavengeGrowth=10`、`cjMSMinGCThreshold=20mb`；
- 三侧：CopyGC、output MSGC SDK、Go mirror。

结果目录为 [`case27_http_complex_json/result`](../Result/case27_http_complex_json/result/)。
报告脚本使用每个 side 的 stdout、GC log 和 RSS/CPU 采样生成 `compare_report.md`；报告中的
GC 次数和时间是日志解析结果，吞吐是 benchmark `[RESULT]` 的请求统计。

### 6.2 代表性结果

下面列出两个最能说明差异的 `100K` 结果。列顺序均为 CopyGC / MSGC / Go；数字来自报告，
不是重新测得的数字。

| 配置 | 吞吐 (req/s) | fail | RSS 平均 (MB) | RSS 峰值 (MB) |
| --- | --- | --- | --- | --- |
| `c100_p100K` | `728 / 880 / 969` | `0 / 0 / 0` | `1809.72 / 457.13 / 162.37` | `2534.92 / 631.33 / 166.64` |
| `c1000_p100K` | `492 / 855 / 992` | `50 / 0 / 392` | `3006.47 / 1405.37 / 1023.55` | `4102.75 / 1461.05 / 1079.20` |

对应报告：

- [`20260712_201532...c100_p100K_compare/compare_report.md`](../Result/case27_http_complex_json/result/20260712_201532_output_msgc_vs_copygc_go_c100_p100K_compare/compare_report.md)
- [`20260712_221609...c1000_p100K_compare/compare_report.md`](../Result/case27_http_complex_json/result/20260712_221609_output_msgc_vs_copygc_go_c1000_p100K_compare/compare_report.md)

GC 指标如下：

| 配置 | GC 次数 CopyGC / MSGC / Go | 平均 GC 时间 (ms) CopyGC / MSGC / Go | 最大 GC 时间 (ms) CopyGC / MSGC / Go |
| --- | --- | --- | --- |
| `c100_p100K` | `242 / 3881 / 3739` | `131.47 / 11.12 / 7.76` | `348.52 / 76.86 / 46.19` |
| `c1000_p100K` | `111 / 979 / 662` | `449.22 / 35.69 / 38.83` | `1221.42 / 73.22 / 86.24` |

2026-07-12 矩阵的吞吐（每格为 CopyGC / MSGC / Go，单位 req/s）为：

| payload | c1 | c10 | c100 | c1000 |
| --- | --- | --- | --- | --- |
| `1K` | `3785 / 5439 / 8211` | `19016 / 25836 / 18822` | `19660 / 23873 / 25716` | `17861 / 21643 / 33862` |
| `10K` | `1523 / 2086 / 1972` | `6611 / 7687 / 2792` | `6212 / 6851 / 6299` | `5657 / 7337 / 7688` |
| `100K` | `215 / 293 / 229` | `751 / 917 / 739` | `728 / 880 / 969` | `492 / 855 / 992` |

### 6.3 总体结论与限制

从这 12 组已保存结果可以得到以下结论：

- MSGC 吞吐在 12 组中都高于 CopyGC；`c1000_p100K` 为 `855 vs 492 req/s`，约高 74%。
- MSGC 的 RSS 在这些结果中均低于 CopyGC；代表性的 `c100_p100K` 平均 RSS 为
  `457.13 MB vs 1809.72 MB`，峰值为 `631.33 MB vs 2534.92 MB`。
- MSGC 的 GC 次数明显更多，但单次 GC 更短；这符合并发 Mark-Sweep 以更多小周期换取
  更短单次停顿的特征。不能只比较 GC 次数判断优劣，应同时看 wall time、吞吐和失败数。
- 在 `c1000_p100K` 压力下，本记录中 MSGC `fail=0`，CopyGC `fail=50`，Go `fail=392`；
  这说明高并发下三侧已经进入 timeout/连接压力区间，不能把该组单独当作纯 GC 微基准。
- Go 是额外参考实现，JSON 树模型和 HTTP 库不同；它用于观察“非 Cangjie 实现”的数量级，
  不能替代 CopyGC 的同语言 A/B 对照。

这些结论只适用于结果中记录的 SDK、runtime md5、参数和机器环境。源码、compiler、stdlib、
stdx 或参数变化后，必须生成新的结果目录再更新结论。

## 7. 一次完整复现清单

按下面顺序执行即可复现一套新的 MSGC SDK + case27 对比：

1. 进入 runtime 根目录，设置第 1 节的源码路径和 SDK 输出路径。
2. 执行第 2.2 节的一键脚本，完成 compiler、LLVM、MSGC runtime、stdlib、stdx 的构建、
   安装、overlay 和 codesign。
3. 检查 `$MSGC_SDK_ROOT/envsetup.sh`、`cjc`、stdx dylib、MSGC runtime dylib 均存在。
4. 对 MSGC SDK 和 CopyGC SDK 分别执行干净 `env -i` 的 dylib 来源探针，确认路径没有串包。
5. 进入 `MSGC_RESULT/Result/case27_http_complex_json`，执行第 5.3 节的单组命令；正式
   复现实验则执行 12 组矩阵循环。
6. 每组先确认 `stdout.txt` 有 `[RESULT]`、`fail` 可解释，`rss.csv` 非空，再阅读
   `compare_report.md` 和图表。
7. 如果只替换了 runtime，下一次对比必须重新执行两侧 `cjc`；`run_compare.sh` 会完成这件事。
8. 实验结束后若要恢复稳定 MSGC 包，按第 3.4 节恢复 `.origin` 并重签名；CopyGC 包不需要
   任何恢复操作，因为整个过程不应修改它。

## 8. 常见错误

### 8.1 runtime 模式错了

`runtime/build.py` 默认 `copygc`。如果忘记 `--gc-mode marksweep`，即使文件名仍是
`libcangjie-runtime.dylib`，也不是 MSGC runtime。检查构建命令和 `gc.log` 中的模式标记。

### 8.2 SDK 环境串包

在同一个 shell 中先 source MSGC、再 source CopyGC，或者在 source 后额外套一层会重置环境
的 `env`/多层 `bash -c`，都可能让 dyld 加载错误路径。使用 `run_compare.sh` 的 `env -i`
模型；手工运行也必须每个 SDK 一个全新子 shell。

### 8.3 替换 dylib 后复用旧 binary

这是禁止的流程。切换 CopyGC/MSGC 或替换任何 SDK runtime 后都重新 `cjc` 编译；macOS arm64
还必须 `codesign -f -s -`。否则可能出现 SIGKILL、dyld 启动异常或不可中断卡死。

### 8.4 把 CopyGC 基线也替换了

CopyGC 固定 SDK 只作为对照，不要把 MSGC build 输出复制到
`$COPYGC_SDK_ROOT/runtime/lib/darwin_aarch64_cjnative/`。如果误改，应重新取得官方基线包，
不要继续用混合状态的结果。

### 8.5 把无效运行当成性能结果

如果 `stdout.txt` 为空、`gc.log` 不生成、`rss.csv` 为空，或 stderr 出现
`Library not loaded`、`Failed to bind`、大量 read timeout，应先检查 dyld、端口、本地回环
权限和旧进程。不要在这种情况下继续等待并把它解释为“只是运行慢”。

### 8.6 参数或来源没有落盘

每次运行都应保留 `run_metadata.env`，至少核对 SDK、stdx、payload、并发度、时长、heap、
GC growth、min threshold 和 runtime md5。缺少这些信息的结果只能作为现场日志，不能作为正式
对比基线。
