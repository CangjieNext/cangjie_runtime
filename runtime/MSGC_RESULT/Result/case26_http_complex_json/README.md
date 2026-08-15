# case26_http_complex_json — HTTP 复杂 JSON 并发读改写压测

## 场景

case26 在单个进程中先建立长期存活对象基线，再启动本地 HTTP server 和并发
client。每个请求执行完整 JSON 反序列化、字段读改写、重新序列化、响应端
反序列化与校验，用于观察高分配、高存活率下的 GC 行为。

仓颉、Go 和 Java 三个实现使用相同的 payload、server/client 模型和持久堆
布局。持久对象创建完成并打印 `Persistent heap baseline ready` 后，才开始
统计 HTTP 请求阶段。

## SDK 与执行入口

Linux 主入口是 `run_compare_linux.sh`，MSGC 默认 SDK 统一为：

```text
/home/lipeisheng/cangjie/cangjie_compiler_CMSGC/output/msgc
```

CopyGC 包按宿主架构自动选择：x86_64 优先使用
`/home/lipeisheng/cangjie-pkg/260723`，aarch64 优先使用
`/home/lipeisheng/cangjie-pkg/main/260723`，并检查对应架构的 stdx 与 runtime。

标准执行方式：

```bash
cd MSGC_RESULT/Result/case26_http_complex_json
bash ./run_compare_linux.sh
```

所有 SDK 路径仍可由 `MSGC_SDK_ROOT`、`MSGC_STDX_ROOT`、`COPYGC_PACKAGE_ROOT`、
`COPYGC_SDK_ROOT` 和 `COPYGC_STDX_ROOT` 覆盖。某个 SDK 或 Go/Java 工具缺失时，
Linux runner 只跳过对应实现，继续运行其他可用实现。macOS 的 `run_compare.sh` 保持使用
`cangjie_compiler_region_mark/output/msgc`，与 Linux SDK 路径分开。

`run_compare_portsafe.sh` 是低并发、便于端口受限环境排查的入口，默认
`payload=10K`、`concurrency=8`；主入口的默认负载见下节。

## Linux 主入口默认参数

| 参数 | 默认值 |
| --- | --- |
| payload | `100K` |
| concurrency | `1000` |
| duration | `60000ms` |
| `HEAP_SIZE` | `16gb` |
| `PROCS` | 未设置（运行时自动决定） |
| `MSGC_HEAP_GROWTH` | `1` |
| `MSGC_SCAVENGE_GROWTH` | `1.10` |
| `MSGC_MIN_GC_THRESHOLD` | `20mb` |
| client mode | `sync` |
| `CASE26_SEND_INTERVAL_MS` | `1` |
| `CASE26_MAX_INFLIGHT` | `32` |
| persistent bytes | `5368709120`（5 GiB） |
| persistent workers | `8` |
| fd soft limit | `65535` |

Go 默认开启并使用 `GODEBUG=gctrace=1`、`GO_GOMEMLIMIT=16GiB`。
Java 默认开启，堆上限随 `HEAP_SIZE` 设置（默认 `-Xmx16g`）。Linux 主入口会运行
所有依赖可用的实现；缺失项可通过 `COPYGC_ENABLE=0`、`MSGC_ENABLE=0`、
`GO_ENABLE=0`、`JAVA_ENABLE=0` 跳过。

## 常用命令

```bash
# 10 个并发、10K payload、10 个处理器
HEAP_SIZE=16gb PROCS=10 \
MSGC_HEAP_GROWTH=2 MSGC_SCAVENGE_GROWTH=2 \
CASE26_PAYLOAD=10K CASE26_CONCURRENCY=10 \
bash ./run_compare_linux.sh

# 小内存冒烟：缩小持久对象和时长
CASE26_PERSISTENT_BYTES=64mb CASE26_PERSISTENT_WORKERS=2 \
CASE26_DURATION_MS=1000 CASE26_CONCURRENCY=8 \
bash ./run_compare_linux.sh
```

## 环境变量

case26 的脚本、Cangjie benchmark、Go benchmark 和 Java benchmark 统一使用
`CASE26_*` 环境变量；不再读取旧用例前缀的环境变量。

Linux 主入口会在设置 `PROCS` 时同时限制 Cangjie、Go 和 Java：Cangjie 使用
`cjProcessorNum`，Go 使用 `GOMAXPROCS`，Java 使用
`-XX:ActiveProcessorCount` 并通过 `taskset -c 0-(P-1)` 设置硬 CPU 亲和性。
Java 默认使用 Java 25 runtime 和 Java 8 javac，Go 默认使用当前 shell 中解析到
的 Go 1.26；这些路径可分别通过 `JAVA_BIN`、`JAVAC_BIN` 和 `GO_BIN` 覆盖。
