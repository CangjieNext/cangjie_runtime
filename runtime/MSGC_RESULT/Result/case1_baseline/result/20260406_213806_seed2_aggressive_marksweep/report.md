# MSGC Benchmark Report

- 用例: case1_baseline
- GC 模式: marksweep
- 堆大小: 256mb
- 结果目录: `20260406_213806_seed2_aggressive_marksweep`
- 时间: 2026-04-06T13:38:06Z

## 总览

| 指标 | 值 |
| --- | ---: |
| Benchmark 总时间 (ms) | 6618 |
| Monitor 总时间 (ms) | 10018 |
| RSS 平均 (MB) | 35.11 |
| RSS 峰值 (MB) | 89.38 |
| RSS P95 (MB) | 71.00 |
| CPU 平均 (%) | 72.74 |
| CPU 峰值 (%) | 118.30 |
| CPU P95 (%) | 117.51 |
| 采样点数 | 47 |
| GC 次数 | 64 |
| GC 总时间 (ms) | 208.66 |
| GC 平均时间 (ms) | 3.26 |
| GC 最大时间 (ms) | 31.81 |
| GC P95 时间 (ms) | 23.98 |
| GC 平均间隔 (ms) | 103.57 |
| True live 平均 (MB) | 2.54 |
| True live 峰值 (MB) | 29.12 |

## GC 原因分布

| 原因 | 次数 |
| --- | ---: |
| heuristic | 64 |

## Benchmark 阶段

| 阶段 | 时间 (ms) |
| --- | ---: |
| P1_SmallAlloc | 306 |
| P2_LargeAlloc | 1283 |
| P3_MixedWorkload | 687 |
| P4_LongLivedChurn | 551 |
| P5_Fragmentation | 948 |

## GC 阶段分解

| 阶段 | 总时间 (ms) | 平均 (ms) | 占比 |
| --- | ---: | ---: | ---: |
| PreEnum | 9.51 | 0.15 | 4.6% |
| EnumRoots | 1.93 | 0.03 | 0.9% |
| ConcMark | 136.46 | 2.13 | 65.4% |
| MarkTerm | 4.29 | 0.07 | 2.1% |
| PostMark | 45.56 | 0.71 | 21.8% |
| ConcSweep | 2.92 | 0.05 | 1.4% |
| PostGC | 0.65 | 0.01 | 0.3% |
| Other | 7.34 | 0.11 | 3.5% |

## 最慢 GC 周期 (Top 10)

| Cycle | 原因 | 总时间 (ms) |
| --- | --- | ---: |
| 6 | heuristic | 31.814 |
| 5 | heuristic | 27.364 |
| 4 | heuristic | 27.174 |
| 7 | heuristic | 26.971 |
| 29 | heuristic | 7.036 |
| 47 | heuristic | 3.520 |
| 46 | heuristic | 3.474 |
| 26 | heuristic | 2.443 |
| 28 | heuristic | 2.376 |
| 52 | heuristic | 2.333 |

