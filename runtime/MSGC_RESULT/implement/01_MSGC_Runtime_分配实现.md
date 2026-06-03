# MSGC Runtime 分配实现详解

## 概述

MSGC（Mark-Sweep GC）的对象分配分为两个层次：
1. **外层接口**：`CJ_MCC_NewObject` / `MCC_NewObject`（runtime call，从汇编 stub 进入）
2. **内层核心**：`AllocBufferMS::Allocate` → `FastAlloc` + `SlowAlloc`（per-thread 分配缓存）

---

## 一、AllocBufferMS：MSGC per-thread 分配缓存

### 设计对标

| 概念 | Go runtime | MSGC |
|------|-----------|------|
| per-P 分配缓存 | `mcache`（每 P 一个，持有每 size-class 的 span） | `AllocBufferMS`（每线程/Processor 一个，持有每 size-class 的 Region） |
| 分配单元 | `mspan`（大小固定的对象池） | `MSRegionInfo`（32KB Region，slot 由 bitmap 管理） |
| 全局补货池 | `mcentral` | `CentralRegionPool` |

### 字段布局（关键字段）

```cpp
class AllocBufferMS {
    // offset 0: 必须与 AllocBuffer::tlRegion 相同（LLVM fast-path 兼容）
    RegionInfo* tlRegion_;                          // 永远 = NullRegion()（见下文）

    MSRegionInfo* activeRegions_[kNumSizeClasses];  // 每 size-class 的活跃 Region
    std::atomic<MSRegionInfo*> cachedRegion_[kNumSizeClasses][4]; // 备货缓存（最多 4 个）

    std::atomic<uint8_t> cacheMiss_[kNumSizeClasses];  // 缺货压力计数
    std::atomic<uint8_t> cacheHeat_[kNumSizeClasses];  // 热点分类热度

    CentralRegionPool* central_;  // 全局 Region 池
    std::vector<BaseObject*> stackRoots_;  // GC 根暂存
    uint64_t gcAssistDebtScaled_;           // Mutator assist 债务
    PerPGCWork gcWork_;                     // per-P GC 工作队列（Step5）
};
```

**关键约束**：`tlRegion_` 必须在 offset 0，且永远为 `NullRegion()`（allocPtr=0, regionEnd=0），以确保 CopyGC 风格的 LLVM inline fast path 总是 miss 并回落到 slow path（见第三章）。

---

## 二、Size Class 设计

36 个 size class，覆盖 8B ～ 16KB：

| 范围 | 步进 | 示例 class |
|-----|------|-----------|
| 8B ～ 128B | 8B ～ 16B | class 0（8B）～ class 9（128B） |
| 160B ～ 512B | 32B ～ 64B | class 10（160B）～ class 17（512B） |
| 640B ～ 2048B | 128B ～ 256B | class 18（640B）～ class 25（2048B） |
| 2560B ～ 8192B | 512B ～ 2048B | class 26（2560B）～ class 32（8192B） |
| 10240B ～ 16384B | — | class 33 ～ 35 |

**大对象阈值**：> 16384B（= 32KB Region / 2）走 Large Object 路径。

**SizeToClass 查找**：O(1)
- size ≤ 1024：直接表 `smallLookup_[ceil(size/8)]`（129 项）
- 1024 < size ≤ 16384：直接表 `largeLookup_[(size-1024+255)/256]`（≈61 项）

---

## 三、分配主流程

### 入口：`AllocBufferMS::Allocate(size, local)`

```
Allocate(size)
  │
  ├─ SizeToClass(size)
  │     ├─ size > 16384 → AllocLarge (Large Object 路径)
  │     └─ else → cls = SizeToClass(size)
  │
  ├─ FastAlloc(cls)
  │     ├─ activeRegions_[cls] != nullptr
  │     │     ├─ regionType == MS_REGION_BUMP → AllocBump()
  │     │     │     ├─ hit: 返回地址
  │     │     │     └─ miss → SlowAlloc(cls)
  │     │     ├─ regionType == MS_REGION_PARTIAL → AllocFromBitmapFast()
  │     │     │     ├─ hit: 返回地址（bitmap CTZ）
  │     │     │     └─ miss → SlowAlloc(cls)
  │     │     └─ hit: 直接返回
  │     └─ activeRegions_[cls] == nullptr → SlowAlloc(cls)
  │
  └─ return addr
```

### AllocBump（新 Region 的 bump pointer）

新分配的空 Region 使用 bump pointer 直接推进 `allocPtr`，直到满：
```
allocPtr += slotSize
if allocPtr <= regionEnd: 返回旧 allocPtr
else: 切换到 bitmap 模式 or 换 Region
```

### AllocFromBitmapFast（部分已回收 Region）

已被 GC Sweep 部分回收的 Region 用 bitmap 管理空闲 slot：
```
word = reservedBitmap[word_idx]  // 预载 free-slot summary
if word == 0: miss
bit = CTZ(RBIT(word))            // 最低位空闲 slot
addr = regionBase + bit * slotSize
reservedBitmap[word_idx] &= ~(1 << bit)  // 标记已分配
freeSlotCount--
return addr
```

### SlowAlloc（补货）

```
SlowAlloc(cls)
  │
  ├─ 1. Return 当前 activeRegion 到 CentralRegionPool
  ├─ 2. TakeCachedRegion(cls)  → 命中 thread-local 备货缓存
  │     命中: 直接使用新 Region
  │     miss → goto 3
  ├─ 3. RefillFromCentral(cls) → 从 CentralRegionPool 取 Region
  │     命中: 使用新 Region
  │     miss: GC assist / OOM
  ├─ 4. (可选) NotifyBackgroundFillIfNeeded(cls) → 通知 gc-helper 线程预热备货
  └─ 5. 在新 Region 上重试 Alloc
```

---

## 四、MSRegionInfo：分配 Region 结构

每个 Region 32KB，被 `MSRegionInfo` 元数据管理：

```
MSRegionInfo {
    uintptr_t   allocPtr;      // bump pointer
    uintptr_t   regionEnd;     // Region 末尾地址
    MSRegionType regionType;   // BUMP / PARTIAL / LARGE / FREE / EMPTY_CACHE
    uint16_t    freeSlotCount; // 空闲 slot 计数
    uint64_t*   allocBitmap;   // 已分配 bitmap（1=已分配）
    uint64_t*   reservedBitmap;// 可分配 bitmap（1=可分配，扫描用）
    ...
}
```

**Region 状态机**：
```
FREE → BUMP（新分配，bump pointer 模式）
BUMP → FULL（bump 满）
FULL → PARTIAL（GC Sweep 回收部分 slot 后）
PARTIAL → FULL（bitmap 分配耗尽）
FULL/PARTIAL → FREE（整 Region 全回收）
```

---

## 五、TLS 映射与兼容层

```
ThreadLocalData（TLS，x28 寄存器指向）
  +0x00: AllocBuffer*    buffer     ← CopyGC TLAB 指针 / MSGC 下存 msBuffer（强转）
  +0x08: Mutator*        mutator
  ...
  +0x4e: uint8_t         encodedGCPhase ← MSGC fast path 读取
  ...
  +0x58: AllocBufferMS*  msAllocBuffer  ← MSGC fast path 读取
```

MSGC 运行时初始化时（`EnsureThreadLocalBuffer`）：
```cpp
AllocBufferMS* msBuffer = AllocBufferMS::GetOrCreateAllocBufferMS();
// 把 msBuffer 强转存入 CopyGC 的 buffer 字段
// 使得 CopyGC 风格的 fast path 读 [buffer+0] = tlRegion_ = NullRegion()，
// 永远 miss，安全回落到 slow path
if (ThreadLocal::GetAllocBuffer() == nullptr) {
    ThreadLocal::SetAllocBuffer(reinterpret_cast<AllocBuffer*>(msBuffer));
}
```

---

## 六、大对象分配

size > 16384B 不走 AllocBufferMS，直接调用：
```
HeapManager::Allocate(size, AllocType::NORMAL_OBJECT)
  → RegionSpaceMS::AllocLarge(size)
    → CentralRegionPool::AllocLargeRegion(size)
      → 分配若干连续 32KB Region，返回首地址
```

---

## 七、关键路径性能

| 路径 | 约估指令数 | 适用场景 |
|------|-----------|---------|
| FastAlloc(BUMP) | ~5 条 | 新 Region，批量分配 |
| FastAlloc(bitmap CTZ) | ~15 条 | 部分已回收 Region |
| SlowAlloc(cached hit) | ~30 条 | 备货缓存命中 |
| SlowAlloc(central) | 数百条（锁竞争） | 缓存全空 |
