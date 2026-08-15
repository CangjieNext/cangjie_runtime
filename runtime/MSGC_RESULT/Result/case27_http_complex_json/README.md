# case27_http_complex_json — HTTP 复杂 JSON 并发读改写压测

## 目标场景

在单进程内启动 HTTP Server，同时启动 N 个并发 HTTP Client，对同一 endpoint
持续发送同样 payload 的 POST 请求；server 端对请求体做完整 JSON
反序列化 → 读改写若干字段 → 重新序列化返回；client 端再反序列化响应并
校验 server 写入的 marker 字段。

与 `case8_http_json_serde` 的差异：

| 维度 | case8 | case27 |
| --- | --- | --- |
| Payload | 4 种规模在代码中硬编码 | 4 种规模（1K/10K/100K/1M）外置为 JSON 文件 |
| 结构复杂度 | 中（嵌套 2-3 层） | 高（嵌套 ≥ 4 层、混合类型、异构数组） |
| Server 处理 | 解析 + 重序列化 + 构造小响应 | 解析 + **读改写多个字段** + 整树重序列化 |
| 客户端模型 | 单线程依次 4 个 phase | **N 并发 worker 同时压测同一 payload** |
| 一次运行 | 跑完全部 phase | **单次只跑一组 (concurrency × payload)** |

更适合复现 long-running 高分配 + 高存活率的稳态 GC 负载。

当前 README 与 `run_compare.sh` 约定的默认基线参数是：

- `CASE27_PAYLOAD=10K`
- `CASE27_CONCURRENCY=8`
- `CASE27_DURATION_MS=60000`
- `MSGC_HEAP_GROWTH=1`
- `MSGC_SCAVENGE_GROWTH=10`

当前目录下同时维护两套 benchmark：

1. `benchmark.cj`
   Cangjie 版本，供 MSGC / CopyGC 对比。
2. `go_benchmark/`
   Go mirror 版本，保留和 `benchmark.cj` 同样的：
   - `both/server` 角色模型
   - `sync/async` client 模式
   - 复杂 JSON 全量反序列化
   - 深层字段读改写
   - 整树重新序列化
   - client 再反序列化校验

Go 版本为了尽量贴近 `JsonValue` 树的大量堆对象行为，优先使用
`map[string]any` + `[]any` 的通用 JSON 树，而不是预定义 struct。

Linux 的 `run_compare_linux.sh` 会根据 `uname -m` 选择
`linux_x86_64_cjnative` 或 `linux_aarch64_cjnative`，并按架构探测 CopyGC 包：
x86_64 优先 `/home/lipeisheng/cangjie-pkg/260723`，aarch64 优先
`/home/lipeisheng/cangjie-pkg/main/260723`。CopyGC、MSGC、Go、Java 的依赖检查彼此
独立，缺失时只跳过对应实现；至少两项可用时才生成对比报告。可使用
`COPYGC_PACKAGE_ROOT`、`COPYGC_SDK_ROOT`、`COPYGC_STDX_ROOT` 等变量覆盖默认路径。

## 这个用例测试了什么

case27 不是单纯的“HTTP 能不能通”，而是把下面几条路径叠在一起压：

1. **同进程 HTTP Server/Client 往返路径**
   - benchmark 在一个进程里同时启动 server 和 client，走本地 `127.0.0.1:<port>`。
   - 因此它会覆盖 listen / accept / send / read response / timeout / 连接关闭等 HTTP1.1 路径。

2. **复杂 JSON 的双向 serde + 读改写**
   - request 侧：client 从外部 `1K/10K/100K/1M` JSON 文件读 payload。
   - response 侧：server 把整棵 JSON 树反序列化成 `JsonValue`，修改多层嵌套字段，再整树序列化返回。
   - client 收到响应后再次反序列化并校验 `server_ok`，确保 request 和 response 两侧的 serde 都真的被跑到。

3. **长时间稳态分配 / 存活对象 / GC 压力**
   - 每次请求都会创建新的 JSON 树、字符串、数组和嵌套对象。
   - payload 越大、运行时间越长，越容易把 case 推到“高分配 + 高存活率 + 持续并发”的 GC 稳态。

4. **两种不同的 client 调度模型**
   - `sync`：`CASE27_CONCURRENCY` 个 worker；每个 worker 都是 `send -> wait response -> next request`。
   - `async`：单 sender 循环每隔 `CASE27_SEND_INTERVAL_MS` 尝试发射一个请求，但总在途数受 `CASE27_MAX_INFLIGHT` 限制；它的目的是把“请求持续发射”与“等待响应完成”这两件事拆开看。

5. **可观测性**
   - `stdout`：`[PROGRESS]` / `[RESULT]` / request 级日志。
   - `gc.log`：runtime GC 日志。
   - `result/`：按一次运行落盘，便于对比吞吐、fail、inflight、GC 行为和抓栈现场。

## 快速使用方式

最常用的是下面 4 种跑法：

1. **单次固定时长跑一组 sync 基线**

```bash
MSGC_SDK_ROOT=/Users/lipeisheng/code/cangjie/cangjie-pkg/self/dev260630/cangjie \
MSGC_STDX_ROOT=/Users/lipeisheng/code/cangjie/cangjie-pkg/self/dev260630/darwin_aarch64_cjnative \
CASE27_PAYLOAD=10K CASE27_CONCURRENCY=8 CASE27_DURATION_MS=60000 \
MSGC_HEAP_GROWTH=1 MSGC_SCAVENGE_GROWTH=10 \
./run_compare.sh
```

补充说明：

- `GO_ENABLE=1`（默认）时会把 Go mirror benchmark 一起跑进统一报告和图表。
- 若只想保留旧的 Cangjie 双边对比，可显式设 `GO_ENABLE=0`。

2. **单次固定时长跑一组 async 实验配置**

```bash
MSGC_SDK_ROOT=/Users/lipeisheng/code/cangjie/cangjie-pkg/self/dev260630/cangjie \
MSGC_STDX_ROOT=/Users/lipeisheng/code/cangjie/cangjie-pkg/self/dev260630/darwin_aarch64_cjnative \
CASE27_PAYLOAD=10K CASE27_CONCURRENCY=8 \
CASE27_CLIENT_MODE=async CASE27_SEND_INTERVAL_MS=1 CASE27_MAX_INFLIGHT=32 \
VERIFY_DURATION_MS=60000 VERIFY_TIMEOUT_SEC=95 \
./bak/verify_async_mode.sh
```

3. **无限运行直到抓到 stall**

```bash
MSGC_SDK_ROOT=/Users/lipeisheng/code/cangjie/cangjie-pkg/self/dev260630/cangjie \
MSGC_STDX_ROOT=/Users/lipeisheng/code/cangjie/cangjie-pkg/self/dev260630/darwin_aarch64_cjnative \
CASE27_PAYLOAD=10K CASE27_CONCURRENCY=8 \
MSGC_HEAP_GROWTH=1 MSGC_SCAVENGE_GROWTH=10 \
./bak/run_msgc_until_stall.sh
```

4. **MSGC vs CopyGC vs Go 联合对比出图**

```bash
MSGC_SDK_ROOT=/Users/lipeisheng/code/cangjie/cangjie-pkg/self/dev260630/cangjie \
MSGC_STDX_ROOT=/Users/lipeisheng/code/cangjie/cangjie-pkg/self/dev260630/darwin_aarch64_cjnative \
COPYGC_SDK_ROOT=/Users/lipeisheng/code/cangjie/cangjie-pkg/dev/20260507/cangjie \
COPYGC_STDX_ROOT=/Users/lipeisheng/code/cangjie/cangjie-pkg/dev/20260507/darwin_aarch64_cjnative \
CASE27_PAYLOAD=10K CASE27_CONCURRENCY=8 CASE27_DURATION_MS=60000 \
HEAP_SIZE=12gb \
MSGC_HEAP_GROWTH=1 MSGC_SCAVENGE_GROWTH=10 \
./run_compare.sh
```

如果你只是想快速验证 benchmark 本体能否正常跑起来，优先用 `bak/verify_async_mode.sh`；
如果你要做正式对比或抓 stall，优先用 `run_compare.sh` 和 `bak/run_msgc_until_stall.sh`。

## 最常用：怎么跑对比，怎么单跑其中一侧

这一节只回答两个问题：

1. 怎么直接跑一组 `MSGC vs CopyGC vs Go` 的联合对比
2. 怎么单独只跑 `MSGC`、`CopyGC` 或 `Go` 其中一侧

### A. 直接跑三方联合对比

这是最推荐的正式入口。它会自动：

- 跑 `CopyGC`
- 跑 `MSGC`
- 跑 `Go mirror benchmark`
- 统一采样 `rss.csv`
- 生成 `compare_report.md` 和 4 张图

```bash
cd /Users/lipeisheng/code/cangjie/cangjie/cangjie_runtime_MSGC/runtime/MSGC_RESULT/Result/case27_http_complex_json

MSGC_SDK_ROOT=/Users/lipeisheng/code/cangjie/cangjie-pkg/self/dev260630/cangjie \
MSGC_STDX_ROOT=/Users/lipeisheng/code/cangjie/cangjie-pkg/self/dev260630/darwin_aarch64_cjnative \
COPYGC_SDK_ROOT=/Users/lipeisheng/code/cangjie/cangjie-pkg/dev/20260507/cangjie \
COPYGC_STDX_ROOT=/Users/lipeisheng/code/cangjie/cangjie-pkg/dev/20260507/darwin_aarch64_cjnative \
CASE27_PAYLOAD=10K CASE27_CONCURRENCY=8 CASE27_DURATION_MS=60000 \
HEAP_SIZE=12gb \
MSGC_HEAP_GROWTH=1 MSGC_SCAVENGE_GROWTH=10 \
./run_compare.sh
```

输出目录形如：

```text
result/<ts>_msgc_vs_copygc_go_c<N>_p<SIZE>_compare/
```

关键文件：

- `copygc/stdout.txt`、`msgc/stdout.txt`、`go/stdout.txt`
- `copygc/gc.log`、`msgc/gc.log`、`go/gc.log`
- `compare_report.md`
- `compare_rss_cpu_timeline.png`
- `compare_summary.png`
- `compare_throughput.png`
- `compare_gc_per_cycle.png`

如果只想保留旧的 Cangjie 双边对比，不跑 Go：

```bash
GO_ENABLE=0 ./run_compare.sh
```

### B. 单独只跑 MSGC

如果你只想看 MSGC 一侧的行为，不做对比，最稳妥的是直接用 MSGC SDK 编译并运行
`benchmark.cj`。

```bash
cd /Users/lipeisheng/code/cangjie/cangjie/cangjie_runtime_MSGC/runtime/MSGC_RESULT/Result/case27_http_complex_json

OUT_DIR="$PWD/result/manual_msgc_$(date +%Y%m%d_%H%M%S)"
mkdir -p "$OUT_DIR"

env -i HOME=$HOME TERM=$TERM PATH=/usr/bin:/bin:/usr/sbin:/sbin:/opt/homebrew/bin \
  SDK_ROOT=/Users/lipeisheng/code/cangjie/cangjie-pkg/self/dev260630/cangjie \
  STDX_ROOT=/Users/lipeisheng/code/cangjie/cangjie-pkg/self/dev260630/darwin_aarch64_cjnative \
  OUT_DIR="$OUT_DIR" \
  bash -c '
    set -euo pipefail
    source "$SDK_ROOT/envsetup.sh" >/dev/null 2>&1
    export DYLD_LIBRARY_PATH="$STDX_ROOT/dynamic/stdx:${DYLD_LIBRARY_PATH:-}"
    cd "$OUT_DIR"
    cjc "'"$PWD"'/benchmark.cj" \
      --import-path "$STDX_ROOT/dynamic/stdx" \
      -L "$STDX_ROOT/dynamic/stdx" \
      --link-option "$STDX_ROOT/dynamic/stdx/libstdx.net.http.dylib" \
      --link-option "$STDX_ROOT/dynamic/stdx/libstdx.encoding.json.dylib" \
      -o benchmark
    MRT_REPORT="$OUT_DIR/gc.log" \
    cjHeapGrowth=1 \
    cjScavengeGrowth=10 \
    CASE27_PAYLOAD=10K \
    CASE27_CONCURRENCY=8 \
    CASE27_DURATION_MS=60000 \
    CASE27_PAYLOAD_DIR="'"$PWD"'/payloads" \
    ./benchmark > stdout.txt 2>&1
  '
```

如果你要的是“无限跑，直到怀疑 stall 再抓现场”，直接用：

```bash
./bak/run_msgc_until_stall.sh
```

### C. 单独只跑 CopyGC

单跑 CopyGC 的方式和 MSGC 基本一样，只是换成 CopyGC SDK，并且常用 `cjHeapSize`
而不是 `cjHeapGrowth`。

```bash
cd /Users/lipeisheng/code/cangjie/cangjie/cangjie_runtime_MSGC/runtime/MSGC_RESULT/Result/case27_http_complex_json

OUT_DIR="$PWD/result/manual_copygc_$(date +%Y%m%d_%H%M%S)"
mkdir -p "$OUT_DIR"

env -i HOME=$HOME TERM=$TERM PATH=/usr/bin:/bin:/usr/sbin:/sbin:/opt/homebrew/bin \
  SDK_ROOT=/Users/lipeisheng/code/cangjie/cangjie-pkg/dev/20260507/cangjie \
  STDX_ROOT=/Users/lipeisheng/code/cangjie/cangjie-pkg/dev/20260507/darwin_aarch64_cjnative \
  OUT_DIR="$OUT_DIR" \
  bash -c '
    set -euo pipefail
    source "$SDK_ROOT/envsetup.sh" >/dev/null 2>&1
    export DYLD_LIBRARY_PATH="$STDX_ROOT/dynamic/stdx:${DYLD_LIBRARY_PATH:-}"
    cd "$OUT_DIR"
    cjc "'"$PWD"'/benchmark.cj" \
      --import-path "$STDX_ROOT/dynamic/stdx" \
      -L "$STDX_ROOT/dynamic/stdx" \
      --link-option "$STDX_ROOT/dynamic/stdx/libstdx.net.http.dylib" \
      --link-option "$STDX_ROOT/dynamic/stdx/libstdx.encoding.json.dylib" \
      -o benchmark
    MRT_REPORT="$OUT_DIR/gc.log" \
    cjHeapSize=12gb \
    CASE27_PAYLOAD=10K \
    CASE27_CONCURRENCY=8 \
    CASE27_DURATION_MS=60000 \
    CASE27_PAYLOAD_DIR="'"$PWD"'/payloads" \
    ./benchmark > stdout.txt 2>&1
  '
```

### D. 单独只跑 Go

如果你只想看 Go mirror benchmark，一般不需要任何 SDK。它会自己在单进程里起 server
和 client。

```bash
cd /Users/lipeisheng/code/cangjie/cangjie/cangjie_runtime_MSGC/runtime/MSGC_RESULT/Result/case27_http_complex_json

OUT_DIR="$PWD/result/manual_go_$(date +%Y%m%d_%H%M%S)"
mkdir -p "$OUT_DIR"

(
  cd go_benchmark
  go build -o "$OUT_DIR/benchmark_go" .
)

GODEBUG=gctrace=1 \
CASE27_PAYLOAD=10K \
CASE27_CONCURRENCY=8 \
CASE27_DURATION_MS=60000 \
CASE27_PAYLOAD_DIR="$PWD/payloads" \
"$OUT_DIR/benchmark_go" > "$OUT_DIR/stdout.txt" 2> "$OUT_DIR/gc.log"
```

如果要跑 Go 的 `async` 模式：

```bash
GODEBUG=gctrace=1 \
CASE27_CLIENT_MODE=async \
CASE27_SEND_INTERVAL_MS=1 \
CASE27_MAX_INFLIGHT=32 \
CASE27_PAYLOAD=10K \
CASE27_CONCURRENCY=8 \
CASE27_DURATION_MS=60000 \
CASE27_PAYLOAD_DIR="$PWD/payloads" \
"$OUT_DIR/benchmark_go" > "$OUT_DIR/stdout.txt" 2> "$OUT_DIR/gc.log"
```

### E. 单跑时看什么结果

无论单跑哪一侧，最重要的都是这两个文件：

- `stdout.txt`
  关注 `[PROGRESS]`、`[RESULT]`、`[DONE]`
- `gc.log`
  - `MSGC` / `CopyGC`：runtime GC 日志
  - `Go`：`GODEBUG=gctrace=1` 的 gctrace 输出

如果你想把单跑结果也接进统一报告，最简单的做法是把单跑目录整理成和
`run_compare.sh` 产物一致的结构，然后调用：

```bash
python3 ./script/compare_results.py \
  --run CopyGC=/path/to/copygc_run \
  --run MSGC=/path/to/msgc_run \
  --run Go=/path/to/go_run \
  --output /path/to/compare_report.md
```

## 目录结构

```
case27_http_complex_json/
├── benchmark.cj                       # 主程序（server + 并发 client）
├── go_benchmark/                      # Go mirror benchmark（同进程 server + client）
│   ├── go.mod
│   ├── main.go
│   └── main_test.go
├── script/                            # 本目录自带的对比脚本，可直接整体拷走执行
│   ├── compare_results.py
│   └── test_compare_results.py
├── payloads/
│   ├── gen.py                         # 生成 4 个 payload 的脚本（可重跑）
│   ├── 1K.json   (~1.4 KB)
│   ├── 10K.json  (~9.3 KB)
│   ├── 100K.json (~97 KB)
│   └── 1M.json   (~999 KB)
├── run_compare.sh                     # MSGC vs CopyGC 一键对比
├── bak/                               # 历史/辅助脚本，仍可在当前目录下直接调用
│   ├── run_msgc_until_stall.sh        # 单独跑 MSGC，直到抓到 stall（可自动 lldb 抓栈）
│   ├── run_sample_until_stall.sh      # 反复跑 sample，直到某一轮撞上 stall
│   ├── run_with_sample.sh             # 对单次运行做 sample 采样
│   ├── go_client_sync.go              # Go sync client（外部对照 client）
│   ├── go_client_sync_test.go         # Go client 最小测试
│   ├── verify_async_mode.sh           # async 模式验证
│   └── verify_go_sync_client.sh       # Cangjie server + Go sync client 一键验证
├── README.md                          # 本文档
└── result/                            # 每次运行的产物
    └── <ts>_<label>_c<N>_p<SIZE>/
        ├── benchmark.stdout.txt
        ├── gc.log
        ├── rss.csv     # headerless: ts,rss_kb,cpu_pct
        └── run_meta.txt
```

## 环境变量

`benchmark.cj` 在运行时读取的环境变量：

| 变量 | 默认值 | 说明 |
| --- | --- | --- |
| `CASE27_PAYLOAD` | `10K` | `1K` / `10K` / `100K` / `1M` |
| `CASE27_CONCURRENCY` | `8` | client worker 数 |
| `CASE27_DURATION_MS` | `60000` | 压测持续时间（ms）；`<=0` 表示不设时间上限 |
| `CASE27_PAYLOAD_DIR` | `./payloads` | payload 文件目录 |
| `CASE27_SERVER_PORT` | `18125` | server 监听端口 |
| `CASE27_CLIENT_TIMEOUT_MS` | `5000` | HTTP client 读写超时（ms） |
| `CASE27_ROLE` | `both` | `both` / `server`；`server` 时只启动 benchmark 自己的 server，等待外部 client |
| `CASE27_CLIENT_MODE` | `sync` | `sync` / `async`；`async` 为单 sender + inflight 限流实验模式 |
| `CASE27_SEND_INTERVAL_MS` | `1` | `async` 模式下 sender 每次尝试发射的间隔（ms） |
| `CASE27_MAX_INFLIGHT` | `32` | `async` 模式下允许同时在途的请求上限 |

补充说明：

- `sync` 模式下，`CASE27_CONCURRENCY` 直接决定启动多少个 worker。
- `async` 模式下，当前实现是**单 sender + `CASE27_MAX_INFLIGHT` 个 slot client**；此时真正决定并发深度的是 `CASE27_MAX_INFLIGHT`，不是 `CASE27_CONCURRENCY`。
- `async` 模式更适合观察“持续发请求时会不会把系统拖进 timeout / hang / stall”，`sync` 模式更适合对照传统“每个 worker 等响应后再发下一个”的行为。

仓颉 runtime 控制变量（在脚本中暴露）：

| 变量 | 默认 | 说明 |
| --- | --- | --- |
| `HEAP_SIZE` | `12gb` | `cjHeapSize`（仅 CopyGC 生效，MSGC 自动堆增长） |
| `PROCS` | `<unset>` | 仅在显式设置时透传 `cjProcessorNum`；默认不限制 Cangjie 侧 processor 数 |
| `MSGC_HEAP_GROWTH` | `1` | 仅传给 MSGC 侧的 `cjHeapGrowth`；设置后 `gc.log` 会显示 `Heap growth: N+1`；设为 `2` 时 threshold = live × 3，堆容忍更高分配再触发 GC |
| `MSGC_SCAVENGE_GROWTH` | `10` | 仅传给 MSGC 侧的 `cjScavengeGrowth`；当前 README 与 `run_compare.sh` 默认都使用 `10`；语义是 `scavengeThreshold = heapThreshold × cjScavengeGrowth` |

## 用法

### 1. 重新生成 payload（仅在改 `gen.py` 后需要）

```bash
cd payloads && python3 gen.py
```

### 当前对比数据来源

当前这份 case27 文档默认按下面两套 SDK 做 Cangjie 对比，`run_compare.sh`
会在它们之外再追加一侧 Go mirror benchmark：

```bash
# MSGC (d619)
MSGC_SDK_ROOT=/Users/lipeisheng/code/cangjie/cangjie-pkg/self/dev260630/cangjie
MSGC_STDX_ROOT=/Users/lipeisheng/code/cangjie/cangjie-pkg/self/dev260630/darwin_aarch64_cjnative

# CopyGC
COPYGC_SDK_ROOT=/Users/lipeisheng/code/cangjie/cangjie-pkg/dev/20260507/cangjie
COPYGC_STDX_ROOT=/Users/lipeisheng/code/cangjie/cangjie-pkg/dev/20260507/darwin_aarch64_cjnative
```

说明：

- `*_SDK_ROOT` 指向包含 `envsetup.sh` 的 SDK 根目录。
- `*_STDX_ROOT` 指向平台根目录；脚本内部会自动使用其中的 `dynamic/stdx` 目录。

### 2. 单独跑 MSGC，直到抓到 stall

```bash
# 默认就是 MSGC + CASE27_DURATION_MS=0（无限运行），直到监控到 stall
MSGC_SDK_ROOT=/Users/lipeisheng/code/cangjie/cangjie-pkg/self/dev260630/cangjie \
MSGC_STDX_ROOT=/Users/lipeisheng/code/cangjie/cangjie-pkg/self/dev260630/darwin_aarch64_cjnative \
CASE27_PAYLOAD=10K CASE27_CONCURRENCY=8 \
MSGC_HEAP_GROWTH=1 MSGC_SCAVENGE_GROWTH=10 \
./bak/run_msgc_until_stall.sh

# 调整 stall 判定阈值：连续 3 个 2s 窗口都低于 20 req 才抓栈
MSGC_SDK_ROOT=/Users/lipeisheng/code/cangjie/cangjie-pkg/self/dev260630/cangjie \
MSGC_STDX_ROOT=/Users/lipeisheng/code/cangjie/cangjie-pkg/self/dev260630/darwin_aarch64_cjnative \
CASE27_PAYLOAD=10K CASE27_CONCURRENCY=8 \
STALL_LAST2S_THRESHOLD=20 STALL_CONSECUTIVE_WINDOWS=3 \
MSGC_HEAP_GROWTH=1 MSGC_SCAVENGE_GROWTH=10 \
./bak/run_msgc_until_stall.sh

# 仅抓现场，不自动 kill benchmark
MSGC_SDK_ROOT=/Users/lipeisheng/code/cangjie/cangjie-pkg/self/dev260630/cangjie \
MSGC_STDX_ROOT=/Users/lipeisheng/code/cangjie/cangjie-pkg/self/dev260630/darwin_aarch64_cjnative \
CASE27_PAYLOAD=10K CASE27_CONCURRENCY=8 \
KILL_AFTER_CAPTURE=0 \
MSGC_HEAP_GROWTH=1 MSGC_SCAVENGE_GROWTH=10 \
./bak/run_msgc_until_stall.sh
```

输出落到 `result/until_stall_loop/run_<ts>/`，关键文件包括：

- `stdout.txt`：benchmark 原始输出
- `gc.log`：runtime GC 日志
- `progress_tail.txt`：触发 stall 判断时最后一段 `PROGRESS`
- `ps_threads.txt`：抓栈前的线程视图
- `lldb_threads.txt`：`thread list` + `thread backtrace all`

说明：

- `bak/run_msgc_until_stall.sh` 默认把 `CASE27_DURATION_MS` 设为 `0`，因此 benchmark 会一直跑，直到脚本判断进入 stall。
- 默认判定条件是：连续 `2` 个 `PROGRESS` 窗口满足 `+last2s < 20`。
- 默认抓到后会自动 `lldb attach`，然后 `kill` benchmark。若想保留现场继续观察，可设 `KILL_AFTER_CAPTURE=0`。

### 3. 对比 MSGC vs CopyGC vs Go

```bash
# MSGC(d619) vs CopyGC(20260507)
MSGC_SDK_ROOT=/Users/lipeisheng/code/cangjie/cangjie-pkg/self/dev260630/cangjie \
MSGC_STDX_ROOT=/Users/lipeisheng/code/cangjie/cangjie-pkg/self/dev260630/darwin_aarch64_cjnative \
COPYGC_SDK_ROOT=/Users/lipeisheng/code/cangjie/cangjie-pkg/dev/20260507/cangjie \
COPYGC_STDX_ROOT=/Users/lipeisheng/code/cangjie/cangjie-pkg/dev/20260507/darwin_aarch64_cjnative \
CASE27_PAYLOAD=10K CASE27_CONCURRENCY=8 CASE27_DURATION_MS=60000 \
HEAP_SIZE=12gb \
MSGC_HEAP_GROWTH=1 MSGC_SCAVENGE_GROWTH=10 \
./run_compare.sh
```

输出到 `result/<ts>_<tag>_c<N>_p<SIZE>_compare/`，结构：

```
<compare_dir>/
├── copygc/{benchmark, stdout.txt, gc.log, rss.csv, run_metadata.env, ...}
├── msgc/{...}
├── go/{benchmark_go, stdout.txt, gc.log, rss.csv, run_metadata.env, ...}
├── compare_report.md
├── compare_rss_cpu_timeline.png     # RSS / CPU 时序曲线
├── compare_summary.png              # 吞吐 + GC 时间 / GC 次数柱状图
└── compare_gc_per_cycle.png         # 单次 GC 用时分布
```

对比脚本默认调用当前目录自带的
`./script/compare_results.py`，因此把整个 `case27_http_complex_json/`
目录拷到别处后仍可直接执行 `./run_compare.sh`。它现在已支持：

- `--run CopyGC=... --run MSGC=... --run Go=...` 这种多侧输入
- `rss.csv` 两种 schema
- Go `GODEBUG=gctrace=1` 产物解析

### 4. 验证 async client 实验模式

```bash
MSGC_SDK_ROOT=/Users/lipeisheng/code/cangjie/cangjie-pkg/self/dev260630/cangjie \
MSGC_STDX_ROOT=/Users/lipeisheng/code/cangjie/cangjie-pkg/self/dev260630/darwin_aarch64_cjnative \
CASE27_CLIENT_MODE=async CASE27_SEND_INTERVAL_MS=1 CASE27_MAX_INFLIGHT=32 \
./bak/verify_async_mode.sh

# 跑更长的 async probe（例如 60s）
MSGC_SDK_ROOT=/Users/lipeisheng/code/cangjie/cangjie-pkg/self/dev260630/cangjie \
MSGC_STDX_ROOT=/Users/lipeisheng/code/cangjie/cangjie-pkg/self/dev260630/darwin_aarch64_cjnative \
CASE27_PAYLOAD=10K CASE27_CONCURRENCY=8 \
CASE27_CLIENT_MODE=async CASE27_SEND_INTERVAL_MS=1 CASE27_MAX_INFLIGHT=32 \
VERIFY_DURATION_MS=60000 VERIFY_TIMEOUT_SEC=95 \
./bak/verify_async_mode.sh
```

`async` 模式下 benchmark 会在 `PROGRESS` 中额外打印 `inflight=<n>`，用于观察“不等响应、仅按 sender tick + inflight cap 发射请求”时的吞吐和挂起形态。
`bak/verify_async_mode.sh` 还会检查 benchmark 是否在 watchdog 时间内正常退出，因此它除了能看 `fail` / `inflight`，也能顺手暴露 shutdown hang。

### 5. 使用 Go sync client 打 Cangjie server

这个入口用于把“client 实现问题”和“Cangjie server / stdx server 路径问题”拆开看：

- Cangjie 侧只起 `CASE27_ROLE=server`
- Go 侧起 8 个 goroutine
- 每个 goroutine 都严格按 `request -> wait response -> next request` 的 sync 模型工作

快速 smoke：

```bash
MSGC_SDK_ROOT=/Users/lipeisheng/code/cangjie/cangjie-pkg/self/dev260630/cangjie \
MSGC_STDX_ROOT=/Users/lipeisheng/code/cangjie/cangjie-pkg/self/dev260630/darwin_aarch64_cjnative \
CASE27_PAYLOAD=10K CASE27_CONCURRENCY=8 \
VERIFY_DURATION_MS=5000 VERIFY_TIMEOUT_SEC=30 \
./bak/verify_go_sync_client.sh
```

60s 长跑：

```bash
MSGC_SDK_ROOT=/Users/lipeisheng/code/cangjie/cangjie-pkg/self/dev260630/cangjie \
MSGC_STDX_ROOT=/Users/lipeisheng/code/cangjie/cangjie-pkg/self/dev260630/darwin_aarch64_cjnative \
CASE27_PAYLOAD=10K CASE27_CONCURRENCY=8 \
VERIFY_DURATION_MS=60000 VERIFY_TIMEOUT_SEC=95 \
./bak/verify_go_sync_client.sh
```

脚本产物会落到 `result/verify_go_sync_client.*`，关键文件：

- `server.stdout.txt`：Cangjie server 侧请求日志
- `server.gc.log`：runtime / GC 日志
- `go_client.stdout.txt`：Go client 的 `PROGRESS` / `RESULT` / timeout 日志

Go client 自己支持的环境变量：

| 变量 | 默认值 | 说明 |
| --- | --- | --- |
| `CASE27_PAYLOAD` | `10K` | payload 名字，读取 `payloads/<name>.json` |
| `CASE27_CONCURRENCY` | `8` | goroutine 数 |
| `CASE27_DURATION_MS` | `60000` | 运行时长 |
| `CASE27_SERVER_PORT` | `18125` | server 端口 |
| `CASE27_SERVER_URL` | 自动拼 `http://127.0.0.1:<port>/process` | 可直接覆盖完整 URL |
| `CASE27_CLIENT_TIMEOUT_MS` | `5000` | 单请求超时 |
| `CASE27_PAYLOAD_DIR` | `./payloads` | payload 目录 |

### 正确执行方式

要得到有效的 case27 对比数据，必须在**允许本地回环网络和 `ps` 采样**的本地终端里执行。
也就是说，运行环境需要同时满足：

- 允许进程在 `127.0.0.1:<port>` 上 `bind/listen/connect`
- 允许 `ps -o rss=,%cpu= -p <pid>` 读取被测进程的 RSS/CPU

推荐直接在本机交互式终端执行下面这条命令：

```bash
cd /Users/lipeisheng/code/cangjie/cangjie/cangjie_runtime_MSGC/runtime/MSGC_RESULT/Result/case27_http_complex_json

MSGC_SDK_ROOT=/Users/lipeisheng/code/cangjie/cangjie-pkg/self/dev260630/cangjie \
MSGC_STDX_ROOT=/Users/lipeisheng/code/cangjie/cangjie-pkg/self/dev260630/darwin_aarch64_cjnative \
COPYGC_SDK_ROOT=/Users/lipeisheng/code/cangjie/cangjie-pkg/dev/20260507/cangjie \
COPYGC_STDX_ROOT=/Users/lipeisheng/code/cangjie/cangjie-pkg/dev/20260507/darwin_aarch64_cjnative \
CASE27_PAYLOAD=10K CASE27_CONCURRENCY=8 CASE27_DURATION_MS=60000 \
HEAP_SIZE=12gb \
MSGC_HEAP_GROWTH=1 MSGC_SCAVENGE_GROWTH=10 ./run_compare.sh
```

若执行正确，预期现象应包括：

- `copygc/stdout.txt` 与 `msgc/stdout.txt` 中 `Warmup done` 正常结束
- `[RESULT]` 中 `ok > 0` 且通常 `fail = 0`
- `copygc/rss.csv` 与 `msgc/rss.csv` 都是非空文件
- `compare_report.md` 中 `RSS 内存` 和 `CPU 使用率` 不是 `0.00 / N/A`

## HEAP_SIZE 与 MSGC 自动堆增长

对比脚本的 `HEAP_SIZE` 环境变量（默认 12gb）以 `cjHeapSize` 的形式传递给被测试进程。
**该设置对 CopyGC 生效**（CopyGC 预分配固定堆），**但对 MSGC 不生效**——MSGC 使用
**Mark-Sweep 算法**，堆会自动按需增长，无需手动设置堆大小。

MSGC 的初始堆较小，分配压力上升时会自动触发 GC。其堆大小增长过程与 GC 周期耦合：
每轮 GC 后如果存活对象 + 分配压力超过当前阈值，堆会自然扩展，直到达到系统内存上限。
因此对 MSGC 传 `cjHeapSize` 不会限制堆的上限或初始大小。

> 注意：当前脚本中 MSGC 仍会收到 `HEAP_SIZE` 环境变量，但这仅用于统一运行配置，
> 不影响 MSGC 的实际堆行为。CopyGC 需要足够大的堆以避免频繁 GC 影响吞吐，
> 这也是默认 12gb 的由来。

## Server 端 read-modify-write 字段清单

每次请求 server 都会对整棵 JSON 树做以下修改后再序列化：

1. `counters.in` / `counters.ok` 各 +1
2. 根对象新增 `processed_seq` (Int)、`server_ok` (Bool)
3. `events[i].seq` += 1
4. `events[i].score` *= 2.0  (Float 路径)
5. `events[i].user.attrs.credits` *= 2.0  (4 层嵌套)
6. `events[i].custom.product_ids.add(Int)`  (Array 追加)
7. `events[i]` 新增 `server_marker` (String)

Client 收到响应后再反序列化并断言 `server_ok == true`，确保两端的
JSON 编解码路径都被实际走通。

## 单组运行策略说明

为了让一次 run 的稳态指标（吞吐、RSS 时序、GC 行为）干净可读，case27
**一次只跑一组配置**。要扫多组时，通过外部 shell 循环调用你选择的 case27 运行入口：

```bash
for p in 1K 10K 100K 1M; do
    for c in 4 8 16; do
        CASE27_PAYLOAD=$p CASE27_CONCURRENCY=$c CASE27_DURATION_MS=30000 \
        # 使用你选择的 case27 运行入口执行单组测试
    done
done
```

## 注意事项

- `case27` 依赖本地回环网络：benchmark 会在同一进程内启动 HTTP Server，并让
  Client 通过 `127.0.0.1:<port>` 发请求。因此运行环境必须允许本地
  `bind/connect`。若环境限制本地监听或回环连接，典型现象是：
  - `stdout.txt` 开头出现 `Failed to bind ... Operation not permitted`
  - warmup 全部失败
  - `[RESULT]` 中 `ok=0`、`fail>0`
- `bak/run_msgc_until_stall.sh` 触发抓栈时默认使用 `xcrun lldb -p <pid>`。
  如果 `lldb_threads.txt` 里出现
  `Not allowed to attach to process`，说明是 macOS 调试权限问题，不是脚本语法问题。
  需要从有调试权限的本地终端执行，必要时在系统设置里给对应终端/开发者工具放行。
- `RSS/CPU` 对比图依赖 `run_compare.sh` 中的 `ps -o rss=,%cpu= -p <pid>` 采样。
  如果运行环境限制 `ps`，则 `rss.csv` 会是空文件，`compare_report.md` 中的
  `RSS 内存` 与 `CPU 使用率` 会显示为 `0.00` / `N/A`，此时不能据此解读内存和 CPU 数据。
- macOS arm64 上替换 dylib 后务必 `codesign -f -s -`，否则进程会被
  SIGKILL（参见根目录 `AGENTS.md`）。本 case 不替换 dylib，但若你为了
  验证新 runtime 而改 SDK 中的 `libcangjie-runtime.dylib`，需要重新签名。
- `run_compare_*.sh` 不依赖 `rss_monitor.py` 的 pty 包装；d602 stdx 在
  pty 下 HTTP 有偶发 read timeout，case27 沿用 case8 的 bare-run 模式。
- d602 SDK 的 stdx 当前已被 d509 stdx full overlay 替换（备份在
  `<stdx>.d602-orig-backup/`），所以 d602 路径里实际使用的是 d509 stdx。
