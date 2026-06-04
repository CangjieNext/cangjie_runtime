# case25_http_complex_json — HTTP 复杂 JSON 并发读改写压测

## 目标场景

在单进程内启动 HTTP Server，同时启动 N 个并发 HTTP Client，对同一 endpoint
持续发送同样 payload 的 POST 请求；server 端对请求体做完整 JSON
反序列化 → 读改写若干字段 → 重新序列化返回；client 端再反序列化响应并
校验 server 写入的 marker 字段。

与 `case8_http_json_serde` 的差异：

| 维度 | case8 | case25 |
| --- | --- | --- |
| Payload | 4 种规模在代码中硬编码 | 5 种规模（1K/10K/100K/1M/10M）外置为 JSON 文件 |
| 结构复杂度 | 中（嵌套 2-3 层） | 高（嵌套 ≥ 4 层、混合类型、异构数组） |
| Server 处理 | 解析 + 重序列化 + 构造小响应 | 解析 + **读改写多个字段** + 整树重序列化 |
| 客户端模型 | 单线程依次 4 个 phase | **N 并发 worker 同时压测同一 payload** |
| 一次运行 | 跑完全部 phase | **单次只跑一组 (concurrency × payload)** |

更适合复现 long-running 高分配 + 高存活率的稳态 GC 负载。

## 目录结构

```
case25_http_complex_json/
├── benchmark.cj                       # 主程序（server + 并发 client）
├── payloads/
│   ├── gen.py                         # 生成 5 个 payload 的脚本（可重跑）
│   ├── 1K.json   (~1.4 KB)
│   ├── 10K.json  (~9.3 KB)
│   ├── 100K.json (~97 KB)
│   ├── 1M.json   (~999 KB)
│   └── 10M.json  (~10 MB)
├── run.sh                             # 单 SDK 跑一组 (compile + run + 三件套)
├── run_compare_d602_vs_d507.sh        # MSGC(d602) vs CopyGC(d507) 一键对比
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
| `CASE25_PAYLOAD` | `10K` | `1K` / `10K` / `100K` / `1M` / `10M` |
| `CASE25_CONCURRENCY` | `8` | client worker 数 |
| `CASE25_DURATION_MS` | `60000` | 压测持续时间（ms） |
| `CASE25_PAYLOAD_DIR` | `./payloads` | payload 文件目录 |
| `CASE25_SERVER_PORT` | `18125` | server 监听端口 |

仓颉 runtime 控制变量（在 `run.sh` 中暴露）：

| 变量 | 默认 | 说明 |
| --- | --- | --- |
| `HEAP` | `512mb` | `cjHeapSize` |
| `PROC` | `6` | `cjProcessorNum`（M1 Pro P 核数） |

## 用法

### 1. 重新生成 payload（仅在改 `gen.py` 后需要）

```bash
cd payloads && python3 gen.py
```

### 2. 单 SDK 跑一组

```bash
# 默认: 用 d602 MSGC SDK，10K payload, 8 并发, 60s
./run.sh

# 自定义
CASE25_PAYLOAD=100K CASE25_CONCURRENCY=16 CASE25_DURATION_MS=120000 ./run.sh

# 换 CopyGC SDK
SDK_ROOT=/Users/lipeisheng/code/cangjie/cangjie-pkg/dev/20260507/cangjie \
STDX_DIR=/Users/lipeisheng/code/cangjie/cangjie-pkg/dev/20260507/darwin_aarch64_cjnative/dynamic/stdx \
CASE25_LABEL=copygc ./run.sh
```

每次输出落到 `result/<ts>_<label>_c<N>_p<SIZE>/`。

### 3. 对比 MSGC vs CopyGC

```bash
# 默认: 10K payload, 8 并发, 60s
./run_compare_d602_vs_d507.sh

# 自定义
CASE25_PAYLOAD=1M CASE25_CONCURRENCY=16 CASE25_DURATION_MS=120000 \
    ./run_compare_d602_vs_d507.sh
```

输出到 `result/<ts>_d602_vs_d507_c<N>_p<SIZE>_compare/`，结构：

```
<compare_dir>/
├── copygc/{benchmark, stdout.txt, gc.log, rss.csv, run_metadata.env, ...}
├── msgc/{...}
├── compare_report.md
├── compare_rss_cpu_timeline.png     # RSS / CPU 时序曲线
├── compare_summary.png              # 吞吐 + GC 时间 / GC 次数柱状图
└── compare_gc_per_cycle.png         # 单次 GC 用时分布
```

对比脚本调用的是仓库现有的
`MSGC_RESULT/Result/script/compare_results.py`（已支持 `rss.csv` 两种 schema）。

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

为了让一次 run 的稳态指标（吞吐、RSS 时序、GC 行为）干净可读，case25
**一次只跑一组配置**。要扫多组时通过外部 shell 循环调用 `run.sh`：

```bash
for p in 1K 10K 100K 1M 10M; do
    for c in 4 8 16; do
        CASE25_PAYLOAD=$p CASE25_CONCURRENCY=$c CASE25_DURATION_MS=30000 ./run.sh
    done
done
```

## 注意事项

- macOS arm64 上替换 dylib 后务必 `codesign -f -s -`，否则进程会被
  SIGKILL（参见根目录 `AGENTS.md`）。本 case 不替换 dylib，但若你为了
  验证新 runtime 而改 SDK 中的 `libcangjie-runtime.dylib`，需要重新签名。
- `run_compare_*.sh` 不依赖 `rss_monitor.py` 的 pty 包装；d602 stdx 在
  pty 下 HTTP 有偶发 read timeout，case25 沿用 case8 的 bare-run 模式。
- d602 SDK 的 stdx 当前已被 d509 stdx full overlay 替换（备份在
  `<stdx>.d602-orig-backup/`），所以 d602 路径里实际使用的是 d509 stdx。
