# MSGC Benchmark Report

- 用例: case3_half_alive
- GC 模式: marksweep
- 堆大小: 2gb
- 结果目录: `msgc`
- 时间: 2026-04-06T15:46:49Z

## 总览

| 指标 | 值 |
| --- | ---: |
| Benchmark 总时间 (ms) | 118804 |
| Monitor 总时间 (ms) | 119288 |
| RSS 平均 (MB) | 83.56 |
| RSS 峰值 (MB) | 507.97 |
| RSS P95 (MB) | 258.86 |
| CPU 平均 (%) | 111.74 |
| CPU 峰值 (%) | 117.40 |
| CPU P95 (%) | 115.51 |
| 采样点数 | 559 |
| GC 次数 | 1590 |
| GC 总时间 (ms) | 1060.89 |
| GC 平均时间 (ms) | 0.67 |
| GC 最大时间 (ms) | 55.14 |
| GC P95 时间 (ms) | 1.14 |
| GC 平均间隔 (ms) | 59.77 |
| True live 平均 (MB) | 2.16 |
| True live 峰值 (MB) | 2.49 |

## GC 原因分布

| 原因 | 次数 |
| --- | ---: |
| heuristic | 1590 |

## GC 阶段分解

| 阶段 | 总时间 (ms) | 平均 (ms) | 占比 |
| --- | ---: | ---: | ---: |
| PreEnum | 195.75 | 0.12 | 18.5% |
| EnumRoots | 80.95 | 0.05 | 7.6% |
| ConcMark | 371.25 | 0.23 | 35.0% |
| MarkTerm | 20.41 | 0.01 | 1.9% |
| PostMark | 175.05 | 0.11 | 16.5% |
| ConcSweep | 33.28 | 0.02 | 3.1% |
| PostGC | 13.36 | 0.01 | 1.3% |
| Other | 170.85 | 0.11 | 16.1% |

## 最慢 GC 周期 (Top 10)

| Cycle | 原因 | 总时间 (ms) |
| --- | --- | ---: |
| 1598 | heuristic | 55.137 |
| 1583 | heuristic | 2.550 |
| 2014 | heuristic | 2.276 |
| 1311 | heuristic | 1.957 |
| 1173 | heuristic | 1.933 |
| 1386 | heuristic | 1.927 |
| 1076 | heuristic | 1.901 |
| 827 | heuristic | 1.817 |
| 1212 | heuristic | 1.803 |
| 2043 | heuristic | 1.799 |

