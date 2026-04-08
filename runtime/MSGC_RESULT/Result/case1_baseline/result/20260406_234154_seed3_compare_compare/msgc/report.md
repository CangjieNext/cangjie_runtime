# MSGC Benchmark Report

- 用例: case1_baseline
- GC 模式: marksweep
- 堆大小: 256mb
- 结果目录: `msgc`
- 时间: 2026-04-06T15:42:52Z

## 总览

| 指标 | 值 |
| --- | ---: |
| Benchmark 总时间 (ms) | 6655 |
| Monitor 总时间 (ms) | 7322 |
| RSS 平均 (MB) | 29.34 |
| RSS 峰值 (MB) | 54.52 |
| RSS P95 (MB) | 47.79 |
| CPU 平均 (%) | 101.28 |
| CPU 峰值 (%) | 130.30 |
| CPU P95 (%) | 125.09 |
| 采样点数 | 34 |
| GC 次数 | 145 |
| GC 总时间 (ms) | 388.73 |
| GC 平均时间 (ms) | 2.68 |
| GC 最大时间 (ms) | 30.43 |
| GC P95 时间 (ms) | 23.04 |
| GC 平均间隔 (ms) | 46.06 |
| True live 平均 (MB) | 2.51 |
| True live 峰值 (MB) | 28.63 |

## GC 原因分布

| 原因 | 次数 |
| --- | ---: |
| heuristic | 145 |

## Benchmark 阶段

| 阶段 | 时间 (ms) |
| --- | ---: |
| P1_SmallAlloc | 309 |
| P2_LargeAlloc | 1109 |
| P3_MixedWorkload | 681 |
| P4_LongLivedChurn | 542 |
| P5_Fragmentation | 952 |

## GC 阶段分解

| 阶段 | 总时间 (ms) | 平均 (ms) | 占比 |
| --- | ---: | ---: | ---: |
| PreEnum | 12.12 | 0.08 | 3.1% |
| EnumRoots | 4.18 | 0.03 | 1.1% |
| ConcMark | 300.98 | 2.08 | 77.4% |
| MarkTerm | 1.68 | 0.01 | 0.4% |
| PostMark | 44.34 | 0.31 | 11.4% |
| ConcSweep | 3.26 | 0.02 | 0.8% |
| PostGC | 1.25 | 0.01 | 0.3% |
| Other | 20.93 | 0.14 | 5.4% |

## 最慢 GC 周期 (Top 10)

| Cycle | 原因 | 总时间 (ms) |
| --- | --- | ---: |
| 15 | heuristic | 30.430 |
| 14 | heuristic | 27.251 |
| 11 | heuristic | 27.023 |
| 12 | heuristic | 26.815 |
| 9 | heuristic | 26.249 |
| 8 | heuristic | 26.125 |
| 10 | heuristic | 26.055 |
| 13 | heuristic | 25.981 |
| 7 | heuristic | 11.278 |
| 5 | heuristic | 6.821 |

