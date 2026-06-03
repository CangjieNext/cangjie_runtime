# Mutator Assist 实现详解

## 1. 背景与目标，主要逻辑

### 背景与目标

MSGC 并发标记阶段，GC 线程和 Mutator（应用线程）同时运行。核心风险是：Mutator 分配新对象的速度可能超过 GC 线程扫描对象的速度，导致堆持续膨胀直至 OOM。

Mutator Assist 是应对这一风险的**背压（back-pressure）机制**：让每个 Mutator 在分配内存时按分配量等比例做一定量的标记工作，保证 GC 扫描进度不会落后于分配速度。设计思想来自 Go runtime 的 gcAssist。

### 主要逻辑

整体流程分为两条路径，最终都汇入同一个 Assist 执行函数：

```
┌── 小对象分配（SlowAlloc 每次获取 Region 时）
│       AccumulatePacingDebt(MS_REGION_SIZE)
│
└── 大对象分配（TryAllocateLargeObjectOnce 完成后）
        AccumulatePacingDebt(allocSize)
            │
            ├─ collector == nullptr 或 ratioScaled == 0？
            │       → 清零 gcAssistDebtScaled_，直接返回
            │
            └─ 累积 gcAssistDebtScaled_ += size × ratioScaled
                    │
                    └─ debtObjects = gcAssistDebtScaled_ / kAssistRatioScale(1024)
                            │
                            └─ debtObjects ≥ kAssistDebtTriggerObjects(64)?
                                    └─ MSMarkAssist(debtObjects)
                                            ├─ TryStealBgCreditPartial()  // 先用 GC 信用抵扣
                                            └─ 循环扫描 per-P gcWork 中的灰色对象
```

关键时序约束：
- `AccumulatePacingDebt` 在**分配路径上同步调用**，Mutator 在 Assist 未完成前不会返回分配结果
- `MSMarkAssist` 每次最多处理 `kAssistMaxObjectsPerCall = 512` 个对象，保证单次延迟可控
- GC 线程 `assistEnabled_.store(false)` 后，`MSMarkAssist` 在下一次 `assistEnabled_` 检查时退出（最多延迟 128 次循环）

---

## 2. 关键数据结构，参数的意义

### 2.1 Per-Thread 欠债：`gcAssistDebtScaled_`（MSAllocBuffer）

```cpp
uint64_t gcAssistDebtScaled_ = 0;
// 单位：对象数 × kAssistRatioScale(1024)
// debtObjects = gcAssistDebtScaled_ / 1024
```

使用定点数而非浮点数的原因：每次分配只累积很小的 debt（可能远小于 1 个对象），用整数定点缩放 1024 倍可以精确积累小额债务而无精度损失。

### 2.2 全局动态比率：`assistWorkPerByteScaled_`（MarkSweepCollector）

```cpp
std::atomic<uint64_t> assistWorkPerByteScaled_{0};
// 含义：每分配 1 字节需要"还"多少扫描量（单位 × kAssistRatioScale）
// 实际 ratio = assistWorkPerByteScaled_ / 1024  （对象/字节）
```

这是控制节流力度的核心参数，每轮 GC 开始时初始化，并在并发标记循环中持续动态更新（详见第 3 节）。

### 2.3 全局背景信用：`bgScanCredit_`（MarkSweepCollector）

```cpp
std::atomic<int64_t> bgScanCredit_{0};
```

GC 线程每扫描一批对象就往这里积累"信用"。Mutator 进入 Assist 时**优先消费信用**，信用足以覆盖全部债务时直接跳过实际扫描（`assistFastSkipCount_++`）。这是防止 GC 线程已经领先时 Mutator 仍被无谓阻塞的关键优化。

### 2.4 全局剩余扫描量：`scanWorkRemaining_`（MarkSweepCollector）

```cpp
std::atomic<int64_t> scanWorkRemaining_{0};
// 初始值 = prevCycleMarkedObjects_（上一轮 GC 扫描的对象总数）
```

GC 线程和 Assist 线程共同消耗这个计数器。它被用来实时计算 `dynamicRatioScaled`——当剩余量趋近于 0 时，比率趋近于 0，Mutator 自然不再积累债务。

写屏障 overflow 产生的新灰色对象会通过 `scanWorkRemaining_.fetch_add(pushed)` 补回，防止比率过早归零。

### 2.5 Per-P 工作队列：`gcWork_`（AllocBufferMS 中的 `PerPGCWork`）

```
PerPGCWork
├── wbuf1_: WorkBuf*    // 主本地缓冲，容量 128 对象
├── wbuf2_: WorkBuf*    // 次本地缓冲（双缓冲）
└── globalFull_: LFStack*  // 全局溢出/补充池（指针，不拥有）
```

对象来源：写屏障 overflow 打包压入 `globalFull_`，根对象由 GC 线程初始注入。Mutator Assist 从 `wbuf1_/wbuf2_` 取对象，为空时从 `globalFull_` 补充。

### 2.6 关键常量一览

| 常量 | 值 | 含义 |
|------|----|------|
| `kAssistRatioScale` | 1024 | 定点数缩放因子，避免整数精度损失 |
| `kAssistDebtTriggerObjects` | 64 | 欠债超过 64 个对象等价量时触发 Assist |
| `kAssistMaxObjectsPerCall` | 512 | 单次 Assist 最多处理对象数，控制单次延迟 |
| `kCreditFlushThreshold` | 1024 | GC 线程每扫描 1024 个对象刷新一次 credit 和动态比率 |
| `kSliceMaxObjects` | 256 | GC 线程每轮 Drain 的最大对象数 |
| `kConvergenceRequiredRounds` | 3 | 连续 3 轮工作队列为空才认定收敛 |

---

## 3. 动态因子的计算与设置

### 3.1 初始值（每轮并发标记开始时）

在 `ConcurrentMark()` 的初始化阶段（`MarkSweepCollector.cpp`），按上一轮 GC 的实测数据计算初始比率：

```cpp
// 本轮允许分配的字节预算 = 下轮 GC 触发阈值 - 当前存活字节
size_t allocBudgetBytes = gcStats.GetThreshold() - gcStats.lastTrueLiveBytes;

// 上一轮并发标记扫描的对象总数（作为本轮预估）
size_t scanEstimateObjects = prevCycleMarkedObjects_;

// ratioScaled = ceil(scanEstimateObjects × 1024 / allocBudgetBytes)
// 意义：每分配 1 字节，需要帮 GC 扫描 (ratioScaled/1024) 个对象
ratioScaled = (scanEstimateObjects * 1024 + allocBudgetBytes - 1) / allocBudgetBytes;
assistWorkPerByteScaled_.store(ratioScaled);

// 同步初始化剩余扫描量
scanWorkRemaining_.store(scanEstimateObjects);
```

**直觉理解**：如果本轮需要扫描 1 亿个对象，而分配预算只有 1GB，那么每分配 10 字节就要扫描 1 个对象（ratio ≈ 0.1 obj/byte），这样等所有预算用完时标记也恰好完成。

### 3.2 动态更新（并发标记循环中）

GC 线程在 `RunConcurrentMarkLoop()` 中每扫描 `kCreditFlushThreshold = 1024` 个对象，就刷新一次比率：

```cpp
// 刷新 bgScanCredit
bgScanCredit_.fetch_add(localCreditAccum);

// 用剩余量重新计算比率
int64_t remainingScan = scanWorkRemaining_.load();
size_t newAlloc = allocatedBytes - assistAllocBaselineBytes_;
size_t remainingAllocBudget = assistThresholdBytes_ - (assistLiveBaselineBytes_ + newAlloc);

// dynamicRatioScaled = ceil(remainingScan × 1024 / remainingAllocBudget)
dynamicRatioScaled = (remainingScan * 1024 + remainingAllocBudget - 1) / remainingAllocBudget;
assistWorkPerByteScaled_.store(dynamicRatioScaled);
```

**为什么要动态更新**：初始估计基于上一轮数据，可能高估（如本轮对象少）或低估（如本轮写屏障产生大量新对象）。动态更新使比率始终反映"当前剩余标记量 ÷ 当前剩余分配预算"，随 GC 进度自然收窄，GC 即将完成时 Mutator 不再被节流。

### 3.3 终止清零

并发标记结束时（不论正常结束还是收敛退出）：

```cpp
assistEnabled_.store(false);
assistWorkPerByteScaled_.store(0);   // 清零：Mutator 不再积累 debt
scanWorkRemaining_.store(0);
```

`AccumulatePacingDebt` 在检测到 `ratioScaled == 0` 时同样会清零 `gcAssistDebtScaled_`，确保旧债不跨周期积累。

---

## 4. 欠账计算与归还

### 4.1 欠账积累（`AccumulatePacingDebt`）

调用时机：
- **小对象**：每次 `SlowAlloc()` 获取一个新 Region 时，调用一次 `AccumulatePacingDebt(MS_REGION_SIZE)`。粒度为 Region 级别（约 64KB），而非每个对象，避免每次分配都触发原子读。编译器快速路径（bump-pointer inline）同样在 SlowAlloc 时统一记账。
- **大对象**：`TryAllocateLargeObjectOnce` 完成后，调用 `AccumulatePacingDebt(allocSize)` 按实际大小记账。

积累逻辑：

```cpp
void AllocBufferMS::AccumulatePacingDebt(size_t size)
{
    MarkSweepCollector* collector = MarkSweepCollector::GetActiveCollector();
    if (collector == nullptr) { gcAssistDebtScaled_ = 0; return; }

    uint64_t ratioScaled = collector->GetAssistWorkPerByteScaled();
    if (ratioScaled == 0) { gcAssistDebtScaled_ = 0; return; }   // 非标记阶段

    // 按分配字节数 × 动态比率积累 debt（饱和加法，防止回绕）
    uint64_t deltaDebt = size * ratioScaled;
    gcAssistDebtScaled_ = saturating_add(gcAssistDebtScaled_, deltaDebt);

    // 折算成对象数，超过触发阈值则归还
    size_t debtObjects = gcAssistDebtScaled_ / kAssistRatioScale;
    if (UNLIKELY(debtObjects >= kAssistDebtTriggerObjects)) {   // 64 个对象等价量
        size_t paidObjects = collector->MSMarkAssist(debtObjects);
        size_t accounted = min(paidObjects, debtObjects);
        gcAssistDebtScaled_ -= accounted * kAssistRatioScale;   // 减去已还部分
    }
}
```

### 4.2 归还路径（`MSMarkAssist`）

归还分两步，优先消费 GC 线程的预存信用，不足时才做实际扫描：

**Step 1 — 偷信用（`TryStealBgCreditPartial`）**

```cpp
int64_t stolen = min(debtObjects, bgScanCredit_);
// CAS 从 bgScanCredit_ 中减去 stolen
// 若 stolen >= debtObjects → 快速路径，assistFastSkipCount_++，直接返回
```

**Step 2 — 实际扫描（最多 `kAssistMaxObjectsPerCall = 512` 个对象）**

```cpp
for (size_t i = 0; i < assistBudget; i++) {
    if ((i & 127) == 0 && !assistEnabled_) break;  // safepoint 检查

    BaseObject* obj = gcWork_.TryGetObj();   // 从 per-P 队列取灰色对象
    if (obj == nullptr) break;               // 队列空，无法继续

    if (!obj->HasRefField()) continue;
    if (obj->IsWeakRef()) { PushSingleToDeferredMainThread(obj); continue; }

    TraceObjectRefFieldsToGCWork(obj, gcWork_);  // 追踪子对象（白→灰，CAS markBit）
    nTraced++;
}
// 更新全局剩余扫描量
ConsumeAssistRemaining(scanWorkRemaining_, nProcessed);
```

**归还记账**：`MSMarkAssist` 返回 `paidByCredit + nProcessed`（共还了多少），调用方 `AccumulatePacingDebt` 用这个值从 `gcAssistDebtScaled_` 中减去对应量：

```cpp
gcAssistDebtScaled_ -= min(paidObjects, debtObjects) * kAssistRatioScale;
```

防止下溢（`min` 保证不超过原始债务量）。

### 4.3 欠账的生命周期

| 阶段 | `gcAssistDebtScaled_` 变化 |
|------|--------------------------|
| 非标记阶段 | `AccumulatePacingDebt` 检测到 `ratioScaled == 0` → **清零** |
| 标记开始 | `assistWorkPerByteScaled_` 非零 → 按分配量**正常积累** |
| 标记进行中 | 每次分配积累，超 64 obj 等价量时**触发归还** |
| GC 线程领先 | `bgScanCredit_` 充足 → **信用快速抵扣**，无实际扫描开销 |
| 标记结束 | `ratioScaled` 清零 → 下一次分配时 `gcAssistDebtScaled_` **清零** |
| 跨周期 | 旧债不跨周期：标记结束后首次分配必然清零 |

---

## 5. 关键源码文件速查

| 文件 | 关键内容 |
|------|---------|
| `src/Heap/MCollector/MarkSweepCollector.cpp` | `ConcurrentMark()`（初始化 ratio）、`RunConcurrentMarkLoop()`（动态更新）、`MSMarkAssist()`、`TryStealBgCreditPartial()` |
| `src/Heap/MCollector/MarkSweepCollector.h` | 所有 assist 相关字段声明、常量定义 |
| `src/Heap/MAllocator/MSAllocBuffer.cpp` | `SlowAlloc()`（小对象触发点）、`AccumulatePacingDebt()`（统一欠账入口） |
| `src/Heap/MAllocator/MSAllocBuffer.h` | `gcAssistDebtScaled_` 字段 |
| `src/Heap/MAllocator/MSRegionSpace.cpp` | `TryAllocateLargeObjectOnce()`（大对象触发点） |
| `src/Common/LFStack.h` | `PerPGCWork`、`WorkBuf`、`LFStack` |

## 6. 相关环境变量

| 变量 | 说明 |
|------|------|
| `CJ_MS_ASSIST_DISABLE=1` | 完全禁用 Mutator Assist（调试用，不影响 GC 本身） |
| `CJ_MS_GC_DISABLE=1` | 禁用整个 GC（仅测纯分配性能） |
