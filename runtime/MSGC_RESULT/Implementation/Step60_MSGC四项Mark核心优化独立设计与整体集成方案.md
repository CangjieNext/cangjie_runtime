# Step60：MSGC 四项 Mark 核心优化独立设计与整体集成方案

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 分别定义 ScanClass Region、三平面 Bitmap、Region 级扫描和 Mark Bitmap 批量更新四项优化，再把它们组合成一条无重复对象检查、具备 Region 局部性且可并发收敛的最终 Mark 热路径。

**Architecture:** 分配时把小对象按 `NO_REF/HAS_REF` 固定到不同 Region；PreEnum 只冻结 Central SEALED Region 为 collectSet，TLS ACTIVE 不 flush；GC Root 闭包可以穿过 ACTIVE 对象，ACTIVE 临时复用 mark/scan plane 表达 visit/scan，但 Sweep 只认 collectSet；引用发现由 Worker-local batch 合并同一 bitmap word，Region owner 按 token role 分别执行 `alloc & mark & ~scan` 或 `visit & ~scan`。

**Tech Stack:** C++14、CMake、Cangjie Runtime Mark-Sweep GC、Clang/LLVM AArch64/X86-64 emitter、独立 C++ 单元测试与 shell ABI contract test。

---

## 1. 文档范围与当前基线

### 1.1 范围

本文只设计以下四项：

1. `NO_REF/HAS_REF Region` 分离；
2. alloc/mark/scan 三平面 Bitmap；
3. `HAS_REF` 小对象的 Region 级扫描；
4. mark word 批量更新。

RangeQueue、Pacer/Assist、Old/Immutable Region、remembered set、Sweep helper 数量和
Scavenge 不在 Step60 范围内。Step60 对这四项的定义优先于 Step59 中相同主题；Step59
其余设计保持独立。

### 1.2 当前源码事实

截至 2026-07-27，当前 MSGC 是：

| 项目 | 当前实现 |
|---|---|
| Region 分类 | 只按 36 个 size class 分类，`activeRegions_[36]`、`buckets_[36]` |
| Region 内对象 | 同一 size class 的有引用和无引用对象可以混合 |
| Bitmap | 每 Region 1024B arena slot，实际按 size class 紧邻放置 alloc/mark 两段 |
| alloc bit | `1=free, 0=allocated`，由独占 AllocBuffer 普通读写 |
| mark bit | `1=marked`，多个 GC/assist/barrier 线程 atomic `fetch_or` |
| 白对象验证 | `DiscoverObject()` 在 mark RMW 前读取对象 stateWord/TypeInfo |
| 灰工作 | mark winner 且 `HasRefField()` 才逐对象进入 `WorkBuf` |
| 消费 | `DrainCurrentBuf()` 逐对象读取 TypeInfo 并扫描字段 |
| 编译器 ABI | `activeRegions_` 从 +8 开始，`activeLog_` 固定 +896 |

当前每条引用的主要路径是：

```text
ref
  -> heap range
  -> RegionInfo + slot/alignment
  -> mark probe
  -> 白对象读取 stateWord/TypeInfo
  -> atomic mark
  -> HasRefField
  -> HAS_REF winner 进入逐对象 WorkBuf
```

2 GiB case26 中约 413.7 万个持久 `Array<Byte>` 都是 `NO_REF` 叶子对象。它们不进入
WorkBuf，但当前每个白对象仍要完成 Region/slot、stateWord/TypeInfo 和 mark RMW。

### 1.3 最终目标

最终热路径先按 Region role 分流：

```text
collectSet NO_REF:
  ref -> Region/slot -> stable alloc bit -> local mark batch -> return

collectSet HAS_REF:
  ref -> Region/slot -> stable alloc bit -> local mark batch
      -> Region DIRTY/QUEUED
      -> Region owner 批量扫描 alloc & mark & ~scan

ACTIVE NO_REF:
  ref -> Region/slot -> return

ACTIVE HAS_REF:
  ref -> Region/slot/object-header validation -> local visit batch
      -> Region DIRTY/QUEUED(role=EXTERNAL_ACTIVE)
      -> Region owner 批量扫描 visit & ~scan
      -> 永不进入本轮 Sweep
```

collectSet `NO_REF` 不读取对象头和 TypeInfo；ACTIVE 不读取正在变化的 live alloc
plane，但 HAS_REF white target 为建立可靠 visit bit 仍做对象头合法性检查。

### 1.4 当前实现：基于逐对象 WBuf 的 Mark

当前实现以“灰对象”为调度单位。mark bitmap 负责回答对象是否已经发现，`WorkBuf`
负责保存“已经标记、但引用字段尚未扫描”的 `HAS_REF` 对象。完整流程如下：

```text
PreEnumRoots
  -> 枚举 Root
  -> DiscoverObject(root)
       -> ResolveMarkTarget
       -> Region/slot 合法性检查
       -> 读取 stateWord/TypeInfo
       -> atomic 设置对象 mark bit
       -> NO_REF：结束
       -> HAS_REF 且本线程第一次置位：PushGrey/PutObj
  -> Root 阶段产生的灰对象转移到 globalFull_

Main Mark / Helper / Assist
  -> 从 globalFull_ 或本地缓存取得 WorkBuf
  -> 逐个弹出灰对象
  -> 读取对象 TypeInfo
  -> ForEachRefFieldGCInline / ForEachRefFieldGCKnownType
  -> 对每个子引用再次调用 DiscoverObject
  -> 新发现的 HAS_REF 对象继续放入 WorkBuf
  -> 当前 WorkBuf 耗尽后获取下一批

并发写屏障
  -> SATB 记录被覆盖的旧引用
  -> 尝试标记旧引用指向的对象
  -> 普通 HAS_REF mark winner 写入 mutator 的 activeLog_ WorkBuf
  -> GC worker 接管、合并并继续逐对象扫描

终止
  -> 发布各线程私有 WorkBuf 和 barrier log
  -> 反复检查 globalFull_、partial buffer 和本地工作
  -> 所有灰对象均被消费后结束 Mark
  -> Sweep 根据 mark bitmap 判断 collectSet 对象存活性
```

该模型中的两类状态彼此分离：

| 状态 | 表示方式 |
|---|---|
| 白/已发现 | Region 的 mark bitmap |
| 已发现但未扫描 | 对象地址是否仍位于某个 `WorkBuf` |
| 已扫描 | 对象已从 `WorkBuf` 弹出并完成字段遍历；没有独立 scan bit |

因此，当前 mark bit 的第一次置位线程必须立即决定对象是否含引用，并把需要扫描的
对象地址可靠地发布到 WBuf。若只置 mark bit 而漏掉 WBuf 发布，对象会表现为
“已经存活”，但它的子引用不会进入对象图遍历。

WBuf 模型的本质是：

```text
对象图边发现
  -> 对象级去重
  -> 对象级入队
  -> 对象级出队
  -> 对象级扫描
```

它不要求某个 worker 拥有整个 Region。不同 worker 可以同时扫描同一 Region 中的
不同对象，负载均衡粒度较细；代价是每个存活 `HAS_REF` 对象都会产生对象地址传递、
WorkBuf 写入/读取以及相对分散的对象元数据访问。

### 1.5 目标实现：基于 Region Scan 的 Mark

目标实现仍然从 Root 出发遍历真实对象图，并不是预先扫描整个 Region 来猜测哪些对象
存活。变化只发生在“对象被发现以后，如何记录和调度待扫描工作”这一层：

```text
对象图边发现
  -> 按 Region 聚合 mark/visit bit
  -> Region 级发布待扫描状态
  -> Region owner 批量找出已标记且未扫描的对象
  -> 对象级字段扫描
```

PreEnum 先确定本轮边界：

- `SEALED` Central Region 进入 `COLLECT_SET`，其 alloc plane 在本轮保持稳定；
- PreEnum 时仍为 TLS `ACTIVE` 的 Region 不进入 collectSet，本轮不能 Sweep；
- Root 闭包仍允许穿过 ACTIVE `HAS_REF` 对象，防止它们成为通向 collectSet 的断点；
- Region 的 `scanClass` 已在分配时固定，热路径不再逐对象询问是否含引用。

完整 Mark 流程如下：

```text
Root / 对象字段 / SATB 旧引用
  -> DiscoverRef(ref)
  -> 定位 Region、slot 和本轮 role
  -> COLLECT_SET：使用稳定 alloc plane 验证对象
     ACTIVE HAS_REF：使用对象头做必要的合法性验证
  -> 写入 worker-local MarkBatch

MarkBatch flush
  -> 按同一 Region、同一 bitmap word 合并 bit mask
  -> 一次 atomic fetch_or 更新 mark/visit word
  -> 计算本次真正由 0 变 1 的 newly-discovered bits
  -> NO_REF：到此结束
  -> HAS_REF 且存在 newly-discovered bits：
       更新 pending-word summary
       把 Region 状态发布为 DIRTY/QUEUED
       必要时只发布一个 Region token

Region owner
  -> worker 取得 Region token 和该 Region 的扫描权
  -> 按预算处理 pending bitmap words
  -> COLLECT_SET 候选 = alloc & mark & ~scan
     ACTIVE 候选     = visit & ~scan
  -> 对候选对象读取 TypeInfo 并扫描引用字段
  -> 发现的子引用重新进入当前 worker 的 MarkBatch
  -> 批量设置对应 scan bits
  -> 达到 WorkUnit 预算时主动让出并重新发布剩余工作
  -> 当前无候选时清除 DIRTY；若并发发现了新 bit，则再次发布 token

终止
  -> flush 所有 worker-local MarkBatch 和 barrier batch
  -> Region queue、特殊对象队列、控制状态和本地 batch 全部为空
  -> 所有 HAS_REF Region 满足“已发现集合中没有未扫描对象”
  -> Sweep 只处理本轮 collectSet
```

这里的 Region owner 只拥有“当前 slice 的扫描权”，不拥有对象的发现权，也不会无条件
反复遍历整个 Region：

- 任何 worker 都可以发现该 Region 的新对象并原子更新 mark/visit plane；
- 同一时刻只有 owner 修改该 Region 的 scan plane 和消费 pending words；
- owner 扫描的仍是 bitmap 选出的具体对象，并仍需读取这些对象的 TypeInfo 来解释字段；
- owner 扫描对象发现的子引用放入自己的 MarkBatch，而不是直接递归扫描子对象；
- owner 达到对象数、bitmap word 数或时间预算后让出，以免一个大 Region 长期独占 worker；
- owner 清理 DIRTY 与发现者发布新工作之间必须使用带版本或状态复查的协议，避免丢唤醒。

目标模型不是完全删除所有对象级队列，而是按工作类型分流：

| 对象/工作类型 | 最终调度方式 |
|---|---|
| 普通 small `NO_REF` | 只批量更新 mark，不产生扫描任务 |
| 普通 small `HAS_REF` | Region token + Region owner 扫描 |
| LARGE 对象 | RangeQueue 或保留的 ObjectQueue |
| WeakRef、Finalizer 等特殊语义 | 专用队列 |
| 兼容或异常回退路径 | 可保留受控的对象级队列 |

因此，最终应删除的是“普通 small `HAS_REF` 对象一对象一 WBuf entry”的主路径，
而不是强行把所有 GC 工作都塞进同一种 Region token。

### 1.6 两套实现的差异、优缺点与最终选择

两套实现都必须从 Root、SATB 和扫描到的子引用出发，沿真实引用边求可达闭包；
Region Scan 不改变存活判定，也不会把 Region 内尚未发现的对象直接标活。核心差别是
灰色状态的表示和扫描任务的调度粒度：

| 维度 | 当前 WBuf Mark | 目标 Region Scan Mark |
|---|---|---|
| 对象图遍历 | 从 Root/SATB 沿字段逐对象发现 | 相同 |
| 发现去重 | 每个对象 atomic 设置 mark bit | MarkBatch 合并同 word 后 atomic 设置 mark/visit bit |
| 灰色状态 | `mark=1` 且对象地址仍在 WBuf | `mark/visit=1 && scan=0` |
| 普通扫描任务 | 一个 `HAS_REF` 对象一个队列项 | 一个 DIRTY Region 一个 token，内部由 bitmap 选对象 |
| Region 内并行 | 多 worker 可同时扫描不同对象 | 同时只有一个 owner；通过短 slice 在 worker 间迁移 |
| 新发现对象发布 | mark winner 立即发布对象地址 | newly-discovered bit 更新 summary，并按状态发布 Region token |
| 字段解释 | 出 WBuf 后读取对象 TypeInfo | owner 选出对象后读取对象 TypeInfo；这一步不会消失 |
| mark 写入 | 引用发现点通常逐对象 atomic RMW | 同 Region/word 的多个发现合并为一次 atomic RMW |
| scan 完成记录 | 无独立 bit，依赖对象已从 WBuf 消费 | scan plane 显式记录，可直接验证未扫描集合 |
| 数据局部性 | 队列顺序近似对象图顺序，地址较分散 | 同一 Region、相邻 bitmap word 和对象连续处理 |
| 队列流量 | 与存活 `HAS_REF` 对象数近似成正比 | 与活跃 Region 数和重新发布次数相关 |
| 负载均衡 | 对象级，天然较细 | Region slice 级，必须有 WorkUnit 预算 |
| 终止检测 | 依赖全局/私有 WBuf、barrier log 全部发布并排空 | 依赖本地 batch、Region 状态、token 和 bitmap 不变量共同收敛 |
| 元数据成本 | alloc/mark bitmap + WBuf 存储 | alloc/mark/scan bitmap + summary/owner/token 状态 |
| 稀疏存活 | 只处理已入队对象，通常有利 | 若 summary 过粗，可能检查较多空 bitmap word |
| 高密度/大堆存活 | 对象任务和随机元数据访问多 | 批量位操作和 Region 局部扫描更有利 |
| LARGE/特殊对象 | 可直接作为对象任务处理 | 仍需 RangeQueue/ObjectQueue/专用队列 |

#### 当前 WBuf 的优点

1. **实现和正确性模型直观。** mark winner 发布唯一灰对象，worker 消费后扫描；
   对象从灰到黑的状态迁移容易理解。
2. **负载均衡粒度细。** worker 按对象取工作，不会因为一个引用密集的大 Region
   长时间占用扫描权。
3. **适合稀疏对象图。** 队列中只有真正已经发现且需要扫描的对象，不需要检查
   Region 中的无关 bitmap word。
4. **适合特殊对象。** LARGE、WeakRef、Finalizer 等需要独立语义的对象可以自然地
   作为专用对象任务处理。

#### 当前 WBuf 的缺点

1. **任务数量随存活 `HAS_REF` 对象数增长。** 每个对象都要传递地址、写入队列并再次
   读取，数百万对象时队列流量和同步成本明显。
2. **局部性受对象图形状支配。** 父子对象可能分散在不同 Region，连续出队时会频繁
   访问不同 Region bitmap、对象头和 TypeInfo。
3. **mark bit 难以批量更新。** mark winner 同时承担对象级发布职责，容易把发现路径
   固定成逐对象 atomic RMW。
4. **扫描完成状态不显式。** 正确性依赖所有私有/全局 WBuf 与 barrier log 的发布和
   排空，终止协议容易出现隐藏工作。

#### 目标 Region Scan 的优点

1. **减少任务和队列流量。** 同一 Region 内大量新对象可以由一个 token 和若干
   bitmap word 表达，不再逐对象传递地址。
2. **提高 Bitmap 更新效率。** MarkBatch 可把同 word 的多个 bit 合并成一次 atomic
   `fetch_or`，并直接得到 newly-discovered mask。
3. **提高缓存与 TLB 局部性。** owner 连续访问同一 Region 的 alloc/mark/scan plane、
   对象和相关元数据，减少跨 Region 往返。
4. **灰色状态可验证。** `mark/visit & ~scan` 直接表示待扫描集合，便于重新发布、
   终止检测和诊断漏扫。
5. **与 `NO_REF/HAS_REF` 分离形成组合收益。** `NO_REF` 完全跳过扫描调度，
   `HAS_REF` 才进入 Region Scan；大规模叶子对象只承担定位和批量 mark 成本。

#### 目标 Region Scan 的缺点与约束

1. **并发协议更复杂。** 必须正确处理 MarkBatch flush、DIRTY/QUEUED 状态、owner
   交接、清 DIRTY 与并发发现之间的竞态及终止复查。
2. **同一 Region 的瞬时并行度降低。** 一个 Region 同时只有一个 owner；必须用短
   WorkUnit slice 和跨 Region 调度避免热点 Region 拖慢尾部。
3. **增加元数据。** scan plane、pending-word summary、owner/token 状态都会占用
   Region 元数据和维护带宽。
4. **稀疏工作可能不划算。** 若一个 Region 只有少量存活对象，而 summary 或 slice
   设计过粗，bitmap 搜索成本可能高于直接消费对象地址。
5. **不能消除对象语义扫描。** Region 只负责快速找出候选对象；真正扫描字段仍要
   读取对象 TypeInfo。该优化主要减少发现、发布和定位开销，不会把引用对象变成
   `NO_REF`。

#### 最终选择

Step60 采用混合设计：

```text
small NO_REF
  -> MarkBatch
  -> mark plane
  -> 不产生扫描任务

small HAS_REF
  -> MarkBatch
  -> mark/visit plane + pending summary
  -> RegionScanQueue
  -> owner 按 WorkUnit 扫描

LARGE / WeakRef / Finalizer / 特殊回退
  -> RangeQueue、ObjectQueue 或专用队列
```

这不是把 WBuf 的名字改成 RegionQueue，而是把普通 small `HAS_REF` 的灰工作表示从
“对象地址集合”改为“Region 内 `mark/visit & ~scan` 的 bit 集合”。实施时必须一次性
切换这条主路径的生产者、消费者和终止协议，不能让同一个普通 small 对象同时依赖
WBuf 和 scan plane 表示灰色，否则会产生重复扫描和无法证明的终止条件。

## 2. 优化一：NO_REF/HAS_REF Region 分离

### 2.1 要解决的问题

当前 Region 只知道 size class，不知道其中对象是否含引用。因此发现任意白对象时，
GC 必须读取对象 TypeInfo 才能回答：

```text
这个对象标活后，是否还需要扫描字段？
```

对 `Array<Byte>` 来说，答案永远是否，但当前 Runtime 仍对数百万个对象重复询问。

### 2.2 最终分类

新增唯一 Region 级分类：

```cpp
enum class MSScanClass : uint8_t {
    NO_REF = 0,
    HAS_REF = 1,
};
```

分类函数在分配入口执行一次：

```cpp
inline MSScanClass ClassifyScan(const TypeInfo* typeInfo)
{
    return typeInfo->HasRefField()
        ? MSScanClass::HAS_REF
        : MSScanClass::NO_REF;
}
```

数组保持现有 `TypeInfo::HasRefField()` 语义：

- `Array<Byte>`、`Array<UInt64>`、不含引用的 struct array：`NO_REF`；
- 引用数组、含引用的 struct array：`HAS_REF`；
- WeakRef、Finalizer 和需要 GC 特殊处理的对象强制进入 `HAS_REF`。

四项 Mark 热路径只改变 small object。LARGE 保留 base Region 的单对象分类和特殊扫描
路径，但必须服从同一个 PreEnum collectSet/excludedSet 周期边界。

### 2.3 Region 和分配器结构

`MSRegionInfo` 增加不可变的 `scanClass`。非空 Region 的 key 为：

```text
(sizeClass, scanClass)
```

分配侧结构改为：

```cpp
static constexpr size_t kScanClassCount = 2;

MSRegionInfo* activeRegions_[kNumSizeClasses * kScanClassCount];

inline size_t ActiveIndex(uint8_t sizeClass, MSScanClass scanClass)
{
    return static_cast<size_t>(sizeClass) * 2 +
        static_cast<size_t>(scanClass);
}
```

Central pool 的 partial/full swept/unswept 容器也按 `[36][2]` 分桶。完全为空且已经
清除对象和 Bitmap 的 `emptySweptCache_` 继续保持 classless；从 empty cache 取出后，
在发布为 BUMP Region 前重新写入 `(sizeClass, scanClass)`。只要 Region 非空，
`scanClass` 就不得改变。

这样既保证 Region 内分类纯净，也不把完全空 Region 永久锁死在某个 scan class。

### 2.4 分配调用链

所有 MSGC small allocation 必须把 `TypeInfo*` 传到 AllocBuffer：

```cpp
MAddress AllocBufferMS::Allocate(const TypeInfo* typeInfo, size_t size)
{
    uint8_t sizeClass = SizeClassInfo::SizeToClass(size);
    MSScanClass scanClass = ClassifyScan(typeInfo);
    return FastAlloc(typeInfo, sizeClass, scanClass);
}
```

Runtime 路径包括：

- `MCC_NewObject`；
- `MCC_NewArray*`；
- `MCC_NewArrayGeneric*`；
- `ObjectManager::NewObject/NewArray`；
- Runtime 内部 raw/generic small object 分配。

LLVM 静态分配把 `scanClass` 作为函数属性或立即数；动态数组根据 TypeInfo/component
TypeInfo 计算。active Region 索引统一为：

```text
activeIndex = sizeClass * 2 + scanClass
```

因此 Runtime 和 AArch64/X86 emitter 必须在同一 SDK 中切换 72-entry 布局。

### 2.5 Mark 路径的独立变化

Region 分类完成后，mark winner 不再调用 `typeInfo->HasRefField()`：

```text
winner
  -> region.scanClass == NO_REF  -> 完成
  -> region.scanClass == HAS_REF -> 发布后续扫描工作
```

这一项单独实现时可以消除 winner 之后的 `HasRefField()` 判断，并保证 WorkBuf 只承载
`HAS_REF`。但在没有优化二的 stable alloc plane 前，白 slot 为了防止 stale edge
仍须读取 stateWord/TypeInfo 验证对象存在，因此单独做优化一还不能实现
“NO_REF 完全不触碰对象”。

### 2.6 正确性条件

1. `scanClass` 在 Region 从空到非空之前设置，以 release 发布；
2. 对象地址对外可见前，Region 分类已经可见；
3. non-empty Region 不得重新分类；
4. Sweep 后只要还存在一个 live slot，就保持原分类；
5. 完全空 Region 清除全部 slot/Bitmap 后才能回 classless cache；
6. Finalizer/WeakRef 等特殊对象不能错误进入 `NO_REF`；
7. CopyGC 分配路径不使用该字段。

### 2.7 独立收益、代价和指标

**直接收益**

- 每个 mark winner 少一次 `HasRefField()`；
- 叶子 winner 不进入 WorkBuf；
- 后续可以为 `NO_REF` 建立完全不读对象的发现路径；
- 为 Region 级扫描提供“整个 Region 都需要扫描”的前提。

**代价**

- 每个 P 的 active Region 指针从 36 增至 72；
- 低负载、多 size class 场景可能多保留一个半空 Region；
- Central bucket 数量翻倍；
- AllocBuffer 与 compiler fast-path ABI 必须同步变化。

**必须记录**

```text
allocatedNoRefObjects / allocatedHasRefObjects
activeNoRefRegions / activeHasRefRegions
noRefRegionOccupancy / hasRefRegionOccupancy
noRefMarkWinners / hasRefMarkWinners
specialObjectForcedHasRef
```

## 3. 优化二：三平面 Bitmap

### 3.1 要解决的问题

当前 small Region 只有 allocator-private alloc bitmap 和 concurrent mark bitmap。
GC 不读取 alloc bitmap，因为 PARTIAL/BITMAP Region 可能正由分配器普通写；白对象只能
通过对象 stateWord/TypeInfo 判断 slot 是否有效。

Region 级扫描又需要区分：

```text
对象已经标活
对象的引用扫描职责已经处理
```

仅有 mark bit 无法表达这两个状态。

### 3.2 三个平面的唯一语义

| 平面 | bit=0 | bit=1 | 主要写入者 |
|---|---|---|---|
| alloc | slot 空闲/不存在对象 | slot 存在当前 Region 代的合法对象 | ACTIVE 独占分配器、自然归还物化、SEALED Sweep |
| mark | 本轮尚未发现 | collectSet：正式存活；ACTIVE：临时 visit | GC main/helper/assist/barrier |
| scan | 扫描职责尚未完成 | 对应 role 的对象字段已扫描完成 | Region owner |

三个物理平面不能合并，但 mark/scan 必须按 Region role 解释：

- alloc 回答“这里有没有对象”；
- collectSet 的 mark 回答“对象是否正式存活”，scan 回答“存活对象是否完成扫描”；
- ACTIVE 的 mark 临时回答“对象是否已被闭包访问”，scan 回答“已访问对象是否完成扫描”。

Region owner 的核心表达式按 token role 分流：

```text
COLLECT_SET:     toScan = alloc & mark & ~scan
EXTERNAL_ACTIVE: toScan = visit(mark) & ~scan
```

只有 `COLLECT_SET` 要求 `mark ⊆ alloc`。`EXTERNAL_ACTIVE` 不读取正在变化的 ACTIVE
alloc plane；visit bit 在写入前通过地址、slot alignment 和对象
`stateWord/TypeInfo` 校验合法性。

### 3.3 固定布局

32 KiB Region 的最小 slot 为 8B，最大 4096 个 slot，因此每个平面固定 4096 bit，
即 512B：

```text
allocWord(regionIdx, wordIdx)
  = allocPlaneBase + regionIdx * 512 + wordIdx * 8

markWord(regionIdx, wordIdx)
  = markPlaneBase  + regionIdx * 512 + wordIdx * 8

scanWord(regionIdx, wordIdx)
  = scanPlaneBase  + regionIdx * 512 + wordIdx * 8
```

`MSBitmapArena` 分别预留三段虚拟地址，Runtime 和 LLVM 都从
`MSGCFastPathState` 取得三个 base。`MSRegionInfo` 不再保存 alloc/mark 指针，
避免每次发现先追逐 RegionInfo 中的 Bitmap 地址。

只访问：

```text
bitmapWords = ceil(totalSlotCount / 64)
```

其余 word 和尾部无效 bit 永远保持 0。

### 3.4 内存代价

逻辑地址开销：

```text
3 * 512B / 32KiB = 4.6875%
```

相比当前两平面固定 arena slot，新增 scan plane 的逻辑上限是 1.5625% heap。

三个平面按独立 OS page/chunk 预留并延迟触页。`NO_REF` 代码绝不访问 scan plane，
但 OS 只能按页提交：macOS 16 KiB 页会覆盖 32 个 512B scan slot，因此不能声称
每个 NO_REF Region 都独立节省 512B RSS。实际常驻量必须以
`scanCommittedPages` 统计，不能用 `hasRefRegionCount * 512` 代替。

### 3.5 alloc plane 的并发协议

#### 3.5.1 当前实现到底做了什么

当前实现已经具备“ACTIVE Region 不进入本轮 Sweep”的主体结构，但还没有形成可直接
供无对象读取 Mark 使用的 alloc plane 协议：

1. `AllocBufferMS::activeRegions_[36]` 中的 `BUMP/BITMAP` Region 由 owner P 独占；
2. BITMAP 分配用普通 load/store 修改 `allocBitmap`，BUMP 分配只推进 `allocPtr`，
   不在热路径实时物化 alloc bits；
3. `PreEnumRoots()` 依次执行：

   ```text
   StopAndWaitForAllSweep()
     -> STW
     -> ClearAllMarkBitmaps()
     -> ReclassifyUnsweptToSwept()
     -> PrepareForSweep()
   ```

4. 当前 PreEnum **不会**调用 `ReturnAllRegionsToCentral()`，不会 flush TLS active Region；
5. `ClearAllMarkBitmaps()` 会遍历 TLS active Region，将仍为 `sg-2` 的 Region 归一化为
   当前 `sg`，然后清它的 mark bitmap，但不会为它构建稳定 allocBitmap；
6. `PrepareForSweep()` 只把当时已经位于 Central swept lists 的 Region 翻转到
   unswept lists，然后令 `globalSweepGen += 2`；
7. ACTIVE Region 自然耗尽或归还时，`ReturnRegionToCentral()` 先把它的 `sweepGen`
   设置为当前 `globalSweepGen`，`PrepareForCentralReturn()` 只在 BUMP partial 的归还
   路径物化 allocBitmap，再放入当前 swept list；
8. 因为 swept→unswept 的翻转已经发生，这个归还 Region 不参加正在进行的 Sweep，
   到下一轮 PreEnum 才可能进入回收集合；
9. `ReturnAllRegionsToCentral()` 当前只用于 AllocBuffer 退出清理，不是 GC 周期协议；
10. 当前 `MarkSweepCollector::MarkNewObject()` 在
    MarkSetup/ConcurrentMark/MarkTermination 中会执行 `MarkObject()`，并把 HAS_REF
    对象放入 deferred 队列；但源码中的实际调用点只有
    `BaseObject::OnFinalizerCreated()` 和 `FinalizerWorkList`，普通 small/LARGE 分配
    路径并未调用它。函数名和注释仍保留 mark-at-alloc 说法，实际承担的是 Finalizer
    相关特殊闭包，不能把它误写成当前所有新对象的分配协议。

`PrepareForSweep()` 中“`FlushAllAllocBuffers returns TL regions to swept`”的注释与实际
调用链不一致：当前 `PreEnumRoots()` 没有该调用。实现最终协议时应同步删除或改正这条
旧注释，不能据此误判当前已经 flush。

因此当前实现的真实状态是：

```text
Central PARTIAL/FULL:
  occupancy 稳定，可以成为本轮 Sweep 集合

TLS BUMP/BITMAP:
  occupancy 仍由 owner 修改，不在本轮 Sweep 集合
  GC 当前仍清它的 mark bitmap；对象依靠 generation 排除而不参加本轮 Sweep
  普通分配路径当前没有 MarkNewObject 调用
  但不能把其 allocBitmap 当作并发只读快照
```

#### 3.5.2 最终协议的两个集合

最终协议不强制封存 TLS Region，而是在 PreEnum 定义两个互斥集合：

```text
collectSet:
  PreEnum 边界时已经位于 Central swept lists、
  随后被翻转为 unswept 的 SEALED Region

excludedSet:
  PreEnum 时仍由 TLS owner 持有的 ACTIVE Region
  + PreEnum 后新取得/新建的 Region
  + PreEnum 后自然归还 Central swept lists 的 Region
```

`SEALED` 是 GC 生命周期状态，不要求复用 `MSRegionType` 的数值。当前
`PARTIAL/FULL + 位于本轮 unswept list + cycle generation 匹配` 可以共同表达
collectSet；`BUMP/BITMAP + TLS owner` 表达 ACTIVE。

核心规则：

```text
GC 只读取 collectSet 的 alloc plane
GC 在 collectSet 写正式 mark/scan，在 ACTIVE HAS_REF 写临时 visit/scan
Sweep 只处理 collectSet
分配器只写 excludedSet 中 ACTIVE Region 的 occupancy，不写 visit/scan
```

因此 alloc plane 的稳定性来自**所有权分离**，不是把全堆 allocBitmap 复制一份：

- SEALED Region 已经离开 AllocBuffer，没有分配者再写 occupancy；
- ACTIVE Region 的 live allocBitmap 继续由 owner 普通写，GC 永远不并发读取；
- 不构建全局 allocBitmap 快照，也不把每次分配改为 atomic RMW。

#### 3.5.3 PreEnum STW 中要做的事

PreEnum 保留一次短 STW，但不 detach、不 refill、不强制 materialize active Region：

```text
1. StopAndWaitForAllSweep
2. STW 停止 mutator
3. ReclassifyUnsweptToSwept，封闭上轮残余分类
4. 确定本轮 collectSet，并记录 cycleSweepGen/collectionEpoch
5. 清 small collectSet 的正式 mark plane；清 large collectSet 的 LARGE_MARKED
6. 清 ACTIVE HAS_REF Region 的临时 visit/scan bits 和 Region control/summary
7. 清 collectSet HAS_REF Region 的 scan plane/control/summary
8. 将 Central swept lists 翻转为本轮 unswept lists
9. globalSweepGen += 2
10. 恢复 mutator；原 ACTIVE Region 仍由原 owner 继续分配
```

ACTIVE 的 alloc plane 仍由 owner 修改，GC 不读取也不清理它。GC 只清 ACTIVE
`HAS_REF` 的 mark/scan plane：allocator 不写这两个平面，因此 STW 后它们可以安全地
作为本轮外部遍历状态。`NO_REF` ACTIVE 不需要 visit/scan bits，不主动触页 scan plane。

#### 3.5.4 GC Root 闭包如何穿过 ACTIVE Region

GC Root 本来就能覆盖所有存活对象，前提是发现 ACTIVE 对象时不能直接终止遍历：

```text
GC Root -> ACTIVE 对象 A -> SEALED/collectSet 对象 B
             │
             └─ A 本轮不回收，但必须继续扫描字段
                                      │
                                      └─ 正常标记 B
```

因此 ACTIVE 不是新的 Root，也不需要枚举整个 ACTIVE Region。最终 Discover 分成两个
语义明确的分支：

```text
SEALED/collectSet target:
  stable alloc bit 验证
  -> 正式 mark bit
  -> HAS_REF 发布 role=COLLECT_SET 的 Region token
  -> 本轮允许 Sweep

ACTIVE/excludedSet target:
  不读取 live allocBitmap
  -> 地址/slot/stateWord/TypeInfo 合法性检查
  -> NO_REF：直接返回
  -> HAS_REF：设置临时 visit bit
  -> winner 发布 role=EXTERNAL_ACTIVE 的 Region token
  -> 本轮禁止 Sweep
```

ACTIVE 的 mark/scan plane 只复用物理存储，不复用 SEALED 的回收语义：

```text
ACTIVE mark bit = 本轮已经从 Root/引用闭包发现并发布扫描
ACTIVE scan bit = 本轮字段扫描已经完成

SEALED mark bit = 本轮存活
SEALED scan bit = 本轮存活对象字段扫描完成
```

所以 ACTIVE 只要求：

```text
activeScan ⊆ activeVisit
```

不要求 `activeVisit ⊆ alloc`，因为 BUMP ACTIVE 不实时物化 alloc plane。ACTIVE target
由实际引用提供，white 路径仍通过 slot alignment 和对象 `stateWord/TypeInfo` 验证；
这部分对象头访问只发生在数量有界的 ACTIVE 引用路径，不影响 collectSet 中数百万
NO_REF 叶子对象的无对象读取热路径。

#### 3.5.5 ACTIVE External Scan 如何接入 Region 级扫描

不新建独立 external queue。统一 `RegionScanQueue` 的 token 增加角色：

```cpp
enum class MSRegionScanRole : uint8_t {
    COLLECT_SET,
    EXTERNAL_ACTIVE,
};
```

两种 owner 使用同一套 `QUEUED/OWNED/DIRTY`、pending summary 和 outstanding 协议：

```text
COLLECT_SET:
  toScan = alloc & mark & ~scan
  扫描结果写正式 scan bit
  Sweep 消费 alloc & mark

EXTERNAL_ACTIVE:
  toScan = visit(mark plane) & ~scan
  不读取 ACTIVE live alloc plane
  扫描结果写临时 scan bit
  Sweep 永远忽略
```

ACTIVE mark winner 已经完成地址、slot 和对象头合法性检查，因此 owner 只枚举 visit
winner 对应的对象。普通对象扫描强引用字段；WeakRef、Finalizer 和特殊对象转入既有
特殊闭包，不能把弱边升级为强边。

Region 从 TLS 自然归还 Central，甚至在本轮被另一个 P 再次取得，都不改变
`EXTERNAL_ACTIVE` 角色：它的 `sweepGen` 仍是当前 excluded generation，本轮不会 Sweep。
token 必须携带 `collectionEpoch + regionGeneration + role`，防止空 Region 重用后把旧
visit 工作应用到新 Region。

#### 3.5.6 PreEnum 后新对象不需要 allocation-time MarkNewObject

PreEnum 后的新对象位于 excludedSet，不需要在分配时标记或主动加入扫描队列：

```text
不调用 MSGC allocation-time MarkNewObject
不因为“刚分配”设置正式 mark bit
不因为“刚分配”发布 deferred scan
```

但“无需 allocation-time MarkNewObject”不等于“任何时候都不扫描新对象”：

```text
新对象没有被 Root/引用闭包发现
  -> 本轮不扫描

新对象被 Root/引用闭包发现
  -> 按 EXTERNAL_ACTIVE 访问一次
  -> HAS_REF 时继续扫描其字段
  -> 仍不参与本轮 Sweep
```

这样 GC Root 的传递闭包可以自然穿过 ACTIVE 新旧对象，不需要记录 PreEnum 时的
occupancy 副本，也不会把不可达 ACTIVE 对象及其子图保活。

SATB old-edge barrier 继续保留：并发扫描期间被覆盖的旧边在覆盖前被 shade；新插入边
来自 mutator 已持有的引用，并会由 Root/已有闭包或 old-edge 来源保住。所有 FFI/native
强引用仍必须遵守既有 Root 注册协议。

该规则同时覆盖 small 和 LARGE：PreEnum 后建立的 LARGE primary 记录当前
allocation/sweep generation，从本轮 large collectSet 排除；若它被 Root/闭包发现，
走 LARGE external scan，而不是仅靠 `AtomicSetMarked()` 把“新大对象存活”混入正式
mark 状态。

CopyGC 的通用 `Collector::MarkNewObject()` ABI 可以保留。MSGC 普通分配路径当前已经
没有该调用，最终实现要把这个约束固化为 contract，禁止后续重新引入
allocation-time mark。现有 `BaseObject::OnFinalizerCreated()` 和
`FinalizerWorkList` 调用不是普通分配语义，应改用独立
`MarkFinalizableObject()`、`MarkResurrectedObject()` 或既有特殊闭包入口。

#### 3.5.7 ACTIVE 可以连续跨越任意多个周期

不设置“最多延后一轮”，也不在下一轮强制 detach/materialize：

```text
PreEnum 时仍 ACTIVE
  -> 清临时 visit/scan 状态
  -> 继续留在 TLS
  -> 再等下一轮

自然耗尽/主动归还
  -> SetSweepGen(current globalSweepGen)
  -> BUMP partial 在 PrepareForCentralReturn 中物化 alloc plane
  -> 进入当前 Central swept list
  -> 本轮仍排除
  -> 下一轮 PreEnum 翻转为 unswept/SEALED
  -> 下一轮才进入 Mark + Sweep
```

这会保留有界浮动垃圾，理论上限约为
`P * activeClassCount * RegionSize`；按 72 类、100 P、32 KiB Region 计算约
225 MiB，实际取决于非空 active class。它换来零强制 TLS flush、零批量 refill、
更短 PreEnum STW 和更好的分配局部性。

本 Step 对 OOM GC 也不增加 ACTIVE 强制归还例外：OOM 只能回收 collectSet 和其他已经
封存的候选，不能指望回收仍在 TLS ACTIVE 中的死亡 slot。若未来需要 emergency detach，
应作为独立 OOM 策略设计，不能偷偷破坏这里的并发协议。

#### 3.5.8 该协议的意义

1. SEALED 引用发现用 alloc bit 判断 slot 存在，不再读取白对象
   `stateWord/TypeInfo`；
2. 分配热路径继续使用 owner-only 普通写，没有 atomic occupancy 成本；
3. GC 与 allocator 永远不并发访问同一个 ACTIVE alloc word，消除 C++ 数据竞争；
4. 不需要在 PreEnum 构建全堆 allocBitmap 快照；
5. 不强制回收 TLS Region，避免 STW detach、Central 锁压力、恢复后的 refill 风暴和
   active Region 局部性丢失；
6. collectSet 在周期边界一次冻结，Mark、Termination 和 Sweep 使用同一份成员关系，
   不再依赖“对象是否刚好被 MarkNewObject”判断能否回收。

#### 3.5.9 对后续步骤的直接影响

| 后续组件 | 必须遵守的变化 | 不遵守的后果 |
|---|---|---|
| NO_REF/HAS_REF Region 分离 | ACTIVE NO_REF 直接返回；只有 ACTIVE HAS_REF 产生 external scan | 为叶子 ACTIVE 对象制造无效 visit/scan 工作 |
| 三平面 Bitmap | collectSet 使用正式 mark/scan；ACTIVE HAS_REF 临时复用两平面表达 visit/scan，不读取 ACTIVE alloc | 把临时 visit 当存活，或与 owner 的 alloc 普通写竞争 |
| Discover/Mark Batch | 先判 Region role；collectSet 校验 alloc，ACTIVE 校验对象头并提交 visit batch | Root 闭包在 ACTIVE 处截断，或错误读取变化中的 alloc word |
| Region 级扫描 | 同一 RegionScanQueue 支持 `COLLECT_SET/EXTERNAL_ACTIVE` 两种 role | ACTIVE 指向 SEALED 的边丢失，或 ACTIVE 被误纳入 Sweep |
| MarkTermination | fixed point 同时检查两种 role 的 token、pending、owner 和 `visit & ~scan` | ACTIVE 外部闭包尚未完成就开始 Sweep |
| Sweep | 只消费本轮 collectSet token/list；不能重新按 RegionType 遍历全堆决定成员 | 自然归还或新分配 Region 被本轮误回收 |
| Large RangeQueue（Step61） | 只拆分 collectSet LARGE；边界后 LARGE 由 generation 排除 | 新大对象被错误加入本轮 RangeQueue/Sweep |
| WorkUnit Pacer/Assist（Step61） | External Active Scan 计入闭包 work；excluded 分配本身不产生 mark-at-alloc debt | assist 债务把 mutator 迫使去处理尚未被发现的新对象 |
| Old/Immutable Region（Step61） | immutable 快路仍以 collectSet membership 为前置条件 | 跨 epoch 复用旧 bitmap/扫描结论 |

#### 3.5.10 源码级改造点

| 当前函数/结构 | 当前行为 | 最终改造 |
|---|---|---|
| `MarkSweepCollector::PreEnumRoots()` | Stop Sweep 后在 STW 清 bitmap、reclassify、PrepareForSweep | 冻结 collectSet；清 ACTIVE HAS_REF 临时 visit/scan 状态；不调用 TLS flush |
| `CentralRegionPool::ClearAllMarkBitmaps()` | 同时清 Central 和 TLS active mark bitmap，并归一化 active sweepGen | Central mark 作为正式存活状态；ACTIVE HAS_REF mark 作为 visit 状态清零，并同步清 scan/control/summary |
| `CentralRegionPool::PrepareForSweep()` | Central swept→unswept，随后 `globalSweepGen += 2` | 保留代际翻转，但返回显式 `MSCollectionSet`/cycle token，供 Mark、batch、Sweep 共用 |
| `AllocBufferMS::ReturnRegionToCentral()` | 设置当前 sweepGen，归还 PARTIAL/FULL/EMPTY | 保留；明确“归还到当前 swept list、本轮排除、下一轮纳入”，不得因跨轮数强制调用 |
| `MSRegionInfo::PrepareForCentralReturn()` | BUMP partial 在归还时物化 allocBitmap | 保留并成为 BUMP 持久 alloc plane 的唯一物化边界；PreEnum 不物化 ACTIVE BUMP |
| `MarkSweepCollector::MarkNewObject()` 及其调用点 | 普通分配路径不调用；`OnFinalizerCreated()`/`FinalizerWorkList` 在标记阶段借该入口执行 mark 与 deferred scan | 增加“普通分配不得调用”的 contract；Finalizer 创建与 resurrection 改走命名明确的特殊闭包入口，并修正误导性的 mark-at-alloc 注释 |
| `DiscoverObject/DiscoverManagedObject()` | white small target 仍可能读取 stateWord/TypeInfo 做合法性检查 | collectSet 用 stable alloc bit；ACTIVE HAS_REF 用对象头验证和临时 visit bit，不能直接返回 |
| `RegionSpaceMS::TryAllocateLargeObjectOnce()` | Mark 期间用 `LARGE_MARKED/AtomicSetMarked()` 保护新大对象 | LARGE primary 记录 current generation；被发现时走 external scan，本轮禁止 Sweep |
| `RegionScanToken/Mark queue/termination` | 只收敛正式 Mark 的 Object/Region/barrier/private work | token 增加 role；同一 queue/outstanding 收敛 collectSet 与 External Active 两种工作 |
| Sweep 入口 | 依靠 central unswept/generation 选择 Region | 只接受本轮 collection token；拒绝 ACTIVE、current-generation 和边界后归还 Region |

### 3.6 mark plane 的并发协议

#### 3.6.1 当前实现是什么

当前普通 root/trace 发现路径仍然以单个对象为单位：

```text
DiscoverObject(obj)
  -> ResolveMarkTarget
  -> probe mark bit
  -> white 对象读取 obj->GetTypeInfo()
  -> target.TryMark()/TestAndSetMarkBit
  -> winner 判断 TypeInfo::HasRefField()
       ├── NO_REF：结束
       └── HAS_REF：对象地址进入 WorkStack/WorkBuf
```

`MSRegionInfo::TestAndSetMarkBit()` 对单个 slot 执行：

```cpp
uint64_t old = __atomic_fetch_or(&markBitmap[wordIdx], bit, __ATOMIC_RELAXED);
return (old & bit) != 0;
```

当前也已经有 `MarkBitmapWord()/MarkBitmapWordResolved()`，能够一次提交一个 word mask，
并返回本次真正赢得的 bit；barrier 等局部路径已经可以利用 word batch。但是普通
root/对象字段发现、HAS_REF 后续任务和终止协议尚未统一到 Region word 模型：

- mark bit 对所有 small Region 只有“正式存活”一种解释；
- collectSet white target 仍读取对象 `stateWord/TypeInfo` 做合法性检查；
- 普通发现路径仍可能逐对象执行 mark RMW；
- HAS_REF winner 发布对象地址，而不是发布所属 Region；
- 没有 `pendingWordSummary/DIRTY/QUEUED/OWNED` 连接 mark winner 与 Region owner；
- 没有 scan plane 表示“已标记但字段尚未扫描”。

#### 3.6.2 与最终设计不一致在哪里

最终 alloc plane 协议已经把 Region 分成两种互斥 role：

```text
COLLECT_SET       -> mark 表示正式存活
EXTERNAL_ACTIVE   -> mark 表示临时 visited
```

因此当前“所有 Region 共用一种 mark 语义、winner 直接发布对象”的路径不能继续保留。
主要差异是：

| 项目 | 当前实现 | 最终设计 |
|---|---|---|
| 目标合法性 | white small 普遍读取 TypeInfo | collectSet 用 stable alloc；ACTIVE HAS_REF 才读对象头 |
| mark 粒度 | 普通路径以单对象 RMW 为主 | Worker-local 合并同一 bitmap word |
| mark 语义 | 统一表示正式存活 | collectSet formal mark；ACTIVE temporary visit |
| HAS_REF 后续任务 | 对象地址进入 WorkStack/WorkBuf | word summary + DIRTY + Region token |
| 重复发现 | atomic bit 去重对象 winner | atomic `newBits` 同时去重 mark 和扫描发布 |
| 生命周期保护 | 主要依赖地址解析和当前 Region 状态 | batch/token 显式携带 epoch、generation、role |

mark plane 只负责“首次发现”，不能再隐含承担“对象已经扫描”或“对象已经可靠入队”。

#### 3.6.3 最终 producer 路径

所有 root、对象字段、barrier、assist 和 Region owner 产生的 small reference 都调用同一
个 role-aware Discover：

```text
Resolve ref -> Region/cycle role

COLLECT_SET:
  heap/Region/generation/slot alignment
  -> stable alloc bit == 1
  -> 加入 MarkBatch(role=COLLECT_SET)

EXTERNAL_ACTIVE:
  heap/Region/generation/slot alignment
  -> NO_REF：直接返回
  -> HAS_REF：验证 stateWord/TypeInfo
  -> 加入 MarkBatch(role=EXTERNAL_ACTIVE)
```

`MarkBatch` 的 entry key 至少包含：

```text
collectionEpoch
regionGeneration
regionIdx
wordIdx
role
scanClass
```

不能只按 `(regionIdx, wordIdx)` 合并，否则 Region 重用后的旧 candidate、正式 mark 和
ACTIVE visit 可能被写进同一个 word 语义。

flush 时先按 role 做最终过滤：

```text
COLLECT_SET:
  candidateMask &= stableAllocWord & validMask

EXTERNAL_ACTIVE:
  candidate 已逐个完成对象合法性检查
  candidateMask &= validMask
  禁止读取 live alloc word
```

然后一次提交整个 word：

```cpp
uint64_t old = AtomicFetchOr(markWord, mask, RELAXED);
uint64_t newBits = mask & ~old;
```

三个值的职责不同：

```text
mask:
  当前 batch 收集到的候选

old:
  所有 Worker 此前已经提交的 bit

newBits:
  当前 flush 真正赢得的首次发现对象
```

只有 `newBits` 可以产生后续职责：

```text
newBits == 0:
  全部是重复引用，结束

COLLECT_SET + NO_REF:
  正式存活已经建立，没有引用字段，结束

HAS_REF:
  pendingWordSummary |= 1 << wordIdx
  DIRTY = 1
  必要时发布带 role 的 Region token
```

禁止根据原始 `mask` 发布扫描任务，否则同一个对象的重复引用会产生重复 Region dirty
和无效 token。

#### 3.6.4 多个 Worker 同时标记同一 Region

mark plane 允许任意数量 producer 并发提交。同一个 Region、同一个 word 的两个 batch：

```text
Worker A mask = 0011
Worker B mask = 0110

A 先 fetch_or:
  old=0000, newBits=0011

B 后 fetch_or:
  old=0011, newBits=0100
```

最终 mark word 是 `0111`，A 负责 bit0/bit1 的首次发现，B 只负责 bit2；没有 bit
丢失，也没有两个 Worker 同时拥有同一个对象的首次发现职责。

对象 mark winner 与 Region scan owner 是两层独立职责：

```text
mark winner:
  负责把本次 newBits 发布到 Region pending 状态

Region owner:
  负责消费该 Region 全部已发布的 marked-but-unscanned bit
```

赢得 mark bit 的 Worker 不一定取得 Region token，也不需要等待该 Region 的 owner。

#### 3.6.5 从 mark winner 到 Region owner 的发布顺序

HAS_REF `newBits` 的正确发布顺序是：

```text
1. atomic OR mark/visit word
2. release OR pendingWordSummary, 1 << wordIdx
3. release OR control, DIRTY
4. control 没有 QUEUED/OWNED:
     CAS 设置 QUEUED
     regionOutstanding++
     release push RegionScanToken(role)
5. control 已有 QUEUED/OWNED:
     不发布第二个 token，由现有 token/owner 吸收
```

不能先 push token 再写 summary。否则 owner 可能已经取得 token，却看到
`pendingWordSummary == 0` 并提前释放。

mark RMW 使用 `RELAXED` 的前提是：后续 summary/control/queue 的 release 发布位于同一
producer，owner 通过 acquire 取得工作。mark atomic 只保护 bitmap 竞争，不是对象初始化
数据的发布屏障；对象字段可见性仍由正常引用发布、Root STW 和 barrier 协议保证。

#### 3.6.6 batch flush、清零和 Region 重用

candidate 尚在 Worker-local batch 时，全局 mark word、summary 和 queue 都看不到它。
以下边界必须 `FlushAll()`：

```text
root 枚举结束
GC Worker/assist 准备 idle 或返回
Region owner 准备释放 OWNED
scalar/bulk barrier 返回
自然终止检查之前
MarkTermination STW 尾收敛
Worker/MarkContext 销毁
```

flush 时必须重新校验 `collectionEpoch/regionGeneration/role`，不能只在 candidate 加入
batch 时检查一次。Region 清空、归还和重用还必须保证：

- 上一轮所有 batch/token 已 drain；
- concurrent Sweep 已停止；
- mark/visit word 只在 STW/quiescent 状态使用普通 store 或 `memset` 清零；
- Region 重用前更新 generation；
- stale batch/token 只能丢弃，不能写入新 Region；
- 最后一个 bitmap word 始终与 `validMask` 相交，禁止设置无效 tail bit。

MarkSetup 清 collectSet 的正式 mark words，也清 ACTIVE HAS_REF 的临时 visit words；
两者都只处理有效 words，不扫描从未使用的 committed Region。ACTIVE 自然归还 Central
后，本 cycle 仍保持 `EXTERNAL_ACTIVE` role，不能把已写入的 visit bit改解释为正式
存活。

#### 3.6.7 这样修改的意义

1. collectSet white target 用 stable alloc bit 验证，不再读取目标对象 TypeInfo；
2. 同一 word 的多个候选合并为一次 atomic RMW；
3. `newBits` 同时去重对象首次发现和后续扫描发布；
4. NO_REF winner 到 mark 为止，不产生队列和 scan 工作；
5. HAS_REF 从逐对象 WorkBuf 改成 Region pending word，降低任务数量；
6. collectSet formal mark 与 ACTIVE temporary visit 共用物理平面但不混淆语义；
7. epoch/generation/role 防止延迟 batch 污染重用后的 Region；
8. 明确的 flush 和发布链使 MarkTermination 能证明不存在隐藏的灰对象。

### 3.7 scan plane 的并发协议

#### 3.7.1 当前实现是什么

当前 `MSRegionInfo` 只有 alloc/mark 两个 bitmap，没有独立 scan plane。普通 HAS_REF
对象通过逐对象任务完成扫描：

```text
TryEnqueueGreyObject
  -> DiscoverObject
  -> mark winner
  -> TypeInfo::HasRefField()
  -> PushGrey(obj)/PutObj(obj)

GC Worker
  -> 从 WorkStack/WorkBuf 取出 obj
  -> 再次检查 obj/TypeInfo/HasRefField
  -> ForEachRefFieldGCInline/ForEachRefFieldGCKnownType
  -> 每个 child 再走 DiscoverObject
  -> HAS_REF child 再进入对象队列
```

当前 mark bit 只告诉 GC“对象已经标记”，对象是否完成字段扫描由“对象地址是否仍在某个
WorkStack/WorkBuf/private buffer 中”隐式表达。这产生四个问题：

1. 一个 HAS_REF winner 通常对应一个对象任务，任务数量接近 HAS_REF 存活对象数；
2. 同一 Region 的对象可能分散在不同 Worker 和不同 WorkBuf，扫描顺序缺少 Region
   局部性；
3. 离开逐对象队列后，没有 bitmap 状态区分“marked but unscanned”和“scanned”；
4. termination 必须同时推断所有全局/私有对象队列是否真正为空，无法直接按 Region
   验证灰工作是否完成。

#### 3.7.2 与最终设计不一致在哪里

最终设计把“对象已经发现”和“对象字段已经处理”拆成两个平面：

```text
mark=0, scan=0:
  白色，尚未发现

mark=1, scan=0:
  灰色，已经发现，字段尚未扫描

mark=1, scan=1:
  黑色，字段已扫描，产生的子引用职责已可靠转交
```

当前没有 scan bit，也没有 Region owner，因此无法使用：

```text
COLLECT_SET:     alloc & mark & ~scan
EXTERNAL_ACTIVE: visit(mark) & ~scan
```

查找待扫描对象。要从逐对象 WorkBuf 转成 Region 任务，scan plane、Region pending
summary 和唯一 owner 必须同时实现；只增加 scan bitmap、不替换对象队列，不会带来性能
收益，反而增加内存和清零成本。

#### 3.7.3 最终 scan plane 的角色语义

scan plane 只对 `HAS_REF` Region 有意义：

```text
COLLECT_SET:
  mark = 本轮正式存活
  scan = 该正式存活对象的字段扫描已经完成

EXTERNAL_ACTIVE:
  mark = 本轮临时 visited
  scan = 该临时 visited 对象的字段扫描已经完成
```

`NO_REF` Region 不读取、不清零、不统计 scan plane：

```text
COLLECT_SET NO_REF:
  formal mark winner 已完成全部 GC 职责

ACTIVE NO_REF:
  本轮不回收且没有出边，Discover 直接返回
```

两种 role 都必须保持：

```text
scan ⊆ mark
```

但只有 collectSet 额外要求：

```text
mark ⊆ alloc
```

#### 3.7.4 Region owner 扫描什么

一个 `HAS_REF` Region 同时只有一个 scan owner。owner 不是无条件遍历 Region 的全部
slot，而是先取得 producer 发布的 word summary：

```text
words = atomic exchange(pendingWordSummary, 0)
```

然后只处理 `words` 指定的 bitmap word：

```text
for each wordIdx in words:
  markWord = atomic load mark
  scanWord = atomic load scan

  COLLECT_SET:
    allocWord = load stable alloc
    toScan = allocWord & markWord & ~scanWord & validMask

  EXTERNAL_ACTIVE:
    不读取 live allocWord
    toScan = markWord & ~scanWord & validMask

  while toScan != 0:
    slotIdx = ctz(toScan)
    obj = RegionStart + slotIdx * slotSize
    扫描 obj 字段
    completed |= slot bit
    toScan &= toScan - 1
```

例如一个 Region 有4096个 slot，但本次 summary 只有 word3 和 word17，owner 只读取这
两个 word，并只扫描其中 `mark=1 && scan=0` 的对象。它不会遍历其他62个 word，也不会
扫描未标记的 slot。

owner 在一次占有期间缓存：

```text
RegionInfo/RegionStart
slotSize/divMagic
alloc/mark/scan plane base
scanClass/scanRole
最近 TypeInfo -> GCScanDescriptor
```

这不会消除每个 HAS_REF 对象读取 TypeInfo 和枚举引用字段的语义成本，但减少逐对象
任务、RegionInfo 重查以及跨 Region 随机跳转。

#### 3.7.5 扫描产生的子引用放到哪里

owner 不把每个普通 small child 直接塞回普通 WorkBuf，而是使用自己的
Worker-local `MarkContext/MarkBatch`：

```text
scan parent fields
  -> childRef
  -> DiscoverSmallRef(childRef, owner.MarkContext)
```

child batch flush 后按目标 Region 分流：

```text
collectSet NO_REF:
  formal mark，结束

collectSet HAS_REF:
  targetRegion summary + DIRTY
  必要时发布 COLLECT_SET token

ACTIVE NO_REF:
  直接返回

ACTIVE HAS_REF:
  temporary visit
  targetRegion summary + DIRTY
  必要时发布 EXTERNAL_ACTIVE token

LARGE/WeakRef/Finalizer/其他特殊对象:
  转入对应的专用 Object/Deferred/Range 工作队列
```

如果 child 仍在当前 OWNED Region：

```text
当前 owner 不递归立即扫描
  -> 当前 Region pendingWordSummary |= childWord
  -> DIRTY = 1
  -> owner 下一轮吸收
```

如果 child 在另一个 Region：

```text
目标 Region 无 QUEUED/OWNED:
  发布目标 Region token

目标 Region 已 QUEUED/OWNED:
  只设置目标 summary + DIRTY
  由目标 token/owner 吸收
```

#### 3.7.6 多个 Worker 向同一个 owner 发布

假设 Worker C 已经持有 Region R 的 `OWNED`：

```text
Worker C:
  正在扫描 R 的 word3

Worker D:
  新发现 R.word17 中的对象
  -> atomic mark/visit
  -> pendingWordSummary |= bit17
  -> DIRTY = 1
  -> 看到 R 已 OWNED，不发布第二个 token
  -> 继续自己的其他工作

Worker C:
  当前扫描轮结束
  -> acquire 检查 DIRTY/summary
  -> 发现 bit17
  -> 继续下一轮
```

如果 producer 与 owner 释放发生竞态，只允许两个结果：

```text
producer 先发布:
  owner 看到 DIRTY/summary，继续扫描

owner 先成功 OWNED -> 0:
  producer 看到无 QUEUED/OWNED
  producer 设置 QUEUED 并发布新 token
```

不允许存在：

```text
roleToScan != 0
且没有 batch/summary/DIRTY/QUEUED/OWNED
```

#### 3.7.7 scan bit 的设置时机

scan bit 不能在字段枚举前设置。普通 small 对象只有完成以下步骤后才能加入
`completed`：

```text
1. 已读取并解释对象 TypeInfo/GC scan descriptor
2. 已枚举全部强引用字段
3. 每个 child 的发现职责已转交到受终止协议保护的 MarkContext/batch
4. WeakRef/Finalizer/特殊任务已成功发布，并由统一 outstanding 保护
```

然后 owner 才能：

```text
release OR scanWord, completed
```

允许 child 暂时位于当前 owner 的 registered MarkContext 中，不要求每扫描一个字段就
立即访问全局队列；但 owner 在释放 `OWNED`、进入 idle 或退出之前必须
`MarkContext.FlushAll()`。因此：

```text
scan=1
  => 字段已经枚举，所有子引用职责至少位于 active owner batch 或已发布队列

owner released
  => 该 owner batch 已全部 flush，不存在隐藏子引用
```

如果希望让 `scan=1` 严格表示“所有 child 已经全局发布”，可以在每轮 owner 扫描完成后
先 flush batch，再批量 OR completed scan bits；这更容易验证，但会缩短 batch 合并窗口。
最终实现二选一，不保留两套模式：建议保留 active-owner batch 方案，并在 termination
显式检查所有 active MarkContext。

#### 3.7.8 owner 什么时候释放

owner 每轮执行：

```text
1. clear DIRTY
2. exchange pendingWordSummary
3. 扫描本轮 toScan
4. 提交 completed scan bits
5. flush MarkContext
6. acquire 检查 DIRTY/summary
7. 非空：回到步骤1
8. 为空：CAS OWNED -> 0
```

释放成功前必须保证当前 Region 达到局部 fixed point：

```text
pendingWordSummary == 0
DIRTY == 0
roleToScan == 0
owner local batch 已 flush
```

Step60 默认由 owner 处理到这个 fixed point。Step61 引入 WorkUnit budget 后，可以在
达到预算时把剩余 summary 保留并执行 `OWNED -> QUEUED`，重新发布同一个 Region token，
以避免热点 Region 长期独占 Worker；这只改变调度公平性，不改变单 owner 不变量。

#### 3.7.9 MarkTermination 如何验证

自然终止和 STW 尾收敛必须同时检查：

```text
RegionScanQueue empty
regionOutstanding == 0
所有 MarkContext batch empty
所有 active owner/assist == 0

所有 collectSet HAS_REF Region:
  pendingWordSummary == 0
  control 无 DIRTY/QUEUED/OWNED
  alloc & mark & ~scan == 0

所有 externalVisitedRegions:
  pendingWordSummary == 0
  control 无 DIRTY/QUEUED/OWNED
  visit(mark) & ~scan == 0
```

这使 GC 不再只依赖“对象队列看起来为空”判断闭包完成，而是可以直接证明不存在已发现但
未扫描的 small 对象。

#### 3.7.10 这样修改的意义

1. 将逐对象 WorkBuf 压缩成每个活跃 Region 至多一个 queued/owned token；
2. owner 按 pending word 和 slot 地址连续扫描，提高对象、TypeInfo 和 bitmap 局部性；
3. `scan` 精确记录已完成对象，Region 再次 DIRTY 时不重扫旧 marked object；
4. 多个 producer 可以无等待地向同一 Region 添加工作，只有一个 owner 消费；
5. 子引用继续通过 MarkBatch 并行扩散到同 Region 或其他 Region，不形成递归扫描；
6. NO_REF 完全不承担 scan plane 触页和调度成本；
7. termination 能用 `mark & ~scan` 和 Region control 直接检查灰工作是否收敛；
8. 为 Step61 的统一任务调度和 WorkUnit budget 提供可暂停、可重新排队的 Region 任务。

### 3.8 Sweep

collectSet 中的 SEALED small Region 按 word 回收：

```text
live = alloc & mark
dead = alloc & ~mark

run cleanup for dead
alloc = live
mark = 0
scan = 0      // 仅 HAS_REF 的有效 words
```

Sweep 结束后：

- partial/full Region 保持原 `(sizeClass, scanClass)`；
- empty Region 清空分类后进入 classless empty cache；
- alloc summary 根据 `~alloc & validMask` 重建。

### 3.9 独立收益、代价和边界

**alloc plane 单独带来的收益**

- SEALED Region 可以不读取对象头就确认 slot 存在；
- Sweep、marked stats 和合法性检查共享同一 occupancy 权威。

**mark plane 的收益**

- 对 collectSet 保持唯一 mark winner，对 ACTIVE 保持唯一 visit winner；
- 为 word batching 提供提交目标。

**scan plane 单独不产生性能收益**

它是 Region 级扫描的必要状态。如果不实施优化三，scan plane 只增加内存和清零成本，
不应单独上线。

**必须记录**

```text
allocPlaneTouchedWords
markPlaneProbeWords / markPlaneRmwWords
scanPlaneTouchedWords
bitmapCommittedPages[3]
bitmapClearBytes[3]
invalidTailBits
```

## 4. 优化三：Region 级扫描

### 4.1 要解决的问题

当前每个 `HAS_REF` mark winner 都作为对象地址进入 WorkBuf。一个 Worker 取得的相邻
WorkBuf entry 可能来自完全不同的 Region：

```text
Region 7 -> Region 931 -> Region 12 -> Region 4800
```

这会反复切换：

- `MSRegionInfo` cache line；
- mark bitmap cache line；
- 对象数据页；
- TypeInfo/GCTib；
- TLB working set。

WorkBuf 同时把“对象已经标活”和“对象等待扫描”绑定成逐对象任务，无法利用同一
Region 中多个 winner 的空间聚集性。

### 4.2 RegionScanToken

普通 `HAS_REF` small object 不再逐对象入队。第一次产生待扫描 bit 时，只发布所属
Region：

```cpp
struct RegionScanToken {
    uint32_t regionIdx;
    uint16_t regionGeneration;
    uint16_t workEpoch;
    MSRegionScanRole role;
};
```

队列不在 Mark 热路径动态分配。初始化时按最大 committed Region 数建立稳定
`RegionScanNode` 数组，node 由 `regionIdx` 唯一索引；同一个 Region 在任意时刻最多
处于 QUEUED 或 OWNED 一次。

一个 Region 在同一 cycle 只能属于 `COLLECT_SET` 或 `EXTERNAL_ACTIVE` 一种 role；
token role 必须与 cycle membership 一致。LARGE、WeakRef、Finalizer 和其他主线程
专用对象继续使用独立 Object/Deferred 队列，不塞进普通 small Region scan 协议。

### 4.3 Region 扫描状态

每个 `HAS_REF` Region 有：

```text
control:
  QUEUED = 1 << 0
  OWNED  = 1 << 1
  DIRTY  = 1 << 2

pendingWordSummary:
  bit i = mark word i 出现了新的 formal-mark/visit winner
```

`pendingWordSummary` 正好是 64 bit，因为一个 Region 最多有 64 个 bitmap word。
它避免每次 dirty 都遍历完整 512B mark/scan plane。

### 4.4 producer 发布

某次 formal-mark/visit 提交在 `(regionIdx, wordIdx, role)` 得到 `newBits != 0` 后：

```text
atomic OR pendingWordSummary, 1 << wordIdx
atomic OR control, DIRTY

control 没有 QUEUED/OWNED:
  CAS 设置 QUEUED
  regionOutstanding++
  release push RegionScanToken(role)

control 已有 QUEUED/OWNED:
  结束，由现有 token/owner 吸收
```

同一 batch 即使赢得 64 个对象，也只设置一次 word summary，最多发布一个 Region
token。

### 4.5 owner 扫描

Worker pop token 后校验 epoch/generation，再 CAS：

```text
QUEUED -> OWNED
```

成功 owner 循环：

```text
1. clear DIRTY
2. words = atomic exchange(pendingWordSummary, 0)
3. 对 words 中每个 wordIdx:
       markWord  = atomic load mark
       scanWord  = load scan
       role == COLLECT_SET:
           allocWord = load stable alloc
           toScan = allocWord & markWord & ~scanWord
       role == EXTERNAL_ACTIVE:
           不读取 live allocWord
           toScan = markWord & ~scanWord

       completed = 0
       while toScan != 0:
           slotIdx = ctz(toScan)
           obj = RegionStart + slotIdx * slotSize
           typeInfo = obj->GetTypeInfo()
           scan obj fields into MarkContext
           completed |= slot bit
           toScan &= toScan - 1

       release OR scanWord, completed

4. flush 当前 MarkContext 的 mark batch
5. acquire load control/pendingWordSummary
6. DIRTY 或 summary 非零 -> 回到 1
7. CAS OWNED -> 0
```

对象字段枚举完成后才加入 `completed`。EXTERNAL_ACTIVE 的 markWord 在这里是 visit
word，不是 Sweep liveness。字段产生的引用已经进入当前活跃
`MarkContext`；owner 在释放 `OWNED` 前必须 flush，所以 Region 完成时不存在隐藏在
已退出 owner 中的子引用。

### 4.6 producer/owner 竞态

最关键的竞态发生在 owner 准备释放时。

**producer 先发生**

```text
producer: formal-mark/visit bit -> summary -> DIRTY
owner:    CAS OWNED -> 0 失败
owner:    继续下一轮
```

**owner 先发生**

```text
owner:    CAS OWNED -> 0 成功
producer: 看到无 QUEUED/OWNED
producer: 设置 QUEUED 并发布新 token
```

最终不允许存在：

```text
roleToScan != 0
且 control 无 DIRTY/QUEUED/OWNED
且 pendingWordSummary == 0
```

其中 `roleToScan` 对 collectSet 是 `alloc & mark & ~scan`，对 external ACTIVE 是
`visit & ~scan`。Debug MarkTermination 必须检查 collectSet 和本轮
`externalVisitedRegions`。

### 4.7 为什么需要 scan plane

Region 第二次 dirty 时，mark plane 同时包含：

```text
上一轮已经扫描过的对象
本次刚发现的对象
```

没有 scan plane，只能：

- 再次扫描全部 marked object；或
- 重新维护逐对象 WorkBuf。

scan plane 让 owner 只处理：

```text
COLLECT_SET:     toScan = alloc & mark & ~scan
EXTERNAL_ACTIVE: toScan = visit(mark) & ~scan
```

所以 scan plane 不是叶子对象优化，而是两种 Region 级增量扫描的去重状态。

### 4.8 局部性收益

一个 token 被 owner 领取后，以下信息保持稳定：

```text
RegionInfo
RegionStart/End
sizeClass/slotSize/divMagic
alloc/mark/scan word base
scanClass = HAS_REF
scanRole
```

owner 连续处理相同 Region：

- 不重复执行 heap-to-Region 映射；
- 不为每个对象重新获取 Bitmap base；
- 对象页和 TypeInfo 更可能命中 cache；
- 多个完成 bit 可以一次 OR 到 scan word；
- 全局任务数量从“HAS_REF winner 数”降到“dirty Region 次数”。

### 4.9 独立收益、代价和边界

**收益**

- 减少逐对象 WorkBuf push/pop；
- 改善 RegionInfo、Bitmap、对象页局部性；
- 同 Region winner 合并为一个任务；
- 通过 `pendingWordSummary` 避免扫描未变化的 bitmap word。

**代价**

- 需要 scan plane；
- 需要 Region token queue、control 和 summary；
- 热点 Region 由一个 owner 扫描，单 Region 内没有并行；
- 含引用对象极少且地址完全随机时，Region 调度收益可能较小。

**不负责解决**

- `NO_REF` 叶子对象发现成本；
- 同 mark word 的原子 RMW 次数；
- 单个超大引用数组的内部并行。

**必须记录**

```text
regionTokensPublished
regionOwnerClaims
regionOwnerClaimFailures
dirtyMergedIntoQueued
dirtyMergedIntoOwned
pendingWordsProcessed
toScanBits
scanBitsCompleted
regionOwnerLoops
regionQueuePush/Pop/Steal
```

## 5. 优化四：Mark Bitmap 批量更新

### 5.1 要解决的问题

当前每个候选对象独立执行：

```text
atomic fetch_or(markWord, oneBit)
```

如果一个高扇出对象连续引用同一 Region 的多个 slot，会出现：

```text
bit 1  -> fetch_or
bit 3  -> fetch_or
bit 7  -> fetch_or
bit 11 -> fetch_or
```

这些操作写同一个 64-bit word。多 Worker 同时处理相邻对象时，还会反复争抢同一
cache line。

### 5.2 MarkWordBatch

每个 GC main/helper/assist 的 `MarkContext` 保存 8 个 entry：

```cpp
struct MarkWordBatchEntry {
    uint32_t regionIdx;
    uint16_t regionGeneration;
    uint8_t wordIdx;
    uint8_t scanClass;
    uint8_t scanRole;
    uint64_t pendingMask;
};

struct MarkWordBatch {
    MarkWordBatchEntry entries[8];
    uint8_t nextVictim;
};
```

key 是：

```text
(regionIdx, regionGeneration, wordIdx, scanRole)
```

新候选命中已有 entry 时只执行：

```text
entry.pendingMask |= bitMask
```

8 个 entry 全部占用后，以 round-robin victim flush 一个 entry，再复用该 slot。
不分配内存，不使用共享锁，不在热路径维护全局统计。

### 5.3 候选加入 batch 前的检查

batch 只合并 mark/visit 写，不能合并或删除对象合法性检查。每个引用先完成公共检查，
再按 role 分流：

```text
1. null/heap range
2. regionIdx 和 Region offset
3. Region lifecycle、generation、scanRole
4. sizeClass、slot 边界、slot 对齐

COLLECT_SET:
  5. stable alloc bit == 1
  6. relaxed formal mark probe
  7. 未标记候选加入 COLLECT_SET batch

EXTERNAL_ACTIVE:
  5. NO_REF 直接返回
  6. stateWord/TypeInfo validity
  7. relaxed visit probe
  8. 未访问候选加入 EXTERNAL_ACTIVE batch
```

collectSet 路径由 ScanClass 和 alloc plane 消除对象读取；EXTERNAL_ACTIVE 因为不能读
并发变化的 alloc plane，仅对 HAS_REF 引用保留对象头验证。

同一个 bit 在 batch 尚未 flush 时再次出现，只会重复 OR 同一个本地 bit，不产生额外
全局原子操作。

### 5.4 flush

flush 一个 entry：

```cpp
uint64_t validMask =
    entry.scanRole == COLLECT_SET
        ? entry.pendingMask & LoadAllocWord(entry)
        : entry.pendingMask; // candidates were validated before batching
uint64_t oldMark = AtomicFetchOr(MarkWord(entry), validMask, RELAXED);
uint64_t newBits = validMask & ~oldMark;
```

flush 前重新检查：

- Region generation 未改变；
- Region role 与本轮 cycle token 一致；
- lifecycle 仍是可标记 small Region；
- COLLECT_SET 的 alloc bits 仍覆盖 pending candidates；
- EXTERNAL_ACTIVE 仍属于本轮 excluded generation。

在本轮 collectSet 冻结协议下这些条件应稳定；二次检查用于防止错误 token、退出路径和未来
Region 生命周期修改破坏前提。

### 5.5 newBits 后续处理

`newBits == 0` 表示所有候选已经由其他 producer 标记，直接结束。

`newBits != 0`：

```text
COLLECT_SET + NO_REF:
  leafMarked += popcount(newBits)
  不产生扫描工作

COLLECT_SET + HAS_REF:
  pendingWordSummary |= 1 << wordIdx
  PublishRegionDirty(regionIdx, COLLECT_SET)

EXTERNAL_ACTIVE + HAS_REF:
  pendingWordSummary |= 1 << wordIdx
  PublishRegionDirty(regionIdx, EXTERNAL_ACTIVE)
```

注意：HAS_REF 不按 `newBits` 恢复多个对象地址并逐个入队。它只发布一次 word
summary 和至多一个 Region token，后续对象地址由 Region owner 根据 bit 计算。

### 5.6 必须 flush 的边界

GC Worker batch 可以跨多个对象字段保留，但必须在以下边界 flush：

```text
entry eviction
当前 Region owner 准备释放 OWNED
当前 WorkBuf/ObjectTask/RegionTask 完成
worker yield 或退出 active claim
assist 返回 mutator
root enumeration chunk 完成
进入 quiesce/MarkTermination
MarkContext 销毁
```

Bulk copy/write barrier 可以在一次调用内 batch，返回前必须 flush。标量 SATB
write barrier 保持立即提交，不能把被覆盖的旧引用留在非持久的临时 batch 中。

compiler inline scalar barrier 是同一协议的“单 bit 立即 flush”特化，不是第二套
Mark 状态机：

```text
resolve Region role
  -> COLLECT_SET:
       validate alloc bit
       atomic OR one formal mark bit
       loser/NO_REF winner: return
       HAS_REF winner:
         call CJ_MRT_MSGCPublishRegionWord(..., COLLECT_SET)
  -> EXTERNAL_ACTIVE:
       call Runtime DiscoverExternalActiveRef(ref)
       Runtime 完成对象头验证、visit RMW 和 role token 发布
```

`CJ_MRT_MSGCPublishRegionWord` 只负责 summary/DIRTY/token 发布，不再读取目标对象，也
不把对象地址写入 activeLog。GC Worker、root、Region owner 和一次调用内含多个引用的
bulk barrier 使用真正的多-entry batch；scalar barrier 为了 SATB 可靠性不跨调用积压。

### 5.7 原子与发布顺序

对 HAS_REF：

```text
atomic fetch_or markWord
  -> release OR pendingWordSummary
  -> release OR DIRTY
  -> release publish Region token
```

owner 通过 acquire pop/CAS 取得 token 后，必须能看到先前的 mark word 和 summary。
mark RMW 本身可以 relaxed，因为后续 release/acquire 链负责工作发布；对象内容由对象
分配/引用写入协议保证。

### 5.8 独立收益、代价和边界

**收益**

- 同一 word 最多 64 个候选合为一次原子 RMW；
- duplicate 在本地合并，不进入共享 cache line；
- 减少 mark bitmap cache-line ownership 转移；
- 与 Region scan 组合后，多个 winner 只产生一次 Region dirty。

**代价**

- 每个活跃 MarkContext 增加约 8 个 entry；
- flush 时需要 generation/alloc 二次检查；
- 引用地址随机时 merge ratio 可能接近 1；
- batch 延迟要求所有退出路径显式 flush。

**单独实现边界**

- 在当前对象 WorkBuf 架构上也能减少 RMW，但每个 `newBits` 仍要恢复对象地址、读取
  TypeInfo 并逐对象入队；
- 没有 stable alloc plane 时，每个候选仍须读取白对象 stateWord；
- 因此完整收益依赖优化一、二；与优化三组合后才能同时减少 RMW 和队列任务。

**必须记录**

```text
batchCandidates
batchEntryHits / batchEntryMisses
batchFlushes
batchBitsSubmitted
batchNewBits
batchDuplicateBits
batchMergeRatio = batchBitsSubmitted / batchFlushes
batchFlushByEvict / ByTaskEnd / ByAssistReturn / ByTermination
```

## 6. 四项优化的独立收益与依赖关系

### 6.1 独立作用

| 优化 | 自己解决的问题 | 自己不解决的问题 |
|---|---|---|
| 1. ScanClass Region | 把叶子和需扫描对象在 Region 级分开，消除 winner 后的 HasRef 判断 | 不提供可靠 occupancy，单独不能删除白对象 TypeInfo 验证 |
| 2. 三平面 Bitmap | alloc 判断对象存在，mark 判断存活，scan 判断扫描职责 | scan plane 单独不提高速度；不改变任务粒度 |
| 3. Region 级扫描 | 把多个 HAS_REF 对象合成 Region 任务，改善局部性 | 不减少 mark RMW；不加速 NO_REF；不拆大数组 |
| 4. mark word batch | 合并同 word 原子更新和 duplicate | 不减少必须读取的引用；随机地址下合并率低 |

### 6.2 依赖图

```text
优化一：ScanClass Region
  ├── 让 NO_REF winner 可以直接结束
  ├── 让 HAS_REF winner 可以只发布 Region
  └──────────────┐
                 v
优化二：alloc/mark/scan planes
  ├── alloc 让 NO_REF 不读取对象
  ├── mark 是 batch 提交目标
  └── scan 是 Region 增量扫描状态
          │                 │
          v                 v
优化三：Region scan     优化四：mark batch
          │                 │
          └────────┬────────┘
                   v
        batch winner -> Region dirty
        Region owner -> child refs -> batch
```

### 6.3 能否单独上线

| 优化 | 能否独立编译验证 | 是否应独立进入最终 Runtime |
|---|---|---|
| ScanClass Region | 能，验证分配纯度和 ABI | 不应；缺 alloc plane 时 NO_REF 仍读对象 |
| 三平面 Bitmap | 能，验证布局、生命周期和 Sweep | 不应；scan plane 未被 Region scan 使用会纯增开销 |
| Region scan | 不能脱离 ScanClass + scan plane | 只能与优化一、二同时发布 |
| mark batch | 能，在当前 WorkBuf 上验证 RMW 合并 | 最终应与 ScanClass/alloc plane/Region scan 同时发布 |

这里的“独立说明”是职责和验证边界独立，不代表最终 SDK 允许四个 feature flag 任意
组合。最终只发布四项全部启用的一套布局。

### 6.4 推荐实施依赖顺序

实现顺序不是编号顺序，而是：

```text
1. 锁定最终 RegionInfo/Bitmap/AllocBuffer/compiler ABI
2. 实现 ScanClass 分配纯度
3. 实现三平面、collectSet 冻结与 ACTIVE 临时 visit/scan
4. 实现 mark batch 和无对象读取的 Discover
5. 实现双 role Region queue/owner/scan
6. 联合接入 barrier、root、termination、Sweep
```

先实现 batch 再接 Region owner，是因为 Region producer 最终只消费 batch 的
`newBits`，无需先保留逐对象过渡接口。

## 7. 最终整体架构

### 7.1 最终 MSRegionInfo 热字段

保持 64B 单 cache line：

| 偏移 | 字段 | 大小 | 用途 |
|---:|---|---:|---|
| +0 | `allocPtr` | 8 | BUMP fast path |
| +8 | `regionEnd` | 8 | 地址边界 |
| +16 | `nextRegionIdx` | 4 | Central list |
| +20 | `prevRegionIdx` | 4 | Central list |
| +24 | `totalSlotCount` | 2 | 有效 slot 数 |
| +26 | `freeSlotCount` | 2 | 空闲 slot 数 |
| +28 | `divMagic` | 4 | offset→slot |
| +32 | `allocEpoch` | 4 | 本轮快照归属 |
| +36 | `sweepGen` | 4 | Sweep ownership |
| +40 | `allocSummary` | 8 | 哪些 alloc word 还有空 slot |
| +48 | `pendingWordSummary` | 8 | 哪些 mark word 有未吸收 winner |
| +56 | `control` | 4 | QUEUED/OWNED/DIRTY 和其他原子标志 |
| +60 | `regionGeneration` | 2 | Region 重用校验 |
| +62 | `sizeClass` | 1 | 0..35 |
| +63 | `layout` | 1 | scanClass/regionType/sweepState |

`layout` 的最终 bit 分配固定为：

```text
bits [2:0] regionType   // FREE/BUMP/BITMAP/PARTIAL/FULL/LARGE/SPAN/EMPTY_CACHE
bit  [3]   isSwept
bit  [4]   scanClass    // 0=NO_REF, 1=HAS_REF；仅 small non-empty 有效
bits [7:5] reserved
```

`control` 的最终原子 bit 分配固定为：

```text
bit 0 QUEUED
bit 1 OWNED
bit 2 DIRTY
bit 3 LARGE_MARKED
bits [31:4] reserved
```

因此 Task 代码和 LLVM 不能直接把现有 `MSRegionType`/`MSSweepState` 对象覆盖写入整个
`layout` byte，必须使用统一 mask/accessor。

LARGE primary 不进入三平面 small slot 算法，small-only 字段按以下唯一方式复用：

```text
+24..+31 logicalObjectBytes
+32..+35 allocEpoch
+36..+39 sweepGen
+40..+47 spanCount
+48..+55 baseRegionIdx
+56      control.LARGE_MARKED
+62      INVALID_SIZE_CLASS
+63      regionType=LARGE，scanClass bit 必须为 0
```

SPAN entry 在 `+48..+55` 保存同一个 `baseRegionIdx`，`layout.regionType=SPAN`；其余
small-only 字段清零。LARGE/SPAN 的标记继续用 `control.LARGE_MARKED` 原子位，绝不读写
small mark/scan plane。

### 7.2 最终 AllocBuffer 和 CentralPool

```text
AllocBufferMS
  activeRegions_[36][2]
    [sizeClass][NO_REF]
    [sizeClass][HAS_REF]

CentralRegionPool
  buckets_[36][2]
    partialSwept
    fullSwept
    partialUnswept
    fullUnswept

  emptySweptCache
    classless，仅保存完全空 Region
```

64 位 compiler ABI：

```text
AllocBufferMS +0       tlRegion
AllocBufferMS +8       activeRegions_[0][0]
AllocBufferMS +1024    activeLog
```

`activeLog` 不再承载普通 small scalar barrier；该路径在返回前直接提交 mark/visit bit
并发布 Region token。它只为 LARGE、WeakRef、Finalizer 以及受控兼容回退保留，
这些工作不能由普通 small Region token 完整表达。

### 7.3 最终 FastPathState

Runtime/LLVM 共同使用一个 64B v2 C ABI：

```text
+0   abiVersion
+4   barrierPhase
+8   heapReservedStart
+16  heapReservedEnd
+24  regionInfoArray
+32  allocPlaneBase
+40  markPlaneBase
+48  scanPlaneBase
+56  layoutSignature
```

`layoutSignature` 编码：

```text
Region shift = 15
RegionInfo size = 64
plane stride = 512
size class count = 36
scan class count = 2
active region base = 8
active log offset = 1024
```

Runtime、compiler、stdlib 必须作为一套 SDK 发布；不导出旧 ABI 数据符号。

### 7.4 分配路径

```text
MCC_NewObject/NewArray(TypeInfo, size)
  -> ClassifyScan(TypeInfo)
  -> SizeToClass(size)
  -> activeIndex = sizeClass * 2 + scanClass
  -> active Region fast allocation
  -> 初始化对象头/TypeInfo
  -> 返回对象
```

分配器独占写 ACTIVE Region 的 occupancy。BUMP Region 不在 MarkSetup 物化，只在自然
归还 Central 且需要变成 PARTIAL 时由 `PrepareForCentralReturn()` 按 allocPtr 物化；
BITMAP Region 维护 live occupancy 和 allocSummary。GC 不读取 ACTIVE 的 live alloc plane。

### 7.5 PreEnum/MarkSetup 周期边界

一次 GC 开始时：

```text
1. StopAndWaitForAllSweep
2. STW 停止 mutator
3. ReclassifyUnsweptToSwept，封闭上轮残余分类
4. 冻结当时 Central swept lists 为本轮 collectSet 候选
5. 清 small collectSet 的正式 mark；清 large collectSet 的 LARGE_MARKED
6. 清 ACTIVE HAS_REF 的临时 visit(mark)/scan/control/pending summary
7. 清 collectSet HAS_REF 的 scan/control/pending summary
8. Central swept lists 翻转为本轮 unswept lists
9. globalSweepGen += 2，发布新的 excluded generation
10. 恢复 mutator；现有 ACTIVE Region 不 detach、不 refill
```

Mark 期间 collectSet alloc plane 只读；ACTIVE、PreEnum 后新 Region，以及自然归还到
当前 Central swept lists 的 Region 都不进入本轮 Sweep。ACTIVE 若跨越多轮，每轮清空
并重新使用临时 visit/scan 状态，不设强制封存上限。

### 7.6 引用发现

所有 root、对象字段和 GC Worker 使用同一个：

```cpp
DiscoverSmallRef(BaseObject* ref, MarkContext& context)
```

路径：

```text
null/heap gate
  -> regionIdx + regionOffset
  -> Region lifecycle/generation/scan role
  -> COLLECT_SET:
       slotIdx + alignment
       stable alloc bit
       formal mark probe
       context.MarkBatch.Add(..., role=COLLECT_SET)
  -> EXTERNAL_ACTIVE:
       slotIdx + alignment
       NO_REF return
       stateWord/TypeInfo validity
       visit probe
       context.MarkBatch.Add(..., role=EXTERNAL_ACTIVE)
```

COLLECT_SET small 路径不读取目标对象头和 TypeInfo；EXTERNAL_ACTIVE HAS_REF 为避免
给 stale/unallocated slot 设置 visit bit，保留对象头合法性检查。

### 7.7 batch flush 与任务发布

```text
entry(role, pendingMask)
  -> COLLECT_SET：与 stable alloc word 相交
  -> EXTERNAL_ACTIVE：候选已逐个验证，不读 alloc word
  -> 一次 atomic fetch_or mark word
  -> newBits
       ├── 0: return
       ├── COLLECT_SET + NO_REF: popcount，完成
       └── HAS_REF:
             pendingWordSummary |= word bit
             DIRTY = 1
             必要时发布一个带 role 的 Region token
```

因此：

```text
64 个 NO_REF winner
  最好情况 = 1 次 mark RMW，0 个扫描任务

64 个 HAS_REF winner
  最好情况 = 1 次 mark RMW，1 个 Region token
```

### 7.8 Region owner 扫描

```text
pop Region token
  -> QUEUED -> OWNED
  -> exchange pendingWordSummary
  -> 每个 pending word:
       COLLECT_SET:     toScan = alloc & mark & ~scan
       EXTERNAL_ACTIVE: toScan = visit(mark) & ~scan
       按 slot 地址连续扫描对象
       子引用进入同一个 MarkBatch
       completed bits OR 到 scan
  -> flush MarkBatch
  -> 吸收新的 DIRTY
  -> fixed point 后释放 OWNED
```

同一 Worker 在 Region 内复用 size class、slot size 和三个 plane base；只有这里读取
`HAS_REF` 源对象的 TypeInfo。EXTERNAL_ACTIVE owner 不读取 ACTIVE live alloc plane。

### 7.9 终止条件

并发 Mark 自然收敛至少要求：

```text
Object/特殊工作队列为空
RegionScanQueue 为空
regionOutstanding == 0
activeWorkers/assists == 0
所有 MarkContext batch 为空
所有 activeLog 已发布
所有 collectSet HAS_REF Region:
  pendingWordSummary == 0
  control 无 DIRTY/QUEUED/OWNED
  alloc & mark & ~scan == 0
所有本轮 externalVisitedRegions:
  pendingWordSummary == 0
  control 无 DIRTY/QUEUED/OWNED
  visit(mark) & ~scan == 0
```

MarkTermination STW 再 flush mutator log、root 尾部和全部 MarkContext，重复 drain 到
同一 fixed point。第一次 EXTERNAL_ACTIVE winner 把 Region 登记进预分配的
`externalVisitedRegions`；终止只检查该列表，不遍历全部 committed Region。该列表只保存
Region token，不保存 occupancy 副本。

### 7.10 Sweep

```text
NO_REF:
  live = alloc & mark
  dead = alloc & ~mark
  不访问 scan plane

HAS_REF:
  Debug 先验证 alloc & mark & ~scan == 0
  live/dead 同上
  清有效 scan words
```

partial/full Region 保持 ScanClass；完全空 Region 才清分类并回 classless cache。

### 7.11 case26 中四项优化的实际作用

2 GiB 持久 `Array<Byte>`：

```text
ScanClass Region:
  全部位于 NO_REF Region

alloc plane:
  验证 slot，不读取 413.7 万个叶子对象头

mark batch:
  相邻分配的叶子目标落入同 Region/word 时合并 RMW

Region scan:
  不扫描这些叶子；只处理产生叶子引用的 HAS_REF small object
```

保存数百万引用的超大外层数组如果属于 LARGE，本 Step 不拆分它；其内部并行需要
RangeQueue。Step60 的收益集中在数百万目标叶子的发现成本、其他 HAS_REF small
object 的局部性，以及 mark word 原子竞争。

## 8. 并发状态机与内存序

### 8.1 Region 生命周期

```text
EMPTY/FREE
  -> Init(sizeClass, scanClass, allocEpoch, generation)
  -> BUMP/BITMAP，owner P 独占分配
  -> PreEnum 时仍 ACTIVE：留在 TLS，进入 excludedSet，允许继续分配
  -> 可跨越任意多个 GC 周期，每轮重置临时 visit/scan
  -> 自然耗尽/归还：设置当前 sweepGen，必要时物化 BUMP alloc plane
  -> 当前 Central swept list，本轮仍属于 excludedSet
  -> 下一轮 PreEnum 翻转为 unswept，成为 SEALED collectSet
  -> Concurrent Mark + Sweep
  -> 非空：保持分类
  -> 全空：清分类并回 empty cache
```

`regionGeneration` 在空 Region 被重新初始化时递增。Region token、batch entry 和
Region cache 都保存 generation；不匹配时丢弃。

### 8.2 分配与 alloc plane

只有 collectSet 的 alloc plane 供 GC 读取：

```text
ACTIVE owner 普通写 occupancy
  -> 自然归还 Central
  -> BUMP partial 物化 alloc plane
  -> 下一轮 PreEnum 纳入 collectSet
  -> GC 普通/acquire 读 stable alloc
```

不把普通分配变成 atomic。ACTIVE/excludedSet 对象被引用发现时，collector 根据
Region owner/lifecycle、cycle generation 和 collectSet token 判定 role。ACTIVE
不读取正在变化的 alloc word；NO_REF 直接返回，HAS_REF 经对象头验证后设置临时 visit
bit，并继续外部扫描。PreEnum 后对象不做 allocation-time MarkNewObject，但被 Root/引用
闭包发现后允许进入 EXTERNAL_ACTIVE scan。

### 8.3 mark 到 Region 发布

```text
producer:
  resolve role
  COLLECT_SET: stable alloc validation
  EXTERNAL_ACTIVE: object-header validation
  relaxed fetch_or markWord
  release fetch_or pendingWordSummary
  release fetch_or DIRTY
  outstanding++ before release queue push(role)

owner:
  acquire queue pop
  acquire CAS QUEUED -> OWNED
  acquire exchange pendingWordSummary
  acquire load markWord
```

release/acquire 链保证 owner 不会先看到 token、后看不到对应 mark bit。
MarkBatch key 必须包含 role，禁止把同一 Region word 的正式 mark candidate 与临时 visit
candidate 合并为一个 entry。

### 8.4 scan 完成

```text
owner:
  enumerate fields
  child refs -> owner-local MarkBatch
  completedMask |= object bit
  release OR scanWord, completedMask

before releasing OWNED:
  flush MarkBatch
  publish all generated Region work
  acquire recheck DIRTY/summary
```

scan bit 表示字段已经枚举，产生的引用要么已提交，要么仍由一个计入
`activeWorkers` 的 MarkContext 持有。owner 退出 active 状态前必须 flush。

### 8.5 DIRTY 状态机

允许的状态：

| 状态 | 含义 |
|---|---|
| `0` | 无已知待扫描工作 |
| `DIRTY` | producer 正在准备发布 |
| `DIRTY|QUEUED` | token 已发布或正在发布 |
| `OWNED` | owner 正在扫描，当前未观察到新工作 |
| `DIRTY|OWNED` | owner 扫描期间产生了新 winner |

禁止：

```text
QUEUED | OWNED
```

所有 CAS 只修改扫描状态位，不覆盖 size/sweep/large 等其他 control 位。

owner 只在：

```text
pendingWordSummary == 0
DIRTY == 0
MarkBatch 已 flush
```

时 CAS 清 `OWNED`。CAS 失败必须重新进入扫描循环。

### 8.6 batch 生命周期

MarkBatch 是 owner-local：

- 不被另一个 Worker 读取；
- 不把指针发布到全局；
- entry 的 Region generation 必须与 flush 时一致；
- assist 返回、worker yield、任务完成、quiesce 前全部 flush；
- scalar SATB barrier 不跨调用保存临时 batch。

这使 batch 延迟不改变 SATB 正确性，也不会让 MarkTermination 看不到灰工作。

### 8.7 MarkTermination 竞态封闭

进入自然终止前执行两次稳定读取：

```text
seq0 = publishSeq
检查队列、outstanding、activeWorkers、所有 local batch/activeLog
检查 dirty Region summary
seq1 = publishSeq

仅 seq0 == seq1 且所有条件为空时进入 quiesce
```

STW 后：

1. flush mutator activeLog；
2. flush/销毁所有 MarkContext；
3. 枚举最终 root；
4. drain Object/Region 工作，包括 `COLLECT_SET/EXTERNAL_ACTIVE` 两种 role；
5. 检查所有 `HAS_REF` collectSet Region 的 `alloc & mark & ~scan == 0`；
6. 检查所有 externalVisited Region 的 `visit & ~scan == 0`；
7. 再进入 Finalizer/WeakRef 既有闭包。

### 8.8 必须保持的全局不变量

```text
mark ⊆ alloc                  // 仅 collectSet small Region
scan ⊆ mark                  // collectSet mark 或 ACTIVE visit
collectSet NO_REF Region 不产生 scan bit
collectSet HAS_REF 的 mark & ~scan != 0
  => DIRTY/QUEUED/OWNED/pending summary/active batch 至少一个成立
ACTIVE NO_REF Region 不产生 visit/scan bit
ACTIVE HAS_REF 的 visit & ~scan != 0
  => DIRTY/QUEUED/OWNED/pending summary/active batch 至少一个成立
QUEUED 与 OWNED 互斥
Region generation 不跨 token/batch/cache 静默变化
ACTIVE live alloc plane 不被 GC 并发读取
ACTIVE mark/scan plane 只表达本轮 visit/scan，Sweep 不消费
ACTIVE visit 不要求是 alloc 子集；设置前必须完成对象合法性检查
PreEnum 后分配对象不调用 MSGC allocation-time MarkNewObject
Sweep 只处理本轮 SEALED collectSet
PreEnum 后归还 Central 的 Region 只能进入下一轮 collectSet
```

Debug 构建在 MarkTermination 和 Sweep 入口检查这些关系；Release 只保留热路径必要
分支。

## 9. 按依赖顺序实施

以下任务在同一开发分支连续完成。Task 1～5 只验证最终组件，不产出可发布 SDK；
Task 6 完成 Runtime/LLVM/stdlib 联合构建后才形成唯一可运行版本。

### Task 1：锁定最终 RegionInfo、FastPathState 和 AllocBuffer ABI

**Files**

- Modify: `src/Heap/MAllocator/MSRegionInfo.h`
- Modify: `src/Heap/MAllocator/MSRegionInfo.cpp`
- Modify: `src/Heap/MAllocator/MSAllocBuffer.h`
- Modify: `src/Heap/MAllocator/MSAllocBuffer.cpp`
- Modify: `src/Heap/MSGCFastPath.h`
- Modify: `llvm/include/llvm/CodeGen/CJMSGCABI.h`
- Modify: `test/MSGCFastPathStateABIContractTest.cpp`
- Modify: `test/MSGCAllocBufferFastPathABIContractTest.sh`
- Create: `test/MSGCStep60FinalLayoutContractTest.cpp`

- [ ] **Step 1：编写最终布局契约**

`test/MSGCStep60FinalLayoutContractTest.cpp` 至少包含：

```cpp
static_assert(sizeof(MSRegionInfo) == 64, "MSRegionInfo must stay one cache line");
static_assert(offsetof(MSRegionInfo, allocPtr) == 0, "allocPtr ABI");
static_assert(offsetof(MSRegionInfo, pendingWordSummary) == 48,
              "pendingWordSummary ABI");
static_assert(offsetof(MSRegionInfo, control) == 56, "control ABI");
static_assert(offsetof(MSRegionInfo, sizeClass) == 62, "sizeClass ABI");
static_assert(offsetof(MSRegionInfo, layout) == 63, "layout ABI");

static_assert(sizeof(MSGCFastPathState) == 64, "fast state ABI");
static_assert(offsetof(MSGCFastPathState, allocPlaneBase) == 32,
              "alloc plane ABI");
static_assert(offsetof(MSGCFastPathState, markPlaneBase) == 40,
              "mark plane ABI");
static_assert(offsetof(MSGCFastPathState, scanPlaneBase) == 48,
              "scan plane ABI");
```

- [ ] **Step 2：实现 64B RegionInfo 和 64B FastPathState**

按第 7.1、7.3 节字段和偏移实现；删除 RegionInfo 中的 alloc/mark pointer。定义：

```cpp
static constexpr size_t kMSBitmapPlaneStride = 512;
static constexpr size_t kMSScanClassCount = 2;
static constexpr size_t kMSActiveRegionOffset = 8;
static constexpr size_t kMSActiveLogOffset = 1024;
```

- [ ] **Step 3：把 LLVM 私有 magic number 收口到 CJMSGCABI**

`CJMSGCABI.h` 定义与 Runtime 一致的 size、offset、layout bits 和 v2 symbol；
AArch64/X86 emitter 删除本地 896、Bitmap pointer offset 和 state size 32。

- [ ] **Step 4：运行 ABI 契约**

```bash
mkdir -p .tmp/step60
clang++ -std=c++14 -Wall -Wextra -Werror -I. -Isrc \
  test/MSGCStep60FinalLayoutContractTest.cpp \
  -o .tmp/step60/final_layout &&
  .tmp/step60/final_layout
bash test/MSGCFastPathStateABIContractTest.sh
bash test/MSGCAllocBufferFastPathABIContractTest.sh
```

Expected: 三个命令退出码为 0；输出确认 RegionInfo 64B、FastPathState 64B、
active Region 72 项、activeLog +1024。

### Task 2：实现 ScanClass 分配纯度

**Files**

- Create: `src/Heap/MAllocator/MSScanClass.h`
- Modify: `src/Heap/MAllocator/MSAllocBuffer.h`
- Modify: `src/Heap/MAllocator/MSAllocBuffer.cpp`
- Modify: `src/Heap/MAllocator/MSCentralRegionPool.h`
- Modify: `src/Heap/MAllocator/MSCentralRegionPool.cpp`
- Modify: `src/CompilerCalls.cpp`
- Modify: `src/CompilerCalls.h`
- Modify: `src/ObjectManager.inline.h`
- Modify: `llvm/lib/Transforms/Scalar/CJRuntimeLowering.cpp`
- Modify: `llvm/include/llvm/CodeGen/CJMSGCABI.h`
- Modify: `llvm/lib/CodeGen/CJBarrierLowering.cpp`
- Modify: `llvm/lib/Target/AArch64/AArch64MSGCEmitter.cpp`
- Modify: `llvm/lib/Target/AArch64/AArch64CangjieCallSize.h`
- Modify: `llvm/lib/Target/AArch64/AArch64CangjieCallSize.cpp`
- Modify: `llvm/lib/Target/X86/X86MSGCEmitter.cpp`
- Modify: `llvm/lib/Target/X86/X86MCInstLower.cpp`
- Modify: `llvm/test/Transforms/CJRuntimeLowering/new_array_*_msgc.ll`
- Create: `test/MSGCScanClassRegionTest.cpp`
- Create: `test/MSGCScanClassCompilerABIContractTest.sh`

- [ ] **Step 1：编写 Region 纯度测试**

测试固定覆盖：

```cpp
ExpectClass(byteArrayType, MSScanClass::NO_REF);
ExpectClass(valueStructArrayType, MSScanClass::NO_REF);
ExpectClass(refObjectType, MSScanClass::HAS_REF);
ExpectClass(refArrayType, MSScanClass::HAS_REF);
ExpectClass(weakRefType, MSScanClass::HAS_REF);
ExpectClass(finalizerType, MSScanClass::HAS_REF);
```

并验证：

```cpp
assert(ActiveIndex(0, MSScanClass::NO_REF) == 0);
assert(ActiveIndex(0, MSScanClass::HAS_REF) == 1);
assert(ActiveIndex(35, MSScanClass::NO_REF) == 70);
assert(ActiveIndex(35, MSScanClass::HAS_REF) == 71);
```

- [ ] **Step 2：实现唯一分类函数和二维桶**

`MSScanClass.h` 提供 `ClassifyScan(TypeInfo*)`；AllocBuffer 和 Central pool 使用
`[kNumSizeClasses][2]`。partial/full Region 保持分类，完全空 Region 清分类进入
classless cache。

- [ ] **Step 3：把 TypeInfo 传到底层分配**

所有 object/array/generic small slow call 使用：

```cpp
Allocate(const TypeInfo* typeInfo, size_t size);
```

不得在下层只拿 size 后重新猜测 ScanClass。

- [ ] **Step 4：修改 AArch64/X86 active Region 选择**

静态类型使用 lowering 生成的 `cj-msgc-scanclass`；动态数组复用现有
TypeInfo/component TypeInfo 判定，最终地址计算：

```text
allocBuffer + 8 + (sizeClass * 2 + scanClass) * 8
```

- [ ] **Step 5：运行分类和 compiler contract**

```bash
clang++ -std=c++14 -pthread -Wall -Wextra -Werror -I. -Isrc \
  test/MSGCScanClassRegionTest.cpp \
  -o .tmp/step60/scan_class &&
  .tmp/step60/scan_class
bash test/MSGCScanClassCompilerABIContractTest.sh
```

Expected: 所有类型进入唯一正确分类；空 Region 可以重新初始化分类；非空 Region 的
重分类断言失败；AArch64/X86 均生成 `sizeClass*2+scanClass`。

### Task 3：实现三平面、collectSet 冻结和 ACTIVE 并发协议

**Files**

- Modify: `src/Heap/MAllocator/MSBitmapArena.h`
- Modify: `src/Heap/MAllocator/MSBitmapArena.cpp`
- Modify: `src/Heap/MAllocator/MSRegionInfo.h`
- Modify: `src/Heap/MAllocator/MSRegionInfo.cpp`
- Modify: `src/Heap/MAllocator/MSRegionManager.cpp`
- Modify: `src/Heap/MAllocator/MSRegionSpace.cpp`
- Modify: `src/Heap/MAllocator/MSAllocBuffer.cpp`
- Modify: `src/Heap/MAllocator/MSCentralRegionPool.cpp`
- Modify: `src/Heap/MCollector/MarkSweepCollector.cpp`
- Modify: `src/Common/BaseObject.cpp`
- Modify: `src/Heap/Collector/FinalizerWorkList.cpp`
- Modify: `src/Heap/MSGCFastPath.h`
- Create: `test/MSGCThreePlaneLifecycleTest.cpp`
- Create: `test/MSGCAllocPlaneConcurrencyTest.cpp`
- Create: `test/MSGCActiveExternalScanTest.cpp`
- Create: `test/MSGCActiveRegionMultiCycleTest.cpp`
- Create: `test/MSGCNewObjectExclusionTest.cpp`
- Create: `test/MSGCNoAllocationMarkNewObjectContractTest.sh`
- Modify: `test/MSRegionInfoBatchMarkTest.cpp`

- [ ] **Step 1：编写平面寻址和位语义测试**

验证：

```cpp
assert(AllocWord(7, 3) ==
       allocPlaneBase + 7 * 512 + 3 * sizeof(uint64_t));
assert(MarkWord(7, 3) ==
       markPlaneBase + 7 * 512 + 3 * sizeof(uint64_t));
assert(ScanWord(7, 3) ==
       scanPlaneBase + 7 * 512 + 3 * sizeof(uint64_t));

assert((alloc & mark) == mark);
assert((mark & scan) == scan);
```

`mark ⊆ alloc` 只对本轮 collectSet 成立；`scan ⊆ mark` 同时适用于 collectSet
正式 mark/scan 和 ACTIVE 临时 visit/scan。

- [ ] **Step 2：实现三段虚拟地址和统一 word accessor**

`MSBitmapArena` 分别管理 alloc/mark/scan base。所有 Clear、Stats、Sweep 和 compiler
fast path 只使用统一 accessor/ABI 常量，不复制物理地址公式。

- [ ] **Step 3：把 alloc 语义改成 1=allocated**

BUMP 热路径只推进 `allocPtr`；自然归还 Central 且需要变成 PARTIAL 时，在
`PrepareForCentralReturn()` 按 allocPtr 物化。BITMAP allocation 使用
`freeMask = ~allocWord & validMask` 找 slot，并重建 `allocSummary`。MarkSetup 不物化
仍在 TLS 的 BUMP Region。

- [ ] **Step 4：实现 collectSet 冻结，不 flush ACTIVE**

严格执行第 7.5 节顺序：

- Central swept lists 在 PreEnum 冻结并翻转为 collectSet；
- 不调用 `ReturnAllRegionsToCentral()`；
- ACTIVE 可跨越任意多个周期；
- ACTIVE 自然归还时设置当前 `globalSweepGen`，只进入当前 swept list；
- 下一轮 PreEnum 才能把该 Region 翻转到 collectSet。

测试用两个线程证明 GC 只读 SEALED alloc word，allocator 只写 ACTIVE live occupancy，
二者不访问同一个并发变化的 word。

- [ ] **Step 5：实现 ACTIVE 临时 visit/scan**

PreEnum STW 枚举 72 个 active class：

- NO_REF 不触碰 scan plane；
- HAS_REF 清有效 mark/scan words、pending summary 和 control；
- 不读取、复制或物化 ACTIVE alloc plane。

Discover 遇到 ACTIVE：

- NO_REF 直接返回；
- HAS_REF 完成 slot 和 stateWord/TypeInfo 合法性检查；
- mark plane 作为 visit plane，winner 发布 `EXTERNAL_ACTIVE` Region token；
- scan plane 记录 external scan 完成；
- WeakRef/Finalizer/特殊对象保持既有特殊闭包语义。

同一个 ACTIVE Region 连续跨越多轮时，每轮重置临时 visit/scan；不得因为跨越轮数增加
而强制 detach，也不得枚举整个 ACTIVE Region 作为 Root。

- [ ] **Step 6：固化 MSGC 无 allocation-time MarkNewObject，并拆分 Finalizer 语义**

增加 contract 证明 MSGC 普通 small/LARGE allocation path 不调用
`MarkNewObject()`、不设置正式 mark bit、不发布 deferred scan；这不是删除一个当前
已经存在的普通分配调用。small 通过 ACTIVE/current-generation 排除；LARGE primary
记录同样的 allocation/sweep generation，并从本轮 large collectSet 排除；被
Root/闭包发现时走 external scan。CopyGC 通用 ABI 保持不变；现有
`OnFinalizerCreated()`/`FinalizerWorkList` 调用改走
`MarkFinalizableObject()`、`MarkResurrectedObject()` 或特殊闭包入口。

- [ ] **Step 7：实现 Sweep 三平面状态转换**

```cpp
uint64_t live = allocWord & markWord;
uint64_t dead = allocWord & ~markWord;
CleanupDeadSlots(dead);
StoreAllocWord(live);
StoreMarkWord(0);
if (scanClass == MSScanClass::HAS_REF) {
    StoreScanWord(0);
}
```

- [ ] **Step 8：运行生命周期和并发协议测试**

```bash
clang++ -std=c++14 -pthread -Wall -Wextra -Werror -I. -Isrc \
  test/MSGCThreePlaneLifecycleTest.cpp \
  -o .tmp/step60/three_plane &&
  .tmp/step60/three_plane
clang++ -std=c++14 -pthread -Wall -Wextra -Werror -I. -Isrc \
  test/MSGCAllocPlaneConcurrencyTest.cpp \
  -o .tmp/step60/alloc_concurrency &&
  .tmp/step60/alloc_concurrency
clang++ -std=c++14 -pthread -Wall -Wextra -Werror -I. -Isrc \
  test/MSGCActiveExternalScanTest.cpp \
  -o .tmp/step60/active_external &&
  .tmp/step60/active_external
clang++ -std=c++14 -pthread -Wall -Wextra -Werror -I. -Isrc \
  test/MSGCActiveRegionMultiCycleTest.cpp \
  -o .tmp/step60/active_multicycle &&
  .tmp/step60/active_multicycle
clang++ -std=c++14 -pthread -Wall -Wextra -Werror -I. -Isrc \
  test/MSGCNewObjectExclusionTest.cpp \
  -o .tmp/step60/new_object_exclusion &&
  .tmp/step60/new_object_exclusion
bash test/MSGCNoAllocationMarkNewObjectContractTest.sh
```

Expected：

- collectSet address/bit/invariant 测试通过；
- Mark 与 allocation 不并发访问同一个 ACTIVE alloc word；
- NO_REF ACTIVE 不产生 visit/scan 工作且不触碰 scan plane；
- Root 闭包可穿过 HAS_REF ACTIVE，新旧 ACTIVE 对象的出边不会丢失；
- 不可达 ACTIVE 对象不会因为 Region 枚举而保活 collectSet 子图；
- ACTIVE WeakRef 不把 referent 升级成强存活，Finalizer 保持既有闭包顺序；
- 边界后分配对象没有 allocation-time mark/deferred scan；被发现后允许 external scan，
  但不会被本轮 Sweep；
- contract 证明普通 small/LARGE allocation path 不调用 `MarkNewObject()`，Finalizer
  创建与 resurrection 只进入各自的特殊闭包；
- ACTIVE 连续跨越多轮不被强制归还，自然归还后只在下一轮成为 collectSet。

### Task 4：实现 MarkWordBatch 和无对象读取的发现路径

**Files**

- Create: `src/Heap/MCollector/MSMarkContext.h`
- Create: `src/Heap/MCollector/MSMarkContext.cpp`
- Modify: `src/Heap/MCollector/MarkSweepCollector.h`
- Modify: `src/Heap/MCollector/MarkSweepCollector.cpp`
- Modify: `src/Heap/MBarrier/MSMarkBarrier.h`
- Modify: `src/Heap/MBarrier/MSMarkBarrier.cpp`
- Modify: `src/Heap/MBarrier/MSRefField.inline.h`
- Modify: `src/CompilerCalls.h`
- Modify: `src/CompilerCalls.cpp`
- Modify: `src/Heap/MCollector/CMakeLists.txt`
- Create: `test/MSGCMarkWordBatchTest.cpp`
- Create: `test/MSGCNoRefObjectTouchTest.cpp`
- Modify: `test/MSGCDiscoverObjectFastPathContractTest.sh`
- Modify: `test/MSGCSatbHookDecoupleContractTest.sh`

- [ ] **Step 1：先写 batch 去重和 winner 测试**

固定构造同一 mark word 的 64 个候选、重复候选和两个 Worker 竞争，验证：

```text
同一 Context 的 64 个不同 bit -> 1 次 mark fetch_or
同一 bit 重复 100 次         -> 1 个 pending bit
两个 Context 竞争             -> newBits 的并集恰好等于首次置位集合
COLLECT_SET 空 slot            -> 被 alloc word 过滤
EXTERNAL_ACTIVE invalid slot   -> 加入 batch 前被对象验证拒绝
相同 Region/word 不同 role      -> 不合并为同一 entry
generation/epoch 不匹配        -> 不写 mark
```

- [ ] **Step 2：实现固定 8-entry 的 MSMarkContext**

`MSMarkContext` 只保存 Worker-local entry、统计和必要的 collector 引用。查找先线性比较
8 个 key；未命中且没有空 entry 时按 round-robin flush victim。不得在
`AddSmallCandidate()`、`FlushEntry()` 中申请内存或获取共享 mutex。

- [ ] **Step 3：把 DiscoverObject 拆成地址解析与 batch 提交**

最终接口改为：

```cpp
enum class MSDiscoverResult : uint8_t {
    REJECTED,
    ALREADY_DISCOVERED,
    BUFFERED_COLLECT_SET,
    BUFFERED_EXTERNAL_ACTIVE,
    LARGE_OR_SPECIAL,
};

MSDiscoverResult DiscoverSmallRef(BaseObject* ref, MSMarkContext& context);
```

small collectSet 路径只读取 RegionInfo、alloc/mark plane，不再通过
`BaseObject::GetTypeInfo()` 验证对象。LARGE/WeakRef/Finalizer 显式转入既有特殊对象
路径，不能用 small slot 规则推断。ACTIVE/current-generation 不读 alloc word；
NO_REF 返回，HAS_REF 经对象头验证后提交 `BUFFERED_EXTERNAL_ACTIVE`。

- [ ] **Step 4：在 flush 中按 ScanClass 发布后续职责**

一次 `fetch_or` 得到 `newBits` 后：

```text
COLLECT_SET + NO_REF -> 更新 leaf winner 统计后结束
COLLECT_SET + HAS_REF -> PublishRegionDirty(role=COLLECT_SET)
EXTERNAL_ACTIVE + HAS_REF -> PublishRegionDirty(role=EXTERNAL_ACTIVE)
```

禁止重新把 `newBits` 展开成普通逐对象 WorkBuf。只有 LARGE 和特殊对象保留
ObjectTask/activeLog。

同时导出稳定 C helper：

```cpp
MRT_EXPORT void CJ_MRT_MSGCPublishRegionWord(
    uint32_t regionIdx, uint16_t generation, uint8_t wordIdx, uint8_t scanRole);
```

compiler scalar barrier 只有在 HAS_REF mark winner 时调用它；helper 与 batch flush
共用同一个 `PublishRegionDirty()` 实现，不允许复制另一份 control CAS 状态机。
compiler 遇到 EXTERNAL_ACTIVE target 必须转 Runtime slow path 完成对象头验证，不能套用
collectSet 的 alloc-bit inline fast path。

- [ ] **Step 5：封闭所有 batch 生命周期边界**

root enumeration、Region owner、GC helper、mutator assist 和 bulk barrier 都使用
显式 `FlushAll()`。scalar SATB barrier 采用调用内临时 batch 并在返回前 flush；
不得把被覆盖的旧引用只留在 mutator TLS 中等待未来调用。

- [ ] **Step 6：运行 batch 和无对象触碰测试**

```bash
clang++ -std=c++14 -pthread -Wall -Wextra -Werror -I. -Isrc \
  test/MSGCMarkWordBatchTest.cpp \
  -o .tmp/step60/mark_batch &&
  .tmp/step60/mark_batch
clang++ -std=c++14 -pthread -Wall -Wextra -Werror -I. -Isrc \
  test/MSGCNoRefObjectTouchTest.cpp \
  -o .tmp/step60/no_ref_touch &&
  .tmp/step60/no_ref_touch
bash test/MSGCDiscoverObjectFastPathContractTest.sh
bash test/MSGCSatbHookDecoupleContractTest.sh
```

Expected: batch winner 集合正确；NO_REF collectSet 用例的
`targetObjectHeaderLoads/targetTypeInfoLoads` 都是 0；所有退出边界 batch 为空。

### Task 5：实现双 role RegionScanQueue 和终止闭包

**Files**

- Create: `src/Heap/MCollector/MSRegionScanQueue.h`
- Create: `src/Heap/MCollector/MSRegionScanQueue.cpp`
- Modify: `src/Heap/MCollector/MSMarkContext.h`
- Modify: `src/Heap/MCollector/MSMarkContext.cpp`
- Modify: `src/Heap/MCollector/MarkSweepCollector.h`
- Modify: `src/Heap/MCollector/MarkSweepCollector.cpp`
- Modify: `src/Heap/MCollector/CMakeLists.txt`
- Modify: `src/Heap/MCollector/MSMarkTerminationPolicy.h`
- Create: `test/MSGCRegionScanStateTest.cpp`
- Create: `test/MSGCRegionScanRaceTest.cpp`
- Create: `test/MSGCRegionScanLocalityTest.cpp`
- Modify: `test/MSGCActiveExternalScanTest.cpp`
- Modify: `test/MSMarkTerminationPolicyTest.cpp`
- Modify: `test/MSGCTailDrainContractTest.sh`

- [ ] **Step 1：先写状态机和丢工作竞态测试**

使用可控 interleaving 覆盖：

```text
producer 在 owner 释放前发布 DIRTY
producer 在 owner 释放后发布新 token
多个 producer 同时首次发布同一 Region
owner 扫描期间同一 word 再出现 winner
stale generation token
MarkTermination 与最后一次 batch flush 竞争
ACTIVE visit 与 owner 普通 alloc 写同时发生，但 GC 不读 alloc word
ACTIVE External Scan 期间 mutator 覆盖 old edge
Root -> ACTIVE -> collectSet 闭包与 ACTIVE 环去重
ACTIVE 连续跨轮后自然归还，并与下一轮 collectSet 冻结交错
```

每个调度结束都必须满足第 8.8 节全局不变量。

- [ ] **Step 2：实现预分配 node 的 RegionScanQueue**

每个可提交 Region 对应一个稳定 node，不在 mark 热路径 `new/delete`。队列至少支持：

```cpp
bool Publish(uint32_t regionIdx, uint16_t generation,
             uint16_t workEpoch, MSRegionScanRole role);
bool TryPop(RegionScanToken& token);
bool TrySteal(RegionScanToken& token);
```

只有成功设置 `QUEUED` 的 producer 增加 `regionOutstanding`；owner 完成或丢弃 stale
token 时精确减少一次。

- [ ] **Step 3：实现 pending word 发布协议**

`MarkWordBatch::FlushEntry()` 对 HAS_REF 的 `newBits`：

```text
release OR pendingWordSummary
release OR DIRTY
若无 QUEUED/OWNED，则 CAS 设置 QUEUED
成功后 outstanding++，再 release push token
```

发布顺序不能改成先 push token 后设置 summary。

- [ ] **Step 4：实现唯一 Region owner**

owner 按第 4.5 节循环，只处理 summary 指定的 word，并按 token role 计算：

```text
COLLECT_SET:     alloc & mark & ~scan
EXTERNAL_ACTIVE: visit(mark) & ~scan
```

扫描字段生成的子引用继续进入 owner 的 `MSMarkContext`，置 scan bit 和释放 owner 前
必须 flush。EXTERNAL_ACTIVE 分支禁止读取 live alloc word。

- [ ] **Step 5：替换普通 small-object WorkBuf drain**

普通 HAS_REF small object 不再进入 `DrainCurrentBuf()`。保留 ObjectTask 通道仅服务：

- LARGE object；
- WeakRef/Finalizer；
- 需要主线程或专用闭包语义的对象；
- 调试构建显式注入的验证任务。

- [ ] **Step 6：接入 EXTERNAL_ACTIVE role**

`EXTERNAL_ACTIVE` 与正式 collectSet scan 共享 Worker、queue 和 `MSMarkContext`：

```text
COLLECT_SET:
  toScan = alloc & mark & ~scan
  扫描完成后写 scan plane

EXTERNAL_ACTIVE:
  toScan = visit(mark) & ~scan
  普通对象枚举强引用字段；特殊对象转专用闭包
  子引用重新按 Region role Discover
  不读取 ACTIVE live alloc plane
  写 ACTIVE 临时 scan bit
  Sweep 忽略 ACTIVE visit/scan
```

第一次 external winner 把 Region token 登记到预分配的 `externalVisitedRegions`，
用于 termination 验证；同一 Region 每轮只登记一次，不分配 node，不枚举 Region 中
未被 Root 闭包发现的对象。

- [ ] **Step 7：把两种 role 接入自然终止和 STW 尾收敛**

自然终止使用 `publishSeq` 双读，并检查 queue、outstanding、owner、local batch 和
pending summary。STW 尾阶段按 role 验证并 drain：

```text
collectSet:       alloc & mark & ~scan == 0
externalVisited: visit(mark) & ~scan == 0
pendingWordSummary == 0
control & (DIRTY | QUEUED | OWNED) == 0
RegionScanQueue empty
regionOutstanding == 0
```

- [ ] **Step 8：运行状态、竞态和终止测试**

```bash
clang++ -std=c++14 -pthread -Wall -Wextra -Werror -I. -Isrc \
  test/MSGCRegionScanStateTest.cpp \
  -o .tmp/step60/region_state &&
  .tmp/step60/region_state
clang++ -std=c++14 -pthread -Wall -Wextra -Werror -I. -Isrc \
  test/MSGCRegionScanRaceTest.cpp \
  -o .tmp/step60/region_race &&
  .tmp/step60/region_race
clang++ -std=c++14 -pthread -Wall -Wextra -Werror -I. -Isrc \
  test/MSGCRegionScanLocalityTest.cpp \
  -o .tmp/step60/region_locality &&
  .tmp/step60/region_locality
bash test/MSGCTailDrainContractTest.sh
```

Expected: 无丢灰对象、无重复 owner、outstanding 最终归零；同 Region 的多个 winner
合并为一个或少量 owner loop，而不是逐对象任务。

### Task 6：一次性完成 LLVM fast path、Runtime 构建和 SDK 验证

**Files**

- Modify: `src/Heap/MSGCFastPath.h`
- Modify: `src/Heap/Heap.cpp`
- Modify: `src/Heap/MAllocator/CMakeLists.txt`
- Modify: `src/Heap/MCollector/CMakeLists.txt`
- Modify: `src/Heap/MBarrier/CMakeLists.txt`
- Modify: `src/CMakeLists.txt`
- Modify: `llvm/include/llvm/CodeGen/CJMSGCABI.h`
- Modify: `llvm/lib/Transforms/Scalar/CJRuntimeLowering.cpp`
- Modify: `llvm/lib/CodeGen/CJBarrierLowering.cpp`
- Modify: `llvm/lib/Target/AArch64/AArch64MSGCEmitter.cpp`
- Modify: `llvm/lib/Target/AArch64/AArch64CangjieCallSize.h`
- Modify: `llvm/lib/Target/AArch64/AArch64CangjieCallSize.cpp`
- Modify: `llvm/lib/Target/X86/X86MSGCEmitter.cpp`
- Modify: `llvm/lib/Target/X86/X86MCInstLower.cpp`
- Modify: `llvm/test/Transforms/CJRuntimeLowering/new_array_*_msgc.ll`
- Create: `llvm/test/CodeGen/AArch64/CangjieGC/msgc-step60-final-abi.ll`
- Create: `llvm/test/CodeGen/X86/cangjie-msgc-step60-final-abi.ll`
- Create: `test/MSGCFourOptimizationIntegrationTest.cpp`
- Create: `test/MSGCFourOptimizationFinalContractTest.sh`

- [ ] **Step 1：完成 Runtime 内唯一 v2 ABI 发布**

初始化 `CJ_MRT_MSGCFastPathState` 的三个 plane base、layout signature 和
`abiVersion=2`。删除旧 32B state、36-entry active Region 和 +896 activeLog 的导出
假设；Runtime 不提供同进程兼容分支。

- [ ] **Step 2：完成 AArch64/X86 最终 fast path**

两套 emitter 只生成最终布局：

```text
allocation:
  load/derive scanClass
  activeIndex = sizeClass * 2 + scanClass
  activeRegion = allocBuffer + 8 + activeIndex * 8

barrier/discovery:
  load v2 FastPathState
  resolve RegionInfo
  resolve scanRole
  COLLECT_SET:
    derive fixed alloc/mark word
    validate alloc bit
    scalar one-bit formal mark RMW
    NO_REF winner return
    HAS_REF winner call CJ_MRT_MSGCPublishRegionWord(..., COLLECT_SET)
  EXTERNAL_ACTIVE:
    call Runtime DiscoverExternalActiveRef(ref)
    Runtime validates object and publishes EXTERNAL_ACTIVE role

activeLog:
  allocBuffer + 1024
```

`CJBarrierLowering` 继续只选择目标无关 MSGC wrapper；`CJRuntimeLowering` 生成的
allocation wrapper 必须保持 `musttail` 和 `cj-frame-transparent`。AArch64
`AArch64CangjieCallSize` 与 emitter 使用同一 emission decision，并按新序列扩大
shadow size；X86 bridge 仍须在未完成发射时保留原 slow call。Mach-O、ELF
`+lse/-lse` 和 MinGW COFF 都必须覆盖，不能只验证当前 macOS 机器。

如果 `abiVersion/layoutSignature` 不匹配，走 Runtime slow call 并在 debug 构建报错；
不得回退解释旧 RegionInfo 或旧 Bitmap 布局。

- [ ] **Step 3：构建 compiler/LLVM 并运行 lit 与静态 contract**

```bash
cd /Users/lipeisheng/code/cangjie/cangjie/cangjie_compiler
python3 build.py build -t release

LLVM_SRC=/Users/lipeisheng/code/cangjie/cangjie/cangjie_compiler/third_party/llvm-project/llvm
LLVM_BUILD=/Users/lipeisheng/code/cangjie/cangjie/cangjie_compiler/build/build/third_party/cjnative-prefix/src/cjnative-build
python3 "$LLVM_SRC/utils/lit/lit.py" -sv \
  "$LLVM_BUILD/test/Transforms/CJRuntimeLowering" \
  "$LLVM_BUILD/test/CodeGen/AArch64/CangjieGC" \
  "$LLVM_BUILD/test/CodeGen/X86"

cd /Users/lipeisheng/code/cangjie/cangjie/cangjie_runtime_MSGC/runtime
bash test/MSGCFourOptimizationFinalContractTest.sh
```

Expected: compiler build 退出码为 0；lowering 保持 `musttail`；AArch64/X86 只出现
v2 offset，AArch64 object-level 测试覆盖 Mach-O relocation 和 `+lse/-lse`；
contract 找不到 `36` entry、`+896`、RegionInfo bitmap pointer 或普通 small-object
WorkBuf push。

- [ ] **Step 4：构建最终 MarkSweep Runtime**

开始前先确认没有本轮之前残留的 `llc`、benchmark、`rss_monitor.py` 进程。然后：

```bash
python3 build.py build \
  --target native \
  --build-type release \
  --gc-mode marksweep \
  -v 0.0.1
```

Expected: 命令退出码为 0；产物为
`output/temp/lib/aarch64_Release/libcangjie-runtime.dylib`。

- [ ] **Step 5：替换 MSGC SDK runtime 并重新编译用例**

三件套来源必须记录为：

```text
compiler:
  /Users/lipeisheng/code/cangjie/cangjie/cangjie_compiler/output/msgc
runtime:
  本分支 output/temp/lib/aarch64_Release/libcangjie-runtime.dylib
stdlib:
  /Users/lipeisheng/code/cangjie/cangjie/cangjie_compiler/output/msgc
```

先对 SDK 原 dylib 建立带时间戳的备份，复制新 dylib 后执行 `codesign -f -s -`。由于
Runtime/LLVM ABI 同时变化，必须先完成对应 compiler 构建并安装到同一
`output/msgc`，再用这个 SDK 重新编译所有 `.cj` benchmark；禁止复用旧 binary。

- [ ] **Step 6：在隔离环境确认 dylib 来源**

```bash
env -i HOME="$HOME" TERM="$TERM" \
  PATH=/usr/bin:/bin:/usr/sbin:/sbin:/opt/homebrew/bin \
  bash -c '
    source /Users/lipeisheng/code/cangjie/cangjie/cangjie_compiler/output/msgc/envsetup.sh \
      >/dev/null 2>&1
    cd /Users/lipeisheng/code/cangjie/cangjie/cangjie_runtime_MSGC/runtime/gc || exit 1
    cjc gc.cj -o step60_probe
    DYLD_PRINT_LIBRARIES=1 cjHeapSize=1m ./step60_probe 2>&1 |
      grep "cangjie-runtime" | head -3
  '
```

Expected: 输出只指向 `cangjie_compiler/output/msgc` 下刚替换并重签名的 Runtime；
runtime log 有 `[MS-GC]` 和 v2 layout signature。

- [ ] **Step 7：运行最终集成、回归和 case26**

```bash
clang++ -std=c++14 -pthread -Wall -Wextra -Werror -I. -Isrc \
  test/MSGCFourOptimizationIntegrationTest.cpp \
  -o .tmp/step60/four_opt_integration &&
  .tmp/step60/four_opt_integration
bash test/MSGCFourOptimizationFinalContractTest.sh
```

随后在同一个干净 SDK 子 shell 内重新编译并运行 case26 的 128 MiB 与 2 GiB
持久对象配置。每组至少 5 次，保存 benchmark stdout/stderr、runtime GC log、
dylib probe、exit code 和本节规定的指标；不得只比较单次 wall time。

## 10. 测试、观测指标与验收

### 10.1 分层测试矩阵

| 层次 | 主要内容 | 必须证明 |
|---|---|---|
| 布局/ABI | RegionInfo、FastPathState、AllocBuffer、LLVM offset | Runtime/LLVM 只有一套 v2 布局 |
| ScanClass | object/array/generic/special 分类、empty 重分类 | 非空 Region 分类纯净且不可变 |
| Bitmap | 三平面寻址、tail mask、collectSet、ACTIVE visit/scan、Sweep | collectSet 满足 `mark ⊆ alloc`；两种 role 均满足 `scan ⊆ mark` |
| Mark batch | 同 word 合并、跨 Worker winner、flush 边界 | 不丢 bit、不重复 winner、空 slot 不被标活 |
| Region scan | producer/owner 竞态、generation、termination | 不丢灰工作、同 Region 单 owner |
| Barrier | scalar SATB、bulk copy、assist return | 被覆盖引用在返回前已提交 |
| 压力 | 高频 Region 重用、并发分配/Mark/Sweep | 无 stale token、无数据竞争、无悬空 TypeInfo |
| 端到端 | 128 MiB/2 GiB case26 | 大 live set Mark 单位对象效率改善 |

至少运行 Debug 和 Release 两套 Runtime。支持 TSAN 的平台额外运行 Region producer/
owner、ACTIVE visit 与 owner alloc 并发、自然归还/下一轮纳入和 batch flush 竞态用例；
TSAN 不可用时，使用可控 barrier 和百万次随机调度重复测试。

### 10.2 四项优化必须各自拥有的证据

| 优化 | 主证据 | 失败信号 |
|---|---|---|
| ScanClass Region | `noRef/hasRefRegionCount`、分类纯度断言 | non-empty Region 出现混合 TypeInfo |
| 三平面 Bitmap | touched/committed/clear bytes、按 role 的集合不变量 | collectSet 出现 `mark & ~alloc`，或任一 role 出现 `scan & ~mark` |
| Region 级扫描 | token、owner loop、pending word、scan bit | 普通 small winner 仍逐对象入 WorkBuf |
| Mark 批量更新 | candidate、batch hit、flush、RMW、newBits | RMW 数接近未标记 candidate 数 |

这四组证据必须可以分别读出，不能只用“总 Mark 变快了”反推某项生效。

### 10.3 最终日志字段

每个 GC 周期新增一行稳定、可脚本解析的汇总：

```text
[MS-GC][step60-mark]
scanClass(noRefRegions=...,hasRefRegions=...,forcedSpecial=...)
discover(candidates=...,invalid=...,alreadyMarked=...,collectSet=...,externalActive=...,
         targetObjectHeaderLoads=...,targetTypeInfoLoads=...)
batch(hits=...,misses=...,flushes=...,rmw=...,newBits=...,duplicateBits=...)
region(tokens=...,claims=...,claimFail=...,dirtyQueued=...,dirtyOwned=...,
       collectSetTokens=...,externalTokens=...,pendingWords=...,
       toScan=...,completed=...,ownerLoops=...)
externalActive(candidates=...,validated=...,alreadyVisited=...,visitBits=...,
               scanBits=...,edges=...,regions=...,maxCyclesActive=...)
plane(allocWords=...,markProbe=...,markRmw=...,scanWords=...,
      committedPages=.../.../...,clearBytes=.../.../...)
```

统计使用 Worker-local counter，周期结束时归并。Mark 热路径不得为了统计执行全局
atomic increment；Release 默认只保留低成本总量，逐 Region 细节由诊断开关控制。

派生指标统一定义：

```text
batchHitRate = batchHits / (batchHits + batchMisses)
formalBitsPerRmw = collectSetNewBits / collectSetMarkRmw
externalVisitBitsPerRmw = externalVisitBits / externalVisitRmw
regionTaskCompression = hasRefNewBits / regionTokens
objectsPerOwnerLoop = scanBitsCompleted / regionOwnerLoops
noRefObjectTouchRate =
  targetTypeInfoLoadsOnNoRef / noRefValidatedCandidates
```

### 10.4 case26 对比基线

使用同一 runtime 的既有结果作为优化前基线：

| 指标 | 128 MiB | 2 GiB |
|---|---:|---:|
| 端到端有效对象速率 | 68.201 Mobj/s | 16.033 Mobj/s |
| natural 有效对象速率 | 76.062 Mobj/s | 23.464 Mobj/s |
| Mark wall P95 | 56.437 ms | 3676.664 ms |
| debt-stalled 比例 | 16.59% | 73.68% |

基线路径：

```text
128 MiB:
MSGC_RESULT/Result/case26_http_complex_json/result/
20260727_095319_output_msgc_vs_copygc_go_java_c1000_p100K_compare/msgc

2 GiB:
MSGC_RESULT/Result/case26_http_complex_json/result/
20260727_095905_output_msgc_vs_copygc_go_java_c1000_p100K_compare/msgc
```

新结果必须保持相同 heap、concurrency、payload、persistent workers、测试时长和
Runtime 构建类型，并记录 compiler/runtime/stdlib 来源及 dylib MD5。每组至少 5 次，
报告中位数、P95 和样本数。

### 10.5 硬正确性验收

以下任一不满足都不得进入性能结论：

1. 所有 ABI、单元、contract、竞态和端到端用例退出码为 0；
2. 无 crash、OOM、invalid stateWord/typeInfo、lost grey、double owner；
3. 每轮 MarkTermination 后 collectSet 满足 `mark ⊆ alloc`、`scan ⊆ mark`；
4. 每个 HAS_REF collectSet Region 的 `alloc & mark & ~scan == 0`；
5. Region queue、outstanding、两种 role 的 control/pending summary 和所有 local batch
   全部归零；
6. NO_REF collectSet 发现路径 `targetObjectHeaderLoads=0` 且
   `targetTypeInfoLoads=0`；
7. NO_REF Region 的 `scanWords=0`；
8. GC 不并发读取 ACTIVE live allocBitmap；ACTIVE mark/scan 只表达临时 visit/scan；
9. GC Root 闭包可穿过所有可达 ACTIVE HAS_REF 对象，且不可达 ACTIVE 不被整区枚举；
10. ACTIVE WeakRef/Finalizer 保持既有弱引用和 resurrection 语义；
11. PreEnum 后新对象不调用 MSGC allocation-time `MarkNewObject`；被闭包发现时允许
    External Active Scan；
12. ACTIVE Region 可连续跨越多轮，自然归还后本轮不回收、下一轮才进入 collectSet；
13. 普通 HAS_REF small winner 不进入逐对象 WorkBuf；
14. 相同业务输入的最终 live object/live byte 与基线在可解释的新分配和有界
    ACTIVE 浮动垃圾范围内。

### 10.6 性能与空间验收

先以结构指标判定四项优化是否真正工作：

| 场景 | 验收 |
|---|---|
| 同 word 64-slot microbench | 64 个新 bit 最多 1 次 batch mark RMW |
| 重复 edge microbench | 同一个本地 pending bit 重复出现不增加 RMW |
| HAS_REF Region locality microbench | `hasRefNewBits / regionTokens >= 4` |
| NO_REF leaf microbench | 0 object-header/TypeInfo load，0 Region token，0 scan word |
| owner race stress | 结束时 collectSet 无 `alloc & mark & ~scan`、external 无 `visit & ~scan`，outstanding 为 0 |

case26 的第一阶段性能门槛：

```text
128 MiB:
  端到端有效对象速率回退不超过 5%

2 GiB:
  端到端有效对象速率 >= 24.05 Mobj/s   // 相对 16.033 至少 +50%
  natural 有效对象速率 >= 35.20 Mobj/s // 相对 23.464 至少 +50%
```

如果单位对象速率没有达到门槛，先依据 `targetTypeInfoLoads`、
`formalBitsPerRmw`、`externalVisitBitsPerRmw` 和 `regionTaskCompression`
判断是哪项未生效，不能直接继续调 Pacer。

空间验收：

```text
三平面逻辑地址 <= heap reserved * 4.6875%
相对两平面的新增逻辑地址 <= heap reserved * 1.5625%
实际 RSS 以 committedPages * OSPageSize 计算
NO_REF 不得主动触页 scan plane
```

### 10.7 不作为本 Step 的硬门槛

Step60 不包含 RangeQueue 和 Pacer，因此以下数据只观察，不作为四项实现正确与否的
单独否决条件：

- 单个 LARGE 引用数组的内部并行度；
- Mark P95 必须直接降到 Go 的量级；
- `debt-stalled` 必须完全消失；
- assist 比例和 GC 触发频率达到最终目标。

如果 2 GiB 的 `targetTypeInfoLoads=0`、batch/Region 压缩均达标，natural 单位对象
速率也改善，但 P95 仍被单个 LARGE root array 或 debt-stalled 主导，下一步应实施
RangeQueue/Pacer，而不是破坏本 Step 的 Region/Bitmap 状态机。

## 11. 文件影响清单

### 11.1 Runtime：Allocator 与 Region 元数据

| 文件 | 最终职责 |
|---|---|
| `src/Heap/MAllocator/MSScanClass.h` | 唯一 ScanClass 定义和分类函数 |
| `src/Heap/MAllocator/MSRegionInfo.h/.cpp` | 64B RegionInfo、generation、epoch、summary/control |
| `src/Heap/MAllocator/MSBitmapArena.h/.cpp` | 三个固定 plane 的预留、提交、寻址和统计 |
| `src/Heap/MAllocator/MSAllocBuffer.h/.cpp` | 72 个 ACTIVE Region、owner-only 分配、自然归还并设置当前 sweepGen |
| `src/Heap/MAllocator/MSCentralRegionPool.h/.cpp` | `[36][2]` 非空桶和 classless empty cache |
| `src/Heap/MAllocator/MSRegionManager.cpp` | plane 生命周期、Region node 预分配、grow/shrink |
| `src/Heap/MAllocator/MSRegionSpace.cpp` | collectSet/excluded generation 和 LARGE 周期边界 |

### 11.2 Runtime：Mark、Barrier 与终止

| 文件 | 最终职责 |
|---|---|
| `src/Heap/MCollector/MSMarkContext.h/.cpp` | Worker-local 8-entry MarkWordBatch |
| `src/Heap/MCollector/MSRegionScanQueue.h/.cpp` | 双 role Region token、预分配 node、push/pop/steal |
| `src/Heap/MCollector/MarkSweepCollector.h/.cpp` | collectSet 冻结、ACTIVE External Discover、双 role Region owner、Sweep 集成 |
| `src/Heap/MCollector/MSMarkTerminationPolicy.h` | 双 role queue、batch、summary、outstanding fixed point |
| `src/Common/BaseObject.cpp` | `OnFinalizerCreated()` 改走 Finalizer 专用闭包；普通分配无 MarkNewObject contract |
| `src/Heap/Collector/FinalizerWorkList.cpp` | resurrection 改走独立特殊闭包，不复用新分配语义 |
| `src/Heap/MBarrier/MSMarkBarrier.h/.cpp` | scalar/bulk barrier 的最终 mark 提交边界 |
| `src/Heap/MBarrier/MSRefField.inline.h` | barrier inline 路径和 v2 ABI 使用 |
| `src/CompilerCalls.h/.cpp` | 导出 scalar winner 的 Region-word 发布 helper |
| `src/Heap/MSGCFastPath.h` | 64B v2 FastPathState 与 layout signature |
| `src/Heap/Heap.cpp` | v2 state 初始化和三个 plane base 发布 |
| 各模块 `CMakeLists.txt` | 新增 MarkContext、RegionScanQueue 编译单元 |

### 11.3 Compiler/LLVM

以下路径相对于
`/Users/lipeisheng/code/cangjie/cangjie/cangjie_compiler/third_party/llvm-project`：

| 文件 | 最终职责 |
|---|---|
| `llvm/include/llvm/CodeGen/CJMSGCABI.h` | Runtime/LLVM 共享的 v2 常量定义 |
| `llvm/lib/Transforms/Scalar/CJRuntimeLowering.cpp` | 静态/动态 ScanClass 和最终 slow call |
| `llvm/lib/CodeGen/CJBarrierLowering.cpp` | 目标无关 MSGC wrapper 分类与属性校验 |
| `llvm/lib/Target/AArch64/AArch64MSGCEmitter.cpp` | AArch64 72-entry allocation 与 v2 barrier |
| `llvm/lib/Target/AArch64/AArch64CangjieCallSize.h/.cpp` | 与 emitter 共用决策并计算新序列上界 |
| `llvm/lib/Target/X86/X86MSGCEmitter.cpp` | X86-64 72-entry allocation 与 v2 barrier |
| `llvm/lib/Target/X86/X86MCInstLower.cpp` | X86 emitter bridge 和 slow-call 保留契约 |
| `llvm/test/Transforms/CJRuntimeLowering/new_array_*_msgc.ll` | ScanClass、musttail 和 O2 wrapper contract |
| `llvm/test/CodeGen/AArch64/CangjieGC/msgc-step60-final-abi.ll` | AArch64 offset/寻址/relocation contract |
| `llvm/test/CodeGen/X86/cangjie-msgc-step60-final-abi.ll` | X86-64 offset/寻址 contract |

Runtime 不能单独先发布；只要上述 LLVM emitter 尚未切到 v2，生成的 binary 就会用旧
offset 访问错误字段。

### 11.4 测试

新增或修改的测试按目标分组：

```text
ABI:
  MSGCStep60FinalLayoutContractTest
  MSGCFastPathStateABIContractTest
  MSGCAllocBufferFastPathABIContractTest
  MSGCScanClassCompilerABIContractTest

ScanClass / plane:
  MSGCScanClassRegionTest
  MSGCThreePlaneLifecycleTest
  MSGCAllocPlaneConcurrencyTest
  MSGCActiveExternalScanTest
  MSGCActiveRegionMultiCycleTest
  MSGCNewObjectExclusionTest
  MSRegionInfoBatchMarkTest

batch / discover:
  MSGCMarkWordBatchTest
  MSGCNoRefObjectTouchTest
  MSGCDiscoverObjectFastPathContractTest
  MSGCSatbHookDecoupleContractTest

Region scan / termination:
  MSGCRegionScanStateTest
  MSGCRegionScanRaceTest
  MSGCRegionScanLocalityTest
  MSMarkTerminationPolicyTest
  MSGCTailDrainContractTest

final:
  MSGCFourOptimizationIntegrationTest
  MSGCFourOptimizationFinalContractTest
  case26 128 MiB / 2 GiB
```

### 11.5 最终边界总结

最终只有一条 small-object Mark 路径：

```text
分配时分类 Region
  -> ACTIVE：owner-only 分配，不进入本轮 Mark/Sweep
       -> Root/引用发现 HAS_REF 时设置临时 visit
       -> Region owner 执行 visit & ~scan
       -> 新对象不做 allocation-time MarkNewObject
       -> 自然归还当前 swept list
       -> 下一轮进入 SEALED collectSet
  -> SEALED collectSet：引用解析只看 Region/alloc/mark 元数据
  -> Worker-local 合并 mark word
  -> NO_REF 到此结束
  -> HAS_REF 发布 Region
  -> Region owner 按 role 使用 alloc & mark & ~scan 或 visit & ~scan 连续扫描
  -> batch、双 role Region 和特殊对象队列共同收敛
  -> Sweep 由 alloc & mark 回收
```

四项之间没有功能冲突，但存在明确依赖：

```text
ScanClass Region ─┐
三平面 Bitmap ───┼─> 无对象读取 Discover + Mark batch
                 └─> HAS_REF Region scan
Mark batch ─────────> Region dirty 的合并 producer
scan plane ─────────> Region owner 的增量去重
```

实现顺序可以分 Task 验证组件，交付结构不能分裂为多个长期兼容模式。最终 SDK 不保留：

- 混合 ScanClass 的普通 small Region；
- 两平面 small-object fallback；
- 普通 HAS_REF small object 的逐对象 WorkBuf；
- 逐候选直接 mark RMW 的另一套常驻路径；
- Runtime/LLVM v1/v2 双 ABI。

这保证后续 RangeQueue、Pacer 或 Sweep 优化建立在一个稳定的 Mark 数据模型上，而不是
继续为中间状态增加分支。
