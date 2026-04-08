# MSGC Benchmark Report

- 用例: case5_mixed_objects
- GC 模式: copygc
- 堆大小: 1gb
- 结果目录: `copygc`
- 时间: 2026-04-06T15:49:31Z

## 总览

| 指标 | 值 |
| --- | ---: |
| Benchmark 总时间 (ms) | 11889 |
| Monitor 总时间 (ms) | 12673 |
| RSS 平均 (MB) | 159.00 |
| RSS 峰值 (MB) | 316.48 |
| RSS P95 (MB) | 262.14 |
| CPU 平均 (%) | 103.08 |
| CPU 峰值 (%) | 129.60 |
| CPU P95 (%) | 127.60 |
| 采样点数 | 59 |
| GC 次数 | 31 |
| GC 总时间 (ms) | 634.74 |
| GC 平均时间 (ms) | 20.48 |
| GC 最大时间 (ms) | 24.77 |
| GC P95 时间 (ms) | 24.12 |
| GC 平均间隔 (ms) | 388.36 |
| True live 平均 (MB) | 24.88 |
| True live 峰值 (MB) | 26.33 |

## GC 原因分布

| 原因 | 次数 |
| --- | ---: |
| heuristic | 31 |

## GC 阶段分解

| 阶段 | 总时间 (ms) | 平均 (ms) | 占比 |
| --- | ---: | ---: | ---: |
| PreEnum | 16.30 | 0.53 | 2.6% |
| EnumRoots | 1.03 | 0.03 | 0.2% |
| ConcMark | 569.83 | 18.38 | 89.8% |
| MarkTerm | 0.40 | 0.01 | 0.1% |
| PostMark | 27.58 | 0.89 | 4.3% |
| ConcSweep | 13.26 | 0.43 | 2.1% |
| PostGC | 0.18 | 0.01 | 0.0% |
| Other | 6.17 | 0.20 | 1.0% |

## 最慢 GC 周期 (Top 10)

| Cycle | 原因 | 总时间 (ms) |
| --- | --- | ---: |
| 11 | heuristic | 24.772 |
| 31 | heuristic | 24.413 |
| 25 | heuristic | 23.817 |
| 10 | heuristic | 23.561 |
| 9 | heuristic | 23.099 |
| 8 | heuristic | 21.812 |
| 20 | heuristic | 21.106 |
| 30 | heuristic | 20.601 |
| 21 | heuristic | 20.554 |
| 28 | heuristic | 20.370 |

