# MSGC Benchmark Report

- 用例: case4_read_heavy
- GC 模式: marksweep
- 堆大小: 512mb
- 结果目录: `msgc`
- 时间: 2026-04-06T14:32:45Z

## 总览

| 指标 | 值 |
| --- | ---: |
| Benchmark 总时间 (ms) | 615591 |
| Monitor 总时间 (ms) | 616307 |
| RSS 平均 (MB) | 161.51 |
| RSS 峰值 (MB) | 175.42 |
| RSS P95 (MB) | 169.51 |
| CPU 平均 (%) | 168.96 |
| CPU 峰值 (%) | 175.90 |
| CPU P95 (%) | 174.00 |
| 采样点数 | 2883 |
| GC 次数 | 1264 |
| GC 总时间 (ms) | 47091.74 |
| GC 平均时间 (ms) | 37.26 |
| GC 最大时间 (ms) | 166.65 |
| GC P95 时间 (ms) | 39.83 |
| GC 平均间隔 (ms) | 58.79 |
| True live 平均 (MB) | 53.87 |
| True live 峰值 (MB) | 56.67 |

## GC 原因分布

| 原因 | 次数 |
| --- | ---: |
| heuristic | 1264 |

## GC 阶段分解

| 阶段 | 总时间 (ms) | 平均 (ms) | 占比 |
| --- | ---: | ---: | ---: |
| PreEnum | 157.24 | 0.12 | 0.3% |
| EnumRoots | 35.82 | 0.03 | 0.1% |
| ConcMark | 46377.75 | 36.69 | 98.5% |
| MarkTerm | 191.62 | 0.15 | 0.4% |
| PostMark | 23.66 | 0.02 | 0.1% |
| ConcSweep | 148.74 | 0.12 | 0.3% |
| PostGC | 10.92 | 0.01 | 0.0% |
| Other | 145.99 | 0.12 | 0.3% |

## 最慢 GC 周期 (Top 10)

| Cycle | 原因 | 总时间 (ms) |
| --- | --- | ---: |
| 10473 | heuristic | 166.645 |
| 10064 | heuristic | 162.308 |
| 10202 | heuristic | 135.162 |
| 10338 | heuristic | 131.037 |
| 9465 | heuristic | 79.508 |
| 10065 | heuristic | 74.559 |
| 10339 | heuristic | 73.325 |
| 9466 | heuristic | 60.444 |
| 10472 | heuristic | 59.834 |
| 10392 | heuristic | 51.055 |

