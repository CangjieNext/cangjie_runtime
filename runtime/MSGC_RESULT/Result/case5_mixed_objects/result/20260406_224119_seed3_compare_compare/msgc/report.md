# MSGC Benchmark Report

- 用例: case5_mixed_objects
- GC 模式: marksweep
- 堆大小: 1gb
- 结果目录: `msgc`
- 时间: 2026-04-06T14:41:34Z

## 总览

| 指标 | 值 |
| --- | ---: |
| Benchmark 总时间 (ms) | 14129 |
| Monitor 总时间 (ms) | 14510 |
| RSS 平均 (MB) | 65.19 |
| RSS 峰值 (MB) | 78.05 |
| RSS P95 (MB) | 75.60 |
| CPU 平均 (%) | 139.44 |
| CPU 峰值 (%) | 159.80 |
| CPU P95 (%) | 159.48 |
| 采样点数 | 67 |
| GC 次数 | 340 |
| GC 总时间 (ms) | 6994.61 |
| GC 平均时间 (ms) | 20.57 |
| GC 最大时间 (ms) | 182.56 |
| GC P95 时间 (ms) | 24.13 |
| GC 平均间隔 (ms) | 41.60 |
| True live 平均 (MB) | 24.89 |
| True live 峰值 (MB) | 27.69 |

## GC 原因分布

| 原因 | 次数 |
| --- | ---: |
| heuristic | 340 |

## GC 阶段分解

| 阶段 | 总时间 (ms) | 平均 (ms) | 占比 |
| --- | ---: | ---: | ---: |
| PreEnum | 77.43 | 0.23 | 1.1% |
| EnumRoots | 61.73 | 0.18 | 0.9% |
| ConcMark | 6513.13 | 19.16 | 93.1% |
| MarkTerm | 178.23 | 0.52 | 2.5% |
| PostMark | 32.14 | 0.09 | 0.5% |
| ConcSweep | 23.73 | 0.07 | 0.3% |
| PostGC | 7.14 | 0.02 | 0.1% |
| Other | 101.08 | 0.30 | 1.4% |

## 最慢 GC 周期 (Top 10)

| Cycle | 原因 | 总时间 (ms) |
| --- | --- | ---: |
| 88 | heuristic | 182.562 |
| 292 | heuristic | 151.114 |
| 66 | heuristic | 150.540 |
| 309 | heuristic | 135.510 |
| 68 | heuristic | 50.445 |
| 255 | heuristic | 33.319 |
| 290 | heuristic | 29.535 |
| 124 | heuristic | 27.287 |
| 311 | heuristic | 27.030 |
| 293 | heuristic | 26.218 |

