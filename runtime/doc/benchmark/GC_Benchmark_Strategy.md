# MS-GC vs CopyGC 对比测试策略

## 1. 目标

在相同测试用例（`gc_benchmark.cj`）下，对比 **MarkSweep GC** 和 **CopyGC** 在以下维度的表现差异：

| 维度 | 指标 |
|------|------|
| 功能完整性 | F1-F5 功能测试全部 PASS |
| 执行性能 | P1-P5 各项耗时、Total wall time |
| CPU 使用率 | 平均 CPU%、峰值 CPU% |
| 内存使用率 | 平均 RSS、峰值 RSS |

## 2. 测试环境

| 项目 | 值 |
|------|------|
| OS | macOS (Apple Silicon / aarch64) |
| Runtime 源码 | `cangjie_runtime/runtime/` |
| SDK 路径 | `cangjie-pkg/self/dev260313/cangjie/` |
| 测试用例 | `doc/benchmark/gc_benchmark.cj` |
| 编译器 | SDK 内置 `cjc` |
| 堆大小 | 256MB、512MB |
| 每组重复 | **3 次**，取平均值 |

## 3. 测试矩阵

共 **2 GC × 2 堆大小 × 3 轮 = 12 次**运行：

| 轮次 | GC 模式 | 堆大小 | 运行标识 |
|------|---------|--------|----------|
| 1-3 | CopyGC | 256MB | copy_256m_r1/r2/r3 |
| 4-6 | CopyGC | 512MB | copy_512m_r1/r2/r3 |
| 7-9 | MS-GC | 256MB | ms_256m_r1/r2/r3 |
| 10-12 | MS-GC | 512MB | ms_512m_r1/r2/r3 |

> **执行顺序**：先完成一种 GC 的所有测试，再切换另一种，避免频繁替换 dylib。

## 4. 测试维度与采集方法

### 4.1 功能完整性
- 解析 benchmark 输出中的 `[BENCH] F*` 行，统计 PASS/FAIL 数量
- 判定：F1-F5 全部 PASS 视为功能完整

### 4.2 执行性能
- 解析 `[BENCH] P*` 行中的耗时（ms）
- 解析 `[BENCH] Total wall time` 行
- 3 轮取平均值

### 4.3 CPU 使用率
- 采样命令：`ps -p $PID -o %cpu=`，每 **0.2 秒**采一次
- 计算指标：
  - **平均 CPU%**：所有采样点的算术平均
  - **峰值 CPU%**：所有采样点的最大值

### 4.4 内存使用率（RSS）
- 采样命令：`ps -p $PID -o rss=`，每 **0.2 秒**采一次（与 CPU 同步采集）
- RSS 单位：KB → 输出时转为 MB
- 计算指标：
  - **平均 RSS**：所有采样点的算术平均
  - **峰值 RSS**：所有采样点的最大值

## 5. 编译替换流程

### 5.1 编译 MS-GC Runtime

```bash
cd <runtime_root>
python3 build.py build --gc-mode marksweep
# 产物: output/temp/lib/aarch64_Release/libcangjie-runtime.dylib
```

### 5.2 替换 SDK dylib（MS-GC）

```bash
SDK=<cangjie_sdk_path>
TARGET=$SDK/runtime/lib/darwin_aarch64_cjnative/libcangjie-runtime.dylib
cp output/temp/lib/aarch64_Release/libcangjie-runtime.dylib "$TARGET"
codesign -f -s - "$TARGET"
```

### 5.3 恢复 CopyGC（原始 SDK）

```bash
cp "$TARGET.origin" "$TARGET"
codesign -f -s - "$TARGET"
```

### 5.4 编译测试用例（每次替换 dylib 后必须重新编译）

```bash
source $SDK/envsetup.sh
cjc gc_benchmark.cj -o gc_benchmark
```

> **关键规则**：切换 GC 模式后，**必须重新用 cjc 编译测试用例**，否则会导致进程卡死。

## 6. 自动化脚本使用说明

### 6.1 测试脚本：`gc_bench_compare.sh`

一键完成 CopyGC 与 MS-GC 的全对比测试。

```bash
cd doc/benchmark
bash gc_bench_compare.sh
```

脚本会自动执行以下流程：
1. 恢复 CopyGC → 编译测试用例 → 运行 3 轮 256MB → 运行 3 轮 512MB
2. 编译 MS-GC runtime → 替换 → 编译测试用例 → 运行 3 轮 256MB → 运行 3 轮 512MB
3. 恢复 CopyGC（收尾）

每轮运行会采集 benchmark 标准输出 + CPU/RSS 采样数据，保存到 `results/` 子目录。

### 6.2 分析脚本：`gc_bench_analyze.py`

汇总 `results/` 目录下的原始数据，生成对比报告。

```bash
python3 gc_bench_analyze.py results/
```

输出：Markdown 格式的对比表格，自动写入 `results/GC_Benchmark_Report_<date>.md`。

### 6.3 输出目录结构

```
doc/benchmark/results/<timestamp>/
├── copy_256m_r1_stdout.txt      # benchmark 标准输出
├── copy_256m_r1_resource.csv    # CPU%,RSS(KB) 采样
├── copy_256m_r2_stdout.txt
├── copy_256m_r2_resource.csv
├── ...
├── ms_512m_r3_stdout.txt
├── ms_512m_r3_resource.csv
└── GC_Benchmark_Report.md       # 自动生成的对比报告
```

## 7. 结果模板

### 7.1 功能完整性

| GC 模式 | 堆大小 | F1 | F2 | F3 | F4 | F5 | 通过率 |
|---------|--------|----|----|----|----|----|----|
| CopyGC | 256MB | | | | | | /5 |
| CopyGC | 512MB | | | | | | /5 |
| MS-GC  | 256MB | | | | | | /5 |
| MS-GC  | 512MB | | | | | | /5 |

### 7.2 执行性能（ms，3 轮平均）

| 测试项 | CopyGC 256M | CopyGC 512M | MS-GC 256M | MS-GC 512M |
|--------|-------------|-------------|------------|------------|
| P1_SmallAlloc | | | | |
| P2_LargeAlloc | | | | |
| P3_MixedWorkload | | | | |
| P4_LongLivedChurn | | | | |
| P5_Fragmentation | | | | |
| **Total** | | | | |

### 7.3 资源使用率（3 轮平均）

| 指标 | CopyGC 256M | CopyGC 512M | MS-GC 256M | MS-GC 512M |
|------|-------------|-------------|------------|------------|
| 平均 CPU% | | | | |
| 峰值 CPU% | | | | |
| 平均 RSS (MB) | | | | |
| 峰值 RSS (MB) | | | | |

## 8. 判定标准

| 维度 | 达标条件 | 优秀条件 |
|------|---------|---------|
| 功能完整性 | F1-F5 全部 PASS | — |
| Total 性能 | MS-GC 不超过 CopyGC 的 **1.5×** | MS-GC 接近或优于 CopyGC |
| 峰值 RSS | MS-GC 不超过 CopyGC 的 **1.3×** | MS-GC 低于 CopyGC |
| 平均 CPU% | MS-GC 不超过 CopyGC 的 **1.5×** | MS-GC 接近 CopyGC |

> 判定系数可根据实际需要调整。对比结论应附在每次报告文件末尾。
