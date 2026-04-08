# MSGC Benchmark Report

- 用例: case4_read_heavy
- GC 模式: copygc
- 堆大小: 512mb
- 结果目录: `copygc`
- 时间: 2026-04-06T14:23:12Z

## 总览

| 指标 | 值 |
| --- | ---: |
| Benchmark 总时间 (ms) | 570881 |
| Monitor 总时间 (ms) | 571579 |
| RSS 平均 (MB) | 913.94 |
| RSS 峰值 (MB) | 1884.02 |
| RSS P95 (MB) | 1600.18 |
| CPU 平均 (%) | 110.78 |
| CPU 峰值 (%) | 185.70 |
| CPU P95 (%) | 166.20 |
| 采样点数 | 2675 |
| GC 次数 | 179 |
| GC 总时间 (ms) | 7811.31 |
| GC 平均时间 (ms) | 43.64 |
| GC 最大时间 (ms) | 69.36 |
| GC P95 时间 (ms) | 51.57 |
| GC 平均间隔 (ms) | 3201.28 |
| True live 平均 (MB) | 53.81 |
| True live 峰值 (MB) | 57.98 |

## GC 原因分布

| 原因 | 次数 |
| --- | ---: |
| heuristic | 179 |

## GC 阶段分解

| 阶段 | 总时间 (ms) | 平均 (ms) | 占比 |
| --- | ---: | ---: | ---: |
| PreEnum | 647.65 | 3.62 | 8.3% |
| EnumRoots | 8.06 | 0.05 | 0.1% |
| ConcMark | 6408.03 | 35.80 | 82.0% |
| MarkTerm | 135.89 | 0.76 | 1.7% |
| PostMark | 4.59 | 0.03 | 0.1% |
| ConcSweep | 522.06 | 2.92 | 6.7% |
| PostGC | 1.02 | 0.01 | 0.0% |
| Other | 83.99 | 0.47 | 1.1% |

## 最慢 GC 周期 (Top 10)

| Cycle | 原因 | 总时间 (ms) |
| --- | --- | ---: |
| 160 | heuristic | 69.365 |
| 51 | heuristic | 61.501 |
| 138 | heuristic | 59.832 |
| 179 | heuristic | 59.315 |
| 170 | heuristic | 58.520 |
| 110 | heuristic | 58.492 |
| 127 | heuristic | 57.661 |
| 173 | heuristic | 54.978 |
| 149 | heuristic | 51.586 |
| 150 | heuristic | 51.571 |

