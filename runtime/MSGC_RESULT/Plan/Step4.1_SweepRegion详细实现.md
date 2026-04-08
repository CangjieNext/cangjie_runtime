# Step 4.1：SweepRegion 详细实现

> 本文档是 [Step4 并发清扫详细设计](Step4_并发清扫详细设计.md) 的子文档。
>
> 2026-04-05 当前代码状态校准：
> 1. 当前 bitmap 存储已经切换到 `MSBitmapArena` sidecar slot；`MSRegionInfo` 只持有 `allocBitmap/markBitmap/scannedBitmap` 指针，不再是“每个 region 独立 mmap bitmap”。
> 2. `markBitmap` 的清零当前由 `MarkSweepCollector::ClearAllMarkBitmaps()` 在 `PreEnumRoots()` 统一完成，不是在 `SweepRegion` 内即扫即清，更不是 region 生命周期切换时立即 `munmap`。
> 3. 文中后续若出现 `ForceFinishSweep`、bitmap 独立 `mmap/munmap` 的表述，应按历史设计阅读，以当前 `MSBitmapArena + sidecar bitmap` 实现为准。

---

## 目录

1. [概述](#1-概述)
2. [bitmap NOT Sweep 算法](#2-bitmap-not-sweep-算法)
3. [sweepState 状态机](#3-sweepstate-状态机)
4. [Region 命运判定](#4-region-命运判定)
5. [markBitmap 清零时机分析](#5-markbitmap-清零时机分析)
6. [性能分析与 SIMD 优化潜力](#6-性能分析与-simd-优化潜力)
7. [与旧版差异对比](#7-与旧版差异对比)

---

## 1. 概述

`SweepRegion()` 是并发清扫的**原子操作单元**：对单个 Region 执行 bitmap 翻转，将标记信息转化为分配信息。

**核心等式**（来自 [Step1.1 §3.5.4](Step1.1_MS_RegionInfo元数据改造设计.md)）：
```
allocBitmap[i] = ~markBitmap[i]
```

设计原则：
- **不触碰死对象内存**：仅操作 bitmap 元数据，不向死对象空间写入 FreeSlot 指针
- **批量处理**：每次 NOT 操作处理 64 个 slot（一个 uint64_t word）
- **原子性**：通过 sweepState CAS 保证同一 Region 不被两个线程同时 sweep
- **来源**：`SweepRegion()` 方法定义在 `MSRegionInfo` 中（Step1.1 §3.11），本文档补充 sweep 上下文

---

## 2. bitmap NOT Sweep 算法

### 2.1 MSRegionInfo 内存布局回顾

每个 Region 的 `MSRegionInfo`（64 字节，位于 Region 头部 offset 0-63，[Step1.1 §3.4](Step1.1_MS_RegionInfo元数据改造设计.md)）：

```
偏移  字段               大小    说明
+0    allocPtr           8B     当前分配指针（bump-pointer 阶段使用）
+8    regionEnd          8B     Region 数据区结束地址
+16   nextRegionIdx      4B     双向链表
+20   prevRegionIdx      4B     双向链表
+24   allocBitmap*       8B     分配位图指针（0=occupied, 1=free）
+32   markBitmap*        8B     标记位图指针（0=dead, 1=alive）
+40   totalSlotCount     2B     总 slot 数
+42   freeSlotCount      2B     空闲 slot 数
+44   liveSlotCount      2B     存活 slot 数
+46   rawPointerObjCount 2B     裸指针对象计数
+48   sizeClass          1B     size class 索引 (0-35)
+49   sweepState         1B     清扫状态 (UNSWEPT/SWEEPING/SWEPT)
+50   regionType         1B     Region 类型 (FREE/BUMP/PARTIAL/FULL/LARGE)
+51   flags              1B     标志位
+52   padding            4B     对齐填充
+56   reserved           8B     预留
```

关键指针：
- `allocBitmap*`：指向独立 mmap 的分配位图（`InitForSizeClass()` 时分配，[Step1.1 §3.5.2](Step1.1_MS_RegionInfo元数据改造设计.md)）
- `markBitmap*`：指向独立 mmap 的标记位图（与 allocBitmap 紧邻分配）

### 2.2 bitmap 语义

```
allocBitmap:  bit=0 → slot 已占用,  bit=1 → slot 空闲（可分配）
markBitmap:   bit=0 → 对象未标记(dead),  bit=1 → 对象已标记(alive)
```

在 Step2 并发标记完成后：
- `markBitmap` 记录了本轮 GC 的存活对象
- `allocBitmap` 仍是上轮分配状态（已过时）

Sweep 的任务就是**根据 markBitmap 更新 allocBitmap**：
- 存活对象 (mark=1) → 不可分配 (alloc=0)
- 死亡对象 (mark=0) → 可分配 (alloc=1)

这正好是 **按位取反** 的关系：`allocBitmap = ~markBitmap`

### 2.3 SweepRegion() 完整实现

```cpp
// src/Heap/Allocator/MSRegionInfo.cpp (Step1.1 定义，Step4 使用)

size_t MSRegionInfo::SweepRegion() {
    ASSERT(sweepState == MS_SWEEP_STATE_SWEEPING);  // CAS 已抢占

    size_t wordCount = (totalSlotCount + 63) / 64;
    uint16_t live = 0;
    size_t freedSlots = 0;

    // ===== 核心循环：bitmap NOT + popcount =====
    for (size_t i = 0; i < wordCount; i++) {
        uint64_t markWord = markBitmap[i];

        // 1. 翻转：存活 → occupied(0), 死亡 → free(1)
        allocBitmap[i] = ~markWord;

        // 2. 统计存活数（用于 freeSlotCount/liveSlotCount 计算）
        live += __builtin_popcountll(markWord);
    }

    // 处理最后一个 word 的多余 bit（超出 totalSlotCount 的 bit 应为 0）
    size_t tailBits = totalSlotCount % 64;
    if (tailBits != 0) {
        // 将超出范围的 bit 清零（这些不是有效 slot）
        uint64_t tailMask = (1ULL << tailBits) - 1;
        allocBitmap[wordCount - 1] &= tailMask;
    }

    // ===== 清零 markBitmap（为下轮标记准备）=====
    memset(markBitmap, 0, wordCount * sizeof(uint64_t));

    // ===== 更新统计 =====
    liveSlotCount = live;
    freeSlotCount = totalSlotCount - live;
    freedSlots = freeSlotCount;  // 本次 sweep 释放的 slot 数

    // ===== 状态转换 =====
    sweepState = MS_SWEEP_STATE_SWEPT;

    return freedSlots;
}
```

### 2.4 示例走查

假设 sizeClass=4 (slotSize=64B)，一个 32KB Region 有 512 个 slot，需要 512/64 = 8 个 uint64_t word：

```
sweep 前:
  markBitmap[0] = 0xFF00FF00FF00FF00  (每隔 8 个 slot, 交替存活/死亡)
  allocBitmap[0] = 0x0000000000000000  (旧值, 全部占用)

sweep 后:
  allocBitmap[0] = ~0xFF00FF00FF00FF00 = 0x00FF00FF00FF00FF
                   (死亡 slot → bit=1 → 可分配)
  markBitmap[0] = 0x0000000000000000   (清零)
  live += popcount(0xFF00FF00FF00FF00) = 32  (每个 0xFF 有 8 个 1-bit)
```

### 2.5 与旧版 free-list sweep 的关键区别

| 维度 | 旧版 (free-list) | 新版 (bitmap NOT) |
|------|-----------------|------------------|
| 操作目标 | 遍历每个 slot，向死对象写 FreeSlot::next 指针 | 仅操作 bitmap 元数据 |
| 是否触碰死对象 | ✅ 写入 next 指针 | ❌ 不触碰 |
| 每 64 slot 操作数 | 64 次内存访问 + 条件判断 | 1 次 NOT + 1 次 popcount |
| cache 友好性 | 差（随机访问 slot 地址） | 好（连续 bitmap 访问） |
| 复杂度 | O(N) per slot | O(N/64) per word |
| 分配时使用 | 遍历 FreeSlot 链表 | CTZ(allocBitmap word) 找下一个 1-bit |

---

## 3. sweepState 状态机

### 3.1 状态定义

```cpp
// src/Heap/Allocator/MSRegionInfo.h (Step1.1 定义)
enum MSSweepState : uint8_t {
    MS_SWEEP_STATE_UNSWEPT  = 0,   // 未清扫, markBitmap 有效
    MS_SWEEP_STATE_SWEEPING = 1,   // 正在被某线程清扫 (CAS 抢占中)
    MS_SWEEP_STATE_SWEPT    = 2,   // 已清扫, allocBitmap 已翻转
};
```

### 3.2 状态转换图

```
                GC 完成 (PrepareForSweep)
                sweepGen += 2
                ┌──────────────────────────────────┐
                │                                  │
                ▼                                  │
         ┌──────────┐                              │
         │ UNSWEPT  │ ◀── 新分配 Region 也从此状态开始  │
         │ (state=0)│     (GetFreshRegion 设 sweepGen=当前代,
         └────┬─────┘      sweepState 初始为上一轮的 SWEPT,
              │             代际翻转后逻辑上变为 UNSWEPT)
              │
              │  CAS(sweepState, UNSWEPT, SWEEPING)
              │
         ┌────▼─────┐
         │ SWEEPING  │
         │ (state=1) │
         └────┬──────┘
              │
              │  allocBitmap = ~markBitmap
              │  memset(markBitmap, 0, ...)
              │  更新 liveSlotCount / freeSlotCount
              │
         ┌────▼─────┐
         │  SWEPT   │
         │ (state=2)│
         └──────────┘
              │
              │  下轮 GC: sweepGen += 2
              │  逻辑上变回 UNSWEPT
              └──────────▶ (回到顶部)
```

### 3.3 CAS 抢占协议

sweep 所有权通过 **两层 CAS** 保证（sweepGen + sweepState 双重保护）：

```cpp
// 在 CentralRegionPool::GetRegion() 或 BackgroundSweepLoop() 中:

bool TrySweepRegion(MSRegionInfo* region) {
    uint32_t sg = globalSweepGen_.load(std::memory_order_acquire);

    // 第一层：sweepGen CAS（对标 Go mspan.sweepgen）
    // 确认此 Region 属于需要 sweep 的代
    if (!region->CasSweepGen(sg - 2, sg - 1)) {
        return false;  // 已被他人抢占或已 sweep
    }

    // 第二层：sweepState CAS（本地状态保护）
    // 此处 sweepGen CAS 成功意味着我们拥有 sweep 权
    region->SetSweepState(MS_SWEEP_STATE_SWEEPING);

    // 执行 sweep
    region->SweepRegion();

    // 标记完成
    region->SetSweepGen(sg);  // sweepGen = 当前代
    // sweepState 已在 SweepRegion() 内设为 SWEPT

    return true;
}
```

**关键设计**：实际的竞争仲裁由 `sweepGen` CAS 完成（全局唯一）；`sweepState` 作为辅助状态，用于调试和断言检查。这与 Go 的 `mspan.sweepgen` CAS 一致。

### 3.4 并发安全分析

| 场景 | 行为 | 安全性 |
|------|------|--------|
| 线程 A、B 同时尝试 sweep Region R | A CAS 成功，B CAS 失败(跳过) | ✅ 无重复 |
| A sweep 中，B 尝试分配 R 中的 slot | 不可能：R 已从链表取出 | ✅ R 不在链表 |
| A sweep 完成，将 R 放回 partialSwept | B 可从 partialSwept 取到 R | ✅ R 已完成 sweep |
| ForceFinishSweep 与 Lazy Sweep 同时运行 | sweepGen CAS 互斥 | ✅ 不会重复 |

---

## 4. Region 命运判定

### 4.1 判定逻辑

`SweepRegion()` 完成后，根据 `freeSlotCount` 和 `liveSlotCount` 确定 Region 的下一步处理：

```cpp
// 在 CentralRegionPool::GetRegion() 中 sweep 完一个 Region 后:

void HandleSweptRegion(MSRegionInfo* region, uint8_t cls) {
    if (region->IsCompletelyFree()) {
        // ===== 全死：物理回收 =====
        // freeSlotCount == totalSlotCount, liveSlotCount == 0
        ReleaseRegion(region, cls);
        // → FreeRegionManager dirty 树 → 后续可被 GetFreshRegion 复用
    }
    else if (region->HasFreeSpace()) {
        // ===== 有空洞：放回 partialSwept =====
        // 0 < freeSlotCount < totalSlotCount
        ReturnPartialRegion(region, cls);
        // → partialSwept[cls] 链表 → 下次 GetRegion 可直接取用
    }
    else {
        // ===== 全满：放回 fullSwept =====
        // freeSlotCount == 0, liveSlotCount == totalSlotCount
        ReturnFullRegion(region, cls);
        // → fullSwept[cls] 链表 → 等下轮 GC
    }
}
```

### 4.2 命运判定状态图

```
                    SweepRegion() 完成
                         │
              ┌──────────┼───────────┐
              │          │           │
              ▼          ▼           ▼
          全死(0存活)  有空洞      全满(全存活)
              │          │           │
              ▼          ▼           ▼
       ReleaseRegion  partialSwept  fullSwept
              │          │           │
              ▼          │           │
       FreeRegionManager │           │
       (dirty 树)        │           │
              │          │           │
              ▼          │           │
       ReleaseGarbageRegions         │
       (madvise DONTNEED)            │
              │          │           │
              ▼          ▼           ▼
       released 树    被分配线程    等下轮 GC
       (等待复用)     GetRegion 取走  sweepGen +2 后
                                   变为 fullUnswept
```

### 4.3 辅助判定方法

```cpp
// MSRegionInfo 上的辅助方法 (Step1.1 定义)

bool IsCompletelyFree() const {
    return liveSlotCount == 0;  // 全死 → 整个 Region 可回收
}

bool HasFreeSpace() const {
    return freeSlotCount > 0 && liveSlotCount > 0;  // 有空洞
}

bool IsFull() const {
    return freeSlotCount == 0;  // 全满
}
```

### 4.4 全死 Region 的回收路径

```
ReleaseRegion(region, cls):
  1. region->ReleaseBitmaps()     // munmap allocBitmap + markBitmap
  2. region->InitFreeUnits()      // 重置 MSRegionInfo 元数据
  3. regionManager_->ReturnRegionToFreeManager(region)
     → FreeRegionManager::MergeInsert()
       → CartesianTree (dirty 树, 按地址排序)
```

**bitmap 释放**：全死 Region 不再需要 bitmap，通过 `munmap` 释放。当此 Region 被 `GetFreshRegion()` 再次获取时，`InitForSizeClass()` 会重新 `mmap` 新的 bitmap 对。（[Step1.1 §3.5.2](Step1.1_MS_RegionInfo元数据改造设计.md)）

---

## 5. markBitmap 清零时机分析

### 5.1 设计选择：sweep 时清零

本设计选择在 `SweepRegion()` 中**立即清零 markBitmap**：

```cpp
memset(markBitmap, 0, wordCount * sizeof(uint64_t));
```

### 5.2 备选方案对比

| 方案 | 时机 | 优点 | 缺点 |
|------|------|------|------|
| **A: sweep 时清零（采用）** | `SweepRegion()` 内 | 简单、语义清晰；bitmap 已在 cache 中 | 无 |
| B: 下轮 GC 开始时批量清零 | `ForceFinishSweep()` 或 `MarkSetup` | 可延迟开销 | 需额外记录哪些 Region 需要清零；ForceFinishSweep 已负责清零 |
| C: epoch 替代清零 | 不清零，mark 判定加 epoch 检查 | 免清零开销 | 增加 IsMarkedObject 热路径判断；复杂度高 |

### 5.3 选择理由

方案 A 最优，原因：
1. **cache 友好**：`SweepRegion()` 刚读完 markBitmap 做 NOT，此时 bitmap 在 L1 cache 中，清零几乎是免费的
2. **语义清晰**：sweep 完成后 markBitmap 保证为零，下轮 Mark 可直接使用
3. **无额外状态**：不需要 "needs-clear" 标记，减少状态管理复杂度
4. **性能实测**：一个 Region 的 markBitmap 通常 ≤ 1KB（512 slot / 8 = 64B; 最大 16KB slot 时 2 个 slot，仅 1B），`memset` 在现代 CPU 上极快

### 5.4 bitmap 生命周期优化

旧有 `ClearLiveInfo()` 会 `munmap` 释放 bitmap 内存。MS 模式下 bitmap 每轮 GC 都需要，频繁 mmap/munmap 浪费。优化方案：

```cpp
// 仅清零 bit，不释放内存（MS 模式专用）
void ClearBitmapMS() {
    if (markBitmap != nullptr) {
        size_t wordCount = (totalSlotCount + 63) / 64;
        memset(markBitmap, 0, wordCount * sizeof(uint64_t));
    }
    liveSlotCount = 0;
}
```

**仅在全死 Region 回收时才 `ReleaseBitmaps()`（munmap）**。非全死的 Region 保留 bitmap 内存，避免下轮 GC 重新 mmap。

---

## 6. 性能分析与 SIMD 优化潜力

### 6.1 基准性能

对于 sizeClass=4 (64B slot) 的 32KB Region（512 个 slot，8 个 bitmap word）：

| 操作 | 次数 | 每次耗时 | 总计 |
|------|------|----------|------|
| NOT (64-bit) | 8 | ~1 cycle | ~8 cycles |
| popcount | 8 | ~1 cycle (popcnt 指令) | ~8 cycles |
| memset (64B) | 1 | ~2-4 cycles (cache line) | ~4 cycles |
| **总计** | | | **~20 cycles ≈ 7ns @3GHz** |

对比旧版 free-list sweep（512 个 slot 逐个遍历）：~512 × 3-5 cycles ≈ 500-800ns

**加速比：~70-100x**

### 6.2 SIMD 优化潜力

bitmapNOT 循环天然适合 SIMD 向量化：

```cpp
// AVX2 版本（256-bit = 4个 uint64_t = 256 个 slot/次）
#ifdef __AVX2__
void SweepRegionSIMD(uint64_t* alloc, const uint64_t* mark, size_t wordCount) {
    size_t i = 0;
    for (; i + 4 <= wordCount; i += 4) {
        __m256i m = _mm256_loadu_si256((__m256i*)&mark[i]);
        __m256i notM = _mm256_xor_si256(m, _mm256_set1_epi64x(-1LL));  // NOT
        _mm256_storeu_si256((__m256i*)&alloc[i], notM);
    }
    // 处理尾部
    for (; i < wordCount; i++) {
        alloc[i] = ~mark[i];
    }
}
#endif
```

**NEON 版本**（AArch64，对 macOS/ARM 和 OpenHarmony）：
```cpp
#ifdef __ARM_NEON
void SweepRegionNEON(uint64_t* alloc, const uint64_t* mark, size_t wordCount) {
    size_t i = 0;
    for (; i + 2 <= wordCount; i += 2) {
        uint64x2_t m = vld1q_u64(&mark[i]);
        uint64x2_t notM = vmvnq_u64(m);  // bitwise NOT
        vst1q_u64(&alloc[i], notM);
    }
    for (; i < wordCount; i++) {
        alloc[i] = ~mark[i];
    }
}
#endif
```

> **优先级**：SIMD 为 P3 可选优化。初始实现用标量 NOT，性能已远优于 free-list。编译器 `-O2` 通常能自动向量化简单的 NOT 循环。

### 6.3 cache 行为分析

- `markBitmap` 和 `allocBitmap` 通过独立 mmap 分配，物理上可能不连续
- 但每个 bitmap 内部是连续的，顺序访问 → **完美预取**
- 一个 Region 的 bitmap 最大约 256B（16KB slot 时仅 2 slot → 1B），通常在 1-4 个 cache line 内
- sweep 后立即 memset 清零 markBitmap → 此时数据已在 L1 cache → **接近零开销**

---

## 7. 与旧版差异对比

### 7.1 差异矩阵

| 维度 | 旧版 Step4 | 新版 Step4.1 | 依据 |
|------|-----------|-------------|------|
| sweep 方法 | `SweepSmallRegion()` 遍历每个 slot，构建 FreeSlot 链表 | `MSRegionInfo::SweepRegion()` bitmap NOT | Step1.1 §3.11 |
| 死对象处理 | 向死对象空间写入 `FreeSlot::next` 指针 | 不触碰死对象内存 | Step1.1 §3.5.4 |
| 分配时使用 | 遍历 FreeSlot 链表 | CTZ(allocBitmap word) 找 1-bit | Step1.1 §3.5.3 |
| 状态判定 | `MS_UNSWEPT_REGION` 等新 RegionType 枚举值 | `sweepState` 字段 (UNSWEPT/SWEEPING/SWEPT) | Step1.1 §3.4 |
| Region 归属 | 自定义 `AtomicRegionQueue` | CentralRegionPool 四链表 | Step1.2 §3 |
| 代际计数 | `gcEpoch_` +1/GC | `globalSweepGen_` +2/GC | Step1.2 §3.1 |
| 大对象 sweep | 重复实现 `SweepLargeRegion()` | 不实现，引用 Step2.4 | Step2.4 §4 |
| size class 数 | 68 | 36 | Step1.2 §1.2 |

### 7.2 已删除的旧版组件

以下旧版 Step4 中的组件在新版中**完全删除**：

- `FreeSlot` 结构体（向死对象写 next 指针）
- `SweepSmallRegion()` free-list 版本
- `SweepLargeRegion()` 及其 `HandleLargeEmptyRegion()`
- `AtomicRegionQueue` 及其 push/pop/tryPop 方法
- `globalSweepQueue_` / `sweepQueuePerClass_[]` / `largeSweepQueue_` / `pinnedSweepQueue_`
- `MS_UNSWEPT_REGION` / `MS_SWEPT_PARTIAL_REGION` / `MS_SWEPT_FULL_REGION` 等 RegionType 枚举值
- `gcEpoch_` (+1/GC) 代际计数器
- `SweepResult` 独立结构体（改用 freeSlotCount/liveSlotCount 直接判定）
