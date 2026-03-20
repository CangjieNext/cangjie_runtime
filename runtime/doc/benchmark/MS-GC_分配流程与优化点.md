# MS-GC 对象分配流程与优化点

- **日期**: 2026-03-19
- **代码基线**: 当前工作区实现（以 runtime/src 中 MS-GC 代码为准）

---

## 1. 分配流程总览

```
编译器生成代码
  ↓
MCC_NewObject / MCC_NewArray              ← CompilerCalls.cpp
  ↓
ObjectManager::NewObject                  ← ObjectManager.inline.h
  ↓
MObject::NewObject → HeapManager::Allocate()
  ↓
RegionSpace::Allocate(size)               ← RegionSpace.cpp L172
  ├── ToAllocSize(size)   // size + HEADER_SIZE, 8字节对齐
  └── do { TryAllocateOnce() } while (retry)
        ↓
      TryAllocateOnceMS(allocSize)        ← RegionSpace.cpp
        │
        ├─ < 16KB  → AllocBufferMS::Allocate()    【小/中对象, 线程本地】
        └─ >= 16KB → MSRegionManager::TakeRegionForLarge()  【大对象, 主堆连续 region】
```

> **当前实现说明**:
> - 主分配路径已经收敛为 **两级路由**，不是旧文档中的“三级路由”。
> - `LargeObjectSpace` 仍然存在，主要用于 GC 标记/清扫对 heap 外对象的统一处理；但 `TryAllocateOnceMS()` 当前**不会**按 `>4MB` 直接分流到 `LargeObjectSpace::Allocate()`。

**重试策略** (`ShouldRetryAllocation`):
- 第1~N次: `CJThreadResched()` 让出CPU
- 第N+1次: 请求 GC (HEU reason)
- 第M次: 请求 GC (OOM reason)
- 超过M次: 抛出 `OutOfMemoryError`

## 2. 二级路由（当前实现）

`TryAllocateOnceMS()` 当前按 `kLargeObjectThreshold` 进行两级分流：

| 层级 | 大小范围 | 路径 | 数据结构 |
|------|---------|------|---------|
| **Normal** | < 16KB (kLargeObjectThreshold) | AllocBufferMS → CentralRegionPool | per-thread TLAB + 四链表中央池 |
| **Large** | >= 16KB | MSRegionManager::TakeRegionForLarge | 主堆连续 unit + MSRegionInfo span |

**注意**: 16KB~32KB 的对象曾有 gap bug — `SizeToClass()` 溢出查表，`AllocBufferMS` 返回 0，无限重试导致 OOM。已修复：在 `TryAllocateOnceMS` 中用 `kLargeObjectThreshold` 而非 `MS_REGION_SIZE` 作为切分点。

## 3. 小/中对象分配（< 16KB）

这是**最热路径**（>95% 分配走此路径）。

```
AllocBufferMS::Allocate(size)              ← AllocBufferMS.cpp L50
  │
  ├── SizeToClass(size) → cls              // O(1) 两级查表, 36个尺寸类
  │
  ├── FastAlloc(cls)  ──────────────────── 【无锁快速路径】
  │   ├── activeRegions_[cls] 存在?
  │   │   ├── BUMP region  → AllocBump()           // 指针递增, ~3条指令
  │   │   └── PARTIAL region → AllocFromBitmapFast() // CTZ扫bitmap, ~8-15条指令
  │   └── 成功 → 返回地址
  │
  ├── SlowAlloc(cls) ──────────────────── 【慢速路径】
  │   ├── ReturnRegion(cls)        // 归还旧region到CentralRegionPool
  │   │   ├── 完全空闲 → ResetToBump() → ReturnPartialRegion
  │   │   ├── 部分空闲 → ReturnPartialRegion
  │   │   └── 满     → ReturnFullRegion
  │   ├── cachedRegion_[cls]       // ★ 先查本地预取缓存
  │   ├── RefillFromCentral(cls)   // 缓存空时从中央池取
  │   │   ├── P1: partialSwept    (CAS无锁Pop, 最快)
  │   │   ├── P2: partialUnswept  (需现场sweep)
    │   │   ├── P3: fullUnswept     (sweep后可能转成partial)
    │   │   └── fallback: GetFreshRegion  (无可用缓存时从MSRegionManager取新region)
  │   ├── cachedRegion_[cls] = GetRegion(cls)  // ★ 预取下一个
  │   └── 从新region分配
  │
  └── Mark-at-alloc ──────────────────── 【GC阶段特殊处理】
      └── if phase ∈ [MARK_SETUP, SWEEP]:
          ├── MarkObject(newObj)    // 新对象标黑, 保证存活
        └── // gcAssistBytes_ 仍在结构体中保留，但当前分配热路径未使用
```

### 关键数据结构 — AllocBufferMS（每线程一个）

```
Per-Thread AllocBufferMS
├── activeRegions_[36]    // 每个SizeClass最多一个活跃region
├── cachedRegion_[36]     // ★ 每个SizeClass预取的备用region（已实现）
├── central_              // 指向全局CentralRegionPool
└── gcAssistBytes_        // mutator辅助计数器（当前仅保留字段，未接入分配路径）
```

- **FastAlloc 无锁**: activeRegions_ 是线程本地的，无需同步
- **SlowAlloc**: P1路径无锁(CAS)，P2/P3/P4需持 `bucket.lock`

## 4. 两种分配模式

### Bump-pointer 模式 (MSRegionInfo::AllocBump)

**触发条件**: 全新 region (`MS_REGION_BUMP`)

```cpp
// MSRegionInfo.h — inline
uintptr_t AllocBump() {
    size_t slotSize = SizeClassInfo::ClassToSize(sizeClass);
    if (allocPtr + slotSize <= regionEnd) {
        uintptr_t addr = allocPtr;
        allocPtr += slotSize;    // 指针递增
        freeSlotCount--;
        return addr;
    }
    return 0;  // 满了
}
```

**性能**: ~3条指令（load + add + store），与 CopyGC bump-pointer 一样快。

### Bitmap 扫描模式 (MSRegionInfo::AllocFromBitmapFast)

**触发条件**: sweep 过的 region (`MS_REGION_PARTIAL`)

```cpp
// MSRegionInfo.h — inline（实际实现）
uintptr_t AllocFromBitmapFast() {
    uint64_t summary = reserved;   // reserved 复用为 summary word: bit[i]=1 表示 allocBitmap[i] 有空闲
    while (summary != 0) {
        int wordIdx = __builtin_ctzll(summary);         // O(1) 找到首个有空闲的 bitmap word
        uint64_t word = allocBitmap[wordIdx];
        if (word != 0) {
            int bit = __builtin_ctzll(word);              // 找到 word 内首个空闲 slot
            allocBitmap[wordIdx] = word & ~(1ULL << bit); // 标记已分配
            freeSlotCount--;
            if (allocBitmap[wordIdx] == 0)                // word 已满，更新 summary
                reserved &= ~(1ULL << wordIdx);
            size_t slotIndex = wordIdx * 64 + bit;
            return RegionStart() + slotIndex * slotSize;
        }
        // summary 过期，自修正
        summary &= ~(1ULL << wordIdx);
        reserved &= ~(1ULL << wordIdx);
    }
    return 0;  // Region 已满
}
```

**性能**: ~5-8 条指令（两次 CTZ + bitwise），比 bump-pointer 慢约 2-3 倍。已实现 **summary word 分层索引**（O4 原优化建议），无线性扫描。

> **注意**: `allocBitmap` 中 `1=free, 0=allocated`（与 markBitmap 语义相反）。`reserved` 字段被复用为 bitmap word 级别的 summary 索引。

### BUMP 模式复用（已实现）

Region 首次使用为 BUMP → 经 sweep 后变 PARTIAL。但 **完全空闲的 Region 可通过 `ResetToBump()` 恢复为 BUMP 模式**（在 `ReturnRegion` 和 P2/P3 sweep 发现全空时触发）。

此优化（原 O3 建议）**已在代码中实现**。

## 5. CentralRegionPool 四链表与获取路径

`CentralRegionPool` 当前每个 size class 维护四条链表，但真正的获取路径是“P1/P2/P3 + fresh fallback”，不是把 `fullSwept` 当作直接分配优先级：

```
每个 SizeClass 一个 Bucket:

┌── SizeClassBucket[cls] ──────────────────────────┐
│  std::mutex lock   ← 仅保护 P2/P3/P4 链表       │
│                                                   │
│  P1: partialSwept  ← LockFreeRegionStack (CAS)   │ ★ 无锁
│  P2: partialUnswept ← MSRegionList (加锁)        │
│  P3: fullUnswept    ← MSRegionList (加锁)        │
│  fullSwept          ← MSRegionList (加锁, 存储态) │
└───────────────────────────────────────────────────┘
```

**获取流程**:
1. **P1 partialSwept**: **CAS 无锁 Pop**（`compare_exchange_weak`）→ 直接返回（最快，无 sweep 无锁）
2. **P2 partialUnswept**: 加锁取出 → CAS sweepGen 抢 sweep 所有权 → 现场 sweep → 全空时 `ReleaseRegionInternal()` 归还给 `MSRegionManager`，否则返回 `PARTIAL`
3. **P3 fullUnswept**: 加锁取出 → CAS sweep → 有空闲则返回，仍满则移到 fullSwept
4. **fallback / GetFreshRegion**: 前三步都没有可用 region 时，从 `MSRegionManager` 分配全新 region（`InitForSizeClass + allocBitmap`）

> **P1 的 LockFreeRegionStack**: Push/Pop 均使用 CAS 循环，memory_order 为 Push=release, Pop=acquire。注释说明"No ABA risk"——因为 Pop 后 Region 被线程独占。

**归还分类** (ReturnRegion):
- 完全空闲 → `ResetToBump()` 后走 `ReturnPartialRegion()`，保留 bitmap，作为 BUMP region 复用
- 部分空闲 → `ReturnPartialRegion()` → partialSwept / partialUnswept
- 满 → `ReturnFullRegion()` → fullSwept 链表

**延迟 sweep 翻代** (PrepareForSweep):
- STW 内先 `FlushAllAllocBuffers()`，再做 generation flip，避免 mutator 持有旧 region 跨代
- `partialSwept` 会被 drain 到 `partialUnswept`，`fullSwept` 与 `fullUnswept` 交换
- `globalSweepGen_ += 2`，O(1) 操作
- 实际 per-region sweep 由 lazy sweep 或 `BackgroundSweep()` 完成；当前 `NotifyToSweep()` 已接线

## 6. 大对象分配（当前实现：>= 16KB 都走主堆）

`TryAllocateOnceMS()` 当前大对象路径如下：

```
allocSize >= 16KB?
  ↓ YES
MSRegionManager::TakeRegionForLarge(unitCount, regionStart)
  │
  ├── 成功 → RegisterLargeObjectMS(addr, regionBytes)
  │         ├── InitForLargeObject(start, end)
  │         ├── 初始化所有 span MSRegionInfo (reserved = baseIdx)
  │         ├── baseInfo->isMarked = 1  (防止被误sweep)
  │         └── CentralRegionPool 登记 (byte accounting)
  │
  └── 失败 → RequestGC(OOM) → WaitForGCFinish → 重试一次 TakeRegionForLarge
```

**TakeRegionForLarge 内部**:
- 会把 `unitCount` 向上补齐到 `unitsPerMSRegion_` 的整数倍，保证对象跨度与 `MS_REGION_SIZE` 边界一致
- `TakeUnitsAligned()` 优先从 `dirtyTree/releasedTree` 复用，再尝试 `inactiveZone` 和 `GrowOneSegment()`

**当前状态**:
- 大对象分配路径里的同步 `ForceFinishSweep()` 已移除
- 剩余问题更偏向于 sweep 及时性/一致性，以及大对象需要连续 unit 时对碎片更敏感
- `LargeObjectSpace` 基础设施依然存在，但当前主分配路径没有基于阈值切过去


## 8. 关键参数

| 参数 | 值 | 说明 |
|------|-----|------|
| MS_REGION_SIZE | 32KB | region 粒度 |
| ALLOC_ALIGN | 8B | 最小对齐 |
| kNumSizeClasses | 36 | 覆盖 8B ~ 16KB |
| kLargeObjectThreshold | 16KB | 小→大分界线 |
| kGCAssistTriggerBytes | 64KB | mutator 辅助标记触发阈值 |
| MSRegionInfo | 64B | 一个 cache line，per-region 元数据 |
| SizeClass 步长 | 8B→2048B | 小尺寸步长小，大尺寸步长大 |

> **注**: 代码中仍保留“Tier 2 / >4MB LOS”相关注释和 `LargeObjectSpace` 实现，但当前 `TryAllocateOnceMS()` 没有 `kHugeObjectThreshold` 这一路由常量或分支。

### SizeClass 分布

| 范围 | 类索引 | 步长 | region内槽数 |
|------|--------|------|:----------:|
| 8-128B | 0-9 | 8-16B | 256-4096 |
| 160-512B | 10-17 | 32-64B | 64-204 |
| 640-2048B | 18-25 | 128-256B | 16-52 |
| 2560-8192B | 26-32 | 512-2048B | 4-13 |
| 10240-16384B | 33-35 | 2048B | 2-3 |

## 9. 优化点分析

### 热路径层级与锁分布

```
层级              函数                          锁        频率
─────────────────────────────────────────────────────────────
编译器入口        MCC_NewObject                 无        ★★★★★
堆路由            TryAllocateOnceMS             无        ★★★★★
线程本地分配      AllocBufferMS::FastAlloc      无        ★★★★★  ← 最热
  ├── BUMP        MSRegionInfo::AllocBump       无        ★★★★
  └── BITMAP      MSRegionInfo::AllocFromBitmapFast  无   ★★★★
慢速补充          AllocBufferMS::SlowAlloc      无        ★★★
中央池取region    CentralRegionPool::GetRegion  P1无锁 / 其余加锁  ★★★
  └── 现场sweep   MSRegionInfo::SweepRegion     CAS       ★★
新region分配      MSRegionManager::TakeRegion   mutex     ★★
大对象分配        TakeRegionForLarge            mutex     ★
后台清扫          BackgroundSweep               bucket.lock/CAS   ★
```

---

### O1: CentralRegionPool partialSwept 去锁 ✅ 已实现

**状态**: ✅ **已实现** — `partialSwept` 已改为 `LockFreeRegionStack`，Push/Pop 均为 CAS 无锁操作。

**实现**: `LockFreeRegionStack` 使用 `compare_exchange_weak` 循环（Push=release, Pop=acquire）。P1 路径无需持 `bucket.lock`，仅 P2/P3/P4 需加锁。注释说明"No ABA risk"——Pop 后 Region 被线程独占。

---

### O2: SlowAlloc 批量预取 region ✅ 已实现

**状态**: ✅ **已实现** — `AllocBufferMS` 中已有 `cachedRegion_[kNumSizeClasses]` 数组，SlowAlloc 在分配完成后立即预取下一个 region 到缓存。

**实现**: SlowAlloc 末尾调用 `cachedRegion_[cls] = GetRegion(cls)` 异步预取。下次 SlowAlloc 时优先使用缓存，避免立即访问 CentralRegionPool。当前为 1:1 预取（每 SizeClass 缓存 1 个）。

**后续优化空间**: 可考虑增大预取深度（2-4个），进一步降低锁竞争频率。

---

### O3: BUMP 模式复用 ✅ 已实现

**状态**: ✅ **已实现** — `MSRegionInfo::ResetToBump()` 在以下路径触发：
- `ReturnRegion()` 检测到完全空闲的 region → 调用 `ResetToBump()` → 归还为 partialSwept
- P2/P3 sweep 后发现全空 → `ResetToBump()` → 重新作为 BUMP region 使用

**效果**: 完全空闲 region 自动恢复为 BUMP 快速路径（~3条指令 vs bitmap ~5-8条指令）。

---

### O4: Bitmap 分层索引 ✅ 已实现

**状态**: ✅ **已实现** — `MSRegionInfo::reserved` 字段被复用为 summary word，`bit[i]=1` 表示 `allocBitmap[i]` 有空闲。

**实现**: `AllocFromBitmapFast()` 中先 CTZ(summary) 得到有空闲的 word 索引，再 CTZ(word) 得到具体 slot，两次 CTZ 即完成 O(1) 分配。word 用尽时自动清除 summary 对应位。

**效果**: 消除了 bitmap 线性扫描，分配从 O(n) 降为 O(1)。

---

### O5: 大对象路径移除 ForceFinishSweep 阻塞 ✅ 已实现

**当前实现**: 大对象分配失败后，`TryAllocateOnceMS()` 不再先调用 `ForceFinishSweep()`；而是直接触发 `RequestGC(GC_REASON_OOM)`，等待 GC 完成后只重试一次 `TakeRegionForLarge()`。

**收益**: 移除了分配线程上的同步 sweep 阻塞，把 sweep 推回 GC 阶段 / 后台线程 / lazy sweep。

**剩余问题**:
- `PrepareForSweep: fullUnswept COUNT MISMATCH`
- `ForceFinishSweep CAS FAIL` 仍说明 sweep 子系统的一致性问题没有解决
- 大对象仍需要连续 unit，对碎片和 sweep 及时性敏感

**结论**: O5 的“去阻塞”部分已经落地，但“让 sweep 更稳定、更及时”仍是当前性能与正确性瓶颈。

---

### O6: Mark-at-alloc 相位检查消除 ⭐

**现状**: 每次成功分配后都读 `GetMutatorPhase()`；当前相位范围是 `MARK_SETUP ... SWEEP`，绝大部分时间 GC idle，分支不会命中。

**问题**: 虽有 `UNLIKELY` 标注，CPU 仍需做 branch prediction + 内存访问（读 mutatorPhase atomic）。

**方案**: 用函数指针分发——GC idle 时使用无 mark-at-alloc 的 Allocate；进入 mark 阶段时切换为带 mark-at-alloc 的版本:
```cpp
using AllocFn = MAddress(*)(AllocBufferMS*, size_t);
AllocFn allocFn_;  // GC idle: AllocateIdle;  GC marking: AllocateMarking
```

**预期**: 消除 GC idle 期间每次分配的相位检查开销。

---

### O7: GetOrCreateAllocBufferMS TLS 开销 ⭐

**现状**: 每次小对象分配调用 `GetOrCreateAllocBufferMS()` → TLS lookup。

**方案**: 在 Mutator 结构中内联 AllocBufferMS 指针，避免额外 TLS 查询。

---

## 10. 基于当前代码的新增优化建议

### O8: 修复 PrepareForSweep 链表一致性，再谈 sweep 并发度

**依据**: 当前代码在 `PrepareForSweep()` 中仍能打出 `fullUnswept COUNT MISMATCH`，说明链表计数和实际遍历不一致，继续叠加 sweep 优化会放大正确性风险。

**建议**:
- 优先把 `MSRegionList::count_` 与真实链路维护修正到可验证状态
- 给 `DrainTo/Swap/TakeHead/Prepend/Remove` 增加一致性断言或 debug-only 校验
- 先保证 sweep 代际翻转正确，再评估更激进的并发 sweep 策略

### O9: 把 cachedRegion 预取从“同步拿一个”改成“按压力自适应”

**依据**: 当前 `SlowAlloc()` 在设置新 active region 后，会同步再调一次 `central_->GetRegion(cls)` 预取一个缓存 region。这个动作发生在 mutator 线程上，可能把慢路径锁竞争拉长。

**建议**:
- 仅在该 size class 近期频繁 miss 时才预取
- 或者改成后台/批量填充，而不是每次 SlowAlloc 都立即再取一个
- 同时记录各 size class 的 cache hit/miss，再决定是否值得扩成 2-depth cache

### O10: 区分“GC 运行中”和“需要 mark-at-alloc”的更细粒度快路径

**依据**: 当前 `Allocate()` 只要成功分配，就会读取 mutator phase，并在 `MARK_SETUP ... SWEEP` 整段区间内执行 `MarkObject()`。这保证了正确性，但把 idle 和 active-GC 的热路径混在一起。

**建议**:
- 用函数指针或轻量状态位切分 idle / active-GC 两种分配函数
- 至少先把 idle 场景做成无 phase 读取的纯 fast path
- 如果担心切换成本，可只在 phase 迁移时更新线程本地标记

### O11: 大对象优先消耗 inactiveZone，减少对树结构的连续块竞争

**依据**: `TakeUnitsAligned()` 当前优先尝试 `dirtyTree/releasedTree`，复用旧空间有利于压低 watermark，但对需要连续大块的对象，碎片化区间的尝试成本更高。

**建议**:
- 对大对象请求增加“优先 inactiveZone / committed tail”的策略分支
- 当 `unitCount` 超过阈值时，先尝试从 committed tail 直接 bump 获取连续块
- 失败后再回退到树结构，避免在碎片严重时反复扫描和切分

---

### 优化收益排序

| 优先级 | 优化 | 状态 | 预期收益 | 改动量 | 风险 |
|:------:|------|:----:|:--------:|:------:|:----:|
| ~~P0~~ | ~~O5: 大对象路径移除 ForceFinishSweep 阻塞~~ | ✅ 已实现 | — | — | — |
| **P0** | O8: 修复 PrepareForSweep 链表一致性 | ❌ 未实现 | 正确性优先 | 中 | 中 |
| ~~P0~~ | ~~O1: partialSwept 去锁~~ | ✅ 已实现 | — | — | — |
| ~~P1~~ | ~~O2: 批量预取 region~~ | ✅ 已实现 | — | — | — |
| ~~P1~~ | ~~O3: BUMP 模式复用~~ | ✅ 已实现 | — | — | — |
| ~~P2~~ | ~~O4: Bitmap 分层索引~~ | ✅ 已实现 | — | — | — |
| **P1** | O11: 大对象优先 inactiveZone | ❌ 未实现 | 3~8% (大对象/碎片场景) | 中 | 中 |
| **P1** | O6: Mark-at-alloc 消除 | ❌ 未实现 | <1% | 中 | 中 |
| **P1** | O9: cachedRegion 自适应预取 | ❌ 未实现 | 1~3% | 中 | 中 |
| **P2** | O7: TLS 开销消除 | ❌ 未实现 | <1% | 小 | 低 |
| **P2** | O10: idle/active-GC 双快路径 | ❌ 未实现 | <1% | 中 | 中 |
