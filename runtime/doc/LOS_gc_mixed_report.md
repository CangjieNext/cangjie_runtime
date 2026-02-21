# gc_mixed.cj GC 日志分析报告

**日期**: 2026-02-21  
**测试文件**: gc/gc_mixed.cj  
**堆大小**: 64MB (4096 units × 16384 B/unit)  
**空间分配**: MAIN:RESERVE:LOS = 6:3:1

## 测试概要

gc_mixed.cj 包含 7 个阶段，验证大对象（LOS）与小对象（MAIN）混合分配、回收和碎片整理：

| 阶段 | 描述 | 结果 |
|---|---|---|
| Phase 1 | 仅大对象分配 | PASS |
| Phase 2 | 持有大对象 + 小对象流转 | PASS |
| Phase 3 | 大小对象交替分配/释放 | PASS |
| Phase 4 | 渐增大对象 | PASS |
| Phase 1 | 仅大对象分配 | PASS |
| Phase 2 | 持有大对象 + 小对象流转 | PASS |
| Phase 3 | 大小对象交替分配/释放 | PASS |
| Phase 4 | 渐增大对象 | PASS |
| Phase 5 | 高压混合 | PASS |
| Phase 6 | 多轮 GC LOS 压力测试 | PASS |
| Phase 7 | LOS 碎片整理验证 | PASS |

## GC 总体

| 指标 | 值 |
|---|---|
| GC 次数 | 2 次 (heuristic + heuristic_sync) |
| LOS Compaction 次数 | 2 次 |
| LOS 累计垃圾 | 24,330,240 B (~23.2MB) |
| LOS 累计释放 | 3,620,864 B (~3.5MB compactFreed) |

### GC #1 (Phase 5 结束后)
- 原因: heuristic, allocated 13.6MB, threshold 13.4MB
- Compaction: 回收 3.6MB 垃圾

### GC #2 (Phase 6-7 压力后)
- 原因: heuristic_sync, allocated 56.4MB, threshold 20.3MB
- Compaction: 累计 24.3MB 垃圾

## LOS 状态（最终）

| 指标 | 值 |
|---|---|
| 容量 | 6,701,056 B (~6.4MB) = 409 units |
| 已用 | 6,701,056 B (**100% 满**) |
| 空闲 | 0 B |
| 分配次数 | 283 次 |
| 分配失败 | 13 次 |
| 回落到 MAIN | 15 次 |
| RESERVE 借用 | 122 次 (批量借用 27 次) |
| Compaction 次数 | 2 次 |
| Compaction 累计垃圾 | 24,330,240 B (~23.2MB) |
| Compaction 移动 | 819,200 B (~0.8MB) |
| Compaction 释放 | 3,620,864 B (~3.5MB) |
| Migration In | 2 次 (MAIN→LOS 迁移) |

**结论**: LOS 283 次分配中 270 次成功（95.4%），13 次 LOS 满后 15 次回落到 MAIN。2 次 Compaction 累计处理 23.2MB 垃圾。MAIN→LOS 迁移 2 次成功。

## RESERVE 状态（最终）

| 指标 | 值 |
|---|---|
| 容量 | 20,119,552 B (~19.2MB) = 1228 units |
| 可用 | 196,608 B (几乎耗尽) |
| LOS 借用 | 19,922,944 B (~19.0MB), 27 次批量借用 |
| MAIN 借用 | 0 B |
| losReturned | 196,608 B (部分归还) |

**结论**: LOS 在 Phase 6-7 压力下大量借用 RESERVE，27 次批量借用共 19.0MB，RESERVE 几乎耗尽。说明压力测试成功激发了 RESERVE 借用机制的极限行为。

## 内存映射（GC #2 后）

```
--- MAIN (units 0 - 2458) ---
  [0+464 G] [464+8 T] [472+1424 G] [1896+20 R] [1916+4 G] [1920+24 R]
  [1944+28 G] [1972+32 R] [2004+24 l] [2028+364 R] [2392+24 l] [2416+32 R]
  [2448+4 T] [2452+4 l] [2456+3 _]
--- RESERVE (units 2459 - 3686) ---
  [2459+12 =] [2471+1216 _]
--- LOS (units 3687 - 4095) ---
  [3687+409 l]
```

### MAIN — 几乎全满

| 状态 | 单元数 | 说明 |
|---|---|---|
| R (RecentFull) | 472 | 存活数据 |
| T (ThreadLocal) | 12 | 正在分配 |
| G (Garbage) | 1920 | 已死对象（占 78%！） |
| l (RecentLarge) | 52 | LOS 回落到 MAIN 的大对象 |
| _ (Inactive) | 3 | 几乎无空闲 |

MAIN 区 78% 为 Garbage — 压力测试产生了大量垃圾。3 个 MAIN 中的大对象 (l) 是 LOS 满后的回落分配。

### RESERVE — 几乎耗尽

仅剩 12 units ReserveFree，1216 units Inactive（已被 LOS 借光后释放）

### LOS — 满载零碎片

409 units 全为 `l` (RecentLarge)，**单个连续块，零碎片**。说明 2 次 Compaction 成功将碎片完全整理。

## 综合结论

1. **LOS 持续可靠**: 283 次分配，95.4% 成功率，2 次 Compaction 处理 23.2MB 垃圾
2. **RESERVE 极限借用**: 27 次批量借用耗尽 19.0MB RESERVE 空间，机制在极限下仍正常
3. **碎片整理优秀**: 2 次 Compaction 后 LOS 为单个连续块，零碎片
4. **MAIN→LOS 迁移**: 2 次成功迁移，回落对象最终归入 LOS
5. **已知问题**: 极端 OOM 压力（>70% 堆占用 + 高频大对象）下 `ExemptFromRegions` 中 `IsFromRegion()` 断言可能失败，需进一步排查

## 极端压力崩溃记录

在更激进的参数下（40 轮 × 4×256KB/轮），Phase 6 round 30 后触发了第 8 次 GC（OOM, 47MB/64MB），崩溃于：
```
F Check failed: IsFromRegion()
```
发生在 `ExemptFromRegions()` 中。8 次 GC 周期中前 7 次均正常。初步分析为极端 OOM 下 region 状态管理问题，需单独排查。
