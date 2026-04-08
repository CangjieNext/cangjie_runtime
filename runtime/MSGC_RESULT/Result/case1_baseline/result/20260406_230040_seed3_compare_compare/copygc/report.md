# CopyGC Benchmark Report

- 用例: case1_baseline
- GC 模式: copygc
- 堆大小: 256mb
- 结果目录: `copygc`
- 时间: 2026-04-06T15:00:40Z

## 总览

| 指标 | 值 |
| --- | ---: |
| Benchmark 总时间 (ms) | 5246 |
| Monitor 总时间 (ms) | 5980 |
| RSS 平均 (MB) | 64.77 |
| RSS 峰值 (MB) | 197.89 |
| RSS P95 (MB) | 148.23 |
| CPU 平均 (%) | 83.72 |
| CPU 峰值 (%) | 108.60 |
| CPU P95 (%) | 106.06 |
| 采样点数 | 28 |
| GC 次数 | 34 |
| GC 总时间 (ms) | 214.52 |
| GC 平均时间 (ms) | 6.31 |
| GC 最大时间 (ms) | 48.89 |
| GC P95 时间 (ms) | 38.07 |
| GC 平均间隔 (ms) | 156.67 |

## GC 原因分布

| 原因 | 次数 |
| --- | ---: |
| heuristic | 34 |

## Benchmark 阶段

| 阶段 | 时间 (ms) |
| --- | ---: |
| P1_SmallAlloc | 241 |
| P2_LargeAlloc | 869 |
| P3_MixedWorkload | 540 |
| P4_LongLivedChurn | 439 |
| P5_Fragmentation | 678 |

## 最慢 GC 周期 (Top 10)

| Cycle | 原因 | 总时间 (ms) |
| --- | --- | ---: |
| 18446744073709551615 | heuristic | 48.889 |
| 18446744073709551615 | heuristic | 38.641 |
| 18446744073709551615 | heuristic | 37.762 |
| 18446744073709551615 | heuristic | 10.444 |
| 18446744073709551615 | heuristic | 9.966 |
| 18446744073709551615 | heuristic | 2.943 |
| 18446744073709551615 | heuristic | 2.915 |
| 18446744073709551615 | heuristic | 2.799 |
| 18446744073709551615 | heuristic | 2.791 |
| 18446744073709551615 | heuristic | 2.789 |

