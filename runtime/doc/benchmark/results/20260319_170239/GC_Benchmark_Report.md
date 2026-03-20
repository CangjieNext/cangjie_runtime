# GC Benchmark 对比报告

生成时间: 2026-03-19 17:05:19

## 功能完整性

| GC 模式 | 堆大小 | F1_TreeIntegrity | F2_Generational | F3_LargeObject | F4_LinkedList | F5_HashMap | 通过率 |
|---------|--------|------|------|------|------|------|--------|
| CopyGC | 256MB | PASS | PASS | PASS | PASS | PASS | 5/5 |
| CopyGC | 512MB | PASS | PASS | PASS | PASS | PASS | 5/5 |
| MS-GC | 256MB | PASS | PASS | PASS | PASS | PASS | 5/5 |
| MS-GC | 512MB | PASS | PASS | PASS | PASS | PASS | 5/5 |

## 执行性能（ms，3 轮平均）

| 测试项 | CopyGC 256M | CopyGC 512M | MS-GC 256M | MS-GC 512M |
|--------||--------|--------|--------|--------|
| P1_SmallAlloc | 226 | 221 | 366 | 322 |
| P2_LargeAlloc | 834 | 807 | 1163 | 1168 |
| P3_MixedWorkload | 510 | 502 | 708 | 712 |
| P4_LongLivedChurn | 396 | 398 | 567 | 578 |
| P5_Fragmentation | 644 | 700 | 980 | 1141 |
| **Total** | **4754** | **4744** | **6765** | **6826** |

## 资源使用率（3 轮平均）

| 指标 | CopyGC 256M | CopyGC 512M | MS-GC 256M | MS-GC 512M |
|------||--------|--------|--------|--------|
| 平均 CPU% | 90.3 | 93.5 | 98.0 | 100.4 |
| 峰值 CPU% | 103.8 | 104.1 | 116.7 | 116.4 |
| 平均 RSS (MB) | 136.9 | 142.4 | 83.1 | 80.8 |
| 峰值 RSS (MB) | 219.3 | 210.8 | 175.5 | 164.3 |

## MS-GC vs CopyGC 对比倍率

| 指标 | 256MB | 512MB |
|------|-------|-------|
| Total 性能 | 1.42× | 1.44× |
| 峰值 RSS | 0.80× | 0.78× |
| 平均 CPU% | 1.09× | 1.07× |
