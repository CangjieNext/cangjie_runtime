# MSGC Benchmark Report

- 用例: case5_mixed_objects
- GC 模式: copygc
- 堆大小: 1gb
- 结果目录: `copygc`
- 时间: 2026-04-06T14:41:19Z

## 总览

| 指标 | 值 |
| --- | ---: |
| Benchmark 总时间 (ms) | 11997 |
| Monitor 总时间 (ms) | 12948 |
| RSS 平均 (MB) | 155.73 |
| RSS 峰值 (MB) | 308.64 |
| RSS P95 (MB) | 261.90 |
| CPU 平均 (%) | 102.89 |
| CPU 峰值 (%) | 125.40 |
| CPU P95 (%) | 124.42 |
| 采样点数 | 60 |
| GC 次数 | 31 |
| GC 总时间 (ms) | 680.18 |
| GC 平均时间 (ms) | 21.94 |
| GC 最大时间 (ms) | 28.00 |
| GC P95 时间 (ms) | 26.74 |
| GC 平均间隔 (ms) | 383.92 |
| True live 平均 (MB) | 24.65 |
| True live 峰值 (MB) | 26.45 |

## GC 原因分布

| 原因 | 次数 |
| --- | ---: |
| heuristic | 31 |

## GC 阶段分解

| 阶段 | 总时间 (ms) | 平均 (ms) | 占比 |
| --- | ---: | ---: | ---: |
| PreEnum | 15.17 | 0.49 | 2.2% |
| EnumRoots | 1.05 | 0.03 | 0.2% |
| ConcMark | 586.30 | 18.91 | 86.2% |
| MarkTerm | 37.06 | 1.20 | 5.4% |
| PostMark | 18.59 | 0.60 | 2.7% |
| ConcSweep | 12.67 | 0.41 | 1.9% |
| PostGC | 0.18 | 0.01 | 0.0% |
| Other | 9.15 | 0.30 | 1.3% |

## 最慢 GC 周期 (Top 10)

| Cycle | 原因 | 总时间 (ms) |
| --- | --- | ---: |
| 29 | heuristic | 28.004 |
| 8 | heuristic | 27.442 |
| 7 | heuristic | 26.030 |
| 20 | heuristic | 25.454 |
| 21 | heuristic | 25.358 |
| 13 | heuristic | 24.014 |
| 17 | heuristic | 23.886 |
| 19 | heuristic | 23.325 |
| 12 | heuristic | 23.054 |
| 26 | heuristic | 22.599 |

