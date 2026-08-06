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

CopyGC 默认 SDK 为：

```text
/home/lipeisheng/cangjie-pkg/main/260723/cangjie
```

标准执行方式：

```bash
cd MSGC_RESULT/Result/case26_http_complex_json
bash ./run_compare_linux.sh
```

所有 SDK 路径仍可由 `MSGC_SDK_ROOT`、`MSGC_STDX_ROOT`、`COPYGC_SDK_ROOT`
和 `COPYGC_STDX_ROOT` 覆盖。macOS 的 `run_compare.sh` 保持使用
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
| `CASE27_SEND_INTERVAL_MS` | `1` |
| `CASE27_MAX_INFLIGHT` | `32` |
| persistent bytes | `5368709120`（5 GiB） |
| persistent workers | `8` |
| fd soft limit | `65535` |

Go 默认开启并使用 `GODEBUG=gctrace=1`、`GO_GOMEMLIMIT=16GiB`。
Java 默认开启，堆上限随 `HEAP_SIZE` 设置（默认 `-Xmx16g`）。Linux 主入口
固定执行 MSGC 和 CopyGC；Go 与 Java 可通过 `GO_ENABLE=0`、`JAVA_ENABLE=0`
跳过。

## 常用命令

```bash
# 10 个并发、10K payload、10 个处理器
HEAP_SIZE=16gb PROCS=10 \
MSGC_HEAP_GROWTH=2 MSGC_SCAVENGE_GROWTH=2 \
CASE27_PAYLOAD=10K CASE27_CONCURRENCY=10 \
bash ./run_compare_linux.sh

# 小内存冒烟：缩小持久对象和时长
CASE27_PERSISTENT_BYTES=64mb CASE27_PERSISTENT_WORKERS=2 \
CASE27_DURATION_MS=1000 CASE27_CONCURRENCY=8 \
bash ./run_compare_linux.sh
```

## 环境变量兼容性

case26 的 benchmark 源码此前沿用了 case27 的环境变量接口，因而当前仍使用
`CASE27_PAYLOAD`、`CASE27_CONCURRENCY`、`CASE27_DURATION_MS` 等变量名。
这只是兼容接口；输出元数据、脚本标题和本文档均标识为 `case26`。不要把
case27 文档中的旧 SDK 路径或旧默认参数当作 case26 的执行配置。
