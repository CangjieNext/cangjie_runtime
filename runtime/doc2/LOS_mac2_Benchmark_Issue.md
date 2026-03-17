# LOS-mac2 分支 gc_benchmark 性能测试记录

> 测试日期: 2026-03-16 | 平台: macOS aarch64 (Apple Silicon) | 堆大小: 256MB
> SDK: dev/20260220 | 测试用例: gc/gc.cj (gc_benchmark)

## 1. 测试目的

评估 LOS-mac2 分支（完整 LOS 实现）相对于 Origin (dev 分支) 在 gc_benchmark Test 12 (Mixed OOM) 上的性能表现和稳定性。

## 2. 测试环境

- **Origin dylib**: SDK 自带原始 runtime (898KB, dev 分支)
- **LOS-mac2 dylib**: 当前 LOS-mac2 分支 `python3 build.py build` 编译产物 (1497KB)
- **测试二进制**: `cjc gc.cj -o gc_benchmark` (使用 dev/20260220 SDK 编译)
- **运行方式**: `cjHeapSize=256mb ./gc_benchmark`

## 3. 测试结果

### 3.1 Origin (dev 分支)

| Run | 结果 | OOM Round | 内存 |
|-----|------|-----------|------|
| R1 | OOM | 260 | ~151MB |
| R2 | OOM | 260 | ~151MB |

**结论**: Origin 稳定，一致在 round 260 OOM，~151MB。无 crash。

### 3.2 LOS-mac2

| Run | 结果 | OOM Round | 内存 | 备注 |
|-----|------|-----------|------|------|
| R1 | **SIGSEGV** | - | - | Test 5/6 HashMap.resize, addr=0x16 |
| R2 | OOM ✅ | **360** | **~215MB** | |
| R3 | **SIGABRT** | - | - | Test 6 HashMap Churn round 35 |
| R4 | OOM ✅ | **360** | **~215MB** | |
| R5 | **SIGSEGV** | - | - | Test 5/6 HashMap.resize, addr=0x8 |
| R6 | OOM ✅ | **360** | **~215MB** | |
| R7 | OOM ✅ | **360** | **~215MB** | |
| R8 | OOM ✅ | **360** | **~215MB** | |

## 4. 性能对比（成功运行时）

| 指标 | Origin (dev) | LOS-mac2 | 变化 |
|------|-------------|----------|------|
| OOM Round | 260 | **360** | **+38.5%** |
| OOM 时内存 | ~151MB | **~215MB** | **+64MB** |
| 多坚持轮数 | - | +100 轮 | |

LOS-mac2 在成功运行时，OOM round 固定在 360，数据完全一致。

## 5. 稳定性问题

### 5.1 Crash 统计

- **总运行**: 8 次
- **成功 (正常 OOM)**: 5 次 (62.5%)
- **Crash**: 3 次 (37.5%)
  - SIGSEGV × 2 (Run 1, Run 5)
  - SIGABRT × 1 (Run 3)

### 5.2 Crash 模式分析

所有 crash 都发生在 **Test 5 (ArrayList Stress)** 或 **Test 6 (HashMap Churn)** 期间。

**一致的调用栈** (SIGSEGV):
```
HashMap.resize → TraceBarrier::WriteGeneric → TraceBarrier::WriteStruct
  → ForEachAggRefFieldInArray → GCTib::ForEachBitmapWord
  → Mutator::RememberObjectImpl → RegionSpace::ShouldEnqueue
  → BaseObject::GetSize()   ← CRASH here, addr=0x8 or 0x16
```

**根因推测**: 写屏障在 trace 阶段遍历对象引用时，引用的目标对象已被 LOS 迁移（MAIN→LOS migration）或 LOS compaction 移走，但引用未及时更新，导致访问到无效地址。addr 0x8 / 0x16 是典型的 null 附近偏移，说明对象指针被清零或指向了已回收的内存。

### 5.3 与 LOS_Frag_Simulation_Report 中 Config B 的关联

在 `LOS_Frag_Simulation_Report.md` 的 Config B (3:7:0) 测试中也观察到了相同模式的 SIGSEGV：
- 同样在 `BaseObject::GetSize()` crash
- LOS-mac2 分支特有，Origin 无此问题
- 说明这是 LOS-mac2 分支引入的并发 GC bug，与 MAIN→LOS 迁移或 CompactLOS 的引用更新有关

## 6. 结论

| 维度 | 评价 |
|------|------|
| **性能** | ✅ 有效提升 +38.5% (round 260 → 360) |
| **稳定性** | ❌ 37.5% crash 率，存在并发 GC bug |
| **可用性** | ❌ 不可上线，需要先修复 crash |

### 待修复问题

1. **P0: 写屏障引用更新竞态** — LOS 迁移/压缩过程中，mutator 线程的写屏障可能读到已经失效的对象地址。需要确保 forwarding pointer 在 memmove 前安装完毕，或在写屏障中加入 forwarding 检查。

2. **P1: SIGABRT 根因** — Run 3 的 SIGABRT 缺少完整堆栈，可能是 assertion failure，需要在 Debug 模式下复现以获取更多信息。
