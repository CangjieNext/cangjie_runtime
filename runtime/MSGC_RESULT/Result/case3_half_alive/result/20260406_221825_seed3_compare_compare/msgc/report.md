# MSGC Benchmark Report

- 用例: case3_half_alive
- GC 模式: marksweep
- 堆大小: 2gb
- 结果目录: `msgc`
- 时间: 2026-04-06T14:20:31Z

## 总览

| 指标 | 值 |
| --- | ---: |
| Benchmark 总时间 (ms) | 117426 |
| Monitor 总时间 (ms) | 117995 |
| RSS 平均 (MB) | 118.90 |
| RSS 峰值 (MB) | 511.56 |
| RSS P95 (MB) | 306.83 |
| CPU 平均 (%) | 111.63 |
| CPU 峰值 (%) | 118.20 |
| CPU P95 (%) | 115.60 |
| 采样点数 | 553 |
| GC 次数 | 1616 |
| GC 总时间 (ms) | 1052.12 |
| GC 平均时间 (ms) | 0.65 |
| GC 最大时间 (ms) | 19.06 |
| GC P95 时间 (ms) | 1.16 |
| GC 平均间隔 (ms) | 55.59 |
| True live 平均 (MB) | 2.16 |
| True live 峰值 (MB) | 3.01 |

## GC 原因分布

| 原因 | 次数 |
| --- | ---: |
| heuristic | 1616 |

## GC 阶段分解

| 阶段 | 总时间 (ms) | 平均 (ms) | 占比 |
| --- | ---: | ---: | ---: |
| PreEnum | 184.23 | 0.11 | 17.5% |
| EnumRoots | 46.09 | 0.03 | 4.4% |
| ConcMark | 410.24 | 0.25 | 39.0% |
| MarkTerm | 18.56 | 0.01 | 1.8% |
| PostMark | 174.52 | 0.11 | 16.6% |
| ConcSweep | 30.34 | 0.02 | 2.9% |
| PostGC | 22.26 | 0.01 | 2.1% |
| Other | 165.86 | 0.10 | 15.8% |

## 最慢 GC 周期 (Top 10)

| Cycle | 原因 | 总时间 (ms) |
| --- | --- | ---: |
| 1433 | heuristic | 19.059 |
| 1287 | heuristic | 15.042 |
| 1793 | heuristic | 11.717 |
| 1159 | heuristic | 7.654 |
| 1331 | heuristic | 2.652 |
| 1303 | heuristic | 2.620 |
| 1276 | heuristic | 2.525 |
| 1286 | heuristic | 2.378 |
| 1668 | heuristic | 2.357 |
| 1742 | heuristic | 2.271 |

