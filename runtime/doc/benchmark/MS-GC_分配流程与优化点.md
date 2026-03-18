# MS-GC 对象分配流程与优化点

- **日期**: 2026-03-17
- **代码版本**: GO_GC 分支 (f2838a53)

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
      TryAllocateOnceMS(allocSize)        ← RegionSpace.cpp L37
        │
        ├─ < 16KB  → AllocBufferMS::Allocate()    【小/中对象, 线程本地】
        ├─ 16KB~4MB → MSRegionManager::TakeRegionForLarge()  【大对象】
        └─ > 4MB   → LargeObjectSpace::Allocate()  【超大对象, 独立mmap】
```

**重试策略** (`ShouldRetryAllocation`):
- 第1~N次: `CJThreadResched()` 让出CPU
- 第N+1次: 请求 GC (HEU reason)
- 第M次: 请求 GC (OOM reason)
- 超过M次: 抛出 `OutOfMemoryError`

## 2. 三级路由

`TryAllocateOnceMS()` (RegionSpace.cpp L37) 按大小分三级:

| 层级 | 大小范围 | 路径 | 数据结构 |
|------|---------|------|---------|
| **Normal** | < 16KB (kLargeObjectThreshold) | AllocBufferMS → CentralRegionPool | per-thread TLAB + 四优先级链表 |
| **Tier 1** | 16KB ~ 4MB | MSRegionManager::TakeRegionForLarge | CartesianTree bitmap |
| **Tier 2** | > 4MB (kHugeObjectThreshold) | LargeObjectSpace::Allocate | 独立 mmap/munmap |

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
  │   │   ├── P3: fullUnswept     (sweep后可能有空闲)
  │   │   └── P4: GetFreshRegion  (从MSRegionManager分配新region)
  │   ├── cachedRegion_[cls] = GetRegion(cls)  // ★ 预取下一个
  │   └── 从新region分配
  │
  └── Mark-at-alloc ──────────────────── 【GC阶段特殊处理】
      └── if phase ∈ [MARK_SETUP, CONCURRENT_MARK, MARK_TERMINATION]:
          ├── MarkObject(newObj)    // 新对象标黑, 保证存活
          └── // TODO(O5): gcAssistBytes mutator辅助逻辑当前被注释未生效
```

### 关键数据结构 — AllocBufferMS（每线程一个）

```
Per-Thread AllocBufferMS
├── activeRegions_[36]    // 每个SizeClass最多一个活跃region
├── cachedRegion_[36]     // ★ 每个SizeClass预取的备用region（已实现）
├── central_              // 指向全局CentralRegionPool
└── gcAssistBytes_        // mutator辅助计数器（当前未生效）
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

## 5. CentralRegionPool 四优先级补充

`CentralRegionPool::GetRegion(cls)` (CentralRegionPool.cpp L30):

```
每个 SizeClass 一个 Bucket:

┌── SizeClassBucket[cls] ──────────────────────────┐
│  std::mutex lock   ← 仅保护 P2/P3/P4 链表       │
│                                                   │
│  P1: partialSwept  ← LockFreeRegionStack (CAS)   │ ★ 无锁
│  P2: partialUnswept ← MSRegionList (加锁)        │
│  P3: fullUnswept    ← MSRegionList (加锁)        │
│  P4: fullSwept      ← MSRegionList (加锁)        │
└───────────────────────────────────────────────────┘
```

**获取流程**:
1. **P1 partialSwept**: **CAS 无锁 Pop**（`compare_exchange_weak`）→ 直接返回（最快，无 sweep 无锁）
2. **P2 partialUnswept**: 加锁取出 → CAS sweepGen 抢 sweep 所有权 → 现场 sweep → 全空时 ResetToBump → 返回
3. **P3 fullUnswept**: 加锁取出 → CAS sweep → 有空闲则返回，仍满则移到 fullSwept
4. **P4 GetFreshRegion**: 从 MSRegionManager 分配全新 region（InitForSizeClass + allocBitmap）

> **P1 的 LockFreeRegionStack**: Push/Pop 均使用 CAS 循环，memory_order 为 Push=release, Pop=acquire。注释说明"No ABA risk"——因为 Pop 后 Region 被线程独占。

**归还分类** (ReturnRegion):
- 完全空闲 → `ReleaseRegion()` 归还 MSRegionManager
- 部分空闲 → `ReturnPartialRegion()` → partialSwept 链表
- 满 → `ReturnFullRegion()` → fullSwept 链表

**延迟 sweep 翻代** (PrepareForSweep):
- `globalSweepGen_ += 2`，O(1) 操作
- 所有 "swept" region 逻辑上变成 "unswept"，无需移动链表

## 6. 大对象分配（16KB - 4MB）

`TryAllocateOnceMS()` 中 (RegionSpace.cpp L48):

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
  ├── 失败 → ForceFinishSweep()  // 同步sweep所有小对象region释放内存
  │         └── 重试 TakeRegionForLarge
  │
  └── 再失败 → RequestGC(OOM) → WaitForGCFinish → 最后重试一次
```

**TakeRegionForLarge 内部**: `TakeUnitsAligned()` 从 CartesianTree 找连续空闲 unit，按 `MS_REGION_SIZE` 对齐。

**性能瓶颈**: `ForceFinishSweep()` 在大对象分配路径上同步阻塞，遍历 36 个 bucket 的所有 unswept region 逐个 sweep。


## 8. 关键参数

| 参数 | 值 | 说明 |
|------|-----|------|
| MS_REGION_SIZE | 32KB | region 粒度 |
| ALLOC_ALIGN | 8B | 最小对齐 |
| kNumSizeClasses | 36 | 覆盖 8B ~ 16KB |
| kLargeObjectThreshold | 16KB | 小→大分界线 |
| kHugeObjectThreshold | 4MB | 大→超大分界线 |
| kGCAssistTriggerBytes | 64KB | mutator 辅助标记触发阈值 |
| MSRegionInfo | 64B | 一个 cache line，per-region 元数据 |
| SizeClass 步长 | 8B→2048B | 小尺寸步长小，大尺寸步长大 |

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
中央池取region    CentralRegionPool::GetRegion  mutex     ★★★   ← 竞争点
  └── 现场sweep   MSRegionInfo::SweepRegion     CAS       ★★
新region分配      MSRegionManager::TakeRegion   mutex     ★★
大对象分配        TakeRegionForLarge            mutex     ★
  └── 强制sweep   ForceFinishSweep              mutex     ★    ← 阻塞热点
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

### O5: ForceFinishSweep 去阻塞 ⭐⭐⭐

**现状**: 大对象分配失败时调用 `ForceFinishSweep()` 同步遍历 36 个 bucket 的所有 unswept region 逐个 sweep，阻塞当前线程。

**问题**: P2_LargeAlloc 差距 +26% 的主要来源之一。200 × 256KB 分配时频繁触发。

**方案**:
- 后台 sweep 线程持续推进（`BackgroundSweep` 已有但 `NotifyToSweep` 被注释掉）
- 分配时只 sweep 1 个 region（Go 的 `sweepone()`），其余交给后台
- 大对象优先从 inactive zone 扩展，避免依赖小对象 sweep 释放

**预期**: 大对象分配去掉同步 sweep 阻塞，P2 测试项 -8~12%。

---

### O6: Mark-at-alloc 相位检查消除 ⭐

**现状**: 每次分配后读 `GetMutatorPhase()` 判断是否在 mark 阶段。绝大部分时间 GC idle，分支永远不走。

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

### 优化收益排序

| 优先级 | 优化 | 状态 | 预期收益 | 改动量 | 风险 |
|:------:|------|:----:|:--------:|:------:|:----:|
| **P0** | O5: ForceFinishSweep 去阻塞 | ❌ 未实现 | -5~8% (P2,P3) | 中 | 中 |
| ~~P0~~ | ~~O1: partialSwept 去锁~~ | ✅ 已实现 | — | — | — |
| ~~P1~~ | ~~O2: 批量预取 region~~ | ✅ 已实现 | — | — | — |
| ~~P1~~ | ~~O3: BUMP 模式复用~~ | ✅ 已实现 | — | — | — |
| ~~P2~~ | ~~O4: Bitmap 分层索引~~ | ✅ 已实现 | — | — | — |
| **P1** | O6: Mark-at-alloc 消除 | ❌ 未实现 | <1% | 中 | 中 |
| **P2** | O7: TLS 开销消除 | ❌ 未实现 | <1% | 小 | 低 |
