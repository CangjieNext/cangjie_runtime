# MSGC Benchmark Report

- 用例: case2_50x_pressure
- GC 模式: copygc
- 堆大小: 16gb
- 结果目录: `copygc`
- 时间: 2026-04-06T16:10:42Z

## 总览

| 指标 | 值 |
| --- | ---: |
| Benchmark 总时间 (ms) | 338811 |
| Monitor 总时间 (ms) | 339695 |
| RSS 平均 (MB) | 416.47 |
| RSS 峰值 (MB) | 812.69 |
| RSS P95 (MB) | 713.25 |
| CPU 平均 (%) | 111.81 |
| CPU 峰值 (%) | 148.00 |
| CPU P95 (%) | 137.80 |
| 采样点数 | 1596 |
| GC 次数 | 351 |
| GC 总时间 (ms) | 4438.21 |
| GC 平均时间 (ms) | 12.64 |
| GC 最大时间 (ms) | 161.61 |
| GC P95 时间 (ms) | 28.07 |
| GC 平均间隔 (ms) | 967.50 |
| True live 平均 (MB) | 3.76 |
| True live 峰值 (MB) | 29.31 |

## GC 原因分布

| 原因 | 次数 |
| --- | ---: |
| heuristic | 351 |

## Benchmark 阶段

| 阶段 | 时间 (ms) |
| --- | ---: |
| P1_SmallAlloc | 16296 |
| P2_LargeAlloc | 60524 |
| P3_MixedWorkload | 37707 |
| P4_LongLivedChurn | 24741 |
| P5_Fragmentation | 52642 |

## GC 阶段分解

| 阶段 | 总时间 (ms) | 平均 (ms) | 占比 |
| --- | ---: | ---: | ---: |
| PreEnum | 419.58 | 1.20 | 9.5% |
| EnumRoots | 11.37 | 0.03 | 0.3% |
| ConcMark | 1156.25 | 3.29 | 26.1% |
| MarkTerm | 54.37 | 0.15 | 1.2% |
| PostMark | 2348.94 | 6.69 | 52.9% |
| ConcSweep | 360.77 | 1.03 | 8.1% |
| PostGC | 5.02 | 0.01 | 0.1% |
| Other | 81.93 | 0.23 | 1.8% |

## 最慢 GC 周期 (Top 10)

| Cycle | 原因 | 总时间 (ms) |
| --- | --- | ---: |
| 245 | heuristic | 161.609 |
| 345 | heuristic | 40.560 |
| 66 | heuristic | 32.927 |
| 58 | heuristic | 32.914 |
| 47 | heuristic | 31.900 |
| 61 | heuristic | 31.562 |
| 55 | heuristic | 30.890 |
| 46 | heuristic | 29.781 |
| 292 | heuristic | 29.460 |
| 62 | heuristic | 28.485 |

