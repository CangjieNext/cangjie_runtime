# MSGC Benchmark Report

- 用例: case6_10g_scale
- GC 模式: copygc
- 堆大小: 12gb
- 结果目录: `copygc`
- 时间: 2026-04-06T14:42:54Z

## 总览

| 指标 | 值 |
| --- | ---: |
| Benchmark 总时间 (ms) | 257880 |
| Monitor 总时间 (ms) | 258780 |
| RSS 平均 (MB) | 1003.15 |
| RSS 峰值 (MB) | 2516.22 |
| RSS P95 (MB) | 1801.37 |
| CPU 平均 (%) | 109.83 |
| CPU 峰值 (%) | 185.60 |
| CPU P95 (%) | 156.77 |
| 采样点数 | 1203 |
| GC 次数 | 288 |
| GC 总时间 (ms) | 10387.97 |
| GC 平均时间 (ms) | 36.07 |
| GC 最大时间 (ms) | 270.93 |
| GC P95 时间 (ms) | 186.67 |
| GC 平均间隔 (ms) | 884.37 |
| True live 平均 (MB) | 41.70 |
| True live 峰值 (MB) | 209.27 |

## GC 原因分布

| 原因 | 次数 |
| --- | ---: |
| heuristic | 288 |

## GC 阶段分解

| 阶段 | 总时间 (ms) | 平均 (ms) | 占比 |
| --- | ---: | ---: | ---: |
| PreEnum | 526.09 | 1.83 | 5.1% |
| EnumRoots | 17.60 | 0.06 | 0.2% |
| ConcMark | 4455.31 | 15.47 | 42.9% |
| MarkTerm | 4806.03 | 16.69 | 46.3% |
| PostMark | 247.73 | 0.86 | 2.4% |
| ConcSweep | 247.57 | 0.86 | 2.4% |
| PostGC | 9.75 | 0.03 | 0.1% |
| Other | 77.89 | 0.27 | 0.7% |

## 最慢 GC 周期 (Top 10)

| Cycle | 原因 | 总时间 (ms) |
| --- | --- | ---: |
| 274 | heuristic | 270.930 |
| 285 | heuristic | 239.185 |
| 260 | heuristic | 227.916 |
| 282 | heuristic | 219.317 |
| 252 | heuristic | 218.867 |
| 284 | heuristic | 211.724 |
| 269 | heuristic | 211.423 |
| 271 | heuristic | 210.392 |
| 262 | heuristic | 206.727 |
| 281 | heuristic | 205.018 |

