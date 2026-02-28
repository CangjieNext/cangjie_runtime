# Ghost 路由目标数据损坏问题分析与修复

> 日期：2026-02-24
> 状态：已修复，5/5 测试通过

## 1. 问题现象

运行 `test_los_functional`（LOS 功能测试，配置 `cjHeapSize=64mb cjMainRatio=3 cjReserveRatio=5 cjLOSRatio=2 cjEnableLOSCompact=1`）时，进程收到 SIGSEGV 崩溃：

```
CJNatvie Handle signal: 11.
Thread "" catched unhandled SIGSEGV (Segmentation fault: 11) from managed frame.
signal pc: 0x10072464c, addr: 0x8
```

- **崩溃地址** `addr: 0x8` — 访问偏移 8 处的空指针（对象首字段）
- **崩溃时机** — 约在第 60-70 轮 GC 后，mutator 线程访问一个已被转发（forwarded）的对象时
- **崩溃原因** — 对象的 Ghost 路由目标（TO-space）数据已被清零，mutator 通过 `GetRoute` 得到指向全零内存的地址
- **复现率** — 接近 100%，每次运行稳定崩溃

## 2. 背景知识：Ghost 路由机制

仓颉运行时使用 Copy GC（复制式垃圾回收），核心流程：

```
DoGarbageCollection
  → TraceHeap              (标记存活对象)
  → PostTrace
      → PrepareFromRegionList   (dispel 上一轮 ghost → 准备本轮 FROM 区域)
  → Preforward
  → ForwardFromSpace       (将 FROM 区域的存活对象复制到 TO 区域)
  → ShrinkInactiveZone     (仅 OOM GC: 回收空闲空间)
  → FlipTagID
  → CollectSmallSpace      (FROM → garbageRegionList)
```

### Ghost 生命周期

1. **Cycle N — ForwardFromSpace**：FROM 区域的存活对象被复制到 TO-space TL 区域。`ForwardRegion` 完成后：
   - FROM 区域设置 `RouteState = FORWARDED`
   - FROM 区域的 `routeInfo` 记录 TO-space 地址：`{toRegion1StartAddress, toRegion1UsedBytes, toRegion2Idx}`
   - 调用 `CollectRegion(FROM)` → 放入 `garbageRegionList`
   - `PrepareForwardableRegion` 将元数据复制到 `*0` 字段，设置 `inGhostFromRegion=1`

2. **Cycle N → Cycle N+1 之间**：FROM 区域成为"ghost"。此时 mutator 可能仍持有指向 FROM 区域中旧对象的引用（tag 未翻转），通过 `GetRoute()` 查询 `routeInfo` 重定向到 TO-space。

3. **Cycle N+1 — PrepareFromRegionList**：调用 `DispelGhostFromRegion()` 清除 `inGhostFromRegion` 标志，ghost 生命周期结束。

### RouteInfo 结构（`LiveInfo.h`）

```cpp
struct RouteInfo {
    uintptr_t toRegion1StartAddress;  // TO-space 第一段起始地址
    uint32_t  toRegion1UsedBytes;     // 第一段已用字节数
    uint32_t  toRegion2Idx;           // 第二段 region 的 unit 索引
};
```

**关键约束**：在 ghost 生命周期内，`routeInfo` 及其指向的 TO-space 数据必须保持完整。

## 3. 诊断方法

### 3.1 排除法确认问题源

先用原始运行时（无 LOS 改动）验证：原始运行时通过所有 P1-P4 触发条件 → 确认 bug 由 LOS 改动引入或暴露。

### 3.2 逐步缩小范围

| 阶段 | 诊断手段 | 结论 |
|------|----------|------|
| ForwardObjectExclusive | 在 memcpy 前后验证源/目标数据完整性 | ✅ 复制本身成功 |
| ForwardRegion | 转发完成后检查 TO-space 首个对象 | ✅ 转发后数据完好 |
| ShrinkInactiveZone | 比较释放范围与 TO-space 地址 | ⚠ 释放范围覆盖了 TO-space |

### 3.3 添加的诊断日志

1. **GC 周期计数器**（`g_gcCycleNum`）：在 `WCollector.cpp` 添加 `std::atomic<uint32_t>` 全局计数器
2. **GHOST-VERIFY 校验点**（3 处）：
   - **GC-START**：检查上一轮 ghost 路由目标是否完好
   - **PRE-SHRINK**：ForwardFromSpace 之后、ShrinkInactiveZone 之前
   - **POST-SHRINK**：ShrinkInactiveZone 之后
3. **TakeRegion ClearUnits 日志**：记录每次从 `garbageRegionList` 取出的区域是否为 ghost-from
4. **ShrinkInactiveZone 释放范围扫描**：枚举释放范围内所有区域的类型和 ghost 状态

### 3.4 测试配置

```bash
DYLD_LIBRARY_PATH=.../runtime/lib/darwin_aarch64_cjnative \
MRT_REPORT=1 cjHeapSize=64mb cjMainRatio=3 cjReserveRatio=5 \
cjLOSRatio=2 cjEnableLOSCompact=1 ./test_los_functional > /tmp/gc_diag.log 2>&1
```

## 4. 风险分析 — Ghost-from 区域被 TakeRegion 复用的语义风险

### 4.1 问题描述

`ForwardRegion` 完成后调用 `CollectRegion(fromRegion)`，将 FROM 区域以 `GARBAGE_REGION` 类型放入 `garbageRegionList`。此时该区域的 `inGhostFromRegion=1`，ghost 路由元数据（`routeInfo`、`nextRegionIdx0`）仍需保留到下一轮 GC 的 `PrepareFromRegionList` 才会 dispel。

`TakeRegion` 从 `garbageRegionList` 取到 ghost-from 垃圾区域后执行：
1. `ClearUnits(idx, num)` — 清零**数据区域**（堆内存），不影响 UnitMetadata
2. `InitRegion(idx, num, type)` → `InitRegionInfo` — 重置**活跃字段**（`allocPtr`、`regionEnd`、`nextRegionIdx`、`liveInfo`、`unitRole` 等）

**关键发现**：`InitRegionInfo` **不会**重置 ghost 备份字段（`routeInfo`、`regionEnd0`、`liveInfo0`、`nextRegionIdx0`、`unitRole0`），也**不会**清除 `inGhostFromRegion` 标志和 `routeState`。因此 TakeRegion 复用 ghost-from 区域**不会直接破坏路由元数据**。

但这会引入**语义混乱风险**：

### 4.2 残留标志导致的风险

TakeRegion 复用后，该区域同时存在：
- `inGhostFromRegion = 1`（残留）
- `routeState = ROUTED`（残留，非 NORMAL）
- 全新的 `unitRole`（如 `SMALL_SIZED_UNITS`），正在被 mutator 分配新对象

此时若任何代码对该区域中的地址调用 `GetGhostFromRegionAt`，会返回非空的 ghost region（因为 `inGhostFromRegion=1` 且 `routeState != NORMAL`），进而用陈旧的 `routeInfo + liveInfo0` 计算出错误的路由地址。

在正常情况下，这被以下时序保护：
- STW 阶段的 `PrepareFromRegionList` → `DispelGhostFromRegion` 会在下一轮 GC 开始时清除所有 ghost 标志
- Mutator 屏障（`TraceBarrier`/`EnumBarrier`）仅对 tagged pointer 调用 `FindToVersion`，而新分配对象的引用未被 tag

**但存在边界风险**：若 ghost-from region 是多 unit 的（如 2 units），被 TakeRegion 以不同大小拆分复用时，`DispelGhostFromRegion` 用 `regionEnd0` 计算 nUnit 可能遗漏尾部 unit 的 `inGhostFromRegion` 标志清除。

### 4.3 诊断证据

```
[cycle=1] TakeRegion: ClearUnits garbage [0x1097dc000, 0x1097ec000) (4 units) ghostFrom=1 routeState=5
[cycle=1] TakeRegion: ClearUnits garbage [0x10989c000, 0x1098ac000) (4 units) ghostFrom=1 routeState=5
[cycle=1] TakeRegion: ClearUnits garbage [0x109a1c000, 0x109a2c000) (4 units) ghostFrom=1 routeState=5
```

多个 `ghostFrom=1 routeState=5 (FORWARDED)` 的区域被 TakeRegion 从 garbageRegionList 取出。虽然 ghost 备份字段（routeInfo 等）未被 InitRegionInfo 直接破坏，但 `inGhostFromRegion` 残留在被复用的区域上。

### 4.4 结论

**不是直接导致 crash 的根因**，但属于需要防御的语义风险。LOS 分区导致 MAIN 空间缩小，大幅增加了 ghost-from 区域被复用的频率，放大了此风险。Fix A 作为**防御性保护**消除此类风险。

## 5. 根因 #2 — ShrinkInactiveZone Phase B 释放 Ghost 路由目标

### 5.1 问题描述

`ShrinkInactiveZone` 是我们在 P3 修复中新增的代码，用于在 OOM GC 时回收 MAIN 尾部空闲空间和归还 RESERVE 借用空间。

Phase B 处理 RESERVE 借用空间 `[mainEnd, mainWatermark)` 的回收：
1. 扫描 `[mainEnd, min(oldInactive, mainWm))` 寻找最高存活区域 `highestLiveBorrowedEnd`
2. 将 watermark 推回到 `highestLiveBorrowedEnd`
3. 调用 `ReleaseUnits` 释放 `[newWatermark, oldMainWm)` 的物理页面

**问题**：Ghost 路由的 `routeInfo.toRegion1StartAddress` 可能指向 RESERVE 借用空间中的 TO-space TL 区域。Phase B 仅通过物理扫描判断"存活"（`IsValidRegion && !IsFreeRegion && !IsGarbageRegion`），但 TO-space 区域在转发完成后已是 `RECENT_FULL_REGION`，可能不在扫描到的范围内（已被 `EnlistFullThreadLocalRegion` 移到 `recentFullRegionList`）。`highestLiveBorrowedEnd` 被低估，导致 ghost 路由目标所在页面被释放。

macOS 上 `ReleaseUnits` 实现为 `mmap(MAP_FIXED)` 清零后重映射，数据直接归零。

### 5.2 触发条件

- OOM GC（`gcReason == GC_REASON_OOM`），触发 `ShrinkInactiveZone`
- MAIN 空间不足，`ForwardFromSpace` 通过 `BorrowForMain` 将 TO-space 分配到 RESERVE 借用区
- Ghost 路由目标地址超过 `mainEnd`，位于借用空间
- Phase B 的 `highestLiveBorrowedEnd` 计算未考虑 ghost 路由目标

### 5.3 诊断证据

Cycle 64 POST-SHRINK（ShrinkInactiveZone 之后）出现清零目标：
```
[cycle=64] GHOST-VERIFY-POST-SHRINK: ZEROED target! ghost=0x104ca9da8
  target=[0x105ffca38+72)    ← 地址超过 mainEnd=0x105ff8000
  to1UsedBytes=72            ← 非零 = 真实转发数据被清零
  targetType=1 targetIsFree=0 targetIsGarbage=0
```

同一 ghost 在 PRE-SHRINK 阶段**未报告**清零，确认是 ShrinkInactiveZone 执行期间释放了页面。

Phase A 扫描也发现 ghost-from 垃圾区域在释放范围内：
```
[cycle=63] ShrinkInactiveZone: region type=14 ghostFrom=1 routeState=5 IN RELEASE RANGE
```

### 5.4 是否为 LOS 引入

**是，100% 由我们引入**。`ShrinkInactiveZone` 是 P3 修复中新增的功能，RESERVE 借用机制也是 LOS 分区后的新增架构。原始运行时无此代码路径。

## 6. 修复方案

### 6.1 Fix A — pendingGhostGarbageList（防御性保护）

**原理**：让 ghost-from 垃圾区域绕过 `garbageRegionList`，避免被 `TakeRegion` 复用后残留 `inGhostFromRegion` / `routeState` 标志导致语义混乱。等 ghost 在下一轮 GC dispel 之后，再将它们归还。

**性质**：防御性修复。虽然不是直接导致 crash 的根因（crash 由 Fix B 修复），但消除了 ghost-from 区域被过早复用带来的一整类边界条件风险（如 region 拆分后 ghost 标志残留、陈旧 routeInfo 被误读等），保证 ghost 生命周期内的语义一致性。

**涉及文件**：`src/Heap/Allocator/RegionManager.h`

**代码改动**：

| 位置 | 改动 |
|------|------|
| 成员变量声明 | 新增 `RegionList pendingGhostGarbageList;`（在 `garbageRegionList` 之后） |
| 构造函数 | 初始化列表添加 `pendingGhostGarbageList("pending ghost garbage")` |
| `CollectRegion()` | 如果 `region->IsGhostFromRegion()`，放入 `pendingGhostGarbageList`；否则照旧放入 `garbageRegionList` |
| `PrepareFromRegionList()` | dispel ghost 之后，执行 `garbageRegionList.MergeRegionList(pendingGhostGarbageList, GARBAGE_REGION)` |

### 6.2 Fix B — Ghost 路由目标保护（修复根因）

**原理**：`ShrinkInactiveZone` 计算释放边界时，遍历所有 ghost 路由目标，将其 region 的结束地址纳入 `highestLiveEnd`（MAIN 区）或 `highestLiveBorrowedEnd`（RESERVE 借用区），确保释放范围不覆盖任何 ghost 路由目标。

**涉及文件**：`src/Heap/Allocator/RegionManager.cpp`

**代码改动**：

| 位置 | 改动 |
|------|------|
| Phase A/B 扫描之后、`newInactive` 计算之前 | 新增 ghost 路由目标扫描逻辑 |
| 扫描 `ghostFromRegionList` | 读取每个 ghost 的 `routeInfo.toRegion1StartAddress` → 找到目标 region → 取 `regionEnd` → 提升对应的 `highestLiveEnd` 或 `highestLiveBorrowedEnd` |
| 扫描 `pendingGhostGarbageList` | 同上逻辑 |
| `toRegion2Idx` 处理 | 如果 `toRegion2Idx != INVALID_VALUE`，同理保护第二跳路由目标 |
| Phase A/B 垃圾清理 | 同步清理 `pendingGhostGarbageList` 中超出回收边界的条目 |
| `borrowedShrunk` | 在 ghost 保护调整后重新计算 |

## 7. 验证结果

### 修复前

```
EXIT: 139  (SIGSEGV, 稳定复现)
```

### 修复后

```bash
for i in 1 2 3 4 5; do
  DYLD_LIBRARY_PATH=.../runtime/lib/darwin_aarch64_cjnative \
  MRT_REPORT=1 cjHeapSize=64mb cjMainRatio=3 cjReserveRatio=5 \
  cjLOSRatio=2 cjEnableLOSCompact=1 ./test_los_functional > /dev/null 2>&1
  echo "EXIT: $?"
done
```

```
=== Run 1 === EXIT: 0
=== Run 2 === EXIT: 0
=== Run 3 === EXIT: 0
=== Run 4 === EXIT: 0
=== Run 5 === EXIT: 0
```

**Fix A 验证**：修复后日志中 `TakeRegion.*ghostFrom=1` 匹配数为 0（Fix A 前为多次命中）。

**Fix B 验证**：修复后日志中 `GHOST-VERIFY-POST-SHRINK.*ZEROED` 匹配数为 0（Fix B 前 cycle 64 报告清零目标）。

## 8. 涉及文件清单

| 文件 | 改动类型 | 说明 |
|------|----------|------|
| `src/Heap/Allocator/RegionManager.h` | Fix A（防御性） | 新增 `pendingGhostGarbageList` 成员；修改 `CollectRegion`、`PrepareFromRegionList` |
| `src/Heap/Allocator/RegionManager.cpp` | Fix B（根因修复） | `ShrinkInactiveZone` 增加 ghost 路由目标保护扫描；Phase A/B 垃圾清理增加 `pendingGhostGarbageList` 处理 |
| `src/Heap/WCollector/WCollector.cpp` | 诊断（已清理） | GC 周期计数器 `g_gcCycleNum`（保留）；GHOST-VERIFY 校验点（已移除）；crash 诊断 LOG（保留） |
