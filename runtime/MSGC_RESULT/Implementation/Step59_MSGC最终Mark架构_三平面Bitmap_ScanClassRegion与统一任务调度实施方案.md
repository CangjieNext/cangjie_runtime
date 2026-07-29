# Step59：MSGC 最终 Mark 架构——三平面 Bitmap、ScanClass Region 与统一任务调度实施方案

> **For agentic workers:** 按本文任务顺序实施；每个任务完成对应测试后再进入下一项，不引入兼容布局、过渡协议或临时数据结构。

**Goal:** 将 MSGC 的小对象发现、标记和扫描改造成以 Region 元数据为入口、以三平面 Bitmap 为状态、以 ScanClass 隔离叶子对象、以统一工作纪元收敛的最终架构，消除大规模 `[]byte` 持久对象在 Mark 阶段反复触碰对象头、TypeInfo 和分散元数据的主要成本。

**Architecture:** 小对象 Region 从分配时即固定为 `NO_REF` 或 `HAS_REF`；每个 Region 使用独立的 alloc/mark/scan 三平面 Bitmap；`NO_REF` 只完成 occupancy 校验和 mark bit 提交，`HAS_REF` 由 RegionScanQueue 批量扫描，超大引用数组由 RangeQueue 分片；Object、Region、Range 三类工作共享一个发布、计数和终止协议。

**Tech Stack:** C++14、C99、CMake、Clang/LLVM AArch64/X86 后端、仓颉 Runtime Mark-Sweep GC、GoogleTest/契约脚本、case26 HTTP complex JSON benchmark。

---

## 1. 最终态约束

本文只描述一套最终结构。Step58 中的连续/striped Bitmap 选择、按条件启用
Region affinity、旧布局兼容等候选项到此终止，不进入本方案。

最终结构遵守以下约束：

1. `ScanClass` 是分配属性，不是 Mark 时从对象头临时推断的属性。一个小对象 Region
   的全部 slot 只能属于同一个 `ScanClass`。
2. `alloc`、`mark`、`scan` 三个 Bitmap 平面均按 `regionIdx` 直接寻址；热路径不从
   `MSRegionInfo` 追逐 Bitmap 指针。
3. `NO_REF` slot 的发现路径不得读取对象 `stateWord`、`TypeInfo` 或 payload，也不得
   进入任何扫描队列。
4. `HAS_REF` slot 的 mark winner 不直接作为独立对象在全局队列中来回搬运；winner
   只更新 mark plane 并使所属 Region 可扫描。
5. 引用数组 payload 超过 128 KiB 时只进入 `RangeQueue`，不得由一个 Worker 独占
   完整数组。
6. 所有工作发布都参与同一个 `workEpoch` 和 in-flight 计数协议；终止判定不能只看
   `WorkBuf`。
7. 普通 GC 只扫描 young Region 和 remembered set；major GC 才扫描 old/immutable
   Region。带 WeakRef、Finalizer 或不稳定布局的 Region 不进入 immutable 集。
8. SATB、alloc-black、WeakRef、Finalizer resurrection、stale edge 防护以及 Sweep
   generation 语义保持成立。
9. Runtime 与 LLVM 只允许使用本文定义的 ABI v2；不存在 v1/v2 运行时分支，也不
   根据环境变量选择旧布局。
10. CopyGC 的数据结构、编译路径和行为不变。

### 1.1 目标热路径

最终的小对象引用发现路径如下：

```text
ref
  -> heap range gate
  -> regionIdx = (ref - heapStart) >> 15
  -> MSRegionInfo[regionIdx]
  -> Region type + generation + slot alignment
  -> allocPlane[regionIdx][slot] occupancy
  -> atomic OR markPlane[regionIdx][slot]
     -> duplicate: return
     -> NO_REF: return
     -> HAS_REF: set Region DIRTY and publish Region token
```

这里的对象本体只在 `HAS_REF` Region 的扫描消费者中读取；`NO_REF` 的 413.7 万个
叶子对象只产生紧凑元数据访问和 mark bit 更新，不再产生 413.7 万次 TypeInfo
随机读取。

### 1.2 正确性不变量

| 不变量 | 保证方式 |
|---|---|
| mark bit 只能对应已分配 slot | 先以 acquire 读取稳定 alloc plane，再提交 mark bit |
| 同一对象的扫描职责最多展开一次 | `scan = scan OR toScan`，`toScan = mark & ~scan` |
| 扫描期间新增 mark 不丢失 | producer 设置 mark 后设置 `DIRTY`；owner 释放前再次读取 dirty 和差集 |
| Region token 不丢失、不无限重复 | `QUEUED/OWNED/DIRTY` 原子状态机只允许一个 owner |
| Mark 期间新分配对象不被本轮 Sweep 回收 | 新 Region 记录当前 `allocEpoch`，对象 alloc-black |
| stale edge 不可把空 slot 标活 | Region generation 在解析期间保持不变，且地址对齐和 occupancy 均在 mark RMW 前验证 |
| 普通 GC 不丢失 old→young 引用 | 写屏障在 store 前后记录对应 old card，MarkSetup 扫描 remembered cards |
| 终止后不存在隐藏工作 | 三类队列为空、所有 local builder 已发布、in-flight 为零、无 dirty Region，且两次读取 `publishSeq` 相同 |

## 2. 最终数据布局

### 2.1 ScanClass

```cpp
enum class MSScanClass : uint8_t {
    NO_REF = 0,
    HAS_REF = 1,
};
```

`ScanClassOf(typeInfo)` 是唯一分类函数：

- 普通对象返回 `typeInfo->HasRefField()`；
- 引用数组以及含引用的 struct 数组返回 `HAS_REF`；
- `Array<UInt8>`、`Array<UInt16>` 等纯值 payload 返回 `NO_REF`；
- WeakRef、Finalizer 和运行时特殊对象返回 `HAS_REF`，由扫描消费者进入专用处理。

分配入口必须持有 `TypeInfo*`。只接收 `size` 的 MSGC 小对象分配接口从最终 API 中
删除，防止分类在调用链中丢失。

### 2.2 MSRegionInfo v2

`MSRegionInfo` 保持 64B、单 cache line，并改为不保存 Bitmap 指针：

| 偏移 | 字段 | 大小 | 最终语义 |
|---:|---|---:|---|
| +0 | `allocPtr` | 8 | BUMP 分配指针；编译器 fast path 使用 |
| +8 | `regionEnd` | 8 | Region 末地址 |
| +16 | `nextRegionIdx` | 4 | 中央链表 next |
| +20 | `prevRegionIdx` | 4 | 中央链表 prev |
| +24 | `totalSlotCount` | 2 | 小对象 slot 数 |
| +26 | `freeSlotCount` | 2 | 空闲 slot 数 |
| +28 | `divMagic` | 4 | 地址到 slot 的除法常量 |
| +32 | `allocEpoch` | 4 | Region 首次对本轮 mutator 可见的分配纪元 |
| +36 | `sweepGen` | 4 | lazy/concurrent Sweep generation |
| +40 | `summaryOrLargeSize` | 8 | 小对象 alloc summary；LARGE 为逻辑对象字节数 |
| +48 | `aux` | 8 | LARGE base/span；小对象为 age/live-ratio 统计 |
| +56 | `control` | 4 | 原子 `QUEUED/OWNED/DIRTY/LARGE_MARKED/OLD/IMMUTABLE/NEEDS_CLEANUP` |
| +60 | `regionGeneration` | 2 | Region 重用代数，用于拒绝 stale token |
| +62 | `sizeClass` | 1 | 0..35；LARGE 使用 `0xff` |
| +63 | `layout` | 1 | `ScanClass`、RegionType、SweepState 的打包字段 |

`layout` 的位分配固定为：

```text
bit 0      scanClass
bit 1..4   regionType
bit 5..6   sweepState
bit 7      reserved，恒为 0
```

所有字段偏移由 Runtime `static_assert` 和 LLVM lit 测试同时锁定。Region
重置时先使 lifecycle 不可分配，清三平面有效区，再递增 `regionGeneration`，最后
以 release 发布新 `layout`；发现路径以 acquire 读取 `layout`。

### 2.3 三平面 Bitmap

每个 32 KiB Region 的最大 slot 数为 4096，每个平面固定 4096 bit，即 512B：

```text
allocWord = allocPlaneBase + regionIdx * 512 + logicalWord * 8
markWord  = markPlaneBase  + regionIdx * 512 + logicalWord * 8
scanWord  = scanPlaneBase  + regionIdx * 512 + logicalWord * 8
```

三个平面分别整段虚拟地址预留，按 256 个 Region 的 chunk 延迟 commit。物理内存
规则固定为：

- alloc plane：Region 进入可分配状态前 commit；
- mark plane：Region 进入可分配状态前 commit；
- scan plane：仅 `HAS_REF` Region 进入可分配状态前 commit；
- Region 归还虚拟堆时对三个 plane 的对应 chunk 做引用计数，引用计数归零后
  `madvise`/decommit；
- tail bits 始终为 0，所有统计、差集和 Sweep 都只访问
  `ceil(totalSlotCount / 64)` 个 word。

逻辑预留开销为每 Region 1536B，即 Region 大小的 4.6875%；`NO_REF` Region 不
commit scan plane，因此其常驻 Bitmap 物理开销为 3.125%。

### 2.4 三个平面的位语义

| 平面 | bit=0 | bit=1 | 写入者 |
|---|---|---|---|
| alloc | slot 未分配/已释放 | slot 已分配且属于稳定 occupancy | 分配器、Sweep |
| mark | 本轮未发现 | 本轮已发现或 alloc-black | Mark producer、MarkSetup |
| scan | 扫描职责未认领 | 字段已扫描，或已展开为 durable Object/Range 任务 | Region scan owner |

这一定义把现有 `allocBitmap: 1=free` 改成 `1=allocated`。分配 fast path 通过
`summaryOrLargeSize` 找到零 bit，成功占用 slot 后以 release OR 写入 alloc bit；
Sweep 使用 `dead = alloc & ~mark` 回收对象，最终写回 `alloc &= mark`。这样发现
路径只需一个 occupancy bit，无需解释反向 free 语义。

### 2.5 AllocBufferMS 最终前缀

每个 size class 按 `NO_REF/HAS_REF` 拆成两个活跃 Region：

```cpp
RegionInfo* tlRegion_;                         // +0
MSRegionInfo* activeRegions_[36 * 2];          // +8
uint8_t abiReserved_[1024 - 8 - 72 * 8];       // +584
WorkBuf* activeLog_;                           // +1024
```

索引固定为：

```text
activeIndex = sizeClass * 2 + uint8_t(scanClass)
```

`activeLog_` 只承载需要主线程专用处理的 WeakRef/Finalizer 以及无法形成 Region
token 的 LARGE 对象；普通 `HAS_REF` 小对象由 RegionScanQueue 承载。所有 Runtime
和 LLVM 常量直接切换到 `activeLog_ +1024`。

### 2.6 MSGCFastPathState v2

编译器可见全局状态 `CJ_MRT_MSGCFastPathState_v2` 固定为 64B：

| 偏移 | 字段 | 大小 |
|---:|---|---:|
| +0 | `abiVersion = 2` | 4 |
| +4 | `barrierPhase` | 1 |
| +5 | `regionShift = 15` | 1 |
| +6 | `scanClassCount = 2` | 1 |
| +7 | reserved | 1 |
| +8 | `heapReservedStart` | 8 |
| +16 | `heapReservedEnd` | 8 |
| +24 | `regionInfoArray` | 8 |
| +32 | `allocPlaneBase` | 8 |
| +40 | `markPlaneBase` | 8 |
| +48 | `scanPlaneBase` | 8 |
| +56 | `layoutSignature` | 8 |

`layoutSignature` 固定编码 RegionInfo size、plane stride、Region shift、
activeLog offset 和 size-class 数。Runtime 初始化不满足签名时直接
`RTLOG_FATAL`。v2 Runtime 只导出带 `_v2` 后缀的符号，使旧 binary 在加载期明确
失败；编译器生成代码不执行双版本探测。

## 3. 分配、封存与对象发现协议

### 3.1 分配入口

最终 MSGC 分配 API 为：

```cpp
MAddress AllocBufferMS::Allocate(const TypeInfo* typeInfo, size_t size);
MAddress AllocBufferMS::AllocateSmall(
    const TypeInfo* typeInfo, uint8_t sizeClass, MSScanClass scanClass);
MAddress AllocBufferMS::AllocateLarge(const TypeInfo* typeInfo, size_t size);
```

`MCC_NewObject`、全部 `MCC_NewArray*`、泛型分配和 Runtime 内部分配入口均把
`TypeInfo*` 传到底层。调用链只在入口计算一次 `scanClass`，并用
`sizeClass * 2 + scanClass` 选择 `activeRegions_`。

小对象成功分配的发布顺序固定为：

1. allocator 独占 Region 和 slot；
2. 清理该 slot 上一代残留的 mark/scan bit；
3. 写入对象 TypeInfo 和初始化头；
4. release OR 对应 alloc bit；
5. 并发 Mark 期间再 release OR mark bit，形成 alloc-black；
6. 返回对象地址给 mutator。

步骤 4 之前，GC 不得把 slot 当成对象；步骤 5 之后，新对象不会被本轮 Sweep
回收。`HAS_REF` 新对象的后续字段写入继续经过写屏障，不能依赖初始零值长期保持。

### 3.2 Region 获取和生命周期

Central pool 的小对象桶固定为 `[36][2]`，每个桶只存相同 size class 和
scan class 的 Region。Region 只允许以下生命周期：

```text
FREE
  -> BUMP(sizeClass, scanClass, allocEpoch, generation)
  -> FULL
  -> UNSWEPT
  -> PARTIAL
  -> BITMAP
  -> FULL/PARTIAL
  -> FREE
```

Region 在 `FREE -> BUMP` 时一次性写入分类，直到重新进入 `FREE` 都不得改变
`scanClass`。空 Region cache 也按 `[sizeClass][scanClass]` 保存，禁止把
`NO_REF` 空壳直接改造成 `HAS_REF`。

### 3.3 MarkSetup 封存

MarkSetup 的 STW 窗口执行以下固定序列：

1. 停止并等待所有 Sweep worker；
2. flush 全部 AllocBuffer 的 Region、activeLog 和 local work builder；
3. 将本轮开始前存在的活跃 Region 标记为 `sealedEpoch = oldEpoch`；
4. 对 BUMP Region 以 `allocPtr` 一次性补齐稳定 alloc plane；
5. 清 oldEpoch Region 的 mark plane；`HAS_REF` 同时清 scan plane；
6. 递增全局 `allocEpoch` 和 `workEpoch`，随后恢复 mutator；
7. 恢复后只从 newEpoch Region 分配，所有新对象执行 alloc-black。

因此，本轮快照内的 `NO_REF` Region 不再发生分配，Mark 可以只读取稳定
occupancy。Mark 期间创建的 newEpoch Region 不进入本轮待扫描集合，本轮 Sweep
也按 `allocEpoch` 跳过它们。

### 3.4 小对象发现

`ResolveSmallMarkTarget(ref)` 只返回：

```cpp
struct ResolvedSmallMarkTarget {
    MSRegionInfo* info;
    uint32_t regionIdx;
    uint16_t generation;
    uint16_t slotIdx;
    uint8_t wordIdx;
    uint64_t bitMask;
    MSScanClass scanClass;
};
```

解析顺序严格固定为：

1. heap range gate；
2. 直接计算 `regionIdx` 和 Region 内 offset；
3. acquire 读取 `layout` 和 `regionGeneration`；
4. 拒绝 FREE、EMPTY、SPAN 以及错误的 Region 代数；
5. 用 `sizeClass/divMagic` 计算 slot，验证边界和对齐；
6. acquire 读取 alloc bit，bit 为 0 时拒绝；
7. relaxed probe mark bit；
8. 对白 slot 执行 atomic `fetch_or(markWord, bitMask)`。

winner 根据 Region 分类处理：

- `NO_REF`：递增本地 `leafMarked`，立即返回；
- `HAS_REF`：调用 `PublishRegionDirty(regionIdx, generation)`，不读取对象；
- LARGE：进入第 5 节定义的 large/Range 路径。

`NO_REF` 路径不执行 `obj->GetTypeInfo()`。stale edge 合法性由
解析前后相同的 `regionGeneration + slot alignment + alloc bit` 完成。原始指针
不携带对象代数，因此地址已经被新对象合法复用时按当前对象保守处理；Region 分类
保证该 slot 无需扫描。

### 3.5 Sweep

Sweep 对 oldEpoch 小对象 Region 按 word 执行：

```text
live = alloc & mark
dead = alloc & ~mark
alloc = live
mark = 0
scan = 0                  // 仅 HAS_REF
```

`dead` 中的每个 bit 才进入析构/清零/空闲计数路径。`NO_REF` 且无析构语义的
Region 使用 word popcount 更新计数并批量清页，不逐 slot 读取 TypeInfo。对象是否
需要析构由 `control.NEEDS_CLEANUP`（bit 6）记录；带析构对象固定进入 `HAS_REF`
类和逐对象清理路径。

## 4. HAS_REF Region 扫描协议

### 4.1 RegionScanToken

队列元素固定为 8B：

```cpp
struct RegionScanToken {
    uint32_t regionIdx;
    uint16_t regionGeneration;
    uint16_t workEpoch;
};
```

token 不保存 `MSRegionInfo*`，消费者从连续 RegionInfo array 重新计算地址并校验
generation。Region 重用后，旧 token 会被安全丢弃。`workEpoch` 的 16 位回绕是安全
的，因为一个 epoch 结束前 WorkLedger 必须全零，队列和所有 local deque 必须为空，
不存在跨越 65,536 轮仍存活的任务。

### 4.2 control 状态

`MSRegionInfo::control` 的低位定义为：

```text
bit 0  QUEUED
bit 1  OWNED
bit 2  DIRTY
bit 3  LARGE_MARKED
bit 4  OLD
bit 5  IMMUTABLE
bit 6  NEEDS_CLEANUP
```

`QUEUED` 和 `OWNED` 互斥。producer 赢得任意 `HAS_REF` slot 的 mark bit 后：

1. `fetch_or(DIRTY, release)`；
2. 观察到既无 `QUEUED` 又无 `OWNED` 时，以 CAS 设置 `QUEUED`；
3. CAS 成功者把 token 推入当前 epoch 的 RegionScanQueue，并递增 published
   work；
4. 观察到 `QUEUED` 或 `OWNED` 时结束，现有 token/owner 负责吸收 dirty。

所有 CAS 只修改低 3 位扫描状态，并保留 `LARGE_MARKED/OLD/IMMUTABLE/
NEEDS_CLEANUP`；lifecycle 代码也不得用整字写覆盖扫描状态。

### 4.3 owner 扫描循环

worker 弹出 token 后，以 CAS 将 `QUEUED` 改为 `OWNED`。成功 owner 重复：

```text
atomic clear DIRTY
for each valid bitmap word:
    toScan = alloc & mark & ~scan
    for each set bit in toScan:
        scan one object/range
        release OR this bit into scan
acquire load control
DIRTY set      -> repeat
DIRTY clear    -> CAS OWNED to 0
CAS failed     -> repeat
```

scan bit 必须在对象字段已经扫描完成，或对应 Object/Range durable task 已全部
发布后设置。一个对象扫描失败或转入 WeakRef/Finalizer 专用队列时，只有专用队列
成功接管后才能设置 scan bit。

producer 与 owner 释放并发时只有两种结果：

- producer 先设置 DIRTY，owner 的 `OWNED -> 0` CAS 失败并继续扫描；
- owner 先清 OWNED，producer 看到无 owner 后设置 QUEUED 并发布新 token。

不存在“mark bit 已设置但 Region 既不 dirty 也不 queued”的状态。

### 4.4 扫描内容

owner 从 slot 地址读取一次 TypeInfo，通过 `ScanDescriptorCache` 获得：

```cpp
struct ScanDescriptor {
    const TypeInfo* typeInfo;
    ScanKind kind;             // OBJECT / REF_ARRAY / STRUCT_ARRAY / WEAK / FINALIZER
    const uint32_t* refOffsets;
    uint32_t refCount;
    uint32_t elementStride;
};
```

普通对象按排序后的 `refOffsets` 连续读取；小数组在 owner 内完成；payload 超过
128 KiB 的引用数组转交 RangeQueue。扫描出的引用先进入 Worker-local mark word
聚合器，再批量提交，避免同一 mark word 上逐引用原子 RMW。

### 4.5 Bitmap word 批量提交

每个 `MarkContext` 保留 8 个 `(regionIdx, generation, wordIdx, mask)` entry。
新候选命中已有 entry 时只 OR mask；entry 冲突、填满、worker yield、队列发布和
终止检查前必须 flush。

flush 对每个 entry 执行一次：

```text
validMask = mask & acquireLoad(allocWord)
newBits = atomicFetchOr(markWord, validMask) & ~oldMarkWord
```

`newBits` 按 Region `scanClass` 处理。由于同一个 entry 只属于一个 Region，
一次 RMW 最多合并 64 个叶子对象。合法性检查在加入 entry 前完成，flush 时再次
以 occupancy 与 generation 拒绝 Region 重用窗口。

## 5. 大引用数组 RangeQueue

### 5.1 RangeTask

引用 payload 大于 128 KiB 时，Region owner 不遍历完整数组，而是创建连续
`RangeTask`：

```cpp
struct RangeTask {
    BaseObject* array;
    const TypeInfo* typeInfo;
    uint32_t beginElement;
    uint32_t endElement;
    uint16_t arrayRegionGeneration;
    uint16_t workEpoch;
};
```

任务边界按完整元素对齐，每个任务覆盖不超过 128 KiB payload。引用数组每个任务
最多处理 16,384 个 8B 引用；struct 数组使用 `elementStride` 换算元素数。

Region owner 先构造全部 range，逐个执行“outstanding++，release push”，所有任务
发布成功后才把外层数组的 scan bit 置 1。这里的 scan bit 表示“该对象的扫描职责
已经且仅已经被展开为持久工作”；RangeQueue 中未完成任务由统一 outstanding
计数保护，终止判定仍会等待真实字段扫描完成。

### 5.2 Range worker

Range worker 按以下循环处理：

1. 校验 `workEpoch`、数组 Region generation 和 alloc bit；
2. 从 `ScanDescriptorCache` 取得 component 描述符；
3. 顺序扫描 `[beginElement, endElement)`；
4. 使用 `MarkContext` 的 last-Region cache 解析相邻引用；
5. 使用 8-entry mark word 聚合器合并同 word mark；
6. 每 4 KiB payload 预取下一页 TypeInfo/Bitmap 元数据；
7. flush 聚合器后完成任务并递减 range outstanding。

RangeQueue 使用 worker 本地 deque：本地从尾部连续消费，其他 worker 从头部 steal。
数组天然按相邻区间分布，既保持顺序读取，又允许多个 Mark worker 并行处理 case26
的持久引用数组。

### 5.3 LARGE 对象

LARGE 对象仍由 base Region 的 `LARGE_MARKED` 原子位完成唯一发现。分类和处理固定为：

- LARGE `NO_REF`：winner 直接结束；
- LARGE 普通 `HAS_REF`：发布一个 ObjectTask；
- LARGE 引用数组/struct 数组：直接展开为 RangeTask；
- LARGE WeakRef/Finalizer：发布到 main-only ObjectQueue。

SPAN entry 只解析回 base Region，不独立标记。LARGE 对象大小从
`summaryOrLargeSize` 读取，不复用小对象 slot 字段。

## 6. 统一工作调度与终止判定

### 6.1 三类队列

最终调度器只有三类逻辑工作：

| 队列 | 元素 | 生产者 | 消费者 |
|---|---|---|---|
| ObjectQueue | `ObjectTask` | roots、barrier、LARGE/特殊对象发现 | GC main、helper；main-only lane 仅 GC main |
| RegionScanQueue | `RegionScanToken` | `HAS_REF` 小对象 mark winner | GC main、helper、assist |
| RangeQueue | `RangeTask` | 大数组扫描展开 | GC main、helper、assist |

每类队列都有 per-worker deque 和一个全局注入队列。worker 按
`Region -> Range -> Object` 轮转配额 `8:4:1`，连续 32 个任务后强制检查其他
队列，避免大数组或热点 Region 饥饿。

### 6.2 WorkLedger

```cpp
struct MSWorkLedger {
    std::atomic<uint64_t> publishSeq;
    std::atomic<uint64_t> objectOutstanding;
    std::atomic<uint64_t> regionOutstanding;
    std::atomic<uint64_t> rangeOutstanding;
    std::atomic<uint32_t> activeProducers;
    std::atomic<uint32_t> activeWorkers;
};
```

发布协议固定为：

```text
outstanding.fetch_add(1, relaxed)
queue.push(task, release)
publishSeq.fetch_add(1, release)
```

消费者成功取得任务后保持该 outstanding；任务及其本地 mark 聚合器 flush 完毕后
才 `fetch_sub(1, release)`。任务处理过程中产生的子任务必须先增加子任务
outstanding，再结束父任务，因此总工作量不会出现瞬时假零。

mutator barrier、root enumeration、Region owner 和 Range worker 进入可能发布工作
的代码前增加 `activeProducers`，退出且 flush local builder 后减少。worker 取得
任务后增加 `activeWorkers`，任务完成后减少。

### 6.3 并发终止

GC main 仅在下面条件同时成立时发起终止检查：

```text
ObjectQueue 全部 local/global lane 为空
RegionScanQueue 全部 local/global lane 为空
RangeQueue 全部 local/global lane 为空
objectOutstanding == 0
regionOutstanding == 0
rangeOutstanding == 0
activeProducers == 0
activeWorkers == 0
所有 AllocBuffer activeLog/local builder 已 publish
不存在 control.DIRTY 且无 QUEUED/OWNED 的 HAS_REF Region
```

随后执行双序列确认：

1. acquire 读取 `seq0 = publishSeq`；
2. 执行一次全队列 acquire fence 和 dirty Region summary 检查；
3. 再次读取全部计数与 `seq1 = publishSeq`；
4. 仅当 `seq0 == seq1` 且条件仍全部成立时进入 MarkTermination。

`dirty Region summary` 是按 64 个 Region 一个 word 的全局摘要；设置 DIRTY 时
同步置摘要 bit，owner 成功清空并释放 Region 后清摘要 bit。终止检查不遍历整个
RegionInfo array。

### 6.4 MarkTermination

MarkTermination 的 STW 固定点顺序为：

1. 停止 mutator，等待 `activeProducers == 0`；
2. flush 全部 write-barrier log、mark word builder 和本地 deque；
3. 扫描线程栈与全局根的最终变化；
4. drain Object、Region、Range 三类队列到 WorkLedger 全零；
5. 处理 WeakRef；
6. 执行 Finalizer resurrection，再次 drain 三类队列到全零；
7. 验证 dirty summary 为零且 `mark & ~scan` 在所有 `HAS_REF` oldEpoch Region
   中为零；
8. 发布 IDLE barrier phase，结束 STW。

任何专用队列都不得绕过第 4、6 步的统一 ledger。`CJ_MS_STW_SWEEP` 继续只作为
Sweep 根因诊断开关，不参与 Mark 终止协议。

## 7. Worker-local MarkContext

每个 GC main、helper 和 assist 调用都持有一个不共享的 `MarkContext`：

```cpp
struct MSMarkContext {
    RegionLookupCache lastRegion;
    MarkWordEntry markWords[8];
    ScanDescriptorEntry scanDescriptors[16];
    LocalDeque<ObjectTask> objectDeque;
    LocalDeque<RegionScanToken> regionDeque;
    LocalDeque<RangeTask> rangeDeque;
    MSMarkLocalStats stats;
};
```

### 7.1 last-Region cache

`RegionLookupCache` 保存：

```text
regionStart, regionEnd, regionIdx, regionGeneration,
MSRegionInfo*, allocWordBase, markWordBase, scanWordBase,
slotSize, divMagic, scanClass
```

引用落入 `[regionStart, regionEnd)` 且 generation 未改变时，不再访问
`MSRegionInfoArray`。引用跨 Region 后覆盖单 entry；不使用共享哈希表或 TLS 多级
cache，避免元数据本身再次分散。

### 7.2 TypeInfo 扫描描述符 cache

16-entry 直接映射 cache 的 key 为 `TypeInfo*`，value 为第 4.4 节的
`ScanDescriptor`。miss 时调用现有 TypeInfo/StackMap 解析逻辑生成描述符；命中后
扫描器直接使用紧凑 offsets/stride，不重复执行 `IsArrayType()`、
`HasRefField()` 和 component TypeInfo 链式判断。

TypeInfo 元数据生命周期覆盖进程，cache 不需要回收。动态泛型 TypeInfo 完成发布后
不可原地改变扫描布局。

### 7.3 本地统计

热路径只修改 context 私有计数，worker 退出或 GC 结束时合并：

```text
resolvedRefs
rejectedNonHeap
rejectedStale
allocBitMisses
markProbeHits
markWordFlushes
markBitsSubmitted
markBitsWon
leafMarked
regionTokensPublished
regionRescans
rangeTasksPublished
rangeBytesScanned
objectBytesScanned
typeInfoCacheHits/Misses
regionCacheHits/Misses
queueSteals
```

这些指标必须出现在单轮 `[MS-GC] MarkPath` 日志中，用于把“Mark 变慢”拆成
引用发现、Bitmap 提交、对象扫描和调度四类成本。

## 8. Pacer 与 Assist

### 8.1 WorkUnit

Pacer 不再用“处理对象数”或“WorkBuf 数”表示工作。每个 context 以 Q16 定点数
累计：

```text
work =
    resolvedRefs          * refCostQ16
  + bitmapWordsFlushed    * bitmapCostQ16
  + objectBytesScanned    * byteCostQ16
  + rangeBytesScanned     * byteCostQ16
```

初始系数固定为：

```text
refCostQ16     = 1.0
bitmapCostQ16  = 8.0
byteCostQ16    = 1.0 / 64
```

每轮 GC 根据各项 CPU time/计数的 EWMA 更新系数，范围限制为前一轮的
`[0.75, 1.25]`，避免单轮噪声使 assist 比例突变。

### 8.2 Pacer 目标

MarkSetup 根据：

```text
expectedWork = liveRefEdgesEWMA * refCost
             + liveScanBytesEWMA * byteCost
             + expectedBitmapWords * bitmapCost

availableAllocBytes = heapGoal - markStartInUseBytes
assistWorkPerByte = max(0, expectedWork - backgroundBudget) /
                    max(availableAllocBytes, 1)
```

计算本轮 assist 比率。`backgroundBudget` 来自当前启用 Mark worker 数与最近三轮
每 CPU 微秒完成的 WorkUnit，不再用逻辑 P 数推导。

heap goal 固定使用：

```text
heapGoal = liveBytes + max(liveBytes * gcPercent / 100, minRunwayBytes)
```

old bytes、young live bytes、remembered-set 扫描债务分别建模；触发 major GC 时
expectedWork 必须包含 old/immutable 的预计扫描工作。

### 8.3 Assist 执行

每次小对象或 LARGE 分配把 `allocatedBytes * assistWorkPerByte` 加入当前 P 的
Q16 debt。debt 为正时：

1. 从 RegionQueue、RangeQueue、ObjectQueue 按统一轮转取工作；
2. 最多运行 200 微秒或完成 4096 WorkUnit；
3. flush `MarkContext` 的 mark word builder 和本地任务；
4. 用实际完成的 WorkUnit 抵扣 debt；
5. 未还清 debt 留给下一次分配，不把一个 mutator 长时间钉死。

background worker 完成的工作只降低全局 remaining-work 预测，不凭空冲销某个 P 的
debt。进入 MarkTermination 前冻结新 debt，并由 STW drain 完成余量。

### 8.4 Worker 数量

helper 数由 `remainingWork / targetRemainingTime` 与实测
`workUnitsPerCpuUs` 决定，限制在：

```text
1 <= markWorkers <= min(activeProcessors, configuredMaxWorkers)
```

扩容要求连续两个 1ms 窗口预测超时；缩容要求连续四个窗口预测提前且队列无 steal
压力。worker 增减只影响消费者数量，不改变三平面、Region ownership 或终止协议。

## 9. Old/Immutable Region 与 Remembered Set

### 9.1 Region 级年龄

MSGC 不移动对象，因此 old 状态只按完整 Region 晋升。一个 Region 在 major GC
结束时同时满足以下条件即设置 `control.OLD`：

1. 连续两个 major GC 存活；
2. 两轮 live ratio 均不低于 95%；
3. Region 不含 WeakRef、Finalizer 或 pinned 特殊语义；
4. Region 已从所有 AllocBuffer 归还，之后不再用于新分配。

`aux` 保存两次 major age、最近 live ratio 和 dirty-card 计数。old Region 的空洞不
重新分配 young 对象；major Sweep 后整个 Region 为空才回到 FREE。该规则用少量
碎片换取 Region 内代际一致性，避免 slot 级年龄表和混代扫描。

Region 内所有对象的 TypeInfo 都带编译器 `immutable` 标志，且连续两轮 major
之间无 dirty card 时，再设置 `control.IMMUTABLE`。写屏障一旦观察到 immutable
Region 作为写入目标，先原子清除 `IMMUTABLE`，再执行 card 标记；反射/unsafe
写入也不能绕过该屏障。

### 9.2 Card table

old heap 使用 512B/card、1 byte/card 的 card table：

```text
cardIdx = (fieldAddress - heapStart) >> 9
cardTable[cardIdx] = DIRTY
```

写屏障在目标对象属于 old Region 且新引用属于 young Region 时，以 release store
设置 DIRTY，并把所属 card chunk 的 summary bit 置 1。SATB 删除屏障在并发 young
Mark 期间对被覆盖的 young old-value 执行发现。

MarkSetup 只枚举 summary 命中的 card chunk。扫描 card 时使用 TypeInfo 描述符找出
与 card 相交的引用字段，把 young target 作为 root 发布；扫描完后以
dirty-generation CAS 清 card。与扫描并发发生的写入会保留到下一次扫描，不被清除。

### 9.3 普通 GC 与 major GC

普通 GC：

- 根扫描只发现 young target；
- old target 直接视为存活，不写本轮 young mark plane；
- 扫描 remembered cards 产生 old→young roots；
- 只 Sweep young Region；
- immutable Region 不进入队列，也不读取对象或 Bitmap。

major GC：

- roots、young、old 和 immutable 全部进入本文第 3～6 节的统一 Mark；
- `NO_REF` old Region 仍只使用 occupancy/mark plane；
- `HAS_REF` immutable 大数组仍由 RangeQueue 并行扫描；
- major Sweep 更新 Region age、live ratio、OLD/IMMUTABLE 和 card summary；
- major 完成后清除已覆盖的 remembered cards，建立下一轮 young 基线。

major GC 在任一条件成立时触发：

```text
minorCyclesSinceMajor == 8
oldAllocatedBytes >= lastMajorOldLiveBytes * (1 + gcPercent / 100)
dirtyOldCards >= totalOldCards / 8
young heap goal 无法在总 heap limit 内建立 minRunwayBytes
显式 FullGC/OOM 请求
```

这使 2 GiB 长期不变 payload 在晋升后退出连续 7 轮普通 GC；major 轮仍依赖
`NO_REF` 快路径和大数组 RangeQueue 保证可控的完整堆扫描。

### 9.4 WeakRef 与 Finalizer

含 WeakRef/Finalizer 的 Region 永不设置 OLD/IMMUTABLE，避免普通 GC 跳过其语义。
WeakRef referent 的处理继续在强可达 fixed point 之后执行；Finalizer resurrection
产生的对象和 Region/Range 工作重新进入同一个 WorkLedger，直到第二次 fixed point。

## 10. Runtime/LLVM 联合 ABI

### 10.1 唯一 ABI 常量源

LLVM 的 `llvm/include/llvm/CodeGen/CJMSGCABI.h` 定义编译期常量，Runtime 的
`src/Heap/MSGCFastPath.h` 定义同名 C ABI；两端由测试逐项比较：

```text
MSGC_ABI_VERSION             2
MSGC_REGION_SHIFT            15
MSGC_REGION_INFO_SIZE        64
MSGC_BITMAP_PLANE_STRIDE     512
MSGC_SCAN_CLASS_COUNT        2
MSGC_ACTIVE_REGION_BASE      8
MSGC_ACTIVE_LOG_OFFSET       1024
MSGC_FAST_PATH_STATE_SIZE    64
MSGC_TYPEINFO_FLAG_OFFSET    9
MSGC_FLAG_HAS_REF_FIELD      0x01
MSGC_FLAG_IMMUTABLE          0x80
MSGC_FAST_PATH_SYMBOL        CJ_MRT_MSGCFastPathState_v2
```

RegionInfo 字段偏移、`layout` 位、`control` 位、WorkBuf/Task 布局也进入 ABI
contract test。目标 emitter 中不得再定义一份私有 magic number。

fast-path 全局符号固定为 `CJ_MRT_MSGCFastPathState_v2`。ABI v2 Runtime 不导出
无版本后缀的旧符号，ABI 不匹配在 dyld/loader 阶段直接失败。

### 10.2 分配 fast path

AArch64/X86 的固定分配路径为：

```text
读取 TypeInfo
  -> 非数组读取 TypeInfo.flag bit0
  -> 数组按现有 TypeInfo::HasRefField 语义解析 component TypeInfo
  -> scanClass = 0/1
读取或计算 sizeClass
activeIndex = sizeClass * 2 + scanClass
region = AllocBufferMS + 8 + activeIndex * 8
  -> BUMP/Bitmap slot 分配
  -> 初始化 TypeInfo
  -> 更新 alloc plane
  -> Mark active 时更新 mark plane
slow -> 带 TypeInfo 调用 MSGC slow helper
```

静态分配由 `CJRuntimeLowering` 附加
`cj-msgc-scanclass=0|1`；动态数组仍在 emitter 中解析 component TypeInfo。所有
slow helper 的第一个参数保留 `TypeInfo*`，不得降级为 size-only 调用。

### 10.3 写屏障 fast path

写屏障通过 `MSGCFastPathState v2` 直接取得 heap、RegionInfo 和三平面 base：

1. 先做 heap range gate；
2. 读取 Region layout 和 generation；
3. 验证 slot 与 alloc plane；
4. 批量或单 bit 提交 mark plane；
5. `NO_REF` winner 结束；
6. `HAS_REF` winner 设置 Region DIRTY/QUEUED；
7. old destination 同时执行 old→young card 标记。

编译器 fast path 只执行无分配、无锁的状态更新。需要申请 Region token、RangeTask、
WorkBuf 或 card chunk 时跳到带完整参数的 Runtime slow helper。

### 10.4 联合发布规则

Runtime dylib、compiler 和 stdlib 作为同一 SDK 构建并发布。切换 runtime 后必须用
该 SDK 重新编译所有 `.cj` 用例；不得用 CopyGC 或 ABI v1 编译的 binary 加载本
Runtime。验证时必须在隔离的 `env -i` shell 中检查实际加载的
`libcangjie-runtime.dylib`。

## 11. 按依赖顺序实施

以下顺序是依赖顺序，不是多套架构的切换顺序。每项只实现本文已经固定的最终字段、
最终协议和最终 ABI；不保留旧路径作为 fallback。

### Task 1：一次落地 ABI v2、MSRegionInfo v2 与三平面 Bitmap

**Files**

- Modify: `src/Heap/MSGCFastPath.h`
- Modify: `src/Heap/MAllocator/MSRegionInfo.h`
- Modify: `src/Heap/MAllocator/MSRegionInfo.cpp`
- Modify: `src/Heap/MAllocator/MSBitmapArena.h`
- Modify: `src/Heap/MAllocator/MSBitmapArena.cpp`
- Modify: `src/Heap/MAllocator/MSRegionManager.cpp`
- Modify: `src/Heap/MAllocator/MSRegionSpace.cpp`
- Modify: `llvm/include/llvm/CodeGen/CJMSGCABI.h`
- Modify: `llvm/lib/Target/AArch64/AArch64MSGCEmitter.cpp`
- Modify: `llvm/lib/Target/X86/X86MSGCEmitter.cpp`
- Modify: `test/MSGCFastPathStateABIContractTest.cpp`
- Modify: `test/MSRegionInfoBatchMarkTest.cpp`
- Create: `test/MSGCThreePlaneBitmapTest.cpp`
- Create: `test/MSGCFinalLayoutABIContractTest.sh`

**Implementation**

- 按第 2.2、2.6 节重排两个结构并补全 `static_assert`；
- 删除 `MSRegionInfo::allocBitmap/markBitmap` 指针字段；
- `MSBitmapArena` 预留三个独立 plane，提供
  `AllocWord/MarkWord/ScanWord(regionIdx, logicalWord)`；
- 把 Clear、Sweep、Stats、Large、Grow/Shrink 的 Bitmap 访问全部改到 plane API；
- AArch64/X86 只从 `CJMSGCABI.h` 读取 ABI v2 常量；
- 删除 emitter 内的 896、32、Bitmap 字段偏移等私有常量。

**Verification**

```bash
mkdir -p .tmp/step59
clang++ -std=c++14 -Wall -Wextra -Werror -I. -Isrc \
  test/MSGCThreePlaneBitmapTest.cpp -o .tmp/step59/three_plane &&
  .tmp/step59/three_plane
bash test/MSGCFinalLayoutABIContractTest.sh
bash test/MSGCFastPathStateABIContractTest.sh
```

预期三个命令退出码均为 0，且 contract 输出的 fast-path state size 为 64、
RegionInfo size 为 64、plane stride 为 512。

### Task 2：按 TypeInfo 建立 `[sizeClass][scanClass]` 分配体系

**Files**

- Create: `src/Heap/MAllocator/MSScanClass.h`
- Modify: `src/Heap/MAllocator/MSAllocBuffer.h`
- Modify: `src/Heap/MAllocator/MSAllocBuffer.cpp`
- Modify: `src/Heap/MAllocator/MSCentralRegionPool.h`
- Modify: `src/Heap/MAllocator/MSCentralRegionPool.cpp`
- Modify: `src/ObjectManager.inline.h`
- Modify: `src/CompilerCalls.cpp`
- Modify: `src/CompilerCalls.h`
- Modify: `test/MSGCAllocBufferFastPathABIContractTest.sh`
- Create: `test/MSGCScanClassAllocationTest.cpp`
- Create: `test/MSGCAllocationPublicationTest.cpp`

**Implementation**

- 所有 MSGC object/array/generic slow helper 将 `TypeInfo*` 传到
  `AllocBufferMS::Allocate`；
- Central pool、empty cache、active Region 全部使用二维分类；
- `activeRegions_` 改成 72 项，`activeLog_` 固定 +1024；
- 分配严格使用“初始化对象头 → release alloc bit → Mark 期间 release mark bit”；
- Region refill 固定 `scanClass` 和 `allocEpoch`，归还前不得重分类；
- MarkSetup 实现第 3.3 节的 flush、封存和 epoch flip。

**Verification**

```bash
clang++ -std=c++14 -pthread -Wall -Wextra -Werror -I. -Isrc \
  test/MSGCScanClassAllocationTest.cpp -o .tmp/step59/scan_class_alloc &&
  .tmp/step59/scan_class_alloc
clang++ -std=c++14 -pthread -Wall -Wextra -Werror -I. -Isrc \
  test/MSGCAllocationPublicationTest.cpp -o .tmp/step59/alloc_publish &&
  .tmp/step59/alloc_publish
bash test/MSGCAllocBufferFastPathABIContractTest.sh
```

预期 `[]byte`、纯值对象进入 `NO_REF` 桶，引用对象/引用数组进入 `HAS_REF` 桶，
并发 reader 永远看不到 alloc bit 为 1 但 TypeInfo 尚未发布的 slot。

### Task 3：同步完成 LLVM 分配与屏障生成

**Files**

- Modify: `llvm/lib/Transforms/Scalar/CJRuntimeLowering.cpp`
- Modify: `llvm/lib/CodeGen/CJBarrierLowering.cpp`
- Modify: `llvm/lib/Target/AArch64/AArch64MSGCEmitter.cpp`
- Modify: `llvm/lib/Target/X86/X86MSGCEmitter.cpp`
- Modify: `llvm/test/Transforms/CJRuntimeLowering/new_array_*_msgc.ll`
- Modify: `llvm/test/CodeGen/AArch64/CangjieGC/gc-mode-newobject-msgc.ll`
- Modify: `llvm/test/CodeGen/X86/cangjie-msgc-allocation-static-fastpath.ll`
- Modify: `llvm/test/CodeGen/X86/cangjie-msgc-allocation-dynamic-fastpath.ll`
- Modify: `llvm/test/CodeGen/X86/cangjie-msgc-writebarrier-fastpath.ll`
- Create: `llvm/test/CodeGen/AArch64/CangjieGC/msgc-scanclass-allocation.ll`
- Create: `llvm/test/CodeGen/X86/cangjie-msgc-scanclass-allocation.ll`

**Implementation**

- lowering 为静态对象/数组附加最终 `cj-msgc-scanclass`；
- 两个 emitter 使用 `sizeClass * 2 + scanClass` 读取 active Region；
- 动态数组按 TypeInfo component 语义计算 scan class；
- fast allocation 更新 alloc/mark plane，不再解引用 RegionInfo Bitmap 指针；
- write barrier 使用 ABI v2 plane base 和 Region control；
- slow call 保留 TypeInfo 参数和原 calling convention；
- Mach-O、ELF、COFF 的 symbol/relocation 路径保持一致。

**Verification**

```bash
LLVM_BUILD=/Users/lipeisheng/code/cangjie/cangjie/cangjie_compiler/out/third_party/llvm
python3 llvm/utils/lit/lit.py -sv \
  "$LLVM_BUILD/test/Transforms/CJRuntimeLowering" \
  "$LLVM_BUILD/test/CodeGen/AArch64/CangjieGC" \
  "$LLVM_BUILD/test/CodeGen/X86"
```

预期新增及受影响 lit 测试全部 PASS；生成汇编中 active index 含 `*2+scanClass`，
且不存在 `+896`、state size 32 和 RegionInfo Bitmap pointer load。

### Task 4：替换小对象发现路径并建立 MarkContext

**Files**

- Create: `src/Heap/MCollector/MSMarkContext.h`
- Create: `src/Heap/MCollector/MSMarkContext.cpp`
- Modify: `src/Heap/MCollector/MarkSweepCollector.h`
- Modify: `src/Heap/MCollector/MarkSweepCollector.cpp`
- Modify: `src/Heap/MBarrier/MSMarkBarrier.cpp`
- Modify: `src/Common/GCRefFieldScan.inline.h`
- Modify: `test/MSGCDiscoverObjectFastPathContractTest.sh`
- Modify: `test/MSRegionInfoBatchMarkTest.cpp`
- Create: `test/MSGCNoRefLeafDiscoveryTest.cpp`
- Create: `test/MSGCMarkContextCacheTest.cpp`

**Implementation**

- `ResolvedMarkTarget` 改成第 3.4 节的纯元数据结果；
- occupancy 校验改读 alloc plane；
- `NO_REF` winner 从函数内直接返回，源码契约禁止出现 `GetTypeInfo`；
- 建立 last-Region、8-entry mark word 和 16-entry scan descriptor cache；
- root、barrier、object field 和 range 扫描统一调用 MarkContext；
- batch flush 以 generation/occupancy 二次校验并按 `newBits` 发布后续工作。

**Verification**

```bash
bash test/MSGCDiscoverObjectFastPathContractTest.sh
clang++ -std=c++14 -pthread -Wall -Wextra -Werror -I. -Isrc \
  test/MSGCNoRefLeafDiscoveryTest.cpp -o .tmp/step59/no_ref_leaf &&
  .tmp/step59/no_ref_leaf
clang++ -std=c++14 -pthread -Wall -Wextra -Werror -I. -Isrc \
  test/MSGCMarkContextCacheTest.cpp -o .tmp/step59/mark_context &&
  .tmp/step59/mark_context
```

预期 NO_REF 测试通过计数器证明：标记 N 个叶子 slot 时 TypeInfo/object-load
计数为 0，mark bit winner 数为 N，重复引用不产生额外 winner。

### Task 5：实现 RegionScanQueue 与 mark/scan 差集状态机

**Files**

- Create: `src/Heap/MCollector/MSRegionScanQueue.h`
- Create: `src/Heap/MCollector/MSRegionScanQueue.cpp`
- Modify: `src/Heap/MCollector/MarkSweepCollector.h`
- Modify: `src/Heap/MCollector/MarkSweepCollector.cpp`
- Modify: `src/Heap/MCollector/MSMarkContext.h`
- Create: `test/MSGCRegionScanStateTest.cpp`
- Create: `test/MSGCRegionDirtyRaceTest.cpp`
- Create: `test/MSGCRegionGenerationTokenTest.cpp`

**Implementation**

- 实现 8B token、per-worker deque 和全局注入队列；
- 按第 4.2、4.3 节实现 `QUEUED/OWNED/DIRTY` CAS；
- 扫描使用 `toScan = alloc & mark & ~scan`；
- 普通对象、小数组、WeakRef、Finalizer 分别进入规定消费者；
- Region 完成后更新 scan plane 并清 dirty summary；
- 所有退出/yield/异常路径先 flush MarkContext 再释放 OWNED。

**Verification**

```bash
for t in RegionScanState RegionDirtyRace RegionGenerationToken; do
  clang++ -std=c++14 -pthread -Wall -Wextra -Werror -I. -Isrc \
    "test/MSGC${t}Test.cpp" -o ".tmp/step59/${t}" &&
    ".tmp/step59/${t}"
done
```

预期百万次 producer/owner 交错后 `mark & ~scan == 0`、无重复 owner、无遗留
DIRTY；Region 重用后的旧 token 不扫描新一代 slot。

### Task 6：实现 RangeQueue 和超大数组拆分

**Files**

- Create: `src/Heap/MCollector/MSRangeQueue.h`
- Create: `src/Heap/MCollector/MSRangeQueue.cpp`
- Modify: `src/Heap/MCollector/MarkSweepCollector.cpp`
- Modify: `src/Common/GCRefFieldScan.inline.h`
- Create: `test/MSGCRangeQueueTest.cpp`
- Create: `test/MSGCLargeRefArrayParallelScanTest.cpp`
- Modify: `test/MSGCLargeRegionSemanticsContractTest.sh`

**Implementation**

- 建立 128 KiB payload 上限的 element-aligned RangeTask；
- 引用数组和含引用 struct 数组使用同一拆分器；
- durable publish 全部成功后才设置外层对象 scan bit；
- Range worker 使用同一个 MarkContext 和 WorkLedger；
- LARGE base/SPAN、WeakRef、Finalizer 按第 5.3 节分流；
- 任务 steal 保持不重叠、不遗漏 `[0, length)`。

**Verification**

```bash
clang++ -std=c++14 -pthread -Wall -Wextra -Werror -I. -Isrc \
  test/MSGCRangeQueueTest.cpp -o .tmp/step59/range_queue &&
  .tmp/step59/range_queue
clang++ -std=c++14 -pthread -Wall -Wextra -Werror -I. -Isrc \
  test/MSGCLargeRefArrayParallelScanTest.cpp -o .tmp/step59/large_ref_array &&
  .tmp/step59/large_ref_array
bash test/MSGCLargeRegionSemanticsContractTest.sh
```

预期边界长度 `0/1/16384/16385`、非 8B struct stride 和多 worker steal 均覆盖
每个元素恰好一次，数组长度 413.7 万时产生多个可并行 range。

### Task 7：用 WorkLedger 重写调度和终止判定

**Files**

- Create: `src/Heap/MCollector/MSWorkLedger.h`
- Create: `src/Heap/MCollector/MSUnifiedMarkScheduler.h`
- Create: `src/Heap/MCollector/MSUnifiedMarkScheduler.cpp`
- Modify: `src/Heap/MCollector/MarkSweepCollector.h`
- Modify: `src/Heap/MCollector/MarkSweepCollector.cpp`
- Modify: `src/Heap/MCollector/MSMarkWorkerControl.h`
- Modify: `src/Heap/MCollector/MSMarkTerminationPolicy.h`
- Modify: `src/Common/LFStack.h`
- Modify: `test/MSMarkTerminationPolicyTest.cpp`
- Modify: `test/MSWorkBufInvariantTest.cpp`
- Create: `test/MSGCWorkLedgerRaceTest.cpp`
- Create: `test/MSGCUnifiedTerminationTest.cpp`

**Implementation**

- Object/Region/Range 统一 outstanding、producer、worker 和 publishSeq；
- 发布子任务先记账，完成父任务后销账；
- worker 按 `8:4:1` 与 32-task fairness 上限取任务；
- assist、helper、GC main 共用 scheduler，不复制 drain 循环；
- 实现 dirty summary 和双 publishSeq 终止检查；
- MarkTermination 按第 6.4 节执行两次 fixed point；
- 删除只根据 WorkBuf、idle worker 或超时决定终止的旧分支。

**Verification**

```bash
for t in MSWorkBufInvariant MSMarkTerminationPolicy \
         MSGCWorkLedgerRace MSGCUnifiedTermination; do
  clang++ -std=c++14 -pthread -Wall -Wextra -Werror -I. -Isrc \
    "test/${t}Test.cpp" -o ".tmp/step59/${t}" &&
    ".tmp/step59/${t}"
done
bash test/MSGCTailDrainContractTest.sh
bash test/MSGCPostMarkSemanticClosureContractTest.sh
```

预期 producer 在终止检查任意指令间发布 Object/Region/Range 工作都能迫使检查
重试；最终 ledger 全零，三类队列与 dirty summary 全空。

### Task 8：把 Pacer/Assist 改成 WorkUnit

**Files**

- Modify: `src/Heap/MCollector/MSAssistPacing.h`
- Modify: `src/Heap/MCollector/MSMarkWorkerPolicy.h`
- Modify: `src/Heap/MCollector/MSMarkLocalStats.h`
- Modify: `src/Heap/MCollector/MarkSweepCollector.cpp`
- Modify: `src/Heap/MAllocator/MSAllocBuffer.cpp`
- Modify: `test/MSAssistPacingTest.cpp`
- Modify: `test/MSMarkWorkerPolicyTest.cpp`
- Modify: `test/MSMarkLocalStatsTest.cpp`
- Create: `test/MSGCWorkUnitPacerTest.cpp`

**Implementation**

- 债务、background credit 和 worker 速率统一换算为 Q16 WorkUnit；
- 采集 ref/bitmap/scan-byte/range-byte 四项成本并更新受限 EWMA；
- assist 实现 200µs/4096 WorkUnit slice；
- helper 扩缩容使用两窗扩、四窗缩；
- 日志输出预计工作、实际工作、各系数和 assist 完成占比；
- 删除 object-count/WorkBuf-count 作为唯一 debt 单位的接口。

**Verification**

```bash
for t in MSAssistPacing MSMarkWorkerPolicy MSMarkLocalStats MSGCWorkUnitPacer; do
  clang++ -std=c++14 -pthread -Wall -Wextra -Werror -I. -Isrc \
    "test/${t}Test.cpp" -o ".tmp/step59/${t}" &&
    ".tmp/step59/${t}"
done
```

预期相同对象数但扫描字节相差 32 倍的输入产生不同 debt；EWMA 单轮变化不超过 25%；
assist slice 不超过两个硬上限。

### Task 9：实现 old/immutable Region 和 CardTable

**Files**

- Create: `src/Heap/MAllocator/MSCardTable.h`
- Create: `src/Heap/MAllocator/MSCardTable.cpp`
- Modify: `src/Heap/MAllocator/MSRegionInfo.h`
- Modify: `src/Heap/MAllocator/MSCentralRegionPool.cpp`
- Modify: `src/Heap/MAllocator/MSRegionSpace.cpp`
- Modify: `src/Heap/MBarrier/MSMarkBarrier.cpp`
- Modify: `src/Heap/MCollector/MarkSweepCollector.cpp`
- Modify: `src/ObjectModel/MClass.h`
- Modify: `llvm/lib/Transforms/Scalar/CJRuntimeLowering.cpp`
- Create: `test/MSGCOldRegionPromotionTest.cpp`
- Create: `test/MSGCCardTableRaceTest.cpp`
- Create: `test/MSGCOldToYoungCorrectnessTest.cpp`
- Create: `test/MSGCImmutableDeoptTest.cpp`

**Implementation**

- 建立 512B card、dirty generation 和 chunk summary；
- 按“两次 major + 95% live + 无特殊对象”晋升完整 Region；
- 编译器发布 TypeInfo immutable 标志，major 汇总 Region 全部 TypeInfo；
- 写屏障实现 old→young card 和 immutable clear；
- 普通 GC 只追踪 young + remembered roots，只 Sweep young；
- 按第 9.3 节固定条件选择 major；
- WeakRef/Finalizer Region 永不晋升。

**Verification**

```bash
for t in MSGCOldRegionPromotion MSGCCardTableRace \
         MSGCOldToYoungCorrectness MSGCImmutableDeopt; do
  clang++ -std=c++14 -pthread -Wall -Wextra -Werror -I. -Isrc \
    "test/${t}Test.cpp" -o ".tmp/step59/${t}" &&
    ".tmp/step59/${t}"
done
```

预期 old→young 写入与 card 清理并发时 young 对象不丢失；immutable Region 第一次
写入立即降级并进入 remembered set；普通 GC 不扫描未 dirty old Region。

### Task 10：完整构建、SDK 联合验证和 case26 验收

**Files**

- Modify: `CMakeLists.txt`
- Modify: `src/CMakeLists.txt`
- Modify: `MSGC_RESULT/Result/case26_http_complex_json/run_compare.sh`
- Create: `MSGC_RESULT/Result/case26_http_complex_json/analyze_step59_mark_path.py`
- Create: `MSGC_RESULT/Implementation/Step59_case26验收记录.md`

**Implementation**

- 把新增 Runtime 源文件加入 MarkSweep 构建，CopyGC 不编译这些实现；
- 完整构建 compiler、Runtime、stdlib 到 MSGC SDK；
- 隔离环境验证 dylib 来源；
- 用最终 SDK 重新编译 case26 Cangjie 用例；
- 分别运行 128 MiB 与 2 GiB 持久对象数据集；
- 汇总 MarkPath、WorkLedger、Pacer、ordinary/major GC 和吞吐数据；
- 将命令、SDK 三件套来源、原始目录、结果和结论写入验收记录。

**Verification**

```bash
python3 build.py build --target native --build-type release \
  --gc-mode marksweep -v 0.0.1
```

Runtime 构建成功后，按仓库“多 SDK 对比测试”规则在 `env -i` 子 shell 中完成 SDK
联合构建与 case26。任何 runtime 替换后都重新执行 `cjc`，并先用
`DYLD_PRINT_LIBRARIES=1` 确认加载路径。

## 12. 验证矩阵与验收门槛

### 12.1 设计冲突闭合

| 组合 | 表面冲突 | 最终闭合方式 |
|---|---|---|
| alloc plane 与并发分配 | Mark 可能观察半初始化 slot | 对象头先初始化，alloc bit release 发布；Mark acquire 读取 |
| NO_REF 不读对象与 stale edge 校验 | 不读 TypeInfo 后如何证明对象合法 | Region generation、slot 对齐、稳定 occupancy 三项完成合法性证明 |
| batch mark 与 Region owner | batch winner 尚未逐对象入队 | batch flush 只设置 Region DIRTY；owner 从 mark/scan 差集取工作 |
| RangeQueue 与 scan bit | 外层数组 scan bit 早于 range 完成 | scan bit 表示职责已 durable 展开；range outstanding 阻止终止 |
| old Region 与 Region 空洞复用 | 新 young 对象会混入 old | old Region 永不重新分配，整 Region 为空才回 FREE |
| immutable 与反射/unsafe 写 | 静态不可变假设可能被打破 | 所有堆引用写仍经屏障，先清 IMMUTABLE 再标 card |
| 三类队列与 termination | 某一队列空不代表无工作 | 统一 outstanding、active producer/worker、publishSeq 和 dirty summary |
| Runtime 与 LLVM 布局 | 单边升级会静默破坏内存 | ABI v2 单一签名、联合 SDK 发布、替换 runtime 后强制重编 `.cj` |
| MarkContext cache 与 Region 重用 | cache 可能命中旧元数据 | 每次 cache hit 同时比较 `regionGeneration` |
| 普通 GC 与 WeakRef/Finalizer | old 跳过可能改变语义 | 特殊对象 Region 不晋升，每轮仍走专用闭包 |

这些闭合规则直接属于最终协议，不通过 feature flag、双布局或运行时探测选择另一条
实现。

### 12.2 正确性矩阵

| 场景 | 必须证明 |
|---|---|
| 纯 `[]byte` 小对象 | mark 正确，TypeInfo/object-load=0，不进入扫描队列 |
| 同 Region 混合 TypeInfo | 因 scanClass 一致可安全按对象描述符扫描，不把 NO_REF 混入 |
| stale 指针命中已回收 slot | alloc bit 或 generation 拒绝，mark bit 不改变 |
| producer/owner 释放竞争 | 最终 `mark & ~scan == 0`，无遗留 DIRTY |
| Range steal | 每个元素恰好扫描一次，无重叠、无缺口 |
| Mark 中新分配 | alloc-black，当前 Sweep 跳过 newEpoch |
| WeakRef | 强闭包后处理，referent 清理语义不变 |
| Finalizer resurrection | 新工作重新进入 ledger 并第二次收敛 |
| old→young 并发写 | remembered card 不丢，young target 存活 |
| Region grow/shrink/reuse | plane commit/decommit 与 generation 同步 |
| OOM/FullGC | 强制 major，三类工作和 Sweep 均完成 |
| CopyGC | 构建和原测试不受 MSGC 文件影响 |

### 12.3 性能基线

固定使用以下原始结果作为实现前基线：

```text
128 MiB:
MSGC_RESULT/Result/case26_http_complex_json/result/
20260727_095319_output_msgc_vs_copygc_go_java_c1000_p100K_compare/msgc

2 GiB:
MSGC_RESULT/Result/case26_http_complex_json/result/
20260727_095905_output_msgc_vs_copygc_go_java_c1000_p100K_compare/msgc
```

基线关键值：

| 指标 | 128 MiB | 2 GiB |
|---|---:|---:|
| 端到端有效对象速率 | 68.201 Mobj/s | 16.033 Mobj/s |
| natural 有效对象速率 | 76.062 Mobj/s | 23.464 Mobj/s |
| Mark wall P95 | 56.437 ms | 3676.664 ms |
| debt-stalled 比例 | 16.59% | 73.68% |
| MarkTermination 时间占比 | 23.88% | 41.59% |

### 12.4 最终验收门槛

正确性门槛全部满足后，再判定性能：

1. `NO_REF` 叶子 microbenchmark 不读取对象头/TypeInfo，单线程 mark throughput
   不低于 80 Mslot/s；
2. 2 GiB major GC 的 natural 有效对象速率不低于 40 Mobj/s；
3. old Region 晋升后的普通 GC 不扫描 2 GiB 持久 `[]byte` payload，对应
   `oldSkippedBytes` 不低于持久对象字节数的 95%；
4. 2 GiB 主窗口加权端到端有效对象速率不低于 50 Mobj/s；
5. 2 GiB `debt-stalled` 周期占比不高于 5%，MarkTermination 占 Mark wall
   不高于 10%；
6. 2 GiB Mark P95 不高于 750 ms；
7. 128 MiB 请求吞吐、P99 latency 和 RSS 相对基线回退均不超过 5%；
8. 三平面实际 committed bytes 与 Region 分类计数吻合，NO_REF 不 commit scan
   plane；
9. GC 结束时三类 outstanding、active producer/worker、dirty summary 全为 0；
10. ASAN/TSAN 定向测试无越界、use-after-free 和数据竞争报告。

以上门槛按相同机器、相同 concurrency/payload、相同 16 GiB heap、相同 60s 主窗口
执行三次，取中位数。单轮最好结果不作为通过依据。

### 12.5 最终命令顺序

```bash
# 1. Runtime 定向单元与契约测试
bash test/MSGCFinalLayoutABIContractTest.sh
bash test/MSGCFastPathStateABIContractTest.sh
bash test/MSGCAllocBufferFastPathABIContractTest.sh
bash test/MSGCDiscoverObjectFastPathContractTest.sh
bash test/MSGCTailDrainContractTest.sh
bash test/MSGCPostMarkSemanticClosureContractTest.sh

# 2. Runtime release 构建
python3 build.py build --target native --build-type release \
  --gc-mode marksweep -v 0.0.1

# 3. LLVM lit
LLVM_BUILD=/Users/lipeisheng/code/cangjie/cangjie/cangjie_compiler/out/third_party/llvm
python3 /Users/lipeisheng/code/cangjie/cangjie/cangjie_compiler/third_party/llvm-project/llvm/utils/lit/lit.py \
  -sv "$LLVM_BUILD/test/CodeGen/AArch64/CangjieGC" \
      "$LLVM_BUILD/test/CodeGen/X86" \
      "$LLVM_BUILD/test/Transforms/CJRuntimeLowering"

# 4. 联合 SDK、dylib 来源探测、重新编译 case26、三轮 A/B
# 严格使用仓库 AGENTS.md 的 env -i 模板和 runtime/compiler/stdlib 三件套记录。
```

## 13. 文件影响清单

### 13.1 Runtime

| 模块 | 文件 | 作用 |
|---|---|---|
| ABI | `src/Heap/MSGCFastPath.h` | 64B fast-path state v2 |
| Region | `src/Heap/MAllocator/MSRegionInfo.{h,cpp}` | 64B RegionInfo v2、control/layout |
| Bitmap | `src/Heap/MAllocator/MSBitmapArena.{h,cpp}` | alloc/mark/scan 三平面 |
| 分类 | `src/Heap/MAllocator/MSScanClass.h` | TypeInfo 到 ScanClass |
| 分配 | `src/Heap/MAllocator/MSAllocBuffer.{h,cpp}` | 72 active Regions、发布顺序 |
| 中央池 | `src/Heap/MAllocator/MSCentralRegionPool.{h,cpp}` | 二维桶、封存、major age |
| Region 空间 | `src/Heap/MAllocator/MSRegionSpace.{h,cpp}` | plane 生命周期、young/old 统计 |
| Card | `src/Heap/MAllocator/MSCardTable.{h,cpp}` | remembered set |
| Mark context | `src/Heap/MCollector/MSMarkContext.{h,cpp}` | cache、batch、local stats |
| Region queue | `src/Heap/MCollector/MSRegionScanQueue.{h,cpp}` | Region token/state machine |
| Range queue | `src/Heap/MCollector/MSRangeQueue.{h,cpp}` | 大数组分片 |
| Ledger/scheduler | `src/Heap/MCollector/MSWorkLedger.h`、`MSUnifiedMarkScheduler.{h,cpp}` | 三类工作统一收敛 |
| Collector | `src/Heap/MCollector/MarkSweepCollector.{h,cpp}` | root、Mark、termination、major |
| Pacer | `src/Heap/MCollector/MSAssistPacing.h`、`MSMarkWorkerPolicy.h` | WorkUnit debt/worker |
| Barrier | `src/Heap/MBarrier/MSMarkBarrier.cpp` | Region dirty、card、SATB |
| 扫描器 | `src/Common/GCRefFieldScan.inline.h` | descriptor/range 扫描 |
| 分配入口 | `src/CompilerCalls.{h,cpp}`、`src/ObjectManager.inline.h` | TypeInfo 全链路 |

### 13.2 LLVM

| 文件 | 作用 |
|---|---|
| `llvm/include/llvm/CodeGen/CJMSGCABI.h` | 唯一编译期 ABI v2 常量 |
| `llvm/lib/Transforms/Scalar/CJRuntimeLowering.cpp` | scanclass 属性和分配入口 |
| `llvm/lib/CodeGen/CJBarrierLowering.cpp` | MSGC 屏障 lowering |
| `llvm/lib/Target/AArch64/AArch64MSGCEmitter.cpp` | AArch64 allocation/barrier |
| `llvm/lib/Target/X86/X86MSGCEmitter.cpp` | X86-64 allocation/barrier |
| `llvm/test/Transforms/CJRuntimeLowering/` | lowering 契约 |
| `llvm/test/CodeGen/AArch64/CangjieGC/` | AArch64 ABI/汇编契约 |
| `llvm/test/CodeGen/X86/` | X86-64 ABI/汇编契约 |

### 13.3 测试与结果

现有 `test/MSGC*ContractTest.sh`、`test/MS*Test.cpp` 中与 RegionInfo、Bitmap、
AllocBuffer、DiscoverObject、WorkBuf、termination、pacer、large object 相关的
测试全部更新到最终协议；第 11 节列出的新测试覆盖三平面、ScanClass、Region race、
Range、WorkLedger、old/card。

性能脚本只增加 Step59 指标解析，不覆盖两组基线原始文件。最终结果写入新的
`Step59_case26验收记录.md`，并明确记录 compiler、runtime、stdlib 来源和 dylib
探测结果。

### 13.4 明确不改

- CopyGC Region、CopyGC allocator 和 CopyGC collector；
- Cangjie 语言层对象语义；
- Region 大小 32 KiB 和 36 个 size class；
- `CJ_MS_STW_SWEEP` 诊断开关；
- 两组 20260727 case26 基线目录及其中原始日志。
