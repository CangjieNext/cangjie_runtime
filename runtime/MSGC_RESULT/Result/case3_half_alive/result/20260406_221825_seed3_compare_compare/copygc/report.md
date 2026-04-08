# MSGC Benchmark Report

- 用例: case3_half_alive
- GC 模式: copygc
- 堆大小: 2gb
- 结果目录: `copygc`
- 时间: 2026-04-06T14:18:25Z

## 总览

| 指标 | 值 |
| --- | ---: |
| Benchmark 总时间 (ms) | 123083 |
| Monitor 总时间 (ms) | 123781 |
| RSS 平均 (MB) | 295.00 |
| RSS 峰值 (MB) | 1013.30 |
| RSS P95 (MB) | 519.78 |
| CPU 平均 (%) | 110.78 |
| CPU 峰值 (%) | 140.20 |
| CPU P95 (%) | 132.40 |
| 采样点数 | 581 |
| GC 次数 | 189 |
| GC 总时间 (ms) | 531.18 |
| GC 平均时间 (ms) | 2.81 |
| GC 最大时间 (ms) | 7.72 |
| GC P95 时间 (ms) | 4.98 |
| GC 平均间隔 (ms) | 653.36 |
| True live 平均 (MB) | 2.02 |
| True live 峰值 (MB) | 2.21 |

## GC 原因分布

| 原因 | 次数 |
| --- | ---: |
| heuristic | 189 |

## GC 阶段分解

| 阶段 | 总时间 (ms) | 平均 (ms) | 占比 |
| --- | ---: | ---: | ---: |
| PreEnum | 166.60 | 0.88 | 31.4% |
| EnumRoots | 6.02 | 0.03 | 1.1% |
| ConcMark | 47.05 | 0.25 | 8.9% |
| MarkTerm | 2.15 | 0.01 | 0.4% |
| PostMark | 187.72 | 0.99 | 35.3% |
| ConcSweep | 85.23 | 0.45 | 16.0% |
| PostGC | 1.42 | 0.01 | 0.3% |
| Other | 34.98 | 0.19 | 6.6% |

## 最慢 GC 周期 (Top 10)

| Cycle | 原因 | 总时间 (ms) |
| --- | --- | ---: |
| 166 | heuristic | 7.718 |
| 167 | heuristic | 6.040 |
| 159 | heuristic | 5.699 |
| 153 | heuristic | 5.665 |
| 136 | heuristic | 5.630 |
| 140 | heuristic | 5.615 |
| 156 | heuristic | 5.350 |
| 132 | heuristic | 5.338 |
| 185 | heuristic | 5.309 |
| 163 | heuristic | 4.994 |

