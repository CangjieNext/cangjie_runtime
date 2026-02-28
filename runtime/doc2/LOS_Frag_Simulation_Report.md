# LOS 碎片化模拟测试报告

> 测试日期: 2026-02-26 | 平台: macOS aarch64 (Apple Silicon) | 堆大小: 128MB
> Runtime: 仓颉 SDK 20260220, 分支 LOS-mac2

## 1. 测试概述

使用 `test_los_frag_simulation.cj` 模拟服务端持续工作负载，评估 LOS 对内存碎片化的控制效果。

**工作负载模型** — 每轮迭代执行以下分配：

| 频率 | 类型 | 大小 | 生命周期 | 模拟场景 |
|------|------|------|----------|----------|
| 每轮 | 中等缓冲 ×2 | 48-96 KB | 8-30 轮 | 请求响应缓冲 |
| 每轮 | 小对象树 ×200 | ~127 节点/树 | 当轮即死 | GC 压力 + 小对象翻搅 |
| 每 3 轮 | 大缓冲 ×1 | 128-256 KB | 100 轮 | 缓存的中长期数据 |
| 每 5 轮 | **永久超大缓冲 ×1** | **512 KB** | **永不释放** | 常驻数据（内存增长源） |
| 每 3 轮 | 小缓冲 ×2 | 16 KB | 8 轮 | 低于 LOS 阈值的小对象 |

程序持续运行直到 OOM。**最后一次 SNAPSHOT 即为 OOM 时刻的测量值。**

核心度量：
- **OOM 轮次**: 越大 = 内存效率越高，程序可运行越久
- **Utilization%**: `存活数据 / 堆大小`，越高 = 碎片浪费越少
- **存活数据量**: 同样堆大小下能容纳的有效数据总量

## 2. 测试配置

| 配置 | MAIN:RESERVE:LOS | MAIN 空间 | RESERVE 空间 | LOS 空间 | 可分配空间 |
|------|-------------------|-----------|--------------|----------|-----------|
| **A** LOS ON | 3:5:2 | ~38 MB | ~67 MB | ~26 MB | ~64 MB (MAIN+LOS) |
| **B** RESERVE only | 3:7:0 | ~38 MB | ~90 MB | 0 | ~38 MB (MAIN only) |
| **C** FLAT baseline | 10:0:0 | ~128 MB | 0 | 0 | ~128 MB (全部) |

所有配置均使用 `cjHeapSize=128mb`。Config A 额外启用 `cjEnableLOSCompact=1`。

## 3. 结果汇总

### 3.1 核心指标对比

| 指标 | Config A (LOS ON) | Config B (RESERVE) | Config C (FLAT) |
|------|-------------------|--------------------|-----------------|
| **OOM 轮次** | ~320 | ❌ SIGSEGV 崩溃 | **~820** |
| **最后完整快照** | round 300 | N/A | round 800 |
| **OOM 时存活数据** | 39 MB | N/A | **89 MB** |
| **存活对象数** | 139 | N/A | **234** |
| **Utilization (vs 堆)** | 30% | N/A | **69%** |
| **Utilization (vs 可分配)** | ~61% | N/A | ~69% |
| **运行总时间** | ~1.6s | 即崩 | **~1.6s** |
| **GC 总次数** | 67 | N/A | 61 |
| **OOM GC 次数** | 66 | N/A | 60 |
| **退出方式** | 输出截断(OOM kill) | SIGSEGV(信号11) | OOM 异常(正常) |

### 3.2 关键结论

> **⚠️ Config A (LOS ON) 在当前配比下表现不如 Config C (FLAT)，存活时间仅为 FLAT 的 39%。**
>
> **根本原因不是 LOS 机制本身有问题，而是 RESERVE 占比过高（50%），大量内存闲置。**

## 4. 详细数据

### 4.1 Config A — LOS ON (3:5:2)

**存活数据增长趋势**（每 100 轮快照）：

| Round | 存活对象 | 存活数据 | 永久对象 | 临时对象 | Utilization | 累计分配 | 累计回收 | 耗时 |
|-------|----------|----------|----------|----------|-------------|----------|----------|------|
| 0 | 6 | 0 MB | 1 (0MB) | 5 (0MB) | 0% | 6 | 0 | 3ms |
| 100 | 100 | 19 MB | 21 (10MB) | 79 (9MB) | 15% | 325 | 225 | 202ms |
| 200 | 114 | 29 MB | 41 (20MB) | 73 (8MB) | 22% | 644 | 530 | 814ms |
| 300 | 139 | 39 MB | 61 (30MB) | 78 (9MB) | 30% | 966 | 827 | 1619ms |
| ~320 | — | — | — | — | — | — | — | OOM |

**OOM 时刻分析**：
- 永久对象 61 个 × 512KB ≈ 30MB，已接近 MAIN 空间上限（~38MB）
- 加上 78 个临时对象 ≈ 9MB，总计 ≈ 39MB
- LOS 已满（free=0），alloc=712, fail=369, fallback=302
- RESERVE 67MB 中只借出了约 7MB → **60MB 空闲浪费**

**GC 运行时统计**：
- GC 总计: 67 次 (1 heuristic + 66 OOM)
- CompactLOS 执行: 184 次
- ShrinkInactiveZone 执行: 74 次
- RESERVE 借用: 268 次尝试
- LOS AllocLarge: 316 次

### 4.2 Config B — RESERVE only (3:7:0)

**结果**: 启动后立即崩溃（SIGSEGV 信号 11）

```
Thread "gc-main-thread" catched unhandled SIGSEGV (Segmentation fault: 11)
  #0  0x10130adfc in BaseObject::GetSize()
      from libcangjie-runtime.dylib
```

**分析**：
- MAIN 仅约 38MB，RESERVE 约 90MB，无 LOS
- 大对象（48-512KB）全部在 MAIN 中分配
- 在 GC 的 `GetSize()` 过程中访问了非法地址(0x2b26)
- 这是一个**运行时 bug** ：当 RESERVE 比例过高、MAIN 过小时，
  GC 在处理半空间拷贝或对象遍历时可能访问到已越界或未映射的内存
- 退出码 139 (128+11=SIGSEGV)

### 4.3 Config C — FLAT baseline (10:0:0)

**存活数据增长趋势**：

| Round | 存活对象 | 存活数据 | 永久对象 | 临时对象 | Utilization | 累计分配 | 累计回收 | 耗时 |
|-------|----------|----------|----------|----------|-------------|----------|----------|------|
| 0 | 6 | 0 MB | 1 (0MB) | 5 (0MB) | 0% | 6 | 0 | 3ms |
| 100 | 100 | 19 MB | 21 (10MB) | 79 (9MB) | 15% | 325 | 225 | 177ms |
| 200 | 114 | 29 MB | 41 (20MB) | 73 (8MB) | 22% | 644 | 530 | 365ms |
| 300 | 139 | 39 MB | 61 (30MB) | 78 (9MB) | 30% | 966 | 827 | 552ms |
| 400 | 160 | 49 MB | 81 (40MB) | 79 (9MB) | 38% | 1285 | 1125 | 754ms |
| 500 | 174 | 59 MB | 101 (50MB) | 73 (8MB) | 46% | 1607 | 1433 | 948ms |
| 600 | 199 | 69 MB | 121 (60MB) | 78 (9MB) | 54% | 1926 | 1727 | 1138ms |
| 700 | 220 | 79 MB | 141 (70MB) | 79 (9MB) | 62% | 2245 | 2025 | 1340ms |
| 800 | 234 | 89 MB | 161 (80MB) | 73 (8MB) | 69% | 2564 | 2330 | 1549ms |
| ~820 | — | — | — | — | — | — | — | OOM |

**OOM 时刻分析**：
- 永久对象 161 个 × 512KB ≈ 80MB，几乎装满了 128MB 可用空间
- 临时对象 73 个 ≈ 8MB，总计 ≈ 89MB
- 实际碎片率 = 1 - 89/128 = **31%**（考虑 GC 的 semi-space 和元数据开销，这是合理的）
- GC 61次，OOM GC 60 次

**注意**: Config C 在 round 300 与 Config A 的存活数据量完全一致（139 objs, 39MB），
证明两者在相同轮次产生的有效数据完全相同，差异**纯粹来自可用空间大小**。

## 5. GC 运行时行为分析

### 5.1 Config A 的 GC 瓶颈

```
LOS 最终状态:
  capacity: 26MB, used: 28MB, free: 0
  alloc: 712, fail: 369, fallback: 302

RESERVE 最终状态:
  capacity: 67MB, available: 60MB
  mainBorrowed: 6MB, losBorrowed: 1.2MB
  mainReturned: 6MB, losReturned: 12MB

MAIN:
  区间 [0x10b238000, 0x113238000) = 134MB 虚拟空间
  end = 0x10d8a0000, inactiveZone = 0x10d818000
  实际可用 ≈ 38MB
```

**关键观察**：
1. LOS 区域(27MB)很快被永久512KB对象填满（每5轮一个 = 100轮后50MB需求，27MB容量远不够）
2. LOS 满后有 302 次 fallback 到 MAIN，MAIN 也随之被填满
3. RESERVE 有 67MB 容量但最终只借出了 ~7MB — **仅利用了 10%**
4. 大量 OOM GC (66次)但无法回收永久对象，只是反复触发 CompactLOS 和 ShrinkInactiveZone
5. RESERVE 借用-归还机制工作正常：借出后在 ShrinkInactiveZone Phase B 中归还

### 5.2 Config C 为何表现更好

- 全部 128MB 作为 MAIN，没有空间浪费
- 大对象直接在 MAIN 中分配，与小对象混排
- 虽然理论上会产生碎片，但 Copy GC 的 semi-space 特性天然消除碎片
  （复制式 GC 每次将存活对象紧凑复制到新半空间）
- 运行 820 轮后 89MB 存活数据，31% 碎片开销主要来自 semi-space 保留和 GC 元数据

### 5.3 Config A vs C 性能对比

| 指标 | Config A | Config C | 差距 |
|------|----------|----------|------|
| OOM 轮次 | ~320 | ~820 | **C 是 A 的 2.6 倍** |
| OOM 时存活数据 | 39 MB | 89 MB | **C 多存 50 MB** |
| 每秒处理轮次 | ~188 轮/s | ~530 轮/s | **C 快 2.8 倍** |
| GC 频率 | 67次/320轮 | 61次/820轮 | A 的 GC 密度是 C 的 2.8 倍 |
| 有效空间利用 | 39/64=61% | 89/128=69% | C 在可分配空间中利用率更高 |

## 6. 关键发现

### 发现 1: RESERVE 占比过高导致空间浪费（严重）

当前默认配比 `3:5:2` 中 RESERVE 占 50%（67MB），但实际运行中只借出了约 7MB。
**60MB RESERVE 空间完全闲置**，直接导致可分配空间缩减 50%，程序提前 OOM。

### 发现 2: LOS 机制本身工作正常

- CompactLOS 执行了 184 次，正确进行低地址滑动压缩
- ShrinkInactiveZone 执行了 74 次，正确归还了借用空间
- RESERVE 借用/归还流程工作正常
- 所有存活对象完整性校验 100% 通过

### 发现 3: Copy GC 天然抗碎片

Config C (FLAT) 没有 LOS 保护，但其碎片率仅 31%。
这是因为仓颉 Copy GC 每次将存活对象紧凑复制到新半空间，天然消除碎片。
在该类 GC 算法下，LOS 隔离大对象的"防碎片"优势被大幅削弱。

### 发现 4: Config B 存在运行时 Bug（新发现）

`cjMainRatio=3 cjReserveRatio=7 cjLOSRatio=0` 配置触发 SIGSEGV。
崩溃发生在 `BaseObject::GetSize()`，地址 `0x2b26` 明显非法。
这表明在 MAIN 空间极小（~38MB）且无 LOS 的情况下，GC 的对象遍历存在越界访问 bug。

### 发现 5: 两种配置在相同存活数据量时行为一致

在 round 0-300，Config A 和 Config C 的存活对象数和存活数据量**完全一致**
（如 round 300: 两者都是 139 objs, 39MB）。
差异纯粹来自可分配空间大小：A 只有 64MB 可用，C 有 128MB 可用。

## 7. 根因分析

### 为什么 LOS ON 反而更差？

```
 128 MB 堆
┌──────────┬──────────────────────────────────────┬──────────┐
│  MAIN    │           RESERVE                     │   LOS    │
│  38 MB   │           67 MB                       │  26 MB   │
│ (30%)    │           (50%)                       │  (20%)   │
│          │                                       │          │
│ ■■■■■■■  │ □□□□□□□□□□□□□□□□□□□□□□□□□□□□□□□□□□□□ │ ■■■■■■■  │
│ 已满     │       60 MB 闲置                       │  已满    │
└──────────┴──────────────────────────────────────┴──────────┘
 实际可分配: 38 + 26 = 64 MB (不到堆的一半)
```

问题链：
1. **RESERVE 50% 占比过高** → 仅 50% 堆空间可直接分配
2. **永久大对象增长不可避免** → 每 5 轮 +512KB，100 轮后 +10MB
3. **LOS 27MB 很快被填满** → fallback 到 MAIN，MAIN 也填满
4. **RESERVE 借用量杯水车薪** → 只借了 7MB，剩余 60MB 无法利用
5. **结果**: 300 轮就 OOM，而 FLAT 可以跑到 820 轮

### Copy GC 场景下 LOS 的价值重新评估

在非 Copy GC（如 Mark-Sweep、Mark-Compact）中：
- 大对象在 MAIN 中会形成难以回收的碎片 → LOS 隔离有明确价值
- 碎片累积导致有效空间下降 → LOS 能显著延长程序寿命

在 Copy GC（如仓颉的 WCollector）中：
- 每次 GC 都复制存活对象到新半空间 → **天然消除碎片**
- LOS 的"防碎片"优势被 Copy GC 的天然特性抵消
- LOS 的唯一价值变成：**避免大对象被频繁复制**（节省拷贝开销）
- 但这需要更精细的空间划分，不能让 RESERVE 浪费太多空间

## 8. 优化建议

### P0: 调整 RESERVE 比例（立即可做）

当前 `3:5:2` → 建议调整为 `6:2:2` 或 `7:1:2`：

| 方案 | MAIN | RESERVE | LOS | 可分配 | 预期效果 |
|------|------|---------|-----|--------|----------|
| 当前 3:5:2 | 38 MB | 67 MB | 26 MB | 64 MB | 本次测试 |
| **方案一 6:2:2** | **77 MB** | **26 MB** | 26 MB | **103 MB** | MAIN+61%,应跑600+轮 |
| **方案二 7:1:2** | **90 MB** | **13 MB** | 26 MB | **116 MB** | MAIN+136%,接近FLAT |
| 方案三 8:0:2 | 102 MB | 0 | 26 MB | 128 MB | 无RESERVE,纯LOS隔离 |

建议：**先实验方案一 `6:2:2`**，平衡 LOS 隔离 + RESERVE 弹性 + 分配空间。

### P1: 动态 RESERVE 发放机制

当前 RESERVE 借用是被动的（OOM 时才借），且每次借用量固定。
建议改为：
- **主动预借**: 当 MAIN/LOS 剩余 < 25% 时主动借用，避免频繁 OOM GC
- **自适应借用量**: 根据分配速率动态计算借用量，而非固定 chunk
- **延迟归还**: RESERVE 借出后不急于归还，减少反复借还开销

### P2: 评估 Copy GC 场景下 LOS 的 ROI

LOS 的核心价值在 Copy GC 下从"防碎片"变为"避免大对象拷贝"。
建议量化：
- 对比 LOS ON vs OFF 时的 GC 暂停时间（Copy 大对象的耗时）
- 如果大对象拷贝耗时可接受，可以考虑简化 LOS 或仅保留 pin 语义
- 在碎片率本就很低的场景下，LOS 的空间划分成本 > 收益

### P3: 修复 Config B 的 SIGSEGV Bug

`3:7:0` 配置下在 `BaseObject::GetSize()` 崩溃，需要排查：
- MAIN 极小时 Copy GC 的 semi-space 是否能正确工作
- 对象遍历是否存在边界条件导致读取非法地址

---

## 附录

### 原始输出文件
- Config A: `los-test/frag_sim_A_output.txt`
- Config B: `los-test/frag_sim_B_output.txt`
- Config C: `los-test/frag_sim_C_output.txt`

### GC 日志文件
- Config A: `los-test/frag_sim_A.log` (4855 行, 424KB)
- Config C: `los-test/frag_sim_C.log` (3894 行, 323KB)

### 测试源码
- `los-test/test_los_frag_simulation.cj`
- `los-test/run_frag_sim.sh`

### 复现命令

```bash
cd los-test

# 编译
source /path/to/cangjie-sdk/envsetup.sh
cjc test_los_frag_simulation.cj -o test_los_frag_simulation

# Config A
cjHeapSize=128mb cjMainRatio=3 cjReserveRatio=5 cjLOSRatio=2 \
cjEnableLOSCompact=1 ./test_los_frag_simulation

# Config B
cjHeapSize=128mb cjMainRatio=3 cjReserveRatio=7 cjLOSRatio=0 \
./test_los_frag_simulation

# Config C
cjHeapSize=128mb cjMainRatio=10 cjReserveRatio=0 cjLOSRatio=0 \
./test_los_frag_simulation
```
