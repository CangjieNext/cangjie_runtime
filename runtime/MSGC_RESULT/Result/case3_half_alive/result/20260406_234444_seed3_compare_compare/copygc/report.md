# MSGC Benchmark Report

- 用例: case3_half_alive
- GC 模式: copygc
- 堆大小: 2gb
- 结果目录: `copygc`
- 时间: 2026-04-06T15:44:44Z

## 总览

| 指标 | 值 |
| --- | ---: |
| Benchmark 总时间 (ms) | 122501 |
| Monitor 总时间 (ms) | 123231 |
| RSS 平均 (MB) | 309.14 |
| RSS 峰值 (MB) | 879.02 |
| RSS P95 (MB) | 534.47 |
| CPU 平均 (%) | 111.67 |
| CPU 峰值 (%) | 138.80 |
| CPU P95 (%) | 132.50 |
| 采样点数 | 579 |
| GC 次数 | 203 |
| GC 总时间 (ms) | 601.68 |
| GC 平均时间 (ms) | 2.96 |
| GC 最大时间 (ms) | 54.76 |
| GC P95 时间 (ms) | 5.22 |
| GC 平均间隔 (ms) | 603.62 |
| True live 平均 (MB) | 2.04 |
| True live 峰值 (MB) | 2.61 |

## GC 原因分布

| 原因 | 次数 |
| --- | ---: |
| heuristic | 203 |

## GC 阶段分解

| 阶段 | 总时间 (ms) | 平均 (ms) | 占比 |
| --- | ---: | ---: | ---: |
| PreEnum | 185.81 | 0.92 | 30.9% |
| EnumRoots | 34.80 | 0.17 | 5.8% |
| ConcMark | 52.55 | 0.26 | 8.7% |
| MarkTerm | 1.95 | 0.01 | 0.3% |
| PostMark | 196.46 | 0.97 | 32.7% |
| ConcSweep | 81.18 | 0.40 | 13.5% |
| PostGC | 1.61 | 0.01 | 0.3% |
| Other | 47.32 | 0.23 | 7.9% |

## 最慢 GC 周期 (Top 10)

| Cycle | 原因 | 总时间 (ms) |
| --- | --- | ---: |
| 36 | heuristic | 54.761 |
| 176 | heuristic | 12.321 |
| 183 | heuristic | 6.952 |
| 134 | heuristic | 6.655 |
| 171 | heuristic | 6.230 |
| 196 | heuristic | 5.848 |
| 184 | heuristic | 5.770 |
| 116 | heuristic | 5.721 |
| 152 | heuristic | 5.555 |
| 191 | heuristic | 5.506 |

