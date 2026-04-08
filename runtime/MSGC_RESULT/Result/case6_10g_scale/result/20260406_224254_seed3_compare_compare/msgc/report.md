# MSGC Benchmark Report

- 用例: case6_10g_scale
- GC 模式: marksweep
- 堆大小: 12gb
- 结果目录: `msgc`
- 时间: 2026-04-06T14:47:15Z

## 总览

| 指标 | 值 |
| --- | ---: |
| Benchmark 总时间 (ms) | 281222 |
| Monitor 总时间 (ms) | 282539 |
| RSS 平均 (MB) | 891.55 |
| RSS 峰值 (MB) | 2491.30 |
| RSS P95 (MB) | 2037.06 |
| CPU 平均 (%) | 170.09 |
| CPU 峰值 (%) | 199.80 |
| CPU P95 (%) | 196.93 |
| 采样点数 | 1316 |
| GC 次数 | 1007 |
| GC 总时间 (ms) | 137654.61 |
| GC 平均时间 (ms) | 136.70 |
| GC 最大时间 (ms) | 333.63 |
| GC P95 时间 (ms) | 147.08 |
| GC 平均间隔 (ms) | 158.99 |
| True live 平均 (MB) | 196.44 |
| True live 峰值 (MB) | 216.04 |

## GC 原因分布

| 原因 | 次数 |
| --- | ---: |
| heuristic | 1007 |

## GC 阶段分解

| 阶段 | 总时间 (ms) | 平均 (ms) | 占比 |
| --- | ---: | ---: | ---: |
| PreEnum | 582.83 | 0.58 | 0.4% |
| EnumRoots | 50.91 | 0.05 | 0.0% |
| ConcMark | 119119.91 | 118.29 | 86.5% |
| MarkTerm | 17228.36 | 17.11 | 12.5% |
| PostMark | 186.60 | 0.19 | 0.1% |
| ConcSweep | 338.11 | 0.34 | 0.2% |
| PostGC | 4.71 | 0.00 | 0.0% |
| Other | 143.17 | 0.14 | 0.1% |

## 最慢 GC 周期 (Top 10)

| Cycle | 原因 | 总时间 (ms) |
| --- | --- | ---: |
| 2634 | heuristic | 333.629 |
| 3390 | heuristic | 319.563 |
| 3007 | heuristic | 316.382 |
| 2684 | heuristic | 305.591 |
| 3446 | heuristic | 304.793 |
| 3059 | heuristic | 295.669 |
| 3063 | heuristic | 295.229 |
| 3441 | heuristic | 260.528 |
| 2739 | heuristic | 243.027 |
| 3442 | heuristic | 160.326 |

