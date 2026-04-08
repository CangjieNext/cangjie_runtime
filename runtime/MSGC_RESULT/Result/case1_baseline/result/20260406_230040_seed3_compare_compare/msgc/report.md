# MSGC Benchmark Report

- 用例: case1_baseline
- GC 模式: marksweep
- 堆大小: 256mb
- 结果目录: `msgc`
- 时间: 2026-04-06T15:01:39Z

## 总览

| 指标 | 值 |
| --- | ---: |
| Benchmark 总时间 (ms) | 6527 |
| Monitor 总时间 (ms) | 7266 |
| RSS 平均 (MB) | 29.93 |
| RSS 峰值 (MB) | 62.31 |
| RSS P95 (MB) | 51.79 |
| CPU 平均 (%) | 102.35 |
| CPU 峰值 (%) | 130.30 |
| CPU P95 (%) | 125.84 |
| 采样点数 | 34 |
| GC 次数 | 146 |
| GC 总时间 (ms) | 425.32 |
| GC 平均时间 (ms) | 2.91 |
| GC 最大时间 (ms) | 27.80 |
| GC P95 时间 (ms) | 25.95 |
| GC 平均间隔 (ms) | 44.78 |
| True live 平均 (MB) | 2.74 |
| True live 峰值 (MB) | 28.82 |

## GC 原因分布

| 原因 | 次数 |
| --- | ---: |
| heuristic | 146 |

## Benchmark 阶段

| 阶段 | 时间 (ms) |
| --- | ---: |
| P1_SmallAlloc | 310 |
| P2_LargeAlloc | 1102 |
| P3_MixedWorkload | 678 |
| P4_LongLivedChurn | 537 |
| P5_Fragmentation | 947 |

## GC 阶段分解

| 阶段 | 总时间 (ms) | 平均 (ms) | 占比 |
| --- | ---: | ---: | ---: |
| PreEnum | 11.94 | 0.08 | 2.8% |
| EnumRoots | 4.79 | 0.03 | 1.1% |
| ConcMark | 353.86 | 2.42 | 83.2% |
| MarkTerm | 1.71 | 0.01 | 0.4% |
| PostMark | 29.16 | 0.20 | 6.9% |
| ConcSweep | 3.21 | 0.02 | 0.8% |
| PostGC | 1.63 | 0.01 | 0.4% |
| Other | 19.02 | 0.13 | 4.5% |

## 最慢 GC 周期 (Top 10)

| Cycle | 原因 | 总时间 (ms) |
| --- | --- | ---: |
| 12 | heuristic | 27.803 |
| 13 | heuristic | 27.065 |
| 8 | heuristic | 26.746 |
| 15 | heuristic | 26.729 |
| 14 | heuristic | 26.696 |
| 11 | heuristic | 26.546 |
| 10 | heuristic | 26.372 |
| 4 | heuristic | 26.017 |
| 9 | heuristic | 25.734 |
| 1 | heuristic | 16.696 |

