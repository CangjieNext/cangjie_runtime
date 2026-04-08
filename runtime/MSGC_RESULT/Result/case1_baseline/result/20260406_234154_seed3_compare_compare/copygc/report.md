# CopyGC Benchmark Report

- 用例: case1_baseline
- GC 模式: copygc
- 堆大小: 256mb
- 结果目录: `copygc`
- 时间: 2026-04-06T15:41:54Z

## 总览

| 指标 | 值 |
| --- | ---: |
| Benchmark 总时间 (ms) | 5404 |
| Monitor 总时间 (ms) | 6252 |
| RSS 平均 (MB) | 64.56 |
| RSS 峰值 (MB) | 154.25 |
| RSS P95 (MB) | 139.07 |
| CPU 平均 (%) | 86.00 |
| CPU 峰值 (%) | 112.30 |
| CPU P95 (%) | 111.46 |
| 采样点数 | 29 |
| GC 次数 | 36 |
| GC 总时间 (ms) | 205.12 |
| GC 平均时间 (ms) | 5.70 |
| GC 最大时间 (ms) | 39.17 |
| GC P95 时间 (ms) | 33.25 |
| GC 平均间隔 (ms) | 152.45 |

## GC 原因分布

| 原因 | 次数 |
| --- | ---: |
| heuristic | 36 |

## Benchmark 阶段

| 阶段 | 时间 (ms) |
| --- | ---: |
| P1_SmallAlloc | 246 |
| P2_LargeAlloc | 897 |
| P3_MixedWorkload | 560 |
| P4_LongLivedChurn | 449 |
| P5_Fragmentation | 702 |

## 最慢 GC 周期 (Top 10)

| Cycle | 原因 | 总时间 (ms) |
| --- | --- | ---: |
| 18446744073709551615 | heuristic | 39.166 |
| 18446744073709551615 | heuristic | 37.420 |
| 18446744073709551615 | heuristic | 31.855 |
| 18446744073709551615 | heuristic | 11.812 |
| 18446744073709551615 | heuristic | 10.452 |
| 18446744073709551615 | heuristic | 3.281 |
| 18446744073709551615 | heuristic | 3.220 |
| 18446744073709551615 | heuristic | 3.208 |
| 18446744073709551615 | heuristic | 3.037 |
| 18446744073709551615 | heuristic | 3.015 |

