# Step53：MSGC MarkBitmap 多核扩展与写屏障批量标记设计

## 0. 文档定位与结论

本文承接 Step52 对 `DiscoverObject()`、Mark bitmap 原子更新和多线程扩展效率的
分析，给出一套允许调整 Bitmap 数据布局的后续方案，并记录 2026-07-25 已完成
的源码实施与验证边界。

推荐组合是：

1. Small Region 的 Mark bitmap 改为 **8 路、每路 64B 对齐的条带布局**，
   把相邻逻辑 word 分散到不同 cache line，降低 GC main、helper、assist 和
   inline 写屏障之间的 false sharing。
2. GC 扫描侧为每个工作线程增加小型、固定容量的 `MarkWordBatcher`，把同一
   `(MSRegionInfo*, logicalWordIndex)` 的多个 bit 合并后再做一次
   `fetch_or`，降低原子 RMW 次数。
3. Runtime bulk 写屏障保留“调用作用域内批处理、返回前强制 flush”的模型，
   复用统一的逻辑 word 提交接口；单引用写屏障保持立即提交，不在 mutator
   TLS 中留下跨屏障调用的 pending mark。
4. AArch64/X86 MSGC inline 写屏障当前直接按连续
   `markBitmap + logicalWordIndex * 8` 寻址。条带布局实施时必须同步修改
   `AArch64MSGCEmitter.cpp` 和 `X86MSGCEmitter.cpp` 的最终机器码发射，
   不能只修改 Runtime。

这两个优化分别解决不同问题：

```text
MarkWordBatcher：减少写 Bitmap 的次数
条带化 Bitmap：剩余写操作尽量不争抢同一 cache line
```

二者可以独立开关和度量，但最终推荐组合使用。它们不能消除对象内容、TypeInfo
和 RegionInfo 的随机内存访问，因此目标是显著提高多线程 Mark 的并行效率，
而不是承诺线程数与吞吐量严格线性增长。

## 1. 当前实现与竞争来源

### 1.1 普通 GC 扫描

当前 Small Object 的 Mark 主链路为：

```text
DrainCurrentBuf
  -> TraceObjectRefFieldsToGCWork
  -> TryEnqueueGreyObject
  -> DiscoverObject / DiscoverManagedObject
  -> ResolvedMarkTarget::TryMark
  -> MSRegionInfo::MarkBitmapWordResolved
  -> __atomic_fetch_or(&markBitmap[wordIdx], bitMask, RELAXED)
```

`DiscoverObject()` 已经做了以下优化：

- 一次解析得到 `MSRegionInfo*`、逻辑 word 和 bit mask；
- 已标记对象通过 relaxed load 快速返回；
- white slot 在置位前读取并保留 `TypeInfo`，避免 winner 再次读取；
- Release 消费端不再重新校验 trusted-grey。

但普通引用扫描仍然是“一个候选对象一次单 bit `fetch_or`”。多个新对象落在同一
word 时没有合并；多个线程写相邻 word 时也可能争抢同一 64B cache line。

Large Object 使用独立的 canonical base entry 和原子 marked flag，不使用
Small Region bitmap。本方案不改变 Large Object 的标记方式。

### 1.2 Runtime 写屏障

当前 MSGC 是 old-only deletion-style barrier：

```text
oldRef = load(field)
oldRef != newRef -> shade/log(oldRef)
store(field, newRef)
```

单引用 `WriteReference()` 最终通过 `EnqueueRefFast()` 立即 mark old ref，并在
winner 的 `TypeInfo::HasRefField()` 为真时追加到当前 `activeLog_`。

数组、struct 和静态 struct 的范围屏障已经使用 `BulkRefMarker`：

- 两个 entry，key 为 `(MSRegionInfo*, wordIdx)`；
- 每个 entry 用一个 64 位 mask 聚合同一 word 的引用；
- `Flush()` 执行一次 `MarkBitmapWord(wordIdx, mask)`；
- 只把 `newBits = mask & ~oldBits` 对应的 winner 追加到 `activeLog_`；
- `BulkRefMarker` 析构时执行 `FlushAll()`，保证屏障返回前没有 pending mark。

因此写屏障并不是完全缺少批处理。Step53 的重点是让它适配条带布局、抽取统一
提交语义，并避免 GC 扫描新 Batcher 与已有 bulk barrier 各自实现一套不一致
的 winner 逻辑。

### 1.3 LLVM inline 写屏障

Runtime wrapper 之外，AArch64 和 X86 MSGC 最终机器码发射也会直接：

1. 通过 `MSRegionInfoMarkBitmapOffset == 32` 读取 `markBitmap`；
2. 计算 `logicalWordIndex = slotIndex >> 6`；
3. 按 `markBitmap + logicalWordIndex * sizeof(uint64_t)` 得到原子 word；
4. 使用目标相关原子指令置位并把 winner 写入 `activeLog_`。

对应文件为：

- `llvm/lib/Target/AArch64/AArch64MSGCEmitter.cpp`
- `llvm/lib/Target/X86/X86MSGCEmitter.cpp`

`MSRegionInfo::markBitmap` 的字段偏移可继续保持 32，不必改变 Runtime/LLVM
结构体 ABI；但指针之后的 word 地址计算必须改为相同的 lane/slot 映射。
AArch64 与 X86 的差异只保留在地址计算和原子机器指令发射，逻辑布局常量必须
共用或由静态断言保证一致。

### 1.4 Bitmap 不是只有 Mark 热路径读取

当前还有多处直接按连续数组访问 `markBitmap[i]`：

- `MSRegionInfo::SweepRegion()`；
- `CentralRegionPool::ComputeMarkedStats()`；
- finalizer/marked-object 枚举；
- `ClearMarkBitmap()` 与 Region 初始化/重置；
- `GetMarkBit()`、`TestAndSetMarkBit()`；
- MarkSetup 清理和分配时 mark-at-allocation；
- AArch64/X86 inline 屏障。

条带化是存储格式变化。任何一处仍使用旧的连续下标都会导致漏标、误回收、
错误 liveBytes 或错误 finalizer 判定，因此必须把布局迁移视为一次全生命周期
协议变更，而不是只改 `MarkBitmapWordResolved()` 的局部优化。

## 2. 设计目标与非目标

### 2.1 目标

- 降低普通对象扫描的 bitmap 原子 RMW/visited-edge 比例。
- 降低多个 Mark 线程修改相邻逻辑 word 时的 cache line false sharing。
- 保持“全局 winner 才入灰队列”的唯一性。
- 保持 write barrier 在返回前完成其要求的 old-edge shade/log。
- 保持 `MarkTermination`、WeakRef、finalizer、Sweep 和 mark-at-allocation 语义。
- 不改变 `MSRegionInfo::markBitmap` 的字段偏移，减少 LLVM ABI 扩散。
- 额外堆内存控制在 5% 以内；当前 `MSBitmapArena` 已按每 Region 固定预留
  1024B sidecar，条带布局复用其中既有 512B mark 预算，不扩大 arena。
- AArch64 macOS/Linux、X86 Linux/Windows 采用一致的逻辑布局。

### 2.2 非目标

- 不修改 CopyGC。
- 不把 Large Object 标记改成条带 Bitmap。
- 不在本步骤改变 WorkBuf 容量、Full/Partial lane 或 helper 扩容策略。
- 不用 owner-sharded 非原子 Bitmap 替代现有全局 winner 协议。
- 不让 helper 直接窃取 mutator TLS 私有数据。
- 不让单引用 barrier 在 TLS 中长期缓存 pending bit。
- 不在没有统计证据时扩大每线程 Batcher 到动态容器。

### 2.3 必须保持的语义

对一个候选 mask，提交操作仍定义为：

```cpp
oldBits = AtomicFetchOr(physicalWord, mask);
newBits = mask & ~oldBits;
```

只有 `newBits` 中的对象是本次提交的 winner。其他线程可能同时缓存同一对象，
但最终只允许原子获胜者把该对象发布为灰对象。

对象在进入 Batcher 前必须完成 Region/slot/TypeInfo 有效性检查。不能为了批量
而先设置 bit、再读取可能已经被 Sweep 清零的 stale slot。

## 3. 总体设计

### 3.1 两层结构

第一层是物理布局：

```text
logical word
  -> MarkBitmapPhysicalIndex(logicalWord)
  -> striped physical word
```

第二层是访问策略：

```text
多个 validated candidate
  -> MarkWordBatcher 按 (Region, logicalWord) 聚合
  -> CommitMarkWord(mask)
  -> newBits
  -> winner 对象进入本线程灰队列
```

所有 producer 最终都通过相同的逻辑 word 提交语义，但生命周期不同：

| Producer | 是否允许延迟 | flush 边界 |
| --- | --- | --- |
| GC main 扫描 | 允许 | WorkBuf 结束、entry 淘汰、终止判断前 |
| helper 扫描 | 允许 | WorkBuf 结束、退出 in-flight 前 |
| mutator Assist | 允许 | Assist 返回、退出 assistsInFlight 前 |
| Runtime bulk barrier | 仅限本次调用 | wrapper 返回前 |
| inline/Runtime 单引用 barrier | 不跨调用延迟 | 当次 barrier 内立即提交 |
| Large Object | 不进入 Small Batcher | 立即原子设置 large mark flag |

### 3.2 推荐端到端流程

```text
读取引用
  -> heap/Region/slot 校验
  -> relaxed probe 全局 mark bit
  -> white 候选读取并保留 TypeInfo
  -> 加入本线程 (Region, logicalWord) entry
  -> 同一 logicalWord 合并 pendingMask
  -> entry 满/淘汰/作用域结束
  -> logicalWord 映射到 striped physical word
  -> 一次 atomic fetch_or(pendingMask)
  -> newBits = pendingMask & ~oldBits
  -> 仅 newBits 对应且 HasRefField 的对象进入 WorkBuf
```

本地 entry 的 `pendingMask` 可以消除同一线程重复边；跨线程重复候选仍由最终
`fetch_or` 决定唯一 winner。

### 3.3 API 分层

不建议直接把现有 `DiscoverObject()` 改成“延迟后仍返回 bool”。该函数当前的
返回值表示调用点已经赢得全局 mark，多个 root、barrier 和辅助路径依赖这个
同步语义。

推荐拆成三层：

1. `Resolve/ValidateMarkCandidate`：只解析并校验，生成
   `MSRegionInfo + logicalWord + bit + ref + TypeInfo`。
2. `CommitMarkWord`：只负责逻辑到物理映射及原子 `fetch_or`，返回 `newBits`。
3. `MarkWordBatcher`：聚合 candidate，调用 `CommitMarkWord`，并通过传入的
   `GreyWorkSink` 发布 winner。

原有 `DiscoverObject()` 继续使用单 candidate 的同步 `CommitMarkWord`。
第一阶段只把 `TraceObjectRefFieldsToGCWork` 等明确持有本地 sink 的扫描热路径
迁移到 Batcher。完成正确性验证后，再评估 root 枚举是否值得批处理。

## 4. 八路条带化 MarkBitmap

### 4.1 逻辑布局

逻辑 Bitmap 含义保持不变：

```text
logical word 0 -> slot 0..63
logical word 1 -> slot 64..127
...
```

物理上按 `logicalWord & 7` 分到 8 个 lane：

```text
Lane 0：word0,  word8,  word16, ..., word56
Lane 1：word1,  word9,  word17, ..., word57
Lane 2：word2,  word10, word18, ..., word58
Lane 3：word3,  word11, word19, ..., word59
Lane 4：word4,  word12, word20, ..., word60
Lane 5：word5,  word13, word21, ..., word61
Lane 6：word6,  word14, word22, ..., word62
Lane 7：word7,  word15, word23, ..., word63
```

每个 lane 起始地址按 64B 对齐。映射公式为：

```cpp
lane = logicalWordIndex & 7;
slotInLane = logicalWordIndex >> 3;
physicalIndex = lane * laneStrideWords + slotInLane;
```

`laneStrideWords` 必须按 cache line 向上取整，而不能简单等于某个 size class
当前使用的有效 word 数。当前 32KB Region、最小 8B slot 最多产生 4096 个
slot，即 64 个逻辑 word；8 个 lane 各保存最多 8 个 `uint64_t`，恰好各占
一条 64B cache line。

### 4.2 存储和分配

当前 `MSBitmapArena` 已为每个 Region 固定预留：

```text
[alloc area：512B，仍只使用前 logicalWordCount 个 word]
[mark lane0: 64B]
[mark lane1: 64B]
...
[mark lane7: 64B]
```

其中：

```text
kBitmapSlotBytes
  = kMaxBitmapWords * sizeof(uint64_t) * 2
  = 64 * 8 * 2
  = 1024B
```

当前 `InitForSizeClass()` 按实际 `bitmapBytes` 设置
`markBitmap = allocBitmap + bitmapBytes`。新布局应固定为：

```cpp
allocBitmap = slotBase;
markBitmap  = slotBase + MSBitmapArena::kMaxBitmapWords;
```

每个 slot 起点相对 arena base 是 1024B 的倍数；同时应对 arena base 增加
64B 对齐断言。这样 `markBitmap` 和每个 lane 都天然 64B 对齐。

```text
现有 sidecar reservation = 1024B / Region
新 sidecar reservation   = 1024B / Region
新增 reserve/commit      = 0
mark 区                  = 512B / 32KB ≈ 1.56%
整个 sidecar             = 1024B / 32KB ≈ 3.125%
```

新布局会让较大 size class 的初始化/清零访问更分散，但不会扩大 BitmapArena
的虚拟地址或已 commit chunk 大小。Release 只需清理各 lane 实际存在的逻辑
word，padding 可只在初始化或 Debug 验证中清零。

若未来 `kMaxBitmapWords`、Region 大小或 cache line 假设变化，应由编译期公式
重新计算 lane stride，并增加 Runtime/LLVM 一致性检查。

### 4.3 统一访问接口

Runtime 中禁止新的调用点直接使用 `markBitmap[logicalWord]`。至少提供：

```cpp
uint64_t* MarkWordAddressResolved(size_t logicalWord);
const uint64_t* MarkWordAddressResolved(size_t logicalWord) const;
uint64_t LoadMarkWordResolved(size_t logicalWord) const;
uint64_t CommitMarkWordResolved(size_t logicalWord, uint64_t mask);
void Store/ClearMarkWordResolved(size_t logicalWord, uint64_t value);
```

Sweep、统计和枚举仍按逻辑 word 递增，只在取 word 时映射到物理地址。最后一个
逻辑 word 的 valid-mask 规则保持不变，padding word 永远不表示对象。

### 4.4 LLVM 地址计算

保留：

```text
MSRegionInfoMarkBitmapOffset = 32
```

修改 AArch64/X86 emitter 中取到 `markBitmap` 后的地址计算：

```text
logicalWord = slotIndex >> 6
lane        = logicalWord & 7
slotInLane  = logicalWord >> 3
byteOffset  = lane * 64 + slotInLane * 8
wordAddr    = markBitmap + byteOffset
```

目标后端可使用各自最合适的 shift/add/LEA 指令，但 lane 数、lane stride 和
逻辑公式不能各自定义成不同值。建议把常量放入 MSGC 公共 layout 头，并保留
Runtime static assert 或生成期检查。

## 5. GC 扫描侧 MarkWordBatcher

### 5.1 推荐结构

每个活跃扫描 owner 持有固定 4 个 entry；每个 entry 最多暂存 8～16 个唯一
candidate：

```cpp
struct Candidate {
    BaseObject* ref;
    TypeInfo* typeInfo;
    uint64_t bitMask;
};

struct Entry {
    MSRegionInfo* info;
    size_t logicalWord;
    uint64_t pendingMask;
    Candidate candidates[kCandidateCapacity];
    uint8_t candidateCount;
};
```

候选数量达到上限时立即 flush，同一 word 后续候选可进入清空后的 entry。
固定容量避免 Mark 热路径动态分配、hash 和锁。4 个 entry 可以覆盖小范围图
遍历常见的多个 Region/word，同时控制每 worker 的栈或成员空间。

### 5.2 Add 规则

`Add()` 的顺序必须是：

1. 解析和校验对象。
2. relaxed probe 发现全局已标记则返回。
3. 读取非空 `TypeInfo`。
4. 找到或分配 `(Region, logicalWord)` entry。
5. 如果 `pendingMask` 已含该 bit，则判定为本线程本地 duplicate，不重复保存。
6. 保存 `ref/typeInfo/bitMask` 并更新 `pendingMask`。

跨线程可能都在 relaxed probe 时看到 white 并缓存相同 bit，这是允许的；
flush 的原子 winner 负责最终去重。

### 5.3 Flush 规则

```cpp
newBits = info->CommitMarkWordResolved(logicalWord, pendingMask);
for candidate in entry:
    if ((newBits & candidate.bitMask) != 0 &&
        candidate.typeInfo->HasRefField()) {
        sink.PushGrey(candidate.ref);
    }
clear(entry);
```

由于 candidate 在置位前已经保留有效 `TypeInfo`，winner 不需要重新读取
stateWord。loser 不得解引用仅为已标记 probe 而留下的空 TypeInfo。

### 5.4 Owner 和 flush 点

- GC main/helper：Batcher 至少覆盖当前 WorkBuf 的字段扫描；WorkBuf 扫完后
  flush，再决定消费本地 `putCur_`、获取 Global 或退出。
- helper：所有 entry 清空前不能退出 `workersInFlight`。
- Assist：flush 后再发布 partial builder，最后退出 `assistsInFlight` 并返回
  mutator。
- Batcher entry 淘汰、candidate 满或显式析构时也必须 flush。
- Mark quiescence 检查只能发生在 GC main 自己的 Batcher 已清空之后。

第一版建议在每个 WorkBuf 边界 flush，正确性和观测最简单。只有数据显示
`bitsPerFlush` 仍过低时，才考虑跨多个 WorkBuf 保留 entry；跨 buffer 保留会
扩大终止协议和异常退出清理范围，不应作为第一版默认。

### 5.5 预期收益

若平均每次提交合并 4 个 bit：

```text
原来：4 次单 bit fetch_or
现在：1 次 mask fetch_or
理论 RMW 数下降约 75%
```

实际收益取决于对象图局部性。即便某个 entry 最终只有一个 bit，行为也退化为
当前一次原子操作，额外成本主要是小型 entry 查找和 flush 分支。因此必须用
统计确认 4-entry 线性查找没有抵消批量收益。

## 6. 写屏障适配

### 6.1 屏障语义边界

MSGC 写屏障保存被覆盖的 old edge。条带化和批处理只能改变“怎样提交 mark
bit”，不能改变：

- old ref 的读取时点；
- shade/log(old) 与字段 store/范围 copy 的既有顺序；
- mark winner 才进入 `activeLog_`；
- wrapper 返回前，本次 bulk barrier 的 pending mark 必须全部提交；
- 非 Mark phase 不应额外记录。

### 6.2 单引用 Runtime barrier

`WriteRefField()` 一次只有一个 old ref，跨 barrier 调用缓存收益不确定，却会
引入 mutator TLS pending work 和 Mark termination 协议。因此第一版保持：

```text
load old
  -> validate
  -> logical-to-striped address
  -> immediate atomic fetch_or(singleBit)
  -> winner append activeLog
  -> store new
```

这里能获得条带化的 false-sharing 收益，但不使用 GC 扫描侧
`MarkWordBatcher`。将来只有在建立“每个 mutator barrier cache 都纳入
quiescence、phase 切换和线程退出 flush”的完整协议后，才允许评估跨调用缓存。

### 6.3 Runtime bulk barrier

现有 `BulkRefMarker` 的作用域模型是正确基础，应保留：

```text
扫描本次覆盖范围中的 old refs
  -> 按 (Region, logicalWord) 聚合
  -> entry 淘汰或作用域结束时 flush
  -> winner 追加到 activeLog
  -> wrapper 才能结束本次 barrier/copy
```

建议适配点：

1. `wordIdx` 明确重命名/注释为 `logicalWordIdx`。
2. `Flush()` 统一调用 `CommitMarkWordResolved()`，由 RegionInfo 内部执行
   striped address mapping。
3. winner 规则继续使用 `newBits`，不能先逐 candidate 调 `DiscoverObject()`。
4. `BulkRefMarker` 析构 flush 作为兜底，但正常控制流也可显式 `FlushAll()`，
   让时序在代码中可见。
5. `AppendMarkedRefInto()` 必须继续处理 activeLog 满时发布 WorkBuf 的逻辑。

现有 bulk entry 使用 `refs[64] + typeInfos[64]`，优点是任何 64 位 mask 都可
一次提交。GC 扫描 Batcher 不必机械复用这个较大的栈结构；两者共享
candidate/commit 语义即可。若要把 bulk entry 改成紧凑 16-candidate 结构，
必须用 `bitsPerFlush` 证明减少清零成本大于增加 flush 次数的代价。

### 6.4 已标记候选的 TypeInfo 规则

`ResolveManagedSmallRefForBarrier()` 当前允许已标记 bit 以 `typeInfo == nullptr`
进入 bulk entry，因为其提交结果不会包含该 bit。该不变量必须保留并写清楚：

```text
probe 已 marked
  -> 可以不读 TypeInfo
  -> flush 后该 bit 必须不在 newBits
  -> 不得解引用 nullptr TypeInfo

probe 为 white
  -> 必须先读取并校验 TypeInfo
  -> 才能加入 pending mask
```

GC 扫描 Batcher 推荐直接跳过 probe 已 marked 的对象，不需要保存这类空
TypeInfo candidate。

### 6.5 LLVM inline barrier

AArch64/X86 emitter 的第一阶段只做正确的 striped word 地址计算，仍保持单
引用即时原子更新。必须覆盖两类发射路径：

- 普通 inline old-ref write barrier；
- struct/array bulk inline loop 中用于 mark descriptor 的地址计算。

当前 AArch64 bulk loop 已尝试成对处理相邻引用，X86 也有独立 inline helper
路径。不能只修普通 scalar emitter，遗漏 bulk/pair helper。

后续可选优化是：当 inline pair 的两个 old ref 已经完成无副作用校验，并且
解析到同一 `MSRegionInfo + logicalWord` 时，把两个 bit OR 后做一次原子
提交；不同 Region/word 继续两次提交。该优化会增加寄存器压力和 fallback
分支，不作为条带布局正确性落地的前置条件。

### 6.6 屏障 flush 与 Mark 终止

Runtime bulk marker 的 pending 状态只存在于 C++ wrapper 栈帧中，wrapper
返回前已清空；inline barrier 第一版没有跨调用 pending。因此它们不需要新增
全局 `barriersInFlight` 计数。

如果未来允许 mutator 持久化 barrier Batcher，则必须同时增加：

- phase 切换 flush；
- safepoint/线程退出 flush；
- Mark quiescence 对 pending barrier work 的可见性；
- `activeLog_` 发布完成后的 release/acquire 协议。

在这些条件完成前，禁止把 GC worker 的延迟 Batcher 直接放进 mutator
`AllocBufferMS` 并跨 barrier 调用复用。

## 7. 正确性与终止判断不变量

### 7.1 mark winner 唯一性

无论来源是 GC main、helper、Assist、Runtime barrier 还是 inline barrier，
同一个物理 word 的唯一全局仲裁点仍是原子 `fetch_or`：

```text
newBits = submittedMask & ~oldBits
```

只有 `newBits` 对应对象可以进入 WorkBuf/activeLog。本地 Batcher 去重只是性能
优化，不能替代全局原子仲裁。

### 7.2 pending candidate 的有效性

- candidate 必须在缓存前完成 slot 和 TypeInfo 校验。
- 已进入本地 Batcher 的对象可能被其他线程先标记，但 Sweep 不能在当前 Mark
  完成前回收它，因此 candidate 引用在 flush 前仍受当前 GC 周期保护。
- relaxed probe 看不到另一个线程尚未 flush 的 bit 只会产生重复 candidate，
  不会产生漏标；最终原子提交会淘汰 loser。
- Large Object 不缓存到 Small Batcher，避免 base-entry 生命周期和
  `isMarked` flag 混入 bitmap entry。

### 7.3 GC worker/Assist 生命周期

以下条件必须成立：

```text
worker/assist 本地仍有 pending candidate
  => 对应 workersInFlight/assistsInFlight 仍然有效
  => 不允许参与“全局无工作”结论
```

helper 的顺序应为：

```text
Drain WorkBuf
  -> Flush MarkWordBatcher
  -> 处理/发布由 winner 产生的 putCur
  -> 确认本地无 pending
  -> 退出 workersInFlight
```

Assist 的顺序应为：

```text
Drain full input
  -> Flush MarkWordBatcher
  -> 发布 non-empty partial builder
  -> 确认本地无 pending
  -> 退出 assistsInFlight
  -> 返回 mutator
```

GC main 在调用 quiesce candidate 检查前也必须 flush 自己的 Batcher。

### 7.4 MarkTermination 与 WeakRef

Batcher 只延迟“赢得 mark 和发布 grey”的动作，不改变对象消费语义。WeakRef
对象成为 winner 后仍像当前实现一样进入 WorkBuf，随后由
`DrainCurrentBuf()` 延迟到 `deferredMainThreadQueue_`，最终 referent 的
保留/清空仍在 MarkTermination fixed point 后决定。

进入 one-way quiesce 后不允许再产生新的 worker-local pending entry。
所有 worker/assist Batcher 已 flush，late Global 和 private partial WorkBuf
仍由既有 MarkTermination 路径收口。

### 7.5 Mark-at-allocation 和 Sweep

Mark phase 新分配对象的 mark-at-allocation 必须写 striped word。分配器
`allocBitmap` 仍为紧凑逻辑数组，不能误套 mark lane 映射。

Sweep 仍按逻辑 word 生成：

```text
freeBits = ~LoadMarkLogicalWord(i) & validMask
allocBitmap[i] = freeBits
```

只有 Mark 完成且对应 Sweep ownership 成立后才能读取稳定 mark bitmap。下一轮
MarkSetup 继续在等待并关闭旧 Sweep 后清零全部有效逻辑 mark word。

### 7.6 内存序

当前 bitmap probe 和 `fetch_or` 使用 relaxed。条带化和本地 mask 合并本身不
要求提高内存序，第一版保持既有内存序，避免引入额外 fence。对象初始化发布、
GC phase gate 和 WorkBuf 发布仍由原有协议承担。

Debug 构建应增加：

- logical/physical index 范围断言；
- lane 起点 64B 对齐断言；
- padding bit 不得被解释成对象；
- flush winner 必须持有可用 TypeInfo；
- quiesce 时所有 GC Batcher 都为空。

## 8. Bitmap 全生命周期迁移清单

### 8.1 Runtime

| 文件/组件 | 必须适配的内容 |
| --- | --- |
| `MSBitmapArena.h/.cpp` | 固定 alloc/mark 半区常量、64B 对齐断言、slot 大小静态断言 |
| `MSRegionInfo::InitForSizeClass` | `markBitmap` 固定指向 slot 后 512B；按逻辑 word 初始化各 lane |
| `MSRegionInfo::GetMarkBit` | 逻辑 word 映射后 relaxed load |
| `IsMarkBitSetResolved` | 使用统一 physical address helper |
| `TestAndSetMarkBit` | 使用统一 commit helper |
| `MarkBitmapWordResolved` | 成为逻辑 word 到 striped word 的唯一原子提交入口 |
| `ClearMarkBitmap` | 遍历有效逻辑 word 清零，不能连续 `memset(bitmapWords*8)` |
| `SweepRegion` | 两轮 `markBitmap[i]` 改为逻辑 word load |
| `ComputeMarkedStats` | 通过逻辑 word accessor popcount |
| `VisitMarkedObjects` | 通过逻辑 word accessor 枚举 bit |
| `MarkObjectInHeapKnown` | 继续经 `TestAndSetMarkBit`，禁止直接寻址 |
| `RegionSpaceMS::IsMarkedObject` | 继续经 `GetMarkBit` |
| Region reset/flush 路径 | 清理有效 striped word，保持 allocBitmap 紧凑布局 |
| `MSMarkBarrier.cpp` | bulk/scalar Runtime barrier 统一调用 commit API |
| `MarkSweepCollector` | 增加 batch-aware candidate/worker 扫描路径 |

必须通过全仓 `rg "markBitmap\\["` 和 `rg "memset\\(markBitmap"` 做收尾审计。
条带化完成后，除封装实现和明确的 Debug 检查外，不应再有按逻辑下标直接访问
物理数组的调用点。

### 8.2 LLVM

| 文件 | 必须适配的内容 |
| --- | --- |
| `AArch64MSGCEmitter.cpp` | scalar、bulk/pair helper 的 striped word 地址计算 |
| `X86MSGCEmitter.cpp` | scalar、bulk/helper 的 striped word 地址计算 |
| MSGC 公共 layout 定义 | lane count、stride、markBitmap offset 的一致常量 |

需要检查所有读取 `MSRegionInfoMarkBitmapOffset` 后执行的 `wordIndex * 8`
寻址，而不能只按函数名搜索 “barrier”。AArch64 macOS/Linux 共用同一逻辑
发射，X86 Linux/Windows 同理；ObjectFormat 不应影响 Bitmap 布局。

### 8.3 不需要改变的部分

- `allocBitmap` 的 logical word 连续布局；
- Large Region 的 marked flag；
- `MSRegionInfo` 64B compiler-visible 字段偏移；
- WorkBuf 的 compiler/runtime ABI；
- CopyGC 的 bitmap/tagged-pointer 路径。

## 9. 统计与性能验证

### 9.1 线程本地统计

统计必须保存在 GC main/helper/Assist 的 local stats 中，退出或周期结束时再
聚合，不能在每条引用上增加新的全局原子计数。

建议增加：

| 指标 | 含义 |
| --- | --- |
| `markCandidates` | 通过基本解析、准备尝试标记的候选数 |
| `probeAlreadyMarked` | relaxed probe 已命中全局 marked |
| `batchCandidates` | 实际加入 Batcher 的候选数 |
| `batchLocalDuplicates` | 同一线程 pendingMask 已包含的重复 bit |
| `batchFlushes` | Batcher 提交次数，即扫描侧实际 RMW 数 |
| `batchSubmittedBits` | 所有 flush mask 的 popcount 总和 |
| `batchWinningBits` | 所有 `newBits` 的 popcount 总和 |
| `batchLostBits` | flush 时已被其他线程抢先标记的 bit |
| `batchEvictions` | 4 个 entry 均占用时发生的淘汰 |
| `bitsPerFlush[1..16]` | 每次提交 bit 数直方图 |

写屏障单独记录：

| 指标 | 含义 |
| --- | --- |
| `barrierScalarRmw` | scalar Runtime/可观测 wrapper 原子提交数 |
| `barrierBulkCandidates` | bulk old refs 候选数 |
| `barrierBulkFlushes` | bulk mask 提交次数 |
| `barrierBulkWinningBits` | bulk winner 数 |

LLVM inline barrier 的逐次统计会污染热路径，第一版不要求在线统计；使用 emitter
汇编检查、Runtime fallback 对照和整体硬件采样验证。

### 9.2 核心比率

```text
扫描侧 RMW/候选       = batchFlushes / batchCandidates
平均提交 bit 数       = batchSubmittedBits / batchFlushes
跨线程 loser 比例     = batchLostBits / batchSubmittedBits
winner 发布比例       = batchWinningBits / batchSubmittedBits
4 线程并行效率        = speedup(4 threads) / 4
```

Step52 已测得 4 后台 Mark 线程相对单线程约 1.779x、并行效率约 44.47%。
实施后的主要目标是降低 RMW/候选、提高平均 bits/flush，并在相同 case/堆/P/SDK
组合下提高 4 线程 Mark objects/s 和并行效率。

### 9.3 正确性验证

至少覆盖：

1. 映射单测：逻辑 word 0..63 映射唯一，`word0..7` 位于不同 cache line，
   `word0/8/.../56` 位于 lane0。
2. 所有 36 个 size class：首 slot、末 slot、trailing bits、空 Region、满
   Region 的 mark/sweep。
3. 同一 word 多线程重复标记：只能有一个全局 winner。
4. 相邻 word 多线程标记：bit 不串位，统计和 Sweep 结果一致。
5. mark-at-allocation：Mark phase 新对象不会被本轮 Sweep 回收。
6. scalar write barrier：覆盖 old edge 后对象仍能被正确追踪。
7. ref array、struct、struct array、static struct bulk barrier：跨 Region、
   同 word、不同 word 和 entry 淘汰均正确。
8. WeakRef/finalizer/resurrection：与当前 MarkTermination 结果一致。
9. mark completeness verifier：遍历 striped bitmap 不漏对象。
10. Debug quiesce：所有 worker/assist Batcher 为空。

LLVM 需要补 AArch64/X86 指令或 MIR/FileCheck 测试，确认最终 word address 使用
lane/slot 公式，而不是旧的连续 `logicalWord * 8`。

### 9.4 性能验证

使用 case27 副本执行：

- 1、2、4 个后台 Mark 线程；
- Assist 关闭与开启两组；
- 相同 P、payload、concurrency、duration 和 heap；
- baseline、仅条带化、仅 Batcher、组合方案四组。

同时记录：

- Mark duration、main/helper/assist processed 和 objects/s；
- `bitsPerFlush`、RMW/候选和 loser 比例；
- 吞吐、失败请求、p95/p99；
- GC CPU、进程 CPU、RSS；
- BitmapArena reserve/commit；
- macOS sample 或 Linux perf 中的 bitmap atomic/cache contention。

替换 Runtime 或改变 LLVM emitter 后必须用对应 MSGC SDK 重新编译 stdlib、stdx
和 case27 `.cj`，不能复用旧 binary。compiler、runtime、stdlib 三件套来源必须
在结果中单独记录。

## 10. 分阶段实施计划

### 阶段 A：统计和连续布局抽象

1. 增加逻辑 word accessor/commit API，但仍映射到连续布局。
2. 把 Runtime 所有直接读取迁移到 accessor。
3. 增加映射、Sweep、stats 和 marked-object 枚举测试。
4. 保持 LLVM emitter 不变，证明纯重构没有行为和性能回退。

这一阶段先建立“逻辑 Bitmap”和“物理布局”的边界，是后续安全切换的前提。

#### 阶段 A 实施状态（2026-07-25）

阶段 A 已在 Runtime 源码中完成，并已继续进入阶段 B～D：

- `MSRegionInfo` 新增 `MarkWordAddressResolved()`、`LoadMarkWordResolved()`、
  `StoreMarkWordResolved()` 和 `CommitMarkWordResolved()`，统一承载逻辑 word
  的地址解析、读、写和原子提交；
- Region 初始化/重置/清零、Sweep 两轮读取、marked stats、marked-object
  枚举、Collector winner 提交和 Runtime bulk barrier 提交均已迁移到逻辑
  accessor；
- 生产代码中已消除 `markBitmap[logicalWord]`、直接
  `memset(markBitmap, ...)` 和旧物理 word 地址 helper；
- accessor/commit 单测、Sweep/source contract 与现有 MSGC 语义契约已经覆盖
  该迁移。

### 阶段 B：条带布局和编译器同步切换

1. 固定 `markBitmap` 到 BitmapArena slot 后 512B。
2. 启用 8 lane、64B stride 映射。
3. 修改 Runtime 初始化、清零、Sweep、统计和 mark-at-allocation。
4. 同步修改 AArch64/X86 MSGC emitter 的 scalar 与 bulk/helper 地址计算。
5. 增加 layout 常量一致性检查。
6. 完整重建 MSGC compiler/runtime/stdlib/stdx 和用例。

条带布局属于 compiler/runtime 隐藏 ABI 变化。旧 cjc 生成的连续 Bitmap
寻址代码不能与新 Runtime 混用；仅替换 dylib 后运行旧 binary 会写错 mark
word。阶段 B 必须整体构建和重新编译所有 `.cj`。

### 阶段 C：GC 扫描 Batcher

1. 增加 `MarkCandidate` 和固定容量 `MarkWordBatcher`。
2. 先接入 helper/GC main 的 `TraceObjectRefFieldsToGCWork`。
3. 接入 mutator Assist，补齐返回前 flush。
4. 增加 local stats、flush 直方图和 Debug quiesce 检查。
5. 先按每 WorkBuf flush，不跨 WorkBuf 保存。

建议保留编译期开关或 MSGC 诊断开关，使 Batcher 可独立关闭；关闭时仍使用
条带布局的单 bit immediate commit。

### 阶段 D：Runtime bulk barrier 统一

1. `BulkRefMarker` 改用统一 logical-word commit API。
2. 核对所有 bulk wrapper 的 flush 与 copy/store 顺序。
3. 增加同 word、多 word、跨 Region、entry 淘汰测试。
4. 比较保留 64-candidate sparse entry 与紧凑 entry 的实际成本。

实际上阶段 B 已要求 bulk barrier 能正确访问条带布局；阶段 D 是进一步消除
重复实现和评估 bulk batching 结构，不应阻塞布局正确性修复。

#### 阶段 B～D 实施状态（2026-07-25）

源码实施已经完成：

1. Runtime 增加 `MSMarkBitmapLayout.h`，采用 64 个逻辑 word、8 lane、
   每 lane 8 个 word/64B 的物理布局：

   ```text
   physical = (logical & 7) * 8 + (logical >> 3)
   ```

2. `MSBitmapArena` 每个 Region 仍保留 1024B sidecar，但固定拆成：

   ```text
   allocBitmap: sidecar + 0B
   markBitmap : sidecar + 512B
   ```

   因而 allocBitmap 继续连续，markBitmap 获得完整 512B 条带空间；arena 基址和
   slot stride 均校验为 64B 对齐。
3. `MSMarkWordBatcher` 已接入 GC main、helper 和 mutator assist 共用的
   `DrainCurrentBuf()`：
   - 每个 WorkBuf 一个 batcher；
   - 4 个 word entry；
   - 每 entry 最多保留 16 个候选；
   - 淘汰、满 entry、WorkBuf 结束和析构均会 flush；
   - 只对 `CommitMarkWordResolved()` 返回的 winning bits 发布 grey 对象。
4. `CJ_MS_MARK_BATCH` 默认开启，可在保持 striped layout 的情况下关闭 batching，
   回退为每候选一次 immediate commit；GC 日志增加候选、probe hit、flush、
   submitted/winning/lost bits、eviction 和 bits-per-flush 直方图。
5. Runtime `BulkRefMarker` 已统一使用 logical-word commit，四个 bulk wrapper
   均在覆盖目标内存或函数返回前显式 `FlushAll()`，析构 flush 只作为异常路径
   和未来维护的安全兜底。
6. LLVM `CJMSGCABI.h` 镜像相同的 lane 常量和映射；AArch64、X86 emitter 的
   scalar 与 helper/bulk 地址计算均改为 lane/slot 物理地址。对应
   AArch64、X86 的汇编、无 inline、Mach-O/ELF/COFF 和 object relocation
   FileCheck 已通过。

本轮 Runtime 验证包括：

- MarkSweep Runtime 在 macOS aarch64 完成源码编译和链接；
- 10 个 MSGC focused contract 全部通过；
- `MSRegionInfoBatchMarkTest.cpp`、`MSMarkWordBatcherTest.cpp` 独立编译运行通过；
- Runtime/LLVM layout ABI contract 通过；
- 直接访问审计未发现生产代码绕过 logical accessor。

当前边界：

- `build.py build --gc-mode marksweep` 证明源码可构建，但该命令的 preinstall
  流程没有刷新 `output/msgc` SDK；
- striped layout 是 compiler/runtime 隐藏 ABI 变化，尚未执行完整 MSGC
  compiler + runtime + stdlib + stdx 重建，也尚未用新 SDK 重新编译 case27；
- 因此本阶段只宣称源码实现和聚焦验证完成，**不宣称性能收益已经验证**。

### 阶段 E：可选 inline pair 合并

在 AArch64/X86 上分别评估：

```text
两个引用解析到同一 Region + logicalWord
  -> combinedMask
  -> 一次原子提交
否则
  -> 保持两次即时提交
```

只有在寄存器压力、代码尺寸、slow fallback 和 benchmark 全部可接受时启用。
最终架构差异仍只存在于机器码发射，逻辑 winner 和布局不分平台。

## 11. 风险、降级与最终建议

### 11.1 主要风险

1. **Runtime/LLVM 布局不一致**：风险最高，会直接产生漏标或误标。必须整体
   重建 SDK 和用例。
2. **遗漏直接下标访问**：Sweep/统计/finalizer 任一遗漏都会造成非立即显现的
   正确性问题。
3. **Batcher 延迟并行工作暴露**：pending winner 尚未进入 WorkBuf，可能短暂
   降低 Global backlog。4 entry × 16 candidate 把最大延迟限制在 64 个对象，
   且 entry 满/淘汰会提前 flush。
4. **低局部性下 entry 查找抵消收益**：如果平均 bits/flush 接近 1，应缩小
   Batcher、调整 hash/替换策略或关闭 Batcher，只保留条带布局。
5. **barrier flush 时序错误**：bulk old refs 不能跨 wrapper 返回继续 pending。
6. **误把 allocBitmap 条带化**：分配 fast path 依赖连续 allocBitmap，本设计
   只改变 markBitmap。
7. **padding 被当成对象**：Sweep、popcount、枚举只能遍历逻辑 word 和有效 bit。

### 11.2 降级方式

- Batcher 可独立关闭，退化为 striped bitmap 的单 bit immediate atomic。
- inline pair 合并可独立关闭，不影响 striped 地址正确性。
- 若条带化收益不足，必须由 compiler/runtime 同步退回连续布局，不能只回退
  一侧。
- Large Object 始终保留当前 marked flag 路径。

### 11.3 最终建议

推荐按以下顺序执行：

```text
先建立连续布局 accessor 和统计
  -> 全仓消除直接 markBitmap[logicalWord]
  -> Runtime + AArch64/X86 同步切换 8-lane 布局
  -> 验证全部 size class、Sweep、barrier 和 mark-at-allocation
  -> 接入 GC main/helper/Assist 的小型 Batcher
  -> 最后评估 inline pair 合并
```

不要先单独修改 `MarkBitmapWordResolved()`，也不要先让 mutator 持有跨调用
Batcher。前者会与直接访问方产生两种布局，后者会把一次局部性能优化扩大为
新的 Mark 终止协议。
