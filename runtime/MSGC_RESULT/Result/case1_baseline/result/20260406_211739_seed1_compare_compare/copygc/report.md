# CopyGC Benchmark Report

- 用例: case1_baseline
- GC 模式: copygc
- 堆大小: 256mb
- 结果目录: `copygc`
- 时间: 2026-04-06T13:17:39Z

## 总览

| 指标 | 值 |
| --- | ---: |
| Benchmark 总时间 (ms) | 5693 |
| Monitor 总时间 (ms) | 6600 |
| RSS 平均 (MB) | 60.72 |
| RSS 峰值 (MB) | 125.38 |
| RSS P95 (MB) | 123.75 |
| CPU 平均 (%) | 81.17 |
| CPU 峰值 (%) | 111.00 |
| CPU P95 (%) | 108.32 |
| 采样点数 | 30 |
| GC 次数 | 35 |
| GC 总时间 (ms) | 465.16 |
| GC 平均时间 (ms) | 13.29 |
| GC 最大时间 (ms) | 161.08 |
| GC P95 时间 (ms) | 64.32 |
| GC 平均间隔 (ms) | 164.58 |

## GC 原因分布

| 原因 | 次数 |
| --- | ---: |
| heuristic | 35 |

## Benchmark 阶段

| 阶段 | 时间 (ms) |
| --- | ---: |
| P1_SmallAlloc | 373 |
| P2_LargeAlloc | 974 |
| P3_MixedWorkload | 554 |
| P4_LongLivedChurn | 466 |
| P5_Fragmentation | 691 |

## 最慢 GC 周期 (Top 10)

| Cycle | 原因 | 总时间 (ms) |
| --- | --- | ---: |
| 18446744073709551615 | heuristic | 161.080 |
| 18446744073709551615 | heuristic | 117.029 |
| 18446744073709551615 | heuristic | 41.734 |
| 18446744073709551615 | heuristic | 38.038 |
| 18446744073709551615 | heuristic | 11.262 |
| 18446744073709551615 | heuristic | 10.685 |
| 18446744073709551615 | heuristic | 5.891 |
| 18446744073709551615 | heuristic | 5.625 |
| 18446744073709551615 | heuristic | 5.098 |
| 18446744073709551615 | heuristic | 4.692 |

