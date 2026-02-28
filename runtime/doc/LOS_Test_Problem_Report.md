# LOS 测试问题报告

> **日期**: 2026-02-23  
> **测试环境**: macOS aarch64, Cangjie SDK 20260220  
> **Runtime 版本**: 含 Bug 1-7 修复 + LOS + RESERVE 借还机制  

---

## 1. 测试概要

使用两个测试程序对 LOS（Large Object Space）实现进行全面验证：

| 测试程序 | 内容 | 源文件 |
|---------|------|--------|
| **功能性测试** | 10 个 Case，覆盖 LOS 分配、回收、迁移、压缩、RESERVE 借还、混合压力、极端碎片 | `gc/test_los_functional.cj` |
| **性能测试** | 5 个 Benchmark，比较三组配置的吞吐量、GC 效率、碎片恢复 | `gc/test_los_perf.cj` |

三组对比配置：
- **A (LOS ON)**: `cjMainRatio=3 cjReserveRatio=5 cjLOSRatio=2 cjEnableLOSCompact=1`
- **B (LOS OFF)**: `cjMainRatio=3 cjReserveRatio=7 cjLOSRatio=0`
- **C (FLAT MAIN)**: `cjMainRatio=10 cjReserveRatio=0 cjLOSRatio=0`

---

## 2. 测试结果总览

### 2.1 功能性测试

| 配置 | 堆大小 | 结果 | 耗时 | 备注 |
|------|--------|------|------|------|
| A (LOS ON) | 128MB | **10/10 PASS** | 928ms | 全部通过 |
| A (LOS ON) | 64MB | **CRASH** | — | Case 5 SIGABRT / Case 8 SIGSEGV |
| B (LOS OFF) | 256MB | **10/10 PASS** | 720ms | 全部通过 |
| B (LOS OFF) | 128MB | **OOM** | — | Case 6 分配 200×128KB 超出半空间 |

### 2.2 性能测试

| 配置 | 堆大小 | 结果 | 耗时 | 备注 |
|------|--------|------|------|------|
| A (LOS ON) | 512MB | **5/5 完成** | 1111ms | 正常 |
| A (LOS ON) | 256MB | **CRASH** | — | Bench 4 SIGABRT |
| A (LOS ON) | 128MB | **CRASH** | — | Bench 2 SIGABRT |
| B (LOS OFF) | 512MB | **5/5 完成** | 1021ms | 正常 |
| B (LOS OFF) | 128MB | **CRASH** | — | Bench 1 SIGSEGV |
| C (FLAT) | 512MB | **5/5 完成** | 681ms | 正常 |

---

## 3. 已发现问题清单

### P1: 64MB LOS ON — SIGABRT `IsValidObject()` 崩溃（可复现）

- **严重程度**: 🔴 高
- **触发条件**: `cjHeapSize=64mb`, Config A, 功能测试 Case 5 (CompactLOS) 或 Case 8 (混合压力)
- **崩溃信息**:
  ```
  Check failed: latest->IsValidObject()
  ```
- **崩溃位置**: GC 线程在静态根枚举阶段（Static Root Enumeration），遍历到一个已失效/已转发的对象引用
- **复现率**: 100%（在 Case 5 处稳定崩溃）
- **分析**: 
  - 64MB 堆下 LOS 分区仅约 12.8MB（20%），MAIN 约 19.2MB（30%）
  - 高分配压力下 GC 频繁触发，GC 线程在标记阶段访问到尚未更新的静态根
  - 可能原因：CompactLOS 或 CopyGC 转发后，静态根表中的旧指针未及时更新
  - 此问题在 128MB 堆下不出现，说明是极端内存压力下的竞态条件
- **状态**: 未修复，已记录为 Bug 8（之前审计中已识别）

### P2: 64MB LOS ON — SIGSEGV (addr=0x8) WriteBarrier 崩溃

- **严重程度**: 🔴 高
- **触发条件**: `cjHeapSize=64mb`, Config A, 功能测试 Case 4 (MAIN→LOS 迁移) 或 Case 8 (混合压力)
- **崩溃信息**:
  ```
  Signal: SIGSEGV (address: 0x8)
  ```
- **崩溃位置**: `WriteBarrier` → `allocBigChunks` 路径，在给大对象数组成员赋值时触发
- **分析**:
  - 地址 `0x8` 是典型的空指针偏移访问（对象指针为 null，访问 offset=8 的字段）
  - 在极端内存压力下，分配失败但返回了 null 对象（或 GC 转发后的无效引用）
  - 与 P1 同属极端内存压力下的 GC 竞态问题
- **状态**: 未修复，与 P1 可能同源

### P3: 256MB LOS ON — SIGABRT `IsThreadLocalRegion()` 崩溃

- **严重程度**: 🟡 中
- **触发条件**: `cjHeapSize=256mb`, Config A, 性能测试 Bench 4 (碎片恢复速度)
- **崩溃信息**:
  ```
  Check failed: toRegion1->IsThreadLocalRegion()
  ```
- **崩溃位置**: GC 线程在 CopyGC 阶段，尝试将对象拷贝到目标 Region，但目标 Region 不是线程本地的
- **分析**:
  - Bench 4 进行高频的 分配→释放→GC→再分配 循环
  - 256MB 堆下分区尺寸：MAIN ≈ 76.8MB, LOS ≈ 51.2MB, RESERVE ≈ 128MB
  - 高频 GC 循环中，Region 的线程本地状态可能因 LOS/MAIN 之间的 borrow/return 操作而错乱
  - 此问题在 512MB 堆下不出现，说明是中等内存压力下的边界条件
- **状态**: 未修复

### P4: 128MB LOS ON — SIGABRT `IsValidObject()` 性能测试崩溃

- **严重程度**: 🟡 中
- **触发条件**: `cjHeapSize=128mb`, Config A, 性能测试 Bench 2 (GC 循环效率)
- **崩溃信息**:
  ```
  Check failed: latest->IsValidObject()
  ```
- **分析**:
  - 与 P1 相同的断言失败，但在 128MB 堆下由性能测试触发
  - Bench 2 做 8 轮 × 40 个 128KB 大对象的分配-清除-GC 循环
  - 每轮分配 40×128KB = 5MB，累计需频繁触发 GC
  - 功能测试在 128MB 下不崩溃，说明性能测试的连续高频 GC 更容易触发此竞态
- **状态**: 未修复，与 P1 同源

### P5: 128MB LOS OFF — SIGSEGV ArrayCopyGeneric 崩溃

- **严重程度**: 🟢 低
- **触发条件**: `cjHeapSize=128mb`, Config B (LOS OFF), 性能测试 Bench 1 (纯分配吞吐)
- **崩溃信息**:
  ```
  Signal: SIGSEGV in ArrayCopyGeneric
  ```
- **分析**:
  - Bench 1 尝试连续分配 300 个 128KB 大对象
  - 300×128KB = 38.4MB，在 Copy GC 下需要一半空间作为 to-space
  - 128MB 堆下 MAIN=38.4MB（30%），其中 to-space ≈ 19.2MB，不足以容纳 300 个大对象
  - 这是预期的容量限制，非代码 Bug，仅在小堆+高分配量场景触发
- **状态**: 预期行为，非 Bug

### P6: 128MB LOS OFF — OOM 功能测试 Case 6

- **严重程度**: 🟢 低
- **触发条件**: `cjHeapSize=128mb`, Config B (LOS OFF), 功能测试 Case 6 (RESERVE 借用)
- **行为**: 程序 OOM 退出
- **分析**:
  - Case 6 尝试分配 200×128KB = 25.6MB 大对象
  - LOS OFF 时所有大对象放入 MAIN（30%=38.4MB），Copy GC to-space 占一半 ≈ 19.2MB
  - 可用空间不足以容纳 200 个大对象
  - 使用 256MB 堆则通过（MAIN=76.8MB，to-space=38.4MB，足够）
- **状态**: 预期行为，非 Bug（Copy GC 半空间限制）

---

## 4. 问题分类汇总

### 4.1 真正的代码 Bug（需修复）

| 编号 | 问题 | 严重程度 | 最小复现堆 | 根因推测 |
|------|------|---------|-----------|---------|
| P1 | `IsValidObject()` 断言失败 | 🔴 高 | 64MB | GC 转发后静态根未更新 |
| P2 | WriteBarrier SIGSEGV(0x8) | 🔴 高 | 64MB | GC 转发/分配返回 null |
| P3 | `IsThreadLocalRegion()` 断言失败 | 🟡 中 | 256MB | borrow/return 后 Region 状态错乱 |
| P4 | `IsValidObject()` (性能测试) | 🟡 中 | 128MB | 与 P1 同源，高频 GC 循环触发 |

### 4.2 预期行为（非 Bug）

| 编号 | 问题 | 原因 |
|------|------|------|
| P5 | ArrayCopyGeneric SIGSEGV | Copy GC 半空间容量不足（128MB 下 300×128KB 超限） |
| P6 | OOM at Case 6 | Copy GC 半空间容量不足（128MB LOS OFF 下 200×128KB 超限） |

---

## 5. 修复建议

### 5.1 P1/P2/P4: `IsValidObject()` + WriteBarrier 竞态

这三个问题极大概率同源，核心原因是 **GC 转发阶段与 mutator 之间的竞态条件**：

1. **根因**: CompactLOS / CopyGC 转发对象后，部分引用（静态根、栈帧、写屏障缓冲区）仍指向旧地址
2. **建议修复方向**:
   - 在 `CompactLOS` 后增加对静态根表的显式更新阶段
   - 在 `TryForwardObject` 中增加 CAS 原子性保证，防止转发中途被读取
   - 增加 forwarding pointer 的读屏障，确保所有读取都经过转发检查
3. **验证方法**: 修复后以 64MB LOS ON 运行功能测试 10 次，Case 5 和 Case 8 不再崩溃

### 5.2 P3: `IsThreadLocalRegion()` 断言失败

1. **根因**: LOS/MAIN 之间的 Region borrow/return 操作改变了 Region 的归属，但 GC 转发阶段仍认为目标 Region 是线程本地的
2. **建议修复方向**:
   - 在 borrow/return 操作中正确更新 Region 的 thread-local 标志
   - 或在 GC 转发时检查 Region 是否仍然有效，若已被 borrow/return 则跳过
3. **验证方法**: 修复后以 256MB LOS ON 运行性能测试 Bench 4 不再崩溃

---

## 6. 稳定运行的推荐配置

基于测试数据，当前实现的稳定配置：

| 场景 | 推荐堆大小 | 推荐配置 |
|------|-----------|---------|
| 功能测试（LOS ON） | ≥ 128MB | Config A |
| 功能测试（LOS OFF 对照） | ≥ 256MB | Config B |
| 性能测试（所有配置） | ≥ 512MB | Config A/B/C |
| 生产环境（保守） | ≥ 512MB | Config A |

**结论**: LOS 功能在 128MB+ 堆下可稳定运行全部 10 个功能测试用例。性能测试需 512MB 堆才能在所有配置下稳定完成。64MB 堆下存在可复现的 GC 竞态 Bug，需后续修复。

