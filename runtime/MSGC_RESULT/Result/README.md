# MSGC_RESULT/Result — 用例测试与对比指南

## 目录结构

```
Result/
├── README.md                   # 本文件
├── seed/                       # MSGC 参数预设（seed 文件）
│   ├── README.md               # 参数说明与对比结论
│   ├── seed1.env               # seed1: 保守（benchmark2 基线）
│   ├── seed2.env               # seed2: 激进（RSS -42%）
│   ├── seed3.env               # seed3: 超激进（RSS -52%）★ 推荐
│   └── seed4.env               # seed4: 极端（RSS -67%）
├── script/                     # 运行与分析脚本
│   ├── run_case.sh             # 构建 runtime → 替换 → 编译用例 → 运行采集
│   ├── analyze_case.py         # 解析单次运行的 GC 日志 + RSS/CPU，生成报告
│   └── compare_results.py      # 对比同一用例的两次运行（CopyGC vs MSGC）
├── gc_benchmark.cj             # 原始完整 benchmark
├── case1_baseline/             # 用例 1: 基准 (无 idle)
│   ├── README.md
│   ├── benchmark.cj
│   └── result/                 # 每次运行产物（自动创建）
│       └── 20260406_143000_msgc/
├── case2_50x_pressure/         # 用例 2: 50 倍压力
├── case3_half_alive/           # 用例 3: 半存活半临时
├── case4_read_heavy/           # 用例 4: 读密集 10:1
├── case5_mixed_objects/        # 用例 5: 混合对象
├── case6_10g_scale/            # 用例 6: 10GB 大规模
├── case7_low_pressure_neutral/ # 用例 7: 小压力（assist ON/OFF 应接近）
├── case8_high_pressure_convergence/ # 用例 8: 高压力多协程（assist 应加速收敛）
├── case8_read_mostly_low_alloc/ # 用例 8-扩展: 低分配、高读取
├── case9_black_publish_closure/ # 用例 9-回归: 预发布子图在发布后是否仍被完整闭包
├── case10_init_raw_store_child/ # 用例 10: 定向验证 init raw-store child 漏扫描窗口
├── case21_small_object_object_struct/ # 用例 21: 纯小对象对象/struct 创建与读取
├── case22_large_object_containers/ # 用例 22: 大对象 + List/Map 创建与读取
├── case23_concurrent_mixed_alloc_read/ # 用例 23: 高并发混合小/大对象创建与读取
```

---

## 1. 构建 Runtime

### CopyGC 模式（默认）

```bash
cd /Users/lipeisheng/code/cangjie/cangjie/cangjie_runtime/runtime
python3 build.py build --gc-mode copygc
```

### MarkSweep (MSGC) 模式

```bash
cd /Users/lipeisheng/code/cangjie/cangjie/cangjie_runtime/runtime
python3 build.py build --gc-mode marksweep
```

构建产物位于：

```
output/temp/lib/aarch64_Release/libcangjie-runtime.dylib
```

---

## 2. 替换 SDK 中的 dylib

```bash
SDK_LIB=/Users/lipeisheng/code/cangjie/cangjie-pkg/dev/20260421/cangjie/runtime/lib/darwin_aarch64_cjnative

# 替换
cp output/temp/lib/aarch64_Release/libcangjie-runtime.dylib "$SDK_LIB/libcangjie-runtime.dylib"

# macOS arm64 必须重签名
codesign -f -s - "$SDK_LIB/libcangjie-runtime.dylib"
```

恢复原始版本：

```bash
cp "$SDK_LIB/libcangjie-runtime.dylib.origin" "$SDK_LIB/libcangjie-runtime.dylib"
codesign -f -s - "$SDK_LIB/libcangjie-runtime.dylib"
```

> **关键约束**：替换 dylib 后必须重新编译 `.cj` 文件，不可复用旧二进制。

---

## 3. Source 编译环境

```bash
source /Users/lipeisheng/code/cangjie/cangjie-pkg/dev/20260421/cangjie/envsetup.sh
```

---

## 4. 编译并运行用例

### 手动方式

```bash
# 进入用例目录
cd MSGC_RESULT/Result/case1_baseline

# 编译
cjc benchmark.cj -o benchmark

# 运行（带 GC 日志）
MRT_REPORT=./gc.log cjHeapSize=256mb ./benchmark > stdout.txt 2>&1
```

### 常见失败：`env` 再包一层导致 dylib 启动失败

本次实际踩到的失败模式是：已经在子 shell 里 `source envsetup.sh` 了，但运行 benchmark 时又写成了下面这种形式：

```bash
env MRT_REPORT=./gc.log CJ_MS_BG_RECLAIM=0 cjHeapSize=6gb cjProcessorNum=6 ./benchmark
```

在当前 SDK / macOS arm64 环境下，这会导致运行时加载路径失效，典型报错是：

```text
dyld: Library not loaded: @rpath/libcangjie-runtime.dylib
Reason: no LC_RPATH's found
```

症状上通常表现为：

- 进程长时间停在异常状态，看起来像“卡住”
- `stdout` 为空
- `time` 文件没有写出最终结果
- `stderr` 里是上面的 dyld 报错

正确做法是：先 `source envsetup.sh`，然后直接用前缀环境变量启动 benchmark，不要再额外套一层 `env ... ./benchmark`。

```bash
source /Users/lipeisheng/code/cangjie/cangjie-pkg/dev/20260421/cangjie/envsetup.sh

cd MSGC_RESULT/Result/case_mid/case

TIMEFORMAT="%3R"
{ time MRT_REPORT=./gc.log CJ_MS_BG_RECLAIM=0 cjHeapSize=6gb cjProcessorNum=6 ./bench_p1_single_gate \
  > stdout.txt 2> stderr.txt; } 2> time.txt
```

如果需要隔离环境，推荐用下面这个模板：先进入一个干净 shell，在这个 shell 里 `source envsetup.sh`，后续执行 benchmark 时仍然直接写“前缀环境变量 + 可执行文件”。

```bash
env -i HOME=$HOME TERM=$TERM PATH=/usr/bin:/bin:/usr/sbin:/sbin:/opt/homebrew/bin bash -c '
source /Users/lipeisheng/code/cangjie/cangjie-pkg/dev/20260421/cangjie/envsetup.sh >/dev/null 2>&1
cd /Users/lipeisheng/code/cangjie/cangjie/cangjie_runtime_MSGC/runtime/MSGC_RESULT/Result/case_mid/case || exit 1
TIMEFORMAT="%3R"
{ time MRT_REPORT="$PWD/gc.log" CJ_MS_BG_RECLAIM=0 cjHeapSize=6gb cjProcessorNum=6 ./bench_p1_single_gate \
  > stdout.txt 2> stderr.txt; } 2> time.txt
'
```

关键点只有一条：`envsetup.sh` 设置出来的运行时库路径必须保留到 benchmark 启动那一刻，不能在最后一跳把启动方式改成会破坏该环境的写法。

### 使用脚本（推荐）

```bash
# 在 runtime 根目录执行

# 运行单个用例（MSGC 模式）
bash MSGC_RESULT/Result/script/run_case.sh \
  --case case1_baseline \
  --gc-mode marksweep \
  --tag my_test_run

# 运行单个用例（CopyGC 模式）
bash MSGC_RESULT/Result/script/run_case.sh \
  --case case1_baseline \
  --gc-mode copygc \
  --tag my_test_run

# CopyGC vs MSGC 完整对比
bash MSGC_RESULT/Result/script/run_case.sh \
  --case case1_baseline \
  --gc-mode compare \
  --tag baseline_compare

# CopyGC vs MSGC 对比，MSGC 使用 seed3 参数（推荐）
bash MSGC_RESULT/Result/script/run_case.sh \
  --case case1_baseline \
  --gc-mode compare \
  --seed seed3 \
  --tag seed3_compare

# 跳过构建（已构建好 runtime）
bash MSGC_RESULT/Result/script/run_case.sh \
  --case case1_baseline \
  --gc-mode marksweep \
  --tag quick_run \
  --skip-build

# 自定义堆大小
bash MSGC_RESULT/Result/script/run_case.sh \
  --case case6_10g_scale \
  --gc-mode compare \
  --tag scale_test \
  --heap-size 12gb

# 传入额外环境变量
bash MSGC_RESULT/Result/script/run_case.sh \
  --case case1_baseline \
  --gc-mode marksweep \
  --tag tuned \
  --env cjGCThreshold=32mb \
  --env cjGCInterval=150ms

# case7: 小压力场景，assist ON/OFF 对比
bash MSGC_RESULT/Result/script/run_case.sh \
  --case case7_low_pressure_neutral \
  --gc-mode marksweep \
  --seed seed3 \
  --tag case7_on

bash MSGC_RESULT/Result/script/run_case.sh \
  --case case7_low_pressure_neutral \
  --gc-mode marksweep \
  --seed seed3 \
  --env CJ_MS_ASSIST_DISABLE=1 \
  --tag case7_off

# case8: 高压力多协程场景，assist ON/OFF 对比
bash MSGC_RESULT/Result/script/run_case.sh \
  --case case8_high_pressure_convergence \
  --gc-mode marksweep \
  --seed seed3 \
  --heap-size 512mb \
  --tag case8_on

bash MSGC_RESULT/Result/script/run_case.sh \
  --case case8_high_pressure_convergence \
  --gc-mode marksweep \
  --seed seed3 \
  --heap-size 512mb \
  --env CJ_MS_ASSIST_DISABLE=1 \
  --tag case8_off

# case8-扩展: 低分配、高读取，直接对比 CopyGC vs MSGC
bash MSGC_RESULT/Result/script/run_case.sh \
  --case case8_read_mostly_low_alloc \
  --gc-mode compare \
  --tag case8_read_mostly_low_alloc
```

---

## 5. 结果产物

每次运行会在用例目录下创建带时间戳的结果子目录：

```
case1_baseline/result/20260406_143000_msgc/
├── run_metadata.env          # 运行参数快照
├── benchmark.cj.snapshot     # 源码快照
├── stdout.txt                # benchmark 标准输出
├── rss.csv                   # RSS/CPU 采样 (wall_time_iso,elapsed_ms,rss_kb,rss_mb,cpu_pct)
├── gc.log                    # GC 周期日志 (MRT_REPORT)
├── bench_events.csv          # [BENCH] 事件时间戳
├── gc_cycles.csv             # 解析后的 GC 周期汇总
└── report.md                 # 自动生成的分析报告
```

对比模式产物：

```
case1_baseline/result/20260406_143000_compare/
├── copygc/
│   ├── stdout.txt
│   ├── rss.csv
│   ├── gc.log
│   ├── bench_events.csv
│   ├── gc_cycles.csv
│   └── report.md
├── msgc/
│   ├── stdout.txt
│   ├── rss.csv
│   ├── gc.log
│   ├── bench_events.csv
│   ├── gc_cycles.csv
│   └── report.md
├── compare_report.md         # 对比报告
└── run_metadata.env
```

---

## 6. 分析与对比

### 单次运行分析

```bash
python3 MSGC_RESULT/Result/script/analyze_case.py \
  --results-dir MSGC_RESULT/Result/case1_baseline/result/20260406_143000_msgc
```

### 两次运行对比

```bash
python3 MSGC_RESULT/Result/script/compare_results.py \
  --left MSGC_RESULT/Result/case1_baseline/result/20260406_143000_copygc \
  --right MSGC_RESULT/Result/case1_baseline/result/20260406_143000_msgc \
  --left-label CopyGC \
  --right-label MSGC
```

---

## 7. 各用例推荐堆大小

| 用例 | 推荐堆大小 | 说明 |
|------|-----------|------|
| case1_baseline | 256mb | 标准基准 |
| case2_50x_pressure | 4gb | 50 倍压力 |
| case3_half_alive | 2gb | 500MB 存活 + 500MB 临时 |
| case4_read_heavy | 512mb | 读密集型 |
| case5_mixed_objects | 1gb | 混合对象 |
| case6_10g_scale | 12gb | 10GB 大规模 |
| case7_low_pressure_neutral | 256mb | 小压力，验证 assist ON/OFF 影响接近 |
| case8_high_pressure_convergence | 512mb | 大堆+多协程 rewiring，验证 assist 加速收敛 |

---

## 8. Seed 参数预设

Seed 文件存放在 `seed/` 目录下，每个 `.env` 文件包含一组 MSGC 调优参数。通过 `--seed` 选项传入时，这些参数**仅作用于 MSGC 运行阶段**，CopyGC 阶段不受影响。

> **推荐使用 seed3**：RSS 均值比 CopyGC 低 52%，峰值低 50%，吞吐量代价可控（+17%）。
> 详细参数说明和对比结论见 [`seed/README.md`](seed/README.md)。

### 当前可用 Seed

| Seed | 文件 | 策略 | RSS vs CopyGC |
|------|------|------|---------------|
| seed1 | `seed/seed1.env` | 保守 | +30%（不推荐） |
| seed2 | `seed/seed2.env` | 激进 | **-42%** |
| **seed3** | **`seed/seed3.env`** | **超激进（推荐）** | **-52%** |
| seed4 | `seed/seed4.env` | 极端 | -67%（CPU 较高） |

### seed3 参数详情（推荐）

```
cjGCThreshold=4mb
cjGCInterval=20ms
CJ_MS_HEU_READY_WATERMARK=1
CJ_MS_HEU_REQUEST_COOLDOWN=2ms
CJ_MS_EMPTY_CACHE_LOW_FLOOR_REGIONS=2
CJ_MS_EMPTY_CACHE_LOW_CAP_REGIONS=4
CJ_MS_EMPTY_CACHE_HIGH_FACTOR=1
CJ_MS_EMPTY_SCAVENGE_MAX_REGIONS_PER_SLICE=128
CJ_MS_BG_SWEEP_PRESSURE_REGIONS_PER_SLICE=256
CJ_MS_BG_SWEEP_MAX_REGIONS_PER_SLICE=128
CJ_MS_INLINE_RECLAIM=1
```

### 使用方式

```bash
# CopyGC vs MSGC(seed3) 对比（推荐）
bash MSGC_RESULT/Result/script/run_case.sh \
  --case case1_baseline \
  --gc-mode compare \
  --seed seed3 \
  --tag seed3_compare

# 仅 MSGC 单跑（使用 seed3）
bash MSGC_RESULT/Result/script/run_case.sh \
  --case case1_baseline \
  --gc-mode marksweep \
  --seed seed3 \
  --tag seed3_run

# 跳过构建（已构建好 runtime）
bash MSGC_RESULT/Result/script/run_case.sh \
  --case case1_baseline \
  --gc-mode marksweep \
  --seed seed3 \
  --tag quick_test \
  --skip-build
```

### 添加新 Seed

创建 `seed/<name>.env` 文件，格式为每行一个 `KEY=VALUE`，支持 `#` 注释。
