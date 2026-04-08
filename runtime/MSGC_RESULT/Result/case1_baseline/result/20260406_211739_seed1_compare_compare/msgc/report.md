# MSGC Benchmark Report

- 用例: case1_baseline
- GC 模式: marksweep
- 堆大小: 256mb
- 结果目录: `msgc`
- 时间: 2026-04-06T13:18:40Z

## 总览

| 指标 | 值 |
| --- | ---: |
| Benchmark 总时间 (ms) | 6754 |
| Monitor 总时间 (ms) | 7285 |
| RSS 平均 (MB) | 79.10 |
| RSS 峰值 (MB) | 130.55 |
| RSS P95 (MB) | 125.31 |
| CPU 平均 (%) | 96.37 |
| CPU 峰值 (%) | 116.40 |
| CPU P95 (%) | 115.53 |
| 采样点数 | 34 |
| GC 次数 | 38 |
| GC 总时间 (ms) | 227.81 |
| GC 平均时间 (ms) | 6.00 |
| GC 最大时间 (ms) | 60.23 |
| GC P95 时间 (ms) | 26.15 |
| GC 平均间隔 (ms) | 179.25 |
| True live 平均 (MB) | 3.29 |
| True live 峰值 (MB) | 28.17 |

## GC 原因分布

| 原因 | 次数 |
| --- | ---: |
| heuristic | 38 |

## Benchmark 阶段

| 阶段 | 时间 (ms) |
| --- | ---: |
| P1_SmallAlloc | 303 |
| P2_LargeAlloc | 1132 |
| P3_MixedWorkload | 928 |
| P4_LongLivedChurn | 574 |
| P5_Fragmentation | 978 |

## GC 阶段分解

| 阶段 | 总时间 (ms) | 平均 (ms) | 占比 |
| --- | ---: | ---: | ---: |
| PreEnum | 9.76 | 0.26 | 4.3% |
| EnumRoots | 1.24 | 0.03 | 0.5% |
| ConcMark | 136.56 | 3.59 | 59.9% |
| MarkTerm | 0.52 | 0.01 | 0.2% |
| PostMark | 55.85 | 1.47 | 24.5% |
| ConcSweep | 3.70 | 0.10 | 1.6% |
| PostGC | 14.13 | 0.37 | 6.2% |
| Other | 6.05 | 0.16 | 2.7% |

## 最慢 GC 周期 (Top 10)

| Cycle | 原因 | 总时间 (ms) |
| --- | --- | ---: |
| 25 | heuristic | 60.232 |
| 2 | heuristic | 26.875 |
| 3 | heuristic | 26.023 |
| 4 | heuristic | 25.945 |
| 17 | heuristic | 8.090 |
| 1 | heuristic | 5.784 |
| 31 | heuristic | 4.647 |
| 28 | heuristic | 4.600 |
| 32 | heuristic | 4.041 |
| 18 | heuristic | 3.964 |

