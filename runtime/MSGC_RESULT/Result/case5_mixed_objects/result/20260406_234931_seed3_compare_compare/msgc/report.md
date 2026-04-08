# MSGC Benchmark Report

- 用例: case5_mixed_objects
- GC 模式: marksweep
- 堆大小: 1gb
- 结果目录: `msgc`
- 时间: 2026-04-06T15:49:46Z

## 总览

| 指标 | 值 |
| --- | ---: |
| Benchmark 总时间 (ms) | 13972 |
| Monitor 总时间 (ms) | 14434 |
| RSS 平均 (MB) | 64.30 |
| RSS 峰值 (MB) | 76.50 |
| RSS P95 (MB) | 75.52 |
| CPU 平均 (%) | 141.09 |
| CPU 峰值 (%) | 158.00 |
| CPU P95 (%) | 157.44 |
| 采样点数 | 67 |
| GC 次数 | 344 |
| GC 总时间 (ms) | 6714.22 |
| GC 平均时间 (ms) | 19.52 |
| GC 最大时间 (ms) | 100.00 |
| GC P95 时间 (ms) | 21.57 |
| GC 平均间隔 (ms) | 40.65 |
| True live 平均 (MB) | 24.66 |
| True live 峰值 (MB) | 27.50 |

## GC 原因分布

| 原因 | 次数 |
| --- | ---: |
| heuristic | 344 |

## GC 阶段分解

| 阶段 | 总时间 (ms) | 平均 (ms) | 占比 |
| --- | ---: | ---: | ---: |
| PreEnum | 39.08 | 0.11 | 0.6% |
| EnumRoots | 29.74 | 0.09 | 0.4% |
| ConcMark | 6099.09 | 17.73 | 90.8% |
| MarkTerm | 405.52 | 1.18 | 6.0% |
| PostMark | 38.42 | 0.11 | 0.6% |
| ConcSweep | 24.15 | 0.07 | 0.4% |
| PostGC | 16.98 | 0.05 | 0.3% |
| Other | 61.23 | 0.18 | 0.9% |

## 最慢 GC 周期 (Top 10)

| Cycle | 原因 | 总时间 (ms) |
| --- | --- | ---: |
| 129 | heuristic | 100.005 |
| 50 | heuristic | 95.972 |
| 336 | heuristic | 90.717 |
| 49 | heuristic | 83.368 |
| 131 | heuristic | 58.325 |
| 51 | heuristic | 57.543 |
| 338 | heuristic | 28.230 |
| 277 | heuristic | 27.450 |
| 152 | heuristic | 26.594 |
| 316 | heuristic | 22.906 |

