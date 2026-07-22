# MSGC 优化实现与效果详解

> 本文基于当前 runtime 源码、仓颉 LLVM 分支和 `MSGC_RESULT/Implementation` 实验记录，梳理已经落地的优化、实测效果、实现方式及后续优化方向。

## 1. 阅读范围与结论口径

本文的源码基线是当前工作区的 Mark-Sweep runtime，以及同一仓颉工程下的
`cangjie_compiler/third_party/llvm-project`。已有设计稿、补丁和 benchmark 记录只作为
线索，最终以当前源码、测试和可复现实验为准。尤其要注意：`MSGC_RESULT/Implementation`
中有些文件记录的是方案设计或已经回退的实验，不能仅凭文件名判断已经进入发布路径。

状态标记采用以下约定：

| 标记 | 含义 |
| --- | --- |
| **已实现** | 当前源码存在对应机制，且有源码/测试证据。 |
| **已验证** | 除源码外还有明确的构建、压力或 A/B 数据支持。 |
| **已实现但收益有限** | 功能已经落地，实测主要是中性、噪声范围或只改善特定 workload。 |
| **已回退** | 做过实验但当前代码不再保留，效果不能作为当前版本结论。 |
| **设计中** | 只有设计稿或局部基础设施，不能写成已生效优化。 |

效果数据优先采用 `MSGC_RESULT/Implementation/MSGC_实现效果.md`、Step6/8/10/40--43
的记录。不同日期、SDK、GC 参数和编译器可能不兼容；文中会注明 workload、运行时长和
对照组，避免把不同实验直接相减。没有可靠 A/B 数据时，只描述机制和预期，不虚构收益。

## 2. 优化总览

当前 MSGC 的优化不是单一 fast path，而是从“编译器生成代码—线程本地分配—并发标记—后台
清扫—物理页归还”逐层减少固定税和同步工作：

| 层次 | 当前优化 | 主要减少的成本 | 当前状态 |
| --- | --- | --- | --- |
| LLVM lowering | MSGC read barrier 消除，ref/struct 直接 load 或 memcpy | helper 调用、phase/tag 检查 | 已实现、已验证 |
| LLVM/AArch64 | 写屏障专用 wrapper 与 ref/struct inline fast path | runtime call、重复 phase/heap 检查 | 已实现；收益依 workload |
| LLVM/AArch64 | 静态 size-class 对象、动态对象、数组分配 fast path | stub、TLS 间接层、slow call | 已实现；静态小对象收益最明确 |
| Runtime allocator | BUMP/bitmap 双路径、TLS AllocBufferMS、本地缓存 | 全局池锁和 per-object 元数据初始化 | 已实现 |
| Region metadata | 空 Region 整块清零、bitmap summary、ResetToBump | 逐 slot 清理和重复初始化 | 已实现、已验证 |
| Refill/reclaim | partial/empty/unswept 分层、hot cache、FreeManager shard | 前台补货、连续页查找和系统调用 | 已实现；部分激进方案已回退 |
| Barrier runtime | old-edge 记录、bulk bitmap-word OR、active log | 重复 mark、逐引用分支和锁竞争 | 已实现 |
| Concurrent mark | Global WorkBuf、动态 helper、one-way quiesce | 单 worker 瓶颈、partial work 延迟 | 已实现、已验证 |
| Assist/pacer | background credit、比例配速、local stats | mark/sweep 债务集中到单个 P | 已实现；仍需调参 |
| 统计 | bitmap 周期统计、本地化 processed/assist 计数 | 每对象共享原子 cache-line bouncing | 已实现、已验证 |

整体取舍是：MSGC 用并发标记、后台 sweep 和原地回收换取低 RSS/短单次停顿，代价是更多
GC 周期、额外后台 CPU，以及分配器在 ready Region 不足时可能把 sweep/refill 成本带回
mutator。优化重点因此从“再加一个复杂 barrier 分支”逐步转向减少 miss、减少共享原子和
缩短并发 mark 尾部。

## 3. LLVM 与 AArch64 后端优化

LLVM 侧的两个关键文件是
[`CJBarrierLowering.cpp`](../../../../cangjie_compiler/third_party/llvm-project/llvm/lib/CodeGen/CJBarrierLowering.cpp)
和
[`AArch64AsmPrinter.cpp`](../../../../cangjie_compiler/third_party/llvm-project/llvm/lib/Target/AArch64/AArch64AsmPrinter.cpp)。
前者在 IR/codegen 期选择 wrapper 和 intrinsic lowering，后者根据函数属性/符号名直接发射
AArch64 汇编。`AsmPrinter.cpp` 的 wrapper dispatch 也决定某类 barrier 是否真的进入 inline
发射；不能只看到一个 wrapper 函数名就认定 fast path 已生效。

MSGC 编译器 ABI 的公共常量集中在 `llvm/include/llvm/CodeGen/CJMSGCABI.h`：六个 barrier wrapper
名称、三个 fast-allocation 属性、36 档 size class、16KB 阈值和 wrapper 分类都只保留一份。
`CJBarrierLowering`、`CJRuntimeLowering`、`AArch64InstrInfo` 与 `AArch64AsmPrinter` 共同使用它，
避免 lowering、展开尺寸估算和最终发射因复制字符串/表格而漂移。

### 3.1 MSGC 读屏障消除

**优化什么**：MSGC 不移动对象，读路径不需要 CopyGC 的 tagged-pointer 修正或转发检查。
在 `EnableMSGCReadBarrierLowering`（当前默认开启）下，`cj_gcread_ref`、weakref 和
static-ref 被改成普通指针 load；struct/static-struct 被改成 `llvm.memcpy`。这把原来的
`intrinsic -> runtime barrier -> helper` 缩为 load/memcpy，并保持对象地址不变的语义。

`cj_gcread_generic` 不能简单按普通 read 消除：它的目标对象若在堆中，runtime 语义会转成
`WriteStruct`，必须先保留被覆盖区域的 old edge。因此当前 lowering 对 generic 保守处理，
当前不再把 fresh 或“仅目标为栈”当作直接 memcpy 的充分条件：两者只能证明目标区域的
状态，不能证明源 payload 中的引用可跳过 GC read/generic 语义。`CJGenericIntrinsicOpt`
仅保留栈到栈、已知无引用 TypeInfo 等与 GC 无关的证明。Step28/29 的统计显示，case28 中 `READGENERIC_STATS`
已经为 0，但 `WriteGeneric` 的纯 memcpy 分支仍达 5.69 亿次，说明下一步热点已从 read
helper 转为 generic write helper 的调用固定税。

**效果**：读屏障消除的收益主要体现在高频 field load、struct copy 和 generic boxing；它不
会减少真正需要 old-edge 保护的堆写。当前没有把“读 helper 归零”误写成整体吞吐同比提升。

### 3.2 写屏障 lowering 与专用 wrapper

`shouldLowerMSGCWriteBarrier()` 在 AArch64 上识别以下 intrinsic：

```text
cj_gcwrite_ref             -> CJ_MCC_WriteRefFieldFast_MSGC
cj_gcwrite_static_ref      -> CJ_MCC_WriteStaticRefFast_MSGC
cj_gcwrite_struct          -> CJ_MCC_WriteStructFieldFast_MSGC
cj_gcwrite_static_struct   -> CJ_MCC_WriteStaticStructFast_MSGC
cj_gcwrite_generic         -> 先按 generic 规则重写为 struct 写
```

wrapper 是 internal/noinline 的占位函数，默认 body 仍调用 runtime slow helper；真正的
优化由 AArch64 后端识别 wrapper 名称后发射。该分层有两个好处：IR 保留 statepoint/调用
约束，后端可以根据目标架构和固定 ABI 生成短路径；不支持的架构或复杂类型仍安全回落到
runtime。

**arraycopy 的边界**：当前 lowering 和 `AsmPrinter` dispatch 已包含
`ArrayCopyRefFast_MSGC`/`ArrayCopyStructFast_MSGC`，但 AArch64 发射器在 idle phase 只发射
plain memmove；mark phase 会回到 runtime barrier，避免旧的“直接追加 raw old-ref、未先赢得
mark-CAS”路径破坏 trusted-grey 不变量。因此它是“idle copy inline + mark slow barrier”，
不能写成全阶段 arraycopy barrier 已完全 inline。

### 3.3 写屏障 inline fast path

当前 AArch64 发射器的 ref/static-ref 路径大致为：

1. 读取导出的 `Heap::inlineFastPathGCPhase`；mark 不活跃时（包括 IDLE 下并发 Sweep）直接 store-only。
2. mark setup/concurrent mark/mark termination 期间读取 old ref。
3. 对 old 为 null、非堆地址或 barrier buffer 不足的情况走 store/slow path。
4. 从 TLS 的 `msAllocBuffer` 取 `activeLog_`（固定偏移 896），在 WorkBuf 尚有空间时写入 old
   edge 并更新 `len`，最后执行 store。

struct/static-struct 路径根据 GCTIB 扫描被覆盖区域的引用；短布局可按 bitmap word 批量
处理，复杂布局回 runtime `WriteStruct`。array copy 和 atomic swap 仍要以当前 wrapper
dispatch 和 runtime 对应实现为准。

**关键语义**：当前 runtime 的 `EnqueueRefInto()` 采用“先 mark winner，再把 winner 且有
引用字段的对象写入 active log”的 trusted-grey 路径；AArch64 发射器为避免 mark winner
后因 log buffer 满而丢失记录，保留了 reserve-log-before-mark 的 slow/fallback 约束。该
顺序是正确性要求，不应为了少一条分支而随意交换。

**效果**：inline 路径把最常见的 old-ref store 从函数调用压缩为 phase/heap/buffer 检查、
一次 bitmap 原子操作和 log 写入；收益取决于 mark 期写比例和 old ref 是否在堆中。Step38
中的 wrapper tail-call、慢路径 dispatch 和 bitmap-word batch mark 仍在当前代码/后端中；
`CJBarrierOpt` 的单引用小 struct 标量化已于 2026-07-20 移除。bitmap-word batch mark 的
独立吞吐收益接近噪声，不能与大幅吞吐提升混写。

### 3.4 新对象 size-class fast path

`doNewFastPath()` 对编译期常量分配大小做 size-class 特化。runtime 与 LLVM 共用 36 个
class，覆盖 8B--16KB；大小向上取整，例如 24B 对象落到 32B class。LLVM lowering 对大于
16KB 的大小使用 dynamic/general wrapper 或 runtime slow path；但 Runtime 顶层
`RegionSpaceMS::Allocate()` 对齐后以 `>=16KB` 进入 large path，因此精确 16KB 是当前两层
实现的边界差异，不能在性能结论中把它当作普通 small 对象。

静态 wrapper 名称带有 `cj-msgc-sizeclass`、`cj-msgc-slotsize` 属性，AArch64 发射器据此
生成：

- 检查 TLS `safepointState`；当前对象分配 emitter 不读取 `encodedGCPhase`，也不在 fastpath 中做 mark-at-alloc；
- 取 TLS `msAllocBuffer` 和 `activeRegions_[class]`；
- BUMP Region 用 `allocPtr/regionEnd` 推进；
- PARTIAL Region 用 `reservedBitmap` 找空闲 word，再在 `allocBitmap` 中清 slot 位；
- 写入 typeinfo，更新 free count，返回对象；任一步骤不满足（包括 safepoint 不安全）则调用
  `CJ_MCC_NewObject`。

这条路径将原来的“编译器 fast wrapper 先 miss，再进入 runtime Allocate”变成真正的
per-class inline 分配。需要强调：MSGC 兼容层仍把历史 `AllocBuffer::tlRegion_` 保持在
offset 0 且指向 NullRegion，以保证未特化的 CopyGC 风格 7 条指令安全 miss；真正有效的
MSGC fast path 读取的是 TLS `msAllocBuffer` 和其 active region 数组。

### 3.5 动态对象与数组分配 fast path

动态对象 wrapper 在运行时查 size-class/slot-size 表，逻辑与静态路径相同，只是 slot size
不是立即数，因此指令更多、命中收益较小；入口检查 safepoint、size 非零且不超过 16KB。
AArch64 当前 MSGC lowering 对精确 16KB 仍可能生成 wrapper，而 Runtime slow path 会按
large 处理；修改阈值时必须同步更新 lowering、AsmPrinter、Runtime 和回归用例。
AArch64 还为动态/具体类型数组生成按元素大小和引用布局分派的 fast path：检查
`safepointState`、TLS `msAllocBuffer`、active region、BUMP/PARTIAL 状态、bitmap/summary
和剩余容量，再回落到 `CJ_MCC_NewArray`。数组的常量总大小也可在 lowering 阶段直接映射
到 size-class wrapper。

该路径的 ABI 风险最高：LLVM 直接使用 `MSRegionInfo`、`ThreadLocalData` 和
`AllocBufferMS` 的硬编码偏移。runtime 在
[`ThreadLocal.cpp`](../../src/Mutator/ThreadLocal.cpp) 里通过 static_assert 固定这些布局，
修改结构体时必须同步更新后端常量，否则会变成静默内存踩踏而不是普通编译错误。

### 3.6 generic/struct/array 热点消解

generic 的优化分两个层次：

1. `CJGenericIntrinsicOpt` 在 IR 级利用栈到栈、无引用 TypeInfo 等 GC 无关证明，
   对满足各自条件的 `cj_assign_generic`/`cj_gcread_generic`/`cj_gcwrite_generic` 改为 memcpy；
   fresh heap 或仅目标为 alloca 的 `gcread_generic` 专项折叠已撤销；
2. `CJBarrierLowering` 对仍需堆写语义的 generic，改写为 struct-write wrapper，继续保留
   old-edge 保护。

当前最明确的后续热点是“runtime `WriteGeneric` 最终只做 memcpy，但 LLVM 没能证明这一点”
的场景。Step29 的深度计数中 `base_write_generic_memcpy=569,089,392`，真正进入
`WriteStruct` 的次数仅约 7.5 万，优先消除 helper call/分支比继续优化 read helper 更有
价值。

### 3.7 编译器—runtime ABI 与 phase 同步

LLVM 生成代码依赖以下稳定布局：

| 数据 | 当前约束 |
| --- | --- |
| `ThreadLocalData::buffer` | offset 0，兼容旧 TLAB wrapper；MSGC 下实际保存可兼容转换的 buffer 指针 |
| `encodedGCPhase` | 当前字段仍保留在历史 ABI 位置，用于 TLS phase 更新/部分代码路径 |
| `msAllocBuffer` | 固定在 TLS 末段，供 MSGC 分配/屏障快路径读取 |
| `AllocBufferMS::activeLog_` | offset 896；WorkBuf `len`/`objs`/capacity 为 8/16/1024 |
| `MSRegionInfo` | allocPtr/regionEnd/bitmap/free count/type/reserved 等偏移由 static_assert 对齐 |

近期实现已把 AArch64 barrier 的主 gate 收敛到导出的
`Heap::inlineFastPathGCPhase`，而不是让每个 callsite 独立查询全局状态；runtime
`InstallBarrier()` 用 release store 发布 phase。TLS 仍保留 `encodedGCPhase`，
`MRT_UpdateThreadLocalDataPhase()` 在 aarch64 上把缓存 phase 字节编码进 TLS 地址高位，
用于 managed entry 等路径的低成本同步。

Step10 的修复解决了 GC 线程代 mutator 切 phase 时只更新 `mutatorPhase`、没有刷新目标 TLS
的窗口；随后又把目标 phase 预发布到 mutator TLS，关闭 global phase 已更新但 LLVM fast
path 仍看到旧 phase 的窗口。这是“性能优化必须服从 phase 一致性”的典型例子：phase gate
错一个周期会影响写屏障的 old-edge 保护。需要区分的是，当前 AArch64 对象/数组分配 emitter
主要检查 `safepointState`，不读取 `encodedGCPhase`，也不在分配 fastpath 中设置 mark bit；
旧记录中“按 mark phase 做 mark-at-alloc”的描述属于历史方案或旧二进制，不能当作当前实现。

Step10 的历史验证中，case1 + seed5 连续 5 次、case2 + seed5 连续 3 次均正常退出，未出现
invalid/panic/OOM；这些结果用于说明当时的 phase/fastpath 修复效果，不等于当前所有分配
emitter 都按 mark phase 分流。当前结论应以 `safepointState` gate、Region/bitmap 命中率和
重新生成的反汇编为准。

## 4. Runtime 分配器优化

分配器的核心目标是把“小对象分配”变成线程本地、无锁、少分支的操作，把 Region 获取、
Sweep、页归还等重工作移出 mutator。实现集中在
[`MSRegionInfo`](../../src/Heap/MAllocator/MSRegionInfo.cpp)、
[`AllocBufferMS`](../../src/Heap/MAllocator/MSAllocBuffer.cpp)、
[`CentralRegionPool`](../../src/Heap/MAllocator/MSCentralRegionPool.cpp) 和
[`MSFreeManager`](../../src/Heap/MAllocator/MSFreeManager.cpp)。

### 4.1 SizeClass、AllocBufferMS 与 BUMP/PARTIAL 双路径

LLVM fastpath 将不超过 16KB 的对象映射到 36 个 size class（8B--16KB），一个 32KB Region
只服务一个 class；Runtime 顶层对齐后的 `>=16KB` 请求则进入 large span。`RegionSpaceMS::Allocate()`
的实际分流为：

```text
allocSize < 16KB  -> class -> AllocBufferMS::Allocate
allocSize >= 16KB -> large-object span

LLVM fast wrapper 的静态/dynamic 上界仍写作 `size <= 16KB`；精确 16KB 命中 wrapper 后若
继续走 Runtime slow path，会按上面的 `>=16KB` large 规则处理。
```

每个 mutator/Processor 持有 `AllocBufferMS`，其中 `activeRegions_[class]` 是各 class 当前
正在使用的 Region；`cachedRegion_[class]` 保存后台或前一次 refill 预备的 Region。这样正常
分配不需要 CentralPool 锁，也不需要每对象更新全局统计。

Region 有两条热路径：

- **BUMP**：新 Region 或完全清空的 Region 用 `allocPtr += slotSize`，只做边界比较、写
  typeinfo 和更新指针；
- **PARTIAL**：部分存活 Region 用 `reservedBitmap` 的 summary 找到非空 bitmap word，再
  在 `allocBitmap` 中清空一个空闲 slot 位，更新 `freeSlotCount`。

`FastAlloc()` 只检查当前 active Region；耗尽或类型不匹配才进入 `SlowAlloc()`。这把绝大
多数分配限制在 TLS + Region 元数据范围内，只有 Region 切换才支付中央池成本。

### 4.2 Region 元数据与 bitmap 优化

[`MSRegionInfo::InitForSizeClass()`](../../src/Heap/MAllocator/MSRegionInfo.cpp) 为每个
Region 建立 alloc bitmap、mark bitmap 和 reserved summary。alloc bitmap 表示 slot 是否
已分配，mark bitmap 表示本轮是否存活，summary 用于快速定位仍有空闲 slot 的 bitmap word。
mark 位使用原子操作，适配 mutator barrier、GC worker 和 assist 的并发标记。

Sweep 时不会把对象逐个交给 runtime 析构；它先拷贝/统计 mark word，按存活位生成 free 位，
再批量清理 dead slot。partial Region 只清掉确实要复用的 slot；全空 Region 走整块路径，
避免对 32KB 区域重复执行细粒度 bitmap/slot 操作。

这里有一个重要边界：Step13 提出的 freelist、slot-level cursor 等方案不是当前默认实现。
slot-level cursor 实验因移位、取模和 mask 的固定税导致碎片场景 Phase2 慢 30%--100%，已
回退；当前仍是 summary + bitmap CTZ/CLZ 的实现。未来若采用 freelist，必须同时改 runtime
结构体偏移和 LLVM AArch64 PARTIAL 发射逻辑，不能只替换 `SweepRegion()`。

### 4.3 空 Region 批量清零与 ResetToBump

[`MSRegionInfo::SweepRegion()`](../../src/Heap/MAllocator/MSRegionInfo.cpp) 有两个关键优化：

1. `liveCount == 0` 时一次 `memset` 清零整个 32KB payload，直接把 alloc bitmap 设为“全空”、
   mark bitmap 置空、summary 归零并切到 SWEPT；
2. [`ResetToBump()`](../../src/Heap/MAllocator/MSRegionInfo.cpp) 复用已经确认全空且已清理的
   bitmap 内存，只重置指针、class 和状态，不重复分配/初始化 sidecar。

这两个优化分别减少了 sweep 的逐 slot 清理和“空 Region 再次作为新 Region”时的元数据初始化。
Step6 的 case1 A/B 数据显示：

| 改动 | 结果 |
| --- | --- |
| 全空 Region bulk clear | 总耗时约 6537ms -> 6314ms（-3.4%），GC 总时间 100.22ms -> 83.60ms（-16.6%），GC P95 10.13ms -> 5.71ms（-43.6%） |
| 在 SlowAlloc 中去掉同步 backup prefetch（保留后台 fill） | 6314ms -> 6247ms（-1.1%），平均 RSS -9.6%，峰值 RSS -14.5%，GC 总时间 -14.1% |

收益说明：bulk clear 对每轮需要处理大量死亡 slot 的 workload 更有效，整机 wall time 的
改善会被其它分配和调度成本稀释；去掉同步 backup prefetch 则同时减少了重复 refill 和
前台 committed/RSS 压力。

### 4.4 补货链、缓存与 lazy sweep 策略

当前 `SlowAlloc()` 的顺序是：归还旧 active Region，尝试本地 cached Region，再从
CentralPool 按以下优先级补货：

```text
partial swept -> local/global empty cache -> unswept（lazy sweep） -> fresh region
```

Region 获取时会携带 class 和 sweep generation；未完成上一轮 sweep 的 Region 不能直接
混入本轮 mark 的可用集合。`CentralRegionPool::TryPublishSweepDone()` 只有在 unswept、
background/lazy/assist/helper 计数都归零后，才发布“本轮 sweep 完成”。这避免了并发
Sweep 尚未结束就清空 mark bitmap 的数据竞争。

Step8 的实验确认了几个当前实现原则：

- 去掉 SlowAlloc 的同步 pre-sweep notify、允许后台 fill 在受控条件下使用 fresh supply，
  case1 wall time 5748ms -> 5655ms，`cached_hit` 2328 -> 44407，`slow_fresh` 103288 ->
  61446；
- hot-class adaptive refill 继续改善 cached hit，hotcache 基线约 5457ms；
- hard pending gate 三跑均值（5435.67ms）优于 soft throttle（5610.33ms）；
- proactive refill、过度 fresh prefill、只扩 top1/top2 class 等方案会提高 RSS 或增大
  `cached_miss/slow_fresh`，没有保留。

这些数据说明补货优化的目标不是把缓存做得越大越好，而是让“最热 class 的下一次切换”
命中 ready/cached Region，同时避免为了备用 Region 把新提交内存提前顶高。当前源码中
`SlowAlloc()` 明确不再同步做 backup prefetch，注释说明这会把一次 miss 变成两次中央补货
并放大 lazy-sweep 成本。

### 4.5 大对象热缓存与释放

超过 16KB 的对象走 `RegionSpaceMS::TryAllocateLargeObjectOnce()`：按 32KB Region 数量
申请连续 span，清零 payload，注册 primary/follower 元数据，并在 mark 期把大对象 primary
直接设为 marked，防止分配时被错误回收。`CentralRegionPool` 维护 largest-first 的大对象
hot cache：回收后如果 span 仍适合复用则保留，过大或压力不足时转给 FreeManager。

该路径的优化点是避免每次大对象分配都重新 mmap/建立 span 元数据；代价是 hot cache 会在
短期保留物理内存，必须和 `cjHeapSize`、scavenge 目标及 workload 的大对象复用率共同调节。
Step6/实现效果中 case5 的大对象混合负载显示，MSGC 虽然 RSS 峰值大幅下降，但 CPU 和 GC
总时间明显上升，说明大对象热缓存只能减少系统分配，不能消除频繁 mark/sweep 的根本成本。

### 4.6 FreeManager 分片、Scavenge 与 segment 回收

[`MSFreeManager`](../../src/Heap/MAllocator/MSFreeManager.cpp) 将物理地址空间按 64MB
segment 管理，每个 segment 有 2048 个 32KB Region 的 alloc/scavenged bitset、64-Region
chunk summary 和 `maxFreeRun`。当前线程优先访问 home shard，找不到连续区间时才借用 donor
shard，减少多 Processor 争用同一棵全局 release tree。

物理回收分两层：

- `ScavengeFreeRegions*()` 逆向扫描并合并相邻 hot-free runs，按保留量和预算调用
  `madvise`/释放页，标记 scavenged bit；
- `ReleaseCompletelyFreeSegment()` 只有整段完全空闲且不在借用/分配状态时才解除 segment
  所有权，避免频繁 grow/shrink 抖动。

这一层的优化主要改善 RSS 和系统调用规模，不直接缩短每次对象分配。它也解释了为什么
MSGC 在 seed3 下 RSS 均值显著低于 CopyGC：空 Region 可以渐进归还，而不是等待整个
from-space 批量释放。

### 4.7 泛型 TypeInfo、TLS 与协程调度的 runtime 固定税

MSGC 主 collector 变快后，runtime 辅助路径会显得更突出。Step7 的 seed5 profile 发现
[`TypeInfoManager::GetOrCreateTypeInfo()`](../../src/TypeInfoManager.cpp)、
`Barrier::WriteGeneric/WriteStruct`（[`Barrier.cpp`](../../src/Heap/Barrier/Barrier.cpp)）和
`memcpy_s` 稳定出现在 HashMap/Array/Iterator 热路径；这类成本不是 mark 算法本身，而是
每次泛型容器操作都重复做 type-argument hash/compare、generic payload copy 和 barrier
路由。当前已经有两类 LLVM 消解方向：

- `CJGenericIntrinsicOpt` 对可证明的 stack-to-stack/non-ref generic copy 改写为 memcpy；
  fresh-destination 或仅 stack-destination 的 `gcread.generic` 专项折叠已撤销，当前也没有
  `gcwrite.generic` 的 fresh-destination 专项折叠；
- `CJBarrierLowering` 对仍需堆写语义的 generic 改写为 struct-write wrapper。

它们尚未覆盖所有 generic write，下一步应以 Step29 的 `base_write_generic_memcpy` 统计
为导向，避免把“runtime 最后走 memcpy”误当成编译期总是安全。

调度方面，Step27 对 `Schdfd/Schdpoll/netpoll`（[`CJThread`](../../src/CJThread)）的结论是：阻塞 socket 会 park 当前协程，
不会直接睡死 OS worker；潜在上限在 ready 批量回流后的单 poller drain、`CJThreadAddBatch`
的 processor wake 爬坡以及 timeout timer 分配。它们不是 MSGC 特有优化，但在 GC 固定税
下降后可能成为 case25/27 服务端吞吐的新瓶颈。建议只在 profile 证明 ready backlog 或
timer 管理占比上升后再改，避免把网络调度变化和 GC A/B 混在同一轮实验。

## 5. MSGC 写屏障与 barrier 优化

MSGC barrier 的核心是并发标记期间保护“即将被覆盖的旧引用”（old edge），而不是像
CopyGC 那样在对象转发期间修正读引用。实现位于
[`MSMarkBarrier.cpp`](../../src/Heap/MBarrier/MSMarkBarrier.cpp)；Idle barrier 只做
普通写入/拷贝，二者由 [`Heap::InstallBarrier()`](../../src/Heap/Heap.cpp) 按 phase 发布。

### 5.1 MSGC 与 CopyGC 的语义边界

CopyGC 有 enum/trace/post-trace/preforward/forward 等多套 barrier，读屏障负责 tagged pointer
和转发协作；MSGC 只需要在 mark setup、concurrent mark、mark termination 期间记录 old edge，
idle、finish、reclaim、sweep 期间可以 store-only。因此不能把 CopyGC 的“old+new 记录”或
tagged-pointer 检查整体移植到 MSGC。

当前 runtime 的 phase 映射是：

```text
IDLE / FINISH / RECLAIM / SWEEP -> msIdleBarrier
MARK_SETUP / CONCURRENT_MARK / MARK_TERMINATION -> msMarkBarrier
```

`InstallBarrier()` 同时 release-store `Heap::inlineFastPathGCPhase`。inline 后端只读取这一
个发布点，减少每个写操作查询完整 GC 状态的成本。未知 phase 走保守 barrier，不允许快路径
假设“当前肯定不在 mark”。

### 5.2 old-edge 记录与 trusted-grey

`MSMarkBarrier::WriteReference()` 先读取 old，再执行 `EnqueueRefInto()`，最后写入 new。
`EnqueueRefInto()` 做以下工作：

1. 判定 old 是否为空且位于 MSGC heap；非堆指针无需进入 GC work；
2. `ShadeRefForBarrier()` 尝试在 mark bitmap 上置位；
3. 只有本次置位成功且对象含引用字段时，才把对象加入当前 `activeLog_`；
4. 已经 marked 的对象不重复入队，避免 write-heavy workload 把同一对象反复推入队列。

当前版本将 log 中的 winner 视为可信灰对象，标记统计不再在每个 winner 上更新共享
`markedObjectCount`。如果 active log 满，runtime 负责发布 WorkBuf/切换 buffer；AArch64 inline
路径也必须遵守同一“不能 mark 成功但丢掉待扫描对象”的顺序约束。

### 5.3 active log、WorkBuf 与 bulk barrier

`AllocBufferMS::activeLog_` 是每个 mutator 的小型 WorkBuf，固定容量为 1024 个对象；满后
发布到 Global/Per-P work 结构，GC main、helper 或 assist 再消费。这样写屏障常态只写 TLS
缓存，不获取中央锁。

struct 和数组写入不能逐字段调用 `WriteReference()`，因此 runtime 使用 `BulkRefMarker`：

- 根据 GCTIB/ref bitmap 找到目标覆盖范围内的引用槽；
- 以 bitmap word 为粒度批量 OR mark bits；
- 只有 mark winner 且含引用字段的对象进入 active log；
- 最后执行 `memcpy_s`/`memmove_s`。

该实现把“扫描字段”和“提交 work”分离，减少每个 slot 的 collector 查找、重复 phase 判断
和 log owner 解析。`ResolveBulkLogOwner()` 在一次 bulk 操作中只解析一次 collector 与
AllocBuffer，属于低风险的固定税优化。

Step38 记录的 wrapper tail-call、slowpath dispatch 和 bitmap-word batch mark 分别减少调用层
和循环层固定成本；单引用小 struct 标量化曾用于减少整体 struct barrier 固定税，
但由于 A/B 收益基本与优化前一致，现已移除。batch mark 的单项吞吐变化在当前数据
中不稳定，主要价值是降低 barrier/mark 的共享原子压力。

### 5.4 heap-range、phase gate 与 inline fast path

AArch64 inline barrier 通过 runtime 发布的 reserved heap start/end 判断 old 指针是否属于
MSGC heap；不在范围内直接走 store-only。相比逐对象查询 Region metadata，这个范围判断只
需两次比较，适合 ref field 热路径。范围边界必须覆盖 large span 和普通 Region，Step37/bugfix
中的错误发布曾导致合法堆引用误判为外部指针，必须在替换 runtime 后用 `DYLD_PRINT_LIBRARIES`
和压力用例确认加载的是同一版本。

主 fast path 的执行逻辑可概括为：

```text
phase idle/sweep?       -> 直接 store/memcpy
old null/non-heap?      -> 直接 store
activeLog 有空间?       -> reserve + mark/log + store
buffer 满或复杂布局?    -> publish/slow helper
```

历史文档中出现过 `gcFastState`/TLS phase 直接作为主 gate 的描述；当前工作区的 AArch64
发射器和 runtime 以 `Heap::inlineFastPathGCPhase` 为主 gate，但 TLS `encodedGCPhase` 仍服务
managed entry 和 phase 刷新。后续阅读旧 benchmark 时必须以当前二进制反汇编和源码为准。

### 5.5 struct/array/atomic/generic 路径

- **ref/static ref**：记录 old，store new；static 版本跳过对象定位但仍做 heap-range/phase
  语义。
- **struct/static struct**：先按 GCTIB/ref bitmap 记录被覆盖区域的 old refs，再 memcpy；
  无引用 struct 可以在 LLVM 侧直接 memcpy。
- **array copy**：runtime `CopyRefArray()` 记录目标旧引用后 memmove；当前 LLVM wrapper
  在 idle phase 可发射普通 memmove，mark phase 仍调用 runtime 完成逐引用 shade/log，
  不能按全阶段 inline 统计。
- **atomic store/CAS/swap**：先读取旧值，完成 CAS/交换后保留 old edge；不能为了减少一次
  load 把原子语义改成普通 store。
- **generic**：`WriteGeneric` 可能是纯 payload memcpy，也可能因 dst 在 heap 上而必须转成
  `WriteStruct`。只有能在 LLVM 证明 dst/source 不涉及堆引用时，才可提前消除 helper。

### 5.6 已验证优化、收益与未采用方案

已经保留的优化包括：

1. read barrier direct load/memcpy；
2. ref/struct barrier wrapper 和部分 AArch64 inline；
3. active log 本地缓存、满块发布和 bulk bitmap-word mark；
4. 删除 MSGC 路径的逐对象 `markedObjectCount` 原子更新；
5. barrier phase gate 与 heap range 的统一发布。

Step36/38 中一些更激进的方案没有作为当前结论：

- 把 `MaybeNotifyMark()` 全部下沉到后台会让 RSS/GC 次数和进度失衡，只保留 reclaim-only
  场景的有限实验结论；
- 先 mark 后 log、扩大 inline 复杂分支或无条件打开 arraycopy inline，可能造成丢 work、
  jump-table range/代码尺寸问题，不能只看指令数；
- 把 CopyGC enum 的 old+new 语义整体迁入 MSGC，会改变当前 mark correctness，不采用。

Barrier 优化的可测收益目前更多体现为降低高并发写入下的 CPU 固定税和 GC worker 负担，
而不是单个 case 的 wall time 必然下降；验证应同时检查吞吐、mark backlog、WorkBuf 溢出、
verifier 和长时无 OOM。

## 6. 标记、并发收敛与 GC 调度优化

主流程在 [`MarkSweepCollector::DoGarbageCollection()`](../../src/Heap/MCollector/MarkSweepCollector.cpp)
中固定为：

```text
PreEnumRoots -> EnumRoots -> ConcurrentMark -> MarkTermination
             -> PostMarkTermination（统计与后台 Sweep/Scavenge 发布）
```

优化围绕两个目标展开：让并发阶段有足够的可消费 work，并在进入 STW termination 前
形成可证明的 fixed point；不能为了缩短日志或减少一次唤醒而跳过 late barrier/work 的闭合。

### 6.1 PreEnumRoots 与 generation 快照

`PreEnumRoots()` 先停止并等待所有正在进行的 sweep，再在 STW 下清理 mark bitmap、把未扫
Region 重新分类、准备下一轮 sweep generation，最后在 mark 前翻转 generation。这样本轮
mark 不会把 PreEnumRoots 之后新分配/重新绑定的 Region 混入旧快照，也不会与 SweepRegion
并发清空同一 mark bitmap。

这项优化不是“减少一条指令”，而是把昂贵的状态转换集中到一个可验证的 STW 边界：

- sweep backlog 的清理职责由前一轮后台/lazy/assist 负责；
- PreEnum 只处理仍未完成的安全尾部；
- generation flip 使并发 mark、MarkNewObject 和 Sweep 使用明确的代际视图。

Step9 的结论是，冻结 sweep 快照比在 mark 中动态判断 Region 是否属于本轮更稳定；代价是
如果后台 sweep 落后，PreEnum 仍可能出现 force-finish 尾部，这也是后续 Sweep helper 的
主要优化目标。

### 6.2 并发 Mark worker 与 Global WorkBuf

早期实现中 GC main/单 helper 和 mutator assist 争用同一批 WorkBuf，Global 中的 partial 还
可能被 `PopFull()` 跳过，导致大量尾工作推迟到 MarkTermination。当前实现做了三层改造：

1. **Global 任意 buffer 消费**：`PerPGCWork::AcquireOneBuffer()` 从 Global 弹出 full 或
   partial，GC main/helper 都可直接扫描；mutator assist 仍只获取 full，保持一次偿债一个
   1024-object buffer 的粒度。
2. **动态 Mark helper**：`cjGCThreads` 表示包括 GC main 在内的总并行度，worker 根据
   Global backlog、增长趋势和 emergency 压力逐步增加；一个 GC 周期内只增不减，避免按
   微秒级低水位反复 park/unpark。
3. **one-way tail drain**：第一次满足 GC main 输入为空、Global 为空、worker/assist
   in-flight 为 0 后，固定 helper 到配置上限、提高 assist ratio，禁止新的并发 claim，
   等待 in-flight 清零后直接进入 MarkTermination；late work 由 STW fixed point 兜底，不
   再恢复并发闭包。

Step40 的原始“global full 统一流转”短测吞吐约 449 req/s，与 468 req/s 基线同量级，说明
只改变 work ownership 不会凭空增加算力。随后加入动态多 worker 后，case27
`c1000/p100K/60s` 的 2026-07-11 验证为 389 req/s、136 个 GC，无 fatal/OOM，helper
累计扫描约 3.57 亿对象，mutator assist 占累计 marked 约 55.75%，明显低于单后台线程
约 80%。这类数据说明 worker 改造确实把扫描工作从 mutator 移出，但吞吐仍受 pacer、
分配和服务端等待影响，不能把 helper 扫描量直接等价为 wall-time 加速。

### 6.3 Assist pacing、credit 与动态并行度

MSGC 不把所有标记工作强行交给 GC 线程，而是在 mutator 分配/写屏障压力达到条件时做
有限 assist。当前 `MSMarkAssist()`：

1. 尝试进入 assist gate，避免 termination/quiesce 期间新增参与者；
2. 先偷取 background scan credit，再至多消费一个 Global WorkBuf；
3. 按 `assistWorkPerByteScaled_` 和 backlog multiplier 计算下一次债务；
4. 退出时减少 in-flight，供并发收敛判断。

Assist ratio 使用上一轮 marked objects/allocated bytes 的 raw base，加上 Global backlog
EMA 和 `1x--4x` 的上限放大。Step43 又增加了周期反馈：根据 main/helper/assist 实际
processed share 调整下一轮 base，当前实现的反馈门槛已从早期 30% 实验收紧为 share>90%、
目标 80%，避免短测噪声把 ratio 压得过低。

Sweep 也采用同一思路：`MSSweepAssist()` 先消费 background/lazy/helper credit，不足时
按每次调用的 region 上限执行 direct assist。`activeAssistSweepCount_`、skip gate 和
StoreLoad fence 保证 PreEnum 在清理 mark bitmap 前不会与正在运行的 mutator sweep 交叉。

### 6.4 MarkTermination fixed-point drain

`MarkTermination()` 在 STW 下切到 mark termination phase，完成以下固定点：

1. 请求所有 mutator flush write-barrier buffer；
2. 收集退出 mutator 的 active log、GC main/helper 的 private partial 和 deferred queue；
3. 反复 drain Global LF stack、deferred main queue 以及新产生的灰对象；
4. 直到没有 late entry、in-flight work 和可继续发现的引用；
5. 统一 dispose/reset per-P work，再允许进入 PostMarkTermination。

该固定点是并发优化的安全底座。Step42 曾尝试在 quiesce 时发布 GC main/helper 的 private
partial 并重新打开并发闭包，短测不报错但长测把 ConcurrentMark 拉到 2.49s 并最终 OOM，
已撤销；当前 private partial 仍交给 MarkTermination STW 处理。任何未来减少 STW 的方案都
必须先证明不会让 partial work、activeLog 生命周期或 mutator 退出产生重复发布。

### 6.5 bitmap 统计替代逐对象原子计数

MSGC 的存活真相是 Region mark bitmap，而不是统计字段。旧实现每次 mark winner 都更新
共享 `markedObjectCount`，多 worker/assist 会造成 cache-line bouncing；Step41 改为在
`PostMarkTermination()` 的 bitmap 稳定窗口做一次 `ComputeMarkedStats()`：

- small/medium Region 通过 popcount 同时累计对象数和 `count * slotSize`；
- large primary 计一个对象并累计 span bytes；
- 周期末把 objectCount/liveBytes 写入 `prevCycleMarkedObjects_` 和日志，驱动下一轮
  assist/pacer。

验证结果（case27 `c1000/p100K/60s`，与 2026-07-11 MSGC 基线相比）：

| 指标 | 基线 | bitmap 统计 | 变化 |
| --- | ---: | ---: | ---: |
| 吞吐 | 388 req/s | 751 req/s | +93.6% |
| ConcurrentMark 平均 | 343.01ms | 52.19ms | -84.8% |
| ConcurrentMark 总时间 | 48.36s | 30.01s | -38.0% |
| GC 平均 | 367.65ms | 73.97ms | -79.9% |
| GC 总时间 | 51.84s | 42.53s | -18.0% |
| 单请求 GC 时间 | 2.162ms | 0.919ms | -57.5% |
| GC 次数 | 141 | 575 | +307.8% |

吞吐提升不能全部归因于删除一个原子：该版本同时运行在多 worker、tail drain 等近期
改造之上。但 bitmap 统计确实移除了每对象共享写，并在 76 个相邻周期验证了 bitmap
objectCount 与下一轮 `prevMarked` 全部一致。GC 次数增加则暴露出 HEU threshold/retained
baseline 仍需重算，不能通过恢复逐对象计数来“修复”。

### 6.6 本地化 Mark/Assist 统计

Step43 将只用于日志和周期反馈的原子统计下沉到 worker/AllocBuffer owner-local：

- helper 处理对象数普通累加，连续四个 WorkBuf 后批量发布 credit，停止/quiesce 时 flush 尾数；
- 每个 `AllocBufferMS` 保存 calls、background stolen、global paid、processed 等 assist
  统计，mutator 退出或 MarkTermination 再低频合并；
- `workersInFlight`、`assistsInFlight`、`bgScanCredit_` 和 `assistWorkPerByteScaled_` 仍是
  实时同步状态，不能本地化。

该优化减少 GC worker 和 mutator 在同一 cache line 上的诊断写入，不改变 mark bitmap、
WorkBuf 消费或 termination 条件。Step43 在撤销 private-partial 并发闭包后复测
`c10/p10K/60s` 为 6658 req/s、400392 成功、0 失败，说明统计本地化本身未引入长时闭包问题。

## 7. Sweep、Reclaim 与物理内存优化

MSGC 将“标记完成”和“可再次分配/可归还物理页”拆开：mark bitmap 保留到对应 Region
完成 Sweep，Sweep 之后再按 empty/partial/full 分类，Reclaim/Scavenge 最后才把完全空闲
的 Region 归还给操作系统。这样能平滑 RSS，但需要维护 generation、并发计数和 backlog。

### 7.1 后台 Sweep、lazy sweep 与 Assist Sweep

[`CentralRegionPool::BackgroundSweep()`](../../src/Heap/MAllocator/MSCentralRegionPool.cpp)
由 `gc-helper` 在 deadline/pressure 预算内按 class order 扫描 unswept Region；mutator
在 refill 不足时可走 lazy sweep；分配债务达到比例后，`MSSweepAssist()` 再让 mutator
主动偿还一小段 Sweep debt。

三个入口共享同一安全协议：

- `activeAssistSweepCount_` 记录正在执行的 mutator assist；
- `skipAssistSweep_` 在 PreEnum 前关闭新 assist；
- StoreLoad fence + active counter 等待已进入 `SweepRegion()` 的线程退出；
- `TryPublishSweepDone()` 同时检查 unswept、background、lazy、assist、helper 计数。

这比“所有 sweep 都在 GC stop-the-world 中做”减少停顿，也比“完全禁止 mutator lazy
sweep”更能保证分配进度。代价是 ready Region 供给不足时，mutator 仍可能承担一段
`SweepRegion()`，形成 Step6 发现的吞吐瓶颈。

### 7.2 空 Region、partial Region 与回收状态

Sweep 后按 live count 分类：

- `liveCount == 0`：整块清零，转 SWEPT/empty 供 BUMP 重用或进入 empty cache；
- `0 < liveCount < total`：生成 alloc/free bitmap 和 summary，转 PARTIAL；
- 全满/未扫：留在相应 full/unswept 列表，不允许被错误当作可分配 Region。

`globalSweepGen_` 和 Region 的 sweep generation 防止上一轮 mark bitmap 尚未清理的 Region
混入新一轮 ready 列表。CentralPool 的 partial/full 返回路径按状态重新挂链；empty cache
则交给 AllocBufferMS 的 class 绑定和本地缓存。

该状态机带来的主要优化是“清扫结果可增量消费”：后台刚扫完一个 class，mutator 就可直接
取 partial/empty，而不必等待全堆 sweep 结束。它同时给 Scavenge 提供精确的“完全空闲”视图。

### 7.3 FreeManager Scavenge 与 segment 释放

FreeManager 在逻辑空闲与物理归还之间加了一层 owned segment 视图。后台 Reclaim 根据
目标保留量调用 `ScavengeFreeRegionsUpTo()`：从高地址向低地址扫描已释放 Region，合并相邻
hot-free run，按 chunk summary 找连续页，再做批量页归还。只有 segment 的 allocBits 全空、
没有借用者和活动 Region 时，才执行整段释放。

这种“先标记 scavenged、后按预算归还”的设计解决了两个冲突：

1. 内存压力大时能快速降低 RSS，不需要等待下一轮 GC；
2. 内存压力小时保留少量 hot free run，避免频繁 mmap/munmap 和地址空间抖动。

Step8 的 aggressive spare/fresh 实验说明，单纯增加可用 Region 或提前归还并不一定提高
吞吐；FreeManager 的预算应与 ready cache、heap growth 和 workload 的复用距离一起调，
不能把“RSS 更低”单独当成优化成功。

### 7.4 大对象回收与 hot cache

大对象由 primary Region + follower span 表示。Sweep 后如果整个 span 死亡，CentralPool
优先放入大对象 hot cache；再次分配同等大小时可直接取出并清零/重新注册，减少连续 Region
获取和系统页操作。若 hot cache 超过目标或 span 不适合复用，则批量交给 FreeManager。

large object 在 mark 期采用分配即标记（primary bitmap bit）避免“mark 进行中刚分配的
span 没有根但却被本轮 sweep 回收”；普通对象的 `MarkNewObject()` 则按当前 collector
phase 标记对象并把含引用对象放入 deferred 主队列。文档不能把两者概括为所有对象都在
LLVM inline path 中 alloc-black：静态新对象 fast path、runtime regular allocation、
large allocation 和 post-init 引用扫描的语义不同，当前代码是最终依据。

### 7.5 Sweep backlog 与 STW 尾部

当后台 Sweep 速度低于分配速度时，下一轮 `PreEnumRoots()` 必须 force-finish unswept
backlog，形成串行尾部。Step44 提出的“HEU 因 `DEFER_FOR_SWEEP` 延后时，临时借用空闲
GC helper 做 bounded parallel sweep”目前仍是设计方案，不是默认实现：当前源码虽已有
`ParallelSweep()`、helper sweep counter 和 stop protocol，但 GCThreadPool 的 Mark/Sweep
跨用途 owner lease、抢占和任务隔离还没有完整落地。

在 Step44 真正实现前，推荐继续使用当前的后台 sweep + lazy/assist + PreEnum force-finish
组合，并增加以下观测：每个 class 的 unswept region 数、mutator lazy sweep 次数/耗时、
PreEnum force-finish bytes、后台/assist/helper 实际 swept bytes。只有确认 backlog 是
主要 wall-time 尾部，才值得引入额外 helper CPU。

## 8. 优化效果与代价

### 8.1 MSGC 相对 CopyGC 的整体基线

下面数据来自 `MSGC_RESULT/Implementation/MSGC_实现效果.md`：macOS aarch64、seed3 超激进
RSS 配置、MSGC 与仓颉默认 CopyGC 对比。它反映架构收益，不是单个 LLVM patch 的隔离
A/B；运行时间也受 GC 阈值和 workload 影响。

| workload/指标 | CopyGC | MSGC | 变化 |
| --- | ---: | ---: | ---: |
| case1 RSS 均值 | 64.6MB | 29.3MB | -54.5% |
| case3 RSS 均值 | 309.1MB | 83.6MB | -73.0% |
| case5 RSS 均值 | 159.0MB | 64.3MB | -59.6% |
| case1 RSS 峰值 | 154.3MB | 54.5MB | -64.7% |
| case3 RSS 峰值 | 879.0MB | 508.0MB | -42.2% |
| case5 RSS 峰值 | 316.5MB | 76.5MB | -75.8% |
| case1 wall time | 5404ms | 6655ms | +23.1% |
| case3 wall time | 122501ms | 118804ms | -3.0% |
| case5 wall time | 11889ms | 13972ms | +17.5% |
| case1 CPU 均值 | 86.0% | 101.3% | +17.8% |
| case3 CPU 均值 | 111.7% | 111.7% | +0.1% |
| case5 CPU 均值 | 103.1% | 141.1% | +36.9% |

GC 侧的共同趋势是“次数增加、单次更短”：

| workload | GC 次数变化 | GC 总时间变化 | 单周期均值变化 | 单周期 P95 变化 |
| --- | ---: | ---: | ---: | ---: |
| case1 | 36 -> 145（+303%） | 205 -> 389ms（+90%） | 5.70 -> 2.68ms（-53%） | 33.25 -> 23.04ms（-31%） |
| case3 | 203 -> 1590（+683%） | 602 -> 1061ms（+76%） | 2.96 -> 0.67ms（-77%） | 5.22 -> 1.14ms（-78%） |
| case5 | 31 -> 344（+1010%） | 635 -> 6714ms（+958%） | 20.48 -> 19.52ms（-5%） | 24.12 -> 21.57ms（-11%） |

**如何理解**：MSGC 原地 mark/sweep 不需要 CopyGC 的 from/to 双空间，因此 RSS 和 STW
停顿明显降低；并发 mark、后台 sweep、bitmap/屏障和更低的触发阈值会增加 GC 次数和累计
CPU。case3 的长运行、大存活集和大量临时对象最适合 MSGC；case5 的大对象 churn 说明
large span、频繁 sweep 和过密触发仍是成本热点。

### 8.2 优化贡献的层次化判断

- **架构级收益**：原地回收、增量 sweep/scavenge、低空闲池下限，是 RSS 大幅下降和
  单周期停顿缩短的主要原因。
- **固定税收益**：LLVM read/write lowering、TLS phase/AllocBuffer 直取、active log 本地
  缓存、bitmap 统计本地化，减少每次 field/mark/统计操作的调用和共享原子。
- **miss 路径收益**：BUMP fast path、partial bitmap、cached Region、hot large cache，
  只有命中时才真正缩短分配；命中率下降时仍会回到 SlowAlloc/lazy sweep。
- **并发度收益**：Global WorkBuf、多 helper 和 Assist pacing 把 mark 工作从 mutator 移出，
  但线程数、pacer 和服务端等待决定 wall time 是否同步改善。

### 8.3 代价与风险

1. 低阈值/激进 reclaim 会导致 GC 过密、固定 STW/Enum 成本增加；
2. 后台预取过度会抬高 committed/RSS，前台补货过少又会把 sweep 返还给 mutator；
3. inline barrier/new fast path 依赖硬编码 ABI，phase、heap range、bitmap 偏移任一不一致
   都可能造成 silent corruption；
4. WorkBuf tail、mutator 退出和 verifier 是并发优化的主要正确性风险；
5. 大对象 hot cache 与 Scavenge 是 RSS/吞吐的权衡，不应只看其中一个指标。

## 9. 未采用、回退及当前实现与旧文档的差异

### 9.1 已回退或收益不足的实验

| 方案 | 现状 | 原因 |
| --- | --- | --- |
| PARTIAL slot-level cursor | 已回退 | 碎片场景 Phase2 慢 30%--100%，算术固定税抵消 bitmap 节省 |
| 无条件 proactive refill/backup prefetch | 已回退 | 可能一次 miss 触发两次 refill，RSS、GC 时间或 wall time 变差 |
| aggressive fresh prefill/partial run-zero | 已回退 | ready 供给增加但提交内存和清零成本更高 |
| fresh/仅栈目标 `gcread_generic -> memcpy` | 已移除 | 目标 fresh/在栈不能证明源引用可跳过 GC read/generic 语义 |
| fresh-init barrier 消除/单引用 struct 标量化 | 已移除 | case27 A/B 收益基本一致；恢复 barrier 原形态并减少 CopyGC/MSGC 语义隔离面 |
| 全部 `MaybeNotifyMark` 下沉后台 | 未采用 | RSS/GC 进度失衡；只保留有限 reclaim-only 实验结论 |
| quiesce 时发布 GC main/helper private partial 并恢复闭包 | 已撤销 | 长测 ConcurrentMark 拉长并最终 OOM |
| 标记每个对象的共享 `markedObjectCount` | 已移除 | cache-line bouncing；改用 bitmap 周期统计 |
| Step44 动态 Sweep helper lease | 设计中 | 跨用途 GCThreadPool owner/抢占/任务隔离尚未完成 |

### 9.2 当前源码与旧文档的关键差异

- 当前 barrier fast gate 以 `Heap::inlineFastPathGCPhase` 为主，旧文档中只写 TLS
  `gcFastState` 的描述已经不完整；TLS `encodedGCPhase` 仍用于其它同步路径。
- `MarkNewObject()` 当前把含引用的新对象放入 deferred 主队列；不能用旧的“所有新对象
  都 alloc-black 且立即入本地 work”概括 regular allocation。
- arraycopy wrapper 的实现代码、lowering 名单和 `AsmPrinter` dispatch 必须同时生效；
  当前工作区不把旧 Step30 的“arraycopy 已完全 inline”当作无条件事实。
- `CJGenericIntrinsicOpt` 已删除 `processFreshGcreadGeneric()` 及其 fresh/栈目标证明辅助函数；
  `gcread_generic` 恢复统一进入原有 `processGenericBarrier()`。
- `CJBarrierOpt` 已删除 `preprocessBarriers()`、`optFreshInitBarrier()` 和
  `scalarizeOneRefStructBarrier()` 等新增逻辑，fresh 首次写与单引用 struct 均保留原 barrier。
- `Step13` freelist、`Step28/29` generic 进一步消解、`Step44` Sweep helper 属于不同
  完成度：前者已实验回退，中间部分有局部基础设施，后者仍是设计。
- `MSGC_实现效果.md` 的 seed3 全局数据与 Step41/42 的 case27 新 runtime 数据来自不同
  SDK/参数，不能直接合并成一张“单 patch 收益表”。

## 10. 下一阶段优化建议

下面按“预期收益 × 实施风险 × 验证成本”排序，建议每项都使用干净 SDK 子 shell、确认
compiler/runtime/stdlib 三件套来源，并在替换 runtime 后重新编译用例。

### P0：优先消除 generic write 的纯 memcpy helper

Step29 已证明 case28 中 `base_write_generic_memcpy` 约 5.69 亿次，而真正进入
`WriteStruct` 仅约 7.5 万次。建议在 `CJGenericIntrinsicOpt` 中按 fresh provenance、
stack alloca、无 ref TypeInfo 和 source/destination heap range 做分层证明：

1. 第一阶段只处理 stack-dst、无引用或明确 non-heap source 的 generic write；
2. 第二阶段处理带 `new_gen` 的 fresh heap dst，必须证明目标 payload 没有旧 ref；
3. 无法证明时保留 `WriteStruct`，不能用 runtime 当前“多数时候 memcpy”反推编译期安全。

验证指标：helper 次数、`base_write_generic_memcpy` 字节数、吞吐、CPU、verifier 和
`WriteStruct` old-edge 计数；不要只看 IR 中 call 数量。

### P1：减少 AllocBufferMS 掉入 SlowAlloc 的次数

当前 case1 的结构性瓶颈仍是 `SlowAlloc -> RefillFromCentral -> GetRegion -> lazy sweep`。
建议：

- 在 active Region 接近低水位时，向后台发 class-specific refill hint，而不是等 miss 后才
  置 `cacheMiss_`；
- 让 `BackgroundFillCache()` 优先生产可立即绑定 class 的 ready Region，fresh fallback
  保持预算；
- 为 mutator lazy sweep 增加每次/每周期预算，优先保证分配进度和可观测性；
- 增加 per-class `slowAlloc`、`partialUnswept/fullUnswept`、lazy sweep bytes/time 计数。

目标不是简单提高 cache 数量，而是提高 hot class 的 `cached_hit`、降低 `slow_fresh`，同时
不让 committed/RSS 反弹。Step8 的 hard pending gate 和“去掉同步 backup prefetch”是当前
可复用的安全基线。

### P1：修正 pacer/threshold，吸收吞吐提升带来的 GC 次数膨胀

Step41 显示吞吐接近翻倍后 GC 次数仍从 141 增至 575，说明 retained baseline、allocation
headroom 和 HEU threshold 口径不一致。建议把上一轮真实 live bytes、当前 occupied Region
和分配 runway 统一在 GC 收尾计算，分配热路径只读取预计算 threshold；同时限制短时间内
重复 HEU 请求，避免单周期固定 STW 成本被高吞吐放大。

### P2：继续缩短 Sweep backlog 的 STW 尾部

先补齐 backlog 观测并确认 `PreEnumRoots()` force-finish 是 wall-time 主项，再评估 Step44：
为 GCThreadPool 增加明确的 IDLE/SWEEP/MARK lease，借用空闲 helper 执行 bounded sweep，
遇到 OOM/FORCE/下一轮 GC 可通过 stop request 抢占。若 helper lease 引入任务混用风险，
宁可保持当前后台/assist 路径，不要把 Mark worker 和 Sweep worker 随意复用。

### P2：完善 barrier inline 的代码尺寸与 dispatch 管理

- 为 ref/struct/arraycopy 分别记录 lowering 命中率和最终 `AsmPrinter` 发射率；
- 继续采用短函数/局部 label，避免 jump-table/fixup range（Step38/bugfix 已出现该问题）；
- 对 bitmap-word batch、pair-ref scalarize 做独立 A/B，若收益在噪声内则优先降低代码尺寸；
- 为 `Heap::inlineFastPathGCPhase`、heap range 和 `activeLog_` 偏移增加编译期/运行时契约
  检查，防止 runtime/LLVM 版本错配。

### P3：优化大对象与 Scavenge 的协同

针对 case5 的高 CPU/GC 总时间，建议记录大对象 span 命中率、hot-cache 保留时间、清零字节
数和 Scavenge 系统调用；按复用距离决定保留还是归还，不要以固定 Region 数硬编码。大对象
mark-at-alloc、follower span 注册和 finalizer 路径应继续用 verifier/长时压力测试覆盖。

### P3：降低高并发运行时统计和调度固定税

继续保留 Step43 的 owner-local stats，把只读日志字段从热路径移到周期末；对 `TypeInfo`
缓存、`MRT_UpdateThreadLocalDataPhase`、generic memcpy 和 `GetMutator/TLS` 做 profile，
避免在 GC 核心已变快后，旧的类型/拷贝辅助成本成为新的主热点。

### P4：建立统一的性能与正确性门禁

每个优化至少固定以下报告：

```text
compiler/runtime/stdlib 来源、dylib 实际加载路径
wall time、CPU、RSS mean/peak/P95、吞吐
GC count/total/mean/P95、mark/sweep/assist share
slowAlloc/cached hit/miss、barrier/workbuf/bitmap 计数
verifier、OOM、SIGV、长测成功率
```

这样才能区分“减少了 GC 工作”“把工作转移给 mutator”“只减少了统计噪声”三种完全不同
的效果，避免再次把局部实验或设计稿误判为整体优化。

## 11. 源码与验证索引

### 11.1 Runtime 关键实现

| 主题 | 源码 |
| --- | --- |
| Region bitmap、bulk clear、ResetToBump | [`MSRegionInfo.cpp`](../../src/Heap/MAllocator/MSRegionInfo.cpp) |
| TLS AllocBufferMS、BUMP/PARTIAL fast/slow alloc | [`MSAllocBuffer.cpp`](../../src/Heap/MAllocator/MSAllocBuffer.cpp) |
| Region 获取、后台/Assist Sweep、generation | [`MSCentralRegionPool.cpp`](../../src/Heap/MAllocator/MSCentralRegionPool.cpp) |
| 大对象 hot cache、large span | [`MSCentralRegionPool.cpp`](../../src/Heap/MAllocator/MSCentralRegionPool.cpp) / [`MSRegionSpace.cpp`](../../src/Heap/MAllocator/MSRegionSpace.cpp) |
| FreeManager segment/scavenge | [`MSFreeManager.cpp`](../../src/Heap/MAllocator/MSFreeManager.cpp) |
| GC 主流程、worker、termination、统计 | [`MarkSweepCollector.cpp`](../../src/Heap/MCollector/MarkSweepCollector.cpp) |
| MSGC read/write/struct/array/atomic barrier | [`MSMarkBarrier.cpp`](../../src/Heap/MBarrier/MSMarkBarrier.cpp) |
| phase/barrier 发布 | [`Heap.cpp`](../../src/Heap/Heap.cpp) |
| TLS phase 与 ABI static_assert | [`ThreadLocal.h`](../../src/Mutator/ThreadLocal.h)、[`Mutator.cpp`](../../src/Mutator/Mutator.cpp)、[`ThreadLocal.cpp`](../../src/Mutator/ThreadLocal.cpp) |

### 11.2 LLVM 关键实现

| 主题 | 源码 |
| --- | --- |
| MSGC wrapper、属性、size class 公共 ABI | [`CJMSGCABI.h`](../../../../cangjie_compiler/third_party/llvm-project/llvm/include/llvm/CodeGen/CJMSGCABI.h) |
| intrinsic、read/write lowering、new wrapper 特化 | [`CJBarrierLowering.cpp`](../../../../cangjie_compiler/third_party/llvm-project/llvm/lib/CodeGen/CJBarrierLowering.cpp) |
| MSGC wrapper dispatch | [`AsmPrinter.cpp`](../../../../cangjie_compiler/third_party/llvm-project/llvm/lib/CodeGen/AsmPrinter/AsmPrinter.cpp) |
| AArch64 barrier/new object/new array 发码 | [`AArch64AsmPrinter.cpp`](../../../../cangjie_compiler/third_party/llvm-project/llvm/lib/Target/AArch64/AArch64AsmPrinter.cpp) |
| 指令展开大小保守估算 | [`AArch64InstrInfo.cpp`](../../../../cangjie_compiler/third_party/llvm-project/llvm/lib/Target/AArch64/AArch64InstrInfo.cpp) |
| MSGC lowering/fastpath 测试 | [`llvm/test/CodeGen/AArch64/CangjieGC`](../../../../cangjie_compiler/third_party/llvm-project/llvm/test/CodeGen/AArch64/CangjieGC) |

### 11.3 实现与效果记录

| 内容 | 记录 |
| --- | --- |
| 架构和阶段基础 | [`01_MSGC_总体架构与实现详解.md`](01_MSGC_总体架构与实现详解.md) |
| 整体 RSS/GC/CPU 基线 | [`MSGC_实现效果.md`](../Implementation/MSGC_实现效果.md) |
| mutator refill、bulk clear、prefetch 实验 | [`Step6`](../Implementation/Step6_Mutator侧吞吐瓶颈分析与优化方向.md)、[`Step8`](../Implementation/Step8_AllocBufferMS补货链优化复盘与踩坑记录.md) |
| LLVM 优化盘点 | [`Step30`](../Implementation/Step30_LLVM_MSGC优化实现盘点.md) |
| barrier 语义与 inline 对比 | [`Step35`](../Implementation/Step35_CopyGC与MSGC屏障语义及Inline实现对比.md)、[`Step38`](../Implementation/Step38_MSGC写屏障fastpath与slowpath优化点分析.md) |
| Global WorkBuf/worker/tail drain | [`Step40`](../Implementation/Step40_MSGC_GlobalFull统一满块流转与单块Assist实验.md)、[`Step42`](../Implementation/Step42_MSGC_Global任意WorkBuf并发消费与收敛.md) |
| bitmap 统计/local stats | [`Step41`](../Implementation/Step41_MSGC_bitmap统计替代markedObjectCount热原子.md)、[`Step43`](../Implementation/Step43_MSGC_Mark统计本地化与周期汇总.md) |
| generic read/write 后续方向 | [`Step28`](../Implementation/Step28_MSGC_gcread_generic_fastpath设计.md)、[`Step29`](../Implementation/Step29_MSGC_writegeneric_memcpy热点与LLVM消解方案.md) |
| Sweep helper 设计（未完全落地） | [`Step44`](../Implementation/Step44_MSGC_NotifyMark动态SweepHelper设计.md) |

### 11.4 验证注意事项

本文没有重新编译 runtime 或替换 SDK dylib；结论来自当前源码审阅和已有结果文件。若要
复现实验，必须遵守项目约定：每个 SDK 使用隔离的 `env -i` 子 shell，先用
`DYLD_PRINT_LIBRARIES=1` 验证实际加载的 runtime；替换 runtime 后重新用同一 compiler/
runtime/stdlib 编译 `.cj` 用例，不能复用旧 binary。对于 macOS aarch64，复制 dylib 后还
需要重新 codesign。

最后，本文刻意没有把旧的 CopyGC 风格 MSGC wrapper miss、arraycopy wrapper 分支、Step13
freelist、Step44 helper lease 和所有历史 TLS 字段描述混为“当前已启用优化”。后续代码变化
时应优先更新本节索引和第 9 节差异，再调整效果结论。
