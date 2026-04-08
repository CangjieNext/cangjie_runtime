# MSGC Benchmark Report

- 用例: case2_50x_pressure
- GC 模式: copygc
- 堆大小: 4gb
- 结果目录: `copygc`
- 时间: 2026-04-06T15:58:46Z

## 总览

| 指标 | 值 |
| --- | ---: |
| Benchmark 总时间 (ms) | 326638 |
| Monitor 总时间 (ms) | 327285 |
| RSS 平均 (MB) | 254.08 |
| RSS 峰值 (MB) | 550.48 |
| RSS P95 (MB) | 472.46 |
| CPU 平均 (%) | 112.72 |
| CPU 峰值 (%) | 139.60 |
| CPU P95 (%) | 130.30 |
| 采样点数 | 1534 |
| GC 次数 | 639 |
| GC 总时间 (ms) | 4514.19 |
| GC 平均时间 (ms) | 7.06 |
| GC 最大时间 (ms) | 122.30 |
| GC P95 时间 (ms) | 26.74 |
| GC 平均间隔 (ms) | 511.29 |
| True live 平均 (MB) | 2.54 |
| True live 峰值 (MB) | 29.85 |

## GC 原因分布

| 原因 | 次数 |
| --- | ---: |
| heuristic | 639 |

## Benchmark 阶段

| 阶段 | 时间 (ms) |
| --- | ---: |
| P1_SmallAlloc | 15755 |
| P2_LargeAlloc | 58319 |
| P3_MixedWorkload | 35382 |
| P4_LongLivedChurn | 22697 |
| P5_Fragmentation | 48910 |

## GC 阶段分解

| 阶段 | 总时间 (ms) | 平均 (ms) | 占比 |
| --- | ---: | ---: | ---: |
| PreEnum | 416.01 | 0.65 | 9.2% |
| EnumRoots | 26.35 | 0.04 | 0.6% |
| ConcMark | 1260.44 | 1.97 | 27.9% |
| MarkTerm | 195.95 | 0.31 | 4.3% |
| PostMark | 2166.62 | 3.39 | 48.0% |
| ConcSweep | 310.62 | 0.49 | 6.9% |
| PostGC | 6.81 | 0.01 | 0.2% |
| Other | 131.39 | 0.21 | 2.9% |

## 最慢 GC 周期 (Top 10)

| Cycle | 原因 | 总时间 (ms) |
| --- | --- | ---: |
| 51 | heuristic | 122.301 |
| 59 | heuristic | 34.265 |
| 67 | heuristic | 33.859 |
| 66 | heuristic | 32.302 |
| 56 | heuristic | 32.275 |
| 69 | heuristic | 32.033 |
| 61 | heuristic | 31.978 |
| 68 | heuristic | 31.031 |
| 49 | heuristic | 30.669 |
| 58 | heuristic | 30.526 |

