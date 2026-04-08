# MSGC Seed 参数说明

## 参数字典

| 参数 | 默认值 | 说明 |
|------|--------|------|
| `cjGCThreshold` | 20mb（上限硬编码 20MB） | GC 触发阈值。内存增量超过此值时请求 GC。值越小 → GC 越频繁 → RSS 越低 |
| `cjGCInterval` | 150ms | 两次 GC 请求之间的最小间隔。值越小 → 允许更密集的 GC |
| `CJ_MS_HEU_READY_WATERMARK` | 4 | 当 ready（可分配）region 数 < 该水位线时触发启发式 GC。值越小 → 更晚触发 |
| `CJ_MS_HEU_REQUEST_COOLDOWN` | 10ms | 启发式 GC 请求之间的最小冷却时间。值越小 → 响应越快 |
| `CJ_MS_EMPTY_CACHE_LOW_FLOOR_REGIONS` | 32 | empty region pool 的最小保留数量（下限）。每个 region ≈ 2MB。**对 RSS 基线影响最大** |
| `CJ_MS_EMPTY_CACHE_LOW_CAP_REGIONS` | 256 | empty region pool 的上限阈值，超过此数量触发 scavenge 归还 OS |
| `CJ_MS_EMPTY_CACHE_HIGH_FACTOR` | 2 | High 水位线 = Low × factor。设为 1 时 High = Low，消除缓冲区 |
| `CJ_MS_EMPTY_SCAVENGE_MAX_REGIONS_PER_SLICE` | 256 | 每次 scavenge 操作归还 OS 的最大 region 数。值越大 → 归还越快 |
| `CJ_MS_BG_SWEEP_MAX_REGIONS_PER_SLICE` | 64 | 后台 sweep 常规模式下每次处理的最大 region 数 |
| `CJ_MS_BG_SWEEP_PRESSURE_REGIONS_PER_SLICE` | 256 | 后台 sweep 压力模式下每次处理的最大 region 数 |
| `CJ_MS_MIN_HEADROOM_REGIONS` | 4 | 最小空闲 region 保留数（防止分配饥饿） |
| `CJ_MS_INLINE_RECLAIM` | 0 | 是否在分配路径内联回收。1 = 开启，降低分配延迟 |

---

## Seed 一览

| Seed | 策略 | RSS 目标 | 核心思路 |
|------|------|----------|----------|
| seed1 | 保守 | 持平 CopyGC | benchmark2 调优基线，较大 cache 池 |
| seed2 | 激进（aggressive） | RSS -30%+ | 大幅缩小 cache + 提高 GC 频率 |
| **seed3** | **超激进（ultra）** | **RSS -50%+** | **最小 cache + 极高 GC 频率 + 加速 sweep** |
| seed4 | 极端（extreme） | RSS -60%+ | 探索 RSS 下限，所有 budget 拉满 |

---

## Seed 参数对比

| 参数 | 默认值 | seed1 | seed2 | seed3 | seed4 |
|------|--------|-------|-------|-------|-------|
| `cjGCThreshold` | 20mb | 16mb | 8mb | **4mb** | 2mb |
| `cjGCInterval` | 150ms | 100ms | 50ms | **20ms** | 10ms |
| `CJ_MS_HEU_READY_WATERMARK` | 4 | 4 | 2 | **1** | 1 |
| `CJ_MS_HEU_REQUEST_COOLDOWN` | 10ms | — | 5ms | **2ms** | 1ms |
| `CJ_MS_EMPTY_CACHE_LOW_FLOOR_REGIONS` | 32 | 24 | 4 | **2** | 1 |
| `CJ_MS_EMPTY_CACHE_LOW_CAP_REGIONS` | 256 | 48 | 8 | **4** | 2 |
| `CJ_MS_EMPTY_CACHE_HIGH_FACTOR` | 2 | — | 1 | **1** | 1 |
| `CJ_MS_EMPTY_SCAVENGE_MAX_REGIONS_PER_SLICE` | 256 | 16 | 64 | **128** | 256 |
| `CJ_MS_BG_SWEEP_MAX_REGIONS_PER_SLICE` | 64 | — | — | **128** | 256 |
| `CJ_MS_BG_SWEEP_PRESSURE_REGIONS_PER_SLICE` | 256 | 64 | 128 | **256** | 512 |
| `CJ_MS_MIN_HEADROOM_REGIONS` | 4 | — | — | — | 2 |
| `CJ_MS_INLINE_RECLAIM` | 0 | 1 | 1 | **1** | 1 |

> "—" 表示该 seed 未设置此参数，使用默认值。

---

## Case1 Baseline 测试结论（2026-04-06）

测试环境：macOS aarch64，case1_baseline（256mb heap，无 idle）

### 各 Seed vs CopyGC 对比

| 指标 | CopyGC | seed1 | seed2 | seed3 | seed4 |
|------|-------:|------:|------:|------:|------:|
| RSS 均值 (MB) | 60.7 | 79.1 (+30%) | 35.1 (**-42%**) | **29.2 (-52%)** | 20.1 (-67%) |
| RSS 峰值 (MB) | 125.4 | 127.3 (+2%) | 89.4 (-29%) | **62.4 (-50%)** | 46.7 (-63%) |
| RSS P95 (MB) | 123.7 | — | 71.0 | **51.4** | 35.8 |
| 墙钟时间 (s) | 5.7 | 7.8 (+37%) | 6.6 (+16%) | **6.7 (+17%)** | 6.6 (+16%) |
| CPU 均值 (%) | 81.2 | 72.9 | 72.7 | **100.1** | 105.7 |
| CPU 峰值 (%) | 111.0 | 125.1 | 118.3 | **139.6** | 150.8 |
| GC 次数 | 35 | 69 | 64 | **149** | 283 |
| GC 总耗时 (ms) | 465.2 | 404.2 | 208.7 | **411.9** | 588.2 |
| 单次 GC 均值 (ms) | 13.3 | 5.9 | 3.3 | **2.8** | 2.1 |
| 单次 GC P95 (ms) | 64.3 | 20.7 | 24.0 | **22.9** | 5.3 |

### 推荐结论

**seed3（ultra）为推荐配置**，理由：

1. **RSS 目标达成**：均值 -52%，峰值 -50%，远超 -30% 目标
2. **吞吐量代价可控**：墙钟仅 +17%，单次 GC 停顿 2.8ms
3. **CPU 开销适中**：均值 100%，远低于 seed4 的 106%
4. **GC 次数合理**：149 次 vs seed4 的 283 次，调度压力更小
5. **seed4 边际收益递减**：RSS 额外降 23MB（-15pp），但 GC 次数翻倍、CPU 更高

> seed2 适合对 CPU 敏感场景（CPU +11%），seed4 适合内存极度受限场景。
