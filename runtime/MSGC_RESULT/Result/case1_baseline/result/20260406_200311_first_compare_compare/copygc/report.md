# CopyGC Benchmark Report

- 用例: unknown
- GC 模式: copygc
- 堆大小: unknown
- 结果目录: `copygc`
- 时间: unknown

## 总览

| 指标 | 值 |
| --- | ---: |
| Benchmark 总时间 (ms) | 5387 |
| Monitor 总时间 (ms) | 6365 |
| RSS 平均 (MB) | 61.90 |
| RSS 峰值 (MB) | 141.42 |
| RSS P95 (MB) | 118.98 |
| CPU 平均 (%) | 84.74 |
| CPU 峰值 (%) | 112.70 |
| CPU P95 (%) | 111.43 |
| 采样点数 | 30 |
| GC 次数 | 36 |
| GC 总时间 (ms) | 200.06 |
| GC 平均时间 (ms) | 5.56 |
| GC 最大时间 (ms) | 40.43 |
| GC P95 时间 (ms) | 18.46 |
| GC 平均间隔 (ms) | 152.82 |

## GC 原因分布

| 原因 | 次数 |
| --- | ---: |
| heuristic | 36 |

## Benchmark 阶段

| 阶段 | 时间 (ms) |
| --- | ---: |
| P1_SmallAlloc | 253 |
| P2_LargeAlloc | 903 |
| P3_MixedWorkload | 555 |
| P4_LongLivedChurn | 451 |
| P5_Fragmentation | 696 |

## 最慢 GC 周期 (Top 10)

| Cycle | 原因 | 总时间 (ms) |
| --- | --- | ---: |
| 18446744073709551615 | heuristic | 40.433 |
| 18446744073709551615 | heuristic | 37.405 |
| 18446744073709551615 | heuristic | 12.148 |
| 18446744073709551615 | heuristic | 10.607 |
| 18446744073709551615 | heuristic | 10.556 |
| 18446744073709551615 | heuristic | 9.938 |
| 18446744073709551615 | heuristic | 4.663 |
| 18446744073709551615 | heuristic | 4.392 |
| 18446744073709551615 | heuristic | 4.010 |
| 18446744073709551615 | heuristic | 2.987 |

