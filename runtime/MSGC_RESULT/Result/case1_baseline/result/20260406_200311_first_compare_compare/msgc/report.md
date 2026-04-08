# MSGC Benchmark Report

- 用例: unknown
- GC 模式: msgc
- 堆大小: unknown
- 结果目录: `msgc`
- 时间: unknown

## 总览

| 指标 | 值 |
| --- | ---: |
| Benchmark 总时间 (ms) | 6798 |
| Monitor 总时间 (ms) | 7346 |
| RSS 平均 (MB) | 83.47 |
| RSS 峰值 (MB) | 189.73 |
| RSS P95 (MB) | 144.23 |
| CPU 平均 (%) | 98.75 |
| CPU 峰值 (%) | 121.70 |
| CPU P95 (%) | 118.48 |
| 采样点数 | 34 |
| GC 次数 | 29 |
| GC 总时间 (ms) | 123.65 |
| GC 平均时间 (ms) | 4.26 |
| GC 最大时间 (ms) | 26.70 |
| GC P95 时间 (ms) | 9.40 |
| GC 平均间隔 (ms) | 234.06 |
| True live 平均 (MB) | 2.13 |
| True live 峰值 (MB) | 27.78 |

## GC 原因分布

| 原因 | 次数 |
| --- | ---: |
| heuristic | 29 |

## Benchmark 阶段

| 阶段 | 时间 (ms) |
| --- | ---: |
| P1_SmallAlloc | 306 |
| P2_LargeAlloc | 1248 |
| P3_MixedWorkload | 709 |
| P4_LongLivedChurn | 562 |
| P5_Fragmentation | 965 |

## GC 阶段分解

| 阶段 | 总时间 (ms) | 平均 (ms) | 占比 |
| --- | ---: | ---: | ---: |
| PreEnum | 8.93 | 0.31 | 7.2% |
| EnumRoots | 0.98 | 0.03 | 0.8% |
| ConcMark | 49.07 | 1.69 | 39.7% |
| MarkTerm | 2.37 | 0.08 | 1.9% |
| PostMark | 54.17 | 1.87 | 43.8% |
| ConcSweep | 3.87 | 0.13 | 3.1% |
| PostGC | 0.19 | 0.01 | 0.2% |
| Other | 4.08 | 0.14 | 3.3% |

## 最慢 GC 周期 (Top 10)

| Cycle | 原因 | 总时间 (ms) |
| --- | --- | ---: |
| 2 | heuristic | 26.699 |
| 1 | heuristic | 10.585 |
| 14 | heuristic | 7.612 |
| 26 | heuristic | 6.734 |
| 25 | heuristic | 6.324 |
| 24 | heuristic | 5.896 |
| 27 | heuristic | 4.682 |
| 23 | heuristic | 4.313 |
| 28 | heuristic | 3.951 |
| 29 | heuristic | 3.684 |

