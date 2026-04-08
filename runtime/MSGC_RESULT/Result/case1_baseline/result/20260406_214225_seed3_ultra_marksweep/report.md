# MSGC Benchmark Report

- 用例: case1_baseline
- GC 模式: marksweep
- 堆大小: 256mb
- 结果目录: `20260406_214225_seed3_ultra_marksweep`
- 时间: 2026-04-06T13:42:25Z

## 总览

| 指标 | 值 |
| --- | ---: |
| Benchmark 总时间 (ms) | 6692 |
| Monitor 总时间 (ms) | 7471 |
| RSS 平均 (MB) | 29.17 |
| RSS 峰值 (MB) | 62.39 |
| RSS P95 (MB) | 51.36 |
| CPU 平均 (%) | 100.09 |
| CPU 峰值 (%) | 139.60 |
| CPU P95 (%) | 126.12 |
| 采样点数 | 34 |
| GC 次数 | 149 |
| GC 总时间 (ms) | 411.87 |
| GC 平均时间 (ms) | 2.76 |
| GC 最大时间 (ms) | 31.71 |
| GC P95 时间 (ms) | 22.86 |
| GC 平均间隔 (ms) | 45.01 |
| True live 平均 (MB) | 2.51 |
| True live 峰值 (MB) | 29.07 |

## GC 原因分布

| 原因 | 次数 |
| --- | ---: |
| heuristic | 149 |

## Benchmark 阶段

| 阶段 | 时间 (ms) |
| --- | ---: |
| P1_SmallAlloc | 309 |
| P2_LargeAlloc | 1111 |
| P3_MixedWorkload | 680 |
| P4_LongLivedChurn | 541 |
| P5_Fragmentation | 935 |

## GC 阶段分解

| 阶段 | 总时间 (ms) | 平均 (ms) | 占比 |
| --- | ---: | ---: | ---: |
| PreEnum | 12.28 | 0.08 | 3.0% |
| EnumRoots | 15.82 | 0.11 | 3.8% |
| ConcMark | 306.78 | 2.06 | 74.5% |
| MarkTerm | 2.21 | 0.01 | 0.5% |
| PostMark | 52.41 | 0.35 | 12.7% |
| ConcSweep | 4.09 | 0.03 | 1.0% |
| PostGC | 1.57 | 0.01 | 0.4% |
| Other | 16.71 | 0.11 | 4.1% |

## 最慢 GC 周期 (Top 10)

| Cycle | 原因 | 总时间 (ms) |
| --- | --- | ---: |
| 12 | heuristic | 31.709 |
| 9 | heuristic | 28.686 |
| 11 | heuristic | 28.224 |
| 8 | heuristic | 28.065 |
| 14 | heuristic | 27.123 |
| 13 | heuristic | 26.902 |
| 10 | heuristic | 26.821 |
| 15 | heuristic | 26.531 |
| 31 | heuristic | 17.350 |
| 6 | heuristic | 7.226 |

