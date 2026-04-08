# MSGC Benchmark Report

- 用例: case6_10g_scale
- GC 模式: copygc
- 堆大小: 12gb
- 结果目录: `copygc`
- 时间: 2026-04-06T15:50:41Z

## 总览

| 指标 | 值 |
| --- | ---: |
| Benchmark 总时间 (ms) | 255863 |
| Monitor 总时间 (ms) | 256845 |
| RSS 平均 (MB) | 1055.49 |
| RSS 峰值 (MB) | 2485.44 |
| RSS P95 (MB) | 1724.37 |
| CPU 平均 (%) | 110.98 |
| CPU 峰值 (%) | 187.00 |
| CPU P95 (%) | 159.41 |
| 采样点数 | 1200 |
| GC 次数 | 281 |
| GC 总时间 (ms) | 10521.05 |
| GC 平均时间 (ms) | 37.44 |
| GC 最大时间 (ms) | 468.77 |
| GC P95 时间 (ms) | 188.71 |
| GC 平均间隔 (ms) | 903.04 |
| True live 平均 (MB) | 41.66 |
| True live 峰值 (MB) | 210.48 |

## GC 原因分布

| 原因 | 次数 |
| --- | ---: |
| heuristic | 281 |

## GC 阶段分解

| 阶段 | 总时间 (ms) | 平均 (ms) | 占比 |
| --- | ---: | ---: | ---: |
| PreEnum | 602.04 | 2.14 | 5.7% |
| EnumRoots | 12.88 | 0.05 | 0.1% |
| ConcMark | 4983.49 | 17.73 | 47.4% |
| MarkTerm | 4315.23 | 15.36 | 41.0% |
| PostMark | 273.73 | 0.97 | 2.6% |
| ConcSweep | 237.71 | 0.85 | 2.3% |
| PostGC | 8.92 | 0.03 | 0.1% |
| Other | 87.03 | 0.31 | 0.8% |

## 最慢 GC 周期 (Top 10)

| Cycle | 原因 | 总时间 (ms) |
| --- | --- | ---: |
| 261 | heuristic | 468.771 |
| 246 | heuristic | 430.780 |
| 271 | heuristic | 240.286 |
| 241 | heuristic | 229.162 |
| 277 | heuristic | 217.612 |
| 259 | heuristic | 216.834 |
| 275 | heuristic | 210.120 |
| 260 | heuristic | 209.062 |
| 268 | heuristic | 203.535 |
| 266 | heuristic | 202.761 |

