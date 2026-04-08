# MSGC Benchmark Report

- 用例: case1_baseline
- GC 模式: marksweep
- 堆大小: 256mb
- 结果目录: `20260406_214540_seed4_extreme_marksweep`
- 时间: 2026-04-06T13:45:40Z

## 总览

| 指标 | 值 |
| --- | ---: |
| Benchmark 总时间 (ms) | 6565 |
| Monitor 总时间 (ms) | 7077 |
| RSS 平均 (MB) | 20.08 |
| RSS 峰值 (MB) | 46.73 |
| RSS P95 (MB) | 35.83 |
| CPU 平均 (%) | 105.75 |
| CPU 峰值 (%) | 150.80 |
| CPU P95 (%) | 130.80 |
| 采样点数 | 33 |
| GC 次数 | 283 |
| GC 总时间 (ms) | 588.20 |
| GC 平均时间 (ms) | 2.08 |
| GC 最大时间 (ms) | 28.92 |
| GC P95 时间 (ms) | 5.33 |
| GC 平均间隔 (ms) | 23.27 |
| True live 平均 (MB) | 2.07 |
| True live 峰值 (MB) | 28.79 |

## GC 原因分布

| 原因 | 次数 |
| --- | ---: |
| heuristic | 283 |

## Benchmark 阶段

| 阶段 | 时间 (ms) |
| --- | ---: |
| P1_SmallAlloc | 316 |
| P2_LargeAlloc | 1109 |
| P3_MixedWorkload | 678 |
| P4_LongLivedChurn | 549 |
| P5_Fragmentation | 939 |

## GC 阶段分解

| 阶段 | 总时间 (ms) | 平均 (ms) | 占比 |
| --- | ---: | ---: | ---: |
| PreEnum | 15.37 | 0.05 | 2.6% |
| EnumRoots | 8.12 | 0.03 | 1.4% |
| ConcMark | 463.94 | 1.64 | 78.9% |
| MarkTerm | 11.49 | 0.04 | 2.0% |
| PostMark | 54.46 | 0.19 | 9.3% |
| ConcSweep | 4.30 | 0.02 | 0.7% |
| PostGC | 2.32 | 0.01 | 0.4% |
| Other | 28.21 | 0.10 | 4.8% |

## 最慢 GC 周期 (Top 10)

| Cycle | 原因 | 总时间 (ms) |
| --- | --- | ---: |
| 14 | heuristic | 28.917 |
| 23 | heuristic | 27.945 |
| 18 | heuristic | 27.205 |
| 21 | heuristic | 27.139 |
| 17 | heuristic | 27.125 |
| 19 | heuristic | 26.733 |
| 16 | heuristic | 26.585 |
| 22 | heuristic | 26.572 |
| 15 | heuristic | 26.359 |
| 13 | heuristic | 26.133 |

