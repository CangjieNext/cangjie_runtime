# Step34：MSGC Sweep Assist 配速化（proportional + credit）落地方案

> 2026-06-28
> 目标：把当前 sweep assist 从「阈值触发 + 突发还债」升级为 Go 风格「比例配速 + credit 抵扣」，消除 hardPressure 断崖与 front-load backlog 漏判。
> 关系：本文是 [Step31_MSGC_Go风格SweepAssist设计] 的**第二阶段落地实施**。Step31 给出方向与挂点；本文给出可直接落到 `MSCentralRegionPool` 上的具体代码改动、删除项、分阶段与验证口径。
> 口径：**以当前代码为准**。涉及文件 `src/Heap/MAllocator/MSCentralRegionPool.{h,cpp}`、`MSAllocBuffer.cpp`、`MSRegionManager.cpp`、`MSRegionSpace.cpp`。
> 补充：本文后半部分新增 **「proportional credit 已接入后的下一步增强：backlog-aware HEU gating」**。该部分解决的是：即便 assist sweep 已经改成 Go 风格还债模型，尾部仍可能因为 `heapThreshold` 快速收缩、旧 `unswept backlog` 未消化完，而在下一轮 `PreEnumRoots()` 被 `ForceFinishAllUnswept` 拉出长暂停。

---

## 目录

1. [背景：当前实现与 Go 的差距](#1-背景当前实现与-go-的差距)
2. [核心思路](#2-核心思路)
3. [ratio 的定义与推导](#3-ratio-的定义与推导)
4. [数据结构改动](#4-数据结构改动)
5. [周期开始：在-PrepareForSweep-定-ratio](#5-周期开始在-prepareforsweep-定-ratio)
6. [返债函数-AssistSweepCredit](#6-返债函数-assistsweepcredit)
7. [共享计数器：让-credit-抵扣免费生效](#7-共享计数器让-credit-抵扣免费生效)
8. [调用点改造](#8-调用点改造)
9. [可删除项](#9-可删除项)
10. [分阶段落地](#10-分阶段落地)
11. [正确性约束](#11-正确性约束)
12. [验证口径](#12-验证口径)
13. [一句话总结](#13-一句话总结)
14. [补充背景：为什么 proportional credit 之后还需要 backlog-aware HEU gating](#14-补充背景为什么-proportional-credit-之后还需要-backlog-aware-heu-gating)
15. [当前实现判断：主路径已经是 AssistSweepCredit，不应再围绕 hardPressure 设计](#15-当前实现判断主路径已经是-assistsweepcredit不应再围绕-hardpressure-设计)
16. [核心定义：runway、waterline、pressure mode](#16-核心定义runwaywaterlinepressure-mode)
17. [落地方案：在 MaybeNotifyMark 先还债，再开下一轮 HEU](#17-落地方案在-maybenotifymark-先还债再开下一轮-heu)
18. [代码挂点与建议实现顺序](#18-代码挂点与建议实现顺序)
19. [补充验证口径](#19-补充验证口径)

---

## 1. 背景：当前实现与 Go 的差距

当前 `ComputeAssistSweepRegions` + `AssistSweepForFreshRegions` 的配速模型：

```
remainingRunwayBytes = threshold - sweptRegions*REGION_SIZE
hardPressure         = (remainingRunwayBytes == 0)

if !hard && unsweptRegions <= ceil(remainingRunwayBytes/REGION_SIZE):
    return 0                                   // backlog 在 runway 之内就啥也不干
normal: assist = min(unswept / P / 10, maxPerCall)
hard  : assist = min(unswept / P,       maxPerCall*10)
```

对照 Go 的 proportional sweep（`mgcsweep.go` + `mgcpacer.go`）：

```
sweepPagesPerByte = pagesToSweep / heapDistance       // mark 结束时定一次
// 每次分配 deductSweepCredit(spanBytes)：
pagesTarget = sweepPagesPerByte * (heap_live - basis)
while (pagesSwept - basis) < pagesTarget: sweepone()
```

主要差距：

| # | 问题 | 说明 |
|---|------|------|
| 1 | **阈值触发 vs 连续比例** | `unswept <= runway` 期间 assist 完全为 0，超过时已贴边才启动；判定只看「总 backlog vs 总 runway」，丢失相位信息，front-load backlog 会被漏掉 |
| 2 | **还债量与分配量无关** | `unswept/P/10` 谁调都一样，与该线程实际分配速率无关，不能 self-correcting；`/10` 是 magic number |
| 3 | **hardPressure 断崖** | runway=0 时跳到 `unswept/P`（cap 也 ×10），恰好在内存逼迫时制造延迟尖刺；Go 正常路径无此模式 |
| 4 | **无 credit、每次全量扫描** | 每次 `GetTotalUnsweptCount()` 数 `kNumSizeClasses` 个 bucket（O(类数)）；无 background 进度抵扣的显式机制 |
| 5 | **runway 基准方向相反** | 当前 `threshold - sweptRegions*REGION_SIZE` 用**清扫进度**做基准（越扫越小）；Go 用**分配进度** `heapGoal - heap_live` |

本文逐条修掉。

---

## 2. 核心思路

把「每次重算 unswept、够不够 runway → 突发还」换成「**周期开始定一个 ratio，之后按已分配量线性追平**」。

关键技巧：用一个**全局共享的 `sweptRegionsThisCycle_` 计数器**当清扫进度基准——background / lazy / assist 三条路谁扫都加它。于是「mutator 还欠多少」会自动把 background 干的活减掉，**credit/抵扣免费得到，不写额外逻辑**（等价于 Go 的 `pagesSweptBasis`）。

四步返债：

```
推进分配进度 allocated → 算应扫到哪 target=ratio×allocated
→ 减去大家已扫的 sweptThisCycle（白嫖 background）→ 还差的部分，限额扫掉
```

没有 hardPressure 分支：ratio>1 时 target 自然跑得比 allocated 快，每次该还的就多，连续平滑。

---

## 3. ratio 的定义与推导

ratio 回答的问题：**每分配 1 个 region，应该顺手扫几个，才能正好在下次触发点扫完全部 unswept。**

```
ratio = 待扫的总 region 数 / 这期间总共会分配的 region 数
```

- **分子 `totalUnswept`**：`PrepareForSweep()` 那一刻（刚 `FlipSweptToUnswept` 把整堆翻成 unswept），各 bucket 的 `partialUnswept + fullUnswept` 之和。确定已知，周期内只读。
- **分母 runway**：到下次触发还能分配多少 region = `(threshold - liveNow) / REGION_SIZE`。
  - `threshold` = `GetHeapThresholdBytes()`（下次触发目标）。
  - `liveNow` = `GetHeapRetainedBytes()`（sweep 开始时占用 ≈ Go 的 heap_live）。
  - **改用分配进度基准，修正差距 #5。**

```
ratio = totalUnswept / max(1, (threshold - liveNow) / MS_REGION_SIZE)
```

三种情形：

| 情形 | ratio | 含义 |
|------|-------|------|
| 待扫少、runway 大 | `< 1` | 每分配几个才扫 1 个，薄薄还 |
| 待扫 ≈ runway | `≈ 1` | 一比一 |
| 待扫多、runway 小 | `> 1` | 每分配 1 个扫好几个，**替代 hardPressure** |

为什么乘 `allocated` 而不是每次扫 ratio 个：ratio 可能是小数（如 0.3），每次 round 会归零。用累计追平 `target = ratio × allocated` 不丢小数。

---

## 4. 数据结构改动

`CentralRegionPool` 新增成员：

```cpp
// —— 周期开始时定一次，周期内只读 ——
double               sweepRegionsPerAlloc_{0.0};      // ratio = 待扫region / runway-region

// —— 周期内累加 ——
std::atomic<size_t>  allocFreshRegionsThisCycle_{0};  // 本周期取走的 fresh region 数（分配进度）
std::atomic<size_t>  sweptRegionsThisCycle_{0};       // 本周期已扫 region 数（清扫进度=共享基准）

// —— O(1) backlog 计数，替代 GetTotalUnsweptCount() 热路径全量扫描 ——
std::atomic<size_t>  unsweptRegionTotal_{0};
```

`unsweptRegionTotal_` 维护点（可放第二阶段）：
- `FlipSweptToUnswept()` 里 `+= partial+full` 总数；
- `ClaimUnsweptHeadForSweep` 成功抢到一个 region 时 `--`。

---

## 5. 周期开始：在 PrepareForSweep 定 ratio

挂点：`CentralRegionPool::PrepareForSweep()`（`MSCentralRegionPool.cpp:571`，`globalSweepGen_ += 2` 处）。

```cpp
void CentralRegionPool::PrepareForSweep()
{
    size_t totalUnswept = 0;
    for (uint8_t cls = 0; cls < kNumSizeClasses; cls++) {
        SizeClassBucket::Pressure p = buckets_[cls].GetPressure();   // 已有
        totalUnswept += p.partialUnswept + p.fullUnswept;
        buckets_[cls].FlipSweptToUnswept();
    }
    unsweptRegionTotal_.store(totalUnswept, std::memory_order_relaxed);
    allocFreshRegionsThisCycle_.store(0, std::memory_order_relaxed);
    sweptRegionsThisCycle_.store(0, std::memory_order_relaxed);

    // runway：到下次触发还能分配多少 region（分配进度基准，非清扫进度）
    RegionSpaceMS& space = static_cast<RegionSpaceMS&>(Heap::GetHeap().GetAllocator());
    size_t threshold = space.GetHeapThresholdBytes();
    size_t liveNow   = space.GetHeapRetainedBytes();    // ≈ heap_live
    size_t runwayB   = threshold > liveNow ? threshold - liveNow : 0;
    size_t runwayR   = std::max<size_t>(1, runwayB / MS_REGION_SIZE);

    sweepRegionsPerAlloc_ = static_cast<double>(totalUnswept) / static_cast<double>(runwayR);

    globalSweepGen_.fetch_add(2, std::memory_order_release);
    sweepDone_.store(false, std::memory_order_release);
}
```

---

## 6. 返债函数 AssistSweepCredit

签名与现 `AssistSweepForFreshRegions` 一致，可直接顶替。**复用现有 `AssistSweep(targetRegions,...)` 当底层原语**（它已带 `activeAssistSweepCount_` guard + seq_cst fence + skip/stop 检查，STW 同步逻辑一行不改）：

```cpp
size_t CentralRegionPool::AssistSweepCredit(size_t requestedRegions, uint8_t preferredCls,
                                            AllocBufferMS* owner)
{
    double ratio = sweepRegionsPerAlloc_;                 // PrepareForSweep 定的，只读
    if (ratio <= 0.0 || requestedRegions == 0 ||
        sweepDone_.load(std::memory_order_acquire)) {
        return 0;                                         // 没活 / 已扫完
    }

    // 1) 推进分配进度：大对象一次吃 requestedRegions 份 runway
    size_t allocated = allocFreshRegionsThisCycle_.fetch_add(
                           requestedRegions, std::memory_order_relaxed) + requestedRegions;

    // 2) 进度表：到现在为止「应该」已扫到的总量
    size_t target = static_cast<size_t>(ratio * static_cast<double>(allocated));

    // 3) 共享计数器已含 background/lazy 功劳 → 自动抵扣
    size_t doneSoFar = sweptRegionsThisCycle_.load(std::memory_order_acquire);
    if (doneSoFar >= target) {
        return 0;                                         // background 已替我们还清
    }

    // 4) 限单次尾延迟；余债下次分配 target 仍领先，会继续追
    size_t owe = target - doneSoFar;
    size_t maxPerCall = CangjieRuntime::GetGCParam().msAssistSweepMaxRegionsPerCall;
    size_t toSweep = std::min(owe, maxPerCall);

    return AssistSweep(toSweep, preferredCls, owner);     // 复用：含 fence/guard/skip
}
```

---

## 7. 共享计数器：让 credit 抵扣免费生效

`AssistSweep` 内每扫一个调 `RecordAssistSweep(1)`，background 走 `RecordBackgroundSweep`，lazy 走 `RecordLazySweep`。三个 Record 各加一句，汇进同一周期计数器：

```cpp
void RecordAssistSweep(size_t n) {
    assistSweptRegions_.fetch_add(n, std::memory_order_relaxed);
    sweptRegionsThisCycle_.fetch_add(n, std::memory_order_relaxed);   // ← 新增
}
// RecordLazySweep / RecordBackgroundSweep 同样各加这一句
```

background sweeper 扫的量会直接抬高 `sweptRegionsThisCycle_`，mutator 端 `doneSoFar >= target` 成立即不用还——这就是 Go credit/reimburse 的等价物，零额外逻辑。

---

## 8. 调用点改造

仅两个外部入口（全量确认，无遗漏）：

**小对象** `AllocBufferMS::RefillFromCentral`（`MSAllocBuffer.cpp:596`）：
```cpp
(void)central_->AssistSweepCredit(1, cls, this);
```
时机：本地 buffer 耗尽、`TakePartialSweptRegion` 拿不到现成 region 之后，`GetCacheRegion`/`TakeUnsweptRegion` 之前。最高频路径。

**大对象** `MSRegionManager::TakeRegionForLarge`（`MSRegionManager.cpp:417`）：
```cpp
(void)pool->AssistSweepCredit(regionCount, kNoPreferredSweepClass, owner);
```
时机：`TakeRegions` 占用前。`requestedRegions = regionCount`（吃 N 份 runway，多还）。

二者正对 Go 的 `mcache.refill → deductSweepCredit`（小对象）与 large alloc 路径。

---

## 9. 可删除项

改造稳定后清理：

- `ComputeAssistSweepRegions`（`/P/10`、`maxPerCall*10`、hardPressure 全消失）。
- `AssistSweepForFreshRegions` 里 `remainingRunwayBytes` / `unsweptRegions <= remainingRunwayRegions` 判定（迁入 ratio）。
- `GetTotalUnsweptCount()` 在返债热路径的调用（保留作监控可以，热路径改用 `unsweptRegionTotal_`）。

---

## 10. 分阶段落地

**第一阶段（最小集，可回滚）**：
1. 加 `sweepRegionsPerAlloc_` + `allocFreshRegionsThisCycle_` + `sweptRegionsThisCycle_`；
2. `PrepareForSweep` 定 ratio；
3. 三个 Record 加共享计数器一行；
4. 新增 `AssistSweepCredit`，改两个调用点；
5. **暂不删** `ComputeAssistSweepRegions` / `AssistSweepForFreshRegions`（留对照/回滚）。

跑基准，确认 `ForceFinishAllUnswept` 接走的 region 数下降、assist 扫描条数分布变平后，再进入：

**第二阶段（清理 + O(1)）**：
6. 引入 `unsweptRegionTotal_` 原子计数，替换热路径全量扫描；
7. 删除第 9 节列出的废弃函数。

---

## 11. 正确性约束

1. **STW 同步不动**：`AssistSweepCredit` 复用 `AssistSweep`，后者的 `activeAssistSweepCount_` guard + seq_cst fence 与 `StopAndWaitForAllSweep` 配对逻辑保持原样。新增计数器都是 relaxed，不参与 STW 正确性。
2. **ratio 周期内只读**：在 `PrepareForSweep` 一次定死。**已确认 `threshold` 在一个 sweep 周期内稳定（只在 GC 边界改，HeapGrowth 不会在周期中途改）**，因此 ratio 定死成立，无需在 `AssistSweepCredit` 内读实时 runway 重算，省掉每次一次除法。后续若 threshold 改为周期内可变，需回退为实时重算。
3. **计数器溢出**：`allocFreshRegionsThisCycle_` / `sweptRegionsThisCycle_` 每周期 `PrepareForSweep` 清零，单周期内不会溢出 size_t。
4. **maxPerCall 仍限尾延迟**：欠债通过 target 持续领先在后续分配补齐，不会因单次截断而永久丢失。

---

## 12. 验证口径

1. **`ForceFinishAllUnswept` 接走的 region 数 → 趋近 0**（Go 把 `finishsweep_m` 当 no-op 的目标）。当前若该值很大，说明改造前 assist 确实 under-pace。
2. **单次分配 assist 扫描条数分布 → 变平**，消除「平时 0、临界一大坨」的双峰。
3. **三类 sweep 计数占比**（`backgroundSweptRegions_` / `lazySweptRegions_` / `assistSweptRegions_`）：assist 占比应随分配压力平滑变化，而非阶跃。
4. 吞吐与 P99 分配延迟基准对比改造前。

---

## 13. 一句话总结

把「阈值触发 + 突发还债」改成「**周期定 ratio + 按分配量 credit 追平 + 共享计数器自动抵扣 background**」，断崖（hardPressure）与 front-load 漏判同时消失，runway 基准对齐 Go 的分配进度——这是当前 MSGC 与 Go proportional sweep 的最后一段差距。

---

## 14. 补充背景：为什么 proportional credit 之后还需要 backlog-aware HEU gating

`Step34` 主体解决的是 **assist sweep 自身怎么按 Go 的 proportional credit 方式平滑还债**。这一步落地后，assist 路径已经不再是「`unswept/P/10` + `hardPressure` 跳变」的旧模型，而是：

1. `PrepareForSweep()` 在周期开始时固定 `sweepRegionsPerAlloc_`
2. `AllocBufferMS::RefillFromCentral()` 与 `MSRegionManager::TakeRegionForLarge()` 通过 `AssistSweepCredit()` 按分配进度还债
3. `sweptRegionsThisCycle_` 自动抵扣 background / lazy 已经扫掉的工作

但这并不自动解决另一个问题：

1. 尾部 live bytes 可能在 1~2 轮 GC 内快速塌陷
2. `heapThreshold` 会跟着明显变小
3. 下一轮 `MSRegionManager::MaybeNotifyMark()` 仍然只看 `sweptBytes >= thresholdBytes`
4. 于是系统可能在旧 `unswept backlog` 还很大时，又很快发起下一轮 `GC_REASON_HEU`
5. 下一轮 GC 开头 `PreEnumRoots()->StopAndWaitForAllSweep()->ForceFinishAllUnswept()` 就把旧 backlog 串行补扫出来，形成尾部大 pause

所以：

- `Step34` 解决的是“**怎么更平滑地还债**”
- 本补充方案解决的是“**当债还没还完时，是否应该先允许下一轮 HEU GC 继续往前冲**”

两者不是替代关系，而是前后衔接关系。

## 15. 当前实现判断：主路径已经是 AssistSweepCredit，不应再围绕 hardPressure 设计

这里必须先澄清当前代码状态，避免设计落到旧路径上。

当前真正生效的 assist sweep 主路径已经是：

1. 小对象 refill：`AllocBufferMS::RefillFromCentral()` 调 `central_->AssistSweepCredit(1, cls, this)`
2. 大对象 region 获取：`MSRegionManager::TakeRegionForLarge()` 调 `pool->AssistSweepCredit(regionCount, kNoPreferredSweepClass, owner)`
3. `AssistSweepCredit()` 按 `target = ratio * allocated` 计算当前应还债量，再用 `sweptRegionsThisCycle_` 抵扣已经完成的 sweep 工作

仍然留在代码里的：

1. `ComputeAssistSweepRegions(...)`
2. `AssistSweepForFreshRegions(...)`

更像是旧逻辑遗留或兼容辅助路径，**不是当前热路径的设计中心**。

因此，如果要做 “backlog-aware HEU gating + 再次提高 background/assist sweep 比率”，正确方向应当是：

1. **在 `MaybeNotifyMark()` 决定是否 defer `GC_REASON_HEU`**
2. **在当前有效的 `AssistSweepCredit()` 上提高还债斜率**
3. **同时提高 `AssistSweepCredit()` 的单次 cap**
4. **让 background sweep 也进入更高 pressure 档位**

而不是重新回到 `hardPressure` 布尔分支上做文章。

另外要注意区分：

- `cjMSAssistRatioScale` / `ratioScaled=...` 是 **mark assist** 的比率
- 本文讨论的是 **sweep assist** 的还债比率 `sweepRegionsPerAlloc_`

二者不是一回事。这里不应通过调 `mark assist ratio` 来解决 sweep backlog。

## 16. 核心定义：runway、waterline、pressure mode

### 16.1 runway

这里的 `runway` 不另起口径，继续复用当前 HEU trigger 已在使用的轴：

```text
thresholdBytes = gcStats.GetThreshold()
sweptBytes     = space.GetSweptHeapBytes()
runwayBytes    = max(thresholdBytes - sweptBytes, 0)
runwayRegions  = ceil(runwayBytes / MS_REGION_SIZE)
```

原因：

1. `MaybeNotifyMark()` 现在就是按 `sweptBytes >= thresholdBytes` 决定是否发 `GC_REASON_HEU`
2. `PrepareForSweep()` / `AssistSweepCredit()` / background sweep 本身也已经围绕这条轴在工作
3. 继续共用同一条轴，最不容易把新 gating 和现有 HEU 语义搞成两套互相打架的系统

### 16.2 waterline

为了避免在边界附近抖动，不用单一阈值，而用带滞回的 enter / exit 双水位线。

建议定义：

1. `NORMAL`
   - `sweepDone == true`
   - 或 `unsweptRegions` 没有明显高于 `runwayRegions`
2. `BACKLOG`
   - `runwayRegions > 0`
   - 且 `unsweptRegions > backlogEnterRatio * runwayRegions`
3. `EMERGENCY`
   - `runwayRegions == 0`
   - 且 `unsweptRegions > emergencyEnterRegions`

建议初始值：

```text
backlogEnterRatio   = 2.0
backlogExitRatio    = 1.0
emergencyEnterRegs  = 64
emergencyExitRegs   = 16
```

解释：

1. `BACKLOG` 进入比退出更高，避免 `NORMAL/BACKLOG` 反复抖动
2. `runway == 0` 时必须单独处理，因为这时“高于 runway”失去区分度
3. `64 -> 16` 的 tail 允许少量 backlog 自然收尾，不至于永远压住 HEU

### 16.3 pressure mode

建议把 pressure 做成三档，而不是单布尔：

1. `SWEEP_PRESSURE_NORMAL`
2. `SWEEP_PRESSURE_BACKLOG`
3. `SWEEP_PRESSURE_EMERGENCY`

建议倍率：

```text
background sweep budget: NORMAL=1x, BACKLOG=4x, EMERGENCY=10x
assist ratio factor    : NORMAL=1x, BACKLOG=4x, EMERGENCY=10x
assist cap factor      : NORMAL=1x, BACKLOG=4x, EMERGENCY=10x
```

这三档的语义不是：

- “有没有 hardPressure”

而是：

- “当前 sweep 债是否已经明显落后于允许继续分配的 runway”

## 17. 落地方案：在 MaybeNotifyMark 先还债，再开下一轮 HEU

核心目标不是“取消 GC 开头 `ForceFinishAllUnswept`”，而是“尽量让它在下一轮 GC 开头没多少旧债可补”。

建议语义：

1. `MaybeNotifyMark()` 先检查原有前提：
   - `!Heap::GetHeap().IsGcStarted()`
   - 通过 `gcInterval` cooldown
   - `pool != nullptr`
   - `sweptBytes >= thresholdBytes`
2. 在这些条件成立后，不立刻 `RequestGC(GC_REASON_HEU, true)`，而是先构造一份 `SweepDebtSnapshot`
3. 如果 `mode == NORMAL`
   - 保持现有逻辑，允许发 `GC_REASON_HEU`
4. 如果 `mode == BACKLOG || mode == EMERGENCY`
   - 且 `!sweepDone`
   - 则 **defer 这次 `GC_REASON_HEU`**
   - 改为 `backgroundProcessor.NotifyToSweep(kNoPreferredSweepClass)`
   - 同时让 background / assist 使用更高 pressure 档位继续还债

简化后的决策就是：

```text
HEU trigger 命中
-> 先看 sweep backlog
-> backlog 不大：正常开 GC
-> backlog 明显偏大：先还债，不开这轮 HEU
```

这里强调两点：

1. **只 gate `GC_REASON_HEU`**
   - `OOM / FORCE / HEU_SYNC` 不受影响
2. **不是永远压住 GC**
   - 一旦 `sweepDone == true`
   - 或 `unsweptRegions` 降到退出水位线以下
   - 下一次 `MaybeNotifyMark()` 会重新允许 HEU 正常进入

## 18. 代码挂点与建议实现顺序

建议把这一步作为 `Step34` 的后续增强，分成四个非常具体的挂点：

### 18.1 纯策略层

新增轻量 helper，例如：

- `MSSweepDebtPolicy.h/.cpp`

只负责纯计算：

1. `ComputeRunwayRegions(...)`
2. `EvaluateSweepPressure(...)`
3. `BuildSweepDebtDecision(...)`

这样做的好处是：

1. `waterline / pressure mode` 可以白盒单测
2. 不需要把整个 `MSCentralRegionPool.cpp` 硬拖进测试

### 18.2 runtime snapshot 层

在 `CentralRegionPool` 中组装运行时状态：

1. `unsweptRegions = GetTotalUnsweptCount()`
2. `runwayRegions = ComputeRunwayRegions(thresholdBytes, sweptBytes, MS_REGION_SIZE)`
3. `sweepDone = IsSweepDone()`
4. `decision = BuildSweepDebtDecision(...)`

并保存最近一次模式，提供：

1. `RefreshSweepDebtSnapshot()`
2. `PeekSweepDebtSnapshot()`
3. `GetSweepPressureMode()`

### 18.3 HEU gating 层

在 `MSRegionManager::MaybeNotifyMark()` 里接入：

1. `snapshot = pool->RefreshSweepDebtSnapshot()`
2. 如果 `snapshot.decision.deferHeu == true`
   - 不发 `RequestGC(GC_REASON_HEU, true)`
   - 改为 `NotifyToSweep(...)`
   - 打一条 `HEU deferred by sweep backlog` 日志

### 18.4 sweep 还债增强层

对当前有效主路径 `AssistSweepCredit()` 动手，而不是旧 `hardPressure` 路径：

```text
effectiveRatio     = sweepRegionsPerAlloc_ * snapshot.decision.assistRatioMultiplier
effectiveMaxPerCall = baseMaxPerCall       * snapshot.decision.assistCapMultiplier
```

background sweep 预算同样按 `snapshot.decision.backgroundBudgetMultiplier` 放大，而不是继续只看 `remainingRunwayBytes == 0`。

## 19. 补充验证口径

这一补充方案的验证重点，不再只是看 assist 是否更平滑，而是看 **尾部 HEU 是否被适当延后，old backlog 是否在 GC 开头前被更多消化**。

建议重点看：

1. `MaybeNotifyMark()` 日志
   - 是否出现 `HEU deferred by sweep backlog`
   - defer 时的 `unswept / runway / mode`
2. `ForceFinishAllUnswept`
   - `swept=%zu regions` 是否明显下降
3. `PreEnumRoots`
   - 尾部最后几轮是否不再出现几秒级 `stopSweep`
4. `sweep-counts`
   - `background / lazy / assist` 是否在尾部更积极接管还债

推荐端到端验证继续使用：

1. `case27_http_complex_json`
2. `c1000`
3. `p100K`
4. 当前使用中的 MSGC 参数组，例如最近常用的 `heapgrow=3`、`scavengegrow=3`

理想现象：

1. GC 次数可以不必显著下降
2. 但最后几轮 GC 的 `PreEnumRoots` 尖峰应该明显缩短
3. `ForceFinishAllUnswept` 仍可能存在，但接手的 region 数量不应再是几万到近十万的量级

## 20. HEU pending + in-flight 合并与执行前重检（已实现）

单独在 `MaybeNotifyMark()` 提交时检查 sweep debt 不足以阻止连续 GC：HEU 入队后，
`Pop()` 原先会立即清除 pending bit，但任务尚未执行完。这个窗口内的分配可以再提交同一
`GC_REASON_HEU`；另外，早先入队的 HEU 真正执行时，heap threshold 和上一轮 sweep debt
都可能已经变化。

当前实现：

1. 异步队列用一个 64-bit atomic 同时保存 pending/running priority bits。
2. `Pop()` 原子地把 pending bit 移到 running bit，同优先级任务在 pending 或 running 时都不会重复入队。
3. `TaskQueue::DrainTaskQueue()` 只在 `Execute()` 返回后清除 running bit；被跳过的陈旧 HEU 也正常 complete。
4. `GCExecutor` 在进入 `RunGarbageCollection()` 前调用 `ShouldExecuteQueuedRequest()`。MSGC 只对
   `GC_REASON_HEU` 重新读取 `sweptBytes / threshold / sweep debt`。
5. 提交侧和执行侧共用 `EvaluateMSHeuExecution()`：低于当前 threshold 则直接丢弃，
   debt 要求 defer 则丢弃并唤醒 background sweep，否则执行 GC。
6. `OOM / FORCE / HEU_SYNC / BACKUP` 及 CopyGC 保持原行为。HEU 是启发式请求，丢弃陈旧任务后，
   若 sweep 追上后仍超过 threshold，后续分配探测会再次提交。

这一层不替代 `MaybeNotifyMark()` 的 sweep backlog gate：提交时 gate 减少无效排队，执行前 gate
处理排队期间状态变化，pending/running 合并则关闭执行窗口内的重复提交。

## 21. Heap threshold 非对称平滑（已实现）

`c1000/p100K` 的退化运行中，true live 曾从约 `160MB` 短时升到 `500MB`，旧算法
立即把 threshold 按 `trueLive * (1 + heapGrowth)` 推到约 `1GB`。这会让暂态活跃图直接
放大后续 GC 的堆水位和 RSS，增加重复运行的波动。

当前 `EvaluateMSHeapThreshold()` 采用非对称规则：

1. `rawTarget = clamp(trueLive * (1 + heapGrowth), minThreshold, heapCap)`。
2. `rawTarget <= oldThreshold` 时立即下降到 raw target，不做平滑，使暂态高水位能快速恢复。
3. 上涨时的常规上限为 `oldThreshold * 1.125`，即单轮最多上涨 `12.5%`。
4. 同时保证 `trueLive * 1.25` 的安全 headroom；当 live 本身已大幅超过旧 threshold 时，
   安全下限可超过 `12.5%` 上限，避免新 threshold 低于 live 导致立即重复 GC。
5. 最终值始终不超过 raw target 和 heap cap；当配置的 `heapGrowth < 0.25` 时仍尊重原始配置。

例如 old threshold 为 `320MB`、true live 突增到 `500MB`、growth 为 `2x`时，raw target
是 `1GB`，常规 rise limit 是 `360MB`，live safety floor 是 `625MB`，最终采用 `625MB`。
日志新增 `rawTarget/riseLimit/liveSafetyFloor/riseLimited`，其中 `riseLimited=1` 表示本轮实际限速。
