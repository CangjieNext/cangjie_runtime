# Step 11：RSS 锯齿平滑 — 同步 Reclaim 与 Dead-Band 归还设计

> 目标：减小 MS-GC 在稳态运行期间 RSS 的锯齿形波动（批量释放→批量重申请→批量释放循环）。
> 2026-04-05 按当前源码校准：现在的实现不是“每次分配严格 1:1 同步释放”，而是“BUMP 扩容时在 empty cache 高于 lowWatermark 的前提下尽力同步释放 1 个 region，后台 scavenge 继续作为水位/true-live slack 兜底”。

## 目录

1. [问题背景](#1-问题背景)
2. [现状分析](#2-现状分析)
3. [根因分析](#3-根因分析)
4. [设计目标](#4-设计目标)
5. [方案 A：分配时同步 Reclaim（主方案）](#5-方案-a分配时同步-reclaim主方案)
6. [方案 B：Dead-Band 后台 Scavenge（备选方案）](#6-方案-bdead-band-后台-scavenge备选方案)
7. [代码变更清单](#7-代码变更清单)
8. [环境变量](#8-环境变量)
9. [端到端时序](#9-端到端时序)
10. [风险与验证](#10-风险与验证)
11. [与 Step 9 / Step 10 的关系](#11-与-step-9--step-10-的关系)

## 1. 问题背景

Step 9 设计并实施了三层控制环（GC Pacer → 后台 Sweep → 后台 Scavenge），使 MS-GC 在
宏观上具备了完整的内存回收链路。然而在实际负载下（10x benchmark），RSS 仍呈明显的锯齿形
波动：

```
RSS (MB)
 ↑
400┤    ╱╲       ╱╲       ╱╲
350┤   ╱  ╲     ╱  ╲     ╱  ╲
300┤  ╱    ╲   ╱    ╲   ╱    ╲
250┤ ╱      ╲ ╱      ╲ ╱      ╲
200┤╱        ╲        ╲        ╲
   └───────────────────────────→ t
```

**波动根因**：当前的后台 Scavenge 是"异步批量释放"模型。committed bytes 略超 target 时
就被通知唤醒，然后一次性释放大量 empty region。下一轮分配又重新 commit 新 region，
造成"释放→重申请→释放"的振荡循环。

**本 Step 的核心思路**：把释放行为从"后台异步批量"变为"分配时同步对冲"——每次 `TakeRegion`
commit 一个新 region 时，同步从 emptySweptCache 释放等量的 region。committed 总量
自然维持稳定，RSS 锯齿从源头消失。后台 Scavenge 降级为仅处理长期空闲的兜底机制。

## 2. 现状分析

### 2.1 分配路径

`GetRegion()` 的五优先级获取链：

```
Priority 1: partialSwept       — 已 sweep，最快路径
Priority 2: partialUnswept     — lazy sweep partial region
Priority 3: fullUnswept        — lazy sweep full region
Priority 4: emptySweptCache    — 空 region 重绑定 sizeClass
Priority 5: GetFreshRegion()   — TakeRegion → 从 releasedTree 或 bump commit 新 region
```

**关键点**：只有 Priority 5 会净增 committed bytes（物理内存），Priority 1-4 复用已有 region。

### 2.2 归还路径（当前实现）

```
MaybeNotifyReclaim()                  ← TakeRegion / PushEmptySwept 触发
  → IsGcStarted() ? return : 继续
  → 条件 A: emptyCount >= highWatermark
  → 条件 B: committedBytes > targetBytes && emptyCount > 0
  → NotifyToReclaimGarbage()          ← 唤醒后台线程（异步）

BackgroundProcessor (后台线程)
  → HandleBackgroundScavenge()
    → while (emptyCount > lowWater || trueLiveSlackHigh) {
          TakeEmptySweptRegion()
          ReclaimEmptyRegionToReleased()  ← madvise / 归还 OS
      }
    → ShrinkHeap()
```

### 2.3 TakeRegion 路径

```cpp
MSRegionInfo* TakeRegion(uint8_t cls, const CentralRegionPool& centralPool)
{
    TakeRegions(1, regionIdx, &source);   // releasedTree → bump → GrowSegment
    MaybeNotifySweep(&centralPool, cls);  // 确保 sweep 跟上
    MaybeNotifyReclaim();                 // 异步通知后台归还（当前实现）
    MaybeNotifyMark(...);                 // HEU GC 触发检查
    return &array_[regionIdx];
}
```

### 2.4 GetTrueLiveSlackTargetBytes

```cpp
size_t GetTrueLiveSlackTargetBytes() const {
    return max(gcThreshold, lastTrueLiveBytes);   // target = 真实存活上界
}
```

### 2.5 当前 Scavenge 的判断条件

```cpp
bool ShouldScavengeForTrueLiveSlack(committed, target) {
    return target != 0 && committed > target;     // ← 0 余量，committed 超 1 byte 就触发
}
```

## 3. 根因分析

RSS 锯齿的根本原因是 **分配与归还的时间解耦**：

### 3.1 当前时序（导致锯齿）

```
时刻 T1: 分配高峰，GetFreshRegion 连续 commit 新 region
         → committed 快速上升（RSS 上升斜坡）
         → MaybeNotifyReclaim 持续唤醒后台线程

时刻 T2: 后台 Scavenge 被唤醒，开始批量释放
         → 一次性释放 maxRegionsPerSlice 个 region
         → committed 急剧下降（RSS 下降悬崖）

时刻 T3: 分配继续，刚释放的内存被重新 commit
         → committed 再次上升（RSS 上升斜坡）

重复 T2-T3 → 锯齿
```

### 3.2 问题的三个维度

| 维度 | 现状 | 问题 |
|------|------|------|
| **触发灵敏度** | `committed > target` 即触发，0 余量 | 微小超调就触发不必要的 scavenge |
| **释放粒度** | 后台一次释放 `maxRegionsPerSlice`（可达 256） | 批量释放导致 RSS 阶跃式下降 |
| **释放与分配的耦合** | 分配端只通知，归还完全异步 | 时间差导致 committed 先涨后跌再涨 |

### 3.3 理想的稳态行为

```
理想: committed ≈ trueLive + 工作集缓冲
     每 commit 1 个 region → 同步释放 1 个 empty region
     净增 committed = 0
     RSS 变为近似水平线
```

## 4. 设计目标

| 编号 | 目标 | 优先级 |
|------|------|--------|
| G1 | RSS 在稳态负载下接近水平线，锯齿振幅 ≤ 20% | P0 |
| G2 | 分配路径延迟增加 < 5%（同步 reclaim 开销可控） | P0 |
| G3 | 不引入额外锁竞争，不阻塞 GC 关键路径 | P0 |
| G4 | 方案可通过环境变量开关和参数调节 | P1 |
| G5 | 后台 Scavenge 仍保留为兜底机制 | P1 |

## 5. 方案 A：分配时同步 Reclaim（当前实现口径）

### 5.1 核心思想

当前实现已经落地了一版 **best-effort 同步 reclaim**：

> 当 `TakeRegion()` 的来源是 `BUMP`，且 `emptySweptCache` 数量高于 `lowWatermark` 时，
> 同步从 empty cache 取 1 个 region 释放到底层 `releasedTree`，以抵消本次 committed 扩张的一部分压力。

这不是严格意义上的“每次分配都 1:1 对冲”，而是一个有前提、有上限的平滑机制。

对比当前实现与旧文档中的理想模型：

| | 旧设计叙事 | 当前实现 |
|---|---|---|
| 触发时机 | 每次新分配都同步对冲 | 仅 `source == BUMP` 时尝试 |
| 释放速率 | 严格 1:1 | 每次最多释放 1 个，且要求 `emptyCount > lowWatermark` |
| 后台 scavenge | 可弱化到接近不用 | 仍然保留并持续作为兜底 |
| 目标 | committed 近乎恒定 | committed 更平滑，锯齿振幅下降 |

### 5.2 当前真实流程

```text
TakeRegion(cls, centralPool)
│
├─ TakeRegions(1, regionIdx, &source)     // source ∈ {RELEASED, BUMP}
│
├─ if (source == BUMP && inline reclaim enabled)
│     InlineReclaimForBalance(centralPool)
│       ├─ emptyCount = pool.GetEmptySweptRegionCount()
│       ├─ lowWatermark = pool.GetEmptyCacheLowWatermarkRegions()
│       ├─ if (emptyCount <= lowWatermark) return
│       ├─ region = pool.TakeEmptySweptRegionForReclaim()
│       ├─ ReclaimEmptyRegionToReleased(region)
│       └─ pool.DecrementTotalMSRegionBytes(MS_REGION_SIZE)
│
├─ MaybeNotifySweep(&centralPool, cls)
├─ MaybeNotifyReclaim()   // 后台 scavenge 仍保留
└─ MaybeNotifyMark(...)
```

几个关键点：

1. 当前同步 reclaim 只在 `TakeRegion()` 的小对象补货路径上出现。
2. 它调用的是 `TakeEmptySweptRegionForReclaim()`，不会重新绑定 size class。
3. 释放后通过 `DecrementTotalMSRegionBytes()` 同步修正 committed 统计。

### 5.3 为什么只在 `BUMP` 来源时触发

这点和旧设计判断一致，当前代码也确实如此：

- **`RELEASED`**：从 `releasedTree` 取回 region，本质上是在复用已经被 scavenger 归还过的地址范围。
- **`BUMP`**：从 `inactiveZone` 扩大 committed heap，才是真正的净新增压力。

因此，只有 `BUMP` 才值得尝试同步回收一个空壳 region 来平滑 RSS。

### 5.4 与 `lowWatermark` 的关系

当前 `InlineReclaimForBalance()` 的第一道门槛就是：

```text
if (emptyCount <= lowWatermark) return;
```

也就是说，同步 reclaim **永远不会把 empty cache 掏到保护线以下**。这一点非常重要，因为当前 empty cache 同时承担：

1. 小对象 steady-state 的快速复用缓存。
2. 后台 scavenger 的可释放候选集合。

如果为了追求更平的 RSS 而把 empty cache 过度抽干，后续分配会更频繁地跌回 `BUMP`，反而放大抖动。

### 5.5 与后台 scavenge 的关系

当前实现不是“同步 reclaim 替代后台 scavenge”，而是两者并行：

1. **同步 reclaim** 负责在 `BUMP` 扩张当下做一次轻量对冲。
2. **后台 scavenge** 负责在以下条件下继续回收：
     - `emptyCount >= highWatermark`
     - `committed > targetBytes && emptyCount > 0`

因此，后台线程仍然是必要的：

- 当分配压力持续而 empty cache 长时间偏高时，它负责批量降水位。
- 当 true-live slack 已经超压时，它负责继续把多余空壳送回 released 树。

### 5.6 当前可预期效果

当前方案的预期效果应描述为：

> 让 RSS 锯齿从“每次后台批量释放后再重新爬升”的大幅波动，收敛成“由同步对冲削峰、由后台 scavenge 慢慢回拉”的较低振幅波动。

也就是说：

- **目标是更平滑**，不是“绝对水平线”。
- `trueLive` 上升时，RSS 仍会增长。
- empty cache 低于保护线时，同步 reclaim 会暂停，后台逻辑接管兜底。

## 6. 方案 B：Dead-Band 后台 Scavenge（备选方案）

> 方案 B 作为方案 A 的补充或独立备选。如果方案 A 已经消除了主要锯齿，方案 B 可以不实施。

### 6.1 核心思想

在后台 Scavenge 的触发条件和循环退出条件中增加 **dead-band（死区余量）**，避免 committed
刚超过 target 就触发释放。

### 6.2 现状 vs 改进

**现状**：
```cpp
// 触发条件 — 0 余量
bool ShouldScavengeForTrueLiveSlack(committed, target) {
    return target != 0 && committed > target;
}

// 循环退出 — 追到 target 才停
while (ShouldScavengeForTrueLiveSlack(GetTotalMSRegionBytes(), target)) {
    // 释放 region...
}
```

**改进**：
```cpp
size_t margin = GetTrueLiveSlackMarginBytes();  // 新增: 如 8MB

// 触发条件 — 超过 target + margin 才触发
bool ShouldScavengeForTrueLiveSlack(committed, target) {
    return target != 0 && committed > target + margin;
}

// 循环退出 — 释放到 target + margin/2 就停
while (committed > target + margin / 2) {
    // 释放 region...
}
```

### 6.3 Dead-Band 效果示意

```
committed
 ↑
 │  ← target + margin  ─── 触发释放线
 │
 │  ← target + margin/2 ── 停止释放线
 │
 │  ← target            ── 理论目标（不再追到这里）
 │
```

在 `[target, target + margin]` 范围内的超调不触发任何释放，消除微小波动带来的无意义
scavenge。当 committed 确实显著超过 target + margin 时（如程序负载突增后回落），
后台 Scavenge 启动但只释放到 `target + margin/2`，保留缓冲避免反复穿越触发线。

### 6.4 环境变量

| 变量名 | 默认值 | 含义 |
|--------|--------|------|
| `CJ_MS_TRUE_LIVE_SLACK_MARGIN` | `8mb` | dead-band 宽度，`margin` |

### 6.5 方案 B 的局限

- 只能降低锯齿的幅度和频率，不能从根本上消除
- 仍然是异步模型，committed 与 target 的差值在分配高峰期会持续累积
- 适合作为方案 A 的兜底，不适合单独使用

## 7. 代码变更清单

### 7.1 方案 A 变更（主方案）

#### 文件 1：`MSRegionManager.cpp` — TakeRegion 增加同步释放

```cpp
MSRegionInfo* MSRegionManager::TakeRegion(uint8_t cls, const CentralRegionPool& centralPool)
{
    RegionIndex regionIdx = 0;
    AllocSource source = AllocSource::BUMP;
    if (!TakeRegions(1, regionIdx, &source)) {
        return nullptr;
    }

    // 【新增】同步对冲：BUMP 来源净增 committed，从空缓存释放 1 个 region 对冲
    if (source == AllocSource::BUMP) {
        InlineReclaimForBalance(centralPool);
    }

    MaybeNotifySweep(&centralPool, cls);
    MaybeNotifyReclaim();                 // 后台兜底仍保留
    MaybeNotifyMark(HeuTriggerKind::SMALL_ALLOC, &centralPool, cls, source == AllocSource::BUMP);

    // ... 后续不变
}
```

#### 文件 2：`MSRegionManager.cpp` — 新增 InlineReclaimForBalance

```cpp
void MSRegionManager::InlineReclaimForBalance(const CentralRegionPool& pool)
{
    size_t emptyCount = pool.GetEmptySweptRegionCount();
    size_t lowWatermark = pool.GetEmptyCacheLowWatermarkRegions();
    if (emptyCount <= lowWatermark) {
        return;  // 空缓存已经很低，保护不掏空
    }

    // 非 const 操作需要 mutable 引用
    CentralRegionPool& mutablePool =
        const_cast<CentralRegionPool&>(pool);
    MSRegionInfo* region = mutablePool.TakeEmptySweptRegionForReclaim();
    if (region == nullptr) {
        return;
    }

    ReclaimEmptyRegionToReleased(region);
    mutablePool.DecrementTotalMSRegionBytes(MS_REGION_SIZE);
}
```

#### 文件 3：`MSCentralRegionPool.cpp` — 新增 TakeEmptySweptRegionForReclaim

```cpp
// 与 TakeEmptySweptRegion 不同：不做 InitForSizeClass，只从缓存中取出用于归还
MSRegionInfo* CentralRegionPool::TakeEmptySweptRegionForReclaim()
{
    std::lock_guard<std::mutex> lg(emptySweptLock_);
    MSRegionInfo* region = emptySweptCache_.TakeHead();
    if (region == nullptr) {
        return nullptr;
    }
    emptySweptRegionCount_.fetch_sub(1, std::memory_order_relaxed);
    emptySweptBytes_.fetch_sub(MS_REGION_SIZE, std::memory_order_relaxed);
    return region;
}

void CentralRegionPool::DecrementTotalMSRegionBytes(size_t bytes)
{
    totalMSRegionBytes_.fetch_sub(bytes, std::memory_order_relaxed);
}
```

#### 文件 4：`MSRegionManager.h` — 新增声明

```cpp
void InlineReclaimForBalance(const CentralRegionPool& pool);
```

#### 文件 5：`MSCentralRegionPool.h` — 新增声明

```cpp
MSRegionInfo* TakeEmptySweptRegionForReclaim();
void DecrementTotalMSRegionBytes(size_t bytes);
```

### 7.2 方案 B 变更（备选）

#### 文件 1：`MSCentralRegionPool.cpp` — 新增 margin 读取

```cpp
namespace {
size_t GetTrueLiveSlackMarginBytes()
{
    static const size_t value = ReadEnvCount("CJ_MS_TRUE_LIVE_SLACK_MARGIN", 8) * 1024 * 1024;
    return value;
}
}
```

#### 文件 2：`MSCentralRegionPool.cpp` — 修改 ShouldScavengeForTrueLiveSlack

```cpp
bool CentralRegionPool::ShouldScavengeForTrueLiveSlack(size_t committed, size_t target) const
{
    size_t margin = GetTrueLiveSlackMarginBytes();
    return target != 0 && committed > target + margin;
}
```

#### 文件 3：`MSCentralRegionPool.cpp` — 修改 HandleBackgroundScavenge 循环退出

```cpp
size_t stopTarget = state.targetBytes + GetTrueLiveSlackMarginBytes() / 2;

while ((GetEmptySweptRegionCount() > state.lowWatermark ||
        (!releaseAll && GetEmptySweptRegionCount() > 0 &&
         GetTotalMSRegionBytes() > stopTarget)) &&     // ← 改用 stopTarget
       releasedRegions < maxRegions &&
       (releaseAll || TimeUtil::MicroSeconds() < deadlineUs)) {
    // ... 释放逻辑不变
}
```

#### 文件 4：`MSRegionManager.cpp` — 修改 MaybeNotifyReclaim

```cpp
void MSRegionManager::MaybeNotifyReclaim()
{
    // ... 不变的前置检查 ...
    if (!shouldReclaim) {
        size_t committedBytes = pool->GetTotalMSRegionBytes();
        size_t targetBytes = pool->GetTrueLiveSlackTargetBytes();
        size_t margin = GetTrueLiveSlackMarginBytes();  // 新增
        shouldReclaim = targetBytes != 0
                     && committedBytes > targetBytes + margin  // 加 margin
                     && emptyCount > 0;
    }
    // ...
}
```

## 8. 环境变量

| 变量名 | 默认值 | 适用方案 | 含义 |
|--------|--------|----------|------|
| `CJ_MS_INLINE_RECLAIM` | `1` | A | 开关：1=启用同步 reclaim，0=禁用 |
| `CJ_MS_TRUE_LIVE_SLACK_MARGIN` | `8` | B | Dead-band 宽度（单位 MB） |
| `CJ_MS_EMPTY_CACHE_LOW_FLOOR_REGIONS` | `24` | A | lowWatermark 下限（保护线） |
| `CJ_MS_EMPTY_SCAVENGE_MAX_REGIONS_PER_SLICE` | `16` | B | 后台单次最大释放量 |

方案 A 复用已有的 `lowWatermark` 机制，无需新增额外参数。
方案 B 新增 `CJ_MS_TRUE_LIVE_SLACK_MARGIN`，可与方案 A 组合使用。

## 9. 端到端时序

### 9.1 方案 A 时序（同步对冲）

```
Mutator Thread              Background Thread
─────────────               ─────────────────
GetRegion(cls)
  P1-P4 未命中
  → GetFreshRegion(cls)
    → TakeRegion(cls)
      → TakeRegions(BUMP)
        committed += 256KB
      → InlineReclaimForBalance()
        emptyCache > lowWater ?
        YES → TakeEmptySweptRegion()
              ReclaimEmptyRegionToReleased()
              committed -= 256KB
              net = 0 ✓
        NO  → skip
                                Scavenge (idle)
      → MaybeNotifyReclaim()    → 由于 committed ≈ stable
                                   很少被触发
```

### 9.2 方案 A + B 组合时序

```
常态: 方案 A 保持 committed 稳定，方案 B 不触发

负载突降: trueLive 下降 → target 下降
         → committed > target + margin
         → 方案 B 后台触发，释放至 target + margin/2
         → 之后方案 A 继续维持稳态
```

## 10. 风险与验证

### 10.1 风险

| 风险 | 影响 | 缓解措施 |
|------|------|----------|
| 同步 Reclaim 增加分配路径延迟 | mutator 在 TakeRegion 中多做一次释放 | 单次 ~160ns，远小于 mmap 开销；可通过 `CJ_MS_INLINE_RECLAIM=0` 关闭 |
| emptySweptCache 与同步释放的锁竞争 | `emptySweptLock_` 被分配线程和后台线程同时访问 | 分配线程持锁时间极短（TakeHead ~50ns）；方案 A 生效后后台 Scavenge 很少触发 |
| lowWatermark 设置过高导致同步释放不生效 | emptyCount 始终 ≤ lowWater，同步 reclaim 从不触发 | 调低 `CJ_MS_EMPTY_CACHE_LOW_FLOOR_REGIONS`；或放宽为 `emptyCount > lowWater / 2` |
| BUMP 扩展 + 同步 Reclaim 频率不匹配 | 可能出现 BUMP 很多但 empty 很少的场景 | 此时说明真正需要更多内存（trueLive 在增长），不释放是正确行为 |

### 10.2 验证计划

1. **构建验证**：
   ```bash
   cd gc && bash run.sh ms-all
   ```

2. **10x Benchmark 对比**：
   ```bash
   # seed3 基线（方案 A 前）
   bash MSGC/benchmark/run_msgc_parameter_tuning.sh  # 已有存档

   # 方案 A 实施后
   bash MSGC/benchmark/run_msgc_parameter_tuning.sh
   python3 MSGC/benchmark/analyze_msgc_parameter_tuning.py
   ```

3. **关注指标**：
   - RSS 峰值变化（目标：不增加）
   - RSS 锯齿幅度（目标：显著降低）
   - Wall time（目标：< 5% 退化）
   - GC 频率和耗时（目标：无退化）

4. **开关对比**：
   ```bash
   CJ_MS_INLINE_RECLAIM=0  # 关闭方案 A，回退到纯后台
   CJ_MS_INLINE_RECLAIM=1  # 开启方案 A
   ```

## 11. 与 Step 9 / Step 10 的关系

### 与 Step 9 的关系

Step 9 定义了三层控制环：GC Pacer → 后台 Sweep → 后台 Scavenge。
Step 11 **不改变三层架构**，而是在第一层（分配）和第三层（归还）之间增加一条"同步短路径"：

```
Step 9 架构:
  分配 → GC Pacer → 后台 Sweep → 后台 Scavenge → 归还 OS

Step 11 增强:
  分配 ─[同步对冲]→ 归还 OS     ← 新增快速路径
    ↓
  GC Pacer → 后台 Sweep → 后台 Scavenge ← 降级为兜底
```

### 与 Step 10 的关系

Step 10 优化 Bitmap 的生命周期和内存开销。Step 11 的同步 Reclaim 会触发
`ReclaimEmptyRegionToReleased → Reset`，其中包含 bitmap 的释放。如果 Step 10 引入了
Bitmap Pool，则 Reset 路径需要适配 pool 的归还逻辑。两者互不阻塞，可并行实施。
